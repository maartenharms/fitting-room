#pragma once

#include "PCH.h"

#include "DyeFlash.h"    // DyeFlash::Key, for the stripe flash below
#include "DyePreview.h"  // DyePreview::State, for the hover preview below
#include "Outfit.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Per-outfit ARMOUR DYE. Tier 1: one colour per mesh shape, painted by swapping
// each shape's material for a BSLightingShaderMaterialFacegenTint and writing
// its tintColor.
//
// ⚠ THE ENGINE'S TINT IS AN OVERLAY, NOT A MULTIPLY. The shader computes
// D*D + 2*T*D*(1-D), so T = 0.5 is the identity and byte 128 is the neutral
// dye. D = 0 and D = 1 are fixed points, which is why a black or white texture
// barely moves and why a saturated texture keeps its hue no matter what colour
// is fed in. This is a property of the shader, not a bug in this module: do not
// "fix" it by scaling the tint.
//
// ---- What the field run measured, AE 1.6.1170, Nolvus, 2026-07-31 --------
//
// These are measurements from the Task 1 spike, not opinions. The plan text was
// written before them, so where the two disagree these win.
//
// THE RENDER GATE PASSED. Replacing a property's material with a freshly
// created FacegenTint, clearing the displaced feature flags and calling
// DoClearRenderPasses does make the engine re-pick the shader permutation, and
// it renders. DoClearRenderPasses is load bearing: the pass list is cached on
// the property, so without it a negative result cannot be told apart from a
// stale pass list.
//
// RESTORE IS RELIABLE. "restored 13/36 material(s), 0 skipped as stale" over
// roughly 16 cycles, never a stale entry, no fault and no CTD, including 88
// kEnvironmentMap swaps.
//
// ---- The shine, and why it is gone --------------------------------------
//
// A NEUTRAL tint (128,128,128), the exact algebraic identity of the overlay
// above, still visibly changed kDefault armour, and so did a control that ran
// the identical machinery with a SAME-FEATURE material and no tint written at
// all. The visible change came from the MACHINERY, not the tint value and not
// the FaceGenRGBTint technique. Disassembly of AE 1.6.1170 found it in the two
// engine calls the spike made after the swap, both of which have since been
// removed:
//   * BSLightingShaderProperty::FinishSetupGeometry (0x1414AD1B0) forces
//     material->specularPower to 1.0 whenever the property has kSpecular and
//     specularPower is at or below 0. specularPower at +0x88 is the NIF's
//     Glossiness exponent, and 0 to 1.0 opens a broad whole-surface specular
//     lobe. That is the shine, exactly.
//   * BSLightingShaderProperty::SetupGeometry (0x1414AD010) rewrites property
//     flags one way only: it recomputes kVertexColors, clears kTreeAnim and
//     kEyeReflect, can hide the mesh outright, and clears kSpecular on a
//     colour test. Nothing puts any of it back.
// Neither is needed. DoClearRenderPasses alone invalidates lastRenderPassState,
// and GetRenderPasses re-picks the technique off that. OutfitDye.cpp calls it
// and nothing else, on both the swap and the restore path, and the reasoning is
// written out at the call site.
//
// Two dead theories, for anyone tempted to revive them:
//   * "CopyBaseMembers leaves specularPower and specularColorScale at
//     defaults". It does not. CopyBaseMembers is a plain C++ body in
//     CommonLibSSE-NG (src/RE/B/BSLightingShaderMaterialBase.cpp) and it
//     assigns the entire tail: materialAlpha, refractionPower, specularPower,
//     specularColorScale, subSurfaceLightRolloff, rimLightPower. Verified by
//     reading that source and by disassembling the compiled object out of this
//     build's own CommonLibSSE.lib. Only hashKey, 0x30, 0x34,
//     diffuseRenderTargetSourceIndex and unk98 are skipped, and the ENGINE's
//     own CopyMembers skips the same five, so they cannot be a source of
//     divergence either.
//   * "the FaceGenRGBTint technique lights differently". Refuted by the
//     swap-only control, which changed no feature at all.
//
// ---- SetMaterial is not a store -----------------------------------------
//
// BSShaderProperty::SetMaterial (0x14147BFF0) hands the material to a cache
// acquire (0x1414F7790) that CRCs it, probes a hash table, and on a miss
// interns a CLONE. The pointer the property ends up holding is the cache's, not
// the one passed in, and a FacegenTint always misses because the acquire forces
// its unique path for that feature. So a material created and handed to
// SetMaterial must be destroyed by its creator, and the reference taken on the
// displaced material must be handed back deliberately. Both are done in
// SwapToTintMaterial and Restore with the derivation written out there.
//
// ---- Which material features get dyed -----------------------------------
//
// kDefault and kEnvironmentMap, nothing else.
//
// Envmap is IN, against the plan text, because of mesh fragmentation measured
// in the same run: the player's single equipped Iron Armor is three shapes, and
// 'IronArmor' is kEnvironmentMap while 'IronSkirt' and 'IronSatchel' are
// kDefault. A kDefault-only rule dyes the leather and leaves the metal, which
// reads as a bug rather than a limit. The spike proved envmap swaps do not
// crash once the displaced feature flags are cleared.
//
// ⚠ THE REFLECTION DOES NOT SURVIVE THE SWAP. This said the question was
// unobserved until 2026-08-02, when it was observed: dyeing a reflective piece
// visibly removes its shine. That is the swap clearing kEnvMap, which it has to
// do because of the 0xA0 collision above, so it is the mechanism working as
// described rather than a defect. The census run that confirmed it also
// measured the scale: four of the eight shapes on a dressed character were
// kEnvironmentMap, so this is half a character rather than an edge case.
// bDyeReflective, default true, is the escape hatch and it is now a real
// tradeoff rather than a placeholder for an unknown. Recovering the reflection
// is what the feature-preserving tint spike exists for.
//
// Everything else is skipped on the FEATURE, explicitly, and counted:
//   * kHairTint streaks. Only the HAIR branch of BSLightingShader.ps.hlsl reads
//     vertex-colour green as a mask; every other branch multiplies the full RGB
//     into the output, so leftover mask data becomes visible colour running
//     along each strand.
//   * kFaceGen and kFaceGenRGBTint are ALREADY CARRYING SOMEBODY'S TINT. The
//     player's body, hands and 11 RaceMenu overlay layers are all
//     kFaceGenRGBTint, and writing a dye over tintColor destroys the
//     character's skin tone.
//   * kEye, same reason.
//   * kGlowMap drops its glow MASK. The emissive add is unconditional in the
//     shader and only the mask is technique gated, so a swapped glowmap
//     material lights evenly across a surface meant to glow in places.
//   * kParallax, kParallaxOcc and kMultilayerParallax lose their height data
//     and have no measurement behind them at all.
// The walk is armour-slot scoped, which makes hair and face geometry unlikely,
// but a hood or helmet slot can carry hair, so the guard is written out rather
// than implied by the scoping.
//
// ---- What this module does NOT do ---------------------------------------
//
// Unlike HairColor it writes NOTHING persistent. Hair colour goes through
// TESNPC::SetHairColor, which is actor base state that lands in the save, and
// therefore needs baseline capture, a write-once latch, restore on clear, and a
// co-save record. A dye only ever touches the scene graph, so there is no
// baseline to capture, nothing reaches the save beyond the outfit record
// itself, and uninstalling leaves no trace.
//
// Player only in tier 1. Followers are out of scope.
//
// A dye is never billed as a slot change and stays out of ChangedSlotCount.
//
// ---- Thread safety ------------------------------------------------------
//
// GAME THREAD ONLY: Repaint, Restore, Clear and QueueRepaint. The first three
// walk live 3D and replace materials on it, so the FUCK present thread must
// never call any of them. That is the whole reason the editor reads shape names
// through the cached snapshot SnapshotShapes returns rather than walking the
// actor itself. QueueRepaint touches no 3D itself, but it resolves an actor
// handle and its task body calls Repaint, so it belongs on the same thread.
//
// SnapshotShapes is the one function safe to call from the present thread. It
// takes the same internal lock and copies out, and it walks nothing.
namespace OS::OutfitDye {

