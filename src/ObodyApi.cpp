#include "ObodyApi.h"

#include "OutfitSession.h"
#include "RaceMenuMorphApi.h"  // the Shape key, which OBody's apply takes with it

#if defined(FR_BODY_STUDIO)
#include "BodyStudioProof.h"
#include "Settings.h"  // bRequireWornForStyles, which decides whether a style shows at all
#endif

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

// The vendored header deliberately leaves the Skyrim types undefined so the
// consumer can bind them to whatever RE layer it uses (see its own preamble).
// CommonLibSSE-NG's are the ones this plugin is built on. These aliases must be
// visible as UNQUALIFIED names from inside namespace OBody::API, so they live at
// global scope and this stays the only TU that includes the vendored header.
using Actor    = RE::Actor;
using TESForm  = RE::TESForm;
#include "../extern/OBody_API.h"

namespace OS::ObodyApi {

    namespace {

        OBody::API::IPluginInterface* g_api = nullptr;
        std::string                   g_baseline;  // player's pre-Fitting-Room preset
        bool                          g_baselineCaptured{ false };

        // Written by OBody's readiness callbacks (its thread), read by the
        // editor (the Present/render thread) - hence atomic rather than a plain
        // bool. Everything else here is touched only while ready.
        std::atomic<bool> g_ready{ false };

        // Atomic snapshot of the active outfit's ORefit policy. Bits 0..1 are
        // mode (0 Auto / 1 On / 2 Off), bits 2..33 are styled torso slots, and
        // bits 34..63 are hidden torso slots. Only bits 2, 16, and 26 occur in
        // either mask, so the packed value fits. One atomic prevents a clothing
        // callback from observing masks from one outfit and mode from another.
        std::atomic<std::uint64_t> g_playerORefitPolicy{ 0 };
        std::mutex                 g_npcPolicyLock;
        std::unordered_map<std::uint32_t, std::uint64_t> g_npcORefitPolicies;

        // Re-entrancy guard: ForcefullyChangeORefitForActor can itself raise an
        // OnORefitForcefullyChanged event, and re-asserting from inside our own
        // re-assert would be an unbounded ping-pong on OBody's thread.
        std::atomic<bool> g_inReassert{ false };

        // ⚠ ORefit IS RE-DERIVED BY OBODY ON EVERY CLOTHING CHANGE, and our
        // transmog causes clothing changes. That is why a forced ORefit
        // "didn't work" in the field: ForcefullyChangeORefitForActor was being
        // applied, then silently undone the next time the player equipped
        // anything, because OBody recomputes from the REAL worn gear (which our
        // display never changes). Re-asserting on the event is the only way a
        // per-outfit override can survive - a one-shot call at Apply cannot.
        //
        // Defined ABOVE ReadinessListener because that class registers this one
        // on OBodyIsReady, and binding it to the IActorChangeEventListener&
        // parameter needs the COMPLETE type - a forward declaration will not do.
        [[nodiscard]] std::uint64_t PackORefitPolicy(int a_mode,
                                                     std::uint32_t a_styleMask,
                                                     std::uint32_t a_hideMask) {
            return (static_cast<std::uint64_t>(a_mode) & 0x3u) |
                   (static_cast<std::uint64_t>(a_styleMask & kORefitTorsoMask) << 2u) |
                   (static_cast<std::uint64_t>(a_hideMask & kORefitTorsoMask) << 34u);
        }

        [[nodiscard]] std::uint64_t PolicyForActor(RE::Actor* a_actor) {
            if (!a_actor) {
                return 0;
            }
            if (a_actor == RE::PlayerCharacter::GetSingleton()) {
                return g_playerORefitPolicy.load(std::memory_order_acquire);
            }
            std::scoped_lock l(g_npcPolicyLock);
            if (const auto it = g_npcORefitPolicies.find(a_actor->GetFormID());
                it != g_npcORefitPolicies.end()) {
                return it->second;
            }
            return 0;
        }

