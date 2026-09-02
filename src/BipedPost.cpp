#include "BipedPost.h"

#include "MeasuredGhosts.h"  // hand the shim what this sweep measured
#include "REAugments.h"
#include "Settings.h"
#include "SlotMask.h"
#include "VersionCheck.h"  // IdOk - membership, not "REL gave me an address"

#include <atomic>
#include <chrono>
#include <thread>

namespace OS::BipedPost {

    namespace {
        // BipedAnim / BIPOBJECT are now real, static_assert'd CommonLibSSE-NG
        // structs (RE/B/BipedAnim.h) whose layout matches the verified 1.5.97
        // disassembly exactly (objects[42] at biped+0x10, stride 0x78; BIPOBJECT
        // item@0x00, addon@0x08, part@0x10, partClone@0x20). Using the typed
        // members lets the header's static_asserts catch any future layout drift
        // at compile time - which raw offsets never would.

        // NiAVObject::flags bit 0 = kHidden. The Hide mechanism for attachment
        // slots culls the staged 3D with this flag (hide-mechanism-final.md,
        // Fallbacks #1: Equipment Toggle 2's technique); a rebuild re-clones
        // partClone without the flag, so un-hiding is automatic on the next
        // refresh. Accessed via CommonLib's GetFlags() accessor: the flags
        // member is at +0xF4 on SE 1.5.97 but +0x10C on AE 1.6.x
        // (RE::NiAVObject::GetFlags = RelocateMember(0x0F4, 0x10C)), so a raw
        // offset would clobber the wrong field on AE.

