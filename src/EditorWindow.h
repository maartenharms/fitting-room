#pragma once

namespace OS::EditorWindow {

    // The Outfit Slots editor, hosted as a FUCK IWindow (Phase 3). Opened by the
    // editor hotkey (InputListener) over one of the menus HostGuard allows; FUCK
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

    // The preview grid's per-present pump: release last present's evicted
    // FUCK handles, then drain the build queue. Called by DyeTexture's
    // Present thunk, OUTSIDE FUCK's whole UI pass; no-op while the editor is
    // closed or not yet populated. Never call it from Draw - the D3D and
    // FUCK-image work inside FUCK's own pass is what hid the editor on
    // loading frames (field 2026-08-09).
    void PumpPreviews();

    void Toggle();        // gated open/close, marshaled to the main thread
    // Open if a menu HostGuard allows is up - for the seamstone, the
    // OutfitSlots_Open mod-event, and the SAM Papyrus entry.
    void RequestOpen();
    void RequestClose();  // close from the render thread (the on-screen Close button)

    // Close, but ask first when there is unpaid work to lose (see
    // EditorUI::CloseWouldLoseWork). For the DELIBERATE exits only: Escape, the
    // editor hotkey and the action-bar button.
    //
    // ⚠ THE INVOLUNTARY PATHS MUST KEEP CALLING RequestClose. HostGuard closes
    // because the host menu has already gone and SamCompat because SAM has;
    // there is nothing left to draw a modal over and nothing the player could
    // usefully answer, so those close outright. Asking is the opt-in, which is
    // also why adding it could not break an existing caller.
    void RequestCloseAsking();

    // Stage the open editor again, in place, without closing it.
    //
    // ⚠⚠ THIS IS WHAT AN APPLY OWES THE EDITOR NOW THAT IT NO LONGER CLOSES IT.
    // A look's face step rebuilds the head and its outfit step activates a
    // DIFFERENT library entry under the editor's staged copy, and the staging
    // machinery has no seam for that; closing used to hand the player the
    // settled result and let the next open stage from scratch. The user wants to
    // stay in the editor after an apply, so the apply calls this at its settle
    // instead, which is the same OnOpen an open would have run. Queued onto the
    // main thread, and a no-op while the editor is closed.
    void RequestRestage();

    // Snap the window back to its default position/size (the gear's "Reset window position").
    void ResetGeometry();

}  // namespace OS::EditorWindow
