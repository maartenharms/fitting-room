#include "CameraProbe.h"

#include "EditorUI.h"

#include <algorithm>  // std::clamp
#include <atomic>
#include <cmath>      // std::acos, std::atan2, std::fabs, std::isnan, std::sqrt
#include <iterator>   // std::begin/std::end over the sample arrays

namespace OS::CameraProbe {

    namespace {

        // -1 == disarmed. Counts EDITOR frames, not game frames: the only window
        // that matters is the one where the editor is up over the inventory.
        std::atomic<int> g_frame{ -1 };

        // The open curve. A rising curve rather than a single shot, because SPII's
        // MenuCamera starts about 11 ms after the menu-open event (its own log:
        // menu opened 23:48:32.060, "[MenuCamera] Started" at .071) and Menu
        // Studio's declutter lands in the same millisecond. One sample at t0 would
        // record the pre-setup state as if it were the steady state.
        constexpr int kOpenSamples[] = { 1, 30, 90, 240 };

        // ...but the open curve alone is USELESS for the follower question, and
        // the 2026-07-31 01:17 field run proved it: four clean samples, every one
        // of them "no loaded NPC target". Frame 240 lands 1.37 s after the editor
        // opens, and nobody picks a follower out of a dropdown that fast.
        //
        // So the real trigger is the target CHANGING. Edge-detected on the raw
        // 32-bit handle, which costs one atomic compare on the render thread and
        // dereferences nothing; the resolve happens in the queued main-thread
        // task like every other sample. The delayed points after a switch matter
        // because switching target kicks a refresh that can re-cull.
        constexpr int kSwitchSamples[] = { 2, 60, 180 };

        std::atomic<std::uint32_t> g_lastTarget{ 0 };
        std::atomic<int>           g_switchFrame{ -1 };

        // A dropdown the user flips repeatedly must not flood a log that is
        // already 5000 lines of biped dump. Budget the whole editor session.
        std::atomic<int>  g_emitted{ 0 };
        constexpr int     kMaxSamples = 40;

        bool Due(const int a_n, const int* a_first, const int* a_last) {
            for (const int* p = a_first; p != a_last; ++p) {
                if (*p == a_n) {
                    return true;
                }
            }
            return false;
        }

        const char* Yn(bool a_b) { return a_b ? "yes" : "no"; }

        // Degrees off the camera's view axis, horizontally. THE metric for this
        // feature, and the one whose absence caused a wrong conclusion: a
        // follower at almost the same DISTANCE as the player was reported as
        // "already in the shot" when she was 110 degrees round the side. A
        // similar distance says nothing about bearing, and nobody notices that
        // until they compute it.
        //
        // The view axis is taken as camera -> whatever cameraTarget names,
        // because the third-person boom is built to point at that ref. Returns
        // a negative value when there is nothing to measure against.
        float BearingOffAxis(const RE::NiPoint3& a_cam, const RE::NiPoint3& a_axisTo,
                             const RE::NiPoint3& a_subject) {
            const float vx = a_axisTo.x - a_cam.x, vy = a_axisTo.y - a_cam.y;
            const float tx = a_subject.x - a_cam.x, ty = a_subject.y - a_cam.y;
            const float mv = std::sqrt(vx * vx + vy * vy);
            const float mt = std::sqrt(tx * tx + ty * ty);
            if (mv < 1.0f || mt < 1.0f) {
                return -1.0f;
            }
            float c = (vx * tx + vy * ty) / (mv * mt);
            c = std::clamp(c, -1.0f, 1.0f);
            return std::acos(c) * 57.2957795f;
        }

