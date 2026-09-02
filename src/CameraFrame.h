#pragma once

namespace OS::CameraFrame {

    // OS-97. Point the world camera at the follower being edited.
    //
    // WHY THIS EXISTS, AND WHY IT IS NOT WHAT WAS FIRST ASSUMED. The 2026-07-31
    // spike settled that the inventory viewport is the live world render driven
    // by PlayerCamera, and the follow-up work made Menu Studio stop culling the
    // edit target, confirmed by log line (appCulled went yes to no). She then
    // still could not be seen, and the reason was measured rather than guessed:
    //
    //   camera  (-171.6, -550.7)
    //   player  (-128.7, -710.1)   165 units, dead ahead
    //   Jenassa (-284.0, -540.5)   113 units, 110 degrees off the view axis
    //
    // She was behind the camera. An earlier run put her at 64 degrees, also
    // outside the 60-degree frustum. "Visible" and "in shot" are different
    // problems and only the first had been solved. A similar camera DISTANCE
    // was mistaken for being in frame; distance says nothing about bearing.
    //
    // HOW. The third-person position builder (AE id 50911) resolves
    // PlayerCamera::cameraTarget - an ActorHandle at +0x3C - through the handle
    // table in its first six instructions, and offsets the boom from THAT ref's
    // world translate. It does not read the PlayerCharacter singleton for the
    // anchor. So retargeting is an engine-native write into the field the mount
    // path already uses, not a hack.
    //
    // ⚠ ThirdPersonState::SetCameraHandle is a `ret 0` stub on AE. The retarget
    // must go through PlayerCamera::cameraTarget and never through the state.
    //
    // ⚠ WE ARE THE FOURTH WRITER on this camera. Show Player In Inventory sets
    // cameraTarget on every menu open and holds SmoothCam camera control for the
    // menu's lifetime; Menu Studio's CameraGate bypasses the obstruction query;
    // three more writers of the setter exist in the binary and are still
    // uncharacterised. That is why Reassert exists and why every exit path
    // restores. Settings::cameraFrameTarget is the kill switch.
    //
    // ⚠ THE FRAMING OFFSETS STAY PLAYER-GOVERNED. The over-shoulder block and
    // the zoom scaling inside the same builder read the PlayerCharacter
    // singleton regardless of cameraTarget. The shot moves onto the follower;
    // how tightly it is framed still follows player state. Polish, not
    // architecture, but it is why the result may look loose.

    // Who the shot should be about. Empty hands the camera back to the player.
    // Safe to call from any thread; the engine write is queued to the main one.
    void SetSubject(RE::ActorHandle a_actor);

    // Per editor frame, from EditorWindow::Draw. Cheap when idle.
    void Tick();

    // Hand the camera back to whatever held it before, and forget the subject.
    // Idempotent, and a no-op when nothing was ever retargeted.
    //
    // ⚠ MUST run on EVERY exit, including the ones that are not an editor close:
    // the host inventory menu closing underneath the editor, and kPreLoadGame.
    // Without those the editor stays nominally open, Tick keeps re-asserting,
    // and the player has gameplay control with the world camera nailed to a
    // follower - a state the re-assert makes unrecoverable rather than
    // transient. HostGuard owns those two paths.
    void Release();

    // Are we currently holding the camera? For the backstop, so it can tell
    // "nothing to release" from "released" without writing anything.
    [[nodiscard]] bool Held();

}  // namespace OS::CameraFrame
