#pragma once

namespace RE {
    class Actor;
}

// OS-241: Fitting Room builds the player's FIRST PERSON overlay clones,
// because the installed skee never does.
//
// ⚠⚠ THE INSTALL HALF IS WHAT IS BROKEN, AND IT WAS MEASURED BEFORE ANY OF
// THIS WAS WRITTEN. The census that shipped with `5a4baa5` read the same
// answer on every refresh of the 2026-08-21 field round: four `Hands [Ovl]`
// clones on the third person root with `Ovl0` wearing the player's art, and
// zero clones of any location on the first person root. The apply half is
// therefore healthy, and no re-attach FR can drive changes the install half:
// skee's `AddOverlays` refuses the player outright ("Cannot add to player,
// already exists") and `RemoveOverlays` refuses him too, which is why all
// three public calls tried during OS-233 created nothing. The only remaining
// move is FR making the clones.
//
// ⚠⚠ THE NAMES ARE skee'S AND THAT IS THE WHOLE DESIGN. A clone named
// exactly `Hands [Ovl0]` is found by skee's own `Impl_SetNodeProperties`,
// which walks BOTH roots by `GetObjectByName`, so the entire paint path
// arrives free: FR's per-edit write push paints these, RaceMenu's own sliders
// reach them, and skee's install checks by name before creating, so a future
// skee that did install on the first person would reuse FR's node rather than
// doubling it. FR builds geometry and attaches it. It never paints.
//
// ---- WHAT A CLONE IS, READ OUT OF skee'S OWN SOURCE ------------------------
//
// `OverlayInterface::InstallOverlay` (expired6978/SKSE64Plugins, skee64/,
// master, last touched 2023-06-07 and so the 0.4.20 era) builds one from four
// parts and nothing else:
//
//   * a fresh shape of the source's class, named for the node;
//   * the SOURCE's `vertexDesc`, `m_localTransform` and `m_spSkinInstance`;
//   * the TEMPLATE NIF's shader property and alpha property;
//   * texture slots 1 through 7 copied off the source skin's material.
//
// ⚠⚠ IT COPIES NO VERTEX DATA AT ALL, AND THAT IS NOT AN OVERSIGHT. A skinned
// `BSTriShape` in Special Edition keeps its vertices and its triangles in the
// `NiSkinPartition` rather than on the shape, so sharing the source's skin
// instance hands the clone the entire geometry, its bones and its partition in
// one pointer. That is also why the two neighbouring scars do not apply here:
// skin-bone-indices-are-skin-global and mismatched-weight-meshes-crash-the-
// blend are both about REMAPPING a skin, and nothing here remaps anything. The
// clone and its source are one skin.
//
// ⚠⚠ THE MATERIAL MUST COME OFF A FRESH TEMPLATE LOAD, ONE PER CLONE. The
// engine's clone shares its properties with what it was cloned from, so a
// clone that kept them would hand skee the player's SKIN material to paint the
// overlay art onto. Loading skee's template again per node is what gives each
// layer a material of its own, and it is measured rather than assumed:
// `BSLightingShaderProperty::LoadBinary` assigns a fresh material straight to
// +0x78 without the pool (the working is in OverlayBake.h). So shader-
// materials-are-pooled-never-write-one still holds everywhere else and does
// not hold here.
//
// ⚠ THE ALPHA NEEDED NO WORK AND THAT WAS CHECKED, NOT GUESSED. skee overrides
// every overlay's alpha with `iAlphaFlags` and `iAlphaThreshold` from
// skee64.ini. This rig's INI holds 4845 and 0, and the template NIF pulled out
// of RaceMenu.bsa already carries a `NiAlphaProperty` of 0x12ED (4845) with a
// threshold of 0. The two agree, so taking the template's property is taking
// the override's value.
//
// ⚠ BARE HANDS FOR ANY FIELD TEST. The clone source is the first
// `kFaceGenRGBTint` geometry under the slot, and the Callisto glove NIF has no
// such shape, so a gloved hands slot holds no hand overlay at all. That is
// skee's own rule (its `SetupOverlay` else-branch uninstalls) and correct
// behaviour rather than a bug.
namespace OS::Overlay1P {

    // Build every missing Body and Hands `[Ovl]` clone under the player's
    // first person root, then ask skee to paint them once.
    //
    // Game thread. Player only. Idempotent by the same name check skee uses,
    // so calling it at every settled point costs one `GetObjectByName` per
    // layer once the clones are there.
    //
    // Returns how many clones were attached by this call; 0 means either
    // nothing was missing or nothing could be built.
    int PaintPlayer(RE::Actor* a_actor);

}  // namespace OS::Overlay1P
