#include "RuleCodec.h"

#include <charconv>
#include <cstdio>  // snprintf, same reason JsonCodec.cpp includes it
#include <optional>
#include <set>

namespace OS::RuleCodec {

    using namespace OS::Rules;

    namespace {
        // Frozen public vocabulary for rules.json / rule packs - append only.
        // No default: an appended ConditionKind that misses a case here is a
        // compiler warning (C4062/-Wswitch), and falls through to "" rather
        // than to some OTHER real kind's name - so a forgotten case degrades
        // to "unrecognised, clause skipped" on the next load instead of
        // silently becoming a different, valid condition. Same discipline as
        // WeaponSlots.h's ClassJsonName.
        constexpr const char* KindName(ConditionKind a_k) {
            switch (a_k) {
            case ConditionKind::kLocation:  return "location";
            case ConditionKind::kCell:      return "cell";
            case ConditionKind::kInterior:  return "interior";
            case ConditionKind::kWeather:   return "weather";
            case ConditionKind::kTimeOfDay: return "timeOfDay";
            case ConditionKind::kCombat:    return "combat";
            case ConditionKind::kSneaking:  return "sneaking";
            case ConditionKind::kSwimming:  return "swimming";
            case ConditionKind::kMounted:   return "mounted";
            case ConditionKind::kWornSlot:  return "wornSlot";
            case ConditionKind::kAdvanced:  return "advanced";
            case ConditionKind::kDialogue:  return "dialogue";
            case ConditionKind::kRegion:    return "region";
            case ConditionKind::kVampire:   return "vampire";
            case ConditionKind::kCasting:   return "casting";
            }
            return "";  // a case missed above, or an out-of-range enum value
        }

        // Case-sensitive reverse lookup of KindName, DERIVED from it by loop
        // rather than hand-duplicated in a parallel if-chain: one table, not
        // two, so the encode and decode directions cannot silently drift
        // apart. Same shape as WeaponSlots.h's ClassFromJsonName.
        //
        // The empty-string guard matters even though no case above currently
        // returns "": it is what keeps a FUTURE forgotten case (which then
        // encodes as "kind": "") from matching anything on decode, rather
        // than silently resolving to whichever kind's name happens to be
        // empty.
        std::optional<ConditionKind> KindFromName(const std::string& a_s) {
            if (a_s.empty()) {
                return std::nullopt;
            }
            // ⚠⚠ THE BOUND USED TO BE THE LAST ENUMERATOR BY HAND, AND THAT WAS
            // A TRAP WITH A SILENT FAILURE. It read kAdvanced while that was
            // last; kDialogue, kRegion and kVampire were appended after it, and
            // each time the bound had to be moved by whoever noticed. Forgetting
            // makes every clause using the new kind decode as "unknown" and get
            // DROPPED ON LOAD - a rule quietly loses a condition and starts
            // matching things it should not, with no build error and no log
            // line. The compiler cannot see a runtime loop bound.
            //
            // It walks the whole underlying range instead. KindName has no
            // default case, so a kind nobody named returns "" through its own
            // fallthrough, and the empty check below skips it. Casting an
            // out-of-range value to an enum with a fixed uint8_t underlying
            // type is well defined for the whole of that type's range, so this
            // is not reading past anything.
            //
            // The result: adding an enumerator now costs exactly one line in
            // KindName, and the switch there has no default, so FORGETTING that
            // line is a compiler warning rather than a data loss.
            for (std::uint16_t i = 0; i <= 0xFFu; ++i) {
                const auto  k    = static_cast<ConditionKind>(static_cast<std::uint8_t>(i));
                const char* name = KindName(k);
                if (!name || !*name) {
                    continue;
                }
                if (a_s == name) {
                    return k;
                }
            }
            return std::nullopt;
        }

        Json::Value FormToJson(const FormKey& a_key) {
            Json::Value v(Json::objectValue);
            v["mod"] = a_key.modName;
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "0x%06X", a_key.localFormID);
            v["id"] = buf;
            return v;
        }

