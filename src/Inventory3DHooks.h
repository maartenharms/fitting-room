#pragma once

// OS-154: hide the inventory's 3D item preview AT THE MOMENT IT IS CREATED,
// while the editor is open, so it is never drawn visible even once.
//
// ⚠ THIS IS THE THIRD TARGET TRIED AND THE FIRST TWO ARE WHY THIS COMMENT IS
// LONG. `UpdateItem3D` (51757) was hooked first and does nothing, being a
// four-line wrapper. `UpdateMagic3D` (51758) was hooked second and also changed
// nothing, because nothing loads while the editor is open: the field log showed
// zero refusals across five editor opens.
//
// The flash is not a load happening during the session, it is the LATENCY of
// noticing one. `EditorWindow::Draw` runs inside Present, at the END of the
// frame, after the UI-3D scene has already rendered, so nothing it does can be
// ahead of the draw. Field-confirmed 2026-08-08: the editor opened at
// 05:16:42.409, a model became visible 6.5 s later when the async load landed,
// and the user saw exactly one frame of it before the hide caught up.
//
// ⚠⚠ AND THE OBVIOUS TARGET, `Render` ITSELF, IS WRONG FOR A SUBTLE REASON.
// Render's body is: complete a pending load (which APPENDS AND UNHIDES A NEW
// MODEL), zoom update, transform, then the UI3DSceneManager scene pass. Hiding
// at the top of Render misses the model its own first step creates, and hiding
// after calling through is one frame too late. So the hook goes on the
// APPENDER, which is where a model becomes visible, is reached from both the
// hover path and the async completion, and always runs before the scene pass in
// the same frame.
namespace OS::Inventory3DHooks {

    // Install the appender entry detour. Call once from SKSEPluginLoad, after
    // SKSE::AllocTrampoline (SafetyHook does not use that trampoline; the
    // ordering just keeps every hook install in one place).
    void Install();

    // TRUE when the detour is live. False is a DEGRADED mode, not a broken one:
    // EditorWindow::Draw still hides the models reactively, one frame late.
    [[nodiscard]] bool Installed();

    // How many models this has hidden at creation. The counterpart to
    // EditorWindow's hide counter: this one climbing while THAT one stays
    // silent is the fix working.
    [[nodiscard]] std::uint32_t Hidden();

}  // namespace OS::Inventory3DHooks
