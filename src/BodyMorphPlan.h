#pragma once

#include "BodyPreset.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    struct BodyMorphValue {
        std::string name;
        float       value{ 0.0f };

        friend bool operator==(const BodyMorphValue&, const BodyMorphValue&) = default;
    };

    [[nodiscard]] inline std::string BodyMorphLower(std::string_view a_value) {
        std::string out(a_value);
        std::ranges::transform(out, out.begin(), [](unsigned char a_char) {
            return static_cast<char>(std::tolower(a_char));
        });
        return out;
    }

    [[nodiscard]] inline bool BodyMorphUsesUnpPolicy(std::string_view a_sourceSet) {
        const auto lower = BodyMorphLower(a_sourceSet);
        return lower.find("unp") != std::string::npos ||
               lower.find("coco") != std::string::npos ||
               lower.find("bhunp") != std::string::npos ||
               lower.find("uunp") != std::string::npos;
    }

    [[nodiscard]] inline bool BodyMorphIsOBodyInverted(std::string_view a_slider) {
        constexpr std::array<std::string_view, 10> inverted = {
            "Breasts", "BreastsSmall", "NippleDistance", "NippleSize", "ButtCrack",
            "Butt", "ButtSmall", "Legs", "Arms", "ShoulderWidth",
        };
        return std::ranges::find(inverted, a_slider) != inverted.end();
    }

    [[nodiscard]] inline float BodyMorphConvertPercent(
        BodyFamily, std::string_view a_sourceSet, std::string_view a_slider,
        float a_percent) {
        const float normalized = a_percent / 100.0f;
        return BodyMorphUsesUnpPolicy(a_sourceSet) &&
                       BodyMorphIsOBodyInverted(a_slider)
                   ? 1.0f - normalized
                   : normalized;
    }

    [[nodiscard]] inline float BodyMorphInterpolatePercent(
        const BodySliderValue& a_slider, float a_actorWeightPercent) {
        const float weight = a_actorWeightPercent / 100.0f;
        return a_slider.smallValue +
               (a_slider.bigValue - a_slider.smallValue) * weight;
    }

    // ---- push-up, which is a recipe per body and not one slider ---------------
    //
    // ⚠⚠ MEASURED WITH tools/tri_morphs.py, NEVER GUESSED. A morph name the
    // body does not carry is a SILENT no-op through SetMorph, so a wrong string
    // here is a feature that ships doing nothing. On the reference load order,
    // 2026-08-26:
    //
    //   CBBE  femalebody.tri           97 morphs   PushUp, BreastCleavage
    //   3BA   femalebody.tri          154 morphs   PushUp, BreastCleavage
    //   UBE   femalebody_tangent.tri  238 morphs   NEITHER
    //
    // ⚠⚠ AND THE ' n|p' IS PART OF THE NAME. UBE spells a bidirectional slider
    // 'BreastsCupSag n|p'. It is not two morphs with N and P suffixes and it is
    // not 'BreastsCupSag'. Read straight out of the tri, quoted, because every
    // plausible guess at that suffix is wrong and fails silently.
    //
    // ⚠ NEGATIVE LIFTS on the two that point downward by default: sag going
    // negative raises the breast and gap width going negative closes the
    // cleavage. A positive value on either is the opposite of the feature.
    //
    // ⚠ THIS COMPOUNDS with whatever BodySlide already baked into the body.
    // It adds lift on top of the player's own preset and cannot dial an
    // armour's built-in push-up back down, so the copy says "adds" and never
    // "sets".
    [[nodiscard]] inline std::vector<BodyMorphValue> PushUpIngredients(
        BodyFamily a_family) {
        switch (a_family) {
            case BodyFamily::k3BA:
            case BodyFamily::kCBBE:
                return { { "PushUp", 0.75f }, { "BreastCleavage", 0.45f } };
            case BodyFamily::kUBE:
                return { { "Breasts_Perky", 0.80f },
                         { "BreastsCupSag n|p", -0.70f },
                         { "BreastUpperCurve n|p", 0.60f },
                         { "BreastCenterGapWidth n|p", -0.50f } };
            case BodyFamily::kHIMBO:
            case BodyFamily::kGenericV1:
            case BodyFamily::kUnknown:
                break;
        }
        return {};
    }

    // What the editor asks before it draws the control at all. The row is
    // HIDDEN on a body with no recipe rather than shown doing nothing, so this
    // and the plan below must never disagree; the suite checks that they agree
    // for every family.
    [[nodiscard]] inline bool HasPushUpRecipe(BodyFamily a_family) {
        return !PushUpIngredients(a_family).empty();
    }

    // ⚠ ONE RECIPE SCALED, NOT A SECOND RECIPE. Subtle is the same
    // ingredients nearer zero, so there is one set of values to eyeball in game
    // instead of one per level, and the sign of every ingredient is preserved.
    [[nodiscard]] inline float PushUpStrength(PushUpMode a_mode) {
        switch (a_mode) {
            case PushUpMode::kSubtle: return 0.5f;
            case PushUpMode::kFull: return 1.0f;
            case PushUpMode::kNone: break;
        }
        return 0.0f;
    }

    [[nodiscard]] inline std::vector<BodyMorphValue> BuildPushUpMorphPlan(
        BodyFamily a_family, PushUpMode a_mode) {
        const float strength = PushUpStrength(a_mode);
        if (strength <= 0.0f) {
            return {};  // off writes nothing; ClearOwned is what takes it off
        }
        auto plan = PushUpIngredients(a_family);
        for (auto& morph : plan) {
            morph.value *= strength;
        }
        return plan;
    }

    [[nodiscard]] inline std::vector<BodyMorphValue> BuildBodyMorphPlan(
        const BodyPreset& a_preset, float a_actorWeightPercent) {
        std::vector<BodyMorphValue> plan;
        plan.reserve(a_preset.sliders.size());
        for (const auto& slider : a_preset.sliders) {
            const float percent = BodyMorphInterpolatePercent(slider, a_actorWeightPercent);
            const float value = BodyMorphConvertPercent(
                a_preset.family, a_preset.sourceSet, slider.name, percent);
            if (std::isfinite(value) && value != 0.0f) {
                plan.push_back({ slider.name, value });
            }
        }
        return plan;
    }

}  // namespace OS
