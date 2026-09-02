#pragma once

// The preview scene's name rules, as pure string arithmetic: which geometry
// inside an armour NIF is the ARMOUR and which is scaffolding. Kept out of
// MeshExtractor for PreviewGrid.h's reason: no test compiles the engine
// side, so every decidable rule lives here and is pinned by test.
//
// Inputs are already folded (PreviewGrid::FoldPath): lowercase, forward
// slashes. One fold in the subsystem, applied by the caller.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace OS::PreviewFilter {

    // The spec's census-derived helper list, copied whole. Matched against
    // the FULL name path, so helpers under helper parents vanish with their
    // parent. The known over-match (a real piece whose name contains a
    // token) is accepted, and the census debug line in MeshExtractor names
    // every drop, which is the audit trail for tuning this list.
    inline constexpr std::array<std::string_view, 24> kHelperTokens{
        "bhk", "bounding box", "boundingbox", "bsbound", "camera", "capsule",
        "collision", "collider", "constraint", "dummy", "editor marker",
        "editormarker", "helper", "inventory marker", "inventorymarker",
        "invmarker", "multibound", "occlusion", "portal", "ragdoll",
        "room marker", "roommarker", "virtual ground", "virtualground"
    };

    [[nodiscard]] inline bool PathHasHelperToken(std::string_view a_foldedPath) {
        for (const auto token : kHelperTokens) {
            if (a_foldedPath.find(token) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    // CBBE and 3BA outfit NIFs ship a full body under these exact leaf
    // names; drawn, it z-fights the garment it sits inside. Exact leaves,
    // never substrings: "cbbe hands" is a GARMENT piece.
    inline constexpr std::array<std::string_view, 8> kEmbeddedBodyLeaves{
        "3ba", "3ba_anus", "3ba_vagina", "cbbe", "virtual", "virtualbody",
        "virtualhead", "virtualtop"
    };

    [[nodiscard]] inline bool IsEmbeddedBodyLeaf(std::string_view a_foldedLeaf) {
        for (const auto leaf : kEmbeddedBodyLeaves) {
            if (a_foldedLeaf == leaf) {
                return true;
            }
        }
        return false;
    }

    // ⚠⚠ A BODY CARD DRAWS THE BODY, NOT THE PHYSICS RIG BESIDE IT. This load
    // order's male body ships VirtualArms, VirtualBelly, VirtualBreasts and
    // VirtualButt in the same file as the body: collision proxies of a few
    // hundred vertices each that the game never renders.
    //
    // ⚠ A PREFIX RULE RATHER THAN THE EXACT LEAVES IN kEmbeddedBodyLeaves
    // ABOVE, AND FOR A DIFFERENT REASON. That list answers "is this a body
    // double hiding inside a GARMENT", and it is exempted on a body card
    // because a body card's whole subject is a body. This one answers "is this
    // a collider", which stays true on a body card. The two questions used to
    // share an answer and could not both be right.
    //
    // The proxies cannot be excluded by simply not being in the slider set's
    // shape list, because some sets DECLARE them: `HIMBO Body - SOS Phys SMP
    // Col` lists VirtualArms through VirtualLegs beside `HIMBO - Body`, so a
    // set-driven whitelist alone would draw them.
    [[nodiscard]] inline bool IsPhysicsProxyLeaf(std::string_view a_foldedLeaf) {
        return a_foldedLeaf.starts_with("virtual");
    }

    inline constexpr std::size_t kNoShapeMorph = static_cast<std::size_t>(-1);

    struct ShapeMorphChoice {
        std::size_t index{ kNoShapeMorph };  // which shape's field applies
        bool        drop{ false };           // this geometry is not the subject
    };

    // Which of a body set's per-shape morph fields belongs to this geometry.
    //
    // ⚠⚠ A BODY NIF IS NOT ONE SHAPE, AND ONE FIELD FOR ALL OF THEM IS THE
    // DEFECT THIS EXISTS TO CLOSE. A .osd delta is indexed by ITS OWN shape's
    // vertex array, so the body's field landing on 3BA_Vagina (1905 vertices)
    // or 3BA_Anus (201) displaces them by strangers' deltas. The slider set
    // already says which shapes it builds and carries a separate <Data> run
    // per shape, so each geometry gets the field that was measured against it.
    //
    // ⚠ AN EMPTY LIST MEANS "NO OPINION", NOT "DROP EVERYTHING". The head,
    // hands and feet arrive as their own roots with no shape list, and so does
    // every armour and weapon scene. Those keep every geometry and take no
    // morph, exactly as before this existed.
    [[nodiscard]] inline ShapeMorphChoice ChooseShapeMorph(
        std::span<const std::string_view> a_foldedShapes,
        std::string_view                  a_foldedLeaf) {
        if (a_foldedShapes.empty()) {
            return {};
        }
        if (IsPhysicsProxyLeaf(a_foldedLeaf)) {
            return { kNoShapeMorph, true };
        }
        for (std::size_t i = 0; i < a_foldedShapes.size(); ++i) {
            if (a_foldedShapes[i] == a_foldedLeaf) {
                return { i, false };
            }
        }
        return { kNoShapeMorph, true };
    }

    // Hands and feet meshes embedded in a garment NIF draw only when the
    // item actually occupies that slot. Exact leaves for the same reason as
    // the body list ("hands" must never catch "handles"); the bits follow
    // Skyrim's slot-minus-30 convention.
    inline constexpr std::uint32_t kHandsSlotBit = 1u << 3;  // biped slot 33
    inline constexpr std::uint32_t kFeetSlotBit  = 1u << 7;  // biped slot 37

    // The torso's slot, and it is here to EXCLUDE rather than to hide. A body
    // card photographs a slider set's own body, so the skin's torso addon is
    // the one mannequin part it must never compose: two near-identical bodies
    // in one scene z-fight the whole card. The hands, feet and head are what
    // that body is missing, which is what the card wants.
    inline constexpr std::uint32_t kBodySlotBit = 1u << 2;  // biped slot 32

    // The head's slot, for the mannequin rather than for a hide rule. ⚠ Biped
    // slot 30 does not hide a head PART, it hides the whole head node: engine
    // 24220 reads exactly two bits of the worn mask, headObject and hairObject,
    // and ORs kHidden onto the head. That is why full-face pieces take 30 and
    // ordinary open helmets do not. So an item on 30 replaces the face, and a
    // mannequin that kept its head under one would be drawing something the
    // game never draws.
    inline constexpr std::uint32_t kHeadSlotBit = 1u << 0;  // biped slot 30

    // Generic scalp caps leave a hair scene by NIF PATH, at resolution
    // rather than per geometry: the whole file is the scaffolding. They are
    // fitted to a head the card never draws and hang misaligned under the
    // hair (field 2026-08-09); the per-hair "<name>hl" hairline pieces are
    // authored against their own hair and stay. Folded input, the caller
    // folds, same as every rule here.
    [[nodiscard]] inline bool IsScalpPath(std::string_view a_foldedPath) {
        return a_foldedPath.find("scalp") != std::string_view::npos;
    }

    // The shield's slot, for the POSE rule rather than a hide rule: a shield
    // lies flat along the forearm in bind pose, so the upright framing worn
    // gear uses shows the disc edge-on (field 2026-08-09). A scene whose
    // only slot is the shield keeps the weapons' diagonal instead.
    inline constexpr std::uint32_t kShieldSlotBit = 1u << 9;  // biped slot 39

    [[nodiscard]] inline bool IsHandsLeaf(std::string_view a_foldedLeaf) {
        return a_foldedLeaf == "hands";
    }
    [[nodiscard]] inline bool IsFeetLeaf(std::string_view a_foldedLeaf) {
        return a_foldedLeaf == "feet";
    }

    // Whether a scene that extracted nothing should be tried again keeping the
    // effect-shader geometry the first pass threw away.
    //
    // Effect-shader geometry is FX and not the item, which is why it is
    // skipped: a glow plane rendered through a pass with no additive path came
    // back as an opaque cream ellipse over Dawnbreaker's blade. But some items
    // are authored ENTIRELY that way. Vanilla's spectral draugr weapons are a
    // single BSTriShape with a BSEffectShaderProperty and no lighting property
    // anywhere in the file, so the skip ate the item and the card failed with
    // a perfectly correct key. Four vanilla weapons sit in that state.
    //
    // ⚠⚠ BOTH TERMS ARE THE SAFETY ARGUMENT, NOT A TIDY GUARD. The retry may
    // only fire where the first pass ALREADY decided to fail, so no card that
    // renders today can change; and it may only fire where FX was actually
    // skipped, so an empty scene with some other cause is not re-walked for
    // nothing. A scene carrying a mannequin can never reach it either, because
    // the grey body is always geometry.
    [[nodiscard]] inline bool ShouldRetryKeepingEffects(std::size_t   a_meshCount,
                                                        std::uint32_t a_effectSkipped,
                                                        std::uint32_t a_blendSkipped   = 0,
                                                        std::uint32_t a_fxNameSkipped = 0) {
        return a_meshCount == 0 &&
               (a_effectSkipped != 0 || a_blendSkipped != 0 || a_fxNameSkipped != 0);
    }

    // ---- blend modes the thumbnail pass cannot reproduce --------------------

    // `NiAlphaProperty::AlphaFunction`, as plain numbers so this stays engine
    // free and testable. The order is the NIF format's own.
    inline constexpr int kBlendOne         = 0;
    inline constexpr int kBlendSrcAlpha    = 6;
    inline constexpr int kBlendInvSrcAlpha = 7;

    // Whether a geometry's alpha blend is one the preview pass has no state
    // for, which makes it an overlay to skip rather than an item to draw.
    //
    // The pass owns exactly two blend states, opaque and the straight
    // (srcAlpha, invSrcAlpha) pair. Anything else is authored to composite
    // against a scene this pass does not have, and drawing it through the
    // standard state does not approximate it: it inverts it. Additive
    // (srcAlpha, ONE) art is black where the game shows nothing, so the
    // standard blend paints the transparent part of the plane as a solid
    // slab.
    //
    // ⚠⚠ THIS USED TO BE HEAD PARTS ONLY AND THE FIELD FALSIFIED THAT. The
    // rule was written for the UBE eye "outer" layer and fenced off with the
    // claim that a gear scene's glow planes all fall to the effect-shader skip
    // instead. Bound Arrow does not: its `FlamesMesh02` is a
    // BSLightingShaderProperty of type EnvMap carrying alpha flags 0x100D,
    // which is (srcAlpha, ONE), so nothing skipped it and the card came back
    // as a violet slab with the arrow buried in it (user 2026-08-21). A glow
    // plane is FX whatever shader class it happens to wear, and the shader
    // class was never the property that mattered.
    //
    // ⚠ ALPHA TEST ALONE IS NOT THIS. A cutout leaf or a hair card tests
    // alpha with blending off, and the pass draws those correctly through the
    // opaque state. Only BLENDING is asked about.
    [[nodiscard]] inline constexpr bool IsUnreproducibleBlend(bool a_blending, int a_src,
                                                              int a_dst) {
        return a_blending && (a_src != kBlendSrcAlpha || a_dst != kBlendInvSrcAlpha);
    }

    // Whether a geometry belongs in the pass's ALPHA draw rather than its
    // opaque one.
    //
    // ⚠⚠ AN UNREPRODUCIBLE BLEND WITH AN ALPHA TEST IS A CUTOUT, NOT AN
    // OVERLAY, AND THE FIELD SHOWED WHAT THE OTHER READING COSTS. This used to
    // be "blending is on", full stop. ENB Light re-authors bound weapon
    // geometry as blend on, srcAlpha to ONE, alpha test GREATER 128, so the
    // Bound Arrow's own six arrows went into the alpha pass, which tests depth
    // and does not write it, and the card came back as translucent shapes
    // ghosting through each other in the dark (user 2026-08-21, third round).
    //
    // On such a shape the TEST is what does the shaping and the exotic blend
    // is what the pass cannot honour anyway, so drawing it opaque with its
    // cutoff intact is strictly closer to the game than blending it. A shape
    // whose blend the pass DOES own still goes to the alpha pass, and an
    // exotic blend with no test really is an overlay and is left where it was.
    [[nodiscard]] inline constexpr bool DrawsInAlphaPass(bool a_blending, bool a_testing,
                                                         int a_src, int a_dst) {
        if (!a_blending) {
            return false;
        }
        return !(IsUnreproducibleBlend(a_blending, a_src, a_dst) && a_testing);
    }

    // An overlay this pass cannot draw: an unreproducible blend with NO alpha
    // test to shape it.
    //
    // ⚠⚠ THE `!a_testing` IS THE WHOLE GUARD AND ITS ABSENCE COST A ROUND. The
    // scene-wide skip shipped once without it and ate the Bound Arrow's own six
    // arrows, which ENB Light authors as srcAlpha to ONE with alpha test
    // GREATER 128. A TEST means the shape is a cutout and real; no test means
    // nothing shapes it but the blend, and the blend is one the pass has no
    // state for. Measured on the FX planes of four arrow families: bound
    // weapons' `FlamesMesh01`, the Creation Club magic arrows' `ArrowQuiver:2`,
    // both 0x100D, blend on and test off.
    [[nodiscard]] inline constexpr bool IsOverlayBlend(bool a_blending, bool a_testing,
                                                       int a_src, int a_dst) {
        return !a_testing && IsUnreproducibleBlend(a_blending, a_src, a_dst);
    }

    // ---- FX geometry named as FX -------------------------------------------

    // ⚠⚠ THE OTHER HALF, AND NO PROPERTY CAN SEE IT. A glow plane routinely
    // carries NO NiAlphaProperty at all, which means it draws fully opaque and
    // there is nothing on it to judge: not a shader class, not a blend mode,
    // not an alpha flag. Bound Arrow's `FlamesMesh02`, Darkend's torment arrow
    // (the same shape again) and Ordinator's `BlurMeshA` are all exactly that,
    // and all three photographed as a hard-edged coloured slab over the item.
    //
    // So they are caught by NAME, which is what they have. These are FX
    // authoring conventions rather than coincidences: Bethesda's own effect
    // meshes use them and mod authors copy the files. Censused with
    // tools/nif_shape_alpha.py over the arrow families the field reported:
    // bound weapons (FlamesMesh, FlamesHit, RefractMesh, RefrectHit),
    // Ordinator's trick arrows (BlurMesh), the Creation Club magic arrows
    // (QuiverFX, FXMesh, trailShort) and ENB Light's emitters (ENBLight).
    //
    // ⚠ COMPOUND WORDS ONLY, never a bare "fx" or "trail". Matched against the
    // FULL folded name path, like kHelperTokens, and a scene these empty out
    // is retried keeping them, so an item authored entirely as FX still draws.
    inline constexpr std::array<std::string_view, 9> kFxTokens{
        "flamesmesh", "flameshit", "blurmesh",  "quiverfx", "fxmesh",
        "refractmesh", "refrecthit", "enblight", "trailshort",
    };

    [[nodiscard]] inline bool IsFxName(std::string_view a_foldedNamePath) {
        for (const auto token : kFxTokens) {
            if (a_foldedNamePath.find(token) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    // The bone node a hair NIF and a head mesh both carry, folded. A hair is
    // authored parented to the skull, so where its skeleton puts this node is
    // a claim about where the head is.
    inline constexpr std::string_view kHeadBoneName = "npc head";

    [[nodiscard]] inline bool IsHeadBoneName(std::string_view a_foldedName) {
        return a_foldedName.rfind(kHeadBoneName, 0) == 0;
    }

    // ⚠ HOW FAR APART TWO SKELETONS MUST BE BEFORE A SCENE IS MOVED. Most
    // hairs are authored on the standard skeleton and their delta is exactly
    // zero, so this only has to reject float noise; setting it loose would
    // move the whole catalog and setting it tight would move nothing. The
    // three the field reported are 4.16 units out, which is not a close call.
    inline constexpr float kAnchorEpsilon = 0.05f;

    [[nodiscard]] inline bool ShouldAlignToHead(float a_dx, float a_dy, float a_dz) {
        const float d2 = a_dx * a_dx + a_dy * a_dy + a_dz * a_dz;
        return d2 > kAnchorEpsilon * kAnchorEpsilon;
    }

}  // namespace OS::PreviewFilter
