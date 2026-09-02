// ⚠ NO PCH, deliberately: the pure-logic sources skip it so a test target can
// compile them standalone, which is how DyeGrid.cpp is built into DyeGridTests.
// The plugin target force-includes PCH.h through target_precompile_headers.
#include "BodyMorphData.h"

#include "PreviewGrid.h"  // Fnv1a64, pure

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>
#include <cstring>
#include <fstream>

namespace OS::BodyMorphData {

    namespace {

        // Little-endian reads with a bound on every one of them. The whole
        // point of this reader is that it cannot walk off the end, so there is
        // no unchecked path: a truncated or misread file fails the cursor
        // check and the caller drops the card.
        struct Cursor {
            const std::byte* data{ nullptr };
            std::size_t      size{ 0 };
            std::size_t      at{ 0 };

            [[nodiscard]] bool Need(std::size_t a_bytes) const {
                return a_bytes <= size - at;
            }
            template <class T>
            [[nodiscard]] bool Read(T& a_out) {
                if (!Need(sizeof(T))) {
                    return false;
                }
                std::memcpy(&a_out, data + at, sizeof(T));
                at += sizeof(T);
                return true;
            }
        };

        constexpr std::size_t kHeaderBytes = 8;   // magic and version, unread
        constexpr std::size_t kDeltaBytes  = 14;  // u16 index + 3 floats

    }  // namespace

    bool Parse(const std::byte* a_data, std::size_t a_size, DeltaSets& a_out) {
        if (!a_data || a_size < kHeaderBytes + sizeof(std::uint32_t)) {
            return false;
        }
        Cursor cur{ a_data, a_size, kHeaderBytes };
        std::uint32_t setCount = 0;
        if (!cur.Read(setCount)) {
            return false;
        }
        // A file claiming more sets than could fit even at their minimum size
        // is refused before a single allocation.
        if (setCount > a_size) {
            return false;
        }

        DeltaSets parsed;
        parsed.reserve(setCount);
        for (std::uint32_t s = 0; s < setCount; ++s) {
            std::uint8_t nameLen = 0;
            if (!cur.Read(nameLen) || !cur.Need(nameLen)) {
                return false;
            }
            std::string name(reinterpret_cast<const char*>(cur.data + cur.at), nameLen);
            cur.at += nameLen;

            std::uint16_t count = 0;
            if (!cur.Read(count) || !cur.Need(static_cast<std::size_t>(count) * kDeltaBytes)) {
                return false;
            }
            DeltaSet set;
            set.indices.reserve(count);
            set.deltas.reserve(count);
            for (std::uint16_t d = 0; d < count; ++d) {
                std::uint16_t       index = 0;
                std::array<float, 3> delta{};
                if (!cur.Read(index) || !cur.Read(delta[0]) || !cur.Read(delta[1]) ||
                    !cur.Read(delta[2])) {
                    return false;
                }
                set.indices.push_back(index);
                set.deltas.push_back(delta);
            }
            parsed.emplace(std::move(name), std::move(set));
        }

        // ⚠⚠ THE INTEGRITY CHECK, AND IT IS THE WHOLE SAFETY ARGUMENT. Every
        // record in this format is length-prefixed, so a correct parse lands
        // on the final byte exactly. A layout that is wrong in any field
        // desynchronises and finishes early or late. Refusing anything that
        // does not land costs a card and never a wrong picture, which is the
        // trade VF_FULLPREC taught this codebase twice.
        if (cur.at != a_size) {
            return false;
        }
        a_out = std::move(parsed);
        return true;
    }

