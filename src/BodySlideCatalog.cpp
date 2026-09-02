#include "BodySlideCatalog.h"

#include "BodyMeshPath.h"

#include <tinyxml2.h>

// ⚠ INCLUDED RATHER THAN INHERITED FROM THE PCH. This file is compiled into
// BodySlideCatalogTests as well as into the plugin, and the test target does
// not carry the plugin's precompiled header, so the logging added on
// 2026-08-29 does not build there without this line.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace OS {

    namespace {
        constexpr std::uintmax_t kMaxXmlBytes = 8u * 1024u * 1024u;
        constexpr std::size_t    kMaxSliders = 2048;
        constexpr std::size_t    kMaxNameBytes = 512;
        // ⚠⚠ kClothedTokens AND IsClothedName ARE GONE FROM THIS SCAN, and the
        // reason is that they were censoring the player's own library. They
        // skipped any preset, and any whole preset FILE, whose name contained
        // cloth / outfit / nevernude / bikini / push / cleavage / armor /
        // armour. Every <Preset> under SliderPresets is a body preset the
        // player chose in BodySlide, so "HIMBO Simple (Clothes)" and "Fit to
        // Thicc - Pushup" are bodies meant to be worn a certain way, not outfit
        // conversions; those live in SliderSets and never reach here. Measured
        // on the reference load order the filename arm alone deleted four whole
        // files, taking presets that merely sat BESIDE a clothed sibling, and
        // the name arm deleted six more. A scan is not the place to decide what
        // a player may see: BodyStudioProofData.cpp keeps its own copy of the
        // token list for the question that actually needs it.

        struct RawSlider {
            float smallValue{ 0.0f };
            float bigValue{ 0.0f };
        };

        struct RawPreset {
            std::string name;
            std::string sourceSet;
            std::vector<std::string> groups;
            std::map<std::string, RawSlider, std::less<>> sliders;
            std::filesystem::path file;
        };

        struct CategoryChoice {
            BodyFamily family{ BodyFamily::kUnknown };
            std::string category;
            std::string display;
            std::size_t order{ 0 };
        };

        struct ProjectSlider {
            std::string name;
        };

        struct ProjectCandidate {
            std::filesystem::path file;
            std::vector<ProjectSlider> sliders;
            BodyFamily family{ BodyFamily::kUnknown };
            BodySex sex{ BodySex::kUnknown };
            bool bodyOutput{ false };
            // ⚠ THE MESH THIS SET ACTUALLY BUILDS, and it is the only thing in
            // this struct no preview can move. See BodyMeshPath.h.
            std::string outputMesh;
            // Whether the set builds the _0 / _1 pair rather than a bare .nif,
            // which is what decides the filename a card has to load.
            bool        genWeights{ false };
        };

        // ⚠⚠ SCORED AGAINST ONE PRESET, NOT ONCE FOR ALL OF THEM. The overlap
        // used to be a field on the candidate, summed while reading the .osp
        // over every preset whose set= named that candidate directly. A preset
        // rescued through a SliderGroup is by definition NOT one of those, so
        // every member of the group would have scored zero, tied, and been
        // refused as ambiguous - the rescue would have found the sets and then
        // thrown them all away.
        [[nodiscard]] std::uint64_t SliderOverlap(const ProjectCandidate& a_candidate,
                                                  const RawPreset&        a_preset) {
            std::uint64_t overlap = 0;
            for (const auto& slider : a_candidate.sliders) {
                if (a_preset.sliders.contains(slider.name)) ++overlap;
            }
            return overlap;
        }

        // ⚠⚠ A DUPLICATE NAME IS ONLY AMBIGUOUS WHEN THE DUPLICATES DISAGREE,
        // which is the same rule ChooseCandidate applies to tied projects and
        // was not applied here. Two mods shipping the same preset is ordinary:
        // HIMBO's own `HIMBO.xml` and a `HIMBO Five.xml` beside it both declare
        // `HIMBO Five` on the same set with the same 57 sliders, so there is
        // nothing to choose between them and nothing to be uncertain about.
        // Refusing on the count alone drew a preset the player owns as a
        // crossed-out card reading "this preset carries no slider values"
        // (field, 2026-08-29).
        [[nodiscard]] bool RawPresetsAgree(const std::vector<RawPreset>& a_entries) {
            if (a_entries.size() < 2) {
                return true;
            }
            const auto& first = a_entries.front();
            for (const auto& other : a_entries) {
                if (other.sourceSet != first.sourceSet ||
                    other.sliders.size() != first.sliders.size()) {
                    return false;
                }
                for (const auto& [name, value] : first.sliders) {
                    const auto it = other.sliders.find(name);
                    if (it == other.sliders.end() ||
                        it->second.smallValue != value.smallValue ||
                        it->second.bigValue != value.bigValue) {
                        return false;
                    }
                }
            }
            return true;
        }

        struct ScoredCandidate {
            const ProjectCandidate* candidate{ nullptr };
            std::uint64_t           score{ 0 };
            std::uint64_t           overlap{ 0 };
            std::string             setName;
        };

        // ⚠⚠ A TIE IS ONLY A TIE WHEN THE TIED ANSWERS DIFFER. Refusing any
        // score tie outright threw away the ordinary case: a body ships several
        // variants of one set - "HIMBO Body - SOS", "HIMBO Body - SOS High
        // Poly", "HIMBO Body - SOS Phys SMP" - carrying the same sliders, the
        // same family and the same output mesh, so which of them wins cannot
        // change anything a preset is built from. Only a tie whose members
        // disagree is real ambiguity, and that is what this refuses.
        [[nodiscard]] const ProjectCandidate* ChooseCandidate(
            const std::vector<ScoredCandidate>& a_scored, std::string& a_resolvedSet) {
            if (a_scored.empty()) {
                return nullptr;
            }
            const auto best =
                std::ranges::max_element(a_scored, {}, &ScoredCandidate::score);
            const ProjectCandidate* winner = best->candidate;
            for (const auto& other : a_scored) {
                if (other.score != best->score || other.candidate == winner) {
                    continue;
                }
                if (other.candidate->family != winner->family ||
                    other.candidate->sex != winner->sex ||
                    other.candidate->outputMesh != winner->outputMesh ||
                    other.candidate->genWeights != winner->genWeights) {
                    return nullptr;
                }
            }
            a_resolvedSet = best->setName;
            return winner;
        }

        [[nodiscard]] std::string Lower(std::string_view a_value) {
            std::string out(a_value);
            std::ranges::transform(out, out.begin(), [](unsigned char a_char) {
                return static_cast<char>(std::tolower(a_char));
            });
            return out;
        }

        // ⚠ ONE CLASSIFIER, EXPOSED RATHER THAN COPIED. The SFW option has to
        // ask the same question of a set name that the scan does, and two
        // classifiers that can disagree would pair a 3BA body with a CBBE
        // covering on whichever install made them differ.
        [[nodiscard]] BodyFamily FamilyFromSignature(std::string_view a_signature) {
            return FamilyFromSetSignature(a_signature);
        }

        // Does this slider set build the character's OWN body, as opposed to a
        // conversion of something worn over it?
        //
        // ⚠⚠ THE PATH DECIDES, NOT THE FILENAME, and getting that wrong picked
        // an armour. The test used to be "the output filename contains body",
        // which is true of "dawnguardbody1m" - HIMBO's Dawnguard armour
        // conversion - just as it is of "malebody". Measured on the reference
        // load order, that conversion carries the same 126 sliders as the real
        // body set and so ties with it on every other signal there is, so a
        // preset rescued through the HIMBO group resolved to a suit of armour.
        // A character's body mesh comes from Actor::GetSkin() and lives in the
        // vanilla assets folder; nothing worn does.
        [[nodiscard]] bool IsOwnBodyOutput(std::string_view a_meshKey) {
            constexpr std::string_view kAssets{ "actors\\character\\character assets\\" };
            return a_meshKey.starts_with(kAssets) &&
                   a_meshKey.find("body") != std::string_view::npos;
        }

        [[nodiscard]] BodySex SexFromSignature(std::string_view a_signature,
                                               BodyFamily a_family) {
            if (a_family == BodyFamily::k3BA || a_family == BodyFamily::kUBE ||
                a_family == BodyFamily::kCBBE) {
                return BodySex::kFemale;
            }
            if (a_family == BodyFamily::kHIMBO) {
                return BodySex::kMale;
            }
            const auto lower = Lower(a_signature);
            if (lower.find("femalebody") != std::string::npos ||
                lower.find("female body") != std::string::npos) {
                return BodySex::kFemale;
            }
            if (lower.find("malebody") != std::string::npos ||
                lower.find("male body") != std::string::npos) {
                return BodySex::kMale;
            }
            return BodySex::kUnknown;
        }

        [[nodiscard]] bool BoolAttr(const tinyxml2::XMLElement& a_node,
                                    const char* a_name) {
            bool value = false;
            return a_node.QueryBoolAttribute(a_name, &value) == tinyxml2::XML_SUCCESS &&
                   value;
        }

        [[nodiscard]] std::optional<std::string> ReadTextFile(
            const std::filesystem::path& a_path, std::size_t& a_rejected) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(a_path, ec);
            if (ec || size > kMaxXmlBytes) {
                ++a_rejected;
                return std::nullopt;
            }
            std::ifstream in(a_path, std::ios::binary);
            if (!in) {
                ++a_rejected;
                return std::nullopt;
            }
            std::string text;
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!in.good() && !in.eof()) {
                ++a_rejected;
                return std::nullopt;
            }
            return text;
        }

        [[nodiscard]] std::vector<std::filesystem::path> XmlFiles(
            const std::filesystem::path& a_root,
            std::initializer_list<std::string_view> a_extensions) {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            if (!std::filesystem::exists(a_root, ec) || ec) {
                return files;
            }
            // ⚠⚠ RECURSIVE, WHICH IT WAS NOT. BodySlide walks SliderPresets
            // to any depth and mods use that: presets arrive in per-author
            // subfolders, and this plugin's OWN exports go to
            // SliderPresets\Fitting Room (BodyMorphPresets.cpp kExport), so a
            // flat directory_iterator could not see the files this mod writes.
            // BodyMorphCatalog::FilesUnder already documents the same finding
            // for the same tree; the two scanners disagreeing about depth is
            // exactly the drift that makes one of them wrong in the field.
            for (std::filesystem::recursive_directory_iterator it(
                     a_root, std::filesystem::directory_options::skip_permission_denied,
                     ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }
                const auto ext = Lower(it->path().extension().string());
                if (std::ranges::any_of(a_extensions, [&](std::string_view a_want) {
                        return ext == a_want;
                    })) {
                    files.push_back(it->path());
                }
            }
            std::ranges::sort(files);
            return files;
        }

        void ReadConfig(const std::filesystem::path& a_path,
                        BodySlideCatalogSnapshot& a_out) {
            // ⚠ ABSENT IS NOT REJECTED, and the new summary line is what made
            // that visible. Config.xml is optional - the slider bounds have
            // defaults - but ReadTextFile counts any unreadable path as a
            // rejection, so an install without one reported a rejected file it
            // did not have and every other rejection count read one too high.
            std::error_code ec;
            if (!std::filesystem::exists(a_path, ec) || ec) {
                return;
            }
            auto text = ReadTextFile(a_path, a_out.rejectedFiles);
            if (!text) return;
            tinyxml2::XMLDocument doc;
            if (doc.Parse(text->data(), text->size()) != tinyxml2::XML_SUCCESS) {
                ++a_out.rejectedFiles;
                return;
            }
            auto* root = doc.FirstChildElement("Config");
            auto* input = root ? root->FirstChildElement("Input") : nullptr;
            float minimum = 0.0f;
            float maximum = 100.0f;
            if (input) {
                if (auto* node = input->FirstChildElement("SliderMinimum")) {
                    node->QueryFloatText(&minimum);
                }
                if (auto* node = input->FirstChildElement("SliderMaximum")) {
                    node->QueryFloatText(&maximum);
                }
            }
            if (std::isfinite(minimum) && std::isfinite(maximum) && minimum < maximum) {
                a_out.sliderMinimum = minimum;
                a_out.sliderMaximum = maximum;
            }
        }

        void ReadCategories(
            const std::filesystem::path& a_root,
            std::unordered_map<std::string, std::vector<CategoryChoice>>& a_choices,
            BodySlideCatalogSnapshot& a_out) {
            std::size_t order = 0;
            for (const auto& path : XmlFiles(a_root, { ".xml" })) {
                ++a_out.categoryFilesScanned;
                auto text = ReadTextFile(path, a_out.rejectedFiles);
                if (!text) continue;
                tinyxml2::XMLDocument doc;
                if (doc.Parse(text->data(), text->size()) != tinyxml2::XML_SUCCESS) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                auto* root = doc.FirstChildElement("SliderCategories");
                if (!root) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                const BodyFamily hint = FamilyFromSignature(path.stem().string());
                for (auto* category = root->FirstChildElement("Category"); category;
                     category = category->NextSiblingElement("Category")) {
                    const char* categoryName = category->Attribute("name");
                    if (!categoryName || !*categoryName ||
                        std::string_view(categoryName).size() > kMaxNameBytes) {
                        continue;
                    }
                    for (auto* slider = category->FirstChildElement("Slider"); slider;
                         slider = slider->NextSiblingElement("Slider")) {
                        const char* name = slider->Attribute("name");
                        if (!name || !*name || std::string_view(name).size() > kMaxNameBytes) {
                            continue;
                        }
                        const char* display = slider->Attribute("displayname");
                        a_choices[name].push_back(CategoryChoice{
                            hint, categoryName,
                            display && *display ? std::string(display) : std::string(name),
                            order++,
                        });
                    }
                }
            }
        }

        [[nodiscard]] const CategoryChoice* PickCategory(
            const std::unordered_map<std::string, std::vector<CategoryChoice>>& a_choices,
            std::string_view a_slider, BodyFamily a_family) {
            const auto it = a_choices.find(std::string(a_slider));
            if (it == a_choices.end()) return nullptr;
            const auto& choices = it->second;
            if (const auto exact = std::ranges::find_if(choices, [&](const CategoryChoice& a_c) {
                    return a_c.family == a_family;
                }); exact != choices.end()) {
                return &*exact;
            }
            return choices.empty() ? nullptr : &choices.front();
        }

        void ReadPresets(const std::filesystem::path& a_root,
                         const std::unordered_set<std::string>& a_allowed,
                         std::unordered_map<std::string, std::vector<RawPreset>>& a_presets,
                         BodySlideCatalogSnapshot& a_out) {
            for (const auto& path : XmlFiles(a_root, { ".xml" })) {
                ++a_out.presetFilesScanned;
                auto text = ReadTextFile(path, a_out.rejectedFiles);
                if (!text) continue;
                tinyxml2::XMLDocument doc;
                if (doc.Parse(text->data(), text->size()) != tinyxml2::XML_SUCCESS) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                auto* root = doc.FirstChildElement("SliderPresets");
                if (!root) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                for (auto* node = root->FirstChildElement("Preset"); node;
                     node = node->NextSiblingElement("Preset")) {
                    const char* name = node->Attribute("name");
                    const char* set = node->Attribute("set");
                    // ⚠ ONE REASON PER BRANCH, NOT ONE COMBINED CONDITION. The
                    // eight tests below used to be a single `if` with a bare
                    // `continue`, so every preset that failed any of them left
                    // the scan identically silent and a field report could not
                    // be attributed to one of them.
                    if (!name || !*name || !set || !*set) {
                        ++a_out.drops.missingNameOrSet;
                        spdlog::debug(
                            "BodySlide: '{}' skipped a preset with no name or an "
                            "empty set.",
                            path.filename().string());
                        continue;
                    }
                    if (std::string_view(name).size() > kMaxNameBytes ||
                        std::string_view(set).size() > kMaxNameBytes) {
                        ++a_out.drops.oversizeName;
                        spdlog::warn("BodySlide: preset name or set over {} bytes in '{}'.",
                                     kMaxNameBytes, path.filename().string());
                        continue;
                    }
                    if (!a_allowed.empty() && !a_allowed.contains(name)) {
                        // ⚠⚠ THIS IS THE TOP OF THE FUNNEL AND IT IS NOT OURS.
                        // a_allowed is OBody's roster for the subject's sex, so
                        // the catalog is a projection of OBody's list and never
                        // of the BodySlide install. A preset OBody does not
                        // report cannot appear anywhere in this mod however
                        // well it scans, and half the install is absent by
                        // construction whenever the subject is the other sex.
                        // Counted so a log can say so instead of implying we
                        // lost it.
                        ++a_out.drops.notInRoster;
                        continue;
                    }
                    RawPreset preset;
                    preset.name = name;
                    preset.sourceSet = set;
                    preset.file = path;
                    for (auto* group = node->FirstChildElement("Group"); group;
                         group = group->NextSiblingElement("Group")) {
                        if (const char* groupName = group->Attribute("name");
                            groupName && *groupName &&
                            std::string_view(groupName).size() <= kMaxNameBytes) {
                            preset.groups.emplace_back(groupName);
                        }
                    }
                    bool malformed = false;
                    for (auto* slider = node->FirstChildElement("SetSlider"); slider;
                         slider = slider->NextSiblingElement("SetSlider")) {
                        const char* sliderName = slider->Attribute("name");
                        const char* size = slider->Attribute("size");
                        float value = 0.0f;
                        if (!sliderName || !*sliderName || !size ||
                            std::string_view(sliderName).size() > kMaxNameBytes ||
                            (std::string_view(size) != "small" &&
                             std::string_view(size) != "big") ||
                            slider->QueryFloatAttribute("value", &value) !=
                                tinyxml2::XML_SUCCESS ||
                            !std::isfinite(value)) {
                            malformed = true;
                            break;
                        }
                        auto& raw = preset.sliders[sliderName];
                        float& endpoint = std::string_view(size) == "big"
                                              ? raw.bigValue
                                              : raw.smallValue;
                        if (endpoint == 0.0f && value != 0.0f) {
                            endpoint = value;
                        }
                        if (preset.sliders.size() > kMaxSliders) {
                            malformed = true;
                            break;
                        }
                    }
                    if (malformed) {
                        // ⚠ COUNTED AS A PRESET, NOT AS A REJECTED FILE, which
                        // is what it used to add to. One bad SetSlider row cost
                        // the file a rejection it had not earned and made the
                        // rejected-file total unreadable.
                        ++a_out.drops.malformedSlider;
                        spdlog::warn(
                            "BodySlide: preset '{}' in '{}' has a slider row that "
                            "would not parse; skipped.",
                            preset.name, path.filename().string());
                        continue;
                    }
                    a_presets[preset.name].push_back(std::move(preset));
                }
            }
        }

        [[nodiscard]] std::string ProjectSignature(tinyxml2::XMLElement& a_set) {
            std::string signature;
            if (const char* name = a_set.Attribute("name")) signature += name;
            for (const char* element : { "SourceFile", "OutputPath", "OutputFile" }) {
                if (auto* child = a_set.FirstChildElement(element); child && child->GetText()) {
                    signature.push_back('|');
                    signature += child->GetText();
                }
            }
            for (auto* shape = a_set.FirstChildElement("Shape"); shape;
                 shape = shape->NextSiblingElement("Shape")) {
                if (const char* target = shape->Attribute("target")) {
                    signature.push_back('|');
                    signature += target;
                }
                if (shape->GetText()) {
                    signature.push_back('|');
                    signature += shape->GetText();
                }
            }
            return signature;
        }

        // BodySlide's SliderGroups files, which name a group and list the slider
        // sets in it:
        //
        //   <SliderGroups><Group name="HIMBO"><Member name="HIMBO Body - SOS"/>
        //
        // ⚠⚠ NOTHING IN THIS PLUGIN HAD EVER OPENED ONE, AND THAT IS WHERE HALF
        // THE LIBRARY WENT. A preset's set= attribute is not required to name a
        // slider set; naming a GROUP is ordinary and the bodies people actually
        // install do it. HIMBO's own shipped presets all carry set="HIMBO", and
        // "HIMBO" is a group of 848 members, not a set - no .osp anywhere on the
        // reference load order declares <SliderSet name="HIMBO">. Measured
        // there, 64 of 129 installed presets named a set that resolved to
        // nothing, 30 of them through this one group, and every one of them was
        // dropped with a clean log. Resolving the group rescues 60 of the 61.
        //
        // ⚠ KEYED LOWERCASE. Group names are written by whichever mod author
        // added to the group, and they do not agree on case.
        void ReadGroups(const std::filesystem::path& a_root,
                        std::unordered_map<std::string, std::vector<std::string>>& a_groups,
                        BodySlideCatalogSnapshot& a_out) {
            for (const auto& path : XmlFiles(a_root, { ".xml" })) {
                ++a_out.groupFilesScanned;
                auto text = ReadTextFile(path, a_out.rejectedFiles);
                if (!text) continue;
                tinyxml2::XMLDocument doc;
                if (doc.Parse(text->data(), text->size()) != tinyxml2::XML_SUCCESS) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                auto* root = doc.FirstChildElement("SliderGroups");
                if (!root) continue;
                for (auto* group = root->FirstChildElement("Group"); group;
                     group = group->NextSiblingElement("Group")) {
                    const char* groupName = group->Attribute("name");
                    if (!groupName || !*groupName ||
                        std::string_view(groupName).size() > kMaxNameBytes) {
                        continue;
                    }
                    auto& members = a_groups[Lower(groupName)];
                    for (auto* member = group->FirstChildElement("Member"); member;
                         member = member->NextSiblingElement("Member")) {
                        const char* memberName = member->Attribute("name");
                        if (!memberName || !*memberName ||
                            std::string_view(memberName).size() > kMaxNameBytes) {
                            continue;
                        }
                        members.emplace_back(memberName);
                    }
                }
            }
            // Every mod that adds to a group re-declares the whole group, so the
            // same set arrives once per contributing mod.
            for (auto& [_, members] : a_groups) {
                std::ranges::sort(members);
                const auto stale = std::ranges::unique(members);
                members.erase(stale.begin(), stale.end());
            }
        }

        void ReadProjects(const std::filesystem::path& a_root,
                          const std::unordered_map<std::string, std::vector<RawPreset>>& a_presets,
                          const std::unordered_map<std::string, std::vector<std::string>>& a_groups,
                          std::unordered_map<std::string, std::vector<ProjectCandidate>>& a_projects,
                          BodySlideCatalogSnapshot& a_out) {
            std::unordered_set<std::string> wanted;
            for (const auto& [_, presets] : a_presets) {
                for (const auto& preset : presets) {
                    wanted.insert(preset.sourceSet);
                    // ⚠ THE MEMBERS OF A GROUP A PRESET NAMES ARE WANTED TOO,
                    // or the rescue below has nothing to pick from: candidates
                    // are only built for sets in this set, so a group member
                    // that no preset names directly would never become one.
                    if (const auto group = a_groups.find(Lower(preset.sourceSet));
                        group != a_groups.end()) {
                        for (const auto& member : group->second) wanted.insert(member);
                    }
                }
            }

            for (const auto& path : XmlFiles(a_root, { ".osp", ".xml" })) {
                ++a_out.projectFilesScanned;
                auto text = ReadTextFile(path, a_out.rejectedFiles);
                if (!text) continue;
                // ⚠ THE `wanted` PREFILTER IS GONE AND IT COST ALMOST NOTHING
                // TO LOSE. It skipped the XML parse for a file naming no
                // installed preset's set, but the file's TEXT was read either
                // way, so the I/O was always paid and only the parse was
                // saved. It also made projectForSet blind to exactly the sets
                // that needed it: a set no installed preset uses was never
                // recorded, so every custom preset on it drew a cross.

                tinyxml2::XMLDocument doc;
                if (doc.Parse(text->data(), text->size()) != tinyxml2::XML_SUCCESS) {
                    ++a_out.rejectedFiles;
                    continue;
                }
                auto* root = doc.FirstChildElement("SliderSetInfo");
                if (!root) continue;
                for (auto* set = root->FirstChildElement("SliderSet"); set;
                     set = set->NextSiblingElement("SliderSet")) {
                    const char* setName = set->Attribute("name");
                    if (!setName) continue;
                    // Recorded for EVERY set, before the installed-preset
                    // filter below, so a set only custom presets use can still
                    // be found. First file wins; see the header.
                    a_out.projectForSet.emplace(setName, path.string());
                    // ⚠ RECORDED FOR EVERY SET TOO, and for the same reason
                    // projectForSet is: the covered sibling of a body set
                    // usually has NO installed preset, so everything below the
                    // `wanted` filter is blind to it. Without this, telling a
                    // femalebody set from a maleunderwear one means reopening
                    // the .osp per candidate.
                    if (auto* output = set->FirstChildElement("OutputFile");
                        output && output->GetText()) {
                        a_out.outputForSet.emplace(setName, output->GetText());
                        // ⚠ THE JOINED KEY AS WELL AS THE RAW FILENAME, because
                        // outputForSet's value is the <OutputFile> text alone
                        // and a mesh key needs <OutputPath> in front of it.
                        // FamilyForMesh compares against a character's built
                        // mesh, so it cannot use the half that has no directory.
                        const char* outDirText = nullptr;
                        if (auto* outDir = set->FirstChildElement("OutputPath")) {
                            outDirText = outDir->GetText();
                        }
                        auto key = BodyMeshKey(
                            outDirText ? std::string_view{ outDirText }
                                       : std::string_view{},
                            output->GetText());
                        if (!key.empty()) {
                            a_out.meshForDeclaredSet.emplace(setName, std::move(key));
                        }
                    }
                    if (!wanted.contains(setName)) continue;
                    ProjectCandidate candidate;
                    candidate.file = path;
                    const auto signature = ProjectSignature(*set);
                    candidate.family = FamilyFromSignature(signature);
                    candidate.sex = SexFromSignature(signature, candidate.family);
                    // ⚠ THE PATH IS KEPT NOW, NOT JUST A BOOL OFF IT. The
                    // <OutputPath> and <OutputFile> pair names the mesh this
                    // set builds, which is what a character's own body mesh is
                    // compared against. Reading only "does the name contain
                    // body" threw that away.
                    const char* outputPath = nullptr;
                    if (auto* outDir = set->FirstChildElement("OutputPath")) {
                        outputPath = outDir->GetText();
                    }
                    if (auto* output = set->FirstChildElement("OutputFile")) {
                        candidate.genWeights = BoolAttr(*output, "GenWeights");
                        if (output->GetText()) {
                            candidate.outputMesh = BodyMeshKey(
                                outputPath ? std::string_view{ outputPath }
                                           : std::string_view{},
                                output->GetText());
                        }
                        candidate.bodyOutput = candidate.genWeights &&
                                               IsOwnBodyOutput(candidate.outputMesh);
                    }
                    for (auto* slider = set->FirstChildElement("Slider"); slider;
                         slider = slider->NextSiblingElement("Slider")) {
                        const char* name = slider->Attribute("name");
                        if (!name || !*name || std::string_view(name).size() > kMaxNameBytes ||
                            BoolAttr(*slider, "hidden") || BoolAttr(*slider, "zap")) {
                            continue;
                        }
                        if (BoolAttr(*slider, "uv")) {
                            ++a_out.excludedUvSliders;
                            continue;
                        }
                        if (!slider->FirstChildElement("Data")) continue;
                        candidate.sliders.push_back({ name });
                        if (candidate.sliders.size() > kMaxSliders) {
                            candidate.sliders.clear();
                            break;
                        }
                    }
                    if (candidate.sliders.empty()) continue;
                    // The overlap that used to be summed here is now asked per
                    // preset at the point of use; see SliderOverlap.
                    a_projects[setName].push_back(std::move(candidate));
                }
            }
        }
    }  // namespace

    const BodyCatalogPreset* BodySlideCatalogSnapshot::Find(std::string_view a_name) const {
        const auto it = std::ranges::find_if(presets, [&](const BodyCatalogPreset& a_preset) {
            return a_preset.seed.name == a_name;
        });
        return it == presets.end() ? nullptr : &*it;
    }

    BodyFamily BodySlideCatalogSnapshot::FamilyForMesh(std::string_view a_mesh) const {
        if (a_mesh.empty()) {
            return BodyFamily::kUnknown;
        }
        // ⚠⚠ EVERY DECLARED SET IS ASKED AND A DISAGREEMENT ANSWERS UNKNOWN.
        // This used to walk the name-sorted preset list and return the first
        // named family it found, which let ONE PRESET'S NAME decide what body
        // the character was wearing. CBBE and 3BA both build
        // actors\character\character assets\femalebody, so on the ordinary
        // CBBE+3BA install "- Zeroed Sliders -" sorted first, the body was
        // declared CBBE, and the fit filter hid all sixty 3BA presets as proven
        // misfits; removing that single preset flipped the answer and hid the
        // fifteen CBBE ones instead. outputForSet covers sets no installed
        // preset uses, so it sees the disagreement the preset list cannot.
        //
        // A mesh two families share is not evidence of either. Saying Unknown
        // is what lets BodyFitCompatible fail open, which is its documented
        // rule: only a PROVEN mismatch may hide a preset.
        BodyFamily agreed = BodyFamily::kUnknown;
        for (const auto& [setName, meshKey] : meshForDeclaredSet) {
            if (meshKey != a_mesh) {
                continue;
            }
            const auto family = FamilyFromSetSignature(setName);
            if (family == BodyFamily::kUnknown || family == BodyFamily::kGenericV1) {
                continue;
            }
            if (agreed == BodyFamily::kUnknown) {
                agreed = family;
            } else if (agreed != family) {
                return BodyFamily::kUnknown;
            }
        }
        if (agreed != BodyFamily::kUnknown) {
            return agreed;
        }
        // Nothing declared reaches this mesh, so fall back to the presets that
        // build it, on the same all-must-agree terms.
        for (const auto& preset : presets) {
            if (preset.outputMesh != a_mesh ||
                preset.seed.family == BodyFamily::kUnknown ||
                preset.seed.family == BodyFamily::kGenericV1) {
                continue;
            }
            if (agreed == BodyFamily::kUnknown) {
                agreed = preset.seed.family;
            } else if (agreed != preset.seed.family) {
                return BodyFamily::kUnknown;
            }
        }
        return agreed;
    }

    std::string_view BodySlideCatalogSnapshot::SetForMesh(std::string_view a_mesh) const {
        if (a_mesh.empty()) {
            return {};
        }
        // ⚠⚠ EXACTLY ONE, OR NOTHING. This feeds BuiltBody::sourceSet, whose
        // only arm in BodyFitCompatible returns true - it can prove a fit and
        // never a misfit - so a wrong answer here shows a preset that does not
        // suit rather than hiding one that does. That is still no reason to
        // guess: several sets build one mesh on any ordinary install, and none
        // of them is more the character's than another.
        std::string_view only;
        for (const auto& [setName, meshKey] : meshForDeclaredSet) {
            if (meshKey != a_mesh) {
                continue;
            }
            if (!only.empty()) {
                return {};
            }
            only = setName;
        }
        return only;
    }

    std::string_view BodySlideCatalogSnapshot::MeshForSet(std::string_view a_set) const {
        if (a_set.empty()) {
            return {};
        }
        const auto it = std::ranges::find_if(presets, [&](const BodyCatalogPreset& a_p) {
            return !a_p.outputMesh.empty() && a_p.seed.sourceSet == a_set;
        });
        return it == presets.end() ? std::string_view{} : std::string_view{ it->outputMesh };
    }

    BodySlideCatalogSnapshot ScanBodySlideCatalog(
        const std::filesystem::path& a_bodySlideRoot,
        const std::vector<std::string>& a_allowedPresetNames) {
        BodySlideCatalogSnapshot out;
        std::unordered_set<std::string> allowed(a_allowedPresetNames.begin(),
                                                 a_allowedPresetNames.end());
        std::unordered_map<std::string, std::vector<RawPreset>> rawPresets;
        std::unordered_map<std::string, std::vector<CategoryChoice>> categories;
        std::unordered_map<std::string, std::vector<ProjectCandidate>> projects;
        std::unordered_map<std::string, std::vector<std::string>> groups;

        ReadConfig(a_bodySlideRoot / "Config.xml", out);
        ReadCategories(a_bodySlideRoot / "SliderCategories", categories, out);
        ReadPresets(a_bodySlideRoot / "SliderPresets", allowed, rawPresets, out);
        // ⚠ GROUPS BEFORE PROJECTS. ReadProjects only keeps a candidate for a
        // set something wants, and what a group-named preset wants is every
        // member of that group, so the membership has to be known first.
        ReadGroups(a_bodySlideRoot / "SliderGroups", groups, out);
        ReadProjects(a_bodySlideRoot / "SliderSets", rawPresets, groups, projects, out);

        std::vector<std::string> names;
        if (!a_allowedPresetNames.empty()) {
            names = a_allowedPresetNames;
        } else {
            names.reserve(rawPresets.size());
            for (const auto& [name, _] : rawPresets) names.push_back(name);
            std::ranges::sort(names);
        }
        for (const auto& name : names) {
            BodyCatalogPreset item;
            item.seed.name = name;
            item.seed.sourcePreset = name;
            const auto rawIt = rawPresets.find(name);
            if (rawIt == rawPresets.end() || rawIt->second.empty()) {
                ++out.drops.noXmlMatch;
                spdlog::info(
                    "BodySlide: '{}' is on OBody's list but no SliderPresets XML "
                    "declares it.",
                    name);
                item.diagnostic = "No exact BodySlide preset XML match.";
                out.presets.push_back(std::move(item));
                continue;
            }
            if (rawIt->second.size() != 1 && !RawPresetsAgree(rawIt->second)) {
                ++out.drops.duplicateName;
                spdlog::info(
                    "BodySlide: '{}' is declared by {} XML entries that disagree about "
                    "the set or the slider values, so it cannot be resolved to one.",
                    name, rawIt->second.size());
                item.ambiguous = true;
                item.diagnostic = "Preset name resolves to multiple BodySlide XML entries.";
                out.presets.push_back(std::move(item));
                continue;
            }
            const auto& raw = rawIt->second.front();
            item.seed.sourceSet = raw.sourceSet;
            item.seed.groups = raw.groups;
            item.presetFile = raw.file;

            // The set the preset declares, scored against this preset's own
            // sliders rather than against every preset that shares the set.
            std::string             resolvedSet;
            std::vector<ScoredCandidate> scored;
            if (const auto projectIt = projects.find(raw.sourceSet);
                projectIt != projects.end()) {
                for (const auto& candidate : projectIt->second) {
                    const auto overlap = SliderOverlap(candidate, raw);
                    scored.push_back({ &candidate,
                                       (candidate.bodyOutput ? 1'000'000ull : 0ull) + overlap,
                                       overlap, raw.sourceSet });
                }
            }
            const ProjectCandidate* best = ChooseCandidate(scored, resolvedSet);
            const bool tied = !scored.empty() && best == nullptr;

            // ⚠⚠ THE GROUP RESCUE, AND IT ONLY RUNS WHEN THE DIRECT LOOKUP
            // FOUND NOTHING AT ALL. A score tie means two sets that really do
            // disagree, which is ambiguity to report rather than to route
            // around; an empty candidate list means set= was never a set name,
            // and that is the case a group answers. See ReadGroups.
            if (!best && !tied) {
                if (const auto group = groups.find(Lower(raw.sourceSet));
                    group != groups.end()) {
                    std::vector<ScoredCandidate> viaGroup;
                    for (const auto& member : group->second) {
                        const auto memberIt = projects.find(member);
                        if (memberIt == projects.end()) continue;
                        for (const auto& candidate : memberIt->second) {
                            const auto overlap = SliderOverlap(candidate, raw);
                            // ⚠ A MEMBER SHARING NO SLIDER IS NOT EVIDENCE. A
                            // group is a bag of every conversion any mod ever
                            // added to it - the HIMBO group holds 848 - so the
                            // only members that can be the body this preset was
                            // built on are the ones whose sliders it moves.
                            if (overlap == 0) continue;
                            viaGroup.push_back(
                                { &candidate,
                                  (candidate.bodyOutput ? 1'000'000ull : 0ull) + overlap,
                                  overlap, member });
                        }
                    }
                    best = ChooseCandidate(viaGroup, resolvedSet);
                    if (best) {
                        ++out.drops.setViaGroup;
                        spdlog::info(
                            "BodySlide: '{}' names group '{}' rather than a set; "
                            "resolved to '{}'.",
                            name, raw.sourceSet, resolvedSet);
                    }
                }
            }

            if (!best) {
                if (tied) {
                    ++out.drops.tiedProjects;
                    spdlog::info(
                        "BodySlide: '{}' set '{}' resolves to projects that disagree "
                        "on family, sex or output mesh.",
                        name, raw.sourceSet);
                    item.ambiguous = true;
                    item.diagnostic = "BodySlide source set resolves to tied projects.";
                    out.presets.push_back(std::move(item));
                    continue;
                }
                ++out.drops.setNotInstalled;
                spdlog::info(
                    "BodySlide: '{}' names set '{}', which no installed .osp and no "
                    "SliderGroup could resolve.",
                    name, raw.sourceSet);
                item.seed.family = FamilyFromSignature(raw.sourceSet);
                item.seed.sex = SexFromSignature(raw.sourceSet, item.seed.family);
                for (const auto& [sliderName, value] : raw.sliders) {
                    BodySliderValue slider{ sliderName, sliderName, "Other",
                                            value.smallValue, value.bigValue };
                    if (const auto* category = PickCategory(categories, sliderName,
                                                            item.seed.family)) {
                        slider.category = category->category;
                        slider.displayName = category->display;
                    }
                    item.seed.sliders.push_back(std::move(slider));
                }
                // ⚠⚠ ASSIGNED HERE TOO, WHICH IT WAS NOT. This branch filled
                // the family, the sex and every slider and then pushed the item
                // with item.authorable left at its default false, so the Body
                // Studio card grid skipped the row outright
                // (BodyStudioUI.cpp, `if (!item.authorable) continue`). A
                // preset with a known family, a known sex and sliders is
                // authorable whether or not we found the .osp it came from; the
                // only thing the missing project costs is outputMesh, and
                // BodyCardScene already has a path for that.
                item.authorable = item.seed.sex != BodySex::kUnknown &&
                                  item.seed.family != BodyFamily::kUnknown &&
                                  !item.seed.sliders.empty();
                item.diagnostic = "BodySlide project could not be resolved; installed use remains available.";
                out.presets.push_back(std::move(item));
                continue;
            }

            item.projectFile = best->file;
            item.seed.family = best->family;
            item.seed.sex = best->sex;
            item.outputMesh = best->outputMesh;
            item.genWeights = best->genWeights;
            for (const auto& projectSlider : best->sliders) {
                BodySliderValue slider;
                slider.name = projectSlider.name;
                slider.displayName = projectSlider.name;
                if (const auto rawValue = raw.sliders.find(projectSlider.name);
                    rawValue != raw.sliders.end()) {
                    slider.smallValue = rawValue->second.smallValue;
                    slider.bigValue = rawValue->second.bigValue;
                }
                if (const auto* category = PickCategory(categories, slider.name,
                                                        item.seed.family)) {
                    slider.category = category->category;
                    slider.displayName = category->display;
                }
                item.seed.sliders.push_back(std::move(slider));
            }
            item.authorable = item.seed.sex != BodySex::kUnknown &&
                              item.seed.family != BodyFamily::kUnknown &&
                              !item.seed.sliders.empty();
            item.diagnostic = item.authorable ? "Ready for customization."
                                              : "Project resolved but its sex/profile is unknown.";
            out.presets.push_back(std::move(item));
        }
        std::ranges::sort(out.presets, [](const BodyCatalogPreset& a_a,
                                         const BodyCatalogPreset& a_b) {
            return Lower(a_a.seed.name) < Lower(a_b.seed.name);
        });
        std::ostringstream diagnostic;
        diagnostic << out.presets.size() << " installed preset(s), "
                   << out.projectFilesScanned << " project file(s), "
                   << out.excludedUvSliders << " UV slider(s) excluded";
        if (out.rejectedFiles != 0) {
            diagnostic << ", " << out.rejectedFiles << " rejected file(s)";
        }
        out.diagnostic = diagnostic.str();
        // ⚠⚠ ONE SUMMARY LINE PER SCAN, AND IT IS THE POINT OF THIS WHOLE PASS.
        // The scan ran 663 lines with no logging in it at all, so "not all
        // bodyslide presets show up in your preset list" arrived with nothing in
        // FittingRoom.log that could tell a set the player never installed from
        // a name this code refused to read. Every reason is counted separately
        // because they all move the same total in the same direction.
        spdlog::info(
            "BodySlide scan: {} preset file(s), {} project file(s), {} group "
            "file(s) -> {} row(s). Not shown: {} outside OBody's roster, {} with "
            "no XML, {} duplicated, {} with an unresolvable set, {} tied, {} "
            "malformed slider, {} nameless, {} oversize. Rescued through a "
            "SliderGroup: {}. Rejected files: {}.",
            out.presetFilesScanned, out.projectFilesScanned, out.groupFilesScanned,
            out.presets.size(), out.drops.notInRoster, out.drops.noXmlMatch,
            out.drops.duplicateName, out.drops.setNotInstalled, out.drops.tiedProjects,
            out.drops.malformedSlider, out.drops.missingNameOrSet, out.drops.oversizeName,
            out.drops.setViaGroup, out.rejectedFiles);
        return out;
    }

    BodySlideCatalog::BodySlideCatalog() {
        snapshot_.store(std::make_shared<const BodySlideCatalogSnapshot>());
    }

    BodySlideCatalog& BodySlideCatalog::GetSingleton() {
        static BodySlideCatalog singleton;
        return singleton;
    }

    void BodySlideCatalog::RequestScan(std::vector<std::string> a_allowedPresetNames) {
        const auto requested = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        activeScans_.fetch_add(1, std::memory_order_acq_rel);
        std::thread([this, requested, names = std::move(a_allowedPresetNames)]() mutable {
            auto next = ScanBodySlideCatalog(
                "Data/CalienteTools/BodySlide", names);
            next.generation = requested;
            // Target switches can request a different sex while the previous
            // filesystem walk is still running. Only the newest request may
            // publish, so a late female scan cannot replace the male list.
            if (generation_.load(std::memory_order_acquire) == requested) {
                snapshot_.store(
                    std::make_shared<const BodySlideCatalogSnapshot>(std::move(next)),
                    std::memory_order_release);
            }
            activeScans_.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }

    std::shared_ptr<const BodySlideCatalogSnapshot> BodySlideCatalog::Snapshot() const {
        return snapshot_.load(std::memory_order_acquire);
    }

    bool BodySlideCatalog::Scanning() const {
        return activeScans_.load(std::memory_order_acquire) != 0;
    }

}  // namespace OS
