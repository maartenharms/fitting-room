#include "Overlay1P.h"

#include "LegacyGeometry.h"  // the template's shader and alpha refs, measured
#include "NifModelLoader.h"  // the template load, through BSResourceNiBinaryStream
#include "OverlayApi.h"      // Layers, Available, PushNodeProperties
#include "OverlayPlan.h"     // Location
#include "VersionCheck.h"    // IdOk, membership rather than "REL gave me an address"

#include <string>
#include <string_view>

namespace OS::Overlay1P {

    namespace {

        // skee's own template meshes, verbatim from its OverlayInterface.h
        // (BODY_MESH and HAND_MESH). They ship inside RaceMenu.bsa, so the load
        // has to go through BSResourceNiBinaryStream: an archived file is
        // invisible to std::filesystem (filesystem-exists-is-blind-to-archives).
        constexpr const char* kBodyMesh =
            "meshes\\actors\\character\\character assets\\body_overlay.nif";
        constexpr const char* kHandMesh =
            "meshes\\actors\\character\\character assets\\hands_overlay.nif";

        // The engine's instantiate-a-template deep clone: a stack
        // NiCloningProcess wired to the engine's own copyType and appendChar
        // globals, then CreateClone plus ProcessClone. It is the same call every
        // placed reference in the world is built with.
        //
        // ⚠ THE IDS ARE Menu Studio'S, ALREADY FIELD-PROVEN on this machine
        // (its src/Offsets.h, backdrop pieces). Reusing a measured pair beats
        // deriving a second one, and the IdOk gate below is what makes a wrong
        // id safe rather than a call into a neighbour.
        constexpr REL::RelocationID kNiAVObjectClone{ 68835, 70187 };
        using NiClone_t = RE::NiAVObject* (*)(RE::NiAVObject*);

        [[nodiscard]] bool CloneIdOk() {
            static const bool ok = [] {
                const bool good = OS::VersionCheck::IdOk(kNiAVObjectClone);
                if (!good) {
                    spdlog::error(
                        "Overlay1P: NiAVObject::Clone is not in this runtime's Address "
                        "Library, so the first person keeps whatever overlay clones skee "
                        "made and Fitting Room adds none.");
                }
                return good;
            }();
            return ok;
        }

        // ⚠ AN RTTI CHAIN TEST, BECAUSE A LEGACY SHAPE IS NOT A BSGeometry. The
        // template is a BSVersion 83 file and loads as NiTriShape, which AE keeps
        // live rather than converting; AsGeometry() answers null on it and a
        // BSGeometry-only walk would drop it silently. Same test MeshExtractor
        // makes for the same reason.
        [[nodiscard]] bool RttiChainNames(RE::NiObject* a_object, std::string_view a_class) {
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

        [[nodiscard]] RE::BSLightingShaderProperty* LightingOf(RE::BSGeometry* a_geometry) {
            if (!a_geometry) {
                return nullptr;
            }
            return netimmerse_cast<RE::BSLightingShaderProperty*>(
                a_geometry->GetGeometryRuntimeData()
                    .shaderProperty
                    .get());
        }

        [[nodiscard]] RE::BSLightingShaderMaterialBase* MaterialOf(
            RE::BSLightingShaderProperty* a_property) {
            return a_property
                       ? static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material)
                       : nullptr;
        }

