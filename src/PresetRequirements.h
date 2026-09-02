#pragma once

#include "ProfileCodec.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What a preset needs installed before it can apply properly.
//
// Two formats answer the same question and the answer has ONE shape, because
// the outfit presets page already computes and draws exactly this: a list of
// plugin names, checked against TESDataHandler::LookupModByName, with the
// absent ones called out. `exportHealth` in EditorUI is that model. Nothing
// here invents a second one; it only decides WHICH names to check.
//
// ⚠⚠ READING A JSLOT TO LIST WHAT IT NAMES IS NOT APPLYING ONE, and the
// distinction is the whole reason this is allowed to exist. Applying a RaceMenu
// preset stays where it has always been, on Papyrus CharGen, and never touches
// `IPresetInterface`, which skee declares and never registers. `ProfilesUI`
// used to say a jslot is never opened here; now it is opened to be READ, and
// the apply path is untouched.
//
// ⚠ FAIL SOFT, ALWAYS. A jslot is somebody else's format and RaceMenu may
// change it, so anything that is not the shape this knows returns nothing WITH
// a reason rather than guessing. A wrong requirements list is worse than none:
// a player who is told a plugin is missing will go and install something they
// did not need, and one who is told nothing is missing will not.
namespace OS::PresetRequirements {

    namespace detail {

        // Case-insensitive, and hand-rolled rather than `_stricmp` so this
        // header stays free of platform includes and the tests can drive it
        // anywhere.
        [[nodiscard]] inline int CmpNoCase(std::string_view a_lhs, std::string_view a_rhs) {
            const auto n = std::min(a_lhs.size(), a_rhs.size());
            for (std::size_t i = 0; i < n; ++i) {
                const auto l = std::tolower(static_cast<unsigned char>(a_lhs[i]));
                const auto r = std::tolower(static_cast<unsigned char>(a_rhs[i]));
                if (l != r) {
                    return l < r ? -1 : 1;
                }
            }
            if (a_lhs.size() == a_rhs.size()) {
                return 0;
            }
            return a_lhs.size() < a_rhs.size() ? -1 : 1;
        }

        // ⚠ A PLUGIN NAME IS NOT CASE SENSITIVE and the two formats disagree
        // about capitalisation constantly: a jslot writes whatever the author's
        // load order held, and an outfit's StyleRefKey writes whatever the
        // record said. Deduping case-sensitively would list one plugin twice.
        inline void Add(std::vector<std::string>& a_out, std::string_view a_name) {
            if (a_name.empty()) {
                return;
            }
            for (const auto& have : a_out) {
                if (CmpNoCase(have, a_name) == 0) {
                    return;
                }
            }
            a_out.emplace_back(a_name);
        }

        inline void Sort(std::vector<std::string>& a_out) {
            std::sort(a_out.begin(), a_out.end(),
                      [](const std::string& a_lhs, const std::string& a_rhs) {
                          return CmpNoCase(a_lhs, a_rhs) < 0;
                      });
        }

        // "KS Hairdo's.esp|01D8F9" names a plugin. Empty for anything else,
        // including a bare form id with no plugin in front of it.
        [[nodiscard]] inline std::string PluginOf(std::string_view a_identifier) {
            const auto bar = a_identifier.find('|');
            if (bar == std::string_view::npos || bar == 0) {
                return {};
            }
            return std::string{ a_identifier.substr(0, bar) };
        }

    }  // namespace detail

    // Every plugin a RaceMenu preset names, sorted and deduped.
    //
    // ⚠⚠ GATED ON THE SHAPE, NOT ON `formatVersion`. Every one of the 537
    // jslots on the reference rig is formatVersion 3 and every one of the 400
    // sampled carries `modNames`, but refusing a future version outright would
    // break this the day RaceMenu bumps the number for an unrelated reason. A
    // newer file that still writes the members below is still readable; one
    // that renames them falls through to the refusal and says so.
    //
    // ⚠ `modNames` IS AUTHORITATIVE AND THE REST IS BELT AND BRACES. On every
    // file measured it is a superset of what the head parts name, but a preset
    // that references a form it forgot to declare would otherwise be reported
    // as applying cleanly when it cannot.
    [[nodiscard]] inline std::vector<std::string> PluginsFromJslot(const Json::Value& a_root,
                                                                   std::string& a_why) {
        a_why.clear();
        if (!a_root.isObject()) {
            a_why = "the file is not a JSON object";
            return {};
        }

        std::vector<std::string> out;

        if (const auto& names = a_root["modNames"]; names.isArray()) {
            for (const auto& entry : names) {
                if (entry.isString()) {
                    detail::Add(out, entry.asString());
                }
            }
        }

        if (const auto& parts = a_root["headParts"]; parts.isArray()) {
            for (const auto& part : parts) {
                if (!part.isObject()) {
                    continue;
                }
                if (const auto& id = part["formIdentifier"]; id.isString()) {
                    detail::Add(out, detail::PluginOf(id.asString()));
                }
            }
        }

        if (const auto& actor = a_root["actor"]; actor.isObject()) {
            if (const auto& tex = actor["headTexture"]; tex.isString()) {
                detail::Add(out, detail::PluginOf(tex.asString()));
            }
        }

        if (out.empty()) {
            a_why = "no modNames array and no head part naming a plugin";
            return {};
        }
        detail::Sort(out);
        return out;
    }

    // Every plugin a Looks preset names, sorted and deduped.
    //
    // ⚠ EXACT, AND IT CANNOT ROT, which is what makes this half the cheap one.
    // Fitting Room owns the profile format outright and every `StyleRefKey` in
    // it already carries `modName`, so this is a walk over what the codec has
    // already decoded rather than a parse of somebody else's file.
    //
    // ⚠⚠ THE WEAPON AND HEAD-PART DIMENSIONS ARE IN, not just the armour slots
    // `exportHealth` walks. A look carries head-part dyes keyed by the part
    // they colour, and a part whose mod is gone is precisely the absence this
    // list exists to name.
    //
    // ⛔ THE FACE BLOCK IS NOT WALKED HERE and that is deliberate: it names a
    // jslot, not a plugin, so its requirements are that file's and are read
    // with PluginsFromJslot by a caller that can reach the disk. Guessing them
    // from the file name would be exactly the wrong list this refuses to make.
    [[nodiscard]] inline std::vector<std::string> PluginsFromProfile(
        const ProfileCodec::Profile& a_profile) {
        std::vector<std::string> out;

        for (const auto& required : a_profile.requires_) {
            detail::Add(out, required);
        }

        if (a_profile.outfit) {
            a_profile.outfit->ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
                detail::Add(out, a_key.modName);
            });
            a_profile.outfit->ForEachWeaponStyle([&](WeaponClass, const StyleRefKey& a_key) {
                detail::Add(out, a_key.modName);
            });
            a_profile.outfit->ForEachHeadPartDye(
                [&](std::uint32_t, const StyleRefKey& a_part, const SlotDye&) {
                    detail::Add(out, a_part.modName);
                });
        }

        // The race the look was captured on. A look from a custom race is
        // unapplyable without it, and the Character block is what says so.
        if (a_profile.character) {
            detail::Add(out, a_profile.character->race.modName);
        }

        detail::Sort(out);
        return out;
    }

}  // namespace OS::PresetRequirements
