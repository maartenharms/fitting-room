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
                if (!entry.style.Empty()) {
                    s["mod"] = entry.style.modName;
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "0x%06X",
                                  entry.style.localFormID);
                    s["id"] = hex;
                }
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

        // Optional body dimension. Omitting defaults keeps legacy outfit and
        // preset files stable while allowing the global library to persist the
        // same per-outfit OBody state as the co-save codec.
        if (!a_outfit.obodyPreset.empty()) {
            o["obodyPreset"] = a_outfit.obodyPreset;
        }
        if (a_outfit.orefit != ORefitMode::kDefault) {
            o["orefit"] = static_cast<unsigned int>(a_outfit.orefit);
        }

        // Weapon dimension (weapon + quiver transmog), same shape as the
        // slots array above but keyed by weapon-class name instead of an
        // editor slot number. Omitted entirely when every weapon entry is
        // passthrough, so an armor-only outfit serializes byte-identically
        // to a pre-weapons-array (0.1.1-era) file.
        Json::Value weapons(Json::arrayValue);
        const auto appendWeapon = [&](WeaponClass a_class, WeaponHand a_hand,
                                      const SlotEntry& a_entry) {
            Json::Value w;
            w["class"] = ClassJsonName(a_class);
            if (a_hand != WeaponHand::Both) {
                w["hand"] = HandJsonName(a_hand);
            }
            if (a_entry.kind == SlotEntry::Kind::kHide) {
                w["kind"] = "hide";
            } else if (a_entry.kind == SlotEntry::Kind::kPassthrough) {
                w["kind"] = "passthrough";
            } else {
                w["kind"] = "style";
                w["mod"]  = a_entry.style.modName;
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%06X", a_entry.style.localFormID);
                w["id"] = hex;
            }
            weapons.append(std::move(w));
        };
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto  wc    = static_cast<WeaponClass>(c);
            const auto& entry = a_outfit.WeaponEntryFor(wc);
            if (entry.kind != SlotEntry::Kind::kPassthrough) {
                appendWeapon(wc, WeaponHand::Both, entry);
            }
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                if (const auto& over = a_outfit.WeaponOverrideFor(wc, hand);
                    over) {
                    appendWeapon(wc, hand, *over);
                }
            }
        }
        if (!weapons.empty()) {
            o["weapons"] = std::move(weapons);
        }
        return o;
    }

    bool JsonToOutfit(const Json::Value& a_json, Outfit& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        a_out.name     = a_json.get("name", "Outfit").asString();
        a_out.favorite = a_json.get("favorite", false).asBool();
        const auto& preset = a_json["obodyPreset"];
        a_out.obodyPreset  = preset.isString() ? preset.asString() : std::string{};
        const auto& refit = a_json["orefit"];
        a_out.orefit      = ORefitMode::kDefault;
        if (refit.isUInt()) {
            const auto value = refit.asUInt();
            if (value <= static_cast<unsigned int>(ORefitMode::kForceOff)) {
                a_out.orefit = static_cast<ORefitMode>(value);
            }
        }
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
                StyleRefKey covered;
                covered.modName = s.get("mod", "").asString();
                covered.localFormID = static_cast<std::uint32_t>(
                    std::strtoul(s.get("id", "0").asString().c_str(), nullptr, 16));
                a_out.SetHiddenWithRestore(bit, std::move(covered));
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
        for (const auto& w : a_json["weapons"]) {
            if (!w.isObject()) {
                continue;
            }
            const auto classOpt = ClassFromJsonName(w.get("class", "").asString());
            if (!classOpt) {
                continue;
            }
            const auto handOpt =
                HandFromJsonName(w.get("hand", "").asString());
            if (!handOpt) {
                continue;
            }
            const auto wc   = *classOpt;
            const auto hand = *handOpt;
            if (hand != WeaponHand::Both && !SupportsHandOverrides(wc)) {
                continue;
            }
            const auto kind = w.get("kind", "").asString();
            if (kind == "hide") {
                a_out.SetWeaponHide(wc, hand);
            } else if (kind == "passthrough" && hand != WeaponHand::Both) {
                a_out.SetWeaponPassthrough(wc, hand);
            } else if (kind == "style") {
                StyleRefKey key;
                key.modName     = w.get("mod", "").asString();
                key.localFormID = static_cast<std::uint32_t>(
                    std::strtoul(w.get("id", "0").asString().c_str(), nullptr, 16));
                if (!key.Empty()) {
                    a_out.SetWeaponStyle(wc, std::move(key), hand);
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
        JsonToOutfit(a_root, a_out.outfit);  // same object: name + slots + weapons
        // Usable-content gate: any non-passthrough armor slot OR weapon entry
        // counts (ChangedSlotCount vs a default outfit covers both dimensions)
        // - a weapons-only preset is a perfectly good preset.
        if (ChangedSlotCount(Outfit{}, a_out.outfit) == 0) {
            a_error = "no usable \"slots\" or \"weapons\" entries (a preset must "
                      "style or hide at least one armor slot 30-61 or weapon class)";
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
