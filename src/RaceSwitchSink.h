#pragma once

// Race-switch suspension sink (spec §6). RE::TESSwitchRaceCompleteEvent fires
// AFTER a SwitchRace completes (vampire lord, werewolf, any scripted
// SwitchRace call) - at that point the actor's CURRENT race either still
// differs from its ActorBase's authored race (an alt form is active) or has
// returned to match it (back on the normal body). This sink translates that
// into OutfitSession's suspension state so the render override never tries to
// skin armor onto a creature form, and reapplies automatically on return.
//
// Also closes an audit-discovered gap: OutfitSession::Suspend()/Resume() (the
// GLOBAL player path, unrelated to the per-actor NPC suspension set) had ZERO
// callers before this - nothing ever stood the player's own transmog down on
// a player race switch either. This sink wires both the player and the
// NPC/follower path through the same event.
namespace OS::RaceSwitchSink {

    // Register the BSTEventSink against RE::ScriptEventSourceHolder. Call
    // once at kDataLoaded.
    void Register();

}  // namespace OS::RaceSwitchSink
