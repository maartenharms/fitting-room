#include "HeadEditorSink.h"

#include "DefaultLook.h"  // the purchases this sink is allowed to delete
#include "DyeUnlocks.h"   // the Seamstone's charge, for the door fee below
#include "EditorStyle.h"  // PlayUISound, for the coins at the door
#include "HairColor.h"
#include "HeadBuildHook.h"  // the late-rebake gate, run on this edge too
#include "HeadPart.h"
#include "MakeupApi.h"  // the look's skin tone across the editor trip
#include "MakeupBaseline.h"  // the tint-list record follows a RaceMenu visit too
#include "MenuStudioCompat.h"  // repaint the strip tile after the door fee moves the charge
#include "OutfitSession.h"
#include "OverlayBaseline.h"  // the overlay record follows a RaceMenu visit's edits
#include "PaintScrape.h"  // OS-219 route B: read RaceMenu's paint lists during the trip
#include "RaceSwitchSink.h"  // the identity guard's switch counter
#include "RefreshGate.h"
#include "Settings.h"

namespace OS::HeadEditorSink {

    namespace {

        // Take the character editor's door fee, once, as it opens.
        //
        // ⚠ A FEE ON ENTRY RATHER THAN A BILL ON WHAT WAS DONE, and the shape
        // is forced by whose window it is. RaceMenu is another mod's, so nothing
        // here can see what was sculpted in there and there is no count to
        // multiply. Charging for the trip is the only honest thing available,
        // which is why the setting is a flat rate and not a per-change one.
        //
        // ⚠ TAKEN, NOT CHECKED FIRST. Refusing entry is not possible from here:
        // the menu is already opening by the time this event arrives and there
        // is no supported way to shut it that does not look like a crash. So a
        // player who cannot pay gets in free, and SpendCharge's own refusal is
        // what stops the charge going negative. The button that OFFERS the trip
        // is the gate; see EditorWindow, which hides it when the fee is out of
        // reach. Anyone reaching RaceMenu another way was never gated at all.
        void TakeLooksMenuFee() {
            const auto& cfg = Settings::GetSingleton();
            switch (cfg.costMode) {
                case OS::CostMode::kFree:
                    return;
                case OS::CostMode::kGold: {
                    const auto price = cfg.goldPerLooksMenu;
                    if (price == 0) {
                        return;
                    }
                    auto* const player = RE::PlayerCharacter::GetSingleton();
                    auto* const gold =
                        RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);
                    if (!player || !gold) {
                        return;
                    }
                    // RemoveItem takes what is there and no more, so a short
                    // purse pays what it can rather than going negative.
                    player->RemoveItem(gold, static_cast<std::int32_t>(price),
                                       RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    // The same coins the apply path plays. Leaving the door fee
                    // silent while applying a style chimes would read as one of
                    // the two being broken rather than as a deliberate
                    // difference, and both are the player paying gold.
                    if (!cfg.goldSpendSound.empty()) {
                        EditorStyle::PlayUISound(cfg.goldSpendSound.c_str());
                    }
                    spdlog::info("LooksMenu: took {} gold at the door.", price);
                    return;
                }
                case OS::CostMode::kCharge: {
                    const auto price = cfg.chargePerLooksMenu;
                    if (price == 0) {
                        return;
                    }
                    // ⚠ MAIN THREAD, which a MenuOpenCloseEvent already is, so
                    // With needs no marshal here. The editor's own spend has to
                    // hop threads because it runs on the FUCK present thread;
                    // this does not.
                    bool paid = false;
                    DyeUnlocks::With([&](DyeUnlockSet& a_set) {
                        paid = a_set.SpendCharge(price);
                    });
                    spdlog::info("LooksMenu: door fee of {} charge {}.", price,
                                 paid ? "taken" : "REFUSED, the stone is short; entry "
                                                  "was free this time");
                    OS::MenuStudioCompat::RefreshCharge();
                    return;
                }
            }
        }

