#pragma once

#include "ProfileCodec.h"

#include <vector>

// The apply pipeline's engine-free half: WHICH steps run and IN WHAT ORDER
// for a given profile and a given set of per-block checkboxes.
//
// ⚠⚠ THE ORDER IS THE TWO-PAINTER RESOLUTION MADE EXECUTABLE (spec section
// 6). Character first because it decides WHO the rest applies to: the sex
// flag flips before any rebuild reads it, and the race switch rides the face
// step's LoadCharacterEx when a face runs (skee's ApplyPreset calls SetRace
// with the race it is handed, source read 2026-08-22) or runs as its own
// SwitchRace when none does. Face next because LoadCharacterEx rebuilds the
// head, rewrites face tints and rebuilds the tint list, and everything after
// absorbs that. Outfit before makeup because the ladder's part pushes
// rebuild and re-derive the tint. Makeup after both, off a fresh
// Fingerprint. Weight before body and shape so the morph plan interpolates
// at the profile's weight. Skin before overlays; overlays last, then the
// reconcile and the 1p pass at the settled point the dye chain already uses.
// A REORDER IS A REGRESSION EVEN WHEN EVERY STEP PASSES ALONE, which is why
// the sequence lives in one pure function with a test pinning it, not in
// whatever order some caller happens to write.
//
// Partial apply is per-block checkboxes on this same order with steps
// skipped: participation changes, the order never does.
namespace OS::ProfilePlan {

    enum class Step : std::uint8_t {
        kCharacter,
        kFace,
        kOutfit,
        kMakeup,
        kWeight,
        kBody,
        kShape,
        kSkin,
        kOverlays,
    };

    // One log label per step, so a field report reads as a checklist. These
    // strings are the contract the ordering test asserts.
    [[nodiscard]] inline const char* LogLabel(Step a_step) {
        switch (a_step) {
            case Step::kCharacter: return "character";
            case Step::kFace:     return "face";
            case Step::kOutfit:   return "outfit";
            case Step::kMakeup:   return "makeup";
            case Step::kWeight:   return "weight";
            case Step::kBody:     return "body";
            case Step::kShape:    return "shape";
            case Step::kSkin:     return "skin";
            case Step::kOverlays: return "overlays";
        }
        return "?";
    }

    // Which blocks take part: the profile's presence AND'd with the page's
    // checkboxes (full apply passes all-true checkboxes).
    //
    // ⚠ dyes IS A HALF OF THE OUTFIT STEP, NOT A NINTH STEP. The user's split
    // (2026-08-22): gear and dyes are separate checkboxes, but one push
    // paints both, so the outfit step runs when EITHER is taken and composes
    // what it pushes from the two flags. The pinned label list below is
    // unchanged, which is the point: a new label would be a reorder by
    // another name.
    struct Participation {
        bool character{ false };
        bool face{ false };
        bool outfit{ false };
        bool dyes{ false };
        bool makeup{ false };
        bool weight{ false };
        bool body{ false };
        bool shape{ false };
        bool skin{ false };
        bool overlays{ false };
    };

    [[nodiscard]] inline Participation ParticipationFor(
        const ProfileCodec::Profile& a_profile) {
        Participation p;
        p.character = a_profile.character.has_value();
        p.face     = a_profile.face.has_value();
        p.outfit   = a_profile.outfit.has_value();
        // The dye data lives inside the outfit block, so its presence is the
        // outfit's; the split is a choice about APPLYING, not about storage.
        p.dyes     = a_profile.outfit.has_value();
        p.makeup   = a_profile.makeup.has_value() && !a_profile.makeup->empty();
        p.weight   = a_profile.weight.has_value();
        p.body     = a_profile.body.has_value();
        p.shape    = a_profile.shape.has_value();
        p.skin     = a_profile.skin.has_value();
        p.overlays = a_profile.overlays.has_value();
        return p;
    }

    [[nodiscard]] inline Participation And(Participation a_a, Participation a_b) {
        return Participation{
            a_a.character && a_b.character,
            a_a.face && a_b.face,       a_a.outfit && a_b.outfit,
            a_a.dyes && a_b.dyes,
            a_a.makeup && a_b.makeup,   a_a.weight && a_b.weight,
            a_a.body && a_b.body,       a_a.shape && a_b.shape,
            a_a.skin && a_b.skin,       a_a.overlays && a_b.overlays,
        };
    }

    [[nodiscard]] inline Participation AllOn() {
        return Participation{ true, true, true, true, true,
                              true, true, true, true, true };
    }

    // How many blocks a set names.
    //
    // ⚠ ONE COUNTER, BECAUSE A HAND-COUNTED EXPRESSION GOES STALE. The Looks
    // page tells the player how many parts an apply will move, and a sum that
    // forgets a field the day an eleventh block is added reads as a bug in the
    // apply rather than in the label.
    [[nodiscard]] inline int Count(const Participation& a_p) {
        return (a_p.character ? 1 : 0) + (a_p.face ? 1 : 0) +
               (a_p.outfit ? 1 : 0) + (a_p.dyes ? 1 : 0) +
               (a_p.makeup ? 1 : 0) + (a_p.weight ? 1 : 0) +
               (a_p.body ? 1 : 0) + (a_p.shape ? 1 : 0) + (a_p.skin ? 1 : 0) +
               (a_p.overlays ? 1 : 0);
    }

    // What "everything" means for ONE look on THIS rig: the blocks the file
    // carries, minus the ones the rig cannot honour.
    //
    // ⚠ NOT AllOn(). A true on a block the file does not carry is a lie -
    // Apply ANDs it away, so the box would tick and nothing would happen, and
    // the player would be right to call that broken.
    [[nodiscard]] inline Participation EveryUsable(const Participation& a_present,
                                                   bool a_faceUsable) {
        auto p = a_present;
        if (!a_faceUsable) {
            p.face = false;
        }
        return p;
    }

    // THE sequence. Participation filters it; nothing reorders it.
    [[nodiscard]] inline std::vector<Step> Build(const Participation& a_take) {
        std::vector<Step> steps;
        if (a_take.character) steps.push_back(Step::kCharacter);
        if (a_take.face)     steps.push_back(Step::kFace);
        if (a_take.outfit || a_take.dyes) steps.push_back(Step::kOutfit);
        if (a_take.makeup)   steps.push_back(Step::kMakeup);
        if (a_take.weight)   steps.push_back(Step::kWeight);
        if (a_take.body)     steps.push_back(Step::kBody);
        if (a_take.shape)    steps.push_back(Step::kShape);
        if (a_take.skin)     steps.push_back(Step::kSkin);
        if (a_take.overlays) steps.push_back(Step::kOverlays);
        return steps;
    }

}  // namespace OS::ProfilePlan
