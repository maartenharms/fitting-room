#pragma once

#include <cstdint>

namespace RE {
    class Actor;
}

// The sex a LOOK states for this character, held by Fitting Room because the
// engine will not keep it.
//
// ⚠⚠ THE FAULT, FIELD 2026-08-25 08:56. ProfileApply writes the flag straight
// onto the player's base:
//
//     base->actorData.actorBaseFlags.set(Flag::kFemale);
//
// and form 00000007 is a STATIC form out of Skyrim.esm whose default is male.
// Nothing marks the base as changed, so the engine never writes the flag into
// the save and the ESM value wins at the next load. The round measured it
// exactly: the save was taken eight seconds after `female false -> true`, and
// the fresh launch that loaded it read `female=false` in both baselines and
// `sex=M` in every probe, while the head built as '00UBE_FemaleHead'.
//
// A male-flagged base wearing a female head is why the body physics looked
// dead: nothing drives breast bones on a body built for the other sex.
//
// ⚠ THIS IS THE SAME SHAPE AS 'OVLB' AND 'SKTN' and it is the last block of a
// look that had no survival story. Fitting Room sets an attribute, the
// attribute lives somewhere a load clears, and nothing puts it back. Hold it,
// save it, put it back.
namespace OS::CharacterSex {

    // A look stated this character's sex. Called from the one place that
    // writes the flag, so the record cannot drift from what was written.
    void Hold(bool a_female);

    // What is held, or false when nothing is. a_female is untouched on false.
    [[nodiscard]] bool Held(bool& a_female);

    // A load boundary: this belongs to the outgoing character. The player's
    // base is form 00000007 in every save, so nothing else separates them.
    void Clear();

    // Remember what the base read BEFORE Fitting Room first wrote the flag
    // this session, so the load boundary can put it back.
    //
    // ⚠⚠ THE FLAG IS A WRITE ONTO A SHARED STATIC FORM AND CLEARING OUR
    // RECORD DOES NOT UNDO IT. Field 2026-08-25: apply a look whose sex differs
    // from the character wearing it, then load another save, and that save's
    // character can come up with the wrong body. Clear() below drops what we
    // are HOLDING; nothing put the FLAG back, and base 00000007 is one form
    // shared by every character in every save. Same shape as the hair capture
    // that leaked across save reverts on 2026-08-16: a mark whose eraser was
    // never written.
    //
    // ⚠ Idempotent, and that is the point: only the FIRST call of an epoch
    // records anything, so a second write cannot overwrite the baseline with
    // Fitting Room's own value.
    void NoteBaselineBeforeWrite(RE::Actor* a_actor);

    // Put the pre-Fitting-Room flag back and spend the baseline. A no-op when
    // this session never wrote one, which is the common case and must stay
    // silent. Returns true if it wrote.
    //
    // ⚠ THE SEAM IS RevertCallback, before Clear(). Revert is the one event
    // both a load and a new game cross, and it runs BEFORE the incoming save's
    // own 'SEXF' re-asserts at kPostLoadGame, so a character who legitimately
    // owns a flipped flag still gets it back a moment later.
    bool RestoreBaseline(RE::Actor* a_actor);

    // Put the held flag back on the actor's base. Safe to call before the 3D
    // exists and that is the point: the body and head are built from this, so
    // it has to be right before they are made rather than corrected after.
    // Returns true if it wrote.
    bool Apply(RE::Actor* a_actor);

}  // namespace OS::CharacterSex
