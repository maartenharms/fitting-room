#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The morph names a BUILT mesh actually carries, read out of the TRIP file
// BodySlide writes beside it.
//
// ⚠⚠ THIS IS GROUND TRUTH AND THE SLIDER SET IS NOT. The Shape page used to
// decide which sliders to offer by resolving the body's OBody preset to a
// BodySlide slider SET and reading that set's slider names out of its .osp.
// That answers "which sliders could this set build", which is a different
// question from "which morphs can move this character", and the two come apart
// in the field:
//
//   * MEASURED 2026-08-16. The preset 'Stoppu_2' names set "UBE 3.0 Release
//     Preview". That set does not exist in 1611 .osp files on the reference
//     load order - the preset was authored against UBE 3.0 and UBE 2.0 TNG is
//     what is installed. The resolve failed, the filter fell open, and the page
//     offered all 678 sliders from all 7 category files. 49 of them belong to
//     the Necoco body, which was never built, so they stored a value under our
//     key and moved nothing. The user's report was "some sliders like burger
//     sliders in shape page doesn't work for UBE", and 'anime breast' - one of
//     the 34 dead ones - was the name they gave.
//   * A slider set can also declare sliders the player did not build, because
//     morphs are a BodySlide checkbox. A set-based filter cannot see that
//     either.
//
// The tri is what RaceMenu's BodyMorph reads, so a name in it can move the
// mesh and a name absent from it cannot. It needs no preset, no set name and
// no walk over a thousand outfit conversions.
//
// ⚠ AN EMPTY RESULT MUST NOT FILTER. A body built with no morphs at all, or a
// mesh whose tri we could not find, gives no names, and treating that as "no
// slider works" would empty the page. The caller falls back rather than
// filtering on nothing; see BodyMorphCatalog.
//
// ---- the format, measured rather than looked up -------------------------
//
// MEASURED over all 3238 .tri files on the reference load order: every one
// begins "PIRT", 1711 carry more than one shape and the largest carries 36.
//
//   char   magic[4]      "PIRT"
//   u16    shapeCount
//   per shape:
//     u8   nameLen, char name[nameLen]
//     u16  morphCount
//     per morph:
//       u8   nameLen, char name[nameLen]
//       f32  multiplier
//       u16  vertexCount
//       vertexCount * 8 bytes   (u16 index, then three i16 deltas)
//
// ⚠ THE DIFF BLOCKS ARE SKIPPED BY ARITHMETIC, NEVER READ. A body tri is
// several megabytes of vertex deltas and we want a name list; walking the
// bytes would cost more than the whole scan. Every skip is bounds checked, so
// a corrupt count stops the parse instead of running off the buffer.
namespace OS::BodyMorphTri {

    // A tri with more shapes or morphs than this is not one BodySlide wrote.
    // The wire fields are u16, so these cannot be exceeded by a well-formed
    // file; they exist to stop a corrupt one reserving on a garbage count.
    inline constexpr std::size_t kMaxShapes = 4096;
    inline constexpr std::size_t kMaxNames  = 65536;

    namespace detail {

        struct Cursor {
            std::string_view bytes;
            std::size_t      at{ 0 };
            bool             ok{ true };

            [[nodiscard]] bool Have(std::size_t a_n) const {
                return ok && a_n <= bytes.size() - at;
            }

            void Skip(std::size_t a_n) {
                if (!Have(a_n)) {
                    ok = false;
                    return;
                }
                at += a_n;
            }

            [[nodiscard]] std::uint8_t U8() {
                if (!Have(1)) {
                    ok = false;
                    return 0;
                }
                return static_cast<std::uint8_t>(bytes[at++]);
            }

            [[nodiscard]] std::uint16_t U16() {
                if (!Have(2)) {
                    ok = false;
                    return 0;
                }
                const auto lo = static_cast<std::uint16_t>(
                    static_cast<std::uint8_t>(bytes[at]));
                const auto hi = static_cast<std::uint16_t>(
                    static_cast<std::uint8_t>(bytes[at + 1]));
                at += 2;
                return static_cast<std::uint16_t>(lo | (hi << 8));
            }

