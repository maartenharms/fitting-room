#pragma once

namespace OS::EditorWindow {

    // The Outfit Slots editor, hosted as a FUCK IWindow (Phase 3). Opened by the
    // editor hotkey (InputListener) over the inventory / Screen Archer Menu; FUCK
    // owns Present, input, scaling and the vanilla styling, and a thin shim in
    // SetOpen keeps the parts FUCK has no concept of (hide the 2D chrome while
    // keeping the Show-Player-In-Menus character, force third person, scene
    // guard, fit refresh). Replaces the bespoke ImGuiOverlay. Register once at
    // kDataLoaded, AFTER SettingsUI::Register (which calls FUCK::Connect).
    void Register();

    [[nodiscard]] bool IsOpen();
    // A FUCK text field is active - so the editor hotkey (a printable letter)
    // must not close the editor mid-type. Best-effort, one frame stale at most.
    [[nodiscard]] bool WantsTextInput();
    // The cursor is over a FUCK window or an active widget, i.e. the panel owns
    // this click rather than the world behind it. Cached on the render thread in
    // Draw(); one frame stale at most, which is why the camera drag latches it
    // on the button edge instead of sampling it per mouse-move.
    [[nodiscard]] bool CursorOverUI();
    // The editor was opened over Screen Archer Menu, so SAM framed this shot and
    // owns the camera (OS-80c). Gates the camera drag OFF - SAM orbits by writing
    // the same FreeCameraState fields our free-look drag writes, so driving it
    // here is a second writer racing SAM - and gates the wheel passthrough ON, so
    // SAM's FOV zoom can reach it. Latched at open; SAM's menu is up at that point
    // in the shooting workflow.
    [[nodiscard]] bool OpenedFromSam();

    void Toggle();        // gated open/close, marshaled to the main thread
    // Open if a permitted context (inventory / SAM) is up - for the seamstone,
    // the OutfitSlots_Open mod-event, and the SAM Papyrus entry.
    void RequestOpen();
    void RequestClose();  // close from the render thread (the on-screen Close button)
    // Snap the window back to its default position/size (the gear's "Reset window position").
    void ResetGeometry();

}  // namespace OS::EditorWindow
