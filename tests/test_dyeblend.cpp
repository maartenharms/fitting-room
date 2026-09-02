// The per-dye blend choice. No SKSE, no engine - DyeBlend.h is pure by the
// same rule DyeRamp.h is: no test compiles OutfitDye.cpp or EditorUI.cpp.
#include "DyeBlend.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::DyeBlend;

    {  // ⚠⚠ THE ONE THAT MATTERS. Zero DEFERS, so a channel decoded from a v18
       // record - and every one of the 318 shipped dyes, none of which name a
       // blend - renders through whatever the install already resolves. A zero
       // that meant soft light would override sDyeBlend on every install that
       // had chosen otherwise, and would drag the field-confirmed iris off
       // kRecolour on load.
        CHECK(ChoiceFromByte(0) == Choice::kDefault);
        CHECK(Choice{} == Choice::kDefault);
        CHECK(static_cast<std::uint8_t>(Choice::kDefault) == 0);
    }

    {  // The byte is the wire, the JSON index and the stepper position at once,
       // so the order is pinned rather than left to the enumerator list.
        CHECK(static_cast<std::uint8_t>(Choice::kSoftLight) == 1);
        CHECK(static_cast<std::uint8_t>(Choice::kMultiply) == 2);
        CHECK(static_cast<std::uint8_t>(Choice::kScreen) == 3);
        CHECK(static_cast<std::uint8_t>(Choice::kOverlay) == 4);
        CHECK(static_cast<std::uint8_t>(Choice::kColour) == 5);
        CHECK(static_cast<std::uint8_t>(Choice::kLuminosity) == 6);
        // ⚠ APPENDED PAST THE SIX so no byte already written changes meaning.
        CHECK(static_cast<std::uint8_t>(Choice::kRecolour) == 7);
        CHECK(kChoiceCount == 8);
    }

    {  // ⚠ AN UNKNOWN BYTE DEFERS, IT IS NOT THE LAST ENUMERATOR. A record
       // written by a later build carries a choice this one has never heard of,
       // and the safe reading of that is the behaviour that shipped before
       // choices existed. Same rule as DyeRamp::ModeFromByte.
        CHECK(ChoiceFromByte(8) == Choice::kDefault);
        CHECK(ChoiceFromByte(99) == Choice::kDefault);
        CHECK(ChoiceFromByte(255) == Choice::kDefault);
    }

    {  // Every real choice survives the byte, in both directions.
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(kChoiceCount); ++i) {
            CHECK(static_cast<std::uint8_t>(ChoiceFromByte(i)) == i);
        }
    }

    {  // The spellings are the pack file's and the INI's, character for
       // character. A pack author writing "multiply" on a dye and an INI
       // writing "multiply" on the install must mean one curve.
        CHECK(ChoiceFromName("softlight") == Choice::kSoftLight);
        CHECK(ChoiceFromName("multiply") == Choice::kMultiply);
        CHECK(ChoiceFromName("screen") == Choice::kScreen);
        CHECK(ChoiceFromName("overlay") == Choice::kOverlay);
        CHECK(ChoiceFromName("colour") == Choice::kColour);
        CHECK(ChoiceFromName("luminosity") == Choice::kLuminosity);
        CHECK(ChoiceFromName("recolour") == Choice::kRecolour);
    }

    {  // ⚠⚠ THE INI CANNOT SPELL RECOLOUR, AND THIS IS THE ARM THAT HOLDS IT.
       // DyeTexture::BlendFromName has never accepted "recolour" and must not
       // start: an install-wide key naming it would point the ARMOUR walk at a
       // curve chosen by texture kind. A dye may carry it, sDyeBlend may not,
       // so both functions have to answer soft light for that word or the
       // editor would draw one curve while the garment painted another.
        CHECK(ChoiceFromIniName("recolour") == Choice::kSoftLight);
        // Every unknown spelling lands there too, matching BlendFromName's own
        // rule, and the row relies on this never returning kDefault: a value
        // with no entry in the list cannot be drawn.
        CHECK(ChoiceFromIniName("") == Choice::kSoftLight);
        CHECK(ChoiceFromIniName("hardlight") == Choice::kSoftLight);
        CHECK(ChoiceFromIniName("softlight") == Choice::kSoftLight);
        CHECK(ChoiceFromIniName("multiply") == Choice::kMultiply);
        CHECK(ChoiceFromIniName("luminosity") == Choice::kLuminosity);
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(kChoiceCount); ++i) {
            CHECK(ChoiceFromIniName(kChoiceNames[i]) != Choice::kDefault);
        }
    }

    {  // ⚠ AN UNKNOWN SPELLING DEFERS, NEVER REFUSES. A pack written against a
       // later build loses its blend, not its colour, which is the trade
       // DyeFromJson already makes for the mode and the rarity. A typo
       // therefore renders exactly as a dye with no key at all.
        CHECK(ChoiceFromName("softLight") == Choice::kDefault);   // case matters
        CHECK(ChoiceFromName("soft light") == Choice::kDefault);
        CHECK(ChoiceFromName("hardlight") == Choice::kDefault);
        CHECK(ChoiceFromName("") == Choice::kDefault);
    }

    {  // ⚠⚠ BOTH SPELLINGS OF COLOUR, AND THIS CASE ONCE ASSERTED THE BUG.
       // DyeTexture::BlendFromName has always taken "color" as well as
       // "colour", because the mod's prose is British and Photoshop's menu is
       // American. This function missing the alias was a real divergence: an
       // INI spelling sDyeBlend = color painted kColour while the editor row
       // resolved kDefault, fell to soft light, and drew a curve the garment
       // was not using. A test that pinned the wrong answer is worse than no
       // test, because it defends the defect from the next reader.
        CHECK(ChoiceFromName("color") == Choice::kColour);
        CHECK(ChoiceFromName("colour") == Choice::kColour);
        CHECK(ChoiceFromIniName("color") == Choice::kColour);
        CHECK(ChoiceFromIniName("colour") == Choice::kColour);
        // ⚠ AND THE WRITER STILL SPELLS IT ONE WAY. The name table is indexed
        // by the stored byte, so the alias may never reach it or one choice
        // would have two spellings on disk.
        CHECK(NameForChoice(Choice::kColour) == "colour");
    }

    {  // ⚠ kDefault HAS NO SPELLING, and that is what tells the writer to omit
       // the key entirely rather than to write "default" into a pack file. A
       // dye that defers must look, on disk, exactly like every dye authored
       // before this existed.
        CHECK(NameForChoice(Choice::kDefault).empty());
        CHECK(NameForChoice(Choice::kOverlay) == "overlay");
        // And an out-of-range byte cast in cannot read past the table.
        CHECK(NameForChoice(static_cast<Choice>(200)).empty());
    }

    {  // Round trip through the name, which is what a pack file save and reload
       // does. kDefault is excluded because it has no name by construction.
        for (std::uint8_t i = 1; i < static_cast<std::uint8_t>(kChoiceCount); ++i) {
            const auto c = static_cast<Choice>(i);
            CHECK(ChoiceFromName(NameForChoice(c)) == c);
        }
    }

    if (g_failures == 0) {
        std::printf("test_dyeblend: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
