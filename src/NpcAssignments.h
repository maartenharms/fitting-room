#pragma once

#include "Outfit.h"
#include "PersistenceCodec.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Per-NPC outfit assignments ('NPCO' co-save payload, pure logic - no engine
// coupling). Each NPC owns an INLINE OutfitLibrary (not a by-name reference
// into the player's library), keyed by the base NPC's identity
// {modName, localFormID} so it resolves load-order-independently. The wire
// format length-prefixes each entry's inner library bytes; that is what lets
// a single corrupt or unresolvable entry be dropped without desyncing the
// rest of the outer stream (a missing plugin, or a future-format library, is
// inert rather than fatal to every other NPC's assignments).
namespace OS {

    struct NpcKey {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        friend bool operator==(const NpcKey&, const NpcKey&) = default;
    };

    struct NpcKeyHash {
        std::size_t operator()(const NpcKey& a_key) const {
            const std::size_t h1 = std::hash<std::string>{}(a_key.modName);
            const std::size_t h2 = std::hash<std::uint32_t>{}(a_key.localFormID);
            // Standard boost-style combine - avoids the trivial XOR collision
            // where two keys with equal hashes-of-parts but swapped fields
            // (or a zero half) would otherwise hash identically.
            return h1 ^ (h2 + 0x9e3779b9U + (h1 << 6) + (h1 >> 2));
        }
    };

    struct NpcRecord {
        OutfitLibrary library;
        // Captured before Fitting Room first assigns an OBody preset to this
        // follower. Empty is a valid captured value ("no prior assignment"),
        // so the flag cannot be inferred from the string.
        std::string obodyBaseline;
        bool        obodyBaselineCaptured{ false };
        // Captured before Fitting Room first applies a hair colour to this
        // follower, as a plugin plus local form ID pair so the reference
        // survives load-order changes. Mirrors obodyBaseline above, including
        // the reason the flag cannot be inferred from the payload: an empty mod
        // name is a real "nothing to restore" value, distinct from never having
        // captured. (Kept as loose fields rather than HairColor::CapturedColor
        // for the same reason EncodeHairBaseline does it in PersistenceCodec.h -
        // this header is pure logic and must not pull in an engine-facing one.)
        std::string   hairBaselineMod;
        std::uint32_t hairBaselineLocalID{ 0 };
        bool          hairBaselineCaptured{ false };
    };

    using NpcAssignmentMap = std::unordered_map<NpcKey, NpcRecord, NpcKeyHash>;

    // ⚠ 17 IS A HOLE, DELIBERATELY, AND THE HOLE IS A CONFESSION. The LIBR v17
    // bump (the eye colour) forgot to bump this constant at all, so NPCO v16
    // spent two builds naming two different inner layouts: the facial-hair
    // build wrote v16 outers embedding LIBR v16, and the eye-colour build wrote
    // v16 outers embedding LIBR v17. That ambiguity cannot be frozen both ways.
    // v16 freezes to 17 below, protecting the saves of the build that is
    // actually being played; a facial-hair-era v16 record's inner library reads
    // as truncated and is skipped, which is what has silently happened since
    // the moment LIBR moved to 17. Jumping this constant to 18 re-syncs the
    // numbering with LIBR so the next maintainer counts one ladder, not two.
    // ⚠ MOVED TO 22 BECAUSE LIBR DID, NOT BECAUSE THIS RECORD GAINED A FIELD.
    // Nothing in the outer structure changed at v22: the inner library did, and
    // EncodeNpcAssignments embeds whatever the CURRENT Encode writes. So a v21
    // outer left in place would claim to embed LIBR v21 while carrying v22
    // bytes, and every follower's inner library would read as trailing garbage
    // and be dropped. Found by the suite the moment kCodecVersion moved, which
    // is what the four-step note below exists to make survivable.
    inline constexpr std::uint32_t kNpcRecordVersion   = 25;
    inline constexpr std::uint32_t kMaxNpcAssignments  = 512;  // guard vs corrupt count

