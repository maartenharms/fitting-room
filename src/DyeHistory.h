#pragma once

#include "DyePalette.h"  // Dye, and DyeChannel / Outfit through Outfit.h
#include "Outfit.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace OS {

    // One colour the player mixed themselves. Three bytes, because a hand
    // picked colour has no dye id by definition and an unset channel is not a
    // colour.
    struct DyeHistoryEntry {
        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };
        friend bool operator==(const DyeHistoryEntry&, const DyeHistoryEntry&) = default;
    };

    // ⚠ PART OF THE WIRE FORMAT. Raising this needs a kDyeHistoryVersion bump
    // and a new golden literal. See the static_assert in DyeHistory.cpp.
    inline constexpr std::uint32_t kMaxHistoryColours = 20;

    // The colours one character picked by hand, most recent first.
    //
    // Pure, so the wire format is provable without a save.
    class DyeHistoryList {
    public:
        // Moves an existing colour to the front rather than storing it twice,
        // and evicts the oldest once the cap is reached.
        void Add(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b);

        [[nodiscard]] std::vector<DyeHistoryEntry> Entries() const;
        [[nodiscard]] std::size_t                  Size() const;
        void                                       Clear();

        // Wire: u32 count, then each entry as three bytes.
        [[nodiscard]] std::vector<std::byte> Encode() const;
        // ⚠ ALL OR NOTHING, the same contract DyeUnlockSet::Decode has. On
        // false this object is untouched, so a refused record cannot leave a
        // character holding half of somebody else's history.
        [[nodiscard]] bool Decode(std::span<const std::byte> a_bytes,
                                  std::uint32_t              a_version);

    private:
        std::vector<DyeHistoryEntry> entries_;  // front is most recent
    };

    inline constexpr std::uint32_t kDyeHistoryVersion = 1;

    // Whether a record of version a_record may be read by a build whose current
    // version is a_current.
    //
    // ⚠ SPLIT OUT OF Decode FOR THE REASON DyeVersionLoadable IS. With the
    // current version at 1 there is no way to construct "a v1 record under a v2
    // build" through Decode, so a runtime test cannot tell a range from an
    // equality. The static_asserts below can, and they fail at COMPILE time
    // against an equality implementation.
    [[nodiscard]] constexpr bool DyeHistoryVersionLoadable(std::uint32_t a_record,
                                                           std::uint32_t a_current) {
        return a_record != 0 && a_record <= a_current;
    }

    static_assert(DyeHistoryVersionLoadable(1, 2),
                  "a v1 record must still load under a v2 build, or bumping the "
                  "version silently destroys every player's history");
    static_assert(DyeHistoryVersionLoadable(1, 1));
    static_assert(!DyeHistoryVersionLoadable(2, 1),
                  "a record from a newer build is refused");
    static_assert(!DyeHistoryVersionLoadable(0, 1), "0 was never written");

    // ⚠ THE ONE DEFINITION OF "HAND PICKED", and it has two callers: the
    // picker, which captures the moment an edit finishes, and the commit, which
    // catches anything that arrived by another route. They must not be able to
    // disagree, so neither one owns the rule.
    //
    // True when no installed dye carries this exact RGB. A colour the palette
    // can still reach is not hand picked, whatever widget produced it.
    [[nodiscard]] bool IsHandPicked(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b,
                                    std::span<const Dye> a_palette);

    // Every colour this commit would newly paint that no installed dye can
    // reach. "Hand picked" is defined as not equal to any installed dye's RGB,
    // which is a recoverability test rather than a provenance flag, so
    // DyeChannel needs no new field and a player switching between the palette
    // and the picker mid-edit cannot confuse it.
    //
    // ⚠ CHANGED CHANNELS ONLY, using the same diff ChangedDyeChannelCount uses.
    // Feeding every committed channel in would re-promote every colour the
    // outfit already wore on each apply and the recency order would stop
    // meaning anything within a session or two.
    //
    // Returned oldest first, so a caller can feed them straight into Add.
    [[nodiscard]] std::vector<DyeHistoryEntry> ChangedHandPickedColours(
        const Outfit& a_base, const Outfit& a_staged,
        std::span<const Dye> a_palette);

    // The live list for the current character, behind a lock.
    //
    // ⚠ NO RAW REFERENCE ESCAPES, for the reason written out in DyeUnlocks.h:
    // the uniform contract in this codebase is a lock plus a Snapshot() copy for
    // the render thread.
    namespace DyeHistory {

        // MAIN THREAD ONLY: the co-save callbacks and the capture at commit.
        //
        // ⚠ NOT REENTRANT. g_lock is a plain mutex, so reaching With or Snapshot
        // from inside a_fn deadlocks the calling thread with no crash and no log
        // line. That includes reaching them indirectly.
        void With(const std::function<void(DyeHistoryList&)>& a_fn);

        // Any thread. A consistent copy, the contract DyePalette::Snapshot has.
        // Take it once per editor open, not once per swatch per frame.
        [[nodiscard]] DyeHistoryList Snapshot();

    }  // namespace DyeHistory

}  // namespace OS
