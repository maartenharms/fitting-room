#pragma once

#include "HeadPart.h"          // Kind, which Part mirrors
#include "Outfit.h"
#include "PersistenceCodec.h"  // DefaultLookRecord, the 'DFLK' row shape

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace RE {
    class Actor;
}

// A character's DEFAULT hair, eyes and brows: what they wear whenever the
// outfit they have on does not name one of its own.
//
// ⚠ THE SAME SHAPE AS DefaultBody, DELIBERATELY. A default body and a default
// hairstyle are the same kind of statement about a character, so they get the
// same storage, the same key and the same lifetime.
//
// ⚠⚠ THAT STORAGE IS THE CO-SAVE, AND IT USED TO BE A FILE. This header and
// DefaultBody.h both argued for a file beside the presets, on the reasoning
// that a default is "a decision about who a character IS, and Jenassa is the
// same Jenassa in every save". **That reasoning was overturned on 2026-08-08
// and it is not a matter of taste.** Setting a default costs a look, and the
// gold or Seamstone charge it costs lives in the co-save. A default living
// outside every save meant: pay, reload an earlier save, and you keep the
// default while the currency comes back. The user found it in one sentence:
// "you can load a save and then spend soulgems to edit and then go back to
// that save".
//
// **A thing the player PAYS FOR must live in the same place as the currency
// they paid with.** If a future default is free, that argument does not
// re-open the question on its own; the leak between playthroughs is the other
// half, and per-save is what a player expects of a new game.
//
// Records 'DFLK' (this) and 'DFBD' (DefaultBody), separate for the reason
// 'DYHI' is not part of 'DYES': one malformed block must not take the other
// down with it. Persistence.cpp owns the wiring; see PersistenceCodec.h.
//
// ⚠ THE KEY IS PLUGIN + LOCAL FORM ID, NEVER A RAW FORM ID, for the reason
// DefaultBody gives: a runtime form id carries the load order's index in its
// top byte, so a file full of them silently rebinds one character's face onto
// another the moment a plugin moves.
//
// ⚠ EACH PART IS INDEPENDENT. Setting a default eye colour must not disturb a
// default hairstyle, so the three are set and cleared one at a time and an
// entry with only one of them filled is normal rather than half written.
namespace OS::DefaultLook {

    struct Entry {
        StyleRefKey hair;
        StyleRefKey eyes;
        StyleRefKey brows;
        StyleRefKey facialHair;
        // The character's default hair COLOUR, and a fifth member of a new type
        // rather than a fifth key. A style is a head part, a mod plus a form id;
        // a colour is an RGB with a presence bit, the same HairTint the outfit
        // carries. They are independent in both directions: a character may be
        // always-red without a usual hairstyle, and always-this-braid without a
        // usual colour.
        HairTint    hairTint{};

        // Slots this load order invented (horns, cat ears), one entry per slot
        // at most.
        //
        // ⚠⚠ A COUNTED LIST RATHER THAN MORE MEMBERS, AND THAT IS WHAT
        // UNBLOCKED THIS. OutfitSession used to say there could be no character
        // default for an invented slot, because Part is a closed enum over four
        // kinds and a fifth would be "a third vocabulary to widen". The outfit
        // had already answered that for itself: a discovered slot is a RAW TYPE
        // NUMBER, so a list keyed by that number widens no enum and keeps no
        // vocabulary in step. Same shape, same rules, same reason
        // (user 2026-08-26: Umbrael canonically has horns).
        std::vector<CustomHeadPartRef> customHeadParts;

        // What this character normally wears in a slot, or an empty key for
        // "they have no usual one". Linear over a list holding one entry per
        // slot the player actually pinned, so a handful at most.
        [[nodiscard]] StyleRefKey CustomHeadPart(std::uint32_t a_slot) const {
            for (const auto& part : customHeadParts) {
                if (part.slot == a_slot) {
                    return part.key;
                }
            }
            return {};
        }

        // Pin a part to a slot, or unpin it with an empty key. Clearing ERASES
        // the entry rather than storing an empty one, which is what keeps
        // Empty() below honest and keeps a set-then-cleared character byte for
        // byte the same as one that never pinned anything.
        void SetCustomHeadPart(std::uint32_t a_slot, StyleRefKey a_key) {
            for (auto it = customHeadParts.begin(); it != customHeadParts.end(); ++it) {
                if (it->slot == a_slot) {
                    if (a_key.Empty()) {
                        customHeadParts.erase(it);
                    } else {
                        it->key = std::move(a_key);
                    }
                    return;
                }
            }
            if (!a_key.Empty()) {
                customHeadParts.push_back(CustomHeadPartRef{ a_slot, std::move(a_key) });
            }
        }

        [[nodiscard]] bool Empty() const {
            return hair.Empty() && eyes.Empty() && brows.Empty() && facialHair.Empty() &&
                   !hairTint.set && customHeadParts.empty();
        }
    };

