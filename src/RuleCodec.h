#pragma once

// The one JSON shape a rule has, everywhere it appears: the objects in
// rules.json's "rules" array, the 'RULE' co-save payload, and author rule
// packs all read and write THIS codec. Pure logic, jsoncpp only, so it stays
// unit-testable - the same arrangement JsonCodec.h has for outfits.
//
//   { "id": "r-7f3a", "name": "Towns", "enabled": true, "priority": 40,
//     "base": {"outfit": "Town Clothes"},
//     "overlay": {"30": {"hide": true},
//                 "37": {"style": {"mod": "X.esp", "id": "0x812"}}},
//     "conditions": [ {"kind": "location", "negate": false,
//                      "keyword": {"mod": "Skyrim.esm", "id": "0x13168"}} ] }

#include "RuleModel.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace OS::RuleCodec {

    inline constexpr int kRulesVersion = 1;

    [[nodiscard]] Json::Value RuleToJson(const Rules::Rule& a_rule);

    // Fills a_out from a rule object. Unknown condition kinds, malformed
    // slots, malformed forms, and anything else unrecognised are skipped
    // rather than failing the file: a rules.json written by a newer build
    // must degrade, not vanish. Never throws - every jsoncpp accessor here is
    // guarded against the type it is about to read, because this build has
    // JSON_USE_EXCEPTION=1 and a hand-edited file is untrusted input.
    // Returns false only when a_json is not an object or carries no id.
    //
    // a_warnings, when non-null, gets one human-readable line APPENDED (not
    // cleared first) per skipped rule/clause, naming the rule id and what
    // was rejected. Most call sites can leave it null; Task 9 wires this into
    // the plugin log so a rule-pack author sees why a clause silently
    // stopped applying instead of guessing.
    bool JsonToRule(const Json::Value& a_json, Rules::Rule& a_out,
                     std::vector<std::string>* a_warnings = nullptr);

    // The whole file: { "version": 1, "rules": [ ... ] }.
    [[nodiscard]] Json::Value RulesToJson(const Rules::RuleSet& a_rules);

    // Reads a rules document. A "version" newer than kRulesVersion, or one
    // present but not a plain integer, loads NOTHING and reports it via
    // a_error - so a downgrade, or a hand-typo'd version field, never
    // silently truncates the user's rules on the next save. A missing
    // "version" field is treated as version 0 (predates the field). A
    // duplicate id drops the later rule but does not fail the document.
    // a_warnings, when non-null, collects the same per-clause lines
    // JsonToRule does, plus one line per dropped duplicate id and per
    // unusable rules[] entry.
    bool JsonToRules(const Json::Value& a_root, Rules::RuleSet& a_out, std::string& a_error,
                       std::vector<std::string>* a_warnings = nullptr);

}  // namespace OS::RuleCodec
