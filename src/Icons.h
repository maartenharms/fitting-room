#pragma once

#include <cstdint>
#include <string>

// Font Awesome 5 Free Solid glyphs, merged into the editor body font by
// EditorStyle (icons.ttf, SIL OFL). Used for the slot list (symbols instead
// of numbers) and the Columns gear. Every codepoint used anywhere must appear
// in kAll so EditorStyle bakes it into the atlas.
//
// VERIFY new codepoints against the BUNDLED font (dist/icons.ttf =
// fontawesome5-solid-webfont-5.15.4), NOT a generic FA5 cheatsheet: the free
// solid set lacks Pro-only glyphs and places some icons at different
// codepoints (e.g. `vest` is e085 here, not the Pro f8dd - that mismatch
// rendered chest as "?", OS-35). All 28 below audited present (the newest,
// `coins` 0xf51e, verified in the bundled cmap 2026-07-15).
//
// Under FLICK the glyphs render from FUCK's own baked fa-solid atlas (not this
// ttf, which only feeds the dormant ImGuiOverlay fallback), but `coins` is a
// stable free-solid codepoint present in both, so it renders on either path.
namespace OS::Icons {

    inline constexpr std::uint16_t kGear     = 0xf013;  // cog - the Columns config button
    inline constexpr std::uint16_t kMask     = 0xf6fa;  // head
    inline constexpr std::uint16_t kHelmet   = 0xf807;  // hard-hat - hair / helmet
    inline constexpr std::uint16_t kBody     = 0xf553;  // tshirt - body
    inline constexpr std::uint16_t kMitten   = 0xf7b5;  // hands
    inline constexpr std::uint16_t kHand     = 0xf255;  // hand-rock - forearms / arms
    inline constexpr std::uint16_t kGem      = 0xf3a5;  // amulet
    inline constexpr std::uint16_t kRing     = 0xf70b;  // ring
    inline constexpr std::uint16_t kShoe     = 0xf54b;  // shoe-prints - feet
    inline constexpr std::uint16_t kSocks    = 0xf696;  // calves
    inline constexpr std::uint16_t kFeather  = 0xf56b;  // feather-alt - tail
    inline constexpr std::uint16_t kUser     = 0xf007;  // long hair
    inline constexpr std::uint16_t kCrown    = 0xf521;  // circlet
    inline constexpr std::uint16_t kEar      = 0xf2a2;  // assistive-listening - ears
    inline constexpr std::uint16_t kSmile    = 0xf118;  // face / mouth
    inline constexpr std::uint16_t kVest     = 0xe085;  // vest - chest (outer). NB free-solid
                                                        // ships vest at e085; the Pro codepoint
                                                        // f8dd is absent and rendered as "?".
    inline constexpr std::uint16_t kBack     = 0xf6ec;  // hiking - back
    inline constexpr std::uint16_t kSkull    = 0xf54c;  // decapitated head
    inline constexpr std::uint16_t kSkullX   = 0xf714;  // decapitate
    inline constexpr std::uint16_t kMagic    = 0xf0d0;  // FX
    inline constexpr std::uint16_t kCube     = 0xf1b2;  // generic / rare slots
    inline constexpr std::uint16_t kRealGear = 0xf2ea;  // undo-alt - revert a slot to worn gear
    inline constexpr std::uint16_t kDice     = 0xf522;  // Random button
    inline constexpr std::uint16_t kStar     = 0xf005;  // favorite toggle (gold = favorited)
    inline constexpr std::uint16_t kUndo     = 0xf0e2;  // undo edit
    inline constexpr std::uint16_t kRedo     = 0xf01e;  // redo edit
    inline constexpr std::uint16_t kSearch   = 0xf002;  // magnifier - the shared search bar
    inline constexpr std::uint16_t kTimes    = 0xf00d;  // X - clear a slot to equipped gear
    inline constexpr std::uint16_t kTrash    = 0xf1f8;  // trash - delete the whole outfit
    inline constexpr std::uint16_t kCoins    = 0xf51e;  // stack of coins - the Apply gold cost

    // --- Weapons accordion (Task 8, weapon+quiver transmog) ----------------
    // FA5 Free Solid has no dedicated icon for most melee weapon classes
    // (sword/dagger/axe/mace/greatsword are Pro-only or simply absent from
    // the free set) - per the OS-35/OS-62 lesson, shipping a guessed
    // codepoint that isn't actually in the bundled font renders as tofu
    // ("?"), so those rows REUSE already-audited glyphs above instead of a
    // new one (see the kWeaponIcon table in EditorUI.cpp for which class
    // reuses which, and why). The five below are NEW codepoints, picked for
    // classes with a strong, well-known free-solid match - UNLIKE the 30
    // above they have NOT been screenshot-verified against the bundled
    // 5.15.4 atlas yet; that is the pending verification pass the design
    // spec calls out (Task 8 §7, [USER-CHECK]: "veto by screenshot"). If one
    // renders as tofu, swap it for a reuse like its melee siblings.
    inline constexpr std::uint16_t kBolt          = 0xf0e7;  // bolt - Bolts (literal pun; very common icon)
    inline constexpr std::uint16_t kBullseye      = 0xf140;  // bullseye - Bow (archery-target pun)
    inline constexpr std::uint16_t kCrosshairs    = 0xf05b;  // crosshairs - Crossbow (precision-aim pun)
    inline constexpr std::uint16_t kLocationArrow = 0xf124;  // location-arrow - Arrows (arrow-shape pun)
    inline constexpr std::uint16_t kHammer        = 0xf6e3;  // hammer - Battleaxe/Warhammer (name match)

    inline constexpr std::uint16_t kAll[] = {
        kGear, kMask, kHelmet, kBody, kMitten, kHand, kGem, kRing, kShoe, kSocks, kFeather, kUser,
        kCrown, kEar, kSmile, kVest, kBack, kSkull, kSkullX, kMagic, kCube, kRealGear, kDice,
        kStar, kUndo, kRedo, kSearch, kTimes, kTrash, kCoins,
        kBolt, kBullseye, kCrosshairs, kLocationArrow, kHammer,
    };

    // UTF-8 encode a BMP codepoint (FA glyphs are all 3-byte 0x800-0xFFFF).
    [[nodiscard]] inline std::string Utf8(std::uint16_t a_cp) {
        std::string s;
        if (a_cp < 0x80) {
            s += static_cast<char>(a_cp);
        } else if (a_cp < 0x800) {
            s += static_cast<char>(0xC0 | (a_cp >> 6));
            s += static_cast<char>(0x80 | (a_cp & 0x3F));
        } else {
            s += static_cast<char>(0xE0 | (a_cp >> 12));
            s += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (a_cp & 0x3F));
        }
        return s;
    }

}  // namespace OS::Icons
