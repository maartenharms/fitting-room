#include "BipedPost.h"

#include "REAugments.h"
#include "Settings.h"
#include "SlotMask.h"

namespace OS::BipedPost {

    namespace {
        // BipedAnim / BIPOBJECT are now real, static_assert'd CommonLibSSE-NG
        // structs (RE/B/BipedAnim.h) whose layout matches the verified 1.5.97
        // disassembly exactly (objects[42] at biped+0x10, stride 0x78; BIPOBJECT
        // item@0x00, addon@0x08, part@0x10, partClone@0x20). Using the typed
        // members lets the header's static_asserts catch any future layout drift
        // at compile time - which raw offsets never would.

        // NiAVObject::flags bit 0 = kHidden. The Hide mechanism for attachment
        // slots culls the staged 3D with this flag (hide-mechanism-final.md,
        // Fallbacks #1: Equipment Toggle 2's technique); a rebuild re-clones
        // partClone without the flag, so un-hiding is automatic on the next
        // refresh. Accessed via CommonLib's GetFlags() accessor: the flags
        // member is at +0xF4 on SE 1.5.97 but +0x10C on AE 1.6.x
        // (RE::NiAVObject::GetFlags = RelocateMember(0x0F4, 0x10C)), so a raw
        // offset would clobber the wrong field on AE.

        RE::BipedAnim* CurrentPlayerBiped() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return nullptr;
            }
            return player->GetCurrentBiped().get();
        }
    }

    void RestoreRealItems(RE::BipedAnim* a_biped, std::uint32_t a_touchedMask,
                          RE::TESObjectARMO* const* a_realWorn, RE::TESObjectARMO* a_nakedSkin) {
        if (!a_touchedMask) {
            return;
        }
        if (!a_biped) {
            spdlog::debug("restore: no biped this pass.");
            return;
        }
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            if (!((a_touchedMask >> bit) & 1u)) {
                continue;
            }
            auto&       obj = a_biped->objects[bit];
            // Nothing staged at all: leave the empty slot alone (attachment
            // slots with no worn item and no injection).
            if (!obj.item && !obj.part) {
                continue;
            }
            // No real gear -> the engine's naked convention is the SKIN, never
            // null (the deferred 3D attach and other readers expect valid
            // fields). .addon is NEVER touched - see BipedPost.h.
            auto* const want = a_realWorn[bit] ? a_realWorn[bit] : a_nakedSkin;
            if (obj.item == want || !want) {
                continue;
            }
            spdlog::debug("restore: slot {} item {:08X} -> {} {:08X}", bit,
                          obj.item ? obj.item->GetFormID() : 0,
                          a_realWorn[bit] ? "real" : "skin", want->GetFormID());
            obj.item = want;  // .part / .partClone keep showing the outfit
        }
        if (Settings::GetSingleton().dumpBiped) {
            DumpBipedObjects("after-restore", a_biped);
        }
    }

    void CullObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask) {
        if (!a_biped || !a_objectMask) {
            return;
        }
        constexpr auto count = static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            if (!((a_objectMask >> slot) & 1ull)) {
                continue;
            }
            auto* const node = a_biped->objects[slot].partClone.get();
            if (!node) {
                continue;
            }
            if (!node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().set(RE::NiAVObject::Flag::kHidden);
                spdlog::debug("hide: culled node for biped object {}.", slot);
            }
        }
    }

    void QueueObjectNodeCull(RE::ActorHandle a_actor, std::uint64_t a_objectMask) {
        auto* task = SKSE::GetTaskInterface();
        if (!task || !a_objectMask) {
            return;
        }
        const auto mask   = a_objectMask;
        const auto handle = a_actor;
        task->AddTask([handle, mask] {
            // Re-resolve the biped at run time against the CAPTURED actor (not
            // the player): the pass's biped may have been torn down before the
            // task queue drains, and a fresh clone from an async model load
            // needs the cull re-applied. Skip silently if the actor has unloaded
            // between queue and drain - a stale handle resolves to null.
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (!actor) {
                return;
            }
            CullObjectNodes(actor->GetCurrentBiped().get(), mask);
        });
    }

    void ShowObjectNodes(RE::BipedAnim* a_biped, std::uint64_t a_objectMask) {
        if (!a_biped || !a_objectMask) {
            return;
        }
        constexpr auto count = static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            if (!((a_objectMask >> slot) & 1ull)) {
                continue;
            }
            if (auto* const node = a_biped->objects[slot].partClone.get();
                node && node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
                spdlog::debug("hide: restored node for biped object {}.", slot);
            }
        }
    }

    void QueueObjectNodeShow(RE::ActorHandle a_actor, std::uint64_t a_objectMask) {
        auto* task = SKSE::GetTaskInterface();
        if (!task || !a_objectMask) {
            return;
        }
        const auto mask   = a_objectMask;
        const auto handle = a_actor;
        task->AddTask([handle, mask] {
            auto       ptr   = handle.get();
            RE::Actor* actor = ptr.get();
            if (actor) {
                ShowObjectNodes(actor->GetCurrentBiped().get(), mask);
            }
        });
    }

    void CullNodes(RE::BipedAnim* a_biped, std::uint32_t a_hiddenAttachmentMask) {
        CullObjectNodes(a_biped, a_hiddenAttachmentMask);
    }

    void QueueNodeCull(RE::ActorHandle a_actor, std::uint32_t a_hiddenAttachmentMask) {
        QueueObjectNodeCull(a_actor, a_hiddenAttachmentMask);
    }

    void DumpBipedObjects(const char* a_when, RE::BipedAnim* a_biped) {
        auto* const current = CurrentPlayerBiped();
        spdlog::info("[dump {}] awmBiped={} currentBiped={} match={}", a_when,
                     static_cast<const void*>(a_biped), static_cast<const void*>(current),
                     a_biped == current);

        auto* const biped = a_biped ? a_biped : current;
        if (!biped) {
            spdlog::info("[dump {}] no biped.", a_when);
            return;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(RE::BIPED_OBJECTS::kTotal); ++i) {
            const auto& obj  = biped->objects[i];
            auto* const item = obj.item;
            auto* const node = obj.partClone.get();
            if (!item && !node) {
                continue;
            }
            const char* name = "?";
            int         at   = -1;
            if (item && item->Is(RE::FormType::Armor)) {
                name = item->GetName();
                at   = static_cast<int>(item->As<RE::TESObjectARMO>()->GetArmorType());
            }
            spdlog::info("[dump {}] slot {:2}: item={:08X} '{}' armorType={} ({}) partClone={}",
                         a_when, i, item ? item->GetFormID() : 0, name, at,
                         at == 0 ? "Light" : at == 1 ? "Heavy" : "-",
                         static_cast<const void*>(node));
        }
    }

}  // namespace OS::BipedPost
