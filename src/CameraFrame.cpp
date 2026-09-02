#include "CameraFrame.h"

#include "Settings.h"

#include <atomic>

namespace OS::CameraFrame {

    namespace {

        // ── State, and which thread owns each piece ──────────────────────────
        //
        // Requests arrive on the RENDER thread (the editor lives inside FLICK's
        // Present hook). Every engine touch happens in a queued task on the MAIN
        // thread. Nothing below resolves a handle or reads camera state outside
        // that task, which is the rule MenuStudioApi already documents and which
        // an earlier version of this file broke by resolving the subject at the
        // call site.

        // Bumped by EVERY request. A queued task carries the epoch it was born
        // with and does nothing if the world has moved on.
        //
        // ⚠ THIS IS WHAT STOPS A RE-ASSERT LANDING AFTER A RELEASE. Without it
        // the sequence "pick a follower, close the editor a frame later" can
        // leave a stale Queue(subject) task behind a Queue(0) release task, and
        // the camera ends up on the follower with nothing pending to correct it
        // and the editor shut. That is the worst failure this feature has, and
        // it is a plain ordering bug rather than anything exotic.
        std::atomic<std::uint32_t> g_epoch{ 0 };

        // The subject's reference FormID, published by the task once it has
        // resolved, and read by Tick to know what to re-assert. Zero means "the
        // camera is ours to give back", not "the player".
        std::atomic<std::uint32_t> g_subject{ 0 };

        // Whether we actually hold the camera. Set inside the task AFTER a
        // successful write, never at request time: claiming it earlier meant a
        // request that never resulted in a write still made the release stomp
        // another mod's value on the way out.
        std::atomic<bool> g_held{ false };

        // What cameraTarget held before we first took it. Restoring THIS rather
        // than forcing the player is the difference between correct and merely
        // usually-correct: mounted, the engine puts the HORSE here (AE id 50821)
        // and does not put it back until dismount. Main thread only, so a plain
        // value is right; BSPointerHandle copies by value over a trivially
        // copyable 32-bit member, so this calls into nothing.
        RE::ActorHandle g_previous{};

        // Re-assert cadence. Show Player In Inventory writes cameraTarget once
        // at menu open rather than per frame, so a slow re-assert wins that race
        // without turning this into a per-frame fight over a shared field.
        constexpr int kReassertEvery = 30;
        std::atomic<int> g_tick{ 0 };

        // ── The only code that touches the engine ────────────────────────────

        void Restore(RE::PlayerCamera* a_cam, bool a_log) {
            if (!g_held.exchange(false, std::memory_order_relaxed)) {
                return;  // never took it, so write nothing at all
            }
            a_cam->cameraTarget = g_previous;
            const auto back = g_previous.get();
            g_previous = RE::ActorHandle{};
            if (a_log) {
                spdlog::info("[frame] camera handed back to '{}'.",
                             back ? back->GetName() : "(its previous target)");
            }
        }

        // Main thread. a_actor null means "give it back".
        void ApplyResolved(RE::Actor* a_actor, std::uint32_t a_epoch, bool a_log) {
            if (a_epoch != g_epoch.load(std::memory_order_relaxed)) {
                return;  // superseded while queued
            }
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) {
                return;
            }
            auto* const ptr = a_actor;
            if (!ptr) {
                Restore(cam, a_log);
                g_subject.store(0, std::memory_order_relaxed);
                return;
            }
            const RE::ActorHandle a_wanted = ptr->GetHandle();
            // Loaded is not enough: an actor with no 3D is not on screen, and
            // pointing the boom at her leaves the shot staring at nothing.
            // Reachable mid-cell-load or straight through a load door.
            //
            // ⚠ LEAVE THE CAMERA ALONE HERE rather than asserting the player. An
            // earlier version wrote the player in this branch, so the path whose
            // entire purpose is "do not frame her" stomped another mod's value,
            // and did it again on every re-assert while that target stayed
            // selected.
            if (!ptr->Get3D()) {
                if (a_log) {
                    spdlog::info("[frame] '{}' 0x{:08X} has no 3D yet - leaving the "
                                 "camera as it is.",
                                 ptr->GetName(), ptr->GetFormID());
                }
                return;
            }
            if (!g_held.exchange(true, std::memory_order_relaxed)) {
                g_previous = cam->cameraTarget;  // first take: remember the real value
            }
            g_subject.store(ptr->GetFormID(), std::memory_order_relaxed);

            if (const auto before = cam->cameraTarget.get(); before && before.get() == ptr) {
                return;  // already ours; writing again observes nothing
            }
            cam->cameraTarget = a_wanted;

            // The readback, not the write. Whether the field HOLDS what we put
            // in it is the entire open question, because three other mods write
            // it and one of them owns SmoothCam for the menu's lifetime.
            const auto back = cam->cameraTarget.get();
            const bool held = back && back.get() == ptr;
            if (a_log || !held) {
                spdlog::info("[frame] retargeted to '{}' 0x{:08X}; readback names "
                             "'{}' - {}.",
                             ptr->GetName(), ptr->GetFormID(),
                             back ? back->GetName() : "(none)",
                             held ? "held" : "STOMPED");
            }
        }

        // Both queue helpers resolve INSIDE the task. Nothing here dereferences
        // a handle or a form id on the calling (render) thread.
        void QueueHandle(RE::ActorHandle a_wanted, bool a_log) {
            const std::uint32_t epoch = g_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_wanted, epoch, a_log] {
                    const auto ptr = a_wanted.get();
                    ApplyResolved(ptr ? ptr.get() : nullptr, epoch, a_log);
                });
            }
        }

        void QueueById(std::uint32_t a_formID, bool a_log) {
            const std::uint32_t epoch = g_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_formID, epoch, a_log] {
                    auto* form = a_formID ? RE::TESForm::LookupByID(a_formID) : nullptr;
                    ApplyResolved(form ? form->As<RE::Actor>() : nullptr, epoch, a_log);
                });
            }
        }

    }  // namespace

    void SetSubject(RE::ActorHandle a_actor) {
        // The kill switch is checked on the way IN only, so a user who turns it
        // off mid-session still gets the release below for whatever is held.
        if (!Settings::GetSingleton().cameraFrameTarget) {
            return;
        }
        g_tick.store(0, std::memory_order_relaxed);
        // The handle travels by value and resolves inside the task. A handle
        // also FAILS SAFE across a game load, which is why the request carries
        // one even though the re-assert has to fall back to a form id.
        QueueHandle(a_actor, true);
    }

    void Tick() {
        if (g_subject.load(std::memory_order_relaxed) == 0) {
            return;
        }
        // Honour the kill switch mid-session rather than only at the request.
        // Turning it off should hand the camera back, not merely stop taking it
        // again next time.
        if (!Settings::GetSingleton().cameraFrameTarget) {
            Release();
            return;
        }
        const int n = g_tick.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % kReassertEvery != 0) {
            return;
        }
        // Log only the first re-assert after a change: a held camera stays
        // silent, a stomped one always logs (ApplyResolved forces it), and the
        // logger flushes every line.
        QueueById(g_subject.load(std::memory_order_relaxed), n == kReassertEvery);
    }

    void Release() {
        g_subject.store(0, std::memory_order_relaxed);
        g_tick.store(0, std::memory_order_relaxed);
        // Always queue, and let the task decide. Bumping the epoch here is what
        // kills any re-assert still in flight.
        QueueById(0, true);
    }

    bool Held() { return g_held.load(std::memory_order_relaxed); }

}  // namespace OS::CameraFrame
