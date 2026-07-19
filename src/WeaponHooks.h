#pragma once

namespace OS {

    // The weapon/quiver render override. Three call-site hooks on the part-3D
    // loader (SE 15526 / AE 15703) - the single funnel every weapon and ammo
    // mesh materializes through - swapping the MODEL ARGUMENT while the real
    // WEAP/AMMO stays staged. See WeaponHooks.cpp for the mechanism and for why
    // the model argument is the right seam.
    class WeaponHooks {
    public:
        static void Install();

        // False if ANY of the three sites failed its install-time byte check.
        // The sites are independent: a drifted weapon site costs the weapon
        // dimension on that path, never armor transmog.
        static bool AllInstalled();

        // Diagnostics: counts weapon/ammo part loads the thunks saw while
        // styling was live, so a refresh that silently never reaches the loader
        // is visible in the log. The tripwire for the AE inlined-site class of
        // bug (the address resolves, the byte check passes, the thunk never
        // runs) that cost us a release once. Mirrors BipedHooks::
        // PlayerWornPassCount. Two counters because the player and NPC weapon
        // sites are DISTINCT offsets (SE 15506+0x17F vs +0x1D0): each must be
        // provable-live independently, and the player tripwire stays
        // player-scoped. The NPC refresh path (RefreshActor) reads the NPC one.
        static std::uint64_t PlayerWeaponPartCount();
        static std::uint64_t NpcWeaponPartCount();

    private:
        // One record per site so a byte-check failure disables only its own.
        // The two weapon sites are the arms of one if/else on the actor, not
        // independent paths (see Install()).
        static inline bool weaponPlayerOk_{ false };  // SE 15506+0x17F / AE 15683+0x2B1
        static inline bool weaponNPCOk_{ false };     // SE 15506+0x1D0 / AE 15683+0x2FA
        static inline bool quiverOk_{ false };        // SE 15511+0x141 / AE 15688+0x199
    };

}  // namespace OS
