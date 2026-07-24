#include "PCH.h"

#include "Persistence.h"

#include "Collection.h"
#include "JsonCodec.h"
#include "NpcAssignments.h"
#include "ObodyApi.h"
#include "OutfitSession.h"
#include "PersistenceCodec.h"

#include <json/json.h>

#include <atomic>
#include <cstdlib>
#include <fstream>

namespace OS::Persistence {

    namespace {
        constexpr std::uint32_t kUniqueID = 'OSLT';
        constexpr std::uint32_t kRecord   = 'LIBR';   // per-save outfit library (collections): this save's OWN outfits
        constexpr std::uint32_t kRecordKnown  = 'KNWN';  // appearance collection
        constexpr std::uint32_t kRecordActive = 'ACTV';  // active outfit NAME (per save)
        constexpr std::uint32_t kRecordNpc    = 'NPCO';  // per-save NPC/follower outfit assignments
        // The player's ORIGINAL OBody preset, captured once before this mod
        // ever assigned one. Its own record rather than a field in 'LIBR'
        // because it is per-SAVE character state, not part of the outfit
        // library (which is also shared globally through outfits.json - a
        // baseline written there would leak one character's body onto another).
        constexpr std::uint32_t kRecordBodyBase = 'BBAS';
        constexpr std::uint32_t kCollectionVersion = 1;
        constexpr std::uint32_t kActiveVersion     = 1;
        constexpr std::uint32_t kBodyBaseVersion   = 1;
        constexpr std::uint32_t kMaxRecordBytes = 1u << 20;  // save and load must agree

        constexpr const char* kLibraryPath = "Data/SKSE/Plugins/FittingRoom/outfits.json";

        std::atomic<bool> g_savePending{ false };

        // The per-outfit shape is the SHARED codec (JsonCodec) - presets and
        // exports read/write the same objects. This file owns only the
        // library wrapper: {version, outfits: [...]}.
        Json::Value LibraryToJson(const OutfitLibrary& a_lib) {
            Json::Value root;
            root["version"] = 1;
            Json::Value arr(Json::arrayValue);
            for (const auto& outfit : a_lib.All()) {
                arr.append(JsonCodec::OutfitToJson(outfit));
            }
            root["outfits"] = std::move(arr);
            return root;
        }

        bool JsonToLibrary(const Json::Value& a_root, OutfitLibrary& a_out) {
            if (!a_root.isObject() || a_root.get("version", 0).asInt() != 1 ||
                !a_root["outfits"].isArray()) {
                return false;
            }
            for (const auto& o : a_root["outfits"]) {
                Outfit parsed;
                if (!JsonCodec::JsonToOutfit(o, parsed)) {
                    continue;  // not an object: skip, never assert
                }
                const int idx = a_out.Create(parsed.name);
                if (idx < 0) {
                    break;  // saved-outfit cap
                }
                *a_out.At(static_cast<std::size_t>(idx)) = std::move(parsed);
            }
            return true;
        }

