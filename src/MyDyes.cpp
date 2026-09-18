#include "MyDyes.h"

#include "BuildChannel.h"
#include "DyeBlend.h"   // the blend key's spelling, shared with the pack parser
#include "JsonCodec.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>

namespace OS::MyDyes {

    namespace {
        const auto kPackPath =
            std::filesystem::path(BuildChannel::DataPath("Dyes")) / "custom.json";
    }  // namespace

    std::string IdForName(std::string_view a_name) {
        std::string folded{ kIdPrefix };
        folded.reserve(a_name.size() + kIdPrefix.size());
        for (const char c : a_name) {
            // The same character set FileNameFor folds, for the same reason,
            // plus lowercasing so "Rose Gold" and "rose gold" are one dye
            // rather than two that shadow each other in the grid.
            const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                             c == '"' || c == '<' || c == '>' || c == '|' ||
                             static_cast<unsigned char>(c) < 0x20;
            folded.push_back(
                bad ? '_'
                    : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return folded;
    }

    Json::Value DyeToJson(const Dye& a_dye) {
        Json::Value o(Json::objectValue);
        o["id"]   = a_dye.id;
        o["name"] = a_dye.name;
        o["hex"]  = JsonCodec::ColourToHex(a_dye.colour);
        if (a_dye.colour.secondSet) {
            const DyeChannel second{ true, a_dye.colour.r2, a_dye.colour.g2, a_dye.colour.b2 };
            o["hex2"] = JsonCodec::ColourToHex(second);
        }
        if (a_dye.colour.mode == 1) {
            o["mode"] = "nacre";
        } else if (a_dye.colour.mode == 2) {
            o["mode"] = "iridescent";
        } else if (a_dye.colour.mode == 3) {
            o["mode"] = "metal";
        } else if (a_dye.colour.mode == 4) {
            o["mode"] = "cloth";
        } else if (a_dye.colour.mode == 5) {
            o["mode"] = "twotone";
        }
        if (a_dye.colour.flake > 0) {
            o["flake"] = a_dye.colour.flake;
        }
        // The cut, only when the dye says one: 128 is "nothing said" and the
        // default picture, so a dye that says nothing looks on disk exactly
        // like one authored before the byte existed.
        if (a_dye.colour.cut != 128) {
            o["cut"] = a_dye.colour.cut;
        }
        // ⚠ OMITTED WHEN THE DYE DEFERS, which is what the empty spelling says
        // and why NameForChoice has one. A dye that names no blend must look on
        // disk exactly like every dye authored before blends existed, so
        // writing "default" here would put a word in hand-editable files that
        // means nothing to a reader and nothing to the parser.
        if (const auto blend = OS::DyeBlend::NameForChoice(
                OS::DyeBlend::ChoiceFromByte(a_dye.colour.blend));
            !blend.empty()) {
            o["blend"] = std::string{ blend };
        }
        if (a_dye.colour.palette.glossSet) {
            o["gloss"] = a_dye.colour.palette.gloss;
        }
        if (a_dye.colour.palette.sheenSet) {
            const DyeChannel sheen{ true, a_dye.colour.palette.sheenR,
                                    a_dye.colour.palette.sheenG, a_dye.colour.palette.sheenB };
            o["sheen"] = JsonCodec::ColourToHex(sheen);
        }
        return o;
    }

    Json::Value PackToJson(const std::vector<Dye>& a_customs) {
        Json::Value root(Json::objectValue);
        Json::Value dyes(Json::arrayValue);
        for (const auto& d : a_customs) {
            dyes.append(DyeToJson(d));
        }
        root["dyes"] = std::move(dyes);
        return root;
    }

    void UpsertById(std::vector<Dye>& a_into, const Dye& a_dye) {
        for (auto& d : a_into) {
            if (d.id == a_dye.id) {
                d = a_dye;
                return;
            }
        }
        a_into.push_back(a_dye);
    }

    bool WritePack(const std::vector<Dye>& a_customs) {
        std::error_code ec;
        std::filesystem::create_directories(kPackPath.parent_path(), ec);
        std::ofstream out(kPackPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";  // hand-editable is the whole point of a pack file
        out << Json::writeString(wb, PackToJson(a_customs));
        if (!out) {
            return false;
        }
        out.close();
        return true;
    }

    std::optional<std::vector<Dye>> CustomsFromPackText(std::string_view a_text) {
        Json::Value             root;
        Json::CharReaderBuilder rb;
        const std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
        const char*                             begin = a_text.data();
        std::string                             errs;
        if (!reader || !begin ||
            !reader->parse(begin, begin + a_text.size(), &root, &errs)) {
            return std::nullopt;
        }
        // ⚠ isObject() BEFORE operator[], and it is a crash guard rather than
        // tidiness. jsoncpp only tolerates object or null on the left and
        // throws Json::LogicError for an array, a string or a number, the same
        // trap DyesFromJson documents. && short-circuits, so the reach for
        // "dyes" never happens on a root that would throw.
        if (!root.isObject()) {
            return std::nullopt;
        }
        // A pack with no "dyes" key at all is a readable EMPTY pack, not a
        // refusal: it is what the loader reads as zero dyes, and refusing it
        // would dead-end a player whose file was hand-emptied, declining every
        // save afterwards with the pack sitting right there. A "dyes" that is
        // present and not an array is the other case, a file saying something
        // this code does not understand, and that one is refused.
        if (root.isMember("dyes") && !root["dyes"].isArray()) {
            return std::nullopt;
        }

        std::vector<Dye> out;
        // The cost is the loader's concern and never round trips: DyeToJson
        // writes no "cost" key, so nothing here can carry one back out to
        // disk and any default is as good as any other. Zero says that
        // plainly rather than inventing a number the file will never hold.
        (void)DyePalette::DyesFromJson(root, 0, out);
        for (auto& d : out) {
            d.custom = true;
        }
        return out;
    }

    std::optional<std::vector<Dye>> LoadPack() {
        std::error_code ec;
        if (!std::filesystem::exists(kPackPath, ec) || ec) {
            // No pack yet is the first save's ordinary state, and an empty
            // list is the honest answer to it.
            return std::vector<Dye>{};
        }
        std::ifstream in(kPackPath, std::ios::binary);
        if (!in) {
            // ⚠ THE FILE IS THERE AND WE CANNOT OPEN IT, so this is the
            // refusal, not the empty pack above. Another process holding it
            // open is exactly when overwriting it would hurt most.
            return std::nullopt;
        }
        const std::string text{ std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>() };
        if (!in && !in.eof()) {
            return std::nullopt;
        }
        return CustomsFromPackText(text);
    }

    bool Save(const Dye& a_dye) {
        if (a_dye.name.empty() || a_dye.id.empty()) {
            return false;
        }
        auto customs = LoadPack();
        if (!customs) {
            return false;  // never write over a pack this build could not read
        }
        UpsertById(*customs, a_dye);
        return WritePack(*customs);
    }

    bool Remove(std::string_view a_id) {
        auto customs = LoadPack();
        if (!customs) {
            return false;
        }
        const auto before = customs->size();
        std::erase_if(*customs, [&](const Dye& a_d) { return a_d.id == a_id; });
        if (customs->size() == before) {
            return false;
        }
        return WritePack(*customs);
    }

}  // namespace OS::MyDyes
