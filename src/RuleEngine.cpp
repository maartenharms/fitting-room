#include "RuleEngine.h"

// std::ranges::stable_sort. Named explicitly rather than leaned on from the
// PCH: this file also compiles into the pure-logic RuleEngineTests target,
// which builds without it.
#include <algorithm>
#include <vector>

namespace OS::Rules {

    namespace {
        // The no-rule target: clears any rule overlay, leaves the base outfit
        // alone. Deliberately NOT "deactivate the outfit" - a rule that stops
        // matching must never strip the player.
        Target NoRuleTarget() {
            Target t;
            t.ruleId = "";
            t.base   = Base{ BaseKind::kKeep, "" };
            return t;
        }

        // PickWinner lived here: one pass returning the single highest-priority
        // matching rule, which then supplied both the base and the overlay. It
        // was removed when composition went additive - Evaluate now collects
        // every matching rule and merges their overlays, so "the winner" is no
        // longer one thing (the base winner and the highest matching rule can
        // differ). Its tie-break rule survives in the stable_sort there, and
        // several comments elsewhere still name it as the place priority is
        // resolved; those are describing the behaviour, which has not changed.

        // Whether this rule keys on a player action that just happened and is
        // still waiting to be dressed for. A rule only earns the exemption if
        // one of its own clauses names the thing that changed.
        //
        // Combat is the asymmetric one. Only a NON-negated Combat clause
        // qualifies, because a negated one is the "peace outfit" rule, which
        // becomes true on combat EXIT - and there is nothing urgent about
        // getting changed again once a fight is over.
        //
        // Sneak, swim and mount are symmetric, and deliberately so. Both
        // directions are an instantaneous player input, so standing up out of a
        // crouch should restore the previous look as promptly as crouching
        // changed it. The peace-outfit ambiguity does not arise, because the
        // engine is reacting to a button the player just pressed rather than to
        // the world's own state settling.
        // Dialogue joins them for the same reason and with the same symmetry:
        // a rule that bares the head to talk to someone is worthless if it
        // lands five seconds into the conversation, and putting the helmet back
        // on afterwards is exactly as urgent as taking it off was.
        //
        // ⚠⚠ CASTING WAS MISSING FROM THIS LIST UNTIL 2026-08-14 AND THAT IS
        // THE FIRST HALF OF THE FIELD REPORT "it takes so long to switch back
        // when we stop casting". Without a case here a casting rule is
        // dwell-gated in BOTH directions, so it waits out fMinDwellSeconds (5
        // in the live ini) on top of everything else.
        //
        // It is symmetric like sneak, and the falling edge is the one that
        // matters, but it earns the exemption for a slightly different reason
        // than the four above and the difference is worth keeping straight.
        // The others are a player input in both directions. Casting is an
        // EVENT that WorldWatch latches into a state for fCastHoldSeconds, so
        // its rising edge is the spell and its falling edge is that window
        // running out - not a button. It still qualifies, because the window
        // closing is exactly the moment "the player has stopped casting"
        // becomes true, and that is the thing the rule keys on. Making the
        // return wait out a dwell window on top of a hold the player already
        // paid for is the compounding this exists to stop.
        struct PendingBypass {
            bool combat{};
            bool sneaking{};
            bool swimming{};
            bool mounted{};
            bool dialogue{};
            bool casting{};
        };

        bool WantsBypass(const Rule& a_rule, const PendingBypass& a_pending) {
            for (const auto& c : a_rule.conditions) {
                switch (c.kind) {
                    case ConditionKind::kCombat:
                        if (a_pending.combat && !c.negate) {
                            return true;
                        }
                        break;
                    case ConditionKind::kSneaking:
                        if (a_pending.sneaking) {
                            return true;
                        }
                        break;
                    case ConditionKind::kSwimming:
                        if (a_pending.swimming) {
                            return true;
                        }
                        break;
                    case ConditionKind::kMounted:
                        if (a_pending.mounted) {
                            return true;
                        }
                        break;
                    case ConditionKind::kDialogue:
                        if (a_pending.dialogue) {
                            return true;
                        }
                        break;
                    case ConditionKind::kCasting:
                        if (a_pending.casting) {
                            return true;
                        }
                        break;
                    default:
                        break;
                }
            }
            return false;
        }
    }