        // Degrees the camera sits ABOVE whatever it is pointing at. Positive is
        // looking DOWN.
        //
        // ⚠ THE METRIC THIS PROBE NEVER HAD, AND THE REASON THE HIGH-ANGLE BUG
        // WAS INVISIBLE TO IT. BearingOffAxis above is x/y only - it drops z
        // before it does anything else - so a shot with the bearing perfect and
        // the camera 79 degrees up in the air scored a clean pass and printed
        // "SHE IS THE SUBJECT". Recomputed by hand from the 2026-08-02 02:17
        // root/pos pairs, the four browses that session framed her at 79.4, 51.8,
        // 51.8 and 51.8 degrees down. The first of those IS the reported
        // "nearly top-down"; nothing in the log said so because nothing measured
        // the only axis it happens on.
        float ElevationDeg(const RE::NiPoint3& a_cam, const RE::NiPoint3& a_at) {
            const float dx = a_at.x - a_cam.x;
            const float dy = a_at.y - a_cam.y;
            const float dz = a_at.z - a_cam.z;
            const float run = std::sqrt(dx * dx + dy * dy);
            if (run < 0.01f && std::fabs(dz) < 0.01f) {
                return 0.0f;
            }
            // dz negative (camera above the subject) reads as a POSITIVE
            // downward angle, which is the direction a reader expects to see
            // grow as the shot gets worse.
            return -std::atan2(dz, run) * 57.2957795f;
        }

        // Walk UP from a node and report the first culled ancestor. A culled
        // parent hides the actor just as completely as a culled root, and the
        // parent chain is a plain NiAVObject member (+0x030), so this costs no
        // RTTI and no relocation. Bounded so a malformed graph cannot stall the
        // main thread inside a diagnostic.
        int CulledAncestorDepth(RE::NiAVObject* a_node) {
            int depth = 0;
            for (auto* p = a_node ? a_node->parent : nullptr; p && depth < 8;
                 p = p->parent) {
                ++depth;
                if (p->GetAppCulled()) {
                    return depth;
                }
            }
            return 0;  // 0 == no culled ancestor found within the walk
        }

        void Sample(const char* a_when, int a_frame) {
            auto* cam    = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!cam || !player) {
                spdlog::info("[probe/cam] {} f={} no PlayerCamera/PlayerCharacter.", a_when,
                             a_frame);
                return;
            }

            const int state = cam->currentState ? static_cast<int>(cam->currentState->id) : -1;
            const RE::NiPoint3 camPos =
                cam->cameraRoot ? cam->cameraRoot->world.translate : RE::NiPoint3{};
            const RE::NiPoint3 playerPos = player->GetPosition();

            // cameraTarget is the boom's world anchor, and logging who it names is
            // the point of this line. The third-person position builder (AE id
            // 50911) resolves PlayerCamera+0x3C through the handle table in its
            // first six instructions and offsets from THAT ref's world translate -
            // it does not read the PlayerCharacter singleton for the anchor. That
            // is why retargeting is an engine-native write rather than a hack, and
            // it is the field a follower handle would eventually go into.
            const auto  targetPtr  = cam->cameraTarget.get();
            const char* targetName = targetPtr ? targetPtr->GetName() : "(none)";

            spdlog::info(
                "[probe/cam] {} f={} camera: state={} fov={:.2f} root=({:.1f},{:.1f},{:.1f}) "
                "camTarget='{}' 0x{:08X} player=({:.1f},{:.1f},{:.1f}) camDistPlayer={:.1f}",
                a_when, a_frame, state, cam->GetRuntimeData2().worldFOV, camPos.x, camPos.y, camPos.z, targetName,
                targetPtr ? targetPtr->GetFormID() : 0u, playerPos.x, playerPos.y, playerPos.z,
                camPos.GetDistance(playerPos));

            // The player's own cull state is the control. Menu Studio exempts the
            // player, so this must read no. If it ever reads yes, the NPC line
            // below means nothing and the reading is void.
            if (auto* p3d = player->Get3D()) {
                spdlog::info("[probe/cam] {} f={} control: player 3d=yes appCulled={}.", a_when,
                             a_frame, Yn(p3d->GetAppCulled()));
            } else {
                spdlog::info("[probe/cam] {} f={} control: player has NO 3D.", a_when, a_frame);
            }

            const auto handle = OS::EditorUI::CurrentNpcTargetHandle();
            const auto npcPtr = handle.get();
            if (!npcPtr) {
                spdlog::info("[probe/cam] {} f={} npc: no loaded NPC target. Pick a follower in "
                             "the Editing: dropdown; the switch itself re-samples, so there is "
                             "no need to hurry.",
                             a_when, a_frame);
                return;
            }

            auto* n3d = npcPtr->Get3D();
            if (!n3d) {
                spdlog::info("[probe/cam] {} f={} npc='{}' 0x{:08X} has NO 3D - not rendered at "
                             "all, so there is nothing to frame.",
                             a_when, a_frame, npcPtr->GetName(), npcPtr->GetFormID());
                return;
            }

