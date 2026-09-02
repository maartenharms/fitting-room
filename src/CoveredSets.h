#pragma once

// Which installed slider set is the COVERED sibling of a nude body set.
//
// The SFW option does not paint anything or generate anything. It builds the
// card from a different slider set: the one the body mod already ships with a
// bra and pants modelled into it. That set is real, its covering carries its
// own authored morph runs, and a preset's slider values transfer to it almost
// completely (142 of 159 names on 3BA, 122 of 137 on CBBE, 89 of 89 on HIMBO,
// measured 2026-08-11). The names that do not transfer are the explicit
// anatomy sliders, which is the one place where losing them is the point.
//
// ⚠ PURE, AND HERE RATHER THAN IN THE SCAN, because nothing compiles
// BodyCardScene or the catalog into a test. The caller hands over what the
// catalog already knows about every declared set and gets a name back.
//
// ⚠⚠ THERE IS NO SIBLING FOR UBE. Counted across 3843 declared sets on this
// load order, the ones carrying a covering are 6 CBBE/3BA sets, 11 HIMBO sets
// and 2 Apachii outfit sets. Zero UBE, and UBE is the largest family installed
// here at 86 of 222 preset entries. An empty answer is therefore the NORMAL
// case for a large part of the grid rather than an error, and the caller has
// to have something to do with it.

#include "BodyPreset.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace OS::CoveredSets {

    // What the catalog knows about one declared slider set.
    struct Candidate {
        std::string_view name;
        BodyFamily       family{ BodyFamily::kUnknown };
        std::string_view outputFile;  // the .osp's <OutputFile>
    };

    // Tokens that mark a set as the covered build of a body. Read off the sets
    // actually installed here: `CBBE NeverNude`, `CBBE 3BBB Amazing
    // NeverNude`, `HIMBO Body - Vanilla (Nevernude)`, `CBBE Underwear`,
    // `HIMBO Undies for SOS - Briefs`.
    [[nodiscard]] inline bool NameMarksCovered(std::string_view a_lowerName) {
        for (const auto token : { "nevernude", "never nude", "underwear",
                                  "undies" }) {
            if (a_lowerName.find(token) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline std::string LowerOf(std::string_view a_text) {
        std::string out{ a_text };
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    [[nodiscard]] inline std::size_t SharedPrefix(std::string_view a_lhs,
                                                  std::string_view a_rhs) {
        std::size_t n = 0;
        while (n < a_lhs.size() && n < a_rhs.size() && a_lhs[n] == a_rhs[n]) {
            ++n;
        }
        return n;
    }

    // Whether an <OutputFile> names a BODY rather than a garment. Read off
    // what the installed sets actually build: bodies output femalebody,
    // malebody and the _tangent variants of both, while everything else in the
    // catalogue is a garment (torsom, maleunderwear, Dread_Sovereign_Armor).
    //
    // ⚠ THIS IS WHAT STOPS A BODY CARD BUILDING FROM A GARMENT SET. Five HIMBO
    // presets on this rig name `HIMBO Vanilla - Body - Farm Clothes 1`, which
    // has "Body" in its name and outputs `torsom`.
    [[nodiscard]] inline bool OutputIsBody(std::string_view a_outputFile) {
        const auto low = LowerOf(a_outputFile);
        return low.starts_with("femalebody") || low.starts_with("malebody");
    }

    // The best covered BODY set for a family, with no nude set to compare
    // against. This is the fallback for a preset whose own slider set is not
    // installed at all, which is most of HIMBO here: 41 of 55 preset entries
    // name a set no .osp on this machine declares (`HIMBO` alone accounts for
    // 30), and their cards are built from the character's BUILT body instead.
    // That fallback has no slider set to swap, so without this they can never
    // be covered.
    //
    // ⚠ IT IS A GUESS, AND SO IS THE THING IT REPLACES. Building from the
    // family's covered set uses a set the preset was never authored against;
    // falling back to the built body uses whatever that character last built.
    // Both are approximations of a preset whose real set is missing, and this
    // one at least carries the preset's slider values, which transfer by name
    // (89 of 89 on HIMBO). It only ever runs with the option on.
    [[nodiscard]] inline std::string_view BestCoveredForFamily(
        BodyFamily a_family, std::span<const Candidate> a_candidates) {
        if (a_family == BodyFamily::kUnknown) {
            return {};
        }
        std::string_view best;
        bool             bestNever = false;
        for (const auto& cand : a_candidates) {
            if (cand.name.empty() || cand.family != a_family ||
                !OutputIsBody(cand.outputFile)) {
                continue;
            }
            const auto low = LowerOf(cand.name);
            if (!NameMarksCovered(low)) {
                continue;
            }
            const bool never = low.find("nevernude") != std::string::npos ||
                               low.find("never nude") != std::string::npos;
            const bool better =
                best.empty() || (never && !bestNever) ||
                (never == bestNever &&
                 (cand.name.size() < best.size() ||
                  (cand.name.size() == best.size() && cand.name < best)));
            if (better) {
                best      = cand.name;
                bestNever = never;
            }
        }
        return best;
    }

    // The covered sibling of a_nudeSet, or empty when the install has none.
    //
    // A candidate qualifies when it is a different set, of the same family,
    // building the same output file, whose name marks it as covered. Among
    // those the winner is:
    //
    //   1. the longest shared name prefix with the nude set. This is what
    //      picks `HIMBO Body - Vanilla (Nevernude)` for `HIMBO Body - SOS`
    //      over `HIMBO Undies for SOS - Briefs`, which shares only "HIMBO ".
    //   2. then a nevernude marker over an underwear one, because nevernude is
    //      the minimal covering and underwear sets add bow ties and straps.
    //   3. then the shortest name, which prefers `CBBE NeverNude` over `CBBE
    //      NeverNude Physics`. ⚠ Physics is not a thing a still card has, so
    //      losing the physics variant costs the picture nothing.
    //   4. then lexicographic, so two installs with the same sets agree. An
    //      unstable answer here would be a different disk key per session and
    //      a pane that rebuilds forever.
    [[nodiscard]] inline std::string_view CoveredSiblingFor(
        std::string_view a_nudeSet, BodyFamily a_family,
        std::string_view a_outputFile, std::span<const Candidate> a_candidates) {
        if (a_nudeSet.empty() || a_outputFile.empty()) {
            return {};
        }
        const auto nudeLower   = LowerOf(a_nudeSet);
        const auto outputLower = LowerOf(a_outputFile);

        std::string_view best;
        std::size_t      bestPrefix = 0;
        bool             bestNever  = false;
        for (const auto& cand : a_candidates) {
            if (cand.name.empty() || cand.name == a_nudeSet ||
                cand.family != a_family) {
                continue;
            }
            if (LowerOf(cand.outputFile) != outputLower) {
                continue;
            }
            const auto candLower = LowerOf(cand.name);
            if (!NameMarksCovered(candLower)) {
                continue;
            }
            const auto prefix = SharedPrefix(nudeLower, candLower);
            const bool never  = candLower.find("nevernude") != std::string::npos ||
                               candLower.find("never nude") != std::string::npos;
            const bool better =
                best.empty() || prefix > bestPrefix ||
                (prefix == bestPrefix &&
                 ((never && !bestNever) ||
                  (never == bestNever &&
                   (cand.name.size() < best.size() ||
                    (cand.name.size() == best.size() && cand.name < best)))));
            if (better) {
                best       = cand.name;
                bestPrefix = prefix;
                bestNever  = never;
            }
        }
        return best;
    }

}  // namespace OS::CoveredSets
