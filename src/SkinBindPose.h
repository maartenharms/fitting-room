#pragma once

// Where a skinned mesh actually stands, as pure arithmetic.
//
// The engine draws a skinned vertex at `sum_i w_i * boneWorld_i * skinToBone_i * v`
// and never consults the shape's own node transform. A card loads the NIF on its
// own and draws the vertex buffer, which asserts that product is the identity.
// For most armour it is, because the author built in body space. For some it is
// not, and then the card draws the mesh wherever the author happened to be
// working while the game keeps drawing it on the body.
//
// ⚠⚠ MEASURED BEFORE IT WAS WRITTEN (2026-08-20), on the file the field report
// named. `practical_pirate_male_1.nif` puts `pirate_male` at z -86.79..-3.81, and
// its `preview.boxes` line reported item z -86.79..-3.81, agreeing to two
// decimals: the loader was drawing exactly what the file says. Every one of that
// file's 23 and 21 bones gives boneWorld * skinToBone = (0, -1.547, 120.344),
// spread 0.008, so the correction is ONE rigid transform rather than a per-vertex
// sum. The female mesh of the same armour gives the identity on all six of its
// shapes, including a 39-bone SMP skirt, which is why three cards in four are
// already right and why applying this changes nothing on them.
//
// Kept out of MeshExtractor for PreviewFilter.h's reason: no test compiles the
// engine side, so the decidable part lives here and is pinned by test. The engine
// side's only job is to gather the per-bone products and hand them over.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace OS::SkinBindPose {

    // A NiTransform without the engine: row-major 3x3, translation, uniform scale.
    struct Transform {
        std::array<float, 9> rotate{ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        std::array<float, 3> translate{ 0.0f, 0.0f, 0.0f };
        float                scale{ 1.0f };
    };

    [[nodiscard]] inline Transform Compose(const Transform& a_outer,
                                           const Transform& a_inner) {
        Transform out{};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (std::size_t k = 0; k < 3; ++k) {
                    sum += a_outer.rotate[r * 3 + k] * a_inner.rotate[k * 3 + c];
                }
                out.rotate[r * 3 + c] = sum;
            }
        }
        for (std::size_t r = 0; r < 3; ++r) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 3; ++k) {
                sum += a_outer.rotate[r * 3 + k] * (a_outer.scale * a_inner.translate[k]);
            }
            out.translate[r] = sum + a_outer.translate[r];
        }
        out.scale = a_outer.scale * a_inner.scale;
        return out;
    }

    // ⚠ TWO EPSILONS, BECAUSE THE COMPONENTS ARE NOT IN THE SAME UNITS. Rotation
    // entries and scale are around 1; translations are in game units and run to
    // 120 on the file this was measured against. One shared threshold would either
    // wave through a real rotation disagreement or reject an ordinary rounding
    // difference in a translation.
    inline constexpr float kRotationEpsilon    = 0.01f;
    inline constexpr float kTranslationEpsilon = 0.50f;

    // How far the per-bone products disagree with each other. Returned as the pair
    // rather than one number so a log line can say WHICH kind of disagreement.
    struct Disagreement {
        float rotation{ 0.0f };
        float translation{ 0.0f };
    };

    [[nodiscard]] inline Disagreement SpreadOf(std::span<const Transform> a_perBone) {
        Disagreement out{};
        if (a_perBone.size() < 2) {
            return out;
        }
        const auto& first = a_perBone.front();
        for (const auto& t : a_perBone.subspan(1)) {
            for (std::size_t i = 0; i < 9; ++i) {
                out.rotation =
                    (std::max)(out.rotation, std::fabs(t.rotate[i] - first.rotate[i]));
            }
            out.rotation = (std::max)(out.rotation, std::fabs(t.scale - first.scale));
            for (std::size_t i = 0; i < 3; ++i) {
                out.translation = (std::max)(
                    out.translation, std::fabs(t.translate[i] - first.translate[i]));
            }
        }
        return out;
    }

    [[nodiscard]] inline bool IsIdentity(const Transform& a_xf) {
        for (std::size_t i = 0; i < 9; ++i) {
            const float want = (i == 0 || i == 4 || i == 8) ? 1.0f : 0.0f;
            if (std::fabs(a_xf.rotate[i] - want) > kRotationEpsilon) {
                return false;
            }
        }
        if (std::fabs(a_xf.scale - 1.0f) > kRotationEpsilon) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            // ⚠ A TIGHTER BAR THAN kTranslationEpsilon ON PURPOSE. That one asks
            // "do these bones agree", which tolerates rounding across a bone list.
            // This one asks "is there anything to correct", and half a unit of
            // silent drift on every armour card is not nothing.
            if (std::fabs(a_xf.translate[i]) > 0.01f) {
                return false;
            }
        }
        return true;
    }

    enum class Verdict : std::uint8_t {
        kNoSkin,    // nothing to read: not skinned, or no bones resolved
        kAlready,   // the product is the identity, so the vertices are already right
        kApply,     // one rigid correction, agreed by every bone
        kPosed,     // the bones disagree, so no single correction is right
    };

    struct Decision {
        Verdict      verdict{ Verdict::kNoSkin };
        Transform    correction{};
        Disagreement spread{};
        std::size_t  bones{ 0 };
    };

    // ⚠⚠ IT DECLINES RATHER THAN GUESSES, AND THE CALLER MUST LOG THE DECLINE.
    // kPosed means the file's bones are not at bind pose relative to each other,
    // so no single transform puts the shape right and a per-vertex weighted sum
    // would be the only honest answer. Correcting it with the first bone's product
    // would move a mesh by a number nobody measured. A refused branch and an absent
    // branch read the same in a log, so the decline has to name itself.
    [[nodiscard]] inline Decision Decide(std::span<const Transform> a_perBone) {
        Decision out{};
        out.bones = a_perBone.size();
        if (a_perBone.empty()) {
            out.verdict = Verdict::kNoSkin;
            return out;
        }
        out.spread     = SpreadOf(a_perBone);
        out.correction = a_perBone.front();
        if (out.spread.rotation > kRotationEpsilon ||
            out.spread.translation > kTranslationEpsilon) {
            out.verdict = Verdict::kPosed;
            return out;
        }
        out.verdict = IsIdentity(out.correction) ? Verdict::kAlready : Verdict::kApply;
        return out;
    }

    [[nodiscard]] inline const char* NameOf(Verdict a_verdict) {
        switch (a_verdict) {
        case Verdict::kNoSkin:  return "no-skin";
        case Verdict::kAlready: return "already-bound";
        case Verdict::kApply:   return "apply";
        case Verdict::kPosed:   return "posed";
        default:                return "?";
        }
    }

}
