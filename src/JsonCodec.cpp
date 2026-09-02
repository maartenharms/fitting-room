#include "JsonCodec.h"

#include "DyeBlend.h"          // ChoiceFromByte, so an unreadable eye blend defers
#include "HeadPartSlotPlan.h"  // kFirstCustomType, refusing an engine-named type
#include "SlotMask.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace OS::JsonCodec {

    namespace {
        // Named, not numeric - "kind" on a slot entry set this precedent
        // ("hide"/"style"/"passthrough") because this file backs a
        // hand-editable authoring surface (see PRESETS.md), unlike orefit's
        // numeric encoding which no preset author is expected to hand-write.
        const char* HairJsonName(HairMode a_mode) {
            switch (a_mode) {
                case HairMode::kShow: return "show";
                case HairMode::kHide: return "hide";
                default:              return "auto";
            }
        }

        HairMode HairFromJsonName(const std::string& a_name) {
            if (a_name == "show") return HairMode::kShow;
            if (a_name == "hide") return HairMode::kHide;
            return HairMode::kAuto;  // includes "auto" and anything unrecognised
        }

        // Hex RRGGBB, because that is what a human editing an exported outfit by
        // hand expects to see and can paste from any colour picker on earth.
        std::string HairTintJsonName(const HairTint& a_tint) {
            char buf[7]{};
            std::snprintf(buf, sizeof(buf), "%02X%02X%02X", a_tint.r, a_tint.g, a_tint.b);
            return buf;
        }

        // Hair's view of the shared colour rule. Delegates so there is exactly
        // one parser: see ColourFromHex below for why that matters.
        HairTint HairTintFromJsonName(const std::string& a_hex) {
            const auto c = ColourFromHex(a_hex);
            return HairTint{ c.set, c.r, c.g, c.b };
        }

        // A schema version read that cannot throw: a hand-edited "version": "2"
        // (string) is junk to reject, not a Json::LogicError to crash on. Bool
        // and non-integral reals are junk too; isIntegral() excludes both. 0 is
        // a safe "reject" sentinel for both callers - kPresetVersion is 1 and
        // the unsupported-hair schema is 2, neither is ever 0.
        int ReadSchemaVersion(const Json::Value& a_root) {
            const auto& v = a_root["version"];
            return v.isIntegral() ? v.asInt() : 0;
        }
    }  // namespace

    std::string ColourToHex(const DyeChannel& a_colour) {
        char rgb[7]{};
        std::snprintf(rgb, sizeof(rgb), "%02X%02X%02X", a_colour.r, a_colour.g,
                      a_colour.b);
        return rgb;
    }

    DyeChannel ColourFromHex(const std::string& a_hex) {
        if (a_hex.size() != 6) {
            return DyeChannel{};
        }
        unsigned int v = 0;
        for (char c : a_hex) {
            unsigned int d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<unsigned int>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<unsigned int>(c - 'A') + 10u;
            } else {
                return DyeChannel{};
            }
            v = (v << 4) | d;
        }
        return DyeChannel{ true, static_cast<std::uint8_t>((v >> 16) & 0xFF),
                           static_cast<std::uint8_t>((v >> 8) & 0xFF),
                           static_cast<std::uint8_t>(v & 0xFF) };
    }

    namespace {

        // Whether a channel says anything a bare RRGGBB string cannot carry.
        // The field list IS the wire format below: a member added to
        // DyeChannel that matters to the paint has to appear in all three of
        // this test, ChannelToJson and ChannelFromJson, or the pearl bug this
        // trio fixes comes straight back for the new field.
        [[nodiscard]] bool ChannelIsPlain(const DyeChannel& a_ch) {
            return a_ch.strength == 255 && a_ch.mode == 0 && !a_ch.secondSet &&
                   a_ch.flake == 0 && a_ch.blend == 0 && !a_ch.palette.Any() &&
                   !a_ch.player.Any();
        }

        [[nodiscard]] std::string BytesToHex(std::uint8_t a_r, std::uint8_t a_g,
                                             std::uint8_t a_b) {
            char rgb[7]{};
            std::snprintf(rgb, sizeof(rgb), "%02X%02X%02X", a_r, a_g, a_b);
            return rgb;
        }

        // One dyed channel as JSON. A plain colour stays the bare hex string
        // it always was, so every outfit dyed before modes existed encodes
        // byte-identically; anything the string cannot say - the ramp and
        // second stop that make a pearl a pearl, the flake, the blend, the
        // finish blocks - rides an object instead. That distinction is the
        // whole fix for the field report "pearlescent dyes always come back
        // single colour": this encoder used to write the hex alone, so the
        // profile's embedded outfit re-imported as a flat dye.
        [[nodiscard]] Json::Value ChannelToJson(const DyeChannel& a_ch) {
            if (ChannelIsPlain(a_ch)) {
                return Json::Value(ColourToHex(a_ch));
            }
            Json::Value c(Json::objectValue);
            c["hex"] = ColourToHex(a_ch);
            if (a_ch.strength != 255) c["strength"] = a_ch.strength;
            if (a_ch.mode != 0) c["mode"] = a_ch.mode;
            if (a_ch.secondSet) c["second"] = BytesToHex(a_ch.r2, a_ch.g2, a_ch.b2);
            if (a_ch.flake != 0) c["flake"] = a_ch.flake;
            if (a_ch.blend != 0) c["blend"] = a_ch.blend;
            if (a_ch.palette.sheenSet) {
                c["sheen"] = BytesToHex(a_ch.palette.sheenR, a_ch.palette.sheenG,
                                        a_ch.palette.sheenB);
            }
            if (a_ch.palette.glossSet) c["gloss"] = a_ch.palette.gloss;
            if (a_ch.player.sheenSet) {
                c["playerSheen"] = BytesToHex(a_ch.player.sheenR, a_ch.player.sheenG,
                                              a_ch.player.sheenB);
            }
            if (a_ch.player.glossSet) c["playerGloss"] = a_ch.player.gloss;
            return c;
        }

        [[nodiscard]] std::uint8_t ByteOr(const Json::Value& a_json,
                                          const char* a_key, std::uint8_t a_def) {
            const auto& v = a_json[a_key];
            return v.isIntegral()
                       ? static_cast<std::uint8_t>(v.asUInt() & 0xFF)
                       : a_def;
        }

        // The read half, string or object; junk decodes to "channel off",
        // JsonToOutfit's own tolerance.
        [[nodiscard]] DyeChannel ChannelFromJson(const Json::Value& a_json) {
            if (a_json.isString()) {
                return ColourFromHex(a_json.asString());
            }
            if (!a_json.isObject()) {
                return DyeChannel{};
            }
            auto ch = ColourFromHex(a_json.get("hex", "").asString());
            if (!ch.set) {
                return DyeChannel{};
            }
            ch.strength = ByteOr(a_json, "strength", 255);
            ch.mode     = ByteOr(a_json, "mode", 0);
            if (const auto second =
                    ColourFromHex(a_json.get("second", "").asString());
                second.set) {
                ch.secondSet = true;
                ch.r2        = second.r;
                ch.g2        = second.g;
                ch.b2        = second.b;
            }
            ch.flake = ByteOr(a_json, "flake", 0);
            ch.blend = ByteOr(a_json, "blend", 0);
            if (const auto sheen =
                    ColourFromHex(a_json.get("sheen", "").asString());
                sheen.set) {
                ch.palette.sheenSet = true;
                ch.palette.sheenR   = sheen.r;
                ch.palette.sheenG   = sheen.g;
                ch.palette.sheenB   = sheen.b;
            }
            if (a_json.isMember("gloss") && a_json["gloss"].isIntegral()) {
                ch.palette.glossSet = true;
                ch.palette.gloss    = ByteOr(a_json, "gloss", 128);
            }
            if (const auto sheen =
                    ColourFromHex(a_json.get("playerSheen", "").asString());
                sheen.set) {
                ch.player.sheenSet = true;
                ch.player.sheenR   = sheen.r;
                ch.player.sheenG   = sheen.g;
                ch.player.sheenB   = sheen.b;
            }
            if (a_json.isMember("playerGloss") &&
                a_json["playerGloss"].isIntegral()) {
                ch.player.glossSet = true;
                ch.player.gloss    = ByteOr(a_json, "playerGloss", 128);
            }
            return ch;
        }

    }  // namespace

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
        // A custom preset owns the morph when both fields are present. Keep
        // the file canonical as well as enforcing that rule on decode.
        if (a_outfit.customBodyPresetId.empty() && !a_outfit.obodyPreset.empty()) {
            o["obodyPreset"] = a_outfit.obodyPreset;
        }
        if (!a_outfit.customBodyPresetId.empty()) {
            o["customBodyPresetId"] = a_outfit.customBodyPresetId;
        }
        if (a_outfit.orefit != ORefitMode::kDefault) {
            o["orefit"] = static_cast<unsigned int>(a_outfit.orefit);
        }
        if (a_outfit.pushUp != PushUpMode::kNone) {
            o["pushUp"] = static_cast<unsigned int>(a_outfit.pushUp);
        }

        // Separate dimension from the body block above (nothing to do with
        // OBody) - same "omit the default" rule so a pre-hair outfit file
        // round-trips byte-identical.
        if (a_outfit.hair != HairMode::kAuto) {
            o["hair"] = HairJsonName(a_outfit.hair);
        }

        // Omit the default, same rule as hair visibility and OBody above, so an
        // outfit file written before this feature stays unchanged.
        if (a_outfit.hairTint.set) {
            o["hairColor"] = HairTintJsonName(a_outfit.hairTint);
        }

        // The eye colours ride the same shape as hair: one RGB each, omitted
        // when unset, hex a human can paste from any colour picker.
        if (a_outfit.eyeTint.set) {
            o["eyeColor"] = HairTintJsonName(a_outfit.eyeTint);
        }
        if (a_outfit.scleraTint.set) {
            o["scleraColor"] = HairTintJsonName(a_outfit.scleraTint);
        }
        // ⚠ THE SECOND EYE COLOUR RIDES HERE FOR THE REASON THE OTHER TWO DO.
        // Each eye colour was added to this file the day it landed, so that an
        // EXPORTED outfit keeps it: the co-save carries them anyway, and this
        // is the path that crosses between saves and between players. A field
        // the co-save keeps and the export drops makes one outfit render two
        // ways depending on which door it came back through.
        if (a_outfit.eyeTint2.set) {
            o["eyeColor2"] = HairTintJsonName(a_outfit.eyeTint2);
        }
        // ⚠ AND THE EYE'S BLEND, WHICH IS THE ONE NON-COLOUR FIELD HERE. Every
        // other blend in the mod is a per-channel byte and no dye channel extra
        // rides this file at all (strength, mode, the stops, flake and the
        // finish blocks are all absent by the same rule), but the eye's blend
        // is not on a channel: it is an outfit field beside the tints above,
        // and dropping it exports an outfit whose eyes render through a
        // different curve than the one that was saved.
        if (a_outfit.eyeBlend != 0) {
            o["eyeBlend"] = static_cast<Json::UInt>(a_outfit.eyeBlend);
        }

        // Hair STYLE, same omit-the-default rule. Written as mod + id rather
        // than as a name: a head part's name is not unique across a load order
        // and does not survive a mod renaming its records, while the pair is
        // exactly what every other form reference in this file already uses.
        if (!a_outfit.hairStyle.Empty()) {
            Json::Value hs(Json::objectValue);
            hs["mod"] = a_outfit.hairStyle.modName;
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%06X", a_outfit.hairStyle.localFormID);
            hs["id"]       = hex;
            o["hairStyle"] = std::move(hs);
        }

        // Eye and brow type (OS-161), on the hair style's exact terms: mod plus
        // id, omitted entirely when empty, so an outfit that names neither
        // serializes byte-identically to a file written before they existed.
        const auto putHeadPart = [&o](const char* a_field, const StyleRefKey& a_key) {
            if (a_key.Empty()) {
                return;
            }
            Json::Value v(Json::objectValue);
            v["mod"] = a_key.modName;
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%06X", a_key.localFormID);
            v["id"]    = hex;
            o[a_field] = std::move(v);
        };
        putHeadPart("eyes", a_outfit.eyes);
        putHeadPart("brows", a_outfit.brows);
        putHeadPart("facialHair", a_outfit.facialHair);

        // Head-part slots this load order invented (horns, cat ears). Omitted
        // entirely when the outfit names none, which is the same omit-the-
        // default rule the dyes below follow, so an outfit made on a rig with
        // no such mods serializes byte-identically to a pre-feature file.
        //
        // The slot NUMBER is written into each entry rather than used as a key
        // name, because it is a number that belongs to whichever mod claimed
        // it and a hand-edited file should be able to say so plainly.
        if (!a_outfit.customHeadParts.empty()) {
            Json::Value slots(Json::arrayValue);
            for (const auto& part : a_outfit.customHeadParts) {
                Json::Value v(Json::objectValue);
                v["slot"]   = part.slot;
                v["mod"]    = part.key.modName;
                char slotHex[16];
                std::snprintf(slotHex, sizeof(slotHex), "0x%06X", part.key.localFormID);
                v["id"] = slotHex;
                slots.append(std::move(v));
            }
            o["customHeadParts"] = std::move(slots);
        }

        // Dye, same omit-the-default rule as everything above: only dyed slots
        // appear, and only their SET channels, so an undyed outfit serializes
        // byte-identically to a pre-dye file. Colours use the same bare
        // RRGGBB hex the hairColor field does, keyed by channel name, keyed
        // in turn by the same editor slot number the slots array speaks.
        Json::Value dyes(Json::arrayValue);
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            const auto& dye = a_outfit.DyeFor(bit);
            if (!dye.Any()) {
                continue;
            }
            Json::Value d(Json::objectValue);
            d["slot"] = bit + 30;
            // A "colours" ARRAY, position by position, rather than the three
            // named keys this used to write. Once a garment can have eight
            // pieces there is no name for the sixth, and the array is the same
            // shape a saved dye scheme already uses. An unset channel is null
            // rather than absent, because the mapping onto a garment is BY
            // ORDER: closing a gap would move every later colour onto the wrong
            // piece. Trailing unset channels are trimmed, so a one-colour slot
            // still writes one entry.
            std::size_t last = 0;
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (dye.channels[c].set) {
                    last = c + 1;
                }
            }
            Json::Value colours(Json::arrayValue);
            for (std::size_t c = 0; c < last; ++c) {
                const auto& ch = dye.channels[c];
                if (!ch.set) {
                    colours.append(Json::Value());
                    continue;
                }
                colours.append(ChannelToJson(ch));
            }
            d["colours"] = std::move(colours);
            dyes.append(std::move(d));
        }
        if (!dyes.empty()) {
            o["dyes"] = std::move(dyes);
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
        const auto& custom = a_json["customBodyPresetId"];
        a_out.customBodyPresetId = custom.isString() ? custom.asString() : std::string{};
        // Corrupt/hand-authored input cannot select two owners. A stable
        // custom ID wins because it is the more specific reference.
        if (!a_out.customBodyPresetId.empty()) {
            a_out.obodyPreset.clear();
        }
        const auto& refit = a_json["orefit"];
        a_out.orefit      = ORefitMode::kDefault;
        if (refit.isUInt()) {
            const auto value = refit.asUInt();
            if (value <= static_cast<unsigned int>(ORefitMode::kForceOff)) {
                a_out.orefit = static_cast<ORefitMode>(value);
            }
        }
        // Range checked on the way in like orefit above, so a byte from a build
        // with more levels than this one reads as off rather than as a mode the
        // recipe table has no arm for.
        const auto& push = a_json["pushUp"];
        a_out.pushUp     = PushUpMode::kNone;
        if (push.isUInt()) {
            const auto value = push.asUInt();
            if (value <= static_cast<unsigned int>(PushUpMode::kFull)) {
                a_out.pushUp = static_cast<PushUpMode>(value);
            }
        }
        const auto& hair = a_json["hair"];
        a_out.hair       = hair.isString() ? HairFromJsonName(hair.asString()) : HairMode::kAuto;
        const auto& hairColor = a_json["hairColor"];
        a_out.hairTint        = hairColor.isString()
                                   ? HairTintFromJsonName(hairColor.asString())
                                   : HairTint{};
        const auto& eyeColor  = a_json["eyeColor"];
        a_out.eyeTint         = eyeColor.isString()
                                   ? HairTintFromJsonName(eyeColor.asString())
                                   : HairTint{};
        const auto& scleraColor = a_json["scleraColor"];
        a_out.scleraTint        = scleraColor.isString()
                                    ? HairTintFromJsonName(scleraColor.asString())
                                    : HairTint{};
        const auto& eyeColor2   = a_json["eyeColor2"];
        a_out.eyeTint2          = eyeColor2.isString()
                                     ? HairTintFromJsonName(eyeColor2.asString())
                                     : HairTint{};
        // ⚠ AN UNREADABLE OR OUT-OF-RANGE BLEND DEFERS, on DyeBlend.h's own
        // rule: the byte's zero means "use this surface's default", which is
        // what every outfit written before the field existed decodes to.
        // ChoiceFromByte answers kDefault for anything it does not know, so a
        // hand-edited file costs the eye its curve rather than its colour.
        const auto& eyeBlend = a_json["eyeBlend"];
        a_out.eyeBlend       = eyeBlend.isIntegral()
                                   ? static_cast<std::uint8_t>(OS::DyeBlend::ChoiceFromByte(
                                         static_cast<std::uint8_t>(
                                             std::clamp(eyeBlend.asInt(), 0, 255))))
                                   : 0u;
        // Absent, malformed or half-filled all mean "no hair style". A partial
        // reference cannot resolve to a head part, so accepting one would turn
        // an obvious omission here into a silent no-op much later.
        const auto& hairStyle = a_json["hairStyle"];
        a_out.hairStyle       = StyleRefKey{};
        if (hairStyle.isObject()) {
            StyleRefKey key;
            key.modName     = hairStyle.get("mod", "").asString();
            key.localFormID = static_cast<std::uint32_t>(std::strtoul(
                hairStyle.get("id", "0").asString().c_str(), nullptr, 16));
            if (!key.modName.empty() && key.localFormID != 0) {
                a_out.hairStyle = std::move(key);
            }
        }
        // Eyes and brows, with the hair style's strictness for the same reason:
        // a half-filled reference cannot resolve to a head part, so accepting
        // one would turn an obvious omission in the file into a silent no-op
        // much later, on someone else's character.
        const auto takeHeadPart = [&a_json](const char* a_field) {
            StyleRefKey  out{};
            const auto&  v = a_json[a_field];
            if (!v.isObject()) {
                return out;
            }
            StyleRefKey key;
            key.modName     = v.get("mod", "").asString();
            key.localFormID = static_cast<std::uint32_t>(
                std::strtoul(v.get("id", "0").asString().c_str(), nullptr, 16));
            if (!key.modName.empty() && key.localFormID != 0) {
                out = std::move(key);
            }
            return out;
        };
        a_out.eyes       = takeHeadPart("eyes");
        a_out.brows      = takeHeadPart("brows");
        a_out.facialHair = takeHeadPart("facialHair");
        // Invented head-part slots. Reset then fill, like every field above,
        // and each entry is taken only when it names both a slot and a part:
        // an imported file is untrusted, and a half-read entry would put a
        // stranger's horns on the player.
        a_out.customHeadParts.clear();
        if (const auto& slots = a_json["customHeadParts"]; slots.isArray()) {
            for (const auto& entry : slots) {
                if (!entry.isObject() || !entry.isMember("slot")) {
                    continue;
                }
                const auto slot = entry["slot"].asUInt();
                if (slot < HeadPartSlotPlan::kFirstCustomType) {
                    continue;  // a type the engine names is not one of these
                }
                StyleRefKey key;
                key.modName = entry.get("mod", "").asString();
                const auto idText = entry.get("id", "").asString();
                if (key.modName.empty() || idText.empty()) {
                    continue;
                }
                key.localFormID = static_cast<std::uint32_t>(
                    std::strtoul(idText.c_str(), nullptr, 0));
                if (key.localFormID == 0) {
                    continue;
                }
                // Through the setter, so the one-entry-per-slot rule holds for
                // an imported file exactly as it does for one the editor wrote.
                a_out.SetCustomHeadPart(slot, std::move(key));
            }
        }
        // ⚠⚠ THE SLOTS ARE DECODED BEFORE THE DYES, AND THE ORDER IS LOAD
        // BEARING. An armour dye is keyed by (slot, garment) since the restyle
        // fix, so SetDye below resolves the bit through whatever garment this
        // loop has just put there. Run the dye loop first and every colour keys
        // to an EMPTY garment, the styles then arrive, and the outfit reads as
        // undyed: that is exactly what happened, and ProfileCodecTests caught
        // it. The binary codec has the same dependency and states it at the
        // armour dye block; this is the same rule spelled in JSON.
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
        // Dye. Reset-then-fill like every field above, and the same strictness
        // as hairColor: a channel is either six hex digits or it decodes to
        // "leave this alone" - an imported file is untrusted, and a half-parsed
        // colour would land on the user's gear.
        for (std::uint32_t bit = 0; bit < Outfit::kBitCount; ++bit) {
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                a_out.SetDye(bit, static_cast<DyeChannelId>(c), DyeChannel{});
            }
        }
        for (const auto& d : a_json["dyes"]) {
            if (!d.isObject()) {
                continue;
            }
            const auto dyeSlot = d.get("slot", 0).asUInt();
            if (dyeSlot < 30 || dyeSlot > 61) {
                continue;
            }
            const auto dyeBit = BitForEditorSlot(dyeSlot);
            // The "colours" array is what this writes now. The three named keys
            // are still READ, because presets and exports written before the
            // widening use them and there is no reason to strand a file that
            // parses perfectly well. A file carrying both is taken at its array,
            // which is the one this version would have produced.
            const auto& colours = d["colours"];
            if (colours.isArray()) {
                const auto n = std::min<std::size_t>(colours.size(), kDyeChannelCount);
                for (std::size_t c = 0; c < n; ++c) {
                    // A string is the plain colour it always was; an object is
                    // the rich form ChannelToJson writes for a pearl. A null
                    // gap or junk decodes to "channel off" either way.
                    const auto col = ChannelFromJson(
                        colours[static_cast<Json::ArrayIndex>(c)]);
                    if (col.set) {
                        a_out.SetDye(dyeBit, static_cast<DyeChannelId>(c), col);
                    }
                }
                continue;
            }
            static constexpr const char* kLegacyChannelNames[3] = {
                "primary", "secondary", "accent"
            };
            for (std::size_t c = 0; c < 3; ++c) {
                const auto& ch = d[kLegacyChannelNames[c]];
                if (!ch.isString()) {
                    continue;
                }
                const auto col = ColourFromHex(ch.asString());
                if (col.set) {
                    a_out.SetDye(dyeBit, static_cast<DyeChannelId>(c), col);
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
        const int version = ReadSchemaVersion(a_root);
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

    Json::Value UnsupportedHairToJson(const std::vector<StyleRefKey>& a_keys) {
        Json::Value root(Json::objectValue);
        root["version"] = 3;
        Json::Value entries(Json::arrayValue);
        for (const auto& k : a_keys) {
            Json::Value e(Json::objectValue);
            e["mod"] = k.modName;
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%06X", k.localFormID);
            e["id"] = hex;
            entries.append(std::move(e));
        }
        root["entries"] = std::move(entries);
        return root;
    }

    bool JsonToUnsupportedHair(const Json::Value& a_json,
                               std::vector<StyleRefKey>& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        // v3 gate (OS-248). Any other version is outdated rather than corrupt:
        // reject wholesale so every entry re-measures once under the current
        // admission rules. v1 predates the FSMP path, so its SMP declines are
        // all wrong now.
        //
        // ⚠⚠ THE BUMP IS THE POINT, NOT BOOKKEEPING. A verdict is only as good
        // as the rule that produced it, and v2 was written by a partition gate
        // that refused any single-partition mesh whose bone map was shorter
        // than its bone list. That rule was measured wrong on 2026-08-21 and
        // removed, so every decline it persisted is a mesh that may well be
        // fine. Keeping the file would leave those styles greyed out for good
        // with no way back short of deleting it by hand, which is exactly the
        // failure the session-scoped declines exist to avoid.
        if (ReadSchemaVersion(a_json) != 3) {
            return false;
        }
        const auto& entries = a_json["entries"];
        if (!entries.isArray()) {
            return true;  // an empty file is an empty list, not an error
        }
        for (const auto& e : entries) {
            if (!e.isObject()) {
                continue;
            }
            StyleRefKey k;
            k.modName     = e.get("mod", "").asString();
            k.localFormID = static_cast<std::uint32_t>(
                std::strtoul(e.get("id", "0").asString().c_str(), nullptr, 16));
            // A local id of 0 is no head part, and covers unparseable ids too
            // (strtoul answers 0 for junk). Same skip-not-fail tolerance as
            // every other reader here.
            if (k.modName.empty() || k.localFormID == 0) {
                continue;
            }
            a_out.push_back(std::move(k));
        }
        return true;
    }

}  // namespace OS::JsonCodec
