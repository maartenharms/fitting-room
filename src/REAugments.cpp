#include "REAugments.h"

#include "BipedHooks.h"
#include "CrashGuard.h"

namespace OS::REAug {

    bool ApplyArmorAddon(RE::TESObjectARMO* a_armor, RE::TESRace* a_race,
                         ActorWeightModel* a_model, bool a_isFemale) {
        using func_t = bool (*)(RE::TESObjectARMO*, RE::TESRace*, ActorWeightModel*, bool);
        static REL::Relocation<func_t> func{ REL::RelocationID(17392, 17792) };
        return func(a_armor, a_race, a_model, a_isFemale);
    }

    void SetEquipFlag(RE::AIProcess* a_process) {
        // Flag 1<<0 ("equipment changed") - matches SOS's Flag::kUnk01.
        using func_t = void (*)(RE::AIProcess*, std::uint8_t);
        static REL::Relocation<func_t> func{ REL::RelocationID(38867, 39907) };
        func(a_process, 1 << 0);
    }

    void UpdateEquipment(RE::AIProcess* a_process, RE::Actor* a_actor) {
        using func_t = void (*)(RE::AIProcess*, RE::Actor*);
        static REL::Relocation<func_t> func{ REL::RelocationID(38404, 39395) };
        func(a_process, a_actor);
    }

    void RefreshPlayer(bool a_sceneKick) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* proc   = player ? player->GetActorRuntimeData().currentProcess : nullptr;
        if (!proc) {
            spdlog::warn("RefreshPlayer: no AIProcess.");
            return;
        }
        // Proven on 1.5.97 (checkpoint 2): this synchronous rebuild runs even
        // while Container/Barter pause the game. The worn-pass counter guards
        // against silent regressions - if a refresh does not re-run the skinning
        // pass, the override cannot update and that must be visible in the log.
        const auto c0 = BipedHooks::PlayerWornPassCount();
        // Bracket the biped rebuild that skins the meshes: if it AVs on a bad
        // mesh (some third-party armor reliably crashes here), the marker
        // survives and next launch flags that style. EndKick only runs if
        // UpdateEquipment returned - i.e. the rebuild did NOT crash.
        CrashGuard::BeginKick();
        SetEquipFlag(proc);
        UpdateEquipment(proc, player);
        CrashGuard::EndKick();
        const auto c1 = BipedHooks::PlayerWornPassCount();
        spdlog::debug("refresh: wornpass ran {}x", c1 - c0);
        if (c1 == c0) {
            spdlog::warn("refresh: skinning pass did NOT run - the override will not update visually.");
        }

        if (a_sceneKick) {
            // Container/Barter pause the game; kick the player's scene graph so
            // the rebuilt biped renders immediately (0x2000 flag from IED).
            if (auto* root = player->Get3D(false)) {
                RE::NiUpdateData ctx;
                ctx.time  = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                root->UpdateWorldBound();
                root->Update(ctx);
            }
        }
        spdlog::debug("RefreshPlayer done (kick={}).", a_sceneKick);
    }

    // Mirrors RE::Actor::GetSkin() (Actor.h:530) deliberately: actor override
    // skin (TESNPC::skin) first, then race->skin. Consumed only as an identity
    // filter ("is this ARMO the naked body", Task 3.1). WARNING: the engine's
    // cleared-slot base body is applied as ApplyArmorAddon(race->skin) inside
    // func 15499 - race->skin specifically. So if the Hide-body-slot fallback
    // (docs/superpowers/research/hide-mechanism-final.md, Fallback #1) is ever
    // built, it must re-apply race->skin, NOT this base-then-race result: a
    // custom-race/body mod that sets TESNPC::skin would diverge.
    RE::TESObjectARMO* GetActorSkin(RE::Actor* a_actor) {
        if (!a_actor) {
            return nullptr;
        }
        if (auto* base = a_actor->GetActorBase(); base && base->skin) {
            return base->skin;
        }
        auto* race = a_actor->GetRace();
        return race ? race->skin : nullptr;
    }

}  // namespace OS::REAug
