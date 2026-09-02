#pragma once

#include "BodyStudioProof.h"

#include <string>
#include <string_view>
#include <vector>

namespace RE {
    class Actor;
}

// ⚠⚠ THE PAGE IS CALLED "Bodies" ON SCREEN AND "BodyStudio" IN THE CODE, AND
// THE SPLIT IS DELIBERATE (user 2026-08-08). The player-facing word changed;
// nothing else did. Identifiers, file names, the FR_BODY_STUDIO build option
// and every channel string still say BodyStudio.
//
// ⚠ THREE OF THOSE STRINGS ARE STORED DATA AND MOVING ANY OF THEM DESTROYS IT,
// which is why the rename stopped at the tooltip. All three live in
// src/BuildChannel.h:
//
//   'FBSD'                                  the dev channel's co-save owner.
//                                           Change it and every body preset
//                                           saved on that channel is orphaned.
//   FittingRoom.BodyStudioDev.<id>.xml      the BodySlide export name. Change
//                                           it and exports players already
//                                           have in BodySlide are stranded.
//   FittingRoom.BodyStudioDev.<id>.log      cosmetic on its own, but it pairs
//                                           with the two above and splitting
//                                           the trio is how one gets missed.
//
// Settings' bBodyStudio and bBodyStudioProof INI keys are the same argument in
// miniature: renaming a key is renaming storage, so the keys stayed and their
// comments were reworded instead.
//
// A rename that reaches the co-save is not a rename, it is a migration.
namespace OS::BodyStudioUI {

    void OnOpen(const std::vector<std::string>& a_installedPresetNames);
    void OnPresetListChanged(const std::vector<std::string>& a_installedPresetNames);
    void OnClose(RE::Actor* a_actor);

    // Is the preset draft different from what is on disk? Leaving the page does
    // not discard it, but the page is where the only Save is, so the editor
    // asks before taking the user somewhere they cannot press it.
    [[nodiscard]] bool HasUnsavedChanges();

    // Draws the complete page below EditorUI's shared title/target row.
    void Draw(RE::Actor* a_actor, bool a_actorFemale,
              BodyStudioProof::ORefitPolicy a_policy,
              std::string_view a_currentInstalledPreset,
              std::string_view a_currentCustomPresetId);

}  // namespace OS::BodyStudioUI
