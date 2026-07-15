#include "PCH.h"

#include "EditorWindow.h"

#include "EditorStyle.h"  // PlayUISound
#include "EditorUI.h"
#include "SamCompat.h"
#include "SceneGuard.h"
#include "StyleCatalog.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h
#include "FUCK_API.h"

#include <imgui.h>  // ImGuiHoveredFlags_AnyWindow (FUCK owns the context; the flag is a pure enum)

#include <algorithm>  // std::clamp
#include <atomic>

namespace {
    // Cross-mod contract with Apparel Preview: clear any hover preview so the
    // editor always starts from the true look (mirrors the old ImGuiOverlay).
    constexpr std::uint32_t kClearPreviewMsg = 'CLRP';

    // Hide/show a menu's 2D content WITHOUT ShowMenus(false). ShowMenus(false) also
    // hides the game cursor that FUCK draws on (Fuzzles: "we rely on the game
    // cursor"), so instead we zero the target menu's Scaleform root alpha - the
    // separate cursor menu and the 3D SPIM character are untouched. AS2 _alpha = 0..100.
    template <class Name>
    void SetMenuRootAlpha(const Name& a_menu, double a_alpha) {
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto menu = ui->GetMenu(a_menu); menu && menu->uiMovie) {
                menu->uiMovie->SetVariable("_root._alpha", RE::GFxValue(a_alpha));
            }
        }
    }

    class EditorIWindow : public FUCK::IWindow {
    public:
        const char* Id() const override { return "OutfitSlotsEditor"; }
        const char* Title() const override { return "Fitting Room"; }

        bool IsOpen() const override { return open_.load(std::memory_order_relaxed); }

        void SetOpen(bool a_open) override {
            const bool was = open_.exchange(a_open, std::memory_order_relaxed);
            if (a_open == was) {
                return;
            }
            auto* ui = RE::UI::GetSingleton();
            if (a_open) {
                // Refuse during a scene (OStim etc.): transmog is suspended and
                // the editor would hijack the scene camera/input.
                if (OS::SceneGuard::Active()) {
                    open_.store(false, std::memory_order_relaxed);
                    RE::DebugNotification("You can't edit outfits during a scene.");
                    return;
                }
                forceLayout_.store(true, std::memory_order_relaxed);  // OS-54: re-apply standardized geometry on the first Draw
                // Re-evaluate style fit if the race changed (RaceMenu); must run
                // before FUCK's Present thread draws rows (fitsBody read unsynced).
                OS::StyleCatalog::GetSingleton().EnsureFitCurrent();
                OS::EditorStyle::PlayUISound("UIMenuOK");
                openedFromSam_ = !(ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) &&
                                 OS::SamCompat::IsMenuOpen();
                // Ask Apparel Preview (when present) to drop any hover preview.
                if (auto* messaging = SKSE::GetMessagingInterface()) {
                    messaging->Dispatch(kClearPreviewMsg, nullptr, 0, "ApparelPreview");
                }
                // In SAM the shot is already framed - leave the camera alone.
                if (!openedFromSam_) {
                    if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                        wasFirstPerson_ = cam->IsInFirstPerson();
                        cam->ForceThirdPerson();
                    }
                }
                // Hide the 2D UI of the menu we opened over so only the world + the
                // SPIM character show behind the editor. NOT ShowMenus(false): that
                // also hides the game cursor FUCK relies on (Fuzzles) - instead we zero
                // the menu's Scaleform root alpha, leaving the cursor menu and the 3D
                // character intact. kRenderDuringTM keeps our window drawing.
                if (openedFromSam_) {
                    SetMenuRootAlpha(OS::Settings::GetSingleton().samMenuName, 0.0);
                } else {
                    SetMenuRootAlpha(RE::InventoryMenu::MENU_NAME, 0.0);
                    // The 2D alpha-hide leaves the inventory's 3D item preview (the
                    // floating rotating model) visible; clear it so only the SPIM
                    // character shows behind the editor.
                    if (auto* inv3d = RE::Inventory3DManager::GetSingleton()) {
                        inv3d->Clear3D();
                    }
                }
                OS::EditorUI::OnOpen();
                FUCK::ForceCursor(true);  // belt-and-suspenders; the game cursor stays now
            } else {
                FUCK::ForceCursor(false);
                OS::EditorStyle::PlayUISound("UIMenuCancel");
                OS::EditorUI::OnClose();
                // Restore the 2D of the menu we alpha-hid on open.
                if (openedFromSam_) {
                    SetMenuRootAlpha(OS::Settings::GetSingleton().samMenuName, 100.0);
                } else {
                    SetMenuRootAlpha(RE::InventoryMenu::MENU_NAME, 100.0);
                }
                if (auto* cam = RE::PlayerCamera::GetSingleton();
                    cam && wasFirstPerson_ && !openedFromSam_) {
                    cam->ForceFirstPerson();
                }
            }
        }

        FUCK::WindowFlags GetFlags() const override {
            // A solid panel (NO kNoBackground - that left everything except the two
            // child panels transparent). The SPIM character sits to the right,
            // outside the window. No vanity drift; render over the open game menu
            // (kRenderDuringTM) while the shim alpha-hides the inventory's 2D (NOT
            // ShowMenus(false), which also hid the game cursor FUCK relies on). NOT
            // kCloseOnGameMenu - we open BECAUSE a game menu is up. kIgnoreUserScale
            // opts the editor out of FUCK's global user content-scale (per Fuzzles), so
            // the widgets aren't inflated past vanilla ImGui; density is instead our own
            // "UI size" slider (SetWindowFontScale in EditorUI).
            //
            // NO kCloseOnEsc: it also maps controller B/Circle to close, which nuked the
            // whole editor instead of doing panel back-nav (per Fuzzles, "close on esc
            // covers back too"). Draw() handles Start + Esc close instead, leaving B for
            // FUCK's default back-nav. Whether dropping the flag fully frees B is
            // empirical - verify in-game.
            FUCK::WindowFlags flags = FUCK::WindowFlags::kBlockVanity |
                                      FUCK::WindowFlags::kRenderDuringTM |
                                      FUCK::WindowFlags::kIgnoreUserScale;
            // Standardized layout by default (user): lock position, size and chrome so
            // the editor always opens the same. kNoDecoration also drops the FUCK title
            // bar so "Outfit Slots" isn't shown twice (title bar + our own header). The
            // gear's "Lock window" toggle clears these for users who want to move/resize.
            if (OS::Settings::GetSingleton().lockLayout) {
                flags = flags | FUCK::WindowFlags::kNoMove | FUCK::WindowFlags::kNoResize |
                        FUCK::WindowFlags::kNoDecoration | FUCK::WindowFlags::kCustomPosition;
            }
            // Character rotation, Fuzzles' RaceMenu-Enhancer pattern (TIGHTENED after a
            // field round). kPassInputToGame is "allows player control while open", so we
            // hand it to the game ONLY while the cursor is over the character (outside
            // every FUCK window, cached in Draw) AND a mouse button is held - i.e. an
            // actual rotate-drag. The first field build gated on hover alone, which (a)
            // let the game hide the cursor the whole time the pointer sat over the
            // character half and (b) leaked Esc to the game (closing the inventory but not
            // the editor). Requiring a held button confines passthrough to the drag: the
            // cursor stays visible while hovering, and Esc closes the editor normally.
            // IsMouseDown is read fresh here (not cached) so the button-down edge is not
            // lost to a frame of latency and rotation still starts cleanly. Passthrough is
            // gated at all because the UNCONDITIONAL flag (ba7fbbb) CTD'd the Papyrus VM
            // over a live InventoryMenu (execute-at-0x0 in SKI_PlayerLoadGameAlias,
            // crash-2026-07-14-01-17-30).
            // Controller (OS-53, user re-ask): ALSO pass through while the RIGHT STICK is
            // deflected - the vanilla InventoryMenu's rotate input - so the stick rotates
            // the character instead of only scrolling the FUCK list. Confined to the
            // gesture the same way the mouse path is (stick centred / button up =>
            // passthrough off), which is what made the mouse case safe vs the CTD-prone
            // unconditional flag. EXPERIMENTAL: if FUCK owns the right stick for scroll they
            // may fight, or the alpha-hidden menu may not rotate - field-test; a clean
            // "give me the right stick" is likely a Fuzzles ask.
            const bool mouseRotate = !cursorOverUI_.load(std::memory_order_relaxed) &&
                                     (FUCK::IsMouseDown(0) || FUCK::IsMouseDown(1));
            if (mouseRotate || rStickActive_.load(std::memory_order_relaxed)) {
                flags = flags | FUCK::WindowFlags::kPassInputToGame;
            }
            return flags;
        }

        ImVec2 GetDefaultSize() const override {
            const ImVec2 d = FUCK::GetDisplaySize();
            const float  w = std::clamp(d.x * 0.60f, 840.0f, 1230.0f);
            // Full display height so the panel sits flush to the top AND bottom edges
            // (user: the old 24px inset left top/left padding while the footer was still
            // very slightly clipped - go flush, or at least symmetric). FUCK's own window
            // padding still insets the content, so top and bottom read as an equal small
            // margin, and the extra height clears the footer clip.
            const float h = d.y > 96.0f ? d.y : 720.0f;
            return ImVec2(w, h);
        }
        // Flush to the top-left corner (user): remove the top/left inset; FUCK's window
        // padding provides the small consistent margin. Pairs with the full-height size.
        ImVec2 GetDefaultPos() const override { return ImVec2(0.0f, 0.0f); }

        void Draw() override {
            // OS-54: force our standardized geometry on the first frame after open (and
            // on any FUCK re-appear). CONFIRMED cause: FUCK persists per-window geometry
            // (x/y/w/h) keyed by Id() in ...\overwrite\FUCKs\FUCK\tools\Outfit Slots.json
            // and RESTORES it on cold boot, so our GetDefaultSize/Pos (only honoured when
            // the window first initialises with no saved entry) are ignored - the saved
            // h/y seated the footer (Apply/Close) off the bottom. kCustomPosition did not
            // spare us (x/y are saved regardless). The gear's "Lock window" toggle only
            // fixed it because flipping the flags made FUCK re-apply the defaults; do that
            // ourselves every open. FUCK applies saved geometry once-on-appear (the toggle
            // fix STICKS for the session), so this in-Draw SetWindowSize(...,Always) wins
            // and holds. Set BEFORE drawing content so the layout uses the corrected size
            // (no open-frame flicker). Gated on lockLayout so an unlocked user's manual
            // size/pos is preserved. cond=0 == ImGuiCond_Always. The forceLayout_ latch
            // (set in SetOpen) makes the first post-open frame deterministic even if
            // IsWindowAppearing does not fire on the hosted window.
            // The gear's "Reset window position" fires resetGeometry_ - unconditional of
            // lockLayout, so it also snaps an unlocked/moved window back to the default.
            const bool resetGeom = resetGeometry_.exchange(false, std::memory_order_relaxed);
            if (resetGeom ||
                (OS::Settings::GetSingleton().lockLayout &&
                 (forceLayout_.exchange(false, std::memory_order_relaxed) ||
                  FUCK::IsWindowAppearing()))) {
                FUCK::SetWindowSize(GetDefaultSize());
                FUCK::SetWindowPos(GetDefaultPos());
            }

            OS::EditorUI::Draw();
            // Cache "is the cursor over our UI" for GetFlags()'s conditional
            // kPassInputToGame (see there). Over any FUCK window, or while a widget is
            // active (typing / holding a slider) => the panel owns input; otherwise the
            // cursor is on the character and the rotation drag should reach the game.
            const bool overUI =
                FUCK::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || FUCK::IsAnyItemActive();
            cursorOverUI_.store(overUI, std::memory_order_relaxed);

            // Controller rotation (OS-53): cache right-stick deflection for GetFlags's
            // passthrough gate. ImGui feeds the right stick as these nav keys; if FUCK
            // doesn't surface them this stays false (no-op - then it's a Fuzzles ask).
            rStickActive_.store(FUCK::IsKeyDown(ImGuiKey_GamepadRStickLeft) ||
                                    FUCK::IsKeyDown(ImGuiKey_GamepadRStickRight) ||
                                    FUCK::IsKeyDown(ImGuiKey_GamepadRStickUp) ||
                                    FUCK::IsKeyDown(ImGuiKey_GamepadRStickDown),
                                std::memory_order_relaxed);

            const bool passing = !overUI && (FUCK::IsMouseDown(0) || FUCK::IsMouseDown(1));

            // Re-assert the cursor every non-drag frame: ShowMenus(false) keeps
            // re-hiding the game cursor, so the one-shot ForceCursor(true) on open does
            // not stick (field: cursor invisible). During an actual rotate-drag we let
            // the game hide it.
            if (!passing) {
                FUCK::ForceCursor(true);
            }

            // Close on Start or Esc - kCloseOnEsc is dropped (see GetFlags) so B/Circle
            // stays free for FUCK's back-nav. RequestClose queues onto the main thread
            // (SetOpen touches RE::UI / the camera).
            if (FUCK::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
                FUCK::IsKeyPressed(ImGuiKey_Escape, false)) {
                OS::EditorWindow::RequestClose();
            }
        }

        // Snap back to GetDefaultSize/Pos next frame (the gear's "Reset window position").
        // Render-thread safe - just sets an atomic Draw() reads.
        void RequestResetGeometry() { resetGeometry_.store(true, std::memory_order_relaxed); }

    private:
        std::atomic<bool> open_{ false };
        std::atomic<bool> cursorOverUI_{ true };  // cached in Draw(); gates kPassInputToGame (safe default: no passthrough)
        std::atomic<bool> forceLayout_{ false };  // OS-54: set on open; Draw re-applies GetDefaultSize/Pos on the first post-open frame
        std::atomic<bool> rStickActive_{ false }; // OS-53: right stick deflected => rotate passthrough (cached in Draw)
        std::atomic<bool> resetGeometry_{ false };// gear "Reset window position" => re-apply defaults next frame
        bool              wasFirstPerson_{ false };
        bool              openedFromSam_{ false };
    };

    EditorIWindow g_editorWindow;
}

