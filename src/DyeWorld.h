#pragma once

#include "DyeConditions.h"
#include "DyeRules.h"
#include "DyeUnlocks.h"

#include <map>
#include <string>

namespace OS::DyeWorld {

    // Gather everything the rules can ask about, once.
    //
    // ⚠ MAIN THREAD ONLY. It reads the player, the actor values and the quest
    // forms. The render thread must never call it.
    //
    // ⚠ The promotion pass that calls it runs at kPostLoadGame / kNewGame, NOT
    // at kDataLoaded. Promotion has to run after the co-save record has been
    // decoded, or it evaluates against an empty unlock set and against whatever
    // character the main menu happened to be showing. Both messages are main
    // thread, so the rule above still holds either way; the trigger matters for
    // correctness, not for threading.
    //
    // ⚠ THE DEEDS ARE A PARAMETER BECAUSE FETCHING THEM DEADLOCKED. This used
    // to read them itself through DyeUnlocks::Snapshot, which takes the unlock
    // lock. That lock is a plain non-reentrant mutex, so calling Gather from
    // inside DyeUnlocks::With hung the game with no crash and no log line, and
    // the only defence was a warning in this comment telling the caller not to.
    // The edit that would have caused it, making the whole promotion pass
    // atomic by moving the gather inside the With, reads like an improvement.
    //
    // Passing the set in turns that fatal call into the correct one: a caller
    // already inside a With hands over the set it is holding. Gather now takes
    // no lock at all.
    //
    // ⚠ So do not "simplify" this back to fetching the deeds internally. The
    // parameter is the fix, and removing it restores a hang that costs the
    // player their session with nothing written down anywhere to explain it.
    //
    // Only the quests a_rules actually mentions are looked up, so a rules file
    // naming three quests costs three form lookups rather than a walk over every
    // quest in the load order.
    [[nodiscard]] DyeWorldState Gather(const DyeRuleSet&   a_rules,
                                       const DyeUnlockSet& a_unlocks);

    // The names of every quest and location the rules mention, so a locked
    // swatch can say WHICH one.
    //
    // ⚠ THE TOOLTIP USED TO NAME THE PLUGIN AND NOTHING ELSE. "Finish a quest
    // in Skyrim.esm" is true and useless, and it was that way because
    // DyeRequirements is a pure module with no engine to resolve a form with.
    // Resolving here rather than there keeps it pure: this is the same
    // main-thread pass that already looks every one of these forms up to ask
    // whether it is done, so the name costs a `GetFullName` beside a call that
    // was happening anyway.
    //
    // ⚠ A REF THAT DOES NOT RESOLVE IS ABSENT FROM THE MAP, NOT PRESENT AND
    // EMPTY. The plugin may not be installed, and the honest line then is the
    // old one that names the plugin: telling a player to finish a quest by a
    // name that does not exist on their machine is worse than telling them
    // less. An unnamed quest, which Skyrim has plenty of, is the same case.
    //
    // ⚠ MAIN THREAD ONLY, on Gather's terms and for Gather's reason.
    [[nodiscard]] std::map<QuestRef, std::string> NameRefs(const DyeRuleSet& a_rules);

}  // namespace OS::DyeWorld