        // ---- the character-editor visit -----------------------------------
        //
        // What Fitting Room knew about the player's look when the editor
        // OPENED, so the close edge can say what the player changed in there.
        // The decision itself is pure and lives in RefreshGate; this is the
        // engine reading that feeds it.
        //
        // ⚠ ONE READING, FILE LOCAL, AND DELIBERATELY NOT PERSISTED. It
        // describes a menu that is open right now, so a save written in between
        // is not a thing that happens, and a stale one from a previous
        // character would authorise deleting the current one's purchases. Wiped
        // by Forget() on a revert for that second reason.
        //
        // The four head-part kinds are held as a list because every one of them
        // is read identically. The mapping back onto EditorVisit's five NAMED
        // fields is written out in full at the bottom of CloseTheVisit, which is
        // where a sixth dimension should make somebody stop and think.
        constexpr HeadPart::Kind kKinds[] = { HeadPart::Kind::kHair,
                                              HeadPart::Kind::kEyes,
                                              HeadPart::Kind::kBrows,
                                              HeadPart::Kind::kFacialHair };
        constexpr std::size_t    kPartCount = std::size(kKinds);

        struct OpenReading {
            bool          armed{ false };
            bool          identityReadable{ false };
            std::uint32_t race{ 0 };
            std::int32_t  sex{ -1 };
            std::uint32_t switches{ 0 };
            bool          partsReadable{ false };
            std::uint32_t part[kPartCount]{};
            std::uint32_t partWrites[kPartCount]{};
            bool          defaultAuthoredPart[kPartCount]{};
            bool          colourReadable{ false };
            std::uint32_t colour{ 0 };
            std::uint32_t colourWrites{ 0 };
            bool          defaultAuthoredColour{ false };
        };
        OpenReading g_open;

        std::uint32_t FormIdOf(RE::BGSHeadPart* a_part) {
            return a_part ? a_part->GetFormID() : 0u;
        }

        // ⚠ CALLED AFTER THE STAND-DOWN, NEVER BEFORE IT. Before it the actor
        // base still carries Fitting Room's own eyes and brows, so the close
        // edge would be comparing the player's work against OUR write and would
        // report a change on every single visit. That is the deletion machine
        // version of this feature, and it is why the call site sits below the
        // handoff switch rather than at the top of Handle.
        //
        // Safe by construction rather than by timing: StandDownPlayerHeadParts
        // writes the base synchronously through ChangeHeadPart and only QUEUES
        // the rebuild, and the queued half writes geometry and materials, not
        // the two quantities read here.
        void TakeOpenReading() {
            g_open = OpenReading{};
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;  // unarmed, and the close edge reads that as kUnarmed
            }
            // The same two fields the offer filter reads, off the actor BASE,
            // through the function that already pairs them.
            const auto key            = HeadPart::OfferKeyFor(player);
            g_open.identityReadable   = key.Valid();
            g_open.race               = key.race;
            g_open.sex                = key.sex;
            g_open.switches           = RaceSwitchSink::PlayerSwitchCount();
            g_open.partsReadable      = key.Valid();
            const auto authorship     = OutfitSession::GetSingleton().DefaultAuthorship();
            const bool authoredPart[kPartCount] = { authorship.hair, authorship.eyes,
                                                    authorship.brows,
                                                    authorship.facialHair };
            for (std::size_t i = 0; i < kPartCount; ++i) {
                g_open.part[i]       = FormIdOf(HeadPart::CurrentFor(player, kKinds[i]));
                g_open.partWrites[i] = HeadPart::WriteCountFor(player, kKinds[i]);
                g_open.defaultAuthoredPart[i] = authoredPart[i];
            }
            const auto colour            = HairColor::CurrentColourFormId(player);
            g_open.colourReadable        = colour.has_value();
            g_open.colour                = colour.value_or(0u);
            g_open.colourWrites          = HairColor::WriteCountFor(player);
            g_open.defaultAuthoredColour = authorship.hairColour;
            g_open.armed                 = true;
            spdlog::info("HeadEditorSink: visit armed. race {:08X} sex {}, hair {:08X}, "
                         "eyes {:08X}, brows {:08X}, beard {:08X}, colour {:08X}{}.",
                         g_open.race, g_open.sex, g_open.part[0], g_open.part[1],
                         g_open.part[2], g_open.part[3], g_open.colour,
                         g_open.colourReadable ? "" : " (unreadable)");
        }

