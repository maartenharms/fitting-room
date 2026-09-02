#pragma once

// Fitting Room's button on Menu Studio's action bar - the floating strip MS
// draws over its bubbled menus. Registered through MS's C ABI the same way
// this plugin already consumes FUCK.dll: GetModuleHandle + GetProcAddress,
// nothing links against anything, and either mod's absence costs exactly one
// button.
namespace OS::MenuStudioCompat {

    // kDataLoaded. Resolves MenuStudio_RegisterAction and, when present, adds
    // one "Fitting Room" button that opens the editor (or closes it when it is
    // already up). Safe to call with Menu Studio absent or too old to have an
    // action bar - both are one debug line, not an error.
    void Register();

    // Show or hide the button to match the Seamstone gate, and call it wherever
    // that answer can change: at load, and whenever the player's inventory
    // moves. In lore mode without the stone the tile is hidden entirely, so the
    // strip carries no sign that this mod is installed until one is found.
    //
    // Safe with Menu Studio absent, too old to have the export, or having
    // renamed the id: all three are a silent no-op, the same posture Register
    // takes.
    void RefreshVisibility();

    // Redraw the tile's charge meter from the stone's current level. Call it
    // wherever the charge MOVES: refilling, paying for a style, paying the
    // character editor's door fee.
    //
    // ⚠ THE METER DOES NOT NOTICE A WRITE ON ITS OWN. It is pushed to Menu
    // Studio, not polled by it, so a charge change with no call here leaves the
    // tile showing the previous level until the next menu opens. That was the
    // 2026-08-29 report: refill in the editor, tile unchanged until you closed
    // and reopened, or opened the console.
    //
    // Same silent-when-absent posture as everything else here.
    void RefreshCharge();

}  // namespace OS::MenuStudioCompat
