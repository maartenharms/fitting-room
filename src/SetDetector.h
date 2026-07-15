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

    struct Options {
        // A small plugin whose pieces have no usable name/EDID stem is treated
        // as one outfit; a large one is dropped (anti-Frankenstein, §4.3).
        std::size_t              maxResidualPieces{ 8 };
        // Body-slot keys already shipped as authored presets or owned by the
        // player - sets whose body piece matches one are dropped (§4.7).
        std::vector<StyleRefKey> excludeBodyKeys;
    };

    struct DetectedSet {
        std::string name;    // human preset name
        std::string source;  // clean plugin name (browser group header)
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
    };

    // Cluster the styles into coherent single-plugin sets, sorted by
    // (clean plugin name, coverage desc, name). a_stats, if non-null, receives
    // per-run diagnostics.
    [[nodiscard]] std::vector<DetectedSet> Detect(const std::vector<DetectStyle>& a_styles,
                                                  const Options& a_opts,
                                                  Stats* a_stats = nullptr);

    // The set identity: display name minus slot-nouns and variant tokens.
    // Exposed for unit tests.
    [[nodiscard]] std::string NameStem(std::string_view a_displayName);

    // A plugin filename turned into a readable label: no extension/version,
    // camelCase and separators split, title-cased. Exposed for unit tests.
    [[nodiscard]] std::string CleanPluginName(std::string_view a_source);

}  // namespace OS::SetDetector
