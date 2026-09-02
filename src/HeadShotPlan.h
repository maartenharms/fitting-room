#pragma once

// Which head-part dimension the shot is on, and what it frames.
//
// WHY THIS IS ITS OWN FILE. Two readers in EditorUI.cpp answer the same
// question about the same selection: SelectionKey builds the string the shot
// driver compares, and CurrentShotFocus picks the node and the distance. They
// have to agree about what is selected, and while they were two separate
// if-ladders they did not. See two-readers-of-one-answer-drift-in-the-gap.
//
// ⚠⚠ A HEAD DIMENSION IS NOT ALWAYS A Kind, AND THAT IS THE WHOLE POINT.
// HeadPart::Kind names four things the engine names. A load order can invent
// slots above the engine's seven (horns on 32 and 106, Chooey's ears on 110,
// measured 2026-08-13); those are discovered at run time and unbounded, and
// HeadPart.h says outright that Kind must never grow to match them. So a
// reader that asks "which Kind is selected" and stops there is blind to every
// invented slot, and blindness here does not read as nothing happening: both
// readers used to fall past their head branches into the trailing armour
// branch, which keys on a g_selectedBit that defaults to the body. The camera
// swung to the torso while the player was fitting horns (user 2026-08-19).
//
// ⚠ /we4062 CANNOT CATCH THAT. It flags a switch that misses an enumerator,
// and the switch below misses none: the gap was the ladder AROUND the switch,
// which simply never ran when there was no Kind at all. PreviewCache.cpp
// records the same trap for kFacialHair at its own line 419, "it is NOT caught
// by /we4062: this is a filter, not a switch". Selection carries both halves,
// so there is no ladder left to fall out of.

#include "HeadPart.h"  // Kind, Slot

#include <optional>
#include <string>

namespace OS::HeadShotPlan {

    // Where the camera turns and how close it comes. An invalid shot means
    // "this selection has nothing to say about the camera", which leaves the
    // framing where the player put it rather than naming a default node.
    struct Shot {
        const char* node{ nullptr };
        float       closeness{ 0.0f };

        [[nodiscard]] bool Valid() const { return node && node[0] != 0; }

        [[nodiscard]] bool operator==(const Shot& a_rhs) const {
            const bool sameNode = (node == nullptr && a_rhs.node == nullptr) ||
                                  (node && a_rhs.node && std::string(node) == a_rhs.node);
            return sameNode && closeness == a_rhs.closeness;
        }
    };

    // What the browser has selected, as both readers see it.
    //
    // ⚠ THE TWO ARE MUTUALLY EXCLUSIVE IN THE EDITOR and this does not lean on
    // it. Every selection path calls ClearBrowseDimensions first, which clears
    // the four Kind flags and the slot together, so only one can be set. The
    // Kind wins below anyway rather than asserting, because a shot on the wrong
    // head node is a smaller fault than no shot at all.
    struct Selection {
        std::optional<HeadPart::Kind> kind{};
        std::optional<HeadPart::Slot> slot{};

        [[nodiscard]] bool Any() const { return kind.has_value() || slot.has_value(); }
    };

    [[nodiscard]] inline Shot ShotFor(const Selection& a_sel) {
        if (a_sel.kind) {
            switch (*a_sel.kind) {
                case HeadPart::Kind::kHair:
                    return { "NPC Head [Head]", 0.85f };
                // The tightest shot in here, and the audience is the reason.
                // OS-161 came from screenarchers wanting a face they can
                // photograph, and an iris or a brow arch is not a thing you can
                // judge from hair distance. Facial hair joins them rather than
                // hair: a beard is a face feature judged at face distance, and
                // framing it at hair distance would put the jaw in the same shot
                // as the shoulders.
                case HeadPart::Kind::kEyes:
                case HeadPart::Kind::kBrows:
                case HeadPart::Kind::kFacialHair:
                    return { "NPC Head [Head]", 0.95f };
            }
        }
        if (a_sel.slot) {
            // ⚠ HAIR DISTANCE, NOT FACE DISTANCE, AND IT IS NOT A TASTE CALL.
            // A discovered slot borrows SceneKind::kHair everywhere else in the
            // editor, which the card grid documents as "a head and shoulders
            // under the horn; Crop::kHead". A horn is the one head part that
            // reaches well above the skull, so the tighter face distance the
            // eyes and brows use would crop the thing being chosen.
            return { "NPC Head [Head]", 0.85f };
        }
        return {};
    }

    // The string the shot driver compares frame to frame. Two different
    // selections must never produce the same one, or picking the second reads
    // as "nothing changed" and the camera never moves.
    //
    // ⚠ THE SLOT KEY CANNOT LOOK LIKE AN ARMOUR KEY. EditorUI's armour branch
    // returns "s" followed by the biped bit, and the horn slot measured on this
    // rig is 32, which is also a biped bit. "s32" would make those one
    // selection as far as the driver is concerned.
    [[nodiscard]] inline std::string KeyFor(const Selection& a_sel) {
        if (a_sel.kind) {
            switch (*a_sel.kind) {
                case HeadPart::Kind::kHair:       return "hair";
                case HeadPart::Kind::kEyes:       return "eyes";
                case HeadPart::Kind::kBrows:      return "brows";
                case HeadPart::Kind::kFacialHair: return "facialhair";
            }
        }
        if (a_sel.slot) {
            return "headslot:" + std::to_string(*a_sel.slot);
        }
        return {};
    }

}  // namespace OS::HeadShotPlan
