#pragma once

#include "JsonCodec.h"  // Outfit.h (DyeChannel) + the one strict RRGGBB rule

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // One curated colour: an identity, a display name, the colour itself, and
    // what it costs to unlock.
    //
    // ⚠ The ID is the identity, not the name and not the position. Unlocks are
    // stored by id, so renaming a dye must not orphan every unlock of it, and
    // file order is not stable once other mods ship packs. Ids are opaque text
    // compared exactly; the convention is "pack:name".
    struct Dye {
        std::string   id;
        std::string   name;
        DyeChannel    colour;
        std::uint32_t cost{ 0 };
        // ESO's own rarity bucket: Common, Uncommon, Rare, Material, Dye Stamp.
        // METADATA, not identity. It selects a tier in the unlock rules and
        // nothing else, and an empty one resolves as free.
        std::string   rarity;
        // Loaded from custom.json, the My Dyes pack, so the grid knows which
        // swatches are the player's own. Set by Load per file, never by the
        // parser: origin is a fact about the file, not the entry.
        bool          custom{ false };
    };

    // The curated palette, read from Data/SKSE/Plugins/FittingRoom/Dyes/*.json.
    // Fitting Room ships vanilla.json; any armour mod can drop its own file
    // beside it, which is how presets already work.
    //
    // Pure: no engine types, so this compiles into the test executable.
    namespace DyePalette {

        // Parse one entry. Returns false and leaves a_out alone when the entry
        // cannot be trusted: no id, no name, or a colour that is not exactly six
        // hex digits. a_defaultCost is used when the entry omits "cost".
        bool DyeFromJson(const Json::Value& a_json, std::uint32_t a_defaultCost,
                         Dye& a_out);

        // What one file's "dyes" array cost, in the two ways it can cost
        // something.
        //
        // ⚠ TWO COUNTS, NOT ONE, because they point in OPPOSITE directions. A
        // rejected entry is a colour the player does not get. A dropped rarity
        // is a colour the player gets FOR FREE, because an empty rarity
        // resolves to no tier and no tier means no requirement. Adding them
        // together would report a lost colour and an unearned one as the same
        // event.
        struct EntryTally {
            // No id, no name, or a colour that failed the strict hex check.
            std::size_t rejected{ 0 };
            // Entries that WROTE a "rarity" and got the type wrong, on a dye
            // that otherwise parsed. Counted only there: a dye that was
            // rejected outright cannot be free, so counting its rarity would
            // warn about a colour that is not loaded.
            std::size_t raritiesDropped{ 0 };
        };

        // Parse a file's "dyes" array and APPEND every entry that parses to
        // a_out; this does not clear a_out first, so Load can fold one
        // file's entries onto what earlier files already contributed. A root
        // that is not a JSON object is dropped whole, the same as a
        // malformed ENTRY inside a good root, one at a time; none of the
        // three takes the rest of a hand-edited pack down with it.
        //
        // Returns what the array cost. A root that is not an object, or has
        // no "dyes" array, returns zeroes rather than guessing at a count
        // when there was no array to have counted.
        EntryTally DyesFromJson(const Json::Value& a_root, std::uint32_t a_defaultCost,
                                std::vector<Dye>& a_out);

        // Add a_dye to a_into unless its id is already taken. Returns false
        // on a collision, not for this pure module to log (it has no logger
        // of its own), but so a caller can count it. Load is that caller: it
        // tallies every refusal into LoadReport::idCollisions so plugin.cpp
        // can warn on the total (there is no per-id or per-file detail to
        // hand back beyond that, only how many).
        bool MergeDye(std::vector<Dye>& a_into, const Dye& a_dye);

        // Everything a caller needs to log a proper report of one Load()
        // call, the way PresetStore reports its own scan: how much came in,
        // how much was turned away, and why. Counts only for entries and
        // collisions, there is no per-entry id or filename to hand back
        // without a lot more bookkeeping for what is a hand-authored pack;
        // a plain string per file skipped WHOLE, since that one is worth
        // naming.
        struct LoadReport {
            std::size_t filesScanned{ 0 };
            std::size_t dyesAccepted{ 0 };
            // Rejected by DyeFromJson: no id, no name, or a colour that
            // failed the strict hex check.
            std::size_t entriesRejected{ 0 };
            // Refused by MergeDye: an earlier file already claimed the id.
            std::size_t idCollisions{ 0 };
            // Dyes that loaded but lost their "rarity" to a wrong type. ⚠ The
            // colour is kept, deliberately, and the cost of keeping it is that
            // the dye now has no tier, which the unlock rules read as FREE.
            // Non-fatal and non-silent: the count is here so the caller can
            // say so once at load.
            std::size_t raritiesDropped{ 0 };
            // One entry per file skipped WHOLE, "'filename': reason". Covers
            // a file that could not be opened, is not valid JSON, is over
            // the size cap, or threw while parsing.
            std::vector<std::string> fileSkips;
        };

        // Scan the directory. MAIN THREAD ONLY: kDataLoaded or a queued task.
        // The render thread must never touch the disk, which is the entire
        // reason Snapshot() exists, so this must never be wired straight to
        // a render-thread control. A thread-safe Rescan shim, the way
        // PresetStore has one, belongs with the editor UI in a later task,
        // not here.
        // a_defaultCost comes from [Lore] iDyeCost.
        //
        // A file that cannot be opened, is not valid JSON, is bigger than
        // kMaxDyeFileBytes, or THROWS while parsing (jsoncpp's own nesting
        // guard trips past a few hundred levels deep; see the try/catch in
        // the .cpp) is skipped WHOLE, the same as an ordinary malformed
        // file; the rest of the directory still loads. Returns a report
        // rather than a bare count so a non-pure caller (plugin.cpp) can log
        // something a pack author can act on; this module stays pure and
        // logs nothing itself.
        LoadReport Load(std::uint32_t a_defaultCost);

        // Consistent copy for the render thread (the editor draw loop).
        [[nodiscard]] std::vector<Dye> Snapshot();
        [[nodiscard]] std::size_t      Count();

        // Search a caller-supplied list. Split out from Find so the matching
        // rule is testable without reaching the locked store, the same way
        // MergeDye is.
        [[nodiscard]] std::optional<Dye> FindIn(const std::vector<Dye>& a_dyes,
                                                std::string_view        a_id);

        // The dye with this id, or nullopt. Used to resolve a stored unlock.
        //
        // ⚠ Returns a COPY, not a pointer into g_dyes. Load() rebuilds the
        // store with a std::move that frees the old buffer and every string
        // inside it; a pointer handed out here would be dangling the instant
        // the lock that made it valid releases. One Dye copy per lookup is
        // the price of that being safe by construction instead of by timing
        // (see OutfitSession::ActiveOutfitFor for the same trade).
        [[nodiscard]] std::optional<Dye> Find(std::string_view a_id);

        // The reverse question: which curated dye, if any, is this channel
        // currently showing? Answers "you are wearing Ancestor Silk" and
        // "nothing in the palette made this" apart, which is what the dye pane
        // needs to name a colour under a part.
        //
        // ⚠ THERE IS NO STORED ID TO READ, so this is a colour-and-finish match
        // through Outfit.h's ChannelCarriesPaletteDye. That is what makes it
        // work on channels painted long before this existed, which is every
        // channel in every save that already has dye on it. See the ⚠⚠ on that
        // function for why the match is the merge run forwards rather than a
        // second list of fields.
        struct AppliedMatch {
            // Set ONLY on an unambiguous match. Empty means either nothing in
            // the palette makes this colour, which is an ordinary answer for a
            // colour picked with the picker, or that several do.
            std::optional<Dye> dye;
            // How many palette entries claim this channel. 0 and 2+ both leave
            // `dye` empty and they are DIFFERENT FACTS, which is the whole
            // reason this is not a bare optional: one means the player mixed
            // their own colour, the other means two packs ship the same colour
            // and finish under different names.
            std::size_t        matches{ 0 };
        };

        // Search a caller-supplied list, the way FindIn does and for the same
        // reason: the matching rule stays testable without reaching the locked
        // store or a render thread.
        //
        // ⚠ A TIE IS REFUSED, NOT BROKEN. Picking the first match would key the
        // displayed name on FILE ORDER, which this header already says is not
        // stable once other mods ship packs, so the same colour would be named
        // differently on two installs and neither would be wrong. Returning the
        // count instead lets the caller say nothing and log something.
        [[nodiscard]] AppliedMatch FindAppliedIn(const std::vector<Dye>& a_dyes,
                                                 const DyeChannel&       a_channel);

        // Where a colour sits when the grid is ordered by hue rather than by
        // the pack's own order.
        //
        // ⚠⚠ GREY IS NOT A HUE, AND IGNORING THAT IS WHAT MAKES A HUE SORT LOOK
        // BROKEN. Black, white and every grey have a saturation of zero, so
        // their hue is not merely unstable, it is undefined: the arithmetic
        // still returns a number and that number comes from rounding noise in
        // the last bit of two equal channels. Sorted on it, the neutrals scatter
        // themselves one at a time through the reds, the greens and the blues,
        // which reads as the sort having failed rather than as a rule the player
        // can see. They go in their own run instead, ordered light to dark.
        struct HueKey {
            // Neutrals sort as one run AFTER the colours. Behind rather than in
            // front because a player reaching for "sort by hue" is looking for a
            // colour; the greys are what they are not looking for.
            bool          neutral{ false };
            // 0..255 around the wheel, starting at red. Meaningless and left at
            // 0 when neutral.
            std::uint8_t  hue{ 0 };
            // Within one hue: strong before washed out, so a run of blues walks
            // from the blue you would call blue toward the pale ones.
            std::uint8_t  saturation{ 0 };
            // Last term, and the ONLY ordering the neutral run has. Light first,
            // so white leads and black closes the grid.
            std::uint8_t  value{ 0 };

            friend bool operator==(const HueKey&, const HueKey&) = default;
        };

        // Pure, so the ordering is testable without a palette, a grid or a
        // rendering context. Reads the PRIMARY stop only: a two-stop pearl is
        // filed under the colour it reads as, which is the one the swatch shows
        // biggest, and sorting a ramp by its second stop would put a colour
        // where the player cannot see why.
        [[nodiscard]] HueKey HueKeyOf(const DyeChannel& a_colour);

        // Strict weak ordering over HueKey, for the grid's comparator.
        [[nodiscard]] bool HueKeyLess(const HueKey& a_l, const HueKey& a_r);

    }  // namespace DyePalette

}  // namespace OS
