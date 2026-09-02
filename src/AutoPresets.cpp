#include "AutoPresets.h"

#include "OutfitSession.h"
#include "PresetStore.h"
#include "SetDetector.h"
#include "Settings.h"
#include "SlotMask.h"
#include "StyleCatalog.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <unordered_map>

namespace OS {

    namespace {
        const std::set<std::string> kVanilla = {
            "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm",
            "dragonborn.esm",
        };
        std::string Lower(std::string a_s) {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return a_s;
        }

        // Case-insensitive substring test (for the [Debug] sDiagnosePlugin trace).
        bool ContainsCI(std::string_view a_hay, std::string_view a_needle) {
            if (a_needle.empty()) {
                return true;
            }
            const auto lower = [](char c) {
                return std::tolower(static_cast<unsigned char>(c));
            };
            const auto it = std::search(a_hay.begin(), a_hay.end(), a_needle.begin(),
                                        a_needle.end(),
                                        [&](char a, char b) { return lower(a) == lower(b); });
            return it != a_hay.end();
        }

        const char* FitName(FitReason a_r) {
            switch (a_r) {
                case FitReason::kFits:    return "fits";
                case FitReason::kNoRace:  return "noRace";
                case FitReason::kNoSex:   return "noSex";
                case FitReason::kCrashed: return "crashed";
                default:                  return "?";
            }
        }
    }

    AutoPresets& AutoPresets::GetSingleton() {
        static AutoPresets instance;
        return instance;
    }

