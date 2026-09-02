#pragma once

namespace OS::EditorUI {
    void OnOpen();   // begin staging from the active outfit
    void OnClose();  // discard staging if the user never pressed Apply
    void Draw();     // called inside the ImGui frame (Present-hook thread)

    // The "Hide outfit" button, drawn FIRST on the bottom bar of the page that
    // calls it (user 2026-08-19: "hide outfit toggle needs to be a button in the
    // bottom bar of all pages"). For the pages whose bar lives in another file:
    // Body Studio, Shape, Overlays and Rules.
    //
    // ⚠ ONE DEFINITION FOR ALL SEVEN PAGES, and this is a forwarder to it. The
    // state, the label, the width, the sound and the tooltip are EditorUI.cpp's,
    // because two painters of one appearance always drift; a page that wants the
    // button asks for it rather than making one.
    //
    // ⚠ CALL IT INSIDE AN EXISTING BOTTOM BAR, before the bar's own buttons and
    // with a SameLine after it. A bar drawn under panes that already took the
    // whole height is a bar pushed off the bottom of the page, so a page adding
    // its first bar has to reserve the height first.
    void DrawHideOutfitButton();

    // The current edit target's actor handle, EMPTY when the target is the
    // player or an "(away)" assignee - so a caller that only cares about a live
    // follower can test the resolved pointer and stop. Added for the OS-97
    // camera probe (CameraProbe.h); delete with it if nothing else picks it up.
    // Open the editor already pointed at this actor instead of at the player.
    //
    // ⚠⚠ A REQUEST, NOT A SWITCH, and it is spent by the next OnOpen. The
    // press happens on the input thread during gameplay and the roster is built
    // on the main thread inside OnOpen, so there is no list to look the actor up
    // in at the moment it is asked for.
    //
    // ⚠ AN ACTOR THE ROSTER DOES NOT CARRY IS IGNORED IN SILENCE, and that is
    // what makes this respect [Targets] bEditOtherNpcs without asking about it:
    // a stranger is simply not in the list when the setting is off, so the
    // request finds nothing and the editor opens on the player as it always did.
    void RequestOpenOnActor(RE::ActorHandle a_actor);

    [[nodiscard]] RE::ActorHandle CurrentNpcTargetHandle();

    // ---- closing on top of work that has not been paid for ----------------
    //
    // ⚠ ONLY LORE-FRIENDLY LOSES ANYTHING. Free-form commits every edit as it
    // is made, so closing there discards nothing and must not ask. Under a
    // charge the edit lives only in staging until Apply authorises the bill, and
    // OnClose then throws it away. A player who cannot AFFORD the bill has no
    // way to press Apply at all, so the work is unreachable and silently gone
    // (user 2026-08-07).
    //
    // Read from the main thread and written by Draw on the render thread, hence
    // the atomic behind it.
    [[nodiscard]] bool CloseWouldLoseWork();

    // Ask before closing. Opens a modal on the next drawn frame; answering it
    // is what actually closes the window.
    void RequestCloseConfirm();

    // Empty the current character's Recent colours.
    //
    // ⚠ FOR A PLAYSTYLE SWITCH AND NOTHING ELSE. Recent holds the colours the
    // player MIXED BY HAND, which is exactly the set free-form allows and
    // lore-friendly does not: the swatch grid locks a colour that has not been
    // earned, but Recent was never gated, so every colour mixed in free-form
    // stayed one click away after switching to lore-friendly and the unlock
    // economy could be walked straight around (user 2026-08-08).
    //
    // Cleared in BOTH directions, deliberately. The list is a convenience, not
    // a record - nothing is lost that cannot be picked again - and a rule that
    // fires on one button and not the other is the asymmetry the playstyle
    // preset block already has a ⚠ about.
    //
    // Safe from any thread: it marshals to the main one, like every other
    // DyeHistory writer reached from a draw.
    void ForgetMixedColours();

    // Re-read whichever style source the browser is showing, for the Rescan
    // button that now lives in the settings panel rather than in the editor's
    // own gear popup.
    //
    // ⚠⚠ IT IS CALLABLE WITH THE EDITOR CLOSED, and that is the whole reason it
    // is a function rather than a moved block of code. The panel opens from
    // FLICK's sidebar with no editor, no staging session and no showcase
    // selection, and the old handler reset all three and re-seeded staging. The
    // STORE rescan is global and always safe; everything editor-shaped inside is
    // guarded on the editor actually being open.
    void RequestSourceRescan();

    // Controller: seat nav on the first card the right-hand pane draws next.
    // Call it wherever a left-hand list opens a right-hand pane.
    void FocusRightPaneOnGamepad();

    // What that button's tooltip should say, which depends on which source the
    // browser is showing. Returned rather than composed in the panel, because
    // g_showcaseTabs is this file's and the panel must not learn about it.
    [[nodiscard]] const char* SourceRescanTip();

    // Whether the character editor's door fee is within reach right now.
    //
    // ⚠ THE GATE LIVES ON THE BUTTON BECAUSE IT CANNOT LIVE AT THE DOOR.
    // HeadEditorSink takes the fee as RaceMenu opens, by which point the menu
    // is already coming up and refusing is not available. So the only place a
    // trip can actually be declined is where it is OFFERED, which is Menu
    // Studio's appearance button. Anyone reaching RaceMenu another way was
    // never gated by us at all, and pays whatever they can.
    [[nodiscard]] bool CanAffordLooksMenu();

    // Tell Menu Studio's strip whether the character editor can be paid for.
    //
    // ⚠ EDGE TRIGGERED, AND a_force IS THE FIRST EDGE. The answer moves while
    // the editor is open, because Apply spends the very charge the door fee
    // comes out of, and publishing only at open is what left a fully live
    // button after the stone was emptied. Publishing every frame is not the
    // alternative: this crosses to the main thread, so it goes out when the
    // answer CHANGES and once unconditionally when the editor opens.
    void PublishLooksMenuAffordability(bool a_force);
}
