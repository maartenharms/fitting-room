#pragma once
#include "PCH.h"

namespace OS {

    // Injects an "Outfits" entry (with the bound editor key's glyph) into the
    // InventoryMenu's bottom button bar - the Compare Equipment NG pattern the
    // user asked to match ("V Compare"). Uses the SkyUI-family ButtonPanel AS2
    // API on the live menu movie; Vel'dun/Edge-style skins restyle that panel,
    // so the entry inherits the load order's look automatically. Purely a
    // discoverability hint: the key itself is handled by InputListener, and
    // non-SkyUI UIs just skip the injection with a log line.
    class MenuButton : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuButton& GetSingleton();
        static void        Register();  // kDataLoaded

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

    private:
        MenuButton() = default;

        static void InjectButton();

        bool loggedMissingPanel_{ false };
    };

}  // namespace OS
