#include "OutfitSession.h"

#include "REAugments.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "StyleRef.h"

namespace OS {

    OutfitSession& OutfitSession::GetSingleton() {
        static OutfitSession instance;
        return instance;
    }

    const Outfit* OutfitSession::EffectiveLocked() const {
        // A scene mod (OStim etc.) is running - stand down entirely so the
        // player renders their real/undressed gear, not the transmog. Both
        // biped hooks gate on IsActive(), which flows through here, so this one
        // early-out suspends the whole override. Lock-free atomic read.
        if (SceneGuard::Active()) {
            return nullptr;
        }
        if (suspended_) {
            return nullptr;
        }
        if (staged_) {
            return &*staged_;
        }
        return library_.Active();
    }

    bool OutfitSession::IsActive() const {
        std::scoped_lock l(lock_);
        return EffectiveLocked() != nullptr;
    }

    DisplaySet OutfitSession::Display() const {
        std::scoped_lock l(lock_);
        const auto* o = EffectiveLocked();
        return o ? ComputeDisplaySet(*o, blocklist_) : DisplaySet{};
    }

    void OutfitSession::VisitStyles(
        const std::function<void(std::uint32_t, RE::TESObjectARMO*)>& a_fn) const {
        std::vector<std::pair<std::uint32_t, StyleRefKey>> snapshot;
        {
            std::scoped_lock l(lock_);
            const auto* o = EffectiveLocked();
            if (!o) {
                return;
            }
            const auto allowed = ComputeDisplaySet(*o, blocklist_).styleMask;
            o->ForEachStyle([&](std::uint32_t bit, const StyleRefKey& key) {
                if ((allowed >> bit) & 1u) {
                    snapshot.emplace_back(bit, key);
                }
            });
        }
        for (const auto& [bit, key] : snapshot) {
            if (auto* armo = StyleRef::Resolve(key)) {
                a_fn(bit, armo);
            } else {
                spdlog::debug("style bit {}: '{}'|{:06X} unresolved (plugin missing?)",
                              bit, key.modName, key.localFormID);
            }
        }
    }

    void OutfitSession::BeginStaging(const Outfit& a_from) {
        {
            std::scoped_lock l(lock_);
            staged_ = a_from;
        }
        RequestRefresh();
    }

    void OutfitSession::UpdateStaging(const Outfit& a_next) {
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            staged_ = a_next;
        }
        RequestRefresh();
    }

    void OutfitSession::CommitStaging() {
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            const int idx = library_.ActiveIndex();
            if (idx >= 0) {
                if (auto* o = library_.At(static_cast<std::size_t>(idx))) {
                    *o = *staged_;
                }
            }
            staged_.reset();
        }
        Persistence::QueueLibrarySave();  // commit mutates the global library
        RequestRefresh();
    }

    void OutfitSession::DiscardStaging() {
        {
            std::scoped_lock l(lock_);
            staged_.reset();
        }
        RequestRefresh();
    }

    bool OutfitSession::IsStaging() const {
        std::scoped_lock l(lock_);
        return staged_.has_value();
    }

    void OutfitSession::Suspend() {
        {
            std::scoped_lock l(lock_);
            if (suspended_) {
                return;
            }
            suspended_ = true;
        }
        spdlog::info("override suspended (beast form / race switch).");
        RequestRefresh();
    }

    void OutfitSession::Resume() {
        {
            std::scoped_lock l(lock_);
            if (!suspended_) {
                return;
            }
            suspended_ = false;
        }
        spdlog::info("override resumed.");
        RequestRefresh();
    }

    void OutfitSession::OnLoad(OutfitLibrary a_lib) {
        {
            std::scoped_lock l(lock_);
            library_   = std::move(a_lib);
            staged_.reset();
            suspended_ = false;
        }
        RequestRefresh();
    }

    void OutfitSession::OnRevert() {
        // The library is GLOBAL (outfits.json) - it survives save boundaries.
        // Only per-save state resets: the active selection and any staging.
        std::scoped_lock l(lock_);
        library_.Deactivate();
        staged_.reset();
        suspended_ = false;
    }

    void OutfitSession::ActivateByName(std::string_view a_name) {
        bool found = false;
        {
            std::scoped_lock l(lock_);
            library_.Deactivate();
            if (!a_name.empty()) {
                for (std::size_t i = 0; i < library_.Count(); ++i) {
                    if (const auto* o = library_.At(i); o && o->name == a_name) {
                        library_.Activate(i);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!a_name.empty() && !found) {
            spdlog::warn("Active outfit '{}' not found in the global library (renamed or "
                         "deleted from another save?) - deactivated.",
                         a_name);
        }
        RequestRefresh();
    }

    void OutfitSession::SetBlocklist(std::uint32_t a_mask) {
        std::scoped_lock l(lock_);
        blocklist_ = a_mask;
    }

    void OutfitSession::RequestRefresh() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                // Defensive: a background refresh must never take the game down;
                // a C++ throw is caught and logged instead (engine AVs are still
                // handled by CrashGuard around the kick, not here).
                try {
                    REAug::RefreshPlayer(Settings::GetSingleton().sceneKick);
                } catch (const std::exception& e) {
                    spdlog::error("RefreshPlayer threw: {}", e.what());
                } catch (...) {
                    spdlog::error("RefreshPlayer threw a non-standard exception.");
                }
            });
        }
    }

}  // namespace OS
