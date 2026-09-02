#pragma once

#include "OverlayPlan.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Which of the four overlay locations a texture belongs on.
//
// ⚠⚠ RaceMenu'S OWN LISTS ARE AUTHORED, NOT DERIVED, AND THAT IS THE WHOLE
// SHAPE OF THIS FILE. The field asked that a body overlay not be offered for a
// face slot, on the grounds that RaceMenu's paint pages are filtered. They are,
// and the filter is not a rule: skee64.dll contains no texture enumeration at
// all, only the "Body [Ovl{}]" node templates and the default texture. The four
// lists live in Papyrus, in RaceMenuBase's _textures_body, _textures_hand,
// _textures_feet and _textures_face, and each pack fills the ones it belongs in
// from its own OnBodyPaintRequest by calling AddBodyPaint, AddHandPaint,
// AddFeetPaint, AddFacePaint or AddWarpaint. Measured 2026-08-16 by extracting
// racemenubase.psc from RaceMenu.bsa.
//
// So there is nothing to reverse into a classifier, and the two obvious rules
// were measured and are both wrong on this library:
//
//   * By folder. The installed presets put Community Overlays CO 2 and CO 3 on
//     all four locations, so the same directory is legitimately every location.
//   * By file name. Of 2047 installed overlay textures, 1323 carry no location
//     word at all, so a name rule alone hides two thirds of the library from
//     every slot.
//
// What this does instead is read the packs' own registrations, generated
// offline by tools/scan-paint-registrations.ps1 into
// SKSE/Plugins/FittingRoom/Overlays/overlay-locations.json. On the reference
// load order that places 1464 of the 1600 textures the picker can see.
//
// ⚠⚠ AND WHAT IT CANNOT PLACE IS SHOWN, NEVER HIDDEN. A texture missing from a
// list is invisible and unexplainable; one offered in the wrong place is merely
// untidy. Every fallback below widens rather than narrows, and the picker
// carries a switch that turns the whole filter off.
namespace OS::OverlayLocations {

    // A set of locations, one bit per OverlayPlan::Location, plus one bit that
    // is not a location at all.
    using Mask = std::uint8_t;

    inline constexpr Mask kNone = 0;
    inline constexpr Mask kAll  = 0x0Fu;

    // ⚠⚠ THE WARPAINT BIT IS NOT A LOCATION AND NOTHING MAY TREAT IT AS ONE.
    // A pack that calls AddWarpaint has said the art goes into the head's TINT
    // MASK layers, which is the Makeup section's system and not the [Ovl] nodes
    // this page drives. It used to be dropped on the floor here, which was right
    // for the overlays picker and left the 1062 textures that carry it with no
    // home at all.
    //
    // ⚠ IT LIVES ABOVE kAll SO EVERY EXISTING TEST STAYS TRUE. Allows() reads
    // one location bit, kAll is still the four locations, and the fall-through
    // in Resolve keys on HasLocation rather than on the mask being non-zero.
    // That last one is the trap: a warpaint-only texture must still fall through
    // to the name rule, and a plain `mask != kNone` test would have stopped it
    // doing so the moment this bit started being stored.
    inline constexpr Mask kWarpaint = 0x10u;

    [[nodiscard]] inline constexpr Mask Bit(OverlayPlan::Location a_location) {
        return static_cast<Mask>(1u << static_cast<std::uint8_t>(a_location));
    }

    [[nodiscard]] inline constexpr bool Allows(Mask a_mask, OverlayPlan::Location a_location) {
        return (a_mask & Bit(a_location)) != 0;
    }

    // The four location bits on their own.
    [[nodiscard]] inline constexpr Mask LocationBits(Mask a_mask) {
        return static_cast<Mask>(a_mask & kAll);
    }

    [[nodiscard]] inline constexpr bool HasLocation(Mask a_mask) {
        return LocationBits(a_mask) != kNone;
    }

    [[nodiscard]] inline constexpr bool IsWarpaint(Mask a_mask) {
        return (a_mask & kWarpaint) != 0;
    }

    // ---- the table's own vocabulary ----------------------------------------

