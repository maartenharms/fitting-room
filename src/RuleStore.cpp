#include "PCH.h"

#include "RuleStore.h"

#include "BuildChannel.h"
#include "Persistence.h"
#include "RuleCodec.h"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <vector>

namespace OS::RuleStore {

    using namespace OS::Rules;

    namespace {
        const auto kRootDir  = BuildChannel::DataRoot();
        const auto kSeedPath = BuildChannel::DataPath("rules.json");
        const auto kPacksDir = BuildChannel::DataPath("Rules");
        // Sanity cap, same reasoning as PresetStore's kMaxPresetBytes: a
        // hand-written rule pack is a few KB even with dozens of rules;
        // anything bigger is not one of ours and must not stall the scan.
        constexpr std::uintmax_t kMaxPackBytes = 256 * 1024;

        std::string Lower(std::string a_s) {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char a_c) {
                return static_cast<char>(std::tolower(a_c));
            });
            return a_s;
        }

        // The save's own rules. Guarded: EditorUI::Draw runs on the render
        // thread (ImGuiOverlay::PresentThunk), while SaveMirrorNow reads this
        // from an SKSE task and Merged() reads it from the WorldWatch
        // heartbeat. A vector of Rule (strings, maps) can reallocate under a
        // concurrent copy, so - unlike Settings' scalar members - a torn read
        // here is a use-after-free, not a stale value. Reach it only through
        // WithRules/Snapshot below, never directly.
        std::mutex g_currentLock;
        RuleSet    g_current;

        // Whether a save currently owns g_current. False at startup (before
        // the first OnSaveLoaded/OnNewGame) and after OnRevert, which runs
        // before every load. QueueMirrorSave must refuse to run in that
        // window - see QueueMirrorSave's comment for what happens if it
        // doesn't.
        std::atomic<bool> g_saveActive{ false };

        // Author packs. g_authored is the as-scanned-from-disk state, set by
        // ScanPacks and left untouched between scans; g_packs is the working
        // copy SetPackRuleEnabled edits. OnSaveLoaded/OnNewGame/OnRevert reset
        // g_packs back to g_authored before the caller (Task 10) reapplies the
        // NEW save's disabled-id list - "disabled" is per-save state
        // (persisted in the 'RULE' record), so a toggle from one character
        // must not leak onto the next, the same leak Task 10 calls out for the
        // engine's pin. A rescan (ScanPacks called again) does the same reset,
        // since the freshly parsed files become the new authored baseline.
        std::mutex g_packsLock;
        RuleSet    g_authored;
        RuleSet    g_packs;

        // Pack-rule ids already logged as colliding with a save rule. Merged()
        // is the WorldWatch evaluation list - called every heartbeat, not just
        // on a state change - so logging the same collision on every call
        // would spam the log for as long as the collision exists. Cleared
        // whenever the id landscape can have changed: a rescan, or a
        // new/loaded/reverted save.
        std::set<std::string> g_loggedCollisions;

        std::atomic<bool> g_mirrorPending{ false };

        // The save's engine state (pin, engine on/off, the overlay
        // reconstruction anchor - see EngineState's own comment in
        // RuleStore.h). Guarded like g_current: small, but Task 10's load
        // path and a future WorldWatch reader/writer (Task 11) can touch it
        // from different threads.
        std::mutex  g_engineStateLock;
        EngineState g_engineState;

        // Caller must hold g_packsLock.
        void ResetPackOverridesLocked() {
            g_packs = g_authored;
            g_loggedCollisions.clear();
        }

        // The one shape a rules document has everywhere: rules.json, each
        // author pack, and (via Task 10) the 'RULE' co-save payload all read
        // { "version": 1, "rules": [...] } through RuleCodec::JsonToRules.
        bool ParseRulesDocument(const std::filesystem::path& a_path, RuleSet& a_out,
                                 std::string& a_error, std::vector<std::string>& a_warnings) {
            std::ifstream           in(a_path);
            Json::Value             root;
            Json::CharReaderBuilder rb;
            std::string             errs;
            if (!in || !Json::parseFromStream(rb, in, &root, &errs)) {
                a_error = errs.empty() ? "not valid JSON (open failed)" : errs;
                return false;
            }
            return RuleCodec::JsonToRules(root, a_out, a_error, &a_warnings);
        }

        // Preserves a rejected file as "<path>.bad" instead of leaving it to
        // be silently clobbered by the next save's mirror write - a hand-edit
        // typo in a shared seed would otherwise cost the author the file with
        // no way to recover what they wrote. Any previous .bad is dropped
        // first so the newest failure is the one kept.
        void RenameAside(const std::filesystem::path& a_path) {
            const std::string bad = a_path.string() + ".bad";
            std::error_code   ec;
            std::filesystem::remove(bad, ec);
            ec.clear();
            std::filesystem::rename(a_path, bad, ec);
            if (ec) {
                spdlog::error("RuleStore: could not preserve rejected '{}' as '{}' ({}); it may "
                              "still be overwritten by the next save.",
                              a_path.string(), bad, ec.message());
            } else {
                spdlog::warn("RuleStore: preserved the rejected file as '{}'.", bad);
            }
        }

        void SaveMirrorNow() {
            // RulesToJson already skips any rule with a non-empty packName
            // (RuleCodec.cpp), so a pack rule can never reach this file even
            // if it somehow ended up in g_current - but g_current itself is
            // never populated from g_packs/g_authored (only WithRules,
            // OnSaveLoaded, and LoadSeed's import path write to it), so that
            // filter is defense in depth, not the only guard.
            const auto      root = RuleCodec::RulesToJson(Snapshot());
            std::error_code ec;
            std::filesystem::create_directories(kRootDir, ec);
            const std::string tmp     = kSeedPath.string() + ".tmp";
            bool              writeOk = false;
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) {
                    spdlog::error("RuleStore: cannot write {}.", tmp);
                    return;
                }
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "  ";
                out << Json::writeString(wb, root);
                writeOk = out.good();
            }  // out closed here - required before remove()/rename() below on Windows
            if (!writeOk) {
                // A partial write (disk full, I/O error) must not become the
                // new rules.json: rename() would replace a good file with
                // truncated JSON. Drop the temp and leave the existing file
                // alone; the next mutation queues another attempt.
                spdlog::error(
                    "RuleStore: write to {} failed midway; rules.json left untouched.", tmp);
                std::error_code ec2;
                std::filesystem::remove(tmp, ec2);
                return;
            }
            std::filesystem::rename(tmp, kSeedPath, ec);
            if (ec) {  // cross-volume or locked: fall back to copy
                std::filesystem::copy_file(
                    tmp, kSeedPath, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp, ec);
            }
            spdlog::debug("RuleStore: rules.json saved ({} rule(s)).", root["rules"].size());
        }
    }  // namespace

    void WithRules(const std::function<void(RuleSet&)>& a_fn) {
        {
            std::scoped_lock l(g_currentLock);
            a_fn(g_current);
        }
        QueueMirrorSave();
    }

    RuleSet Snapshot() {
        std::scoped_lock l(g_currentLock);
        return g_current;
    }

    RuleSet Merged() {
        RuleSet                out = Snapshot();
        std::set<std::string> ids;
        for (const auto& r : out) {
            ids.insert(r.id);
        }

        std::scoped_lock l(g_packsLock);
        for (const auto& pack : g_packs) {
            if (ids.contains(pack.id)) {
                // The save's own rule wins; log once per id, not once per
                // Merged() call (see g_loggedCollisions above).
                if (g_loggedCollisions.insert(pack.id).second) {
                    spdlog::warn("RuleStore: pack rule '{}' from '{}' collides with a save "
                                 "rule id; the save's own rule wins, pack rule dropped.",
                                 pack.id, pack.packName);
                }
                continue;
            }
            out.push_back(pack);
        }
        return out;
    }

    void ScanPacks() {
        try {
            RuleSet     collected;
            std::size_t skippedFiles = 0;

            std::error_code ec;
            if (std::filesystem::exists(kPacksDir, ec)) {
                std::vector<std::filesystem::path> files;
                for (const auto& entry : std::filesystem::directory_iterator(kPacksDir, ec)) {
                    if (entry.is_regular_file(ec) &&
                        Lower(entry.path().extension().string()) == ".json") {
                        files.push_back(entry.path());
                    }
                }
                // Deterministic scan order, same mechanism and same reason as
                // PresetStore::Load(): this array position becomes the merged
                // evaluation order, and RuleEngine::PickWinner keeps the
                // EARLIER entry on a priority tie. An unsorted
                // directory_iterator order would make tie-breaks vary launch
                // to launch on the same modlist, for files nobody touched.
                std::ranges::sort(files);

                for (const auto& path : files) {
                    const auto file = path.filename().string();

                    if (const auto size = std::filesystem::file_size(path, ec);
                        !ec && size > kMaxPackBytes) {
                        spdlog::warn("RuleStore: SKIP pack '{}': {} bytes (cap {}).", file, size,
                                     kMaxPackBytes);
                        ++skippedFiles;
                        continue;
                    }

                    RuleSet                  parsed;
                    std::string              error;
                    std::vector<std::string> warnings;
                    if (!ParseRulesDocument(path, parsed, error, warnings)) {
                        spdlog::warn("RuleStore: SKIP pack '{}': {}.", file, error);
                        ++skippedFiles;
                        continue;
                    }
                    // Per-clause/per-rule skips inside an otherwise-valid pack
                    // are not harmless: conditions AND, so a silently dropped
                    // clause makes its rule fire MORE often, and a rule
                    // dropped down to zero clauses becomes unconditional. Log
                    // every one, named to this file, so a pack author can
                    // find the typo.
                    for (const auto& w : warnings) {
                        spdlog::warn("RuleStore: pack '{}': {}", file, w);
                    }

                    for (auto& rule : parsed) {
                        rule.packName = file;
                        collected.push_back(std::move(rule));
                    }
                    spdlog::info("RuleStore: pack '{}': {} rule(s).", file, parsed.size());
                }
            }

            // Cross-file id collisions: JsonToRules already dedups WITHIN one
            // file (later rule dropped); a duplicate id shared by two
            // DIFFERENT pack files needs the same "earlier file wins"
            // treatment, both so Merged()'s array position stays well-defined
            // and so SetPackRuleEnabled's id lookup is never ambiguous.
            RuleSet                deduped;
            std::set<std::string> seen;
            for (auto& rule : collected) {
                if (!seen.insert(rule.id).second) {
                    spdlog::warn(
                        "RuleStore: pack '{}': duplicate rule id '{}' (already provided by an "
                        "earlier pack), skipped.",
                        rule.packName, rule.id);
                    continue;
                }
                deduped.push_back(std::move(rule));
            }

            std::size_t authoredCount = 0;
            {
                std::scoped_lock l(g_packsLock);
                // A rescan (this is not the very first call) wipes every
                // SetPackRuleEnabled override made for whichever save is
                // currently loaded, because the freshly parsed files become
                // the new authored baseline. Nothing here re-applies a save's
                // disabled-id list automatically - the caller must, exactly
                // as OnSaveLoaded's caller (Task 10) already does after a
                // fresh load.
                const bool wasRescan = !g_authored.empty() || !g_packs.empty();
                g_authored    = deduped;
                g_packs       = std::move(deduped);
                g_loggedCollisions.clear();
                authoredCount = g_authored.size();
                if (wasRescan) {
                    spdlog::info(
                        "RuleStore: pack rescan reset every pack-rule enable override; "
                        "reapply the loaded save's disabled-id list.");
                }
            }
            spdlog::info("RuleStore: {} pack rule(s) loaded, {} file(s) skipped.", authoredCount,
                         skippedFiles);
        } catch (const std::exception& e) {
            // directory_iterator's range-for operator++ throws
            // filesystem_error if the directory becomes unreadable mid-scan
            // (removed, permissions changed, a dropped network share); a
            // hand-edited pack file is untrusted input parsed with
            // JSON_USE_EXCEPTION=1. Either way, a scan run at kDataLoaded
            // must not take the whole plugin load down - the previous pack
            // state, if any, is left exactly as it was.
            spdlog::error("RuleStore: ScanPacks threw: {}; pack state left unchanged.", e.what());
        } catch (...) {
            spdlog::error(
                "RuleStore: ScanPacks threw a non-standard exception; pack state left unchanged.");
        }
    }

    void SetPackRuleEnabled(const std::string& a_id, bool a_enabled) {
        std::scoped_lock l(g_packsLock);
        for (auto& rule : g_packs) {
            if (rule.id == a_id) {
                rule.enabled = a_enabled;
                spdlog::info("RuleStore: pack rule '{}' ({}) {}.", a_id, rule.packName,
                             a_enabled ? "enabled" : "disabled");
                return;
            }
        }
        // Not an error: a save can carry a disabled id for a pack rule the
        // author has since removed or renamed (or a pack that is simply not
        // installed on this modlist). The override is inert, same tolerance
        // as a rule naming a missing plugin.
        spdlog::info(
            "RuleStore: SetPackRuleEnabled('{}'): no loaded pack rule with that id (ignored).",
            a_id);
    }

    void QueueMirrorSave(bool a_pairRules) {
        // Task 10 pairs this with Persistence::QueueLibrarySave (see the
        // call a few lines below) so that whenever either the library or
        // the rules of the current save queue their debounced write, BOTH
        // outfits.json and rules.json are rewritten from that same save -
        // otherwise alternating between two characters could leave a seed
        // pair describing one character's outfits with another's rules.
        // The pairing is mutual and each side is individually debounced, so
        // either function alone is a complete, safe call - no caller needs
        // to remember to trigger both.
        //
        // g_saveActive guards the startup and post-revert windows, where
        // g_current is empty because no save owns it yet, not because the
        // player deleted every rule. Without this guard, this function's own
        // "no task interface yet" fallback below fires exactly in the
        // startup window and calls SaveMirrorNow synchronously, overwriting
        // a good shared seed with an empty one before any save has loaded.
        if (!g_saveActive.load(std::memory_order_acquire)) {
            spdlog::debug("RuleStore: QueueMirrorSave skipped - no save loaded yet.");
            return;
        }
        if (g_mirrorPending.exchange(true, std::memory_order_acq_rel)) {
            return;  // one save already queued
        }
        // Pair the mirrors, this direction: a rules-only edit must also
        // refresh outfits.json, or a later edit on a DIFFERENT character
        // could leave the two seed files momentarily describing different
        // characters. Passing false here (see a_pairRules's doc comment in
        // RuleStore.h) is what makes the pairing STRUCTURALLY one hop deep:
        // Persistence::QueueLibrarySave(false) can never itself trigger
        // another pairing call, so this cannot recurse regardless of how
        // either function's branches below get reordered later.
        if (a_pairRules) {
            Persistence::QueueLibrarySave(false);
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            g_mirrorPending.store(false, std::memory_order_release);
            SaveMirrorNow();  // startup edge: save inline
            return;
        }
        task->AddTask([] {
            g_mirrorPending.store(false, std::memory_order_release);
            // Defensive: a background file save must never take the game down.
            try {
                SaveMirrorNow();
            } catch (const std::exception& e) {
                spdlog::error("RuleStore: SaveMirrorNow threw: {}", e.what());
            } catch (...) {
                spdlog::error("RuleStore: SaveMirrorNow threw a non-standard exception.");
            }
        });
    }

    bool LoadSeed(RuleSet& a_out) {
        a_out.clear();
        try {
            std::error_code ec;
            if (!std::filesystem::exists(kSeedPath, ec)) {
                spdlog::info("RuleStore: no rules.json seed yet (fresh install).");
                return false;
            }
            std::string              error;
            std::vector<std::string> warnings;
            if (!ParseRulesDocument(kSeedPath, a_out, error, warnings)) {
                spdlog::error("RuleStore: rules.json rejected: {}.", error);
                a_out.clear();
                RenameAside(kSeedPath);
                return false;
            }
            for (const auto& w : warnings) {
                spdlog::warn("RuleStore: rules.json: {}", w);
            }
            spdlog::info("RuleStore: loaded {} rule(s) from the rules.json seed.", a_out.size());
            return true;
        } catch (const std::exception& e) {
            // JsonToRule/JsonToRules are documented never to throw, but this
            // is untrusted, possibly hand-edited input read with
            // JSON_USE_EXCEPTION=1 - guard the read path the same way the
            // write path (SaveMirrorNow, via QueueMirrorSave) already is.
            spdlog::error("RuleStore: LoadSeed threw: {}; treating rules.json as unreadable.",
                          e.what());
            a_out.clear();
            return false;
        } catch (...) {
            spdlog::error(
                "RuleStore: LoadSeed threw a non-standard exception; treating rules.json as "
                "unreadable.");
            a_out.clear();
            return false;
        }
    }

    void OnSaveLoaded(RuleSet a_fromCoSave) {
        // Deliberately just installs whatever the caller (Task 10) passed -
        // no seed fallback here. A save with no 'RULE' record must arrive as
        // an EMPTY RuleSet, never as rules.json's content: generic outfit
        // names recur across characters, so a silent import would make one
        // character's rules live on another. Importing the seed is only ever
        // the explicit "Load shared rules" button (LoadSeed) or a genuinely
        // new game (OnNewGame below).
        std::size_t count = 0;
        {
            std::scoped_lock l(g_currentLock);
            g_current = std::move(a_fromCoSave);
            count     = g_current.size();
        }
        {
            std::scoped_lock l(g_packsLock);
            ResetPackOverridesLocked();
        }
        // Reset engine state to the fresh-save default (engine enabled, NOT
        // pinned, no last-applied id) BEFORE the caller (Task 10) has a
        // chance to overwrite it from a decoded 'RULE' record via
        // SetEngineState. This is the second, defense-in-depth line against
        // the pin-leak hazard: Persistence::LoadCallback's own local
        // variable already defaults the same way on every call, but this
        // reset means OnSaveLoaded is correct even if called on its own.
        {
            std::scoped_lock l(g_engineStateLock);
            g_engineState = EngineState{};
        }
        g_saveActive.store(true, std::memory_order_release);
        spdlog::info("RuleStore: save loaded with {} rule(s).", count);
    }

    void OnNewGame() {
        // "rules.json is the seed a new game starts from" (RuleStore.h). A
        // missing/unreadable seed is a valid outcome too: LoadSeed logs it and
        // leaves the rule set empty, matching a fresh install with no author
        // input at all.
        RuleSet seed;
        LoadSeed(seed);
        std::size_t count = 0;
        {
            std::scoped_lock l(g_currentLock);
            g_current = std::move(seed);
            count     = g_current.size();
        }
        {
            std::scoped_lock l(g_packsLock);
            ResetPackOverridesLocked();
        }
        // A new game starts unpinned with the engine enabled - same default
        // as a save with no 'RULE' record (see OnSaveLoaded above).
        {
            std::scoped_lock l(g_engineStateLock);
            g_engineState = EngineState{};
        }
        g_saveActive.store(true, std::memory_order_release);
        spdlog::info("RuleStore: new game rule set ready ({} rule(s)).", count);
    }

    void OnRevert() {
        // Rules are per-save (unlike the global outfit library): a revert
        // clears them entirely, matching OutfitSession::OnNpcRevert. The next
        // OnSaveLoaded/OnNewGame reinstalls. Disarm QueueMirrorSave FIRST, so
        // nothing in between can mistake this transient empty state for "the
        // player deleted every rule" and mirror it to disk.
        g_saveActive.store(false, std::memory_order_release);
        {
            std::scoped_lock l(g_currentLock);
            g_current.clear();
        }
        {
            std::scoped_lock l(g_packsLock);
            ResetPackOverridesLocked();
        }
        // Same reasoning as g_current just above: the pin is per-save state
        // and must not survive into whatever loads next.
        {
            std::scoped_lock l(g_engineStateLock);
            g_engineState = EngineState{};
        }
        spdlog::info("RuleStore: reverted; rule state cleared pending the next load.");
    }

    EngineState GetEngineState() {
        std::scoped_lock l(g_engineStateLock);
        return g_engineState;
    }

    void SetEngineState(EngineState a_state) {
        std::scoped_lock l(g_engineStateLock);
        g_engineState = std::move(a_state);
    }

    std::vector<std::string> DisabledPackRuleIds() {
        std::vector<std::string> out;
        std::scoped_lock         l(g_packsLock);
        for (const auto& rule : g_packs) {
            if (!rule.enabled) {
                out.push_back(rule.id);
            }
        }
        return out;
    }

    void RenameOutfitEverywhere(const std::string& a_from, const std::string& a_to) {
        if (a_from.empty() || a_from == a_to) {
            return;  // nothing to rewrite, or not actually a rename
        }
        std::size_t rulesChanged = 0;
        {
            // The pure rewrite itself lives in RuleModel.h (RenameBasesIn)
            // so it is shared with RuleModelTests, not reimplemented here
            // under a lock where a test can't reach it.
            std::scoped_lock l(g_currentLock);
            rulesChanged = RenameBasesIn(g_current, a_from, a_to);
        }
        bool pinChanged = false;
        {
            std::scoped_lock l(g_engineStateLock);
            if (g_engineState.pinned && g_engineState.pinnedName == a_from) {
                g_engineState.pinnedName = a_to;
                pinChanged               = true;
            }
        }
        if (rulesChanged == 0 && !pinChanged) {
            return;
        }
        spdlog::info("RuleStore: outfit renamed '{}' -> '{}': {} rule base(s){}.", a_from, a_to,
                     rulesChanged, pinChanged ? ", pin" : "");
        // Rule base names changed on disk-bound state (rules.json); queuing
        // here also pairs the library mirror through QueueMirrorSave's own
        // Persistence::QueueLibrarySave call, so the rename lands in both
        // files together (Task 10's paired-mirror requirement) even when
        // only the pin (not any rule) matched.
        QueueMirrorSave();
    }

}  // namespace OS::RuleStore
