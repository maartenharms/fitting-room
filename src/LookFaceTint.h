#pragma once

#include <string>

namespace RE {
    class Actor;
}

// The CharGen jslot whose EXPORTED face tint this character wears, held by
// Fitting Room because nothing else puts it back.
//
// ⚠⚠ THE FAULT, FIELD 2026-08-25 16:5x. A look whose face comes from a preset
// carries its painted detail in one baked file:
//
//     Textures\CharGen\Exported\FR_<jslot>.dds
//
// ProfileApply's 'makeup' step stands aside on purpose when a look carries a
// face block, because a preset face arrives with its tints already baked in, so
// the player's 34-slot tint list is never given layers that could rebuild it.
// The measurement is the same head either side of a quit to desktop:
//
//     apply   tintTex = 'Textures\CharGen\Exported\FR_Umbrael 17.dds'
//     reload  tintTex = 'Player face tint'
//
// The load's head build comes up with NO tint bound at all, MakeupApi's owed
// rebake composites 'Player face tint' from the empty list, and the face comes
// back without its skull. ⛔ It is not an overlay: 'Face [Ovl0]' reads the same
// third-party file on both sides and OverlayBaseline correctly stands down.
//
// ⚠ THIS IS THE TENTH BLOCK OF A LOOK and the same shape as the other nine.
// Fitting Room sets an attribute, the attribute lives somewhere a load clears,
// nothing puts it back. Hold it, save it, put it back.
//
// ⚠⚠ THE BIND IS A skee NODE OVERRIDE, NOT A MATERIAL WRITE. Shader materials
// are pooled and writing one in place is the OS-192 crash; the supported route
// is override key 9 slot 6, skee's `renderedTexture` for a faceGen shader,
// which is the tint composite. ⚠ Nothing re-applies a node override after a
// head build, so Apply has to run from HeadBuildHook, the seam
// SkinApi::ReapplyHead already uses for the same reason.
namespace OS::LookFaceTint {

    // A look's face block was applied. Called from the one place that applies
    // it, so the record cannot drift from what was bound.
    void Hold(std::string a_jslot);

    // What is held, or false when nothing is. a_jslot is untouched on false.
    [[nodiscard]] bool Held(std::string& a_jslot);

    // A load boundary: this belongs to the outgoing character. Same reasoning
    // as CharacterSex::Clear, and for a sharper reason here: the export lives
    // under a shared folder keyed by look name and not by character, which is
    // exactly how the cross-save face bleed happened.
    void Clear();

    // Bind the held export onto the player's face geometries. Returns true only
    // if a path was actually written, which is what lets the owed face rebake
    // stand down rather than compositing over it.
    //
    // ⚠ Safe to call when nothing is held, when the file is missing, or before
    // the face exists: all three answer false.
    bool Apply(RE::Actor* a_actor);

    // The file a held jslot names, or empty when nothing is held. Exposed for
    // the log line and the tests; the resolution rule lives in one place.
    [[nodiscard]] std::string PathFor(const std::string& a_jslot);

}  // namespace OS::LookFaceTint
