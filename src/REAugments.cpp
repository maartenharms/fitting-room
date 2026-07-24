#include "REAugments.h"

#include "BipedPost.h"
#include "BipedHooks.h"
#include "CrashGuard.h"
#include "OutfitSession.h"
#include "PresetPreviewPolicy.h"
#include "WeaponHooks.h"

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

    // OS-76. The rebuild above does everything EXCEPT put weapon 3D back.
    //
    // The staged-parts resolver (SE 15501) walks all 42 biped slots. For an
    // NPC it attaches inline, which is why follower weapon transmog worked
    // from the start. For the PLAYER it does not attach at all: it only
    // QUEUES each weapon into a 10x2 array on PlayerCharacter and returns.
    // That queue is drained by THIS function, and its live caller is a
    // per-frame player update - which a paused menu never runs. So the styled
    // mesh never loaded until something else re-ran the loader, i.e. the
    // user's "exit the menu, unequip and re-equip".
    //
    // Calling it here is sound because the rebuild has already cleared
    // objects[slot].partClone for all 42 slots (SE 15499 -> 15486 zeroes
    // item/part/partClone per slot, byte-verified), so the change-detect at
    // the head of the weapon attach passes on its `partClone == 0` arm and
    // the part-3D loader - and therefore our styling hook - actually runs.
    //
    // CALL IT, NEVER HOOK IT. On AE the per-frame path INLINED this loop
    // (AE 40447 = PlayerCharacter vtable slot 173) and no longer calls out,
    // so a hook here would install, pass a byte check, fire on the other
    // caller and silently miss the per-frame drain - the inlined-site trap
    // that has cost this project a release before. Calling is unaffected:
    // AE 40439 is an instruction-for-instruction clone of SE 39367, still
    // live with a real caller. Note the array base differs between runtimes
    // (SE +0x730, AE +0x738); that is internal to the function, which is
    // exactly why calling it beats reimplementing the drain ourselves.
    void FlushPendingWeapons(RE::PlayerCharacter* a_player) {
        using func_t = void (*)(RE::PlayerCharacter*);
        static REL::Relocation<func_t> func{ REL::RelocationID(39367, 40439) };
        func(a_player);
    }

    namespace {
        // BipedAnim::ClearBipedPart - detaches the slot's node (weapon-aware
        // branch also removes the scabbard), releases weaponManager, and NULLs
        // partClone. SE 15496 (0x1401C6300) / AE 15661 (0x140212730), both
        // confirmed against the shipped address libraries.
        void ClearBipedPart(RE::BipedAnim* a_biped, RE::BIPOBJECT* a_obj) {
            using func_t = void (*)(RE::BipedAnim*, RE::BIPOBJECT*, bool, std::int32_t);
            static REL::Relocation<func_t> func{ REL::RelocationID(15496, 15661) };
            func(a_biped, a_obj, true, 0);
        }

        // Actor::AttachWeapon - stages objects[slot].item/.part and runs the
        // part-3D loader (our styling hook). For the player it covers the 1st-
        // and 3rd-person bipeds internally. This is the engine's OWN equip-path
        // entry point. SE 19342 (0x140295130) / AE 19769 (0x1402E92A0).
        void AttachWeapon(RE::Actor* a_actor, RE::TESForm* a_weapon, bool a_leftHand) {
            using func_t = void (*)(RE::Actor*, RE::TESForm*, bool);
            static REL::Relocation<func_t> func{ REL::RelocationID(19342, 19769) };
            func(a_actor, a_weapon, a_leftHand);
        }

        // BipedAnim ammo attach - the AMMO mirror of AttachWeaponPart, and the
        // reason OS-76's weapon fix could not simply be widened to cover ammo.
        // Actor::AttachWeapon is UNUSABLE here, verified on the binaries rather
        // than assumed (OS-77 flagged exactly this as needing a check, since the
        // signature's TESForm* makes it look permissive): AttachWeaponPart opens
        //     cmp byte ptr [rdx+0x1a], 0x29   ; formType == kWeapon (41)
        //     jne <exit>
        // so an AMMO form (kAmmo, 0x2A) is rejected on the FIRST instruction -
        // SE 1401C85D0 / AE 140214BD0, identical on both. Ammo therefore has its
        // own entry point, the same function shape gated on 0x2A instead:
        //     SE 15511 (0x1401C8D40)  cmp byte ptr [rdx+0x1a], 0x2A
        //     AE 15688 (0x1402154A0)  cmp byte ptr [rdx+0x1a], 0x2A
        // Both byte-verified on the unpacked binaries; prologue, the +0x2770
        // biped member and the form gate all match across runtimes.
        //
        // TWO args, no leftHand - ammo has exactly one biped slot (kQuiver).
        // This is also the very function whose inner part-3D call WeaponHooks
        // already hooks (SE 15511+0x141 / AE 15688+0x199), so calling it is what
        // runs our own styling hook.
        void AttachAmmoPart(RE::BipedAnim* a_biped, RE::TESForm* a_ammo) {
            using func_t = void (*)(RE::BipedAnim*, RE::TESForm*);
            static REL::Relocation<func_t> func{ REL::RelocationID(15511, 15688) };
            func(a_biped, a_ammo);
        }

        // Actor virtual 0xB4 - the hip<->hand re-parent, the ONLY thing that
        // moves a weapon between its sheath node and the hand node. Called as a
        // VIRTUAL, never by address: the `Actor` implementation only touches the
        // 3rd-person root, and it is the `PlayerCharacter` OVERRIDE that also
        // does first person (SE 39404 / AE 40479 vs SE 36383 / AE 37374). Slot
        // verified on both binaries - PlayerCharacter's vtable 0xB4 is
        // SE 1406A1BF0 / AE 140736500, and the callee opens `movzx edi, r9b`,
        // i.e. arg4 is the bool, which is the signature below.
        //
        // Self-defers through the engine's own task queue when it is called off
        // the safe window, so it does not need marshalling here. CommonLibSSE
        // declares this slot only as Unk_B4(void).
        void UpdateWeaponNode(RE::Actor* a_actor, RE::TESObjectWEAP* a_weapon, bool a_draw,
                              bool a_leftHand) {
            using func_t         = void (*)(RE::Actor*, RE::TESObjectWEAP*, bool, bool);
            auto** const vtbl    = *reinterpret_cast<void***>(a_actor);
            const auto   reparent = reinterpret_cast<func_t>(vtbl[0xB4]);
            reparent(a_actor, a_weapon, a_draw, a_leftHand);
        }

        enum class WeaponPartSide {
            kMainHand,
            kOffHand,
            kAny,
        };

        [[nodiscard]] bool WeaponSlotMatchesSide(
            std::uint32_t a_slot, WeaponPartSide a_side) {
            return a_side == WeaponPartSide::kAny ||
                   (a_side == WeaponPartSide::kOffHand &&
                    IsOffHandWeaponBipedSlot(a_slot)) ||
                   (a_side == WeaponPartSide::kMainHand &&
                    IsMainHandWeaponBipedSlot(a_slot));
        }

        // Read the visual placement BEFORE UpdateEquipment rebuilds it. Menu
        // entry can temporarily make ActorState say "sheathed" even though the
        // existing clone is still on WEAPON/SHIELD in the player's hand.
        [[nodiscard]] bool WeaponPartWasDrawn(
            RE::BipedAnim* a_biped, RE::TESForm* a_weapon,
            WeaponPartSide a_side) {
            if (!a_biped || !a_weapon) {
                return false;
            }
            for (std::size_t s = 0;
                 s <= static_cast<std::size_t>(RE::BIPED_OBJECTS::kQuiver);
                 ++s) {
                if (!WeaponSlotMatchesSide(
                        static_cast<std::uint32_t>(s), a_side)) {
                    continue;
                }
                const auto& obj = a_biped->objects[s];
                if (obj.item != a_weapon || !obj.partClone ||
                    !obj.partClone->parent) {
                    continue;
                }
                const char* const parent =
                    obj.partClone->parent->name.c_str();
                if (PreserveDrawnWeaponPlacement(
                        false, parent ? std::string_view{ parent }
                                      : std::string_view{})) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::uint8_t CaptureDrawnWeaponHands(
            RE::Actor* a_actor) {
            constexpr std::uint8_t kRight = 1u << 0;
            constexpr std::uint8_t kLeft  = 1u << 1;
            const bool stateDrawn =
                a_actor->AsActorState()->IsWeaponDrawn();
            const bool isPlayer =
                RE::PlayerCharacter::GetSingleton() == a_actor;
            std::uint8_t result = 0;
            for (const bool leftHand : { false, true }) {
                auto* const weapon = a_actor->GetEquippedObject(leftHand);
                if (!weapon || !weapon->IsWeapon()) {
                    continue;
                }
                const auto side = leftHand ? WeaponPartSide::kOffHand
                                           : WeaponPartSide::kMainHand;
                const bool visuallyDrawn =
                    WeaponPartWasDrawn(
                        a_actor->GetBiped1(false).get(), weapon, side) ||
                    (isPlayer &&
                     WeaponPartWasDrawn(
                         a_actor->GetBiped1(true).get(), weapon, side));
                if (PreserveDrawnWeaponPlacement(
                        stateDrawn, visuallyDrawn ? "WEAPON" : "")) {
                    result |= leftHand ? kLeft : kRight;
                }
            }
            return result;
        }

        // Tear the weapon's node off one biped so the change-detect documented
        // on RestyleEquippedWeapons stops short-circuiting. Main-hand weapons
        // occupy class slots 32..40; off-hand weapons occupy the race's
        // shield/editor slot below 32. Keeping those domains separate is
        // load-bearing when both hands use the SAME WEAP form: the main-hand
        // pass must not detach the off-hand clone before its own attach pass.
        // Returns true if a node in the requested domain was detached.
        bool DetachWeaponFrom(RE::BipedAnim* a_biped, RE::TESForm* a_weapon,
                              WeaponPartSide a_side = WeaponPartSide::kAny) {
            if (!a_biped || !a_weapon) {
                return false;
            }
            bool any = false;
            for (std::size_t s = 0;
                 s <= static_cast<std::size_t>(RE::BIPED_OBJECTS::kQuiver); ++s) {
                if (!WeaponSlotMatchesSide(
                        static_cast<std::uint32_t>(s), a_side)) {
                    continue;
                }
                auto& obj = a_biped->objects[s];
                if (obj.item != a_weapon || !obj.partClone) {
                    continue;
                }
                ClearBipedPart(a_biped, &obj);
                any = true;
                // Gate 2 of the same change-detect compares objects[slot]
                // against bufferedObjects[slot] and also skips the load when
                // they agree. Whether the teardown above already breaks that
                // comparison is the ONE thing that could not be settled off the
                // binaries, so report its four inputs rather than guess: if a
                // field log still shows `weapon parts loaded 0x` after this,
                // these numbers say immediately whether gate 2 is the blocker
                // (all four non-zero and matching) or something else is.
                const auto& buf = a_biped->bufferedObjects[s];
                spdlog::debug("restyle: slot {} cleared - part={} partClone={} "
                              "buf.part={} buf.partClone={} buf.itemMatch={}",
                              s, static_cast<const void*>(obj.part),
                              static_cast<const void*>(obj.partClone.get()),
                              static_cast<const void*>(buf.part),
                              static_cast<const void*>(buf.partClone.get()),
                              buf.item == obj.item);
            }
            return any;
        }

        // Move the exact offhand biped clone to Skyrim's left-hand attachment
        // node. Field logging disproved the earlier "left attach already
        // targets the hand" assumption: third person returned on
        // WeaponSwordLeft (the sheath), while first person returned on SHIELD.
        //
        // Do not use Actor virtual 0xB4 for this side. That virtual chooses a
        // sheath child rather than accepting the clone, so same-form dual
        // wield can move the other hand's child[0]. The biped slot gives us the
        // unambiguous clone and biped->root keeps 1P and 3P node trees separate.
        bool RestoreOffHandWeaponPart(
            RE::BipedAnim* a_biped, RE::TESForm* a_weapon,
            const char* a_perspective) {
            if (!a_biped || !a_biped->root || !a_weapon) {
                return false;
            }

            static const RE::BSFixedString kLeftHandNode{ "SHIELD" };
            auto* const targetObject =
                a_biped->root->GetObjectByName(kLeftHandNode);
            auto* const target = targetObject ? targetObject->AsNode() : nullptr;
            if (!target) {
                spdlog::warn(
                    "restyle: cannot restore left-hand '{}' {} clone: "
                    "SHIELD node is missing.",
                    a_weapon->GetName() ? a_weapon->GetName() : "?",
                    a_perspective);
                return false;
            }

            bool any = false;
            for (std::size_t s = 0;
                 s <= static_cast<std::size_t>(RE::BIPED_OBJECTS::kQuiver);
                 ++s) {
                if (!WeaponSlotMatchesSide(
                        static_cast<std::uint32_t>(s),
                        WeaponPartSide::kOffHand)) {
                    continue;
                }
                auto& obj = a_biped->objects[s];
                if (obj.item != a_weapon || !obj.partClone) {
                    continue;
                }

                // Keep a strong reference across DetachChild. BIPOBJECT also
                // owns one, but the local makes this operation safe even if an
                // engine callback mutates that slot while the node is moving.
                RE::NiPointer<RE::NiAVObject> clone = obj.partClone;
                auto* const oldParent = clone->parent;
                const char* const oldParentName =
                    oldParent ? oldParent->name.c_str() : nullptr;
                const std::string_view oldName =
                    oldParentName ? std::string_view{ oldParentName }
                                  : std::string_view{};

                if (oldParent != target &&
                    OffHandCloneNeedsHandReparent(true, oldName)) {
                    if (oldParent) {
                        RE::NiPointer<RE::NiAVObject> detached;
                        oldParent->DetachChild(clone.get(), detached);
                        if (clone->parent == oldParent) {
                            spdlog::warn(
                                "restyle: could not detach left-hand '{}' {} "
                                "clone from '{}'.",
                                a_weapon->GetName()
                                    ? a_weapon->GetName()
                                    : "?",
                                a_perspective,
                                oldParentName && *oldParentName
                                    ? oldParentName
                                    : "<unnamed>");
                            continue;
                        }
                    }
                    target->AttachChild(clone.get(), true);
                }

                clone->SetAppCulled(false);
                const char* const newParentName =
                    clone->parent ? clone->parent->name.c_str() : nullptr;
                spdlog::debug(
                    "restyle: restored left-hand '{}' {} clone in slot {} "
                    "(parent '{}' -> '{}').",
                    a_weapon->GetName() ? a_weapon->GetName() : "?",
                    a_perspective, s,
                    oldParentName && *oldParentName ? oldParentName : "<none>",
                    newParentName && *newParentName
                        ? newParentName
                        : "<none>");
                any = true;
            }
            return any;
        }
    }  // namespace

    void RestyleEquippedWeapons(
        RE::Actor* a_actor, std::uint8_t a_preserveDrawnHands) {
        if (!a_actor) {
            return;
        }
        // OS-76, the real mechanism (2026-07-18). The drain above IS genuine and
        // does run - but it cannot help by itself, and neither can any number of
        // refreshes. Everything that attaches a weapon, the drain and the normal
        // equip path alike, funnels into BipedAnim::AttachWeaponPart
        // (SE 15506 / AE 15683), which opens with a change-detect that returns
        // BEFORE the part-3D loader when
        //
        //     weapon == objects[slot].item  &&  objects[slot].partClone != nullptr
        //
        // (raw bytes SE 1401C8679..1401C8689 / AE 140214C83..140214C98, each
        // jumping to the function epilogue; verified on both unpacked binaries).
        // On a refresh the weapon is still equipped and its node is still
        // parented, so both halves hold, the loader never runs, and our styling
        // hook never sees the part. That is exactly what the field log showed:
        // 27 refreshes, `weapon parts loaded 0x` on every one, while the armor
        // pass ran fine each time - armor never goes through this function, the
        // 42-slot resolver loads armor parts inline. Unequipping is what clears
        // partClone, which is why "unequip and re-equip" was the ONLY thing that
        // ever applied a weapon style.
        //
        // So stop trying to make a refresh reach the loader and do what an equip
        // does: tear the node down, then re-attach through the engine's own
        // entry point.
        //
        // Two traps, both load-bearing:
        //  - do NOT null partClone by hand instead of calling ClearBipedPart -
        //    the teardown early-returns on a null partClone, so the old node
        //    would stay parented and the player would visibly wear two weapons;
        //  - do NOT null item either - the teardown keys off item->formType to
        //    take its weapon-aware branch, and without it the scabbard is left
        //    behind.
        //
        // Both bipeds, because AttachWeapon re-attaches 1st- AND 3rd-person
        // internally: detach only the 3rd and the stale 1st-person node still
        // satisfies its own gate, so first person keeps the unstyled mesh.
        // The attach ALWAYS parks a weapon on its sheath node - it never reads
        // weapon state to decide placement (the resolver reads it only to set
        // kHidden, and only on the left-hand and staff branches). The hip->hand
        // move is a separate engine step that normally runs on a draw/sheathe
        // transition, and nothing on the attach path calls it. So a forced
        // re-attach while the weapon is DRAWN leaves it on the hip in third
        // person and, in first person, parked at the 1P sheath node - which is
        // off-camera, so it reads as "invisible" rather than misplaced. Both of
        // the field symptoms are that one fact; restoring the placement fixes
        // both. (Field 2026-07-18, first build of the OS-76 fix.)
        const bool isPlayer =
            RE::PlayerCharacter::GetSingleton() == a_actor;

        for (const bool leftHand : { false, true }) {
            auto* const weapon = a_actor->GetEquippedObject(leftHand);
            // Weapons only. Ammo attaches through its own call site, and a
            // torch/shield/spell in the off hand is not a styling surface.
            if (!weapon || !weapon->IsWeapon()) {
                continue;
            }
            const auto side = leftHand ? WeaponPartSide::kOffHand
                                       : WeaponPartSide::kMainHand;
            const bool d3 = DetachWeaponFrom(
                a_actor->GetBiped1(false).get(), weapon, side);
            const bool d1 =
                isPlayer && DetachWeaponFrom(
                                a_actor->GetBiped1(true).get(), weapon, side);
            if (!d3 && !d1) {
                continue;
            }
            AttachWeapon(a_actor, weapon, leftHand);

            // AttachWeapon parks both replacement sides at their attachment
            // defaults. The right can use virtual 0xB4. The left uses its exact
            // offhand biped clone, because 0xB4's child selection is ambiguous
            // when both hands use the same WEAP form.
            const bool preserveDrawn =
                (a_preserveDrawnHands & (leftHand ? 1u << 1 : 1u << 0)) != 0;
            switch (DrawnWeaponRepairFor(preserveDrawn, leftHand)) {
                case DrawnWeaponRepair::ReparentRight:
                    if (auto* const weap =
                            weapon->As<RE::TESObjectWEAP>()) {
                        UpdateWeaponNode(a_actor, weap, true, false);
                        spdlog::debug(
                            "restyle: re-parented '{}' to the right hand node "
                            "(weapon drawn).",
                            weap->GetName() ? weap->GetName() : "?");
                    }
                    break;
                case DrawnWeaponRepair::ReparentLeftClone:
                    RestoreOffHandWeaponPart(
                        a_actor->GetBiped1(false).get(), weapon,
                        "third-person");
                    if (isPlayer) {
                        RestoreOffHandWeaponPart(
                            a_actor->GetBiped1(true).get(), weapon,
                            "first-person");
                    }
                    break;
                case DrawnWeaponRepair::None:
                    break;
            }
        }

        // Some followers carry an engine-owned/template display weapon that is
        // already materialized in BipedAnim but is absent from inventory and
        // therefore invisible to GetEquippedObject above. Do not invent an
        // item: rebuild only the EXISTING visual biped object, and only in
        // slots whose hand cannot be ambiguous. That reaches default bows such
        // as Jenassa's while refusing to guess for one-handed weapons/staves.
        if (!isPlayer) {
            auto* const b3 = a_actor->GetBiped1(false).get();
            if (b3) {
                auto* const equippedRight = a_actor->GetEquippedObject(false);
                auto* const equippedLeft  = a_actor->GetEquippedObject(true);
                for (std::uint32_t slot = 32; slot <= 40; ++slot) {
                    if (!IsUnambiguousVisualWeaponSlot(slot)) {
                        continue;
                    }
                    auto* const visual = b3->objects[slot].item;
                    if (!visual || !visual->IsWeapon() ||
                        visual == equippedRight || visual == equippedLeft ||
                        !b3->objects[slot].partClone) {
                        continue;
                    }
                    if (DetachWeaponFrom(
                            b3, visual, WeaponPartSide::kMainHand)) {
                        AttachWeapon(a_actor, visual, false);
                        spdlog::info(
                            "restyle: rebuilt visual-only follower weapon '{}' "
                            "from biped slot {} (inventory unchanged).",
                            visual->GetName() ? visual->GetName() : "?", slot);
                    }
                }
            }
        }

        // OS-77: the quiver, which the loop above can never reach. Ammo is not a
        // hand slot, so GetEquippedObject never returns it, and Actor::AttachWeapon
        // would reject it anyway (see AttachAmmoPart). But it hits the SAME
        // change-detect as weapons did, which is why a quiver style needed an
        // unequip/re-equip to show. Same remedy: tear the node down, re-attach
        // through the engine's own ammo entry point.
        //
        // Per biped BY HAND. Actor::AttachWeapon walks the 3rd- and 1st-person
        // bipeds internally and calls the biped-level attach on each; ammo has no
        // Actor-level counterpart that does the walk, so we do it. Both bipeds for
        // the OS-76 reason: detach only the 3rd and the stale 1st-person node
        // still satisfies its own gate.
        if (auto* const ammo = a_actor->GetCurrentAmmo()) {
            auto* const b3 = a_actor->GetBiped1(false).get();
            auto* const b1 = isPlayer ? a_actor->GetBiped1(true).get() : nullptr;
            const bool  d3 = DetachWeaponFrom(b3, ammo);
            const bool  d1 = DetachWeaponFrom(b1, ammo);
            if (d3 || d1) {
                if (b3) {
                    AttachAmmoPart(b3, ammo);
                }
                if (b1) {
                    AttachAmmoPart(b1, ammo);
                }
                // `drawn` is logged, not acted on. The weapon path needs an
                // explicit hip->hand restore after a forced re-attach; the quiver
                // has no such move (it stays on the back in both states). The open
                // question is the NOCKED arrow, which OS-75 records as cloned from
                // the quiver 3D - so a re-attach mid-draw could plausibly disturb
                // it. Nothing on the binaries settles that, so field-test it with a
                // bow drawn and an arrow nocked, and read this line when reporting.
                spdlog::debug("restyle: quiver re-attached '{}' (drawn={}).",
                              ammo->GetName() ? ammo->GetName() : "?",
                              a_preserveDrawnHands != 0);
            }
        }
    }

    namespace {
        // GetName() can return null (StyleCatalog/EditorUI guard the same
        // thing for ARMO forms - "std::string + null is UB"); a debug log is
        // not worth risking a bad fmt call over.
        const char* SafeName(RE::Actor* a_actor) {
            const char* n = a_actor->GetName();
            return n ? n : "?";
        }
    }  // namespace

    void RefreshActor(RE::Actor* a_actor, bool a_sceneKick) {
        if (!a_actor) {
            spdlog::warn("RefreshActor: null actor.");
            return;
        }
        auto*      player   = RE::PlayerCharacter::GetSingleton();
        const bool isPlayer = (player && a_actor == player);

        auto* proc = a_actor->GetActorRuntimeData().currentProcess;
        // An unloaded / low-process actor has no biped to rebuild - it
        // restyles naturally on its next load (the render hooks are always
        // armed), so this function does not force one. middleHigh (proc+0x08)
        // is our loaded-enough gate; note UpdateEquipment does its OWN early
        // return on high (proc+0x10) being null (disasm: `cmp [rcx+0x10],0;
        // je end`), so a middle-high-but-not-high actor simply no-ops here
        // rather than crashing - the Get3D(false) check is the real "has a
        // rendered biped" signal. In practice this never trips for the
        // player: RequestRefresh's task only ever runs while the player is
        // loaded and controllable, so a hit there is a real signal something
        // upstream broke and stays a WARN; for an NPC "not loaded yet" is
        // routine (a dismissed/away follower, or one still streaming in), so
        // it is a quiet DEBUG.
        if (!proc || !proc->middleHigh || !a_actor->Get3D(false)) {
            if (isPlayer) {
                spdlog::warn("RefreshActor: player has no AIProcess / is not loaded - skipping.");
            } else {
                spdlog::debug("RefreshActor: '{}' not loaded (no AIProcess/biped) - skipping.",
                              SafeName(a_actor));
            }
            return;
        }

        // Proven on 1.5.97 (checkpoint 2): this synchronous rebuild runs even
        // while Container/Barter pause the game - unchanged for NPCs, same
        // engine calls, just retargeted. The worn-pass tripwire stays
        // PLAYER-scoped (BipedHooks::PlayerWornPassCount never moves for an
        // NPC rebuild - see BipedHooks.h); the weapon tripwire has a matching
        // NPC-scoped counter (WeaponHooks::NpcWeaponPartCount).
        const auto c0 = isPlayer ? BipedHooks::PlayerWornPassCount() : 0;
        const auto w0 = isPlayer ? WeaponHooks::PlayerWeaponPartCount()
                                 : WeaponHooks::NpcWeaponPartCount();
        // Capture each hand's visual truth before UpdateEquipment can replace
        // a hand-parented clone with a sheath-parented one during menu entry.
        const auto preserveDrawnHands =
            CaptureDrawnWeaponHands(a_actor);

        // CrashGuard's pending-preview marker is a SINGLE global key, not
        // actor-scoped (CrashGuard.h) - bracketing a non-player refresh with
        // it would blame whatever style the editor happens to be previewing
        // right now for a rebuild that may have nothing to do with it (a
        // race-switch resume, the co-save load catch-up pass, another NPC's
        // Apply). Design §7 is explicit: batch/NPC refreshes never run inside
        // a BeginKick bracket - only the player path (one target, one live
        // preview at a time) keeps the original bracket.
        if (isPlayer) {
            CrashGuard::BeginKick();
        }
        SetEquipFlag(proc);
        UpdateEquipment(proc, a_actor);
        if (isPlayer) {
            // OS-76: the rebuild just queued the player's weapons and nothing
            // will drain that queue while the menu holds the world paused.
            // Drain it inside the SAME crash-guard bracket as the rebuild that
            // filled it. Unconditional on purpose: with an empty queue this is
            // ten null tests, and gating it on "is weapon styling active" would
            // leave the vanilla under-pause behaviour inconsistent for no gain.
            // NPCs need no equivalent - the resolver attaches theirs inline.
            FlushPendingWeapons(player);
        }
        // Draining is necessary but NOT sufficient for the player, and NPC
        // refreshes hit the same already-attached-node short circuit. Force
        // the part loader for either actor so switching a follower outfit
        // previews and applies weapon looks without an unequip/re-equip.
        RestyleEquippedWeapons(a_actor, preserveDrawnHands);
        if (isPlayer) {
            CrashGuard::EndKick();
        }

        // Preview modes deliberately suppress unrelated equipment nodes.
        // Presets retain linked weapon and shield entries for saving while
        // hiding their preview objects. A follower mannequin also hides the
        // player's own weapons, shield and quiver because they do not describe
        // the follower. Immediate and queued sweeps cover sync and late clones.
        auto& session = OutfitSession::GetSingleton();
        if (const auto mask = session.PreviewEquipmentSuppressionMask(a_actor);
            mask != 0) {
            BipedPost::CullObjectNodes(a_actor->GetCurrentBiped().get(), mask);
            BipedPost::QueueObjectNodeCull(a_actor->GetHandle(), mask);
        }

        if (isPlayer) {
            const auto c1 = BipedHooks::PlayerWornPassCount();
            spdlog::debug("refresh: wornpass ran {}x", c1 - c0);
            if (c1 == c0) {
                spdlog::warn("refresh: skinning pass did NOT run - the override will not update visually.");
            }

            // Weapon dimension, same tripwire shape. Unlike the worn pass, the
            // part-3D loader is only reached when a weapon or quiver is
            // equipped, so zero loads is perfectly benign for an unarmed player -
            // but it is also exactly how a DEAD hook site presents (the AE
            // inlined-site bug: address resolves, byte check passes, thunk never
            // runs). Ambiguous evidence beats none: this is the line that turns
            // "the styled sword just doesn't show" into a five-second diagnosis
            // from a field log, so it says both things it could mean.
            if (OutfitSession::GetSingleton().AnyWeaponStyling()) {
                const auto w1 = WeaponHooks::PlayerWeaponPartCount();
                spdlog::debug("refresh: weapon parts loaded {}x", w1 - w0);
                if (w1 == w0) {
                    spdlog::warn("refresh: NO weapon parts loaded while weapon styling is active - "
                                 "expected if no weapon or ammo is equipped, otherwise the part-3D "
                                 "call-site hooks are not firing on this runtime.");
                }
            }
        } else {
            // NPC tripwire: debug only, never warn - a styled NPC with
            // nothing equipped in a weapon slot (armor-only assignment, or
            // simply unarmed) is completely benign, and this refresh path
            // serves those NPCs too, so a zero delta alone is not evidence of
            // anything broken. Enough to turn "the styled sword doesn't show
            // on my follower" into a one-line field-log check.
            const auto w1 = WeaponHooks::NpcWeaponPartCount();
            spdlog::debug("refresh actor '{}': weapon parts loaded {}x", SafeName(a_actor),
                         w1 - w0);
        }

        if (a_sceneKick) {
            // Container/Barter pause the game; kick the actor's scene graph so
            // the rebuilt biped renders immediately (0x2000 flag from IED).
            if (auto* root = a_actor->Get3D(false)) {
                RE::NiUpdateData ctx;
                ctx.time  = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                root->UpdateWorldBound();
                root->Update(ctx);
            }
        }
        spdlog::debug("RefreshActor '{}' done (kick={}).", SafeName(a_actor), a_sceneKick);
    }

    void RefreshPlayer(bool a_sceneKick) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            spdlog::warn("RefreshPlayer: no player.");
            return;
        }
        RefreshActor(player, a_sceneKick);
    }

    // Mirrors RE::Actor::GetSkin() (Actor.h:530) deliberately: actor override
    // skin (TESNPC::skin) first, then race->skin. Consumed only as an identity
    // filter ("is this ARMO the naked body", Task 3.1). WARNING: the engine's
    // cleared-slot base body is applied as ApplyArmorAddon(race->skin) inside
    // func 15499 - race->skin specifically. So if the Hide-body-slot fallback
    // is ever built, it must re-apply race->skin, NOT this base-then-race
    // result: a custom-race/body mod that sets TESNPC::skin would diverge.
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
