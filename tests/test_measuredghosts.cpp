#include "MeasuredGhosts.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {
    using namespace OS::MeasuredGhosts;

    constexpr std::uint32_t kPlayer = 0x00000014;
    constexpr std::uint32_t kNpc    = 0x000E1BA9;

    // The two races the 2026-08-25 field round measured. A look apply rides
    // ChangeRace through the vanilla one and back, twice per apply.
    constexpr std::uint32_t kCustom = 0xEB05A198;  // UBE, helmet stages nothing
    constexpr std::uint32_t kNord   = 0x00013746;  // vanilla, helmet really draws

    constexpr std::uint32_t kHead    = 1u << 0;   // biped 30
    constexpr std::uint32_t kHair    = 1u << 1;   // biped 31
    constexpr std::uint32_t kCirclet = 1u << 12;  // biped 42
    constexpr std::uint32_t kEars    = 1u << 13;  // biped 43
    constexpr std::uint32_t kBody    = 1u << 2;   // biped 32, outside the family

    // The field's own occupant, and a different real helmet to swap in.
    constexpr std::uint32_t kHelmet = 0x481730E2;
    constexpr std::uint32_t kOther  = 0x00012E49;

    constexpr std::uint32_t kMiss = 0xFFFFFFFFu;

    // One sweep: a_worn is who sits where, a_drawn is which of them render.
    void Sweep(std::uint32_t a_actor, std::uint32_t a_race, std::uint32_t a_worn,
               std::uint32_t a_drawn, double a_now,
               std::uint32_t a_form = kHelmet) {
        std::uint32_t occ[kTrackedBits]{};
        for (std::uint32_t b = 0; b < kTrackedBits; ++b) {
            if ((a_worn >> b) & 1u) {
                occ[b] = a_form;
            }
        }
        Publish(a_actor, a_race, a_drawn, occ, a_now);
    }

    std::uint32_t ReadOr(std::uint32_t a_actor, std::uint32_t a_miss) {
        std::uint32_t out = 0;
        return Read(a_actor, out) ? out : a_miss;
    }

    std::uint32_t FamilyOr(std::uint32_t a_actor, std::uint32_t a_miss) {
        std::uint32_t out = 0;
        return ReadHeadFamily(a_actor, out) ? out : a_miss;
    }
}  // namespace

