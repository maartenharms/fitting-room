#include "LoreModule.h"

#include "DirectEntry.h"  // the summon-a-host-then-open flow, owned there
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

        // Opens the editor when the Seamstone is "used". A plain MISC has no
        // native use action, but misc-interaction mods route use through the
        // equip pipeline (ActorEquipManager::EquipObject), which lands here as
        // TESEquipEvent on the main thread. Wheeler (Nexus 97345) is exactly
        // such a mod: a Seamstone slotted in its wheel activates via EquipObject,
        // firing this sink from the WORLD (no menu open).
        //
        // Used from the inventory → open the editor directly; used in the
        // world → hand DirectEntry the summon (OS-38), which opens the inventory
        // FOR the player and opens the editor once it is up. Nothing is consumed
        // and nothing is worn.
        //
        // ⚠ DELIBERATELY NOT HostGuard::HostMenuOpen, AND THE QUESTION IS A
        // DIFFERENT ONE. That gate asks "may the editor live here"; this asks
        // "do I have to summon a menu first", and the answer is the inventory
        // because that is the menu this sink can open. The Seamstone is a MISC
        // item, so the only place it can be equipped from is the inventory or
        // the world - a hotkey mod firing it over the magic menu still lands in
        // the world branch, which summons the inventory rather than refusing.
        // RequestOpen re-asks the real gate at the far end either way.
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
                    // editor follows on the InventoryMenu-open event.
                    //
                    // ⚠⚠ THROUGH DirectEntry, WHICH OWNS THIS FLOW NOW. This
                    // file had its own copy of the summon, its own pending flag
                    // and its own menu sink, and the direct-entry key needed the
                    // same three. Two summoners would disagree about exactly one
                    // thing, which is whose menu it is to dismiss afterwards,
                    // and that is the half the player sees.
                    OS::DirectEntry::SummonHostAndOpen();
                    return RE::BSEventNotifyControl::kContinue;
                }
                OS::EditorWindow::RequestOpen();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        EquipSink g_equipSink;

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

        // The BASE stock list behind the merchant chest, not the chest itself.
        // Resolved live from the faction rather than hardcoded, for the reason
        // LiveVendorFaction already gives: a runtime patcher may have moved the
        // shop and we want whichever chest the game is really going to open.
        [[nodiscard]] RE::TESContainer* MerchantStockList() {
            auto* fac   = LiveVendorFaction();
            auto* chest = fac ? fac->vendorData.merchantContainer : nullptr;
            auto* base  = chest ? chest->GetBaseObject() : nullptr;
            if (!base || base->GetFormType() != RE::FormType::Container) {
                return nullptr;
            }
            return static_cast<RE::TESObjectCONT*>(base);
        }

        // ⚠⚠ THE SEAMSTONE HAS TO BE IN THE CHEST'S BASE LIST, AND NOTHING
        // PUT INTO THE CHEST ITSELF CAN SURVIVE. Opening Farengar's barter
        // REBUILDS MerchantWhiterunFarengarsChest from that list: it carries
        // the Respawns flag, and the rebuild throws away every loose item the
        // reference was holding.
        //
        // The 2026-08-28 field log caught it doing exactly that, in three lines
        // thirteen seconds apart. The stone went in when his cell attached
        // ("0 -> 1"), was still there when the conversation opened ("the chest
        // already holds 1"), and the chest read ZERO again the instant the
        // barter opened. The player was looking at a shop full of nothing but
        // soul gems, which is precisely what this list resolves to.
        //
        // So we add the stone to the list the rebuild reads. This is a live
        // edit of the CONT form. Base forms are never written to a save, so it
        // is re-applied every session, it bakes nothing into anyone's game, and
        // it goes away with the DLL.
        //
        // ⚠ AND IT IS A SYNC RATHER THAN AN ADD, which is what keeps the "one
        // Seamstone" promise the runtime paths have always made. A list entry
        // is restocked on every rebuild, so leaving it there unconditionally
        // would sell a second copy of a key item to somebody already carrying
        // one. The conversation is a safe moment to take it back out: the field
        // log has the dialogue opening a full 2.5 seconds before the barter,
        // and the barter is what reads the list.
        void SyncBaseStock(const char* a_why) {
            if (!g_seamstone) {
                return;
            }
            auto* stock = MerchantStockList();
            if (!stock) {
                spdlog::warn("Lore module: Farengar's merchant chest has no container "
                             "form ({}) - the stone can only be placed loose, and a "
                             "barter rebuild will drop it.", a_why);
                return;
            }
            const bool wanted = !PlayerHas(g_seamstone);
            const auto held   = stock->CountObjectsInContainer(g_seamstone);
            if (wanted && held <= 0) {
                stock->AddObjectToContainer(g_seamstone, 1, nullptr);
                spdlog::info("Lore module: the Seamstone is on Farengar's stock list "
                             "now, {} -> {} ({}).", held,
                             stock->CountObjectsInContainer(g_seamstone), a_why);
            } else if (!wanted && held > 0) {
                stock->RemoveObjectFromContainer(g_seamstone, held);
                spdlog::info("Lore module: the player has a Seamstone, so it comes off "
                             "Farengar's stock list, {} -> {} ({}).", held,
                             stock->CountObjectsInContainer(g_seamstone), a_why);
            } else {
                spdlog::debug("Lore module: the stock list already reads {} and that is "
                              "right ({}).", held, a_why);
            }
        }

        // Keep exactly one Seamstone purchasable. Chest first (read back -
        // an add into a never-loaded container can silently fail), then
        // Farengar himself. Idempotent: every path counts before adding.
        //
        // ⚠⚠ EVERY EARLY RETURN SAYS SO NOW, AND a_why NAMES THE MOMENT.
        // Three of the four used to be silent, so a log could not tell "the
        // chest already had one" from "this never ran" from "the add landed
        // and something ate it afterwards". Those are three different bugs
        // with three different fixes and the 2026-08-28 field run needed
        // exactly this line to separate them.
        void StockFarengar(const char* a_why) {
            if (!g_seamstone) {
                return;
            }
            auto* fac   = LiveVendorFaction();
            auto* chest = fac ? fac->vendorData.merchantContainer : nullptr;
            auto* list  = fac ? fac->vendorData.vendorSellBuyList : nullptr;
            // Once at info, because it is the line that proves we resolved the
            // right shop; after that at debug, because this now runs on every
            // conversation held within earshot of Farengar.
            static bool s_announced = false;
            const auto  where =
                fmt::format("Lore module: vendor faction {:08X}, chest {:08X}, VEND "
                            "list {:08X}{} ({}).",
                            fac ? fac->GetFormID() : 0,
                            chest ? chest->GetFormID() : 0,
                            list ? list->GetFormID() : 0,
                            (fac && fac->vendorData.vendorValues.notBuySell)
                                ? " (blacklist mode)" : "",
                            a_why);
            if (!s_announced) {
                s_announced = true;
                spdlog::info("{}", where);
            } else {
                spdlog::debug("{}", where);
            }
            EnsureTradableWith(fac);
            // ⚠ FIRST, AND IT IS THE ARM THAT ACTUALLY STOCKS THE SHOP. The
            // loose adds below only cover the gap between one rebuild and the
            // next; this is what the rebuild itself reads.
            SyncBaseStock(a_why);
            if (PlayerHas(g_seamstone)) {
                spdlog::debug("Lore module: nothing to stock ({}) - the player is "
                              "already carrying a Seamstone.", a_why);
                return;  // never duplicate while the player owns one
            }
            if (chest) {
                const auto before = CountIn(chest);
                if (before > 0) {
                    spdlog::debug("Lore module: the chest already holds {} ({}).",
                                  before, a_why);
                    return;
                }
                chest->AddObjectToContainer(g_seamstone, nullptr, 1, nullptr);
                const auto after = CountIn(chest);
                spdlog::info("Lore module: chest stock read-back {} -> {} ({}).",
                             before, after, a_why);
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
                spdlog::debug("Lore module: Farengar already carries {} ({}).", before,
                              a_why);
                return;
            }
            farengar->AddObjectToContainer(g_seamstone, nullptr, 1, nullptr);
            spdlog::info("Lore module: Farengar-actor stock read-back {} -> {} ({}).",
                         before, CountIn(farengar), a_why);
        }

        // ⚠⚠ NOTHING STOCKS FROM INSIDE A CELL ATTACH ANY MORE, AND THAT WAS
        // THE BUG. Farengar's merchant chest is not the never-loaded container
        // the comment above assumed: MerchantWhiterunFarengarsChest sits at
        // (1077, 1974, -172) and Farengar at (1083, 2025, -80), which is his own
        // alcove in Dragonsreach. It attaches in the same breath he does, it
        // carries the Respawns flag, and a container the engine is still
        // bringing up is the one place an add cannot be trusted to survive.
        //
        // The 2026-08-28 field log is the whole story in two lines: at
        // 13:05:08.415 the attach handler added a Seamstone and read it straight
        // back ("0 -> 1"), and eighty-three seconds later Farengar had nothing to
        // sell. The add was real and something after it was not.
        //
        // So every caller hands the work to the task queue instead, which runs
        // it on the main thread on a later frame, off the attach entirely.
        // a_why must be a string literal: the lambda captures the pointer.
        void StockSoon(const char* a_why) {
            if (!g_seamstone) {
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_why] { StockFarengar(a_why); });
                return;
            }
            StockFarengar(a_why);
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
                    StockSoon("Farengar's cell attached");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        AttachSink g_attachSink;

        // Is Farengar standing in front of the player right now? Cheap enough
        // to ask on every menu open, and it is the only question worth asking:
        // the stocking below is pointless anywhere he cannot be reached.
        [[nodiscard]] bool FarengarLoaded() {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(kFarengarRefID);
            return ref && ref->Is3DLoaded();
        }

        // ⚠⚠ THE STOCK IS CHECKED AGAIN AT THE POINT OF SALE, AND THIS IS THE
        // ARM THAT ACTUALLY FIXES THE REPORT. The two load-shaped paths above
        // both have a hole in them: kPostLoadGame is never delivered to a
        // character started with `coc` from the main menu
        // (coc-from-main-menu-skips-newgame, and this is the fourth feature in
        // this repo to hang there), and the attach path fires at the one moment
        // the container is not settled. Talking to a merchant has neither
        // problem. It happens long after any cell has finished loading, it
        // happens every single time, and it happens BEFORE the player asks what
        // he has for sale.
        //
        // Both menus are watched because they answer different questions. The
        // dialogue menu is the one that can still change what this visit sells:
        // it opens a conversation before "What have you got for sale?" exists.
        // The barter menu cannot (its list is built as it opens), but reading
        // the chest there costs nothing and writes down what the player is
        // actually looking at, which is the reading this bug wanted and did not
        // have.
        struct VendorMenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || !a_event->opening || !g_seamstone) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool talking = a_event->menuName == RE::DialogueMenu::MENU_NAME;
                const bool trading = a_event->menuName == RE::BarterMenu::MENU_NAME;
                if ((!talking && !trading) || !FarengarLoaded()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                StockSoon(talking ? "a conversation opened beside Farengar"
                                  : "a barter menu opened beside Farengar");
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        VendorMenuSink g_vendorMenuSink;
    }

    RE::TESObjectMISC* Stone() { return g_seamstone; }

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
        // The point-of-sale re-check. Registered here rather than from
        // plugin.cpp because it is the lore module's own business and it must
        // not exist at all in pure-UI mode, where there is no stone to sell.
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_vendorMenuSink);
        }
        // ⚠ THE SECOND HALF OF THE OS-38 FLOW IS NOT REGISTERED HERE ANY MORE.
        // Waiting for the summoned inventory and opening the editor on it is
        // DirectEntry's sink now, registered from plugin.cpp, because the
        // direct-entry key needed exactly the same wait. Two sinks on one menu
        // event, both calling RequestOpen off one flag, is the shape a double
        // open comes from.
        // ⚠ THE STOCK LIST IS SET UP HERE, BEFORE ANY GAME EXISTS, AND ONLY
        // THE LIST. Base forms are loaded by now and the faction resolves, so
        // the shop is stocked from the first barter of the session; the loose
        // adds are not attempted, because there is no world to add into and no
        // player inventory worth asking about yet. Every later moment re-syncs.
        SyncBaseStock("the plugin loaded");
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
        StockSoon("the game finished loading");
    }

    bool Available() { return g_seamstone != nullptr; }

    bool HasSeamstone() { return PlayerHas(g_seamstone); }

    bool RequirementActive() {
        return Settings::GetSingleton().requireSeamstone && Available();
    }

    bool GateSatisfied() { return !RequirementActive() || HasSeamstone(); }

}  // namespace OS::LoreModule
