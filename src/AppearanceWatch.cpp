#include "AppearanceWatch.h"

#include "HairColor.h"  // what the strands are actually wearing, r85
#include "MakeupApi.h"
#include "MakeupPlan.h"
#include "OverlayApi.h"
#include "OverlayReconcile.h"
#include "Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace OS::AppearanceWatch {

    namespace {

        struct Shot {
            bool          valid{ false };
            // ⚠⚠ THE BASE'S OWN IDENTITY, AND IT IS THE FIELD THE FIRST ROUND
            // WANTED AND DID NOT HAVE. r80 caught the hair form, bodyTintColor
            // and the tint list's skintone slot all flipping to another
            // character's values together and all flipping back together 1.9 s
            // later. Three independent writers agreeing to the millisecond
            // twice is not plausible; ONE READ OF THE WRONG BASE is. Nothing in
            // that round could tell the two apart, because every field it
            // logged was read THROUGH the base and none of them named it.
            std::uintptr_t basePtr{ 0 };
            std::uint32_t  baseFormId{ 0 };
            std::uint32_t raceId{ 0 };
            std::uint32_t charGenRaceId{ 0 };
            bool          female{ false };
            bool          hasHair{ false };
            std::uint32_t hairFormId{ 0 };
            int           hairR{ 0 }, hairG{ 0 }, hairB{ 0 };
            int           bodyR{ 0 }, bodyG{ 0 }, bodyB{ 0 };
            bool          hasSkinSlot{ false };
            int           skinR{ 0 }, skinG{ 0 }, skinB{ 0 };
            float         skinAlpha{ 0.0f };
            std::string   listOrigin;
            std::uint32_t layers{ 0 };
            std::uint32_t layersWithArt{ 0 };
            int           clones3p{ 0 };
            int           clones1p{ 0 };
            std::intptr_t headPartList{ 0 };
            int           headPartCount{ 0 };
            // ⚠⚠ WHAT THE FACE IS ACTUALLY TEXTURED WITH, and retiring the old
            // probes took this with them, which was my omission. A face wears
            // three things: its tint list, its overlays and its DIFFUSE, and
            // the cross-save bleed lives in the third. FIELD 2026-08-25: a Nord
            // save loaded with its own 34-layer tint list and an empty overlay
            // store, every clone wearing the default, and the head still drew
            // another character's blue skin and skull paint the moment RaceMenu
            // rebuilt it. Every field this watchdog had read clean.
            //
            // A facegen head draws its material's OWN tint texture, so the name
            // AND the pointer both matter: the engine caches the head model per
            // NPC, material included, and pointer identity across a boundary is
            // what names a cached carrier rather than a fresh write.
            std::string   headFace;
            // ⚠⚠ THE SAME OMISSION AS headFace, ONE LAYER OVER. Every
            // hair field above is read THROUGH the actor base: the form and
            // its rgb. r85 loaded a Nord whose base carried NO hair colour
            // form at all, `hair (none) rgb=(none)` on three consecutive
            // baselines, and the hair drew BLACK the whole time. A watchdog
            // that only reads the base cannot see that, in exactly the way it
            // could not see the face wearing another character's tint.
            //
            // So: what the geometry wears, plus the material POINTER, because
            // a value that merely looks wrong cannot separate somebody writing
            // black from a rebuild handing back a cached black material.
            std::string   hairTint;
        };

        std::mutex        g_lock;
        Shot              g_last{};
        std::atomic<bool> g_started{ false };
        std::atomic<bool> g_inFlight{ false };
        std::atomic<bool> g_baseline{ true };
        std::atomic<std::int64_t> g_t0{ 0 };
        // ⚠⚠ A ONE-SECOND SAMPLE CANNOT NAME A WRITER. r89 caught the
        // Nord's strands going (17,17,17) -> (14,16,17), the previous
        // character's colour, on ONE material pointer inside a 492 ms gap,
        // with no hair colour form on the base and no Fitting Room write
        // logged. Both of this module's write paths log, and neither fired,
        // so the writer is somebody else and the only way to name it is to
        // pin the change to a frame and read what else is in the log there.
        // The burst runs after a load boundary, which is the only window
        // the fault has ever appeared in.
        //
        // ⚠⚠ FIFTEEN SECONDS WAS TOO SHORT AND THE 2026-08-25 ROUND FELL OFF
        // THE END OF IT. The skin tone reverted to the vanilla race's
        // bodyTintColor at t+22045 ms, so the burst had already dropped to its
        // one-second rest period and the change is pinned to a whole second
        // with nothing else in it. That is the same "pin it to a frame and read
        // what else is in the log there" problem this burst exists to solve,
        // just further out than the hair fault ever sat: the hair moved inside
        // a 492 ms gap near the boundary, and this one waits twenty-two seconds
        // and then moves with NO head build and NO race switch anywhere near
        // it. Sixty seconds covers where the fault actually lives.
        //
        // ⚠ The cost is bounded and known: a sample is a handful of pointer
        // reads and prints only on a transition, so a settled save stays
        // silent and pays 20 samples a second for a minute after a load.
        constexpr auto kBurstFor    = std::chrono::seconds(60);
        constexpr auto kBurstPeriod = std::chrono::milliseconds(50);
        constexpr auto kRestPeriod  = std::chrono::seconds(1);
        std::atomic<std::int64_t> g_burstUntil{ 0 };
        const char*       g_why{ "start" };

        [[nodiscard]] std::int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        [[nodiscard]] std::int64_t SinceBoundary() {
            const auto t0 = g_t0.load(std::memory_order_acquire);
            return t0 == 0 ? 0 : NowMs() - t0;
        }

        // r42's rule, kept: skee fills every slot with its own default art on
        // initialise, so an occupied layer and a painted one are different
        // facts and the count has to carry both.
        [[nodiscard]] bool IsDefaultDiffuse(const std::string& a_path) {
            std::string lower{ a_path };
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find("overlays\\default.dds") != std::string::npos ||
                   lower.find("overlays/default.dds") != std::string::npos;
        }

        // Game thread only. OverlayApi::Read and the scenegraph counts both say so.
        [[nodiscard]] Shot Sample() {
            Shot        s{};
            auto* const pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) {
                return s;
            }
            auto* const base = pc->GetActorBase();
            if (!base) {
                return s;
            }
            s.valid      = true;
            s.basePtr    = reinterpret_cast<std::uintptr_t>(base);
            s.baseFormId = base->GetFormID();

            auto* const race = base->race;
            auto* const cgr  = pc->GetRaceData().charGenRace;
            s.raceId         = race ? race->GetFormID() : 0u;
            s.charGenRaceId  = cgr ? cgr->GetFormID() : 0u;
            s.female         = base->actorData.actorBaseFlags.all(
                RE::ACTOR_BASE_DATA::Flag::kFemale);

            auto* const hrd   = base->headRelatedData;
            auto* const color = hrd ? hrd->hairColor : nullptr;
            if (color) {
                s.hasHair    = true;
                s.hairFormId = color->GetFormID();
                s.hairR      = color->color.red;
                s.hairG      = color->color.green;
                s.hairB      = color->color.blue;
            }

            s.bodyR = base->bodyTintColor.red;
            s.bodyG = base->bodyTintColor.green;
            s.bodyB = base->bodyTintColor.blue;

            s.headPartList  = reinterpret_cast<std::intptr_t>(base->headParts);
            s.headPartCount = base->numHeadParts;

            s.listOrigin = OS::MakeupApi::ListOrigin(pc);
            const auto layers = OS::MakeupApi::Layers(pc);
            const auto state  = OS::MakeupApi::Read(pc);
            const auto n      = std::min(layers.size(), state.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (layers[i].type !=
                    static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                    continue;
                }
                s.hasSkinSlot = true;
                s.skinR       = state[i].tint.r;
                s.skinG       = state[i].tint.g;
                s.skinB       = state[i].tint.b;
                s.skinAlpha   = state[i].strength;
                break;
            }

            if (OverlayApi::Available()) {
                for (const auto& layer : OverlayApi::Layers()) {
                    const auto o = OverlayApi::Read(pc, layer.node);
                    if (!o.hasTexture && !o.hasTint && !o.hasAlpha) {
                        continue;
                    }
                    ++s.layers;
                    if (o.hasTexture && !IsDefaultDiffuse(o.texture)) {
                        ++s.layersWithArt;
                    }
                }
            }

            if (auto* const face = pc->GetFaceNodeSkinned()) {
                RE::BSVisit::TraverseScenegraphGeometries(
                    face, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                            a_geom->GetGeometryRuntimeData()
                                .properties[RE::BSGeometry::States::kEffect]
                                .get());
                        if (!prop) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        auto* const mat =
                            static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                        if (!mat ||
                            mat->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        // The facegen head, which is the one that carries the
                        // bleed. Stop at the first: the mouth is facegen too and
                        // its tint never moves.
                        const char* bound = "";
                        if (auto* const b = mat->diffuseTexture.get()) {
                            bound = b->name.c_str();
                        }
                        auto* const fg =
                            static_cast<RE::BSLightingShaderMaterialFacegen*>(mat);
                        auto* const tt = fg->tintTexture.get();
                        s.headFace = fmt::format(
                            "'{}' bound='{}' mat=0x{:X} tintTex=0x{:X} '{}'",
                            a_geom->name.c_str(), bound,
                            reinterpret_cast<std::uintptr_t>(mat),
                            reinterpret_cast<std::uintptr_t>(tt),
                            tt ? tt->name.c_str() : "");
                        return RE::BSVisit::BSVisitControl::kStop;
                    });
            }

            s.hairTint = OS::HairColor::GeometryTintReading(pc);

            s.clones3p = OverlayReconcile::CountPlayerBodyClones();
            s.clones1p = OverlayReconcile::CountPlayerFirstPersonClones();
            return s;
        }

        void Say(const char* a_field, const std::string& a_from, const std::string& a_to) {
            spdlog::info("AppearanceWatch t+{} ms: {} {} -> {}", SinceBoundary(),
                         a_field, a_from, a_to);
        }

        [[nodiscard]] std::string Rgb(int a_r, int a_g, int a_b) {
            return fmt::format("({},{},{})", a_r, a_g, a_b);
        }

        void Baseline(const Shot& a_s) {
            spdlog::info(
                "AppearanceWatch BASELINE after '{}': base {:08X}@0x{:X} race {:08X} charGenRace {:08X} "
                "female={} hair {} rgb={} bodyTint={} skintone={} alpha={:.2f} "
                "list='{}' layers={} ({} with art) clones 3p={} 1p={} "
                "headParts={} list=0x{:X} head={} hairTint={}.",
                g_why, a_s.baseFormId, a_s.basePtr, a_s.raceId, a_s.charGenRaceId,
                a_s.female,
                a_s.hasHair ? fmt::format("{:08X}", a_s.hairFormId) : "(none)",
                a_s.hasHair ? Rgb(a_s.hairR, a_s.hairG, a_s.hairB) : "(none)",
                Rgb(a_s.bodyR, a_s.bodyG, a_s.bodyB),
                a_s.hasSkinSlot ? Rgb(a_s.skinR, a_s.skinG, a_s.skinB) : "(no slot)",
                a_s.skinAlpha, a_s.listOrigin, a_s.layers, a_s.layersWithArt,
                a_s.clones3p, a_s.clones1p, a_s.headPartCount, a_s.headPartList,
                a_s.headFace.empty() ? "(no facegen head)" : a_s.headFace,
                a_s.hairTint.empty() ? "(no 3d)" : a_s.hairTint);
        }

        void Diff(const Shot& a_was, const Shot& a_is) {
            // FIRST, because everything below is read through it. A base that
            // moved makes every other line on this tick a consequence rather
            // than a separate finding.
            if (a_was.baseFormId != a_is.baseFormId || a_was.basePtr != a_is.basePtr) {
                Say("ACTOR BASE",
                    fmt::format("{:08X}@0x{:X}", a_was.baseFormId, a_was.basePtr),
                    fmt::format("{:08X}@0x{:X}", a_is.baseFormId, a_is.basePtr));
            }
            if (a_was.raceId != a_is.raceId) {
                Say("base race", fmt::format("{:08X}", a_was.raceId),
                    fmt::format("{:08X}", a_is.raceId));
            }
            if (a_was.charGenRaceId != a_is.charGenRaceId) {
                Say("charGenRace", fmt::format("{:08X}", a_was.charGenRaceId),
                    fmt::format("{:08X}", a_is.charGenRaceId));
            }
            if (a_was.female != a_is.female) {
                Say("female", a_was.female ? "true" : "false",
                    a_is.female ? "true" : "false");
            }
            // ⚠⚠ THE FORM AND ITS CONTENTS ARE TWO FIELDS, AND SPLITTING THEM
            // IS THE POINT. r78 and r79 both caught a ColorForm whose own rgb
            // moved while the form id stayed put, which a probe that printed
            // "hair colour 4C000801" and nothing else would have called
            // steady. A changed id is somebody assigning a different colour; a
            // changed rgb under a steady id is somebody writing through the
            // form every earlier painter already read.
            if (a_was.hasHair != a_is.hasHair || a_was.hairFormId != a_is.hairFormId) {
                Say("base hair FORM",
                    a_was.hasHair ? fmt::format("{:08X}", a_was.hairFormId) : "(none)",
                    a_is.hasHair ? fmt::format("{:08X}", a_is.hairFormId) : "(none)");
            } else if (a_is.hasHair && (a_was.hairR != a_is.hairR ||
                                        a_was.hairG != a_is.hairG ||
                                        a_was.hairB != a_is.hairB)) {
                Say(fmt::format("base hair RGB, same form {:08X}", a_is.hairFormId).c_str(),
                    Rgb(a_was.hairR, a_was.hairG, a_was.hairB),
                    Rgb(a_is.hairR, a_is.hairG, a_is.hairB));
            }
            if (a_was.bodyR != a_is.bodyR || a_was.bodyG != a_is.bodyG ||
                a_was.bodyB != a_is.bodyB) {
                Say("bodyTintColor", Rgb(a_was.bodyR, a_was.bodyG, a_was.bodyB),
                    Rgb(a_is.bodyR, a_is.bodyG, a_is.bodyB));
            }
            if (a_was.hasSkinSlot != a_is.hasSkinSlot || a_was.skinR != a_is.skinR ||
                a_was.skinG != a_is.skinG || a_was.skinB != a_is.skinB ||
                a_was.skinAlpha != a_is.skinAlpha) {
                Say("tint skintone",
                    a_was.hasSkinSlot
                        ? fmt::format("{} alpha={:.2f}",
                                      Rgb(a_was.skinR, a_was.skinG, a_was.skinB),
                                      a_was.skinAlpha)
                        : "(no slot)",
                    a_is.hasSkinSlot
                        ? fmt::format("{} alpha={:.2f}",
                                      Rgb(a_is.skinR, a_is.skinG, a_is.skinB),
                                      a_is.skinAlpha)
                        : "(no slot)");
            }
            if (a_was.listOrigin != a_is.listOrigin) {
                Say("tint list", a_was.listOrigin, a_is.listOrigin);
            }
            if (a_was.layers != a_is.layers || a_was.layersWithArt != a_is.layersWithArt) {
                Say("skee overlay layers",
                    fmt::format("{} ({} with art)", a_was.layers, a_was.layersWithArt),
                    fmt::format("{} ({} with art)", a_is.layers, a_is.layersWithArt));
            }
            if (a_was.clones3p != a_is.clones3p) {
                Say("painted clones 3p", std::to_string(a_was.clones3p),
                    std::to_string(a_is.clones3p));
            }
            if (a_was.clones1p != a_is.clones1p) {
                Say("painted clones 1p", std::to_string(a_was.clones1p),
                    std::to_string(a_is.clones1p));
            }
            if (a_was.headFace != a_is.headFace) {
                Say("HEAD TEXTURE",
                    a_was.headFace.empty() ? "(none)" : a_was.headFace,
                    a_is.headFace.empty() ? "(none)" : a_is.headFace);
            }
            if (a_was.hairTint != a_is.hairTint) {
                Say("HAIR TINT", a_was.hairTint.empty() ? "(no 3d)" : a_was.hairTint,
                    a_is.hairTint.empty() ? "(no 3d)" : a_is.hairTint);
            }
            if (a_was.headPartList != a_is.headPartList ||
                a_was.headPartCount != a_is.headPartCount) {
                Say("head part list",
                    fmt::format("{} at 0x{:X}", a_was.headPartCount, a_was.headPartList),
                    fmt::format("{} at 0x{:X}", a_is.headPartCount, a_is.headPartList));
            }
        }

        void SampleAndReport() {
            const auto now = Sample();
            if (!now.valid) {
                return;
            }
            std::scoped_lock l(g_lock);
            if (g_baseline.exchange(false, std::memory_order_acq_rel) || !g_last.valid) {
                Baseline(now);
            } else {
                Diff(g_last, now);
            }
            g_last = now;
        }

    }  // namespace

    void Start() {
        if (!Settings::GetSingleton().appearanceWatch) {
            spdlog::info("AppearanceWatch: off, [Debug] bAppearanceWatch is false.");
            return;
        }
        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        g_t0.store(NowMs(), std::memory_order_release);
        spdlog::info("AppearanceWatch: on, sampling once a second and logging only "
                     "transitions, and every {} ms for {} s after a load boundary. It "
                     "retires the boundary dump and the seven timed rungs behind it.",
                     kBurstPeriod.count(), kBurstFor.count());
        std::thread([] {
            for (;;) {
                const bool burst =
                    NowMs() < g_burstUntil.load(std::memory_order_acquire);
                std::this_thread::sleep_for(burst ? kBurstPeriod
                                                  : std::chrono::duration_cast<
                                                        std::chrono::milliseconds>(
                                                        kRestPeriod));
                // ⚠ ONE SAMPLE IN FLIGHT AT A TIME. The task interface runs
                // these on the game thread and a frame that hitches would
                // otherwise stack a queue of samples that all report the same
                // moment with different timestamps.
                if (g_inFlight.load(std::memory_order_acquire)) {
                    continue;
                }
                auto* const task = SKSE::GetTaskInterface();
                if (!task) {
                    continue;
                }
                g_inFlight.store(true, std::memory_order_release);
                task->AddTask([] {
                    SampleAndReport();
                    g_inFlight.store(false, std::memory_order_release);
                });
            }
        }).detach();
    }

    void NoteLoadBoundary(const char* a_why) {
        if (!Settings::GetSingleton().appearanceWatch) {
            return;
        }
        g_burstUntil.store(
            NowMs() + std::chrono::duration_cast<std::chrono::milliseconds>(kBurstFor)
                          .count(),
            std::memory_order_release);
        {
            std::scoped_lock l(g_lock);
            g_why = a_why ? a_why : "?";
        }
        g_t0.store(NowMs(), std::memory_order_release);
        g_baseline.store(true, std::memory_order_release);
    }

}  // namespace OS::AppearanceWatch
