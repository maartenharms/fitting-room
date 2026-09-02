#include "OverlayTextures.h"

#include "OverlayLocations.h"
#include "OverlayPlan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace OS::OverlayTextures {

    namespace {

        std::atomic<std::uint64_t>            g_generation{ 0 };
        std::atomic<int>                      g_active{ 0 };
        std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{
            std::make_shared<const Snapshot>()
        };

        // Where overlay art lives, relative to the textures root. skee's own
        // default texture sits directly under it.
        constexpr const char* kOverlaysDir = "actors/character/overlays";

        // ⚠⚠ AND THE SECOND ROOT, WHICH IS A DIFFERENT LIBRARY. The game's own
        // tint masks live here and the Makeup sections are what they are for.
        // Scanning only the first is why the field could not find the dirt art:
        // FemaleHeadDirt_01.dds through _03.dds have never been under overlays\.
        constexpr const char* kTintMasksDir = "actors/character/character assets/tintmasks";

        // What the archive half of the backfill cost, so the log can price it
        // rather than leave it to be guessed at the next report.
        struct ArchiveProbe {
            std::size_t asked = 0;  // rows the filesystem could not answer
            std::size_t found = 0;  // of those, rows an archive holds
            double      ms    = 0.0;
        };

        // Can the GAME open this texture? Loose file or archive, either counts.
        //
        // ⚠⚠ THE FILESYSTEM ANSWERS FIRST AND THE ARCHIVE ONLY IF IT SAID NO.
        // A loose hit costs one stat call and is the common case on this rig
        // (2038 of 2583 rows), so the expensive question is asked about the
        // remainder alone. It is asked at all because std::filesystem cannot
        // see into a BSA and community overlay packs ship inside one.
        //
        // ⚠ BSResourceNiBinaryStream IS THE SAME DOOR THE OVERRIDE WILL USE.
        // HeadPart::ModelResolves settled this for meshes in the same words:
        // it is the path the engine loads through, so it sees loose files and
        // archives alike and answers the question the renderer will ask. The
        // path it wants is Data-relative, and the table's keys are relative to
        // textures\, hence the prefix.
        //
        // ⚠ COST IS MEASURED, NOT ASSUMED. The mesh version of this froze the
        // game for five seconds on its first open because it ran 882 distinct
        // resolves on the main thread. This one runs on the scan thread, over
        // the rows the walk did not already cover, and reports its own numbers.
        [[nodiscard]] bool TextureResolves(const std::filesystem::path& a_texturesRoot,
                                           const std::string& a_path, ArchiveProbe& a_probe) {
            std::error_code ec;
            if (std::filesystem::exists(a_texturesRoot / a_path, ec) && !ec) {
                return true;
            }
            const auto  start = std::chrono::steady_clock::now();
            std::string full  = "textures\\" + a_path;
            RE::BSResourceNiBinaryStream stream{ full.c_str() };
            const bool                   ok = stream.good();
            a_probe.ms += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
            ++a_probe.asked;
            if (ok) {
                ++a_probe.found;
            }
            return ok;
        }

    }  // namespace

    Snapshot ScanRoot(const std::filesystem::path& a_texturesRoot) {
        Snapshot   snap;
        const auto root = a_texturesRoot / kOverlaysDir;

        // ⚠ A MISSING ROOT IS A NOTE, NOT A RETURN, and that is the change the
        // second library forced. An install with tint masks but no overlays
        // folder used to scan as nothing at all.
        std::vector<std::string> missing;

        // Entries no walk below could read. Counted rather than fatal; the
        // ⚠⚠ on the loop says why that distinction was worth a release.
        std::size_t unreadable = 0;

        const auto walk = [&](const std::filesystem::path& a_root, bool a_tintMasks) {
            std::error_code ec;
            if (!std::filesystem::exists(a_root, ec) || ec) {
                missing.push_back(a_root.string());
                return;
            }

            std::filesystem::recursive_directory_iterator it{
                a_root, std::filesystem::directory_options::skip_permission_denied, ec
            };
            if (ec) {
                snap.diagnostic += "could not walk " + a_root.string() + ": " + ec.message() + " ";
                return;
            }

            // One entry's work, as a call so every early exit below is a
            // return this loop can catch around rather than a continue that
            // would have to remember to advance the iterator itself.
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

                // ⚠ NOT std::filesystem::relative. See the ⚠⚠ on
                // OverlayPlan::FromScanPath: under a mod manager the file does
                // not live under the directory that was walked, and the
                // relative path walks out of it into the mod staging folder.
                auto overridePath = OverlayPlan::FromScanPath(file.string());
                if (overridePath.empty()) {
                    return;
                }

                // ⚠ THE MAPS ARE COUNTED, NOT JUST DROPPED. A pack whose art is
                // all suffixed would otherwise scan as empty and read as
                // "nothing installed", which is a very different thing from
                // "everything here was a normal map".
                if (OverlayPlan::IsMapTexture(overridePath)) {
                    ++snap.mapsSkipped;
                    return;
                }

                Entry item;
                item.name   = OverlayPlan::DisplayName(overridePath);
                item.folder = OverlayPlan::PackFolder(overridePath);
                // ⚠ THE FLAG COMES FROM THE ROOT THAT WAS WALKED, not from the
                // path. They agree today and the root is the authority: it is
                // the thing that decided this file is in the tint mask library
                // at all.
                item.tintMask  = a_tintMasks;
                item.locations = OverlayLocations::For(overridePath);
                // ⚠ Registered, NOT For. The resolved answer has already
                // dropped the warpaint bit by design, so this has to be asked of
                // the table itself. See the note on Entry::warpaint.
                item.registered = OverlayLocations::Registered(overridePath);
                item.warpaint   = OverlayLocations::IsWarpaint(item.registered);
                item.path       = std::move(overridePath);
                if (a_tintMasks) {
                    ++snap.tintMasks;
                }
                snap.entries.push_back(std::move(item));
            };

            // ⚠⚠ THE ec ABOVE COVERS THE CONSTRUCTOR AND NOTHING ELSE, AND THAT
            // OMISSION WAS A SILENT PROCESS KILL. A range-for over this iterator
            // advances with operator++, the THROWING overload, and
            // skip_permission_denied excuses permission errors and no others: a
            // path over MAX_PATH, a broken junction, an IO error, or a name
            // path::string() cannot convert all raise filesystem_error from the
            // increment. This walk runs on a DETACHED WORKER (RequestScan
            // below), and an exception escaping a std::thread's function reaches
            // std::terminate unconditionally. That is a CRT fail-fast: it skips
            // the unhandled-exception filter Crash Logger and Trainwreck hook,
            // so there is no crash log, and the editor's own draw guard cannot
            // see it because it is on another thread. "Clicking the overlays
            // section crashes my game, with no crash log", reported through
            // August 2026 and still live in 1.1.5, whose fix guarded a JSON
            // parse on the main thread instead. WHOSE tree trips it is per-rig,
            // which is exactly why it hits some players and nobody else.
            //
            // Narrowest containment first: one unreadable FILE is skipped, one
            // unreadable DIRECTORY ends this walk with everything found so far
            // kept, and RequestScan carries a backstop under both. None of the
            // three may cost the process.
            const std::filesystem::recursive_directory_iterator end{};
            while (it != end) {
                try {
                    take(*it);
                } catch (const std::exception& e) {
                    ++unreadable;
                    spdlog::warn("OverlayTextures: skipped an entry under {} that could not "
                                 "be read: {}", a_root.string(), e.what());
                } catch (...) {
                    ++unreadable;
                    spdlog::warn("OverlayTextures: skipped an entry under {} that threw "
                                 "something that is not a std::exception.", a_root.string());
                }
                // ⚠ increment(ec) IS THE NON-THROWING OVERLOAD AND IT IS THE
                // WHOLE POINT. On failure the standard leaves the iterator at
                // end(), so the walk cannot be resumed and stopping with what we
                // have is the honest outcome; it is still infinitely better than
                // the throw, which took the process.
                std::error_code stepEc;
                it.increment(stepEc);
                if (stepEc) {
                    snap.diagnostic += "the walk of " + a_root.string() +
                                       " stopped early: " + stepEc.message() + " ";
                    break;
                }
            }
        };

        walk(root, false);
        walk(a_texturesRoot / kTintMasksDir, true);

        // ⚠⚠ THE TABLE KNOWS ART THE ROOTS CANNOT SEE. 103 of the reference
        // table's rows live under textures\!ube\..., outside both walked
        // roots, and they include the ONLY feet-only art installed (UBE's
        // toenail overlays) and 26 UBE warpaints. The field report "the feet
        // grid is body art" was half the fallback's lopsided counts and half
        // this: the art that would have topped the feet grid was never
        // scanned. So every registered path that was not walked is asked about
        // and added if the GAME can open it.
        //
        // ⚠⚠ AND "CAN THE GAME OPEN IT" IS NOT std::filesystem::exists, WHICH
        // IS WHAT THIS COMMENT USED TO CLAIM. It said the check "runs under the
        // game's own VFS, which is the same view the override will resolve the
        // path through". That is true of usvfs and loose files and FALSE of
        // archives: nothing in std::filesystem can see inside a BSA. Measured
        // on the user's load order 2026-08-19, where the scrape found 2583 rows
        // and the picker offered 2021 of them:
        //
        //   UBE 2.0 Community Overlay 3 (51- 70)  CommunityOverlays3 for UBE
        //                                         2.0.bsa, 0 loose, absent
        //   Community Overlays 3 - Main           CommunityOverlays3.bsa,
        //                                         0 loose, absent
        //   Community Overlays 2 UBE RaceMenu     101 loose files, present
        //
        // The walk cannot enumerate an archive either, so for BSA art this
        // backfill is the ONLY route in and the existence check was closing it
        // ("I am missing a lot of overlays ... overlays like '56 Body'").
        std::unordered_set<std::string> walked;
        walked.reserve(snap.entries.size() * 2);
        for (const auto& entry : snap.entries) {
            walked.insert(OverlayLocations::Key(entry.path));
        }
        std::size_t  seeded = 0;
        ArchiveProbe archives;
        for (const auto& path : OverlayLocations::TablePaths()) {
            if (walked.contains(path)) {
                continue;
            }
            if (OverlayPlan::IsMapTexture(path)) {
                continue;
            }
            if (!TextureResolves(a_texturesRoot, path, archives)) {
                continue;
            }
            ++snap.filesSeen;
            Entry item;
            item.name   = OverlayPlan::DisplayName(path);
            item.folder = OverlayPlan::PackFolder(path);
            if (item.folder.empty()) {
                // Art outside both conventional trees has no pack level to
                // take, so the path itself has to name the group.
                //
                // ⚠⚠ THE TOP FOLDER LEADS, AND THAT IS THE HALF THIS USED TO
                // DROP. It took the file's own parent alone, which reads
                // beautifully for "nail overlays" and disastrously for a pack
                // laid out by subject: Veilguard Overlays ships 135 registered
                // textures under textures\veilguard\{blush,eye,lips,scar}, so
                // the picker offered all of them under four generic headings
                // and NOTHING anywhere said Veilguard. The art was present the
                // whole time and unfindable among 2571 entries (user
                // 2026-08-26: "i do not see them in FR").
                //
                // ⚠ BOTH PARTS WHEN THEY DIFFER, because each answers a
                // different question: the top folder is the mod you installed,
                // the leaf is what the art is. "veilguard / blush" and
                // "!ube / nail overlays" both read as their pack and their
                // subject, and a pack that is only one level deep still reads
                // as itself rather than as itself twice.
                const auto cut = path.find_last_of('\\');
                if (cut != std::string::npos) {
                    const auto dir  = path.substr(0, cut);
                    const auto up   = dir.find_last_of('\\');
                    const auto leaf = up == std::string::npos ? dir : dir.substr(up + 1);
                    const auto firstCut = dir.find('\\');
                    const auto top =
                        firstCut == std::string::npos ? dir : dir.substr(0, firstCut);
                    item.folder = (top == leaf) ? leaf : top + " / " + leaf;
                }
            }
            item.tintMask   = OverlayPlan::IsTintMaskPath(path);
            item.locations  = OverlayLocations::For(path);
            item.registered = OverlayLocations::Registered(path);
            item.warpaint   = OverlayLocations::IsWarpaint(item.registered);
            item.path       = path;
            if (item.tintMask) {
                ++snap.tintMasks;
            }
            snap.entries.push_back(std::move(item));
            ++seeded;
        }
        if (seeded != 0) {
            snap.diagnostic += std::to_string(seeded) + " registered files outside the "
                               "scanned roots added from the table. ";
        }
        // The archive half of that, priced. It is the number to compare against
        // if this scan is ever suspected of costing the page its first open.
        if (archives.asked != 0) {
            snap.diagnostic += std::to_string(archives.found) + " of " +
                               std::to_string(archives.asked) +
                               " row(s) the filesystem could not see were found in an "
                               "archive, in " +
                               std::to_string(static_cast<int>(archives.ms + 0.5)) + " ms. ";
        }

        if (snap.entries.empty() && !missing.empty()) {
            snap.diagnostic += "no art at " + missing.front();
            return snap;
        }

        // ⚠ A SECOND PASS, BECAUSE A TWIN CAN BE FOUND IN ANY ORDER. The mask
        // and the art it belongs to are two directory entries and the walk
        // gives no guarantee about which arrives first, so the question can
        // only be asked once every path is known.
        std::unordered_set<std::string> present;
        present.reserve(snap.entries.size() * 2);
        for (const auto& entry : snap.entries) {
            present.insert(OverlayLocations::Key(entry.path));
        }
        for (auto& entry : snap.entries) {
            // ⚠ NEVER IN THE TINT MASK LIBRARY. The twin rule hides a tint mask
            // that is sitting beside real overlay art, which is exactly the
            // wrong thing to do to a file whose whole library is tint masks.
            if (entry.tintMask) {
                continue;
            }
            const auto twin = OverlayLocations::MaskTwinOf(entry.path);
            if (twin.empty() || !present.contains(twin)) {
                continue;
            }
            if (OverlayLocations::MaskTwinIsRedundant(entry.path, twin)) {
                entry.maskTwin = true;
                ++snap.masksHidden;
            }
        }

        // ⚠ THE LIBRARY IS THE FIRST KEY. Each picker draws one of the two and
        // builds its groups by walking a sorted list looking for the folder to
        // change, so keeping a library contiguous keeps a pack from appearing as
        // two headings with the same name.
        std::sort(snap.entries.begin(), snap.entries.end(),
                  [](const Entry& a_lhs, const Entry& a_rhs) {
                      if (a_lhs.tintMask != a_rhs.tintMask) {
                          return !a_lhs.tintMask;
                      }
                      if (a_lhs.folder != a_rhs.folder) {
                          return a_lhs.folder < a_rhs.folder;
                      }
                      return a_lhs.name < a_rhs.name;
                  });

        snap.diagnostic += std::to_string(snap.entries.size() - snap.tintMasks) +
                           " overlay textures and " + std::to_string(snap.tintMasks) +
                           " tint masks from " + std::to_string(snap.filesSeen) + " files (" +
                           std::to_string(snap.mapsSkipped) + " maps skipped, " +
                           std::to_string(snap.masksHidden) +
                           " warpaint mask copies of art already listed).";
        if (unreadable != 0) {
            snap.diagnostic +=
                " " + std::to_string(unreadable) + " entry/entries could not be read and were "
                "skipped.";
        }
        return snap;
    }

    void RequestScan() {
        const auto requested = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_active.fetch_add(1, std::memory_order_acq_rel);
        std::thread([requested]() {
            // ⚠⚠ THE BACKSTOP, AND IT MAY NEVER BE REMOVED. An exception that
            // escapes a std::thread's function calls std::terminate outright,
            // with no unwinding and no filter: a CRT fail-fast that no crash
            // logger can see and that the editor's draw guard cannot reach
            // across threads. ScanRoot is hardened against the throws we know
            // about, and this is here for the ones we do not. A worker that
            // dies must cost its own scan and nothing else.
            //
            // ⚠ THE ACTIVE COUNT IS DECREMENTED ON EVERY PATH. Scanning() feeds
            // the page's spinner, so a worker that left without decrementing
            // would leave the overlays page loading for the rest of the session.
            struct ActiveGuard {
                ~ActiveGuard() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
            } activeGuard;
            try {
                auto next       = ScanRoot("Data/textures");
                next.generation = requested;
                spdlog::info("OverlayTextures: {}", next.diagnostic);
                // Only the newest request may publish, or a slower earlier scan
                // replaces the one the page is already showing.
                if (g_generation.load(std::memory_order_acquire) == requested) {
                    g_snapshot.store(std::make_shared<const Snapshot>(std::move(next)),
                                     std::memory_order_release);
                }
            } catch (const std::exception& e) {
                spdlog::critical(
                    "OverlayTextures: the texture scan threw and was abandoned rather than "
                    "allowed to end the process as a fail-fast with no crash log. The page "
                    "shows the previous scan's art. The exception was: {}",
                    e.what() ? e.what() : "a std::exception with no message");
                if (const auto logger = spdlog::default_logger()) {
                    logger->flush();
                }
            } catch (...) {
                spdlog::critical(
                    "OverlayTextures: the texture scan threw something that does not derive "
                    "from std::exception and was abandoned rather than allowed to end the "
                    "process.");
                if (const auto logger = spdlog::default_logger()) {
                    logger->flush();
                }
            }
        }).detach();
    }

    std::shared_ptr<const Snapshot> Get() {
        return g_snapshot.load(std::memory_order_acquire);
    }

    bool Scanning() { return g_active.load(std::memory_order_acquire) != 0; }

}  // namespace OS::OverlayTextures
