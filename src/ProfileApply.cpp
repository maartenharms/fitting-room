#include "PCH.h"

#include "ProfileApply.h"

#include <algorithm>  // ranges::any_of, the slots a look does not name
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "BipedPost.h"  // RestoreHeadPartPartitions from the settle tasks the probe proves reachable
#include "CharacterSex.h"  // the sex a look states; the engine will not keep it
#include "BodyPresetStore.h"
#include "BodyWeight.h"
#include "DefaultBody.h"  // the look's body becomes the character's fallback
#include "LookFaceTint.h"  // the export a preset face bakes its detail into
#include "DefaultLook.h"  // and the look's hair colour becomes its default too
#include "HairColor.h"
#include "HeadPart.h"
#include "MakeupApi.h"
#include "RaceTint.h"   // Slots: the race's own tint count, the makeup step's gate
#include "NodeTransformApi.h"
#include "ObodyApi.h"
#include "EditorWindow.h"  // the apply closes the editor, or re-stages it
#include "SculptProbe.h"  // PulseHead: the r73 pre-face bracket
#include "Settings.h"
#include "OutfitSession.h"
#include "CbpcRefresh.h"
#include "VmCall.h"  // a static call that refuses instead of dereferencing null
#include "Overlay1P.h"
#include "OverlayApi.h"
#include "OverlayBaseline.h"  // the record a replacing look leaves behind with the pins
#include "MakeupBaseline.h"   // and the tint-list record is dropped for the same reason
#include "OverlayPlan.h"
#include "OverlayReconcile.h"
#include "RaceMenuMorphApi.h"
#include "ShapeOverlay.h"
#include "SkinApi.h"
#include "StyleRef.h"

namespace OS::ProfileApply {

    namespace {

        // Milliseconds on the steady clock until which an apply is in
        // flight; zero when none is. A deadline rather than a bare flag so a
        // path that never reaches the clear (a quit mid-apply, a lost
        // callback) cannot suppress editor-close re-asserts forever.
        std::atomic<long long> g_inFlightUntilMs{ 0 };

        [[nodiscard]] long long SteadyNowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        void SetInFlight(bool a_on) {
            g_inFlightUntilMs.store(a_on ? SteadyNowMs() + 30'000 : 0);
        }

        // Whether a face step answered true this session; the load boundary's
        // mapped-preset erase (header scar) keys on it. Never cleared by the
        // erase failing - the residue is still there to erase on the next
        // boundary.
        std::atomic<bool> g_faceAppliedThisSession{ false };

        // Whether the apply in flight switched the player's race or sex, read
        // once by RunSteps so the sibling steps write into a tint list this
        // race owns. See MakeupApi::RebuildListForRace.
        std::atomic<bool> g_switchedThisApply{ false };

        [[nodiscard]] bool RequiresSatisfied(const ProfileCodec::Profile& a_profile) {
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler) return false;
            for (const auto& plugin : a_profile.requires_) {
                if (!handler->LookupModByName(plugin)) {
                    spdlog::warn(
                        "ProfileApply: '{}' refused, required plugin '{}' is "
                        "not in the load order.",
                        a_profile.name, plugin);
                    return false;
                }
            }
            return true;
        }

        // ---- the character decision, made once and threaded ---------------

        // WHO the rest of the apply targets. Resolved a single time in
        // Apply() and handed to both consumers (the character step and the
        // face dispatch), because two readers deriving one answer drift in
        // the gap between them.
        struct SwitchDecision {
            RE::TESRace* targetRace{ nullptr };  // null: keep the current race
            bool         flipFemale{ false };    // the sex flag must change
            bool         female{ false };        // what it changes to
            bool         differs{ false };       // any of race or sex differ
        };

        // The sex flag flips FIRST, before any rebuild reads it: skee's
        // ApplyPreset picks chargenData[GetSex()] and gender-flagged head
        // parts off the live base, and the engine's own race switch loads the
        // skeleton and body model for whatever the flag says at that moment.
        // A jslot carries no sex, so this flag is the entire sex half.
        //
        // The race half has two carriers. With a face in the plan, the
        // profile's race rides the face step's LoadCharacterEx and skee's own
        // SetRace does the switch (PresetInterface.cpp:1531, read
        // 2026-08-22): one rebuild, RaceMenu's own preset-load path. Without
        // one, SwitchRace here does the same thing directly; a sex flip with
        // no race change still needs it (to the CURRENT race), because the
        // flag alone repaints nothing, which is exactly the male body the
        // field kept at 02:08.
        void StepCharacter(RE::Actor* a_player, const SwitchDecision& a_d,
                           bool a_faceCarriesRace) {
            if (!a_d.differs) {
                spdlog::info("ProfileApply: step 'character' (already this "
                             "race and sex).");
                return;
            }
            // ⚠⚠ A SWITCH TEARS UP EVERY FIRST-TOUCH HEAD CAPTURE. The
            // ladder's bottom rung is "put back their own", and "their own"
            // is whatever Fitting Room captured when it first touched a slot
            // on THIS character - the pre-switch one. Field 2026-08-22 03:05:
            // the face step built Umbrael's head on the switched actor and
            // 76 ms later the outfit push's hair rung, whose outfit named no
            // hair, restored the capture taken on the nord and the hair fell
            // back to vanilla. Same class as the 2026-08-16 hair leak across
            // save reverts, so it takes the same cure: drop the captures,
            // restore nothing, and the first touch AFTER the switch captures
            // the new character as their own.
            HeadPart::Clear();
            HairColor::Clear();
            if (a_d.flipFemale) {
                // ⚠⚠ THE BASE IS SHARED AND THIS WRITE OUTLIVES THE CHARACTER.
                // Form 00000007 is one static record; applying a look whose sex
                // differs and then loading another save handed that save's
                // character the wrong body (field 2026-08-25). Record what the
                // engine had so RevertCallback can put it back.
                OS::CharacterSex::NoteBaselineBeforeWrite(a_player);
                if (auto* const base = a_player->GetActorBase()) {
                    using Flag = RE::ACTOR_BASE_DATA::Flag;
                    if (a_d.female) {
                        base->actorData.actorBaseFlags.set(Flag::kFemale);
                    } else {
                        base->actorData.actorBaseFlags.reset(Flag::kFemale);
                    }
                }
            }
            // ⚠⚠ HELD WHETHER OR NOT THE FLAG NEEDED FLIPPING, and the
            // difference matters. `flipFemale` is only true when the base
            // DISAGREES with the look right now, so a look applied onto a
            // character who already matches writes nothing above and would
            // record nothing here - and then a load, which resets the base to
            // Skyrim.esm's male default, would have no record to correct it
            // with. What is being recorded is what the LOOK says, not what the
            // write happened to change (CharacterSex.h).
            CharacterSex::Hold(a_d.female);
            // ⚠⚠ THE CHARGEN RACE IS THE REVERTER, AND ROUND TEN MEASURED THE
            // WHOLE MECHANISM. TESNPC::HasOverlays (AE 24790) carries a
            // special branch for form id 7, the player base: it answers TRUE
            // whenever PlayerCharacter's charGenRace differs from the base's
            // current race (the vanilla vampire-face machinery), and every
            // head build that sees TRUE takes its part list from the STORED
            // overlay parts instead of npc->headParts. SetRace never writes
            // charGenRace (decompiled, both builds: it only compares it), so
            // after a switch the engine kept rebuilding the OLD character's
            // head from the overlay store: a fully-formed pre-switch head
            // node, attached with zero paint calls, one to three seconds
            // after every apply, invisible to every data instrument until
            // the head-watch read the node's child names. The RaceMenu dial
            // repairs it because the chargen menu COMMITS charGenRace; this
            // is that commit, done where the switch is decided.
            //
            // ⚠ Written for BOTH carriers (skee's SetRace and the SwitchRace
            // below), before either runs, so no build in the transition
            // window reads the stale pair. A vampire character's charGenRace
            // is its human race by design; a race switch under active
            // vampirism was already broken before this line and stays its
            // own problem.
            if (a_player == RE::PlayerCharacter::GetSingleton()) {
                auto* const pc = RE::PlayerCharacter::GetSingleton();
                auto* const target =
                    a_d.targetRace ? a_d.targetRace : a_player->GetRace();
                auto& raceData = pc->GetRaceData();
                if (target && raceData.charGenRace != target) {
                    spdlog::info(
                        "ProfileApply: step 'character' commits charGenRace "
                        "{:08X} -> {:08X} (the overlay-head trigger).",
                        raceData.charGenRace ? raceData.charGenRace->GetFormID()
                                             : 0u,
                        target->GetFormID());
                    raceData.charGenRace = target;
                }
            }
            if (a_faceCarriesRace) {
                spdlog::info(
                    "ProfileApply: step 'character' ({}the race switch rides "
                    "the face step's LoadCharacterEx).",
                    a_d.flipFemale
                        ? (a_d.female ? "sex flag set to female; "
                                      : "sex flag set to male; ")
                        : "");
                return;
            }
            auto* const race =
                a_d.targetRace ? a_d.targetRace : a_player->GetRace();
            a_player->SwitchRace(race,
                                 a_player == RE::PlayerCharacter::GetSingleton());
            spdlog::info(
                "ProfileApply: step 'character' ({}SwitchRace to '{}').",
                a_d.flipFemale ? (a_d.female ? "sex flag set to female; "
                                             : "sex flag set to male; ")
                               : "",
                race ? race->GetFormEditorID() : "?");
        }

        // ---- the head reconcile for a switched apply ----------------------

        // What finishes a race or sex switch. Round four (2026-08-22 04:15,
        // docs/handoffs/2026-08-22-head-regen-lever.md) exonerated every
        // Fitting Room painter and isolated the last defect to the render:
        // the npc's data is fully right after a switched apply and the
        // player's live head never changes, because out of menu the engine
        // rebuilds the BODY for the new race and leaves the face node
        // exactly as it was — old parts, old geometry. The user's RaceMenu
        // race-dial round-trip repairs it because the chargen path detaches
        // and rebuilds every part.
        //
        // PrepareHeadPartForShaders (skee64's source detours this entry as
        // "RegenerateHead") is the per-part derive-and-paint the engine's
        // full head build (SE 24846, measured 2026-08-22) runs once per
        // non-null entry of npc->headParts before one update of the node;
        // per part rather than face-only is what puts the hair through the
        // same bake.
        //
        // ⚠ THE CALL GOES THROUGH THE LIVE ENTRY BY DESIGN. CommonLib's
        // member routes through REL::Relocation, which lands on the
        // detoured entry: Fitting Room's own HeadBuildHook first (it counts
        // the build and queues the hair repaint and the skin pack's head
        // files behind the whole rebuild), then skee64's detour, which runs
        // the engine body and re-applies the whole mapped preset on the
        // fresh geometry (morphs, sculpt, tints). One call, three owners,
        // each doing the half only it can.
        //
        // ---- the base watch, round eight's instrument ---------------------
        //
        // The 11:34 census proved the reconcile lands the whole new head on
        // the live node and the BASE then reads nord again a handful of
        // seconds later, with zero head builds in the window: something
        // rewrites the TESNPC silently. This samples the base's race, sex
        // flag and part-list identity four times a second for thirty
        // seconds after a reconcile and logs every transition with a
        // wall-clock stamp, which brackets the reverter to a quarter
        // second beside whatever else the log shows at that moment.
        //
        // A WATCHER THREAD, the FaceWait idiom, never a task re-queue (the
        // same-pass drain scar). Reads are single aligned loads plus a
        // dereference of a race pointer, and races are forms that live for
        // the session; the part ARRAY is deliberately read as a pointer
        // identity only, because dereferencing its elements from another
        // thread during an engine write would be a use-after-free. One
        // watcher at a time; a second reconcile inside the window just
        // lets the running one see it.
        std::atomic<bool> g_baseWatchRunning{ false };

        // The render half of the watch, round nine's addition: the base
        // NEVER reverted (11:49, race and sex and part list held for the
        // whole window, one legitimate ChangeRace, zero head builds) and
        // the head still went back to the nord on screen. So the revert is
        // pure scenegraph, invisible to every data instrument, and the one
        // fact that names it is what the LIVE face node contains over time:
        // head geometry carries its part's editor name, so the child list
        // IS the part list of whatever is actually being rendered.
        //
        // Sampled ON THE GAME THREAD: the watcher thread queues one plain
        // task per tick (the FaceWait posture: a thread may hand the queue
        // single tasks; a task may never re-queue itself), and skips a tick
        // when the previous sample has not drained yet. The task walks
        // GetFaceNodeSkinned()'s direct children, hashes the names, and
        // logs the full list whenever the set changes.
        struct HeadWatchState {
            std::atomic<bool> sampleInFlight{ false };
            std::size_t       lastHash{ 0 };
        };

