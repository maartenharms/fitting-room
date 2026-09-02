#pragma once

// One skin pack, turned into the scene that photographs it (OS-212).
//
// ⚠⚠ THE CARD IS THE CHARACTER'S OWN PARTS WITH THE PACK'S FILES ON THEM, not
// a picture of a texture file. A body diffuse in UV space is a skin coloured
// blob that reads as "a file", and the thing being chosen is what the pack
// looks like ON this character. So the scene is a body card's composition,
// torso first and the head, hands and feet after it as subject roots
// (Mannequin::BodyPath and SubjectExtras), of kind kSkin: framed as a whole
// figure, exempt from the garment filters, and DRAWN TEXTURED where a body
// card is grey (PreviewGrid::DrawsGreyBody).
//
// ⚠⚠ THE PACK REACHES THE PICTURE BY THE LIVE RULE AND NO OTHER. SkinApi puts
// a pack on by matching FILE NAMES against what each shape's texture set
// reads; the card swaps by the same rule (TextureSwapEntry::whenTex0Name), so
// a shape the pack would leave alone in game (3BA's genital shapes, whose
// diffuse is femalebody_etc_v2_1.dds) is left alone on the card too, and a
// card can never show a skin the click would not put on.
//
// Pure: no engine type, so the identity a card gets is pinned by test. The
// caller resolves the part paths on the main thread and hands them in.

#include "PreviewGrid.h"
#include "SkinPlan.h"

#include <string>
#include <vector>

namespace OS::SkinCardScene {

    // The identity of one card. a_paths is the torso and its extras in the
    // order the mannequin resolved them; empty gives a scene with no roots,
    // which the card draws as its cross. a_pack null is the game's own skin:
    // the same figure with nothing swapped, which IS the picture of the
    // default on this rig.
    [[nodiscard]] inline PreviewGrid::SceneIdentity Build(const std::vector<std::string>& a_paths,
                                                          const SkinPlan::Pack* a_pack) {
        PreviewGrid::SceneIdentity id;
        id.kind       = PreviewGrid::SceneKind::kSkin;
        id.editorId   = "skin:" + (a_pack ? a_pack->id : std::string{});
        id.modelPaths = a_paths;
        // No mannequin composed (MannequinFor says none for kSkin), no morph,
        // every path an ordinary form model path under meshes\.
        id.mannequinPathCount  = 0;
        id.morphPathCount      = 0;
        id.dataRootedPathCount = 0;
        if (a_pack && !a_pack->files.empty() && !a_paths.empty()) {
            // Every file the pack carries, as a by-name entry: whichever shape
            // in whichever part reads a diffuse of that name takes it. A
            // normal or a specular in the pack names no shape's TX00 and
            // matches nothing, which costs a few bytes of key and no pixels.
            std::vector<PreviewGrid::TextureSwapEntry> list;
            list.reserve(a_pack->files.size());
            for (const auto& [name, path] : a_pack->files) {
                PreviewGrid::TextureSwapEntry e;
                e.whenTex0Name = name;
                e.texPaths[0]  = path;
                list.push_back(std::move(e));
            }
            // The same list on every root: the body reads the body file, the
            // hands the hands file, and each ignores the rest.
            id.swaps.assign(a_paths.size(), list);
        }
        return id;
    }

}  // namespace OS::SkinCardScene
