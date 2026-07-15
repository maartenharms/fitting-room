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
        styles.reserve(items.size());
        for (const auto& it : items) {
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
                const bool included = it.fitReason != FitReason::kCrashed &&
                                      it.fitReason != FitReason::kNoSex;
                spdlog::info("[diag/detect] '{}' edid='{}' slot={} type={} fit={} "
                             "stem(name)='{}' stem(edid)='{}' -> {}",
                             it.name, it.edid, it.primaryBit + 30u, it.armorType,
                             FitName(it.fitReason), SetDetector::NameStem(it.name),
                             SetDetector::NameStem(it.edid),
                             included ? "INCLUDED in detection"
                                      : "EXCLUDED (crash-risk fit reason)");
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

        SetDetector::Stats stats;
        const auto         sets = SetDetector::Detect(styles, opts, &stats);

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
            int alts = 0;
            for (const auto& [bit, n] : d.variants) {
                alts += n;
            }
            if (alts > 0) {
                p.description = std::to_string(alts) + " variant piece(s) available.";
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
                     "residual ({} dropped), {} qualified, {} deduped.",
                     total, moddedSets, setSrc.size(), moddedSetSrc.size(), droppedUnfit,
                     styles.size(), moddedFitting, moddedFitSrc.size(), stats.clusters,
                     stats.residualClusters, stats.residualDropped, stats.qualified,
                     stats.deduped);
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
