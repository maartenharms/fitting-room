#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace OS::EditorGate {

    // What the editor hotkey should do this press.
    enum class GateAction { kIgnore, kClose, kOpen, kNeedContext, kNeedSeamstone };

    // Pure decision for the editor hotkey.
    //   isOpen       - the editor is currently open
    //   wantsText    - an ImGui text field has focus (typing the hotkey letter
    //                  must not close the editor)
    //   canOpenHere  - a permitted context is open (inventory OR Screen Archer Menu)
    //   seamstoneOk  - the lore gate is satisfied (no requirement, or stone held)
    [[nodiscard]] inline GateAction DecideGate(bool isOpen, bool wantsText,
                                               bool canOpenHere, bool seamstoneOk) {
        if (isOpen) {
            return wantsText ? GateAction::kIgnore : GateAction::kClose;
        }
        if (!canOpenHere) {
            return GateAction::kNeedContext;
        }
        if (!seamstoneOk) {
            return GateAction::kNeedSeamstone;
        }
        return GateAction::kOpen;
    }

    // A Fitting Room window opened over SAM cannot outlive that host menu.
    // World-hover passthrough deliberately lets SAM receive Escape, so its
    // close event must close the editor as the same transaction.
    [[nodiscard]] inline constexpr bool ShouldCloseForLostHost(
        bool a_openedFromSam, bool a_samStillOpen) {
        return a_openedFromSam && !a_samStillOpen;
    }

    // A showcase preset may become a saved outfit only when there is a valid
    // selection, the library has room, and lore-friendly mode's collection
    // gate is satisfied. Free-form mode deliberately ignores ownership.
    [[nodiscard]] inline bool CanSaveShowcase(bool a_haveSelection, bool a_libraryFull,
                                              bool a_collectionOnly,
                                              bool a_ownsEveryPiece) {
        return a_haveSelection && !a_libraryFull &&
               (!a_collectionOnly || a_ownsEveryPiece);
    }

    // Playstyle owns style visibility completely: free-form sees every
    // installed look, lore-friendly sees collected looks. There is no
    // session/settings override that can put either mode into a contradictory
    // state.
    [[nodiscard]] inline constexpr bool BrowseCollectedOnly(
        bool a_loreFriendly) {
        return a_loreFriendly;
    }

    // Preset import writes into the CURRENT target library. Naming that target
    // on the action prevents "my outfits" from implying the player while a
    // follower is selected.
    [[nodiscard]] inline constexpr const char* PresetSaveLabel(
        bool a_targetIsPlayer) {
        return a_targetIsPlayer ? "Save to player outfits"
                                : "Save to follower outfits";
    }

    // A readable name for a DirectInput scan code (DIK), for the rebind display.
    // Covers the keys anyone would bind; unlisted codes fall back to "Key 0xNN".
    // 0 = unbound.
    [[nodiscard]] inline std::string DikName(std::uint32_t a_dik) {
        switch (a_dik) {
            case 0x00: return "(unbound)";
            case 0x01: return "Esc";
            case 0x0E: return "Backspace";
            case 0x0F: return "Tab";
            case 0x1C: return "Enter";
            case 0x1D: return "L-Ctrl";
            case 0x2A: return "L-Shift";
            case 0x38: return "L-Alt";
            case 0x39: return "Space";
            case 0x1E: return "A"; case 0x30: return "B"; case 0x2E: return "C";
            case 0x20: return "D"; case 0x12: return "E"; case 0x21: return "F";
            case 0x22: return "G"; case 0x23: return "H"; case 0x17: return "I";
            case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
            case 0x32: return "M"; case 0x31: return "N"; case 0x18: return "O";
            case 0x19: return "P"; case 0x10: return "Q"; case 0x13: return "R";
            case 0x1F: return "S"; case 0x14: return "T"; case 0x16: return "U";
            case 0x2F: return "V"; case 0x11: return "W"; case 0x2D: return "X";
            case 0x15: return "Y"; case 0x2C: return "Z";
            case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
            case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
            case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x3B: return "F1"; case 0x3C: return "F2"; case 0x3D: return "F3";
            case 0x3E: return "F4"; case 0x3F: return "F5"; case 0x40: return "F6";
            case 0x41: return "F7"; case 0x42: return "F8"; case 0x43: return "F9";
            case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
            default: {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "Key 0x%X", a_dik);
                return std::string(buf);
            }
        }
    }

    // Keys offered in the settings-panel hotkey dropdown (DIK, name). Names
    // match DikName. A dropdown avoids raw key capture, which fails while the
    // SKSE Menu Framework overlay owns the keyboard.
    struct KeyOption {
        std::uint32_t dik;
        const char*   name;
    };
    inline constexpr KeyOption kBindableKeys[] = {
        { 0x00, "(unbound)" },
        { 0x18, "O" }, { 0x19, "P" }, { 0x25, "K" }, { 0x26, "L" },
        { 0x24, "J" }, { 0x23, "H" }, { 0x16, "U" }, { 0x17, "I" },
        { 0x15, "Y" }, { 0x31, "N" }, { 0x30, "B" }, { 0x2F, "V" },
        { 0x22, "G" }, { 0x21, "F" }, { 0x14, "T" }, { 0x13, "R" },
        { 0x2E, "C" }, { 0x10, "Q" }, { 0x12, "E" },
        { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
        { 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" },
        { 0x43, "F9" }, { 0x44, "F10" }, { 0x57, "F11" }, { 0x58, "F12" },
    };

}  // namespace OS::EditorGate
