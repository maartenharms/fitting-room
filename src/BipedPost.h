#pragma once

#include "Outfit.h"

namespace OS::BipedPost {

    // Put the REAL worn armor back into objects[slot].item for every slot in
    // a_touchedMask, leaving .part/.partClone showing the outfit. Called at the
    // end of the worn-pass wrapper, on the same call stack as 15500's write.
    // a_biped MUST be the BipedAnim the styles were injected into - derived
    // register-free from the actor's own 3rd-person holder,
    // player->GetBiped1(false).get() (Cause-B fix) - NOT GetCurrentBiped().
    // a_realWorn is indexed by biped bit (0..31).
    //
    // Field discipline (2026-07-11 invisible-torso bug): NEVER touch .addon -
    // the 3D attach runs DEFERRED (every dump shows partClone=0x0 at restore
    // time) and needs the staged ARMA to skin the mesh; nulling it kills the
    // attach and leaves the slot invisible. And when no real armor is worn,
    // write a_nakedSkin (the actor's skin ARMO - the engine's own "nothing
    // worn here" convention, XP-neutral) instead of null; valid-but-mismatched
    // item/part is attach-proven (the equipped-transmog case runs that way).
    void RestoreRealItems(RE::BipedAnim* a_biped, std::uint32_t a_touchedMask,
                          RE::TESObjectARMO* const* a_realWorn,
                          RE::TESObjectARMO* a_nakedSkin);

    // Gate instrumentation: log objects[0..41] - form, formType, armorType,
    // partClone - for a_biped (the AWM-derived biped), plus the actor's
    // GetCurrentBiped() and whether the two match, so the checkpoint can confirm
    // the restore lands where the readers look (3rd person) and observe 1st.
    void DumpBipedObjects(const char* a_when, RE::BipedAnim* a_biped);

    // The Hide mechanism for attachment slots (hide-mechanism-final.md
    // §Fallbacks #1, promoted to primary after the visitor proxy crashed IED):
    // set NiAVObject::kHidden on objects[slot].partClone. CullNodes acts
    // immediately on the given biped (clones 15500 made synchronously on this
    // stack); QueueNodeCull re-runs it deferred against the actor the handle
    // resolves to (its CURRENT biped), catching clones the BSTaskPool attaches
    // late (uncached models) and tolerating the actor unloading before the queue
    // drains. Player callers pass the player's handle; the NPC attachment-hide
    // path passes the NPC's handle - so a follower's deferred cull re-resolves
    // the follower, never the player.
    void CullNodes(RE::BipedAnim* a_biped, std::uint32_t a_hiddenAttachmentMask);
    void QueueNodeCull(RE::ActorHandle a_actor, std::uint32_t a_hiddenAttachmentMask);

