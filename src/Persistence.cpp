#include "PCH.h"

#include "Persistence.h"

#include "BuildChannel.h"
#include "Collection.h"
#include "AppearanceWatch.h"
#include "LookFaceTint.h"  // 'FTNT': the export a look's baked face lives in
#include "DefaultLook.h"  // 'DFLK': this save's default hair, eyes and brows
#include "DyeHistory.h"
#include "DyeUnlocks.h"
#include "HairColor.h"
#include "HeadEditorSink.h"  // the visit reading, dropped on revert
#include "HeadPart.h"  // 'HPBS': this save's captured pre-Fitting-Room parts
#include "NpcHair.h"
#include "JsonCodec.h"
#include "MeasuredGhosts.h"  // measured ghost bits die with the save
#include "NpcAssignments.h"
#include "ObodyApi.h"
#include "OutfitDye.h"
#include "OutfitSession.h"
#include "PersistenceCodec.h"
#include "ProfileCapture.h"  // pending face waits abandon on revert
#include "CharacterSex.h"     // the sex a look states; the engine will not keep it
#include "MakeupBaseline.h"   // the player's tint list, saved the same way
#include "OverlayBaseline.h"  // the player's overlay art, saved as the look codec writes it
#include "ProfileCodec.h"     // ProfileToJson/ParseProfile: one field list, not two
#include "RuleCodec.h"
#include "RuleStore.h"
#include "Settings.h"
#include "SharedCollection.h"
#include "SharedDyeUnlocks.h"
#include "MakeupApi.h"  // ReleaseSkinTone: the held tone must not cross into another character
#include "SkinApi.h"  // 'SKIN': the packs actors wear and the overrides written for them
#include "WorldWatch.h"

#if defined(FR_BODY_STUDIO)
#include "DefaultBody.h"
#endif

