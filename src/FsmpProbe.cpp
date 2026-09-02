#include "FsmpProbe.h"

#include "RaceMenuMorphApi.h"  // LogKeyCensus: the zeroed-sliders half of the dead body
#include "RealWorn.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>

namespace OS::FsmpProbe {

    namespace {

        // Bumped on every editor edge. A sample task or a sleeping watcher
        // that wakes to find the epoch moved belongs to a dead run and says
        // nothing; the guard is the whole teardown story, so a re-open mid-run
        // simply strands the old thread's remaining posts.
        std::atomic<std::uint32_t> g_epoch{ 0 };

        constexpr int  kSamples        = 6;
        constexpr auto kSampleInterval = std::chrono::milliseconds(1000);

        // Previous sample's clock readings, so each line prints DELTAS - the
        // question is "is this clock advancing", and two absolute readings a
        // second apart answer it only after arithmetic nobody does at 1 am.
        // Reset at run start; main-thread only (every writer is a queued task).
        struct ClockBaseline {
            bool   valid{ false };
            float  anim{ 0.0f };
            double calendarHours{ 0.0 };
        };
        ClockBaseline g_clocks;

        // Same idea for the sampled bones: name -> last world AND local
        // translate. The local half is the r24 lesson: a world-space delta on
        // a walking player is locomotion, not physics, and the two are
        // indistinguishable in one number. The parent-relative translate only
        // moves when something (FSMP, CBPC, an animation) drives THIS bone.
        // A fixed array because the sample set is at most three nodes.
        struct BoneBaseline {
            std::string  name;
            RE::NiPoint3 pos;
            RE::NiPoint3 local;
        };
        // 2 hair witnesses plus up to 8 Breast-family bones. ⚠⚠ THE OLD SINGLE
        // BREAST PICK WAS THE WRONG BONE: the walk took the FIRST name
        // containing "Breast", which on this rig is a control bone that sits
        // still by construction, so dl read 0.0000 in every round INCLUDING the
        // ones where the field said the body was fine. FSMP hands the moving
        // copy an armour-namespaced rename (hdtSSEPhysics_AutoRename_Armor_*)
        // and CBPC drives "NPC L/R Breast" directly; only a sweep across every
        // Breast-family bone can hold the dead and the alive rounds apart.
        constexpr int kMaxSampledBones = 10;
        BoneBaseline  g_bones[kMaxSampledBones];
        int           g_boneCount{ 0 };

        // FSMP's head-adoption tell is the rename: only FSMP manufactures
        // hdtSSEPhysics_AutoRename_Head_* bones. The face-constraint bones it
        // also creates (lips, cheeks, throat) sit still on a posed character
        // by nature, so they are useless as motion witnesses - the wig bones
        // are the ones wind should move.
        bool IsFsmpHeadBone(std::string_view a_name) {
            return a_name.find("hdtSSEPhysics_AutoRename_Head") != std::string_view::npos;
        }

        bool IsFaceConstraintBone(std::string_view a_name) {
            return a_name.find("Nose") != std::string_view::npos ||
                   a_name.find("Lip") != std::string_view::npos ||
                   a_name.find("Cheeck") != std::string_view::npos ||  // FSMP's own spelling
                   a_name.find("Throat") != std::string_view::npos;
        }

        struct BoneSample {
            std::string  name;
            RE::NiPoint3 pos;
            RE::NiPoint3 local;
        };

        struct SkeletonRead {
            int        fsmpHeadBones{ 0 };  // every AutoRename_Head node seen
            int        breastBones{ 0 };    // every Breast-family node seen
            BoneSample picked[kMaxSampledBones];
            int        pickedHead{ 0 };    // slots 0..1
            int        pickedBreast{ 0 };  // slots 2..9
        };

        // The bones CBPC and FSMP move by hand, not the driven-by-weights
        // scale/anchor family around them: CBPC writes "NPC L Breast", FSMP's
        // per-armour merge moves its "...AutoRename_Armor_XXXXXXXX L Breast"
        // twins, and the CME/Pre/Scale relatives hold still through both.
        bool IsPassiveBreastRelative(std::string_view a_name) {
            return a_name.find("CME") != std::string_view::npos ||
                   a_name.find("Pre") != std::string_view::npos ||
                   a_name.find("Scale") != std::string_view::npos;
        }

