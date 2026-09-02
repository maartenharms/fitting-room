#include "MenuButton.h"

#include "HostGuard.h"  // the menus the editor can be hosted by, and so hinted in
#include "Settings.h"

namespace OS {

    namespace {
        // Known homes of the SkyUI-family bottom button bar inside a menu movie,
        // most common first. Vel'dun and other Edge-style skins keep SkyUI's
        // structure and only reskin the assets, and SkyUI builds the magic menu
        // on the same ItemMenu base as the inventory, so one list covers both.
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
            spdlog::info("MenuButton: watching {} and {} for the Outfits hint.",
                         HostGuard::kInventoryMenu, HostGuard::kMagicMenu);
        }
    }

    void MenuButton::InjectButton(const std::string& a_menu) {
        const auto key = Settings::GetSingleton().editorKeyDIK;
        auto*      ui  = RE::UI::GetSingleton();
        if (!ui || !key) {
            return;
        }
        const auto menu = ui->GetMenu(a_menu);
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
            if (self.loggedMissingPanel_.insert(a_menu).second) {
                spdlog::warn("MenuButton: no SkyUI-family button panel in the {} movie - "
                             "the Outfits hint is skipped there on this UI.", a_menu);
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
            if (self.loggedMissingPanel_.insert(a_menu).second) {
                spdlog::warn("MenuButton: {}.addButton not invocable in {} - hint skipped.",
                             found, a_menu);
            }
            return;
        }
        RE::GFxValue immediate(true);
        panel.Invoke("updateButtons", nullptr, &immediate, 1);
        spdlog::debug("MenuButton: 'Outfits' hint added to {} via {}.", a_menu, found);
    }

    RE::BSEventNotifyControl MenuButton::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
        if (!a_event || !a_event->opening) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // ⚠ THE VANILLA HOSTS ONLY, AND THE NAME COMES FROM HostGuard. SAM is
        // deliberately absent: it is not a SkyUI menu and has no button panel to
        // add to, and its own addon already carries the entry point.
        const std::string_view name{ a_event->menuName.c_str() };
        if (name != HostGuard::kInventoryMenu && name != HostGuard::kMagicMenu) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // The movie exists when the open event fires, but SkyUI finishes its
        // own bottom-bar setup in the same breath - inject from the task queue
        // so we land after it, not under it. Re-done every open: each open
        // builds a fresh movie.
        //
        // The name is COPIED into the task rather than captured by view: the
        // event is gone by the time the task runs.
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([menu = std::string(name)] { InjectButton(menu); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace OS
