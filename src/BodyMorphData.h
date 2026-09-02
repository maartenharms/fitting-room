#pragma once

// BodySlide's per-vertex morph data (.osd) and the arithmetic that turns a
// preset's slider values into one displacement per vertex.
//
// ⚠⚠ THE FORMAT IS MEASURED AND IT SELF-VALIDATES, which is the only reason
// this file is allowed to exist. This project has shipped garbage geometry
// twice by ASSUMING a binary layout (VF_FULLPREC, twice, in MeshExtractor), so
// a reader that merely "looks right" is not acceptable. The layout below was
// read off the real files and, critically, a correct parse consumes the file
// EXACTLY: every set is length-prefixed and the last one ends on the final
// byte. A wrong layout desynchronises and overruns or underruns, so Parse
// refuses anything that does not land precisely, and a refusal costs a card
// rather than a wrong picture.
//
//   u32   magic / version         bytes 0..7 are header, not read
//   u32   set count               at byte 8
//   then, per set, packed with no alignment:
//     u8    name length
//     char  name[len]             latin-1, the <Data> name an .osp slider cites
//     u16   delta count
//     then delta count records of 14 bytes:
//       u16      vertex index     indexes the SHAPE's vertex array
//       float[3] displacement     game units, added to the base position
//
// Cross-checked on this load order 2026-08-10, four ways that agree: the
// reference NIF's skin partition holds 29298 vertices, the built body's holds
// 29298, the largest index in any .osd set is 29297, and the reference's z
// range (11.187..114.476) is the live engine's own mannequin box to two
// decimals. So the indices address exactly the array the engine loads.
//
// ⚠ THE VERTEX ORDER IS THE SHAPE'S, NOT THE PARTITION'S. A caller applying
// these has to be sure the array it holds is in the NIF's shape order; if the
// extractor ever reorders (a vertexMap indirection, a partition split), the
// deltas must be remapped or the mesh scrambles while the log stays clean.
//
// ⚠⚠ AND ON THIS ENGINE THE BUFFER IS ALREADY IN SHAPE ORDER, so remapping it
// is the thing that scrambles. Each skin partition is handed a buffer holding
// the WHOLE shape with a triList in shape-global indices: measured 2026-08-10,
// p0 walks 29298 vertices while declaring 23683 and p1 walks 29293 while
// declaring 5704. Sending that through p.vertexMap displaces the first
// `declared` vertices of each partition by some other vertex's delta, which
// came back as a clean torso with shredded hands and lower legs. Decide it
// from the buffer the loop actually walks (vertCount > p.vertices), never from
// the declared counts, and never from what the layout is supposed to be.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OS::BodyMorphData {

    // One slider's displacement list. Sparse: only the vertices it moves.
    struct DeltaSet {
        std::vector<std::uint16_t>        indices;
        std::vector<std::array<float, 3>> deltas;
    };

    using DeltaSets = std::unordered_map<std::string, DeltaSet>;

    // ⚠⚠ A DELTA SET IS ADDRESSED BY ITS FILE AND ITS NAME, NEVER THE NAME
    // ALONE. A slider set can draw runs from more than one .osd, and the same
    // run NAME really does appear in two of them with DIFFERENT contents,
    // because each file was built against its own reference mesh. MEASURED on
    // the 3BA nevernude pair (`SE 3BBB Body Amazing v2.osd` against `CBBE 3BBB
    // Amazing NeverNude.osd`): 94 names appear in both, 71 byte-identical and
    // 23 NOT, and all 23 of those are 3BA_Vagina or 3BA_Anus runs. Keying by
    // name alone lets whichever file is read second silently replace the
    // other's deltas, on exactly the shapes that are hardest to notice.
    //
    // The file half is folded to lower case because the key is built twice,
    // once from the .osp's spelling of a <Data> reference and once when the
    // parsed file is filed away, and nothing makes a mod spell it the same way
    // in both places.
    //
    // ⚠ THE RUNTIME (.tri) SOURCE DOES NOT USE THIS. It has exactly one file
    // by construction, its morph names are already unique within it, and its
    // rules address them bare.
    [[nodiscard]] std::string DeltaKey(std::string a_osdFile,
                                       const std::string& a_setName);

    // What a slider set's .osp says about one slider, which is what decides
    // how a preset's 0..1 value becomes a multiplier.
    struct SliderRule {
        std::string dataName;          // the <Data> set this slider drives
        float       small{ 0.0f };     // the .osp's own defaults, in 0..100
        float       big{ 0.0f };
        bool        invert{ false };
        bool        zap{ false };      // a toggle that deletes geometry, never morphs
        bool        uv{ false };       // moves texture coordinates, not vertices
    };

    // ⚠ A PRESET NAMES ONLY WHAT IT CHANGES, so a slider the preset never
    // mentions takes the .osp's OWN default rather than zero. Reading an
    // unnamed slider as zero is the difference between a body and a
    // half-collapsed one, and it is silent.
    //
    // a_value is the preset's slider value in 0..1 when it named one.
    //
    // ⚠⚠ EVERY SLIDER IS A FUNCTION OF THE CHARACTER'S WEIGHT, small at 0 and
    // big at 100, which is the same walk BodyMorphInterpolatePercent does in
    // game. Reading `big` alone renders the weight-100 body for every
    // character, so a character at weight 0 was shown a shape they will never
    // have. a_weight01 is 0..1 and defaults to the big end, which is both the
    // old behaviour and the right answer for a caller that has no character.
    //
    // ⚠ THE NAMED CASE IS ALREADY INTERPOLATED by the time it arrives: the
    // preset stores a small and a big per slider and the caller walks them,
    // because only the caller knows which preset it is reading. This walks the
    // .osp's OWN default for the sliders a preset never mentions, which are
    // the majority of them and just as weight-dependent.
    [[nodiscard]] inline float SliderWeight(const SliderRule& a_rule, bool a_named,
                                            float a_value, float a_weight01 = 1.0f) {
        const float dflt =
            (a_rule.small + (a_rule.big - a_rule.small) * a_weight01) / 100.0f;
        float t = a_named ? a_value : dflt;
        if (a_rule.invert) {
            t = 1.0f - t;
        }
        return t;
    }

    // One slider's value at a character's weight, 0..1 out of the preset's own
    // 0..100 pair. The named half of the rule above, kept here so the editor
    // and any test agree on it.
    [[nodiscard]] inline float ValueAtWeight(float a_small, float a_big,
                                             float a_weight01) {
        return (a_small + (a_big - a_small) * a_weight01) / 100.0f;
    }

    // ⚠⚠ THE CARD'S WEIGHT IS SNAPPED AND THE PICTURE IS DRAWN AT THE SNAPPED
    // VALUE, not at the exact one. Weight is a drag: every intermediate value
    // the handle passes through is a different shape, a different key and a
    // full rebuild of every body card, and a cold one is an .osp parse and an
    // 11 MB .osd read per set plus a NIF load and a morph per card. Five parts
    // in a hundred is invisible on a thumbnail and turns a drag from twenty
    // rebuilds into a handful.
    //
    // ⚠⚠ SNAPPED BEFORE THE SHAPE IS BUILT, NEVER ONLY IN THE KEY. Keying a
    // coarse weight while rendering the exact one makes the key stop
    // describing the picture, which is the whole family of bug this pane has
    // already paid for twice: the first card to land wins the step and every
    // other weight in it silently inherits that photograph. Snapping the INPUT
    // keeps the key honest, because the same key really is the same picture.
    inline constexpr float kWeightStep01 = 0.05f;  // 5 of the game's 0..100

    [[nodiscard]] inline float SnapWeight(float a_weight01) {
        const float clamped =
            a_weight01 < 0.0f ? 0.0f : (a_weight01 > 1.0f ? 1.0f : a_weight01);
        return std::round(clamped / kWeightStep01) * kWeightStep01;
    }

    // ⚠ zap AND uv SLIDERS ARE NOT SHAPE. A zap is a delete-this-geometry
    // toggle and a uv slider moves texture coordinates; feeding either through
    // the position arithmetic corrupts the mesh, so they are refused here
    // rather than at every call site.
    [[nodiscard]] inline bool SliderMovesVertices(const SliderRule& a_rule) {
        return !a_rule.zap && !a_rule.uv && !a_rule.dataName.empty();
    }

    using SliderRules  = std::unordered_map<std::string, SliderRule>;  // by slider name
    using SliderValues = std::unordered_map<std::string, float>;       // 0..1, what a preset named

    // Every slider folded into ONE displacement per vertex, dense and indexed
    // by the SHAPE's vertex index. Dense because the consumer is a per-vertex
    // loop that wants a constant-time lookup, and a body is tens of thousands
    // of vertices rather than millions.
    //
    // ⚠ A vertex index at or past a_vertexCount is DROPPED rather than grown
    // into: the .osd is third-party content and its indices are bounded by the
    // mesh, not the other way round.
    [[nodiscard]] std::vector<std::array<float, 3>> Accumulate(
        const DeltaSets& a_sets, const SliderRules& a_rules,
        const SliderValues& a_values, std::size_t a_vertexCount,
        float a_weight01 = 1.0f);

    // What a body card puts in SceneIdentity::editorId, which is already part
    // of the disk key.
    //
    // ⚠ NO NEW KEY FIELD, and that is deliberate. Race and sex were kept out
    // of the key because they arrive through the model paths; a preset cannot
    // do that, because every preset of a body builds the SAME path, so its
    // identity has to ride a field that is already keyed. editorId is one.
    //
    // ⚠⚠ AND IT HASHES THE SLIDER VALUES, not just the name. A preset edited
    // in Body Studio keeps its name, so a name-only identity would leave the
    // old picture on disk forever and the card would go stale silently. The
    // values ARE the shape, so hashing them is the invalidation.
    //
    // ⚠⚠ THE SLIDERS ARE SORTED BEFORE HASHING. They arrive in an unordered
    // map, whose iteration order is not stable across runs, so folding them as
    // they come would hand back a different key every session and rebuild the
    // entire pane forever while looking perfectly correct.
    // ⚠⚠ AND THE WEIGHT IS IN IT, because the card is drawn at the character's
    // weight and two weights are two pictures. The named values already move
    // with weight, but the sliders a preset never mentions take the .osp's own
    // small->big default and those are invisible here, so a key built from the
    // values alone could hand back one weight's photograph for another's.
    //
    // ⚠⚠ AND THE SET NAME IS IN IT, WHICH IS HOW A RUNTIME OPTION REACHES THE
    // DISK KEY. The SFW option rebuilds a card from the body mod's COVERED
    // slider set, and nothing else about that card moves: same preset, same
    // slider values, same weight, same model paths. A compile-time tag cannot
    // express a switch the user flips, so the thing the switch actually
    // CHANGES has to be keyed instead. Without this the pane serves the other
    // setting's photographs and the option looks like it does nothing.
    [[nodiscard]] std::string CardIdentity(std::string_view    a_presetName,
                                           const SliderValues& a_values,
                                           float               a_weight01 = 1.0f,
                                           std::string_view    a_setName  = {});

    // Parse a whole .osd. False leaves a_out untouched and means the bytes did
    // not land exactly, which is the format's own integrity check rather than
    // a guess about how strict to be.
    [[nodiscard]] bool Parse(const std::byte* a_data, std::size_t a_size, DeltaSets& a_out);

    [[nodiscard]] bool ParseFile(const std::string& a_path, DeltaSets& a_out);

    // ---- the RUNTIME morph data (.tri), which is a different source -------
    //
    // ⚠⚠ THIS IS HOW A PRESET WITH NO BodySlide PROJECT STILL HAS A SHAPE, and
    // it is why clicking a crossed card visibly reshapes the character while
    // the card itself cannot draw. The two paths carry the same displacements
    // in different files for different consumers:
    //
    //   .osd  ships with the BodySlide PROJECT and is what BodySlide builds a
    //         body from. Absent unless the slider set is installed.
    //   .tri  ships beside the BUILT body and is what RaceMenu's BodyMorph
    //         applies at runtime, by slider NAME. Present whenever the body
    //         was built with morphs, which is what makes an OBody preset work
    //         at all.
    //
    // HIMBO ships malebody.tri beside malebody_0.nif and malebody_1.nif with
    // no .osp anywhere in the load order, which is exactly the case that drew
    // a grid full of crosses (field 2026-08-10).
    //
    // ⚠ THE NAMES ARE THE PRESET'S OWN SLIDER NAMES, so the output drops
    // straight into Accumulate beside an .osd's. No rules file is involved:
    // a .tri names its morphs the way a preset names its sliders.
    //
    // The layout, measured 2026-08-10 and cross-validated the only way that
    // counts, by consuming two unrelated files EXACTLY to their final byte
    // (HIMBO's malebody.tri and BodySlide (Nude)'s):
    //
    //   char  magic[4]              "PIRT"
    //   u16   shape count
    //   per shape:
    //     u8    name length, char name[len]
    //     u16   morph count
    //     per morph:
    //       u8    name length, char name[len]
    //       f32   multiplier        deltas are i16 ticks OF THIS
    //       u16   vertex count
    //       per vertex, 8 bytes:
    //         u16   vertex index    indexes the SHAPE's vertex array
    //         i16   dx, dy, dz      multiply by the multiplier for game units
    //   u16   UV shape count        then the same nesting with SIX-byte
    //                               records, which is the only reason this
    //                               half is read at all: skipping it wrong
    //                               desynchronises the exactness check that
    //                               makes the rest trustworthy.
    //
    // ⚠ THE UV MORPHS ARE NOT SHAPE, exactly like an .osp's uv sliders, so
    // they are consumed and discarded rather than folded into positions.
    //
    // False means the bytes did not land exactly and a_out is untouched, the
    // same contract Parse has and for the same reason: a refusal costs a card
    // and a wrong layout costs a scrambled body.
    //
    // ⚠⚠ ONLY THE FIRST SHAPE'S MORPHS ARE KEPT, because every shape indexes
    // its OWN vertex array and the shapes after the first are OTHER MESHES. A
    // BodySlide build with morphs writes the body plus its Virtual* physics
    // colliders into one file, sharing 120 morph names, and folding those
    // together stacked collider deltas onto body vertices 0..~1900: torn
    // seams on every strong preset (field 2026-08-11). The later shapes are
    // still consumed so the exactness check keeps its meaning.
    // a_shapeName, when given, receives the name of the shape the morphs
    // belong to.
    //
    // ⚠⚠ IT IS THE ONLY THING SAYING WHICH MESH THIS FILE IS FOR, and a
    // fallback has no other way to know. The .osd path ships a reference mesh
    // beside its deltas so the pair cannot be wrong; a .tri is resolved
    // through the VFS at a fixed path and the .nif beside it is resolved
    // separately, so two different mods can win the two halves. That is
    // exactly what happened here: The New Gentleman supplies malebody_1.nif
    // and HIMBO supplies malebody.tri, and indices meant for one body landed
    // on another (field 2026-08-10, shredded male cards).
    [[nodiscard]] bool ParseTri(const std::byte* a_data, std::size_t a_size,
                                DeltaSets& a_out, std::string* a_shapeName = nullptr);

    [[nodiscard]] bool ParseTriFile(const std::string& a_path, DeltaSets& a_out,
                                    std::string* a_shapeName = nullptr);

}  // namespace OS::BodyMorphData