        void Walk(RE::NiAVObject* a_obj, SkeletonRead& a_out, int a_depth) {
            if (!a_obj || a_depth > 64) {
                return;
            }
            const std::string_view name{ a_obj->name.c_str() ? a_obj->name.c_str() : "" };
            if (IsFsmpHeadBone(name)) {
                ++a_out.fsmpHeadBones;
                if (!IsFaceConstraintBone(name) && a_out.pickedHead < 2) {
                    a_out.picked[a_out.pickedHead++] = { std::string{ name },
                                                         a_obj->world.translate,
                                                         a_obj->local.translate };
                }
            } else if (name.find("Breast") != std::string_view::npos ||
                       name.find("Butt") != std::string_view::npos) {
                // Butt joined in r51: the field says a RaceMenu trip revives
                // the BUTT and not the breasts, so the butt is the working
                // control the breast readings compare against.
                ++a_out.breastBones;
                if (!IsPassiveBreastRelative(name) &&
                    a_out.pickedBreast < kMaxSampledBones - 2) {
                    a_out.picked[2 + a_out.pickedBreast++] = { std::string{ name },
                                                               a_obj->world.translate,
                                                               a_obj->local.translate };
                }
            }
            if (auto* node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    Walk(child.get(), a_out, a_depth + 1);
                }
            }
        }

        std::string DescribeSlot(const RealWorn& a_worn, int a_slot) {
            auto* armo = a_worn.armo[a_slot - 30];
            if (!armo) {
                return "-";
            }
            const char* name = armo->GetName();
            return fmt::format("'{}' {:08X}", (name && name[0]) ? name : "(unnamed)",
                               armo->GetFormID());
        }

        void Sample(std::uint32_t a_epoch, int a_k, const char* a_tag,
                    std::chrono::steady_clock::time_point a_runStart) {
            if (g_epoch.load(std::memory_order_acquire) != a_epoch) {
                return;  // a newer edge owns the log now
            }
            const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - a_runStart)
                                     .count();

            // --- the clocks (story 1: is the world advancing) ---
            const float  anim  = RE::Main::QFrameAnimTime();
            double       hours = 0.0;
            if (auto* cal = RE::Calendar::GetSingleton()) {
                hours = cal->GetHoursPassed();
            }
            std::string clocks;
            if (g_clocks.valid) {
                clocks = fmt::format("animD={:+.4f} calD={:+.6f}h", anim - g_clocks.anim,
                                     hours - g_clocks.calendarHours);
            } else {
                clocks = fmt::format("anim={:.4f} cal={:.6f}h (baseline)", anim, hours);
            }
            g_clocks = { true, anim, hours };

            // --- the pause posture ---
            bool          paused  = false;
            std::uint32_t npauses = 0;
            if (auto* ui = RE::UI::GetSingleton()) {
                paused  = ui->GameIsPaused();
                npauses = ui->numPausesGame;
            }
            bool freeze = false;
            if (auto* main = RE::Main::GetSingleton()) {
                freeze = main->freezeTime;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                spdlog::info("FsmpProbe[{} {}/{}]: t+{}ms {} paused={}(n={}) freeze={} - no player",
                             a_tag, a_k + 1, kSamples, sinceMs, clocks, paused ? 1 : 0, npauses,
                             freeze ? 1 : 0);
                return;
            }

            // --- the wig trigger, read live (story 3) ---
            RealWorn worn;
            if (auto* changes = player->GetInventoryChanges()) {
                worn = SnapshotRealWorn(changes);
            }

            // --- once per run: the body-side context the dead physics needs
            // (story 5). The field's "I see the zeroed sliders" is a skee
            // BODYMORPH fact, and 3BA's whole CBPC-versus-SMP mode machinery
            // rides those morphs plus an invisible worn SMP object; a census
            // of the morph keys and the full worn-slot map, taken on the
            // switched character and on the working one, is the comparison.
            if (a_k == 0) {
                OS::RaceMenuMorphApi::LogKeyCensus(player, a_tag);
                std::string slots;
                for (int bit = 0; bit < 32; ++bit) {
                    if (auto* const armo = worn.armo[bit]) {
                        const char* an = armo->GetName();
                        slots += fmt::format(" {}='{}' {:08X}", 30 + bit,
                                             (an && an[0]) ? an : "(unnamed)",
                                             armo->GetFormID());
                    }
                }
                spdlog::info("FsmpProbe[{}]: worn slots:{}", a_tag,
                             slots.empty() ? " (none)" : slots.c_str());
            }

