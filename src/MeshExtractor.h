#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "PreviewGrid.h"    // TextureSwapEntry, SwapFor (pure)
#include "PreviewScopes.h"  // per-path hide/show fixups (pure)

// Walks a standalone-loaded NIF and turns every visible geometry into a
// drawable mesh: vertex and index buffers in the preview's own layout, plus
// the material facts the preview shader's constant buffer wants.
//
// ⚠ THE THREE OUT-OF-BOUNDS FIXES LIVE HERE AND ARE NOT OPTIONAL. Wardrobe
// ships this pipeline with an index clamp applied only on its hair path, a
// vertex count grown from the NIF's own index data with no buffer bound, and
// a dynamic-shape size leniency that admits a 16x overrun. All three read
// third-party bytes and all three are fixed on EVERY path in this port; the
// spec's Appendix C carries the corrected code this file follows.
namespace OS::MeshExtractor {

    struct PreviewVertex {
        float Px{ 0.0f }, Py{ 0.0f }, Pz{ 0.0f };
        float U{ 0.0f }, V{ 0.0f };
        float Nx{ 0.0f }, Ny{ 0.0f }, Nz{ 1.0f };
    };

    // Where a NIF's own skeleton puts the head bone, in the root's space.
    //
    // ⚠⚠ THE HAIR THAT SITS LOW IS AUTHORED LOW, and this is how that is
    // detected without a hand-written table. KS Hairdo's ships two authoring
    // skeletons: 2277 of its files put "NPC Head [Head]" at (0, -1.548,
    // 120.344), which is where this load order's head mesh puts it, and 264
    // put it at (0, +0.997, 117.056) - the same rigid translation on every
    // bone, not a rotation. A hair on the second one hangs 3.3 units low and
    // the grey scalp comes through the top of it, which was the field report
    // (Flower, Flying Dance and Firenze, 2026-08-10; nine cards in total, and
    // Fairytale is worse than any of the three that got named).
    //
    // Comparing against the MANNEQUIN's own head rather than against a
    // constant is what keeps this rig-independent: the reference is whatever
    // head the scene is actually wearing, so a different body ships a
    // different number and the arithmetic is unchanged. A correctly authored
    // hair measures a delta of exactly zero and not one vertex moves.
    struct NodeAnchor {
        bool  valid{ false };
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    // One geometry's morph field, named by the shape it was measured against.
    // The name is matched against the geometry's leaf, folded, so the caller
    // passes it exactly as the slider set spells it.
    struct ShapeMorph {
        std::string_view                      shape;
        std::span<const std::array<float, 3>> deltas;
    };

    struct RenderMesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer>             VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>             IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Diffuse;  // null = flat shading
        std::uint32_t                                    IndexCount{ 0 };
        bool                                             UseAlpha{ false };
        bool                                             BlendAlpha{ false };
        float                                            MaterialAlpha{ 1.0f };
        float                                            AlphaCutoff{ 0.0f };
        float                                            SpecularStrength{ 0.10f };
        float                                            SpecularPower{ 42.0f };
        std::array<float, 3>                             EmissiveColor{ 0.0f, 0.0f, 0.0f };
        float                                            EmissiveStrength{ 0.0f };
        std::array<float, 3>                             BoundsMin{};
        std::array<float, 3>                             BoundsMax{};
        bool                                             HaveBounds{ false };
        // Whether this mesh is the mannequin rather than the item (OS-204).
        // The framing needs the two boxes apart: a slab is a fraction of the
        // BODY's height, and measuring it off a scene box that a spiked helmet
        // has made taller slides the head slab up off the head.
        bool                                             Mannequin{ false };
        // The mannequin's HEAD specifically: drawn like any other part, but
        // never measured, because it is the one part that can be absent and a
        // reference box that changes size moves every standard view.
        bool                                             MannequinHead{ false };
        // The mannequin's FEET, on exactly the same terms as the head: drawn,
        // never measured. Footwear drops them, and they are what the box
        // stands on, so measuring them puts a boots card's floor at the hips.
        bool                                             MannequinFeet{ false };
    };

