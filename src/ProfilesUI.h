#pragma once

namespace RE {
    class Actor;
}

// The Looks page (W2): the library of saved whole-character profiles, over
// the W1 spine (ProfileStore / ProfileCapture / ProfileApply). The page is
// presentation only: every capture, apply, rename and delete goes through
// the W1 halves, so there is no second painter of a profile anywhere here.
//
// ⚠ THE RAIL TILE SAYS "Looks" AND THE CODE SAYS Profiles, ON PURPOSE
// (decision 4's own word, 2026-08-21). ProfileCodec, ProfileStore and the
// file format all predate the page name and two of them are stored data;
// the tile is the only place the player-facing word appears.
namespace OS::ProfilesUI {

    // Re-read the store from disk and rebuild the page's rows. Called when
    // the page is entered; the store is files on disk and this build is not
    // their only writer (a manual copy into Profiles/ counts), so a page
    // that trusted its first read would show a library the disk no longer
    // has.
    void OnOpen(RE::Actor* a_player);

    // Draws the complete page below EditorUI's shared title and target row.
    void Draw(RE::Actor* a_player);

    // ⚠ THERE IS NO OnClose AND NO HasUnsavedChanges, DELIBERATELY, the same
    // call the Shape and Overlays pages make. Save is an explicit press and
    // everything else acts immediately, so leaving the page can strand
    // nothing.

}  // namespace OS::ProfilesUI
