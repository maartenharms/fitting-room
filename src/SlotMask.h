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

    // The three slots OBody uses to decide whether an actor is clothed for
    // ORefit: body, primary chest, and secondary chest.
    inline constexpr std::uint32_t kORefitTorsoMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(46) | MaskForEditorSlot(56);

    // Body armor meshes CONTAIN the body: hiding these means re-applying the
    // race skin's per-slot ARMA, not culling a node.
    inline constexpr std::uint32_t kBodySkinMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(33) | MaskForEditorSlot(37);

    // The player first-person biped renders torso/arms, hands/forearms and a
    // shield. Head and feet live only on the third-person biped. Shield hiding
    // is filtered by kNeverHideMask before these helpers are reached.
    inline constexpr std::uint32_t kFirstPersonArmorMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(33) |
        MaskForEditorSlot(34) | MaskForEditorSlot(39);

    // Their worn-mask bits drive hair/head-part culling in engine function 24220.
    inline constexpr std::uint32_t kHeadPartMask =
        MaskForEditorSlot(31) | MaskForEditorSlot(42);

    // Styling and hiding are separate policies. Shield styling is proven on
    // both live player biped passes, but shield hiding remains intentionally
    // unsupported. Keeping distinct masks prevents Hide All or a persisted
    // kHide entry from silently expanding the feature's scope.
    inline constexpr std::uint32_t kNeverStyleMask = 0;
    inline constexpr std::uint32_t kNeverHideMask  = MaskForEditorSlot(39);

    // Most armor styles may render without real gear in that slot. A shield is
    // different: it is a render-only replacement for an actually equipped
    // gameplay shield, never a way to conjure one.
    constexpr bool StyleRequiresWornItem(std::uint32_t a_bit) {
        return a_bit == kBitShield;
    }
    constexpr bool CanApplyStyleBit(std::uint32_t a_bit, std::uint32_t a_realWornMask) {
        return !StyleRequiresWornItem(a_bit) || ((a_realWornMask >> a_bit) & 1u) != 0;
    }

    // Restore only slots hidden by the pass or covered by a style that actually
    // passed its actor-specific gate. The raw requested style mask is unsafe:
    // shield and off-hand weapon share biped object 9, so a rejected shield
    // style must not make armor honesty replace an off-hand WEAP with skin ARMO.
    constexpr std::uint32_t PostPassArmorRestoreMask(
        std::uint32_t a_hideMask, std::uint32_t a_appliedStyleCoverage) {
        return a_hideMask | a_appliedStyleCoverage;
    }

    // A body-class hide must restage naked skin separately on the 1P biped.
    // In practice this is torso and hands; feet are body-class but have no 1P
    // geometry.
    constexpr std::uint32_t FirstPersonBodySkinHideMask(
        std::uint32_t a_hiddenBodySkinMask) {
        return a_hiddenBodySkinMask & kFirstPersonArmorMask;
    }

    // Keep objects[].item honest only for first-person-visible objects touched
    // by the pass. Style ARMA coverage can add forearms even though it is not
    // an independently selected outfit bit.
    constexpr std::uint32_t FirstPersonArmorRestoreMask(
        std::uint32_t a_hideMask, std::uint32_t a_appliedStyleCoverage) {
        return PostPassArmorRestoreMask(a_hideMask, a_appliedStyleCoverage) &
               kFirstPersonArmorMask;
    }

    // Precondition for both: a_bit < 32 (else the mask shift is UB).
    constexpr bool IsBodySkinBit(std::uint32_t a_bit) { return (kBodySkinMask >> a_bit) & 1u; }
    constexpr bool IsHeadPartBit(std::uint32_t a_bit) { return (kHeadPartMask >> a_bit) & 1u; }

}  // namespace OS
