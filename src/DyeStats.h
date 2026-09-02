#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>

namespace OS::DyeStats {

    // Skyrim's own misc stat counters, fetched for the unlock rules that name
    // them.
    //
    // ⚠⚠ THERE IS NO NATIVE AND THIS IS NOT A SYNCHRONOUS READ. CommonLibSSE-NG
    // has no MiscStatManager, and the conclusion drawn from that once in this
    // project - "so the stats are not reachable" - was wrong and cost a whole
    // design. The route is the Papyrus VM: Game.QueryStat is a vanilla global
    // native, so DispatchStaticCall reaches it with no new dependency on
    // anything.
    //
    // ⚠⚠ THE ANSWER ARRIVES ~180 ms LATER, ON THE VM'S OWN THREAD. Measured
    // 2026-08-08: three dispatches accepted immediately, all three answered in
    // the same millisecond about 180 ms afterwards. So a promotion pass at
    // kPostLoadGame CANNOT read a stat inline, and this module is shaped around
    // that: Request returns at once, values land later, and the caller is told
    // when they have.
    //
    // ⚠⚠ AN UNKNOWN STAT NAME ANSWERS int 0, WHICH IS AN HONEST ZERO'S ANSWER.
    // Nothing throws, nothing logs, nothing distinguishes them. That is why a
    // name is never typed into a rules file from memory and why
    // tools/check_dye_rules.py compares every one against
    // tools/data/stat_index.json, read out of SkyrimSE.exe. Nothing at runtime
    // can catch it.
    //
    // WHY THAT IS SURVIVABLE AT ALL. A missing value reads 0, 0 is below every
    // threshold worth writing, and unlocks are add-only, so an absent stat can
    // only ever WITHHOLD a colour. Withheld is one load away from fixed;
    // granted is permanent. Every ambiguity in this module falls that way on
    // purpose.

    // Ask the engine for every stat named, and return immediately.
    //
    // a_onSettled runs once the last outstanding answer has landed, ON THE MAIN
    // THREAD via SKSE's task interface, so the caller may do anything a game
    // thread may do. It does NOT run when nothing could be dispatched.
    //
    // ⚠ THE CALLER'S JOB IS TO BE IDEMPOTENT, NOT TO WAIT. The intended shape is
    // to run the promotion pass immediately with the stats absent and run the
    // same pass again from a_onSettled. Promotion is add-only, so the second
    // pass can only ever grant what the first could not see, and a dispatch that
    // never answers costs one load rather than a hang.
    //
    // ⚠ A SECOND Request SUPERSEDES THE FIRST, and this is not a nicety. Stats
    // are per CHARACTER. Loading save B while save A's answers are still in
    // flight would otherwise land A's counters in B's map, and a counter that
    // is too high grants a colour permanently - the one direction nothing takes
    // back. Each call bumps a generation and answers from an older one are
    // discarded, unread.
    void Request(const std::set<std::string, std::less<>>& a_names,
                 std::function<void()>                     a_onSettled);

    // Everything that has arrived for the CURRENT generation. A name that was
    // never asked for, has not answered yet, or is not a stat the engine knows
    // is simply absent, and every consumer reads absent as 0.
    //
    // ⚠ Callable from the main thread while answers are still landing; the map
    // is guarded. The value it returns is a copy, so it cannot be torn by a
    // callback arriving mid-read.
    [[nodiscard]] std::unordered_map<std::string, std::uint32_t> Values();

    // Drop everything and invalidate any answer still in flight.
    //
    // ⚠ CALLED ON THE WAY INTO A LOAD, not on the way out of one. The map holds
    // the OUTGOING character's counters until it is cleared, and promotion that
    // read them would grant the incoming character colours off somebody else's
    // playthrough. Request clears as part of superseding, so this exists for the
    // paths that do not re-request at all.
    void Forget();

}  // namespace OS::DyeStats