    // Parse the comma separated list value the generator writes, e.g.
    // "body,feet". Unknown words are ignored rather than refused, so a table
    // written by a newer generator degrades to whatever this build understands.
    //
    // ⚠ MaskToLists BELOW IS ITS INVERSE AND THEY MUST STAY ONE PAIR. The
    // scrape writes a file this function then reads back, so a word spelt one
    // way going out and another coming in would give a scraped row that reads
    // as kNone and silently narrows the texture to nothing.
    //
    // ⚠ "warp" IS DELIBERATELY NOT A LOCATION. A warpaint goes into the head's
    // tint mask layer, which is a different system from the [Ovl] nodes this
    // page drives, so a pack that registered a texture ONLY as a warpaint has
    // said nothing about the four overlay lists. Reading it as Face would have
    // been a guess with teeth: Shep's two tattoo collections register all 264
    // of their BODY tattoos as warpaints, and mapping those to Face would have
    // hidden every one of them from the location they are painted for.
    // Warpaint-only art therefore falls through to the name rule below and, if
    // that says nothing either, RESOLVES to everywhere.
    //
    // ⚠⚠ RESOLVES, NOT "IS OFFERED". Since OS-219 (2026-08-18) the overlays
    // picker reads the registration itself before it reads the resolved mask:
    // `entry.warpaint && !HasLocation(entry.registered)` is makeup and stays
    // out of the overlays picker unless Show all art, which is exactly what
    // RaceMenu shows for every REGISTERED texture (its face-overlay list is
    // `_textures_face`, and Lovely Makeup's 26, LDD's, Koralina's and the rest
    // are not in it). MEASURED on the reference table (regenerated 2026-08-18,
    // 2263 rows from 36 scripts): all 1389 warp rows are warp-only, so the rule
    // is the whole makeup library. Shep's 264 keep their body page through the
    // fixups file, not through this fall-through.
    [[nodiscard]] Mask FromLists(std::string_view a_lists);

    // The words FromLists reads, written back out in a fixed order so two runs
    // of the scrape over the same rig produce the same file and a diff of two
    // scrapes is a diff of what RaceMenu said.
    [[nodiscard]] std::string MaskToLists(Mask a_mask);

    // ---- the scrape's vocabulary -------------------------------------------
    //
    // ⚠⚠ THIS IS RaceMenu'S WIRE FORMAT AND NOT OURS. Each entry a pack pushes
    // into the RaceSex Menu is one string: the display name, then ";;", then
    // either one texture path or, from the Ex forms, up to eight of them
    // separated by '|'. MEASURED in racemenubase.psc (RaceMenu.bsa 1.6):
    // AddWarpaint and friends build "name;;path", AddWarpaintEx and friends
    // build "name;;t0|t1|...|t7".
    //
    // ⚠ THE NAME IS DROPPED ON PURPOSE. It is the pack's label for a picker we
    // do not draw, and two packs may legitimately give the same file two names.
    // The key is the path, exactly as everywhere else in this file.
    //
    // Returns every path in the entry, empty ones skipped. An entry with no
    // ";;" at all is not one of these and gives nothing back rather than being
    // read as a bare path: a malformed line must add no row, because a scraped
    // row OUTRANKS the shipped table.
    [[nodiscard]] std::vector<std::string> PathsInEntry(std::string_view a_entry);

    // ---- the fallback, which only ever runs when the table said nothing ----

    // A location word in the file name, then in any folder above it.
    //
    // ⚠ SECOND, NEVER FIRST, and never on its own. The handoff's measurement
    // stands: on its own this rule hides two thirds of the library. Under the
    // authored table it is doing a different job, which is placing the packs
    // that ship no registration script at all, and it is measured too: widened
    // from the file name to the whole path it covers 1426 of the 2047 installed
    // textures rather than 724.
    [[nodiscard]] Mask FromPathTokens(std::string_view a_overridePath);

    // The whole rule, in order: the pack's own registration, then the name and
    // folder words, then every location.
    [[nodiscard]] Mask Resolve(Mask a_registered, std::string_view a_overridePath);

    // ---- the mask twins ----------------------------------------------------
    //
    // ⚠⚠ MANY PACKS SHIP EACH PIECE OF ART TWICE, and the second copy has a
    // black background because it is a tint mask rather than an overlay. The
    // field called it jarring in a grid of cards and it is: the two cards sit
    // side by side, one correct and one a black square with the art faintly on
    // it. MEASURED on the reference rig: 96 files are another file's name plus
    // " m", and 80 of those are the mask halves, DXT1 with no alpha where the
    // original is DXT5 with one.
    //
    // ⚠⚠ AND THE SUFFIX ALONE CANNOT DECIDE IT. In Community Overlays 3 the
    // same " m" means MALE: 76 files ending in it are registered as real body,
    // hands and face art, beside their " f" twins. A rule that hid every " m"
    // would delete half of the biggest pack in the library from the picker.
    //
    // So the registrations decide, and they separate the two cleanly. A mask
    // twin is a file whose pack registered it as a WARPAINT ONLY, sitting
    // beside a file of the same name minus the suffix that the same pack
    // registered for an actual overlay location. Shep's 898 warpaint-only
    // textures have no such twin and stay visible, because they are the art.

    // The file this one would be the mask of: the same path with the mask
    // suffix taken off its name. Empty when the name carries no mask suffix.
    [[nodiscard]] std::string MaskTwinOf(std::string_view a_overridePath);

