#pragma once

// The onboarding tutorial's runtime half: the card on screen, the position in
// it, and the write-back that stops it coming again.
//
// The decision of WHETHER to run lives in TutorialPlan.h, which is pure and
// tested. This half is presentation and settings, and no test compiles it.

#include "EditorGate.h"  // PaneMode

#include <imgui.h>  // ImVec2, for the anchors a card points at

#include <cstdint>

namespace OS::Tutorial {

    // What a card can point at.
    //
    // ⚠ A CARD DIMS THE WHOLE EDITOR BEHIND IT, WHICH IS THE SPOTLIGHT FOR
    // FREE. The host draws a modal's backdrop itself, and the ring is drawn on
    // the SCREEN list, which sits above everything including that backdrop. So
    // a dimmed editor with one control ringed and pulsing costs two draw calls
    // and no scrim of our own.
    //
    // ⚠ AN ANCHOR NOBODY PUBLISHED DRAWS NOTHING, and that is what makes the
    // list safe to extend ahead of the wiring. Each publish is stamped with the
    // frame it happened on, so a control that is not on screen this frame, or
    // one whose page is not open, simply has no ring rather than a ring in the
    // wrong place.
    enum class Anchor : std::uint8_t {
        kNone = 0,
        kRail,         // the page switcher down the left edge
        kOutfitTabs,   // the saved-outfit tab strip
        kAddOutfitTab, // the "+" at the end of that strip, on its own
        kSlotRow,      // the slot list
        kTargetName,   // who is being dressed; a control, though it reads as a label
        kApply,        // the footer's commit
        kDyeTiles,     // the garment tiles, left half of the dye page
        kDyePalette,   // the colours, right half of the dye page
        kPresetList,   // the preset browser's results
        kPresetTabs,   // Discovered / Curated / Exported, above that list
        kShapeSliders, // the slider surface on the Shape page
        kBodyLibrary,  // the preset list on the left of Body Studio
        kSettingsTile, // the gear pinned to the bottom of the rail
        kNewRuleButton,    // "+ New Rule" in the Rules toolbar, always drawn
        kRuleStarters,     // the "start from one of these" row on an empty Rules page
        kRuleList,         // the cards themselves, once there is one to point at
        // ⚠ THE FIRST EDITABLE ROW'S buttons, not every row's. Published once
        // per frame by the first save-owned card drawn, because a ring around
        // the last row's "+ Condition" would be pointing at whichever rule
        // happened to be at the bottom of the list.
        kRuleAddCondition, // "+ Condition" on that card
        kRuleAddEffect,    // "+ Effect" on that card
        kOverlayLayers,    // the location accordions and their layer rows
        kOverlayTextures,  // the texture card grid on the right
        kMakeupSections,   // the makeup accordions at the bottom of the left pane
        kLooksLibrary,     // the saved-looks list down the left of the Looks page
        kLooksParts,       // the per-part checkbox column beside a selected look
        kLooksSave,        // the name field and Save look button above the library
        kCount
    };

    // Record where a control is, for the frame it was drawn on. Call straight
    // after submitting it, while GetItemRectMin/Max still describe it.
    void PublishAnchor(Anchor a_anchor, const ImVec2& a_min, const ImVec2& a_max);

    // What a card can ask the player to actually do.
    //
    // ⚠ A STEP THAT ASKS FOR ONE OF THESE DRAWS NO MODAL, and that is the whole
    // mechanism rather than a detail. A modal blocks the editor underneath it,
    // so a card cannot both be up and be waiting for you to click the thing it
    // is describing. An action step puts an inline banner in the layout
    // instead, rings its target, and waits. Nothing is blocked, nothing is
    // faked, and the step ends because the player did it.
    // ⚠ EVERY ONE OF THESE HAS TO BE UNDOABLE OR HARMLESS. A tutorial that asks
    // the player to do something leaves that something done, so the only fair
    // asks are the ones a player would happily undo: picking a slot, opening a
    // list, trying a preset on, nudging a slider that the page itself puts back
    // when you leave it. That rule is also why the Rules page has no action
    // step. The honest ask there would be "make a rule", and a tutorial has no
    // business leaving a live auto-switching rule behind in someone's game.
    enum class Action : std::uint8_t {
        kNone = 0,
        kSelectedSlot,      // clicked a slot row in the list
        kOpenedRoster,      // clicked the who-is-being-dressed control
        kSelectedDyeStripe, // ringed a piece on a dye tile
        kPickedDyeColour,   // painted the ringed piece from the palette
        kTriedPreset,       // clicked a preset, which only previews it
        kMovedShapeSlider,  // dragged a body or proportion slider
        kPickedBodyPreset,  // opened a body preset in the workbench
        kOpenedPage,        // chose a page on the rail
        // ⚠⚠ THIS ONE LEAVES SOMETHING BEHIND AND IT WAS ASKED FOR ANYWAY (user
        // 2026-08-16: "we don't really have a rules page tutorial run through
        // which we kind of need"). The note above says every honest ask on this
        // page leaves a live rule, and that was the argument for having no
        // action step here at all. It still costs what it costs; what changed is
        // that the page now offers STARTERS, so the rule the player is asked to
        // make is a complete, named, one-click one rather than an empty card
        // they have to fill in, and the card that follows tells them the bin
        // takes it away again.
        kMadeRule,          // pressed a starter or + New Rule, so a rule exists
        // The two halves of the sentence on a card, asked for one at a time
        // (user 2026-08-16: "including how to add rules and condition and then
        // effects"). Both are edits to a rule the player has just made, and
        // both are undone by the same bin the last card points at.
        kAddedCondition,    // put a When clause on a rule
        kAddedEffect,       // put a Then effect on a rule
        kSelectedOverlayLayer, // clicked a layer row on the Overlays page
        kPickedOverlayTexture, // clicked a texture card for the selected layer
        // ⚠ SELECTING, NOT APPLYING, AND THAT IS THE ONLY HONEST ASK ON THIS
        // PAGE. Picking a look off the library only fills the part column in;
        // it moves nothing. Apply is the opposite: it rewrites the character
        // and closes the editor to show you, so a tutorial has no business
        // asking for it, exactly as the Rules page refuses to leave a live
        // rule behind.
        kSelectedLook,         // clicked a saved look in the Looks library
        kCount
    };

