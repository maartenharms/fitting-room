#include "PaintScrape.h"

#include "OverlayLocations.h"
#include "OverlayTextures.h"  // the rescan that carries the new answers to the cards
#include "Settings.h"         // [Debug] bPaintScrape, the OS-231 A/B switch

#include <json/json.h>

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

namespace OS::PaintScrape {

    namespace {

        using OverlayLocations::Mask;

        // Where the packs push their rows. MEASURED in racemenubase.psc: this
        // exact path is what UI.InvokeStringA is handed, one function name at a
        // time, so if this string is wrong nothing else in the file can work.
        constexpr const char* kPanelsPath = "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance";

        // ⚠ THE ALIAS IS WHERE THE ORIGINAL LIVES WHILE WE ARE WRAPPED, AND IT
        // LIVES IN THE MOVIE RATHER THAN IN C++. A GFxValue is a reference into
        // the movie's own heap: hold one in a static and it outlives the heap
        // the moment the menu closes, which is a crash with our name on it.
        // Parked on the panels object instead, the original dies exactly when
        // the movie does and this file holds no engine memory at all between the
        // open and the close.
        //
        // ⚠⚠ AND IT IS THE RE-ENTRY GUARD TOO. Install runs from the open event
        // AND from the retry, so it can be reached twice on one movie. Without
        // this check the second pass would park OUR wrapper as "the original",
        // and the wrapper would then forward to itself: RaceMenu's paint lists
        // would recurse until the stack ran out. The alias existing means we are
        // already in place and there is nothing to do.
        struct ListSpec {
            const char* fn;
            const char* alias;
            Mask        bit;
            const char* label;
        };

        constexpr ListSpec kLists[] = {
            // ⚠ THE WARPAINT LIST IS NOT A LOCATION, exactly as in the shipped
            // table: it is the Makeup section's library, and a pack that appears
            // ONLY here is what the overlays picker keeps out.
            { "RSM_AddWarpaints", "FR_orig_RSM_AddWarpaints", OverlayLocations::kWarpaint,
              "warpaint" },
            { "RSM_AddBodyPaints", "FR_orig_RSM_AddBodyPaints",
              OverlayLocations::Bit(OverlayPlan::Location::kBody), "body" },
            { "RSM_AddHandPaints", "FR_orig_RSM_AddHandPaints",
              OverlayLocations::Bit(OverlayPlan::Location::kHands), "hands" },
            { "RSM_AddFeetPaints", "FR_orig_RSM_AddFeetPaints",
              OverlayLocations::Bit(OverlayPlan::Location::kFeet), "feet" },
            { "RSM_AddFacePaints", "FR_orig_RSM_AddFacePaints",
              OverlayLocations::Bit(OverlayPlan::Location::kFace), "face" },
        };
        constexpr std::size_t kListCount = std::size(kLists);

        // ---- what was captured ------------------------------------------------
        //
        // ⚠ UNDER A LOCK EVEN THOUGH THE CALLS LOOK MAIN-THREAD. Papyrus hands
        // UI.InvokeStringA to SKSE, which marshals it onto the UI task queue, and
        // "which thread finally runs it" is another mod's implementation detail
        // rather than a promise. The map is touched a few thousand times per
        // trip and never per frame, so the lock costs nothing worth measuring.
        std::mutex                             g_lock;
        std::unordered_map<std::string, Mask>  g_rows;
        std::size_t                            g_entries[kListCount]{};
        std::size_t                            g_malformed = 0;

        // Install state, main thread only.
        bool          g_installed      = false;
        std::size_t   g_wrapped        = 0;  // how many of the five went on
        int           g_attempts       = 0;
        std::uint32_t g_generation     = 0;  // bumped on every open and close
        bool          g_menuOpen       = false;

        // How long to keep asking. The panels instance is built by the SWF's own
        // first frames; the packs' rows arrive later still, because Papyrus
        // delivers RSM_Initialized asynchronously and every pack quest then runs
        // a handler. Half a second of frames is far more than that gap and still
        // short enough that a menu opened and shut immediately stops asking.
        constexpr int kMaxAttempts = 30;