    // Whether the table knows this path at all. Distinct from Registered
    // returning kNone, which is also what a warpaint-only texture gives: only
    // one of the two means "no pack has ever mentioned this file".
    [[nodiscard]] bool Known(std::string_view a_overridePath);

    // Whether a_overridePath is a redundant copy of a_twinPath. Both halves are
    // consulted in the table, so this is only ever true for a pair whose own
    // pack said what each half is.
    [[nodiscard]] bool MaskTwinIsRedundant(std::string_view a_overridePath,
                                           std::string_view a_twinPath);

    // ---- the shipped table -------------------------------------------------

    // ⚠ THE LOAD REPORTS RATHER THAN LOGGING, the same shape DyeRules uses and
    // for its reason: this translation unit is compiled into a pure-logic test
    // as well as into the DLL, and spdlog reaches only one of the two. The
    // caller on the DLL side is the one that writes the line.
    //
    // ⚠⚠ THE TABLE IS RE-READ WHILE THE GAME IS RUNNING AND IT USED NOT TO BE.
    // It was loaded once at kDataLoaded and read-only from then on, which is
    // the sentence in OverlayTextures.h that made it safe to consult from the
    // detached scan thread. The scrape breaks that: a RaceMenu trip ends on the
    // main thread and reloads, and the picker's scan can be walking the same
    // map on its own thread at that moment. So the table is held behind an
    // atomic shared pointer and a load SWAPS it rather than clearing and
    // refilling in place. Readers take one snapshot and finish against it; a
    // reload in the middle gives a reader the old answer for the rest of its
    // pass, which is a stale scan and not a torn map, and the rescan the reload
    // asks for is what replaces it.
    struct LoadReport {
        std::size_t entries{ 0 };    // rows read, including warpaint-only ones
        std::size_t placed{ 0 };     // rows naming at least one overlay location, after fixups
        std::size_t corrected{ 0 };  // rows a fixup moved onto a location
        std::size_t fixups{ 0 };     // fixups that named a script the table knows
        std::size_t scraped{ 0 };    // rows read from the scraped file
        std::size_t scrapedAdded{ 0 };     // of those, keys the shipped table lacked
        std::size_t scrapedOverrode{ 0 };  // of those, keys it held and disagreed with
        // Rows whose value was not text, so nothing could be read from them.
        // ⚠ Counted rather than fatal: jsoncpp THROWS on asString() over a
        // non-string, an uncaught throw is a fail-fast, and a fail-fast is a
        // crash with no crash log. One unreadable row is not worth the process.
        std::size_t scrapedSkipped{ 0 };
        bool        loaded{ false };
        std::string diagnostic;
    };

    // Read the generated table, the scrape taken off this rig's own RaceMenu,
    // and the corrections. Missing is a supported state and not an error: an
    // install without the table filters on names alone, which is the tier
    // below, and a table that will not parse does the same rather than emptying
    // the picker. A missing or unreadable fixups file corrects nothing and says
    // so in the diagnostic; a missing scrape is the normal state until the
    // player has opened RaceMenu once with this build.
    LoadReport Load();

    // Load the table alone from a given file, for tests and for a second
    // install layout. No scrape and no corrections are applied.
    LoadReport LoadFrom(const std::filesystem::path& a_file);

    // ---- the corrections ---------------------------------------------------
    //
    // ⚠⚠ A PACK CAN REGISTER ITS ART IN THE WRONG LIST, AND THE TABLE RECORDS
    // WHAT IT SAID, NOT WHAT IT MEANT. Shep's two tattoo collections register
    // all 264 of their BODY tattoos with AddWarpaint, so RaceMenu offers them
    // only under war paint and the honest reading of the registration (a
    // warpaint-only texture is makeup) would take them off the body page they
    // are painted for. The generator cannot know that; a person can. So the
    // table ships with a second file beside it, overlay-locations-fixups.json,
    // that names a registration SCRIPT by its file name and says which overlay
    // locations that script's warpaint-only registrations really belong on:
    //
    //   { "fixups": [ { "script": "Sheps_Tattoo_Collection.pex",
    //                   "warpaint_is": "body", "why": "..." } ] }
    //
    // The script's file name is the key because the mod folder in front of it
    // is this rig's and the file name is the pack's. The table's by_source
    // object (written by the generator since 2026-08-18) says which keys each
    // script registered, and a fixup ORs its locations into every one of those
    // keys that no pack placed on a location. The warpaint bit is kept, so a
    // corrected texture stays in the Makeup library as well, which is where
    // RaceMenu shows it.
    //
    // ⚠ A FIXUP NEVER NARROWS. It can only add location bits to a row that has
    // none, so a broken fixups file cannot hide anything the table would have
    // shown, and it cannot move a texture a pack DID place.
    LoadReport LoadFrom(const std::filesystem::path& a_file,
                        const std::filesystem::path& a_fixups);