        void SetPolicyForActor(RE::Actor* a_actor, std::uint64_t a_policy) {
            if (!a_actor) {
                return;
            }
            if (a_actor == RE::PlayerCharacter::GetSingleton()) {
                g_playerORefitPolicy.store(a_policy, std::memory_order_release);
                return;
            }
            std::scoped_lock l(g_npcPolicyLock);
            if (a_policy == 0) {
                g_npcORefitPolicies.erase(a_actor->GetFormID());
            } else {
                g_npcORefitPolicies[a_actor->GetFormID()] = a_policy;
            }
        }

        [[nodiscard]] std::uint32_t WornTorsoMask(
            RE::Actor* a_actor, const RE::TESForm* a_changed = nullptr,
            bool a_equipping = false) {
            if (!a_actor) {
                return 0;
            }
            const auto* changedArmor =
                a_changed ? a_changed->As<RE::TESObjectARMO>() : nullptr;
            const auto changedMask = changedArmor
                                         ? static_cast<std::uint32_t>(changedArmor->GetSlotMask())
                                         : 0u;
            std::uint32_t wornMask = 0;
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            for (const auto bit : { kBitBody, BitForEditorSlot(46), BitForEditorSlot(56) }) {
                const auto slotMask = 1u << bit;
                auto*      worn     = a_actor->GetWornArmor(static_cast<Slot>(slotMask));
                bool       occupied = worn != nullptr;
                if (changedArmor && (changedMask & slotMask) != 0) {
                    // OBody sends this callback from TESEquipEvent before the
                    // inventory's worn state is guaranteed to reflect the event.
                    occupied = a_equipping || worn != changedArmor;
                }
                if (occupied) {
                    wornMask |= slotMask;
                }
            }
            return wornMask;
        }

        [[nodiscard]] int DesiredORefit(RE::Actor* a_actor,
                                        const RE::TESForm* a_changed = nullptr,
                                        bool a_equipping = false) {
            const auto policy = PolicyForActor(a_actor);
            const auto mode   = static_cast<ORefitMode>(policy & 0x3u);
            const auto styles = static_cast<std::uint32_t>((policy >> 2u) & 0xFFFFFFFFu);
            const auto hides  = static_cast<std::uint32_t>((policy >> 34u) & 0x3FFFFFFFu);
            return static_cast<int>(ResolveAutoORefit(
                mode, styles, hides, WornTorsoMask(a_actor, a_changed, a_equipping),
                g_api && g_api->IsORefitEnabled(),
                Settings::GetSingleton().requireWornForStyles));
        }

        // ---- keeping the Shape page's morphs across an OBody body pass -------
        //
        // ⚠⚠ MEASURED 2026-08-16, AND IT IS TOTAL. Toggling Hide outfit runs a
        // body pass, and every one of them emptied Fitting Room's own morph key:
        //
        //   Shape: RaceMenu's store LOST 1 of 1 morph(s) ... 'Amazon' page 1.180 store 0.000
        //   Shape: RaceMenu's store LOST 2 of 2 morph(s) ...
        //   Shape: RaceMenu's store LOST 3 of 3 morph(s) ...
        //
        // n of n, every time, both directions. OBody's apply clears EVERY morph
        // key rather than the two it owns, so a shape written under our key is
        // collateral. RaceMenu sums keys precisely so two mods can shape one
        // body, and that only holds while neither clears the other's.
        //
        // ⚠⚠ AND THE SHAPE PAGE IS NOT THE STORAGE, WHICH IS WHY THIS IS NOT A
        // UI BUG. A Shape slider writes straight through to RaceMenu and nothing
        // in this plugin keeps a copy, so with the page CLOSED there was nobody
        // to notice and nobody to put it back: an outfit change carrying a body
        // preset silently destroyed the shape for good. The page-side watch that
        // found this only runs while the page is open.
        //
        // ⚠ SO THE GUARD IS HERE, AT THE CONSUMER, and not at the call sites.
        // Four paths reach an OBody apply - the editor, the session's player and
        // follower passes, and Body Studio - and a fix at any one of them is a
        // fix for that one. This is the same shape the hide-outfit defect was
        // finally fixed with: gate where the damage happens, so every caller
        // inherits it.
        //
        // ⚠ IT RESTORES ONLY WHAT WAS ACTUALLY LOST. The destructor re-reads the
        // key and does nothing when everything it held is still there, so a pass
        // that does not wipe costs one enumeration and no refresh at all.
        class ShapeKeeper {
        public:
            explicit ShapeKeeper(RE::Actor* a_actor)
                : actor_(a_actor), held_(RaceMenuMorphApi::SnapshotShape(a_actor)) {}

