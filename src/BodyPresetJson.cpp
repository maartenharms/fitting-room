#include "BodyPresetJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace OS::BodyPresetJson {

    std::string Lower(std::string_view a_value) {
        std::string out(a_value);
        std::ranges::transform(out, out.begin(), [](unsigned char a_char) {
            return static_cast<char>(std::tolower(a_char));
        });
        return out;
    }

    std::string Trimmed(std::string_view a_value) {
        const auto begin = a_value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = a_value.find_last_not_of(" \t\r\n");
        return std::string(a_value.substr(begin, end - begin + 1));
    }

    Json::Value ToJson(const BodyPreset& a_preset) {
        Json::Value root(Json::objectValue);
        root["version"] = a_preset.version;
        root["id"] = a_preset.id;
        root["name"] = a_preset.name;
        root["sex"] = std::string(BodySexId(a_preset.sex));
        root["bodyFamily"] = std::string(BodyFamilyId(a_preset.family));
        root["sourceSet"] = a_preset.sourceSet;
        root["sourcePreset"] = a_preset.sourcePreset;
        for (const auto& group : a_preset.groups) root["groups"].append(group);
        for (const auto& slider : a_preset.sliders) {
            Json::Value item(Json::objectValue);
            item["name"] = slider.name;
            item["displayName"] = slider.displayName;
            item["category"] = slider.category;
            item["small"] = slider.smallValue;
            item["big"] = slider.bigValue;
            root["sliders"].append(std::move(item));
        }
        return root;
    }

    bool Validate(const BodyPreset& a_preset, std::string& a_error) {
        if (a_preset.version != BodyPreset::kSchemaVersion || a_preset.id.empty() ||
            a_preset.id.size() > 64 || Trimmed(a_preset.name).empty() ||
            a_preset.name.size() > kMaxText || a_preset.sourceSet.empty() ||
            a_preset.sourceSet.size() > kMaxText || a_preset.sex == BodySex::kUnknown ||
            a_preset.family == BodyFamily::kUnknown || a_preset.sliders.empty() ||
            a_preset.sliders.size() > kMaxSliders) {
            a_error = "preset metadata is incomplete or unsupported";
            return false;
        }
        for (const auto& slider : a_preset.sliders) {
            if (slider.name.empty() || slider.name.size() > kMaxText ||
                !std::isfinite(slider.smallValue) || !std::isfinite(slider.bigValue)) {
                a_error = "preset contains an invalid slider";
                return false;
            }
        }
        return true;
    }

    namespace {
        [[nodiscard]] bool StringMember(const Json::Value& a_root, const char* a_key,
                                        std::string& a_out) {
            if (!a_root.isMember(a_key) || !a_root[a_key].isString()) return false;
            a_out = a_root[a_key].asString();
            return a_out.size() <= kMaxText;
        }
    }  // namespace

    bool FromJson(const Json::Value& a_root, BodyPreset& a_out, std::string& a_error) {
        try {
            if (!a_root.isObject() || !a_root["version"].isUInt() ||
                a_root["version"].asUInt() != BodyPreset::kSchemaVersion) {
                a_error = "unsupported or missing schema version";
                return false;
            }
            a_out.version = a_root["version"].asUInt();
            std::string sex;
            std::string family;
            if (!StringMember(a_root, "id", a_out.id) ||
                !StringMember(a_root, "name", a_out.name) ||
                !StringMember(a_root, "sex", sex) ||
                !StringMember(a_root, "bodyFamily", family) ||
                !StringMember(a_root, "sourceSet", a_out.sourceSet) ||
                !StringMember(a_root, "sourcePreset", a_out.sourcePreset)) {
                a_error = "missing or oversized metadata";
                return false;
            }
            a_out.sex = BodySexFromId(sex);
            a_out.family = BodyFamilyFromId(family);
            const auto& groups = a_root["groups"];
            if (!groups.isNull() && !groups.isArray()) {
                a_error = "groups must be an array";
                return false;
            }
            for (const auto& group : groups) {
                if (!group.isString() || group.asString().size() > kMaxText) {
                    a_error = "invalid group";
                    return false;
                }
                a_out.groups.push_back(group.asString());
            }
            const auto& sliders = a_root["sliders"];
            if (!sliders.isArray() || sliders.empty() || sliders.size() > kMaxSliders) {
                a_error = "sliders must be a non-empty bounded array";
                return false;
            }
            std::set<std::string> names;
            for (const auto& item : sliders) {
                BodySliderValue slider;
                if (!item.isObject() || !StringMember(item, "name", slider.name) ||
                    !StringMember(item, "displayName", slider.displayName) ||
                    !StringMember(item, "category", slider.category) ||
                    !item["small"].isNumeric() || !item["big"].isNumeric()) {
                    a_error = "invalid slider entry";
                    return false;
                }
                slider.smallValue = item["small"].asFloat();
                slider.bigValue = item["big"].asFloat();
                if (!std::isfinite(slider.smallValue) || !std::isfinite(slider.bigValue) ||
                    slider.name.empty() || !names.insert(Lower(slider.name)).second) {
                    a_error = "non-finite, empty, or duplicate slider";
                    return false;
                }
                if (slider.displayName.empty()) slider.displayName = slider.name;
                if (slider.category.empty()) slider.category = "Other";
                a_out.sliders.push_back(std::move(slider));
            }
            return Validate(a_out, a_error);
        } catch (const Json::Exception&) {
            a_error = "invalid JSON member type";
            return false;
        }
    }

}  // namespace OS::BodyPresetJson
