#pragma once

#include "DyeUnlocks.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// The colours earned on ANY character, in one file beside the settings, so a
// new character does not re-earn the whole palette (OS-198, user 2026-08-09).
//
// Spec: docs/superpowers/specs/2026-08-09-account-wide-dye-unlocks-design.md.
//
// ⚠⚠ THIS FILE HOLDS UNLOCK FLAGS AND NOTHING ELSE, AND THE FORMAT IS UNABLE
// TO EXPRESS ANYTHING ELSE ON PURPOSE. The Seamstone CHARGE is currency, bought
// with soul gems out of one save's inventory, and the DEEDS are counters of
// acts the player was CHARGED for (`kDeedChannelsDyed` is billed per look).
// Either of those going account-wide is OS-172 rebuilt: pay, reload, keep the
// thing, get the money back. The flags are safe to share because they are
// EARNED and never bought: DyeCondKind carries kAlways, kNever, kLevel,
// kSkill, kQuest, kDeed and kLocationCleared, and no code path anywhere turns
// gold or charge into an unlock.
//
// Adding a "charge" key here would therefore be a schema change, not a
// convenience, and whoever writes it has to read this paragraph on the way
// past. That is the whole reason the schema is this narrow.
//
// Pure half in this header so the tests reach it with no engine, no save and
// no filesystem, the DyeSchemes and MyDyes precedent.
namespace OS::SharedDyeUnlocks {

    // Beside the other FittingRoom data, through DataPath, so the isolated
    // Body Studio dev channel gets its own file for free. It must not pollute
    // release's shared unlocks any more than it may touch release's co-save.
    inline constexpr std::string_view kFileName = "dye-unlocks-shared.json";

    // The array of ids. Named because both halves spell it and a typo is
    // silent: a decode looking for the wrong key reads every file as empty,
    // and the save side would then write that emptiness back.
    inline constexpr std::string_view kRootKey = "unlocked";

    // What one merge did. Two numbers, because they have different meanings
    // and only one of them is good news.
    struct MergeReport {
        // Ids the character did not hold and now does.
        std::size_t gained{ 0 };
        // Ids Add turned down: empty, over kMaxStrLen, or past kMaxUnlocks.
        //
        // ⚠ COUNTED AND REPORTED RATHER THAN IGNORED. At the cap a merge
        // silently dropping shared colours looks exactly like a merge that
        // had nothing to add, and the player's complaint ("my colours did not
        // come across") would arrive with nothing in the log to separate the
        // two.
        std::size_t refused{ 0 };
    };

    // The file's ids as JSON text.
    [[nodiscard]] std::string EncodeSharedUnlocks(const std::set<std::string>& a_ids);

    // ⚠ ALL OR NOTHING, the contract DyeUnlockSet::Decode has. On false
    // a_out is untouched. A malformed element fails the WHOLE read rather than
    // costing itself, which is the opposite of the dye packs' per-entry
    // tolerance and deliberately so: a pack is hand-authored and this file is
    // machine-written, so a bad element here means corruption rather than a
    // typo, and merging half a corrupt file is how a partial set gets written
    // back over a good one.
    //
    // An ABSENT array is not malformed. An empty file is a legal empty set.
    [[nodiscard]] bool DecodeSharedUnlocks(std::string_view        a_text,
                                           std::set<std::string>& a_out);

    // Add every id the character does not already hold.
    //
    // ⚠⚠ WRITES THROUGH DyeUnlockSet::Add AND NOTHING ELSE. Add is the only
    // thing that enforces the empty, kMaxStrLen and kMaxUnlocks invariants,
    // and inserting into the set directly would be a second constructor that
    // skips them, which is precisely what Decode's own ReadString comment
    // forbids one module over. The caps are part of the WIRE FORMAT: a set
    // over them encodes a record the co-save cannot read back.
    [[nodiscard]] MergeReport MergeInto(const std::set<std::string>& a_ids,
                                        DyeUnlockSet&               a_set);

    // ---- the filesystem half ------------------------------------------

    // ⚠⚠ NULLOPT MEANS "A FILE IS THERE AND I COULD NOT READ IT", WHICH IS NOT
    // AN EMPTY SET, and the difference is the whole safety of the feature.
    // Save() writes the file WHOLE, so handing back an empty set for an
    // unreadable file would delete every colour every character ever earned,
    // on exactly the load whose data just failed to parse. Same split, and the
    // same reason, as MyDyes::LoadPack.
    //
    // A MISSING file is not a failure: it is an empty set, and it is the
    // ordinary state of a player who has just turned the setting on.
    [[nodiscard]] std::optional<std::set<std::string>> Load();

    // Write the file whole. False on an I/O failure.
    //
    // ⚠ REFUSES while this session has seen an unreadable file, so a failed
    // Load cannot be followed by a Save that overwrites what it could not
    // read. Nothing clears that latch except a successful Load.
    [[nodiscard]] bool Save(const std::set<std::string>& a_ids);

    // Whether this session has seen an unreadable file. Exposed so a caller
    // can say so in its own log line rather than inferring it from a false
    // return that also means "the disk is full".
    [[nodiscard]] bool Unreadable();

    // Delete the file and forget the unreadable latch. True if nothing is on
    // disk afterwards, which includes the file never having been there.
    //
    // ⚠⚠ THE ONLY WAY OUT OF AN ADD-ONLY FILE, and that is why it exists (user
    // 2026-08-11). Nothing else here ever removes an id: Save writes the union,
    // MergeInto only adds, and the tooltip promises as much. Without this the
    // answer to "I want to start the account over" was "find the JSON yourself".
    //
    // ⚠ IT DOES NOT TOUCH ANY SAVE. Every character keeps the colours already
    // in their own co-save, because those were earned and are theirs; what goes
    // is the pool a NEW character would have been handed. A caller that says
    // "reset your progress" without saying that has mis-sold it.
    //
    // ⚠ CLEARING THE LATCH IS PART OF THE JOB, not a convenience. A session
    // that failed to read the file stands every later Save down, so a delete
    // that left the latch set would leave sharing dead until the game was
    // restarted, on exactly the path taken to fix a bad file.
    [[nodiscard]] bool Forget();

    // Testing seam: forget the unreadable latch. NOT for production use, and
    // there is no production caller.
    void ResetForTests();

}  // namespace OS::SharedDyeUnlocks