    // The measure-first counters. diffuseResolved versus diffuseMissing is
    // the number that decides whether the absent BSShaderManager fallback
    // matters at all (spec: instrument, then decide), and skinPath counts the
    // geometries phase 1 deliberately skips, so phase 2 knows the real rate.
    struct ExtractStats {
        std::uint32_t geometries{ 0 };
        std::uint32_t extracted{ 0 };
        std::uint32_t nodeCulled{ 0 };
        std::uint32_t faulted{ 0 };
        std::uint32_t skinPath{ 0 };
        std::uint32_t rendererPath{ 0 };
        std::uint32_t diffuseResolved{ 0 };
        std::uint32_t diffuseMissing{ 0 };
        std::uint32_t effectSkipped{ 0 };  // FX planes (BSEffectShaderProperty)
        // FX planes caught by what they DO rather than what they wear: a
        // blend this pass owns no state for, which is anything but the
        // straight (srcAlpha, invSrcAlpha) pair. ⚠ COUNTED APART FROM
        // effectSkipped for the reason scopeFiltered is counted apart from
        // nameFiltered: "the shape was an effect shader" and "the shape
        // blended additively" are different facts with different fixes, and
        // a field log that merged them could not tell which rule ate a card.
        std::uint32_t blendSkipped{ 0 };
        // FX planes caught by NAME, which is all some of them have: a glow
        // plane routinely carries no alpha property at all, so no shader
        // class, blend mode or alpha flag can see it. Counted apart again,
        // for the same reason: "we knew the shape was FX because of what it
        // is called" is a weaker claim than the other two and a field log
        // should be able to say when a card leaned on it.
        std::uint32_t fxNameSkipped{ 0 };
        std::uint32_t nameFiltered{ 0 };   // helper tokens, embedded bodies, slot-gated hands/feet
        std::uint32_t faceFiltered{ 0 };   // kFace / kFaceGenRGBTint shader flags
        std::uint32_t swapApplied{ 0 };    // texture swaps applied (OS-192)
        std::uint32_t swapNoDiffuse{ 0 };  // swap promised a diffuse, none resolved
        // Hidden by an authored per-path scope (PreviewScopes.h, OS-191).
        // ⚠ COUNTED APART FROM nameFiltered ON PURPOSE, which is the spec's
        // own rule that "the NIF hid it" and "we hid it" must stay
        // distinguishable: a field log has to separate a built-in name rule
        // from somebody's fixup file, because the two have different fixes.
        std::uint32_t scopeFiltered{ 0 };
        // The bind pose (SkinBindPose.h). ⚠ THE PAIR IS THE READING, not either
        // number alone: corrected says a shape was standing somewhere its vertex
        // buffer put it and the engine never would, posed says the bones
        // disagreed and it was left exactly as it was. A card whose armour is
        // still misplaced with posed=1 is a different bug from one with both at
        // zero, and only the pair tells them apart.
        std::uint32_t bindCorrected{ 0 };
        std::uint32_t bindPosed{ 0 };
        // Would have been corrected, but the head path had already moved this
        // root and the two must not both fire. See the exclusion note in
        // MeshExtractor.cpp beside the head-align block.
        std::uint32_t bindDeferred{ 0 };
        // Legacy NiGeometry leaves the walk found, and how many of them were
        // actually drawn from their own CPU arrays (the fourth source path).
        // ⚠ THE PAIR IS THE READING. Before OS-237 these shapes were dropped
        // before any filter ran and an armour card came back as the mannequin
        // alone with a clean log; legacyGeometry high beside legacyPath at zero
        // is that failure returning, and it is invisible in every other counter.
        std::uint32_t legacyGeometry{ 0 };
        std::uint32_t legacyPath{ 0 };
        // Legacy shapes this pass could NOT draw. ⚠ IT DOUBLES AS THE PROBE'S
        // CAP: the per-shape report is written only while this is small, and
        // the stats are fresh per card, so a file of nothing but strips names
        // its first few and then goes quiet.
        std::uint32_t legacyDeclined{ 0 };
        // The pose's gating measurement (OS-204 phase 2), and it is ANSWERED.
        // ⚠ How the blend weights and indices are packed inside VA_SKINNING is
        // the one fact the skinning cannot be written without, and this file's
        // rule is that a width is measured rather than assumed: believing
        // VF_FULLPREC about the position width shipped garbage geometry with a
        // clean log twice.
        //
        // THE ANSWER, measured 2026-08-10 over 84364 NIFs and 137368 skin
        // partitions across 16 distinct descriptors on this load order: 12
        // bytes, every time, static and dynamic alike. Four half-float weights
        // at skinOffset..+7, then four uint8 bone indices at +8..+11. The
        // weights are normalised but not exactly (0.99976 to 1.00018
        // measured), so phase 2 renormalises rather than trusting the sum.
        //
        // ⚠⚠ TWO TRAPS PHASE 2 MUST TAKE FROM HERE RATHER THAN FROM THE LOG:
        //  - PRESENCE IS VF_SKINNED, NEVER bonesPerVertex. 39 hair cards
        //    declare numWeightsPerVertex = 0 in the partition header while
        //    VF_SKINNED is set and all 12 blend bytes are real and normalised
        //    (measured on FairytaleHL.nif: 0.8486 / 0.1514, indices 0 and 1).
        //    Gating on bonesPerVertex skips them or divides by zero.
        //  - A ZERO HERE NOW MEANS TWO THINGS. It still means no skin
        //    partition was seen, and it also means a partition whose
        //    VA_SKINNING flag is clear, because AttributeSpan answers 0 for an
        //    absent attribute instead of measuring its noise.
        std::uint32_t skinSpan{ 0 };        // bytes VA_SKINNING occupies
        std::uint32_t bonesPerVertex{ 0 };  // the partition's own count
        std::uint32_t skinBones{ 0 };       // bones the whole skin carries
        // Effect-shader geometry KEPT by the last-resort retry, counted apart
        // from effectSkipped for the reason scopeFiltered is counted apart
        // from nameFiltered: "we skipped it" and "we kept it because there was
        // nothing else" have different fixes and a field log has to tell them
        // apart. Nonzero on a card means that card is FX and nothing but.
        std::uint32_t effectKept{ 0 };
        // Roots moved onto the mannequin's head by the skeleton alignment.
        // Nonzero names a NIF authored against a different head, and the
        // preview.head-align line says by how much and against what.
        std::uint32_t aligned{ 0 };
    };

