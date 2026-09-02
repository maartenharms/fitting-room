#pragma once

// Task 1 of the GPU diffuse dye spike, OS-139.
// Spec: docs/superpowers/specs/2026-08-05-gpu-diffuse-dye-spike.md
//
// ⚠ THIS WHOLE MODULE IS A PROBE AND IS MEANT TO BE DELETED. It ships behind
// [Debug] iDyeGpuTask, which is 0 in every INI that is not a spike run. It
// answers one question, "does a compute dispatch run at all on the game's own
// device", and it answers it with a negative control because this branch has
// twice believed a result that turned out to be the machinery moving on its
// own.
//
// ⚠ WHAT THE SPEC GOT WRONG, CORRECTED HERE. The spec says the device is
// obtained from the swap chain. It is not. `ImGuiOverlay::EnsureInit` reads
// `RE::BSRenderManager::GetSingleton()->GetRuntimeData()` and takes `.forwarder`
// as the device and `.context` as the immediate context; only the HWND comes
// off the swap chain. That is better than the spec assumed, because it means
// NOTHING here depends on ImGuiOverlay and no accessor had to be added to it.
//
// ⚠ WHAT STILL CONSTRAINS US IS THE THREAD, NOT THE DEVICE. `.context` is the
// IMMEDIATE context and D3D11 immediate contexts are not thread safe. Touching
// it from the main thread while the render thread is drawing is a race whose
// symptom is a corrupted frame or a crash, not an error return. Every entry
// point below must therefore be called from the render thread.
//
// ⚠ THE RENDER THREAD IS `EditorIWindow::Draw`, NOT `ImGuiOverlay`. This probe
// was first called from `ImGuiOverlay::PresentThunk`, which looks exactly right
// and never executes: `ImGuiOverlay` installs its own Present hook from its own
// `Toggle`, and nothing outside `ImGuiOverlay.cpp` has called that since
// `ddb5c4a` made the editor a FUCK IWindow. The whole class is dead code. The
// probe logged NOTHING across a session where the editor was opened four times,
// which is what a dead call site looks like from a log rather than an error.
// FUCK calls `Draw` from its own Present hook, so that is the live render
// thread and the only place this belongs.

namespace OS::DyeGpu {

    // Runs the configured probe task ONCE and logs the result. Safe to call
    // every frame; it self-disables after the first run.
    //
    // ⚠ RENDER THREAD ONLY. See the header comment.
    void RunProbeOnce();

}  // namespace OS::DyeGpu
