#pragma once

#include <string_view>

// Whether the installer's answers should be applied over what Settings::Load
// read, split out so a test can reach it, the way UiScaleMigration.h is.
//
// ⚠⚠ THE PROBLEM THIS EXISTS FOR, measured on the author's rig 2026-09-02 08:30:
// the FOMOD wrote its FittingRoom.ini into FittingRoom-1.1.7\SKSE\Plugins, and
// the game read and saved MODS\overwrite\SKSE\Plugins\FittingRoom.ini, which
// outranks every mod in Mod Organizer. So "Earn dyes" was answered in the
// installer and never arrived (user: "i don't think our fomod settings are
// updating from what we pick from the fomod"). A player is in the same place
// whenever the plugin ever wrote the ini itself, which lands it in Overwrite,
// or when an older copy sits in a mod above.
//
// The installer therefore ships the same file a second time, as
// SKSE\Plugins\FittingRoom\installer.ini, a path the plugin never writes and
// no leftover can shadow, and every flavour of it carries a stamp naming the
// zip and the answers. Load compares that stamp with the one the live file
// last took and, when they differ, copies the eight answers over and saves
// with the new stamp.
namespace OS::InstallerSeed {

    // ⚠ A SAME STAMP IS A NO-OP, AND THAT IS WHAT KEEPS THE PLAYER'S PANEL
    // EDITS. The stamp changes only with a new zip or a different set of
    // answers, so a setting changed in the panel after the install stays
    // changed for as long as that install stands. An empty installer stamp is
    // a hand-copied install with nothing to seed from.
    [[nodiscard]] inline constexpr bool ShouldSeed(std::string_view a_liveStamp,
                                                   std::string_view a_installerStamp) {
        return !a_installerStamp.empty() && a_installerStamp != a_liveStamp;
    }

}  // namespace OS::InstallerSeed
