#include "MeshExtractor.h"

#include "LegacyGeometry.h"      // where a NiGeometry keeps its blocks, measured
#include "PreviewFilter.h"       // the scene's name rules, pure and tested
#include "PreviewGrid.h"         // FoldPath, the subsystem's one fold
#include "PreviewSwapCapture.h"  // LoaderOk, the swap feature's one gate
#include "SkinBindPose.h"        // where a skinned mesh stands, pure and tested
#include "SkinPlan.h"            // FileName: the skin pack rule a by-name swap uses
#include "TextureLoad.h"         // the engine's own loader by path, shared

#include <Windows.h>  // the SEH vocabulary

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <unordered_map>

namespace OS::MeshExtractor {

    namespace {

        // ---- small maths ------------------------------------------------

        RE::NiPoint3 TransformNormal(const RE::NiTransform& a_xf, float a_x, float a_y,
                                     float a_z) {
            // Rotation only: a normal takes the transform's rotation and
            // ignores its translation and (uniform) scale, then renormalises.
            const auto& r = a_xf.rotate.entry;
            RE::NiPoint3 n{ r[0][0] * a_x + r[0][1] * a_y + r[0][2] * a_z,
                            r[1][0] * a_x + r[1][1] * a_y + r[1][2] * a_z,
                            r[2][0] * a_x + r[2][1] * a_y + r[2][2] * a_z };
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 1.0e-6f) {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            }
            return n;
        }

        void IncludeBoundsPoint(std::array<float, 3>& a_min, std::array<float, 3>& a_max,
                                bool& a_have, const RE::NiPoint3& a_p) {
            if (!a_have) {
                a_min = { a_p.x, a_p.y, a_p.z };
                a_max = { a_p.x, a_p.y, a_p.z };
                a_have = true;
                return;
            }
            a_min[0] = (std::min)(a_min[0], a_p.x);
            a_min[1] = (std::min)(a_min[1], a_p.y);
            a_min[2] = (std::min)(a_min[2], a_p.z);
            a_max[0] = (std::max)(a_max[0], a_p.x);
            a_max[1] = (std::max)(a_max[1], a_p.y);
            a_max[2] = (std::max)(a_max[2], a_p.z);
        }

        std::string GetNodeName(RE::NiAVObject* a_object) {
            const char* n = a_object ? a_object->name.c_str() : nullptr;
            return n ? n : "";
        }

        // ---- the tree walk (spec lines 515-534) -------------------------

        // ⚠ EXACTLY ONE OF THE TWO POINTERS IS SET, and they are separate
        // fields rather than a common base because the engine gives them no
        // useful one: a `BSGeometry` and a legacy `NiGeometry` are siblings
        // under `NiAVObject`, they keep their properties, skin and vertex data
        // at different offsets, and neither can be cast to the other. Every
        // reader here asks which it has.
        struct GeometryCandidate {
            RE::BSGeometry* geometry{ nullptr };
            RE::NiGeometry* legacy{ nullptr };
            std::string     namePath;
            RE::NiTransform transform;
            bool            hidden{ false };
        };

