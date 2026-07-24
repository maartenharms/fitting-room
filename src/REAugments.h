#pragma once
#include "PCH.h"

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
    // site). a_preserveDrawnHands is captured BEFORE UpdateEquipment (bit 0 =
    // right, bit 1 = left), because a paused menu rebuild can lose the visual
    // hand placement after ActorState transiently reports sheathed. MAIN
    // THREAD ONLY, same contract as RefreshActor.
    void RestyleEquippedWeapons(
        RE::Actor* a_actor, std::uint8_t a_preserveDrawnHands);

    // The actor's skin ARMO, mirroring RE::Actor::GetSkin() (Actor.h:530):
    // an actor-specific override (TESNPC::skin) if present, else race->skin.
    // Used only as an identity filter - "is this ARMO the naked body" (Task 3.1
    // catalog). For the player these agree (the Player NPC has no WNAM skin).
    // NOTE: if the Hide-body-slot fallback is ever implemented, it must apply
    // race->skin specifically (what engine func 15499 applies), NOT this - a
    // custom-race mod that sets TESNPC::skin would diverge from base->skin here.
    RE::TESObjectARMO* GetActorSkin(RE::Actor* a_actor);

}  // namespace OS::REAug
