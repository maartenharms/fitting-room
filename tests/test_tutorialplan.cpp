// Pure-logic tests for the onboarding tutorial's fire-once decision.
// No engine, no RE:: types, no FUCK.
#include "TutorialPlan.h"

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
    using namespace OS::TutorialPlan;

    {  // A fresh install has seen nothing and every tutorial is waiting
        CHECK(!Seen(kNothingSeen, Id::kWelcome));
        CHECK(!Seen(kNothingSeen, Id::kOutfits));
        CHECK(ShouldFire(kNothingSeen, Id::kWelcome, 2, true, false, true));
    }
    {  // Finishing is once, and it is local to the one tutorial
        const auto after = Finish(kNothingSeen, Id::kOutfits);
        CHECK(Seen(after, Id::kOutfits));
        CHECK(!ShouldFire(after, Id::kOutfits, 4, true, false, true));
        // ⚠ THE ONE THAT MATTERS. If finishing any tutorial marked the rest,
        // a page installed later would be counted as taught and never shown.
        // That is the environment-keyed failure this whole design exists to
        // avoid; see the header.
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            const auto other = static_cast<Id>(raw);
            if (other == Id::kOutfits) {
                continue;
            }
            CHECK(!Seen(after, other));
            CHECK(ShouldFire(after, other, 3, true, false, true));
        }
    }
    {  // Skip counts as done, which is the same write as finishing (user
       // 2026-08-08). Asserted as an identity so the two endings cannot drift.
        CHECK(Finish(kNothingSeen, Id::kDye) == MarkSeen(kNothingSeen, Id::kDye));
    }
    {  // Every tutorial gets its own bit, so none of them can shadow another
        std::uint32_t all = kNothingSeen;
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            const auto id = static_cast<Id>(raw);
            CHECK(!Seen(all, id));  // not already claimed by an earlier one
            all = Finish(all, id);
            CHECK(Seen(all, id));
        }
        // Seven distinct bits set, nothing collided.
        std::uint32_t counted = 0;
        for (std::uint32_t m = all; m != 0u; m &= m - 1u) {
            ++counted;
        }
        CHECK(counted == static_cast<std::uint32_t>(Id::kCount));
    }
    {  // The three refusals
        CHECK(!ShouldFire(kNothingSeen, Id::kShape, 0, true, false, true));   // no cards written yet
        CHECK(!ShouldFire(kNothingSeen, Id::kShape, 3, false, false, true));  // page has nothing on it
        CHECK(!ShouldFire(kNothingSeen, Id::kShape, 3, true, true, true));    // another modal is up
    }
    {  // Replaying puts every tutorial back, not just the last one
        std::uint32_t all = kNothingSeen;
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            all = Finish(all, static_cast<Id>(raw));
        }
        CHECK(all != kNothingSeen);
        const std::uint32_t replayed = kNothingSeen;
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            CHECK(ShouldFire(replayed, static_cast<Id>(raw), 2, true, false, true));
        }
    }
    {  // The card cap the user set is the one the step tables are held to.
       // Four until 2026-08-16, six from then: the same user who set the
       // original ceiling asked for more cards on the Rules page, and the
       // reasoning behind the first number ("past about six people skip") is
       // what picks the second. A test rather than a comment because the cap
       // exists to be enforced.
        CHECK(kMaxSteps == 6);
    }
    {  // Saying no on the first card turns every one of them off, including the
       // ones that do not exist yet. ⚠ THIS IS THE REASON IT IS A SETTING and
       // not seven flags: a tutorial added later has no key in anyone's INI, so
       // it defaults to unseen, and marking the current seven would let it
       // through to someone who already declined.
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            CHECK(!ShouldFire(kNothingSeen, static_cast<Id>(raw), 3, true, false, false));
        }
        // And turning them back on brings them back, rather than leaving a
        // player who changed their mind with nothing.
        CHECK(ShouldFire(kNothingSeen, Id::kWelcome, 2, true, false, true));
    }

    {  // A run cut short by a missing requirement has not taught the page
        // ⚠ THE LOOKS CASE, and the reason this exists. A brand new install has
        // an empty library, so the card that says to click a look and the card
        // that explains the per part ticks are both dropped: two of four. Marking
        // it seen there is the environment-keyed verdict the header refuses, one
        // level down, and the player never sees those two cards again.
        CHECK(!ShouldMarkSeen(2, 4, false));
        CHECK(ShouldMarkSeen(4, 4, false));
        // ⚠ AND SKIP IS STILL FINAL. Someone who dismissed a two card run said
        // no to the tutorial, not to the empty library, and a dismissal that
        // comes back is worse than one that never ran.
        CHECK(ShouldMarkSeen(2, 4, true));
        CHECK(ShouldMarkSeen(0, 4, true));
        // ⚠⚠ THE OUTFITS CASE, which must NOT retry. The follower step is
        // dropped for a solo player and nobody waits on a follower, so the
        // caller leaves it out of the total and the run counts as taught.
        // Retrying it would put three cards up every time the editor opened,
        // because Outfits is the page the editor opens on.
        CHECK(ShouldMarkSeen(3, 3, false));
        // A plan that showed MORE than the total cannot happen, but if the two
        // counts ever drift the safe answer is taught rather than forever.
        CHECK(ShouldMarkSeen(5, 4, false));
    }

    if (g_failures == 0) {
        std::printf("TutorialPlanTests: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
