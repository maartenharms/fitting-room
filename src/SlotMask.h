#pragma once

#include <cstdint>

namespace OS {

    // CommonLib's BipedObjectSlot is a bitmask: kHead(1<<0) is editor slot 30.
    // bit = editorSlot - 30.  Cloak (editor 46) is bit 16 (kModChestPrimary).
    // Precondition: a_editorSlot >= 30 (else unsigned underflow -> shift UB).
    constexpr std::uint32_t BitForEditorSlot(std::uint32_t a_editorSlot) {
        return a_editorSlot - 30u;
    }
    // Precondition: a_editorSlot in [30, 61] (else the 1u shift is UB).
    constexpr std::uint32_t MaskForEditorSlot(std::uint32_t a_editorSlot) {
        return 1u << BitForEditorSlot(a_editorSlot);
    }

    inline constexpr std::uint32_t kBitHead    = BitForEditorSlot(30);
    inline constexpr std::uint32_t kBitHair    = BitForEditorSlot(31);  // helmet
    inline constexpr std::uint32_t kBitBody    = BitForEditorSlot(32);
    inline constexpr std::uint32_t kBitHands   = BitForEditorSlot(33);
    inline constexpr std::uint32_t kBitAmulet  = BitForEditorSlot(35);
    inline constexpr std::uint32_t kBitRing    = BitForEditorSlot(36);
    inline constexpr std::uint32_t kBitFeet    = BitForEditorSlot(37);
    inline constexpr std::uint32_t kBitShield  = BitForEditorSlot(39);
    inline constexpr std::uint32_t kBitCirclet = BitForEditorSlot(42);
    inline constexpr std::uint32_t kBitCloak   = BitForEditorSlot(46);

    // Body armor meshes CONTAIN the body: hiding these means re-applying the
    // race skin's per-slot ARMA, not culling a node.
    inline constexpr std::uint32_t kBodySkinMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(33) | MaskForEditorSlot(37);

    // Their worn-mask bits drive hair/head-part culling in engine function 24220.
    inline constexpr std::uint32_t kHeadPartMask =
        MaskForEditorSlot(31) | MaskForEditorSlot(42);

    // Never styled or hidden by this mod.
    inline constexpr std::uint32_t kNeverTouchMask = MaskForEditorSlot(39);  // shield

    // Precondition for both: a_bit < 32 (else the mask shift is UB).
    constexpr bool IsBodySkinBit(std::uint32_t a_bit) { return (kBodySkinMask >> a_bit) & 1u; }
    constexpr bool IsHeadPartBit(std::uint32_t a_bit) { return (kHeadPartMask >> a_bit) & 1u; }

}  // namespace OS