            ShapeKeeper(const ShapeKeeper&)            = delete;
            ShapeKeeper& operator=(const ShapeKeeper&) = delete;

            ~ShapeKeeper() {
                if (!actor_ || held_.empty()) {
                    return;
                }
                const auto now = RaceMenuMorphApi::SnapshotShape(actor_);
                std::size_t lost = 0;
                for (const auto& want : held_) {
                    bool stillThere = false;
                    for (const auto& have : now) {
                        if (have.name == want.name && have.value == want.value) {
                            stillThere = true;
                            break;
                        }
                    }
                    if (!stillThere) {
                        ++lost;
                    }
                }
                if (lost == 0) {
                    return;
                }
                (void)RaceMenuMorphApi::ApplyShape(actor_, held_);
                // ⚠ ONE INFO THE FIRST TIME, DEBUG AFTER. "Did this ever run" has
                // to be answerable from an ordinary log, and a body pass happens
                // on every outfit change, so a line per pass would drown the file.
                static bool s_saidOnce = false;
                if (!s_saidOnce) {
                    s_saidOnce = true;
                    spdlog::info(
                        "Shape: an OBody body pass cleared {} of {} morph(s) under our key; "
                        "put them back. This is expected and handled - OBody clears every "
                        "morph key, not only its own.",
                        lost, held_.size());
                } else {
                    spdlog::debug("Shape: put back {} of {} morph(s) after an OBody body pass.",
                                  lost, held_.size());
                }
            }

        private:
            RE::Actor*                                actor_;
            std::vector<RaceMenuMorphApi::MorphValue> held_;
        };

        class ChangeListener final : public OBody::API::IActorChangeEventListener {
        public:
            OnActorClothingUpdate::Response OnActorClothingUpdate(
                Actor* a_actor, OnActorClothingUpdate::Flags a_flags,
                OnActorClothingUpdate::Payload& a_payload) override {
                const bool equipping =
                    (static_cast<std::uint64_t>(a_flags) &
                     OnActorClothingUpdate::Flags::ActorIsEquipping) != 0;
                Enforce(a_actor, "clothing update",
                        DesiredORefit(a_actor, a_payload.changedEquipment, equipping));
                return OnActorClothingUpdate::Response::None;
            }

            OnORefitForcefullyChanged::Response OnORefitForcefullyChanged(
                Actor* a_actor, OnORefitForcefullyChanged::Flags,
                OnORefitForcefullyChanged::Payload&) override {
                Enforce(a_actor, "forced ORefit change", DesiredORefit(a_actor));
                return OnORefitForcefullyChanged::Response::None;
            }

        private:
            static void Enforce(Actor* a_actor, const char* a_reason, int a_want) {
                const int want = a_want;
                if (want == 0 || !a_actor) {
                    return;  // following OBody's setting
                }
                auto* api = g_ready.load(std::memory_order_acquire) ? g_api : nullptr;
                if (!api) {
                    return;
                }

                // Event flags are explicitly frozen before any listener runs.
                // Another listener ahead of us may already have changed the
                // actor, so ask OBody for the authoritative state instead.
                const bool isApplied = api->ActorHasORefitApplied(a_actor);
                const bool shouldBe = (want == 1);
                if (isApplied == shouldBe) {
                    return;  // already right
                }
                bool expected = false;
                if (!g_inReassert.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
                    return;  // already re-asserting
                }
                api->ForcefullyChangeORefitForActor(a_actor, shouldBe);
                spdlog::debug("OBody: re-asserted ORefit={} after {} "
                              "(OBody had set it to {}).", shouldBe, a_reason, isApplied);
                g_inReassert.store(false, std::memory_order_release);
            }
        };

