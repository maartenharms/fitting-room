#include "DyeFlash.h"

#include <cstdio>
#include <initializer_list>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using OS::kDyeChannelCount;
    using OS::DyeFlash::Classify;
    using OS::DyeFlash::EndsIt;
    using OS::DyeFlash::Key;
    using OS::DyeFlash::kHoldSeconds;
    using OS::DyeFlash::OwnsNothing;
    using OS::DyeFlash::ShapesOwnedByChannel;
    using OS::DyeFlash::Teardown;
    using OS::DyeFlash::WorthFlashing;

    // Named so a call reads as something other than five bare booleans.
    constexpr bool kArmed    = true;
    constexpr bool kIdle     = false;
    constexpr bool kOpen     = true;
    constexpr bool kClosed   = false;
    constexpr bool kSameTgt  = true;
    constexpr bool kNewTgt   = false;
    constexpr bool kSame     = false;  // not superseded
    constexpr bool kNewClick = true;

    // ---- the restore rules, which are the point of the file ---------------
    {
        // Lit, editor up, same target, clock running: stays lit. This is the
        // ONLY state other than "nothing armed" that does not restore.
        CHECK(Classify(kArmed, kOpen, kSameTgt, kSame, 0.0, kHoldSeconds) == Teardown::kHold);
        CHECK(!EndsIt(Classify(kArmed, kOpen, kSameTgt, kSame, 0.0, kHoldSeconds)));

        // Nothing armed is not a teardown, it is a no-op. Restoring here would
        // write through a record that does not exist.
        CHECK(Classify(kIdle, kOpen, kSameTgt, kSame, 0.0, kHoldSeconds) == Teardown::kNotArmed);
        CHECK(!EndsIt(Teardown::kNotArmed));

        // The three the brief names, each on its own.
        CHECK(Classify(kArmed, kClosed, kSameTgt, kSame, 0.0, kHoldSeconds) ==
              Teardown::kEditorClosed);
        CHECK(Classify(kArmed, kOpen, kNewTgt, kSame, 0.0, kHoldSeconds) ==
              Teardown::kTargetChanged);
        CHECK(Classify(kArmed, kOpen, kSameTgt, kNewClick, 0.0, kHoldSeconds) ==
              Teardown::kSuperseded);

        // ...and the clock.
        CHECK(Classify(kArmed, kOpen, kSameTgt, kSame, kHoldSeconds, kHoldSeconds) ==
              Teardown::kExpired);
        // Exactly at the deadline counts as expired. A strictly-greater test
        // would hold one extra frame, which is harmless, but the boundary
        // should be stated rather than discovered.
        CHECK(Classify(kArmed, kOpen, kSameTgt, kSame, 10.0, 10.0) == Teardown::kExpired);
        CHECK(Classify(kArmed, kOpen, kSameTgt, kSame, 9.999, 10.0) == Teardown::kHold);

        // ⚠ EVERY REASON RESTORES. This is the property the feature rests on:
        // a shape left glowing is worse than no flash, so anything that is not
        // "hold" or "nothing armed" has to take it down.
        for (const auto t : { Teardown::kEditorClosed, Teardown::kTargetChanged,
                              Teardown::kSuperseded, Teardown::kExpired }) {
            CHECK(EndsIt(t));
        }
    }

    // ---- precedence, which only the log line depends on --------------------
    {
        // All four true at once. The editor is reported, because that is the
        // thing that actually happened; blaming the timer would send the next
        // session hunting a clock that was never involved.
        CHECK(Classify(kArmed, kClosed, kNewTgt, kNewClick, 99.0, 1.0) ==
              Teardown::kEditorClosed);
        // Editor still up: the target wins over both the click and the clock.
        CHECK(Classify(kArmed, kOpen, kNewTgt, kNewClick, 99.0, 1.0) ==
              Teardown::kTargetChanged);
        // Same target: a fresh click wins over the clock, so a stripe clicked
        // on the very frame the last one expired reads as replaced.
        CHECK(Classify(kArmed, kOpen, kSameTgt, kNewClick, 99.0, 1.0) == Teardown::kSuperseded);
        // Not armed beats everything: there is nothing to blame.
        CHECK(Classify(kIdle, kClosed, kNewTgt, kNewClick, 99.0, 1.0) == Teardown::kNotArmed);
    }

    // ---- which shapes a stripe owns ---------------------------------------
    {
        // The tier-1 binding: every channel but the last takes one shape, and
        // the last absorbs every shape from its own index on.
        //
        // ⚠ WRITTEN AGAINST THE CONSTANT, not against its value, so widening
        // the cap moves this one line rather than every expectation under it.
        CHECK(kDyeChannelCount == 16);
        constexpr auto kLast = kDyeChannelCount - 1;

        // A three-shape garment, the ordinary case.
        CHECK(ShapesOwnedByChannel(0, 3) == 1);
        CHECK(ShapesOwnedByChannel(2, 3) == 1);
        CHECK(ShapesOwnedByChannel(3, 3) == 0);      // stripe past the end
        CHECK(ShapesOwnedByChannel(kLast, 3) == 0);  // the tail channel, no tail

        // A garment past the OLD cap of eight: channel 7 owns its own shape and
        // nothing more, which is exactly what widening bought.
        CHECK(ShapesOwnedByChannel(7, 12) == 1);
        CHECK(ShapesOwnedByChannel(6, 12) == 1);

        // A busy garment: the last stripe owns everything from its index on.
        CHECK(ShapesOwnedByChannel(kLast, kDyeChannelCount) == 1);
        CHECK(ShapesOwnedByChannel(kLast, kLast + 5) == 5);

        // Nothing worn, nothing owned.
        CHECK(ShapesOwnedByChannel(0, 0) == 0);
        // A channel past the array cannot own anything, whatever is worn.
        CHECK(ShapesOwnedByChannel(kDyeChannelCount, 12) == 0);
        CHECK(ShapesOwnedByChannel(99, 12) == 0);
    }

    // ---- what arms, and the one thing that does not -------------------------
    {
        // ⚠ SINGLE-SHAPE GEAR ARMS (OS-164, user 2026-08-07). It did not until
        // then: the one stripe owns the whole garment, so the flash lights all
        // of it and picks nothing out, and that was read as not worth arming.
        // The user asked for it anyway, for standardisation, and the case is
        // the MAJORITY rather than a corner - `LiveChannelCount`'s comment puts
        // 52.1% of worn armour meshes at exactly one shape, so the old rule
        // meant most clicks in the game lit nothing.
        CHECK(WorthFlashing(0, 1));
        CHECK(!OwnsNothing(0, 1));  // it owns something; it just owns everything

        // Two shapes: each stripe picks one out, which is the feature working.
        CHECK(WorthFlashing(0, 2));
        CHECK(WorthFlashing(1, 2));

        // A stripe past the end owns nothing, and it must never arm: a flash
        // with no shapes has nothing to restore, so the record would sit armed
        // with an empty list until something else took it down. This is the
        // only refusal left.
        CHECK(OwnsNothing(4, 3));
        CHECK(!WorthFlashing(4, 3));

        // Nothing worn at all owns nothing either.
        CHECK(OwnsNothing(0, 0));
        CHECK(!WorthFlashing(0, 0));

        // The tail channel picks one shape out of eight, so it arms like any
        // other stripe that owns part of a garment.
        CHECK(WorthFlashing(7, 8));
        CHECK(ShapesOwnedByChannel(7, 8) == 1);
    }

    // ---- the burst curve ---------------------------------------------------
    {
        using OS::DyeFlash::BurstMultAt;
        using OS::DyeFlash::kBurstPulses;
        using OS::DyeFlash::kDarkEmissiveMult;
        using OS::DyeFlash::kLitEmissiveMult;

        const double hold = kHoldSeconds;
        const auto   near = [](float a_a, float a_b) {
            const float d = a_a - a_b;
            return (d < 0.0f ? -d : d) < 0.001f;
        };

        // ⚠ DARK AT BOTH ENDS OF THE BURST. The last pulse has to land on dark
        // or the restore underneath it pops: the flash forces emissiveColor to
        // white, so ending mid-pulse would drop a lit shape straight back to
        // its own colour in one frame.
        CHECK(near(BurstMultAt(0.0, hold), kDarkEmissiveMult));
        CHECK(near(BurstMultAt(hold, hold), kDarkEmissiveMult));
        // Past the end, and before the start, are both dark rather than
        // wrapping round to another pulse.
        CHECK(near(BurstMultAt(hold * 2.0, hold), kDarkEmissiveMult));
        CHECK(near(BurstMultAt(-1.0, hold), kDarkEmissiveMult));
        // A zero or negative hold cannot divide, and answers dark.
        CHECK(near(BurstMultAt(0.5, 0.0), kDarkEmissiveMult));
        CHECK(near(BurstMultAt(0.5, -1.0), kDarkEmissiveMult));

        // Peak in the middle of each pulse, trough at every boundary. This is
        // what makes it read as three flashes rather than one long shimmer.
        for (int i = 0; i < kBurstPulses; ++i) {
            const double pulse = hold / kBurstPulses;
            const double start = pulse * i;
            CHECK(near(BurstMultAt(start + pulse * 0.5, hold), kLitEmissiveMult));
            if (i > 0) {
                CHECK(near(BurstMultAt(start, hold), kDarkEmissiveMult));
            }
        }

        // Monotonic on the way up through the first pulse, so the curve is a
        // pulse rather than something that flickers inside itself.
        const double pulse = hold / kBurstPulses;
        float        prev  = -1.0f;
        for (int i = 0; i <= 10; ++i) {
            const float v = BurstMultAt(pulse * 0.5 * (i / 10.0), hold);
            CHECK(v >= prev - 0.001f);
            prev = v;
        }

        // Never above the peak, never below dark, anywhere in the burst.
        for (int i = 0; i <= 200; ++i) {
            const float v = BurstMultAt(hold * (i / 200.0), hold);
            CHECK(v <= kLitEmissiveMult + 0.001f);
            CHECK(v >= kDarkEmissiveMult - 0.001f);
        }
    }

    // ---- the key is compared, never followed -------------------------------
    {
        const Key a{ OS::DyeTarget::kArmour, 2, OS::WeaponClass::Sword, OS::WeaponHand::Both, 1 };
        Key       b = a;
        CHECK(a == b);
        b.channel = 2;
        CHECK(!(a == b));
        b         = a;
        b.slotBit = 3;
        CHECK(!(a == b));
        // ⚠ THE DIMENSION HAS TO COUNT. Armour slot 2 channel 1 and a weapon
        // channel 1 are different stripes, and a key that ignored target would
        // treat clicking one as re-clicking the other, which reads as the
        // flash refusing to move.
        b        = a;
        b.target = OS::DyeTarget::kWeapon;
        CHECK(!(a == b));
        b             = a;
        b.target      = OS::DyeTarget::kWeapon;
        Key c         = b;
        c.weaponHand  = OS::WeaponHand::Left;
        CHECK(!(b == c));
        c             = b;
        c.weaponClass = OS::WeaponClass::Bow;
        CHECK(!(b == c));
    }

    if (g_failures == 0) {
        std::printf("all DyeFlash tests passed\n");
    }
    return g_failures;
}
