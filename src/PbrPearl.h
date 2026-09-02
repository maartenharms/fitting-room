#pragma once

// Pearlescent on a Community Shaders True PBR material: the pure half.
//
// ⚠ NO ENGINE TYPES IN THIS HEADER, the DyeRamp.h rule for the DyeRamp.h
// reason: the one caller lives in OutfitDye.cpp, which no test compiles, so
// every decision left there reaches the field unexercised.
//
// ⚠⚠ EVERYTHING BELOW IS MEASURED AGAINST THE SHIPPED BINARY, NOT READ FROM
// SOURCE. The winning install is `Community Shaders Jiaye 26.8.16`
// (CommunityShaders.dll beside its own PDB), and the numbers were taken from
// it on 2026-08-30 three ways:
//
//  * The member offsets are the PDB's, confirmed by disassembling the getters
//    with capstone. GetFuzzColor is `lea rax,[rcx+0xb0]; ret` and
//    GetCoatColor is `lea rax,[rcx+0x38]; ret`, so the COAT COLOUR IS NOT A
//    MEMBER OF THE PBR TYPE even though its getter is: it lives in the base
//    class slot vanilla calls specularColor, shared byte for byte with the
//    subsurface colour (GetSubsurfaceColor returns the same address).
//    GetCoatStrength and GetSubsurfaceOpacity share 0x90 the same way.
//
//  * The pbrFlags bit values are ApplyTextureSetData's, which writes the coat
//    fields under `test r8b,2`, the subsurface fields under `test r8b,1` and
//    the fuzz fields under `test r8b,0x20` after loading r8d from
//    [material+0xA4]. ⚠ THESE ARE THE C++ ENUM'S BITS AND THEY ARE NOT THE
//    SHADER'S: the HLSL's PBR::Flags puts four texture-presence bits first
//    and has Fuzz at 1<<9, so a value read out of PBRMath.hlsli is wrong
//    here by four positions.
//
//  * What each write RENDERS as is the installed Lighting.hlsl's. The fuzz
//    branch sets material.FuzzColor from the uploaded constant and, when the
//    mesh ships a fuzz map, MULTIPLIES the sample in on top - so writing the
//    constant tints the fuzz response on exactly the pieces that already
//    have one, which is the whole mechanism. The coat colour reaches the
//    picture only under the ColoredCoat flag, which is why PlanFor demands
//    it beside TwoLayer rather than treating the pair as one feature.
//
// ⚠⚠ A RAW OFFSET WRITE ON THE WRONG ALLOCATION IS A CRASH, NOT A COSMETIC.
// The property's kVertexLighting flag says what the SHADER thinks; only the
// allocation's own RTTI says what the bytes ARE, and it is the bytes being
// written. MsvcRttiName below is that gate, and the caller must refuse any
// material whose name is not kMaterialRttiName - Bethesda's NiRTTI cannot
// answer this because BSShaderMaterial is not an NiObject.
//
// ⚠ A CS UPDATE CAN MOVE ANY OF THESE NUMBERS. The RTTI gate keeps the write
// on the right TYPE but cannot notice a relaid-out one; bDyePbrPearl exists
// so a field round on a newer Community Shaders costs an INI edit rather
// than a crash log autopsy.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace OS::PbrPearl {

    // ---- Community Shaders' PBR material, the shipped layout ---------------

    inline constexpr std::string_view kMaterialRttiName{
        ".?AVBSLightingShaderMaterialPBR@@"
    };

    // C++-side pbrFlags bits, out of ApplyTextureSetData's disassembly.
    inline constexpr std::uint32_t kSubsurface{ 1u << 0 };
    inline constexpr std::uint32_t kTwoLayer{ 1u << 1 };
    inline constexpr std::uint32_t kColoredCoat{ 1u << 2 };
    inline constexpr std::uint32_t kFuzz{ 1u << 5 };

    // Byte offsets into the material allocation, PDB plus getter disassembly.
    inline constexpr std::ptrdiff_t kPbrFlagsOffset{ 0xA4 };        // uint32
    inline constexpr std::ptrdiff_t kCoatColorOffset{ 0x38 };       // float3, shared with subsurface
    inline constexpr std::ptrdiff_t kRoughnessScaleOffset{ 0x8C };  // float
    inline constexpr std::ptrdiff_t kFuzzColorOffset{ 0xB0 };       // float3
    inline constexpr std::ptrdiff_t kFuzzWeightOffset{ 0xBC };      // float

    // ---- which features a pearl can actually land on -----------------------
    //
    // ⚠ ONLY FEATURES THE MESH ALREADY DECLARES, NEVER TURNED ON. A bit set
    // here means PG Patcher's json authored the feature and its textures, so
    // the shader is already running the branch and the write only changes a
    // constant it already reads. Setting a missing bit would send the shader
    // through a branch whose texture slot holds whatever happens to be bound.
    //
    // ⚠ COAT NEEDS BOTH BITS. TwoLayer alone is a CLEAR coat: the shader
    // computes the layer but multiplies its colour in only under ColoredCoat,
    // so writing the colour under TwoLayer alone changes nothing and would
    // read in the field as "pearl does nothing on this piece". Demanding both
    // makes that piece a logged refusal instead.
    struct Writes {
        bool fuzz{ false };
        bool coat{ false };

        [[nodiscard]] constexpr bool Any() const { return fuzz || coat; }
    };

    [[nodiscard]] constexpr Writes PlanFor(std::uint32_t a_pbrFlags,
                                           bool          a_hasPearlColour) {
        if (!a_hasPearlColour) {
            return {};
        }
        Writes w{};
        w.fuzz = (a_pbrFlags & kFuzz) != 0;
        w.coat = (a_pbrFlags & kTwoLayer) != 0 && (a_pbrFlags & kColoredCoat) != 0;
        return w;
    }

    // ---- the arithmetic of the three writes --------------------------------

    // The dye's stop bytes are sRGB, the way every swatch and diffuse build
    // treats them. The uploaded constant is consumed LINEAR: Lighting.hlsl
    // runs Color::Diffuse() over the fuzz map's SAMPLE and multiplies the
    // constant in raw, so the constant is already expected on the far side of
    // that conversion.
    [[nodiscard]] inline float SrgbToLinear(std::uint8_t a_byte) {
        const float c = static_cast<float>(a_byte) / 255.0f;
        return c <= 0.04045f ? c / 12.92f
                             : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    // ⚠⚠ THE FUZZ TAKES THE STOP'S HUE AT FULL ENERGY, NEVER ITS DARKNESS.
    // Field, 2026-08-31: a pearl whose second stop was 5A0A1A wrote a linear
    // constant of (0.10, 0.004, 0.011) and the sheen "isn't coming through" -
    // near-black handed to an ADDITIVE light response is invisible, however
    // correct its hue. The fuzz term is specular energy, not paint: the map
    // multiplies it down and the weight lerps it in, so the constant's job is
    // to say WHICH colour the grazing light is, and the weight's job is to say
    // how much. Normalising the largest channel to 1 keeps the hue and hands
    // the whole energy question to the weight, which the ceiling already
    // owns. A stop so dark it has no hue (max under the epsilon) is refused
    // by the caller instead, logged, since 0/0 is not a colour.
    inline constexpr float kFuzzHueEpsilon{ 1.0f / 512.0f };

    [[nodiscard]] constexpr float FuzzEnergyScale(float a_maxChannel) {
        return a_maxChannel > kFuzzHueEpsilon ? 1.0f / a_maxChannel : 0.0f;
    }

    // ⚠ A LIFT TOWARD A CEILING, NEVER A CUT AND NO LONGER A FLOOR AT FULL.
    // The first field run (2026-08-30 19:58, the whole Callisto set) is why:
    // the artists authored these pieces at 0.2 to 0.4 and the old rule slammed
    // every one to 1.0 at default strength, which reads as an oil slick
    // rather than a pearl. fuzzWeight is a LERP between the piece's own
    // specular and the fuzz response, so 1.0 does not add a sheen, it
    // REPLACES the material.
    //
    // The rule now: strength walks the weight from the shipped value toward
    // a_target (the install's ceiling, fDyePbrPearlSheenMax), and a piece the
    // artist authored fuzzier than the ceiling keeps its own weight. The
    // ceiling is an INI so the next field round tunes it without a rebuild;
    // 0 turns the lift off entirely and leaves every artist value alone.
    [[nodiscard]] constexpr float FuzzWeightFor(float        a_shipped,
                                                std::uint8_t a_strength,
                                                float        a_target) {
        const float target = std::clamp(a_target, 0.0f, 1.0f);
        if (a_shipped >= target) {
            return a_shipped;
        }
        const float s = static_cast<float>(a_strength) / 255.0f;
        return a_shipped + (target - a_shipped) * s;
    }

    // The dye's gloss, on the PBR path. The classic path multiplies
    // specularPower, which this material never reads; the equivalent lever it
    // does read is roughnessScale, a multiplier over the _rmaos roughness
    // channel, and shine moves OPPOSITE to roughness so the same factor
    // divides here. 128 is unchanged, each 64 above halves the roughness,
    // each 64 below doubles it, and the clamp keeps a stacked nudge from
    // zeroing the texture's own detail out entirely.
    [[nodiscard]] inline float RoughnessScaleFor(float        a_shipped,
                                                 std::uint8_t a_gloss) {
        const float factor =
            std::exp2((static_cast<int>(a_gloss) - 128) / 64.0f);
        return std::clamp(a_shipped / factor, 0.05f, 4.0f);
    }

    // ---- the RTTI gate ------------------------------------------------------
    //
    // MSVC x64 RTTI, walked by hand because the material is not an NiObject
    // and this DLL does not share CS's type_info objects. The vtable's [-1]
    // slot holds the Complete Object Locator; signature 1 means every field
    // is an image-relative offset and [5] is the COL's own RVA, which is what
    // lets the module base be recovered without asking the loader. The
    // TypeDescriptor's name starts 0x10 in, the undecorated-class prefix
    // `.?AV` and all.
    //
    // ⚠ SIGNATURE 1 OR NOTHING. Signature 0 is the pre-2015 absolute-pointer
    // layout, which no x64 DLL ships; treating an unknown value as either
    // guess would walk garbage, so both readings refuse instead.
    [[nodiscard]] inline const char* MsvcRttiName(const void* a_object) {
        if (!a_object) {
            return nullptr;
        }
        const auto* const vtbl =
            *static_cast<const std::uintptr_t* const*>(a_object);
        if (!vtbl) {
            return nullptr;
        }
        const auto* const col =
            reinterpret_cast<const std::uint32_t*>(vtbl[-1]);
        if (!col || col[0] != 1) {
            return nullptr;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(col) - col[5];
        const auto* const name =
            reinterpret_cast<const char*>(base + col[3] + 0x10);
        return name;
    }

    [[nodiscard]] inline bool IsTruePbrMaterial(const void* a_object) {
        const char* const name = MsvcRttiName(a_object);
        return name && kMaterialRttiName == name;
    }

}  // namespace OS::PbrPearl
