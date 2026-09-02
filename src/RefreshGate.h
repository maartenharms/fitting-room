#pragma once

#include <cstdint>

namespace OS::RefreshGate {

    enum class StagedUpdate {
        kNone,
        kBodyOnly,
        kEquipment,
    };

    // A body preset/ORefit edit does not change a biped object. Sending it
    // through AIProcess::UpdateEquipment needlessly rebuilds every worn armor
    // addon before OBody runs, exposing unrelated malformed armor to Skyrim's
    // unsafe skinning path. Keep that path for actual slot changes only.
    // Hair is the third input and it forces the equipment path. The hair
    // override rides on the worn mask read by engine 24220, which only runs
    // inside the rebuild orchestrator, so the body-only path cannot deliver it.
    // Left out of this classifier, a hair-only edit returned kNone and the
    // character did not change until some unrelated slot was clicked.
    // Hair COLOUR is the fourth input and it forces the equipment path too, for
    // a different reason than visibility does. The colour itself is pushed into
    // the actor base before this classification is acted on, so what is left to
    // arrange is a REBUILD that re-reads it. The body-only path is not one: it
    // runs actor-scoped OBody calls and touches no head geometry at all, so a
    // colour change routed there would sit in the base unseen until the next
    // unrelated slot edit - the same symptom hair visibility had.
    // Hair STYLE is the fifth input, and it is here for a blunter reason than
    // the other four. UpdateStaging returns early on kNone, and that early
    // return sits AHEAD of the hair push, so a style-only edit classified kNone
    // never reaches the code that applies it: the row would highlight, the Apply
    // button would light, and the character's hair would not change. Exactly
    // what happened to hair visibility when it was left out of this classifier.
    //
    // It does not need the equipment REBUILD the way the others do, because
    // HairStyle::Apply issues its own head rebuild. What it needs is to not be
    // dropped on the floor before it gets there.
    //
    // Outfit DYE is the sixth input, and it is here because the painter is
    // driven off a rebuild. OutfitDye::Repaint runs at the tail of
    // REAug::RefreshActor and off the worn-pass hook, and both of those sit
    // behind the kEquipment branch: kBodyOnly runs actor-scoped OBody calls,
    // touches no material and issues no rebuild, and kNone returns before either
    // branch. So a dye-only edit classified anything but kEquipment never
    // reaches the code that paints it, and the swatch would change while the
    // armour did not, until some unrelated slot edit forced a refresh. Same
    // forgotten-half shape as hair visibility, hair colour and hair style before
    // it.
    //
    // ⚠ A dye is still not a slot. It stays out of ChangedSlotCount and off the
    // lore-mode Apply bill; this parameter is a SEPARATE input for the same
    // reason a_bodyDiffers and the three hair inputs are.
    //
    // EYE and BROW type is the seventh input, and it is the one this classifier
    // was actually missing rather than merely gaining. OS-161 made eyes and
    // brows outfit state, gave them a push (PushPlayerHeadParts), a dirty term
    // (HeadPartsDiffer in EditorGate::StillDirty) and a codec field, and never
    // came here. The early return on kNone sits AHEAD of that push, exactly as
    // it does for hair style, so an eyes-only or brows-only edit was applied to
    // nobody: the row renamed itself, the Apply button lit, the character did
    // not change.
    //
    // ⚠ IT LOOKED LIKE IT WORKED, AND THAT IS WHY IT SURVIVED FOUR SESSIONS.
    // Clicking a row in the eye or brow list means hovering it first, and the
    // hover preview calls HeadPart::Apply DIRECTLY, bypassing the outfit. So
    // the part was already on the character before the click ever staged it,
    // and the broken push had nothing left to do. The Random button has no
    // hover, which is the path that finally exposed it (field 2026-08-08).
    //
    // ⚠ THREE WORKAROUNDS WERE WRITTEN BEFORE THE CAUSE WAS, and one of them
    // said so in its own comment ("ClassifyStagedUpdate has no eyes-or-brows
    // term"). If a fourth call site is reaching for HeadPart::Restore straight
    // after a Push(), it is compensating for this and it should not be.
    //
    // EYE COLOUR is the eighth input, and it is the fifth time this classifier
    // has been the forgotten half of a feature. The Eyes row shipped with a
    // codec field, a dye tile and a paint pass, and no term here, so an
    // eye-colour-only edit classified kNone and was painted onto nobody. It
    // reached the screen only when the eye PART changed in the same edit,
    // because HeadPartsDiffer fired for it; the field report reads exactly that
    // way: "it only changes when we switch eye, not when we are going through
    // swatches" (2026-08-13). The painter is PaintEyeTint at the tail of the
    // repaint, and a repaint is issued by the equipment branch alone, so the
    // routing is the dye's.
    //
    // ⚠ NO DEFAULT ARGUMENT ON ANY DIMENSION, the same rule EditorGate::StillDirty
    // carries and for the same failure. a_hairStyleDiffers and a_dyeDiffers both
    // arrived defaulted to false, which is precisely how the NEXT dimension gets
    // forgotten: every existing call site keeps compiling and the new input
    // silently reads false, so the edit classifies kNone and is applied to
    // nothing. Adding a parameter here has to break the build at each caller.
    [[nodiscard]] constexpr StagedUpdate ClassifyStagedUpdate(
        bool a_bodyDiffers, std::uint32_t a_changedSlots, bool a_hairDiffers,
        bool a_hairTintDiffers, bool a_hairStyleDiffers, bool a_dyeDiffers,
        bool a_headPartsDiffer, bool a_eyeTintDiffers) {
        if (a_changedSlots != 0 || a_hairDiffers || a_hairTintDiffers ||
            a_hairStyleDiffers || a_dyeDiffers || a_headPartsDiffer ||
            a_eyeTintDiffers) {
            return StagedUpdate::kEquipment;
        }
        return a_bodyDiffers ? StagedUpdate::kBodyOnly : StagedUpdate::kNone;
    }

