#pragma once

#include "Outfit.h"

#include <array>
#include <cstdint>

// Which editor row owns which biped slot, once a style's FULL coverage is taken
// into account rather than only the row it was assigned to.
//
// Coverage is (ARMO slot mask) union (every ARMA's slot mask) - see
// StyleCoverageOf. It is DERIVED at run time and never persisted: it depends on
// the live armature list, which a mod update can change, so saving it would
// freeze a stale answer into every outfit file.
namespace OS {

    struct SlotClaims {
        static constexpr std::uint32_t kNoOwner = 0xFFFFFFFFu;

        std::array<std::uint32_t, Outfit::kBitCount> owner{};
        // Rows that hold a style but lost their own slot to a lower row. Only
        // reachable from outfits written before coverage existed, or where a mod
        // widened an armature since; assignment prevents it going forward.
        std::uint32_t displaced{ 0 };

        [[nodiscard]] bool IsFree(std::uint32_t a_bit) const {
            return a_bit < Outfit::kBitCount && owner[a_bit] == kNoOwner;
        }
        [[nodiscard]] bool IsOwner(std::uint32_t a_bit) const {
            return a_bit < Outfit::kBitCount && owner[a_bit] == a_bit;
        }
        [[nodiscard]] bool IsCovered(std::uint32_t a_bit) const {
            return a_bit < Outfit::kBitCount && owner[a_bit] != kNoOwner &&
                   owner[a_bit] != a_bit;
        }
        [[nodiscard]] std::uint32_t Displaced() const { return displaced; }
    };

