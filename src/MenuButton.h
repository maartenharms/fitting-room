#pragma once
#include "PCH.h"

#include <set>
#include <string>

namespace OS {

    // Injects an "Outfits" entry (with the bound editor key's glyph) into the
    // bottom button bar of each vanilla menu the editor can be hosted by - the
    // Compare Equipment NG pattern the user asked to match ("V Compare"). Uses
    // the SkyUI-family ButtonPanel AS2 API on the live menu movie;
    // Vel'dun/Edge-style skins restyle that panel, so the entry inherits the
    // load order's look automatically. Purely a discoverability hint: the key
    // itself is handled by InputListener, and non-SkyUI UIs just skip the
    // injection with a log line.
    //
    // ⚠ THE MENU LIST IS HostGuard'S, read at the event rather than copied.
    // A hint offered where the key is refused is worse than no hint, and a
    // menu the key works in with no hint is the discoverability problem this
    // class exists for.
    class MenuButton : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuButton& GetSingleton();
        static void        Register();  // kDataLoaded

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

    private:
        MenuButton() = default;

        static void InjectButton(const std::string& a_menu);

        // ⚠ PER MENU, NOT ONE FLAG. A UI that carries SkyUI's panel in the
        // inventory and not in the magic menu is ordinary, and a single latch
        // would report whichever came first and then go quiet about the other -
        // which reads as "the inventory is fine" when the missing one is the
        // menu being asked about.
        std::set<std::string> loggedMissingPanel_;
    };

}  // namespace OS
