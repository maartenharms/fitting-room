// Pure-logic tests for the editor hotkey gate. No engine, no RE:: types.
#include "EditorGate.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::EditorGate;

    {  // DecideGate: open editor closes unless a text field is focused
        CHECK(DecideGate(true, false, true, true, false) == GateAction::kClose);
        CHECK(DecideGate(true, true, true, true, false) == GateAction::kIgnore);
    }
    {  // The game is taking typed text, so the key is a letter and not a hotkey.
        // ⚠ EVERY OTHER ANSWER IS WRONG WHILE THE CONSOLE IS UP, which is why
        // this is checked across all four of the remaining inputs rather than in
        // the one combination that was reported. Pressing the editor key in the
        // console put "open your inventory" on screen mid-command (user
        // 2026-08-08); the same press must also not open the editor over the
        // console, and must not close an editor the player still wants.
        for (bool isOpen : { false, true }) {
            for (bool wantsText : { false, true }) {
                for (bool canOpenHere : { false, true }) {
                    for (bool seamstoneOk : { false, true }) {
                        CHECK(DecideGate(isOpen, wantsText, canOpenHere, seamstoneOk,
                                         /*gameWantsText*/ true) == GateAction::kIgnore);
                    }
                }
            }
        }
        // And it is the ONLY thing suppressed by: the same press with nothing
        // typing still behaves exactly as it did.
        CHECK(DecideGate(false, false, true, true, false) == GateAction::kOpen);
        CHECK(DecideGate(true, false, true, true, false) == GateAction::kClose);
    }
    {  // DecideGate: closed editor needs the seamstone, then a context, then opens
        CHECK(DecideGate(false, false, false, true, false) == GateAction::kNeedContext);
        CHECK(DecideGate(false, false, true, false, false) == GateAction::kNeedSeamstone);
        CHECK(DecideGate(false, false, true, true, false) == GateAction::kOpen);
        // ⚠ THE SEAMSTONE WINS OVER A MISSING CONTEXT, and the order is a
        // privacy rule rather than a preference. kNeedContext puts a line on
        // screen naming the mod; kNeedSeamstone is deliberately silent, because
        // a player who has never found the Seamstone should not learn it exists
        // by pressing a key. Checking the context first meant that player got
        // told anyway, from anywhere outside an inventory (user 2026-08-07).
        CHECK(DecideGate(false, false, false, false, false) == GateAction::kNeedSeamstone);
    }
    {  // An editor opened from SAM must close if Escape removes its host menu
        CHECK(!ShouldCloseForLostHost(false, false));
        CHECK(!ShouldCloseForLostHost(true, true));
        CHECK(ShouldCloseForLostHost(true, false));
    }
    {  // DikName: named keys + unbound + hex fallback
        CHECK(DikName(0x18) == "O");
        CHECK(DikName(0x39) == "Space");
        CHECK(DikName(0x3B) == "F1");
        CHECK(DikName(0x00) == "(unbound)");
        CHECK(DikName(0xFF) == "Key 0xFF");
    }
    {  // showcase imports obey the collection only in lore-friendly mode
        CHECK(!CanSaveShowcase(false, false, false, false));
        CHECK(!CanSaveShowcase(true, true, false, true));
        CHECK(CanSaveShowcase(true, false, false, false));
        CHECK(!CanSaveShowcase(true, false, true, false));
        CHECK(CanSaveShowcase(true, false, true, true));
    }
    {  // playstyle, not a second toggle, owns catalog visibility
        CHECK(!BrowseCollectedOnly(false));  // free-form: everything installed
        CHECK(BrowseCollectedOnly(true));    // lore-friendly: collected only
    }
    {  // preset import names the library it will actually mutate
        CHECK(std::string(PresetSaveLabel(true)) == "Save to player outfits");
        CHECK(std::string(PresetSaveLabel(false)) == "Save to follower outfits");
    }

    {  // the dirty flag has to survive every dimension the editor can stage
        using OS::EditorGate::StillDirty;

        // Nothing pushed, nothing dirty, whatever else is true.
        CHECK(!StillDirty(false, 3, true, true, true));
        // A slot edit is the ordinary case.
        CHECK(StillDirty(true, 1, false, false, false));
        // Editing a slot back to its committed value stands the flag down
        // again; this is the Hide->Show round trip that Apply used to survive
        // at "0 gold" with the status stuck on "Unsaved changes".
        CHECK(!StillDirty(true, 0, false, false, false));

        // ⚠ The regression this predicate exists for. Each of these dimensions
        // has zero changed SLOTS, so keying the flag on the slot count alone
        // force-cleared it the same frame Push() set it and left Apply greyed
        // out with no way to commit. It shipped for body once, then for hair
        // and hair colour again, and dye was built with the same hole.
        CHECK(StillDirty(true, 0, true, false, false));   // body preset / ORefit only
        CHECK(StillDirty(true, 0, false, true, false));   // hair visibility or colour only
        CHECK(StillDirty(true, 0, false, false, true));   // dye only, no slot changed
        CHECK(StillDirty(true, 0, true, true, true));     // all three, still no slots
    }

    {  // free-form commits every edit on its own; a priced edit still needs Apply
        using OS::EditorGate::ShouldAutoApply;

        // Nothing staged, nothing to write.
        CHECK(!ShouldAutoApply(false, false, 0, false, false));
        // The ordinary free-form edit: click a slot, it is saved.
        CHECK(ShouldAutoApply(true, false, 0, false, false));

        // ⚠ THE RULE THIS PREDICATE EXISTS FOR. Apply is a deliberate button
        // because it bills the player, so a session that charges must never
        // commit on its own. Auto-applying with the charge live would subtract
        // gold on every slot click, and the cantAfford floor that normally
        // blocks Apply is not consulted on a path with no button - so it would
        // drain past zero silently. Lore-friendly keeps its Apply button.
        CHECK(!ShouldAutoApply(true, true, 40, false, false));

        // ⚠ AND THE OTHER HALF OF THAT RULE, which shipped missing: a charging
        // session whose PENDING BILL IS ZERO has nothing to authorise. Body,
        // hair, hair colour and putting real gear back are all free in
        // lore-friendly, so making the player press Apply to keep one was pure
        // ceremony - the same ceremony free-form dropped. A zero bill cannot
        // drain a purse, which is the only thing the rule above protects.
        CHECK(ShouldAutoApply(true, true, 0, false, false));
        // One gold is still a price and still needs the press. The test is
        // cost > 0, never a threshold.
        CHECK(!ShouldAutoApply(true, true, 1, false, false));
        // A cost with no charging mode cannot happen (PendingCost returns 0
        // when nothing is billed), and if it ever did, free-form still saves as
        // it goes: the mode is what decides, the cost only ever excuses.
        CHECK(ShouldAutoApply(true, false, 500, false, false));

        // Read-only covers both of the editor's uncommittable states: an
        // "(away)" follower whose actor is not loaded, and the Equipped view
        // where no outfit is active. The second is why auto-apply can never
        // invent an outfit nobody asked for - with no active outfit the whole
        // slot panel is disabled, so there is no edit to commit in the first
        // place, and this keeps that true if the panel ever stops being.
        CHECK(!ShouldAutoApply(true, false, 0, true, false));

        // A live widget defers the write until the mouse comes up. The hair
        // colour picker is the one continuous control that reaches Push(), and
        // committing per frame during a drag would rewrite outfits.json for
        // every quantized byte the drag passes through.
        CHECK(!ShouldAutoApply(true, false, 0, false, true));
        // Released, the same drag commits once.
        CHECK(ShouldAutoApply(true, false, 0, false, false));
        // ⚠ AND THE DEBOUNCE OUTRANKS THE FREE PASS. A free lore-friendly edit
        // now commits on its own, so the drag guard is the only thing left
        // between the hair colour picker and a write per frame.
        CHECK(!ShouldAutoApply(true, true, 0, false, true));
    }

    {  // a slot row warns "nothing underneath" only when that is the real reason
        using OS::EditorGate::ShouldWarnNoWornGear;

        // No style on the slot: nothing to fail to render.
        CHECK(!ShouldWarnNoWornGear(false, false, false));
        CHECK(!ShouldWarnNoWornGear(false, true, false));
        // A style that renders needs no warning.
        CHECK(!ShouldWarnNoWornGear(true, true, true));
        // A style that resolves and cannot apply is exactly the case.
        CHECK(ShouldWarnNoWornGear(true, true, false));
    }

    {  // a slot row says "fitting room only" when the preview alone draws it
        using OS::EditorGate::ShouldWarnPreviewOnly;

        // No style, or one whose plugin is missing: nothing to say.
        CHECK(!ShouldWarnPreviewOnly(false, false, true, false));
        CHECK(!ShouldWarnPreviewOnly(true, false, true, false));
        // Draws everywhere: no note.
        CHECK(!ShouldWarnPreviewOnly(true, true, true, true));
        // Draws nowhere: that is "nothing underneath", not this note.
        CHECK(!ShouldWarnPreviewOnly(true, true, false, false));
        // Draws here and not outside: the case.
        CHECK(ShouldWarnPreviewOnly(true, true, true, false));
        // Refused here and allowed outside is not a verdict the gate can
        // give, and the note stays quiet rather than guess.
        CHECK(!ShouldWarnPreviewOnly(true, true, false, true));
    }

    {  // one Escape closes one window, and FLICK's panel is first in the queue
        using OS::EditorGate::ShouldCloseOnOwnEscape;

        // The ordinary case: FLICK is shut, the editor owns the key.
        CHECK(ShouldCloseOnOwnEscape(false, false));
        // FLICK's panel is up right now. The press is its business.
        CHECK(!ShouldCloseOnOwnEscape(true, false));
        // ⚠ AND THE FRAME AFTER IT SHUTS, which is the case a one-frame test
        // would miss. Nothing pins whether FLICK closes its menu before or
        // after our Draw within a frame, so on the press that closes it the
        // live flag may already read false and only the previous frame still
        // remembers. Both orderings have to refuse the same press.
        CHECK(!ShouldCloseOnOwnEscape(false, true));
        CHECK(!ShouldCloseOnOwnEscape(true, true));
    }

    {  // a weapon row says when the character carries nothing we can reach
        using OS::EditorGate::ShouldWarnNoEngineWeapon;

        // No style staged on the class: nothing to fail to render.
        CHECK(!ShouldWarnNoEngineWeapon(false, false, false));
        CHECK(!ShouldWarnNoEngineWeapon(false, true, false));
        // A style over a weapon the engine really did attach is fine.
        CHECK(!ShouldWarnNoEngineWeapon(true, true, true));
        // ⚠ THE CASE THIS EXISTS FOR. Jenassa carries a bow and has an Iron
        // Sword equipped. The engine attaches only the sword, so biped slot 38
        // is empty, the rebuild has nothing to rebuild, and an Ebony Bow style
        // staged on the row rendered nothing and said nothing.
        CHECK(ShouldWarnNoEngineWeapon(true, true, false));
        // A style whose plugin is gone keeps its own explanation. One row, one
        // cause - the same rule the armour sibling above follows.
        CHECK(!ShouldWarnNoEngineWeapon(true, false, false));

        // ⚠ A style whose plugin is missing does NOT warn, even though it
        // cannot render either. That row already says "Missing", and two
        // explanations for one row send the player looking for gear to equip
        // when the actual fix is to install a mod. One row, one cause.
        CHECK(!ShouldWarnNoWornGear(true, false, false));
    }

    {  // Clicking the lit rail entry does nothing at all.
        for (const auto m : { PaneMode::kStyles, PaneMode::kPresets,
                              PaneMode::kDye, PaneMode::kRules,
                              PaneMode::kBodyStudio, PaneMode::kShape,
                              PaneMode::kOverlays, PaneMode::kProfiles }) {
            const auto t = PlanPaneTransition(m, m);
            CHECK(!t.clearShowcaseSel);
            CHECK(!t.requestShowcaseTabs);
            CHECK(t.preview == PreviewSuppression::kUnchanged);
            CHECK(!t.restageEditBuffer);
            CHECK(!t.armDyeDump);
        }
    }
    {  // Leaving Presets always drops the selection and the preview
       // suppression. This is the hole the rail closes: the Dye button
       // assigned g_paneMode directly and did neither.
        for (const auto to : { PaneMode::kStyles, PaneMode::kDye,
                               PaneMode::kRules, PaneMode::kBodyStudio,
                               PaneMode::kShape, PaneMode::kProfiles }) {
            const auto t = PlanPaneTransition(PaneMode::kPresets, to);
            CHECK(t.clearShowcaseSel);
            CHECK(t.preview == PreviewSuppression::kOff);
        }
    }
    {  // The Looks page stages nothing, so crossing it changes nothing: in
       // from anywhere plain is a no-op plan, and out again only does what
       // the DESTINATION asks (into Dye arms the dump, exactly as from any
       // other page).
        const auto in = PlanPaneTransition(PaneMode::kStyles, PaneMode::kProfiles);
        CHECK(!in.clearShowcaseSel);
        CHECK(!in.restageEditBuffer);
        CHECK(in.preview == PreviewSuppression::kUnchanged);
        const auto out = PlanPaneTransition(PaneMode::kProfiles, PaneMode::kDye);
        CHECK(out.armDyeDump);
        CHECK(!out.restageEditBuffer);
    }
    {  // The edit buffer comes back only where it survives the frame. Into
       // Rules it does not: the rules-mode detector discards staging on the
       // next frame, so restaging here buys a rebuild only to undo it.
        CHECK(PlanPaneTransition(PaneMode::kPresets, PaneMode::kStyles).restageEditBuffer);
        CHECK(PlanPaneTransition(PaneMode::kPresets, PaneMode::kDye).restageEditBuffer);
        CHECK(PlanPaneTransition(PaneMode::kPresets, PaneMode::kBodyStudio).restageEditBuffer);
        CHECK(PlanPaneTransition(PaneMode::kPresets, PaneMode::kShape).restageEditBuffer);
        CHECK(!PlanPaneTransition(PaneMode::kPresets, PaneMode::kRules).restageEditBuffer);
    }
    {  // Entering Presets opens the browser fresh from whichever mode.
        for (const auto from : { PaneMode::kStyles, PaneMode::kDye,
                                 PaneMode::kRules, PaneMode::kBodyStudio,
                                 PaneMode::kShape }) {
            const auto t = PlanPaneTransition(from, PaneMode::kPresets);
            CHECK(t.clearShowcaseSel);
            CHECK(t.requestShowcaseTabs);
            CHECK(t.preview == PreviewSuppression::kOn);
        }
    }
    {  // Entering Dye arms the repaint dump, and only entering it.
        CHECK(PlanPaneTransition(PaneMode::kStyles, PaneMode::kDye).armDyeDump);
        CHECK(PlanPaneTransition(PaneMode::kPresets, PaneMode::kDye).armDyeDump);
        CHECK(PlanPaneTransition(PaneMode::kRules, PaneMode::kDye).armDyeDump);
        CHECK(!PlanPaneTransition(PaneMode::kDye, PaneMode::kStyles).armDyeDump);
    }
    {  // Rules enter and leave stay with the g_rulesModeApplied detector in
       // Draw(), which also catches the OnOpen re-stage no click path can.
       // So a plan touching Rules carries nothing of its own.
        const auto out = PlanPaneTransition(PaneMode::kRules, PaneMode::kStyles);
        CHECK(!out.clearShowcaseSel);
        CHECK(out.preview == PreviewSuppression::kUnchanged);
        CHECK(!out.restageEditBuffer);
        const auto in = PlanPaneTransition(PaneMode::kStyles, PaneMode::kRules);
        CHECK(!in.clearShowcaseSel);
        CHECK(in.preview == PreviewSuppression::kUnchanged);
        CHECK(!in.armDyeDump);
    }

    {  // Leaving a page with an edit on it asks first, and only then.
        CHECK(ShouldWarnOnLeave(true, PaneMode::kShape, PaneMode::kDye));
        CHECK(ShouldWarnOnLeave(true, PaneMode::kBodyStudio, PaneMode::kStyles));
        // Clean page: never ask.
        CHECK(!ShouldWarnOnLeave(false, PaneMode::kShape, PaneMode::kDye));
        // ⚠ Clicking the rail entry you are already on is not leaving, and
        // PlanPaneTransition returns an empty plan for that pair. The two have
        // to agree or a dirty page would ask on a click that changes nothing.
        CHECK(!ShouldWarnOnLeave(true, PaneMode::kShape, PaneMode::kShape));
        CHECK(!PlanPaneTransition(PaneMode::kShape, PaneMode::kShape).restageEditBuffer);
    }

    {  // Escape over SAM belongs to the editor, and the case that matters is
       // the one that shipped wrong: with the editor up over SAM, the press
       // must never reach SAM, because SAM closing takes the editor with it
       // through ShouldCloseForLostHost and the user loses both menus.
        CHECK(ShouldConsumeHostEscape(true, true, false));

        // Editor shut: SAM has to keep its own Escape or it cannot be closed.
        CHECK(!ShouldConsumeHostEscape(false, true, false));
        // Not hosted by SAM: the inventory path is modal already and there is
        // no second menu to hand back to.
        CHECK(!ShouldConsumeHostEscape(true, false, false));
        // Typing: the field's own Escape unfocuses it, and taking that would
        // shut the editor on someone halfway through naming an outfit.
        CHECK(!ShouldConsumeHostEscape(true, true, true));

        // The safety net is untouched and still catches SAM closing for any
        // reason that is not our key: its own close button, another mod, a
        // menu stack change. Consuming Escape only removes the path that took
        // both down at once.
        CHECK(ShouldCloseForLostHost(true, false));
        CHECK(!ShouldCloseForLostHost(true, true));
        CHECK(!ShouldCloseForLostHost(false, false));
    }

    {  // The dye pane's Special fold: it must never come back on its own.
       //
       // ⚠ THE REAPPEARANCE IS THE BUG. The section is only submitted on a
       // dyed stripe, and a host collapsing header keeps its open state keyed
       // by id whether or not it was drawn - so leaving it open, hiding it,
       // and putting a colour on brought it back expanded with no click
       // anywhere near it (user 2026-08-11, "sometimes").
        CHECK(ForceSpecialFoldClosed(/*visibleNow*/ true, /*visibleBefore*/ false,
                                     /*colourPicked*/ false));
        // A pick closes it outright, which is the other half the report asked
        // for: the click rewrites every field the section edits.
        CHECK(ForceSpecialFoldClosed(true, true, true));
        // Steady state: on screen, still on screen, nothing picked. The
        // player's own open or closed stands, or the section could never be
        // opened at all.
        CHECK(!ForceSpecialFoldClosed(true, true, false));
        // ⚠ NOT WHILE IT IS OFF SCREEN, in either combination. Spending the
        // request on a frame with no header to close would let the very
        // reappearance this catches sail through on the next one.
        CHECK(!ForceSpecialFoldClosed(false, false, false));
        CHECK(!ForceSpecialFoldClosed(false, true, false));
        CHECK(!ForceSpecialFoldClosed(false, false, true));
        CHECK(!ForceSpecialFoldClosed(false, true, true));
    }

    {  // The Lore-friendly preset's currency, against whether the stone exists.
       //
       // ⚠ THE PRESET USED TO HAND OUT A SEAMSTONE ECONOMY WITH NO SEAMSTONE.
       // The charge purse works without the plugin, because the charge is our
       // own co-save number and Refill only wants soul gems - so nothing
       // crashed and nothing logged, and the meter simply described an item
       // the player could not own or buy (user 2026-08-11).
        CHECK(LorePresetCostMode(/*seamstoneAvailable*/ true) == OS::CostMode::kCharge);
        CHECK(LorePresetCostMode(/*seamstoneAvailable*/ false) == OS::CostMode::kGold);
        // ⚠ NEVER kFree, in either direction. Lore-friendly with no bill at all
        // is Free-form wearing the other button's label, and the fallback has
        // to keep the playstyle it was asked for: the same slots, the same
        // count, a currency that exists.
        CHECK(LorePresetCostMode(true) != OS::CostMode::kFree);
        CHECK(LorePresetCostMode(false) != OS::CostMode::kFree);
        // The wire values the INI stores, pinned here because the fallback is
        // written into iCostMode and read back by CostModeFrom.
        CHECK(OS::CostModeFrom(2) == OS::CostMode::kCharge);
        CHECK(OS::CostModeFrom(1) == OS::CostMode::kGold);
        CHECK(OS::CostModeFrom(0) == OS::CostMode::kFree);
        // ⚠ AND AN UNKNOWN MODE IS GOLD, NOT FREE. Of the two ways to be wrong
        // about a hand-edited INI, the one that stops billing is the one
        // nothing on screen contradicts.
        CHECK(OS::CostModeFrom(7) == OS::CostMode::kGold);
        CHECK(OS::CostModeFrom(-1) == OS::CostMode::kGold);
    }

    if (g_failures == 0) {
        std::printf("all EditorGate tests passed\n");
    }
    return g_failures;
}