        void SampleFaceNode(const std::shared_ptr<HeadWatchState>& a_state,
                            long long a_ms) {
            auto* const pc = RE::PlayerCharacter::GetSingleton();
            auto* const node = pc ? pc->GetFaceNodeSkinned() : nullptr;
            if (!node) {
                if (a_state->lastHash != 1) {
                    a_state->lastHash = 1;
                    spdlog::info("ProfileApply: head-watch t+{} ms: NO face node.",
                                 a_ms);
                }
                return;
            }
            std::string names;
            std::size_t hash  = 0;
            int         count = 0;
            for (const auto& child : node->GetChildren()) {
                auto* const obj = child.get();
                if (!obj) {
                    continue;
                }
                const char* const name   = obj->name.c_str();
                const bool        hidden = obj->GetAppCulled();
                const float       scale  = obj->local.scale;
                names += name ? name : "<null>";
                // The two ways attached geometry renders as nothing: culled,
                // or scaled away. Round eleven's bald head had the hair
                // PRESENT in this list, so the names alone stopped being
                // the answer.
                if (hidden) {
                    names += "[HIDDEN]";
                }
                if (scale < 0.999f || scale > 1.001f) {
                    names += fmt::format("[s={:.2f}]", scale);
                }
                names += ", ";
                hash = hash * 1315423911u + std::hash<std::string_view>{}(
                                                name ? name : "");
                hash = hash * 31 + (hidden ? 7u : 1u);
                hash = hash * 31 + static_cast<std::size_t>(scale * 100.0f);
                ++count;
            }
            hash = hash * 31 + reinterpret_cast<std::uintptr_t>(node);
            if (hash != a_state->lastHash) {
                a_state->lastHash = hash;
                spdlog::info(
                    "ProfileApply: head-watch t+{} ms: node {} children({}): {}.",
                    a_ms, static_cast<const void*>(node), count, names);
            }
        }

        // Round seventeen: FSMP's repoint SUCCEEDS on the wig (its verbose log)
        // and the vanilla SkinSingleGeometry decompile is the SAME three
        // writes (bones, world-transform pointers, rootParent), so the broken
        // layer is one NOBODY writes. This dumps everything a skinned shape
        // renders through, for every direct child of the face node: skin
        // instance identity, bone count, first and last bone with WORLD
        // positions, dismember partition slots with their editorVisible flag
        // (+ on, - off), and the world bound. A child that is not geometry is
        // SAID rather than skipped (nitrishape-is-not-bsgeometry: silent drops
        // hide exactly the shape in question). Strip with the switch
        // instruments.
        void ProbeHeadSkins() {
            auto* const pc   = RE::PlayerCharacter::GetSingleton();
            auto* const node = pc ? pc->GetFaceNodeSkinned() : nullptr;
            if (!node) {
                spdlog::info("WigProbe: no face node.");
                return;
            }
            for (const auto& child : node->GetChildren()) {
                auto* const obj = child.get();
                if (!obj) {
                    continue;
                }
                const char* const nm = obj->name.empty() ? "(unnamed)" : obj->name.c_str();
                auto* const geom = obj->AsGeometry();
                if (!geom) {
                    spdlog::info("WigProbe: '{}' is not BSGeometry (rtti {}), worldBound r={:.1f}.",
                                 nm, obj->GetRTTI() ? obj->GetRTTI()->name : "?",
                                 obj->worldBound.radius);
                    continue;
                }
                auto* const skin = geom->GetGeometryRuntimeData().skinInstance.get();
                if (!skin) {
                    spdlog::info("WigProbe: '{}' NO SKIN INSTANCE, worldBound r={:.1f}.",
                                 nm, geom->worldBound.radius);
                    continue;
                }
                auto* const        sd = skin->skinData.get();
                const std::uint32_t nb = sd ? sd->bones : 0u;
                const auto boneTxt = [](RE::NiAVObject* a_bone) {
                    if (!a_bone) {
                        return std::string{ "<null>" };
                    }
                    const auto& p = a_bone->world.translate;
                    return fmt::format("'{}'@({:.0f},{:.0f},{:.0f})",
                                       a_bone->name.c_str(), p.x, p.y, p.z);
                };
                std::string bonesTxt = "none";
                if (skin->bones && nb > 0) {
                    bonesTxt = boneTxt(skin->bones[0]);
                    if (nb > 1) {
                        bonesTxt += " .. " + boneTxt(skin->bones[nb - 1]);
                    }
                }
                std::string parts = "plain-skin";
                if (auto* const dis = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin)) {
                    const auto& rd = dis->GetRuntimeData();
                    parts.clear();
                    for (std::int32_t i = 0; i < rd.numPartitions; ++i) {
                        parts += fmt::format("{}{}{}", i ? " " : "",
                                             rd.partitions[i].slot,
                                             rd.partitions[i].visible ? "+" : "-");
                    }
                    if (parts.empty()) {
                        parts = "0-partitions";
                    }
                }
                spdlog::info(
                    "WigProbe: '{}' skin={} bones={} rootParent={} [{}] parts[{}] "
                    "worldBound r={:.1f} c=({:.0f},{:.0f},{:.0f}).",
                    nm, static_cast<const void*>(skin), nb,
                    static_cast<const void*>(skin->rootParent), bonesTxt, parts,
                    geom->worldBound.radius, geom->worldBound.center.x,
                    geom->worldBound.center.y, geom->worldBound.center.z);
            }
        }

        std::atomic<bool> g_makeupProbeRunning{ false };

        // ⚠⚠ THE TINT LIST FR WRITES INTO IS NOT THE ONE THE PRESET PAINTED,
        // OR NOT YET, AND THIS IS THE PROBE THAT SAYS WHICH. FIELD 2026-08-23
        // (r32, 13:52:15): the face step answered at .248 and the makeup step
        // read the list at .406, and every slot in it carried a MALE default
        // texture at alpha 0 on a female Dark Elf. `MaleHeadWarPaint_01.dds`
        // where the preset holds `!COR\MakeupOverlays\Makeup_010.dds`. So the
        // match rule had nothing of the preset's to match against, the entry
        // fell through to the free-canvas pass, and it landed one slot over
        // from the copy the preset would put down later.
        //
        // Two questions this cannot answer from one reading: whether the
        // preset's tints ever reach THIS list, and if so when. So the same
        // census runs again on a thread that sleeps, at two rungs past the
        // apply. ⛔ A watcher thread posting single tasks is the legal shape;
        // never re-queue a task from inside a task (the FaceWait scar).
        //
        // ⚠ AN INSTRUMENT. It joins the one-commit strip.
        void DumpMakeupLive(const char* a_where);

