#pragma once

#include "ProfileCodec.h"  // OverlaysBlock, the shape a look already stores

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE {
    class Actor;
}

// The player's overlay ART, held by Fitting Room so that skee's store is not
// the only copy of it.
//
// ⚠⚠ THE FAULT THIS EXISTS FOR, FIELD 2026-08-25. The user imported Umbrael in
// one session, saved, and loaded that save in the next: "she has the original
// nord skin color and lost her overlays, this is quite bad." The load round
// measured the overlay half exactly:
//
//   AppearanceWatch BASELINE after 'post-load': ... layers=0 (0 with art)
//   OverlayCensus: 3p on 'Prisoner': body 7, hands 0, feet 0, face 3.
//   OverlayCensus:   3p every clone wears the default, path and bound both.
//
// The clones came back and the ART did not, which is the split OverlayReconcile
// already names: "no clones on the 1p root is the install half, clones wearing
// the default diffuse are the apply half". Applying the look by hand put all of
// it back (`skee overlay layers 0 (0 with art) -> 8 (5 with art)`), so nothing
// is broken about writing them. Nothing re-writes them on a load.
//
// ⚠ WHY THE STORE COMES BACK EMPTY IS NOT SETTLED, and this deliberately does
// not depend on the answer. It could be the r42 wipe (LoadCharacterPresetEx
// replaces the whole overlay store with the preset's, and a bare face preset
// carries none) landing before the save was written, or skee's own
// serialization. Neither log covers the moment the save was made, so both are
// live. What is not in doubt is the principle: Fitting Room writes this art
// through somebody else's API and then trusts that API's store to still hold it
// a session later, which is an-api-you-write-to-is-not-a-source-of-truth in one
// sentence. Keeping our own copy is right under either cause.
//
// ⚠⚠ AND THE RECORD IS THE SAVE'S TRUTH, MEASURED 2026-09-01 21:03. The first
// reassert fired only into a vacuum, on the theory that art in the store meant
// somebody meant it to be there. The field found the case that theory misses:
// apply Umbrael (skee's own preset apply seeds ITS store), apply Almalexia on
// top (our writes repaint and our record follows), save, load. Our cosave came
// back holding Almalexia's 5; skee's came back holding Umbrael's set and landed
// LATE, after our restore had already run against an empty store, and the first
// body rebuild painted Umbrael over the face eighteen seconds into the load.
// Art in the store is not proof anybody CURRENTLY means it: skee's half can be
// stale by a whole look. So the reassert now compares CONTENT, and a store that
// disagrees with the record is rewritten to it.
//
// ⚠⚠ WHAT KEEPS THAT FROM EATING RACEMENU WORK: the record follows every
// author. Fitting Room's writes feed it at the Write/Clear choke points, and a
// RaceSexMenu visit re-syncs it from skee's store on the close edge
// (HeadEditorSink), so art the player painted in RaceMenu is IN the record
// before any save can carry it. A hold must be refreshed by every writer of
// what it holds; with that in place, disagreement at a load boundary can only
// mean one of the two serialized halves lost writes, and ours is the one fed at
// the choke points.
namespace OS::OverlayBaseline {

    // Record one layer the way it was just written. Called from the single
    // choke point every Fitting Room overlay write already goes through, so the
    // record cannot drift from what was actually painted. Ignored for anyone
    // but the player: followers do not survive a load as themselves here.
    void Note(RE::Actor* a_actor, const std::string& a_node,
              const OverlayPlan::LayerState& a_state);

    // Take one layer back out of the record, from the same choke point that put
    // it in. A mark and its eraser have to ask one question.
    //
    // ⚠⚠ THE FAULT THIS EXISTS FOR, FIELD 2026-08-27. A look import left the
    // OUTGOING character's face overlays on the imported face: a male Nord
    // wearing Umbrael's '(SDZ21) Fabulous Makeup 2' and Almalexia's '64 Head
    // Alt F', which is the report that arrived as "the previous makeup stays
    // on". Measured three times in one round, always the same 196 ms:
    //
    //   02:28:10.733 ProfileApply: replace-on-apply took off ... (6 overlay layer(s), the shape)
    //   02:28:10.928 OverlayBaseline: skee held no art after this load, so 9 recorded layer(s) went back on
    //
    // Every clear in the mod goes through OverlayApi::Clear, which did not pass
    // this way, so the record still named art nobody was painting and the next
    // empty store put it straight back. ⚠ It is not only an import: a player
    // emptying a slot on the Overlays page had the same layer returned to them
    // by the next load.
    void Forget(RE::Actor* a_actor, const std::string& a_node);