    // Whether the head editor closing has to re-assert the outfit's hair colour.
    //
    // The menu is "RaceSex Menu", the VANILLA character editor - RaceMenu
    // reskins it rather than replacing it, so this covers both. Opening it
    // rebuilds the head, and that rebuild runs the engine's own hair painter
    // (BSFaceGenManager::PrepareHeadPartForShaders, SE 26259 / AE 26838), which
    // derives the tint from the actor base and writes it over every hair-tint
    // material. Whatever HairColor::Repaint put on the geometry is gone, and
    // nothing tells Fitting Room it happened - the field log for the 2026-07-30
    // session simply stops at the moment the menu opens.
    //
    // Not a conflict of METHOD: RaceMenu's own RGB slider drives the same
    // engine painter this mod does (SetHairColor, then walk the scenegraph
    // writing tintColor). skee64 exposes no hair-tint interface at all - its
    // published IPluginInterface.h declares ten interfaces and the only colour
    // methods are item TEXTURE LAYER colours, i.e. worn gear. There is nothing
    // to integrate with; the loser is simply whoever wrote first.
    //
    // ⚠ Gated on the outfit actually driving the tint, and that gate is the
    // point. With no outfit colour there is nothing to re-assert, and pushing
    // anyway would run HairColor::Restore - which would overwrite a hair colour
    // the user just deliberately picked in the editor with a stale baseline.
    // Staying out of the way is the correct behaviour there, not a shortcut.
    [[nodiscard]] constexpr bool ShouldReassertAfterHeadEditor(bool a_isHeadEditor,
                                                              bool a_opening,
                                                              bool a_outfitDrivesTint) {
        return a_isHeadEditor && !a_opening && a_outfitDrivesTint;
    }

