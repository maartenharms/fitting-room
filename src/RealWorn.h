#pragma once

#include "PCH.h"

#include <cstdint>

namespace OS {

    // The actor's REAL worn armor, captured in one walk over their
    // InventoryChanges entry list. armo[bit] = the ARMO the actor wears in
    // biped slot (30 + bit), or null; coverage = the OR of every worn ARMO's
    // slot mask (the actor's real worn slots). The NPC worn-required rule
    // (spec §3) intersects the outfit's style/hide masks against coverage;
    // the player ignores it. The rules engine's WorldWatch (Task 11) reads
    // coverage alone for its WornSlot condition.
    struct RealWorn {
        RE::TESObjectARMO* armo[32]{};
        std::uint32_t      coverage{ 0 };
    };

    // ONE walk over the actor's worn inventory. The old shape called
    // Actor::GetWornArmor 32x - and each of those builds the FULL armor
    // inventory (Actor.cpp: GetInventory + linear scan), so a styled pass
    // paid 32 inventory builds; that does not scale to a market square of
    // styled NPCs (spec §3), nor to a periodic world-state poll (the rules
    // engine's heartbeat, Task 11). Semantically identical to the per-slot
    // version: CommonLib's GetWornArmor(slot) returns the first worn ARMO
    // whose slot mask covers the slot (count>0 && entry->IsWorn() &&
    // armor->HasPartOf, Actor.cpp), and every worn item necessarily owns an
    // InventoryEntryData in entryList (ExtraWorn/ExtraWornLeft live on an
    // entry's extra list - RE/I/InventoryEntryData.cpp::IsWorn), so walking
    // entryList once and OR-ing each worn ARMO's GetSlotMask into the array
    // fills all 32 slots with the same first-worn-wins result. Verified
    // against RE/I/InventoryChanges.h (entryList/owner) + InventoryEntryData.h
    // (object/IsWorn) + Actor.cpp (GetWornArmor). Takes a caller-supplied
    // InventoryChanges* - the biped hooks pass the rebuild pass's own
    // (a_changes->owner is the actor, no extra lookup); WorldWatch passes
    // RE::PlayerCharacter::GetInventoryChanges() - a plain accessor onto the
    // actor's already-maintained ExtraContainerChanges, NOT the same
    // GetInventory()-builds-a-map cost GetWornArmor pays.
    [[nodiscard]] inline RealWorn SnapshotRealWorn(RE::InventoryChanges* a_changes) {
        RealWorn r;
        if (!a_changes || !a_changes->entryList) {
            return r;
        }
        for (auto* entry : *a_changes->entryList) {
            if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) {
                continue;
            }
            auto* armo = entry->object->As<RE::TESObjectARMO>();
            if (!armo) {
                continue;
            }
            const auto mask = armo->GetSlotMask().underlying();
            r.coverage |= mask;
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                if (((mask >> bit) & 1u) && !r.armo[bit]) {
                    // First worn ARMO seen wins the slot. GetWornArmor breaks
                    // the same tie by the inventory map's pointer-key order,
                    // so the winner can differ ONLY when two worn ARMOs cover
                    // one slot - a state the engine's one-item-per-slot equip
                    // never produces. coverage (the §3 input) is tie-order-
                    // independent regardless.
                    r.armo[bit] = armo;
                }
            }
        }
        return r;
    }

}  // namespace OS