    // Which of the three a call is about. Mirrors HeadPart::Kind, plus hair,
    // which is a head part in the engine but a separate dimension in this
    // editor.
    enum class Part : std::uint8_t { kHair, kEyes, kBrows, kFacialHair };

    [[nodiscard]] const char* PartName(Part a_part);

    // The part a head-part kind maps onto. A switch rather than a ternary
    // chain at each call site: this mapping decides which default a "Set as
    // default" click writes and which one a push falls back to, so a wrong
    // arm gives a character somebody else's face. Default-less, so /we4062
    // makes a new kind a build error here rather than a silent brow.
    [[nodiscard]] inline Part PartFor(HeadPart::Kind a_kind) {
        switch (a_kind) {
            case HeadPart::Kind::kHair:       return Part::kHair;
            case HeadPart::Kind::kEyes:       return Part::kEyes;
            case HeadPart::Kind::kBrows:      return Part::kBrows;
            case HeadPart::Kind::kFacialHair: return Part::kFacialHair;
        }
        return Part::kHair;  // unreachable: the switch is total, MSVC cannot see it
    }

    // ---- persistence, owned by the co-save ---------------------------------
    //
    // ⚠ NOTHING IS WRITTEN TO DISK ANY MORE. Set() mutates the map and the next
    // game save carries it; there is no file to keep in step.

    // Everything this save holds, as flat rows for 'DFLK'. Ordered by key so a
    // save file does not churn on an unordered_map's iteration order.
    [[nodiscard]] std::vector<DefaultLookRecord> Snapshot();

    // Replace the whole map from a decoded record. Called on save load ONLY.
    void Restore(const std::vector<DefaultLookRecord>& a_rows);

    // ⚠ CALLED BEFORE READING A SAVE'S RECORDS AND ON REVERT, and it is not
    // belt and braces. Without it, loading save B after save A leaves A's
    // defaults in the map for any character B has no record for, which is the
    // cross-save leak in a different costume.
    void ClearAll();

    // The one-time import from the retired default-looks.json, for a save that
    // predates 'DFLK'. Returns how many entries it took.
    //
    // ⚠ RUN ONLY WHEN THE SAVE CARRIES NO RECORD AT ALL, never on a decode
    // FAILURE. A corrupt record that fell back to the file would put the
    // cross-save leak straight back, and silently.
    std::size_t ImportLegacyFile();

    // The whole default for this actor, or an empty Entry when they have none.
    [[nodiscard]] Entry For(RE::Actor* a_actor);

    // One part of it. Empty when that part has no default, which is what every
    // caller falls back from.
    [[nodiscard]] StyleRefKey For(RE::Actor* a_actor, Part a_part);

    // Set one part. An empty key clears that part, so the caller does not need
    // a second entry point for "no longer a default".
    void Set(RE::Actor* a_actor, Part a_part, const StyleRefKey& a_key);

    void Clear(RE::Actor* a_actor, Part a_part);

    [[nodiscard]] bool Has(RE::Actor* a_actor, Part a_part);

    // ---- the default hair COLOUR ------------------------------------------
    //
    // ⚠ ITS OWN THREE ENTRY POINTS RATHER THAN A FIFTH Part, and the reason is
    // the type. Part indexes a StyleRefKey through Field(), so a colour arm
    // would have nothing to return; every caller of Set() would have to carry a
    // reference it cannot fill, and Field() would gain an arm that returns
    // nullptr for a part that really does have a default. Two shapes, two
    // surfaces, the same storage and the same lifetime.
    //
    // A cleared tint clears the default, so there is no separate "no longer a
    // default" call here either.

    // What this character is always coloured, or a cleared tint when they have
    // no default. `set` false is the leave-their-own-colour-alone answer that
    // every caller falls back from.
    [[nodiscard]] HairTint HairColour(RE::Actor* a_actor);

    void SetHairColour(RE::Actor* a_actor, const HairTint& a_tint);

    [[nodiscard]] bool HasHairColour(RE::Actor* a_actor);

    // ---- the slots this load order invented --------------------------------
    //
    // ⚠ THEIR OWN ENTRY POINTS RATHER THAN A FIFTH Part, for the reason the
    // hair colour gives above: Part indexes a member through Field(), and a
    // slot is not a member. It is a lookup by number into a list, so it needs a
    // number rather than an enumerator, and inventing an enumerator per
    // discovered slot is exactly the widening this design avoids.
    //
    // ⚠ AN EMPTY KEY CLEARS, the same rule Set() follows, so there is no
    // separate "no longer pinned" call here either.

    // What this character normally wears in a slot, or an empty key when they
    // have nothing pinned there. Every caller falls back from empty.
    [[nodiscard]] StyleRefKey ForSlot(RE::Actor* a_actor, std::uint32_t a_slot);

    void SetSlot(RE::Actor* a_actor, std::uint32_t a_slot, const StyleRefKey& a_key);

    void ClearSlot(RE::Actor* a_actor, std::uint32_t a_slot);

    [[nodiscard]] bool HasSlot(RE::Actor* a_actor, std::uint32_t a_slot);

}  // namespace OS::DefaultLook