    // Generalized biped-object cull used by the transient Presets preview.
    // Unlike the armor-only helpers above, this 64-bit mask can address weapon
    // and quiver objects 32 through 41 as well as shield object 9.
    void CullObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask);
    void QueueObjectNodeCull(RE::ActorHandle a_actor, std::uint64_t a_objectMask);

    // Symmetric restore for preview-only culls. Skyrim may reuse the same
    // partClone across a refresh, so leaving Presets must clear kHidden
    // explicitly rather than assume a rebuild replaces the clone.
    void ShowObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask);
    void QueueObjectNodeShow(RE::ActorHandle a_actor, std::uint64_t a_objectMask);

    // Put back the SKIN the hidden piece took with it, for the headgear group.
    //
    // ⚠⚠ THE HEAD IS A BODY-SKIN CASE AND WAS ON THE ATTACHMENT PATH. Hiding
    // body armour has always meant re-applying the race skin rather than culling
    // a node (hiddenBodySkinMask), because the geometry underneath is skin and
    // the armour SUPPRESSED it. The head is the same shape of problem and this
    // is the missing half of it: `!UBE\Head\FemaleHead_tangent.nif` carries a
    // BSDismemberSkinInstance whose partition 1 is body part 43, the EARS, 856
    // vertices on one bone. Staging a helmet, which declares 30/31/42/43, makes
    // the engine disable that partition. Culling the helmet's node afterwards
    // removes the helmet and leaves the ears disabled, so the player is looking
    // at the gap the helmet used to cover (field 2026-08-12, and the same run
    // confirmed a circlet leaves them alone because it declares 42 alone).
    //
    // ⚠ NOTHING IN THE SCENEGRAPH SAYS THIS HAPPENED. Five rounds of census
    // measured no detached geometry, no kHidden, no moved bone, no changed
    // shader, alpha or partition count, in the exact frames where the ears were
    // visibly shredded. A partition is disabled BELOW the geometry, so it can
    // only be read - and only be repaired - through the engine's own accessor.
    //
    // a_slots is in editor-bit space (bit = editor slot - 30) and MUST already
    // be narrowed to kHeadgearSlotMask by the caller, minus anything a style
    // actually landed on: a style covering the ears has its own geometry there
    // and the engine is right to keep the skin off.
    //
    // Re-asserted every pass, like the node show above, because every rebuild
    // re-runs the engine's own disable.
    void RestoreDismemberPartitions(RE::Actor* a_actor, std::uint32_t a_slots);

    // The deferred half, and it exists for the reason QueueNodeCull does.
    //
    // ⚠⚠ THE FIRST CUT HAD ONLY THE SYNCHRONOUS CALL AND THE EARS STAYED GONE
    // WITH THE REPAIR REPORTING SUCCESS. The tell is in its own log: two passes
    // 100 ms apart BOTH found body part 43 disabled, and the second would have
    // skipped it had our re-enable survived the first. So something disables it
    // after we run, which is exactly the shape this file already knows about -
    // the 3D attach is DEFERRED through the BSTaskPool, so the pass that stages
    // an armature is not the pass that finishes hanging its geometry. The node
    // cull needed the same second, queued attempt for the same reason.
    void QueueRestoreDismemberPartitions(RE::ActorHandle a_actor, std::uint32_t a_slots);

    // The HEAD-FAMILY half of the same scar, found by round eighteen's wig
    // probe: the switched apply leaves the KS SMP strands (and their hairline)
    // with their single partition, slot 131, editorVisible OFF - fully
    // skinned, healthy world bound, invisible - and FSMP's cooperative pass
    // cannot help because a partition is disabled BELOW the geometry. Head
    // partition slots are biped slots plus 100; a slot is put back ON only
    // when nothing worn AND DRAWN declares the matching biped slot, so a
    // real helmet keeps hiding what it covers while a ghost occupant (worn
    // armor staging no geometry on this race, round twenty's finding) does
    // not. Re-asserted per head build like the body
    // half, and with the same deferred second attempt. Returns the biped-bit
    // mask it re-enabled.
    //
    // ⚠ SPEAKS UNCONDITIONALLY since round nineteen: the shipped restore ran
    // three times sync plus queued and logged NOTHING because it only spoke
    // when it flipped, so "ran and found nothing" and "never ran" read the
    // same and the round was ambiguous
    // (a-verify-that-asks-one-question-calls-half-a-failure-healthy). Every
    // call now logs what it visited, found, and put back, tagged a_note.
    // a_note MUST point at static storage - the queued half captures the
    // pointer.
    std::uint32_t RestoreHeadPartPartitions(RE::Actor* a_actor, const char* a_note);
    void          QueueRestoreHeadPartPartitions(RE::ActorHandle a_actor, const char* a_note);

    // Round nineteen's timing lesson: both existing restore calls run inside
    // the head-build call chain BEFORE the hair geometry attaches, and their
    // queued pass drains on the SKSE task queue while the attach defers
    // through the BSTaskPool - a different queue - and loses. This arms a
    // short one-shot ladder on its own watcher thread (the ArmBaseWatch
    // posture, NEVER a task re-queueing a task - the FaceWait scar) that
    // posts one plain restore task at +1 s, +2.5 s and +5 s, provably after
    // the attach has drained. HeadSettleActive is the lock-free "a ladder is
    // pending" query the dismember census gates on.
    void               ArmHeadPartitionSettle(RE::ActorHandle a_actor);
    [[nodiscard]] bool HeadSettleActive();

}  // namespace OS::BipedPost
