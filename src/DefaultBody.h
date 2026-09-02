#pragma once

#include "PersistenceCodec.h"  // DefaultBodyRecord, the 'DFBD' row shape

#include <cstddef>
#include <vector>

#include <cstdint>
#include <string>
#include <string_view>

namespace RE {
    class Actor;
}

// A character's DEFAULT body: the preset they wear whenever the outfit they
// have on does not name one of its own.
//
// ⚠ A FILE, NOT THE CO-SAVE, AND THAT IS A BEHAVIOURAL CHOICE RATHER THAN A
// SHORTCUT. The alternatives were a new co-save record or a field appended to
// 'NPCO', and either would make the default per SAVE. This is not captured
// state like the OBody baseline in 'BBAS' - that records what a character
// looked like BEFORE this mod touched them, which is only meaningful inside
// the save that captured it. A default body is a decision the player made
// about who a character IS, and Jenassa is the same Jenassa in every save, so
// it lives beside the presets and dye schemes it refers to. It is also
// hand-editable and shareable for the same reason those are.
//
// ⚠ THE KEY IS PLUGIN + LOCAL FORM ID, NEVER A RAW FORM ID. A runtime form id
// carries the load order's index in its top byte and changes the moment a
// plugin moves, so a file full of them would silently rebind one character's
// body onto another. Same rule the hair-style reference and the follower
// baselines already follow.
namespace OS::DefaultBody {

    struct Entry {
        std::string installed;  // an installed OBody preset NAME
        std::string customId;   // or a Body Studio custom preset's stable id
        [[nodiscard]] bool Empty() const { return installed.empty() && customId.empty(); }
    };

    // ---- persistence, owned by the co-save ---------------------------------
    //
    // ⚠ THIS USED TO BE default-bodies.json AND THAT WAS A CURRENCY EXPLOIT.
    // The full argument, and the rule it comes from, is on DefaultLook.h: a
    // thing the player PAYS FOR must live in the same place as the currency
    // they paid with. Record 'DFBD'.
    [[nodiscard]] std::vector<DefaultBodyRecord> Snapshot();
    void Restore(const std::vector<DefaultBodyRecord>& a_rows);
    void ClearAll();
    std::size_t ImportLegacyFile();

    // The default for this actor, or an empty Entry when they have none.
    [[nodiscard]] Entry For(RE::Actor* a_actor);

    // ⚠ CUSTOM ID WINS WHEN BOTH ARE GIVEN, matching RestoreStagedBody's own
    // precedence, so the two cannot disagree about which preset a character has.
    void Set(RE::Actor* a_actor, std::string_view a_installed, std::string_view a_customId);

    // Removing the default falls the character back to their captured baseline,
    // which is exactly what they did before one was ever set. The feature stays
    // reversible by construction.
    void Clear(RE::Actor* a_actor);

    [[nodiscard]] bool Has(RE::Actor* a_actor);

}  // namespace OS::DefaultBody
