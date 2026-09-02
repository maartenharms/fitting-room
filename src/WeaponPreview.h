#pragma once

// ⚠ NO PCH.h AND NO CommonLib INCLUDE, for WeaponSlots.h's own reason: the
// policy below has to be includable standalone by the pure-logic test suite,
// and this file's whole value is that the DECISION is testable even though
// neither the editor nor the engine-facing half can be compiled by a test. The
// two engine types are forward declared because they are only ever pointers
// here; the definitions belong to WeaponPreview.cpp.
#include "WeaponSlots.h"

#include <cstdint>

namespace RE {
    class Actor;
    class TESForm;
}

// Show a weapon the character does NOT carry, sheathed on their body, for as
// long as the editor is looking at it.
//
// THE REPORT (field 2026-08-11): styling a weapon class with nothing equipped
// shows nothing, and the row says "[not equipped]". That tag is honest - a
// style rebuilds onto geometry the engine attached, and the engine attached
// none - but "honest" is not the same as "useful", and the user asked to see
// the piece on their character rather than only on a card.
//
// ⚠⚠ SHEATHED, AND THAT IS NOT A COMPROMISE - IT IS THE WHOLE REASON THIS IS
// CHEAP. Actor::AttachWeapon NEVER reads weapon state to decide placement: the
// node resolver reads it only to set kHidden, and only on its left-hand and
// staff branches. The hip-to-hand move is a SEPARATE engine step that runs on
// a draw/sheathe transition, and nothing on the attach path reaches it. So a
// forced attach parks the weapon on its sheath node all by itself, with no
// animation graph involved at all. Drawing it would mean virtual 0xB4 and the
// idle-selection problem that has survived fifteen-plus rounds elsewhere
// ([[weapon-idle-selection-is-invisible]]: the engine never sheathes on a
// swap, so any path that does produces a visibly wrong idle). This module
// deliberately stops at the sheath.
//
// ⚠ IT IS 3D ONLY. Nothing here touches inventory, equip state, the co-save or
// any counter. The editor already promises the player in as many words that
// Fitting Room does not add preview items to their inventory, and this keeps
// that promise: the actor does not own the weapon, is not carrying it, and
// gains nothing from it. It is a picture, hung on a bone.
namespace OS::WeaponPreview {

    // Why a preview did not happen, so the caller can SAY so. An absent log
    // line is a reading, and the two "it does not work" reports of the last
    // stint were both a silent path.
    enum class Decline : std::uint8_t {
        kNone = 0,       // go ahead
        kNoActor,        // no actor, or no biped built yet
        kNoStyle,        // nothing picked to show
        kBadClass,       // the class has no attachable biped slot
        kIsAmmo,         // the quiver: a different engine entry point entirely
        kSlotOccupied,   // the actor REALLY carries something there
        kAlreadyShowing  // this exact weapon is already up; nothing to do
    };

    [[nodiscard]] constexpr const char* Why(Decline a_d) {
        switch (a_d) {
            case Decline::kNone: return "ok";
            case Decline::kNoActor: return "no actor or no biped";
            case Decline::kNoStyle: return "nothing selected to preview";
            case Decline::kBadClass: return "class has no attachable slot";
            case Decline::kIsAmmo: return "ammo attaches through its own path";
            case Decline::kSlotOccupied: return "the actor really carries one";
            case Decline::kAlreadyShowing: return "already showing this one";
        }
        return "?";
    }

