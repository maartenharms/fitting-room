#pragma once

#include "BodyPreset.h"

#include <json/json.h>

#include <string>
#include <string_view>

// The BodyPreset file codec, pulled out of BodyPresetStore so the store and
// the profile codec read ONE serializer. A profile embeds a custom preset
// payload verbatim (its body block is self-contained where the co-save is
// not), and a second hand-rolled reader of the same struct is the drift
// two-readers-of-one-answer exists to prevent.
namespace OS::BodyPresetJson {

    inline constexpr std::size_t kMaxSliders = 2048;
    inline constexpr std::size_t kMaxText    = 512;

    [[nodiscard]] std::string Lower(std::string_view a_value);
    [[nodiscard]] std::string Trimmed(std::string_view a_value);

    [[nodiscard]] Json::Value ToJson(const BodyPreset& a_preset);

    // Schema version, bounded metadata, finite non-duplicate sliders. On
    // failure a_error carries one author-readable line.
    [[nodiscard]] bool Validate(const BodyPreset& a_preset, std::string& a_error);

    [[nodiscard]] bool FromJson(const Json::Value& a_root, BodyPreset& a_out,
                                std::string& a_error);

}  // namespace OS::BodyPresetJson
