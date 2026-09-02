#pragma once

#include <imgui.h>

#include <optional>
#include <string>

// Portraits: a backbuffer crop of the framed head, saved as a PNG and shown
// on the Looks page's preset and look rows. Route 1 of the preset-card ask
// (2026-08-22): the only legal way to know what a preset looks like is to
// let the engine build the face and photograph the result - rendering a
// jslot offline would mean parsing it, which is forbidden, and reimplementing
// facegen, which is a mountain.
//
// The capture rides DyeTexture's Present thunk, the one tick with the
// immediate context in hand, and runs while the editor is open: the world is
// paused there, the player is framed and visible, and the scenegraph is
// still, which is what makes reading the face node's bound from the render
// thread safe. The crop centres on the projected head; the FR window can in
// principle overlap it (the player may drag it anywhere), which is what the
// retake button is for.
//
// A portrait is captured PER CHARACTER: the same preset on another race or
// sex looks different from its card. "As last loaded" is the honest label.
namespace OS::Portrait {

    // The two namespaces a portrait can belong to. Separate folders, so a
    // preset and a look sharing a name never share a file.
    enum class Kind : std::uint8_t { kPreset, kLook };

    // Ask for a capture of the current framing, saved for a_name. Runs a few
    // presents later on the render thread; no-ops with one already pending
    // or the editor closed (the log says which). Callable from any thread.
    void RequestCapture(Kind a_kind, const std::string& a_name,
                        std::uint32_t a_delayPresents = 2);

    // Whether a capture is still in flight (pending or encoding), so the
    // button can disable itself instead of racing.
    [[nodiscard]] bool CapturePending();

    // The owed half of the preset-load flow: loading a preset closes the
    // editor, so its portrait is taken on the NEXT editor open, after the
    // open framing settles. NoteOwed is called at the Load click; the page's
    // OnOpen consumes it and requests the capture with a settle delay.
    void NoteOwed(Kind a_kind, const std::string& a_name);
    [[nodiscard]] std::optional<std::pair<Kind, std::string>> TakeOwed();

    // The portrait texture for a name, or 0 while none exists. Loading and
    // reloading happen on the present thunk; the first ask queues the load,
    // so a card drawn this frame shows the image a few presents later.
    // ⚠ Call from the FUCK draw thread only - the image map is single-thread
    // by construction (draw and pump are both the render thread).
    [[nodiscard]] ImTextureID TextureFor(Kind a_kind, const std::string& a_name);

    // The per-present pump: finishes pending captures, drains queued texture
    // loads, releases replaced images one present later. Called from
    // DyeTexture's Present thunk beside PumpPreviews; must not throw and
    // must be one atomic load on the frames nothing is happening.
    void Pump(void* a_swapChain);

}  // namespace OS::Portrait
