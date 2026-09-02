#include "BodyStudioProofData.h"

#include <tinyxml2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace OS::BodyStudioProofData {

    using namespace std::literals;

    namespace {
        constexpr std::uintmax_t kMaxProofXmlBytes = 4u * 1024u * 1024u;
        constexpr std::array<std::string_view, 10> kOBodyInvertedSliders = {
            "Breasts", "BreastsSmall", "NippleDistance", "NippleSize", "ButtCrack",
            "Butt", "ButtSmall", "Legs", "Arms", "ShoulderWidth",
        };
        constexpr std::array<std::string_view, 9> kClothedTokens = {
            "cloth", "outfit", "nevernude", "bikini", "feet",
            "hands", "push", "cleavage", "armor",
        };

        [[nodiscard]] std::string Lower(std::string_view a_value) {
            std::string out(a_value);
            std::ranges::transform(out, out.begin(), [](unsigned char a_char) {
                return static_cast<char>(std::tolower(a_char));
            });
            return out;
        }

        template <std::size_t N>
        [[nodiscard]] bool ContainsAny(std::string_view a_value,
                                       const std::array<std::string_view, N>& a_tokens) {
            const auto lower = Lower(a_value);
            return std::ranges::any_of(a_tokens, [&](std::string_view a_token) {
                return lower.find(a_token) != std::string::npos;
            });
        }

        [[nodiscard]] bool IsClothed(std::string_view a_value) {
            return ContainsAny(a_value, kClothedTokens);
        }

        [[nodiscard]] Family ResolveFamily(std::string_view a_set,
                                           const std::vector<std::string>& a_groups) {
            std::string signature(a_set);
            for (const auto& group : a_groups) {
                signature.push_back('|');
                signature += group;
            }
            const auto lower = Lower(signature);
            if (lower.find("himbo") != std::string::npos) {
                return Family::kHIMBO;
            }
            if (lower.find("ube") != std::string::npos) {
                return Family::kUBE;
            }
            if (lower.find("3ba") != std::string::npos ||
                lower.find("3bbb") != std::string::npos ||
                lower.find("cbbe 3bbb") != std::string::npos) {
                return Family::k3BA;
            }
            return Family::kOther;
        }

        [[nodiscard]] bool IsXml(const std::filesystem::path& a_path) {
            return Lower(a_path.extension().string()) == ".xml";
        }
    }  // namespace

    const char* FamilyName(Family a_family) {
        switch (a_family) {
            case Family::k3BA: return "3BA";
            case Family::kUBE: return "UBE";
            case Family::kHIMBO: return "HIMBO";
            default: return "Other";
        }
    }

    bool IsUnpSourceSet(std::string_view a_sourceSet) {
        constexpr std::array tokens{ "unp"sv, "coco"sv, "bhunp"sv, "uunp"sv };
        return ContainsAny(a_sourceSet, tokens);
    }

    float ConvertEndpoint(std::string_view a_sourceSet, std::string_view a_slider,
                          float a_bodySlidePercent) {
        const float normalized = a_bodySlidePercent / 100.0f;
        const bool invert = IsUnpSourceSet(a_sourceSet) &&
                            std::ranges::find(kOBodyInvertedSliders, a_slider) !=
                                kOBodyInvertedSliders.end();
        return invert ? 1.0f - normalized : normalized;
    }

    std::vector<Preset> ParseMatches(std::string_view a_xml, std::string_view a_name,
                                     std::string& a_error) {
        a_error.clear();
        std::vector<Preset> out;
        tinyxml2::XMLDocument doc;
        if (doc.Parse(a_xml.data(), a_xml.size()) != tinyxml2::XML_SUCCESS) {
            a_error = doc.ErrorStr() ? doc.ErrorStr() : "invalid XML";
            return out;
        }
        auto* root = doc.FirstChildElement("SliderPresets");
        if (!root) {
            a_error = "missing SliderPresets root";
            return out;
        }
        for (auto* node = root->FirstChildElement("Preset"); node;
             node = node->NextSiblingElement("Preset")) {
            const char* name = node->Attribute("name");
            if (!name || a_name != name || IsClothed(name)) {
                continue;
            }
            const char* sourceSet = node->Attribute("set");
            if (!sourceSet || !*sourceSet) {
                a_error = "matching preset has no source set";
                continue;
            }

            Preset preset;
            preset.name      = name;
            preset.sourceSet = sourceSet;
            for (auto* group = node->FirstChildElement("Group"); group;
                 group = group->NextSiblingElement("Group")) {
                if (const char* groupName = group->Attribute("name");
                    groupName && *groupName) {
                    preset.groups.emplace_back(groupName);
                }
            }
            preset.family = ResolveFamily(preset.sourceSet, preset.groups);

            // Match OBody 4.4.x's AddSliderToSet semantics: endpoints start at
            // zero and the first non-zero value for an endpoint wins. Its UNP
            // inversion is applied to each present endpoint before this merge;
            // an absent endpoint remains zero rather than becoming one.
            std::map<std::string, Slider, std::less<>> sliders;
            bool malformed = false;
            for (auto* slider = node->FirstChildElement("SetSlider"); slider;
                 slider = slider->NextSiblingElement("SetSlider")) {
                const char* sliderName = slider->Attribute("name");
                const char* size       = slider->Attribute("size");
                float       percent    = 0.0f;
                if (!sliderName || !*sliderName || !size ||
                    (std::string_view(size) != "small" && std::string_view(size) != "big") ||
                    slider->QueryFloatAttribute("value", &percent) != tinyxml2::XML_SUCCESS ||
                    !std::isfinite(percent)) {
                    malformed = true;
                    continue;
                }
                auto [it, inserted] = sliders.try_emplace(sliderName, Slider{ sliderName });
                auto& endpoint = std::string_view(size) == "big" ? it->second.big
                                                                  : it->second.small;
                const float converted = ConvertEndpoint(preset.sourceSet, sliderName, percent);
                if (inserted || (endpoint == 0.0f && converted != 0.0f)) {
                    endpoint = converted;
                }
            }
            if (malformed) {
                a_error = "matching preset contains malformed SetSlider entries";
                continue;
            }
            preset.sliders.reserve(sliders.size());
            for (auto& [_, slider] : sliders) {
                preset.sliders.push_back(std::move(slider));
            }
            out.push_back(std::move(preset));
        }
        return out;
    }

    ResolveReport ResolveInstalled(const std::filesystem::path& a_root,
                                   std::string_view a_name) {
        ResolveReport report;
        if (a_name.empty()) {
            report.error = "select an installed preset first";
            return report;
        }
        std::error_code ec;
        if (!std::filesystem::exists(a_root, ec) || ec) {
            report.error = "BodySlide SliderPresets directory is unavailable";
            return report;
        }

        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(a_root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file(ec) && !ec && IsXml(it->path()) &&
                !IsClothed(it->path().filename().string())) {
                files.push_back(it->path());
            }
        }
        if (ec) {
            report.error = "SliderPresets directory scan failed: " + ec.message();
            return report;
        }
        std::ranges::sort(files);

        std::vector<Preset> matches;
        for (const auto& path : files) {
            ++report.filesScanned;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxProofXmlBytes) {
                ++report.filesRejected;
                ec.clear();
                continue;
            }
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            if (!in.good() && !in.eof()) {
                ++report.filesRejected;
                continue;
            }
            std::string error;
            auto parsed = ParseMatches(buffer.str(), a_name, error);
            if (!error.empty()) {
                ++report.filesRejected;
            }
            matches.insert(matches.end(), std::make_move_iterator(parsed.begin()),
                           std::make_move_iterator(parsed.end()));
        }

        report.matches = matches.size();
        if (matches.empty()) {
            report.error = "no exact BodySlide XML preset match";
            return report;
        }
        if (matches.size() != 1) {
            report.error = "preset name resolves to multiple BodySlide slider sets";
            return report;
        }
        report.preset = std::move(matches.front());
        report.found  = true;
        return report;
    }

    std::vector<RaceMenuMorphApi::MorphValue> BuildPlan(
        const Preset& a_preset, float a_actorWeightPercent) {
        const float weight = a_actorWeightPercent / 100.0f;
        std::vector<RaceMenuMorphApi::MorphValue> plan;
        plan.reserve(a_preset.sliders.size());
        for (const auto& slider : a_preset.sliders) {
            const float value = slider.small + (slider.big - slider.small) * weight;
            if (value != 0.0f) {
                plan.push_back({ slider.name, value });
            }
        }
        return plan;
    }

}  // namespace OS::BodyStudioProofData
