#pragma once
#include "PCH.h"

namespace OS {

    // OS-80: drive the free camera (tfc / SAM) from a mouse delta. Called by
    // EditorWindow::Draw, because FLICK owns the mouse while the editor window
    // is up and the BSInputEvent sink never sees the click. Deltas are raw
    // ImGui mouse deltas; the sensitivity scale and the byte-verified
    // FreeCameraState offsets live with the implementation. No-op unless the
    // camera is genuinely in the free state.
    void ApplyEditorCameraDrag(float a_dx, float a_dy);

    // Input bindings:
    //  - editor key / gamepad button (INI [Input]) toggles the ImGui editor;
    //  - "next outfit" key cycles the active outfit (quick-switch).
    // (The Phase-2 F10 debug harness and the [Outfit] INI stopgap were removed
    //  once the editor was user-verified - plan Task 4.3 Step 3.)
    class InputListener : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputListener& GetSingleton();
        static void           Register();  // kDataLoaded, after Settings::Load

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
                                              RE::BSTEventSource<RE::InputEvent*>*) override;

        // Live-update the editor hotkey from the SKSE settings panel dropdown
        // (the cached copy the input sink compares against). Persisting to the
        // INI is the caller's job.
        void SetEditorKey(std::uint32_t a_dik) { editorKey_ = a_dik; }

    private:
        InputListener() = default;

        void CycleOutfit();

        std::uint32_t editorKey_{ 0 };  // DIK; 0 = unbound
        std::uint32_t editorPad_{ 0 };  // gamepad IDCode; 0 = unbound
        std::uint32_t nextKey_{ 0 };    // DIK; 0 = unbound

        // OS-73 camera drag: LMB is held and it went down OFF the editor's
        // panels, so the gesture belongs to the world and not to a widget.
        // Latched on the button edge (not sampled per move) so dragging ONTO a
        // panel mid-gesture does not cut the drag, matching every other
        // click-drag in the game.
        bool worldDrag_{ false };
    };

}  // namespace OS