#include <json/json.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace OS::Persistence {

    namespace {
        constexpr std::uint32_t kRecord   = 'LIBR';   // per-save outfit library (collections): this save's OWN outfits
        constexpr std::uint32_t kRecordKnown  = 'KNWN';  // appearance collection
        // Which of those looks the player has actually LOOKED AT, so a newly
        // found piece can wear a mark until they do.
        //
        // ⚠ ITS OWN RECORD, and Collection.h argues why: a malformed
        // acknowledged block must cost the gold corners and nothing else, where
        // folding it into 'KNWN' would put the browser's whole collection
        // filter in the same failure domain as an ornament. Same split, same
        // argument, as 'DYHI' beside 'DYES'.
        //
        // ⚠ IT SHARES kCollectionVersion DELIBERATELY. The two records hold the
        // same wire shape, written by the same pair of helpers, and versioning
        // them apart would invite one to move without the other.
        constexpr std::uint32_t kRecordKnownAck = 'KACK';
        // The colour equivalent: which EARNED colours the player has looked at.
        //
        // ⚠⚠ ITS OWN RECORD, AND 'DYES' IS DELIBERATELY NOT WIDENED. That one
        // carries the charge the player bought with soul gems and the deeds
        // they were billed for, and a bump on the record holding paid currency
        // is the mistake this project has already made twice. An ornament must
        // not share a failure domain with money. Its version travels with
        // kDyeUnlockVersion so the two cannot drift.
        constexpr std::uint32_t kRecordDyeAck = 'DACK';
        constexpr std::uint32_t kRecordActive = 'ACTV';  // active outfit NAME (per save)
        constexpr std::uint32_t kRecordNpc    = 'NPCO';  // per-save NPC/follower outfit assignments
        // The player's ORIGINAL OBody preset, captured once before this mod
        // ever assigned one. Its own record rather than a field in 'LIBR'
        // because it is per-SAVE character state, not part of the outfit
        // library (which is also shared globally through outfits.json - a
        // baseline written there would leak one character's body onto another).
        constexpr std::uint32_t kRecordBodyBase = 'BBAS';
        // The player's captured pre-Fitting-Room hair colour. Same reasoning
        // as kRecordBodyBase directly above: it is character state, not
        // outfit state, so it gets its own record and must not ride the
        // outfit library's version.
        constexpr std::uint32_t kRecordHairBase = 'HCOL';
        // The player's captured pre-Fitting-Room head PARTS: the style half of
        // what 'HCOL' holds for colour. Its own record for the same reason as
        // every other one here, and see PersistenceCodec.h for why it had to
        // exist at all rather than the capture staying session state.
        constexpr std::uint32_t kRecordHeadBase = 'HPBS';
        // The colours this character has EARNED, their Seamstone charge and the
        // deeds we count. Character state again, so the same reasoning as the
        // two above applies twice over: it rides neither 'LIBR' nor 'NPCO'.
        // Those two just moved to v9 and a palette is no reason to move them
        // again. Its version is kDyeUnlockVersion, owned by DyeUnlocks.h.
        constexpr std::uint32_t kRecordDyes = 'DYES';
        // The colours the player mixed by hand.
        //
        // ⚠ ITS OWN RECORD RATHER THAN AN APPEND TO 'DYES', deliberately. A
        // record that fails to decode leaves its set empty and SaveCallback
        // writes that emptiness straight back, and unlocks are add only and
        // rest on base actor values that move, so a lost unlock set cannot be
        // re-earned. Sharing one record would put the least valuable data in
        // the dye design in the same failure domain as the most valuable: a
        // malformed history block would cost the player their progression.
        // Its version is kDyeHistoryVersion, owned by DyeHistory.h.
        constexpr std::uint32_t kRecordDyeHistory = 'DYHI';
        // Per-save rule-engine state (Task 10): the save's own rule set plus
        // the small scalars that ride with it (engine on/off, pin, the
        // overlay reconstruction anchor, disabled pack-rule ids). See
        // PersistenceCodec.h's RuleRecordFields/EncodeRuleState/
        // DecodeRuleState and RuleStore.h's EngineState for the two halves
        // of this record's shape.
        constexpr std::uint32_t kRecordRule = 'RULE';
        // This save's CHARACTER DEFAULTS: the hair, eyes and brows a character
        // falls back to, and the body they do.
        //
        // ⚠ THESE WERE FILES ON DISK UNTIL 2026-08-08 AND THAT WAS A CURRENCY
        // EXPLOIT. Setting a default costs a look, the gold or charge comes out
        // of THIS co-save, and the default it bought lived outside every save:
        // pay, reload an earlier save, keep the default, get the money back.
        // Same reasoning as kRecordBodyBase and kRecordHairBase directly above,
        // arrived at the hard way. See DefaultLook.h.
        //
        // ⚠ TWO RECORDS, NOT ONE, for the reason 'DYHI' is not part of 'DYES':
        // a malformed block must not take the other half down with it. It also
        // keeps the Body Studio gate out of the format, since DefaultBody is
        // compiled only on that channel and DefaultLook ships everywhere.
        constexpr std::uint32_t kRecordDefLook = 'DFLK';
        constexpr std::uint32_t kRecordDefBody = 'DFBD';
        // Which skin pack each actor wears and every skee armour override this
        // mod wrote for it. Character state, its own record, same reasoning as
        // 'HCOL' and 'HPBS': what it protects is the ability to take the
        // overrides OFF again, since skee holds them for the life of the save
        // and cannot list ours. See SkinApi.h.
        constexpr std::uint32_t kRecordSkin = 'SKIN';
        // The player's overlay ART, held by us because skee's store is not a
        // source of truth for it. Character state, its own record, same
        // reasoning as 'HCOL', 'HPBS' and 'SKIN' above: what it protects is a
        // load coming back with the clones rebuilt and nothing painted on them,
        // which is the 2026-08-25 field report. See OverlayBaseline.h.
        //
        // ⚠ IT CARRIES JSON, and that is deliberate. The payload is a look's
        // own overlays block, encoded by the codec that already writes looks to
        // disk, so a new LayerState field cannot be added without this record
        // learning it too. A second hand-rolled copy of that field list is
        // exactly the drift this avoids; PersistenceCodec only packs and
        // unpacks the string, as it does for 'RULE'.
        constexpr std::uint32_t kRecordOvlBase = 'OVLB';
        // The player's TINT LIST, held for the same reason 'OVLB' holds the
        // overlay art: RaceMenu serializes the 108-slot list in ITS cosave and
        // restores the copy IT last knew, which predates every in-session
        // write of ours. JSON through the look codec, 'OVLB's own reasoning.
        // See MakeupBaseline.h for the 2026-09-02 00:14 skull that taught it.
        constexpr std::uint32_t kRecordMkBase = 'MKUP';
        // The skin tone we are HOLDING for the player. Its own record for the
        // same reason 'HCOL' and 'OVLB' have theirs: character state, and one
        // malformed block must not take another down with it. See MakeupApi.h.
        constexpr std::uint32_t kRecordSkinHold = 'SKTN';
        // The sex a look states for this character. Its own record, and the
        // last block of a look that had no survival story. See CharacterSex.h.
        constexpr std::uint32_t kRecordSex = 'SEXF';
        // The CharGen jslot whose exported face tint this character wears. Its
        // own record for the reason 'SEXF' has one: a block of a look that had
        // no survival story at all. See LookFaceTint.h.
        constexpr std::uint32_t kRecordFaceTint = 'FTNT';
        constexpr std::uint32_t kCollectionVersion = 1;
        constexpr std::uint32_t kActiveVersion     = 1;
        constexpr std::uint32_t kBodyBaseVersion   = 1;
        constexpr std::uint32_t kHairBaseVersion   = 1;
        constexpr std::uint32_t kHeadBaseVersion   = 1;
        constexpr std::uint32_t kRuleVersion = 1;
        constexpr std::uint32_t kOvlBaseVersion = 1;
        constexpr std::uint32_t kMkBaseVersion  = 1;
        constexpr std::uint32_t kSkinHoldVersion = 1;
        constexpr std::uint32_t kSexVersion = 1;
        constexpr std::uint32_t kFaceTintVersion = 1;
        // ⚠ THE WRITE VERSION. The READ side accepts v1 too, because bumping
        // this while the read tested equality would have skipped every v1
        // record and deleted the player's defaults (OS-196; the decoder takes
        // the version now).
        constexpr std::uint32_t kDefLookVersion = kDefaultLookVersion;
        constexpr std::uint32_t kDefBodyVersion = 1;
        // v2 (2026-08-18) adds the head rows; the codec names both layouts.
        constexpr std::uint32_t kSkinVersion    = kSkinLayout;
        // kMaxRecordBytes moved to Persistence.h so DyeUnlocks.cpp can
        // static_assert its own caps against it. Same value, same meaning:
        // save and load must agree. feat/auto-rules still declared it locally;
        // that copy is dropped here rather than shadowing the header's.

        const auto kLibraryPath = BuildChannel::DataPath("outfits.json");

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
            std::filesystem::create_directories(BuildChannel::DataRoot(), ec);
            const std::string tmp = kLibraryPath.string() + ".tmp";
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
                // ⚠ The old wording here said "previous co-save data kept" and
                // meant the opposite of what happens. SKSE REBUILDS the co-save
                // on every save, so a record we decline to write is not left at
                // its previous value, it is absent from the save that gets
                // written. For 'LIBR' outfits.json can rebuild it and for
                // 'KNWN' the collection re-accumulates, but 'DYES' carries the
                // Seamstone charge and the deed counters, and nothing anywhere
                // else holds those. A reassuring sentence pointed whoever read
                // this log away from the one record that cannot come back.
                spdlog::error("Persistence: encoded {} bytes exceeds {}-byte cap; {} NOT saved. "
                              "SKSE rebuilds the co-save, so this record is GONE from this save.",
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
            // The player's pre-Fitting-Room hair colour. An absent record means
            // "we never changed this character's hair", same convention as the
            // OBody baseline above and as kRecordNpc: no empty records.
            //
            // Its OWN record with its OWN version, deliberately not part of the
            // outfit library's. This is CHARACTER state, not outfit state, so it
            // must not be dragged along by an outfit-format bump.
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                const auto captured = HairColor::CapturedFor(player);
                if (captured.captured) {
                    const auto hairBytes =
                        EncodeHairBaseline(captured.modName, captured.localFormID);
                    if (WriteRecord(a_intf, kRecordHairBase, kHairBaseVersion, hairBytes,
                                    "hair colour baseline")) {
                        spdlog::debug("Persistence: saved hair colour baseline ('{}' {:06X}).",
                                      captured.modName, captured.localFormID);
                    }
                }
            }
            // The style half of the same idea, and the record that stops Base
            // gear losing its "before" across a load. Skipped when there is
            // nothing captured: an absent record is exactly "this character
            // has had no head part changed", which is what a fresh save means.
            {
                std::vector<HeadPartBaselineRow> rows;
                for (const auto& c : HeadPart::SnapshotCaptures()) {
                    rows.push_back(HeadPartBaselineRow{ c.slot, c.modName, c.localFormID });
                }
                if (!rows.empty()) {
                    const auto headBytes = EncodeHeadPartBaseline(rows);
                    if (WriteRecord(a_intf, kRecordHeadBase, kHeadBaseVersion, headBytes,
                                    "head part baseline")) {
                        spdlog::debug("Persistence: saved {} head part baseline row(s).",
                                      rows.size());
                    }
                }
            }
            // The skin packs actors wear and the overrides written for them.
            // Skipped when nothing is worn and nothing is written: an absent
            // record is "no actor's skin was touched", the same convention as
            // the two baselines above.
            {
                const auto skinRows = SkinApi::Snapshot();
                if (!skinRows.empty()) {
                    const auto skinBytes = EncodeSkinRows(skinRows);
                    if (WriteRecord(a_intf, kRecordSkin, kSkinVersion, skinBytes, "skin packs")) {
                        std::size_t written = 0;
                        for (const auto& row : skinRows) {
                            written += row.skin.written.size();
                        }
                        spdlog::info("Persistence: saved skin rows for {} actor(s), {} override(s).",
                                     skinRows.size(), written);
                    }
                }
            }
            const auto known = Collection::GetSingleton().Encode();
            if (WriteRecord(a_intf, kRecordKnown, kCollectionVersion, known, "collection")) {
                spdlog::info("Persistence: saved {} bytes (appearance collection).", known.size());
            }
            // ⚠ WRITTEN UNCONDITIONALLY, EMPTY OR NOT, AND THE EMPTINESS IS
            // LOAD BEARING, exactly as the default-looks record below spells
            // out. The record's PRESENCE is what tells the next load that this
            // save has been through the feature; skipping an empty one would
            // make a save whose player has acknowledged nothing re-take the
            // baseline on every load and silently clear the very marks they
            // had not got to yet.
            {
                const auto seen = Collection::GetSingleton().EncodeAcked();
                if (WriteRecord(a_intf, kRecordKnownAck, kCollectionVersion, seen,
                                "collection seen-marks")) {
                    spdlog::debug("Persistence: saved {} bytes (collection seen-marks).",
                                  seen.size());
                }
            }
            // This save's character defaults.
            //
            // ⚠ WRITTEN UNCONDITIONALLY, EMPTY OR NOT, AND THE EMPTINESS IS
            // LOAD BEARING. "This save carries a record" is what tells the load
            // path it has already been through this build, so the one-time
            // import from the retired default-looks.json runs exactly once.
            // Skipping an empty write would make every defaults-free save
            // re-import the old global file on every load, which is the
            // cross-save leak these records exist to end.
            {
                const auto looks = EncodeDefaultLooks(DefaultLook::Snapshot());
                if (WriteRecord(a_intf, kRecordDefLook, kDefLookVersion, looks,
                                "default looks")) {
                    spdlog::debug("Persistence: saved {} byte(s) of default looks.",
                                  looks.size());
                }
            }
            // The sex this character's look states. ⚠ ONLY WHEN A LOOK HAS
            // SAID SO: absent means Fitting Room has never been told, and the
            // base keeps whatever the game gives it, which is the right answer
            // for a character we have not dressed.
            {
                bool female = false;
                if (CharacterSex::Held(female)) {
                    if (WriteRecord(a_intf, kRecordSex, kSexVersion,
                                    EncodeCharacterSex(female), "character sex")) {
                        spdlog::info("Persistence: saved this character's sex ({}).",
                                     female ? "female" : "male");
                    }
                }
            }
            // The export this character's face is baked into. ⚠ ONLY WHEN A
            // LOOK HAS SAID SO, the same convention as the sex above: absent
            // means this character's face is whatever the tint list builds,
            // which is what a character we have not dressed means and what the
            // load path already does the right thing with.
            {
                std::string jslot;
                if (LookFaceTint::Held(jslot)) {
                    if (WriteRecord(a_intf, kRecordFaceTint, kFaceTintVersion,
                                    EncodeLookFaceTint(jslot), "look face tint")) {
                        spdlog::info("Persistence: saved the look's baked face '{}'.", jslot);
                    }
                }
            }
            // The skin tone being held for the player. ⚠ ONLY WHEN ONE IS:
            // absent means "Fitting Room is not driving this character's body
            // colour", which is what a save carrying no look means and what the
            // load path already does the right thing with.
            {
                OverlayPlan::Rgb tint{};
                float            strength = 1.0f;
                if (MakeupApi::HeldSkinTone(tint, strength)) {
                    if (WriteRecord(a_intf, kRecordSkinHold, kSkinHoldVersion,
                                    EncodeSkinToneHold(tint.r, tint.g, tint.b, strength),
                                    "skin tone hold")) {
                        spdlog::info("Persistence: saved the held skin tone "
                                     "({},{},{}) at {:.3f}.",
                                     tint.r, tint.g, tint.b, strength);
                    }
                }
            }
            // The player's overlay art. ⚠ WRITTEN ONLY WHEN THERE IS SOMETHING
            // TO WRITE, the same convention as the OBody and hair baselines
            // above and the opposite of default looks below them: an absent
            // record here means "we have written no overlay for this
            // character", which is what a fresh save means and what the load
            // path already does the right thing with. There is no one-time
            // import to guard, so emptiness carries nothing.
            //
            // ⚠ GUARDED, like the rules record, because the JSON is built here
            // rather than handed over ready-made. A throw inside a
            // serialization callback is a crash inside BGSSaveLoadManager;
            // losing this one record beats terminating the save.
            try {
                if (OverlayBaseline::HasAny()) {
                    ProfileCodec::Profile wrapper;
                    wrapper.name     = "overlay-baseline";
                    wrapper.overlays = OverlayBaseline::Snapshot();
                    Json::StreamWriterBuilder wb;
                    wb["indentation"] = "";  // co-save bytes, not a human-edited file
                    const auto json =
                        Json::writeString(wb, ProfileCodec::ProfileToJson(wrapper));
                    if (WriteRecord(a_intf, kRecordOvlBase, kOvlBaseVersion,
                                    EncodeOverlayBaseline(json), "overlay baseline")) {
                        spdlog::info("Persistence: saved the player's overlay art "
                                     "({} byte(s)).",
                                     json.size());
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("Persistence: overlay baseline NOT saved this time "
                              "({}); the previous record, if any, stands.",
                              e.what());
            } catch (...) {
                spdlog::error("Persistence: overlay baseline NOT saved this time "
                              "(unknown exception); the previous record, if any, "
                              "stands.");
            }
            // The tint-list record, guarded for the same reason as the block
            // above: JSON is built here, and losing one record beats
            // terminating the save.
            try {
                // ⚠⚠ Recorded(), NOT HasAny(): a bare list is a claim. Field
                // 2026-09-02 02:40, a save written with an empty record
                // carried no record, and an older save's makeup walked into
                // it on the next load. The codec omits an empty makeup block,
                // so a bare claim rides as a wrapper with a name and a stamp
                // and no block, which the load reads back as exactly that.
                if (MakeupBaseline::Recorded()) {
                    ProfileCodec::Profile wrapper;
                    wrapper.name   = "makeup-baseline";
                    wrapper.makeup = MakeupBaseline::Snapshot();
                    // WHO the list belongs to rides beside it, so the converge
                    // on the far side of a load can refuse a stranger's record
                    // (the Nord 3 leftovers, 2026-09-02). The codec's own
                    // character block: same shape a look already carries.
                    wrapper.character = MakeupBaseline::Who();
                    const auto worn   = wrapper.makeup->size();
                    Json::StreamWriterBuilder wb;
                    wb["indentation"] = "";  // co-save bytes, not a human-edited file
                    const auto json =
                        Json::writeString(wb, ProfileCodec::ProfileToJson(wrapper));
                    if (WriteRecord(a_intf, kRecordMkBase, kMkBaseVersion,
                                    EncodeOverlayBaseline(json), "makeup baseline")) {
                        spdlog::info("Persistence: saved the player's tint list "
                                     "({} byte(s), {}).",
                                     json.size(),
                                     worn == 0 ? std::string{ "a bare claim, nothing worn" }
                                               : fmt::format("{} worn entr{}", worn,
                                                             worn == 1 ? "y" : "ies"));
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("Persistence: makeup baseline NOT saved this time "
                              "({}); the previous record, if any, stands.",
                              e.what());
            } catch (...) {
                spdlog::error("Persistence: makeup baseline NOT saved this time "
                              "(unknown exception); the previous record, if any, "
                              "stands.");
            }
#if defined(FR_BODY_STUDIO)
            {
                const auto bodies = EncodeDefaultBodies(DefaultBody::Snapshot());
                if (WriteRecord(a_intf, kRecordDefBody, kDefBodyVersion, bodies,
                                "default bodies")) {
                    spdlog::debug("Persistence: saved {} byte(s) of default bodies.",
                                  bodies.size());
                }
            }
#endif
            // Earned colours, charge and deeds. Written UNCONDITIONALLY, unlike
            // the NPC, OBody and hair records, none of which write when empty:
            // absent and empty mean the same thing for those, and here they do
            // not. A character who has spent their Seamstone down to nothing has
            // a real state worth recording, and the deed counters ride this same
            // record.
            //
            // STILL one With() for the whole read, and that part must survive
            // any future edit: three separate calls would take the lock three
            // times and could encode one state and log another.
            //
            // What the lock does NOT span is the co-save write. The bytes, the
            // count and the charge are copied out under it (about 10 KB at
            // shipped scale) and OpenRecord plus the two WriteRecordData calls
            // run outside. DyeUnlocks::Snapshot promises any thread may call it,
            // and once the dye pane is wired the render thread would otherwise
            // block on g_lock for the length of a disk write.
            std::vector<std::byte> dyeBytes;
            std::size_t            dyeCount  = 0;
            std::uint32_t          dyeCharge = 0;
            // ⚠ THE IDS COME OUT UNDER THE SAME ONE With AS THE BYTES, for the
            // reason spelled out above: three separate calls would take the
            // lock three times and could encode one state while sharing
            // another. Ids() hands back a copy for the same reason Encode
            // does, so the file write below happens with the lock released.
            std::set<std::string> dyeIds;
            DyeUnlocks::With([&](DyeUnlockSet& a_set) {
                dyeBytes  = a_set.Encode();
                dyeCount  = a_set.Size();
                dyeCharge = a_set.Charge();
                dyeIds    = a_set.Ids();
            });
            const bool dyeWritten =
                WriteRecord(a_intf, kRecordDyes, kDyeUnlockVersion, dyeBytes, "dye unlocks");
            if (dyeWritten) {
                spdlog::debug("Persistence: wrote {} dye unlock(s), charge {}.", dyeCount,
                              dyeCharge);
            }
            // ---- the account-wide half of the unlocks (OS-198) -------------
            //
            // ⚠⚠ ONLY THE FLAGS. The charge sitting in dyeCharge two lines up
            // and the deed counters inside dyeBytes stay in this save, with the
            // gold that bought them (OS-172). SharedDyeUnlocks' schema cannot
            // express either, so this is enforced rather than remembered.
            //
            // ⚠ GATED ON THE RECORD HAVING BEEN WRITTEN. A record refused for
            // exceeding the size cap is GONE from this save, so banking its ids
            // account-wide would hand every other character colours this one
            // just lost.
            //
            // ⚠ WRITTEN FROM WHAT WAS SAVED, WHICH IS THE WHOLE MITIGATION. A
            // colour gated on the channelsDyed deed was earned by paying, so if
            // the flag could be banked from LIVE state a player could pay,
            // reload without saving, and keep it for nothing. Sitting in
            // SaveCallback means the save is the commitment, exactly as it
            // already is for the charge itself.
            //
            // ⚠ RE-READ RATHER THAN REMEMBERED FROM THE LOAD. The setting can
            // be turned on mid-session, in which case no merge has run, and
            // writing a remembered baseline would put ONLY this character's ids
            // over a file holding every other character's. Re-reading is also
            // what re-arms the unreadable latch, so a file that went bad since
            // the load stands this write down instead of replacing it.
            //
            // ⚠ THE PUBLISH IS SyncSharedDyeUnlocks' SECOND HALF NOW, and this
            // site keeps only the gate the other callers cannot state: the
            // record must actually have been written. That is why this is not
            // simply the shared call.
            if (dyeWritten) {
                SyncSharedDyeUnlocks("the save");
            }
            // The gear half. No record gate: the collection's own record is
            // written unconditionally a screen above, and unlike the dye set it
            // carries nothing that was paid for, so there is no "this save just
            // lost it" case to guard against.
            SyncSharedCollection("the save");
            // Which colours have been looked at. Written unconditionally, empty
            // or not, for the seen-marks reason the collection's own note
            // gives: the record's presence is what stops the next load
            // re-taking the baseline over marks the player has not reached.
            {
                const auto seen = DyeUnlocks::EncodeAcked();
                if (WriteRecord(a_intf, kRecordDyeAck, kDyeUnlockVersion, seen,
                                "dye seen-marks")) {
                    spdlog::debug("Persistence: saved {} byte(s) of dye seen-marks.",
                                  seen.size());
                }
            }
            // The hand-picked colour history. Skipped entirely when empty,
            // matching the "no empty record" convention kRecordNpc and the two
            // baselines follow.
            //
            // ⚠ THE OPPOSITE OF 'DYES' DIRECTLY ABOVE, and the difference is
            // what absence MEANS. An absent 'DYES' and an empty one decode
            // differently, because that record carries a charge and deed
            // counters that nothing else holds. An absent history and an empty
            // history both decode to no history, so there is nothing to
            // preserve by writing four bytes into every save of every player
            // who never mixed a colour.
            //
            // Lock discipline as above: the bytes come out under the lock, the
            // record write runs outside it.
            std::vector<std::byte> historyBytes;
            std::size_t            historyCount = 0;
            DyeHistory::With([&](DyeHistoryList& a_list) {
                historyCount = a_list.Size();
                if (historyCount != 0) {
                    historyBytes = a_list.Encode();
                }
            });
            if (historyCount != 0 &&
                WriteRecord(a_intf, kRecordDyeHistory, kDyeHistoryVersion, historyBytes,
                            "dye history")) {
                spdlog::debug("Persistence: wrote {} hand-picked colour(s).", historyCount);
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
            // Rules (Task 10): authoritative per save, same as 'LIBR' above -
            // always written, never skipped when empty, so a save that has
            // been through this code path once always carries an explicit
            // "empty, unpinned, engine on" record rather than relying on
            // absence to mean that. The rule set travels as the same JSON
            // document shape rules.json uses (RuleCodec::RulesToJson), so a
            // future format change to that codec is just bytes to this file.
            //
            // Unlike every other record above, this one allocates through
            // RuleCodec::RulesToJson/Json::writeString before it ever
            // reaches WriteRecord - a new risk class inside a serialization
            // callback (a bad_alloc here is narrow, but the consequence is
            // a crash mid-save, inside BGSSaveLoadManager). Guarded the same
            // way the LOAD side already is ("a load must never abort
            // partway through because of a rules record"): losing just the
            // rules record on failure beats terminating the whole save.
            try {
                const auto rules = RuleStore::Snapshot();
                const auto engineState = RuleStore::GetEngineState();

                RuleRecordFields fields;
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";  // compact: this is co-save bytes, not a human-edited file
                fields.rulesJson         = Json::writeString(wb, RuleCodec::RulesToJson(rules));
                fields.engineEnabled     = engineState.engineEnabled;
                fields.pinned            = engineState.pinned;
                fields.pinnedName        = engineState.pinnedName;
                fields.lastAppliedRuleId = engineState.lastAppliedRuleId;
                fields.disabledPackIds   = RuleStore::DisabledPackRuleIds();

                if (WriteRecord(a_intf, kRecordRule, kRuleVersion, EncodeRuleState(fields),
                                "rules")) {
                    spdlog::info(
                        "Persistence: saved {} rule(s) into the save ({}{}, {} pack override(s)).",
                        rules.size(), engineState.engineEnabled ? "engine on" : "engine off",
                        engineState.pinned ? ", pinned" : "", fields.disabledPackIds.size());
                }
            } catch (const std::exception& e) {
                spdlog::error("Persistence: building the rules record threw ({}); rules NOT "
                              "saved this time (previous co-save data kept).",
                              e.what());
            } catch (...) {
                spdlog::error("Persistence: building the rules record threw a non-standard "
                              "exception; rules NOT saved this time (previous co-save data "
                              "kept).");
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_intf) {
            // Collections: reset the library to the GLOBAL default (outfits.json) before
            // reading this save's records. A 'LIBR' record then REPLACES it with the
            // save's own outfits; a save without one (old, pre-collections) keeps the
            // global. Without this reset, loading an old save after a LIBR save in the
            // same session would inherit the previous save's library.
            LoadGlobalIntoSession();

            // Rules (Task 10): defaults to "no record" - empty rule set,
            // engine enabled, NOT pinned. This is a FRESH local on every
            // LoadCallback invocation, so there is no stale-state channel
            // for a previous save's pin to leak through even in principle;
            // haveRuleRecord only flips true a few lines below, once a
            // 'RULE' record for THIS load has actually decoded. See the
            // ⚠ hazard note on RuleEngine::ResetForLoad never touching the
            // pin - this local variable, plus RuleStore::OnSaveLoaded's own
            // reset below, are the two places that guarantee it clears.
            RuleRecordFields ruleFields;
            bool             haveRuleRecord = false;

            // Character defaults: emptied BEFORE any record is read, exactly as
            // the library is above. Without this, loading save B after save A
            // leaves A's defaults sitting in the map for every character B has
            // no row for, which is the cross-save leak in a different costume.
            //
            // ⚠ `have*Record` MUST MEAN "THE SAVE CARRIED ONE", NOT "IT
            // DECODED". They are set from GetNextRecordInfo seeing the type, so
            // a CORRUPT record still counts as present and the legacy import
            // below stays out of it. Falling back to the old global file on a
            // decode failure would put the leak straight back, silently, and on
            // exactly the save whose data we just failed to read.
            DefaultLook::ClearAll();
            bool haveDefLookRecord = false;
#if defined(FR_BODY_STUDIO)
            DefaultBody::ClearAll();
            bool haveDefBodyRecord = false;
#endif

            std::uint32_t type = 0, version = 0, length = 0;
            while (a_intf->GetNextRecordInfo(type, version, length)) {
                // ⚠⚠ EVERY TYPE WITH A HANDLER BELOW HAS TO BE NAMED HERE, and
                // two of them were not. 'KACK' and 'DACK' are written by
                // SaveCallback and each has a full decode block further down,
                // and this gate dropped both before they could reach it: the
                // seen-marks were saved every time and read back never, so a
                // look or a colour a player had earned but not yet looked at
                // came back adopted-as-seen after any reload. Silent, because
                // an unreached handler logs nothing.
                if (type != kRecord && type != kRecordKnown && type != kRecordActive &&
                    type != kRecordNpc && type != kRecordBodyBase &&
                    type != kRecordHairBase && type != kRecordHeadBase &&
                    type != kRecordDyes && type != kRecordDyeHistory &&
                    type != kRecordRule && type != kRecordDefLook &&
                    type != kRecordDefBody && type != kRecordKnownAck &&
                    type != kRecordDyeAck && type != kRecordSkin &&
                    type != kRecordOvlBase && type != kRecordMkBase &&
                    type != kRecordSkinHold && type != kRecordSex) {
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
                if (type == kRecordDyeAck) {
                    // A refusal leaves the baseline owed, the safe direction:
                    // nothing is marked new this session rather than every
                    // colour the character has ever earned.
                    if (!DyeUnlocks::DecodeAcked(bytes, version)) {
                        spdlog::warn("Persistence: refused the dye seen-marks record "
                                     "(version {}). No colour is marked new this session "
                                     "rather than all of them.",
                                     version);
                    }
                    continue;
                }
                if (type == kRecordKnownAck) {
                    // ⚠ A REFUSAL LEAVES THE BASELINE OWED, which is the safe
                    // direction and not an oversight. DecodeAcked clears the
                    // flag only on success, so a record this build cannot read
                    // ends with every known look adopted as seen rather than
                    // with the whole browser in gold. The marks are the
                    // cheapest thing in the save to lose.
                    if (!Collection::GetSingleton().DecodeAcked(bytes, version)) {
                        spdlog::warn("Persistence: refused the collection seen-marks record "
                                     "(version {}). Nothing is marked new this session "
                                     "rather than everything.",
                                     version);
                    }
                    continue;
                }
                if (type == kRecordDyeHistory) {
                    // The version goes THROUGH to Decode, a range, for the same
                    // reason spelled out for 'DYES' below. Decode is all or
                    // nothing, so a refusal leaves the list exactly as
                    // RevertCallback left it, which is cleared.
                    bool        ok    = false;
                    std::size_t count = 0;
                    DyeHistory::With([&](DyeHistoryList& a_list) {
                        ok    = a_list.Decode(bytes, version);
                        count = a_list.Size();
                    });
                    if (!ok) {
                        spdlog::warn("Persistence: refused dye history record "
                                     "(version {}, {} bytes).",
                                     version, bytes.size());
                    } else {
                        spdlog::debug("Persistence: loaded {} hand-picked colour(s).", count);
                    }
                    continue;
                }
                if (type == kRecordDyes) {
                    // The version goes THROUGH to Decode rather than being
                    // tested here. It applies DyeVersionLoadable, a range, so
                    // an older record still loads under a newer build. The
                    // `version == k...` form used by the two baseline records
                    // below would destroy data on a bump instead: the read is
                    // refused, the set stays empty, and SaveCallback writes
                    // that emptiness straight back over the good record.
                    //
                    // Decode is all or nothing, so a refusal leaves the set
                    // exactly as RevertCallback left it, which is cleared.
                    //
                    // Same lock discipline as the write above, for a smaller
                    // exposure: this path never held g_lock across the co-save
                    // READ (the bytes are already in hand by here), only across
                    // the log line. The verdict, the count and the charge come
                    // out of one With so they still describe one state, and
                    // spdlog runs outside it.
                    bool          ok        = false;
                    std::size_t   dyeCount  = 0;
                    std::uint32_t dyeCharge = 0;
                    DyeUnlocks::With([&](DyeUnlockSet& a_set) {
                        ok        = a_set.Decode(bytes, version);
                        dyeCount  = a_set.Size();
                        dyeCharge = a_set.Charge();
                    });
                    if (!ok) {
                        spdlog::warn("Persistence: refused dye unlock record "
                                     "(version {}, {} bytes).",
                                     version, bytes.size());
                    } else {
                        spdlog::info("Persistence: loaded {} dye unlock(s) from the "
                                     "save, charge {}.",
                                     dyeCount, dyeCharge);
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
                if (type == kRecordDefLook) {
                    // ⚠ SET BEFORE THE VERSION TEST. A record from a FUTURE
                    // build is still a record: the save has been through this
                    // feature, so the legacy import must not fire and hand it a
                    // different playthrough's defaults.
                    haveDefLookRecord = true;
                    // ⚠ v1 AND v2 ARE STILL READ, and that is the whole point
                    // of the version parameter: a save written before facial
                    // hair (v1) or before the default hair colour (v2) carries
                    // a shorter row, and refusing it would delete every default
                    // the player had set.
                    // ⚠ EACH ONE SPELLED OUT. This list is the half of the fix
                    // that lives outside the codec, and it has to grow by hand
                    // on every bump: the decoder accepting a version is worth
                    // nothing while this test still refuses to hand it over.
                    if (version == 1 || version == 2 || version == 3 ||
                        version == kDefLookVersion) {
                        std::vector<DefaultLookRecord> rows;
                        if (DecodeDefaultLooks(bytes, version, rows)) {
                            DefaultLook::Restore(rows);
                        } else {
                            spdlog::error("Persistence: the default-looks record did not "
                                          "decode; this save's character defaults are LOST "
                                          "rather than replaced by another save's.");
                        }
                    } else {
                        spdlog::warn("Persistence: default-looks record version {} is not {}; "
                                     "skipped.",
                                     version, kDefLookVersion);
                    }
                    continue;
                }
                if (type == kRecordDefBody) {
#if defined(FR_BODY_STUDIO)
                    haveDefBodyRecord = true;
                    if (version == kDefBodyVersion) {
                        std::vector<DefaultBodyRecord> rows;
                        if (DecodeDefaultBodies(bytes, rows)) {
                            DefaultBody::Restore(rows);
                        } else {
                            spdlog::error("Persistence: the default-bodies record did not "
                                          "decode; this save's default bodies are LOST rather "
                                          "than replaced by another save's.");
                        }
                    } else {
                        spdlog::warn("Persistence: default-bodies record version {} is not {}; "
                                     "skipped.",
                                     version, kDefBodyVersion);
                    }
#else
                    // ⚠ READ AND DROPPED ON A BUILD WITHOUT BODY STUDIO, not
                    // treated as unknown. The save keeps carrying it because
                    // SaveCallback only writes what it can produce, so a
                    // player moving between channels loses their default body
                    // the first time they save on this one. Stated rather than
                    // hidden: it is the same one-way cost the channel split
                    // already has for every other Body Studio field.
                    spdlog::info("Persistence: this save carries default bodies and this build "
                                 "has no Body Studio; they are ignored, and dropped on the "
                                 "next save.");
#endif
                    continue;
                }
                if (type == kRecordHairBase) {
                    if (version == kHairBaseVersion) {
                        HairColor::CapturedColor captured;
                        captured.captured =
                            DecodeHairBaseline(bytes, captured.modName, captured.localFormID);
                        if (captured.captured) {
                            // Seed, never overwrite: if the pass order ever puts a
                            // live capture before this runs, the live one is newer
                            // and SeedCaptured leaves it alone (see its own
                            // comment in HairColor.cpp).
                            HairColor::SeedCaptured(RE::PlayerCharacter::GetSingleton(),
                                                    captured);
                            spdlog::info("Persistence: hair colour baseline restored "
                                         "('{}' {:06X}).",
                                         captured.modName, captured.localFormID);
                        } else {
                            spdlog::warn("Persistence: malformed hair colour baseline "
                                         "record; ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordSex) {
                    if (version == kSexVersion) {
                        bool female = false;
                        if (DecodeCharacterSex(bytes, female)) {
                            CharacterSex::Hold(female);
                            // ⛔⛔ THE BASE IS NOT WRITTEN HERE. It was, for one
                            // build, on the reasoning that the body is BUILT
                            // from this flag so it should be right before the
                            // build rather than corrected after. That build
                            // CTD'd on load, twice, with no log surviving
                            // (2026-08-25 09:2x), and this is the only thing in
                            // it that touches an engine structure from inside
                            // the deserialization callback.
                            //
                            // ⚠ NOT PROVEN, because there is no log. It is the
                            // prime suspect on shape alone: every other reassert
                            // this mod does runs at kPostLoadGame with the
                            // player fully constructed, and none of them has
                            // ever crashed. The write now happens there too.
                            // If a load still CTDs with this moved, the suspect
                            // is wrong and the next step is rolling back to
                            // 72c49ee, which was field-tested.
                            spdlog::info("Persistence: this character's look says {}; "
                                         "held for the boundary to put back.",
                                         female ? "female" : "male");
                        } else {
                            spdlog::warn("Persistence: malformed character sex record; "
                                         "ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordFaceTint) {
                    if (version == kFaceTintVersion) {
                        std::string jslot;
                        if (DecodeLookFaceTint(bytes, jslot)) {
                            // ⚠ HELD, NOT BOUND. The bind is a skee node
                            // override on the face geometries and there is no
                            // face here: this load's head build has not run
                            // yet, and the one before it belonged to the
                            // outgoing character. HeadBuildHook puts it on
                            // after every build, which is the only seam that
                            // re-applies a node override at all.
                            LookFaceTint::Hold(jslot);
                            spdlog::info("Persistence: this character's face is baked into "
                                         "'{}'; held for the head build to put back.",
                                         jslot);
                        } else {
                            spdlog::warn("Persistence: malformed look face tint record; "
                                         "ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordSkinHold) {
                    if (version == kSkinHoldVersion) {
                        std::uint8_t r = 0, g = 0, b = 0;
                        float        strength = 1.0f;
                        if (DecodeSkinToneHold(bytes, r, g, b, strength)) {
                            // ⚠ THE REASSERT DOES THE REST. It is already wired
                            // to the head build that takes the tone away
                            // (HeadBuildHook); all that was ever missing was a
                            // held value on the far side of a load.
                            MakeupApi::HoldSkinTone(OverlayPlan::Rgb{ r, g, b }, strength);
                            spdlog::info("Persistence: restored the held skin tone "
                                         "({},{},{}) at {:.3f}. The reassert puts it "
                                         "back after each head build.",
                                         r, g, b, strength);
                        } else {
                            spdlog::warn("Persistence: malformed skin tone hold record; "
                                         "ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordOvlBase) {
                    if (version == kOvlBaseVersion) {
                        // ⚠ THE WHOLE BLOCK IS GUARDED. This parses untrusted
                        // save bytes through jsoncpp, and the load path's
                        // standing rule is that one bad record must never abort
                        // a load partway; the rules record above is guarded for
                        // the same reason.
                        try {
                            std::string json;
                            if (!DecodeOverlayBaseline(bytes, json)) {
                                spdlog::warn("Persistence: malformed overlay baseline "
                                             "record; ignoring it.");
                            } else if (json.empty()) {
                                spdlog::info("Persistence: the overlay baseline record "
                                             "is empty, so this character has had no "
                                             "overlay written by us.");
                            } else {
                                Json::Value  root;
                                Json::Reader reader;
                                ProfileCodec::ProfileParse parsed;
                                std::string                error;
                                if (!reader.parse(json, root) ||
                                    !ProfileCodec::ParseProfile(root, parsed, error)) {
                                    spdlog::warn("Persistence: the overlay baseline "
                                                 "record did not parse ({}); ignoring "
                                                 "it.",
                                                 error.empty() ? "bad JSON" : error);
                                } else if (!parsed.profile.overlays) {
                                    spdlog::warn("Persistence: the overlay baseline "
                                                 "record parsed with no overlays block; "
                                                 "ignoring it.");
                                } else {
                                    std::size_t layers = 0;
                                    for (const auto& entries :
                                         parsed.profile.overlays->byLocation) {
                                        layers += entries.size();
                                    }
                                    OverlayBaseline::Restore(*parsed.profile.overlays);
                                    // ⚠ THE WORDING FOLLOWS OverlayBaseline::ReassertRecord,
                                    // which has three verdicts, not the one this line
                                    // used to claim: the 09-16 handoff read "only if
                                    // skee holds none" off it and priced a design
                                    // question the code had already answered.
                                    spdlog::info(
                                        "Persistence: restored the player's overlay art "
                                        "({} layer(s)). It is re-asserted once skee has "
                                        "restored its own copy: a store holding none gets "
                                        "it back, one holding a different set loses to it "
                                        "and the extras come off, one that agrees is "
                                        "pushed once anyway.",
                                        layers);
                                }
                            }
                        } catch (const std::exception& e) {
                            spdlog::error("Persistence: the overlay baseline record threw "
                                          "({}); ignoring it.",
                                          e.what());
                        } catch (...) {
                            spdlog::error("Persistence: the overlay baseline record threw; "
                                          "ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordMkBase) {
                    if (version == kMkBaseVersion) {
                        // Guarded like the overlay baseline above: untrusted
                        // JSON inside a serialization callback.
                        try {
                            std::string json;
                            if (!DecodeOverlayBaseline(bytes, json)) {
                                spdlog::warn("Persistence: malformed makeup baseline "
                                             "record; ignoring it.");
                            } else if (json.empty()) {
                                spdlog::info("Persistence: the makeup baseline record "
                                             "is empty, so this character has had no "
                                             "tint written by us.");
                            } else {
                                Json::Value  root;
                                Json::Reader reader;
                                ProfileCodec::ProfileParse parsed;
                                std::string                error;
                                if (!reader.parse(json, root) ||
                                    !ProfileCodec::ParseProfile(root, parsed, error)) {
                                    spdlog::warn("Persistence: the makeup baseline "
                                                 "record did not parse ({}); ignoring "
                                                 "it.",
                                                 error.empty() ? "bad JSON" : error);
                                } else if (!parsed.profile.makeup) {
                                    // A wrapper with no makeup block IS the
                                    // bare claim: the codec omits an empty
                                    // block, and the save only writes the
                                    // wrapper when the record claims something.
                                    MakeupBaseline::Restore({}, parsed.profile.character);
                                    spdlog::info(
                                        "Persistence: restored the player's tint list "
                                        "as a bare claim (nothing worn, {}). Whatever "
                                        "the game restores beyond the tone is cleared "
                                        "after skee restores its own copy.",
                                        parsed.profile.character
                                            ? "stamped with its character"
                                            : "unstamped");
                                } else {
                                    MakeupBaseline::Restore(*parsed.profile.makeup,
                                                            parsed.profile.character);
                                    spdlog::info(
                                        "Persistence: restored the player's tint list "
                                        "({} worn entr{}, {}). It is re-asserted after "
                                        "skee restores its own copy.",
                                        parsed.profile.makeup->size(),
                                        parsed.profile.makeup->size() == 1 ? "y"
                                                                           : "ies",
                                        parsed.profile.character
                                            ? "stamped with its character"
                                            : "unstamped, an older build's record");
                                }
                            }
                        } catch (const std::exception& e) {
                            spdlog::error("Persistence: the makeup baseline record threw "
                                          "({}); ignoring it.",
                                          e.what());
                        } catch (...) {
                            spdlog::error("Persistence: the makeup baseline record threw; "
                                          "ignoring it.");
                        }
                    }
                    continue;
                }
                if (type == kRecordHeadBase) {
                    if (version == kHeadBaseVersion) {
                        std::vector<HeadPartBaselineRow> rows;
                        if (DecodeHeadPartBaseline(bytes, rows)) {
                            std::vector<HeadPart::Capture> captures;
                            captures.reserve(rows.size());
                            for (const auto& row : rows) {
                                captures.push_back(
                                    HeadPart::Capture{ row.slot, row.modName, row.localFormID });
                            }
                            HeadPart::AdoptCaptures(captures);
                        } else {
                            // ⚠ THE REFUSAL LEAVES THE MAP EMPTY, and empty is the
                            // safe direction here: Base gear then puts nothing back
                            // rather than putting a stranger's face back. The map
                            // was cleared by RevertCallback moments ago and this is
                            // the only thing that refills it.
                            spdlog::warn("Persistence: malformed head part baseline record; "
                                         "ignoring it. Base gear will have nothing to "
                                         "restore for this character.");
                        }
                    }
                    continue;
                }
                if (type == kRecordSkin) {
                    // ⚠ VERSION TESTED WITH <=, NOT ==. A record from a build
                    // that writes v1 must stay readable when this becomes v2,
                    // or the one list of overrides this mod can take off again
                    // is thrown away on the bump. cosave-version-equality-
                    // destroys-on-bump is the memory; the decoder takes the
                    // version, because v1 has no head list and v2 does.
                    if (version <= kSkinVersion) {
                        std::vector<SkinRow> rows;
                        if (DecodeSkinRows(bytes, rows, version)) {
                            std::size_t written = 0, head = 0, bare = 0;
                            for (const auto& row : rows) {
                                written += row.skin.written.size();
                                head += row.skin.head.size();
                                bare += row.skin.bare.size();
                            }
                            spdlog::info("Persistence: loaded skin rows for {} actor(s), {} "
                                         "armour override(s), {} head override(s), {} bare "
                                         "skin override(s) (v{}).",
                                         rows.size(), written, head, bare, version);
                            SkinApi::Restore(std::move(rows));
                        } else {
                            // ⚠ THE REFUSAL LEAVES THE STORE EMPTY, and here that
                            // is the WORSE direction: skee still holds every
                            // override this mod wrote, and with the list gone
                            // Default cannot find them. Said out loud for exactly
                            // that reason.
                            spdlog::warn("Persistence: malformed skin record; ignoring it. "
                                         "Skin overrides RaceMenu still holds for this save "
                                         "cannot be listed, so Default may leave some on.");
                        }
                    } else {
                        spdlog::warn("Persistence: skin record v{} is newer than this build's "
                                     "v{}; ignoring it.",
                                     version, kSkinVersion);
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
                if (type == kRecordRule) {
                    // A version we don't understand, or bytes that don't
                    // decode cleanly, are refused exactly like every other
                    // record above: ignored, not fatal, and haveRuleRecord
                    // stays false so this save falls back to the "no
                    // record" default (empty rules, unpinned) applied after
                    // the loop, never a half-trusted partial record.
                    if (version == kRuleVersion) {
                        RuleRecordFields parsed;
                        if (DecodeRuleState(bytes, parsed)) {
                            ruleFields     = std::move(parsed);
                            haveRuleRecord = true;
                        } else {
                            spdlog::warn("Persistence: refused rules record (malformed); this "
                                         "save's rules start empty and unpinned.");
                        }
                    } else {
                        spdlog::warn("Persistence: refused rules record (version {}, expected "
                                     "{}); this save's rules start empty and unpinned.",
                                     version, kRuleVersion);
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
                        // OS-100 census. "Loaded N assignments" says nothing
                        // about COLOUR, and colour is the thing reported
                        // missing. It rides inside each entry's inner LIBR, so
                        // a version mapping that silently hands the inner
                        // decoder the wrong LIBR number drops every tint while
                        // the outer count still reads perfectly healthy - which
                        // is exactly the failure NpcoInnerLibrVersion's own
                        // header warns has reached a build twice. Counting the
                        // tints here separates "the data never came back" from
                        // "the data came back and was never painted", and those
                        // two have completely different fixes.
                        std::size_t outfits = 0;
                        std::size_t tinted  = 0;
                        for (const auto& [npcKey, rec] : npcMap) {
                            for (const auto& outfit : rec.library.All()) {
                                ++outfits;
                                if (outfit.hairTint.set) {
                                    ++tinted;
                                }
                            }
                        }
                        spdlog::info("Persistence: NPC payload carries {} outfit(s), {} with a "
                                     "hair colour (NPCO v{} -> LIBR v{}).",
                                     outfits, tinted, version, NpcoInnerLibrVersion(version));
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
                // ⚠ THE SAME CENSUS THE NPC RECORD ABOVE ALREADY TAKES, and for
                // the same reason: "loaded N outfits" says nothing about COLOUR,
                // and colour is the thing being reported missing. The player
                // half was never counted, so a hair tint that failed to decode
                // and a hair tint that decoded and was never painted produced an
                // identical, perfectly healthy-looking load line. Those two have
                // completely different fixes.
                //
                // The ACTIVE outfit is named separately from the count because
                // it is the only one the reassert at kPostLoadGame will look at:
                // a library holding nine tinted outfits and an active one
                // carrying none is a silent no-op there, and reads here as
                // "9 outfits, 9 with a hair colour" if the active one is not
                // called out.
                std::size_t tinted = 0;
                for (const auto& outfit : lib.All()) {
                    if (outfit.hairTint.set) {
                        ++tinted;
                    }
                }
                if (const auto* active = lib.Active()) {
                    spdlog::info("Persistence: {} of them carry a hair colour; the active "
                                 "one is '{}' and it {}.",
                                 tinted, active->name,
                                 active->hairTint.set
                                     ? "carries one"
                                     : "does NOT, so a load-time reassert has nothing "
                                       "to put back");
                } else {
                    spdlog::info("Persistence: {} of them carry a hair colour, and NO outfit "
                                 "is active in this save (Base gear), so a load-time "
                                 "reassert has nothing to put back.",
                                 tinted);
                }
                OutfitSession::GetSingleton().OnLoad(std::move(lib));
            }

            // ---- the one-time hand-over from the retired global files -------
            //
            // A save from before 2026-08-08 carries no defaults record, and the
            // player's assignments are sitting in default-looks.json and
            // default-bodies.json where every save could see them. Read them in
            // ONCE, here, so nobody loses a default they set; the write above
            // then puts a record on this save and this branch never runs for it
            // again. The files are left on disk untouched rather than deleted,
            // because a second save that has not been loaded yet still needs
            // them, and because deleting a player's data on an upgrade is not
            // ours to do.
            //
            // ⚠ GATED ON "THE SAVE CARRIED A RECORD", NEVER ON "THE MAP IS
            // EMPTY". A character with no defaults is an ordinary state, and
            // keying on emptiness would re-import the global file on every load
            // for every such save, forever, which is exactly the leak.
            if (!haveDefLookRecord) {
                if (const auto n = DefaultLook::ImportLegacyFile(); n > 0) {
                    spdlog::info("Persistence: took {} default look(s) into this save from the "
                                 "old global file. They are this save's now.",
                                 n);
                }
            }
#if defined(FR_BODY_STUDIO)
            if (!haveDefBodyRecord) {
                if (const auto n = DefaultBody::ImportLegacyFile(); n > 0) {
                    spdlog::info("Persistence: took {} default body/bodies into this save from "
                                 "the old global file. They are this save's now.",
                                 n);
                }
            }
#endif

            // Rules (Task 10). haveRuleRecord false (no record, wrong
            // version, or malformed bytes) takes the SAME path as an empty
            // parse below: RuleStore::OnSaveLoaded(emptyRuleSet) and
            // RuleStore::SetEngineState(EngineState{}) - engine enabled,
            // NOT pinned. This is deliberately NOT the outfits.json
            // fallback LoadGlobalIntoSession uses above: generic outfit
            // names recur across characters, so importing another
            // character's rules here would make them fire on a save whose
            // owner never authored them (RuleStore::OnSaveLoaded's own
            // comment; the explicit import path is the "Load shared rules"
            // button, Task 13).
            Rules::RuleSet parsedRules;
            if (haveRuleRecord && !ruleFields.rulesJson.empty()) {
                // RuleCodec::JsonToRules is documented never to throw (every
                // jsoncpp accessor it uses is type-guarded first), but this
                // is untrusted co-save bytes read with JSON_USE_EXCEPTION=1
                // - guard the call the same defensive way RuleStore::LoadSeed
                // already guards this exact call chain for rules.json. A
                // load must never abort partway through because of a rules
                // record; every OTHER record here already tolerates this by
                // being written not to throw at all.
                try {
                    Json::Value              rulesRoot;
                    Json::CharReaderBuilder  rb;
                    std::string              jsonErrs;
                    std::istringstream       jsonIn(ruleFields.rulesJson);
                    std::string              parseError;
                    std::vector<std::string> warnings;
                    if (Json::parseFromStream(rb, jsonIn, &rulesRoot, &jsonErrs) &&
                        RuleCodec::JsonToRules(rulesRoot, parsedRules, parseError, &warnings)) {
                        // Conditions AND, so a silently dropped clause makes
                        // its rule fire MORE often, and a rule dropped to
                        // zero clauses becomes unconditional - log every
                        // warning (Task 9's codec already collects them
                        // per-clause).
                        for (const auto& w : warnings) {
                            spdlog::warn("Persistence: 'RULE' record: {}", w);
                        }
                        spdlog::info("Persistence: loaded {} rule(s) from the save.",
                                     parsedRules.size());
                    } else {
                        spdlog::warn(
                            "Persistence: 'RULE' record's rule set rejected ({}); this save's "
                            "rules start empty.",
                            parseError.empty() ? jsonErrs : parseError);
                        parsedRules.clear();
                    }
                } catch (const std::exception& e) {
                    spdlog::warn(
                        "Persistence: 'RULE' record's rule set threw while parsing ({}); this "
                        "save's rules start empty.",
                        e.what());
                    parsedRules.clear();
                } catch (...) {
                    spdlog::warn("Persistence: 'RULE' record's rule set threw a non-standard "
                                 "exception while parsing; this save's rules start empty.");
                    parsedRules.clear();
                }
            }
            RuleStore::OnSaveLoaded(std::move(parsedRules));

            // Pin/engine-enabled/last-applied-id restore. An outer 'RULE'
            // record that decoded cleanly (haveRuleRecord) is trusted for
            // these scalars even if the NESTED rules JSON above failed to
            // parse - the two failures are independent (one binary decode,
            // one JSON parse), so a damaged rule LIST must not also throw
            // away an otherwise-good pin. ⚠ The `else` (no record at all,
            // or the outer decode itself failed) is what clears the pin:
            // RuleEngine::ResetForLoad deliberately never touches
            // pinned_/pinnedName_, so THIS default - a value-initialized
            // EngineState{}, engine enabled, NOT pinned - is what stops a
            // pinned save from leaking its pin into the next save loaded
            // in the same session.
            RuleStore::EngineState engineState;
            if (haveRuleRecord) {
                engineState.engineEnabled     = ruleFields.engineEnabled;
                engineState.pinned            = ruleFields.pinned;
                engineState.pinnedName        = ruleFields.pinnedName;
                engineState.lastAppliedRuleId = ruleFields.lastAppliedRuleId;
            }
            RuleStore::SetEngineState(engineState);
            if (haveRuleRecord) {
                // RuleStore::OnSaveLoaded (just above) already reset every
                // pack rule's enabled flag back to its authored default;
                // reapply THIS save's overrides now. A disabled id naming a
                // pack rule that is not currently loaded is a documented
                // no-op (SetPackRuleEnabled), same tolerance as a rule
                // naming a missing plugin.
                for (const auto& id : ruleFields.disabledPackIds) {
                    RuleStore::SetPackRuleEnabled(id, false);
                }
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
                            // OS-100 instrumentation. This walk is the ONLY
                            // thing that repaints an assigned NPC on a load -
                            // RefreshActor calls HairColor::Repaint
                            // unconditionally - and it is high-process only, so
                            // a follower who is not high when the load callback
                            // runs is never touched at all (that is OS-92's gap,
                            // reached from the load side). Silent before this:
                            // whether she was walked, matched, or missed all
                            // produced exactly zero lines, so a missing colour
                            // could not be told from a colour that was painted
                            // and then erased by a later engine head rebuild
                            // (OS-93, which she is - "no HeadRelatedData;
                            // geometry-only tint, not durable across a head
                            // rebuild" appears 88 times in the 02:14 log).
                            std::size_t high    = 0;
                            std::size_t matched = 0;
                            processLists->ForEachHighActor(
                                [&](RE::Actor* a_actorPtr) -> RE::BSContainer::ForEachResult {
                                    if (!a_actorPtr) {
                                        return RE::BSContainer::ForEachResult::kContinue;
                                    }
                                    auto& a_actor = *a_actorPtr;
                                    ++high;
                                    if (const auto* base = a_actor.GetActorBase();
                                        base && npcSnapshot->contains(base->GetFormID())) {
                                        ++matched;
                                        spdlog::info(
                                            "Persistence: deferred NPC refresh queued for '{}' "
                                            "0x{:08X} (base 0x{:08X}).",
                                            a_actor.GetName(), a_actor.GetFormID(),
                                            base->GetFormID());
                                        OutfitSession::RequestRefreshActor(a_actor.GetHandle());
                                    }
                                    return RE::BSContainer::ForEachResult::kContinue;
                                });
                            spdlog::info(
                                "Persistence: deferred NPC refresh walked {} high actor(s), "
                                "matched {} of {} assigned. An assigned follower absent from "
                                "that match count was NEVER refreshed on this load.",
                                high, matched, npcSnapshot->size());
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
            // Instruments (field 2026-09-02 02:41): the list and the base's
            // saved layers as our records finish reading, before any converge.
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                MakeupApi::DumpWorn(player, "load, after the records");
                MakeupApi::DumpSavedLayers(player, "load, after the records");
            }
        }

        void RevertCallback(SKSE::SerializationInterface*) {
            // The watchdog's clock restarts BEFORE the clears below, so the
            // baseline it prints says what the outgoing session left in the
            // base, the charGenRace pair and skee's store as the boundary
            // opens.
            AppearanceWatch::NoteLoadBoundary("revert");
            // ⚠⚠ THE OVERLAY ART CLEARS HERE, NOT AT kPostLoadGame, AND THE
            // FIRST CUT PUT IT IN THE WRONG CALLBACK. Revert runs BEFORE
            // LoadCallback; kPostLoadGame runs after it. Clearing there wiped
            // the record LoadCallback had just restored, so the reassert found
            // an empty baseline and stood down without a word. Field 2026-08-25
            // 08:00, the whole chain in three lines:
            //
            //   08:00:22.493  Persistence: restored the player's overlay art (7 layer(s))
            //   08:00:29.531  BASELINE after 'post-load': ... layers=0 (0 with art) clones 3p=7
            //   (and no OverlayBaseline line at all, because HasAny was false)
            //
            // Revert is the one event both a load and a new game cross, which
            // is the same reasoning coc-from-main-menu-skips-newgame arrived at
            // when a skin-tone hold outlived its character.
            // Instruments (field 2026-09-02 02:41): what the OUTGOING session
            // leaves in the live list and in the base's saved layers as the
            // boundary opens, so the post-load reading can say what the load
            // itself replaced and what merely survived it.
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                MakeupApi::DumpWorn(player, "revert, before the load");
                MakeupApi::DumpSavedLayers(player, "revert, before the load");
            }
            OverlayBaseline::Clear();
            MakeupBaseline::Clear();
            // Same seam, same reason: the sex a look stated belongs to the
            // outgoing character, and LoadCallback puts the incoming one's back
            // a moment later. An absent record correctly leaves the base with
            // whatever the game gives it.
            // ⚠⚠ BEFORE Clear(), AND IT IS A DIFFERENT JOB. Clear drops what a
            // look STATED; this puts back what the ENGINE had before we wrote
            // the flag onto base 00000007, which is one form shared by every
            // character in every save. Without it, applying a look whose sex
            // differs and then loading another save gives that save's character
            // the outgoing look's body.
            CharacterSex::RestoreBaseline(RE::PlayerCharacter::GetSingleton());
            CharacterSex::Clear();
            // Same seam, and a sharper reason than the sex flag's. The export
            // lives under a shared folder keyed by LOOK NAME and not by
            // character, so a hold that outlived its save is exactly how the
            // face bled onto the Nord on 2026-08-25 at 00:15.
            LookFaceTint::Clear();
            // ⚠⚠ THE LIBRARY IS GLOBAL ONLY WHEN THE PLAYER SAYS IT IS, AND
            // THIS COMMENT USED TO SAY IT ALWAYS WAS. With [General]
            // bOutfitsShared off, outfits belong to the character who made
            // them, and the seed that enforced that hung on kNewGame alone.
            // `coc` typed at the main menu reverts and then delivers NEITHER
            // kNewGame NOR kPostLoadGame (coc-from-main-menu-skips-newgame), so
            // a character started that way kept the global library that
            // LoadLibraryFileAtStartup put in the session at kDataLoaded and
            // read exactly as though sharing were on. Reported from the field
            // 2026-08-26: "share outfits across characters is always active even
            // when I have the toggle off." It was, on that one path.
            //
            // ⚠⚠ REVERT IS THE EVENT BOTH STARTS CROSS, which is the same
            // conclusion the overlay art above and the skin-tone hold before it
            // both arrived at. That memory names it outright as the fix shape.
            //
            // ⚠ THIS COSTS THE UPGRADE PATH NOTHING, and that is worth checking
            // rather than assuming, because a save with no 'LIBR' record is
            // MEANT to fall back to outfits.json. It still does: LoadCallback
            // calls LoadGlobalIntoSession() as its very first statement, so the
            // fallback re-reads the file for itself and never depended on what
            // revert happened to leave behind. A coc'd start reaches no
            // LoadCallback at all, which is exactly why it must be empty here.
            if (!Settings::GetSingleton().outfitsShared) {
                OutfitSession::GetSingleton().OnLoad(OutfitLibrary{});
                // ⚠ SAID OUT LOUD, because the whole coc diagnosis was only
                // possible last time because the declining path logged. An
                // absent line is a reading only if the path was built to speak.
                spdlog::info("Persistence: revert, so the outfit library resets to EMPTY "
                             "([General] bOutfitsShared is off). A load re-reads "
                             "outfits.json a moment from now; a new game or a coc does "
                             "not, which is the point.");
            }
            OutfitSession::GetSingleton().OnRevert();
            OutfitSession::GetSingleton().OnNpcRevert();
            Collection::GetSingleton().Revert();
            // Earned colours, charge and deeds, for exactly the reason the
            // collection reset directly above exists. Revert runs before every
            // load, and without this a session that loads a second save hands
            // the new character the first one's palette and their Seamstone.
            // Worse than a stale read, because SaveCallback writes the record
            // unconditionally: the borrowed colours would be stamped into the
            // second character's own save as if they had earned them, and
            // unlocks are add only, so nothing ever takes them back.
            DyeUnlocks::With([](DyeUnlockSet& a_set) { a_set.Clear(); });
            // The seen-marks go with the colours they describe, and the clear
            // re-arms the baseline so a new game adopts its starting palette
            // instead of greeting the player in gold.
            DyeUnlocks::ClearAcked();
            // The hand-picked colours are the character's too, and for the same
            // reason: a session that loads a second save would otherwise show
            // the new character the first one's history, and the next save
            // would write it into their record as if they had mixed it.
            DyeHistory::With([](DyeHistoryList& a_list) { a_list.Clear(); });
            // The character defaults are per-save now, so they reset here for
            // the same reason as everything above it. This is the FIRST of two
            // independent clears; LoadCallback empties them again before it
            // reads a record. Both, deliberately: revert runs before every load
            // and covers the paths LoadCallback does not, and SaveCallback
            // writes these records unconditionally, so a borrowed default would
            // be stamped into the second character's own save.
            DefaultLook::ClearAll();
#if defined(FR_BODY_STUDIO)
            DefaultBody::ClearAll();
#endif
            // Rules are per-save too (RuleStore.h). Revert runs before every
            // load, same as everything else in this function - this is the
            // first of two independent resets to "not pinned" (the second is
            // LoadCallback's own fresh-every-call local default), so the pin
            // cannot survive into whichever save loads next.
            RuleStore::OnRevert();
            // The rules engine's own live state (pin, dwell clock, combat
            // latch) is per-save too, same reasoning as RuleStore::OnRevert
            // just above - not wired from plugin.cpp because this callback,
            // not an SKSE message, is the actual "revert" event source.
            WorldWatch::OnRevert();
            // The baseline belongs to the CHARACTER, so it must not survive
            // into another save. Revert runs before every load; a stale
            // baseline here would send the next character's "Your usual body"
            // to the previous character's preset.
            ObodyApi::SetBaseline({}, false);
            // Same reasoning, for hair colour, and worse if missed: every
            // captured baseline in HairColor's map describes a character we are
            // about to stop being. Left in place, a stale entry is not just
            // read and ignored - SaveCallback would persist the WRONG
            // character's original into THIS save's own HCOL record, and a
            // later Restore on the new character would then actively paint
            // that foreign colour onto them as if it were theirs.
            HairColor::Clear();
            // ⚠⚠ THE STYLE HALF OF THE LINE ABOVE, AND IT WAS MISSING. The
            // head-part captures are keyed on the actor's form id and the
            // player is 0x14 in every save, so one character's "what they had
            // before" sat in the map describing the next one. It is not a stale
            // read either: the ladder's bottom rung is Restore, and it runs on
            // the LOAD path, so the incoming character was actively written the
            // outgoing character's hair (field 2026-08-16, reported as hair
            // leaking across characters and as Base gear leaving the hair on).
            // HeadPart::Clear has said "for a save revert" since it was
            // written; nothing called it. The 'HPBS' record is what puts THIS
            // save's own captures back a moment later.
            HeadPart::Clear();
            // The face half of any in-flight capture was aimed at the
            // character being torn down: its SaveCharacter, settling after
            // this load, would snapshot the INCOMING character's head under
            // the outgoing look's name (field 2026-08-22 03:2x, the faceless
            // cross-save apply). Every pending face wait abandons here.
            ProfileCapture::OnRevert();
            // Same reasoning again, and this one holds ENGINE OBJECTS rather
            // than colour bytes. Every entry in OutfitDye's swap map names a
            // BSGeometry and a BSLightingShaderProperty belonging to 3D that
            // this load is tearing down, keyed on a FormID that will mean
            // something else in a moment. Left in place, the next Restore for a
            // matching FormID writes a material into detached geometry, and the
            // reference each record holds on the displaced material never comes
            // back. Clear drops them and releases those references without
            // touching the scene.
            OutfitDye::Clear();
            // And the follower hair captures, for the plainest version of the
            // same reason: each one names scenegraph nodes on a character who
            // is being torn down. Clear() deliberately restores nothing, since
            // reaching those actors now would write to the wrong head.
            NpcHair::Clear();
            // The character-editor visit reading, for the sharpest version of
            // the same reason: it is keyed on nothing at all, it describes the
            // player as they were a moment ago, and what it authorises is
            // DELETING defaults they paid gold or Seamstone charge for. A
            // reading that survived a load would grade the next character's
            // editor visit against the previous one's face.
            HeadEditorSink::Forget();
            // The skin store is keyed by base identity and the player is the
            // same base in every save, so a stale row would tell the attach
            // observer that the incoming character wears the outgoing one's
            // pack, and it would write it. The 'SKIN' record refills this a
            // moment later with the incoming save's own rows.
            SkinApi::Revert();
            // The measured ghosts describe biped slots on actors this load is
            // tearing down, keyed on form ids that will mean something else in
            // a moment. Stale entries would tell the worn-mask shim to drop a
            // hide the incoming character's gear legitimately owns.
            MeasuredGhosts::Clear();
            // ⚠⚠ THE BLUE BODY (field 2026-08-24): apply a look, go to the main
            // menu, `coc qasmoke` into a new game, and the new character's BODY
            // came up blue while their face was right. The skin-tone hold is a
            // session static with no identity on it, and its only release lived
            // in the kNewGame / kPostLoadGame handler. `coc` from the main menu
            // delivers NEITHER message ([[coc-from-main-menu-skips-newgame]],
            // already measured for the collection revert), so the hold survived
            // and the first head build re-asserted the OLD character's tone
            // onto the new one. Only the body, because the re-assert paints
            // bodyTintColor and deliberately never calls Retint, so the face
            // kept what the engine baked from the incoming character's own
            // list.
            //
            // ⚠ HERE AND NOT ONLY IN THE MESSAGE HANDLER, because this callback
            // is the one event BOTH paths cross. The handler's call stays: a
            // release is idempotent, and two callers are cheaper than deciding
            // which single one is reached on a path nobody has field-tested
            // yet.
            MakeupApi::ReleaseSkinTone();
        }
    }

    void Register() {
        const auto* ser = SKSE::GetSerializationInterface();
        ser->SetUniqueID(BuildChannel::kSerializationUniqueId);
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

    void QueueLibrarySave(bool a_pairRules) {
        if (g_savePending.exchange(true, std::memory_order_acq_rel)) {
            return;  // one save already queued
        }
        // Task 10: pair the mirrors. Whenever the library's debounced write
        // is queued, the rules mirror queues too, so outfits.json and
        // rules.json are always rewritten from the SAME save - never a
        // stale rules.json sitting next to a freshly rewritten outfits.json
        // (or vice versa) after a player alternates between two
        // characters. Passing false here (RuleStore::QueueMirrorSave's own
        // reciprocal call into this function) is what makes the pairing
        // STRUCTURALLY one hop deep - see a_pairRules's doc comment in
        // Persistence.h - rather than merely safe by virtue of the exchange
        // above happening before this call.
        if (a_pairRules) {
            RuleStore::QueueMirrorSave(false);
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

    void SyncSharedDyeUnlocks(const char* a_reason, bool a_publish) {
        // ⚠ THE SKIP IS SAID OUT LOUD, and it is the line whose ABSENCE cost a
        // field round (user 2026-08-11). The read half used to return here in
        // silence, so a log that had never merged looked exactly like a log
        // whose merge found nothing, and "the shared file holds N" was the only
        // evidence either way. An absent log line is a reading, and this one
        // was unreadable.
        if (!Settings::GetSingleton().dyeUnlocksShared) {
            spdlog::info("Dye unlocks: sharing is OFF ([General] bDyeUnlocksShared), "
                         "so {} neither took colours from the shared file nor put any "
                         "in it. This character keeps its own.",
                         a_reason);
            return;
        }
        // ⚠ ONE Load FOR BOTH HALVES. Reading twice would let the file change
        // underneath the merge, and re-reading is also what re-arms the
        // unreadable latch, so a file that went bad since the last touch stands
        // the write below down instead of replacing it.
        const auto shared = SharedDyeUnlocks::Load();
        if (!shared) {
            spdlog::error("Dye unlocks: the shared file is present and did NOT read, so "
                          "{} left it alone. This character keeps its own colours and "
                          "nothing already in the file is lost. Repair or delete {}.",
                          a_reason, SharedDyeUnlocks::kFileName);
            return;
        }

        // ---- take -------------------------------------------------------
        SharedDyeUnlocks::MergeReport report;
        std::size_t                   held = 0;
        std::set<std::string>         mine;
        DyeUnlocks::With([&](DyeUnlockSet& a_set) {
            report = SharedDyeUnlocks::MergeInto(*shared, a_set);
            held   = a_set.Size();
            // ⚠ READ UNDER THE SAME ONE With AS THE MERGE. Two acquisitions
            // could publish a set that is not the set that was just merged
            // into, which is the same argument SaveCallback makes for taking
            // its bytes, count, charge and ids together.
            mine = a_set.Ids();
        });
        if (report.refused > 0) {
            // At this count the only reachable cause is the per-character cap.
            // Silence here would look exactly like a merge with nothing to add.
            spdlog::warn("Dye unlocks: {} shared colour(s) were REFUSED, which means "
                         "this character's unlock set is full. They remain in the "
                         "shared file and are not lost.",
                         report.refused);
        }

        // ---- and give ---------------------------------------------------
        //
        // ⚠ EVERYTHING PUBLISHED IS ALREADY COMMITTED, so this is not a second
        // way to earn. The set was built by the co-save's own record plus this
        // session's promotion, and a colour promotion granted but no save has
        // banked is lost on the next load exactly as before: the reload reverts
        // the set and this function has nothing to publish. The save is still
        // the commitment. What went away is the WAIT, which is what the field
        // hit: colours committed two saves ago used to need another save before
        // any other character could see them.
        auto       merged = *shared;
        if (a_publish) {
            merged.insert(mine.begin(), mine.end());
        }
        const auto added  = merged.size() - shared->size();
        if (added == 0 && report.gained == 0) {
            spdlog::info("Dye unlocks: shared file and this character already agree "
                         "({} colour(s) each way, {} held) at {}.",
                         shared->size(), held, a_reason);
            return;
        }
        if (added > 0 && !SharedDyeUnlocks::Save(merged)) {
            spdlog::error("Dye unlocks: could not write the shared file at {}. This "
                          "character's colours are safe in the co-save; they are "
                          "simply not shared yet.",
                          a_reason);
            return;
        }
        spdlog::info("Dye unlocks: {} took {} colour(s) from the shared file and put {} "
                     "into it. This character holds {}; the file holds {}.",
                     a_reason, report.gained, added, held, merged.size());
    }

    void SyncSharedCollection(const char* a_reason, bool a_publish) {
        // The skip is said out loud for SyncSharedDyeUnlocks' reason: an absent
        // line and a line saying "nothing to do" are the same silence
        // otherwise, and that silence cost a field round once already.
        if (!Settings::GetSingleton().collectionShared) {
            spdlog::info("Collection: sharing is OFF ([General] bCollectionShared), so {} "
                         "neither took looks from the shared file nor put any in it.",
                         a_reason);
            return;
        }
        const auto shared = SharedCollection::Load();
        if (!shared) {
            spdlog::error("Collection: the shared file is present and did NOT read, so {} "
                          "left it alone. This character keeps its own looks and nothing "
                          "already in the file is lost. Repair or delete {}.",
                          a_reason, SharedCollection::kFileName);
            return;
        }

        auto&      collection = Collection::GetSingleton();
        const auto gained     = collection.MergeSharedIds(*shared);

        // ⚠ PUBLISHED AFTER THE MERGE, so the file ends up holding the union
        // whichever direction had more. Taken fresh rather than remembered
        // because the merge above just changed it.
        auto merged = *shared;
        if (a_publish) {
            const auto mine = collection.SharedIds();
            merged.insert(mine.begin(), mine.end());
        }
        const auto added  = merged.size() - shared->size();
        if (added == 0 && gained == 0) {
            spdlog::info("Collection: shared file and this character already agree ({} "
                         "look(s) each way) at {}.",
                         shared->size(), a_reason);
            return;
        }
        if (added > 0 && !SharedCollection::Save(merged)) {
            spdlog::error("Collection: could not write the shared file at {}. This "
                          "character's looks are safe in the co-save; they are simply "
                          "not shared yet.",
                          a_reason);
            return;
        }
        spdlog::info("Collection: {} took {} look(s) from the shared file and put {} into "
                     "it. This character knows {}; the file holds {}.",
                     a_reason, gained, added, collection.Size(), merged.size());
    }

}  // namespace OS::Persistence
