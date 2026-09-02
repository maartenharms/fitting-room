#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

// Which body mesh is this, reduced to a key two very different sources can be
// compared on.
//
// ⚠⚠ THIS EXISTS BECAUSE ASKING OBody WAS THE WRONG QUESTION. The body preset
// filter used to ask "which body is this character wearing" through OBody, and
// both pickers show a preset by PUTTING IT ON the character, so the answer was
// always whichever preset had just been clicked. Field 2026-08-08: it walked
// from UBE to 3BA and back purely by following the cursor. A mesh path is the
// opposite kind of fact. Nothing this mod does writes one.
//
// The two sources:
//
//   the character   Actor::GetSkin(kBody) -> the armour addon valid for this
//                   race -> bipedModels[sex].GetModel(), which is a path
//                   relative to meshes\, e.g.
//                   "actors\character\character assets\femalebody_1.nif"
//
//   a preset        the slider set's own .osp names what it builds, as an
//                   <OutputPath> and an <OutputFile>, e.g.
//                   "meshes\actors\character\character assets" + "femalebody"
//
// ⚠ RACE AND GENDER BOTH DECIDE IT, which is the whole reason this works here
// (user 2026-08-08). The skin hangs off the race, and the addon carries one
// model per sex, so GetSkin plus bipedModels[sex] answers for exactly the
// character in front of you rather than for the install in general.
namespace RE {
    class Actor;
}

namespace OS {

    // The mesh key for the body this character is actually wearing.
    //
    // ⚠ Actor::GetSkin(kBody) AND NOT THE RACE DIRECTLY, because an individual
    // NPC can override the race's skin and this has to answer for the character
    // in front of you. The addon it resolves to carries one model per sex, so
    // race and gender both land in the answer, which is how the body is decided
    // on the reference install (user 2026-08-08).
    //
    // Empty when it could not be worked out, and every caller fails open on
    // that. Reads engine forms, so main thread or a draw that already reads
    // actor state; it walks a handful of pointers and allocates one string.
    [[nodiscard]] std::string BuiltBodyMeshKey(RE::Actor* a_actor);

    // Lowercased, backslashed, with the parts that differ between the two
    // sources removed: a leading Data\ or meshes\, the .nif extension, and the
    // _0 / _1 weight suffix. What is left is the body, which is what we are
    // asking about.
    //
    // ⚠ THE WEIGHT SUFFIX HAS TO GO AND IT IS NOT COSMETIC. The engine names a
    // specific weight ("femalebody_1.nif") and BodySlide names the pair
    // ("femalebody"), so keeping it would make every honest match fail and the
    // filter would hide everything.
    [[nodiscard]] inline std::string BodyMeshKey(std::string_view a_path) {
        std::string out;
        out.reserve(a_path.size());
        for (const char raw : a_path) {
            const char c = raw == '/' ? '\\' : raw;
            // Collapse repeated separators; the two sources disagree about
            // whether a joined path doubles them.
            if (c == '\\' && (out.empty() || out.back() == '\\')) {
                continue;
            }
            out.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        while (!out.empty() && (out.back() == '\\' || out.back() == ' ')) {
            out.pop_back();
        }

        constexpr std::string_view kData{ "data\\" };
        if (out.starts_with(kData)) {
            out.erase(0, kData.size());
        }
        constexpr std::string_view kMeshes{ "meshes\\" };
        if (out.starts_with(kMeshes)) {
            out.erase(0, kMeshes.size());
        }

        constexpr std::string_view kNif{ ".nif" };
        if (out.size() > kNif.size() && out.ends_with(kNif)) {
            out.erase(out.size() - kNif.size());
        }
        if (out.size() > 2 && out[out.size() - 2] == '_' &&
            (out.back() == '0' || out.back() == '1')) {
            out.erase(out.size() - 2);
        }
        return out;
    }

    // The same key, built from a slider set's two halves.
    [[nodiscard]] inline std::string BodyMeshKey(std::string_view a_outputPath,
                                                 std::string_view a_outputFile) {
        if (a_outputFile.empty()) {
            return {};  // a set that names no output tells us nothing
        }
        std::string joined(a_outputPath);
        if (!joined.empty()) {
            joined.push_back('\\');
        }
        joined.append(a_outputFile);
        return BodyMeshKey(joined);
    }

    // The key turned back into a model path a preview scene can load
    // (OS-204): relative to meshes\, with the extension and the weight suffix
    // the key deliberately threw away put back on.
    //
    // ⚠⚠ GenWeights IS WHAT DECIDES THE SUFFIX, and it is a rule rather than a
    // guess. A slider set with GenWeights builds the _0 and _1 PAIR and never a
    // bare .nif; one without builds the bare file and never the pair. Measured
    // across the 12 distinct output meshes on the reference load order
    // 2026-08-10: eleven declare GenWeights and exist only as _1.nif, the
    // twelfth declares it false and exists only as .nif, with no exceptions and
    // no mesh existing in both forms.
    //
    // ⚠ _1 AND NOT _0, so the card shows the weight-100 build. It is the one
    // the mannequin itself is composed from, so a body card and the figure on
    // every other card are the same build of the same mesh.
    [[nodiscard]] inline std::string BodyMeshModelPath(std::string_view a_key,
                                                       bool             a_genWeights) {
        if (a_key.empty()) {
            return {};
        }
        std::string out(a_key);
        out.append(a_genWeights ? "_1.nif" : ".nif");
        return out;
    }

    // ⚠ BOTH SIDES HAVE TO BE KNOWN FOR THIS TO MEAN ANYTHING. An empty key is
    // "could not be worked out", never "matches nothing", and every caller has
    // to keep failing open on it.
    [[nodiscard]] inline bool BodyMeshKeysComparable(std::string_view a_left,
                                                     std::string_view a_right) {
        return !a_left.empty() && !a_right.empty();
    }

}  // namespace OS
