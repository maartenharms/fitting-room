#include "MenuStudioCompat.h"

#include "BuildChannel.h"  // DataPath, so the hood resolves beside icons.ttf
#include "DyeUnlocks.h"    // CurrentCharge, for the tile's charge meter
#include "EditorWindow.h"
#include "LoreModule.h"
#include "MenuStudioApi.h"
#include "Settings.h"      // costMode / seamstoneCapacity, the meter's two gates

#include <Windows.h>

namespace OS::MenuStudioCompat {

    namespace {
        // Menu Studio's export, resolved at runtime. The signature is MS's
        // PublicApi.cpp contract: id, label, icon path (empty = lettered
        // tile), and a plain callback invoked from the FLICK render pass.
        using RegisterAction_t = bool (*)(const char* a_id, const char* a_label,
                                          const char* a_iconPath,
                                          void (*a_onClick)());

        // Which bubbled menus the button belongs in, comma separated. Added to
        // Menu Studio after the action bar shipped, so a build without it
        // resolves nothing and the button appears everywhere, exactly as it
        // does today.
        using SetActionMenus_t = bool (*)(const char* a_id, const char* a_menus);

        // The click lands on the FLICK render thread. RequestOpen/RequestClose
        // both marshal through the SKSE task queue, which is exactly why they
        // are the entry points here rather than the window itself - same rule
        // the hotkey path follows.
        void OnClick() {
            if (EditorWindow::IsOpen()) {
                // Deliberate exit, so unpaid work gets asked about.
                EditorWindow::RequestCloseAsking();
            } else {
                // Gated inside, against HostGuard's one list, and silent when it
                // refuses. The scope below keeps the tile off the menus that
                // would refuse, so a click that gets this far normally opens.
                EditorWindow::RequestOpen();
            }
        }

        // ⚠ ON MENU OPEN RATHER THAN ON INVENTORY CHANGE, and the choice is
        // about being RIGHT rather than about being cheap. The stone can arrive
        // or leave by a dozen routes - looted, bought, sold, stolen, scripted -
        // and the one container event this plugin already listens to only sees
        // items ENTERING the player, so a sold stone would leave the tile up.
        // The strip is only ever drawn over a menu, so asking as a menu opens is
        // both always current and impossible to miss.
        //
        // Not filtered to InventoryMenu by name on purpose: the scope MS is
        // given can change, and asking on every open costs one bool.
        struct Sink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*               a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                // BSTEventSink contract: never throw into the engine.
                try {
                    if (a_event && a_event->opening) {
                        RefreshVisibility();
                    }
                } catch (const std::exception& e) {
                    spdlog::error("MenuStudioCompat sink threw: {}", e.what());
                } catch (...) {
                    spdlog::error("MenuStudioCompat sink threw a non-standard exception.");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        // How full the Seamstone is, drawn as the tile's own background fill.
        //
        // ⚠⚠ ONLY WHILE CHARGE IS THE CURRENCY, and that gate is the whole design.
        // Under iCostMode 0 or 1 the stone's charge buys nothing, so a meter there
        // would be a number the player has no reason to care about sitting on the
        // one button that is supposed to be self-explanatory. Negative clears it,
        // which is why the mode switch does not need its own teardown.
        //
        // ⚠ CurrentCharge, NOT Snapshot. The unlock set is several hundred strings
        // behind the same lock and this runs on every menu edge; DyeUnlocks.h calls
        // that out by name as the reason CurrentCharge exists.
        //
        // ⚠ THE CAP COMES FROM THE SETTING, and a zero cap means the feature is off
        // rather than that the stone is full. Dividing by it would be a divide by
        // zero on a live INI that has iSeamstoneCapacity = 0, which is a supported
        // way to switch the economy off.
        void PublishCharge() {
            const auto& cfg = Settings::GetSingleton();
            if (cfg.costMode != OS::CostMode::kCharge || cfg.seamstoneCapacity == 0) {
                MenuStudioApi::SetActionMeter("FittingRoom.Open", -1.0f);
                return;
            }
            const float held = static_cast<float>(DyeUnlocks::CurrentCharge());
            const float cap  = static_cast<float>(cfg.seamstoneCapacity);
            MenuStudioApi::SetActionMeter("FittingRoom.Open", held / cap);
        }

        Sink g_sink;
    }

