#pragma once

#include "Outfit.h"

#include <mutex>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OS {

    // The player's appearance collection: every style form (ARMO, and since the
    // weapon dimension landed WEAP/AMMO) that has ever passed through their
    // inventory (WoW-transmog style "you must own the look").
    // Seeded from the current inventory on each game load (covers saves that
    // predate the feature), then kept current by the container-changed event.
    // The style browser filters to this set by default ([General]
    // bCollectionOnly); applied outfits are NEVER invalidated by the filter.
    // One map for every dimension - the StyleRefKey shape is identical.
    class Collection : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    public:
        static Collection& GetSingleton();

        void Register();                 // event sink; call at kDataLoaded
        void SeedFromPlayerInventory();  // kPostLoadGame / kNewGame

        // Learn a look. Silently ignores forms that are not style forms.
        void               Add(RE::TESBoundObject* a_form);
        [[nodiscard]] bool Knows(RE::FormID a_id) const;
        [[nodiscard]] std::size_t Size() const;

        // ---- what the player has actually LOOKED AT (record 'KACK') --------
        //
        // A look is NEW when it is known and not acknowledged. That is the
        // whole definition, and it is a set rather than a timestamp because a
        // co-save carries no clock and a launch counter is what OS-26 already
        // does one dimension over (see RecentMods.h, which stays: it answers
        // "this PLUGIN is new" and this answers "you have not seen this PIECE").
        //
        // ⚠ ITS OWN RECORD, NOT A FIELD ON 'KNWN'. A malformed acknowledged
        // block must cost the marks and nothing else; folding it into the
        // collection would put the browser's whole filter in the same failure
        // domain as a cosmetic ornament, which is the argument 'DYHI' makes for
        // not being part of 'DYES'.
        [[nodiscard]] bool IsNew(RE::FormID a_id) const;
        void               Acknowledge(RE::FormID a_id);

        // ⚠⚠ THE BASELINE, AND WITHOUT IT THIS FEATURE LIGHTS UP EVERY ROW IN
        // THE BROWSER ON ITS FIRST LOAD. A save that predates the record knows
        // thousands of looks and has acknowledged none of them, so "new" would
        // mean "everything you have ever owned". Owed is the DEFAULT state,
        // set by Revert, and only a decoded record clears it: the record's
        // PRESENCE is what says this save has been through the feature, the
        // same load-bearing emptiness 'DFLK' relies on.
        //
        // ⚠ CALL IT AFTER SeedFromPlayerInventory, NOT AT LOAD. The seed runs
        // at kPostLoadGame and adds whatever the player is carrying that the
        // record did not have, which on a pre-feature save is their whole kit.
        // Taking the baseline before that would leave exactly those looks
        // flashing as new.
        void TakeAckBaselineIfOwed();

        // Co-save (record 'KNWN'): length-prefixed {modName, localFormID}
        // pairs - load-order independent, same doctrine as StyleRefKey.
        [[nodiscard]] std::vector<std::byte> Encode() const;
        bool Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version);

        // Record 'KACK', the same wire shape and the same load-order
        // independence, because an acknowledged look is named the same way a
        // known one is.
        [[nodiscard]] std::vector<std::byte> EncodeAcked() const;
        bool DecodeAcked(std::span<const std::byte> a_bytes, std::uint32_t a_version);

        // ---- the account-wide half (SharedCollection) ----------------------
        //
        // Every known look as stable key text, for publishing.
        [[nodiscard]] std::set<std::string> SharedIds() const;

        // Take looks another character found. Returns how many this character
        // did not already know.
        //
        // ⚠ THEY ARRIVE UNACKNOWLEDGED, which is the whole point and not a
        // side effect. An inherited wardrobe that came in already seen would be
        // invisible: no gold anywhere, nothing to find, and no way to tell it
        // from a browser that had always looked like that. See
        // SharedCollection.h on why the seen-marks are not in the file.
        //
        // ⚠ A KEY THAT NO LONGER RESOLVES IS DROPPED, not kept as a phantom.
        // Decode already works that way for the co-save's own record, and a
        // look whose plugin is absent has nothing to draw and nothing to wear.
        std::size_t MergeSharedIds(const std::set<std::string>& a_ids);

        void Revert();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;

    private:
        Collection() = default;

        mutable std::mutex                            lock_;
        std::unordered_map<RE::FormID, StyleRefKey>   known_;  // live id -> stable key
        // ⚠ LIVE IDS AT RUNTIME, STABLE KEYS ON DISK, which is known_'s own
        // split. The key for an acknowledged id is looked up in known_ at
        // encode time rather than stored twice: an acknowledged look is by
        // construction a known one, so the second copy could only ever
        // disagree with the first.
        std::unordered_set<RE::FormID>                acked_;
        // Owed until a record says otherwise. See TakeAckBaselineIfOwed.
        bool                                          ackBaselineOwed_{ true };
    };

}  // namespace OS
