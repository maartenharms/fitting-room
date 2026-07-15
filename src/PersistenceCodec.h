#pragma once

#include "Outfit.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace OS {

    inline constexpr std::uint32_t kCodecVersion = 1;

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
        }
        return out;
    }

    // Returns false on a version mismatch or malformed data; a_lib is only
    // mutated on success.
    inline bool Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version,
                       OutfitLibrary& a_lib) {
        using namespace detail;
        if (a_version != kCodecVersion) {
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
            tmp.At(static_cast<std::size_t>(idx))->favorite = fav != 0;

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
                auto* o = tmp.At(static_cast<std::size_t>(idx));
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetStyle(bit, StyleRefKey{ mod, fid });
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetHide(bit);
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
