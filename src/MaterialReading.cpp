#include "MaterialReading.h"

#include <RE/B/BSLightingShaderMaterialEnvmap.h>
#include <RE/B/BSLightingShaderMaterialEye.h>
#include <RE/B/BSLightingShaderMaterialGlowmap.h>

#include <string>

namespace OS::MaterialReading {

    namespace {

        // ⚠⚠ THE NUMBER RIDES ALONG, AND THE FIRST READING IS WHY. The 22:54
        // census named this character's demon iris `feat=other`, which is this
        // table saying "not one of the six I know" and nothing more. That one
        // word is the difference between a rule that covers her eye and a rule
        // that silently skips it, and a name table can only ever answer for
        // the names already in it. Print what the engine actually said.
        //
        // The table is wider now for the same reason: glow and parallax are
        // what an authored eye is most likely to be wearing when it is not
        // kEye, and a geometry called 'glowUBEwbDemonEye04' is a strong hint
        // rather than a measurement.
        [[nodiscard]] const char* FeatureName(RE::BSShaderMaterial::Feature a_feature) {
            using F = RE::BSShaderMaterial::Feature;
            switch (a_feature) {
                case F::kEye:                 return "eye";
                case F::kFaceGen:             return "faceGen";
                case F::kFaceGenRGBTint:      return "faceGenRGB";
                case F::kHairTint:            return "hairTint";
                case F::kEnvironmentMap:      return "envMap";
                case F::kDefault:             return "default";
                case F::kGlowMap:             return "glowMap";
                case F::kParallax:            return "parallax";
                case F::kParallaxOcc:         return "parallaxOcc";
                case F::kMultilayerParallax:  return "multilayerParallax";
                case F::kNone:                return "none";
                default:                      return "other";
            }
        }

        [[nodiscard]] const char* TextureName(const RE::NiSourceTexture* a_tex) {
            return a_tex ? a_tex->name.c_str() : "";
        }

        // rather than printed as a hex word: the question this instrument is
        // asked is "is the glow live", and 0x4000004001 does not answer it.
        [[nodiscard]] std::string FlagNames(RE::BSLightingShaderProperty* a_prop) {
            using F = RE::BSShaderProperty::EShaderPropertyFlag;
            static constexpr struct {
                F           bit;
                const char* name;
            } kFlags[] = {
                { F::kSpecular, "specular" },   { F::kVertexAlpha, "vertexAlpha" },
                { F::kEnvMap, "envMap" },       { F::kEyeReflect, "eyeReflect" },
                { F::kOwnEmit, "ownEmit" },     { F::kExternalEmittance, "extEmit" },
                { F::kGlowMap, "glowMap" },     { F::kSoftLighting, "softLighting" },
                { F::kRimLighting, "rimLighting" },
                // ⚠⚠ BIT 42 IS COMMUNITY SHADERS' True PBR MARKER, measured out
                // of CommunityShaders.dll on 2026-08-02: its GetRenderPasses
                // thunk tests 1 << 42 against the flags at +0x38. It is the one
                // flag that says the shape is not being drawn by the shader this
                // whole module reasons about, and leaving it off this list is
                // why two readings could not tell a vanilla eye from a CS one.
                { F::kVertexLighting, "PBR(vertexLighting)" },
            };
            std::string out;
            for (const auto& f : kFlags) {
                if (a_prop->flags.any(f.bit)) {
                    if (!out.empty()) {
                        out += '+';
                    }
                    out += f.name;
                }
            }
            return out.empty() ? std::string{ "none" } : out;
        }

    }  // namespace

    std::string Describe(RE::BSLightingShaderProperty* a_prop) {
        if (!a_prop || !a_prop->material) {
            return "(no lighting material)";
        }
        auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(a_prop->material);
        const auto  feature = mat->GetFeature();

        // The base slots first, in the order BSLightingShaderMaterialBase holds
        // them, so a diff between two of these lines reads down the struct.
        auto out = fmt::format(
            "feat={}({}) mat={} set={} diffuse='{}' normal='{}' rimSoft='{}' specBack='{}'",
            FeatureName(feature), static_cast<int>(feature), static_cast<void*>(mat),
            static_cast<void*>(mat->textureSet.get()), TextureName(mat->diffuseTexture.get()),
            TextureName(mat->normalTexture.get()),
            TextureName(mat->rimSoftLightingTexture.get()),
            TextureName(mat->specularBackLightingTexture.get()));

        // ⚠ THE DERIVED SLOT IS THE WHOLE POINT AND IT IS FEATURE GATED FOR THE
        // REASON THE SWAP'S OWN FINISH PROBE RECORDS: these classes differ in
        // SIZE, so reading +0xA0 off a material that does not have it is a read
        // past the end of the allocation. Create() is virtual and returns the
        // source's own type, so the feature is what says which type this is.
        switch (feature) {
            case RE::BSShaderMaterial::Feature::kGlowMap: {
                auto* const glow = static_cast<RE::BSLightingShaderMaterialGlowmap*>(mat);
                out += fmt::format(" glow='{}'", TextureName(glow->glowTexture.get()));
                break;
            }
            case RE::BSShaderMaterial::Feature::kEye: {
                auto* const eye = static_cast<RE::BSLightingShaderMaterialEye*>(mat);
                out += fmt::format(" env='{}' envMask='{}' envMapScale={:.3f}",
                                   TextureName(eye->envTexture.get()),
                                   TextureName(eye->envMaskTexture.get()), eye->envMapScale);
                break;
            }
            case RE::BSShaderMaterial::Feature::kEnvironmentMap: {
                auto* const env = static_cast<RE::BSLightingShaderMaterialEnvmap*>(mat);
                out += fmt::format(" env='{}' envMask='{}' envMapScale={:.3f}",
                                   TextureName(env->envTexture.get()),
                                   TextureName(env->envMaskTexture.get()), env->envMapScale);
                break;
            }
            default:
                break;
        }

        // The property's own half. An emissive that is live is the one way a
        // material whose every slot checks out can still put a colour on screen
        // that nobody asked for, and the flags are what decide whether any of
        // the slots above are sampled at all.
        const auto* const emit = a_prop->emissiveColor;
        out += fmt::format(
            " | matAlpha={:.3f} specPower={:.1f} specScale={:.3f} propAlpha={:.3f}"
            " emit={} emitMult={:.3f} flags={}",
            mat->materialAlpha, mat->specularPower, mat->specularColorScale, a_prop->alpha,
            emit ? fmt::format("{:02X}{:02X}{:02X}",
                               static_cast<int>(emit->red * 255.0f + 0.5f),
                               static_cast<int>(emit->green * 255.0f + 0.5f),
                               static_cast<int>(emit->blue * 255.0f + 0.5f))
                 : std::string{ "(null)" },
            a_prop->emissiveMult, FlagNames(a_prop));
        return out;
    }

}  // namespace OS::MaterialReading