    // ⚠ The retry's decision is PreviewFilter::ShouldRetryKeepingEffects, not
    // here: this header is plugin-target only, and a rule nothing compiles is
    // a rule nothing pins.

    // a_modelPath and a_scopes carry the per-path fixups (OS-191). The path
    // is passed in rather than recovered from the root: only the caller knows
    // which NIF it loaded, and a scope keys on the MODEL path because there is
    // no mod identity at runtime (the VFS flattens every mod into one Data).
    // A null scope set, or one no scope covers, leaves behaviour exactly as it
    // was before scopes existed.
    //
    // Extract every visible geometry under a_root, honouring the NIF's own
    // cull flags down the tree and the scene filters (PreviewFilter.h).
    // a_slotMask gates the embedded hands and feet meshes: they draw only
    // when the item occupies those slots (weapons pass 0, which always
    // hides them). a_headPartScene turns the face-flag filter OFF: a
    // head-part scene IS face geometry (brows ride kFaceGenRGBTint,
    // NpcHair.cpp), so the filter that keeps FaceGen scaffolding out of an
    // armour scene would blank these cards. a_swaps is THIS root's texture
    // swap list or null (OS-192): entries match the collected candidate
    // index, which is the engine's own running geometry counter, and the
    // swap overrides the extracted meshes' diffuse SRVs. ⚠ NO engine
    // material is ever written: materials are POOLED and live-shared, and
    // writing one nulled a live eye material's env textures and crashed
    // the world pass (field 2026-08-09; the whole account sits on
    // LoadSwapDiffuse in the cpp). APPENDS to a_out, so a multi-NIF scene
    // accumulates across calls; false only when THIS root gave nothing
    // renderable. Render thread only: the staging copies touch the
    // immediate context.
    [[nodiscard]] bool Extract(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                               RE::NiAVObject* a_root, std::vector<RenderMesh>& a_out,
                               ExtractStats& a_stats, std::uint32_t a_slotMask,
                               bool a_headPartScene,
                               const std::vector<PreviewGrid::TextureSwapEntry>* a_swaps,
                               std::string_view                                  a_modelPath,
                               const PreviewScopes::ScopeSet*                    a_scopes,
                               bool a_mannequin = false, bool a_mannequinHead = false,
                               bool a_mannequinFeet = false,
                               // Align this root onto that head bone, when both
                               // it and this root carry one (hair only).
                               const NodeAnchor* a_alignTo = nullptr,
                               // Report this root's own head bone out.
                               NodeAnchor* a_anchorOut = nullptr,
                               // The last-resort retry (ShouldRetryKeepingEffects
                               // above). Defaulted off so every existing caller
                               // and every first pass behaves exactly as before.
                               bool a_keepEffect = false,
                               // This root IS a body being photographed, so it
                               // takes the mannequin's exemption from the
                               // garment filters. See IsBodySubjectKind.
                               bool a_bodySubject = false,
                               // One entry per GEOMETRY the slider set builds,
                               // or empty for a scene that has no morphs.
                               //
                               // ⚠⚠ A BODY NIF IS NOT ONE SHAPE AND ONE FIELD
                               // WILL NOT DO. A .osd run is indexed by the
                               // vertex array of the shape it was measured
                               // against, so the body's run is meaningless on
                               // every other geometry in the same file. This
                               // load order's male body ships VirtualArms,
                               // VirtualBelly, VirtualBreasts and VirtualButt
                               // beside it, physics colliders of a few hundred
                               // vertices each against a 14597 vertex body,
                               // and 3BA ships 3BA_Vagina (1905) and 3BA_Anus
                               // (201) beside its 18436 vertex body. Handing
                               // all of them the body's field shreds every one
                               // that is not the body (field 2026-08-10 for
                               // the colliders, code-read for 3BA's pair).
                               //
                               // A geometry the list does not name is DROPPED,
                               // which is how the physics rig stays out of a
                               // body card. An EMPTY list is "no opinion" and
                               // keeps everything, which is what the head,
                               // hands, feet and every armour scene get.
                               std::span<const ShapeMorph> a_morphs = {},
                               // A body subject that keeps its own diffuse
                               // (a skin card, OS-212). The body's exemption
                               // from the garment filters and its grey figure
                               // were one flag; this splits the second half
                               // off. Ignored unless a_bodySubject or
                               // a_mannequin is set. See DrawsGreyBody.
                               bool a_texturedBody = false,
                               // ⚠⚠ THE RESCUE IS TIERED, AND THIS IS ITS
                               // FIRST RUNG. The skips are not equally
                               // trustworthy: an effect shader and an FX NAME
                               // are strong signals, and an unreproducible
                               // blend is the weakest, because whether real
                               // geometry carries one is entirely up to the
                               // author. Vigilant's Umaril arrow proves it:
                               // every one of its eight arrow shapes is 0x100D,
                               // blend on, srcAlpha to ONE, test off, exactly
                               // like a glow plane. Skipping them emptied the
                               // scene, the old all-or-nothing rescue put
                               // EVERYTHING back, and the card came back with
                               // the slab (field 2026-08-21).
                               //
                               // So an empty scene is retried keeping the
                               // blend-skipped geometry FIRST, still dropping
                               // effect shaders and FX names. Only if that is
                               // also empty does a_keepEffect keep the lot.
                               bool a_keepBlend = false);

}  // namespace OS::MeshExtractor
