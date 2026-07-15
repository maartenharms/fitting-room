#pragma once

#include <array>
#include <filesystem>

namespace OS::Migration {

    // One-time move of USER data from the pre-rename path (…/Plugins/OutfitSlots) to the
    // new one (…/Plugins/FittingRoom) after the "Fitting Room" rename. Best-effort and
    // idempotent (skips files already at the new path). MO2 redirects runtime writes to
    // its overwrite, so the old VFS path stays readable even after the old "Outfit Slots"
    // mod is disabled (the user data lives in overwrite, not the mod folder). The co-save
    // keeps its 'OSLT' unique id across the rename, so save-embedded data (active outfit
    // name, appearance collection) needs no migration - only these on-disk files do.
    // Call once at kDataLoaded BEFORE Settings::Load and the library load.
    inline void RunOnce() {
        namespace fs = std::filesystem;
        std::error_code ec;

        // The INI sits beside the folder, not inside it.
        const fs::path oldIni{ "Data/SKSE/Plugins/OutfitSlots.ini" };
        const fs::path newIni{ "Data/SKSE/Plugins/FittingRoom.ini" };
        if (fs::exists(oldIni, ec) && !fs::exists(newIni, ec)) {
            fs::copy_file(oldIni, newIni, ec);
        }

        const fs::path oldDir{ "Data/SKSE/Plugins/OutfitSlots" };
        const fs::path newDir{ "Data/SKSE/Plugins/FittingRoom" };
        if (!fs::exists(oldDir, ec)) {
            return;  // fresh install, or the old data was already cleaned up
        }
        fs::create_directories(newDir, ec);

        // Only the runtime-written user files - the shipped fonts/icons/Presets come
        // with the new mod folder, so don't drag those across.
        static constexpr std::array kFiles{ "outfits.json", "favorites.txt",
                                             "crashed_styles.txt", "known_plugins.txt",
                                             "pending_preview.txt" };
        int copied = 0;
        for (const auto* f : kFiles) {
            if (fs::exists(oldDir / f, ec) && !fs::exists(newDir / f, ec)) {
                if (fs::copy_file(oldDir / f, newDir / f, ec)) {
                    ++copied;
                }
            }
        }
        // Author/user exports too.
        if (fs::exists(oldDir / "Exports", ec)) {
            fs::copy(oldDir / "Exports", newDir / "Exports",
                     fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        }
        if (copied > 0) {
            spdlog::info("Migration: copied {} user file(s) OutfitSlots -> FittingRoom.", copied);
        }
    }

}  // namespace OS::Migration