    Decision RuleEngine::Evaluate(const RuleSet& a_rules, const WorldSnapshot& a_snapshot) {
        // Combat edge, handled in ONE place so no exit path can forget it -
        // including the pin early-return just below, which IS such an exit
        // path and must not duplicate or skip this update. The edge is one
        // tick wide, so it is latched into combatBypassPending_ immediately:
        // whether that latch survives to be spent is NoteApplied's job, not
        // this function's.
        const bool rising = a_snapshot.inCombat && hasPrevState_ && !prevInCombat_;
        if (rising) {
            bypassCombat_ = true;
        }
        if (!a_snapshot.inCombat) {
            bypassCombat_ = false;  // the fight ended; nothing to rush
        }
        // Sneak/swim/mount latch on EITHER edge - see WantsBypass for why they
        // are symmetric where combat is not. hasPrevState_ guards the first
        // evaluation of a session, which has nothing to compare against and
        // must not read as three simultaneous transitions.
        if (hasPrevState_) {
            if (a_snapshot.sneaking != prevSneaking_) {
                bypassSneaking_ = true;
            }
            if (a_snapshot.swimming != prevSwimming_) {
                bypassSwimming_ = true;
            }
            if (a_snapshot.mounted != prevMounted_) {
                bypassMounted_ = true;
            }
            if (a_snapshot.inDialogue != prevInDialogue_) {
                bypassDialogue_ = true;
            }
            if (a_snapshot.casting != prevCasting_) {
                bypassCasting_ = true;
            }
        }
        prevInCombat_ = a_snapshot.inCombat;
        prevSneaking_ = a_snapshot.sneaking;
        prevSwimming_ = a_snapshot.swimming;
        prevMounted_  = a_snapshot.mounted;
        prevInDialogue_ = a_snapshot.inDialogue;
        prevCasting_    = a_snapshot.casting;
        hasPrevState_ = true;

        // Deliberately kept AFTER the combat-edge update above, not before:
        // prevInCombat_/hasPrevCombat_/combatBypassPending_ must keep tracking
        // entering and leaving combat on every tick, even while paused, or
        // they read stale the moment the user resumes - a pin can last many
        // evaluations. A single Decision while paused never surfaces that
        // latched state, so comparing Decision output cannot prove this
        // ordering is right; it is defended by this comment, not a test.
        if (pinned_) {
            Decision d;
            d.outcome    = Outcome::kPaused;
            d.pinnedName = pinnedName_;
            return d;
        }

        // ---- additive composition -------------------------------------------
        //
        // ⚠ This used to be a single PickWinner and nothing else: the one
        // highest-priority matching rule supplied the base AND the overlay, and
        // every other matching rule was discarded. That made an overlay-only
        // rule mutually exclusive with an outfit rule, which is wrong for the
        // whole class of rule the Helmet Toggle pack is built from - "hide the
        // headgear" is meant to layer ON TOP of whatever outfit is being worn,
        // not to replace it. In the field it showed up as the pack silently
        // cancelling the user's own outfit rules.
        //
        // Now: EVERY matching rule contributes its overlay, and only the
        // highest-priority rule that actually names a base sets the outfit.
        // BaseKind::kKeep means "this rule expresses no opinion about the
        // outfit", which is exactly what an overlay-only rule wants to say, so
        // the two readings of kKeep collapse into one rather than conflicting.
        //
        // Priority now means two separate things, and that is deliberate: which
        // rule wins the BASE, and which rule wins a CONTESTED SLOT. Both resolve
        // the same way (higher wins), so there is one rule to explain.
        std::vector<const Rule*> matching;
        for (const auto& r : a_rules) {
            // ⚠ A PACK RULE NEVER EVALUATES. The Rule Library is a CATALOG you
            // import from, not a set of rules that run.
            //
            // This reverses the original design, in which author-pack rules were
            // live and only their enable checkbox belonged to the player. The
            // field showed what that meant in practice: a rule the player never
            // chose, sitting in a library section, marked Active and dressing
            // them. Nobody picked it, so it does not get to win.
            //
            // ⚠ KEYED ON packName, NOT ON `enabled`. `enabled` is the player's
            // switch for their OWN rules, and a library row no longer has one.
            // Importing copies the rule into the save's own set, which clears
            // packName, and from that moment it is an ordinary rule that runs
            // like any other. So there is one code path and importing is the
            // only thing that changes.
            if (!r.packName.empty()) {
                continue;
            }
            if (!r.enabled || r.invalid) {
                continue;
            }
            if (!AllMatch(r, a_snapshot)) {
                continue;
            }
            matching.push_back(&r);
        }
        // Highest first. stable_sort, not sort: ties must keep the store's own
        // array order (save rules, then packs in sorted filename order), which
        // is the same tie-break PickWinner's strict > gave and what the pack
        // scan's deterministic file ordering exists to make reproducible.
        std::ranges::stable_sort(
            matching, [](const Rule* a, const Rule* b) { return a->priority > b->priority; });

        Target target = NoRuleTarget();
        for (const auto* r : matching) {
            if (r->base.kind != BaseKind::kKeep) {
                target.ruleId = r->id;
                target.base   = r->base;
                break;  // highest-priority base wins; the rest contribute overlay only
            }
        }
        // Merged LOWEST priority first, so a higher-priority rule's entry
        // overwrites a lower one's for the same slot. Per-slot, not per-rule: two
        // rules touching different slots both survive intact, which is the point.
        Decision d;
        for (auto it = matching.rbegin(); it != matching.rend(); ++it) {
            for (const auto& [bit, entry] : (*it)->overlay) {
                target.overlay[bit] = entry;
                // Attribution recorded in the SAME pass that resolves the
                // contest, so it cannot disagree with the winner. Assigned
                // rather than inserted, for the same reason: the last write
                // wins here exactly as it does for the entry beside it.
                d.overlaySource[bit] = (*it)->id;
            }
        }
        // Published before the kNoChange early returns below, so the editor can
        // still explain a slot in the steady state where nothing is changing -
        // which is most of the time, and precisely when someone is browsing.
        d.activeOverlay = target.overlay;
        // Set before either kNoChange early return below, so a live reader
        // (WorldWatch::GetPublished) always has "what currently matches"
        // even in the (common) steady state where ruleId/base/overlay stay
        // at their empty defaults because nothing changed.
        //
        // matchedRuleId is the highest-priority MATCHING rule, which is not
        // necessarily the one that set the base - an overlay-only rule can
        // outrank every outfit rule and still leave the outfit alone. The full
        // set goes in activeRuleIds so the UI can mark every contributor rather
        // than implying only one is live.
        //
        // A rule naming no base and carrying no overlay changes nothing, so it
        // is not reported as live however well its conditions match. A fresh
        // "+ New Rule" is exactly that - no conditions, so it matches
        // everything, and no effects - and it now lands at the TOP of the list.
        // Reporting it would put the gold wash on a card doing nothing and make
        // the status strip name it over the rule actually dressing the player.
        const auto contributes = [](const Rule* a_r) {
            return a_r->base.kind != BaseKind::kKeep || !a_r->overlay.empty();
        };
        d.activeRuleIds.reserve(matching.size());
        for (const auto* r : matching) {
            if (!contributes(r)) {
                continue;
            }
            if (d.matchedRuleId.empty()) {
                d.matchedRuleId = r->id;  // highest-priority CONTRIBUTING match
            }
            d.activeRuleIds.push_back(r->id);
        }
        if (hasApplied_ && applied_ == target) {
            d.outcome = Outcome::kNoChange;
            return d;
        }
        if (!hasApplied_ && target.ruleId.empty() && target.overlay.empty()) {
            // Nothing applied and nothing to apply: the empty-rule-list case.
            d.outcome = Outcome::kNoChange;
            return d;
        }

        d.ruleId  = target.ruleId;
        d.base    = target.base;
        d.overlay = target.overlay;

        const PendingBypass pending{ bypassCombat_,  bypassSneaking_, bypassSwimming_,
                                     bypassMounted_, bypassDialogue_, bypassCasting_ };

        // ⚠ On a FALLING edge the rule carrying the clause is the one being LEFT,
        // not the one taking over. Stand up out of a crouch and the winner is
        // whatever matches when not sneaking, which typically has no sneak clause
        // at all - so checking only the winner leaves the player stuck in the
        // crouch outfit for the rest of the dwell window. Getting out of it is
        // exactly as urgent as getting into it was, so the currently applied rule
        // counts too.
        //
        // This does not loosen combat: its latch is cleared outright the moment
        // the fight ends, so pending.combat is already false on that edge and
        // neither rule can qualify.
        const Rule* leaving = nullptr;
        if (hasApplied_ && !applied_.ruleId.empty()) {
            for (const auto& r : a_rules) {
                if (r.id == applied_.ruleId) {
                    leaving = &r;
                    break;
                }
            }
        }
        // bypassUserEdit_ is unconditional, unlike the two WantsBypass terms:
        // those ask whether the rule keys on the thing that just changed, and
        // for a hand edit the thing that changed IS the rule, so there is
        // nothing to match it against. See BypassDwellOnce.
        //
        // Checked across EVERY matching rule, not just the base winner: under
        // additive composition a rule can contribute nothing but an overlay and
        // still be the one keyed on what just changed. The helmet pack is
        // exactly that - "hide the headgear while talking" carries the dialogue
        // clause and sets no base at all, so a base-only check would leave it
        // waiting out the dwell window it is most entitled to skip.
        bool bypass = bypassUserEdit_;
        if (!bypass) {
            for (const auto* r : matching) {
                if (WantsBypass(*r, pending)) {
                    bypass = true;
                    break;
                }
            }
        }
        if (!bypass && leaving != nullptr && WantsBypass(*leaving, pending)) {
            bypass = true;
        }

        const double minDwellSeconds = static_cast<double>(minDwell_);
        const double elapsed         = a_snapshot.nowSeconds - appliedAt_;
        const bool   inWindow        = hasApplied_ && elapsed < minDwellSeconds;

        if (inWindow && !bypass) {
            d.outcome        = Outcome::kSuppressed;
            d.dwellRemaining = static_cast<float>(minDwellSeconds - elapsed);
            return d;
        }
        d.outcome = Outcome::kApply;
        return d;
    }

