#pragma once

// r55-r57, the whole arc in three sentences: CBPC mutes breast amplitude by
// the REAL worn chest piece's armor class (the winning Petite config zeroes
// Heavy), Fitting Room renders a different piece, and the player reads that
// as "the costume killed the physics". The r56/r57 keyword route - tagging
// the worn ARMO with runtime-created CBPCAs* keywords - was field-falsified:
// the tag landed, the log proved it, cbp.dll never honored it. And the
// equip-path NiNodeUpdate that rode along destabilized the unequip 3D storm
// (r57 froze the game).
//
// What CBPC actually offers, in its own CBPCPluginScript.psc:
//   ApplyBounceInterpolation(Actor, String uniqueName, int percentage)
// - a per-ACTOR override that blends the amplitudes from
// CBPCBounceinterpolationconfig_<uniqueName>.txt onto that actor. The
// shipped _Test profile zeroes amplitudes the same way. So while a Fitting
// Room outfit is SHOWING a chest piece (style or hide), the player gets the
// FittingRoom profile at 100 - the rig's baseline amplitudes - and when the
// chest is passthrough or no outfit is active, percentage 0 hands the read
// back to the worn gear. Per-actor, no forms touched, nothing persists.

#include <cstdint>

namespace OS::CbpcArmorClass {

    // Whether CBPC's script was there the last time we tried to reach it.
    //
    // ⚠⚠ THREE STATES AND NOT A BOOL, because "we have not asked yet" is not
    // the same answer as "it is not installed" and a page that greys a control
    // must not do it on the strength of a question nobody asked. DispatchStaticCall
    // already returns this; it was only ever going to the log.
    enum class Support : std::uint8_t { kUnknown, kPresent, kAbsent };

    [[nodiscard]] Support SupportState();

    // Register the equip sink. kDataLoaded.
    void Install();

    // Re-derive whether the shown chest should own the bounce read and
    // dispatch ApplyBounceInterpolation on a change. Game-thread only (call
    // from an SKSE task or a sink). Never touches 3D and never fires
    // NiNodeUpdate - that is what froze r57.
    void AssertPlayer();

    // The forced variant for the tail of CbpcRefresh's apply ladder: a
    // Refresh-pair rebuild may have thrown the actor's interpolation state
    // away, so this re-dispatches the current want even when it "already
    // holds". Only a want of 100 spends the VM call.
    void Reassert();

}  // namespace OS::CbpcArmorClass
