#pragma once

#include "MakeupPlan.h"
#include "ProfileCodec.h"  // MakeupEntry, the shape a look already stores

#include <optional>
#include <vector>

namespace RE {
    class Actor;
}

// The player's TINT LIST, held by Fitting Room so that skee's cosave is not
// the only copy of it.
//
// ⚠⚠ THE FAULT THIS EXISTS FOR, FIELD 2026-09-02 00:14. Apply Almalexia over
// Umbrael, save, load: the face came back wearing Umbrael's skull warpaint
// ('CO 3\56 Head 2 F M', her tint-list slot) with part of Almalexia's makeup
// gone, while the OVERLAY record's converge reported full agreement and its
// pushes all landed. The skull is not an overlay. The 108-slot tint list is
// serialized by RaceMenu in ITS cosave, our StepMakeup writes mutate the live
// masks in place, and skee's restore puts back the list IT last knew, which
// predates every in-session write of ours. The load-time rebake then
// composites the face from that reverted list, faithfully: "the face tint was
// rebaked after the load, from the tint list this save restored". Same split
// the overlay baseline closed one hour earlier, one container down.
//
// The medicine is the same and mostly reused: a record fed at the write choke
// point, carried in our cosave, re-asserted after skee's restore through
// MakeupPlan::PlanFaceCarried, whose write/clear semantics and tests already
// exist. The record follows every author: our writes re-sync it at the end of
// each batch, and a RaceSexMenu visit re-syncs it on the close edge, so
// RaceMenu work is in the record before any save can carry it.
namespace OS::MakeupBaseline {

    // Re-capture the record from the live list, wholesale. Called at the end
    // of every write batch (the choke point) and on the RaceSexMenu close
    // edge. ⚠ Refuses a list that is not this race's (the r35 shape): a
    // wrong-race list is a stranger's, and a record captured from it would
    // re-assert that stranger after the next load.
    void SyncFromList(RE::Actor* a_actor);

    // Everything recorded, in the block shape a look already uses, so the
    // co-save reuses ProfileCodec's JSON (the OVLB record's own reasoning).
    [[nodiscard]] std::vector<ProfileCodec::MakeupEntry> Snapshot();

    [[nodiscard]] bool HasAny();

    // Whether the record makes a CLAIM at all, entries or not.
    //
    // ⚠⚠ A BARE LIST IS A CLAIM, NOT AN ABSENCE, AND THE 2026-09-02 02:40
    // FIELD ROUND IS WHY. Almalexia was applied over Umbrael on a list that
    // stayed 34 slots against a 108 slot race, so the makeup step refused, the
    // re-sync refused, and the record the Umbrael apply had dropped stayed
    // EMPTY. HasAny was false, so the save wrote no 'MKUP' record at all; the
    // user then loaded an older save (Umbrael's six converged and mirrored)
    // and loaded the new one, which restored nothing, and the six were on
    // Almalexia. Recorded() is true from a sync, a drop or a restore, and only
    // a load boundary makes it false: the save writes the claim even when it
    // is bare, and the converge pushes bareness like any other record.
    [[nodiscard]] bool Recorded();

    // The apply's drop: this look replaces the character, so the record now
    // claims what the LOOK's own makeup block says (nothing, for a look that
    // carries none) until the apply's makeup step, or any later write,
    // re-records what the list wears. Unstamped, because the incoming
    // character is not the one standing here yet.
    //
    // ⚠⚠ SEEDED WITH THE LOOK'S BLOCK, NOT LEFT BARE, and the 03:1x field round
    // is why: a race-switching apply leaves the list the OLD race's length for
    // the whole session (r35), the makeup step refuses to write into it, and
    // RaceMenu then replays its own copy of the tints by index about a second
    // later. A bare record would erase that replay even when it is the look's
    // own makeup (Umbrael over a Nord's list); a record carrying the look's
    // block keeps the r35 refusal for a makeup-bearing look and clears only
    // for a look that wears nothing, which is the Nord 3 case.
    void Adopt(const std::vector<ProfileCodec::MakeupEntry>& a_look);

    // Whether the record claims no makeup: nothing, or the skin tone alone.
    // A record like that can be pushed onto ANY list, because pushing it is a
    // clear and a clear writes nothing by index.
    [[nodiscard]] bool BareOfMakeup();

    // WHO the record was captured on, stamped by every successful sync and
    // carried in the co-save as the wrapper profile's character block.
    //
    // ⚠⚠ THE STAMP EXISTS BECAUSE A RECORD CAN OUTLIVE ITS CHARACTER. Field
    // 2026-09-02 01:18, the Nord 3 leftovers: a race-switching apply refuses
    // both the makeup write and the batch-end sync (the r35 shape, correctly),
    // so nothing between the switch and the save could teach the record that
    // Umbrael was gone. Her fourteen entries crossed the save, and the
    // post-load converge saw a live list whose LENGTH matched the new race
    // and painted her makeup onto Nord 3. Length is not identity; the stamp
    // is. A converge on a character the stamp does not name stands down.
    [[nodiscard]] std::optional<ProfileCodec::CharacterBlock> Who();

    // Replace the record wholesale, from the save. An absent stamp is a
    // record from a build that predates it and converges unguarded, exactly
    // as it did before the stamp existed.
    void Restore(const std::vector<ProfileCodec::MakeupEntry>&        a_entries,
                 const std::optional<ProfileCodec::CharacterBlock>&   a_who);

    // A load boundary: this record belongs to the outgoing character, and
    // after this there is NO claim until the incoming save restores one.
    void Clear();

    // A load boundary crossed: the next TWO checks push the record even when
    // the list agrees, for the reason the overlay baseline's NoteLoadBoundary
    // gives: the first converge runs inside the load's repaint storm and the
    // second lands on the far side of it.
    void NoteLoadBoundary();

    // Put the recorded list back when the live one disagrees with it, and say
    // which it was. The write goes through MakeupApi::Write, so it ends in the
    // one retint that rebuilds the face from the corrected list; the r35
    // wrong-race refusal stands here too.
    std::size_t ReassertRecord(RE::Actor* a_actor);

}  // namespace OS::MakeupBaseline
