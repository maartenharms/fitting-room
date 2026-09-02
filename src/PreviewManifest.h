#pragma once

// The disk cache's manifest, as a pure codec: the file's shape, its staleness
// rules and its entry states, apart from any IO.
//
// ⚠ AN UNREADABLE MANIFEST IS REFUSED, NEVER READ AS EMPTY. The disk cache
// rewrites the file whole on save, so an empty read of a present file would be
// a silent wipe of every entry, including the persisted failures that keep
// broken NIFs from being re-parsed every session. MyDyes::CustomsFromPackText
// walked into exactly this and its rule is copied here.

#include <json/json.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace OS::PreviewManifest {

    inline constexpr std::uint32_t kFormatVersion = 1;

    // Four storage states; the UI sees three, because kStale reports to
    // callers as missing so the card re-renders instead of reading as failed.
    enum class Status : std::uint8_t { kReady, kFailed, kStale, kMissing };

    struct Entry {
        std::string   key;      // the FULL disk key, re-compared on every read:
                                // hash collisions are detected, not ignored
        Status        status{ Status::kMissing };
        std::string   file;     // shard-relative, "AB/AB12....png"
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        std::string   failureReason;
        std::uint64_t updatedTick{ 0 };  // the eviction clock
        // The image file's size, recorded when it was written.
        //
        // ⚠ HERE RATHER THAN AS ONE TOTAL IN THE HEADER, and the difference is
        // what happens when an entry goes away. A single persisted total has
        // nothing to subtract when an entry is evicted or overwritten, so it
        // drifts and never recovers; a per-entry size subtracts exactly what it
        // added. The load used to sum these by calling file_size() once per
        // ready entry, which on a 14,991-entry cache was 2012 ms of USVFS
        // round-trips on the thread that opens the editor (field 2026-08-12).
        //
        // ⚠ ZERO MEANS "NOT RECORDED YET", NOT "AN EMPTY FILE". A manifest
        // written before this field existed has no sizes at all, so the load
        // stats those entries once and writes the answer back. After one save
        // the whole file carries them and nothing stats again. This is why the
        // format version is NOT bumped: a bump refuses the old manifest, and
        // refusing it wipes every thumbnail to save a migration that costs one
        // session's worth of stats.
        std::uint64_t bytes{ 0 };
    };

    struct Manifest {
        std::uint32_t                 thumbnailSize{ 0 };
        std::string                   rendererVersion;
        std::map<std::string, Entry>  entries;  // keyed by the 16-hex hash
    };

    [[nodiscard]] inline const char* StatusName(Status a_s) {
        switch (a_s) {
            case Status::kReady:  return "ready";
            case Status::kFailed: return "failed";
            case Status::kStale:  return "stale";
            case Status::kMissing: break;
        }
        return "missing";
    }

    [[nodiscard]] inline Status StatusFrom(std::string_view a_s) {
        if (a_s == "ready")  { return Status::kReady; }
        if (a_s == "failed") { return Status::kFailed; }
        if (a_s == "stale")  { return Status::kStale; }
        return Status::kMissing;
    }

    [[nodiscard]] inline std::string ToText(const Manifest& a_m) {
        Json::Value root(Json::objectValue);
        root["version"]         = kFormatVersion;
        root["thumbnailSize"]   = a_m.thumbnailSize;
        root["rendererVersion"] = a_m.rendererVersion;
        Json::Value entries(Json::objectValue);
        for (const auto& [hash, e] : a_m.entries) {
            Json::Value o(Json::objectValue);
            o["key"]           = e.key;
            o["status"]        = StatusName(e.status);
            o["file"]          = e.file;
            o["width"]         = e.width;
            o["height"]        = e.height;
            o["failureReason"] = e.failureReason;
            o["updatedTick"]   = static_cast<Json::UInt64>(e.updatedTick);
            o["bytes"]         = static_cast<Json::UInt64>(e.bytes);
            entries[hash] = std::move(o);
        }
        root["entries"] = std::move(entries);
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";
        return Json::writeString(wb, root);
    }

    // Nullopt on: unparseable text, a non-object root, a version mismatch, a
    // thumbnail-size mismatch or a renderer mismatch. The last three are the
    // spec's staleness mechanisms and they make a total cache reset free.
    [[nodiscard]] inline std::optional<Manifest> FromText(
        std::string_view a_text, std::uint32_t a_thumbnailSize,
        std::string_view a_rendererVersion) {
        Json::Value             root;
        Json::CharReaderBuilder rb;
        const std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
        const char*                             begin = a_text.data();
        std::string                             errs;
        if (!reader || !begin ||
            !reader->parse(begin, begin + a_text.size(), &root, &errs)) {
            return std::nullopt;
        }
        // ⚠ isObject() before operator[]: jsoncpp THROWS on an array root.
        if (!root.isObject()) {
            return std::nullopt;
        }
        if (!root["version"].isUInt() || root["version"].asUInt() != kFormatVersion) {
            return std::nullopt;
        }
        if (!root["thumbnailSize"].isUInt() ||
            root["thumbnailSize"].asUInt() != a_thumbnailSize) {
            return std::nullopt;
        }
        if (!root["rendererVersion"].isString() ||
            root["rendererVersion"].asString() != a_rendererVersion) {
            return std::nullopt;
        }
        Manifest m;
        m.thumbnailSize   = a_thumbnailSize;
        m.rendererVersion = std::string{ a_rendererVersion };
        const auto& entries = root["entries"];
        if (entries.isObject()) {
            for (const auto& hash : entries.getMemberNames()) {
                const auto& o = entries[hash];
                // A malformed ENTRY costs that entry and the file survives:
                // DyesFromJson's split between an entry and a file.
                if (!o.isObject() || !o["key"].isString() || !o["status"].isString()) {
                    continue;
                }
                Entry e;
                e.key    = o["key"].asString();
                e.status = StatusFrom(o["status"].asString());
                e.file   = o["file"].isString() ? o["file"].asString() : std::string{};
                e.width  = o["width"].isUInt() ? o["width"].asUInt() : 0u;
                e.height = o["height"].isUInt() ? o["height"].asUInt() : 0u;
                e.failureReason =
                    o["failureReason"].isString() ? o["failureReason"].asString()
                                                  : std::string{};
                e.updatedTick =
                    o["updatedTick"].isUInt64() ? o["updatedTick"].asUInt64() : 0u;
                // Absent on every manifest written before the field existed,
                // which is the migration path rather than an error: zero means
                // "ask the filesystem once, then record it".
                e.bytes = o["bytes"].isUInt64() ? o["bytes"].asUInt64() : 0u;
                m.entries[hash] = std::move(e);
            }
        }
        return m;
    }

}  // namespace OS::PreviewManifest
