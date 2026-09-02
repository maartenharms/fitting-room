#pragma once

namespace OS::CameraProbe {

    // OS-97, a TEMPORARY field probe for the actor-mannequin stint. It ships
    // nothing, gates nothing, and the whole module should be deleted once the
    // question below is answered.
    //
    // WHAT THE 2026-07-31 SPIKE ALREADY SETTLED, off disk, without a build:
    // the inventory "viewport" is the live world render driven by PlayerCamera,
    // not an offscreen character render. InventoryMenu's own constructor writes
    // menuFlags = 0xA481 as a literal on both runtimes (SE 1.5.97 0x14088CFDF,
    // AE 1.6.1170 0x14092C3BF), and neither kFreezeFrameBackground (0x20) nor
    // kRendersOffscreenTargets (0x1000) is in it. A whole-image census found
    // only JournalMenu sets the freeze bit and only BookMenu/MapMenu set the
    // offscreen bit. So framing a follower is a CAMERA problem.
    //
    // WHAT IS NOT SETTLED, and what this measures: whether the follower is
    // renderable at the moment we would frame them. Menu Studio's
    // Declutter::HideNearbyActors sweeps every high and middle-high actor and
    // SetAppCulled(true)s the lot, exempting only the player and their mount.
    // There is no teammate branch. In the 2026-07-30 field session it hid 8
    // actors in the same millisecond the menu opened. If the edit target is one
    // of them, retargeting the camera frames a culled actor in the dark and
    // every hour spent on the camera is wasted.
    //
    // READ ONLY, deliberately. Show Player In Inventory holds SmoothCam camera
    // control for the menu's lifetime, so a camera WRITE would make Fitting Room
    // the fourth writer on a camera three other mods already own. OS-80c is the
    // field-proven cost of being the second. That test is worth running, but
    // only after this one says there is something to frame.

    void Arm();     // editor opened - start the sample chain
    void Disarm();  // editor closed - stop it
    void Tick();    // once per editor frame, from EditorWindow::Draw (render thread)

}  // namespace OS::CameraProbe
