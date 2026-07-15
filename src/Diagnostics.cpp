#include "Diagnostics.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OS::Diagnostics {

    void WarnOnConflicts() {
        // Plugins that override the player's worn armor rendering the same way
        // we do. (Real-equip outfit managers - e.g. Outfit Preview Selector -
        // are NOT conflicts: they drive the vanilla equip pipeline.)
        //
        // DynamicArmorVariants.dll was removed from this list 2026-07-11 after
        // verification against its shipped PDB: DAV hooks 24736+0x2F0
        // (InitWornArmor) and 16044+0x28 (a GetWornMask caller) - neither is
        // our 24231+0x81 / 24220+0x7C - and it coexisted through every in-game
        // session of the 2.5 gate and editor work on the Nolvus load order.
        // The SOS-lineage incompatibility with DAV does not apply to this
        // design. Variant-swap + our style injection compose through the
        // rebuild pipeline (last-wins; our restore is field-disciplined).
        static constexpr std::string_view kConflicting[]{
            "SkyrimOutfitSystemSE.dll",
            "SkyrimOutfitEquipmentSystemNG.dll",
            "SkyrimVanitySystem.dll",
        };

        std::error_code          ec;
        std::vector<std::string> found;
        for (const auto& entry : std::filesystem::directory_iterator("Data/SKSE/Plugins", ec)) {
            if (ec) {
                return;
            }
            const auto name = entry.path().filename().string();
            for (auto candidate : kConflicting) {
                if (_stricmp(name.c_str(), candidate.data()) == 0) {
                    found.push_back(name);
                }
            }
        }
        if (found.empty()) {
            return;
        }
        std::string list;
        for (const auto& f : found) {
            if (!list.empty()) {
                list += ", ";
            }
            list += f;
        }
        spdlog::warn("CONFLICT: these plugins override worn-armor rendering too: {}. "
                     "Expect visual glitches. Use only one.",
                     list);
        const std::string msg = "Fitting Room detected a conflicting mod:\n\n" + list +
                                "\n\nBoth change how worn armor is rendered. Please use only one.";
        RE::DebugMessageBox(msg.c_str());
    }

}  // namespace OS::Diagnostics
