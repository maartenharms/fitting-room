#include "NodeTransformApi.h"

#include <cstdint>

// RaceMenu's public modder header uses opaque global Skyrim type declarations.
// Keep it in this one translation unit and bridge the actor pointer at the call
// boundary, exactly as RaceMenuMorphApi does; no RaceMenu type escapes through
// our header.
#include "../extern/RaceMenu/IPluginInterface.h"

namespace OS::NodeTransformApi {

    namespace {

        INiTransformInterface* g_transforms{ nullptr };

        [[nodiscard]] TESObjectREFR* AsRaceMenuRef(RE::Actor* a_actor) {
            return reinterpret_cast<TESObjectREFR*>(a_actor);
        }

        // ⚠ THIRD PERSON ONLY, AND THAT IS NOT AN OVERSIGHT. The interface
        // stores first- and third-person overrides separately, and the only
        // bones a first-person view shows are the hands. Writing both would
        // double every call to change a pair of hands nobody is looking at from
        // the outside, and it is the sort of thing that gets noticed as "my
        // arms are wrong in first person" long after the fact. If first-person
        // hands are ever wanted they are a deliberate second pass.
        constexpr bool kThirdPerson = false;

        [[nodiscard]] bool IsFemaleActor(RE::Actor* a_actor) {
            auto* const base = a_actor ? a_actor->GetActorBase() : nullptr;
            return base && base->IsFemale();
        }

    }  // namespace

    namespace {
        Status        g_status{ Status::kNotRequested };
        std::uint32_t g_refusedVersion{ 0 };
    }  // namespace

    void Request() {
        if (g_transforms) {
            return;
        }
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            g_status = Status::kNoMessaging;
            spdlog::error("Shape: SKSE messaging unavailable; RaceMenu not acquired.");
            return;
        }

