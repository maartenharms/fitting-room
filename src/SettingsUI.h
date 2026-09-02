#pragma once

namespace OS::SettingsUI {

    // In-game config panel via FLICK/FUCK (Fuzzles' framework), a FUCK::ITool in
    // its sidebar. Pure runtime SOFT dependency: without FUCK.dll the mod stays
    // INI-only and logs one line - nothing else changes. Call Register() once at
    // kDataLoaded, AFTER Settings::Load() (the panel edits the Settings singleton).
    void Register();

    // The panel body, so the editor's gear popup can show the same controls
    // without the user leaving the editor to find them (OS-130 follow-up,
    // user's call 2026-08-04).
    //
    // ⚠ ONE DEFINITION, TWO CALLERS. The FLICK sidebar tool and the editor gear
    // both call this. Do not copy rows into either caller: two views of one
    // Settings singleton is fine, two sets of widgets writing it is how they
    // drift.
    //
    // ⚠ IT EDITS THE Settings SINGLETON IN PLACE and Saves on its own. A caller
    // holding a cached copy of any setting must re-read it afterwards. EditorUI
    // caches uiScale and hoverPreview, and re-syncs both on return.
    void DrawPanel();

}  // namespace OS::SettingsUI