        void SaveLibraryFileNow() {
            const auto  lib  = OutfitSession::GetSingleton().SnapshotLibrary();
            const auto  root = LibraryToJson(lib);
            std::error_code ec;
            std::filesystem::create_directories("Data/SKSE/Plugins/FittingRoom", ec);
            const std::string tmp = std::string(kLibraryPath) + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) {
                    spdlog::error("Persistence: cannot write {}.", tmp);
                    return;
                }
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "  ";
                out << Json::writeString(wb, root);
            }
            std::filesystem::rename(tmp, kLibraryPath, ec);
            if (ec) {  // cross-volume or locked: fall back to copy
                std::filesystem::copy_file(tmp, kLibraryPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp, ec);
            }
            spdlog::debug("Persistence: outfits.json saved ({} outfits).", lib.Count());
        }

        // Read outfits.json (the shared/global library) into the session as the current
        // library. Returns true on a valid load. No file => loads an EMPTY library so a
        // fresh game (or a save with no per-save record) starts clean; an unreadable /
        // rejected file leaves the session library untouched (returns false).
        bool LoadGlobalIntoSession() {
            std::error_code ec;
            if (!std::filesystem::exists(kLibraryPath, ec)) {
                OutfitSession::GetSingleton().OnLoad(OutfitLibrary{});
                return false;
            }
            std::ifstream           in(kLibraryPath);
            Json::Value             root;
            Json::CharReaderBuilder rb;
            std::string             errs;
            if (!in || !Json::parseFromStream(rb, in, &root, &errs)) {
                spdlog::error("Persistence: outfits.json unreadable ({}).",
                              errs.empty() ? "open failed" : errs);
                return false;
            }
            OutfitLibrary lib;
            if (!JsonToLibrary(root, lib)) {
                spdlog::error("Persistence: outfits.json rejected (wrong version/shape).");
                return false;
            }
            OutfitSession::GetSingleton().OnLoad(std::move(lib));
            return true;
        }

        bool WriteRecord(SKSE::SerializationInterface* a_intf, std::uint32_t a_type,
                         std::uint32_t a_version, const std::vector<std::byte>& a_bytes,
                         const char* a_what) {
            if (a_bytes.size() > kMaxRecordBytes) {
                spdlog::error("Persistence: encoded {} bytes exceeds {}-byte cap; {} NOT saved "
                              "(previous co-save data kept).",
                              a_bytes.size(), kMaxRecordBytes, a_what);
                return false;
            }
            if (!a_intf->OpenRecord(a_type, a_version)) {
                spdlog::error("Persistence: OpenRecord failed; {} NOT saved.", a_what);
                return false;
            }
            const auto len = static_cast<std::uint32_t>(a_bytes.size());
            if (!a_intf->WriteRecordData(len) ||
                (len && !a_intf->WriteRecordData(a_bytes.data(), len))) {
                spdlog::error("Persistence: WriteRecordData failed; {} NOT saved.", a_what);
                return false;
            }
            return true;
        }

        void SaveCallback(SKSE::SerializationInterface* a_intf) {
            const auto lib = OutfitSession::GetSingleton().SnapshotLibrary();
            // Collections: write this save's FULL outfit library into the co-save so the
            // save OWNS its outfits - editing/deleting an outfit on another save (the
            // shared outfits.json) can no longer make them vanish here. Written FIRST so
            // LoadCallback restores the library before ACTV re-selects within it. (Encode
            // also carries the active index; ACTV stays for clarity + the old load path.)
            const auto libBytes = Encode(lib);
            if (WriteRecord(a_intf, kRecord, kCodecVersion, libBytes, "library")) {
                spdlog::info("Persistence: saved {} outfit(s) into the save (collection).",
                             lib.Count());
            }
            std::string activeName;
            if (const auto* active = lib.Active()) {
                activeName = active->name;
            }
            std::vector<std::byte> activeBytes(activeName.size());
            std::memcpy(activeBytes.data(), activeName.data(), activeName.size());
            if (WriteRecord(a_intf, kRecordActive, kActiveVersion, activeBytes, "active outfit")) {
                spdlog::info("Persistence: saved active outfit '{}'.",
                             activeName.empty() ? "(none)" : activeName);
            }
            // The OBody baseline. Written only once it has actually been
            // captured: an absent record means "we never moved this
            // character's body", which is exactly what a fresh save should
            // restore to on load.
            if (ObodyApi::BaselineCaptured()) {
                const auto     base = ObodyApi::Baseline();
                std::vector<std::byte> baseBytes(base.size());
                std::memcpy(baseBytes.data(), base.data(), base.size());
                if (WriteRecord(a_intf, kRecordBodyBase, kBodyBaseVersion, baseBytes,
                                "OBody baseline")) {
                    spdlog::debug("Persistence: saved OBody baseline '{}'.",
                                  base.empty() ? "(none)" : base.c_str());
                }
            }
            const auto known = Collection::GetSingleton().Encode();
            if (WriteRecord(a_intf, kRecordKnown, kCollectionVersion, known, "collection")) {
                spdlog::info("Persistence: saved {} bytes (appearance collection).", known.size());
            }
            // NPC/follower assignments: per-save co-save state, never routed through
            // WithLibrary/outfits.json (see OutfitSession::UpsertNpcLibrary). Skip the
            // record entirely when empty, matching the "no empty record" convention -
            // an old save with no NPCO record decodes as zero assignments regardless.
            const auto npcMap = OutfitSession::GetSingleton().SnapshotNpcAssignments();
            if (!npcMap.empty()) {
                const auto npcBytes = EncodeNpcAssignments(npcMap);
                if (WriteRecord(a_intf, kRecordNpc, kNpcRecordVersion, npcBytes,
                                "NPC assignments")) {
                    spdlog::info("Persistence: saved {} NPC assignment(s).", npcMap.size());
                }
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_intf) {
            // Collections: reset the library to the GLOBAL default (outfits.json) before
            // reading this save's records. A 'LIBR' record then REPLACES it with the
            // save's own outfits; a save without one (old, pre-collections) keeps the
            // global. Without this reset, loading an old save after a LIBR save in the
            // same session would inherit the previous save's library.
            LoadGlobalIntoSession();

            std::uint32_t type = 0, version = 0, length = 0;
            while (a_intf->GetNextRecordInfo(type, version, length)) {
                if (type != kRecord && type != kRecordKnown && type != kRecordActive &&
                    type != kRecordNpc && type != kRecordBodyBase) {
                    continue;
                }
                std::uint32_t len = 0;
                if (a_intf->ReadRecordData(len) != sizeof(len) || len > kMaxRecordBytes) {
                    spdlog::error("Persistence: bad record header; skipping.");
                    continue;
                }
                std::vector<std::byte> bytes(len);
                if (len && a_intf->ReadRecordData(bytes.data(), len) != len) {
                    spdlog::error("Persistence: truncated record; skipping.");
                    continue;
                }
                if (type == kRecordKnown) {
                    if (!Collection::GetSingleton().Decode(bytes, version)) {
                        spdlog::warn("Persistence: refused collection record (version {}).", version);
                    }
                    continue;
                }
                if (type == kRecordBodyBase) {
                    if (version == kBodyBaseVersion) {
                        const std::string base(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size());
                        // Present at all == captured. An empty STRING is a
                        // real value ("this character had no OBody preset
                        // before we touched them"), distinct from an absent
                        // record, so the flag cannot be derived from emptiness.
                        ObodyApi::SetBaseline(base, true);
                        spdlog::info("Persistence: OBody baseline restored ('{}').",
                                     base.empty() ? "(none)" : base.c_str());
                    }
                    continue;
                }
                if (type == kRecordActive) {
                    if (version == kActiveVersion) {
                        const std::string name(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size());
                        OutfitSession::GetSingleton().ActivateByName(name);
                    }
                    continue;
                }
                if (type == kRecordNpc) {
                    // Per-save NPC/follower assignments. A decode failure (bad version,
                    // truncated outer structure) is logged and skipped, never abort the
                    // load - RevertCallback already left npcAssignments_ empty, so a
                    // skip here is equivalent to "no assignments this save", same
                    // tolerance as a refused LIBR/KNWN record above. A single corrupt
                    // ENTRY inside an otherwise-valid record is handled one level down
                    // by DecodeNpcAssignments itself (per-entry tolerance).
                    NpcAssignmentMap npcMap;
                    if (DecodeNpcAssignments(bytes, version, npcMap)) {
                        spdlog::info("Persistence: loaded {} NPC assignment(s) from the save.",
                                     npcMap.size());
                        OutfitSession::GetSingleton().OnNpcLoad(std::move(npcMap));
                    } else {
                        spdlog::warn(
                            "Persistence: refused NPC assignment record (version {}, {} bytes).",
                            version, len);
                    }
                    continue;
                }
                // 'LIBR' - this save's OWN outfit library (collections). Authoritative:
                // it REPLACES the global default loaded above, so the save keeps its
                // outfits no matter what other saves did to the shared file. Encode
                // carries the active index, so OnLoad restores the selection too (a
                // following ACTV record just re-confirms it by name).
                OutfitLibrary lib;
                if (!Decode(bytes, version, lib)) {
                    spdlog::warn("Persistence: refused library record (version {}, {} bytes).",
                                 version, len);
                    continue;
                }
                spdlog::info("Persistence: loaded {} outfit(s) from the save (collection).",
                             lib.All().size());
                OutfitSession::GetSingleton().OnLoad(std::move(lib));
            }

            // Deferred NPC refresh (spec §6): an assigned actor that already rendered
            // its biped BEFORE this LoadCallback ran (already high-process at the point
            // the save finished loading) needs an explicit kick; a not-yet-loaded actor
            // restyles naturally on its own next rebuild regardless. RenderSnapshot()
            // is a cheap atomic load safe from any thread - it is only CAPTURED here,
            // not walked; the actual ForEachHighActor walk runs inside the queued task,
            // on the MAIN thread, per BSTArray-mutation-on-process-transition safety
            // (see the ForEachReferenceInRange AE pitfall this deliberately avoids by
            // using ProcessLists instead of a cell/ref scan).
            const auto npcSnapshot = OutfitSession::GetSingleton().RenderSnapshot();
            if (npcSnapshot && !npcSnapshot->empty()) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([npcSnapshot] {
                        try {
                            auto* processLists = RE::ProcessLists::GetSingleton();
                            if (!processLists) {
                                return;
                            }
                            processLists->ForEachHighActor(
                                [&](RE::Actor& a_actor) -> RE::BSContainer::ForEachResult {
                                    if (const auto* base = a_actor.GetActorBase();
                                        base && npcSnapshot->contains(base->GetFormID())) {
                                        OutfitSession::RequestRefreshActor(a_actor.GetHandle());
                                    }
                                    return RE::BSContainer::ForEachResult::kContinue;
                                });
                        } catch (const std::exception& e) {
                            spdlog::error("Persistence: deferred NPC refresh walk threw: {}",
                                          e.what());
                        } catch (...) {
                            spdlog::error(
                                "Persistence: deferred NPC refresh walk threw a non-standard "
                                "exception.");
                        }
                    });
                }
            }
        }

        void RevertCallback(SKSE::SerializationInterface*) {
            // The library is GLOBAL - keep it. Only per-save state resets.
            OutfitSession::GetSingleton().OnRevert();
            OutfitSession::GetSingleton().OnNpcRevert();
            Collection::GetSingleton().Revert();
            // The baseline belongs to the CHARACTER, so it must not survive
            // into another save. Revert runs before every load; a stale
            // baseline here would send the next character's "Your usual body"
            // to the previous character's preset.
            ObodyApi::SetBaseline({}, false);
        }
    }

    void Register() {
        const auto* ser = SKSE::GetSerializationInterface();
        ser->SetUniqueID(kUniqueID);
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
        spdlog::info("Persistence registered.");
    }

    void LoadLibraryFileAtStartup() {
        std::error_code ec;
        if (!std::filesystem::exists(kLibraryPath, ec)) {
            spdlog::info("Persistence: no outfits.json yet (fresh install).");
            return;
        }
        if (LoadGlobalIntoSession()) {
            spdlog::info("Persistence: global library loaded ({} outfits).",
                         OutfitSession::GetSingleton().SnapshotLibrary().Count());
        } else {
            spdlog::warn("Persistence: outfits.json present but not loaded (bad/empty); "
                         "it will not be overwritten until an outfit changes.");
        }
    }

    void QueueLibrarySave() {
        if (g_savePending.exchange(true, std::memory_order_acq_rel)) {
            return;  // one save already queued
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            g_savePending.store(false, std::memory_order_release);
            SaveLibraryFileNow();  // startup edge: save inline
            return;
        }
        task->AddTask([] {
            g_savePending.store(false, std::memory_order_release);
            // Defensive: a background file save must never take the game down.
            try {
                SaveLibraryFileNow();
            } catch (const std::exception& e) {
                spdlog::error("SaveLibraryFileNow threw: {}", e.what());
            } catch (...) {
                spdlog::error("SaveLibraryFileNow threw a non-standard exception.");
            }
        });
    }

}  // namespace OS::Persistence
