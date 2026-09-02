#include "DyeHistory.h"

// For kMaxRecordBytes only, to bind the cap below to the record size limit at
// compile time. Persistence.h is include-free and declaration-only, so this
// module stays as pure as its header claims.
#include "Persistence.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace OS {

    // ⚠ IF THIS EVER FAILS, LOWER THE CAP. Do NOT raise kMaxRecordBytes, which
    // guards the outfit library, the appearance collection, the NPC assignments
    // and the two baselines at the same time.
    static_assert(4ull + std::uint64_t{ kMaxHistoryColours } * 3ull <=
                      Persistence::kMaxRecordBytes,
                  "the encoded history must fit the record size limit");

    void DyeHistoryList::Add(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b) {
        const DyeHistoryEntry e{ a_r, a_g, a_b };
        // Promote rather than duplicate: re-picking a colour should move it up
        // the list, not fill the list with one colour.
        if (const auto it = std::find(entries_.begin(), entries_.end(), e);
            it != entries_.end()) {
            entries_.erase(it);
        }
        entries_.insert(entries_.begin(), e);
        if (entries_.size() > kMaxHistoryColours) {
            entries_.resize(kMaxHistoryColours);
        }
    }

    std::vector<DyeHistoryEntry> DyeHistoryList::Entries() const { return entries_; }

    std::size_t DyeHistoryList::Size() const { return entries_.size(); }

    void DyeHistoryList::Clear() { entries_.clear(); }

    std::vector<std::byte> DyeHistoryList::Encode() const {
        std::vector<std::byte> out;
        const auto             n = static_cast<std::uint32_t>(entries_.size());
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((n >> (i * 8)) & 0xFF));
        }
        for (const auto& e : entries_) {
            out.push_back(static_cast<std::byte>(e.r));
            out.push_back(static_cast<std::byte>(e.g));
            out.push_back(static_cast<std::byte>(e.b));
        }
        return out;
    }

    bool DyeHistoryList::Decode(std::span<const std::byte> a_bytes,
                                std::uint32_t              a_version) {
        if (!DyeHistoryVersionLoadable(a_version, kDyeHistoryVersion)) {
            return false;
        }
        if (a_bytes.size() < 4) {
            return false;
        }
        std::uint32_t count = 0;
        for (int i = 0; i < 4; ++i) {
            count |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(a_bytes[i]))
                     << (i * 8);
        }
        // Bound the count BEFORE trusting it for a size calculation, so a
        // garbage u32 is an early refusal rather than an overflowed length.
        if (count > kMaxHistoryColours) {
            return false;
        }
        if (a_bytes.size() < 4ull + std::uint64_t{ count } * 3ull) {
            return false;
        }
        // ⚠ BUILT ASIDE AND SWAPPED IN, so a refusal cannot leave this object
        // holding half of somebody else's history.
        std::vector<DyeHistoryEntry> parsed;
        parsed.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = 4 + std::size_t{ i } * 3;
            parsed.push_back(
                DyeHistoryEntry{ std::to_integer<std::uint8_t>(a_bytes[at]),
                                 std::to_integer<std::uint8_t>(a_bytes[at + 1]),
                                 std::to_integer<std::uint8_t>(a_bytes[at + 2]) });
        }
        entries_ = std::move(parsed);
        return true;
    }

    bool IsHandPicked(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b,
                      std::span<const Dye> a_palette) {
        // The recoverability test. A colour still reachable from the palette is
        // not hand picked, so typing a locked swatch's hex is refused entry by
        // construction rather than by a separate rule.
        return !std::any_of(a_palette.begin(), a_palette.end(), [&](const Dye& d) {
            return d.colour.set && d.colour.r == a_r && d.colour.g == a_g &&
                   d.colour.b == a_b;
        });
    }

    std::vector<DyeHistoryEntry> ChangedHandPickedColours(
        const Outfit& a_base, const Outfit& a_staged,
        std::span<const Dye> a_palette) {
        std::vector<DyeHistoryEntry> out;
        for (std::uint32_t bit = 0; bit < Outfit::kBitCount; ++bit) {
            const auto& x = a_base.DyeFor(bit).channels;
            const auto& y = a_staged.DyeFor(bit).channels;
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                // ⚠ THE SAME PREDICATE ChangedDyeChannelCount USES. Clearing a
                // channel deliberately counts as no change here, because
                // nothing was painted.
                if (!y[c].set || (x[c].set && x[c] == y[c])) {
                    continue;
                }
                if (!IsHandPicked(y[c].r, y[c].g, y[c].b, a_palette)) {
                    continue;
                }
                const DyeHistoryEntry e{ y[c].r, y[c].g, y[c].b };
                if (std::find(out.begin(), out.end(), e) == out.end()) {
                    out.push_back(e);
                }
            }
        }
        return out;
    }

    namespace {
        std::mutex     g_lock;
        DyeHistoryList g_history;
    }

    namespace DyeHistory {

        void With(const std::function<void(DyeHistoryList&)>& a_fn) {
            std::scoped_lock lock(g_lock);
            a_fn(g_history);
        }

        DyeHistoryList Snapshot() {
            std::scoped_lock lock(g_lock);
            return g_history;
        }

    }  // namespace DyeHistory

}  // namespace OS
