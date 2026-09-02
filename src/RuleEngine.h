#pragma once

// The pure decision core. Holds the arbitration state that must survive
// between evaluations: what is currently applied, when it was applied (dwell),
// whether the user pinned a look, and the previous combat/sneak/swim/mount/
// dialogue/casting flags, whose edges are what let a rule keyed on that action
// skip the dwell window.
//
// No engine headers: this compiles into the pure-logic test executable, which
// is the point. Every rule that is easy to get subtly wrong - priority ties,
// dwell, pin, "the overlay must clear when its rule stops matching" - is
// decided here and tested offline, because a rules engine is close to
// untestable in-game.

#include "RuleModel.h"

#include <string>
#include <string_view>

namespace OS::Rules {

    class RuleEngine {
    public:
        // Derive the target for this snapshot and compare it to what is
        // applied. Never mutates dwell state: a decision that the caller
        // cannot act on (a menu is open) must not burn the window.
        [[nodiscard]] Decision Evaluate(const RuleSet& a_rules, const WorldSnapshot& a_snapshot);

        // The caller accepted a decision and is requesting the refresh. Opens
        // the dwell window and records the new applied target.
        void NoteApplied(const Decision& a_decision, double a_nowSeconds);

        // The user picked a look by hand. a_outfitName empty means they took
        // their outfit off and are in real gear. Also forgets whatever this
        // engine last applied and clears the dwell clock: the user is now
        // wearing something it did not put on them, so that record is stale.
        // Without this, the first Evaluate after Resume would derive that
        // same target, compare equal, report kNoChange, and strand the user
        // in the manually picked outfit until some unrelated world change.
        void SetPinned(std::string a_outfitName);
        void ClearPin();
        [[nodiscard]] bool IsPinned() const { return pinned_; }

        // What this engine last dressed the player in. kKeep means it has
        // applied nothing, kRealGear that it took their outfit off, kOutfit
        // that it named one. Const and state-free.
        //
        // ⚠ READ IT BEFORE ANYTHING PINS, because SetPinned calls
        // ForgetApplied: the moment the user picks a look by hand this record
        // is gone by design, since what they are wearing is no longer what the
        // engine put there. WorldWatch takes it when the editor OPENS for
        // exactly that reason - see the resume test in SetEditorOpen.
        [[nodiscard]] Base AppliedBase() const {
            return hasApplied_ ? applied_.base : Base{};
        }

        // The pinned outfit was renamed elsewhere (the editor's Name field),
        // not re-picked - keep the live pin pointed at the SAME outfit under
        // its new name. Unlike SetPinned, this must NOT ForgetApplied/reset
        // the dwell clock: nothing about what is worn or when it was applied
        // changed, only the label. A no-op if unpinned or pinned to a
        // different name than a_from.
        void RenamePin(std::string_view a_from, std::string_view a_to);

        // Post-load reconstruction: the co-save knows which rule was applied,
        // but the overlay it produced was never persisted. Seeding the applied
        // target as "nothing" makes the first evaluation re-derive and
        // re-apply it, and a fresh dwell clock means it is not suppressed.
        // Leaves pinned_/pinnedName_ untouched: ResetForLoad ITSELF is not a
        // clear (a load's pin, if any, is seeded separately from the
        // co-save's mirror). The pin is per-save state and is cleared by
        // Resume (ClearPin), by WorldWatch::OnRevert, and by
        // WorldWatch::SetEngineEnabled when it flips the engine on over a
        // leftover manual pin - never as a side effect of THIS function.
        void ResetForLoad();

        // Forgets what this engine last applied and clears the dwell clock,
        // forcing the next Evaluate to re-derive instead of comparing equal
        // against stale applied_ state and reporting kNoChange forever.
        // Originally a private helper shared by SetPinned and ResetForLoad;
        // its own comment already anticipated this - WorldWatch::
        // NotifyOutfitDeleted is that "third caller". Deleting an outfit
        // rewrites no rule text (unlike a rename, there is nothing to
        // rewrite - Base::outfitName still names the gone outfit verbatim),
        // so without a way to force applied_ stale, the derived target
        // would stay textually identical forever and the engine would never
        // notice the outfit disappeared. Touches ONLY applied_/hasApplied_/
        // appliedAt_ - not pinned_/pinnedName_, and not the combat-edge
        // tracking ResetForLoad also resets; a caller that needs those
        // cleared too wants ResetForLoad/ClearPin instead of this.
        void ForgetApplied();