    // a_coverage[b] is the coverage of the style assigned at bit b, or 0 where
    // no style is assigned there. Resolves SLOT-ASCENDING, so a contested slot
    // goes to the LOWEST owning row - deterministic and stable across sessions.
    // This is the load-time REPAIR rule and is deliberately NOT the same as the
    // assignment-time conflict rule (which is newest-wins, added in task 2).
    [[nodiscard]] inline SlotClaims ComputeSlotClaims(
        std::uint32_t a_styleMask,
        const std::array<std::uint32_t, Outfit::kBitCount>& a_coverage) {
        SlotClaims claims;
        claims.owner.fill(SlotClaims::kNoOwner);

        for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
            if (((a_styleMask >> b) & 1u) == 0) {
                continue;
            }
            // A style with no resolvable coverage still owns its own row, so a
            // style whose plugin vanished does not silently free the slot.
            const std::uint32_t coverage = a_coverage[b] ? a_coverage[b] : (1u << b);
            for (std::uint32_t c = 0; c < Outfit::kBitCount; ++c) {
                if (((coverage >> c) & 1u) && claims.owner[c] == SlotClaims::kNoOwner) {
                    claims.owner[c] = b;
                }
            }
            // "< b", not "!= b". Displaced means a LOWER row took this row, and
            // that is the only case the editor can explain to the user. The two
            // spellings differ if coverage ever excludes a style's own bit: the
            // row would then be unclaimed rather than taken, which is orphaned,
            // not displaced. Coverage cannot do that today - it is an ARMO mask
            // unioned with its armatures' masks, and the fallback below is the
            // row's own bit - but naming the real condition costs nothing and
            // stops a future coverage source from quietly changing the meaning.
            if (claims.owner[b] < b) {
                claims.displaced |= (1u << b);
            }
        }
        return claims;
    }

    // Assign a_key at a_bit and clear true editor-row ownership conflicts.
    //
    // THE CONFLICT RULE: newest wins when either piece covers the OTHER piece's
    // assigned editor row. A mere intersection on a third, auxiliary slot is
    // not a conflict: cuirasses and gauntlets routinely both cover Forearms
    // (34), yet Body (32) and Hands (33) are independent pieces that must coexist.
    // Treating any coverage intersection as eviction made choosing Hands erase
    // Body. Hidden rows are cleared when the new piece directly covers that row:
    // hiding one slot of a piece that renders across several is not a state the
    // engine can express.
    //
    // This is NOT ComputeSlotClaims' lowest-row-wins rule. That one repairs an
    // overlap already sitting in a saved outfit, where there is no "newest" to
    // prefer. Both are needed and they are deliberately different.
    //
    // a_existingCoverage[b] is the coverage of the style currently at bit b, 0
    // where none. Returns the bits cleared, for the footer to report and for
    // the caller's undo entry.
    inline std::uint32_t AssignStyleWithCoverage(
        Outfit& a_outfit, std::uint32_t a_bit, StyleRefKey a_key,
        std::uint32_t a_newCoverage,
        const std::array<std::uint32_t, Outfit::kBitCount>& a_existingCoverage) {
        if (a_bit >= Outfit::kBitCount) {
            return 0;
        }
        const std::uint32_t coverage = a_newCoverage ? a_newCoverage : (1u << a_bit);

        std::uint32_t cleared = 0;
        for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
            if (b == a_bit) {
                continue;  // replacing a row is not a collision with itself
            }
            const auto& entry = a_outfit.EntryFor(b);
            if (entry.kind == SlotEntry::Kind::kStyle) {
                const std::uint32_t other =
                    a_existingCoverage[b] ? a_existingCoverage[b] : (1u << b);
                const bool newOwnsOtherRow = ((coverage >> b) & 1u) != 0;
                const bool otherOwnsNewRow = ((other >> a_bit) & 1u) != 0;
                if (newOwnsOtherRow || otherOwnsNewRow) {
                    // ClearSlot, not SetPassthrough: the evicted row's piece is
                    // gone, so its colours are too. Leaving them would be worse
                    // here than an orphan stripe - the winner's armature covers
                    // this bit as well, so the biped's part clone at b is the
                    // NEW garment, and the loser's dye would paint it.
                    ClearSlot(a_outfit, b);
                    cleared |= (1u << b);
                }
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                if (((coverage >> b) & 1u) != 0) {
                    ClearSlot(a_outfit, b);
                    cleared |= (1u << b);
                }
            }
        }
        a_outfit.SetStyle(a_bit, std::move(a_key));
        return cleared;
    }

    // Grow a hide mask to the FULL coverage of whatever occupies each hidden
    // slot. Half a helmet is not something the engine renders, and culling one
    // slot's partClone while a sibling slot still references the same node is
    // the undefined state that produced the headless-character reports.
    //
    // a_coverage[b] is the coverage of the piece occupying bit b: a style from
    // the outfit, or the REAL worn ARMO when the caller is the render pass. A
    // slot with no known coverage expands to itself, so a hide is never lost.
    //
    // This CANNOT be a direct a_coverage[hiddenBit] lookup. Every other
    // function in this file is handed a second signal that says which rows
    // are the "owning" ones - ComputeSlotClaims gets a_styleMask, Assign-
    // StyleWithCoverage checks EntryFor(b).kind == kStyle - and only reads
    // a_coverage at those rows, leaving every slot a piece merely covers at 0
    // (see ComputeSlotClaims' own helmet-spanning-30/31/42 test: cov[1] and
    // cov[12] are never set even though row 0's style covers them). This
    // function gets no such signal, so a hidden row that is a covered slot
    // rather than the owning one - row 1 of a helm assigned at row 0 - has
    // a_coverage[1] == 0 while the real mask sits at a_coverage[0]. The fix
    // is to search every row for whichever one's mask actually claims the
    // hidden slot, not to push a "populate every occupied slot" burden onto
    // callers that nothing enforces.
    [[nodiscard]] inline std::uint32_t ExpandHideOverCoverage(
        std::uint32_t a_hideMask,
        const std::array<std::uint32_t, Outfit::kBitCount>& a_coverage) {
        std::uint32_t expanded = 0;
        for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
            if (((a_hideMask >> b) & 1u) == 0) {
                continue;
            }
            // Union every row that claims slot b rather than taking the first:
            // a legacy overlap (see the LEGACY REPAIR test) can leave two rows
            // both claiming it, and hiding too much is the safe direction.
            std::uint32_t claim = 0;
            for (std::uint32_t r = 0; r < Outfit::kBitCount; ++r) {
                if ((a_coverage[r] >> b) & 1u) {
                    claim |= a_coverage[r];
                }
            }
            expanded |= claim ? claim : (1u << b);
        }
        return expanded;
    }

}  // namespace OS
