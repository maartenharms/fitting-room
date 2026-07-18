#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace OS {

    // The editor's render surface: lazily hooks IDXGISwapChain::Present
    // (vtable[8]) on first open, draws EditorUI inside the ImGui frame, and
    // takes gameplay input away from the game while open. Never unhooked -
    // SKSE plugins do not unload.
    class ImGuiOverlay {
    public:
        static ImGuiOverlay& GetSingleton();

        void Toggle();
        [[nodiscard]] bool IsOpen() const noexcept { return open_; }

        // True when the last input fed to the editor came from a gamepad (set on
        // gamepad button / thumbstick, cleared on mouse or keyboard). Drives the
        // controller-only on-screen hint in EditorUI. Best-effort, one frame of
        // lag at most.
        [[nodiscard]] bool UsingGamepad() const noexcept { return usingGamepad_; }

        // Close the editor from the render thread (the on-screen Close button).
        // Marshaled to the main thread - Toggle() touches engine state.
        void RequestClose();

        // Open the editor from an external trigger (the OutfitSlots_Open mod
        // event - e.g. a Screen Archer Menu entry). Marshaled to the main
        // thread; opens only from a valid UI context (inventory or SAM), since
        // the editor is a modal that hides the menu behind it.
        void RequestOpen();

        // True while an ImGui text field has focus. Hotkeys that overlap
        // printable characters (the editor toggle 'O') and the cancel event
        // (Esc first unfocuses a field, second press closes) must defer.
        [[nodiscard]] bool WantsTextInput() const;

        // True while the mouse is over the editor panel (ImGui wants the mouse).
        // The input-block hook uses this: block input only here (the list-scroll
        // that leaked to SAM's FOV); when the mouse is over the 3D character,
        // input passes through so the inventory/SAM can rotate the preview.
        [[nodiscard]] bool WantsCaptureMouse() const;

        // Install the MenuControls handler that makes the editor MODAL while
        // open: every input event is consumed before the (hidden) menu stack
        // sees it, and Esc (keyboard) / Start (gamepad) close the editor. Pad B
        // is left for ImGui panel-back navigation. Call once at kDataLoaded.
        void RegisterMenuGuard();

        // While the editor is open, the input-block hook calls this with the
        // raw event list: it feeds every event to ImGui and detects the close
        // inputs (editor hotkey, Esc, gamepad Start). The caller then hands the
        // game an empty list, so nothing downstream (game, SAM, Wheeler) sees
        // input while the modal editor owns it.
        void HandleModalInput(RE::InputEvent* const* a_events);

        // Feed one of the game's own input events to ImGui. Skyrim reads the
        // mouse through DirectInput in EXCLUSIVE mode, so WM_* button messages
        // never reach the game window - the WndProc path alone can render but
        // not click. InputListener calls this for every event while the editor
        // is open (the pattern Photo Mode / dMenu use).
        void FeedEvent(const RE::InputEvent* a_event);

    private:
        ImGuiOverlay() = default;

        void EnsureInit(IDXGISwapChain* a_swapChain);
        void OnOpen();
        void OnClose();

        // (The camera drag lived here until 2026-07-18. This class was retired
        // by ddb5c4a - the editor is a FUCK IWindow now - so it never ran, and
        // its raw FreeCameraState offset was wrong besides. It lives in
        // InputListener::ProcessEvent, byte-verified; see ApplyFreeCameraDrag.)

        using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        static HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain*, UINT, UINT);
        static LRESULT CALLBACK          WndProc(HWND, UINT, WPARAM, LPARAM);

        static inline Present_t g_origPresent{ nullptr };
        static inline WNDPROC   g_origWndProc{ nullptr };

        ID3D11Device*        device_{ nullptr };
        ID3D11DeviceContext* context_{ nullptr };
        HWND                 hwnd_{ nullptr };
        bool                 initialized_{ false };
        bool                 open_{ false };
        bool                 wasFirstPerson_{ false };
        bool                 wasShowingMenus_{ true };
        bool                 openedFromSam_{ false };  // opened over Screen Archer Menu
        float                mouseX_{ 0.0f };  // accumulated from MouseMoveEvent deltas
        float                mouseY_{ 0.0f };
        bool                 usingGamepad_{ false };  // last input source (gamepad vs mouse/kbd)
        bool                 worldDrag_{ false };  // LMB held down over the world (camera drag)
        bool                 forcedFreeRotation_{ false };  // we enabled TPS free-rotation; undo on close
    };

}  // namespace OS
