#pragma once

#include "Outfit.h"  // StyleRefKey

#include <string>

namespace OS::CrashGuard {

    // A safety net for third-party armor whose mesh crashes the engine's
    // skinning pass during our forced biped rebuild (reliable, per-item: e.g.
    // Elianora Battle Mage [VANILLA] cuirasses, vanilla-body meshes on a
    // modded body). We cannot fix the mesh and cannot safely catch an access
    // violation deep in the engine's skinning, so instead we make the SAME
    // item crash at most ONCE: a pending marker written around the rebuild
    // survives a crash and, on the next launch, promotes that style to a
    // persistent "crashers" list that the browser flags and hides.

    // kDataLoaded, BEFORE StyleCatalog::Build(): load the crashers list and, if
    // a pending marker survived last session, promote its style to a crasher.
    void LoadAtStartup();

    // EditorUI: the style whose preview rebuild is about to run (set on stage,
    // cleared on close / real-gear / hide). Only a set key is ever blamed.
    void SetPreviewing(const StyleRefKey& a_key);
    void ClearPreviewing();

    // REAug::RefreshPlayer brackets the biped rebuild that skins the meshes:
    // BeginKick writes the pending marker (if a preview key is set), EndKick
    // deletes it (the rebuild survived). A crash between them leaves the marker.
    void BeginKick();
    void EndKick();

    // True if this style crashed a preview on a previous launch.
    [[nodiscard]] bool IsCrasher(const StyleRefKey& a_key);

    // How many styles are on the crashers list (for the load log line).
    [[nodiscard]] std::size_t Count();

}  // namespace OS::CrashGuard
