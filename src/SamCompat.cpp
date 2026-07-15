#include "SamCompat.h"

#include "EditorWindow.h"

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

    void Register() {
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            source->AddEventSink(&g_sink);
            spdlog::info("SamCompat: mod-event bridge active "
                         "(OutfitSlots_Open / _Close / _Toggle).");
        }
        if (auto* papyrus = SKSE::GetPapyrusInterface()) {
            papyrus->Register(RegisterPapyrusFuncs);
        }
    }

}  // namespace OS::SamCompat