        // Everything the player-facing line needs, so the notification is built
        // from the same verdicts the deletions were.
        const char* PartNameFor(std::size_t a_i) {
            return DefaultLook::PartName(DefaultLook::PartFor(kKinds[a_i]));
        }

        // ⚠ RUNS BEFORE ANY FITTING ROOM WRITE ON THE CLOSE EDGE, and the
        // deletions inside it run before the handoff switch below for a
        // structural reason rather than a tidy one: DrivesPlayerHeadParts and
        // DrivesPlayerHairTint fold DefaultLook::Has and HasHairColour, and
        // they are evaluated as the switch's arguments. Clearing first is what
        // makes the existing re-assert stop having a reason to run, with no new
        // suppression flag anywhere. Where the OUTFIT names a part or a tint,
        // its own terms are untouched and the re-assert still runs, which is
        // "an outfit is a separate deliberate choice" honoured for free.
        void CloseTheVisit() {
            const OpenReading open = g_open;
            // One close consumes one open, whatever happens below. A reading
            // left armed would be graded against the NEXT visit.
            g_open = OpenReading{};

            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            RefreshGate::EditorVisit visit;
            visit.armed            = open.armed;
            const auto key         = HeadPart::OfferKeyFor(player);
            visit.identityReadable = open.identityReadable && key.Valid();
            visit.identityChanged  = key.race != open.race || key.sex != open.sex;
            visit.identitySwitched = RaceSwitchSink::PlayerSwitchCount() != open.switches;

            RefreshGate::LookDimension dims[kPartCount];
            for (std::size_t i = 0; i < kPartCount; ++i) {
                auto& d           = dims[i];
                d.hasDefault      = DefaultLook::Has(player, DefaultLook::PartFor(kKinds[i]));
                d.defaultAuthored = open.defaultAuthoredPart[i];
                d.readable        = open.partsReadable && key.Valid();
                // A COUNT, not a flag: the stand-down on the way in and the
                // hover previews the editor can leave behind are all writes,
                // and only "how many since then" separates ours from theirs.
                d.weWrote = HeadPart::WriteCountFor(player, kKinds[i]) != open.partWrites[i];
                d.before  = open.part[i];
                d.after   = FormIdOf(HeadPart::CurrentFor(player, kKinds[i]));
            }
            RefreshGate::LookDimension colourDim;
            const auto                 colourNow = HairColor::CurrentColourFormId(player);
            colourDim.hasDefault      = DefaultLook::HasHairColour(player);
            colourDim.defaultAuthored = open.defaultAuthoredColour;
            colourDim.readable        = open.colourReadable && colourNow.has_value();
            colourDim.weWrote = HairColor::WriteCountFor(player) != open.colourWrites;
            colourDim.before  = open.colour;
            colourDim.after   = colourNow.value_or(0u);

            visit.hair       = dims[0];
            visit.eyes       = dims[1];
            visit.brows      = dims[2];
            visit.facialHair = dims[3];
            visit.hairColour = colourDim;

            const auto outcome = RefreshGate::ClassifyEditorVisit(visit);
            const RefreshGate::DefaultVerdict partVerdicts[kPartCount] = {
                outcome.hair, outcome.eyes, outcome.brows, outcome.facialHair
            };
            const RefreshGate::BaselineRepair partRepairs[kPartCount] = {
                outcome.repairHair, outcome.repairEyes, outcome.repairBrows,
                outcome.repairFacialHair
            };

            // ⚠ THE REPAIRS GO FIRST. Clearing a hair default without
            // re-pointing its capture sends the ladder to Wear::kOwn, and
            // Restore then writes the pre-editor hair straight over the preset
            // the player just loaded, which is a worse symptom than the bug
            // being fixed. Hair and hair colour get no stand-down on the way in,
            // so at this point their captures still describe the pre-editor
            // character.
            for (std::size_t i = 0; i < kPartCount; ++i) {
                if (partRepairs[i] == RefreshGate::BaselineRepair::kReseed) {
                    HeadPart::ReseedCapture(player, kKinds[i]);
                }
            }
            if (outcome.repairHairColour == RefreshGate::BaselineRepair::kReseed) {
                HairColor::ReseedCaptured(player);
            }

            std::string cleared;
            for (std::size_t i = 0; i < kPartCount; ++i) {
                spdlog::info("HeadEditorSink: default {} - {}. (before {:08X}, after "
                             "{:08X}, ours={}, stored={}, authoring={})",
                             PartNameFor(i), RefreshGate::VerdictName(partVerdicts[i]),
                             dims[i].before, dims[i].after, dims[i].weWrote,
                             dims[i].hasDefault, dims[i].defaultAuthored);
                if (partVerdicts[i] != RefreshGate::DefaultVerdict::kClear) {
                    continue;
                }
                DefaultLook::Clear(player, DefaultLook::PartFor(kKinds[i]));
                cleared = PartNameFor(i);
            }
            spdlog::info("HeadEditorSink: default hair colour - {}. (before {:08X}, after "
                         "{:08X}, ours={}, stored={}, authoring={})",
                         RefreshGate::VerdictName(outcome.hairColour), colourDim.before,
                         colourDim.after, colourDim.weWrote, colourDim.hasDefault,
                         colourDim.defaultAuthored);
            if (outcome.hairColour == RefreshGate::DefaultVerdict::kClear) {
                // A cleared tint clears the default; DefaultLook has no separate
                // entry point for "no longer a default", by design.
                DefaultLook::SetHairColour(player, HairTint{});
                cleared = "hair colour";
            }

            const int cleanup = outcome.ClearedCount();
            spdlog::info("HeadEditorSink: the visit cleared {} default(s) and re-pointed "
                         "{} baseline(s).",
                         cleanup, outcome.ReseededCount());
            if (cleanup == 1) {
                RE::SendHUDMessage::ShowHUDMessage(
                    ("Fitting Room let this character's default " + cleared +
                     " go, so the look you just made stays.")
                        .c_str());
            } else if (cleanup > 1) {
                RE::SendHUDMessage::ShowHUDMessage(
                    ("Fitting Room let " + std::to_string(cleanup) +
                     " of this character's defaults go, so the look you just made stays.")
                        .c_str());
            }
        }

