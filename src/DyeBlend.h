#pragma once

// Which arithmetic a DYE asks for, kept pure so it is exercised.
//
// ⚠ NO ENGINE TYPES IN THIS HEADER, the rule DyeRamp.h, TutorialPlan.h and
// EditorGate.h all follow and for the same reason: no test compiles
// OutfitDye.cpp or EditorUI.cpp, so a decision left in either reaches the field
// unexercised.
//
// ---- WHY THIS IS A CHOICE AND NOT A DyeTexture::Blend --------------------
//
// ⚠⚠ ZERO MEANS "WHAT THE INSTALL SAYS", NOT "SOFT LIGHT", AND THAT IS THE
// WHOLE DESIGN. The blend shipped on 2026-08-13 as one global [Dye] sDyeBlend
// key, so every dye in every save already renders through whatever that key
// resolves to. Storing an absolute blend here would make a record written
// before this feature decode to soft light and OVERRIDE that key, restyling
// every dye on an install that had chosen multiply. Storing a choice whose zero
// defers leaves them all exactly as they are, which is the rule the whole dye
// struct is built on: every default is today's behaviour.
//
// ⚠⚠ AND IT IS WHAT LETS THE EYE HAVE A DIFFERENT DEFAULT FROM ARMOUR AT ALL.
// The eye's fallback is its own (Overlay, a constant in PaintEyeTint, user call
// 2026-08-13), the armour's is sDyeBlend, and one byte says "do not choose for
// me" to both. A stored value that named a curve could not do that: it would
// mean one thing on a garment and the same thing on an eye, and the two
// surfaces have never wanted the same answer.
//
// ⚠ THE EYE'S DEFAULT HAS MOVED ONCE ALREADY, from kRecolour to kOverlay, and
// the deferring zero is why that cost nothing but a rebuild. Every eye carrying
// zero followed it; every eye whose player had picked a curve kept the pick.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace OS::DyeBlend {

    // ⚠ THE ORDER IS THE WIRE, THE JSON AND THE STEPPER AT ONCE. The byte is
    // persisted, the name table below is indexed by it, and the editor row
    // casts it to an int, so an insertion anywhere but the end silently
    // repaints every dye already saved.
    enum class Choice : std::uint8_t {
        kDefault = 0,  // whatever the surface's own setting resolves to
        kSoftLight,
        kMultiply,
        kScreen,
        kOverlay,
        kColour,
        kLuminosity,
        // ⚠⚠ APPENDED PAST THE SIX, AND EYE-FACING ONLY. It is the curve an eye
        // renders through by default, so the eye tile has to be able to NAME it
        // once the row stops showing a "from settings" entry: without it a
        // player who stepped off the default could never step back onto it.
        //
        // ⚠ IT IS NOT IN THE ARMOUR TILE'S LIST and must not be. DyeTexture.h's
        // kSelectableBlendNames excludes it deliberately so an INI cannot point
        // the armour walk at arithmetic chosen by texture kind, and that
        // reasoning is untouched: this widens what a DYE may carry, not what
        // the install-wide key may spell.
        kRecolour,
    };

    inline constexpr std::size_t kChoiceCount = 8;

    // ⚠ AN UNKNOWN BYTE DEFERS, IT IS NOT THE LAST ENUMERATOR. Same rule as
    // DyeRamp::ModeFromByte: a record written by a later build carries a choice
    // this one has never heard of, and the safe reading of "I do not know what
    // this is" is the behaviour that shipped before choices existed.
    [[nodiscard]] constexpr Choice ChoiceFromByte(std::uint8_t a_raw) {
        return a_raw < static_cast<std::uint8_t>(kChoiceCount) ? static_cast<Choice>(a_raw)
                                                               : Choice::kDefault;
    }

    // The JSON and INI spelling of each choice, indexed by the byte.
    //
    // ⚠ THE SIX NAMES ARE DyeTexture.h's kSelectableBlendNames, CHARACTER FOR
    // CHARACTER, and a static_assert over there holds them together. A pack
    // author writing "multiply" on a dye and an INI writing "multiply" on the
    // install must not be able to mean two different curves.
    //
    // ⚠ kDefault's ENTRY IS EMPTY ON PURPOSE. There is no spelling for it: a
    // dye that defers simply omits the key, which is what every dye authored
    // before this existed already does.
    inline constexpr std::string_view kChoiceNames[]{
        "", "softlight", "multiply", "screen", "overlay", "colour", "luminosity",
        // ⚠ SPELLABLE SO A SAVED DYE SURVIVES A PACK ROUND TRIP. "Save as dye"
        // on the eye tile copies the channel whole, so an eye dye carrying the
        // recolour has to be writable and readable back or the dye changes
        // meaning between sessions. A pack author may therefore put it on an
        // armour dye, which renders the recolour on a diffuse: a real curve and
        // not normal-map arithmetic, so it is allowed rather than policed.
        "recolour",
    };

    static_assert(std::size(kChoiceNames) == kChoiceCount,
                  "a Choice exists with no spelling, so a dye could carry a blend that "
                  "cannot be written back to its own pack file");

    // ⚠ AN UNKNOWN SPELLING DEFERS, NEVER REFUSES. A dye pack written against a
    // later build should lose its blend, not its colour, which is the trade
    // DyeFromJson already makes for the mode and the rarity.
    [[nodiscard]] constexpr Choice ChoiceFromName(std::string_view a_name) {
        // ⚠⚠ BOTH SPELLINGS OF COLOUR, BECAUSE DyeTexture::BlendFromName TAKES
        // BOTH. The mod's prose is British and Photoshop's menu is American, so
        // that function has always accepted "color" as well. Missing the alias
        // here was a real divergence: sDyeBlend = color painted kColour while
        // the editor row resolved kDefault, fell to soft light and drew the
        // wrong curve under a deferring channel. The alias is here rather than
        // in kChoiceNames because that table is INDEXED BY THE BYTE and is what
        // NameForChoice writes back, so a second "color" entry would either
        // shift every stored value or give one choice two spellings on disk.
        if (a_name == "color") {
            return Choice::kColour;
        }
        for (std::size_t i = 1; i < kChoiceCount; ++i) {
            if (kChoiceNames[i] == a_name) {
                return static_cast<Choice>(i);
            }
        }
        return Choice::kDefault;
    }

    // Empty for kDefault, which is the signal to write no key at all.
    [[nodiscard]] constexpr std::string_view NameForChoice(Choice a_choice) {
        const auto i = static_cast<std::size_t>(a_choice);
        return i < kChoiceCount ? kChoiceNames[i] : std::string_view{};
    }

    // What an INI spelling of the install-wide [Dye] sDyeBlend resolves to, as
    // a Choice.
    //
    // ⚠⚠ THIS EXISTS TO STOP TWO ANSWERS DRIFTING APART. The editor has to show
    // the player what a DEFERRING channel actually renders, and the paint walk
    // asks DyeTexture::BlendFromName the same question. That function resolves
    // an unknown spelling to soft light, so this must too: if it answered
    // kDefault for a typo the row would draw a blank while the garment painted
    // soft light. Never returns kDefault, which is the property the row relies
    // on to always find itself in the list.
    //
    // ⚠⚠ kRecolour FALLS TO SOFT LIGHT HERE, AND THAT IS THE WHOLE POINT OF THE
    // SECOND ARM. BlendFromName has never accepted "recolour" and must not
    // start: an INI naming it would point the ARMOUR walk at a curve chosen by
    // texture kind. A dye may carry the recolour, the install-wide key may not,
    // so `sDyeBlend = recolour` paints soft light and this says the same word.
    [[nodiscard]] constexpr Choice ChoiceFromIniName(std::string_view a_name) {
        const auto c = ChoiceFromName(a_name);
        return c == Choice::kDefault || c == Choice::kRecolour ? Choice::kSoftLight : c;
    }

}  // namespace OS::DyeBlend
