#include "InputListener.h"

#include "EditorGate.h"
#include "EditorWindow.h"
#include "ImGuiOverlay.h"
#include "LoreModule.h"
#include "OutfitSession.h"
#include "SamCompat.h"
#include "Settings.h"

#include <cmath>  // std::fmod - the free-camera angle wrap

namespace OS {

    namespace {

        // OS-73. Drive the FREE camera (tfc) directly from the drag, because in
        // the NO-SAM case nothing else will: the engine does not route look
        // input to the camera while a menu context owns it, so with the editor
        // open over the inventory this is the only camera control there is.
        //
        // OS-80c, 2026-07-19: NOT under SAM. This path is now gated off whenever
        // EditorWindow::OpenedFromSam(), because SAM is an orbit+pan+FOV camera
        // and this is free-look - a different interaction that CANNOT be tuned
        // into feeling like SAM by adjusting sensitivity or signs. Worse, SAM
        // orbits by writing translation and rotation on the same FreeCameraState
        // instance, so both writers race the two floats below. The gate lives at
        // the LMB arm in ProcessEvent; see the note there for the field evidence.
        //
        // Correcting an over-claim in the previous version of this comment: alpha-
        // hiding SAM's 2D does NOT make SAM's camera controls unreachable. They are
        // raw mouse gestures, not menu buttons, and kPassInputToGame delivers them.
        // The user reaching SAM's camera at all through the passthrough is what
        // proved it.
        //
        // ONLY the free camera. In third person the editor already hands the
        // gesture to the game (kPassInputToGame in EditorWindow::GetFlags,
        // granted for precisely this drag), and the vanilla menu rotates the
        // character itself - writing the camera here as well would apply the
        // drag twice, the SPIM double-rotation failure. The free camera does
        // not double-apply for the same reason it needed this at all: the
        // passed-through look never reaches it.
        //
        // FreeCameraState is absent from this CommonLib snapshot, so the layout
        // is BYTE-VERIFIED off both shipped binaries rather than assumed - the
        // previous (never-executed) version of this code had it wrong:
        //   +0x30  NiPoint3 translation   (GetTranslation reads 0x30/0x34/0x38)
        //   +0x3C  float    pitch         (GetRotation reads 0x3C, engine SUBTRACTS the vertical axis)
        //   +0x40  float    yaw           (GetRotation reads 0x40, engine ADDS the horizontal axis)
        // Identical on SE 1.5.97 (vtable 0x16A9F50) and AE 1.6.1170 (0x18EF2E8),
        // read out of FreeCameraState's own GetRotation/GetTranslation and its
        // update helper (SE 140848AA0 / AE 1408E0640). The old +0x2C would have
        // written yaw into translation.x and teleported the camera.
        //
        // Signs and wrapping mirror that helper exactly, so a drag feels like
        // ordinary free-camera look: the engine wraps BOTH fields into
        // [0, 2pi) each frame and clamps neither (the free camera is meant to
        // loop over the top), so we wrap too rather than inventing a pitch limit.
        // OS-80 diagnostics. Every gate in this path used to decline SILENTLY,
        // so a field report of "the camera will not move with SAM" could not say
        // WHICH gate refused - and the three candidates need opposite fixes. One
        // line per gesture (reset on the LMB edge, never per mouse-move) names
        // the reason. Reads as noise in a log only until it saves a round trip.
        bool g_dragDiagDone{ false };