    // What the head editor opening or closing means for the outfit's EYE and
    // BROW types (OS-161).
    //
    // ⚠ TWO DIRECTIONS, WHICH IS WHAT MAKES THIS DIFFERENT FROM THE HAIR TINT
    // ABOVE. A tint only needs rewriting afterwards, because the editor's own
    // rebuild repaints it and there is nothing to hand over. A head PART is a
    // record on the actor base, which is the very thing RaceMenu edits, so
    // leaving ours in place means the editor opens showing Fitting Room's eyes
    // as though they were the character's own.
    //
    // ⚠ AND THE STALE-CAPTURE HAZARD IS THE REAL REASON, not the cosmetic one.
    // HeadPart's capture is written once and holds what the character had
    // before we first touched them. If we keep the override through the editor,
    // that capture still describes the PRE-EDITOR eyes, so the next time the
    // outfit's eyes are cleared it would restore them and silently undo work
    // the user did in RaceMenu, with no way back. Standing down on the way in
    // drops the capture; re-asserting on the way out takes a fresh one, so
    // whatever they left the editor with becomes the new "their own".
    //
    // ⚠ Gated on the outfit actually naming a part, for the reason the tint
    // gate gives: with nothing of ours on the character there is nothing to
    // hand back, and re-asserting anyway would overwrite eyes the user just
    // deliberately picked in the editor.
    enum class HeadEditorHandoff : std::uint8_t {
        kNone,
        kStandDown,  // opening: put their own parts back, drop our capture
        kReassert,   // closing: apply the outfit's parts again, recapture
    };

    [[nodiscard]] constexpr HeadEditorHandoff ClassifyHeadPartHandoff(
        bool a_isHeadEditor, bool a_opening, bool a_outfitDrivesHeadParts) {
        if (!a_isHeadEditor || !a_outfitDrivesHeadParts) {
            return HeadEditorHandoff::kNone;
        }
        return a_opening ? HeadEditorHandoff::kStandDown : HeadEditorHandoff::kReassert;
    }

    // ---- the character-editor visit (2026-08-17) ----------------------------
    //
    // What Fitting Room knew about ONE dimension of a character's look at the
    // two edges of a RaceSexMenu visit. No engine types: the caller reads the
    // actor base and hands identities over.
    //
    // The problem this solves: a player loads a RaceMenu preset, and the
    // character's Fitting Room DEFAULT hair, eyes, brows or hair colour is
    // re-asserted over it the moment the menu closes, because
    // ClassifyHeadPartHandoff above says kReassert and means it. Letting the
    // default go is the fix the user asked for.
    //
    // ⚠⚠ AND A DELETION HERE COSTS THE PLAYER REAL CURRENCY. Setting a default
    // costs a look, in gold or Seamstone charge (DefaultLook.h), and
    // HeadPartLadder.h already carries the ruling: "deleting one on an act the
    // player can undo would destroy something they bought". So every arm below
    // that is not kClear exists to protect a purchase, and the burden of proof
    // sits on the deletion rather than on the keep.
    //
    // ⚠ before/after are FORM IDS off TESNPC, and 0 is a legitimate "they have
    // no part of this kind". That is why `readable` is its own bit: a failed
    // read must never be mistaken for an empty slot, because one authorises a
    // deletion and the other does not.
    struct LookDimension {
        bool          hasDefault{ false };       // DefaultLook holds one right now
        bool          defaultAuthored{ false };  // the ladder chose kDefault at the OPEN edge
        bool          readable{ false };         // BOTH edges read successfully
        bool          weWrote{ false };          // FR wrote THIS actor's THIS dimension
        std::uint32_t before{ 0 };
        std::uint32_t after{ 0 };
    };

    enum class DefaultVerdict : std::uint8_t {
        kNoDefault,      // nothing bought here, so nothing to lose
        kUnarmed,        // a close with no matching open reading
        kUnreadable,     // a reading failed; uncertainty must not authorise a delete
        kOurOwnWrite,    // Fitting Room moved it during the visit, not the player
        kIdentityMoved,  // race or sex moved, or could not be shown to be steady
        kUnchanged,      // they visited and left this alone
        kDormant,        // it moved, but this default was not what they were wearing
        kClear,          // they changed the thing the default was authoring
    };

