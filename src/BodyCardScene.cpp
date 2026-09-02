#include "BodyCardScene.h"

#include "BodyMorphData.h"
#include "BodyMorphSource.h"
#include "BodySlideCatalog.h"
#include "BodyWeight.h"
#include "CoveredSets.h"
#include "Mannequin.h"
#include "Settings.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OS::BodyCardScene {

    namespace {

        // Which .osp owns a slider set.
        //
        // ⚠ THE INSTALLED PRESETS ARE NOT THE WHOLE CATALOGUE. A custom preset
        // can name a set no installed preset uses, and that loop can only see
        // sets somebody installed a preset for. Eight sets crossed every card
        // on them that way (field 2026-08-10), so the scan's own
        // set-to-project map answers the rest.
        [[nodiscard]] std::string ProjectFor(const BodySlideCatalogSnapshot& a_catalog,
                                             const std::string&              a_set) {
            for (const auto& item : a_catalog.presets) {
                if (item.seed.sourceSet == a_set && !item.projectFile.empty()) {
                    return item.projectFile.string();
                }
            }
            if (const auto it = a_catalog.projectForSet.find(a_set);
                it != a_catalog.projectForSet.end()) {
                return it->second;
            }
            return {};
        }

        // The covered sibling of a_set when the option is on and the install
        // has one, else a_set unchanged. Cached per set, because a pane asks
        // for the same set once per card.
        [[nodiscard]] const std::string& CoveredFor(
            const BodySlideCatalogSnapshot& a_catalog, const std::string& a_set) {
            if (!Settings::GetSingleton().sfwBodyCards || a_set.empty()) {
                return a_set;
            }
            // ⚠ KEYED ON THE CATALOG'S GENERATION TOO. A rescan can install a
            // body mod that DOES ship a covered build, and a cache that
            // outlived the scan would keep answering "there is none".
            struct Entry {
                std::uint64_t generation{ 0 };
                std::string   covered;
            };
            static std::unordered_map<std::string, Entry> cache;
            auto&                                         slot = cache[a_set];
            if (slot.generation == a_catalog.generation && !slot.covered.empty()) {
                return slot.covered;
            }

            std::vector<CoveredSets::Candidate> candidates;
            candidates.reserve(a_catalog.outputForSet.size());
            for (const auto& [name, output] : a_catalog.outputForSet) {
                candidates.push_back(CoveredSets::Candidate{
                    name, FamilyFromSetSignature(name), output });
            }
            std::string_view output;
            if (const auto it = a_catalog.outputForSet.find(a_set);
                it != a_catalog.outputForSet.end()) {
                output = it->second;
            }
            const auto family = FamilyFromSetSignature(a_set);
            auto found = CoveredSets::CoveredSiblingFor(a_set, family, output,
                                                        candidates);
            // ⚠⚠ THE FAMILY'S OWN COVERED SET WHEN THE PRESET'S SET IS NOT
            // INSTALLED. 41 of this rig's 55 HIMBO preset entries name a
            // slider set no .osp declares, `HIMBO` alone accounting for 30, so
            // their cards come from the character's BUILT body and there is no
            // set to find a sibling of. Without this they can never be
            // covered, which is most of HIMBO.
            //
            // ⚠ ONLY WHEN THE DIRECT SEARCH FOUND NOTHING, so a set that HAS a
            // sibling always uses its own rather than the family's.
            bool byFamily = false;
            if (found.empty()) {
                found    = CoveredSets::BestCoveredForFamily(family, candidates);
                byFamily = !found.empty();
            }

            slot.generation = a_catalog.generation;
            slot.covered    = found.empty() ? a_set : std::string{ found };
            static std::unordered_set<std::string> told;
            if (told.insert(a_set).second) {
                if (found.empty()) {
                    spdlog::info("BodyCardScene: SFW cards are on and nothing "
                                 "installed carries a covered build for slider set "
                                 "'{}' or its family, so its cards draw unchanged. "
                                 "That is the normal answer for UBE, which ships "
                                 "none: measured over 3843 declared sets, the only "
                                 "UBE set with any shape beside the body declares "
                                 "physics helpers, not a garment.",
                                 a_set);
                } else {
                    spdlog::info("BodyCardScene: SFW cards are on; '{}' builds from "
                                 "'{}' instead{}.",
                                 a_set, slot.covered,
                                 byFamily ? " (its own set is not installed, so this "
                                            "is the family's covered build)"
                                          : "");
                }
            }
            return slot.covered;
        }

    }  // namespace

    Context MakeContext(const BodySlideCatalogSnapshot* a_catalog, RE::Actor* a_target) {
        Context ctx;
        ctx.catalog  = a_catalog;
        ctx.extras   = Mannequin::SubjectExtras();
        ctx.bodyPath = Mannequin::BodyPath();
        ctx.weight01 =
            BodyMorphData::SnapWeight(BodyWeight::Of(a_target) / 100.0f);
        ctx.ready = a_catalog && !a_catalog->presets.empty();
        return ctx;
    }

    Scene Build(const Context& a_ctx, std::string_view a_presetName,
                const std::vector<BodySliderValue>& a_sliders,
                const std::string&                  a_requestedSet) {
        Scene out;
        if (!a_ctx.catalog) {
            return out;
        }
        // ⚠⚠ THE SFW OPTION IS A DIFFERENT SLIDER SET, NOT A DIFFERENT PICTURE
        // OF THE SAME ONE. The body mod already ships a build with a bra and
        // pants modelled in, carrying its own authored morph runs, so the card
        // is BUILT from that set instead. Everything downstream is unchanged.
        //
        // ⚠ AND IT SILENTLY DOES NOTHING WHERE THE DATA IS ABSENT, which is
        // most of the grid on this rig: zero UBE sets carry a covering and UBE
        // is the largest family installed. Logged once per set so a field
        // report of "it did nothing" has an answer.
        const auto& a_setName = CoveredFor(*a_ctx.catalog, a_requestedSet);
        if (a_sliders.empty() || a_setName.empty()) {
            out.noCardKey = "$FR_Body_NoCardNoValues";
            // ⚠ ONCE PER PRESET AND NOT WHILE THE CATALOG IS WARMING UP. The
            // scan is async, so on the first frames every installed preset
            // looks missing; a once-per-preset latch made that permanent and
            // the line fired for 45 presets that were rendering perfectly.
            if (a_ctx.ready) {
                static std::unordered_set<std::string> told;
                if (told.insert(std::string{ a_presetName }).second) {
                    spdlog::warn("BodyCardScene: preset '{}' has {} and source set '{}', "
                                 "so it gets no card. An installed preset needs a catalog "
                                 "entry; a custom one carries its own set name.",
                                 a_presetName,
                                 a_sliders.empty() ? "no slider values" : "slider values",
                                 a_setName.empty() ? "(none)" : a_setName);
                }
            }
            return out;
        }

        const auto project = ProjectFor(*a_ctx.catalog, a_setName);
        if (project.empty()) {
            out.noCardKey = "$FR_Body_NoCardSetMissing";
            static std::unordered_set<std::string> told;
            if (told.insert(a_setName).second) {
                // ⚠ THIS MEANS THE SET IS NOT INSTALLED. The scan records every
                // set every .osp under SliderSets declares, so reaching here
                // means nothing on disk declares this one: the preset outlived
                // the body it was authored from.
                spdlog::warn("BodyCardScene: no BodySlide project declares slider set "
                             "'{}'. Its .osp is not installed; the presets are here and "
                             "the body they build from is not. Only SliderSets is "
                             "scanned, not RefTemplates.",
                             a_setName);
            }
        }
        // Cached per set after the first card, so this is a map lookup rather
        // than an XML parse per frame.
        const auto* src =
            project.empty() ? nullptr : BodyMorphSource::For(project, a_setName);

        // ⚠⚠ THE BUILT BODY ANSWERS WHEN THE PROJECT CANNOT, and that is why a
        // crossed card reshaped the character the moment it was clicked. The
        // game never reads the .osd: it applies morphs by slider name out of
        // the .tri beside the body being worn.
        const bool fromBuiltBody = !src && !a_ctx.bodyPath.empty() && a_ctx.ready;
        if (fromBuiltBody) {
            src = BodyMorphSource::ForRuntime(a_ctx.bodyPath);
        }
        if (!project.empty() && !src) {
            // The .osp is there and something in it or beside it is not.
            // BodyMorphSource has already named which, once, in the log.
            out.noCardKey = "$FR_Body_NoCardSetUnusable";
        }
        if (!src) {
            return out;
        }
        // Recoverable after all: the reason is set the moment the project
        // comes back empty, so leaving it would put a "not installed" tooltip
        // on a card that drew perfectly well.
        out.noCardKey = nullptr;

        auto shape         = std::make_shared<PreviewGrid::SceneIdentity::BodyShape>();
        shape->projectFile = project;
        shape->setName     = a_setName;
        // Empty unless the built body answered, which is what the build reads
        // to pick the same source it was given here.
        shape->runtimeMesh = fromBuiltBody ? a_ctx.bodyPath : std::string{};
        shape->weight01    = a_ctx.weight01;

        BodyMorphData::SliderValues values;
        shape->sliders.reserve(a_sliders.size());
        for (const auto& sl : a_sliders) {
            // ⚠ THE FILE STORES 0..100 AND THE MORPH WANTS 0..1, and a slider
            // is a PAIR rather than a number: the preset holds the shape at
            // weight 0 and at weight 100 and the character sits between them.
            const float v =
                BodyMorphData::ValueAtWeight(sl.smallValue, sl.bigValue, a_ctx.weight01);
            shape->sliders.emplace_back(sl.name, v);
            values.emplace(sl.name, v);
        }

        // ⚠⚠ THE SET IS IN THE KEY, and it is what carries the SFW option into
        // the disk cache. Flipping the option changes nothing else about the
        // card: same preset, same slider values, same weight, same model
        // paths. Keying the RESOLVED set is also exactly right when the option
        // is on and the install has no covered build, because then the set
        // really is unchanged and so is the picture.
        out.id.editorId = BodyMorphData::CardIdentity(a_presetName, values,
                                                      a_ctx.weight01, a_setName);
        out.id.modelPaths = { src->referenceMesh };
        // ⚠ THE REFERENCE MESH STAYS FIRST AND THE EXTRAS GO AFTER IT, because
        // both counts below run from the front.
        out.id.modelPaths.insert(out.id.modelPaths.end(), a_ctx.extras.begin(),
                                 a_ctx.extras.end());
        // ⚠⚠ THE MORPH REACHES THE BODY ONLY. A .osd or .tri is indexed by the
        // body shape's vertex array and a hand NIF has its own, so the same
        // displacement would land on unrelated vertices and shred it.
        out.id.morphPathCount = 1;
        // ⚠⚠ ONLY A BodySlide REFERENCE IS Data-ROOTED. A built body is an
        // ordinary form model path under meshes\, like the parts beside it,
        // and each needs its own loader: the wrong one returns null and drops
        // the subject in silence.
        out.id.dataRootedPathCount = fromBuiltBody ? 0 : 1;
        out.id.kind                = PreviewGrid::SceneKind::kBody;
        out.id.bodyShape           = std::move(shape);
        out.ok                     = true;
        return out;
    }

}  // namespace OS::BodyCardScene
