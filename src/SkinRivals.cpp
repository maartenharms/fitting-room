#include "SkinRivals.h"

#include "WorkerGuard.h"  // no worker body may reach terminate
#include "SkinApi.h"     // OpenablePath / RealPathOf: the one owner of the path edge
#include "SkinImport.h"  // OwnerOf / ModsRootOf / CandidatePath / LinkTarget
#include "SkinPlan.h"    // Lower, and kSkinsDir so one folder has one spelling

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

// ⚠ EXPLICIT, because the PCH does not reach here. SkinApi.cpp calls
// CreateFileW without this include only because SkinApi.h pulls the engine
// headers that carry it; this file includes none of those and the compiler said
// so. WIN32_LEAN_AND_MEAN and NOMINMAX are set on the command line by
// CMakeLists, so there is no header ordering to get wrong.
#include <Windows.h>

namespace OS::SkinRivals {

    namespace {

        std::atomic<std::uint64_t>                   g_generation{ 0 };
        std::atomic<int>                             g_active{ 0 };
        std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{
            std::make_shared<const Snapshot>()
        };

        std::mutex                      g_linkedLock;
        // ⚠⚠ THE COUNT, NOT JUST THE NAME, and the difference is a whole
        // failure shape. A mod fully linked in an EARLIER session and a mod that
        // gained files ten minutes ago are one entry in a set and two entirely
        // different things on screen: the second is HALF VISIBLE, because the pack
        // is in the virtual tree from last launch and the files added since are
        // not. FIELD 2026-08-26: the body swapped and the face did not, on packs
        // whose head files were linked twelve minutes into the session.
        std::map<std::string, std::size_t> g_linked;  // lower mod -> links MADE here

        // The skins folder as something CreateFile can open. SkinPlan::kSkinsDir
        // is "FittingRoom\skins\" in the form an override wants; this is the
        // same folder in the form a handle wants.
        [[nodiscard]] std::string SkinsDirReal() {
            std::string virt{ SkinPlan::kSkinsDir };
            while (!virt.empty() && (virt.back() == '\\' || virt.back() == '/')) {
                virt.pop_back();
            }
            // ⚠ RESOLVED, NOT NAMED. Whichever mod supplies this folder is where
            // links belong, and on this rig that is the base "Fitting Room" mod
            // that redeploy keeps enabled below every branch slot. Asking the
            // handle means the feature follows the install instead of carrying a
            // mod name it would eventually be wrong about.
            return SkinApi::RealPathOf(SkinApi::OpenablePath("textures\\" + virt), true);
        }

        [[nodiscard]] std::string LastErrorText(DWORD a_err) {
            switch (a_err) {
                case ERROR_NOT_SAME_DEVICE:
                    return "the skin and this mod are on different drives, so a hard "
                           "link is impossible";
                case ERROR_ACCESS_DENIED:
                    return "access denied writing into the skins folder";
                case ERROR_INVALID_FUNCTION:
                    return "the filesystem does not support hard links (not NTFS)";
                case ERROR_PATH_NOT_FOUND:
                    return "the destination folder could not be made";
                default:
                    return "Windows error " + std::to_string(a_err);
            }
        }

        [[nodiscard]] std::wstring Wide(const std::string& a_utf8) {
            return std::filesystem::path{
                std::u8string_view{ reinterpret_cast<const char8_t*>(a_utf8.data()),
                                    a_utf8.size() }
            }
                .wstring();
        }

