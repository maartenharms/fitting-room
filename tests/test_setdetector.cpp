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
    {  // Colour variants of one dress collapse onto a base row, and each
       // alternate earns a row of its own (user's call, 2026-08-20). The label
       // is what the alternate's name has and the base's does not.
        std::vector<DetectStyle> in = {
            mk("Elaborate Textiles - Emerald Dress", "ET.esp", 32),
            mk("Elaborate Textiles - Ruby Dress", "ET.esp", 32),
            mk("Elaborate Textiles - Emerald Shoes", "ET.esp", 37),
        };
        in[1].key.localFormID = 0x801;
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
        CHECK(sets[0].name == "Elaborate Textiles");
        bool bodyHasAlt = false;
        for (auto& [bit, n] : sets[0].variants) {
            if (bit == OS::kBitBody && n == 1) { bodyHasAlt = true; }
        }
        CHECK(bodyHasAlt);
        CHECK(sets[1].name == "Elaborate Textiles - Ruby");
        CHECK(sets[1].outfit.EntryFor(OS::kBitBody).style == in[1].key);
        // ⚠ THE REST OF THE SET RIDES ALONG. A variant row is the same outfit
        // with ONE slot swapped, not a row holding a single piece.
        CHECK(sets[1].outfit.EntryFor(OS::kBitFeet).style == in[2].key);
        CHECK(sets[1].variants.empty());  // the count belongs to the base row
    }
    {  // The per-set cap is honoured and what it dropped is counted.
        std::vector<DetectStyle> in = { mk("Recolour Cuirass", "R.esp", 32),
                                        mk("Recolour Boots", "R.esp", 37) };
        for (int i = 0; i < 10; ++i) {
            in.push_back(mk("Recolour Cuirass", "R.esp", 32));
            in.back().name += std::string(" mk") + static_cast<char>('2' + i);
            in.back().key.localFormID = 0x900u + static_cast<std::uint32_t>(i);
        }
        Options opts;
        opts.maxVariantRows = 3;
        Stats stats;
        auto  sets = Detect(in, opts, &stats);
        CHECK(sets.size() == 4);  // the base plus three
        CHECK(stats.variantRows == 3);
        CHECK(stats.variantsDropped == 7);
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
        CHECK(sets.size() == 2);  // the base plus the unfit boot's own row
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).style.localFormID == 0x999);
        CHECK(sets[0].fullyFits);  // body fits (default) + the chosen feet rep fits
        // ⚠ AND THE VARIANT ROW CARRIES ITS OWN FIT VERDICT rather than the
        // base's. AutoPresets drops a set that does not render, so a row built
        // on an unfit piece has to say so or it reaches the browser and draws
        // nothing.
        CHECK(!sets[1].fullyFits);
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

    // ---- CompleteFromOutfits: the guard armour report --------------------
    // The measured shape, from Skyrim.esm: the guard outfit record names
    // ArmorGuardCuirassWhiterun and ArmorStormcloakBoots, whose names share no
    // word at all, so the clusterer correctly files the boots under Stormcloak
    // and the guard set has no feet.
    {
        auto guardBody  = mk("Whiterun Guard's Armor", "Skyrim.esm", 32);
        auto guardHelm  = mk("Whiterun Guard's Helmet", "Skyrim.esm", 31);
        auto scBody     = mk("Stormcloak Cuirass", "Skyrim.esm", 32);
        auto scBoots    = mk("Stormcloak Boots", "Skyrim.esm", 37);
        scBody.key.localFormID  = 0x101;  // distinct from the guard body
        scBoots.key.localFormID = 0x102;
        const std::vector<DetectStyle> in = { guardBody, guardHelm, scBody, scBoots };

        auto sets = Detect(in, {});
        const auto guardOf = [](std::vector<DetectedSet>& a_sets) -> DetectedSet* {
            for (auto& s : a_sets) {
                if (s.name == "Whiterun Guard") { return &s; }
            }
            return nullptr;
        };
        DetectedSet* guard = guardOf(sets);
        CHECK(guard != nullptr);
        CHECK(guard->outfit.EntryFor(OS::kBitFeet).kind ==
              OS::SlotEntry::Kind::kPassthrough);  // the reported bug

        const std::vector<DetectOutfit> outfits = {
            { { guardBody.key, scBoots.key } },
        };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 1);
        guard = guardOf(sets);
        CHECK(guard != nullptr);
        CHECK(guard->outfit.EntryFor(OS::kBitFeet).style == scBoots.key);
        CHECK(guard->coverage == 3);  // head + body + feet, re-counted

        // ⚠ AND THE STORMCLOAK SET STILL HAS ITS OWN BOOTS. Completing one set
        // reads the other's pieces; it must not move them.
        for (const auto& s : sets) {
            if (s.name == "Stormcloak") {
                CHECK(s.outfit.EntryFor(OS::kBitFeet).style == scBoots.key);
            }
        }
    }
    {  // A slot the clusterer already chose is NEVER overwritten.
        auto body  = mk("Ebony Cuirass", "V.esp", 32);
        auto boots = mk("Ebony Boots", "V.esp", 37);
        auto other = mk("Fur Boots", "V.esp", 37);
        other.key.localFormID = 0x201;
        const std::vector<DetectStyle> in = { body, boots, other };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { body.key, other.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).style == boots.key);
    }
    {  // The BODY is never replaced, even by an outfit record naming another
       // one. The body piece is the identity this match was made on.
        auto body   = mk("Scaled Cuirass", "V.esp", 32);
        auto helm   = mk("Scaled Helmet", "V.esp", 31);
        auto other  = mk("Hide Cuirass", "V.esp", 32);
        other.key.localFormID = 0x301;
        const std::vector<DetectStyle> in = { body, helm, other };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { body.key, other.key } } };
        CompleteFromOutfits(sets, outfits, in);
        CHECK(sets[0].outfit.EntryFor(OS::kBitBody).style == body.key);
    }
    {  // An outfit record that does not name this set's body piece contributes
       // NOTHING. This is the anti-Frankenstein guard: without it, any record
       // could donate to any set.
        auto body    = mk("Elven Cuirass", "V.esp", 32);
        auto helm    = mk("Elven Helmet", "V.esp", 31);
        auto foreign = mk("Orcish Boots", "V.esp", 37);
        foreign.key.localFormID = 0x401;
        const std::vector<DetectStyle> in = { body, helm, foreign };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { foreign.key, helm.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).kind ==
              OS::SlotEntry::Kind::kPassthrough);
    }
    {  // ⚠ AN UNFIT PIECE IS NEVER ADDED. AutoPresets drops a set whose
       // representatives do not all render, so filling a slot with one would
       // delete a set that was showing correctly.
        auto body   = mk("Glass Cuirass", "V.esp", 32);
        auto helm   = mk("Glass Helmet", "V.esp", 31);
        auto boots  = mk("Dwarven Boots", "V.esp", 37);
        boots.fits  = false;
        boots.key.localFormID = 0x501;
        const std::vector<DetectStyle> in = { body, helm, boots };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].fullyFits);
        const std::vector<DetectOutfit> outfits = { { { body.key, boots.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        CHECK(sets[0].fullyFits);
    }
    {  // A key the catalog dropped is skipped rather than stored unresolved.
        auto body  = mk("Steel Cuirass", "V.esp", 32);
        auto helm  = mk("Steel Helmet", "V.esp", 31);
        const std::vector<DetectStyle> in = { body, helm };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const OS::StyleRefKey ghost{ "V.esp", 0x6001 };  // never in a_styles
        const std::vector<DetectOutfit> outfits = { { { body.key, ghost } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
    }
    {  // No records, or no sets, is a no-op rather than a special case.
        std::vector<DetectedSet> none;
        CHECK(CompleteFromOutfits(none, { { {} } }, {}) == 0);
        auto body = mk("Fur Cuirass", "V.esp", 32);
        auto helm = mk("Fur Helmet", "V.esp", 31);
        const std::vector<DetectStyle> in = { body, helm };
        auto sets = Detect(in, {});
        CHECK(CompleteFromOutfits(sets, {}, in) == 0);
        CHECK(sets.size() == 1);
    }

    // ---- CompleteFromOutfits: a contested slot is left empty ---------------
    // ⚠⚠ THE BODY'S IDENTITY IS NOT ENOUGH, and the header above claimed it
    // was. Measured in the field 2026-08-20 on a 235-plugin load order: the
    // Iron set gained a DWARVEN helmet, Ebony gained an IRON one, Stormcloak
    // gained an IMPERIAL OFFICER'S, and one Twisted Faith Mask reached a dozen
    // unrelated sets. A generic cuirass is named by dozens of NPC outfit
    // records that pair it with whatever that NPC happened to wear, and the
    // old rule let the FIRST record reached win the slot outright. Agreement
    // across the records that name this body is what separates "this set's
    // missing boot" from "one bandit's hat".
    {
        auto body    = mk("Iron Cuirass", "Skyrim.esm", 32);
        auto boots   = mk("Iron Boots", "Skyrim.esm", 37);
        auto dwarven = mk("Dwarven Helmet", "Skyrim.esm", 31);
        auto orcish  = mk("Orcish Helmet", "Skyrim.esm", 31);
        dwarven.key.localFormID = 0x601;
        orcish.key.localFormID  = 0x602;
        const std::vector<DetectStyle> in = { body, boots, dwarven, orcish };

        auto sets = Detect(in, {});
        const auto ironOf = [](std::vector<DetectedSet>& a_sets) -> DetectedSet* {
            for (auto& s : a_sets) {
                if (s.name == "Iron") { return &s; }
            }
            return nullptr;
        };
        CHECK(ironOf(sets) != nullptr);

        // Two records name this body and DISAGREE about slot 31.
        const std::vector<DetectOutfit> outfits = {
            { { body.key, dwarven.key } },
            { { body.key, orcish.key } },
        };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        DetectedSet* iron = ironOf(sets);
        CHECK(iron != nullptr);
        CHECK(iron->outfit.EntryFor(OS::kBitHair).kind ==
              OS::SlotEntry::Kind::kPassthrough);
    }
    {  // ⚠ AND AGREEMENT STILL FILLS. Every record that names this body offers
       // the same boot, which is the guard-armour shape this pass exists for.
        auto body  = mk("Whiterun Guard's Armor", "Skyrim.esm", 32);
        auto helm  = mk("Whiterun Guard's Helmet", "Skyrim.esm", 31);
        auto boots = mk("Stormcloak Boots", "Skyrim.esm", 37);
        boots.key.localFormID = 0x701;
        const std::vector<DetectStyle> in = { body, helm, boots };
        auto sets = Detect(in, {});
        const std::vector<DetectOutfit> outfits = {
            { { body.key, boots.key } },
            { { body.key, boots.key } },  // a second record, same pairing
        };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 1);
        for (const auto& s : sets) {
            if (s.name == "Whiterun Guard") {
                CHECK(s.outfit.EntryFor(OS::kBitFeet).style == boots.key);
            }
        }
    }
    {  // ⚠⚠ SLOT 52 IS NEVER FILLED, WHATEVER THE RECORDS AGREE ON. It is the
       // community's schlong slot: The New Gentleman and SOS both reach the
       // body through it, and this load order parks a TNG GenitalCover there.
       // A set that owns 52 displaces that cover the moment it is worn, which
       // is a HIMBO male standing in full armour with his genitals out (field
       // 2026-08-20). Mannequin.cpp already refuses to DRAW slot 52; refusing
       // to OWN it is the same rule one layer earlier.
        auto body = mk("Triss Cuirass", "Triss.esp", 32);
        auto helm = mk("Triss Helmet", "Triss.esp", 31);
        auto neck = mk("Triss Necklace", "Triss.esp", 52);
        const std::vector<DetectStyle> in = { body, helm, neck };
        auto sets = Detect(in, {});
        CHECK(!sets.empty());
        const std::vector<DetectOutfit> outfits = { { { body.key, neck.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        for (const auto& s : sets) {
            CHECK(s.outfit.EntryFor(OS::kBitGenitals).kind ==
                  OS::SlotEntry::Kind::kPassthrough);
        }
    }

    {  // ⚠⚠ A CIRCLET AUTHORED AS CLOTHING STILL BELONGS TO ITS HEAVY SET.
       // Field 2026-08-20, the user's Callisto card: the mod ships
       // _Fuse00_CircletOutfitCallisto on slot 42 as CLOTHING beside a heavy
       // body, gauntlets and boots, so the type in the cluster key split it
       // off, it could not stand alone, and the set was shown with no head
       // piece. The stray type also earned the set a "(Heavy)" suffix it had
       // nothing to be distinguished from.
        std::vector<DetectStyle> in = {
            mk("Callisto Armor", "Callisto.esp", 32, 1),
            mk("Callisto Gauntlets", "Callisto.esp", 33, 1),
            mk("Callisto Boots", "Callisto.esp", 37, 1),
            mk("Callisto Circlet", "Callisto.esp", 42, 2),  // clothing
        };
        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        CHECK(sets[0].name == "Callisto");  // no type suffix: nothing to split
        CHECK(sets[0].outfit.EntryFor(OS::BitForEditorSlot(42)).kind ==
              OS::SlotEntry::Kind::kStyle);
        CHECK(sets[0].coverage == 4);  // head now counts
    }
    {  // ...and a real clothing SET of the same stem is still its own set,
       // because only a cluster that cannot stand alone is ever merged.
        std::vector<DetectStyle> in = {
            mk("Sylvan Cuirass", "Syl.esp", 32, 1),
            mk("Sylvan Boots", "Syl.esp", 37, 1),
            mk("Sylvan Robes", "Syl.esp", 32, 2),
            mk("Sylvan Shoes", "Syl.esp", 37, 2),
        };
        in[2].key.localFormID = 0x601;
        in[3].key.localFormID = 0x602;
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
    }
    {  // With a light and a heavy set under one stem, an orphan of one of
       // those types joins its own rather than crossing.
        std::vector<DetectStyle> in = {
            mk("Elven Cuirass", "El.esp", 32, 0),
            mk("Elven Boots", "El.esp", 37, 0),
            mk("Elven Cuirass", "El.esp", 32, 1),
            mk("Elven Boots", "El.esp", 37, 1),
            mk("Elven Circlet", "El.esp", 42, 1),  // heavy orphan
        };
        in[2].key.localFormID = 0x701;
        in[3].key.localFormID = 0x702;
        auto sets = Detect(in, {});
        CHECK(sets.size() == 2);
        for (const auto& s : sets) {
            const bool hasCirclet = s.outfit.EntryFor(OS::BitForEditorSlot(42)).kind ==
                                    OS::SlotEntry::Kind::kStyle;
            CHECK(hasCirclet == (s.name == "Elven (Heavy)"));
        }
    }

    // ---- CompleteFromOutfits: a piece may not contest an occupied slot -----
    // ⚠⚠ THE FIELD REPORT OF 2026-08-20, MEASURED OFF Skyrim.esm AND REPRODUCED
    // HERE. 'Scaled' wears ArmorScaledHelmet, slots 31 and 42. An outfit record
    // pairs the scaled cuirass with ExecutionHood, slots 30, 31, 42 and 43.
    // Slot 30 was free, so the old rule filled it and the card drew two helmets
    // on one head. The engine cannot equip that pair at all.
    {
        auto body  = mk("Scaled Cuirass", "S.esp", 32);
        auto helm  = mk("Scaled Helmet", "S.esp", 31);
        helm.slotMask |= OS::MaskForEditorSlot(42);  // a real helmet takes both
        auto hood = mk("Execution Hood", "S.esp", 30);
        hood.slotMask |= OS::MaskForEditorSlot(31) | OS::MaskForEditorSlot(42) |
                         OS::MaskForEditorSlot(43);
        hood.key.localFormID = 0x401;
        const std::vector<DetectStyle> in = { body, helm, hood };

        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { body.key, hood.key } } };
        std::vector<Completion>         declined;
        CHECK(CompleteFromOutfits(sets, outfits, in, nullptr, &declined) == 0);
        CHECK(sets[0].outfit.EntryFor(OS::BitForEditorSlot(30)).kind ==
              OS::SlotEntry::Kind::kPassthrough);
        CHECK(sets[0].outfit.EntryFor(OS::BitForEditorSlot(31)).style == helm.key);
        CHECK(declined.size() == 1);
        if (declined.size() == 1) {
            CHECK(declined[0].reason == Completion::Reason::kOverlaps);
            CHECK(declined[0].pieceName == "Execution Hood");
        }
    }
    {  // The same rule with the overlap the other way round: the set's own
       // piece is the wide one (a guard's full helmet is 30/31/42/43) and the
       // record offers a narrow helmet for a slot inside it.
        auto body = mk("Reach Guard's Armor", "G.esp", 32);
        auto full = mk("Reach Guard's Helmet", "G.esp", 30);
        full.slotMask |= OS::MaskForEditorSlot(31) | OS::MaskForEditorSlot(42) |
                         OS::MaskForEditorSlot(43);
        auto hide = mk("Hide Helmet", "G.esp", 31);
        hide.slotMask |= OS::MaskForEditorSlot(42);
        hide.key.localFormID = 0x402;
        const std::vector<DetectStyle> in = { body, full, hide };

        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { body.key, hide.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        CHECK(sets[0].outfit.EntryFor(OS::BitForEditorSlot(31)).kind ==
              OS::SlotEntry::Kind::kPassthrough);
    }
    {  // A piece that overlaps NOTHING is still filled, so the rule refuses
       // exactly the collisions and nothing else.
        auto body  = mk("Reach Guard's Armor", "H.esp", 32);
        auto full  = mk("Reach Guard's Helmet", "H.esp", 30);
        full.slotMask |= OS::MaskForEditorSlot(31) | OS::MaskForEditorSlot(42);
        auto boots = mk("Fur Boots", "H.esp", 37);
        boots.key.localFormID = 0x403;
        const std::vector<DetectStyle> in = { body, full, boots };

        auto sets = Detect(in, {});
        CHECK(sets.size() == 1);
        const std::vector<DetectOutfit> outfits = { { { body.key, boots.key } } };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 1);
        CHECK(sets[0].outfit.EntryFor(OS::kBitFeet).style == boots.key);
    }
    {  // ⚠⚠ JEWELLERY IS NEVER COMPLETED. An amulet or ring in an outfit record
       // is that NPC's own, not the armour's. Field 2026-08-20: 'Hide' gained
       // the Amulet of Dibella and 'Scaled' gained Ogmund's Amulet of Talos and
       // Nightweaver's Band, each off one NPC record that nothing contradicted.
        auto body   = mk("Hide Cuirass", "J.esp", 32);
        auto helm   = mk("Hide Helmet", "J.esp", 31);
        auto amulet = mk("Amulet of Dibella", "J.esp", 35);
        auto ring   = mk("Nightweaver's Band", "J.esp", 36);
        amulet.key.localFormID = 0x501;
        ring.key.localFormID   = 0x502;
        const std::vector<DetectStyle> in = { body, helm, amulet, ring };

        auto sets = Detect(in, {});
        CHECK(!sets.empty());
        const std::vector<DetectOutfit> outfits = {
            { { body.key, amulet.key, ring.key } },
        };
        CHECK(CompleteFromOutfits(sets, outfits, in) == 0);
        for (const auto& s : sets) {
            CHECK(s.outfit.EntryFor(OS::kBitAmulet).kind ==
                  OS::SlotEntry::Kind::kPassthrough);
            CHECK(s.outfit.EntryFor(OS::kBitRing).kind ==
                  OS::SlotEntry::Kind::kPassthrough);
        }
    }

    if (g_failures == 0) {
        std::printf("all SetDetector tests passed\n");
    }
    return g_failures;
}
