#pragma once

#include <functional>
#include <set>
#include <string>

namespace OS::DyeTick {

    // Re-check the unlock rules while the player is playing, so a colour earned
    // by what they just did arrives while they are still standing there.
    //
    // ⚠⚠ WITHOUT THIS THERE IS NO SUCH MOMENT, and that is the finding this
    // module exists for. Until 2026-08-14 promotion ran at exactly two call
    // sites, kPostLoadGame and the editor's own refresh, so a colour was never
    // earned "during play": finishing a questline granted nothing, and the
    // colour appeared on the next loading screen. An unlock card wired to those
    // two sites would fire over a load screen or inside the editor it is
    // suppressed in, which is not a loot notification by any reading.
    //
    // ⚠ ONE MORE DETACHED TIMER THREAD, SHAPED LIKE WorldWatch's HEARTBEAT
    // rather than invented here: a 100 ms slice, an interval check, and every
    // piece of real work marshalled onto the game thread through SKSE's task
    // interface. That pattern is field proven in this repo and it is the reason
    // this does not ride DyeTexture's Present hook, which is installed lazily
    // from the first dye and so would never run for a character who has not
    // dyed anything yet.
    //
    // ⚠ NOT WorldWatch's OWN HEARTBEAT EITHER. That one runs every 2 seconds
    // and belongs to the rules engine; a promotion pass is fifteen times more
    // expensive and wanted fifteen times less often, and hanging one off the
    // other would tie two features together at their timers.

    // The pass to run on each tick. plugin.cpp installs it, because promotion
    // and the stats request both live beside the messaging handler.
    //
    // ⚠ It is called ON THE GAME THREAD, so it may do anything the load-time
    // pass may do.
    void SetPass(std::function<void()> a_pass);

    // A save is live and the tick may run. kPostLoadGame and kNewGame.
    //
    // ⚠ IT MUST NOT TICK AT THE MAIN MENU. Promotion reads the player and the
    // quest forms, and the main menu's idea of who the player is belongs to
    // whatever save the camera happens to be showing.
    void Start();

    // Stop ticking and forget where we were. kPreLoadGame, and the setting
    // going to 0.
    void Stop();

    // Ask for a pass SOON rather than at the next tick (2026-09-04). The engine
    // just did something a rule can gate on (a quest stage, a tracked stat the
    // rules name, a level, a skill), or the editor bumped the channels deed.
    // Coalesced: one pass about a second and a half after the first poke of a
    // burst, never within five seconds of the last pass, never while the
    // editor is open (it waits), and never before the tick has taken its
    // baseline (dropped; the first ticks own that window). Any thread.
    void Poke(const char* a_why);

    // The engine events that poke, registered once at kDataLoaded.
    void InstallEventSinks();

    // Which tracked stats are worth a poke: the ones the rules name. Set
    // beside the stats request, so it follows a rules reload; an empty filter
    // pokes on none of them, and kills or steps must never run the gather
    // every five seconds of a fight for a rule nobody wrote.
    void SetStatFilter(std::set<std::string, std::less<>> a_names);

    // Re-read the interval from the INI. The thread is started on the first
    // Start with a non-zero interval and lives for the process; a zero interval
    // parks it rather than tearing it down, so toggling the setting back on
    // does not have to build a thread inside a settings handler.
    void ApplySettings();

}  // namespace OS::DyeTick