        // skee's GetFirstShaderType(node, kShaderType_FaceGenRGBTint): the shape
        // an overlay is cloned off. No such shape means skee UNINSTALLS that
        // location's overlays rather than installing them, which is why a gloved
        // hands slot legitimately holds no hand overlay.
        [[nodiscard]] RE::BSGeometry* FirstSkinTint(RE::NiAVObject* a_node) {
            if (!a_node) {
                return nullptr;
            }
            RE::BSGeometry* found = nullptr;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_node, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
                    auto* const material = MaterialOf(LightingOf(a_geometry));
                    if (material &&
                        material->GetFeature() ==
                            RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
                        found = a_geometry;
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return found;
        }

        // What one clone takes off skee's template.
        struct TemplateProps {
            RE::NiPointer<RE::BSShaderProperty> shader;
            RE::NiPointer<RE::NiAlphaProperty>  alpha;
        };

        // ⚠⚠ A FRESH LOAD PER CLONE, NEVER ONE CACHED PAIR FOR A WHOLE LOCATION.
        // skee calls InstallOverlay once per node and each call re-opens the
        // template, so every [Ovl] ends up owning its material. Sharing one
        // property across six body layers would give them one material between
        // them, and the first layer painted would be the only layer visible.
        //
        // The NiPointers take their references before the loaded tree goes away,
        // which is what keeps the two properties alive after the root is dropped.
        [[nodiscard]] TemplateProps LoadTemplateProps(const char* a_mesh) {
            TemplateProps out;
            const auto    root = NifModelLoader::Load(a_mesh);
            if (!root) {
                return out;
            }
            for (auto& child : root->GetChildren()) {
                auto* const legacy = AsLegacyGeometry(child.get());
                if (!legacy) {
                    continue;
                }
                // The geometry's runtime data types its two properties now, so the
                // template's are read through their RTTI rather than held as the
                // NiProperty base.
                out.shader = RE::NiPointer<RE::BSShaderProperty>{
                    netimmerse_cast<RE::BSShaderProperty*>(LegacyGeometry::ShaderProperty(legacy))
                };
                out.alpha = RE::NiPointer<RE::NiAlphaProperty>{
                    netimmerse_cast<RE::NiAlphaProperty*>(LegacyGeometry::AlphaProperty(legacy))
                };
                break;
            }
            return out;
        }

        // Build one [Ovl] clone the way skee builds it, and attach it.
        //
        // Returns true when a clone was attached. Everything is done before the
        // attach on purpose: an unattached shape is inert, so a half-built one
        // never reaches the renderer.
        [[nodiscard]] bool BuildClone(RE::NiNode* a_root, RE::BSGeometry* a_source,
                                      const std::string& a_node, const char* a_mesh) {
            const auto props = LoadTemplateProps(a_mesh);
            if (!props.shader) {
                spdlog::warn("Overlay1P: '{}' gave no shader property, so '{}' is not built. "
                             "That mesh is RaceMenu's and lives in its BSA.",
                             a_mesh, a_node);
                return false;
            }

            static const auto clone = reinterpret_cast<NiClone_t>(kNiAVObjectClone.address());

            const RE::NiPointer<RE::NiAVObject> made{ clone(a_source) };
            auto* const                        geometry = made ? made->AsGeometry() : nullptr;
            if (!geometry) {
                spdlog::warn("Overlay1P: the engine clone of the skin shape gave no geometry, "
                             "so '{}' is not built.",
                             a_node);
                return false;
            }

            made->name = a_node.c_str();

            auto&       runtime = geometry->GetGeometryRuntimeData();
            const auto& source  = a_source->GetGeometryRuntimeData();

            // ⚠⚠ THE PROPERTIES ARE REPLACED AND NOT EDITED. The engine's clone
            // SHARES its shader property and its material with what it was cloned
            // from, so a clone that kept them would hand skee the player's own
            // SKIN material to write the overlay art onto.
            runtime.shaderProperty = props.shader;
            // ⚠ ONLY WHEN THE TEMPLATE HAS ONE, which is skee's own guard. This
            // rig's template does carry a NiAlphaProperty of 0x12ED with a
            // threshold of 0, and those are exactly the iAlphaFlags and
            // iAlphaThreshold in the installed skee64.ini, so taking it is
            // taking the override's value. A template without one must leave the
            // clone's alone rather than clear it.
            if (props.alpha) {
                runtime.alphaProperty = props.alpha;
            }

            // ⚠⚠ ONE SKIN, SHARED, EXACTLY AS skee SHARES IT. A skinned
            // BSTriShape in Special Edition keeps its vertices and triangles in
            // the NiSkinPartition rather than on the shape, so this single
            // pointer is the clone's whole geometry, its bones and its partition.
            // Nothing is remapped, which is why the two bone-index scars do not
            // reach this code.
            runtime.skinInstance = source.skinInstance;
            runtime.vertexDesc   = source.vertexDesc;
            made->local          = a_source->local;

            // The two flags skee copies off the source, set or cleared to match
            // it rather than or-ed in.
            //
            // ⚠ MODEL SPACE NORMALS IS THE ONE THAT MATTERS HERE AND THE TEMPLATE
            // HAS IT ON. Measured out of RaceMenu.bsa: the overlay template's
            // shaderType is 5 with flags1 0x82601303, which carries
            // ModelSpaceNormals. A tangent-space body (this rig's UBE hands mesh
            // is literally femalehands_tangent_1.nif) would have its normal map
            // read the wrong way round if the clone kept it, and the clearing is
            // why a live layer measures without it.
            if (auto* const dst = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    props.shader.get())) {
                if (auto* const src = LightingOf(a_source)) {
                    using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
                    using Bit  = RE::BSShaderProperty::EShaderPropertyFlag8;
                    dst->SetFlags(Bit::kVertexColors,
                                  src->flags.all(Flag::kVertexColors));
                    dst->SetFlags(Bit::kModelSpaceNormals,
                                  src->flags.all(Flag::kModelSpaceNormals));
                }

                // ⚠ SLOTS 1 THROUGH 8 COME OFF THE SKIN, SLOT 0 DOES NOT. skee
                // copies exactly this range at install and again at every
                // ResetOverlay, so the overlay wears the body's own normal,
                // subsurface and environment maps while its diffuse stays the
                // layer's art. Slot 0 is left at the template's default texture,
                // which is what an empty layer is supposed to show.
                auto* const dstMaterial = MaterialOf(dst);
                auto* const srcMaterial = MaterialOf(LightingOf(a_source));
                if (dstMaterial && srcMaterial && dstMaterial->textureSet &&
                    srcMaterial->textureSet) {
                    for (int slot = 1; slot < RE::BSTextureSet::Textures::kTotal; ++slot) {
                        const auto which = static_cast<RE::BSTextureSet::Texture>(slot);
                        if (const char* path = srcMaterial->textureSet->GetTexturePath(which)) {
                            dstMaterial->textureSet->SetTexturePath(which, path);
                        }
                    }
                    dstMaterial->ClearTextures();
                    dstMaterial->OnLoadTextureSet(0, dstMaterial->textureSet.get());
                }

                // ⚠ DoClearRenderPasses AND NOTHING ELSE. SetupGeometry and
                // FinishSetupGeometry mutate a property one way only and nothing
                // puts back what they clear; they were the shine OutfitDye.h
                // documents. Invalidating the pass list is enough, because
                // GetRenderPasses re-picks the technique off it.
                dst->DoClearRenderPasses();
            }

            a_root->AttachChild(made.get(), false);
            return true;
        }

