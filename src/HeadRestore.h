#pragma once

#include "SlotMask.h"

#include <cstdint>
#include <span>

// The pure mask arithmetic behind "Hidden means render as if nothing was
// equipped there". Header-only and RE::-free so the composition the worn-mask
// shim runs on the hot path is unit-tested (see tests/test_headrestore.cpp),
// exactly like NpcResolve.h does for the NPC render decision.
namespace OS {

    // Head slots whose WORN bit suppresses head content in engine function
    // 24220: 30 Head, 31 Hair, 42 Circlet, 43 Ears.
    //
    // kHeadPartMask (SlotMask.h) is deliberately NOT the same set: it is the
    // narrower "our hide stripped these" mask and covers 31+42 only, which is
    // why hiding an OPEN helm restored the head perfectly while hiding a CLOSED
    // one left a headless character - bits 30 and 43 of the real helm went on
    // suppressing face and ears no matter what was hidden. From a BOD2 scan of
    // Skyrim.esm: Daedric, Ebony, Dwarven and Steel Plate helmets are slots
    // 30+31+42+43, while Iron, Steel, Glass and Dragonplate are 31+42 only.
    //
    // Slots 41 (LongHair) and 44 (Face/Mouth) are NOT included: nothing observed
    // shows 24220 culling off them, and every bit in this set is one the mask
    // shim will strip, so widening it on a guess would change what the engine
    // renders with nothing to verify against.
    inline constexpr std::uint32_t kHeadContentMask =
        MaskForEditorSlot(30) | MaskForEditorSlot(31) | MaskForEditorSlot(42) |
        MaskForEditorSlot(43);

    // Which head-content bits a REAL worn item stops contributing, given the
    // slots the outfit has taken over (hidden or styled over).
    //
    // The rule: "Hidden" means "render as if nothing was equipped there", not
    // "literally do not render it". So once ANY slot an item covers is hidden or
    // replaced by a style, that item's geometry is gone from the render and its
    // ENTIRE head suppression lifts. One click on Hair/Helmet un-heads a Daedric
    // helm; the user is not asked to hide four rows to undo one helmet.
    //
    // a_realItemCoverages holds one slot mask per real worn item that covers
    // any head-content slot (at most a handful - the engine equips one item per
    // slot). An item untouched by the outfit keeps suppressing exactly as
    // vanilla does, so a character wearing an unhidden helmet is unaffected.
    [[nodiscard]] inline constexpr std::uint32_t LiftedHeadBits(
        std::span<const std::uint32_t> a_realItemCoverages, std::uint32_t a_touchedMask) {
        std::uint32_t lifted = 0;
        for (const auto coverage : a_realItemCoverages) {
            if ((coverage & a_touchedMask) != 0) {
                lifted |= coverage & kHeadContentMask;
            }
        }
        return lifted;
    }

    // The worn mask to hand engine function 24220, which reads it ONLY to
    // set/clear kHidden on hair and head-part nodes. Composed so it describes
    // what is actually RENDERED on the head rather than what is equipped:
    //
    //   * drop the head bits of real items we removed or replaced
    //     (a_liftedHeadBits, above) - the face comes back;
    //   * keep the styled entry bits (a_styleMask), unchanged from 0.3.0 - a
    //     styled helmet must still hide hair;
    //   * add the styled items' OWN head coverage - a cosmetic closed helm or
    //     an Execution-Hood look covering slot 30 shadows the face just as the
    //     real thing would. Without this the lift above would REGRESS the case
    //     where a closed helm is styled over a closed helm: the real item's
    //     suppression would lift with nothing putting it back, and the face
    //     would show through a solid helmet.
    //   * finally strip a_hiddenHeadPartMask, 0.3.0's existing behavior, which
    //     still covers hiding a head slot that holds no real item at all.
    //
    // Hide and style are mutually exclusive per slot, so the final strip can
    // never cancel a styled bit.
    [[nodiscard]] inline constexpr std::uint32_t ComposeWornMask(
        std::uint32_t a_realWorn, std::uint32_t a_styleMask,
        std::uint32_t a_styledHeadCoverage, std::uint32_t a_hiddenHeadPartMask,
        std::uint32_t a_liftedHeadBits) {
        return (((a_realWorn & ~a_liftedHeadBits) | a_styleMask |
                 (a_styledHeadCoverage & kHeadContentMask)) &
                ~a_hiddenHeadPartMask);
    }

    // Does this item's slot mask touch head content at all? The shim uses it to
    // collect only the items worth passing to LiftedHeadBits.
    [[nodiscard]] inline constexpr bool CoversHeadContent(std::uint32_t a_slotMask) {
        return (a_slotMask & kHeadContentMask) != 0;
    }

}  // namespace OS