    void RuleEngine::NoteApplied(const Decision& a_decision, double a_nowSeconds) {
        // appliedAt_ is the dwell anchor. Only a kApply decision represents a
        // refresh the caller actually performed; stamping the clock for
        // kNoChange/kSuppressed/kPaused would keep resetting the dwell window
        // without anything having changed.
        //
        // The pinned_ check additionally guards a decision that was DERIVED
        // before the pin but reported after it: application is deferred
        // through RequestRefresh/RunPlayerRefreshWhenSafe behind a
        // BlockingCooldown gate, so Evaluate and NoteApplied are not adjacent
        // in time, and a pin from SetPinned can land in that gap. Recording
        // such a decision would undo SetPinned's ForgetApplied and re-plant
        // the exact stale applied_ target THE RESUME CASE test exists to
        // rule out - a kApply derived before the pin should no more be
        // recorded now than a suppressed one would be.
        if (a_decision.outcome != Outcome::kApply || pinned_) {
            return;
        }
        applied_    = Target{ a_decision.ruleId, a_decision.base, a_decision.overlay };
        hasApplied_ = true;
        appliedAt_  = a_nowSeconds;
        // The re-dress has happened, so every pending exemption is spent,
        // whether or not it was the reason this decision got through. Clearing
        // the whole set rather than only the one that fired keeps the invariant
        // simple: a latch survives exactly until the next applied change.
        // ⚠ Every latch added to the set below has to be cleared here AND in
        // ResetForLoad. Dialogue was added to Evaluate and to neither, and it
        // survived a load as a free pass nobody had earned.
        bypassCombat_   = false;
        bypassSneaking_ = false;
        bypassSwimming_ = false;
        bypassMounted_  = false;
        bypassDialogue_ = false;
        bypassCasting_  = false;
        bypassUserEdit_ = false;
    }

