#pragma once

// Startup address self-check - the whole diagnosis for a runtime we do not
// have. See VersionCheck.cpp for why every part of it exists.
//
// ⚠ NEAR-TWIN OF Menu Studio's src/VersionCheck.cpp, which shipped first and
// does not have Anchor. Once both are field-verified the common part should be
// extracted to one vendored header rather than kept as two copies; until then
// a fix here probably belongs there too.
namespace OS::VersionCheck {
    // Run once from SKSEPluginLoad, BEFORE any hook installs.
    void Run();

    // TRUE when both biped hook sites were located. They are what Fitting Room
    // IS, so a false here means "do not load".
    bool CriticalOk();

    // Exact-membership test against the running build's Address Library.
    // ⚠ NOT the same question as "REL::Relocation gave me an address".
    // CommonLib's id2offset does a lower_bound and, on SE/AE, only fails when
    // the id is past the END of the database - an id that is simply ABSENT
    // resolves to its NEIGHBOUR's offset with no error at all.
    bool IdOk(std::uint64_t a_id);
    bool IdOk(const REL::RelocationID& a_id);

    // Located call sites, relative to their containing function. 0 = absent,
    // and callers MUST treat it as "do not install this hook".
    std::ptrdiff_t WornPassCallOffset();   // BipedHooks::InstallInjection
    std::ptrdiff_t WornMaskCallOffset();   // BipedHooks::InstallWornMaskShim
    std::ptrdiff_t InputBlockCallOffset(); // ImGuiOverlay input-block hook
    std::ptrdiff_t WeaponPlayerCallOffset(); // WeaponHooks, player equip
    std::ptrdiff_t WeaponNpcCallOffset();    // WeaponHooks, NPC equip
    std::ptrdiff_t QuiverCallOffset();       // WeaponHooks, quiver (OS-77)
}
