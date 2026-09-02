#pragma once

// Read RaceMenu's own paint lists off this rig, while the menu is open.
//
// ⚠⚠ THE LISTS EXIST ONLY IN FLIGHT AND THAT IS THE WHOLE SHAPE OF THIS FILE.
// MEASURED 2026-08-18 in racemenubase.psc and racesex_menu.swf, extracted from
// RaceMenu.bsa 1.6:
//
//   * The arrays are per PACK. Every overlay pack's registration script extends
//     RaceMenuBase on its own quest, so there is no one global _textures_face to
//     read: there is one per pack holding only that pack's rows.
//   * The trigger is the mod event RSM_Initialized, sent by the SWF's own
//     ActionScript when the RaceSex Menu initialises. skee64.dll carries none of
//     the RSM_ strings; the SWF carries them.
//   * RaceMenuBase.OnMenuInitialized runs the pack's OnWarpaintRequest,
//     OnBodyPaintRequest, OnHandPaintRequest, OnFeetPaintRequest and
//     OnFacePaintRequest (which is where AddWarpaint, AddBodyPaint and friends
//     fill the arrays), pushes each array into the movie with
//     UI.InvokeStringA("RaceSex Menu",
//     "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance.RSM_AddWarpaints",
//     textures), and then calls FlushBuffer, which re-creates every array EMPTY
//     - all inside the same handler.
//
// So a Papyrus read after the fact reads empty arrays, and sending the event
// with the menu shut hands the data to a movie that is not there. The only
// moment the answer exists is between the fill and the flush, on its way through
// Scaleform. This wraps the five RSM_Add*Paints functions on the live movie's
// RaceSexPanelsInstance while RaceMenu is open, records what goes past, and
// forwards to the originals.
//
// ⚠ THE TRIPLESS ROUTE WAS PRICED AND REJECTED. RSMDT_SendMenuName,
// RSMDT_SendRootName and RSMDT_SendPrefix will point every pack at another menu
// and another event prefix, which would let Fitting Room ask for the data with
// RaceMenu shut. It re-registers every pack quest's mod events, needs a movie
// Fitting Room does not ship, and RSMDT_SendRestore does NOT put the RSM_
// registrations back (only OnGameReload or a second prefix send does). One
// mistake there breaks every overlay pack on the load order until a reload.
namespace OS::PaintScrape {

    // The RaceSex Menu opened. Wraps the five functions, or says in the log why
    // it could not. Safe to call when RaceMenu is not installed at all.
    //
    // ⚠ IT RETRIES ON THE TASK QUEUE. Whether the panels instance exists at the
    // MenuOpenCloseEvent or only after the SWF's first frame is a property of the
    // movie rather than of anything here, so the first attempt is made inline
    // and, if the instance is not there yet, further attempts are made one frame
    // at a time for a short window. The log says which attempt landed, which is
    // the measurement.
    void OnRaceMenuOpened();

    // The RaceSex Menu closed. Puts the originals back, writes what was captured
    // to overlay-locations-scraped.json, reloads OverlayLocations and asks the
    // overlay scan to republish so the picker's answers change without a
    // restart.
    //
    // ⚠ THE ORIGINALS ARE PUT BACK WHILE THE MOVIE IS STILL ALIVE. Everything
    // this file holds between the two calls is plain C++; no GFxValue outlives
    // the function that made it, because a GFxValue is a reference into the
    // movie's own heap and the heap goes when the menu does.
    void OnRaceMenuClosed();

}  // namespace OS::PaintScrape
