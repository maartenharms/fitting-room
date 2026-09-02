#pragma once

#include "PCH.h"

#include "Outfit.h"

#include <cstdint>
#include <string>
#include <vector>

// Per-outfit hair COLOUR. Entirely separate machinery from hair VISIBILITY:
// visibility rides the worn mask engine 24220 reads and therefore lives in the
// biped pass, while colour goes through the actor base and facegen and has no
// business in that pass at all.
//
// ⚠ AN EARLIER VERSION OF THIS COMMENT WAS WRONG ON EVERY POINT, AND THE
// FEATURE SHIPPED ONCE LOOKING COMPLETELY INERT BECAUSE OF IT. It claimed a
// spike had proven that Fitting Room's own cheap refresh renders the colour live
// and that RaceMenu's TintMaskInterface covers it. Corrected 2026-07-30 after
// the field test showed no visible change at all:
//
//   * TESNPC::SetHairColor is a PLAIN MEMBER WRITE. It paints nothing.
//   * The cheap refresh does NOT render it. The only engine code that paints
//     hair is BSFaceGenManager::PrepareHeadPartForShaders (SE 26259 / AE 26838),
//     reachable only from the facegen head build, and AIProcess::UpdateEquipment
//     gates that branch behind the kHead|kFace reset flags (SE 38404+0x167 /
//     AE 39395+0x167, `test bpl, 0xC`). This mod's refresh passes kModel alone.
//   * The spike's second button was not a heavier rebuild. Actor::DoReset3D is
//     SetEquipFlag(proc, 0x17) plus the SAME UpdateEquipment, and 0x17 contains
//     kHead. It worked because of one bit, and the log showed that button was
//     pressed 42 times against the cheap one's 3.
//   * RaceMenu's TintMaskInterface CANNOT help. Its hair tinting is
//     bEnableTintHairSlot with uTintHairSlot=2050, biped slots 31 and 41, i.e.
//     WORN hair-slot gear, and its NiNodeUpdate sink walks the worn inventory
//     (kContainerChanges), so it never reaches head-part hair. RaceMenu's own
//     slider does SetHairColor followed by a scenegraph walk that writes the
//     tint, which is exactly what Repaint below does. It remains NOT a hard
//     dependency, and none of its published interfaces expose tint at all.
//
// The lesson worth keeping: a mechanism is not proven by a symptom report. The
// log said which button was pressed and it was not read carefully enough.
//
// ---- Thread safety ------------------------------------------------------
//
// Every function here is safe to call from any thread, and it has to be: this
// module is reached from THREE of them. The FUCK present thread drives the
// editor's staging paths (OutfitSession's colour pushes, and the editor's own
// Snap for the swatch), the input thread drives the quick-switch hotkey, and
// the game thread drives save, load and revert through Persistence.
//
// Two independent locks, both internal:
//   * the captured-baseline map, taken by Apply, Restore, CapturedFor,
//     SeedCaptured and Clear;
//   * the colour palette, taken by WarmPalette and Snap.
// Separate because they guard unrelated state with opposite lifetimes; see
// HairColor.cpp for why the palette is not a std::once_flag.
//
// ⚠ WHAT THE LOCKS DO NOT COVER. They protect the CONTAINERS, nothing else:
//   * A BGSColorForm* returned by Snap outlives the lock. That is safe because
//     the data handler owns it and it is stable for the whole session - it is
//     NOT the lock making it safe, so do not reason from "Snap was locked" to
//     "this pointer is protected".
//   * The actor base is not covered. SetHairColor is deliberately called with
//     no lock held, so two threads writing the same actor's hair colour still
//     race on the base, and the last writer wins. No FR path does that: every
//     caller is one edge-triggered user action on one subject.
//   * Neither lock makes a read-decide-write sequence across two calls atomic.
//     CapturedFor followed by SeedCaptured is two critical sections, not one.
namespace OS::HairColor {

    // A captured pre-Fitting-Room hair colour, as a plugin plus local form ID
    // pair (the shape StyleRefKey already uses) so the reference survives
    // load-order changes.
    struct CapturedColor {
        bool          captured{ false };
        std::string   modName;
        std::uint32_t localFormID{ 0 };
    };

    // Gather FormType::ColorForm once per session. The load order cannot change
    // mid-session, so this never needs invalidating.
    void WarmPalette();

    // Resolve an RGB tint to the nearest colour form in the load order, or null
    // when the palette is empty.
    [[nodiscard]] RE::BGSColorForm* Snap(const HairTint& a_tint);