    // Moved to Outfit.h so the pure DyeGrid module can take it as input.
    // Aliased here because every existing caller says OutfitDye::ShapeInfo.
    using ShapeInfo = OS::ShapeInfo;

    struct RepaintResult {
        std::size_t written{ 0 };  // materials swapped on this pass
        // Dyed slots whose geometry is STAGED but whose partClone has not been
        // attached yet. Not a failure and not "nothing to dye": the 3D attach
        // runs deferred off BSTaskPool, so these are slots that will paint on a
        // later pass. See DyeGate::ClassifySlot for how the two are told apart.
        std::size_t pending{ 0 };
    };

    // Paint a_outfit's dye onto a_actor's live geometry.
    //
    // ⚠ ZERO IS ONLY A DIAGNOSTIC WHEN SOMETHING WAS ACTUALLY DYEABLE. Zero
    // written is the normal answer for a dye sitting on a slot the actor is not
    // wearing, and for a garment whose every shape carries a skipped feature.
    // The warning fires only when at least one shape passed the feature guard,
    // had a channel set, and still produced no material, which means
    // CreateMaterial failed and no amount of re-refreshing will fix it.
    //
    // ⚠ A SINGLE SYNCHRONOUS CALL RIGHT AFTER A REBUILD IS NOT ENOUGH, which is
    // what `pending` is for. Prefer QueueRepaint below unless you need the shape
    // snapshot refreshed on this very frame.
    //
    // ⚠ Two dyed slots covered by ONE armour addon share a partClone and
    // therefore share a material. The lower bit's colour wins; the walk de-dups
    // on the property pointer so the second slot cannot strand the first one's
    // real material.
    //
    // Also refreshes the shape snapshot SnapshotShapes serves, for every armour
    // slot AND every weapon slot with 3D on it, not only the dyed ones. The
    // editor has to label the channels of a slot that has no dye yet, which is
    // the moment the user is choosing one.
    //
    // ⚠ THE SNAPSHOT IS TWO KINDS OF THING NOW. A ShapeInfo carries a DyeTarget
    // saying whether it is keyed by armour slot bit or by weapon class and
    // hand, and a consumer that reads slotBit without checking gets armour bit
    // 0 for every weapon shape.
    //
    // Restores first when this actor already has a swap recorded, so a second
    // call cannot record a swapped material as the original and strand the
    // real one.
    //
    // Call AFTER the refresh, on the game thread.
    RepaintResult Repaint(RE::Actor* a_actor, const Outfit& a_outfit);

