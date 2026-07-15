#include "MenuButton.h"

#include "Settings.h"

namespace OS {

    namespace {
        // Known homes of the SkyUI-family bottom button bar inside the
        // InventoryMenu movie, most common first. Vel'dun and other Edge-style
        // skins keep SkyUI's structure and only reskin the assets.
        constexpr const char* kPanelPaths[] = {
            "_root.Menu_mc.navPanel",
            "_root.Menu_mc.bottomBar",
            "_root.Menu_mc.bottomBarInfo",
        };
    }

    MenuButton& MenuButton::GetSingleton() {
        static MenuButton instance;
        return instance;
    }

    void MenuButton::Register() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&GetSingleton());
            spdlog::info("MenuButton: inventory watcher registered.");
        }
    }

    void MenuButton::InjectButton() {
        const auto key = Settings::GetSingleton().editorKeyDIK;
        auto*      ui  = RE::UI::GetSingleton();
        if (!ui || !key) {
            return;
        }
        const auto menu = ui->GetMenu(RE::InventoryMenu::MENU_NAME);
        if (!menu || !menu->uiMovie) {
            return;
        }
        auto& movie = *menu->uiMovie;

        RE::GFxValue panel;
        const char*  found = nullptr;
        for (const char* path : kPanelPaths) {
            if (movie.GetVariable(&panel, path) && panel.IsObject()) {
                found = path;
                break;
            }
        }
        auto& self = GetSingleton();
        if (!found) {
            if (!self.loggedMissingPanel_) {
                self.loggedMissingPanel_ = true;
                spdlog::warn("MenuButton: no SkyUI-family button panel in the InventoryMenu "
                             "movie - the Outfits hint is skipped on this UI.");
            }
            return;
        }

        RE::GFxValue button;
        movie.CreateObject(&button);
        button.SetMember("text", "Outfits");
        RE::GFxValue controls;
        movie.CreateObject(&controls);
        controls.SetMember("keyCode", static_cast<double>(key));
        button.SetMember("controls", controls);

        RE::GFxValue ret;
        if (!panel.Invoke("addButton", &ret, &button, 1)) {
            if (!self.loggedMissingPanel_) {
                self.loggedMissingPanel_ = true;
                spdlog::warn("MenuButton: {}.addButton not invocable - hint skipped.", found);
            }
            return;
        }
        RE::GFxValue immediate(true);
        panel.Invoke("updateButtons", nullptr, &immediate, 1);
        spdlog::debug("MenuButton: 'Outfits' hint added via {}.", found);
    }

    RE::BSEventNotifyControl MenuButton::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
        if (!a_event || !a_event->opening ||
            a_event->menuName != RE::InventoryMenu::MENU_NAME) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // The movie exists when the open event fires, but SkyUI finishes its
        // own bottom-bar setup in the same breath - inject from the task queue
        // so we land after it, not under it. Re-done every open: each open
        // builds a fresh movie.
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { InjectButton(); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace OS