        ChangeListener& Changes() {
            static ChangeListener listener;  // same lifetime rule as the readiness one
            return listener;
        }

        // ⚠ THIS LISTENER MUST OUTLIVE THE PROCESS. The API contract is explicit:
        // the instance handed to OBody "must remain valid until the process
        // terminates". A member of some singleton that could be torn down, or
        // anything with a non-trivial destructor ordering story, is a
        // use-after-free waiting for someone else's load order. A function-local
        // static with static storage duration is the cheapest way to be sure.
        class ReadinessListener final : public OBody::API::IOBodyReadinessEventListener {
        public:
            void OBodyIsReady() override {
                g_ready.store(true, std::memory_order_release);
                spdlog::info("OBody: API ready.");
                RegisterChangeListenerOnce();
                LogCapabilitiesOnce();
                // The save's active outfit may have refreshed while OBody was
                // still unready. Re-run the single appearance path now so its
                // preset and ORefit mode cannot remain unapplied until the next
                // unrelated edit or equipment change.
                OutfitSession::RequestRefresh();
                OutfitSession::RequestRefreshLoadedNpcs();
#if defined(FR_BODY_STUDIO)
                // Queue after the standard refresh requests. The proof adds
                // one more task hop so custom ownership is reasserted last.
                BodyStudioProof::OnOBodyReady();
#endif
            }

            void OBodyIsNoLongerReady() override {
                // Fires around saves, not just at shutdown. Anything cached from
                // the API (preset name string_views above all) is dead from here
                // until the next OBodyIsReady.
                g_ready.store(false, std::memory_order_release);
                spdlog::debug("OBody: API no longer ready (expected around saves).");
            }

        private:
            // Registered once per session. HasRegisteredEventListener guards
            // the ready/unready cycle (which repeats on every save) from
            // stacking duplicate registrations.
            static void RegisterChangeListenerOnce() {
                if (!g_api || g_api->HasRegisteredEventListener(Changes())) {
                    return;
                }
                if (g_api->RegisterEventListener(Changes())) {
                    spdlog::info("OBody: change listener registered - a per-outfit ORefit "
                                 "override now survives equipment changes.");
                } else {
                    spdlog::warn("OBody: change listener registration REFUSED; a per-outfit "
                                 "ORefit override will be undone by the next equip.");
                }
            }

            // Once per session, not once per ready-cycle: the ready/unready
            // cycle repeats on every save, and logging preset counts each time
            // would put a line in the log for every save the player makes.
            static void LogCapabilitiesOnce() {
                static std::once_flag once;
                std::call_once(once, [] {
                    if (!g_api) {
                        return;
                    }
                    OBody::API::PresetCounts counts{};
                    g_api->GetPresetCounts(counts);
                    spdlog::info("OBody: {} female preset(s) ({} blacklisted), {} male ({} "
                                 "blacklisted); global ORefit {}.",
                                 counts.female, counts.femaleBlacklisted, counts.male,
                                 counts.maleBlacklisted,
                                 g_api->IsORefitEnabled() ? "ON" : "OFF");
                });
            }
        };

        ReadinessListener& Listener() {
            static ReadinessListener listener;  // never destroyed before exit
            return listener;
        }

        // One place that answers "may I touch the API this instant".
        [[nodiscard]] OBody::API::IPluginInterface* Live() {
            return g_ready.load(std::memory_order_acquire) ? g_api : nullptr;
        }

        void RestoreDefaultORefit(OBody::API::IPluginInterface& a_api, RE::Actor* a_actor) {
            // "Default" means OBody's normal result for the actor's REAL gear,
            // not merely its global MCM switch. A globally enabled ORefit still
            // stays off for an actor OBody considers naked.
            const bool shouldBe = a_api.IsORefitEnabled() && !a_api.ActorIsNaked(a_actor);
            a_api.ForcefullyChangeORefitForActor(a_actor, shouldBe);
        }

    }  // namespace