    // ⚠ INDEPENDENT OF THE DELETION, ON PURPOSE. Re-pointing a baseline costs
    // the player nothing, and a stale baseline is its own bug: hair and hair
    // colour get no stand-down on the way in, so their captures still describe
    // the PRE-editor character at the close edge. Clearing a default without
    // re-pointing sends the ladder to kOwn, and Restore then writes the
    // pre-editor hair straight over the preset, which is a worse symptom than
    // the one being fixed.
    enum class BaselineRepair : std::uint8_t { kNone, kReseed };

    struct EditorVisit {
        bool armed{ false };
        // ⚠ THE GUARD (user 2026-08-17, "yes add the guard"). Any doubt about
        // identity kills the DELETION for every dimension, because a race or
        // sex change is reversible inside the menu and a switch-race event can
        // land after the close. identityReadable is the third term rather than
        // an implicit true: if we could not read race and sex at both edges we
        // cannot claim they held still, and a claim we cannot make must not
        // authorise destroying a purchase. Repairs are unaffected.
        bool          identityChanged{ false };   // base race or sex differ across the edges
        bool          identitySwitched{ false };  // a switch-race COMPLETED during the visit
        bool          identityReadable{ false };  // race and sex read at BOTH edges
        LookDimension hair{}, eyes{}, brows{}, facialHair{}, hairColour{};
    };

    struct VisitOutcome {
        DefaultVerdict hair{ DefaultVerdict::kNoDefault },
            eyes{ DefaultVerdict::kNoDefault }, brows{ DefaultVerdict::kNoDefault },
            facialHair{ DefaultVerdict::kNoDefault },
            hairColour{ DefaultVerdict::kNoDefault };
        BaselineRepair repairHair{ BaselineRepair::kNone },
            repairEyes{ BaselineRepair::kNone }, repairBrows{ BaselineRepair::kNone },
            repairFacialHair{ BaselineRepair::kNone },
            repairHairColour{ BaselineRepair::kNone };

        // Counted by naming all five rather than by iterating a list: the five
        // dimensions are separate FIELDS by design (DefaultLook.h: each part is
        // independent), and adding a sixth should make the compiler point here.
        [[nodiscard]] constexpr int ClearedCount() const {
            const auto one = [](DefaultVerdict a_v) {
                return a_v == DefaultVerdict::kClear ? 1 : 0;
            };
            return one(hair) + one(eyes) + one(brows) + one(facialHair) + one(hairColour);
        }
        [[nodiscard]] constexpr int ReseededCount() const {
            const auto one = [](BaselineRepair a_r) {
                return a_r == BaselineRepair::kReseed ? 1 : 0;
            };
            return one(repairHair) + one(repairEyes) + one(repairBrows) +
                   one(repairFacialHair) + one(repairHairColour);
        }
    };

    // ⚠ NO DEFAULT ARGUMENTS, the rule this file already carries, and for a
    // sharper reason here: a forgotten input on ClassifyStagedUpdate silently
    // applies nothing, a forgotten one here would silently delete something the
    // player paid for.
    [[nodiscard]] constexpr DefaultVerdict ClassifyVisitedDefault(
        bool a_armed, bool a_identityMoved, const LookDimension& a_dim) {
        // Order matters. "Nothing bought here" comes first so the common case
        // is never an alarm, and so DefaultLook::Clear - which logs
        // unconditionally - is never reached for a default that never existed.
        if (!a_dim.hasDefault) {
            return DefaultVerdict::kNoDefault;
        }
        if (!a_armed) {
            return DefaultVerdict::kUnarmed;
        }
        if (!a_dim.readable) {
            return DefaultVerdict::kUnreadable;
        }
        if (a_dim.weWrote) {
            return DefaultVerdict::kOurOwnWrite;
        }
        if (a_identityMoved) {
            return DefaultVerdict::kIdentityMoved;
        }
        if (a_dim.before == a_dim.after) {
            return DefaultVerdict::kUnchanged;
        }
        if (!a_dim.defaultAuthored) {
            return DefaultVerdict::kDormant;
        }
        return DefaultVerdict::kClear;
    }

