#pragma once

#include "Outfit.h"
#include "SlotMask.h"

#include <cstdint>

namespace OS::PresetPreviewPolicy {

    // Exported presets are intentionally retained when their source plugins
    // disappear, so row visibility and row health are separate questions.
    // Lore ownership remains a distinct save gate; it shares the established
    // red warning treatment without being misreported as a missing dependency.
    [[nodiscard]] inline constexpr bool HighlightPresetRow(
        bool a_exported, std::size_t a_missingDependencies,
        std::size_t a_unfitPieces, bool a_loreOwnershipIncomplete) {
        return a_loreOwnershipIncomplete ||
               (a_exported &&
                (a_missingDependencies != 0 || a_unfitPieces != 0));
    }

    // Biped object indices, not editor-mask bits. Shield is armor object 9;
    // weapons, staves and quivers occupy objects 32 through 41. This policy is
    // transient while browsing Presets and is never serialized into an Outfit.
    inline constexpr std::uint64_t kSuppressedBipedObjects =
        (1ull << kBitShield) | (0x3FFull << 32u);

    // A follower mannequin hides unrelated real player weapons but leaves a
    // biped object visible when the follower outfit supplies that class's look.
    [[nodiscard]] inline std::uint64_t MannequinSuppressedBipedObjects(
        const Outfit& a_outfit) {
        std::uint64_t mask = kSuppressedBipedObjects;
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto cls = static_cast<WeaponClass>(i);
            const bool both = a_outfit.WeaponEntryFor(cls).kind ==
                              SlotEntry::Kind::kStyle;
            const bool right =
                a_outfit.ResolvedWeaponEntryFor(cls, WeaponHand::Right).kind ==
                SlotEntry::Kind::kStyle;
            const bool left =
                a_outfit.ResolvedWeaponEntryFor(cls, WeaponHand::Left).kind ==
                SlotEntry::Kind::kStyle;
            if (both || right || left) {
                mask &= ~(1ull << BipedSlotForClass(cls));
            }
            if (SupportsHandOverrides(cls) && left) {
                // Humanoid player off-hand/shield biped object. This is a
                // preview-only mask; actual hand routing remains actor-race
                // driven in the weapon loader.
                mask &= ~(1ull << kBitShield);
            }
        }
        return mask;
    }

}  // namespace OS::PresetPreviewPolicy