        RE::BipedAnim* CurrentPlayerBiped() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return nullptr;
            }
            return player->GetCurrentBiped().get();
        }
    }

    void RestoreRealItems(RE::BipedAnim* a_biped, std::uint32_t a_touchedMask,
                          RE::TESObjectARMO* const* a_realWorn, RE::TESObjectARMO* a_nakedSkin) {
        if (!a_touchedMask) {
            return;
        }
        if (!a_biped) {
            spdlog::debug("restore: no biped this pass.");
            return;
        }
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            if (!((a_touchedMask >> bit) & 1u)) {
                continue;
            }
            auto&       obj = a_biped->objects[bit];
            // Nothing staged at all: leave the empty slot alone (attachment
            // slots with no worn item and no injection).
            if (!obj.item && !obj.part) {
                continue;
            }
            // No real gear -> the engine's naked convention is the SKIN, never
            // null (the deferred 3D attach and other readers expect valid
            // fields). .addon is NEVER touched - see BipedPost.h.
            auto* const want = a_realWorn[bit] ? a_realWorn[bit] : a_nakedSkin;
            if (obj.item == want || !want) {
                continue;
            }
            spdlog::debug("restore: slot {} item {:08X} -> {} {:08X}", bit,
                          obj.item ? obj.item->GetFormID() : 0,
                          a_realWorn[bit] ? "real" : "skin", want->GetFormID());
            obj.item = want;  // .part / .partClone keep showing the outfit
        }
        if (Settings::GetSingleton().dumpBiped) {
            DumpBipedObjects("after-restore", a_biped);
        }
    }

    void CullObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask) {
        if (!a_biped || !a_objectMask) {
            return;
        }
        constexpr auto count = static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            if (!((a_objectMask >> slot) & 1ull)) {
                continue;
            }
            auto* const node = a_biped->objects[slot].partClone.get();
            if (!node) {
                continue;
            }
            if (!node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().set(RE::NiAVObject::Flag::kHidden);
                spdlog::debug("hide: culled node for biped object {}.", slot);
            }
        }
    }

    void QueueObjectNodeCull(RE::ActorHandle a_actor, std::uint64_t a_objectMask) {
        auto* task = SKSE::GetTaskInterface();
        if (!task || !a_objectMask) {
            return;
        }
        const auto mask   = a_objectMask;
        const auto handle = a_actor;
        task->AddTask([handle, mask] {
            // Re-resolve the biped at run time against the CAPTURED actor (not
            // the player): the pass's biped may have been torn down before the
            // task queue drains, and a fresh clone from an async model load
            // needs the cull re-applied. Skip silently if the actor has unloaded
            // between queue and drain - a stale handle resolves to null.
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (!actor) {
                return;
            }
            CullObjectNodes(actor->GetCurrentBiped().get(), mask);
        });
    }

    void ShowObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask) {
        if (!a_biped || !a_objectMask) {
            return;
        }
        constexpr auto count = static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            if (!((a_objectMask >> slot) & 1ull)) {
                continue;
            }
            if (auto* const node = a_biped->objects[slot].partClone.get();
                node && node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
                spdlog::debug("hide: restored node for biped object {}.", slot);
            }
        }
    }

    void QueueObjectNodeShow(RE::ActorHandle a_actor, std::uint64_t a_objectMask) {
        auto* task = SKSE::GetTaskInterface();
        if (!task || !a_objectMask) {
            return;
        }
        const auto mask   = a_objectMask;
        const auto handle = a_actor;
        task->AddTask([handle, mask] {
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (actor) {
                ShowObjectNodes(actor->GetCurrentBiped().get(), mask);
            }
        });
    }

    void CullNodes(RE::BipedAnim* a_biped, std::uint32_t a_hiddenAttachmentMask) {
        CullObjectNodes(a_biped, a_hiddenAttachmentMask);
    }

    void QueueNodeCull(RE::ActorHandle a_actor, std::uint32_t a_hiddenAttachmentMask) {
        QueueObjectNodeCull(a_actor, a_hiddenAttachmentMask);
    }

    void RestoreDismemberPartitions(RE::Actor* a_actor, std::uint32_t a_slots) {
        if (!a_actor || a_slots == 0) {
            return;
        }
        // ⚠ MEMBERSHIP FIRST, NOT "REL GAVE ME AN ADDRESS". CommonLib declares
        // UpdateDismemberPartion with RELOCATION_ID(15576, 15753) and calls it
        // unguarded, and id2offset does a lower_bound - so an id that is merely
        // ABSENT from this runtime's database resolves to its NEIGHBOUR and we
        // would be calling an unrelated engine function with a slot number and a
        // bool. Checked once and latched; a refusal leaves the old behaviour,
        // which is the ears staying hidden rather than anything worse.
        static const bool s_idOk = [] {
            constexpr REL::RelocationID kUpdate{ 15576, 15753 };
            if (OS::VersionCheck::IdOk(kUpdate)) {
                return true;
            }
            spdlog::error("dismember: UpdateDismemberPartion id {} is ABSENT from this "
                          "build's Address Library, so a hidden helmet will go on taking "
                          "the ear partition with it. Nothing else changes.",
                          kUpdate.id());
            return false;
        }();
        if (!s_idOk) {
            return;
        }

        auto* const root = a_actor->Get3D(false);
        if (!root) {
            return;
        }
        // ⚠ THE WHOLE ACTOR, NOT THE FACE NODE. RaceMenu stages overlay copies
        // of the head ('Face [Ovl0..2]' on the reporting rig) and each carries
        // its own skin instance with the same partitions, so repairing only the
        // head part would leave three copies of the ears disabled on top of it.
        // The narrowing that keeps this safe is the SLOT set, not the subtree:
        // a_slots is headgear-only, and a body skin carries no partition for
        // those body parts, so a traversal that reaches one changes nothing.
        std::uint32_t touched = 0;
        RE::BSVisit::TraverseScenegraphGeometries(
            root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                auto* const skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
                auto* const dis  = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin);
                if (!dis) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                const auto& rd = dis->GetRuntimeData();
                if (!rd.partitions || rd.numPartitions <= 0) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                // ⚠ ONLY SLOTS THIS SKIN ACTUALLY DECLARES. The engine's own
                // accessor is happy to be handed a body part the mesh does not
                // carry, and calling it anyway would be a write we cannot
                // describe. Asking the partition table first keeps every call
                // to something that exists.
                for (std::int32_t i = 0; i < rd.numPartitions; ++i) {
                    const auto slot = rd.partitions[i].slot;
                    if (slot < 30u || slot > 61u) {
                        continue;  // section caps and the like: not our space
                    }
                    const auto bit = static_cast<std::uint32_t>(slot - 30u);
                    if (((a_slots >> bit) & 1u) == 0) {
                        continue;
                    }
                    if (rd.partitions[i].editorVisible) {
                        continue;  // already on; nothing to put back
                    }
                    dis->UpdateDismemberPartion(slot, true);
                    touched |= (1u << bit);
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });

        if (touched) {
            spdlog::debug("dismember: re-enabled skin partition(s) 0x{:X} the hidden "
                          "headgear had suppressed (asked 0x{:X}).",
                          touched, a_slots);
        }
    }

    void QueueRestoreDismemberPartitions(RE::ActorHandle a_actor, std::uint32_t a_slots) {
        auto* task = SKSE::GetTaskInterface();
        if (!task || !a_slots) {
            return;
        }
        const auto slots  = a_slots;
        const auto handle = a_actor;
        task->AddTask([handle, slots] {
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (actor) {
                RestoreDismemberPartitions(actor, slots);
            }
        });
    }

    std::uint32_t RestoreHeadPartPartitions(RE::Actor* a_actor, const char* a_note) {
        if (!a_actor) {
            spdlog::info("dismember: [{}] head sweep: NO ACTOR.", a_note);
            return 0;
        }
        // Same membership guard as the body-slot restorer above, same reason.
        static const bool s_idOk = [] {
            constexpr REL::RelocationID kUpdate{ 15576, 15753 };
            if (OS::VersionCheck::IdOk(kUpdate)) {
                return true;
            }
            spdlog::error("dismember: UpdateDismemberPartion id {} ABSENT; the head "
                          "family stays as the engine left it.",
                          kUpdate.id());
            return false;
        }();
        if (!s_idOk) {
            return 0;
        }
        auto* const root = a_actor->Get3D(false);
        if (!root) {
            spdlog::info("dismember: [{}] head sweep: actor {:08X} has NO 3D.",
                         a_note, a_actor->GetFormID());
            return 0;
        }
        // Which head-family biped slots are LEGITIMATELY covered right now. A
        // partition slot in the head family is its biped slot plus 100
        // (131 hair <-> 31, 141 long hair <-> 41, 130 head <-> 30, 142
        // circlet <-> 42, 143 ears <-> 43), and a worn piece that declares
        // the biped slot has the RIGHT to keep that partition off.
        //
        // ⚠⚠ WORN IS NOT ENOUGH: THE PIECE MUST ALSO DRAW (round twenty). The
        // r20 field round measured the whole chain: the engine (one call
        // site, RVA 0x3C57D9) turns 131 off on every head build BECAUSE the
        // save really wears an item declaring biped 31 - and on the switched
        // custom race that item stages NO geometry (its ARMA has no addon
        // for the race, the engine-wears-male-model asymmetry's cousin), so
        // the player is bald behind an invisible occupant while a worn-only
        // gate calls the hide legitimate. A hide belongs to something that
        // draws: the gate honors a worn slot only when its biped object has
        // an attached, un-culled partClone. A worn-but-ghost slot is named
        // in the log with its form, so the field round can say what sits
        // there.
        std::uint32_t wornMask  = 0;
        std::uint32_t drawnMask = 0;
        // ⚠ WHO is on each tracked bit, not just that something is. A verdict
        // about a piece that renders nothing must not survive onto a different
        // piece swapped into the same slot, and MeasuredGhosts cannot tell the
        // two apart from a mask (MeasuredGhosts.h).
        std::uint32_t occupants[MeasuredGhosts::kTrackedBits]{};
        auto* const   biped = a_actor->GetCurrentBiped().get();
        for (std::uint32_t bit = 0; bit <= 13; ++bit) {
            const auto  slotMask = 1u << bit;
            auto* const armo     = a_actor->GetWornArmor(
                static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(slotMask));
            if (!armo) {
                continue;
            }
            wornMask |= slotMask;
            if (bit < MeasuredGhosts::kTrackedBits) {
                occupants[bit] = armo->GetFormID();
            }
            RE::NiAVObject* clone = biped ? biped->objects[bit].partClone.get()
                                          : nullptr;
            if (clone && !clone->GetAppCulled()) {
                drawnMask |= slotMask;
            } else {
                spdlog::info("dismember: [{}] slot {} worn by {:08X} '{}' but {} - a "
                             "ghost occupant does not own a hide.",
                             a_note, bit + 30, armo->GetFormID(), armo->GetName(),
                             clone ? "its clone is culled" : "it stages NO geometry");
            }
        }
        // ⚠⚠ HAND THE SHIM WHAT THIS SWEEP JUST MEASURED. Restoring a partition
        // a second after the engine turned it off wins the frame and loses the
        // war - the r28 round shows `re-enabled 0x2` at repaint, at settle+4s
        // and again at settle+10s, one bald flash each. The mask shim can end
        // it at the source, but only if it knows which occupants really draw,
        // and this walk is where that is known. Head-part bits only: they are
        // the two the engine's hide reads (MeasuredGhosts.h).
        //
        // ⚠⚠ AND THE RACE GOES WITH IT, because a look apply changes race twice
        // and the helmet that stages nothing here really draws over there. A
        // verdict measured on one race must not answer for the other; r30's
        // whole baldness was that honest vanilla-race reading zeroing the
        // custom race's clock (MeasuredGhosts.h).
        //
        // ⚠ THE WHOLE HEAD FAMILY GOES OVER, NOT JUST THE ENGINE'S TWO BITS.
        // MeasuredGhosts splits them: the shim still only ever sees
        // kEngineHideMask, and the Head page gets 41, 42 and 43 as well so a
        // row can say what sits on it. This static_assert is what keeps the
        // two definitions of "the bits 24220 reads" from drifting apart, since
        // MeasuredGhosts.h cannot include SlotMask.h.
        static_assert(MeasuredGhosts::kEngineHideMask == kHeadPartMask,
                      "MeasuredGhosts::kEngineHideMask must be SlotMask.h's "
                      "kHeadPartMask; the shim drops exactly what 24220 reads.");
        auto* const race   = a_actor->GetRace();
        const auto  raceID = race ? race->GetFormID() : 0u;
        MeasuredGhosts::Publish(
            a_actor->GetFormID(), raceID, drawnMask, occupants,
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        // The whole actor, not the face node, for the body-slot restorer's
        // own reason: overlay head copies carry the same partitions.
        std::uint32_t touched   = 0;
        std::uint32_t geoms     = 0;
        std::uint32_t skins     = 0;
        std::uint32_t headParts = 0;
        std::uint32_t offBefore = 0;
        RE::BSVisit::TraverseScenegraphGeometries(
            root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                ++geoms;
                auto* const skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
                auto* const dis  = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin);
                if (!dis) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                const auto& rd = dis->GetRuntimeData();
                if (!rd.partitions || rd.numPartitions <= 0) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                ++skins;
                for (std::int32_t i = 0; i < rd.numPartitions; ++i) {
                    const auto slot = rd.partitions[i].slot;
                    if (slot < 130u || slot > 143u) {
                        continue;  // the body-slot restorer's space, or caps
                    }
                    ++headParts;
                    if (rd.partitions[i].editorVisible) {
                        continue;
                    }
                    ++offBefore;
                    const auto bipedBit = 1u << (slot - 130u);
                    if ((drawnMask & bipedBit) != 0) {
                        continue;  // a worn piece that DRAWS owns this hide
                    }
                    dis->UpdateDismemberPartion(slot, true);
                    touched |= bipedBit;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        // Unconditional since round nineteen: the fielded version spoke only
        // when it flipped, so three silent runs could not say whether the
        // restore ran too early or never ran at all. One line carrying every
        // half of the answer: what it saw, what was off, what a worn piece
        // legitimately held down, what it put back.
        //
        // ⚠ IT NAMES ITS ACTOR AND ITS RACE since r30. Without them the banked
        // log had to be correlated by geometry count to tell one sweep from
        // another, and the two readings that mattered - the same helmet drawn
        // on the vanilla race and not drawn on the custom one - read as one
        // actor contradicting itself.
        spdlog::info("dismember: [{}] head sweep: actor {:08X} race {:08X}, {} geom(s), "
                     "{} dismember skin(s), {} head-family partition(s), {} OFF, "
                     "worn 0x{:X} drawn 0x{:X}, re-enabled 0x{:X}.",
                     a_note, a_actor->GetFormID(), raceID, geoms, skins, headParts,
                     offBefore, wornMask, drawnMask, touched);
        return touched;
    }

    void QueueRestoreHeadPartPartitions(RE::ActorHandle a_actor, const char* a_note) {
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        const auto handle = a_actor;
        task->AddTask([handle, a_note] {
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (actor) {
                RestoreHeadPartPartitions(actor, a_note);
            }
        });
    }

    namespace {
        // Live ladder threads, never the count of arms: the query below is
        // "could a late head-family write still be ours to repair", which is
        // what the dismember census gates on, and a capped skip below keeps a
        // click burst from spawning a thread per click.
        std::atomic<int> g_headSettleThreads{ 0 };
    }  // namespace

    bool HeadSettleActive() {
        return g_headSettleThreads.load(std::memory_order_acquire) > 0;
    }

    void ArmHeadPartitionSettle(RE::ActorHandle a_actor) {
        // ⚠ A WATCHER THREAD POSTING SINGLE TASKS, the ArmBaseWatch posture.
        // The round-nineteen restore lost because its queued pass rode the
        // SKSE task queue while the hair attach deferred through the
        // BSTaskPool; a rung at +1 s is on the far side of any attach that is
        // going to happen, and the +2.5 s and +5 s rungs cover a slow one.
        // ⛔ NEVER a task re-queueing a task (the FaceWait scar).
        if (g_headSettleThreads.fetch_add(1, std::memory_order_acq_rel) >= 3) {
            g_headSettleThreads.fetch_sub(1, std::memory_order_acq_rel);
            spdlog::debug("dismember: head settle ladder NOT armed, three already live.");
            return;
        }
        std::thread([a_actor] {
            struct Rung {
                std::chrono::milliseconds at;
                const char*               note;
            };
            // ⚠⚠ THE LAST RUNG MUST SIT PAST THE GHOST DWELL OR A PLAIN LOAD
            // CAN NEVER CONFIRM A GHOST. The dwell needs an unbroken not-drawn
            // stretch longer than kGhostDwellSeconds, and the clock only
            // advances when a SWEEP takes a reading - so a ladder whose last
            // rung is +5 s cannot reach a 6 s dwell however long the player
            // stands there. Field 2026-08-25 08:00, a plain load with no apply:
            // three sweeps at +1 s, +2.5 s and +5 s, the drop line stuck at
            // 00000001 for the whole session, and the character bald. Earlier
            // rounds only ever settled because a look apply happened to fire
            // more sweeps.
            //
            // ⛔ The answer is a longer ladder, NOT a shorter dwell: the dwell
            // is 6 s because a real helmet can still be attaching at +5 s
            // (MeasuredGhosts.h), so cutting it would put the r29 hair-through-
            // helmet bug back.
            // ⚠⚠ A RUNG'S NAME IS WHEN IT WAS ARMED, NOT WHEN IT RAN, and on a
            // load those are wildly different. The thread sleeps in wall-clock
            // while the GAME thread is stalled by the load, so every rung comes
            // due during the stall and the tasks then drain in a burst. Field
            // 2026-08-25 08:18, the five rungs above landing inside 5.2 s:
            //
            //   +1s   08:18:44.921     +8s   08:18:48.214
            //   +2.5s 08:18:44.922     +12s  08:18:50.168
            //   +5s   08:18:48.041
            //
            // The dwell is measured in real time from the FIRST not-drawn
            // reading, so a ladder that drains inside 5.2 s cannot clear a 6 s
            // dwell however far out its last rung is NAMED. That round stayed
            // bald with the drop line stuck at 00000001, which is the same
            // symptom as the +5 s ladder before it and a different cause.
            //
            // So the tail is long enough to survive the compression rather than
            // sized against the dwell directly. Two extra sweeps cost nothing;
            // the alternative is pacing the ladder off observed drains, which
            // means a task waiting on a task and that is the FaceWait scar.
            static constexpr Rung kRungs[] = {
                { std::chrono::milliseconds(1000), "ladder+1s" },
                { std::chrono::milliseconds(2500), "ladder+2.5s" },
                { std::chrono::milliseconds(5000), "ladder+5s" },
                { std::chrono::milliseconds(8000), "ladder+8s" },
                { std::chrono::milliseconds(12000), "ladder+12s" },
                { std::chrono::milliseconds(20000), "ladder+20s" },
                { std::chrono::milliseconds(30000), "ladder+30s" },
            };
            auto elapsed = std::chrono::milliseconds(0);
            for (const auto& rung : kRungs) {
                std::this_thread::sleep_for(rung.at - elapsed);
                elapsed = rung.at;
                if (auto* const task = SKSE::GetTaskInterface()) {
                    const auto  handle = a_actor;
                    const char* note   = rung.note;
                    task->AddTask([handle, note] {
                        auto       ptr   = handle.get();
                        RE::Actor* actor = ptr.get();
                        if (actor) {
                            RestoreHeadPartPartitions(actor, note);
                        } else {
                            spdlog::info("dismember: [{}] head sweep: actor gone.", note);
                        }
                    });
                }
            }
            g_headSettleThreads.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }

    void DumpBipedObjects(const char* a_when, RE::BipedAnim* a_biped) {
        auto* const current = CurrentPlayerBiped();
        spdlog::info("[dump {}] awmBiped={} currentBiped={} match={}", a_when,
                     static_cast<const void*>(a_biped), static_cast<const void*>(current),
                     a_biped == current);

        auto* const biped = a_biped ? a_biped : current;
        if (!biped) {
            spdlog::info("[dump {}] no biped.", a_when);
            return;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(RE::BIPED_OBJECTS::kTotal); ++i) {
            const auto& obj  = biped->objects[i];
            auto* const item = obj.item;
            auto* const node = obj.partClone.get();
            if (!item && !node) {
                continue;
            }
            const char* name = "?";
            int         at   = -1;
            if (item && item->Is(RE::FormType::Armor)) {
                name = item->GetName();
                at   = static_cast<int>(item->As<RE::TESObjectARMO>()->GetArmorType());
            }
            spdlog::info("[dump {}] slot {:2}: item={:08X} '{}' armorType={} ({}) partClone={}",
                         a_when, i, item ? item->GetFormID() : 0, name, at,
                         at == 0 ? "Light" : at == 1 ? "Heavy" : "-",
                         static_cast<const void*>(node));
        }
    }

}  // namespace OS::BipedPost