    // The record's own bookkeeping, pure so a test can hold it. Drops the entry
    // at a location and index, and says whether one was there.
    [[nodiscard]] inline bool ForgetIn(ProfileCodec::OverlaysBlock& a_block,
                                       OverlayPlan::Location        a_location,
                                       std::uint32_t                a_index) {
        auto& entries = a_block.byLocation[OverlayPlan::Slot(a_location)];
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->index == a_index) {
                entries.erase(it);
                return true;
            }
        }
        return false;
    }

    // Whether a record holds anything at all. Pure, and the only definition of
    // it: HasAny's flag is kept from this so a forget cannot leave the flag
    // saying yes over an empty block.
    [[nodiscard]] inline bool AnyIn(const ProfileCodec::OverlaysBlock& a_block) {
        for (const auto& entries : a_block.byLocation) {
            if (!entries.empty()) {
                return true;
            }
        }
        return false;
    }

    // Everything recorded so far, in the block shape a look already uses so the
    // co-save can reuse ProfileCodec's JSON rather than a second copy of
    // LayerState's field list that would drift the first time a field is added.
    [[nodiscard]] ProfileCodec::OverlaysBlock Snapshot();

    // Whether anything at all has been recorded. An empty baseline is "this
    // character has never had an overlay written by us", which is a normal
    // state and not a failure.
    [[nodiscard]] bool HasAny();

    // Replace the record wholesale, from the save.
    void Restore(const ProfileCodec::OverlaysBlock& a_block);

    // A load boundary: this record belongs to the outgoing character. ⚠ The key
    // here is the player, and the player's form ID is 0x14 in every save, so
    // nothing distinguishes character A's overlays from character B's except
    // clearing them at the boundary. That is head-part baseline's scar
    // (HeadPart.h) and it applies here word for word.
    void Clear();

    // ---- the load-boundary judgement, pure so a test can hold it ------------

    // The record's entry at a location and index, or null. Any entry counts as
    // a claim, an unoccupied one included: a recorded empty state means "this
    // layer is meant to be bare", which the write path expresses by writing it.
    [[nodiscard]] inline const OverlayPlan::LayerState* FindIn(
        const ProfileCodec::OverlaysBlock& a_block, OverlayPlan::Location a_location,
        std::uint32_t a_index) {
        const auto& entries = a_block.byLocation[OverlayPlan::Slot(a_location)];
        for (const auto& entry : entries) {
            if (entry.index == a_index) {
                return &entry.state;
            }
        }
        return nullptr;
    }

    // One art identity, folded the way the field spells it: case and slash
    // fold, and a 'textures\' prefix is forgiven on either side, because
    // skee's store and a cosave roundtrip write the same file both ways
    // (OverlayReconcile's set-versus-bound note is the same lesson).
    [[nodiscard]] inline bool SameOverlayArt(std::string_view a_lhs,
                                             std::string_view a_rhs) {
        const auto strip = [](std::string_view a_path) {
            constexpr std::string_view kPrefix{ "textures" };
            if (a_path.size() > kPrefix.size() &&
                (a_path[kPrefix.size()] == '\\' || a_path[kPrefix.size()] == '/')) {
                bool match = true;
                for (std::size_t i = 0; i < kPrefix.size(); ++i) {
                    char c = a_path[i];
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                    if (c != kPrefix[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return a_path.substr(kPrefix.size() + 1);
                }
            }
            return a_path;
        };
        const auto lhs = strip(a_lhs);
        const auto rhs = strip(a_rhs);
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            char x = lhs[i];
            char y = rhs[i];
            if (x == '/') x = '\\';
            if (y == '/') y = '\\';
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    // ⚠⚠ OCCUPIED, WITH THE DEFAULT SPELLED ANY WAY. OverlayPlan::Occupied
    // tests the exact default path, and the field found the variant that
    // slips it (2026-09-01 22:00): a look captured the default texture with a
    // 'textures\' prefix, the record read that entry as ART on a layer whose
    // live reading is bare, and the judge convicted the same "divergence" on
    // every check forever, three re-asserts in one minute. A claim of the
    // default is a claim of bareness however it is spelled.
    [[nodiscard]] inline bool OccupiedLoose(const OverlayPlan::LayerState& a_state) {
        return OverlayPlan::Occupied(a_state) &&
               !SameOverlayArt(a_state.texture, OverlayPlan::kDefaultTexture);
    }

    enum class StoreVerdict : std::uint8_t {
        kVacuum,   // skee holds no art at all: the original measured fault
        kAgrees,   // every layer's art matches the record: nothing to do
        kDiverged  // skee holds a different set than the record: the record wins
    };

    // One worn layer against the record's claim of it. ⚠ EVERY DETERMINISTIC
    // CHANNEL CONVICTS, not the art alone. The first judge compared art
    // identity only, on a churn theory about colour wobble, and the field
    // answered the same night (2026-09-01 21:41): Almalexia's skeleton face
    // art survived a load and its BLACK TINT did not, so a layer meant as
    // dark jaw shading drew as a pale skull, and the art-only judge called
    // that agreement. Both sides here are stored bytes, not measurements, so
    // there is no wobble to churn on; the float channels get an epsilon for
    // the codec roundtrip and nothing more.
    // Why one worn layer disagrees with the record's claim of it, or null for
    // agreement. The tag is a log word, not an enum: it exists so a field
    // round can say WHICH channel diverged instead of guessing, which is how
    // the 21:41 tint loss hid behind the 21:03 art story for a whole evening.
    [[nodiscard]] inline const char* LayerDisagreement(
        const OverlayPlan::LayerState& a_rec, const OverlayPlan::LayerState& a_live) {
        const auto near = [](float a_lhs, float a_rhs) {
            const float d = a_lhs - a_rhs;
            return (d < 0.0f ? -d : d) < 0.01f;
        };
        if (!SameOverlayArt(a_rec.texture, a_live.texture)) {
            return "art";
        }
        if (a_rec.hasTint != a_live.hasTint ||
            (a_rec.hasTint && !(a_rec.tint == a_live.tint))) {
            return "tint";
        }
        if (a_rec.hasAlpha != a_live.hasAlpha ||
            (a_rec.hasAlpha && !near(a_rec.alpha, a_live.alpha))) {
            return "alpha";
        }
        if (!(a_rec.glow == a_live.glow) ||
            !near(a_rec.glowStrength, a_live.glowStrength)) {
            return "glow";
        }
        return nullptr;
    }

    // Judge skee's store against the record. a_live is every installed layer
    // beside what skee holds for it, read once by the caller. When
    // a_disagreements is given, every convicting layer lands in it with its
    // channel tag, so the verdict and the log cannot read different stores.
    [[nodiscard]] inline StoreVerdict JudgeStore(
        const ProfileCodec::OverlaysBlock& a_block,
        const std::vector<std::pair<OverlayPlan::Layer, OverlayPlan::LayerState>>&
            a_live,
        std::vector<std::pair<OverlayPlan::Layer, const char*>>* a_disagreements =
            nullptr) {
        bool anyLiveArt = false;
        bool diverged   = false;
        const auto flag = [&](const OverlayPlan::Layer& a_layer, const char* a_why) {
            diverged = true;
            if (a_disagreements) {
                a_disagreements->emplace_back(a_layer, a_why);
            }
        };
        for (const auto& [layer, state] : a_live) {
            const bool liveOcc = OccupiedLoose(state);
            anyLiveArt         = anyLiveArt || liveOcc;
            const auto* const rec    = FindIn(a_block, layer.location, layer.index);
            const bool        recOcc = rec && OccupiedLoose(*rec);
            if (recOcc != liveOcc) {
                flag(layer, recOcc ? "worn in the record, bare on skee"
                                   : "bare in the record, worn on skee");
            } else if (recOcc) {
                if (const char* why = LayerDisagreement(*rec, state)) {
                    flag(layer, why);
                }
            }
        }
        if (!anyLiveArt) {
            return StoreVerdict::kVacuum;
        }
        return diverged ? StoreVerdict::kDiverged : StoreVerdict::kAgrees;
    }

    // Re-sync the record from skee's store, wholesale. The close edge of a
    // RaceSexMenu visit calls this so the record follows RaceMenu's writes the
    // way the choke points make it follow ours; without it, the load-boundary
    // reassert above would be the second painter the header warns about.
    void SyncFromStore(RE::Actor* a_actor);

    // A load boundary crossed: the next TWO agreeing checks still push the
    // record. ⚠ Field 2026-09-01 21:41: the drawn clones lost a tint the STORE
    // still agreed on, because nothing on that load's path ever pushed the
    // stored appearance back onto the rebuilt 3D (the node push arms off a
    // skin repaint, and no repaint ran). ⚠⚠ And 23:45 measured why once is not
    // enough: the single converge ran half a second into the load and skee's
    // cosave replay repainted after it, so the second armed check, seconds
    // later on the far side of the storm, carries the push that sticks. Both
    // end in the coalescing node-property arm, so the materials are asked for
    // again once the repaints stop. Two and no more, because a write per
    // face-install check is churn the editor sessions would pay all day.
    void NoteLoadBoundary();

    // Put the recorded art back on the player when the store disagrees with it
    // or holds nothing, and say which it was; on the first check after a load
    // boundary the record is pushed even on agreement (see NoteLoadBoundary).
    // Returns the number of layers written; the log line says which case ran.
    std::size_t ReassertRecord(RE::Actor* a_actor);

}  // namespace OS::OverlayBaseline
