#pragma once

// Whether a stored face answer is a preference or a leftover, split out of
// Settings.h so a test can reach it, the way UiScaleMigration.h is.
//
// ⚠⚠ THE PROBLEM THIS EXISTS FOR: the three face keys shipped OFF from
// 2026-08-29 to 1.1.6 (bLooks, bReassertAppearance, bLooksRaceMenu, one
// decision in three keys), because two players lost a face to them on 1.1.2.
// The face arc that closed on 2026-09-01 fixed what turned them off, and the
// user's call on 2026-09-02 is that they ship ON. Settings::Save had pinned the
// 0 into every file that ever loaded, and the FOMOD's rewrite reaches only a
// mod-manager install: a hand-copied ini, or one the plugin wrote itself into
// Overwrite on a first run without an installer file, keeps the 0 for good
// (live-ini-overrides-code-defaults). The stamp is what tells the two apart:
// a stored 0 under an old stamp is the installer's answer of its day, not the
// player's.
namespace OS::LooksOn {

    // Bumped when this migration was added. Settings::kSettingsVersion must be
    // at least this, and Settings.cpp static_asserts it.
    //
    // ⚠ A 0 UNDER THIS STAMP IS A CHOICE AND STAYS. The panel's Looks tick and
    // any ini edit are saved with the current stamp, so the flip happens once
    // per install and never again, whatever the player then picks. The cost is
    // named rather than hidden: a player who chose "Leave my face alone" in the
    // 1.1.3 to 1.1.6 installer is moved too, once, and the ini comments and the
    // changelog say so.
    inline constexpr int kSettingsVersion = 4;

    // What a stored face key should become on load.
    [[nodiscard]] inline constexpr bool Adopt(bool a_stored, int a_iniVersion) {
        if (a_iniVersion >= kSettingsVersion) {
            return a_stored;  // stamped, so this is a choice
        }
        return true;  // the installer's old answer, moved onto the new one
    }

}  // namespace OS::LooksOn
