#include "InputListener.h"

#include "EditorGate.h"
#include "EditorWindow.h"
#include "ImGuiOverlay.h"
#include "LoreModule.h"
#include "OutfitSession.h"
#include "SamCompat.h"
#include "Settings.h"

namespace OS {

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
        session.WithLibrary([&](OutfitLibrary& lib) {
            const auto count = lib.Count();
            if (count == 0) {
                return;
            }
            const auto next = static_cast<std::size_t>(
                (lib.ActiveIndex() + 1) % static_cast<int>(count));
            lib.Activate(next);
            if (const auto* o = lib.At(next)) {
                name = o->name;
            }
        });
        if (!name.empty()) {
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
