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
    };

    using NpcAssignmentMap = std::unordered_map<NpcKey, NpcRecord, NpcKeyHash>;

    inline constexpr std::uint32_t kNpcRecordVersion   = 3;
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
        }
        return out;
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
        if (a_version != 1 && a_version != 2 &&
            a_version != kNpcRecordVersion) {
            return false;
        }
        // The outer record did not store an inner-version byte, so its own
        // version is the historical contract: NPCO v1 embedded LIBR v2,
        // NPCO v2 embedded LIBR v3, and NPCO v3 embeds LIBR v4.
        const std::uint32_t innerVersion =
            a_version == 1 ? 2u : a_version == 2 ? 3u : kCodecVersion;
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
                tmp[NpcKey{ modName, localFormID }] = std::move(rec);
            } else if (a_version >= 2) {
                // Even when the inner library is corrupt, consume the v2
                // baseline fields so the next outer entry stays aligned.
                (void)r.U32();
                (void)r.Str();
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
