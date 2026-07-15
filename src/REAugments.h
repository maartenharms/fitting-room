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

    // Full player visual refresh (flag + update + optional scene kick so the
    // rebuild renders inside paused Container/Barter menus - IED's recipe).
    // Mutates engine state: MAIN THREAD ONLY. Off-main-thread callers must
    // marshal via SKSE::GetTaskInterface()->AddTask, never call this directly.
    void RefreshPlayer(bool a_sceneKick);

    // The actor's skin ARMO, mirroring RE::Actor::GetSkin() (Actor.h:530):
    // an actor-specific override (TESNPC::skin) if present, else race->skin.
    // Used only as an identity filter - "is this ARMO the naked body" (Task 3.1
    // catalog). For the player these agree (the Player NPC has no WNAM skin).
    // NOTE: if the Hide-body-slot fallback is ever implemented, it must apply
    // race->skin specifically (what engine func 15499 applies), NOT this - a
    // custom-race mod that sets TESNPC::skin would diverge from base->skin here.
    RE::TESObjectARMO* GetActorSkin(RE::Actor* a_actor);

}  // namespace OS::REAug