    void Request() {
        if (g_api) {
            return;  // already answered; re-requesting is not permitted mid-flight
        }
        OBody::API::SKSEMessages::RequestPluginInterface req{};
        req.version                = OBody::API::PluginAPIVersion::Latest;
        req.pluginInterface        = &g_api;
        req.readinessEventListener = &Listener();

        SKSE::GetMessagingInterface()->Dispatch(decltype(req)::type, &req,
                                                sizeof(decltype(req)), "OBody");

        if (g_api) {
            // Naming ourselves is purely diagnostic - it is what OBody prints
            // when it attributes an API call, so a bug report naming us is
            // traceable in OBody's own log rather than only in ours.
            g_api->SetOwner("FittingRoom");
            spdlog::info("OBody: plugin API acquired (v{}). Body settings are available.",
                         static_cast<int>(g_api->PluginAPIVersion()));
        } else {
            // Not an error: no OBody, an OBody older than 4.4.0 (the release
            // that added the API), or a version it no longer serves. The body
            // category simply does not appear.
            spdlog::info("OBody: no plugin API (OBody absent, or older than 4.4.0). "
                         "Per-outfit body settings will be hidden.");
        }
    }

    bool Present() { return g_api != nullptr; }

    bool Available() { return Live() != nullptr; }

    std::vector<std::string> PresetNames(bool a_female) {
        std::vector<std::string> out;
        auto* api = Live();
        if (!api) {
            return out;
        }
        const auto category = a_female ? OBody::API::PresetCategoryFemale
                                       : OBody::API::PresetCategoryMale;

        OBody::API::PresetCounts counts{};
        api->GetPresetCounts(counts);
        const std::size_t n = a_female ? counts.female : counts.male;
        if (n == 0) {
            return out;
        }

        // Ask for exactly what the count promised, then trust the RETURN VALUE
        // rather than the count: the two are read at slightly different moments
        // and only the return value describes what was actually written.
        std::vector<std::string_view> views(n);
        const std::size_t written = api->GetPresetNames(category, views.data(), views.size());

        out.reserve(written);
        for (std::size_t i = 0; i < written; ++i) {
            out.emplace_back(views[i]);  // COPY - see the note in ObodyApi.h
        }
        return out;
    }

    std::string AssignedPreset(RE::Actor* a_actor) {
        auto* api = Live();
        if (!api || !a_actor) {
            return {};
        }
        OBody::API::PresetAssignmentInformation info{};
        api->GetPresetAssignedToActor(a_actor, info);
        return std::string{ info.presetName };  // copy, same lifetime rule
    }

    void EnsureProcessed(RE::Actor* a_actor) {
        if (auto* api = Live(); api && a_actor) {
            api->EnsureActorIsProcessed(a_actor);
        }
    }

    bool AssignPreset(RE::Actor* a_actor, std::string_view a_presetName, bool a_applyMorphsNow) {
        auto* api = Live();
        if (!api || !a_actor) {
            return false;
        }
        const ShapeKeeper keeper{ a_actor };
        OBody::API::AssignPresetPayload payload{};
        payload.presetName = a_presetName;
        payload.flags      = a_applyMorphsNow
                                 ? OBody::API::AssignPresetPayload::Flags::ForceImmediateApplicationOfMorphs
                                 : OBody::API::AssignPresetPayload::Flags::DoNotApplyMorphs;
        return api->AssignPresetToActor(a_actor, payload);
    }

    bool RemovePresetMorphsForCustom(RE::Actor* a_actor) {
        auto* api = Live();
        if (!api || !a_actor) {
            return false;
        }
        const ShapeKeeper keeper{ a_actor };
        api->EnsureActorIsProcessed(a_actor);
        OBody::API::AssignPresetPayload payload{};
        payload.presetName = {};
        payload.flags = OBody::API::AssignPresetPayload::Flags::DoNotApplyMorphs;
        if (!api->AssignPresetToActor(a_actor, payload)) {
            return false;
        }
        // OBody owns the OBody/OClothe keys and clears them itself here. Fitting
        // Room never reaches through RaceMenu to clear either key.
        api->RemoveOBodyMorphsFromActor(a_actor);
        return true;
    }