    void Register() {
        const HMODULE ms = ::GetModuleHandleW(L"MenuStudio.dll");
        if (!ms) {
            spdlog::debug("MenuStudioCompat: Menu Studio not loaded - no bar button.");
            return;
        }
        const auto reg = reinterpret_cast<RegisterAction_t>(
            ::GetProcAddress(ms, "MenuStudio_RegisterAction"));
        if (!reg) {
            spdlog::info("MenuStudioCompat: Menu Studio is loaded but has no action "
                         "bar (older build) - no button.");
            return;
        }
        // ⚠⚠ A PICTURE, NOT A GLYPH, AND THIS IS THE ONE TILE THAT HAS TO BE.
        // It used to send Icons::kBody, a t-shirt, drawn by MENU STUDIO out of
        // FUCK's baked fa-solid atlas. Two things are wrong with that.
        //
        // The first is correctness. Icons.h runs a runtime audit that measures
        // a known-absent codepoint and drops every glyph to letters on a host
        // whose atlas is missing one, and that audit CANNOT reach a glyph
        // another mod renders. So this was the single place in the project
        // where an unproven codepoint reaches a player as "?" with nothing able
        // to catch it (a-glyph-atlas-you-do-not-ship-cannot-be-verified-
        // offline). A PNG cannot be tofu.
        //
        // The second is the subject, and it has taken several goes. A t-shirt
        // is not what this mod is about, so it became a hood; the hood came
        // back the same day it shipped ("i don't like the hood icon", user
        // 2026-08-14, with a screenshot) because at the 28px the strip draws it
        // read as a hooded figure rather than as clothes. A Seamstone was tried
        // next, on the reasoning that the tile should show the object that
        // opens the editor; the user's call was that it should be a garment,
        // which is the more direct statement of what the mod does.
        //
        // ⚠ IT IS A ROBE, AND IT IS SOURCED RATHER THAN DRAWN, which is the
        // other half of why the hood missed. lorc/robe from game-icons.net,
        // the same set and pipeline as the nineteen row icons.
        //
        // ⚠⚠ IT HAS A HOOD ON IT, AND THAT WAS FLAGGED AND CHOSEN ANYWAY. The
        // risk is precisely the one the last icon died of: at 28px a hooded
        // garment can start to read as a hooded figure. The difference is that
        // this one hangs as a robe with sleeves and a hem rather than being a
        // head and shoulders, so the silhouette is a garment. If the field
        // reports it reading as a figure again, the fix is a garment with no
        // hood at all (delapouite/clothes survives 28px best of the six that
        // exist) rather than another pass at drawing this one.
        //
        // MS reads a dot or a slash as a path, so this takes its image route
        // and the lettered "FR" fallback still covers a file that fails to
        // load. Same Data-relative shape IconImages::Load uses, so it resolves
        // wherever the mod manager staged us.
        //
        // ⚠ A NEW FILENAME NEEDS AN F5 IN MO2 before it can be judged at all:
        // a new path in a directory the manager has already cached falls back
        // to the lettered "FR", which looks like the tile ignoring us.
        const std::string icon =
            (BuildChannel::DataPath("icons") / "robe.png").string();
        if (!reg("FittingRoom.Open", "Fitting Room", icon.c_str(), &OnClick)) {
            spdlog::warn("MenuStudioCompat: Menu Studio refused the bar button.");
            return;
        }
        spdlog::info("MenuStudioCompat: 'Fitting Room' registered on Menu "
                     "Studio's action bar.");

        // ⚠⚠ THE MENUS HostGuard ALLOWS, AND THE TWO LISTS ARE ONE DECISION
        // WEARING TWO HATS. The strip also draws over barter and container,
        // which this editor cannot be hosted by, and a click there only ever
        // earned a silent refusal in the log (see OnClick). Naming the menus is
        // how the button stops being offered where it cannot work.
        //
        // ⚠ SO THIS STRING MOVES WITH HostGuard::ClassifyHost. It cannot be
        // built from it: Menu Studio takes the names as text over a C ABI, and
        // the SAM half has no place here at all. A menu added to the gate and
        // missed here is a working entry point nobody can see; a menu added here
        // and missed in the gate is a tile that refuses every press.
        //
        // SAM is unaffected: it is not one of the menus Menu Studio bubbles, so
        // the strip was never on screen for the SAM entry point to begin with.
        //
        // An older Menu Studio has no such export. The button then keeps
        // appearing in all four, which is the behaviour that build already had.
        const auto scope = reinterpret_cast<SetActionMenus_t>(
            ::GetProcAddress(ms, "MenuStudio_SetActionMenus"));
        if (!scope) {
            spdlog::info("MenuStudioCompat: this Menu Studio cannot scope a button to "
                         "a menu (older build) - the button shows in every menu it "
                         "draws over.");
            return;
        }
        // ⚠ THE SAME LIST HostGuard ALLOWS, MINUS SAM. Grid Inventory is in it
        // because Menu Studio draws its action bar there too once its own
        // sMenus carries the name, and a button scoped to two menus would be
        // missing from the third for no reason a player could work out.
        if (!scope("FittingRoom.Open", "InventoryMenu,GridInventoryMenu,MagicMenu")) {
            spdlog::warn("MenuStudioCompat: Menu Studio refused the menu scope - the "
                         "button shows in every menu it draws over.");
        }
        // Born correct rather than born visible: a save loaded straight into
        // lore mode without the stone must not flash the tile on its first
        // inventory. This runs at kDataLoaded, before any save exists, so it can
        // only ever read "no stone" - which is the safe direction. The sink
        // below is what makes it right from then on.
        RefreshVisibility();
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
            spdlog::info("MenuStudioCompat: watching menu opens to keep the button's "
                         "visibility matched to the Seamstone gate.");
        }
    }

