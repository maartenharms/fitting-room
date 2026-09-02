#include "Collection.h"

#include "SharedCollection.h"  // the account-wide file's key text and parser
#include "StyleRef.h"

#include <cstring>

namespace OS {

    namespace {
        constexpr std::uint32_t kCollectionVersion = 1;
        constexpr std::size_t   kMaxEntries        = 100'000;  // sanity cap on decode

        void AppendU32(std::vector<std::byte>& a_out, std::uint32_t a_v) {
            const auto* p = reinterpret_cast<const std::byte*>(&a_v);
            a_out.insert(a_out.end(), p, p + sizeof(a_v));
        }
        void AppendString(std::vector<std::byte>& a_out, const std::string& a_s) {
            AppendU32(a_out, static_cast<std::uint32_t>(a_s.size()));
            const auto* p = reinterpret_cast<const std::byte*>(a_s.data());
            a_out.insert(a_out.end(), p, p + a_s.size());
        }
        bool ReadU32(std::span<const std::byte>& a_in, std::uint32_t& a_v) {
            if (a_in.size() < sizeof(a_v)) {
                return false;
            }
            std::memcpy(&a_v, a_in.data(), sizeof(a_v));
            a_in = a_in.subspan(sizeof(a_v));
            return true;
        }
        bool ReadString(std::span<const std::byte>& a_in, std::string& a_s) {
            std::uint32_t len = 0;
            if (!ReadU32(a_in, len) || len > 1024 || a_in.size() < len) {
                return false;
            }
            a_s.assign(reinterpret_cast<const char*>(a_in.data()), len);
            a_in = a_in.subspan(len);
            return true;
        }
    }

    Collection& Collection::GetSingleton() {
        static Collection instance;
        return instance;
    }

