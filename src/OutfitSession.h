#pragma once

#include "PCH.h"

#include "Outfit.h"
#include "Persistence.h"

#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace OS {

    // The single source of truth for what the render override should display.
    // Thread-safe: hooks read it from the skinning pass.
    class OutfitSession {
    public:
        static OutfitSession& GetSingleton();

        // The outfit the override renders: the editor's staged outfit while the
        // editor is open, else the library's active outfit. One code path - a
        // live preview is not a second mechanism.
        [[nodiscard]] bool       IsActive() const;
        [[nodiscard]] DisplaySet Display() const;

        // Calls a_fn(bit, ARMO*) for each styled slot, resolved to live forms.
        void VisitStyles(const std::function<void(std::uint32_t, RE::TESObjectARMO*)>& a_fn) const;

        // Locked access to the outfit library. The callback runs while lock_ is
        // held, so it must NOT call another OutfitSession method (self-deadlock)
        // and must NOT call RequestRefresh(). Does not auto-refresh: if the
        // callback changes the displayed appearance, the caller calls
        // RequestRefresh() afterwards. Every call queues a global-library file
        // save (the library persists across saves) - mutators are the only
        // intended callers; readers use SnapshotLibrary.
        template <class Fn>
        void WithLibrary(Fn&& a_fn) {
            {
                std::scoped_lock l(lock_);
                std::forward<Fn>(a_fn)(library_);
            }
            Persistence::QueueLibrarySave();
        }

        // A consistent copy for readers that need to hold library state across a
        // frame (e.g. the ImGui editor's draw loop) without holding the lock.
        [[nodiscard]] OutfitLibrary SnapshotLibrary() const {
            std::scoped_lock l(lock_);
            return library_;
        }

        // Editor staging.
        void        BeginStaging(const Outfit& a_from);
        void        UpdateStaging(const Outfit& a_next);
        void        CommitStaging();   // staged -> active outfit
        void        DiscardStaging();
        [[nodiscard]] bool IsStaging() const;

        // Beast form / race switch.
        void Suspend();
        void Resume();

        // Persistence callbacks. OnLoad installs the GLOBAL library (file or
        // legacy migration); OnRevert resets only per-save state - the library
        // is global and survives save/load boundaries.
        void OnLoad(OutfitLibrary a_lib);
        void OnRevert();

        // Activate the outfit with this name (per-save active selection);
        // empty or unknown name deactivates. Refreshes the player.
        void ActivateByName(std::string_view a_name);

        // Does not refresh; a caller changing this at runtime must call RequestRefresh() afterwards.
        void SetBlocklist(std::uint32_t a_mask);

        // Queue a player visual rebuild on the main thread.
        static void RequestRefresh();

    private:
        OutfitSession() = default;
        [[nodiscard]] const Outfit* EffectiveLocked() const;

        mutable std::mutex    lock_;
        OutfitLibrary         library_;
        std::optional<Outfit> staged_;
        std::uint32_t         blocklist_{ 0 };
        bool                  suspended_{ false };
    };

}  // namespace OS
