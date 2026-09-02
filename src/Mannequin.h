#pragma once

// The preview grid's mannequin (OS-204): the body and head a card's item is
// shown ON, resolved once per fit target and composed into a scene by rule.
//
// ⚠ IT ARRIVES AS MODEL PATHS AND THAT IS THE DESIGN, not an implementation
// detail. DiskKeyFor already folds every entry of modelPaths, and the body a
// race and sex resolve to IS a distinct path, so race and sex reach the key by
// the route garments already use. No key field, no renderer bump, and a scene
// that composes nothing keys byte-identically to the one that built the
// thumbnail already on disk (pinned in the grid suite).
//
// The rules for WHICH parts a scene earns are pure and live in PreviewGrid.h
// (MannequinFor, MannequinPartShows); this is the engine walk that turns a
// race, a sex and a skin into the paths those rules select from.

#include "PreviewGrid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class TESObjectARMO;
    class TESRace;
}

namespace OS::Mannequin {

    // Resolve the parts for one fit target. Called from the fit walk, which
    // already knows the race and sex and already runs on the main thread.
    // Passing a null skin or race clears, which is what a target with neither
    // should render: today's card.
    void Refresh(RE::TESObjectARMO* a_skin, RE::TESRace* a_race, int a_sexIdx);

    void Clear();

    // Prepend the parts this scene has earned, set mannequinPathCount, and
    // keep the parallel vectors aligned. Idempotent by construction: it is
    // called on the identity the cache stamps, once, before the key.
    void Compose(PreviewGrid::SceneIdentity& a_id);

    // For the log line and for tests of the caller: how many parts resolved.
    [[nodiscard]] std::size_t PartCount();

    // The head, hands and feet WITHOUT the torso, for a body card.
    //
    // ⚠ THESE ARE SUBJECT PATHS, NOT MANNEQUIN PATHS, and the difference is
    // the framing rather than the picture. A mannequin's head and feet are
    // drawn but never measured, so the reference box stays the same size when
    // footwear drops the feet; on a body card the head and feet ARE the
    // subject and a box that ignored them would frame the torso and crop the
    // head off the top. So they go on the end of modelPaths as ordinary
    // roots, they get measured with the body, and mannequinPathCount stays 0.
    //
    // ⚠⚠ THE MORPH MUST NOT REACH THEM. The .osd is indexed by the BODY
    // shape's vertices and a hand NIF has its own count, so applying the field
    // to these would scramble them exactly the way the partition map did. That
    // is what SceneIdentity::morphPathCount is for.
    //
    // Race and sex reach the disk key through these paths, the same route the
    // mannequin and every garment already use, so no key field is added.
    [[nodiscard]] std::vector<std::string> SubjectExtras();

    // The TORSO alone, the part SubjectExtras deliberately leaves out.
    //
    // A body card normally photographs a BodySlide reference mesh, but a
    // preset whose slider set is not installed has no such mesh. What it does
    // have is the body the character is actually wearing, which ships its own
    // runtime morph file; see BodyMorphSource::ForRuntime. Empty when no skin
    // resolved.
    [[nodiscard]] std::string BodyPath();

}  // namespace OS::Mannequin