            // appCulled on the ROOT is the whole answer to the blocker question:
            // Menu Studio's Declutter::HideLoadedRef sets exactly this flag on
            // exactly this node (a_ref->Get3D()), and exempts only the player and
            // the mount. appCulled=yes here names the blocker on the spot.
            const RE::NiPoint3 npcPos      = npcPtr->GetPosition();
            const int          culledAbove = CulledAncestorDepth(n3d);
            // ⚠ MEASURE THE SEPARATION, NOT THE OFF-AXIS BEARING. The obvious
            // metric is "how far is she off the camera's view axis", taking the
            // axis as camera -> cameraTarget. That is exactly what was measured
            // to prove she was 110 degrees round the side - and it SELF-
            // INVALIDATES the moment the retarget works, because cameraTarget
            // then IS her and the angle is zero by construction. It would report
            // success for a shot that had merely stopped being measured.
            //
            // The separation between the two subjects as seen from the camera
            // needs no view convention, stays meaningful whichever one the boom
            // is anchored to, and answers the real question: can both fit in one
            // frame at this FOV.
            const float sep = BearingOffAxis(camPos, playerPos, npcPos);
            const bool  onHer = targetPtr && targetPtr.get() == npcPtr.get();
            spdlog::info(
                "[probe/cam] {} f={} npc='{}' 0x{:08X} 3d=yes appCulled={} culledAncestor={} "
                "pos=({:.1f},{:.1f},{:.1f}) distPlayer={:.1f} distCam={:.1f} "
                "sepFromPlayer={:.1f}deg (fov {:.0f}) boomOn={} => {}",
                a_when, a_frame, npcPtr->GetName(), npcPtr->GetFormID(),
                Yn(n3d->GetAppCulled()), culledAbove, npcPos.x, npcPos.y, npcPos.z,
                npcPos.GetDistance(playerPos), npcPos.GetDistance(camPos), sep,
                cam->GetRuntimeData2().worldFOV, onHer ? "her" : "the player",
                sep < 0.0f                        ? "UNMEASURABLE"
                : onHer                           ? "SHE IS THE SUBJECT"
                : sep <= cam->GetRuntimeData2().worldFOV * 0.5f ? "BOTH IN FRAME"
                                                  : "SHE IS OUT OF FRAME");

            // ── The shot, on the axis nobody was measuring ────────────────────
            //
            // Two aim points because they answer different halves. The actor
            // ORIGIN is her feet, which is what distCam above has always used;
            // the world bound CENTRE is roughly mid-body and is what a viewer
            // would call "the shot". Steep on both is a camera genuinely
            // overhead rather than one merely aimed low at a tall subject.
            const RE::NiPoint3 mid = n3d->worldBound.center;
            const float dxm = mid.x - camPos.x;
            const float dym = mid.y - camPos.y;
            const float run = std::sqrt(dxm * dxm + dym * dym);
            spdlog::info(
                "[probe/cam] {} f={} shot: elevToFeet={:.1f}deg elevToMid={:.1f}deg "
                "(positive = looking DOWN) horizRun={:.1f} rise={:.1f} boom={:.1f} "
                "midBound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
                a_when, a_frame, ElevationDeg(camPos, npcPos), ElevationDeg(camPos, mid), run,
                camPos.z - mid.z, mid.GetDistance(camPos), mid.x, mid.y, mid.z,
                n3d->worldBound.radius);