        // The actual work, split out of ProcessEvent so the sink's try/catch
        // stays a one-line wrapper around it (mirrors RaceSwitchSink).
        void Handle(const RE::MenuOpenCloseEvent* a_event) {
            if (!a_event) {
                return;
            }
            const bool isHeadEditor =
                std::string_view{ a_event->menuName.c_str() } == RE::RaceSexMenu::MENU_NAME;
            // Queried BEFORE the pure decision, and only reached for the right
            // menu: DrivesPlayerHairTint takes the session lock, and this sink
            // runs on every menu transition in the game.
            if (!isHeadEditor) {
                return;
            }
            // Before the handoffs below, because it is about the trip rather
            // than about the face, and because a handoff that throws must not
            // leave a free visit behind it.
            if (a_event->opening) {
                TakeLooksMenuFee();
            }

            // ---- OS-219 route B: read RaceMenu's own paint lists ----------
            //
            // ⚠ A SECOND JOB FOR THIS SINK AND IT MUST NOT DISTURB THE FIRST.
            // Everything below is about the player's FACE; this is about the
            // overlay art library and touches neither the actor nor the
            // session. It goes first on the way in so the wrappers are on the
            // movie before any pack's RSM_Initialized handler can run, and it
            // is the last thing on the way out so the visit's verdict, the
            // handoffs and the tint re-assert have all had their turn before
            // this reloads a table and starts a disk scan. See PaintScrape.h
            // for why the capture has to happen during the trip at all.
            if (a_event->opening) {
                PaintScrape::OnRaceMenuOpened();
            }

            // ---- instruments on both edges (field 2026-09-02) --------------
            //
            // What the list wears and what the base's saved layers carry, on
            // the way in before the editor's init has run, and on the way out
            // before anything of ours writes. The head-build hook dumps the
            // same reading after every build the editor makes in between.
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                const char* const edge = a_event->opening ? "editor-open" : "editor-close";
                MakeupApi::DumpWorn(player, edge);
                MakeupApi::DumpSavedLayers(player, edge);
            }
            if (a_event->opening) {
                // The worn-set watch may answer the editor's own init replay
                // for a few seconds; after that the list is the user's.
                MakeupApi::NoteEditorOpened();
            }

