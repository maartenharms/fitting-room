#include "JsonCodec.h"

#include "SlotMask.h"

#include <cstdio>
#include <cstdlib>

namespace OS::JsonCodec {

    Json::Value OutfitToJson(const Outfit& a_outfit) {
        Json::Value o;
        o["name"]     = a_outfit.name;
        o["favorite"] = a_outfit.favorite;
        Json::Value slots(Json::arrayValue);
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            const auto& entry = a_outfit.EntryFor(bit);
            if (entry.kind == SlotEntry::Kind::kPassthrough) {
                continue;
            }
            Json::Value s;
            s["slot"] = bit + 30;
            if (entry.kind == SlotEntry::Kind::kHide) {
                s["kind"] = "hide";
            } else {
                s["kind"] = "style";
                s["mod"]  = entry.style.modName;
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%06X", entry.style.localFormID);
                s["id"] = hex;
            }
            slots.append(std::move(s));
        }
        o["slots"] = std::move(slots);
        return o;
    }

    bool JsonToOutfit(const Json::Value& a_json, Outfit& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        a_out.name     = a_json.get("name", "Outfit").asString();
        a_out.favorite = a_json.get("favorite", false).asBool();
        for (const auto& s : a_json["slots"]) {
            if (!s.isObject()) {
                continue;
            }
            const auto slot = s.get("slot", 0).asUInt();
            if (slot < 30 || slot > 61) {
                continue;
            }
            const auto bit  = BitForEditorSlot(slot);
            const auto kind = s.get("kind", "").asString();
            if (kind == "hide") {
                a_out.SetHide(bit);
            } else if (kind == "style") {
                StyleRefKey key;
                key.modName     = s.get("mod", "").asString();
                key.localFormID = static_cast<std::uint32_t>(
                    std::strtoul(s.get("id", "0").asString().c_str(), nullptr, 16));
                if (!key.Empty()) {
                    a_out.SetStyle(bit, std::move(key));
                }
            }
        }
        return true;
    }

    bool ParsePreset(const Json::Value& a_root, Preset& a_out, std::string& a_error) {
        if (!a_root.isObject()) {
            a_error = "root is not a JSON object";
            return false;
        }
        const int version = a_root.get("version", 0).asInt();
        if (version != kPresetVersion) {
            a_error = "version " + std::to_string(version) + " (this build reads " +
                      std::to_string(kPresetVersion) + " - a newer Fitting Room may)";
            return false;
        }
        a_out.name = a_root.get("name", "").asString();
        if (a_out.name.empty()) {
            a_error = "\"name\" is required";
            return false;
        }
        a_out.author      = a_root.get("author", "").asString();
        a_out.description = a_root.get("description", "").asString();

        a_out.requires_.clear();
        if (a_root.isMember("requires")) {
            const auto& req = a_root["requires"];
            if (!req.isArray()) {
                a_error = "\"requires\" must be an array of plugin filenames";
                return false;
            }
            for (const auto& r : req) {
                if (!r.isString() || r.asString().empty()) {
                    a_error = "\"requires\" entries must be non-empty strings";
                    return false;
                }
                a_out.requires_.push_back(r.asString());
            }
        }

        a_out.outfit = Outfit{};
        JsonToOutfit(a_root, a_out.outfit);  // same object: name + slots
        if ((a_out.outfit.StyleMask() | a_out.outfit.HideMask()) == 0) {
            a_error = "no usable \"slots\" entries (a preset must style or hide "
                      "at least one slot 30-61)";
            return false;
        }
        return true;
    }

    Json::Value PresetToJson(const Outfit& a_outfit, const std::string& a_author,
                             const std::string& a_description,
                             const std::vector<std::string>& a_requires) {
        Json::Value root = OutfitToJson(a_outfit);
        root.removeMember("favorite");  // meaningless in a shipped preset
        root["version"]     = kPresetVersion;
        root["author"]      = a_author;
        root["description"] = a_description;
        Json::Value req(Json::arrayValue);
        for (const auto& r : a_requires) {
            req.append(r);
        }
        root["requires"] = std::move(req);
        return root;
    }

}  // namespace OS::JsonCodec