            // WHICH TERM IS STEEP. CameraFrame.h already predicts the shape of
            // the answer - "the over-shoulder block and the zoom scaling inside
            // the same builder read the PlayerCharacter singleton regardless of
            // cameraTarget" - but predicting is not measuring, and FOUR
            // candidates produce the same overhead shot: free-look pitch, the
            // zoom/boom length, the over-shoulder offset, and the collision
            // sweep riding the boom up a wall. Each has its own field here, so
            // one browse NAMES the term instead of narrowing it.
            //
            // ⚠ READ ONLY, deliberately, and this is the file where that
            // matters most: we are already the fourth writer on this camera and
            // OS-80c is the recorded cost of being the second. Nothing below
            // assigns to anything.
            auto* const stateRaw = cam->currentState.get();
            if (stateRaw && stateRaw->id == RE::CameraState::kThirdPerson) {
                auto* const tps = static_cast<RE::ThirdPersonState*>(stateRaw);
                // collisionPosValid is NaN when the sweep did not resolve, per
                // the member's own contract. NaN means the position came
                // straight out of the builder and the pitch is ours to explain;
                // a number means a wall moved the camera and no offset change
                // will fix it.
                const bool collided = !std::isnan(tps->collisionPosValid);
                spdlog::info(
                    "[probe/cam] {} f={} tps: pitch={:.1f}deg yaw={:.1f}deg freeRot={} "
                    "zoomCur={:.3f} zoomTarget={:.3f} pitchZoom={:.2f} "
                    "offsetActual=({:.1f},{:.1f},{:.1f}) offsetExpected=({:.1f},{:.1f},{:.1f}) "
                    "applyOffsets={} collisionResolved={} collisionPos=({:.1f},{:.1f},{:.1f})",
                    a_when, a_frame, tps->freeRotation.x * 57.2957795f,
                    tps->freeRotation.y * 57.2957795f, Yn(tps->freeRotationEnabled),
                    tps->currentZoomOffset, tps->targetZoomOffset, tps->pitchZoomOffset,
                    tps->posOffsetActual.x, tps->posOffsetActual.y, tps->posOffsetActual.z,
                    tps->posOffsetExpected.x, tps->posOffsetExpected.y, tps->posOffsetExpected.z,
                    Yn(tps->applyOffsets), Yn(collided), tps->collisionPos.x, tps->collisionPos.y,
                    tps->collisionPos.z);
            } else {
                spdlog::info("[probe/cam] {} f={} tps: camera state is {}, not kThirdPerson(9) - "
                             "the boom terms do not apply.",
                             a_when, a_frame, state);
            }
        }

    }  // namespace

    void Arm() {
        g_frame.store(0, std::memory_order_relaxed);
        g_switchFrame.store(-1, std::memory_order_relaxed);
        g_emitted.store(0, std::memory_order_relaxed);
        // Cleared to 0, which is also what the player target reads, so the editor
        // opening on the player is correctly a non-event. Leaving the previous
        // session's target latched here would fire a spurious switch on frame 1.
        g_lastTarget.store(0, std::memory_order_relaxed);
    }

    void Disarm() { g_frame.store(-1, std::memory_order_relaxed); }

    // Queue one sample onto the main thread. Draw runs on the render thread, and
    // everything Sample touches (actor 3D, camera state, a handle-table resolve)
    // is engine state owned by the main thread, so this queues exactly as the
    // Inventory3DManager re-clear a few lines above the Tick() call site does.
    namespace {
        void Emit(const char* a_when, int a_n) {
            if (g_emitted.fetch_add(1, std::memory_order_relaxed) >= kMaxSamples) {
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_when, a_n] { Sample(a_when, a_n); });
            }
        }
    }  // namespace

    void Tick() {
        const int prev = g_frame.load(std::memory_order_relaxed);
        if (prev < 0) {
            return;
        }
        const int n = prev + 1;
        g_frame.store(n, std::memory_order_relaxed);

        // The target-change edge, and the reason this probe works at all. Reading
        // the raw 32-bit handle dereferences nothing, so it is safe from the
        // render thread; the resolve happens inside Emit's queued task.
        const std::uint32_t cur =
            OS::EditorUI::CurrentNpcTargetHandle().native_handle();
        const std::uint32_t last = g_lastTarget.exchange(cur, std::memory_order_relaxed);
        if (cur != last) {
            g_switchFrame.store(0, std::memory_order_relaxed);
            if (cur != 0) {
                Emit("switch", 0);  // the moment it changed
            }
        }

        const int sPrev = g_switchFrame.load(std::memory_order_relaxed);
        if (sPrev >= 0) {
            const int s = sPrev + 1;
            g_switchFrame.store(s, std::memory_order_relaxed);
            if (cur != 0 &&
                Due(s, std::begin(kSwitchSamples), std::end(kSwitchSamples))) {
                Emit("switch", s);
            }
        }

        if (Due(n, std::begin(kOpenSamples), std::end(kOpenSamples))) {
            Emit("open", n);
        }
    }

}  // namespace OS::CameraProbe
