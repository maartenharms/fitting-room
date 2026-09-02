#include "SeamstoneCharge.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// ⚠ THE PRICE LIST ONLY. Holding, spending and topping up the charge belong to
// DyeUnlockSet and are covered by test_dyeunlocks.cpp. If a Spend, CanAfford or
// Recharge ever shows up here again, it is the duplicate SeamstoneCharge.h
// warns about, not a gap.
int main() {
    using namespace OS::SeamstoneCharge;

    // ---- the bill mirrors the gold economy: per slot, per dye, per look ----
    const Price p{ 100, 250, 100 };
    CHECK(CostFor(p, 0, 0, 0) == 0);
    CHECK(CostFor(p, 1, 0, 0) == 100);
    CHECK(CostFor(p, 5, 0, 0) == 500);
    CHECK(CostFor(p, 0, 1, 0) == 250);
    CHECK(CostFor(p, 3, 2, 0) == 300 + 500);
    // ⚠ THE THIRD TERM, ADDED 2026-08-07. A body preset, hair visibility, hair
    // colour, hair style, eyes and brows all stage with zero changed slots, so
    // with two terms a character could be rebuilt head to toe for nothing.
    CHECK(CostFor(p, 0, 0, 1) == 100);
    CHECK(CostFor(p, 0, 0, 6) == 600);
    CHECK(CostFor(p, 3, 2, 4) == 300 + 500 + 400);

    // ⚠ SATURATES RATHER THAN WRAPPING. Every term comes from a hand-editable
    // INI multiplied by a count, and a wrapped product turns an unaffordable
    // bill into a free one, which is the failure that GRANTS rather than
    // refuses.
    const Price huge{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    CHECK(CostFor(huge, 32, 8, 6) == 0xFFFFFFFFu);
    // A saturated bill stays out of reach of a purse one short of the maximum,
    // which is what SpendCharge is then asked, and what a wrapped product would
    // have quietly turned into a discount.
    CHECK(0xFFFFFFFEu < CostFor(huge, 32, 8, 6));
    // The look term alone can saturate, so it is not merely riding on the other
    // two being large.
    CHECK(CostFor(Price{ 0, 0, 0xFFFFFFFFu }, 0, 0, 6) == 0xFFFFFFFFu);

    // ---- soul gems ----
    CHECK(ValueOf(SoulSize::kNone) == 0);
    CHECK(ValueOf(SoulSize::kPetty) < ValueOf(SoulSize::kLesser));
    CHECK(ValueOf(SoulSize::kLesser) < ValueOf(SoulSize::kCommon));
    CHECK(ValueOf(SoulSize::kCommon) < ValueOf(SoulSize::kGreater));
    CHECK(ValueOf(SoulSize::kGreater) < ValueOf(SoulSize::kGrand));
    // A petty soul is worth a couple of slots and a grand is worth a wardrobe.
    // That relationship is the point of the values, so it is asserted rather
    // than left to whoever next edits the table. It is also the only thing
    // holding the value table and the price table together: they are two
    // unrelated lists of numbers in the same header otherwise.
    CHECK(ValueOf(SoulSize::kPetty) >= CostFor(p, 2, 0, 0));
    CHECK(ValueOf(SoulSize::kGrand) >= CostFor(p, 20, 0, 0));

    // ---- the meter ----
    CHECK(Fraction(0, 100) == 0.0f);
    CHECK(Fraction(50, 100) == 0.5f);
    CHECK(Fraction(100, 100) == 1.0f);
    // Clamped, never past full: AddCharge leaves a stone above a lowered cap
    // alone rather than cutting it down, so held > capacity is an ordinary
    // state and the meter must draw it as full.
    CHECK(Fraction(150, 100) == 1.0f);
    CHECK(Fraction(10, 0) == 0.0f);  // a hand-edited zero capacity cannot divide

    if (g_failures == 0) {
        std::printf("SeamstoneChargeTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
