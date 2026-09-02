#pragma once

// Composing one preview scene out of a saved outfit's pieces (OS-193, preview
// grid phase 4). A saved outfit is a multi-piece scene: every piece's models
// merged into one, rendered once.
//
// Pure, for PreviewGrid.h's reason: no test compiles the engine side, so every
// decidable rule lives in a header and is pinned by test. The engine half is a
// lookup, which is exactly the part with nothing to decide.

#include "PreviewGrid.h"  // TextureSwapEntry (pure)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OS::PresetScene {

    // One piece's contribution, taken verbatim off the catalog item the fit
    // walk already resolved.
    //
    // ⚠ NOTHING HERE IS RESOLVED ON THE RENDER THREAD. StyleItem::modelPaths is
    // filled during RefreshFitFor on the main thread precisely because the
    // answer depends on the target's race and sex; composing a preset is a
    // concatenation of those cached answers and must stay one. Calling
    // CollectArmourPaths from a draw would put the form-graph walk back on the
    // render thread, which is the thing phase 2 moved off it.
    struct Piece {
        std::vector<std::string>                                modelPaths;
        std::vector<std::vector<PreviewGrid::TextureSwapEntry>> swaps;
        std::uint32_t                                           slotMask{ 0 };
    };

    struct Composed {
        std::vector<std::string>                                modelPaths;
        std::vector<std::vector<PreviewGrid::TextureSwapEntry>> swaps;
        std::uint32_t                                           slotMask{ 0 };
        // Pieces that contributed at least one model, and pieces the outfit
        // named that resolved to nothing.
        //
        // ⚠ BOTH COUNTED, because a preset whose pieces are half missing must
        // not look like a preset that is simply small. The card draws what
        // loaded, the partial-load rule phase 2 already follows, and the
        // caller uses missing to say so rather than silently showing three
        // pieces of a nine-piece outfit as if that were the outfit.
        std::size_t resolved{ 0 };
        std::size_t missing{ 0 };
    };

    [[nodiscard]] inline Composed Compose(const std::vector<Piece>& a_pieces) {
        Composed out;
        // Whether ANY piece carried a swap. Decided before the swaps vector is
        // built, because of the rule below.
        bool anySwaps = false;
        for (const auto& piece : a_pieces) {
            for (const auto& list : piece.swaps) {
                if (!list.empty()) {
                    anySwaps = true;
                    break;
                }
            }
            if (anySwaps) {
                break;
            }
        }

        for (const auto& piece : a_pieces) {
            if (piece.modelPaths.empty()) {
                ++out.missing;
                continue;
            }
            ++out.resolved;
            out.slotMask |= piece.slotMask;
            for (std::size_t i = 0; i < piece.modelPaths.size(); ++i) {
                out.modelPaths.push_back(piece.modelPaths[i]);
                if (!anySwaps) {
                    continue;
                }
                // ⚠⚠ PADDED TO STAY PARALLEL WITH modelPaths. A piece with no
                // swaps sitting between two that have them must still occupy
                // its index, or every swap after it is applied to the wrong
                // NIF. The key fold and the extractor both index swaps BY
                // PATH POSITION, so a short vector is not a smaller answer, it
                // is a wrong one.
                out.swaps.push_back(i < piece.swaps.size()
                                        ? piece.swaps[i]
                                        : std::vector<PreviewGrid::TextureSwapEntry>{});
            }
        }
        // ⚠⚠ EMPTY WHEN NOTHING CARRIES A SWAP, never a vector of empty
        // vectors. DiskKeyFor reads the vector's SIZE to decide whether a path
        // grows its "@..." suffix, so a padded-but-empty list would still fold
        // to the same bytes today and is one edit away from not doing. The
        // swap-less key being byte-identical is a pinned rule from phase 1 and
        // it is what stops a cache orphaning; keeping the empty case truly
        // empty is how that rule stays cheap to hold.
        return out;
    }

    // What a composed set of preset scenes DEPENDS ON.
    //
    // ⚠⚠ THE INPUTS, NOT THE SUBJECT. This project has now been bitten three
    // times by a cache keyed on what identifies a thing rather than on what
    // its answer is computed from: the armour fit cache, the head-part offer
    // lists and the Restore capture. A preset's scene is built from the
    // catalog's per-item modelPaths, and those are re-resolved whenever the
    // target's RACE or SEX changes, without any preset changing at all. Keying
    // on the preset list alone would mean a follower with a different race
    // shows the player's meshes forever.
    //
    // The showcase SOURCE is here because the three tabs are three different
    // lists that can hold the same names.
    struct CacheKey {
        int           source{ -1 };
        std::uint32_t raceFormId{ 0 };
        int           sexIdx{ -1 };
        // The preset names in snapshot order. Order matters: the composed
        // vector is indexed positionally by the draw.
        std::vector<std::string> names;

        friend bool operator==(const CacheKey&, const CacheKey&) = default;
    };

}  // namespace OS::PresetScene
