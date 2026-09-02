#include "RequipDiff.h"

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
    using OS::SlotEntry;
    using OS::StyleRefKey;
    using OS::RequipDiff::ChangedMask;
    using OS::RequipDiff::SlotEntries;

    const StyleRefKey kSteel{ "Skyrim.esm", 0x0001397D };
    const StyleRefKey kElven{ "Skyrim.esm", 0x000896A3 };

    // ---- nothing moved --------------------------------------------------
    {
        SlotEntries before{};
        SlotEntries after{};
        CHECK(ChangedMask(before, after) == 0u);

        before[3] = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        after[3]  = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        CHECK(ChangedMask(before, after) == 0u);
    }

    // ---- one slot moved --------------------------------------------------
    {
        SlotEntries before{};
        SlotEntries after{};
        before[3] = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        after[3]  = SlotEntry{ SlotEntry::Kind::kStyle, kElven };
        CHECK(ChangedMask(before, after) == (1u << 3));
    }

    // ---- the kind changing is a change even when the style does not ------
    {
        SlotEntries before{};
        SlotEntries after{};
        before[5] = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        after[5]  = SlotEntry{ SlotEntry::Kind::kHide, kSteel };
        CHECK(ChangedMask(before, after) == (1u << 5));

        // Both directions, because taking a hide off is as visible as putting
        // one on.
        CHECK(ChangedMask(after, before) == (1u << 5));
    }

    // ---- ⚠ STALE STYLE DATA IN AN UNUSED SLOT MUST NOT FORGE A CHANGE ----
    // A passthrough slot carries whatever StyleRefKey was last written there.
    // Comparing the struct wholesale would arm a flourish on a slot that looks
    // identical on screen, which is a purple flash on a garment nobody touched.
    {
        SlotEntries before{};
        SlotEntries after{};
        before[7] = SlotEntry{ SlotEntry::Kind::kPassthrough, kSteel };
        after[7]  = SlotEntry{ SlotEntry::Kind::kPassthrough, kElven };
        CHECK(ChangedMask(before, after) == 0u);

        // Same rule for hide: the slot shows nothing either way.
        before[8] = SlotEntry{ SlotEntry::Kind::kHide, kSteel };
        after[8]  = SlotEntry{ SlotEntry::Kind::kHide, kElven };
        CHECK(ChangedMask(before, after) == 0u);
    }

    // ---- several at once, which is the ordinary case ---------------------
    {
        SlotEntries before{};
        SlotEntries after{};
        before[0]  = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        before[31] = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        after[0]   = SlotEntry{ SlotEntry::Kind::kStyle, kElven };
        after[31]  = SlotEntry{ SlotEntry::Kind::kStyle, kSteel };
        after[16]  = SlotEntry{ SlotEntry::Kind::kStyle, kElven };
        CHECK(ChangedMask(before, after) == ((1u << 0) | (1u << 16)));
    }

    // ---- which shapes the flourish may light (field 2026-08-15) ----------
    //
    // ⚠⚠ THE BUG THIS EXISTS FOR. The first build lit every geometry hanging
    // off a changed biped slot. Skin is worn as a TESObjectARMO in those same
    // slots, so switching to Naked left nothing there BUT skin, and the
    // character's hands went flat violet. It read as a missing texture, which
    // is exactly what the design said would happen and exactly what it said to
    // prevent.
    {
        using OS::DyeSkipReason;
        using OS::RequipDiff::RequipMayPaint;

        // The three that are the character rather than the outfit.
        CHECK(!RequipMayPaint(DyeSkipReason::kCharacterColour));  // skin, the field bug
        CHECK(!RequipMayPaint(DyeSkipReason::kHair));
        CHECK(!RequipMayPaint(DyeSkipReason::kEyes));

        // An ordinary garment shape, which is the whole point of the feature.
        CHECK(RequipMayPaint(DyeSkipReason::kNone));

        // ⚠ NOT DYEABLE IS NOT THE SAME QUESTION AS NOT LIGHTABLE. A glowing or
        // parallax garment is refused a dye stripe because dyeing it looks
        // wrong, but it is still the outfit that is being swapped and it should
        // still burn away with the rest of the set. Reusing DyeSkipReason for
        // the classifier must not quietly import its whole verdict.
        CHECK(RequipMayPaint(DyeSkipReason::kGlow));
        CHECK(RequipMayPaint(DyeSkipReason::kUnsupported));
        CHECK(RequipMayPaint(DyeSkipReason::kReflectiveOff));

        // Weapon-side reasons: reachable only through the weapon walk, which
        // the armour mask never visits, so they are lightable by default rather
        // than by intent. Stated so a future weapon flourish has to decide.
        CHECK(RequipMayPaint(DyeSkipReason::kOffHandWeapon));
        CHECK(RequipMayPaint(DyeSkipReason::kTorch));
        CHECK(!RequipMayPaint(DyeSkipReason::kDecal));  // invisible until it bleeds
    }

    if (g_failures == 0) {
        std::printf("test_requipdiff: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
