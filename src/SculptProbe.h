#pragma once

#include "FaceGenFod.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// The sculpt-route probe: what the head the game just built is made of, shape
// by shape, beside the chargen `.tri` each shape's part carries.
//
// ⚠⚠ WHY THIS EXISTS AND WHAT IT DECIDES. A RaceMenu sculpt lives in the
// preset file as one entry per head part: `host` is that part's chargen
// `.tri`, and `data` is a sparse `[vertexIndex, dx, dy, dz]` list over a
// divisor of 10000. The deltas are measured against the head AFTER that
// preset's slider morphs, never against a stock head (measured 2026-08-23:
// nine presets sharing a 3832 vertex head produced eight different bases),
// so writing a sculpt means reading the built head and subtracting it from an
// exported mesh. That only works if the two agree shape for shape. An author
// who built their mesh on another version of a head mod will not agree, and
// the honest answer there is to refuse rather than to guess. This probe is
// the measurement that says which rig is which, and it runs BEFORE any of it
// is built.
//
// r61 grew it VALUE columns for the second-save face hunt: the r60 rows
// proved shape names and vert counts identical between a right head and a
// wrong one, so the difference lives in vertex VALUES this probe could not
// see. Per dynamic shape it now sums the morphed output (dynamicData) and
// the base-morph accumulator (FOD), and each dump carries the base's
// faceData sliders — the three places a preset's shape can fail to land.
// ⚠ dynamicData also carries live expression morphs, so a small pos drift
// between dumps is noise; the hunt's signal is whole-face.
//
// ⚠ AN INSTRUMENT. It joins FsmpProbe, CrossSaveProbe and the watches in the
// one-commit strip once the route is either built or abandoned.
namespace OS::SculptProbe {

    namespace detail {

        inline std::atomic<std::size_t> g_lastFingerprint{ 0 };

        // The chargen `.tri` of every head part on the base, mains and their
        // extras, keyed by editor ID. ⚠ ONE LEVEL, NO RECURSION, the same
        // rule HeadPart::ScenePathsOf follows: chargen nests one level and a
        // cycle in authored data must not hang a walk.
        inline std::unordered_map<std::string, std::string> ChargenTriByPart(
            RE::TESNPC* a_base) {
            std::unordered_map<std::string, std::string> out;
            if (!a_base) {
                return out;
            }
            const auto note = [&out](RE::BGSHeadPart* a_part) {
                if (!a_part) {
                    return;
                }
                const char* const edid = a_part->GetFormEditorID();
                if (!edid || !*edid) {
                    return;
                }
                const auto& tri =
                    a_part->morphs[RE::BGSHeadPart::MorphIndices::kChargenMorph];
                out.insert_or_assign(edid, tri.model.empty()
                                               ? std::string{}
                                               : std::string(tri.model.c_str()));
            };
            for (std::uint32_t i = 0; i < a_base->numHeadParts; ++i) {
                auto* const part = a_base->headParts[i];
                note(part);
                if (part) {
                    for (auto* const extra : part->extraParts) {
                        note(extra);
                    }
                }
            }
            return out;
        }

    }  // namespace detail