    // Arm a deferred repaint chain for a_actor: re-resolve the actor from the
    // handle when the task drains, re-resolve the outfit it is showing, paint,
    // and re-arm while any slot is still waiting for its partClone.
    //
    // ⚠ THIS, NOT Repaint, IS WHAT MAKES A DYE SURVIVE A REBUILD FITTING ROOM
    // DID NOT ASK FOR (B1, 2026-07-31 review). Repaint used to have exactly one
    // caller, at the tail of REAug::RefreshActor, so a persisted dye reached the
    // screen only when Fitting Room's own refresh happened to run. A save load,
    // a re-equip, a cell change and a race-menu exit all rebuild the biped
    // through the engine and left the armour undyed. BipedHooks arms this from
    // the worn-pass hook, which is the seam every one of those rebuilds passes
    // through.
    //
    // ⚠ AND IT IS WHAT MAKES A LATE CLONE LAND (B2). The single synchronous
    // Repaint read objects[bit].partClone right after UpdateEquipment, and
    // BipedPost.h has already measured that the attach runs deferred - every
    // dump taken there showed partClone == 0x0. Same shape of fix as
    // BipedPost::QueueNodeCull, and for the same measured reason.
    //
    // Safe to call more than once per rebuild. A second call while a chain is in
    // flight REFRESHES that chain's attempt budget instead of starting a second
    // one, so two callers on one rebuild cost one chain rather than two.
    //
    // Takes a handle, not an actor: the actor may unload between the queue and
    // the drain, and a stale handle resolves to null (BipedPost::QueueNodeCull's
    // contract, unchanged).
    //
    // Game thread. The task body runs on the game thread too.
    void QueueRepaint(RE::ActorHandle a_actor);

    // Put every displaced material back and forget them.
    //
    // ⚠ MUST RUN BEFORE THE REBUILD, NOT AFTER. Skyrim may reuse a partClone
    // across a refresh, so a swapped material can survive one. Restoring first
    // makes a cleared dye deterministic rather than dependent on whether the
    // engine happened to rebuild that particular clone.
    void Restore(RE::Actor* a_actor);

    // Drop every record without writing to any of them, releasing the material
    // references they hold.
    //
    // ⚠ CALL THIS ON A SAVE LOAD, from Persistence's revert callback, next to
    // HairColor::Clear. g_swapped is keyed on FormID, and a FormID means
    // nothing across a load: records left behind by the torn-down session would
    // be consumed by the next Restore and written through 3D that no longer
    // belongs to anybody. The shape snapshot goes with them for the same
    // reason, otherwise the editor labels the new character's channels from the
    // old character's mesh until the first refresh lands.
    //
    // Restore is the wrong tool for this. It writes materials back into the
    // scene, which is exactly what must not happen to 3D that is being
    // discarded.
    //
    // ⚠ A deferred repaint chain in flight is deliberately NOT stood down. It
    // holds a countdown and nothing else, and it re-resolves both the actor and
    // the outfit when it drains, so one that crosses this load paints whatever
    // the new session says. Cancelling it would instead let the next
    // QueueRepaint arm a second chain for the same actor sharing one budget.
    //
    // Game thread, like everything else here except SnapshotShapes.
    void Clear();

