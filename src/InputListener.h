#pragma once
#include "PCH.h"

namespace OS {

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
    };

}  // namespace OS
