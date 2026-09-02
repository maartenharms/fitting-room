// Preset scene composition tests (OS-193, preview grid phase 4). No engine:
// composing a saved outfit's pieces into one scene is concatenation with two
// rules that are easy to get wrong and invisible when you do.
#include "PresetScene.h"

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

using namespace OS;
using OS::PresetScene::Piece;

namespace {
    PreviewGrid::TextureSwapEntry Swap(const char* a_tex) {
        PreviewGrid::TextureSwapEntry e;
        e.geomIndex   = 0;
        e.texPaths[0] = a_tex;
        return e;
    }
}  // namespace

int main() {
    {  // the ordinary case: pieces merge into one scene and their slots union
        const auto out = PresetScene::Compose({
            Piece{ { "a_0.nif" }, {}, 0x1 },
            Piece{ { "b_0.nif", "b_1.nif" }, {}, 0x4 },
        });
        CHECK(out.modelPaths.size() == 3);
        CHECK(out.modelPaths[0] == "a_0.nif");
        CHECK(out.modelPaths[2] == "b_1.nif");
        CHECK(out.slotMask == 0x5);
        CHECK(out.resolved == 2);
        CHECK(out.missing == 0);
    }

    {  // ⚠⚠ THE SWAPS VECTOR IS EMPTY WHEN NOTHING CARRIES ONE, never a vector
       // of empty vectors. DiskKeyFor reads its SIZE to decide whether a path
       // grows its "@" suffix, and the swap-less key being byte-identical is a
       // pinned rule from phase 1: it is what stops the whole thumbnail cache
       // orphaning itself.
        const auto out = PresetScene::Compose({
            Piece{ { "a_0.nif" }, {}, 0x1 },
            Piece{ { "b_0.nif" }, {}, 0x4 },
        });
        CHECK(out.swaps.empty());

        // And the composed identity folds to the same key a scene built
        // without this feature would have.
        PreviewGrid::SceneIdentity id;
        id.plugin      = "preset";
        id.editorId    = "a look";
        id.modelPaths  = out.modelPaths;
        id.swaps       = out.swaps;
        PreviewGrid::SceneIdentity bare = id;
        bare.swaps.clear();
        CHECK(PreviewGrid::DiskKeyFor(id) == PreviewGrid::DiskKeyFor(bare));
    }

    {  // ⚠⚠ A PIECE WITH NO SWAPS BETWEEN TWO THAT HAVE THEM STILL OCCUPIES ITS
       // INDEX. Both the key fold and the extractor index swaps BY PATH
       // POSITION, so a short vector is not a smaller answer, it is a wrong
       // one: every swap after the gap lands on the wrong NIF.
        const auto out = PresetScene::Compose({
            Piece{ { "a_0.nif" }, { { Swap("red.dds") } }, 0x1 },
            Piece{ { "plain.nif" }, {}, 0x2 },
            Piece{ { "c_0.nif" }, { { Swap("blue.dds") } }, 0x4 },
        });
        CHECK(out.modelPaths.size() == 3);
        CHECK(out.swaps.size() == 3);
        CHECK(out.swaps[0].size() == 1);
        CHECK(out.swaps[0][0].texPaths[0] == "red.dds");
        CHECK(out.swaps[1].empty());   // the gap, held open
        CHECK(out.swaps[2].size() == 1);
        CHECK(out.swaps[2][0].texPaths[0] == "blue.dds");
    }

    {  // a multi-path piece whose swaps list is shorter than its paths pads
       // rather than reading off the end
        const auto out = PresetScene::Compose({
            Piece{ { "a_0.nif", "a_1.nif" }, { { Swap("red.dds") } }, 0x1 },
            Piece{ { "b_0.nif" }, { { Swap("blue.dds") } }, 0x2 },
        });
        CHECK(out.swaps.size() == 3);
        CHECK(out.swaps[0].size() == 1);
        CHECK(out.swaps[1].empty());
        CHECK(out.swaps[2][0].texPaths[0] == "blue.dds");
    }

    {  // ⚠ A PIECE THAT RESOLVED TO NOTHING IS COUNTED, not silently dropped.
       // A preset whose pieces are half missing must not read as a preset that
       // is simply small, which is the partial-load rule phase 2 already
       // follows for a scene's own NIFs.
        const auto out = PresetScene::Compose({
            Piece{ { "a_0.nif" }, {}, 0x1 },
            Piece{ {}, {}, 0x8 },   // named by the outfit, resolves to nothing
            Piece{ {}, {}, 0x10 },
        });
        CHECK(out.resolved == 1);
        CHECK(out.missing == 2);
        CHECK(out.modelPaths.size() == 1);
        // ⚠ A MISSING PIECE CONTRIBUTES NO SLOT BIT. The mask drives the pose
        // and the hands/feet gating, and a bit from a piece that rendered
        // nothing would gate on gear that is not in the picture.
        CHECK(out.slotMask == 0x1);
    }

    {  // an outfit that resolved to nothing at all composes to an empty scene,
       // which the caller draws as the cross rather than as a blank card
        const auto out = PresetScene::Compose({ Piece{ {}, {}, 0x1 } });
        CHECK(out.modelPaths.empty());
        CHECK(out.resolved == 0);
        CHECK(out.missing == 1);
    }

    {  // ⚠⚠ THE CACHE KEY IS THE INPUTS, NOT THE SUBJECT. A preset's scene is
       // built from the catalog's per-item modelPaths, and those are
       // re-resolved whenever the TARGET's race or sex changes with no preset
       // changing at all. Keying on the preset list alone means a follower of
       // another race shows the player's meshes forever, which is the mistake
       // this project has already made three times.
        PresetScene::CacheKey a;
        a.source     = 1;
        a.raceFormId = 0x13746;
        a.sexIdx     = 0;
        a.names      = { "Nightgown", "Travel" };

        auto sameThing = a;
        CHECK(sameThing == a);

        auto otherRace       = a;
        otherRace.raceFormId = 0x13747;
        CHECK(!(otherRace == a));

        auto otherSex   = a;
        otherSex.sexIdx = 1;
        CHECK(!(otherSex == a));

        // The three showcase tabs are three lists that can hold the same names.
        auto otherTab   = a;
        otherTab.source = 2;
        CHECK(!(otherTab == a));

        // Order matters: the composed vector is indexed positionally.
        auto reordered  = a;
        reordered.names = { "Travel", "Nightgown" };
        CHECK(!(reordered == a));

        auto added  = a;
        added.names = { "Nightgown", "Travel", "Third" };
        CHECK(!(added == a));
    }

    if (g_failures == 0) {
        std::printf("PresetSceneTests: all passed\n");
        return 0;
    }
    std::printf("PresetSceneTests: %d failure(s)\n", g_failures);
    return 1;
}