        void Note(std::size_t a_list, const char* a_entry) {
            if (!a_entry) {
                return;
            }
            const auto paths = OverlayLocations::PathsInEntry(a_entry);
            std::scoped_lock lock{ g_lock };
            ++g_entries[a_list];
            if (paths.empty()) {
                ++g_malformed;
                return;
            }
            for (const auto& path : paths) {
                auto& mask = g_rows[OverlayLocations::Key(path)];
                // ⚠ OR'D, NOT ASSIGNED. A pack that registers one file in two
                // lists sends it past two different wrappers, and the answer is
                // both locations, exactly as the offline generator's FromLists
                // folds a row that appears twice.
                mask = static_cast<Mask>(mask | kLists[a_list].bit);
            }
        }

        // ---- the wrapper -------------------------------------------------------

        class Recorder : public RE::GFxFunctionHandler {
        public:
            explicit Recorder(std::size_t a_list) : list(a_list) {}

            void Call(Params& a_params) override {
                // A Scaleform callback must never throw into the player.
                try {
                    Forward(a_params);
                } catch (...) {
                    spdlog::error("PaintScrape: forwarding {} threw; RaceMenu's {} list may be "
                                  "short this trip.",
                                  kLists[list].fn, kLists[list].label);
                }
                try {
                    Record(a_params);
                } catch (...) {
                    spdlog::error("PaintScrape: recording {} threw; the scrape is incomplete.",
                                  kLists[list].fn);
                }
            }

        private:
            // ⚠⚠ FORWARDED FIRST AND RECORDED SECOND, AND THE ORDER IS THE
            // SAFETY. Everything past this point is another mod's feature: if
            // the original does not run, the player's RaceMenu loses a paint
            // list. So the pack's own call is made before anything of ours can
            // go wrong with it.
            void Forward(Params& a_params) {
                RE::GFxValue        self;
                RE::GFxValue*       target = nullptr;
                if (a_params.thisPtr && a_params.thisPtr->IsObject()) {
                    target = a_params.thisPtr;
                } else if (a_params.movie &&
                           a_params.movie->GetVariable(&self, kPanelsPath) && self.IsObject()) {
                    // ⚠ A FALLBACK RATHER THAN AN ASSUMPTION. Scaleform resolves
                    // the path SKSE hands it and calls with the parent as this,
                    // which is the panels instance; re-resolving is what covers
                    // a caller that does not.
                    target = &self;
                }
                if (!target) {
                    spdlog::error("PaintScrape: {} was called with no object to forward to, so "
                                  "RaceMenu's {} list lost this pack's rows.",
                                  kLists[list].fn, kLists[list].label);
                    return;
                }
                if (!target->Invoke(kLists[list].alias, a_params.retVal, a_params.args,
                                    a_params.argCount)) {
                    spdlog::error("PaintScrape: {} could not be forwarded to {}; RaceMenu's {} "
                                  "list lost this pack's rows.",
                                  kLists[list].fn, kLists[list].alias, kLists[list].label);
                }
            }

            void Record(Params& a_params) {
                // The rows arrive as ONE argument that is an AS array: SKSE's
                // UI.InvokeStringA builds an array out of the Papyrus String[]
                // and passes it as a single value. The loose form is covered
                // because it costs two lines and a caller that spreads the
                // strings would otherwise record nothing at all.
                if (a_params.argCount >= 1 && a_params.args && a_params.args[0].IsArray()) {
                    const auto& list0 = a_params.args[0];
                    const auto  count = list0.GetArraySize();
                    for (std::uint32_t i = 0; i < count; ++i) {
                        RE::GFxValue entry;
                        if (list0.GetElement(i, &entry) && entry.IsString()) {
                            Note(list, entry.GetString());
                        }
                    }
                    return;
                }
                for (std::uint32_t i = 0; i < a_params.argCount && a_params.args; ++i) {
                    if (a_params.args[i].IsString()) {
                        Note(list, a_params.args[i].GetString());
                    }
                }
            }