            // ---- the exported tint residue, on the way in -------------------
            //
            // ⚠ THE SAME GATE THE HEAD-BUILD DETOUR RUNS, JUST EARLIER AND
            // SYNCHRONOUSLY. The detour cannot rebake on the engine's stack
            // because the head is mid-build there, so it posts the work and
            // the task drains after the frame that build belongs to. r84:
            // five catches, five corrections, one visible frame of the
            // previous character's face in front of each.
            //
            // ⚠⚠ MEASURED r85, AND THIS EDGE IS UPSTREAM OF THE BUILD.
            // Five catches that round, every one of them labelled 'queued
            // head build' and not one labelled 'editor open', with RaceMenu
            // demonstrably open across three of them. The menu-open event
            // fires BEFORE RaceMenu rebuilds the head, so there is nothing
            // bound yet for this call to find and the flash on the way in is
            // not reachable from this edge. Route 2 is answered and it is no.
            //
            // The call stays because it is nearly free, because the ordering
            // belongs to this RaceMenu build rather than to the engine, and
            // because it is now self-reporting: if it ever does catch
            // something, its own label in the log line says so.
            //
            // The one-frame flash itself needs route 1, baking to a
            // per-character path so nothing can inherit the name, which stops
            // the write instead of correcting after it. DEFERRED 2026-08-25,
            // the user's call: the residue is corrected every time and the
            // frame is not worth the change.
            if (a_event->opening) {
                OS::HeadBuildHook::RebakeForeignPresetTintNow("editor open");
            }

            auto& session = OutfitSession::GetSingleton();

            // ---- the visit's verdict, and it goes FIRST on the way out ----
            //
            // Reads the close edge BEFORE any Fitting Room write, and deletes
            // before the switch below reads DefaultLook through its two Drives
            // gates. Both orderings are load bearing; see CloseTheVisit.
            if (!a_event->opening) {
                CloseTheVisit();
            }

            // ---- eye and brow handoff (OS-161) --------------------------
            //
            // ⚠ ACTS IN BOTH DIRECTIONS, unlike the tint below, and the reason
            // is that a head PART is a record on the actor base - the very thing
            // RaceMenu edits. Leaving ours in place means the editor opens
            // showing Fitting Room's eyes as though they were the character's
            // own, and worse, our capture goes on describing the pre-editor
            // face, so a later clear of the outfit would silently undo the
            // user's work in there with no way back.
            //
            // Done FIRST, before the tint, because standing down queues a head
            // rebuild and the tint re-assert wants to be the last write.
            switch (RefreshGate::ClassifyHeadPartHandoff(
                isHeadEditor, a_event->opening, session.DrivesPlayerHeadParts())) {
                case RefreshGate::HeadEditorHandoff::kStandDown:
                    spdlog::info("HeadEditorSink: character editor opening with outfit "
                                 "eyes or brows active - handing the face back.");
                    session.StandDownPlayerHeadParts();
                    break;
                case RefreshGate::HeadEditorHandoff::kReassert:
                    spdlog::info("HeadEditorSink: character editor closed - taking the "
                                 "outfit's eyes and brows back over.");
                    session.ReassertPlayerHeadParts();
                    break;
                case RefreshGate::HeadEditorHandoff::kNone:
                    break;
            }

            // ⚠ AFTER THE STAND-DOWN, WHICH IS THE WHOLE REASON IT IS HERE AND
            // NOT AT THE TOP OF THIS FUNCTION. See TakeOpenReading.
            if (a_event->opening) {
                TakeOpenReading();
            }

