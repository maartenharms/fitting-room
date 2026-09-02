#include "../src/OverlayPlan.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// The glow slider's travel is a square curve, so its round trip is exact at the
// ends and floating point everywhere between. A tolerance is the honest test
// there; an equality check would pin the compiler's rounding rather than the
// behaviour.
[[nodiscard]] inline bool Near(float a_lhs, float a_rhs, float a_epsilon = 0.001f) {
    return std::fabs(a_lhs - a_rhs) <= a_epsilon;
}

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

int main() {
    using namespace OS::OverlayPlan;

    {  // ⚠⚠ THE FAULT THIS WHOLE FILE EXISTS FOR. skee's formats are
        // std::format templates, so the placeholder is "{}". A printf style
        // substitution leaves the braces, the node lookup misses, and the page
        // silently does nothing on every layer.
        CHECK(NodeName("Body [Ovl{}]", 0) == "Body [Ovl0]");
        CHECK(NodeName("Body [Ovl{}]", 5) == "Body [Ovl5]");
        CHECK(NodeName("Face [SOvl{}]", 2) == "Face [SOvl2]");
        // A format with no placeholder yields nothing rather than one name
        // shared by every layer, which would have the page write all six body
        // slots onto whichever node happened to match.
        CHECK(NodeName("Body [Ovl%d]", 0).empty());
        CHECK(NodeName("", 0).empty());
    }
    {  // The node names measured on the reference rig, straight out of skee's
        // own defines.
        CHECK(NodeName("Hands [Ovl{}]", 1) == "Hands [Ovl1]");
        CHECK(NodeName("Feet [Ovl{}]", 2) == "Feet [Ovl2]");
    }
    {  // ⚠ SIX BODY OVERLAYS ON THIS RIG, NOT THREE. The counts come from the
        // user's skee64.ini through GetOverlayCount, and a list built from a
        // constant loses the slots above it without any sign that it has.
        const Formats formats{ "Face [Ovl{}]", "Body [Ovl{}]", "Hands [Ovl{}]",
                               "Feet [Ovl{}]" };
        const Counts  counts{ 3, 6, 3, 3 };
        const auto    layers = BuildLayers(formats, counts);
        CHECK(layers.size() == 15);
        // Face leads, because that is the page's order and not skee's.
        CHECK(layers.front().location == Location::kFace);
        CHECK(layers.front().node == "Face [Ovl0]");
        CHECK(layers[3].location == Location::kBody);
        CHECK(layers[3].node == "Body [Ovl0]");
        CHECK(layers[8].node == "Body [Ovl5]");
        CHECK(layers.back().node == "Feet [Ovl2]");
    }
    {  // A location whose count is zero contributes nothing. Face ships with no
        // spell overlays on the reference rig, so this is a real configuration
        // and not a defensive case.
        const Formats formats{ "Face [SOvl{}]", "Body [SOvl{}]", "Hands [SOvl{}]",
                               "Feet [SOvl{}]" };
        const Counts  counts{ 0, 1, 1, 1 };
        CHECK(BuildLayers(formats, counts).size() == 3);
    }
    {  // An unusable format drops its own location and leaves the rest alone,
        // rather than taking the page down with it.
        const Formats formats{ "Face [Ovl%d]", "Body [Ovl{}]", "Hands [Ovl{}]",
                               "Feet [Ovl{}]" };
        const Counts  counts{ 3, 2, 1, 1 };
        const auto    layers = BuildLayers(formats, counts);
        CHECK(layers.size() == 4);
        CHECK(layers.front().location == Location::kBody);
    }
    {  // Only the texture key carries an index. The other two have theirs
        // overwritten inside skee, which is why every preset shows -1 there.
        CHECK(KeyTakesIndex(kKeyTexture));
        CHECK(!KeyTakesIndex(kKeyTint));
        CHECK(!KeyTakesIndex(kKeyAlpha));
    }
    {  // ⚠⚠ THE ASYMMETRY THAT WOULD HAVE COST A FIELD ROUND. skee normalises
        // an unindexed key's index to 0xFF when it stores the override, and
        // does NOT normalise it when it looks one up. Writing a tint at index 0
        // therefore stores it at -1, and reading or clearing at index 0 misses:
        // the colour appears on the character, the page shows none, and the
        // clear button does nothing. One value has to be used for all three
        // operations, and 0xFF is the one the store already holds.
        CHECK(IndexFor(kKeyTint, 0) == kIndexNone);
        CHECK(IndexFor(kKeyAlpha, 0) == kIndexNone);
        CHECK(IndexFor(kKeyTint, 7) == kIndexNone);  // the slot is ignored outright
        // The texture key keeps whichever slot it was given, which is the whole
        // reason a normal is reachable at all.
        CHECK(IndexFor(kKeyTexture, kSlotDiffuse) == 0);
        CHECK(IndexFor(kKeyTexture, kSlotNormal) == 1);
    }
    {  // The three numbers, pinned. These are what RaceMenu wrote into 331
        // presets on disk; changing one changes what we read back out of them.
        CHECK(kKeyTint == 7);
        CHECK(kKeyAlpha == 8);
        CHECK(kKeyTexture == 9);
        CHECK(kSlotDiffuse == 0);
        CHECK(kSlotNormal == 1);
    }
    {  // Tint packing round-trips, and the top byte never becomes a red
        // channel on the way back. It IS the alpha on the way out.
        CHECK(PackTint(0xC9, 0xA0, 0xD8, 0xFF) == 0xFFC9A0D8u);
        CHECK(UnpackTint(0x00C9A0D8u) == (Rgb{ 0xC9, 0xA0, 0xD8 }));
        CHECK(UnpackTint(0xFFC9A0D8u) == (Rgb{ 0xC9, 0xA0, 0xD8 }));
        CHECK(UnpackTint(PackTint(1, 2, 3)) == (Rgb{ 1, 2, 3 }));

        // ⚠⚠ AN OPAQUE COLOUR IS THE DEFAULT, AND BLACK IS NOT ZERO. The
        // shipped build packed 0x00RRGGBB, so black went to skee as the
        // integer 0 and the field could not reach it. Fully opaque black is
        // 0xFF000000, which is what 32 preset layers on disk hold.
        CHECK(PackTint(0, 0, 0) == 0xFF000000u);
        CHECK(PackTint(0, 0, 0, 0) == 0x00000000u);

        // The alpha byte is what RaceMenu's own presets store beside key 8.
        // Orchi's 0.294 alpha sits at 75, Dalen's 0.769 at 196, Maelle's
        // 0.616 at 157: rounded, not truncated.
        CHECK(AlphaByte(0.0f) == 0);
        CHECK(AlphaByte(1.0f) == 255);
        CHECK(AlphaByte(0.294117659f) == 75);
        CHECK(AlphaByte(0.768627465f) == 196);
        CHECK(AlphaByte(0.615686297f) == 157);
        // Out of range cannot wrap the byte round to the far end.
        CHECK(AlphaByte(-1.0f) == 0);
        CHECK(AlphaByte(2.0f) == 255);
        // ⚠ AND THE PAIR AGREES. This is the invariant the field bug broke:
        // the packed top byte and the float under key 8 are one value.
        CHECK((PackTint(0x20, 0x40, 0x60, AlphaByte(0.5f)) >> 24) == AlphaByte(0.5f));
    }
    {
        CHECK(ClampAlpha(-0.5f) == 0.0f);
        CHECK(ClampAlpha(1.5f) == 1.0f);
        CHECK(ClampAlpha(0.25f) == 0.25f);
    }
    {  // ⚠⚠ THE GLOW STRENGTH IS THE GLOW COLOUR'S ALPHA BYTE OVER TEN, and
        // that is RaceMenu's arithmetic rather than a scale of our choosing.
        // Its OnOverlayGlowColorChange does `alpha / 10.0` on the top byte of
        // the colour it writes to key 0, which is why every multiple in the
        // installed presets is a tenth and none exceeds 25.5.
        // ⚠ THE WIRE STILL REACHES 25.5 AND THE SLIDER STOPS AT 1.0. The
        // packing has to represent what RaceMenu wrote, or a preset read and
        // rewritten here would come back changed by the arithmetic rather than
        // by the player.
        CHECK(kGlowStrengthWireMax == 25.5f);
        CHECK(kGlowStrengthMax == 25.5f);
        CHECK(GlowAlphaByte(0.0f) == 0);
        CHECK(GlowAlphaByte(25.5f) == 255);
        CHECK(GlowAlphaByte(4.0f) == 40);
        // The values the presets actually hold, rounded rather than truncated.
        CHECK(GlowAlphaByte(2.9f) == 29);
        CHECK(GlowAlphaByte(4.4f) == 44);
        CHECK(GlowAlphaByte(12.4f) == 124);
        // ⚠ OUT OF RANGE CANNOT WRAP THE BYTE ROUND TO THE FAR END, which for
        // a glow would turn "far too bright" into "off" without a word.
        CHECK(GlowAlphaByte(-1.0f) == 0);
        CHECK(GlowAlphaByte(100.0f) == 255);

        CHECK(ClampGlow(-1.0f) == 0.0f);
        CHECK(ClampGlow(30.0f) == kGlowStrengthMax);
        CHECK(ClampGlow(0.4f) == 0.4f);
        // A glow RaceMenu wrote is inside our range now, so it survives being
        // read and rewritten unchanged.
        CHECK(ClampGlow(4.4f) == 4.4f);

        // Round trip on the WIRE range, which is what lets a layer RaceMenu
        // wrote read back without the packing corrupting it.
        for (const float strength : { 0.0f, 0.1f, 1.0f, 4.0f, 12.4f, 25.5f }) {
            CHECK(GlowStrengthFromByte(GlowAlphaByte(strength)) == strength);
        }

        // ⚠⚠ THE SLIDER'S TRAVEL IS SQUARED, and the numbers below are the
        // reason it is worth the trouble: a linear slider to 25.5 puts every
        // subtle value inside the first half percent of its length, which is
        // what the field complained about. Ends exact, middle curved.
        CHECK(GlowFromPercent(0.0f) == 0.0f);
        CHECK(GlowFromPercent(100.0f) == kGlowStrengthMax);
        CHECK(Near(GlowFromPercent(10.0f), 0.255f));
        CHECK(Near(GlowFromPercent(20.0f), 1.02f));
        CHECK(Near(GlowFromPercent(50.0f), 6.375f));
        CHECK(GlowPercent(0.0f) == 0.0f);
        CHECK(Near(GlowPercent(kGlowStrengthMax), 100.0f));
        CHECK(Near(GlowPercent(6.375f), 50.0f));
        // Out of range in either direction lands on an end, never wraps.
        CHECK(GlowFromPercent(-10.0f) == 0.0f);
        CHECK(GlowFromPercent(200.0f) == kGlowStrengthMax);
        // ⚠ THE ROUND TRIP HOLDS, or a drag would drift the value it started
        // from every frame it did not move.
        for (const float percent : { 0.0f, 10.0f, 25.0f, 50.0f, 100.0f }) {
            CHECK(Near(GlowPercent(GlowFromPercent(percent)), percent));
        }

        // The pair as it goes on the wire: colour in the low three bytes, the
        // strength in the top one.
        CHECK(PackTint(0x11, 0x22, 0x33, GlowAlphaByte(4.0f)) == 0x28112233u);
        CHECK((PackTint(0, 0, 0, GlowAlphaByte(0.0f))) == 0x00000000u);
    }
    {  // ⚠ A LAYER HOLDING THE DEFAULT TEXTURE IS EMPTY. skee writes it on
        // reset, so counting it as content would leave every reset slot reading
        // as used forever.
        CHECK(IsDefaultTexture("Actors\\Character\\Overlays\\Default.dds"));
        CHECK(IsDefaultTexture("actors\\character\\overlays\\default.dds"));
        CHECK(IsDefaultTexture("Actors/Character/Overlays/Default.dds"));
        CHECK(!IsDefaultTexture("Actors\\Character\\Overlays\\Freckles.dds"));
        CHECK(!IsDefaultTexture(""));

        LayerState state;
        CHECK(!Occupied(state));
        state.hasTexture = true;
        state.texture    = "Actors\\Character\\Overlays\\Default.dds";
        CHECK(!Occupied(state));
        state.texture = "Actors\\Character\\Overlays\\Community Overlays\\ink.dds";
        CHECK(Occupied(state));
    }
    {  // OS-209: what key 9 holds is the source, or its bake, and never a hash
        // for an identity transform. `texture` stays the source either way.
        LayerState state;
        CHECK(WirePath(state) == std::string{ kDefaultTexture });
        state.hasTexture = true;
        state.texture    = "Actors\\Character\\Overlays\\Pack\\ink.dds";
        CHECK(WirePath(state) == state.texture);
        state.transform.offsetX = 0.0004f;  // identity after quantising
        CHECK(WirePath(state) == state.texture);
        state.transform.offsetX = 0.25f;
        const auto wire = WirePath(state);
        CHECK(wire != state.texture);
        CHECK(OS::OverlayTransform::IsBakedPath(wire));
        CHECK(wire == OS::OverlayTransform::BakedPath(state.texture, state.transform));
        CHECK(Occupied(state));  // a moved layer is still an occupied layer
    }
    {  // ⚠ THE PATH IS RELATIVE TO textures\ AND NOT TO Data\. Leaving the
        // textures level on gives Data\textures\textures\..., which resolves to
        // the engine's placeholder with nothing logged.
        CHECK(ToOverridePath("textures\\Actors\\Character\\Overlays\\ink.dds") ==
              "Actors\\Character\\Overlays\\ink.dds");
        CHECK(ToOverridePath("Textures/Actors/Character/Overlays/ink.dds") ==
              "Actors\\Character\\Overlays\\ink.dds");
        CHECK(ToOverridePath("\\Actors\\Character\\ink.dds") == "Actors\\Character\\ink.dds");
        // A path that merely starts with the same letters keeps them.
        CHECK(ToOverridePath("texturesque\\a.dds") == "texturesque\\a.dds");
    }
    {  // ⚠⚠ THE FIELD BUG OF 2026-08-16. Under a mod manager the scanned file
        // does not live under the directory that was walked, so the relative
        // path walked back OUT of it and every picked texture was written to
        // the character as a path that could not resolve.
        CHECK(FromScanPath("..\\..\\mods\\Pretty Makeup - UBE - Racemenu Overlays\\"
                           "Textures\\Actors\\Character\\Overlays\\(SDZ21)\\Lips 1.dds") ==
              "Actors\\Character\\Overlays\\(SDZ21)\\Lips 1.dds");
        CHECK(FromScanPath("C:\\Games\\Skyrim\\Data\\textures\\actors\\character\\"
                           "overlays\\ink.dds") == "actors\\character\\overlays\\ink.dds");
        CHECK(FromScanPath("Data/textures/actors/character/overlays/ink.dds") ==
              "actors\\character\\overlays\\ink.dds");
        // Already relative to the textures root.
        CHECK(FromScanPath("textures\\actors\\ink.dds") == "actors\\ink.dds");
        // ⚠ THE LAST SEGMENT WINS, since a mod folder may itself be called
        // textures and the game path is always the deeper one.
        CHECK(FromScanPath("D:\\textures\\mymod\\textures\\actors\\ink.dds") ==
              "actors\\ink.dds");
        // Nothing usable is dropped rather than guessed at.
        CHECK(FromScanPath("C:\\somewhere\\else\\ink.dds").empty());
        CHECK(FromScanPath("").empty());
    }
    {  // The pack folder is what the picker groups by, and art sitting loose
        // at the top of the overlays directory belongs to no pack.
        CHECK(PackFolder("Actors\\Character\\Overlays\\Koralina\\freckles.dds") ==
              "Koralina");
        CHECK(PackFolder("actors\\character\\overlays\\ink.dds").empty());
        CHECK(PackFolder("Actors\\Character\\Overlays\\A\\B\\deep.dds") == "A");
        CHECK(PackFolder("Actors\\Character\\ink.dds").empty());
    }
    {
        CHECK(DisplayName("Actors\\Character\\Overlays\\Community\\ink.dds") == "ink");
        CHECK(DisplayName("ink.DDS") == "ink");
        CHECK(DisplayName("Actors/Character/ink.dds") == "ink");
        CHECK(DisplayName("") == "");
    }
    {  // ⚠ WITHOUT THIS THE PICKER OFFERS EVERY NORMAL MAP AS ART. Packs ship
        // the maps beside the diffuse under the same stem, so an unfiltered
        // scan doubles the list and half of it paints a blue normal map onto
        // the character's skin.
        CHECK(IsMapTexture("Actors\\Character\\Overlays\\ink_n.dds"));
        CHECK(IsMapTexture("Actors\\Character\\Overlays\\ink_msn.dds"));
        CHECK(IsMapTexture("ink_s.dds"));
        CHECK(IsMapTexture("ink_sk.DDS"));
        CHECK(IsMapTexture("INK_N.DDS"));
        CHECK(!IsMapTexture("Actors\\Character\\Overlays\\ink.dds"));
        // A stem that merely ends in the letter is not a map, and a folder
        // named with a suffix must not disqualify the file inside it.
        CHECK(!IsMapTexture("scars.dds"));
        CHECK(!IsMapTexture("Overlays_n\\ink.dds"));
        CHECK(!IsMapTexture("readme.txt"));
    }
    {  // The normal guess is a suggestion for a field the user can overwrite,
        // so it refuses anything that is not a .dds rather than inventing one.
        CHECK(GuessNormal("Actors\\Character\\Overlays\\ink.dds") ==
              "Actors\\Character\\Overlays\\ink_n.dds");
        CHECK(GuessNormal("ink.txt").empty());
        CHECK(GuessNormal(".dds").empty());
    }

    {  // ---- OS-226 S1, the finish ------------------------------------------
        //
        // ⚠⚠ THE KEY NUMBERS ARE WHAT RaceMenu SERIALISES INTO EVERY PRESET AND
        // CO-SAVE ON DISK. Recovered from OverrideVariant.h and corroborated
        // against the 331 installed presets, 15 of which carry each of these
        // two as an unindexed float. Changing either number here changes what
        // is read back out of files RaceMenu wrote.
        CHECK(kKeyGloss == 2);
        CHECK(kKeySpecular == 3);
        CHECK(!KeyTakesIndex(kKeyGloss));
        CHECK(!KeyTakesIndex(kKeySpecular));
        CHECK(IndexFor(kKeyGloss, 0) == kIndexNone);
        CHECK(IndexFor(kKeySpecular, 0) == kIndexNone);
    }
    {  // ⚠ THE DEFAULTS ARE A MEASUREMENT, NOT A TASTE. The 2026-08-18 probe
        // read specPower=30.0 and specScale=3.000 on all nine installed layers,
        // and the RaceMenu template parses to the same pair, so an untouched
        // layer already sits exactly here and the sliders open where it is.
        CHECK(Near(kDefaultGloss, 30.0f));
        CHECK(Near(kDefaultSpecular, 3.0f));
        // ⚠ AND A FRESH LAYER CARRIES NO FINISH. False here is what makes the
        // write REMOVE both keys, which is what lets the skin's own finish
        // through; a default of true would put 30 and 3 onto every layer on the
        // character the first time the page wrote one.
        const LayerState fresh;
        CHECK(!fresh.hasFinish);
        CHECK(Near(fresh.gloss, kDefaultGloss));
        CHECK(Near(fresh.specular, kDefaultSpecular));
    }
    {  // ⚠ THE CEILINGS HAVE TO REACH WHAT AUTHORS ALREADY WROTE. Scanning the
        // installed presets on 2026-08-18: key 2 holds 0, 5 and 500, key 3
        // holds 0, 5 and 10. A slider stopping short of 500 or 10 could not
        // reproduce a preset sitting on this disk.
        CHECK(Near(ClampGloss(500.0f), 500.0f));
        CHECK(Near(ClampGloss(5.0f), 5.0f));
        CHECK(Near(ClampSpecular(10.0f), 10.0f));
        CHECK(Near(ClampSpecular(5.0f), 5.0f));
        // And they are clamps, not assertions.
        CHECK(Near(ClampGloss(-1.0f), 0.0f));
        CHECK(Near(ClampGloss(9999.0f), kGlossMax));
        CHECK(Near(ClampSpecular(-1.0f), 0.0f));
        CHECK(Near(ClampSpecular(99.0f), kSpecularMax));
    }
    {  // The gloss curve, which exists so the useful end of a 0 to 500 range is
        // not crushed into the first tenth of the travel. Same shape as the
        // glow slider above, and the round trip is exact at the ends.
        CHECK(Near(GlossPercent(0.0f), 0.0f));
        CHECK(Near(GlossPercent(kGlossMax), 100.0f));
        CHECK(Near(GlossFromPercent(0.0f), 0.0f));
        CHECK(Near(GlossFromPercent(100.0f), kGlossMax));
        CHECK(Near(GlossFromPercent(10.0f), 5.0f));
        CHECK(Near(GlossFromPercent(50.0f), 125.0f));
        // ⚠ THE DEFAULT LANDS NEAR A QUARTER OF THE WAY ALONG, which is the
        // whole reason for the curve: linear it would sit at 6% with every
        // ordinary value bunched underneath it.
        CHECK(GlossPercent(kDefaultGloss) > 20.0f);
        CHECK(GlossPercent(kDefaultGloss) < 30.0f);
        // Round trip through the slider's own units.
        CHECK(Near(GlossFromPercent(GlossPercent(30.0f)), 30.0f, 0.01f));
        CHECK(Near(GlossFromPercent(GlossPercent(500.0f)), 500.0f, 0.01f));
        // Out of range percentages are clamped rather than extrapolated.
        CHECK(Near(GlossFromPercent(-10.0f), 0.0f));
        CHECK(Near(GlossFromPercent(500.0f), kGlossMax));
    }

    std::printf("OverlayPlanTests OK\n");
    return 0;
}
