// Pure-logic tests for the set detector. No engine, no RE:: types.
#include "SetDetector.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::SetDetector;

    {  // NameStem: strip slot-nouns, keep the set identity
        CHECK(NameStem("College of Winterhold Hood") == "college of winterhold");
        CHECK(NameStem("College of Winterhold Robes") == "college of winterhold");
        CHECK(NameStem("Nightingale Armor") == "nightingale");
        CHECK(NameStem("Blades Armor") == "blades");  // faction name, not weapon noise
    }
    {  // NameStem: strip color variants and bracket tags
        CHECK(NameStem("Elaborate Textiles - Emerald Dress") == "elaborate textiles");
        CHECK(NameStem("Elaborate Textiles - Ruby Dress") == "elaborate textiles");
        CHECK(NameStem("Steel Cuirass [VANILLA]") == "steel");  // material kept
    }
    {  // NameStem: keeps material words, strips version numbers
        CHECK(NameStem("Iron Boots") == "iron");
        CHECK(NameStem("Daedric Gauntlets v2") == "daedric");
    }
    {  // NameStem: only slot-noun + variant -> empty (residual case)
        CHECK(NameStem("Boots").empty());
        CHECK(NameStem("Red Dress").empty());
    }
    {  // NameStem: bikini-armor slot words (arms/legs/top) + tags + possessive
        CHECK(NameStem("Abyss Top [OBI]") == "abyss");
        CHECK(NameStem("Abyss Arms [OBI]") == "abyss");   // "arms" is a slot word
        CHECK(NameStem("Abyss Boots [OBI]") == "abyss");
        CHECK(NameStem("Zoe's Boots") == "zoe");          // possessive "s" dropped
    }
    {  // CleanPluginName: strip extension/version, split camelCase, title-case
        CHECK(CleanPluginName("ZerofrostNightingalePrime.esp") ==
              "Zerofrost Nightingale Prime");
        CHECK(CleanPluginName("Common Clothes and Armors.esp") ==
              "Common Clothes And Armors");
        CHECK(CleanPluginName("3BBB_Armor_v2.esp") == "3bbb Armor");
        CHECK(CleanPluginName("Elaborate Textiles.esp") == "Elaborate Textiles");
    }

    // Build a DetectStyle. slot is an EDITOR slot (30=head, 32=body, 33=hands,
    // 37=feet, 46=cloak); the mask/primaryBit derive from it.
    auto mk = [](const char* name, const char* plugin, std::uint32_t editorSlot,
                 std::uint8_t type = 0) {
        DetectStyle s;
        s.name       = name;
        s.source     = plugin;
        s.slotMask   = OS::MaskForEditorSlot(editorSlot);
        s.primaryBit = OS::BitForEditorSlot(editorSlot);
        s.armorType  = type;
        s.key        = OS::StyleRefKey{ plugin, editorSlot * 0x10u + 1u };
        return s;
    };

    {  // one plugin's stem family -> one set covering all four majors
        std::vector<DetectStyle> in = {
            mk("College of Winterhold Hood", "College.esp", 31),
            mk("College of Winterhold Robes", "College.esp", 32),
            mk("College of Winterhold Gloves", "College.esp", 33),
            mk("College of Winterhold Boots", "College.esp", 37),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].name == "College Of Winterhold");
        CHECK(sets[0].source == "College");
        CHECK(sets[0].outfit.EntryFor(OS::kBitBody).kind == OS::SlotEntry::Kind::kStyle);
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).kind == OS::SlotEntry::Kind::kStyle);
    }
    {  // same piece name in two different plugins never merges
        std::vector<DetectStyle> in = {
            mk("Leather Boots", "ModA.esp", 37),
            mk("Leather Cuirass", "ModA.esp", 32),
            mk("Leather Boots", "ModB.esp", 37),
            mk("Leather Cuirass", "ModB.esp", 32),
        };
        CHECK(Detect(in, {}).size() == 2);  // one "Leather" set per plugin
    }
    {  // lone gloves = not a set (body anchor + >=2 majors required)
        std::vector<DetectStyle> in = { mk("Fancy Gloves", "G.esp", 33) };
        CHECK(Detect(in, {}).empty());
    }
    {  // robe + boots = a set (body + 1 more major)
        std::vector<DetectStyle> in = {
            mk("Mage Robe", "M.esp", 32),
            mk("Mage Boots", "M.esp", 37),
        };
        CHECK(Detect(in, {}).size() == 1);
    }
    {  // a single multi-slot robe covering body+feet qualifies on its own
        DetectStyle robe = mk("Wanderer Robe", "W.esp", 32);
        robe.slotMask |= OS::MaskForEditorSlot(37);  // also covers feet
        CHECK(Detect({ robe }, {}).size() == 1);
    }
    {  // color variants of one dress collapse; alternates counted
        std::vector<DetectStyle> in = {
            mk("Elaborate Textiles - Emerald Dress", "ET.esp", 32),
            mk("Elaborate Textiles - Ruby Dress", "ET.esp", 32),
            mk("Elaborate Textiles - Emerald Shoes", "ET.esp", 37),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].name == "Elaborate Textiles");
        bool bodyHasAlt = false;
        for (auto& [bit, n] : sets[0].variants) {
            if (bit == OS::kBitBody && n == 1) { bodyHasAlt = true; }
        }
        CHECK(bodyHasAlt);
    }
    {  // small plugin with no usable stems -> one plugin-fallback set
        std::vector<DetectStyle> in = {
            mk("Cuirass", "Tiny.esp", 32),
            mk("Boots", "Tiny.esp", 37),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].name == "Tiny");  // named from the plugin
    }
    {  // large plugin with no usable stems -> residual dropped (no Frankenstein)
        std::vector<DetectStyle> in;
        for (int i = 0; i < 12; ++i) {
            in.push_back(mk("Cuirass", "Mega.esp", 32));  // 12 nameless bodies
            in.back().key.localFormID = 0x1000u + static_cast<std::uint32_t>(i);
        }
        in.push_back(mk("Boots", "Mega.esp", 37));
        CHECK(Detect(in, {}).empty());
    }
    {  // same stem in two armor types -> two sets, disambiguated names
        std::vector<DetectStyle> in = {
            mk("Elven Cuirass", "E.esp", 32, 0),  // light
            mk("Elven Boots", "E.esp", 37, 0),
            mk("Elven Cuirass", "E.esp", 32, 1),  // heavy
            mk("Elven Boots", "E.esp", 37, 1),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
        bool hasLight = false, hasHeavy = false;
        for (auto& s : sets) {
            if (s.name == "Elven (Light)") { hasLight = true; }
            if (s.name == "Elven (Heavy)") { hasHeavy = true; }
        }
        CHECK(hasLight);
        CHECK(hasHeavy);
    }
    {  // fuller sets rank ahead of thinner ones from the same plugin
        std::vector<DetectStyle> in = {
            mk("Thin Set Robe", "P.esp", 32),   // body + feet = coverage 2
            mk("Thin Set Boots", "P.esp", 37),
            mk("Full Kit Hood", "P.esp", 31),   // coverage 4
            mk("Full Kit Body", "P.esp", 32),
            mk("Full Kit Gloves", "P.esp", 33),
            mk("Full Kit Boots", "P.esp", 37),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
        CHECK(sets[0].coverage >= sets[1].coverage);
        CHECK(sets[0].name == "Full Kit");
    }
    {  // Obi's Abyss pattern (Top/Arms/Boots) clusters into one complete set
        std::vector<DetectStyle> in = {
            mk("Abyss Top [OBI]", "Obi's Abyss Armor.esp", 32),
            mk("Abyss Arms [OBI]", "Obi's Abyss Armor.esp", 33),
            mk("Abyss Boots [OBI]", "Obi's Abyss Armor.esp", 37),
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].name == "Abyss");
        CHECK(sets[0].coverage == 3);  // body + hands + feet
    }
    {  // Matching weapon looks join a discovered armor preset by raw plugin
       // and normalized set name. Unmatched classes stay passthrough, so the
       // detector never invents a weapon link for a preset that has none.
        std::vector<DetectStyle> armor = {
            mk("Abyss Top [OBI]", "Obi's Abyss Armor.esp", 32),
            mk("Abyss Boots [OBI]", "Obi's Abyss Armor.esp", 37),
            mk("Abyss Shield [OBI]", "Obi's Abyss Armor.esp", 39),
        };
        auto sets = Detect(armor, {});
        CHECK(sets.size() == 1);

        std::vector<DetectWeapon> weapons = {
            { "Abyss Sword [OBI]", "Obi's Abyss Armor.esp", "", OS::WeaponClass::Sword,
              { "Obi's Abyss Armor.esp", 0x901 } },
            { "Unrelated Bow", "Obi's Abyss Armor.esp", "", OS::WeaponClass::Bow,
              { "Obi's Abyss Armor.esp", 0x902 } },
            { "Abyss Dagger", "Different.esp", "", OS::WeaponClass::Dagger,
              { "Different.esp", 0x903 } },
        };
        LinkWeapons(sets, weapons);

        const auto& shield = sets[0].outfit.EntryFor(OS::BitForEditorSlot(39));
        CHECK(shield.kind == OS::SlotEntry::Kind::kStyle);
        CHECK(sets[0].outfit.WeaponEntryFor(OS::WeaponClass::Sword).kind ==
              OS::SlotEntry::Kind::kStyle);
        CHECK(sets[0].outfit.WeaponEntryFor(OS::WeaponClass::Sword).style.localFormID ==
              0x901);
        CHECK(sets[0].outfit.WeaponEntryFor(OS::WeaponClass::Bow).kind ==
              OS::SlotEntry::Kind::kPassthrough);
        CHECK(sets[0].outfit.WeaponEntryFor(OS::WeaponClass::Dagger).kind ==
              OS::SlotEntry::Kind::kPassthrough);
    }
    {  // prefix-merge: an unrecognized-slot piece attaches to its set (Gladiator)
        std::vector<DetectStyle> in = {
            mk("Gladiator Cuirass", "Glad.esp", 32),
            mk("Gladiator Boots", "Glad.esp", 37),
            mk("Gladiator Codpiece", "Glad.esp", 49),  // "codpiece" not a slot word
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);  // codpiece merged in, not orphaned into its own
        CHECK(sets[0].name == "Gladiator");
        CHECK(sets[0].outfit.EntryFor(OS::BitForEditorSlot(49)).kind ==
              OS::SlotEntry::Kind::kStyle);  // the extra (pelvis) slot is included
    }
    {  // prefix-merge does NOT fuse two distinct standalone sets
        std::vector<DetectStyle> in = {
            mk("Iron Cuirass", "V.esp", 32), mk("Iron Boots", "V.esp", 37),
            mk("Iron Dragon Cuirass", "V.esp", 32),  // "iron dragon" stands alone
            mk("Iron Dragon Boots", "V.esp", 37),
        };
        CHECK(Detect(in, {}).size() == 2);  // both qualify -> kept separate
    }
    {  // dedup: a set whose body piece is already owned/authored is dropped
        auto                     body = mk("Owned Robe", "D.esp", 32);
        std::vector<DetectStyle> in   = { body, mk("Owned Boots", "D.esp", 37) };
        Options                  opts;
        opts.excludeBodyKeys.push_back(body.key);
        CHECK(Detect(in, opts).empty());
    }
    {  // a FITTING piece is preferred over an unfit one for the same slot
        auto body      = mk("Nice Cuirass", "X.esp", 32);
        auto bootsUnfit = mk("Nice Boots", "X.esp", 37);
        bootsUnfit.fits = false;
        auto bootsFit  = mk("Nice Red Boots", "X.esp", 37);  // same "nice" stem
        bootsFit.fits  = true;
        bootsFit.key.localFormID = 0x999;
        auto sets = Detect({ body, bootsUnfit, bootsFit }, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).style.localFormID == 0x999);
        CHECK(sets[0].fullyFits);  // body fits (default) + the chosen feet rep fits
    }
    {  // a set with an unfit representative is marked not-fully-fitting
        auto body      = mk("Cape Cuirass", "Y.esp", 32);
        body.fits      = false;
        std::vector<DetectStyle> in = { body, mk("Cape Boots", "Y.esp", 37) };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(!sets[0].fullyFits);
    }
    {  // within a plugin, fully-fitting sets rank before partially-unfit ones
        auto a1 = mk("Alpha Cuirass", "Z.esp", 32);
        auto a2 = mk("Alpha Boots", "Z.esp", 37);
        a1.fits = false;  // Alpha is unfit but sorts first by name
        a2.fits = false;
        std::vector<DetectStyle> in = { a1, a2, mk("Beta Cuirass", "Z.esp", 32),
                                        mk("Beta Boots", "Z.esp", 37) };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
        CHECK(sets[0].name == "Beta");   // fully-fitting wins over the alphabetical order
        CHECK(sets[0].fullyFits);
        CHECK(!sets[1].fullyFits);
    }

    if (g_failures == 0) {
        std::printf("all SetDetector tests passed\n");
    }
    return g_failures;
}