    void AutoPresets::Generate() {
        const auto& items = StyleCatalog::GetSingleton().Items();
        // [Debug] sDiagnosePlugin (OS-45): trace matching styles through
        // detection - fit reason, name-stem, and final-set membership.
        const std::string& diag = Settings::GetSingleton().diagnosePlugin;

        // Skip only true hazards: crashers and wrong-gender gear (both crash the
        // preview). INCLUDE race-misfit gear (kNoRace, e.g. a UBE body whose gear
        // lacks a matching armature) - it renders invisible, not a crash, and is
        // flagged red in the browser. Without this a body-mod character sees
        // almost no modded sets (most gear is race-flagged may-not-fit).
        std::vector<SetDetector::DetectStyle> styles;
        std::vector<SetDetector::DetectWeapon> weapons;
        styles.reserve(items.size());
        weapons.reserve(items.size());
        for (const auto& it : items) {
            if (it.IsWeapon()) {
                // Weapons do not participate in armor clustering. They are
                // linked afterward only when their raw plugin and normalized
                // set identity match a coherent detected armor set.
                if (it.fitReason != FitReason::kCrashed) {
                    weapons.push_back({ it.name, it.source, it.edid, *it.weaponClass,
                                        it.key });
                }
                continue;
            }
            if (it.fitReason == FitReason::kCrashed || it.fitReason == FitReason::kNoSex) {
                continue;
            }
            SetDetector::DetectStyle s;
            s.name       = it.name;
            s.source     = it.source;
            s.edid       = it.edid;
            s.slotMask   = it.slotMask;
            s.primaryBit = it.primaryBit;
            s.armorType  = it.armorType;
            s.fits       = (it.fitReason == FitReason::kFits);
            s.key        = it.key;
            styles.push_back(std::move(s));
        }

        // Diagnostic pass 1: every matching catalog item, its fit reason (kNoSex
        // is EXCLUDED here - hypothesis H1), the stem it will cluster on (name,
        // then EDID fallback - differing stems = H4), and its armor type (a stem
        // that spans types splits the cluster = H5). Compare the three Abyss
        // pieces' lines side by side and the cause is one glance away.
        if (!diag.empty()) {
            for (const auto& it : items) {
                if (!(ContainsCI(it.name, diag) || ContainsCI(it.source, diag))) {
                    continue;
                }
                const bool included = !it.IsWeapon() && it.fitReason != FitReason::kCrashed &&
                                      it.fitReason != FitReason::kNoSex;
                spdlog::info("[diag/detect] '{}' edid='{}' slot={} type={} fit={} "
                             "stem(name)='{}' stem(edid)='{}' -> {}",
                             it.name, it.edid, it.primaryBit + 30u, it.armorType,
                             FitName(it.fitReason), SetDetector::NameStem(it.name),
                             SetDetector::NameStem(it.edid),
                             included ? "INCLUDED in detection"
                                      : (it.IsWeapon() ? "WEAPON LINK CANDIDATE"
                                                       : "EXCLUDED (crash-risk fit reason)"));
            }
        }

        // Dedup keys: body pieces already shipped as AUTHORED presets (so the
        // same set doesn't appear in both the Curated and Discovered tabs). We
        // deliberately do NOT dedup against the player's OWNED outfits anymore -
        // the user wants a Discovered set to keep showing even after they've
        // saved their own version of it (OS-45 follow-up), e.g. an "Abyss"
        // outfit no longer hides the auto-detected Obi's Abyss set.
        SetDetector::Options opts;
        for (const auto& p : PresetStore::GetSingleton().Snapshot()) {
            const auto& e = p.outfit.EntryFor(kBitBody);
            if (e.kind == SlotEntry::Kind::kStyle) {
                opts.excludeBodyKeys.push_back(e.style);
            }
        }

        // The game's own OUTFIT records, flattened to catalog keys. See
        // SetDetector::DetectOutfit for why no naming rule can replace this and
        // CompleteFromOutfits for why it fills sets rather than making them.
        //
        // ⚠ AN OUTFIT NAMES A RECORD; THE CATALOG SHOWS A LOOK. StyleCatalog
        // collapses enchanted variants onto the lowest-FormID representative
        // (see StyleItem::variantIds), and an outfit record routinely names one
        // of the collapsed ones, so a map keyed on the representative alone
        // would miss them. Every id of the look points at the same key here.
        std::unordered_map<RE::FormID, StyleRefKey> keyByForm;
        for (const auto& it : items) {
            if (it.IsWeapon() || !it.form) {
                continue;
            }
            keyByForm.emplace(it.form->GetFormID(), it.key);
            for (const RE::FormID variant : it.variantIds) {
                keyByForm.emplace(variant, it.key);
            }
        }
        std::vector<SetDetector::DetectOutfit> outfits;
        std::size_t                            outfitRecords = 0;
        if (auto* const handler = RE::TESDataHandler::GetSingleton()) {
            const auto& records = handler->GetFormArray<RE::BGSOutfit>();
            outfitRecords        = records.size();
            outfits.reserve(records.size());
            for (auto* const record : records) {
                if (!record) {
                    continue;
                }
                SetDetector::DetectOutfit flat;
                const auto take = [&](RE::TESForm* a_form) {
                    if (!a_form) {
                        return;
                    }
                    if (const auto it = keyByForm.find(a_form->GetFormID());
                        it != keyByForm.end()) {
                        flat.pieces.push_back(it->second);
                    }
                };
                for (auto* const item : record->outfitItems) {
                    if (!item) {
                        continue;
                    }
                    // ⚠ ONE LEVEL OF LEVELED LIST AND NO RECURSION. Guard
                    // helmets are a LVLI rather than an ARMO, so a reader that
                    // stopped at the outfit's own entries would miss them; a
                    // reader that recursed would walk lists that nest into the
                    // hundreds for a boot this pass may not even use. numEntries
                    // is the count, because SimpleArray carries none.
                    if (auto* const list = item->As<RE::TESLevItem>()) {
                        for (std::uint32_t i = 0; i < list->numEntries; ++i) {
                            take(list->entries[i].form);
                        }
                        continue;
                    }
                    take(item);
                }
                if (flat.pieces.size() > 1) {
                    outfits.push_back(std::move(flat));
                }
            }
        }

        SetDetector::Stats stats;
        auto               sets = SetDetector::Detect(styles, opts, &stats);
        // ⚠ EVERY FILL IS NAMED, ONCE PER RESCAN AND NOT PER FRAME. Vanilla is
        // not what anybody runs: measured offline, all twelve hold guard sets
        // gain boots from the masters alone, so a field report of "some guard
        // armour still has none" can only be answered by the records THIS load
        // order resolved. Without these lines the answer is a single count, and
        // a count cannot say which set missed out or what won the slot.
        std::vector<SetDetector::Completion> fills;
        std::vector<SetDetector::Completion> declined;
        const std::size_t                    completed =
            SetDetector::CompleteFromOutfits(sets, outfits, styles, &fills, &declined);
        for (const auto& f : fills) {
            spdlog::debug("complete: '{}' += '{}' on slot {}", f.setName, f.pieceName,
                          f.bit + 30u);
        }
        // ⚠ THE DECLINED SLOTS ARE THE OTHER HALF OF THE ANSWER, and without
        // them a set with no boot cannot say whether the load order offered
        // none or offered four different ones. The named piece is whichever
        // candidate arrived first; it is the loser being reported, not a
        // choice. Field 2026-08-20 is the reason both rules exist: the old
        // first-come fill put a Dwarven helmet on the Iron set, and the
        // primary-bit test then put an Execution Hood on top of the Scaled
        // set's own helmet.
        for (const auto& c : declined) {
            if (c.reason == SetDetector::Completion::Reason::kOverlaps) {
                spdlog::debug("overlaps: '{}' refused '{}' on slot {}; it contests a "
                              "slot the set already wears",
                              c.setName, c.pieceName, c.bit + 30u);
            } else {
                spdlog::debug("contested: '{}' left slot {} empty; records disagree "
                              "(first was '{}')",
                              c.setName, c.bit + 30u, c.pieceName);
            }
        }
        SetDetector::LinkWeapons(sets, weapons);

        // Diagnostic pass 2: did each matching piece land in a final set? A
        // piece that was INCLUDED above but shows "NOT in any set" here means
        // its cluster failed to qualify (or was deduped) - cross-reference the
        // aggregate `deduped`/`qualified` counts in the summary line below.
        if (!diag.empty()) {
            for (const auto& it : items) {
                if (!(ContainsCI(it.name, diag) || ContainsCI(it.source, diag))) {
                    continue;
                }
                std::string where = "NOT in any discovered set";
                for (const auto& d : sets) {
                    bool found = false;
                    d.outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_k) {
                        if (a_k == it.key) {
                            found = true;
                        }
                    });
                    d.outfit.ForEachWeaponStyle([&](WeaponClass, const StyleRefKey& a_k) {
                        if (a_k == it.key) {
                            found = true;
                        }
                    });
                    if (found) {
                        where = "in set '" + d.name + "'";
                        break;
                    }
                }
                spdlog::info("[diag/detect] '{}' -> {}", it.name, where);
            }
        }

        std::vector<JsonCodec::Preset> built;
        built.reserve(sets.size());
        for (const auto& d : sets) {
            // Only surface sets that actually FIT the player's body (user
            // request): on a UBE body a 3BA-only set (no UBE armature) doesn't
            // render, so it's dropped; a set with UBE support (e.g. Obi's Abyss)
            // fits and shows. Symmetrically, UBE-exclusive sets are dropped on a
            // 3BA/vanilla body. `fullyFits` = every representative piece renders
            // (the shared race+sex fit check), so this covers both directions.
            if (!d.fullyFits) {
                continue;
            }
            JsonCodec::Preset p;
            p.name        = d.name;
            p.author      = d.source;  // clean plugin name = browser group header
            p.outfit      = d.outfit;
            p.outfit.name = d.name;
            p.file        = "discovered";  // shown as "(discovered)" - no extra parens
            // requires_: the non-vanilla plugin(s) the pieces come from.
            d.outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
                if (a_key.modName.empty() || kVanilla.contains(Lower(a_key.modName))) {
                    return;
                }
                if (std::ranges::find(p.requires_, a_key.modName) == p.requires_.end()) {
                    p.requires_.push_back(a_key.modName);
                }
            });
            d.outfit.ForEachWeaponStyle([&](WeaponClass, const StyleRefKey& a_key) {
                if (a_key.modName.empty() || kVanilla.contains(Lower(a_key.modName))) {
                    return;
                }
                if (std::ranges::find(p.requires_, a_key.modName) == p.requires_.end()) {
                    p.requires_.push_back(a_key.modName);
                }
            });
            int alts = 0;
            for (const auto& [bit, n] : d.variants) {
                alts += n;
            }
            if (alts > 0) {
                // "piece(s)" was fine while this only ever rendered in the
                // detail column's body text. That column is gone and this is
                // the line under a preset's title in its hover now, where a
                // parenthesised plural reads as unfinished copy. The count is
                // right here, so the word can be.
                // The alternates have their own browser rows now (user's
                // call, 2026-08-20), so this says where they are rather than
                // that they exist somewhere unnamed.
                p.description = std::to_string(alts) +
                                (alts == 1 ? " variant of this set has its own row"
                                           : " variants of this set have their own rows");
            }
            built.push_back(std::move(p));
        }

        // Diagnostics (computed before the move): how much of the yield is
        // modded vs vanilla-record armor. A set is "modded" when its pieces
        // reference a non-vanilla plugin (requires_ is non-empty). This answers
        // "why do I only see vanilla sets?" directly from the log.
        std::set<std::string> setSrc, moddedSetSrc;
        std::size_t           moddedSets = 0;
        for (const auto& p : built) {
            setSrc.insert(p.author);
            if (!p.requires_.empty()) {
                moddedSetSrc.insert(p.author);
                ++moddedSets;
            }
        }
        std::set<std::string> moddedFitSrc;
        std::size_t           moddedFitting = 0;
        for (const auto& s : styles) {
            if (!kVanilla.contains(Lower(s.source))) {
                moddedFitSrc.insert(s.source);
                ++moddedFitting;
            }
        }
        const std::size_t total       = built.size();
        // Sets that qualified but were dropped because they don't fit the
        // player's body (the new fullyFits filter) - the count answers "why so
        // few sets on my UBE character?".
        const std::size_t droppedUnfit =
            sets.size() > built.size() ? sets.size() - built.size() : 0;

        {
            std::scoped_lock l(lock_);
            presets_ = std::move(built);
        }
        spdlog::info("AutoPresets: {} fitting set(s) shown ({} modded) from {} "
                     "plugin(s) ({} modded); {} dropped as body-unfit; {} candidate "
                     "styles ({} modded across {} plugin(s)); Detect: {} clusters, {} "
                     "residual ({} dropped), {} qualified, {} deduped, {} variant "
                     "row(s) ({} over the per-set cap); outfit records: "
                     "{} read, {} usable, {} set(s) completed.",
                     total, moddedSets, setSrc.size(), moddedSetSrc.size(), droppedUnfit,
                     styles.size(), moddedFitting, moddedFitSrc.size(), stats.clusters,
                     stats.residualClusters, stats.residualDropped, stats.qualified,
                     stats.deduped, stats.variantRows, stats.variantsDropped,
                     outfitRecords, outfits.size(), completed);
    }

    std::vector<JsonCodec::Preset> AutoPresets::Snapshot() const {
        std::scoped_lock l(lock_);
        return presets_;
    }

    std::size_t AutoPresets::Count() const {
        std::scoped_lock l(lock_);
        return presets_.size();
    }

    void AutoPresets::RequestRescan() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { GetSingleton().Generate(); });
        }
    }

}  // namespace OS