    void EnsurePlayerBaselineCaptured(RE::Actor* a_actor) {
        if (!a_actor || !a_actor->IsPlayerRef() || g_baselineCaptured) {
            return;
        }
        if (auto* api = Live()) {
            api->EnsureActorIsProcessed(a_actor);
            g_baseline         = AssignedPreset(a_actor);
            g_baselineCaptured = true;
            spdlog::info("Body Studio proof: captured player baseline '{}' before custom handoff.",
                         g_baseline.empty() ? "(none)" : g_baseline);
        }
    }

    void ForgetPlayerBaseline() {
        // The baseline is one character's "usual body", latched once. Across
        // a save load it would restore the OUTGOING character's preset onto
        // whoever was loaded (round eighteen's cross-save latch family), so
        // the load boundary clears it and the next handoff re-captures.
        g_baseline.clear();
        g_baselineCaptured = false;
    }

    void ForceORefit(RE::Actor* a_actor, bool a_applied) {
        if (auto* api = Live(); api && a_actor) {
            api->ForcefullyChangeORefitForActor(a_actor, a_applied);
        }
    }

    int ApplyORefitPolicy(RE::Actor* a_actor, int a_orefit,
                          std::uint32_t a_torsoStyleMask,
                          std::uint32_t a_torsoHideMask) {
        SetPolicyForActor(a_actor, PackORefitPolicy(
            a_orefit, a_torsoStyleMask, a_torsoHideMask));
        auto* api = Live();
        if (!api || !a_actor) {
            return 0;
        }
        const int desired = DesiredORefit(a_actor);
        switch (desired) {
            case 1: api->ForcefullyChangeORefitForActor(a_actor, true); break;
            case 2: api->ForcefullyChangeORefitForActor(a_actor, false); break;
            default: RestoreDefaultORefit(*api, a_actor); break;
        }
        return desired;
    }

    bool ORefitApplied(RE::Actor* a_actor) {
        auto* api = Live();
        return api && a_actor && api->ActorHasORefitApplied(a_actor);
    }

    bool GlobalORefitEnabled() {
        auto* api = Live();
        return api && api->IsORefitEnabled();
    }

    // ---- outfit glue -----------------------------------------------------

    std::string Baseline() { return g_baseline; }
    bool        BaselineCaptured() { return g_baselineCaptured; }

    void SetBaseline(std::string_view a_preset, bool a_captured) {
        g_baseline         = a_preset;
        g_baselineCaptured = a_captured;
    }

    void ApplyOutfitBody(RE::Actor* a_actor, std::string_view a_preset, int a_orefit,
                         std::uint32_t a_torsoStyleMask,
                         std::uint32_t a_torsoHideMask) {
        // Publish even while OBody is unready. A save-load refresh commonly
        // lands in that window, and the clothing listener must still know what
        // the active outfit wants when readiness returns.
        SetPolicyForActor(a_actor, PackORefitPolicy(
            a_orefit, a_torsoStyleMask, a_torsoHideMask));

        auto* api = Live();
        if (!api || !a_actor) {
            return;  // no OBody, or unready (saves) - the next refresh re-asserts
        }

        if (!a_preset.empty()) {
            // CAPTURE BEFORE THE FIRST ASSIGN, and only ever once. Reading the
            // assigned preset AFTER assigning would just read back our own
            // value, and re-capturing on a later switch would overwrite the
            // real baseline with the previous outfit's preset.
            if (!g_baselineCaptured) {
                g_baseline         = AssignedPreset(a_actor);
                g_baselineCaptured = true;
                spdlog::info("OBody: captured baseline body preset '{}' before the first "
                             "outfit assignment - \"Your usual body\" returns here.",
                             g_baseline.empty() ? "(none)" : g_baseline);
            }
            AssignPreset(a_actor, a_preset, true);
        } else if (g_baselineCaptured) {
            // "Your usual body": revert, but ONLY if we ever moved it. Without
            // this guard an outfit with no body setting would clear the preset
            // of a player who set one in OBody's own MCM and never touched ours.
            AssignPreset(a_actor, g_baseline, true);  // empty baseline clears the assignment
        }

        // OBody's GenerateBodyByPreset clears its OClothe morph key and then
        // derives ORefit again from the actor's REAL equipped torso. Therefore
        // the outfit override must be the final operation, after any preset
        // assignment or baseline restore, or opening Fitting Room can silently
        // replace Force Off with the real gear's refit state.
        const int desiredORefit = DesiredORefit(a_actor);
        switch (desiredORefit) {
            case 1: api->ForcefullyChangeORefitForActor(a_actor, true); break;
            case 2: api->ForcefullyChangeORefitForActor(a_actor, false); break;
            default:
                // Leaving a forced outfit must not leave its actor state stuck
                // behind. Restore OBody's normal real-gear result once, then
                // want==0 lets future clothing updates follow OBody normally.
                RestoreDefaultORefit(*api, a_actor);
                break;
        }
    }