        struct LocationPlan {
            OverlayPlan::Location          location;
            RE::BIPED_OBJECTS::BIPED_OBJECT slot;
            const char*                    mesh;
        };

        // The first person renders the body and the hands and nothing else, so
        // feet and face have no slot to paint there.
        constexpr LocationPlan kPlan[] = {
            { OverlayPlan::Location::kBody, RE::BIPED_OBJECTS::kBody, kBodyMesh },
            { OverlayPlan::Location::kHands, RE::BIPED_OBJECTS::kHands, kHandMesh },
        };

    }  // namespace

    int PaintPlayer(RE::Actor* a_actor) {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!a_actor || a_actor != player || !OverlayApi::Available() || !CloneIdOk()) {
            return 0;
        }
        auto* const root3d = player->Get3D(true);
        auto* const root   = root3d ? root3d->AsNode() : nullptr;
        if (!root) {
            return 0;  // no first person 3D loaded; nothing to hang a clone on
        }
        const auto& biped = player->GetBiped1(true);
        auto* const anim  = biped.get();
        if (!anim) {
            return 0;
        }

        int built = 0;
        for (const auto& plan : kPlan) {
            auto* const worn =
                anim->objects[static_cast<std::size_t>(plan.slot)].partClone.get();
            auto* const source = FirstSkinTint(worn);
            if (!source) {
                // No Skin_Tint shape on this slot, so there is nothing to clone
                // off and skee would have uninstalled here too. A gloved hand is
                // the ordinary case.
                continue;
            }
            for (const auto& layer : OverlayApi::Layers()) {
                if (layer.location != plan.location) {
                    continue;
                }
                // Idempotent by the same name check skee's own install makes, so
                // a settled point that runs again costs one lookup per layer.
                const RE::BSFixedString name{ layer.node.c_str() };
                if (root->GetObjectByName(name)) {
                    continue;
                }
                if (BuildClone(root, source, layer.node, plan.mesh)) {
                    ++built;
                }
            }
        }

        if (built > 0) {
            spdlog::info("Overlay1P: built {} overlay clone(s) on the player's first person "
                         "root, which the installed skee never makes. Pushing the layers "
                         "once so skee paints them.",
                         built);
            // ⚠ ONE PUSH FOR THE WHOLE BUILD, and it is skee's own walk. Its
            // Impl_SetNodeProperties finds these by name on BOTH roots, so the
            // node overrides the player already has land on them with nothing
            // here touching a material.
            OverlayApi::PushNodeProperties(a_actor);
        }
        return built;
    }

}  // namespace OS::Overlay1P
