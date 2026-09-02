#pragma once

// Advanced clause text to a real RE::TESCondition. The syntax is the one DAV
// and OAR users already write, deliberately: someone who can author an OAR
// condition can author ours without learning a second grammar - narrowed to
// parameterless conditions only (AdvancedCondition.cpp's file banner has the
// reason: a resolvable form of the wrong FormType is silent type confusion,
// not a refusal, and this project could not confidently build the
// per-function type table that would be needed to accept parameters safely).
//
//   FunctionName <op> <value>
//   IsSneaking == 1
//   IsInCombat == 0
//
// This is the ONLY place in the rules feature that touches engine condition
// machinery. The result of every clause is folded into WorldSnapshot as a
// plain bool, which is what keeps RuleEngine pure.

#include "RuleModel.h"

#include <RE/Skyrim.h>

#include <memory>
#include <string>

namespace OS::AdvancedCondition {

    // Parse one clause. On failure returns null and puts a one-line
    // user-readable reason in a_error (it is shown in the Rules tab, so it
    // must read as an error message, not a parser dump).
    [[nodiscard]] std::shared_ptr<RE::TESCondition> Parse(const std::string& a_text,
                                                          std::string&       a_error);

    // Materialize and evaluate every Advanced clause in the rule set against
    // the player, writing results into a_snapshot.advanced. Rules whose
    // clauses will not parse are marked invalid with a reason; a rule whose
    // clause(s) now all parse has that invalidity cleared, so a fixed typo
    // does not stay stuck invalid until a save reload. Main thread only:
    // this calls into the engine.
    //
    // Parsed conditions are cached per (rule id, clause index, text); a rule
    // edit changes the text and so misses the cache naturally.
    void Resolve(Rules::RuleSet& a_rules, Rules::WorldSnapshot& a_snapshot);

    // Drop the cache. Called when the rule set is reloaded wholesale.
    void ClearCache();

}  // namespace OS::AdvancedCondition