        void ApplyFreeCameraDrag(float a_dx, float a_dy) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->currentState) {
                if (!g_dragDiagDone) {
                    g_dragDiagDone = true;
                    spdlog::info("camera drag: no PlayerCamera/currentState - drag ignored.");
                }
                return;
            }
            auto* const state = cam->currentState.get();
            if (state->id != RE::CameraState::kFree) {
                if (!g_dragDiagDone) {
                    g_dragDiagDone = true;
                    spdlog::info(
                        "camera drag: camera state is {}, not kFree(3) - drag ignored. "
                        "If SAM framed this shot, that number IS SAM's camera, and this "
                        "path's free-camera-only scope is why nothing moves.",
                        static_cast<int>(state->id));
                }
                return;
            }
            if (!g_dragDiagDone) {
                g_dragDiagDone = true;
                spdlog::info("camera drag: driving the free camera (state 3) - "
                             "gates passed, writing pitch/yaw.");
            }
            constexpr float kTwoPi = 6.2831853f;
            const float     s      = Settings::GetSingleton().cameraDragSensitivity;
            auto* const     rot =
                reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(state) + 0x3C);

            float pitch = rot[0] - a_dy * s;
            float yaw   = rot[1] + a_dx * s;
            pitch       = std::fmod(pitch, kTwoPi);
            yaw         = std::fmod(yaw, kTwoPi);
            if (pitch < 0.0f) {
                pitch += kTwoPi;
            }
            if (yaw < 0.0f) {
                yaw += kTwoPi;
            }
            rot[0] = pitch;
            rot[1] = yaw;
        }

    }  // namespace

    // OS-80 ROOT CAUSE. The editor runs as a FLICK IWindow, and FLICK owns the
    // mouse while its window is up, so the BSInputEvent sink further down never
    // receives the click that used to drive this. The drag was architecturally
    // stranded when the editor moved off Fitting Room's own ImGuiOverlay - the
    // sink code is intact and correct and simply never runs, which is why the
    // field symptom was total silence rather than a wrong rotation.
    //
    // EditorWindow::Draw calls this instead, from inside FLICK's own frame where
    // the mouse delta is real (FUCK::GetMouseDelta, FLICK's ImGui context - NOT
    // ImGui::GetIO(), which is a different context in this process). The camera
    // work itself is unchanged and still uses the byte-verified offsets.
    void ApplyEditorCameraDrag(float a_dx, float a_dy) { ApplyFreeCameraDrag(a_dx, a_dy); }

    InputListener& InputListener::GetSingleton() {
        static InputListener instance;
        return instance;
    }

    void InputListener::Register() {
        const auto& settings = Settings::GetSingleton();
        auto&       self     = GetSingleton();
        self.editorKey_      = settings.editorKeyDIK;
        self.editorPad_      = settings.editorGamepadButton;
        self.nextKey_        = settings.nextOutfitKeyDIK;

        if (!self.editorKey_ && !self.editorPad_ && !self.nextKey_) {
            spdlog::info("InputListener NOT registered (no keys bound).");
            return;
        }
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(static_cast<RE::BSTEventSink<RE::InputEvent*>*>(&self));
            spdlog::info("InputListener registered (editor DIK 0x{:X}, pad 0x{:X}, "
                         "next-outfit DIK 0x{:X}).",
                         self.editorKey_, self.editorPad_, self.nextKey_);
        }
    }

    void InputListener::CycleOutfit() {
        auto&       session = OutfitSession::GetSingleton();
        std::string name;
        bool        changed = false;
        session.WithLibrary([&](OutfitLibrary& lib) {
            const auto count = lib.Count();
            if (count == 0) {
                return;
            }
            const int before = lib.ActiveIndex();
            const int next   = lib.CycleIncludingEquipped(true);
            changed          = next != before;
            if (next < 0) {
                name = "Equipped gear";
            } else if (const auto* o = lib.At(static_cast<std::size_t>(next))) {
                name = o->name;
            }
        });
        if (changed && !name.empty()) {
            OutfitSession::RequestRefresh();
            RE::DebugNotification(("Outfit: " + name).c_str());
            spdlog::info("quick-switch: '{}' activated.", name);
        }
    }

    RE::BSEventNotifyControl InputListener::ProcessEvent(
        RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) {
        if (!a_events) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto& overlay = ImGuiOverlay::GetSingleton();
        for (auto* e = *a_events; e; e = e->next) {
            const auto* btn = e->AsButtonEvent();

            if (btn && btn->IsDown()) {
                const auto device = btn->GetDevice();
                const auto code   = btn->GetIDCode();
                const bool editorHit =
                    (device == RE::INPUT_DEVICE::kKeyboard && editorKey_ && code == editorKey_) ||
                    (device == RE::INPUT_DEVICE::kGamepad && editorPad_ && code == editorPad_);
                if (editorHit) {
                    // Opening requires a permitted context: the inventory (the
                    // editor is composed around the Show-Player-In-Menus
                    // character) OR Screen Archer Menu (screenarchery). Lore mode
                    // adds the Seamstone requirement on top. Closing is allowed
                    // any time except while typing (the key is a printable letter).
                    auto* ui = RE::UI::GetSingleton();
                    const bool inInventory =
                        ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
                    const bool canOpenHere = inInventory || SamCompat::IsMenuOpen();
                    const bool seamstoneOk =
                        !(Settings::GetSingleton().requireSeamstone &&
                          LoreModule::Available()) ||
                        LoreModule::HasSeamstone();
                    // The editor hotkey opens/closes the FUCK IWindow (Phase 3).
                    switch (EditorGate::DecideGate(EditorWindow::IsOpen(),
                                                   EditorWindow::WantsTextInput(),
                                                   canOpenHere, seamstoneOk)) {
                        case EditorGate::GateAction::kClose:
                        case EditorGate::GateAction::kOpen:
                            EditorWindow::Toggle();
                            break;
                        case EditorGate::GateAction::kNeedContext:
                            RE::DebugNotification(
                                "Open your inventory or Screen Archer Menu to edit outfits.");
                            break;
                        case EditorGate::GateAction::kNeedSeamstone:
                            RE::DebugNotification("You need a seamstone. Farengar of "
                                                  "Dragonsreach sells one.");
                            break;
                        case EditorGate::GateAction::kIgnore:
                            break;
                    }
                    continue;
                }
            }

            // OS-73 camera drag, editor-only. Latch on the LMB edges, apply on
            // mouse move. This sink is the raw device feed, upstream of the
            // menu control map, so it still sees both while a menu context owns
            // input - which is the whole reason the free camera can be driven
            // from here at all. Everything below is inert with the editor shut.
            if (EditorWindow::IsOpen() && Settings::GetSingleton().cameraDragWhileOpen) {
                if (btn && btn->GetDevice() == RE::INPUT_DEVICE::kMouse &&
                    btn->GetIDCode() == 0) {
                    if (btn->IsDown()) {
                        // OS-80c: when SAM framed the shot it OWNS the camera.
                        // SAM orbits by writing translation AND rotation on the
                        // very FreeCameraState object this drag writes pitch/yaw
                        // into, so arming here is a second writer racing SAM on
                        // +0x3C/+0x40 every mouse-move. That is field-proven, not
                        // theory: the 2026-07-19 02:46 log shows every gate PASSED
                        // and the write happening, and the shot still felt "not at
                        // all like SAM". Decline instead; kPassInputToGame already
                        // hands the gesture to SAM, now uncontested.
                        const bool samOwnsCamera = EditorWindow::OpenedFromSam();
                        const bool overUI        = EditorWindow::CursorOverUI();
                        worldDrag_               = !overUI && !samOwnsCamera;
                        // OS-80: a fresh gesture, so let the camera path speak
                        // once more. This line also proves the sink SEES the
                        // click at all - if it is ABSENT from a failing session
                        // then the input-block hook consumed the event upstream
                        // and every camera gate below is innocent.
                        g_dragDiagDone = false;
                        spdlog::info("camera drag: LMB down, cursorOverUI={} -> {}.",
                                     overUI,
                                     worldDrag_      ? "ARMED"
                                     : samOwnsCamera ? "declined (SAM owns the camera)"
                                                     : "declined (click was over a panel)");
                    } else if (btn->IsUp()) {
                        worldDrag_ = false;
                    }
                } else if (worldDrag_ &&
                           e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    if (const auto* move = static_cast<const RE::MouseMoveEvent*>(
                            e->AsIDEvent())) {
                        ApplyFreeCameraDrag(static_cast<float>(move->mouseInputX),
                                            static_cast<float>(move->mouseInputY));
                    }
                }
            } else {
                worldDrag_ = false;  // never resume a drag across a close
            }

            if (overlay.IsOpen()) {
                // Fallback feed for the one frame before the input-block hook
                // installs (first Present after first open). Once installed, the
                // hook empties the event list before this sink runs, so while
                // open this sink receives nothing and the real feed happens in
                // ImGuiOverlay::HandleModalInput. See InputDispatchHook.
                overlay.FeedEvent(e);
                continue;
            }

            if (!btn || !btn->IsDown()) {
                continue;
            }
            if (btn->GetDevice() == RE::INPUT_DEVICE::kKeyboard && nextKey_ &&
                btn->GetIDCode() == nextKey_) {
                CycleOutfit();
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace OS
