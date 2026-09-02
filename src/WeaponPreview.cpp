#include "WeaponPreview.h"

#include "PCH.h"

#include "REAugments.h"

#include <spdlog/spdlog.h>

#include <atomic>

namespace OS::WeaponPreview {

    namespace {

        // What we put up, so we can take exactly it down again.
        //
        // ⚠ THE HANDLE, NOT THE Actor*. An actor pointer outlives nothing in
        // particular: the cell can change, the follower can be unloaded, and
        // the editor can sit open across all of it. A handle tells us whether
        // the actor we attached to is still there before we write to a biped.
        struct Held {
            RE::ActorHandle    actor;
            RE::TESForm*       weapon{ nullptr };
            std::uint32_t      slot{ 0 };
            bool               active{ false };
            // The player's OWN weapon we took this slot from, if any, and
            // whether it was hand-parented when we took it. Both are the
            // restore, and the restore is the whole cost of this feature.
            RE::TESForm*       displaced{ nullptr };
            bool               displacedDrawn{ false };
            // ⚠ WHERE IT WAS, WHICH IS NOT WHERE THE PREVIEW WENT. A sword
            // lives in slot 33 and a browsed greatsword goes to 37; the
            // teardown has to clear the slot the REAL piece occupied, not the
            // one we borrowed. Conflating them is what left the player
            // carrying both (field 2026-08-11 evening).
            std::uint32_t      displacedSlot{ 0 };
        };

        Held g_held;

        // Every biped a preview can be sitting on. The player has two and the
        // attach covers both internally, so the teardown has to as well - a
        // 3P-only detach leaves the 1P node satisfying its own change-detect,
        // which is the exact shape of the OS-76 residue.
        template <class Fn>
        void ForEachBiped(RE::Actor* a_actor, Fn&& a_fn) {
            if (!a_actor) {
                return;
            }
            if (auto* third = a_actor->GetBiped1(false).get()) {
                a_fn(third, false);
            }
            if (RE::PlayerCharacter::GetSingleton() == a_actor) {
                if (auto* first = a_actor->GetBiped1(true).get()) {
                    a_fn(first, true);
                }
            }
        }

        // objects[a_slot].item, or null if anything on the way is missing.
        RE::TESForm* SlotItem(RE::Actor* a_actor, std::uint32_t a_slot) {
            auto* biped = a_actor ? a_actor->GetBiped1(false).get() : nullptr;
            if (!biped || a_slot >= 42u) {
                return nullptr;
            }
            return biped->objects[a_slot].item;
        }

        // Is our preview STILL ON THE BIPED, asked of the biped itself?
        //
        // ⚠⚠ THIS IS THE WHOLE FIX FOR "I CLICK AND NOTHING APPEARS" (field
        // 2026-08-11 evening). The first version answered "already showing"
        // out of g_held alone, which is a record of what we DID, not of what
        // is there. Every staging update rebuilds the biped and restages
        // objects[], so the engine silently takes our node - and the record
        // still said "showing", so the next pass declined with
        // kAlreadyShowing and NEVER PUT IT BACK. The log shows it exactly:
        // one `hung 'Ebony Dagger' on slot 34`, then `no longer ours` every
        // frame for the rest of the session. Clicking to another class and
        // back was the only escape, because that changed g_held.slot.
        //
        // An API you write to is not a source of truth. Ask the biped.
        // Which biped slot a form is actually staged in, or kNoSlot.
        //
        // ⚠ MEASURED, NOT DERIVED FROM THE FORM'S CLASS. The class table says
        // where a weapon SHOULD go; this says where it IS. They agree almost
        // always and the teardown has to be right in the case they do not,
        // because clearing the wrong slot leaves the player's own weapon
        // parented with nothing left that knows about it.
        constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

        std::uint32_t StagedSlotOf(RE::Actor* a_actor, RE::TESForm* a_form) {
            auto* biped = a_actor ? a_actor->GetBiped1(false).get() : nullptr;
            if (!biped || !a_form) {
                return kNoSlot;
            }
            // 32..41: every main-hand class slot plus the quiver. The off-hand
            // domain below 32 is deliberately not searched - this module never
            // touches the off hand, and a shield or torch down there must not
            // be mistaken for the piece we are hiding.
            for (std::uint32_t b = 32; b <= 41u; ++b) {
                if (biped->objects[b].item == a_form) {
                    return b;
                }
            }
            return kNoSlot;
        }

