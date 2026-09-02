#include "../src/DyeRamp.h"

#include <cstdio>
#include <cstdlib>

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
        // a True PBR pearl carrier ramps its diffuse (OutfitDye.cpp then hands
        // that ramp to the fuzz, or leaves it on the diffuse for a coat-only
        // piece), the cloth shape ramps NOTHING, and no shape ramps both.
        CHECK(RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kCubemap)));
        CHECK(!RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kCubemap)));
        CHECK(RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl)));
        CHECK(!RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kPbrPearl)));
        CHECK(!RampsDiffuse(EffectiveMode(Mode::kIridescent, Carrier::kNone)));
        CHECK(!RampsReflection(EffectiveMode(Mode::kIridescent, Carrier::kNone)));
        // A nacre dye on cloth still ramps its diffuse, exactly as 1.1.7 did.
        CHECK(RampsDiffuse(EffectiveMode(Mode::kNacre, Carrier::kNone)));
    }
    {  // An unknown byte from a future record reads as flat rather than as
        // whatever the enum's last value happens to be.
        CHECK(ModeFromByte(0) == Mode::kFlat);
        CHECK(ModeFromByte(1) == Mode::kNacre);
        CHECK(ModeFromByte(2) == Mode::kIridescent);
        CHECK(ModeFromByte(3) == Mode::kFlat);
        CHECK(ModeFromByte(255) == Mode::kFlat);
    }

    std::printf("DyeRampTests: all passed\n");
    return 0;
}
