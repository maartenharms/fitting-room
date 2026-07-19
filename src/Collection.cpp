#include "Collection.h"

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

    void Collection::Revert() {
        std::scoped_lock l(lock_);
        known_.clear();
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
