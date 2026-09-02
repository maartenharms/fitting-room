#include "DyeTick.h"

#include "DyeUnlockCard.h"
#include "EditorWindow.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace OS::DyeTick {

    namespace {

        double NowSeconds() {
            using Seconds = std::chrono::duration<double>;
            return Seconds(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::mutex            g_passLock;
        std::function<void()> g_pass;

        std::atomic<bool>   g_started{ false };  // the thread exists
        std::atomic<bool>   g_running{ false };  // a save is live
        std::atomic<int>    g_intervalSec{ 30 };
        std::atomic<bool>   g_inFlight{ false };
        std::atomic<double> g_lastRun{ 0.0 };
        std::atomic<int>    g_passesThisLoad{ 0 };

        // The first tick of a session comes early and is DELIBERATELY silent.
        //
        // ⚠⚠ THE BASELINE, and it is the same argument
        // DyeUnlocks::TakeAckBaselineIfOwed makes for the gold fold: without it
        // the first thing a returning player sees is a stack of cards for
        // colours they earned last session. The load's own two passes are
        // absorbed the same way, because the announcer is not armed until this
        // module arms it.
        //
        // ⚠ AND ARMING WAITS FOR THE TICK AFTER THAT, not for the first one to
        // return. The tick asks Skyrim for its misc stat counters, and those
        // answer about a second later on the VM's own thread, running a second
        // promotion pass behind this one's back. Arming on the first tick would
        // therefore announce whatever that settle earns, which on a save whose
        // load-time stat request never answered is every counter-gated colour
        // the character owns.
        //
        // The cost is a window at the start of a session, roughly
        // kFirstTickSec + the interval, in which an earned colour is absorbed
        // silently and gets only its gold corner in the grid. That is the safe
        // direction: a missed card is one questline away from happening again,
        // and a wall of forty cards on load is the failure a player would
        // actually report.
        constexpr double kFirstTickSec = 10.0;

        [[nodiscard]] bool MayRunNow() {
            if (!g_running.load(std::memory_order_acquire)) {
                return false;
            }
            // ⚠ NOT WHILE THE EDITOR IS UP. The editor runs its own promotion
            // pass on open and the cards are suppressed over it anyway (user's
            // call, 2026-08-14), so a tick there would queue cards that could
            // only appear after the player closed the editor, describing
            // colours the gold fold already marked in the grid they were just
            // looking at.
            if (OS::EditorWindow::IsOpen()) {
                return false;
            }
            return true;
        }

        void RunOnGameThread() {
            // ⚠ THE IN-FLIGHT GUARD IS NOT DECORATION. The timer thread cannot
            // see how long the game thread takes, so a stalled frame (a cell
            // load, a big autosave) would let it post a second pass behind the
            // first. Promotion is add-only and would survive that, but the
            // gather is 148 form lookups and posting them in a pile during a
            // load screen is the one moment it is least affordable.
            if (g_inFlight.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                g_inFlight.store(false, std::memory_order_release);
                return;
            }
            task->AddTask([] {
                // Re-checked on the game thread: the editor can have opened, or
                // a load started, between the post and the run.
                if (MayRunNow()) {
                    // ⚠ ARM BEFORE THE PASS, FROM THE SECOND TICK ON. Arming
                    // after it would swallow this pass's gains as well, and by
                    // the second tick the first tick's stats settle has long
                    // since landed and been absorbed.
                    if (g_passesThisLoad.load(std::memory_order_relaxed) >= 1 &&
                        !OS::DyeUnlockCard::Armed()) {
                        OS::DyeUnlockCard::Arm();
                        spdlog::info("Dye unlocks: the baseline is taken, so colours "
                                     "earned from here on are announced on screen.");
                    }
                    std::function<void()> pass;
                    {
                        std::scoped_lock lk(g_passLock);
                        pass = g_pass;
                    }
                    if (pass) {
                        pass();
                        g_passesThisLoad.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                g_inFlight.store(false, std::memory_order_release);
            });
        }

        void Loop() {
            constexpr auto kSlice = std::chrono::milliseconds(100);
            for (;;) {
                std::this_thread::sleep_for(kSlice);
                if (!g_running.load(std::memory_order_acquire)) {
                    continue;
                }
                const int interval = g_intervalSec.load(std::memory_order_relaxed);
                if (interval <= 0) {
                    continue;  // parked by the INI
                }
                const double now  = NowSeconds();
                const double last = g_lastRun.load(std::memory_order_relaxed);
                const double due =
                    g_passesThisLoad.load(std::memory_order_relaxed) == 0
                        ? kFirstTickSec
                        : static_cast<double>(interval);
                if (now - last < due) {
                    continue;
                }
                // ⚠ STAMPED BEFORE THE WORK, NOT AFTER. Stamping on completion
                // would make the interval "N seconds of idle" rather than "every
                // N seconds", so a pass that ran long would push the next one
                // out by its own duration, compounding.
                g_lastRun.store(now, std::memory_order_relaxed);
                RunOnGameThread();
            }
        }

        void StartThreadOnce() {
            if (g_started.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            try {
                std::thread(Loop).detach();
                spdlog::info("Dye unlocks: the gameplay tick is running, so a colour "
                             "earned by what you just did arrives while you are still "
                             "standing there.");
            } catch (const std::exception& e) {
                // ⚠ SURVIVABLE, AND THE LINE SAYS HOW. Losing this thread puts
                // the mod back on load-time promotion, which is exactly where it
                // was before this module existed: colours are still earned, they
                // just arrive on the next load or editor open and nothing is
                // announced.
                spdlog::error("Dye unlocks: could not start the gameplay tick ({}). "
                              "Colours will still be earned at load and when the "
                              "editor opens; nothing will be announced on screen.",
                              e.what());
                g_started.store(false, std::memory_order_release);
            }
        }

    }  // namespace

    void SetPass(std::function<void()> a_pass) {
        std::scoped_lock lk(g_passLock);
        g_pass = std::move(a_pass);
    }

    void ApplySettings() {
        const int secs = std::clamp(OS::Settings::GetSingleton().dyeTickSeconds, 0, 600);
        // 0 parks the tick; anything else is floored at 5 so a hand-edited 1
        // cannot ask for a 148-lookup gather every second.
        g_intervalSec.store(secs == 0 ? 0 : std::max(5, secs), std::memory_order_relaxed);
    }

    void Start() {
        ApplySettings();
        g_passesThisLoad.store(0, std::memory_order_relaxed);
        g_lastRun.store(NowSeconds(), std::memory_order_relaxed);
        g_running.store(true, std::memory_order_release);
        if (g_intervalSec.load(std::memory_order_relaxed) > 0) {
            StartThreadOnce();
        }
    }

    void Stop() {
        g_running.store(false, std::memory_order_release);
        g_passesThisLoad.store(0, std::memory_order_relaxed);
    }

}  // namespace OS::DyeTick
