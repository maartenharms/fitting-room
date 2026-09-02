#include "CoveredSets.h"
#include "PreviewFilter.h"
// Header-only and pure apart from one declaration this suite never calls, so
// the body-key arithmetic is pinned here with the rest of the scene rules.
#include "BodyMeshPath.h"
#include "BodyMorphData.h"
#include "MannequinPose.h"
#include "PreviewFraming.h"
#include "PreviewGrid.h"
#include "PreviewManifest.h"

#include <cmath>
#include <array>
#include <vector>
#include <cstring>
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
    using namespace OS;

    {  // ⚠⚠ THE SCROLLBAR STROBE (field 2026-08-21). The layout width has to be
       // the SAME in both scrollbar states, or the grid oscillates at one flip
       // per frame: cards grow with the pane, so a scrollbar appearing shrinks
       // the cards, which shortens the rows, which makes the content fit, which
       // removes the scrollbar.
       //
       // The two states of one pane: a window spanning x 0..700, 8 of padding
       // each side and a 12 bar, with the cursor at the content left edge (8).
       // Without a bar the content region is 684 wide; with one it is 672.
       // Both must lay out against 672.
       //
       // ⚠ a_toWindowEdge IS MEASURED FROM THE CURSOR, not from the window's
       // origin: 700 - 8 = 692, and it is the same in both states because the
       // scrollbar does not move the window's right edge. Getting that wrong is
       // what this block caught on its first run.
        {
            const float toEdge  = 692.0f;    // window right (700) minus cursor x (8)
            const float pad     = 8.0f;
            const float bar     = 12.0f;
            const float noBar   = PreviewGrid::StableAvail(684.0f, toEdge, pad, bar);
            const float withBar = PreviewGrid::StableAvail(672.0f, toEdge, pad, bar);
            CHECK(std::fabs(noBar - withBar) < 1e-3f);
            CHECK(std::fabs(noBar - 672.0f) < 1e-3f);  // 692 - 8 - 12

            // And the thing that actually strobed: same column count, same card
            // size, therefore the same row height in both states.
            CHECK(PreviewGrid::ColumnsFor(noBar, 160.0f, 6.0f) ==
                  PreviewGrid::ColumnsFor(withBar, 160.0f, 6.0f));
            CHECK(std::fabs(PreviewGrid::FittedSide(noBar, 160.0f, 6.0f, 20.0f) -
                            PreviewGrid::FittedSide(withBar, 160.0f, 6.0f, 20.0f)) < 1e-3f);
        }

        // ⚠ DEGRADES TO THE WIDTH IT WAS HANDED. A grid nested in a column or a
        // table cell can measure a window edge that means nothing here, and a
        // wrong layout is worse than a flicker, so a reserved width that is
        // negative, zero, or WIDER than what the caller was given is refused.
        CHECK(std::fabs(PreviewGrid::StableAvail(300.0f, 10.0f, 8.0f, 12.0f) - 300.0f) <
              1e-3f);                                                    // negative
        CHECK(std::fabs(PreviewGrid::StableAvail(300.0f, 20.0f, 8.0f, 12.0f) - 300.0f) <
              1e-3f);                                                    // exactly zero
        CHECK(std::fabs(PreviewGrid::StableAvail(300.0f, 900.0f, 8.0f, 12.0f) - 300.0f) <
              1e-3f);                                                    // wider than given
        // A pane with no scrollbar style at all is left alone by arithmetic
        // rather than by a special case.
        CHECK(std::fabs(PreviewGrid::StableAvail(300.0f, 308.0f, 8.0f, 0.0f) - 300.0f) <
              1e-3f);
    }

    {  // Columns: floor((avail+spacing)/(side+spacing)), never below one and
       // NO upper cap. The spec's cap of three predates the card-size
       // slider: shrinking the cards bought no columns, which read as the
       // slider doing nothing (field 2026-08-09). The click-target concern
       // the cap served lives in kCardScaleMin now.
        CHECK(PreviewGrid::ColumnsFor(100.0f, 90.0f, 6.0f) == 1);
        CHECK(PreviewGrid::ColumnsFor(200.0f, 90.0f, 6.0f) == 2);
        CHECK(PreviewGrid::ColumnsFor(300.0f, 90.0f, 6.0f) == 3);
        // ⚠⚠ THE ROW IS FILLED, because ColumnsFor floors and whatever does
        // not divide evenly was simply left blank. The Bodies library is a
        // sidebar about 400 wide and its card was about 215: one column and
        // nearly half the width dead (field 2026-08-10).
        {
            // Two 90s and a 6 gap fit in 300, so the pair grows to eat the
            // 114 that ColumnsFor would have thrown away.
            const float two = PreviewGrid::FittedSide(300.0f, 140.0f, 6.0f, 10.0f);
            CHECK(PreviewGrid::ColumnsFor(300.0f, 140.0f, 6.0f) == 2);
            CHECK(std::fabs(two - 147.0f) < 1e-3f);          // (300 - 6) / 2
            CHECK(std::fabs(two * 2.0f + 6.0f - 300.0f) < 1e-3f);  // exactly the row
        }
        // An exact fit is left exactly alone.
        CHECK(std::fabs(PreviewGrid::FittedSide(186.0f, 90.0f, 6.0f, 10.0f) - 90.0f) <
              1e-3f);
        // ⚠ A SMALLER TARGET BUYS COLUMNS, not a bigger hole, which is what
        // makes the size setting mean density rather than waste.
        CHECK(PreviewGrid::ColumnsFor(400.0f, 60.0f, 6.0f) >
              PreviewGrid::ColumnsFor(400.0f, 215.0f, 6.0f));
        // ⚠ A pane narrower than one card SHRINKS the card rather than
        // overflowing it, and the floor stops that becoming unreadable.
        CHECK(std::fabs(PreviewGrid::FittedSide(40.0f, 215.0f, 6.0f, 10.0f) - 40.0f) <
              1e-3f);
        CHECK(std::fabs(PreviewGrid::FittedSide(4.0f, 215.0f, 6.0f, 10.0f) - 10.0f) <
              1e-3f);
        CHECK(PreviewGrid::ColumnsFor(400.0f, 90.0f, 6.0f) == 4);
        CHECK(PreviewGrid::ColumnsFor(2000.0f, 90.0f, 6.0f) == 20);
        CHECK(PreviewGrid::ColumnsFor(0.0f, 90.0f, 6.0f) == 1);   // never zero
        CHECK(PreviewGrid::ColumnsFor(-50.0f, 90.0f, 6.0f) == 1); // never negative
    }

    {  // Reveal ramp: 0..1 over kRevealFrames presents from the ready stamp.
        // A zero stamp means "never recorded" and must show the whole picture,
        // or every pre-stamp entry would draw invisible forever.
        CHECK(PreviewGrid::RevealAlpha(0, 100) == 1.0f);
        // A clock behind the stamp reveals nothing yet.
        CHECK(PreviewGrid::RevealAlpha(50, 49) == 0.0f);
        // The first present already shows something - a card must never spend
        // a frame both unspun and unpictured.
        const float first = PreviewGrid::RevealAlpha(50, 50);
        CHECK(first > 0.0f && first < 0.5f);
        // Monotone up, and saturated at the ramp's end.
        CHECK(PreviewGrid::RevealAlpha(50, 53) > PreviewGrid::RevealAlpha(50, 51));
        CHECK(PreviewGrid::RevealAlpha(50, 50 + PreviewGrid::kRevealFrames) == 1.0f);
        CHECK(PreviewGrid::RevealAlpha(50, 5000) == 1.0f);
    }

    {  // FNV-1a-64, pinned against the reference vectors so the disk cache
       // cannot silently re-key across a refactor. Empty input = offset basis.
        CHECK(PreviewGrid::Fnv1a64("") == 0xCBF29CE484222325ull);
        CHECK(PreviewGrid::Fnv1a64("a") == 0xAF63DC4C8601EC8Cull);
        CHECK(PreviewGrid::Fnv1a64("foobar") == 0x85944171F73967E8ull);
    }

    {  // Shard path: 16 uppercase hex digits, first two as the subdirectory,
       // .png extension because the disk format IS PNG (Task 0 verdict).
        const auto p = PreviewGrid::ShardPathFor(0xAB12CD34EF56789Aull);
        CHECK(p == "AB/AB12CD34EF56789A.png");
        const auto p0 = PreviewGrid::ShardPathFor(0x0000000000000001ull);
        CHECK(p0 == "00/0000000000000001.png");
    }

    {  // Disk key: lowercased, slashes normalised, fields joined with '|',
       // context fixed to "none" while no scene contains the body.
       // ⚠ THE ONE-PATH KEY IS THE WEAPON-CACHE PIN: this exact string is
       // what phase 1 wrote to disk, and phase 2's list-of-paths change must
       // keep producing it or every weapon thumbnail orphans.
        PreviewGrid::SceneIdentity id;
        id.plugin      = "Skyrim.ESM";
        id.localFormId = 0x0001397E;
        id.editorId    = "IronSword";
        id.modelPaths  = { "Weapons\\Iron\\LongSword.nif" };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "skyrim.esm|0001397e|ironsword|weapons/iron/longsword.nif|none");
        // ⚠ Two identities differing only by FormID must not share a key:
        // texture-swap variants render the same bare NIF today, and the key
        // keeping them apart is what lets a swap-aware renderer land later
        // with no invalidation story.
        auto id2 = id;
        id2.localFormId = 0x0001397F;
        CHECK(PreviewGrid::DiskKeyFor(id2) != PreviewGrid::DiskKeyFor(id));
    }

    {  // Multi-path disk keys (phase 2): folded, ';'-joined within the path
       // field, order-preserving, and the slot mask stays out.
        PreviewGrid::SceneIdentity id;
        id.plugin      = "Skyrim.ESM";
        id.localFormId = 0x00012E46;
        id.editorId    = "ArmorIronCuirass";
        id.modelPaths  = { "Armor\\Iron\\CuirassM_1.nif", "Armor\\Iron\\Pauldron.nif" };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "skyrim.esm|00012e46|armorironcuirass|"
              "armor/iron/cuirassm_1.nif;armor/iron/pauldron.nif|none");
        // Path order is identity: a reordered list is a different scene.
        auto id2 = id;
        std::swap(id2.modelPaths[0], id2.modelPaths[1]);
        CHECK(PreviewGrid::DiskKeyFor(id2) != PreviewGrid::DiskKeyFor(id));
        // ⚠ The slot mask is render CONTEXT, not identity: it must not
        // change the key, or every mask tweak would orphan the cache.
        auto id3 = id;
        id3.slotMask = 0x4;
        CHECK(PreviewGrid::DiskKeyFor(id3) == PreviewGrid::DiskKeyFor(id));
    }

    {  // Prune: queued entries stamped two or more presents back are stale.
       // ⚠ ONE present back is CURRENT: the drain sits in the Present thunk,
       // which can run on either side of FUCK's pass, so last present's
       // stamps must survive or every request dies before it can build.
       // Ready and Failed survive whatever their stamp.
        std::vector<PreviewGrid::EntryMeta> v{
            { 10, 0, PreviewGrid::State::kQueued },
            { 11, 0, PreviewGrid::State::kQueued },
            { 10, 0, PreviewGrid::State::kReady },
            { 9, 0, PreviewGrid::State::kFailed },
        };
        CHECK(PreviewGrid::PruneSelection(v, 11).empty());
        const auto stale = PreviewGrid::PruneSelection(v, 12);
        CHECK(stale.size() == 1 && stale[0] == 0);
    }

    {  // Build order: FIFO by stamp, then the ON-SCREEN order hint, so a
       // cold screen fills top-left to bottom-right instead of hash order
       // (field 2026-08-09: "they didn't load in the same order").
        std::vector<PreviewGrid::EntryMeta> v{
            { 12, 0, PreviewGrid::State::kQueued, 0, 2 },
            { 11, 0, PreviewGrid::State::kQueued, 0, 9 },
            { 11, 0, PreviewGrid::State::kQueued, 0, 4 },
            { 10, 0, PreviewGrid::State::kReady, 0, 0 },
        };
        const auto next = PreviewGrid::NextToBuild(v);
        CHECK(next.has_value() && *next == 2);  // oldest stamp, lowest hint
        std::vector<PreviewGrid::EntryMeta> none{
            { 10, 0, PreviewGrid::State::kReady, 0, 0 },
        };
        CHECK(!PreviewGrid::NextToBuild(none).has_value());
    }

    {  // LRU victim: only Ready entries are evictable (Queued has no handle
       // yet, Failed holds none), and only past the cap.
        std::vector<PreviewGrid::EntryMeta> v{
            { 0, 5, PreviewGrid::State::kReady },
            { 0, 3, PreviewGrid::State::kReady },
            { 0, 1, PreviewGrid::State::kQueued },
            { 0, 2, PreviewGrid::State::kFailed },
        };
        CHECK(!PreviewGrid::LruVictim(v, 2).has_value());  // 2 ready, cap 2: no victim
        const auto victim = PreviewGrid::LruVictim(v, 1);
        CHECK(victim.has_value() && *victim == 1);  // oldest lastUsed among Ready
    }

    {  // Manifest round trip, including a failed entry with its reason.
        PreviewManifest::Manifest m;
        m.thumbnailSize    = 256;
        m.rendererVersion  = "fittingroom-preview-png-v1";
        PreviewManifest::Entry e;
        e.key         = "skyrim.esm|0001397e|ironsword|weapons/iron/longsword.nif|none";
        e.status      = PreviewManifest::Status::kReady;
        e.file        = "AB/AB12CD34EF56789A.png";
        e.width       = 256;
        e.height      = 256;
        e.updatedTick = 41;
        m.entries["AB12CD34EF56789A"] = e;
        PreviewManifest::Entry f;
        f.key           = "some|other|key|none";
        f.status        = PreviewManifest::Status::kFailed;
        f.failureReason = "no renderable geometry";
        f.updatedTick   = 42;
        m.entries["0000000000000002"] = f;

        const auto text   = PreviewManifest::ToText(m);
        const auto parsed = PreviewManifest::FromText(text, 256,
                                                      "fittingroom-preview-png-v1");
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK(parsed->entries.size() == 2);
            const auto it = parsed->entries.find("AB12CD34EF56789A");
            CHECK(it != parsed->entries.end());
            if (it != parsed->entries.end()) {
                CHECK(it->second.key == e.key);
                CHECK(it->second.status == PreviewManifest::Status::kReady);
                CHECK(it->second.file == e.file);
                CHECK(it->second.updatedTick == 41);
            }
            const auto fit = parsed->entries.find("0000000000000002");
            CHECK(fit != parsed->entries.end() &&
                  fit->second.status == PreviewManifest::Status::kFailed &&
                  fit->second.failureReason == "no renderable geometry");
        }
    }

    {  // ⚠ THE THREE STALENESS MECHANISMS EACH DISCARD THE WHOLE MANIFEST.
       // A size change, a renderer change or a format-version change makes
       // every cached image wrong, and a survivor would be served stale.
        PreviewManifest::Manifest m;
        m.thumbnailSize   = 256;
        m.rendererVersion = "fittingroom-preview-png-v1";
        const auto text   = PreviewManifest::ToText(m);
        CHECK(!PreviewManifest::FromText(text, 512, "fittingroom-preview-png-v1")
                   .has_value());
        CHECK(!PreviewManifest::FromText(text, 256, "different-renderer").has_value());
    }

    {  // An unreadable manifest is refused, never read as empty: MyDyes'
       // exact rule, because the caller rewrites the file whole.
        CHECK(!PreviewManifest::FromText("", 256, "v").has_value());
        CHECK(!PreviewManifest::FromText("{", 256, "v").has_value());
        CHECK(!PreviewManifest::FromText("[]", 256, "v").has_value());
        // A malformed ENTRY costs that entry, not the file.
        const char* oneBad = R"({"version":1,"thumbnailSize":256,
            "rendererVersion":"v","entries":{
            "AAAA":{"key":"k","status":"ready","file":"AA/AAAA.png",
                     "width":256,"height":256,"updatedTick":1},
            "BBBB":"not an object"}})";
        const auto mixed = PreviewManifest::FromText(oneBad, 256, "v");
        CHECK(mixed.has_value() && mixed->entries.size() == 1);
    }

    {  // Helper and marker geometry: tokens against the FOLDED FULL PATH.
       // Path-wide on purpose (helpers live under helper parents) and the
       // over-match is the spec's own accepted risk: a real piece named
       // HelperPlate disappears, and the census line names it.
        CHECK(PreviewFilter::PathHasHelperToken("root/bsbound"));
        CHECK(PreviewFilter::PathHasHelperToken("scene/collision/hull01"));
        CHECK(PreviewFilter::PathHasHelperToken("a/editor marker"));
        CHECK(PreviewFilter::PathHasHelperToken("cuirass/helperplate"));  // accepted over-match
        CHECK(!PreviewFilter::PathHasHelperToken("armor/iron/cuirass_1"));
        CHECK(!PreviewFilter::PathHasHelperToken(""));
        // The caller folds; the filter is case-exact on purpose so there is
        // exactly one fold in the subsystem (PreviewGrid::FoldPath).
        CHECK(!PreviewFilter::PathHasHelperToken("Root/BSBound"));
    }

    {  // Embedded bodies: outfit NIFs routinely ship a full body shape that
       // z-fights the garment. LEAF names, exact match after folding.
        CHECK(PreviewFilter::IsEmbeddedBodyLeaf("cbbe"));
        CHECK(PreviewFilter::IsEmbeddedBodyLeaf("3ba"));
        CHECK(PreviewFilter::IsEmbeddedBodyLeaf("3ba_vagina"));
        CHECK(PreviewFilter::IsEmbeddedBodyLeaf("virtualbody"));
        // The SMP wigs' collision bust ('VirtualHead', field 2026-08-09):
        // its siblings virtualground and virtualhaircollision fall to the
        // helper tokens, this one needed its own leaf.
        CHECK(PreviewFilter::IsEmbeddedBodyLeaf("virtualhead"));
        CHECK(!PreviewFilter::IsEmbeddedBodyLeaf("cbbe hands"));  // not exact
        CHECK(!PreviewFilter::IsEmbeddedBodyLeaf("cuirass"));
    }

    {  // Hands and feet leaves hide unless the item occupies those slots.
       // EXACT leaf tokens only: "hands" must never catch "handles".
        CHECK(PreviewFilter::IsHandsLeaf("hands"));
        CHECK(PreviewFilter::IsFeetLeaf("feet"));
        CHECK(!PreviewFilter::IsHandsLeaf("handles"));
        CHECK(!PreviewFilter::IsFeetLeaf("feetwraps"));
        // Biped slot bits, Skyrim's slot-minus-30 convention: hands 33 ->
        // bit 3, feet 37 -> bit 7.
        CHECK(PreviewFilter::kHandsSlotBit == (1u << 3));
        CHECK(PreviewFilter::kFeetSlotBit == (1u << 7));
    }

    {  // Axis ranking: extents in any order, stable on ties so equal boxes
       // frame identically everywhere.
        const auto r = PreviewFraming::RankAxes({ 1.0f, 9.0f, 4.0f });
        CHECK(r.longest == 1 && r.second == 2 && r.shortest == 0);
        const auto tie = PreviewFraming::RankAxes({ 5.0f, 5.0f, 5.0f });
        CHECK(tie.longest == 0 && tie.second == 1 && tie.shortest == 2);
    }

    {  // Framing a sword-shaped box: length along Y, width along Z, thickness
       // along X. The eye looks mostly down the thickness (the flat's
       // normal), the basis is orthonormal, the long axis lands on the
       // frame's diagonal, every corner fits the fill cone, and the light
       // rides the camera's side of the scene.
        const std::array<float, 3> mn{ -2.0f, -50.0f, -6.0f };
        const std::array<float, 3> mx{ 2.0f, 50.0f, 6.0f };
        const auto                 f = PreviewFraming::FrameFor(mn, mx);

        CHECK(std::fabs(PreviewFraming::Length(f.eyeDir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(PreviewFraming::Length(f.up) - 1.0f) < 1e-4f);
        CHECK(std::fabs(PreviewFraming::Length(f.lightDir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(PreviewFraming::Dot(f.eyeDir, f.up)) < 1e-4f);
        CHECK(std::fabs(f.eyeDir.x) > 0.85f);  // down the thin axis

        // The length axis, projected on the image plane, sits 45 degrees off
        // the camera's up: the diagonal, by construction and kept by test.
        const PreviewFraming::Vec3 yAxis{ 0.0f, 1.0f, 0.0f };
        const auto proj = PreviewFraming::Normalize(PreviewFraming::Add(
            yAxis,
            PreviewFraming::Scale(f.eyeDir, -PreviewFraming::Dot(yAxis, f.eyeDir))));
        CHECK(std::fabs(std::fabs(PreviewFraming::Dot(proj, f.up)) - 0.70710678f) <
              0.02f);

        // Corner fit: from the fitted distance, every corner projects inside
        // the filled half-angle on both frame axes, and in front of the eye.
        const auto forward = PreviewFraming::Scale(f.eyeDir, -1.0f);
        const auto right =
            PreviewFraming::Normalize(PreviewFraming::Cross(f.up, forward));
        const float tanHalf =
            std::tan(PreviewFraming::kFovDegrees * 3.14159265f / 180.0f * 0.5f) *
            PreviewFraming::kFill;
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    const PreviewFraming::Vec3 c{ 2.0f * static_cast<float>(sx),
                                                  50.0f * static_cast<float>(sy),
                                                  6.0f * static_cast<float>(sz) };
                    const float z = f.distance - PreviewFraming::Dot(c, f.eyeDir);
                    CHECK(z > 0.0f);
                    CHECK(std::fabs(PreviewFraming::Dot(c, right)) <=
                          z * tanHalf * 1.001f);
                    CHECK(std::fabs(PreviewFraming::Dot(c, f.up)) <=
                          z * tanHalf * 1.001f);
                }
            }
        }
        CHECK(PreviewFraming::Dot(f.lightDir, f.eyeDir) > 0.5f);  // front-lit

        // Depth planes bracket the whole box.
        CHECK(f.nearZ > 0.0f);
        CHECK(f.farZ > f.distance);
    }

    {  // The shield pose: the diagonal MIRRORED to the negative side of the
       // thickness axis, because a shield's bind pose points its face there
       // and the plain diagonal photographed the straps (field 2026-08-09).
       // Everything else about the frame holds: orthonormal basis, front
       // lighting, corner fit through the same fitter.
        const std::array<float, 3> mn{ -2.0f, -30.0f, -30.0f };
        const std::array<float, 3> mx{ 2.0f, 30.0f, 30.0f };
        const auto                 d = PreviewFraming::FrameFor(mn, mx);
        const auto                 s =
            PreviewFraming::FrameFor(mn, mx, PreviewFraming::Pose::kShield);
        CHECK(d.eyeDir.x > 0.85f);   // the diagonal looks down +thickness
        CHECK(s.eyeDir.x < -0.85f);  // the shield looks down -thickness
        CHECK(std::fabs(PreviewFraming::Length(s.eyeDir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(PreviewFraming::Dot(s.eyeDir, s.up)) < 1e-4f);
        CHECK(PreviewFraming::Dot(s.lightDir, s.eyeDir) > 0.5f);  // front-lit
        CHECK(s.distance > 1.0f);
        CHECK(s.nearZ > 0.0f && s.farZ > s.distance);
    }

    {  // The upright pose (worn gear): roll-free up in the world's Z, the
       // eye in front (+Y, the skeleton's facing) and a little above, and
       // the corner fit still holds. A cuirass-shaped box: wide X, thin Y,
       // tall Z.
        const std::array<float, 3> mn{ -30.0f, -12.0f, -80.0f };
        const std::array<float, 3> mx{ 30.0f, 12.0f, 80.0f };
        const auto                 f =
            PreviewFraming::FrameFor(mn, mx, PreviewFraming::Pose::kUpright);
        CHECK(f.up.z > 0.95f);         // roll-free, Z dominant
        CHECK(std::fabs(f.up.x) < 0.1f);
        CHECK(f.eyeDir.y > 0.8f);      // from the front
        CHECK(f.eyeDir.z > 0.05f);     // slightly above
        CHECK(std::fabs(PreviewFraming::Dot(f.eyeDir, f.up)) < 1e-4f);
        const auto forward = PreviewFraming::Scale(f.eyeDir, -1.0f);
        const auto right =
            PreviewFraming::Normalize(PreviewFraming::Cross(f.up, forward));
        const float tanHalf =
            std::tan(PreviewFraming::kFovDegrees * 3.14159265f / 180.0f * 0.5f) *
            PreviewFraming::kFill;
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    const PreviewFraming::Vec3 c{ 30.0f * static_cast<float>(sx),
                                                  12.0f * static_cast<float>(sy),
                                                  80.0f * static_cast<float>(sz) };
                    const float z = f.distance - PreviewFraming::Dot(c, f.eyeDir);
                    CHECK(z > 0.0f);
                    CHECK(std::fabs(PreviewFraming::Dot(c, right)) <=
                          z * tanHalf * 1.001f);
                    CHECK(std::fabs(PreviewFraming::Dot(c, f.up)) <=
                          z * tanHalf * 1.001f);
                }
            }
        }
        CHECK(PreviewFraming::Dot(f.lightDir, f.eyeDir) > 0.5f);  // front-lit
    }

    {  // A degenerate box still frames: unit vectors, a positive distance,
       // and no NaN anywhere. The failure mode is a wrong picture, never a
       // crash or a black card.
        const std::array<float, 3> zero{ 0.0f, 0.0f, 0.0f };
        const auto                 f = PreviewFraming::FrameFor(zero, zero);
        CHECK(f.distance >= 1.0f);
        CHECK(std::fabs(PreviewFraming::Length(f.eyeDir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(PreviewFraming::Length(f.up) - 1.0f) < 1e-4f);
        CHECK(f.eyeDir.x == f.eyeDir.x && f.up.y == f.up.y &&
              f.lightDir.z == f.lightDir.z);  // NaN self-compares false
    }

    {  // Head-part scene context: OUT of the disk key (the slot-mask rule,
       // second verse), and an EMPTY editor ID keys cleanly because the
       // runtime rarely retains one for a head part (HeadPart.cpp, NameOf).
        PreviewGrid::SceneIdentity id;
        id.plugin      = "Skyrim.ESM";
        id.localFormId = 0x0001C766;
        id.editorId    = "";
        id.modelPaths  = {
            "Actors\\Character\\Character Assets\\Hair\\HairFemaleNord01.nif",
            "Actors\\Character\\Character Assets\\Hair\\HairLineFemaleNord01.nif"
        };
        id.kind        = PreviewGrid::SceneKind::kHair;
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "skyrim.esm|0001c766||"
              "actors/character/character assets/hair/hairfemalenord01.nif;"
              "actors/character/character assets/hair/hairlinefemalenord01.nif|none");
        auto id2 = id;
        id2.kind = PreviewGrid::SceneKind::kGear;
        CHECK(PreviewGrid::DiskKeyFor(id2) == PreviewGrid::DiskKeyFor(id));
        auto id3 = id;
        id3.kind = PreviewGrid::SceneKind::kEyes;
        CHECK(PreviewGrid::DiskKeyFor(id3) == PreviewGrid::DiskKeyFor(id));
        // The kind family: everything but gear is a head part.
        CHECK(!PreviewGrid::IsHeadPartKind(PreviewGrid::SceneKind::kGear));
        CHECK(PreviewGrid::IsHeadPartKind(PreviewGrid::SceneKind::kHair));
        CHECK(PreviewGrid::IsHeadPartKind(PreviewGrid::SceneKind::kEyes));
        CHECK(PreviewGrid::IsHeadPartKind(PreviewGrid::SceneKind::kBrows));
    }

    {  // The eye crop: one eye instead of the staring pair. The eyes mesh
       // spans both eyeballs across X; the card frames the -X half (the
       // symmetric halves make either correct) with Y and Z untouched.
        const std::array<float, 3> mn{ -4.0f, -1.5f, 118.0f };
        const std::array<float, 3> mx{ 4.0f, 1.5f, 126.0f };
        const auto [emn, emx] = PreviewFraming::EyeHalf(mn, mx);
        CHECK(emn[0] == -4.0f && emx[0] == 0.0f);
        CHECK(emn[1] == mn[1] && emx[1] == mx[1]);
        CHECK(emn[2] == mn[2] && emx[2] == mx[2]);
    }

    {  // The eyes pose: straight at the face (+Y), +Z up, no yaw: an iris
       // chip, not a staring sphere pair (field 2026-08-09 round 3).
        const std::array<float, 3> mn{ -4.0f, -1.5f, 118.0f };
        const std::array<float, 3> mx{ 0.0f, 1.5f, 126.0f };
        const auto f = PreviewFraming::FrameFor(mn, mx, PreviewFraming::Pose::kEyes);
        CHECK(f.eyeDir.y > 0.99f && std::fabs(f.eyeDir.x) < 1e-4f &&
              std::fabs(f.eyeDir.z) < 1e-4f);
        CHECK(f.up.z > 0.99f);
        CHECK(f.distance >= 1.0f);
        CHECK(PreviewFraming::Dot(f.lightDir, f.eyeDir) > 0.5f);  // front-lit
    }

    {  // Scalp extra parts leave the scene by PATH: the generic caps are
       // fitted to a head the card never draws and hang misaligned under
       // the hair (field 2026-08-09 round 3). The per-hair "<name>hl"
       // hairline pieces stay: they are authored against their own hair.
        CHECK(PreviewFilter::IsScalpPath("ks hairdo's/hairline/straightscalphuman.nif"));
        CHECK(PreviewFilter::IsScalpPath("ks hairdo's/hdt/elves/somescalp_2.nif"));
        CHECK(!PreviewFilter::IsScalpPath("ks hairdo's/amorhl.nif"));
        CHECK(!PreviewFilter::IsScalpPath("armor/iron/cuirassm_1.nif"));
    }

    {  // The pose rule, whole: worn gear stands upright, a head part stands
       // upright with no bits at all, and a SHIELD alone lies back on the
       // weapons' diagonal, because its bind pose is flat along the forearm
       // and the upright framing showed the disc edge-on (field 2026-08-09).
        CHECK(!PreviewGrid::StandsUpright(0, false));
        CHECK(PreviewGrid::StandsUpright(1u << 3, false));
        CHECK(PreviewGrid::StandsUpright(0, true));
        CHECK(PreviewGrid::StandsUpright(1u << 3, true));
        CHECK(PreviewFilter::kShieldSlotBit == (1u << 9));  // biped slot 39
        CHECK(!PreviewGrid::StandsUpright(PreviewFilter::kShieldSlotBit, false));
        CHECK(PreviewGrid::StandsUpright(
            PreviewFilter::kShieldSlotBit | (1u << 2), false));
        CHECK(PreviewGrid::StandsUpright(PreviewFilter::kShieldSlotBit, true));
    }

    {  // Swap-less scenes: swaps ABSENT keys exactly as today. The
       // byte-identity pin, third verse: OS-192 must not orphan the cache.
        PreviewGrid::SceneIdentity id;
        id.plugin      = "Skyrim.ESM";
        id.localFormId = 0x0010CFF2;
        id.editorId    = "ClothesMonkRobesColorRed";
        id.modelPaths  = { "Clothes\\Monk\\MonkRobes_F_1.nif" };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "skyrim.esm|0010cff2|clothesmonkrobescolorred|"
              "clothes/monk/monkrobes_f_1.nif|none");

        // One armour swap folds into ITS path: '@' then idx:tx00, entries
        // ','-joined (';' already separates paths in this field).
        auto id2 = id;
        PreviewGrid::TextureSwapEntry red;
        red.geomIndex   = 0;
        red.texPaths[0] = "Clothes\\Monk\\robes_color4.dds";
        id2.swaps       = { { red } };
        CHECK(PreviewGrid::DiskKeyFor(id2) ==
              "skyrim.esm|0010cff2|clothesmonkrobescolorred|"
              "clothes/monk/monkrobes_f_1.nif@0:clothes/monk/robes_color4.dds|none");
        CHECK(PreviewGrid::DiskKeyFor(id2) != PreviewGrid::DiskKeyFor(id));

        // ⚠⚠ THE TWO SPELLINGS OF ONE TEXTURE ARE ONE TEXTURE, so they share
        // one key and one card. A TESTextureSet's paths are rooted at
        // Data\Textures, and authors ship both "actors/..." and
        // "textures/actors/..."; the engine takes either. The swap loader
        // prepended the root unconditionally, so the second form asked for
        // Data\Textures\textures\actors\... , the engine substituted its purple
        // placeholder, and 116 demon-eye cards rendered the same lavender ball
        // with diffuse=2/2 and zero swap-no-diffuse in the log (field
        // 2026-08-09). Normalising in the key is also what makes the fix cheap:
        // the wrong cards re-key themselves and every other key is untouched.
        auto idPrefixed                  = id2;
        idPrefixed.swaps[0][0].texPaths[0] = "Textures\\Clothes\\Monk\\robes_color4.dds";
        CHECK(PreviewGrid::DiskKeyFor(idPrefixed) == PreviewGrid::DiskKeyFor(id2));

        // ⚠ AND THE PIN IN THE OTHER DIRECTION, which is the one that keeps the
        // cache alive: a path that never carried the prefix folds to exactly
        // the bytes it always did.
        CHECK(PreviewGrid::DiskKeyFor(id2) ==
              "skyrim.esm|0010cff2|clothesmonkrobescolorred|"
              "clothes/monk/monkrobes_f_1.nif@0:clothes/monk/robes_color4.dds|none");

        // ⚠ ONE leading prefix is stripped, not every occurrence: a real
        // folder named "textures" deeper in the path is part of the name.
        CHECK(PreviewGrid::NormalizeTexturePath("textures/textures/eye.dds") ==
              "textures/eye.dds");
        CHECK(PreviewGrid::NormalizeTexturePath("actors/textures/eye.dds") ==
              "actors/textures/eye.dds");
        // Not a prefix, just a name that starts the same way.
        CHECK(PreviewGrid::NormalizeTexturePath("textureset/eye.dds") ==
              "textureset/eye.dds");
        CHECK(PreviewGrid::NormalizeTexturePath("") == "");

        // A swap that changes only non-diffuse slots keys IDENTICALLY to a
        // diffuse-equal one: the card samples only TX00.
        auto id3 = id2;
        id3.swaps[0][0].texPaths[1] = "Clothes\\Monk\\robes_n.dds";
        CHECK(PreviewGrid::DiskKeyFor(id3) == PreviewGrid::DiskKeyFor(id2));

        // Authored-empty TX00 still keys (the apply clears the diffuse and
        // the card visibly changes), distinct from no swap at all.
        auto id4 = id;
        PreviewGrid::TextureSwapEntry bare;
        bare.geomIndex = 3;
        id4.swaps      = { { bare } };
        CHECK(PreviewGrid::DiskKeyFor(id4) ==
              "skyrim.esm|0010cff2|clothesmonkrobescolorred|"
              "clothes/monk/monkrobes_f_1.nif@3:|none");

        // An aligned swaps vector of all-empty lists keys as no swaps at
        // all: the capture side promises absent-when-empty, and the fold
        // must not punish a caller that sized the vector anyway.
        auto id5 = id;
        id5.swaps = { {} };
        CHECK(PreviewGrid::DiskKeyFor(id5) == PreviewGrid::DiskKeyFor(id));
    }

    {  // Eyes: kAllGeometry folds as '*', on the main model only; a
       // multi-path scene folds per path and a swap-less sibling path
       // contributes exactly its old bytes.
        PreviewGrid::SceneIdentity id;
        id.plugin      = "LDD_EyesForUBE.esp";
        id.localFormId = 0x00000804;
        id.editorId    = "LDD_Eye_DarkBrown_F";
        id.modelPaths  = { "LDDUUE\\Eyes\\LDD_Eyes.nif",
                           "LDDUUE\\Eyes\\LDD_Otoczka.nif" };
        PreviewGrid::TextureSwapEntry tnam;
        tnam.geomIndex   = PreviewGrid::TextureSwapEntry::kAllGeometry;
        tnam.texPaths[0] = "LDDUUE\\Eyes\\Eye_darkbrown.dds";
        id.swaps         = { { tnam }, {} };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "ldd_eyesforube.esp|00000804|ldd_eye_darkbrown_f|"
              "ldduue/eyes/ldd_eyes.nif@*:ldduue/eyes/eye_darkbrown.dds;"
              "ldduue/eyes/ldd_otoczka.nif|none");

        // Two entries on one path, ','-joined, authored order preserved.
        PreviewGrid::TextureSwapEntry a, b;
        a.geomIndex   = 2;
        a.texPaths[0] = "LDD\\Top_d_red.dds";
        b.geomIndex   = 5;
        b.texPaths[0] = "LDD\\Bot_d_red.dds";
        id.swaps      = { { a, b }, {} };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "ldd_eyesforube.esp|00000804|ldd_eye_darkbrown_f|"
              "ldduue/eyes/ldd_eyes.nif@2:ldd/top_d_red.dds,5:ldd/bot_d_red.dds;"
              "ldduue/eyes/ldd_otoczka.nif|none");
    }

    {  // SwapFor: exact index first, kAllGeometry as the fallback, null
       // when neither, first match wins among duplicates. The collected
       // index the caller passes is the ENGINE's running geometry counter
       // (unfiltered depth-first visit order), never a post-filter index.
        std::vector<PreviewGrid::TextureSwapEntry> entries(3);
        entries[0].geomIndex   = 4;
        entries[0].texPaths[0] = "a.dds";
        entries[1].geomIndex   = 4;
        entries[1].texPaths[0] = "b.dds";
        entries[2].geomIndex   = PreviewGrid::TextureSwapEntry::kAllGeometry;
        entries[2].texPaths[0] = "all.dds";
        CHECK(PreviewGrid::SwapFor(entries, 4) == &entries[0]);
        CHECK(PreviewGrid::SwapFor(entries, 7) == &entries[2]);
        entries.pop_back();
        CHECK(PreviewGrid::SwapFor(entries, 7) == nullptr);
        const std::vector<PreviewGrid::TextureSwapEntry> none;
        CHECK(PreviewGrid::SwapFor(none, 0) == nullptr);
    }

    {  // The by-name swap (OS-212, the skin pack rule): matched by the
       // geometry's own TX00 file name alone, after an exact index and before
       // kAllGeometry, and never by index; an indexed entry never by name.
        std::vector<PreviewGrid::TextureSwapEntry> entries(3);
        entries[0].geomIndex    = 4;
        entries[0].texPaths[0]  = "indexed.dds";
        entries[1].whenTex0Name = "femalebody_1_d.dds";
        entries[1].texPaths[0]  = "FittingRoom\\skins\\Sayble 4K\\!UBE\\Body\\femalebody_1_d.dds";
        entries[2].geomIndex    = PreviewGrid::TextureSwapEntry::kAllGeometry;
        entries[2].texPaths[0]  = "all.dds";
        // Index 4 with a matching name: the index still wins.
        CHECK(PreviewGrid::SwapFor(entries, 4, "femalebody_1_d.dds") == &entries[0]);
        // Any other index with the name, whatever the case: the by-name entry.
        CHECK(PreviewGrid::SwapFor(entries, 0, "femalebody_1_d.dds") == &entries[1]);
        CHECK(PreviewGrid::SwapFor(entries, 9, "FemaleBody_1_D.DDS") == &entries[1]);
        // The wrong name, or no name at all: kAllGeometry as ever.
        CHECK(PreviewGrid::SwapFor(entries, 9, "femalebody_etc_v2_1.dds") == &entries[2]);
        CHECK(PreviewGrid::SwapFor(entries, 9) == &entries[2]);
        // A by-name entry is never found by index, even kAllGeometry's.
        std::vector<PreviewGrid::TextureSwapEntry> byNameOnly{ entries[1] };
        CHECK(PreviewGrid::SwapFor(byNameOnly, 0) == nullptr);
        CHECK(PreviewGrid::SwapFor(byNameOnly, PreviewGrid::TextureSwapEntry::kAllGeometry) ==
              nullptr);
        CHECK(PreviewGrid::SwapFor(byNameOnly, 0, "femalebody_1_d.dds") == &byNameOnly[0]);
        // And an indexed entry is never found by name.
        std::vector<PreviewGrid::TextureSwapEntry> indexedOnly{ entries[0] };
        CHECK(PreviewGrid::SwapFor(indexedOnly, 0, "indexed.dds") == nullptr);

        // The key spells a by-name entry '=name:tx00', a spelling no index or
        // '*' produces, so every existing key is byte-identical (pinned above).
        PreviewGrid::SceneIdentity id;
        id.editorId   = "skin:Sayble 4K";
        id.modelPaths = { "!UBE\\Body\\femalebody_tangent_1.nif",
                          "!UBE\\Hands\\femalehands_tangent_1.nif" };
        id.kind       = PreviewGrid::SceneKind::kSkin;
        id.swaps      = { { entries[1] }, { entries[1] } };
        CHECK(PreviewGrid::DiskKeyFor(id) ==
              "|00000000|skin:sayble 4k|"
              "!ube/body/femalebody_tangent_1.nif@=femalebody_1_d.dds:"
              "fittingroom/skins/sayble 4k/!ube/body/femalebody_1_d.dds;"
              "!ube/hands/femalehands_tangent_1.nif@=femalebody_1_d.dds:"
              "fittingroom/skins/sayble 4k/!ube/body/femalebody_1_d.dds|none");
        // The same scene with nothing swapped keys as a plain two-path scene.
        PreviewGrid::SceneIdentity plain = id;
        plain.swaps.clear();
        CHECK(PreviewGrid::DiskKeyFor(plain) ==
              "|00000000|skin:sayble 4k|!ube/body/femalebody_tangent_1.nif;"
              "!ube/hands/femalehands_tangent_1.nif|none");
    }

    {  // the mannequin's kind rule (OS-204)
        using PreviewGrid::MannequinFor;
        using PreviewGrid::MannequinKind;
        using PreviewGrid::SceneKind;

        // ⚠⚠ THE PIN THAT KEEPS PHASE 1's THUMBNAILS. A weapon or ammo scene
        // carries no slot bit and must gain nothing, because the mannequin
        // reaches the disk key only through the paths it adds: gain none and
        // the key is byte-identical to the one that built the card already on
        // disk. Break this and every weapon card in the world rebuilds.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear, 0) == MannequinKind::kNone);

        // A lone shield stays a shield: it keeps the weapons' diagonal, so a
        // body behind it would be photographed edge-on with the disc.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear, PreviewFilter::kShieldSlotBit) ==
              MannequinKind::kNone);
        // Sharing a scene with body slots, it follows the body, the same way
        // StandsUpright already decides its pose.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear,
                           PreviewFilter::kShieldSlotBit | PreviewFilter::kHandsSlotBit) ==
              MannequinKind::kFigure);

        // Worn gear takes the figure whichever slot it is worn on.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear, 1u << 2) == MannequinKind::kFigure);
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear, PreviewFilter::kHandsSlotBit) ==
              MannequinKind::kFigure);
        CHECK(MannequinFor(PreviewGrid::SceneKind::kGear, PreviewFilter::kHeadSlotBit) ==
              MannequinKind::kFigure);

        // ⚠ Hair and facial hair take the SAME figure gear takes, and both
        // crop to head and shoulders, so a hair card and a helmet card are the
        // same picture with a different thing on it. A head framing itself
        // came back at a different scale and read as a second feature (field
        // 2026-08-10). Eyes and brows keep their field-approved close-ups, so
        // they gain nothing and keep their thumbnails too.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kHair, 0) == MannequinKind::kFigure);
        CHECK(MannequinFor(PreviewGrid::SceneKind::kFacialHair, 0) == MannequinKind::kFigure);
        // ⚠ Eyes take a face now; brows keep their field-approved close-up
        // and therefore keep their thumbnails too.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kEyes, 0) == MannequinKind::kFigure);
        CHECK(MannequinFor(PreviewGrid::SceneKind::kBrows, 0) == MannequinKind::kNone);
    }

    {  // the standard views (OS-204, field 2026-08-10)
        using PreviewFraming::Crop;
        using PreviewFraming::CropFor;

        constexpr std::uint32_t kBody   = 1u << 2;   // 32
        constexpr std::uint32_t kHair   = 1u << 1;   // 31
        constexpr std::uint32_t kAmulet = 1u << 5;   // 35
        constexpr std::uint32_t kRing   = 1u << 6;   // 36
        constexpr std::uint32_t kCalves = 1u << 8;   // 38

        // No mannequin, no crop: such a scene keys byte-identically to the
        // card already on disk, so a different framing would leave the cached
        // picture and the live framing disagreeing with nothing to tell them
        // apart.
        using PreviewFraming::HeadScene;

        CHECK(CropFor(false, kBody) == Crop::kWhole);
        CHECK(CropFor(false, 0, HeadScene::kHair) == Crop::kWhole);
        CHECK(CropFor(false, 0, HeadScene::kEyes) == Crop::kWhole);
        CHECK(CropFor(false, 0, HeadScene::kFacialHair) == Crop::kWhole);

        CHECK(CropFor(true, kBody) == Crop::kWhole);
        CHECK(CropFor(true, kBody | kHair) == Crop::kWhole);
        CHECK(CropFor(true, kHair) == Crop::kHead);
        CHECK(CropFor(true, PreviewFilter::kHeadSlotBit) == Crop::kHead);
        CHECK(CropFor(true, kAmulet) == Crop::kChest);
        CHECK(CropFor(true, PreviewFilter::kHandsSlotBit) == Crop::kHand);
        CHECK(CropFor(true, PreviewFilter::kFeetSlotBit | kCalves) == Crop::kFeet);
        // ⚠ A RING ALONE EARNS THE FINGERS, because a band framed with the
        // whole hand is a few pixels of metal (field 2026-08-10).
        CHECK(CropFor(true, kRing) == Crop::kRing);
        // ⚠ BUT ONLY ALONE. Anything claiming the ring slot AND a gauntlet
        // slot is a gauntlet, and framing it on the fingers would crop it.
        CHECK(CropFor(true, kRing | PreviewFilter::kHandsSlotBit) == Crop::kHand);
        // ⚠ A BEARD IS NOT HAIR FOR FRAMING. They shared the head window until
        // the field asked for a beard 70% closer; a hair needs the whole skull
        // and a beard needs the jaw.
        CHECK(CropFor(true, 0, HeadScene::kHair) == Crop::kHead);
        CHECK(CropFor(true, 0, HeadScene::kFacialHair) == Crop::kFacialHair);
        CHECK(CropFor(true, 0, HeadScene::kEyes) == Crop::kEye);
        // Mixed across groups is the whole figure rather than a wrong close-up.
        CHECK(CropFor(true, kRing | kAmulet) == Crop::kWhole);
    }

    {  // the FX last resort: only ever on a card that already lost
        using PreviewFilter::ShouldRetryKeepingEffects;

        // The case it exists for: a spectral draugr weapon, one geometry, and
        // the FX skip took it.
        CHECK(ShouldRetryKeepingEffects(0, 1));
        // ⚠⚠ THE GUARD THAT MAKES IT SAFE. A scene that rendered ANYTHING is
        // never retried, so no card that works today can change, and that
        // holds however much FX was skipped beside the geometry that stayed:
        // Dawnbreaker keeps its four real pieces and loses its glow planes,
        // and the eye cards skip three each.
        CHECK(!ShouldRetryKeepingEffects(1, 1));
        CHECK(!ShouldRetryKeepingEffects(4, 6));
        CHECK(!ShouldRetryKeepingEffects(6, 3));
        // An empty scene with no FX skipped has some other cause, and walking
        // every root a second time would not find it.
        CHECK(!ShouldRetryKeepingEffects(0, 0));

        // ⚠ THE BLEND SKIP JOINS THE SAME RESCUE. An item authored entirely
        // out of additive planes has to be retried the way an all-FX one is,
        // or the new rule would turn a working card into a failed one. The
        // guard is unchanged: a scene that rendered anything is never retried.
        CHECK(ShouldRetryKeepingEffects(0, 0, 1));
        CHECK(ShouldRetryKeepingEffects(0, 1, 1));
        CHECK(!ShouldRetryKeepingEffects(1, 0, 1));
        CHECK(!ShouldRetryKeepingEffects(0, 0, 0));
        // The old two-argument form still means what it meant.
        CHECK(ShouldRetryKeepingEffects(0, 1));
    }

    {  // glow planes are caught by what they DO, not what shader they wear
        using PreviewFilter::IsUnreproducibleBlend;
        using PreviewFilter::kBlendInvSrcAlpha;
        using PreviewFilter::kBlendOne;
        using PreviewFilter::kBlendSrcAlpha;

        // ⚠⚠ THE CASE THE FIELD REPORTED (user 2026-08-21, Bound Arrow card).
        // `FlamesMesh02` is a BSLightingShaderProperty of type EnvMap, so the
        // effect-shader skip never saw it, and its NiAlphaProperty is 0x100D:
        // blending on, src srcAlpha, dst ONE. Additive art is BLACK where the
        // game shows nothing, so the standard blend state painted the plane's
        // empty half as a violet slab over the arrow.
        CHECK(IsUnreproducibleBlend(true, kBlendSrcAlpha, kBlendOne));

        // The straight pair is the one state the pass owns, so it draws.
        CHECK(!IsUnreproducibleBlend(true, kBlendSrcAlpha, kBlendInvSrcAlpha));

        // ⚠ BLENDING OFF IS NEVER THIS, whatever the modes say. A NIF stores
        // src and dst even when blending is disabled, and a cutout leaf or a
        // hair card tests alpha with blending off; those draw correctly
        // through the opaque state, and reading the modes alone would skip
        // them. The arrow's own body shapes are 0x120D, which is exactly this:
        // test on, blend on... so the pair matters, not the flag alone.
        CHECK(!IsUnreproducibleBlend(false, kBlendSrcAlpha, kBlendOne));
        CHECK(!IsUnreproducibleBlend(false, kBlendOne, kBlendOne));

        // The UBE eye "outer" layer, which is where this rule was born and
        // why it was already right before it was moved.
        CHECK(IsUnreproducibleBlend(true, kBlendSrcAlpha, kBlendOne));
        // Anything else exotic is skipped for the same reason rather than
        // approximated: the pass has no state for it either.
        CHECK(IsUnreproducibleBlend(true, kBlendOne, kBlendOne));
        CHECK(IsUnreproducibleBlend(true, kBlendInvSrcAlpha, kBlendSrcAlpha));
    }

    {  // which pass a geometry draws in
        using PreviewFilter::DrawsInAlphaPass;
        using PreviewFilter::kBlendInvSrcAlpha;
        using PreviewFilter::kBlendOne;
        using PreviewFilter::kBlendSrcAlpha;

        // ⚠⚠ THE CASE THE FIELD REPORTED (user 2026-08-21, third round). ENB
        // Light re-authors the Bound Arrow's own arrows as blend on, srcAlpha
        // to ONE, alpha test GREATER 128. Blending them put six arrows in the
        // alpha pass, which tests depth and does not write it, and they
        // ghosted through each other. The test is what shapes such a mesh, so
        // it draws OPAQUE with its cutoff.
        CHECK(!DrawsInAlphaPass(true, true, kBlendSrcAlpha, kBlendOne));

        // A blend the pass owns is still a blend, test or no test.
        CHECK(DrawsInAlphaPass(true, false, kBlendSrcAlpha, kBlendInvSrcAlpha));
        CHECK(DrawsInAlphaPass(true, true, kBlendSrcAlpha, kBlendInvSrcAlpha));

        // An exotic blend with NO test really is an overlay, and nothing here
        // changes where it used to go.
        CHECK(DrawsInAlphaPass(true, false, kBlendSrcAlpha, kBlendOne));

        // Blending off never reaches the alpha pass, whatever the modes say.
        CHECK(!DrawsInAlphaPass(false, true, kBlendSrcAlpha, kBlendOne));
        CHECK(!DrawsInAlphaPass(false, false, kBlendSrcAlpha, kBlendInvSrcAlpha));
    }

    {  // an overlay blend: exotic modes AND no alpha test to shape it
        using PreviewFilter::IsOverlayBlend;
        using PreviewFilter::kBlendInvSrcAlpha;
        using PreviewFilter::kBlendOne;
        using PreviewFilter::kBlendSrcAlpha;

        // 0x100D on bound weapons' FlamesMesh01 and the Creation Club magic
        // arrows' ArrowQuiver:2: blend on, srcAlpha to ONE, test off.
        CHECK(IsOverlayBlend(true, false, kBlendSrcAlpha, kBlendOne));

        // ⚠⚠ THE GUARD THAT COST A ROUND. 0x120D, the same exotic blend WITH
        // alpha test GREATER 128, is how ENB Light authors the arrows
        // themselves. Without this the skip ate seven of them.
        CHECK(!IsOverlayBlend(true, true, kBlendSrcAlpha, kBlendOne));

        // A blend the pass owns is never an overlay, test or not.
        CHECK(!IsOverlayBlend(true, false, kBlendSrcAlpha, kBlendInvSrcAlpha));
        CHECK(!IsOverlayBlend(true, true, kBlendSrcAlpha, kBlendInvSrcAlpha));
        // Blending off is never this.
        CHECK(!IsOverlayBlend(false, false, kBlendSrcAlpha, kBlendOne));
    }

    {  // FX planes that carry no property to judge at all, caught by name
        using PreviewFilter::IsFxName;

        // Censused with tools/nif_shape_alpha.py over the four arrow families
        // reported on 2026-08-21. Every one of these carries NO alpha property,
        // so nothing but the name can see it.
        CHECK(IsFxName("arrow/flamesmesh02"));        // bound + Darkend torment
        CHECK(IsFxName("arrow/blurmesha"));           // Ordinator trick arrows
        CHECK(IsFxName("quiver/quiverfx:3"));         // CC magic arrows
        CHECK(IsFxName("arrow/fxmesh"));              // CC magic arrows
        CHECK(IsFxName("arrow/trailshort01"));        // CC magic arrows
        CHECK(IsFxName("boundsword/flameshit02:1"));  // bound ench effects
        CHECK(IsFxName("boundsword/refractmesh2"));
        CHECK(IsFxName("boundaxe/refrecthit02:0"));   // the author's spelling
        CHECK(IsFxName("arrow/enblight01"));

        // ⚠⚠ THE REAL PIECES OF THE SAME FILES, which must survive. If any of
        // these ever matches, the card loses the item and keeps nothing.
        CHECK(!IsFxName("arrow/arrowquiver/arrow1"));
        CHECK(!IsFxName("arrow/arrow:0"));
        CHECK(!IsFxName("arrow/arrowquiver:0"));
        CHECK(!IsFxName("arrow/boundarrowflight:0"));
        CHECK(!IsFxName("arrow/mesharrow:0"));

        // ⚠ COMPOUND WORDS ONLY. A bare "fx" or "trail" would reach ordinary
        // names, and these are the ones that would have been hit.
        CHECK(!IsFxName("weapons/fxtail/blade"));
        CHECK(!IsFxName("armor/trailingcape"));
        CHECK(!IsFxName("weapons/effects/blade"));
    }

    {  // a body card is its own scene kind (OS-204, the Bodies page)
        using PreviewGrid::IsAlignedKind;
        using PreviewGrid::IsHeadPartKind;
        using PreviewGrid::MannequinFor;
        using PreviewGrid::MannequinKind;
        using PreviewGrid::SceneKind;

        // ⚠⚠ THE BODY IS THE SUBJECT, so nothing composes on top of it. The
        // mannequin is made OUT OF a body mesh, and one of the meshes the
        // Bodies page can show is that very file: composing would put the two
        // inside each other to z-fight for the whole card.
        CHECK(MannequinFor(PreviewGrid::SceneKind::kBody, 0) == MannequinKind::kNone);
        CHECK(MannequinFor(PreviewGrid::SceneKind::kBody, 1u << 2) == MannequinKind::kNone);
        // ⚠⚠ AND IT IS NOT A HEAD PART. This predicate used to read "not
        // kGear", which a new kind silently flips: the head-part scaffolding
        // rules drop geometry with no material and geometry on a non-standard
        // blend, both written about eye lenses and hair shells, and a body has
        // neither. Nothing in the compiler catches that, so it is pinned.
        CHECK(!IsHeadPartKind(PreviewGrid::SceneKind::kBody));
        CHECK(!IsHeadPartKind(PreviewGrid::SceneKind::kGear));
        CHECK(IsHeadPartKind(PreviewGrid::SceneKind::kHair));
        CHECK(IsHeadPartKind(PreviewGrid::SceneKind::kEyes));
        CHECK(IsHeadPartKind(PreviewGrid::SceneKind::kBrows));
        CHECK(IsHeadPartKind(PreviewGrid::SceneKind::kFacialHair));
        // A body has no head bone to align and nothing to align it to.
        CHECK(!IsAlignedKind(PreviewGrid::SceneKind::kBody));
        // ⚠⚠ AND THE GARMENT FILTERS MUST NOT BE POINTED AT IT. They exist to
        // strip a body OUT of a garment scene, and here the body IS the scene:
        // the face-flag rule (a UBE body is kFaceGenRGBTint), the embedded-body
        // leaf list (the 3BA reference NIF's shapes are named exactly 3BA,
        // 3BA_Vagina and 3BA_Anus) and the hands and feet slot gates (a body's
        // hands leaf is "hands" and its slot mask is zero) each delete the
        // whole subject. This is the mannequin's own exemption, and skipping it
        // is the m1 failure again: the card comes back empty with a perfectly
        // correct key.
        CHECK(PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kBody));
        CHECK(!PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kGear));
        CHECK(!PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kHair));
        CHECK(!PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kEyes));
        CHECK(!PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kBrows));
        CHECK(!PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kFacialHair));

        // A skin card (OS-212) is a body subject in every way but one: framed
        // as the whole figure, exempt from the garment filters, no mannequin
        // composed, not a head part, not aligned, and DRAWN TEXTURED where a
        // body card is the grey figure. That last split is the new predicate.
        CHECK(PreviewGrid::IsBodySubjectKind(PreviewGrid::SceneKind::kSkin));
        CHECK(!IsHeadPartKind(PreviewGrid::SceneKind::kSkin));
        CHECK(!IsAlignedKind(PreviewGrid::SceneKind::kSkin));
        CHECK(MannequinFor(PreviewGrid::SceneKind::kSkin, 0) == MannequinKind::kNone);
        CHECK(PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kBody));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kSkin));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kGear));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kHair));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kEyes));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kBrows));
        CHECK(!PreviewGrid::DrawsGreyBody(PreviewGrid::SceneKind::kFacialHair));
    }

    {  // BodySlide's .osd morph data, and the check that makes it safe
        namespace BMD = OS::BodyMorphData;

        // A blob in the real layout: 8 header bytes, a u32 set count, then per
        // set a length-prefixed name, a u16 delta count and 14 bytes each.
        const auto blob = [](bool a_truncate, bool a_trailing) {
            std::vector<std::byte> b;
            const auto u8v  = [&](std::uint8_t v) { b.push_back(std::byte{ v }); };
            const auto raw  = [&](const void* p, std::size_t n) {
                const auto* q = static_cast<const unsigned char*>(p);
                for (std::size_t i = 0; i < n; ++i) b.push_back(std::byte{ q[i] });
            };
            const auto u16v = [&](std::uint16_t v) { raw(&v, 2); };
            const auto u32v = [&](std::uint32_t v) { raw(&v, 4); };
            const auto f32v = [&](float v) { raw(&v, 4); };
            u32v(0); u32v(0);                     // header, unread
            u32v(1);                              // one set
            const char* nm = "Breasts";
            u8v(static_cast<std::uint8_t>(std::strlen(nm)));
            raw(nm, std::strlen(nm));
            u16v(2);                              // two deltas
            u16v(7);    f32v(1.0f); f32v(2.0f); f32v(3.0f);
            u16v(29297); f32v(-0.5f); f32v(0.0f); f32v(0.25f);
            if (a_truncate) b.pop_back();
            if (a_trailing) b.push_back(std::byte{ 0 });
            return b;
        };

        BMD::DeltaSets sets;
        const auto     good = blob(false, false);
        CHECK(BMD::Parse(good.data(), good.size(), sets));
        CHECK(sets.size() == 1);
        CHECK(sets.count("Breasts") == 1);
        CHECK(sets["Breasts"].indices.size() == 2);
        CHECK(sets["Breasts"].indices[1] == 29297);  // the real file's top index
        CHECK(std::fabs(sets["Breasts"].deltas[0][1] - 2.0f) < 1e-6f);

        // ⚠⚠ THE INTEGRITY CHECK IS THE SAFETY ARGUMENT, so it is pinned from
        // both sides. Every record is length-prefixed, so a correct parse
        // lands on the final byte exactly; a layout wrong in any field
        // finishes early or late. This project has shipped garbage geometry
        // twice from an assumed layout, and refusing costs a card rather than
        // a wrong picture.
        BMD::DeltaSets bad;
        const auto     shortBlob = blob(true, false);
        CHECK(!BMD::Parse(shortBlob.data(), shortBlob.size(), bad));
        const auto longBlob = blob(false, true);
        CHECK(!BMD::Parse(longBlob.data(), longBlob.size(), bad));
        // ⚠ AND A REFUSAL LEAVES THE OUTPUT ALONE, so a caller that ignores
        // the bool cannot render half a body from a rejected file.
        CHECK(bad.empty());
        CHECK(!BMD::Parse(nullptr, 0, bad));
        const std::array<std::byte, 4> tiny{};
        CHECK(!BMD::Parse(tiny.data(), tiny.size(), bad));

        {  // the RUNTIME morph file (.tri), which is a second source of the
           // same displacements and the only one a preset with no BodySlide
           // project has. Built here byte by byte so the layout is pinned by
           // something other than the reader that reads it.
            auto u8v  = [](std::vector<std::byte>& b, unsigned v) {
                b.push_back(static_cast<std::byte>(v & 0xFF));
            };
            auto u16v = [&](std::vector<std::byte>& b, unsigned v) {
                u8v(b, v & 0xFF); u8v(b, (v >> 8) & 0xFF);
            };
            auto f32v = [&](std::vector<std::byte>& b, float f) {
                std::uint32_t bits{}; std::memcpy(&bits, &f, 4);
                u16v(b, bits & 0xFFFF); u16v(b, (bits >> 16) & 0xFFFF);
            };
            auto name = [&](std::vector<std::byte>& b, const char* s) {
                const auto n = std::strlen(s);
                u8v(b, static_cast<unsigned>(n));
                for (std::size_t i = 0; i < n; ++i) u8v(b, static_cast<unsigned>(s[i]));
            };
            std::vector<std::byte> t;
            for (char c : std::string{ "PIRT" }) u8v(t, static_cast<unsigned>(c));
            u16v(t, 2);            // TWO shapes: the body and a collider
            name(t, "Body");
            u16v(t, 1);            // one morph
            name(t, "Breasts");
            f32v(t, 0.5f);         // multiplier: deltas are ticks of this
            u16v(t, 2);            // two vertices
            u16v(t, 3); u16v(t, 4); u16v(t, 0); u16v(t, 0);          // v3 -> x +2
            u16v(t, 7); u16v(t, 0); u16v(t, 0xFFFE); u16v(t, 0);     // v7 -> y -1
            // ⚠⚠ THE SECOND SHAPE'S MORPH SHARES THE FIRST'S NAME AND MUST NOT
            // MERGE INTO IT. A BodySlide build writes the body's physics
            // colliders into the same .tri under the same 120 morph names,
            // each indexing its OWN few-hundred-vertex array; folding them
            // together stacked collider deltas onto low-index body vertices
            // and tore every seam on every strong preset (field 2026-08-11).
            // It IS consumed: the exactness check below still has to land.
            name(t, "VirtualBreasts");
            u16v(t, 1);
            name(t, "Breasts");    // the same name, another mesh's indexing
            f32v(t, 1.0f);
            u16v(t, 1);
            u16v(t, 3); u16v(t, 100); u16v(t, 0); u16v(t, 0);        // poison
            u16v(t, 1);            // one UV shape, and it must be consumed
            name(t, "Body");
            u16v(t, 1);
            name(t, "UVThing");
            f32v(t, 1.0f);
            u16v(t, 1);
            u16v(t, 0); u16v(t, 0); u16v(t, 0);  // SIX bytes, not eight

            BMD::DeltaSets tri;
            CHECK(BMD::ParseTri(t.data(), t.size(), tri));
            CHECK(tri.size() == 1);
            CHECK(tri.contains("Breasts"));
            CHECK(!tri.contains("UVThing"));  // uv morphs are not shape
            // Two entries, not three: the collider's same-named morph was
            // consumed and dropped, so the body's set carries the body alone.
            const std::vector<std::uint16_t> wantIndices{ 3, 7 };
            CHECK(tri["Breasts"].indices == wantIndices);
            // i16 ticks scaled by the multiplier.
            CHECK(std::fabs(tri["Breasts"].deltas[0][0] - 2.0f) < 1e-6f);
            CHECK(std::fabs(tri["Breasts"].deltas[1][1] + 1.0f) < 1e-6f);
            // ⚠ EXACTNESS IS THE WHOLE SAFETY ARGUMENT, same as the .osd. A
            // trailing byte means the layout was misread, and a misread that
            // is tolerated scrambles a body with a clean log.
            auto tooLong = t;  u8v(tooLong, 0);
            BMD::DeltaSets rejected;
            CHECK(!BMD::ParseTri(tooLong.data(), tooLong.size(), rejected));
            CHECK(!BMD::ParseTri(t.data(), t.size() - 1, rejected));
            CHECK(rejected.empty());  // a refusal leaves the output untouched
            std::vector<std::byte> notTri = t;  notTri[0] = std::byte{ 'X' };
            CHECK(!BMD::ParseTri(notTri.data(), notTri.size(), rejected));
        }

        // The slider arithmetic. ⚠ A PRESET NAMES ONLY WHAT IT CHANGES, so an
        // unnamed slider takes the .osp's OWN default and not zero: reading it
        // as zero is the difference between a body and a collapsed one, and it
        // is silent.
        BMD::SliderRule rule;
        rule.dataName = "Breasts";
        rule.big      = 40.0f;
        CHECK(std::fabs(BMD::SliderWeight(rule, true, 0.75f) - 0.75f) < 1e-6f);
        CHECK(std::fabs(BMD::SliderWeight(rule, false, 0.0f) - 0.40f) < 1e-6f);
        rule.invert = true;
        CHECK(std::fabs(BMD::SliderWeight(rule, true, 0.75f) - 0.25f) < 1e-6f);
        rule.invert = false;
        // ⚠⚠ AN UNNAMED SLIDER IS A small->big PAIR WALKED BY THE CHARACTER'S
        // WEIGHT, not the big end. Reading big alone drew the weight-100 body
        // for every character, and it is silent because the result is still a
        // perfectly plausible body.
        rule.small = 20.0f;  // rule.big is 40
        CHECK(std::fabs(BMD::SliderWeight(rule, false, 0.0f, 1.0f) - 0.40f) < 1e-6f);
        CHECK(std::fabs(BMD::SliderWeight(rule, false, 0.0f, 0.0f) - 0.20f) < 1e-6f);
        CHECK(std::fabs(BMD::SliderWeight(rule, false, 0.0f, 0.5f) - 0.30f) < 1e-6f);
        // The default is the big end, so a caller with no character behaves
        // exactly as it did before weight existed.
        CHECK(std::fabs(BMD::SliderWeight(rule, false, 0.0f) - 0.40f) < 1e-6f);
        // The named half, which the editor walks before the value ever
        // reaches a rule.
        CHECK(std::fabs(BMD::ValueAtWeight(20.0f, 40.0f, 0.0f) - 0.20f) < 1e-6f);
        CHECK(std::fabs(BMD::ValueAtWeight(20.0f, 40.0f, 1.0f) - 0.40f) < 1e-6f);
        CHECK(std::fabs(BMD::ValueAtWeight(20.0f, 40.0f, 0.25f) - 0.25f) < 1e-6f);
        rule.small = 0.0f;
        // ⚠ zap AND uv SLIDERS ARE NOT SHAPE. A zap deletes geometry and a uv
        // slider moves texture coordinates; either one fed through the
        // position arithmetic corrupts the mesh.
        BMD::SliderRule plain;
        plain.dataName = "Breasts";
        CHECK(BMD::SliderMovesVertices(plain));
        BMD::SliderRule zap = plain;  zap.zap = true;
        BMD::SliderRule uv  = plain;  uv.uv   = true;
        BMD::SliderRule none;
        CHECK(!BMD::SliderMovesVertices(zap));
        CHECK(!BMD::SliderMovesVertices(uv));
        CHECK(!BMD::SliderMovesVertices(none));  // names no data at all

        // Folding sliders into one displacement per vertex.
        BMD::DeltaSets two;
        two["A"].indices = { 0, 2 };
        two["A"].deltas  = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        two["B"].indices = { 2, 99 };  // 99 is past the end on purpose
        two["B"].deltas  = { { 0.0f, 0.0f, 4.0f }, { 9.0f, 9.0f, 9.0f } };

        BMD::SliderRules rules;
        rules["Wide"].dataName = "A";
        rules["Tall"].dataName = "B";
        BMD::SliderValues vals;
        vals["Wide"] = 0.5f;
        vals["Tall"] = 1.0f;

        const auto field = BMD::Accumulate(two, rules, vals, 4);
        CHECK(field.size() == 4);
        CHECK(std::fabs(field[0][0] - 0.5f) < 1e-6f);   // half of A's first
        CHECK(std::fabs(field[1][0]) < 1e-6f);          // untouched by anything
        // ⚠ SLIDERS ACCUMULATE ON A SHARED VERTEX rather than the last one
        // winning: vertex 2 is moved by A in y and by B in z.
        CHECK(std::fabs(field[2][1] - 0.5f) < 1e-6f);
        CHECK(std::fabs(field[2][2] - 4.0f) < 1e-6f);
        // ⚠ AND AN INDEX PAST THE MESH IS DROPPED, not grown into. The .osd is
        // third-party content and the mesh bounds it, never the other way
        // round.
        CHECK(field.size() == 4);

        // A zap or uv slider contributes nothing even with a value set.
        BMD::SliderRules zapRules;
        zapRules["Wide"]          = rules["Wide"];
        zapRules["Wide"].zap      = true;
        const auto zapped         = BMD::Accumulate(two, zapRules, vals, 4);
        CHECK(std::fabs(zapped[0][0]) < 1e-6f);

        // ⚠ AND AN UNNAMED SLIDER STILL MOVES, on the .osp's own default. This
        // is the one that is silent: reading it as zero gives a collapsed body
        // rather than an error.
        BMD::SliderRules deflt;
        deflt["Wide"]          = rules["Wide"];
        deflt["Wide"].big      = 100.0f;
        const auto unnamed     = BMD::Accumulate(two, deflt, BMD::SliderValues{}, 4);
        CHECK(std::fabs(unnamed[0][0] - 1.0f) < 1e-6f);

        // ---- the card's identity, which is what reaches the disk key -------
        BMD::SliderValues a;
        a["Waist"] = 0.25f;
        a["Bust"]  = 0.80f;
        BMD::SliderValues b;  // same sliders, inserted the other way round
        b["Bust"]  = 0.80f;
        b["Waist"] = 0.25f;
        // ⚠⚠ ORDER-INDEPENDENT, AND THIS IS THE ONE THAT WOULD HAVE LOOKED
        // FINE. The values arrive in an unordered map whose iteration order is
        // not stable across runs, so folding them as they come would hand back
        // a different key every session, rebuild the whole pane forever, and
        // never once look wrong on screen.
        CHECK(BMD::CardIdentity("Curvy", a) == BMD::CardIdentity("Curvy", b));
        // ⚠⚠ AND THE VALUES ARE IN IT, not just the name. A preset edited in
        // Body Studio keeps its name, so a name-only identity would leave the
        // old picture on disk and the card would go stale in silence.
        BMD::SliderValues edited = a;
        edited["Waist"]          = 0.30f;
        CHECK(BMD::CardIdentity("Curvy", edited) != BMD::CardIdentity("Curvy", a));
        // Two presets are two cards.
        CHECK(BMD::CardIdentity("Curvy", a) != BMD::CardIdentity("Slim", a));
        // A change too small to quantise is the same shape and the same card.
        BMD::SliderValues nudged = a;
        nudged["Waist"]          = 0.25f + 1e-6f;
        CHECK(BMD::CardIdentity("Curvy", nudged) == BMD::CardIdentity("Curvy", a));
        // An empty preset still has an identity rather than an empty string.
        CHECK(!BMD::CardIdentity("Empty", BMD::SliderValues{}).empty());
        // ⚠⚠ TWO WEIGHTS ARE TWO PICTURES, so they must be two keys. The
        // sliders a preset never names still move with weight and they are
        // invisible to the values map, so a key built from the values alone
        // would hand one weight's photograph back for another's.
        CHECK(BMD::CardIdentity("Curvy", a, 0.0f) != BMD::CardIdentity("Curvy", a, 1.0f));
        CHECK(BMD::CardIdentity("Curvy", a, 0.5f) == BMD::CardIdentity("Curvy", a, 0.5f));
        // ⚠⚠ THE SNAP IS ON THE INPUT, so two weights inside one step are one
        // shape AND one key. Keying coarse while rendering exact would make
        // the key stop describing the picture, and the first card to land in
        // a step would silently lend its photograph to every other weight in
        // it. The ends must not drift: a character at 0 or 100 is drawn there.
        CHECK(std::fabs(BMD::SnapWeight(0.0f) - 0.0f) < 1e-6f);
        CHECK(std::fabs(BMD::SnapWeight(1.0f) - 1.0f) < 1e-6f);
        CHECK(std::fabs(BMD::SnapWeight(0.34f) - 0.35f) < 1e-6f);
        CHECK(std::fabs(BMD::SnapWeight(0.36f) - 0.35f) < 1e-6f);
        // Out of range cannot escape the range.
        CHECK(std::fabs(BMD::SnapWeight(-3.0f) - 0.0f) < 1e-6f);
        CHECK(std::fabs(BMD::SnapWeight(9.0f) - 1.0f) < 1e-6f);
        // The point of it: a nudge inside a step reuses the card.
        CHECK(BMD::CardIdentity("Curvy", a, BMD::SnapWeight(0.34f)) ==
              BMD::CardIdentity("Curvy", a, BMD::SnapWeight(0.36f)));
        CHECK(BMD::CardIdentity("Curvy", a, BMD::SnapWeight(0.34f)) !=
              BMD::CardIdentity("Curvy", a, BMD::SnapWeight(0.44f)));
    }

    {  // a body key turned back into a loadable path
        using OS::BodyMeshKey;
        using OS::BodyMeshModelPath;

        // ⚠ GenWeights DECIDES THE SUFFIX and it is a rule, not a guess: a set
        // that declares it builds the _0 / _1 pair and never a bare .nif, and
        // one that does not builds the bare file and never the pair. Measured
        // across all 12 distinct output meshes on the reference load order
        // 2026-08-10, with no mesh existing in both forms.
        CHECK(BodyMeshModelPath("!ube\\body\\femalebody_tangent", true) ==
              "!ube\\body\\femalebody_tangent_1.nif");
        CHECK(BodyMeshModelPath("morten\\lingas\\linga7", false) ==
              "morten\\lingas\\linga7.nif");
        // An unknown mesh stays unknown rather than becoming ".nif".
        CHECK(BodyMeshModelPath("", true).empty());
        // ⚠ AND IT ROUND-TRIPS WITH THE KEY, which is the property that makes
        // a card's path and the character's own body comparable: the key
        // strips meshes\, the extension and the weight, and this puts back
        // exactly what a scene needs to load.
        const auto key = BodyMeshKey("meshes\\!UBE\\Body\\femalebody_tangent_1.nif");
        CHECK(key == "!ube\\body\\femalebody_tangent");
        CHECK(BodyMeshModelPath(key, true) == "!ube\\body\\femalebody_tangent_1.nif");
    }

    {  // the hair skeleton alignment (OS-204)
        using PreviewFilter::IsHeadBoneName;
        using PreviewFilter::ShouldAlignToHead;
        using PreviewGrid::IsAlignedKind;
        using PreviewGrid::SceneKind;

        // The node both a hair NIF and a head mesh carry, folded by the caller.
        CHECK(IsHeadBoneName("npc head [head]"));
        CHECK(IsHeadBoneName("npc head"));
        // ⚠ A PREFIX, NEVER A SUBSTRING. "NPC Head MagicNode" is a real child
        // of the head bone and would be a different anchor; matching loosely
        // elsewhere in the name would take the first thing it stumbled on.
        CHECK(!IsHeadBoneName("npc l hand [lhnd]"));
        CHECK(!IsHeadBoneName("npc spine2 [spn2]"));
        CHECK(!IsHeadBoneName("hairline"));
        CHECK(!IsHeadBoneName(""));
        // ⚠ AND THIS IS THE ONE THAT SEPARATES A PREFIX FROM A SUBSTRING. A
        // node that merely CONTAINS the bone's name is not the bone, and a
        // loose match would anchor the whole scene to whatever it found first.
        CHECK(!IsHeadBoneName("hair over npc head cap"));

        // ⚠⚠ THE MEASURED CASE, and the reason the epsilon can be generous.
        // KS Hairdo's ships two authoring skeletons: the standard one puts the
        // head bone at (0, -1.548, 120.344), which is where this rig's head
        // mesh puts it, and 264 files put it at (0, +0.997, 117.056).
        const float dx = 0.0f - 0.0f;
        const float dy = -1.548f - 0.997f;   // -2.545
        const float dz = 120.344f - 117.056f;  // +3.288
        CHECK(ShouldAlignToHead(dx, dy, dz));
        // ⚠⚠ AND THE CASE THAT MUST NOT MOVE, which is 2277 of the same 2541
        // files. A correctly authored hair measures exactly zero, so the whole
        // catalog keeps byte-identical geometry and only the low ones move.
        CHECK(!ShouldAlignToHead(0.0f, 0.0f, 0.0f));
        // Float noise is not a skeleton difference.
        CHECK(!ShouldAlignToHead(0.0f, 0.001f, -0.002f));
        // Any single axis on its own still counts.
        CHECK(ShouldAlignToHead(0.0f, 0.0f, 3.288f));
        CHECK(ShouldAlignToHead(0.0f, -2.545f, 0.0f));

        // ⚠⚠ EYES ARE IN NOW, AND THIS CHECK USED TO ASSERT THE OPPOSITE.
        // It was written when the reason was "their framing is settled and
        // field-approved", which was a statement about the WINDOW and was read
        // as one about the PLACEMENT. Measured 2026-08-15: eight of forty eye
        // cards put their mesh at z 2.57..4.04 against a window covering
        // 121.5..123.8, because the separate faceparts/eyes*left.nif and
        // eyes*right.nif carry no skeleton and are authored in head space. They
        // rendered at the ankles and their cards were blank.
        //
        // This line earned its keep on the way in rather than on the way out:
        // it failed the build the moment the gate widened, which is exactly
        // what a pinned decision is for. It is being changed deliberately, not
        // deleted because it was in the way.
        CHECK(IsAlignedKind(PreviewGrid::SceneKind::kHair));
        CHECK(IsAlignedKind(PreviewGrid::SceneKind::kFacialHair));
        CHECK(IsAlignedKind(PreviewGrid::SceneKind::kEyes));
        // Brows carry no mannequin to align against; gear is skinned to the
        // body rather than parented to the skull. Both still stay out.
        CHECK(!IsAlignedKind(PreviewGrid::SceneKind::kBrows));
        CHECK(!IsAlignedKind(PreviewGrid::SceneKind::kGear));
    }

    {  // ⚠⚠ THE POINT OF THE VIEWS: every card in a slot frames identically
        using PreviewFraming::Crop;
        using PreviewFraming::WindowFor;

        const std::array<float, 3> bMin{ -20.0f, -10.0f, 0.0f };
        const std::array<float, 3> bMax{ 20.0f, 10.0f, 200.0f };
        // No head in this block: these are the figure-anchored windows, and
        // the head only ever answers for the eye (the block below).
        const PreviewFraming::Box kNoHead{};
        const PreviewFraming::Box kNoItem{};

        // The window does not depend on the item AT ALL, which is the ask:
        // "each card has the mannequin in the same position".
        const auto head = WindowFor(bMin, bMax, Crop::kHead, kNoHead, kNoItem);
        CHECK(std::fabs(head.first[2] - 159.8f) < 1e-3f);   // 0.799
        // ⚠ WELL PAST THE CROWN, because a helmet is taller than the head it
        // covers and a window ending at the skull decapitates every crest.
        CHECK(head.second[2] > bMax[2]);
        CHECK(std::fabs(head.second[2] - 207.8f) < 1e-3f);  // 1.039
        // ⚠⚠ AND IT IS CENTRED ON THE HEAD, which is the defect the numbers
        // above were changed to fix and the one a future tune can undo without
        // noticing. On this rig the head runs f 0.84075 to 0.99928 of the
        // reference box, centre 0.92001; the window that shipped centred on
        // 0.965 and put the head an eighth of a card low with the jaw cut off.
        // Measured in fractions so it survives any body size.
        const float headCentre = ((head.first[2] + head.second[2]) * 0.5f) /
                                 (bMax[2] - bMin[2]);
        CHECK(std::fabs(headCentre - 0.92001f) < 0.005f);
        CHECK(std::fabs(head.first[0] + 5.4f) < 1e-3f);     // centred, 0.135 each way
        CHECK(std::fabs(head.second[0] - 5.4f) < 1e-3f);
        // Depth is untouched by default: an item stands proud of the body.
        CHECK(head.first[1] == bMin[1] && head.second[1] == bMax[1]);

        // Each view is tighter than the figure and they sit where they say.
        const auto eye   = WindowFor(bMin, bMax, Crop::kEye, kNoHead, kNoItem);
        const auto chest = WindowFor(bMin, bMax, Crop::kChest, kNoHead, kNoItem);
        const auto hand  = WindowFor(bMin, bMax, Crop::kHand, kNoHead, kNoItem);
        const auto feet  = WindowFor(bMin, bMax, Crop::kFeet, kNoHead, kNoItem);
        CHECK(eye.second[2] < head.second[2] && eye.first[2] > chest.second[2]);
        CHECK(chest.first[2] > hand.second[2]);
        CHECK(hand.first[2] > feet.second[2]);
        // ⚠ THE HAND WINDOW MUST CONTAIN THE HANDS, and the cut before this one
        // did not: the hands hang at f 0.45761 to 0.59469 and the window's TOP
        // edge was 0.490, below the centre of what it was framing, so most of
        // the hand sat above the frame. Both edges, not just the centre,
        // because a window can straddle a centre and still cut both ends.
        CHECK((hand.first[2] / (bMax[2] - bMin[2])) < 0.45761f);
        CHECK((hand.second[2] / (bMax[2] - bMin[2])) > 0.59469f);
        // The eye and the hand are off centre, on the same side, so the card
        // shows ONE of a pair rather than both at a distance.
        CHECK(eye.first[0] > 0.0f);
        CHECK(hand.first[0] > 0.0f);
        // The eye is the tightest window there is, and tight enough that an
        // eyeball of about a fiftieth of the body fills a good part of it:
        // the field asked for 40% and the span is what delivers it.
        CHECK((eye.second[0] - eye.first[0]) < (chest.second[0] - chest.first[0]));
        CHECK((eye.second[2] - eye.first[2]) < 6.0f);  // of a 200-unit body
        // ⚠⚠ AND IT MUST NOT GO MUCH TIGHTER, which is the opposite of what
        // the eye window has been told three times. On this rig the window is
        // 2.41 units and ONE EYE IS 2.39, so the window already IS the eye and
        // shrinking it crops the iris rather than filling more card. The field
        // asked for 70% closer and the answer is a depth clip, not this band:
        // the camera stands at maxDepth + halfZ/tanHalf, WindowFor leaves the
        // depth whole, and half the body's front-to-back extent is what holds
        // it back. 2.0 of a 200-unit body is 1.3 units, comfortably inside one
        // eye, so this catches a tightening that would start cutting.
        CHECK((eye.second[2] - eye.first[2]) > 2.0f);
        // ⚠⚠ THE EYE'S ZOOM COMES FROM THE FRONT FACE, and this is the pin
        // that says so. The camera stands off the window's maximum y, so the
        // eye is the one view that pulls its front in: on this rig the body's
        // front is the bust and the eye sits four units behind it, which held
        // the camera off and capped the eye at 41% of the card.
        CHECK(eye.second[1] < bMax[1]);
        CHECK(eye.first[1] == bMin[1]);  // the BACK is left alone; it buys nothing
        // Every other view keeps the whole depth, so nothing else moved.
        CHECK(chest.second[1] == bMax[1] && hand.second[1] == bMax[1]);
        CHECK(feet.second[1] == bMax[1]);

        // ⚠ THE THREE CLOSE-UPS THE FIELD ASKED FOR (2026-08-10). Each is
        // pinned as a RELATIONSHIP to the window it was cut out of, not as a
        // literal, so a later tune can move them together without silently
        // undoing what they were for.
        const auto beard = WindowFor(bMin, bMax, Crop::kFacialHair, kNoHead, kNoItem);
        const auto ring  = WindowFor(bMin, bMax, Crop::kRing, kNoHead, kNoItem);
        const auto span  = [](const auto& w) { return w.second[2] - w.first[2]; };

        // A beard is tighter than the head and sits LOWER on it: the head
        // window's own centre would cut a beard off at the bottom edge.
        CHECK(span(beard) < span(head));
        CHECK(beard.second[2] < head.second[2]);
        CHECK(beard.first[2] < (head.first[2] + head.second[2]) * 0.5f);
        // A ring is tighter than the hand and lies INSIDE it, because it is
        // the same hand seen closer rather than a different part of the body.
        CHECK(span(ring) < span(hand));
        CHECK(ring.first[2] >= hand.first[2] && ring.second[2] <= hand.second[2]);
        // ⚠⚠ THE AMULET WINDOW IS CENTRED ON THE AMULET, measured off the
        // meshes rather than tuned: the amulets on this rig centre at f 0.812
        // to 0.824 of the reference box, and the window that shipped centred
        // on 0.785 with nearly half the card empty chest BELOW the pendant.
        const float chestCentre =
            ((chest.first[2] + chest.second[2]) * 0.5f) / (bMax[2] - bMin[2]);
        CHECK(std::fabs(chestCentre - 0.818f) < 0.015f);
        // ⚠ AND x MUST STAY UNDER z, or the framing fits the x half-extent
        // instead and every tightening of the band does nothing at all.
        CHECK((chest.second[0] - chest.first[0]) < span(chest));
        // The other two close-ups keep the whole depth: only the eye was ever
        // held back by the bust, and only the eye pays the risk of a near front
        // face.
        CHECK(beard.second[1] == bMax[1] && ring.second[1] == bMax[1]);
        // ⚠ AND THE FEET WINDOW REACHES BELOW THE FLOOR, which is not a margin:
        // a high heel is authored with the foot lifted and its sole reaching
        // under the ground a flat-footed body stands on, so a window that
        // stopped at the floor clipped the heel off the boots this view exists
        // for.
        CHECK(feet.first[2] < bMin[2]);

        // A taller body lines up with a shorter one, because the fractions are
        // of each body's own extent: that is what makes two races' cards match.
        const std::array<float, 3> tallMax{ 20.0f, 10.0f, 400.0f };
        const auto tall = WindowFor(bMin, tallMax, Crop::kHead, kNoHead, kNoItem);
        CHECK(std::fabs((tall.first[2] / 400.0f) - (head.first[2] / 200.0f)) < 1e-4f);

        // kWhole and a degenerate body both hand the body back untouched.
        const auto whole = WindowFor(bMin, bMax, Crop::kWhole, kNoHead, kNoItem);
        CHECK(whole.first == bMin && whole.second == bMax);
        const std::array<float, 3> flat{ 20.0f, 10.0f, 0.0f };
        const auto flatWin = WindowFor(bMin, flat, Crop::kHead, kNoHead, kNoItem);
        CHECK(flatWin.first == bMin && flatWin.second == flat);
    }

    {  // ⚠⚠ OS-205: THE EYE'S WINDOW HANGS FROM THE EYE'S OWN TOP
        using PreviewFraming::Box;
        using PreviewFraming::Crop;
        using PreviewFraming::ReferenceBody;
        using PreviewFraming::WindowFor;

        // ⚠⚠ EVERY NUMBER HERE IS MEASURED, and between them they killed
        // three anchors. The boxes are the field's own (`preview.boxes crop=2`,
        // 2026-08-18 03:20, two mannequins, 28 cards each); the eye centres come
        // from clustering the meshes' vertices by side rather than boxing them.
        //
        //   rig A: head 109.86..131.69, LDD eye z 122.915..124.385, one eye at
        //          x 2.609 of a pair reaching 4.080 (f 0.6395)
        //   rig B: head 110.63..131.48, UBE eye z 122.113..124.501, one eye at
        //          x 2.351 of a pair reaching 3.545 (f 0.6632)
        //
        // The two eye meshes' tops are 0.133 apart and their BOTTOMS are 0.819
        // apart, which is the whole reason the top is the anchor.
        const Box headA{ { -6.17f, -6.59f, 109.86f }, { 6.17f, 9.68f, 131.69f }, true };
        const Box eyeA{ { -4.080f, 5.84f, 122.915f }, { 4.080f, 7.63f, 124.385f }, true };
        const Box headB{ { -6.20f, -6.31f, 110.63f }, { 6.20f, 9.52f, 131.48f }, true };
        const Box eyeB{ { -3.545f, 5.00f, 122.113f }, { 3.545f, 7.11f, 124.501f }, true };
        const auto bodyA = ReferenceBody({ -32.25f, -11.69f, 11.43f },
                                         { 32.25f, 11.22f, 113.98f });
        const auto bodyB = ReferenceBody({ -31.67f, -12.43f, 11.19f },
                                         { 31.67f, 10.88f, 114.48f });

        const auto winA = WindowFor(bodyA.first, bodyA.second, Crop::kEye, headA, eyeA);
        const auto winB = WindowFor(bodyB.first, bodyB.second, Crop::kEye, headB, eyeB);
        const auto span = [](const auto& w) { return w.second[2] - w.first[2]; };

        // ⚠⚠ EACH WINDOW SITS ON ITS OWN MESH'S BOX CENTRE, which is the
        // PUPIL AXIS on a globe and the middle of the aperture on a patch. The
        // UBE mesh's front-most vertices, which is where an eyeball looks, put
        // its pupil at z 123.307 and its box centre is 123.307 to three places.
        // Its box TOP is 0.49 units higher, all of it crown behind the lid, and
        // hanging the window there is what the field saw as "slightly too low".
        for (const auto& pair : { std::pair{ winA, std::pair{ headA, eyeA } },
                                  std::pair{ winB, std::pair{ headB, eyeB } } }) {
            const auto& w    = pair.first;
            const auto& head = pair.second.first;
            const auto& eye  = pair.second.second;
            const float tall = (head.max[2] - head.min[2]) *
                               PreviewFraming::kEyeHeightOfHead;
            CHECK(std::fabs((w.first[2] + w.second[2]) * 0.5f -
                            (eye.min[2] + eye.max[2]) * 0.5f) < 1e-3f);
            // And the frame is the same multiple of the head's own eye on both,
            // so two characters' cards read as the same card.
            CHECK(std::fabs(tall / span(w) - PreviewFraming::kEyeFill) < 1e-3f);
        }
        // The rig the field called fine is unmoved by this: on a mesh that is
        // the aperture itself, the box centre and the hung-from-the-top answer
        // are the same window.
        CHECK(std::fabs(winA.first[2] - 122.671f) < 1e-2f);
        CHECK(std::fabs(winA.second[2] - 124.630f) < 1e-2f);

        // ⚠⚠ THE SIZE COMES FROM THE HEAD, NOT FROM THE ITEM'S BOX. Rig
        // B's mesh is 62% taller than rig A's and its card is not 62% wider: a
        // window filled with the box is what made the field call one of them
        // "much smaller".
        CHECK(std::fabs(span(winA) - span(winB)) < 0.15f);
        CHECK((eyeB.max[2] - eyeB.min[2]) > (eyeA.max[2] - eyeA.min[2]) * 1.5f);

        // ⚠⚠ x LANDS ON THE EYE, WITHIN A TWENTIETH OF A UNIT. The
        // camera looks down -Y with +Z up, so screen-right is +X and a window
        // centred inboard of the eye pushes the eye right, which is what came
        // back from the field on the 0.52 share.
        const auto xCentre = [](const auto& w) { return (w.first[0] + w.second[0]) * 0.5f; };
        CHECK(std::fabs(xCentre(winA) - 2.609f) < 0.05f);
        CHECK(std::fabs(xCentre(winB) - 2.351f) < 0.05f);
        CHECK(xCentre(winA) > 0.0f && xCentre(winB) > 0.0f);  // one eye, not the pair

        // ⚠⚠ x MUST NEVER DECIDE THE FRAME. The framing fits whichever
        // half-extent is larger, so an x window wider than the z one takes over
        // the camera distance and every change to the band does nothing.
        CHECK((winA.second[0] - winA.first[0]) < span(winA));
        CHECK((winB.second[0] - winB.first[0]) < span(winB));

        // The camera's plane stands in FRONT of the eye on both rigs. On the
        // body anchor it sat at y 5.18 against an eye reaching 7.63, which is
        // inside the thing being photographed.
        CHECK(winA.second[1] > eyeA.max[1] && winB.second[1] > eyeB.max[1]);

        // ⚠ AN "EYE" THE SIZE OF A FACE IS NOT AN EYE, and the head fraction
        // catches it rather than the window framing whatever arrived.
        const Box huge{ { -6.0f, -6.0f, 112.0f }, { 6.0f, 9.0f, 128.0f }, true };
        const auto fallback = WindowFor(bodyA.first, bodyA.second, Crop::kEye, headA, huge);
        CHECK(std::fabs(fallback.first[2] -
                        (headA.min[2] + (headA.max[2] - headA.min[2]) *
                                            PreviewFraming::kEyeHeadView.zLo)) < 1e-3f);

        // With no item at all the head fraction answers, and with neither the
        // figure does, which is the window this shipped with for a year.
        const Box none{};
        const auto headOnly = WindowFor(bodyA.first, bodyA.second, Crop::kEye, headA, none);
        CHECK(std::fabs(headOnly.first[2] - fallback.first[2]) < 1e-4f);
        const auto figure = WindowFor(bodyB.first, bodyB.second, Crop::kEye, none, none);
        CHECK(std::fabs(figure.first[2] - 122.017f) < 0.01f);

        // With an item but no head the item's own box has to answer for the
        // eye's height, because nothing else can.
        const auto itemOnly = WindowFor(bodyA.first, bodyA.second, Crop::kEye, none, eyeA);
        CHECK(std::fabs((eyeA.max[2] - eyeA.min[2]) / span(itemOnly) -
                        PreviewFraming::kEyeFill) < 1e-3f);

        // Only the eye moved. Every other window reads the figure whatever the
        // head and the item say, which is what keeps this to the one card the
        // report was about.
        for (const auto crop : { Crop::kHead, Crop::kFacialHair, Crop::kChest,
                                 Crop::kHand, Crop::kRing, Crop::kFeet }) {
            const auto withBoxes = WindowFor(bodyA.first, bodyA.second, crop, headA, eyeA);
            const auto without   = WindowFor(bodyA.first, bodyA.second, crop, none, none);
            CHECK(withBoxes.first[2] == without.first[2]);
            CHECK(withBoxes.second[2] == without.second[2]);
        }
    }

    {  // a slot nobody wrote down picks its view by where the item sits
        using PreviewFraming::Crop;
        using PreviewFraming::CropByHeight;

        CHECK(CropByHeight(0.0f, 200.0f, 176.0f, 196.0f) == Crop::kHead);
        CHECK(CropByHeight(0.0f, 200.0f, 140.0f, 156.0f) == Crop::kChest);
        CHECK(CropByHeight(0.0f, 200.0f, 0.0f, 30.0f) == Crop::kFeet);
        // Something spanning the figure IS the figure.
        CHECK(CropByHeight(0.0f, 200.0f, 20.0f, 180.0f) == Crop::kWhole);
        // A degenerate body cannot answer, and says so.
        CHECK(CropByHeight(0.0f, 0.0f, 0.0f, 1.0f) == Crop::kWhole);
    }

    {  // ⚠⚠ THE REFERENCE BOX IS THE SAME BOX HOWEVER MANY PARTS WENT
        using PreviewFraming::ReferenceBody;

        // This rig's measured mannequin, in its own space (2026-08-10):
        //   feet 0.0424..11.4121   body 11.1875..114.4760   head 110.6280..131.4800
        // The body is the only part nothing can remove, so it is the input,
        // and both ends extrapolate back to the standing figure.
        const std::array<float, 3> bodyMin{ -20.0f, -10.0f, 11.1875f };
        const std::array<float, 3> bodyMax{ 20.0f, 10.0f, 114.4760f };
        const auto                 ref = ReferenceBody(bodyMin, bodyMax);
        // The floor lands on the ground the feet stand on, without the feet.
        CHECK(std::fabs(ref.first[2] - 0.0424f) < 0.2f);
        // And the crown lands past the top of the head.
        CHECK(std::fabs(ref.second[2] - 131.4800f) < 0.2f);
        CHECK(ref.second[2] > bodyMax[2]);
        CHECK(ref.first[2] < bodyMin[2]);
        // ⚠ X AND Y ARE THE BODY'S OWN, UNTOUCHED: only the height is
        // extrapolated, because only the height loses parts.
        CHECK(ref.first[0] == bodyMin[0] && ref.second[0] == bodyMax[0]);
        CHECK(ref.first[1] == bodyMin[1] && ref.second[1] == bodyMax[1]);
        // ⚠⚠ THE REGRESSION THAT REACHED THE FIELD. Footwear drops the feet,
        // so a boots card's mannequin box is the body's alone; before this the
        // feet were measured, the floor jumped from the ankle to the hips on
        // exactly those cards, and three knee-high boots came back cut off at
        // the bottom. Feeding the body box is now the ONLY thing that can
        // happen, so the same body answers the same box every time.
        const auto again = ReferenceBody(bodyMin, bodyMax);
        CHECK(again.first[2] == ref.first[2] && again.second[2] == ref.second[2]);
        // Proportional, so a taller race lines up with a shorter one.
        const std::array<float, 3> tallMin{ -20.0f, -10.0f, 22.375f };
        const std::array<float, 3> tallMax{ 20.0f, 10.0f, 228.952f };
        const auto                 tall = ReferenceBody(tallMin, tallMax);
        CHECK(std::fabs((tall.second[2] - tall.first[2]) -
                        2.0f * (ref.second[2] - ref.first[2])) < 0.5f);
        // A degenerate body hands itself back rather than dividing.
        const std::array<float, 3> flatMax{ 20.0f, 10.0f, 11.1875f };
        const auto                 flat = ReferenceBody(bodyMin, flatMax);
        CHECK(flat.first == bodyMin && flat.second == flatMax);
    }

    {  // ⚠ the count is CONTEXT: it must not reach the key
        PreviewGrid::SceneIdentity bare;
        bare.plugin      = "skyrim.esm";
        bare.localFormId = 0x0001397Du;
        bare.modelPaths  = { "armor/elven/f/cuirass_1.nif" };

        auto counted               = bare;
        counted.mannequinPathCount = 1;
        CHECK(PreviewGrid::DiskKeyFor(bare) == PreviewGrid::DiskKeyFor(counted));

        // ⚠ The TAG is identity, though, and an empty one adds no bytes. That
        // pairing is what lets a mannequin fix rebuild the cards carrying a
        // mannequin without touching a single weapon thumbnail.
        auto tagged         = bare;
        tagged.mannequinTag = "m2";
        CHECK(PreviewGrid::DiskKeyFor(tagged) != PreviewGrid::DiskKeyFor(bare));
        auto bumped         = bare;
        bumped.mannequinTag = "m3";
        CHECK(PreviewGrid::DiskKeyFor(bumped) != PreviewGrid::DiskKeyFor(tagged));
        auto empty         = bare;
        empty.mannequinTag = "";
        CHECK(PreviewGrid::DiskKeyFor(empty) == PreviewGrid::DiskKeyFor(bare));

        // And the paths themselves are what re-key a card, which is the whole
        // reason the renderer version does not move for this feature.
        auto composed       = bare;
        composed.modelPaths = { "!ube/body/femalebody_tangent_1.nif",
                                "armor/elven/f/cuirass_1.nif" };
        composed.mannequinPathCount = 1;
        CHECK(PreviewGrid::DiskKeyFor(composed) != PreviewGrid::DiskKeyFor(bare));
    }

    {  // the mannequin's pose arithmetic (OS-204 phase 2), still wired to nothing
        namespace MP = OS::MannequinPose;

        const auto close = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        const auto isIdentity = [&](const MP::Xform& x) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if (!close(x.rot.m[r][c], r == c ? 1.0f : 0.0f)) {
                        return false;
                    }
                }
            }
            return close(x.trans.x, 0.0f) && close(x.trans.y, 0.0f) &&
                   close(x.trans.z, 0.0f) && close(x.scale, 1.0f);
        };

        // A bind transform with a rotation, an offset and a scale, standing in
        // for a real bone's.
        MP::Xform bone;
        bone.rot   = MP::AxisAngle({ 0.3f, 1.0f, -0.2f }, 0.7f);
        bone.trans = { 12.0f, -4.0f, 96.0f };
        bone.scale = 1.0f;

        // Inverse and Multiply, pinned together: a transform composed with its
        // own inverse is the identity, both ways round.
        CHECK(isIdentity(MP::Multiply(MP::Inverse(bone), bone)));
        CHECK(isIdentity(MP::Multiply(bone, MP::Inverse(bone))));

        // THE CORRECTNESS PIN FOR THE WHOLE FEATURE. skinToBone is the bind
        // INVERSE, so with no authored joints every skinning matrix must come
        // out identity and the posed mesh must be byte-identical to today's.
        const std::vector<std::string> names{ "NPC L UpperArm [LUar]",
                                              "NPC L Forearm [LLar]",
                                              "NPC Spine2 [Spn2]" };
        std::vector<MP::Xform>         skinToBone;
        for (std::size_t i = 0; i < names.size(); ++i) {
            MP::Xform b;
            b.rot   = MP::AxisAngle({ 1.0f, static_cast<float>(i) * 0.4f, 0.5f },
                                    0.3f * static_cast<float>(i + 1));
            b.trans = { 10.0f * static_cast<float>(i), 2.0f,
                        90.0f + 6.0f * static_cast<float>(i) };
            skinToBone.push_back(MP::Inverse(b));  // the file stores the inverse
        }
        const std::vector<MP::Joint> noJoints;
        const auto rest = MP::SkinningMatrices(names, skinToBone, noJoints);
        CHECK(rest.size() == names.size());
        for (const auto& x : rest) {
            CHECK(isIdentity(x));
        }
        // And a vertex under identity matrices does not move.
        const MP::Vec3                   v{ 3.0f, -7.0f, 101.0f };
        const std::vector<std::uint32_t> idx{ 0, 1 };
        const std::vector<float>         w{ 0.5f, 0.5f };
        const auto                       same = MP::SkinPoint(v, rest, idx, w);
        CHECK(close(same.x, v.x) && close(same.y, v.y) && close(same.z, v.z));

        // Weights that do not sum to one must not shrink the mesh toward the
        // origin: authored meshes routinely sum to 0.98 or 1.02.
        const std::vector<float> unnormalised{ 0.45f, 0.45f };
        const auto               still = MP::SkinPoint(v, rest, idx, unnormalised);
        CHECK(close(still.x, v.x) && close(still.y, v.y) && close(still.z, v.z));
        // An unweighted vertex stays where it was authored rather than
        // collapsing to the origin.
        const std::vector<float> none{ 0.0f, 0.0f };
        const auto               kept = MP::SkinPoint(v, rest, idx, none);
        CHECK(close(kept.x, v.x) && close(kept.z, v.z));

        // A joint moves its own bone and everything under it, and nothing
        // above it: the forearm follows the upper arm, the spine does not.
        const std::vector<MP::Joint> one{
            { "NPC L UpperArm [LUar]", { 0.0f, 1.0f, 0.0f }, 20.0f }
        };
        const auto posed = MP::SkinningMatrices(names, skinToBone, one);
        CHECK(!isIdentity(posed[0]));  // the upper arm
        CHECK(!isIdentity(posed[1]));  // the forearm, carried by it
        CHECK(isIdentity(posed[2]));   // the spine, above it, untouched

        // A joint rotates about its own BIND position, so the pivot bone's own
        // origin is a fixed point of its rotation. Get this wrong and the arm
        // swings from the world origin, which is between the feet.
        const MP::Vec3                   pivot = MP::Inverse(skinToBone[0]).trans;
        const std::vector<std::uint32_t> justArm{ 0 };
        const std::vector<float>         full{ 1.0f };
        const auto stayed = MP::SkinPoint(pivot, posed, justArm, full);
        CHECK(close(stayed.x, pivot.x) && close(stayed.y, pivot.y) &&
              close(stayed.z, pivot.z));

        // The parent map answers, and an unknown bone is its own root rather
        // than somebody else's child.
        CHECK(MP::ParentOf("NPC L Hand [LHnd]") == "NPC L Forearm [LLar]");
        CHECK(MP::ParentOf("NPC Root [Root]").empty());
        CHECK(MP::ParentOf("SomeModAddedBone").empty());
    }

    {  // NameScaleFor: the card's name scales with the card (field 2026-08-11,
       // "the text is too large and cuts off easily depending on card size").
       //
       // ⚠ THE INPUT IS THE FITTED SIDE, NOT THE SETTING. Every case below is
       // written as (side, bodyPx) rather than as a card-scale number, because
       // FittedSide can return more OR less than the setting asked for and the
       // scale has to follow what was actually drawn.
        namespace PG = OS::PreviewGrid;
        const auto  near   = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        const float body   = 20.0f;  // any body height; the RATIO is what counts
        const float refU   = PG::kCardNameRefUnits;
        const float floorS = PG::kCardNameMinScale;

        // The reference size draws the name at full body text - byte-identical
        // to the old behaviour for anyone already running a card that big.
        CHECK(near(PG::NameScaleFor(refU * body, body), 1.0f));

        // Today's fresh-install default is a 4-unit card, which is where the
        // report came from: the name comes down to two thirds.
        CHECK(near(PG::NameScaleFor(4.0f * body, body), 4.0f / refU));

        // The old default of 5 sits between the two, still under full size.
        CHECK(PG::NameScaleFor(5.0f * body, body) < 1.0f);
        CHECK(PG::NameScaleFor(5.0f * body, body) >
              PG::NameScaleFor(4.0f * body, body));

        // A card BIGGER than the reference is capped rather than growing the
        // name past body text, which would be a different kind of wrong.
        CHECK(near(PG::NameScaleFor(14.0f * body, body), 1.0f));
        CHECK(near(PG::NameScaleFor(1000.0f * body, body), 1.0f));

        // FittedSide's own floor is a 2-unit card. The raw ratio there is a
        // third, which is unreadable, so the floor takes over.
        CHECK(near(PG::NameScaleFor(2.0f * body, body), floorS));
        CHECK(near(PG::NameScaleFor(0.5f * body, body), floorS));

        // Monotonic in between: a bigger card never gets a smaller name.
        float prev = 0.0f;
        for (float units = 2.0f; units <= 8.0f; units += 0.25f) {
            const float s = PG::NameScaleFor(units * body, body);
            CHECK(s >= prev - 1e-6f);
            CHECK(s >= floorS);
            CHECK(s <= 1.0f);
            prev = s;
        }

        // The ratio is scale-free: doubling BOTH the card and body text is the
        // same card, so it must give the same answer. This is what makes the
        // formula hold at any UI scale without reading one.
        CHECK(near(PG::NameScaleFor(4.0f * body, body),
                   PG::NameScaleFor(8.0f * body, 2.0f * body)));

        // Degenerate inputs draw exactly as before rather than at the floor: a
        // missing measurement is not evidence of a small card.
        CHECK(near(PG::NameScaleFor(4.0f * body, 0.0f), 1.0f));
        CHECK(near(PG::NameScaleFor(4.0f * body, -1.0f), 1.0f));
        CHECK(near(PG::NameScaleFor(0.0f, body), 1.0f));
        CHECK(near(PG::NameScaleFor(-5.0f, body), 1.0f));
    }

    {  // Per-shape morph selection (the b15 defect). A body NIF is not one
       // shape, and the rule that decides which field a geometry gets is the
       // only decidable part of that fix, so it is pinned here.
        using PreviewFilter::ChooseShapeMorph;
        using PreviewFilter::kNoShapeMorph;

        // The 3BA reference mesh, as its slider set declares it.
        const std::string_view                  threeBa[] = { "3ba", "3ba_anus",
                                                              "3ba_vagina" };
        const std::span<const std::string_view> set{ threeBa };

        // Each geometry takes ITS OWN field, and the two small shapes are the
        // whole point: before this they took the body's 18436-vertex run.
        CHECK(ChooseShapeMorph(set, "3ba").index == 0);
        CHECK(!ChooseShapeMorph(set, "3ba").drop);
        CHECK(ChooseShapeMorph(set, "3ba_anus").index == 1);
        CHECK(ChooseShapeMorph(set, "3ba_vagina").index == 2);

        // ⚠ AND NOT BY PREFIX. "3ba" is a prefix of both of the others, so a
        // startswith rule would give the body's field to all three and look
        // like it worked.
        CHECK(ChooseShapeMorph(set, "3ba_vagina").index != 0);

        // A geometry the set never describes is not drawn.
        CHECK(ChooseShapeMorph(set, "somethingelse").drop);
        CHECK(ChooseShapeMorph(set, "somethingelse").index == kNoShapeMorph);

        // The physics rig goes even when the set DECLARES it, which the HIMBO
        // collision sets do.
        const std::string_view                  himbo[] = { "himbo - body",
                                                            "virtualarms",
                                                            "virtualbelly" };
        const std::span<const std::string_view> col{ himbo };
        CHECK(ChooseShapeMorph(col, "himbo - body").index == 0);
        CHECK(ChooseShapeMorph(col, "virtualarms").drop);
        // ⚠⚠ AND WITH NO SET AT ALL, which is what an ITEM card hands
        // the mannequin: an empty list is no opinion, so this rule cannot be
        // the one that drops a proxy there. MeshExtractor asks the predicate
        // directly in its filter chain, and this is that predicate (OS-205:
        // VirtualFeet reaching z -0.2907 stretched every male card's reference
        // box by 11.6 units).
        CHECK(!ChooseShapeMorph({}, "virtualfeet").drop);
        CHECK(PreviewFilter::IsPhysicsProxyLeaf("virtualfeet"));
        CHECK(PreviewFilter::IsPhysicsProxyLeaf("virtualhands"));
        CHECK(PreviewFilter::IsPhysicsProxyLeaf("virtualground"));
        CHECK(!PreviewFilter::IsPhysicsProxyLeaf("himbo - body"));
        CHECK(!PreviewFilter::IsPhysicsProxyLeaf("3ba"));
        CHECK(ChooseShapeMorph(col, "virtualbelly").drop);
        CHECK(PreviewFilter::IsPhysicsProxyLeaf("virtualbreasts"));
        CHECK(PreviewFilter::IsPhysicsProxyLeaf("virtualground"));
        CHECK(!PreviewFilter::IsPhysicsProxyLeaf("3ba"));
        CHECK(!PreviewFilter::IsPhysicsProxyLeaf("baseshape"));

        // ⚠⚠ AN EMPTY LIST IS "NO OPINION", NOT "DROP EVERYTHING". The head,
        // hands and feet ride their own roots with no shape list, as does
        // every armour and weapon scene. Getting this backwards empties every
        // card in the app rather than just the body ones.
        const std::span<const std::string_view> none{};
        CHECK(!ChooseShapeMorph(none, "3ba").drop);
        CHECK(ChooseShapeMorph(none, "3ba").index == kNoShapeMorph);
        CHECK(!ChooseShapeMorph(none, "femalehead").drop);
        CHECK(!ChooseShapeMorph(none, "virtualarms").drop);
    }

    {  // A delta set is addressed by its FILE and its name. A slider set can
       // draw runs from more than one .osd, and the 3BA nevernude pair really
       // does carry 94 shared run names across its two files with 23 of them
       // DIFFERENT, all on 3BA_Vagina and 3BA_Anus.
        using OS::BodyMorphData::DeltaKey;

        // The same name in two files is two different runs.
        CHECK(DeltaKey("SE 3BBB Body Amazing v2.osd", "3BA_VaginaLabiaNeat_v2") !=
              DeltaKey("CBBE 3BBB Amazing NeverNude.osd", "3BA_VaginaLabiaNeat_v2"));

        // Two names in one file are two different runs, which is the ordinary
        // case and would break if the separator were ever dropped.
        CHECK(DeltaKey("a.osd", "BraArms") != DeltaKey("a.osd", "PantyArms"));

        // ⚠ THE FILE HALF IS CASE-FOLDED, because the key is built once from
        // the .osp's spelling of a <Data> reference and once when the parsed
        // file is filed away, and nothing makes a mod spell it the same way in
        // both places. A mismatch there loses every delta set in that file
        // while every other count in the log stays healthy.
        CHECK(DeltaKey("CBBE Body.osd", "X") == DeltaKey("cbbe body.osd", "X"));
        CHECK(DeltaKey("CBBE BODY.OSD", "X") == DeltaKey("cbbe body.osd", "X"));

        // The SET half is NOT folded: run names are the .osd's own and are
        // matched exactly, so folding them would merge runs that differ.
        CHECK(DeltaKey("a.osd", "BraArms") != DeltaKey("a.osd", "braarms"));
    }

    {  // Which installed set is the COVERED sibling of a nude body set. The
       // candidate lists below are the real set names on this rig.
        using OS::CoveredSets::Candidate;
        using OS::CoveredSets::CoveredSiblingFor;
        const auto F  = OS::BodyFamily::k3BA;
        const auto G  = OS::BodyFamily::kGenericV1;
        const auto H  = OS::BodyFamily::kHIMBO;
        const auto U  = OS::BodyFamily::kUBE;
        const auto fb = std::string_view{ "femalebody" };
        const auto mb = std::string_view{ "malebody" };

        const Candidate all[] = {
            { "CBBE 3BBB Body Amazing", F, "femalebody" },
            { "CBBE 3BBB Amazing NeverNude", F, "femalebody" },
            { "CBBE 3BBB Amazing NeverNude UniBoob", F, "femalebody" },
            { "CBBE Body", G, "femalebody" },
            { "CBBE Body Physics", G, "femalebody" },
            { "CBBE NeverNude", G, "femalebody" },
            { "CBBE NeverNude Physics", G, "femalebody" },
            { "CBBE Underwear", G, "femalebody" },
            { "HIMBO Body - SOS", H, "malebody" },
            { "HIMBO Body - Vanilla (Nevernude)", H, "malebody" },
            { "HIMBO Undies for SOS - Briefs", H, "maleunderwear" },
            { "HIMBO Undies for SOS - Thong", H, "maleunderwear" },
            { "UBE SE 2.0 - Necoco Body", U, "femalebody_tangent" },
        };
        const std::span<const Candidate> sets{ all };

        CHECK(CoveredSiblingFor("CBBE 3BBB Body Amazing", F, fb, sets) ==
              "CBBE 3BBB Amazing NeverNude");   // shortest of the two NeverNudes
        CHECK(CoveredSiblingFor("CBBE Body", G, fb, sets) == "CBBE NeverNude");

        // ⚠ THE HIMBO PAIR IS NOT A NAME MUTATION. Its covered build is a
        // different reference mesh entirely ("Vanilla" against "SOS"), and the
        // Undies sets share the longer literal token. Output file is what
        // rejects them: they build maleunderwear, not a body.
        CHECK(CoveredSiblingFor("HIMBO Body - SOS", H, mb, sets) ==
              "HIMBO Body - Vanilla (Nevernude)");

        // ⚠⚠ UBE HAS NONE, and that is the normal answer for the largest
        // family installed here rather than an error. Empty means "draw
        // unchanged".
        CHECK(CoveredSiblingFor("UBE SE 2.0 - Necoco Body", U,
                                "femalebody_tangent", sets).empty());

        // A set never answers with itself, and a family never crosses.
        CHECK(CoveredSiblingFor("CBBE NeverNude", G, fb, sets) != "CBBE NeverNude");
        CHECK(CoveredSiblingFor("CBBE 3BBB Body Amazing", F, fb, sets)
                  .find("CBBE NeverNude") == std::string_view::npos);

        // No output file means no answer: without it a body set could pair
        // with an underwear-only set that merely shares a family.
        CHECK(CoveredSiblingFor("HIMBO Body - SOS", H, {}, sets).empty());

        // ---- the family fallback, for a set that is not installed ---------
        using OS::CoveredSets::BestCoveredForFamily;
        using OS::CoveredSets::OutputIsBody;

        // `HIMBO` is named by 30 preset entries here and declared by nothing,
        // so there is no sibling to find and the family answers instead.
        CHECK(CoveredSiblingFor("HIMBO", H, {}, sets).empty());
        CHECK(BestCoveredForFamily(H, sets) == "HIMBO Body - Vanilla (Nevernude)");
        CHECK(BestCoveredForFamily(F, sets) == "CBBE 3BBB Amazing NeverNude");
        // Nevernude beats underwear: CBBE ships both and the underwear build
        // adds bow ties and straps.
        CHECK(BestCoveredForFamily(G, sets) == "CBBE NeverNude");

        // ⚠⚠ UBE STILL ANSWERS NOTHING, and the fallback must not invent one.
        // Confirmed by shapes and not just by names: the only UBE body set on
        // this rig that declares anything beside the body declares 3BCA_Arm,
        // 3BCA_Hand, 3BCA_Leg and Blocker, which are physics helpers.
        CHECK(BestCoveredForFamily(U, sets).empty());
        CHECK(BestCoveredForFamily(OS::BodyFamily::kUnknown, sets).empty());

        // ⚠ A GARMENT SET IS NOT A BODY. Five HIMBO presets here name `HIMBO
        // Vanilla - Body - Farm Clothes 1`, which has "Body" in its name and
        // outputs torsom.
        CHECK(OutputIsBody("femalebody"));
        CHECK(OutputIsBody("malebody"));
        CHECK(OutputIsBody("femalebody_tangent"));
        CHECK(!OutputIsBody("torsom"));
        CHECK(!OutputIsBody("maleunderwear"));
        CHECK(!OutputIsBody("Dread_Sovereign_Armor"));
    }

    {  // the mannequin's head consults the anchor on EVERY kind. Vanilla's
       // MaleHeadArgonian.nif has no shape transform (its three beast siblings
       // carry trans z=120.344), so without this its head renders at the origin
       // and lies on the floor by the figure's feet. Field 2026-08-20.
        using OS::PreviewGrid::RootConsultsHeadAnchor;
        for (const auto kind : { PreviewGrid::SceneKind::kHair, PreviewGrid::SceneKind::kFacialHair,
                                 PreviewGrid::SceneKind::kEyes, PreviewGrid::SceneKind::kGear,
                                 PreviewGrid::SceneKind::kBody, PreviewGrid::SceneKind::kSkin }) {
            CHECK(RootConsultsHeadAnchor(true, true, kind));
        }
    }
    {  // the figure's other parts are skinned to the body and never move, on
       // any kind. Moving them would take the whole mannequin apart.
        using OS::PreviewGrid::RootConsultsHeadAnchor;
        CHECK(!RootConsultsHeadAnchor(true, false, PreviewGrid::SceneKind::kHair));
        CHECK(!RootConsultsHeadAnchor(true, false, PreviewGrid::SceneKind::kGear));
    }
    {  // the ITEM's rule is unchanged: it aligns on the aligned kinds only.
        using OS::PreviewGrid::RootConsultsHeadAnchor;
        CHECK(RootConsultsHeadAnchor(false, false, PreviewGrid::SceneKind::kHair));
        CHECK(RootConsultsHeadAnchor(false, false, PreviewGrid::SceneKind::kFacialHair));
        CHECK(RootConsultsHeadAnchor(false, false, PreviewGrid::SceneKind::kEyes));
        CHECK(!RootConsultsHeadAnchor(false, false, PreviewGrid::SceneKind::kGear));
        CHECK(!RootConsultsHeadAnchor(false, false, PreviewGrid::SceneKind::kBrows));
    }
    {  // publishing: only the figure reports its head bone, and the head root
       // does so on every kind so the align step has an anchor to read.
        using OS::PreviewGrid::RootPublishesHeadAnchor;
        CHECK(RootPublishesHeadAnchor(true, true, PreviewGrid::SceneKind::kGear));
        CHECK(RootPublishesHeadAnchor(true, true, PreviewGrid::SceneKind::kHair));
        CHECK(RootPublishesHeadAnchor(true, false, PreviewGrid::SceneKind::kHair));
        CHECK(!RootPublishesHeadAnchor(true, false, PreviewGrid::SceneKind::kGear));
        CHECK(!RootPublishesHeadAnchor(false, false, PreviewGrid::SceneKind::kHair));
    }

    if (g_failures == 0) {
        std::printf("PreviewGridTests: all passed\n");
    }
    return g_failures;
}
