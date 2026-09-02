#include "ApparelPreviewWire.h"

#include <cstdio>
#include <initializer_list>  // the range-for over a braced list, below

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using OS::ApparelPreviewState;
    using OS::DecodePreviewActive;
    using OS::DecodePreviewSlots;
    using OS::DyeSkipMask;
    using OS::kPreviewSlotsMsg;
    using OS::kPreviewSlotsPayloadSize;
    using OS::kPreviewStateMsg;
    using OS::PackPreviewState;
    using OS::UnpackPreviewState;
    using OS::WithPreviewActive;
    using OS::WithPreviewSlots;

    // ---- the message ids ---------------------------------------------------
    // ⚠ A MULTI-CHAR LITERAL IS AN INTEGER AND A TYPO IN ONE IS SILENT. It
    // compiles, it has a value, and the only symptom is a message nobody
    // receives - which reads identically to "Apparel Preview is not installed",
    // the case this contract is already designed to tolerate. Spelling the two
    // numbers out is the only thing that makes a wrong character fail loudly.
    // (These are also the bytes to search a built DLL for; searching for the
    // ASCII spelling of a multi-char literal returns a confident false
    // negative, which cost one wrong conclusion on 2026-08-06.)
    CHECK(kPreviewStateMsg == 0x41505056u);  // 'APPV'
    CHECK(kPreviewSlotsMsg == 0x4150534Du);  // 'APSM'
    CHECK(kPreviewStateMsg != kPreviewSlotsMsg);

    // ---- 'APPV' decodes exactly as it always did ---------------------------
    // This block is a compatibility pin, not a new behaviour. Fitting Room
    // 0.4.0 and earlier apply these same rules, so anything that changes here
    // changes what an OLDER Fitting Room does with a NEWER Apparel Preview.
    {
        const std::uint8_t on  = 1;
        const std::uint8_t off = 0;
        CHECK(DecodePreviewActive(&on, 1));
        CHECK(!DecodePreviewActive(&off, 1));
        CHECK(!DecodePreviewActive(nullptr, 1));
        CHECK(!DecodePreviewActive(&on, 0));

        // ⚠ THE CASE THE WHOLE TWO-MESSAGE DESIGN EXISTS FOR. A five-byte
        // 'APPV' - the payload we would have sent had the mask gone INSIDE this
        // message - reads as "no preview". An older Fitting Room does exactly
        // this, which would have switched its worn-mask stand-down off and
        // brought OS-141 back the moment someone updated Apparel Preview first.
        const std::uint8_t wide[5] = { 1, 0x04, 0, 0, 0 };
        CHECK(!DecodePreviewActive(wide, sizeof(wide)));
    }

    // ---- 'APSM' decodes little-endian, or not at all -----------------------
    {
        // 0x80000004: bit 2 (the body) and bit 31 (the highest armour bit), so
        // a byte-order mistake cannot pass and neither can a sign extension.
        const std::uint8_t four[4] = { 0x04, 0x00, 0x00, 0x80 };
        const auto         slots   = DecodePreviewSlots(four, sizeof(four));
        CHECK(slots.known);
        CHECK(slots.mask == 0x80000004u);

        CHECK(!DecodePreviewSlots(nullptr, kPreviewSlotsPayloadSize).known);
        CHECK(!DecodePreviewSlots(four, 3).known);
        CHECK(!DecodePreviewSlots(four, 5).known);
        CHECK(DecodePreviewSlots(four, 3).mask == 0u);
    }

    // ---- the predicate the dye walk actually asks --------------------------
    {
        // Nothing live: paint everything.
        CHECK(DyeSkipMask(ApparelPreviewState{ false, false, 0 }) == 0u);
        // Not live, but a mask left over from the last preview. Still paint
        // everything: the geometry went back when the preview ended.
        CHECK(DyeSkipMask(ApparelPreviewState{ false, true, 0x04 }) == 0u);

        // ⚠ THE LINE THAT KEEPS THE FIX FROM BEING WORSE THAN THE BUG. A live
        // preview whose slots we cannot name skips NOTHING. Returning "all
        // bits" here would strip the dye off the whole outfit for as long as
        // the mouse rests on an inventory row.
        CHECK(DyeSkipMask(ApparelPreviewState{ true, false, 0 }) == 0u);
        // ...and it stays zero even if a stale mask is sitting in the field.
        CHECK(DyeSkipMask(ApparelPreviewState{ true, false, 0xFFFFFFFF }) == 0u);

        // Live and named: skip exactly those bits.
        CHECK(DyeSkipMask(ApparelPreviewState{ true, true, 0x04 }) == 0x04u);
        CHECK(DyeSkipMask(ApparelPreviewState{ true, true, 0x0200 }) == 0x0200u);  // biped 9
        // Live, named, and covering nothing is not a contradiction worth
        // guessing about: paint everything.
        CHECK(DyeSkipMask(ApparelPreviewState{ true, true, 0 }) == 0u);
    }

    // ---- packing keeps the flags off the mask ------------------------------
    {
        // A full 32-bit mask must not reach either flag bit.
        const auto full = PackPreviewState(ApparelPreviewState{ false, false, 0xFFFFFFFF });
        CHECK(UnpackPreviewState(full).slotMask == 0xFFFFFFFFu);
        CHECK(!UnpackPreviewState(full).active);
        CHECK(!UnpackPreviewState(full).slotsKnown);

        const auto both = PackPreviewState(ApparelPreviewState{ true, true, 0xFFFFFFFF });
        CHECK(UnpackPreviewState(both).slotMask == 0xFFFFFFFFu);
        CHECK(UnpackPreviewState(both).active);
        CHECK(UnpackPreviewState(both).slotsKnown);

        // Round trip of each corner.
        for (const auto& s : { ApparelPreviewState{ false, false, 0 },
                               ApparelPreviewState{ true, false, 0 },
                               ApparelPreviewState{ false, true, 0x1234 },
                               ApparelPreviewState{ true, true, 0x80000001 } }) {
            const auto r = UnpackPreviewState(PackPreviewState(s));
            CHECK(r.active == s.active);
            CHECK(r.slotsKnown == s.slotsKnown);
            CHECK(r.slotMask == s.slotMask);
        }
    }

    // ---- the sequence, which is where a half-update would show -------------
    {
        // Apparel Preview publishes mask FIRST, then the active flag, before
        // every refresh. Walk that order and check the walk never sees a pair
        // that would paint the previewed slot.
        std::uint64_t w = 0;

        w = WithPreviewSlots(w, 0x04);  // 'APSM': previewing on the body
        CHECK(UnpackPreviewState(w).slotsKnown);
        CHECK(UnpackPreviewState(w).slotMask == 0x04u);
        // Not active yet, so nothing is skipped - and nothing is on the biped
        // yet either, because the refresh has not run.
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0u);

        w = WithPreviewActive(w, true);  // 'APPV': and now it is live
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0x04u);

        // ⚠ THE FAILURE THIS BLOCK IS FOR: if 'APPV' reset the mask, the skip
        // would be zero here and OS-145 would look unfixed while every test on
        // the individual pieces still passed.
        CHECK(UnpackPreviewState(w).slotMask == 0x04u);

        // Hover moves to a helmet: mask first again, flag unchanged.
        w = WithPreviewSlots(w, 0x01);
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0x01u);
        CHECK(UnpackPreviewState(w).active);

        // Hover leaves the list entirely.
        w = WithPreviewSlots(w, 0);
        w = WithPreviewActive(w, false);
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0u);
        // The known bit survives, because it is a fact about the SENDER rather
        // than about this preview.
        CHECK(UnpackPreviewState(w).slotsKnown);
    }

    // ---- an Apparel Preview too old to send 'APSM' -------------------------
    {
        // Only 'APPV' ever arrives. The mask stays unknown for the whole
        // session, so the dye walk behaves exactly as it did before OS-145
        // rather than guessing.
        std::uint64_t w = 0;
        w               = WithPreviewActive(w, true);
        CHECK(UnpackPreviewState(w).active);
        CHECK(!UnpackPreviewState(w).slotsKnown);
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0u);
        w = WithPreviewActive(w, false);
        CHECK(DyeSkipMask(UnpackPreviewState(w)) == 0u);
    }

    // ---- the editor-open self-heal ----------------------------------------
    {
        // NotifyApparelPreview latches the whole word to zero. Check that zero
        // is the fail-safe reading rather than a state that skips something:
        // an Apparel Preview that has crashed must not leave a slot exempt from
        // its own dye until the game restarts.
        const auto healed = UnpackPreviewState(0);
        CHECK(!healed.active);
        CHECK(!healed.slotsKnown);
        CHECK(healed.slotMask == 0u);
        CHECK(DyeSkipMask(healed) == 0u);
    }

    if (g_failures == 0) {
        std::printf("all ApparelPreviewWire tests passed\n");
    }
    return g_failures;
}