        InterfaceExchangeMessage exchange{};
        messaging->Dispatch(
            static_cast<std::uint32_t>(InterfaceExchangeMessage::kMessage_ExchangeInterface),
            &exchange, sizeof(exchange), "skee");
        if (!exchange.interfaceMap) {
            g_status = Status::kRaceMenuAbsent;
            spdlog::warn("Shape: RaceMenu interface exchange got no map; the Shape "
                         "page is unavailable this session.");
            return;
        }
        auto* candidate = static_cast<INiTransformInterface*>(
            exchange.interfaceMap->QueryInterface("NiTransform"));
        if (!candidate) {
            g_status = Status::kNoNiTransform;
            spdlog::warn("Shape: RaceMenu has no NiTransform interface.");
            return;
        }
        const auto version = candidate->GetVersion();
        if (version < INiTransformInterface::kCurrentPluginVersion) {
            g_status         = Status::kTooOld;
            g_refusedVersion = version;
            // ⚠⚠ THIS IS THE SE 1.5.97 REFUSAL and the log line names it.
            // RaceMenu's last Special Edition build is 0.4.16, uploaded
            // 2020-10-04; the Anniversary line has moved on ever since and this
            // floor is whatever the AE build happens to report. A player on SE
            // has no newer RaceMenu to install, so the page has to say which
            // number was wanted rather than tell them to go and update.
            spdlog::error("Shape: RaceMenu NiTransform v{} is older than the v{} this "
                          "build was written against; no transform calls will be made.",
                          version,
                          static_cast<std::uint32_t>(
                              INiTransformInterface::kCurrentPluginVersion));
            return;
        }
        g_status     = Status::kReady;
        g_transforms = candidate;
        spdlog::info("Shape: RaceMenu NiTransform v{} acquired; owned key='{}'.", version,
                     ShapeOverlay::kOwnedKey);
    }

    bool Available() { return g_transforms != nullptr; }

    Status GetStatus() { return g_status; }

    std::uint32_t RefusedVersion() { return g_refusedVersion; }

    // ⚠ THE PAGE SAID "RaceMenu is not loaded" FOR EVERY ONE OF THESE, which
    // was true for one of them. See the note on Status.
    const char* UnavailableKey() {
        switch (g_status) {
            case Status::kReady:
                return "";
            case Status::kNotRequested:
            case Status::kNoMessaging:
            case Status::kRaceMenuAbsent:
                return "$FR_Shape_NoRaceMenu";
            case Status::kNoNiTransform:
                return "$FR_Shape_TransformsMissing";
            case Status::kTooOld:
                return "$FR_Shape_TransformsOld";
        }
        return "$FR_Shape_NoRaceMenu";
    }

    std::uint32_t Version() { return g_transforms ? g_transforms->GetVersion() : 0u; }

    ApplyResult Apply(RE::Actor*                                   a_actor,
                      const std::vector<ShapeOverlay::Adjustment>& a_plan) {
        ApplyResult result;
        if (!g_transforms || !a_actor) {
            return result;
        }
        result.available   = true;
        result.actorLoaded = a_actor->Is3DLoaded();
        for (const auto& adj : a_plan) {
            if (adj.remove) {
                ++result.bonesCleared;
            } else {
                ++result.bonesWritten;
            }
        }

        // ⚠ THE HANDLE TRAVELS, NOT THE POINTER, and it resolves inside the
        // task. This is reached from the render thread (the editor draws
        // through FUCK's Present hook), and UpdateNodeAllTransforms walks the
        // scenegraph. A handle also fails safe across a game load, where a raw
        // pointer captured now would be stale by the time the task ran.
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return result;
        }
        auto plan = a_plan;
        task->AddTask([handle, plan = std::move(plan)] {
            const auto ptr   = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            if (!actor || !g_transforms) {
                return;
            }
            auto* const  refr     = AsRaceMenuRef(actor);
            const bool   isFemale = IsFemaleActor(actor);
            for (const auto& adj : plan) {
                if (adj.remove) {
                    // ⚠ REMOVE THE SCALE, NOT THE WHOLE NODE ENTRY.
                    // RemoveNodeTransform drops position and rotation under our
                    // key as well, which today is the same thing because scale
                    // is all we write. It stops being the same thing the moment
                    // a position control is added, and this is the line nobody
                    // would think to revisit then.
                    g_transforms->RemoveNodeTransformScale(refr, kThirdPerson, isFemale,
                                                           adj.bone.c_str(),
                                                           ShapeOverlay::kOwnedKey);
                } else {
                    g_transforms->AddNodeTransformScale(refr, kThirdPerson, isFemale,
                                                        adj.bone.c_str(),
                                                        ShapeOverlay::kOwnedKey, adj.scale);
                }
            }
            // ⚠ ONE UPDATE AFTER THE WHOLE BATCH, never one per bone. Same rule
            // RaceMenuMorphApi::ApplyOwned holds for its single refresh: the
            // per-item version costs a scenegraph walk for each of twenty bones
            // and shows the character mid-way through its own edit.
            g_transforms->UpdateNodeAllTransforms(refr);
        });
        result.refreshed = result.actorLoaded;
        return result;
    }

    ApplyResult ClearOwned(RE::Actor* a_actor) {
        std::vector<ShapeOverlay::Adjustment> plan;
        plan.reserve(ShapeOverlay::kSliderCount * 2);
        for (const auto& slider : ShapeOverlay::kSliders) {
            for (const char* bone : slider.bones) {
                if (!bone) {
                    continue;
                }
                ShapeOverlay::Adjustment adj;
                adj.bone   = bone;
                adj.remove = true;
                plan.push_back(std::move(adj));
            }
        }
        return Apply(a_actor, plan);
    }

    std::vector<ShapeOverlay::Reading> Read(RE::Actor* a_actor) {
        std::vector<ShapeOverlay::Reading> readings;
        if (!g_transforms || !a_actor) {
            return readings;
        }
        auto* const  refr     = AsRaceMenuRef(a_actor);
        const bool   isFemale = IsFemaleActor(a_actor);
        readings.reserve(ShapeOverlay::kSliderCount * 2);
        for (const auto& slider : ShapeOverlay::kSliders) {
            for (const char* bone : slider.bones) {
                if (!bone) {
                    continue;
                }
                ShapeOverlay::Reading r;
                r.bone = bone;
                // ⚠ ASK Has BEFORE Get. GetNodeTransformScale on a bone with no
                // override under our key returns whatever the implementation
                // has to hand rather than a documented default, so reading it
                // unguarded is how every unshaped character would come back
                // holding a number.
                r.present = g_transforms->HasNodeTransformScale(
                    refr, kThirdPerson, isFemale, bone, ShapeOverlay::kOwnedKey);
                if (r.present) {
                    r.scale = g_transforms->GetNodeTransformScale(
                        refr, kThirdPerson, isFemale, bone, ShapeOverlay::kOwnedKey);
                }
                readings.push_back(std::move(r));
            }
        }
        return readings;
    }

}  // namespace OS::NodeTransformApi
