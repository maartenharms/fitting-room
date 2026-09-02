#pragma once

#include "PreviewGrid.h"

#include <imgui.h>

#include <string>

// The request-and-drain engine between the cards and everything below them.
// Render thread only, all of it: Request and Texture are called from Draw,
// the drain and the release run from DyeTexture's Present thunk
// (EditorWindow::PumpPreviews), so there is still no lock and no thread
// question - one thread, two spots in its frame. ⚠ NEVER move the drain
// back into Draw: its D3D and FUCK-image work inside FUCK's own UI pass hid
// the whole editor on loading frames (field 2026-08-09). A queued request
// holds strings and a FormID, never an RE:: pointer, which is why a save
// load cannot corrupt the queue.
namespace OS::PreviewCache {

    // Called by every card during ITS OWN draw; request-on-draw IS the
    // visibility calculation, there is no viewport pass. Stamps the entry
    // with the Present counter and the card's on-screen index, which is what
    // makes a cold fill run top-left to bottom-right.
    // Returns the scene's disk key, which is also the handle every other
    // call here takes.
    //
    // ⚠ THE KEY COMES BACK RATHER THAN BEING RECOMPUTED. Callers used to ask
    // DiskKeyFor for it on the very next line, and since OS-191 the key
    // carries a scope tag this stamps, so a second computation is a second
    // place that has to agree. One that quietly did not would be a card stuck
    // loading forever, with both halves looking correct in isolation.
    std::string Request(const PreviewGrid::SceneIdentity& a_id, std::uint32_t a_orderHint);

    // The display handle for a key, or 0 while queued, failed or missing.
    // ⚠ A FUCK HANDLE, never one of our SRVs (Task 0 verdict). Valid until
    // the entry is evicted, and eviction releases one full frame later
    // through the pending list.
    [[nodiscard]] ImTextureID Texture(const std::string& a_diskKey);

    // True when the entry is known failed, so the card draws its cross.
    [[nodiscard]] bool Failed(const std::string& a_diskKey);

    // 0..1: the fade-in ramp for a thumbnail that just became ready, so a
    // filling page reads as arrival rather than flicker. 1 for anything
    // settled or unknown; pair with Texture as the image tint's alpha.
    [[nodiscard]] float RevealAlpha(const std::string& a_diskKey);

    // Once per present, from the thunk: prune stale requests, then build or
    // load AT MOST ONE entry. Never batches, never catches up; a screenful
    // fills in as many presents and that is the design.
    void Drain();

    // Before Drain in the same thunk: release the previous present's
    // evictions. The one-present retention keeps a released handle out of
    // draw data FUCK may still be rendering.
    void ReleasePending();

    // Drop every card held in memory, so a disk rebuild is visible in the
    // browser this session instead of the next one.
    //
    // ⚠ THE OTHER HALF OF 'Rebuild previews'. PreviewDiskCache::RebuildAll
    // empties the disk; this empties what is already decoded and on screen.
    // Calling one without the other is what made the button look broken: the
    // files went and the pictures stayed.
    void ForgetAll();

    // Editor close: flush the manifest, report the frame harness and the
    // session stats.
    void OnEditorClose();

}  // namespace OS::PreviewCache
