#include "SkinPacks.h"

#include "OverlayPlan.h"  // FromScanPath, the one way a disk path becomes an override path

#include <algorithm>
#include <atomic>
#include <system_error>
#include <thread>
#include <utility>

namespace OS::SkinPacks {

    namespace {

        std::atomic<std::uint64_t>                   g_generation{ 0 };
        std::atomic<int>                             g_active{ 0 };
        std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{
            std::make_shared<const Snapshot>()
        };

        // Where packs live, relative to the textures root, in the form the
        // filesystem wants. SkinPlan::kSkinsDir is the same place in the form
        // an override wants; the two are one folder.
        constexpr const char* kSkinsDir = "FittingRoom/skins";

    }  // namespace

    Snapshot ScanRoot(const std::filesystem::path& a_texturesRoot) {
        Snapshot        snap;
        const auto      root = a_texturesRoot / kSkinsDir;
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) {
            snap.diagnostic = "no skins folder at " + root.string();
            return snap;
        }

        std::filesystem::recursive_directory_iterator it{
            root, std::filesystem::directory_options::skip_permission_denied, ec
        };
        if (ec) {
            snap.diagnostic = "could not walk " + root.string() + ": " + ec.message();
            return snap;
        }

        std::size_t loose      = 0;
        std::size_t duplicates = 0;
        std::size_t unreadable = 0;

