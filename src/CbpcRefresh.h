#pragma once

// r50 CONVICTION, CBPC's own log: the switched player runs 'Prisoner -
// Female' yet updates GenitalsLag and weapon MOV bones - the MALE bone kit.
// CBPC re-reads the sex per update, but the per-actor Thing list (which
// bones get simulated) is built ONCE, at first sight, and MalePhysics=0
// excluded every female bone while the base was still the male Nord. A sex
// flip alone never rebuilds the list; a save load does, which is why the
// Umbrael save always jiggles and the switched character never does, and
// why neither Auto Physics Reset's forceful 3D reset nor a re-equip cures
// anything.
//
// CBPC registers its own Papyrus repair for exactly this:
// CBPCPluginScript.RefreshActorBounceSettings / RefreshActorCollisionSettings
// (seen in cbp.dll beside "Refresh actor bounce for %x"). Calling them on
// the player after a race-or-sex apply settles makes CBPC rebuild the list
// from the sex the apply set. A rig without CBPC, or with an older CBPC,
// refuses the dispatch and the refusal is logged and harmless.

namespace OS::CbpcRefresh {

    // Queue the repair ladder ~2.5 s out (past the apply's head-build storm),
    // on the game thread, fire-and-forget: the Refresh pair, then ONE SKSE
    // NiNodeUpdate event for the player - the per-actor rescan CBPC sinks,
    // the same signal a RaceMenu close sends. The old
    // StopPhysics/StartPhysics stage is gone: CBPC's own psc declares the
    // pair as (Actor, String) and the argument-less dispatch never ran, so
    // the Refresh pair was the real rebuilder all along. Call at the settle
    // of an apply whose character step participated; NEVER from an equip
    // transition (the r57 freeze).
    void QueueAfterSwitch();

}  // namespace OS::CbpcRefresh
