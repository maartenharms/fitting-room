#include "PCH.h"

#include "Persistence.h"

#include "Collection.h"
#include "JsonCodec.h"
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
        constexpr std::uint32_t kCollectionVersion = 1;
        constexpr std::uint32_t kActiveVersion     = 1;
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
                    break;  // 10-outfit cap
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
            const auto known = Collection::GetSingleton().Encode();
            if (WriteRecord(a_intf, kRecordKnown, kCollectionVersion, known, "collection")) {
                spdlog::info("Persistence: saved {} bytes (appearance collection).", known.size());
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
                if (type != kRecord && type != kRecordKnown && type != kRecordActive) {
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
                if (type == kRecordActive) {
                    if (version == kActiveVersion) {
                        const std::string name(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size());
                        OutfitSession::GetSingleton().ActivateByName(name);
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
        }

        void RevertCallback(SKSE::SerializationInterface*) {
            // The library is GLOBAL - keep it. Only per-save state resets.
            OutfitSession::GetSingleton().OnRevert();
            Collection::GetSingleton().Revert();
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
