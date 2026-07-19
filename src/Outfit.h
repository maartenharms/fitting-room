#pragma once

#include "SlotMask.h"
#include "WeaponSlots.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OS {

    inline constexpr std::size_t kMaxOutfits = 6;  // user cap (was 10)
    inline constexpr std::uint32_t kBitCount = 32;
    static_assert(kBitCount <= 32, "masks are uint32_t");

    // A style piece, stored load-order-independently. Resolved to a
    // TESObjectARMO* only inside StyleRef.cpp (engine code).
    struct StyleRefKey {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool Empty() const { return modName.empty() && localFormID == 0; }
        friend bool operator==(const StyleRefKey&, const StyleRefKey&) = default;
    };

    struct SlotEntry {
        enum class Kind : std::uint8_t { kPassthrough = 0, kStyle = 1, kHide = 2 };
        Kind        kind{ Kind::kPassthrough };
        StyleRefKey style;
    };

    class Outfit {
    public:
        std::string name;
        bool        favorite{ false };

        void SetStyle(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetHide(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kHide, {} };
        }
        void SetPassthrough(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = {};
        }

        [[nodiscard]] const SlotEntry& EntryFor(std::uint32_t a_bit) const {
            static const SlotEntry kNone{};
            return a_bit < kBitCount ? entries_[a_bit] : kNone;
        }

        [[nodiscard]] std::uint32_t StyleMask() const { return MaskOf(SlotEntry::Kind::kStyle); }
        [[nodiscard]] std::uint32_t HideMask() const { return MaskOf(SlotEntry::Kind::kHide); }

        template <class F>
        void ForEachStyle(F&& a_fn) const {
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (entries_[b].kind == SlotEntry::Kind::kStyle) {
                    a_fn(b, entries_[b].style);
                }
            }
        }

        // Weapon dimension (weapon + quiver transmog, stage 1). Parallel to
        // the armor entries_ above - same SlotEntry kind, a separate array
        // indexed by WeaponClass instead of an editor-slot bit (see
        // WeaponSlots.h for the class<->BipedAnim-slot mapping). Additive:
        // an outfit that never touches weapons leaves weaponEntries_
        // all-passthrough, so ChangedSlotCount/EditHistory below are unaffected.
        void SetWeaponStyle(WeaponClass a_class, StyleRefKey a_key) {
            weaponEntries_[Idx(a_class)] = { SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetWeaponHide(WeaponClass a_class) {
            weaponEntries_[Idx(a_class)] = { SlotEntry::Kind::kHide, {} };
        }
        void SetWeaponPassthrough(WeaponClass a_class) { weaponEntries_[Idx(a_class)] = {}; }

        [[nodiscard]] const SlotEntry& WeaponEntryFor(WeaponClass a_class) const {
            return weaponEntries_[Idx(a_class)];
        }

        template <class F>
        void ForEachWeaponStyle(F&& a_fn) const {
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                if (weaponEntries_[i].kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), weaponEntries_[i].style);
                }
            }
        }

    private:
        [[nodiscard]] std::uint32_t MaskOf(SlotEntry::Kind a_kind) const {
            std::uint32_t m = 0;
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (entries_[b].kind == a_kind) {
                    m |= (1u << b);
                }
            }
            return m;
        }

        static constexpr std::size_t Idx(WeaponClass a_class) {
            return static_cast<std::size_t>(a_class);
        }

        std::array<SlotEntry, kBitCount>         entries_{};
        std::array<SlotEntry, kWeaponClassCount> weaponEntries_{};
    };

    // What the render override must do this pass. Pure function of the outfit
    // and the user's blocklist - the unit-test surface.
    struct DisplaySet {
        std::uint32_t styleMask{ 0 };
        std::uint32_t hideMask{ 0 };
        std::uint32_t hiddenBodySkinMask{ 0 };    // re-apply race skin ARMA here
        std::uint32_t hiddenAttachmentMask{ 0 };  // cull objects[slot].partClone here
        std::uint32_t hiddenHeadPartMask{ 0 };    // AND-NOT out of the worn mask
    };

    [[nodiscard]] inline DisplaySet ComputeDisplaySet(const Outfit& a_outfit,
                                                      std::uint32_t a_blocklist) {
        const std::uint32_t forbidden = a_blocklist | kNeverTouchMask;
        DisplaySet d;
        d.styleMask = a_outfit.StyleMask() & ~forbidden;
        d.hideMask  = a_outfit.HideMask() & ~forbidden;

        d.hiddenBodySkinMask   = d.hideMask & kBodySkinMask;
        d.hiddenAttachmentMask = d.hideMask & ~kBodySkinMask;
        d.hiddenHeadPartMask   = d.hideMask & kHeadPartMask;
        return d;
    }

    // True when the outfit styles or hides ANY weapon class - hiding counts.
    // The predicate behind OutfitSession's weapon fast-path flag; it lives here,
    // pure, so the part deciding whether the feature runs at all is unit-tested.
    [[nodiscard]] inline bool AnyWeaponEntry(const Outfit& a_outfit) {
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            if (a_outfit.WeaponEntryFor(static_cast<WeaponClass>(i)).kind !=
                SlotEntry::Kind::kPassthrough) {
                return true;
            }
        }
        return false;
    }

    // True when the two slot entries render differently (drives the Apply
    // cost: a slot that returns to its baseline value costs nothing).
    [[nodiscard]] inline bool SlotDiffers(const SlotEntry& a, const SlotEntry& b) {
        return a.kind != b.kind ||
               (a.kind == SlotEntry::Kind::kStyle && !(a.style == b.style));
    }

    // How many slots the staged outfit changes vs. the committed baseline -
    // armor bits then weapon classes, same SlotDiffers predicate for both.
    // The lore-mode Apply charges per changed slot, so this must return to 0
    // whenever the staged outfit is edited back to its baseline.
    [[nodiscard]] inline std::uint32_t ChangedSlotCount(const Outfit& a_base,
                                                        const Outfit& a_staged) {
        std::uint32_t n = 0;
        for (std::uint32_t b = 0; b < kBitCount; ++b) {
            if (SlotDiffers(a_base.EntryFor(b), a_staged.EntryFor(b))) {
                ++n;
            }
        }
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto wc = static_cast<WeaponClass>(c);
            if (SlotDiffers(a_base.WeaponEntryFor(wc), a_staged.WeaponEntryFor(wc))) {
                ++n;
            }
        }
        return n;
    }

    // Reversible hide toggle for the editor's per-slot Hide/Show button.
    // Hiding stashes the slot's prior entry; showing restores it - so a
    // Hide-then-Show round trip lands back on the ORIGINAL entry (a style,
    // real gear, whatever), not unconditionally on passthrough. Without this
    // the round trip destroys a styled slot and leaves a permanent phantom
    // change on the Apply cost. a_stash is editor-session state, one entry
    // per bit, seeded empty at editor open.
    inline void ToggleHideSlot(Outfit& a_staged, std::array<SlotEntry, kBitCount>& a_stash,
                               std::uint32_t a_bit) {
        if (a_bit >= kBitCount) {
            return;
        }
        if (a_staged.EntryFor(a_bit).kind == SlotEntry::Kind::kHide) {
            const SlotEntry prev = a_stash[a_bit];
            if (prev.kind == SlotEntry::Kind::kStyle) {
                a_staged.SetStyle(a_bit, prev.style);
            } else {
                a_staged.SetPassthrough(a_bit);
            }
        } else {
            a_stash[a_bit] = a_staged.EntryFor(a_bit);
            a_staged.SetHide(a_bit);
        }
    }

    // Bounded undo/redo timeline for the editor's staged outfit (OS-21). A
    // linear list of snapshots with a cursor: Reset seeds the state the editor
    // opened on, Record appends the state reached by each REAL edit (style
    // pick, Remove/Show, real-gear, Random), and Undo/Redo walk the cursor.
    // Transient hover-preview is deliberately never recorded - only committed
    // edits reach Record. Slot-only comparison (names are edited on a separate
    // path and must not create history), so re-picking the selected style is a
    // no-op. Pure logic - unit-tested like ToggleHideSlot/ChangedSlotCount.
    class EditHistory {
    public:
        static constexpr std::size_t kCap = 20;  // bounded so a long session can't grow unbounded

        void Reset(const Outfit& a_initial) {
            states_.assign(1, a_initial);
            cursor_ = 0;
        }

        void Record(const Outfit& a_next) {
            if (states_.empty()) {  // never Reset: treat this as the seed
                states_.push_back(a_next);
                cursor_ = 0;
                return;
            }
            if (!SlotsDiffer(states_[cursor_], a_next)) {
                return;  // idempotent edit (e.g. re-picking the selected style)
            }
            states_.resize(cursor_ + 1);  // a new edit drops any redo tail
            states_.push_back(a_next);
            cursor_ = states_.size() - 1;
            if (states_.size() > kCap) {
                const std::size_t drop = states_.size() - kCap;
                states_.erase(states_.begin(),
                              states_.begin() + static_cast<std::ptrdiff_t>(drop));
                cursor_ -= drop;
            }
        }

        [[nodiscard]] bool CanUndo() const { return cursor_ > 0; }
        [[nodiscard]] bool CanRedo() const { return cursor_ + 1 < states_.size(); }

        // Walk the cursor and return the now-current snapshot. A no-op (returns
        // the current snapshot unchanged) when there is nothing to undo/redo.
        const Outfit& Undo() {
            if (CanUndo()) {
                --cursor_;
            }
            return Current();
        }
        const Outfit& Redo() {
            if (CanRedo()) {
                ++cursor_;
            }
            return Current();
        }

        [[nodiscard]] const Outfit& Current() const {
            static const Outfit kEmpty;
            return states_.empty() ? kEmpty : states_[cursor_];
        }
        [[nodiscard]] std::size_t Size() const { return states_.size(); }

    private:
        // Two outfits differ as edits iff any slot (armor bit or weapon
        // class) renders differently - structurally the same predicate
        // ChangedSlotCount uses, so history and Apply-cost agree.
        static bool SlotsDiffer(const Outfit& a, const Outfit& b) {
            return ChangedSlotCount(a, b) != 0;
        }

        std::vector<Outfit> states_;
        std::size_t         cursor_{ 0 };
    };

    class OutfitLibrary {
    public:
        [[nodiscard]] std::size_t Count() const { return outfits_.size(); }
        [[nodiscard]] int         ActiveIndex() const { return active_; }

        // Invalidated by Create()/Remove(); do not hold across calls.
        [[nodiscard]] Outfit*       At(std::size_t i) { return i < outfits_.size() ? &outfits_[i] : nullptr; }
        [[nodiscard]] const Outfit* At(std::size_t i) const { return i < outfits_.size() ? &outfits_[i] : nullptr; }

        [[nodiscard]] const Outfit* Active() const {
            return active_ >= 0 ? At(static_cast<std::size_t>(active_)) : nullptr;
        }

        // Returns the new index, or -1 if the 10-slot cap is reached.
        int Create(std::string a_name) {
            if (outfits_.size() >= kMaxOutfits) {
                return -1;
            }
            Outfit o;
            o.name = std::move(a_name);
            outfits_.push_back(std::move(o));
            return static_cast<int>(outfits_.size()) - 1;
        }

        void Rename(std::size_t i, std::string a_name) {
            if (auto* o = At(i)) {
                o->name = std::move(a_name);
            }
        }

        void Remove(std::size_t i) {
            if (i >= outfits_.size()) {
                return;
            }
            outfits_.erase(outfits_.begin() + static_cast<std::ptrdiff_t>(i));
            if (active_ == static_cast<int>(i)) {
                active_ = -1;
            } else if (active_ > static_cast<int>(i)) {
                --active_;
            }
        }

        void Activate(std::size_t i) {
            if (i < outfits_.size()) {
                active_ = static_cast<int>(i);
            }
        }
        void Deactivate() { active_ = -1; }

        // Move the outfit at a_from so it sits at index a_to; the active
        // outfit follows its entry.
        void Move(std::size_t a_from, std::size_t a_to) {
            if (a_from >= outfits_.size() || a_to >= outfits_.size() || a_from == a_to) {
                return;
            }
            auto moved = std::move(outfits_[a_from]);
            outfits_.erase(outfits_.begin() + static_cast<std::ptrdiff_t>(a_from));
            outfits_.insert(outfits_.begin() + static_cast<std::ptrdiff_t>(a_to),
                            std::move(moved));
            const int from = static_cast<int>(a_from), to = static_cast<int>(a_to);
            if (active_ == from) {
                active_ = to;
            } else if (from < to && active_ > from && active_ <= to) {
                --active_;
            } else if (to < from && active_ >= to && active_ < from) {
                ++active_;
            }
        }
        void Clear() {
            outfits_.clear();
            active_ = -1;
        }

        [[nodiscard]] std::vector<Outfit>&       All() { return outfits_; }
        [[nodiscard]] const std::vector<Outfit>& All() const { return outfits_; }

    private:
        std::vector<Outfit> outfits_;
        int                 active_{ -1 };
    };

}  // namespace OS
