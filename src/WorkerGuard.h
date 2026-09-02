#pragma once

#include "PCH.h"

#include <exception>
#include <utility>

namespace OS::WorkerGuard {

    // ⚠⚠ EVERY WORKER BODY IN THIS PLUGIN GOES THROUGH HERE, AND THE REASON IS
    // A SHIPPED CRASH THAT NOBODY COULD SEE. An exception that escapes the
    // function of a std::thread does not unwind and is not filtered: the
    // standard requires std::terminate outright, which the CRT implements as a
    // fail-fast. A fail-fast skips the unhandled-exception filter that Crash
    // Logger and Trainwreck hook, so the process dies leaving NO crash log, and
    // no try/catch anywhere on the main thread can reach it - including
    // EditorWindow's own draw guard, which is why the editor could be perfectly
    // protected and the game still vanish.
    //
    // ⚠ THE FAULT WAS NEVER IN OUR DATA, WHICH IS WHY IT WAS ONLY EVER SOME
    // PLAYERS. OverlayTextures walked the player's whole texture tree and read
    // each entry through path::string(), which converts wide to narrow through
    // the ACTIVE CODE PAGE. A mod folder named "至真女性皮肤4K-Zhizhen's female
    // skin 4K" has no mapping in code page 1252 and throws std::system_error on
    // the spot. Whose disk holds a name like that is per-rig and nothing we
    // ship decides it, so the same build was solid for most people and instant
    // death for the rest. Reported twice through August 2026 as "clicking the
    // overlays section crashes my game" with no crash log.
    //
    // Wrapping is CONTAINMENT, not a diagnosis. The log line is as much the
    // point as the survival: it converts a silent process kill into a named
    // exception in FittingRoom.log, which is the difference between a report
    // nobody can act on and a bug with an address.
    //
    // ⚠ noexcept IS DELIBERATE. If the logging itself throws there is nothing
    // left to try, and terminate here would at least be honest about where it
    // came from rather than blaming the worker.
    template <class F>
    void Run(const char* a_name, F&& a_body) noexcept {
        try {
            std::forward<F>(a_body)();
        } catch (const std::exception& e) {
            spdlog::critical(
                "{}: a worker thread threw and was abandoned rather than allowed to end the "
                "process as a fail-fast with no crash log. The exception was: {}",
                a_name, e.what() ? e.what() : "a std::exception with no message");
            if (const auto logger = spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
            spdlog::critical(
                "{}: a worker thread threw something that does not derive from std::exception "
                "and was abandoned rather than allowed to end the process.",
                a_name);
            if (const auto logger = spdlog::default_logger()) {
                logger->flush();
            }
        }
    }

    // ⚠ THE LOOP FLAVOUR EXISTS BECAUSE THE ONE-SHOT FLAVOUR IS WRONG FOR A
    // SERVICE. Wrapping a for(;;) worker from the outside means the first bad
    // item kills the thread for the rest of the session, and every later
    // request queues behind a worker that is never coming back: a permanent
    // hang traded for a crash, which is no bargain. Guarding one ITERATION
    // costs that item and nothing else. Returns false only if the body threw,
    // so a caller that wants to count failures can.
    template <class F>
    bool RunOnce(const char* a_name, F&& a_body) noexcept {
        try {
            std::forward<F>(a_body)();
            return true;
        } catch (const std::exception& e) {
            spdlog::error(
                "{}: one worker item threw and was skipped; the worker is still running. The "
                "exception was: {}",
                a_name, e.what() ? e.what() : "a std::exception with no message");
        } catch (...) {
            spdlog::error(
                "{}: one worker item threw something that does not derive from std::exception "
                "and was skipped; the worker is still running.",
                a_name);
        }
        return false;
    }

}  // namespace OS::WorkerGuard
