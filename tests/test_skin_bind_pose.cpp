#include "SkinBindPose.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

namespace S = OS::SkinBindPose;

static bool Near(float a_lhs, float a_rhs, float a_tol = 0.001f) {
    return std::fabs(a_lhs - a_rhs) <= a_tol;
}

// ⚠⚠ THE NUMBERS BELOW ARE MEASURED, NOT INVENTED. They come off the two files
// the 2026-08-20 field report named, read with tools/nif_bindpose.py:
//
//   practical_pirate_male_1.nif   23 bones and 21 bones, every one of them
//                                 boneWorld * skinToBone = (0, -1.547, 120.344),
//                                 translation spread 0.0083
//   practical_pirate_1.nif        six shapes, every bone the identity, including
//                                 a 39-bone SMP skirt chain
//
// and the vertex extents come from tools/nif_bounds.py on the same files. A test
// written from a plan rather than from the files would have passed against the
// wrong arithmetic just as happily.
constexpr float kMaleOffsetY = -1.547f;
constexpr float kMaleOffsetZ = 120.344f;
constexpr float kMaleSpread  = 0.0083f;

// practical_pirate_male_1.nif, shape `pirate_male`, as the card drew it.
constexpr float kRawMinZ = -86.7868f;
constexpr float kRawMaxZ = -3.8115f;

// The mannequin box and head box off the card's own preview.boxes line.
constexpr float kMannFloorZ = 11.32f;
constexpr float kHeadTopZ   = 131.85f;

static S::Transform Translated(float a_x, float a_y, float a_z) {
    S::Transform out{};
    out.translate = { a_x, a_y, a_z };
    return out;
}

