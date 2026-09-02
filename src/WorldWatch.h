#pragma once

// The engine side of the rules feature: builds a WorldSnapshot, asks the pure
// RuleEngine what should be worn, and applies the answer when the gates allow.
//
// Evaluation and application are DELIBERATELY separate. Evaluation runs on
// every trigger and is never suppressed, because the Rules tab shows live
// condition state and because suppressing evaluation is what starved the
// first design (a weather change during an open inventory never landed).
// Application is what the gates hold back.

#include "RuleModel.h"

#include <map>
#include <string>

namespace OS::WorldWatch {

    void Init();          // register sinks; call once at kDataLoaded
    void OnSaveLoaded();  // reset engine state, then evaluate
    void OnRevert();

    // Gate bookkeeping. Application resumes, and one evaluate-and-apply pass
    // runs, when the last gate clears.
    void SetMenuOpen(bool a_anyMenuOpen);
    void SetEditorOpen(bool a_open);

    // The editor is open but showing the Rules view, which previews no edit
    // buffer of its own - so the editor gate does not apply and rules may land
    // live while they are being authored. See SetEditorOpen's own note and
    // GateReason. EditorUI hands the character back (DiscardStaging) on the way
    // in and takes it again on the way out, so the two flags stay honest.
    void SetRulesViewOpen(bool a_open);

    // The user picked a look by hand: pin the engine. a_outfitName empty
    // means they took their outfit off. Called ONLY from user-initiated
    // paths, never from co-save load or the rules engine itself. Pins
    // unconditionally, even while the engine is disabled or has no rules -
    // harmless on its own (application stays gated either way), and it is
    // the precondition SetEngineEnabled relies on to know a pin might be
    // sitting there when it flips on.
    void NotifyManualPick(std::string_view a_outfitName);
    void Resume();  // the Rules tab's Resume button

    // Whether the live pin is going to clear on its own when the editor closes,
    // which decides whether the Rules tab needs to offer a Resume button at all.
    //
    // ⚠ THE ANSWER IS NOT ALWAYS YES, AND THE QUICK-SWITCH HOTKEY IS WHY. That
    // path pins from outside the editor (InputListener), so nothing marks the
    // session as having changed an outfit and the pin outlives every editor
    // close. That pin genuinely needs a way out, and Resume is it.
    //
    // A pin restored from a co-save answers false for the same reason, which is
    // correct: the session that set it is over.
    //
    // Safe from any thread; the Rules tab asks it from the render thread.
    [[nodiscard]] bool PinResolvesOnEditorExit();

    // An outfit named a_from was renamed to a_to (the editor's Name field,
    // player target only). Rewrites every save-owned rule base naming it
    // (RuleStore::RenameOutfitEverywhere) AND, if the live pin currently
    // names a_from, renames the pin in place - the engine, the pin-name
    // mirror, and the 'RULE' co-save record all move together through this
    // one entry point, the single-writer discipline PushEngineState already
    // enforces for every other pin change. Call this INSTEAD OF
    // RuleStore::RenameOutfitEverywhere directly whenever a live session may
    // be pinned; never a new pin (SetPinned) - a rename is not a fresh pick.
    void NotifyOutfitRenamed(std::string_view a_from, std::string_view a_to);

    // An outfit named a_name was deleted (the editor's delete-confirm
    // action, player target only). The asymmetric twin of
    // NotifyOutfitRenamed: a rename rewrites every rule base naming the
    // outfit, so the derived Target changes and Evaluate naturally
    // reconsiders on its own. A delete rewrites NOTHING - a rule's
    // Base::outfitName is still the exact same string it always was - so
    // without this call, RuleEngine::Evaluate keeps deriving the identical
    // Target every trigger, keeps comparing it equal to what it last
    // applied, and keeps reporting kNoChange: the overlay an orphaned rule
    // composed (a helmet hide, say) would outlive the outfit that produced
    // it for the rest of the session, with no warning glyph and no log
    // line, because HandleMissingOutfit is only reachable from kApply and
    // kApply never comes.
    //
    // This forces RuleEngine::ForgetApplied (the same primitive SetPinned/
    // ResetForLoad already use) so the next evaluation is forced to
    // re-derive rather than sit on kNoChange, clears the live rule overlay
    // so it stops composing even before that evaluation gets a chance to
    // land (it may be sitting behind a gate), and evaluates immediately.
    // Deliberately does NOT mark any rule invalid itself: the re-derive
    // this triggers reaches ApplyDecision's existing kApply branch exactly
    // like any other trigger, and HandleMissingOutfit is the ONE place
    // that marks a rule inert for a missing outfit - a second writer of
    // Rule::invalid here would repeat the two-mechanisms-one-field hazard
    // this feature already has on record.
    void NotifyOutfitDeleted(std::string_view a_name);

    // The engine on/off toggle in the Rules tab. Enabling also clears a
    // leftover manual pin and resumes evaluation - see the .cpp - so it
    // overlaps with Resume() when a pin was set while the engine was off.
    void SetEngineEnabled(bool a_enabled);

    // Force one evaluation now (rule edits, Resume, gate release).
    void RequestEvaluation();

    // The same, for a change the USER just made by hand in the Rules tab.
    // Additionally lets the next applied change skip the dwell window, so an
    // edit shows on the character at once instead of up to fMinDwellSeconds
    // later. Every mutation in RulesUI.cpp goes through this rather than
    // RequestEvaluation; world-driven triggers keep their normal window. See
    // RuleEngine::BypassDwellOnce.
    void RequestEvaluationForUserEdit();

    // The most recent evaluation - every trigger, every heartbeat, never
    // gated (see the file comment above: evaluation itself always runs even
    // while application is held back). This is what the Rules tab's status
    // strip and live chip coloring read (Task 13): the currently matched
    // rule's chips need the SAME snapshot the engine just judged them
    // against, not a second one built fresh on the render thread a frame
    // later and possibly disagreeing with it.
    struct Published {
        Rules::Decision      decision;
        Rules::WorldSnapshot snapshot;

        // Every rule (pack OR save-owned) this evaluation's Advanced-clause
        // resolution found invalid, keyed by rule id, valued by the display
        // reason - taken from the throwaway RuleSet BuildSnapshot/
        // AdvancedCondition::Resolve just annotated, before
        // PersistAdvancedValidity's write-back (which only reaches
        // save-owned rules; packs are read-only and have no persistent
        // invalid setter - see that function's own comment). A reader that
        // wants a PACK rule's Advanced invalidity - the Rules tab's warning
        // glyph - cannot get it from RuleStore::Merged() (its `invalid`
        // field is never set for that case) and reads this instead.
        std::map<std::string, std::string> advancedInvalid;
    };

    // Returns COPIES taken under the internal lock. The render thread
    // (EditorUI::Draw/RulesUI::Draw) calls this every drawn frame while the
    // main thread overwrites the published state every evaluate; hedging out
    // a reference would be a torn read across threads, the same reasoning as
    // RuleStore::Snapshot() over a raw g_current reference. Safe to call
    // from any thread.
    [[nodiscard]] Published GetPublished();

}  // namespace OS::WorldWatch
