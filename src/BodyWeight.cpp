#include "BodyWeight.h"

#include "BodyStudioProof.h"
#include "OutfitSession.h"

#include <spdlog/spdlog.h>

namespace OS::BodyWeight {

    namespace {

        [[nodiscard]] RE::TESNPC* BaseOf(RE::Actor* a_actor) {
            return a_actor ? a_actor->GetActorBase() : nullptr;
        }

    }  // namespace

    float Of(RE::Actor* a_actor) {
        if (auto* base = BaseOf(a_actor)) {
            return base->GetWeight();
        }
        return 0.0f;
    }

    bool CanEdit(RE::Actor* a_actor) {
        auto* base = BaseOf(a_actor);
        return base && base->IsUnique();
    }

    void Set(RE::Actor* a_actor, float a_weight) {
        if (!a_actor || !CanEdit(a_actor)) {
            return;
        }
        // Clamped here rather than trusted from the control, because this is
        // also the entry point any future caller will use and the engine takes
        // whatever it is handed.
        const float weight = a_weight < 0.0f ? 0.0f : (a_weight > 100.0f ? 100.0f : a_weight);

        // ⚠ THE HANDLE TRAVELS, NOT THE POINTER, and it resolves inside the
        // task. This is called from the render thread (the editor draws through
        // FUCK's Present hook), so nothing here may dereference an actor, and a
        // handle also fails safe across a game load.
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([handle, weight] {
            const auto ptr = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            auto*      base  = actor ? actor->GetActorBase() : nullptr;
            if (!base || !base->IsUnique()) {
                return;
            }
            if (base->weight == weight) {
                return;  // nothing moved; do not pay for a refresh
            }
            const float was = base->weight;
            base->weight    = weight;

            // ⚠ THIS WRITES THE VALUE AND NOTHING ELSE, AND THE RE-APPLY IS THE
            // CALLER'S. It used to call BodyStudioProof::QueueRefreshAndReassert
            // from here, which was wrong twice over.
            //
            // It only reaches an ACTIVE CUSTOM body: a character wearing an
            // INSTALLED OBody preset has no entry in Body Studio's active map,
            // so it returned early and nothing was rebuilt. Field 2026-08-06:
            // the weight wrote every time and the shape only caught up when the
            // player switched preset and back, which is the normal apply path
            // running afterwards and picking up the new value.
            //
            // And knowing WHICH body to put back means reading the staged
            // outfit, which is editor state this module has no business
            // touching. EditorUI::RestoreStagedBody already answers it for both
            // kinds, so the caller re-applies and this stays a setter.
            //
            // ⚠ THE ORDER IS FIFO AND THAT IS WHAT MAKES IT SAFE. Both this
            // write and the caller's re-apply go through the SKSE task queue on
            // the main thread, this one queued first, so the rebuild reads the
            // weight already written.
            spdlog::info("BodyWeight: '{}' weight {:.1f} -> {:.1f}.",
                         actor->GetName() ? actor->GetName() : "(unnamed)", was, weight);
        });
    }

}  // namespace OS::BodyWeight