    // Tell the tutorial the player just did something. A no-op unless a step is
    // waiting for exactly this, so call sites need no guard of their own.
    void NotifyAction(Action a_action);

    // Something about this session that decides whether a step can be asked at
    // all.
    //
    // ⚠ THIS IS NOT THE SAME QUESTION AS "IS THE CONTROL ON SCREEN", and
    // conflating them is the trap. An anchor's frame stamp already answers
    // visibility, and it cannot answer this: a control that has scrolled out of
    // view and a control that does not exist in this playthrough both publish
    // nothing, and skipping a step for the first would be wrong. So the editor
    // states the durable fact outright rather than letting the tutorial infer it
    // from silence.
    //
    // ⚠ AND A STEP WHOSE REQUIREMENT FAILS IS DROPPED FROM THE PLAN, NOT LEFT
    // TO BE SKIPPED. An action step ends only when the player does the thing, so
    // a step asking for a control that is not there waits forever: the player
    // with no follower was told to click a roster that the editor never draws
    // (user 2026-08-08). Skip is still on the banner, but a tutorial whose way
    // out is the only way past it has already failed at the one job it has.
    enum class Requires : std::uint8_t {
        kNone = 0,
        kOtherTargets,  // someone besides the player to dress, so the roster draws
        // ⚠ kEmptyRuleList LIVED HERE AND IS GONE (2026-08-16). It guarded the
        // make-a-rule step while that step pointed only at the starters, which
        // are drawn on an empty page and nowhere else. The step points at
        // "+ New Rule" now, with the starters as its second ring, and that
        // button is on the toolbar whatever the list holds - so there is
        // nothing left to require, and a save with rules already in it gets the
        // step rather than losing it.
        //
        // ⚠ THE LOOKS LIBRARY STARTS EMPTY FOR EVERYONE, which makes this the
        // clearest case the Requires machinery has. On a first visit there is
        // no look to pick, so the pick step would ring a list with nothing in
        // it and wait for a click that cannot happen. Same failure the roster
        // step had, and the same fix.
        kSavedLooks,    // at least one saved look in the library, so one can be picked
        kCount
    };

    // State a session fact. Call before the tutorial could start: the plan is
    // built when a tutorial activates, and a fact that arrives afterwards does
    // not retroactively remove a step.
    void SetRequirement(Requires a_requirement, bool a_met);

    // ⚠ CALL FROM THE LAYOUT, not at window scope. This is the instruction for
    // an action step and it is an ordinary banner: it flows with the page,
    // pushes what follows down, and cannot be covered by a child window. The
    // rules page's paused-auto-switching banner is the same shape.
    //
    // Draws nothing unless an action step is live.
    void DrawBanner();

    // Whether the editor should show the rail and nothing else.
    //
    // ⚠ TRUE ONLY WHILE THE WELCOME IS RUNNING, and it is what makes the first
    // thing anyone sees a question rather than a full editor with a card on
    // top. The welcome ends by asking the player to choose a page, and an empty
    // right-hand side is what makes that ask legible: there is one thing to do
    // and the rail is the only place to do it. Every other route out of the
    // welcome, Skip and No thanks included, clears this, so nobody is ever left
    // looking at an editor with nothing in it.
    [[nodiscard]] bool ShowRailOnly();

    // Called from the editor's open reset. Drops any card left on screen from a
    // previous session and re-reads the flags, so a replay pressed in the
    // settings panel takes effect on the next open rather than the next restart.
    void OnEditorOpened();

    // ⚠ CALL AT WINDOW SCOPE, ONCE PER FRAME, and nowhere else. It both asks for
    // the popup and begins it, and the two halves hash their name against the
    // current id stack, so a call from inside a child, a PushID or a tab bar is
    // two different popups and a card that never appears. That is OS-29.
    //
    // a_pageReady is the caller's answer to "is there anything on this page to
    // teach": a Shape page with no RaceMenu behind it, for instance, is a page
    // with no controls, and a tutorial about controls that are not there is
    // worse than none.
    void Draw(EditorGate::PaneMode a_mode, bool a_pageReady);

    // Put every tutorial back. For the settings panel's replay button.
    void ResetAll();

}  // namespace OS::Tutorial