    void RuleEngine::ForgetApplied() {
        applied_    = Target{};
        hasApplied_ = false;
        appliedAt_  = 0.0;
    }

    void RuleEngine::SetPinned(std::string a_outfitName) {
        pinned_     = true;
        pinnedName_ = std::move(a_outfitName);
        // The user is now wearing something this engine did not put on them,
        // so whatever it last applied is no longer what is on screen. Keeping
        // it would make the first evaluation after Resume derive that same
        // target, compare equal, return kNoChange, and strand the user in the
        // manually picked outfit until some unrelated world change.
        // Clearing the dwell clock too means Resume acts immediately rather
        // than waiting out a window that started before the pin.
        ForgetApplied();
    }

    void RuleEngine::ClearPin() {
        pinned_ = false;
        pinnedName_.clear();
    }

    void RuleEngine::RenamePin(std::string_view a_from, std::string_view a_to) {
        if (pinned_ && pinnedName_ == a_from) {
            pinnedName_ = std::string(a_to);
        }
    }

    void RuleEngine::ResetForLoad() {
        ForgetApplied();
        prevInCombat_   = false;
        prevSneaking_   = false;
        prevSwimming_   = false;
        prevMounted_    = false;
        prevInDialogue_ = false;
        prevCasting_    = false;
        hasPrevState_   = false;
        bypassCombat_   = false;
        bypassSneaking_ = false;
        bypassSwimming_ = false;
        bypassMounted_  = false;
        bypassDialogue_ = false;
        bypassCasting_  = false;
        // pinned_/pinnedName_ deliberately untouched: the pin is per-save
        // state, persisted in the 'RULE' co-save record, and is cleared only
        // by Resume (ClearPin), never as a side effect of a load.
    }

}  // namespace OS::Rules
