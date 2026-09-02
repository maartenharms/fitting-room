#include "RequipFlourish.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static bool Near(float a_l, float a_r) {
    const float d = a_l - a_r;
    return (d < 0.0f ? -d : d) < 1e-4f;
}

int main() {
    using namespace OS::RequipFlourish;

    // ---- the endpoints, which are the part a bug hides in ----------------
    {
        // Burn starts at the original and ends fully at the peak.
        CHECK(Near(BurnMix(0.0), 0.0f));
        CHECK(Near(BurnMix(kBurnSeconds), 1.0f));
        CHECK(Near(BurnMix(kBurnSeconds * 2.0), 1.0f));  // clamped past the end
        CHECK(Near(BurnMix(-1.0), 0.0f));                // clamped before the start

        // ⚠ THE ONE THAT MATTERS. A garment left part-way transparent is
        // invisible armour, so Burn must reach EXACTLY zero alpha, not near it.
        CHECK(BurnAlpha(kBurnSeconds) == 0.0f);
        CHECK(BurnAlpha(kBurnSeconds * 2.0) == 0.0f);
        CHECK(Near(BurnAlpha(0.0), 1.0f));
        CHECK(Near(BurnAlpha(-1.0), 1.0f));

        // Condense starts at the peak and lands exactly back on the original.
        CHECK(Near(CondenseMix(0.0), 1.0f));
        CHECK(CondenseMix(kCondenseSeconds) == 0.0f);
        CHECK(CondenseMix(kCondenseSeconds * 2.0) == 0.0f);
    }

    // ---- the shape claim the design rests on ------------------------------
    {
        // "Bright before it is gone": at the halfway point the garment is
        // already mostly lit while still mostly solid. Without this the piece
        // fades out grey and the flash has nothing to hide the cut behind.
        const double half = kBurnSeconds * 0.5;
        CHECK(BurnMix(half) > 0.6f);
        CHECK(BurnAlpha(half) > 0.8f);
    }

    // ---- monotonic, because a curve that doubles back reads as a stutter --
    {
        float lastMix = -1.0f, lastAlpha = 2.0f;
        for (int i = 0; i <= 20; ++i) {
            const double t   = kBurnSeconds * (static_cast<double>(i) / 20.0);
            const float  mix = BurnMix(t);
            const float  a   = BurnAlpha(t);
            CHECK(mix >= lastMix);
            CHECK(a <= lastAlpha);
            CHECK(mix >= 0.0f && mix <= 1.0f);
            CHECK(a >= 0.0f && a <= 1.0f);
            lastMix   = mix;
            lastAlpha = a;
        }
        float lastCond = 2.0f;
        for (int i = 0; i <= 20; ++i) {
            const double t = kCondenseSeconds * (static_cast<double>(i) / 20.0);
            const float  c = CondenseMix(t);
            CHECK(c <= lastCond);
            CHECK(c >= 0.0f && c <= 1.0f);
            lastCond = c;
        }
    }

    // ---- the whole flourish fits the budget the user agreed to -----------
    {
        CHECK(kBurnSeconds + kCondenseSeconds < 0.55);
        CHECK(kPeakEmissiveMult > 6.0f);  // above DyeFlash's, or it reads as a dye flash
    }

    // ---- the phase machine ------------------------------------------------
    {
        constexpr bool kAlive = true, kGone = false;
        constexpr bool kLive = true, kDead = false;
        constexpr bool kSame = false, kNewSwap = true;
        constexpr bool kReady = true, kNotReady = false;

        // ⚠⚠ THE CLAIM THE WHOLE DESIGN RESTS ON. RequestRefresh POSTS. Wait
        // may sit for one frame or for several, and no elapsed time proves the
        // rebuild landed. Only the geometry does.
        CHECK(NextPhase(Phase::kWait, kNotReady, 0.0) == Phase::kWait);
        CHECK(NextPhase(Phase::kWait, kNotReady, 999.0) == Phase::kWait);
        CHECK(NextPhase(Phase::kWait, kReady, 0.0) == Phase::kCondense);

        // Burn is the opposite: it owns its own clock and nothing else ends it.
        CHECK(NextPhase(Phase::kBurn, kReady, 0.0) == Phase::kBurn);
        CHECK(NextPhase(Phase::kBurn, kNotReady, kBurnSeconds) == Phase::kWait);

        // Condense runs to its own end and Classify takes it down from there.
        CHECK(NextPhase(Phase::kCondense, kReady, 0.0) == Phase::kCondense);
        CHECK(NextPhase(Phase::kIdle, kReady, 999.0) == Phase::kIdle);

        // ---- teardown, and the order it asks its questions in --------------
        // Nothing armed is a no-op, never a restore. Restoring here would write
        // through records that do not exist.
        CHECK(Classify(Phase::kIdle, kAlive, kLive, kSame, 0.0, 0) == Teardown::kNotArmed);
        CHECK(!EndsIt(Teardown::kNotArmed));

        // Mid-burn with everything healthy: it holds.
        CHECK(Classify(Phase::kBurn, kAlive, kLive, kSame, 0.0, 0) == Teardown::kHold);
        CHECK(!EndsIt(Teardown::kHold));

        // ⚠ ORDER IS DELIBERATE AND IT IS NOT "SOONEST FIRST", the lesson
        // DyeFlash::Classify already paid for. A flourish that both lost its
        // actor and blew its wait budget in the same frame reports the actor,
        // because a wrong reason in the log is how the next session hunts a
        // timer that was never involved.
        CHECK(Classify(Phase::kWait, kGone, kLive, kSame, 0.0, kWaitFrameBudget) ==
              Teardown::kActorGone);
        CHECK(Classify(Phase::kWait, kGone, kDead, kSame, 0.0, 0) == Teardown::kSessionEnded);
        CHECK(Classify(Phase::kBurn, kAlive, kLive, kNewSwap, 0.0, 0) == Teardown::kSuperseded);

        // The backstop, and the only reason that accepts a visible pop.
        CHECK(Classify(Phase::kWait, kAlive, kLive, kSame, 0.0, kWaitFrameBudget) ==
              Teardown::kWaitExpired);
        CHECK(Classify(Phase::kWait, kAlive, kLive, kSame, 0.0, kWaitFrameBudget - 1) ==
              Teardown::kHold);

        // The ordinary ending.
        CHECK(Classify(Phase::kCondense, kAlive, kLive, kSame, kCondenseSeconds, 0) ==
              Teardown::kFinished);
        CHECK(Classify(Phase::kCondense, kAlive, kLive, kSame, kCondenseSeconds - 0.01, 0) ==
              Teardown::kHold);

        // Every reason except the two no-ops puts the garments back. A reason
        // that does not is how a garment stays at alpha 0.
        CHECK(EndsIt(Teardown::kActorGone));
        CHECK(EndsIt(Teardown::kSessionEnded));
        CHECK(EndsIt(Teardown::kSuperseded));
        CHECK(EndsIt(Teardown::kWaitExpired));
        CHECK(EndsIt(Teardown::kFinished));

        // Burn's clock must not end the flourish. Only NextPhase moves it on.
        CHECK(Classify(Phase::kBurn, kAlive, kLive, kSame, 999.0, 0) == Teardown::kHold);
    }

    if (g_failures == 0) {
        std::printf("test_requipflourish: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
