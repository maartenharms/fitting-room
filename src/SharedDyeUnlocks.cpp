#include "SharedDyeUnlocks.h"

#include "BuildChannel.h"

#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>

namespace OS::SharedDyeUnlocks {

    namespace {
        [[nodiscard]] std::filesystem::path FilePath() {
            return BuildChannel::DataPath(kFileName);
        }

        // ⚠ A BOUND ON WHAT WE WILL READ INTO MEMORY, derived rather than
        // picked. DyeUnlockSet's own caps make the honest maximum 2048 ids of
        // at most 256 bytes, which is about 550 KB of JSON with the quoting
        // and the indent. Two megabytes is four times that, so a legitimate
        // file can never trip it and a corrupt length cannot make us allocate
        // without limit. Refusing is safe here in a way it would not be if the
        // refusal led to a rewrite: it does not, because a refused read latches
        // the file unreadable and Save then declines.
        constexpr std::uintmax_t kMaxFileBytes = 2ull * 1024ull * 1024ull;

        // Main thread only: RunDyePromotion at kPostLoadGame/kNewGame and the
        // co-save SaveCallback. A plain bool rather than an atomic for that
        // reason, following the OS-173 rate-limit state.
        bool g_unreadable = false;
    }  // namespace

    std::string EncodeSharedUnlocks(const std::set<std::string>& a_ids) {
        Json::Value root(Json::objectValue);
        // A player who opens this file should be able to tell what it is and
        // what it deliberately does not hold. It is rewritten every save and
        // nothing reads it back, so it costs a line and cannot drift.
        root["_readme"] =
            "Dye colours earned on any character, shared across saves. The "
            "Seamstone charge and the deed counters are NOT here and never "
            "will be: they belong to one save, with the gold that bought them.";
        Json::Value arr(Json::arrayValue);
        for (const auto& id : a_ids) {
            arr.append(id);
        }
        root[std::string{ kRootKey }] = std::move(arr);

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";  // a player may want to read or prune it
        return Json::writeString(wb, root);
    }

    bool DecodeSharedUnlocks(std::string_view a_text, std::set<std::string>& a_out) {
        Json::Value                             root;
        Json::CharReaderBuilder                 rb;
        const std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
        const char*                             begin = a_text.data();
        std::string                             errs;
        if (!reader || !begin ||
            !reader->parse(begin, begin + a_text.size(), &root, &errs)) {
            return false;
        }
        // ⚠ isObject() BEFORE operator[]. jsoncpp throws Json::LogicError for
        // an array, a string or a number on the left, the same trap
        // CustomsFromPackText documents. && short-circuits, so the reach for
        // the key never happens on a root that would throw.
        if (!root.isObject()) {
            return false;
        }
        const std::string key{ kRootKey };
        // An ABSENT array is a readable empty file, not a refusal. That is the
        // ordinary state of a player who has just turned the setting on, and
        // refusing it would latch the file unreadable and then decline every
        // save afterwards with a perfectly fine file sitting there.
        if (!root.isMember(key)) {
            a_out.clear();
            return true;
        }
        const Json::Value& arr = root[key];
        if (!arr.isArray()) {
            return false;
        }

        // ⚠ BUILT ASIDE AND SWAPPED IN AT THE END, which is what makes the
        // all-or-nothing promise in the header true rather than nearly true.
        // Filling a_out as we go and returning false partway leaves the caller
        // holding half a corrupt file under a header that says it is untouched.
        std::set<std::string> parsed;
        for (const auto& entry : arr) {
            if (!entry.isString()) {
                return false;
            }
            auto id = entry.asString();
            if (id.empty()) {
                return false;
            }
            parsed.insert(std::move(id));
        }
        a_out = std::move(parsed);
        return true;
    }

    MergeReport MergeInto(const std::set<std::string>& a_ids, DyeUnlockSet& a_set) {
        MergeReport report;
        for (const auto& id : a_ids) {
            if (a_set.Has(id)) {
                // Already held, so not a gain and not a refusal. Asked before
                // Add rather than inferred from its return, because Add
                // answers "is it held now", which is TRUE for an id that was
                // already there; counting that as a gain would report the same
                // colours as newly shared on every single load.
                continue;
            }
            if (a_set.Add(id)) {
                ++report.gained;
            } else {
                ++report.refused;
            }
        }
        return report;
    }

    std::optional<std::set<std::string>> Load() {
        const auto      path = FilePath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            // No file yet is the ordinary first-run state, and an empty set is
            // the honest answer to it. Not a refusal, so the latch stays down
            // and the next save may create the file.
            g_unreadable = false;
            return std::set<std::string>{};
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxFileBytes) {
            g_unreadable = true;
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            // ⚠ THE FILE IS THERE AND WILL NOT OPEN, so this is the refusal
            // rather than the empty set above. Another process holding it open
            // is exactly the moment overwriting it would hurt most.
            g_unreadable = true;
            return std::nullopt;
        }
        const std::string text{ std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>() };
        if (!in && !in.eof()) {
            g_unreadable = true;
            return std::nullopt;
        }
        std::set<std::string> ids;
        if (!DecodeSharedUnlocks(text, ids)) {
            g_unreadable = true;
            return std::nullopt;
        }
        g_unreadable = false;
        return ids;
    }

    bool Save(const std::set<std::string>& a_ids) {
        // ⚠ THE LATCH IS THE POINT. A session that failed to read this file
        // must not then write it: Save replaces it WHOLE, so one unreadable
        // load followed by one autosave would delete every colour every
        // character ever earned, silently, on exactly the load whose data we
        // already know we could not parse.
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
        out << EncodeSharedUnlocks(a_ids);
        if (!out) {
            return false;
        }
        out.close();
        return true;
    }

    bool Forget() {
        std::error_code ec;
        std::filesystem::remove(FilePath(), ec);
        // ⚠ remove() ANSWERS FALSE FOR "IT WAS NOT THERE", which is a SUCCESS
        // here and not a failure, so the verdict is taken from the disk rather
        // than from the return value. A player pressing this twice, or pressing
        // it having never shared anything, has got what they asked for.
        const bool gone = !std::filesystem::exists(FilePath(), ec);
        if (gone) {
            // The latch describes a file that no longer exists. Leaving it set
            // would stand every later Save down and leave sharing dead until a
            // restart, on exactly the path someone takes to fix a bad file.
            g_unreadable = false;
        }
        return gone;
    }

    bool Unreadable() { return g_unreadable; }

    void ResetForTests() { g_unreadable = false; }

}  // namespace OS::SharedDyeUnlocks
