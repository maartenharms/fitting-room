#include "NpcLoadSink.h"

#include "NpcHair.h"  // HoldsHeadPartFor: the other half of "is she one of ours"
#include "NpcIdentity.h"
#include "OutfitSession.h"

namespace OS::NpcLoadSink {

    namespace {

        // Split out of ProcessEvent so the sink stays a one-line try/catch
        // wrapper, exactly as RaceSwitchSink does.
        void Handle(const RE::TESObjectLoadedEvent* a_event) {
            if (!a_event || !a_event->loaded) {
                return;  // an UNLOAD has no geometry to put anything back onto
            }
            auto* const form  = RE::TESForm::LookupByID(a_event->formID);
            auto* const actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) {
                return;
            }
            if (actor->IsPlayerRef()) {
                // The player has his own refresh path and does not lose his look
                // to this: his 3D is not rebuilt by a cell change the way a
                // streamed follower's is.
                //
                // ⚠⚠ THAT SECOND CLAUSE IS A CLAIM ABOUT THE PLAYER'S 3D, NOT A
                // MEASUREMENT, AND THE FIELD DISPUTES IT. This sink exists
                // because a cell change reverted a FOLLOWER to her vanilla look
                // (OS-109), and eight or more reporters describe the PLAYER's
                // head doing the same thing on a cell change, recovering only on
                // a rebuild: RaceMenu, a save reload, or opening and closing the
                // inventory. If this event fires for the player on the doors
                // that break, the premise above is false and the player is
                // excluded from the one mechanism built for this defect.
                //
                // ⚠ SO IT IS COUNTED RATHER THAN ASSUMED. Same shape as the
                // slot 51 bug closed on 2026-09-01: an exclusion resting on an
                // untested claim about what cannot happen.
                //
                // ⛔ AN INSTRUMENT, NOT A CHANGE. The player is still returned
                // on, and wiring him into the follower re-apply would put a
                // SECOND painter on one appearance, which this codebase has
                // been bitten by before. Read the log first.
                static int playerLoads = 0;
                spdlog::info("NpcLoadSink: the PLAYER's 3D reported loaded (#{}). The follower "
                             "re-apply skips him here, on the claim that a cell change does not "
                             "rebuild his 3D. Every one of these lines is that claim being "
                             "tested; a line beside a face going wrong refutes it.",
                             ++playerLoads);
                return;
            }
            auto* const base = actor->GetActorBase();
            if (!base || base->IsDynamicForm()) {
                return;  // no persistent identity, so nothing could be assigned
            }
            // ⚠ RESOLVE AND KEY BEFORE SNAPSHOTTING. SnapshotNpcAssignments
            // copies the map under the session lock, and this event fires for
            // every reference a cell load streams in. Everything above is a
            // pointer test, so the copy is paid only for real, non-dynamic,
            // non-player actors - a couple of dozen per cell rather than
            // hundreds - and the map itself holds only assigned followers.
            const auto key = NpcKeyFor(base);
            if (!key) {
                return;
            }
            // ⚠⚠ TWO STORES, TWO QUESTIONS, AND THIS ASKED ONLY ONE. The outfit
            // assignment map does not know about a follower whose HAIR was
            // changed and who was never given an outfit, so she read as "not one
            // of ours", her reload was skipped, and her head rebuilt with her own
            // hair back underneath our style. Reported 2026-08-29: "when I change
            // a follower hair her default hair comes back on top of the hair i
            // chose when i switch cell". Hair-only was the whole of that report
            // and hair-only is exactly what this gate could not see.
            //
            // ⚠ THE HAIR CHECK IS SECOND ON PURPOSE. The snapshot above is
            // already paid for by the time we get here, and && short-circuits,
            // so the map walk behind HoldsHeadPartFor runs only for an actor the
            // outfit map has already disowned.
            const auto assignments = OutfitSession::GetSingleton().SnapshotNpcAssignments();
            if (!assignments.contains(*key) && !NpcHair::HoldsHeadPartFor(actor)) {
                return;  // not one of ours, by either store
            }
            // The same full re-apply the race switch uses, and for the same
            // reason: her geometry was rebuilt against a snapshot our writes had
            // never been applied to. It queues, re-resolves the handle when it
            // drains, and orders colour/refresh/style/hair internally.
            OutfitSession::RequestRefreshActor(actor->GetHandle());
            spdlog::debug("NpcLoadSink: '{}' {:08X} loaded 3D - re-applying her look "
                          "(OS-109).",
                          actor->GetName(), actor->GetFormID());
        }

        struct Sink : RE::BSTEventSink<RE::TESObjectLoadedEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESObjectLoadedEvent*               a_event,
                RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
                // BSTEventSink contract: never throw into the engine.
                try {
                    Handle(a_event);
                } catch (const std::exception& e) {
                    spdlog::error("NpcLoadSink threw: {}", e.what());
                } catch (...) {
                    spdlog::error("NpcLoadSink threw a non-standard exception.");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        Sink g_sink;

    }  // namespace

    void Register() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESObjectLoadedEvent>(&g_sink);
            spdlog::info("NpcLoadSink: registered (re-apply follower looks when their 3D "
                         "is rebuilt - cell change, fast travel, streaming).");
        }
    }

}  // namespace OS::NpcLoadSink
