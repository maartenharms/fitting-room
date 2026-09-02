#include "HostGuard.h"

#include "CameraFrame.h"
#include "EditorWindow.h"
#include "SamCompat.h"
#include "Settings.h"

#include <string_view>

namespace OS::HostGuard {

    // ⚠ THE LITERALS IN THE HEADER, PROVED AGAINST THE ENGINE'S OWN CONSTANTS.
    // The header cannot include RE or the pure classifier stops being testable,
    // so the spelling lives there and the proof lives here. A typo would
    // otherwise be a gate that matches nothing, refuses every open, and logs
    // "neither the inventory nor Screen Archer Menu is open" while the player is
    // standing in the menu it names.
    static_assert(kInventoryMenu == RE::InventoryMenu::MENU_NAME);
    static_assert(kMagicMenu == RE::MagicMenu::MENU_NAME);

    namespace {

        void Handle(const RE::MenuOpenCloseEvent* a_event) {
            if (!a_event || a_event->opening) {
                return;  // only closes matter, and this runs on every transition
            }

            if (OS::EditorWindow::IsOpen()) {
                // Read the CURRENT stack rather than the closing menu's name.
                // The editor can be hosted by any of the three, and a SAM-hosted
                // editor must not be closed because the inventory went away.
                if (!HostMenuOpen()) {
                    spdlog::info("HostGuard: the editor's host menu closed underneath it "
                                 "- closing the editor so the camera and the 2D chrome "
                                 "are handed back.");
                    OS::EditorWindow::RequestClose();
                }
                return;
            }

            // The backstop, and the only part that covers the editor ceasing to
            // draw without SetOpen(false) ever running. Costs one bool on a menu
            // transition and writes nothing unless the camera is genuinely held.
            if (OS::CameraFrame::Held()) {
                spdlog::warn("HostGuard: the editor is closed but the camera is still "
                             "held - releasing it. If this line appears, an exit path "
                             "skipped its own release and wants finding.");
                OS::CameraFrame::Release();
            }
        }

        struct Sink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*               a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                // BSTEventSink contract: never throw into the engine.
                try {
                    Handle(a_event);
                } catch (const std::exception& e) {
                    spdlog::error("HostGuard threw: {}", e.what());
                } catch (...) {
                    spdlog::error("HostGuard threw a non-standard exception.");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        Sink g_sink;

    }  // namespace

    Host CurrentHost() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return {};
        }
        // ⚠ SamCompat::IsMenuOpen READS THE CONFIGURED NAME, so the name that
        // answered the question is the name handed back. Re-deriving it at the
        // close instead is what would let a settings edit between open and close
        // restore the alpha on a menu nobody hid.
        // ⚠ THE GRID NAME IS A LITERAL FROM THE HEADER, not an RE constant: it
        // is another author's menu and there is nothing to prove it against.
        // IsMenuOpen takes any registered name, so a plugin that is not
        // installed simply answers false and costs one map lookup.
        return ClassifyHost(ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME),
                            ui->IsMenuOpen(kGridInventoryMenu),
                            ui->IsMenuOpen(RE::MagicMenu::MENU_NAME),
                            OS::SamCompat::IsMenuOpen(),
                            OS::Settings::GetSingleton().samMenuName);
    }

    bool HostMenuOpen() { return CurrentHost().Present(); }

    void Register() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
            spdlog::info("HostGuard: registered (close the editor when its host menu "
                         "goes away; release the camera if it is ever held while the "
                         "editor is not open).");
        }
    }

    void OnPreLoadGame() {
        // Order matters. Release first: RequestClose marshals onto the main
        // thread and may not run before the load tears the session down, and a
        // camera left held across a load is the one failure that survives into
        // gameplay.
        OS::CameraFrame::Release();
        if (OS::EditorWindow::IsOpen()) {
            spdlog::info("HostGuard: game loading with the editor open - camera "
                         "released and the editor asked to close.");
            OS::EditorWindow::RequestClose();
        }
    }

}  // namespace OS::HostGuard
