#pragma once

#include <string>
#include <string_view>

namespace OS::HostGuard {

    // ⚠⚠ THE ONE LIST OF MENUS THE EDITOR MAY LIVE OVER, AND IT IS ONE LIST
    // BECAUSE IT WAS SIX. The same two lines - inventory open, or Screen Archer
    // Menu open - were copied into InputListener's hotkey, both RequestOpen
    // entries, both openedFromSam bookkeeping sites and this file's own close
    // gate, and the close gate's comment promised it "mirrors the open gate
    // exactly", which is a promise six copies cannot keep.
    //
    // ⚠⚠ THEY MOVE TOGETHER OR THE EDITOR BREAKS ONE OF TWO WAYS. A menu added
    // to the open side and missed on the close side opens the editor and shuts
    // it the same frame, because the close gate reads the current stack and
    // finds no host. Missed on the open side and added to the close side is
    // worse, and the note below spells it out: a host that goes away without
    // SetOpen(false) leaves CameraFrame::Tick re-asserting the retarget every 30
    // frames while the player has gameplay control.
    //
    // ⚠ IT IS AN ALLOWLIST, WHICH THIS PROJECT HAS BEEN BITTEN BY BEFORE. It is
    // the right shape here only because the question is not "which menus exist"
    // but "which menus has this window's modal setup been tried against": the
    // editor alpha-hides its host's 2D, forces third person and takes the
    // cursor, so a menu it has never been opened over is a guess rather than an
    // omission. Adding one is a deliberate act with a field round behind it.

    // The engine's own spelling, as literals, so ClassifyHost below is pure and
    // the suite can run without a game. HostGuard.cpp static_asserts each
    // against RE's constant, so a typo here is a build error rather than a gate
    // that quietly never matches.
    inline constexpr std::string_view kInventoryMenu = "InventoryMenu";
    inline constexpr std::string_view kMagicMenu     = "MagicMenu";

    // Grid Inventory's own menu. It is an SKSE plugin that registers a REAL
    // IMenu under this name and opens it INSTEAD of the vanilla inventory, so
    // the vanilla name is never open and the editor refused every hotkey press
    // with "neither the inventory nor Screen Archer Menu is open" (field,
    // 2026-08-27). Verified as a string in GridInventory.dll rather than taken
    // from the report.
    //
    // ⚠ NO static_assert FOR THIS ONE, because there is no engine constant to
    // prove it against: it belongs to another author's plugin. That makes it the
    // one name here that a typo would turn into a silently dead gate, so it is
    // spelled once and every site reads this.
    //
    // ⚠⚠ AND THE ALPHA-HIDE DOES NOTHING HERE. Grid Inventory draws with ImGui,
    // not Scaleform, so its IMenu has no uiMovie and EditorWindow's root-alpha
    // call is a silent no-op. That is safe (SetMenuRootAlpha already guards on
    // uiMovie) but it is NOT invisible: the grid stays on screen under the
    // editor instead of being hidden the way the vanilla inventory is. Named
    // here so the next reader does not go looking for a bug in the alpha code.
    inline constexpr std::string_view kGridInventoryMenu = "GridInventoryMenu";

    // Which menu is hosting, by the name the engine knows it by.
    //
    // ⚠ A NAME AND NOT A FLAG, because the caller has to be able to put back
    // exactly what it took. EditorWindow zeroes its host's Scaleform root alpha
    // on open and restores it on close, and while a bool could say "SAM or the
    // inventory" it could not say WHICH of three, so the close would hand the
    // alpha back to a menu that was never hidden and leave the real host
    // invisible for the rest of the session.
    struct Host {
        std::string name;
        // Screen Archer Menu, which is a context rather than a vanilla menu:
        // SAM frames its own shot, so the editor leaves the camera alone there,
        // anchors its panel to the other side and takes Escape back by hand.
        bool isSam = false;

        [[nodiscard]] bool Present() const { return !name.empty(); }
    };

    // The pure half: which of the three is hosting, given who is open.
    //
    // ⚠ THE ORDER IS THE PRECEDENCE AND IT IS NOT ARBITRARY. The inventory has
    // always won over SAM (the code this replaces read "SAM and not the
    // inventory"), so it stays first and SAM stays last. Two of these can be up
    // at once - SAM is a separate menu and does not close the inventory - and
    // the answer must not depend on which line happens to be evaluated.
    // ⚠ GRID INVENTORY SITS WITH THE VANILLA INVENTORY, FIRST. It replaces the
    // inventory rather than stacking on it, so in practice only one of the two
    // is ever up; putting it anywhere else would only matter on a rig where
    // both somehow open, and there the answer must still be an inventory.
    [[nodiscard]] inline Host ClassifyHost(bool a_inventoryOpen, bool a_gridOpen,
                                           bool a_magicOpen, bool a_samOpen,
                                           std::string_view a_samMenuName) {
        if (a_inventoryOpen) {
            return Host{ std::string(kInventoryMenu), false };
        }
        if (a_gridOpen) {
            return Host{ std::string(kGridInventoryMenu), false };
        }
        if (a_magicOpen) {
            return Host{ std::string(kMagicMenu), false };
        }
        // An empty configured name means SAM compatibility is switched off, and
        // SamCompat::IsMenuOpen answers false there - but a host with an empty
        // name would read as "no host" to Present() anyway, so this cannot
        // produce a host nobody can restore.
        if (a_samOpen && !a_samMenuName.empty()) {
            return Host{ std::string(a_samMenuName), true };
        }
        return Host{};
    }

    // The same question asked of the live UI. Cold: menu transitions, the
    // hotkey edge and the scripted open, never per frame.
    [[nodiscard]] Host CurrentHost();

    // Is the editor allowed to be open where it is right now? The open gates
    // and the close gate both ask this, which is the point.
    [[nodiscard]] bool HostMenuOpen();

    // The editor is HOSTED: it opens over a menu it does not own (the vanilla
    // InventoryMenu or MagicMenu, or Screen Archer Menu) and deliberately does
    // NOT set kCloseOnGameMenu, because it opens BECAUSE a game menu is up. SamCompat
    // already closes it when SAM goes away. Nothing did the same for the
    // inventory, and that gap became load-bearing the moment the editor started
    // writing the world camera.
    //
    // THE FAILURE IT CLOSES, every step of which is documented in the source:
    // the editor grants kPassInputToGame while the cursor is off the panel and a
    // button is held (the rotate-drag gesture), which is exactly how Esc reached
    // the game before and "closed the inventory but not the editor"
    // (EditorWindow.cpp's own note). With the host gone the editor is still
    // open_ and still ready_, SetOpen(false) never runs, so CameraFrame::Release
    // never runs either - while CameraFrame::Tick keeps re-asserting the
    // retarget every 30 frames. The player gets gameplay control with the world
    // camera nailed to a follower, and the re-assert is what makes that
    // unrecoverable instead of transient.
    //
    // Two jobs, in order:
    //   1. Host menu closed while the editor is open  -> close the editor.
    //   2. ANY menu closed, editor not open, camera still held -> release it.
    // The second is the true backstop: it is the only thing that covers FLICK
    // ceasing to call Draw without SetOpen(false) ever running, which this
    // project has already been bitten by once (kCloseOnGameMenu HIDES rather
    // than closes).
    void Register();

    // kPreLoadGame. A load with the editor nominally open is the other way the
    // camera gets stranded, and it is worse than the menu case: the subject is
    // held as a reference FormID so it re-resolves in the NEW save rather than
    // going stale, which would point the camera at that save's follower.
    void OnPreLoadGame();

}  // namespace OS::HostGuard
