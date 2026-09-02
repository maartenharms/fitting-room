#include "PCH.h"

#include "BodyMorphSource.h"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string_view>

namespace OS::BodyMorphSource {

    namespace {

        std::map<std::string, Source, std::less<>>& Cache() {
            static std::map<std::string, Source, std::less<>> cache;
            return cache;
        }

        [[nodiscard]] bool BoolAttr(const tinyxml2::XMLElement& a_node, const char* a_name) {
            bool value = false;
            return a_node.QueryBoolAttribute(a_name, &value) == tinyxml2::XML_SUCCESS && value;
        }

        [[nodiscard]] float FloatAttr(const tinyxml2::XMLElement& a_node, const char* a_name) {
            float value = 0.0f;
            return a_node.QueryFloatAttribute(a_name, &value) == tinyxml2::XML_SUCCESS ? value
                                                                                       : 0.0f;
        }

        [[nodiscard]] std::string TextOf(const tinyxml2::XMLElement* a_node) {
            if (!a_node) {
                return {};
            }
            const char* t = a_node->GetText();
            return t ? std::string{ t } : std::string{};
        }

        // A <Data> body reads "<file>.osd\<set name>". Both halves are wanted:
        // the file says which .osd to load, the set says which run of deltas
        // inside it this slider drives.
        //
        // ⚠ THE .osd NAME IS THE PROJECT'S, NOT THE MESH'S. They are usually
        // the same word and are not required to be, so it is read rather than
        // derived from SourceFile.
        struct DataRef {
            std::string file;
            std::string set;
        };

        [[nodiscard]] DataRef SplitDataRef(std::string a_text) {
            std::replace(a_text.begin(), a_text.end(), '/', '\\');
            const auto slash = a_text.rfind('\\');
            if (slash == std::string::npos) {
                return { {}, a_text };  // no file half: a set in the default .osd
            }
            return { a_text.substr(0, slash), a_text.substr(slash + 1) };
        }

    }  // namespace

    void Clear() {
        Cache().clear();
    }

    const Source* ForRuntime(const std::string& a_bodyMeshPath) {
        if (a_bodyMeshPath.empty()) {
            return nullptr;
        }
        const std::string cacheKey = "tri|" + a_bodyMeshPath;
        auto&             cache    = Cache();
        if (const auto it = cache.find(cacheKey); it != cache.end()) {
            return it->second.Usable() ? &it->second : nullptr;
        }
        Source& out = cache[cacheKey];

        // ⚠ THE .tri DROPS THE WEIGHT SUFFIX. A body ships as <name>_0.nif and
        // <name>_1.nif for the two weight endpoints and ONE <name>.tri beside
        // them, so the suffix has to come off or the file is never found.
        std::filesystem::path mesh(a_bodyMeshPath);
        std::string           stem = mesh.stem().string();
        if (stem.size() > 2 && stem[stem.size() - 2] == '_' &&
            (stem.back() == '0' || stem.back() == '1')) {
            stem.erase(stem.size() - 2);
        }
        const auto triPath =
            std::filesystem::path("Data") / "meshes" / mesh.parent_path() / (stem + ".tri");

        std::error_code ec;
        if (!std::filesystem::exists(triPath, ec)) {
            spdlog::warn("BodyMorphSource: no runtime morph data at '{}', so '{}' gets no "
                         "card from its built body either. A body carries one only when "
                         "it was built with morphs.",
                         triPath.string(), a_bodyMeshPath);
            return nullptr;
        }
        std::string triShape;
        if (!BodyMorphData::ParseTriFile(triPath.string(), out.deltas, &triShape)) {
            spdlog::warn("BodyMorphSource: '{}' exists but did not parse as runtime morph "
                         "data. The format check is exact by design: a file that does not "
                         "land on its final byte is refused rather than half read.",
                         triPath.string());
            return nullptr;
        }

        out.referenceMesh = a_bodyMeshPath;
        // ⚠ THE .tri NAMES ITS SHAPE AND THAT NAME IS THE GATE. The mesh it
        // sits beside carries physics colliders alongside the body, and a
        // field indexed by the body is meaningless on any of them.
        //
        // ⚠ ONE SHAPE, BY CONSTRUCTION. ParseTri keeps only the first shape's
        // morphs, because every shape indexes its own vertex array, so this
        // source can never describe more than the one it names. That is the
        // difference from the .osp path, which describes all of them.
        out.shapes.push_back(ShapeRules{ triShape, {} });
        // A .tri names its morphs the way a preset names its sliders, so each
        // one is its own rule and drives the delta set of the same name.
        for (const auto& [name, ignored] : out.deltas) {
            BodyMorphData::SliderRule rule;
            rule.dataName                = name;
            out.shapes.front().rules[name] = rule;
        }
        // ⚠ THE SHAPE NAME AND THE SPAN ARE THE PAIRING EVIDENCE. A .tri is
        // resolved through the VFS at a fixed path and the .nif beside it is
        // resolved separately, so two mods can win the two halves and the
        // indices land on a body they were never measured against.
        std::size_t span = 0;
        for (const auto& [ignored, set] : out.deltas) {
            for (const auto index : set.indices) {
                span = (std::max)(span, static_cast<std::size_t>(index) + 1);
            }
        }
        spdlog::info("BodyMorphSource: runtime '{}' from '{}' shape '{}' morphs {} span {}",
                     a_bodyMeshPath, triPath.string(), triShape, out.deltas.size(), span);
        return out.Usable() ? &out : nullptr;
    }