    // ⚠⚠ THE RENDER TAG, AND IT IS THE ONLY THING THAT REBUILDS A BODY CARD
    // WHEN THE PICTURE CHANGES AND THE PRESET DOES NOT. The slider values key
    // the SHAPE, not the photograph of it: when the renderer's treatment of a
    // body changes, every value is unchanged, every key still matches, and
    // every cached PNG stays valid by the key while being wrong on screen. The
    // first field run after the morph fix proved it, with 17 body cards served
    // from disk-cache.upload and BuildOne never called, so the fix was in the
    // build and not one pixel moved.
    //
    // ⚠ IT LIVES IN THE HASH RATHER THAN BESIDE IT, so the key format and
    // length do not change; the mannequin's kTag is the same idea for the same
    // reason. Bumping this invalidates body cards ONLY, which is why it exists
    // instead of kRendererVersion, whose bump rebuilds every card in the app.
    //
    // b1: the morph reaches the extractor at all (it had been landing in
    //     Extract's defaulted last parameter, so every card was the unmorphed
    //     reference mesh), and a body draws grey like every other card rather
    //     than resolving its own skin diffuse.
    // b2: instrumentation only. The b1 bodies came back differing and grey,
    //     so the field arrives, but with the hands, forearms and lower legs
    //     shredded while the torso stayed clean. A card that does not rebuild
    //     logs nothing, so measuring the partition mapping needs a bump of
    //     its own.
    // b3: the apply stops sending an already-shape-indexed buffer through
    //     p.vertexMap. Each partition carries the WHOLE shape's vertices with
    //     a shape-global triList, so the map was displacing the first
    //     `declared` vertices of each partition by another vertex's delta.
    // b4: a preset with no BodySlide project resolves its morphs at BUILD
    //     time too. The editor picked the built body's runtime data and the
    //     build re-asked for a project that was never there, so the field came
    //     back empty and every fallback card drew its base body unmorphed. The
    //     wrong pictures are on disk under keys nothing else moves.
    // b5: instrumentation. Some male cards come back shredded and some do
    //     not, split exactly by which source answered: the .osp path is clean
    //     and the .tri fallback is not. A .tri names the shape it was measured
    //     against, so the log carries that name and its index span now. A card
    //     that does not rebuild logs nothing.
    // b6: the morph reaches ONE geometry. A body NIF carries physics
    //     colliders beside the body (VirtualArms, VirtualBelly,
    //     VirtualBreasts, VirtualButt on this rig, a few hundred vertices
    //     each against a 14597 vertex body) and the field indexed by the body
    //     was applied to every one of them, shredding each into debris
    //     exactly where it sits. The .tri names its shape and that name is
    //     the gate.
    // b7: and the shapes it does not name are not DRAWN either. Denying the
    //     colliders the morph stopped them shredding and left them sitting
    //     unmorphed inside a body that had moved, poking through it at the
    //     chest, belly and hips. The game never renders them.
    // b8: the normals follow the morph. Moving the positions and leaving the
    //     normals alone lit a morphed body for the shape it used to be, which
    //     is the dark blotching across the chest, arms and shins of the bulky
    //     male presets: no holes, no debris, just patches too dark for the
    //     surface under them. The pass rotates each authored normal by the
    //     rotation the morph put into the surface around it, and reports what
    //     it measured either way, so the log settles the theory even if the
    //     picture does not move.
    // b9: instrumentation. `b8` fixed the shading and left bright artifacts at
    //     the WRISTS, ANKLES, NECK and KNEES, which are joints, which is where
    //     the skin partitions split: this body is 27930 / 60 / 356 triangles
    //     and a sixty triangle partition is a seam ring. Three causes fit and
    //     reading cannot separate them, so the log carries the partition's raw
    //     buffer pointer (the `shared=` the b3 comment describes and never
    //     printed), its stride and raw descriptor (which also settles why every
    //     vertex reports no normal), and a count of triangles that appear in
    //     more than one partition, which is the drawn-twice test.
    //     ⚠ The per-mesh .tri route was measured and is DEAD: malehands.tri and
    //     malefeet.tri carry one "Weight 0 to 1" morph each and none of the
    //     124 HIMBO sliders, so the hands and feet have no preset shape to take.
    // b10: WHERE the fold is. b9 answered its three questions and killed two
    //     of them: dupTris=0 on every card, so nothing is drawn twice, and
    //     flags=0x43 with nrmOff=0 confirms from the raw flag word that these
    //     buffers carry VERTEX|UV|SKINNED and no normals at all. What survives
    //     is the morph folding the surface: roughly a thousand vertices a card
    //     turned past 25 degrees, a few inverted outright, and nonzero=14558 of
    //     14597 says the field moves the seam rings rather than holding them.
    //     A count cannot say whether that is the wrist, the neck, the knee or
    //     the SOS cut, and guessing between them is what this stint keeps
    //     paying for, so the log now carries a Z histogram foot to head and the
    //     bounding box of the folded set.
    // b11: does partition 1's vertex i equal partition 0's vertex i? b10 put
    //     the fold everywhere the morph is strong, which stopped pointing at
    //     one seam, but b9's raw= also proved each partition owns its own
    //     buffer, and the whole shape-indexed apply stands on those buffers
    //     agreeing on vertex order. That was inferred from counts (b3), never
    //     read from the bytes. bufDiff= is the byte reading: 0 and the apply is
    //     sound everywhere; nonzero and p1/p2's triangles - the bone-overflow
    //     ones at the wrists, neck and crotch - have been morphing and shading
    //     the wrong vertices this whole time, which is the speckle.
    // b12: the petal test. b11's bufDiff=0 proved every partition agrees on
    //     what vertex i is, so the apply is mechanically sound end to end,
    //     and comparing the b7 and b11 screenshots shows the debris in the
    //     SAME places dark then lit: it is geometry, it predates the normal
    //     fix, and the "no debris" claim in the b7 handoff was wrong. Two
    //     causes remain: a genuinely violent morph (deltas stacked on a built
    //     body that already carries a shape) or garbage deltas on a few
    //     indices (a .tri decode fault). Edge stretch separates them: a real
    //     morph moves neighbours together and no edge grows much; a wrong
    //     delta tears one corner away and its edges grow several-fold. The
    //     log counts triangles past 3x, boxes them, and names the worst
    //     triangle's three shape indices with each corner's delta, which are
    //     the exact entries to pull out of the .tri offline.
    // b13: THE TEARS, ROOT-CAUSED AND FIXED. The resolved MaleBody.tri is the
    //     MO2 Overwrite's, and it carries EIGHT shapes: the body plus its
    //     seven Virtual* physics colliders, 120 morph names shared between
    //     them, each shape indexing its OWN vertex array. ParseTri merged
    //     morphs by name across shapes, so Accumulate stacked collider deltas
    //     onto body vertices 0..~1900: every worst= triangle b12 caught paired
    //     a polluted low index (138 is VirtualBelly's last, 261/262 inside
    //     VirtualBreasts, 84/92 inside VirtualArms, 643 inside VirtualLegs)
    //     against its clean high-index seam twin. ParseTri now keeps the first
    //     shape's morphs only, which is how RaceMenu reads the format.
    //
    //     ⚠⚠ THE LINE THAT USED TO END THIS ENTRY WAS WRONG, and it is left
    //     named here because it read as proof and cost a stint: "The .osp path
    //     was never affected: .osd sets are chosen per slider." Sets are
    //     chosen per slider, and that is not the same as per SHAPE. The .osp
    //     path had the identical defect the whole time, in a different file.
    //     See b15.
    // b14: the head, hands and feet are shaded too. b8 recomputed the normals
    //     these buffers never carried, but it was gated on the morph, so it
    //     reached the body alone and left the extremities as flat plates
    //     beside it (field 2026-08-11, HIMBO and 3BA). The pass now runs for
    //     any skinned mesh whose descriptor has no VF_NORMAL, morph or not;
    //     with no morph its two recomputes are identical, so the rotation is
    //     the identity and only the missing normals get written. UBE looked
    //     right throughout because it ships `_tangent` builds of the same
    //     parts, which DO carry normals: the discriminator is the buffer, not
    //     the body mod.
    // b15: THE .osp PATH GETS THE PER-SHAPE FIX b13 GAVE THE .tri PATH. A
    //     slider set carries one <Data> run per SHAPE it builds, with the
    //     shape in a target attribute, and the loader took the first and
    //     stopped. Everything else in the file was then displaced by the
    //     body's field: 3BA_Vagina (1905 vertices) took the 18436-vertex
    //     body's run, 3BA_Anus (201) took it as well, and the physics proxies
    //     took it wherever a set declares them. The drop filter that was meant
    //     to catch this could never fire, because it keyed on a shape name the
    //     .osp path never set. Rules are per shape now, the extractor picks
    //     the field measured against each geometry, and a geometry the set
    //     does not describe is not drawn.
    //
    //     ⚠ THE BUMP IS NOT OPTIONAL AND modelPaths DID NOT MOVE. The picture
    //     changes on every .osp-sourced body card while the paths, the preset
    //     name, the slider values and the weight are all identical, and
    //     DiskKeyFor folds none of the loader's or the extractor's behaviour.
    //     Without this the old photograph is served forever and the fix looks
    //     like it did nothing.
    namespace {
        constexpr const char* kTag = "b15";
    }

