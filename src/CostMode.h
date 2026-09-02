#pragma once

#include <cstdint>

// What committing an outfit costs, split out of Settings.h so a test can reach
// it.
//
// ⚠ SETTINGS.H INCLUDES PCH.H AND THEREFORE THE WHOLE ENGINE, which is why this
// is its own file rather than a section of that one. The rule this header
// exists to serve is EditorGate.h's: a decision that can be wrong belongs where
// a test executable can run it, and the playstyle preset's currency choice is
// one of those. Settings.h includes this, so every existing `OS::CostMode` site
// reads unchanged.
namespace OS {

    // ⚠ A MODE, AND bUseGold IS NOT REPURPOSED INTO IT. The cheap edit was to
    // let the existing bool mean "the Seamstone pays instead of gold", and it
    // would have moved every save that already reads bUseGold=1 onto an economy
    // it never opted into. That is not a re-balance, it is a lockout: a
    // Seamstone starts at zero charge, and an empty stone blocks styling
    // outright rather than discounting it. So gold keeps its own value, and an
    // absent iCostMode inherits bUseGold rather than the C++ default (see
    // Settings::Load).
    enum class CostMode : std::uint32_t {
        kFree   = 0,  // nothing is billed
        kGold   = 1,  // iGoldPerSlot per changed slot, the economy that shipped
        kCharge = 2,  // the Seamstone's charge, iChargePerSlot per changed slot
    };

    // ⚠ AN UNRECOGNISED VALUE FALLS BACK TO GOLD, NOT TO FREE. This reads a
    // hand-editable INI, and it will also read a mode number from a build newer
    // than this one. Of the two ways to be wrong, the one that GRANTS is the
    // expensive one: a typo that quietly made every edit free reads as the mod
    // having dropped its economy, and nothing in the game says otherwise. Gold
    // is the mode that shipped, so it is the safe answer to "I do not know what
    // this is".
    [[nodiscard]] constexpr CostMode CostModeFrom(long a_raw) {
        switch (a_raw) {
            case 0:
                return CostMode::kFree;
            case 2:
                return CostMode::kCharge;
            default:
                return CostMode::kGold;
        }
    }

}  // namespace OS