            std::size_t list;
        };

        // ⚠ ALLOCATED ONCE AND NEVER RELEASED, ON PURPOSE. A GFxFunctionHandler
        // is reference counted and the movie takes a reference of its own, so
        // holding ours forever costs five objects for the process's life and
        // removes every question about what happens if the movie outlives a
        // static, or a static's destructor runs after the Scaleform heap is
        // gone. The same five are reused for every RaceMenu trip.
        Recorder* HandlerFor(std::size_t a_list) {
            static Recorder* handlers[kListCount] = {};
            if (!handlers[a_list]) {
                handlers[a_list] = new Recorder(a_list);
            }
            return handlers[a_list];
        }

        // ---- installing --------------------------------------------------------

        enum class Probe {
            kNoMovie,    // RaceMenu is not up, or has no movie yet
            kNoPanels,   // the movie is there and the panels instance is not
            kReady,      // the panels instance is there
        };

        Probe ProbePanels(RE::GFxMovieView*& a_movie, RE::GFxValue& a_panels) {
            auto* const ui = RE::UI::GetSingleton();
            if (!ui) {
                return Probe::kNoMovie;
            }
            const auto menu = ui->GetMenu(RE::RaceSexMenu::MENU_NAME);
            if (!menu || !menu->uiMovie) {
                return Probe::kNoMovie;
            }
            a_movie = menu->uiMovie.get();
            if (!a_movie->GetVariable(&a_panels, kPanelsPath) || !a_panels.IsObject()) {
                return Probe::kNoPanels;
            }
            return Probe::kReady;
        }

        // The measurement the whole route was gated on, written once per attempt
        // that mattered rather than every frame.
        void SayProbe(int a_attempt, Probe a_probe, const RE::GFxValue& a_panels) {
            if (a_probe != Probe::kReady) {
                spdlog::info("PaintScrape: attempt {}: {}.", a_attempt,
                             a_probe == Probe::kNoMovie
                                 ? "the RaceSex Menu has no movie yet"
                                 : "the movie is up but _root.RaceSexMenuBaseInstance."
                                   "RaceSexPanelsInstance is not there yet");
                return;
            }
            RE::GFxValue member;
            const bool   has = a_panels.GetMember(kLists[1].fn, &member);
            spdlog::info("PaintScrape: attempt {}: RaceSexPanelsInstance exists; {} is {} "
                         "(GFx type {}).",
                         a_attempt, kLists[1].fn,
                         has && member.IsObject() ? "callable" : "NOT there",
                         has ? static_cast<int>(member.GetType()) : -1);
        }

        bool Install(int a_attempt) {
            RE::GFxMovieView* movie = nullptr;
            RE::GFxValue      panels;
            const auto        probe = ProbePanels(movie, panels);
            // The first attempt is the answer to "does it exist at the open
            // event", which is the thing the route was waiting on; after that
            // only the landing is worth a line.
            if (a_attempt == 1 || probe == Probe::kReady) {
                SayProbe(a_attempt, probe, panels);
            }
            if (probe != Probe::kReady) {
                return false;
            }

            // Already wrapped on this movie: the alias is the witness. See the
            // note above ListSpec for what happens without this.
            if (panels.HasMember(kLists[0].alias)) {
                spdlog::info("PaintScrape: already wrapped on this movie, attempt {} did "
                             "nothing.",
                             a_attempt);
                g_installed = true;
                return true;
            }

            std::size_t wrapped = 0;
            for (std::size_t i = 0; i < kListCount; ++i) {
                RE::GFxValue original;
                if (!panels.GetMember(kLists[i].fn, &original) || !original.IsObject()) {
                    spdlog::warn("PaintScrape: {} is not on RaceSexPanelsInstance, so the {} "
                                 "list cannot be read.",
                                 kLists[i].fn, kLists[i].label);
                    continue;
                }
                if (!panels.SetMember(kLists[i].alias, original)) {
                    spdlog::warn("PaintScrape: could not park {} as {}; leaving it alone.",
                                 kLists[i].fn, kLists[i].alias);
                    continue;
                }
                RE::GFxValue wrapper;
                movie->CreateFunction(&wrapper, HandlerFor(i));
                if (!wrapper.IsObject() || !panels.SetMember(kLists[i].fn, wrapper)) {
                    // ⚠ PUT IT BACK. A half-installed list whose original is
                    // only under the alias is a paint list RaceMenu never gets.
                    panels.SetMember(kLists[i].fn, original);
                    panels.DeleteMember(kLists[i].alias);
                    spdlog::warn("PaintScrape: could not wrap {}, so it was left as it was.",
                                 kLists[i].fn);
                    continue;
                }
                ++wrapped;
            }

            g_wrapped   = wrapped;
            g_installed = wrapped > 0;
            spdlog::info("PaintScrape: wrapped {} of {} paint list(s) on attempt {}.", wrapped,
                         kListCount, a_attempt);
            return g_installed;
        }

