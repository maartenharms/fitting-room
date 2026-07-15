#pragma once

#include "PCH.h"

#include "Settings.h"

namespace OS::SamCompat {

    // True while Screen Archer Menu's Scaleform menu is open. The menu name is
    // read from Settings ([Compat] sSamMenuName, default "ScreenArcherMenu").
    // Stays true while ImGuiOverlay's ShowMenus(false) hides SAM - that toggles
    // rendering, not the menu stack - so it works for both the open gate and the
    // "opened from SAM" context check.
    [[nodiscard]] inline bool IsMenuOpen() {
        auto*       ui   = RE::UI::GetSingleton();
        const auto& name = Settings::GetSingleton().samMenuName;
        return ui && !name.empty() && ui->IsMenuOpen(name);
    }

    // Register the mod-event bridge: any mod (a Screen Archer Menu addon, MCM,
    // hotkey mod, …) can open/close the editor by firing the Papyrus mod events
    //   OutfitSlots_Open   OutfitSlots_Close   OutfitSlots_Toggle
    // via SKSE's SendModEvent. Call at kDataLoaded (after the overlay exists).
    void Register();

}  // namespace OS::SamCompat