    // ⚠ DELIBERATELY NOT GATED ON hasDefault, defaultAuthored OR identity, and
    // each omission is load bearing. A hover preview leaves a capture with no
    // default behind it and nothing repairs that today. A dormant default's
    // dimension still has a stale capture. And a race change is precisely when
    // a capture stops describing the character.
    [[nodiscard]] constexpr BaselineRepair ClassifyBaselineRepair(
        bool a_armed, const LookDimension& a_dim) {
        const bool moved = a_dim.before != a_dim.after;
        return (a_armed && a_dim.readable && !a_dim.weWrote && moved)
                   ? BaselineRepair::kReseed
                   : BaselineRepair::kNone;
    }

    [[nodiscard]] constexpr VisitOutcome ClassifyEditorVisit(const EditorVisit& a_visit) {
        const bool moved = a_visit.armed && (a_visit.identityChanged ||
                                             a_visit.identitySwitched ||
                                             !a_visit.identityReadable);
        VisitOutcome out;
        out.hair       = ClassifyVisitedDefault(a_visit.armed, moved, a_visit.hair);
        out.eyes       = ClassifyVisitedDefault(a_visit.armed, moved, a_visit.eyes);
        out.brows      = ClassifyVisitedDefault(a_visit.armed, moved, a_visit.brows);
        out.facialHair = ClassifyVisitedDefault(a_visit.armed, moved, a_visit.facialHair);
        out.hairColour = ClassifyVisitedDefault(a_visit.armed, moved, a_visit.hairColour);
        out.repairHair       = ClassifyBaselineRepair(a_visit.armed, a_visit.hair);
        out.repairEyes       = ClassifyBaselineRepair(a_visit.armed, a_visit.eyes);
        out.repairBrows      = ClassifyBaselineRepair(a_visit.armed, a_visit.brows);
        out.repairFacialHair = ClassifyBaselineRepair(a_visit.armed, a_visit.facialHair);
        out.repairHairColour = ClassifyBaselineRepair(a_visit.armed, a_visit.hairColour);
        return out;
    }

    // For the log line, and for the field round that grades this.
    [[nodiscard]] constexpr const char* VerdictName(DefaultVerdict a_v) {
        switch (a_v) {
            case DefaultVerdict::kNoDefault:     return "no default";
            case DefaultVerdict::kUnarmed:       return "unarmed";
            case DefaultVerdict::kUnreadable:    return "unreadable";
            case DefaultVerdict::kOurOwnWrite:   return "our own write";
            case DefaultVerdict::kIdentityMoved: return "race or sex not steady";
            case DefaultVerdict::kUnchanged:     return "unchanged";
            case DefaultVerdict::kDormant:       return "default was dormant";
            case DefaultVerdict::kClear:         return "cleared";
        }
        return "?";
    }

    // Pure state machine behind the player refresh deferral. Observing a block
    // freezes visual rebuilds until the block ends and one real-time second has
    // elapsed. With no observed block, refreshes remain immediate.
    class BlockingCooldown {
    public:
        [[nodiscard]] bool Ready(bool a_blocking, double a_nowSeconds) {
            if (a_blocking) {
                sawBlocking_ = true;
                releasedAt_  = -1.0;
                return false;
            }
            if (!sawBlocking_) {
                return true;
            }
            if (releasedAt_ < 0.0) {
                releasedAt_ = a_nowSeconds;
                return false;
            }
            if (a_nowSeconds - releasedAt_ < 1.0) {
                return false;
            }
            sawBlocking_ = false;
            releasedAt_  = -1.0;
            return true;
        }

    private:
        bool   sawBlocking_{ false };
        double releasedAt_{ -1.0 };
    };

}  // namespace OS::RefreshGate
