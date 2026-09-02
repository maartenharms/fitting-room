#include "TextEntry.h"

#include "PCH.h"

namespace OS::TextEntry {

    namespace {
        // 0 = not resolved yet, -1 = resolved to "cannot be read".
        std::ptrdiff_t g_countOffset = 0;

        // Resolve once and say so once. Called on the first Active(), never at
        // load: the context stack is filled as the game comes up, so asking
        // earlier would be asking whether an object that is not finished yet
        // has the shape we expect.
        void ResolveOnce(const RE::ControlMap* a_map) {
            if (g_countOffset != 0) {
                return;
            }
            const auto* const bytes = reinterpret_cast<const std::uint8_t*>(a_map);
            const auto        base  = ResolveArrayBase(bytes);
            if (base == 0) {
                g_countOffset = -1;
                spdlog::warn(
                    "TextEntry: could not locate ControlMap::textEntryCount on this "
                    "runtime, so the editor hotkey keeps its old console-only guard. "
                    "It will still open while another menu's text field has focus.");
                return;
            }
            g_countOffset = base + kCountFromBase;
            const auto raw =
                *reinterpret_cast<const std::int8_t*>(bytes + g_countOffset);
            // ⚠ THE LINE IS THE PROOF. A field report of "the hotkey does
            // nothing" and one of "the hotkey fires while I type" are opposite
            // faults, and which base was chosen is the first thing that tells
            // them apart. The value beside it should read 0 here, because
            // nothing is being typed at the moment a hotkey press arrives.
            spdlog::info("TextEntry: ControlMap text-entry counter at +0x{:X} "
                         "(array base +0x{:X}), reading {}.",
                         g_countOffset, base, static_cast<int>(raw));
        }
    }  // namespace

    bool Active() {
        auto* const map = RE::ControlMap::GetSingleton();
        if (!map) {
            return false;
        }
        ResolveOnce(map);
        if (g_countOffset <= 0) {
            return false;
        }
        const auto count = static_cast<std::int32_t>(
            *reinterpret_cast<const std::int8_t*>(
                reinterpret_cast<const std::uint8_t*>(map) + g_countOffset));
        return CountMeansTyping(count);
    }

}  // namespace OS::TextEntry