    void Collection::Register() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESContainerChangedEvent>(&GetSingleton());
            spdlog::info("Collection: container-changed sink registered.");
        }
    }

    namespace {
        // The form types a look can be.
        bool IsStyleForm(const RE::TESForm* a_form) {
            return a_form && (a_form->Is(RE::FormType::Armor) || a_form->Is(RE::FormType::Weapon) ||
                              a_form->Is(RE::FormType::Ammo));
        }
    }

    void Collection::SeedFromPlayerInventory() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        std::size_t added = 0;
        for (const auto& [obj, data] : player->GetInventory()) {
            if (!IsStyleForm(obj) || data.first <= 0) {
                continue;
            }
            const auto before = Size();
            Add(obj);
            added += Size() - before;
        }
        spdlog::info("Collection: inventory seed added {} looks ({} total known).", added, Size());
    }

    void Collection::Add(RE::TESBoundObject* a_form) {
        if (!IsStyleForm(a_form)) {
            return;
        }
        StyleRefKey key;
        if (!StyleRef::Make(a_form, key)) {
            return;
        }
        std::scoped_lock l(lock_);
        known_.try_emplace(a_form->GetFormID(), std::move(key));
    }

    bool Collection::Knows(RE::FormID a_id) const {
        std::scoped_lock l(lock_);
        return known_.contains(a_id);
    }

    std::size_t Collection::Size() const {
        std::scoped_lock l(lock_);
        return known_.size();
    }

    std::vector<std::byte> Collection::Encode() const {
        std::scoped_lock       l(lock_);
        std::vector<std::byte> out;
        AppendU32(out, static_cast<std::uint32_t>(known_.size()));
        for (const auto& [id, key] : known_) {
            AppendString(out, key.modName);
            AppendU32(out, key.localFormID);
        }
        return out;
    }

    bool Collection::Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version) {
        if (a_version != kCollectionVersion) {
            spdlog::warn("Collection: unknown record version {}; ignoring.", a_version);
            return false;
        }
        std::uint32_t count = 0;
        if (!ReadU32(a_bytes, count) || count > kMaxEntries) {
            return false;
        }
        std::unordered_map<RE::FormID, StyleRefKey> loaded;
        loaded.reserve(count);
        std::size_t unresolved = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            StyleRefKey key;
            if (!ReadString(a_bytes, key.modName) || !ReadU32(a_bytes, key.localFormID)) {
                return false;  // truncated - refuse the whole record
            }
            // ResolveAny, not Resolve: the map holds every dimension, so
            // resolving ARMO-only would silently drop each saved weapon look
            // on load and the collection filter would hide weapons forever.
            if (auto* form = StyleRef::ResolveAny(key)) {
                loaded.try_emplace(form->GetFormID(), std::move(key));
            } else {
                ++unresolved;  // plugin removed: drop silently, look re-earns on re-own
            }
        }
        {
            std::scoped_lock l(lock_);
            known_ = std::move(loaded);
        }
        spdlog::info("Collection: loaded {} known looks ({} unresolved skipped).",
                     Size(), unresolved);
        return true;
    }

    bool Collection::IsNew(RE::FormID a_id) const {
        std::scoped_lock l(lock_);
        // ⚠ KNOWN AND NOT ACKNOWLEDGED, in that order. Testing acked_ alone
        // would call every look in the game new, including the ones the
        // collection filter is hiding because the player has never owned them.
        return known_.contains(a_id) && !acked_.contains(a_id);
    }

    void Collection::Acknowledge(RE::FormID a_id) {
        std::scoped_lock l(lock_);
        if (known_.contains(a_id)) {
            acked_.insert(a_id);
        }
    }

    void Collection::TakeAckBaselineIfOwed() {
        std::size_t adopted = 0;
        {
            std::scoped_lock l(lock_);
            if (!ackBaselineOwed_) {
                return;
            }
            ackBaselineOwed_ = false;
            for (const auto& [id, key] : known_) {
                if (acked_.insert(id).second) {
                    ++adopted;
                }
            }
        }
        // ⚠ SAID OUT LOUD AND ONLY WHEN IT DID SOMETHING. A baseline that
        // adopts thousands of looks is the difference between a quiet browser
        // and one covered in gold, and a field report of "everything is marked
        // new" needs this line to tell a baseline that never ran from one that
        // ran on an empty collection.
        if (adopted > 0) {
            spdlog::info("Collection: this save had no acknowledged record, so its {} "
                         "known look(s) are adopted as already seen. Only looks found "
                         "from now on are marked new.",
                         adopted);
        }
    }

    std::vector<std::byte> Collection::EncodeAcked() const {
        std::scoped_lock       l(lock_);
        std::vector<std::byte> out;
        // ⚠ COUNTED AFTER THE WALK, NOT BEFORE IT. An acknowledged id whose
        // look has since left known_ has no key to write, so the honest count
        // is what actually went in. Reserving the size and then writing fewer
        // entries is how a record decodes as truncated.
        std::vector<const StyleRefKey*> keys;
        keys.reserve(acked_.size());
        for (const auto id : acked_) {
            if (const auto it = known_.find(id); it != known_.end()) {
                keys.push_back(&it->second);
            }
        }
        AppendU32(out, static_cast<std::uint32_t>(keys.size()));
        for (const auto* key : keys) {
            AppendString(out, key->modName);
            AppendU32(out, key->localFormID);
        }
        return out;
    }

    bool Collection::DecodeAcked(std::span<const std::byte> a_bytes, std::uint32_t a_version) {
        if (a_version != kCollectionVersion) {
            spdlog::warn("Collection: unknown acknowledged-record version {}; ignoring.",
                         a_version);
            return false;
        }
        std::uint32_t count = 0;
        if (!ReadU32(a_bytes, count) || count > kMaxEntries) {
            return false;
        }
        std::unordered_set<RE::FormID> loaded;
        loaded.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            StyleRefKey key;
            if (!ReadString(a_bytes, key.modName) || !ReadU32(a_bytes, key.localFormID)) {
                return false;  // truncated - refuse the whole record
            }
            // ResolveAny for Decode's reason: this set spans every dimension.
            // A look whose plugin has gone is dropped exactly as known_ drops
            // it, and it comes back unacknowledged if the plugin returns, which
            // is the harmless direction: a gold corner, not a lost colour.
            if (auto* form = StyleRef::ResolveAny(key)) {
                loaded.insert(form->GetFormID());
            }
        }
        std::size_t seen = 0;
        {
            std::scoped_lock l(lock_);
            acked_ = std::move(loaded);
            seen   = acked_.size();
            // ⚠ THE RECORD'S PRESENCE IS THE SIGNAL, not its contents. A save
            // that has been through this feature and acknowledged nothing
            // carries an empty record, and taking the baseline over it would
            // silently clear every mark the player had not got to yet.
            ackBaselineOwed_ = false;
        }
        // Read under the lock above rather than off acked_ here: this map is
        // touched by the container-changed sink, which is not this thread.
        spdlog::info("Collection: {} look(s) already seen.", seen);
        return true;
    }

    std::set<std::string> Collection::SharedIds() const {
        std::scoped_lock      l(lock_);
        std::set<std::string> out;
        for (const auto& [id, key] : known_) {
            out.insert(SharedCollection::KeyText(key));
        }
        return out;
    }

    std::size_t Collection::MergeSharedIds(const std::set<std::string>& a_ids) {
        std::size_t gained = 0;
        for (const auto& text : a_ids) {
            StyleRefKey key;
            if (!SharedCollection::ParseKeyText(text, key)) {
                continue;  // Load already refuses a malformed file whole
            }
            // ResolveAny for Decode's reason: this set spans every dimension,
            // and resolving ARMO-only would drop every shared weapon look.
            auto* const form = StyleRef::ResolveAny(key);
            if (!form) {
                continue;  // plugin absent here: nothing to draw, nothing to wear
            }
            std::scoped_lock l(lock_);
            // ⚠ NOT Add(), and the difference matters. Add takes a live form
            // and re-derives the key from it; the key is already in hand here
            // and re-deriving it would throw away the file's own answer for a
            // recomputation that can only agree or be wrong.
            if (known_.try_emplace(form->GetFormID(), key).second) {
                ++gained;
                // ⚠ DELIBERATELY NOT acked_. An inherited look arrives UNSEEN
                // so the player can find it; see Collection.h.
            }
        }
        return gained;
    }

    void Collection::Revert() {
        std::scoped_lock l(lock_);
        known_.clear();
        acked_.clear();
        // ⚠ OWED AGAIN, and this is what makes a genuinely new game behave.
        // Revert runs before every load AND before a new game, so the default
        // is "no record has spoken yet"; only DecodeAcked clears it. A new
        // character's starting kit is therefore adopted at kPostLoadGame
        // rather than greeting them in gold.
        ackBaselineOwed_ = true;
    }

    RE::BSEventNotifyControl Collection::ProcessEvent(
        const RE::TESContainerChangedEvent* a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || a_event->newContainer != player->GetFormID()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (auto* form = RE::TESForm::LookupByID(a_event->baseObj); IsStyleForm(form)) {
            if (auto* obj = form->As<RE::TESBoundObject>()) {
                const auto before = Size();
                Add(obj);
                if (Size() != before) {
                    spdlog::debug("Collection: learned look '{}' ({:08X}).", obj->GetName(),
                                  obj->GetFormID());
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace OS