    // The form's display name, for the editor's snapped swatch. Empty when null.
    [[nodiscard]] std::string NameOf(RE::BGSColorForm* a_form);

    // Apply a_outfit's tint to a_actor, or restore the captured original when the
    // tint is disabled. Does NOT refresh: the caller must request FR's refresh
    // AFTER this returns. Set the colour first, refresh second, which is the
    // order the spike proved.
    void Apply(RE::Actor* a_actor, const Outfit& a_outfit);

    // Apply one tint directly, or restore the captured original when it is
    // cleared. Apply above is this with the outfit's tint handed in.
    void ApplyTint(RE::Actor* a_actor, const HairTint& a_tint);

    // ⚠⚠ THE WHOLE LADDER, AND EVERY CALLER SHOULD USE THIS RATHER THAN Apply.
    // The outfit's colour wins where it names one, a character's DEFAULT colour
    // answers when it does not, and only a character with neither goes back to
    // their own. a_outfit may be null, which is the no-outfit case and still
    // reaches the default: a character who is always this shade is still that
    // shade in their own clothes.
    //
    // ⚠ ONE READER ON PURPOSE. The player path and the follower path both
    // resolve this question, and a ladder written out at each call site is two
    // readers of one answer that will drift in the gap - which is exactly the
    // defect the hide-outfit round was spent on. The rung lives here, so both
    // inherit it and so does the hotkey cycle.
    void Push(RE::Actor* a_actor, const Outfit* a_outfit);

    // Put the captured original back and forget it. Safe when nothing was
    // captured.
    void Restore(RE::Actor* a_actor);

    // The LOAD-BOUNDARY variant of Restore, and the difference is the guard:
    // it restores ONLY while the actor base still wears the exact colour form
    // this module last applied. A session's g_state outlives a save load, so
    // loading an earlier save whose co-save names no look leaves the base
    // wearing the abandoned session's colour with nobody to put it back -
    // that is the contamination. The lastApplied comparison is the same test
    // Apply's recapture uses: if anything else (the head editor, RaceMenu,
    // the load's own changeform) moved the colour since, this does nothing.
    // Returns whether a restore ran.
    bool RestoreIfStillOurs(RE::Actor* a_actor);

    // Paint the actor's CURRENT hair colour onto its live geometry, and return
    // how many hair-tint materials were written. Zero is meaningful, see below.
    //
    // ⚠ THIS IS WHAT MAKES THE COLOUR VISIBLE. Setting hairColor on the actor
    // base changes nothing on screen by itself: TESNPC::SetHairColor is a plain
    // member write. The only engine code that paints it is
    // BSFaceGenManager::PrepareHeadPartForShaders (SE 26259 / AE 26838), which
    // is reachable ONLY from the facegen head build, and AIProcess::
    // UpdateEquipment gates that whole branch behind the kHead|kFace reset flags
    // (SE 38404+0x167 / AE 39395+0x167, `test bpl, 0xC`). Fitting Room's refresh
    // passes kModel alone, so the gate never opens and the hair keeps its old
    // tint. That is why the feature shipped once looking completely inert.
    //
    // So do what the engine does, directly: walk the actor's 3D and write
    // BSLightingShaderMaterialHairTint::tintColor on every geometry whose
    // material reports Feature::kHairTint. Same conversion the engine uses,
    // byte * (1/255) * 2, which is NOT the /128 RE::Actor::UpdateHairColor
    // applies. Matching the engine matters because any later head rebuild
    // re-derives the tint from hairColor with the engine's constant, and a
    // mismatched value would visibly shift at that moment.
    //
    // The COUNT is the diagnostic and the reason this is not just a call to
    // RE::Actor::UpdateHairColor, which returns nothing. Zero written means the
    // hair mesh carries no hair-tint material at all, which no amount of
    // rebuilding or flag-setting can fix, because the engine's own painter has
    // the identical predicate. That distinction is the whole fallback tree.
    //
    // Call it AFTER the colour is on the base and after the refresh, on the main
    // thread. Cheap: one scenegraph walk and three float stores per match, no
    // allocation and no engine call.
    std::size_t Repaint(RE::Actor* a_actor);

