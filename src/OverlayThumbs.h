#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>

// Thumbnails for the overlay picker: a DDS on disk turned into something the
// editor can draw.
//
// ⚠⚠ THE DECODE IS CPU AND OFF THE RENDER THREAD, AND THAT IS THE WHOLE SHAPE
// OF THIS FILE. The alternative was a GPU blit through the game's own immediate
// context, which is what PreviewRenderer does for meshes, and it carries that
// file's entire burden: the context is not thread safe, every binding has to be
// saved and restored around the draw, and getting either wrong is a corrupted
// frame or a crash rather than an error return. A texture needs none of that.
// DirectXTex decompresses on the CPU, so the work runs on a worker thread and
// touches no device at all.
//
// ⚠ WIC WAS TRIED FIRST AND MEASURED, NOT ASSUMED. PreviewPng already encodes
// through WIC, so decoding through it too would have added no dependency.
// Measured against the reference rig on 2026-08-16: WIC decodes DXT1 and DXT5
// and REFUSES BC7, which is 595 of the 1851 overlay textures installed there.
// A picker that silently lost a third of the library to a blank card is not
// worth the saved dependency.
//
// ⚠ THE HANDLE IS A FUCK IMAGE, NEVER ONE OF OUR OWN SRVs, which is
// PreviewCache.h's Task 0 verdict and applies here unchanged. That is also why
// the thumbnail goes to disk as a PNG rather than staying in memory: FUCK loads
// images by PATH, so a file is the only way in.
namespace OS::OverlayThumbs {

    // The handle for a texture path, or 0 while queued, failed or missing.
    // Requesting is what asking for it does, so a card calls this during its own
    // draw and nothing else has to know which cards are on screen.
    //
    // Safe from Draw: it never touches the device and never loads a file.
    [[nodiscard]] ImTextureID Texture(const std::string& a_texturePath);

    // True when this one is known to have failed, so the card can draw its
    // cross rather than spin forever.
    [[nodiscard]] bool Failed(const std::string& a_texturePath);

    // ---- art or mask -------------------------------------------------------
    //
    // ⚠⚠ AN OVERLAY WITH NO TRANSPARENCY IS NOT AN OVERLAY. It is the tint mask
    // copy that most packs ship beside the real art, and painted onto a face it
    // is a solid rectangle rather than a freckle. In the picker it draws as a
    // black card with a faint shape on it, which the field called jarring twice
    // (user 2026-08-16).
    //
    // ⚠⚠ AND THE FILE NAME CANNOT ANSWER IT, WHICH TWO ROUNDS PROVED. The
    // suffix " m" is a mask in SkFO and WNB and means MALE in Community
    // Overlays 3, and packs that ship no registration script give no other
    // clue. The ASSET answers it: MEASURED across the 449 thumbnails this rig
    // had already decoded, 68 have NOT ONE transparent pixel and the other 381
    // are more than half transparent. There is nothing in between, so any
    // threshold in that gap gives the same verdict.
    //
    // ⚠ IT IS ONLY KNOWN AFTER A DECODE, so kUnknown means "not yet" rather
    // than "no". A caller filtering on this must treat kUnknown as showing the
    // card, or the grid starts empty and fills in, which is worse than the
    // problem. The verdict is carried in the cached PNG's own file name, so a
    // texture decoded in any earlier session is known before this one draws.
    //
    // ⚠⚠ A THIRD KIND, kWhiteMask (OS-218, user 2026-08-18): a tint mask that
    // DOES have transparency. LDD's eye circles, and most of the makeup packs,
    // are authored the way the engine's tint compositor wants: the shape in
    // the alpha and the RGB near white, so the makeup colour multiplies in. As
    // an [Ovl] overlay that same file paints white and cannot be darkened
    // (MEASURED 2026-08-16, the [Ovl] tint reaches the material and the shape
    // still renders white), and RaceMenu's own face-overlay list never offers
    // it because its pack registered it as a warpaint. So it is offered to
    // the makeup pickers and kept out of the overlays picker unless a pack
    // registered it as an overlay somewhere. A DARK tattoo (Shep's 264 body
    // tattoos are warpaint-only registrations too) is not near white and
    // stays kArt, which is why the content decides and not the registration.
    enum class Alpha : std::uint8_t {
        kUnknown,    // never decoded, or the decode failed
        kArt,        // has transparency and colour: a real overlay
        kMask,       // no transparency at all: a tint mask, and a black square on the body
        kWhiteMask,  // transparency over near-white RGB: a tint mask with a shape
    };

    [[nodiscard]] Alpha AlphaKind(const std::string& a_texturePath);

    // ⚠ ONCE PER PRESENT, FROM THE SAME THUNK THAT PUMPS THE PREVIEW CARDS, AND
    // NEVER FROM Draw. Constructing a FUCK::Image is FUCK image traffic, and
    // PreviewCache.h records what that costs inside FUCK's own UI pass: the
    // whole editor vanished on exactly the loading frames. At most one image is
    // turned into a handle per call, for the same reason PreviewCache::Drain
    // builds at most one entry.
    void Pump();

    // Drop every decoded handle, so a rebuilt cache is visible this session.
    void ForgetAll();

}  // namespace OS::OverlayThumbs