        void SetMinDwellSeconds(float a_seconds) { minDwell_ = a_seconds; }

        // The user changed a rule by hand: let the NEXT applied change skip the
        // dwell window.
        //
        // The window exists to stop WORLD churn re-dressing the player every
        // few seconds - a location boundary being crossed back and forth,
        // weather flapping. A deliberate edit in the Rules tab is not churn,
        // and at the default five seconds it made picking an outfit from a
        // rule's dropdown look broken: the character kept the old look, and a
        // second pick made during the wait was swallowed, so the eventual
        // change could even be to the wrong outfit. Field log, 2026-08-01:
        // "suppressed 'Naked' (dwell 4.5s remaining)" then "applied 'Naked'"
        // 5.4 seconds after the click.
        //
        // One-shot, spent by NoteApplied like the four edge latches, so it
        // cannot leave the dwell disabled for a whole editing session - the
        // next world-driven evaluation gets its normal window back.
        void BypassDwellOnce() { bypassUserEdit_ = true; }

    private:
        Target applied_{};
        bool   hasApplied_{ false };
        double appliedAt_{ 0.0 };
        float  minDwell_{ 5.0f };
        // LIVE AUTHORITY for the pin while a game session is running: only
        // SetPinned/ClearPin/RenamePin/ResetForLoad above touch these two. Task 10
        // added a SECOND copy, RuleStore::EngineState::pinned/pinnedName,
        // to serialize into the 'RULE' co-save record - required because no
        // RuleEngine instance exists at co-save load/save time yet (Task 11
        // is what creates one and owns the read-and-write-back contract
        // between the two: see EngineState's own comment in RuleStore.h).
        // Two fields with identical names and no compiler-enforced link
        // between them is a real risk - a SetPinned that forgets the
        // write-back leaves EngineState invisibly stale - so that sync must
        // happen in exactly ONE place. Do not add a third reader/writer of
        // the pin without going through whatever Task 11 establishes there.
        bool   pinned_{ false };
        std::string pinnedName_;
        bool   prevInCombat_{ false };
        bool   prevSneaking_{ false };
        bool   prevSwimming_{ false };
        bool   prevMounted_{ false };
        bool   prevInDialogue_{ false };
        // ⚠ NOT a live "is casting" read. WorldWatch latches the cast event
        // with a timestamp and the snapshot's `casting` means "cast within
        // fCastHoldSeconds", so the falling edge tracked here is that hold
        // running out rather than a key coming up. See WantsBypass.
        bool   prevCasting_{ false };
        bool   hasPrevState_{ false };

        // A player action has happened and the character has not been
        // re-dressed for it yet. Latched rather than derived per-call: an edge
        // is one tick wide, but the caller may be unable to apply for many
        // ticks (a menu is open), and the exemption exists to make the new
        // outfit appear promptly, not to expire while the player reads a map.
        //
        // One flag per kind, not a single shared one, so a rule only skips the
        // dwell when it actually keys on the thing that just changed. Crouching
        // must not hand a weather rule a free pass through the window that
        // exists to stop it flapping.
        bool bypassCombat_{ false };
        bool bypassSneaking_{ false };
        bool bypassSwimming_{ false };
        bool bypassMounted_{ false };
        bool bypassDialogue_{ false };
        bool bypassCasting_{ false };

        // The user just edited a rule in the Rules tab. Unlike the four above
        // this is NOT keyed on a condition kind, because it is not answering
        // "does this rule care about what changed" - the thing that changed IS
        // the rule. See BypassDwellOnce.
        bool bypassUserEdit_{ false };
    };

