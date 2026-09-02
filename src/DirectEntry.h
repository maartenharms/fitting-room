#pragma once

// Entering Fitting Room from gameplay, on a key of the player's choosing.
//
// ⚠⚠ THE EDITOR IS HOSTED AND THAT IS NOT NEGOTIABLE HERE. It opens over a
// vanilla menu it does not own (HostGuard's allowlist: the inventory, the magic
// menu or Screen Archer Menu), alpha-hides that menu's 2D and draws in front of
// the character the menu is already showing. So "enter from the world" cannot
// mean "open the editor with nothing behind it": it means SUMMON a host and
// open once it is up, which is the flow the Seamstone already used from
// Wheeler (OS-38). This file owns that flow now, and the Seamstone calls it,
// because two paths that both summon a menu and then open a window would drift
// apart on which one cleans up.
//
// ⚠ AND THE CLEAN-UP IS THE HALF THAT IS EASY TO MISS. A host WE summoned is
// ours to dismiss: the field's ask was a round trip, gameplay to the editor and
// back to gameplay, and leaving the inventory standing when the editor closes
// makes the key a one-way door. A host the PLAYER opened is not ours and stays
// exactly where it was.

#include <cstdint>

namespace OS::DirectEntry {

    // What one press of the direct-entry key should do.
    enum class Entry : std::uint8_t {
        kIgnore,       // nothing, and nothing is drawn or said
        kCloseEditor,  // the editor is up: this is its close edge
        kOpenHere,     // a host is already up, so open over it
        kSummonHost,   // no host: ask for the inventory, open when it arrives
        kRefusedLore   // lore mode, and the player is not carrying the Seamstone
    };

    // Everything the decision needs, so the decision itself can be tested
    // without a game.
    struct Ask {
        bool bound{ false };          // the player has assigned a key
        bool editorOpen{ false };     // the editor window is up
        bool hostOpen{ false };       // HostGuard::HostMenuOpen()
        bool loreOk{ true };          // LoreModule::GateSatisfied()
        bool typing{ false };         // a text field somewhere has the keyboard
        bool summonPending{ false };  // a host was already asked for
    };

    // ⚠ THE ORDER OF THESE ARMS IS THE BEHAVIOUR, and two of them are the
    // reason this is a function rather than an if-chain at the call site.
    //
    // ⚠⚠ UNBOUND COMES FIRST AND ANSWERS kIgnore. The key ships unassigned, so
    // until the player binds it every arm below is unreachable. The input sink
    // will not even call this with an unbound key, and that is exactly why the
    // arm is here: a rule that only holds because of who calls it is a rule
    // that stops holding the moment somebody else calls it.
    //
    // ⚠⚠ THE CLOSE EDGE OUTRANKS EVERY REFUSAL BELOW IT. A player who is INSIDE
    // the editor must be able to leave with the key that opened it, whatever
    // the lore gate now says: a Seamstone dropped, stolen or sold while the
    // editor is open would otherwise lock the door from the inside. The
    // existing editor hotkey learned this in EditorGate and it holds here.
    [[nodiscard]] constexpr Entry Decide(const Ask& a_ask) {
        if (!a_ask.bound) {
            return Entry::kIgnore;
        }
        if (a_ask.editorOpen) {
            return Entry::kCloseEditor;
        }
        // ⚠ TYPING BEATS OPENING AND NOT CLOSING, for the reason the editor
        // hotkey documents at length: a bind can be a printable letter, and a
        // name being typed into Screen Archer Menu or a console line is a
        // keystroke that belongs to whoever asked for it.
        if (a_ask.typing) {
            return Entry::kIgnore;
        }
        if (!a_ask.loreOk) {
            return Entry::kRefusedLore;
        }
        if (a_ask.hostOpen) {
            return Entry::kOpenHere;
        }
        // ⚠ A SECOND PRESS WHILE THE INVENTORY IS ON ITS WAY MUST NOT ASK AGAIN.
        // The menu arrives a frame or two later and the open rides its event, so
        // a double tap would queue two shows and, worse, leave the second one
        // pending after the first has been consumed.
        if (a_ask.summonPending) {
            return Entry::kIgnore;
        }
        return Entry::kSummonHost;
    }

    // The runtime half.
    //
    // ⚠⚠ THE BINDING ITSELF IS NOT HERE, AND IT WAS FOR ONE BUILD. This shipped
    // as a FUCK ManagedHotkey so the key could be assigned with FLICK's own
    // binder, and in the field that binder never captured: clicking it started
    // its flashing bind state and stayed there through every key pressed
    // (2026-08-18). Completing a FUCK bind needs the plugin to pump
    // UpdateManagedHotkey from its own async-input hook, and a sidebar tool
    // exists only while it is being drawn. So the key is a DIK code in this
    // mod's own INI, out of the same curated dropdown as the editor hotkey and
    // the change-outfit hotkey (Settings::directEntryKeyDIK), the input sink
    // compares it beside those two, and this module is left with the part that
    // was always its own: what a press MEANS.

    // Register the menu sink that opens the editor once a summoned host is up.
    void Register();

    // One press, already matched by the input sink.
    void Fire();

    // The editor closed. Dismiss the host if this module is the one that
    // summoned it, so the key is a round trip rather than a one-way door.
    void OnEditorClosed();

    // The Seamstone's world use (OS-38) asks for the same thing this key does,
    // so it asks THIS, and the summoned host is cleaned up the same way.
    void SummonHostAndOpen();

    // Is a summoned host still on its way?
    [[nodiscard]] bool SummonPending();

}  // namespace OS::DirectEntry