            // The name is length-prefixed by ONE byte, so it can never exceed
            // 255 and no cap of our own is needed on it.
            [[nodiscard]] std::string Str() {
                const auto n = U8();
                if (!Have(n)) {
                    ok = false;
                    return {};
                }
                std::string out{ bytes.substr(at, n) };
                at += n;
                return out;
            }
        };

    }  // namespace detail

    // Every morph name in the file, in file order, duplicates included: a name
    // may appear on more than one shape and the caller is building a set.
    //
    // Returns EMPTY for anything that is not a TRIP file and for a truncated
    // one. A partial list would be worse than none here, because the caller
    // filters with it and a name missing from a half-read file reads exactly
    // like a slider that does not work.
    [[nodiscard]] inline std::vector<std::string> ParseNames(std::string_view a_bytes) {
        std::vector<std::string> out;
        if (a_bytes.size() < 6 || a_bytes.substr(0, 4) != "PIRT") {
            return out;
        }
        detail::Cursor c{ a_bytes, 4, true };
        const auto     shapes = c.U16();
        if (!c.ok || shapes > kMaxShapes) {
            return {};
        }
        for (std::size_t s = 0; s < shapes; ++s) {
            (void)c.Str();  // the shape's own name, which we do not need
            const auto morphs = c.U16();
            if (!c.ok) {
                return {};
            }
            for (std::size_t m = 0; m < morphs; ++m) {
                auto name = c.Str();
                c.Skip(4);  // the multiplier
                const auto verts = c.U16();
                // ⚠ THE ONE MULTIPLICATION IN THIS PARSER, AND IT CANNOT
                // OVERFLOW: verts is a u16, so the product is at most 524280
                // and std::size_t is 64 bits. Skip() bounds checks it anyway.
                c.Skip(static_cast<std::size_t>(verts) * 8u);
                if (!c.ok) {
                    return {};
                }
                if (!name.empty() && out.size() < kMaxNames) {
                    out.push_back(std::move(name));
                }
            }
        }
        // ⚠ TRAILING BYTES ARE NOT AN ERROR HERE, unlike the co-save codec.
        // This is somebody else's format and a future BodySlide may append to
        // it; refusing the whole file over bytes we did not ask for would turn
        // a tool upgrade into "every slider stopped working".
        return out;
    }

    // The tri BodySlide writes beside a built mesh.
    //
    //   "!UBE\Body\femalebody_tangent_1.nif" -> "!UBE/Body/femalebody_tangent.tri"
    //   "Actors\Character\Character Assets\MaleBody_1.NIF"
    //                                    -> "Actors/Character/Character Assets/MaleBody.tri"
    //
    // ⚠ THE WEIGHT SUFFIX COMES OFF AND THE EXTENSION IS MATCHED CASE-BLIND.
    // BodySlide builds a _0 and a _1 nif and ONE tri for the pair, and the
    // engine's own records spell the extension both ways ('MaleBody_1.NIF' is
    // vanilla's own casing). A path with no weight suffix keeps its whole stem,
    // which is what a head mesh needs.
    //
    // Returns empty for a path that is not a mesh, so the caller has one test.
    [[nodiscard]] inline std::string TriPathFor(std::string_view a_nifPath) {
        if (a_nifPath.empty()) {
            return {};
        }
        const auto dot = a_nifPath.find_last_of('.');
        if (dot == std::string_view::npos) {
            return {};
        }
        std::string ext{ a_nifPath.substr(dot + 1) };
        for (auto& ch : ext) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (ext != "nif") {
            return {};
        }
        std::string stem{ a_nifPath.substr(0, dot) };
        if (stem.size() >= 2 && stem[stem.size() - 2] == '_' &&
            (stem.back() == '0' || stem.back() == '1')) {
            stem.resize(stem.size() - 2);
        }
        if (stem.empty()) {
            return {};
        }
        // Forward slashes throughout, which is what the rest of this codebase
        // folds paths to and what std::filesystem accepts on Windows anyway.
        for (auto& ch : stem) {
            if (ch == '\\') {
                ch = '/';
            }
        }
        return stem + ".tri";
    }

}  // namespace OS::BodyMorphTri
