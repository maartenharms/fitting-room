#include "SamCompat.h"

#include "EditorGate.h"
#include "EditorWindow.h"
#include "MenuHandlerVtable.h"

#include <string_view>

namespace OS::SamCompat {

    namespace {
        // The mod-event integration API. A mod fires one of these via
        // SendModEvent (Papyrus) or SKSE::ModCallbackEvent (C++) to drive the
        // editor. This is how a Screen Archer Menu addon opens Outfit Slots: a
        // menu entry's `global:` action calls a tiny Papyrus function that
        // SendModEvent("OutfitSlots_Open").
        struct EventSink : RE::BSTEventSink<SKSE::ModCallbackEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const SKSE::ModCallbackEvent*                 a_event,
                RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const std::string_view name{ a_event->eventName.c_str()
                                                 ? a_event->eventName.c_str()
                                                 : "" };
                if (name == "OutfitSlots_Open") {
                    EditorWindow::RequestOpen();
                } else if (name == "OutfitSlots_Close") {
                    EditorWindow::RequestClose();
                } else if (name == "OutfitSlots_Toggle") {
                    EditorWindow::IsOpen() ? EditorWindow::RequestClose()
                                           : EditorWindow::RequestOpen();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        EventSink g_sink;

        // ⚠ TAKES ESCAPE BACK FROM SAM WHILE THE EDITOR IS UP OVER IT.
        //
        // The hosted window sets kPassInputToGame whenever the cursor is off
        // the panel, so SAM keeps receiving drag, wheel and stick as one
        // gesture. That flag is per window and all or nothing, so the keyboard
        // rode along with the mouse: Escape reached SAM, SAM closed, MenuSink
        // below then closed the editor, and one press took down both menus when
        // the user expected to be handed back to SAM (field 2026-08-07).
        //
        // ⚠ A MenuEventHandler AND NOT THE InputListener SINK. That sink is the
        // raw device feed, upstream of the control map, so it can observe a key
        // but cannot stop anything downstream from also seeing it. Consuming is
        // what this needs, and returning true from ProcessButton is the only
        // place that does it. Registered at the front of MenuControls, so it
        // answers before SAM's own menu does.
        //
        // ⚠ IT DECLINES EVERYTHING ELSE, DELIBERATELY. ImGuiOverlay's
        // EditorMenuGuard consumes every event while it is open, because that
        // path is modal over a hidden inventory. Doing the same here would
        // swallow the camera gesture this whole passthrough exists to deliver.
        // One key, and only under one condition.
        struct EscapeGuard : OS::MenuHandlerVtable::Impl {
            [[nodiscard]] static bool Claims(RE::InputEvent* a_event) {
                const auto* btn = a_event ? a_event->AsButtonEvent() : nullptr;
                if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard ||
                    btn->GetIDCode() != 0x01) {
                    return false;
                }
                return EditorGate::ShouldConsumeHostEscape(
                    EditorWindow::IsOpen(), EditorWindow::OpenedFromSam(),
                    EditorWindow::WantsTextInput());
            }

            bool CanProcess(RE::InputEvent* a_event) override { return Claims(a_event); }

            bool ProcessButton(RE::ButtonEvent* a_event) override {
                if (!Claims(a_event)) {
                    return false;
                }
                // ⚠ THE UP IS CONSUMED TOO, AND THAT IS THE POINT OF NOT
                // TESTING IsDown IN Claims. Closing on the down while letting
                // the release through hands SAM half a keystroke, and a menu
                // that sees an Escape release it never saw pressed is a state
                // nobody tests for.
                if (a_event->IsDown()) {
                    EditorWindow::RequestClose();
                }
                return true;
            }

            bool ProcessMouseMove(RE::MouseMoveEvent*) override { return false; }
            bool ProcessThumbstick(RE::ThumbstickEvent*) override { return false; }
            bool ProcessKinect(RE::KinectEvent*) override { return false; }
        };
        EscapeGuard g_escapeGuard;

        // If SAM closes for any reason, close the editor from the same UI event
        // instead of leaving its hosted window stranded over regular gameplay.
        //
        // ⚠ STILL THE SAFETY NET, NO LONGER THE ESCAPE PATH. SAM's own close
        // button, another mod, or a menu stack change all still reach here.
        // What no longer reaches here is our own Escape, because EscapeGuard
        // above takes it first.
        struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                const auto& menuName = Settings::GetSingleton().samMenuName;
                if (!a_event || menuName.empty() ||
                    std::string_view{ a_event->menuName.c_str() } != menuName) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (EditorGate::ShouldCloseForLostHost(
                        EditorWindow::OpenedFromSam(), a_event->opening)) {
                    EditorWindow::RequestClose();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;

        // Native global Papyrus function OutfitSlotsSAM.OpenEditor() - the SAM
        // addon's menu entry calls this via its `global:` action (SAM's
        // CallGlobalFunction dispatches to it). Ships as the dependency-free
        // OutfitSlotsSAM.pex; the implementation lives here.
        void PapyrusOpenEditor(RE::StaticFunctionTag*) {
            EditorWindow::RequestOpen();
        }

        bool RegisterPapyrusFuncs(RE::BSScript::IVirtualMachine* a_vm) {
            a_vm->RegisterFunction("OpenEditor", "OutfitSlotsSAM", PapyrusOpenEditor);
            spdlog::info("SamCompat: registered Papyrus OutfitSlotsSAM.OpenEditor.");
            return true;
        }
    }

    void ArmEscapeGuard() {
        auto* controls = RE::MenuControls::GetSingleton();
        if (!controls) {
            return;
        }
        // ⚠ REMOVE THEN ADD, AND THE ORDER IS THE POINT. MenuControls inserts a
        // new handler at the FRONT of its list, so whoever registered last is
        // asked first. Ours went in at kDataLoaded and SAM's goes in when its
        // menu opens, which put SAM in front of us for the whole session: SAM
        // took the Escape, closed, and the lost-host sink took the editor down
        // with it, which is exactly the symptom that was supposed to be fixed.
        // Re-arming on every editor open puts us back at the front of whatever
        // has registered since. Removing first is what stops the list growing a
        // copy of us per open.
        static bool s_layoutLogged = false;
        const char* const layout = OS::MenuHandlerVtable::Install(&g_escapeGuard);
        if (!s_layoutLogged) {
            s_layoutLogged = true;
            spdlog::info("SamCompat: Escape guard vtable: {}.", layout);
        }
        controls->RemoveHandler(&g_escapeGuard);
        controls->AddHandler(&g_escapeGuard);
        spdlog::debug("SamCompat: Escape guard armed at the front of MenuControls.");
    }

    void Register() {
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            source->AddEventSink(&g_sink);
            spdlog::info("SamCompat: mod-event bridge active "
                         "(OutfitSlots_Open / _Close / _Toggle).");
        }
        if (auto* papyrus = SKSE::GetPapyrusInterface()) {
            papyrus->Register(RegisterPapyrusFuncs);
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
        }
        ArmEscapeGuard();
    }

}  // namespace OS::SamCompat
