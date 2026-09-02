#pragma once
#include "PCH.h"

#include "WeaponSlots.h"

#include <string>

namespace OS::REAug {

    // Opaque engine type: the biped-model holder ApplyArmorAddon populates
    // (SKSE lineage calls it ActorWeightModel). We only pass pointers through.
    struct ActorWeightModel;

    // TESObjectARMO::ApplyArmorAddon - attaches the armor's ArmorAddon 3D for
    // race/sex/weight into the given weight model. SE 17392, AE 17792.
    bool ApplyArmorAddon(RE::TESObjectARMO* a_armor, RE::TESRace* a_race,
                         ActorWeightModel* a_model, bool a_isFemale);

    // AIProcess "equipment changed" flag + rebuild. SE 38867/38404, AE 39907/39395.
    // Together they re-run the (hooked) skinning pass without any equip.
    void SetEquipFlag(RE::AIProcess* a_process);
    void UpdateEquipment(RE::AIProcess* a_process, RE::Actor* a_actor);

    // Full per-actor visual refresh (flag + update + optional scene kick so
    // the rebuild renders inside paused Container/Barter menus - IED's
    // recipe), the shared core RefreshPlayer and OutfitSession::
    // RequestRefreshActor both funnel through. Null-guards proc/middleHigh/
    // Get3D: an unloaded / low-process actor has no biped to rebuild - it
    // restyles naturally on its next load, so this does not force one.
    // Mutates engine state: MAIN THREAD ONLY. Off-main-thread callers must
    // marshal via SKSE::GetTaskInterface()->AddTask (RequestRefresh /
    // RequestRefreshActor do this already) - never call this directly.
    void RefreshActor(RE::Actor* a_actor, bool a_sceneKick);

    // Full player visual refresh. A thin wrapper over RefreshActor(player,
    // sceneKick) plus the player-only extras: the CrashGuard preview-crash
    // bracket (its pending marker is a single global key, not actor-scoped -
    // see CrashGuard.h - so it stays player-only) and the worn-pass tripwire
    // (BipedHooks::PlayerWornPassCount is PLAYER-scoped and never moves for
    // an NPC rebuild). Same MAIN THREAD ONLY contract as RefreshActor.
    void RefreshPlayer(bool a_sceneKick);

    // OS-76. Re-run the part-3D loader for an actor's EQUIPPED weapons, which
    // a biped rebuild alone never does: the attach short-circuits while the
    // weapon's node is still parented, so a style change could not show until
    // the player unequipped and re-equipped. Detaches then re-attaches through
    // the engine's own equip entry point - see the mechanism written out on the
    // definition. Weapons only (armor rides the rebuild; ammo has its own call
    // site). a_preserveDrawnHands is captured BEFORE UpdateEquipment, because a
    // paused menu rebuild can lose the visual hand placement after ActorState
    // transiently reports sheathed. MAIN THREAD ONLY, same contract as
    // RefreshActor.
    //
    // ⚠ One mask PER BIPED, never combined. The first-person rig has no hip
    // sheath, so an off-hand weapon's 1P clone sits on SHIELD - the left-HAND
    // node - whether drawn or sheathed. A single shared flag fed by
    // `third || first` therefore always read "drawn" for the off hand and
    // dragged the correctly sheathed third-person clone into the hand, which
    // is the dual-wield bug field-caught on 2026-08-01.
    struct DrawnWeaponHands {
        std::uint8_t third{};  // bit 0 = right, bit 1 = left
        std::uint8_t first{};
    };

    void RestyleEquippedWeapons(
        RE::Actor* a_actor, DrawnWeaponHands a_preserveDrawnHands);

    // ---- The two halves WeaponPreview drives ------------------------------
    //
    // These are the SAME engine entry points RestyleEquippedWeapons uses, named
    // separately because their caller's contract is different: that one only
    // ever touches a weapon the actor genuinely has equipped, and these are for
    // hanging one the actor does NOT carry. Keeping the names apart is what
    // stops a future reader assuming an equipped weapon on either side.

    // Actor::AttachWeapon on a form the actor does not have equipped, to show
    // it sheathed. The caller MUST have proven objects[slot].item is null
    // first: on an occupied slot this fights real gear and trips the attach's
    // change-detect. See WeaponPreview::ShouldShow, which is where that proof
    // lives and is tested.
    //
    // ⚠ It lands on the SHEATH node with no animation and no graph event,
    // because nothing on the attach path performs the hip-to-hand move.
    void AttachWeaponForPreview(RE::Actor* a_actor, RE::TESForm* a_weapon,
                                bool a_leftHand);

