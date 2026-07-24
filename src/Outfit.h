#pragma once

#include "OutfitTabs.h"
#include "SlotMask.h"
#include "WeaponSlots.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OS {

    inline constexpr std::size_t kMaxOutfits = 10;  // saved outfits per player/follower library
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

    // Per-outfit ORefit override (OBody NG). kDefault leaves OBody's own global
    // setting alone - the only value that behaves identically to a build with
    // no OBody integration at all, which is why it is the zero.
    enum class ORefitMode : std::uint8_t { kDefault = 0, kForceOn = 1, kForceOff = 2 };

    class Outfit {
    public:
        std::string name;
        bool        favorite{ false };

        // Body dimension (OBody NG integration, 2026-07-22). A third dimension
        // alongside armor bits and weapon classes, but NOT slot-shaped: it is
        // one setting for the whole outfit, not per-slot.
        //
        // Both default to "leave the body alone", so an outfit written before
        // this existed - or one made on a setup without OBody - behaves exactly
        // as it always did. That is what lets the codec read v1 records by
        // simply not filling these in.
        std::string obodyPreset;                    // empty = do not change the body
        ORefitMode  orefit{ ORefitMode::kDefault };

        void SetStyle(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetHide(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            // A hidden entry may retain the style it covers. The renderer
            // considers only kind==kHide, while keeping the key here makes
            // Show reversible after copying, saving, and reopening an outfit.
            // Calling SetHide on an already-hidden entry preserves that key.
            StyleRefKey covered;
            if (entries_[a_bit].kind == SlotEntry::Kind::kStyle ||
                entries_[a_bit].kind == SlotEntry::Kind::kHide) {
                covered = entries_[a_bit].style;
            }
            entries_[a_bit] = { SlotEntry::Kind::kHide, std::move(covered) };
        }
        // Persistence decoder seam: the armor wire format has always carried
        // mod/form fields for Hide entries. Old records contain an empty key;
        // new records use those existing fields for the covered style.
        void SetHiddenWithRestore(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kHide, std::move(a_key) };
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
        void SetWeaponStyle(WeaponClass a_class, StyleRefKey a_key,
                            WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) =
                SlotEntry{ SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetWeaponHide(WeaponClass a_class,
                           WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) =
                SlotEntry{ SlotEntry::Kind::kHide, {} };
        }
        // For Both, passthrough clears the legacy class value. For Right/Left,
        // it is an EXPLICIT override meaning "show the real weapon in this
        // hand", distinct from no override (inherit Both).
        void SetWeaponPassthrough(WeaponClass a_class,
                                  WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) = SlotEntry{};
        }

        // Editor replacement semantics. Choosing a new Both value means "use
        // this on both hands", so stale explicit Right/Left overrides must not
        // continue winning resolution. Raw setters above deliberately retain
        // additive behavior for persistence/JSON decoding.
        void SetWeaponStyleForSelection(
            WeaponClass a_class, StyleRefKey a_key, WeaponHand a_hand) {
            ClearOverridesForBothSelection(a_class, a_hand);
            SetWeaponStyle(a_class, std::move(a_key), a_hand);
        }
        void SetWeaponPassthroughForSelection(
            WeaponClass a_class, WeaponHand a_hand) {
            ClearOverridesForBothSelection(a_class, a_hand);
            SetWeaponPassthrough(a_class, a_hand);
        }

        void ClearWeaponHandOverride(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Right) {
                rightWeaponEntries_[Idx(a_class)].reset();
            } else if (a_hand == WeaponHand::Left) {
                leftWeaponEntries_[Idx(a_class)].reset();
            }
        }

        // Stored value. For Right/Left with no override this returns
        // passthrough; use WeaponOverrideFor to distinguish inheritance.
        [[nodiscard]] const SlotEntry& WeaponEntryFor(
            WeaponClass a_class, WeaponHand a_hand = WeaponHand::Both) const {
            if (a_hand == WeaponHand::Right) {
                const auto& value = rightWeaponEntries_[Idx(a_class)];
                return value ? *value : kPassthroughWeapon_;
            }
            if (a_hand == WeaponHand::Left) {
                const auto& value = leftWeaponEntries_[Idx(a_class)];
                return value ? *value : kPassthroughWeapon_;
            }
            return weaponEntries_[Idx(a_class)];
        }

        [[nodiscard]] const std::optional<SlotEntry>& WeaponOverrideFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            static const std::optional<SlotEntry> kNoOverride;
            if (a_hand == WeaponHand::Right) {
                return rightWeaponEntries_[Idx(a_class)];
            }
            if (a_hand == WeaponHand::Left) {
                return leftWeaponEntries_[Idx(a_class)];
            }
            return kNoOverride;
        }

        [[nodiscard]] const SlotEntry& ResolvedWeaponEntryFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            if (SupportsHandOverrides(a_class) && a_hand != WeaponHand::Both) {
                const auto& over = WeaponOverrideFor(a_class, a_hand);
                if (over) {
                    return *over;
                }
            }
            return WeaponEntryFor(a_class);
        }

        template <class F>
        void ForEachWeaponStyle(F&& a_fn) const {
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                if (weaponEntries_[i].kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), weaponEntries_[i].style);
                }
                if (rightWeaponEntries_[i] &&
                    rightWeaponEntries_[i]->kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), rightWeaponEntries_[i]->style);
                }
                if (leftWeaponEntries_[i] &&
                    leftWeaponEntries_[i]->kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), leftWeaponEntries_[i]->style);
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

        void ClearOverridesForBothSelection(
            WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Both) {
                rightWeaponEntries_[Idx(a_class)].reset();
                leftWeaponEntries_[Idx(a_class)].reset();
            }
        }

        SlotEntry& WeaponEntryStorage(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Right) {
                auto& value = rightWeaponEntries_[Idx(a_class)];
                if (!value) value.emplace();
                return *value;
            }
            if (a_hand == WeaponHand::Left) {
                auto& value = leftWeaponEntries_[Idx(a_class)];
                if (!value) value.emplace();
                return *value;
            }
            return weaponEntries_[Idx(a_class)];
        }

        std::array<SlotEntry, kBitCount>         entries_{};
        std::array<SlotEntry, kWeaponClassCount> weaponEntries_{};
        std::array<std::optional<SlotEntry>, kWeaponClassCount> rightWeaponEntries_{};
        std::array<std::optional<SlotEntry>, kWeaponClassCount> leftWeaponEntries_{};
        inline static const SlotEntry kPassthroughWeapon_{};
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
        DisplaySet d;
        d.styleMask = a_outfit.StyleMask() & ~(a_blocklist | kNeverStyleMask);
        d.hideMask  = a_outfit.HideMask() & ~(a_blocklist | kNeverHideMask);

        d.hiddenBodySkinMask   = d.hideMask & kBodySkinMask;
        d.hiddenAttachmentMask = d.hideMask & ~kBodySkinMask;
        d.hiddenHeadPartMask   = d.hideMask & kHeadPartMask;
        return d;
    }

    // Does the outfit's BODY dimension differ? Separate from ChangedSlotCount
    // on purpose - see the note on EditHistory::SlotsDiffer.
    [[nodiscard]] inline bool BodyDiffers(const Outfit& a_base, const Outfit& a_staged) {
        return a_base.obodyPreset != a_staged.obodyPreset ||
               a_base.orefit != a_staged.orefit;
    }

    // True when the outfit says anything at all about the body.
    [[nodiscard]] inline bool AnyBodyEntry(const Outfit& a_outfit) {
        return !a_outfit.obodyPreset.empty() ||
               a_outfit.orefit != ORefitMode::kDefault;
    }

    // Resolve the stored ORefit setting against the torso the player actually
    // SEES. Explicit On and Off win. Default is the transmog-aware Auto mode:
    // styled torso slots count as clothed, hidden ones count as empty, and
    // passthrough slots inherit their real worn state. If the outfit does not
    // touch any OBody-relevant torso slot, leave OBody in charge of real gear.
    [[nodiscard]] inline ORefitMode ResolveAutoORefit(
        ORefitMode a_configured, std::uint32_t a_styleMask,
        std::uint32_t a_hideMask, std::uint32_t a_realWornMask,
        bool a_globalORefitEnabled = true) {
        if (a_configured != ORefitMode::kDefault) {
            return a_configured;
        }
        const auto changed = (a_styleMask | a_hideMask) & kORefitTorsoMask;
        if (changed == 0 || !a_globalORefitEnabled) {
            return ORefitMode::kDefault;
        }
        const auto visible = (a_styleMask |
                              (a_realWornMask & ~(a_styleMask | a_hideMask))) &
                             kORefitTorsoMask;
        return visible != 0 ? ORefitMode::kForceOn : ORefitMode::kForceOff;
    }

    // True when the outfit styles or hides ANY weapon class - hiding counts.
    // The predicate behind OutfitSession's weapon fast-path flag; it lives here,
    // pure, so the part deciding whether the feature runs at all is unit-tested.
    [[nodiscard]] inline bool AnyWeaponEntry(const Outfit& a_outfit) {
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto wc = static_cast<WeaponClass>(i);
            if (a_outfit.WeaponEntryFor(wc).kind != SlotEntry::Kind::kPassthrough ||
                a_outfit.WeaponOverrideFor(wc, WeaponHand::Right).has_value() ||
                a_outfit.WeaponOverrideFor(wc, WeaponHand::Left).has_value()) {
                return true;
            }
        }
        return false;
    }

    // Build the player's transient mannequin look while editing a follower.
    // Styled slots are copied verbatim. Every other hideable, unblocked armor
    // slot is hidden so the player's real outfit cannot show through the
    // follower preview. Body settings are retained deliberately so the player
    // mannequin can preview the follower's staged OBody preset and ORefit
    // shape. This is transient only: OutfitSession clears playerMannequin_ on
    // target switch, Apply, discard, and close, then refreshes the player's
    // real saved outfit/body.
    //
    // The returned Outfit is render-only. OutfitSession never inserts it into
    // either library, so it cannot affect persistence or saved outfit data.
    [[nodiscard]] inline Outfit MakeMannequinPreview(
        const Outfit& a_follower, std::uint32_t a_blocklist) {
        Outfit out = a_follower;
        const std::uint32_t cannotHide = kNeverHideMask | a_blocklist;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (((cannotHide >> bit) & 1u) == 0 &&
                out.EntryFor(bit).kind == SlotEntry::Kind::kPassthrough) {
                out.SetHide(bit);
            }
        }
        return out;
    }

    // Build the render-only source for the player mannequin while editing a
    // follower. Captured equipped gear is the visual base even for a mutable
    // saved outfit: passthrough means "show the follower's real gear", not
    // "show the player's real gear". Explicit staged styles/hides overlay that
    // base. The mutable saved outfit itself remains untouched, so a freshly
    // created all-passthrough outfit is still empty in persistence.
    [[nodiscard]] inline Outfit ComposeMannequinSource(
        bool a_actualGear, const Outfit& a_equipped, const Outfit& a_staged) {
        if (a_actualGear) {
            return a_equipped;
        }
        Outfit out = a_equipped;
        out.name   = a_staged.name;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            const auto& entry = a_staged.EntryFor(bit);
            if (entry.kind == SlotEntry::Kind::kStyle) {
                out.SetStyle(bit, entry.style);
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                out.SetHide(bit);
            }
        }
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto cls   = static_cast<WeaponClass>(i);
            const auto& entry = a_staged.WeaponEntryFor(cls);
            if (entry.kind != SlotEntry::Kind::kPassthrough) {
                // A new Both choice replaces the equipped per-hand baseline;
                // absent staged hand overrides now inherit that new choice.
                out.ClearWeaponHandOverride(cls, WeaponHand::Right);
                out.ClearWeaponHandOverride(cls, WeaponHand::Left);
            }
            if (entry.kind == SlotEntry::Kind::kStyle) {
                out.SetWeaponStyle(cls, entry.style);
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                out.SetWeaponHide(cls);
            }
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                if (const auto& over = a_staged.WeaponOverrideFor(cls, hand);
                    over) {
                    if (over->kind == SlotEntry::Kind::kStyle) {
                        out.SetWeaponStyle(cls, over->style, hand);
                    } else if (over->kind == SlotEntry::Kind::kHide) {
                        out.SetWeaponHide(cls, hand);
                    } else {
                        out.SetWeaponPassthrough(cls, hand);
                    }
                }
            }
        }
        // Body settings belong to the staged follower outfit. Preserve them so
        // the render-only player mannequin previews the requested body along
        // with the follower's composed equipment.
        out.obodyPreset = a_staged.obodyPreset;
        out.orefit      = a_staged.orefit;
        return out;
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
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                const auto& before = a_base.WeaponOverrideFor(wc, hand);
                const auto& after  = a_staged.WeaponOverrideFor(wc, hand);
                if (before.has_value() != after.has_value() ||
                    (before && after && SlotDiffers(*before, *after))) {
                    ++n;
                }
            }
        }
        return n;
    }

    // Gold is the price of PENDING edits, not merely a structural difference
    // between two outfit values. The explicit pending bit is important in the
    // editor's tab-switch frame, where the frame-start library snapshot still
    // names the old outfit while staging already names the newly selected one.
    [[nodiscard]] inline std::uint64_t PendingGoldCost(
        bool a_hasPendingEdits, bool a_charging, std::uint32_t a_changedSlots,
        std::uint32_t a_costPerSlot) {
        return a_hasPendingEdits && a_charging
                   ? std::uint64_t(a_changedSlots) * a_costPerSlot
                   : 0;
    }

    // Reversible hide toggle for the editor's per-slot Hide/Show button.
    // A hidden SlotEntry carries the style it covers, so the round trip stays
    // reversible through editor close/open, history snapshots, JSON, and
    // follower co-saves. Legacy hides have an empty key and reveal real gear.
    inline void ToggleHideSlot(Outfit& a_staged, std::uint32_t a_bit) {
        if (a_bit >= kBitCount) {
            return;
        }
        if (a_staged.EntryFor(a_bit).kind == SlotEntry::Kind::kHide) {
            const StyleRefKey covered = a_staged.EntryFor(a_bit).style;
            if (!covered.Empty()) {
                a_staged.SetStyle(a_bit, covered);
            } else {
                a_staged.SetPassthrough(a_bit);
            }
        } else {
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
        //
        // ⚠ BODY IS DELIBERATELY *NOT* IN ChangedSlotCount, BUT *IS* HERE.
        // ChangedSlotCount drives the lore-mode Apply gold cost, which is
        // priced per changed SLOT - a body preset is not a slot and must not
        // be billed as one. But undo/redo keys on this predicate, so leaving
        // body out of it entirely would make a body-only edit invisible to
        // history: change the preset, press undo, nothing happens. The two
        // callers want different questions answered, so they get different
        // predicates.
        static bool SlotsDiffer(const Outfit& a, const Outfit& b) {
            return ChangedSlotCount(a, b) != 0 || BodyDiffers(a, b);
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

        // Returns the new index, or -1 if the saved-outfit cap is reached.
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

        // Remove one saved outfit and keep the library on the nearest remaining
        // saved outfit when the removed entry was active. If it was the final
        // entry, the library becomes inactive: Equipped gear is the permanent
        // baseline, so an empty saved-outfit library is valid.
        //
        // Returns the resulting active index (-1 == Equipped gear). Invalid
        // indices are harmless and leave the current selection untouched.
        int RemoveAndSelectNeighbor(std::size_t i) {
            if (i >= outfits_.size()) {
                return active_;
            }
            const bool removedActive = active_ == static_cast<int>(i);
            Remove(i);
            if (removedActive && !outfits_.empty()) {
                Activate(std::min(i, outfits_.size() - 1));
            }
            return active_;
        }

        void Activate(std::size_t i) {
            if (i < outfits_.size()) {
                active_ = static_cast<int>(i);
            }
        }
        void Deactivate() { active_ = -1; }

        // Quick-switch treats immutable Equipped gear as logical tab zero,
        // then every saved outfit in library order. This is the same model as
        // controller tab cycling inside the editor, including a valid
        // Equipped-only state when the saved library is empty.
        int CycleIncludingEquipped(bool a_next) {
            const int next =
                OutfitTabs::Cycle(active_, static_cast<int>(outfits_.size()), a_next);
            if (next < 0) {
                Deactivate();
            } else {
                Activate(static_cast<std::size_t>(next));
            }
            return active_;
        }

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