    std::string CardIdentity(std::string_view a_presetName, const SliderValues& a_values,
                             float a_weight01, std::string_view a_setName) {
        // Sorted, because the map's order is not stable across runs and the
        // hash has to be. Name and value both, so a slider moved to a new
        // value changes the key and the card rebuilds.
        std::vector<std::pair<std::string_view, float>> sorted;
        sorted.reserve(a_values.size());
        for (const auto& [name, value] : a_values) {
            sorted.emplace_back(name, value);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::string folded;
        folded.reserve(sorted.size() * 12 + 24);
        folded.append(kTag);
        folded.push_back('|');
        // Quantised for the same reason the values are: a float's spelling
        // varies with the formatting path and its bits with how it was
        // computed, and a key that differs between two runs of the same weight
        // rebuilds the pane forever while looking perfectly correct.
        folded.append("w=");
        folded.append(std::to_string(
            static_cast<std::int64_t>(std::lround(static_cast<double>(a_weight01) * 10000.0))));
        folded.push_back('|');
        // ⚠ THE SET, WHICH IS WHAT THE SFW OPTION SWAPS. Empty for a caller
        // that has no set, so every existing key is byte-identical to what it
        // was and nothing rebuilds that did not have to.
        if (!a_setName.empty()) {
            folded.append("s=");
            folded.append(a_setName);
            folded.push_back('|');
        }
        for (const auto& [name, value] : sorted) {
            folded.append(name);
            folded.push_back('=');
            // ⚠ QUANTISED RATHER THAN PRINTED. A float's decimal spelling
            // varies with the formatting path and a bit pattern varies with
            // how it was computed; a fixed-point integer is the same on every
            // run for the same slider, which is what the key needs.
            const auto ticks =
                static_cast<std::int64_t>(std::lround(static_cast<double>(value) * 10000.0));
            folded.append(std::to_string(ticks));
            folded.push_back(';');
        }

        std::string out{ "body:" };
        out.append(a_presetName);
        out.push_back(':');
        char hex[17]{};
        std::snprintf(hex, sizeof hex, "%016llx",
                      static_cast<unsigned long long>(PreviewGrid::Fnv1a64(folded)));
        out.append(hex);
        return out;
    }

    std::vector<std::array<float, 3>> Accumulate(const DeltaSets&    a_sets,
                                                 const SliderRules&  a_rules,
                                                 const SliderValues& a_values,
                                                 std::size_t         a_vertexCount,
                                                 float               a_weight01) {
        std::vector<std::array<float, 3>> field(a_vertexCount, std::array<float, 3>{});
        for (const auto& [name, rule] : a_rules) {
            if (!SliderMovesVertices(rule)) {
                continue;
            }
            const auto set = a_sets.find(rule.dataName);
            if (set == a_sets.end()) {
                continue;
            }
            const auto  named  = a_values.find(name);
            const float weight = SliderWeight(rule, named != a_values.end(),
                                              named != a_values.end() ? named->second : 0.0f,
                                              a_weight01);
            if (weight == 0.0f) {
                continue;
            }
            const auto& indices = set->second.indices;
            const auto& deltas  = set->second.deltas;
            for (std::size_t d = 0; d < indices.size() && d < deltas.size(); ++d) {
                const std::size_t v = indices[d];
                if (v >= a_vertexCount) {
                    continue;  // third-party content; the mesh bounds it
                }
                field[v][0] += deltas[d][0] * weight;
                field[v][1] += deltas[d][1] * weight;
                field[v][2] += deltas[d][2] * weight;
            }
        }
        return field;
    }

    bool ParseTri(const std::byte* a_data, std::size_t a_size, DeltaSets& a_out,
                  std::string* a_shapeName) {
        if (!a_data || a_size < 6) {
            return false;
        }
        if (std::memcmp(a_data, "PIRT", 4) != 0) {
            return false;
        }
        Cursor cur{ a_data, a_size, 4 };

        // A name is length-prefixed by ONE byte here, unlike the .osd's, and
        // an empty one is legal content rather than an error.
        const auto readName = [&](std::string& a_name) {
            std::uint8_t len = 0;
            if (!cur.Read(len) || !cur.Need(len)) {
                return false;
            }
            a_name.assign(reinterpret_cast<const char*>(cur.data + cur.at), len);
            cur.at += len;
            return true;
        };

        DeltaSets parsed;
        std::uint16_t shapeCount = 0;
        if (!cur.Read(shapeCount)) {
            return false;
        }
        for (std::uint16_t s = 0; s < shapeCount; ++s) {
            std::string shapeName;
            if (!readName(shapeName)) {
                return false;
            }
            // The first shape names the file. A body carries one; reporting
            // the first is what lets a caller check the pairing.
            if (a_shapeName && a_shapeName->empty()) {
                *a_shapeName = shapeName;
            }
            std::uint16_t morphCount = 0;
            if (!cur.Read(morphCount)) {
                return false;
            }
            for (std::uint16_t m = 0; m < morphCount; ++m) {
                std::string morphName;
                if (!readName(morphName)) {
                    return false;
                }
                float         multiplier = 0.0f;
                std::uint16_t vertexCount = 0;
                if (!cur.Read(multiplier) || !cur.Read(vertexCount)) {
                    return false;
                }
                if (!cur.Need(static_cast<std::size_t>(vertexCount) * 8)) {
                    return false;
                }
                // ⚠⚠ THE FIRST SHAPE'S MORPHS ONLY, AND MERGING BY NAME WAS A
                // FIELD-MEASURED DISASTER. Every shape in the file indexes its
                // OWN vertex array: a BodySlide build with morphs writes the
                // body and its seven Virtual* physics colliders into one .tri,
                // 120 morph names shared between them, and the collider
                // entries are measured against collider arrays a few hundred
                // vertices long. Merging appended them into the body's sets,
                // and Accumulate stacked collider deltas onto body vertices
                // 0..~1900: torn triangles at every seam where a polluted
                // low-index vertex sits beside its clean high-index duplicate,
                // scaling with preset strength (field 2026-08-11, PAU Bola,
                // and the .osp path was clean the whole time because .osd sets
                // are chosen per slider, never by name). RaceMenu reads this
                // format per shape onto the matching geometry, which is what
                // the shape names are FOR. The other shapes are still walked
                // so the exactness check keeps its meaning.
                if (s != 0) {
                    cur.at += static_cast<std::size_t>(vertexCount) * 8;
                    continue;
                }
                auto& set = parsed[morphName];
                set.indices.reserve(set.indices.size() + vertexCount);
                set.deltas.reserve(set.deltas.size() + vertexCount);
                for (std::uint16_t v = 0; v < vertexCount; ++v) {
                    std::uint16_t index = 0;
                    std::int16_t  dx = 0, dy = 0, dz = 0;
                    if (!cur.Read(index) || !cur.Read(dx) || !cur.Read(dy) ||
                        !cur.Read(dz)) {
                        return false;
                    }
                    set.indices.push_back(index);
                    set.deltas.push_back({ static_cast<float>(dx) * multiplier,
                                           static_cast<float>(dy) * multiplier,
                                           static_cast<float>(dz) * multiplier });
                }
            }
        }

        // ⚠ THE UV HALF IS CONSUMED AND THROWN AWAY, and consuming it is not
        // optional. It is what lets the exactness check below mean anything:
        // stopping early would leave a tail on every well-formed file and make
        // the one honest signal of a misread indistinguishable from normal.
        std::uint16_t uvShapeCount = 0;
        if (!cur.Read(uvShapeCount)) {
            return false;
        }
        for (std::uint16_t s = 0; s < uvShapeCount; ++s) {
            std::string shapeName;
            if (!readName(shapeName)) {
                return false;
            }
            std::uint16_t morphCount = 0;
            if (!cur.Read(morphCount)) {
                return false;
            }
            for (std::uint16_t m = 0; m < morphCount; ++m) {
                std::string morphName;
                if (!readName(morphName)) {
                    return false;
                }
                float         multiplier = 0.0f;
                std::uint16_t vertexCount = 0;
                if (!cur.Read(multiplier) || !cur.Read(vertexCount)) {
                    return false;
                }
                // SIX bytes here, not eight. Measured; see the header.
                const std::size_t bytes = static_cast<std::size_t>(vertexCount) * 6;
                if (!cur.Need(bytes)) {
                    return false;
                }
                cur.at += bytes;
            }
        }

        // The same integrity rule the .osd reader lives by: a correct layout
        // lands on the final byte and anything else is refused whole.
        if (cur.at != a_size) {
            return false;
        }
        a_out = std::move(parsed);
        return true;
    }

    bool ParseTriFile(const std::string& a_path, DeltaSets& a_out,
                      std::string* a_shapeName) {
        std::ifstream file(a_path, std::ios::binary | std::ios::ate);
        if (!file) {
            return false;
        }
        const auto size = file.tellg();
        if (size <= 0) {
            return false;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
            return false;
        }
        return ParseTri(bytes.data(), bytes.size(), a_out, a_shapeName);
    }

    bool ParseFile(const std::string& a_path, DeltaSets& a_out) {
        std::ifstream file(a_path, std::ios::binary | std::ios::ate);
        if (!file) {
            return false;
        }
        const auto size = file.tellg();
        if (size <= 0) {
            return false;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
            return false;
        }
        return Parse(bytes.data(), bytes.size(), a_out);
    }

    std::string DeltaKey(std::string a_osdFile, const std::string& a_setName) {
        std::transform(a_osdFile.begin(), a_osdFile.end(), a_osdFile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a_osdFile + "\\" + a_setName;
    }

}  // namespace OS::BodyMorphData
