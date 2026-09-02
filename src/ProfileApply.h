#pragma once

#include "ProfileCodec.h"
#include "ProfilePlan.h"

namespace RE {
    class Actor;
}

// The apply half of W1: run a profile's blocks onto the player in
// ProfilePlan's order. The ordering is the two-painter resolution made
// executable (spec section 6); this module only EXECUTES the plan, it never
// decides an order of its own.
//
// The face step is asynchronous the way capture's is: LoadCharacterEx is a
// Papyrus dispatch, so the remaining steps are queued behind its answer plus
// one task drain (the head build the 21:19 field cycle measured happens
// inside the dispatch window, and one drain is the same settling the
// reconcile trusts). Every step logs one "ProfileApply: step '<name>'" line,
// so a field report reads as a checklist against the pinned order.
namespace OS::ProfileApply {

    // Apply a_profile to a_player, taking only the blocks a_boxes leaves on
    // (full apply passes ProfilePlan::AllOn()). Refuses whole, with one log
    // line, when a top-level `requires` plugin is absent.
    void Apply(RE::Actor* a_player, const ProfileCodec::Profile& a_profile,
               const ProfilePlan::Participation& a_boxes);

    // Whether an Apply is between its first step and its settle refresh.
    // The editor-close path reads this: Apply closes the editor by design,
    // and the close's own look re-assert (DiscardStaging's push and refresh)
    // otherwise runs INSIDE the apply, against a half-switched character
    // (field 2026-08-22 03:56: it pushed the old outfit's parts one
    // millisecond after the character step, recaptured pre-switch state the
    // step had just cleared, and computed fit and skin against Nord-race,
    // female-sex). The apply's own push and settle refresh carry the final
    // look, so the close pass stands down while this is true. Self-heals:
    // reads false again 30 s after the apply began no matter what.
    [[nodiscard]] bool InFlight();

    // Whether a switched apply's head is still settling: true from the head
    // reconcile until its thirty-second watch window closes. HeadBuildHook
    // publishes the face node to FSMP after each player head build while
    // this reads true, because the engine's own rebuilds inside the window
    // (the race-switch reload above all) leave an SMP wig swallowed by
    // FSMP's SkinAll hook with no binding: attached, visible, and rendering
    // as nothing (round twelve's bald head).
    [[nodiscard]] bool SwitchSettling();

    // ⚠⚠ THE MAPPED PRESET IS SESSION RESIDUE AND MUST NOT FOLLOW A SAVE.
    // skee's LoadCharacterEx keys the loaded preset to the player's TESNPC -
    // the SAME form in every save - and that map is in neither its revert nor
    // its save list (skee64 main.cpp 328/368, read 2026-08-24), so it
    // outlives every load. The in-load erase that used to live here was
    // REMOVED after r63-r70 proved any mid-load ClearPreset, at any timing,
    // manufactures the load-double (the +5-7 s rebuild applies the cosave
    // sculpt twice; see the scar at the bottom of ProfileApply.cpp). The
    // apply-time erase now chains inside the face step instead.

    // Called at kPostLoadGame/kNewGame: every load re-mints skee's string
    // table, and the face step keys its epoch-align double pass on whether a
    // face has applied since the last boundary (r75; the r74 capture round
    // proved skee's sculpt maps go lookup-blind across the epoch).
    void NoteLoadBoundary();

    // Called at kPreLoadGame, and ONLY from there. r90 caught skee painting the
    // outgoing character's hair colour onto the incoming one during the load
    // screen, out of the mapped preset, 42 s before kPostLoadGame - so every
    // erase r63-r70 tried was downstream of the paint it was meant to prevent.
    // This is the one window upstream of it.
    //
    // ⚠ IT IS A DISPATCH, NOT A CALL, AND r91 WON THAT RACE ONCE IN FOUR.
    // The VM this needs is the one the load resets, so a single ask is a coin
    // flip. It now retries on a 150 ms slice behind two rails:
    //
    //   * ONE outstanding dispatch at a time. A queued stack cannot be
    //     recalled, so a burst aimed at a stalled VM would all run when it
    //     wakes, and anything running after the paint is r63-r70's doubling
    //     window arriving by the back door.
    //   * NotePlayerHairBuilt() disarms it. Past the paint an erase cannot
    //     save the hair and can still double the face, so the debt is written
    //     off rather than carried.
    //
    // Gated on [Compat] bPreLoadPresetErase; off is the control arm and needs
    // no rebuild.
    void ErasePresetAtPreLoad();

    // Called from HeadBuildHook the moment the player's own hair part is
    // built. This is the hard rail on the retry above, and it must fire on
    // EVERY player hair build rather than only the logged ones.
    void NotePlayerHairBuilt();

}  // namespace OS::ProfileApply