    // The shapes currently on a_actor, for the editor's channel labels.
    // Populated on the GAME thread by Repaint and read on the present thread,
    // for the same reason HairColor caches its observed tint: the present
    // thread must not walk a scenegraph the game thread may be rebuilding.
    [[nodiscard]] std::vector<ShapeInfo> SnapshotShapes(RE::FormID a_actor);

    // ---- OS-144: light the shapes one dye stripe owns ---------------------
    //
    // ⚠ GAME THREAD, BOTH OF THEM. They walk a scenegraph and write to shader
    // properties, which is exactly what the present thread must not do; the
    // editor's click handler runs on the render thread and has to marshal.
    //
    // ⚠ EndFlash IS NOT OPTIONAL AND HAS FOUR CALLERS, one per way a flash can
    // outlive its welcome: the hold expiring, the editor closing, the target
    // switching, and a different stripe being clicked. The rules and the reason
    // a 3D rebuild is NOT among them are in DyeFlash.h. Restore() also calls it,
    // which is the rebuild backstop.
    //
    // FlashChannel returns how many shapes it lit, so a caller can tell a
    // correct no-op (single-shape gear, nothing worn) apart from a failure and
    // say so. Zero means nothing is armed and nothing needs putting back.
    std::size_t FlashChannel(RE::Actor* a_actor, const DyeFlash::Key& a_key);
    void        EndFlash();
    [[nodiscard]] bool FlashActive();

    // The same flash, aimed at a NAMED node instead of a dye stripe, for the
    // overlays page: clicking a layer lights that layer so the player can see
    // which part of the character it is on.
    //
    // ⚠ A SECOND WAY TO FILL ONE REGISTER, NOT A SECOND REGISTER. It shares
    // g_flash with FlashChannel, so EndFlash and PulseFlash drive it unchanged
    // and the "two live registers holding one property" hazard ArmRequip warns
    // about cannot arise between these two. It also ends whatever was lit
    // first, exactly as FlashChannel does.
    //
    // ⚠ NO CHANNEL FILTER, unlike FlashChannel. A dye stripe owns SOME of the
    // shapes under its node and a layer owns all of them, so every geometry
    // under the node is lit. On an overlay that is the right answer twice over:
    // the sheet is one shape, and the only part of it that is not transparent
    // is the paint.
    //
    // ⚠ THIRD PERSON, and it resolves the node off Get3D(false) rather than a
    // biped. Overlay nodes hang off the skin and the head, not off worn gear,
    // so FlashChannel's biped walk cannot reach them at all.
    //
    // ⚠ GAME THREAD, on FlashChannel's terms. Returns how many shapes were lit;
    // zero means nothing is armed and nothing needs putting back.
    std::size_t FlashNode(RE::Actor* a_actor, const char* a_nodeName);

    // The same flash, aimed at ONE HEAD PART's geometry, for the head stripes
    // in the dye grid: a horn, an ear, a hair's ornament. Head geometry hangs
    // off the face node and not off any biped clone, so FlashChannel's walk
    // cannot reach it; this walks the face node with the part's editor id as
    // the name filter, which is how the head paint walk finds the same shapes.
    //
    // ⚠ THE CHANNEL IS THE PART'S OWN INDEX SPACE, as the painter numbers it:
    // the exact name, CountableShapeProp and the invisible-shape skip all come
    // before the index, in that order. Same register as FlashChannel, so
    // EndFlash and PulseFlash drive it unchanged, and it ends whatever was lit
    // first.
    //
    // ⚠ GAME THREAD, on FlashChannel's terms. Returns how many shapes were lit;
    // zero means nothing is armed and nothing needs putting back.
    std::size_t FlashHeadPart(RE::Actor* a_actor, const char* a_partEditorId,
                              std::size_t a_channel);


