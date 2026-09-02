#pragma once

#include "Outfit.h"  // StyleRefKey

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// The looks found by ANY character, in one file beside the settings, so a new
// character does not have to re-find a wardrobe the account already owns (user
// 2026-08-11). The gear half of what SharedDyeUnlocks does for colours.
//
// ⚠⚠ THIS FILE HOLDS LOOKS AND NOTHING ELSE, and the schema is unable to
// express anything else on purpose, which is SharedDyeUnlocks' own paragraph
// one dimension over. A look is EARNED by owning the item once; nothing here
// was bought, so nothing here can be farmed by reloading. The moment someone
// wants to put a cost, a count or a charge in this file they are rebuilding
// OS-172: pay, reload, keep the thing, get the money back.
//
// ⚠ IT DOES NOT SHARE THE SEEN-MARKS, and that is a decision rather than an
// omission. 'KACK' says "this CHARACTER has looked at this piece", and handing
// a new character the marks of an old one would mean their whole inherited
// wardrobe arrives pre-acknowledged, with nothing to discover and no gold on
// anything. Inherited looks arrive UNSEEN, which is what makes the inheritance
// visible at all.
//
// ⚠ THE IDS ARE STABLE KEY TEXT, "Plugin.esp|0008F0", the same shape the
// co-save's own records travel as and the same text DefaultLook writes beside
// its entries. A runtime form id carries the load order's index and would
// rebind one look onto another the moment a plugin moved.
//
// Pure half in this header so a test could reach it with no engine, no save and
// no filesystem, the SharedDyeUnlocks precedent.
namespace OS::SharedCollection {

    // Beside the other FittingRoom data, through DataPath, so the isolated
    // Body Studio dev channel gets its own file for free.
    inline constexpr std::string_view kFileName = "collection-shared.json";

    // The array of ids. Named because both halves spell it and a typo is
    // silent: a decode looking for the wrong key reads every file as empty,
    // and the save side would then write that emptiness back.
    inline constexpr std::string_view kRootKey = "known";

    // ---- the pure half ---------------------------------------------------

    [[nodiscard]] std::string KeyText(const StyleRefKey& a_key);

    // ⚠ FALSE ON ANYTHING THAT IS NOT EXACTLY "mod|HEX". A key that parses
    // loosely is a key that resolves to the wrong form, and this file is
    // machine-written, so a malformed element means corruption rather than a
    // typo somebody made.
    [[nodiscard]] bool ParseKeyText(std::string_view a_text, StyleRefKey& a_out);

    [[nodiscard]] std::string EncodeSharedLooks(const std::set<std::string>& a_ids);

    // ⚠ ALL OR NOTHING, DecodeSharedUnlocks' contract. On false a_out is
    // untouched: merging half a corrupt file is how a partial set gets written
    // back over a good one. An ABSENT array is not malformed; an empty file is
    // a legal empty set.
    [[nodiscard]] bool DecodeSharedLooks(std::string_view        a_text,
                                         std::set<std::string>& a_out);

    // ---- the filesystem half ---------------------------------------------

    // ⚠⚠ NULLOPT MEANS "A FILE IS THERE AND I COULD NOT READ IT", WHICH IS NOT
    // AN EMPTY SET. Save writes the file WHOLE, so handing back an empty set
    // for an unreadable file would delete every look every character ever
    // found, on exactly the load whose data just failed to parse.
    //
    // A MISSING file is not a failure: it is an empty set, and it is the
    // ordinary state of a player who has just turned the setting on.
    [[nodiscard]] std::optional<std::set<std::string>> Load();

    // Write the file whole. False on an I/O failure.
    //
    // ⚠ REFUSES while this session has seen an unreadable file, so a failed
    // Load cannot be followed by a Save that overwrites what it could not read.
    [[nodiscard]] bool Save(const std::set<std::string>& a_ids);

    [[nodiscard]] bool Unreadable();

    // Delete the file and forget the unreadable latch. True if nothing is on
    // disk afterwards, which includes its never having been there.
    // SharedDyeUnlocks::Forget carries the full argument; it applies unchanged.
    [[nodiscard]] bool Forget();

}  // namespace OS::SharedCollection
