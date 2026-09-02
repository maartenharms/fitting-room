#include "../src/PbrPearl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

namespace {
    [[nodiscard]] bool Near(float a, float b) {
        return (a - b) < 0.002f && (b - a) < 0.002f;
    }

    // A fabricated MSVC x64 RTTI chain, laid out in one buffer exactly the way
    // the compiler lays it out in an image: object -> vtable -> COL at [-1],
    // every COL field an offset from a base the COL's own [5] recovers. The
    // walk under test does real pointer arithmetic, so the fake has to be a
    // real memory layout rather than a mock.
    struct FakeRtti {
        // "image", offsets below index into this
        unsigned char image[256]{};
        std::uintptr_t vtblStorage[4]{};  // [0] = COL*, [1..] = fake slots
        std::uintptr_t objStorage[1]{};   // [0] = the object's vptr
        void* object{};                   // what the walk is handed

        // a_name lands at image[0x40], TypeDescriptor at image[0x30]
        // (name at td+0x10), COL at image[0x00].
        void Build(const char* a_name, std::uint32_t a_signature) {
            auto* const col = reinterpret_cast<std::uint32_t*>(image + 0x00);
            col[0] = a_signature;  // signature
            col[1] = 0;            // offset
            col[2] = 0;            // cdOffset
            col[3] = 0x30;         // TypeDescriptor RVA
            col[4] = 0x60;         // ClassDescriptor RVA, unused by the walk
            col[5] = 0x00;         // the COL's own RVA
            std::strcpy(reinterpret_cast<char*>(image + 0x40), a_name);
            vtblStorage[0] = reinterpret_cast<std::uintptr_t>(col);
            // The object's vptr points AT slot 1 so that [-1] is the COL,
            // which is where the linker puts it in a real image.
            objStorage[0] = reinterpret_cast<std::uintptr_t>(&vtblStorage[1]);
            object        = &objStorage[0];
        }
    };
}

