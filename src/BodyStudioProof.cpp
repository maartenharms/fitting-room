#include "BodyStudioProof.h"

#include "BodyMorphPlan.h"
#include "BuildChannel.h"
#include "ObodyApi.h"
#include "OutfitSession.h"
#include "RaceMenuMorphApi.h"
#include "Settings.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OS::BodyStudioProof {

    namespace {
        struct ActorToken {
            RE::ActorHandle handle;
            std::uint32_t   referenceFormId{ 0 };
            std::uint32_t   baseFormId{ 0 };
            bool            player{ false };
            std::string     name;
        };

        struct ActiveCustom {
            ActorToken token;
            BodyPreset preset;
            ORefitPolicy policy;
        };

        std::mutex                                      g_lock;
        std::unordered_map<std::uint32_t, ActiveCustom> g_active;
        std::unordered_map<std::uint32_t, ActiveCustom> g_pending;
        std::unordered_set<std::uint32_t>                g_pendingTasks;
        std::string g_status{ "Ready. Select an installed OBody preset." };

        [[nodiscard]] ActorToken TokenFor(RE::Actor* a_actor) {
            ActorToken token;
            if (!a_actor) {
                return token;
            }
            token.handle          = a_actor->GetHandle();
            token.referenceFormId = a_actor->GetFormID();
            token.player          = a_actor->IsPlayerRef();
            if (auto* base = a_actor->GetActorBase()) {
                token.baseFormId = base->GetFormID();
            }
            if (const char* name = a_actor->GetName()) {
                token.name = name;
            }
            return token;
        }

        [[nodiscard]] RE::Actor* Resolve(const ActorToken& a_token) {
            RE::Actor* actor = nullptr;
            if (a_token.player) {
                actor = RE::PlayerCharacter::GetSingleton();
            } else if (auto ptr = a_token.handle.get()) {
                actor = ptr.get();
            }
            if (!actor || actor->GetFormID() != a_token.referenceFormId) {
                return nullptr;
            }
            auto* base = actor->GetActorBase();
            return base && base->GetFormID() == a_token.baseFormId ? actor : nullptr;
        }

        void SetStatus(std::string a_message) {
            std::scoped_lock l(g_lock);
            g_status = std::move(a_message);
        }

        [[nodiscard]] bool RuntimeReady() {
            return Enabled() && ObodyApi::Available() && RaceMenuMorphApi::Available();
        }

        void QueueGameTask(std::function<void()> a_task) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask(std::move(a_task));
            } else {
                SetStatus("SKSE task interface unavailable; proof action refused.");
            }
        }

        [[nodiscard]] float WeightOf(RE::Actor* a_actor) {
            if (a_actor) {
                if (auto* base = a_actor->GetActorBase()) {
                    return base->GetWeight();
                }
            }
            return 0.0f;
        }

        [[nodiscard]] const char* ActorName(RE::Actor* a_actor) {
            if (a_actor) {
                if (const char* name = a_actor->GetName(); name && *name) {
                    return name;
                }
            }
            return "?";
        }

        void LogApply(const char* a_transition, RE::Actor* a_actor,
                      const ActiveCustom& a_state,
                      const RaceMenuMorphApi::ApplyResult& a_result,
                      int a_resolvedORefit) {
            spdlog::info(
                "Body Studio proof: transition={} actor='{}' family={} sourceSet='{}' "
                "sliders={} actorWeight={:.1f} RaceMenu=v{} key='{}' valuesSet={} "
                "refresh={} actor3D={} ORefitResolved={} ORefitApplied={}.",
                a_transition, ActorName(a_actor),
                BodyFamilyName(a_state.preset.family),
                a_state.preset.sourceSet, a_state.preset.sliders.size(), WeightOf(a_actor),
                RaceMenuMorphApi::Version(), RaceMenuMorphApi::kOwnedKey,
                a_result.valuesSet, a_result.refreshed, a_result.actorLoaded,
                a_resolvedORefit, ObodyApi::ORefitApplied(a_actor));
        }

        bool ApplyCustomNow(ActiveCustom a_state, const char* a_transition) {
            RE::Actor* actor = Resolve(a_state.token);
            if (!actor || !RuntimeReady()) {
                SetStatus("Custom apply skipped: actor or runtime bridge became unavailable.");
                return false;
            }
            bool alreadyOwned = false;
            {
                std::scoped_lock l(g_lock);
                alreadyOwned = g_active.contains(a_state.token.referenceFormId);
            }
            // OBody ownership changes once, at the installed->custom boundary.
            // Subsequent authoring gestures replace only our RaceMenu key.
            if (!alreadyOwned) {
                ObodyApi::EnsurePlayerBaselineCaptured(actor);
                if (!ObodyApi::RemovePresetMorphsForCustom(actor)) {
                    SetStatus("OBody refused the unassign/remove ownership handoff.");
                    spdlog::error(
                        "Body Studio: OBody refused sanctioned custom handoff for '{}'.",
                        ActorName(actor));
                    return false;
                }
            }
            const auto bodyPlan = BuildBodyMorphPlan(a_state.preset, WeightOf(actor));
            std::vector<RaceMenuMorphApi::MorphValue> plan;
            plan.reserve(bodyPlan.size());
            for (const auto& value : bodyPlan) {
                plan.push_back({ value.name, value.value });
            }
            const auto applied = RaceMenuMorphApi::ApplyOwned(actor, plan);
            const int orefit = ObodyApi::ApplyORefitPolicy(
                actor, a_state.policy.mode, a_state.policy.torsoStyleMask,
                a_state.policy.torsoHideMask);
            {
                std::scoped_lock l(g_lock);
                g_active[a_state.token.referenceFormId] = a_state;
                g_status = std::string("Custom key active: ") + a_state.preset.name +
                           " [" + BodyFamilyName(a_state.preset.family) + "]";
            }
            LogApply(a_transition, actor, a_state, applied, orefit);
            // Round sixteen: the doubled-silhouette census, at the ownership
            // boundary only. A slider drag re-enters here already owned and
            // must not print thirty lines per gesture.
            if (!alreadyOwned) {
                RaceMenuMorphApi::LogKeyCensus(actor, "custom-boundary");
            }
            return true;
        }

        [[nodiscard]] std::vector<ActiveCustom> ActiveSnapshot() {
            std::scoped_lock l(g_lock);
            std::vector<ActiveCustom> out;
            out.reserve(g_active.size());
            for (const auto& [_, state] : g_active) {
                out.push_back(state);
            }
            return out;
        }

        void QueueReassertAfterRefresh(std::vector<ActiveCustom> a_states,
                                       const char* a_transition) {
            if (a_states.empty()) {
                return;
            }
            QueueGameTask([states = std::move(a_states), transition = std::string(a_transition)]() mutable {
                // OutfitSession's equipment refresh schedules its body pass one
                // task later. Schedule ours from this task so the custom key is
                // the final owner after that nested standard pass.
                QueueGameTask([states = std::move(states), transition = std::move(transition)] {
                    for (auto state : states) {
                        ApplyCustomNow(std::move(state), transition.c_str());
                    }
                });
            });
        }
    }  // namespace

    bool Enabled() {
        static_assert(BuildChannel::kBodyStudio,
                      "BodyStudioProof belongs only in builds carrying Body Studio");
        return Settings::GetSingleton().bodyStudioProof;
    }

    void ForgetSession() {
        // ⚠⚠ THE OWNERSHIP LATCH IS PER CHARACTER AND g_active IS PER PROCESS,
        // and round eighteen watched the gap: the switched save's custom apply
        // latched the player's FormID, the user loaded the ORIGINAL save, and
        // its apply printed "custom->custom-preview" - alreadyOwned - so it
        // skipped the baseline capture AND the OBody handoff on a character
        // that never handed anything off. Same class as DyeStats::Forget and
        // cleared at the same messages. Pending gestures go with it: they hold
        // tokens minted against the save being left behind.
        std::scoped_lock l(g_lock);
        g_active.clear();
        g_pending.clear();
        g_pendingTasks.clear();
        g_status = "Body Studio idle.";
    }

    Status Snapshot(RE::Actor* a_actor) {
        std::scoped_lock l(g_lock);
        const auto id = a_actor ? a_actor->GetFormID() : 0u;
        return { g_status, id != 0 && g_active.contains(id) };
    }

    void QueueInstalled(RE::Actor* a_actor, std::string_view a_preset,
                        ORefitPolicy a_policy) {
        if (!Enabled() || !a_actor || a_preset.empty()) {
            SetStatus("Select an installed preset before applying the reference.");
            return;
        }
        const auto token = TokenFor(a_actor);
        const auto name  = std::string(a_preset);
        SetStatus("Installed reference queued on the game thread.");
        QueueGameTask([token, name, a_policy] {
            auto* actor = Resolve(token);
            if (!actor || !RuntimeReady()) {
                SetStatus("Installed reference skipped: runtime bridge unavailable.");
                return;
            }
            ObodyApi::EnsurePlayerBaselineCaptured(actor);
            const auto clear = RaceMenuMorphApi::ClearOwned(actor);
            const bool accepted = ObodyApi::AssignPreset(actor, name, true);
            const int orefit = ObodyApi::ApplyORefitPolicy(
                actor, a_policy.mode, a_policy.torsoStyleMask, a_policy.torsoHideMask);
            {
                std::scoped_lock l(g_lock);
                g_active.erase(token.referenceFormId);
                g_status = accepted ? "Installed OBody reference active."
                                    : "OBody rejected the installed reference preset.";
            }
            spdlog::info(
                "Body Studio proof: transition=custom-or-baseline->installed actor='{}' "
                "preset='{}' accepted={} ownedKeyCleared={} clearRefresh={} "
                "ORefitResolved={} ORefitApplied={}.",
                ActorName(actor), name, accepted, !RaceMenuMorphApi::HasOwned(actor),
                clear.refreshed, orefit, ObodyApi::ORefitApplied(actor));
        });
    }

    void QueueCustomPreset(RE::Actor* a_actor, BodyPreset a_preset,
                           ORefitPolicy a_policy) {
        if (!Enabled() || !a_actor || a_preset.sex == BodySex::kUnknown ||
            a_preset.family == BodyFamily::kUnknown || a_preset.sliders.empty()) {
            SetStatus("Custom preview refused: preset or runtime target is incomplete.");
            return;
        }
        ActiveCustom state{ TokenFor(a_actor), std::move(a_preset), a_policy };
        const auto id = state.token.referenceFormId;
        bool schedule = false;
        {
            std::scoped_lock l(g_lock);
            g_pending[id] = std::move(state);
            schedule = g_pendingTasks.insert(id).second;
            g_status = "Body Studio preview queued.";
        }
        if (!schedule) return;
        QueueGameTask([id] {
            ActiveCustom latest;
            {
                std::scoped_lock l(g_lock);
                const auto it = g_pending.find(id);
                if (it == g_pending.end()) {
                    g_pendingTasks.erase(id);
                    return;
                }
                latest = std::move(it->second);
                g_pending.erase(it);
                g_pendingTasks.erase(id);
            }
            const auto transition = Snapshot(Resolve(latest.token)).activeCustom
                                        ? "custom->custom-preview"
                                        : "installed-or-baseline->custom-preview";
            ApplyCustomNow(std::move(latest), transition);
        });
    }

    void QueueBaseline(RE::Actor* a_actor, Baseline a_baseline,
                       ORefitPolicy a_policy) {
        if (!Enabled() || !a_actor) {
            return;
        }
        if (!a_actor->IsPlayerRef() && !a_baseline.captured) {
            SetStatus("Baseline restore refused: no pre-Fitting-Room assignment was captured.");
            return;
        }
        const auto token = TokenFor(a_actor);
        SetStatus("Baseline restore queued on the game thread.");
        QueueGameTask([token, baseline = std::move(a_baseline), a_policy]() mutable {
            auto* actor = Resolve(token);
            if (!actor || !RuntimeReady()) {
                SetStatus("Baseline restore skipped: runtime bridge unavailable.");
                return;
            }
            if (token.player) {
                ObodyApi::EnsurePlayerBaselineCaptured(actor);
                baseline = { ObodyApi::Baseline(), ObodyApi::BaselineCaptured() };
            }
            if (!baseline.captured) {
                SetStatus("Baseline restore refused: capture is still unavailable.");
                return;
            }
            const auto clear = RaceMenuMorphApi::ClearOwned(actor);
            const bool accepted = ObodyApi::AssignPreset(actor, baseline.preset, true);
            ObodyApi::ApplyORefitPolicy(
                actor, a_policy.mode, a_policy.torsoStyleMask,
                a_policy.torsoHideMask);
            {
                std::scoped_lock l(g_lock);
                g_active.erase(token.referenceFormId);
                g_status = accepted ? "Captured baseline restored."
                                    : "OBody rejected the captured baseline restore.";
            }
            spdlog::info(
                "Body Studio proof: transition=custom-or-installed->baseline actor='{}' "
                "baseline='{}' accepted={} ownedKeyCleared={} refresh={}.",
                ActorName(actor), baseline.preset.empty() ? "(none)" : baseline.preset,
                accepted, !RaceMenuMorphApi::HasOwned(actor), clear.refreshed);
        });
    }

    void QueueClearOwned(RE::Actor* a_actor) {
        if (!Enabled() || !a_actor) {
            return;
        }
        const auto token = TokenFor(a_actor);
        SetStatus("Owned-key clear queued on the game thread.");
        QueueGameTask([token] {
            auto* actor = Resolve(token);
            if (!actor || !RaceMenuMorphApi::Available()) {
                SetStatus("Owned-key clear skipped: RaceMenu bridge unavailable.");
                return;
            }
            const auto result = RaceMenuMorphApi::ClearOwned(actor);
            {
                std::scoped_lock l(g_lock);
                g_active.erase(token.referenceFormId);
                g_status = "FittingRoom.CustomBody cleared; OBody keys were untouched.";
            }
            spdlog::info(
                "Body Studio proof: transition=clear-owned actor='{}' key='{}' "
                "refresh={} stillOwned={}.",
                ActorName(actor), RaceMenuMorphApi::kOwnedKey, result.refreshed,
                RaceMenuMorphApi::HasOwned(actor));
        });
    }

    void QueueRefreshAndReassert(RE::Actor* a_actor) {
        if (!Enabled() || !a_actor) {
            return;
        }
        std::vector<ActiveCustom> states;
        {
            std::scoped_lock l(g_lock);
            if (const auto it = g_active.find(a_actor->GetFormID()); it != g_active.end()) {
                states.push_back(it->second);
            }
        }
        if (states.empty()) {
            SetStatus("No active custom body to reassert after refresh.");
            return;
        }
        if (a_actor->IsPlayerRef()) {
            OutfitSession::RequestRefresh();
        } else {
            OutfitSession::RequestRefreshActor(a_actor->GetHandle());
        }
        SetStatus("Equipment refresh queued; custom reassert will run last.");
        QueueReassertAfterRefresh(std::move(states), "equipment-refresh->custom-reassert");
    }

    void OnOBodyReady() {
        if (!Enabled()) {
            return;
        }
        auto states = ActiveSnapshot();
        if (!states.empty()) {
            spdlog::info(
                "Body Studio proof: OBody ready cycle returned; queuing {} custom actor reassertion(s).",
                states.size());
            QueueReassertAfterRefresh(std::move(states), "obody-ready->custom-reassert");
        }
    }

}  // namespace OS::BodyStudioProof
