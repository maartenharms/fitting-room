#include "WorldWatch.h"

#include "AdvancedCondition.h"
#include "CrashGuard.h"
#include "OutfitSession.h"
#include "MakeupApi.h"         // ReassertSkinTone: the held tone, between head builds
#include "OverlayApi.h"        // RunNodePropertyPush: the layers a skin repaint took
#include "OverlayReconcile.h"  // OS-233: Tick, the repair's armed check
#include "RaceMenuMorphApi.h"  // RunPushUpProbe: does our key survive OBody
#include "RealWorn.h"
#include "RefreshGate.h"
#include "RuleEngine.h"
#include "RuleStore.h"
#include "SamCompat.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace OS::WorldWatch {

    namespace {

        // ---- Engine state ------------------------------------------------
        //
        // Every mutator either marshals through SKSE::GetTaskInterface()->
        // AddTask, or is documented main-thread-only. The two exceptions are
        // OnSaveLoaded and OnRevert: both write this state inline, with no
        // AddTask hop, because both are ALREADY guaranteed main-thread by
        // their only callers - plugin.cpp's kPostLoadGame/kNewGame handler
        // and Persistence.cpp's SKSE serialization revert callback, neither
        // of which is reachable from anywhere but the main thread. That is a
        // documented precondition on those two functions, not a convention
        // every mutator here independently upholds - everything else in this
        // file (sinks, the heartbeat, the public Notify*/Set*/Resume/
        // RequestEvaluation entry points) marshals through AddTask precisely
        // because it CANNOT assume its caller's thread. g_publishedLock below
        // is a second, unrelated exception (see its own comment).

        Rules::RuleEngine g_engine;
        bool              g_engineEnabled{ true };
        // RuleEngine exposes only IsPinned(), not the pinned name, so this
        // shadows what WE told it via SetPinned - needed to write the name
        // back into RuleStore::EngineState (hazard 3) and to log it.
        std::string g_pinnedNameMirror;

        // What the engine had applied when the editor was last OPENED, kept so
        // the close can ask whether the look being left on is the one the rules
        // chose. See the resume test in SetEditorOpen for why it cannot be
        // derived at close time instead.
        Rules::Base g_editorEntryBase{};

        // Did THIS editor session touch the outfits at all? Opening the editor
        // to read the Rules tab and closing it again must not redress anybody,
        // so the exit apply below needs a reason to fire and this is it.
        //
        // Set by the three notifications that mean an outfit changed hands or
        // changed identity - a manual pick, a rename, a delete - rather than by
        // the editor's dirty flag. Dirty describes a staged edit that may still
        // be discarded; these three have already happened.
        //
        // ⚠ ATOMIC BECAUSE THE RULES TAB READS IT FROM THE RENDER THREAD. It is
        // written only on the game thread, and the read is one bool used to
        // decide whether to draw a button, so relaxed is enough: a frame drawn
        // against the previous value shows the right thing one frame later.
        std::atomic<bool> g_editorChangedOutfits{ false };

        // ⚠ ALIVE FOR EXACTLY ONE Evaluate() CALL, AND THE PROOF IS THAT IT IS
        // SET AND CLEARED AROUND ONE SYNCHRONOUS CALL RATHER THAN TRUSTED TO BE
        // CONSUMED. A one-shot that some path forgets to clear becomes a
        // permanent bypass of the setting it exists to step around, and the
        // symptom is auto switching applying rules while it is turned off,
        // which reads as the checkbox being broken.
        bool g_exitApplyOnce{ false };

        // The editor is OUR OWN FUCK window with one call site (EditorWindow
        // ::SetOpen) driving it, so a plain bool is fine - unlike the menu
        // gate below, there is no third party dispatching events we might
        // miss. GateClosed() reads this directly.
        bool g_editorOpen{ false };

        // ...and whether that editor is currently showing the Rules view rather
        // than the outfit body. Same single-call-site reasoning as above
        // (EditorUI's one transition detector drives it), so a plain bool again.
        bool g_rulesViewOpen{ false };

        // hazard 9: engine singletons exist before their arrays. "blocking"
        // here means "the world has not finished loading yet" rather than
        // OutfitSession's own BlockingCooldown's meaning (player mid-block
        // animation) - a second, independent instance for a second, distinct
        // purpose. Reconstructed (not just consulted) in OnSaveLoaded/
        // OnRevert: BlockingCooldown::Ready clears its own arming flag once
        // it releases and returns true immediately afterward, so on a
        // mid-session load - where pc->Get3D() is usually already non-null
        // by the time kPostLoadGame fires - a stale, already-released gate
        // would let the very first post-load BuildSnapshot read weather/
        // cell/worn state with zero settle. Reconstructing re-arms it.
        RefreshGate::BlockingCooldown g_loadGate;

        // The overlay-reconstruction anchor (RuleStore::EngineState::
        // lastAppliedRuleId), mirrored here so the "stopped matching" log
        // lines can name the rule that is clearing, and so PushEngineState
        // has something to write back.
        std::string g_lastAppliedRuleId;
        std::string g_lastAppliedRuleName;

        // Log latches (hazard 10): kSuppressed and kPaused repeat every
        // heartbeat with no change in RuleEngine's own state (dwell/pin do
        // not advance on their own), so a steady state must be de-duplicated
        // here or the log floods.
        bool          g_hasLastSuppressed{ false };
        Rules::Target g_lastSuppressed;
        bool          g_pinnedLogged{ false };

        // Missing-outfit rule ids already logged this save (hazard 5). Same
        // idiom as RuleStore.cpp's g_loggedCollisions.
        std::set<std::string> g_loggedMissingOutfit;

        // What was last written back into RuleStore for each Advanced-
        // bearing rule id (review finding 5), so a steady "still invalid,
        // same reason" state does not call RuleStore::WithRules - and so
        // queue a mirror save - on every single evaluate forever (review
        // finding 6). Cleared per save the same as the set above.
        std::unordered_map<std::string, std::pair<bool, std::string>> g_lastPushedAdvancedValidity;

        // The latest evaluation, read by the Rules tab (Task 13) through
        // WorldWatch::GetPublished() below, which copies all three fields
        // out under g_publishedLock.
        std::mutex                         g_publishedLock;
        Rules::Decision                    g_publishedDecision;
        Rules::WorldSnapshot               g_publishedSnapshot;
        std::map<std::string, std::string> g_publishedAdvancedInvalid;

        // Coalescing latch (review finding 2), same shape as OutfitSession::
        // RequestRefresh's g_playerRefreshQueued (OutfitSession.cpp:219,
        // 1435-1444): every trigger source (sinks, the heartbeat, gate
        // release, Resume, ...) funnels through QueueEvaluate, which queues
        // at most ONE pending Evaluate at a time. Without this, a fight
        // involving 20 NPCs queues 20 full evaluations - SKSE drains its
        // whole task queue in one frame, so that is 20 evaluations in one
        // frame, not spread out.
        std::atomic<bool> g_evalQueued{ false };

        // Heartbeat on its own timer thread. It used to re-queue itself onto the
        // SKSE task queue, which froze the game every launch - see the long note
        // at HeartbeatLoop below. The clock lives in that loop now, so there is no
        // shared timing state here.
        std::atomic<bool> g_heartbeatStarted{ false };  // guards a double Init() from starting two chains
        // Spec default (design doc: "fHeartbeatSeconds default 2.0"), now
        // read from Settings::Load() (Task 14, [Rules] fHeartbeatSeconds) and
        // cached here at Init(). The field-local default below only governs
        // if Init() were ever skipped, which it never is (plugin.cpp calls
        // it unconditionally at kDataLoaded).
        float g_heartbeatSeconds = 2.0f;

        // Defensive backstop for every entry point reachable from an engine
        // sink or the SKSE task queue (hazard 8: nothing may unwind into
        // engine frames). Mirrors the try/catch every other sink in this
        // project wraps its ProcessEvent in (see RaceSwitchSink.cpp).
        void SafeRun(const char* a_what, const std::function<void()>& a_fn) noexcept {
            try {
                a_fn();
            } catch (const std::exception& e) {
                spdlog::error("WorldWatch: {} threw: {}", a_what, e.what());
            } catch (...) {
                spdlog::error("WorldWatch: {} threw a non-standard exception.", a_what);
            }
        }

        double NowSeconds() {
            using Seconds = std::chrono::duration<double>;
            return Seconds(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // When the player was last casting, as NowSeconds(). Far in the past so
        // a session that has never seen a cast reads false rather than true.
        //
        // ⚠ IT IS NOT "WHEN THE CAST EVENT FIRED" ANY MORE. CastingHeld pushes
        // it forward on every poll that finds the player still casting, so it
        // means "the last moment casting was observed" and the tail below is
        // measured from the END of the cast rather than from its start.
        //
        // ⚠ DECLARED HERE, ABOVE BOTH USERS. The sink writes it and the
        // snapshot builder reads it, and the builder comes first in this file.
        std::atomic<double> g_lastCastAt{ -1.0e9 };

        // The spell the last cast event named, so the live read below can ask
        // the engine whether THAT cast is still running.
        //
        // ⚠ A FormID, NOT A MagicItem*. This is written by a sink and read by a
        // 100ms poll that outlives any number of loads, and an id that stops
        // resolving is a false rather than a dangling pointer.
        std::atomic<RE::FormID> g_lastCastSpell{ 0 };

        // Is the player casting RIGHT NOW, as the engine understands it.
        //
        // ⚠⚠ THIS IS THE ONLY THING THAT CAN TELL A CONCENTRATION SPELL FROM A
        // FIRE AND FORGET ONE, and until 2026-08-14 nothing did. TESSpellCastEvent
        // fires once, when the spell leaves the hand, and fires identically for
        // both: Flames channelled for twenty seconds and a fireball thrown once
        // produce the same single event. A fixed window measured from that event
        // therefore cannot fit either - it is always too short for the channel
        // and too long for the throw, and the field report of it being too long
        // ("it takes so long to switch back when we stop casting") is the second
        // half of that. Actor::IsCasting stays true for exactly as long as the
        // caster is actually running, which IS the distinction.
        //
        // ⚠ IT FAILS TO FALSE ON PURPOSE, and CastingHeld degrades to the old
        // timed window when it does. A wrong true here would strand the player
        // in the casting outfit with nothing left to end it; a wrong false costs
        // one tail. So an unresolvable spell, a missing player and a form that is
        // not a MagicItem all read false rather than guess.
        //
        // ⚠ MAIN THREAD ONLY. It is an engine call against live actor state, and
        // both callers are already main-thread (BuildSnapshot, and PollFastState
        // via the task queue).
        bool PlayerIsCastingNow() {
            const RE::FormID id = g_lastCastSpell.load(std::memory_order_relaxed);
            if (id == 0) {
                return false;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            auto* form = RE::TESForm::LookupByID(id);
            // As<MagicItem>, which covers SpellItem, ScrollItem, EnchantmentItem
            // and the rest in one: the base is what IsCasting takes, and every
            // concrete castable derives from it.
            auto* spell = form ? form->As<RE::MagicItem>() : nullptr;
            return spell != nullptr && player->IsCasting(spell);
        }

        // The state the engine grades: "casting, or stopped less than
        // fCastHoldSeconds ago".
        //
        // ⚠⚠ ONE FUNCTION BECAUSE THERE ARE TWO READERS AND THEY MUST NOT
        // DRIFT. BuildSnapshot asks so a rule can match; PollFastState asks so
        // the moment it goes false gets noticed inside 100ms instead of waiting
        // for the next heartbeat. Two copies of this would disagree the instant
        // either the tail or the clock source changed, and the symptom would be
        // an evaluate queued for an expiry the snapshot does not agree has
        // happened - or worse, never queued at all.
        //
        // ⚠ THE TAIL EXISTS FOR THE GAP BETWEEN SPELLS, not to make the
        // condition catchable - the live read already does that. It stops a
        // rotation of separate fire-and-forget casts flickering the outfit off
        // and on between them. That is why it is now short where the old fixed
        // window had to be long.
        //
        // A tail of 0 or less switches the condition off entirely rather than
        // making it permanently false-but-armed: a rule using kCasting then
        // simply never matches, which is what a player who set it to 0 asked
        // for. ⚠ Checked BEFORE the live read, so 0 means off even mid-cast.
        bool CastingHeld() {
            const float tail = Settings::GetSingleton().castHoldSeconds;
            if (tail <= 0.0f) {
                return false;
            }
            const double now = NowSeconds();
            if (PlayerIsCastingNow()) {
                g_lastCastAt.store(now, std::memory_order_relaxed);
                return true;
            }
            return (now - g_lastCastAt.load(std::memory_order_relaxed)) <
                   static_cast<double>(tail);
        }

        // Mirrors StyleRef::Make (StyleRef.cpp) for Rules::FormKey: identical
        // load-order-independent shape, identical GetFile(0) guard against
        // the GetLocalFormID-on-a-runtime-form crash (see NpcIdentity.h).
        bool MakeFormKey(RE::TESForm* a_form, Rules::FormKey& a_out) {
            if (!a_form) {
                return false;
            }
            auto* file = a_form->GetFile(0);
            if (!file) {
                return false;
            }
            a_out.modName     = std::string{ file->GetFilename() };
            a_out.localFormID = a_form->GetLocalFormID();
            return true;
        }

        // review finding 8 (round 2): RE::UI::GameIsPaused() answers "is a
        // PAUSING menu open" (RE/U/UI.h documents numPausesGame: += 1 only
        // if imenu->flags & kPausesGame), which is NOT "is the user occupied
        // in a menu". Skyrim Souls and friends deliberately unpause the
        // inventory and other menus; with one installed, GameIsPaused()
        // reads false while the player is mid-browse in their inventory,
        // and a rule would apply right then - exactly what the spec's Menu
        // stack row exists to prevent, and the inverse of the starvation bug
        // the gating design was built around in the first place.
        //
        // menuStack.empty() is no better a substitute: the HUD menu sits in
        // the stack during ordinary gameplay, so an empty-stack test would
        // gate forever.
        //
        // The gate is instead a live query over a DELIBERATE, hand-picked
        // set of menu names - "the user is occupied in UI", independent of
        // whether the game clock happens to be running, because an unpause
        // mod is exactly what pulls those two apart. Inventory/Container/
        // Barter/Gift cover item browsing (the SlotEntry-composition case
        // the spec calls out); Magic/Favorites are equip-adjacent; Journal/
        // Stats/Tween/Map are full-screen reads; Sleep/Wait and the Console
        // are world-state transactions in progress; RaceSex is the head editor
        // (HeadEditorSink already treats it specially elsewhere); Crafting is a
        // workbench; and SAM is a context this project already hosts its own
        // editor inside (SamCompat::IsMenuOpen(), which reads the configurable
        // menu name rather than a MENU_NAME constant).
        //
        // ⚠ DialogueMenu was in this list and has been REMOVED, deliberately.
        // It was here on the reasoning that "Dialogue is a scene", but that job
        // belongs to SceneGuard, which suspends the whole override for a real
        // scene mod (OStim etc.) and never reaches an evaluation at all.
        // Ordinary conversation is not that, and gating it made the kDialogue
        // condition self-defeating: a rule that takes the helmet off to talk to
        // someone could not apply until the conversation had ENDED, which is
        // the one moment it is not wanted. Helmet Toggle 2's dialogue option is
        // the whole reason the condition exists.
        constexpr std::string_view kGatingMenus[] = {
            RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
            RE::BarterMenu::MENU_NAME,    RE::GiftMenu::MENU_NAME,
            RE::MagicMenu::MENU_NAME,     RE::FavoritesMenu::MENU_NAME,
            RE::JournalMenu::MENU_NAME,   RE::StatsMenu::MENU_NAME,
            RE::TweenMenu::MENU_NAME,     RE::MapMenu::MENU_NAME,
            RE::SleepWaitMenu::MENU_NAME, RE::Console::MENU_NAME,
            RE::RaceSexMenu::MENU_NAME,   RE::CraftingMenu::MENU_NAME,
        };

        // Names what is holding application back, or empty when nothing is.
        // ApplyDecision used to return silently here, which made "my rule never
        // fires" unanswerable from a log: a rule can evaluate, win, and then be
        // deferred every single time with nothing written down.
        std::string GateReason() {
            // ⚠ The Rules view suspends the WHOLE gate, menus included - not
            // just the editor clause below.
            //
            // Narrowing only the editor clause was tried first and did not fix
            // the symptom, because there were two gates stacked. The editor is
            // launched from a button inside the INVENTORY menu, so InventoryMenu
            // is still open underneath it and kGatingMenus caught every apply
            // the editor clause had just stopped catching. The log said so
            // outright: "wants to apply but is waiting - InventoryMenu is open".
            //
            // Both clauses are answering the same question - "is the user busy
            // doing something a rule would fight?" - and in the Rules view the
            // answer is no for either. There is no edit buffer to fight
            // (EditorUI discards staging on the way in), and the inventory
            // behind the editor is the host it was opened from, not a
            // transaction in progress. A rebuild here is not a new risk either:
            // the editor's own staging preview rebuilds the player from exactly
            // this context every time a slot is clicked.
            //
            // Guarded on g_editorOpen as well so a stale g_rulesViewOpen - it is
            // deliberately not cleared on close - can never suspend the gate
            // during ordinary gameplay.
            if (g_editorOpen && g_rulesViewOpen) {
                return {};
            }
            if (g_editorOpen) {
                return "the outfit editor is open";
            }
            if (auto* ui = RE::UI::GetSingleton()) {
                for (const auto& name : kGatingMenus) {
                    if (ui->IsMenuOpen(name)) {
                        return std::string(name) + " is open";
                    }
                }
            }
            if (OS::SamCompat::IsMenuOpen()) {
                return "the SAM menu is open";
            }
            return {};
        }

        bool GateClosed() { return !GateReason().empty(); }

        // hazard 3: RuleEngine is the live authority, RuleStore::EngineState
        // is the serialization mirror. This is the ONE place that writes the
        // mirror, so the two cannot drift by a second writer forgetting.
        void PushEngineState() {
            RuleStore::EngineState st;
            st.engineEnabled     = g_engineEnabled;
            st.pinned             = g_engine.IsPinned();
            st.pinnedName          = g_engine.IsPinned() ? g_pinnedNameMirror : std::string{};
            st.lastAppliedRuleId  = g_lastAppliedRuleId;
            RuleStore::SetEngineState(std::move(st));
        }

        // Every place that drops the pin - a fresh unpinned load, a revert,
        // Resume, or SetEngineEnabled clearing a leftover manual pin when it
        // flips on - needs the SAME four pieces cleared together: the live
        // engine's pin, the name mirror shadowing it (hazard 3), and both
        // per-pin log latches. g_pinnedLogged so the next pause logs again;
        // g_hasLastSuppressed because evaluation is never gated on the
        // engine being enabled, so a target suppressed WHILE disabled would
        // otherwise dedupe away and silently eat the first "suppressed" line
        // once the engine comes back. One function so a future fifth site
        // inherits the whole set instead of reinventing which of these four
        // fields it remembers to clear - the exact drift that made the
        // SetEngineEnabled fix incomplete the first time. Caller still owns
        // PushEngineState/Evaluate: this only touches the four fields.
        // ⚠ GATED ON THE EDITOR BEING OPEN, and that is what makes the flag
        // mean "this editor session". All three callers are user-initiated and
        // none of them is editor-only: a manual pick can come from outside the
        // editor entirely, and an ungated flag would then still be set the next
        // time the editor closed, so a session that only read the Rules tab
        // would redress the player on the way out.
        void NoteEditorTouchedOutfits() {
            if (g_editorOpen) {
                g_editorChangedOutfits.store(true, std::memory_order_relaxed);
            }
        }

        void ClearPinState() {
            g_engine.ClearPin();
            g_pinnedNameMirror.clear();
            g_pinnedLogged      = false;
            g_hasLastSuppressed = false;
        }

        void PublishDecision(const Rules::WorldSnapshot& a_snapshot, const Rules::Decision& a_decision,
                              std::map<std::string, std::string> a_advancedInvalid) {
            std::scoped_lock l(g_publishedLock);
            g_publishedSnapshot        = a_snapshot;
            g_publishedDecision        = a_decision;
            g_publishedAdvancedInvalid = std::move(a_advancedInvalid);
        }

        const Rules::Rule* FindRule(const Rules::RuleSet& a_rules, const std::string& a_id) {
            if (a_id.empty()) {
                return nullptr;
            }
            for (const auto& r : a_rules) {
                if (r.id == a_id) {
                    return &r;
                }
            }
            return nullptr;
        }

        std::string TargetLabel(const Rules::Base& a_base) {
            switch (a_base.kind) {
                case Rules::BaseKind::kOutfit:
                    return fmt::format("'{}'", a_base.outfitName);
                case Rules::BaseKind::kRealGear:
                    return "real gear";
                case Rules::BaseKind::kKeep:
                default:
                    return "current look";
            }
        }

        std::string PinLabel(const std::string& a_pinnedName) {
            return a_pinnedName.empty() ? "real gear" : fmt::format("'{}'", a_pinnedName);
        }

        // ---- Snapshot build ----------------------------------------------

        Rules::WorldSnapshot BuildSnapshot(Rules::RuleSet& a_rules, double a_nowSeconds) {
            Rules::WorldSnapshot snap;
            snap.nowSeconds = a_nowSeconds;

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return snap;  // default/false-everything; Evaluate() already
                              // gates on pc existing before it gets here
            }

            // Location keyword chain: this location's own keywords, then
            // each parent's - Condition::kLocation matches a keyword
            // (LocTypeCity etc.), never a raw location form. Bounded so a
            // malformed (cyclic) parentLoc chain cannot spin forever.
            RE::BGSLocation* loc = player->GetCurrentLocation();
            for (int hops = 0; loc != nullptr && hops < 32; ++hops, loc = loc->parentLoc) {
                loc->ForEachKeyword([&](RE::BGSKeyword& a_kw) {
                    Rules::FormKey key;
                    if (MakeFormKey(&a_kw, key)) {
                        snap.locationKeywords.push_back(std::move(key));
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }

            if (auto* cell = player->GetParentCell()) {
                Rules::FormKey key;
                if (MakeFormKey(cell, key)) {
                    snap.cell = key;
                }
                snap.interior = cell->IsInteriorCell();

                // Regions hang off the CELL, and a cell can belong to several
                // overlapping ones at once (Helmet Toggle 2's cold-region spell
                // is an OR over fourteen), which is why the snapshot carries a
                // list and Matches is an any-of.
                //
                // GetRegionList(false) - never true. The argument is
                // createIfMissing, and this is a read on the render-adjacent
                // evaluate path: fabricating engine state to answer a query
                // would be a write we have no business making, and an interior
                // legitimately has none.
                if (auto* regions = cell->GetRegionList(false)) {
                    for (auto* region : *regions) {
                        Rules::FormKey rkey;
                        if (region && MakeFormKey(region, rkey)) {
                            snap.regions.push_back(std::move(rkey));
                        }
                    }
                }
            }

            if (auto* sky = RE::Sky::GetSingleton()) {
                if (auto* weather = sky->currentWeather) {
                    snap.raining = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy);
                    snap.snowing = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
                }
            }

            if (auto* calendar = RE::Calendar::GetSingleton()) {
                snap.gameHour = calendar->GetHour();
            }

            snap.inCombat = player->IsInCombat();
            snap.casting  = CastingHeld();  // see CastingHeld: PollFastState reads it too
            snap.sneaking = player->IsSneaking();
            // IsSwimming lives on ActorState, not Actor/PlayerCharacter
            // directly (unlike IsInCombat/IsSneaking, which are on Actor).
            if (auto* state = player->AsActorState()) {
                snap.swimming = state->IsSwimming();
            }

            RE::NiPointer<RE::Actor> mount;
            snap.mounted = player->GetMount(mount);

            if (auto* ui = RE::UI::GetSingleton()) {
                snap.inDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }

            // Keyword by EDITOR ID, not by a hardcoded FormID. Keywords are one
            // of the few form types that keep their editor id in-game, so this
            // needs no Address Library entry, no po3 cache, and cannot retarget
            // when the load order moves - the same reason FormKey exists in this
            // feature at all. Actor's keyword lookup folds in the race's
            // keywords, which is where "Vampire" actually lives (the vampire
            // races carry it; the actor record does not).
            snap.vampire = player->HasKeywordString("Vampire");

            // Real worn coverage: SnapshotRealWorn (RealWorn.h, shared with
            // BipedHooks.cpp), ONE walk over the player's InventoryChanges
            // entry list. review finding 3: a 32x GetWornArmor loop was
            // tried here first and reintroduced the exact cost this project
            // already profiled and abandoned in BipedHooks.cpp - each
            // GetWornArmor call builds the FULL armor inventory, so 32 calls
            // is 32 inventory builds, now paid every evaluate instead of
            // once per editor open. GetInventoryChanges() is a plain
            // accessor onto the actor's already-maintained
            // ExtraContainerChanges, not the GetInventory()-builds-a-map
            // path GetWornArmor takes internally.
            if (auto* changes = player->GetInventoryChanges()) {
                snap.wornSlotMask = SnapshotRealWorn(changes).coverage;
            }

            // Advanced clauses resolved last: main-thread only, per
            // AdvancedCondition.h, and needs a_rules to mark invalid rules.
            AdvancedCondition::Resolve(a_rules, snap);
            return snap;
        }

        // ---- CrashGuard filter (Step 2b) ----------------------------------

        void FilterCrashers(Rules::Overlay& a_overlay, const std::string& a_ruleName) {
            for (auto it = a_overlay.begin(); it != a_overlay.end();) {
                const auto& e = it->second;
                if (e.kind == SlotEntry::Kind::kStyle && CrashGuard::IsCrasher(e.style)) {
                    spdlog::warn(
                        "rules: dropped crasher style '{}'|{:06X} from rule '{}' (slot {})",
                        e.style.modName, e.style.localFormID, a_ruleName, it->first + 30u);
                    it = a_overlay.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // ---- Logging (hazard 10) -------------------------------------------
        //
        // Lines match the spec's Application path section verbatim so field
        // reports match the docs. Called only on a genuine transition -
        // RuleEngine::Evaluate itself only returns kApply when the derived
        // target differs from what it has recorded as applied, so the
        // "applied"/"cleared overlay" lines below need no latch of their
        // own; kSuppressed/kPaused repeat every heartbeat with nothing
        // changed, so those two DO need the latches declared above.

        void LogApplySuccess(const Rules::RuleSet& a_rules, const Rules::Decision& a_decision) {
            if (a_decision.ruleId.empty()) {
                const std::string prevName =
                    g_lastAppliedRuleName.empty() ? g_lastAppliedRuleId : g_lastAppliedRuleName;
                if (!prevName.empty()) {
                    spdlog::info("rules: cleared overlay (rule '{}' stopped matching)", prevName);
                }
                g_lastAppliedRuleId.clear();
                g_lastAppliedRuleName.clear();
            } else {
                const auto*       rule     = FindRule(a_rules, a_decision.ruleId);
                const std::string ruleName = rule ? rule->name : a_decision.ruleId;
                const int         priority = rule ? rule->priority : 0;
                spdlog::info("rules: applied {} (rule '{}', prio {}, dwell ok)",
                             TargetLabel(a_decision.base), ruleName, priority);
                g_lastAppliedRuleId   = a_decision.ruleId;
                g_lastAppliedRuleName = ruleName;
            }
        }

        void LogSuppressed(const Rules::RuleSet& a_rules, const Rules::Decision& a_decision) {
            const Rules::Target key{ a_decision.ruleId, a_decision.base, a_decision.overlay };
            if (g_hasLastSuppressed && key == g_lastSuppressed) {
                return;  // steady state; already logged this exact target
            }
            g_hasLastSuppressed = true;
            g_lastSuppressed    = key;

            if (a_decision.ruleId.empty()) {
                const std::string prevName =
                    g_lastAppliedRuleName.empty() ? g_lastAppliedRuleId : g_lastAppliedRuleName;
                spdlog::info(
                    "rules: suppressed overlay clear (rule '{}' stopped matching, dwell {:.1f}s "
                    "remaining)",
                    prevName.empty() ? "?" : prevName, a_decision.dwellRemaining);
            } else {
                const auto*       rule     = FindRule(a_rules, a_decision.ruleId);
                const std::string ruleName = rule ? rule->name : a_decision.ruleId;
                spdlog::info("rules: suppressed {} (rule '{}', dwell {:.1f}s remaining)",
                             TargetLabel(a_decision.base), ruleName, a_decision.dwellRemaining);
            }
        }

        void LogPaused(const Rules::Decision& a_decision) {
            if (g_pinnedLogged) {
                return;
            }
            g_pinnedLogged = true;
            spdlog::info("rules: paused (manual pin: {})", PinLabel(a_decision.pinnedName));
        }

        // ---- Missing-outfit handling (hazard 5) -----------------------------

        void HandleMissingOutfit(const Rules::Decision& a_decision, const std::string& a_ruleName) {
            if (a_decision.ruleId.empty()) {
                return;  // the no-rule target never names an outfit
            }
            if (!g_loggedMissingOutfit.insert(a_decision.ruleId).second) {
                return;  // already logged (and, if save-owned, already marked inert)
            }
            spdlog::warn(
                "rules: rule '{}' names outfit '{}' which is not in this save's library; "
                "marking the rule inert.",
                a_ruleName, a_decision.base.outfitName);

            // Persist inertness for a save-owned rule so PickWinner excludes
            // it on the next Merged() fetch and the UI can flag it (the
            // design doc's "flagged in the UI and inert" doctrine). Author
            // packs are read-only - RuleStore has no setter for a pack
            // rule's `invalid` flag - so a pack rule falls through the loop
            // below untouched and relies only on the logged-id set above to
            // avoid repeat log spam; PickWinner will just keep losing it to
            // whatever else matches every heartbeat until the outfit
            // reappears.
            //
            // review finding 6: the ownership test is folded into this ONE
            // WithRules call (a single g_currentLock acquisition) rather
            // than a separate RuleStore::Snapshot() read first - the earlier
            // shape took the store's lock twice for the same answer.
            const std::string reason =
                fmt::format("Outfit '{}' not found in this save's library.", a_decision.base.outfitName);
            RuleStore::WithRules([&](Rules::RuleSet& a_rules) {
                for (auto& r : a_rules) {
                    if (r.id == a_decision.ruleId) {
                        r.invalid       = true;
                        r.invalidReason = reason;
                        return;
                    }
                }
            });
        }

        // ---- Advanced-clause validity write-back (review finding 5) --------
        //
        // AdvancedCondition::Resolve (called from inside BuildSnapshot, every
        // evaluate) only mutates the throwaway RuleSet copy Evaluate() builds
        // each pass - RuleStore::Snapshot()/Merged() never see it, so a rule
        // an Advanced clause disqualifies is refused INVISIBLY: PickWinner
        // correctly never picks it (that copy's invalid=true is real for
        // this one pass), but the Rules tab (Task 13), which reads
        // RuleStore::Snapshot() for its error glyph/tooltip, would show it
        // as a perfectly healthy rule that simply never wins. invalidReason
        // is documented user-facing for exactly that UI.
        void PersistAdvancedValidity(const Rules::RuleSet& a_rules) {
            for (const auto& r : a_rules) {
                const bool hasAdvanced = std::ranges::any_of(
                    r.conditions,
                    [](const Rules::Condition& c) { return c.kind == Rules::ConditionKind::kAdvanced; });
                if (!hasAdvanced) {
                    // ⚠ Was a bare `continue`, and that stranded the warning
                    // triangle: delete a rule's Advanced clause and nothing
                    // re-runs for that rule ever again, so whatever invalid
                    // flag this loop last wrote stayed set for the rest of the
                    // session. Field-reported 2026-08-01.
                    //
                    // Cleared only when the live state is EXACTLY what this
                    // path last wrote. Rule::invalid is a single field shared
                    // with HandleMissingOutfit's missing-outfit reason, so a
                    // blind clear here would silently hide a real problem that
                    // has nothing to do with Advanced clauses. Matching on the
                    // reason string too is what keeps the two sources apart
                    // until they get the separate slots they need.
                    const auto it = g_lastPushedAdvancedValidity.find(r.id);
                    if (it == g_lastPushedAdvancedValidity.end()) {
                        continue;
                    }
                    if (it->second.first && r.invalid && r.invalidReason == it->second.second) {
                        RuleStore::WithRules([&](Rules::RuleSet& a_owned) {
                            for (auto& owned : a_owned) {
                                if (owned.id == r.id) {
                                    owned.invalid = false;
                                    owned.invalidReason.clear();
                                    return;
                                }
                            }
                        });
                        spdlog::info(
                            "rules: '{}' no longer has an Advanced clause; cleared the invalid "
                            "marker it left behind.",
                            r.id);
                    }
                    // Forget the cache entry either way, so re-adding an
                    // Advanced clause is resolved fresh rather than matching a
                    // stale answer and skipping the write-back.
                    g_lastPushedAdvancedValidity.erase(it);
                    continue;
                }
                // Skip the WithRules round trip (and the mirror save it
                // queues - review finding 6) when the last thing we wrote
                // already matches: Resolve reruns every evaluate and, with
                // today's stub, reaches the identical answer every time.
                const auto it = g_lastPushedAdvancedValidity.find(r.id);
                if (it != g_lastPushedAdvancedValidity.end() && it->second.first == r.invalid &&
                    it->second.second == r.invalidReason) {
                    continue;
                }
                const bool        invalid = r.invalid;
                const std::string reason  = r.invalidReason;
                RuleStore::WithRules([&](Rules::RuleSet& a_owned) {
                    for (auto& owned : a_owned) {
                        if (owned.id == r.id) {
                            owned.invalid       = invalid;
                            owned.invalidReason = reason;
                            return;
                        }
                    }
                    // Not save-owned - a pack rule. Packs are read-only and
                    // have no persistent invalid setter (same tolerance as
                    // HandleMissingOutfit above); the cache entry below still
                    // gets recorded so this loop does not keep retrying it.
                });
                g_lastPushedAdvancedValidity[r.id] = { invalid, reason };
            }
        }

        // ---- Application (the gated half) -----------------------------------

        // Latch so a steady deferral logs once rather than every heartbeat
        // (hazard 10, same reasoning as the kSuppressed/kPaused latches).
        std::string g_lastDeferReason;

        void ApplyDecision(const Rules::RuleSet& a_rules, const Rules::Decision& a_decision) {
            // ⚠ THE ONE-SHOT LIFTS THE ENGINE GATE AND NOTHING ELSE. GateReason
            // still holds: a menu open over the editor, or any other gate, is a
            // statement about whether it is SAFE to redress the player right
            // now, and leaving the editor does not make it safe. Only the
            // "auto-switching is turned off" half is stepped around, because
            // that is the setting this pass exists to serve rather than defy.
            const std::string gate = (!g_engineEnabled && !g_exitApplyOnce)
                                         ? "auto-switching is turned off"
                                         : GateReason();
            if (!gate.empty()) {
                // Deferred: RuleEngine's own applied_/target comparison IS
                // the retry mechanism (hazard 2) - the same target keeps
                // being derived as kApply every trigger until it actually
                // lands, and SetMenuOpen/SetEditorOpen(false) force an
                // immediate retry the instant the last gate clears.
                if (g_lastDeferReason != gate) {
                    g_lastDeferReason = gate;
                    spdlog::info("rules: '{}' wants to apply but is waiting - {}.",
                                 a_decision.ruleId.empty() ? "(no rule)" : a_decision.ruleId, gate);
                }
                return;
            }
            g_lastDeferReason.clear();

            const auto*       rule     = FindRule(a_rules, a_decision.ruleId);
            const std::string ruleName = rule ? rule->name : a_decision.ruleId;

            Rules::Overlay overlay = a_decision.overlay;
            FilterCrashers(overlay, ruleName.empty() ? "(no rule)" : ruleName);

            const bool realGear = a_decision.base.kind == Rules::BaseKind::kRealGear;
            const std::string_view outfitName =
                a_decision.base.kind == Rules::BaseKind::kOutfit
                    ? std::string_view(a_decision.base.outfitName)
                    : std::string_view{};

            const bool ok = OutfitSession::GetSingleton().ApplyRuleDecision(
                realGear, outfitName, std::move(overlay));
            if (!ok) {
                // hazard 5: nothing was applied, so NoteApplied must not run
                // - that would open a dwell window over a no-op.
                HandleMissingOutfit(a_decision, ruleName);
                return;
            }

            g_engine.NoteApplied(a_decision, NowSeconds());
            OutfitSession::RequestRefresh();
            LogApplySuccess(a_rules, a_decision);
            PushEngineState();  // lastAppliedRuleId just changed
        }

        // ---- The evaluate-and-apply pass -------------------------------------

        void Evaluate() {
            // review finding 2: clear the coalescing latch FIRST, same
            // "clear before applying" ordering as RunPlayerRefreshWhenSafe
            // (OutfitSession.cpp) - a trigger arriving while this pass is
            // still running queues exactly one follow-up, which then sees
            // whatever the world looks like when IT runs, not a stale
            // snapshot of the moment it was requested.
            g_evalQueued.store(false, std::memory_order_release);

            auto*      player   = RE::PlayerCharacter::GetSingleton();
            const bool notReady = player == nullptr || player->Get3D() == nullptr;
            // hazard 9: engine singletons exist before their arrays - give
            // the world a full settled second after pc->Get3D() appears
            // before the first post-load read of weather/cell/worn state.
            if (!g_loadGate.Ready(notReady, NowSeconds())) {
                return;
            }

            Rules::RuleSet       rules    = RuleStore::Merged();
            const double         now      = NowSeconds();
            Rules::WorldSnapshot snapshot = BuildSnapshot(rules, now);
            // review finding 5: write Advanced-clause invalidity back to the
            // store BEFORE the RuleEngine::Evaluate below, so a rule this
            // pass excludes is also visibly excluded to a Rules-tab reader,
            // not just silently skipped by PickWinner on the throwaway copy.
            PersistAdvancedValidity(rules);

            // Task 13 review finding 1: capture the SAME invalidity this
            // pass just resolved for PACK rules too, before it is lost -
            // PersistAdvancedValidity above only writes it back to
            // RuleStore for save-owned rules (packs are read-only), so
            // `rules` right here is the only place a pack rule's Advanced
            // invalidity is visible at all. Published alongside the
            // decision so a reader never needs a second, possibly-stale
            // evaluation of its own.
            std::map<std::string, std::string> advancedInvalid;
            for (const auto& r : rules) {
                if (r.invalid) {
                    advancedInvalid.emplace(r.id, r.invalidReason);
                }
            }

            // hazard 1: evaluation itself is never suppressed - this call
            // runs on every trigger regardless of menu/editor/engine-off
            // state, so the Rules tab's live condition display (Task 13)
            // stays accurate even while application is gated.
            const Rules::Decision decision = g_engine.Evaluate(rules, snapshot);
            PublishDecision(snapshot, decision, std::move(advancedInvalid));

            switch (decision.outcome) {
                case Rules::Outcome::kNoChange:
                    g_hasLastSuppressed = false;
                    g_pinnedLogged      = false;
                    break;
                case Rules::Outcome::kPaused:
                    g_hasLastSuppressed = false;
                    LogPaused(decision);
                    break;
                case Rules::Outcome::kSuppressed:
                    g_pinnedLogged = false;
                    LogSuppressed(rules, decision);
                    break;
                case Rules::Outcome::kApply:
                    g_hasLastSuppressed = false;
                    g_pinnedLogged      = false;
                    ApplyDecision(rules, decision);
                    break;
            }
        }

        // review finding 2: coalescing latch, same shape as OutfitSession::
        // RequestRefresh. Every trigger source in this file calls this
        // rather than posting Evaluate directly, so N events arriving before
        // the queue next drains collapse into exactly one evaluate.
        void QueueEvaluate() {
            if (g_evalQueued.exchange(true, std::memory_order_acq_rel)) {
                return;  // already queued; it will see the latest world state
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([] { SafeRun("Evaluate", &Evaluate); });
            } else {
                g_evalQueued.store(false, std::memory_order_release);
            }
        }

        // ---- Sinks -----------------------------------------------------------

        // review finding 8: this sink no longer OWNS the gate - it is just a
        // prompt trigger. GateClosed() (above) asks RE::UI::IsMenuOpen() about
        // a named set of menus, live, so a menu already on the stack when
        // Init() registers this sink, or any missed/unmatched event, can no
        // longer latch the gate closed for the rest of the session the way an
        // open/close counter could.
        struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                SafeRun("MenuSink", [&] {
                    if (a_event) {
                        QueueEvaluate();
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;

        // Combat/death: the engine does not reliably notify player combat
        // END (hazard 7, SOS's hard-won lesson), so neither sink trusts its
        // event's newState/dead field for STATE - the actual state is
        // re-read straight off the player inside BuildSnapshot every time.
        // review finding 2: the payload IS used to FILTER, which hazard 7
        // never forbade - ScriptEventSourceHolder dispatches these for every
        // actor in the loaded area, so without this filter one NPC fight
        // queues as many evaluations as it has combat-state transitions.
        struct CombatSink : RE::BSTEventSink<RE::TESCombatEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCombatEvent* a_event,
                RE::BSTEventSource<RE::TESCombatEvent>*) override {
                SafeRun("CombatSink", [&] {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (a_event && player &&
                        (a_event->actor.get() == player || a_event->targetActor.get() == player)) {
                        QueueEvaluate();
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        CombatSink g_combatSink;

        // ---- casting, the one trigger that is an EVENT ----------------------
        //
        // ⚠⚠ EVERY OTHER CONDITION IS A STATE AND THIS ONE IS NOT. The engine
        // grades a snapshot, so a condition has to be answerable as "is this
        // true right now". A fire-and-forget spell is a blink: polling for it
        // on a 2 second heartbeat misses nearly every cast. So the cast is
        // LATCHED here with a timestamp, and RuleModel's `casting` means "cast
        // within fCastHoldSeconds", which is a state.
        //
        // ⚠ AND IT QUEUES AN EVALUATION ITSELF. Without that the change waits
        // for the next heartbeat, so the outfit arrives up to two seconds after
        // the fireball and reads as unrelated to it.
        //
        // ⚠⚠ THIS EVENT CANNOT END A CAST AND MUST NOT BE ASKED TO. It fires
        // once, on release, and identically for a channelled Flames and a
        // thrown fireball, so nothing measured from it can tell the two apart.
        // How long the look stays on is decided by CastingHeld's live read and
        // its tail. Three things used to pile up behind this event instead, and
        // all three were fixed on 2026-08-14: the window was a fixed six seconds
        // that fitted neither kind of spell, nothing noticed it closing until
        // the next heartbeat, and RuleEngine's WantsBypass had no kCasting case
        // so the return trip waited out the dwell window on top of that.
        //
        // ⚠ AN AddSpell'd ABILITY FIRES NO EVENT HERE, which is correct rather
        // than a gap: a constant-effect ability attaches without a cast, and a
        // player who was handed a power by a script has not cast anything.
        struct SpellCastSink : RE::BSTEventSink<RE::TESSpellCastEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSpellCastEvent* a_event,
                RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
                SafeRun("SpellCastSink", [&] {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    // ⚠ THE PLAYER ONLY. Every NPC in earshot casts, and this
                    // sink runs for all of them; without the filter a town
                    // guard's candlelight would restyle the player.
                    if (!a_event || !player || a_event->object != player) {
                        return;
                    }
                    // The spell first: CastingHeld's live read is keyed on it,
                    // and the evaluate queued below may run before the next
                    // poll does.
                    g_lastCastSpell.store(a_event->spell, std::memory_order_relaxed);
                    g_lastCastAt.store(NowSeconds(), std::memory_order_relaxed);
                    QueueEvaluate();
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        SpellCastSink g_spellCastSink;

        struct DeathSink : RE::BSTEventSink<RE::TESDeathEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESDeathEvent* a_event,
                RE::BSTEventSource<RE::TESDeathEvent>*) override {
                SafeRun("DeathSink", [&] {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (a_event && player &&
                        (a_event->actorDying.get() == player || a_event->actorKiller.get() == player)) {
                        QueueEvaluate();
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        DeathSink g_deathSink;

        struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* a_event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override {
                SafeRun("EquipSink", [&] {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (a_event && player && a_event->actor && a_event->actor.get() == player) {
                        QueueEvaluate();
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        EquipSink g_equipSink;

        // ---- Heartbeat ---------------------------------------------------------
        //
        // ⚠ This was a self-requeuing SKSE task, and it froze the game on every
        // single launch. The design rested on the claim in RunPlayerRefreshWhenSafe's
        // comment (OutfitSession.cpp:351) that a task added during a drain lands on
        // a LATER pass. A probe measured the opposite on 2026-07-31: tasks added
        // during a drain run in the SAME pass, 6 to 237 microseconds later, where a
        // frame is 16700. So `AddTask(HeartbeatTick)` from inside HeartbeatTick
        // never gave the game loop back, and the main thread spun there forever.
        //
        // The tell in the field log, worth recognising again: heartbeats exactly
        // 2.000000s apart with a byte-identical fractional part every time. A
        // frame-driven poll cannot produce that, because frames jitter. Perfect
        // periodicity meant a tight loop checking the clock, not a healthy timer.
        // It reads as the opposite of a bug, which is why it survived a review.
        //
        // A timer thread rather than a per-frame hook, because this plugin has no
        // per-frame hook armed at the main menu: ImGuiOverlay's Present thunk is
        // patched in lazily on the first editor open (ImGuiOverlay.cpp:628), and
        // the freeze happens long before that.
        //
        // Shutdown hygiene is what review finding 1 actually wanted when it removed
        // the original thread, so it is kept: the thread wakes on a short slice
        // instead of sleeping the whole interval, so it is never parked in a long
        // sleep while the process tears down, and it checks a stop flag each time.
        // QueueEvaluate is already safe to call from any thread - it is what the
        // four event sinks call.
        std::atomic<bool> g_heartbeatStop{ false };

        // Cheap player-state edge watcher.
        //
        // The four sinks cover menus, combat, death and equipment. Everything
        // else - sneaking, swimming, mounting, and the ambient inputs like
        // weather and time - was only ever noticed by the heartbeat, so
        // crouching left the player standing in the wrong outfit for up to
        // fHeartbeatSeconds (2s by default). That is the unresponsiveness the
        // field report describes.
        //
        // The heartbeat cannot simply run faster: it is a full Evaluate, and
        // BuildSnapshot walks the location parent chain, reads the calendar and
        // weather, and snapshots the worn mask. These three are single boolean
        // reads off the player, so they can be sampled ten times a second and
        // only wake a real evaluate when one actually flips. Ambient inputs
        // keep the slower heartbeat, which is the right cadence for them.
        //
        // Runs on the main thread as a queued task, NOT on the heartbeat thread:
        // these read live actor state.
        // Dialogue is sampled here too rather than off the MenuOpenCloseEvent
        // sink: that sink exists to drive SetMenuOpen's gate bookkeeping, and
        // DialogueMenu was deliberately taken OUT of kGatingMenus, so routing
        // dialogue through it would mean re-adding the very entry the gate must
        // not have. IsMenuOpen is a hash lookup on an already-maintained map -
        // the same order of cost as the three boolean reads beside it.
        //
        // ⚠⚠ CASTING IS SAMPLED HERE AND IT IS NOT A PLAYER READ. Every other
        // member below is a boolean off the actor; this one is CastingHeld(),
        // the same derived latch BuildSnapshot grades. It is here because the
        // cast hold CLOSING fires nothing at all - the sink fires when the
        // spell leaves the hand and then nothing does - so the heartbeat was
        // what noticed, up to fHeartbeatSeconds (2s live) after a hold the
        // player had already waited fCastHoldSeconds (6s live) for. That is the
        // second half of "it takes so long to switch back when we stop
        // casting". Sampling it on the 100ms slice collapses that tail to 0.1s
        // with no timer per cast and without touching the heartbeat's cadence,
        // which is what the ambient inputs still want.
        //
        // ⚠ The ARRIVAL does not come from here and must not be moved here. The
        // spell cast sink queues its own evaluate, so the outfit lands on the
        // cast rather than on the next slice. This only covers the far edge.
        struct FastState {
            bool sneaking{};
            bool swimming{};
            bool mounted{};
            bool inDialogue{};
            bool casting{};

            bool operator==(const FastState&) const = default;
        };
        FastState g_lastFast;
        bool      g_hasLastFast{ false };

        void PollFastState() {
            // OS-233: the repair's armed check, run when it falls due. Cheap when
            // nothing is armed, which is nearly always.
            OverlayReconcile::Tick();
            auto* player = RE::PlayerCharacter::GetSingleton();
            // ⚠⚠ THE HELD SKIN TONE, AND THE SEVEN SECOND WHITE FLASH IS WHY.
            // The reassert has always been hooked to the head build, which is
            // what takes the tone away, and that is correct as far as it goes:
            // it puts the tone back on the NEXT build. Field 2026-08-25 08:56,
            // a fresh launch of a save that carries the hold:
            //
            //   08:56:18.567  tint skintone (136,177,198) -> (0,63,97)   correct
            //   08:56:23.199  tint skintone (0,63,97) -> (167,134,122)   a build took it
            //   08:56:30.005  Makeup: skin tone re-asserted after a head build
            //
            // Seven seconds of the wrong body, because nothing rebuilt the head
            // in between. Here it is corrected on the next heartbeat instead.
            //
            // ⚠ CHEAP AND SILENT WHEN IT AGREES: ReassertSkinTone early-returns
            // once the live slot already holds the value, and it does nothing at
            // all unless a tone is held, so a character Fitting Room is not
            // driving pays one predicate every hundred milliseconds.
            MakeupApi::ReassertSkinTone(player);
            // The worn-set watch, an instrument: says the moment any writer
            // changes what the list wears (field 2026-09-02 02:58).
            MakeupApi::NoteHeartbeat(player);
            // ⚠⚠ THE SECOND HALF OF THAT SENTENCE, AND NOT A DUPLICATE OF IT.
            // The reassert above owns the tint MASK and bodyTintColor. Neither is
            // what the head wears: the head wears a baked TEXTURE, and when skee
            // re-binds a preset file over that slot the mask is still perfectly
            // correct, so the reassert early-returns and the face stays wrong
            // with nothing saying why. Field 2026-08-26. Silent and free unless
            // something armed it; see ArmDelayedFaceRebake.
            MakeupApi::RunDelayedFaceRebake(player);
            // ⚠ DIAGNOSTIC, AND SILENT UNLESS A PUSH-UP WRITE ARMED IT. Same
            // heartbeat for the same reason: the answer only exists after OBody's
            // rebuild has landed, which is later than any call site of ours can
            // reach from where the write itself happens.
            RaceMenuMorphApi::RunPushUpProbe();
            // ⚠ THE THIRD PAINTER OF ONE APPEARANCE, AND THE SAME SHAPE AS THE
            // TWO ABOVE. A skin repaint puts bodyTintColor on the overlay clones
            // as well as on the body; this puts the layers' own colours back
            // once the repaints have stopped. Silent unless one armed it.
            OverlayApi::RunNodePropertyPush();
            // ⚠ THE FOURTH PAINTER, AND THE SAME SHAPE AGAIN. OBody's rebuild
            // clears the push-up morph key it knows nothing about; this puts the
            // lift back once the rebuild has finished. Silent unless a rebuild
            // armed it, and a no-op when the key survived.
            OutfitSession::RunPushUpReassert();
            if (!player || !player->Get3D()) {
                // Same gate Evaluate uses. Forget the baseline too, so the first
                // sample after a load is treated as a baseline rather than as a
                // change against whatever the previous save left behind.
                g_hasLastFast = false;
                return;
            }
            // Read through the same accessors BuildSnapshot uses, so this can
            // never disagree with the snapshot the evaluate then builds.
            FastState now;
            now.sneaking = player->IsSneaking();
            if (auto* state = player->AsActorState()) {
                now.swimming = state->IsSwimming();
            }
            RE::NiPointer<RE::Actor> mount;
            now.mounted = player->GetMount(mount);
            if (auto* ui = RE::UI::GetSingleton()) {
                now.inDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }
            now.casting = CastingHeld();

            if (!g_hasLastFast) {
                g_lastFast    = now;
                g_hasLastFast = true;
                return;
            }
            if (now == g_lastFast) {
                return;
            }
            g_lastFast = now;
            QueueEvaluate();  // coalesced like every other trigger
        }

        void HeartbeatLoop() {
            constexpr auto kSlice = std::chrono::milliseconds(100);
            double         last   = NowSeconds();
            while (!g_heartbeatStop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(kSlice);
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([] { SafeRun("PollFastState", &PollFastState); });
                }
                const double now = NowSeconds();
                if (now - last < static_cast<double>(g_heartbeatSeconds)) {
                    continue;
                }
                last = now;
                SafeRun("HeartbeatTick", [] {
                    QueueEvaluate();  // coalesced (finding 2) with every other trigger
                });
            }
        }

        void StartHeartbeatOnce() {
            if (g_heartbeatStarted.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            try {
                std::thread(HeartbeatLoop).detach();
            } catch (const std::exception& e) {
                // Resource exhaustion at construction, which is the risk review
                // finding 1 raised. Losing the periodic tick is survivable: the
                // four sinks still drive an evaluate on every menu, combat, death
                // and equip transition, so only the unprompted weather and
                // time-of-day re-check goes away.
                spdlog::error(
                    "WorldWatch: could not start the heartbeat thread ({}); rules will still "
                    "evaluate on menu/combat/death/equip events.",
                    e.what());
                g_heartbeatStarted.store(false, std::memory_order_release);
            }
        }

    }  // namespace

    void Init() {
        // review finding 1: std::thread construction could throw (resource
        // exhaustion); it is gone now, but Init() still touches RE::UI /
        // ScriptEventSourceHolder / the task interface, none of which are
        // expected to throw but none of which are worth trusting blind
        // either (hazard 8).
        SafeRun("Init", [] {
            // [Rules] fHeartbeatSeconds/fMinDwellSeconds. Settings::Load()
            // already ran at kDataLoaded, before this (see plugin.cpp).
            const auto& settings = OS::Settings::GetSingleton();
            g_heartbeatSeconds   = settings.heartbeatSeconds;
            g_engine.SetMinDwellSeconds(settings.minDwellSeconds);

            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
            }
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESCombatEvent>(&g_combatSink);
                holder->AddEventSink<RE::TESDeathEvent>(&g_deathSink);
                holder->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
                holder->AddEventSink<RE::TESSpellCastEvent>(&g_spellCastSink);
            }
            StartHeartbeatOnce();
            spdlog::info(
                "WorldWatch: registered (menu/combat/death/equip sinks, {}s heartbeat, {}s min dwell).",
                g_heartbeatSeconds, settings.minDwellSeconds);
        });
    }

    void OnSaveLoaded() {
        SafeRun("OnSaveLoaded", [] {
            g_engine.ResetForLoad();
            const auto state = RuleStore::GetEngineState();
            g_engineEnabled   = state.engineEnabled;
            // hazard 3: seed the engine's pin from the co-save's mirror -
            // miss this and a pinned save loads unpaused.
            if (state.pinned) {
                g_engine.SetPinned(state.pinnedName);
                g_pinnedNameMirror = state.pinnedName;
            } else {
                ClearPinState();
            }
            g_lastAppliedRuleId = state.lastAppliedRuleId;
            g_lastAppliedRuleName.clear();  // resolved lazily from the rule set on first use
            g_hasLastSuppressed = false;
            g_pinnedLogged      = false;
            g_loggedMissingOutfit.clear();
            g_lastPushedAdvancedValidity.clear();
            // The cast latch belongs to the session that was running, not to
            // this one. A spell id left over from the previous save would have
            // PlayerIsCastingNow querying a form this save may not even have.
            g_lastCastSpell.store(0, std::memory_order_relaxed);
            g_lastCastAt.store(-1.0e9, std::memory_order_relaxed);
            // review finding 4: reconstruct, not just consult - Ready()
            // clears its own arming flag once released and returns true
            // immediately after, so a stale gate from the PREVIOUS load
            // would let this save's very first BuildSnapshot read
            // weather/cell/worn state before the world has settled.
            g_loadGate = RefreshGate::BlockingCooldown{};
            // review finding 9: "the rule set reloaded wholesale" is exactly
            // what just happened - a stale cache entry keyed on a rule id
            // that a DIFFERENT save also happens to use would otherwise
            // evaluate the previous save's clause text against this one.
            AdvancedCondition::ClearCache();
            spdlog::info("WorldWatch: save loaded ({}{}).",
                         g_engineEnabled ? "engine on" : "engine off",
                         state.pinned ? ", pinned" : "");
        });
        // The first evaluation after load re-derives and re-applies the
        // overlay (the spec's Application path section); Evaluate() itself
        // waits out pc->Get3D()/BlockingCooldown (hazard 9) if the world is
        // not ready yet, so queuing this unconditionally is safe.
        QueueEvaluate();
    }

    void OnRevert() {
        SafeRun("OnRevert", [] {
            g_engine.ResetForLoad();
            ClearPinState();
            g_engineEnabled = true;
            g_lastAppliedRuleId.clear();
            g_lastAppliedRuleName.clear();
            g_loggedMissingOutfit.clear();
            g_lastPushedAdvancedValidity.clear();
            g_lastCastSpell.store(0, std::memory_order_relaxed);  // see OnSaveLoaded
            g_lastCastAt.store(-1.0e9, std::memory_order_relaxed);
            g_loadGate = RefreshGate::BlockingCooldown{};  // review finding 4
            AdvancedCondition::ClearCache();                // review finding 9
            // ruleOverlay_ itself is cleared by OutfitSession::OnRevert
            // (already wired, OutfitSession.cpp) - not this function's job.
        });
    }

    void SetMenuOpen(bool a_anyMenuOpen) {
        // review finding 8: the gate itself no longer lives here - it is
        // derived live from GateClosed()'s named-menu-set query. This is now
        // just a prompt trigger, same shape as the combat/death/equip sinks:
        // a menu transition is worth an evaluate attempt either way (closing
        // may unblock application; opening can only reconfirm the gate), and
        // ApplyDecision's own GateClosed() check is what actually decides
        // whether anything lands. a_anyMenuOpen is therefore ADVISORY ONLY -
        // it picks which of Evaluate()/QueueEvaluate() to call, nothing more
        // - not an oversight that the gate stopped reading it.
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([a_anyMenuOpen] {
                SafeRun("SetMenuOpen", [a_anyMenuOpen] {
                    if (!a_anyMenuOpen) {
                        Evaluate();  // a menu just closed - retry now, not next heartbeat
                    } else {
                        QueueEvaluate();
                    }
                });
            });
        }
    }

    void SetEditorOpen(bool a_open) {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([a_open] {
                SafeRun("SetEditorOpen", [a_open] {
                    g_editorOpen = a_open;
                    if (a_open) {
                        // ⚠ TAKEN ON THE WAY IN, AND IT HAS TO BE. SetPinned
                        // calls ForgetApplied, so the first tab click destroys
                        // the record the close below needs: by then the engine
                        // no longer remembers what it dressed the player in.
                        // This is that record, kept across the session.
                        g_editorEntryBase = g_engine.AppliedBase();
                        // Per session, cleared on the way IN rather than on the
                        // way out, so a close that returns early for any reason
                        // cannot leave the next session inheriting this one's
                        // answer.
                        g_editorChangedOutfits.store(false, std::memory_order_relaxed);
                        return;
                    }

                    // ---- the resume test --------------------------------
                    // ⚠ THE PIN IS DECIDED HERE, NOT AT THE TAB CLICK. Every
                    // PinManualPick site in the editor is an outfit-SWITCHING
                    // action, and clicking a tab is how you LOOK at an outfit
                    // as well as how you choose one, so merely browsing paused
                    // auto-switching and left the player hunting for Resume
                    // (user 2026-08-04, "frustrating having to click it all
                    // the time"). EditorUI carries the same lesson one level
                    // down, where Apply stopped pinning for this exact reason.
                    //
                    // ⚠ NOT "CLEAR THE PIN ON CLOSE". That was the first
                    // proposal and was declined the same day: a hand-picked
                    // outfit could then never survive closing the editor while
                    // rules are on, which makes the outfit tabs useless. The
                    // question is narrower - is the look being left on the one
                    // the RULES put there? If it is, nothing was overridden
                    // and there is nothing to pause.
                    //
                    // A stale entry base is the safe direction. If the world
                    // moved while the editor was open, the snapshot names an
                    // outfit the rules have since stopped wanting; leaving on
                    // it clears the pin and the evaluation below simply dresses
                    // the player correctly, which is the outcome they wanted
                    // anyway.
                    // ⚠ THE NARROW TEST ABOVE IS NO LONGER THE WHOLE ANSWER, and
                    // the paragraph above it now describes a decision that has
                    // been reversed. Kept because the reasoning is still the
                    // reason this is a decision rather than an obvious call.
                    //
                    // The narrow test only resumed when the editor was left on
                    // the outfit the RULES had chosen - which is precisely the
                    // case where the user had not picked anything. So every
                    // actual outfit switch still ended paused, with a Resume
                    // button to go and find, every single time. The user hit
                    // that repeatedly on 2026-08-06 and called it: resuming
                    // matters more than protecting the hand-pick. The full
                    // derivation, and the known cost, are on
                    // Rules::ShouldClearPinOnEditorExit.
                    if (g_engine.IsPinned()) {
                        const auto  snap   = OutfitSession::GetSingleton().SnapshotLibrary();
                        const auto* active = snap.Active();
                        const bool  leftOnRulesPick =
                            g_editorEntryBase.kind == Rules::BaseKind::kOutfit
                                ? (active != nullptr &&
                                   active->name == g_editorEntryBase.outfitName)
                                : (g_editorEntryBase.kind == Rules::BaseKind::kRealGear &&
                                   active == nullptr);
                        if (Rules::ShouldClearPinOnEditorExit(g_editorChangedOutfits.load(std::memory_order_relaxed),
                                                              leftOnRulesPick)) {
                            spdlog::info(
                                "rules: editor closed after an outfit change ({}), so auto "
                                "switching resumes without a Resume press.",
                                leftOnRulesPick ? "left on the rules' own pick"
                                                : "left on a hand-picked outfit");
                            ClearPinState();
                        }
                    }
                    // Cleared either way. It describes ONE editor session and a
                    // later close must not be able to read an older one's answer.
                    g_editorEntryBase = Rules::Base{};

                    // ---- the exit apply, for players who keep the engine off -
                    //
                    // ⚠ A DIFFERENT QUESTION FROM THE PIN ABOVE, which is why it
                    // is a second predicate and not the same one. The pin coming
                    // off says the engine may act again, and that happens in
                    // both states. This says the CLOSE has to apply the rules
                    // itself, and that is only true when no heartbeat is going
                    // to: with auto switching on, the engine re-evaluates within
                    // a couple of seconds of the pin clearing and doing it here
                    // as well would just be the same answer applied twice.
                    if (Rules::ShouldApplyOnEditorExit(g_engineEnabled,
                                                       g_editorChangedOutfits.load(
                                                           std::memory_order_relaxed))) {
                        spdlog::info(
                            "rules: editor closed after an outfit change with auto switching "
                            "off - running one apply pass (the heartbeat is not going to).");
                        // ⚠ SET AND CLEARED AROUND ONE SYNCHRONOUS CALL. Evaluate
                        // runs inline, so the flag cannot outlive it however that
                        // call returns, and there is no path where a forgotten
                        // one-shot turns into a permanent bypass of the setting.
                        g_exitApplyOnce = true;
                        Evaluate();
                        g_exitApplyOnce = false;
                        PushEngineState();  // the pin may have just been cleared
                        return;
                    }

                    Evaluate();  // last gate cleared - retry now, not next heartbeat
                });
            });
        }
    }

    bool PinResolvesOnEditorExit() {
        return g_editorChangedOutfits.load(std::memory_order_relaxed);
    }

    void SetRulesViewOpen(bool a_open) {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([a_open] {
                SafeRun("SetRulesViewOpen", [a_open] {
                    g_rulesViewOpen = a_open;
                    if (a_open) {
                        // A gate just cleared, so retry now rather than waiting
                        // out the heartbeat - the same shape as SetEditorOpen
                        // (false) and SetMenuOpen(false). Leaving Rules needs no
                        // counterpart: the gate CLOSES on the way out, and
                        // EditorUI re-asserts the edit preview itself.
                        Evaluate();
                    }
                });
            });
        }
    }

    void NotifyManualPick(std::string_view a_outfitName) {
        const std::string name(a_outfitName);
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([name] {
                SafeRun("NotifyManualPick", [name] {
                    NoteEditorTouchedOutfits();
                    g_engine.SetPinned(name);
                    g_pinnedNameMirror  = name;
                    g_pinnedLogged      = false;  // a fresh pin should log again
                    g_hasLastSuppressed = false;
                    // hazard 4: ApplyRuleDecision runs only on kApply, so
                    // nothing else clears the overlay when the engine goes
                    // quiet - without this the paused rule's overlay (e.g.
                    // a helmet-hide) stays composed over the look the user
                    // just picked by hand.
                    OutfitSession::GetSingleton().ClearRuleOverlay();
                    OutfitSession::RequestRefresh();
                    PushEngineState();
                    Evaluate();  // logs "paused" immediately via LogPaused
                });
            });
        }
    }

    void NotifyOutfitRenamed(std::string_view a_from, std::string_view a_to) {
        const std::string from(a_from);
        const std::string to(a_to);
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([from, to] {
                SafeRun("NotifyOutfitRenamed", [from, to] {
                    NoteEditorTouchedOutfits();
                    // Rule bases and, if the STORE's own mirror happens to
                    // name a_from, the co-save record: this part is unrelated
                    // to whether a live pin exists (it also runs correctly
                    // before any session/engine exists, at co-save decode).
                    RuleStore::RenameOutfitEverywhere(from, to);
                    // The LIVE pin (hazard 3's other owner) only needs
                    // following if it currently names a_from - which is NOT
                    // true on most keystrokes of an in-progress rename, so
                    // this stays cheap. A rename is not a fresh pick, so
                    // RenamePin (not SetPinned) - it must not ForgetApplied
                    // or reset the dwell clock, only relabel. g_pinnedLogged
                    // resets so LogPaused prints the corrected name on the
                    // next evaluation instead of staying latched on the old
                    // one for the rest of the session (finding 1).
                    if (g_engine.IsPinned() && g_pinnedNameMirror == from) {
                        g_engine.RenamePin(from, to);
                        g_pinnedNameMirror = to;
                        g_pinnedLogged     = false;
                        PushEngineState();
                        Evaluate();
                    }
                });
            });
        }
    }

    void NotifyOutfitDeleted(std::string_view a_name) {
        const std::string name(a_name);
        if (name.empty()) {
            return;  // nothing to reconsider
        }
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([name] {
                SafeRun("NotifyOutfitDeleted", [name] {
                    NoteEditorTouchedOutfits();
                    // Unlike a rename, deleting an outfit rewrites no rule
                    // text - Base::outfitName still names the gone outfit
                    // verbatim - so PickWinner would keep deriving the SAME
                    // Target and Evaluate would keep comparing it equal to
                    // applied_ forever. ForgetApplied (the same primitive
                    // SetPinned/ResetForLoad use) is what forces the next
                    // Evaluate to actually reconsider instead of reporting
                    // kNoChange.
                    g_engine.ForgetApplied();
                    // hazard 4 again (see NotifyManualPick/SetEngineEnabled's
                    // disable edge): the overlay must not keep composing
                    // between now and whenever Evaluate below actually gets
                    // to apply something - the editor is almost certainly
                    // still open right now, mid-delete-flow, which gates
                    // ApplyDecision - so clear it eagerly rather than
                    // waiting for ApplyDecision to get there.
                    OutfitSession::GetSingleton().ClearRuleOverlay();
                    OutfitSession::RequestRefresh();
                    spdlog::info(
                        "rules: outfit '{}' deleted; forgetting the applied target so rules "
                        "re-derive.",
                        name);
                    // Deliberately NOT marking any rule invalid here - see
                    // this function's declaration comment. The re-derive
                    // below reaches ApplyDecision's existing kApply branch
                    // exactly like any other trigger (once the gate clears,
                    // if it is not clear already), and HandleMissingOutfit
                    // is the one place that marks a rule inert for a
                    // missing outfit.
                    Evaluate();
                });
            });
        }
    }

    void Resume() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                SafeRun("Resume", [] {
                    ClearPinState();
                    PushEngineState();
                    Evaluate();  // the resulting switch lands immediately
                });
            });
        }
    }

    void SetEngineEnabled(bool a_enabled) {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([a_enabled] {
                SafeRun("SetEngineEnabled", [a_enabled] {
                    const bool was  = g_engineEnabled;
                    g_engineEnabled = a_enabled;
                    if (was && !a_enabled) {
                        // hazard 4: same reasoning as NotifyManualPick - the
                        // engine going quiet must not leave a stale overlay
                        // composed over whatever the player is wearing. The
                        // pin, unlike the overlay, is deliberately LEFT SET:
                        // the enable edge below always clears it, so there is
                        // nothing to fix up here, and a save written during
                        // the disabled window correctly remembers what the
                        // user was wearing on purpose when it went quiet.
                        OutfitSession::GetSingleton().ClearRuleOverlay();
                        OutfitSession::RequestRefresh();
                    }
                    if (!was && a_enabled) {
                        // Enabling auto-switching IS the user asking for rules
                        // to run, so it overrides any pin left over from
                        // dressing by hand while the engine was off. Without
                        // this, a user who has been using the editor normally
                        // turns the feature on and finds it immediately
                        // reporting itself paused. No overlay work needed on
                        // this edge, unlike disable's: the disable edge above
                        // already cleared it, and the Evaluate() below
                        // re-derives whatever the now-running rules compose.
                        ClearPinState();
                    }
                    PushEngineState();
                    Evaluate();
                });
            });
        }
    }

    void RequestEvaluation() { QueueEvaluate(); }

    void RequestEvaluationForUserEdit() {
        // Not QueueEvaluate: the latch has to be set on the MAIN thread before
        // the evaluation reads it, and RulesUI calls this from the render
        // thread. Marshalling both together also means the coalescing flag
        // cannot merge this with an in-flight world-driven evaluation that has
        // already passed the dwell check. Same Evaluate()-inside-AddTask shape
        // as SetEditorOpen and NotifyManualPick.
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                SafeRun("RequestEvaluationForUserEdit", [] {
                    g_engine.BypassDwellOnce();
                    Evaluate();
                });
            });
        }
    }

    Published GetPublished() {
        std::scoped_lock l(g_publishedLock);
        return Published{ g_publishedDecision, g_publishedSnapshot, g_publishedAdvancedInvalid };
    }

}  // namespace OS::WorldWatch