    // The colour the actor's hair carries right now, straight off the actor
    // base. nullopt when there is no head data to read, which is the same
    // condition Apply and Repaint refuse on.
    //
    // For seeding the editor's picker when the user first ticks the colour box:
    // starting at black meant every character began the interaction by having
    // their hair turned black, which reads as a bug rather than a default.
    // Their own colour is the only sensible starting point.
    //
    // ⚠ Reads the base, so it reports FITTING ROOM's colour while one is
    // applied. That is correct for the one caller it has - the box being ticked
    // is the off-to-on edge, and with the box off no Fitting Room colour is on
    // the actor - but it makes this the wrong function to ask "what was their
    // original colour", which is what CapturedFor below is for.
    [[nodiscard]] std::optional<HairTint> CurrentColorOf(RE::Actor* a_actor);

    // WHICH colour form sits on the actor base right now, as a form id.
    //
    // ⚠ NOT CurrentColorOf, AND THE DIFFERENCE DECIDES A DELETION. That one
    // falls back to the shade Repaint observed on the geometry when the base
    // has none, which is Fitting Room's own sample; this one reports the base
    // or nothing at all. nullopt means the character could not be read, 0 means
    // they were read and carry no colour form, and those two must never
    // collapse into one answer: HeadEditorSink compares this across a
    // character-editor visit and a failed read must not look like a change.
    [[nodiscard]] std::optional<std::uint32_t> CurrentColourFormId(RE::Actor* a_actor);

    // How many times Fitting Room has written this actor's hair colour, apply
    // and restore alike. The counterpart of HeadPart::WriteCountFor and it
    // answers the same question across the same window; see that header.
    [[nodiscard]] std::uint32_t WriteCountFor(RE::Actor* a_actor);

    // Re-point an EXISTING baseline at whatever colour the actor carries now.
    //
    // ⚠⚠ RESEEDS, NEVER ERASES, and this is not a style preference. Erasing
    // resets `attempted`, and ReassertPlayerHairColor runs three statements
    // later on the same stack: the !attempted branch would then Describe our
    // OWN colour and 'HCOL' would persist it as the character's original.
    // Refuses when nothing was ever captured, for HeadPart::ReseedCapture's
    // reason: a baseline invented here is one a later Restore would paint on.
    void ReseedCaptured(RE::Actor* a_actor);

    // Accessors for persistence. The captured original is CHARACTER state, not
    // outfit state, so it is stored outside the outfit record.
    [[nodiscard]] CapturedColor CapturedFor(RE::Actor* a_actor);
    void                        SeedCaptured(RE::Actor* a_actor, const CapturedColor& a_captured);

    // Discard every captured baseline. Called on a save revert, when the game is
    // switching to a different character entirely and the old baselines describe
    // somebody else.
    //
    // ⚠ Deliberately does NOT restore anything. A revert is already tearing the
    // old character's actor base down and replacing it from the incoming save, so
    // calling SetHairColor here would write to state that is about to be discarded
    // at best, and to the WRONG character's base at worst. Discard only.
    // What the hair GEOMETRY is actually wearing, read live off the
    // scenegraph and formatted for a log line. Empty when the actor has no 3D.
    //
    // ⚠⚠ NOT CurrentColorOf, AND THE DIFFERENCE IS THE WHOLE POINT.
    // That one answers "what colour did Fitting Room last observe or paint",
    // out of a cache, for a picker. This one answers "what is on the strands
    // right now", by walking, for a watchdog. r85 is why the second question
    // exists: the Nord's actor base carried NO hair colour form at all and the
    // hair still drew black, so every field keyed on the base read clean while
    // the thing the player was looking at was wrong. The head texture turned
    // out to be the same shape of question.
    //
    // ⚠ GAME THREAD ONLY. It walks the scenegraph, so the present thread
    // must keep using CurrentColorOf and its cache.
    [[nodiscard]] std::string GeometryTintReading(RE::Actor* a_actor);

    // The same reading from a node the caller already has, for a detour
    // that is standing on the tree and cannot afford a Get3D round trip.
    [[nodiscard]] std::string GeometryTintReadingOf(RE::NiAVObject* a_root);

    // The names of the geometries carrying a hair-tint material under a_root,
    // deduplicated. It exists so a caller can ask skee what it holds against
    // each of them WITHOUT writing a second copy of the traversal predicate:
    // this file already states that the paint, the restore and the sample have
    // to agree on that predicate exactly, and a probe that walked its own tree
    // would be measuring different strands than the ones the pulses read.
    //
    // ⚠ GAME THREAD ONLY, for the same reason GeometryTintReading is.
    [[nodiscard]] std::vector<std::string> HairTintNodeNamesOf(RE::NiAVObject* a_root);

    void Clear();

}  // namespace OS::HairColor
