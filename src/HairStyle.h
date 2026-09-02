#pragma once

#include "HeadPart.h"

#include <optional>
#include <vector>

// Hair STYLE for the player, as distinct from hair colour (HairColor.h).
//
// Since OS-161 this is a thin forwarder onto HeadPart, which is this module's
// own machinery generalised over the part type so eyes and brows could reuse
// it. Hair therefore runs exactly the code it always ran rather than a copy
// that can drift, and a fix here reaches all three. The names are kept because
// hair has call sites, a persisted Outfit field and a log history under them.
//
// ⚠ PLAYER ONLY, and HeadPart::Apply now enforces that rather than asking
// callers to. The reason is the head rebuild that puts the change on screen:
// on a named NPC it hands her a different complexion, and no repaint can put
// the right one back. NpcHair.h owns the follower route and explains why it is
// a different mechanism rather than a scoped version of this one.
//
// ⚠ A REASON THAT USED TO SIT HERE WAS MEASURED WRONG, and it is worth knowing
// which, because it is a tempting inference. This header argued for a year that
// a follower's hair could not be reached at all, since her head comes from a
// baked FaceGeom mesh and a record change would therefore never render. A full
// dump of Jenassa's 22 head geometries on 2026-07-31 said otherwise: only the
// FACE is baked, her hair carries a vanilla name matching her record exactly,
// and ChangeHeadPart does reach it (confirmed on screen). What a FaceGeom NIF
// contains is not what the engine renders for hair. The conclusion survived the
// correction, but on the rebuild's cost rather than on reachability.
namespace RE {
    class Actor;
    class BGSHeadPart;
}

namespace OS::HairStyle {

    using Entry = HeadPart::Entry;

    [[nodiscard]] inline std::vector<Entry> AvailableFor(RE::Actor* a_actor) {
        return HeadPart::AvailableFor(a_actor, HeadPart::Kind::kHair);
    }

    [[nodiscard]] inline RE::BGSHeadPart* CurrentFor(RE::Actor* a_actor) {
        return HeadPart::CurrentFor(a_actor, HeadPart::Kind::kHair);
    }

    inline bool Apply(RE::Actor* a_actor, RE::BGSHeadPart* a_part) {
        return HeadPart::Apply(a_actor, HeadPart::Kind::kHair, a_part);
    }

    inline bool Restore(RE::Actor* a_actor) {
        return HeadPart::Restore(a_actor, HeadPart::Kind::kHair);
    }

    [[nodiscard]] inline std::optional<Entry> CapturedFor(RE::Actor* a_actor) {
        return HeadPart::CapturedFor(a_actor, HeadPart::Kind::kHair);
    }

    // ⚠ CLEARS EVERY KIND, not just hair, and that is correct for its one
    // caller. It runs from the save-revert callback, where the character all
    // three captures describe is being torn down and replaced.
    inline void Clear() { HeadPart::Clear(); }

}  // namespace OS::HairStyle