    // Wire format (the record's own bytes; version is carried alongside by
    // the caller, matching how OS::Decode is used):
    //   PutU32(count)
    //   per entry: PutStr(modName) PutU32(localFormID) PutU32(innerLen)
    //              <innerLen bytes = OS::Encode(library)>
    //   v2 appends: PutU32(obodyBaselineCaptured ? 1 : 0)
    //               PutStr(obodyBaseline)
    //   v3 keeps that outer shape but declares that innerLen contains LIBR v4
    //      (per-hand weapon overrides). v1 maps to LIBR v2; v2 maps to LIBR v3.
    //   v4 keeps it again and declares LIBR v5 (per-outfit hair visibility).
    //   v5 keeps it again and declares LIBR v6 (per-outfit hair colour). The
    //      outer bytes are byte-identical to v4's; the version bump exists
    //      ONLY to name which LIBR version the inner payload is, which is the
    //      whole reason NpcoInnerLibrVersion below must freeze v4 at LIBR v5.
    //   v6 appends, per entry: PutU32(hairBaselineCaptured ? 1 : 0)
    //                          PutStr(hairBaselineMod)
    //                          PutU32(hairBaselineLocalID)
    //      This is the FIRST outer-shape change since v2. It is a separate
    //      version from v5 precisely because v5's outer shape is v4's: a dev
    //      build already wrote v5-without-the-triple, so folding the triple
    //      into v5 would make one version number mean two different byte
    //      layouts and those saves would fail on the missing bytes and have
    //      their follower libraries silently dropped. One version, one shape.
    //   v7 keeps v6's outer shape and declares LIBR v7 (per-outfit hair
    //      style), the same shape-preserving bump as v3, v4 and v5.
    //   v8 keeps it again and declares LIBR v8 (per-outfit dye).
    //   v9 keeps it again and declares LIBR v9 (the dye channel count on the
    //      wire, and eight channels instead of three).
    //   v10 keeps it again and declares LIBR v10 (per-outfit WEAPON dye). Same
    //      shape-preserving bump as v3, v4, v5, v7, v8 and v9: the outer bytes
    //      are byte-identical to v9's and the version exists only to name which
    //      LIBR version the inner payload is.
    //   v11 keeps the outer shape and declares LIBR v11 (stable custom body
    //      preset identity).
    //   v12 keeps it and declares LIBR v12 (per-outfit eye and brow refs).
    //   v13 keeps it and declares LIBR v13 (a strength byte per dye channel).
    //   v14 keeps it and declares LIBR v14 (a mode and a second colour stop per
    //      dye channel). Same shape-preserving bump as v3 through v5 and v7
    //      through v13: the outer bytes are byte-identical to v13's and the
    //      version exists only to name which LIBR version the inner payload is.
    //   v15 keeps it and declares LIBR v15 (flake plus both DyeMaterial finish
    //      blocks per dye channel). v14 held the top spot for one day and never
    //      left the dev machine, but the dev machine's own saves carry it, so
    //      it freezes and stays accepted like every other version.
    //      ⚠ WHICH IS EXACTLY WHY IT CANNOT BE SKIPPED. Leaving this at 13 while
    //      LIBR moved to 14 would make the default arm of NpcoInnerLibrVersion
    //      answer 14 for every v13 record already in a save, and each one would
    //      run off the end of its first dye channel looking for a mode nobody
    //      wrote.
    //   v21 keeps the outer shape and declares LIBR v21 (per-outfit head-part
    //      slots the load order invented: horns, cat ears). Shape-preserving
    //      like v3 through v5 and v7 through v20, so v20 freezes above and the
    //      version exists only to name which LIBR version the inner payload is.
    //   v16 keeps the outer shape and declares LIBR v16 (a per-outfit FACIAL
    //      HAIR reference, OS-196). v15 is the version the 2026-08-09
    //      texture-swap builds wrote and the dev machine's saves carry it, so
    //      it freezes above and is written into both accepted lists.
    //   ⚠ THE LIBR v17 BUMP (the eye colour) FORGOT THIS LADDER ENTIRELY, so
    //      v16 spent two builds meaning two inner layouts. It freezes to 17,
    //      the meaning of the saves being played; see the note on the
    //      constant. There is NO NPCO v17 for that reason.
    //   v18 keeps the outer shape and declares LIBR v18 (the per-outfit
    //      SCLERA colour), re-syncing the two ladders' numbering.
    //   v19 keeps it and declares LIBR v19 (a blend byte per dye channel, plus
    //      the outfit's own eye blend). v18 held the top spot for one day and
    //      the dev machine's 2026-08-13 saves carry it, so it freezes above and
    //      goes into the accepted list below as a literal, on the terms v14 and
    //      v15 each did.
    //   v20 keeps it and declares LIBR v20 (the outfit's SECOND eye colour).
    //      v19 held the top spot for part of one afternoon, which is exactly as
    //      binding as v14's single day: the dev machine's saves carry it, so it
    //      freezes and goes into the accepted list like every other version.
    //   v25 keeps it and declares LIBR v25 (a cut byte per dye channel, where
    //      metal starts on the piece). v24 is what 1.1.8 writes, the version
    //      live on Nexus, so it freezes below and goes into both accepted lists
    //      as a literal, on the terms every version before it did.
    // The innerLen prefix lets decode skip a corrupt/unresolvable inner
    // library without losing sync with the outer stream.
    [[nodiscard]] inline std::vector<std::byte> EncodeNpcAssignments(const NpcAssignmentMap& a_map) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_map.size()));
        for (const auto& [key, rec] : a_map) {
            PutStr(out, key.modName);
            PutU32(out, key.localFormID);
            const auto inner = Encode(rec.library);
            PutU32(out, static_cast<std::uint32_t>(inner.size()));
            out.insert(out.end(), inner.begin(), inner.end());
            PutU32(out, rec.obodyBaselineCaptured ? 1u : 0u);
            PutStr(out, rec.obodyBaseline);
            PutU32(out, rec.hairBaselineCaptured ? 1u : 0u);
            PutStr(out, rec.hairBaselineMod);
            PutU32(out, rec.hairBaselineLocalID);
        }
        return out;
    }

    // Which LIBR version an NPCO record of a_version embeds. The outer record
    // stores no inner-version byte, so its own version IS that contract.
    //
    // ⚠ EVERY HISTORICAL BRANCH IS A FROZEN LITERAL. Only the CURRENT version
    // may alias kCodecVersion. This mapping once read
    // `a_version == 2 ? 3u : kCodecVersion`, which silently re-pointed NPCO v3
    // at whatever LIBR version happened to be current, so bumping LIBR made
    // every already-saved v3 record decode at the wrong version, fail on a
    // missing trailing byte, and get DROPPED by the per-entry tolerance. The
    // follower's whole outfit library vanished and the load still reported
    // success with a smaller count. Caught in review before it shipped
    // (1e49d57).
    //
    // It then happened AGAIN, and this time it was not caught by reading: the
    // LIBR v5 to v6 bump re-pointed NPCO v4 the same way, and only an
    // end-to-end test found it. Both the mapping and the bytes are pinned in
    // tests now. Do not collapse these branches.
    //
    // Pulled out of DecodeNpcAssignments so a test can pin the whole mapping
    // rather than only the path a given save happens to exercise.
    //
    // ⚠ BUMPING kNpcRecordVersion? FOUR THINGS ARE REQUIRED, NOT ONE. Adding
    // the constant is the easy part. The version that just LOST current status
    // is the one that silently breaks, because the default arm below keeps
    // answering for it with whatever LIBR version is current. Every time:
    //
    //   1. Add a frozen literal branch for the OUTGOING version, mapping it to
    //      the LIBR version that was current while it held the top spot. Never
    //      leave it to the default arm.
    //   2. Add an explicit CHECK(NpcoInnerLibrVersion(<outgoing>) ==
    //      <the LIBR version it embeds>) to the mapping test in
    //      test_persistence.cpp. Do not rely on the trailing
    //      CHECK(NpcoInnerLibrVersion(kNpcRecordVersion) == kCodecVersion)
    //      sitting beside it: that one passes for ANY pair of constants, by
    //      construction of the default arm, so it proves nothing about the
    //      freeze. Retargeting the kNpcRecordVersion literal next to it is by
    //      itself enough to keep the whole suite green while the freeze is
    //      missing, which is the trap, not the safety net.
    //   3. Add an end-to-end test that FORGES an outgoing-version payload and
    //      decodes it, alongside the existing ones. The mapping assertions only
    //      check a pure function, and this bug reached a build twice while that
    //      pure function read perfectly fine.
    //   4. Write the outgoing version as a LITERAL into both accepted-version
    //      lists, DecodeNpcAssignments' below and OS::Decode's in
    //      PersistenceCodec.h. Both end with the current-version constant, so
    //      the outgoing version falls out of them the instant that constant
    //      moves and its records are refused rather than decoded. Added at the
    //      v13 bump, having been absent from this list for the first three
    //      occurrences: it never bit, because the same edit that moves the
    //      constant is the one that adds the literal, and every previous bump
    //      happened to do it. Being right by habit is not the same as being
    //      guarded, and this one loses the WHOLE record rather than one entry.
    //
    // Skip any of the four and the next occurrence depends on a maintainer
    // remembering, which is precisely what failed both previous times.
    [[nodiscard]] inline constexpr std::uint32_t NpcoInnerLibrVersion(std::uint32_t a_version) {
        return a_version == 1   ? 2u
               : a_version == 2 ? 3u
               : a_version == 3 ? 4u
               : a_version == 4 ? 5u
               : a_version == 5 ? 6u
               : a_version == 6 ? 6u   // FROZEN: NPCO v6 shipped while LIBR was 6
               : a_version == 7 ? 7u   // FROZEN: NPCO v7 shipped while LIBR was 7
               : a_version == 8 ? 8u   // FROZEN: NPCO v8 shipped while LIBR was 8
               : a_version == 9 ? 9u   // FROZEN: NPCO v9 shipped while LIBR was 9
               : a_version == 10 ? 10u // FROZEN: NPCO v10 shipped while LIBR was 10
               : a_version == 11 ? 11u // FROZEN: NPCO v11 shipped while LIBR was 11
               : a_version == 12 ? 12u // FROZEN: NPCO v12 shipped while LIBR was 12
               : a_version == 13 ? 13u // FROZEN: NPCO v13 shipped while LIBR was 13
               : a_version == 14 ? 14u // FROZEN: NPCO v14 held the top spot for one day
               : a_version == 15 ? 15u // FROZEN: NPCO v15 shipped while LIBR was 15
               // FROZEN TO 17, NOT 16, AND THAT IS A TRIAGE RATHER THAN A
               // MAPPING. The LIBR v17 bump forgot the outer constant, so v16
               // records embed LIBR v16 from one build and LIBR v17 from the
               // next. 17 protects the saves being played now; the older
               // build's inner libraries read as truncated and are skipped,
               // exactly as they already were the day the constant moved.
               : a_version == 16 ? 17u
               : a_version == 18 ? 18u // FROZEN: NPCO v18 shipped while LIBR was 18
               : a_version == 19 ? 19u // FROZEN: NPCO v19 shipped while LIBR was 19
               : a_version == 20 ? 20u // FROZEN: NPCO v20 shipped while LIBR was 20
               : a_version == 21 ? 21u // FROZEN: NPCO v21 shipped while LIBR was 21
               // FROZEN: NPCO v22 is what the 2026-08-26 face and dye stint
               // builds write, and those saves are on the dev rig. Without the
               // freeze the default arm answers 23 for records that stop before
               // the push-up byte, so every follower library in them runs off the
               // end of its last outfit and is dropped.
               : a_version == 22 ? 22u
               // FROZEN: NPCO v23 is what the 2026-08-27 and 2026-08-28 builds
               // write, including the 1.1.1 zips the testers already have.
               // Without the freeze the default arm answers 24 for records
               // that stop before the armour dye block grew its garment key,
               // so every follower library in them runs off the end of its
               // last outfit and is dropped.
               : a_version == 23 ? 23u
               // FROZEN: NPCO v24 is what 1.1.8 writes, the version live on
               // Nexus, so every player's save carries it. Without the freeze
               // the default arm answers 25 for records whose dye channels stop
               // before the cut byte, so every dyed follower library in them
               // runs off the end of a channel and is dropped.
               : a_version == 24 ? 24u
                                : kCodecVersion;
    }

    // Returns false only on a malformed OUTER structure (bad version, bad
    // count, a truncated entry header, or an innerLen that overruns the
    // remaining bytes) - a_out is only mutated on a true return. A single
    // entry whose inner library fails OS::Decode is dropped (not fatal);
    // sibling entries still land and the call still returns true.
    [[nodiscard]] inline bool DecodeNpcAssignments(std::span<const std::byte> a_bytes,
                                                    std::uint32_t a_version,
                                                    NpcAssignmentMap& a_out) {
        using namespace detail;
        // ⚠ A FOURTH THING THE BUMP NOTE ABOVE DOES NOT LIST, AND IT LOSES MORE
        // THAN THE OTHER THREE. This list ends with kNpcRecordVersion rather
        // than a literal, so the outgoing version drops out of it the instant
        // that constant moves, and a record at that version is refused outright
        // instead of decoded. The freeze decides which LIBR version a v12 outer
        // embeds; this decides whether a v12 outer is read at all, and getting
        // the freeze right while missing this drops every follower assignment
        // rather than one entry. Add the outgoing literal here on every bump.
        if (a_version != 1 && a_version != 2 && a_version != 3 && a_version != 4 &&
            a_version != 5 && a_version != 6 && a_version != 7 && a_version != 8 &&
            a_version != 9 && a_version != 10 && a_version != 11 &&
            a_version != 12 && a_version != 13 && a_version != 14 &&
            a_version != 15 && a_version != 16 && a_version != 18 &&
            a_version != 19 && a_version != 20 && a_version != 21 &&
            a_version != 22 && a_version != 23 &&
            // 24 WRITTEN DOWN THE DAY IT STOPPED BEING CURRENT: the version 1.1.8
            // writes, live on Nexus, so every player carries it. The terms are
            // the note above.
            a_version != 24 &&
            a_version != kNpcRecordVersion) {
            return false;
        }
        const std::uint32_t innerVersion = NpcoInnerLibrVersion(a_version);
        Reader r{ a_bytes };

        const auto count = r.U32();
        if (!r.ok || count > kMaxNpcAssignments) {
            return false;
        }

        NpcAssignmentMap tmp;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto modName     = r.Str();
            const auto localFormID = r.U32();
            const auto innerLen    = r.U32();
            if (!r.ok || !r.Need(innerLen)) {
                return false;  // truncated outer entry: reject, out left unmutated
            }
            const auto slice = a_bytes.subspan(r.pos, innerLen);
            r.pos += innerLen;  // skip past the inner library regardless of decode result

            OutfitLibrary lib;
            if (Decode(slice, innerVersion, lib)) {
                NpcRecord rec;
                rec.library = std::move(lib);
                if (a_version >= 2) {
                    rec.obodyBaselineCaptured = r.U32() != 0;
                    rec.obodyBaseline         = r.Str();
                    if (!r.ok) {
                        return false;
                    }
                }
                if (a_version >= 6) {
                    rec.hairBaselineCaptured = r.U32() != 0;
                    rec.hairBaselineMod      = r.Str();
                    rec.hairBaselineLocalID  = r.U32();
                    if (!r.ok) {
                        return false;
                    }
                }
                tmp[NpcKey{ modName, localFormID }] = std::move(rec);
            } else if (a_version >= 2) {
                // Even when the inner library is corrupt, consume the v2
                // baseline fields so the next outer entry stays aligned.
                //
                // ⚠ EVERY trailing per-entry field the success branch above
                // reads must be discarded here too. Miss one and a single
                // dropped follower mis-positions the reader for every follower
                // after it in the stream.
                (void)r.U32();
                (void)r.Str();
                if (a_version >= 6) {
                    (void)r.U32();
                    (void)r.Str();
                    (void)r.U32();
                }
                if (!r.ok) {
                    return false;
                }
            }
            // else: drop this entry only - the outer stream stays aligned
            // because innerLen already told us how far to skip.
        }
        if (r.pos != a_bytes.size()) {
            return false;  // trailing garbage
        }

        a_out = std::move(tmp);
        return true;
    }

}  // namespace OS
