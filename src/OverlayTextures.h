#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The overlay art installed on this load order, read off disk.
//
// ⚠ THE PATHS ARE WHAT THE OVERRIDE WANTS, not what the scan found. Every entry
// is already relative to textures\ with backslashes, because that is the only
// form skee accepts and the form all 331 reference presets store. Converting at
// the point of use instead would put the conversion behind every caller.
//
// ⚠ LOOSE FILES ONLY, AND THIS IS A KNOWN LIMIT RATHER THAN AN OVERSIGHT. Most
// overlay packs ship loose: of the ones on the reference rig, Community
// Overlays, Koralina's, Female Makeup Suite and Pretty Makeup all do, and only
// RaceMenu Animated Overlays packs its art into a BSA. Reading archives needs
// the engine's own resource layer rather than std::filesystem, so a packed pack
// is invisible here and the user can still type its path by hand.
namespace OS::OverlayTextures {

    struct Entry {
        std::string path;    // relative to textures\, backslashes, ready to write
        std::string name;    // the file's stem, for a button
        std::string folder;  // the containing folder under overlays\, for grouping
        // Which locations this art may be offered for. Resolved once here
        // rather than per frame in the picker: the answer builds a lower cased
        // key and hashes it, and the picker walks every entry on every frame it
        // is open, which on this rig is 1600 of them.
        //
        // ⚠ THE TABLE IS READ AT kDataLoaded AND THIS SCAN IS REQUESTED WHEN
        // THE PAGE IS FIRST OPENED, so the table is always in place by the time
        // this is filled. It is also read-only from here on, which is what
        // makes it safe to consult from the scan thread.
        std::uint8_t locations{ 0x0Fu };  // OverlayLocations::Mask, kAll
        // ⚠ THE BLACK BACKGROUNDED SECOND COPY. True when this file is the
        // warpaint mask of another texture that IS in this scan and IS real
        // overlay art. Resolved here rather than in the picker because it takes
        // the whole installed file list to answer, which is a thing only the
        // scan holds. See OverlayLocations::MaskTwinIsRedundant.
        bool maskTwin{ false };
        // ⚠⚠ THIS FILE IS A TINT MASK RATHER THAN AN OVERLAY, because it lives
        // under Character Assets\TintMasks. It is scanned for the Makeup
        // sections and it must never reach the OVERLAYS picker: a tint mask is
        // painted into the head's tint texture by the engine's own compositor
        // and putting one in an [Ovl] node override paints a lip-shaped mask
        // onto a body. The two libraries share a scan and share nothing else.
        bool tintMask{ false };
        // What the packs' own table said, unresolved. `locations` is the answer
        // after the name rule and the show-everything fallback have had their
        // turn, so by then there is no way to tell art a pack placed here from
        // art nobody placed anywhere.
        //
        // ⚠⚠ AND THE PICKER NEEDS THE DIFFERENCE. Field 2026-08-16: the feet
        // grid shows body art. MEASURED cause: 85 of the table's 1846 rows name
        // feet at all, so on that location the fallback is nearly everything on
        // screen. The fallback stays, because 1323 of 2047 installed textures
        // carry no location word and hiding them was rejected; what changes is
        // that art a pack really placed here is drawn first and the rest sits
        // under a band that says what it is.
        std::uint8_t registered{ 0 };
        // ⚠⚠ THE PACK REGISTERED THIS AS A WARPAINT, which is the Makeup
        // section's library and not one of the four overlay locations. Kept
        // separately because `locations` is the RESOLVED answer and resolving
        // deliberately drops this bit: a warpaint-only texture still falls
        // through to the name rule and then to being shown everywhere, so by the
        // time `locations` exists there is no way to tell it from art no pack
        // ever mentioned. 1062 of the 1846 registered rows on the reference
        // load order are these, and they are the makeup packs by name.
        bool warpaint{ false };
    };

    struct Snapshot {
        std::vector<Entry> entries;  // sorted by folder then name
        std::size_t        filesSeen{ 0 };
        std::size_t        mapsSkipped{ 0 };
        std::size_t        masksHidden{ 0 };  // entries with maskTwin set
        std::size_t        tintMasks{ 0 };    // entries with tintMask set
        std::string        diagnostic;
        std::uint64_t      generation{ 0 };
    };

    // Walk the disk and publish a new snapshot. Runs on a detached thread, as
    // BodyMorphCatalog's scan does and for the same reason: a large load order
    // has thousands of these and the editor draws every frame.
    void RequestScan();

    [[nodiscard]] std::shared_ptr<const Snapshot> Get();
    [[nodiscard]] bool                            Scanning();

    // The pure half, exposed for the scan and for tests to drive with a
    // temporary tree rather than the live install.
    [[nodiscard]] Snapshot ScanRoot(const std::filesystem::path& a_texturesRoot);

}  // namespace OS::OverlayTextures
