#pragma once

#include "PCH.h"

#include "JsonCodec.h"

#include <mutex>
#include <string>
#include <vector>

namespace OS {

    // Showcase presets: curated outfits shipped INSIDE armor mods as
    // Data/SKSE/Plugins/FittingRoom/Presets/*.json (the MO2/Vortex VFS merges
    // every mod's Presets folder into one). Discovered at kDataLoaded; a
    // preset whose `requires` plugins are absent is skipped with a log line.
    // Read-only data - the player copies a preset into the library to own it.
    class PresetStore {
    public:
        static PresetStore& GetSingleton();

        // Scan + parse + filter. Main thread (kDataLoaded or a queued task) -
        // needs TESDataHandler for the `requires` check.
        void Load();

        // Consistent copy for the render thread (editor draw loop).
        [[nodiscard]] std::vector<JsonCodec::Preset> Snapshot() const;
        [[nodiscard]] std::size_t                    Count() const;

        // Re-run Load() on the main thread - the authoring loop: drop a file
        // in Presets/, click Rescan, see it. Safe from any thread.
        static void RequestRescan();

        // "Share this outfit": write a_outfit to
        // Data/SKSE/Plugins/FittingRoom/Exports/<name>.json in the exact
        // preset schema (author/description left blank for the author;
        // `requires` prefilled with the non-vanilla plugins the styles
        // reference). Returns the path, or empty on failure (logged).
        [[nodiscard]] static std::string ExportOutfit(const Outfit& a_outfit);

    private:
        PresetStore() = default;

        mutable std::mutex             lock_;
        std::vector<JsonCodec::Preset> presets_;
    };

}  // namespace OS