    // Whether to hang a preview weapon on the character, given what the caller
    // measured. Pure so it can be tested: nothing compiles the editor or the
    // engine-facing half of this module.
    //
    // ⚠ GUARD 1, AND IT IS THE ONE THAT MATTERS: THE SLOT MUST BE EMPTY. If
    // the actor really carries something in this class we do not touch it, and
    // that single rule buys three separate things at once:
    //
    //   * We never fight a real weapon, so the player's own gear cannot be
    //     detached, replaced, or left behind by us.
    //   * We never trip AttachWeaponPart's change-detect. It returns before the
    //     part loader when `weapon == objects[slot].item && partClone != null`,
    //     which cost three stints to find once already
    //     ([[skyrim-weapon-attach-change-detect]]). On an EMPTY slot both
    //     halves are false, so the loader runs and our styling hook sees the
    //     part - no teardown dance needed on the way in.
    //   * Teardown stays trivially correct: the slot was empty when we found
    //     it, so "put it back" means "make it empty again" rather than
    //     restoring a state we would have had to remember.
    //
    // ⚠ AND IT IS WHY THIS CANNOT REPLACE THE "[not equipped]" TAG. A player
    // who really wears a sword still sees their own sword; the preview is for
    // the empty hand only. The tag stays true whenever it is shown.
    //
    //   a_haveActor    an actor with a built biped
    //   a_haveStyle    something is picked to show
    //   a_slot         BipedSlotForClass of the class in question
    //   a_slotItem     the slot holds something that is NOT our own preview
    //   a_stillUp      our preview is on the biped RIGHT NOW - see below
    //
    // ⚠⚠ a_stillUp MUST BE MEASURED OFF THE BIPED, NEVER OFF A RECORD OF WHAT
    // THE CALLER ATTACHED. This is the parameter that broke in the field
    // (2026-08-11 evening) and the bug is worth stating in full, because the
    // wrong version looks completely reasonable:
    //
    // The first cut answered it from the caller's own bookkeeping - "I called
    // attach for this form on this slot, so it must be showing". But every
    // staging update rebuilds the biped and restages objects[], and the engine
    // takes our node with it. The record still said showing, so the next pass
    // returned kAlreadyShowing and NEVER RE-ATTACHED. From the player's side:
    // click a weapon, see nothing, click to another class and back, and there
    // it is - because that is what finally made the record disagree.
    //
    // An API you write to is not a source of truth. The caller must ask
    // objects[slot] whether the item is still ours AND the clone still exists.
    [[nodiscard]] constexpr Decline ShouldShow(bool a_haveActor, bool a_haveStyle,
                                               std::uint32_t a_slot, bool a_slotItem,
                                               bool a_stillUp) {
        if (!a_haveActor) {
            return Decline::kNoActor;
        }
        if (!a_haveStyle) {
            return Decline::kNoStyle;
        }
        // ⚠ THE QUIVER IS ALLOWED, AND IT REACHES A DIFFERENT ENGINE FUNCTION.
        // Actor::AttachWeapon rejects AMMO on its FIRST instruction - it opens
        // `cmp byte ptr [rdx+0x1a], 0x29`, kWeapon, and kAmmo is 0x2A - so a
        // quiver can never ride the weapon path and the caller must route it
        // to AttachAmmoPart instead. That is a separate entry point, not a
        // wider range, which is why this file still names the slot explicitly
        // rather than widening IsUnambiguousVisualWeaponSlot to swallow it.
        //
        // (This returned kIsAmmo until 2026-08-11 evening, when the field
        // asked for quivers and bolts. The decline was never a limit of the
        // engine, only of what had been wired.)
        //
        // ⚠ IsUnambiguousVisualWeaponSlot, NOT IsMainHandWeaponBipedSlot. The
        // latter answers "is this slot a hand signal" and accepts 32, which is
        // the BODY - it is only in range because the range starts there, and
        // nothing stages a weapon in it. This one is exactly 33..40, the slots
        // AttachWeapon actually stages a main-hand weapon into. It also keeps
        // slot 0 out, and slot 0 is a head armour slot: attaching there would
        // hang a sword off the character's face.
        if (a_slot != 41u && !IsUnambiguousVisualWeaponSlot(a_slot)) {
            return Decline::kBadClass;
        }
        if (a_slotItem) {
            return Decline::kSlotOccupied;
        }
        if (a_stillUp) {
            return Decline::kAlreadyShowing;
        }
        return Decline::kNone;
    }