    void RefreshVisibility() {
        // ⚠ THE TILE IS THE LAST THING THAT ANNOUNCED THE MOD. Every path that
        // OPENS the editor now refuses silently without the Seamstone, but a
        // shirt icon sitting on the strip still tells a player there is
        // something here and that they cannot have it, which is the same
        // disclosure one step earlier. Hidden entirely, so lore mode looks like
        // no mod at all until the stone is found (user 2026-08-07).
        //
        // ⚠ NOT DECIDED ONCE AT REGISTRATION. The stone is an inventory item, so
        // the answer changes the moment one is picked up or sold, which is why
        // this is a function called on those edges rather than a branch around
        // Register.
        const bool show = LoreModule::GateSatisfied();
        MenuStudioApi::SetActionVisible("FittingRoom.Open", show);
        PublishCharge();
    }

    void RefreshCharge() {
        // ⚠⚠ THE METER USED TO MOVE ONLY ON A MENU EDGE, and that was the bug.
        // PublishCharge was reachable from RefreshVisibility alone, which runs
        // when a menu opens, so refilling the stone inside the editor left the
        // tile showing the old level until something opened a menu. Closing and
        // reopening the editor fixed it, and so did opening the console, which
        // is what made it look arbitrary: the console is a menu open like any
        // other. Reported 2026-08-29.
        //
        // ⚠ CALL THIS FROM EVERY SITE THAT MOVES THE CHARGE. There are three:
        // the Refill button, the styling spend, and the character editor's door
        // fee. A fourth has to call it too, because nothing here can notice a
        // write it was not told about.
        PublishCharge();
    }


}  // namespace OS::MenuStudioCompat