            // --- what CBPC's conditions would read (story 4: the dead body)
            // The winning CBPCMasterConfig maps every breast/butt bone through
            // IsFemale() and disables the female bone set outright for males
            // (MalePhysics=0). The apply flips the base's sex flag mid-session;
            // whether the LIVE flags actually read female at each sample is
            // the fact that decides if CBPC is refusing the config or holding
            // a stale evaluation.
            const char*   sexNow = "?";
            std::uint32_t raceId = 0;
            std::uint32_t cgrId  = 0;
            if (auto* const base = player->GetActorBase()) {
                sexNow = base->IsFemale() ? "F" : "M";
                if (base->race) {
                    raceId = base->race->GetFormID();
                }
            }
            if (auto* const cgr = player->GetRaceData().charGenRace) {
                cgrId = cgr->GetFormID();
            }

            // --- the bones (stories 2 and 3) ---
            SkeletonRead read;
            Walk(player->Get3D(), read, 0);

            std::string bones;
            const auto  emit = [&](const BoneSample& b) {
                // Match against the previous sample by name; a browse swap that
                // replaced the wig mid-run just re-baselines that slot. d is
                // the world delta (locomotion included); dl is the
                // parent-relative delta, which only physics or animation can
                // move - dl=0.0000 across samples on a moving player is a
                // frozen bone, whatever d says.
                float dWorld = -1.0f;
                float dLocal = -1.0f;
                for (int i = 0; i < g_boneCount; ++i) {
                    if (g_bones[i].name == b.name) {
                        const auto& p = g_bones[i].pos;
                        dWorld = std::sqrt((b.pos.x - p.x) * (b.pos.x - p.x) +
                                           (b.pos.y - p.y) * (b.pos.y - p.y) +
                                           (b.pos.z - p.z) * (b.pos.z - p.z));
                        const auto& l = g_bones[i].local;
                        dLocal = std::sqrt((b.local.x - l.x) * (b.local.x - l.x) +
                                           (b.local.y - l.y) * (b.local.y - l.y) +
                                           (b.local.z - l.z) * (b.local.z - l.z));
                        break;
                    }
                }
                bones += fmt::format(" | '{}' ({:.2f},{:.2f},{:.2f}) d={} dl={}", b.name,
                                     b.pos.x, b.pos.y, b.pos.z,
                                     dWorld < 0.0f ? std::string{ "first" }
                                                   : fmt::format("{:.4f}", dWorld),
                                     dLocal < 0.0f ? std::string{ "first" }
                                                   : fmt::format("{:.4f}", dLocal));
            };
            for (int i = 0; i < read.pickedHead; ++i) {
                emit(read.picked[i]);
            }
            for (int i = 0; i < read.pickedBreast; ++i) {
                emit(read.picked[2 + i]);
            }
            // New baseline AFTER emitting deltas against the old one.
            int store = 0;
            for (int i = 0; i < read.pickedHead && store < kMaxSampledBones; ++i) {
                g_bones[store++] = { read.picked[i].name, read.picked[i].pos,
                                     read.picked[i].local };
            }
            for (int i = 0; i < read.pickedBreast && store < kMaxSampledBones; ++i) {
                g_bones[store++] = { read.picked[2 + i].name, read.picked[2 + i].pos,
                                     read.picked[2 + i].local };
            }
            g_boneCount = store;

            spdlog::info(
                "FsmpProbe[{} {}/{}]: t+{}ms {} paused={}(n={}) freeze={} sex={} race={:08X} "
                "cgr={:08X} slot31={} 41={} 42={} hdtHeadBones={} breastBones={}{}",
                a_tag, a_k + 1, kSamples, sinceMs, clocks, paused ? 1 : 0, npauses, freeze ? 1 : 0,
                sexNow, raceId, cgrId,
                DescribeSlot(worn, 31), DescribeSlot(worn, 41), DescribeSlot(worn, 42),
                read.fsmpHeadBones, read.breastBones,
                bones.empty() ? " | no sampled bones" : bones.c_str());
        }

    }  // namespace

    void OnEditorToggle(bool a_open) {
        const std::uint32_t epoch = g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Fresh run, fresh baselines. Main thread, same as every Sample task.
        g_clocks    = {};
        g_boneCount = 0;
        const char* tag      = a_open ? "editor" : "world";
        const auto  runStart = std::chrono::steady_clock::now();
        std::thread([epoch, tag, runStart] {
            for (int k = 0; k < kSamples; ++k) {
                if (g_epoch.load(std::memory_order_acquire) != epoch) {
                    return;
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([epoch, k, tag, runStart] { Sample(epoch, k, tag, runStart); });
                }
                std::this_thread::sleep_for(kSampleInterval);
            }
        }).detach();
    }

}  // namespace OS::FsmpProbe
