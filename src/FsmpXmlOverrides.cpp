#include "FsmpXmlOverrides.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace OS::FsmpXmlOverrides {

    namespace {

        struct Row {
            std::string sourceSuffix;  // lowered, backslashes
            std::string replacement;   // game-relative path, verbatim
        };
        std::vector<Row> g_rows;

        // The extra data's 'value' is a raw char* into NIF-owned storage; the
        // replacement string must outlive the clone, so it is interned here
        // for the session. Bounded by the number of mapped wigs ever worn.
        std::deque<std::string> g_interned;

        [[nodiscard]] std::string Lowered(const char* a_s) {
            std::string out{ a_s ? a_s : "" };
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return c == '/' ? '\\' : static_cast<char>(std::tolower(c));
            });
            return out;
        }

    }  // namespace

    void Load() {
        g_rows.clear();
        const char* path =
            "Data/SKSE/Plugins/FittingRoom/fsmp/fsmp-xml-overrides.json";
        std::ifstream in(path);
        if (!in) {
            spdlog::info("FsmpXmlOverrides: no override map shipped; every wig "
                         "keeps its own physics file.");
            return;
        }
        Json::Value  root;
        Json::Reader reader;
        if (!reader.parse(in, root) || !root["overrides"].isArray()) {
            spdlog::warn("FsmpXmlOverrides: the override map did not parse; "
                         "every wig keeps its own physics file.");
            return;
        }
        for (const auto& row : root["overrides"]) {
            const auto suffix      = row["sourceSuffix"].asString();
            const auto replacement = row["replacement"].asString();
            if (suffix.empty() || replacement.empty()) {
                continue;
            }
            g_rows.push_back({ Lowered(suffix.c_str()), replacement });
        }
        spdlog::info("FsmpXmlOverrides: {} wig xml override(s) loaded.",
                     g_rows.size());
    }

    namespace {

        // r54, Jenassa's wig: the fresh root a claim hands over is usually the
        // GEOMETRY itself (FSMP's SkinSingleGeometry logs that very object),
        // and extra data lives on NiObjectNET, which every NiAVObject is. The
        // old AsNode() gate returned false on every shape without a word, the
        // original GuanYinping.xml reached FSMP, and the 29 body bones held
        // her still. One object's own extra-data list:
        bool SwapOnObject(RE::NiAVObject* a_obj) {
            const auto count = a_obj->GetExtraDataSize();
            for (std::uint16_t i = 0; i < count; ++i) {
                auto* const xd = a_obj->GetExtraDataAt(i);
                if (!xd || !xd->name.c_str() ||
                    std::strcmp(xd->name.c_str(),
                                "HDT Skinned Mesh Physics Object") != 0) {
                    continue;
                }
                auto* const sd = netimmerse_cast<RE::NiStringExtraData*>(xd);
                if (!sd || !sd->value || !sd->value[0]) {
                    continue;
                }
                const auto have = Lowered(sd->value);
                for (const auto& row : g_rows) {
                    if (have.size() < row.sourceSuffix.size() ||
                        have.compare(have.size() - row.sourceSuffix.size(),
                                     row.sourceSuffix.size(),
                                     row.sourceSuffix) != 0) {
                        continue;
                    }
                    g_interned.push_back(row.replacement);
                    const char* was = sd->value;
                    sd->value = const_cast<char*>(g_interned.back().c_str());
                    spdlog::info(
                        "FsmpXmlOverrides: '{}' physics file '{}' registers "
                        "body bones FSMP would re-pose over CBPC, swapped to "
                        "'{}'.",
                        a_obj->name.c_str() ? a_obj->name.c_str() : "?", was,
                        g_interned.back());
                    return true;
                }
            }
            return false;
        }

        // The exporter parks the extra data on whichever node it liked; the
        // claim hands over one root per part. Sweep the subtree so the file
        // is re-pointed wherever it sits, before the publish reads it.
        bool SwapWalk(RE::NiAVObject* a_obj, int a_depth) {
            if (!a_obj || a_depth > 16) {
                return false;
            }
            bool swapped = SwapOnObject(a_obj);
            if (auto* const node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child) {
                        swapped |= SwapWalk(child.get(), a_depth + 1);
                    }
                }
            }
            return swapped;
        }

    }  // namespace

    bool SwapIfMapped(RE::NiAVObject* a_root) {
        if (!a_root || g_rows.empty()) {
            return false;
        }
        return SwapWalk(a_root, 0);
    }

}  // namespace OS::FsmpXmlOverrides
