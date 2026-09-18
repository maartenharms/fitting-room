#pragma once

// The one name contract NpcHair and the head-part dye walk share.
//
// The engine names a head part's geometry by the part's editor id, stamped
// from BGSHeadPart's formEditorID, hers and ours alike. Hair packs share extra
// parts across styles, so a follower whose own hair is family to the style we
// attach ends up with two objects under one name (OS-246, the bald follower).
// NpcHair's answer is that OUR pieces stop wearing engine names: every root it
// attaches is renamed to the prefix below plus the engine's name, and from
// then on a bare name can only resolve HERS and a prefixed one only OURS.
//
// ⚠ TWO READERS OF ONE PREFIX, which is why it lives here rather than in
// NpcHair.cpp's anonymous namespace where it started. The head-part dye walk
// (OutfitDye.cpp, 2026-09-04) finds a follower's style by these names and
// reads the part back out of each; a second spelling of "FR|" anywhere is how
// her ornament goes undyed with nothing in the log.
//
// ⚠ NO ENGINE TYPES IN THIS HEADER, the DyeRamp.h rule: HeadPartPlanTests
// compiles it with no engine, and the contract is pinned there.

#include <optional>
#include <string>
#include <string_view>

namespace OS::NpcHairNames {

    inline constexpr std::string_view kOwnPrefix = "FR|";

    // What ApplyNow names a root it attached for the part with this editor id.
    [[nodiscard]] inline std::string OwnRootName(std::string_view a_edid) {
        std::string out{ kOwnPrefix };
        out += a_edid;
        return out;
    }

    // The editor id under one of our root names, or nothing when the name is
    // not ours: hers (engine named), empty, or the prefix over nothing, which
    // no part could have produced.
    //
    // ⚠ AN OWNING STRING, NOT A VIEW INTO a_name. The natural return here is
    // a substr view, and the first test written against it held one past the
    // temporary it pointed into. A root name is short and a walk asks once
    // per root, so the copy costs nothing worth a dangling reference.
    [[nodiscard]] inline std::optional<std::string> EdidOfOwnRoot(std::string_view a_name) {
        if (a_name.size() <= kOwnPrefix.size() || !a_name.starts_with(kOwnPrefix)) {
            return std::nullopt;
        }
        return std::string{ a_name.substr(kOwnPrefix.size()) };
    }

}  // namespace OS::NpcHairNames
