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

    // How many race switches the PLAYER has completed this session.
    //
    // ⚠ A COUNTER RATHER THAN A FLAG, AND THAT IS THE WHOLE USE. HeadEditorSink
    // reads it at both edges of a character-editor visit and compares, because
    // the thing it has to detect is a switch that landed BETWEEN them - and a
    // switch out and back leaves race and sex looking perfectly steady from
    // inside the close handler. A deletion is authorised only when nothing
    // moved, so an event that can be missed is one a purchase gets destroyed by.
    //
    // Session-scoped, monotonic, and it counts every completed switch of the
    // player rather than only the beast forms this sink suspends on: the
    // question is "did their identity move", not "did we stand down".
    [[nodiscard]] std::uint32_t PlayerSwitchCount();

}  // namespace OS::RaceSwitchSink