    // ---- one apply pass when the editor closes -----------------------------
    //
    // ⚠ THE POLARITY IS INVERTED ON PURPOSE AND IT IS THE FIRST THING ANYONE
    // WILL TRY TO "FIX". Auto switching being ON is what SUPPRESSES this, not
    // what enables it. Two separate decisions land on that, and both were the
    // user's:
    //
    //   * With the engine ON, the heartbeat already owns the outfit. It
    //     re-evaluates every couple of seconds, so an extra one-shot at the
    //     moment the editor closes would apply the same answer the heartbeat is
    //     about to apply anyway. Redundant, and redundant work that touches the
    //     player's look is how flicker gets shipped.
    //
    //   * With the engine ON, closing the editor MUST NOT override a hand-picked
    //     outfit. "Clear the pin on close" was proposed on 2026-08-04 and
    //     declined the same day, because a look chosen by hand could then never
    //     survive leaving the editor and the outfit tabs would be pointless.
    //     SetEditorOpen already carries that note at the resume test.
    //
    // So this exists FOR the player who keeps auto switching off. For them the
    // rules never fire on their own at all, and leaving the editor is the one
    // predictable moment to run them: it is the substitute for the heartbeat
    // rather than an addition to it. Nothing about it contradicts "auto
    // switching is off", which is a statement about switching CONTINUOUSLY.
    //
    // a_outfitsChanged is "this editor session touched the outfits", not "the
    // staged look is dirty". Opening the editor to read the Rules tab and
    // closing it again must not redress anybody.
    [[nodiscard]] inline constexpr bool ShouldApplyOnEditorExit(bool a_engineEnabled,
                                                                bool a_outfitsChanged) {
        return !a_engineEnabled && a_outfitsChanged;
    }

    // ---- and whether the pin comes off, which is a SEPARATE question --------
    //
    // ⚠ THIS ONE IS NOT GATED ON THE ENGINE AT ALL, and the difference between
    // the two is the whole reason they are two functions. The one above asks
    // "does closing the editor have to APPLY the rules itself", which only
    // matters when no heartbeat is going to. This asks "is the engine allowed
    // to act again", and the answer is yes in both states: with the heartbeat
    // running it applies within a couple of seconds, and with it stopped the
    // pass above does it once.
    //
    // ⚠ IT SUPERSEDES THE NARROW "left on the rules' own pick" TEST, and it
    // reverses a decision from 2026-08-04. That day, "clear the pin on close"
    // was declined because a hand-picked outfit would never survive leaving the
    // editor. The narrow test that replaced it only resumed when you left on
    // the outfit the rules had already chosen, which is the one case where you
    // had not picked anything - so every real outfit switch still ended with a
    // paused engine and a Resume button to find. The user hit that repeatedly
    // (2026-08-06, in those words) and called it: resuming matters more than
    // protecting the hand-pick.
    //
    // ⚠ SO THE KNOWN COST IS ACCEPTED RATHER THAN OVERLOOKED. With rules on and
    // a rule matching, the outfit chosen by hand IS replaced shortly after the
    // editor closes. That is the behaviour asked for; if it ever needs undoing,
    // the fix is to make a hand-pick the engine's new BASE rather than to bring
    // this pause back, because the pause is what made the tabs feel broken.
    //
    // a_outfitsChanged keeps it honest: opening the editor to read something
    // and closing it again leaves an existing pin exactly where it was.
    [[nodiscard]] inline constexpr bool ShouldClearPinOnEditorExit(bool a_outfitsChanged,
                                                                   bool a_leftOnRulesPick) {
        return a_outfitsChanged || a_leftOnRulesPick;
    }

    // ---- is there anything for a pin to hold off -----------------------------
    //
    // A pin stops the rules dressing you. With no rule that could ever dress
    // you, it stops nothing, and the Rules tab was still announcing "Auto
    // switching paused. Wearing 'Outfit 7' manually." with a Resume button
    // beside it (user 2026-08-08). That banner describes a thing that cannot
    // happen, and asks for a click to leave a state that has no effect.
    //
    // ⚠ A RULE THAT CANNOT WIN DOES NOT COUNT, and skipping that is what makes
    // this a real predicate rather than `!a_rules.empty()`. `enabled` is the
    // player's own switch on the row. `invalid` is the loader keeping a rule
    // whose Advanced text will not parse, or whose form references are gone,
    // precisely so it is visible and never applied. Neither can reach
    // ApplyDecision, so a save holding only those has nothing to pause.
    //
    // ⚠ DELIBERATELY NOT GATED ON THE ENGINE TOGGLE. Auto switching being off
    // is a state the player flips back on from the same tab, and SetEngineEnabled
    // already clears a leftover pin when it does. Folding the toggle in here
    // would drop the pin for a player who is about to want it.
    [[nodiscard]] inline bool AnyRuleCanApply(const RuleSet& a_rules) {
        for (const auto& r : a_rules) {
            if (r.enabled && !r.invalid) {
                return true;
            }
        }
        return false;
    }

}  // namespace OS::Rules