    void ApplyNpcOutfitBody(RE::Actor* a_actor, std::string_view a_preset,
                            int a_orefit, std::uint32_t a_torsoStyleMask,
                            std::uint32_t a_torsoHideMask,
                            std::string_view a_baseline,
                            bool a_baselineCaptured) {
        SetPolicyForActor(a_actor, PackORefitPolicy(
            a_orefit, a_torsoStyleMask, a_torsoHideMask));
        auto* api = Live();
        if (!api || !a_actor) {
            return;
        }

        // Followers are not guaranteed to have crossed OBody's normal
        // distribution path before Fitting Room edits them. Establish that
        // state first. AssignPreset uses
        // ForceImmediateApplicationOfMorphs, so it both stores the assignment
        // and applies it; calling ApplyOBodyMorphsToActor again would regenerate
        // the same body twice and add no stronger guarantee.
        api->EnsureActorIsProcessed(a_actor);
        const std::string_view requested =
            !a_preset.empty() ? a_preset
                              : (a_baselineCaptured ? a_baseline : std::string_view{});
        const bool shouldAssign = !a_preset.empty() || a_baselineCaptured;
        bool       accepted     = true;
        if (shouldAssign) {
            accepted = AssignPreset(a_actor, requested, true);
        }

        const char* actorName = a_actor->GetName();
        if (shouldAssign) {
            const auto assigned = AssignedPreset(a_actor);
            if (!accepted || assigned != requested) {
                spdlog::warn(
                    "OBody: follower preset assignment for '{}' was not confirmed "
                    "(requested='{}', accepted={}, assigned='{}').",
                    actorName ? actorName : "?", requested.empty() ? "(none)" : requested,
                    accepted, assigned.empty() ? "(none)" : assigned);
            } else {
                spdlog::debug(
                    "OBody: follower preset '{}' applied to '{}'.",
                    assigned.empty() ? "(none)" : assigned, actorName ? actorName : "?");
            }
        }

        switch (DesiredORefit(a_actor)) {
            case 1: api->ForcefullyChangeORefitForActor(a_actor, true); break;
            case 2: api->ForcefullyChangeORefitForActor(a_actor, false); break;
            default: RestoreDefaultORefit(*api, a_actor); break;
        }

        const int desiredORefit = DesiredORefit(a_actor);
        if (desiredORefit != 0) {
            const bool applied = api->ActorHasORefitApplied(a_actor);
            const bool wanted  = desiredORefit == 1;
            if (applied != wanted) {
                spdlog::warn(
                    "OBody: follower ORefit for '{}' was not confirmed "
                    "(wanted={}, applied={}).",
                    actorName ? actorName : "?", wanted, applied);
            }
        }
    }

    void ReleaseOutfitORefit(RE::Actor* a_actor) {
        SetPolicyForActor(a_actor, 0);
        if (auto* api = Live(); api && a_actor) {
            RestoreDefaultORefit(*api, a_actor);
        }
    }

}  // namespace OS::ObodyApi
