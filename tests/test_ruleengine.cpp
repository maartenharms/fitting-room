// Pure-logic tests for the rules decision core. No engine, no RE:: types.
#include "RuleEngine.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS::Rules;

// A rule that always matches (no conditions), naming an outfit.
static Rule MakeRule(const char* id, int priority, const char* outfit) {
    Rule r;
    r.id            = id;
    r.name          = id;
    r.priority      = priority;
    r.base.kind     = BaseKind::kOutfit;
    r.base.outfitName = outfit;
    return r;
}

static Condition InteriorCond(bool negate = false) {
    Condition c;
    c.kind   = ConditionKind::kInterior;
    c.negate = negate;
    return c;
}

int main() {
    {  // higher priority wins
        RuleSet rules = { MakeRule("low", 10, "A"), MakeRule("high", 40, "B") };
        RuleEngine e;
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.ruleId == "high");
        CHECK(d.base.outfitName == "B");
    }
    {  // equal priority: the earlier array position wins
        RuleSet rules = { MakeRule("first", 10, "A"), MakeRule("second", 10, "B") };
        RuleEngine e;
        CHECK(e.Evaluate(rules, WorldSnapshot{}).ruleId == "first");
    }
    {  // disabled and invalid rules never win
        RuleSet rules = { MakeRule("off", 90, "A"), MakeRule("bad", 80, "B"),
                          MakeRule("ok", 10, "C") };
        rules[0].enabled = false;
        rules[1].invalid = true;
        RuleEngine e;
        CHECK(e.Evaluate(rules, WorldSnapshot{}).ruleId == "ok");
    }
    {  // conditions gate the win
        RuleSet rules = { MakeRule("inside", 50, "A") };
        rules[0].conditions = { InteriorCond() };
        RuleEngine e;
        WorldSnapshot s;
        s.interior = false;
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kNoChange);  // nothing applied yet
        s.interior = true;
        CHECK(e.Evaluate(rules, s).ruleId == "inside");
    }
    {  // an empty rule set is a permanent no-op
        RuleEngine e;
        CHECK(e.Evaluate(RuleSet{}, WorldSnapshot{}).outcome == Outcome::kNoChange);
    }
    {  // re-deriving the same target after applying it is kNoChange
        RuleSet   rules = { MakeRule("r", 10, "A") };
        RuleEngine e;
        const auto first = e.Evaluate(rules, WorldSnapshot{});
        CHECK(first.outcome == Outcome::kApply);
        e.NoteApplied(first, 100.0);
        WorldSnapshot later;
        later.nowSeconds = 200.0;  // well past any dwell window
        CHECK(e.Evaluate(rules, later).outcome == Outcome::kNoChange);
    }
    {  // losing the winner yields the no-rule target: clears the overlay,
       // leaves the base alone (kKeep, empty rule id)
        RuleSet rules = { MakeRule("inside", 50, "A") };
        rules[0].conditions = { InteriorCond() };
        RuleEngine e;
        WorldSnapshot s;
        s.interior = true;
        const auto applied = e.Evaluate(rules, s);
        CHECK(applied.outcome == Outcome::kApply);
        e.NoteApplied(applied, 100.0);
        s.interior  = false;
        s.nowSeconds = 200.0;
        const auto cleared = e.Evaluate(rules, s);
        CHECK(cleared.outcome == Outcome::kApply);
        CHECK(cleared.ruleId.empty());
        CHECK(cleared.base.kind == BaseKind::kKeep);
        CHECK(cleared.overlay.empty());

        // Settle after clearing: once the caller notes the cleared ("nothing")
        // target as applied, re-deriving it again must be kNoChange. A bug
        // that keeps re-emitting kApply of the same nothing-target every tick
        // would be a visible repeated refresh in-game.
        e.NoteApplied(cleared, 200.0);
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kNoChange);
    }
    {  // same visible outfit reached through a DIFFERENT rule is still a new
       // target: the comparison keys on ruleId too, not just the visible look
        RuleSet rules = { MakeRule("a", 50, "Same"), MakeRule("b", 10, "Same") };
        rules[0].id = "a";
        rules[1].id = "b";
        RuleEngine e;
        const auto first = e.Evaluate(rules, WorldSnapshot{});
        e.NoteApplied(first, 100.0);
        rules[0].enabled = false;  // now "b" wins, same outfit
        WorldSnapshot later;
        later.nowSeconds = 200.0;
        const auto second = e.Evaluate(rules, later);
        CHECK(second.outcome == Outcome::kApply);  // ruleId differs, so it is a new target
        CHECK(second.base.outfitName == "Same");
    }
    {  // ResetForLoad's whole documented purpose: after a load, the applied
       // state is forgotten, so the next Evaluate re-derives and re-applies
       // rather than reporting kNoChange for a target nothing has seen yet.
        RuleSet   rules = { MakeRule("r", 10, "A") };
        RuleEngine e;
        const auto first = e.Evaluate(rules, WorldSnapshot{});
        CHECK(first.outcome == Outcome::kApply);
        e.NoteApplied(first, 100.0);
        CHECK(e.Evaluate(rules, WorldSnapshot{}).outcome == Outcome::kNoChange);

        e.ResetForLoad();
        const auto afterLoad = e.Evaluate(rules, WorldSnapshot{});
        CHECK(afterLoad.outcome == Outcome::kApply);
        CHECK(afterLoad.ruleId == "r");
    }
    {  // pin accessor smoke test: SetPinned/ClearPin/IsPinned move the bits;
       // Evaluate's and NoteApplied's actual consultation of pinned_ is
       // covered by the dedicated pin tests further below.
        RuleEngine e;
        CHECK(!e.IsPinned());
        e.SetPinned("Foo");
        CHECK(e.IsPinned());
        e.ClearPin();
        CHECK(!e.IsPinned());
    }

    {  // inside the dwell window a different target is suppressed, with the
       // remaining time reported
        RuleSet rules = { MakeRule("a", 50, "A"), MakeRule("b", 10, "B") };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        const auto first = e.Evaluate(rules, s);
        CHECK(first.ruleId == "a");
        e.NoteApplied(first, 100.0);

        rules[0].enabled = false;  // "b" would win now
        s.nowSeconds     = 102.0;  // 2s into a 5s window
        const auto held  = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId == "b");
        CHECK(held.dwellRemaining > 2.9f && held.dwellRemaining < 3.1f);

        s.nowSeconds     = 106.0;  // past the window
        const auto lands = e.Evaluate(rules, s);
        CHECK(lands.outcome == Outcome::kApply);
        CHECK(lands.ruleId == "b");
    }
    {  // repeated Evaluate calls with nothing ever applied stay kApply:
       // hasApplied_ is false throughout, so there is no window to burn. (The
       // menu-cannot-act case is exercised for real further below, once a
       // target has actually been applied and there is a window to protect.)
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kApply);
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kApply);  // still applyable
        s.nowSeconds = 100.5;
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kApply);
    }
    {  // the no-rule target (overlay clear) is dwell-gated like any other
        RuleSet rules = { MakeRule("inside", 50, "A") };
        rules[0].conditions = { InteriorCond() };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.interior   = true;
        s.nowSeconds = 100.0;
        const auto applied = e.Evaluate(rules, s);
        e.NoteApplied(applied, 100.0);
        s.interior   = false;
        s.nowSeconds = 101.0;
        const auto held = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId.empty());
    }
    {  // entering combat bypasses dwell for a non-negated Combat rule
        RuleSet rules = { MakeRule("peace", 10, "A"), MakeRule("fight", 50, "B") };
        Condition combat;
        combat.kind = ConditionKind::kCombat;
        rules[1].conditions = { combat };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.inCombat   = false;
        const auto peace = e.Evaluate(rules, s);
        CHECK(peace.ruleId == "peace");
        e.NoteApplied(peace, 100.0);

        s.nowSeconds = 100.5;  // deep inside the window
        s.inCombat   = true;   // rising edge
        const auto fight = e.Evaluate(rules, s);
        CHECK(fight.outcome == Outcome::kApply);
        CHECK(fight.ruleId == "fight");
    }
    {  // crouching bypasses dwell for a Sneaking rule, same as entering combat
        RuleSet   rules = { MakeRule("stand", 10, "A"), MakeRule("crouch", 50, "B") };
        Condition sneak;
        sneak.kind          = ConditionKind::kSneaking;
        rules[1].conditions = { sneak };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.sneaking   = false;
        const auto standing = e.Evaluate(rules, s);
        CHECK(standing.ruleId == "stand");
        e.NoteApplied(standing, 100.0);

        s.nowSeconds = 100.5;  // deep inside the window
        s.sneaking   = true;
        const auto crouched = e.Evaluate(rules, s);
        CHECK(crouched.outcome == Outcome::kApply);
        CHECK(crouched.ruleId == "crouch");
    }
    {  // ...and STANDING BACK UP bypasses too, which is where sneak departs
       // from combat. Combat only exempts its rising edge, because a rule that
       // becomes true on combat exit is the peace outfit and nothing about it
       // is urgent. Crouch is a button the player just pressed in both
       // directions, so getting the previous look back has to be as prompt as
       // losing it was. Under the old combat-only rule this case suppressed.
        RuleSet   rules = { MakeRule("stand", 10, "A"), MakeRule("crouch", 50, "B") };
        Condition sneak;
        sneak.kind          = ConditionKind::kSneaking;
        rules[1].conditions = { sneak };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.sneaking   = true;
        const auto crouched = e.Evaluate(rules, s);
        CHECK(crouched.ruleId == "crouch");
        e.NoteApplied(crouched, 100.0);

        s.nowSeconds = 100.5;  // still deep inside the window
        s.sneaking   = false;  // falling edge
        const auto standing = e.Evaluate(rules, s);
        CHECK(standing.outcome == Outcome::kApply);
        CHECK(standing.ruleId == "stand");
    }
    {  // the exemption is per-kind: a sneak edge must NOT let an unrelated rule
       // skip the dwell. Without the per-clause check this passes the window to
       // whatever happens to be winning, which is exactly the flapping the
       // dwell exists to prevent.
        RuleSet   rules = { MakeRule("outside", 10, "A"), MakeRule("inside", 50, "B") };
        Condition interior;
        interior.kind       = ConditionKind::kInterior;
        rules[1].conditions = { interior };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.interior   = false;
        s.sneaking   = false;
        const auto outside = e.Evaluate(rules, s);
        CHECK(outside.ruleId == "outside");
        e.NoteApplied(outside, 100.0);

        s.nowSeconds = 100.5;
        s.interior   = true;  // the winner changes...
        s.sneaking   = true;  // ...and a sneak edge fires in the same tick
        const auto held = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);  // the winner has no sneak clause
        CHECK(held.ruleId == "inside");
    }
    {  // THE CAST HOLD CLOSING bypasses the dwell. This is the field report of
       // 2026-08-14, "it takes so long to switch back when we stop casting":
       // kCasting was missing from WantsBypass, so a casting rule was
       // dwell-gated in both directions. The falling edge is the one that was
       // complained about, so it is the one asserted first.
       //
       // ⚠ `casting` is a LATCH, not a live read. The engine sees a boolean
       // that WorldWatch holds true for fCastHoldSeconds, so the false here is
       // that window expiring rather than a key coming up.
        RuleSet   rules = { MakeRule("normal", 10, "A"), MakeRule("robe", 50, "B") };
        Condition cast;
        cast.kind           = ConditionKind::kCasting;
        rules[1].conditions = { cast };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.casting    = true;
        const auto robed = e.Evaluate(rules, s);
        CHECK(robed.ruleId == "robe");
        e.NoteApplied(robed, 100.0);

        s.nowSeconds = 100.5;  // deep inside the dwell window
        s.casting    = false;  // the hold expired
        const auto back = e.Evaluate(rules, s);
        CHECK(back.outcome == Outcome::kApply);
        CHECK(back.ruleId == "normal");
    }
    {  // ...and the cast itself bypasses too, which is what keeps the ARRIVAL
       // prompt when something else dressed the player moments earlier. The
       // sink queues its own evaluate, but an evaluate that arrives inside a
       // dwell window opened by an unrelated rule is still suppressed without
       // this.
        RuleSet   rules = { MakeRule("normal", 10, "A"), MakeRule("robe", 50, "B") };
        Condition cast;
        cast.kind           = ConditionKind::kCasting;
        rules[1].conditions = { cast };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.casting    = false;
        const auto normal = e.Evaluate(rules, s);
        CHECK(normal.ruleId == "normal");
        e.NoteApplied(normal, 100.0);

        s.nowSeconds = 100.5;
        s.casting    = true;
        const auto robed = e.Evaluate(rules, s);
        CHECK(robed.outcome == Outcome::kApply);
        CHECK(robed.ruleId == "robe");
    }
    {  // the casting exemption is per-kind like every other one: a cast edge
       // must not hand the window to a rule that says nothing about casting.
        RuleSet   rules = { MakeRule("outside", 10, "A"), MakeRule("inside", 50, "B") };
        rules[1].conditions = { InteriorCond() };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.interior   = false;
        s.casting    = false;
        const auto outside = e.Evaluate(rules, s);
        CHECK(outside.ruleId == "outside");
        e.NoteApplied(outside, 100.0);

        s.nowSeconds = 100.5;
        s.interior   = true;  // the winner changes...
        s.casting    = true;  // ...and a cast edge fires in the same tick
        const auto held = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId == "inside");
    }
    {  // ⚠⚠ THE BYPASS ALONE DOES NOT EXPLAIN THE FIELD REPORT, and this test
       // exists to keep that measured rather than argued. Run the LIVE numbers
       // (fCastHoldSeconds 6, fMinDwellSeconds 5) as a timeline: the cast lands
       // at t=0, so the dwell window closes at t=5, and the hold does not close
       // until t=6. The dwell is already over by then, so removing it changes
       // nothing about this particular shape - the wait the user feels is the
       // HOLD, plus however long the expiry takes to be noticed.
       //
       // What the bypass does fix is every shape where the two overlap: a hold
       // shorter than the dwell (which is where tuning this number wants to
       // go), or an unrelated rule that re-anchored the dwell clock after the
       // cast. The test above covers those. This one pins the reason the hold
       // itself still has to come down.
        RuleSet   rules = { MakeRule("normal", 10, "A"), MakeRule("robe", 50, "B") };
        Condition cast;
        cast.kind           = ConditionKind::kCasting;
        rules[1].conditions = { cast };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 0.0;
        s.casting    = true;  // the sink queues this evaluate
        const auto robed = e.Evaluate(rules, s);
        CHECK(robed.outcome == Outcome::kApply);
        e.NoteApplied(robed, 0.0);

        s.nowSeconds = 5.5;  // the dwell is over and the hold is not
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kNoChange);

        s.nowSeconds = 6.0;   // the hold closes
        s.casting    = false;
        const auto back = e.Evaluate(rules, s);
        CHECK(back.outcome == Outcome::kApply);
        CHECK(back.ruleId == "normal");
    }
    {  // leaving combat does NOT bypass: a negated Combat rule waits its turn
        RuleSet rules = { MakeRule("peace", 10, "A"), MakeRule("fight", 50, "B") };
        Condition combat;
        combat.kind = ConditionKind::kCombat;
        rules[1].conditions = { combat };
        Condition notCombat;
        notCombat.kind   = ConditionKind::kCombat;
        notCombat.negate = true;
        rules[0].conditions = { notCombat };

        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.inCombat   = true;
        const auto fight = e.Evaluate(rules, s);
        CHECK(fight.ruleId == "fight");
        e.NoteApplied(fight, 100.0);

        s.nowSeconds = 100.5;
        s.inCombat   = false;  // falling edge
        const auto held = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId == "peace");
    }
    {  // a rising edge alone is not enough: the WINNING rule must itself
       // carry a non-negated Combat clause. This is the case that actually
       // pins WantsCombatBypass - the "entering combat" and "leaving combat"
       // tests above would both still pass if it returned true
       // unconditionally, since the leaving-combat case short-circuits on
       // the edge direction before the winner's clauses ever matter.
        RuleSet rules = { MakeRule("a", 50, "A"), MakeRule("b", 10, "B") };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.inCombat   = false;
        const auto first = e.Evaluate(rules, s);
        CHECK(first.ruleId == "a");
        e.NoteApplied(first, 100.0);

        rules[0].enabled = false;  // "b" wins now, but "b" has no Combat clause
        s.nowSeconds     = 100.5;  // deep inside the window
        s.inCombat       = true;   // rising edge, but irrelevant to who wins
        const auto held  = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId == "b");
    }
    {  // the exemption survives an unapplied evaluation: the caller could not
       // act on the first kApply (a menu is open), so the bypass must still
       // be available on the next tick rather than being consumed by mere
       // observation
        RuleSet rules = { MakeRule("peace", 10, "A"), MakeRule("fight", 50, "B") };
        Condition combat;
        combat.kind = ConditionKind::kCombat;
        rules[1].conditions = { combat };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.inCombat   = false;
        const auto peace = e.Evaluate(rules, s);
        CHECK(peace.ruleId == "peace");
        e.NoteApplied(peace, 100.0);

        s.nowSeconds = 100.5;  // deep inside the window
        s.inCombat   = true;   // rising edge
        const auto fight1 = e.Evaluate(rules, s);
        CHECK(fight1.outcome == Outcome::kApply);
        CHECK(fight1.ruleId == "fight");
        // No NoteApplied here: the caller (a menu, in this scenario) could
        // not act on the decision.

        s.nowSeconds = 100.8;  // still inside the original dwell window
        const auto fight2 = e.Evaluate(rules, s);
        CHECK(fight2.outcome == Outcome::kApply);
        CHECK(fight2.ruleId == "fight");
    }
    {  // the exemption is spent once used: after NoteApplied consumes it, a
       // further different target inside the same window is suppressed
       // again, even though it also carries a non-negated Combat clause
        RuleSet rules = { MakeRule("peace", 10, "A"), MakeRule("fight", 50, "B"),
                          MakeRule("third", 30, "C") };
        Condition combat;
        combat.kind = ConditionKind::kCombat;
        rules[1].conditions = { combat };
        rules[2].conditions = { combat };  // "third" would also want the bypass
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        s.inCombat   = false;
        const auto peace = e.Evaluate(rules, s);
        CHECK(peace.ruleId == "peace");
        e.NoteApplied(peace, 100.0);

        s.nowSeconds = 100.5;  // rising edge, bypass consumed here
        s.inCombat   = true;
        const auto fight = e.Evaluate(rules, s);
        CHECK(fight.outcome == Outcome::kApply);
        CHECK(fight.ruleId == "fight");
        e.NoteApplied(fight, 100.5);

        rules[1].enabled = false;  // "third" wins now: still in combat, still
                                    // inside the window, still carries a
                                    // Combat clause - but the exemption was
                                    // already spent on "fight"
        s.nowSeconds     = 100.7;
        const auto held  = e.Evaluate(rules, s);
        CHECK(held.outcome == Outcome::kSuppressed);
        CHECK(held.ruleId == "third");
    }

    {  // a pin pauses the engine and reports the pinned name
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetPinned("Fancy Dress");
        CHECK(e.IsPinned());
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kPaused);
        CHECK(d.pinnedName == "Fancy Dress");
    }
    {  // pinning to real gear reports an empty name, still paused
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetPinned("");
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kPaused);
        CHECK(d.pinnedName.empty());
    }
    {  // resume lets the engine decide again on the very next evaluation
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetPinned("Fancy Dress");
        CHECK(e.Evaluate(rules, WorldSnapshot{}).outcome == Outcome::kPaused);
        e.ClearPin();
        CHECK(!e.IsPinned());
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.ruleId == "a");
    }
    {  // THE RESUME CASE. Pinning means the user is now wearing something the
       // engine did not put on them, so the applied target is stale and must
       // be forgotten. If it were kept, Evaluate after Resume would derive the
       // same target it "already applied", return kNoChange, and leave the
       // user stuck in the manually picked outfit forever.
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        const auto applied = e.Evaluate(rules, s);
        CHECK(applied.outcome == Outcome::kApply);
        e.NoteApplied(applied, 100.0);
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kNoChange);  // steady state

        e.SetPinned("Fancy Dress");
        s.nowSeconds = 200.0;
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kPaused);

        e.ClearPin();
        const auto resumed = e.Evaluate(rules, s);
        CHECK(resumed.outcome == Outcome::kApply);  // NOT kNoChange
        CHECK(resumed.ruleId == "a");
    }
    {  // resume is not suppressed by a dwell window left over from before the
       // pin: the user asked for the rules back, they get them now
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetMinDwellSeconds(5.0f);
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        e.NoteApplied(e.Evaluate(rules, s), 100.0);
        e.SetPinned("Fancy Dress");
        s.nowSeconds = 101.0;  // still inside the old window
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kPaused);
        e.ClearPin();
        CHECK(e.Evaluate(rules, s).outcome == Outcome::kApply);
    }
    {  // NoteApplied must ignore a decision derived BEFORE the pin but
       // reported AFTER it. In the real engine, applying is deferred through
       // RequestRefresh/RunPlayerRefreshWhenSafe behind a BlockingCooldown
       // gate, so Evaluate and NoteApplied are not adjacent calls - a pin can
       // land in between. Recording the stale decision would silently undo
       // SetPinned's forgetting of the applied target and reintroduce THE
       // RESUME CASE bug: the user stuck in the manually picked outfit.
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        WorldSnapshot s;
        s.nowSeconds = 100.0;
        const auto applied = e.Evaluate(rules, s);
        CHECK(applied.outcome == Outcome::kApply);
        e.NoteApplied(applied, 100.0);

        e.SetPinned("Fancy Dress");
        // The stale decision (derived before the pin) arrives late.
        e.NoteApplied(applied, 150.0);

        e.ClearPin();
        const auto resumed = e.Evaluate(rules, s);
        CHECK(resumed.outcome == Outcome::kApply);  // NOT kNoChange
        CHECK(resumed.ruleId == "a");
    }
    {  // NoteApplied ignores a kPaused decision too: the natural caller shape
       // is "d = Evaluate(...); NoteApplied(d, now);" called unconditionally,
       // so a kPaused Decision reaches NoteApplied in production, and its
       // guard has to tolerate that without side effects.
        RuleSet rules = { MakeRule("a", 50, "A") };
        RuleEngine e;
        e.SetPinned("Fancy Dress");
        const auto paused = e.Evaluate(rules, WorldSnapshot{});
        CHECK(paused.outcome == Outcome::kPaused);
        e.NoteApplied(paused, 100.0);  // must be a no-op

        e.ClearPin();
        const auto resumed = e.Evaluate(rules, WorldSnapshot{});
        CHECK(resumed.outcome == Outcome::kApply);
        CHECK(resumed.ruleId == "a");
    }

    {  // ADDITIVE COMPOSITION - the Helmet Toggle case.
       //
       // An overlay-only rule (base kKeep) must layer ON TOP of a lower-priority
       // outfit rule, not replace it. Before composition went additive this was
       // the field bug: the pack's helmet rule outranked the user's outfit rule,
       // won outright, and its kKeep base cancelled the outfit entirely.
        Rule outfit       = MakeRule("outfit", 5, "Town Clothes");
        Rule hideHelmet;
        hideHelmet.id          = "helmet";
        hideHelmet.name        = "helmet";
        hideHelmet.priority    = 50;              // outranks the outfit rule
        hideHelmet.base.kind   = BaseKind::kKeep;  // expresses no opinion on the outfit
        hideHelmet.overlay[1]  = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };

        RuleSet    rules = { outfit, hideHelmet };
        RuleEngine e;
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kApply);
        // The outfit survives even though the helmet rule outranks it...
        CHECK(d.base.kind == BaseKind::kOutfit);
        CHECK(d.base.outfitName == "Town Clothes");
        CHECK(d.ruleId == "outfit");  // the base came from the outfit rule
        // ...and the overlay is applied on top of it.
        CHECK(d.overlay.size() == 1);
        CHECK(d.overlay.count(1) == 1);
        // Both rules are reported live; the highest-priority MATCH is the
        // helmet rule even though it set no base.
        CHECK(d.matchedRuleId == "helmet");
        CHECK(d.activeRuleIds.size() == 2);
        if (d.activeRuleIds.size() == 2) {
            CHECK(d.activeRuleIds[0] == "helmet");  // highest priority first
            CHECK(d.activeRuleIds[1] == "outfit");
        }
    }
    {  // Overlays merge PER SLOT across every matching rule, and a contested
       // slot goes to the higher-priority rule.
        Rule low;
        low.id           = "low";
        low.priority     = 10;
        low.base.kind    = BaseKind::kKeep;
        low.overlay[1]   = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };
        low.overlay[2]   = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };

        Rule high;
        high.id          = "high";
        high.priority    = 20;
        high.base.kind   = BaseKind::kKeep;
        // Same slot 1 as `low`, but passthrough - the higher rule wins it.
        high.overlay[1]  = OS::SlotEntry{};
        high.overlay[7]  = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };

        RuleSet    rules = { low, high };
        RuleEngine e;
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.overlay.size() == 3);                              // 1, 2 and 7 all present
        CHECK(d.overlay.at(1).kind == OS::SlotEntry::Kind::kPassthrough);  // high won the contest
        CHECK(d.overlay.at(2).kind == OS::SlotEntry::Kind::kHide);      // only low touched it
        CHECK(d.overlay.at(7).kind == OS::SlotEntry::Kind::kHide);      // only high touched it
        CHECK(d.ruleId.empty());                                    // neither named a base
        CHECK(d.base.kind == BaseKind::kKeep);
    }
    {  // A rule that MATCHES but does nothing is not reported as live.
       //
       // This is the shape of a freshly created rule: no conditions, so it
       // matches everything, and no effects. "+ New Rule" inserts at the top of
       // the list, so without this it would take matchedRuleId from the rule
       // actually dressing the player and put a gold "live" wash on a card
       // doing nothing at all.
        Rule fresh;
        fresh.id        = "fresh";
        fresh.priority  = 99;                 // outranks everything
        fresh.base.kind = BaseKind::kKeep;    // ...and contributes nothing

        RuleSet    rules = { fresh, MakeRule("outfit", 5, "Town Clothes") };
        RuleEngine e;
        const auto d = e.Evaluate(rules, WorldSnapshot{});
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.base.outfitName == "Town Clothes");  // the outfit still lands
        CHECK(d.matchedRuleId == "outfit");          // NOT "fresh"
        CHECK(d.activeRuleIds.size() == 1);
        if (d.activeRuleIds.size() == 1) {
            CHECK(d.activeRuleIds[0] == "outfit");
        }
    }
    {  // A non-matching rule contributes nothing, overlay included.
        Rule indoorsOnly;
        indoorsOnly.id         = "indoors";
        indoorsOnly.priority   = 90;
        indoorsOnly.base.kind  = BaseKind::kKeep;
        indoorsOnly.overlay[1] = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };
        indoorsOnly.conditions = { InteriorCond() };

        RuleSet       rules = { MakeRule("outfit", 5, "Town Clothes"), indoorsOnly };
        RuleEngine    e;
        WorldSnapshot outside;  // interior stays false
        const auto    d = e.Evaluate(rules, outside);
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.overlay.empty());
        CHECK(d.activeRuleIds.size() == 1);
        CHECK(d.matchedRuleId == "outfit");
    }

    {  // ⚠ A PACK RULE NEVER EVALUATES. The Rule Library is a CATALOG you import
       // from, not a set of rules that run. This reverses the original design,
       // where author-pack rules were live and only their enable checkbox was
       // yours; the field showed what that actually means, which is a pack rule
       // sitting in the library marked Active and dressing the player. Nobody
       // chose that rule, so it does not get to win.
       //
       // Keyed on packName rather than on `enabled`, because enabled is the
       // player's switch for THEIR rules and a pack rule no longer has one.
       // Importing copies the rule into the save's own set, which clears
       // packName, and from that moment it is an ordinary rule that runs.
        Rule pack        = MakeRule("frompack", 90, "PackOutfit");
        pack.packName    = "HelmetToggle.json";
        Rule mine        = MakeRule("mine", 10, "MyOutfit");

        RuleEngine e;
        const auto d = e.Evaluate(RuleSet{ pack, mine }, WorldSnapshot{});
        // The pack rule has FAR the higher priority and matches unconditionally,
        // so if it were evaluated at all it would win outright.
        CHECK(d.outcome == Outcome::kApply);
        CHECK(d.matchedRuleId == "mine");

        // And a library with nothing but pack rules decides nothing.
        const auto none = e.Evaluate(RuleSet{ pack }, WorldSnapshot{});
        CHECK(none.matchedRuleId.empty());

        // ⚠ Importing is what makes it live, and that is the same rule with its
        // pack tag gone rather than a different code path.
        Rule imported     = pack;
        imported.packName.clear();
        const auto after = e.Evaluate(RuleSet{ imported, mine }, WorldSnapshot{});
        CHECK(after.matchedRuleId == "frompack");
    }

    {  // ---- the exit apply, whose polarity is inverted on purpose ----------
        using OS::Rules::ShouldApplyOnEditorExit;
        constexpr bool kEngineOn  = true;
        constexpr bool kEngineOff = false;
        constexpr bool kTouched   = true;
        constexpr bool kJustRead  = false;

        // ⚠ THE ONE THAT LOOKS WRONG AND IS NOT. Auto switching ON suppresses
        // the exit apply. The heartbeat is already going to re-evaluate within
        // a couple of seconds, and a hand-picked outfit has to survive closing
        // the editor - "clear the pin on close" was proposed on 2026-08-04 and
        // declined the same day for exactly that reason. Anyone "fixing" this
        // line to the intuitive polarity breaks both.
        CHECK(!ShouldApplyOnEditorExit(kEngineOn, kTouched));
        CHECK(!ShouldApplyOnEditorExit(kEngineOn, kJustRead));

        // Engine off is who this is for: the rules never fire on their own, so
        // leaving the editor is the one predictable moment to run them.
        CHECK(ShouldApplyOnEditorExit(kEngineOff, kTouched));

        // ...but only after the session actually touched the outfits. Opening
        // the editor to read the Rules tab and closing it again must not
        // redress anybody.
        CHECK(!ShouldApplyOnEditorExit(kEngineOff, kJustRead));
    }

    {  // ---- and the pin, which is a SEPARATE question ----------------------
        using OS::Rules::ShouldClearPinOnEditorExit;
        constexpr bool kTouched     = true;
        constexpr bool kJustRead    = false;
        constexpr bool kOnRulesPick = true;
        constexpr bool kHandPicked  = false;

        // ⚠ NOT GATED ON THE ENGINE, unlike the apply above, and that is the
        // fix for the complaint that started this: the narrow test only ever
        // resumed when the editor was left on the outfit the RULES had chosen,
        // which is exactly the case where nothing had been picked by hand. So
        // every real outfit switch ended paused with a Resume button to find,
        // over and over (user 2026-08-06).
        CHECK(ShouldClearPinOnEditorExit(kTouched, kHandPicked));
        CHECK(ShouldClearPinOnEditorExit(kTouched, kOnRulesPick));

        // The old narrow case still resumes on its own, so a session that
        // changed nothing but was sitting on the rules' pick is unaffected.
        CHECK(ShouldClearPinOnEditorExit(kJustRead, kOnRulesPick));

        // ⚠ THE ONE THAT MUST STAY FALSE. Opening the editor to read something
        // and closing it again leaves an existing pin exactly where it was;
        // otherwise merely looking at the Rules tab would undo a pause the user
        // set deliberately.
        CHECK(!ShouldClearPinOnEditorExit(kJustRead, kHandPicked));
    }

    {  // ---- is there anything for a pin to hold off ------------------------
        //
        // The Rules tab announced "Auto switching paused" with a Resume button
        // over a save with no rules in it, which is a pause on nothing and a
        // click to leave a state with no effect (user 2026-08-08).
        using OS::Rules::AnyRuleCanApply;

        CHECK(!AnyRuleCanApply({}));

        OS::Rules::Rule live;
        live.id      = "live";
        live.enabled = true;
        CHECK(AnyRuleCanApply({ live }));

        // ⚠ THE TWO KINDS THAT CANNOT WIN, and skipping them is what makes this
        // worth writing instead of asking whether the vector is empty. A rule
        // the player switched off, and a rule the loader kept but marked
        // invalid, both reach no decision, so a save holding only those has
        // nothing to pause.
        OS::Rules::Rule off;
        off.id      = "off";
        off.enabled = false;
        CHECK(!AnyRuleCanApply({ off }));

        OS::Rules::Rule broken;
        broken.id      = "broken";
        broken.enabled = true;
        broken.invalid = true;
        CHECK(!AnyRuleCanApply({ broken }));

        CHECK(!AnyRuleCanApply({ off, broken }));
        // One live rule among dead ones is still a reason to pause.
        CHECK(AnyRuleCanApply({ off, broken, live }));
    }

    if (g_failures == 0) {
        std::printf("all RuleEngine tests passed\n");
    }
    return g_failures;
}
