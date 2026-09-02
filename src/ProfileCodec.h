#pragma once

#include "BodyPreset.h"
#include "MakeupPlan.h"
#include "Outfit.h"
#include "OverlayPlan.h"

#include <json/json.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The character profile file codec (the parity spec's section 5): one JSON
// file per named look under Data/SKSE/Plugins/FittingRoom/Profiles/. Pure
// logic - no SKSE, no engine - so the format is testable the way JsonCodec is.
//
// The shape is NESTED per-subsystem blocks, deliberately unlike the flat
// preset files: a profile embeds the outfit object as ONE block among peers,
// which keeps the flat shape's metadata namespace collision out of the new
// format. Every block struct here reuses the page's own engine-free state
// type (OverlayPlan::LayerState, MakeupPlan::LayerState, BodyPreset, Outfit);
// a codec-private mirror of any of them would be a second reader that drifts.
//
// Decode discipline, inherited from the codecs with scars:
//   * version is an integer and exactly 1; anything else rejects the file.
//   * a block that fails to parse drops with one reason line naming the
//     block; the siblings apply. Entries inside a block skip junk silently,
//     the same tolerance JsonToOutfit has always had.
//   * unknown top-level members are preserved on rewrite, so an older
//     build's resave does not strip a newer build's block. A file whose
//     present known blocks ALL fail is never rewritten at all.
//   * colours are strict RRGGBB through the one shared parser; a malformed
//     overlay tint decodes to "leave that channel alone", and a malformed
//     makeup tint drops its entry whole because a tint-mask layer cannot
//     say "leave it alone" (strength and colour are one write).
//
// ⚠⚠ THE EXCLUSIVITY RULE LIVES HERE AND NOT IN THE UI. A face block whose
// ApplyTypes flags are not 0 makes RaceMenu rewrite a channel FR also owns,
// which is two painters of one appearance by construction. Decode drops the
// corresponding FR block (overrides 1 -> overlays, body morphs 2 -> body +
// shape morphs, transforms 4 -> shape scales, skin 8 -> skin) with a reason
// line, so no caller downstream can apply both. Makeup is deliberately NOT in
// that set: a flags=0 face apply rebuilds the tint list no matter what, and
// the apply pipeline re-reads and edits it afterwards - there is no
// ApplyTypes bit to collide with.
namespace OS::ProfileCodec {

    inline constexpr int kProfileVersion = 1;

    // Whether the profile's jslot is FR's own capture (deleted with the
    // profile) or a file the player exported themselves (never touched).
    // The distinction decides a DELETION, so decode is strict: a junk value
    // drops the face block rather than guessing.
    enum class FaceSource : std::uint8_t { kCaptured, kReferenced };

    // Which CharGen folder the jslot lives in, because skee reads them
    // through DIFFERENT natives: LoadCharacterEx takes Exported and does the
    // race switch itself; LoadCharacterPresetEx takes Presets, has no race
    // parameter, and never touches the race. The apply side branches on
    // this, so decode is strict the way source is: a junk value drops the
    // block rather than dispatching the wrong native. Absent means Exported,
    // which keeps every existing profile file byte-stable.
    enum class FaceFolder : std::uint8_t { kExported, kPresets };

    struct FaceBlock {
        std::string   jslot;  // CharGen name, no path, no extension
        FaceSource    source{ FaceSource::kReferenced };
        std::uint32_t flags{ 0 };  // RaceMenu ApplyTypes; 0 = face only
        FaceFolder    folder{ FaceFolder::kExported };

        // ⚠⚠ THE LOOK'S OWN HAIR COLOUR, BECAUSE THE JSLOT CANNOT BE TRUSTED
        // TO CARRY IT. skee writes its `hairColor` key only when the actor
        // base holds a colour form at save time and reads an absent key back
        // as 0, which is black. The player's base reads (none) for the first
        // ~70 s after a load, so a look captured in that window is a look with
        // black hair for good: `FR_Nord 1.jslot` on this rig has `weight` and
        // nothing else, and applying it painted (0,0,0) over a Nord whose own
        // colour is (57,55,40).
        //
        // Absent here keeps every profile written before this field
        // byte-identical on resave, and means only "this look does not state a
        // colour", which is the pre-existing behaviour.
        std::optional<HairTint> hairColour{};

