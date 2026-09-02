#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace OS {

    // ONE "deed" CLAUSE KIND, TWO PRODUCERS, AND THE NAME SAYS WHICH.
    //
    // Fitting Room counts its own deeds (channelsDyed, billed per look) and
    // Skyrim counts about a hundred misc stats reachable through
    // Game.QueryStat. Both are "a counter with a threshold", so they share the
    // kDeed clause rather than growing a second kind that would duplicate every
    // parse, display and strictness path for no difference the author cares
    // about.
    //
    // What they cannot share is the LOOKUP. A name handed to the wrong producer
    // comes back 0, and 0 is below every threshold worth writing, so the colour
    // locks for the life of the character. This prefix is what keeps them
    // apart, and it is a prefix rather than a lookup table so that a rules file
    // shipped by a third party can gate on a stat this project never listed.
    inline constexpr std::string_view kStatDeedPrefix = "stat:";

    // The Skyrim stat name inside a deed name, or nullopt when the deed is one
    // of Fitting Room's own counters.
    //
    // ⚠⚠ THE NAME COMES BACK VERBATIM AND MUST. Game.QueryStat answers int 0
    // for a name the engine does not know, which is indistinguishable from an
    // honest zero: no throw, no null, no log line, nothing that fails. Measured
    // 2026-08-08, 'Fitting Room Not A Real Stat' answered int 0 in the same
    // millisecond as two real reads. So there is no "nearly right" here, and
    // any tidying this function did - a trim, a case fold, a smart-quote
    // rewrite - would be a silent permanent lock on every character.
    //
    // ⚠ AND THE NAMES ARE NOT WHAT YOU WOULD TYPE. Read out of SkyrimSE.exe
    // into tools/data/stat_index.json:
    //
    //     "The Companions Quests Completed"        not "Companions ..."
    //     "Thieves' Guild Quests Completed"        not "Thieves Guild ..."
    //     "The Dark Brotherhood Quests Completed"  not "Dark Brotherhood ..."
    //
    // All three were typed the obvious way first. tools/check_dye_rules.py
    // compares the authored text against that index BYTE FOR BYTE, which is the
    // one place a wrong name is catchable at all, and it only works while this
    // function hands the engine exactly what the author wrote.
    //
    // ⚠ THE RETURN IS A VIEW INTO a_deed. It is valid only as long as the
    // argument is; copy it if it has to outlive the call.
    [[nodiscard]] constexpr std::optional<std::string_view> StatNameFromDeed(
        std::string_view a_deed) {
        if (!a_deed.starts_with(kStatDeedPrefix)) {
            return std::nullopt;
        }
        a_deed.remove_prefix(kStatDeedPrefix.size());
        // A bare "stat:" names no stat. Refusing it here keeps the bridge from
        // dispatching an empty string, which would answer 0 like everything
        // else the engine does not know and would look, in the log, exactly
        // like a real counter sitting at zero.
        if (a_deed.empty()) {
            return std::nullopt;
        }
        return a_deed;
    }

}  // namespace OS
