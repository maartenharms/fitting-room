// Pure-logic tests for the rules data model. No engine, no RE:: types.
#include "RuleModel.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::Rules;
    using OS::SlotEntry;

    auto styleEntry = [](const char* mod, std::uint32_t id) {
        SlotEntry e;
        e.kind  = SlotEntry::Kind::kStyle;
        e.style = OS::StyleRefKey{ mod, id };
        return e;
    };
    auto hideEntry = [] {
        SlotEntry e;
        e.kind = SlotEntry::Kind::kHide;
        return e;
    };

    {  // overlay hide wins over a base style on the same slot
        OS::Outfit base;
        base.SetStyle(OS::kBitHead, OS::StyleRefKey{ "Base.esp", 0x800 });
        Overlay ov;
        ov[OS::kBitHead] = hideEntry();
        const auto out = Compose(base, ov);
        CHECK(out.EntryFor(OS::kBitHead).kind == SlotEntry::Kind::kHide);
        CHECK((out.HideMask() & OS::MaskForEditorSlot(30)) != 0);
    }
    {  // overlay style wins over a base style
        OS::Outfit base;
        base.SetStyle(OS::kBitFeet, OS::StyleRefKey{ "Base.esp", 0x801 });
        Overlay ov;
        ov[OS::kBitFeet] = styleEntry("Over.esp", 0x900);
        const auto out = Compose(base, ov);
        CHECK(out.EntryFor(OS::kBitFeet).style.modName == "Over.esp");
    }
    {  // untouched base slots pass through
        OS::Outfit base;
        base.SetStyle(OS::kBitBody, OS::StyleRefKey{ "Base.esp", 0x802 });
        base.SetStyle(OS::kBitFeet, OS::StyleRefKey{ "Base.esp", 0x803 });
        Overlay ov;
        ov[OS::kBitFeet] = hideEntry();
        const auto out = Compose(base, ov);
        CHECK(out.EntryFor(OS::kBitBody).style.localFormID == 0x802);
        CHECK(out.EntryFor(OS::kBitFeet).kind == SlotEntry::Kind::kHide);
    }
    {  // overlay over a DEFAULT outfit yields just the overlay: the kKeep
       // case, the one that renders over the player's own real gear
        Overlay ov;
        ov[OS::kBitHead] = hideEntry();
        const auto out = Compose(OS::Outfit{}, ov);
        CHECK(out.EntryFor(OS::kBitHead).kind == SlotEntry::Kind::kHide);
        CHECK(out.EntryFor(OS::kBitBody).kind == SlotEntry::Kind::kPassthrough);
    }
    {  // no overlay leaves the base untouched
        OS::Outfit base;
        base.SetStyle(OS::kBitBody, OS::StyleRefKey{ "Base.esp", 0x804 });
        const auto out = Compose(base, {});
        CHECK(out.EntryFor(OS::kBitBody).style.localFormID == 0x804);
    }
    {  // Compose skips an overlay entry whose bit is out of range instead of
       // writing past the slot array. An out-of-range key can arrive from a
       // rules file authored against a future, wider slot layout.
        OS::Outfit base;
        base.SetStyle(OS::kBitBody, OS::StyleRefKey{ "Base.esp", 0x805 });
        Overlay ov;
        ov[OS::kBitCount]       = hideEntry();               // exactly one past the end
        ov[OS::kBitCount + 100] = styleEntry("OOB.esp", 0x1);
        const auto out = Compose(base, ov);
        CHECK(out.EntryFor(OS::kBitBody).kind == SlotEntry::Kind::kStyle);
        CHECK(out.EntryFor(OS::kBitBody).style.localFormID == 0x805);
        CHECK(out.HideMask() == 0);
        CHECK(out.StyleMask() == OS::MaskForEditorSlot(32));
    }
    {  // SameEntry/SameOverlay on plain style-vs-style entries - the majority
       // case for real overlay content, not just the hide/stale-key edge case
        const auto s1 = styleEntry("A.esp", 0x10);
        const auto s2 = styleEntry("A.esp", 0x10);
        const auto s3 = styleEntry("A.esp", 0x11);
        CHECK(SameEntry(s1, s2));
        CHECK(!SameEntry(s1, s3));

        Overlay a;
        Overlay b;
        a[OS::kBitHead] = s1;
        b[OS::kBitHead] = s2;
        CHECK(SameOverlay(a, b));
        b[OS::kBitHead] = s3;
        CHECK(!SameOverlay(a, b));
    }
    {  // SameOverlay ignores a kHide entry's stale style payload, exactly as
       // Outfit.h's SlotDiffers does. A defaulted comparison would call these
       // two different and cause a pointless re-apply every evaluation.
        Overlay a;
        Overlay b;
        SlotEntry hideWithStaleKey;
        hideWithStaleKey.kind  = SlotEntry::Kind::kHide;
        hideWithStaleKey.style = OS::StyleRefKey{ "Stale.esp", 0x1 };
        a[OS::kBitHead] = hideEntry();
        b[OS::kBitHead] = hideWithStaleKey;
        CHECK(SameOverlay(a, b));

        b[OS::kBitHead] = styleEntry("X.esp", 0x1);
        CHECK(!SameOverlay(a, b));
        b.clear();
        CHECK(!SameOverlay(a, b));
    }
    {  // Target equality composes the above
        Target t1{ "r1", Base{ BaseKind::kOutfit, "A" }, {} };
        Target t2{ "r1", Base{ BaseKind::kOutfit, "A" }, {} };
        CHECK(t1 == t2);
        t2.base.outfitName = "B";
        CHECK(!(t1 == t2));
        t2.base.outfitName = "A";
        t2.overlay[OS::kBitHead] = hideEntry();
        CHECK(!(t1 == t2));
    }

    {  // Interior, and negate flipping it
        WorldSnapshot s;
        s.interior = true;
        Condition c;
        c.kind = ConditionKind::kInterior;
        CHECK(Matches(c, s, "r1", 0));
        c.negate = true;
        CHECK(!Matches(c, s, "r1", 0));
        s.interior = false;
        CHECK(Matches(c, s, "r1", 0));
    }
    {  // Location matches any keyword in the chain
        WorldSnapshot s;
        s.locationKeywords = { FormKey{ "Skyrim.esm", 0x13168 },
                               FormKey{ "Skyrim.esm", 0x1CB86 } };
        Condition c;
        c.kind = ConditionKind::kLocation;
        c.form = FormKey{ "Skyrim.esm", 0x1CB86 };
        CHECK(Matches(c, s, "r1", 0));
        c.form = FormKey{ "Skyrim.esm", 0xDEAD };
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // Location: an unset form must not match even a blank keyword entry,
       // same guard as kCell below
        WorldSnapshot s;
        s.locationKeywords = { FormKey{} };
        Condition c;
        c.kind = ConditionKind::kLocation;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // Weather reads the right flag
        WorldSnapshot s;
        s.raining = true;
        Condition c;
        c.kind    = ConditionKind::kWeather;
        c.weather = WeatherFlag::kRaining;
        CHECK(Matches(c, s, "r1", 0));
        c.weather = WeatherFlag::kSnowing;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // TimeOfDay, normal window: inclusive start, exclusive end
        WorldSnapshot s;
        Condition c;
        c.kind      = ConditionKind::kTimeOfDay;
        c.startHour = 7.0f;
        c.endHour   = 22.0f;
        s.gameHour  = 7.0f;
        CHECK(Matches(c, s, "r1", 0));
        s.gameHour = 12.5f;
        CHECK(Matches(c, s, "r1", 0));
        s.gameHour = 22.0f;
        CHECK(!Matches(c, s, "r1", 0));
        s.gameHour = 3.0f;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // TimeOfDay wrapping midnight
        WorldSnapshot s;
        Condition c;
        c.kind      = ConditionKind::kTimeOfDay;
        c.startHour = 22.0f;
        c.endHour   = 6.0f;
        s.gameHour  = 23.5f;
        CHECK(Matches(c, s, "r1", 0));
        s.gameHour = 2.0f;
        CHECK(Matches(c, s, "r1", 0));
        s.gameHour = 12.0f;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // WornSlot reads the real-equipment mask
        WorldSnapshot s;
        s.wornSlotMask = (1u << OS::kBitHead);
        Condition c;
        c.kind    = ConditionKind::kWornSlot;
        c.slotBit = OS::kBitHead;
        CHECK(Matches(c, s, "r1", 0));
        c.slotBit = OS::kBitFeet;
        CHECK(!Matches(c, s, "r1", 0));
        c.negate = true;  // "no boots on" is the negated form
        CHECK(Matches(c, s, "r1", 0));
    }
    {  // WornSlot: an out-of-range bit must not match and must not shift out
       // of range (undefined behaviour on a 32-bit shift by >= 32)
        WorldSnapshot s;
        s.wornSlotMask = (1u << OS::kBitHead);
        Condition c;
        c.kind    = ConditionKind::kWornSlot;
        c.slotBit = 999;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // the boolean player-state kinds
        WorldSnapshot s;
        s.inCombat = true;
        s.sneaking = true;
        s.swimming = false;
        s.mounted  = false;
        Condition c;
        c.kind = ConditionKind::kCombat;
        CHECK(Matches(c, s, "r1", 0));
        c.kind = ConditionKind::kSneaking;
        CHECK(Matches(c, s, "r1", 0));
        c.kind = ConditionKind::kSwimming;
        CHECK(!Matches(c, s, "r1", 0));
        c.kind = ConditionKind::kMounted;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // Casting reads the snapshot's latch like any other boolean, and NEGATE
       // works on it, which is what "change back out of the robe once I have
       // stopped casting" is built from.
       //
       // ⚠ THE LATCH IS NOT THIS FILE'S JOB. WorldWatch turns "a spell left the
       // hand" into "cast within the hold window"; by the time it reaches here
       // it is an ordinary bool, and keeping it that way is what lets the whole
       // matcher stay pure and testable with no engine.
        WorldSnapshot s;
        Condition     c;
        c.kind = ConditionKind::kCasting;
        s.casting = false;
        CHECK(!Matches(c, s, "r1", 0));
        s.casting = true;
        CHECK(Matches(c, s, "r1", 0));
        c.negate = true;
        CHECK(!Matches(c, s, "r1", 0));
        s.casting = false;
        CHECK(Matches(c, s, "r1", 0));
    }
    {  // Advanced reads the pre-resolved result keyed by (rule id, index);
       // a missing entry is false, never a crash
        WorldSnapshot s;
        s.advanced[{ "r1", 2 }] = true;
        Condition c;
        c.kind = ConditionKind::kAdvanced;
        CHECK(Matches(c, s, "r1", 2));
        CHECK(!Matches(c, s, "r1", 0));
        CHECK(!Matches(c, s, "other", 2));
    }
    {  // Cell equality
        WorldSnapshot s;
        s.cell = FormKey{ "Skyrim.esm", 0x1A26F };
        Condition c;
        c.kind = ConditionKind::kCell;
        c.form = FormKey{ "Skyrim.esm", 0x1A26F };
        CHECK(Matches(c, s, "r1", 0));
        c.form = FormKey{ "Skyrim.esm", 0x1 };
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // Cell: an unset form must not match an unset snapshot cell either -
       // "no cell condition" and "the cell is unloaded" must stay distinct
        WorldSnapshot s;
        Condition c;
        c.kind = ConditionKind::kCell;
        CHECK(!Matches(c, s, "r1", 0));
    }
    {  // AllMatch is AND over the list; empty list matches (an always-on rule)
        WorldSnapshot s;
        s.interior = true;
        s.inCombat = true;
        Rule r;
        r.id = "r1";
        Condition a;
        a.kind = ConditionKind::kInterior;
        Condition b;
        b.kind = ConditionKind::kCombat;
        r.conditions = { a, b };
        CHECK(AllMatch(r, s));
        r.conditions[1].negate = true;
        CHECK(!AllMatch(r, s));
        r.conditions.clear();
        CHECK(AllMatch(r, s));
    }

    {  // RenameBasesIn rewrites every kOutfit base naming a_from, leaves
       // every other rule (a different base name, kKeep, kRealGear) alone,
       // and reports the count RuleStore::RenameOutfitEverywhere logs.
        RuleSet rules;
        Rule    match1;
        match1.id                = "r1";
        match1.base.kind         = BaseKind::kOutfit;
        match1.base.outfitName   = "Town Clothes";
        Rule match2               = match1;
        match2.id                = "r2";
        Rule differentName        = match1;
        differentName.id          = "r3";
        differentName.base.outfitName = "Dungeon Gear";
        Rule keepBase;
        keepBase.id        = "r4";
        keepBase.base.kind = BaseKind::kKeep;
        rules = { match1, match2, differentName, keepBase };

        const auto changed = RenameBasesIn(rules, "Town Clothes", "Formal Wear");
        CHECK(changed == 2);
        CHECK(rules[0].base.outfitName == "Formal Wear");
        CHECK(rules[1].base.outfitName == "Formal Wear");
        CHECK(rules[2].base.outfitName == "Dungeon Gear");  // untouched
        CHECK(rules[3].base.kind == BaseKind::kKeep);       // untouched
    }
    {  // RenameBasesIn is a no-op for a blank a_from or a same-name "rename"
       // - neither is a real rename, and treating an empty base name as a
       // wildcard match would be a much worse bug than doing nothing.
        RuleSet rules;
        Rule    r;
        r.id              = "r1";
        r.base.kind       = BaseKind::kOutfit;
        r.base.outfitName = "Town Clothes";
        rules              = { r };

        CHECK(RenameBasesIn(rules, "", "Formal Wear") == 0);
        CHECK(rules[0].base.outfitName == "Town Clothes");
        CHECK(RenameBasesIn(rules, "Town Clothes", "Town Clothes") == 0);
        CHECK(rules[0].base.outfitName == "Town Clothes");
    }

    {  // kDialogue and kVampire are plain snapshot booleans, and negate has to
       // invert them like every other kind.
        WorldSnapshot s;
        Condition     dlg;
        dlg.kind = ConditionKind::kDialogue;
        Condition vamp;
        vamp.kind = ConditionKind::kVampire;

        CHECK(!Matches(dlg, s, "r", 0));
        CHECK(!Matches(vamp, s, "r", 0));

        s.inDialogue = true;
        s.vampire    = true;
        CHECK(Matches(dlg, s, "r", 0));
        CHECK(Matches(vamp, s, "r", 0));

        dlg.negate  = true;
        vamp.negate = true;
        CHECK(!Matches(dlg, s, "r", 0));
        CHECK(!Matches(vamp, s, "r", 0));
    }
    {  // kRegion is any-of over the snapshot's region list, because one cell
       // can sit in several overlapping regions at once. An UNSET form must
       // never match, even against a snapshot carrying regions - the same
       // guard kLocation and kCell have.
        WorldSnapshot s;
        s.regions = { FormKey{ "Skyrim.esm", 0x1000 }, FormKey{ "Dawnguard.esm", 0x2000 } };

        Condition c;
        c.kind = ConditionKind::kRegion;

        CHECK(!Matches(c, s, "r", 0));  // form unset

        c.form = FormKey{ "Dawnguard.esm", 0x2000 };
        CHECK(Matches(c, s, "r", 0));  // second entry, not just the first

        c.form = FormKey{ "Skyrim.esm", 0x1000 };
        CHECK(Matches(c, s, "r", 0));

        // Right id, wrong plugin: FormKey compares BOTH, so this is a miss.
        c.form = FormKey{ "Dragonborn.esm", 0x1000 };
        CHECK(!Matches(c, s, "r", 0));

        // An interior has no regions at all; nothing may match there.
        c.form    = FormKey{ "Skyrim.esm", 0x1000 };
        s.regions.clear();
        CHECK(!Matches(c, s, "r", 0));
    }

    {  // ⚠ ONE DEFINITION OF THE THREE-STATE WORD, shared by the editable card
       // and the Rule Library row. The pack row's comment already said it used
       // "the same three-state word as an editable card" while spelling the
       // ternary out for itself a second time, which is the shape a drift takes
       // before anyone notices it drifted (OS-121).
        CHECK(std::string(RuleStateWord(true, true)) == "Active");
        CHECK(std::string(RuleStateWord(true, false)) == "Waiting");
        CHECK(std::string(RuleStateWord(false, false)) == "Off");
        // ⚠ OFF WINS OVER ACTIVE, and this case is the reason the function is
        // worth having. A switched-off rule cannot be the winner, so if a stale
        // active set ever says it is, the row must not answer "Active" and tell
        // the player a disabled rule is dressing them.
        CHECK(std::string(RuleStateWord(false, true)) == "Off");
    }

    {  // ---- the overlay row's hide toggle -------------------------------
       // ⚠ The Rules overlay row hides and shows a slot with the same click
       // now (user 2026-08-27), so the toggle has to be reversible the way
       // Outfit.h's ToggleHideSlot is. These hold the round trips it promises.
        {  // Nothing there: one click puts a hide on it, another takes it off.
            Overlay ov;
            ToggleOverlayHide(ov, 2);
            CHECK(ov.size() == 1);
            CHECK(ov[2].kind == SlotEntry::Kind::kHide);
            CHECK(ov[2].style.Empty());
            ToggleOverlayHide(ov, 2);
            CHECK(ov.empty());
        }
        {  // ⚠⚠ THE STYLE SURVIVES THE ROUND TRIP. Before the row-wide click
           // the icon simply refused to touch a styled slot; now that the whole
           // row toggles, a hide that dropped the key would throw a style the
           // player picked away on one click.
            // ⚠ Hoisted, because a braced init inside CHECK() splits on its
            // comma and the macro reads two arguments.
            const OS::StyleRefKey picked{ "Markynaz.esl", 0x801 };
            Overlay               ov;
            ov[16] = styleEntry("Markynaz.esl", 0x801);
            ToggleOverlayHide(ov, 16);
            CHECK(ov[16].kind == SlotEntry::Kind::kHide);
            CHECK(ov[16].style == picked);
            ToggleOverlayHide(ov, 16);
            CHECK(ov[16].kind == SlotEntry::Kind::kStyle);
            CHECK(ov[16].style == picked);
        }
        {  // ⚠ A "Shown" carries no key, so showing it again lands on no
           // override rather than back on Shown. Two deliberate clicks, and the
           // row says what it ended up as after each; pinned here so the
           // asymmetry is a decision rather than a surprise.
            Overlay ov;
            ov[1] = SlotEntry{ SlotEntry::Kind::kPassthrough, {} };
            ToggleOverlayHide(ov, 1);
            CHECK(ov[1].kind == SlotEntry::Kind::kHide);
            ToggleOverlayHide(ov, 1);
            CHECK(ov.empty());
        }
        {  // A bit no outfit has is refused rather than stored.
            Overlay ov;
            ToggleOverlayHide(ov, OS::kBitCount);
            CHECK(ov.empty());
        }
        {  // ⚠ AND A COVERED KEY IS STILL NOT A DIFFERENCE. SameOverlay
           // delegates to SlotDiffers exactly so a hide carrying a stale style
           // cannot turn a no-change into a re-apply plus a refresh, and the
           // toggle above is now a source of such hides.
            Overlay bare;
            bare[16] = hideEntry();
            Overlay covered;
            covered[16] = SlotEntry{ SlotEntry::Kind::kHide,
                                     OS::StyleRefKey{ "Markynaz.esl", 0x801 } };
            CHECK(SameOverlay(bare, covered));
        }
    }

    if (g_failures == 0) {
        std::printf("all RuleModel tests passed\n");
    }
    return g_failures;
}
