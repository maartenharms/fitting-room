#include "OutfitDye.h"

#include "ApparelPreviewSignal.h"  // skip the slots a hover preview owns (OS-145)
#include "DyeFlash.h"              // the stripe flash's lifetime rules (OS-144)
#include "DyeGate.h"
#include "DyeRamp.h"      // the two-stop ramp, and which target it lands on
#include "DyeStrength.h"  // how much of the chosen colour lands
#include "DyeKey.h"       // IsEnginePlaceholder, for the unusable-diffuse probe
#include "DyeTexture.h"   // OS-139 rung 3: the tinted-diffuse cache
#include "PbrPearl.h"     // a pearl's writes on Community Shaders' PBR material
#include "MaterialReading.h"  // Describe, for the eye pass's before-and-after
#include "HeadPart.h"     // DiscoveredSlots: the slots a mod invented
#include "NpcHair.h"      // AppliedTo, AttachedRootNames: a follower's hair worn through us
#include "NpcHairNames.h" // EdidOfOwnRoot: the part behind one of our renamed roots
#include "StyleRef.h"     // Make: a part becomes the key its colour is stored under
#include "RequipDiff.h"      // RequipMayPaint, which shapes the flourish may light
#include "RequipFlourish.h"  // kPeakEmissiveMult, the top of the requip ramp
#include "OutfitSession.h"
#include "Overlay1P.h"         // OS-241: build the first person clones skee never makes
#include "OverlayReconcile.h"  // LogOverlayCensus at the chain's settled point
#include "Settings.h"
#include "StyleCatalog.h"  // ClassOfWeaponForm, for the weapon walk

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>  // strtoul, for the eye spike's hex colour
#include <exception>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OS::OutfitDye {

    namespace {
        // The overlay's identity is 0.5, so a byte maps straight to 0..1 and
        // byte 128 is the neutral dye. This is NOT HairColor's kTintScale,
        // which is 2/255 because the hair shader multiplies and its identity is
        // 1.0. Both make byte 128 the neutral, by different arithmetic.
        constexpr float kTintScale = 1.0f / 255.0f;

        // Outfit's dye array is indexed by armour bit (editor slot minus 30),
        // which is exactly BIPED_OBJECTS 0..31. Anything at or past
        // kEditorTotal is a weapon and has no dye.
        static_assert(OS::kBitCount ==
                          static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kEditorTotal),
                      "dye bits must line up 1:1 with the armour biped objects");

        using PF = RE::BSShaderProperty::EShaderPropertyFlag8;

        // EVERY property flag the swap writes, in ONE list, so the way in and
        // the way out cannot drift apart. Everything up to kFaceGenRGBTint is
        // CLEARED by the swap, kFaceGenRGBTint is SET, and Restore reads each
        // one's old value back out of the saved 64-bit word.
        //
        // EShaderPropertyFlag8 values are BIT INDICES, not masks:
        // BSShaderProperty::SetFlags (0x14147BEE0) does 1ull << (flag & 0x3F).
        constexpr PF kSwapFlags[] = {
            PF::kEnvMap,   PF::kParallax,    PF::kParallaxOcclusion,
            PF::kGlowMap,  PF::kFace,        PF::kHairTint,
            PF::kEyeReflect, PF::kMultiLayerParallax,
            PF::kFaceGenRGBTint,
            // ⚠ BIT 42, COMMUNITY SHADERS' True PBR MARKER, AND IT IS A CRASH
            // AND NOT A COSMETIC without this entry (field 2026-08-21, CTD on
            // leaving the editor, CommunityShaders.dll TruePBR.cpp:1507).
            // CS's render-pass thunk checks the feature beside this bit, but
            // its SetupMaterial trusts the TECHNIQUE alone and casts the
            // material to BSLightingShaderMaterialPBR: a FacegenTint is 0xB0
            // bytes, the PBR texture pointers live past that, and the first
            // null it reads is an access violation. The swap must take the
            // marker down with the material; the flags snapshot in the record
            // puts it back with the original on restore, the same round trip
            // kEnvMap has always made.
            PF::kVertexLighting,
            // ⚠ THE SECOND EXIT CTD, MEASURED AT THE INSTRUCTION (field
            // 2026-08-21, pastebin iuVGmR3X): SkyrimSE.exe+14DCC12 is
            // BSLightingShader::vf4+0x902 on AE 1.6.1170, and the fault
            // sequence is `BT EAX,0xa` (technique's SOFT LIGHTING bit) then
            // `MOV RAX,[RBX+0x60]` (rimSoftLightingTexture) then
            // `MOV RAX,[RAX+0x48]` with RAX zero. Community Shaders' TruePBR
            // repurposes the classic lighting bits as PBR sub-features
            // (subsurface, coat), so a PG-patched property carries them while
            // its material's BASE soft/back slots hold nothing;
            // CopyBaseMembers copies the nulls into the FacegenTint clone and
            // the vanilla technique then dereferences one. All three
            // texture-demanding lighting bits go down with the swap; the
            // specBack slot at +0x68 is the same crash one field over, which
            // is why back and rim leave together.
            PF::kSoftLighting, PF::kRimLighting, PF::kBackLighting,
        };

        struct Swapped {
            // ⚠ WHAT WAS DONE TO THIS PROPERTY, because the two are undone
            // differently and one record type cannot describe both. A material
            // swap owns a reference and puts a material back; an emissive write
            // owns nothing and puts three values back. Reading a record of one
            // kind with the other's teardown would either leak a material or
            // DecRef one nobody IncRef'd.
            enum class Kind : std::uint8_t {
                kMaterial,  // the shipped swap and spike rung 1
                kEmissive,  // spike rung 2: the property's own emissive colour
            };
            Kind                          kind{ Kind::kMaterial };
            RE::NiPointer<RE::BSGeometry> geom;
            RE::BSLightingShaderProperty* prop{ nullptr };
            // ⚠ NULL ON AN EMISSIVE RECORD, and that is load-bearing rather than
            // untidy: RestoreOne's DecRef is already guarded on it, so a record
            // that never took a reference cannot release one.
            RE::BSShaderMaterial*         original{ nullptr };  // holds one ref
            std::uint64_t                 flags{ 0 };
            // kEmissive only. The VALUE, never the pointer: the probe measured
            // that each property owns its own NiColor, so putting the value back
            // through the same pointer restores exactly what was there.
            RE::NiColor                   origEmissive{};
            float                         origEmissiveMult{ 1.0f };
        };

        // ⚠ g_swapped IS DELIBERATELY LEAKED, the same way the spike leaks its
        // vector. A dye stays applied until something restores it, so quitting
        // to desktop with entries still in here is the EXPECTED path. A plain
        // namespace-scope container would run its destructor at
        // DLL_PROCESS_DETACH, DecRef'ing NiPointers and materials into a
        // renderer and heap that are already torn down. HairColor's statics
        // avoid this by holding nothing engine-owned; this one has to hold
        // geometry, so it never dies.
        //
        // Clear() is the ORDERLY teardown, called from Persistence's revert
        // callback while the renderer and the heap are both still up. The leak
        // above only covers process exit.
        //
        // g_shapes is NOT leaked. It holds nothing but PODs and strings, so its
        // destructor at detach touches no engine state.
        auto&                                                  g_swapped =
            *new std::unordered_map<RE::FormID, std::vector<Swapped>>();
        std::unordered_map<RE::FormID, std::vector<ShapeInfo>>  g_shapes;

        // See ArmPassDump in the header.
        std::atomic<bool> g_passDump{ false };

        // ---- the hover dye preview (spec 2026-08-09) -----------------------
        // Its own lock rather than g_lock: Set/Clear arrive from the task
        // queue while Repaint holds g_lock for the snapshot, and the two have
        // no shared data beyond this struct.
        std::mutex        g_dyePreviewLock;
        DyePreview::State g_dyePreview;
        std::atomic<bool> g_dyePreviewOn{ false };

        // The pass's one read. Inactive unless armed AND this is the actor
        // the editor is dressing, so a follower's walk never wears the
        // player's preview and vice versa.
        [[nodiscard]] DyePreview::State DyePreviewFor(RE::Actor* a_actor) {
            if (!g_dyePreviewOn.load(std::memory_order_acquire) || !a_actor) {
                return {};
            }
            std::scoped_lock l(g_dyePreviewLock);
            if (!g_dyePreview.armed || a_actor->GetFormID() != g_dyePreview.actorId) {
                return {};
            }
            return g_dyePreview;
        }

        // What one in-flight repaint chain is owed. See DyeGate.h for why a
        // count of posts is not a count of frames (OS-110): a walk is spent only
        // when real time has moved, and posts are bounded separately so a queue
        // that drains in one pass cannot spin.
        struct RetryState {
            unsigned    walksLeft{ 0 };
            unsigned    postsLeft{ 0 };
            std::size_t pending{ 1 };  // 1 so the first link always has a reason to run
            std::chrono::steady_clock::time_point lastWalk{};  // epoch: the first link walks
            std::chrono::steady_clock::time_point started{};
            unsigned    walksDone{ 0 };
            unsigned    postsDone{ 0 };
        };

        // Deferred repaint attempts still owed to an actor. An entry exists
        // EXACTLY while a task chain is in flight for that actor, so its
        // presence doubles as the "already armed" flag: one map instead of a map
        // plus a set, and no way for the two to disagree.
        //
        // Keyed on FormID like g_swapped, and unlike g_swapped that is harmless
        // here. An entry names nothing but a countdown, and the task re-resolves
        // both the actor and the outfit when it drains, so a chain that outlives
        // a save load paints whatever the NEW session says. That is why Clear()
        // deliberately leaves this map alone; see the comment there.
        std::unordered_map<RE::FormID, RetryState> g_retry;

        // ONE lock over all three maps. They are written in the same critical
        // section by the same function, so a second lock would buy nothing and
        // would create a lock-order rule where there is none today.
        //
        // ⚠ Non-recursive. No function here may call another PUBLIC function in
        // this module while holding it: Repaint's delegation to Restore happens
        // before the lock is taken and has to stay there.
        std::mutex g_lock;

        // ---- who owns biped object 9 (OS-111) -----------------------------
        //
        // Every armour bit holds armour, with exactly one exception. The engine
        // stages the OFF-HAND WEAPON in the actor race's shield slot, normally
        // 9, and a torch lands there too. SlotMask.h records the sharing at
        // PostPassArmorRestoreMask and WeaponSlots.h gives the mechanism at
        // IsOffHandWeaponBipedSlot.
        //
        // ⚠ EMPTY IS NOT FOREIGN. A null item means nothing is in the slot,
        // which is kNothingWorn and always was. Folding the two together would
        // change the answer for every character not carrying a shield.
        //
        // ⚠ A null item WITH a clone attached is treated as ours, which is the
        // one case this predicate gets deliberately wrong in the permissive
        // direction. The weapon stager writes .item synchronously before any 3D
        // exists, so a clone with no item is far more likely to be armour caught
        // mid-transition than a weapon; refusing it would leave a real shield
        // undyed for a frame on every rebuild.
        // a_item is BipedAnim::BIPOBJECT::item, which is a TESForm and not a
        // TESBoundObject. Taking the base type keeps this honest rather than
        // casting at the call site to satisfy a narrower signature.
        bool OccupantIsForeign(std::uint32_t a_bit, RE::TESForm* a_item) {
            return a_bit == kBitShield && a_item != nullptr &&
                   !a_item->Is(RE::FormType::Armor);
        }

        // What to tell the player about a slot they cannot dye. Two answers
        // rather than one because the fixes differ: a weapon becomes dyeable
        // when weapon dye ships, a torch never does.
        DyeSkipReason ForeignReasonFor(RE::TESForm* a_item) {
            if (a_item && a_item->Is(RE::FormType::Weapon)) {
                return DyeSkipReason::kOffHandWeapon;
            }
            if (a_item && a_item->Is(RE::FormType::Light)) {
                return DyeSkipReason::kTorch;
            }
            return DyeSkipReason::kUnsupported;
        }

        // properties[] sits behind the runtime-data accessor because its offset
        // differs between VR and SE/AE, which is what that indirection is for.
        RE::BSLightingShaderProperty* LightingPropOf(RE::BSGeometry* a_geom) {
            return netimmerse_cast<RE::BSLightingShaderProperty*>(
                a_geom->GetGeometryRuntimeData()
                    .shaderProperty
                    .get());
        }

        // Is a_obj still hanging off a_root? A rebuild does not null a
        // geometry's own parent, it builds a NEW partClone and unhooks the OLD
        // one higher up, so the only honest test walks all the way to the top.
        // Depth is about six nodes from a worn shape to the actor root.
        bool IsUnder(RE::NiAVObject* a_obj, const RE::NiAVObject* a_root) {
            for (auto* n = a_obj; n; n = n->parent) {
                if (n == a_root) {
                    return true;
                }
            }
            return false;
        }

        const char* FeatureName(RE::BSShaderMaterial::Feature a_feature) {
            using F = RE::BSShaderMaterial::Feature;
            switch (a_feature) {
                case F::kDefault:            return "Default";
                case F::kEnvironmentMap:     return "EnvironmentMap";
                case F::kGlowMap:            return "GlowMap";
                case F::kParallax:           return "Parallax";
                case F::kFaceGen:            return "FaceGen";
                case F::kFaceGenRGBTint:     return "FaceGenRGBTint";
                case F::kHairTint:           return "HairTint";
                case F::kParallaxOcc:        return "ParallaxOcc";
                case F::kMultilayerParallax: return "MultilayerParallax";
                case F::kEye:                return "Eye";
                default:                     return "other";
            }
        }

        // The engine half of EngineShapeFormIDs: turn " (00012E4C)[1]/
        // (00000D64) [50%]" into the armour's own name, so a dye stripe says
        // "Iron Helmet" rather than a hex string. The slot 43 tile is the case
        // that prompted this - it is a helmet's ears, and nothing on screen said
        // so.
        //
        // ⚠ FALLS BACK TO THE RAW NAME AT EVERY STEP, so this can only improve
        // a label and never replace a good one with an empty one. No form ID in
        // the name means a nif-authored name, which is already the best
        // available; a form that does not resolve, or resolves to something
        // nameless, leaves the raw string alone.
        //
        // ⚠ The FIRST resolvable form wins, not the last. The pair is parent
        // then addon, and the parent is the one carrying a name a player has
        // seen in their inventory. Two shapes from one armour therefore label
        // identically, which is correct: they ARE one armour, and that is the
        // fact the Ears tile was failing to communicate.
        //
        // Game thread, with the rest of the gather.
        std::string ReadableShapeName(const char* a_raw) {
            std::string raw = a_raw ? a_raw : "";
            std::uint32_t ids[2]{};
            const auto    n = OS::EngineShapeFormIDs(raw, ids, 2);
            for (std::size_t i = 0; i < n; ++i) {
                auto* const form = RE::TESForm::LookupByID(ids[i]);
                if (!form) {
                    continue;
                }
                if (const char* nm = form->GetName(); nm && *nm) {
                    return nm;
                }
                if (const char* ed = form->GetFormEditorID(); ed && *ed) {
                    return ed;
                }
            }
            return raw;
        }

        // The dyeable set, guarded on the FEATURE and nothing else. See the
        // header for why each excluded feature is excluded; every one of them
        // is a measurement, not a preference.
        bool IsDyeableFeature(RE::BSShaderMaterial::Feature a_feature) {
            using F = RE::BSShaderMaterial::Feature;
            if (a_feature == F::kDefault) {
                return true;
            }
            if (a_feature == F::kEnvironmentMap) {
                // ⚠ ALWAYS DYEABLE NOW, AND bDyeReflective NO LONGER GATES THE
                // PAINTER. It used to return the setting here, which made the
                // choice global: every reflective shape on the character either
                // dyed and went matte, or refused. That was written while
                // "whether the reflection survives the swap" was still an open
                // question, and it is not open any more. It does not survive,
                // and the feature-preserving tint spike tested three ways round
                // that and returned a no on all of them, so the trade is
                // permanent and real.
                //
                // A permanent trade belongs to the PIECE. A channel is only
                // painted when its colour is `set`, which happens when the
                // player clicks that stripe, and the stripe's tooltip already
                // says dyeing it costs its shine. So an explicit click is the
                // player accepting the trade on that piece, and it is honoured.
                // The setting now governs BULK instead, in
                // MapColoursToSlotSkipping: paste and apply-a-scheme leave
                // reflective channels alone while it is on, because one click
                // that dulls every piece of metal you own is not a choice
                // anybody made.
                return true;
            }
            return false;
        }

        // The user-facing half of IsDyeableFeature. Kept next to it so the two
        // cannot drift: a feature added to the dyeable set there and forgotten
        // here would show a row that says "not supported" while dyeing fine.
        DyeSkipReason SkipReasonFor(RE::BSShaderMaterial::Feature a_feature) {
            using F = RE::BSShaderMaterial::Feature;
            switch (a_feature) {
                case F::kDefault:
                    return DyeSkipReason::kNone;
                // Never a skip reason now: a reflective shape is always
                // dyeable and the editor says what that costs on the stripe
                // itself (`$FR_Dye_ReflectiveNote`) rather than refusing it.
                case F::kEnvironmentMap:
                    return DyeSkipReason::kNone;
                case F::kHairTint:
                    return DyeSkipReason::kHair;
                case F::kFaceGen:
                case F::kFaceGenRGBTint:
                    return DyeSkipReason::kCharacterColour;
                case F::kEye:
                    return DyeSkipReason::kEyes;
                case F::kGlowMap:
                    return DyeSkipReason::kGlow;
                default:
                    return DyeSkipReason::kUnsupported;
            }
        }

        // ---- the material census, TEMPORARY -----------------------------
        //
        // Delete with Settings::dyeCensus. Task 1 of
        // docs/superpowers/specs/2026-08-02-feature-preserving-tint-spike.md.
        //
        // ⚠ STRICTLY READ-ONLY. Nothing in this block writes to a material, a
        // property or a geometry. It rides the walk that already runs rather
        // than adding a second one, so a census pass costs the same traversal
        // the dye pass was going to do anyway plus a handful of loads.
        //
        // Three of the readings are not already in ShapeInfo, and each answers
        // one open question in that spec:
        //
        //   * kVertexLighting, bit 42, is what Community Shaders sets on a
        //     property it treats as True PBR. Measured out of
        //     CommunityShaders.dll on 2026-08-02:
        //     BSLightingShaderProperty_GetRenderPasses::thunk tests 1 << 42
        //     against flags at +0x38, then accepts GetFeature() of 0 or 0x13.
        //     Our swap makes the feature kFaceGenRGBTint, which is 5, so a
        //     dyed shape leaves the PBR path until it is restored. This counts
        //     how many worn shapes that actually costs, which is the one part
        //     of the question the binary cannot answer.
        //   * kSpecular, bit 0, gates whether a written specular value renders
        //     at all. It decides whether the dye finish unit has anywhere to
        //     put a sheen on a given shape, and the swap does not touch it.
        //   * VF_COLORS says whether the mesh carries vertex colour data,
        //     which is the whole gate on the spike's vertex-colour rung.
        //
        // Geometry type is recorded because a vertex write is only plausible
        // on a dynamic tri shape; a static one is GPU resident and shared
        // through the model cache, so writing it would tint that mesh for
        // every actor wearing it.
        struct Census {
            std::size_t shapes{ 0 };
            std::size_t pbr{ 0 };            // kVertexLighting
            std::size_t specular{ 0 };       // kSpecular
            std::size_t vertexColours{ 0 };  // VF_COLORS
            // The control on the line above. VF_VERTEX is 1 and every
            // renderable mesh carries it, so this must equal `shapes`. Anything
            // less means the vertexDesc read is broken and the vertexColours
            // count means nothing.
            std::size_t vertexPos{ 0 };      // VF_VERTEX
            std::size_t dynamic{ 0 };        // kDynamicTriShape
            // Indexed by BSShaderMaterial::Feature, whose highest value is
            // kMultiTexLandLODBlend at 19. Sized 20 and bounds-checked rather
            // than sized off an enumerator, because the value arrives from a
            // virtual call on a material this module does not own: Community
            // Shaders returns kDefault from its own PBR type, and nothing
            // stops another plugin returning something outside the range.
            std::array<std::size_t, 20> byFeature{};
        };

        void CensusShape(Census& a_t, RE::BSGeometry* a_geom,
                         RE::BSLightingShaderProperty* a_prop,
                         RE::BSShaderMaterial::Feature a_feature, std::uint32_t a_bit) {
            using PFlag = RE::BSShaderProperty::EShaderPropertyFlag;

            const auto& rt = a_geom->GetGeometryRuntimeData();
            // ⚠ THE WHOLE FLAG WORD, AS THE CONTROL ON THE READING BESIDE IT.
            // The first run reported vertexColours=0 on 11 of 11 shapes, and an
            // all-false column is exactly the shape a broken read makes. The
            // kSpecular column proves a true can travel through this function,
            // but it comes off the property's flags word rather than off
            // vertexDesc, so it proves nothing about this read.
            //
            // VF_VERTEX is 1 and every renderable mesh must carry it. A non
            // zero word here therefore proves the read is live, and VF_COLORS
            // at 0x20 can then be believed whichever way it lands. A zero word
            // means the reading is broken and the vertex-colour rung was struck
            // on nothing.
            const auto  desc = static_cast<std::uint32_t>(rt.vertexDesc.GetFlags());
            const bool  vc   = rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS);
            const bool  pbr  = a_prop->flags.any(PFlag::kVertexLighting);
            const bool  spec = a_prop->flags.any(PFlag::kSpecular);
            const bool  dyn  = a_geom->GetType() == RE::BSGeometry::Type::kDynamicTriShape;

            ++a_t.shapes;
            a_t.pbr += pbr ? 1 : 0;
            a_t.specular += spec ? 1 : 0;
            a_t.vertexColours += vc ? 1 : 0;
            a_t.vertexPos +=
                rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX) ? 1 : 0;
            a_t.dynamic += dyn ? 1 : 0;
            if (const auto f = static_cast<std::size_t>(a_feature); f < a_t.byFeature.size()) {
                ++a_t.byFeature[f];
            }

            // The material was already proven kLighting by the caller's guard,
            // which is what makes this cast defined. Reading the three specular
            // fields matters because a written value displaces whatever is here,
            // and because under Community Shaders these same three fields carry
            // specular level, roughness scale and subsurface colour instead.
            const auto* const mat =
                static_cast<const RE::BSLightingShaderMaterialBase*>(a_prop->material);
            // ⚠ THE DIFFUSE PATH IS HERE FOR RUNG 3 AND NOTHING ELSE. That rung
            // would replace the diffuse texture on a same-feature clone with a
            // tinted copy, which is the only approach in the spike that can
            // produce a FULL colour change with no feature change, so it is the
            // honest answer to whether the thing is possible at all. It is also
            // almost certainly too expensive to ship, and the spec is explicit
            // that "we decided it was too expensive" and "we never worked out
            // what it would cost" are different answers with only one of them
            // being a finding. The cost is dominated by the size and format of
            // these files, and nothing so far has named even one of them. This
            // is the reading that turns that into arithmetic.
            const auto* const diffuse = mat->diffuseTexture.get();
            spdlog::info("DyeCensus: slot {:2} '{}' feature={} pbr={} specular={} "
                         "vcolours={} vdesc=0x{:04X} dynamic={} geomType={} "
                         "specPower={:.3f} specScale={:.3f} specColor=({:.3f},{:.3f},{:.3f}) "
                         "diffuse='{}'",
                         a_bit, a_geom->name.c_str(), FeatureName(a_feature), pbr, spec, vc,
                         desc, dyn, static_cast<int>(a_geom->GetType().get()),
                         mat->specularPower, mat->specularColorScale,
                         mat->specularColor.red, mat->specularColor.green,
                         mat->specularColor.blue,
                         diffuse ? diffuse->name.c_str() : "<none>");
        }

        // ⚠ THE ONE PLACE THE MATERIAL SWAP HAPPENS. Everything else in this
        // module is deliberately ignorant of how the swap is performed.
        //
        // ⚠ DO NOT CALL SetupGeometry OR FinishSetupGeometry HERE. THEY WERE
        // THE SHINE. A neutral tint of (128,128,128), the exact algebraic
        // identity of the overlay, still visibly changed kDefault armour in the
        // 2026-07-31 field run, and so did a control that swapped in a
        // same-feature material with no tint written at all. Disassembly of AE
        // 1.6.1170 found it in the two calls this function used to make after
        // the swap:
        //
        //   BSLightingShaderProperty::FinishSetupGeometry (0x1414AD1B0, body at
        //   0x1414AD420) ends in, in effect,
        //       if ((prop->flags & kSpecular) && material->specularPower <= 0)
        //           material->specularPower = 1.0f;
        //   specularPower at +0x88 is the NIF's Glossiness exponent. Driving it
        //   from 0 to 1.0 opens a broad whole-surface specular lobe, which is
        //   the reported "gave it some shine" exactly. The same function also
        //   calls ReceiveValuesFromRootMaterial (0x1414B7B00), which
        //   substitutes default textures into empty slots and, under
        //   kLocalMapClear, overwrites diffuseTexture and forces
        //   textureClampMode to 3.
        //
        //   BSLightingShaderProperty::SetupGeometry (0x1414AD010) mutates the
        //   PROPERTY, one way only. It recomputes kVertexColors from the vertex
        //   layout, CLEARS kTreeAnim and kEyeReflect, can set NiAVObject
        //   kHidden when the mesh carries no texcoords or no tangents, and
        //   CLEARS kSpecular when specularColor * specularColorScale equals a
        //   runtime global. Nothing anywhere puts any of that back.
        //
        // Neither call is needed. DoClearRenderPasses (0x1414B0990) ends in
        // BSShaderProperty::ClearRenderPasses (0x14147BFA0), which frees the
        // pass list and sets lastRenderPassState (+0x34) to 0x7FFFFFFF;
        // GetRenderPasses (0x1414ADFB0) rebuilds the list and RE-PICKS THE
        // TECHNIQUE whenever that cached word mismatches. DoClearRenderPasses
        // alone is therefore what makes the new material take effect, and it is
        // the only one of the three that this module calls.
        //
        // ⚠ DO NOT ADD A HAND-COPY OF THE BASE TAIL HERE either. That was tried
        // during the spike on the theory that specularPower and
        // specularColorScale were being left at FacegenTint defaults. The
        // theory is dead: CopyBaseMembers assigns the whole tail, and the
        // engine's own CopyMembers copies exactly the same field set. See the
        // header.
        //
        // Fills a_out with what Restore needs and returns whether the swap
        // happened. The caller must keep a_out; the reference it holds on the
        // displaced material is released in Restore or in Clear, nowhere else.
        bool SwapToTintMaterial(RE::BSGeometry* a_geom, RE::BSLightingShaderProperty* a_prop,
                                const RE::NiColor& a_tint, Swapped& a_out) {
            auto* const fresh = RE::BSLightingShaderMaterialBase::CreateMaterial<
                RE::BSLightingShaderMaterialFacegenTint>();
            if (!fresh) {
                return false;
            }
            fresh->CopyBaseMembers(
                static_cast<RE::BSLightingShaderMaterialBase*>(a_prop->material));
            fresh->tintColor = a_tint;

            a_out.geom     = RE::NiPointer<RE::BSGeometry>(a_geom);
            a_out.prop     = a_prop;
            a_out.original = a_prop->material;
            // ⚠ MANDATORY, NOT DEFENSIVE, AND IT MUST PRECEDE SetMaterial.
            // SetMaterial runs the displaced material through the material
            // cache's release (0x1414F7A40), which DecRefs it and, at zero,
            // unhooks it from the cache and destroys it. An armour material
            // interned for one shape normally has a refcount of exactly 1, so
            // without this reference the "original" dies the instant we swap
            // and Restore writes through freed memory.
            a_out.original->IncRef();
            a_out.flags = a_prop->flags.underlying();

            a_prop->SetMaterial(fresh, true);
            // ⚠ SetMaterial IS NOT A STORE, SO THIS IS NOT A DOUBLE FREE.
            // BSShaderProperty::SetMaterial (0x14147BFF0) hands the material to
            // the cache's acquire (0x1414F7790), which computes a CRC over it,
            // probes a hash table, and ON A MISS does
            //     clone = mat->Create(); clone->CopyMembers(mat);
            // and interns the CLONE. Only the returned pointer is IncRef'd and
            // only it lands in prop->material. Acquire also forces its unique
            // path whenever GetFeature() == kFaceGenRGBTint, which salts the
            // CRC from a counter and therefore ALWAYS misses, so a FacegenTint
            // handed to SetMaterial is always copied from and then dropped.
            // (The `true` above is for that reason moot; it is kept because a
            // per-shape tint is genuinely meant to be unique.)
            //
            // Nothing in the engine ever releases `fresh`. We own it, and every
            // swap leaked 0xB0 bytes plus the five texture references
            // CopyBaseMembers took until this line existed. The destructor is
            // virtual (vtable slot 0), so `delete` runs the engine's own
            // deleting destructor, which drops those texture references and
            // frees through the game heap that CreateMaterial allocated from.
            delete fresh;

            // ⚠ CLEARING THE OLD FEATURE FLAGS IS NOT TIDINESS, IT IS THE
            // DIFFERENCE BETWEEN A DYE AND AN ACCESS VIOLATION. The new
            // material is a 0xB0-byte FacegenTint whose tintColor sits at
            // +0xA0. BSLightingShaderMaterialEnvmap keeps
            // NiPointer<NiSourceTexture> envTexture at that exact offset, so a
            // property still advertising kEnvMap makes the renderer read three
            // floats as a texture pointer: with red that is 0x000000003F800000,
            // unmapped. MultiLayerParallax is worse, its envmapScale is at 0xC8
            // and past the end of the allocation.
            //
            // kMultiLayerParallax is in kSwapFlags where the spike's list was
            // not. The feature guard already refuses those materials, so it
            // cannot fire today; it costs one store and closes the case where a
            // property advertises the flag over a material that does not.
            for (const auto f : kSwapFlags) {
                a_prop->SetFlags(f, f == PF::kFaceGenRGBTint);
            }
            // The pass list is CACHED on the property. Without this the engine
            // reuses last frame's list and the new permutation is never picked,
            // which is the difference between the dye rendering and nothing at
            // all happening. See the block comment above for why this is the
            // ONLY engine call here.
            a_prop->DoClearRenderPasses();
            return true;
        }

        // ---- rung 1 of the feature-preserving tint spike ---------------------
        //
        // Tint a shape WITHOUT moving GetFeature(). The whole skip list is one
        // cause: the shipped swap changes the feature, so the material stops
        // being what the renderer was told it was and whatever the old feature
        // carried stops working. Envmap, glow, parallax and PBR are four
        // symptoms of that one event, so if a shape can be tinted at its own
        // feature they come back together.
        //
        // ⚠ CLONED THROUGH VIRTUAL Create AND CopyMembers, NEVER CopyBaseMembers,
        // and the spike corrected itself on exactly this point before any code
        // was written. CopyBaseMembers copies the base subobject only. Cloning
        // an Envmap material that way leaves envTexture at 0xA0 unset, losing
        // the very reflection this rung exists to preserve, and cloning a
        // Community Shaders PBR material leaves everything from 0xA0 to 0x130 at
        // defaults including all four PBR textures. Create is vtable slot 01 and
        // CopyMembers is slot 02, every material overrides both, and this is the
        // same pair the engine's own material cache uses on a miss.
        //
        // ⚠ NO FLAG IS TOUCHED, which is the point rather than an omission. The
        // shipped swap MUST clear kEnvMap because a FacegenTint keeps tintColor
        // at 0xA0 where an Envmap material keeps envTexture, and clearing it is
        // what costs the reflection. A same-feature clone has no such collision,
        // so there is nothing to clear.
        //
        // ⚠ IT TINTS THE HIGHLIGHT, NOT THE DIFFUSE, and that is the known
        // weakness rather than a surprise. The colour shift is weaker than the
        // overlay and reads strongest on metal. Since the reflective shapes ARE
        // the metal, that may be the right place for it, which is the thing the
        // field run has to judge.
        //
        // Delete this with Settings::dyeSpikeRung when the spike returns.
        bool SwapToSameFeatureTint(RE::BSGeometry* a_geom,
                                   RE::BSLightingShaderProperty* a_prop,
                                   const RE::NiColor& a_tint, bool a_write, Swapped& a_out) {
            auto* const src = static_cast<RE::BSLightingShaderMaterialBase*>(a_prop->material);
            auto* const fresh = static_cast<RE::BSLightingShaderMaterialBase*>(src->Create());
            if (!fresh) {
                return false;
            }
            fresh->CopyMembers(src);

            // ⚠ a_write FALSE IS THE NEGATIVE CONTROL AND IT RUNS EVERYTHING
            // ELSE. The clone, the cache round trip and the render-pass
            // invalidation all still happen; only the two stores are skipped. If
            // the picture moves in this mode then the machinery is doing
            // something on its own and no colour seen in write mode can be
            // attributed to the write.
            if (a_write) {
                // specularColor at 0x38 and specularColorScale at 0x8C.
                //
                // ⚠ ON A COMMUNITY SHADERS PBR MATERIAL THESE TWO MEAN
                // SUBSURFACE COLOUR AND ROUGHNESS SCALE, which is why the caller
                // refuses a PBR shape outright rather than writing them here.
                fresh->specularColor      = a_tint;
                fresh->specularColorScale = 1.0f;
            }

            a_out.geom     = RE::NiPointer<RE::BSGeometry>(a_geom);
            a_out.prop     = a_prop;
            a_out.original = a_prop->material;
            // Mandatory and for the same reason as the shipped swap: SetMaterial
            // runs the displaced material through the cache's release, which
            // destroys it at zero, and an armour material interned for one shape
            // normally has a refcount of exactly 1.
            a_out.original->IncRef();
            a_out.flags = a_prop->flags.underlying();

            // ⚠ true, THE UNIQUE PATH, DELIBERATELY. Acquire computes a CRC over
            // the material and probes a hash table; a same-feature clone that
            // differs only in two floats could plausibly collide with the very
            // material it was cloned from, which would hand back the untinted
            // original and make this rung read as a silent no. Forcing unique
            // guarantees the values that were written are the values interned.
            a_prop->SetMaterial(fresh, true);
            // Nothing in the engine releases `fresh`; SetMaterial copies from it
            // and interns its own. The destructor is virtual, so this runs the
            // engine's deleting destructor and drops the texture references
            // CopyMembers took.
            delete fresh;

            // The pass list is cached on the property, so without this the
            // engine reuses last frame's and nothing changes on screen. This is
            // the only engine call here, and it is the one the spike established
            // is sufficient on its own.
            a_prop->DoClearRenderPasses();
            return true;
        }

        // ---- rung 3 of the feature-preserving tint spike, OS-139 -------------
        //
        // ⚠ THIS IS SwapToSameFeatureTint WITH ONE LINE CHANGED, and that is the
        // point rather than a coincidence. Rung 1 proved the clone-and-intern
        // machinery moves nothing on its own, in the field, with its own
        // negative control; it was refused only because a highlight tint is too
        // weak to read as a dye. Rung 3 reuses every part of it and writes
        // `diffuseTexture` instead of `specularColor`. Read every ⚠ on that
        // function before touching this one; each of them cost a field run.
        //
        // ⚠ THE TEXTURE MAY NOT EXIST YET AND THAT IS NORMAL. Building it needs
        // the render thread and this runs on the game thread, so a first-time
        // colour answers "not yet" through a_pending. The caller counts that as
        // pending, the deferred repaint chain re-arms, and the next link finds a
        // cache hit. See DyeTexture.h for why that also makes a dye survive a
        // rebuild for free.
        //
        // ⚠ NOTHING HERE IS UNDONE DIFFERENTLY. The record is a kMaterial like
        // rung 1's: one reference on the displaced material, put back by the
        // same RestoreOne. The tinted texture is owned by DyeTexture and outlives
        // every swap that uses it, so teardown has nothing to say about it.
        //
        // Delete with Settings::dyeSpikeRung when the spike returns.
        // a_ramp arrives with its mode DECLARED rather than effective, which is
        // the one thing about this signature worth saying out loud: only this
        // function knows the shape, so only this function knows the shape's
        // CARRIER and can resolve iridescent against it (the sweep on a cubemap,
        // nacre on a True PBR pearl carrier, flat with neither). Its stops are
        // already narrowed by strength, at the call site, beside the tint, for
        // the reason the call site records.
        // a_capPx is a ceiling THIS SHAPE needs on top of the install's, 0 for
        // none, and it reaches only the DIFFUSE. The right resolution is a
        // property of the surface: an eye is a few dozen pixels on screen and an
        // uncapped one measured 87 MiB, while the same texture on a cuirass is
        // worth keeping. The normal map, the cubemap and the mask below are
        // deliberately left alone, because nothing has measured them on an eye
        // and a cap nobody has looked at is a guess.
        // a_irisMask confines the diffuse tint to the iris, through the shape's
        // own NORMAL map: Skyrim reads a normal map's alpha as the specular
        // mask, the cornea is the shiny part of an eye, so the disc the author
        // drew to place the highlight is exactly where the iris is. MEASURED on
        // the ILV set 2026-08-13: eye01_n.dds alpha is a soft-edged disc
        // sitting over the iris in eye01.dds, covering 7.9% of the texture.
        // Only the eye path asks for this; armour keeps its whole-surface tint.
        //
        // a_maskDye says which side of that disc takes colour: a_tint inside
        // it, a_maskDye's own colour outside it (the sclera). Only read when
        // a_irisMask is set. ⚠ WHEN a_maskDye.irisSet IS FALSE, a_tint MUST BE
        // CANONICAL BLACK: the tint is in the cache key even when the shader
        // never reads it, and a varying ignored tint would fragment one sclera
        // dye into many entries. PaintEyeTint owns that contract.
        bool SwapToTintedDiffuse(RE::BSGeometry* a_geom, RE::BSLightingShaderProperty* a_prop,
                                 const RE::NiColor& a_tint, bool a_write, Swapped& a_out,
                                 bool& a_pending, RE::ActorHandle a_waiter,
                                 const DyeTexture::Ramp& a_ramp,
                                 const DyeMaterial& a_fin, std::uint8_t a_flake,
                                 // The dye's strength byte, verbatim. The ramp's
                                 // stops arrive already narrowed by it, but the
                                 // PBR pearl needs the byte itself: the fuzz
                                 // weight's floor is the one write whose size is
                                 // the strength rather than a colour.
                                 std::uint8_t a_strength,
                                 bool a_previewOnly, std::uint32_t a_capPx = 0,
                                 // ⚠ THE DEFAULT IS SOFT LIGHT, WHICH IS THE CURVE THIS
                                 // ARGUMENT HAS ALWAYS MEANT. It was spelled kOverlay
                                 // until 2026-08-13 and the enumerator was renamed, not
                                 // repointed; DyeTexture.h carries why. A caller that
                                 // wants the real Overlay now has to name it.
                                 DyeTexture::Blend a_diffuseBlend =
                                     DyeTexture::Blend::kSoftLight,
                                 bool a_repick = true, bool a_irisMask = false,
                                 const DyeTexture::MaskDye& a_maskDye = {}) {
            a_pending         = false;
            auto* const src   = static_cast<RE::BSLightingShaderMaterialBase*>(a_prop->material);
            auto* const shown = src->diffuseTexture.get();
            if (!shown) {
                // A lighting material with no diffuse. Nothing to replace, and
                // inventing one would be painting rather than dyeing.
                spdlog::warn("DyeSource: '{}' has a lighting material with NO diffuse "
                             "texture at all, so there is nothing to dye.",
                             a_geom->name.c_str());
                return false;
            }
            // ⚠⚠ THE INSTRUMENT FOR "ARMOUR NEVER DYES", 2026-08-13. MEASURED: the
            // whole armour walk sat on "diffuse not yet" for a whole session while
            // the only refusals in the log were placeholder refusals whose SLOT
            // printed as bare "|ovl", meaning the source texture's name was the
            // EMPTY STRING. IsEnginePlaceholder treats empty as a placeholder, so
            // every armour diffuse was refused at the funnel and none was ever
            // built. The same lines are in the 04:25 log on the build from before
            // blend modes existed, so it is not the blends.
            //
            // What is NOT known is why the name is empty: the meshes on disk name
            // 'textures\armor\iron\f\CuirassPlate.dds' correctly, so it is a
            // runtime state. This line names the SHAPE beside its texture, which
            // the refusal inside Acquire cannot do because it has no geometry.
            //
            // ⚠ A POINTER, NOT ONLY A NAME. An empty name on a live texture and an
            // empty name on a dead one look identical in a log, and the address is
            // what lets two passes be compared. Delete this once the cause is
            // known and the guard is corrected.
            if (DyeKey::HasNoIdentity(shown->name.c_str())) {
                spdlog::debug("DyeSource: '{}' diffuse is nameless (ptr={}), so its key "
                              "comes from the material's authored path instead.",
                              a_geom->name.c_str(), static_cast<const void*>(shown));
            }
            // ---- what the material says this shape's textures are called ----
            //
            // ⚠⚠ THE MATERIAL KNOWS WHAT THE TEXTURE WILL NOT SAY. MEASURED
            // 2026-08-13: every armour diffuse reaches Acquire resident and
            // NAMELESS, so the key collapsed to the empty string and the guard
            // refused the lot; no armour dyed between 00:11 and the fix. The
            // NIFs are innocent, naming
            // `textures\armor\iron\f\CuirassPlate.dds` in tex[0], and the
            // material still carries that path in its texture set. So the path
            // is fetched here, where the material is in hand, and handed to
            // Acquire as the identity of last resort.
            //
            // ⚠ A HINT, NOT AN OVERRIDE. Acquire prefers the texture's own name
            // whenever it has one, so every key already cached in the field is
            // byte-identical and nothing is invalidated.
            //
            // ⚠ AND IT IS NOT A PLACEHOLDER BYPASS. The engine's own default
            // can sit in a live slot while the material still names the real
            // path, which is exactly the purple eye; Acquire tests the RAW name
            // for that and only uses this to fill an empty one.
            // ⚠ ALL FIVE SLOTS, NOT ONLY THE DIFFUSE, AND THE REASON IS
            // CONTROL FLOW. The diffuse's refusal returns before the block
            // holding the other five Acquire calls, so while armour was refused
            // those five were DEAD CODE on these materials and no log could say
            // whether their textures are nameless too. Fixing the diffuse makes
            // them reachable for the first time; giving them their paths now
            // means the fix does not simply move the refusal one slot along.
            const char* diffusePath = nullptr;
            const char* normalPath  = nullptr;
            const char* envPath     = nullptr;
            const char* envMaskPath = nullptr;
            const char* glowPath    = nullptr;
            if (const auto ts = src->textureSet) {
                diffusePath = ts->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
                normalPath  = ts->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
                envPath     = ts->GetTexturePath(RE::BSTextureSet::Texture::kEnvironment);
                envMaskPath =
                    ts->GetTexturePath(RE::BSTextureSet::Texture::kEnvironmentMask);
                glowPath    = ts->GetTexturePath(RE::BSTextureSet::Texture::kGlowMap);
            }

            // ---- which CARRIER this SHAPE has, then which mode it gets -------
            //
            // ⚠⚠ THE CARRIER IS WORKED OUT BEFORE THE MODE, AND IT HAS TO BE
            // (2026-09-02). DyeRamp::EffectiveMode degrades iridescent to FLAT
            // on a shape with no carrier for a second colour, and a True PBR
            // piece is NOT reflective either: Community Shaders answers
            // kDefault for its own material and PG Patcher stripped the
            // cubemap. Asking only "reflective?" would flatten the pearl on
            // every PBR piece, which is the 2026-08-30/31 arc undone. So the
            // pearl question (same RTTI test, same flag read, on the SOURCE
            // material, as the write site below asks of the clone) is asked
            // here first, and the carrier decides the mode.
            //
            // ⚠ THE FEATURE TEST, NOT A NULL CHECK ON THE ENVMAP MATERIAL. The
            // cast below in the finish probe is unconditional and never yields
            // null, so a null test there would answer "reflective" for a plain
            // kDefault shape and ask for a cubemap that does not exist. The
            // source's own feature is the authority, exactly as it is for the
            // 0xB0 against 0xB8 material size question a few lines down, and it
            // has to be known HERE because the diffuse is acquired first.
            const bool reflective =
                src->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap;
            const bool pbrShapeHere = a_prop->flags.any(
                RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting);
            PbrPearl::Writes pearlPlan{};
            if (pbrShapeHere && Settings::GetSingleton().dyePbrPearl &&
                PbrPearl::IsTruePbrMaterial(src)) {
                const auto srcFlags = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::byte*>(src) +
                    PbrPearl::kPbrFlagsOffset);
                pearlPlan = PbrPearl::PlanFor(srcFlags, true);
            }
            const auto carrier   = DyeRamp::CarrierFor(reflective, pearlPlan.Any());
            const auto effective = DyeRamp::EffectiveMode(
                DyeRamp::ModeFromByte(a_ramp.mode), carrier);

            // The same stops under the resolved mode. Handed to whichever ONE
            // target the mode names; the other gets a flat request, which is
            // byte for byte what it asked for before this feature existed.
            //
            // ⚠ ONE TARGET, NEVER BOTH. On a reflective shape running iridescent
            // the reflection is what changes with viewing angle, so the ramp goes
            // to the cubemap and the diffuse keeps the flat tint. Ramping the
            // diffuse as well would double the colour and read as a stain rather
            // than as a sheen. A shape with no carrier under an iridescent dye
            // hands a flat request to BOTH, which is what that dye now means on
            // cloth: the primary colour through the ordinary curve.
            DyeTexture::Ramp resolved = a_ramp;
            resolved.mode             = static_cast<std::uint8_t>(effective);

            // ---- the second stop as a colour, for the envmask modes ----------
            //
            // A twotone dye's metal colour is hex2, already strength-narrowed by
            // the caller exactly as the ramp's stops are. A mask mode authored
            // without hex2 paints its own colour on both sides, the same "a dye
            // without hex2 is a dye whose hex2 equals hex" rule NarrowStop keeps
            // for the ramps.
            const RE::NiColor secondTint =
                a_ramp.secondSet
                    ? RE::NiColor{ a_ramp.r2 / 255.0f, a_ramp.g2 / 255.0f, a_ramp.b2 / 255.0f }
                    : a_tint;

            // ---- a pearl whose sweep already rides the fuzz -----------------
            //
            // ⚠⚠ FIELD, 2026-08-30 ROUND THREE: flat dyes through multiply
            // finally arrive saturated on a True PBR piece, and a nacre or
            // iridescent dye on the SAME piece still lands pale. Both halves
            // have one cause: the nacre-on-diffuse arm forces recolour, and
            // recolour keeps the SOURCE's luminance, which on a PBR base
            // colour map is uniformly bright, so the hue arrives washed out.
            // The ramp itself cannot sweep there either, since its key is that
            // same flat luminance - measured in round zero and parked.
            //
            // So on a shape whose material already carries the pearl's second
            // stop in its FUZZ (the PbrPearl block below writes it), the
            // diffuse drops out of the ramp business entirely: it takes the
            // FLAT tint through the same resolved blend a flat dye takes,
            // which is the path the field just confirmed. Stop A on the
            // albedo at full saturation, stop B on the grazing angle, one
            // writer each.
            //
            // ⚠ GATED ON A PEARL WRITE ACTUALLY LANDING: the plan read above
            // off the SOURCE material is what made the carrier kPbrPearl, and
            // the mode still has to be one that ramps the diffuse (nacre, or
            // iridescent on this carrier; a flat dye has no second stop).
            // Through 1.1.9 only a fuzz piece was spared and a coat-only piece
            // kept the nacre-recolour diffuse beside its coat write; since
            // 2026-09-15 the coat spares the diffuse too, DyeRamp.h says why.
            // A shape with neither feature is kNone above, so an iridescent
            // dye goes FLAT on it through sDyePbrBlend, where it used to take
            // nacre-recolour on a flat albedo whose luminance key collapsed
            // and showed no ramp anyway; flat multiply is the more saturated
            // answer there.
            const bool pearlRidesFuzz = DyeRamp::PearlRidesMaterial(carrier, effective);

            const DyeTexture::Ramp diffuseRamp =
                (DyeRamp::RampsDiffuse(effective) && !pearlRidesFuzz)
                    ? resolved
                    : DyeTexture::Ramp{};
            const DyeTexture::Ramp reflectionRamp =
                DyeRamp::RampsReflection(effective) ? resolved : DyeTexture::Ramp{};

            RE::NiSourceTexture* tinted = nullptr;
            if (a_write) {
                // ---- the iris mask, read off the shape's own normal map ------
                //
                // ⚠ NULL IS THE FALLBACK AND THE FALLBACK IS THE OLD BEHAVIOUR:
                // a shape with no normal map (vanilla FemaleEyesDarkElfRed
                // carries none at all) takes the dye whole, exactly as every
                // eye did before the mask existed. Whether that is good enough
                // for a vanilla eye is the field question the handoff owes; a
                // flat-alpha normal needs no branch here because the shader's
                // own flatness test falls back to the same place.
                //
                // ⚠ A PLACEHOLDER IN THE NORMAL SLOT IS HANDLED AT THE FUNNEL.
                // The eye's normal slot can hold BSShader_DefNormalMap around a
                // head rebuild exactly as the diffuse slot did on 2026-08-12,
                // and Acquire refuses it by NAME, which reports this pass
                // pending and lets the repaint chain come back once the real
                // texture resolves.
                RE::NiSourceTexture* mask = nullptr;
                if (a_irisMask) {
                    mask = src->normalTexture.get();
                    if (!mask && !a_maskDye.irisSet) {
                        // A sclera colour with no disc has nowhere to land:
                        // painting the whole eye with it would be the sclera
                        // defect inverted. Refuse the swap outright rather
                        // than building a texture that changes nothing.
                        spdlog::info("EyeLayer: '{}' has no normal map, so there is no "
                                     "iris disc to put a sclera colour outside of; "
                                     "nothing to dye.",
                                     a_geom->name.c_str());
                        return false;
                    }
                    if (!mask) {
                        spdlog::info("EyeLayer: '{}' has no normal map, so there is no "
                                     "iris mask to read; the whole eye takes the dye.",
                                     a_geom->name.c_str());
                    }
                }

                // ---- the METAL SPLIT, for the envmask dye modes (2026-09-04) --
                //
                // The shape's own reflection-strength map is the line between
                // metal and cloth, read in the engine's own order: the
                // environment mask where one is bound (its red), else the normal
                // map's alpha. DyeRamp::DiffuseTakeFor says what this shape's
                // diffuse gets and SplitFor which side of a split takes which
                // colour; both are pure and tested, so nothing is decided here,
                // only carried to the build as the two-sided request the eye
                // path already knows how to make.
                //
                // ⚠ irisSet FALSE MEANS THE REQUEST'S TINT IS CANONICAL BLACK,
                // the contract the eye path keeps: the tint is in the cache key
                // even when the shader never reads it.
                RE::NiColor         tintHere    = a_tint;
                DyeTexture::MaskDye maskDyeHere = a_maskDye;
                const char*         maskHint    = normalPath;
                if (DyeRamp::IsMaskMode(effective)) {
                    const auto modeWord = [](DyeRamp::Mode a_m) {
                        return a_m == DyeRamp::Mode::kMetal   ? "metal"
                               : a_m == DyeRamp::Mode::kCloth ? "cloth"
                                                              : "twotone";
                    };
                    RE::NiSourceTexture* strength = nullptr;
                    bool                 red      = false;
                    if (reflective) {
                        // ⚠ USABLE MEANS KEYABLE, the funnel's own test (field,
                        // 2026-09-04): a texture with a real name, or a
                        // nameless one whose texture set authors a path. The
                        // vanilla iron war axe has NEITHER for its mask slot,
                        // the engine's nameless default sits there, and a split
                        // keyed on the pointer was refused six times while the
                        // axe stayed undyed. Its normal map is nameless too but
                        // authored, so that is the map, exactly as the engine
                        // reads it with no mask bound.
                        const auto usable = [](RE::NiSourceTexture* a_tex, const char* a_path) {
                            if (!a_tex) {
                                return false;
                            }
                            const char* const name = a_tex->name.c_str();
                            if (DyeKey::IsEnginePlaceholder(name)) {
                                return false;
                            }
                            return (name && *name) || (a_path && *a_path);
                        };
                        auto* const envMaskTex =
                            static_cast<const RE::BSLightingShaderMaterialEnvmap*>(src)
                                ->envMaskTexture.get();
                        auto* const normalTex = src->normalTexture.get();
                        switch (DyeRamp::MapSourceFor(usable(envMaskTex, envMaskPath),
                                                      usable(normalTex, normalPath))) {
                            case DyeRamp::MapSource::kEnvMask:
                                strength = envMaskTex;
                                red      = true;
                                break;
                            case DyeRamp::MapSource::kNormalAlpha:
                                strength = normalTex;
                                red      = false;
                                break;
                            default:
                                break;
                        }
                    }
                    const auto take =
                        DyeRamp::DiffuseTakeFor(effective, reflective, strength != nullptr);
                    const auto colourOf = [&](DyeRamp::Take a_take) {
                        return a_take == DyeRamp::Take::kSecond ? secondTint : a_tint;
                    };
                    const auto byteOf = [](float a_v) {
                        return static_cast<std::uint8_t>(std::clamp(a_v * 255.0f + 0.5f, 0.0f, 255.0f));
                    };
                    switch (take) {
                        case DyeRamp::Take::kUntouched:
                            spdlog::info("MetalSplit: '{}' is {}, and a {} dye has nothing to "
                                         "paint there, so it is left alone.",
                                         a_geom->name.c_str(),
                                         reflective ? "all metal" : "not reflective at all",
                                         modeWord(effective));
                            return false;
                        case DyeRamp::Take::kPrimary:
                        case DyeRamp::Take::kSecond:
                            tintHere = colourOf(take);
                            spdlog::info("MetalSplit: '{}' is {} with no map to split it, so "
                                         "the {} dye paints it whole with its {} colour.",
                                         a_geom->name.c_str(),
                                         reflective ? "all metal" : "not reflective",
                                         modeWord(effective),
                                         take == DyeRamp::Take::kSecond ? "second" : "first");
                            break;
                        case DyeRamp::Take::kSplit: {
                            const auto split = DyeRamp::SplitFor(effective);
                            mask             = strength;
                            maskHint         = red ? envMaskPath : normalPath;
                            maskDyeHere      = DyeTexture::MaskDye{};
                            maskDyeHere.region  = true;
                            maskDyeHere.red     = red;
                            maskDyeHere.cut     = a_ramp.cut;
                            maskDyeHere.irisSet = split.inside != DyeRamp::Take::kUntouched;
                            tintHere = maskDyeHere.irisSet ? colourOf(split.inside)
                                                           : RE::NiColor{ 0.0f, 0.0f, 0.0f };
                            maskDyeHere.scleraSet = split.outside != DyeRamp::Take::kUntouched;
                            if (maskDyeHere.scleraSet) {
                                const auto c  = colourOf(split.outside);
                                maskDyeHere.r = byteOf(c.red);
                                maskDyeHere.g = byteOf(c.green);
                                maskDyeHere.b = byteOf(c.blue);
                            }
                            spdlog::info("MetalSplit: '{}' splits by its {} '{}' under a {} "
                                         "dye: the metal {}, the rest {}.",
                                         a_geom->name.c_str(),
                                         red ? "environment mask" : "normal map's alpha",
                                         (strength->name.c_str() && *strength->name.c_str())
                                             ? strength->name.c_str()
                                             : (maskHint ? maskHint : "(nameless)"),
                                         modeWord(effective),
                                         maskDyeHere.irisSet ? "takes colour" : "is left alone",
                                         maskDyeHere.scleraSet ? "takes colour" : "is left alone");
                            break;
                        }
                    }
                }
                // ⚠ THE BLEND IS THE CALLER'S FOR ONE SURFACE ONLY, and armour
                // never passes it. The overlay is what makes a dyed garment look
                // dyed rather than painted, and it is the WRONG arithmetic for
                // an eye: it keeps the source's hue, so amber on a green iris
                // measured #9ABF13, still green. An eye colour picker has to
                // give the colour it was asked for.
                // ⚠⚠ A NACRE RAMP ON THE DIFFUSE TAKES RECOLOUR, NOT THE
                // INSTALL'S CURVE, AND THE FIELD IS WHY (2026-08-30: "if i use
                // a pearlescent dye on cloth it goes pure white"). The
                // reflection arm has always paired this mode with recolour -
                // every cubemap build in the log reads `m2:...` beside `rec` -
                // and the diffuse arm was left on whatever sDyeBlend said. On
                // this install that is overlay, whose upper branch is
                // 1 - 2*(1-T)*(1-D): a pearl's bright primary over a bright
                // cloth diffuse drives both terms toward zero and the result to
                // WHITE. The garment does not go pale, it disappears.
                //
                // Recolour cannot do that. It takes the tint's hue and
                // saturation and carries the source's luminance across as the
                // output's value, so the output is bounded by what the texture
                // already was, and the ramp's colours arrive as themselves
                // instead of as a mix with the source's own hue - which is the
                // whole point of a two-stop pearl.
                //
                // ⚠ IT OVERRIDES AN EXPLICIT PER-DYE BLEND TOO, and that is a
                // narrowing rather than an oversight: by the time the choice
                // reaches this function it has already been resolved against the
                // fallback and the two are indistinguishable. A pearl authored
                // with a deliberate overlay is not a case anybody has, and a
                // white garment is not a look anybody chose.
                const auto diffuseBlendHere =
                    (DyeRamp::RampsDiffuse(effective) && !pearlRidesFuzz)
                        ? DyeTexture::Blend::kRecolour
                        : a_diffuseBlend;
                if (diffuseBlendHere != a_diffuseBlend) {
                    spdlog::info("DyeRamp: '{}' runs a nacre ramp on its DIFFUSE, so the "
                                 "blend is recolour rather than '{}': this mode's other "
                                 "arm has always used it, and the install's curve blows a "
                                 "bright pearl to white here.",
                                 a_geom->name.c_str(), DyeTexture::BlendName(a_diffuseBlend));
                }
                if (pearlRidesFuzz) {
                    spdlog::info(
                        "PbrPearl: '{}' sends its ramp to the {}, so the diffuse takes "
                        "the flat tint through blend '{}' like any flat dye.",
                        a_geom->name.c_str(),
                        pearlPlan.fuzz && pearlPlan.coat ? "FUZZ and COAT"
                        : pearlPlan.fuzz                 ? "FUZZ"
                                                         : "COAT",
                        DyeTexture::BlendName(a_diffuseBlend));
                }
                tinted = DyeTexture::Acquire(shown, tintHere, a_waiter, diffuseBlendHere,
                                             diffuseRamp, a_previewOnly, a_capPx, mask,
                                             maskDyeHere, diffusePath, maskHint);
                if (!tinted) {
                    // Queued, or permanently refused. Either way this pass has
                    // nothing to swap in, and swapping in a clone with the
                    // ORIGINAL diffuse would look like a working dye that does
                    // nothing, which is the worst of the three outcomes.
                    a_pending = true;
                    return false;
                }
            }

            auto* const fresh = static_cast<RE::BSLightingShaderMaterialBase*>(src->Create());
            if (!fresh) {
                return false;
            }
            fresh->CopyMembers(src);

            // ⚠ a_write FALSE IS THE NEGATIVE CONTROL AND IT RUNS EVERYTHING
            // ELSE. The clone, the cache round trip and the render-pass
            // invalidation all still happen; only the texture store is skipped,
            // and no build is even requested. If the picture moves in this mode
            // then the machinery is doing something on its own and no colour
            // seen in write mode can be attributed to the diffuse.
            if (a_write) {
                fresh->diffuseTexture = RE::NiPointer<RE::NiSourceTexture>(tinted);

                // ---- the FINISH, live at last (2026-08-08) -------------------
                //
                // ⚠ THE FIELDS SHIPPED FOR MONTHS WITH NO WRITER. DyeMaterial
                // was parsed, staged at swatch click and resolved by
                // EffectiveMaterial, and then nothing applied it: rung 1 wrote
                // its TINT into specularColor and rung 5 wrote no specular field
                // at all. This is the missing write, on the CLONE, which is
                // discarded whole on restore so it needs no restore code.
                //
                // The specular highlight is computed from N, V and the light, so
                // a coloured sheen is a VIEW-DEPENDENT second colour in the
                // engine's own lighting path — on cloth as well as metal, under
                // vanilla shaders and Community Shaders alike. For a pearl this
                // is stop B riding the highlights while the body carries stop A.
                //
                // ---- and the finish's PBR fork (2026-08-30) ------------------
                //
                // ⚠⚠ ON COMMUNITY SHADERS' PBR MATERIAL EVERY CLASSIC FINISH
                // FIELD MEANS SOMETHING ELSE OR NOTHING. specularColor's bytes
                // are the subsurface and coat colour (measured off the shipped
                // DLL's getters: GetSubsurfaceColor and GetCoatColor both
                // return this+0x38), and specularPower goes unread. So a PBR
                // material takes this block INSTEAD of the classic writes: the
                // dye's second stop (or its sheen) lands on the fuzz and coat
                // constants the shader already reads, for the features the
                // mesh already declares - which is what a pearlescent finish
                // physically is, a thin tinted layer colouring the
                // grazing-angle response. The installed Lighting.hlsl
                // MULTIPLIES the fuzz map's sample over the constant, so the
                // write tints exactly the pieces that already carry the map.
                //
                // ⚠⚠ GATED ON THE ALLOCATION'S OWN RTTI, NEVER THE PROPERTY
                // FLAG ALONE. The flag says what the shader THINKS, the RTTI
                // says what the BYTES are, and it is the bytes taking a raw
                // offset write. A flagged shape whose material is some other
                // type falls through to the classic finish, logged.
                //
                // ⚠ FEATURES ARE RECOLOURED, NEVER TURNED ON. A bit unset in
                // pbrFlags means the mesh does not carry the feature's
                // textures; PbrPearl.h carries why that stays a logged refusal.
                //
                // ⚠ ON THE CLONE, like every finish write, so restore is still
                // the discard it always was and nothing here needs undo code.
                bool pbrPearlMaterial = false;
                if (a_prop->flags.any(
                        RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting) &&
                    Settings::GetSingleton().dyePbrPearl) {
                    if (!PbrPearl::IsTruePbrMaterial(fresh)) {
                        const char* const rtti = PbrPearl::MsvcRttiName(fresh);
                        spdlog::info(
                            "PbrPearl: '{}' is flagged kVertexLighting but the material "
                            "allocation's RTTI is '{}', so no raw write lands and the "
                            "classic finish applies.",
                            a_geom->name.c_str(), rtti ? rtti : "(unreadable)");
                    } else {
                        pbrPearlMaterial = true;
                        auto* const bytes    = reinterpret_cast<std::byte*>(fresh);
                        const auto  pbrFlags = *reinterpret_cast<const std::uint32_t*>(
                            bytes + PbrPearl::kPbrFlagsOffset);
                        // The pearl's colour: the ramp's second stop when the
                        // dye is two-stop (already strength-narrowed by the
                        // caller), else the authored sheen. Either way it is
                        // the colour the dye wants the light to catch.
                        // ⚠ A MASK MODE IS NOT A TWO-STOP RAMP. Its second
                        // colour is the metal's, and on a True PBR piece there
                        // is no metal map read yet, so the fuzz keeps its
                        // authored sheen rather than taking hex2 (2026-09-04).
                        const bool twoStop    = effective != DyeRamp::Mode::kFlat &&
                                             !DyeRamp::IsMaskMode(effective);
                        const bool haveColour = twoStop || a_fin.sheenSet;
                        const std::uint8_t cr = twoStop ? a_ramp.r2 : a_fin.sheenR;
                        const std::uint8_t cg = twoStop ? a_ramp.g2 : a_fin.sheenG;
                        const std::uint8_t cb = twoStop ? a_ramp.b2 : a_fin.sheenB;
                        const auto  plan = PbrPearl::PlanFor(pbrFlags, haveColour);
                        const float lin[3] = { PbrPearl::SrgbToLinear(cr),
                                               PbrPearl::SrgbToLinear(cg),
                                               PbrPearl::SrgbToLinear(cb) };
                        const float linMax =
                            std::max(lin[0], std::max(lin[1], lin[2]));
                        const float energy = PbrPearl::FuzzEnergyScale(linMax);
                        if (plan.fuzz && energy <= 0.0f) {
                            // A stop this dark has no hue to give the grazing
                            // light; writing it would dim the piece's own fuzz
                            // for nothing visible in return.
                            spdlog::info(
                                "PbrPearl: '{}' second stop {:02X}{:02X}{:02X} is too "
                                "dark to carry a hue, so the fuzz keeps its shipped "
                                "colour.",
                                a_geom->name.c_str(), cr, cg, cb);
                        }
                        if (plan.fuzz && energy > 0.0f) {
                            auto* const fuzz = reinterpret_cast<float*>(
                                bytes + PbrPearl::kFuzzColorOffset);
                            auto* const weight = reinterpret_cast<float*>(
                                bytes + PbrPearl::kFuzzWeightOffset);
                            const float shipped = *weight;
                            fuzz[0]             = lin[0] * energy;
                            fuzz[1]             = lin[1] * energy;
                            fuzz[2]             = lin[2] * energy;
                            *weight = PbrPearl::FuzzWeightFor(
                                shipped, a_strength,
                                Settings::GetSingleton().dyePbrPearlSheenMax);
                            spdlog::info(
                                "PbrPearl: '{}' fuzz takes {:02X}{:02X}{:02X} at full "
                                "energy (x{:.1f}), weight {:.3f} -> {:.3f} (strength {}, "
                                "ceiling {:.2f}), pbrFlags {:#x}.",
                                a_geom->name.c_str(), cr, cg, cb, energy, shipped,
                                *weight, a_strength,
                                Settings::GetSingleton().dyePbrPearlSheenMax, pbrFlags);
                        }
                        if (plan.coat) {
                            auto* const coat = reinterpret_cast<float*>(
                                bytes + PbrPearl::kCoatColorOffset);
                            coat[0] = lin[0];
                            coat[1] = lin[1];
                            coat[2] = lin[2];
                            spdlog::info(
                                "PbrPearl: '{}' coat takes {:02X}{:02X}{:02X}, pbrFlags "
                                "{:#x}.",
                                a_geom->name.c_str(), cr, cg, cb, pbrFlags);
                        }
                        if (haveColour && !plan.Any()) {
                            // The finding rather than a silent nothing: this
                            // piece declares no feature a pearl can colour, so
                            // the second colour has nowhere to live on it.
                            spdlog::info(
                                "PbrPearl: '{}' declares no colourable feature (pbrFlags "
                                "{:#x}; fuzz needs {:#x}, a coloured coat {:#x}|{:#x}), "
                                "so the pearl's second colour has nowhere to land.",
                                a_geom->name.c_str(), pbrFlags, PbrPearl::kFuzz,
                                PbrPearl::kTwoLayer, PbrPearl::kColoredCoat);
                        }
                        if (a_fin.glossSet) {
                            // The classic gloss multiplies specularPower, which
                            // this material never reads; roughnessScale is the
                            // lever it does read, and shine moves opposite to
                            // roughness, so the same factor divides.
                            auto* const scale = reinterpret_cast<float*>(
                                bytes + PbrPearl::kRoughnessScaleOffset);
                            const float shipped = *scale;
                            *scale = PbrPearl::RoughnessScaleFor(shipped, a_fin.gloss);
                            spdlog::info(
                                "PbrPearl: '{}' gloss {} moves roughnessScale {:.3f} -> "
                                "{:.3f}.",
                                a_geom->name.c_str(), a_fin.gloss, shipped, *scale);
                        }
                    }
                }
                if (!pbrPearlMaterial && a_fin.sheenSet) {
                    fresh->specularColor = RE::NiColor{ a_fin.sheenR / 255.0f,
                                                        a_fin.sheenG / 255.0f,
                                                        a_fin.sheenB / 255.0f };
                    fresh->specularColorScale = 1.0f;
                    // ⚠ THE FLAG DECIDES WHETHER THIS WRITE RENDERS AT ALL, and
                    // the swap deliberately does not touch it (see the census
                    // notes). Field 2026-08-09: an authored sheen never showed;
                    // this line is the instrument that says which suspect is
                    // left. specular=false means the write had nowhere to land
                    // on this shape; specular=true moves the question to the
                    // shader path and the magnitude.
                    spdlog::info(
                        "DyeFinish: '{}' sheen {:02X}{:02X}{:02X}, specular flag {}, "
                        "power {:.1f}, feature {}",
                        a_geom->name.c_str(), a_fin.sheenR, a_fin.sheenG, a_fin.sheenB,
                        a_prop->flags.any(
                            RE::BSShaderProperty::EShaderPropertyFlag::kSpecular),
                        fresh->specularPower, FeatureName(src->GetFeature()));
                }
                // ⚠ A NUDGE ON WHAT THE MESH SHIPPED, NEVER A REPLACEMENT, which
                // is the rule DyeMaterial's own header sets: the census measured
                // real meshes from 30 to 267, so an absolute write would
                // collapse a deliberately glossy cuirass and a deliberately dull
                // draugr piece onto one number. 128 is no change; each 64 above
                // or below doubles or halves.
                if (!pbrPearlMaterial && a_fin.glossSet) {
                    const float factor =
                        std::exp2((static_cast<int>(a_fin.gloss) - 128) / 64.0f);
                    fresh->specularPower =
                        std::clamp(fresh->specularPower * factor, 10.0f, 500.0f);
                }

                // ---- sparkle, the half that twinkles ---------------------
                //
                // ⚠ ON THE NORMAL MAP, NOT ONLY THE MASK, AND ON EVERY SHAPE.
                // The mask speckles are static dots: fixed in texture space,
                // averaged away down the mip chain, blind to the camera - which
                // is exactly how the first field run described them. Tilting
                // the normal per flake cell makes micro-facets whose specular
                // highlights flare and die as the view moves, so the engine
                // does the twinkling per frame. Cloth has a normal map too, so
                // Champagne Frost glitters on fabric where the mask path never
                // could.
                if (a_flake > 0) {
                    auto* const normal = fresh->normalTexture.get();
                    if (!normal) {
                        spdlog::info("DyeFinish: '{}' has no normal map, so no glints.",
                                     a_geom->name.c_str());
                    } else if (auto* const glinted = DyeTexture::Acquire(
                                   normal, RE::NiColor{ a_flake / 255.0f, 0.0f, 0.0f },
                                   a_waiter, DyeTexture::Blend::kFlakeNormal,
                                   DyeTexture::Ramp{}, a_previewOnly, 0, nullptr, {},
                                   normalPath)) {
                        spdlog::info("DyeFinish: '{}' normal '{}' glinted at {}/255.",
                                     a_geom->name.c_str(), normal->name.c_str(), a_flake);
                        fresh->normalTexture = RE::NiPointer<RE::NiSourceTexture>(glinted);
                    } else {
                        a_pending = true;
                    }
                }

                // ---- the finish probe, OS-139 ---------------------------
                //
                // ⚠ THE FEATURE TEST IS NOT DEFENSIVE, IT IS THE DIFFERENCE
                // BETWEEN A SETTING AND A HEAP CORRUPTION.
                // BSLightingShaderMaterialEnvmap is 0xB8 bytes and keeps
                // envMapScale at 0xB0; a plain BSLightingShaderMaterialBase is
                // 0xB0, so the same store on a kDefault material writes eight
                // bytes past the end of the allocation. `fresh` came from
                // src->Create(), which is virtual and returns the source's own
                // type, so the source's feature is what says which type this is.
                //
                // ⚠ NOTHING RESTORES THIS SEPARATELY, and nothing needs to. The
                // clone is discarded whole on restore and the original material
                // goes back with its own value, exactly as the diffuse does.
                const auto& cfg = Settings::GetSingleton();
                if (src->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap) {
                    auto* const env = static_cast<RE::BSLightingShaderMaterialEnvmap*>(fresh);
                    if (cfg.dyeEnvScale >= 0.0f) {
                        // ⚠ THE ORIGINAL IS LOGGED BECAUSE THERE IS NO SINGLE
                        // VANILLA NUMBER. Every nif ships its own, so "what did
                        // I change it from" is a per-piece question and the only
                        // way to learn the real baselines is to print them.
                        spdlog::info("DyeFinish: '{}' envMapScale {:.3f} -> {:.3f}",
                                     a_geom->name.c_str(), env->envMapScale, cfg.dyeEnvScale);
                        env->envMapScale = cfg.dyeEnvScale;
                    }

                    // ---- the reflection's own colour ---------------------
                    //
                    // ⚠ TWO LEVERS WERE ELIMINATED BEFORE THIS ONE, AND BOTH
                    // ELIMINATIONS ARE MEASUREMENTS RATHER THAN REASONING.
                    //
                    // envMapScale is a trade and nothing more: at 2.0 the
                    // reflection drowns the dye, at 0.5 the dye reads but the
                    // metal stops looking like metal, and no value gives both,
                    // because scale only changes HOW MUCH of an unchanged
                    // reflection is added.
                    //
                    // envMaskTexture looked like the answer and is not. The mask
                    // was tinted successfully on 1380 shapes with zero misses,
                    // and the picture did not move. ⚠ THE SHADER READS ONE
                    // CHANNEL OF IT. A red dye normalises to (1,0,0) and left
                    // the red channel at times 1, doing nothing at all; a blue
                    // dye took it to times 0 and the reflection FADED rather
                    // than turning blue. The mask is a per-texel strength map,
                    // so it is a second envMapScale and never a colour.
                    //
                    // That leaves the cubemap, by elimination rather than by
                    // hope: it is the only thing in the envmap path carrying
                    // colour at all.
                    //
                    // ⚠ A NULL CUBEMAP IS ORDINARY. Logged rather than silently
                    // skipped, because "did nothing" and "had nothing to do" are
                    // the two readings this has to separate, and the mask round
                    // is exactly why.
                    // ⚠ TintsReflection(), NOT the raw key. bDyeKeepShine is
                    // what an ordinary install has, and the reflection carries
                    // most of the look on exactly the pieces that path exists
                    // for, so reading the [Debug] key here shipped a dyed
                    // diffuse under an undyed steel cubemap.
                    // ⚠ THE REFLECTION IS THE METAL (2026-09-04), so under the
                    // envmask modes it takes the metal's colour: hex2 for a
                    // twotone dye, nothing at all for a cloth dye, and the
                    // request's own tint for every other mode, as always.
                    const auto reflTake = DyeRamp::ReflectionTakeFor(effective);
                    if (reflTake == DyeRamp::Take::kUntouched) {
                        spdlog::info("MetalSplit: '{}' keeps its own reflection; a cloth "
                                     "dye does not reach the metal.",
                                     a_geom->name.c_str());
                    } else if (cfg.TintsReflection()) {
                        const RE::NiColor reflTint =
                            reflTake == DyeRamp::Take::kSecond ? secondTint : a_tint;
                        auto* const cubemap = env->envTexture.get();
                        if (!cubemap) {
                            spdlog::info("DyeFinish: '{}' has NO envTexture, so it has no "
                                         "reflection colour to change.",
                                         a_geom->name.c_str());
                        } else if (auto* const tinted = DyeTexture::Acquire(
                                       cubemap, reflTint, a_waiter,
                                       DyeTexture::Blend::kRecolour, reflectionRamp,
                                       a_previewOnly, 0, nullptr, {}, envPath)) {
                            spdlog::info("DyeFinish: '{}' cubemap '{}' recoloured{}.",
                                         a_geom->name.c_str(), cubemap->name.c_str(),
                                         reflectionRamp.mode != 0
                                             ? " through a two-stop ramp, so it shifts with "
                                               "viewing angle"
                                             : "");
                            env->envTexture = RE::NiPointer<RE::NiSourceTexture>(tinted);
                        } else {
                            // Queued. The diffuse above already landed, so this
                            // piece is dyed but not yet recoloured in its
                            // reflection, and the repaint the build triggers
                            // will come back for it.
                            a_pending = true;
                        }
                    }

                    // ---- metallic flake, on the MASK -------------------------
                    //
                    // ⚠ THE MASK CARRIES STRENGTH, NOT COLOUR, AND FLAKE IS THE
                    // FIRST USE THAT WANTS EXACTLY THAT. The 1380-shape
                    // measurement that killed mask TINTING said the shader reads
                    // one channel of it, which is precisely why per-texel
                    // speckles work: sparse full-strength cells over a dimmed
                    // body break the reflection into discrete glints, under the
                    // vanilla shader and Community Shaders alike. Dimming the
                    // body between the flakes also lets more of the diffuse
                    // read through, which is the stop-A balance the marker-F0
                    // reflection was drowning.
                    if (a_flake > 0) {
                        auto* const mask = env->envMaskTexture.get();
                        if (!mask) {
                            spdlog::info("DyeFinish: '{}' has NO envMaskTexture, so there "
                                         "is nothing to break into flakes.",
                                         a_geom->name.c_str());
                        } else if (auto* const flaked = DyeTexture::Acquire(
                                       mask,
                                       RE::NiColor{ a_flake / 255.0f, 0.0f, 0.0f },
                                       a_waiter, DyeTexture::Blend::kFlake,
                                       DyeTexture::Ramp{}, a_previewOnly, 0, nullptr, {},
                                       envMaskPath)) {
                            spdlog::info("DyeFinish: '{}' mask '{}' broken into flakes at "
                                         "{}/255.",
                                         a_geom->name.c_str(), mask->name.c_str(), a_flake);
                            env->envMaskTexture = RE::NiPointer<RE::NiSourceTexture>(flaked);
                        } else {
                            a_pending = true;
                        }
                    }
                }

                // ---- THE OTHER LAYER AN EYE'S COLOUR LIVES IN ----------------
                //
                // ⚠⚠ A MODDED EYE OFTEN CARRIES NO COLOUR IN ITS DIFFUSE AT ALL,
                // and that, not the swap, is why a dye "did nothing" on some
                // eyes and turned others purple. MEASURED 2026-08-13 on this
                // rig: the ILV eye set's diffuse `eye01.dds` is a neutral
                // grey-brown, bright decile #91837B, and every colour a player
                // sees on it comes from the reflection layer beside it. The
                // demon set does the same through an emissive: emissiveColor
                // 15.0 at multiple 0.1 over a greyscale glow mask.
                //
                // Tinting only the diffuse therefore moved a neutral base to a
                // dim tint and left a bright untinted layer sitting on top of
                // it. Amber under blue reads PURPLE, with the core blown to
                // white, which is exactly what the field shot showed and why the
                // result barely changed between magenta and amber: the layer
                // carrying the picture was never being touched.
                //
                // ⚠ THIS IS THE SAME LESSON THE CUBEMAP ALREADY TAUGHT, one
                // material class over. The envmap path above recolours
                // envTexture for precisely this reason, and the note there says
                // it outright: the cubemap is the only thing in that path
                // carrying colour. kEye and kGlowMap keep their colour in the
                // same 0xA0 slot under different names, and nothing was reading
                // it.
                //
                // ⚠ THE ARMOUR WALK CANNOT REACH EITHER BRANCH. It skips kEye
                // and kGlowMap on the FEATURE, with the reasons written out in
                // OutfitDye.h, so these run for the eye path and nothing else.
                // The writes land on `fresh`, the clone, exactly as every other
                // write here does. shader-materials-are-pooled-never-write-one.
                if (src->GetFeature() == RE::BSShaderMaterial::Feature::kEye) {
                    auto* const eye = static_cast<RE::BSLightingShaderMaterialEye*>(fresh);
                    // ⚠⚠ THE CUBEMAP ONLY. The first cut of this recoloured the
                    // MASK beside it as well, which this module has already
                    // measured as wrong: the mask was tinted successfully on
                    // 1380 shapes with zero misses and the picture did not move,
                    // because the shader reads ONE CHANNEL of it. It is a
                    // per-texel strength map, a second envMapScale, and never a
                    // colour. The note is written out at the envmap branch above
                    // and it applies here unchanged.
                    //
                    // ⚠ AND THE SLOTS ARE NOT RELIABLY IN THE ORDER THEIR NAMES
                    // SUGGEST. Vanilla `FemaleEyesDarkElfRed` reads
                    // env='cubemaps\eyecubemap.dds' with the mask beside it,
                    // which matches, and so does the demon set. The ILV set
                    // reads env='eye01_m.dds' with 'eyecubemap.dds' in the MASK
                    // slot, so that mod has its texture set authored the other
                    // way round. That is the author's doing and it is measured
                    // rather than assumed; it also means an eye from that set
                    // has a 2D mask bound where the engine samples a cubemap,
                    // which is worth knowing before blaming anything here.
                    if (auto* const cube = eye->envTexture.get()) {
                        if (auto* const t = DyeTexture::Acquire(
                                cube, a_tint, a_waiter, DyeTexture::Blend::kRecolour,
                                DyeTexture::Ramp{}, a_previewOnly, a_capPx, nullptr, {},
                                envPath)) {
                            spdlog::info("EyeLayer: '{}' recoloured '{}'.",
                                         a_geom->name.c_str(), cube->name.c_str());
                            eye->envTexture = RE::NiPointer<RE::NiSourceTexture>(t);
                        } else {
                            a_pending = true;
                        }
                    }
                } else if (src->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap) {
                    auto* const gm = static_cast<RE::BSLightingShaderMaterialGlowmap*>(fresh);
                    // ⚠ RECOLOUR, NEVER THE OVERLAY, AND THE MASK IS WHY. A glow
                    // map is an INTENSITY map, greyscale on both sets measured
                    // here, so the overlay would return it almost unchanged and
                    // the eye would keep glowing its old colour. Recolour is
                    // luminance times the tint, which keeps every bit of the
                    // mask's shape and falloff and only says what colour it
                    // glows.
                    if (auto* const glow = gm->glowTexture.get()) {
                        if (auto* const t = DyeTexture::Acquire(
                                glow, a_tint, a_waiter, DyeTexture::Blend::kRecolour,
                                DyeTexture::Ramp{}, a_previewOnly, a_capPx, nullptr, {},
                                glowPath)) {
                            spdlog::info("EyeLayer: '{}' recoloured glow '{}'.",
                                         a_geom->name.c_str(), glow->name.c_str());
                            gm->glowTexture = RE::NiPointer<RE::NiSourceTexture>(t);
                        } else {
                            a_pending = true;
                        }
                    }
                }
            }

            a_out.geom     = RE::NiPointer<RE::BSGeometry>(a_geom);
            a_out.prop     = a_prop;
            a_out.original = a_prop->material;
            // Mandatory, and for the reason written out at both other swap
            // sites: SetMaterial runs the displaced material through the cache's
            // release, which destroys it at zero, and an armour material
            // interned for one shape normally has a refcount of exactly 1.
            a_out.original->IncRef();
            a_out.flags = a_prop->flags.underlying();

            // ⚠ true, THE UNIQUE PATH, DELIBERATELY. Acquire CRCs the material
            // and probes a hash table, and a clone differing only in one texture
            // pointer could plausibly collide with the material it was cloned
            // from, which would hand back the untinted original and make this
            // read as a silent no.
            a_prop->SetMaterial(fresh, true);
            // Nothing in the engine releases `fresh`; SetMaterial copies from it
            // and interns its own. The destructor is virtual, so this runs the
            // engine's deleting destructor and drops the texture references
            // CopyMembers took, including the one it took on the tinted texture.
            // DyeTexture's own reference is what keeps that off zero.
            delete fresh;

            // ⚠ NO FLAG IS TOUCHED, which is the point rather than an omission.
            // A same-feature clone has no offset collision to clear, so the
            // envmap that the shipped swap must sacrifice survives untouched.
            //
            // ⚠⚠ AND THIS ONE CALL IS THE LAST SUSPECT FOR THE EYE. MEASURED
            // 2026-08-13: thirteen eye swaps ran with the texture store skipped
            // entirely, no tinted texture built and none written, and the iris
            // still lit up and went purple. So nothing about the colour is
            // involved and what is left is the clone, the intern and this. The
            // pass list caches the TECHNIQUE the engine picked, not the textures
            // it binds, and a rebuild is what lets the engine decide the eye
            // renders differently than it originally did.
            //
            // ⚠ It is load bearing for ARMOUR and that is measured, so this is a
            // per-caller opt out and never a removal. The armour spike could not
            // tell a negative result from a stale pass list without it.
            if (a_repick) {
                a_prop->DoClearRenderPasses();
            }
            return true;
        }

        // ---- rung 2 of the feature-preserving tint spike ---------------------
        //
        // Tint through the PROPERTY's own emissive colour. No material is touched
        // at all, so nothing can change a feature and every effect survives by
        // construction. That makes it the cheapest thing in the spike that cannot
        // break anything else, which is the whole reason it is measured.
        //
        // ⚠ ITS BLOCKING QUESTION WAS ANSWERED BEFORE THIS EXISTED. emissiveColor
        // is a `NiColor*`, not a value, and writing through a pointer somebody
        // else also holds is the material-cache hazard one layer down. The spec
        // refused a write until ownership was settled, and bDyeEmissiveProbe
        // settled it in the field: 30 to 44 distinct allocations across five
        // passes, 0 named by more than one property, 0 null. Every property owns
        // its own colour and every one has something to write to.
        //
        // ⚠ ADDITIVE, NOT AN OVERLAY, AND THAT IS THE KNOWN WEAKNESS RATHER THAN
        // A SURPRISE. It lightens rather than tints and it will glow in the dark.
        // The same probe sharpened it: kOwnEmit is ALREADY true and emissiveMult
        // ALREADY 1.0 on the shapes measured, so the emissive is live and
        // contributing and this adds to what is already emitting. Viable is not
        // the same as right, and that judgement is the field run's.
        //
        // Delete with Settings::dyeSpikeRung when the spike returns.
        // ⚠ a_repick FALSE IS FOR THE EYE. The emissive reaches the shader through
        // the property's own constants rather than through the render pass, so a
        // rebuild is not needed to make a written colour show. It IS needed for
        // rung 2's armour case, which is why the default keeps it. On a glowing
        // eye a rebuild is the one thing worth not doing: OutfitDye.h records
        // that a glowmap material stops having its emissive gated by the glow
        // MASK once the technique moves, and an ungated emissive lights the
        // whole eyeball instead of the iris ring, sclera and all, which is
        // exactly what the field reported.
        bool WriteEmissiveTint(RE::BSGeometry* a_geom, RE::BSLightingShaderProperty* a_prop,
                               const RE::NiColor& a_tint, bool a_write, Swapped& a_out,
                               bool a_repick = true) {
            auto* const ec = a_prop->emissiveColor;
            if (!ec) {
                // Measured as never happening, handled anyway. Allocating one
                // would mean owning it for the property's lifetime, which is a
                // different and much larger question than tinting an existing
                // one, so this refuses rather than inventing an allocation.
                return false;
            }

            a_out.kind             = Swapped::Kind::kEmissive;
            a_out.geom             = RE::NiPointer<RE::BSGeometry>(a_geom);
            a_out.prop             = a_prop;
            a_out.original         = nullptr;  // no material was touched, no ref taken
            a_out.flags            = a_prop->flags.underlying();
            a_out.origEmissive     = *ec;
            a_out.origEmissiveMult = a_prop->emissiveMult;

            // ⚠ a_write FALSE IS THE NEGATIVE CONTROL. It records the originals
            // and runs the render-pass invalidation, and writes nothing. Rung 1's
            // control is what made rung 1's negative readable, and this rung gets
            // the same treatment for the same reason.
            if (a_write) {
                *ec = a_tint;
                a_prop->SetFlags(PF::kOwnEmit, true);
            }
            if (a_repick) {
                a_prop->DoClearRenderPasses();
            }
            return true;
        }

        // The tint at the brightness the author already chose.
        //
        // ⚠⚠ AN EMISSIVE IS NOT A 0..1 COLOUR AND WRITING ONE AS IF IT WERE
        // THROWS THE GLOW AWAY. MEASURED: this character's demon eye carries
        // emissiveColor (15.0, 15.0, 15.0) at multiple 0.1, so it is fifteen
        // times over the top of the range a colour picker works in. Writing a
        // picked colour straight in would take a glowing iris down to a dim one
        // and read as the dye having broken the eye.
        //
        // So the HUE comes from the pick and the MAGNITUDE stays the author's:
        // normalise the tint by its own largest channel, then scale by the
        // largest channel of what was already there. White at 15 asked for green
        // gives (0, 15, 0), which is the same glow in a different colour.
        [[nodiscard]] RE::NiColor EmissiveAt(const RE::NiColor& a_tint,
                                             const RE::NiColor& a_original) {
            const float tmax = std::max({ a_tint.red, a_tint.green, a_tint.blue });
            const float omax =
                std::max({ a_original.red, a_original.green, a_original.blue });
            if (tmax <= 0.0f) {
                return a_tint;
            }
            // A black emissive has no magnitude to preserve, so fall back to the
            // picked colour as written. Nothing reaches this today: the caller
            // only asks about an emissive it has already found to be live.
            const float scale = omax > 0.0f ? omax : 1.0f;
            return RE::NiColor{ a_tint.red / tmax * scale, a_tint.green / tmax * scale,
                                a_tint.blue / tmax * scale };
        }

        // ⚠ THE ONE TEARDOWN PATH. Restore, which runs before every repaint,
        // and Clear, which runs on the save load, both come through here.
        //
        // They did not always, and that is the 2026-08-01 skin bug. Clear
        // released its reference and dropped the record WITHOUT putting the
        // material back, on the theory that a save load is tearing this 3D down
        // anyway. Reloading into the same character tears nothing down: the
        // property came back still carrying the FacegenTint, the record that
        // knew to restore it was gone, so the feature guard read that shape as
        // kFaceGenRGBTint and skipped it for good, and the engine's own attach
        // filled the material's empty facegen texture slots from the actor
        // root. That is how armour ended up rendering with the player's skin.
        //
        // ALWAYS releases the reference SwapToTintMaterial took, on both
        // branches, and nulls it so a second pass over the same record cannot
        // double-release. Returns the action taken so the caller can count it.
        // ⚠ TWO ROOTS, AND THE SECOND ONE IS NOT OPTIONAL SINCE OS-125.
        //
        // `a_root` used to be the only one, and it is the THIRD-person root. The
        // moment the first-person biped started contributing swaps to the same
        // record, every one of them began failing this test: first-person
        // geometry does not hang off the third-person root, so it classified
        // release-only, its material was dropped without being put back, and the
        // property kept our FacegenTint with nothing left that knew how to undo
        // it. The engine then filled that material's empty facegen slots from
        // the actor and the piece rendered as bare skin (OS-126).
        //
        // That is the SAME failure the comment in Clear() describes for the
        // third-person path, reproduced on a second biped by widening what goes
        // into the record without widening what checks it. A record is keyed by
        // actor, so its attachment question is "still under ANY 3D this actor
        // owns", never "under the one root the caller happened to have".
        //
        // ⚠ NEITHER ROOT MAY BE ASSUMED PRESENT. A null root is not a failure,
        // it is the honest answer that that half of the actor has no 3D, and it
        // has to stay release-only for that geometry rather than falling through
        // to the other root's answer.
        DyeGate::TeardownAction RestoreOne(Swapped& a_s, const RE::NiAVObject* a_root,
                                           const RE::NiAVObject* a_rootFirst,
                                           std::size_t& a_cloned) {
            // ⚠ TWO WAYS A RECORD GOES STALE, AND THE OBVIOUS ONE IS THE RARER
            // ONE. Re-read the property off the geometry and write only if it
            // is STILL the one that was swapped: the NiPointer keeps the
            // GEOMETRY alive, but the geometry owns its property through
            // properties[kEffect], so anything that reassigns that slot drops
            // the last reference and frees the BSLightingShaderProperty while
            // this raw pointer still names it. Comparing the stale value is
            // defined; dereferencing it is not.
            //
            // The likelier outcome of a rebuild is not a reassigned property at
            // all, it is a DETACHED geometry: the engine builds a fresh
            // partClone and unhooks the old one, leaving our NiPointer holding
            // a perfectly valid shape that is no longer in the scene. Its own
            // parent pointer is untouched, so only a walk to the root sees it.
            auto* const live = a_s.geom ? LightingPropOf(a_s.geom.get()) : nullptr;
            // Under EITHER root, per the note above. Written as a walk per root
            // rather than one clever test because the two roots are genuinely
            // separate scenes and a geometry is in one of them or in neither.
            const bool attached =
                a_s.geom != nullptr &&
                ((a_root != nullptr && IsUnder(a_s.geom.get(), a_root)) ||
                 (a_rootFirst != nullptr && IsUnder(a_s.geom.get(), a_rootFirst)));
            const bool propStillOurs = live != nullptr && live == a_s.prop;

            // ⚠ AN EMISSIVE RECORD IS UNDONE HERE AND NEVER THROUGH
            // ClassifyTeardown. That gate is about MATERIALS: its third argument
            // is "do we hold an original to put back", and an emissive record
            // deliberately holds none. Passing it through would classify every
            // one of them release-only and strand a tint forever, which is
            // exactly the shape of OS-126. It asks the same two safety questions
            // in the same order instead, because those are about the property
            // and the scene rather than about materials.
            if (a_s.kind == Swapped::Kind::kEmissive) {
                auto* const ec = a_s.prop ? a_s.prop->emissiveColor : nullptr;
                if (propStillOurs && attached && ec) {
                    // The VALUE back through the same pointer. The probe measured
                    // that no other property names this allocation, so this
                    // restores exactly what was displaced and nothing else.
                    *ec                  = a_s.origEmissive;
                    a_s.prop->emissiveMult = a_s.origEmissiveMult;
                    a_s.prop->SetFlags(
                        PF::kOwnEmit,
                        ((a_s.flags >> static_cast<std::uint32_t>(PF::kOwnEmit)) & 1ull) != 0);
                    a_s.prop->DoClearRenderPasses();
                    return DyeGate::TeardownAction::kRestore;
                }
                // Detached or no longer ours: writing through either would be a
                // write into freed memory, which is the one direction this whole
                // teardown refuses to get wrong.
                return DyeGate::TeardownAction::kReleaseOnly;
            }

            const auto action = DyeGate::ClassifyTeardown(
                propStillOurs, attached, a_s.original != nullptr);

            if (action == DyeGate::TeardownAction::kRestore) {
                // ⚠ unk1 IS FALSE HERE AND TRUE ON THE WAY IN. That argument is
                // the whole reference protocol.
                //
                // The material cache's acquire (0x1414F7790) hands a material
                // straight back to the caller, IncRef'd and unchanged, only
                // when the caller does NOT ask for a unique instance and the
                // material is already interned plainly, meaning hashKey is not
                // the 0xFFFFFFFF "never interned" sentinel and unk30 (the
                // uniqueness salt) is zero, with no controllers on the
                // property. That is the normal state of a material the NIF
                // loader interned, so `false` usually makes the property
                // re-adopt THIS OBJECT and take back exactly the reference
                // SwapToTintMaterial borrowed. The DecRef below then lands on
                // the pre-swap count and nothing is orphaned.
                //
                // `true` would force the salted-CRC path, guarantee a miss, and
                // intern a field-identical CLONE instead, leaving our reference
                // with nobody to hand it to. The picture is right either way;
                // only the accounting differs, which is why the clone case is
                // counted rather than treated as an error.
                a_s.prop->SetMaterial(a_s.original, false);
                if (a_s.prop->material != a_s.original) {
                    ++a_cloned;
                    // ⚠⚠ A CLONE FROM THIS PATH CAN BE A DIFFERENT MATERIAL
                    // ENTIRELY, and the eye is where it was MEASURED (04:24:20,
                    // 2026-08-13). The non-unique acquire probes the cache by
                    // CRC, and every per-part eye material hashes alike: they
                    // all share the mesh-baked texture SET object, because the
                    // engine's head-part texture application swaps the derived
                    // texture pointers and never touches the set. So the cache
                    // holds ONE entry for all of them, whichever was interned
                    // first, and it handed back the baked-eye01 template
                    // against an eye21 original while the negative control
                    // proved no colour was involved. The original then died at
                    // zero refs on the DecRef below, which is why one part's
                    // texture was tinted once and never seen again.
                    //
                    // Faithful is checked by TEXTURE POINTERS, not paths: a
                    // clone of our original copies its texture references, so
                    // pointer equality is exactly "these are the textures we
                    // displaced". The unique path re-intern below is the one
                    // the swap already uses on the way in, and the 04:25:21
                    // pass-1 AFTER line measured it faithful.
                    auto* const got =
                        static_cast<RE::BSLightingShaderMaterialBase*>(a_s.prop->material);
                    auto* const gave =
                        static_cast<RE::BSLightingShaderMaterialBase*>(a_s.original);
                    if (got->diffuseTexture.get() != gave->diffuseTexture.get() ||
                        got->normalTexture.get() != gave->normalTexture.get()) {
                        const auto name = [](const RE::NiSourceTexture* a_t) {
                            return a_t ? a_t->name.c_str() : "<none>";
                        };
                        spdlog::warn(
                            "OutfitDye: the material cache swapped a restore: gave "
                            "diffuse='{}' normal='{}', got diffuse='{}' normal='{}'. "
                            "Re-interning the original through the unique path.",
                            name(gave->diffuseTexture.get()),
                            name(gave->normalTexture.get()),
                            name(got->diffuseTexture.get()),
                            name(got->normalTexture.get()));
                        a_s.prop->SetMaterial(a_s.original, true);
                    }
                }
                // Put back only the bits the swap touched, and put them back
                // through SetFlags. A wholesale write of the saved 64-bit word
                // would also undo whatever the engine changed in between, and
                // it would skip the render-pass invalidation SetFlags does:
                // 0x14147BEE0 stamps lastRenderPassState (+0x34) with
                // 0x7FFFFFFF whenever a bit actually moves.
                for (const auto f : kSwapFlags) {
                    a_s.prop->SetFlags(
                        f, ((a_s.flags >> static_cast<std::uint32_t>(f)) & 1ull) != 0);
                }
                // Same single call as the swap path, and for the same reason:
                // SetupGeometry and FinishSetupGeometry mutate the property and
                // the material, and FinishSetupGeometry forcing specularPower
                // to 1.0 is what used to leave restored armour shinier than it
                // started. See SwapToTintMaterial.
                a_s.prop->DoClearRenderPasses();
            }
            if (a_s.original) {
                a_s.original->DecRef();  // matches the IncRef in SwapToTintMaterial
                a_s.original = nullptr;
            }
            return action;
        }

        // Everything a finished chain has to say, in one place so the two exits
        // cannot report differently. Caller holds g_lock.
        //
        // ⚠ THE MEASUREMENT IS THE POINT, not the tidy summary. OS-110 existed
        // because a comment asserted the chain waited twelve frames and no line
        // anywhere reported how long it actually took. The elapsed figure below
        // settles that from a field log rather than from anyone's model of the
        // task queue, and it keeps settling it after this fix.
        void ReportChainEnd(RE::FormID a_id, const RetryState& a_st) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - a_st.started)
                                .count();

            if (DyeGate::DrainedWithoutWaiting(a_st.walksLeft, a_st.postsLeft)) {
                // The gate never opened, so the queue drained every link inside
                // one frame. Not fatal: the walk that did run is the same walk
                // the old code ran twelve times, and the next biped rebuild
                // re-arms. Loud because it is the OS-110 signature and because
                // it means the retry path cannot cover a cold-cache attach.
                spdlog::warn("OutfitDye: actor {:08X} spent all {} chain post(s) in {} ms "
                             "with {} walk(s) never taken, so the task queue drained the "
                             "whole chain without a frame passing. {} walk(s) actually ran "
                             "and {} dyed slot(s) are still waiting; the next biped rebuild "
                             "re-arms.",
                             a_id, a_st.postsDone, ms, a_st.walksLeft, a_st.walksDone,
                             a_st.pending);
                return;
            }
            if (a_st.pending > 0) {
                // Budget spent with slots still waiting. Said out loud because
                // it is the ONE state that looks exactly like a working dye that
                // simply did not appear: a mesh that never finishes attaching
                // leaves partClone null forever, and without this line the field
                // log shows nothing but debug chatter.
                spdlog::warn("OutfitDye: actor {:08X} still has {} dyed slot(s) with no "
                             "partClone after {} walk(s) over {} ms. Those garments render "
                             "undyed until the next biped rebuild; a mesh that never "
                             "finishes attaching presents exactly like this.",
                             a_id, a_st.pending, a_st.walksDone, ms);
                return;
            }
            spdlog::debug("OutfitDye: actor {:08X} chain finished, {} walk(s) across {} "
                          "post(s) over {} ms, nothing left waiting.",
                          a_id, a_st.walksDone, a_st.postsDone, ms);
        }

        // One deferred painting attempt, plus the decision to arm the next one.
        // The tail re-arms by calling itself, and that costs no stack: each link
        // is a fresh SKSE task posted from inside the previous one's body, so
        // the chain is a queue of single frames bounded by the budget in
        // g_retry, not a recursion.
        void PostRepaintAttempt(RE::ActorHandle a_actor, RE::FormID a_id) {
            auto* const task = SKSE::GetTaskInterface();
            if (!task) {
                std::scoped_lock l(g_lock);
                g_retry.erase(a_id);  // never armed, so never leave the flag set
                return;
            }
            task->AddTask([a_actor, a_id] {
                using clock = std::chrono::steady_clock;

                // Decide BEFORE doing anything, under the lock, so two links
                // cannot both conclude that enough time has passed.
                bool        walk    = false;
                std::size_t pending = 0;
                {
                    std::scoped_lock l(g_lock);
                    auto&            st = g_retry[a_id];
                    const auto       now = clock::now();
                    const auto       gapMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - st.lastWalk)
                            .count();
                    walk = DyeGate::ShouldWalkNow(
                        gapMs >= static_cast<long long>(DyeGate::kRepaintGapMs), st.walksLeft);
                    if (walk) {
                        st.lastWalk = now;
                        --st.walksLeft;
                        ++st.walksDone;
                    }
                    if (st.postsLeft > 0) {
                        --st.postsLeft;
                    }
                    ++st.postsDone;
                    pending = st.pending;
                }

                // A link that is only here because no real time has passed does
                // no work at all. That is the whole point: under a same-pass
                // drain these cost a lambda each and the biped is walked once
                // rather than twelve times.
                if (!walk) {
                    bool again = false;
                    {
                        std::scoped_lock l(g_lock);
                        auto&            st = g_retry[a_id];
                        again = DyeGate::ShouldPostAgain(st.pending, st.walksLeft, st.postsLeft);
                        if (!again) {
                            ReportChainEnd(a_id, st);
                            g_retry.erase(a_id);
                        }
                    }
                    if (again) {
                        PostRepaintAttempt(a_actor, a_id);
                    } else if (auto ptr = a_actor.get(); ptr && ptr->IsPlayerRef()) {
                        // The settled point of a refresh, so the first person
                        // paint runs against the tree that is really there and
                        // the census reads the tree the player is looking at.
                        // ⚠ THE PAINT GOES FIRST so the census counts what it
                        // just built, in the same pass.
                        Overlay1P::PaintPlayer(ptr.get());
                        OverlayReconcile::LogOverlayCensus(ptr.get());
                    }
                    return;
                }

                try {
                    // Re-resolve BOTH the actor and the outfit at drain time,
                    // the same discipline BipedPost::QueueNodeCull follows: the
                    // actor may have unloaded since the rebuild (a stale handle
                    // resolves to null), and the outfit the player is showing
                    // can have changed in between, because the editor writes it
                    // from the FUCK present thread. ActiveOutfitFor hands back a
                    // COPY taken under the session lock, which is what makes
                    // reading it from here safe at all.
                    auto             ptr   = a_actor.get();
                    RE::Actor* const actor = ptr.get();
                    if (actor) {
                        if (const auto outfit =
                                OutfitSession::GetSingleton().ActiveOutfitFor(actor)) {
                            pending = Repaint(actor, *outfit).pending;
                        }
                    }
                } catch (const std::exception& e) {
                    // Never unwind into the task queue. A throw here costs one
                    // attempt, and the chain below still winds down cleanly.
                    spdlog::error("OutfitDye: deferred repaint threw: {}", e.what());
                } catch (...) {
                    spdlog::error("OutfitDye: deferred repaint threw a non-standard exception.");
                }

                bool again = false;
                {
                    std::scoped_lock l(g_lock);
                    // operator[] rather than find: a missing entry default-
                    // constructs to a spent budget, which ends the chain and
                    // erases it again. There is no path that can leave this
                    // holding a dangling iterator or a set flag.
                    auto& st   = g_retry[a_id];
                    st.pending = pending;
                    again = DyeGate::ShouldPostAgain(pending, st.walksLeft, st.postsLeft);
                    if (!again) {
                        ReportChainEnd(a_id, st);
                        g_retry.erase(a_id);
                    }
                }

                if (again) {
                    PostRepaintAttempt(a_actor, a_id);
                } else if (auto ptr = a_actor.get(); ptr && ptr->IsPlayerRef()) {
                    Overlay1P::PaintPlayer(ptr.get());
                    OverlayReconcile::LogOverlayCensus(ptr.get());
                }
            });
        }
    }

    // ---- OS-144: the dye-stripe flash ------------------------------------
    //
    // ⚠ ONE SHARED ANSWER TO "IS THIS A SHAPE", used by the paint walk and the
    // flash walk both. The stripe's channel is derived from a shape's position
    // in the traversal, so if the two walks disagreed about which geometries
    // count, the flash would light a different piece than the stripe dyes, and
    // it would do it only on meshes carrying whatever the two disagreed about.
    // That is unfindable in the field. One function, two callers.
    RE::BSLightingShaderProperty* CountableShapeProp(RE::BSGeometry* a_geom) {
        auto* const prop = LightingPropOf(a_geom);
        if (!prop || !prop->material) {
            return nullptr;
        }
        // The netimmerse_cast validated the PROPERTY, not its material, and
        // CopyBaseMembers reads out to 0x78; the derivation is at the paint
        // walk's own copy of this test.
        if (prop->material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
            return nullptr;
        }
        return prop;
    }

    // Can nothing this shape draws reach the screen?
    //
    // ⚠ MEASURED ON ONE ASSET AND WRITTEN NO WIDER THAN THAT. Tullius SMP Hair 3
    // blanks the KS HDT hairline it inherits by overriding the nif with a dummy
    // whose BSLightingShaderProperty alpha is 0.000 and whose NiAlphaProperty has
    // blending and testing on at threshold 255. Under blending an alpha of zero
    // contributes nothing; under testing it fails every pixel. Either way the
    // shape is invisible and a dye stripe for it is a control over nothing.
    //
    // ⚠ BOTH HALVES, AND THE ORDER OF THE AND MATTERS. Without an alpha property
    // the engine draws opaque and ignores the alpha channel, so a zero alpha on
    // an opaque shape is a mesh that IS visible and must keep its stripe. That is
    // why this is not "material alpha zero" alone.
    //
    // ⚠ THE MATERIAL'S ALPHA, NOT THE PROPERTY'S. BSShaderProperty::alpha is
    // what a float controller animates, so a pulsing rune would flicker in and
    // out of the grid if it were read here; materialAlpha comes off the nif and
    // stays put. The census prints both, and on the shape that produced this
    // rule they agreed at 0.000.
    //
    // a_prop is the CountableShapeProp result, so material is a lighting
    // material and the base cast is safe.
    bool ShapeIsInvisible(RE::BSGeometry* a_geom, RE::BSLightingShaderProperty* a_prop) {
        auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(a_prop->material);
        if (mat->materialAlpha > 0.0f) {
            return false;
        }
        auto* const alpha = a_geom->GetGeometryRuntimeData().alphaProperty.get();
        return alpha && (alpha->GetAlphaBlending() || alpha->GetAlphaTesting());
    }

    // ---- the eye dye spike (dyeing-eyes step 3) -----------------------------
    //
    // TEMPORARY. Delete with Settings::eyeDyeSpike once the feature is decided.
    //
    // ⚠⚠ THE TARGET IS THE HEAD PART, NOT THE MATERIAL, AND THAT WAS MEASURED
    // THE HARD WAY. The research proposed "material feature is kEye plus a live
    // diffuse" and the head census killed it on 2026-08-12: this character's
    // demon iris is kGlowMap, so that rule finds NOTHING on her, and kGlowMap
    // cannot simply be added because a glowing tattoo wears it too. What the
    // census did establish, on a heavily modded head AND on a vanilla one, is
    // that a head part's EDITOR ID equals its geometry's NAME under the face
    // node, exactly. So the part names the shape and the material never enters
    // into it, which is also why this works on an iris the feature test cannot
    // classify.
    //
    // ⚠ THE PART ITSELF, NOT ITS extraParts. On both of this character's eye
    // sets the part was the iris while the outer lens, the two lens dummies and
    // the shadow ring were extras. The shadow ring would spread colour onto the
    // lids and the outer lens may be additive, so they are left alone until
    // there is a reason on screen to include them.
    //
    // ⚠ A GEOMETRY UNDER THE FACE NODE MAY BELONG TO NO PART AT ALL. The census
    // found another mod's '_NK_EDHorns02' sitting there. That is why this is a
    // name match against one part rather than a walk of the face node.
    // a_tint colours the iris, a_sclera the white around it; either may be
    // unset. One pass for the pair because they land in ONE texture build:
    // the iris mask splits the diffuse into the two sides and each takes its
    // own colour, so two passes would be two builds racing to swap one slot.
    // a_blend is the outfit's own OS::DyeBlend::Choice for the pair, and its
    // zero is what every eye in every save carries. a_second is the eye's
    // second colour, which takes the high-u half of the texture; see
    // Outfit::eyeTint2 for why one field gives two different looks.
    void PaintEyeTint(RE::Actor* a_actor, const HairTint& a_tint, const HairTint& a_sclera,
                      const HairTint& a_second, std::uint8_t a_blend,
                      std::vector<Swapped>& a_record, std::size_t& a_pending) {
        // ⚠⚠ THE PASS SAYS WHAT IT WAS ASKED FOR BEFORE IT DECIDES ANYTHING.
        // This function was silent on the path it takes most often, so "I set
        // an eye colour and nothing happened" produced a log with no eye line
        // in it at all and nothing to separate "the walk never ran", "it ran
        // and refused" and "it painted and the picture is wrong". One line at
        // the funnel is what makes the next field report answerable in one
        // read: instrument-your-own-component-first.
        spdlog::info("EyeDye: asked for iris={} sclera={} second={} blend={}",
                     a_tint.set ? fmt::format("{:02X}{:02X}{:02X}", a_tint.r, a_tint.g,
                                              a_tint.b)
                                : "off",
                     a_sclera.set ? fmt::format("{:02X}{:02X}{:02X}", a_sclera.r,
                                                a_sclera.g, a_sclera.b)
                                  : "off",
                     a_second.set ? fmt::format("{:02X}{:02X}{:02X}", a_second.r,
                                                a_second.g, a_second.b)
                                  : "off",
                     a_blend);
        // ⚠ THE SECOND COLOUR IS NOT A REASON TO RUN ON ITS OWN. It needs the
        // iris colour to sit beside, and the refusal below says so; a pass that
        // started for it alone would build a texture with nothing to paint.
        if (!a_tint.set && !a_sclera.set) {
            spdlog::info("EyeDye: no iris and no sclera colour on this outfit, so her own "
                         "eyes are left alone.");
            return;
        }
        auto* const base = a_actor ? a_actor->GetActorBase() : nullptr;
        if (!base) {
            return;
        }
        // GetCurrentHeadPartByType, not GetHeadPartByType: the former consults
        // the overlay list first, which is what the engine renders from. Same
        // reason HeadPart::CurrentFor records.
        auto* const part =
            base->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kEyes);
        const char* const edid = part ? part->GetFormEditorID() : nullptr;
        if (!edid || !*edid) {
            spdlog::info("EyeSpike: actor {:08X} has no current eyes head part, or it "
                         "carries no editor id, so there is no name to match.",
                         a_actor->GetFormID());
            return;
        }
        auto* const proc     = a_actor->GetActorRuntimeData().currentProcess;
        auto* const mid      = proc ? proc->middleHigh : nullptr;
        auto* const faceNode = mid ? mid->faceNodeSkinned : nullptr;
        if (!faceNode) {
            spdlog::info("EyeSpike: actor {:08X} has no face node yet, so the eyes are not "
                         "built. Nothing to dye this pass.",
                         a_actor->GetFormID());
            return;
        }

        const auto& cfg = Settings::GetSingleton();

        // Parsed here rather than through JsonCodec::ColourFromHex, which would
        // drag the JSON codec into this file for something built to be deleted.
        //
        // ⚠⚠ THE DEFAULT WAS MAGENTA AND THAT COST TWO ROUNDS. It was chosen
        // because no eye texture is magenta, so it could not be mistaken for the
        // swap quietly not happening. It is also what a flat blue placeholder
        // turns into under both blends, so a success and the placeholder fault
        // looked the same and the second sighting read as a confirmation of the
        // first. Amber is the default now: nothing in this pipeline produces it
        // by accident. a-test-colour-must-not-also-be-a-failure-mode.
        // The iris colour, canonical BLACK when the outfit sets none. The tint
        // is in the cache key even on a sclera-only build (the contract at
        // SwapToTintedDiffuse), so an arbitrary value under a cleared flag
        // would fragment one sclera dye into many cache entries.
        bool         irisSet = a_tint.set;
        std::uint8_t rgb[3]{};
        if (irisSet) {
            rgb[0] = a_tint.r;
            rgb[1] = a_tint.g;
            rgb[2] = a_tint.b;
        }
        // ⚠ THE INI HEX IS A DEBUG OVERRIDE NOW, NOT THE SOURCE. bEyeDyeSpike
        // forces one colour onto whatever the walk repaints, which is what the
        // field runs used before this was an outfit field. With the spike off,
        // and that is the shipping path, the outfit's own colour is what lands.
        if (const auto& hex = cfg.eyeDyeSpikeHex;
            cfg.eyeDyeSpike && hex.size() == 6) {
            char*      end = nullptr;
            const auto v   = std::strtoul(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 6) {
                irisSet = true;
                rgb[0] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
                rgb[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
                rgb[2] = static_cast<std::uint8_t>(v & 0xFFu);
            }
        }
        const RE::NiColor tint{ static_cast<float>(rgb[0]) / 255.0f,
                                static_cast<float>(rgb[1]) / 255.0f,
                                static_cast<float>(rgb[2]) / 255.0f };
        DyeTexture::MaskDye maskDye{ irisSet, a_sclera.set, a_sclera.r, a_sclera.g,
                                     a_sclera.b };
        // The eye's SECOND colour. The paint splits the TEXTURE down the
        // middle: the picker's colour keeps the low-u half and this takes the
        // high-u half.
        //
        // ⚠⚠ WHAT THE PLAYER SEES DEPENDS ON THE MESH, AND BOTH ANSWERS ARE
        // WANTED. A SHARED-disc set (1123 of 1549 parts censused: ILV,
        // Aretuza, Hit2, LDD) has both eyeballs reading the same texels, so
        // this paints a two-tone iris on both eyes. A UV-SPLIT set (426 parts:
        // wammy, UBE 8_1) has the eyes reading disjoint halves, so the same
        // bytes give one colour per eye. Outfit::eyeTint2 carries why.
        //
        // ⚠⚠ sEyeDyeSplitHex IS GONE, AND ITS LAST DAY COST A FIELD SESSION.
        // The spike key forced a second colour on EVERY eye paint, and because
        // the split takes gStopB the sclera was dropped every single time. The
        // field report was "sclera dye doesn't work" plus "I cannot get both
        // eyes one colour", and both were one armed [Debug] line in an INI
        // nobody was looking at: live-ini-overrides-code-defaults. A spike key
        // whose feature has shipped is not harmless, it is a second author of
        // the same state, so it is deleted rather than left for tidiness.
        std::uint8_t split[3]{};
        bool         splitSet = false;
        // ⚠⚠ THE SETTING GATES THE PAINT AND DyeGrid GATES THE CONTROL, AND THE
        // TWO MUST NOT DRIFT. bEyeAdvancedColour is off by default, so an
        // ordinary install dyes the iris and the white and nothing here runs.
        // A stored colour is NOT cleared when the setting is off: it stops
        // painting and comes back untouched when the setting returns.
        if (a_second.set && !cfg.eyeAdvancedColour) {
            spdlog::info("EyeSplit: this outfit carries a second eye colour, but "
                         "[Dye] bEyeAdvancedColour is off, so it is not painted. The "
                         "colour is kept.");
        }
        if (a_second.set && cfg.eyeAdvancedColour) {
            splitSet = true;
            split[0] = a_second.r;
            split[1] = a_second.g;
            split[2] = a_second.b;
        }
        // ⚠ A SECOND COLOUR NEEDS A FIRST ONE TO SIT BESIDE. With the iris
        // unset the build carries a canonical black tint and the low-u half
        // has no colour of its own, so half an eye would paint and half would
        // not. Refused rather than half-applied, and said out loud.
        if (splitSet && !irisSet) {
            spdlog::info("EyeSplit: a second colour is set and the iris is not, so there "
                         "is nothing for it to sit beside; the second colour is dropped.");
            splitSet = false;
        }
        if (splitSet) {
            maskDye.splitSet = true;
            maskDye.r2       = split[0];
            maskDye.g2       = split[1];
            maskDye.b2       = split[2];
            if (maskDye.scleraSet) {
                // gStopB carries ONE colour and the split wins it, which is the
                // precedence DyeTexture already ships. On a split set the flat
                // mask leaves the sclera nothing to land on anyway. Said out
                // loud so a sclera colour going quiet reads as this line rather
                // than as a dead control; the editor crosses the stripe for the
                // same reason.
                spdlog::info("EyeSplit: a second eye colour is set, so the sclera colour "
                             "is dropped for this build; gStopB carries the second "
                             "colour instead.");
                maskDye.scleraSet = false;
            }
            spdlog::info("EyeSplit: first colour {:02X}{:02X}{:02X} takes the low-u half, "
                         "second colour {:02X}{:02X}{:02X} takes the high-u half. On a "
                         "shared-disc eye that is two tones per iris; on a UV-split eye "
                         "it is one colour per eye.",
                         rgb[0], rgb[1], rgb[2], split[0], split[1], split[2]);
        }

        bool found = false;
        RE::BSVisit::TraverseScenegraphGeometries(
            faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                if (!a_geom || a_geom->name != edid) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                found = true;
                auto* const prop = LightingPropOf(a_geom);
                if (!prop) {
                    spdlog::info("EyeSpike: '{}' matched the eyes part but carries no "
                                 "lighting material, so there is no diffuse to replace.",
                                 edid);
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                // ---- the eyes this cannot dye, refused out loud -------------
                //
                // ⚠⚠ A LIVE EMISSIVE IS THE DISCRIMINATOR, AND IT IS MEASURED
                // ACROSS TEN EYES ON TWO HEADS RATHER THAN REASONED. Every eye
                // whose emissiveColor is black took its colour correctly and
                // looked right. Every eye carrying a real emissive lit up and
                // went purple the moment the swap landed, and stayed that way.
                // The user named this correlation before the log did.
                //
                // ⚠⚠ AND THE COLOUR IS NOT INVOLVED, which is what makes this a
                // refusal rather than a bug to keep chasing here. Thirteen swaps
                // ran with the texture store skipped entirely, no tinted texture
                // built and none written, and the iris still lit and went
                // purple. Skipping the render pass rebuild did not change it
                // either. So the swap itself provokes it on these materials, and
                // no amount of work on the tint can reach it.
                //
                // ⚠ THE FEATURE IS NOT THE TEST. This character's glowing eye is
                // kGlowMap and her ordinary one is kEye, which makes the feature
                // look like a fine signal right up until an eye set ships a
                // glowing kEye. The emissive is what the fault tracks, so the
                // emissive is what this reads.
                // material-feature-is-not-a-reliable-eye-signal.
                //
                // ⚠ REFUSING OUT LOUD IS THE USER'S OWN ANSWER to what a
                // non-dyeable eye does: the control greys and says why on hover.
                // This log line is what the UI will read to grey it. Silence was
                // ruled out before any of this was built.
                if (const auto* const emissive = prop->emissiveColor;
                    prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit) &&
                    emissive && prop->emissiveMult > 0.0f &&
                    (emissive->red > 0.0f || emissive->green > 0.0f ||
                     emissive->blue > 0.0f)) {
                    // ⚠ THE SCLERA HAS NO SURFACE HERE, whichever colours are
                    // set. A glowing eye is tinted through its emissive and its
                    // material is never touched (swapping it breaks the glow
                    // mask's gating), so there is no texture build for the
                    // sclera's half to ride. Said out loud rather than
                    // silently, because a picked colour doing nothing reads as
                    // a dead control.
                    if (a_sclera.set) {
                        spdlog::info("EyeSpike: '{}' glows, so its material is never "
                                     "touched and a sclera colour has nowhere to land; "
                                     "the iris colour still reaches the emissive.",
                                     edid);
                    }
                    if (!irisSet) {
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    const auto want = EmissiveAt(tint, *emissive);
                    spdlog::info("EyeSpike: '{}' glows ({:.2f},{:.2f},{:.2f}) at x{:.2f}, so "
                                 "its colour is the EMISSIVE and not the diffuse; writing "
                                 "({:.2f},{:.2f},{:.2f}) and leaving the material alone.",
                                 edid, emissive->red, emissive->green, emissive->blue,
                                 prop->emissiveMult, want.red, want.green, want.blue);
                    Swapped em;
                    if (WriteEmissiveTint(a_geom, prop, want, !cfg.eyeDyeNegativeControl, em,
                                          false)) {
                        a_record.push_back(std::move(em));
                    }
                    return RE::BSVisit::BSVisitControl::kStop;
                }

                Swapped s;
                bool    wait = false;
                // ⚠⚠ THE READING BEFORE THE SWAP, AND IT IS HALF AN ANSWER ON
                // ITS OWN. A slot that is populated here and empty in the line
                // below is the defect; a slot that is empty in both was never
                // ours to lose. Nothing else can tell those two apart, and
                // OS-192's chain took three passes to separate exactly that.
                spdlog::info("EyeSpike: BEFORE '{}' {}", edid, MaterialReading::Describe(prop));
                // Ramp{} and DyeMaterial{} are both their own neutral: one flat
                // colour, no second stop, no sheen and no gloss override. The
                // spike asks whether a tinted diffuse renders on an eye, and a
                // finish on top would be a second variable in a one-variable
                // question.
                // ⚠ a_write FALSE IS THE NEGATIVE CONTROL. Everything but the
                // texture store still runs, so a picture that still moves in
                // this mode cannot be blamed on the colour. Purple was already
                // identical under magenta and amber, which is the same reading
                // one step less rigorously.
                const bool write = !cfg.eyeDyeNegativeControl;
                // The last two arguments are the iris mask and its dye: the eye
                // is the one caller that confines its tint to the disc the
                // normal map's alpha draws, so a green dye gives green irises
                // rather than green eyeballs, and the sclera's colour rides the
                // far side of the same disc in the same build. The note at
                // SwapToTintedDiffuse carries the measurement.
                const bool ok = SwapToTintedDiffuse(a_geom, prop, tint, write, s, wait,
                                                    a_actor->GetHandle(),
                                                    DyeTexture::Ramp{}, DyeMaterial{}, 0,
                                                    255, false, cfg.eyeDyeCapPx,
                                                    // ⚠ THE EYE IS STILL NOT ON sDyeBlend.
                                                    // The armour key never reaches here;
                                                    // what overrides this fallback is the
                                                    // OUTFIT's own blend, chosen on the eye
                                                    // tile.
                                                    //
                                                    // ⚠⚠ OVERLAY, NOT RECOLOUR, USER CALL
                                                    // 2026-08-13: an eye texture carries
                                                    // white reflective highlights and
                                                    // recolour repaints them along with
                                                    // everything else, where a base-
                                                    // dependent curve leaves them standing.
                                                    //
                                                    // ⚠ AND IT IS THE REAL OVERLAY, WHICH
                                                    // HAS NEVER RUN ON AN EYE. The measured
                                                    // "overlay is wrong for an eye" note
                                                    // (amber on a green iris at #9ABF13,
                                                    // still green) was taken when kOverlay
                                                    // NAMED SOFT LIGHT, before the rename
                                                    // earlier the same day. That reading
                                                    // belongs to kSoftLight and says
                                                    // nothing about this curve. Recolour is
                                                    // still one pick away on the eye tile.
                                                    DyeTexture::ResolveDyeBlend(
                                                        a_blend, DyeTexture::Blend::kOverlay),
                                                    !cfg.eyeDyeSkipRepick, true, maskDye);
                // ⚠ prop->material, NOT the clone the swap built. SetMaterial
                // hands its argument to the material cache, which interns a
                // CLONE of it through the same virtual Create and CopyMembers
                // pair and stores THAT on the property. The material the engine
                // renders from is therefore a second copy nobody has ever read,
                // and reading the one the swap made would answer for the wrong
                // object.
                if (ok) {
                    spdlog::info("EyeSpike: AFTER  '{}' {}", edid, MaterialReading::Describe(prop));
                }
                // ⚠ RECORDED ON ok ALONE, MATCHING THE ARMOUR WALK. A swap that
                // returned true took ownership of the displaced material, and a
                // record is the only thing that knows how to put it back. A
                // wait is not a failure: the texture is building on the render
                // thread and the completion queues a repaint that re-enters
                // this pass.
                if (ok) {
                    a_record.push_back(std::move(s));
                }
                if (wait) {
                    ++a_pending;
                }
                spdlog::info("EyeSpike: '{}' on actor {:08X} rgb={:02X}{:02X}{:02X} -> {}{}{}",
                             edid, a_actor->GetFormID(), rgb[0], rgb[1], rgb[2],
                             ok ? "SWAPPED" : "not swapped",
                             wait ? " (waiting on the texture build)" : "",
                             write ? "" : " [NEGATIVE CONTROL: no texture written]");
                return RE::BSVisit::BSVisitControl::kStop;
            });

        if (!found) {
            // ⚠ THIS IS THE LINE THAT WOULD CATCH THE RULE BEING WRONG on a
            // head nobody has censused. The whole targeting rule is "the part's
            // editor id is the geometry's name", measured on two heads, and a
            // third that disagrees shows up here and nowhere else.
            spdlog::warn("EyeSpike: the eyes part is '{}' but NO geometry under actor "
                         "{:08X}'s face node carries that name. Either this head does not "
                         "follow the editor-id-is-the-shape-name rule the census measured "
                         "on two others, or the eyes are not built yet.",
                         edid, a_actor->GetFormID());
        }
    }

    // ⚠ A SEPARATE REGISTRY FROM g_swapped, AND NOT AS A MATTER OF TASTE.
    // Repaint rebuilds g_swapped wholesale after calling Restore, so a flash
    // record parked there would be silently dropped by the next refresh: the
    // record would go, the emissive would not, and the shape would stay lit
    // with nothing left that knew how to put it back. That is precisely the
    // shape of OS-126, one dimension over.
    //
    // Records are Swapped/kEmissive so RestoreOne undoes them, which is the
    // proven path rather than a second one written to match.
    std::mutex           g_flashLock;
    std::vector<Swapped> g_flash;          // guarded by g_flashLock
    RE::FormID           g_flashActor{ 0 };  // whose 3D the records hang off

    // Caller holds g_flashLock.
    void EndFlashLocked() {
        if (g_flash.empty()) {
            g_flashActor = 0;
            return;
        }
        // Look the actor up rather than storing a pointer: a flash outlives a
        // frame, and the reason this teardown exists at all is that the 3D
        // underneath it may be gone.
        RE::Actor* actor = nullptr;
        if (g_flashActor != 0) {
            actor = RE::TESForm::LookupByID<RE::Actor>(g_flashActor);
        }
        // Both roots, for the reason Restore's own comment gives: a record
        // naming a root that is now null is stale by definition, and null is
        // the honest answer rather than a failure.
        const auto* const root      = actor ? actor->Get3D(false) : nullptr;
        const auto* const rootFirst = actor ? actor->Get3D(true) : nullptr;

        std::size_t put = 0, stale = 0, cloned = 0;
        for (auto& s : g_flash) {
            if (RestoreOne(s, root, rootFirst, cloned) == DyeGate::TeardownAction::kRestore) {
                ++put;
            } else {
                ++stale;
            }
        }
        spdlog::info("DyeFlash: put back {} shape(s), {} already stale (OS-144).", put, stale);
        g_flash.clear();
        g_flashActor = 0;
    }

    // ---- OS-206: the requip transition -----------------------------------
    //
    // ⚠⚠ A SECOND REGISTER, SHARING THE RECORD TYPE AND RestoreOne, AND THAT
    // SHARING IS THE POINT. The stripe flash and this both drive emissiveMult
    // on the same properties, and the editor can arm a stripe and then hit
    // Apply. Two registers holding one property restore over each other and
    // leave a garment at the peak forever, so ArmRequip takes the flash down
    // first and neither ever writes through the other's record.
    //
    // Separate rather than merged because their lifetimes disagree: a flash is
    // taken down by the editor closing or the stripe changing, and a flourish
    // by a rebuild landing or a frame budget expiring.
    std::mutex           g_requipLock;
    std::vector<Swapped> g_requip;           // guarded by g_requipLock
    RE::FormID           g_requipActor{ 0 };  // whose 3D the records hang off

    // Caller holds g_requipLock.
    void EndRequipLocked() {
        if (g_requip.empty()) {
            g_requipActor = 0;
            return;
        }
        // Looked up rather than stored, on EndFlashLocked's terms: a flourish
        // outlives many frames and the 3D under it is expected to be gone.
        RE::Actor* actor = nullptr;
        if (g_requipActor != 0) {
            actor = RE::TESForm::LookupByID<RE::Actor>(g_requipActor);
        }
        const auto* const root      = actor ? actor->Get3D(false) : nullptr;
        const auto* const rootFirst = actor ? actor->Get3D(true) : nullptr;

        std::size_t put = 0, stale = 0, cloned = 0;
        for (auto& s : g_requip) {
            if (RestoreOne(s, root, rootFirst, cloned) == DyeGate::TeardownAction::kRestore) {
                ++put;
            } else {
                ++stale;
            }
        }
        spdlog::info("Requip: put back {} shape(s), {} already stale (OS-206).", put, stale);
        g_requip.clear();
        g_requipActor = 0;
    }

    // ---- the scabbard that goes with a weapon clone ----------------------
    //
    // ⚠⚠ MEASURED 2026-08-14, AND IT IS NOT WHERE THE NIF PUTS IT. A sword or
    // axe NIF carries its scabbard inside it as a node named `Scb`, so the
    // obvious reading is that a walk over the clone gets it for free. The
    // census says otherwise: on a live war axe the `Scb` node sits under the
    // SKELETON's `WeaponAxe` node (whose own parent is XPMSSE's
    // `MOV WeaponAxeDefault`) and `underThisClone=false`. `Scb` is an engine
    // FixedString, so the engine looks it up by name after the load and
    // re-parents it out of the clone, which is what lets an empty scabbard stay
    // on the hip while the weapon is in the hand.
    //
    // ⚠ THE SIBLING RULE, AND IT IS WHY THIS NEEDS NO NODE-NAME TABLE. A
    // sheathed weapon's clone hangs on the same sheath node the scabbard was
    // moved to, so the scabbard is the clone's SIBLING and the parent we
    // already hold names it. A table of skeleton node names per class and hand
    // would have to be right about vanilla, XPMSSE, every weapon-style mod and
    // every left-hand variant; the parent pointer is right by construction.
    //
    // ⚠ A DRAWN WEAPON ANSWERS NULL AND THAT IS CORRECT RATHER THAN
    // UNFINISHED. Its clone's parent is then `WEAPON` or `SHIELD`, the hand
    // nodes, which own no scabbard. Moving a node is not rebuilding it, so a
    // scabbard dyed in the editor keeps its colour when the player draws.
    //
    // ⚠⚠ A FREE FUNCTION AND NOT A LAMBDA IN Repaint, BECAUSE THE FLASH WALK
    // NEEDS THE SAME ANSWER. A shape's channel is its POSITION in the
    // traversal, so the painter and the flash have to agree about which roots
    // are walked and in what order, or clicking a stripe lights a different
    // shape than the one it dyes. Field 2026-08-14: the scabbard dyed and did
    // not flash, which is exactly that disagreement, one walk wide.
    RE::NiAVObject* ScabbardFor(RE::NiAVObject* a_clone) {
        auto* const parent = a_clone ? a_clone->parent : nullptr;
        if (!parent) {
            return nullptr;
        }
        // Direct children only. A deep search from the sheath node would walk
        // back down into the weapon clone itself, which is the one subtree this
        // must not claim: it is already being walked, and walking it twice would
        // give its shapes two channels.
        for (const auto& child : parent->GetChildren()) {
            auto* const node = child.get();
            if (node && !node->name.empty() &&
                std::string_view{ node->name.c_str() } == "Scb") {
                return node;
            }
        }
        return nullptr;
    }

    // The node a tile's geometry hangs off, for either dimension. Null is a
    // real answer: the slot is not worn, or its clone has not attached yet.
    RE::NiAVObject* FlashNodeFor(RE::BipedAnim* a_biped, const DyeFlash::Key& a_key) {
        if (!a_biped) {
            return nullptr;
        }
        if (a_key.target == DyeTarget::kArmour) {
            return a_key.slotBit < OS::kBitCount
                       ? a_biped->objects[a_key.slotBit].partClone.get()
                       : nullptr;
        }
        // ⚠ WEAPONS ARE FOUND BY WALKING SLOTS, never by mapping a class to
        // one, and that is the weapon walk's own rule rather than a new one:
        // class does not map onto slot in both directions at once. Greatsword
        // and battleaxe share biped 37, and a one-handed class occupies its own
        // slot in the main hand and biped 9 in the off hand.
        for (std::uint32_t slot = 0;
             slot < static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal); ++slot) {
            if (slot < OS::kBitCount && slot != OS::kBitShield) {
                continue;
            }
            const auto& obj = a_biped->objects[slot];
            const auto  cls = ClassOfWeaponForm(obj.item);
            if (!cls || *cls != a_key.weaponClass) {
                continue;
            }
            if (HandForBipedSlot(*cls, slot) != a_key.weaponHand) {
                continue;
            }
            return obj.partClone.get();
        }
        return nullptr;
    }

    RepaintResult Repaint(RE::Actor* a_actor, const Outfit& a_outfit) {
        if (!a_actor) {
            return {};
        }
        // Never stack a swap on a swap. Repaint runs after every refresh, and
        // recording an already-swapped FacegenTint as the "original" would
        // strand the real material for good. Called BEFORE g_lock is taken:
        // the lock is non-recursive and Restore takes it itself.
        Restore(a_actor);

        // The THIRD-PERSON biped holder, virtual GetBiped1(false). That is the
        // one the world and the editor's own camera show, and the accessor
        // absorbs the AE layout shift that a raw +0x260 would not. The
        // first-person biped is not dyed in tier 1, so styled gauntlets keep
        // their undyed colour in first person until that is addressed.
        //
        // ⚠ Slot-scoped through objects[bit].partClone rather than the actor
        // root on purpose. The root carries the head, the hair head-part and
        // the eyes, all of which the spike turned red, and none of which any
        // armour dye has any business touching.
        auto* const biped = a_actor->GetBiped1(false).get();
        if (!biped) {
            // Drop the shape snapshot with the biped. Every other exit rewrites
            // it wholesale, so this early return is the one path that could
            // leave the editor naming channels after geometry that is no longer
            // there: unequip everything, or walk far enough for the actor to
            // unload, and the popup would keep offering "Cloak" and "Cape" for
            // a body with nothing on it.
            std::scoped_lock l(g_lock);
            g_shapes.erase(a_actor->GetFormID());
            return {};
        }

        // ---- the slots Apparel Preview is standing on (OS-145) ---------------
        //
        // A hover preview swaps the geometry on a slot without changing
        // anything this walk can see: objects[bit].partClone is still a clone,
        // it is still armour, and the stored dye for that bit still resolves.
        // So the channel that coloured your own cuirass colours the previewed
        // piece instead, and the user reads it as Fitting Room dyeing something
        // they never dyed.
        //
        // ⚠ PER SLOT, NEVER THE WHOLE WALK. Standing the walk down for the
        // duration of a preview undresses the rest of the outfit for as long as
        // the mouse rests on an inventory row, which is a worse frame than the
        // bug. Only the bits the preview names are skipped; everything else
        // paints exactly as before.
        //
        // ⚠ PLAYER ONLY, and the gate has to be here rather than inside the
        // walk. Repaint runs on every dyed actor, Apparel Preview previews on
        // the player alone, and a mask read without this test would punch a
        // hole in a follower's dye every time the player hovered a row. The
        // worn-mask shim carries the same restriction for the same reason.
        //
        // ⚠ ZERO WHEN THE SLOTS ARE UNKNOWN. An Apparel Preview too old to send
        // 'APSM' leaves this at zero and the walk behaves exactly as it did
        // before this change - see DyeSkipMask in ApparelPreviewWire.h for why
        // unknown must not mean "skip everything".
        const std::uint32_t previewSkip =
            (a_actor == RE::PlayerCharacter::GetSingleton()) ? ApparelPreviewKnownSlots()
                                                            : std::uint32_t{ 0 };
        if (previewSkip != 0) {
            // ⚠ THE ONLY LINE THAT SAYS THE FIX RAN. Everything else about this
            // is invisible: the previewed piece simply keeps its own colours,
            // which is also what a broken signal, an unregistered listener and
            // an Apparel Preview too old to send 'APSM' all look like. "It
            // stopped happening" cannot tell those apart, so print the mask.
            // Info rather than debug for the same reason - the field log is
            // where this gets confirmed.
            spdlog::info("OutfitDye: Apparel Preview owns slot(s) {:08X} on the player, so those "
                         "bits keep their own colours this walk (OS-145).",
                         previewSkip);
        }

        // ---- the hover dye preview (spec 2026-08-09) -------------------------
        // Snapshot once for the whole pass, the same shape as previewSkip
        // above: the walk must not read a state that can move under it.
        const auto dyePreview = DyePreviewFor(a_actor);

        std::vector<Swapped>   record;
        std::vector<ShapeInfo> shapes;
        // ⚠ ONE ARMOUR ADDON COVERING TWO SLOTS GIVES BOTH BITS THE SAME
        // partClone, so the per-bit walk reaches the same BSGeometry twice.
        // Swapping the same property twice records the FIRST swap's own tint
        // material as the second swap's "original", and the real material is
        // then stranded for good: Restore puts a FacegenTint back and the
        // garment never returns to its own colours. De-dup on the PROPERTY
        // pointer, which is what the swap actually writes through.
        //
        // A consequence worth naming: when two dyed slots share one garment,
        // the lower bit's colour wins. There is one material, so there can be
        // one answer.
        std::unordered_set<RE::BSLightingShaderProperty*> painted;
        // Taken ONCE for the whole pass rather than
        // per slot, so one arming dumps one complete actor rather than one
        // armour slot.
        const bool  dump       = g_passDump.exchange(false, std::memory_order_acq_rel);
        std::size_t seen       = 0;
        std::size_t written    = 0;
        std::size_t skipped    = 0;
        std::size_t shared     = 0;
        std::size_t candidates = 0;
        std::size_t pending    = 0;
        // Read once rather than per shape: the setting is a plain bool behind a
        // singleton and this walk visits every geometry on the actor.
        const bool  census     = Settings::GetSingleton().dyeCensus;
        Census      tally;
        // Which dye path runs. Read once for the same reason as census: this
        // walk visits every geometry on the actor.
        //
        // ⚠ EffectiveDyeRung, NOT dyeSpikeRung. The shine-preserving path is a
        // player setting now ([Dye] bDyeKeepShine, on by default) rather than a
        // spike rung somebody has to know about, and reading the raw key here
        // would leave every default install on the matte material swap.
        const auto  spikeRung      = Settings::GetSingleton().EffectiveDyeRung();
        // Whether the rung above was ASKED FOR by the spike key rather than by
        // bDyeKeepShine. A True PBR shape is refused outright under an explicit
        // spike rung, because a measurement that silently swaps paths on some
        // shapes is not a measurement; under ordinary keep-shine play the same
        // shape DOWNGRADES to the shipped material swap instead (field
        // 2026-08-21 16:29: a whole PG-patched UBE outfit, 22 of 26 shapes,
        // refused as True PBR and the user saw "no color change" - a refusal
        // that covers most of a PG-patched load order is not a guard, it is
        // the dye system turning itself off).
        const bool  spikeExplicit  = Settings::GetSingleton().dyeSpikeRung != 0;
        // ⚠ RESOLVED ONCE PER PASS, NOT PER SHAPE. The INI carries a spelling
        // and DyeTexture owns what it means, so this is the one place the two
        // meet on the paint path; putting the compare inside the geometry walk
        // would run it per shape for an answer that cannot change mid-pass.
        const auto diffuseBlend =
            DyeTexture::BlendFromName(Settings::GetSingleton().dyeBlendName);
        // ⚠ THE PBR SHAPES READ A DIFFERENT TEXTURE, SO THEY GET A DIFFERENT
        // CURVE. Resolved here for the same reason its twin is: the spelling
        // cannot change mid-pass. Which of the two a shape takes is decided in
        // the walk, where the property's flags are in hand. See sDyePbrBlend for
        // the field round that produced it.
        const auto pbrBlend =
            DyeTexture::BlendFromName(Settings::GetSingleton().dyePbrBlendName);
        std::size_t spikeSame      = 0;
        std::size_t spikePbrSkipped = 0;
        std::size_t spikePbrDowngraded = 0;
        // 2026-08-30: the shapes that stayed ON the PBR path instead of being
        // downgraded off it. Counted beside the downgrades rather than folded
        // into spikeSame, because the whole question a field round asks is
        // which of the two a PBR piece took.
        std::size_t spikePbrOnPbr = 0;
        std::size_t spikeNoEmissive  = 0;
        // Rung 2's blocking question. How many PROPERTIES name each emissiveColor
        // allocation, across every shape on this actor and both bipeds.
        //
        // ⚠ THE MAP IS THE MEASUREMENT. A per-shape line alone cannot answer
        // "is it shared", because sharing is a statement about two shapes at
        // once. Counting per pointer is what turns a list of addresses into the
        // yes or no the spike is asking for.
        const bool  emissiveProbe = Settings::GetSingleton().dyeEmissiveProbe;
        std::unordered_map<const void*, std::size_t> emissiveOwners;

        // ⚠ THE OTHER HALF OF THE TILE COLLAPSE IN DyeGrid::Build, and shipping
        // either half alone is worse than shipping neither. A garment whose
        // armature covers two biped slots is attached once per slot and the
        // engine clones the mesh for each, so the two copies hold two different
        // shader properties and can be painted two different colours. Collapse
        // the tile without this and the hidden bit keeps painting its own clone
        // with no control left on screen to fix the mismatch.
        //
        // `part` is the armature's own model, which is what makes the grouping
        // safe rather than a heuristic: one armature owns one model, so two
        // armatures cannot present the same pointer. Taken for every bit with a
        // staged part, clone attached or not - a bit still waiting on its 3D
        // paints nothing either way, and leaving it out of its group would let
        // it become an owner for one pass.
        //
        // ⚠ COMPARED, NEVER DEREFERENCED, and it does not outlive this pass.
        // What the shapes coming out of one traversal belong to.
        struct WalkKey {
            DyeTarget      target{ DyeTarget::kArmour };
            std::uint32_t  slot{ 0 };                   // biped object, both kinds
            std::uint32_t  viaSlot{ 0 };                // kArmour: the group owner
            std::uintptr_t sourceId{ 0 };               // kArmour only
            WeaponClass    cls{ WeaponClass::Sword };   // kWeapon only
            WeaponHand     hand{ WeaponHand::Both };    // kWeapon only
            std::string    label;                       // what the log calls it
            // kHeadPart only: the head-part slot and the part whose geometry
            // this walk is about. Both land on the ShapeInfo, which is what the
            // grid reads a tile's identity from.
            std::uint32_t  headSlot{ 0 };
            StyleRefKey    headPart;
            // ⚠ ONLY GEOMETRY WITH THIS EXACT NAME IS WALKED, and empty means
            // every geometry, which is what armour and weapons pass. It exists
            // because ONE face node holds every head part's geometry at once,
            // where a partClone holds one garment's: the head walk calls this
            // once per part with the part's editor id here, so each part gets its
            // own dye, its own index space and its own ShapeInfo sequence.
            //
            // ⚠ EXACT, NOT FOLDED, because it is the same compare PaintEyeTint
            // ships and the census measured 20 of 20 geometries matching that way
            // (2026-08-17). A case-insensitive match here would dye a shape the
            // eye path refuses, and the two would disagree about one head.
            std::string    nameFilter;
            // ⚠ FALSE ON THE FIRST-PERSON WALK, and it is not cosmetic. See the
            // note on walkBiped: g_shapes feeds the editor's channel labels, so
            // a second walk that also pushed would double every tile in the
            // pane. Painting is the only thing first person contributes.
            bool           snapshot{ true };
        };

        // ---- where the scabbard is, when the dump is armed ------------------
        //
        // ⚠⚠ MEASURES A POINTER RATHER THAN ASSUMING IT. Field 2026-08-14: "the
        // weapon gets dyed, but the sheathe cannot be dyed." A sword NIF carries
        // its scabbard INSIDE it as a node named `Scb` with its own shader
        // property and its own textures (read off two sword meshes on this
        // machine), so at attach time it is under objects[slot].partClone and
        // this walk paints it as an ordinary second shape on its own channel.
        // The suspicion is that a DRAWN weapon is a different arrangement: the
        // engine moves the weapon to the hand and the empty scabbard stays on
        // the hip, and if it does that by REPARENTING `Scb` out of the clone,
        // then the walk cannot see it and the editor never offers a stripe for
        // it. NOBODY HAS MEASURED WHICH, and the fix differs completely between
        // the two, so this says which before anything is built on it.
        //
        // ⚠ ONE RUN, NOT A FRAME LOOP. Armed by the same one-shot the DYEPASS
        // dump uses, which the editor sets when the Dye pane is entered.
        // Read-only: it names nodes and nothing else.
        const auto logScabbards = [](RE::Actor* a_actor, RE::NiAVObject* a_clone,
                                     const std::string& a_label) {
            auto* const root = a_actor ? a_actor->Get3D(false) : nullptr;
            if (!root) {
                return;
            }
            // Descendant test by walking parents, because the clone is a
            // subtree of the same root and "did the walk reach it" is exactly
            // "is it under the clone".
            const auto under = [](RE::NiAVObject* a_node, RE::NiAVObject* a_top) {
                for (auto* p = a_node; p; p = p->parent) {
                    if (p == a_top) {
                        return true;
                    }
                }
                return false;
            };
            int found = 0;
            RE::BSVisit::TraverseScenegraphObjects(
                root, [&](RE::NiAVObject* a_obj) -> RE::BSVisit::BSVisitControl {
                    if (!a_obj || a_obj->name.empty() ||
                        std::string_view{ a_obj->name.c_str() } != "Scb") {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    ++found;
                    auto* const parent = a_obj->parent;
                    auto* const gp     = parent ? parent->parent : nullptr;
                    spdlog::info("SCABBARD: '{}' sees an Scb node under '{}' (its parent's "
                                 "parent is '{}'), underThisClone={}.",
                                 a_label,
                                 parent && parent->name.c_str() ? parent->name.c_str() : "?",
                                 gp && gp->name.c_str() ? gp->name.c_str() : "?",
                                 a_clone && under(a_obj, a_clone));
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            // ⚠ A ZERO IS AN ANSWER TOO, and an unsaid one reads as "the census
            // did not run". A weapon with no scabbard at all (an axe, a mace) is
            // the ordinary reason for it.
            if (found == 0) {
                spdlog::info("SCABBARD: '{}' found no Scb node anywhere on this actor's 3D.",
                             a_label);
            }
        };

        // ---- one attached node, painted -------------------------------------
        //
        // ⚠ SHARED BY THE ARMOUR WALK AND THE WEAPON WALK, and not copied for
        // the second one. The work is identical: traverse the clone's
        // geometries, record a ShapeInfo per shape so the editor can label the
        // channels, and swap the material wherever a channel is set. Only the
        // KEY on the ShapeInfo and the words in the log differ, so those are the
        // only things this takes.
        //
        // A second copy would carry its own version of the property de-dup, the
        // material-type guard that exists for a memory-safety reason, and the
        // census hook, and it would be the copy that stops matching the first.
        // ⚠⚠ a_extraRoot IS PAINTED AS A CONTINUATION OF THE SAME WALK, NOT AS A
        // SECOND WALK, and that is the whole reason it is a parameter here
        // instead of a second paintNode call at the call site. A shape's CHANNEL
        // is its POSITION in this traversal, so two calls would both start at
        // index 0 and the scabbard would fight the weapon for channel 0 while
        // the editor labelled only one of them. One call, one running index, one
        // `painted` set, one ShapeInfo sequence.
        const auto paintNode = [&](RE::NiAVObject* a_node, const OS::SlotDye& a_dyeIn,
                                   const WalkKey& a_key,
                                   RE::NiAVObject* a_extraRoot = nullptr) {
            // The overlay consult (spec 2026-08-09). The substituted copy is
            // what the whole body reads, so a preview on an UNDYED slot makes
            // paint true and the piece previews like any other.
            const OS::SlotDye a_dye = DyePreview::WithPreview(
                a_dyeIn, dyePreview, a_key.target, a_key.viaSlot, a_key.cls, a_key.hand);
            // Whether the substitution above named this walk at all; the
            // per-shape previewOnly below narrows it to the one channel, so
            // only the previewed colour's builds carry the no-commit flag.
            const bool previewHit = DyePreview::Hits(dyePreview, a_key.target, a_key.viaSlot,
                                                     a_key.cls, a_key.hand);
            const bool  paint      = a_dye.Any();
            std::size_t shapeIndex = 0;

            const auto visit =
                [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    // ⚠ SHARED WITH THE FLASH WALK (OS-144). The test used to be
                    // written out here; it moved into CountableShapeProp so the
                    // two walks cannot drift apart, because a shape's channel is
                    // its POSITION in this traversal and a disagreement about
                    // what counts would silently shift every index after it.
                    // The reason the material type is checked at all: the
                    // netimmerse_cast validated the PROPERTY, not its material,
                    // and CopyBaseMembers reads out to 0x78 and IncRefs five
                    // NiPointers on the way, so a plain 0x38-byte
                    // BSShaderMaterial here would be read 0x68 bytes past its
                    // end.
                    // ⚠ BEFORE EVERYTHING, INCLUDING THE INDEX. A geometry this
                    // walk is not about must not consume a channel index or
                    // appear in the snapshot, or the parts of one hair would
                    // renumber each other's channels and every stored colour
                    // would move the first time a player wore a different hair.
                    if (!a_key.nameFilter.empty() &&
                        std::string_view{ a_geom->name.c_str() } != a_key.nameFilter) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const prop = CountableShapeProp(a_geom);
                    if (!prop) {
                        // ⚠ NO LIGHTING PROPERTY, NO STRIPE, ON EVERY TARGET.
                        // A head part briefly earned a row here with its own
                        // reason (kNoMaterial), on the 2026-08-17 call that a
                        // refused shape is listed rather than hidden. The field
                        // reversed it for THIS class the same day, on purpose:
                        // the shapes with no lighting property under a face node
                        // are FSMP's collision proxies (VirtualGround,
                        // VirtualHead, VirtualHairCollision_N), which the player
                        // never installed as a shape and cannot act on. A crossed
                        // strand row teaches that the hair colour paints it; a
                        // crossed collision proxy teaches nothing. Three of one
                        // hair's nine shapes were these.
                        //
                        // ⚠ THE RULE IS THE PROPERTY, NOT THE NAME. Nothing here
                        // reads "Virtual"; a shape with nothing a dye could
                        // write to gets no control, whatever it is called.
                        //
                        // ⚠ HIDDEN FROM THE GRID IS NOT HIDDEN FROM THE LOG. The
                        // head census still prints every such shape as "(no
                        // lighting shader)", so "why can I not dye X" is still
                        // answered from FittingRoom.log.
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    // ⚠ AN INVISIBLE HEAD SHAPE GETS NO STRIPE EITHER, and this
                    // is measured on the asset rather than reasoned. Tullius SMP
                    // Hair 3 overrides KS Hairdo's HDT hairline
                    // (`0_HAIRLINE_Female_Human_Straight`) with a 1874-byte dummy:
                    // BSLightingShaderProperty alpha 0.000 IN THE FILE, an
                    // NiAlphaProperty with blending and testing on, threshold
                    // 255. Nothing it draws can reach the screen, and it read
                    // matAlpha=0.000 in the census while the hair's own visible
                    // hairline read 1.000. A stripe for it is a control over
                    // nothing (field 2026-08-17, "i can edit the hairlines but i
                    // don't see any noticable effect").
                    //
                    // ⚠ BEFORE THE INDEX, like the no-property skip above: a
                    // geometry the walk is not about must not consume a channel.
                    // ⚠ HEAD PARTS ONLY, for now. The same predicate would hold
                    // on armour, but an armour tile's stripes are shipped and
                    // field-proven and nobody has asked; widening it is one
                    // condition, and it should be widened on a report rather
                    // than on symmetry.
                    if (a_key.target == DyeTarget::kHeadPart && ShapeIsInvisible(a_geom, prop)) {
                        if (dump) {
                            spdlog::debug("DYEPASS: {} skips '{}': material alpha {:.3f} "
                                          "under an alpha property, so nothing it draws is "
                                          "visible and it gets no stripe",
                                          a_key.label, a_geom->name.c_str(),
                                          static_cast<RE::BSLightingShaderMaterialBase*>(
                                              prop->material)
                                              ->materialAlpha);
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }

                    const auto feature = prop->material->GetFeature();
                    // ⚠ A BLOOD OVERLAY IS A DYNAMIC DECAL, AND THIS IS MEASURED
                    // RATHER THAN REASONED. Two earlier answers were wrong: the
                    // FEATURE cannot discriminate, because the same EdgeBlood01
                    // reads EnvironmentMap on one weapon and Default on another;
                    // and SLSF2_Weapon_Blood, which is the engine's own NAME for
                    // this geometry and looked certain, reads false on every
                    // single one of them. It is gone from this test as measured
                    // dead weight.
                    //
                    // What actually separates them came out of dumping the whole
                    // flag word and diffing an overlay against the blade beside
                    // it on the same weapon. Both pairs XOR to the same two bits:
                    //
                    //   EdgeBlood01              0x000080008E400301
                    //   2H_Hammer1:0             0x0000800182400301
                    //   EdgeBlood01              0x000000008E400381
                    //   1stPersonVampireSword:0  0x0000800182400381
                    //
                    // kDecal (26) and kDynamicDecal (27), set on every overlay
                    // and clear on every blade. That is not a coincidence of this
                    // load order: a dynamic decal IS geometry the engine paints
                    // onto at runtime, which is what weapon blood is (OS-123).
                    //
                    // ⚠ BOTH BITS, NOT EITHER. kDecal alone is a broader class
                    // than we want to refuse.
                    //
                    // Asked on EVERY shape rather than only on weapons, because
                    // it says what the geometry IS and narrowing it to the
                    // weapon walk would be the arbitrary half of the statement.
                    using PFlag        = RE::BSShaderProperty::EShaderPropertyFlag;
                    const bool decal   = prop->flags.all(PFlag::kDecal, PFlag::kDynamicDecal);
                    // ⚠ THE NAME LIST IS GONE, DELETED ON EVIDENCE RATHER THAN ON
                    // taste. It shipped as the working rule while this flag test
                    // was being measured beside it, and a field run carrying BOTH
                    // answers found ZERO shapes where the names decided anything
                    // the flags missed, and zero the other way. They agreed on
                    // every shape in the session, so `blood` is the flag test.
                    const bool blood   = decal;
                    const bool dyeable = !blood && IsDyeableFeature(feature);
                    const auto index   = shapeIndex++;
                    ++seen;
                    // Before the dyeable filter and before the channel check,
                    // deliberately: the census has to describe every shape the
                    // walk reaches, including the ones the dye path refuses,
                    // because the refused ones are what the spike is about.
                    // ⚠ AND ONLY ON THE SNAPSHOT WALK. The census counts shapes
                    // per feature to size the spike; letting the first-person
                    // walk add its own copies would inflate every column by
                    // however much of the body that biped happens to carry.
                    if (census && a_key.snapshot) {
                        CensusShape(tally, a_geom, prop, feature, a_key.slot);
                    }
                    // ⚠ READ-ONLY, AND BEFORE EVERY FILTER, because rung 2 would
                    // write to the PROPERTY rather than the material and so is
                    // not bound by the dyeable set at all. A shape this walk
                    // refuses to dye is still a shape rung 2 could reach, so
                    // excluding it here would measure the wrong population.
                    //
                    // Counted on BOTH walks, unlike the census: two bipeds naming
                    // one allocation is exactly the sharing this is looking for,
                    // and it is the most likely place to find it.
                    if (emissiveProbe) {
                        const auto* const ec = prop->emissiveColor;
                        ++emissiveOwners[static_cast<const void*>(ec)];
                        // Dereferenced only when non-null, and only read. The
                        // engine reads this same pointer every frame, so a
                        // non-null value here is as safe to read as it is to
                        // render; it is the WRITE the spike is refusing to allow
                        // until this comes back.
                        spdlog::info("DyeEmissive: {} '{}' ptr=0x{:X} mult={:.3f} ownEmit={} "
                                     "rgb={}",
                                     a_key.label, a_geom->name.c_str(),
                                     reinterpret_cast<std::uintptr_t>(ec), prop->emissiveMult,
                                     prop->flags.any(
                                         RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit),
                                     ec ? fmt::format("({:.3f},{:.3f},{:.3f})", ec->red,
                                                      ec->green, ec->blue)
                                        : std::string{ "null" });
                    }
                    // ⚠ slotBit AND sourceId ARE ZEROED ON A WEAPON, not merely
                    // left alone. slotBit means nothing outside the armour bits,
                    // and a non-zero sourceId would enrol the weapon in the
                    // armour clone grouping, where an off-hand weapon's slot
                    // number is the shield's.
                    //
                    // ⚠ THE FIRST-PERSON WALK PAINTS AND RECORDS NOTHING. g_shapes
                    // is what the editor labels its channels from, so a second
                    // walk pushing here would give every tile in the pane a
                    // duplicate set of channels, on geometry the player cannot
                    // see from the editor's own camera (OS-125).
                    if (a_key.snapshot) {
                        // ⚠ THE STRAND REASON IS DIFFERENT ON A HEAD PART, and it
                        // points at a control rather than refusing. A hair's own
                        // strands ARE painted, by the hair colour two tiles up, so
                        // kHair's "dyeing it would streak along the strands"
                        // leaves the player believing nothing can colour them. On
                        // the ARMOUR walk kHair stays exactly as it was: a hood's
                        // strands are worn geometry the hair colour never reaches,
                        // so pointing at that tile there would be a lie.
                        auto reason = blood ? DyeSkipReason::kDecal
                                            : SkipReasonFor(feature);
                        if (a_key.target == DyeTarget::kHeadPart &&
                            reason == DyeSkipReason::kHair) {
                            reason = DyeSkipReason::kHairOwnColour;
                        }
                        shapes.push_back(
                            ShapeInfo{ a_key.target, a_key.cls, a_key.hand,
                                       a_key.target == DyeTarget::kArmour ? a_key.slot : 0u,
                                       a_key.headSlot, a_key.headPart,
                                       index, ReadableShapeName(a_geom->name.c_str()),
                                       dyeable, reason,
                                       feature == RE::BSShaderMaterial::Feature::kEnvironmentMap,
                                       a_key.target == DyeTarget::kArmour ? a_key.sourceId
                                                                          : std::uintptr_t{ 0 } });
                    }
                    // Before every early return
                    // below, so a shape that leaves by any of them still gets a
                    // line. `paint` false means the slot carries no dye at all,
                    // which is the state that currently looks like nothing.
                    if (dump) {
                        const auto  chId = ChannelForShapeIndex(index);
                        const auto& dch  = a_dye.channels[static_cast<std::size_t>(chId)];
                        // `via` is the bit the colour was READ from. It differs
                        // from `slot` exactly when this slot is a clone of a
                        // lower one, so a field report can tell one garment on
                        // two slots from two garments, which is the thing the
                        // eye cannot do: two coincident copies of one mesh
                        // looked correct for the entire life of this feature.
                        //
                        // ⚠ `prop=` WAS HERE AND IS GONE. It was added to prove or
                        // refute one shared BSLightingShaderProperty across both
                        // bipeds, which was the wrong theory for OS-126: the
                        // cause was the teardown asking about one root, and what
                        // settled it was the user saying WHEN it happened rather
                        // than any pointer. Kept out so the line stays readable.
                        spdlog::debug("DYEPASS: {} via {} idx {} chan {} feature {} "
                                      "blood={} dyeable={} slotDyed={} chanSet={} "
                                      "rgb={:02X}{:02X}{:02X} name='{}'",
                                      a_key.label, a_key.viaSlot, index,
                                      static_cast<int>(chId), FeatureName(feature), blood,
                                      dyeable, paint, dch.set, dch.r, dch.g, dch.b,
                                      a_geom->name.c_str());
                    }
                    if (!paint) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (!dyeable) {
                        ++skipped;
                        spdlog::debug("OutfitDye: skip '{}' on {}, feature {}",
                                      a_geom->name.c_str(), a_key.label, FeatureName(feature));
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }

                    const auto  channel = ChannelForShapeIndex(index);
                    const auto& ch = a_dye.channels[static_cast<std::size_t>(channel)];
                    if (!ch.set) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (!painted.insert(prop).second) {
                        ++shared;
                        spdlog::debug("OutfitDye: '{}' on {} shares a property already "
                                      "dyed by a lower slot; leaving it alone.",
                                      a_geom->name.c_str(), a_key.label);
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    ++candidates;

                    // Never mutate the existing material. BSShaderMaterial is
                    // refcounted and deduplicated through a material cache
                    // keyed on hashKey, so writing through this pointer would
                    // tint every actor wearing this armour.
                    Swapped s;
                    // ⚠ THE BLEND GOES HERE, WHERE THE COLOUR BECOMES A TINT,
                    // AND NOT WHERE IT IS READ OUT OF THE OUTFIT. The stored
                    // bytes are the colour the player chose and the pane shows
                    // them as that colour; blending on the way out of storage
                    // would drag the swatch and the hex field toward grey as the
                    // slider moved, which is a different feature.
                    //
                    // ⚠ ONE SITE SERVES EVERY RUNG, which was worth confirming
                    // rather than assuming: rungs 1 and 2 write this value into
                    // a property tint, and rungs 3 and 5 hand the SAME value to
                    // DyeTexture::Acquire as the colour to bake. So the blend
                    // lands once here instead of at each rung's own call.
                    const auto tint =
                        RE::NiColor{ ApplyDyeStrength(ch.r, ch.strength) * kTintScale,
                                     ApplyDyeStrength(ch.g, ch.strength) * kTintScale,
                                     ApplyDyeStrength(ch.b, ch.strength) * kTintScale };
                    // ⚠ THE SECOND STOP IS NARROWED HERE, BESIDE THE TINT, FOR THE
                    // REASON THE TINT ITSELF IS. Strength has to land once, and
                    // the two stops have to land the SAME way or a dye at 60%
                    // would have its first colour weakened and its second not.
                    //
                    // ⚠ AND THE COMPOSITION ORDER IS LOAD BEARING. NarrowStopByte
                    // walks the second stop toward the FIRST, then ApplyDyeStrength
                    // walks the result toward the shader's neutral exactly as it
                    // walks the primary. That order is the only one where strength
                    // 0 still means "no dye at all", which is what every dye that
                    // shipped before this means at 0. Applying the strength first
                    // and narrowing after would leave a special dye at 0 showing
                    // half a ramp between two greys.
                    //
                    // ⚠ THE MODE IS DECLARED HERE, NOT EFFECTIVE. The shape is not
                    // known at this level; SwapToTintedDiffuse resolves it.
                    // ⚠ THE FINISH RESOLVES HERE TOO, beside the ramp, and false
                    // for the override because bDyeFinishOverride exists only as
                    // a comment: the player block has no UI yet, so what renders
                    // is what the DYE declared. First live call site of
                    // EffectiveMaterial; until 2026-08-08 sheen and gloss were
                    // parsed, staged and then read by nothing.
                    const auto fin = EffectiveMaterial(ch, false);
                    DyeTexture::Ramp ramp{};
                    ramp.mode      = ch.mode;
                    ramp.secondSet = ch.secondSet;
                    ramp.gloss     = fin.glossSet ? fin.gloss : std::uint8_t{ 128 };
                    ramp.cut       = ch.cut;  // where metal starts, the piece's own byte
                    if (ch.secondSet) {
                        ramp.r2 = ApplyDyeStrength(
                            DyeRamp::NarrowStopByte(ch.r, ch.r2, ch.strength), ch.strength);
                        ramp.g2 = ApplyDyeStrength(
                            DyeRamp::NarrowStopByte(ch.g, ch.g2, ch.strength), ch.strength);
                        ramp.b2 = ApplyDyeStrength(
                            DyeRamp::NarrowStopByte(ch.b, ch.b2, ch.strength), ch.strength);
                    } else {
                        // No second stop authored: the ramp runs between the
                        // colour and itself, which reduces to flat exactly in both
                        // the arithmetic and the shader. Carried rather than
                        // zeroed so a "mode": "nacre" dye with no hex2 paints its
                        // own colour instead of a ramp down to black.
                        ramp.r2 = ApplyDyeStrength(ch.r, ch.strength);
                        ramp.g2 = ApplyDyeStrength(ch.g, ch.strength);
                        ramp.b2 = ApplyDyeStrength(ch.b, ch.strength);
                    }
                    bool       ok   = false;
                    if (spikeRung != 0) {
                        // ⚠ A PBR SHAPE NEVER TAKES AN EXPLICIT SPIKE RUNG, and
                        // it is logged so the outcome is a finding instead of a
                        // silent gap. ⚠⚠ ORDINARY KEEP-SHINE PLAY IS NO LONGER
                        // COVERED BY THAT SENTENCE: since 2026-08-30 a PBR shape
                        // takes rung 3 like everything else unless
                        // bDyePbrOnPbrPath is off (see the branch below and the
                        // setting's own note). What follows is why the OTHER
                        // rungs stay shut to it, and it is still exactly true of
                        // them. Detected off the PROPERTY's kVertexLighting
                        // flag and never off GetFeature(): Community Shaders
                        // returns kDefault from its own PBR material on purpose,
                        // which is how a PG-patched chest sailed through the
                        // feature gate on 2026-08-21 and rendered white. Rung 1
                        // writes specularColor and specularColorScale, which a
                        // PBR material reads as subsurface colour and roughness
                        // scale; rung 3 swaps the diffuse, which is the one
                        // write a PBR material can take (⚠ the 2026-08-21 white
                        // chest was blamed on this and the blame was never
                        // tested, so bDyePbrOnPbrPath is how it gets tested);
                        // rung 2 writes
                        // the emissive, which nothing has measured there.
                        //
                        // ⚠ WHAT HAPPENS INSTEAD DEPENDS ON WHO ASKED. An
                        // explicit iDyeSpikeRung refuses outright: a
                        // measurement that swaps paths on some shapes is not a
                        // measurement. Ordinary keep-shine play DOWNGRADES to
                        // the shipped material swap: the FacegenTint feature
                        // fails CS's own accepts-feature-0-or-0x13 gate, the
                        // shape leaves the PBR path while dyed and comes back
                        // on restore, which is exactly what rung 0 always did
                        // to these shapes. The alternative was field-run
                        // 2026-08-21 16:29: a whole PG-patched outfit refused,
                        // "no color change", dye dead on most of the load
                        // order.
                        //
                        // ⚠⚠ AND THE DOWNGRADE IS NO LONGER THE ONLY ANSWER
                        // (2026-08-30, the Callisto circlet). It took the
                        // colour and left the piece FLAT, which reached us as
                        // "the circlet loses its cubemap when we dye it". It
                        // never had one: PG Patcher strips slot 4 when it
                        // converts a mesh to True PBR, so once the downgrade
                        // takes the shape off the PBR path its `_rmaos` goes
                        // unread and there is nothing left to shade it with.
                        //
                        // Rung 3 keeps it on the path. The clone goes through
                        // the material's own VIRTUAL Create() and CopyMembers,
                        // so a PBR material clones as a PBR material with its
                        // rmaos, emissive and feature textures carried over,
                        // and the only write is `diffuseTexture` on the base
                        // class. NO FLAG MOVES, so the technique and the
                        // material still agree - which is why this is safer
                        // than the downgrade rather than braver than it: the
                        // 2026-08-21 exit CTD needed a FacegenTint material
                        // under a PBR technique, and that pairing cannot arise
                        // here.
                        //
                        // ⚠ RUNG 5 ONLY. At rung 0 to 2 the write is
                        // specularColor, which a PBR material reads as
                        // SUBSURFACE COLOUR, so a PBR shape must keep taking
                        // the downgrade whenever keep-shine is off.
                        const bool pbrShape = prop->flags.any(
                            RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting);
                        const bool pbrOnPbrPath =
                            pbrShape && !spikeExplicit && spikeRung >= 5 &&
                            Settings::GetSingleton().dyePbrOnPbrPath;
                        if (pbrShape && pbrOnPbrPath) {
                            ++spikePbrOnPbr;
                            // ⚠ THE BLEND IS ON THE LINE BECAUSE IT IS THE ONE
                            // THING A FIELD ROUND CAN TURN. A PBR piece that
                            // comes back paler than the set beside it is the
                            // curve meeting a flatter source map, and the next
                            // question is always "which curve did it take".
                            spdlog::info("DyeSpike: '{}' on {} is True PBR "
                                         "(kVertexLighting); dyed ON the PBR path through "
                                         "blend '{}' (sDyePbrBlend), so it keeps its own "
                                         "shading.",
                                         a_geom->name.c_str(), a_key.label,
                                         DyeTexture::BlendName(
                                             DyeTexture::ResolveDyeBlend(ch.blend, pbrBlend)));
                        }
                        if (pbrShape && !pbrOnPbrPath) {
                            if (spikeExplicit) {
                                ++spikePbrSkipped;
                                spdlog::info("DyeSpike: '{}' on {} is True PBR "
                                             "(kVertexLighting); rung {} refuses it rather "
                                             "than mis-colour a shape the PBR shader draws.",
                                             a_geom->name.c_str(), a_key.label, spikeRung);
                                painted.erase(prop);
                                return RE::BSVisit::BSVisitControl::kContinue;
                            }
                            ++spikePbrDowngraded;
                            spdlog::info("DyeSpike: '{}' on {} is True PBR (kVertexLighting); "
                                         "keep-shine downgrades it to the shipped material "
                                         "swap, so it dyes classic and leaves the PBR path "
                                         "until the dye is removed.",
                                         a_geom->name.c_str(), a_key.label);
                            ok = SwapToTintMaterial(a_geom, prop, tint, s);
                        } else if (spikeRung >= 5) {
                            bool wait = false;
                            // The actor is the waiter: a build that completes
                            // queues QueueRepaint for whoever asked, which is
                            // what actually lands the swap. Polling on `pending`
                            // alone expired before the texture existed.
                            // ⚠ THE ONLY CALLER THAT NAMES sDyeBlend, and it has
                            // to name a_capPx too because the blend sits behind
                            // it. 0 is the value the default argument was
                            // already supplying, so the cap is unchanged.
                            //
                            // ⚠⚠ THE DYE'S OWN CHOICE OVERRIDES THE INSTALL'S,
                            // AND ITS ZERO DEFERS. diffuseBlend is what
                            // sDyeBlend resolved to for this pass, so a channel
                            // that names nothing - every channel in every save
                            // written before v19, and every one of the 318
                            // shipped dyes - lands on exactly the curve it
                            // already landed on. Resolved PER SHAPE rather than
                            // per pass because the choice is per channel, which
                            // is the one thing that cannot be hoisted out of
                            // this walk.
                            ok = SwapToTintedDiffuse(a_geom, prop, tint, spikeRung == 5, s,
                                                     wait,
                                                     a_actor ? a_actor->GetHandle()
                                                             : RE::ActorHandle{},
                                                     ramp, fin, ch.flake, ch.strength,
                                                     previewHit &&
                                                         static_cast<std::size_t>(channel) ==
                                                             dyePreview.key.channel,
                                                     0,
                                                     DyeTexture::ResolveDyeBlend(
                                                         ch.blend,
                                                         pbrShape ? pbrBlend : diffuseBlend));
                            // ⚠ THE COLOUR IS ON BOTH LINES BECAUSE THE FIRST
                            // FIELD RUN COULD NOT TELL TWO FAULTS APART. Five
                            // textures were built and every one of them carried
                            // the SAME rgb, across 48 swaps, while the user was
                            // clicking swatches that should have produced other
                            // colours. That is either "the walk never saw the
                            // new colour" or "it saw it and the cache handed
                            // back the old texture", and nothing logged then
                            // could separate them. Printing what the walk was
                            // ASKED for, beside what DyeTexture reports building,
                            // makes the next run decide it in one line.
                            if (wait) {
                                // ⚠ PENDING, NOT FAILED, AND THE DIFFERENCE IS
                                // THE WHOLE DESIGN. The texture is being built on
                                // the render thread and this walk is on the game
                                // thread, so the only honest answer this pass is
                                // "not yet". Counting it here re-arms the
                                // deferred chain that already exists for a late
                                // partClone, and the next link finds a cache hit.
                                ++pending;
                                // ⚠ THE CHOSEN COLOUR, WITH THE STRENGTH BESIDE
                                // IT, NEVER THE BLENDED ONE. A field log has to
                                // separate "the player picked a dark red" from
                                // "the player picked a red at 20%", and the
                                // blended value alone cannot say which.
                                spdlog::info("DyeSpike: rung 3 WAITING on a texture for '{}' "
                                             "on {} rgb={:02X}{:02X}{:02X} strength={} "
                                             "(diffuse {})",
                                             a_geom->name.c_str(), a_key.label, ch.r, ch.g,
                                             ch.b, ch.strength,
                                             ok ? "already swapped" : "not yet");
                            }
                            // ⚠ NOT `else if`, AND THE REFLECTION MASK IS WHY.
                            // Since the mask can miss the cache while the
                            // diffuse hits it, a shape can be SWAPPED and still
                            // owed a texture. It has to be recorded either way:
                            // treating a wait as "nothing happened" would drop
                            // the record and strand the displaced material with
                            // nothing left that knows how to put it back.
                            if (ok) {
                                ++spikeSame;
                                spdlog::info("DyeSpike: rung 3 {} '{}' on {}, feature {} kept, "
                                             "rgb={:02X}{:02X}{:02X} strength={}",
                                             spikeRung == 5 ? "re-diffused"
                                                            : "cloned (control, no write)",
                                             a_geom->name.c_str(), a_key.label,
                                             FeatureName(feature), ch.r, ch.g, ch.b,
                                             ch.strength);
                            }
                        } else if (spikeRung <= 2) {
                            ok = SwapToSameFeatureTint(a_geom, prop, tint, spikeRung == 1, s);
                            if (ok) {
                                ++spikeSame;
                                spdlog::info(
                                    "DyeSpike: rung 1 {} '{}' on {}, feature {} kept",
                                    spikeRung == 1 ? "tinted" : "cloned (control, no write)",
                                    a_geom->name.c_str(), a_key.label, FeatureName(feature));
                            }
                        } else {
                            ok = WriteEmissiveTint(a_geom, prop, tint, spikeRung == 3, s);
                            if (ok) {
                                ++spikeSame;
                                spdlog::info("DyeSpike: rung 2 {} '{}' on {}, feature {} "
                                             "untouched",
                                             spikeRung == 3 ? "emissive-tinted"
                                                            : "recorded (control, no write)",
                                             a_geom->name.c_str(), a_key.label,
                                             FeatureName(feature));
                            } else {
                                ++spikeNoEmissive;
                            }
                        }
                    } else {
                        // Never mutate the existing material. BSShaderMaterial is
                        // refcounted and deduplicated through a material cache
                        // keyed on hashKey, so writing through this pointer would
                        // tint every actor wearing this armour.
                        ok = SwapToTintMaterial(a_geom, prop, tint, s);
                    }
                    if (!ok) {
                        painted.erase(prop);
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    record.push_back(std::move(s));
                    ++written;
                    if (dump) {
                        spdlog::debug("DYEPASS:   -> WROTE {} idx {} '{}'", a_key.label, index,
                                      a_geom->name.c_str());
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                };

            RE::BSVisit::TraverseScenegraphGeometries(a_node, visit);
            // ⚠ AFTER THE CLONE AND NEVER BEFORE. The clone's shapes have to keep
            // the indices they have always had, or every saved weapon colour on
            // every outfit would move to a different part of the mesh the first
            // time a player loaded a save with this build.
            if (a_extraRoot) {
                RE::BSVisit::TraverseScenegraphGeometries(a_extraRoot, visit);
            }
        };

        // ---- one biped holder, walked -----------------------------------------
        //
        // ⚠ CALLED TWICE AND THE SECOND CALL IS NOT A COPY OF THE FIRST. The
        // third-person biped is walked with a_snapshot true, the first-person one
        // with it false, and everything else about the two passes is identical
        // (OS-125). Extracted rather than duplicated for the same reason
        // paintNode is shared between the armour and weapon loops inside it: a
        // second copy would grow its own version of the clone grouping, the
        // pending classification and the shared-property de-dup, and it would be
        // the copy that stops matching.
        //
        // ⚠ BOTH PASSES PUSH INTO THE SAME `record`. g_swapped is keyed by actor
        // FormID and holds ONE vector, and Restore runs before every Repaint, so
        // a second biped's swaps have to join that vector or Restore puts half
        // the actor back and leaves the other half wearing a tint material with
        // nothing left that knows how to undo it.
        //
        // ⚠ `painted` IS SHARED ACROSS BOTH PASSES TOO, and that is what makes it
        // safe rather than what makes it risky. If the two bipeds ever hand back
        // the same BSLightingShaderProperty, the second pass would otherwise
        // record the first pass's OWN tint material as the original and strand
        // the real one for good.
        //
        // ⚠ THE FIRST-PERSON PASS DOES NOT FEED `pending`. A staged slot with no
        // clone on that biped would otherwise arm the retry chain and burn its
        // budget on geometry the editor never labels, which is exactly the false
        // "still waiting for their 3D" that OS-116 was closed to stop.
        const auto walkBiped = [&](RE::BipedAnim* a_biped, bool a_snapshot) {
        // ⚠ RECOMPUTED PER BIPED, never hoisted. The clone grouping is a
        // statement about THIS holder's armatures: the first-person biped
        // carries a different set of slots (FPPROBE measured it missing 0, 7 and
        // 8), so owners derived from the third-person one would map its bits
        // onto groups that do not exist here.
        std::array<std::uintptr_t, OS::kBitCount> sources{};
        for (std::uint32_t bit = 0; bit < OS::kBitCount; ++bit) {
            sources[bit] = reinterpret_cast<std::uintptr_t>(a_biped->objects[bit].part);
        }
        const auto owners = OS::DyeCloneOwnersFrom(sources);

        for (std::uint32_t bit = 0; bit < OS::kBitCount; ++bit) {
            // ⚠ FIRST STATEMENT IN THE LOOP, AHEAD OF EVERYTHING (OS-145). A
            // previewed slot must not reach the pending classification either:
            // counting it as staged-but-uncloned would arm the retry chain and
            // spend its budget waiting for a clone that belongs to Apparel
            // Preview, which is the false "still waiting for their 3D" OS-116
            // was closed to stop. Skipping before the read makes that
            // impossible rather than making the next branch remember.
            if ((previewSkip & (std::uint32_t{ 1 } << bit)) != 0) {
                continue;
            }
            const auto& obj  = a_biped->objects[bit];
            auto* const node = obj.partClone.get();
            // Resolved through the group rather than read off this bit, so both
            // clones of one garment take one colour. Every bit in a group
            // resolves to the same value, which is the whole point: the two
            // properties get the same write and the copies stop disagreeing.
            //
            // A COPY, and nothing here writes back. CollapseCloneDyes is what
            // tidies the stored outfit, and it runs in the editor where there is
            // an Apply to carry it; a garment has to render correctly before and
            // without that, so the paint path resolves instead of migrating.
            const auto dye = OS::ResolvedSlotDye(a_outfit, owners, bit);
            // Slots with no dye are still WALKED, and only walked: nothing is
            // written on them. The editor has to label the channels of a slot
            // the user has not dyed yet, which is exactly the moment they are
            // choosing a colour, and the snapshot is the only way it can read
            // shape names without walking the scenegraph off the present
            // thread.
            //
            // Cost is one traversal per WORN armour slot, not one per actor:
            // this loop runs 32 roots and skips every bit whose partClone is
            // null. The field run saw 49 geometries across the whole actor, of
            // which the worn armour clones held 36.
            const bool paint = dye.Any();

            // ⚠ ASKED BEFORE THE CLONE, not after. An off-hand weapon in biped
            // 9 stages .item and .addon and NEVER fills .partClone, so the null
            // test below would read it as "the attach has not run yet" and the
            // chain would spend its whole budget waiting for a clone that is not
            // coming (OS-111).
            if (DyeGate::ClassifySlot(paint, node != nullptr,
                                      obj.addon != nullptr || obj.part != nullptr,
                                      OccupantIsForeign(bit, obj.item),
                                      owners[bit] == bit) ==
                DyeGate::SlotPaintState::kForeignOccupant) {
                // One synthetic shape so the tile exists and can say why, rather
                // than the slot vanishing from the grid and the stored colour
                // reading as orphaned. There is no geometry to name, so the name
                // is empty and the reason carries the whole message.
                //
                // ⚠ sourceId IS LEFT DEFAULTED, and it must stay that way. This
                // aggregate is one field short of the ones below because a
                // foreign occupant has no source model to identify, and Outfit.h
                // gives zero a meaning: an unidentified slot groups with no
                // other, including with another unidentified one. Filling this
                // in with anything would enrol a torch in OS-112's clone
                // grouping and hand its colour to a real garment.
                //
                // ⚠ A WEAPON GETS NOTHING HERE NOW. It has a real tile from the
                // weapon walk, so emitting this one too puts a second, dead
                // Shield square beside it whose tooltip says weapons cannot be
                // dyed, which stopped being true when weapon dye shipped
                // (OS-120). A torch still earns one: it genuinely never dyes,
                // and without the tile its slot leaves the grid with the
                // shield's colour still stored behind it.
                const auto reason = ForeignReasonFor(obj.item);
                if (a_snapshot && ForeignOccupantEarnsATile(reason)) {
                    // ⚠ THE TWO HEAD FIELDS ARE SPELLED OUT HERE AS ZEROES, and
                    // the compiler is what asked for them: ShapeInfo gained
                    // headSlot and headPart in the MIDDLE rather than at the end,
                    // exactly so a positional caller like this one breaks the
                    // build instead of silently reading a dye key as an index.
                    shapes.push_back(ShapeInfo{ DyeTarget::kArmour, WeaponClass::Sword,
                                                WeaponHand::Both, bit, 0u, StyleRefKey{},
                                                0, std::string{},
                                                false, reason, false });
                }
                spdlog::debug("OutfitDye: slot {} holds something that is not armour, so its "
                              "dye is not painted and no clone is waited for. tile={}",
                              bit, ForeignOccupantEarnsATile(reason));
                continue;
            }

            if (!node) {
                // ⚠ A NULL partClone IS TWO ANSWERS, AND THE OLD CODE READ BOTH
                // AS "NOTHING TO DYE". BipedPost.h records the measurement this
                // rests on: 15500 writes .item, .addon and .part synchronously
                // on the worn-pass stack, and the 3D attach that fills
                // .partClone runs DEFERRED off BSTaskPool. Every dump taken
                // there showed partClone == 0x0, which is the whole reason
                // BipedPost::QueueNodeCull exists. Reading it synchronously
                // after UpdateEquipment therefore MISSES any slot whose model
                // came off disk rather than the cache - silently, because the
                // slot looked identical to one the actor is not wearing.
                //
                // Staged-but-not-cloned is counted, not skipped: the caller
                // re-arms on it (QueueRepaint), and it is the number the field
                // log needs to tell a missing dye apart from a dye on an unworn
                // slot.
                //
                // Foreign is passed false here rather than re-asked: the branch
                // above consumed that case and continued, so anything reaching
                // this point is a slot whose occupant this walk genuinely owns
                // (OS-111).
                //
                // ⚠ owners[bit] == bit IS LOAD-BEARING, and it is here because
                // leaving it out made this fire 779 times in one session and put
                // 60 false warnings in the field log. A multi-slot garment stages
                // its model on every slot it covers and the engine does not
                // always clone it onto each: a cuirass on 32 and 34 attached one
                // clone on the chest and left the forearm bit staged with none,
                // for the whole session. Since OS-112 that forearm bit resolves
                // its colour through the chest bit, so it reads as dyed, and
                // without this it waits forever for geometry that is already on
                // screen under a different slot (OS-116).
                //
                // ⚠ AND ONLY ON THE SNAPSHOT WALK. `pending` is what re-arms the
                // retry chain, and a first-person slot that is staged with no
                // clone would spend that whole budget on geometry the editor
                // never labels, then print the warning that says a garment is
                // rendering undyed. Sixty false ones of exactly that line is
                // what OS-116 was closed to stop (OS-125).
                if (a_snapshot &&
                    DyeGate::ClassifySlot(paint, false,
                                          obj.addon != nullptr || obj.part != nullptr,
                                          false, owners[bit] == bit) ==
                    DyeGate::SlotPaintState::kClonePending) {
                    ++pending;
                    spdlog::debug("OutfitDye: slot {} is dyed and its geometry is staged, but "
                                  "the partClone has not been attached yet.",
                                  bit);
                }
                continue;
            }

            WalkKey key;
            key.target   = DyeTarget::kArmour;
            key.slot     = bit;
            key.viaSlot  = owners[bit];
            key.sourceId = sources[bit];
            key.label    = std::string{ a_snapshot ? "" : "fp " } + "slot " + std::to_string(bit);
            key.snapshot = a_snapshot;
            paintNode(node, dye, key);
        }

        // ---- weapons --------------------------------------------------------
        //
        // ⚠ THE WALK IS OVER SLOTS AND NEVER OVER CLASSES, and the reason is
        // that class does not map one to one onto slot IN BOTH DIRECTIONS AT
        // ONCE. Greatsword and battleaxe share biped 37; arrows and bolts share
        // biped 41, so two classes want one slot. A one-handed class occupies
        // its own slot in the main hand and biped 9 in the off hand, so one
        // class wants two slots. Walking classes and looking up slots has to
        // resolve that ambiguity on every pass; walking slots does not, because
        // the engine has already answered it: objects[slot].item names the real
        // form, ClassOfWeaponForm derives the class, HandForBipedSlot derives
        // the hand.
        //
        // ⚠ BIPED 9 IS SHARED WITH THE SHIELD AND THE TWO PARTITION IT BY FORM
        // TYPE. Weapon dye claims a TESObjectWEAP there, shield dye claims a
        // TESObjectARMO, and a torch is a TESObjectLIGH and belongs to neither.
        // The armour walk above already refuses a non-armour occupant of that
        // slot (OS-111), so the two never both paint it.
        //
        // ⚠ 39 IS SPELLED TWICE IN THIS CODEBASE AND MEANS TWO DIFFERENT
        // THINGS. Editor slot 39 is the shield, which is biped object 9. Biped
        // object 39 is the staff. Neither mistake crashes or logs.
        for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(
                                                RE::BIPED_OBJECTS::kTotal); ++slot) {
            // The armour bits own everything below 32 except the off-hand's
            // share of the shield slot. kBitShield is BitForEditorSlot(39) and
            // biped object 9 is the same number, which is the one place the two
            // index spaces genuinely overlap.
            if (slot < OS::kBitCount && slot != OS::kBitShield) {
                continue;
            }
            // ⚠ THE PREVIEW SKIP REACHES BIPED 9 TOO, AND ONLY BIPED 9. Apparel
            // Preview previews armour, so every bit it can name is below 32,
            // and biped 9 is the single index the two walks share. Guarded on
            // kBitCount rather than shifted blind: this loop runs past 31 and a
            // shift that wide is undefined, however harmless the answer looks.
            if (slot < OS::kBitCount && (previewSkip & (std::uint32_t{ 1 } << slot)) != 0) {
                continue;
            }
            const auto& obj = a_biped->objects[slot];
            const auto  cls = ClassOfWeaponForm(obj.item);
            if (!cls) {
                continue;  // empty, or something this mod does not style
            }
            // On biped 9 specifically, ClassOfWeaponForm returning a class is
            // already the WEAP test that partitions the slot: a shield is an
            // ARMO and a torch is a LIGH, and neither yields one.
            const auto hand = HandForBipedSlot(*cls, slot);
            const auto dye  = a_outfit.ResolvedWeaponDyeFor(*cls, hand);

            auto* const node = obj.partClone.get();
            if (!node) {
                // Same three-way reading as the armour side, minus the clone
                // grouping, which cannot apply: a weapon is one object in one
                // slot, so it always owns its own clone if it has one. Foreign
                // is false because the form test above already passed.
                // Snapshot walk only, same reason as the armour side: the
                // first-person biped must not drive the retry budget.
                if (a_snapshot &&
                    DyeGate::ClassifySlot(dye.Any(), false,
                                          obj.addon != nullptr || obj.part != nullptr,
                                          false, true) ==
                    DyeGate::SlotPaintState::kClonePending) {
                    ++pending;
                    spdlog::debug("OutfitDye: weapon slot {} is dyed and its geometry is "
                                  "staged, but the partClone has not been attached yet.",
                                  slot);
                }
                continue;
            }

            WalkKey key;
            key.target  = DyeTarget::kWeapon;
            key.slot    = slot;
            key.viaSlot = slot;  // no grouping: a weapon is always its own owner
            key.cls     = *cls;
            key.hand    = hand;
            key.label   = std::string{ a_snapshot ? "" : "fp " } + "weapon " +
                        ClassJsonName(*cls) + "/" + HandJsonName(hand) + " (biped " +
                        std::to_string(slot) + ")";
            key.snapshot = a_snapshot;
            // ⚠ THE SCABBARD IS ONLY EVER THE WEAPON'S OWN. Resolved from this
            // clone's parent rather than by searching the actor, so a dual wield
            // paints two scabbards with two colours instead of painting whichever
            // one the traversal reached first with both.
            auto* const scabbard = ScabbardFor(node);
            paintNode(node, dye, key, scabbard);
            // After the paint, so the DYEPASS lines for this weapon and the
            // census of its scabbard sit together in the log and can be read as
            // one answer: what the walk reached, then where the scabbard was.
            if (dump && a_snapshot) {
                logScabbards(a_actor, node, key.label);
                spdlog::info("SCABBARD: '{}' resolved its own scabbard through the clone's "
                             "parent '{}': {}.",
                             key.label,
                             node->parent && node->parent->name.c_str() ? node->parent->name.c_str()
                                                                        : "?",
                             scabbard ? "found, and it is painted as a shape of this weapon"
                                      : "none, which is right for a drawn weapon or a "
                                        "weapon that has no scabbard");
            }
        }
        };  // walkBiped

        walkBiped(biped, true);

        // ⚠ THE SECOND WALK, AND THE GUARD IN FRONT OF IT IS NOT DEFENSIVE
        // PADDING. TESObjectREFR::GetBiped1 ignores its flag in the base
        // implementation and returns GetBiped2(), so on an actor with no separate
        // first-person biped this would walk the third-person geometry a second
        // time: every shape painted, then revisited and counted again. FPPROBE
        // measured distinct=true on the player, which is what makes first person
        // reachable at all, and the player is not the only actor Repaint runs on.
        //
        // A follower has no first-person biped and simply skips this (OS-125).
        if (auto* const fpBiped = a_actor->GetBiped1(true).get();
            DyeGate::ShouldWalkSecondBiped(biped, fpBiped)) {
            walkBiped(fpBiped, false);
        }

        // ---- head parts: horns, ears, and a hair's non-strand shapes ---------
        //
        // ⚠⚠ THE SAME paintNode THE ARMOUR AND WEAPON WALKS USE, called once per
        // PART with that part's name as the filter. Nothing about the paint is
        // written twice: the rung selection, the preview consult, the property
        // de-dup, the pending classification, the swap record and the restore
        // contract are all the field-proven body above. A second painter here is
        // exactly the thing that drifts, and the shine is how it would show:
        // Settings::EffectiveDyeRung defaults to the keep-the-shine path, so a
        // head walk with its own swap call would leave a dyed horn matte beside
        // dyed armour that kept its reflection, on one character, from one click.
        //
        // ⚠ ONE CALL PER PART, NOT ONE PER FACE NODE, and the name filter is what
        // makes that work. A partClone holds one garment; the face node holds
        // EVERY head part at once, so one call would give nine shapes of one hair
        // a single dye and a single index space. Per part they each get their own
        // colour, their own channel 0 and their own ShapeInfo.
        //
        // ⚠ THE COST IS A TRAVERSAL PER WORN PART, measured small: the field
        // census found 20 geometries and about a dozen parts, so this is a few
        // hundred pointer reads on a pass that already walks 32 armour roots.
        //
        // ⚠ SCOPED TO THE HAIR SLOT AND THE INVENTED SLOTS, which is the whole
        // of what was asked for (horns, ears, the shapes a modded hair carries
        // that are not strands). Face, mouth, brows, lashes and scars are left
        // out: every one of them is refused by the feature guard anyway, so
        // including them would add dead rows saying "this carries your
        // character's own colouring" to a grid that is about clothes and horns.
        //
        // ⚠⚠ AND THE EYES SLOT IS EXCLUDED DELIBERATELY. PaintEyeTint owns it,
        // through a different path for a measured reason (a glowing eye is tinted
        // through its emissive alone and its material is never touched). Two
        // painters over one geometry is the drift this whole comment is about, and
        // here it would be two painters that disagree by design.
        // ⚠⚠ EVERY ACTOR WITH A HEAD, NOT THE PLAYER ALONE (field 2026-09-04,
        // "only 1 hair dye slot for npcs even if their hair is multi shaped").
        // This walk is also the snapshot the grid draws its hair tile from, so
        // gating it on IsPlayerRef gave a follower ONE stripe, her hair colour,
        // however many ornaments her style carried. Her head is walked on the
        // same terms as his, with one difference the names force:
        //
        // ⚠⚠ HER OWN PIECES WEAR ENGINE NAMES, OURS DO NOT. The engine stamps a
        // head part's geometry with the part's editor id, hers and ours alike,
        // which is why NpcHair renames every root it attaches to
        // NpcHairNames::OwnRootName (OS-246, the bald follower). So a style she
        // wears through us is found by the roots NpcHair recorded and walked
        // WHOLE, one root per part, because the shapes under such a root keep
        // the nif's own names and the exact-name filter would skip every one
        // of them. Her own hair, and the invented slots, are walked by editor
        // id exactly as the player's are. One key per part either way, so a
        // follower's ornament stores under the same (slot, part) a player's
        // does, and the codec has carried it since LIBR v22.
        {
            auto* const npcBase = a_actor->GetActorBase();
            auto* const proc    = a_actor->GetActorRuntimeData().currentProcess;
            auto* const mid     = proc ? proc->middleHigh : nullptr;
            auto* const faceNode = mid ? mid->faceNodeSkinned : nullptr;
            if (npcBase && faceNode) {
                std::size_t headParts = 0;
                std::size_t headDyed  = 0;
                std::size_t ownRoots  = 0;
                // a_root null: the engine-named geometry under the face node, by
                // exact name. a_root set: one of OUR roots, walked whole.
                const auto walkPart = [&](std::uint32_t a_slot, RE::BGSHeadPart* a_part,
                                          RE::NiAVObject* a_root) {
                    if (!a_part) {
                        return;
                    }
                    const char* const edid = a_part->GetFormEditorID();
                    if (!edid || !*edid) {
                        return;  // no name, so the engine's stamp cannot be matched
                    }
                    StyleRefKey ref;
                    if (!StyleRef::Make(a_part, ref)) {
                        // A part with no defining file cannot be named in a way
                        // the next load could resolve, which is the same answer
                        // HeadPart::SnapshotCaptures gives to the same question.
                        // It is still WALKED for the snapshot below, with an empty
                        // ref, so the grid can show the shape and say nothing
                        // about colouring it.
                        ref = StyleRefKey{};
                    }
                    ++headParts;
                    WalkKey key;
                    key.target     = DyeTarget::kHeadPart;
                    key.headSlot   = a_slot;
                    key.headPart   = ref;
                    key.nameFilter = a_root ? std::string{} : std::string{ edid };
                    key.label      = fmt::format("head part {} '{}'{}", a_slot, edid,
                                                 a_root ? " (ours)" : "");
                    key.snapshot   = true;
                    // ⚠ AN EMPTY REF READS AS NO COLOUR RATHER THAN AS A LOOKUP,
                    // which HeadPartDyeFor already answers correctly: nothing is
                    // ever stored under an empty key, so this returns the shared
                    // empty and the part is walked for its snapshot alone.
                    const auto& dye = a_outfit.HeadPartDyeFor(a_slot, ref);
                    if (dye.Any()) {
                        ++headDyed;
                    }
                    paintNode(a_root ? a_root : static_cast<RE::NiAVObject*>(faceNode), dye,
                              key);
                };
                const auto walkSlot = [&](std::uint32_t a_slot) {
                    auto* const part = npcBase->GetCurrentHeadPartByType(
                        static_cast<RE::BGSHeadPart::HeadPartType>(a_slot));
                    if (!part) {
                        return;
                    }
                    walkPart(a_slot, part, nullptr);
                    // ⚠ THE EXTRA PARTS ARE THE FEATURE, not a completeness
                    // gesture. Measured 2026-08-17: the hair part itself carries
                    // model='' and contributes NO geometry, and every dyeable
                    // shape on that hair was an extra part (`ACC`, `001ACC`,
                    // `GuanYinping2`, `24_bodyc_0.6_0_0`). A walk of parents alone
                    // would find nothing at all on the case that was asked for.
                    for (auto* const extra : part->extraParts) {
                        walkPart(a_slot, extra, nullptr);
                    }
                };
                const auto hairSlot =
                    static_cast<std::uint32_t>(RE::BGSHeadPart::HeadPartType::kHair);
                // The hair slot: OURS when NpcHair put a style on her, else the
                // record's own. The player's hair is never ours (HairStyle.h
                // writes his actor base and the engine builds the head), so his
                // walk is the editor-id one, unchanged.
                auto* const ours = a_actor->IsPlayerRef()
                                       ? nullptr
                                       : NpcHair::AppliedTo(a_actor, HeadPart::Kind::kHair);
                if (ours) {
                    const auto partNamed = [&](std::string_view a_edid) -> RE::BGSHeadPart* {
                        const auto wears = [&](RE::BGSHeadPart* a_p) {
                            const char* const id = a_p ? a_p->GetFormEditorID() : nullptr;
                            return id && a_edid == id;
                        };
                        if (wears(ours)) {
                            return ours;
                        }
                        for (auto* const extra : ours->extraParts) {
                            if (wears(extra)) {
                                return extra;
                            }
                        }
                        return nullptr;
                    };
                    for (const auto& name :
                         NpcHair::AttachedRootNames(a_actor, HeadPart::Kind::kHair)) {
                        const auto  edid = NpcHairNames::EdidOfOwnRoot(name);
                        auto* const part = edid ? partNamed(*edid) : nullptr;
                        auto* const root =
                            faceNode->GetObjectByName(RE::BSFixedString(name.c_str()));
                        if (!part || !root) {
                            // Recorded but not walkable: a root the engine named
                            // by something other than a part of the style (a
                            // reused root keeps its engine name, NpcHair says
                            // so), or one no longer under her face node. Named,
                            // so a follower whose ornament will not dye has a
                            // line in the log.
                            spdlog::debug("HeadDye: actor {:08X} recorded root '{}' is not "
                                          "walked: {}.",
                                          a_actor->GetFormID(), name,
                                          !part ? "no part of the style wears that name"
                                                : "it is not under her face node");
                            continue;
                        }
                        ++ownRoots;
                        walkPart(hairSlot, part, root);
                    }
                } else {
                    walkSlot(hairSlot);
                }
                for (const auto& found : HeadPart::DiscoveredSlots()) {
                    walkSlot(found.type);
                }
                // ⚠ THE FUNNEL SAYS WHAT IT WAS ASKED FOR, before any verdict,
                // for PaintEyeTint's own recorded reason: a pass that is silent
                // on the path it takes most often leaves "I dyed a horn and
                // nothing happened" with no line to separate "the walk never
                // ran" from "it ran and refused". The actor is named now that
                // there is more than one.
                spdlog::info("HeadDye: actor {:08X} walked {} head part(s) on {} slot(s), {} "
                             "of them carrying a colour this outfit set{}.",
                             a_actor->GetFormID(), headParts,
                             1 + HeadPart::DiscoveredSlots().size(), headDyed,
                             ownRoots ? fmt::format(", {} through Fitting Room's own hair "
                                                    "root(s)",
                                                    ownRoots)
                                      : std::string{});
            } else {
                spdlog::debug("HeadDye: actor {:08X} has no face node or no base yet, so no "
                              "head part can be dyed this pass.",
                              a_actor->GetFormID());
            }
        }

        if (census) {
            std::string features;
            for (std::size_t f = 0; f < tally.byFeature.size(); ++f) {
                if (tally.byFeature[f] == 0) {
                    continue;
                }
                if (!features.empty()) {
                    features += ", ";
                }
                features += fmt::format(
                    "{}={}", FeatureName(static_cast<RE::BSShaderMaterial::Feature>(f)),
                    tally.byFeature[f]);
            }
            // ⚠ A ZERO IS NOT AN ANSWER ON ITS OWN. Nothing worn being PBR
            // means this load order has no True PBR armour on this character
            // right now, not that the PBR question is settled for players. The
            // same goes for vertex colours. Say so here rather than let a
            // reader take a zero out of the log as a verdict.
            spdlog::info("DyeCensus: actor {:08X}, {} shape(s) across the armour walk. "
                         "pbr(kVertexLighting)={} specular(kSpecular)={} vertexColours={} "
                         "dynamicTriShape={}. Features: {}. A zero above means this "
                         "character is not wearing any, NOT that none exist.",
                         a_actor->GetFormID(), tally.shapes, tally.pbr, tally.specular,
                         tally.vertexColours, tally.dynamic,
                         features.empty() ? "none" : features);
            // The control, on its own line so it cannot be skimmed past. If
            // these two numbers disagree the vertexColours count above is not
            // evidence of anything.
            if (tally.vertexPos == tally.shapes) {
                spdlog::info("DyeCensus: control PASSED, vertexDesc read live on {} of {} "
                             "shape(s) (VF_VERTEX). The vertexColours count above is real.",
                             tally.vertexPos, tally.shapes);
            } else {
                spdlog::warn("DyeCensus: control FAILED, VF_VERTEX read on only {} of {} "
                             "shape(s). Every renderable mesh carries it, so the vertexDesc "
                             "read is wrong and the vertexColours count above means NOTHING.",
                             tally.vertexPos, tally.shapes);
            }
        }

        // ---- the eye dye spike (dyeing-eyes step 3) -------------------------
        //
        // ⚠⚠ INSIDE Repaint AND SHARING ITS RECORD VECTOR, which is the whole
        // reason it is here rather than in a module of its own. Three things
        // come free and every one of them is the dangerous half of the job:
        //
        //   * Restore already runs at the top of Repaint and already walks BOTH
        //     actor roots, and the face node hangs under the third-person one,
        //     so an eye record is torn down by machinery that is field-proven
        //     rather than by a second teardown nobody has exercised.
        //   * A texture build that completes queues QueueRepaint for the waiter,
        //     which re-enters THIS function, which re-runs the pass below. A
        //     swap sitting beside Repaint would be asked once and never again,
        //     and the first pass always comes back pending because the build is
        //     on the render thread.
        //   * The record goes into the same vector under the same lock, so
        //     there is one owner of a displaced material and not two.
        //
        // ⚠ IT IS A SPIKE AND IT IS SUPPOSED TO BE UGLY. The colour is an INI
        // string, not an outfit field, and nothing here is persisted. That is
        // deliberate: the research's own order is to look at a violent colour
        // on real eyes BEFORE spending a codec version, because what is unknown
        // is artistic (a saturated iris may refuse to move at all) and no
        // amount of persistence work answers it.
        // ⚠ NOT GATED ON THE SPIKE ANY MORE. The eye colour is a real outfit
        // field, so this runs on every repaint exactly as the armour walk above
        // does, and PaintEyeTint returns immediately when the outfit carries no
        // colour. bEyeDyeSpike survives only as a debug override on WHICH
        // colour lands, read inside.
        PaintEyeTint(a_actor, a_outfit.eyeTint, a_outfit.scleraTint, a_outfit.eyeTint2,
                     a_outfit.eyeBlend, record, pending);

        const auto id = a_actor->GetFormID();
        {
            std::scoped_lock l(g_lock);
            g_shapes[id] = std::move(shapes);
            if (record.empty()) {
                // Do not leave an empty vector behind. Restore's first act is a
                // find, and an empty entry costs it a lookup on every refresh
                // for an actor that has nothing to put back.
                g_swapped.erase(id);
            } else {
                g_swapped[id] = std::move(record);
            }
        }

        if (emissiveProbe) {
            // ⚠ THE VERDICT LINE, and the only one that answers rung 2. `shared`
            // counting anything above zero means at least two properties name one
            // NiColor, which kills rung 2 as designed: writing a tint through it
            // would recolour every other shape pointing at the same allocation,
            // the material-cache hazard one layer down. `null` matters too, since
            // a property with no emissive colour at all has nothing for rung 2 to
            // write to and would need one allocated, which is a different and
            // much larger question than tinting an existing one.
            std::size_t distinct = 0, shared = 0, nulls = 0, maxOwners = 0;
            for (const auto& [ptr, owners] : emissiveOwners) {
                if (!ptr) {
                    nulls = owners;
                    continue;
                }
                ++distinct;
                maxOwners = std::max(maxOwners, owners);
                if (owners > 1) {
                    ++shared;
                }
            }
            spdlog::info("DyeEmissive: actor {:08X} SUMMARY {} distinct allocation(s), {} of "
                         "them named by more than one property, busiest holds {}, {} shape(s) "
                         "carried a NULL emissiveColor. Rung 2 is only viable if shared is 0 "
                         "and null is 0.",
                         a_actor->GetFormID(), distinct, shared, maxOwners, nulls);
        }
        if (spikeRung != 0) {
            // ⚠ ITS OWN LINE AT info, NOT FOLDED INTO THE DEBUG SUMMARY. The
            // spike's deliverable is a verdict with evidence, and a verdict that
            // has to be reconstructed from a debug-level line nobody enabled is
            // not evidence. Says which mode ran, so a control run and a write run
            // cannot be confused for each other afterwards.
            //
            // ⚠ THE RUNG AND THE MODE ARE NOT THE SAME NUMBER, AND THIS LINE
            // GOT BOTH WRONG FOR EVERY RUN OF THE ONE MODE ANYONE SHIPS. It
            // read `isRung1 = spikeRung <= 2` and `writing = mode 1 or 3`,
            // which are the right tests for the three modes that existed when
            // it was written. Mode 5 arrived with rung 3 (OS-139) and falls
            // through both: it printed "rung 2 ... NEGATIVE CONTROL ... tinted
            // through the property, material untouched" over a run that had
            // just re-diffused nine shapes through a baked texture. Every claim
            // in that sentence was false, and mode 5 is what the live INI runs.
            //
            // The mapping is the walk's own, and it is the walk that has to stay
            // the authority: >= 5 goes to SwapToTintedDiffuse (rung 3), <= 2 to
            // SwapToSameFeatureTint (rung 1), the rest to WriteEmissiveTint
            // (rung 2). The odd mode of each pair writes, the even one is that
            // rung's negative control.
            const int  rung    = spikeRung >= 5 ? 3 : (spikeRung <= 2 ? 1 : 2);
            const bool writing = (spikeRung % 2) != 0;
            const char* did    = rung == 3   ? "re-diffused through a baked texture, feature kept"
                                 : rung == 1 ? "cloned at their own feature"
                                             : "tinted through the property, material untouched";
            spdlog::info("DyeSpike: rung {} mode {} ({}), {} shape(s) {}, {} True PBR dyed "
                         "ON the PBR path, {} downgraded to the shipped swap as True PBR, "
                         "{} refused as True PBR, {} had no emissiveColor, on actor {:08X}",
                         rung, spikeRung,
                         writing ? "writing the tint" : "NEGATIVE CONTROL", spikeSame, did,
                         spikePbrOnPbr, spikePbrDowngraded, spikePbrSkipped, spikeNoEmissive,
                         a_actor->GetFormID());
        }
        spdlog::debug("OutfitDye: actor {:08X} wrote {} tint material(s) across {} "
                      "shape(s), {} skipped on feature, {} shared with a dyed slot, {} slot(s) "
                      "still waiting for their 3D",
                      id, written, seen, skipped, shared, pending);
        if (candidates > 0 && written == 0 && pending == 0) {
            // ⚠ NARROW ON PURPOSE. The old test was "a dye is set and nothing
            // was written", which fires for two entirely normal states: a dye
            // sitting on a slot the actor is not currently wearing, and a dye
            // on a garment whose every shape carries a skipped feature. Both
            // logged a warning on every single refresh and buried the case this
            // line exists for. `candidates` counts shapes that passed the
            // feature guard AND had a channel set AND were not already dyed
            // through a shared property.
            //
            // ⚠ `pending == 0` IS THE THIRD TERM AND IT WAS MISSING, so this
            // warning cried wolf on two nights of logs and named a function the
            // running rung never calls. The reasoning behind it was rung 0's,
            // where the only swap is SwapToTintMaterial and its only failure is
            // CreateMaterial returning null, so `written == 0` genuinely did
            // mean a hard failure. **The live INI runs rung 5**, where the
            // colour is baked into a texture on the RENDER thread and `ok` comes
            // back false for the whole pass while that build is in flight. That
            // is "not yet", not "failed", and the deferred chain lands it a
            // moment later.
            //
            // Field 2026-08-08, one pass on actor 000E1BA9 at 00:55:38.874: nine
            // `rung 3 WAITING on a texture ... (diffuse not yet)` lines, this
            // warning claiming CreateMaterial had failed for all nine, then the
            // same nine re-diffused by 00:55:39.471. Sixty-eight of these in a
            // log where every dye landed.
            //
            // ⚠ NAMES THE RUNG RATHER THAN THE CALL. Which call actually failed
            // depends on which rung is running, and the last version of this
            // line hard-coded rung 0's answer into a message rung 5 was
            // printing. Whoever reads this next needs the number to know
            // whether to look at CreateMaterial or at DyeTexture::Acquire.
            spdlog::warn("OutfitDye: {} shape(s) on actor {:08X} passed the dye filter, NOT "
                         "ONE material was written and none is waiting on a texture. The "
                         "swap failed outright for every one of them on dye rung {}; no dye "
                         "will appear.",
                         candidates, id, spikeRung);
        }
        return RepaintResult{ written, pending };
    }

    void QueueRepaint(RE::ActorHandle a_actor) {
        auto             ptr   = a_actor.get();
        RE::Actor* const actor = ptr.get();
        if (!actor) {
            return;
        }
        const auto id = actor->GetFormID();
        {
            std::scoped_lock l(g_lock);
            // insert_or_assign REFRESHES an in-flight chain's budget and tells
            // us whether it created the entry. So a second rebuild arriving
            // mid-chain gets a full budget without a second chain, and the two
            // callers that arm on one rebuild - the worn-pass hook and
            // REAug::RefreshActor - cost one chain between them rather than two
            // running the same swap churn in lockstep.
            RetryState fresh;
            fresh.walksLeft = DyeGate::kRepaintAttempts;
            fresh.postsLeft = DyeGate::kRepaintPosts;
            fresh.started   = std::chrono::steady_clock::now();
            // lastWalk stays at the epoch on purpose, so the FIRST link of a
            // chain always walks rather than waiting out the gap before it does
            // anything at all.
            if (!g_retry.insert_or_assign(id, fresh).second) {
                return;
            }
        }
        PostRepaintAttempt(a_actor, id);
    }

    void EndFlash() {
        std::scoped_lock l(g_flashLock);
        EndFlashLocked();
    }

    void PulseFlash(float a_mult) {
        std::scoped_lock l(g_flashLock);
        if (g_flash.empty()) {
            return;
        }
        for (auto& s : g_flash) {
            // ⚠ THE O(1) HALF OF RestoreOne'S SAFETY TEST, AND ONLY THAT HALF.
            // Re-reading the property off the geometry is what guards the real
            // hazard: the geometry owns its property through properties[kEffect],
            // so anything that reassigns that slot frees the
            // BSLightingShaderProperty while our raw pointer still names it.
            // Comparing the stale value is defined; dereferencing it is not.
            //
            // The other half, "is the geometry still under one of the actor's
            // roots", is deliberately NOT asked here. It walks to the root, and
            // this runs on every shape on every frame of the burst. It also
            // answers a question this write does not need: a detached geometry
            // is still a live object we hold a NiPointer to, so writing to its
            // property is harmless, merely invisible. The teardown asks it,
            // because deciding whether to bother restoring is exactly what it
            // is for.
            auto* const live = s.geom ? LightingPropOf(s.geom.get()) : nullptr;
            if (!live || live != s.prop) {
                continue;
            }
            s.prop->emissiveMult = a_mult;
            s.prop->DoClearRenderPasses();
        }
    }

    bool FlashActive() {
        std::scoped_lock l(g_flashLock);
        return !g_flash.empty();
    }

    // ---- OS-206: the requip transition, public half -----------------------

    std::size_t ArmRequip(RE::Actor* a_actor, std::uint32_t a_slotMask, bool a_logEmpty) {
        // ⚠ THE STRIPE FLASH COMES DOWN FIRST AND UNCONDITIONALLY, before the
        // lock below and before any early return. Both features write
        // emissiveMult on the same properties, so a flash still up when this
        // arms would be restored through records describing a state this has
        // already overwritten. Taken outside g_requipLock because EndFlash
        // takes g_flashLock, and the two must never be held in one order here
        // and the other order anywhere else.
        EndFlash();

        std::scoped_lock l(g_requipLock);
        // The old flourish goes down even when the new one arms nothing, for
        // the reason FlashChannel gives: bailing early while records are live
        // is how a garment stays lit with nothing left that knows about it.
        EndRequipLocked();

        if (!a_actor || a_slotMask == 0) {
            return 0;
        }
        auto* const biped = a_actor->GetBiped1(false).get();
        if (!biped) {
            return 0;
        }

        // ⚠ DEDUPED BY GEOMETRY, NOT BY BIT. A garment covering several slots
        // is not always cloned per slot, so two bits can name one partClone and
        // the walk would record the same shape twice. Two records on one
        // property restore in an order nobody chose, and the second write puts
        // back whatever the first had already changed.
        std::unordered_set<RE::BSGeometry*> seen;
        std::size_t                         armed = 0;
        // ⚠ COUNTED AND LOGGED, because "nothing happened" and "everything was
        // refused" look identical on screen and need different fixes. The skin
        // bug of 2026-08-15 was invisible in the log for exactly this reason:
        // the arm line said how many shapes it lit and never what it walked
        // past.
        std::size_t refusedCharacter = 0;
        std::size_t refusedNoEmissive = 0;
        // ⚠ THE COUNTER THAT WOULD HAVE ANSWERED "I SAW NO EFFECT" ON THE FIRST
        // FIELD RUN. A property with kOwnEmit off ignores its own emissive
        // entirely, so an armed shape and a lit shape were never the same
        // number and the log could not tell them apart.
        std::size_t neededOwnEmit = 0;

        for (std::uint32_t bit = 0; bit < OS::kBitCount; ++bit) {
            if ((a_slotMask & (1u << bit)) == 0) {
                continue;
            }
            auto* const node = biped->objects[bit].partClone.get();
            if (!node) {
                continue;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                node, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    auto* const prop = CountableShapeProp(a_geom);
                    if (!prop || !seen.insert(a_geom).second) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    // ⚠⚠ SKIN IS WORN IN THESE SAME SLOTS AND MUST NOT BE LIT.
                    // The actor's skin is a TESObjectARMO staged into the biped
                    // like any garment, so a mask naming the hand slot reaches
                    // the character's hands whenever no gauntlet is covering
                    // them. Field 2026-08-15: switching to Naked turned the
                    // hands flat violet and read as a missing texture. The
                    // verdict comes from the dye grid's own classifier so the
                    // two cannot disagree about what skin is.
                    if (!RequipDiff::RequipMayPaint(SkipReasonFor(prop->material->GetFeature()))) {
                        ++refusedCharacter;
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const ec = prop->emissiveColor;
                    if (!ec) {
                        // Nothing to drive and nothing to put back. Skipping is
                        // correct rather than merely safe: the flourish rides a
                        // channel that is already live, it does not switch one
                        // on, and a record with no colour could not be restored.
                        ++refusedNoEmissive;
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    Swapped rec{};
                    rec.kind             = Swapped::Kind::kEmissive;
                    rec.geom             = RE::NiPointer<RE::BSGeometry>(a_geom);
                    rec.prop             = prop;
                    // ⚠⚠ RECORDED, AND IT WAS NOT UNTIL 2026-08-15. RestoreOne
                    // puts kOwnEmit back by reading this field, so leaving it at
                    // the zero-initialised default made every restore CLEAR the
                    // flag on a garment that had arrived with it set. Every other
                    // arm site in this file captures it; this one is the outlier
                    // that a grep for `.flags =` finds in one line.
                    rec.flags            = prop->flags.underlying();
                    rec.origEmissive     = *ec;
                    rec.origEmissiveMult = prop->emissiveMult;
                    // ⚠ THE OTHER ENUM. PF is EShaderPropertyFlag8, which is what
                    // SetFlags takes; `flags` is an enumeration over
                    // EShaderPropertyFlag, and asking it about a PF value does
                    // not compile. Both name the same bit.
                    if (!prop->flags.any(
                            RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit)) {
                        ++neededOwnEmit;
                    }
                    g_requip.push_back(std::move(rec));
                    ++armed;
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        if (armed == 0) {
            if (!a_logEmpty) {
                return 0;
            }
            // ⚠ SAID OUT LOUD, because this is the "I saw no effect" case and
            // the three numbers separate its causes: no geometry under the mask
            // at all, everything refused as character rather than outfit, or
            // garments present but carrying no emissive to drive.
            spdlog::info("Requip: armed NOTHING over mask {:#010x}; {} refused as character, "
                         "{} had no emissive colour (OS-206).",
                         a_slotMask, refusedCharacter, refusedNoEmissive);
            return 0;
        }
        g_requipActor = a_actor->GetFormID();
        spdlog::info("Requip: armed {} shape(s) over mask {:#010x}; {} refused as character, "
                     "{} had no emissive colour, {} needed kOwnEmit switching on (OS-206).",
                     armed, a_slotMask, refusedCharacter, refusedNoEmissive, neededOwnEmit);
        return armed;
    }

    std::vector<RE::NiAVObject*> RequipSlotNodes(RE::Actor* a_actor, std::uint32_t a_slotMask) {
        std::vector<RE::NiAVObject*> nodes;
        if (!a_actor || a_slotMask == 0) {
            return nodes;
        }
        // Third person only, on ArmRequip's and FlashChannel's terms.
        auto* const biped = a_actor->GetBiped1(false).get();
        if (!biped) {
            return nodes;
        }
        for (std::uint32_t bit = 0; bit < OS::kBitCount; ++bit) {
            if ((a_slotMask & (1u << bit)) == 0) {
                continue;
            }
            auto* const node = biped->objects[bit].partClone.get();
            if (!node) {
                continue;
            }
            // ⚠ DEDUPED BY NODE, EXACTLY AS ArmRequip DEDUPES BY GEOMETRY. A
            // garment covering several slots is not always cloned per slot, so
            // two bits can name one partClone and the caller would hang two
            // copies of the same effect on one shoulder.
            if (std::find(nodes.begin(), nodes.end(), node) != nodes.end()) {
                continue;
            }
            nodes.push_back(node);
        }
        return nodes;
    }

    void PulseRequip(float a_mix, const RE::NiColor& a_peakColour) {
        std::scoped_lock l(g_requipLock);
        if (g_requip.empty()) {
            return;
        }
        const float mix = a_mix < 0.0f ? 0.0f : (a_mix > 1.0f ? 1.0f : a_mix);
        const float keep = 1.0f - mix;
        for (auto& s : g_requip) {
            // ⚠ THE O(1) HALF OF RestoreOne'S SAFETY TEST, AND ONLY THAT HALF,
            // on PulseFlash's terms and for PulseFlash's reason: the geometry
            // owns its property through properties[kEffect], so anything that
            // reassigns that slot frees the BSLightingShaderProperty while this
            // raw pointer still names it. Comparing the stale value is defined;
            // dereferencing it is not.
            //
            // The other half, the walk to a root, is deliberately not asked: it
            // runs per shape per frame, and a detached geometry is still a live
            // object we hold a NiPointer to, so writing to it is invisible
            // rather than unsafe.
            auto* const live = s.geom ? LightingPropOf(s.geom.get()) : nullptr;
            if (!live || live != s.prop) {
                continue;
            }
            auto* const ec = s.prop->emissiveColor;
            if (!ec) {
                continue;
            }
            ec->red   = s.origEmissive.red * keep + a_peakColour.red * mix;
            ec->green = s.origEmissive.green * keep + a_peakColour.green * mix;
            ec->blue  = s.origEmissive.blue * keep + a_peakColour.blue * mix;
            s.prop->emissiveMult =
                s.origEmissiveMult * keep + RequipFlourish::kPeakEmissiveMult * mix;
            // ⚠⚠ THE FLAG, AND WITHOUT IT THE THREE WRITES ABOVE ARE DECORATION.
            // A BSLightingShaderProperty only contributes its own emissive when
            // kOwnEmit is set, so a garment that arrives with it off swallowed
            // the whole flourish silently: 18 shapes armed, 18 shapes lighting
            // nothing, and a log line that read healthy. FlashChannel has always
            // set it at the moment it writes; this rides the same rule.
            //
            // ⚠ HERE RATHER THAN IN ArmRequip, so the flag is never on for a
            // frame in which nothing has written the colour underneath it. The
            // original goes back through RestoreOne from the recorded flags.
            s.prop->SetFlags(PF::kOwnEmit, true);
            s.prop->DoClearRenderPasses();
        }
    }

    void EndRequip() {
        std::scoped_lock l(g_requipLock);
        EndRequipLocked();
    }

    bool RequipActive() {
        std::scoped_lock l(g_requipLock);
        return !g_requip.empty();
    }

    std::size_t RequipLiveShapes() {
        std::scoped_lock l(g_requipLock);
        // ⚠⚠ BOTH HALVES OF RestoreOne'S TEST, AND THE SECOND HALF IS THE WHOLE
        // POINT. This asked property identity alone until 2026-08-15, which is
        // the ONE thing a rebuild usually leaves alone: RestoreOne's own comment
        // says the likely outcome is a DETACHED geometry whose parent pointer
        // and property are both untouched. So the count never fell to zero, the
        // flourish sat in kWait until the 30 frame backstop fired, and kCondense
        // never ran once in the field. The teardown line said so on every swap,
        // reporting the same records as stale that this call had just counted as
        // live, because the two were not asking the same question.
        //
        // ⚠ The actor is looked up rather than stored, on EndRequipLocked's
        // terms: a flourish outlives many frames and the 3D under it is expected
        // to go.
        RE::Actor* actor = nullptr;
        if (g_requipActor != 0) {
            actor = RE::TESForm::LookupByID<RE::Actor>(g_requipActor);
        }
        const auto* const root      = actor ? actor->Get3D(false) : nullptr;
        const auto* const rootFirst = actor ? actor->Get3D(true) : nullptr;

        std::size_t live = 0;
        for (auto& s : g_requip) {
            auto* const p = s.geom ? LightingPropOf(s.geom.get()) : nullptr;
            if (!p || p != s.prop) {
                continue;
            }
            // Under either root, per RestoreOne: the two are separate scenes and
            // a geometry is in one of them or in neither.
            const bool attached =
                (root != nullptr && IsUnder(s.geom.get(), root)) ||
                (rootFirst != nullptr && IsUnder(s.geom.get(), rootFirst));
            if (attached) {
                ++live;
            }
        }
        return live;
    }

    std::size_t FlashChannel(RE::Actor* a_actor, const DyeFlash::Key& a_key) {
        std::scoped_lock l(g_flashLock);
        // ⚠ THE OLD ONE COMES DOWN FIRST, ALWAYS, INCLUDING WHEN THE NEW ONE
        // LIGHTS NOTHING. Returning early on a bad request while a previous
        // flash was still up is how a stripe stays lit forever: the user clicks
        // a stripe with no shapes, this bails, and nothing else is coming to
        // put the last one back until the editor closes.
        EndFlashLocked();

        if (!a_actor) {
            return 0;
        }
        // The third-person biped, the one the editor's own camera shows. The
        // first-person one is deliberately not lit: the flash answers "which
        // piece is this stripe" about the character on screen.
        auto* const biped = a_actor->GetBiped1(false).get();
        auto* const node  = FlashNodeFor(biped, a_key);
        if (!node) {
            return 0;
        }

        std::size_t shapeIndex = 0;
        std::size_t lit        = 0;
        const auto  visit =
            [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                auto* const prop = CountableShapeProp(a_geom);
                if (!prop) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                const auto index = shapeIndex++;
                if (static_cast<std::size_t>(ChannelForShapeIndex(index)) != a_key.channel) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                auto* const ec = prop->emissiveColor;
                if (!ec) {
                    // Measured as never happening by the rung-2 probe (0 null
                    // across five passes), refused anyway: allocating one would
                    // mean owning it for the property's lifetime, which is a
                    // much larger question than writing an existing one.
                    return RE::BSVisit::BSVisitControl::kContinue;
                }

                Swapped rec;
                rec.kind             = Swapped::Kind::kEmissive;
                rec.geom             = RE::NiPointer<RE::BSGeometry>(a_geom);
                rec.prop             = prop;
                rec.original         = nullptr;  // no material touched, no ref taken
                rec.flags            = prop->flags.underlying();
                rec.origEmissive     = *ec;
                rec.origEmissiveMult = prop->emissiveMult;
                g_flash.push_back(std::move(rec));

                // ⚠ THE COLOUR AND THE MULTIPLIER BOTH, not the multiplier
                // alone. emissiveColor is whatever the mesh author left there
                // and it is routinely black, against which any multiplier is
                // still black and the flash is a silent no-op on exactly the
                // meshes nobody would think to test.
                ec->red   = 1.0f;
                ec->green = 1.0f;
                ec->blue  = 1.0f;
                prop->emissiveMult = DyeFlash::kLitEmissiveMult;
                prop->SetFlags(PF::kOwnEmit, true);
                prop->DoClearRenderPasses();
                ++lit;
                return RE::BSVisit::BSVisitControl::kContinue;
            };

        RE::BSVisit::TraverseScenegraphGeometries(node, visit);
        // ⚠⚠ THE SAME SECOND ROOT THE PAINTER WALKS, IN THE SAME ORDER, AND
        // THAT IS THE WHOLE POINT. Field 2026-08-14: the scabbard dyed and did
        // not flash. A shape's channel is its POSITION in the traversal, so a
        // painter that walks two roots and a flash that walks one do not merely
        // miss a highlight, they disagree about what every later channel means.
        // Armour keys resolve null here and are unaffected.
        if (a_key.target == DyeTarget::kWeapon) {
            if (auto* const scabbard = ScabbardFor(node)) {
                RE::BSVisit::TraverseScenegraphGeometries(scabbard, visit);
            }
        }

        if (lit == 0) {
            g_flashActor = 0;
            return 0;
        }
        g_flashActor = a_actor->GetFormID();
        spdlog::info("DyeFlash: lit {} shape(s) for channel {} on {} {} (OS-144).", lit,
                     a_key.channel,
                     a_key.target == DyeTarget::kArmour ? "armour slot" : "weapon slot",
                     a_key.slotBit);
        return lit;
    }

    // The half of a flash that is the same however the node was found. Called
    // with g_flashLock held and with the previous flash already taken down.
    //
    // ⚠ EXTRACTED RATHER THAN COPIED, because there are two ways in now: a node
    // NAME for an overlay layer, and the face node by POINTER for makeup. A
    // second copy of this walk would be a second place for the record keeping to
    // drift, and the records are what put the character back.
    std::size_t FlashObjectLocked(RE::Actor* a_actor, RE::NiAVObject* a_node,
                                  const char* a_label) {
        if (!a_actor || !a_node) {
            return 0;
        }
        std::size_t lit        = 0;
        std::size_t walked     = 0;
        std::size_t noProp     = 0;
        std::size_t noEmissive = 0;
        const auto  visit = [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
            ++walked;
            auto* const prop = CountableShapeProp(a_geom);
            if (!prop) {
                ++noProp;
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            auto* const ec = prop->emissiveColor;
            if (!ec) {
                ++noEmissive;
                return RE::BSVisit::BSVisitControl::kContinue;
            }

            Swapped rec;
            rec.kind             = Swapped::Kind::kEmissive;
            rec.geom             = RE::NiPointer<RE::BSGeometry>(a_geom);
            rec.prop             = prop;
            rec.original         = nullptr;
            rec.flags            = prop->flags.underlying();
            rec.origEmissive     = *ec;
            rec.origEmissiveMult = prop->emissiveMult;
            g_flash.push_back(std::move(rec));

            // The colour AND the multiplier, for the reason FlashChannel gives:
            // an authored black emissive stays black under any multiplier.
            ec->red   = 1.0f;
            ec->green = 1.0f;
            ec->blue  = 1.0f;
            prop->emissiveMult = DyeFlash::kLitEmissiveMult;
            prop->SetFlags(PF::kOwnEmit, true);
            prop->DoClearRenderPasses();
            ++lit;
            return RE::BSVisit::BSVisitControl::kContinue;
        };

        RE::BSVisit::TraverseScenegraphGeometries(a_node, visit);

        if (lit == 0) {
            // ⚠⚠ A SILENT ZERO IS WHAT THIS PATH IS ACTUALLY EXPOSED TO, and
            // it used to return without a word. emissiveColor is a NULLABLE
            // pointer and nothing has measured whether a facegen property
            // carries one; DyeFlash.h's "none is null" was measured on worn
            // ARMOUR. Without this line a flash that lit nothing looks exactly
            // like a flash that was never asked for, and no log separates them.
            // ArmRequip already logs its zero case with counters; this walk did
            // not.
            spdlog::info("DyeFlash: lit NOTHING on {}. {} geometry walked, {} with no "
                         "lighting property, {} with no emissive colour.",
                         a_label, walked, noProp, noEmissive);
            g_flashActor = 0;
            return 0;
        }
        g_flashActor = a_actor->GetFormID();
        spdlog::info("DyeFlash: lit {} of {} shape(s) on {} ({} had no emissive colour).",
                     lit, walked, a_label, noEmissive);
        return lit;
    }

    std::size_t FlashNode(RE::Actor* a_actor, const char* a_nodeName) {
        std::scoped_lock l(g_flashLock);
        // The old one comes down first, always, for FlashChannel's reason: a
        // request that lights nothing must not leave the previous flash up.
        EndFlashLocked();

        if (!a_actor || !a_nodeName || a_nodeName[0] == '\0') {
            return 0;
        }
        // ⚠ Get3D(false), NEVER Get3D(). The no-argument form is the FIRST
        // person model for the player, which holds no overlay nodes and would
        // make this a silent no-op on exactly the character being edited most
        // of the time.
        auto* const root = a_actor->Get3D(false);
        if (!root) {
            return 0;
        }
        const RE::BSFixedString wanted{ a_nodeName };
        auto* const             node = root->GetObjectByName(wanted);
        if (!node) {
            return 0;
        }
        const std::string label = std::string{ "node '" } + a_nodeName + "'";
        return FlashObjectLocked(a_actor, node, label.c_str());
    }

    std::size_t FlashHeadPart(RE::Actor* a_actor, const char* a_partEditorId,
                              std::size_t a_channel) {
        std::scoped_lock l(g_flashLock);
        // The old one comes down first, always, for FlashChannel's reason: a
        // request that lights nothing must not leave the previous flash up.
        EndFlashLocked();

        if (!a_actor || !a_partEditorId || a_partEditorId[0] == '\0') {
            return 0;
        }
        // The same face node the head paint walks, reached the same way. A
        // head part's geometry hangs off it, not off any biped clone, which is
        // why FlashChannel's walk lights nothing here and this exists.
        auto* const proc     = a_actor->GetActorRuntimeData().currentProcess;
        auto* const mid      = proc ? proc->middleHigh : nullptr;
        auto* const faceNode = mid ? mid->faceNodeSkinned : nullptr;
        if (!faceNode) {
            spdlog::info("DyeFlash: actor {:08X} has no face node, so head part '{}' "
                         "cannot be lit.",
                         a_actor->GetFormID(), a_partEditorId);
            return 0;
        }

        // ⚠⚠ THE SAME THREE GATES THE HEAD PAINT WALK APPLIES, IN THE SAME
        // ORDER, BEFORE THE INDEX: the exact name, CountableShapeProp, and the
        // invisible-shape skip. A stripe's channel is the shape's POSITION among
        // the geometries that survive those gates, so a flash that skipped one
        // of them would number the shapes differently from the painter and light
        // the wrong one on exactly the parts that carry a proxy or a blank. The
        // census measured one geometry per part, so today this is channel 0
        // finding one shape; the gates are kept so that stays true the day a
        // part ships two.
        const std::string_view wanted{ a_partEditorId };
        std::size_t            shapeIndex = 0;
        std::size_t            lit        = 0;
        std::size_t            named      = 0;
        std::size_t            noEmissive = 0;
        const auto             visit =
            [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                if (std::string_view{ a_geom->name.c_str() } != wanted) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                ++named;
                auto* const prop = CountableShapeProp(a_geom);
                if (!prop) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                if (ShapeIsInvisible(a_geom, prop)) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                const auto index = shapeIndex++;
                if (static_cast<std::size_t>(ChannelForShapeIndex(index)) != a_channel) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                auto* const ec = prop->emissiveColor;
                if (!ec) {
                    ++noEmissive;
                    return RE::BSVisit::BSVisitControl::kContinue;
                }

                Swapped rec;
                rec.kind             = Swapped::Kind::kEmissive;
                rec.geom             = RE::NiPointer<RE::BSGeometry>(a_geom);
                rec.prop             = prop;
                rec.original         = nullptr;
                rec.flags            = prop->flags.underlying();
                rec.origEmissive     = *ec;
                rec.origEmissiveMult = prop->emissiveMult;
                g_flash.push_back(std::move(rec));

                // The colour AND the multiplier, for the reason FlashChannel
                // gives: an authored black emissive stays black under any
                // multiplier.
                ec->red   = 1.0f;
                ec->green = 1.0f;
                ec->blue  = 1.0f;
                prop->emissiveMult = DyeFlash::kLitEmissiveMult;
                prop->SetFlags(PF::kOwnEmit, true);
                prop->DoClearRenderPasses();
                ++lit;
                return RE::BSVisit::BSVisitControl::kContinue;
            };

        RE::BSVisit::TraverseScenegraphGeometries(faceNode, visit);

        if (lit == 0) {
            // Said out loud, for FlashObjectLocked's reason: a flash that lit
            // nothing must not read like one that was never asked for. The
            // counters separate "no geometry wears that name" (the part
            // contributed nothing, which a hair parent routinely does) from
            // "found, but its property carries no emissive colour".
            spdlog::info("DyeFlash: lit NOTHING on head part '{}' channel {}: {} geometry "
                         "wore the name, {} of those had no emissive colour.",
                         a_partEditorId, a_channel, named, noEmissive);
            g_flashActor = 0;
            return 0;
        }
        g_flashActor = a_actor->GetFormID();
        spdlog::info("DyeFlash: lit {} shape(s) on head part '{}' channel {}.", lit,
                     a_partEditorId, a_channel);
        return lit;
    }

    // ⚠⚠ THERE IS NO MAKEUP FLASH, AND IT IS NOT AN OVERSIGHT. MEASURED
    // 2026-08-16, on the field, with the walk narrowed as far as it goes:
    //
    //   DyeFlash: lit 2 of 21 shape(s) on the head (makeup)
    //
    // The two were 00UBE_FemaleHead and 00UBE_FemaleMouth, with thirteen hair
    // parts, the eyes, the lashes and the brows all correctly skipped by a
    // kFaceGen filter. The whole head washed white anyway, because those two
    // shapes ARE the whole head.
    //
    // That is the end of the road rather than a tuning problem. A flash marks a
    // SHAPE, and makeup has no shape of its own: it is baked into the head's
    // tint TEXTURE. There is no sub-region of the head to light, so no filter
    // and no colour and no multiplier makes this mark a lip colour rather than
    // a face. User's call after seeing it: no flash beats a wrong one.
    //
    // What a future attempt would need is a cue that is not a flash, on the UI
    // side, since the camera already frames the face at 0.95 closeness.


    void Restore(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        // ⚠ THE FLASH COMES DOWN WITH THE DYE, AND THIS IS THE 3D-REBUILD
        // BACKSTOP. Restore runs before every Repaint, so any refresh that can
        // replace a partClone passes through here first. Without this the flash
        // records would name geometry the refresh is about to unhook, and the
        // gate in DyeFlash.h cannot see a rebuild coming: it has no flag to
        // read. RestoreOne then abandons anything already detached, which is
        // correct rather than merely safe.
        // Unconditional, and cheap when nothing is lit: narrowing it to "this
        // actor" would need the flash to be keyed per actor, and it is not -
        // there is one lit stripe at a time, on whoever the editor is dressing.
        EndFlash();
        std::vector<Swapped> mine;
        {
            std::scoped_lock l(g_lock);
            const auto       it = g_swapped.find(a_actor->GetFormID());
            if (it == g_swapped.end()) {
                return;
            }
            mine = std::move(it->second);
            g_swapped.erase(it);
        }
        // The roots a recorded geometry may still hang off. Null means that half
        // of the actor's 3D is gone, and records naming it are stale by
        // definition, so the loop below only releases their references.
        //
        // ⚠ BOTH, SINCE OS-125 PUT FIRST-PERSON SHAPES IN THE SAME RECORD. With
        // only the third-person root here every one of those released without
        // restoring and stranded a FacegenTint, which renders as bare skin.
        const auto* const root      = a_actor->Get3D(false);
        const auto* const rootFirst = a_actor->Get3D(true);

        std::size_t put = 0, stale = 0, cloned = 0;
        for (auto& s : mine) {
            if (RestoreOne(s, root, rootFirst, cloned) == DyeGate::TeardownAction::kRestore) {
                ++put;
            } else {
                ++stale;
            }
        }
        spdlog::debug("OutfitDye: actor {:08X} restored {} material(s), {} skipped as "
                      "stale, {} re-interned as a clone (roots 3rd={} 1st={})",
                      a_actor->GetFormID(), put, stale, cloned, root != nullptr,
                      rootFirst != nullptr);
    }

    void Clear() {
        std::size_t actors = 0, records = 0, put = 0, stale = 0, cloned = 0;
        {
            std::scoped_lock l(g_lock);
            actors = g_swapped.size();
            for (auto& [id, v] : g_swapped) {
                // ⚠ RESOLVE THE ACTOR SO THE SAME ATTACHMENT TEST THE REFRESH
                // PATH USES CAN RUN HERE TOO. This function used to release its
                // references and touch nothing else, on the theory that a save
                // load always tears down the 3D these records name. It does not:
                // reloading into the same character leaves the property alive
                // still carrying our FacegenTint, and dropping the record was
                // then the last chance anything had to put the real material
                // back. The shape rendered with the player's skin from that
                // point on and no repaint could recover it, because the feature
                // guard reads a FacegenTint as undyeable.
                //
                // A load that genuinely DID tear the 3D down gives a null actor
                // or a null root here, every record classifies as release-only,
                // and the behaviour is exactly what it was before. The old
                // reasoning was right about that case and it is preserved
                // rather than replaced.
                //
                // ⚠ AND BOTH ROOTS, for the reason on RestoreOne. This is the
                // path a save load takes, which is exactly where OS-126 was
                // reported: with only the third-person root, every first-person
                // swap released without restoring and the gauntlets came back as
                // skin. Same bug this comment already describes, one biped over.
                auto* const       actor     = RE::TESForm::LookupByID<RE::Actor>(id);
                const auto* const root      = actor ? actor->Get3D(false) : nullptr;
                const auto* const rootFirst = actor ? actor->Get3D(true) : nullptr;
                for (auto& s : v) {
                    if (RestoreOne(s, root, rootFirst, cloned) ==
                        DyeGate::TeardownAction::kRestore) {
                        ++put;
                    } else {
                        ++stale;
                    }
                    ++records;
                }
            }
            // The NiPointer destructors run here, on the game thread, with the
            // renderer and the heap both still up. That is the difference
            // between this and the process-exit teardown g_swapped's leak
            // exists to avoid.
            g_swapped.clear();
            // The snapshot goes with them. It describes the shapes the previous
            // character was wearing, and the editor would otherwise label the
            // next character's channels from it until the first refresh landed.
            g_shapes.clear();
            // ⚠ g_retry IS DELIBERATELY LEFT ALONE, and it is the one map here
            // that must be. Its entries are countdowns, not engine state: an
            // in-flight chain re-resolves the actor AND the outfit when it
            // drains, so one that crosses this load simply paints whatever the
            // new session says, which is what we want anyway. Erasing entries
            // out from under a queued task would instead let the next
            // QueueRepaint arm a SECOND chain for the same actor, and the two
            // would share one budget.
        }
        // put and stale are broken out rather than summed because they are the
        // field-readable difference between the fix working and the bug being
        // back. A reload that leaves a dyed garment attached must show put > 0
        // here; a stranded tint shows up as put == 0 with records > 0, followed
        // by a 'skip ... feature FaceGenRGBTint' naming an armour shape.
        spdlog::debug("OutfitDye: cleared {} record(s) across {} actor(s); {} restored, {} "
                      "released as stale, {} re-interned as a clone. Dropped the shape "
                      "snapshot.",
                      records, actors, put, stale, cloned);
    }

    std::vector<ShapeInfo> SnapshotShapes(RE::FormID a_actor) {
        std::scoped_lock l(g_lock);
        const auto       it = g_shapes.find(a_actor);
        return it == g_shapes.end() ? std::vector<ShapeInfo>{} : it->second;
    }

    // See the header.
    void ArmPassDump() { g_passDump.store(true, std::memory_order_release); }

    void SetDyePreview(const DyePreview::State& a_state) {
        {
            std::scoped_lock l(g_dyePreviewLock);
            g_dyePreview = a_state;
        }
        g_dyePreviewOn.store(a_state.armed, std::memory_order_release);
        // ⚠ THE ONLY LINE THAT SAYS THE FEATURE RAN. A working preview looks
        // exactly like a working commit from the outside, and a broken one
        // looks like nothing at all; the OS-145 skip mask learned the same
        // lesson in this file. Info, because the field log is where this gets
        // confirmed.
        spdlog::info("DyePreview: armed on {:08X}, target {} slot {} chan {}, "
                     "rgb={:02X}{:02X}{:02X} mode={} flake={}",
                     a_state.actorId, static_cast<int>(a_state.key.target),
                     a_state.key.slotBit, a_state.key.channel, a_state.channel.r,
                     a_state.channel.g, a_state.channel.b, a_state.channel.mode,
                     a_state.channel.flake);
    }

    void ClearDyePreview() {
        const bool was = g_dyePreviewOn.exchange(false, std::memory_order_acq_rel);
        {
            std::scoped_lock l(g_dyePreviewLock);
            g_dyePreview = {};
        }
        if (was) {
            spdlog::info("DyePreview: cleared");
        }
    }

}  // namespace OS::OutfitDye
