#include "PCH.h"

#include "BodyMeshPath.h"

namespace OS {

    std::string BuiltBodyMeshKey(RE::Actor* a_actor) {
        if (!a_actor) {
            return {};
        }
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        // ⚠⚠ GetSkin() WITH NO ARGUMENT, AND THE SLOT OVERLOAD IS A DIFFERENT
        // QUESTION. GetSkin(slot) answers "what is covering this slot", which
        // is the WORN ARMOUR whenever there is any, and only falls back to the
        // skin on a bare slot. This read it and got the character's cuirass:
        // field log 2026-08-08 shows the body being reported as
        // '!ube\armor\iron\f\cuirasslight' and 'armor\studded\female\body',
        // which are outfits, not bodies. The no-argument overload is the
        // actor's SKIN: the NPC's own override if it has one, otherwise the
        // race's, and nothing to do with what they are wearing.
        auto* skin = a_actor->GetSkin();
        if (!skin) {
            return {};
        }
        auto* race = a_actor->GetRace();
        auto* addon = skin->GetArmorAddonByMask(race, Slot::kBody);
        if (!addon) {
            return {};
        }

        // ⚠ THE SEX PICKS THE MODEL, and this is half of why the answer is
        // right per character rather than per install: one addon carries a male
        // and a female mesh, and a body mod usually replaces only one of them.
        const auto* npc = a_actor->GetActorBase();
        const auto  sex = (npc && npc->IsFemale()) ? RE::SEXES::kFemale
                                                   : RE::SEXES::kMale;
        const char* model = addon->bipedModels[sex].GetModel();
        if (!model || !*model) {
            return {};
        }
        return BodyMeshKey(model);
    }

}  // namespace OS
