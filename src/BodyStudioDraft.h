#pragma once

#include "BodyPreset.h"

#include <cmath>
#include <optional>
#include <ranges>

namespace OS {

    [[nodiscard]] inline const char* BodyStudioCommitLabel(bool a_existingCustom) {
        return a_existingCustom ? "Save changes" : "Create custom preset";
    }

    [[nodiscard]] inline bool CanMakeEmptyBodyDraft(const BodyPreset& a_source) {
        return !a_source.sourceSet.empty() && a_source.sex != BodySex::kUnknown &&
               a_source.family != BodyFamily::kUnknown && !a_source.sliders.empty() &&
               std::ranges::all_of(a_source.sliders, [](const BodySliderValue& a_slider) {
                   return !a_slider.name.empty() && std::isfinite(a_slider.smallValue) &&
                          std::isfinite(a_slider.bigValue);
               });
    }

    // Empty body is an authoring seed, not a source-free preset. Preserve the
    // resolved project identity and complete slider schema, then clear only the
    // values and stable custom identity. That keeps runtime conversion and XML
    // export data-driven for 3BA, UBE, HIMBO and future adapters.
    [[nodiscard]] inline std::optional<BodyPreset> MakeEmptyBodyDraft(
        const BodyPreset& a_source) {
        if (!CanMakeEmptyBodyDraft(a_source)) return std::nullopt;

        BodyPreset draft = a_source;
        draft.version = BodyPreset::kSchemaVersion;
        draft.id.clear();
        draft.name = "Untitled body";
        for (auto& slider : draft.sliders) {
            slider.smallValue = 0.0f;
            slider.bigValue = 0.0f;
        }
        return draft;
    }

}  // namespace OS