namespace OS::EditorWindow {

    void Register() {
        // SettingsUI::Register already called FUCK::Connect; RegisterWindow is a
        // no-op if FUCK is absent (no editor without FUCK.dll).
        FUCK::RegisterWindow(&g_editorWindow);
        spdlog::info("EditorWindow: registered as a FUCK IWindow (editor hotkey opens it).");
    }

    bool IsOpen() { return g_editorWindow.IsOpen(); }

    bool WantsTextInput() { return FUCK::IsAnyItemActive(); }

    void ResetGeometry() { g_editorWindow.RequestResetGeometry(); }

    void Toggle() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { g_editorWindow.SetOpen(!g_editorWindow.IsOpen()); });
        }
    }

    void RequestOpen() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                if (g_editorWindow.IsOpen()) {
                    return;
                }
                auto*      ui          = RE::UI::GetSingleton();
                const bool inInventory = ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
                if (inInventory || OS::SamCompat::IsMenuOpen()) {
                    g_editorWindow.SetOpen(true);
                } else {
                    RE::DebugNotification(
                        "Open your inventory or Screen Archer Menu to edit outfits.");
                }
            });
        }
    }

    void RequestClose() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                if (g_editorWindow.IsOpen()) {
                    g_editorWindow.SetOpen(false);
                }
            });
        }
    }

}  // namespace OS::EditorWindow
