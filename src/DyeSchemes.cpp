#include "DyeSchemes.h"

#include "BuildChannel.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace OS::DyeSchemes {

    namespace {
        const auto kSchemesDir = BuildChannel::DataPath("DyeSchemes");

        std::mutex             g_lock;
        std::vector<DyeScheme> g_schemes;

        // An unset colour still occupies a position, because the mapping onto a
        // garment is BY ORDER: collapsing a gap would shift every colour after
        // it onto the wrong piece. Null is the JSON way to say "this position,
        // no colour", and it survives a hand-edit better than an empty string.
        bool IsNullEntry(const Json::Value& a_v) { return a_v.isNull(); }
    }  // namespace

    Json::Value ToJson(const DyeScheme& a_scheme) {
        Json::Value o(Json::objectValue);
        o["name"]      = a_scheme.name;
        Json::Value cs(Json::arrayValue);
        for (const auto& c : a_scheme.colours) {
            if (c.set) {
                cs.append(JsonCodec::ColourToHex(c));
            } else {
                cs.append(Json::Value());  // a gap, kept so the order holds
            }
        }
        o["colours"] = std::move(cs);
        return o;
    }

    bool FromJson(const Json::Value& a_json, DyeScheme& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        const auto& name = a_json["name"];
        if (!name.isString() || name.asString().empty()) {
            // A scheme with no name cannot be shown, chosen or overwritten, so
            // there is nothing useful to do with its colours.
            return false;
        }
        a_out.name = name.asString();
        a_out.colours.clear();

        const auto& cs = a_json["colours"];
        if (!cs.isArray()) {
            return true;  // named but empty: harmless, shows as a scheme with nothing in it
        }
        for (const auto& c : cs) {
            if (IsNullEntry(c)) {
                a_out.colours.push_back(DyeChannel{});
                continue;
            }
            if (!c.isString()) {
                continue;  // malformed: dropped, never guessed
            }
            const auto parsed = JsonCodec::ColourFromHex(c.asString());
            if (parsed.set) {
                a_out.colours.push_back(parsed);
            }
            // A malformed hex string is dropped entirely rather than kept as a
            // gap: a gap is a deliberate "no colour here" and this is not one,
            // and pretending otherwise would silently clear a piece.
        }
        return true;
    }

    std::string FileNameFor(std::string_view a_name) {
        std::string out;
        out.reserve(a_name.size() + 5);
        for (const char c : a_name) {
            // Anything that could steer the path, plus the characters Windows
            // refuses in a filename. Replaced rather than removed so two names
            // that differ only in punctuation do not collide.
            const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' ||
                             c == '|' || static_cast<unsigned char>(c) < 0x20;
            out.push_back(bad ? '_' : c);
        }
        // A name that was nothing but separators leaves dots and underscores,
        // and ".json" or "..json" is not a file anyone can pick again.
        const auto firstReal = out.find_first_not_of("._ ");
        if (firstReal == std::string::npos) {
            out = "scheme";
        }
        // Trailing dots and spaces are legal in the string and illegal in a
        // Windows filename.
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
            out.pop_back();
        }
        if (out.empty()) {
            out = "scheme";
        }
        return out + ".json";
    }

    void Load() {
        std::vector<DyeScheme> found;
        std::error_code        ec;
        if (std::filesystem::exists(kSchemesDir, ec)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(kSchemesDir, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                const auto path = entry.path();
                if (path.extension() != ".json") {
                    continue;
                }
                std::ifstream in(path);
                if (!in) {
                    continue;
                }
                Json::Value             root;
                Json::CharReaderBuilder rb;
                std::string             errs;
                if (!Json::parseFromStream(rb, in, &root, &errs)) {
                    continue;  // an unparseable file is skipped, never partly applied
                }
                DyeScheme s;
                if (FromJson(root, s)) {
                    found.push_back(std::move(s));
                }
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const DyeScheme& a, const DyeScheme& b) { return a.name < b.name; });

        std::scoped_lock l(g_lock);
        g_schemes = std::move(found);
    }

    std::vector<DyeScheme> Snapshot() {
        std::scoped_lock l(g_lock);
        return g_schemes;
    }

    std::size_t Count() {
        std::scoped_lock l(g_lock);
        return g_schemes.size();
    }

    bool Save(const DyeScheme& a_scheme) {
        if (a_scheme.name.empty()) {
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(kSchemesDir, ec);

        const auto path =
            std::filesystem::path(kSchemesDir) / FileNameFor(a_scheme.name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";  // hand-editable is the whole point of a global file
        out << Json::writeString(wb, ToJson(a_scheme));
        if (!out) {
            return false;
        }
        out.close();
        Load();
        return true;
    }

    bool Remove(std::string_view a_name) {
        if (a_name.empty()) {
            return false;
        }
        std::error_code ec;
        const auto      path =
            std::filesystem::path(kSchemesDir) / FileNameFor(a_name);
        const bool gone = std::filesystem::remove(path, ec);
        if (gone) {
            Load();
        }
        return gone && !ec;
    }

}  // namespace OS::DyeSchemes