        void Uninstall() {
            RE::GFxMovieView* movie = nullptr;
            RE::GFxValue      panels;
            if (ProbePanels(movie, panels) != Probe::kReady) {
                // The movie is already gone, which takes our wrappers and the
                // parked originals with it. Nothing to undo and nothing leaked:
                // this file holds no GFxValue between the two events.
                return;
            }
            for (std::size_t i = 0; i < kListCount; ++i) {
                RE::GFxValue original;
                if (!panels.GetMember(kLists[i].alias, &original) || !original.IsObject()) {
                    continue;
                }
                panels.SetMember(kLists[i].fn, original);
                panels.DeleteMember(kLists[i].alias);
            }
        }

        // ---- the file ----------------------------------------------------------

        [[nodiscard]] std::string NowStamp() {
            const auto now = std::time(nullptr);
            std::tm    local{};
            if (localtime_s(&local, &now) != 0) {
                return "unknown";
            }
            char buffer[32]{};
            if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0) {
                return "unknown";
            }
            return buffer;
        }

        // Returns the row count written, or 0 when nothing was written.
        std::size_t WriteScrape() {
            std::unordered_map<std::string, Mask> rows;
            std::size_t                           entries[kListCount]{};
            std::size_t                           malformed = 0;
            {
                std::scoped_lock lock{ g_lock };
                rows = g_rows;
                for (std::size_t i = 0; i < kListCount; ++i) {
                    entries[i] = g_entries[i];
                }
                malformed = g_malformed;
            }

            // ⚠⚠ ALL FIVE OR NOTHING. A scraped row OUTRANKS the shipped table,
            // so a capture that missed AddFacePaints would record every face
            // texture as warpaint-only and the picker would hide art the player
            // has. Under-reading is the one way this feature can take something
            // away, and this is the gate that stops it.
            if (g_wrapped != kListCount) {
                spdlog::warn("PaintScrape: only {} of {} lists were wrapped, so nothing was "
                             "written. A partial scrape would read a face texture as makeup.",
                             g_wrapped, kListCount);
                return 0;
            }
            // ⚠ AND AN EMPTY TRIP WRITES NOTHING. A menu opened and shut before
            // the packs' handlers ran has nothing to say, and blanking a good
            // file with it would undo the last real scrape.
            if (rows.empty()) {
                spdlog::info("PaintScrape: RaceMenu closed with no paint rows seen, so the "
                             "scrape file was left as it was.");
                return 0;
            }

            Json::Value root{ Json::objectValue };
            Json::Value paints{ Json::objectValue };
            for (const auto& [key, mask] : rows) {
                paints[key] = OverlayLocations::MaskToLists(mask);
            }
            root["paints"] = paints;

            Json::Value header{ Json::objectValue };
            header["when"]  = NowStamp();
            header["rows"]  = static_cast<Json::UInt64>(rows.size());
            Json::Value seen{ Json::objectValue };
            for (std::size_t i = 0; i < kListCount; ++i) {
                seen[kLists[i].label] = static_cast<Json::UInt64>(entries[i]);
            }
            header["entries"]   = seen;
            header["malformed"] = static_cast<Json::UInt64>(malformed);
            // ⚠ NO RaceMenu VERSION FIELD. There is no string on the movie that
            // honestly carries one, and a field filled with a guess is worse
            // than a field that is not there: the next reader would trust it.
            root["scraped"] = header;

            const auto file = OverlayLocations::ScrapedFile();
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            // ⚠ STRAIGHT TO THE DESTINATION, NOT THROUGH A TEMPORARY AND A
            // RENAME. Under MO2 the rename hook has been seen to fault on a
            // write burst (memory: mo2-rename-hook-crashes-under-a-write-burst),
            // and this file is regenerated on the next trip anyway, so a torn
            // write costs one more RaceMenu visit rather than any data.
            std::ofstream out{ file };
            if (!out) {
                spdlog::error("PaintScrape: could not open {} for writing, so this trip's "
                              "reading was lost.",
                              file.string());
                return 0;
            }
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "  ";
            out << Json::writeString(builder, root);
            out.close();
            if (!out) {
                spdlog::error("PaintScrape: {} did not write cleanly.", file.string());
                return 0;
            }
            return rows.size();
        }

