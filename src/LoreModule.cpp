#include "LoreModule.h"

#include "EditorWindow.h"
#include "Settings.h"

namespace OS::LoreModule {

    namespace {
        constexpr const char* kEsp         = "FittingRoomLore.esp";
        constexpr RE::FormID  kNoteID      = 0x801;  // BOOK "On the Outward Art"
        constexpr RE::FormID  kSeamstoneID = 0x805;  // MISC "The Seamstone"
        // Retired ids - never reuse; saves may reference them: 0x800
        // (Dressing Stand ACTI), 0x802 (its REFR), 0x803 (ARMO Seamstone:
        // slot-61 armor is hidden from every item list), 0x804 (ALCH
        // Seamstone: a zero-effect ingestible CTDs ItemCardPopulate).

        // Skyrim.esm Farengar. Stocking is DLL-side (no vendor/cell override
        // in the ESP) and self-healing: the vendor faction is resolved LIVE
        // from his NPC record (runtime patchers can swap it), tradability is
        // checked against the live VEND list (adopting one of its keywords
        // when ours misses), and every add is read back - a merchant chest
        // lives in a never-loaded cell, and an add that silently fails there
        // falls back to Farengar himself, re-tried when his ref attaches.
        constexpr RE::FormID kFarengarFactionID = 0x000ABB43;  // fallback only
        constexpr RE::FormID kFarengarNPCID     = 0x00013BBB;  // FarengarSecretFire
        constexpr RE::FormID kFarengarRefID     = 0x0001A67E;  // his placed ACHR

        RE::TESObjectMISC* g_seamstone = nullptr;
        RE::TESObjectBOOK* g_note      = nullptr;

        // Set when a WORLD use of the Seamstone (e.g. slotted in a Wheeler wheel)
        // asks us to open the inventory so the editor can follow (OS-38). The
        // MenuSink consumes it on the next InventoryMenu open. Guards against a
        // duplicate open request if the equip event fires twice.
        bool g_openEditorOnInventory = false;

        // Opens the editor when the Seamstone is "used". A plain MISC has no
        // native use action, but misc-interaction mods route use through the
        // equip pipeline (ActorEquipManager::EquipObject), which lands here as
        // TESEquipEvent on the main thread. Wheeler (Nexus 97345) is exactly
        // such a mod: a Seamstone slotted in its wheel activates via EquipObject,
        // firing this sink from the WORLD (no menu open).
        //
        // The editor needs the inventory context (the modal + SPIM character
        // composition assume InventoryMenu is open - same gate as the hotkey).
        // So: used from the inventory → open the editor directly; used in the
        // world → open the inventory FOR the player (OS-38), and let g_menuSink
        // open the editor once it is up. Nothing is consumed and nothing is worn.
        struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* a_event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_event || !a_event->equipped || !g_seamstone ||
                    a_event->baseObject != g_seamstone->GetFormID()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player || a_event->actor.get() != player) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* ui = RE::UI::GetSingleton();
                if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
                    // World use (Wheeler et al.): open the inventory, then the
                    // editor follows on the InventoryMenu-open event. The guard
                    // stops a second equip event from re-queuing the open.
                    if (!g_openEditorOnInventory) {
                        g_openEditorOnInventory = true;
                        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                            q->AddMessage(RE::InventoryMenu::MENU_NAME,
                                          RE::UI_MESSAGE_TYPE::kShow, nullptr);
                        }
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                OS::EditorWindow::RequestOpen();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        EquipSink g_equipSink;

        // Second half of the OS-38 world-use flow: when the inventory we asked
        // for (above) actually opens, open the editor. Deferred to the task
        // queue so we land AFTER the menu finishes its own setup (the same
        // reason MenuButton defers its injection), and re-checked there in case
        // the menu closed in the same frame.
        struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || !a_event->opening || !g_openEditorOnInventory ||
                    a_event->menuName != RE::InventoryMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                g_openEditorOnInventory = false;
                OS::EditorWindow::RequestOpen();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;

