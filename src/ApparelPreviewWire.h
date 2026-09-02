#pragma once

// ⚠ NO PCH.h, NO SKSE, NO RE. ApparelPreviewWireTests compiles this header in a
// pure-logic target with no engine dependencies (see CMakeLists), mirroring the
// split Apparel Preview already made in its own src/FittingRoomSignal.h: the
// wire format is the part worth testing, and the atomic that holds the decoded
// state plus the outbound Dispatch stay in ApparelPreviewSignal.h where engine
// types are allowed.
#include <cstddef>
#include <cstdint>

namespace OS {

    // ---- Apparel Preview -> Fitting Room, both messages --------------------
    //
    //   'APPV'  one byte, nonzero = a hover preview is on the player right now
    //                        zero = it is not
    //   'APSM'  four bytes, little-endian uint32 = the biped-object bits that
    //           preview occupies. Sent IMMEDIATELY BEFORE the matching 'APPV'.
    //
    // The mask is in BIPED OBJECT space, bit N = biped object N = editor slot
    // N + 30, which is what BGSBipedObjectForm::GetSlotMask already returns on
    // AP's side and what OutfitDye's per-bit walk already indexes on ours. The
    // two ends need no translation, and adding one would be the bug.
    //
    // ⚠ TWO MESSAGES RATHER THAN ONE LONGER PAYLOAD, AND THAT IS THE POINT OF
    // THE SHAPE. OS-145 needs the slot mask, and the obvious way to get it is
    // to grow 'APPV' from one byte to five. That breaks the pair in the
    // direction nobody tests: Fitting Room 0.4.0 and earlier read anything that
    // is not EXACTLY one byte as "no preview", so a NEWER Apparel Preview
    // talking to an OLDER Fitting Room would silently switch the worn-mask
    // stand-down back off and bring OS-141 back with it - the player renders
    // headless with a slot-30 style under a previewed helmet, which is a
    // shipped, field-confirmed fix. Nobody updates two mods in the same second.
    //
    // A companion message cannot do that. 'APPV' keeps its exact old shape, so
    // an older Fitting Room goes on working unchanged and never sees 'APSM' at
    // all: its listener tests the type and drops everything else. A newer
    // Fitting Room paired with an older Apparel Preview never receives 'APSM',
    // leaves the mask UNKNOWN, and falls back to today's behaviour rather than
    // to a guess. Both halves degrade to the previous release, in both
    // directions, with no version handshake to get wrong.
    inline constexpr std::uint32_t kPreviewStateMsg = 'APPV';
    inline constexpr std::uint32_t kPreviewSlotsMsg = 'APSM';

    inline constexpr std::size_t kPreviewStatePayloadSize = 1;
    inline constexpr std::size_t kPreviewSlotsPayloadSize = 4;

    struct ApparelPreviewState {
        bool          active{ false };      // a preview is on the player
        bool          slotsKnown{ false };  // ...and this sender told us which slots
        std::uint32_t slotMask{ 0 };        // biped object bits, meaningless unless known
    };

    // What the dye walk must NOT paint while a preview is live.
    //
    // ⚠ UNKNOWN IS NOT "EVERYTHING", and this line is what keeps the fix from
    // being worse than the bug. Standing the whole walk down for the duration
    // of a preview strips the dye off every other garment the character is
    // wearing, for as long as the mouse rests on an inventory row. So an active
    // preview whose slots we cannot name skips NOTHING and paints exactly as it
    // did before: that leaves OS-145 unfixed against an old Apparel Preview,
    // which is the state those two versions were already in, rather than
    // trading it for an undressed outfit.
    [[nodiscard]] inline constexpr std::uint32_t DyeSkipMask(const ApparelPreviewState& a_state) {
        return (a_state.active && a_state.slotsKnown) ? a_state.slotMask : std::uint32_t{ 0 };
    }

    // Anything other than exactly one byte reads as "no preview", unchanged
    // from the day this contract shipped: an older or malformed sender leaves
    // the worn-mask shim alone rather than disabling it.
    [[nodiscard]] inline bool DecodePreviewActive(const void* a_data, std::size_t a_length) {
        return a_data && a_length == kPreviewStatePayloadSize &&
               *static_cast<const std::uint8_t*>(a_data) != 0;
    }

