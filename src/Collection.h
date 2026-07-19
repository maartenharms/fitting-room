#pragma once

#include "Outfit.h"

#include <mutex>
#include <span>
#include <unordered_map>
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

        // Co-save (record 'KNWN'): length-prefixed {modName, localFormID}
        // pairs - load-order independent, same doctrine as StyleRefKey.
        [[nodiscard]] std::vector<std::byte> Encode() const;
        bool Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version);
        void Revert();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;

    private:
        Collection() = default;

        mutable std::mutex                            lock_;
        std::unordered_map<RE::FormID, StyleRefKey>   known_;  // live id -> stable key
    };

}  // namespace OS
