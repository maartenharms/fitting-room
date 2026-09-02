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
    {  // ⚠ THE COVERAGE RULE. Iridescent needs a cubemap to be view dependent,
        // and envmap and default shapes coexist inside ONE garment: an Iron
        // Armor is IronArmor (EnvironmentMap) beside IronSkirt and IronSatchel
        // (Default). Degrading to nacre rather than to flat is what stops one
        // garment half shimmering, which reads as a bug rather than a look.
        CHECK(EffectiveMode(Mode::kIridescent, true) == Mode::kIridescent);
        CHECK(EffectiveMode(Mode::kIridescent, false) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kNacre, true) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kNacre, false) == Mode::kNacre);
        CHECK(EffectiveMode(Mode::kFlat, true) == Mode::kFlat);
        CHECK(EffectiveMode(Mode::kFlat, false) == Mode::kFlat);
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
    {  // And composed with the degrade, which is how the two halves of one
        // garment end up agreeing: the cloth shape ramps its diffuse, the metal
        // shape ramps its reflection, and neither ramps both.
        CHECK(RampsDiffuse(EffectiveMode(Mode::kIridescent, false)));
        CHECK(!RampsReflection(EffectiveMode(Mode::kIridescent, false)));
        CHECK(!RampsDiffuse(EffectiveMode(Mode::kIridescent, true)));
        CHECK(RampsReflection(EffectiveMode(Mode::kIridescent, true)));
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
