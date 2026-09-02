#include "DyePreview.h"

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
    using namespace OS;
    using namespace OS::DyePreview;

    {  // The debounce, as the sequence a player produces.
        // No candidate: clear, whatever else is true.
        CHECK(Decide(false, false, false, 1.0, 0.0, kArmDelaySeconds) == Hover::kClear);
        CHECK(Decide(false, true, true, 1.0, 0.0, kArmDelaySeconds) == Hover::kClear);
        // The armed swatch under the cursor: keep, never re-arm.
        CHECK(Decide(true, true, false, 1.0, 0.0, kArmDelaySeconds) == Hover::kKeep);
        // A new swatch: start its dwell.
        CHECK(Decide(true, false, false, 1.0, 0.0, kArmDelaySeconds) == Hover::kRepend);
        // Still dwelling: keep waiting.
        CHECK(Decide(true, false, true, 1.0, 1.0 - kArmDelaySeconds / 2.0,
                     kArmDelaySeconds) == Hover::kKeep);
        // Dwelt long enough: arm. Clearly past the delay rather than exactly
        // on it: 1.0 - (1.0 - 0.18) rounds a hair under 0.18 in doubles, and
        // the boundary frame is not a behaviour anybody can see.
        CHECK(Decide(true, false, true, 1.0, 1.0 - kArmDelaySeconds * 2.0,
                     kArmDelaySeconds) == Hover::kArm);
        // A sweep across the grid re-pends every frame and never arms: each
        // new swatch resets the dwell, which is the whole point of having one.
        CHECK(Decide(true, false, false, 1.0, 0.999, kArmDelaySeconds) == Hover::kRepend);
    }

    {  // Hits: armour matches on the via bit.
        State s;
        s.armed       = true;
        s.key.target  = DyeTarget::kArmour;
        s.key.slotBit = 3;
        s.key.channel = 2;
        CHECK(Hits(s, DyeTarget::kArmour, 3, WeaponClass::Sword, WeaponHand::Both));
        CHECK(!Hits(s, DyeTarget::kArmour, 4, WeaponClass::Sword, WeaponHand::Both));
        CHECK(!Hits(s, DyeTarget::kWeapon, 3, WeaponClass::Sword, WeaponHand::Both));
        // Disarmed never hits.
        s.armed = false;
        CHECK(!Hits(s, DyeTarget::kArmour, 3, WeaponClass::Sword, WeaponHand::Both));
    }

    {  // Hits: weapons match on class AND hand.
        State s;
        s.armed           = true;
        s.key.target      = DyeTarget::kWeapon;
        s.key.weaponClass = WeaponClass::Sword;
        s.key.weaponHand  = WeaponHand::Both;
        s.key.channel     = 0;
        CHECK(Hits(s, DyeTarget::kWeapon, 0, WeaponClass::Sword, WeaponHand::Both));
        CHECK(!Hits(s, DyeTarget::kWeapon, 0, WeaponClass::Bow, WeaponHand::Both));
    }

    {  // Hair never previews, and an out-of-range channel never substitutes.
        State s;
        s.armed      = true;
        s.key.target = DyeTarget::kHair;
        CHECK(!Hits(s, DyeTarget::kHair, 0, WeaponClass::Sword, WeaponHand::Both));
        s.key.target  = DyeTarget::kArmour;
        s.key.slotBit = 1;
        s.key.channel = kDyeChannelCount;  // one past the end
        CHECK(!Hits(s, DyeTarget::kArmour, 1, WeaponClass::Sword, WeaponHand::Both));
    }

    {  // WithPreview substitutes exactly one channel on a hit and nothing on
       // a miss, and a preview on an UNDYED slot makes it paintable: the
       // caller computes Any() off the result, which is the line that makes
       // an undyed piece previewable at all.
        State s;
        s.armed       = true;
        s.key.target  = DyeTarget::kArmour;
        s.key.slotBit = 5;
        s.key.channel = 1;
        s.channel     = DyeChannel{ true, 0xAA, 0xBB, 0xCC };

        SlotDye undyed{};
        CHECK(!undyed.Any());
        const auto hit = WithPreview(undyed, s, DyeTarget::kArmour, 5, WeaponClass::Sword,
                                     WeaponHand::Both);
        CHECK(hit.Any());
        CHECK(hit.channels[1].set && hit.channels[1].r == 0xAA);
        CHECK(!hit.channels[0].set);  // the other channels untouched

        const auto miss = WithPreview(undyed, s, DyeTarget::kArmour, 6, WeaponClass::Sword,
                                      WeaponHand::Both);
        CHECK(!miss.Any());

        // A dyed slot keeps its other channels and loses only the ringed one.
        SlotDye dyed{};
        dyed.channels[0] = DyeChannel{ true, 1, 2, 3 };
        dyed.channels[1] = DyeChannel{ true, 4, 5, 6 };
        const auto over = WithPreview(dyed, s, DyeTarget::kArmour, 5, WeaponClass::Sword,
                                      WeaponHand::Both);
        CHECK(over.channels[0].r == 1 && over.channels[1].r == 0xAA);
    }

    if (g_failures == 0) {
        std::printf("DyePreviewTests: all passed\n");
    }
    return g_failures;
}
