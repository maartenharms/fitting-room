#include "DyeConditions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace OS {

    namespace {
        // Skyrim's eighteen skills and their ActorValue numbers, read out of
        // CommonLibSSE's RE/A/ActorValues.h. The block is contiguous, 6 to 23.
        //
        // ⚠ Two of these are not called what you would guess: it is kArchery
        // and not Marksman, and kSpeech and not Speechcraft. The names here are
        // the ones a rules file writes, so they follow the game's UI where the
        // enum does not.
        constexpr std::array<std::pair<std::string_view, std::uint32_t>, 18>
            kSkills{ { { "OneHanded", 6 },
                       { "TwoHanded", 7 },
                       { "Archery", 8 },
                       { "Block", 9 },
                       { "Smithing", 10 },
                       { "HeavyArmor", 11 },
                       { "LightArmor", 12 },
                       { "Pickpocket", 13 },
                       { "Lockpicking", 14 },
                       { "Sneak", 15 },
                       { "Alchemy", 16 },
                       { "Speech", 17 },
                       { "Alteration", 18 },
                       { "Conjuration", 19 },
                       { "Destruction", 20 },
                       { "Illusion", 21 },
                       { "Restoration", 22 },
                       { "Enchanting", 23 } } };

        // A value from a map, or zero. An ungathered skill and an unknown deed
        // both fail closed: a rule naming something we never collected locks
        // its colour rather than freeing it.
        template <class Map, class Key>
        std::uint32_t ValueOr0(const Map& a_map, const Key& a_key) {
            const auto it = a_map.find(a_key);
            return it == a_map.end() ? 0u : it->second;
        }

        // Hex, with an OPTIONAL 0x prefix, because the header offers 0x2a12f as
        // an authoring example and because xEdit shows form ids that way. The
        // digit cap is applied AFTER the prefix so "0x0002A12F" fits.
        bool HexToU32(std::string_view a_text, std::uint32_t& a_out) {
            if (a_text.size() > 2 && a_text[0] == '0' &&
                (a_text[1] == 'x' || a_text[1] == 'X')) {
                a_text.remove_prefix(2);
            }
            if (a_text.empty() || a_text.size() > 8) {
                return false;
            }
            std::uint32_t v = 0;
            for (const char c : a_text) {
                v <<= 4;
                if (c >= '0' && c <= '9') {
                    v |= static_cast<std::uint32_t>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    v |= static_cast<std::uint32_t>(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    v |= static_cast<std::uint32_t>(c - 'A' + 10);
                } else {
                    return false;
                }
            }
            a_out = v;
            return true;
        }
    }  // namespace

    std::optional<std::uint32_t> SkillByName(std::string_view a_name) {
        const auto it = std::find_if(
            kSkills.begin(), kSkills.end(),
            [&](const auto& p) { return p.first == a_name; });
        if (it == kSkills.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string_view SkillNameByAV(std::uint32_t a_av) {
        // ⚠ THE SAME kSkills ARRAY SkillByName READS, which is the entire
        // reason this function is in this file rather than beside the pane
        // that needs it. The names are the ones a rules file writes, so the
        // two directions cannot drift into disagreeing about what 8 is called.
        const auto it = std::find_if(
            kSkills.begin(), kSkills.end(),
            [&](const auto& p) { return p.second == a_av; });
        if (it == kSkills.end()) {
            return {};
        }
        return it->first;
    }

    bool Satisfied(const DyeCondition& a_cond, const DyeWorldState& a_world) {
        switch (a_cond.kind) {
        case DyeCondKind::kAlways:
            return true;
        case DyeCondKind::kNever:
            return false;
        case DyeCondKind::kLevel:
            return a_world.level >= a_cond.min;
        case DyeCondKind::kSkill:
            return ValueOr0(a_world.skills, a_cond.skill) >= a_cond.min;
        case DyeCondKind::kQuest:
            return a_world.questsDone.contains(a_cond.quest);
        case DyeCondKind::kDeed:
            return ValueOr0(a_world.deeds, a_cond.deed) >= a_cond.min;
        case DyeCondKind::kLocationCleared:
            return a_world.locationsCleared.contains(a_cond.location);
        }
        // ⚠ Unreachable for a valid enum, and REQUIRED: the plugin target
        // compiles with /we4715, so a switch with no trailing return is a hard
        // error there. Fails closed on a corrupt value, which matches the rest
        // of the module. The exhaustiveness net is /w14062 in CMakePresets.json,
        // not this line; see the build notes at the top of the plan.
        return false;
    }

    bool AllSatisfied(const std::vector<DyeCondition>& a_conds,
                      const DyeWorldState&             a_world) {
        return std::all_of(a_conds.begin(), a_conds.end(),
                           [&](const DyeCondition& c) {
                               return Satisfied(c, a_world);
                           });
    }

    bool ConditionFromJson(const Json::Value& a_json, DyeCondition& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        const auto& type = a_json["type"];
        if (!type.isString()) {
            return false;
        }
        const auto   name = type.asString();
        DyeCondition c;

        if (name == "always") {
            c.kind = DyeCondKind::kAlways;
        } else if (name == "never") {
            // ⚠ Deliberately NOT accepted from a file, but not because it
            // withholds anything: a pack author can lock a dye anyway, since
            // ANY unparseable clause ends at the same kNever. "horoscope" does
            // it just as well.
            //
            // What the refusal buys is that every lock is COUNTED as a rejected
            // rule and logged, instead of one spelling being a silent way to
            // lock a colour; and that kNever keeps exactly one producer, so
            // "how did this become kNever" has one answer.
            return false;
        } else if (name == "level") {
            c.kind = DyeCondKind::kLevel;
            if (!a_json["min"].isUInt()) {
                return false;
            }
            c.min = a_json["min"].asUInt();
        } else if (name == "skill") {
            c.kind = DyeCondKind::kSkill;
            if (!a_json["skill"].isString() || !a_json["min"].isUInt()) {
                return false;
            }
            const auto av = SkillByName(a_json["skill"].asString());
            if (!av) {
                return false;
            }
            c.skill = *av;
            c.min   = a_json["min"].asUInt();
        } else if (name == "quest") {
            c.kind = DyeCondKind::kQuest;
            if (!a_json["plugin"].isString() || !a_json["formId"].isString()) {
                return false;
            }
            c.quest.plugin = a_json["plugin"].asString();
            if (c.quest.plugin.empty() ||
                !HexToU32(a_json["formId"].asString(), c.quest.formId)) {
                return false;
            }
        } else if (name == "locationCleared") {
            c.kind = DyeCondKind::kLocationCleared;
            if (!a_json["plugin"].isString() || !a_json["formId"].isString()) {
                return false;
            }
            c.location.plugin = a_json["plugin"].asString();
            if (c.location.plugin.empty() ||
                !HexToU32(a_json["formId"].asString(), c.location.formId)) {
                return false;
            }
        } else if (name == "deed") {
            c.kind = DyeCondKind::kDeed;
            if (!a_json["deed"].isString() || !a_json["min"].isUInt()) {
                return false;
            }
            c.deed = a_json["deed"].asString();
            if (c.deed.empty()) {
                return false;
            }
            c.min = a_json["min"].asUInt();
        } else {
            return false;
        }

        a_out = std::move(c);
        return true;
    }

    bool ConditionsFromJson(const Json::Value&         a_json,
                            std::vector<DyeCondition>& a_out) {
        a_out.clear();
        if (!a_json.isArray()) {
            return false;
        }
        std::vector<DyeCondition> parsed;
        for (const auto& entry : a_json) {
            DyeCondition c;
            if (!ConditionFromJson(entry, c)) {
                return false;  // all or nothing: see the header
            }
            parsed.push_back(std::move(c));
        }
        a_out = std::move(parsed);
        return true;
    }

}  // namespace OS
