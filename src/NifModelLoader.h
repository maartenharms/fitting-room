#pragma once

#include <string>

// Loads a NIF into a node that is NEVER attached to the scene graph.
//
// ⚠ THE STANDALONE-LOAD INVARIANT IS A HARD REQUIREMENT, not a detail: the
// preview pipeline reads materials reached from this node, which is safe
// only because nothing in the running game renders them. If a preview is
// ever built from the live player's 3D, that assumption dies and takes a
// running material with it.
//
// ⚠⚠ READS, NEVER WRITES, AND THE TREE BEING STANDALONE DOES NOT SOFTEN
// THAT: shader materials are POOLED through a global manager
// (BSShaderProperty::SetMaterial dedups; re_verify/ae_crash_chain.c), so a
// material reached from this node can be THE instance the live world
// shares. Writing one from here nulled a live eye material's env textures
// and crashed the render pass (field 2026-08-09). Wardrobe's original
// wrote materials from its copy of this loader; that part must never be
// ported.
namespace OS::NifModelLoader {

    // Null on: ids absent from this runtime's Address Library, a missing or
    // unparseable file, no node among the top objects, or the latch below
    // having tripped. Render thread only, like every caller in the preview
    // pipeline.
    [[nodiscard]] RE::NiPointer<RE::NiNode> Load(const std::string& a_modelPath);

    // The same load for a path already rooted at Data rather than at meshes\.
    //
    // ⚠ A SEPARATE ENTRY POINT RATHER THAN SNIFFING THE PATH. Almost every
    // path here is a form's model path, authored relative to meshes\, and
    // Load re-roots it. BodySlide's REFERENCE meshes are not model paths: they
    // live at CalienteTools\BodySlide\ShapeData\..., which Load would ask
    // for under meshes\ and never find. Deciding from the leading directory
    // would work today and break on the first mod shipping a mesh under a
    // folder of the same name, so the CALLER says which root it means.
    //
    // ⚠ The standalone-load invariant at the top of this file applies here
    // unchanged: this tree is never attached, and nothing reached through it
    // may be written.
    [[nodiscard]] RE::NiPointer<RE::NiNode> LoadDataRelative(const std::string& a_dataPath);

    // True once the guard bytes behind the hand-built stream have ever been
    // found overwritten. The heap is already damaged at that point, so the
    // whole preview subsystem stands down for the session rather than
    // continuing to gamble. Wardrobe logs the trip and keeps going; that is
    // the one part of its loader deliberately not ported.
    [[nodiscard]] bool Latched();

}  // namespace OS::NifModelLoader