        bool SlotHoldsOurs(RE::Actor* a_actor, std::uint32_t a_slot,
                           RE::TESForm* a_weapon) {
            auto* biped = a_actor ? a_actor->GetBiped1(false).get() : nullptr;
            if (!biped || a_slot >= 42u || !a_weapon) {
                return false;
            }
            const auto& obj = biped->objects[a_slot];
            // BOTH halves. `item` alone would be satisfied while the clone is
            // still being built, and a claim with no geometry is the thing
            // that made this invisible in the first place.
            return obj.item == a_weapon && obj.partClone != nullptr;
        }

        // ⚠ ONE LINE PER STATE CHANGE, NOT ONE PER FRAME. The first version
        // logged every present: 28804 lines and 3.9 MB in a single session,
        // 23156 of them the same "already showing". This file's neighbours
        // already learned that lesson - BipedHooks' per-pass `masks:` dump was
        // deleted for being 543 lines a session, which is two percent of this.
        // A decline that repeats is one reading, not thousands.
        struct LastSaid {
            std::uint32_t slot{ 0 };
            Decline       why{ Decline::kNone };
            const void*   weapon{ nullptr };
            bool          valid{ false };
        };
        LastSaid g_said;

        bool WorthSaying(std::uint32_t a_slot, Decline a_why, const void* a_weapon) {
            if (g_said.valid && g_said.slot == a_slot && g_said.why == a_why &&
                g_said.weapon == a_weapon) {
                return false;
            }
            g_said = LastSaid{ a_slot, a_why, a_weapon, true };
            return true;
        }

    }  // namespace

    bool Showing() { return g_held.active; }

    namespace {
        // ⚠ ATOMIC BECAUSE THE TWO SIDES ARE DIFFERENT THREADS. The editor
        // writes it from the present thread while it draws; the render pass
        // reads it on the game thread. A relaxed load is enough - a one-frame
        // stale answer only ever costs one frame of a shield that is on its
        // way in or out, and both directions are harmless.
        std::atomic<bool> g_shieldRowActive{ false };
    }

    void SetShieldRowActive(bool a_active) {
        g_shieldRowActive.store(a_active, std::memory_order_relaxed);
    }

    bool ShieldRowActive() {
        return g_shieldRowActive.load(std::memory_order_relaxed);
    }

    void Clear() {
        if (!g_held.active) {
            return;
        }
        auto* const actor = g_held.actor.get().get();
        if (!actor) {
            // The actor is gone, and so is its biped with it. There is nothing
            // left to detach from and nothing leaked: the nodes died with the
            // 3D. Drop the record so the next Show starts clean.
            spdlog::debug("weapon preview: actor gone before teardown; "
                          "dropping the record for slot {}.", g_held.slot);
            g_held = Held{};
            return;
        }

        // ⚠ ORDER IS LOAD-BEARING AND BOTH HALVES ARE WRITTEN DOWN IN
        // [[skyrim-weapon-attach-change-detect]]. ClearBipedPart FIRST, while
        // .item is still set, because its teardown keys off item->formType to
        // take the weapon-aware branch - that branch is what removes the
        // SCABBARD. Nulling .item first leaves the scabbard behind. And never
        // null partClone by hand instead of calling it: the teardown
        // early-returns on a null partClone, so the old node stays parented and
        // the character visibly wears two weapons.
        bool detached = false;
        bool stillOurs = false;
        ForEachBiped(actor, [&](RE::BipedAnim* a_biped, bool) {
            if (g_held.slot >= 42u) {
                return;
            }
            auto& obj = a_biped->objects[g_held.slot];
            if (obj.item != g_held.weapon) {
                // Somebody else owns this slot now - the player equipped a real
                // weapon, or a biped rebuild restaged it. Taking theirs down
                // would be the bug this whole module exists to avoid.
                return;
            }
            stillOurs = true;
            REAug::ClearWeaponPart(a_biped, g_held.slot);
            detached = true;
        });

        // ⚠ RELEASE THE RECORD WHEN THERE IS NOTHING OF OURS LEFT TO RELEASE,
        // WHICH IS NOT THE SAME AS "the detach succeeded".
        // [[release-ownership-only-when-the-restore-succeeds]] is about a
        // restore that FAILED while our geometry is still parented - clear the
        // flag there and the next capture adopts the fault as its baseline.
        // That is not this case, and reading it as this case is what produced
        // the field bug: when a rebuild restages the slot, our node is already
        // gone with it, so "nothing detached" meant "nothing to detach" and
        // keeping the record made the module spin on a slot it no longer owned
        // for the rest of the session.
        //
        // So: ours and detached -> done. Not ours on any biped -> also done,
        // because there is no geometry of ours anywhere to leak. The only case
        // that keeps the record is the one the memory is actually about - the
        // slot IS ours and the detach did not run.
        if (detached || !stillOurs) {
            if (WorthSaying(g_held.slot, Decline::kNone, g_held.weapon)) {
                spdlog::debug(
                    "weapon preview: released slot {} ('{}') - {}.", g_held.slot,
                    g_held.weapon && g_held.weapon->GetName() ? g_held.weapon->GetName()
                                                              : "?",
                    detached ? "took it down" : "a rebuild had already taken it");
            }
            // ⚠ AND GIVE THE PLAYER THEIR OWN WEAPON BACK, BEFORE THE RECORD
            // GOES. This is the half that makes displacing real gear
            // acceptable at all, and it runs on the "a rebuild had already
            // taken it" branch too: a rebuild that restaged the slot restaged
            // it with whatever the engine thinks belongs there, which is not
            // guaranteed to be the piece we displaced, and re-attaching an
            // already-attached weapon is a no-op through the change-detect
            // rather than a fault.
            if (g_held.displaced) {
                spdlog::debug("weapon preview: restoring the player's own '{}' to "
                              "slot {} (was {}).",
                              g_held.displaced->GetName() ? g_held.displaced->GetName()
                                                          : "?",
                              g_held.slot,
                              g_held.displacedDrawn ? "drawn" : "sheathed");
                REAug::RestoreDisplacedWeapon(actor, g_held.displaced,
                                              g_held.displacedDrawn);
            }
            g_held = Held{};
        } else {
            spdlog::warn("weapon preview: slot {} is still ours and the detach did "
                         "NOT run; keeping the record so the next pass retries.",
                         g_held.slot);
        }
    }