        Snapshot ScanNow(const std::vector<std::string>& a_virtualPaths,
                         const std::string&              a_bodyDiffuse) {
            Snapshot snap;

            // ---- half one: who really owns what this character is wearing ----
            std::unordered_set<std::string> winners;   // lower-cased mod names
            std::vector<std::string>        winnerNames;  // the same, as spelled
            std::vector<std::string>        relatives;  // distinct, original spelling
            std::unordered_set<std::string> seenRel;
            std::size_t                     resolved = 0;

            for (const auto& virt : a_virtualPaths) {
                const auto real = SkinApi::RealPathOf(SkinApi::OpenablePath(virt), false);
                if (real.empty()) {
                    continue;  // a texture that will not open names no mod
                }
                ++resolved;
                const auto owner = SkinImport::OwnerOf(real);
                if (!owner.Ok()) {
                    continue;
                }
                if (snap.modsRoot.empty()) {
                    snap.modsRoot = SkinImport::ModsRootOf(real);
                }
                if (winners.insert(SkinPlan::Lower(owner.mod)).second) {
                    winnerNames.push_back(owner.mod);
                }
                if (seenRel.insert(SkinPlan::Lower(owner.relative)).second) {
                    relatives.push_back(owner.relative);
                }
            }

            // ⚠⚠ THE INPUTS, SPELLED OUT, AND THE 2026-08-26 FIELD ROUND IS
            // WHY. The first build logged only counts, so when five installed
            // UBE skins failed to appear the log could not say whether their
            // shared path was never collected or was collected and probed to
            // nothing. An absent line is a reading only if the path was built
            // to speak, and this one was not.
            for (const auto& name : winnerNames) {
                spdlog::info("SkinRivals: winner '{}' supplies part of this skin, so it is "
                             "not offered as a rival.",
                             name);
            }
            for (const auto& rel : relatives) {
                spdlog::info("SkinRivals: matching against '{}'.", rel);
            }

            // ⛔ THE ONE PATH THAT DECIDES WHAT COUNTS AS A SKIN. A mod that
            // does not replace the texture the body's own diffuse slot reads is
            // not a skin, whatever else it happens to supply.
            std::string keyRelative;
            if (!a_bodyDiffuse.empty()) {
                const auto real =
                    SkinApi::RealPathOf(SkinApi::OpenablePath(a_bodyDiffuse), false);
                const auto owner = SkinImport::OwnerOf(real);
                if (owner.Ok()) {
                    keyRelative = SkinPlan::Lower(owner.relative);
                }
            }

            // ⚠⚠ THIS MOD'S OWN FOLDER IS NOT A RIVAL. Linking a skin puts hard
            // links under whichever mod supplies textures\FittingRoom\skins,
            // so after one link that mod carries the body diffuse and the scan
            // would offer Fitting Room as a skin to import into itself. The
            // field saw exactly that card.
            std::string ownMod;
            if (const auto skins = SkinsDirReal(); !skins.empty()) {
                ownMod = SkinPlan::Lower(SkinImport::OwnerOf(skins).mod);
            }

            if (snap.modsRoot.empty()) {
                // ⚠ THE HONEST ANSWER, not an empty list. Vortex, a manual
                // install, or a character whose skin is entirely in archives.
                snap.diagnostic =
                    "no mods folder above this character's skin textures (" +
                    std::to_string(resolved) + " of " + std::to_string(a_virtualPaths.size()) +
                    " opened), so the rival skins cannot be found. This needs Mod Organizer.";
                return snap;
            }

            // ---- half two: which siblings carry the same files ----
            std::error_code ec;
            std::filesystem::directory_iterator it{
                snap.modsRoot, std::filesystem::directory_options::skip_permission_denied, ec
            };
            if (ec) {
                snap.diagnostic = "could not read " + snap.modsRoot + ": " + ec.message();
                snap.modsRoot.clear();  // unusable, and Usable() has to say so
                return snap;
            }

            if (keyRelative.empty()) {
                snap.diagnostic =
                    "this character's body diffuse could not be traced to a mod, so there "
                    "is no way to tell a skin from a hair or eye texture. No rivals listed.";
                snap.modsRoot.clear();  // Usable() has to say so
                return snap;
            }
            // ⚠⚠ TWO KEYS, NOT ONE, AND THE SECOND IS THE OTHER SEX'S BODY
            // DIFFUSE. See SkinPlan::SexSiblingOf for the field case: a skin
            // whose futa half ships as a SEPARATE mod carries only
            // malebody_1_d.dds, which is a body diffuse of this character and
            // was thrown away for not being the one the body itself reads.
            // ⚠ Empty when the anchor's name carries no sex, and an empty key
            // matches nothing, so a body named neither way behaves as before.
            const auto keySibling = SkinPlan::SexSiblingOf(keyRelative);
            if (keySibling.empty()) {
                spdlog::info("SkinRivals: a mod counts as a skin only if it carries '{}'.",
                             keyRelative);
            } else {
                spdlog::info("SkinRivals: a mod counts as a skin if it carries '{}' or the "
                             "other sex's body diffuse '{}'. A skin that ships its futa "
                             "half as a separate mod carries only the second.",
                             keyRelative, keySibling);
            }

            std::size_t looked        = 0;
            std::size_t withTextures  = 0;
            std::size_t carriedSome   = 0;
            for (const auto& entry : it) {
                std::error_code dirEc;
                if (!entry.is_directory(dirEc) || dirEc) {
                    continue;
                }
                ++looked;
                const auto name = entry.path().filename().string();
                // ⛔ NO WINNER EXCLUSION HERE, AND THE FIELD IS WHY (2026-08-26:
                // "there are so many skins we are missing the list"). This used
                // to drop any mod that owned even ONE of the paths above, which
                // sounds right and is badly wrong on a real load order: ten UBE
                // skins ship overlapping but different file sets, so several of
                // them win a file or two and were dropped WHOLE, taking the
                // other two dozen files they were competing for with them.
                // '[LRO] SKIN v3 final' ships four files; winning those four
                // erased it from the list entirely.
                //
                // ⚠ THE ONE THAT MUST NOT APPEAR TWICE IS THE BASE SKIN'S OWN
                // MOD, and the page is where that is known: the Base Skin card
                // is named from SkinApi::Fit's defaultName. Filtering it here
                // would mean this file guessing which of several winners the
                // card ended up named after.
                // ⚠ ONE STAT BEFORE TWENTY-SIX. Most installed mods ship no
                // textures at all, and probing every skin path inside each of
                // them is a hundred thousand filesystem calls for an answer a
                // single directory test settles.
                //
                // ⚠⚠ IT IS ALSO A MEASUREMENT. If this count comes back absurdly
                // low on a load order this size, the probe itself is what is
                // wrong rather than the mods, and the counts below would have
                // hidden that behind an honest-looking zero.
                std::error_code texEc;
                if (!std::filesystem::exists(entry.path() / "textures", texEc) || texEc) {
                    continue;
                }
                ++withTextures;
                if (!ownMod.empty() && SkinPlan::Lower(name) == ownMod) {
                    continue;  // our own skins folder lives here
                }
                Rival rival;
                rival.mod = name;
                bool carriesBody = false;
                for (const auto& rel : relatives) {
                    const auto candidate =
                        SkinImport::CandidatePath(snap.modsRoot, name, rel);
                    std::error_code fileEc;
                    if (std::filesystem::exists(candidate, fileEc) && !fileEc) {
                        rival.relatives.push_back(rel);
                        const auto lowered = SkinPlan::Lower(rel);
                        if (lowered == keyRelative ||
                            (!keySibling.empty() && lowered == keySibling)) {
                            carriesBody = true;
                        }
                    }
                }
                if (!rival.relatives.empty()) {
                    ++carriedSome;
                }
                // ⛔ EVERY SHARED FILE COMES ALONG, but only a mod that replaces
                // a BODY DIFFUSE is offered. Linking a skin should bring its
                // whole set, and a mod that shares one cubemap is not a skin.
                // ⚠ "a" rather than "the" since the sibling key: a mod carrying
                // only the other sex's body diffuse is a partial skin, which
                // Fits() already says is still a pack.
                if (carriesBody) {
                    snap.rivals.push_back(std::move(rival));
                }
            }

            std::sort(snap.rivals.begin(), snap.rivals.end(),
                      [](const Rival& a_lhs, const Rival& a_rhs) {
                          return SkinPlan::Lower(a_lhs.mod) < SkinPlan::Lower(a_rhs.mod);
                      });

            snap.diagnostic = std::to_string(snap.rivals.size()) + " installed skin mod(s) " +
                              "overridden on this character, from " +
                              std::to_string(looked) + " mod folder(s) (" +
                              std::to_string(withTextures) + " with a textures folder, " +
                              std::to_string(carriedSome) +
                              " sharing any file, the rest not replacing the body) against " +
                              std::to_string(relatives.size()) + " skin texture path(s)";
            if (snap.rivals.empty()) {
                snap.diagnostic += ". Nothing is competing for this character's skin";
            }
            snap.diagnostic += ".";
            return snap;
        }

    }  // namespace