int main() {
    // ⚠ THE DWELL STILL GUARDS THE ATTACH, and r29 is why: a real Iron Helmet
    // on the vanilla race reads not-drawn at the ladder's +1 s and +2.5 s rungs
    // and only gains its clone by +5 s. Nothing below may let a reading that
    // young become a ghost.
    {
        Clear();
        Sweep(kPlayer, kNord, kHair, 0u, 100.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);  // measured, not yet a ghost
        Sweep(kPlayer, kNord, kHair, 0u, 101.0);
        Sweep(kPlayer, kNord, kHair, 0u, 102.5);
        Sweep(kPlayer, kNord, kHair, 0u, 105.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);        // still inside the dwell
        Sweep(kPlayer, kNord, kHair, kHair, 105.5);  // the clone landed
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
    }

    // An unbroken stretch past the dwell is a ghost.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 200.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        Sweep(kPlayer, kCustom, kHair, 0u, 200.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
    }

    // ⚠⚠ THE RACE IS PART OF THE KEY. The look apply switches the player to the
    // vanilla race and back. On that leg the Iron Helmet has a real addon, so
    // it genuinely DRAWS and the sweep honestly reports it drawn. Keyed on the
    // actor alone that reading zeroed the clock, so the custom race had to
    // re-serve the full dwell on return and the player was bald for all of it,
    // every apply. The log showed the published mask walking 0x2 -> 0x1 -> 0x3
    // -> 0x1 in step with the two ChangeRace lines.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 300.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 300.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);  // settled on the custom race

        // ChangeRace -> vanilla. The helmet draws here, and that is honest.
        Sweep(kPlayer, kNord, kHair, kHair, 307.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);  // no ghost on THIS race

        // ChangeRace -> back. The custom race's verdict was never in doubt, so
        // the very first sweep publishes it: no second bald window.
        Sweep(kPlayer, kCustom, kHair, 0u, 313.0);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
    }

    // ⚠⚠ AN UNWORN SLOT IS NOT A SLOT THAT DRAWS, and the first cut of the race
    // fix conflated them. Applying a look re-equips gear, so the helmet leaves
    // the biped for a sweep or two, and the field round read the cost plainly:
    //
    //   07:14:38.498  ghost slot(s) 00000003   <- settled, correct
    //   07:14:42.505  worn 0x8C drawn 0x8C     <- helmet not worn this sweep
    //   07:14:44.115  ghost slot(s) 00000001   <- verdict thrown away
    //
    // An unworn slot judges nothing and clears nothing.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, 400.0);
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, 400.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);

        // The re-equip: nothing on the head family at all for this sweep.
        Sweep(kPlayer, kCustom, 0u, 0u, 408.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);  // nothing worn, so nothing to drop

        // And it comes straight back, with no fresh dwell to serve.
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, 408.1);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
    }

    // ⚠ BUT THE VERDICT FOLLOWS THE PIECE. Freezing across an unequip must not
    // let a ghost's verdict land on a REAL helmet swapped into the same slot,
    // because that is hair through a helmet.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 500.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 500.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        Sweep(kPlayer, kCustom, 0u, 0u, 507.0);  // off
        // A DIFFERENT helmet goes on, and it has not drawn yet either.
        Sweep(kPlayer, kCustom, kHair, 0u, 507.1, kOther);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);  // starts over, no inherited verdict
        Sweep(kPlayer, kCustom, kHair, kHair, 509.0, kOther);  // it attaches
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
    }

    // ⚠ AND IT SELF-HEALS, which is what keeps the memo honest. If the piece
    // ever draws on the race it was condemned on, the verdict goes with it.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 600.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 600.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        Sweep(kPlayer, kCustom, kHair, kHair, 610.0);  // it drew after all
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        // and it has to earn the verdict again from scratch
        Sweep(kPlayer, kCustom, kHair, 0u, 611.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        Sweep(kPlayer, kCustom, kHair, 0u, 611.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
    }

    // The two head-part bits are independent, and the head bit is the one that
    // costs a whole head if it is wrong, so it gets its own clock.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHead | kHair, 0u, 700.0);
        Sweep(kPlayer, kCustom, kHead | kHair, kHead, 700.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);  // the head bit drew in between
        Sweep(kPlayer, kCustom, kHead | kHair, 0u, 701.0 + kGhostDwellSeconds);
        Sweep(kPlayer, kCustom, kHead | kHair, 0u, 701.0 + 2 * kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == (kHead | kHair));
    }

    // ⚠⚠ THE SHIM NEVER SEES A BIT 24220 DOES NOT READ. The field case is the
    // Iron Helmet on 31 AND 42: the hair hide is bit 31's alone, and dropping
    // 42 would be inert, so the two readers get two answers out of one walk.
    {
        Clear();
        const auto both = kHair | kCirclet;
        Sweep(kPlayer, kCustom, both, 0u, 800.0);
        Sweep(kPlayer, kCustom, both, 0u, 800.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);                 // engine: 31 only
        CHECK(FamilyOr(kPlayer, kMiss) == (kHair | kCirclet));  // page: both
    }

    // 41 and 43 are measured for the same reason and stay out of the shim too.
    {
        Clear();
        Sweep(kPlayer, kCustom, kEars, 0u, 900.0);
        Sweep(kPlayer, kCustom, kEars, 0u, 900.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        CHECK(FamilyOr(kPlayer, kMiss) == kEars);
    }

    // A ghost outside the head family is nobody's business here: the body-slot
    // restorer owns those, and neither reader may see one.
    {
        Clear();
        Sweep(kPlayer, kCustom, kBody | kHair, 0u, 1000.0);
        Sweep(kPlayer, kCustom, kBody | kHair, 0u, 1000.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        CHECK(FamilyOr(kPlayer, kMiss) == kHair);
    }

    // Two actors on two races do not read each other's verdicts.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 1100.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 1100.0 + kGhostDwellSeconds);
        Sweep(kNpc, kNord, kHead, 0u, 1100.0);
        Sweep(kNpc, kNord, kHead, 0u, 1100.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        CHECK(ReadOr(kNpc, kMiss) == kHead);
    }

    // A miss is silence, not a ghost: nothing is measured until the first
    // sweep, and the shim keeps its own strict synchronous test for that
    // window.
    {
        Clear();
        CHECK(ReadOr(kPlayer, kMiss) == kMiss);
        CHECK(FamilyOr(kPlayer, kMiss) == kMiss);
        CHECK(ReadOr(0u, kMiss) == kMiss);
        Sweep(0u, kCustom, kHair, 0u, 1200.0);  // no actor, no entry
        CHECK(ReadOr(0u, kMiss) == kMiss);
    }

    // A load boundary tears down every actor these ids name, verdicts included.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 1300.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 1300.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        Clear();
        CHECK(ReadOr(kPlayer, kMiss) == kMiss);
        // and the sticky verdict did not survive it either
        Sweep(kPlayer, kCustom, kHair, 0u, 1310.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
    }

    // ⚠ MORE RACES THAN THE TABLE HOLDS MUST DEGRADE TO THE OLD BEHAVIOUR, not
    // to a wrong answer. An evicted entry re-serves its dwell, which is the
    // fail-safe direction: a bald flash, never hair through a helmet.
    {
        Clear();
        Sweep(kPlayer, kCustom, kHair, 0u, 1400.0);
        Sweep(kPlayer, kCustom, kHair, 0u, 1400.0 + kGhostDwellSeconds);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        for (std::uint32_t r = 1; r <= 64; ++r) {
            Sweep(kPlayer, 0x00010000u + r, kHair, kHair, 1410.0 + r);
        }
        Sweep(kPlayer, kCustom, kHair, 0u, 1500.0);
        const auto after = ReadOr(kPlayer, kMiss);
        CHECK(after == kHair || after == 0u);  // remembered, or honestly re-timing
    }

    // ⚠⚠ THE WHOLE 2026-08-25 ROUND, END TO END, because every step of it was a
    // separate wrong answer first. Load, settle, apply a look (which switches
    // race out and back and re-equips the gear), and the hair must be back the
    // moment the race lands rather than six seconds later.
    {
        Clear();
        double t = 0.0;
        // Post-load: the helmet is on and stages nothing. The dwell is owed.
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, t += 1.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, t += 2.5);
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, t += 5.0);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);  // settled, hair back

        // The look apply: out to the vanilla race, where it really draws.
        Sweep(kPlayer, kNord, kHair | kCirclet, kHair, t += 1.0);
        CHECK(ReadOr(kPlayer, kMiss) == 0u);
        // the gear re-equips somewhere in there
        Sweep(kPlayer, kNord, 0u, 0u, t += 0.1);
        // and back to the custom race
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, t += 6.5);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);  // ⚠ NOT bald, and not in 6 s

        // The settle sweeps that used to undo it, at +4 s and +10 s.
        Sweep(kPlayer, kCustom, 0u, 0u, t += 4.0);       // mid re-equip
        Sweep(kPlayer, kCustom, kHair | kCirclet, 0u, t += 6.0);
        CHECK(ReadOr(kPlayer, kMiss) == kHair);
        CHECK(FamilyOr(kPlayer, kMiss) == (kHair | kCirclet));
    }

    if (g_failures == 0) {
        std::printf("MeasuredGhosts tests PASSED\n");
    } else {
        std::printf("MeasuredGhosts tests FAILED (%d)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
