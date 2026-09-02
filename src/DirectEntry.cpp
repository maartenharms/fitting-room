#include "PCH.h"

#include "DirectEntry.h"

#include "EditorUI.h"  // RequestOpenOnActor, for the actor under the crosshair
#include "EditorWindow.h"
#include "HostGuard.h"
#include "LoreModule.h"
#include "Settings.h"
#include "TextEntry.h"

#include <atomic>

namespace OS::DirectEntry {

    namespace {

        // A host was asked for and has not arrived yet. Written on the main
        // thread (the input sink and the menu sink both), read from the same.
        std::atomic<bool> g_pending{ false };
        // ⚠ AND WHETHER THE HOST IS OURS TO DISMISS. The player's own inventory
        // is not: opening the editor from a menu they opened has always left
        // that menu standing, and the round trip this key asks for is only
        // about the menu THIS module summoned.
        std::atomic<bool> g_summoned{ false };

        // Is the key assigned at all? ⚠ ASKED OF THE SETTINGS RATHER THAN
        // REMEMBERED HERE, because the settings panel writes it live and a
        // cached copy in this file would be a third holder of one number: the
        // INI, the input sink's compare, and us.
        [[nodiscard]] bool Bound() {
            return OS::Settings::GetSingleton().directEntryKeyDIK != 0;
        }

        // The second half of the summon: the menu we asked for has arrived, so
        // the editor follows. Deferred onto the task queue for MenuButton's
        // reason, that the menu is still finishing its own setup on this event.
        struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || a_event->menuName != RE::InventoryMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!a_event->opening) {
                    // ⚠ THE HOST WENT AWAY WITHOUT US. A summon that never
                    // landed, or a menu the player shut before the editor got
                    // there, must not leave the pending flag set: the next press
                    // would read it and do nothing at all.
                    g_pending.store(false, std::memory_order_relaxed);
                    g_summoned.store(false, std::memory_order_relaxed);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_pending.exchange(false, std::memory_order_relaxed)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                OS::EditorWindow::RequestOpen();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;

    }  // namespace

    void Register() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
        }
    }

    bool SummonPending() { return g_pending.load(std::memory_order_relaxed); }

    void SummonHostAndOpen() {
        if (g_pending.exchange(true, std::memory_order_relaxed)) {
            return;  // already on its way
        }
        g_summoned.store(true, std::memory_order_relaxed);
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow,
                          nullptr);
        }
        spdlog::info("DirectEntry: no host open, asking for the inventory; the editor "
                     "opens when it lands.");
    }

    void OnEditorClosed() {
        // ⚠ ONLY A HOST WE SUMMONED, and the flag is the only thing that knows.
        // Re-deriving it from "was the inventory open when the editor opened"
        // cannot tell the player's own inventory from ours, and dismissing
        // theirs would take a menu away that they opened by hand.
        if (!g_summoned.exchange(false, std::memory_order_relaxed)) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
            return;  // already gone, which is the same destination
        }
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide,
                          nullptr);
        }
        spdlog::info("DirectEntry: dismissing the inventory this key summoned, so the "
                     "player lands back in gameplay.");
    }

    void Fire() {
        Ask ask;
        ask.bound         = Bound();
        ask.editorOpen    = OS::EditorWindow::IsOpen();
        ask.hostOpen      = OS::HostGuard::HostMenuOpen();
        ask.loreOk        = OS::LoreModule::GateSatisfied();
        // The same two questions the editor hotkey asks, and for the same
        // reason: a text field is a thing INSIDE a menu, so a menu-name list
        // cannot answer it.
        ask.typing        = OS::TextEntry::Active() ||
                     (RE::UI::GetSingleton() &&
                      RE::UI::GetSingleton()->IsMenuOpen(RE::Console::MENU_NAME));
        ask.summonPending = SummonPending();

        switch (Decide(ask)) {
            case Entry::kCloseEditor:
                OS::EditorWindow::Toggle();  // its own close edge, asking as Escape does
                break;
            case Entry::kOpenHere:
                OS::EditorWindow::RequestOpen();
                break;
            case Entry::kSummonHost:
                // ⚠⚠ THE CROSSHAIR IS READ HERE AND ON THIS ARM ALONE. This is
                // the arm where the player is standing in the WORLD: they are
                // looking at someone, and the host menu does not exist yet.
                // kOpenHere means a menu is already up, where "the actor in
                // front of me" has no meaning any more and CrosshairPickData
                // holds whatever gameplay left behind, so that arm deliberately
                // does not ask (user 2026-08-26 asked for the key that opens
                // from gameplay).
                //
                // ⚠ NO VIABILITY TEST HERE. Whether this actor may be edited is
                // the roster's question and it already answers it, including
                // [Targets] bEditOtherNpcs; asking a second time here would be
                // two authors of one rule.
                //
                // ⚠ targetActor IS AN ObjectRefHandle DESPITE THE NAME, so it is
                // resolved and cast rather than handed straight over. A
                // crosshair can rest on a barrel.
                if (auto* const pick = RE::CrosshairPickData::GetSingleton()) {
                    if (const auto ref = pick->targetActor.get()) {
                        if (auto* const actor = ref->As<RE::Actor>()) {
                            OS::EditorUI::RequestOpenOnActor(actor->GetHandle());
                        }
                    }
                }
                SummonHostAndOpen();
                break;
            case Entry::kRefusedLore:
                // ⚠ SILENT ON PURPOSE, exactly as the editor hotkey is. A player
                // who has never found the Seamstone must not learn it exists
                // from a key they may have pressed by accident. The log line is
                // the only thing that separates "the key did nothing" from "the
                // key never arrived", which are opposite faults.
                spdlog::info("DirectEntry: refused, the lore module is active and the "
                             "player is not carrying the Seamstone. Nothing shown, by "
                             "design.");
                break;
            case Entry::kIgnore:
                break;
        }
    }

}  // namespace OS::DirectEntry