            // ⚠ AN if RATHER THAN THE EARLY RETURN IT USED TO BE. The scrape's
            // close edge below has to run on EVERY close, and a return here
            // skipped it for every character whose outfit does not drive a hair
            // colour, which is nearly all of them: the capture would have worked
            // and the file would never have been written.
            if (RefreshGate::ShouldReassertAfterHeadEditor(
                    isHeadEditor, a_event->opening, session.DrivesPlayerHairTint())) {
                spdlog::debug("HeadEditorSink: head editor closed with an outfit hair "
                              "colour active - re-asserting.");
                session.ReassertPlayerHairColor();
            }

            // ---- the look's skin tone across the trip (user 2026-08-24) ----
            //
            // "if i load a looks preset and then later open Racemenu the skin
            // color can get overwritten by racemenu." HeadBuildHook stands the
            // skin-tone re-assert down for as long as that menu is open, which
            // is correct, and this close edge put the hair and the head parts
            // back and never the skin. The editor's own head build restores the
            // tint list from the character's saved layers, so the look's tone is
            // gone by the time it closes.
            //
            // ⚠ AFTER THE HAIR, FOR THE REASON THE HEAD-PART BLOCK ALREADY
            // GIVES: a stand-down queues a head rebuild, and the tint writes
            // want to be the last thing that happens on this edge.
            //
            // ⚠ CLOSE ONLY, AND MakeupApi's HEADER SAYS WHY AN OPEN-EDGE
            // CAPTURE CANNOT REFINE THIS. A no-op when no tone is held, which
            // is the ordinary case.
            if (!a_event->opening) {
                if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                    MakeupApi::NoteHeadEditorClosed(player);
                }
            }

            // ---- the overlay record across the trip (field 2026-09-01) ------
            //
            // RaceMenu is an author of skee's overlay store, and the record has
            // to follow every author or the load-boundary reassert
            // (OverlayBaseline.h) would undo a visit's overlay work with the
            // previous state. Close edge only: the store at close is the truth
            // of what the visit left behind, our own writes included.
            if (!a_event->opening) {
                if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                    OverlayBaseline::SyncFromStore(player);
                    // And the TINT LIST record follows the visit for the same
                    // reason: RaceMenu edits the list in there, and the next
                    // load's reassert must carry the visit's work, not undo it.
                    MakeupBaseline::SyncFromList(player);
                    // And the character's SAVED tint layers follow it last,
                    // after the tone re-assert above has landed: the editor's
                    // own init rebuilds the list from those layers on the next
                    // visit, so they have to say what this visit left behind.
                    MakeupApi::SyncSavedLayers(player, true);
                    MakeupApi::DumpSavedLayers(player, "editor-close, after the mirror");
                    // And the list RaceMenu closed on is remembered as its
                    // copy, which it replays by index inside the next visit;
                    // the worn-set watch answers that replay by content.
                    MakeupApi::NoteEditorClosed(player);
                }
            }

            // Last, for the reason given at the open edge above.
            if (!a_event->opening) {
                PaintScrape::OnRaceMenuClosed();
            }
        }

        struct Sink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*               a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                // BSTEventSink contract: never throw into the engine.
                try {
                    Handle(a_event);
                } catch (const std::exception& e) {
                    spdlog::error("HeadEditorSink threw: {}", e.what());
                } catch (...) {
                    spdlog::error("HeadEditorSink threw a non-standard exception.");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        Sink g_sink;

    }  // namespace

    void Register() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
            spdlog::info("HeadEditorSink: registered (re-assert hair colour after the "
                         "character editor repaints it).");
        }
    }

    void Forget() {
        // ⚠ THE PLAYER IS 0x14 IN EVERY SAVE, which is the same thing that made
        // HeadPart's captures leak across characters. A reading taken before a
        // load describes somebody we are about to stop being, and this one
        // authorises DELETIONS, so leaving it armed is worse than a stale read.
        g_open = OpenReading{};
    }

}  // namespace OS::HeadEditorSink
