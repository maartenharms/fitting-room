#pragma once

#include "EditorGate.h"  // PaneMode

#include <optional>
#include <span>

// Which pages the player wants in the rail, and where to land when the page
// they are standing on stops being drawn.
//
// ⚠⚠ WANTED IS NOT THE SAME QUESTION AS DRAWN, AND THEY MUST NOT MERGE. This
// file answers "did the player ask for this page". Whether the tile appears
// also depends on things the player does not control: OBody being installed for
// Bodies, the build channel for anything behind FR_BODY_STUDIO, a preset source
// existing for Presets. Those are asked separately at the rail, every frame.
//
// The reason to keep them apart is the one an-install-choice-must-not-gate-a-
// runtime-setting records. A requirement is a fact about right now and it can
// change while the game is running; a preference is a decision the player made
// and it has to survive a load order they have not fixed yet. Fold them into
// one bool and removing a mod for one session silently rewrites what the player
// asked for.
//
// ⚠⚠ AND THERE IS NO PAGE ORDER IN THIS FILE, DELIBERATELY. EditorUI records
// the order as the tiles draw and its banner says why: a second list would be a
// second answer to "what pages are there" and the two would drift the first
// time a tile grew a condition. Correction below takes the drawn list as an
// argument rather than holding one of its own.
namespace OS::PagePolicy {

    // ⚠ EditorGate owns the enum and keeps it engine-free so its own suite can
    // run. Aliasing it here rather than qualifying every site is what EditorUI
    // already does, for the same reason: every PaneMode:: below reads the way
    // it reads at the rail.
    using PaneMode = EditorGate::PaneMode;

    // The player's choice, one flag per page. Everything on is the shipped
    // default: a page that is off out of the box is a page nobody finds.
    struct Visibility {
        bool styles{ true };
        bool presets{ true };
        bool dye{ true };
        bool rules{ true };
        bool bodies{ true };
        bool shape{ true };
        bool overlays{ true };
        bool looks{ true };

        friend bool operator==(const Visibility&, const Visibility&) = default;
    };

    [[nodiscard]] constexpr bool Wanted(const Visibility& a_v, PaneMode a_mode) {
        switch (a_mode) {
            case PaneMode::kStyles:     return a_v.styles;
            case PaneMode::kPresets:    return a_v.presets;
            case PaneMode::kDye:        return a_v.dye;
            case PaneMode::kRules:      return a_v.rules;
            case PaneMode::kBodyStudio: return a_v.bodies;
            case PaneMode::kShape:      return a_v.shape;
            case PaneMode::kOverlays:   return a_v.overlays;
            case PaneMode::kProfiles:   return a_v.looks;
        }
        // ⚠ A pane this file has not been taught about is WANTED. The failure
        // that costs a bug report is a page the player cannot reach, not one
        // they did not ask to hide, so an unknown value fails toward visible.
        return true;
    }

    // How many pages the player has asked for. This is the PREFERENCE count and
    // not what is on screen, so the panel can say "you have hidden six of
    // eight" without claiming anything about OBody being installed.
    [[nodiscard]] constexpr int WantedCount(const Visibility& a_v) {
        int n = 0;
        for (const auto m : { PaneMode::kStyles, PaneMode::kPresets, PaneMode::kDye,
                              PaneMode::kRules, PaneMode::kBodyStudio, PaneMode::kShape,
                              PaneMode::kOverlays, PaneMode::kProfiles }) {
            if (Wanted(a_v, m)) {
                ++n;
            }
        }
        return n;
    }

    // Put Styles back when the player has turned every page off. Returns true
    // if it changed anything.
    //
    // ⚠⚠ THIS IS WHAT MAKES THE EMPTY RAIL UNREACHABLE, AND IT HAS TO RUN IN
    // BOTH PLACES. A load reads whatever the INI says and the INI is a text
    // file, so clamping only in the panel leaves a hand-edited all-off install
    // with nowhere to stand; clamping only at load lets the panel walk into the
    // same state a moment later. One function, called from both, beats an empty
    // state drawn in a rail that by then has no tiles to draw it beside.
    //
    // ⚠ Styles rather than the first flag that happens to be false: it is the
    // page the mod exists for, it has no requirement that can be missing, and
    // it is the first tile, so the correction below lands on it anyway.
    inline bool EnsureAtLeastOne(Visibility& a_v) {
        if (WantedCount(a_v) != 0) {
            return false;
        }
        a_v.styles = true;
        return true;
    }

    // Where the editor should be standing, given the pages that actually drew.
    //
    // nullopt means STAY PUT: either the current page is still on screen, or
    // there is nothing to move to. A value means switch to it.
    //
    // ⚠⚠ THE CALLER TELLS THE EMPTY CASE APART BY LOOKING AT a_drawn, NOT BY
    // LOOKING AT THIS. Both "you are fine" and "there is nowhere to go" return
    // nullopt on purpose, because the alternative is a tri-state whose third
    // value every caller would have to remember to handle. An empty a_drawn is
    // the empty state and it is one test.
    //
    // ⚠ It corrects for a requirement disappearing as readily as for a
    // preference changing, because it only asks what drew. Uninstall OBody
    // while standing on Bodies and this moves you off it.
    [[nodiscard]] inline std::optional<PaneMode> Correction(std::span<const PaneMode> a_drawn,
                                                            PaneMode                 a_current) {
        if (a_drawn.empty()) {
            return std::nullopt;
        }
        for (const auto m : a_drawn) {
            if (m == a_current) {
                return std::nullopt;
            }
        }
        return a_drawn.front();
    }

}  // namespace OS::PagePolicy
