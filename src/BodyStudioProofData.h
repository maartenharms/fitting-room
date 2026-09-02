#pragma once

#include "RaceMenuMorphApi.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OS::BodyStudioProofData {

    enum class Family {
        k3BA,
        kUBE,
        kHIMBO,
        kOther,
    };

    struct Slider {
        std::string name;
        // RaceMenu-normalized endpoints after applying OBody 4.4.x's source
        // set policy. An absent endpoint remains zero, matching OBody.
        float small{ 0.0f };
        float big{ 0.0f };
    };

    struct Preset {
        std::string              name;
        std::string              sourceSet;
        std::vector<std::string> groups;
        std::vector<Slider>      sliders;
        Family                   family{ Family::kOther };
    };

    struct ResolveReport {
        Preset      preset;
        bool        found{ false };
        std::size_t matches{ 0 };
        std::size_t filesScanned{ 0 };
        std::size_t filesRejected{ 0 };
        std::string error;
    };

    [[nodiscard]] const char* FamilyName(Family a_family);
    [[nodiscard]] bool        IsUnpSourceSet(std::string_view a_sourceSet);
    [[nodiscard]] float       ConvertEndpoint(std::string_view a_sourceSet,
                                              std::string_view a_slider,
                                              float a_bodySlidePercent);

    // Parse every exact-name match from one BodySlide SliderPresets document.
    // The public shape makes the OBody-equivalence math unit-testable without
    // Skyrim or a live mod manager VFS.
    [[nodiscard]] std::vector<Preset> ParseMatches(std::string_view a_xml,
                                                   std::string_view a_name,
                                                   std::string& a_error);

    // Resolve one unambiguous OBody-visible preset from the runtime VFS.
    [[nodiscard]] ResolveReport ResolveInstalled(const std::filesystem::path& a_root,
                                                 std::string_view a_name);

    [[nodiscard]] std::vector<RaceMenuMorphApi::MorphValue> BuildPlan(
        const Preset& a_preset, float a_actorWeightPercent);

}  // namespace OS::BodyStudioProofData