    // ---- the requip transition (OS-206) ----------------------------------
    //
    // Arm every shape on the armour slots named in a_slotMask, recording what
    // each one looked like. Returns how many shapes were recorded; ZERO MEANS
    // NOTHING WAS ARMED and the caller must not start a flourish, because a
    // flourish with an empty record set has nothing to put back and nothing to
    // take it down.
    //
    // ⚠ ENDS ANY DYE STRIPE FLASH FIRST. Both write emissiveMult on the same
    // properties, so two live registers holding one property restore over each
    // other and leave a garment at the peak forever. The editor can arm a
    // stripe and then hit Apply, so this is a real sequence rather than a
    // theoretical one.
    //
    // ⚠ THIRD PERSON ONLY, on FlashChannel's terms: it walks GetBiped1(false).
    // ⚠ a_logEmpty IS FOR THE RETRY AND NOTHING ELSE. Condense arms once per
    // frame until the rebuild hands it geometry, so the "armed NOTHING" line
    // would print up to the whole wait budget and bury the run that mattered.
    // Every caller that arms once leaves it true.
    std::size_t ArmRequip(RE::Actor* a_actor, std::uint32_t a_slotMask,
                          bool a_logEmpty = true);

    // The nodes the changed slots hang off, deduped, for hanging art on a
    // GARMENT rather than on the actor.
    //
    // ⚠ THE POINTERS ARE ONLY GOOD FOR THIS FRAME. They are the same partClones
    // ArmRequip walks, and the rebuild that the flourish is waiting for is the
    // thing that destroys them. Use them and drop them; never store one.
    [[nodiscard]] std::vector<RE::NiAVObject*> RequipSlotNodes(RE::Actor* a_actor,
                                                               std::uint32_t a_slotMask);

    // Drive one frame. a_mix is 0 for "exactly as recorded" and 1 for "fully at
    // the peak", and it is clamped to that range rather than trusted. Touches
    // only the properties ArmRequip recorded, so it costs a short vector walk
    // per frame and never a scenegraph traversal.
    void PulseRequip(float a_mix, const RE::NiColor& a_peakColour);

    // Put every recorded shape back. Safe when nothing is armed.
    void EndRequip();

    [[nodiscard]] bool RequipActive();

    // How many armed records still name the property their geometry actually
    // holds. ⚠ THE DRIVER READS THIS INVERTED, AND THAT IS THE POINT: a rebuild
    // replaces the geometry, so every record going stale is what "the rebuild
    // landed" looks like from here. Zero means Wait may hand over to Condense,
    // and nothing else is allowed to make that decision.
    [[nodiscard]] std::size_t RequipLiveShapes();

    // Drive the burst. Touches only the properties FlashChannel already
    // recorded, so it costs a short vector walk per frame and never a
    // scenegraph traversal; the shapes were chosen once, and only the
    // brightness moves after that. Safe to call when nothing is lit.
    //
    // ⚠ NEVER pass the ORIGINAL multiplier through here to "turn it off". The
    // originals live in the records and come back through EndFlash, which also
    // restores the emissive colour and the kOwnEmit flag. This function knows
    // about the burst and nothing else.
    void PulseFlash(float a_mult);

    // Arm a one-shot per-shape dump of the NEXT Repaint pass: every geometry it
    // walks, with its slot, index, channel, material feature, whether the slot
    // is dyed, whether that channel is set, its colour, and a WROTE line on
    // success.
    //
    // ⚠ KEPT RATHER THAN REMOVED WITH THE PROBE IT STARTED AS, and the Ears
    // tile is why. The pass counters say "wrote 5 across 8 shapes, 1 skipped on
    // feature", and the only per-shape line fires on a skip, so a shape nobody
    // gave a colour to and a shape painted somewhere invisible produce
    // identical logs. Every future "this piece will not dye" report runs into
    // that same wall, and the answer is one armed run.
    //
    // ⚠ ONE SHOT, and it has to be. Repaint runs on every refresh and this is a
    // line per geometry, so a flag left standing is a log flood on any actor
    // with a busy mesh.
    //
    // The editor arms it when the dye station is opened, not when the editor
    // is: opening the editor runs a refresh that would consume the shot before
    // the user has dyed anything.
    //
    // Any thread. It is consumed by an exchange inside Repaint on the game
    // thread, which is what makes one arming dump one whole actor rather than
    // one armour slot.
    void ArmPassDump();

    // ---- the hover dye preview (spec 2026-08-09) ---------------------------
    //
    // A transient overlay the walk consults ahead of the staged channel, in
    // the ApparelPreviewSignal shape: keyed to the hovered swatch and the
    // ringed stripe, cleared on unhover, never persisted, never counted. The
    // deed cannot see it because nothing here writes the staged outfit.
    //
    // Any thread. The state sits behind its own lock with an atomic armed
    // fast path; the walk snapshots it once per Repaint pass on the game
    // thread. The caller queues its own repaint; arming alone changes nothing
    // on screen.
    void SetDyePreview(const DyePreview::State& a_state);
    void ClearDyePreview();

}  // namespace OS::OutfitDye