        void Attempt(std::uint32_t a_generation) {
            // A retry queued before the menu shut must not run after it: the
            // movie it was queued for is gone and the next open gets its own.
            if (a_generation != g_generation || !g_menuOpen || g_installed) {
                return;
            }
            ++g_attempts;
            if (Install(g_attempts)) {
                return;
            }
            if (g_attempts >= kMaxAttempts) {
                spdlog::warn("PaintScrape: gave up after {} attempts; RaceMenu's lists were "
                             "not readable this trip.",
                             g_attempts);
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_generation] { Attempt(a_generation); });
            }
        }

    }  // namespace

    void OnRaceMenuOpened() {
        // OS-231 diagnostic position. The scrape's wrappers run inside the
        // RaceSex Menu's own AdvanceMovie tick, which is the pass the preset
        // load CTD corrupts, so the A/B needs a way to keep the DLL in and
        // the wrappers out. Off means no wrap is ever installed this visit;
        // OnRaceMenuClosed stays safe because g_installed never goes true.
        if (!Settings::GetSingleton().paintScrape) {
            spdlog::info("PaintScrape: disabled by [Debug] bPaintScrape, the menu "
                         "visit goes unwrapped.");
            return;
        }
        ++g_generation;
        g_menuOpen  = true;
        g_installed = false;
        g_wrapped   = 0;
        g_attempts  = 0;
        {
            std::scoped_lock lock{ g_lock };
            g_rows.clear();
            for (auto& count : g_entries) {
                count = 0;
            }
            g_malformed = 0;
        }
        Attempt(g_generation);
    }

    void OnRaceMenuClosed() {
        if (!g_menuOpen) {
            return;
        }
        g_menuOpen = false;
        ++g_generation;  // any queued retry is now stale
        if (!g_installed) {
            return;
        }
        Uninstall();

        const auto rows = WriteScrape();
        if (rows == 0) {
            return;
        }
        // ⚠ RELOADED HERE RATHER THAN AT THE NEXT LAUNCH, which is why the
        // table is behind an atomic swap now. See the note above LoadReport.
        const auto report = OverlayLocations::Load();
        spdlog::info("PaintScrape: {} row(s) written to {}. OverlayLocations reloaded: {}",
                     rows, OverlayLocations::ScrapedFile().filename().string(),
                     report.diagnostic);
        // ⚠ AND THE SCAN IS WHAT CARRIES IT TO THE CARDS. The picker draws off
        // OverlayTextures' snapshot, whose entries hold `registered`, `warpaint`
        // and the resolved `locations` as they were when the scan ran. Reloading
        // the table without republishing changes nothing on screen.
        OverlayTextures::RequestScan();
    }

}  // namespace OS::PaintScrape
