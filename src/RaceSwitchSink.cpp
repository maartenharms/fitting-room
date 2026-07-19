#include "RaceSwitchSink.h"

#include "NpcResolve.h"
#include "OutfitSession.h"

namespace OS::RaceSwitchSink {

    namespace {

        // The ActorTypeCreature keyword (Skyrim.esm 0x00013795): every beast/
        // creature race carries it, no humanoid race does. Werewolf
        // (WerewolfBeastRace) and Vampire Lord (DLC1VampireBeastRace) have it;
        // ordinary vampire races (NordRaceVampire etc.) and Khajiit/Argonian
        // do NOT - they are styleable humanoids. Resolved once on first use,
        // like BipedHooks' HT2StateGlobal; the sink only runs on the game
        // thread.
        constexpr RE::FormID kActorTypeCreatureID = 0x00013795;

        RE::BGSKeyword* ActorTypeCreature() noexcept {
            static RE::BGSKeyword* kw = []() -> RE::BGSKeyword* {
                auto* dh = RE::TESDataHandler::GetSingleton();
                auto* k  = dh ? dh->LookupForm<RE::BGSKeyword>(kActorTypeCreatureID, "Skyrim.esm")
                              : nullptr;
                if (!k) {
                    spdlog::warn("RaceSwitchSink: ActorTypeCreature keyword not found - "
                                 "beast-form suspension disabled (race switches will not "
                                 "stand the override down).");
                }
                return k;
            }();
            return kw;
        }

        // True iff a_race is a beast/creature race (no styleable humanoid
        // biped). A missing keyword (ActorTypeCreature() == null) makes this
        // conservatively false: never suspend a humanoid we failed to classify.
        bool RaceIsBeast(RE::TESRace* a_race) noexcept {
            auto* kw = ActorTypeCreature();
            return kw && a_race && a_race->HasKeyword(kw);
        }

        // The actual work, split out of ProcessEvent so the sink's try/catch
        // stays a one-line wrapper around it (see Sink::ProcessEvent below).
        void Handle(const RE::TESSwitchRaceCompleteEvent* a_event) {
            if (!a_event || !a_event->subject) {
                return;
            }
            auto* actor = a_event->subject->As<RE::Actor>();
            if (!actor) {
                return;  // a non-actor reference "switched race" - never happens; defensive
            }
            auto* base = actor->GetActorBase();
            if (!base) {
                return;
            }
            // Beast-ness of the CURRENT race is the trigger - NOT a current-vs-
            // base race diff (which would wrongly suspend an ordinary vampire;
            // see NpcResolve::ShouldSuspendForRace for the full why).
            const bool suspend = NpcResolve::ShouldSuspendForRace(RaceIsBeast(actor->GetRace()));

            auto&       session = OutfitSession::GetSingleton();
            auto* const player  = RE::PlayerCharacter::GetSingleton();
            if (player && actor == player) {
                // The GLOBAL player path (independent of the NPC suspension
                // set below). Suspend()/Resume() already refresh internally -
                // see OutfitSession.cpp - so nothing further is needed here.
                if (suspend) {
                    session.Suspend();
                } else {
                    session.Resume();
                }
                return;
            }

            const std::uint32_t baseFormID = base->GetFormID();
            if (suspend) {
                session.SuspendActor(baseFormID);
                // No follow-up refresh: a beast/vampire-lord/werewolf form has
                // no humanoid biped to style in the first place, and the
                // engine's own SwitchRace rebuild already ran before this
                // event fired - there is nothing left to visually undo.
            } else {
                session.ResumeActor(baseFormID);
                // The switch's OWN biped rebuild already ran (this event
                // fires AFTER completion) against the still-suspended
                // snapshot, so the assignment did not apply to it. Kick one
                // more rebuild now that ResumeActor has published the
                // unsuspended snapshot, instead of waiting on the actor's
                // next unrelated rebuild to pick it up.
                OutfitSession::RequestRefreshActor(actor->GetHandle());
            }
        }

        struct Sink : RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSwitchRaceCompleteEvent*               a_event,
                RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override {
                // BSTEventSink contract: never throw into the engine. Handle()
                // touches OutfitSession (mutex-protected) and queues a task -
                // neither is expected to throw, but this is a hard backstop.
                try {
                    Handle(a_event);
                } catch (const std::exception& e) {
                    spdlog::error("RaceSwitchSink threw: {}", e.what());
                } catch (...) {
                    spdlog::error("RaceSwitchSink threw a non-standard exception.");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        Sink g_sink;

    }  // namespace

    void Register() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESSwitchRaceCompleteEvent>(&g_sink);
            spdlog::info("RaceSwitchSink: registered (race-switch suspension, player + NPC).");
        }
    }

}  // namespace OS::RaceSwitchSink
