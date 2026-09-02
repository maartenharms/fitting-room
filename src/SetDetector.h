#pragma once

#include "Outfit.h"  // StyleRefKey, Outfit - pure, no engine types

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OS::SetDetector {

    // A StyleCatalog item flattened to plain data so the algorithm stays
    // engine-free and unit-testable.
    struct DetectStyle {
        std::string   name;
        std::string   source;    // defining plugin filename
        std::string   edid;      // best-effort; empty on runtimes without EDID retention
        std::uint32_t slotMask{ 0 };
        std::uint32_t primaryBit{ 0 };  // the one slot it lists under
        std::uint8_t  armorType{ 0 };   // 0 light, 1 heavy, 2 clothing
        bool          fits{ true };     // renders on the player (else flagged, still usable)
        StyleRefKey   key;
    };

    // A weapon/ammo catalog entry flattened to engine-free data. Weapons do
    // not participate in armor clustering. Once a coherent armor set exists,
    // LinkWeapons attaches only same-plugin, same-stem looks to it.
    struct DetectWeapon {
        std::string   name;
        std::string   source;
        std::string   edid;
        WeaponClass   weaponClass{ WeaponClass::Sword };
        StyleRefKey   key;
    };

    // One of the game's own OUTFIT records (OTFT), flattened to the catalog
    // keys of the armour it names. Leveled lists are expanded by the caller,
    // and anything the catalog dropped is simply absent.
    //
    // ⚠⚠ THIS EXISTS BECAUSE NO NAMING RULE COULD EVER HAVE FOUND THESE
    // PIECES, and that was MEASURED against Skyrim.esm rather than argued.
    // The field report was "many discovered presets have no boots or gloves,
    // guard armour for example". The guard outfit record is
    // ArmorGuardCuirassWhiterun plus ArmorStormcloakBoots: the boots are the
    // STORMCLOAK set's boots, sharing not one word with the cuirass, and
    // loosening the stem rule far enough to join them would join the whole
    // Stormcloak set to it as well. Across the five masters, 495 outfit
    // records are body-anchored with two or more major slots, and 258 of them
    // carry FEET the stem rule cannot reach, 106 HANDS. The College robes are
    // paired with a record named simply "Boots", whose stem is EMPTY.
    //
    // So the authoring the clusterer is guessing at already exists in the
    // load order, and this is reading it instead of guessing better.
    // ⚠ NO PLUGIN FIELD, DELIBERATELY. The match is the body piece's identity
    // and nothing else: a mod that ships an outfit record pairing its own
    // cuirass with vanilla boots is describing a real set, and a same-plugin
    // rule would refuse exactly that.
    struct DetectOutfit {
        std::vector<StyleRefKey> pieces;  // catalog keys; order is the record's
    };

    struct Options {
        // A small plugin whose pieces have no usable name/EDID stem is treated
        // as one outfit; a large one is dropped (anti-Frankenstein, §4.3).
        std::size_t              maxResidualPieces{ 8 };
        // Body-slot keys already shipped as authored presets or owned by the
        // player - sets whose body piece matches one are dropped (§4.7).
        std::vector<StyleRefKey> excludeBodyKeys;
        // How many VARIANT rows a set may add beyond its base (user's call,
        // 2026-08-20: "we also want to work on showing variants of armors if
        // they exist", one browser row per variant).
        //
        // ⚠ ONE SLOT AT A TIME, NOT EVERY COMBINATION, and the difference is
        // the whole browser. Callisto ships four bodies, so the product would
        // still be four rows; a set with four bodies and three boots is twelve
        // rows of a product and seven of a sum. A preset is a starting point
        // and every slot is editable after it is applied, so the sum reaches
        // anything worth reaching.
        //
        // ⚠ AND THE CAP IS NOT SILENT. A pack with forty recolours would bury
        // its own plugin group; Detect reports what it dropped.
        std::size_t              maxVariantRows{ 8 };
    };

    struct DetectedSet {
        std::string name;    // human preset name
        std::string source;  // clean plugin name (browser group header)
        std::string sourcePlugin;  // raw filename, used for exact weapon matching
        std::string stem;          // normalized set identity, used for weapon matching
        Outfit      outfit;
        bool        fullyFits{ true };  // every representative renders on the player
        int         coverage{ 0 };  // major slots filled (head/body/hands/feet)
        // (primaryBit, count of unused alternates) per filled slot - the
        // "(+N variants)" hint.
        std::vector<std::pair<std::uint32_t, int>> variants;
    };

    // Optional diagnostics filled by Detect - answers "why so few sets?".
    struct Stats {
        int clusters{ 0 };          // total clusters formed (named + residual)
        int residualClusters{ 0 };  // of those, empty-stem (nameless) clusters
        int residualDropped{ 0 };   // residual clusters skipped (plugin too big)
        int qualified{ 0 };         // clusters that produced a set (pre-dedup)
        int deduped{ 0 };           // sets dropped as already owned/authored
        int variantRows{ 0 };       // rows added for an alternate piece
        int variantsDropped{ 0 };   // alternates the per-set cap refused
    };

    // Cluster the styles into coherent single-plugin sets, sorted by
    // (clean plugin name, coverage desc, name). a_stats, if non-null, receives
    // per-run diagnostics.
    [[nodiscard]] std::vector<DetectedSet> Detect(const std::vector<DetectStyle>& a_styles,
                                                  const Options& a_opts,
                                                  Stats* a_stats = nullptr);

    // Fill the EMPTY slots of already-detected sets from the game's own outfit
    // records, and re-sort. a_styles supplies the slot/fit facts for the keys
    // a_outfits names; a key that is not in it is skipped.
    //
    // ⚠⚠ IT ADDS NO SETS AND REPLACES NO PIECE. That is the whole reason this
    // is a completion pass rather than a second discovery source. Detection
    // from outfit records would put those 495 vanilla records into the browser
    // as rows of their own, most of them things nobody wants to wear (five
    // College novice robes, a prisoner's cuffs, a court wizard); completing
    // what the clusterer already found adds a boot to the guard set and not
    // one row anywhere.
    //
    // ⚠⚠ THE ANTI-FRANKENSTEIN GUARD IS THE BODY PIECE'S IDENTITY PLUS
    // AGREEMENT, and the identity ALONE was not enough. This comment claimed
    // for months that naming the body meant "nothing can leak in from an
    // outfit that merely looks related", and the field disproved it on
    // 2026-08-20: a generic cuirass is named by dozens of unrelated NPC outfit
    // records, so the body key admitted all of them and the first one reached
    // won the slot. Iron wore a Dwarven helmet, Ebony an Iron one. A record
    // contributes only when it names that exact body piece AND every other
    // record naming it offers the same piece for that slot.
    // Options::maxResidualPieces guards the same failure on the naming side
    // and neither one covers the other.
    //
    // ⚠ ONLY FITTING PIECES ARE ADDED, and that is not tidiness. AutoPresets
    // drops a set whose every representative does not render, so filling a
    // slot with a piece that does not fit this body would DELETE a set that
    // was showing correctly a moment ago.
    // One slot this pass filled or refused, for the caller to log. Names rather
    // than keys, because the question being answered is a player's ("why has my
    // Riften guard no boots") and a mod/formid pair does not answer it.
    struct Completion {
        // ⚠ THE REFUSALS ARE NOT ONE THING. "Three records offered three
        // different boots" and "the piece offered would sit on top of the
        // helmet this set already wears" are different facts about the same
        // empty slot and they want different fixes, so the reason travels with
        // the report rather than being inferred from which vector it landed in.
        enum class Reason {
            kFilled,
            kContested,  // the records naming this body disagreed
            kOverlaps,   // the piece contests a slot the set already occupies
        };
        std::string   setName;
        std::string   pieceName;
        std::uint32_t bit{ 0 };
        Reason        reason{ Reason::kFilled };
    };

    // Returns how many sets gained a slot, which is the one number that says
    // whether this pass did anything on a given load order. A silent completion
    // pass would be indistinguishable from an absent one in the field log.
    //
    // ⚠ a_log IS HOW THE FIELD ANSWERS THIS, AND IT HAS TO BE, because vanilla
    // is not what anybody runs. Measured offline against the five masters, all
    // twelve hold guard sets gain boots here (ten Fur, Solitude Imperial), so a
    // report of "some guard armour still has none" is a fact about that load
    // order's own outfit records rather than about this rule, and only that
    // install can say which record won.
    //
    // ⚠ REPORTED RATHER THAN LOGGED HERE. This file compiles into the pure test
    // executables and names no engine or config type, the same discipline
    // RuleModel.h and SlotMask.h keep.
    // a_declined collects the slots this pass refused, each carrying its
    // reason. Same shape as a_log and reported for the same reason: "no record
    // offered a boot" and "three records offered three different boots" look
    // identical in a set with no boot and want opposite fixes.
    //
    // ⚠⚠ A PIECE MUST NOT CONTEST A SLOT THE SET ALREADY OCCUPIES, and testing
    // only the primary bit is what let it. Measured 2026-08-20 against
    // Skyrim.esm and the field log together: 'Scaled' wears `ArmorScaledHelmet`
    // (slots 31 and 42) and the pass added `ExecutionHood` (30, 31, 42, 43)
    // because slot 30 was free; 'Ancient Nord' took `ArmorSteelPlateHelmet`
    // (30, 31, 42, 43) over its own draugr helmet (31, 42); 'Hide' took a
    // chitin helmet the same way. The engine cannot even equip those pairs
    // together, and the card draws both, which is the clipping the field
    // reported. So the whole slot MASK is tested against the set's occupied
    // mask, which grows as the pass fills.
    //
    // ⚠⚠ AND JEWELLERY IS NEVER COMPLETED. An amulet or a ring in an outfit
    // record is what that NPC happens to be wearing, never set membership.
    // Measured over the five masters: every single-record agreement on slot 35
    // or 36 is a personal effect or a quest item (Amulet of Dibella,
    // Ogmund's Amulet of Talos, Nightweaver's Band, the Thieves Guild
    // amulet), 18 amulets and 4 rings in total, and not one of them belongs to
    // the armour it was about to be attached to.
    std::size_t CompleteFromOutfits(std::vector<DetectedSet>& a_sets,
                                    const std::vector<DetectOutfit>& a_outfits,
                                    const std::vector<DetectStyle>& a_styles,
                                    std::vector<Completion>* a_log = nullptr,
                                    std::vector<Completion>* a_declined = nullptr);

    // Attach weapon/ammo styles whose raw plugin and normalized name stem both
    // match a detected armor set. Every unmatched class remains passthrough.
    void LinkWeapons(std::vector<DetectedSet>& a_sets,
                     const std::vector<DetectWeapon>& a_weapons);

    // The set identity: display name minus slot-nouns and variant tokens.
    // Exposed for unit tests.
    [[nodiscard]] std::string NameStem(std::string_view a_displayName);

    // A plugin filename turned into a readable label: no extension/version,
    // camelCase and separators split, title-cased. Exposed for unit tests.
    [[nodiscard]] std::string CleanPluginName(std::string_view a_source);

}  // namespace OS::SetDetector
