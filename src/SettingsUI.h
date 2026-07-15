#pragma once

namespace OS::SettingsUI {

    // In-game config panel via FLICK/FUCK (Fuzzles' framework), a FUCK::ITool in
    // its sidebar. Pure runtime SOFT dependency: without FUCK.dll the mod stays
    // INI-only and logs one line - nothing else changes. Call Register() once at
    // kDataLoaded, AFTER Settings::Load() (the panel edits the Settings singleton).
    void Register();

}  // namespace OS::SettingsUI