        friend bool operator==(const FaceBlock&, const FaceBlock&) = default;
    };

    struct OverlayEntry {
        std::uint32_t           index{ 0 };  // layer index within its location
        OverlayPlan::LayerState state;

        friend bool operator==(const OverlayEntry&, const OverlayEntry&) = default;
    };

    struct OverlaysBlock {
        // Page order (kFace first), indexed by OverlayPlan::Slot(location).
        std::array<std::vector<OverlayEntry>, OverlayPlan::kLocationCount> byLocation;

        friend bool operator==(const OverlaysBlock&, const OverlaysBlock&) = default;
    };

    struct MakeupEntry {
        std::uint32_t          index{ 0 };  // position in the engine tint list
        std::uint32_t          type{ 0 };   // the mask's own type field
        MakeupPlan::LayerState state;

        friend bool operator==(const MakeupEntry&, const MakeupEntry&) = default;
    };

    struct BodyBlock {
        // The OBody preset name, and/or the embedded slider payload. The
        // payload is what makes a profile self-contained where the co-save is
        // not: a rig without the BodyPresetStore file still gets the body.
        std::string               obodyPreset;
        std::optional<BodyPreset> custom;

        friend bool operator==(const BodyBlock&, const BodyBlock&) = default;
    };

    struct ShapeBlock {
        std::map<std::string, float> morphs;  // owned-key body morphs
        std::map<std::string, float> scales;  // proportion group id -> factor

        friend bool operator==(const ShapeBlock&, const ShapeBlock&) = default;
    };

    struct SkinBlock {
        // Skin pack folder name. EMPTY IS A VALUE, not an absence: it means
        // the character wears no pack, and applying it takes a pack OFF. The
        // block being missing is what means "say nothing about the skin".
        std::string pack;

        friend bool operator==(const SkinBlock&, const SkinBlock&) = default;
    };

    // Who the look was captured ON: the race and sex it assumes. Recorded on
    // every capture so a library of looks knows what it is holding; the APPLY
    // side switches race and sex to match when the Character box is checked
    // (the pinned order's first step) and warns without moving when it is
    // not.
    struct CharacterBlock {
        StyleRefKey race;  // {modName, localFormID}, resolved at read time
        bool        female{ false };

        friend bool operator==(const CharacterBlock&, const CharacterBlock&) = default;
    };

    struct Profile {
        std::string              name;
        std::string              author;
        std::string              description;
        std::vector<std::string> requires_;  // top-level hard gate, author-opted

        std::optional<FaceBlock>                face;
        std::optional<Outfit>                   outfit;
        std::optional<OverlaysBlock>            overlays;
        std::optional<std::vector<MakeupEntry>> makeup;
        std::optional<BodyBlock>                body;
        std::optional<ShapeBlock>               shape;
        std::optional<SkinBlock>                skin;
        std::optional<float>                    weight;  // 0..100
        std::optional<CharacterBlock>           character;

        // Unknown top-level members, carried verbatim so a resave by this
        // build keeps a newer build's blocks.
        Json::Value extras{ Json::objectValue };
    };

    struct ProfileParse {
        Profile profile;
        // One line per dropped block, each naming the block and the reason.
        // These end up in FittingRoom.log; they are the field's checklist.
        std::vector<std::string> dropped;
        // False when known blocks were present and every one of them failed:
        // such a file is never rewritten, so a broken-but-newer file cannot
        // be flattened into an empty one.
        bool rewritable{ true };
    };

    // Parses a profile file's root object. Returns false only for root-level
    // refusals (not an object, wrong version, missing name, malformed
    // requires), with the one-line reason in a_error. Block failures do not
    // fail the file; they land in a_out.dropped.
    bool ParseProfile(const Json::Value& a_root, ProfileParse& a_out,
                      std::string& a_error);

    // The exact file ParseProfile accepts, plus the preserved extras.
    [[nodiscard]] Json::Value ProfileToJson(const Profile& a_profile);

}  // namespace OS::ProfileCodec