    // May the piece already in this slot be TEMPORARILY DISPLACED so the
    // browsed weapon can be shown in its place, and put back afterwards?
    //
    // ⚠⚠ THIS IS THE ONLY PLACE THIS MODULE IS ALLOWED TO TOUCH GEAR THE
    // PLAYER ACTUALLY OWNS, so it is deliberately the narrowest question that
    // can be asked. Asked and answered 2026-08-11 evening, when the field
    // asked for the preview to replace an equipped weapon while browsing -
    // before that, an occupied slot was refused outright.
    //
    // ⚠ IT MUST BE THE ACTOR'S OWN EQUIPPED MAIN-HAND WEAPON, compared by
    // form against GetEquippedObject(false), and nothing weaker. "The slot
    // holds a weapon" is NOT enough: a follower's spare is drawn from
    // inventory by Immersive Equipment Displays onto the skeleton rather than
    // through BipedAnim ([[follower-spare-weapons-drawn-by-ied]]), and engine
    // visual-only weapons exist that the actor does not carry at all. Taking
    // either of those down leaves us holding something we cannot put back,
    // because "put it back" means re-attaching a form the actor still has
    // EQUIPPED - which is the one thing that makes the restore trivially
    // correct.
    //
    // ⚠ AND THE OFF HAND IS UNREACHABLE HERE BY CONSTRUCTION, which is why
    // there is no hand parameter. AttachWeapon stages the main hand in the
    // class slot (33..40) and the OFF hand in the race's shield slot, below
    // 32, so a class slot can only ever hold the main hand. That is also what
    // makes the restore able to use virtual 0xB4, whose child selection is
    // ambiguous for the left when both hands share one WEAP form.
    // ⚠⚠ AND a_slot IS WHERE THE REAL PIECE IS STAGED, NOT WHERE THE PREVIEW
    // IS GOING. Those are different slots whenever the browsed class differs
    // from the equipped one, and conflating them is the field bug of
    // 2026-08-11 evening: with a sword equipped, browsing GREATSWORDS attached
    // the preview to slot 37, left the sword sitting in slot 33, and the
    // character carried both. "Hide the weapon in the player's hand" is not
    // "hide whatever is in the slot I am about to use".
    [[nodiscard]] constexpr bool MayDisplaceWorn(bool a_isActorsEquippedPiece,
                                                 bool          a_isWeaponOrAmmo,
                                                 std::uint32_t a_slot) {
        return a_isActorsEquippedPiece && a_isWeaponOrAmmo &&
               (a_slot == 41u || IsUnambiguousVisualWeaponSlot(a_slot));
    }

    // ---- The engine-facing half (WeaponPreview.cpp) ----------------------
    //
    // ⚠ ONE PREVIEW AT A TIME, PROCESS WIDE, AND THAT IS DELIBERATE. Two would
    // mean two teardowns to get right and a way for one to leak while the
    // other is replaced. The editor looks at one thing at a time anyway.

    // Put a_weapon (a TESObjectWEAP, as a TESForm*) on a_actor's sheath node,
    // taking down whatever we had up before. Safe to call every frame with the
    // same arguments: it declines with kAlreadyShowing and does nothing.
    // Returns why it declined, or kNone if the weapon is now up.
    Decline Show(RE::Actor* a_actor, RE::TESForm* a_weapon, WeaponClass a_class);

    // Take down whatever is up, if anything. Idempotent, and safe to call when
    // nothing is showing or when the actor is long gone.
    //
    // ⚠ CALL IT FROM EVERY ROOT THAT CAN END A PREVIEW, not only the tidy one.
    // The editor closing is the obvious path; the ones that bite are the
    // others ([[teardown-must-check-every-root]]).
    void Clear();

    // Whether anything is up. For the caller's own logging and for the editor
    // to know it must not also draw the "[not equipped]" tag.
    [[nodiscard]] bool Showing();

    // ---- the shield's own preview flag ------------------------------------
    //
    // The shield is NOT handled by this module's attach path - it is an ARMO on
    // biped object 9, staged by the armour pass - so all it needs from here is
    // the same lifetime the weapons get. This is that lifetime, and nothing
    // else.
    //
    // ⚠⚠ THE ROW, NOT THE EDITOR. The first cut gated a conjured shield on
    // EditorWindow::IsOpen(), which is far too coarse: browse shields, move to
    // Boots, and the shield stayed on the arm for the rest of the session
    // (field 2026-08-11 evening). A preview belongs to the thing being
    // previewed, so it ends when the player looks somewhere else - the same
    // rule the weapon preview above already follows by keying on the selection.
    //
    // Set every frame by the editor while the shield row is the selection, and
    // cleared on close. Read by the render pass through ShieldStyleMayConjure.
    void SetShieldRowActive(bool a_active);
    [[nodiscard]] bool ShieldRowActive();

}  // namespace OS::WeaponPreview
