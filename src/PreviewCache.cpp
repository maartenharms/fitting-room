#include "PreviewCache.h"
#include "GpuAccess.h"

#include "DyeTexture.h"  // EnsurePresent, so the frame clock runs dye or no dye
#include "FuckCompat.h"
#include "BodyMorphSource.h"
#include "Mannequin.h"
#include "MeshExtractor.h"
#include "NifModelLoader.h"
#include "PreviewDiskCache.h"
#include "PreviewFrame.h"
#include "PreviewPng.h"
#include "PreviewRenderer.h"
#include "Settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace OS::PreviewCache {

    namespace {

        // 48 ready handles at most: at 256 squared that is a modest texture
        // set, and stalls rather than memory are the reason to be careful.
        constexpr std::size_t kLruCap = 48;

        struct Entry {
            PreviewGrid::SceneIdentity id;
            FUCK::Image                image;  // move-only, owns the FUCK handle
            PreviewGrid::EntryMeta     meta;
        };

        struct Stats {
            std::uint64_t built{ 0 };
            std::uint64_t loaded{ 0 };
            std::uint64_t failed{ 0 };
            std::uint64_t pruned{ 0 };
            std::uint64_t evicted{ 0 };
        };

        struct Harness {
            std::uint64_t       drains{ 0 };
            std::uint64_t       over33{ 0 };
            std::uint64_t       over50{ 0 };
            double              maxMs{ 0.0 };
            std::vector<double> ring;  // for the p95, capped
        };

        // Deliberately leaked at exit, IconImages' reason: every entry's
        // destructor calls back into FUCK, and process exit is after the
        // host may be gone.
        struct Store {
            std::unordered_map<std::string, Entry> entries;  // by disk key
            std::vector<FUCK::Image>               pendingRelease;
            Stats                                  stats;
            Harness                                harness;
        };
        Store* g_store = nullptr;

        Store& Get() {
            if (!g_store) {
                g_store = new Store{};
            }
            return *g_store;
        }

        double MsSince(std::chrono::steady_clock::time_point a_t0) {
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - a_t0)
                .count();
        }

        // The one cold build: load, extract, render, encode, write, load
        // back through FUCK. Any stage that refuses marks the key failed on
        // disk so no later session pays for it again.
        void BuildOne(Store& a_s, const std::string& a_key, Entry& a_entry) {
            const auto t0 = std::chrono::steady_clock::now();

            auto* device = OS::Gpu::Device();
            auto* ctx    = OS::Gpu::Context();
            if (!device || !ctx) {
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                return;
            }

            // One card, N NIFs: an armour scene is every addon that resolved
            // for the target's race and sex, merged. The partial-load rule:
            // a scene renders what loaded (an addon whose NIF is missing
            // costs its piece, not the card); only zero loaded NIFs is a
            // model failure. Extract APPENDS by contract, so one mesh vector
            // and one stats block accumulate across the roots.
            std::vector<MeshExtractor::RenderMesh> meshes;
            MeshExtractor::ExtractStats            xstats;
            std::size_t                            loadedNifs = 0;
            // ⚠ ONCE PER SCENE, never per root and never per geometry. The
            // snapshot copies the whole rule set, and the candidate loop
            // inside Extract runs to several hundred on a merged armour scene.
            const auto scopes = PreviewScopes::Snapshot();
            // One pass over every root. Run a second time with a_keepEffect
            // true only when the first came back with nothing (below), which
            // is the whole safety argument for the retry: it cannot reach a
            // card that renders. loadedNifs is counted on the first pass only,
            // so a retry cannot inflate it.
            // Where the mannequin's own head bone sits, filled by the leading
            // mannequin roots and read by the item roots after them. ⚠ THE
            // ORDER IS WHAT MAKES ONE PASS ENOUGH: Compose PREPENDS the
            // mannequin, so every mannequin path is extracted before any item
            // path and the reference is known by the time a hair needs it.
            MeshExtractor::NodeAnchor headAnchor;
            const bool                aligns = PreviewGrid::IsAlignedKind(a_entry.id.kind);

            // A body card's SHAPE, rebuilt here because the card is built long
            // after it was requested. ⚠ The reference mesh is morphed, never
            // the built one: the built file already has whatever preset was
            // last built baked in, so deltas on top of it stack two shapes.
            // ⚠ ONE FIELD PER SHAPE THE SET BUILDS, not one for the root. A
            // .osd run is indexed by its own shape's vertex array, so the
            // fields cannot be folded together and the extractor picks per
            // geometry. `fields` owns the storage; `morphs` is the view handed
            // down, and the two must not diverge.
            std::vector<std::vector<std::array<float, 3>>> fields;
            std::vector<MeshExtractor::ShapeMorph>         morphs;
            // A body subject with a shape to build: a body card. A skin card is
            // a body subject too and carries no shape, so it takes no morph and
            // writes no body-morph line (OS-212).
            const bool bodyScene = PreviewGrid::IsBodySubjectKind(a_entry.id.kind) &&
                                   a_entry.id.bodyShape != nullptr;
            if (bodyScene) {
                const auto* shape = a_entry.id.bodyShape.get();
                // ⚠ THE SAME TWO SOURCES THE EDITOR CHOSE BETWEEN, and asking
                // the wrong one costs the whole field rather than failing
                // loudly: a project that was never there cannot be reopened,
                // so the morph came back empty and the card drew its base body
                // unmorphed.
                const auto* src =
                    !shape ? nullptr
                    : shape->projectFile.empty()
                        ? BodyMorphSource::ForRuntime(shape->runtimeMesh)
                        : BodyMorphSource::For(shape->projectFile, shape->setName);
                // Counted for the log below, not used by the arithmetic:
                // `named` is how many of the .osp's sliders the preset actually
                // names, and `driven` how many end up contributing a non-zero
                // weight to a delta set that exists.
                std::size_t named = 0, driven = 0, span = 0;
                if (src) {
                    BodyMorphData::SliderValues values;
                    values.reserve(shape->sliders.size());
                    for (const auto& [name, value] : shape->sliders) {
                        values.emplace(name, value);
                    }
                    for (const auto& shapeRules : src->shapes) {
                        for (const auto& [name, rule] : shapeRules.rules) {
                            const auto hit   = values.find(name);
                            const bool isSet = hit != values.end();
                            if (isSet) {
                                ++named;
                            }
                            if (!BodyMorphData::SliderMovesVertices(rule) ||
                                src->deltas.find(rule.dataName) == src->deltas.end()) {
                                continue;
                            }
                            if (BodyMorphData::SliderWeight(rule, isSet,
                                                            isSet ? hit->second : 0.0f,
                                                            shape->weight01) != 0.0f) {
                                ++driven;
                            }
                        }
                    }
                    // Sized by what the deltas actually address rather than by
                    // the mesh: the extractor bounds every lookup anyway, and
                    // the real vertex count is not known until the NIF loads.
                    //
                    // ⚠ ONE SPAN FOR ALL THE SHAPES, and that is safe in the
                    // direction that matters. It is the largest index any run
                    // in the file addresses, so a small shape's field is
                    // longer than its own vertex array rather than shorter,
                    // and the extractor's own bound is what rejects the tail.
                    // Sizing each shape to its own runs would be tighter and
                    // would buy nothing, because a shape's run cannot address
                    // past its own array in the first place.
                    for (const auto& [ignored, set] : src->deltas) {
                        for (const auto index : set.indices) {
                            span = (std::max)(span, static_cast<std::size_t>(index) + 1);
                        }
                    }
                    // ⚠⚠ RESERVED BEFORE THE LOOP AND NEVER GROWN INSIDE IT.
                    // `morphs` holds spans INTO `fields`, so a reallocation
                    // would leave every span already pushed pointing at freed
                    // storage. The whole per-geometry morph would then read
                    // rubbish with a perfectly clean log.
                    fields.reserve(src->shapes.size());
                    morphs.reserve(src->shapes.size());
                    for (const auto& shapeRules : src->shapes) {
                        fields.push_back(BodyMorphData::Accumulate(
                            src->deltas, shapeRules.rules, values, span,
                            shape->weight01));
                        morphs.push_back(MeshExtractor::ShapeMorph{
                            shapeRules.shape, fields.back() });
                    }
                }
                // ⚠ ONE LINE THAT SEPARATES EVERY CAUSE OF "every body looks
                // the same", because guessing between them costs a field round
                // each time. shape=0 means the slider values did not survive
                // into the cache; src=0 means the set stopped resolving between
                // the card being drawn and being built; driven=0 beside a real
                // rules count means the preset's slider NAMES do not match the
                // .osp's; nonzero=0 beside driven>0 means the values are zero;
                // and a real max with the bodies still identical means the
                // field is not reaching the vertices.
                // ⚠ COUNTED OVER EVERY SHAPE'S FIELD, AND PER SHAPE IN THE
                // SECOND LINE. A single total would go on reading "nonzero"
                // while the shape that matters got nothing, which is the case
                // this whole change exists to make visible.
                std::size_t nonzero  = 0;
                std::size_t fieldLen = 0;
                float       maxMag   = 0.0f;
                std::string perShape;
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    std::size_t shapeNonzero = 0;
                    for (const auto& d : fields[i]) {
                        const float m =
                            std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]);
                        if (m > 0.0f) {
                            ++shapeNonzero;
                        }
                        maxMag = (std::max)(maxMag, m);
                    }
                    nonzero += shapeNonzero;
                    fieldLen += fields[i].size();
                    if (!perShape.empty()) {
                        perShape += ' ';
                    }
                    perShape += std::string{ morphs[i].shape } + '=' +
                                std::to_string(shapeNonzero);
                }
                spdlog::info("PreviewCache: body-morph '{}' shape={} src={} "
                             "sliders={} rules={} sets={} named={} driven={} "
                             "span={} field={} nonzero={} max={:.3f} weight={:.3f} "
                             "shapes=[{}]",
                             a_key, shape ? 1 : 0, src ? 1 : 0,
                             shape ? shape->sliders.size() : 0,
                             src ? src->RuleCount() : 0,
                             src ? src->deltas.size() : 0, named, driven, span,
                             fieldLen, nonzero, maxMag,
                             shape ? shape->weight01 : 1.0f, perShape);
            }
            const auto runPass = [&](bool a_keepEffect,
                                     MeshExtractor::ExtractStats& a_stats,
                                     bool a_countLoads, bool a_keepBlend = false) {
            for (std::size_t i = 0; i < a_entry.id.modelPaths.size(); ++i) {
                const auto& path = a_entry.id.modelPaths[i];
                // ⚠ A BODY'S REFERENCE MESH IS ROOTED AT DATA, not at
                // meshes\, so it needs the other entry point. Everything else
                // is a form's model path and keeps the one it always used.
                //
                // ⚠⚠ WHICH IS PER PATH, NOT PER SCENE. A body card also
                // carries the head, hands and feet, and those ARE form model
                // paths: sending them through the Data-relative entry point
                // asks for Data\meshes\... and comes back null, so the card
                // would quietly lose exactly the parts this composes. The
                // BodySlide reference mesh is the leading path, the same one
                // the morph applies to.
                const auto  dataRooted = i < a_entry.id.dataRootedPathCount;
                const auto  root = dataRooted ? NifModelLoader::LoadDataRelative(path)
                                              : NifModelLoader::Load(path);
                if (!root) {
                    if (a_countLoads) {
                        spdlog::debug("PreviewCache: preview.nif-miss '{}' in '{}'", path,
                                      a_key);
                    }
                    continue;
                }
                if (a_countLoads) {
                    ++loadedNifs;
                }
                // This root's texture swaps, when the scene carries any
                // (OS-192): aligned with modelPaths by the capture side.
                const auto* swaps =
                    i < a_entry.id.swaps.size() && !a_entry.id.swaps[i].empty()
                        ? &a_entry.id.swaps[i]
                        : nullptr;
                // The per-root verdict is deliberately unused: an empty root
                // is priced by the meshes.empty() check over the whole scene.
                // The leading roots are the mannequin (OS-204). They resolve
                // no diffuse, which is what makes them grey: the shader's
                // no-texture branch is a flat warm grey with the studio rig
                // already on it, so nothing in the renderer changes and no
                // material is touched.
                const bool mannequin = i < a_entry.id.mannequinPathCount;
                // ⚠⚠ THE FIGURE'S OWN HEAD IS AN ALIGNMENT SUBJECT ON EVERY
                // KIND. See RootConsultsHeadAnchor: vanilla's
                // MaleHeadArgonian.nif carries no shape transform, so without
                // this the Argonian male's head renders at the origin and lies
                // on the floor by his feet, on hair cards AND armour cards
                // alike. `aligns` alone cannot express this: it is the ITEM's
                // rule, and gating the figure's head on it would have fixed
                // the hair cards and left every armour card broken.
                const bool mannHead = mannequin && i == a_entry.id.mannequinHeadIndex;
                const bool consults =
                    PreviewGrid::RootConsultsHeadAnchor(mannequin, mannHead,
                                                        a_entry.id.kind);
                const bool publishes =
                    PreviewGrid::RootPublishesHeadAnchor(mannequin, mannHead,
                                                         a_entry.id.kind);
                // The mannequin reports its head bone; the item aligns to it.
                // Both halves are hair-only and both are null otherwise, so
                // every other scene walks exactly the tree it always did.
                static_cast<void>(MeshExtractor::Extract(
                    device, ctx, root.get(), meshes, a_stats, a_entry.id.slotMask,
                    PreviewGrid::IsHeadPartKind(a_entry.id.kind), swaps, path,
                    &scopes, mannequin, mannequin && i == a_entry.id.mannequinHeadIndex,
                    mannequin && i == a_entry.id.mannequinFeetIndex,
                    consults ? &headAnchor : nullptr,
                    publishes ? &headAnchor : nullptr, a_keepEffect,
                    PreviewGrid::IsBodySubjectKind(a_entry.id.kind),
                    // ⚠ THE MORPH IS THE LAST PARAMETER AND IT IS DEFAULTED,
                    // so leaving it off compiled green, ran the whole .osp
                    // parse and 11 MB .osd read per set, accumulated the field
                    // and then threw it away: every body card rendered the raw
                    // reference mesh, which is exactly "every body looks the
                    // same". A defaulted trailing argument is the third silent
                    // no-op on this branch. Never add one to this call.
                    //
                    // ⚠⚠ AND ONLY THE PATHS THE FIELD IS INDEXED FOR. A body
                    // card also carries the head, hands and feet its reference
                    // mesh lacks, and those have their own vertex arrays:
                    // handing them a field indexed by the BODY's shape lands
                    // every displacement on an unrelated vertex. The same
                    // failure the partition map produced, one root further on.
                    //
                    // ⚠ THE LIST GOES ONLY WHERE THE FIELDS DO. It DROPS every
                    // geometry it does not name, so handing it to the head,
                    // hands and feet roots would delete them: they are their
                    // own roots and their shapes are named nothing like the
                    // body's.
                    i < a_entry.id.morphPathCount
                        ? std::span<const MeshExtractor::ShapeMorph>{ morphs }
                        : std::span<const MeshExtractor::ShapeMorph>{},
                    // A skin card is a body subject that keeps its diffuse
                    // (OS-212); a body card is the grey figure it always was.
                    PreviewGrid::IsBodySubjectKind(a_entry.id.kind) &&
                        !PreviewGrid::DrawsGreyBody(a_entry.id.kind),
                    a_keepBlend));
            }
            };
            runPass(false, xstats, true);

            // Whether the mannequin offered a head bone at all. Every aligned
            // branch downstream sits behind valid, so when this line says
            // false the whole alignment feature was inert for the scene and
            // zero head-align/head-local lines is the expected reading, not
            // a missing one.
            //
            // ⚠ AND IT IS NO LONGER GATED ON `aligns`, because the mannequin's
            // own head now consults the anchor on every kind. Leaving the gate
            // here would have made the armour-card half of the 2026-08-20 fix
            // invisible in the field: the branch would run and report nothing,
            // which is the same silence as not running at all.
            if (aligns || a_entry.id.mannequinHeadIndex !=
                              static_cast<std::size_t>(-1)) {
                spdlog::info("PreviewCache: preview.head-anchor '{}' valid={} "
                             "({:.3f}, {:.3f}, {:.3f})",
                             a_key, headAnchor.valid, headAnchor.x, headAnchor.y,
                             headAnchor.z);
            }

            // ⚠ THE LAST RESORT, AND IT ONLY RUNS ON A CARD ALREADY LOST.
            // Some items are authored entirely through the effect shader:
            // vanilla's spectral draugr weapons are one BSTriShape with a
            // BSEffectShaderProperty and no lighting property in the file, so
            // the FX skip that is right for a glow plane ate the whole item
            // and the card failed(geometry) with a perfectly correct key.
            // Four vanilla weapons are in that state right now.
            //
            // ⚠ A SEPARATE STATS BLOCK, so the failure line's fx= count stays
            // honest about what the FIRST pass skipped. The retry's own
            // effectKept is what says a card is FX and nothing but.
            // ⚠⚠ THE RESCUE IS TIERED, AND THE ORDER IS THE POINT. The three
            // skips are not equally trustworthy. An effect shader and an FX
            // NAME are strong signals; an unreproducible blend is the weakest,
            // because whether REAL geometry carries one is entirely the
            // author's choice. Vigilant's Umaril arrow settled it: all eight of
            // its arrow shapes are 0x100D, blend on, srcAlpha to ONE, test off,
            // indistinguishable by blend from a glow plane. The old
            // all-or-nothing rescue put every skipped shape back and the card
            // returned with the slab on it (field 2026-08-21).
            //
            // So rung one puts back ONLY the blend-skipped geometry and keeps
            // dropping effect shaders and FX names, which is exactly the right
            // card for that arrow. Rung two is the original last resort for an
            // item that really is nothing but FX.
            if (meshes.empty() && xstats.blendSkipped != 0) {
                MeshExtractor::ExtractStats rescue;
                runPass(false, rescue, false, true);
                if (!meshes.empty()) {
                    spdlog::info("PreviewCache: preview.fx-rescue '{}' kept {} "
                                 "blend-skipped geometry; the scene had nothing else, "
                                 "and its effect shaders and FX names stay dropped.",
                                 a_key, meshes.size());
                }
            }
            if (PreviewFilter::ShouldRetryKeepingEffects(meshes.size(),
                                                         xstats.effectSkipped,
                                                         xstats.blendSkipped,
                                                         xstats.fxNameSkipped)) {
                MeshExtractor::ExtractStats rescue;
                runPass(true, rescue, false);
                if (!meshes.empty()) {
                    xstats.effectKept = rescue.effectKept;
                    spdlog::info("PreviewCache: preview.fx-rescue '{}' kept {} "
                                 "effect-shader geometry; the scene had nothing else.",
                                 a_key, rescue.effectKept);
                }
            }
            if (loadedNifs == 0) {
                PreviewDiskCache::MarkFailed(a_key, "model load failed");
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::info("PreviewCache: processed '{}' status=failed(model) "
                             "build={:.2f}ms",
                             a_key, MsSince(t0));
                return;
            }
            if (meshes.empty()) {
                PreviewDiskCache::MarkFailed(a_key, "no renderable geometry");
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::info(
                    "PreviewCache: processed '{}' status=failed(geometry) "
                    "build={:.2f}ms geoms={} skin={} culled={} faulted={} fx={} "
                    "fxblend={} fxname={} filtered={}/{}",
                    a_key, MsSince(t0), xstats.geometries, xstats.skinPath,
                    xstats.nodeCulled, xstats.faulted, xstats.effectSkipped,
                    xstats.blendSkipped, xstats.fxNameSkipped, xstats.nameFiltered,
                    xstats.faceFiltered);
                return;
            }
            // A swap that promised a diffuse and got none is environmental
            // (the texture machinery, not the item), so it does NOT persist;
            // the card crosses for the session and the next session tries
            // again. ⚠ Never cache a variant card wearing the wrong look:
            // that frozen lie is the exact bug this feature removes.
            if (xstats.swapNoDiffuse != 0) {
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::info(
                    "PreviewCache: processed '{}' status=failed(swap-texture) "
                    "build={:.2f}ms swaps={} missing={}",
                    a_key, MsSince(t0), xstats.swapApplied, xstats.swapNoDiffuse);
                return;
            }
            const double buildMs = MsSince(t0);

            const auto size = Settings::GetSingleton().previewThumbPx;
            std::vector<std::uint8_t> rgba;
            PreviewRenderer::Timings  times;
            // The pose picker: eyes get the straight-on iris chip, worn gear
            // carries a slot bit, a head part carries its kind, weapons and
            // ammo carry neither. The upright rule is pure and pinned
            // (PreviewGrid::StandsUpright). A LONE shield is the one scene
            // that reaches the diagonal WITH a slot bit, and it takes the
            // mirrored side: the plain diagonal photographed its straps
            // (field 2026-08-09).
            const auto kind = a_entry.id.kind;
            PreviewFraming::Pose pose;
            if (kind == PreviewGrid::SceneKind::kEyes) {
                pose = PreviewFraming::Pose::kEyes;
            } else if (kind == PreviewGrid::SceneKind::kBody ||
                       kind == PreviewGrid::SceneKind::kSkin) {
                // ⚠ A BODY CARRIES NO SLOT BITS, so it would fall all the way
                // through to the weapons' diagonal and be photographed lying
                // across the card. It stands up for the same reason worn gear
                // does: its bind pose already carries the one right
                // orientation.
                pose = PreviewFraming::Pose::kUpright;
            } else if (PreviewGrid::StandsUpright(a_entry.id.slotMask,
                                                  PreviewGrid::IsHeadPartKind(kind))) {
                pose = PreviewFraming::Pose::kUpright;
            } else if ((a_entry.id.slotMask & PreviewFilter::kShieldSlotBit) != 0) {
                pose = PreviewFraming::Pose::kShield;
            } else {
                pose = PreviewFraming::Pose::kDiagonal;
            }
            // Brows are near-black strands on a near-black studio; the
            // lifted backdrop is what makes their cards readable (field
            // 2026-08-09 round 3). ⚠ FACIAL HAIR TAKES THE SAME LIFT and it
            // is NOT caught by /we4062: this is a filter, not a switch, so
            // the enum widening said nothing about it. A beard is the same
            // dark strands on the same dark studio.
            const float bgLift = (kind == PreviewGrid::SceneKind::kBrows ||
                                  kind == PreviewGrid::SceneKind::kFacialHair)
                                     ? 0.30f
                                     : 0.0f;
            // The crop, once a body is actually under the item (OS-204). ⚠ IT
            // ASKS mannequinPathCount, NOT THE RULE: a scene whose mannequin
            // resolved to nothing keys byte-identically to the card already on
            // disk, so it must frame identically too or the cached picture and
            // the live framing disagree with nothing to tell them apart.
            const bool figure = a_entry.id.mannequinPathCount != 0 &&
                                PreviewGrid::MannequinFor(kind, a_entry.id.slotMask) ==
                                    PreviewGrid::MannequinKind::kFigure;
            // An eye card takes ONE side of its band, which is one eye in a
            // face rather than a pair staring out of one (the pair read as
            // horror, field 2026-08-09, and that verdict outlives the head
            // arriving). Hair and beards band on the head part they sit on.
            const auto headScene =
                kind == PreviewGrid::SceneKind::kEyes
                    ? PreviewFraming::HeadScene::kEyes
                    : kind == PreviewGrid::SceneKind::kFacialHair
                          ? PreviewFraming::HeadScene::kFacialHair
                          : kind == PreviewGrid::SceneKind::kHair
                                ? PreviewFraming::HeadScene::kHair
                                : PreviewFraming::HeadScene::kNone;
            const auto crop =
                PreviewFraming::CropFor(figure, a_entry.id.slotMask, headScene);
            if (!PreviewRenderer::RenderThumbnail(device, ctx, meshes, size, pose,
                                                  bgLift, crop, rgba, times)) {
                // A renderer refusal is environmental (a compile latch, a
                // device hiccup), not a fact about the item, so it does NOT
                // persist; the next session tries again.
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::info("PreviewCache: processed '{}' status=failed(render) "
                             "build={:.2f}ms",
                             a_key, buildMs);
                return;
            }

            // ⚠ THE FILE IS WRITTEN EVEN WITH bPreviewDiskCache OFF, because
            // the file IS the display path: FUCK loads images from disk and
            // nothing else (Task 0). Off means the store is wiped at editor
            // close instead of kept, see OnEditorClose.
            const auto   encodeStart = std::chrono::steady_clock::now();
            const auto   file        = PreviewDiskCache::PathFor(a_key);
            const bool   wrote       = PreviewPng::WriteRgba(file, rgba, size, size);
            const double encodeMs = MsSince(encodeStart);
            if (!wrote) {
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::info("PreviewCache: processed '{}' status=failed(encode) "
                             "build={:.2f}ms encode={:.2f}ms",
                             a_key, buildMs, encodeMs);
                return;
            }
            // ⚠⚠ THE LOAD-BACK NOW DECIDES WHETHER THE MANIFEST HEARS ABOUT
            // THIS FILE, and the order matters since PreviewPng stopped writing
            // through a temp file (see its header: the rename crashed inside
            // MO2's usvfs hook). A process killed mid-write leaves a truncated
            // PNG, and marking ready BEFORE the read meant the manifest would
            // vouch for it forever: every later session would load a file it
            // could not decode and fail the card again, with nothing ever
            // rebuilding it. Marking after the decode makes that case cost one
            // rebuild instead of being permanent.
            const auto  loadStart = std::chrono::steady_clock::now();
            FUCK::Image image(file.string().c_str());
            const double loadMs = MsSince(loadStart);
            if (!image.IsLoaded()) {
                // ⚠ THE FILE GOES AND THE MANIFEST HEARS NOTHING. MarkFailed
                // would be wrong here: a failed entry is PERSISTED and Lookup
                // hands it straight back, so a card that failed to decode once
                // would draw the cross for the rest of the install. Leaving no
                // entry at all is what makes the next session try again, and
                // the drain's own hit path already treats "manifest says ready,
                // disk will not decode" as a cold rebuild.
                std::error_code rmEc;
                std::filesystem::remove(file, rmEc);
                a_entry.meta.state = PreviewGrid::State::kFailed;
                ++a_s.stats.failed;
                spdlog::warn("PreviewCache: '{}' rendered and wrote but FUCK would "
                             "not load it back; the file is deleted and the card "
                             "rebuilds rather than being served broken.",
                             a_key);
                return;
            }
            PreviewDiskCache::MarkReady(a_key, size, size);
            a_entry.image           = std::move(image);
            a_entry.meta.state      = PreviewGrid::State::kReady;
            a_entry.meta.readyFrame = PreviewFrame::Current();
            ++a_s.stats.built;
            spdlog::info(
                "PreviewCache: processed '{}' status=ready build={:.2f}ms "
                "render={:.2f}ms readback={:.2f}ms encode={:.2f}ms load={:.2f}ms "
                "meshes={} skin={} diffuse={}/{} fx={}/{} fxblend={} fxname={} filtered={}/{} scoped={} "
                "swaps={} bind={}/{}/{} legacy={}/{} blend={}b/{}bpv/{}bones",
                a_key, buildMs, times.renderMs, times.readbackMs, encodeMs, loadMs,
                meshes.size(), xstats.skinPath, xstats.diffuseResolved,
                xstats.diffuseResolved + xstats.diffuseMissing, xstats.effectSkipped,
                xstats.effectKept, xstats.blendSkipped, xstats.fxNameSkipped,
                xstats.nameFiltered, xstats.faceFiltered,
                xstats.scopeFiltered,
                xstats.swapApplied, xstats.bindCorrected, xstats.bindPosed,
                xstats.bindDeferred, xstats.legacyPath, xstats.legacyGeometry,
                xstats.skinSpan,
                xstats.bonesPerVertex,
                xstats.skinBones);
        }

    }  // namespace

    std::string Request(const PreviewGrid::SceneIdentity& a_id, std::uint32_t a_orderHint) {
        auto& s = Get();
        // ⚠ THE SCOPE TAGS ARE STAMPED HERE AND THE KEY IS RETURNED, so this
        // is the ONLY place either is computed. The caller used to key the
        // same identity again on the very next line; two computations of a
        // key that must match is a card that never resolves the day they stop
        // matching.
        auto stamped = a_id;
        // ⚠ THE MANNEQUIN GOES ON BEFORE THE TAGS AND BEFORE THE KEY, in that
        // order, because both read modelPaths: tagging first would leave the
        // tags one path short and misaligned against every entry the compose
        // prepended, and keying first would key the scene without the body it
        // is about to draw.
        Mannequin::Compose(stamped);
        stamped.scopeTags = PreviewScopes::TagsFor(PreviewScopes::Snapshot(),
                                                   stamped.modelPaths);
        const auto key = PreviewGrid::DiskKeyFor(stamped);
        const auto now = PreviewFrame::Current();
        const auto it  = s.entries.find(key);
        if (it == s.entries.end()) {
            // First sight also arms the Present hook, so the frame clock
            // runs even in a session where no dye ever installed it.
            OS::DyeTexture::EnsurePresent();
            Entry e;
            e.id                = stamped;
            e.meta.requestFrame = now;
            e.meta.lastUsed     = now;
            e.meta.orderHint    = a_orderHint;
            e.meta.state        = PreviewGrid::State::kQueued;
            s.entries.emplace(key, std::move(e));
            return key;
        }
        it->second.meta.lastUsed  = now;
        it->second.meta.orderHint = a_orderHint;  // the screen may have scrolled
        if (it->second.meta.state == PreviewGrid::State::kQueued) {
            it->second.meta.requestFrame = now;  // still wanted this present
        }
        return key;
    }

    ImTextureID Texture(const std::string& a_diskKey) {
        auto&      s  = Get();
        const auto it = s.entries.find(a_diskKey);
        if (it == s.entries.end() ||
            it->second.meta.state != PreviewGrid::State::kReady ||
            !it->second.image.IsLoaded()) {
            return static_cast<ImTextureID>(0);
        }
        it->second.meta.lastUsed = PreviewFrame::Current();
        return it->second.image.GetID();
    }

    bool Failed(const std::string& a_diskKey) {
        auto&      s  = Get();
        const auto it = s.entries.find(a_diskKey);
        return it != s.entries.end() &&
               it->second.meta.state == PreviewGrid::State::kFailed;
    }

    float RevealAlpha(const std::string& a_diskKey) {
        auto&      s  = Get();
        const auto it = s.entries.find(a_diskKey);
        if (it == s.entries.end()) {
            return 1.0f;
        }
        return PreviewGrid::RevealAlpha(it->second.meta.readyFrame,
                                        PreviewFrame::Current());
    }

    void Drain() {
        auto&      s  = Get();
        const auto t0 = std::chrono::steady_clock::now();
        const auto now = PreviewFrame::Current();

        // 1. Prune: queued entries no card requested THIS present belong to
        // a page nobody is looking at, and dropping them is what makes
        // paging free. Snapshot the metas in key order for the pure helpers.
        std::vector<std::string>            keys;
        std::vector<PreviewGrid::EntryMeta> metas;
        keys.reserve(s.entries.size());
        metas.reserve(s.entries.size());
        for (const auto& [key, e] : s.entries) {
            keys.push_back(key);
            metas.push_back(e.meta);
        }
        const auto stale = PreviewGrid::PruneSelection(metas, now);
        if (!stale.empty()) {
            for (const auto idx : stale) {
                s.entries.erase(keys[idx]);
            }
            s.stats.pruned += stale.size();
            spdlog::debug("PreviewCache: preview.queue-prune {} stale request(s).",
                          stale.size());
            keys.clear();
            metas.clear();
            for (const auto& [key, e] : s.entries) {
                keys.push_back(key);
                metas.push_back(e.meta);
            }
        }

        // 2. At most one entry: a disk hit loads, a disk failure marks, a
        // miss builds cold. Whichever it is, it is the frame's whole budget.
        if (const auto next = PreviewGrid::NextToBuild(metas)) {
            const auto& key   = keys[*next];
            auto&       entry = s.entries.at(key);
            const auto  hit   = PreviewDiskCache::Lookup(key);
            if (hit.status == PreviewManifest::Status::kFailed) {
                entry.meta.state = PreviewGrid::State::kFailed;
                ++s.stats.failed;
            } else if (hit.status == PreviewManifest::Status::kReady) {
                const auto  loadStart = std::chrono::steady_clock::now();
                FUCK::Image image(hit.file.string().c_str());
                if (image.IsLoaded()) {
                    entry.image           = std::move(image);
                    entry.meta.state      = PreviewGrid::State::kReady;
                    entry.meta.readyFrame = PreviewFrame::Current();
                    ++s.stats.loaded;
                    spdlog::debug("PreviewCache: disk-cache.upload '{}' load={:.2f}ms",
                                  key, MsSince(loadStart));
                } else {
                    // The manifest says ready and the disk disagrees: the
                    // disk wins and the card rebuilds cold.
                    BuildOne(s, key, entry);
                }
            } else {
                BuildOne(s, key, entry);
            }
        } else {
            // Queue idle: the batched manifest write happens here, never per
            // thumbnail.
            PreviewDiskCache::FlushIfDirty();
        }

        // 3. LRU past the cap moves the handle to the pending list; the
        // release itself is next frame's first act.
        for (;;) {
            metas.clear();
            keys.clear();
            for (const auto& [key, e] : s.entries) {
                keys.push_back(key);
                metas.push_back(e.meta);
            }
            const auto victim = PreviewGrid::LruVictim(metas, kLruCap);
            if (!victim) {
                break;
            }
            auto& e = s.entries.at(keys[*victim]);
            s.pendingRelease.push_back(std::move(e.image));
            s.entries.erase(keys[*victim]);
            ++s.stats.evicted;
        }

        const double ms = MsSince(t0);
        ++s.harness.drains;
        if (ms > 33.0) {
            ++s.harness.over33;
        }
        if (ms > 50.0) {
            ++s.harness.over50;
        }
        s.harness.maxMs = (std::max)(s.harness.maxMs, ms);
        if (s.harness.ring.size() < 4096) {
            s.harness.ring.push_back(ms);
        }
    }

    void ReleasePending() {
        auto& s = Get();
        // Each FUCK::Image destructor calls ReleaseImage now, one full frame
        // after eviction, so a handle can never leave a submit mid-flight.
        s.pendingRelease.clear();
    }

    void ForgetAll() {
        auto& s = Get();
        // ⚠⚠ THE DISK WIPE WAS ONLY HALF THE BUTTON, AND THE MISSING HALF IS
        // THE HALF THE PLAYER CAN SEE. PreviewDiskCache::RebuildAll deletes
        // every PNG and empties the manifest, and its log line promises
        // "thumbnails render again as they are browsed" - but the cards
        // already on screen are held HERE, as decoded FUCK images, and nothing
        // was dropping them. So the grid went on drawing the exact pictures the
        // player pressed the button to be rid of, and the only thing that ever
        // cleared them was quitting the game.
        //
        // Measured 2026-08-15: after a rebuild the disk cache really was empty
        // (0 files) while the drain still reported built=0 loaded=51 and the
        // browser still showed the old cards. Reported as "rebuilt previews,
        // some eyes still invisible in the cards", which is the button doing
        // nothing visible rather than the eyes being unfixable.
        //
        // ⚠ THROUGH pendingRelease, NEVER BY CLEARING entries OUTRIGHT. A
        // FUCK::Image destructor calls ReleaseImage, and this runs from the
        // settings panel inside FLICK's own draw pass, so a handle freed here
        // could be one the current submit is still reading. Eviction already
        // solved that by deferring the release a full frame, and this is the
        // same hazard with the same answer; the only difference is that it
        // takes every entry rather than the LRU victim.
        for (auto& [key, e] : s.entries) {
            s.pendingRelease.push_back(std::move(e.image));
        }
        const auto n = s.entries.size();
        s.entries.clear();
        s.stats.evicted += n;
        spdlog::info("PreviewCache: dropped {} card(s) held in memory, so the rebuilt "
                     "cache is what the browser draws from this session rather than "
                     "next one.", n);
    }

    void OnEditorClose() {
        auto& s = Get();
        if (!Settings::GetSingleton().previewDiskCache) {
            // Off means "keep nothing between sessions", not "display
            // nothing": the files were still needed to show the cards, so
            // they go now instead of never existing.
            PreviewDiskCache::RebuildAll();
        } else {
            PreviewDiskCache::FlushIfDirty();
        }
        if (s.harness.drains == 0) {
            return;
        }
        double p95 = 0.0;
        if (!s.harness.ring.empty()) {
            auto sorted = s.harness.ring;
            std::sort(sorted.begin(), sorted.end());
            p95 = sorted[static_cast<std::size_t>(
                static_cast<double>(sorted.size() - 1) * 0.95)];
        }
        spdlog::info("PreviewCache[frames]: drains={} over33={} over50={} "
                     "p95={:.2f}ms max={:.2f}ms",
                     s.harness.drains, s.harness.over33, s.harness.over50, p95,
                     s.harness.maxMs);
        spdlog::info("PreviewCache[drain]: held={} built={} loaded={} failed={} "
                     "pruned={} evicted={}",
                     s.entries.size(), s.stats.built, s.stats.loaded, s.stats.failed,
                     s.stats.pruned, s.stats.evicted);
    }

}  // namespace OS::PreviewCache