    // Move a weapon this actor is already STAGED with from its sheath node into
    // the hand, the way the engine's own draw does.
    //
    // ⚠ THIS EXISTS BECAUSE THE ATTACH ALWAYS SHEATHES AND THE PLAYER MAY NOT
    // BE (user 2026-08-17: "if an actor has a weapon unsheathed we do not have
    // to show it sheathed we can just transmog it"). A preview taken while the
    // weapon is drawn puts the browsed piece on the hip and leaves the hand
    // empty, which is neither what they were looking at nor a pose that exists.
    //
    // ⚠ THE SAME CALL RestoreDisplacedWeapon MAKES, and deliberately the same
    // one: virtual 0xB4 is the only thing that moves a weapon between the
    // sheath node and the hand node, and the restore has been paying for it in
    // the field since the preview shipped. Right hand only, for the reason that
    // function gives: a class slot is main-hand by construction.
    //
    // ⚠ AMMO IS A NO-OP HERE. A quiver sits on the back in both weapon states,
    // so there is no move to make.
    //
    // Says where the clone actually ENDED UP, because that is the one thing
    // this cannot be sure of: 0xB4 is documented against an EQUIPPED weapon and
    // a preview piece is not equipped. A log line beats a guess.
    void DrawStagedWeapon(RE::Actor* a_actor, RE::TESForm* a_weapon,
                          WeaponClass a_class);

    // BipedAnim::ClearBipedPart for one weapon slot, scabbard included.
    //
    // ⚠ CALL IT WHILE objects[a_slot].item IS STILL SET. The teardown keys off
    // item->formType to take its weapon-aware branch, and that branch is what
    // removes the scabbard; clearing item first leaves it behind. It also
    // early-returns on a null partClone, so nulling that by hand instead leaves
    // the node parented and the character wearing two weapons.
    void ClearWeaponPart(RE::BipedAnim* a_biped, std::uint32_t a_slot);

    // Put the actor's REAL main-hand weapon back after a preview displaced it.
    //
    // ⚠ THE DRAWN REPAIR IS THE WHOLE REASON THIS IS NOT JUST AttachWeapon. A
    // forced attach ALWAYS parks a weapon on its sheath node, so restoring a
    // weapon the player had DRAWN would hand it back sitting on their hip in
    // third person and on the off-camera 1P sheath node in first - which reads
    // as "my weapon vanished". Virtual 0xB4 is the engine's own hip-to-hand
    // move; RestyleEquippedWeapons pays for exactly this and this is the same
    // repair, narrowed to the one hand a class slot can hold.
    //
    // ⚠ RIGHT HAND ONLY, and that is not a limitation. Class slots 33..40 are
    // main-hand by construction - AttachWeapon stages the off hand in the
    // race's shield slot instead - so a preview can never have displaced a
    // left-hand weapon, and 0xB4's child selection is ambiguous for the left
    // when both hands share one WEAP form.
    void RestoreDisplacedWeapon(RE::Actor* a_actor, RE::TESForm* a_weapon,
                                bool a_wasDrawn);

    // The AMMO mirror of AttachWeaponForPreview: put a quiver or bolt case on
    // the actor's back.
    //
    // ⚠ AMMO IS A SEPARATE ENTRY POINT, NOT A WIDER RANGE. Actor::AttachWeapon
    // opens `cmp byte ptr [rdx+0x1a], 0x29` (kWeapon) and rejects kAmmo (0x2A)
    // on its first instruction, so a quiver can never ride the weapon path.
    //
    // ⚠ AND IT WALKS THE BIPEDS BY HAND. Actor::AttachWeapon covers the 3rd-
    // and 1st-person rigs internally; ammo has no Actor-level counterpart that
    // does the walk, so this does it - both, for the OS-76 reason.
    //
    // No drawn repair exists or is needed: a quiver stays on the back in both
    // weapon states, so nothing on this path has a hip-to-hand step to miss.
    void AttachAmmoForPreview(RE::Actor* a_actor, RE::TESForm* a_ammo);

    // The name of the node the actor's weapon of this class and hand is
    // actually hanging on, or an empty string when nothing of it is attached.
    //
    // Drawn answers "WEAPON" or "SHIELD"; sheathed answers whatever sheath node
    // the skeleton used; a modded sheath position answers itself. Nobody has to
    // keep a table of where a class hangs, which is the point: the engine has
    // already made that decision and this reads it back.
    //
    // ⚠ THE CLASS IS NOT OPTIONAL, for two reasons that both cost a field
    // round. It is what makes "nothing equipped in that class" answer empty
    // instead of framing some other weapon, and the class slot is the only
    // EXACT placement there is: the off-hand domain has no per-class slot and
    // has to be scanned past every armour slot to reach a weapon.
    [[nodiscard]] std::string WeaponParentNodeName(RE::Actor* a_actor,
                                                   WeaponClass a_class,
                                                   WeaponHand a_hand);

    // The actor's skin ARMO, mirroring RE::Actor::GetSkin() (Actor.h:530):
    // an actor-specific override (TESNPC::skin) if present, else race->skin.
    // Used only as an identity filter - "is this ARMO the naked body" (Task 3.1
    // catalog). For the player these agree (the Player NPC has no WNAM skin).
    // NOTE: if the Hide-body-slot fallback is ever implemented, it must apply
    // race->skin specifically (what engine func 15499 applies), NOT this - a
    // custom-race mod that sets TESNPC::skin would diverge from base->skin here.
    RE::TESObjectARMO* GetActorSkin(RE::Actor* a_actor);

}  // namespace OS::REAug