    // The r66 time-series half of the load-double hunt: ONE line, every
    // FOD-bearing shape's |sum|, no fingerprint gate. The full Dump fires per
    // distinct head and so cannot say WHEN between two builds a value moved;
    // this runs on a dense post-load ladder and pins the transition to a
    // second, so the log's neighbours name the writer. Game thread only.
    inline void PulseHead(const char* a_note) {
        auto* const pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return;
        }
        auto* const faceNode = pc->GetFaceNodeSkinned();
        if (!faceNode) {
            spdlog::info("FodPulse[{}]: no skinned face node.", a_note);
            return;
        }
        std::string line;
        RE::BSVisit::TraverseScenegraphGeometries(
            faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                if (!a_geom || a_geom->name.empty() ||
                    a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                const auto  verts = ds->GetTrishapeRuntimeData().vertexCount;
                const auto  fod   = OS::FaceGen::ResolveFod(
                    a_geom, static_cast<std::uint16_t>(verts));
                if (!fod) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                double sum = 0.0;
                for (std::uint32_t i = 0; i < fod.count; ++i) {
                    const float* const d =
                        fod.verts + i * OS::FaceGen::kFodFloatsPerVertex;
                    sum += std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]);
                }
                // The buffer pointer rides every entry since r66: a value step
                // with the SAME pointer is an in-place write, with a NEW
                // pointer a rebuild, and the r66 round needed exactly that
                // split for the +0.4 s window the dumps cannot see into.
                line += fmt::format("{}'{}'={:.2f}@{}", line.empty() ? "" : " ",
                                    a_geom->name.c_str(), sum,
                                    static_cast<const void*>(fod.verts));
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        spdlog::info("FodPulse[{}]: {}", a_note, line.empty() ? "(no FOD shapes)" : line);
    }

    // Game thread only: it walks the live scenegraph.
    inline void Dump(const char* a_where) {
        auto* const pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return;
        }
        auto* const faceNode = pc->GetFaceNodeSkinned();
        if (!faceNode) {
            spdlog::info("SculptProbe[{}]: no skinned face node.", a_where);
            return;
        }
        auto* const base = pc->GetActorBase();
        const auto  tris = detail::ChargenTriByPart(base);

        struct Row {
            std::string name;
            std::string tri;
            std::uint32_t verts = 0;
            bool          dynamic = false;
            double        posSum = 0.0;   // Σ|x|+|y|+|z| over dynamicData
            float         v0[3] = { 0, 0, 0 };
            std::uint32_t fodNz = 0;      // verts with any nonzero base delta
            double        fodSum = 0.0;   // Σ|delta| over the FOD block
            bool          fod = false;    // the block passed the layout gate
            // ⚠ IDENTITY, NOT VALUE. The load-double hunt needs to know
            // whether a "fresh" build reuses the previous build's buffers
            // (write-through poison in the cached base) or clones new ones
            // that already carry doubled values (poisoned clone source).
            // Neither pointer joins the fingerprint: a rebuild may move both
            // without moving a single value, and value movement is the dump
            // signal.
            const void*   fodPtr = nullptr;
            const void*   dynPtr = nullptr;
        };
        std::vector<Row> rows;
        RE::BSVisit::TraverseScenegraphGeometries(
            faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                if (!a_geom || a_geom->name.empty()) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                Row row;
                row.name = a_geom->name.c_str();
                // ⚠ ONLY A DYNAMIC SHAPE CAN CARRY A SCULPT. The morphed
                // output lives in dynamicData, and a plain BSTriShape on the
                // face node has no such buffer to read or to write.
                row.dynamic =
                    a_geom->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape;
                if (row.dynamic) {
                    auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    row.verts = ds->GetTrishapeRuntimeData().vertexCount;
                    const auto& dd = ds->GetDynamicTrishapeRuntimeData();
                    row.dynPtr = dd.dynamicData;
                    if (dd.dynamicData && row.verts > 0) {
                        const auto* const v =
                            static_cast<const OS::FaceGen::DynVertex*>(dd.dynamicData);
                        row.v0[0] = v[0].x;
                        row.v0[1] = v[0].y;
                        row.v0[2] = v[0].z;
                        for (std::uint32_t i = 0; i < row.verts; ++i) {
                            row.posSum += std::fabs(v[i].x) + std::fabs(v[i].y) +
                                          std::fabs(v[i].z);
                        }
                    }
                    if (const auto fod = OS::FaceGen::ResolveFod(
                            a_geom, static_cast<std::uint16_t>(row.verts))) {
                        row.fod = true;
                        row.fodPtr = fod.verts;
                        for (std::uint32_t i = 0; i < fod.count; ++i) {
                            const float* const d =
                                fod.verts + i * OS::FaceGen::kFodFloatsPerVertex;
                            const double mag = std::fabs(d[0]) + std::fabs(d[1]) +
                                               std::fabs(d[2]);
                            row.fodSum += mag;
                            if (mag > 0.0) {
                                ++row.fodNz;
                            }
                        }
                    }
                }
                if (const auto it = tris.find(row.name); it != tris.end()) {
                    row.tri = it->second;
                }
                rows.push_back(std::move(row));
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        if (rows.empty()) {
            spdlog::info("SculptProbe[{}]: the face node carries no named "
                         "geometry.", a_where);
            return;
        }
        // ⚠ ONE DUMP PER DISTINCT HEAD, because a head build is not rare and
        // a preset apply is several of them in a row. The values ride the
        // fingerprint (quantised, so float noise below the signal does not
        // re-dump): a build whose VERTICES moved logs even when its shape
        // list did not, which is exactly the second-save case.
        std::size_t fp = 1469598103934665603ull;
        const auto  fold = [&fp](std::size_t a_bits) {
            fp = (fp ^ a_bits) * 1099511628211ull;
        };
        for (const auto& row : rows) {
            for (const char c : row.name) {
                fold(static_cast<unsigned char>(c));
            }
            fold(row.verts);
            fold(static_cast<std::size_t>(std::llround(row.posSum / 8.0)));
            fold(row.fodNz);
            fold(static_cast<std::size_t>(std::llround(row.fodSum * 100.0)));
        }
        if (auto* const fd = base ? base->faceData : nullptr) {
            for (const float m : fd->morphs) {
                fold(static_cast<std::size_t>(std::llround(m * 100.0)));
            }
            for (const std::int32_t p : fd->parts) {
                fold(static_cast<std::size_t>(p));
            }
        }
        if (detail::g_lastFingerprint.exchange(fp) == fp) {
            return;
        }
        spdlog::info("SculptProbe[{}]: {} shape(s) on the built head.",
                     a_where, rows.size());
        for (const auto& row : rows) {
            if (row.dynamic) {
                spdlog::info(
                    "SculptProbe[{}]:   '{}' verts={} dynamic pos|sum|={:.1f} "
                    "v0=({:.3f},{:.3f},{:.3f}) fod={} dynPtr={} fodPtr={} "
                    "chargen='{}'",
                    a_where, row.name, row.verts, row.posSum, row.v0[0],
                    row.v0[1], row.v0[2],
                    row.fod ? fmt::format("{} nz |sum|={:.4f}", row.fodNz,
                                          row.fodSum)
                            : "-",
                    fmt::ptr(row.dynPtr), fmt::ptr(row.fodPtr),
                    row.tri.empty() ? "-" : row.tri);
            } else {
                spdlog::info("SculptProbe[{}]:   '{}' verts={} STATIC (no "
                             "sculpt buffer) chargen='{}'",
                             a_where, row.name, row.verts,
                             row.tri.empty() ? "-" : row.tri);
            }
        }
        if (auto* const fd = base ? base->faceData : nullptr) {
            std::string morphs;
            for (std::size_t i = 0; i < RE::TESNPC::FaceData::Morphs::kTotal; ++i) {
                morphs += fmt::format("{}{:.2f}", i ? "," : "", fd->morphs[i]);
            }
            spdlog::info(
                "SculptProbe[{}]: faceData parts=[{},{},{},{}] morphs=[{}]",
                a_where, fd->parts[0], fd->parts[1], fd->parts[2], fd->parts[3],
                morphs);
        } else {
            spdlog::info("SculptProbe[{}]: the base carries NO faceData block.",
                         a_where);
        }
    }

    // The FOD pulse ladder, r66. It moved here from CrossSaveProbe when the
    // appearance watchdog retired that header's eight probes; this is the half
    // the face arc still owes a decision on, and it called into this file
    // anyway.
    //
    // ⚠ OWED, NOT KEPT. It belongs in the instrument strip beside the
    // epoch-align double pass, which the user has deferred to a Trello card
    // along with the RaceMenu sculpt fix. Retiring the probes did not settle
    // that, so it is carried across rather than deleted.
    inline std::atomic<std::uint32_t>& PulseEpoch() {
        static std::atomic<std::uint32_t> epoch{ 0 };
        return epoch;
    }

    inline void ArmPulseLadder() {
        const std::uint32_t epoch = PulseEpoch().fetch_add(1, std::memory_order_acq_rel) + 1;
        std::thread([epoch] {
            static constexpr int kPulseMs[] = { 200,  400,  600,  800,  1000,
                                                1300, 1600, 2000, 2500, 3000,
                                                4000, 5000, 6000, 7000, 8000,
                                                9000, 10000, 12000, 15000 };
            int elapsed = 0;
            for (const int at : kPulseMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(at - elapsed));
                elapsed = at;
                if (PulseEpoch().load(std::memory_order_acquire) != epoch) {
                    return;  // a newer load owns the ladder now
                }
                if (auto* const task = SKSE::GetTaskInterface()) {
                    task->AddTask([at] {
                        char note[24];
                        std::snprintf(note, sizeof(note), "pulse+%dms", at);
                        OS::SculptProbe::PulseHead(note);
                    });
                }
            }
        }).detach();
    }

}  // namespace OS::SculptProbe