    struct PreviewSlots {
        bool          known{ false };
        std::uint32_t mask{ 0 };
    };

    // ⚠ READ BYTE BY BYTE, never through a cast to uint32_t. The payload is
    // whatever the sender handed SKSE and carries no alignment promise, and the
    // byte order is stated on the wire rather than inherited from whichever
    // host happens to compile it.
    [[nodiscard]] inline PreviewSlots DecodePreviewSlots(const void* a_data, std::size_t a_length) {
        if (!a_data || a_length != kPreviewSlotsPayloadSize) {
            return {};
        }
        const auto* const b = static_cast<const std::uint8_t*>(a_data);
        return PreviewSlots{ true,
                             static_cast<std::uint32_t>(b[0]) |
                                 (static_cast<std::uint32_t>(b[1]) << 8) |
                                 (static_cast<std::uint32_t>(b[2]) << 16) |
                                 (static_cast<std::uint32_t>(b[3]) << 24) };
    }

    // ---- the packed word behind the atomic --------------------------------
    //
    // ⚠ ONE 64-BIT WORD, NOT A BOOL AND A UINT32 SIDE BY SIDE. Two atomics let
    // the dye walk read this publish's mask beside the previous publish's
    // active flag, and a pair that is individually honest and jointly wrong is
    // the exact failure this whole signal exists to avoid - see the derivation
    // in ApparelPreviewSignal.h, where the union of two correct worn masks
    // culled the player's head. Packing makes the pair unsplittable instead of
    // making the reader careful.
    inline constexpr std::uint64_t kPreviewActiveBit     = std::uint64_t{ 1 } << 32;
    inline constexpr std::uint64_t kPreviewSlotsKnownBit = std::uint64_t{ 1 } << 33;

    [[nodiscard]] inline constexpr std::uint64_t PackPreviewState(const ApparelPreviewState& a_state) {
        return static_cast<std::uint64_t>(a_state.slotMask) |
               (a_state.active ? kPreviewActiveBit : std::uint64_t{ 0 }) |
               (a_state.slotsKnown ? kPreviewSlotsKnownBit : std::uint64_t{ 0 });
    }

    [[nodiscard]] inline constexpr ApparelPreviewState UnpackPreviewState(std::uint64_t a_packed) {
        return ApparelPreviewState{
            (a_packed & kPreviewActiveBit) != 0,
            (a_packed & kPreviewSlotsKnownBit) != 0,
            static_cast<std::uint32_t>(a_packed & 0xFFFFFFFFull)
        };
    }

    // ---- what each message does to the word it finds -----------------------
    //
    // The two arrive separately and each owns its own half, so the whole state
    // machine is these two functions and it is worth being able to test the
    // SEQUENCE rather than the pieces. The atomic and its compare-exchange are
    // in ApparelPreviewSignal.h; nothing about the arithmetic lives there.
    //
    // ⚠ EACH ONE PRESERVES THE OTHER'S HALF. 'APSM' must not clear the active
    // flag and 'APPV' must not clear the mask, because AP sends the mask FIRST
    // and the flag SECOND: a setter that reset the other half would make the
    // second message undo the first, every publish, and the mask would be
    // permanently zero in exactly the state that needs it.
    [[nodiscard]] inline constexpr std::uint64_t WithPreviewActive(std::uint64_t a_packed,
                                                                   bool          a_active) {
        return (a_packed & ~kPreviewActiveBit) |
               (a_active ? kPreviewActiveBit : std::uint64_t{ 0 });
    }

    // Sets the known bit as well: receiving a well-formed 'APSM' IS the fact
    // that this sender names its slots.
    [[nodiscard]] inline constexpr std::uint64_t WithPreviewSlots(std::uint64_t a_packed,
                                                                  std::uint32_t a_mask) {
        return (a_packed & kPreviewActiveBit) | static_cast<std::uint64_t>(a_mask) |
               kPreviewSlotsKnownBit;
    }

}  // namespace OS
