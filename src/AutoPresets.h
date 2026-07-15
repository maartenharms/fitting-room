#pragma once

#include "PCH.h"

#include "JsonCodec.h"

#include <mutex>
#include <vector>

namespace OS {

    // Auto-detected outfit presets (the "Discovered" tab). Derived from
    // StyleCatalog each launch - never persisted. Mirrors PresetStore so the
    // Showcases browser can consume either list.
    class AutoPresets {
    public:
        static AutoPresets& GetSingleton();

        // Cluster the fitting catalog into single-plugin sets. Main thread,
        // AFTER fit is evaluated (kPostLoadGame) and while the editor is closed.
        void Generate();

        [[nodiscard]] std::vector<JsonCodec::Preset> Snapshot() const;
        [[nodiscard]] std::size_t                    Count() const;

        // Re-run Generate() on the main thread (the Discovered "Rescan" button).
        static void RequestRescan();

    private:
        AutoPresets() = default;

        mutable std::mutex             lock_;
        std::vector<JsonCodec::Preset> presets_;
    };

}  // namespace OS