int main() {
    using namespace OS::PbrPearl;

    {  // ⚠ NO COLOUR, NO WRITES, whatever the material declares. A flat dye on
        // a PBR piece must render exactly as it did before this header
        // existed, or every plain dye in every save restyles on update.
        CHECK(!PlanFor(kFuzz | kTwoLayer | kColoredCoat, false).Any());
        CHECK(!PlanFor(0, false).Any());
    }
    {  // Fuzz takes the pearl on its own bit and nothing else's.
        CHECK(PlanFor(kFuzz, true).fuzz);
        CHECK(!PlanFor(kFuzz, true).coat);
        CHECK(!PlanFor(0, true).fuzz);
    }
    {  // ⚠ COAT NEEDS BOTH BITS. TwoLayer alone is a clear coat: the shader
        // multiplies the coat colour in only under ColoredCoat, so writing it
        // under TwoLayer alone would change nothing and read in the field as
        // "pearl does nothing on this piece". Demanding both turns that piece
        // into a logged refusal instead of a silent one.
        CHECK(!PlanFor(kTwoLayer, true).coat);
        CHECK(!PlanFor(kColoredCoat, true).coat);
        CHECK(PlanFor(kTwoLayer | kColoredCoat, true).coat);
    }
    {  // Both features on one material take both writes; the shader runs the
        // branches independently and each write lands on its own constants.
        const auto w = PlanFor(kFuzz | kTwoLayer | kColoredCoat, true);
        CHECK(w.fuzz);
        CHECK(w.coat);
        CHECK(w.Any());
    }
    {  // Subsurface must not attract a write. Its colour SHARES the coat
        // colour's bytes (0x38, measured off both getters), so a write keyed
        // on the wrong bit would repaint a skin tone.
        CHECK(!PlanFor(kSubsurface, true).Any());
    }
    {  // The sRGB ends are exact and the middle is the curve, not the line.
        CHECK(Near(SrgbToLinear(0), 0.0f));
        CHECK(Near(SrgbToLinear(255), 1.0f));
        CHECK(Near(SrgbToLinear(128), 0.2158f));
        CHECK(SrgbToLinear(128) < 128.0f / 255.0f);
    }
    {  // ⚠ THE WEIGHT LIFTS TOWARD THE CEILING, NEVER A CUT AND NEVER PAST
        // IT. The first field run measured why a floor at full strength was
        // wrong: authored weights of 0.2 to 0.4 all slammed to 1.0 and the
        // whole set read as an oil slick. Full strength now reaches exactly
        // the ceiling, half strength walks half the gap, and a piece the
        // artist authored fuzzier than the ceiling keeps its own weight.
        CHECK(Near(FuzzWeightFor(0.2f, 255, 0.5f), 0.5f));
        CHECK(Near(FuzzWeightFor(0.4f, 255, 0.5f), 0.5f));
        CHECK(Near(FuzzWeightFor(0.2f, 128, 0.5f), 0.2f + 0.3f * 128.0f / 255.0f));
        CHECK(Near(FuzzWeightFor(0.9f, 255, 0.5f), 0.9f));
        CHECK(Near(FuzzWeightFor(0.2f, 0, 0.5f), 0.2f));
        // Ceiling 0 is the off switch: every artist value survives untouched.
        CHECK(Near(FuzzWeightFor(0.2f, 255, 0.0f), 0.2f));
        CHECK(Near(FuzzWeightFor(0.0f, 255, 0.0f), 0.0f));
        // A ceiling above 1 clamps rather than overdriving the lerp.
        CHECK(Near(FuzzWeightFor(0.2f, 255, 5.0f), 1.0f));
    }
    {  // ⚠ THE FUZZ TAKES A HUE AT FULL ENERGY, NEVER A DARKNESS. The field
        // measured why: a second stop of 5A0A1A wrote a near-black constant
        // into an ADDITIVE light response and no sheen showed. The scale puts
        // the largest channel at exactly 1 and the weight owns the energy;
        // a stop too dark to carry a hue refuses (scale 0) instead of
        // dividing by almost-zero into a blowout.
        CHECK(Near(FuzzEnergyScale(1.0f), 1.0f));
        CHECK(Near(FuzzEnergyScale(0.5f), 2.0f));
        CHECK(Near(FuzzEnergyScale(0.1f), 10.0f));
        CHECK(Near(FuzzEnergyScale(0.0f), 0.0f));
        CHECK(Near(FuzzEnergyScale(kFuzzHueEpsilon / 2.0f), 0.0f));
        // The field case itself: 5A0A1A's largest linear channel scales to 1.
        const float r = SrgbToLinear(0x5A);
        CHECK(Near(r * FuzzEnergyScale(r), 1.0f));
    }
    {  // Gloss 128 is unchanged; shine and roughness move opposite ways; and
        // the clamp holds both rails, matching the classic path's own rule
        // that a nudge is never a replacement.
        CHECK(Near(RoughnessScaleFor(1.0f, 128), 1.0f));
        CHECK(Near(RoughnessScaleFor(1.0f, 192), 0.5f));
        CHECK(Near(RoughnessScaleFor(1.0f, 64), 2.0f));
        CHECK(Near(RoughnessScaleFor(0.06f, 255), 0.05f));
        CHECK(Near(RoughnessScaleFor(3.9f, 0), 4.0f));
    }
    {  // The RTTI walk reads the fabricated chain back by pointer arithmetic
        // alone, which is exactly what it will do to a live vtable.
        FakeRtti fake;
        fake.Build(".?AVBSLightingShaderMaterialPBR@@", 1);
        const char* const name = MsvcRttiName(fake.object);
        CHECK(name != nullptr);
        CHECK(kMaterialRttiName == name);
        CHECK(IsTruePbrMaterial(fake.object));
    }
    {  // A different class walks fine and is refused by NAME, which is the
        // half the gate exists for: the envmap material would take these
        // offsets as an out-of-bounds write.
        FakeRtti fake;
        fake.Build(".?AVBSLightingShaderMaterialEnvmap@@", 1);
        CHECK(MsvcRttiName(fake.object) != nullptr);
        CHECK(!IsTruePbrMaterial(fake.object));
    }
    {  // ⚠ SIGNATURE 1 OR NOTHING. Signature 0 is the absolute-pointer layout
        // no x64 DLL ships, so the walk refuses it rather than dereferencing
        // offsets as pointers; and a null object refuses before touching
        // anything at all.
        FakeRtti fake;
        fake.Build(".?AVBSLightingShaderMaterialPBR@@", 0);
        CHECK(MsvcRttiName(fake.object) == nullptr);
        CHECK(!IsTruePbrMaterial(fake.object));
        CHECK(MsvcRttiName(nullptr) == nullptr);
        CHECK(!IsTruePbrMaterial(nullptr));
    }
    {  // The measured constants themselves, pinned so a "harmless" edit to
        // the header is a test failure rather than a field crash. These are
        // Jiaye 26.8.16's numbers; if CS moves them, this suite is where the
        // new measurement gets recorded.
        CHECK(kPbrFlagsOffset == 0xA4);
        CHECK(kCoatColorOffset == 0x38);
        CHECK(kRoughnessScaleOffset == 0x8C);
        CHECK(kFuzzColorOffset == 0xB0);
        CHECK(kFuzzWeightOffset == 0xBC);
        CHECK(kFuzz == 0x20u);
        CHECK(kTwoLayer == 0x02u);
        CHECK(kColoredCoat == 0x04u);
        CHECK(kSubsurface == 0x01u);
    }

    std::printf("PbrPearlTests: all passed\n");
    return 0;
}
