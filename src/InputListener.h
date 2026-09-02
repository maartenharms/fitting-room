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

    // ⚠⚠ THE STICKS COME FROM THIS SINK BECAUSE FUCK NEVER SURFACES EITHER ONE.
    // MEASURED 2026-08-15 from the field log, 134 navprobe lines over a run that
    // worked every control: A, B, X, Y, LB, RB, LT, RT, Start and the D-PAD all
    // arrive as ImGui keys; `LStick*` and `RStick*` appear ZERO times, as do L3
    // and R3. So `FUCK::IsKeyDown(ImGuiKey_GamepadLStick*)` and its RStick twin
    // can only ever answer false, and two features were written on them: the
    // editor's pane hop (dead on arrival, "left stick doesn't do anything") and
    // EditorWindow's `rStickActive_` passthrough gate (dead since OS-53, which
    // is why the right stick has always needed Show Player In Inventory's R3
    // hold). The raw device sink sees both sticks as ThumbstickEvents, which is
    // where SPII reads them too.
    //
    // ⚠ THE HANDOFF SAID "D-pad, left stick: yes" AND THAT WAS ONE CLAIM COVERING
    // TWO INPUTS. Only the d-pad half was ever true. A grouped measurement is not
    // a measurement of each thing in the group.

    // ⚠ ONLY THE RIGHT STICK IS PUBLISHED. The left one has driven a pane hop, an
    // item stepper and a mouse pointer over the course of 2026-08-15 and all
    // three were rejected in the field; it is read by nothing now (user: "just
    // have basic"). The measurement above is kept because it is what makes the
    // polling route necessary, not because anything still hops.

    // The right stick's X for this frame, -1..1, and CONSUMES it. Zero while R3
    // is held (that gesture belongs to Show Player In Inventory), while the
    // stick rests, and while the editor's camera setting is off.
    //
    // ⚠ IT IS A RATE, NOT A DISTANCE. Spend it once a frame; a caller that
    // skipped a frame has simply lost that frame's motion, which is what a
    // turning control should do.
    [[nodiscard]] float TakeEditorLookX();

    // The right stick's Y for this frame, -1..1, and CONSUMES it. Same gates and
    // the same rate rule as the X above. Positive is the stick pushed away from
    // the player.
    [[nodiscard]] float TakeEditorLookY();

    // Sample the right stick off the gamepad device. ⚠ CALL ONCE A FRAME FROM
    // THE EDITOR'S DRAW and from nowhere else: it publishes the frame's look
    // rate, so a second caller doubles it. Polled rather than received for the
    // reason written at the definition - FLICK owns input while its window is up
    // and the event sink below never sees a thing.
    void PollEditorSticks();

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

        // Live-update the hotkeys from the SKSE settings panel dropdowns (the
        // cached copies the input sink compares against). Persisting to the
        // INI is the caller's job.
        void SetEditorKey(std::uint32_t a_dik) { editorKey_ = a_dik; }
        void SetNextOutfitKey(std::uint32_t a_dik) { nextKey_ = a_dik; }
        void SetDirectEntryKey(std::uint32_t a_dik) { directKey_ = a_dik; }

    private:
        InputListener() = default;

        void CycleOutfit();

        std::uint32_t editorKey_{ 0 };  // DIK; 0 = unbound
        std::uint32_t editorPad_{ 0 };  // gamepad IDCode; 0 = unbound
        std::uint32_t nextKey_{ 0 };    // DIK; 0 = unbound
        // OS-207, the key that enters the editor with no menu open. ⚠ IT IS A
        // DIK CODE OUT OF THE SAME DROPDOWN AS THE OTHER TWO. It was a FLICK
        // ManagedHotkey for one build and the binder never captured: it went
        // into its flashing bind state and stayed there, because completing a
        // FUCK bind needs the plugin to pump UpdateManagedHotkey from its own
        // async-input hook, which a sidebar tool that is not drawing cannot do.
        // The field verdict was to make it a dropdown like its neighbours, and
        // that is also the only one of the three that a player can already read
        // in a screenshot of the panel.
        std::uint32_t directKey_{ 0 };  // DIK; 0 = unbound
        // W1 spike (ProfileProbe). INI only, no settings panel dropdown
        // and no setter: a debug key that dies with its probe does not earn
        // UI. Read once at Register, like everything here.
        std::uint32_t profileProbeKey_{ 0 };  // DIK; 0 = disarmed

        // OS-73 camera drag: LMB is held and it went down OFF the editor's
        // panels, so the gesture belongs to the world and not to a widget.
        // Latched on the button edge (not sampled per move) so dragging ONTO a
        // panel mid-gesture does not cut the drag, matching every other
        // click-drag in the game.
        bool worldDrag_{ false };
    };

}  // namespace OS
