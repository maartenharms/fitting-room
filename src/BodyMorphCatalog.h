#pragma once

#include "BodyMorphCatalogParse.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The installed body's slider vocabulary, read off disk.
//
// ⚠ NOT BodySlideCatalog, AND NOT A DUPLICATE OF IT. That one reads PRESETS so
// Body Studio can author endpoints, is gated on the Body Studio channel, and
// runs every shipped file through tinyxml2. This one reads the CATEGORY
// vocabulary so the Shape page can offer live morphs on every channel, and it
// deliberately does not use an XML library: some shipped category files are not
// well-formed and a conforming parser drops them whole. See
// BodyMorphCatalogParse.h.
//
// ⚠ THE SCAN ROOT IS Data/CalienteTools AND NOT Data/CalienteTools/BodySlide.
// Measured on the reference load order 2026-08-07: category files ship under
// three different layouts, and UBE's own two live directly under
// CalienteTools/SliderCategories with no BodySlide level. BodySlideCatalog
// hardcodes the deeper root and therefore cannot see them at all, which on that
// load order means the body's own vocabulary is invisible to it.
namespace OS::BodyMorphCatalog {

    struct Snapshot {
        std::vector<BodyMorphCatalogParse::Category> categories;

        // ⚠⚠ EVERY SLIDER NAME THE INSTALLED BODIES DESCRIBE, BEFORE THE FILTER,
        // AND IT IS NOT A DIAGNOSTIC. The page DRAWS `categories` and must, but
        // it has to READ with this, because writing the shape back clears our
        // whole RaceMenu key and rewrites it from what the page holds. Read with
        // the filtered list and a morph the character already carries under a
        // name the filter dropped is not merely hidden: it is absent from the
        // rewrite, so the next undo, discard or worn shape DELETES it.
        //
        // Read wide, show narrow, write back everything you read.
        std::vector<std::string> allNames;

        // The slider set the body was built from. Empty means it could not be
        // worked out, in which case nothing is filtered and everything the
        // installed bodies describe is offered.
        std::string resolvedSet;
        std::string sourcePreset;  // the OBody preset the set was resolved from
        // The set name the preset NAMED, kept even when its sliders could not
        // be found. resolvedSet is cleared in that case so Filtered() cannot
        // answer true for a filter that did not happen, and without this the
        // two failures are indistinguishable in the log - which is exactly how
        // a whole session went into "could not resolve a slider set" when the
        // set had resolved fine and it was the set's SLIDERS that were absent.
        std::string namedSet;

        // ⚠ THE PRIMARY FILTER, AND THE REASON IT BEATS THE SLIDER SET. These
        // are the morph names the character's built meshes actually carry, read
        // out of the tri beside each one. See BodyMorphTri.h.
        std::size_t triFilesRead{ 0 };
        std::size_t builtNames{ 0 };
        bool        fromMeshes{ false };

        std::size_t categoryFilesRead{ 0 };
        std::size_t availableNames{ 0 };
        std::size_t slidersBeforeFilter{ 0 };
        std::string diagnostic;
        std::uint64_t generation{ 0 };

        [[nodiscard]] bool Filtered() const { return fromMeshes || !resolvedSet.empty(); }
    };

    // Walk the disk and publish a new snapshot.
    //
    // a_wornMeshes are the subject's SKIN mesh paths, as the engine spells them
    // (relative to meshes\, weight suffix and all). Their tris are the filter.
    // a_presetName is the OBody preset the subject is wearing, kept as the
    // FALLBACK route: it names a slider set, whose .osp lists slider names.
    // Pass either empty and the result is simply unfiltered.
    //
    // ⚠ THE MESH PATHS ARE COLLECTED BY THE CALLER, ON THE GAME THREAD. This
    // function's work happens on a detached thread and an actor's skin is
    // engine state, so resolving the paths in here would be a background thread
    // reading forms while the game mutates them.
    //
    // Runs on a detached thread, as BodySlideCatalog's does and for the same
    // reason: this touches thousands of files on a large load order and the
    // editor draws every frame.
    void RequestScan(std::string a_presetName, std::vector<std::string> a_wornMeshes);

    [[nodiscard]] std::shared_ptr<const Snapshot> Get();
    [[nodiscard]] bool                            Scanning();

    // The pure half, exposed for the scan and for tests to drive with a
    // temporary tree rather than the live install.
    //
    // a_meshRoot is the Data\meshes directory the worn paths are relative to.
    [[nodiscard]] Snapshot ScanRoot(const std::filesystem::path&    a_calienteRoot,
                                    const std::string&              a_presetName,
                                    const std::filesystem::path&    a_meshRoot,
                                    const std::vector<std::string>& a_wornMeshes);

}  // namespace OS::BodyMorphCatalog
