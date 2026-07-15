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
    // stack); QueueNodeCull re-runs it deferred against the CURRENT player
    // biped, catching clones the BSTaskPool attaches late (uncached models).
    void CullNodes(RE::BipedAnim* a_biped, std::uint32_t a_hiddenAttachmentMask);
    void QueueNodeCull(std::uint32_t a_hiddenAttachmentMask);

}  // namespace OS::BipedPost
