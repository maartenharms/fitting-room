#pragma once

#include "CostMode.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace OS::EditorGate {

    // What the Lore-friendly playstyle button should set the cost to.
    //
    // ⚠⚠ THE STONE IS THE ONE PART OF THAT PRESET THAT NEEDS A PLUGIN, and the
    // preset used to hand out a Seamstone economy whether or not the plugin was
    // there. The charge purse itself does work without it, because the charge is
    // our own number in our own co-save and the Refill list only wants soul
    // gems - but the meter then reads "Your Seamstone holds 0 of 5000" to a
    // player who does not own a Seamstone, cannot buy one, and was never told
    // why (user 2026-08-11). Gold is the same bill, per changed slot, in a
    // currency that exists on every install.
    //
    // ⚠ IT IS NOT A DOWNGRADE OF THE PLAYSTYLE. Everything else the preset sets
    // - the collection filter, the colour unlocks, the Seamstone requirement -
    // is untouched, because none of those needs the plugin to mean something.
    // Only the currency moves, and only while the stone is unavailable.
    //
    // ⚠ AND bRequireSeamstone STAYS ON REGARDLESS, deliberately. It is already
    // AND-ed with the ESP being loaded (LoreModule::RequirementActive), so it
    // is inert rather than wrong, the settings panel already says so out loud,
    // and leaving it set means the requirement is simply there the day the
    // player installs the plugin instead of silently absent.
    [[nodiscard]] inline constexpr CostMode LorePresetCostMode(bool a_seamstoneAvailable) {
        return a_seamstoneAvailable ? CostMode::kCharge : CostMode::kGold;
    }

    // What the editor hotkey should do this press.
    enum class GateAction { kIgnore, kClose, kOpen, kNeedContext, kNeedSeamstone };

    // Which surface the editor body is showing. One mode at a time, which is
    // the enum's whole job: the pair of bools it replaced could hold two modes
    // at once, and every button had to clear the other one by hand.
    //
    // ⚠ DECLARED HERE rather than in EditorUI.cpp's anonymous namespace so
    // PlanPaneTransition below can be covered by tests/test_editorgate.cpp.
    // Nothing in this file may pull in an engine type; that is the whole reason
    // the suite can run at all. EditorUI.cpp aliases it straight back in, so
    // every PaneMode:: site there reads unchanged.
    enum class PaneMode : std::uint8_t {
        kStyles  = 0,
        kPresets = 1,
        kDye     = 2,
        kRules   = 3,
        kBodyStudio = 4,
        kShape      = 5,
        kOverlays   = 6,
        kProfiles   = 7,  // the Looks page (W2); "Profiles" in code, per spec
    };

    // Whether a transition changes the preset preview suppression, and which
    // way.
    //
    // ⚠ A TRI-STATE AND NOT TWO BOOLS. SetPresetPreviewSuppression takes one
    // argument, and a pair of independent flags would let a caller ask for on
    // and off at once. kUnchanged means do not call it at all, which is worth
    // having: the setter early-returns on an unchanged value but still takes a
    // lock to find that out.
    enum class PreviewSuppression { kUnchanged, kOn, kOff };

    // What switching the editor from one pane mode to another has to do.
    struct PaneTransition {
        bool               clearShowcaseSel    = false;
        bool               requestShowcaseTabs = false;
        PreviewSuppression preview             = PreviewSuppression::kUnchanged;
        bool               restageEditBuffer   = false;
        bool               armDyeDump          = false;
    };

    // ⚠ THIS IS NOT A CONVENIENCE WRAPPER. With Styles a real entry in the mode
    // rail, every ordered pair of modes is one click apart. The per-button code
    // this replaces only ever knew the paths its own button could take, which
    // is how the Dye button came to leave Presets without clearing the preview
    // suppression: it assigned g_paneMode directly, and only the Rules and
    // Presets buttons carried a leave-Presets branch.
    //
    // Rules enter and leave are deliberately absent. They stay with the
    // g_rulesModeApplied detector in EditorUI::Draw, which catches a case no
    // click path can: OnOpen re-stages unconditionally, so reopening the editor
    // while Rules was showing has to discard again.
    //
    // ⚠ BOTH SWITCHES CARRY NO default: LABEL. The build sets /we4062, so a
    // fifth mode becomes a build error listing every site rather than a silent
    // fallthrough. That is the technique that found nine DyeTarget sites when
    // the OS-127 plan had listed six.
    [[nodiscard]] inline PaneTransition PlanPaneTransition(PaneMode a_from,
                                                           PaneMode a_to) {
        PaneTransition t{};
        if (a_from == a_to) {
            return t;
        }

        switch (a_from) {
            case PaneMode::kPresets:
                t.clearShowcaseSel = true;
                t.preview          = PreviewSuppression::kOff;
                // ⚠ NO RESTAGE ON THE WAY INTO RULES. The rules-mode detector
                // discards staging on the next frame anyway, so re-asserting
                // the edit preview here would buy a character rebuild only to
                // undo it.
                t.restageEditBuffer = a_to != PaneMode::kRules;
                break;
            case PaneMode::kStyles:
            case PaneMode::kDye:
            case PaneMode::kRules:
            case PaneMode::kBodyStudio:
            // ⚠ NOTHING TO UNDO ON THE WAY OUT OF SHAPE, and that is a
            // property of the feature rather than an omission. The page has no
            // staged edit to discard: a slider writes straight through to
            // RaceMenu's node transforms, which are the storage, so leaving is
            // the same as never having arrived.
            // ⚠ AND NOTHING TO UNDO ON THE WAY OUT OF OVERLAYS EITHER, for
            // exactly the reason above it. A texture, a tint and an alpha are
            // written straight through as node overrides, and the override is
            // the storage.
            // ⚠ AND NOTHING ON THE WAY OUT OF PROFILES. Save is an explicit
            // press through ProfileStore and Apply closes the editor itself,
            // so the page can hold no staged edit for a page change to
            // strand.
            case PaneMode::kOverlays:
            case PaneMode::kShape:
            case PaneMode::kProfiles:
                break;
        }

        switch (a_to) {
            case PaneMode::kPresets:
                t.clearShowcaseSel    = true;
                t.requestShowcaseTabs = true;
                t.preview             = PreviewSuppression::kOn;
                break;
            case PaneMode::kDye:
                // Armed on the mode switch and not at editor open: opening runs
                // a refresh, which would consume the one shot before the user
                // has dyed anything. A mode switch runs no refresh, so the
                // arming survives until the first Push.
                t.armDyeDump = true;
                break;
            case PaneMode::kStyles:
            case PaneMode::kRules:
            case PaneMode::kBodyStudio:
            case PaneMode::kShape:
            case PaneMode::kOverlays:
            case PaneMode::kProfiles:
                break;
        }
        return t;
    }

    // Should leaving this page stop and ask first?
    //
    // ⚠ THE PAGE BEING LEFT OWNS THE ANSWER, and the mode pair is here so it
    // can be a property of the transition rather than of the page alone. Two
    // pages in this editor hold an edit that leaving would strand: Shape holds
    // the state its Discard button puts back, and Body Studio holds a preset
    // draft. Everything else is either committed as you go or staged on the
    // outfit, which survives a page change untouched.
    //
    // ⚠ NOT A DIRTY CHECK ON THE OUTFIT. The Styles and Dye pages leave their
    // staging in place across a page change on purpose, so warning about it
    // would fire on every click of the rail and teach the player to dismiss the
    // one warning that matters.
    //
    // a_toSameMode is folded in so a rail click on the page you are already on
    // never asks; PlanPaneTransition returns an empty plan for that pair and
    // this has to agree with it.
    [[nodiscard]] inline constexpr bool ShouldWarnOnLeave(bool a_dirty, PaneMode a_from,
                                                          PaneMode a_to) {
        return a_dirty && a_from != a_to;
    }

    // Pure decision for the editor hotkey.
    //   isOpen       - the editor is currently open
    //   wantsText    - an ImGui text field has focus (typing the hotkey letter
    //                  must not close the editor)
    //   canOpenHere  - a permitted context is open (inventory OR Screen Archer Menu)
    //   seamstoneOk  - the lore gate is satisfied (no requirement, or stone held)
    //
    // ⚠ THE SEAMSTONE IS ASKED BEFORE THE CONTEXT, AND THE ORDER IS A PRIVACY
    // RULE RATHER THAN A PREFERENCE. The two refusals are not equal: the context
    // one puts a line on screen that names this mod, and the seamstone one is
    // deliberately SILENT, because a player who has never found the Seamstone
    // should not learn it exists by pressing a key they may have hit by
    // accident. Asking the context first meant that player was told anyway, from
    // anywhere outside an inventory, which defeated the silence entirely (user
    // 2026-08-07). Whoever cannot open the editor at all is told nothing at all.
    //   gameWantsText - the GAME is taking typed text, so the key is a letter
    //                   somebody is entering rather than a hotkey
    //
    // ⚠ gameWantsText IS ASKED FIRST AND ANSWERS EVERYTHING. The editor key is a
    // printable letter, so while the console is up it is being typed, and every
    // other answer this function can give is wrong there: opening the editor
    // over the console, closing one the player wants kept, or putting "open your
    // inventory" on screen at somebody mid-command (user 2026-08-08, pressing Y
    // in the console). wantsText below is the same rule for OUR text fields; the
    // two are separate inputs because they come from different places and the
    // game's version has to hold even when the editor is shut.
    [[nodiscard]] inline GateAction DecideGate(bool isOpen, bool wantsText,
                                               bool canOpenHere, bool seamstoneOk,
                                               bool gameWantsText) {
        if (gameWantsText) {
            return GateAction::kIgnore;
        }
        if (isOpen) {
            return wantsText ? GateAction::kIgnore : GateAction::kClose;
        }
        if (!seamstoneOk) {
            return GateAction::kNeedSeamstone;
        }
        if (!canOpenHere) {
            return GateAction::kNeedContext;
        }
        return GateAction::kOpen;
    }

    // Should the dye pane's Special fold be forced shut this frame?
    //
    // ⚠⚠ A SECTION THAT CAN VANISH MUST NOT COME BACK HOLDING THE STATE IT HAD
    // BEFORE IT WENT AWAY, and that is the whole bug. The Special fold is only
    // submitted on a dyed stripe in free-form, so an unset stripe hides it
    // outright - but the host keeps a collapsing header's open state keyed by
    // its id whether or not it was drawn that frame. Leave it open, select
    // something that hides it, then put a colour on: the section reappears
    // already expanded, with no click anywhere near it. "Sometimes" is the
    // signature of exactly that (user 2026-08-11); it happened when, and only
    // when, it had been left open earlier.
    //
    // ⚠ AND A PICK CLOSES IT TOO, which the same report asked for outright: a
    // metallic dye writes every field that section edits, so what is on screen
    // when it lands is a description of the colour BEFORE the click. Closing is
    // the honest state for a panel whose contents were just replaced by
    // something the player chose somewhere else.
    //
    // The caller owns last frame's visibility, the same shape DyeSectionHeader's
    // own open/closed cue uses and for the same reason: the host reports the
    // state a control is IN, every frame, never the edge that changed it.
    [[nodiscard]] inline constexpr bool ForceSpecialFoldClosed(bool a_visibleNow,
                                                              bool a_visibleBefore,
                                                              bool a_colourPicked) {
        // ⚠ NOTHING TO SAY WHILE IT IS NOT ON SCREEN. Forcing a header that is
        // not submitted would spend the request on a frame that cannot use it,
        // and the reappearance it exists to catch would sail through.
        return a_visibleNow && (!a_visibleBefore || a_colourPicked);
    }

    // Reconcile the dirty flag against what actually differs from the committed
    // outfit. Editing something back to its committed value undoes the edit, so
    // Apply and the "unsaved changes" status have to go quiet again rather than
    // sitting enabled at a cost of zero.
    //
    // ⚠ EVERY dimension the editor can stage has to appear here. Only changed
    // SLOTS are priced on the lore-mode Apply bill, which makes it tempting to
    // key the flag on that count alone, and that has now been the same bug
    // four times: a dimension with zero changed slots gets its flag
    // force-cleared the same frame Push() set it, so Apply stays greyed out and
    // the setting can never be committed. It shipped that way for body presets
    // ("Body is not applied / no Apply to press"), then again for hair
    // visibility and hair colour, and dye arrived with the same hole. Pricing
    // still uses the slot count on its own, so a body, hair or dye edit remains
    // free.
    //
    // ⚠ NO DEFAULT ARGUMENT ON ANY DIMENSION. A defaulted false is exactly how
    // the next dimension gets forgotten again: every existing call site keeps
    // compiling and the new flag silently reads false. Adding a parameter here
    // must break the build at each caller.
    //
    // A predicate with a test rather than an inline expression precisely
    // because the failure is silent: nothing crashes, nothing logs, the button
    // is just dead.
    [[nodiscard]] inline constexpr bool StillDirty(bool a_pushed,
                                                   std::uint32_t a_changedSlots,
                                                   bool a_bodyEdited, bool a_hairEdited,
                                                   bool a_dyeEdited) {
        return a_pushed &&
               (a_changedSlots > 0 || a_bodyEdited || a_hairEdited || a_dyeEdited);
    }

    // Free-form saves as you go; lore-friendly keeps its Apply button. The
    // split is not the playstyle label, it is whether the edit is being billed:
    // Apply is a deliberate press precisely because it charges the player, so
    // the button earns its place exactly when there is a price on it.
    //
    // Derived rather than stored. A bAutoApply setting would be a fourth
    // playstyle field, and the ⚠ in SettingsUI's preset block is there because
    // a playstyle field that one button sets and the other forgets silently
    // means different things depending on what was clicked before -
    // bCollectionOnly already shipped that bug. A value computed from
    // `charging` cannot drift out of step with the playstyle that owns it.
    //
    //   a_dirty        - there is a staged edit worth writing
    //   a_charging     - a cost is being billed per changed slot: the mode's
    //                    own rate is above zero. Currency-agnostic, so gold and
    //                    the Seamstone's charge both arrive here as true
    //   a_pendingCost  - what THIS edit would actually be billed, in whichever
    //                    currency the mode uses. Zero means free
    //   a_readOnly     - the slot panel is disabled: an "(away)" follower, or
    //                    the Equipped view where no outfit is active
    //   a_widgetActive - a control is still being dragged
    //
    // ⚠ a_charging is the hard one. Apply consults cantAfford before it bills;
    // a path with no button consults nothing, so auto-applying under a charge
    // would drain gold on every click and keep going past zero. Never commit
    // on its own while the session charges A PRICE.
    //
    // ⚠ AND THE PRICE IS THE TEST, NOT THE MODE. Lore-friendly bills styled
    // slots and dyed channels; body, hair, hair colour and putting real gear
    // back are all free in it, and making the player press Apply to keep a free
    // edit is the ceremony ShouldAutoApply exists to remove (user 2026-08-08:
    // "in lore mode we don't need to apply changes that didn't cost anything").
    // A zero bill cannot drain a purse, so the paragraph above is satisfied by
    // the cost rather than by the mode: the only thing it was ever protecting
    // is money leaving without a press.
    //
    // ⚠ THIS DOES NOT HIDE THE APPLY BUTTON. Its visibility stays keyed on
    // `charging` alone, because PendingCost is zero whenever nothing is staged
    // and a button that came and went with every edit would be worse than one
    // that greys out. Free edits simply commit and leave it grey.
    //
    // ⚠ a_readOnly is what stops auto-apply inventing an outfit. Committing
    // runs EnsureActiveOutfit, which creates one when none is active; today
    // that state also disables the whole slot panel, so no edit can reach here
    // to begin with. This keeps the guarantee if that ever stops being true.
    //
    // ⚠ a_widgetActive is the debounce. Committing persists to outfits.json,
    // and the hair colour picker reaches Push() continuously while dragged, so
    // without this a single drag rewrites the file for every byte it crosses.
    // Same lesson the rules tab learned when dragging wrote rules.json ten
    // times in twenty seconds.
    [[nodiscard]] inline constexpr bool ShouldAutoApply(bool a_dirty, bool a_charging,
                                                        std::uint64_t a_pendingCost,
                                                        bool          a_readOnly,
                                                        bool          a_widgetActive) {
        return a_dirty && !(a_charging && a_pendingCost > 0) && !a_readOnly &&
               !a_widgetActive;
    }

    // Whether a slot row should say its style has nothing underneath it.
    //
    // A style that cannot render is otherwise invisible in the editor: the row
    // names the piece, the browser shows it picked, and the character simply
    // does not wear it, with nothing on screen connecting the two. That is the
    // same complaint an unmarked rule card drew - a state signalled only by
    // absence reads as broken.
    //
    // ⚠ Silent about a style whose plugin is MISSING, even though that one
    // cannot render either. That row already says so, and a second explanation
    // sends the player hunting for gear to equip when the real fix is to
    // install a mod. One row, one cause.
    //
    // a_canApplyStyle is CanApplyStyleBit's verdict, so this covers the shield
    // whether or not bRequireWornForStyles is on: a shield style with no shield
    // carried has never rendered, and has never said why.
    [[nodiscard]] inline constexpr bool ShouldWarnNoWornGear(bool a_hasStyle,
                                                             bool a_styleResolves,
                                                             bool a_canApplyStyle) {
        return a_hasStyle && a_styleResolves && !a_canApplyStyle;
    }

    // Whether a slot row should say its style shows in the fitting room only.
    //
    // The worn rule (bRequireWornForStyles) stands down for the actor being
    // staged while the editor is open, so a piece with nothing worn under it
    // draws here and vanishes the moment the editor shuts. The row is the
    // place to say so before that happens. a_canApplyHere and
    // a_canApplyOutside are CanApplyStyleBit's two verdicts, previewing and
    // not. A piece refused both ways is "nothing underneath" and that row
    // already says it (one row, one cause); a piece drawn both ways has
    // nothing to explain.
    [[nodiscard]] inline constexpr bool ShouldWarnPreviewOnly(bool a_hasStyle,
                                                              bool a_styleResolves,
                                                              bool a_canApplyHere,
                                                              bool a_canApplyOutside) {
        return a_hasStyle && a_styleResolves && a_canApplyHere && !a_canApplyOutside;
    }

    // The weapon side of the same sentence: whether a weapon row should say the
    // character is not carrying one this mod can reach.
    //
    // ⚠ "THE ENGINE HAS ONE", NOT "THEY OWN ONE", and the distinction is the
    // whole reason this exists. Fitting Room restyles by rebuilding the object
    // the engine attached, so its reach ends exactly where the engine's does. A
    // follower's SPARE weapon is drawn by a display mod (Immersive Equipment
    // Displays puts an unequipped bow on the skeleton's WeaponBow node) and
    // never enters BipedAnim::objects at all, so there is nothing to rebuild
    // and a staged style is a silent no-op. Jenassa's bow was reported twice as
    // "the transmog is not working" before the log said the slot was empty
    // (2026-08-08).
    //
    // ⚠ SILENT ABOUT A STYLE WHOSE PLUGIN IS MISSING, for the reason its
    // armour sibling gives: that row already says so, and one row gets one
    // cause.
    [[nodiscard]] inline constexpr bool ShouldWarnNoEngineWeapon(
        bool a_hasStyle, bool a_styleResolves, bool a_engineHasClass) {
        return a_hasStyle && a_styleResolves && !a_engineHasClass;
    }

    // A Fitting Room window opened over SAM cannot outlive that host menu. If
    // SAM closes for any reason, its close event closes the editor as the same
    // transaction rather than leaving a hosted window stranded over gameplay.
    //
    // ⚠ THIS IS THE SAFETY NET, NOT THE ESCAPE PATH. It used to be both, and
    // that is the bug ShouldConsumeHostEscape below fixes: Escape reached SAM,
    // SAM closed, this fired, and one keypress took down two menus.
    [[nodiscard]] inline constexpr bool ShouldCloseForLostHost(
        bool a_openedFromSam, bool a_samStillOpen) {
        return a_openedFromSam && !a_samStillOpen;
    }

    // Whether the editor should swallow this Escape rather than let the host
    // menu have it.
    //
    // ⚠ THE CAMERA PASSTHROUGH IS WHY THIS IS NEEDED AT ALL. Hosted over SAM,
    // the window sets kPassInputToGame whenever the cursor is off the panel, so
    // SAM keeps receiving drag, wheel and stick as one coherent gesture. That
    // flag is all or nothing per window, so the keyboard rides along with the
    // mouse and Escape reached SAM too. SAM closed itself, ShouldCloseForLostHost
    // then closed the editor, and both menus went down on one press when the
    // user expected to be handed back to SAM (field 2026-08-07).
    //
    // The fix cannot be to narrow the passthrough. Gating it on a held button
    // instead of hover is what the OS-80c notes in EditorWindow.cpp already
    // record failing: the gate opens on the press it is meant to admit, so the
    // first press is always eaten and a drag half-starts. So the passthrough
    // stays exactly as it is and one key is taken back, upstream, by a menu
    // handler.
    //
    // ⚠ ONLY WHEN HOSTED BY SAM. In the inventory the editor is modal and
    // nothing downstream is listening for Escape anyway, and with the editor
    // shut Escape has to reach SAM or the user cannot close it.
    //
    // ⚠ NOT WHILE TYPING. The first Escape in a text field unfocuses it, which
    // is native behaviour and is the widget's key rather than ours; taking it
    // here would close the whole editor out from under a half-typed name.
    [[nodiscard]] inline constexpr bool ShouldConsumeHostEscape(bool a_editorOpen,
                                                                bool a_openedFromSam,
                                                                bool a_wantsText) {
        return a_editorOpen && a_openedFromSam && !a_wantsText;
    }

    // Whether the editor window's OWN Escape handler should act on this press.
    //
    // ⚠ FLICK'S PANEL IS A WINDOW ON TOP OF OURS AND ESCAPE BELONGS TO IT.
    // Both windows draw in the same ImGui frame off the same key state, so one
    // press reached FLICK's own close and ours, and opening FLICK's settings
    // over the editor and pressing Escape once took both down (user
    // 2026-08-08). The editor stands down while FLICK's menu owns the key; the
    // NEXT press finds the menu shut and closes the editor, which is the two
    // presses the player already expects for two windows.
    //
    // ⚠ TWO FRAMES, AND THAT IS NOT BELT AND BRACES. Nothing fixes the order
    // in which FLICK closes its own menu relative to our Draw within a frame.
    // If it closes FIRST, the live flag is already false on the press we must
    // refuse, and only last frame's value still remembers. If it closes AFTER,
    // the live flag catches it. Testing one of the two is a coin flip on
    // somebody else's call order.
    //
    // ⚠ IT COSTS NOTHING WHEN FLICK IS SHUT. Both terms are false in the
    // ordinary case, so the Escape that closes the editor from the editor is
    // untouched.
    [[nodiscard]] inline constexpr bool ShouldCloseOnOwnEscape(
        bool a_flickMenuOpen, bool a_flickMenuOpenLastFrame) {
        return !a_flickMenuOpen && !a_flickMenuOpenLastFrame;
    }

    // A showcase preset may become a saved outfit only when there is a valid
    // selection, the library has room, and lore-friendly mode's collection
    // gate is satisfied. Free-form mode deliberately ignores ownership.
    [[nodiscard]] inline bool CanSaveShowcase(bool a_haveSelection, bool a_libraryFull,
                                              bool a_collectionOnly,
                                              bool a_ownsEveryPiece) {
        return a_haveSelection && !a_libraryFull &&
               (!a_collectionOnly || a_ownsEveryPiece);
    }

    // Playstyle owns style visibility completely: free-form sees every
    // installed look, lore-friendly sees collected looks. There is no
    // session/settings override that can put either mode into a contradictory
    // state.
    [[nodiscard]] inline constexpr bool BrowseCollectedOnly(
        bool a_loreFriendly) {
        return a_loreFriendly;
    }

    // Preset import writes into the CURRENT target library. Naming that target
    // on the action prevents "my outfits" from implying the player while a
    // follower is selected.
    [[nodiscard]] inline constexpr const char* PresetSaveLabel(
        bool a_targetIsPlayer) {
        return a_targetIsPlayer ? "Save to player outfits"
                                : "Save to follower outfits";
    }

    // A readable name for a DirectInput scan code (DIK), for the rebind display.
    // Covers the keys anyone would bind; unlisted codes fall back to "Key 0xNN".
    // 0 = unbound.
    [[nodiscard]] inline std::string DikName(std::uint32_t a_dik) {
        switch (a_dik) {
            case 0x00: return "(unbound)";
            case 0x01: return "Esc";
            case 0x0E: return "Backspace";
            case 0x0F: return "Tab";
            case 0x1C: return "Enter";
            case 0x1D: return "L-Ctrl";
            case 0x2A: return "L-Shift";
            case 0x38: return "L-Alt";
            case 0x39: return "Space";
            case 0x1E: return "A"; case 0x30: return "B"; case 0x2E: return "C";
            case 0x20: return "D"; case 0x12: return "E"; case 0x21: return "F";
            case 0x22: return "G"; case 0x23: return "H"; case 0x17: return "I";
            case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
            case 0x32: return "M"; case 0x31: return "N"; case 0x18: return "O";
            case 0x19: return "P"; case 0x10: return "Q"; case 0x13: return "R";
            case 0x1F: return "S"; case 0x14: return "T"; case 0x16: return "U";
            case 0x2F: return "V"; case 0x11: return "W"; case 0x2D: return "X";
            case 0x15: return "Y"; case 0x2C: return "Z";
            case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
            case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
            case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x3B: return "F1"; case 0x3C: return "F2"; case 0x3D: return "F3";
            case 0x3E: return "F4"; case 0x3F: return "F5"; case 0x40: return "F6";
            case 0x41: return "F7"; case 0x42: return "F8"; case 0x43: return "F9";
            case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
            default: {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "Key 0x%X", a_dik);
                return std::string(buf);
            }
        }
    }

    // Keys offered in the settings-panel hotkey dropdown (DIK, name). Names
    // match DikName. A dropdown avoids raw key capture, which fails while the
    // SKSE Menu Framework overlay owns the keyboard.
    struct KeyOption {
        std::uint32_t dik;
        const char*   name;
    };
    inline constexpr KeyOption kBindableKeys[] = {
        { 0x00, "(unbound)" },
        { 0x18, "O" }, { 0x19, "P" }, { 0x25, "K" }, { 0x26, "L" },
        { 0x24, "J" }, { 0x23, "H" }, { 0x16, "U" }, { 0x17, "I" },
        { 0x15, "Y" }, { 0x31, "N" }, { 0x30, "B" }, { 0x2F, "V" },
        { 0x22, "G" }, { 0x21, "F" }, { 0x14, "T" }, { 0x13, "R" },
        { 0x2E, "C" }, { 0x10, "Q" }, { 0x12, "E" },
        { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
        { 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" },
        { 0x43, "F9" }, { 0x44, "F10" }, { 0x57, "F11" }, { 0x58, "F12" },
    };

}  // namespace OS::EditorGate
