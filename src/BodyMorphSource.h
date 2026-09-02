#pragma once

// Everything a body card needs that lives in a slider set's own files: which
// mesh the shape is built ON, and how each slider drives the .osd deltas.
//
// ⚠ RE-READ FROM THE .osp RATHER THAN KEPT BY THE CATALOG SCAN. The catalog
// already stores every preset's projectFile, and a card needs its set's rules
// once and then never again, so widening the scan's own structures to carry
// slider rules for 1628 slider sets to serve the handful a pane draws would be
// the expensive way round. Cached per set, because a pane of cards asks for
// the same set many times in a row.
//
// ⚠⚠ THE REFERENCE MESH IS NOT A MODEL PATH. It lives at
// CalienteTools\BodySlide\ShapeData\<DataFolder>\<SourceFile>, rooted at Data
// and not at meshes\, which is why NifModelLoader::LoadDataRelative exists.
// The BUILT mesh is the wrong file to morph: it already has whatever preset
// was last built baked into it, so applying deltas on top would stack two
// shapes.

#include "BodyMorphData.h"

#include <cstddef>
#include <string>
#include <vector>

namespace OS::BodyMorphSource {

    // One geometry inside the reference mesh, and the slider rules that drive
    // THAT geometry.
    //
    // ⚠⚠ A BODY NIF IS NOT ONE SHAPE, AND EACH SHAPE HAS ITS OWN DELTAS. A
    // .osd run is indexed by the shape it was measured against, so the body's
    // run is meaningless on any other geometry in the file. This load order's
    // male body carries VirtualArms, VirtualBelly, VirtualBreasts and
    // VirtualButt beside the body itself, physics colliders of 414, 139, 302
    // and 358 vertices next to a 14597 vertex body, and 3BA carries
    // 3BA_Vagina (1905) and 3BA_Anus (201) beside its 18436 vertex body. One
    // field for all of them shreds every one that is not the body.
    //
    // ⚠ THE .osp SAYS WHICH SHAPE EACH RUN IS FOR, so none of this is
    // guessed: a <Data> element carries target="3BA_Vagina" beside the run
    // name, and a slider names one run per shape it drives.
    struct ShapeRules {
        std::string                shape;  // the .osp's <Shape target="...">
        BodyMorphData::SliderRules rules;  // this shape's own <Data> runs
    };

    struct Source {
        // Data-relative, for NifModelLoader::LoadDataRelative.
        std::string              referenceMesh;
        // In the .osp's own order, which puts the body first on every set
        // measured here. Never empty for a usable source.
        std::vector<ShapeRules>  shapes;
        BodyMorphData::DeltaSets deltas;

        [[nodiscard]] bool Usable() const {
            return !referenceMesh.empty() && !shapes.empty() && !deltas.empty();
        }

        // How many slider rules the whole set carries, for the log lines that
        // used to read rules.size().
        [[nodiscard]] std::size_t RuleCount() const {
            std::size_t n = 0;
            for (const auto& s : shapes) {
                n += s.rules.size();
            }
            return n;
        }
    };

    // The set named by a_setName inside the .osp at a_projectFile. Null when
    // the project cannot be read, names no such set, or its .osd fails the
    // integrity check in BodyMorphData::Parse. Every one of those is a card
    // that does not draw rather than a card that draws something wrong.
    //
    // The returned pointer is owned by the cache and stays valid for the
    // session. Render thread only, like the rest of the preview build.
    [[nodiscard]] const Source* For(const std::string& a_projectFile,
                                    const std::string& a_setName);

    // The same shape from the RUNTIME morph file instead of the project, for a
    // preset whose slider set is not installed.
    //
    // ⚠⚠ THIS IS THE PATH THE GAME ITSELF USES, which is why it can answer
    // when the project cannot. Clicking a crossed card reshaped the character
    // perfectly while the card drew a cross, because RaceMenu's BodyMorph
    // never reads the .osd: it applies morphs by slider NAME out of the .tri
    // that ships beside the built body. HIMBO ships malebody.tri with no .osp
    // anywhere in the load order (measured 2026-08-10).
    //
    // a_bodyMeshPath is a model path relative to meshes\, exactly as a form
    // carries it, so the caller loads it with NifModelLoader::Load and NOT the
    // Data-relative entry point the BodySlide reference mesh needs.
    //
    // ⚠ THE REFERENCE HERE IS THE BUILT BODY, not a neutral reference mesh,
    // and that is the one real difference between the two sources. The built
    // file already carries whatever preset was last built, and the runtime
    // morphs stack on top of it in game for exactly the same reason, so the
    // card reproduces what the player sees rather than what BodySlide would
    // produce from scratch. Worth a field look before it is called correct.
    //
    // ⚠ NO .osp MEANS NO SLIDER DEFAULTS. A rule is synthesised per morph with
    // small and big both zero, so a slider the preset never names contributes
    // nothing. That is what BodyMorph does too: it applies what it is told and
    // leaves the rest of the built body alone.
    [[nodiscard]] const Source* ForRuntime(const std::string& a_bodyMeshPath);

    // Drop everything. For a catalog rescan, so an install that changed on
    // disk is not served from a stale parse.
    void Clear();

}  // namespace OS::BodyMorphSource
