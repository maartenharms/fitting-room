#include "ImGuiOverlay.h"

#include "EditorStyle.h"
#include "EditorUI.h"
#include "KeyboardArbiter.h"
#include "SamCompat.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "StyleCatalog.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>  // std::clamp
#include <mutex>      // std::once_flag - install the input-block hook once
#include <utility>    // std::to_underlying

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace OS {

    namespace {
        // Patch one COM vtable entry (process-global; there is one swapchain).
        template <class T>
        void PatchVtable(void* a_instance, std::size_t a_index, T a_detour, T* a_orig) {
            void** vtbl = *reinterpret_cast<void***>(a_instance);
            DWORD  prot{};
            ::VirtualProtect(&vtbl[a_index], sizeof(void*), PAGE_READWRITE, &prot);
            *a_orig       = reinterpret_cast<T>(vtbl[a_index]);
            vtbl[a_index] = reinterpret_cast<void*>(a_detour);
            ::VirtualProtect(&vtbl[a_index], sizeof(void*), prot, &prot);
        }

        // DIK scancode -> ImGuiKey, for the navigation/editing keys the UI needs.
        ImGuiKey DIKToImGuiKey(std::uint32_t a_dik) {
            switch (a_dik) {
                case 0x01: return ImGuiKey_Escape;
                case 0x0E: return ImGuiKey_Backspace;
                case 0x0F: return ImGuiKey_Tab;
                case 0x1C: return ImGuiKey_Enter;
                case 0x39: return ImGuiKey_Space;
                case 0xC7: return ImGuiKey_Home;
                case 0xC8: return ImGuiKey_UpArrow;
                case 0xC9: return ImGuiKey_PageUp;
                case 0xCB: return ImGuiKey_LeftArrow;
                case 0xCD: return ImGuiKey_RightArrow;
                case 0xCF: return ImGuiKey_End;
                case 0xD0: return ImGuiKey_DownArrow;
                case 0xD1: return ImGuiKey_PageDown;
                case 0xD3: return ImGuiKey_Delete;
                default:   return ImGuiKey_None;
            }
        }

        // DIK scancode -> printable char (lowercase QWERTY), for the search box.
        char DIKToChar(std::uint32_t a_dik) {
            constexpr std::string_view kRow1 = "1234567890";   // 0x02..0x0B
            constexpr std::string_view kRow2 = "qwertyuiop";   // 0x10..0x19
            constexpr std::string_view kRow3 = "asdfghjkl";    // 0x1E..0x26
            constexpr std::string_view kRow4 = "zxcvbnm";      // 0x2C..0x32
            if (a_dik >= 0x02 && a_dik <= 0x0B) return kRow1[a_dik - 0x02];
            if (a_dik >= 0x10 && a_dik <= 0x19) return kRow2[a_dik - 0x10];
            if (a_dik >= 0x1E && a_dik <= 0x26) return kRow3[a_dik - 0x1E];
            if (a_dik >= 0x2C && a_dik <= 0x32) return kRow4[a_dik - 0x2C];
            if (a_dik == 0x39) return ' ';
            if (a_dik == 0x0C) return '-';
            return 0;
        }

        // The control groups we take away from the game while the editor is open.
        RE::UserEvents::USER_EVENT_FLAG BlockedControls() {
            using F = RE::UserEvents::USER_EVENT_FLAG;
            return static_cast<F>(
                std::to_underlying(F::kMovement) | std::to_underlying(F::kLooking) |
                std::to_underlying(F::kActivate) | std::to_underlying(F::kMenu) |
                std::to_underlying(F::kFighting) | std::to_underlying(F::kSneaking) |
                std::to_underlying(F::kVATS) | std::to_underlying(F::kWheelZoom) |
                std::to_underlying(F::kMainFour) | std::to_underlying(F::kPOVSwitch));
        }
    }

    namespace {
        // Cross-mod contract with Apparel Preview: clear any hover preview.
        constexpr std::uint32_t kClearPreviewMsg = 'CLRP';

        // Keyboard source arbitration. The keyboard is USUALLY non-exclusive in
        // menus (WM key/char messages flow; the WndProc backend owns them), but
        // the user hit a state where WM keyboard died on menu re-entry and
        // typing went dead. Self-healing rule: whenever WndProc has seen a
        // keyboard message recently, the engine-event feed stays silent; if WM
        // goes quiet, the feed takes over. Worst case at a takeover boundary is
        // one doubled character - strictly better than dead input either way.
        std::atomic<std::uint32_t> g_lastWmKeyTick{ 0 };

        // Left-stick -> D-pad nav emulation: the direction currently "held"
        // (0 none, 1 up, 2 down, 3 left, 4 right). Edge-triggered so a resting
        // stick never fights the real D-pad. Touched only on the input thread.
        int g_stickNavDir = 0;

        bool WndProcOwnsKeyboard() {
            return ::GetTickCount() - g_lastWmKeyTick.load(std::memory_order_relaxed) < 1000;
        }

        // The engine keyboard feed is deferred one input frame through this
        // arbiter to kill the doubled first keystroke (OS-46). See
        // KeyboardArbiter.h. Input-thread-only, like g_stickNavDir above.
        KeyboardArbiter g_keyArbiter;

        // Feed one decided keyboard transition to ImGui: the nav-key event plus,
        // for a press while a text field has focus, the character.
        void FeedKeyToImGui(ImGuiIO& a_io, std::uint32_t a_dik, bool a_down) {
            if (const auto key = DIKToImGuiKey(a_dik); key != ImGuiKey_None) {
                a_io.AddKeyEvent(key, a_down);
            }
            if (a_down && a_io.WantTextInput) {
                if (const char c = DIKToChar(a_dik)) {
                    a_io.AddInputCharacter(static_cast<unsigned int>(c));
                }
            }
        }

        // Replay (or drop) the keys held from the previous input frame - called
        // once at the top of each input frame, before this frame's events. If
        // WndProc has since claimed the keyboard the holds were WM's and are
        // dropped (no double); otherwise WM is dead and they're fed now.
        void DrainDeferredKeyboard() {
            for (const auto& k : g_keyArbiter.Drain(WndProcOwnsKeyboard())) {
                FeedKeyToImGui(ImGui::GetIO(), k.dik, k.down);
            }
        }

        // While the editor is open it is MODAL over the menu stack: consume
        // every input event before menus see it - the InventoryMenu beneath is
        // hidden (ShowMenus(false)) and must not react to stray keys - and
        // close on the cancel event. Raw-device sinks (our InputListener feed)
        // sit UPSTREAM of MenuControls, so ImGui input is unaffected.
        struct EditorMenuGuard : RE::MenuEventHandler {
            static void QueueClose() {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([] {
                        auto& overlay = ImGuiOverlay::GetSingleton();
                        if (overlay.IsOpen()) {
                            overlay.Toggle();
                        }
                    });
                }
            }

            bool CanProcess(RE::InputEvent* a_event) override {
                return a_event && ImGuiOverlay::GetSingleton().IsOpen();
            }
            bool ProcessButton(RE::ButtonEvent* a_event) override {
                if (!ImGuiOverlay::GetSingleton().IsOpen()) {
                    return false;
                }
                if (a_event && a_event->IsDown()) {
                    // Close on Esc (keyboard) and Start (gamepad). NOT on the
                    // mapped "cancel" user event and NOT on pad B: ImGui polls
                    // XInput directly for gamepad navigation, where B is the
                    // "back" button used to step between panels and out of
                    // sub-controls (user: "B is actually necessary to navigate
                    // the panels"). Consuming B to close stole that. Start is
                    // free - ImGui nav never uses it - and reads as "close menu"
                    // on a controller. Esc still needs its raw fallback because
                    // our modal state (ToggleControls + hidden menus) strips the
                    // cancel mapping. There is also an on-screen Close button.
                    const bool close =
                        (a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                         a_event->GetIDCode() == 0x01) ||  // Esc, even if unmapped
                        (a_event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
                         a_event->GetIDCode() == RE::BSWin32GamepadDevice::Key::kStart);
                    // While a text field is focused, the first Esc only
                    // unfocuses it (ImGui handles that); closing waits for the
                    // next press - native text-field behavior.
                    if (close && !ImGuiOverlay::GetSingleton().WantsTextInput()) {
                        QueueClose();
                    }
                }
                return true;  // consumed
            }
            bool ProcessMouseMove(RE::MouseMoveEvent*) override {
                return ImGuiOverlay::GetSingleton().IsOpen();
            }
            bool ProcessThumbstick(RE::ThumbstickEvent*) override {
                return ImGuiOverlay::GetSingleton().IsOpen();
            }
            bool ProcessKinect(RE::KinectEvent*) override {
                return ImGuiOverlay::GetSingleton().IsOpen();
            }
        };
        EditorMenuGuard g_menuGuard;

        // Input-dispatch hook. While the editor is open, feed ImGui from the raw
        // event list, then hand the game an EMPTY list so NOTHING downstream sees
        // input: the game's controls, MenuControls (Screen Archer Menu's
        // scroll->FOV), and other mods' own input hooks (Wheeler's quick wheel)
        // all read from this one dispatch. Hooks the call to the input-event
        // dispatcher inside BSInputDeviceManager::PollInputDevices
        // (RELOCATION_ID(67315, 68617) + 0x7B) - the canonical Community Shaders
        // technique. Returning kStop from a peer input SINK cannot work: sinks
        // only see kStop from LATER sinks, and Wheeler isn't a sink at all - it
        // wraps this same call. Installed on the first Present frame so our
        // detour sits OUTERMOST of the other input hooks (they install at load),
        // emptying the list before theirs can read it.
        struct InputDispatchHook {
            static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
                              RE::InputEvent* const*               a_events) {
                auto& overlay = ImGuiOverlay::GetSingleton();
                if (overlay.IsOpen() && a_events && *a_events &&
                    Settings::GetSingleton().blockInputWhileOpen) {
                    overlay.HandleModalInput(a_events);  // feed ImGui + detect close
                    // Block downstream ONLY while the mouse is over the editor
                    // panel - that's where the list-scroll-to-SAM-FOV leak was.
                    // When the mouse is over the 3D character, fall through so the
                    // inventory/SAM can rotate the preview (ImGui was already fed).
                    if (overlay.WantsCaptureMouse()) {
                        RE::InputEvent* const empty[1] = { nullptr };
                        func(a_dispatcher, empty);
                        return;
                    }
                }
                // Passthrough: editor closed, block toggle off, or the mouse is
                // over the character (rotation). The editor's own input already
                // went to ImGui above; ControlMap still blocks game movement.
                func(a_dispatcher, a_events);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        void InstallInputBlockHook() {
            const REL::Relocation<std::uintptr_t> target{
                REL::RelocationID(67315, 68617), 0x7B
            };
            InputDispatchHook::func = SKSE::GetTrampoline().write_call<5>(
                target.address(), InputDispatchHook::thunk);
            spdlog::info("ImGuiOverlay: input-block hook installed (67315+0x7B).");
        }
    }

    ImGuiOverlay& ImGuiOverlay::GetSingleton() {
        static ImGuiOverlay instance;
        return instance;
    }

    void ImGuiOverlay::RequestClose() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                auto& overlay = ImGuiOverlay::GetSingleton();
                if (overlay.IsOpen()) {
                    overlay.Toggle();
                }
            });
        }
    }

    void ImGuiOverlay::RequestOpen() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                auto& overlay = ImGuiOverlay::GetSingleton();
                if (overlay.IsOpen()) {
                    return;  // already open
                }
                // Only from a valid UI context - the editor hides the menu
                // behind it; opening in the open world breaks its modal setup.
                auto*      ui          = RE::UI::GetSingleton();
                const bool inInventory = ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
                if (inInventory || SamCompat::IsMenuOpen()) {
                    overlay.Toggle();
                } else {
                    RE::DebugNotification(
                        "Open your inventory or Screen Archer Menu to edit outfits.");
                }
            });
        }
    }

    void ImGuiOverlay::HandleModalInput(RE::InputEvent* const* a_events) {
        // Replay/drop the previous frame's deferred keyboard keys FIRST, before
        // this frame's events queue new holds (OS-46 doubled-first-key fix).
        DrainDeferredKeyboard();
        const std::uint32_t editorKey = Settings::GetSingleton().editorKeyDIK;
        for (auto* e = *a_events; e; e = e->next) {
            FeedEvent(e);  // ImGui gets keyboard / mouse / wheel / gamepad
            const auto* btn = e->AsButtonEvent();
            if (!btn || !btn->IsDown() || WantsTextInput()) {
                continue;  // while typing, Esc/hotkey close waits (ImGui owns the key)
            }
            const auto device = btn->GetDevice();
            const auto code   = btn->GetIDCode();
            // Close on Esc, the editor hotkey, or gamepad Start (same set the
            // old EditorMenuGuard used - now that the guard's MenuControls path
            // is starved by the emptied event list, close lives here).
            const bool close =
                (device == RE::INPUT_DEVICE::kKeyboard && code == 0x01) ||  // Esc
                (device == RE::INPUT_DEVICE::kKeyboard && editorKey && code == editorKey) ||
                (device == RE::INPUT_DEVICE::kGamepad &&
                 code == RE::BSWin32GamepadDevice::Key::kStart);
            if (close) {
                RequestClose();
            }
        }
    }

    bool ImGuiOverlay::WantsTextInput() const {
        // Cross-thread read of a bool the render thread updates - a one-frame
        // stale value at focus transitions is acceptable.
        return initialized_ && open_ && ImGui::GetIO().WantTextInput;
    }

    bool ImGuiOverlay::WantsCaptureMouse() const {
        return initialized_ && open_ && ImGui::GetIO().WantCaptureMouse;
    }

    void ImGuiOverlay::RegisterMenuGuard() {
        if (auto* controls = RE::MenuControls::GetSingleton()) {
            controls->AddHandler(&g_menuGuard);
            spdlog::info("ImGuiOverlay: modal menu guard registered.");
        }
    }

    void ImGuiOverlay::EnsureInit(IDXGISwapChain* a_swapChain) {
        if (initialized_ || !a_swapChain) {
            return;
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        a_swapChain->GetDesc(&desc);
        hwnd_ = desc.OutputWindow;

        auto* rm = RE::BSRenderManager::GetSingleton();
        if (!rm) {
            spdlog::error("ImGuiOverlay: no BSRenderManager.");
            return;
        }
        auto& data = rm->GetRuntimeData();
        device_    = data.forwarder;  // the device member is named 'forwarder'
        context_   = data.context;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;  // XInput polled by the win32 backend
        io.IniFilename = nullptr;                         // no imgui.ini in the game folder

        // Fonts + palette before the backend bakes the atlas (first NewFrame).
        EditorStyle::InitFonts(Settings::GetSingleton().menuFontSize);
        EditorStyle::Apply();

        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_, context_);

        g_origWndProc = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));

        initialized_ = true;
        spdlog::info("ImGuiOverlay: initialized (hwnd={}).", static_cast<void*>(hwnd_));
    }

    HRESULT STDMETHODCALLTYPE ImGuiOverlay::PresentThunk(IDXGISwapChain* a_swap, UINT a_sync,
                                                         UINT a_flags) {
        // Install the input-block hook on the first rendered frame - strictly
        // after every plugin's load-time hooks, so we sit outermost of other
        // input hooks (Wheeler) and empty the event list before they read it.
        static std::once_flag s_inputHookOnce;
        std::call_once(s_inputHookOnce, [] { InstallInputBlockHook(); });

        auto& self = GetSingleton();
        if (self.initialized_ && self.open_) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();  // polls XInput for gamepad nav
            // The win32 backend polls ::GetCursorPos when no WM_MOUSEMOVE flows
            // - and with the mouse DirectInput-exclusive in EVERY context, none
            // ever does: the OS cursor is hidden/pinned and the poll would park
            // ImGui's cursor there (the reported vanishing cursor). Queue our
            // accumulated position AFTER the backend's - last event wins.
            ImGui::GetIO().AddMousePosEvent(self.mouseX_, self.mouseY_);
            ImGui::NewFrame();

            EditorUI::Draw();

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        return g_origPresent(a_swap, a_sync, a_flags);
    }

    LRESULT CALLBACK ImGuiOverlay::WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam,
                                           LPARAM a_lParam) {
        auto& self = GetSingleton();
        if (self.open_ && self.initialized_) {
            switch (a_msg) {  // WM keyboard is alive - it owns ImGui key input
                case WM_KEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYDOWN:
                case WM_SYSKEYUP:
                case WM_CHAR:
                    g_lastWmKeyTick.store(::GetTickCount(), std::memory_order_relaxed);
                    break;
                default:
                    break;
            }
            ImGui_ImplWin32_WndProcHandler(a_hwnd, a_msg, a_wParam, a_lParam);
            switch (a_msg) {  // swallow so the game never sees these
                case WM_INPUT:
                case WM_KEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYDOWN:
                case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_MOUSEMOVE:
                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL:
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONUP:
                    return 0;
                default:
                    break;
            }
        }
        return ::CallWindowProcW(g_origWndProc, a_hwnd, a_msg, a_wParam, a_lParam);
    }

    void ImGuiOverlay::FeedEvent(const RE::InputEvent* a_event) {
        if (!initialized_ || !open_ || !a_event) {
            return;
        }
        // Input model, established empirically across three sessions: the
        // MOUSE is DirectInput-EXCLUSIVE in every context - menus draw their
        // own cursor and WM_* mouse messages never reach the window, so the
        // engine events are the ONLY mouse source (clicks died when this fed
        // nothing in menus). The KEYBOARD is non-exclusive in menus: real
        // WM_KEYDOWN/WM_CHAR flow and the WndProc backend owns it there -
        // feeding it too doubled every keystroke.
        auto& io = ImGui::GetIO();

        if (a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
            usingGamepad_ = false;  // touched the mouse → hide the gamepad hint
            const auto* move = static_cast<const RE::MouseMoveEvent*>(
                a_event->AsIDEvent());
            if (move) {
                const float maxX = std::max(io.DisplaySize.x - 1.0f, 0.0f);
                const float maxY = std::max(io.DisplaySize.y - 1.0f, 0.0f);
                mouseX_ = std::clamp(mouseX_ + static_cast<float>(move->mouseInputX), 0.0f, maxX);
                mouseY_ = std::clamp(mouseY_ + static_cast<float>(move->mouseInputY), 0.0f, maxY);
                io.AddMousePosEvent(mouseX_, mouseY_);
            }
            return;
        }

        // Left thumbstick -> D-pad nav. ImGui item navigation uses the D-pad
        // keys (the analog LStick only moves windows), so translate the stick to
        // them. Edge-triggered: press when the stick crosses the deadzone,
        // release when it returns - a resting stick emits nothing and never
        // fights the real D-pad.
        if (a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
            if (const auto* ts = static_cast<const RE::ThumbstickEvent*>(a_event);
                ts && ts->IsLeft()) {
                constexpr float kDead = 0.5f;
                int             dir   = 0;
                if (ts->yValue > kDead) {
                    dir = 1;  // up
                } else if (ts->yValue < -kDead) {
                    dir = 2;  // down
                } else if (ts->xValue < -kDead) {
                    dir = 3;  // left
                } else if (ts->xValue > kDead) {
                    dir = 4;  // right
                }
                if (dir != 0) {
                    usingGamepad_ = true;  // stick deflected past the deadzone
                }
                if (dir != g_stickNavDir) {
                    const auto keyFor = [](int a_d) {
                        switch (a_d) {
                            case 1:  return ImGuiKey_GamepadDpadUp;
                            case 2:  return ImGuiKey_GamepadDpadDown;
                            case 3:  return ImGuiKey_GamepadDpadLeft;
                            case 4:  return ImGuiKey_GamepadDpadRight;
                            default: return ImGuiKey_None;
                        }
                    };
                    if (g_stickNavDir != 0) {
                        io.AddKeyEvent(keyFor(g_stickNavDir), false);
                    }
                    if (dir != 0) {
                        io.AddKeyEvent(keyFor(dir), true);
                    }
                    g_stickNavDir = dir;
                }
            }
            return;
        }

        const auto* btn = a_event->AsButtonEvent();
        if (!btn) {
            return;
        }
        // Feed TRANSITIONS only; held-repeat events would double-fire ImGui.
        const bool down = btn->IsDown();
        const bool up   = btn->IsUp();

        switch (btn->GetDevice()) {
            case RE::INPUT_DEVICE::kMouse: {
                usingGamepad_ = false;
                const auto id = btn->GetIDCode();
                if (id <= 4) {  // left, right, middle, x1, x2
                    if (down || up) {
                        io.AddMouseButtonEvent(static_cast<int>(id), down);
                    }
                } else if (id == 8 && down) {  // wheel up
                    io.AddMouseWheelEvent(0.0f, 1.0f);
                } else if (id == 9 && down) {  // wheel down
                    io.AddMouseWheelEvent(0.0f, -1.0f);
                }
                break;
            }
            case RE::INPUT_DEVICE::kKeyboard: {
                usingGamepad_ = false;
                if (!(down || up)) {
                    break;  // ignore held-repeat; only transitions matter
                }
                const bool wmOwns = WndProcOwnsKeyboard();
                if (Settings::GetSingleton().blockInputWhileOpen) {
                    // Default path: defer one frame (KeyboardArbiter) so the
                    // first key after an idle pause isn't fed by BOTH this feed
                    // and WndProc - DrainDeferredKeyboard drops it next frame if
                    // WM turns out to own the keyboard. Kills the doubled key.
                    g_keyArbiter.OnEngineKey(btn->GetIDCode(), down, wmOwns);
                    break;
                }
                // Safety-valve path (bBlockInputWhileOpen=0): the older
                // immediate feed, recency-gated. HandleModalInput does not run
                // in this mode, so there is no drain point to defer into.
                if (!wmOwns) {
                    FeedKeyToImGui(io, btn->GetIDCode(), down);
                }
                break;
            }
            case RE::INPUT_DEVICE::kGamepad: {
                // Feed gamepad buttons as ImGui nav keys. The win32 backend's
                // XInput poll never reached ImGui on this setup (controller nav
                // was dead), so the editor's own input feed supplies it - the
                // same reason the mouse is engine-fed.
                usingGamepad_   = true;
                using GKey      = RE::BSWin32GamepadDevice::Key;
                ImGuiKey navKey = ImGuiKey_None;
                switch (btn->GetIDCode()) {
                    case GKey::kUp:            navKey = ImGuiKey_GamepadDpadUp; break;
                    case GKey::kDown:          navKey = ImGuiKey_GamepadDpadDown; break;
                    case GKey::kLeft:          navKey = ImGuiKey_GamepadDpadLeft; break;
                    case GKey::kRight:         navKey = ImGuiKey_GamepadDpadRight; break;
                    case GKey::kA:             navKey = ImGuiKey_GamepadFaceDown; break;   // activate
                    case GKey::kB:             navKey = ImGuiKey_GamepadFaceRight; break;  // cancel / back
                    case GKey::kX:             navKey = ImGuiKey_GamepadFaceLeft; break;
                    case GKey::kY:             navKey = ImGuiKey_GamepadFaceUp; break;
                    case GKey::kLeftShoulder:  navKey = ImGuiKey_GamepadL1; break;
                    case GKey::kRightShoulder: navKey = ImGuiKey_GamepadR1; break;
                    default:                   break;
                }
                if (navKey != ImGuiKey_None && (down || up)) {
                    io.AddKeyEvent(navKey, down);
                }
                break;
            }
            default:
                break;
        }
    }

    void ImGuiOverlay::Toggle() {
        if (!initialized_) {
            auto* rm = RE::BSRenderManager::GetSingleton();
            if (!rm) {
                return;
            }
            auto& data = rm->GetRuntimeData();
            EnsureInit(data.swapChain);
            if (initialized_ && !g_origPresent) {
                PatchVtable<Present_t>(data.swapChain, 8, &PresentThunk, &g_origPresent);
                spdlog::info("ImGuiOverlay: Present hook installed (vtable[8]).");
            }
            if (!initialized_) {
                return;
            }
        }
        if (!open_) {
            // Don't open over a running scene (OStim etc.): the editor forces
            // third person, hides the menus and grabs input - it would hijack
            // the scene. (Transmog is already suspended by SceneGuard.)
            if (SceneGuard::Active()) {
                RE::DebugNotification("You can't edit outfits during a scene.");
                return;
            }
            // About to open: re-evaluate style fit if the player's race
            // changed (RaceMenu). Must finish before the Present thread can
            // draw rows - fitsBody is read unsynchronized.
            StyleCatalog::GetSingleton().EnsureFitCurrent();
        }
        open_ = !open_;
        open_ ? OnOpen() : OnClose();
    }

    void ImGuiOverlay::OnOpen() {
        EditorStyle::PlayUISound("UIMenuOK");
        // NPC-shaped-later seam (spec §6): the editor always acts on the PLAYER.
        // A future version that transmogs SAM's selected NPC would read
        // SAM.GetRefr() here and thread that refr into the render override -
        // which is player-only today, so no refr parameter is added now. This
        // comment is the single documented handoff point.
        // Determine the open context. SAM (a Scaleform menu) means the posed,
        // lit character is already framed - we must NOT force the camera.
        auto*      ui          = RE::UI::GetSingleton();
        const bool inInventory = ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
        openedFromSam_         = !inInventory && SamCompat::IsMenuOpen();
        // Opening from a menu (inventory OR SAM): the WM keyboard path is alive,
        // so start the engine feed silent (prevents a doubled first keystroke).
        if (inInventory || openedFromSam_) {
            g_lastWmKeyTick.store(::GetTickCount(), std::memory_order_relaxed);
        }
        auto& io = ImGui::GetIO();
        // Drop any stuck key state from the previous session: the closing
        // keystroke's RELEASE always lands after the editor stopped listening
        // (WndProc forwards only while open), so Esc/Ctrl/Shift could stay
        // logically held - a held Esc instantly deactivates every text field
        // the user clicks ("can't type no matter how many times I click").
        io.ClearInputKeys();
        g_keyArbiter.Clear();  // no stale held keys carried across an open (OS-46)
        io.MouseDrawCursor = true;
        // Seed the software cursor (every context - the OS cursor is never
        // valid); positions then accumulate from MouseMoveEvent deltas
        // (FeedEvent). On the very first open no NewFrame has run, so
        // DisplaySize may be zero - measure the window directly.
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        const float w = rc.right > 0 ? static_cast<float>(rc.right) : 1920.0f;
        const float h = rc.bottom > 0 ? static_cast<float>(rc.bottom) : 1080.0f;
        mouseX_ = w * 0.30f;  // over the left-column panel
        mouseY_ = h * 0.35f;
        io.AddMousePosEvent(mouseX_, mouseY_);

        // Ask Apparel Preview (when present) to clear any hover preview, so
        // the editor always starts from the TRUE look (outfit/real gear), not
        // a stale preview latched under the now-hidden inventory. Contract:
        // message type 'CLRP', no payload, receiver plugin "ApparelPreview".
        if (auto* messaging = SKSE::GetMessagingInterface()) {
            messaging->Dispatch(kClearPreviewMsg, nullptr, 0, "ApparelPreview");
        }
        // In SAM context the screenarcher has set up the camera/pose - leave it.
        if (!openedFromSam_) {
            if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                wasFirstPerson_ = cam->IsInFirstPerson();
                cam->ForceThirdPerson();
            }
        }
        if (auto* cm = RE::ControlMap::GetSingleton()) {
            cm->ToggleControls(BlockedControls(), false);
            cm->ignoreKeyboardMouse = true;
        }
        // Hide the game's UI (inventory, HUD, game cursor) so only the world -
        // and the Show-Player-In-Menus character - remains behind the editor.
        if (auto* ui = RE::UI::GetSingleton()) {
            wasShowingMenus_ = ui->IsShowingMenus();
            ui->ShowMenus(false);
        }
        EditorUI::OnOpen();
    }

    void ImGuiOverlay::OnClose() {
        EditorStyle::PlayUISound("UIMenuCancel");
        ImGui::GetIO().ClearInputKeys();  // symmetric with OnOpen
        g_keyArbiter.Clear();
        EditorUI::OnClose();
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->ShowMenus(wasShowingMenus_);
        }
        ImGui::GetIO().MouseDrawCursor = false;
        if (auto* cm = RE::ControlMap::GetSingleton()) {
            cm->ToggleControls(BlockedControls(), true);
            cm->ignoreKeyboardMouse = false;
        }
        // We only forced third person outside SAM context, so only restore there.
        if (auto* cam = RE::PlayerCamera::GetSingleton();
            cam && wasFirstPerson_ && !openedFromSam_) {
            cam->ForceFirstPerson();
        }
    }

}  // namespace OS
