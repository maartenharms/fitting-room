#include "ProfileCodec.h"

#include "BodyPresetJson.h"
#include "JsonCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace OS::ProfileCodec {

    namespace {

        // The known top-level members. Anything else is a newer build's block
        // and rides in extras, preserved on rewrite.
        constexpr const char* kKnownMembers[] = {
            "version", "name",   "author", "description", "requires", "face",
            "outfit",  "overlays", "makeup", "body",      "shape",    "skin",
            "weight",  "character",
        };

        [[nodiscard]] bool KnownMember(const std::string& a_name) {
            return std::ranges::any_of(kKnownMembers, [&](const char* a_known) {
                return a_name == a_known;
            });
        }

        // The block members whose presence counts toward "every known block
        // failed" (version/name/metadata are the root's, not blocks).
        constexpr const char* kBlockMembers[] = {
            "face", "outfit", "overlays", "makeup", "body", "shape", "skin",
            "weight", "character",
        };

        int ReadSchemaVersion(const Json::Value& a_root) {
            const auto& v = a_root["version"];
            return v.isIntegral() ? v.asInt() : 0;
        }

        [[nodiscard]] std::string StringOr(const Json::Value& a_value,
                                           const char* a_key) {
            const auto& v = a_value[a_key];
            return v.isString() ? v.asString() : std::string{};
        }

        // The one shared colour rule, bridged to the overlay pages' Rgb the
        // same way HairTintFromJsonName bridges it: exactly one parser.
        [[nodiscard]] bool RgbFromJson(const Json::Value& a_value,
                                       OverlayPlan::Rgb& a_out) {
            if (!a_value.isString()) return false;
            const auto c = JsonCodec::ColourFromHex(a_value.asString());
            if (!c.set) return false;
            a_out = OverlayPlan::Rgb{ c.r, c.g, c.b };
            return true;
        }

        [[nodiscard]] std::string RgbToHex(const OverlayPlan::Rgb& a_rgb) {
            return JsonCodec::ColourToHex(DyeChannel{ true, a_rgb.r, a_rgb.g, a_rgb.b });
        }

        [[nodiscard]] bool FiniteNumber(const Json::Value& a_value, float& a_out) {
            if (!a_value.isNumeric()) return false;
            const auto v = a_value.asFloat();
            if (!std::isfinite(v)) return false;
            a_out = v;
            return true;
        }

        // ---- face ----------------------------------------------------------

        [[nodiscard]] bool ParseFace(const Json::Value& a_json, FaceBlock& a_out,
                                     std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            a_out.jslot = StringOr(a_json, "jslot");
            if (a_out.jslot.empty()) {
                a_reason = "\"jslot\" is required";
                return false;
            }
            // Absent defaults to referenced, the side that never deletes a
            // file. Present-but-junk is refused: this field decides whether
            // DeleteCharacter runs, and a guess either leaks a capture or
            // deletes the player's own export.
            a_out.source = FaceSource::kReferenced;
            if (a_json.isMember("source")) {
                const auto source = StringOr(a_json, "source");
                if (source == "captured") {
                    a_out.source = FaceSource::kCaptured;
                } else if (source == "referenced") {
                    a_out.source = FaceSource::kReferenced;
                } else {
                    a_reason = "\"source\" must be \"captured\" or \"referenced\"";
                    return false;
                }
            }
            // ⚠ REFUSED RATHER THAN CLAMPED. This value ends up painted on the
            // character, and half of a malformed colour is a colour nobody
            // chose. Absent is the ordinary case and means the look states no
            // colour.
            a_out.hairColour.reset();
            if (a_json.isMember("hairColor")) {
                const auto& hc = a_json["hairColor"];
                if (!hc.isIntegral() || hc.asInt64() < 0 ||
                    hc.asInt64() > 0xFFFFFF) {
                    a_reason = "\"hairColor\" must be an integer 0..16777215";
                    return false;
                }
                const auto packed = static_cast<std::uint32_t>(hc.asInt64());
                a_out.hairColour  = HairTint{
                    true, static_cast<std::uint8_t>((packed >> 16) & 0xFF),
                    static_cast<std::uint8_t>((packed >> 8) & 0xFF),
                    static_cast<std::uint8_t>(packed & 0xFF)
                };
            }
            a_out.flags = 0;
            if (a_json.isMember("flags")) {
                const auto& flags = a_json["flags"];
                if (!flags.isIntegral() || flags.asInt64() < 0 ||
                    flags.asInt64() > 15) {
                    // ApplyTypes has exactly four maskable bits. A value past
                    // them is a newer or hand-mangled file, and applying HALF
                    // of an unknown claim is how a second painter slips in.
                    a_reason = "\"flags\" must be an integer 0..15";
                    return false;
                }
                a_out.flags = static_cast<std::uint32_t>(flags.asInt64());
            }
            // Absent defaults to Exported, the folder every profile before
            // this field existed meant. Present-but-junk is refused for the
            // apply side's reason: the folder picks the NATIVE, and guessing
            // dispatches against the wrong one.
            a_out.folder = FaceFolder::kExported;
            if (a_json.isMember("folder")) {
                const auto folder = StringOr(a_json, "folder");
                if (folder == "presets") {
                    a_out.folder = FaceFolder::kPresets;
                } else if (folder == "exported") {
                    a_out.folder = FaceFolder::kExported;
                } else {
                    a_reason = "\"folder\" must be \"exported\" or \"presets\"";
                    return false;
                }
            }
            return true;
        }

        // ---- overlays ------------------------------------------------------

        void ParseOverlayEntry(const Json::Value& a_json,
                               std::vector<OverlayEntry>& a_entries,
                               std::set<std::uint32_t>& a_seen) {
            if (!a_json.isObject()) return;
            const auto& index = a_json["index"];
            if (!index.isIntegral() || index.asInt64() < 0) return;
            OverlayEntry entry;
            entry.index = static_cast<std::uint32_t>(index.asInt64());
            if (!a_seen.insert(entry.index).second) return;  // first one wins

            auto& state = entry.state;
            if (a_json["texture"].isString()) {
                state.hasTexture = true;
                state.texture    = a_json["texture"].asString();
            }
            if (a_json["normal"].isString()) {
                state.hasNormal = true;
                state.normal    = a_json["normal"].asString();
            }
            // A malformed colour decodes to "leave that channel alone", the
            // same posture as everywhere else a colour comes off disk.
            if (a_json.isMember("tint") && RgbFromJson(a_json["tint"], state.tint)) {
                state.hasTint = true;
            }
            float alpha = 0.0f;
            if (a_json.isMember("alpha") && FiniteNumber(a_json["alpha"], alpha)) {
                state.hasAlpha = true;
                state.alpha    = OverlayPlan::ClampAlpha(alpha);
            }
            OverlayPlan::Rgb glow{ 0, 0, 0 };
            if (a_json.isMember("glow") && RgbFromJson(a_json["glow"], glow)) {
                state.glow = glow;
            }
            float glowStrength = 0.0f;
            if (a_json.isMember("glowStrength") &&
                FiniteNumber(a_json["glowStrength"], glowStrength)) {
                state.glowStrength = OverlayPlan::ClampGlow(glowStrength);
            }
            const auto& finish = a_json["finish"];
            float gloss = 0.0f;
            float specular = 0.0f;
            if (finish.isObject() && FiniteNumber(finish["gloss"], gloss) &&
                FiniteNumber(finish["specular"], specular)) {
                state.hasFinish = true;
                state.gloss     = OverlayPlan::ClampGloss(gloss);
                state.specular  = OverlayPlan::ClampSpecular(specular);
            }
            const auto& transform = a_json["transform"];
            if (transform.isObject()) {
                OverlayTransform::Transform t;
                float v = 0.0f;
                bool ok = true;
                if (transform.isMember("offsetX")) {
                    ok = ok && FiniteNumber(transform["offsetX"], v) && ((t.offsetX = v), true);
                }
                if (ok && transform.isMember("offsetY")) {
                    ok = ok && FiniteNumber(transform["offsetY"], v) && ((t.offsetY = v), true);
                }
                if (ok && transform.isMember("scale")) {
                    ok = ok && FiniteNumber(transform["scale"], v) && ((t.scale = v), true);
                }
                if (ok && transform.isMember("rotationDeg")) {
                    ok = ok && FiniteNumber(transform["rotationDeg"], v) && ((t.rotationDeg = v), true);
                }
                if (ok) {
                    state.transform = OverlayTransform::Quantise(t);
                }
            }
            a_entries.push_back(std::move(entry));
        }

        [[nodiscard]] bool ParseOverlays(const Json::Value& a_json,
                                         OverlaysBlock& a_out,
                                         std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            bool any = false;
            for (const auto& info : OverlayPlan::kLocations) {
                const auto& list = a_json[info.id];
                if (!list.isArray()) continue;  // junk location drops itself
                auto& entries = a_out.byLocation[OverlayPlan::Slot(info.location)];
                std::set<std::uint32_t> seen;
                for (const auto& entry : list) {
                    ParseOverlayEntry(entry, entries, seen);
                }
                any = any || !entries.empty();
            }
            if (!any) {
                a_reason = "no usable layer entries";
                return false;
            }
            return true;
        }

        // ---- makeup --------------------------------------------------------

        [[nodiscard]] bool MakeupTypeFromJson(const Json::Value& a_value,
                                              std::uint32_t& a_out) {
            if (a_value.isIntegral() && a_value.asInt64() >= 0) {
                a_out = static_cast<std::uint32_t>(a_value.asInt64());
                return true;
            }
            if (a_value.isString()) {
                const auto id = a_value.asString();
                for (const auto& info : MakeupPlan::kTypes) {
                    if (id == info.id) {
                        a_out = static_cast<std::uint32_t>(info.type);
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool ParseMakeup(const Json::Value& a_json,
                                       std::vector<MakeupEntry>& a_out,
                                       std::string& a_reason) {
            if (!a_json.isArray()) {
                a_reason = "not an array";
                return false;
            }
            std::set<std::uint32_t> seen;
            for (const auto& item : a_json) {
                if (!item.isObject()) continue;
                const auto& index = item["index"];
                if (!index.isIntegral() || index.asInt64() < 0) continue;
                MakeupEntry entry;
                entry.index = static_cast<std::uint32_t>(index.asInt64());
                if (!seen.insert(entry.index).second) continue;
                if (!MakeupTypeFromJson(item["type"], entry.type)) continue;
                // Tint and strength are ONE write on a tint mask (the colour
                // carries the strength in its top byte), so an entry missing
                // either half is dropped whole rather than half-applied.
                if (!RgbFromJson(item["tint"], entry.state.tint)) continue;
                float strength = 0.0f;
                if (!FiniteNumber(item["strength"], strength)) continue;
                entry.state.strength = OverlayPlan::ClampAlpha(strength);
                if (item["texture"].isString()) {
                    entry.state.hasTexture = true;
                    entry.state.texture    = item["texture"].asString();
                }
                a_out.push_back(std::move(entry));
            }
            if (a_out.empty()) {
                a_reason = "no usable layer entries";
                return false;
            }
            return true;
        }

        // ---- body / shape / skin / weight ---------------------------------

        [[nodiscard]] bool ParseBody(const Json::Value& a_json, BodyBlock& a_out,
                                     std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            a_out.obodyPreset = StringOr(a_json, "obodyPreset");
            if (a_json.isMember("custom")) {
                BodyPreset custom;
                std::string error;
                if (!BodyPresetJson::FromJson(a_json["custom"], custom, error)) {
                    a_reason = "embedded preset: " + error;
                    return false;
                }
                a_out.custom = std::move(custom);
            }
            if (a_out.obodyPreset.empty() && !a_out.custom) {
                a_reason = "neither \"obodyPreset\" nor \"custom\" present";
                return false;
            }
            return true;
        }

        void ParseValueMap(const Json::Value& a_json,
                           std::map<std::string, float>& a_out) {
            if (!a_json.isObject()) return;
            for (const auto& name : a_json.getMemberNames()) {
                float v = 0.0f;
                if (name.empty() || !FiniteNumber(a_json[name], v)) continue;
                a_out.emplace(name, v);
            }
        }

        [[nodiscard]] bool ParseShape(const Json::Value& a_json, ShapeBlock& a_out,
                                      std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            ParseValueMap(a_json["morphs"], a_out.morphs);
            ParseValueMap(a_json["scales"], a_out.scales);
            if (a_out.morphs.empty() && a_out.scales.empty()) {
                a_reason = "no usable morph or scale entries";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ParseSkin(const Json::Value& a_json, SkinBlock& a_out,
                                     std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            // ⚠ AN EMPTY STRING IS A VALUE ("no pack, take one off"); only a
            // MISSING or non-string member is junk. The old required-non-empty
            // rule is why a default skin could not be captured (user
            // 2026-08-22, "capture the skin even if it's default").
            if (!a_json.isMember("pack") || !a_json["pack"].isString()) {
                a_reason = "\"pack\" is required";
                return false;
            }
            a_out.pack = a_json["pack"].asString();
            return true;
        }

        [[nodiscard]] bool ParseCharacter(const Json::Value& a_json,
                                          CharacterBlock& a_out,
                                          std::string& a_reason) {
            if (!a_json.isObject()) {
                a_reason = "not an object";
                return false;
            }
            a_out.race.modName = StringOr(a_json, "mod");
            const auto id      = StringOr(a_json, "id");
            if (a_out.race.modName.empty() || id.empty()) {
                a_reason = "\"mod\" and \"id\" are required";
                return false;
            }
            a_out.race.localFormID = static_cast<std::uint32_t>(
                std::strtoul(id.c_str(), nullptr, 16));
            if (a_out.race.localFormID == 0) {
                a_reason = "\"id\" is not a form id";
                return false;
            }
            const auto& female = a_json["female"];
            if (!female.isBool()) {
                a_reason = "\"female\" is required";
                return false;
            }
            a_out.female = female.asBool();
            return true;
        }

        // ---- the exclusivity rule -----------------------------------------

        void DropForFlags(ProfileParse& a_parse) {
            auto& profile = a_parse.profile;
            if (!profile.face || profile.face->flags == 0) return;
            const auto flags = profile.face->flags;

            const auto drop = [&](const char* a_block, int a_bit,
                                  const char* a_channel) {
                a_parse.dropped.push_back(
                    std::string("block '") + a_block + "' dropped: face flags bit " +
                    std::to_string(a_bit) + " (" + a_channel +
                    ") makes RaceMenu the painter of that channel");
            };

            if ((flags & 1) && profile.overlays) {  // kPresetApplyOverrides
                profile.overlays.reset();
                drop("overlays", 1, "overrides");
            }
            if (flags & 2) {  // kPresetApplyBodyMorphs
                if (profile.body) {
                    profile.body.reset();
                    drop("body", 2, "body morphs");
                }
                if (profile.shape && !profile.shape->morphs.empty()) {
                    profile.shape->morphs.clear();
                    drop("shape morphs", 2, "body morphs");
                }
            }
            if ((flags & 4) && profile.shape && !profile.shape->scales.empty()) {
                profile.shape->scales.clear();  // kPresetApplyTransforms
                drop("shape scales", 4, "transforms");
            }
            if (profile.shape && profile.shape->morphs.empty() &&
                profile.shape->scales.empty()) {
                profile.shape.reset();
            }
            if ((flags & 8) && profile.skin) {  // kPresetApplySkinOverrides
                profile.skin.reset();
                drop("skin", 8, "skin");
            }
        }

    }  // namespace

    bool ParseProfile(const Json::Value& a_root, ProfileParse& a_out,
                      std::string& a_error) {
        try {
            if (!a_root.isObject()) {
                a_error = "root is not a JSON object";
                return false;
            }
            const int version = ReadSchemaVersion(a_root);
            if (version != kProfileVersion) {
                a_error = "version " + std::to_string(version) +
                          " (this build reads " + std::to_string(kProfileVersion) +
                          " - a newer Fitting Room may)";
                return false;
            }
            auto& profile = a_out.profile;
            profile.name  = StringOr(a_root, "name");
            if (profile.name.empty()) {
                a_error = "\"name\" is required";
                return false;
            }
            profile.author      = StringOr(a_root, "author");
            profile.description = StringOr(a_root, "description");

            // Same hard gate the presets have: requires is the author's own
            // opt-in refusal, so a malformed one refuses the file rather than
            // silently waving the gate open.
            profile.requires_.clear();
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
                    profile.requires_.push_back(r.asString());
                }
            }

            std::size_t present = 0;
            std::size_t parsed  = 0;
            const auto  block   = [&](const char* a_name, auto a_parse) {
                if (!a_root.isMember(a_name)) return;
                ++present;
                std::string reason;
                if (a_parse(a_root[a_name], reason)) {
                    ++parsed;
                } else {
                    a_out.dropped.push_back(std::string("block '") + a_name +
                                            "' dropped: " + reason);
                }
            };

            block("face", [&](const Json::Value& a_json, std::string& a_reason) {
                FaceBlock face;
                if (!ParseFace(a_json, face, a_reason)) return false;
                profile.face = std::move(face);
                return true;
            });
            block("outfit", [&](const Json::Value& a_json, std::string& a_reason) {
                Outfit outfit;
                if (!JsonCodec::JsonToOutfit(a_json, outfit)) {
                    a_reason = "not an object";
                    return false;
                }
                if (ChangedSlotCount(Outfit{}, outfit) == 0) {
                    a_reason = "no usable \"slots\" or \"weapons\" entries";
                    return false;
                }
                profile.outfit = std::move(outfit);
                return true;
            });
            block("overlays", [&](const Json::Value& a_json, std::string& a_reason) {
                OverlaysBlock overlays;
                if (!ParseOverlays(a_json, overlays, a_reason)) return false;
                profile.overlays = std::move(overlays);
                return true;
            });
            block("makeup", [&](const Json::Value& a_json, std::string& a_reason) {
                std::vector<MakeupEntry> makeup;
                if (!ParseMakeup(a_json, makeup, a_reason)) return false;
                profile.makeup = std::move(makeup);
                return true;
            });
            block("body", [&](const Json::Value& a_json, std::string& a_reason) {
                BodyBlock body;
                if (!ParseBody(a_json, body, a_reason)) return false;
                profile.body = std::move(body);
                return true;
            });
            block("shape", [&](const Json::Value& a_json, std::string& a_reason) {
                ShapeBlock shape;
                if (!ParseShape(a_json, shape, a_reason)) return false;
                profile.shape = std::move(shape);
                return true;
            });
            block("skin", [&](const Json::Value& a_json, std::string& a_reason) {
                SkinBlock skin;
                if (!ParseSkin(a_json, skin, a_reason)) return false;
                profile.skin = std::move(skin);
                return true;
            });
            block("weight", [&](const Json::Value& a_json, std::string& a_reason) {
                float weight = 0.0f;
                if (!FiniteNumber(a_json, weight)) {
                    a_reason = "not a finite number";
                    return false;
                }
                profile.weight = std::clamp(weight, 0.0f, 100.0f);
                return true;
            });
            block("character", [&](const Json::Value& a_json, std::string& a_reason) {
                CharacterBlock character;
                if (!ParseCharacter(a_json, character, a_reason)) return false;
                profile.character = std::move(character);
                return true;
            });

            DropForFlags(a_out);

            for (const auto& name : a_root.getMemberNames()) {
                if (!KnownMember(name)) {
                    profile.extras[name] = a_root[name];
                }
            }

            a_out.rewritable = present == 0 || parsed > 0;
            return true;
        } catch (const Json::Exception&) {
            a_error = "invalid JSON member type";
            return false;
        }
    }

    Json::Value ProfileToJson(const Profile& a_profile) {
        Json::Value root(Json::objectValue);
        root["version"] = kProfileVersion;
        root["name"]    = a_profile.name;
        if (!a_profile.author.empty()) root["author"] = a_profile.author;
        if (!a_profile.description.empty()) {
            root["description"] = a_profile.description;
        }
        if (!a_profile.requires_.empty()) {
            Json::Value req(Json::arrayValue);
            for (const auto& r : a_profile.requires_) req.append(r);
            root["requires"] = std::move(req);
        }

        if (a_profile.face) {
            Json::Value face(Json::objectValue);
            face["jslot"] = a_profile.face->jslot;
            // Always explicit: this field decides a deletion, so no reader
            // should ever have to know the default.
            face["source"] = a_profile.face->source == FaceSource::kCaptured
                                 ? "captured"
                                 : "referenced";
            face["flags"] = a_profile.face->flags;
            // Written only for Presets, so every pre-folder profile file
            // resaves byte-identical; absent has always meant Exported.
            if (a_profile.face->folder == FaceFolder::kPresets) {
                face["folder"] = "presets";
            }
            // Same rule, same reason: absent stays absent, so a profile
            // captured before this field resaves unchanged.
            if (a_profile.face->hairColour && a_profile.face->hairColour->set) {
                const auto& t = *a_profile.face->hairColour;
                face["hairColor"] =
                    static_cast<Json::UInt>((static_cast<std::uint32_t>(t.r) << 16) |
                                            (static_cast<std::uint32_t>(t.g) << 8) |
                                            static_cast<std::uint32_t>(t.b));
            }
            root["face"]  = std::move(face);
        }

        if (a_profile.outfit) {
            auto outfit = JsonCodec::OutfitToJson(*a_profile.outfit);
            outfit.removeMember("favorite");  // meaningless inside a profile
            root["outfit"] = std::move(outfit);
        }

        if (a_profile.overlays) {
            Json::Value overlays(Json::objectValue);
            bool any = false;
            for (const auto& info : OverlayPlan::kLocations) {
                const auto& entries =
                    a_profile.overlays->byLocation[OverlayPlan::Slot(info.location)];
                if (entries.empty()) continue;
                Json::Value list(Json::arrayValue);
                for (const auto& entry : entries) {
                    Json::Value e(Json::objectValue);
                    e["index"] = entry.index;
                    const auto& state = entry.state;
                    if (state.hasTexture) e["texture"] = state.texture;
                    if (state.hasNormal) e["normal"] = state.normal;
                    if (state.hasTint) e["tint"] = RgbToHex(state.tint);
                    if (state.hasAlpha) e["alpha"] = state.alpha;
                    if (state.glow != OverlayPlan::Rgb{ 0, 0, 0 } ||
                        state.glowStrength != 0.0f) {
                        e["glow"]         = RgbToHex(state.glow);
                        e["glowStrength"] = state.glowStrength;
                    }
                    if (state.hasFinish) {
                        Json::Value finish(Json::objectValue);
                        finish["gloss"]    = state.gloss;
                        finish["specular"] = state.specular;
                        e["finish"]        = std::move(finish);
                    }
                    const auto t = OverlayTransform::Quantise(state.transform);
                    if (!OverlayTransform::IsIdentity(t)) {
                        Json::Value transform(Json::objectValue);
                        transform["offsetX"]     = t.offsetX;
                        transform["offsetY"]     = t.offsetY;
                        transform["scale"]       = t.scale;
                        transform["rotationDeg"] = t.rotationDeg;
                        e["transform"]           = std::move(transform);
                    }
                    list.append(std::move(e));
                }
                overlays[info.id] = std::move(list);
                any               = true;
            }
            if (any) root["overlays"] = std::move(overlays);
        }

        if (a_profile.makeup && !a_profile.makeup->empty()) {
            Json::Value makeup(Json::arrayValue);
            for (const auto& entry : *a_profile.makeup) {
                Json::Value e(Json::objectValue);
                e["index"] = entry.index;
                if (MakeupPlan::KnownType(entry.type)) {
                    e["type"] = MakeupPlan::IdFor(entry.type);
                } else {
                    // An unknown type round-trips as its number; "other" would
                    // flatten every unknown into one and lose the original.
                    e["type"] = entry.type;
                }
                e["tint"]     = RgbToHex(entry.state.tint);
                e["strength"] = entry.state.strength;
                if (entry.state.hasTexture) e["texture"] = entry.state.texture;
                makeup.append(std::move(e));
            }
            root["makeup"] = std::move(makeup);
        }

        if (a_profile.body) {
            Json::Value body(Json::objectValue);
            if (!a_profile.body->obodyPreset.empty()) {
                body["obodyPreset"] = a_profile.body->obodyPreset;
            }
            if (a_profile.body->custom) {
                body["custom"] = BodyPresetJson::ToJson(*a_profile.body->custom);
            }
            if (!body.empty()) root["body"] = std::move(body);
        }

        if (a_profile.shape &&
            (!a_profile.shape->morphs.empty() || !a_profile.shape->scales.empty())) {
            Json::Value shape(Json::objectValue);
            if (!a_profile.shape->morphs.empty()) {
                Json::Value morphs(Json::objectValue);
                for (const auto& [name, value] : a_profile.shape->morphs) {
                    morphs[name] = value;
                }
                shape["morphs"] = std::move(morphs);
            }
            if (!a_profile.shape->scales.empty()) {
                Json::Value scales(Json::objectValue);
                for (const auto& [name, value] : a_profile.shape->scales) {
                    scales[name] = value;
                }
                shape["scales"] = std::move(scales);
            }
            root["shape"] = std::move(shape);
        }

        if (a_profile.skin) {
            // Written even for an empty pack: empty is "wears no pack" and
            // applying it takes one off, so the block has to survive the
            // round-trip (ParseSkin's rule).
            Json::Value skin(Json::objectValue);
            skin["pack"] = a_profile.skin->pack;
            root["skin"] = std::move(skin);
        }

        if (a_profile.weight) {
            root["weight"] = std::clamp(*a_profile.weight, 0.0f, 100.0f);
        }

        if (a_profile.character) {
            Json::Value character(Json::objectValue);
            character["mod"] = a_profile.character->race.modName;
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%06X",
                          a_profile.character->race.localFormID);
            character["id"]     = hex;
            character["female"] = a_profile.character->female;
            root["character"]   = std::move(character);
        }

        for (const auto& name : a_profile.extras.getMemberNames()) {
            if (!KnownMember(name)) {
                root[name] = a_profile.extras[name];
            }
        }
        return root;
    }

}  // namespace OS::ProfileCodec