        // ⚠⚠ A LEAF THAT IS NEITHER GEOMETRY NOR NODE IS NOT NECESSARILY
        // NOTHING, AND ASSUMING IT WAS COST A WHOLE CLASS OF BLANK CARD.
        // A legacy BSVersion 83 mesh carries `NiTriShape`, which AE keeps as a
        // live class registered in the NIF stream factory beside `BSTriShape`
        // and does NOT convert on load. It is not a `BSGeometry`: its
        // `AsGeometry` at NiAVObject vtable slot 7 is `33 C0 C3`, xor eax,eax
        // and ret, against `BSGeometry`'s `48 8B C1 C3`. So the walk below used
        // to drop it before any filter ran, with no line naming it, and the
        // card came back `status=ready` showing the mannequin alone
        // (field 2026-08-20, Ahzidal's Armored Robes: `meshes=4 skin=4`).
        //
        // ⚠ IDENTIFIED THROUGH THE RTTI NAME CHAIN RATHER THAN A CAST.
        // CommonLibSSE-NG declares `NiGeometry` with no `RTTI` constant, so
        // `netimmerse_cast` will not compile for it; the name walk needs no
        // relocation and cannot be wrong about what the object is.
        [[nodiscard]] bool RttiChainNames(const RE::NiObject* a_object,
                                          std::string_view    a_class) {
            if (!a_object) {
                return false;
            }
            for (const auto* rtti = a_object->GetRTTI(); rtti; rtti = rtti->GetBaseRTTI()) {
                const char* name = rtti->GetName();
                if (name && std::string_view{ name } == a_class) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] RE::NiGeometry* AsLegacyGeometry(RE::NiAVObject* a_object) {
            return RttiChainNames(a_object, "NiGeometry")
                       ? static_cast<RE::NiGeometry*>(a_object)
                       : nullptr;
        }

        // What a legacy shape can actually offer, reported once per shape.
        // ⚠ THIS IS A MEASUREMENT, NOT A FIX. Whether `NiGeometryData` still
        // holds its CPU arrays at runtime is governed by `keepFlags` and was
        // never measured; it decides whether reading one of these is a short
        // extra source path or needs a NIF reader of our own.
        struct LegacyFacts {
            std::uint32_t code{ 0 };  // SEH code; 0 means the read completed
            bool          hasData{ false };
            const char*   dataRtti{ nullptr };
            std::uint32_t verts{ 0 };
            bool          hasPos{ false };
            bool          hasNrm{ false };
            bool          hasUV{ false };
            std::uint32_t keep{ 0 };
            std::uint32_t triPoints{ 0 };
            bool          hasTris{ false };
            const char*   litRtti{ nullptr };
            const char*   alphaRtti{ nullptr };
            bool          hasSkin{ false };
            const char*   skinRtti{ nullptr };
            std::uint32_t parts{ 0 };
            std::uint32_t partVerts{ 0 };
            bool          partPos{ false };
        };

        // ⚠⚠ UNDER SEH, AND THE FIRST CUT OF THIS WAS NOT, WHICH CRASHED THE
        // GAME (field 2026-08-20, browsing preset cards: access violation in
        // this function reading `skinPartition->numPartitions` off a pointer
        // that was not one). Every offset touched here comes from CommonLib's
        // idea of a class the walk has never handled before, which is the
        // definition of "memory whose shape a third party chose" that the
        // ExtractOneSEH comment below already sets the rule for. A probe gets
        // the same protection as a feature.
        //
        // ⚠ NO C++ OBJECTS IN THIS FRAME (MSVC forbids __try beside unwinding).
        // Raw pointers and a POD out-parameter only; the caller formats.
        __declspec(noinline) void ReadLegacyFactsSEH(RE::NiGeometry* a_geometry,
                                                     LegacyFacts*    a_out) {
            __try {
                // ⚠⚠ MEASURED OFFSETS, NOT `GetRuntimeData()`. CommonLib has
                // the skin instance and the model data the wrong way round on
                // this class, which is why the first cut of this probe read the
                // skin where the geometry lives and faulted on all 17 shapes.
                // LegacyGeometry.h carries the three functions that measure it.
                auto* data     = LegacyGeometry::ModelData(a_geometry);
                a_out->hasData = data != nullptr;
                if (data) {
                    // The data's own RTTI name is the tell for whether these
                    // offsets are being read where they live: a real class name
                    // means the pointer is a real object.
                    const auto* drtti = data->GetRTTI();
                    a_out->dataRtti   = drtti ? drtti->GetName() : nullptr;
                    a_out->verts      = data->vertices;
                    a_out->hasPos     = data->vertex != nullptr;
                    a_out->hasNrm     = data->normal != nullptr;
                    a_out->hasUV      = data->texture != nullptr;
                    a_out->keep = static_cast<std::uint32_t>(data->keepFlags.underlying());
                    // Only a triangle list has these two; a NiTriStripsData
                    // stores strips and the offsets mean something else there.
                    if (a_out->dataRtti &&
                        std::string_view{ a_out->dataRtti } == "NiTriShapeData") {
                        a_out->triPoints = LegacyGeometry::NumTrianglePoints(data);
                        a_out->hasTris   = LegacyGeometry::Triangles(data) != nullptr;
                    }
                }
                // The material half, and the whole of what is known about it is
                // that this is the 3rd stream ref. A legacy file may carry
                // NiMaterialProperty rather than BSLightingShaderProperty, so
                // the name is the measurement and nothing is assumed from it.
                if (auto* lit = LegacyGeometry::ShaderProperty(a_geometry)) {
                    const auto* lrtti = lit->GetRTTI();
                    a_out->litRtti    = lrtti ? lrtti->GetName() : nullptr;
                }
                if (auto* alpha = LegacyGeometry::AlphaProperty(a_geometry)) {
                    const auto* artti = alpha->GetRTTI();
                    a_out->alphaRtti  = artti ? artti->GetName() : nullptr;
                }
                auto* skin     = LegacyGeometry::SkinInstance(a_geometry);
                a_out->hasSkin = skin != nullptr;
                if (skin) {
                    const auto* srtti = skin->GetRTTI();
                    a_out->skinRtti   = srtti ? srtti->GetName() : nullptr;
                    auto* part        = skin->skinPartition.get();
                    if (part) {
                        a_out->parts     = part->numPartitions;
                        a_out->partVerts = part->vertexCount;
                        if (part->numPartitions > 0) {
                            a_out->partPos = part->partitions[0].vertexDesc.HasFlag(
                                RE::BSGraphics::Vertex::Flags::VF_VERTEX);
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                a_out->code = static_cast<std::uint32_t>(GetExceptionCode());
            }
        }

        void ReportLegacyGeometry(RE::NiGeometry* a_geometry, const std::string& a_path,
                                  const char* a_rttiName) {
            // ⚠⚠ THE SAFE HALF IS LOGGED FIRST AND ON ITS OWN LINE. The first
            // cut put every fact in one call after the risky reads, so when
            // those faulted the log carried NOTHING and the round bought only a
            // crash address. What the object IS costs one virtual call and
            // cannot fault, so it is never bundled with what it holds.
            spdlog::info("MeshExtractor: preview.legacy-geometry '{}' rtti={}", a_path,
                         a_rttiName ? a_rttiName : "?");
            LegacyFacts facts{};
            ReadLegacyFactsSEH(a_geometry, &facts);
            if (facts.code != 0) {
                spdlog::warn("MeshExtractor: preview.legacy-geometry '{}' FAULTED "
                             "0x{:08X} reading its runtime data; the layout is not "
                             "where CommonLib puts it and nothing here can be trusted.",
                             a_path, facts.code);
                return;
            }
            spdlog::info("MeshExtractor: preview.legacy-geometry '{}' data={} dataRtti={} "
                         "verts={} pos={} nrm={} uv={} keep=0x{:02X} triPoints={} tris={} "
                         "lit={} alpha={} skin={} skinRtti={} parts={} partVerts={} "
                         "partPos={}",
                         a_path, facts.hasData, facts.dataRtti ? facts.dataRtti : "?",
                         facts.verts, facts.hasPos, facts.hasNrm, facts.hasUV, facts.keep,
                         facts.triPoints, facts.hasTris,
                         facts.litRtti ? facts.litRtti : "-",
                         facts.alphaRtti ? facts.alphaRtti : "-", facts.hasSkin,
                         facts.skinRtti ? facts.skinRtti : "?", facts.parts,
                         facts.partVerts, facts.partPos);
        }

        void CollectGeometries(RE::NiAVObject* a_object, const std::string& a_parentPath,
                               bool a_parentHidden, const RE::NiTransform& a_parentTransform,
                               std::vector<GeometryCandidate>& a_out,
                               std::uint32_t& a_legacy) {
            if (!a_object) {
                return;
            }
            const std::string nodeName = GetNodeName(a_object);
            const std::string namePath =
                a_parentPath.empty() ? nodeName : a_parentPath + "/" + nodeName;
            const bool hiddenByNode    = a_parentHidden || a_object->GetAppCulled();
            const RE::NiTransform xf   = a_parentTransform * a_object->local;

            if (auto* geometry = a_object->AsGeometry()) {
                a_out.push_back({ geometry, nullptr, namePath, xf, hiddenByNode });
                return;
            }
            if (auto* node = a_object->AsNode()) {
                // GetChildren(), never the member: this DLL is universal and
                // the array sits at 0x110 or 0x138 per runtime (NpcHair.cpp
                // carries the same rule with the same reason).
                for (auto& child : node->GetChildren()) {
                    if (child) {
                        CollectGeometries(child.get(), namePath, hiddenByNode, xf, a_out,
                                          a_legacy);
                    }
                }
                return;
            }
            if (auto* legacy = AsLegacyGeometry(a_object)) {
                ++a_legacy;
                a_out.push_back({ nullptr, legacy, namePath, xf, hiddenByNode });
            }
        }

        // The world translation of the first head bone under a_object, or
        // nothing. A separate walk from CollectGeometries on purpose: that one
        // returns as soon as it reaches geometry, and a skeleton node carries
        // no geometry of its own, so folding this into it would mean changing
        // the shape of the hot path to answer a question nine cards ask.
        void FindHeadAnchor(RE::NiAVObject* a_object, const RE::NiTransform& a_parent,
                            NodeAnchor& a_out) {
            if (!a_object || a_out.valid) {
                return;
            }
            const RE::NiTransform xf = a_parent * a_object->local;
            if (PreviewFilter::IsHeadBoneName(PreviewGrid::FoldPath(GetNodeName(a_object)))) {
                a_out = { true, xf.translate.x, xf.translate.y, xf.translate.z };
                return;
            }
            if (auto* node = a_object->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (child) {
                        FindHeadAnchor(child.get(), xf, a_out);
                        if (a_out.valid) {
                            return;
                        }
                    }
                }
            }
        }

        // ---- the bind pose ------------------------------------------------

        [[nodiscard]] SkinBindPose::Transform ToPure(const RE::NiTransform& a_xf) {
            SkinBindPose::Transform out{};
            const auto&             r = a_xf.rotate.entry;
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    out.rotate[row * 3 + col] = r[row][col];
                }
            }
            out.translate = { a_xf.translate.x, a_xf.translate.y, a_xf.translate.z };
            out.scale     = a_xf.scale;
            return out;
        }

        [[nodiscard]] RE::NiTransform FromPure(const SkinBindPose::Transform& a_xf) {
            RE::NiTransform out{};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    out.rotate.entry[row][col] = a_xf.rotate[row * 3 + col];
                }
            }
            out.translate = { a_xf.translate[0], a_xf.translate[1], a_xf.translate[2] };
            out.scale     = a_xf.scale;
            return out;
        }

        // A bone's world transform, accumulated UP the parent chain.
        //
        // ⚠⚠ `world` IS NOT READ, AND A STANDALONE-LOADED NIF IS EXACTLY WHY.
        // Nothing runs an update pass over a scene that was loaded to be
        // photographed, so `world` is whatever the loader left. CollectGeometries
        // and FindHeadAnchor both accumulate `local` DOWNWARD for the same
        // reason; this walks the same transforms upward, so any node all three
        // reach gets the same answer.
        //
        // ⚠ THE WALK STOPS WHERE THE DESCENT STARTS. CollectGeometries begins at
        // the scene root and applies that root's own local, and a bone chain ends
        // at the same root with a null parent, so the two include exactly the
        // same links and the products below are in the candidate's own space.
        [[nodiscard]] RE::NiTransform WorldOf(RE::NiAVObject* a_object) {
            RE::NiTransform out{};
            // A depth cap rather than trust: a malformed file with a parent cycle
            // would otherwise hang the render thread inside a card build.
            constexpr int kMaxDepth = 64;
            int           depth     = 0;
            for (RE::NiAVObject* node = a_object; node && depth < kMaxDepth;
                 node = node->parent, ++depth) {
                out = node->local * out;
            }
            return out;
        }

        // boneWorld * skinToBone for every bone the skin resolves, which is what
        // SkinBindPose::Decide reads. Empty means "nothing to say", never "the
        // identity": the two have different answers and the header separates them.
        void CollectBindProducts(RE::NiSkinInstance*                   a_skin,
                                 std::vector<SkinBindPose::Transform>& a_out) {
            a_out.clear();
            if (!a_skin || !a_skin->bones) {
                return;
            }
            auto* data = a_skin->skinData.get();
            if (!data || !data->boneData || data->bones == 0) {
                return;
            }
            // ⚠ THE BONE ARRAY AND THE BONE DATA ARE TWO ALLOCATIONS AND THIS
            // TRUSTS NEITHER TO BOUND THE OTHER. The file format keeps them the
            // same length and a sane file does; a cap costs nothing and a
            // disagreement here reads a stranger's memory on the render thread.
            constexpr std::uint32_t kMaxBones = 512;
            const std::uint32_t     count     = (std::min)(data->bones, kMaxBones);
            a_out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto* bone = a_skin->bones[i];
                if (!bone) {
                    continue;
                }
                a_out.push_back(ToPure(WorldOf(bone) * data->boneData[i].skinToBone));
            }
        }

        // ---- the material (Wardrobe's resolver, minus what cannot port) --

        struct MaterialInfo {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Diffuse;
            bool                 UseAlpha{ false };
            bool                 BlendAlpha{ false };
            float                MaterialAlpha{ 1.0f };
            float                AlphaCutoff{ 0.0f };
            float                SpecularStrength{ 0.10f };
            float                SpecularPower{ 42.0f };
            std::array<float, 3> EmissiveColor{ 0.0f, 0.0f, 0.0f };
            float                EmissiveStrength{ 0.0f };
        };

        float ClampFinite(float a_v, float a_min, float a_max, float a_fallback) {
            if (!std::isfinite(a_v)) {
                return a_fallback;
            }
            return (std::clamp)(a_v, a_min, a_max);
        }

        ID3D11ShaderResourceView* TextureSrv(RE::NiTexture* a_texture) {
            auto* src = netimmerse_cast<RE::NiSourceTexture*>(a_texture);
            if (!src || !src->rendererTexture) {
                return nullptr;
            }
            // ⚠ RE::BSGraphics::Texture is forward-declared only in this
            // CommonLib; that class IS RE::NiTexture::RendererData, the same
            // 0x28 bytes, resourceView at 0x10 (DyeTexture.cpp:24-36 carries
            // the whole argument). Nothing past offset 0x10 is trusted.
            auto* rd = reinterpret_cast<RE::NiTexture::RendererData*>(src->rendererTexture);
            return rd ? reinterpret_cast<ID3D11ShaderResourceView*>(rd->resourceView)
                      : nullptr;
        }

        // ⚠ NO BSShaderManager FALLBACK, MEASURED INSTEAD. This CommonLib does
        // not have the class, and the spec's own task order is: count how
        // often the direct SRV resolves before deciding the fallback matters.
        // diffuseResolved / diffuseMissing in the stats are that count.
        //
        // ⚠⚠ IT TAKES THE TWO PROPERTIES, NOT THE GEOMETRY, and that is what
        // lets one resolver serve both kinds of shape. A `BSGeometry` keeps
        // them in `properties[kEffect]` and `properties[kProperty]`; a legacy
        // `NiGeometry` keeps the same two at its own measured offsets
        // (LegacyGeometry.h). Writing a second resolver for the legacy path
        // would mean two readers of one answer, which drift apart in the gap.
        MaterialInfo ResolveMaterialFrom(RE::NiProperty* a_shader, RE::NiProperty* a_alpha,
                                         ExtractStats& a_stats, bool a_greyBody) {
            MaterialInfo info;
            auto* shaderProperty =
                netimmerse_cast<RE::BSLightingShaderProperty*>(a_shader);
            if (shaderProperty && shaderProperty->material) {
                auto* material =
                    static_cast<RE::BSLightingShaderMaterialBase*>(shaderProperty->material);
                // ⚠ THE MANNEQUIN IS GREY BY NOT ASKING (OS-204). Leaving the
                // diffuse null takes the shader's no-texture branch, so the
                // body comes out flat warm grey with the studio rig on it and
                // nothing here writes a material, which is the pooled-material
                // CTD's whole lesson.
                //
                // ⚠ A BODY CARD ASKS FOR THE SAME THING and that is why this
                // flag is not called a_mannequin any more. A body's own
                // reference mesh DOES resolve a skin diffuse, unlike the
                // mannequin, so a body card came back textured while every
                // other card in the app is a grey figure (field 2026-08-10,
                // user's call). The framing flag stayed behind: a body card
                // carries no mannequin and must not be measured like one.
                //
                // ⚠⚠ AND IT MUST NOT COUNT EITHER WAY. diffuse=N/N is a health
                // signal for the ITEM, read out of field logs to tell a
                // texture failure from a working card. A grey-on-purpose body
                // landing in diffuseMissing would put a permanent shortfall in
                // every armour line and poison every future diagnosis from
                // one.
                if (a_greyBody) {
                    // fall through with no diffuse and no counter touched
                } else if (auto* srv = TextureSrv(material->diffuseTexture.get())) {
                    info.Diffuse = srv;
                    ++a_stats.diffuseResolved;
                } else {
                    ++a_stats.diffuseMissing;
                }

                const float propertyAlpha =
                    ClampFinite(shaderProperty->alpha, 0.0f, 1.0f, 1.0f);
                const float materialAlpha =
                    ClampFinite(material->materialAlpha, 0.0f, 1.0f, 1.0f);
                info.MaterialAlpha = (std::clamp)(propertyAlpha * materialAlpha, 0.0f, 1.0f);

                using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
                const bool blends =
                    info.MaterialAlpha < 0.995f ||
                    shaderProperty->flags.any(Flag::kScreendoorAlphaFade) ||
                    shaderProperty->flags.any(Flag::kPremultAlpha) ||
                    shaderProperty->flags.any(Flag::kVertexAlpha);
                info.UseAlpha   = blends;
                info.BlendAlpha = blends;

                if (shaderProperty->flags.any(Flag::kSpecular)) {
                    const float scale =
                        ClampFinite(material->specularColorScale, 0.0f, 2.5f, 1.0f);
                    info.SpecularStrength =
                        (std::clamp)(0.04f + scale * 0.055f, 0.02f, 0.18f);
                } else {
                    info.SpecularStrength = 0.045f;
                }
                info.SpecularPower =
                    ClampFinite(material->specularPower, 10.0f, 96.0f, 42.0f);

                const bool hasEmissive =
                    shaderProperty->emissiveColor &&
                    (shaderProperty->flags.any(Flag::kOwnEmit) ||
                     shaderProperty->flags.any(Flag::kGlowMap) ||
                     shaderProperty->emissiveMult > 0.01f);
                if (hasEmissive) {
                    info.EmissiveColor = {
                        ClampFinite(shaderProperty->emissiveColor->red, 0.0f, 1.0f, 0.0f),
                        ClampFinite(shaderProperty->emissiveColor->green, 0.0f, 1.0f, 0.0f),
                        ClampFinite(shaderProperty->emissiveColor->blue, 0.0f, 1.0f, 0.0f)
                    };
                    info.EmissiveStrength =
                        ClampFinite(shaderProperty->emissiveMult * 0.12f, 0.0f, 0.32f, 0.0f);
                }
            }

            auto* alphaProperty = netimmerse_cast<RE::NiAlphaProperty*>(a_alpha);
            if (alphaProperty) {
                const bool testing  = alphaProperty->GetAlphaTesting();
                const bool blending = alphaProperty->GetAlphaBlending();
                info.UseAlpha   = info.UseAlpha || testing || blending;
                // ⚠ NOT "blending is on". A blend this pass owns no state for,
                // carrying an alpha test, is a CUTOUT: the test shapes it and
                // the exotic blend cannot be honoured anyway, so it draws
                // opaque with its cutoff. Blending it instead put the Bound
                // Arrow's own arrows in the alpha pass, which does not write
                // depth, and they ghosted through each other (2026-08-21).
                info.BlendAlpha =
                    info.BlendAlpha ||
                    PreviewFilter::DrawsInAlphaPass(
                        blending, testing,
                        static_cast<int>(alphaProperty->GetSrcBlendMode()),
                        static_cast<int>(alphaProperty->GetDestBlendMode()));
                if (testing) {
                    const float threshold = alphaProperty->alphaThreshold > 0
                                                ? alphaProperty->alphaThreshold / 255.0f
                                                : 0.04f;
                    info.AlphaCutoff = (std::clamp)(threshold, 0.005f, 0.95f);
                } else if (blending) {
                    info.AlphaCutoff = 0.005f;
                }
            }
            return info;
        }

        MaterialInfo ResolveMaterial(RE::BSGeometry* a_geometry, ExtractStats& a_stats,
                                     bool a_greyBody) {
            auto& rt = a_geometry->GetGeometryRuntimeData();
            return ResolveMaterialFrom(rt.properties[RE::BSGeometry::States::kEffect].get(),
                                       rt.properties[RE::BSGeometry::States::kProperty].get(),
                                       a_stats, a_greyBody);
        }

        // ---- the texture swap (OS-192), the card's own diffuse ----------

        // ⚠⚠ FIELD LESSON (2026-08-09 round 1, the hover CTD): the first
        // build retextured the MESH'S MATERIAL the engine's way,
        // ClearTextures + OnLoadTextureSet. That material is not ours to
        // write. Shader materials are POOLED through a global manager
        // (BSShaderProperty::SetMaterial dedups and releases through it,
        // re_verify/ae_crash_chain.c), so the material reached from the
        // standalone tree can be THE instance other loads share; the
        // engine's own applier makes the property's material unique before
        // mutating, the step a port must not skip. Worse, the Eyes
        // override of OnLoadTextureSet fills the derived envTexture and
        // envMaskTexture from set slots 4 and 5 (re_verify/
        // ae_eyes_onload.c), so a diffuse-only set NULLED them on the
        // pooled eye material, and the eye path of SetupMaterial binds
        // envTexture with no null check (re_verify/ae_crash_chain.c).
        // Hovering an eye rebuilt the player's eye from the polluted pool
        // and the next world frame dereferenced null.
        //
        // So NO material is touched here, ever. The swap's diffuse loads
        // through the texture set's own loader vfunc into OUR pointer, and
        // the extracted mesh's SRV is overridden after the fact. The
        // engine texture cache dedups by path underneath, which is the
        // sharing textures always had (phases 1-3, field-clean).

        // The set's loader (vfunc slot 38, the exact call Base::
        // OnLoadTextureSet makes) lives in TextureLoad now, shared with the
        // overlay bake, so there is one SEH frame and one prefix rule.

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadSwapDiffuse(
            const PreviewGrid::TextureSwapEntry& a_swap) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> out;
            if (!OS::PreviewSwapCapture::LoaderOk() || a_swap.texPaths[0].empty()) {
                return out;
            }
            // The engine itself hands the loader a stack buffer with this
            // exact prefix (re_verify/ae_swap_helpers.c).
            //
            // ⚠⚠ NORMALISED FIRST, AND THAT IS THE WHOLE OF THE 2026-08-09
            // PURPLE-EYE DEFECT. This prepended the root unconditionally, so a
            // TXST that spells its path "textures\..." (which authors do, and
            // which the engine itself accepts) became
            // Data\Textures\textures\... , a file that does not exist.
            //
            // ⚠⚠ A MISSING TEXTURE IS NOT AN ERROR TO THIS ENGINE. It
            // substitutes the purple placeholder and still hands back a live
            // NiSourceTexture, so the swap counted as applied, the diffuse
            // counted as resolved, preview.swap-no-diffuse stayed at zero, and
            // 116 demon-eye cards rendered the identical lavender ball with a
            // completely clean log. Nothing downstream of here can tell the
            // placeholder from a real texture, which is why the fix has to be
            // at the path and why the line below reports what it sent.
            const std::string folded = PreviewGrid::FoldPath(a_swap.texPaths[0]);
            const auto        normalised = PreviewGrid::NormalizeTexturePath(folded);
            const std::string rooted     = "Data\\Textures\\" + normalised;
            // One line per session when a path needed stripping. The failure
            // this replaces was invisible in every existing counter, so the
            // instrument has to name the path that actually reached the
            // engine rather than the one the record spelled.
            if (normalised.size() != folded.size()) {
                static bool s_said = false;
                if (!s_said) {
                    s_said = true;
                    spdlog::info("MeshExtractor: swap texture paths in this load order "
                                 "carry a leading 'textures\\' and are normalised before "
                                 "loading (first: '{}' -> '{}').",
                                 a_swap.texPaths[0], rooted);
                }
            }
            const auto tex = OS::TextureLoad::LoadRooted(rooted);
            if (!tex) {
                return out;  // faulted or no loader; TextureLoad said which
            }
            // The ComPtr's AddRef is what keeps the SRV alive after the
            // NiPointers release: the same lifetime story every extracted
            // diffuse already lives by.
            out = TextureSrv(tex.get());
            return out;
        }

        // ---- dynamic positions, with OOB 3 fixed ------------------------

        // FIX (OOB 3): Wardrobe accepts dataSize == vertexCount as well as
        // dataSize >= vertexCount * 16, for the sake of old creation tools.
        // When the two coincide it hands back a span over one sixteenth of
        // the memory it claims. The byte count is required here.
        std::span<const DirectX::XMFLOAT4> GetDynamicPositions(RE::BSTriShape* a_shape,
                                                               std::uint32_t a_vertexCount) {
            auto* dynamicShape = a_shape ? a_shape->AsDynamicTriShape() : nullptr;
            if (!dynamicShape || a_vertexCount == 0) {
                return {};
            }
            auto& dyn = dynamicShape->GetDynamicTrishapeRuntimeData();
            if (!dyn.dynamicData) {
                return {};
            }
            const auto requiredBytes =
                static_cast<std::size_t>(a_vertexCount) * sizeof(DirectX::XMFLOAT4);
            if (dyn.dataSize < requiredBytes) {
                return {};
            }
            return { static_cast<const DirectX::XMFLOAT4*>(dyn.dynamicData),
                     a_vertexCount };
        }

        // ---- everything one geometry needs, gathered up front -----------

        struct SourceGeometry {
            RE::BSGraphics::TriShape*          RendererGeometry{ nullptr };
            RE::NiTransform                    Transform;
            std::uint32_t                      VertexStride{ 0 };
            std::uint32_t                      VertexCount{ 0 };
            std::uint32_t                      TriangleCount{ 0 };
            std::uint32_t                      PositionOffset{ 0 };
            std::uint32_t                      UVOffset{ 0 };
            std::uint32_t                      NormalOffset{ 0 };
            bool                               HasUV{ false };
            bool                               HasNormal{ false };
            // ⚠ POSITIONS ARE FOUR HALF FLOATS UNLESS VF_FULLPREC IS SET, and
            // most armour ships halves; weapons ship full floats, which is
            // why phase 1 never saw this. Reading float* on half data is
            // garbage geometry with a clean log.
            bool                               FullPrecision{ true };
            bool                               HasPosition{ true };
            std::span<const DirectX::XMFLOAT4> DynamicPositions;
        };

        bool BuildBuffers(ID3D11Device* a_device, const std::vector<PreviewVertex>& a_vertices,
                          const std::vector<std::uint32_t>& a_indices, RenderMesh& a_out) {
            if (a_vertices.empty() || a_indices.empty()) {
                return false;
            }
            D3D11_BUFFER_DESC vbDesc{};
            vbDesc.ByteWidth =
                static_cast<UINT>(a_vertices.size() * sizeof(PreviewVertex));
            vbDesc.Usage     = D3D11_USAGE_DEFAULT;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA vbData{};
            vbData.pSysMem = a_vertices.data();
            if (FAILED(a_device->CreateBuffer(&vbDesc, &vbData,
                                              a_out.VertexBuffer.GetAddressOf()))) {
                return false;
            }
            D3D11_BUFFER_DESC ibDesc{};
            ibDesc.ByteWidth =
                static_cast<UINT>(a_indices.size() * sizeof(std::uint32_t));
            ibDesc.Usage     = D3D11_USAGE_DEFAULT;
            ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA ibData{};
            ibData.pSysMem = a_indices.data();
            if (FAILED(a_device->CreateBuffer(&ibDesc, &ibData,
                                              a_out.IndexBuffer.GetAddressOf()))) {
                a_out.VertexBuffer.Reset();
                return false;
            }
            a_out.IndexCount = static_cast<std::uint32_t>(a_indices.size());
            return true;
        }

        // ---- the renderer-data path, spec Appendix C with both clamps ----

        bool BuildFromRendererData(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                                   const SourceGeometry& a_source, RenderMesh& a_out) {
            auto* renderer = a_source.RendererGeometry;
            if (!renderer || a_source.VertexStride == 0 || a_source.TriangleCount == 0) {
                return false;
            }
            // The members are RE::ID3D11Buffer*, CommonLib's own forward
            // declaration inside namespace RE; the objects are the real
            // interfaces, so the cast is a naming correction, not a reach.
            auto* vb = reinterpret_cast<ID3D11Buffer*>(renderer->vertexBuffer);
            auto* ib = reinterpret_cast<ID3D11Buffer*>(renderer->indexBuffer);
            if (!vb || !ib) {
                return false;
            }

            D3D11_BUFFER_DESC vbDesc{};
            vb->GetDesc(&vbDesc);

            // FIX (OOB 2): the vertex count can never exceed what the buffer
            // holds, whatever the NIF declared.
            const std::uint32_t maxVerts = vbDesc.ByteWidth / a_source.VertexStride;
            std::uint32_t       vertexCount =
                (std::min)(a_source.VertexCount != 0 ? a_source.VertexCount : maxVerts,
                           maxVerts);
            if (!a_source.HasPosition) {
                // A positionless layout (dynamic shape): the morph buffer is
                // the only position source, so it also caps the count. With
                // no morph buffer there is no geometry to build.
                vertexCount = (std::min)(
                    vertexCount,
                    static_cast<std::uint32_t>(a_source.DynamicPositions.size()));
            }
            if (vertexCount == 0) {
                return false;
            }

            D3D11_BUFFER_DESC staging = vbDesc;
            staging.Usage          = D3D11_USAGE_STAGING;
            staging.BindFlags      = 0;
            staging.MiscFlags      = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            Microsoft::WRL::ComPtr<ID3D11Buffer> stagingVB;
            if (FAILED(a_device->CreateBuffer(&staging, nullptr,
                                              stagingVB.GetAddressOf()))) {
                return false;
            }
            a_ctx->CopyResource(stagingVB.Get(), vb);  // stall 1 of 2

            D3D11_MAPPED_SUBRESOURCE mappedVB{};
            if (FAILED(a_ctx->Map(stagingVB.Get(), 0, D3D11_MAP_READ, 0, &mappedVB))) {
                return false;
            }

            std::vector<PreviewVertex> vertices(vertexCount);
            std::array<float, 3>       boundsMin{};
            std::array<float, 3>       boundsMax{};
            bool                       haveBounds = false;

            for (std::uint32_t i = 0; i < vertexCount; ++i) {
                const auto* v = static_cast<const std::uint8_t*>(mappedVB.pData) +
                                static_cast<std::size_t>(i) * a_source.VertexStride;
                RE::NiPoint3 raw;
                if (a_source.FullPrecision) {
                    const float* pf =
                        reinterpret_cast<const float*>(v + a_source.PositionOffset);
                    raw = { pf[0], pf[1], pf[2] };
                } else {
                    // Four half floats; the fourth is bitangent X, not a
                    // position component.
                    const auto* ph = reinterpret_cast<const std::uint16_t*>(
                        v + a_source.PositionOffset);
                    raw = { DirectX::PackedVector::XMConvertHalfToFloat(ph[0]),
                            DirectX::PackedVector::XMConvertHalfToFloat(ph[1]),
                            DirectX::PackedVector::XMConvertHalfToFloat(ph[2]) };
                }
                if (i < a_source.DynamicPositions.size()) {
                    const auto& d = a_source.DynamicPositions[i];
                    raw = { d.x, d.y, d.z };
                }

                const RE::NiPoint3 p = a_source.Transform * raw;
                IncludeBoundsPoint(boundsMin, boundsMax, haveBounds, p);
                vertices[i].Px = p.x;
                vertices[i].Py = p.y;
                vertices[i].Pz = p.z;

                // ⚠ NO "offset != 0" HERE: on a positionless dynamic layout
                // the UVs legitimately sit at offset 0. HasUV/HasNormal come
                // from the flag nibble, which is the honest presence test.
                if (a_source.HasUV) {
                    const auto* uv =
                        reinterpret_cast<const std::uint16_t*>(v + a_source.UVOffset);
                    vertices[i].U = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
                    vertices[i].V = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
                }
                if (a_source.HasNormal) {
                    const std::uint8_t* n = v + a_source.NormalOffset;
                    const RE::NiPoint3  tn =
                        TransformNormal(a_source.Transform, (n[0] / 255.0f) * 2.0f - 1.0f,
                                        (n[1] / 255.0f) * 2.0f - 1.0f,
                                        (n[2] / 255.0f) * 2.0f - 1.0f);
                    vertices[i].Nx = tn.x;
                    vertices[i].Ny = tn.y;
                    vertices[i].Nz = tn.z;
                }
            }
            a_ctx->Unmap(stagingVB.Get(), 0);

            D3D11_BUFFER_DESC ibDesc{};
            ib->GetDesc(&ibDesc);
            D3D11_BUFFER_DESC ibStaging = ibDesc;
            ibStaging.Usage          = D3D11_USAGE_STAGING;
            ibStaging.BindFlags      = 0;
            ibStaging.MiscFlags      = 0;
            ibStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            Microsoft::WRL::ComPtr<ID3D11Buffer> stagingIB;
            if (FAILED(a_device->CreateBuffer(&ibStaging, nullptr,
                                              stagingIB.GetAddressOf()))) {
                return false;
            }
            a_ctx->CopyResource(stagingIB.Get(), ib);  // stall 2 of 2

            D3D11_MAPPED_SUBRESOURCE mappedIB{};
            if (FAILED(a_ctx->Map(stagingIB.Get(), 0, D3D11_MAP_READ, 0, &mappedIB))) {
                return false;
            }

            // FIX (OOB 1): clamp on EVERY path. Wardrobe applies this only
            // when the source is hair, so a NIF whose declared triangleCount
            // overruns its real index buffer reads past the mapped staging
            // buffer on every non-hair card.
            const auto availableIndices =
                static_cast<std::uint32_t>(ibDesc.ByteWidth / sizeof(std::uint16_t));
            const std::uint32_t indexCount =
                (std::min)(a_source.TriangleCount * 3u, availableIndices);

            std::vector<std::uint32_t> indices(indexCount);
            const auto* src = static_cast<const std::uint16_t*>(mappedIB.pData);
            for (std::uint32_t i = 0; i < indexCount; ++i) {
                const std::uint32_t index = src[i];
                indices[i] = index < vertexCount ? index : 0u;  // degenerate, never OOB
            }
            a_ctx->Unmap(stagingIB.Get(), 0);

            if (!BuildBuffers(a_device, vertices, indices, a_out)) {
                return false;
            }
            if (haveBounds) {
                a_out.BoundsMin  = boundsMin;
                a_out.BoundsMax  = boundsMax;
                a_out.HaveBounds = true;
            }
            return true;
        }

        // The position field's width, measured from the descriptor's own
        // layout rather than trusted from VF_FULLPREC: the next attribute's
        // offset bounds the position bytes, 8 means four halves, 12 or more
        // means full floats. ⚠ THE FLAG LIED IN THE FIELD (2026-08-09,
        // armour round 1): reading floats where the buffer held halves gave
        // textured polygon soup, correct UVs, garbage positions. The offsets
        // proved themselves right in the same picture (the textures mapped),
        // so the layout is the authority and the flag is not consulted.
        // By value: the descriptor is one packed uint64 and its accessors
        // are non-const on this CommonLib.
        // How many bytes an attribute actually occupies, measured from the
        // gap to the next attribute that starts after it (or to the stride).
        //
        // ⚠⚠ MEASURED, NEVER ASSUMED, AND THAT IS THIS FILE'S OLDEST SCAR.
        // VF_FULLPREC lies about the position width, and believing it shipped
        // garbage geometry with a clean log and a green build. The blend data
        // is the same shape of question, so it gets the same answer: the
        // descriptor's own offsets are the only honest source.
        // Whether the descriptor actually CARRIES an attribute.
        //
        // ⚠⚠ GetAttributeOffset DOES NOT CHECK, and an absent attribute's
        // nibble is not zero. The accessor is
        // (desc >> (4*attr + 2)) & 0x3C, which is nibble[attr + 1] * 4: four
        // bits, in units of four bytes, no presence test anywhere in it.
        // Bits 4-7 are the worst of them, because they are not VA_POSITION's
        // offset at all - they are the DYNAMIC vertex size / 4, and
        // CommonLib's own SetAttributeOffset refusing to write VA_POSITION is
        // the tell. On a BSDynamicTriShape's partition VF_VERTEX is clear
        // (the positions live in the morph buffer) and those bits read 4, so
        // GetAttributeOffset(VA_POSITION) hands back a phantom 16 for a
        // buffer holding no positions at all.
        //
        // That phantom is what the field log was reporting. The dominant
        // dynamic descriptor is 0x0045A00030210046: UV at 0, normal at 4,
        // tangent at 8, skinning at 12, stride 24. Skinning IS the last
        // attribute there and the stride fallback alone would have answered
        // 12 correctly, but 16 satisfies "after skinning, before the stride",
        // won the search, and truncated the span to 4. 144 of 471 cards read
        // blend=4b for that reason and none of them had a four-byte layout.
        //
        // MEASURED 2026-08-10 over 84364 NIFs, 137368 skin partitions and 16
        // distinct descriptors on this load order: VA_SKINNING spans 12 bytes
        // in EVERY partition, static or dynamic, without exception.
        bool AttributePresent(RE::BSGraphics::VertexDesc a_desc,
                              RE::BSGraphics::Vertex::Attribute a_attr) {
            using V = RE::BSGraphics::Vertex;
            switch (a_attr) {
                case V::VA_POSITION:  return a_desc.HasFlag(V::VF_VERTEX);
                case V::VA_TEXCOORD0: return a_desc.HasFlag(V::VF_UV);
                case V::VA_TEXCOORD1: return a_desc.HasFlag(V::VF_UV_2);
                case V::VA_NORMAL:    return a_desc.HasFlag(V::VF_NORMAL);
                // ⚠ GetSize() only budgets the tangent when the normal is
                // there too, and a span that falls back to that stride has to
                // agree with how it was computed.
                case V::VA_BINORMAL:
                    return a_desc.HasFlag(V::VF_NORMAL) &&
                           a_desc.HasFlag(V::VF_TANGENT);
                case V::VA_COLOR:    return a_desc.HasFlag(V::VF_COLORS);
                case V::VA_SKINNING: return a_desc.HasFlag(V::VF_SKINNED);
                case V::VA_LANDDATA: return a_desc.HasFlag(V::VF_LANDDATA);
                case V::VA_EYEDATA:  return a_desc.HasFlag(V::VF_EYEDATA);
                default:             return false;
            }
        }

        std::uint32_t AttributeSpan(RE::BSGraphics::VertexDesc a_desc,
                                    RE::BSGraphics::Vertex::Attribute a_attr) {
            // An attribute that is not there occupies nothing, and its offset
            // is noise. Answering 0 says "absent" instead of inventing a width.
            if (!AttributePresent(a_desc, a_attr)) {
                return 0u;
            }
            const std::uint32_t stride = a_desc.GetSize();
            const std::uint32_t start  = a_desc.GetAttributeOffset(a_attr);
            std::uint32_t       next   = stride;
            // ⚠ VA_LANDDATA was missing from this list and is now in it. It
            // sits between skinning and eye data, so an absent one could never
            // shorten a span, but a PRESENT one would have been stepped over.
            const RE::BSGraphics::Vertex::Attribute all[] = {
                RE::BSGraphics::Vertex::VA_POSITION,
                RE::BSGraphics::Vertex::VA_TEXCOORD0,
                RE::BSGraphics::Vertex::VA_TEXCOORD1,
                RE::BSGraphics::Vertex::VA_NORMAL,
                RE::BSGraphics::Vertex::VA_BINORMAL,
                RE::BSGraphics::Vertex::VA_COLOR,
                RE::BSGraphics::Vertex::VA_SKINNING,
                RE::BSGraphics::Vertex::VA_LANDDATA,
                RE::BSGraphics::Vertex::VA_EYEDATA
            };
            for (const auto attr : all) {
                if (attr == a_attr || !AttributePresent(a_desc, attr)) {
                    continue;
                }
                const auto off = a_desc.GetAttributeOffset(attr);
                if (off > start && off < next) {
                    next = off;
                }
            }
            return next > start ? next - start : 0u;
        }

        bool PositionIsFullFloat(RE::BSGraphics::VertexDesc a_desc,
                                 std::uint32_t a_posOffset) {
            const std::uint32_t stride = a_desc.GetSize();
            std::uint32_t       next   = stride;
            const RE::BSGraphics::Vertex::Attribute others[] = {
                RE::BSGraphics::Vertex::VA_TEXCOORD0,
                RE::BSGraphics::Vertex::VA_TEXCOORD1,
                RE::BSGraphics::Vertex::VA_NORMAL,
                RE::BSGraphics::Vertex::VA_BINORMAL,
                RE::BSGraphics::Vertex::VA_COLOR,
                RE::BSGraphics::Vertex::VA_SKINNING,
                RE::BSGraphics::Vertex::VA_EYEDATA
            };
            for (const auto attr : others) {
                const auto off = a_desc.GetAttributeOffset(attr);
                if (off > a_posOffset && off < next) {
                    next = off;
                }
            }
            return next - a_posOffset >= 12;
        }

        // ---- what the morph did to the SHADING, measured then corrected ----
        //
        // ⚠⚠ THE MORPH MOVES POSITIONS AND NOTHING MOVED THE NORMALS WITH
        // THEM, so a strongly morphed body is lit for the shape it used to be.
        // That predicts the dark blotching exactly: no holes, no debris, no
        // scrambling, just patches too dark for the surface they sit on, worst
        // where the preset displaced the mesh furthest and mildest on the slim
        // presets. It had been written down as a known unjudged cost since
        // this feature started and nobody had measured it.
        //
        // ⚠⚠ ACCUMULATED BY SHAPE INDEX, NEVER BY OUTPUT POSITION, and that is
        // not tidiness. This engine hands each partition a buffer holding the
        // WHOLE shape, so the output array carries the body once per partition
        // while each partition contributes only its OWN triangles. Averaging
        // per output vertex would hand every vertex on the partition boundary a
        // one-sided normal in BOTH of its copies: a lighting crease straight
        // down the body, traded for the blotches. Folding both partitions into
        // one accumulator keyed by shape index closes it.
        //
        // ⚠⚠ THE STORED NORMAL IS ROTATED, NOT REPLACED, and that is what
        // makes this safe on a half-float buffer. Positions are four halves on
        // most of this content, so a face normal recomputed from them carries
        // quantisation noise of a few degrees; replacing the authored normals
        // with those trades stale shading for noisy shading. The base and the
        // morphed recompute share that noise EXACTLY, because the morph adds a
        // float delta to the same quantised position, so the rotation between
        // them cancels it and carries only what the morph actually did. It is
        // also the identity wherever the preset did not deform the surface,
        // which is most of the body on most presets.
        // The b8..b12 diagnostic harness that once lived here (fold bands,
        // edge-stretch petal test, partition byte-compare) found its bug: the
        // multi-shape .tri merge, fixed in BodyMorphData::ParseTri at b13 and
        // recorded there. What remains is the correction itself.
        struct MorphNormalReport {
            std::uint32_t vertices{ 0 };
            std::uint32_t noNormal{ 0 };
            std::uint32_t degenerate{ 0 };
            std::uint32_t rotated{ 0 };
        };

        bool NormaliseInPlace(RE::NiPoint3& a_n) {
            const float len =
                std::sqrt(a_n.x * a_n.x + a_n.y * a_n.y + a_n.z * a_n.z);
            if (len <= 1.0e-12f) {
                return false;
            }
            a_n.x /= len;
            a_n.y /= len;
            a_n.z /= len;
            return true;
        }

        void MeasureAndCorrectMorphedNormals(
            std::vector<PreviewVertex>&        a_vertices,
            const std::vector<std::uint32_t>&  a_indices,
            const std::vector<RE::NiPoint3>&   a_basePositions,
            const std::vector<std::uint32_t>&  a_shapeIndex,
            const std::vector<std::uint8_t>&   a_hadNormal,
            bool a_apply, MorphNormalReport& a_report) {
            // Parallel arrays or nothing: a mismatch means the vertex loop
            // stopped pushing them in step, and averaging across that would
            // silently shade the body by another vertex's neighbourhood.
            if (a_vertices.empty() || a_indices.size() < 3 ||
                a_basePositions.size() != a_vertices.size() ||
                a_shapeIndex.size() != a_vertices.size() ||
                a_hadNormal.size() != a_vertices.size()) {
                return;
            }

            std::uint32_t slots = 0;
            for (const auto s : a_shapeIndex) {
                slots = (std::max)(slots, s + 1u);
            }
            if (slots == 0) {
                return;
            }
            const RE::NiPoint3        zero{ 0.0f, 0.0f, 0.0f };
            std::vector<RE::NiPoint3> accBase(slots, zero);
            std::vector<RE::NiPoint3> accMorph(slots, zero);

            // Unnormalised on purpose: the cross product's length is twice the
            // triangle's area, which is the weight an authored smooth normal
            // carries. It also costs a degenerate triangle its vote for free,
            // and the triList clamp above manufactures one whenever an index
            // overruns the partition.
            const auto faceNormal = [](const RE::NiPoint3& a, const RE::NiPoint3& b,
                                       const RE::NiPoint3& c) {
                const RE::NiPoint3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
                const RE::NiPoint3 e2{ c.x - a.x, c.y - a.y, c.z - a.z };
                return RE::NiPoint3{ e1.y * e2.z - e1.z * e2.y,
                                     e1.z * e2.x - e1.x * e2.z,
                                     e1.x * e2.y - e1.y * e2.x };
            };

            const std::size_t triCount = a_indices.size() / 3;
            for (std::size_t t = 0; t < triCount; ++t) {
                const std::uint32_t i0 = a_indices[t * 3 + 0];
                const std::uint32_t i1 = a_indices[t * 3 + 1];
                const std::uint32_t i2 = a_indices[t * 3 + 2];
                if (i0 >= a_vertices.size() || i1 >= a_vertices.size() ||
                    i2 >= a_vertices.size()) {
                    continue;
                }
                const RE::NiPoint3 fb = faceNormal(
                    a_basePositions[i0], a_basePositions[i1], a_basePositions[i2]);
                const RE::NiPoint3 fm = faceNormal(
                    RE::NiPoint3{ a_vertices[i0].Px, a_vertices[i0].Py, a_vertices[i0].Pz },
                    RE::NiPoint3{ a_vertices[i1].Px, a_vertices[i1].Py, a_vertices[i1].Pz },
                    RE::NiPoint3{ a_vertices[i2].Px, a_vertices[i2].Py, a_vertices[i2].Pz });
                const std::uint32_t corners[3] = { i0, i1, i2 };
                for (const std::uint32_t v : corners) {
                    const std::uint32_t s = a_shapeIndex[v];
                    accBase[s].x += fb.x;
                    accBase[s].y += fb.y;
                    accBase[s].z += fb.z;
                    accMorph[s].x += fm.x;
                    accMorph[s].y += fm.y;
                    accMorph[s].z += fm.z;
                }
            }

            for (std::size_t v = 0; v < a_vertices.size(); ++v) {
                RE::NiPoint3 nb = accBase[a_shapeIndex[v]];
                RE::NiPoint3 nm = accMorph[a_shapeIndex[v]];
                if (!NormaliseInPlace(nb) || !NormaliseInPlace(nm)) {
                    ++a_report.degenerate;
                    continue;
                }

                const float bm = std::clamp(nb.x * nm.x + nb.y * nm.y + nb.z * nm.z,
                                            -1.0f, 1.0f);
                if (a_hadNormal[v] == 0) {
                    ++a_report.noNormal;
                }

                if (!a_apply) {
                    continue;
                }
                if (a_hadNormal[v] == 0) {
                    // Nothing authored to rotate, and (0, 0, 1) is what the
                    // vertex would otherwise carry. The recompute is strictly
                    // better than a constant.
                    a_vertices[v].Nx = nm.x;
                    a_vertices[v].Ny = nm.y;
                    a_vertices[v].Nz = nm.z;
                    ++a_report.rotated;
                    continue;
                }
                if (bm > 0.999999f) {
                    continue;  // the preset did not deform here
                }
                RE::NiPoint3 axis{ nb.y * nm.z - nb.z * nm.y,
                                   nb.z * nm.x - nb.x * nm.z,
                                   nb.x * nm.y - nb.y * nm.x };
                const float sinT = std::sqrt(axis.x * axis.x + axis.y * axis.y +
                                             axis.z * axis.z);
                if (sinT <= 1.0e-6f) {
                    // Parallel or antiparallel. A 180 degree flip has no
                    // defined axis and no smooth surface produces one, so the
                    // authored normal stands rather than being spun at random.
                    continue;
                }
                axis.x /= sinT;
                axis.y /= sinT;
                axis.z /= sinT;
                RE::NiPoint3 n{ a_vertices[v].Nx, a_vertices[v].Ny, a_vertices[v].Nz };
                // Rodrigues about the axis that carries nb onto nm, through the
                // angle between them: cos is their dot, sin is the cross's
                // length, both already in hand.
                const float cosT = bm;
                const float kv   = axis.x * n.x + axis.y * n.y + axis.z * n.z;
                const RE::NiPoint3 kxv{ axis.y * n.z - axis.z * n.y,
                                        axis.z * n.x - axis.x * n.z,
                                        axis.x * n.y - axis.y * n.x };
                RE::NiPoint3 rot{
                    n.x * cosT + kxv.x * sinT + axis.x * kv * (1.0f - cosT),
                    n.y * cosT + kxv.y * sinT + axis.y * kv * (1.0f - cosT),
                    n.z * cosT + kxv.z * sinT + axis.z * kv * (1.0f - cosT)
                };
                if (!NormaliseInPlace(rot)) {
                    continue;
                }
                a_vertices[v].Nx = rot.x;
                a_vertices[v].Ny = rot.y;
                a_vertices[v].Nz = rot.z;
                ++a_report.rotated;
            }

            a_report.vertices = static_cast<std::uint32_t>(a_vertices.size());
        }

        // The nine material assignments both extraction paths share, in one
        // place so they cannot drift.
        void AssignMaterial(const MaterialInfo& a_material, RenderMesh& a_out) {
            a_out.Diffuse          = a_material.Diffuse;
            a_out.UseAlpha         = a_material.UseAlpha;
            a_out.BlendAlpha       = a_material.BlendAlpha;
            a_out.MaterialAlpha    = a_material.MaterialAlpha;
            a_out.AlphaCutoff      = a_material.AlphaCutoff;
            a_out.SpecularStrength = a_material.SpecularStrength;
            a_out.SpecularPower    = a_material.SpecularPower;
            a_out.EmissiveColor    = a_material.EmissiveColor;
            a_out.EmissiveStrength = a_material.EmissiveStrength;
        }

        // ⚠⚠ A RESCUED FX MESH MUST LEAVE THE ALPHA PASS, and this is not a
        // tidy-up. ResolveMaterial's lighting cast fails on an effect shader
        // so its whole block is skipped, but the NiAlphaProperty block still
        // runs, and a spectral weapon's blend flag sets UseAlpha and
        // BlendAlpha. PreviewRenderer routes any BlendAlpha mesh into the
        // second pass with depth WRITES off, which is right for an overlay
        // over solid geometry and wrong here: when the rescued mesh is the
        // only thing in the scene, nothing occludes anything, the last
        // triangle drawn wins every pixel, and the card comes back with a
        // correct silhouette and scrambled shading. The item draws additively
        // in game through a shader path the thumbnail pass does not have, so
        // there is nothing to blend against and opaque is the honest answer.
        void ForceOpaque(RenderMesh& a_out) {
            a_out.UseAlpha    = false;
            a_out.BlendAlpha  = false;
            a_out.AlphaCutoff = 0.0f;
        }

        // ---- the skin-partition path (phase 2) ---------------------------
        //
        // Armour meshes are skinned and carry bind-pose buffers per
        // partition; each partition owns a TriShape whose triList indexes its
        // raw vertex buffer DIRECTLY (Wardrobe derives the vertex count from
        // max(triList)+1, which is the proof of that indexing). Read the CPU
        // copy (rawVertexData) where it exists; a partition without one is
        // counted and logged so the staging fallback is built only if the
        // field says it is needed (measure first, the standing rule).
        //
        // The spec's two skin-path clamps are applied: the vertex count is
        // capped by what the buffer really holds (the D3D descriptor when a
        // buffer exists, the shape-wide declared count otherwise), and the
        // triList walk is capped by the declared triangle count. Everything
        // here runs inside ExtractOneSEH's __try, so a partition whose
        // pointers lie faults into the counted skip, never a crash.
        bool BuildFromSkinPartition(ID3D11Device* a_device, const GeometryCandidate& a_cand,
                                    RE::NiSkinPartition& a_part,
                                    std::span<const DirectX::XMFLOAT4> a_dynamicPositions,
                                    RenderMesh& a_out, ExtractStats& a_stats,
                                    std::span<const std::array<float, 3>> a_morph) {
            // ⚠ THE POSE'S GATING MEASUREMENT (OS-204 phase 2), AND IT MOVES
            // NOTHING. Skyrim carries the blend weights and blend indices
            // together under VA_SKINNING, and how they are packed inside it is
            // the one fact the skinning cannot be written without. Twelve
            // bytes would be four half weights beside four byte indices; eight
            // would be something else again. It is reported per card so the
            // field log answers it across the whole catalog before a single
            // vertex is skinned, which is the rule this file learned the hard
            // way from VF_FULLPREC.
            if (a_part.numPartitions != 0) {
                const auto& p0 = a_part.partitions[0];
                a_stats.skinSpan =
                    AttributeSpan(p0.vertexDesc, RE::BSGraphics::Vertex::VA_SKINNING);
                a_stats.bonesPerVertex = p0.bonesPerVertex;
                a_stats.skinBones      = p0.numBones;
            }
            std::vector<PreviewVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::array<float, 3>       boundsMin{};
            std::array<float, 3>       boundsMax{};
            bool                       haveBounds = false;

            // ⚠⚠ A MESH WHOSE BUFFER CARRIES NO NORMALS NEEDS THE PASS EVEN
            // WITH NO MORPH, and gating it on the morph alone is what left the
            // head, hands and feet FLAT beside a correctly shaded body (field
            // 2026-08-11, HIMBO and 3BA). These layouts really do exist: this
            // load order's body and extremity partitions report
            // VERTEX|UV|SKINNED, stride 32, no VF_NORMAL, so every vertex
            // carried PreviewVertex's (0, 0, 1) default and the shader lit a
            // flat plate. UBE looked right the whole time because it ships
            // `_tangent` builds of the same parts, which DO carry normals: the
            // discriminator is the buffer, never the body mod.
            //
            // Read from the descriptors before the walk, so a mesh that has
            // its normals pays nothing at all: no parallel arrays, no
            // accumulator, no second pass.
            bool lacksNormals = false;
            for (std::uint32_t pi = 0; pi < a_part.numPartitions; ++pi) {
                const auto& p = a_part.partitions[pi];
                if (p.buffData && p.triList && p.triangles != 0 &&
                    !p.buffData->vertexDesc.HasFlag(
                        RE::BSGraphics::Vertex::Flags::VF_NORMAL)) {
                    lacksNormals = true;
                    break;
                }
            }
            // The positions BEFORE the morph, the key each output vertex
            // accumulates under, and whether it had an authored normal at all.
            // All three are pushed in lockstep with `vertices`.
            const bool                 collectNormals = !a_morph.empty() || lacksNormals;
            std::vector<RE::NiPoint3>  basePositions;
            std::vector<std::uint32_t> shapeIndexOf;
            std::vector<std::uint8_t>  hadNormal;

            for (std::uint32_t pi = 0; pi < a_part.numPartitions; ++pi) {
                auto& p    = a_part.partitions[pi];
                auto* buff = p.buffData;
                if (!buff || !p.triList || p.triangles == 0) {
                    continue;
                }
                if (!buff->rawVertexData) {
                    spdlog::debug("MeshExtractor: preview.skin-no-raw '{}' partition {}",
                                  a_cand.namePath, pi);
                    continue;
                }
                const std::uint32_t stride = buff->vertexDesc.GetSize();
                if (stride == 0) {
                    continue;
                }
                // ⚠ PRESENCE COMES FROM THE FLAG NIBBLE, NEVER FROM "offset
                // is zero". A dynamic shape's partition buffer carries no
                // positions, so its FIRST attribute (usually the UVs) sits
                // at offset 0 legitimately; the old "uvOffset != 0" sentinel
                // read that as "no UVs" and sampled texel (0,0) for the
                // whole mesh: a transparent atlas corner on hair and brows
                // (every fragment discarded, an invisible card), a dark one
                // on eyes (uniform black spheres). Field 2026-08-09, phase 3
                // round 1. The presence bits are safe where VF_FULLPREC was
                // not: the engine BUILDS this layout from them, so they
                // cannot disagree with it, while FULLPREC merely claims a
                // width the authored buffer may not honour. Width stays
                // measured.
                const bool hasPos =
                    buff->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX);
                const bool hasUV =
                    buff->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV);
                const bool hasNrm =
                    buff->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
                if (!hasPos && a_dynamicPositions.empty()) {
                    // No position source at all: the buffer has none and no
                    // morph buffer arrived. Skip rather than fabricate
                    // geometry from whatever sits at offset 0.
                    spdlog::debug("MeshExtractor: preview.skin-no-pos '{}' partition {}",
                                  a_cand.namePath, pi);
                    continue;
                }
                const std::uint32_t posOffset = buff->vertexDesc.GetAttributeOffset(
                    RE::BSGraphics::Vertex::VA_POSITION);
                const std::uint32_t uvOffset = buff->vertexDesc.GetAttributeOffset(
                    RE::BSGraphics::Vertex::VA_TEXCOORD0);
                const std::uint32_t nrmOffset = buff->vertexDesc.GetAttributeOffset(
                    RE::BSGraphics::Vertex::VA_NORMAL);
                const bool fullPrec =
                    hasPos && PositionIsFullFloat(buff->vertexDesc, posOffset);

                // FIX (OOB 2, skin path): the triList is uint16 and NIF
                // content; grow the vertex count from it, then cap it by what
                // the buffer really holds.
                const auto    safeTriangles = static_cast<std::uint32_t>(p.triangles);
                std::uint32_t maxIndex      = 0;
                for (std::uint32_t t = 0; t < safeTriangles * 3u; ++t) {
                    maxIndex =
                        (std::max)(maxIndex, static_cast<std::uint32_t>(p.triList[t]));
                }
                std::uint32_t vertCount = maxIndex + 1;
                if (auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer)) {
                    D3D11_BUFFER_DESC desc{};
                    vb->GetDesc(&desc);
                    vertCount = (std::min)(vertCount, desc.ByteWidth / stride);
                } else {
                    vertCount = (std::min)(vertCount, a_part.vertexCount);
                }
                if (!hasPos) {
                    // The morph buffer is the only position source, so it
                    // also caps the count.
                    vertCount = (std::min)(
                        vertCount,
                        static_cast<std::uint32_t>(a_dynamicPositions.size()));
                }
                if (vertCount == 0) {
                    continue;
                }

                const std::uint8_t* raw  = buff->rawVertexData;
                const auto          base = static_cast<std::uint32_t>(vertices.size());
                // ⚠⚠ WHETHER THIS BUFFER IS SHAPE-INDEXED IS MEASURED, NOT
                // ASSUMED, and getting it from the DECLARED counts is what
                // scrambled the first build. This engine hands each partition
                // a buffer holding the WHOLE shape, with a triList in
                // shape-global indices: on this body p0 walks 29298 vertices
                // while declaring 23683, and p1 walks 29293 while declaring
                // 5704 (field 2026-08-10). So i is already a shape index and
                // p.vertexMap, which maps partition-local to shape, displaces
                // every vertex it touches by some other vertex's delta. The
                // old code mapped the first `declared` vertices and left the
                // rest alone, which is exactly the clean torso with shredded
                // hands and lower legs that came back.
                //
                // A genuinely partition-local buffer cannot address past its
                // own vertex count, so vertCount > p.vertices is the tell and
                // the map stays available for a NIF that really needs it.
                const bool shapeIndexed = vertCount > p.vertices;
                for (std::uint32_t i = 0; i < vertCount; ++i) {
                    const auto*  v = raw + static_cast<std::size_t>(i) * stride;
                    RE::NiPoint3 rawPos;
                    if (i < a_dynamicPositions.size()) {
                        // A dynamic tri shape keeps its positions in the
                        // morph buffer, full floats, and its partition
                        // buffer holds everything else. Robes and other
                        // morph-bearing garments are this case; reading
                        // "positions" from their partition buffer reads
                        // whatever sits at that offset instead.
                        const auto& d = a_dynamicPositions[i];
                        rawPos = { d.x, d.y, d.z };
                    } else if (fullPrec) {
                        const float* pf = reinterpret_cast<const float*>(v + posOffset);
                        rawPos = { pf[0], pf[1], pf[2] };
                    } else {
                        const auto* ph =
                            reinterpret_cast<const std::uint16_t*>(v + posOffset);
                        rawPos = { DirectX::PackedVector::XMConvertHalfToFloat(ph[0]),
                                   DirectX::PackedVector::XMConvertHalfToFloat(ph[1]),
                                   DirectX::PackedVector::XMConvertHalfToFloat(ph[2]) };
                    }
                    // ⚠⚠ THROUGH THE PARTITION'S vertexMap, NEVER BY ARRAY
                    // POSITION. A .osd delta is indexed by the SHAPE's vertex
                    // index, and this loop is walking one PARTITION's buffer:
                    // on this rig's body the shape holds 29298 vertices while
                    // its two partitions hold 23683 and 5704, so 89 vertices
                    // appear in BOTH and no partition-local index equals a
                    // shape index past the first partition. Applying by
                    // position would scramble the mesh and log nothing at all.
                    // Going through the map also gives the duplicated vertices
                    // the same displacement in both copies, which is what
                    // keeps the seam closed.
                    //
                    // ⚠ IN THE MESH'S OWN SPACE, so it lands before the node
                    // transform rather than after it.
                    // ⚠ CAPTURED BEFORE THE MORPH AND HOISTED OUT OF IT: the
                    // normal pass needs the surface the authored normals were
                    // measured against, and the shape index is what keys the
                    // accumulator that both partitions fold into.
                    const RE::NiPoint3  preMorph = rawPos;
                    const bool          mapped =
                        !shapeIndexed && p.vertexMap && i < p.vertices;
                    const std::uint32_t shapeIndex = mapped ? p.vertexMap[i] : i;
                    if (!a_morph.empty() && shapeIndex < a_morph.size()) {
                        const auto& d = a_morph[shapeIndex];
                        rawPos.x += d[0];
                        rawPos.y += d[1];
                        rawPos.z += d[2];
                    }
                    // ⚠⚠ THE ACCUMULATOR KEY IS NOT ALWAYS THE SHAPE INDEX, and
                    // taking it for granted would merge unrelated vertices on
                    // content the body never exercised. Two partitions must
                    // share a key ONLY when they are really the same vertex,
                    // which is true when the buffer is shape-indexed (the key
                    // IS the shape index) or when the map resolved one. With
                    // neither, `i` is partition-local and would collide across
                    // partitions, so the output position keys instead: that
                    // costs a lighting seam at the partition boundary and never
                    // shades a vertex by a stranger's neighbourhood.
                    const std::uint32_t normalKey =
                        (shapeIndexed || mapped) ? shapeIndex : base + i;
                    const RE::NiPoint3 pos = a_cand.transform * rawPos;
                    PreviewVertex out{};
                    out.Px = pos.x;
                    out.Py = pos.y;
                    out.Pz = pos.z;
                    IncludeBoundsPoint(boundsMin, boundsMax, haveBounds, pos);
                    if (hasUV) {
                        const auto* uv =
                            reinterpret_cast<const std::uint16_t*>(v + uvOffset);
                        out.U = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
                        out.V = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
                    }
                    if (hasNrm) {
                        const std::uint8_t* n  = v + nrmOffset;
                        const RE::NiPoint3  tn = TransformNormal(
                            a_cand.transform, (n[0] / 255.0f) * 2.0f - 1.0f,
                            (n[1] / 255.0f) * 2.0f - 1.0f, (n[2] / 255.0f) * 2.0f - 1.0f);
                        out.Nx = tn.x;
                        out.Ny = tn.y;
                        out.Nz = tn.z;
                    }
                    // ⚠ IN LOCKSTEP WITH vertices, AND THE PASS REFUSES TO RUN
                    // IF THEY EVER DIVERGE. All four are indexed by the same
                    // output position, so a push that happened on one path and
                    // not another would shade each vertex by its neighbour's
                    // surface: wrong everywhere, and wrong quietly.
                    if (collectNormals) {
                        basePositions.push_back(a_cand.transform * preMorph);
                        shapeIndexOf.push_back(normalKey);
                        hadNormal.push_back(hasNrm ? std::uint8_t{ 1 }
                                                   : std::uint8_t{ 0 });
                    }
                    vertices.push_back(out);
                }
                for (std::uint32_t t = 0; t < safeTriangles * 3u; ++t) {
                    const std::uint32_t idx = p.triList[t];
                    indices.push_back(base + (idx < vertCount ? idx : 0u));
                }
            }

            if (collectNormals) {
                // ⚠ THE `true` IS THE CORRECTION. False runs the same pass
                // without touching a pixel, which is the cheap A/B if the
                // rotation is ever suspected again.
                //
                // With no morph the two recomputes are built from the SAME
                // positions, so the rotation is exactly the identity and every
                // vertex that has a normal keeps it untouched. Only the ones
                // the buffer never carried get written, which is the whole
                // point on the head, hands and feet.
                MorphNormalReport report{};
                MeasureAndCorrectMorphedNormals(vertices, indices, basePositions,
                                                shapeIndexOf, hadNormal, true, report);
                spdlog::debug("MeshExtractor: skin-normals '{}' verts={} noNrm={} "
                              "degen={} written={} morph={}",
                              a_cand.namePath, report.vertices, report.noNormal,
                              report.degenerate, report.rotated,
                              a_morph.empty() ? 0 : 1);
            }

            if (!BuildBuffers(a_device, vertices, indices, a_out)) {
                return false;
            }
            if (haveBounds) {
                a_out.BoundsMin  = boundsMin;
                a_out.BoundsMax  = boundsMax;
                a_out.HaveBounds = true;
            }
            return true;
        }

        // Where a skinned shape actually stands, decided once for every path
        // that draws one.
        //
        // ⚠ ONE PLACER, NOT ONE PER SOURCE PATH. The skin-partition path and
        // the legacy path both draw a skinned mesh from its own vertices, so
        // both owe it the bind pose; two copies of this decision would drift
        // apart at the first change to either.
        [[nodiscard]] GeometryCandidate PlaceAtBindPose(const GeometryCandidate& a_cand,
                                                        RE::NiSkinInstance* a_skin,
                                                        bool          a_headCorrected,
                                                        ExtractStats& a_stats) {
            GeometryCandidate placed = a_cand;
            std::vector<SkinBindPose::Transform> products;
            CollectBindProducts(a_skin, products);
            const auto decision = SkinBindPose::Decide(products);
            if (a_headCorrected) {
                // ⚠ THE STAND-DOWN IS LOGGED, because "the head path
                // already moved this" and "the bind pose had nothing to
                // say" are different facts about a card that is still
                // wrong, and they read identically as silence.
                if (decision.verdict == SkinBindPose::Verdict::kApply) {
                    ++a_stats.bindDeferred;
                    spdlog::debug(
                        "MeshExtractor: preview.bind-pose '{}' stood down, "
                        "the head correction already moved this root; it "
                        "would have moved by ({:.3f}, {:.3f}, {:.3f})",
                        a_cand.namePath, decision.correction.translate[0],
                        decision.correction.translate[1],
                        decision.correction.translate[2]);
                }
            } else if (decision.verdict == SkinBindPose::Verdict::kApply) {
                // ⚠⚠ IT REPLACES THE NODE TRANSFORM, IT DOES NOT COMPOSE
                // WITH IT, and composing was wrong in a way that would
                // have broken every card in the app. The engine ignores a
                // skinned shape's node transform outright: what it draws
                // is boneWorld * skinToBone * v and nothing else. Some
                // files supply the placement through the node chain as
                // well, and femalehead.nif is one, sitting under an
                // 'NPC Head [Head]' node at z 120.344 with a bind product
                // of z 120.344 to match. Composing the two puts the
                // mannequin's head at 240 on every card that draws a
                // figure. Replacing agrees with the file wherever both
                // halves already agree, which is what makes it safe.
                //
                // ⚠ ONLY ON kApply. An identity product with a
                // non-identity node transform is a case nothing here has
                // measured, so it keeps the placement it has always had
                // rather than being moved to the origin on a theory.
                placed.transform = FromPure(decision.correction);
                ++a_stats.bindCorrected;
                spdlog::info(
                    "MeshExtractor: preview.bind-pose '{}' placed at "
                    "({:.3f}, {:.3f}, {:.3f}) from ({:.3f}, {:.3f}, "
                    "{:.3f}) on {} bone(s), spread {:.4f}",
                    a_cand.namePath, decision.correction.translate[0],
                    decision.correction.translate[1],
                    decision.correction.translate[2],
                    a_cand.transform.translate.x,
                    a_cand.transform.translate.y,
                    a_cand.transform.translate.z, decision.bones,
                    decision.spread.translation);
            } else if (decision.verdict == SkinBindPose::Verdict::kPosed) {
                // ⚠⚠ THE DECLINE IS LOGGED IN THE SAME COMMIT AS THE
                // RULE, because a refused branch and an absent branch
                // read identically in a field log and a round was lost
                // to exactly that. This one is not rare and it is not a
                // fault: this rig's own HIMBO mannequin body declines
                // with a spread of 3.54 and is drawn correctly anyway,
                // since what the engine draws is the weighted sum rather
                // than any single bone's product.
                ++a_stats.bindPosed;
                spdlog::debug(
                    "MeshExtractor: preview.bind-pose '{}' left alone, "
                    "{} bone(s) disagree by {:.4f} units / {:.4f} rot",
                    a_cand.namePath, decision.bones,
                    decision.spread.translation, decision.spread.rotation);
            }
            return placed;
        }

        // ---- the fourth source path: a legacy shape's own arrays ---------

        // A `NiTriShape`'s geometry, straight off `NiGeometryData`.
        //
        // This is the simplest of the four and the only one that reads no
        // packed buffer at all: `NiGeometryData` keeps plain `NiPoint3` and
        // `NiPoint2` arrays, so there is no vertex descriptor, no attribute
        // offset, no half floats and no byte-encoded normals to unpack.
        //
        // ⚠⚠ THE ARRAYS ARE ALIVE AND `keepFlags` DOES NOT SAY SO. Field
        // 2026-08-20, twelve distinct shapes across Night Lurker, Ahzidal's
        // Armored Robes and Miraak's robes: every one reported
        // `keep=0x00 pos=true nrm=true uv=true` with a real vertex count. The
        // flag is what the FILE asked to keep, not what the runtime holds, so
        // gating on it would refuse every shape this path exists for. The
        // pointers are the measurement.
        //
        // ⚠⚠ THE SKIN PARTITION IS NOT READ HERE AND MUST NOT BE. These files
        // are BSVersion 83, so their `NiSkinPartition` is the old on-disk kind
        // with no vertex buffer and no `BSGraphics::VertexDesc`, and
        // CommonLib's class describes the SSE runtime one. The same field round
        // shows what reading it gets: plausible `parts=1` beside a
        // `partVerts` of 3187306752. The bone matrices on `NiSkinData` are a
        // different structure and those ARE read, by CollectBindProducts, which
        // the bind pose has been using on this rig since OS-236.
        bool BuildFromLegacyGeometry(ID3D11Device* a_device, const GeometryCandidate& a_cand,
                                     RE::NiGeometryData& a_data, RenderMesh& a_out,
                                     std::span<const std::array<float, 3>> a_morph) {
            const auto  vertexCount = static_cast<std::uint32_t>(a_data.vertices);
            const auto* positions   = a_data.vertex;
            if (vertexCount == 0 || !positions) {
                return false;
            }
            const auto* normals = a_data.normal;
            const auto* uvs     = a_data.texture;

            // ⚠ THE INDEX OFFSETS BELONG TO NiTriShapeData ALONE, so the class
            // is proven before they are read. A `NiTriStripsData` keeps strips
            // at the same offsets and would be read as a triangle list of
            // whatever the strip lengths happen to spell.
            if (!RttiChainNames(&a_data, "NiTriShapeData")) {
                return false;
            }
            const std::uint32_t  points    = LegacyGeometry::NumTrianglePoints(&a_data);
            const std::uint16_t* triangles = LegacyGeometry::Triangles(&a_data);
            if (points < 3 || !triangles) {
                return false;
            }
            // The engine allocates this buffer as exactly `points * 2` bytes
            // (NiTriShapeData::LoadBinary), so the count bounds the read. The
            // cap is for a corrupt file rather than a real one: without it a
            // bad count walks a stranger's memory on the render thread.
            constexpr std::uint32_t kMaxPoints = 3u * 4'000'000u;
            const std::uint32_t     safePoints = (std::min)(points, kMaxPoints);

            std::vector<PreviewVertex> vertices(vertexCount);
            std::array<float, 3>       boundsMin{};
            std::array<float, 3>       boundsMax{};
            bool                       haveBounds = false;

            // Same rule the skin path applies: recompute normals only where the
            // mesh has none of its own, or where a morph has moved it and the
            // authored ones no longer describe the surface.
            const bool                 collectNormals = !normals || !a_morph.empty();
            std::vector<RE::NiPoint3>  basePositions;
            std::vector<std::uint32_t> shapeIndexOf;
            std::vector<std::uint8_t>  hadNormal;
            if (collectNormals) {
                basePositions.reserve(vertexCount);
                shapeIndexOf.reserve(vertexCount);
                hadNormal.reserve(vertexCount);
            }

            for (std::uint32_t i = 0; i < vertexCount; ++i) {
                RE::NiPoint3 raw = positions[i];
                const RE::NiPoint3 preMorph = raw;
                if (!a_morph.empty() && i < a_morph.size()) {
                    const auto& d = a_morph[i];
                    raw.x += d[0];
                    raw.y += d[1];
                    raw.z += d[2];
                }
                const RE::NiPoint3 pos = a_cand.transform * raw;
                PreviewVertex&     out = vertices[i];
                out.Px = pos.x;
                out.Py = pos.y;
                out.Pz = pos.z;
                IncludeBoundsPoint(boundsMin, boundsMax, haveBounds, pos);
                if (uvs) {
                    out.U = uvs[i].x;
                    out.V = uvs[i].y;
                }
                if (normals) {
                    const RE::NiPoint3 tn = TransformNormal(a_cand.transform, normals[i].x,
                                                            normals[i].y, normals[i].z);
                    out.Nx = tn.x;
                    out.Ny = tn.y;
                    out.Nz = tn.z;
                }
                if (collectNormals) {
                    basePositions.push_back(a_cand.transform * preMorph);
                    shapeIndexOf.push_back(i);
                    hadNormal.push_back(normals ? std::uint8_t{ 1 } : std::uint8_t{ 0 });
                }
            }

            std::vector<std::uint32_t> indices;
            indices.reserve(safePoints);
            for (std::uint32_t t = 0; t < safePoints; ++t) {
                const std::uint32_t idx = triangles[t];
                indices.push_back(idx < vertexCount ? idx : 0u);
            }

            if (collectNormals) {
                MorphNormalReport report{};
                MeasureAndCorrectMorphedNormals(vertices, indices, basePositions,
                                                shapeIndexOf, hadNormal, true, report);
                spdlog::debug("MeshExtractor: legacy-normals '{}' verts={} noNrm={} "
                              "degen={} written={} morph={}",
                              a_cand.namePath, report.vertices, report.noNormal,
                              report.degenerate, report.rotated,
                              a_morph.empty() ? 0 : 1);
            }

            if (!BuildBuffers(a_device, vertices, indices, a_out)) {
                return false;
            }
            if (haveBounds) {
                a_out.BoundsMin  = boundsMin;
                a_out.BoundsMax  = boundsMax;
                a_out.HaveBounds = true;
            }
            return true;
        }

        // ---- one geometry, end to end ------------------------------------

        bool ExtractOne(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                        const GeometryCandidate& a_cand, RenderMesh& a_out,
                        ExtractStats& a_stats, bool a_headPartScene,
                        bool a_greyBody, bool a_keepEffect, bool a_keepBlend,
                        bool a_filtersExempt, bool a_headCorrected,
                        std::span<const std::array<float, 3>> a_morph) {
            auto* geometry = a_cand.geometry;
            // ⚠ THE TWO PROPERTIES ARE FETCHED ONCE, HERE, FOR WHICHEVER KIND
            // OF SHAPE THIS IS. Every filter below and the material resolver
            // read them, and a legacy `NiGeometry` keeps them at its own
            // measured offsets rather than in a `GEOMETRY_RUNTIME_DATA`. Asking
            // each reader to branch again would be five places that can drift.
            RE::NiProperty* shaderProp = nullptr;
            RE::NiProperty* alphaProp  = nullptr;
            if (geometry) {
                auto& geomRt = geometry->GetGeometryRuntimeData();
                shaderProp   = geomRt.properties[RE::BSGeometry::States::kEffect].get();
                alphaProp    = geomRt.properties[RE::BSGeometry::States::kProperty].get();
            } else if (a_cand.legacy) {
                shaderProp = LegacyGeometry::ShaderProperty(a_cand.legacy);
                alphaProp  = LegacyGeometry::AlphaProperty(a_cand.legacy);
            } else {
                return false;
            }

            // ⚠ EFFECT-SHADER GEOMETRY IS FX, NOT THE ITEM. Dawnbreaker's sun
            // disc is a glow plane under BSEffectShaderProperty; the lighting
            // cast in ResolveMaterial fails on it, so it rendered as the
            // shader's flat untextured fallback, an opaque cream ellipse over
            // the blade (field 2026-08-09). Skipping it here also keeps its
            // quad out of the AABB the camera frames. In game these meshes
            // draw additively through their own shader path, which the
            // thumbnail pass does not have.
            //
            // ⚠ UNLESS THE FX IS ALL THERE IS. Some items are authored
            // entirely through the effect shader, and the skip that is right
            // for a glow plane eats the whole item: vanilla's spectral draugr
            // weapons are one BSTriShape with a BSEffectShaderProperty and no
            // BSLightingShaderProperty anywhere in the file, so the card came
            // back failed(geometry) with a perfectly correct key. The caller
            // retries the scene with this true only after the first pass has
            // ALREADY decided to fail, so nothing that renders today can reach
            // it. See ShouldRetryKeepingEffects in MeshExtractor.h.
            const bool isEffect =
                netimmerse_cast<RE::BSEffectShaderProperty*>(shaderProp) != nullptr;
            if (isEffect && !a_keepEffect) {
                ++a_stats.effectSkipped;
                return false;
            }

            // ⚠⚠ AND THE TWO KINDS OF FX NO SHADER CLASS CAN SEE. Measured with
            // tools/nif_shape_alpha.py over four arrow families the field
            // reported in one night (bound weapons, Darkend's torment arrow,
            // Ordinator's trick arrows, the Creation Club magic arrows):
            //
            //  * an overlay blend, meaning an unreproducible blend with NO
            //    alpha test to shape it. `FlamesMesh01` and `ArrowQuiver:2`
            //    are both 0x100D, blend on and test off.
            //  * a plane with NO alpha property at all, which draws fully
            //    opaque and offers nothing to judge. `FlamesMesh02`,
            //    `BlurMeshA`. Those are caught by NAME, which is what they
            //    have, and the names are FX authoring conventions rather than
            //    coincidences.
            //
            // ⚠ BOTH ARE GATED ON a_keepEffect AND COUNTED WHERE THE RESCUE
            // CAN SEE THEM, so an item authored entirely as FX still draws.
            if (!a_keepEffect) {
                // ⚠ THE BLEND SKIP YIELDS FIRST. It is the weakest of the three
                // signals, so the rescue's first rung puts exactly these back.
                if (auto* const blend = netimmerse_cast<RE::NiAlphaProperty*>(alphaProp);
                    !a_keepBlend && blend && PreviewFilter::IsOverlayBlend(
                                 blend->GetAlphaBlending(), blend->GetAlphaTesting(),
                                 static_cast<int>(blend->GetSrcBlendMode()),
                                 static_cast<int>(blend->GetDestBlendMode()))) {
                    ++a_stats.blendSkipped;
                    spdlog::debug("MeshExtractor: preview.fx-blend '{}' ({}->{}, no test)",
                                  a_cand.namePath,
                                  static_cast<int>(blend->GetSrcBlendMode()),
                                  static_cast<int>(blend->GetDestBlendMode()));
                    return false;
                }
                if (PreviewFilter::IsFxName(PreviewGrid::FoldPath(a_cand.namePath))) {
                    ++a_stats.fxNameSkipped;
                    spdlog::debug("MeshExtractor: preview.fx-name '{}'", a_cand.namePath);
                    return false;
                }
            }

            // ⚠⚠ THE BLEND RULE IS HEAD PARTS ONLY, AND IT WENT SCENE-WIDE
            // FOR ONE BUILD AND WAS PUT BACK BY THE FIELD. The theory was that
            // a glow plane is FX whatever shader class it wears, which is true,
            // and that an unreproducible blend identifies one, which is NOT.
            // Measured on the Bound Arrow card that prompted it (2026-08-21):
            // ENB Light re-authors the ARROW ITSELF as blend on, srcAlpha to
            // ONE, alpha test GREATER 128, so the scene-wide rule ate Arrow1
            // through Arrow6 and Arrow:0, seven shapes, and left the card
            // showing the quiver and the very glow plane it was written to
            // remove. `fxblend=7 meshes=2` in the processed line, and the
            // per-shape debug named all seven.
            //
            // The plane it was aimed at, `FlamesMesh02`, carries NO
            // NiAlphaProperty at all, so no blend rule could ever have reached
            // it. That is still open (OS-244) and wants a measurement rather
            // than a third guess.
            //
            // Head parts keep the rule because that is where it was measured
            // and has been field-clean since 2026-08-09.
            // Head-part scenes reject one more kind of scaffolding of their
            // own, field-measured 2026-08-09 (phase 3 round 2): geometry with
            // NO lighting shader property at all. The UBE eye scenes ship
            // "lens dummy" meshes with no material; the flat fallback colour
            // drew them as opaque shells OVER the iris, which is most of why
            // an eye card was a black ball. Wardrobe applies the same rule to
            // hair (spec, line 1047).
            //
            // ⚠ THE BLEND RULE THAT USED TO LIVE HERE IS NOW EVERY SCENE'S,
            // above. It was written for the UBE eye "outer" layer, which
            // blends additively, and fenced to head parts on the claim that a
            // gear scene's glow planes all fall to the effect-shader skip.
            // Bound Arrow's did not (2026-08-21), and the eye layer and the
            // arrow's flame plane were always the same fact.
            if (a_headPartScene) {
                auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProp);
                if (!lighting) {
                    ++a_stats.effectSkipped;
                    spdlog::debug("MeshExtractor: preview.headpart-scaffold '{}' (no material)",
                                  a_cand.namePath);
                    return false;
                }
                // The UBE eye "outer" layer blends additively; the standard
                // blend state renders that as a dark shell over the iris.
                if (auto* const blend = netimmerse_cast<RE::NiAlphaProperty*>(alphaProp);
                    blend && PreviewFilter::IsUnreproducibleBlend(
                                 blend->GetAlphaBlending(),
                                 static_cast<int>(blend->GetSrcBlendMode()),
                                 static_cast<int>(blend->GetDestBlendMode()))) {
                    ++a_stats.blendSkipped;
                    spdlog::debug(
                        "MeshExtractor: preview.headpart-scaffold '{}' (blend {}->{})",
                        a_cand.namePath,
                        static_cast<int>(blend->GetSrcBlendMode()),
                        static_cast<int>(blend->GetDestBlendMode()));
                    return false;
                }
            }

            // Face-tinted geometry is FaceGen's, never a garment's: reject on
            // the shader flags, which the spec calls more reliable than any
            // name rule. UNLESS this scene is deliberately a face part: a
            // brow IS kFaceGenRGBTint geometry (NpcHair.cpp banked that), so
            // this filter would return the brow dimension a grid of blank
            // cards.
            // ⚠⚠ AND UNLESS THIS ROOT IS THE MANNEQUIN, which the field found
            // the hard way (2026-08-10): a UBE body and head are BOTH
            // kFaceGenRGBTint, so a rule written to drop face scaffolding out
            // of a garment silently deleted the whole figure. Every armour
            // card came back as the item alone with faceFiltered=2, the
            // feature composed perfectly and drew nothing.
            if (!a_headPartScene && !a_filtersExempt) {
                using SFlag = RE::BSShaderProperty::EShaderPropertyFlag;
                auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProp);
                if (lighting && (lighting->flags.any(SFlag::kFace) ||
                                 lighting->flags.any(SFlag::kFaceGenRGBTint))) {
                    ++a_stats.faceFiltered;
                    return false;
                }
            }

            // ⚠⚠ THE LEGACY SHAPE LEAVES HERE, BEFORE ANY RUNTIME DATA IS
            // TOUCHED. It has no `GEOMETRY_RUNTIME_DATA`, no renderer data and
            // no usable skin partition, so everything below this point would be
            // read off the wrong offsets. What it does have is the CPU arrays
            // the file was loaded with, which is all this pass ever wanted.
            if (!geometry) {
                auto* data = LegacyGeometry::ModelData(a_cand.legacy);
                if (!data) {
                    return false;
                }
                // Skinned like any other garment, so it is placed the same way.
                // ⚠ NOT SPECIAL-CASED OUT OF THE BIND POSE. A legacy file is
                // just as likely to be authored off the body as a modern one,
                // and the engine draws both by `boneWorld * skinToBone`.
                const GeometryCandidate placed = PlaceAtBindPose(
                    a_cand, LegacyGeometry::SkinInstance(a_cand.legacy), a_headCorrected,
                    a_stats);
                if (!BuildFromLegacyGeometry(a_device, placed, *data, a_out, a_morph)) {
                    // ⚠⚠ THE DECLINE IS THE ONLY THING THIS PROBE STILL SAYS,
                    // and that is the point. While it reported every legacy
                    // shape it wrote two lines per piece per card build, on a
                    // load order carrying 1676 of them, to describe what is now
                    // the ordinary case. A shape this path cannot draw is a
                    // blank card, which is the fault OS-237 closed returning in
                    // a form nothing else names: a `NiTriStripsData` rather
                    // than a triangle list, or arrays the runtime really freed.
                    ++a_stats.legacyDeclined;
                    if (a_stats.legacyDeclined <= 4) {
                        const auto* rtti = a_cand.legacy->GetRTTI();
                        ReportLegacyGeometry(a_cand.legacy, a_cand.namePath,
                                             rtti ? rtti->GetName() : nullptr);
                    }
                    return false;
                }
                ++a_stats.legacyPath;
                AssignMaterial(ResolveMaterialFrom(shaderProp, alphaProp, a_stats, a_greyBody),
                               a_out);
                if (isEffect) {
                    ForceOpaque(a_out);
                    ++a_stats.effectKept;
                }
                return true;
            }

            auto&          geomRt = geometry->GetGeometryRuntimeData();
            SourceGeometry source;
            source.RendererGeometry = geomRt.rendererData;
            source.Transform        = a_cand.transform;

            if (auto* triShape = geometry->AsTriShape()) {
                auto& triRt = triShape->GetTrishapeRuntimeData();
                source.VertexStride  = geomRt.vertexDesc.GetSize();
                source.VertexCount   = triRt.vertexCount;
                source.TriangleCount = triRt.triangleCount;
                source.PositionOffset =
                    geomRt.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
                source.UVOffset =
                    geomRt.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
                source.NormalOffset =
                    geomRt.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
                source.FullPrecision =
                    PositionIsFullFloat(geomRt.vertexDesc, source.PositionOffset);
                source.DynamicPositions = GetDynamicPositions(triShape, source.VertexCount);
            }

            // No renderer data means a skinned mesh in every ordinary case:
            // the skin-partition path (phase 2). skinPath keeps counting the
            // geometries that take it, now as a rate of real extractions.
            if (!source.RendererGeometry) {
                auto* skin = geomRt.skinInstance.get();
                auto* part = skin ? skin->skinPartition.get() : nullptr;
                if (!part || part->numPartitions == 0) {
                    return false;
                }
                ++a_stats.skinPath;
                // Where this shape actually stands. The vertex buffer alone
                // cannot say: the engine never draws a skinned mesh from it
                // without the bind pose, and an author who built off the body
                // leaves the difference here and nowhere else.
                const GeometryCandidate placed =
                    PlaceAtBindPose(a_cand, skin, a_headCorrected, a_stats);
                // The dynamic span rides in from the SHAPE: a dynamic tri
                // shape's positions live there, not in the partition buffer.
                const auto dynamicPositions =
                    GetDynamicPositions(geometry->AsTriShape(), part->vertexCount);
                if (!BuildFromSkinPartition(a_device, placed, *part, dynamicPositions,
                                            a_out, a_stats, a_morph)) {
                    return false;
                }
                AssignMaterial(ResolveMaterial(geometry, a_stats, a_greyBody), a_out);
                if (isEffect) {
                    ForceOpaque(a_out);
                    ++a_stats.effectKept;
                }
                return true;
            }
            ++a_stats.rendererPath;

            // Fallbacks Wardrobe also applies: a NIF that declares no counts
            // gets them from the buffers themselves (already clamped there).
            auto* srcVb =
                reinterpret_cast<ID3D11Buffer*>(source.RendererGeometry->vertexBuffer);
            auto* srcIb =
                reinterpret_cast<ID3D11Buffer*>(source.RendererGeometry->indexBuffer);
            if (source.VertexStride > 0 && source.VertexCount == 0 && srcVb) {
                D3D11_BUFFER_DESC desc{};
                srcVb->GetDesc(&desc);
                source.VertexCount = desc.ByteWidth / source.VertexStride;
            }
            if (source.TriangleCount == 0 && srcIb) {
                D3D11_BUFFER_DESC desc{};
                srcIb->GetDesc(&desc);
                source.TriangleCount =
                    (desc.ByteWidth / sizeof(std::uint16_t)) / 3;
            }
            // ⚠ PRESENCE COMES FROM THE FLAG NIBBLE, NEVER FROM "offset is
            // zero". A dynamic shape's buffer carries no positions, so its
            // FIRST attribute (usually the UVs) sits at offset 0
            // legitimately; the old sentinel read that as "no UVs" and the
            // whole mesh sampled texel (0,0), which is a transparent atlas
            // corner on hair and brows (every fragment discarded, an
            // invisible card) and a dark one on eyes (uniform black
            // spheres). The presence bits are safe where VF_FULLPREC was
            // not: the engine BUILDS the layout from them, so they cannot
            // disagree with it, while FULLPREC merely claims a width the
            // authored buffer may not honour. Width stays measured.
            source.HasPosition =
                geomRt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX);
            source.HasUV = geomRt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV);
            source.HasNormal =
                geomRt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);

            if (!BuildFromRendererData(a_device, a_ctx, source, a_out)) {
                return false;
            }
            AssignMaterial(ResolveMaterial(geometry, a_stats, a_greyBody), a_out);
            if (isEffect) {
                ForceOpaque(a_out);
                ++a_stats.effectKept;
            }
            return true;
        }

        // Everything after the load is a bare dereference of memory whose
        // shape a third party chose, so the WHOLE per-geometry body sits
        // under SEH, not just the parse. No C++ objects in this frame (MSVC
        // forbids __try beside unwinding); the callee owns them all.
        __declspec(noinline) bool ExtractOneSEH(ID3D11Device* a_device,
                                                ID3D11DeviceContext* a_ctx,
                                                const GeometryCandidate* a_cand,
                                                RenderMesh* a_out, ExtractStats* a_stats,
                                                std::uint32_t* a_code,
                                                bool a_headPartScene,
                                                bool a_greyBody,
                                                bool a_keepEffect,
                                                bool a_keepBlend,
                                                bool a_filtersExempt,
                                                bool a_headCorrected,
                                                const std::array<float, 3>* a_morph,
                                                std::size_t a_morphCount) {
            *a_code = 0;
            __try {
                return ExtractOne(a_device, a_ctx, *a_cand, *a_out, *a_stats,
                                  a_headPartScene, a_greyBody, a_keepEffect, a_keepBlend,
                                  a_filtersExempt, a_headCorrected,
                                  std::span<const std::array<float, 3>>(a_morph,
                                                                       a_morphCount));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *a_code = static_cast<std::uint32_t>(GetExceptionCode());
                return false;
            }
        }

    }  // namespace

    bool Extract(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                 RE::NiAVObject* a_root, std::vector<RenderMesh>& a_out,
                 ExtractStats& a_stats, std::uint32_t a_slotMask,
                 bool a_headPartScene,
                 const std::vector<PreviewGrid::TextureSwapEntry>* a_swaps,
                 std::string_view a_modelPath,
                 const PreviewScopes::ScopeSet* a_scopes, bool a_mannequin,
                 bool a_mannequinHead, bool a_mannequinFeet,
                 const NodeAnchor* a_alignTo, NodeAnchor* a_anchorOut,
                 bool a_keepEffect, bool a_bodySubject,
                 std::span<const ShapeMorph> a_morphs, bool a_texturedBody,
                 bool a_keepBlend) {
        // Folded once for the whole root rather than per geometry, because the
        // inner loop asks the same question of the same list every time.
        std::vector<std::string>      foldedShapes;
        std::vector<std::string_view> shapeViews;
        foldedShapes.reserve(a_morphs.size());
        shapeViews.reserve(a_morphs.size());
        for (const auto& m : a_morphs) {
            foldedShapes.push_back(PreviewGrid::FoldPath(m.shape));
        }
        for (const auto& s : foldedShapes) {
            shapeViews.emplace_back(s);
        }
        // ⚠ THE MANNEQUIN'S EXEMPTION, ASKED FOR BY A SECOND CALLER. The
        // garment filters strip a body OUT of a garment scene; on a Bodies
        // card the body IS the scene, so pointing them at it deletes the
        // subject exactly the way they deleted the mannequin at tag m1.
        const bool filtersExempt = a_mannequin || a_bodySubject;
        // Grey by not asking for a diffuse (ResolveMaterial), unless this
        // subject is being photographed FOR its skin.
        const bool greyBody = (a_mannequin || a_bodySubject) && !a_texturedBody;
        // Whether any swap on this root picks by the geometry's own TX00 file
        // name (the skin pack rule). Read once so the loop below reads a
        // material only when an entry can use it.
        bool swapsByName = false;
        if (a_swaps) {
            for (const auto& e : *a_swaps) {
                if (!e.whenTex0Name.empty()) {
                    swapsByName = true;
                    break;
                }
            }
        }
        if (!a_device || !a_ctx || !a_root) {
            return false;
        }
        std::vector<GeometryCandidate> candidates;
        std::uint32_t                  legacy = 0;
        CollectGeometries(a_root, std::string{}, false, RE::NiTransform{}, candidates,
                          legacy);
        a_stats.legacyGeometry += legacy;

        // The skeleton alignment (OS-204). Reported out for the mannequin's
        // head so a later root can align to it, and applied here for a hair
        // whose own skeleton disagrees with it.
        if (a_anchorOut) {
            FindHeadAnchor(a_root, RE::NiTransform{}, *a_anchorOut);
        }
        // ⚠⚠ A HEAD PART WITH NO SKELETON IS AUTHORED IN HEAD SPACE, AND
        // WITHOUT THIS IT IS PHOTOGRAPHED AT THE MANNEQUIN'S ANKLES. Measured
        // 2026-08-15 off the preview.boxes line: of forty eye cards, thirty-two
        // put their mesh at z 122.92..124.38 and eight put it at z 2.57..4.04,
        // against an eye window covering 121.5..123.8. The eight are the
        // separate faceparts/eyes*left.nif and eyes*right.nif files the blind
        // variants use, and they rendered as blank cards - reported as "a lot of
        // eyes are still missing".
        //
        // The skeleton alignment could not help them, and the 2026-08-15
        // probe run measured why: the faceparts files DO carry an npc head
        // node (mine.valid true on every eye root), and that skeleton agrees
        // with the mannequin's, so the subtraction is zero and nothing moves.
        // The fault is in the GEOMETRY: it is authored in head space while
        // the skeleton stands at world height. Zero align lines either way.
        //
        // ⚠⚠ AND THE OBVIOUS FIX WOULD HAVE BROKEN THE THIRTY-TWO. Treating a
        // missing anchor as the origin translates every skeleton-less head part
        // by the full head height, which is right for these eight and wrong for
        // eyesfemale.nif, whose vertices are already authored at world height
        // and would have been pushed to z 243, off the top of every card. Both
        // kinds have identity node transforms, so the node graph cannot tell
        // them apart. The GEOMETRY can, and it is the thing being framed.
        //
        // So the test is a comparison rather than a threshold: is this mesh
        // nearer the origin than it is to the head it belongs to? Head-local
        // authoring sits at a small offset within the head and answers yes;
        // world authoring sits on the head and answers no. A mesh exactly
        // between them is left alone, which is the old behaviour, so the
        // fail-safe direction is "do nothing".
        //
        // ⚠ GetModelData(), NOT the bare modelBound member: that member is
        // compiled out on the VR layout and this ships one universal DLL. Same
        // trap GetFlags() carries in the swap loop below.
        const auto headLocalOffset = [&]() -> const NodeAnchor* {
            // ⚠⚠ THE MANNEQUIN'S OWN HEAD EARNS THIS TEST ON EVERY KIND, and
            // a_headPartScene alone refused it on 2026-08-20. That flag says
            // what the CARD IS ABOUT, not what this root is: on an armour card
            // it is false, so the figure's head returned here before measuring
            // anything and logged neither of the two lines below. The field
            // read exactly that way, and it is worth naming because the silence
            // is the confusing part: 44 head-mine lines said the head reached
            // the align block, and zero head-local-z lines said the geometry
            // test never ran, which looks like a missing branch rather than a
            // refused one.
            //
            // The head root is a head part whatever the card is about, so the
            // subject's kind cannot be the gate for it.
            if ((!a_headPartScene && !a_mannequinHead) || !a_alignTo ||
                !a_alignTo->valid) {
                return nullptr;
            }
            // ⚠⚠ AN EMPTY BOUND IS NOT A MEASUREMENT, AND TREATING IT AS ONE
            // MADE MOST OF THE HAIR CARDS BALD (field 2026-08-16, screenshot of
            // the hair page). A skinned BSDynamicTriShape carries its placement
            // in the SKIN, so the file leaves the bounding sphere at zero and
            // the shape's own node at the origin: measured in KS Hairdo's
            // Butterfly130.nif, CandyBar.nif and Camellia2.nif, every one of
            // them center (0,0,0) radius 0 translate (0,0,0). That sums to
            // exactly 0.000, which reads as "this mesh sits at the origin" and
            // wins the comparison against a head at 120.34, so the hair was
            // lifted a whole head height above the mannequin and off the top of
            // the card. 88 of 88 head-local verdicts in the field log measured
            // that same 0.000, while every real measurement in the same run
            // read 115..130.
            //
            // ⚠ THE RADIUS IS THE TEST, AND THE EIGHT EYES THE BRANCH EXISTS
            // FOR STILL PASS IT: faceparts EyesFemaleLeft.nif measures center
            // z 3.326 radius 1.501, a real bound around a real head-local
            // mesh. So this refuses exactly the shapes that never answered the
            // question and nothing else.
            //
            // ⚠ AND THE BALD HAIRS NEED NO CORRECTION AT ALL. They reach this
            // test only because their own skeleton already agrees with the
            // mannequin's (both heads at z 120.344), which is the file saying
            // it is authored where the mannequin stands. Doing nothing is
            // right for them, and it is what the fail-safe direction above
            // already promised.
            float       zSum     = 0.0f;
            std::size_t n        = 0;
            std::size_t unbound  = 0;
            for (const auto& cand : candidates) {
                if (!cand.geometry) {
                    continue;
                }
                const auto& bound = cand.geometry->GetModelData().modelBound;
                if (!(bound.radius > 0.0f)) {
                    ++unbound;
                    continue;
                }
                zSum += cand.transform.translate.z + bound.center.z;
                ++n;
            }
            if (n == 0) {
                spdlog::info("MeshExtractor: preview.head-local-z '{}' has no "
                             "geometry with a measured bound ({} shape(s) carry an "
                             "empty one), so the head-space question cannot be "
                             "answered and nothing is moved.",
                             a_modelPath, unbound);
                return nullptr;
            }
            const float z      = zSum / static_cast<float>(n);
            const bool  nearer = std::abs(z) < std::abs(z - a_alignTo->z);
            // Either verdict is worth a line: "leave" on a mesh whose card
            // is blank means the geometry test refused it, which is the next
            // question after the anchor reads valid.
            spdlog::info("MeshExtractor: preview.head-local-z '{}' z={:.3f} "
                         "anchorZ={:.3f} measured={} unbound={} verdict={}",
                         a_modelPath, z, a_alignTo->z, n, unbound,
                         nearer ? "align" : "leave");
            return nearer ? a_alignTo : nullptr;
        };

        // ⚠⚠ THE GEOMETRY TEST MUST NOT HIDE BEHIND "HAS NO SKELETON".
        // Measured 2026-08-15 (preview.head-anchor probe): the faceparts
        // eyes DO put an npc head node in their own graph (mine.valid true,
        // blend=4bones), and that skeleton agrees with the mannequin's, so
        // the head-align delta is zero and ShouldAlignToHead refuses. Their
        // GEOMETRY is still authored head-local (measured z 2.57..4.04
        // against an anchor at 120.34) and still rendered at the ankles.
        // So the gate is "the skeleton offered no correction", whether that
        // is because there is no skeleton or because the skeleton matches;
        // the geometry comparison below is what separates head-local
        // authoring from world authoring either way.
        // ⚠⚠ TWO PAINTERS OF ONE PLACEMENT, AND THEY ARE MADE EXCLUSIVE HERE.
        // The head corrections below and the bind pose in ExtractOne answer the
        // same question, "where does this geometry really stand", and they agree
        // on head-slot content: a circlet skinned to NPC Head has vertices near
        // the origin and a bind product of +120.344, which is exactly the lift
        // the head-local branch applies. Running both puts a helmet at +240.
        //
        // ⚠ THE HEAD PATH WINS, because it is the one the field closed on
        // 2026-08-20 and this stint is about armour. So the bind pose stands
        // down wherever a head correction has already moved the candidates, and
        // the case it exists for (a garment on a body slot, where a_alignTo
        // moves nothing) is untouched by that. Measured over this load order:
        // of 25527 skinned shapes in 12599 worn meshes, 80.1% need no
        // correction at all, 11.6% take one, and the top of the 11.6% is
        // helmets and circlets, which is precisely the overlap this closes.
        bool headCorrected = false;
        if (a_alignTo && a_alignTo->valid) {
            NodeAnchor mine;
            FindHeadAnchor(a_root, RE::NiTransform{}, mine);
            spdlog::info("MeshExtractor: preview.head-mine '{}' mineValid={} "
                         "mineZ={:.3f} anchorZ={:.3f}",
                         a_modelPath, mine.valid, mine.z, a_alignTo->z);
            bool movedBySkeleton = false;
            if (mine.valid) {
                const float dx = a_alignTo->x - mine.x;
                const float dy = a_alignTo->y - mine.y;
                const float dz = a_alignTo->z - mine.z;
                // ⚠ A CORRECTLY AUTHORED FILE MEASURES EXACTLY ZERO HERE and
                // must not be touched at all, which is what makes this safe to
                // apply to a whole catalog: the 2277 KS hairs on the standard
                // skeleton keep byte-identical geometry and only the 264 on
                // the other one move.
                if (PreviewFilter::ShouldAlignToHead(dx, dy, dz)) {
                    for (auto& cand : candidates) {
                        cand.transform.translate.x += dx;
                        cand.transform.translate.y += dy;
                        cand.transform.translate.z += dz;
                    }
                    ++a_stats.aligned;
                    movedBySkeleton = true;
                    headCorrected   = true;
                    spdlog::info("MeshExtractor: preview.head-align '{}' by "
                                 "({:.3f}, {:.3f}, {:.3f}); its skeleton puts the head "
                                 "at ({:.3f}, {:.3f}, {:.3f}) and the mannequin's is at "
                                 "({:.3f}, {:.3f}, {:.3f}).",
                                 a_modelPath, dx, dy, dz, mine.x, mine.y, mine.z,
                                 a_alignTo->x, a_alignTo->y, a_alignTo->z);
                }
            }
            if (!movedBySkeleton) {
                if (const auto* to = headLocalOffset()) {
                    for (auto& cand : candidates) {
                        cand.transform.translate.x += to->x;
                        cand.transform.translate.y += to->y;
                        cand.transform.translate.z += to->z;
                    }
                    ++a_stats.aligned;
                    headCorrected = true;
                    spdlog::info(
                        "MeshExtractor: preview.head-local '{}' took no correction "
                        "from its skeleton and its geometry sits nearer the origin "
                        "than the head, so it is authored in head space; moved by "
                        "({:.3f}, {:.3f}, {:.3f}) onto the mannequin's head. Without "
                        "this it renders at the ankles and its card is blank.",
                        a_modelPath, to->x, to->y, to->z);
                }
            }
        }

        // Folded ONCE for the whole root rather than per geometry: the scope
        // question is about the NIF, and the candidate loop below can run to
        // several hundred.
        const std::string foldedModel =
            a_scopes ? PreviewGrid::FoldPath(a_modelPath) : std::string{};

        const std::size_t before = a_out.size();
        // The swap SRVs, loaded once per entry and shared by every mesh the
        // entry paints (an eye TNAM covers the whole model).
        std::unordered_map<const PreviewGrid::TextureSwapEntry*,
                           Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
            swapSrvs;
        // ⚠ The swap index is the candidate index over BSGeometry ALONE, and
        // the engine's own walker is why. CollectGeometries visits every
        // geometry depth first in child order, which is that walker's running
        // counter (re_verify/ae_swap_walker.c), and the filters below drop
        // candidates AFTER this numbering, so a filtered mesh costs no index.
        //
        // ⚠⚠ BUT A LEGACY SHAPE COSTS NO INDEX EITHER, AND THAT IS MEASURED
        // OFF THE WALKER RATHER THAN ASSUMED. It tests AsNode, then AsGeometry,
        // and returns on a leaf that is neither WITHOUT incrementing the
        // counter. A `NiTriShape` answers null to both, so the engine never
        // numbers one and never applies a swap to one. OS-237 put those shapes
        // into this list to draw them, so the counter has to skip them or every
        // swap on a model that mixes the two kinds lands on the wrong shape.
        std::uint32_t swapIndex = 0;
        for (std::size_t candIndex = 0; candIndex < candidates.size(); ++candIndex) {
            const auto&         cand      = candidates[candIndex];
            const std::uint32_t thisIndex = swapIndex;
            if (cand.geometry) {
                ++swapIndex;
            }
            // The geometry's own TX00 file name, for the by-name entries only:
            // a plain read of the property the extractor reads anyway a few
            // lines down, before the SEH frame because it dereferences the
            // same pointers ResolveMaterial does with the same null checks.
            std::string tex0Name;
            if (swapsByName && cand.geometry) {
                auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    cand.geometry->GetGeometryRuntimeData()
                        .properties[RE::BSGeometry::States::kEffect]
                        .get());
                auto* const material =
                    prop ? static_cast<RE::BSLightingShaderMaterialBase*>(prop->material)
                         : nullptr;
                const char* tex0 =
                    material && material->textureSet
                        ? material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)
                        : nullptr;
                if (tex0 && *tex0) {
                    tex0Name = SkinPlan::FileName(tex0);
                }
            }
            const auto* swap = (a_swaps && cand.geometry)
                                   ? PreviewGrid::SwapFor(*a_swaps, thisIndex, tex0Name)
                                   : nullptr;
            ++a_stats.geometries;
            if (cand.hidden) {
                // The NIF's own cull flag, honoured and counted apart from
                // anything this side decides, so "the NIF hid it" stays
                // distinguishable in the log.
                ++a_stats.nodeCulled;
                continue;
            }
            // The scene's name rules (PreviewFilter.h). The census line is
            // the audit trail: every drop names itself at debug, so the
            // helper-token over-match and a wrongly hidden garment piece are
            // findable in a field log instead of being a silent hole.
            const auto             folded = PreviewGrid::FoldPath(cand.namePath);
            const auto             slash  = folded.rfind('/');
            const std::string_view leaf =
                slash == std::string::npos
                    ? std::string_view{ folded }
                    : std::string_view{ folded }.substr(slash + 1);
            // ⚠⚠ THE SLOT GATES AND THE BODY RULE ARE ABOUT A GARMENT'S
            // STOWAWAYS, AND THE MANNEQUIN IS NEITHER. Its hands leaf really
            // is called "hands" and its body really is a body, so on a
            // mannequin root these three rules delete exactly the thing the
            // scene was composed to show. Which mannequin parts belong in a
            // scene was already decided, once, by MannequinPartShows against
            // the item's slots; asking again here with the GARMENT's question
            // could only ever disagree with it.
            //
            // Field 2026-08-10: it did. Every card lost 'scene root/hands' and
            // 'scene root/feet' to the slot gates and the body and head to the
            // face flags, so a boots card logged five paths in its key and
            // meshes=1.
            const bool handsHidden = !filtersExempt && PreviewFilter::IsHandsLeaf(leaf) &&
                                     (a_slotMask & PreviewFilter::kHandsSlotBit) == 0;
            const bool feetHidden = !filtersExempt && PreviewFilter::IsFeetLeaf(leaf) &&
                                    (a_slotMask & PreviewFilter::kFeetSlotBit) == 0;
            // The authored per-path fixups (OS-191), asked before the
            // built-in name rules so a Show can rescue what they over-match.
            const auto verdict =
                a_scopes ? PreviewScopes::VerdictFor(*a_scopes, foldedModel, folded)
                         : PreviewScopes::Verdict::kNoOpinion;
            if (verdict == PreviewScopes::Verdict::kHide) {
                ++a_stats.scopeFiltered;
                spdlog::debug("MeshExtractor: preview.scope-filtered '{}' in '{}'",
                              folded, foldedModel);
                continue;
            }
            // ⚠⚠ WHAT A Show MAY RESCUE IS PINNED HERE AND NOWHERE ELSE. It
            // beats the helper-token rule, which is the documented over-match
            // ("a real piece named HelperPlate would disappear") and the
            // whole reason a rescue exists. It must NOT beat the
            // embedded-body or hands/feet rules: those are correctness rules
            // tied to body doubles and slot occupancy rather than name
            // guesses, so a rescue there puts a second body back inside the
            // garment to z-fight with it, which is a defect nobody asked for
            // and one a fixup file should not be able to cause.
            const bool rescued = verdict == PreviewScopes::Verdict::kShow;
            // The helper tokens still apply to a mannequin root: collision
            // capsules and bounding boxes are scaffolding in a body NIF for
            // exactly the reason they are in a garment.
            // ⚠⚠ THE PHYSICS PROXIES ARE DROPPED HERE AND NOT ONLY IN
            // ChooseShapeMorph, WHICH IS OS-205's SECOND HALF. That rule fires
            // only where a slider set described the shapes, and a set is
            // handed to the BODY-card paths alone (PreviewCache: morphPathCount).
            // So on an item card the mannequin's body root arrives with an
            // empty list, the list has no opinion, and VirtualArms through
            // VirtualFeet survive.
            //
            // They are not merely drawn. The reference box is measured off
            // that root, and VirtualFeet on this rig's male body reaches
            // z -0.2907 where the body itself stops at 11.3191, so a HIMBO
            // mannequin measured 11.6 units taller than it is and every window
            // on every male card was displaced and 11% too tall (measured
            // 2026-08-18). VirtualGround was already caught, by the helper
            // token list, which is why only the ground plane ever left.
            //
            // ⚠ NOT RESCUABLE, unlike the helper tokens: this is a
            // correctness rule about a collider rather than a guess about a
            // name, and the game does not render these either.
            if ((!rescued && PreviewFilter::PathHasHelperToken(folded)) ||
                (!filtersExempt && PreviewFilter::IsEmbeddedBodyLeaf(leaf)) ||
                PreviewFilter::IsPhysicsProxyLeaf(leaf) || handsHidden || feetHidden) {
                ++a_stats.nameFiltered;
                spdlog::debug("MeshExtractor: preview.filtered '{}'", folded);
                continue;
            }
            // ⚠⚠ EACH GEOMETRY TAKES THE FIELD MEASURED AGAINST IT, and one
            // that the slider set never describes is not drawn. That covers
            // the physics rig, which this rig's male body ships in the same
            // file as the body: VirtualArms, VirtualBelly, VirtualBreasts and
            // VirtualButt, collision proxies of a few hundred vertices that
            // the game never renders. Denying them the morph alone was not
            // enough, because it left them sitting UNMORPHED inside a body
            // that had moved, poking through it at the chest, belly and hips
            // (field 2026-08-10).
            //
            // ⚠ AN EMPTY LIST IS NO OPINION. The head, hands and feet arrive
            // as their own roots with no shape list, as does every armour and
            // weapon scene, and all of them keep every geometry and take no
            // morph. The rule fires only where a source described the shapes.
            const auto choice = PreviewFilter::ChooseShapeMorph(shapeViews, leaf);
            if (choice.drop) {
                ++a_stats.nameFiltered;
                spdlog::debug("MeshExtractor: preview.not-morph-shape '{}' (the card's "
                              "set builds {} shape(s))",
                              folded, shapeViews.size());
                continue;
            }
            const auto morph = choice.index == PreviewFilter::kNoShapeMorph
                                   ? std::span<const std::array<float, 3>>{}
                                   : a_morphs[choice.index].deltas;
            RenderMesh    mesh;
            std::uint32_t code = 0;
            // ⚠ GREY IS A WIDER SET THAN THE MANNEQUIN, and only here. The
            // mannequin flag still means "measured differently" everywhere
            // else in this function; what ResolveMaterial wants is "draw this
            // as a grey figure", which a photographed body asks for too.
            if (ExtractOneSEH(a_device, a_ctx, &cand, &mesh, &a_stats, &code,
                              a_headPartScene, greyBody,
                              a_keepEffect, a_keepBlend, filtersExempt, headCorrected,
                              morph.data(), morph.size())) {
                // The swap overrides the extracted DIFFUSE, nothing else:
                // no engine material is written (the hover-CTD lesson at
                // LoadSwapDiffuse). A swap that promised a diffuse and
                // delivered none is a texture-load miss, not a fact about
                // the item: the caller fails the build WITHOUT persisting
                // on that count, so a transient can never freeze a wrong
                // card into the cache. An authored-empty TX00 goes flat on
                // purpose, the in-game outcome of a cleared diffuse.
                if (swap) {
                    if (swap->texPaths[0].empty()) {
                        mesh.Diffuse.Reset();
                        ++a_stats.swapApplied;
                    } else {
                        auto it = swapSrvs.find(swap);
                        if (it == swapSrvs.end()) {
                            it = swapSrvs.emplace(swap, LoadSwapDiffuse(*swap))
                                     .first;
                        }
                        if (it->second) {
                            mesh.Diffuse = it->second;
                            ++a_stats.swapApplied;
                        } else {
                            ++a_stats.swapNoDiffuse;
                            spdlog::debug(
                                "MeshExtractor: preview.swap-no-diffuse '{}' on '{}'",
                                swap->texPaths[0], cand.namePath);
                        }
                    }
                }
                mesh.Mannequin     = a_mannequin;
                mesh.MannequinHead = a_mannequinHead;
                mesh.MannequinFeet = a_mannequinFeet;
                a_out.push_back(std::move(mesh));
                ++a_stats.extracted;
            } else if (code != 0) {
                ++a_stats.faulted;
                spdlog::warn(
                    "MeshExtractor: geometry '{}' FAULTED with 0x{:08X} and was "
                    "skipped; the item should be recorded as a persistent failure.",
                    cand.namePath, code);
            }
        }
        // APPENDS by contract: a multi-NIF scene accumulates across calls,
        // so the verdict is about THIS root's contribution alone.
        return a_out.size() > before;
    }

}  // namespace OS::MeshExtractor