    Decline Show(RE::Actor* a_actor, RE::TESForm* a_weapon,
                 WeaponClass a_class) {
        const auto slot = BipedSlotForClass(a_class);
        const bool haveActor =
            a_actor != nullptr && a_actor->GetBiped1(false).get() != nullptr;

        // Does the record claim this exact slot on this exact actor?
        const bool claimed = g_held.active && g_held.weapon == a_weapon &&
                             g_held.slot == slot &&
                             g_held.actor.get().get() == a_actor;

        // ⚠⚠ AND IS IT ACTUALLY THERE. The claim alone is what broke this in
        // the field: a rebuild takes the node, the record still says "showing",
        // and the preview never comes back. Both halves, every frame.
        const bool stillUp = claimed && SlotHoldsOurs(a_actor, slot, a_weapon);

        // ⚠ ASK THE SLOT ONLY ONCE WE KNOW IT IS ONE. BipedSlotForClass answers
        // 0 for kTotal, and objects[0] is a head armour slot. The same
        // predicate the policy uses, so the two cannot disagree about which
        // slots are attachable.
        //
        // ⚠ "TAKEN" MEANS "HOLDS SOMETHING THAT IS NOT THE PREVIEW WE PUT
        // THERE", and the ownership test is why. Comparing against a_weapon
        // instead would adopt the player's OWN weapon whenever they happen to
        // have the very form being previewed equipped - and then teardown
        // would call ClearWeaponPart on their real gear.
        auto* const held = SlotItem(a_actor, slot);
        const bool  ours = g_held.active && g_held.slot == slot &&
                          g_held.actor.get().get() == a_actor &&
                          held != nullptr && held == g_held.weapon;

        // ⚠⚠ THE PIECE TO HIDE IS THE ONE IN THE PLAYER'S HAND, WHEREVER IT IS
        // STAGED - not whatever happens to sit in the slot we want to borrow.
        // Browsing greatswords with a sword equipped puts the preview on slot
        // 37 and leaves the sword on 33, and the character carries both. That
        // was the field report, and it is why this looks up the real piece by
        // EQUIPMENT and then asks the biped where it landed.
        //
        // Ammo is the same question asked of GetCurrentAmmo: a quiver is not a
        // hand slot, so GetEquippedObject never returns it.
        const bool isAmmoDim = slot == 41u;
        auto* const realPiece =
            !haveActor ? nullptr
                       : (isAmmoDim
                              ? static_cast<RE::TESForm*>(a_actor->GetCurrentAmmo())
                              : a_actor->GetEquippedObject(false));
        const std::uint32_t realSlot = StagedSlotOf(a_actor, realPiece);
        const bool          mayDisplace =
            realPiece != nullptr && realPiece != a_weapon && realSlot != kNoSlot &&
            MayDisplaceWorn(true, realPiece->IsWeapon() || realPiece->IsAmmo(),
                            realSlot);

        // The preview's OWN slot is refused only when something we may not
        // touch is sitting in it: not ours, and not the real piece we are
        // about to take off anyway.
        const bool slotTaken = haveActor && held != nullptr && !ours &&
                               !(mayDisplace && realSlot == slot);

        const auto verdict =
            ShouldShow(haveActor, a_weapon != nullptr, slot, slotTaken, stillUp);
        if (verdict != Decline::kNone) {
            // ⚠ EVERY DECLINE SAYS SO, BUT ONCE PER STATE, NOT ONCE PER FRAME.
            // A path that returns quietly is undiagnosable; a path that returns
            // loudly 23000 times is undiagnosable in the other direction, and
            // the first cut of this managed both at the same time.
            if (WorthSaying(slot, verdict, a_weapon)) {
                spdlog::debug("weapon preview: declined for slot {} - {}.", slot,
                              Why(verdict));
            }
            if (verdict != Decline::kAlreadyShowing) {
                Clear();  // whatever was up is not what is wanted now
            }
            return verdict;
        }

        // ⚠ CAPTURE THE DISPLACED WEAPON BEFORE Clear(), AND BEFORE ANYTHING
        // TOUCHES THE BIPED. Once the teardown below runs, objects[slot] no
        // longer says what was there, and a restore cannot be reconstructed
        // from an actor whose 3D we have already edited.
        RE::TESForm*  displaced      = nullptr;
        bool          displacedDrawn = false;
        std::uint32_t displacedSlot  = 0;
        if (mayDisplace) {
            displaced     = realPiece;
            displacedSlot = realSlot;
            // ⚠ NOT AsActorState()->IsWeaponDrawn() ON ITS OWN. The editor is
            // a paused menu, and the inventory/menu transition transiently
            // reports a SHEATHED actor state while the 3D is still visibly
            // hand-parented. Reading the node the weapon actually hangs on is
            // the second source of truth PreserveDrawnWeaponPlacement exists
            // to fold in - get this wrong and the player's weapon comes back
            // on their hip while the game thinks it is drawn.
            // ⚠⚠ AND ONLY THE FIRST READING COUNTS. A rebuild restages the
            // slot with the real weapon SHEATHED - a forced attach always
            // parks on the sheath node - so re-measuring on the second and
            // every later displacement would answer "sheathed" for a weapon
            // the player had drawn, and they would get it back on their hip.
            // The first reading is the only one taken while the placement is
            // still the player's own.
            const bool renewing = g_held.active && g_held.displaced == displaced;
            if (renewing) {
                displacedDrawn = g_held.displacedDrawn;
                // ⚠ AND DO NOT HAND IT BACK ONLY TO TAKE IT STRAIGHT OFF
                // AGAIN. Clear() below would otherwise attach the player's
                // weapon and this pass would detach it one line later, on
                // every rebuild. Dropping the record here is safe precisely
                // because the same form is carried onto the new record.
                g_held.displaced = nullptr;
            } else if (displaced->IsAmmo()) {
                // A quiver sits on the back in both weapon states, so there is
                // no placement to preserve and nothing to repair on the way
                // back. Saying so beats an unexplained `false`.
                displacedDrawn = false;
                spdlog::debug("weapon preview: displacing the player's own ammo "
                              "'{}' from slot {} (a quiver has no drawn state).",
                              displaced->GetName() ? displaced->GetName() : "?",
                              displacedSlot);
            } else {
                // ⚠ ASK ABOUT THE REAL WEAPON'S OWN CLASS, NOT THE BROWSED
                // ONE. They are different whenever the player is looking at a
                // class they do not carry, which is the common case now that
                // the piece being hidden is "whatever is in their hand". The
                // browsed class would send WeaponParentNodeName looking down
                // an empty slot and answer "" - which reads as sheathed, and
                // would hand a drawn weapon back onto the hip.
                auto* const weap = displaced->As<RE::TESObjectWEAP>();
                const auto  realClass =
                    weap ? ClassFromAnimType(static_cast<std::uint8_t>(
                               weap->weaponData.animationType.underlying()))
                         : std::nullopt;
                auto* const state = a_actor->AsActorState();
                const bool  drawn = state && state->IsWeaponDrawn();
                const auto  node =
                    realClass ? REAug::WeaponParentNodeName(a_actor, *realClass,
                                                            WeaponHand::Right)
                              : std::string{};
                displacedDrawn = PreserveDrawnWeaponPlacement(drawn, node);
                spdlog::debug("weapon preview: displacing the player's own '{}' "
                              "from slot {} (state {}, node '{}' -> restore {}).",
                              displaced->GetName() ? displaced->GetName() : "?",
                              displacedSlot, drawn ? "drawn" : "sheathed",
                              node.empty() ? "<none>" : node.c_str(),
                              displacedDrawn ? "drawn" : "sheathed");
            }
        }

        Clear();  // one at a time - and this restores any EARLIER displacement

        // ⚠ AN OCCUPIED SLOT NEEDS THE TEARDOWN THE EMPTY ONE DOES NOT.
        // AttachWeaponPart's change-detect returns before the part loader when
        // the weapon is already the slot's item with a clone built, and more
        // to the point the real weapon's node is still parented - attaching
        // over it would leave the player wearing two. ClearWeaponPart is the
        // engine's own teardown and takes the scabbard with it.
        // ⚠ displacedSlot, NOT slot. The real piece is wherever the engine
        // staged IT - a sword on 33 while the browsed greatsword goes to 37 -
        // and clearing the slot we are about to borrow would leave the sword
        // exactly where the player could see it. That was the report.
        if (displaced) {
            ForEachBiped(a_actor, [&](RE::BipedAnim* a_biped, bool) {
                if (a_biped->objects[displacedSlot].item == displaced) {
                    REAug::ClearWeaponPart(a_biped, displacedSlot);
                }
            });
        }

        // ⚠ THE ATTACH IS THE WHOLE TRICK AND IT NEEDS NO TEARDOWN FIRST.
        // AttachWeaponPart's change-detect returns early only when the weapon
        // is ALREADY this slot's item and its clone is built. We refuse to run
        // on an occupied slot, so both halves are false here and the part
        // loader runs - which is also what puts our own weapon-styling hook in
        // the path, so the previewed weapon arrives already styled and dyed.
        //
        // ⚠ AND IT LANDS SHEATHED BY ITSELF. Nothing on this path performs the
        // hip-to-hand move, so there is no graph event, no idle selection and
        // no draw state to get wrong. That is the entire reason this feature is
        // small enough to write.
        // ⚠ AND THE QUIVER GOES THROUGH ITS OWN DOOR. Handing ammo to
        // Actor::AttachWeapon is a SILENT no-op - rejected on the first
        // instruction - which is exactly how "quivers and bolts do not appear"
        // looks from the outside.
        if (isAmmoDim) {
            REAug::AttachAmmoForPreview(a_actor, a_weapon);
        } else {
            REAug::AttachWeaponForPreview(a_actor, a_weapon, /*leftHand*/ false);
            // ⚠⚠ AND INTO THE HAND WHEN THAT IS WHERE THE PLAYER'S OWN WEAPON
            // WAS (user 2026-08-17: "if an actor has a weapon unsheathed we do
            // not have to show it sheathed we can just transmog it"). The note
            // above is still true - the attach parks on the sheath node and
            // knows nothing about draw state - so this is the move done
            // afterwards rather than a different attach.
            //
            // ⚠ GATED ON THE DISPLACED PIECE'S PLACEMENT, NOT ON ActorState.
            // displacedDrawn is the reading taken while the placement was still
            // the player's own, and it already folds the menu transition's
            // transient "sheathed" answer through PreserveDrawnWeaponPlacement.
            // It is also the honest question: it means their POSE is holding a
            // weapon, which is the pose the preview has to fit.
            //
            // ⚠ NOTHING TO UNDO ON THE WAY OUT. The teardown is
            // ClearWeaponPart on the biped SLOT, which does not care which node
            // the clone hung on, and the player's own weapon is restored with
            // its own drawn repair either way.
            if (displacedDrawn) {
                REAug::DrawStagedWeapon(a_actor, a_weapon, a_class);
            }
        }

        g_held.actor          = a_actor->GetHandle();
        g_held.weapon         = a_weapon;
        g_held.slot           = slot;
        g_held.active         = true;
        g_held.displaced      = displaced;
        g_held.displacedDrawn = displacedDrawn;
        g_held.displacedSlot  = displacedSlot;
        // ⚠ NOT THROTTLED, AND DELIBERATELY. A re-attach means a rebuild took
        // the last one, and how OFTEN that happens is the measurement that
        // would show this feature fighting the engine every frame. Throttling
        // it would hide exactly the symptom the field just reported.
        spdlog::debug("weapon preview: hung '{}' on slot {} ({}){}.",
                      a_weapon->GetName() ? a_weapon->GetName() : "?", slot,
                      displacedDrawn ? "drawn, into the hand" : "sheathed",
                      claimed ? " - RE-ATTACH, a rebuild had taken it" : "");
        g_said = LastSaid{};  // the next decline is news again
        return Decline::kNone;
    }

}  // namespace OS::WeaponPreview
