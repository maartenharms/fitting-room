#pragma once

// PCH.h explicitly, matching every other engine-type-using header here
// (StyleRef.h, REAugments.h, ...): the build's per-target PCH makes it
// redundant inside this project, but a header naming RE:: types must not
// depend on its includer's include order to compile.
#include "PCH.h"

#include <cstdint>

namespace OS {

    // Every biped slot a style ARMO puts geometry on. The ARMO's own slot
    // mask is the DECLARED coverage; each ARMA carries its own, and they
    // routinely differ (a cuirass declaring 32 whose armature also owns 34 -
    // the OS-83 case). Both matter, because it is the union that decides
    // which slots the engine's staging actually takes over.
    //
    // Within BipedHooks.cpp this keeps the styling passes and the worn-mask
    // shim from disagreeing about what a style covers; the editor's claims
    // cache needs the identical computation. ONE definition shared by all of
    // them - two copies that could drift is exactly the bug class this
    // exists to close.
    [[nodiscard]] inline std::uint32_t StyleCoverageOf(RE::TESObjectARMO* a_armo) {
        if (!a_armo) {
            return 0;
        }
        auto coverage = static_cast<std::uint32_t>(a_armo->GetSlotMask());
        for (auto* arma : a_armo->armorAddons) {
            if (arma) {
                coverage |= static_cast<std::uint32_t>(arma->GetSlotMask());
            }
        }
        return coverage;
    }

}  // namespace OS
