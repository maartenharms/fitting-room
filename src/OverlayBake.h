#pragma once

#include "PCH.h"

#include "OverlayTransform.h"

#include <cstdint>
#include <functional>
#include <string>

// OS-209: baking an overlay at a transform, so the offset sliders can exist.
//
// ---- WHY A BAKE AND NOT A MATERIAL WRITE ----------------------------------
//
// ⚠⚠ MEASURED 2026-08-18: no skee override key writes BSShaderMaterial::
// texCoordOffset (OverrideVariant.h, keys 0..9 shader, 20..24 controller,
// 30..33 transform, 40 destination), and BSLightingShader::SetupMaterial (AE
// id 107298) DOES read texCoordOffset/texCoordScale for every technique, skin
// included, so the constants would work and skee could not persist them. A
// plugin that wrote them would be a second painter of the [Ovl] material for
// ever, reapplied at every install and after every RaceMenu edit, and it could
// not show a rotation at all. Two painters of one appearance always drift.
//
// So the overlay's own RGBA is resampled at the transform into a DDS under
// textures\FittingRoom\baked\ and that path goes into key 9 through
// OverlayApi::Write exactly as any texture does. skee loads it, persists it in
// its co-save and repaints it at every install. This module never touches an
// [Ovl] material after the file lands.
//
// ---- THE TRANSIENT, DURING A DRAG -----------------------------------------
//
// A slider posts a value per frame and a file per frame is unthinkable, so a
// drag shows an IN-MEMORY bake: the same shader into a texture this module
// keeps, assigned onto the [Ovl] clone's own material in place. That is the
// exact write skee's NIOVTaskUpdateTexture makes for key 9, and it is safe on
// this material for a reason that is MEASURED and not assumed: the clone's
// shader property comes from a fresh NiStream load of skee's template
// (OverlayInterface.cpp 102..149), and BSLightingShaderProperty::LoadBinary (AE
// 106493) assigns a fresh material straight to +0x78 without the pool;
// BSShaderProperty::SetMaterial (AE 105544) pools only when unique==0 AND the
// property has no name. Every [Ovl] material is therefore this clone's alone,
// which is also why skee's per-overlay glossiness works. shader-materials-are-
// pooled-never-write-one still holds for everything reached from a cached
// model; this is the one material that provably is not.
//
// ⚠ THE TRANSIENT IS ONE TEXTURE PER (ACTOR, NODE), REPAINTED, NEVER FREED.
// A forged NiSourceTexture whose refcount reaches zero would run the engine's
// destructor over a RendererData this module allocated, which is the balance
// never-hand-balance-refcounted-engine-state forbids. So each layer gets one
// texture object for the session and every drag frame dispatches into the SAME
// D3D texture; a source of another size makes a second one and the first is
// kept. A few MiB per layer touched, bounded by kTransientCapPx.
//
// The commit is the same shader at iOverlayBakeCapPx, a staging readback of
// the whole chain, a DX10 R8G8B8A8 DDS plus its sidecar written on a worker
// thread STRAIGHT TO THE DESTINATION (mo2-rename-hook-crashes-under-a-write-
// burst: no temp and rename), then the caller's continuation on the game
// thread, where OverlayApi puts the path into key 9.
//
// ⚠⚠ THE ONE UNMEASURED LINK, AND THE PROBE THAT MEASURES IT. Whether a file
// this process writes under Data\textures\ at runtime lands in MO2's overwrite
// and loads back through the ENGINE's loader by its relative path has been
// shown for this mod's own reads (PreviewDiskCache's write probe) and never
// for skee's LoadTexture. So every Write of a baked path arms a probe: two
// seconds of presents later, a game-thread task reads the [Ovl] material's
// diffuse and logs its name and size against what was baked. MATCH is the
// field answer; MISMATCH with a small size is the engine's placeholder, which
// is what a missing file looks like (missing-texture-resolves-to-a-placeholder).
//
// ⚠ RENDER THREAD FOR ALL D3D. Pump is called from the editor's Present hook
// beside DyeTexture::Pump; the immediate context is not thread safe. Requests
// arrive from anywhere and are queued under a lock.
namespace OS::OverlayBake {

    using Transform = OverlayTransform::Transform;

    // Show a_t on a_node of a_actor now, in memory, no file. Any thread. A
    // second call for the same layer before the first was built replaces it,
    // which is what makes a sixty-frame drag cost one build per present.
    void Preview(RE::ActorHandle a_actor, const std::string& a_node,
                 const std::string& a_source, const Transform& a_t);

    // Make sure the baked file for (a_source, a_t) exists, then run a_then ON
    // THE GAME THREAD with true; on a bake that could not be made, with false.
    // Identity needs no file and continues at once. A file already on disk is
    // touched (it is the most recently used) and continues at once. Any thread.
    //
    // ⚠ CALLERS WRITE THE PATH ONLY FROM INSIDE a_then. Putting the baked path
    // into key 9 before the file exists makes skee load a missing file, and
    // the engine caches the placeholder it substitutes under that path for as
    // long as it stays referenced, so a file that arrives a moment later is
    // never seen. OverlayApi::Write is the one caller and does exactly this.
    void Ensure(const std::string& a_source, const Transform& a_t,
                std::function<void(bool)> a_then);

    // Arm the field probe described above for a_node, expecting a_expectedPath
    // (an override path) at a_width x a_height. Game or render thread.
    void ProbeLater(RE::ActorHandle a_actor, const std::string& a_node,
                    const std::string& a_expectedPath);

    // Render thread, every present, beside DyeTexture::Pump. Builds at most
    // one preview and one commit per call and returns at once when idle.
    void Pump();

    // The full path on disk of a baked file, for a log line or a check.
    [[nodiscard]] std::string DiskPath(const std::string& a_overridePath);

    // Read the sidecar beside a baked path off disk. ok false when there is
    // none or it is another version's.
    [[nodiscard]] OverlayTransform::Decoded ReadSidecar(const std::string& a_bakedPath);

    struct Stats {
        std::size_t previews{ 0 };   // transient builds
        std::size_t commits{ 0 };    // files written
        std::size_t hits{ 0 };       // Ensure found the file already there
        std::size_t failed{ 0 };
        std::size_t evicted{ 0 };
        std::size_t superseded{ 0 }; // previews replaced before they ran
    };
    [[nodiscard]] Stats GetStats();

}  // namespace OS::OverlayBake