        // One entry's work, as a call so every early exit is a return this loop
        // can catch around rather than a continue that would have to remember
        // to advance the iterator itself.
        const auto take = [&](const std::filesystem::directory_entry& entry) {
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) {
                return;
            }
            const auto& file = entry.path();
            auto        ext  = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext != ".dds") {
                return;
            }
            ++snap.filesSeen;

            // ⚠ NOT std::filesystem::relative, for the reason OverlayPlan::
            // FromScanPath spells out: under a mod manager the enumerated file
            // does not live under the directory that was walked.
            const auto overridePath = OverlayPlan::FromScanPath(file.string());
            if (overridePath.empty()) {
                return;
            }
            const auto before = snap.packs.size();
            const auto id     = SkinPlan::AddFile(snap.packs, overridePath);
            if (id.empty()) {
                ++loose;  // sitting at the root, in no pack
                return;
            }
            // AddFile keeps the first of two same-named files in one pack and
            // says nothing; the count is how a pack author finds out.
            if (snap.packs.size() == before) {
                const auto* pack = Find(snap, id);
                if (pack && pack->files.at(SkinPlan::FileName(overridePath)) != overridePath) {
                    ++duplicates;
                }
            }
        };

        // ⚠⚠ THE ec ABOVE COVERS THE CONSTRUCTOR AND NOTHING ELSE. A range-for
        // over this iterator advances with operator++, the THROWING overload,
        // and skip_permission_denied excuses permission errors and no others: a
        // path over MAX_PATH, a broken junction, an IO error, or a name
        // path::string() cannot convert all raise filesystem_error from the
        // increment. This runs on a DETACHED WORKER (RequestScan below), where
        // an escaping exception reaches std::terminate unconditionally, which
        // is a CRT fail-fast: no crash log, and out of reach of the editor's
        // draw guard because it is on another thread. The overlays page starts
        // this scan and OverlayTextures' together, so both had the same silent
        // kill on the same click and both are hardened the same way. See the
        // fuller note in OverlayTextures::ScanRoot.
        //
        // ⚠ THE PATH WALKED HERE IS OURS (FittingRoom/skins) AND STILL NOT SAFE.
        // A pack folder is named by whoever made it, so its name is as arbitrary
        // as any other mod's, and it is the name that trips path conversion.
        const std::filesystem::recursive_directory_iterator end{};
        while (it != end) {
            try {
                take(*it);
            } catch (const std::exception& e) {
                ++unreadable;
                spdlog::warn("SkinPacks: skipped an entry under {} that could not be read: {}",
                             root.string(), e.what());
            } catch (...) {
                ++unreadable;
                spdlog::warn("SkinPacks: skipped an entry under {} that threw something that "
                             "is not a std::exception.", root.string());
            }
            // increment(ec) is the non-throwing overload. On failure the
            // standard leaves the iterator at end(), so the walk cannot resume
            // and stopping with what we have is the honest outcome.
            std::error_code stepEc;
            it.increment(stepEc);
            if (stepEc) {
                snap.diagnostic +=
                    "the walk of " + root.string() + " stopped early: " + stepEc.message() + " ";
                break;
            }
        }

        std::sort(snap.packs.begin(), snap.packs.end(),
                  [](const SkinPlan::Pack& a_lhs, const SkinPlan::Pack& a_rhs) {
                      return SkinPlan::Lower(a_lhs.id) < SkinPlan::Lower(a_rhs.id);
                  });

        // ⚠ APPENDED, NOT ASSIGNED. The early-stop note above is written before
        // this line and an assignment here would erase the one sentence that
        // says the walk did not finish.
        snap.diagnostic += std::to_string(snap.packs.size()) + " skin pack(s) from " +
                           std::to_string(snap.filesSeen) + " files";
        if (loose != 0) {
            snap.diagnostic += ", " + std::to_string(loose) + " loose at the root and ignored";
        }
        if (duplicates != 0) {
            snap.diagnostic += ", " + std::to_string(duplicates) +
                               " same-named file(s) inside one pack (the first found is used)";
        }
        if (unreadable != 0) {
            snap.diagnostic += ", " + std::to_string(unreadable) +
                               " entry/entries that could not be read and were skipped";
        }
        snap.diagnostic += ".";
        return snap;
    }

    void RequestScan() {
        const auto requested = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_active.fetch_add(1, std::memory_order_acq_rel);
        std::thread([requested]() {
            // ⚠⚠ THE BACKSTOP, AND IT MAY NEVER BE REMOVED. An exception that
            // escapes a std::thread's function calls std::terminate outright:
            // a CRT fail-fast no crash logger can see, out of reach of the
            // editor's draw guard because it is on another thread. ScanRoot is
            // hardened against the throws we know about; this is for the ones we
            // do not. A worker that dies must cost its own scan and nothing more.
            //
            // ⚠ THE ACTIVE COUNT IS DECREMENTED ON EVERY PATH, because Scanning()
            // feeds the page's spinner and a worker that left without
            // decrementing would leave it loading for the rest of the session.
            struct ActiveGuard {
                ~ActiveGuard() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
            } activeGuard;
            try {
                auto next       = ScanRoot("Data/textures");
                next.generation = requested;
                spdlog::info("SkinPacks: {}", next.diagnostic);
                for (const auto& pack : next.packs) {
                    spdlog::info("SkinPacks:   '{}' with {} file(s).", pack.id, pack.Count());
                }
                // Only the newest request may publish, or a slower earlier scan
                // replaces the one the page is already showing.
                if (g_generation.load(std::memory_order_acquire) == requested) {
                    g_snapshot.store(std::make_shared<const Snapshot>(std::move(next)),
                                     std::memory_order_release);
                }
            } catch (const std::exception& e) {
                spdlog::critical(
                    "SkinPacks: the skin pack scan threw and was abandoned rather than allowed "
                    "to end the process as a fail-fast with no crash log. The page shows the "
                    "previous scan's packs. The exception was: {}",
                    e.what() ? e.what() : "a std::exception with no message");
                if (const auto logger = spdlog::default_logger()) {
                    logger->flush();
                }
            } catch (...) {
                spdlog::critical(
                    "SkinPacks: the skin pack scan threw something that does not derive from "
                    "std::exception and was abandoned rather than allowed to end the process.");
                if (const auto logger = spdlog::default_logger()) {
                    logger->flush();
                }
            }
        }).detach();
    }

    std::shared_ptr<const Snapshot> Get() {
        return g_snapshot.load(std::memory_order_acquire);
    }

    // ⚠⚠ THIS EXISTS BECAUSE THE SCAN USED TO BE THE SKIN PAGE'S PRIVATE
    // BUSINESS. RequestScan had exactly one caller, the Overlays page opening,
    // so a session that imported a look before ever visiting that page asked
    // SkinApi for a pack against a snapshot that was still the empty one it was
    // constructed with. The pack was "not on this rig", the import's skin step
    // warned and returned, and the character kept the skin it already had with
    // nothing on screen to say why (field 2026-08-27, 04:04:17 refused
    // 'Sayble 4K' and the first scan published at 04:04:31).
    //
    // Synchronous, and deliberately so. The walk is one folder of this mod's
    // own convention (the field rig: 160 files, 23 packs), the caller is a step
    // in an apply that is already rebuilding a head, and the alternative is a
    // retry queue whose failure mode is the silent one we are here to remove.
    // A scan already in flight is left to finish and publish on its own; this
    // takes its own ticket, so whichever finishes last wins, which is the rule
    // RequestScan already plays by.
    std::shared_ptr<const Snapshot> EnsureScanned() {
        auto current = g_snapshot.load(std::memory_order_acquire);
        if (current && current->generation != 0) {
            return current;  // somebody has already published a real one
        }
        const auto requested = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto       next      = ScanRoot("Data/textures");
        next.generation      = requested;
        spdlog::info("SkinPacks: scanned on demand, no page had asked yet: {}", next.diagnostic);
        auto published = std::make_shared<const Snapshot>(std::move(next));
        if (g_generation.load(std::memory_order_acquire) == requested) {
            g_snapshot.store(published, std::memory_order_release);
        }
        return published;
    }

    bool Scanning() { return g_active.load(std::memory_order_acquire) != 0; }

    const SkinPlan::Pack* Find(const Snapshot& a_snap, std::string_view a_id) {
        for (const auto& pack : a_snap.packs) {
            if (pack.id == a_id) {
                return &pack;
            }
        }
        return nullptr;
    }

}  // namespace OS::SkinPacks