    void RequestScan(std::vector<std::string> a_virtualPaths, std::string a_bodyDiffuse) {
        const auto requested = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_active.fetch_add(1, std::memory_order_acq_rel);
        std::thread([requested, paths = std::move(a_virtualPaths),
                     body = std::move(a_bodyDiffuse)]() {
          // ⚠ THE ACTIVE COUNT IS A DESTRUCTOR'S JOB NOW. Scanning() feeds this
          // page's spinner, so a worker that left early without decrementing
          // would leave it loading for the rest of the session.
          struct ActiveGuard {
              ~ActiveGuard() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
          } activeGuard;
          WorkerGuard::Run("SkinRivals", [&] {
            auto next       = ScanNow(paths, body);
            next.generation = requested;
            spdlog::info("SkinRivals: {}", next.diagnostic);
            for (const auto& rival : next.rivals) {
                spdlog::info("SkinRivals:   '{}' carries {} of this character's skin file(s).",
                             rival.mod, rival.relatives.size());
            }
            // Only the newest request may publish, or a slower earlier scan
            // replaces the one the page is already showing.
            if (g_generation.load(std::memory_order_acquire) == requested) {
                g_snapshot.store(std::make_shared<const Snapshot>(std::move(next)),
                                 std::memory_order_release);

                // ⚠⚠ LINKED WITHOUT BEING ASKED, AND THE FIELD ASKED FOR THAT
                // (user 2026-08-26: "the click to link is annoying, we should
                // just have a symbol ... and we link automatically"). There was
                // never a decision behind the click: every rival on the grid is
                // a skin the player installed and wants to try, the link costs
                // no disk because it is a hard link, and it is undone by
                // deleting one folder.
                //
                // ⛔ SAFE ONLY BECAUSE THE FILTER IS NARROW NOW. The first cut
                // offered any mod sharing any texture path, and auto-linking
                // THAT would have poured cubemaps and hairlines into the skins
                // folder unasked. Nothing here may loosen without this being
                // reconsidered.
                //
                // ⚠ AFTER THE PUBLISH, because Link reads the published
                // snapshot to find the mod's file list.
                const auto published = Get();
                if (published) {
                    std::size_t linked = 0;
                    for (const auto& rival : published->rivals) {
                        if (Linked(rival.mod)) {
                            continue;
                        }
                        std::string why;
                        if (Link(rival.mod, why) != 0) {
                            ++linked;
                        }
                    }
                    if (linked != 0) {
                        spdlog::info("SkinRivals: linked {} skin(s) automatically. They "
                                     "appear after the next restart, not before it.",
                                     linked);
                    }
                }
            }
          });
        }).detach();
    }

