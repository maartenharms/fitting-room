// Pure-logic tests for the rules JSON codec. jsoncpp only, no engine.
#include "RuleCodec.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS::Rules;

int main() {
    {  // full round trip over every field the model carries
        Rule r;
        r.id              = "r-7f3a";
        r.name            = "Towns";
        r.enabled         = true;
        r.priority        = 40;
        r.base.kind       = BaseKind::kOutfit;
        r.base.outfitName = "Town Clothes";

        OS::SlotEntry hide;
        hide.kind = OS::SlotEntry::Kind::kHide;
        r.overlay[OS::kBitHead] = hide;
        OS::SlotEntry style;
        style.kind  = OS::SlotEntry::Kind::kStyle;
        style.style = OS::StyleRefKey{ "Elaborate Textiles.esp", 0x812 };
        r.overlay[OS::kBitFeet] = style;

        Condition loc;
        loc.kind = ConditionKind::kLocation;
        loc.form = FormKey{ "Skyrim.esm", 0x13168 };
        Condition tod;
        tod.kind      = ConditionKind::kTimeOfDay;
        tod.startHour = 7.0f;
        tod.endHour   = 22.0f;
        Condition adv;
        adv.kind         = ConditionKind::kAdvanced;
        adv.advancedText = "IsSneaking == 1";
        Condition worn;
        worn.kind    = ConditionKind::kWornSlot;
        worn.slotBit = OS::kBitBody;
        worn.negate  = true;
        r.conditions = { loc, tod, adv, worn };

        // The serialized keys are biped slot numbers, not bit indices: this is
        // the hand-editable surface and the shape rule packs ship in.
        const auto encoded = OS::RuleCodec::RuleToJson(r);
        CHECK(encoded["overlay"].isMember("30"));  // kBitHead
        CHECK(encoded["overlay"].isMember("37"));  // kBitFeet
        CHECK(!encoded["overlay"].isMember("0"));

        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(encoded, back));
        CHECK(back.id == r.id);
        CHECK(back.name == r.name);
        CHECK(back.priority == 40);
        CHECK(back.base.kind == BaseKind::kOutfit);
        CHECK(back.base.outfitName == "Town Clothes");
        CHECK(back.overlay.size() == 2);
        CHECK(back.overlay.at(OS::kBitHead).kind == OS::SlotEntry::Kind::kHide);
        CHECK(back.overlay.at(OS::kBitFeet).style.modName == "Elaborate Textiles.esp");
        CHECK(back.overlay.at(OS::kBitFeet).style.localFormID == 0x812);
        CHECK(back.conditions.size() == 4);
        CHECK(back.conditions[0].form == loc.form);
        CHECK(back.conditions[1].startHour == 7.0f);
        CHECK(back.conditions[1].endHour == 22.0f);
        CHECK(back.conditions[2].advancedText == "IsSneaking == 1");
        CHECK(back.conditions[3].negate);
        CHECK(back.conditions[3].slotBit == OS::kBitBody);
    }
    {  // the two non-outfit base encodings survive
        Rule keep;
        keep.id = "k";
        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(OS::RuleCodec::RuleToJson(keep), back));
        CHECK(back.base.kind == BaseKind::kKeep);

        Rule real;
        real.id        = "r";
        real.base.kind = BaseKind::kRealGear;
        CHECK(OS::RuleCodec::JsonToRule(OS::RuleCodec::RuleToJson(real), back));
        CHECK(back.base.kind == BaseKind::kRealGear);
    }
    {  // a rule with no id is rejected; everything else degrades quietly
        Json::Value bad(Json::objectValue);
        bad["name"] = "no id";
        Rule out;
        CHECK(!OS::RuleCodec::JsonToRule(bad, out));
    }
    {  // an unknown condition kind is skipped, the rest of the rule survives
        Json::Value v(Json::objectValue);
        v["id"] = "x";
        Json::Value conds(Json::arrayValue);
        Json::Value unknown(Json::objectValue);
        unknown["kind"] = "moonPhase";  // from some future build
        conds.append(unknown);
        Json::Value known(Json::objectValue);
        known["kind"] = "combat";
        conds.append(known);
        v["conditions"] = conds;
        Rule out;
        CHECK(OS::RuleCodec::JsonToRule(v, out));
        CHECK(out.conditions.size() == 1);
        CHECK(out.conditions[0].kind == ConditionKind::kCombat);
    }
    {  // document round trip
        RuleSet rules;
        Rule a;
        a.id = "a";
        a.priority = 5;
        Rule b;
        b.id = "b";
        b.priority = 9;
        rules = { a, b };
        RuleSet back;
        std::string err;
        CHECK(OS::RuleCodec::JsonToRules(OS::RuleCodec::RulesToJson(rules), back, err));
        CHECK(back.size() == 2);
        CHECK(back[0].id == "a");
        CHECK(back[1].id == "b");
    }
    {  // a newer version loads NOTHING rather than truncating the user's rules
        Json::Value root(Json::objectValue);
        root["version"] = OS::RuleCodec::kRulesVersion + 1;
        root["rules"]   = Json::Value(Json::arrayValue);
        RuleSet     back;
        std::string err;
        CHECK(!OS::RuleCodec::JsonToRules(root, back, err));
        CHECK(!err.empty());
        CHECK(back.empty());
    }
    {  // duplicate ids: the later rule is dropped, the file still loads
        Json::Value root(Json::objectValue);
        root["version"] = OS::RuleCodec::kRulesVersion;
        Json::Value arr(Json::arrayValue);
        Json::Value one(Json::objectValue);
        one["id"]   = "dup";
        one["name"] = "first";
        Json::Value two(Json::objectValue);
        two["id"]   = "dup";
        two["name"] = "second";
        arr.append(one);
        arr.append(two);
        root["rules"] = arr;
        RuleSet     back;
        std::string err;
        CHECK(OS::RuleCodec::JsonToRules(root, back, err));
        CHECK(back.size() == 1);
        CHECK(back[0].name == "first");
    }
    {  // pack rules are never written back to the user's file
        RuleSet rules;
        Rule mine;
        mine.id = "mine";
        Rule pack;
        pack.id       = "pack";
        pack.packName = "Curator.json";
        rules = { mine, pack };
        RuleSet     back;
        std::string err;
        CHECK(OS::RuleCodec::JsonToRules(OS::RuleCodec::RulesToJson(rules), back, err));
        CHECK(back.size() == 1);
        CHECK(back[0].id == "mine");
    }
    {  // a malformed overlay entry (not an object) is skipped rather than
        // crashing - this is exactly the shorthand a hand-editor would guess
        // ("30": "hide" instead of {"hide": true}), and jsoncpp asserts on
        // operator[] into a non-object Value, so this is a crash regression
        // test, not just a tolerance test.
        Json::Value v(Json::objectValue);
        v["id"] = "x";
        Json::Value ov(Json::objectValue);
        ov["30"] = "hide";                         // string, not an object
        ov["31"] = 5;                               // number, not an object
        ov["32"] = Json::Value(Json::arrayValue);   // array, not an object
        v["overlay"] = ov;
        Rule                      out;
        std::vector<std::string>  warnings;
        CHECK(OS::RuleCodec::JsonToRule(v, out, &warnings));  // must not throw
        CHECK(out.overlay.empty());
        CHECK(warnings.size() == 3);
    }
    {  // overlay keys outside the 30-61 biped slot range are skipped, the
        // rule survives - "0" is the internal bit index a confused writer
        // could produce, "99" is a plain typo.
        Json::Value v(Json::objectValue);
        v["id"] = "x";
        Json::Value ov(Json::objectValue);
        Json::Value hide(Json::objectValue);
        hide["hide"] = true;
        ov["99"]     = hide;
        ov["0"]      = hide;
        v["overlay"] = ov;
        Rule out;
        CHECK(OS::RuleCodec::JsonToRule(v, out));
        CHECK(out.overlay.empty());
    }
    {  // a negative wornSlot "slot" and an out-of-int32-range "priority" are
        // rejected, not crashed on - both used to reach an asXxx() call whose
        // precondition the old isIntegral() gate did not actually enforce.
        Json::Value v(Json::objectValue);
        v["id"]       = "x";
        v["priority"] = Json::Int64(9999999999LL);  // overflows int32
        Json::Value conds(Json::arrayValue);
        Json::Value worn(Json::objectValue);
        worn["kind"] = "wornSlot";
        worn["slot"] = -1;
        conds.append(worn);
        v["conditions"] = conds;
        Rule out;
        CHECK(OS::RuleCodec::JsonToRule(v, out));  // must not throw
        CHECK(out.priority == 0);                  // out-of-range priority ignored
        CHECK(out.conditions.empty());              // negative-slot clause dropped
    }
    {  // the document's own "version" field must actually be written - this
        // is the assertion that would have caught deleting the
        // `root["version"] = kRulesVersion;` line while every other
        // JsonToRules test still passed.
        RuleSet rules;
        Rule    a;
        a.id  = "a";
        rules = { a };
        const auto encoded = OS::RuleCodec::RulesToJson(rules);
        CHECK(encoded.isMember("version"));
        CHECK(encoded["version"].asInt() == OS::RuleCodec::kRulesVersion);
    }
    {  // a "version" present but not a plain integer is rejected outright,
        // not silently read as version 0 ("ancient file, load everything") -
        // the exact inverse of what this gate exists to prevent.
        Json::Value root(Json::objectValue);
        root["version"] = "9";  // string, not a number
        root["rules"]   = Json::Value(Json::arrayValue);
        RuleSet     back;
        std::string err;
        CHECK(!OS::RuleCodec::JsonToRules(root, back, err));
        CHECK(!err.empty());
        CHECK(back.empty());
    }
    {  // JsonToRules rejects a non-object root outright.
        Json::Value root(Json::arrayValue);  // not an object
        RuleSet     back;
        std::string err;
        CHECK(!OS::RuleCodec::JsonToRules(root, back, err));
        CHECK(!err.empty());
        CHECK(back.empty());
    }
    {  // JsonToRules rejects a document whose "rules" is missing or not an array.
        Json::Value root(Json::objectValue);
        root["version"] = OS::RuleCodec::kRulesVersion;
        // "rules" intentionally absent.
        RuleSet     back;
        std::string err;
        CHECK(!OS::RuleCodec::JsonToRules(root, back, err));
        CHECK(!err.empty());

        root["rules"] = "not an array";
        RuleSet     back2;
        std::string err2;
        CHECK(!OS::RuleCodec::JsonToRules(root, back2, err2));
        CHECK(!err2.empty());
    }
    {  // A kPassthrough overlay entry round-trips as "show": true.
        //
        // ⚠ This test previously asserted the OPPOSITE - that kPassthrough is
        // never written out, because it "carries no instruction". That held
        // while one winning rule supplied the whole overlay. Composition is
        // additive now: overlays merge per slot, highest priority winning, so an
        // explicit passthrough is what overrides a LOWER-priority rule's hide.
        // It is the Headgear control's "Shown" state and the helmet pack's
        // combat rule. Dropping it on encode meant "Shown" reverted to "Default"
        // on the next save/load - correct in memory, gone through the codec.
        Rule r;
        r.id                    = "x";
        r.overlay[OS::kBitHead] = OS::SlotEntry{};  // kind defaults to kPassthrough
        const auto encoded = OS::RuleCodec::RuleToJson(r);
        CHECK(encoded["overlay"].isMember("30"));
        CHECK(encoded["overlay"]["30"]["show"].asBool());

        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(encoded, back));
        CHECK(back.overlay.count(OS::kBitHead) == 1);
        if (back.overlay.count(OS::kBitHead) == 1) {
            CHECK(back.overlay.at(OS::kBitHead).kind == OS::SlotEntry::Kind::kPassthrough);
        }
    }
    {  // A hide carries through the file the style it covers.
        //
        // ⚠ The Rules overlay row hides a styled slot by tucking its key under
        // the hide and hands it back on the way out (RuleModel.h,
        // ToggleOverlayHide). That was lossless in memory and lost through the
        // codec, which is the same shape the "show" bug above took: correct
        // until you save.
        Rule r;
        r.id = "x";
        r.overlay[OS::kBitHead] =
            OS::SlotEntry{ OS::SlotEntry::Kind::kHide,
                           OS::StyleRefKey{ "Elaborate Textiles.esp", 0x812 } };
        const auto encoded = OS::RuleCodec::RuleToJson(r);
        CHECK(encoded["overlay"]["30"]["hide"].asBool());
        CHECK(encoded["overlay"]["30"]["style"].isObject());

        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(encoded, back));
        CHECK(back.overlay.count(OS::kBitHead) == 1);
        if (back.overlay.count(OS::kBitHead) == 1) {
            // ⚠ HIDE STILL WINS THE KIND. The reader takes "hide" first, which
            // is what lets an older build parse this entry and lose only the
            // covered key instead of refusing the whole file.
            CHECK(back.overlay.at(OS::kBitHead).kind == OS::SlotEntry::Kind::kHide);
            CHECK(back.overlay.at(OS::kBitHead).style.modName == "Elaborate Textiles.esp");
            CHECK(back.overlay.at(OS::kBitHead).style.localFormID == 0x812);
        }
    }
    {  // A hide covering nothing writes no style key at all, so the ordinary
        // case stays what every rule pack already on disk holds.
        Rule r;
        r.id                    = "x";
        r.overlay[OS::kBitHead] = OS::SlotEntry{ OS::SlotEntry::Kind::kHide, {} };
        const auto encoded      = OS::RuleCodec::RuleToJson(r);
        CHECK(encoded["overlay"]["30"]["hide"].asBool());
        CHECK(!encoded["overlay"]["30"].isMember("style"));
    }
    {  // An overlay entry naming none of the three instructions is still
        // skipped, with a warning - a hand-edited "31": {} is a typo, not a
        // passthrough. "show": false is the same case: it asserts nothing.
        Json::Value ov(Json::objectValue);
        ov["31"] = Json::Value(Json::objectValue);
        ov["41"] = Json::Value(Json::objectValue);
        ov["41"]["show"] = false;
        Json::Value rv(Json::objectValue);
        rv["id"]      = "r";
        rv["overlay"] = ov;

        Rule                     back;
        std::vector<std::string> warnings;
        CHECK(OS::RuleCodec::JsonToRule(rv, back, &warnings));
        CHECK(back.overlay.empty());
        CHECK(warnings.size() == 2);
    }

    {  // The three kinds appended after kAdvanced round-trip by NAME.
        //
        // This is the regression guard for KindFromName's hand-written loop
        // bound: it stopped at kAdvanced, and leaving it there would make every
        // dialogue/region/vampire clause decode as "unknown condition kind" and
        // be dropped - silent data loss on the next save, with a clean build
        // and every other test still green.
        Rule r;
        r.id = "ht";

        Condition dlg;
        dlg.kind = ConditionKind::kDialogue;
        Condition reg;
        reg.kind = ConditionKind::kRegion;
        reg.form = FormKey{ "Skyrim.esm", 0x1234 };
        Condition vamp;
        vamp.kind   = ConditionKind::kVampire;
        vamp.negate = true;
        r.conditions = { dlg, reg, vamp };

        const auto encoded = OS::RuleCodec::RuleToJson(r);
        CHECK(encoded["conditions"][0]["kind"].asString() == "dialogue");
        CHECK(encoded["conditions"][1]["kind"].asString() == "region");
        CHECK(encoded["conditions"][2]["kind"].asString() == "vampire");
        // The region form goes under its own key, not "cell" or "keyword".
        CHECK(encoded["conditions"][1].isMember("region"));

        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(encoded, back));
        CHECK(back.conditions.size() == 3);  // nothing silently dropped
        // Guarded, not indexed straight off the CHECK above. The failure this
        // test exists to catch is clauses being DROPPED, so the vector is empty
        // in exactly the case the assertions below want to report on - and
        // indexing it there crashes the runner (verified: reverting the bound
        // gave 0xC0000005, not a failure count). A crashed suite says far less
        // than a failed one.
        if (back.conditions.size() == 3) {
            CHECK(back.conditions[0].kind == ConditionKind::kDialogue);
            CHECK(back.conditions[1].kind == ConditionKind::kRegion);
            CHECK(back.conditions[1].form == reg.form);
            CHECK(back.conditions[2].kind == ConditionKind::kVampire);
            CHECK(back.conditions[2].negate);
        }
    }
    {  // ⚠⚠ EVERY KIND ROUND-TRIPS, WALKED OFF THE ENUM RATHER THAN LISTED.
        // The test above names the three kinds that were last when it was
        // written, which is exactly how the trap it guards keeps coming back:
        // the next appended kind is not in its list either. This one iterates
        // the whole range, so a kind added tomorrow is covered without anybody
        // remembering to come here.
        //
        // What it catches: a kind with no KindName case encodes as "" and
        // decodes as nothing, and a kind the reverse lookup cannot reach is
        // DROPPED ON LOAD. Both are silent - a clean build, a clean save, and a
        // rule that quietly lost a condition and now matches things it should
        // not.
        for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(ConditionKind::kCasting);
             ++i) {
            const auto kind = static_cast<ConditionKind>(i);
            Rule       r;
            r.id = "every";
            Condition c;
            c.kind = kind;
            // The two kinds that carry a payload the encoder refuses to write
            // without: a formless location/cell/region clause is deliberately
            // skipped, which is its own tested behaviour above and not what
            // this loop is about.
            if (kind == ConditionKind::kLocation || kind == ConditionKind::kCell ||
                kind == ConditionKind::kRegion) {
                c.form = FormKey{ "Skyrim.esm", 0x800 };
            }
            if (kind == ConditionKind::kAdvanced) {
                c.advancedText = "x";
            }
            r.conditions = { c };

            const auto encoded = OS::RuleCodec::RuleToJson(r);
            const auto name    = encoded["conditions"][0]["kind"].asString();
            CHECK(!name.empty());  // a kind nobody named in KindName

            Rule back;
            CHECK(OS::RuleCodec::JsonToRule(encoded, back));
            CHECK(back.conditions.size() == 1);  // a kind the reverse lookup cannot reach
            if (back.conditions.size() == 1) {
                CHECK(back.conditions[0].kind == kind);
            }
        }
    }
    {  // Casting names itself, and the name is part of the on-disk format, so
        // renaming it silently orphans every rule already saved with it.
        Rule r;
        r.id = "cast";
        Condition c;
        c.kind = ConditionKind::kCasting;
        r.conditions = { c };
        CHECK(OS::RuleCodec::RuleToJson(r)["conditions"][0]["kind"].asString() == "casting");
    }
    {  // A region clause with no usable form is skipped rather than kept as a
        // match-nothing clause, same as location and cell.
        Json::Value cv(Json::objectValue);
        cv["kind"] = "region";
        Json::Value conds(Json::arrayValue);
        conds.append(cv);
        Json::Value rv(Json::objectValue);
        rv["id"]         = "r";
        rv["conditions"] = conds;

        Rule back;
        CHECK(OS::RuleCodec::JsonToRule(rv, back));
        CHECK(back.conditions.empty());
    }

    if (g_failures == 0) {
        std::printf("all RuleCodec tests passed\n");
    }
    return g_failures;
}