    const Source* For(const std::string& a_projectFile, const std::string& a_setName) {
        const std::string cacheKey = a_projectFile + "|" + a_setName;
        auto&             cache    = Cache();
        if (const auto it = cache.find(cacheKey); it != cache.end()) {
            return it->second.Usable() ? &it->second : nullptr;
        }
        // Cached either way, so a project that cannot be read is not re-parsed
        // once per card for the whole session.
        Source& out = cache[cacheKey];

        // ⚠ EVERY REFUSAL BELOW NAMES ITSELF. Two of these six used to warn
        // and the other four returned null in silence, so a crossed card with
        // a clean log meant nothing at all and the field had to guess between
        // "no project", "not XML", "wrong set" and "no rules". A cross is a
        // card the user can see; it must never be the only evidence.
        tinyxml2::XMLDocument doc;
        if (doc.LoadFile(a_projectFile.c_str()) != tinyxml2::XML_SUCCESS) {
            spdlog::warn("BodyMorphSource: '{}' would not open as XML, so '{}' gets no "
                         "card.",
                         a_projectFile, a_setName);
            return nullptr;
        }
        auto* root = doc.FirstChildElement("SliderSetInfo");
        if (!root) {
            spdlog::warn("BodyMorphSource: '{}' has no <SliderSetInfo>, so '{}' gets no "
                         "card.",
                         a_projectFile, a_setName);
            return nullptr;
        }

        std::string dataFolder;
        // ⚠ RESOLVED BEFORE THE WALK, so a run that names no file and one that
        // names this file explicitly build the SAME key. Leaving the empty
        // half in the key would parse one file twice and file it under two
        // prefixes: correct, and confusing to read in a log for no reason.
        const auto defaultOsd =
            std::filesystem::path(a_projectFile).stem().string() + ".osd";
        // Every .osd this set's runs name, already resolved.
        std::set<std::string> osdFiles;
        for (auto* set = root->FirstChildElement("SliderSet"); set;
             set = set->NextSiblingElement("SliderSet")) {
            const char* name = set->Attribute("name");
            if (!name || a_setName != name) {
                continue;
            }
            const auto folder = TextOf(set->FirstChildElement("DataFolder"));
            const auto source = TextOf(set->FirstChildElement("SourceFile"));
            if (folder.empty() || source.empty()) {
                spdlog::warn("BodyMorphSource: '{}' names set '{}' with DataFolder '{}' "
                             "and SourceFile '{}', and both are required, so it gets no "
                             "card.",
                             a_projectFile, a_setName, folder, source);
                return nullptr;
            }
            // ⚠ KEPT, because the .osd needs it too. Both the reference mesh
            // and the deltas live in ShapeData\<DataFolder>\, and losing this
            // one line collapsed the path to ShapeData\<file>.osd and crossed
            // every card a second time (field 2026-08-10).
            dataFolder = folder;
            out.referenceMesh =
                "CalienteTools\\BodySlide\\ShapeData\\" + folder + "\\" + source;

            // The shapes this set builds, in the .osp's own order. This is the
            // whitelist for what a card draws as well as the index for the
            // per-shape rules: a geometry the set never mentions is not part
            // of the body it describes.
            for (auto* shape = set->FirstChildElement("Shape"); shape;
                 shape = shape->NextSiblingElement("Shape")) {
                const char* target = shape->Attribute("target");
                if (target && *target) {
                    out.shapes.push_back(ShapeRules{ target, {} });
                }
            }
            const auto ruleSlot = [&out](std::string_view a_target)
                -> BodyMorphData::SliderRules* {
                for (auto& s : out.shapes) {
                    if (s.shape == a_target) {
                        return &s.rules;
                    }
                }
                return nullptr;
            };

            for (auto* slider = set->FirstChildElement("Slider"); slider;
                 slider = slider->NextSiblingElement("Slider")) {
                const char* sliderName = slider->Attribute("name");
                if (!sliderName || !*sliderName) {
                    continue;
                }
                BodyMorphData::SliderRule proto;
                proto.small  = FloatAttr(*slider, "small");
                proto.big    = FloatAttr(*slider, "big");
                proto.invert = BoolAttr(*slider, "invert");
                proto.zap    = BoolAttr(*slider, "zap");
                proto.uv     = BoolAttr(*slider, "uv");
                // ⚠⚠ EVERY <Data>, FILED UNDER ITS OWN target. This used to
                // take the first run and break, on the reasoning that "a body
                // card is morphing the body, so it wants that target's run
                // only". That is right about which run the BODY wants and
                // wrong about the card: the other runs are the other
                // GEOMETRIES in the same file, and dropping them left them to
                // be displaced by the body's field instead of their own.
                //
                // ⚠ THE target ATTRIBUTE IS AUTHORITATIVE AND IT IS THERE. A
                // run reads target="3BA_Vagina" beside name="3BA_VaginaArms",
                // so nothing here has to infer a shape from a name prefix,
                // which would have to get "3BA" versus "3BA_Vagina" right by
                // longest match and would fail silently when it did not.
                for (auto* data = slider->FirstChildElement("Data"); data;
                     data = data->NextSiblingElement("Data")) {
                    const auto ref = SplitDataRef(TextOf(data));
                    if (ref.set.empty()) {
                        continue;
                    }
                    // ⚠ A SET DRAWS FROM AS MANY .osd FILES AS IT NAMES. The
                    // nevernude sets take the body's runs from the nude set's
                    // file and the covering's from their own, so "the" .osd
                    // was never a thing. A run that names no file belongs to
                    // the project's own.
                    const auto refFile = ref.file.empty() ? defaultOsd : ref.file;
                    osdFiles.insert(refFile);
                    const char* target = data->Attribute("target");
                    auto*       slot =
                        target && *target
                                  ? ruleSlot(target)
                                  : (out.shapes.empty() ? nullptr : &out.shapes.front().rules);
                    if (!slot) {
                        // A run naming a shape the set does not declare. Its
                        // deltas index a geometry that is not in this build,
                        // so there is nothing to apply it to.
                        continue;
                    }
                    auto rule     = proto;
                    rule.dataName = BodyMorphData::DeltaKey(refFile, ref.set);
                    slot->emplace(sliderName, std::move(rule));
                }
            }
            break;
        }
        if (out.referenceMesh.empty() || out.RuleCount() == 0) {
            // The set was not in this .osp at all (the loop above never
            // matched the name and broke), or it was there and carried no
            // usable sliders. Both leave the card crossed and they have
            // different fixes, so the counts say which.
            spdlog::warn("BodyMorphSource: '{}' gave set '{}' no reference mesh ({}), "
                         "{} shape(s) and {} slider rule(s), so it gets no card. Zero "
                         "of all three usually means the .osp does not carry that set.",
                         a_projectFile, a_setName,
                         out.referenceMesh.empty() ? "none" : out.referenceMesh,
                         out.shapes.size(), out.RuleCount());
            return nullptr;
        }

        // ⚠⚠ THE .osd SITS WITH THE REFERENCE MESH, NOT WITH THE PROJECT. The
        // .osp lives in SliderSets\ and the deltas live in
        // ShapeData\<DataFolder>\ beside the .nif they displace, which is the
        // only place they could be: they are indexed by that mesh's vertices.
        // The first cut looked beside the .osp, found nothing, and every card
        // in the pane drew a cross (field 2026-08-10).
        const auto shapeData = std::filesystem::path(a_projectFile)
                                   .parent_path()   // SliderSets
                                   .parent_path()   // BodySlide
                                   / "ShapeData" / dataFolder;

        // ⚠⚠ EVERY FILE THE SET NAMES, AND ALL OF THEM OR NONE. A nevernude
        // set takes the body's runs from the nude set's .osd and the
        // covering's from its own, so loading one of two leaves whole shapes
        // with no deltas at all. That is a body that morphs inside underwear
        // that does not, which is a worse picture than no card and a much
        // quieter one. This file's standing rule applies: a refusal costs a
        // card, a half load costs a wrong picture.
        for (const auto& fileName : osdFiles) {
            const auto osdPath = shapeData / fileName;
            // ⚠ ABSENT AND MALFORMED ARE DIFFERENT PROBLEMS WITH DIFFERENT
            // FIXES, and saying "did not parse" for both cost a wrong first
            // diagnosis: the message sent the reader at the binary layout when
            // the real answer was a wrong directory. One message per cause.
            std::error_code ec;
            if (!std::filesystem::exists(osdPath, ec)) {
                spdlog::warn("BodyMorphSource: no morph data at '{}', so '{}' gets no "
                             "card. The file is expected beside the reference mesh in "
                             "ShapeData, not beside the .osp.",
                             osdPath.string(), a_setName);
                return nullptr;
            }
            BodyMorphData::DeltaSets fileSets;
            if (!BodyMorphData::ParseFile(osdPath.string(), fileSets)) {
                spdlog::warn("BodyMorphSource: '{}' exists but did not parse as morph "
                             "data, so '{}' gets no card. The format check is exact by "
                             "design: a set that does not land on the final byte is "
                             "refused rather than half read.",
                             osdPath.string(), a_setName);
                return nullptr;
            }
            // Filed under file-and-name, which is what the rules ask for. See
            // DeltaKey: the same run name in two files is not the same run.
            for (auto& [runName, set] : fileSets) {
                out.deltas.emplace(BodyMorphData::DeltaKey(fileName, runName), std::move(set));
            }
        }
        // Per shape, because "sliders 145" over a set that builds five
        // geometries never said which of them was actually driven, and the
        // shape that silently got none is the whole failure mode here.
        std::string shapeLine;
        for (const auto& s : out.shapes) {
            if (!shapeLine.empty()) {
                shapeLine += ' ';
            }
            shapeLine += s.shape;
            shapeLine += '=';
            shapeLine += std::to_string(s.rules.size());
        }
        // osd= is the nevernude tell: a set that draws from two files is the
        // case that used to load one of them and say nothing.
        std::string osdLine;
        for (const auto& f : osdFiles) {
            osdLine += (osdLine.empty() ? "" : ", ") + f;
        }
        spdlog::debug("BodyMorphSource: '{}' ref '{}' shapes [{}] rules {} deltaSets {} "
                      "osd {} [{}]",
                      a_setName, out.referenceMesh, shapeLine, out.RuleCount(),
                      out.deltas.size(), osdFiles.size(), osdLine);
        return out.Usable() ? &out : nullptr;
    }

}  // namespace OS::BodyMorphSource
