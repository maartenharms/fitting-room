#include "SharedCollection.h"

#include "BuildChannel.h"

#include <json/json.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>

namespace OS::SharedCollection {

    namespace {
        [[nodiscard]] std::filesystem::path FilePath() {
            return BuildChannel::DataPath(kFileName);
        }

        // ⚠ A BOUND ON WHAT WE WILL READ INTO MEMORY, derived rather than
        // picked, SharedDyeUnlocks' rule. Collection's own decode caps at
        // 100'000 entries and a key is at most a plugin name plus seven bytes,
        // so a legitimate file cannot pass about 5 MB. Eight is comfortably
        // clear of that and still cannot make us allocate without limit.
        constexpr std::uintmax_t kMaxFileBytes = 8ull * 1024ull * 1024ull;

        // Main thread only: the sync at load, at save and on the setting's
        // rising edge. A plain bool for SharedDyeUnlocks' reason.
        bool g_unreadable = false;
    }  // namespace

    std::string KeyText(const StyleRefKey& a_key) {
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "%06X", a_key.localFormID);
        return a_key.modName + "|" + buf;
    }

    bool ParseKeyText(std::string_view a_text, StyleRefKey& a_out) {
        const auto bar = a_text.rfind('|');
        // rfind, not find: a plugin name may legally contain a bar on some
        // filesystems and the id never can, so the LAST one is the separator.
        if (bar == std::string_view::npos || bar == 0 || bar + 1 >= a_text.size()) {
            return false;
        }
        const auto hex = a_text.substr(bar + 1);
        std::uint32_t id = 0;
        const auto* first = hex.data();
        const auto* last  = hex.data() + hex.size();
        const auto  res   = std::from_chars(first, last, id, 16);
        // ⚠ THE WHOLE TAIL HAS TO BE THE NUMBER. from_chars stops at the first
        // byte it cannot use and reports success for the prefix, so "0008F0xy"
        // would parse as 0x0008F0 and resolve to a real form that is not the
        // one the file named.
        if (res.ec != std::errc{} || res.ptr != last) {
            return false;
        }
        a_out.modName     = std::string(a_text.substr(0, bar));
        a_out.localFormID = id;
        return true;
    }

    std::string EncodeSharedLooks(const std::set<std::string>& a_ids) {
        Json::Value root(Json::objectValue);
        // A player who opens this file should be able to tell what it is and
        // what it deliberately does not hold. It is rewritten on every sync and
        // nothing reads it back, so it costs a line and cannot drift.
        root["_readme"] =
            "Looks found by any character, shared across saves. Which pieces a "
            "character has SEEN is not here and never will be: an inherited "
            "wardrobe arrives unseen, so there is still something to discover.";
        Json::Value arr(Json::arrayValue);
        for (const auto& id : a_ids) {
            arr.append(id);
        }
        root[std::string(kRootKey)] = std::move(arr);
        Json::StreamWriterBuilder b;
        b["indentation"] = "  ";
        return Json::writeString(b, root);
    }

    bool DecodeSharedLooks(std::string_view a_text, std::set<std::string>& a_out) {
        Json::Value             root;
        Json::String            errs;
        Json::CharReaderBuilder b;
        const std::unique_ptr<Json::CharReader> reader(b.newCharReader());
        if (!reader->parse(a_text.data(), a_text.data() + a_text.size(), &root, &errs)) {
            return false;
        }
        if (!root.isObject()) {
            return false;
        }
        const auto& arr = root[std::string(kRootKey)];
        if (arr.isNull()) {
            a_out.clear();  // absent array is a legal empty set
            return true;
        }
        if (!arr.isArray()) {
            return false;
        }
        std::set<std::string> ids;
        for (const auto& v : arr) {
            if (!v.isString()) {
                return false;  // all or nothing
            }
            auto        text = v.asString();
            StyleRefKey probe;
            if (!ParseKeyText(text, probe)) {
                return false;  // machine-written: a bad element is corruption
            }
            ids.insert(std::move(text));
        }
        a_out = std::move(ids);
        return true;
    }

    std::optional<std::set<std::string>> Load() {
        const auto      path = FilePath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return std::set<std::string>{};  // absent is an empty set, not a failure
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxFileBytes) {
            g_unreadable = true;
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            g_unreadable = true;
            return std::nullopt;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::set<std::string> ids;
        if (!DecodeSharedLooks(text, ids)) {
            g_unreadable = true;
            return std::nullopt;
        }
        g_unreadable = false;
        return ids;
    }

    bool Save(const std::set<std::string>& a_ids) {
        // ⚠ THE LATCH IS THE POINT, SharedDyeUnlocks' argument unchanged: one
        // unreadable load followed by one autosave would delete every look
        // every character ever found, on exactly the data we know we could not
        // parse.
        if (g_unreadable) {
            return false;
        }
        const auto      path = FilePath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << EncodeSharedLooks(a_ids);
        if (!out) {
            return false;
        }
        out.close();
        return true;
    }

    bool Unreadable() { return g_unreadable; }

    bool Forget() {
        std::error_code ec;
        std::filesystem::remove(FilePath(), ec);
        // remove() answers false for "it was not there", which is a success
        // here, so the verdict comes off the disk rather than the return value.
        const bool gone = !std::filesystem::exists(FilePath(), ec);
        if (gone) {
            g_unreadable = false;
        }
        return gone;
    }

}  // namespace OS::SharedCollection