    // ---- the scrape ---------------------------------------------------------
    //
    // ⚠⚠ THE SHIPPED TABLE IS A SNAPSHOT OF SOMEBODY ELSE'S RIG AND THE SCRAPE
    // IS THIS ONE. overlay-locations.json is generated offline by reading the
    // registration scripts installed on the reference load order, so a pack
    // this rig has and that one did not is a pack the table has never heard of,
    // and an unknown texture RESOLVES TO EVERYWHERE. MEASURED 2026-08-18 when
    // the table was regenerated: the reference rig had grown by ten scripts and
    // 417 rows, and every one of those packs (Lovely Makeup 2 and 3, Ancient
    // Beauty, Fabulous Makeup 2, Koralina's COtR details, UBE Even More Makeup,
    // BB Nipple Blush) had been offered on every overlay page in the meantime.
    // Regenerating the table fixes it for one rig on one day. The scrape is the
    // same answer taken from THIS rig's own RaceMenu, in flight, and it does
    // not go stale.
    //
    // ⚠⚠ AND IT IS THE ONE LAYER IN THIS FILE THAT MAY NARROW. Everything else
    // widens by design, because art that is hidden is unexplainable and art in
    // the wrong place is merely untidy. This narrows on purpose and the licence
    // is that it is not a guess: it is what RaceMenu itself listed on this
    // install, captured while the menu was open, which is the exact authority
    // the whole file defers to. Three things keep it honest, and all three are
    // enforced by the writer in PaintScrape rather than here:
    //
    //   * ALL FIVE lists must have been wrapped, or nothing is written. A
    //     capture that missed AddFacePaints would record a face texture as
    //     warpaint-only and hide it.
    //   * A capture that recorded nothing writes nothing, so a trip that ended
    //     before the packs' handlers ran cannot blank the file.
    //   * The file names only what was seen. A key the scrape lacks keeps the
    //     shipped table's answer untouched, so a partial scrape can only
    //     under-add and never hide.
    //
    // A scraped row OUTRANKS the shipped row for the same key and ADDS the keys
    // the shipped table lacks. The fixups run after both, which leaves Shep's
    // 264 alone: RaceMenu has them warpaint-only too, so the scrape agrees with
    // the table, the row still carries no location, and the fixup still gives
    // it body.
    LoadReport LoadFrom(const std::filesystem::path& a_file,
                        const std::filesystem::path& a_scraped,
                        const std::filesystem::path& a_fixups);

    // Where the three files live. Exposed because the scrape's WRITER and this
    // READER must name the same file and a second spelling of it would be a
    // capture that works and never arrives.
    [[nodiscard]] std::filesystem::path TableFile();
    [[nodiscard]] std::filesystem::path ScrapedFile();
    [[nodiscard]] std::filesystem::path FixupsFile();

    // What the table alone says, kNone when it has never heard of the path.
    [[nodiscard]] Mask Registered(std::string_view a_overridePath);

    // Every path whose pack registered it as a warpaint, which is the Makeup
    // section's library: 1062 of the 1846 rows on the reference load order, and
    // the makeup packs by name. Sorted, so the picker's order does not depend on
    // the table's hash order and two runs show the same grid.
    [[nodiscard]] std::vector<std::string> WarpaintPaths();

    // Every path the table knows at all, sorted for the same reason.
    //
    // ⚠⚠ THE TABLE NAMES ART THE SCAN'S ROOTS CANNOT SEE, and this is how the
    // scan learns about it. MEASURED on the reference load order: 103 of the
    // 1846 rows sit under textures\!ube\..., a top-level folder that is neither
    // overlays\ nor TintMasks\. Among them: the ONLY feet-only art installed
    // (UBE's toenail overlays, in !ube\[spaz490]\nail overlays\) and 26 UBE
    // warpaints. A picker that only walks the two conventional roots shows a
    // feet grid with no real feet art in it, which is the field report of
    // 2026-08-16.
    [[nodiscard]] std::vector<std::string> TablePaths();

    // The answer the picker uses: Resolve applied to the table's verdict.
    [[nodiscard]] Mask For(std::string_view a_overridePath);

    // Whether a table was loaded at all, so the page can say why it is not
    // filtering rather than appearing to have no opinion.
    [[nodiscard]] bool Loaded();

    // The key the table is stored under: lower case, backslashes, no leading
    // separator and no textures\ prefix. Exposed because the generator has to
    // agree with it exactly and the tests pin that agreement.
    [[nodiscard]] std::string Key(std::string_view a_overridePath);

}  // namespace OS::OverlayLocations