        void ArmMakeupProbe() {
            if (g_makeupProbeRunning.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            // ⚠⚠ THE RUNGS BRACKET THE REVERT RATHER THAN MERELY WITNESSING
            // IT. FIELD r34 proved the write lands whole and is undone by
            // +2.5 s, and two rungs three and a half seconds apart cannot say
            // WHAT ran in between. These sit close enough together that the
            // reverting write falls inside one gap, and everything else in the
            // log is already timestamped, so the gap names its own suspect.
            // The sleeps are cumulative, so the labels are the elapsed time.
            std::thread([] {
                struct Rung {
                    int         sleepMs;
                    const char* label;
                };
                static constexpr Rung kRungs[]{
                    { 250, "+0.25s" }, { 250, "+0.5s" }, { 500, "+1s" },
                    { 1000, "+2s" },   { 2000, "+4s" },  { 4000, "+8s" },
                };
                for (const auto& rung : kRungs) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(rung.sleepMs));
                    if (auto* const task = SKSE::GetTaskInterface()) {
                        task->AddTask(
                            [label = rung.label] { DumpMakeupLive(label); });
                    }
                }
                g_makeupProbeRunning.store(false, std::memory_order_release);
            }).detach();
        }

        void ArmBaseWatch() {
            if (g_baseWatchRunning.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            std::thread([] {
                const auto t0 = std::chrono::steady_clock::now();
                bool          morphCensusQueued = false;
                bool          earlyProbeQueued  = false;
                auto          headState  = std::make_shared<HeadWatchState>();
                // ⚠⚠ THIS THREAD READS NO ENGINE OBJECT. It sleeps, it counts,
                // and everything that touches the game goes through AddTask.
                // It used to open with a base read: GetActorBase(), then
                // npc->race, then GetFormID() on it, straight from this worker.
                // That is a torn read waiting for a load boundary, and it found
                // one. FIELD 2026-08-24: a look applied, a save loaded inside
                // the 30 s window, and npc->race came back holding 00013746 -
                // NordRace's form ID, not a pointer to it, caught mid-write by
                // the engine rebuilding the player's base. Dereferencing that
                // is an access violation and it took the game down.
                //
                // The reporting is not rebuilt here, because AppearanceWatch
                // already logs race, sex, the head part count and the list's
                // identity every second, on the game thread, and prints only
                // what changed. Two watchers over one base was the duplication
                // the watchdog existed to end; this is the half that had to go.
                while (std::chrono::steady_clock::now() - t0 <
                       std::chrono::seconds(30)) {
                    if (!headState->sampleInFlight.exchange(
                            true, std::memory_order_acq_rel)) {
                        if (auto* task = SKSE::GetTaskInterface()) {
                            const auto ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                            task->AddTask([headState, ms] {
                                SampleFaceNode(headState, ms);
                                headState->sampleInFlight.store(
                                    false, std::memory_order_release);
                            });
                        } else {
                            headState->sampleInFlight.store(
                                false, std::memory_order_release);
                        }
                    }
                    // Round sixteen's doubled silhouette: one morph-key census
                    // late in the settle, queued as ONE plain task from this
                    // watcher thread (the legal FaceWait posture, same as the
                    // sampler above). The boundary census in Body Studio fires
                    // at the first custom write; this one catches any writer
                    // that lands AFTER the settle refreshes. Strip with the
                    // rest of the switch instruments.
                    if (!earlyProbeQueued &&
                        std::chrono::steady_clock::now() - t0 >
                            std::chrono::seconds(4)) {
                        earlyProbeQueued = true;
                        if (auto* task = SKSE::GetTaskInterface()) {
                            // The round-seventeen exit came 8 s after the
                            // apply and lost the whole late block, so the
                            // wig probe gets an early sample too.
                            //
                            // Round nineteen: the restore RIDES THIS TASK,
                            // before the probe. The shipped restore ran
                            // inside the head-build chain and lost the
                            // BSTaskPool attach race; this tick is +4 s
                            // after the apply, where the probe has already
                            // proven the wig reachable. The restore's own
                            // line is the before (how many OFF), the probe
                            // on the same tick is the after.
                            task->AddTask([] {
                                if (auto* const pc =
                                        RE::PlayerCharacter::GetSingleton()) {
                                    BipedPost::RestoreHeadPartPartitions(
                                        pc, "settle+4s");
                                }
                                ProbeHeadSkins();
                            });
                        }
                    }
                    if (!morphCensusQueued &&
                        std::chrono::steady_clock::now() - t0 >
                            std::chrono::seconds(10)) {
                        morphCensusQueued = true;
                        if (auto* task = SKSE::GetTaskInterface()) {
                            task->AddTask([] {
                                // Restore first for the +4 s task's reason:
                                // its line is the before, the probe after it
                                // is the after, same tick.
                                if (auto* const pc =
                                        RE::PlayerCharacter::GetSingleton()) {
                                    BipedPost::RestoreHeadPartPartitions(
                                        pc, "settle+10s");
                                }
                                RaceMenuMorphApi::LogKeyCensus(
                                    RE::PlayerCharacter::GetSingleton(),
                                    "switch-settle-10s");
                                ProbeHeadSkins();
                            });
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                spdlog::info("ProfileApply: settle watch ended (30 s).");
                g_baseWatchRunning.store(false, std::memory_order_release);
            }).detach();
        }

        // ⚠⚠ THE BAKE ALONE IS NOT ENOUGH, AND THE 05:05 FIELD ROUND IS THE
        // MEASUREMENT. The first cut of this function only ran the per-part
        // regeneration, seventeen calls, clean log — and the head stayed the
        // nord's, because out of menu the engine never attaches the new
        // parts to the player's live face node at all (the outfit push's
        // eyes swap in the same log detached 'MaleEyesHumanLightBlue' from
        // that node AFTER the "regenerated" line). The regeneration finds
        // each part's geometry BY NAME, and the new parts' names were not
        // there to find. So the parts are SWAPPED first, through the
        // engine's own per-part chargen swap (HeadPart::SwapSwitchedParts,
        // OS-230's machinery), which detaches the old geometry and builds,
        // paints and skins the new — SkinSingleGeometry included, which is
        // the call FSMP patches, so the SMP hair comes out simulating.
        //
        // The bake then still runs over every part: a part the swap kept
        // (same form both sides of a sex flip) re-derives with the flipped
        // sex, and re-deriving a part the swap just built is idempotent.
        // A missing node or base logs and returns; an empty snapshot means
        // the swaps are skipped and only the bake runs (the pre-lever
        // behaviour, and the log says so).
        void ReconcileSwitchedHead(RE::Actor* a_player,
                                   const std::vector<RE::BGSHeadPart*>& a_preSwitch) {
            auto* const npc  = a_player ? a_player->GetActorBase() : nullptr;
            auto* const node = a_player ? a_player->GetFaceNodeSkinned() : nullptr;
            if (!npc || !node) {
                spdlog::warn(
                    "ProfileApply: the switched head could not reconcile "
                    "({} is missing); the RaceMenu round-trip still repairs "
                    "it by hand.",
                    !npc ? "the actor base" : "the face node");
                return;
            }
            const auto swap = HeadPart::SwapSwitchedParts(a_player, a_preSwitch);
            auto* const mgr = RE::BSFaceGenManager::GetSingleton();
            int         baked = 0;
            for (std::int8_t i = 0; i < npc->numHeadParts; ++i) {
                if (auto* const part = npc->headParts[i]) {
                    mgr->PrepareHeadPartForShaders(node, part, npc);
                    ++baked;
                }
            }
            RE::NiUpdateData update{};
            node->Update(update);
            // The node pointer rides the line so the census (HeadBuildHook)
            // can say whether a later build painted THIS node or a
            // replacement.
            spdlog::info(
                "ProfileApply: head reconciled for the switched character "
                "({} swapped, {} kept, {} removed, {} baked, node {}{}).",
                swap.swapped, swap.kept, swap.removed, baked,
                static_cast<const void*>(node),
                swap.available ? "" : "; the engine swap was unavailable, only the bake ran");
            ArmBaseWatch();
        }

        // ⚠⚠ A SAME-RACE FACE APPLY MUST REGENERATE TOO, r63's lever. skee's
        // LoadCharacterEx on a character already wearing this race runs NO
        // engine head build (r61 apply #2: zero census lines), so its sculpt
        // and extended morphs land `+=` on the LIVE head's FOD base
        // (FaceMorphInterface.cpp:1118/1194) - applied once at the first
        // build, applied AGAIN by the update, the face bends by one extra
        // sculpt (head fod 1488959 against the true 1487186, MEASURED). The
        // race-switch path never shows it because ReconcileSwitchedHead's
        // per-part RegenerateHead pass (SE 26259 / AE 26838) rebuilds each
        // part's base from its chargen tri and the morphs re-apply ONCE onto
        // fresh data - every reconciled apply measured the clean single
        // value. So the cure is the same pass without the swap half: bake
        // every part fresh, one node update, and arm the same settle watch
        // (the FSMP republish rides SwitchSettling, and a regenerated SMP
        // wig needs it exactly as a switched one does).
        void RegenerateAppliedHead(RE::Actor* a_player) {
            auto* const npc  = a_player ? a_player->GetActorBase() : nullptr;
            auto* const node = a_player ? a_player->GetFaceNodeSkinned() : nullptr;
            if (!npc || !node) {
                spdlog::warn(
                    "ProfileApply: the applied head could not regenerate "
                    "({} is missing); the doubled morphs stand until the "
                    "next full build.",
                    !npc ? "the actor base" : "the face node");
                return;
            }
            auto* const mgr = RE::BSFaceGenManager::GetSingleton();
            int         baked = 0;
            for (std::int8_t i = 0; i < npc->numHeadParts; ++i) {
                if (auto* const part = npc->headParts[i]) {
                    mgr->PrepareHeadPartForShaders(node, part, npc);
                    ++baked;
                }
            }
            RE::NiUpdateData update{};
            node->Update(update);
            spdlog::info(
                "ProfileApply: head regenerated after the same-race face "
                "apply ({} baked, node {}).",
                baked, static_cast<const void*>(node));
            ArmBaseWatch();
        }

        // ---- one function per step, each logging its checklist line -------

        // The outfit step serves TWO checkboxes (spec note in ProfilePlan.h):
        // gear and dyes compose into one push, because two pushes of one
        // appearance would be the drift this pipeline exists to prevent.
        //   gear && dyes  - the profile outfit whole, as before.
        //   gear && !dyes - the profile's gear wearing the CURRENT outfit's
        //                   dyes, so applying an outfit does not repaint it.
        //   !gear && dyes - the current outfit wearing the profile's dyes.
        // The body fields always start from the CURRENT outfit: the body is
        // its own block now, and StepBody (which runs later in the pinned
        // order) writes them when the body box is checked. An import with no
        // current outfit keeps the profile's own fields; there is nothing on
        // this rig for "stays as it is" to mean there.
        void StepOutfit(const ProfileCodec::Profile& a_profile,
                        const ProfilePlan::Participation& a_take) {
            const auto& outfit  = *a_profile.outfit;
            auto&       session = OutfitSession::GetSingleton();
            int         index   = -1;
            bool        updated = false;
            bool        noDyeTarget = false;
            session.WithLibrary([&](OutfitLibrary& a_lib) {
                if (!a_take.outfit) {
                    // Dyes only: recolour whatever is active, touch nothing
                    // else about it.
                    const int active = a_lib.ActiveIndex();
                    if (active < 0) {
                        noDyeTarget = true;
                        return;
                    }
                    auto* current = a_lib.At(static_cast<std::size_t>(active));
                    if (!current) {
                        noDyeTarget = true;
                        return;
                    }
                    current->CopyDyeStateFrom(outfit);
                    index   = active;
                    updated = true;
                    return;
                }
                const Outfit* current =
                    a_lib.ActiveIndex() >= 0
                        ? a_lib.At(static_cast<std::size_t>(a_lib.ActiveIndex()))
                        : nullptr;
                Outfit incoming = outfit;
                if (!a_take.dyes && current) {
                    incoming.CopyDyeStateFrom(*current);
                }
                if (current) {
                    // The body rides its own checkbox; the push must not drag
                    // the profile outfit's embedded body fields along.
                    incoming.obodyPreset        = current->obodyPreset;
                    incoming.customBodyPresetId = current->customBodyPresetId;
                }
                for (std::size_t i = 0; i < a_lib.Count(); ++i) {
                    if (auto* existing = a_lib.At(i);
                        existing && existing->name == incoming.name) {
                        *existing = incoming;
                        index     = static_cast<int>(i);
                        updated   = true;
                        break;
                    }
                }
                if (index < 0) {
                    index = a_lib.Create(incoming.name);
                    if (index >= 0) *a_lib.At(static_cast<std::size_t>(index)) = incoming;
                }
                if (index >= 0) a_lib.Activate(static_cast<std::size_t>(index));
            });
            if (noDyeTarget) {
                spdlog::info("ProfileApply: step 'outfit' (dyes only, and no "
                             "outfit is active to carry them; nothing "
                             "painted).");
                return;
            }
            if (index < 0) {
                spdlog::warn("ProfileApply: step 'outfit' dropped, the outfit "
                             "library is full.");
                return;
            }
            // The ladder runs the whole look: head parts before hair colour,
            // the kPostLoadGame ordering, and dyes ride inside. Re-read from
            // the library so the push carries exactly what was stored.
            std::optional<Outfit> pushed;
            session.WithLibrary([&](OutfitLibrary& a_lib) {
                if (const auto* stored = a_lib.At(static_cast<std::size_t>(index))) {
                    pushed = *stored;
                }
            });
            if (!pushed) {
                return;
            }
            session.PushPlayerLookFor(&*pushed);
            spdlog::info("ProfileApply: step 'outfit' ('{}' {} at index {}, "
                         "gear {}, dyes {}, look pushed).",
                         pushed->name, updated ? "updated" : "imported", index,
                         a_take.outfit ? "applied" : "kept",
                         a_take.dyes ? "applied" : "kept");
        }

        // The census itself, on the game thread. ⚠ THE TEXTURE AND THE ALPHA
        // BESIDE THE TYPE: the type is the half that moves, and the round that
        // found the mismatch could only infer the other half from a jslot on
        // disk. The match rule keys on the path now, so the path is what a
        // field round has to be able to read.
        void DumpMakeupLive(const char* a_where) {
            if (!spdlog::should_log(spdlog::level::debug)) {
                return;
            }
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            const auto layers = MakeupApi::Layers(player);
            const auto state  = MakeupApi::Read(player);
            std::string layout;
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (!layout.empty()) layout += ' ';
                layout += fmt::format("{}:{}", i, MakeupPlan::IdFor(layers[i].type));
            }
            const auto* const npc = player->GetActorBase();
            spdlog::debug(
                "ProfileApply: makeup live list [{}] ({} layer(s), {}; "
                "bodyTintColor ({},{},{})): {}",
                a_where, layers.size(), MakeupApi::ListOrigin(player),
                npc ? npc->bodyTintColor.red : 0, npc ? npc->bodyTintColor.green : 0,
                npc ? npc->bodyTintColor.blue : 0, layout);
            for (std::size_t i = 0; i < layers.size() && i < state.size(); ++i) {
                spdlog::debug(
                    "ProfileApply: makeup live [{}] {:2d} {:12s} alpha={:3d} "
                    "texture='{}'",
                    a_where, i, MakeupPlan::IdFor(layers[i].type),
                    OverlayPlan::AlphaByte(state[i].strength),
                    state[i].hasTexture ? state[i].texture : "(none)");
            }
        }

        // ⚠⚠ A PRESET FACE ARRIVES WITH ITS TINTS BAKED, AND THE SKIN TONE IS
        // ALL IT GETS TO KEEP OUT OF THE LIST. r40 field: writing the WHOLE
        // capture beside a face block ended in a retint that rebuilt the face
        // texture out of the tint list and threw away what skee's preset apply
        // had just painted, so this step stood aside entirely whenever a face
        // was carried. The user's A/B of 2026-09-01 is the other half: a look
        // applied whole left its makeup and tint masks EMPTY, while unticking
        // everything except makeup and applying loaded them perfectly, through
        // the very write the stand-aside withheld. And the first cut of the
        // write, paint only, lost the LIPS to the first face-overlay edit,
        // because the edit's retint composites from the list and skee never
        // populates it (measured 20:12 the same evening).
        // MakeupPlan::PlanFaceCarried carries the split: everything the look
        // wears goes into the list except the skin tone, whose hold the apply
        // deliberately releases, and every unclaimed slot but the tone is
        // cleared so the previous look's markings never survive by omission
        // (the d46f6a88 rule, now the replace half of a write, widened to the
        // kFace band by the second field round's surviving lips).
        //
        // ⚠ WHY THE WRITE IS SAFE HERE AND WAS NOT IN r40: the one retint it
        // triggers lands inside the apply, where the stand-down at the end of
        // RunSteps keeps the face watch quiet (ArmDelayedFaceRebake checks the
        // quiet window before anything else, and the apply drops the hold), and
        // the export the face block binds arrives with the head build AFTER, so
        // the preset's face is the last painter exactly as it is on the
        // clear-only path the field confirmed. What changes is the LIST a later
        // retint composites from: with the look's own paint in it, an Overlays
        // edit rebuilds the face the look meant instead of one missing its
        // makeup.
        void StepMakeup(RE::Actor* a_player, const ProfileCodec::Profile& a_profile,
                        bool a_faceCarried) {
            if (a_faceCarried) {
                const auto layers = MakeupApi::Layers(a_player);
                auto       live   = MakeupApi::Read(a_player);
                // ⚠⚠ THE r35 REFUSAL GUARDS THE PLAN: a list that is not this
                // race's makes every captured index a stranger's slot and its
                // type fields unreliable, so nothing is written into it and
                // only the conservative clear below runs, exactly as it did
                // before the write existed.
                const auto raceSlots = RaceTint::Slots(a_player);
                const bool listIsThisRaces =
                    raceSlots.empty() || raceSlots.size() == layers.size();
                if (listIsThisRaces) {
                    // The plan runs even when the look captured nothing: the
                    // replace half still owes the clear. Field 2026-09-01,
                    // second round: 'Nord 3' carries only its tone, its apply
                    // left Almalexia's lips standing in the kFace band, and the
                    // next edit's retint wore them. The plan's eraser takes
                    // every unclaimed slot but the tone, so a look that wears
                    // nothing leaves nothing.
                    DumpMakeupLive("at-facecarried-write");
                    static const std::vector<ProfileCodec::MakeupEntry> kNoMakeup;
                    const auto plan = MakeupPlan::PlanFaceCarried(
                        a_profile.makeup ? *a_profile.makeup : kNoMakeup,
                        layers, live);
                    if (!plan.indices.empty()) {
                        MakeupApi::Write(a_player, plan.indices, plan.target);
                    }
                    spdlog::info(
                        "ProfileApply: step 'makeup' replaced the list beside "
                        "the look's face ({} written, {} stale cleared, {} "
                        "skipped for missing slots, {} skin tone entr{} left "
                        "to the preset).",
                        plan.written, plan.cleared, plan.skipped, plan.kept,
                        plan.kept == 1 ? "y" : "ies");
                    ArmMakeupProbe();
                    return;
                }
                spdlog::warn(
                    "ProfileApply: step 'makeup' cleared only. The live tint "
                    "list is {} slot(s) and this race's record carries {}, so "
                    "the list belongs to another race and the look's layers "
                    "would land in a stranger's slots. Live list: {}.",
                    layers.size(), raceSlots.size(),
                    MakeupApi::ListOrigin(a_player));
                // ⚠⚠ BUT THE OLD LOOK'S PAINT DOES NOT GET TO STAY. Standing
                // aside leaves the list exactly as the PREVIOUS character left
                // it, and a retint rebuilds the face out of the tint list. It
                // does not care that the layers belong to somebody else. Field
                // 2026-09-01: apply a look over another, open the Overlays
                // page, change a layer, and the previous character's markings
                // come back over a face that is otherwise correct.
                //
                // ⚠ SO THE PAINT IS CLEARED AND THE COMPLEXION IS NOT.
                // MakeupPlan::AllowsClear already draws that line and it is the
                // user's own call, 2026-08-16: a face slot is not an accessory,
                // and emptying SkinTone or Neck removes the character's
                // complexion rather than any makeup. Only the warpaint and dirt
                // library is cleared, which is where the markings live.
                std::vector<std::size_t> cleared;
                for (std::size_t i = 0; i < layers.size() && i < live.size(); ++i) {
                    if (!MakeupPlan::AllowsClear(MakeupPlan::CategoryOf(layers[i].type))) {
                        continue;
                    }
                    // ⚠⚠ THE TYPES ARE ZERO ON THIS LIST, so the category test
                    // above cannot see the tone. Field 2026-09-02 02:58: this
                    // pass wrote the SKIN TONE to alpha 0 on a 108 slot list
                    // over a 34 slot race, and the face lost its complexion.
                    // The file name is the identity the type cannot give.
                    if (MakeupPlan::IsSkinToneTexture(live[i].texture)) {
                        continue;
                    }
                    if (!live[i].hasTexture && live[i].strength <= 0.0f) {
                        continue;  // already empty, so nothing to say or write
                    }
                    live[i] = MakeupPlan::LayerState{};
                    cleared.push_back(i);
                }
                if (!cleared.empty()) {
                    MakeupApi::Write(a_player, cleared, live);
                    spdlog::info(
                        "ProfileApply: step 'makeup' cleared {} paint layer(s) the previous "
                        "look left behind, so a later retint composites from an empty list "
                        "rather than from that character's markings. The complexion slots "
                        "are untouched, because emptying those removes the face rather than "
                        "the makeup.",
                        cleared.size());
                }
                return;
            }
            // Fresh read first: a face step before this one rebuilt the tint
            // list, and writing against a stale picture of it is the drift the
            // Fingerprint exists to catch.
            const auto layers = MakeupApi::Layers(a_player);
            // ⚠⚠ REFUSE THE WHOLE STEP WHEN THE LIVE LIST IS NOT THIS RACE'S.
            // MEASURED r35 on a Nord wearing a Dark Elf look: the live tint
            // list is 34 slots of VANILLA DEFAULTS (`MaleHeadWarPaint_01..06`,
            // `MaleHead_Frekles_01`) while the race record the look switched
            // to carries 108. It is the OLD race's list and it is never
            // rebuilt: the six rungs read 34 at +0.25 s and still 34 at +8 s.
            //
            // Writing into it is worse than doing nothing, and this is the
            // whole of the field's "the scar isn't the right colour". FR's
            // colours land on slots whose ART is a vanilla stencil, something
            // puts the default names back between +0.5 s and +1 s, and FR's
            // alphas stay. Those slots sat at zero before we touched them, so
            // OUR WRITE IS WHAT MAKES THE WRONG STENCIL VISIBLE. Standing
            // aside leaves the face the preset painted.
            if (const auto raceSlots = RaceTint::Slots(a_player);
                !raceSlots.empty() && raceSlots.size() != layers.size()) {
                spdlog::warn(
                    "ProfileApply: step 'makeup' STOOD ASIDE. The live tint "
                    "list is {} slot(s) and this race's record carries {}, so "
                    "the list belongs to another race and every index in the "
                    "capture would name a stranger's slot. The face keeps what "
                    "the face step painted. Live list: {}.",
                    layers.size(), raceSlots.size(), MakeupApi::ListOrigin(a_player));
                ArmMakeupProbe();
                return;
            }
            auto       target = MakeupApi::Read(a_player);
            std::vector<std::size_t> indices;
            std::size_t skipped = 0;
            // The live list's layout, once per apply. The first field round
            // skipped 7 of 7 entries and the one-line summary could not say
            // whether the list shrank or the types moved; this line plus the
            // per-entry skip below is what the next round decides the match
            // rule from.
            DumpMakeupLive("at-write");
            // ⚠⚠ THE CAPTURED INDEX IS A HINT, NOT AN IDENTITY. Round fourteen
            // field: RaceMenu reshapes this list per session, and index+type
            // wrote a war paint into a stranger's slot. MakeupPlan::MatchSlot
            // holds the rules and the test; this loop only claims and writes.
            std::vector<bool> claimed(target.size(), false);
            for (const auto& entry : *a_profile.makeup) {
                const std::size_t i = MakeupPlan::MatchSlot(entry.index, entry.type,
                                                            entry.state, layers,
                                                            target, claimed);
                if (i == MakeupPlan::kNoSlot) {
                    spdlog::info(
                        "ProfileApply: makeup entry skipped: captured index {} "
                        "type '{}' matched no live slot ({} layer(s); texture "
                        "'{}').",
                        entry.index, MakeupPlan::IdFor(entry.type), layers.size(),
                        entry.state.hasTexture ? entry.state.texture : "(none)");
                    ++skipped;
                    continue;
                }
                if (i != entry.index) {
                    spdlog::info(
                        "ProfileApply: makeup entry rehomed: captured index {} "
                        "type '{}' writes live slot {}.",
                        entry.index, MakeupPlan::IdFor(entry.type), i);
                }
                auto next = entry.state;
                if (!next.hasTexture) {
                    // The capture did not own the texture, so the slot keeps
                    // the one the race (or the jslot) put there.
                    next.hasTexture = target[i].hasTexture;
                    next.texture    = target[i].texture;
                }
                claimed[i] = true;
                target[i]  = std::move(next);
                indices.push_back(i);
            }
            // ⚠⚠ A LOOK'S MAKEUP REPLACES ON THIS PATH TOO. Field 2026-09-02:
            // a makeup-only import of Almalexia onto Umbrael left Umbrael's
            // skull warpaint standing beside the imported set, because only
            // the face-carried path had learned the 2026-08-27 replace
            // lesson. Same eraser, same question: every unclaimed slot but
            // the skin tone comes off, so the previous character's markings
            // never survive by omission here either.
            const std::size_t written = indices.size();
            std::size_t       cleared = 0;
            for (std::size_t i = 0; i < layers.size() && i < target.size(); ++i) {
                if (layers[i].type ==
                    static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                    continue;
                }
                if (i < claimed.size() && claimed[i]) {
                    continue;
                }
                if (!target[i].hasTexture && target[i].strength <= 0.0f) {
                    continue;  // already empty, so nothing to say or write
                }
                target[i] = MakeupPlan::LayerState{};
                indices.push_back(i);
                ++cleared;
            }
            MakeupApi::Write(a_player, indices, target);
            spdlog::info("ProfileApply: step 'makeup' ({} layer(s) written, {} "
                         "stale cleared, {} skipped, one rebake).",
                         written, cleared, skipped);
            ArmMakeupProbe();
        }

        void StepWeight(RE::Actor* a_player, const ProfileCodec::Profile& a_profile) {
            BodyWeight::Set(a_player, *a_profile.weight);
            spdlog::info("ProfileApply: step 'weight' ({:.1f}).", *a_profile.weight);
        }

        // The body block writes the ACTIVE outfit's body fields and stops.
        // The outfit's fields are the one painter of the player's body (the
        // refresh's body pass reads them and nothing else), so "apply this
        // body" means "make the active outfit say it"; painting past the
        // outfit here would be a second painter the next refresh fights.
        // The settle refresh at the end of RunSteps is what paints.
        void StepBody(RE::Actor*, const ProfileCodec::Profile& a_profile) {
            const auto& body = *a_profile.body;
            std::string customId;
            if (body.custom) {
                // Embedding is what makes the profile self-contained: put the
                // payload into the store so customBodyPresetId resolves on
                // this rig too.
                auto        preset = *body.custom;
                std::string error;
                if (BodyPresetStore::GetSingleton().Save(preset, error)) {
                    customId = preset.id;
                } else if (error.find("already uses that name") !=
                           std::string::npos) {
                    // A DIFFERENT preset already owns the name here. The
                    // embedded payload still has to win - it is what the look
                    // captured - so it lands under a derived name.
                    preset.name += " (look)";
                    std::string retryError;
                    if (BodyPresetStore::GetSingleton().Save(preset, retryError)) {
                        customId = preset.id;
                    } else {
                        spdlog::warn("ProfileApply: step 'body' embedded "
                                     "preset '{}' not stored: {}",
                                     preset.name, retryError);
                    }
                } else {
                    spdlog::warn("ProfileApply: step 'body' embedded preset "
                                 "'{}' not stored: {}", preset.name, error);
                }
            }
            bool wrote = false;
            OutfitSession::GetSingleton().WithLibrary([&](OutfitLibrary& a_lib) {
                const int active = a_lib.ActiveIndex();
                auto*     outfit = active >= 0
                                       ? a_lib.At(static_cast<std::size_t>(active))
                                       : nullptr;
                if (!outfit) {
                    return;
                }
                if (!customId.empty()) {
                    outfit->customBodyPresetId = customId;
                    outfit->obodyPreset.clear();
                } else {
                    outfit->obodyPreset        = body.obodyPreset;
                    outfit->customBodyPresetId.clear();
                }
                wrote = true;
            });
            if (!wrote) {
                spdlog::info("ProfileApply: step 'body' (no active outfit to "
                             "carry the body; nothing written).");
                return;
            }
            // The look's body ALSO becomes this character's DEFAULT (field
            // 2026-08-24): the active outfit carried the preset, so switching
            // to any outfit that names none fell back to a default that was
            // never set - the body snapped to zeroed. A look states who the
            // character is, which is exactly what DefaultBody exists to hold,
            // and the fallback rung in the body pass already reads it.
            if (!customId.empty() || !body.obodyPreset.empty()) {
                auto* const player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    DefaultBody::Set(player, body.obodyPreset, customId);
                    spdlog::info(
                        "ProfileApply: step 'body' set the character default "
                        "({}), so outfits naming no body keep this look's.",
                        !customId.empty() ? "custom preset" : body.obodyPreset);
                }
            }
            spdlog::info("ProfileApply: step 'body' ({} written to the active "
                         "outfit; the settle refresh applies it).",
                         !customId.empty()
                             ? "embedded custom preset"
                             : (body.obodyPreset.empty() ? "no preset"
                                                         : "OBody preset"));
        }

        void StepShape(RE::Actor* a_player, const ProfileCodec::Profile& a_profile) {
            const auto& shape = *a_profile.shape;
            std::size_t morphs = 0;
            if (!shape.morphs.empty() && RaceMenuMorphApi::Available()) {
                std::vector<RaceMenuMorphApi::MorphValue> values;
                values.reserve(shape.morphs.size());
                for (const auto& [name, value] : shape.morphs) {
                    values.push_back({ name, value });
                }
                const auto result = RaceMenuMorphApi::ApplyShape(a_player, values);
                morphs            = result.valuesSet;
            }
            std::size_t bones = 0;
            if (!shape.scales.empty() && NodeTransformApi::Available()) {
                std::vector<ShapeOverlay::Adjustment> plan;
                for (const auto& [id, value] : shape.scales) {
                    for (std::size_t i = 0; i < ShapeOverlay::kSliderCount; ++i) {
                        if (id == ShapeOverlay::kSliders[i].id) {
                            auto part = ShapeOverlay::PlanFor(i, value);
                            plan.insert(plan.end(), part.begin(), part.end());
                            break;
                        }
                    }
                }
                const auto result = NodeTransformApi::Apply(a_player, plan);
                bones             = result.bonesWritten;
            }
            spdlog::info("ProfileApply: step 'shape' ({} morph value(s), {} "
                         "bone(s) scaled).", morphs, bones);
        }

        void StepSkin(RE::Actor* a_player, const ProfileCodec::Profile& a_profile) {
            SkinApi::Apply(a_player, a_profile.skin->pack);
            spdlog::info("ProfileApply: step 'skin' (pack '{}').",
                         a_profile.skin->pack);
        }

        void StepOverlays(RE::Actor* a_player, const ProfileCodec::Profile& a_profile) {
            if (!OverlayApi::Available()) {
                spdlog::warn("ProfileApply: step 'overlays' dropped, the "
                             "overlay interfaces are unavailable.");
                return;
            }
            if (!OverlayApi::HasOverlays(a_player)) {
                OverlayApi::Install(a_player);
            }
            const auto& layers  = OverlayApi::Layers();
            std::size_t written = 0;
            std::size_t skipped = 0;
            std::size_t cleared = 0;
            std::vector<const OverlayPlan::Layer*> named;
            for (const auto& info : OverlayPlan::kLocations) {
                const auto& entries =
                    a_profile.overlays->byLocation[OverlayPlan::Slot(info.location)];
                for (const auto& entry : entries) {
                    const OverlayPlan::Layer* slot = nullptr;
                    for (const auto& layer : layers) {
                        if (layer.location == info.location &&
                            layer.index == entry.index) {
                            slot = &layer;
                            break;
                        }
                    }
                    if (!slot) {
                        // Captured on a rig with more layers of this location
                        // than this one has (the counts come from skee64.ini).
                        ++skipped;
                        continue;
                    }
                    OverlayApi::Write(a_player, slot->node, entry.state);
                    named.push_back(slot);
                    ++written;
                }
            }
            // ⚠⚠ A LOOK REPLACES THE LAYERS, IT DOES NOT ADD TO THEM. Writing
            // only the slots the incoming look names left every other slot
            // carrying the previous character's art, and the two looks then wore
            // each other. Field 2026-08-27: importing 'Almalexia' onto Umbrael
            // wrote five layers over a character already holding six, and the
            // sixth stayed on the face. The user's own words for it were that
            // both presets were mashed together, and that the layers vanish for
            // a moment and come back, which is skee's store being re-pushed with
            // the leftovers still in it.
            //
            // ⚠ CLEARED FROM THE STORE AND NOT THE SCENEGRAPH, which is why the
            // re-push cannot bring them back a second time. Only occupied slots
            // are touched, so a look with fewer layers costs a handful of map
            // lookups rather than a write per slot on the rig.
            for (const auto& layer : layers) {
                const bool isNamed =
                    std::ranges::any_of(named, [&](const OverlayPlan::Layer* a_l) {
                        return a_l->node == layer.node;
                    });
                if (isNamed || !OverlayPlan::Occupied(OverlayApi::Read(a_player, layer.node))) {
                    continue;
                }
                OverlayApi::Clear(a_player, layer.node);
                ++cleared;
            }
            // The settled point the dye chain already uses: the 1p clones get
            // the layers skee never paints there, and the census accounts for
            // the round in the log.
            Overlay1P::PaintPlayer(a_player);
            OverlayReconcile::LogOverlayCensus(a_player);
            spdlog::info("ProfileApply: step 'overlays' ({} layer(s) written, "
                         "{} cleared because this look does not name them, {} "
                         "skipped for missing slots).",
                         written, cleared, skipped);
            // ⚠⚠ AND NOTHING PUTS THE OLD ONES BACK OVER THESE. The step before
            // this one is 'skin', a skin repaint arms the node push, and the
            // push re-asserts what SKEE holds rather than what this step just
            // decided. Field 2026-09-01, a look applied over a loaded save: the
            // step cleared to nothing at 14:51:34.013, the push fired at
            // 14:51:35.349, and the PREVIOUS character's 7 body layers were back
            // 155 ms later wearing her face overlay too.
            //
            // ⚠ AFTER THE WRITE AND NOT BEFORE IT, so the census above still
            // reports what this step decided, and so a stand-down is never left
            // armed by a step that bailed out early.
            OverlayApi::StandDownNodePush(OverlayPlan::kNodePushQuietAfterApplyMs);
        }

        void RunStep(ProfilePlan::Step a_step, RE::Actor* a_player,
                     const ProfileCodec::Profile& a_profile,
                     const ProfilePlan::Participation& a_take) {
            switch (a_step) {
                case ProfilePlan::Step::kCharacter:
                    break;  // runs at the head of the apply task, never here
                case ProfilePlan::Step::kFace:
                    break;  // dispatched ahead of the drain, never here
                case ProfilePlan::Step::kOutfit:
                    StepOutfit(a_profile, a_take);
                    break;
                case ProfilePlan::Step::kMakeup:
                    // A face block in the same apply narrows this step to the
                    // paint library; the complexion is the preset's. See
                    // StepMakeup.
                    StepMakeup(a_player, a_profile,
                               a_take.face && a_profile.face.has_value());
                    break;
                case ProfilePlan::Step::kWeight:
                    StepWeight(a_player, a_profile);
                    break;
                case ProfilePlan::Step::kBody:
                    StepBody(a_player, a_profile);
                    break;
                case ProfilePlan::Step::kShape:
                    StepShape(a_player, a_profile);
                    break;
                case ProfilePlan::Step::kSkin:
                    StepSkin(a_player, a_profile);
                    break;
                case ProfilePlan::Step::kOverlays:
                    StepOverlays(a_player, a_profile);
                    break;
            }
        }

        void RunSteps(std::vector<ProfilePlan::Step> a_steps,
                      ProfileCodec::Profile a_profile,
                      ProfilePlan::Participation a_take) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) {
                spdlog::warn("ProfileApply: the player unloaded mid-apply; the "
                             "remaining steps are dropped.");
                SetInFlight(false);
                return;
            }
            // ⚠⚠ THE LIST IS REBUILT TO THIS RACE'S SLOTS BEFORE ANY STEP WRITES
            // INTO IT, which closes r35 (2026-08-23 to 2026-09-02). A race
            // switch, skee's SetRace inside LoadCharacterEx or StepCharacter's
            // own, never builds the new race's tint slots: a 34 slot Nord list
            // survived every switch to a 108 slot race, the makeup step, the
            // body sync and the record all refused it (correctly), and the look
            // had no makeup until the character editor opened, whose init
            // rebuilds the list from the race (measured 03:44:42). This runs
            // the same engine rebuild here, after the race is committed and
            // before the makeup step, so the step writes into a list this race
            // owns. A sex flip on one race switches the race's HALF (male masks
            // on a female character), so it counts as a switch too.
            MakeupApi::RebuildListForRace(
                player, g_switchedThisApply.exchange(false, std::memory_order_acq_rel));
            for (const auto step : a_steps) {
                RunStep(step, player, a_profile, a_take);
            }
            // ⚠⚠ THE OTHER HALF OF THE PAIR EVERY STAGING PATH RUNS: push,
            // then refresh. StepOutfit ran PushPlayerLookFor and stopped, and
            // the first field round measured what that leaves: the dye restore
            // skipped every material as stale, no hair repaint fired, and the
            // body pass that resolves customBodyPresetId never ran - until the
            // editor opened and its refresh did all three. This is that same
            // refresh (equipment rebuild, OutfitDye restore + repaint,
            // HairColor::Repaint, then the body state pass), not a parallel
            // painter. Queued here rather than per step so one settle covers
            // the whole apply; a task added during this drain runs in the same
            // pass, so no frame shows the unsettled state.
            // ⚠⚠ THE BODY AFTER A FACE BLOCK, BECAUSE A FACE BLOCK REPLACES THE
            // TINT LIST UNDERNEATH US AND NOTHING REPAINTS THE BODY FROM THE NEW
            // ONE. MEASURED r35/r36 and named in MakeupApi: the engine's own race
            // change paints the body from the OLD list's skin tone slot and then
            // skee's preset apply installs a whole tint list of its own, so the
            // face wears the look's colour while the body keeps the race it came
            // from. This reads the live list's skin tone layer, whichever of the
            // two lists is live, and hands it to the same SetSkinFromTint the
            // engine uses. A look with no face block never moves the list, so
            // nothing is owed there.
            //
            // ⚠⚠ AND THE CAPTURE'S OWN SKIN TONE IS THE SOURCE WHEN THERE IS
            // ONE, not the live list. MEASURED r37 on the reference rig: after a
            // look switches the Nord to a 108 slot race the live list is STILL
            // the Nord's 34, no overlay list exists at any rung, and the face is
            // right anyway because skee's preset apply paints it from the
            // preset's own baked tint texture. So the list has no slot the new
            // skin tone belongs in, and reading it painted the body the OLD
            // race's white: the pale body under an Umbrael head. The look's
            // captured skintone layer is the colour the face was authored with,
            // and SetSkinFromTint needs nothing but that colour and its alpha.
            const MakeupPlan::LayerState* capturedSkin = nullptr;
            if (a_profile.makeup) {
                for (const auto& entry : *a_profile.makeup) {
                    if (entry.type ==
                        static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                        capturedSkin = &entry.state;
                        break;
                    }
                }
            }
            if (capturedSkin && (a_take.makeup || a_take.face)) {
                MakeupApi::PaintBodyFromTint(player, capturedSkin->tint,
                                             capturedSkin->strength);
            } else if (a_profile.face) {
                MakeupApi::SyncBodyToSkinTone(player);
            }
            OutfitSession::RequestRefresh();
            // ⚠⚠ AND THE EDITOR STAYS OPEN, WHICH THE USER ASKED FOR BY NAME.
            // The apply used to close it at the press, because the face step
            // rebuilds the head and the outfit step activates a DIFFERENT
            // library entry under the editor's staged copy and the staging
            // machinery has no seam for that. The seam it does have is OnOpen,
            // so the apply re-stages in place at its settle rather than handing
            // the player back to the game. AFTER the refresh above, in the same
            // drain, so the pane it re-reads is the settled character; a no-op
            // while the editor is closed.
            // ⚠⚠ AND WHICH OF THE TWO ENDINGS THE PLAYER GETS IS A SETTING,
            // defaulting to the old one. Staying in the editor is what the user
            // asked for and the re-stage does put the pane back. It was
            // defaulted off when r40 reported the body's SMP and CBPC dead
            // after an apply and this was the change that had arrived with it;
            // r41 paired it properly and cleared it, because the physics dies
            // on a look that SWITCHES RACE and is fine on a same-race look with
            // this behaving identically either way. Conservative default until
            // that hunt closes; bStayInEditorAfterApply asks for the other.
            if (Settings::GetSingleton().stayInEditorAfterApply) {
                EditorWindow::RequestRestage();
            } else {
                EditorWindow::RequestClose();
            }
            // In-flight ends HERE, after the settle refresh is queued: the
            // refresh itself then runs with the gate open, and any close-path
            // re-assert from now on reads a settled character.
            SetInFlight(false);
            // The character step ran, so the race or sex may have changed
            // under CBPC's per-actor bone list, which is built once and never
            // rebuilt by a sex flip (r50, CBPC's own log). Ask CBPC to
            // rebuild it through its own Papyrus API once the storm settles.
            if (a_take.character) {
                CbpcRefresh::QueueAfterSwitch();
            }
            spdlog::info("ProfileApply: settle refresh queued (the editor-open "
                         "pass).");
            // ⚠⚠ THE FACE WATCH HAS TO BE TOLD, BECAUSE IT CANNOT WORK THIS OUT.
            // A look the user just imported and a preset skee re-bound behind our
            // back are the same named texture in the same slot, so the repair
            // fired on the face the apply had only just put there. The makeup
            // step already stands aside for exactly this reason; the watch had no
            // way to hear about it. Quiet rather than disarmed, because the head
            // keeps rebuilding for seconds after this line.
            //
            // ⚠ AND THE HOLD GOES WITH IT. A held tone is a standing
            // instruction to put OUR composite back on every rebuild, so leaving
            // it set would take the imported face off again at the first head
            // build past the quiet period. The look owns this character's face
            // now; a tone the user wants on top of it is a fresh pick.
            if (a_take.face && a_profile.face.has_value()) {
                MakeupApi::StandDownFaceWatch(MakeupPlan::kFaceWatchQuietAfterApplyMs);
                if (MakeupApi::HoldsSkinTone()) {
                    MakeupApi::ReleaseSkinTone();
                    spdlog::info("ProfileApply: the held skin tone is released, "
                                 "because this look brought its own face and a "
                                 "hold would put ours back over it.");
                }
            }
            // ⚠⚠ AND WHAT THE LOOK DOES NOT CARRY COMES OFF, IF THE PLAYER
            // SAID SO. Every checkbox on the Looks page is AND'd with the blocks
            // the file actually holds, so a look naming no overlays has no box
            // to tick and the previous character's layers simply stayed on.
            // Field 2026-08-27: importing 'Nord 3' over Umbrael left Umbrael
            // wearing her own overlays under somebody else's face.
            //
            // ⚠ ONLY WHERE A CLEAR MEANS SOMETHING. Overlays, the Shape page's
            // morphs and its bone scales, and the skin pack all have an empty
            // state that is a real answer. A makeup capture does NOT: the live
            // tint list belongs to the race and emptying it is not the same
            // statement as a look having no makeup, so it is left alone. Nor do
            // a body, a weight or an outfit, which have no off.
            if (Settings::GetSingleton().replaceOnLookApply) {
                const auto present = ProfilePlan::ParticipationFor(a_profile);
                std::size_t wiped  = 0;
                if (!present.overlays && OverlayApi::Available()) {
                    for (const auto& layer : OverlayApi::Layers()) {
                        if (OverlayPlan::Occupied(OverlayApi::Read(player, layer.node))) {
                            OverlayApi::Clear(player, layer.node);
                            ++wiped;
                        }
                    }
                }
                if (!present.shape) {
                    RaceMenuMorphApi::ClearShape(player);
                    if (NodeTransformApi::Available()) {
                        NodeTransformApi::ClearOwned(player);
                    }
                }
                if (!present.skin) {
                    SkinApi::Apply(player, std::string{});
                }
                spdlog::info(
                    "ProfileApply: replace-on-apply took off what this look does "
                    "not carry ({} overlay layer(s){}{}). Makeup, body, weight "
                    "and outfit have no empty state and are left alone.",
                    wiped, present.shape ? "" : ", the shape",
                    present.skin ? "" : ", the skin pack");
            }
            spdlog::info("ProfileApply: '{}' complete.", a_profile.name);
        }

        class LoadAnswer : public RE::BSScript::IStackCallbackFunctor {
        public:
            LoadAnswer(std::vector<ProfilePlan::Step> a_steps,
                       ProfileCodec::Profile a_profile,
                       ProfilePlan::Participation a_take,
                       RE::TESRace* a_owedOnFailure,
                       std::vector<RE::BGSHeadPart*> a_preSwitchParts)
                : steps_(std::move(a_steps)), profile_(std::move(a_profile)),
                  take_(a_take), owedOnFailure_(a_owedOnFailure),
                  preSwitchParts_(std::move(a_preSwitchParts)) {}

            void operator()(RE::BSScript::Variable a_result) override {
                const bool ok = a_result.IsBool() && a_result.GetBool();
                spdlog::info("ProfileApply: step 'face' answered {}.",
                             ok ? "true" : "FALSE (jslot missing or refused; "
                                           "the siblings still apply)");
                if (ok) {
                    // A true answer is the one moment skee is known to have
                    // mapped the preset (AssignMappedPreset sits inside the
                    // natives, before their ApplyPreset). The load boundary
                    // reads this to know residue exists to erase.
                    g_faceAppliedThisSession.store(true);
                }
                // One task drain between the answer and the siblings: the
                // head build the dispatch started lands first, which is the
                // same settling the reconcile trusts.
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([steps = std::move(steps_),
                                    profile = std::move(profile_),
                                    take = take_, ok,
                                    owed = owedOnFailure_,
                                    preSwitch = std::move(preSwitchParts_)]() mutable {
                        // A false answer means skee returned before its
                        // SetRace (the jslot failed to load), so the switch
                        // the character step left riding this dispatch is
                        // still owed; the sex flag is already flipped and
                        // this rebuild is what makes it visible.
                        if (!ok && owed) {
                            if (auto* player =
                                    RE::PlayerCharacter::GetSingleton()) {
                                spdlog::info(
                                    "ProfileApply: the face carried the race "
                                    "switch and failed; SwitchRace to '{}' "
                                    "runs by itself.",
                                    owed->GetFormEditorID());
                                player->SwitchRace(owed, true);
                            }
                        }
                        // The head reconcile: `owed` is non-null exactly
                        // when a switch rode this dispatch, and success
                        // needs it as much as failure does. The one-drain
                        // settle above let skee's own head build land;
                        // this swaps the live node onto the new parts and
                        // rebakes (the counts in its log line say what it
                        // found to do).
                        if (owed) {
                            if (auto* player =
                                    RE::PlayerCharacter::GetSingleton()) {
                                ReconcileSwitchedHead(player, preSwitch);
                            }
                        } else if (ok) {
                            // No switch rode this dispatch, so no engine
                            // rebuild is coming and skee's morphs just
                            // landed `+=` on the live head. Regenerate
                            // (function scar carries the r63 measurement).
                            if (auto* player =
                                    RE::PlayerCharacter::GetSingleton()) {
                                RegenerateAppliedHead(player);
                            }
                        }
                        // ⚠⚠ THE DEFAULT WAS SET BEFORE THE DISPATCH AND NOTHING
                        // PUSHED IT. skee's rebuild paints the hair from the
                        // actor base, the base still carries the PREVIOUS
                        // look's colour, and the character kept it until
                        // something else happened to run the reassert. The
                        // field report named the mechanism exactly: the colour
                        // was wrong after every apply and correct the moment
                        // the editor opened, because the editor's refresh is a
                        // reassert and the apply was not.
                        //
                        // ⚠ HERE AND NOT EARLIER, for two reasons. The head build
                        // this dispatch started has landed by now (the one task
                        // drain above is the same settling the reconcile
                        // trusts), so this is not a write the rebuild is about
                        // to paint over. And the load-time trap that guards the
                        // kPostLoadGame call site does not apply: that one is
                        // about 'ACTV' arriving before 'HCOL' while the co-save
                        // is still being read, and an apply during play is long
                        // past it.
                        //
                        // ⚠ NOT A SECOND PAINTER. This is the reassert that
                        // already owns the colour, running at the moment it is
                        // owed instead of only at a load boundary or an editor
                        // open. The ladder still decides: an outfit that names
                        // a colour outranks the look's default, and a sibling
                        // outfit step running below pushes again for itself.
                        if (ok) {
                            OutfitSession::GetSingleton()
                                .ReassertPlayerHairColor();
                        }
                        RunSteps(std::move(steps), std::move(profile), take);
                    });
                }
            }
            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::vector<ProfilePlan::Step> steps_;
            ProfileCodec::Profile          profile_;
            ProfilePlan::Participation     take_;
            RE::TESRace*                   owedOnFailure_;
            // The pre-switch part list for the head reconcile: raw pointers
            // are safe here because head parts are forms and the answer
            // arrives within the same session.
            std::vector<RE::BGSHeadPart*>  preSwitchParts_;
        };

        // The facegen model-cache eviction, the shelved r65 lever landing at
        // the site the r72 pointer trace picked. Both r72 applies rebuilt the
        // head with FRESH buffers through IDENTICAL pass sequences, and one
        // came out canonical while the other came out doubled: the only
        // varying input is the CLONE SOURCE - the cached BSFaceGenModel the
        // fresh FOD is born from (docs/re/facegen-model-cache.md carries the
        // anatomy: map keyed by the part's model NIF path, nothing evicts at
        // runtime, the FOD ctor extracts the clone's current content as its
        // base). Session history accumulates in those cached models through a
        // writer no census has caught; evicting the player's entries right
        // before a face apply makes the rebuild cache-miss and clone from
        // PRISTINE disk models, so the apply's outcome stops depending on
        // what the session did before it. Entries are NiPointer-held: erasing
        // drops the cache reference, in-flight users keep theirs, and other
        // actors sharing a path just reload it once.
        void EvictPlayerHeadModelCache(RE::TESNPC* a_npc) {
            auto* const mgr = RE::BSFaceGenManager::GetSingleton();
            if (!a_npc || !mgr) {
                return;
            }
            std::uint32_t evicted = 0;
            {
                RE::BSWriteLockGuard lock{ mgr->modelMap.lock };
                const auto evict = [&](RE::BGSHeadPart* a_part) {
                    if (!a_part) {
                        return;
                    }
                    if (const auto& path = a_part->model; !path.empty()) {
                        evicted += static_cast<std::uint32_t>(
                            mgr->modelMap.map.erase(path));
                    }
                    for (auto* const extra : a_part->extraParts) {
                        if (extra && !extra->model.empty()) {
                            evicted += static_cast<std::uint32_t>(
                                mgr->modelMap.map.erase(extra->model));
                        }
                    }
                };
                for (std::uint32_t i = 0; i < a_npc->numHeadParts; ++i) {
                    evict(a_npc->headParts[i]);
                }
            }
            spdlog::info("ProfileApply: step 'face' evicted {} cached head "
                         "model(s); the rebuild clones from disk.",
                         evicted);
        }

        // The apply-time erase's chain link: whatever ClearPreset answered,
        // run the face dispatch next, on the main thread. r71 measured the
        // reason this exists: an apply that starts with a LIVE mapped preset
        // (left by a previous apply, surviving any number of loads) lands the
        // face DOUBLED (1489620 twice in one round), while the same look on a
        // map-free session applies clean (1487849). Mid-session, before
        // LoadCharacterEx, is the ONE erase site r63-r70 left standing: the
        // VM is calm and no load-window rebuild chain exists to make the
        // erase inconsistent.
        class ChainLink : public RE::BSScript::IStackCallbackFunctor {
        public:
            ChainLink(const char* a_what, std::function<void()> a_next)
                : what_(a_what), next_(std::move(a_next)) {}

            void operator()(RE::BSScript::Variable a_result) override {
                const bool yes = a_result.IsBool() && a_result.GetBool();
                spdlog::info("ProfileApply: step 'face' {} ({}).", what_,
                             yes ? "true" : "false");
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([fn = std::move(next_)] { fn(); });
                } else {
                    next_();
                }
            }
            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            const char*           what_;
            std::function<void()> next_;
        };

    }  // namespace

    void Apply(RE::Actor* a_player, const ProfileCodec::Profile& a_profile,
               const ProfilePlan::Participation& a_boxes) {
        if (!a_player) return;
        if (!RequiresSatisfied(a_profile)) return;

        auto take = ProfilePlan::And(
            ProfilePlan::ParticipationFor(a_profile), a_boxes);

        // ⚠⚠ THE RACEMENU HALF OF A LOOK IS OPT IN AS OF 2026-08-29. Two
        // players reported a broken face on 1.1.2 within one night, one of them
        // a second face over the first, and both rolled back. `character` is
        // the race and sex switch and `face` is the LoadCharacterEx dispatch;
        // between them they are everything a look does through RaceMenu, and
        // the rest of a look (outfit, dyes, body, shape, skin, overlays) does
        // not go near it.
        //
        // ⚠ HERE, NOT AT THE CALL SITES. Every route into a look apply has to
        // be covered, and a caller added later must not have to remember this.
        // Dropping the boxes is also the posture the plan already has for a
        // block whose mod is missing, so nothing downstream needs a new case.
        if (!Settings::GetSingleton().looksRaceMenu &&
            (take.character || take.face)) {
            spdlog::info("ProfileApply: the character and face steps are off "
                         "(bLooksRaceMenu=0), so this look applies everything "
                         "except its RaceMenu preset, race and sex.");
            take.character = false;
            take.face      = false;
        }

        // The character decision, made once, before the plan is built: an
        // unresolvable race drops the block (the posture every missing-mod
        // block has), and everything downstream reads THIS decision rather
        // than deriving its own.
        SwitchDecision decision;
        if (a_profile.character) {
            auto* const base      = a_player->GetActorBase();
            const bool  femaleNow = base && base->IsFemale();
            auto* const raceNow   = a_player->GetRace();
            StyleRefKey current;
            const bool  haveKey = raceNow && StyleRef::Make(raceNow, current);
            const bool  raceDiffers =
                haveKey && !(current == a_profile.character->race);
            const bool sexDiffers =
                a_profile.character->female != femaleNow;
            if (take.character) {
                if (raceDiffers) {
                    auto* const dh = RE::TESDataHandler::GetSingleton();
                    auto* const target =
                        dh ? dh->LookupForm<RE::TESRace>(
                                 a_profile.character->race.localFormID,
                                 a_profile.character->race.modName)
                           : nullptr;
                    if (!target) {
                        spdlog::warn(
                            "ProfileApply: '{}' was captured on {}|{:06X} "
                            "and that race no longer resolves; the character "
                            "block is dropped and this character stays as "
                            "they are.",
                            a_profile.name, a_profile.character->race.modName,
                            a_profile.character->race.localFormID);
                        take.character = false;
                    } else {
                        decision.targetRace = target;
                    }
                }
                if (take.character) {
                    decision.flipFemale = sexDiffers;
                    decision.female     = a_profile.character->female;
                    decision.differs    = raceDiffers || sexDiffers;
                }
            } else if (raceDiffers || sexDiffers) {
                spdlog::warn(
                    "ProfileApply: '{}' was captured on {}|{:06X} ({}); this "
                    "character is {}|{:06X} ({}). The Character box is off, "
                    "so race and sex stay; the other blocks still apply.",
                    a_profile.name, a_profile.character->race.modName,
                    a_profile.character->race.localFormID,
                    a_profile.character->female ? "female" : "male",
                    haveKey ? current.modName : "?",
                    haveKey ? current.localFormID : 0u,
                    femaleNow ? "female" : "male");
            }
        }

        auto steps = ProfilePlan::Build(take);
        if (steps.empty()) {
            spdlog::info("ProfileApply: '{}' has nothing to apply.",
                         a_profile.name);
            return;
        }
        spdlog::info("ProfileApply: '{}' begins, {} step(s).", a_profile.name,
                     steps.size());
        // From here to the settle refresh, the editor-close re-assert stands
        // down (see InFlight in the header). Set before returning to the
        // caller, whose next act is closing the editor.
        SetInFlight(true);
        // And the orphan-blank pass stands down from the FIRST step to the
        // next load, not from the finish: the face step's install arms a
        // 0.15 s check that can land mid-apply, in the window where skee's
        // store already reads as the preset's (possibly empty) and the clones
        // still wear the outgoing art.
        OverlayReconcile::NoteProfileApply();
        // The apply owns the face now: the load's rebake debt AND its late
        // window are void, or the window's residue check would wipe the
        // preset face this apply is about to bind (r42 applied 28 s after a
        // load, inside the window).
        MakeupApi::CancelFaceRebake();

        // The character and face steps run at the head of one game task; the
        // siblings drain after (through the face's answer when it runs). The
        // caller's thread does no engine mutation and no VM dispatch.
        const bool character =
            !steps.empty() && steps.front() == ProfilePlan::Step::kCharacter;
        if (character) steps.erase(steps.begin());
        const bool face =
            !steps.empty() && steps.front() == ProfilePlan::Step::kFace;
        if (face) steps.erase(steps.begin());
        // Which native the face rides picks who does the race switch:
        // LoadCharacterEx (Exported) runs skee's SetRace itself, so the race
        // rides the dispatch; LoadCharacterPresetEx (Presets) has NO race
        // parameter and never touches the race, so a character block beside
        // a preset face runs its own SwitchRace exactly as the no-face path
        // does.
        const bool faceCarries =
            face && a_profile.face &&
            a_profile.face->folder == ProfileCodec::FaceFolder::kExported;

        auto work = [steps = std::move(steps), profile = a_profile, take,
                     decision, character, face, faceCarries]() mutable {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) {
                spdlog::warn("ProfileApply: the player unloaded before the "
                             "apply began; every step is dropped.");
                SetInFlight(false);
                return;
            }
            // The pre-switch part list, taken BEFORE anything writes the
            // base: it is the "old" half the head reconcile needs to know
            // which geometry to detach from the live face node. skee's
            // SetRace (face path) and StepCharacter's SwitchRace (no-face
            // path) both replace the base's list with the new race's, so
            // after either runs there is no record of what the node wears.
            std::vector<RE::BGSHeadPart*> preSwitch;
            if (character && decision.differs) {
                if (auto* const base = player->GetActorBase()) {
                    for (std::int8_t i = 0; i < base->numHeadParts; ++i) {
                        if (base->headParts[i]) {
                            preSwitch.push_back(base->headParts[i]);
                        }
                    }
                }
            }
            // What RunSteps reads to decide the tint list rebuild: this apply
            // moves the race or the sex. Set here, after the unload check, so
            // a dropped apply never leaves it for the next one.
            g_switchedThisApply.store(character && decision.differs,
                                      std::memory_order_release);
            if (character) {
                StepCharacter(player, decision, faceCarries);
            }
            // What the face dispatch owes back if it fails before skee's own
            // SetRace runs: the rebuild that makes the character step's work
            // visible. Null when nothing differs or no face carries it (a
            // preset-folder face never does).
            RE::TESRace* const owed =
                (character && decision.differs && faceCarries)
                    ? (decision.targetRace ? decision.targetRace
                                           : player->GetRace())
                    : nullptr;
            if (!face) {
                // The head reconcile, the no-face half: StepCharacter's own
                // SwitchRace has replaced the base's parts (ChangeRace
                // defaults them from the new race synchronously), so the
                // swap has both halves in hand right here. Also the whole
                // cure for the sex flip on the SAME race, whose parts no
                // reload touches while the flag alone repaints nothing.
                if (character && decision.differs) {
                    ReconcileSwitchedHead(player, preSwitch);
                }
                RunSteps(std::move(steps), std::move(profile), take);
                return;
            }

            // A preset-folder face beside a differing character block: the
            // switch just ran by itself (StepCharacter's SwitchRace), so the
            // head reconcile has both halves in hand now, before the preset
            // dispatch builds on top of the switched base.
            if (face && !faceCarries && character && decision.differs) {
                ReconcileSwitchedHead(player, preSwitch);
            }

            const auto siblings = steps;  // for the refused path; cb owns `steps`
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                spdlog::warn("ProfileApply: no Papyrus VM, the face block is "
                             "dropped and the siblings apply.");
                if (owed) {
                    spdlog::info("ProfileApply: the face carried the race "
                                 "switch; SwitchRace to '{}' runs by itself.",
                                 owed->GetFormEditorID());
                    player->SwitchRace(owed, true);
                }
                RunSteps(std::move(steps), std::move(profile), take);
                return;
            }
            // The race handed to skee is the decision's: its ApplyPreset
            // calls SetRace with exactly this argument, which is the switch.
            auto* const raceArg =
                decision.targetRace ? decision.targetRace : player->GetRace();
            const bool presetFolder = !faceCarries;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new LoadAnswer(std::move(steps), profile, take, owed,
                               std::move(preSwitch))
            };
            // Whether this is the FIRST face apply since a load boundary.
            // r74's bracket proved the post-load first apply is the one that
            // lands wrong (skee's sculpt maps key StringTableItems with text
            // hash but shared_ptr POINTER equality, and every load re-mints
            // the table, so cross-epoch content is lookup-invisible yet
            // iteration-alive - the capture round SHOWED the holes). The
            // second apply in one epoch measured canonical five rounds
            // straight, so the first one after a load runs the dispatch
            // TWICE: the second pass reads and writes one consistent epoch.
            // ⚠⚠ THE LOOK'S HAIR COLOUR BECOMES THIS CHARACTER'S DEFAULT, for
            // the same reason the body does one step above: a look states who
            // the character is, and the reassert ladder already reads the
            // default as its middle rung. Without this the ladder had nothing
            // to fall back on and said so on every load - "this character has
            // no default hair colour, so there is nothing to put back" - while
            // skee painted the jslot's absent key as black over the top.
            //
            // ⚠ SET BEFORE THE DISPATCH. LoadCharacterEx rebuilds the head, and
            // the rebuild is what the reassert is racing; a default written
            // afterwards would arrive to a head already painted.
            if (profile.face->hairColour && profile.face->hairColour->set) {
                const auto& t = *profile.face->hairColour;
                DefaultLook::SetHairColour(player, t);
                spdlog::info("ProfileApply: step 'face' set the character "
                             "default hair colour to ({},{},{}) from the "
                             "look, so the reassert has something to put back.",
                             t.r, t.g, t.b);
            }
            // ⚠⚠ AND THE CHARACTER'S PINNED PARTS COME OFF, WHICH IS THE OTHER
            // HALF OF THE SAME SENTENCE. A default is what this character wears
            // whenever the outfit names nothing, so the outfit step's look push
            // resolves straight to it, and a look that brought its own face was
            // being handed the previous character's hair, eyes, brows and horns
            // one line later. Field 2026-08-27, importing 'Almalexia' onto
            // Umbrael, the head-watch either side of the outfit step:
            //
            //   t+26779  ... KSSMP_SweetEscape, 00UBE_FemaleEyesAmber, 00UBE_FemaleBrows_007
            //   t+27779  ... KSSMP_Kaysa_Elf, glowUBEwbDemonEye01, D_EDHornB
            //
            // The face was Almalexia's for exactly one step. The user's reading
            // of it was the right one: importing a character is meant to reset
            // the pinned defaults.
            //
            // ⚠ THE HAIR COLOUR DEFAULT SURVIVES, and that is not an
            // inconsistency: the block above has just SET it from this look, so
            // it already belongs to the incoming character rather than the
            // outgoing one. Clearing styles never touches it.
            //
            // ⚠ BEFORE THE DISPATCH, like everything else here. A default
            // cleared after the outfit step has already resolved against it
            // arrives to a head wearing the parts it was meant to prevent.
            {
                const auto  pinned  = DefaultLook::For(player);
                std::size_t dropped = 0;
                for (const auto part :
                     { DefaultLook::Part::kHair, DefaultLook::Part::kEyes,
                       DefaultLook::Part::kBrows, DefaultLook::Part::kFacialHair }) {
                    if (DefaultLook::Has(player, part)) {
                        DefaultLook::Clear(player, part);
                        ++dropped;
                    }
                }
                // ⚠ OVER A COPY, because ClearSlot mutates the list this would
                // otherwise be walking.
                for (const auto& pin : pinned.customHeadParts) {
                    DefaultLook::ClearSlot(player, pin.slot);
                    ++dropped;
                }
                if (dropped != 0) {
                    spdlog::info(
                        "ProfileApply: step 'face' dropped {} pinned default "
                        "part(s) ({} invented slot(s) among them), because this "
                        "look brings its own head and a pin would put the "
                        "previous character's back one step later. The default "
                        "hair COLOUR stays, this look just set it.",
                        dropped, pinned.customHeadParts.size());
                }
            }
            // ⚠⚠ AND THE OVERLAY RECORD IS A PIN OF THE SAME KIND, WHICH IS
            // THE 2026-08-27 FIELD REPORT. OverlayBaseline holds the art this
            // mod last painted so an empty store after a load can have it back.
            // Nothing told it a look import had replaced the character, so it
            // put the OUTGOING one's face overlays onto the incoming face: a
            // male Nord in Umbrael's '(SDZ21) Fabulous Makeup 2', which is what
            // arrived as "the previous makeup stays on".
            //
            // ⚠⚠ AND IT HAS TO HAPPEN HERE RATHER THAN AT THE SETTLE, because
            // a store read at the settle can already be empty. A look that
            // switches race empties skee's store before the replace pass runs,
            // so that pass found nothing to clear on the very apply the field
            // reported (02:27:50.434, `0 overlay layer(s)`) and the record put
            // nine layers back 196 ms later. The record survives an empty
            // store, so dropping it is the only move that works on both. The
            // overlay step runs after this one and re-records whatever the
            // incoming look writes.
            //
            // ⚠ THE SWITCH GATES IT, unlike the pins above. With replace off, a
            // look naming no overlays is meant to leave the previous
            // character's on, and across a race switch this record is the only
            // thing that can put them back.
            if (Settings::GetSingleton().replaceOnLookApply &&
                OverlayBaseline::HasAny()) {
                OverlayBaseline::Clear();
                spdlog::info(
                    "ProfileApply: step 'face' dropped the overlay record, "
                    "because this look replaces the character and the record "
                    "would put the previous one's layers back the next time "
                    "skee's store came up empty.");
            }
            // ⚠⚠ AND THE TINT-LIST RECORD IS THE SAME PIN ONE CONTAINER DOWN,
            // which is the 2026-09-02 Nord 3 field report. A race-switching
            // apply refuses both the makeup write and the batch-end sync (the
            // r35 shape, and correctly), so nothing between here and the save
            // can teach the record that the character it describes is gone:
            // Umbrael's fourteen entries crossed the save and the post-load
            // converge painted them onto Nord 3. Same cure as the overlay
            // record above, same timing argument: the makeup step runs after
            // this one and the batch-end sync re-records whatever it writes,
            // so a same-race apply repopulates the record milliseconds later,
            // and a race-switching one leaves it honestly empty until a list
            // this race owns can be captured.
            // ⚠ DROPPED TO A BARE CLAIM, NOT TO NOTHING. Field 2026-09-02
            // 02:40: a record dropped to nothing stayed nothing when the
            // makeup step then refused (r35), the save wrote no record, and
            // an older save's makeup walked into the new one. See
            // MakeupBaseline::Recorded.
            if (Settings::GetSingleton().replaceOnLookApply) {
                const bool had = MakeupBaseline::HasAny();
                static const std::vector<ProfileCodec::MakeupEntry> kNoMakeup;
                const auto& block = profile.makeup ? *profile.makeup : kNoMakeup;
                MakeupBaseline::Adopt(block);
                spdlog::info(
                    "ProfileApply: step 'face' {} the tint-list record with this "
                    "look's own makeup block ({} entr{}), because this look "
                    "replaces the character and the record would re-assert the "
                    "previous one's makeup after the next load.",
                    had ? "replaced" : "seeded", block.size(),
                    block.size() == 1 ? "y" : "ies");
            }
            // ⚠⚠ HELD BEFORE THE DISPATCH, for the reason the default hair
            // colour above is set before it: LoadCharacterEx rebuilds the head,
            // HeadBuildHook binds the export on the far side of that rebuild,
            // and a hold written afterwards would arrive one build too late.
            // See LookFaceTint.h.
            LookFaceTint::Hold(profile.face->jslot);
            const bool doubleTap =
                !g_faceAppliedThisSession.load(std::memory_order_acquire);
            const char*                          native = nullptr;
            RE::BSScript::IFunctionArguments*    args   = nullptr;
            if (presetFolder) {
                // ⚠ The hair colour parameter is deliberately None: skee
                // writes the preset's colour INTO the form it is handed
                // (PapyrusCharGen.cpp:295-301, read 2026-08-22), and there
                // is no scratch BGSColorForm to sacrifice - passing a real
                // one would mutate a shared form. The preset's hair colour
                // is skipped; the log says so once per load.
                native = "LoadCharacterPresetEx";
                spdlog::info("ProfileApply: step 'face' (LoadCharacterPresetEx "
                             "'{}' flags={}; the preset's hair colour is "
                             "skipped, no scratch colour form exists).",
                             profile.face->jslot, profile.face->flags);
                args = RE::MakeFunctionArguments(
                    static_cast<RE::Actor*>(player),
                    RE::BSFixedString(profile.face->jslot),
                    static_cast<RE::BGSColorForm*>(nullptr),
                    static_cast<std::int32_t>(profile.face->flags));
            } else {
                native = "LoadCharacterEx";
                spdlog::info("ProfileApply: step 'face' (LoadCharacterEx '{}' "
                             "race '{}' flags={}).",
                             profile.face->jslot,
                             raceArg ? raceArg->GetFormEditorID() : "?",
                             profile.face->flags);
                args = RE::MakeFunctionArguments(
                    static_cast<RE::Actor*>(player),
                    static_cast<RE::TESRace*>(raceArg),
                    RE::BSFixedString(profile.face->jslot),
                    static_cast<std::int32_t>(profile.face->flags));
            }
            // The final face dispatch, carrying the real answer callback.
            auto dispatchFace = [vm, native, args, cb, owed, siblings,
                                 profile, take, player]() mutable {
                if (!VmCall::Static(vm, "CharGen", native, args, cb)) {
                    spdlog::warn("ProfileApply: {} refused; the face "
                                 "block is dropped and the siblings apply.",
                                 native);
                    if (owed) {
                        spdlog::info("ProfileApply: the face carried the race "
                                     "switch; SwitchRace to '{}' runs by "
                                     "itself.",
                                     owed->GetFormEditorID());
                        player->SwitchRace(owed, true);
                    }
                    // The callback will not fire, so the siblings run here
                    // instead, with the caller's boxes intact.
                    RunSteps(siblings, std::move(profile), take);
                }
            };
            // The chain's entry: on the first face apply since a load, an
            // extra pass runs FIRST with a throwaway callback, and the final
            // dispatch above (reconcile, siblings and all) rides its answer.
            // The extra pass re-mints every skee-side key in the current
            // string-table epoch; the final pass then reads what it wrote.
            std::function<void()> entry = dispatchFace;
            if (doubleTap) {
                RE::BSScript::IFunctionArguments* firstArgs =
                    presetFolder
                        ? RE::MakeFunctionArguments(
                              static_cast<RE::Actor*>(player),
                              RE::BSFixedString(profile.face->jslot),
                              static_cast<RE::BGSColorForm*>(nullptr),
                              static_cast<std::int32_t>(profile.face->flags))
                        : RE::MakeFunctionArguments(
                              static_cast<RE::Actor*>(player),
                              static_cast<RE::TESRace*>(raceArg),
                              RE::BSFixedString(profile.face->jslot),
                              static_cast<std::int32_t>(profile.face->flags));
                entry = [vm, native, firstArgs, dispatchFace]() mutable {
                    spdlog::info("ProfileApply: step 'face' epoch-align pass "
                                 "(first face apply since a load).");
                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>
                        mid{ new ChainLink("epoch-align pass answered",
                                           dispatchFace) };
                    if (!VmCall::Static(vm, "CharGen", native, firstArgs,
                                        mid)) {
                        spdlog::warn("ProfileApply: the epoch-align pass "
                                     "refused; the final pass runs alone.");
                        dispatchFace();
                    }
                };
            }
            // ⚠⚠ THE STALE MAP DIES BEFORE THE FACE LANDS, AND ONLY HERE.
            // r71: an apply beginning with a live mapped preset doubles; the
            // erase chains so LoadCharacterEx cannot start until skee has
            // answered. If the erase dispatch itself refuses, the face
            // proceeds anyway - that is the pre-r62 behaviour, not a new
            // failure mode.
            auto* const npcBase = player->GetActorBase();
            if (!npcBase) {
                entry();
                return;
            }
            // The cache eviction first, synchronously: the rebuild this apply
            // starts must cache-miss (r72's verdict, scar on the function).
            EvictPlayerHeadModelCache(npcBase);
            // r73's bracket, the pre half: with the post half queued from the
            // whole-face detour, the pair says exactly which state skee's
            // pipeline STARTED from and what it produced.
            OS::SculptProbe::PulseHead("pre-face");
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> chain{
                new ChainLink("pre-erase answered", entry)
            };
            auto* const eraseArgs =
                RE::MakeFunctionArguments(static_cast<RE::TESNPC*>(npcBase));
            if (!VmCall::Static(vm, "CharGen", "ClearPreset", eraseArgs,
                                chain)) {
                spdlog::warn("ProfileApply: the pre-erase could not dispatch; "
                             "the face applies onto whatever map is live.");
                entry();
            }
        };
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(std::move(work));
        } else {
            // No task interface is a load-order accident this plugin has
            // never seen; inline is the pre-switch behaviour and only costs
            // the caller what it always paid.
            work();
        }
    }

    bool InFlight() {
        const auto until = g_inFlightUntilMs.load();
        return until != 0 && SteadyNowMs() < until;
    }

    bool SwitchSettling() {
        return g_baseWatchRunning.load(std::memory_order_acquire);
    }

    // ⚠⚠ THE ONE WINDOW UPSTREAM OF THE PAINT, AND IT IS A RACE WE HAVE TO
    // WIN EVERY TIME. See ProfileApply.h for the two rails.
    namespace {
        std::atomic<bool>          g_eraseOwed{ false };
        std::atomic<bool>          g_erasePending{ false };
        std::atomic<long long>     g_erasePendingUntilMs{ 0 };
        std::atomic<long long>     g_eraseDeadlineMs{ 0 };
        std::atomic<bool>          g_eraseRetryRunning{ false };
        std::atomic<std::uint32_t> g_eraseAttempts{ 0 };

        // One attempt. Returns false when there is nothing to attempt against,
        // which is a reason to keep waiting rather than to give up: the VM is
        // exactly what a load takes away and gives back.
        bool DispatchEraseOnce() {
            if (!g_eraseOwed.load(std::memory_order_acquire)) return false;
            // ⚠⚠ ONE OUTSTANDING CALL, EVER. A queued stack cannot be
            // recalled, so a burst issued at a stalled VM would all run
            // whenever it wakes - and anything running after the paint is
            // r63-r70's doubling window arriving by the back door.
            // ⚠⚠ ONE OUTSTANDING CALL, BUT ONLY WHILE IT CAN STILL ANSWER.
            // r92 shipped this rail without the second half and every retry
            // was blocked: a load eats the queued stack, the answer never
            // comes, `pending` never clears, and all four loads logged
            // "attempt 1" with nineteen seconds between them. Measured answer
            // latency is ~500 ms (481 ms r91, 503 ms r92), so a call with no
            // answer at 1200 ms is presumed eaten and one more may issue.
            //
            // ⚠ A PRESUMED-LOST CALL IS GENUINELY LOST, NOT DEFERRED. r92's two
            // unanswered dispatches doubled nothing, so the load discards
            // queued stacks rather than running them late. That is what makes
            // presuming safe; the attempt cap is the belt for the case where
            // it is not.
            if (g_erasePending.load(std::memory_order_acquire)) {
                if (SteadyNowMs() <
                    g_erasePendingUntilMs.load(std::memory_order_acquire)) {
                    return false;
                }
                spdlog::info("ProfileApply: pre-load erase attempt {} never "
                             "answered within 1200 ms; presuming the load ate "
                             "it and trying again.",
                             g_eraseAttempts.load(std::memory_order_relaxed));
                g_erasePending.store(false, std::memory_order_release);
            }
            if (g_eraseAttempts.load(std::memory_order_relaxed) >= 8) {
                return false;
            }
            g_erasePending.store(true, std::memory_order_release);
            g_erasePendingUntilMs.store(SteadyNowMs() + 1200,
                                        std::memory_order_release);
            auto* const player = RE::PlayerCharacter::GetSingleton();
            auto* const base   = player ? player->GetActorBase() : nullptr;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!base || !vm) {
                g_erasePending.store(false, std::memory_order_release);
                return false;
            }

            class Answer : public RE::BSScript::IStackCallbackFunctor {
            public:
                void operator()(RE::BSScript::Variable a_result) override {
                    const bool had = a_result.IsBool() && a_result.GetBool();
                    // Either answer retires the debt. "Dropped it" is the win;
                    // "held none" means there was nothing of the outgoing
                    // character left to bleed, which is the same clean state.
                    g_eraseOwed.store(false, std::memory_order_release);
                    g_erasePending.store(false, std::memory_order_release);
                    spdlog::info("ProfileApply: pre-load erase ANSWERED on "
                                 "attempt {}, skee {} a mapped preset for the "
                                 "player.",
                                 g_eraseAttempts.load(std::memory_order_relaxed),
                                 had ? "HAD and dropped" : "held no");
                }
                bool CanSave() const override { return false; }
                void SetObject(
                    const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
            };

            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> answer{
                new Answer()
            };
            auto* const args =
                RE::MakeFunctionArguments(static_cast<RE::TESNPC*>(base));
            const auto n =
                g_eraseAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!VmCall::Static(vm, "CharGen", "ClearPreset", args, answer)) {
                g_erasePending.store(false, std::memory_order_release);
                spdlog::warn("ProfileApply: pre-load erase attempt {} could not "
                             "dispatch; the retry keeps the debt open.",
                             n);
                return false;
            }
            spdlog::info("ProfileApply: pre-load erase DISPATCHED, attempt {}, "
                         "player base {:08X}@{}.",
                         n, base->GetFormID(), static_cast<const void*>(base));
            return true;
        }

        // ⚠ ONE MORE DETACHED TIMER, SHAPED LIKE DyeTick AND WorldWatch rather
        // than invented here: a short slice, and every piece of real work
        // marshalled onto the game thread through SKSE's task interface. It
        // exists because the VM this needs is the one the load is resetting,
        // so there is no single moment to ask at.
        void StartEraseRetry() {
            bool expected = false;
            if (!g_eraseRetryRunning.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return;
            }
            std::thread([] {
                while (g_eraseOwed.load(std::memory_order_acquire)) {
                    if (SteadyNowMs() >=
                        g_eraseDeadlineMs.load(std::memory_order_acquire)) {
                        g_eraseOwed.store(false, std::memory_order_release);
                        spdlog::warn("ProfileApply: pre-load erase gave up after "
                                     "{} attempt(s) over {} ms; skee keeps the "
                                     "outgoing character's preset for this load "
                                     "and the hair WILL bleed. Every attempt was "
                                     "queued and none came back, so the VM was "
                                     "down for the whole window rather than busy.",
                                     g_eraseAttempts.load(std::memory_order_relaxed),
                                     20000);
                        break;
                    }
                    // ⚠⚠ NO PENDING GUARD HERE, AND THAT IS THE WHOLE FIX. r93
                    // read `pending` in this loop AND again inside
                    // DispatchEraseOnce, and the two drifted in the gap: a
                    // stuck `pending` is precisely the state the retry exists
                    // to break, so a guard on it here meant the only code that
                    // could clear it never ran. Both r93 loads spent twenty
                    // seconds on "attempt 1". The slice belongs to this
                    // thread; the decision belongs to DispatchEraseOnce, which
                    // owns the pending flag, its 1200 ms presume-lost timeout
                    // and the attempt cap together.
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] { DispatchEraseOnce(); });
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                g_eraseRetryRunning.store(false, std::memory_order_release);
            }).detach();
        }
    }  // namespace

    void NotePlayerHairBuilt() {
        // ⚠⚠ THE HARD RAIL. Past this instant skee has already repainted out
        // of the map, so an erase can no longer save the hair and CAN still
        // manufacture the load-double that cost r63-r70 eight rounds. Whatever
        // is still owed is written off here, deliberately.
        if (g_eraseOwed.exchange(false, std::memory_order_acq_rel)) {
            spdlog::info("ProfileApply: pre-load erase DISARMED by the player's "
                         "hair build after {} attempt(s) with no answer. The "
                         "paint has happened; an erase from here is downstream "
                         "of it and is not worth the doubling risk.",
                         g_eraseAttempts.load(std::memory_order_relaxed));
        }
    }

    void ErasePresetAtPreLoad() {
        if (!OS::Settings::GetSingleton().preLoadPresetErase) {
            spdlog::info("ProfileApply: pre-load preset erase is OFF "
                         "([Compat] bPreLoadPresetErase), so skee keeps "
                         "whatever the outgoing character left mapped. This is "
                         "the control arm.");
            return;
        }
        g_eraseAttempts.store(0, std::memory_order_relaxed);
        g_erasePending.store(false, std::memory_order_release);
        g_erasePendingUntilMs.store(0, std::memory_order_release);
        g_eraseDeadlineMs.store(SteadyNowMs() + 20000, std::memory_order_release);
        g_eraseOwed.store(true, std::memory_order_release);
        // The first attempt is synchronous with the message, because the VM is
        // most likely to still be alive right here and a hit costs the retry
        // nothing.
        DispatchEraseOnce();
        StartEraseRetry();
    }

    void NoteLoadBoundary() {
        // The flag's meaning since r75: "a face applied in THIS string-table
        // epoch". Every load re-mints skee's string table, so the boundary
        // clears it and the next face apply runs the epoch-align double
        // pass.
        g_faceAppliedThisSession.store(false, std::memory_order_release);
    }

    // ⚠⚠ THE IN-LOAD MAPPED-PRESET ERASE IS GONE, AND NO TIMING BRINGS IT
    // BACK. Eight rounds (r63-r70, 2026-08-24) measured every variant: the
    // boundary dispatch, a +2 s delayed dispatch, and no dispatch at all.
    // Any in-load erase, at ANY timing, made the +5-7 s rebuild manufacture
    // the load-double (the cosave sculpt applied twice, 1488959, on FRESH
    // buffers, r70 pulse trace); with no erase the same recipe loads CLEAN
    // and the flush is single even with the map alive. The map's three
    // states at that rebuild are the whole story: alive = single, never
    // existed = single, erased mid-load = DOUBLE.
    //
    // The r62 bleed the erase existed for (a different-faced save wearing
    // the session's face through the surviving map) is BACK for now, as a
    // bounded transient: the late-rebake gate still covers its texture
    // half, and any face apply replaces the map. The lasting cure is an
    // erase OUTSIDE the load window - at the next apply, before
    // LoadCharacterEx, where the VM is calm and no engine rebuild chain is
    // in flight - and that design belongs to a fresh stint, not to the tail
    // of this one. g_faceAppliedThisSession stays maintained for it.

}  // namespace OS::ProfileApply
