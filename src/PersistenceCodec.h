#pragma once

#include "Outfit.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace OS {

    inline constexpr std::uint32_t kCodecVersion = 4;
    // v1 -> v2: appended a per-outfit weapon block (weapon + quiver
    // transmog) after the existing armor slot block. CRITICAL: the co-save
    // 'LIBR' record is every save's own outfit library, so Decode MUST keep
    // accepting v1 bytes forever - if it ever stopped, every save written
    // before this version would silently lose its whole library on load.
    // See the v1-branch below and the v1-decode test in test_persistence.cpp.
    //
    // v2 -> v3: appended a per-outfit BODY block (OBody preset name + ORefit
    // mode). Same rule, same shape: every older version stays readable
    // forever, each block stops where its version stopped, and a missing
    // block leaves its fields at the "changes nothing" default. There is
    // deliberately NO migration pass - nothing is transformed, an old record
    // simply does not fill in the newer fields.
    //
    // v3 -> v4: appended optional Right/Left weapon overrides after the body
    // block. The original v2 class entry is still the Both fallback, so every
    // existing co-save remains semantically identical.

    // Guard against absurd allocations from corrupt data.
    inline constexpr std::uint32_t kMaxStringLen = 512;
    inline constexpr std::uint32_t kMaxOutfitCount = static_cast<std::uint32_t>(kMaxOutfits);

    namespace detail {
        inline void PutU32(std::vector<std::byte>& a_out, std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                a_out.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        }
        inline void PutU8(std::vector<std::byte>& a_out, std::uint8_t a_v) {
            a_out.push_back(static_cast<std::byte>(a_v));
        }
        inline void PutStr(std::vector<std::byte>& a_out, const std::string& a_s) {
            PutU32(a_out, static_cast<std::uint32_t>(a_s.size()));
            for (char c : a_s) {
                a_out.push_back(static_cast<std::byte>(c));
            }
        }

        struct Reader {
            std::span<const std::byte> data;
            std::size_t                pos{ 0 };
            bool                       ok{ true };

            bool Need(std::size_t n) {
                if (!ok || pos + n > data.size()) {
                    ok = false;
                    return false;
                }
                return true;
            }
            std::uint32_t U32() {
                if (!Need(4)) {
                    return 0;
                }
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[pos + i])) << (i * 8);
                }
                pos += 4;
                return v;
            }
            std::uint8_t U8() {
                if (!Need(1)) {
                    return 0;
                }
                return std::to_integer<std::uint8_t>(data[pos++]);
            }
            std::string Str() {
                const auto len = U32();
                if (!ok || len > kMaxStringLen || !Need(len)) {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<const char*>(data.data() + pos), len);
                pos += len;
                return s;
            }
        };
    }  // namespace detail

    inline std::vector<std::byte> Encode(const OutfitLibrary& a_lib) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_lib.Count()));
        PutU32(out, static_cast<std::uint32_t>(a_lib.ActiveIndex() + 1));  // 0 == none

        for (const auto& o : a_lib.All()) {
            PutStr(out, o.name);
            PutU8(out, o.favorite ? 1 : 0);

            // Only non-passthrough slots are written.
            std::uint32_t count = 0;
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (o.EntryFor(b).kind != SlotEntry::Kind::kPassthrough) {
                    ++count;
                }
            }
            PutU32(out, count);
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                const auto& e = o.EntryFor(b);
                if (e.kind == SlotEntry::Kind::kPassthrough) {
                    continue;
                }
                PutU32(out, b);
                PutU8(out, static_cast<std::uint8_t>(e.kind));
                PutStr(out, e.style.modName);
                PutU32(out, e.style.localFormID);
            }

            // Weapon dimension (v2), same non-passthrough-only shape as the
            // armor slot block above, indexed by WeaponClass instead of an
            // editor-slot bit.
            std::uint32_t weaponCount = 0;
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                if (o.WeaponEntryFor(static_cast<WeaponClass>(c)).kind != SlotEntry::Kind::kPassthrough) {
                    ++weaponCount;
                }
            }
            PutU32(out, weaponCount);
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto  wc = static_cast<WeaponClass>(c);
                const auto& e  = o.WeaponEntryFor(wc);
                if (e.kind == SlotEntry::Kind::kPassthrough) {
                    continue;
                }
                PutU8(out, static_cast<std::uint8_t>(c));
                PutU8(out, static_cast<std::uint8_t>(e.kind));
                PutStr(out, e.style.modName);
                PutU32(out, e.style.localFormID);
            }

            // Body dimension (v3): OBody preset name + ORefit mode. Written
            // unconditionally (unlike the two blocks above, which are
            // non-passthrough-only) because it is a fixed two-field record,
            // not a variable-length list - an empty name IS the "no change"
            // value and costs 4 bytes.
            PutStr(out, o.obodyPreset);
            PutU8(out, static_cast<std::uint8_t>(o.orefit));

            // Per-hand weapon overrides (v4). Presence is meaningful:
            // kPassthrough means explicitly show the real weapon in this hand,
            // while no record means inherit the legacy Both class value.
            std::uint32_t handCount = 0;
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto wc = static_cast<WeaponClass>(c);
                for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                    if (o.WeaponOverrideFor(wc, hand)) {
                        ++handCount;
                    }
                }
            }
            PutU32(out, handCount);
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto wc = static_cast<WeaponClass>(c);
                for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                    const auto& over = o.WeaponOverrideFor(wc, hand);
                    if (!over) {
                        continue;
                    }
                    PutU8(out, static_cast<std::uint8_t>(c));
                    PutU8(out, static_cast<std::uint8_t>(hand));
                    PutU8(out, static_cast<std::uint8_t>(over->kind));
                    PutStr(out, over->style.modName);
                    PutU32(out, over->style.localFormID);
                }
            }
        }
        return out;
    }

    // Returns false on a version mismatch or malformed data; a_lib is only
    // mutated on success.
    inline bool Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version,
                       OutfitLibrary& a_lib) {
        using namespace detail;
        if (a_version != 1 && a_version != 2 &&
            a_version != 3 && a_version != 4) {
            return false;
        }
        Reader r{ a_bytes };

        const auto count  = r.U32();
        const auto active = r.U32();
        if (!r.ok || count > kMaxOutfitCount || active > count) {
            return false;
        }

        OutfitLibrary tmp;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto name = r.Str();
            const auto fav  = r.U8();
            if (!r.ok) {
                return false;
            }
            const int idx = tmp.Create(name);
            if (idx < 0) {
                return false;
            }
            auto* o    = tmp.At(static_cast<std::size_t>(idx));
            o->favorite = fav != 0;

            const auto slots = r.U32();
            if (!r.ok || slots > kBitCount) {
                return false;
            }
            for (std::uint32_t s = 0; s < slots; ++s) {
                const auto bit  = r.U32();
                const auto kind = r.U8();
                const auto mod  = r.Str();
                const auto fid  = r.U32();
                if (!r.ok || bit >= kBitCount || kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    return false;
                }
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetStyle(bit, StyleRefKey{ mod, fid });
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetHiddenWithRestore(bit, StyleRefKey{ mod, fid });
                }
            }

            if (a_version == 1) {
                continue;  // v1 bytes stop here - no weapon block; leave it all-passthrough
            }

            // Cap at what the wire can express (the class index is a u8, so a
            // same-version writer can never emit more than 256 entries), NOT at
            // this build's kWeaponClassCount - a future writer with appended
            // classes must still decode here, with its unknown entries skipped
            // per-entry below. Absurd counts from corruption die on the
            // r.ok / kMaxStringLen guards (and the record itself is capped at
            // kMaxRecordBytes before Decode ever sees it).
            const auto weaponCount = r.U32();
            if (!r.ok || weaponCount > 256) {
                return false;
            }
            for (std::uint32_t w = 0; w < weaponCount; ++w) {
                const auto classIdx = r.U8();
                const auto kind     = r.U8();
                const auto mod      = r.Str();
                const auto fid      = r.U32();
                if (!r.ok) {
                    return false;
                }
                if (classIdx >= kWeaponClassCount ||
                    kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    continue;  // forward-tolerant: unknown class/kind, consumed and skipped
                }
                const auto wc = static_cast<WeaponClass>(classIdx);
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetWeaponStyle(wc, StyleRefKey{ mod, fid });
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetWeaponHide(wc);
                }
            }

            if (a_version == 2) {
                continue;  // v2 bytes stop here - no body block; leave it at "no change"
            }

            // Body dimension (v3). Fixed shape, so no count to guard - but the
            // mode is still range-checked: an out-of-range byte from a future
            // writer (or corruption) falls back to kDefault rather than being
            // cast into a value the switch statements below do not handle.
            const auto preset = r.Str();
            const auto mode   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->obodyPreset = preset;
            o->orefit      = (mode <= static_cast<std::uint8_t>(ORefitMode::kForceOff))
                                 ? static_cast<ORefitMode>(mode)
                                 : ORefitMode::kDefault;

            if (a_version == 3) {
                continue;
            }

            const auto handCount = r.U32();
            if (!r.ok || handCount > 256) {
                return false;
            }
            for (std::uint32_t h = 0; h < handCount; ++h) {
                const auto classIdx = r.U8();
                const auto handByte = r.U8();
                const auto kind     = r.U8();
                const auto mod      = r.Str();
                const auto fid      = r.U32();
                if (!r.ok) {
                    return false;
                }
                if (classIdx >= kWeaponClassCount ||
                    (handByte != static_cast<std::uint8_t>(WeaponHand::Right) &&
                     handByte != static_cast<std::uint8_t>(WeaponHand::Left)) ||
                    kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    continue;
                }
                const auto wc   = static_cast<WeaponClass>(classIdx);
                const auto hand = static_cast<WeaponHand>(handByte);
                if (!SupportsHandOverrides(wc)) {
                    continue;
                }
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetWeaponStyle(wc, StyleRefKey{ mod, fid }, hand);
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetWeaponHide(wc, hand);
                } else {
                    o->SetWeaponPassthrough(wc, hand);
                }
            }
        }
        if (r.pos != a_bytes.size()) {
            return false;  // trailing garbage
        }

        a_lib = std::move(tmp);
        if (active > 0) {
            a_lib.Activate(active - 1);
        } else {
            a_lib.Deactivate();
        }
        return true;
    }

}  // namespace OS