    std::shared_ptr<const Snapshot> Get() {
        return g_snapshot.load(std::memory_order_acquire);
    }

    bool Scanning() { return g_active.load(std::memory_order_acquire) != 0; }

    bool Linked(std::string_view a_mod) {
        std::scoped_lock l(g_linkedLock);
        return g_linked.contains(SkinPlan::Lower(a_mod));
    }

    std::size_t FreshLinks(std::string_view a_mod) {
        std::scoped_lock l(g_linkedLock);
        const auto       it = g_linked.find(SkinPlan::Lower(a_mod));
        return it == g_linked.end() ? 0 : it->second;
    }

    std::size_t Link(std::string_view a_mod, std::string& a_why) {
        a_why.clear();
        const auto snap = Get();
        if (!snap || !snap->Usable()) {
            a_why = "no mods folder was found, so there is nothing to link from";
            return 0;
        }
        const Rival* rival = nullptr;
        for (const auto& candidate : snap->rivals) {
            if (SkinPlan::Lower(candidate.mod) == SkinPlan::Lower(a_mod)) {
                rival = &candidate;
                break;
            }
        }
        if (!rival) {
            a_why = "that mod is not in the current scan";
            return 0;
        }

        const auto skins = SkinsDirReal();
        if (skins.empty()) {
            a_why = "the skins folder could not be opened, so its real location is unknown";
            return 0;
        }

        std::size_t made    = 0;
        std::size_t already = 0;
        DWORD       lastErr = 0;
        for (const auto& rel : rival->relatives) {
            const auto from = SkinImport::CandidatePath(snap->modsRoot, rival->mod, rel);
            const auto to   = SkinImport::LinkTarget(skins, rival->mod, rel);

            std::error_code ec;
            std::filesystem::create_directories(
                std::filesystem::path{ Wide(to) }.parent_path(), ec);
            if (ec) {
                lastErr = ERROR_PATH_NOT_FOUND;
                continue;
            }
            if (::CreateHardLinkW(Wide(to).c_str(), Wide(from).c_str(), nullptr)) {
                ++made;
                continue;
            }
            const auto err = ::GetLastError();
            if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
                ++already;  // a previous session linked it; that is a success
                continue;
            }
            lastErr = err;
        }

        if (made == 0 && already == 0) {
            a_why = LastErrorText(lastErr);
            spdlog::warn("SkinRivals: linking '{}' made nothing: {}.", rival->mod, a_why);
            return 0;
        }
        {
            std::scoped_lock l(g_linkedLock);
            // ⚠ ACCUMULATED, NEVER ASSIGNED. A second Link for the same mod finds
            // every file already there and would reset the count to zero, erasing
            // the restart the first call earned.
            g_linked[SkinPlan::Lower(rival->mod)] += made;
        }
        spdlog::info("SkinRivals: linked '{}': {} new hard link(s), {} already there, out of "
                     "{} file(s). ⚠ usvfs built its tree at launch, so this skin appears "
                     "after the next restart and not before it.",
                     rival->mod, made, already, rival->relatives.size());
        return made + already;
    }

}  // namespace OS::SkinRivals
