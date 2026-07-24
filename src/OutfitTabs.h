#pragma once

#include <cstddef>
#include <optional>

namespace OS::OutfitTabs {

    inline constexpr int kNoForcedSelection = -1;
    inline constexpr int kForceEquippedGear = -2;

    // SetSelected is a one-frame request, not a description of which library
    // entry is active. Reapplying it for the inactive state makes every saved
    // tab click bounce straight back to Equipped gear.
    [[nodiscard]] inline constexpr bool ShouldForceEquipped(
        bool, int a_forcedSelection) {
        return a_forcedSelection == kForceEquippedGear;
    }

    // Logical tab 0 is immutable equipped gear. Saved outfits start at 1.
    [[nodiscard]] inline constexpr int LogicalFromActive(int a_active) {
        return a_active < 0 ? 0 : a_active + 1;
    }

    [[nodiscard]] inline constexpr int ActiveFromLogical(int a_logical) {
        return a_logical <= 0 ? -1 : a_logical - 1;
    }

    // SetSelected accepts either a saved-outfit index or the dedicated
    // Equipped-gear sentinel. Centralising this keeps open, target-switch and
    // final-delete transitions from accidentally passing plain -1 ("no force").
    [[nodiscard]] inline constexpr int ForcedSelectionForActive(int a_active) {
        return a_active >= 0 ? a_active : kForceEquippedGear;
    }

    // FLICK can report the previously selected tab active early in the frame
    // before it reaches a later tab carrying SetSelected. While a one-shot
    // programmatic selection is pending, only that target may mutate the model.
    [[nodiscard]] inline constexpr bool ShouldAcceptActivation(
        int a_reportedActive, int a_forcedSelection) {
        if (a_forcedSelection == kNoForcedSelection) {
            return true;
        }
        if (a_forcedSelection == kForceEquippedGear) {
            return a_reportedActive < 0;
        }
        return a_reportedActive == a_forcedSelection;
    }

    // Equipped gear is outside the saved library. Every real saved outfit,
    // including the only remaining one, may be deleted.
    [[nodiscard]] inline constexpr bool CanDeleteSaved(int a_active,
                                                       std::size_t a_outfitCount) {
        return a_active >= 0 &&
               static_cast<std::size_t>(a_active) < a_outfitCount;
    }

    [[nodiscard]] inline int Cycle(int a_active, int a_outfitCount, bool a_next) {
        const int total = a_outfitCount + 1;
        if (total <= 1) {
            return -1;
        }
        const int current = LogicalFromActive(a_active);
        const int logical = a_next ? (current + 1) % total
                                   : (current - 1 + total) % total;
        return ActiveFromLogical(logical);
    }

    // Vertical wheel input is the practical horizontal navigation affordance
    // most mice provide. Claim it whenever the pointer is over a tab strip
    // containing saved outfits, even if every tab currently fits. The returned
    // active index feeds the existing one-shot SetSelected path, which also
    // asks ImGui to reveal the newly selected tab when scrolling is necessary.
    [[nodiscard]] inline std::optional<int> WheelCycleRequest(
        int a_active, int a_outfitCount, float a_wheel,
        bool a_stripHovered) {
        if (!a_stripHovered || a_outfitCount <= 0 || a_wheel == 0.0f) {
            return std::nullopt;
        }
        return Cycle(a_active, a_outfitCount, /*next/right*/ a_wheel < 0.0f);
    }

}  // namespace OS::OutfitTabs
