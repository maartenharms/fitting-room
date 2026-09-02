#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A head-part SLOT that Fitting Room did not know about at compile time.
//
// The engine names seven head-part types, kMisc through kEyebrows, and the
// field holding them is four bytes wide. RaceMenu's convention is that a mod
// wanting a slot of its own picks an unused number above that range and owns
// it. MEASURED on this rig 2026-08-13: ED Horns uses 106, Chooey's Dint Ears
// uses 110 for its eleven selectable parts, and the horn slider the reporting
// character actually wears uses 32. All three are ordinary head parts in every
// other respect.
//
// ⚠ HeadPart::Kind DOES NOT GROW A CASE FOR THESE, and that is the whole
// reason this file exists. Kind is closed on purpose (HeadPart.h) so that a
// switch over it makes the compiler list every site needing a case when a new
// kind arrives. A discovered slot can never be a compile-time case, so widening
// the enum would defeat the argument that justifies it. A slot is a raw type
// number; the four shipped kinds map onto slot numbers through EngineType.
//
// Engine-free on purpose, like HeadPartPlan.h and NpcHairPlan.h beside it: the
// discovery loop needs TESDataHandler and can therefore never be unit tested,
// so the decisions live here where a test can catch them being wrong.
namespace OS::HeadPartSlotPlan {

    // One past the engine's last named type. Anything at or above this is a
    // slot somebody invented, which is exactly what this module is about.
    //
    // ⚠ NOT SPELLED AS RE::BGSHeadPart::HeadPartType::kTotal, because this
    // header must stay engine-free. HeadPart.cpp static_asserts the two agree,
    // so a CommonLib that adds an eighth type breaks the build rather than
    // silently reclassifying a vanilla type as custom.
    inline constexpr std::uint32_t kFirstCustomType = 7;

    [[nodiscard]] inline bool IsCustomType(std::uint32_t a_type) {
        return a_type >= kFirstCustomType;
    }

    // What discovery found for one slot. The count is kept because a slot with
    // one part is almost always a mod's "none" placeholder and reads as broken
    // in a browser, and because a log line that says how many parts a slot has
    // is how a missing mod gets diagnosed from one log.
    struct Slot {
        std::uint32_t type{ 0 };
        std::size_t   parts{ 0 };
        std::string   plugin;  // defining plugin of the first non-extra part
        std::string   label;   // resolved by LabelFor, never empty
    };

    // The name shown on the row.
    //
    // Nothing in a head-part record says what its slot is FOR, so the label has
    // to come from beside it. MEASURED convention, true for both mods on this
    // rig: the plugin ships Interface/translations/<plugin>_<lang>.txt holding
    // exactly ONE key, and that key's value is the name RaceMenu puts on the
    // slider. `$PRMI_EDHorns` gives "ED Horns"; `$Chooey_DintEarsEdit` gives
    // "Chooey's Dint Ears Edit".
    //
    // ⚠ A CONVENTION, NOT A SCHEMA, and the ladder is built to fall off it
    // cleanly. The key name is arbitrary and nothing ties it to the type
    // number, so a plugin declaring two slots would give both the same label.
    // That is why the sole-key test is a REQUIREMENT rather than a preference:
    // a file with two keys cannot say which slot either belongs to, so it is
    // refused and the plugin name is used instead.
    //
    // a_soleTranslationValue is the value of that only key, or empty when the
    // file is missing, unreadable, or holds anything other than one key.
    // a_pluginName is the defining plugin with its extension still on.
    [[nodiscard]] inline std::string LabelFor(std::uint32_t a_type,
                                              std::string_view a_soleTranslationValue,
                                              std::string_view a_pluginName) {
        if (!a_soleTranslationValue.empty()) {
            return std::string{ a_soleTranslationValue };
        }
        if (!a_pluginName.empty()) {
            std::string_view stem = a_pluginName;
            if (const auto dot = stem.find_last_of('.'); dot != std::string_view::npos) {
                stem = stem.substr(0, dot);
            }
            if (!stem.empty()) {
                return std::string{ stem };
            }
        }
        // Never empty. A row the player cannot name is still a row they can
        // use, and an empty label reads as a rendering fault rather than as a
        // mod that shipped no name.
        return "Head part type " + std::to_string(a_type);
    }

    // Parse a translation file's text into the sole value the ladder wants.
    //
    // The format is one key, a TAB, then the value, per line. Fitting Room's
    // own tables are the same shape. Returns empty unless the file holds
    // EXACTLY one usable line, for the reason on LabelFor.
    //
    // ⚠ A BOM IS EXPECTED AND IS NOT A KEY. These files are UTF-16 on disk and
    // the caller decodes them; a leading U+FEFF survives that decode and would
    // otherwise become part of the first key, which does not matter here (the
    // key is discarded) but would if this ever grew a key lookup.
    [[nodiscard]] inline std::string SoleTranslationValue(std::string_view a_text) {
        std::string        found;
        std::size_t        usable = 0;
        std::size_t        pos = 0;
        while (pos <= a_text.size()) {
            const auto nl   = a_text.find('\n', pos);
            auto       line = a_text.substr(pos, nl == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : nl - pos);
            pos = (nl == std::string_view::npos) ? a_text.size() + 1 : nl + 1;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const auto tab = line.find('\t');
            if (tab == std::string_view::npos) {
                continue;  // a blank line or a comment, not a pair
            }
            auto value = line.substr(tab + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            if (value.empty()) {
                continue;
            }
            ++usable;
            if (usable > 1) {
                return {};  // two keys cannot say which slot they name
            }
            found = std::string{ value };
        }
        return found;
    }

    // Put the discovered slots in the order the browser shows them.
    //
    // By type number, ascending. Not by label and not by part count: the type
    // is the only thing about a slot that is stable across sessions, and an
    // order that moves when a mod is renamed or when a plugin's part count
    // changes would move the row under the player's cursor between sessions.
    // The same reasoning already governs AvailableFor's sort.
    inline void SortForDisplay(std::vector<Slot>& a_slots) {
        for (std::size_t i = 1; i < a_slots.size(); ++i) {
            auto        held = std::move(a_slots[i]);
            std::size_t j    = i;
            while (j > 0 && a_slots[j - 1].type > held.type) {
                a_slots[j] = std::move(a_slots[j - 1]);
                --j;
            }
            a_slots[j] = std::move(held);
        }
    }

}  // namespace OS::HeadPartSlotPlan