int main() {
    // ---- Compose ---------------------------------------------------------
    {
        const S::Transform identity{};
        const auto         both = S::Compose(identity, identity);
        CHECK(S::IsIdentity(both));

        // The case the whole header exists for: a pure translation applied to
        // vertices that carry none of their own.
        const auto lift = S::Compose(Translated(0.0f, kMaleOffsetY, kMaleOffsetZ),
                                     identity);
        CHECK(Near(lift.translate[2], kMaleOffsetZ));
        CHECK(Near(lift.translate[1], kMaleOffsetY));

        // Rotation has to actually turn the inner translation, or a bone with a
        // rotated bind would be corrected along the wrong axis. A quarter turn
        // about z sends +x to +y.
        S::Transform quarterTurn{};
        quarterTurn.rotate = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        const auto turned = S::Compose(quarterTurn, Translated(5.0f, 0.0f, 0.0f));
        CHECK(Near(turned.translate[0], 0.0f));
        CHECK(Near(turned.translate[1], 5.0f));

        // Scale multiplies through to the inner translation, not just to the
        // scale field.
        S::Transform doubled{};
        doubled.scale      = 2.0f;
        const auto scaled  = S::Compose(doubled, Translated(0.0f, 0.0f, 10.0f));
        CHECK(Near(scaled.translate[2], 20.0f));
        CHECK(Near(scaled.scale, 2.0f));
    }

    // ---- IsIdentity ------------------------------------------------------
    {
        CHECK(S::IsIdentity(S::Transform{}));
        CHECK(S::IsIdentity(Translated(0.0f, 0.0f, 0.005f)));
        CHECK(!S::IsIdentity(Translated(0.0f, 0.0f, 0.02f)));
        // The measured correction must never read as "nothing to do".
        CHECK(!S::IsIdentity(Translated(0.0f, kMaleOffsetY, kMaleOffsetZ)));

        S::Transform bigScale{};
        bigScale.scale = 1.5f;
        CHECK(!S::IsIdentity(bigScale));
    }

    // ---- the file that measures correct ----------------------------------
    {
        const std::vector<S::Transform> allIdentity(39, S::Transform{});
        const auto                      decision = S::Decide(allIdentity);
        CHECK(decision.verdict == S::Verdict::kAlready);
        CHECK(decision.bones == 39);
        CHECK(Near(decision.spread.translation, 0.0f));
    }

    // ---- the file that does not ------------------------------------------
    {
        std::vector<S::Transform> male(23,
                                       Translated(0.0f, kMaleOffsetY, kMaleOffsetZ));
        // The real spread, put on one bone so the tolerance is exercised rather
        // than assumed.
        male.back().translate[2] += kMaleSpread;

        const auto decision = S::Decide(male);
        CHECK(decision.verdict == S::Verdict::kApply);
        CHECK(decision.bones == 23);
        CHECK(decision.spread.translation <= S::kTranslationEpsilon);
        CHECK(Near(decision.correction.translate[2], kMaleOffsetZ));

        // And the point of all of it: the corrected mesh stands on the figure.
        const float lowZ  = S::Compose(decision.correction,
                                       Translated(0.0f, 0.0f, kRawMinZ))
                               .translate[2];
        const float highZ = S::Compose(decision.correction,
                                       Translated(0.0f, 0.0f, kRawMaxZ))
                                .translate[2];
        CHECK(Near(lowZ, 33.557f, 0.01f));
        CHECK(Near(highZ, 116.533f, 0.01f));
        CHECK(lowZ > kMannFloorZ);   // it was at -86.79, below the floor
        CHECK(highZ < kHeadTopZ);    // and it does not now tower over the head
    }

    // ---- the mannequin, which is the reason this declines at all ----------
    {
        // ⚠⚠ THIS IS THE REGRESSION THE DECLINE EXISTS TO PREVENT, and it was
        // caught by measuring before building rather than by a broken card.
        // The HIMBO body this rig uses for the mannequin (malebody_1.nif) has
        // 31 bones whose products do NOT agree: y runs -2.159..0.003 and z runs
        // -0.000..3.539, a spread of 3.5392. Its vertices are nonetheless
        // correct, because what the engine draws is the WEIGHTED SUM and that
        // comes out at the bind pose even where the individual products do not.
        // Correcting it by the first bone's product would have moved the
        // mannequin on every card in the app.
        std::vector<S::Transform> himbo(31, S::Transform{});
        himbo[7]  = Translated(0.001f, -2.159f, 0.0f);
        himbo[19] = Translated(0.0f, 0.003f, 3.539f);

        const auto decision = S::Decide(himbo);
        CHECK(decision.verdict == S::Verdict::kPosed);
        CHECK(decision.bones == 31);
        CHECK(decision.spread.translation > 3.5f);

        // The two files sit either side of the threshold by a wide margin, so
        // the number is not balanced on a knife edge: 0.0083 accepted,
        // 3.5392 declined, the bar at 0.50.
        CHECK(kMaleSpread * 10.0f < S::kTranslationEpsilon);
        CHECK(decision.spread.translation > S::kTranslationEpsilon * 5.0f);
    }

    // ---- a file whose bones really are posed ------------------------------
    {
        // ⚠ THE DECLINE IS THE FEATURE. Bones that disagree mean no single
        // transform is right, and the first bone's product is not a safe guess.
        std::vector<S::Transform> posed{
            Translated(0.0f, 0.0f, 120.344f),
            Translated(0.0f, 0.0f, 44.0f),
            Translated(0.0f, 0.0f, 91.2f),
        };
        const auto decision = S::Decide(posed);
        CHECK(decision.verdict == S::Verdict::kPosed);
        CHECK(decision.spread.translation > S::kTranslationEpsilon);

        // A rotation disagreement alone is enough, with translations agreeing.
        std::vector<S::Transform> twisted(3, S::Transform{});
        twisted[1].rotate = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        const auto twistDecision = S::Decide(twisted);
        CHECK(twistDecision.verdict == S::Verdict::kPosed);
        CHECK(twistDecision.spread.rotation > S::kRotationEpsilon);
    }

    // ---- nothing to read --------------------------------------------------
    {
        const auto decision = S::Decide({});
        CHECK(decision.verdict == S::Verdict::kNoSkin);
        CHECK(decision.bones == 0);
    }

    // ---- one bone cannot disagree with itself -----------------------------
    {
        const std::vector<S::Transform> lone{
            Translated(0.0f, kMaleOffsetY, kMaleOffsetZ)
        };
        const auto decision = S::Decide(lone);
        CHECK(decision.verdict == S::Verdict::kApply);
        CHECK(Near(decision.spread.translation, 0.0f));
    }

    if (g_failures == 0) {
        std::printf("SkinBindPose: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