        bool JsonToForm(const Json::Value& a_v, FormKey& a_out) {
            if (!a_v.isObject() || !a_v["mod"].isString()) {
                return false;
            }
            a_out.modName = a_v["mod"].asString();
            const auto& id = a_v["id"];
            if (id.isString()) {
                const std::string s = id.asString();
                const char* begin = s.c_str();
                // Always hex, regardless of a "0x" prefix: every FormID in
                // Skyrim tooling is hex, and this codec's own writer
                // (FormToJson above) always emits "0x%06X". A base-10
                // fallback for an unprefixed string would silently mis-target
                // any hand-typed id - JsonCodec.cpp:207,227,233,266 all parse
                // the same "id" key as base 16 unconditionally.
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    begin += 2;
                }
                std::uint32_t value = 0;
                const auto res = std::from_chars(begin, s.c_str() + s.size(), value, 16);
                if (res.ec != std::errc{}) {
                    return false;
                }
                a_out.localFormID = value;
                return true;
            }
            if (id.isUInt()) {
                a_out.localFormID = id.asUInt();
                return true;
            }
            return false;
        }
    }

    Json::Value RuleToJson(const Rule& a_rule) {
        Json::Value v(Json::objectValue);
        v["id"]       = a_rule.id;
        v["name"]     = a_rule.name;
        v["enabled"]  = a_rule.enabled;
        v["priority"] = a_rule.priority;

        switch (a_rule.base.kind) {
        case BaseKind::kKeep:
            v["base"] = "keep";
            break;
        case BaseKind::kRealGear:
            v["base"] = "real";
            break;
        case BaseKind::kOutfit: {
            Json::Value b(Json::objectValue);
            b["outfit"] = a_rule.base.outfitName;
            v["base"]   = b;
            break;
        }
        }

        if (!a_rule.overlay.empty()) {
            Json::Value ov(Json::objectValue);
            for (const auto& [bit, entry] : a_rule.overlay) {
                Json::Value e(Json::objectValue);
                if (entry.kind == SlotEntry::Kind::kHide) {
                    e["hide"] = true;
                    // ⚠ AND THE STYLE THE HIDE COVERS, when it covers one. The
                    // overlay row's left click hides a styled slot by tucking
                    // its key under the hide and hands it back on the way out
                    // (RuleModel.h, ToggleOverlayHide), which was lossless in
                    // memory and lost through the file until this line.
                    //
                    // Deliberately NOT a kRulesVersion bump, for the reason the
                    // "show" key below states: the reader takes "hide" first, so
                    // an older build parses the entry correctly and loses only
                    // the covered key, whereas a bump would make it refuse the
                    // whole file.
                    if (!entry.style.Empty()) {
                        e["style"] = FormToJson(FormKey{ entry.style.modName,
                                                         entry.style.localFormID });
                    }
                } else if (entry.kind == SlotEntry::Kind::kStyle) {
                    e["style"] = FormToJson(FormKey{ entry.style.modName,
                                                     entry.style.localFormID });
                } else {
                    // ⚠ kPassthrough used to be dropped here, on the reasoning
                    // that it "carries no instruction". That was true while a
                    // single winning rule supplied the whole overlay. It stopped
                    // being true when composition went additive: overlays now
                    // merge per slot with the highest-priority rule winning, so
                    // an explicit passthrough on a higher-priority rule is what
                    // OVERRIDES a lower-priority rule's hide. That is the entire
                    // mechanism behind the Headgear control's "Shown" state and
                    // behind the helmet pack's combat rule.
                    //
                    // Dropping it meant "Shown" silently reverted to "Default"
                    // on the next save/load - it survived in memory and vanished
                    // through the codec, which is the worst shape a bug like
                    // this can take.
                    //
                    // Deliberately NOT a kRulesVersion bump: an older build
                    // reading this key logs "neither hide nor style, skipped"
                    // and loses only those entries, whereas a version bump would
                    // make it refuse the whole file.
                    e["show"] = true;
                }
                // The KEY is the biped slot number (30 to 61), never the
                // internal bit index. JsonCodec.cpp:75 writes `bit + 30` for
                // outfit slots for the same reason: a user hand-editing a
                // rules file, and every author writing a rule pack, works in
                // the numbers the Creation Kit shows.
                ov[std::to_string(bit + 30u)] = e;
            }
            v["overlay"] = ov;
        }

        Json::Value conds(Json::arrayValue);
        for (const auto& c : a_rule.conditions) {
            Json::Value cv(Json::objectValue);
            cv["kind"]   = KindName(c.kind);
            cv["negate"] = c.negate;
            switch (c.kind) {
            case ConditionKind::kLocation:
                cv["keyword"] = FormToJson(c.form);
                break;
            case ConditionKind::kCell:
                cv["cell"] = FormToJson(c.form);
                break;
            case ConditionKind::kRegion:
                cv["region"] = FormToJson(c.form);
                break;
            case ConditionKind::kWeather:
                cv["flag"] = c.weather == WeatherFlag::kRaining ? "raining" : "snowing";
                break;
            case ConditionKind::kTimeOfDay:
                cv["start"] = c.startHour;
                cv["end"]   = c.endHour;
                break;
            case ConditionKind::kWornSlot:
                cv["slot"] = c.slotBit;
                break;
            case ConditionKind::kAdvanced:
                cv["text"] = c.advancedText;
                break;
            default:
                break;
            }
            conds.append(cv);
        }
        v["conditions"] = conds;
        return v;
    }

    bool JsonToRule(const Json::Value& a_json, Rule& a_out, std::vector<std::string>* a_warnings) {
        if (!a_json.isObject() || !a_json["id"].isString() ||
            a_json["id"].asString().empty()) {
            return false;
        }
        a_out    = Rule{};
        a_out.id = a_json["id"].asString();
        if (a_json["name"].isString()) {
            a_out.name = a_json["name"].asString();
        }
        if (a_json["enabled"].isBool()) {
            a_out.enabled = a_json["enabled"].asBool();
        }
        // isInt(), not isIntegral(): isIntegral() accepts any jsoncpp numeric
        // type representing a whole number, including ones wider than a
        // 32-bit int, and asInt() asserts (throws - this build has
        // JSON_USE_EXCEPTION=1) if the value doesn't actually fit. A
        // hand-typed "priority": 9999999999 must degrade to "ignored", not
        // crash the load.
        if (a_json["priority"].isInt()) {
            a_out.priority = a_json["priority"].asInt();
        }

        const auto& base = a_json["base"];
        if (base.isString()) {
            a_out.base.kind = base.asString() == "real" ? BaseKind::kRealGear
                                                        : BaseKind::kKeep;
        } else if (base.isObject() && base["outfit"].isString()) {
            a_out.base.kind       = BaseKind::kOutfit;
            a_out.base.outfitName = base["outfit"].asString();
        }

        const auto& ov = a_json["overlay"];
        if (ov.isObject()) {
            for (const auto& key : ov.getMemberNames()) {
                // Keys are biped slot numbers 30 to 61; convert to the
                // internal bit index. Anything outside that range is a typo in
                // a hand-edited file: skip the entry, keep the rule.
                std::uint32_t slot = 0;
                const auto res = std::from_chars(key.c_str(), key.c_str() + key.size(), slot);
                if (res.ec != std::errc{} || slot < 30u || slot > 61u) {
                    if (a_warnings) {
                        a_warnings->push_back("rule " + a_out.id + ": overlay key \"" + key +
                                              "\" is not a slot number 30-61, skipped");
                    }
                    continue;
                }
                const std::uint32_t bit = BitForEditorSlot(slot);
                const auto& e = ov[key];
                // A hand-editor's most likely shorthand mistake is a bare
                // value ("30": "hide") instead of an object. jsoncpp's
                // operator[] on a non-object/non-null Value asserts (throws,
                // JSON_USE_EXCEPTION=1 in this build), so this guard MUST run
                // before e["hide"]/e["style"] are indexed at all - the
                // conditions loop below already guards its array elements the
                // same way.
                if (!e.isObject()) {
                    if (a_warnings) {
                        a_warnings->push_back("rule " + a_out.id + ": overlay slot " +
                                              std::to_string(slot) + " is not an object, skipped");
                    }
                    continue;
                }
                SlotEntry entry;
                if (e["hide"].isBool() && e["hide"].asBool()) {
                    entry.kind = SlotEntry::Kind::kHide;
                    // The style this hide covers, if the file carries one. A
                    // bad form here is dropped rather than refused: the hide is
                    // the instruction and it stands on its own, so a covered
                    // key from a mod that is gone costs the show-again step its
                    // style and nothing else.
                    if (FormKey covered; e["style"].isObject() &&
                                         JsonToForm(e["style"], covered)) {
                        entry.style = StyleRefKey{ covered.modName, covered.localFormID };
                    }
                } else if (e["style"].isObject()) {
                    FormKey f;
                    if (!JsonToForm(e["style"], f)) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id + ": overlay slot " +
                                                  std::to_string(slot) +
                                                  " has an invalid style form, skipped");
                        }
                        continue;
                    }
                    entry.kind  = SlotEntry::Kind::kStyle;
                    entry.style = StyleRefKey{ f.modName, f.localFormID };
                } else if (e["show"].isBool() && e["show"].asBool()) {
                    // An explicit "do not hide this slot", which under additive
                    // composition overrides a LOWER-priority rule's hide. See
                    // the encoder's note for why this is a real instruction and
                    // not just an absent one.
                    entry.kind = SlotEntry::Kind::kPassthrough;
                } else {
                    if (a_warnings) {
                        a_warnings->push_back("rule " + a_out.id + ": overlay slot " +
                                              std::to_string(slot) +
                                              " has none of \"hide\", \"show\" or \"style\", skipped");
                    }
                    continue;
                }
                a_out.overlay[bit] = entry;
            }
        }

        const auto& conds = a_json["conditions"];
        if (conds.isArray()) {
            for (const auto& cv : conds) {
                if (!cv.isObject() || !cv["kind"].isString()) {
                    continue;
                }
                Condition c;
                const auto kindStr = cv["kind"].asString();
                const auto kind    = KindFromName(kindStr);
                if (!kind) {
                    if (a_warnings) {
                        a_warnings->push_back("rule " + a_out.id + ": unknown condition kind \"" +
                                              kindStr + "\", clause skipped");
                    }
                    continue;  // a kind from a newer build: skip the clause
                }
                c.kind = *kind;
                if (cv["negate"].isBool()) {
                    c.negate = cv["negate"].asBool();
                }
                switch (c.kind) {
                case ConditionKind::kLocation:
                    if (!JsonToForm(cv["keyword"], c.form)) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": location condition has an invalid "
                                                  "\"keyword\" form, clause skipped");
                        }
                        continue;
                    }
                    break;
                case ConditionKind::kCell:
                    if (!JsonToForm(cv["cell"], c.form)) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": cell condition has an invalid \"cell\" "
                                                  "form, clause skipped");
                        }
                        continue;
                    }
                    break;
                case ConditionKind::kRegion:
                    if (!JsonToForm(cv["region"], c.form)) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": region condition has an invalid \"region\" "
                                                  "form, clause skipped");
                        }
                        continue;
                    }
                    break;
                case ConditionKind::kWeather:
                    c.weather = cv["flag"].isString() && cv["flag"].asString() == "snowing"
                                    ? WeatherFlag::kSnowing
                                    : WeatherFlag::kRaining;
                    break;
                case ConditionKind::kTimeOfDay:
                    if (!cv["start"].isNumeric() || !cv["end"].isNumeric()) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": timeOfDay condition has a non-numeric "
                                                  "\"start\"/\"end\", clause skipped");
                        }
                        continue;
                    }
                    c.startHour = cv["start"].asFloat();
                    c.endHour   = cv["end"].asFloat();
                    break;
                case ConditionKind::kWornSlot:
                    // isUInt(), not isIntegral(): a negative "slot" passes
                    // isIntegral() and asserts (throws) inside asUInt() - the
                    // same precondition-mismatch bug as "priority" above.
                    if (!cv["slot"].isUInt()) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": wornSlot condition has an invalid "
                                                  "\"slot\", clause skipped");
                        }
                        continue;
                    }
                    c.slotBit = cv["slot"].asUInt();
                    if (c.slotBit >= kBitCount) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": wornSlot condition's \"slot\" is out of "
                                                  "range, clause skipped");
                        }
                        continue;
                    }
                    break;
                case ConditionKind::kAdvanced:
                    if (!cv["text"].isString()) {
                        if (a_warnings) {
                            a_warnings->push_back("rule " + a_out.id +
                                                  ": advanced condition is missing \"text\", "
                                                  "clause skipped");
                        }
                        continue;
                    }
                    c.advancedText = cv["text"].asString();
                    break;
                default:
                    break;
                }
                a_out.conditions.push_back(std::move(c));
            }
        }
        return true;
    }

    Json::Value RulesToJson(const RuleSet& a_rules) {
        Json::Value root(Json::objectValue);
        root["version"] = kRulesVersion;
        Json::Value arr(Json::arrayValue);
        for (const auto& r : a_rules) {
            if (r.packName.empty()) {  // pack rules are read-only, never mirrored
                arr.append(RuleToJson(r));
            }
        }
        root["rules"] = arr;
        return root;
    }

    bool JsonToRules(const Json::Value& a_root, RuleSet& a_out, std::string& a_error,
                     std::vector<std::string>* a_warnings) {
        a_out.clear();
        if (!a_root.isObject()) {
            a_error = "rules document is not an object";
            return false;
        }
        const auto& versionField = a_root["version"];
        int         version      = 0;
        if (!versionField.isNull()) {
            // isInt(), not isIntegral(): a "version" that IS numeric but out
            // of int range would otherwise assert inside asInt() below - the
            // same precondition-mismatch bug class as "priority" and "slot"
            // in JsonToRule. A non-numeric "version" (string, bool, array,
            // object) is rejected outright rather than silently read as
            // version 0 ("ancient file, load everything"), which is the
            // inverse of what this gate exists to protect against.
            if (!versionField.isInt()) {
                a_error = "rules file \"version\" field is not a valid integer; nothing loaded";
                return false;
            }
            version = versionField.asInt();
        }
        if (version > kRulesVersion) {
            a_error = "rules file version " + std::to_string(version) +
                      " is newer than this build understands (" +
                      std::to_string(kRulesVersion) + "); nothing loaded";
            return false;
        }
        const auto& arr = a_root["rules"];
        if (!arr.isArray()) {
            a_error = "rules document has no rules array";
            return false;
        }
        std::set<std::string> seen;
        for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
            Rule r;
            if (!JsonToRule(arr[i], r, a_warnings)) {
                if (a_warnings) {
                    a_warnings->push_back("rules[" + std::to_string(i) +
                                          "] is not an object or has no id, skipped");
                }
                continue;
            }
            if (!seen.insert(r.id).second) {
                if (a_warnings) {
                    a_warnings->push_back("rule " + r.id + ": duplicate id, later rule dropped");
                }
                continue;  // duplicate id: the later rule loses
            }
            a_out.push_back(std::move(r));
        }
        return true;
    }

}  // namespace OS::RuleCodec
