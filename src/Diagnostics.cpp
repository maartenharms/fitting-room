#include "Diagnostics.h"

#include <cstring>
#include <wchar.h>  // _wcsicmp: the name compare that cannot throw
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
        // ⚠⚠ THE NAME IS COMPARED WIDE AND REPORTED FROM THE TABLE, NEVER
        // CONVERTED FROM THE DIRECTORY. `path::string()` narrows through the
        // ACTIVE ANSI CODE PAGE and THROWS std::system_error on any filename
        // that page cannot represent. This function runs inside the SKSE
        // message handler at kDataLoaded with nothing above it catching, so
        // that throw is a crash to desktop before the main menu, caused by a
        // file we do not care about and never open.
        //
        // FIELD 2026-08-24: installing `GT - Softbody v3.37` did exactly that.
        // It ships `新建文本文档.txt` in its SKSE/Plugins folder, cp1252 has no
        // mapping for those characters, and every launch died here. Nothing
        // about Fitting Room had changed.
        //
        // So the comparison is wide, which cannot fail, and a match reports the
        // table's OWN narrow spelling rather than the one read from disk. A
        // conflicting plugin's name is ASCII by construction because we wrote
        // it here.
        struct Conflict {
            const wchar_t* wide;
            const char*    narrow;
        };
        static constexpr Conflict kConflicting[]{
            { L"SkyrimOutfitSystemSE.dll", "SkyrimOutfitSystemSE.dll" },
            { L"SkyrimOutfitEquipmentSystemNG.dll", "SkyrimOutfitEquipmentSystemNG.dll" },
            { L"SkyrimVanitySystem.dll", "SkyrimVanitySystem.dll" },
        };

        // ⚠ AND THE WALK IS STEPPED BY HAND. A range-for over
        // directory_iterator increments through the THROWING overload, so an
        // entry that goes unreadable mid-walk is the same crash by a different
        // route. The error_code form turns both ends of the walk into a return.
        std::error_code                 ec;
        std::filesystem::directory_iterator it{ "Data/SKSE/Plugins", ec };
        if (ec) {
            return;
        }
        const std::filesystem::directory_iterator end{};
        std::vector<std::string>                   found;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                return;
            }
            const std::wstring name = it->path().filename().native();
            for (const auto& candidate : kConflicting) {
                if (_wcsicmp(name.c_str(), candidate.wide) == 0) {
                    found.push_back(candidate.narrow);
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