        [[nodiscard]] bool PlayerHas(RE::TESBoundObject* a_obj) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !a_obj) {
                return false;
            }
            const auto inv = player->GetInventory(
                [&](RE::TESBoundObject& a_o) { return &a_o == a_obj; });
            const auto it = inv.find(a_obj);
            return it != inv.end() && it->second.first > 0;
        }

        [[nodiscard]] std::int32_t CountIn(RE::TESObjectREFR* a_holder) {
            if (!a_holder || !g_seamstone) {
                return -1;
            }
            const auto inv = a_holder->GetInventory(
                [](RE::TESBoundObject& a_o) { return &a_o == g_seamstone; });
            const auto it = inv.find(g_seamstone);
            return it != inv.end() ? it->second.first : 0;
        }

        // Farengar's ACTIVE vendor faction, straight from his NPC record -
        // survives any override or runtime patcher that swaps his factions.
        [[nodiscard]] RE::TESFaction* LiveVendorFaction() {
            if (auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(kFarengarNPCID)) {
                for (const auto& f : npc->factions) {
                    if (f.faction && f.faction->IsVendor()) {
                        return f.faction;
                    }
                }
            }
            return RE::TESForm::LookupByID<RE::TESFaction>(kFarengarFactionID);
        }

        // Vendors trade only items whose keywords hit their VEND formlist
        // (whitelist mode). The ESP ships VendorItemSoulGem, but the LIVE
        // list is what counts - if nothing on our stone matches it, adopt
        // the list's first keyword (our own form; safe to mutate).
        void EnsureTradableWith(RE::TESFaction* a_fac) {
            if (!a_fac || !g_seamstone) {
                return;
            }
            auto* list = a_fac->vendorData.vendorSellBuyList;
            if (!list) {
                return;  // no list: the vendor trades everything
            }
            const bool     blacklist = a_fac->vendorData.vendorValues.notBuySell;
            bool           matched   = false;
            RE::BGSKeyword* first    = nullptr;
            for (auto* form : list->forms) {
                auto* kywd = form ? form->As<RE::BGSKeyword>() : nullptr;
                if (!kywd) {
                    continue;
                }
                if (!first) {
                    first = kywd;
                }
                if (g_seamstone->HasKeyword(kywd)) {
                    matched = true;
                    break;
                }
            }
            if (blacklist) {
                if (matched) {
                    spdlog::warn("Lore module: the vendor list is a BLACKLIST that "
                                 "matches the Seamstone - it cannot be traded here.");
                }
                return;
            }
            if (!matched && first) {
                g_seamstone->AddKeyword(first);
                spdlog::info("Lore module: adopted live vendor keyword {:08X} - the "
                             "shipped one was not in this list.", first->GetFormID());
            }
        }

        // Keep exactly one Seamstone purchasable. Chest first (read back -
        // an add into a never-loaded container can silently fail), then
        // Farengar himself; re-tried when his ref attaches (guaranteed
        // loaded), so one of the paths always lands before the player can
        // reach his shop. Idempotent: every path counts before adding.
        void StockFarengar() {
            if (!g_seamstone) {
                return;
            }
            auto* fac   = LiveVendorFaction();
            auto* chest = fac ? fac->vendorData.merchantContainer : nullptr;
            auto* list  = fac ? fac->vendorData.vendorSellBuyList : nullptr;
            spdlog::info("Lore module: vendor faction {:08X}, chest {:08X}, VEND "
                         "list {:08X}{}.",
                         fac ? fac->GetFormID() : 0, chest ? chest->GetFormID() : 0,
                         list ? list->GetFormID() : 0,
                         (fac && fac->vendorData.vendorValues.notBuySell)
                             ? " (blacklist mode)" : "");
            EnsureTradableWith(fac);
            if (PlayerHas(g_seamstone)) {
                return;  // never duplicate while the player owns one
            }
            if (chest) {
                const auto before = CountIn(chest);
                if (before > 0) {
                    return;
                }
                chest->AddObjectToContainer(g_seamstone, nullptr, 1, nullptr);
                const auto after = CountIn(chest);
                spdlog::info("Lore module: chest stock read-back {} -> {}.", before,
                             after);
                if (after > 0) {
                    return;
                }
                spdlog::warn("Lore module: chest add did not land - stocking "
                             "Farengar himself.");
            }
            auto* farengar = RE::TESForm::LookupByID<RE::TESObjectREFR>(kFarengarRefID);
            if (!farengar) {
                spdlog::warn("Lore module: Farengar's reference not found.");
                return;
            }
            const auto before = CountIn(farengar);
            if (before > 0) {
                return;
            }
            farengar->AddObjectToContainer(g_seamstone, nullptr, 1, nullptr);
            spdlog::info("Lore module: Farengar-actor stock read-back {} -> {}.",
                         before, CountIn(farengar));
        }

        // A merchant chest never loads, and adds into unloaded containers
        // may no-op - but Farengar HIMSELF loads whenever the player enters
        // Dragonsreach. His attach is the guaranteed stocking moment.
        struct AttachSink : RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellAttachDetachEvent* a_event,
                RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
                if (a_event && a_event->attached && a_event->reference &&
                    a_event->reference->GetFormID() == kFarengarRefID) {
                    StockFarengar();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        AttachSink g_attachSink;
    }

    void Init() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kEsp)) {
            spdlog::info("Lore module: {} not found - running in pure-UI mode.", kEsp);
            return;
        }
        g_seamstone = dh->LookupForm<RE::TESObjectMISC>(kSeamstoneID, kEsp);
        g_note      = dh->LookupForm<RE::TESObjectBOOK>(kNoteID, kEsp);
        if (!g_seamstone) {
            spdlog::error("Lore module: {} present but the Seamstone form is missing "
                          "(pre-Seamstone esp?) - pure-UI mode.", kEsp);
            return;
        }
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
            holder->AddEventSink<RE::TESCellAttachDetachEvent>(&g_attachSink);
        }
        // MenuOpenCloseEvent comes from UI, not the script holder - the OS-38
        // world-use flow opens the editor after the inventory it requested opens.
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
        }
        spdlog::info("Lore module active: Seamstone {:08X}, note {}.",
                     g_seamstone->GetFormID(), g_note ? "present" : "MISSING");
    }

    void OnPostLoadGame() {
        if (!Available()) {
            return;
        }
        // The note is RETIRED for now (user, 2026-07-18). It used to be handed
        // to the player on every load, but the book does nothing yet - reading
        // it is a later feature - so an item with no purpose was landing in
        // every inventory. Stop delivering it, AND take back the copies the
        // older builds already pushed, so nobody is left carrying dead weight
        // they cannot get rid of sensibly.
        //
        // The FORM STAYS in the ESP and 0x801 is NOT retired: existing saves
        // reference it, so the id must keep resolving, and the whole thing
        // comes back the moment the book is implemented properly.
        if (g_note) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                const auto inv = player->GetInventory(
                    [](RE::TESBoundObject& a_o) { return &a_o == g_note; });
                if (const auto it = inv.find(g_note);
                    it != inv.end() && it->second.first > 0) {
                    player->RemoveItem(g_note, it->second.first,
                                       RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    spdlog::info("Lore module: retired note reclaimed ({} copy/copies).",
                                 it->second.first);
                }
            }
        }
        StockFarengar();
    }

    bool Available() { return g_seamstone != nullptr; }

    bool HasSeamstone() { return PlayerHas(g_seamstone); }

}  // namespace OS::LoreModule
