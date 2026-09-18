#include "../src/DyeRamp.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

namespace {
    [[nodiscard]] bool Near(float a, float b) { return (a - b) < 0.002f && (b - a) < 0.002f; }
}

int main() {
    using namespace OS::DyeRamp;

    {  // Strength 0 collapses the ramp onto the first stop, which IS flat.
        // This is what keeps the slider meaningful on a special dye instead of
        // undefined: 0 is the plain colour, 255 is the full two-stop ramp.
        CHECK(Near(NarrowStop(0.2f, 0.9f, 0), 0.2f));
        CHECK(Near(NarrowStop(0.9f, 0.2f, 0), 0.9f));
    }
    {  // Strength 255 reaches the second stop exactly.
        CHECK(Near(NarrowStop(0.2f, 0.9f, 255), 0.9f));
    }
    {  // Halfway is halfway.
        CHECK(Near(NarrowStop(0.0f, 1.0f, 128), 0.502f));
    }
    {  // Two equal stops are flat at every strength, so a dye authored with
        // hex2 == hex behaves exactly like one authored without hex2 at all.
        for (int s = 0; s <= 255; ++s) {
            CHECK(Near(NarrowStop(0.4f, 0.4f, static_cast<std::uint8_t>(s)), 0.4f));
        }
    }
    {  // ⚠ THE BYTE OVERLOAD IS WHAT THE PAINT PATH ACTUALLY CALLS, and it must
        // agree with the float one at both ends. The stops are authored as bytes
        // and ApplyDyeStrength wants a byte back, so a float round trip in
        // between would round differently from the primary colour beside it and
        // the two stops would drift apart by a level at the same strength.
        CHECK(NarrowStopByte(0x20, 0xC0, 0) == 0x20);
        CHECK(NarrowStopByte(0x20, 0xC0, 255) == 0xC0);
        CHECK(NarrowStopByte(0x40, 0x40, 128) == 0x40);
        // ⚠ MONOTONE ACROSS THE WHOLE SLIDER, which is the property a player
        // feels. A rounding rule that stepped backwards anywhere would make the
        // second stop jitter under a dragged handle.
        for (int s = 1; s <= 255; ++s) {
            const auto lo = NarrowStopByte(0x20, 0xC0, static_cast<std::uint8_t>(s - 1));
            const auto hi = NarrowStopByte(0x20, 0xC0, static_cast<std::uint8_t>(s));
            CHECK(hi >= lo);
        }
        // And downward, since a second stop DARKER than the first is ordinary.
        for (int s = 1; s <= 255; ++s) {
            const auto hi = NarrowStopByte(0xC0, 0x20, static_cast<std::uint8_t>(s - 1));
            const auto lo = NarrowStopByte(0xC0, 0x20, static_cast<std::uint8_t>(s));
            CHECK(lo <= hi);
        }
    }
    {  // ⚠ THE CARRIER RULE (2026-09-02). An iridescent dye's second colour
        // needs somewhere OTHER than the diffuse to live: the cubemap of a
        // reflective shape (the angle sweep on metal) or the fuzz and coat
        // constants of a True PBR material (the 1.1.6 pearl). A shape with
        // neither takes its first colour FLAT, like a flat dye.
        //
        // Field, 2026-09-02: 1.1.6 had degraded iridescent to nacre on such a
        // shape, and since d9c49f10 forced recolour there the luminance-keyed
        // ramp actually ran, spreading both stops across a cloth robe's folds
        // as marbling. The user chose flat over a tamer ramp ("do 1"). The
        // half-shimmering Iron Armor the old rule guarded against is now the
        // look: the cuirass sweeps, the skirt is plain.
        CHECK(EffectiveMode(Mode::kIridescent, Carrier::kCubemap) == Mode::kIridescent);
        CHECK(EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kIridescent, Carrier::kNone) == Mode::kFlat);
        // ⚠ NACRE DYES ARE UNTOUCHED. Nacre IS the diffuse ramp by design, and
        // the user named iridescent alone.
        CHECK(EffectiveMode(Mode::kNacre, Carrier::kCubemap) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kNacre, Carrier::kPbrPearl) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kNacre, Carrier::kNone) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kFlat, Carrier::kCubemap) == Mode::kFlat);
        CHECK(EffectiveMode(Mode::kFlat, Carrier::kPbrPearl) == Mode::kFlat);
        CHECK(EffectiveMode(Mode::kFlat, Carrier::kNone) == Mode::kFlat);
    }
    {  // ⚠ THE CUBEMAP OUTRANKS THE PEARL, which pins the order OutfitDye.cpp
        // asks its two questions in. A reflective shape keeps the sweep it has
        // always had whatever its material's flags say; the pearl carrier is
        // only consulted on a shape with no cubemap, which is where a True PBR
        // piece always lands (Community Shaders answers kDefault for it).
        CHECK(CarrierFor(true, false) == Carrier::kCubemap);
        CHECK(CarrierFor(true, true) == Carrier::kCubemap);
        CHECK(CarrierFor(false, true) == Carrier::kPbrPearl);
        CHECK(CarrierFor(false, false) == Carrier::kNone);
    }
    {  // ⚠ THE RAMP LANDS ON EXACTLY ONE TARGET, and this is the pair of
        // predicates the paint path branches on. Ramping BOTH the diffuse and
        // the cubemap of an iridescent metal piece would double the colour and
        // read as a stain rather than as a sheen, which is why iridescent moves
        // the ramp OFF the diffuse rather than adding the cubemap to it.
        CHECK(!RampsDiffuse(Mode::kFlat));
        CHECK(RampsDiffuse(Mode::kNacre));
        CHECK(!RampsDiffuse(Mode::kIridescent));

        CHECK(!RampsReflection(Mode::kFlat));
        CHECK(!RampsReflection(Mode::kNacre));
        CHECK(RampsReflection(Mode::kIridescent));
    }
    {  // And composed with the degrade, which is what each shape of one garment
        // does under an iridescent dye: the metal shape ramps its reflection,
        // a True PBR pearl carrier resolves to the diffuse ramp (which
        // PearlRidesMaterial below then hands to the fuzz or the coat), the
        // cloth shape ramps NOTHING, and no shape ramps both.
        CHECK(RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kCubemap)));
        CHECK(!RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kCubemap)));
        CHECK(RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl)));
        CHECK(!RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl)));
        CHECK(!RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kNone)));
        CHECK(!RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kNone)));
        // A nacre dye on cloth still ramps its diffuse, exactly as 1.1.7 did.
        CHECK(RampsDiffuse(EffectiveMode(Mode::kNacre, Carrier::kNone)));
    }
    {  // ⚠ A PEARL CARRIER'S DIFFUSE NEVER RAMPS (2026-09-15). Its second stop
        // lives in the fuzz or the coat, so the diffuse takes the first colour
        // flat. Through 1.1.9 only a fuzz piece was spared and a coat-only
        // piece kept the luminance-keyed ramp beside its coat write: the
        // marbling the user retired from the palette. The predicate takes the
        // EFFECTIVE mode, so it is asked after EffectiveMode has run.
        CHECK(PearlRidesMaterial(Carrier::kPbrPearl,
                                 EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl)));
        CHECK(PearlRidesMaterial(Carrier::kPbrPearl,
                                 EffectiveMode(Mode::kNacre, Carrier::kPbrPearl)));
        // A flat dye and the mask modes have no ramp to hand over.
        for (const auto m : { Mode::kFlat, Mode::kMetal, Mode::kCloth, Mode::kTwoTone }) {
            CHECK(!PearlRidesMaterial(Carrier::kPbrPearl, EffectiveMode(m, Carrier::kPbrPearl)));
        }
        // The cubemap carrier keeps its sweep on the reflection, and a nacre
        // dye on a shape with no carrier still ramps its diffuse.
        CHECK(!PearlRidesMaterial(Carrier::kCubemap,
                                  EffectiveMode(Mode::kIridescent, Carrier::kCubemap)));
        CHECK(!PearlRidesMaterial(Carrier::kCubemap, Mode::kNacre));
        CHECK(!PearlRidesMaterial(Carrier::kNone,
                                  EffectiveMode(Mode::kNacre, Carrier::kNone)));
        CHECK(!PearlRidesMaterial(Carrier::kNone,
                                  EffectiveMode(Mode::kIridescent, Carrier::kNone)));
    }
    {  // An unknown byte from a future record reads as flat rather than as
        // whatever the enum's last value happens to be.
        CHECK(ModeFromByte(0) == Mode::kFlat);
        CHECK(ModeFromByte(1) == Mode::kNacre);
        CHECK(ModeFromByte(2) == Mode::kIridescent);
        CHECK(ModeFromByte(3) == Mode::kMetal);
        CHECK(ModeFromByte(4) == Mode::kCloth);
        CHECK(ModeFromByte(5) == Mode::kTwoTone);
        CHECK(ModeFromByte(6) == Mode::kFlat);
        CHECK(ModeFromByte(255) == Mode::kFlat);
    }

    // ---- the envmask modes (2026-09-04) ------------------------------------
    //
    // A shape's own environment mask says per texel how reflective it is, and
    // that is the metal-versus-cloth line an artist already drew. Three modes
    // read it: metal (hex on the reflective texels, the rest untouched), cloth
    // (the reverse) and twotone (hex on the cloth, hex2 on the metal). They are
    // not ramps: no stop sweeps with light or luminance, so both ramp
    // predicates stay off, and the carrier rule that degrades iridescent does
    // not apply, because the per-shape answer is a different question, asked
    // of a shape's reflectivity and its mask rather than of its carrier.
    {  // Mask modes are exactly the three, and nothing else is one.
        CHECK(!IsMaskMode(Mode::kFlat));
        CHECK(!IsMaskMode(Mode::kNacre));
        CHECK(!IsMaskMode(Mode::kIridescent));
        CHECK(IsMaskMode(Mode::kMetal));
        CHECK(IsMaskMode(Mode::kCloth));
        CHECK(IsMaskMode(Mode::kTwoTone));
    }
    {  // The carrier does not move a mask mode: EffectiveMode is the ramp
        // degrade, and a mask mode has no ramp to degrade.
        for (const auto m : { Mode::kMetal, Mode::kCloth, Mode::kTwoTone }) {
            CHECK(EffectiveMode(m, Carrier::kCubemap) == m);
            CHECK(EffectiveMode(m, Carrier::kPbrPearl) == m);
            CHECK(EffectiveMode(m, Carrier::kNone) == m);
            CHECK(!RampsDiffuse(m));
            CHECK(!RampsReflection(m));
        }
    }
    {  // ⚠ WHAT THE DIFFUSE GETS, per shape, and the engine's own reflectivity
        // decides: a reflective shape with a mask is split by it; a reflective
        // shape with no mask is all metal (that is how the engine draws it); a
        // shape the engine does not reflect at all is all cloth, and that
        // includes every True PBR piece for now (its metallic map is a second
        // reader, not this one).
        // metal: the reflective texels take hex, everything else is left alone.
        CHECK(DiffuseTakeFor(Mode::kMetal, true, true) == Take::kSplit);
        CHECK(DiffuseTakeFor(Mode::kMetal, true, false) == Take::kPrimary);
        CHECK(DiffuseTakeFor(Mode::kMetal, false, true) == Take::kUntouched);
        CHECK(DiffuseTakeFor(Mode::kMetal, false, false) == Take::kUntouched);
        // cloth: the reverse, so an all-metal shape has nothing to dye.
        CHECK(DiffuseTakeFor(Mode::kCloth, true, true) == Take::kSplit);
        CHECK(DiffuseTakeFor(Mode::kCloth, true, false) == Take::kUntouched);
        CHECK(DiffuseTakeFor(Mode::kCloth, false, true) == Take::kPrimary);
        CHECK(DiffuseTakeFor(Mode::kCloth, false, false) == Take::kPrimary);
        // twotone: hex on the cloth, hex2 on the metal, both sides painted.
        CHECK(DiffuseTakeFor(Mode::kTwoTone, true, true) == Take::kSplit);
        CHECK(DiffuseTakeFor(Mode::kTwoTone, true, false) == Take::kSecond);
        CHECK(DiffuseTakeFor(Mode::kTwoTone, false, true) == Take::kPrimary);
        CHECK(DiffuseTakeFor(Mode::kTwoTone, false, false) == Take::kPrimary);
        // Every ramp or flat mode keeps what it always had: the request's own
        // tint on the diffuse, whatever the shape.
        for (const auto m : { Mode::kFlat, Mode::kNacre, Mode::kIridescent }) {
            CHECK(DiffuseTakeFor(m, true, true) == Take::kPrimary);
            CHECK(DiffuseTakeFor(m, true, false) == Take::kPrimary);
            CHECK(DiffuseTakeFor(m, false, false) == Take::kPrimary);
        }
    }
    {  // ⚠ WHICH SIDE OF THE MASK TAKES WHICH COLOUR on a split build. Inside
        // is the reflective side (the mask high), outside the rest. This is
        // what pins the paint path's construction of its two-sided request.
        CHECK(SplitFor(Mode::kMetal).inside == Take::kPrimary);
        CHECK(SplitFor(Mode::kMetal).outside == Take::kUntouched);
        CHECK(SplitFor(Mode::kCloth).inside == Take::kUntouched);
        CHECK(SplitFor(Mode::kCloth).outside == Take::kPrimary);
        CHECK(SplitFor(Mode::kTwoTone).inside == Take::kSecond);
        CHECK(SplitFor(Mode::kTwoTone).outside == Take::kPrimary);
    }
    {  // ⚠ WHICH MAP, AND "AUTHORED" IS THE TEST, NOT "BOUND" (field, 2026-09-04).
        // The vanilla iron war axe ships no _m.dds, so the engine binds a
        // nameless default into the mask slot and the texture set's mask path
        // is empty; a split keyed on that pointer was refused six times and the
        // axe stayed undyed. The engine's own order is the rule: the environment
        // mask where the texture set NAMES one, else the normal map's alpha
        // where it names that, else no map at all (an all-metal shape).
        CHECK(MapSourceFor(true, true) == MapSource::kEnvMask);
        CHECK(MapSourceFor(true, false) == MapSource::kEnvMask);
        CHECK(MapSourceFor(false, true) == MapSource::kNormalAlpha);
        CHECK(MapSourceFor(false, false) == MapSource::kNone);
    }
    {  // The reflection (the cubemap, or under Community Shaders the marker
        // texel) is the metal, so it takes the metal's colour: hex2 under
        // twotone, nothing under cloth, and the request's own tint everywhere
        // else, which is what every mode before these did.
        CHECK(ReflectionTakeFor(Mode::kTwoTone) == Take::kSecond);
        CHECK(ReflectionTakeFor(Mode::kCloth) == Take::kUntouched);
        CHECK(ReflectionTakeFor(Mode::kMetal) == Take::kPrimary);
        CHECK(ReflectionTakeFor(Mode::kFlat) == Take::kPrimary);
        CHECK(ReflectionTakeFor(Mode::kNacre) == Take::kPrimary);
        CHECK(ReflectionTakeFor(Mode::kIridescent) == Take::kPrimary);
    }

    {  // ---- the METAL STARTS slider (2026-09-04) ------------------------------
        //
        // The cut is t of the way up the class gap, from the dark class mean
        // (byte 0: everything above the cloth is metal) to the bright one
        // (byte 255). 128 is halfway, which is where Otsu landed on every
        // mask measured (0.49 to 0.51 of the gap), so the default is today's
        // picture.
        CHECK(Near(CutFor(0.2f, 0.6f, 0), 0.2f));
        CHECK(Near(CutFor(0.2f, 0.6f, 255), 0.6f));
        CHECK(Near(CutFor(0.2f, 0.6f, 128), 0.4f));
        // The cloak, measured: cloth mean 0.027, gold mean 0.544. Otsu cut it
        // at 0.28, inside the gold's own 0.10 to 0.75 spread; byte 51 (20%)
        // puts the cut at 0.13, under the duller gold.
        CHECK(Near(CutFor(0.027f, 0.544f, 51), 0.130f));
        // Monotone over the byte, the property a dragged handle feels.
        for (int s = 1; s <= 255; ++s) {
            CHECK(CutFor(0.027f, 0.544f, static_cast<std::uint8_t>(s)) >=
                  CutFor(0.027f, 0.544f, static_cast<std::uint8_t>(s - 1)));
        }
    }
    {  // The blend band around the cut, feathered as a fraction of the class
        // gap ([Dye] fDyeMetalMaskFeather, 0.5 shipped).
        //
        // ⚠ THE LOW EDGE NEVER FALLS BELOW THE MIDPOINT OF THE DARK MEAN AND
        // THE CUT, so a low cut cannot bleed metal colour into the cloth: the
        // band narrows with the cut rather than sliding over the cloth. And
        // it narrows on BOTH sides, so what the slider says is metal is
        // fully metal. The cloak at byte 51: the cloth at 0.05 is outside,
        // the gold at 0.20 fully inside.
        const auto w51 = WindowFor(0.027f, 0.544f, 51, 0.5f);
        CHECK(w51.lo >= (0.027f + CutFor(0.027f, 0.544f, 51)) * 0.5f - 0.0001f);
        CHECK(w51.lo >= 0.05f);  // the cloth: t = 0
        CHECK(w51.hi <= 0.20f);  // the duller gold: t = 1
        CHECK(w51.lo < w51.hi);
        // At 128 with the shipped feather the clamp sits exactly on the band
        // it would have had, so the default picture is unchanged by a byte.
        const auto  w128    = WindowFor(0.027f, 0.544f, 128, 0.5f);
        const float cut128  = CutFor(0.027f, 0.544f, 128);
        const float half128 = (0.544f - 0.027f) * 0.5f * 0.5f;
        CHECK(Near(w128.lo, cut128 - half128));
        CHECK(Near(w128.hi, cut128 + half128));
        // Above halfway the feather is the whole story, as it was.
        const auto  w200   = WindowFor(0.2f, 0.6f, 200, 0.5f);
        const float cut200 = CutFor(0.2f, 0.6f, 200);
        CHECK(Near(w200.lo, cut200 - 0.1f));
        CHECK(Near(w200.hi, cut200 + 0.1f));
        // Byte 0 collapses the band onto the dark mean: a hard edge there,
        // never a window below it.
        const auto w0 = WindowFor(0.2f, 0.6f, 0, 0.5f);
        CHECK(Near(w0.lo, 0.2f));
        CHECK(Near(w0.hi, 0.2f));
    }

    std::printf("DyeRampTests: all passed\n");
    return 0;
}
