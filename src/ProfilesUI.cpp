#include "ProfilesUI.h"

#include "ChamferPanel.h"
#include "EditorNotice.h"
#include "EditorStyle.h"
#include "FoldAll.h"  // the shared framed accordion header, and expand/collapse all
#include "FuckCompat.h"
#include "Icons.h"  // the shared magnifier on both search rows
#include "OutfitTabStrip.h"  // TabGap / TabWidth, the numbers every strip lays out with
#include "OverlayApi.h"
#include "VmCall.h"  // a static call that refuses instead of dereferencing null
// Portrait.h is deliberately not included: the framed-head snapshots are
// parked (tag portrait-capture-parked) - every source reachable at the
// present thunk reads black on a Community Shaders plus upscaler rig.
#include "PresetBrowse.h"  // the RaceMenu preset browser's listings + normaliser
#include "PresetRequirements.h"  // which plugins either preset format names
#include "ProfileApply.h"
#include "ProfileCapture.h"
#include "ProfilePlan.h"
#include "Settings.h"  // replaceOnLookApply: what a look does not carry
#include "ProfileStore.h"
#include "Tutorial.h"  // the Looks page's rings, its one ask, and the empty-library requirement

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>       // clock_cast, for the preset pane's saved-on stamp
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>        // localtime_s / strftime, the same pair PaintScrape uses
#include <filesystem>
#include <fstream>  // reading a jslot to LIST what it needs; applying is untouched
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace OS::ProfilesUI {

    namespace {

        // One row of the library: the name and which blocks the file carries.
        // Deliberately NOT the whole ProfileCodec::Profile - a profile embeds
        // the outfit, the overlays and a full body preset, and the page only
        // needs them at the moment Apply is pressed, where it re-reads the
        // store. Holding payloads here would be a second copy that goes stale
        // the moment anything writes the file.
        struct Row {
            std::string                name;
            ProfilePlan::Participation blocks;
            bool                       faceCaptured{ false };
            std::size_t                droppedBlocks{ 0 };
            // The character the look was captured on, resolved to a label at
            // refresh ("Dark Elf" or, with the plugin gone, "mod|id").
            bool        hasCharacter{ false };
            bool        female{ false };
            std::string raceLabel;
            // What the look needs installed. ⚠ `faceNeedsUnknown` is separate
            // from an empty list on purpose: a look whose jslot could not be
            // read may need mods this list does not name, and saying nothing
            // would claim the opposite.
            std::vector<std::string> needs;
            std::vector<std::string> missing;
            bool                     faceNeedsUnknown{ false };
        };

        std::vector<Row> g_rows;
        std::size_t      g_rejected{ 0 };
        std::string      g_selected;  // by name; the name IS the identity

        // Per-block checkboxes for the selected row, reset to "every present
        // block" on every selection change. Absent blocks stay unchecked and
        // disabled; ProfileApply ANDs against presence anyway, so a stray
        // true here could never invent a block.
        ProfilePlan::Participation g_boxes;

        // Which of the page's two tabs is showing. FLICK's tab bar owned
        // this and reported it a frame late; the strip below is drawn by us, so
        // the lit tab is simply this and a click is the click.
        inline constexpr int kTabSaved   = 0;
        inline constexpr int kTabPresets = 1;
        int                  g_looksTab{ kTabSaved };

        std::string g_saveName;    // the Save field
        std::string g_lookFilter;    // the library's own search
        std::string g_presetFilter;  // the preset browser's

        // Case-folded substring, the match every other browse row in the
        // editor uses. Empty pattern matches everything, so a caller can hand
        // the box straight in.
        [[nodiscard]] bool MatchesFilter(const std::string& a_haystack,
                                         const std::string& a_needle) {
            if (a_needle.empty()) {
                return true;
            }
            const auto fold = [](std::string a_s) {
                std::transform(a_s.begin(), a_s.end(), a_s.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                return a_s;
            };
            return fold(a_haystack).find(fold(a_needle)) != std::string::npos;
        }

        // The search row the browse pages use, without the favourites box
        // neither list here has.
        void DrawFilterRow(const char* a_id, std::string& a_text,
                           const char* a_tipKey) {
            const auto icon = Icons::Utf8(Icons::kSearch);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(icon.c_str());
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            FUCK::InputText(a_id, &a_text);
            if (FUCK::IsItemHovered() && a_text.empty()) {
                FUCK::SetTooltip(FUCK::Translate(a_tipKey));
            }
        }

        // Set from the heal's game task when it patched at least one face
        // block in, so the next draw re-reads the store and the Face box
        // stops showing absent for a look that just gained its face.
        std::atomic<bool> g_healRefreshed{ false };
        void              NoteHealed() { g_healRefreshed.store(true); }

        // A capture's face half settles through the Papyrus VM, so the
        // profile file lands AFTER the Save press returns. The page polls the
        // store on a throttle until the name appears (or the wait runs out),
        // because a list that only refreshed on page entry would show the
        // player a library without the look they just saved.
        bool        g_capturePending{ false };
        std::string g_captureName;
        double      g_captureNext{ 0.0 };
        double      g_captureDeadline{ 0.0 };

        // Window-scope flags for the popups, the OS-29 rule: the rows draw
        // inside a child, and a popup opened there is a different popup from
        // one begun at the page tail.
        bool        g_requestMenu{ false };
        std::string g_menuFor;
        bool        g_requestDelete{ false };
        bool        g_requestOverwrite{ false };
        std::string g_actionFor;  // the row a modal is about

        // The detail pane's live name field, and which look it belongs to.
        //
        // ⚠⚠ A LOOK IS A FILE, SO THIS COMMITS ON DEACTIVATE AND NOT PER
        // KEYSTROKE. The Outfits page renames on every edit because an outfit
        // lives in memory until the library is saved; a look is a rename on
        // disk, and doing that per character typed would rewrite the file a
        // dozen times for one new name and leave a trail of them if any write
        // failed halfway.
        std::string g_nameEdit;
        std::string g_nameEditFor;
        // Why the last commit was refused, drawn under the field until the next
        // edit. Empty is the ordinary state.
        std::string g_nameNote;
        // Set by the row context menu, spent on the next frame's field.
        bool        g_focusName{ false };

        constexpr double kCapturePollSeconds = 0.25;
        constexpr double kCaptureWaitSeconds = 8.0;
        // Rows before a list earns a search box of its own.
        constexpr std::size_t kFilterFromRows = 8;

        // ---- the RaceMenu preset browser (spec W3) -------------------------
        //
        // FILENAMES ONLY, per the standing rule: the rows are stems from the
        // two CharGen folders, rescanned on page entry, and nothing ever
        // opens a jslot. The two folders stay two groups because they ride
        // two different natives on apply (the codec's FaceFolder comment).
        std::vector<PresetBrowse::Entry> g_presetRows;    // CharGen\Presets
        std::vector<PresetBrowse::Entry> g_exportedRows;  // CharGen\Exported, FR_ filtered
        std::string                      g_presetSelected;  // stem
        ProfileCodec::FaceFolder         g_presetSelectedFolder{
            ProfileCodec::FaceFolder::kPresets };
        std::string g_exportName;  // the Export head field
        bool        g_wasExporting = false;  // the export's falling edge

        void RescanPresets() {
            g_presetRows =
                PresetBrowse::ListPresets(PresetBrowse::PresetsDir(), false);
            // FR_ names are FR's own captures, reachable through their looks;
            // a second door to the same face would just be confusing.
            g_exportedRows =
                PresetBrowse::ListPresets(PresetBrowse::ExportedDir(), true);
            const auto lives = [](const std::vector<PresetBrowse::Entry>& a_rows,
                                  const std::string& a_stem) {
                for (const auto& row : a_rows) {
                    if (row.stem == a_stem) return true;
                }
                return false;
            };
            if (!g_presetSelected.empty() &&
                !lives(g_presetRows, g_presetSelected) &&
                !lives(g_exportedRows, g_presetSelected)) {
                g_presetSelected.clear();
            }
        }

        // The transient profile a preset row loads or saves as: a referenced
        // face and nothing else. No character block is fabricated - a jslot
        // is never parsed, so the race it assumes is unknowable here.
        [[nodiscard]] ProfileCodec::Profile PresetProfileFor(
            const std::string& a_stem, ProfileCodec::FaceFolder a_folder) {
            ProfileCodec::Profile profile;
            profile.name = a_stem;
            profile.face = ProfileCodec::FaceBlock{
                a_stem, ProfileCodec::FaceSource::kReferenced, 0, a_folder
            };
            return profile;
        }

        [[nodiscard]] const Row* FindRow(const std::string& a_name) {
            for (const auto& row : g_rows) {
                if (row.name == a_name) return &row;
            }
            return nullptr;
        }

        // ---- what the side pane can honestly say about a preset -------------
        //
        // ⚠⚠ FILE FACTS, NEVER FILE CONTENTS. PresetBrowse's standing rule is
        // that a `.jslot` is never parsed: RaceMenu owns that format, it has a
        // binary fallback loader, and a second reader drifts
        // (racemenu-preset-api-is-declared-not-registered). Nothing here opens
        // one. Everything below comes from the directory entry, which is a
        // different question from what is inside the file and stays true
        // however the format changes.
        //
        // ⚠ AND THE MESH PAIR IS THE ONE WORTH SHOWING. An exported character
        // is three files, and the mesh half silently did not land for the whole
        // life of the feature (measured 2026-08-23: nine jslots, nine tints,
        // zero nifs, because `Meshes\CharGen\Exported` did not exist and skee's
        // SavePath does not build a path). A pane that says the mesh is missing
        // turns that from a mystery into a line of text.
        // ⚠⚠ THE ABSENT HALF OF A REQUIREMENTS LIST, and the only half that
        // needs the engine. PresetRequirements says WHICH plugins a preset
        // names; this says which of them this load order does not have. It is
        // the same question exportHealth asks on the outfit presets page,
        // asked the same way, because a second shape would be a second answer.
        [[nodiscard]] std::vector<std::string> MissingOf(
            const std::vector<std::string>& a_plugins) {
            std::vector<std::string> missing;
            auto* const              dh = RE::TESDataHandler::GetSingleton();
            for (const auto& plugin : a_plugins) {
                if (!dh || !dh->LookupModByName(plugin)) {
                    missing.push_back(plugin);
                }
            }
            return missing;
        }

        // ⚠ A PANE IS NOT A LOG. Twenty plugin names wrapped across a narrow
        // column is a wall nobody reads, so the line names the first few and
        // counts the rest; the log carries the whole list for anyone chasing
        // one down.
        [[nodiscard]] std::string JoinCapped(const std::vector<std::string>& a_names,
                                             std::size_t                     a_cap = 6) {
            std::string out;
            for (std::size_t i = 0; i < a_names.size() && i < a_cap; ++i) {
                if (!out.empty()) {
                    out += ", ";
                }
                out += a_names[i];
            }
            if (a_names.size() > a_cap) {
                out += ", +" + std::to_string(a_names.size() - a_cap);
            }
            return out;
        }

        // Read a jslot purely to LIST what it names. ⛔ Applying one stays on
        // Papyrus CharGen; nothing here reaches an apply path.
        [[nodiscard]] std::vector<std::string> PluginsOfJslotFile(
            const std::filesystem::path& a_file, std::string& a_why) {
            std::ifstream in(a_file, std::ios::binary);
            if (!in) {
                a_why = "the file could not be opened";
                return {};
            }
            Json::Value             root;
            Json::CharReaderBuilder rb;
            std::string             errs;
            if (!Json::parseFromStream(rb, in, &root, &errs)) {
                a_why = "the file is not readable JSON";
                return {};
            }
            return PresetRequirements::PluginsFromJslot(root, a_why);
        }

        struct PresetFacts {
            bool          known{ false };   // the stat succeeded
            bool          binaryOnly{ false };  // a .slot with no .jslot beside it
            std::uintmax_t bytes{ 0 };
            std::string   saved;            // local time, or empty
            bool          exported{ false };  // came from the Exported folder
            bool          meshPresent{ false };
            bool          tintPresent{ false };

            // What the preset needs installed. `needsRead` false means the
            // file could not be understood, which the pane SAYS rather than
            // drawing an empty list that reads as "needs nothing".
            bool                     needsRead{ false };
            std::vector<std::string> needs;
            std::vector<std::string> missing;
        };

        PresetFacts g_facts;

        [[nodiscard]] std::string StampOf(const std::filesystem::path& a_file) {
            std::error_code ec;
            const auto      wt = std::filesystem::last_write_time(a_file, ec);
            if (ec) {
                return {};
            }
            // file_clock to system_clock to time_t. clock_cast is the portable
            // half of this; the rest is the same strftime PaintScrape uses, so
            // the two stamps in this plugin read alike.
            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(wt);
            const auto t   = std::chrono::system_clock::to_time_t(sys);
            std::tm    local{};
            if (localtime_s(&local, &t) != 0) {
                return {};
            }
            char buffer[32]{};
            if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local) == 0) {
                return {};
            }
            return buffer;
        }

        // Recomputed on selection change only. A stat per frame is a syscall
        // per frame for an answer that cannot move while the editor is modal.
        void RefreshFacts() {
            g_facts = PresetFacts{};
            if (g_presetSelected.empty()) {
                return;
            }
            const bool exported =
                g_presetSelectedFolder == ProfileCodec::FaceFolder::kExported;
            g_facts.exported = exported;
            const auto dir =
                exported ? PresetBrowse::ExportedDir() : PresetBrowse::PresetsDir();

            std::error_code ec;
            auto            file = dir / (g_presetSelected + ".jslot");
            if (!std::filesystem::exists(file, ec)) {
                file                  = dir / (g_presetSelected + ".slot");
                g_facts.binaryOnly    = std::filesystem::exists(file, ec);
                if (!g_facts.binaryOnly) {
                    return;
                }
            }
            g_facts.bytes = std::filesystem::file_size(file, ec);
            if (ec) {
                g_facts.bytes = 0;
            }
            g_facts.saved = StampOf(file);
            g_facts.known = true;

            if (exported) {
                g_facts.meshPresent = std::filesystem::exists(
                    PresetBrowse::ExportedMeshesDir() / (g_presetSelected + ".nif"), ec);
                g_facts.tintPresent = std::filesystem::exists(
                    PresetBrowse::ExportedTintsDir() / (g_presetSelected + ".dds"), ec);
            }

            // ⚠⚠ THE JSLOT IS OPENED HERE, AND THE RULE IT REVERSES WAS ABOUT
            // THE APPLY PATH. This pane used to answer with file facts alone
            // because "a jslot is never opened here"; reading one to LIST what
            // it names is not the same claim as applying one, and applying is
            // still Papyrus CharGen and still never IPresetInterface.
            //
            // ⚠ ONCE PER SELECTION, like every other fact in this function. A
            // parse per frame for an answer that cannot move while the editor
            // is modal would be a file read per frame.
            //
            // ⛔ A BINARY .slot CANNOT BE READ AT ALL, so it is not attempted:
            // needsRead stays false and the pane says so.
            if (!g_facts.binaryOnly) {
                std::string why;
                g_facts.needs = PluginsOfJslotFile(file, why);
                if (why.empty()) {
                    g_facts.needsRead = true;
                    g_facts.missing   = MissingOf(g_facts.needs);
                    if (!g_facts.missing.empty()) {
                        spdlog::info("ProfilesUI: preset '{}' needs {} plugin(s), {} absent: {}",
                                     g_presetSelected, g_facts.needs.size(),
                                     g_facts.missing.size(),
                                     fmt::join(g_facts.missing, ", "));
                    }
                } else {
                    spdlog::info("ProfilesUI: preset '{}': could not read what it needs: {}",
                                 g_presetSelected, why);
                }
            }
        }

        void SelectRow(const std::string& a_name) {
            g_selected = a_name;
            if (const auto* row = FindRow(a_name)) {
                g_boxes = row->blocks;
            } else {
                g_boxes = ProfilePlan::Participation{};
            }
        }

        // Rebuild the rows from the store's files. The dropped-block reasons
        // are logged here, once per refresh, because the store stays silent
        // by design and a block that quietly vanished from a look is exactly
        // the kind of thing the field log has to be able to answer.
        void Refresh() {
            auto& store = ProfileStore::GetSingleton();
            store.Load();
            const auto entries = store.Snapshot();
            g_rows.clear();
            g_rows.reserve(entries.size());
            for (const auto& entry : entries) {
                Row row;
                row.name          = entry.profile.name;
                row.blocks        = ProfilePlan::ParticipationFor(entry.profile);
                row.faceCaptured  = entry.profile.face.has_value() &&
                                    entry.profile.face->source ==
                                        ProfileCodec::FaceSource::kCaptured;
                row.droppedBlocks = entry.dropped.size();
                if (entry.profile.character) {
                    row.hasCharacter = true;
                    row.female       = entry.profile.character->female;
                    const auto& key  = entry.profile.character->race;
                    auto* const dh   = RE::TESDataHandler::GetSingleton();
                    auto* const race =
                        dh ? dh->LookupForm<RE::TESRace>(key.localFormID,
                                                         key.modName)
                           : nullptr;
                    if (race && race->GetName() && race->GetName()[0]) {
                        row.raceLabel = race->GetName();
                    } else {
                        row.raceLabel = fmt::format("{}|{:06X}", key.modName,
                                                    key.localFormID);
                    }
                }
                // ⚠⚠ THE LOOK'S OWN PLUGINS, PLUS ITS FACE'S. A look that
                // references a RaceMenu preset inherits that preset's needs:
                // its hair, its brows and its eyes come from the jslot, not
                // from the profile, and a face that cannot build is broken in
                // exactly the way this list exists to warn about.
                row.needs = PresetRequirements::PluginsFromProfile(entry.profile);
                if (entry.profile.face) {
                    const auto dir = entry.profile.face->folder ==
                                             ProfileCodec::FaceFolder::kExported
                                         ? PresetBrowse::ExportedDir()
                                         : PresetBrowse::PresetsDir();
                    std::string faceWhy;
                    const auto  faceNeeds = PluginsOfJslotFile(
                        dir / (entry.profile.face->jslot + ".jslot"), faceWhy);
                    if (faceWhy.empty()) {
                        for (const auto& plugin : faceNeeds) {
                            if (std::ranges::none_of(row.needs,
                                                     [&](const std::string& a_have) {
                                                         return _stricmp(a_have.c_str(),
                                                                         plugin.c_str()) == 0;
                                                     })) {
                                row.needs.push_back(plugin);
                            }
                        }
                    } else {
                        row.faceNeedsUnknown = true;
                    }
                }
                std::sort(row.needs.begin(), row.needs.end(),
                          [](const std::string& a_lhs, const std::string& a_rhs) {
                              return _stricmp(a_lhs.c_str(), a_rhs.c_str()) < 0;
                          });
                row.missing = MissingOf(row.needs);
                for (const auto& line : entry.dropped) {
                    spdlog::info("ProfilesUI: '{}': {}", row.name, line);
                }
                if (!row.missing.empty()) {
                    spdlog::info("ProfilesUI: look '{}' needs {} plugin(s), {} absent: {}",
                                 row.name, row.needs.size(), row.missing.size(),
                                 fmt::join(row.missing, ", "));
                }
                g_rows.push_back(std::move(row));
            }
            std::sort(g_rows.begin(), g_rows.end(),
                      [](const Row& a_a, const Row& a_b) {
                          return _stricmp(a_a.name.c_str(), a_b.name.c_str()) < 0;
                      });
            g_rejected = store.RejectedCount();
            if (!g_selected.empty() && !FindRow(g_selected)) {
                g_selected.clear();
            }
            if (!g_selected.empty()) {
                SelectRow(g_selected);  // re-seed the boxes from the fresh read
            }
        }

        // The one runtime debt ProfileStore::Delete hands back: a captured
        // FR_ jslot dies with its profile, through the same CharGen class the
        // capture wrote it with. Fire-and-forget by design - the file is
        // RaceMenu's to remove, and a refusal only means an orphan preset in
        // its browser, which the log line below is enough to find.
        class DeleteAnswer : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit DeleteAnswer(std::string a_jslot) : jslot_(std::move(a_jslot)) {}
            void operator()(RE::BSScript::Variable) override {
                spdlog::info("ProfilesUI: CharGen.DeleteCharacter answered for "
                             "'{}'.", jslot_);
            }
            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::string jslot_;
        };

        void DeleteCapturedJslot(const std::string& a_jslot) {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                spdlog::warn("ProfilesUI: no Papyrus VM; the captured jslot "
                             "'{}' stays behind in CharGen's folder.", a_jslot);
                return;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new DeleteAnswer(a_jslot)
            };
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(a_jslot));
            if (!VmCall::Static(vm, "CharGen", "DeleteCharacter", args, cb)) {
                spdlog::warn("ProfilesUI: CharGen.DeleteCharacter refused for "
                             "'{}'; the jslot stays behind.", a_jslot);
            }
        }

        void BeginCapture(RE::Actor* a_player, const std::string& a_name) {
            // The face is captured whenever the skee interfaces answered this
            // session; without them ProfileCapture degrades to a faceless
            // profile with its own log line, so the press still lands.
            ProfileCapture::Capture(a_player, a_name, OverlayApi::Available());
            g_capturePending  = true;
            g_captureName     = a_name;
            g_captureNext     = 0.0;
            g_captureDeadline = FUCK::GetTime() + kCaptureWaitSeconds;
            EditorStyle::PlayUISound("UIMenuOK");
        }

        void PollCapture() {
            if (!g_capturePending) {
                return;
            }
            const double now = FUCK::GetTime();
            if (now < g_captureNext) {
                return;
            }
            g_captureNext = now + kCapturePollSeconds;
            auto& store   = ProfileStore::GetSingleton();
            store.Load();
            if (store.Find(g_captureName)) {
                Refresh();
                // ⚠ THE LIBRARY TOO, because a saved look now writes a
                // RaceMenu preset beside its own face block and the presets
                // tab is otherwise rescanned on page entry alone. The same
                // reason the export needed a rescan on its falling edge.
                RescanPresets();
                SelectRow(g_captureName);
                g_capturePending = false;
                g_saveName.clear();
                return;
            }
            if (now > g_captureDeadline) {
                // The capture's own log lines carry the reason; the page just
                // stops claiming a save is still on its way.
                spdlog::warn("ProfilesUI: '{}' never appeared in the store "
                             "within {}s; see ProfileCapture's lines above.",
                             g_captureName, kCaptureWaitSeconds);
                g_capturePending = false;
                Refresh();
            }
        }

        // One checkbox row of the Apply list. a_present is whether the
        // profile carries the block at all; a_usable adds the face row's
        // RaceMenu gate on top.
        //
        // ⚠ a_coming IS FOR A BLOCK THAT IS ABSENT AND ON ITS WAY, which is
        // one row and one moment: the face. MEASURED, and it is what the field
        // called confusing rather than broken: a capture logs `saved
        // (face=false, ...)` and adds its face block 0.3 s later, through the
        // Papyrus VM, so the pane draws the profile before it is finished and
        // the Face box reads OFF on the look the player just saved. A disabled
        // box that is TICKED says "this look has a face and it is being
        // written"; an empty one says the face was never captured, and only one
        // of those is true. It stays disabled either way, so no press can send
        // a block the file does not carry yet.
        void BlockCheckbox(const char* a_label, bool& a_value, bool a_present,
                           bool a_usable, const char* a_absentTip,
                           const char* a_unusableTip, bool a_coming = false) {
            const bool enabled = a_present && a_usable;
            if (!enabled) {
                FUCK::BeginDisabled();
                bool off = a_coming;
                FUCK::Checkbox(a_label, &off);
                FUCK::EndDisabled();
            } else {
                FUCK::Checkbox(a_label, &a_value);
            }
            if (FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!a_present) {
                    FUCK::SetTooltip(a_absentTip);
                } else if (!a_usable) {
                    FUCK::SetTooltip(a_unusableTip);
                }
            }
        }

        // ---- the page's two tabs ------------------------------------------
        //
        // ⚠⚠ NOT FUCK::BeginTabBar ANY MORE, AND THE REASON IS THE ONE THE
        // OUTFIT STRIP RECORDED FIRST: A FLICK TAB CANNOT BE DECORATED AT ALL.
        // BeginTabItem submits the tab background AND its label in one call and
        // exposes no draw-list channel, so FLICK boxes the active tab and leaves
        // the idle ones as bare text while every other strip in this editor
        // outlines all of them. Looks was the last page still on the old widget,
        // which is exactly what the field saw (user 2026-08-25, the Looks tabs
        // do not look like the Outfits and Presets tabs).
        //
        // ⚠ NO CHILD AND NO SCROLL, the preset strip's reasoning: there are
        // exactly two tabs and both labels are ours, so the strip cannot outgrow
        // its row.
        void DrawTabStrip() {
            struct PageTab {
                const char* label;
                int         id;
            };
            const PageTab tabs[] = {
                { "$FR_Looks_TabSaved"_T, kTabSaved },
                { "$FR_Looks_PresetsHeader"_T, kTabPresets },
            };
            // The same three numbers the outfit and preset strips lay out with,
            // so all three are the same size as well as the same shape.
            const float  tabH   = ChamferPanel::FrameWidgetHeight();
            const float  padX   = OS::ui::FramePadding().x;
            const float  gap    = OutfitTabStrip::TabGap(FUCK::GetResolutionScale());
            const ImVec2 origin = FUCK::GetCursorScreenPos();

            int   clicked = -1;
            float penX    = 0.0f;
            for (int i = 0; i < IM_ARRAYSIZE(tabs); ++i) {
                const auto&  tab = tabs[i];
                const ImVec2 lsz = FUCK::CalcTextSize(tab.label);
                const float  w   = OutfitTabStrip::TabWidth(lsz.x, padX);
                FUCK::PushID(i);
                FUCK::SetCursorScreenPos(ImVec2(origin.x + penX, origin.y));
                const bool   pressed = FUCK::InvisibleButton("##looks_tab", ImVec2(w, tabH));
                const bool   hovered = FUCK::IsItemHovered();
                const ImVec2 mn      = FUCK::GetItemRectMin();
                const ImVec2 mx      = FUCK::GetItemRectMax();
                const bool   isOn    = g_looksTab == tab.id;
                ChamferPanel::PaintTabBox(mn, mx, hovered, isOn);
                // An idle tab reads as somewhere you could go rather than where
                // you are, which is the one thing worth keeping from FLICK's bar.
                OS::ui::TextAt(
                    ImVec2(mn.x + (w - lsz.x) * 0.5f, mn.y + (tabH - lsz.y) * 0.5f),
                    OS::ui::Col(isOn ? OS::ui::StyleColor(ImGuiCol_Text)
                                     : OS::ui::StyleColor(ImGuiCol_TextDisabled)),
                    tab.label);
                // The footprint is sealed back the way every hand-rolled control
                // in this editor closes: TextAt restores the cursor but not the
                // previous line's extent.
                FUCK::SetCursorScreenPos(mn);
                FUCK::Dummy(ImVec2(w, tabH));
                if (pressed) {
                    clicked = tab.id;
                }
                FUCK::PopID();
                penX += w + gap;
            }
            if (clicked >= 0 && clicked != g_looksTab) {
                g_looksTab = clicked;
                EditorStyle::PlayUISound("UIMenuFocus");
            }
        }

    }  // namespace

    void OnOpen(RE::Actor*) {
        // Faceless profiles whose FR_ jslot exists heal on page entry (the
        // orphaned pairs of the pre-fix builds, and any capture whose patch
        // was lost to a quit). The task reports back through NoteHealed and
        // the next draw re-reads.
        ProfileCapture::HealFacelessFromDisk(&NoteHealed);
        Refresh();
        RescanPresets();
        // ⚠ STATED HERE BECAUSE THE PLAN IS BUILT WHEN THE TUTORIAL STARTS, and
        // this runs on the way into the page, after Refresh has filled the
        // library. A first-ever visit has an empty library and no look to pick,
        // so the pick step is dropped from the plan rather than left ringing a
        // list with nothing in it. See Tutorial::Requires::kSavedLooks.
        Tutorial::SetRequirement(Tutorial::Requires::kSavedLooks, !g_rows.empty());
        // ⚠ THE SELECTION SURVIVES RE-ENTRY ON PURPOSE, unlike the browse
        // pages: a look is a named thing the player came back to, not a
        // framing the camera should forget. Refresh() already cleared it if
        // the file is gone.
    }

    void Draw(RE::Actor* a_player) {
        if (g_healRefreshed.exchange(false)) {
            Refresh();
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();

        // ⚠⚠ POLLED OUTSIDE THE TABS, AND THE SETTINGS PAGE'S AUDIT IS WHY.
        // A hidden tab draws nothing, so anything that has to keep running
        // while the player is looking elsewhere cannot live inside one. A save
        // settles through the Papyrus VM after the press returns, so its poll
        // is exactly that: switch to the presets tab mid-save and the library
        // still has to notice the look landing.
        PollCapture();

        // ---- the Save row ---------------------------------------------------
        //
        // ⚠ THE PAGE CAPTURES AND APPLIES THE PLAYER, whatever the editor's
        // target is (decision 4: profiles are PLAYER ONLY in v1). The line
        // below says so rather than greying the page, because reading a
        // library costs nothing on any target.
        if (a_player && player && a_player != player) {
            OS::ui::TextDisabledWrapped("$FR_Looks_PlayerOnly"_T);
            FUCK::Spacing();
        }

        // ⚠⚠ TWO BROWSERS STACKED IS ONE BROWSER BELOW THE FOLD. The page
        // held the saved-look library and the RaceMenu preset browser one
        // above the other, so reaching a preset meant scrolling past the whole
        // library first and the buttons that act on it went with it (field
        // verdict: awkward). They are two tabs now. Neither half has to share
        // the height any more, so each gets the pane it needed all along.
        DrawTabStrip();
        FUCK::Separator();

        // ⚠ AN ID PER TAB BODY, WHICH THE TAB BAR USED TO GIVE FOR FREE.
        // BeginTabItem pushed one, so the two tabs' contents hashed in separate
        // scopes; drawing the strip ourselves collapses them into the page's own
        // scope and two children could then collide by name. Pushing here keeps
        // every id in this file exactly where it was.
        const bool looksTab = g_looksTab == kTabSaved;
        if (looksTab) {
            FUCK::PushID("looks_saved");

        {
            // The name field and its button, ringed as one thing: the tutorial's
            // last card is about what Save look captures and who from, and the
            // two controls are one gesture.
            const ImVec2 saveRowTop = FUCK::GetCursorScreenPos();
            const float buttonW =
                FUCK::CalcTextSize("$FR_Looks_Save"_T).x +
                OS::ui::FramePadding().x * 4.0f;
            FUCK::SetNextItemWidth(
                std::max(OS::ui::FontSize() * 6.0f,
                         FUCK::GetContentRegionAvail().x - buttonW -
                             OS::ui::ItemSpacing().x));
            FUCK::InputText("##look_name", &g_saveName);
            FUCK::SameLine();
            const bool nameEmpty = g_saveName.find_first_not_of(" \t") ==
                                   std::string::npos;
            const auto save = ChamferPanel::Button(
                "$FR_Looks_Save"_T, 0.0f,
                nameEmpty || !player || g_capturePending);
            if (save.clicked && player && !nameEmpty && !g_capturePending) {
                if (ProfileStore::GetSingleton().NameAvailable(g_saveName)) {
                    BeginCapture(player, g_saveName);
                } else {
                    // Same name = same look being re-saved; ask before the
                    // old capture is replaced, the delete modal's manners.
                    g_actionFor        = g_saveName;
                    g_requestOverwrite = true;
                }
            }
            if (save.hovered) {
                FUCK::SetTooltip(g_capturePending ? "$FR_Looks_SavePendingTip"_T
                                 : nameEmpty     ? "$FR_Looks_NameHint"_T
                                                 : "$FR_Looks_SaveTip"_T);
            }
            Tutorial::PublishAnchor(
                Tutorial::Anchor::kLooksSave, saveRowTop,
                ImVec2(saveRowTop.x + FUCK::GetContentRegionAvail().x,
                       FUCK::GetItemRectMax().y));
        }
        FUCK::Separator();

        // ---- empty library --------------------------------------------------
        //
        // No early return, and the tab split did not change why: on a first
        // visit the preset browser is the whole point of the page (a played
        // rig has a hundred presets before it has one saved look), so this
        // tab says its library is empty and the other tab is a click away.
        const bool libraryEmpty = g_rows.empty() && !g_capturePending;
        if (libraryEmpty) {
            OS::ui::TextDisabledWrapped("$FR_Looks_Empty1"_T);
            OS::ui::TextDisabledWrapped("$FR_Looks_Empty2"_T);
            FUCK::Spacing();
        }

        // ---- the two panes --------------------------------------------------
        //
        // The whole tab now. The old 52% share existed because the preset
        // browser sat underneath; it has its own tab, so nothing is competing
        // for this height any more.
        const float paneH =
            std::max(OS::ui::FontSize() * 8.0f,
                     FUCK::GetContentRegionAvail().y);
        const float listW =
            std::max(OS::ui::FontSize() * 11.0f,
                     FUCK::GetContentRegionAvail().x * 0.36f);

        if (!libraryEmpty) {
        // ⚠ THE CHILD'S OWN BOX, TAKEN BEFORE IT BEGINS. Inside a scrolled
        // child the cursor is in that child's space, and a ring drawn from
        // those coordinates lands somewhere else on screen. The first two
        // cards point at the library as a whole rather than at one row, so
        // the frame the list occupies is the honest target.
        const ImVec2 listTop = FUCK::GetCursorScreenPos();
        FUCK::BeginChild("looks_list", ImVec2(listW, paneH), true);
        {
            // ⚠ THE FILTER ONLY APPEARS WHEN IT COULD HELP. A search box over
            // three looks is furniture; over a library that has grown past a
            // screenful it is the only way back to one you named months ago.
            const bool worthFiltering =
                g_rows.size() > kFilterFromRows || !g_lookFilter.empty();
            if (worthFiltering) {
                DrawFilterRow("##looks_filter", g_lookFilter,
                              "$FR_Looks_Search");
                FUCK::Separator();
            }
            std::size_t shown = 0;
            for (const auto& row : g_rows) {
                if (!MatchesFilter(row.name, g_lookFilter)) {
                    continue;
                }
                ++shown;
                FUCK::PushID(row.name.c_str());
                const bool selected = row.name == g_selected;
                if (FUCK::Selectable(row.name.c_str(), selected)) {
                    SelectRow(row.name);
                    EditorStyle::PlayUISound("UIMenuFocus");
                    // Picking a look only fills the part column in, so this is
                    // the one ask on the page that moves nothing.
                    Tutorial::NotifyAction(Tutorial::Action::kSelectedLook);
                }
                if (FUCK::IsItemClicked(ImGuiMouseButton_Right)) {
                    SelectRow(row.name);
                    g_menuFor     = row.name;
                    g_requestMenu = true;
                }
                if (row.droppedBlocks > 0 && FUCK::IsItemHovered()) {
                    OS::ui::SetTooltipF("$FR_Looks_DroppedTip"_T,
                                        static_cast<int>(row.droppedBlocks));
                }
                FUCK::PopID();
            }
            // A filter that hides everything must say so; an empty list under
            // a full library reads as a lost library.
            if (shown == 0 && !g_lookFilter.empty()) {
                FUCK::TextDisabled("%s", "$FR_Looks_NoMatch"_T);
            }
            if (g_capturePending) {
                FUCK::TextDisabled("%s", "$FR_Looks_Saving"_T);
            }
        }
        FUCK::EndChild();
        Tutorial::PublishAnchor(Tutorial::Anchor::kLooksLibrary, listTop,
                                ImVec2(listTop.x + listW, listTop.y + paneH));

        FUCK::SameLine();

        FUCK::BeginChild("looks_detail", ImVec2(0.0f, paneH), true);
        {
            const auto* row = g_selected.empty() ? nullptr : FindRow(g_selected);
            if (!row) {
                EditorNotice::DrawCentred({ "$FR_Looks_PickPrompt"_T });
            } else {
                // ⚠⚠ THE NAME IS AN EDITABLE FIELD, NOT A LABEL AND A BUTTON
                // (user 2026-08-26: "I do not like the UX of having to click a
                // rename button"). The Outfits page has renamed in place since
                // it shipped and this page did not, so the same gesture meant
                // two different things depending on which page you were on.
                //
                // ⚠ RESEEDED WHENEVER THE SELECTION MOVES, and compared against
                // the row rather than latched on a selection event, so a rename
                // that lands, a refresh that rebuilds the rows and a look that
                // disappears under the pane all recover on the next frame
                // without any of them having to remember to say so.
                if (g_nameEditFor != row->name) {
                    g_nameEdit    = row->name;
                    g_nameEditFor = row->name;
                    g_nameNote.clear();
                }
                FUCK::AlignTextToFramePadding();
                FUCK::TextUnformatted("$FR_Name"_T);
                FUCK::SameLine();
                if (g_focusName) {
                    g_focusName = false;
                    FUCK::SetKeyboardFocusHere();
                }
                FUCK::SetNextItemWidth(
                    std::max(OS::ui::FontSize() * 8.0f,
                             FUCK::GetContentRegionAvail().x));
                FUCK::InputText("##look_rename", &g_nameEdit);
                // ⚠ THE COMMIT IS HERE, ON THE EDGE THE FIELD LOSES FOCUS AFTER
                // AN EDIT. Enter and clicking away both reach it; typing does
                // not, which is the whole point (see g_nameEdit).
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    const std::string from = row->name;
                    const bool        blank =
                        g_nameEdit.find_first_not_of(" \t") == std::string::npos;
                    auto& store = ProfileStore::GetSingleton();
                    if (blank || g_nameEdit == from) {
                        // ⚠ A BLANK FIELD IS NOT A RENAME TO NOTHING. Put the
                        // name back the way the Outfits field does, silently:
                        // the player emptied a box, they did not ask for a look
                        // with no name, and there is no such thing to make.
                        g_nameEdit = from;
                        g_nameNote.clear();
                    } else if (!store.NameAvailable(g_nameEdit, from)) {
                        g_nameNote = "$FR_Looks_NameTaken"_T;
                        g_nameEdit = from;
                    } else {
                        std::string error;
                        if (store.Rename(from, g_nameEdit, error)) {
                            const std::string to = g_nameEdit;
                            Refresh();
                            SelectRow(to);
                            // The row this pane is drawing has just been
                            // rebuilt under it, so hand the buffer to the new
                            // name rather than letting the reseed above fight
                            // a stale one for a frame.
                            g_nameEdit    = to;
                            g_nameEditFor = to;
                            g_nameNote.clear();
                            EditorStyle::PlayUISound("UIMenuOK");
                        } else {
                            spdlog::warn("ProfilesUI: rename of '{}' refused: {}",
                                         from, error);
                            g_nameEdit = from;
                        }
                    }
                }
                if (!g_nameNote.empty()) {
                    FUCK::TextDisabled("%s", g_nameNote.c_str());
                }
                if (row->hasCharacter) {
                    FUCK::TextDisabled("$FR_Looks_CharacterNote"_T,
                                       row->raceLabel.c_str(),
                                       row->female ? "$FR_Looks_Female"_T
                                                   : "$FR_Looks_Male"_T);
                }

                // ---- what this look needs installed ----------------------
                //
                // ⚠⚠ EXACT, UNLIKE THE PRESET PANE'S. Fitting Room owns the
                // profile format and every StyleRefKey in it carries modName,
                // so this list is a walk over what the codec already decoded
                // and it cannot go stale. The one part that CAN is the face,
                // which lives in somebody else's file, and the line below says
                // so rather than letting a short list imply a small need.
                if (!row->needs.empty()) {
                    OS::ui::TextDisabledWrapped(
                        OS::ui::FormatF("$FR_Looks_PresetNeeds"_T,
                                        JoinCapped(row->needs).c_str())
                            .c_str());
                }
                if (!row->missing.empty()) {
                    OS::ui::TextDisabledWrapped(
                        OS::ui::FormatF("$FR_Looks_PresetMissingMods"_T,
                                        JoinCapped(row->missing).c_str())
                            .c_str());
                }
                if (row->faceNeedsUnknown) {
                    OS::ui::TextDisabledWrapped("$FR_Looks_LookFaceUnknown"_T);
                }
                FUCK::Separator();

                // The face row's gate is the skee handshake, the same answer
                // the capture side reads: with RaceMenu absent the CharGen
                // class is not registered and a face apply could only refuse.
                // The box is forced off too, not just greyed: a hidden true
                // would still reach ProfileApply, and a control that shows
                // unchecked while its value is checked is two answers.
                const bool faceUsable = OverlayApi::Available();
                if (!faceUsable) {
                    g_boxes.face = false;
                }

                // ---- what this apply will move -------------------------
                //
                // ⚠ TEN CHECKBOXES IN ONE COLUMN IS TEN DECISIONS, and the
                // page asked for them every single time. The count says what
                // the column adds up to and the two buttons answer the common
                // cases in one press.
                //
                // ⚠⚠ AND THE ACTIONS ARE PINNED BELOW THEM. The r29 version
                // stacked ten rows under four headings and pushed Apply off
                // the bottom of the pane, so taking a look meant scrolling to
                // reach the button that takes it (field verdict: awkward).
                // Whatever the boxes cost, the footer is reserved before they
                // are drawn, so the buttons cannot be pushed anywhere and a
                // list too long for the pane scrolls inside its own child.
                const auto usable   = ProfilePlan::EveryUsable(row->blocks,
                                                               faceUsable);
                const int  takeN    = ProfilePlan::Count(
                    ProfilePlan::And(g_boxes, usable));
                const int  usableN  = ProfilePlan::Count(usable);
                FUCK::AlignTextToFramePadding();
                FUCK::TextDisabled(
                    "%s", OS::ui::FormatF("$FR_Looks_PartsSummary"_T, takeN,
                                          usableN)
                              .c_str());
                FUCK::SameLine();
                const auto all = ChamferPanel::Button("$FR_Looks_PartsAll"_T,
                                                      0.0f, takeN == usableN);
                if (all.clicked) {
                    g_boxes = usable;
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                if (all.hovered) {
                    FUCK::SetTooltip("$FR_Looks_PartsAllTip"_T);
                }
                FUCK::SameLine();
                const auto none = ChamferPanel::Button("$FR_Looks_PartsNone"_T,
                                                       0.0f, takeN == 0);
                if (none.clicked) {
                    g_boxes = ProfilePlan::Participation{};
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                if (none.hovered) {
                    FUCK::SetTooltip("$FR_Looks_PartsNoneTip"_T);
                }
                FUCK::Spacing();

                // ⛔ NO HINT LINE HERE, AND THE KEY WENT WITH IT. A dead-key
                // audit found $FR_Looks_BlocksHint unreferenced and drew it above
                // the boxes; the field looked at it and said the column explains
                // itself (user 2026-08-26). It is deleted from the translation
                // file rather than left sitting there unreferenced, so the next
                // audit cannot rediscover it and draw it a second time.

                // ⚠⚠ THE FOOTER IS RESERVED BEFORE THE BOXES ARE DRAWN.
                // Apply, Rename and Delete used to be the last things in the
                // pane, so anything above them that grew pushed them out of
                // sight and taking a look meant scrolling to find the button
                // that takes it. They now sit outside the scrolling region
                // entirely: the boxes get what is left after the footer, and
                // the footer is always where the player last saw it.
                const float footerH = FUCK::GetFrameHeight() +
                                      OS::ui::ItemSpacing().y * 3.0f;
                const float boxesH =
                    std::max(OS::ui::FontSize() * 3.0f,
                             FUCK::GetContentRegionAvail().y - footerH);
                // Same rule as the library above: the child's frame, sampled
                // before it begins, because the boxes themselves live in the
                // child's scrolled space.
                const ImVec2 boxesTop = FUCK::GetCursorScreenPos();
                const float  boxesW   = FUCK::GetContentRegionAvail().x;
                FUCK::BeginChild("looks_blocks", ImVec2(0.0f, boxesH), false);

                // ⚠ ONE COLUMN, IN APPLY ORDER, AND THE USER ASKED FOR THAT BY
                // NAME. Two columns of five put two checkboxes on every line,
                // and the field read that as the same toggle drawn twice:
                // "the toggles appearing twice together in a row is confusing,
                // just make it a list." A single column reads top to bottom in
                // the order the apply runs, and the child scrolls if the pane
                // is short. The footer is still reserved before this, so the
                // buttons cannot be pushed out of sight.
                //
                // ⚠⚠ THE HEADERS BELOW ARE PRESENTATION AND NOTHING ELSE.
                // They group ten rows that were a flat run (user 2026-08-25);
                // they do not change which blocks exist, what any of them
                // captures, or the order they apply in. Every one of the ten was
                // field proven on its own and several cost a session each, so a
                // grouping pass that moved one would be paying for that twice.
                // The line the rail already draws is the line these follow:
                // Shape and Overlays "are also the pair that edit the CHARACTER
                // rather than what the character is wearing".
                //
                // ⚠ DefaultOpen ON ALL THREE, so the pane shows the same ten
                // boxes it showed before. A header the player has not touched
                // hides nothing, so the grouping costs the player no visible
                // box.
                FUCK::BeginGroup();
                // ⚠ CHARACTER IS ABOVE THE GROUPS RATHER THAN IN ONE. It
                // carries the sex flag rather than an appearance, it is what
                // everything below lands ON, and the apply runs it first. A
                // group of one would read like a mistake.
                //
                // Checked, a look captured on another race or sex switches this
                // character to match; unchecked, the old advisory warn and
                // nothing moves.
                BlockCheckbox("$FR_Looks_BlockCharacter"_T, g_boxes.character,
                              row->blocks.character, true,
                              "$FR_Looks_BlockAbsentTip"_T, "");
                FUCK::Spacing();

                if (OS::ui::FramedHeader("$FR_Looks_GroupHeadFace"_T,
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
                    // A just-saved look's face is absent-but-coming while the
                    // editor pause holds the VM; the tooltip says so instead of
                    // leaving a dimmed box to explain itself.
                    const bool facePending =
                        !row->blocks.face &&
                        ProfileCapture::FacePending(row->name);
                    BlockCheckbox("$FR_Looks_BlockFace"_T, g_boxes.face,
                                  row->blocks.face, faceUsable,
                                  facePending ? "$FR_Looks_FaceSavingTip"_T
                                              : "$FR_Looks_BlockAbsentTip"_T,
                                  "$FR_Looks_FaceOffTip"_T, facePending);
                    BlockCheckbox("$FR_Looks_BlockSkin"_T, g_boxes.skin,
                                  row->blocks.skin, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                    BlockCheckbox("$FR_Looks_BlockMakeup"_T, g_boxes.makeup,
                                  row->blocks.makeup, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                    BlockCheckbox("$FR_Looks_BlockOverlays"_T, g_boxes.overlays,
                                  row->blocks.overlays, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                }
                if (OS::ui::FramedHeader("$FR_Looks_GroupBody"_T,
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
                    BlockCheckbox("$FR_Looks_BlockWeight"_T, g_boxes.weight,
                                  row->blocks.weight, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                    BlockCheckbox("$FR_Looks_BlockBody"_T, g_boxes.body,
                                  row->blocks.body, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                    BlockCheckbox("$FR_Looks_BlockShape"_T, g_boxes.shape,
                                  row->blocks.shape, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                }
                if (OS::ui::FramedHeader("$FR_Looks_GroupWorn"_T,
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
                    BlockCheckbox("$FR_Looks_BlockOutfit"_T, g_boxes.outfit,
                                  row->blocks.outfit, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                    BlockCheckbox("$FR_Looks_BlockDyes"_T, g_boxes.dyes,
                                  row->blocks.dyes, true,
                                  "$FR_Looks_BlockAbsentTip"_T, "");
                }
                FUCK::EndGroup();
                FUCK::EndChild();
                Tutorial::PublishAnchor(
                    Tutorial::Anchor::kLooksParts, boxesTop,
                    ImVec2(boxesTop.x + boxesW, boxesTop.y + boxesH));
                FUCK::Separator();

                const bool anyBox = g_boxes.character || g_boxes.face ||
                                    g_boxes.outfit ||
                                    g_boxes.dyes || g_boxes.makeup ||
                                    g_boxes.weight || g_boxes.body ||
                                    g_boxes.shape || g_boxes.skin ||
                                    g_boxes.overlays;
                const auto apply =
                    ChamferPanel::Button("$FR_Apply"_T, 0.0f, !anyBox || !player);
                // ⚠⚠ ON THE APPLY ROW AND NOT ABOVE IT. A row of its own pushed
                // Apply below the fold and the page had to be scrolled to reach
                // the one button it exists for (user 2026-08-27). It belongs
                // beside the boxes it modifies either way, and this is the only
                // place that is true of which costs no height.
                if (apply.clicked && anyBox && player) {
                    // Re-read at the press: the row holds no payload, and the
                    // file is the truth (it may have been rewritten since the
                    // list was built).
                    auto& store = ProfileStore::GetSingleton();
                    store.Load();
                    if (const auto entry = store.Find(row->name)) {
                        ProfileApply::Apply(player, entry->profile, g_boxes);
                        EditorStyle::PlayUISound("UIMenuOK");
                        // ⚠⚠ THE EDITOR USED TO CLOSE WITH THE APPLY AND THE
                        // USER ASKED FOR THE OPPOSITE. The reason it closed is
                        // real: the face step rebuilds the head and the outfit
                        // step activates a DIFFERENT library entry under the
                        // editor's staged copy, which is the one state the
                        // staging machinery has no seam for. So the apply owes
                        // the editor a re-stage instead of an exit, and it runs
                        // one at its settle (ProfileApply's RunSteps tail calls
                        // EditorWindow::RequestRestage). Nothing to do here.
                    } else {
                        spdlog::warn("ProfilesUI: '{}' vanished from the "
                                     "store before Apply.", row->name);
                        Refresh();
                    }
                }
                if (apply.hovered) {
                    FUCK::SetTooltip("$FR_Looks_ApplyTip"_T);
                }

                // ⚠ NO Rename BUTTON HERE ANY MORE. The name field above IS the
                // rename now, so a button that opened a modal to do the same
                // thing was a second way to reach one answer and the slower of
                // the two.
                // ⚠ AND THE ROW IS A PREFERENCE, NOT A PROMISE. Three controls
                // fit on one line at this font and need not at the next one up
                // or under a longer translation, and letting the overflow hang
                // off the right is what made the clip invisible to the code
                // that caused it. Measured against what is actually left on the
                // line, the way the outfit toolbar measures its own group, so
                // Delete drops to a row of its own rather than off the panel.
                {
                    const float delW = FUCK::CalcTextSize("$FR_Delete"_T).x +
                                       OS::ui::FramePadding().x * 2.0f;
                    const ImVec2 nextLine = FUCK::GetCursorPos();
                    FUCK::SameLine();
                    if (FUCK::GetCursorPos().x + delW + OS::ui::EdgeClearance() >
                        FUCK::GetWindowSize().x - OS::ui::WindowPadding().x) {
                        FUCK::SetCursorPos(nextLine);
                    }
                }
                const auto del = ChamferPanel::Button("$FR_Delete"_T);
                if (del.clicked) {
                    g_actionFor     = row->name;
                    g_requestDelete = true;
                }
                if (del.hovered && row->faceCaptured) {
                    FUCK::SetTooltip("$FR_Looks_DeleteFaceTip"_T);
                }

                // ⚠⚠ THE SWITCH SITS AT THE RIGHT EDGE, AND IT USED TO SIT
                // BETWEEN THE TWO BUTTONS. "It's kind of awkward in between
                // apply and delete" (user 2026-08-27), and it was: Apply and
                // Delete are one pair of actions on one row and a preference
                // wedged between them reads as a third button. Both buttons
                // keep the left, the preference takes the right, and the gap
                // between them says which is which without a separator.
                //
                // ⚠ MEASURED AND CLAMPED, NOT PLACED. FUCK::Checkbox's own
                // alignFar would push only its BOX to the edge and leave the
                // label back at the cursor, which is the layout this replaces.
                // The width here is the compact form's own parts (label, one
                // gap, a box a frame high), and the clamp is what stops a long
                // translation or a narrow panel putting it back on top of
                // Delete: it gives up the anchor before it gives up the row.
                {
                    auto& s       = Settings::GetSingleton();
                    bool  replace = s.replaceOnLookApply;
                    const float boxW  = FUCK::GetFrameHeight();
                    const float wantW = FUCK::CalcTextSize("$FR_Looks_Replace"_T).x +
                                        OS::ui::ItemSpacing().x + boxW;
                    const ImVec2 nextLine = FUCK::GetCursorPos();
                    FUCK::SameLine();
                    const float rowRight = FUCK::GetWindowSize().x -
                                           OS::ui::WindowPadding().x - OS::ui::EdgeClearance();
                    const float here = FUCK::GetCursorPos().x;
                    if (here + wantW > rowRight) {
                        FUCK::SetCursorPos(nextLine);  // no room beside Delete
                    } else {
                        FUCK::SetCursorPosX(rowRight - wantW);
                    }
                    if (FUCK::Checkbox("$FR_Looks_Replace"_T, &replace, false, true)) {
                        s.replaceOnLookApply = replace;
                        s.Save();
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Looks_ReplaceTip"_T);
                    }
                }
            }
        }
        FUCK::EndChild();

        if (g_rejected > 0) {
            FUCK::TextDisabled("$FR_Looks_Rejected"_T,
                               static_cast<int>(g_rejected));
        }
        }  // !libraryEmpty
            FUCK::PopID();
        }  // looksTab

        // ---- the RaceMenu preset browser (spec W3) --------------------------
        //
        // Two labelled groups, two folders, two natives. Loading applies the
        // preset's whole face - sculpt included, skee applies it ungated at
        // flags 0 (PresetInterface.cpp, read 2026-08-22) - through the
        // ordinary ProfileApply, and closes the editor the way Apply does.
        const bool presetsTab = g_looksTab == kTabPresets;
        if (presetsTab) {
            FUCK::PushID("looks_presets");
        const bool faceUsable = OverlayApi::Available();
        if (!faceUsable) {
            OS::ui::TextDisabledWrapped("$FR_Looks_FaceOffTip"_T);
        } else {
            // ⚠⚠ THE EXPORT ROW LEADS THE TAB, BECAUSE THE SAVE ROW LEADS THE
            // OTHER ONE. Both tabs on this page do the same thing with a typed
            // name and a button, and this one sat under the panes while the
            // saved-looks tab's sat above them, so the same gesture was in two
            // places depending on which tab you were on (user 2026-08-26). The
            // panes take what is left rather than a reserve measured off the
            // bottom, which is what the 2.6 font sizes here used to be for.
            FUCK::SetNextItemWidth(std::max(
                OS::ui::FontSize() * 5.0f,
                FUCK::GetContentRegionAvail().x -
                    FUCK::CalcTextSize("$FR_Looks_ExportHead"_T).x -
                    OS::ui::FramePadding().x * 4.0f - OS::ui::ItemSpacing().x));
            FUCK::InputText("##export_head_name", &g_exportName);
            FUCK::SameLine();
            const auto normalized =
                PresetBrowse::NormalizeExportName(g_exportName);
            const bool exporting = ProfileCapture::ExportPending();
            // ⚠ THE EXPORT SETTLES SECONDS AFTER THE PRESS, so its row cannot
            // exist when the button comes back up. The library is otherwise
            // rescanned on page entry alone, which left a fresh export
            // invisible until the user walked off the page and back (field
            // 2026-08-23: "I think I exported it, but I don't see it in the
            // list"). One rescan on the falling edge is the whole fix, and it
            // costs a folder read once per export rather than once per frame.
            if (g_wasExporting && !exporting) {
                RescanPresets();
            }
            g_wasExporting = exporting;
            const auto exp       = ChamferPanel::Button(
                "$FR_Looks_ExportHead"_T, 0.0f,
                normalized.empty() || exporting || !player);
            if (exp.clicked && !normalized.empty() && !exporting && player) {
                ProfileCapture::ExportHeadTrio(normalized);
                EditorStyle::PlayUISound("UIMenuOK");
                g_exportName.clear();
            }
            if (exp.hovered) {
                FUCK::SetTooltip(exporting          ? "$FR_Looks_ExportPendingTip"_T
                                 : normalized.empty() ? "$FR_Looks_ExportNameHint"_T
                                                      : "$FR_Looks_ExportHeadTip"_T);
            }
            FUCK::Separator();

            const float presetListH =
                std::max(OS::ui::FontSize() * 5.0f,
                         FUCK::GetContentRegionAvail().y);
            const float presetListW =
                std::max(OS::ui::FontSize() * 10.0f,
                         FUCK::GetContentRegionAvail().x * 0.56f);
            FUCK::BeginChild("preset_list", ImVec2(presetListW, presetListH),
                             true);
            {
                // ⚠ THIS LIST IS THE ONE THAT REALLY NEEDED A SEARCH. A
                // RaceMenu preset folder is a hundred stems deep on a rig
                // that has been played, and the only structure it had was
                // the two folder headings.
                const bool worthFiltering =
                    g_presetRows.size() + g_exportedRows.size() >
                        kFilterFromRows ||
                    !g_presetFilter.empty();
                if (worthFiltering) {
                    DrawFilterRow("##preset_filter", g_presetFilter,
                                  "$FR_Looks_PresetSearch");
                    FUCK::Separator();
                }
                std::size_t   shownTotal = 0;
                const auto drawGroup =
                    [&](const char* a_label,
                        const std::vector<PresetBrowse::Entry>& a_rows,
                        ProfileCodec::FaceFolder a_folder) {
                        std::size_t shown = 0;
                        for (const auto& row : a_rows) {
                            if (MatchesFilter(row.stem, g_presetFilter)) {
                                ++shown;
                            }
                        }
                        shownTotal += shown;
                        // The heading carries the count, so a filtered group
                        // says how much of itself is left rather than looking
                        // like a folder that lost its contents.
                        //
                        // ⚠⚠ AN ACCORDION RATHER THAN A LINE OF TEXT, so this page
                        // answers the same right-click every other list in the
                        // editor does (user 2026-08-26). It also earns its keep
                        // on a played rig: a RaceMenu preset folder runs to a
                        // hundred and sixty stems, and reaching the exported
                        // group meant scrolling past all of them.
                        //
                        // ⚠ THE ID IS FIXED AND THE COUNT IS NOT PART OF IT. The
                        // label changes as the filter narrows, and a header
                        // whose id follows its text is a different header every
                        // keystroke, which loses whatever the player collapsed.
                        const std::string heading =
                            OS::ui::FormatF("%s (%d)", a_label,
                                            static_cast<int>(shown)) +
                            "###looks_preset_grp_" +
                            std::to_string(static_cast<int>(a_folder));
                        if (!OS::ui::FramedHeader(heading.c_str(),
                                                  ImGuiTreeNodeFlags_DefaultOpen)) {
                            return;
                        }
                        // ⚠⚠ THE ID IS THE FOLDER AND THE POSITION, NEVER THE
                        // NAME. A preset stem is not unique. The same file name
                        // occurs in the Presets folder and in Exported, and this
                        // lambda draws both groups into one id scope, so a stem
                        // that exists in both gave two VISIBLE rows the same id
                        // and ImGui put its "2 visible items with conflicting ID"
                        // panel over the page (field 2026-08-24, a library of 60
                        // presets beside our own FR_ exports).
                        //
                        // PushID hashes whatever it is handed, so a name is only
                        // ever a safe id when the name is the key. Here it is not:
                        // the key is the PAIR, stem and folder, which is exactly
                        // what `selected` below already compares. Position is used
                        // rather than that pair because it cannot collide at all,
                        // and nothing here needs an id that survives a frame:
                        // selection lives outside the stack in g_presetSelected.
                        FUCK::PushID(static_cast<int>(a_folder));
                        int rowId = 0;
                        for (const auto& row : a_rows) {
                            if (!MatchesFilter(row.stem, g_presetFilter)) {
                                continue;
                            }
                            FUCK::PushID(rowId++);
                            const bool selected =
                                row.stem == g_presetSelected &&
                                a_folder == g_presetSelectedFolder;
                            if (FUCK::Selectable(row.stem.c_str(), selected)) {
                                g_presetSelected       = row.stem;
                                g_presetSelectedFolder = a_folder;
                                RefreshFacts();
                                EditorStyle::PlayUISound("UIMenuFocus");
                            }
                            FUCK::PopID();
                        }
                        FUCK::PopID();
                        if (a_rows.empty()) {
                            FUCK::TextDisabled("%s", "$FR_Looks_PresetsEmpty"_T);
                        }
                    };
                drawGroup("$FR_Looks_PresetsGroupPresets"_T, g_presetRows,
                          ProfileCodec::FaceFolder::kPresets);
                FUCK::Spacing();
                drawGroup("$FR_Looks_PresetsGroupExported"_T, g_exportedRows,
                          ProfileCodec::FaceFolder::kExported);
                if (shownTotal == 0 && !g_presetFilter.empty()) {
                    FUCK::TextDisabled("%s", "$FR_Looks_NoMatch"_T);
                }
            }
            FUCK::EndChild();
            FUCK::SameLine();

            // The side pane: what the selected preset can do, and the two
            // ways to take it.
            //
            // ⚠⚠ NoScrollbar IS SAFE HERE ONLY BECAUSE THE FOOTER IS RESERVED
            // BELOW. This pane used to be natural-height content in a fixed
            // height box: a tall enough hint, a long enough fact list or a big
            // enough UI scale pushed the two buttons past the bottom and the
            // pane grew a scrollbar for a pane nobody wants to scroll (user
            // 2026-08-25). Reserving the footer first makes the content exactly
            // fill the box whatever is in it, so suppressing the bar hides
            // nothing; the facts child inside owns any overflow and shows its
            // own bar when the facts genuinely do not fit. ⛔ Do not keep the
            // flag if the footer reservation ever goes: it would clip the
            // buttons instead of scrolling to them.
            FUCK::BeginChild("preset_side", ImVec2(0.0f, presetListH), true,
                             ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse);
            {
                const bool havePick = !g_presetSelected.empty();

                // ⚠⚠ THE FOOTER IS RESERVED BEFORE THE FACTS, WHICH IS THE
                // SAVED-LOOKS PANE'S RULE AND NOW THE REASON BOTH PANES PUT
                // THEIR BUTTONS IN THE SAME PLACE (user 2026-08-25: the two
                // right panes should read as one control). Apply, Rename and
                // Delete sit at the bottom of the other pane on one row; Load
                // and Save as look sit here on one row, at the same height,
                // whatever is above them.
                const float footerH = FUCK::GetFrameHeight() +
                                      OS::ui::ItemSpacing().y * 3.0f;
                const float factsH =
                    std::max(OS::ui::FontSize(),
                             FUCK::GetContentRegionAvail().y - footerH);
                FUCK::BeginChild("preset_facts", ImVec2(0.0f, factsH), false);
                // ⚠ THE PANE USED TO BE TWO BUTTONS AND NOTHING ELSE (user
                // 2026-08-24: "the right pane of racemenu presets ... right now
                // it's very anemic"). With a hundred and sixty stems in the
                // list, the question the pane has to answer is "which one is
                // this", and the only honest answers are file facts: a jslot is
                // never opened here. The date is the one that does the work,
                // because the stems are named by whoever made them and the
                // recent one is the one you usually want.
                if (!havePick) {
                    EditorNotice::DrawCentred({ "$FR_Looks_PresetPickPrompt"_T });
                } else {
                    FUCK::TextUnformatted(g_presetSelected.c_str());
                    FUCK::TextDisabled(
                        "%s", g_facts.exported
                                  ? "$FR_Looks_PresetsGroupExported"_T
                                  : "$FR_Looks_PresetsGroupPresets"_T);
                    FUCK::Spacing();
                    if (g_facts.known) {
                        if (!g_facts.saved.empty()) {
                            FUCK::TextDisabled(
                                "%s", OS::ui::FormatF("$FR_Looks_PresetSaved"_T,
                                                      g_facts.saved.c_str())
                                          .c_str());
                        }
                        if (g_facts.bytes > 0) {
                            FUCK::TextDisabled(
                                "%s",
                                OS::ui::FormatF(
                                    "$FR_Looks_PresetSize"_T,
                                    static_cast<double>(g_facts.bytes) / 1024.0)
                                    .c_str());
                        }
                        if (g_facts.binaryOnly) {
                            FUCK::TextDisabled("%s",
                                               "$FR_Looks_PresetBinary"_T);
                        }
                        // ⚠ ONLY WHEN SOMETHING IS ABSENT. An export is three
                        // files and the usual case is that all three are
                        // there, so a pair of "present" lines on every preset
                        // would be noise with one useful state hidden in it.
                        // Silence means whole; a line means look.
                        if (g_facts.exported &&
                            (!g_facts.meshPresent || !g_facts.tintPresent)) {
                            FUCK::Spacing();
                            if (!g_facts.meshPresent) {
                                FUCK::TextDisabled("%s",
                                                   "$FR_Looks_PresetNoMesh"_T);
                            }
                            if (!g_facts.tintPresent) {
                                FUCK::TextDisabled("%s",
                                                   "$FR_Looks_PresetNoTint"_T);
                            }
                        }

                        // ---- what it needs installed ------------------
                        //
                        // ⚠ WRAPPED, NOT CLIPPED. A preset naming six plugins
                        // in a narrow column is the ordinary case, and
                        // TextDisabled does not wrap, so the tail of the list
                        // would simply not be there.
                        FUCK::Spacing();
                        if (!g_facts.needsRead) {
                            OS::ui::TextDisabledWrapped(
                                "$FR_Looks_PresetNeedsUnknown"_T);
                        } else {
                            OS::ui::TextDisabledWrapped(
                                OS::ui::FormatF("$FR_Looks_PresetNeeds"_T,
                                                JoinCapped(g_facts.needs).c_str())
                                    .c_str());
                            // ⚠ THE ABSENT ONES GET THEIR OWN LINE, because the
                            // needs line answers "what is this" and this one
                            // answers "will it work", and a player scanning
                            // for the second should not have to read the
                            // first and compare it against their load order.
                            if (!g_facts.missing.empty()) {
                                OS::ui::TextDisabledWrapped(
                                    OS::ui::FormatF(
                                        "$FR_Looks_PresetMissingMods"_T,
                                        JoinCapped(g_facts.missing).c_str())
                                        .c_str());
                            }
                        }
                    } else {
                        FUCK::TextDisabled("%s", "$FR_Looks_PresetUnreadable"_T);
                    }
                }
                FUCK::EndChild();
                FUCK::Separator();

                const auto load = ChamferPanel::Button(
                    "$FR_Looks_PresetLoad"_T, 0.0f, !havePick || !player);
                if (load.clicked && havePick && player) {
                    ProfilePlan::Participation boxes;
                    boxes.face = true;
                    ProfileApply::Apply(player,
                                        PresetProfileFor(g_presetSelected,
                                                         g_presetSelectedFolder),
                                        boxes);
                    EditorStyle::PlayUISound("UIMenuOK");
                }
                if (load.hovered) {
                    FUCK::SetTooltip(havePick ? "$FR_Looks_PresetLoadTip"_T
                                              : "$FR_Looks_PresetPickTip"_T);
                }
                const bool nameTaken =
                    havePick && !ProfileStore::GetSingleton().NameAvailable(
                                    g_presetSelected);
                FUCK::SameLine();
                const auto keep = ChamferPanel::Button(
                    "$FR_Looks_PresetSaveAsLook"_T, 0.0f,
                    !havePick || nameTaken || !player);
                if (keep.clicked && havePick && !nameTaken && player) {
                    // The full character as they stand, wrapped around the
                    // REFERENCED preset face: gear, dyes, body, colours and
                    // the character block all captured, so applying the look
                    // later restores an outfit and not just a face. The
                    // face-only version surprised the field.
                    auto profile =
                        ProfileCapture::SnapshotBlocks(player,
                                                       g_presetSelected);
                    profile.face = ProfileCodec::FaceBlock{
                        g_presetSelected, ProfileCodec::FaceSource::kReferenced,
                        0, g_presetSelectedFolder
                    };
                    std::string error;
                    if (ProfileStore::GetSingleton().Save(profile, error)) {
                        Refresh();
                        SelectRow(profile.name);
                        EditorStyle::PlayUISound("UIMenuOK");
                    } else {
                        spdlog::warn(
                            "ProfilesUI: preset look '{}' not saved: {}",
                            profile.name, error);
                    }
                }
                if (keep.hovered) {
                    FUCK::SetTooltip(nameTaken
                                         ? "$FR_Looks_PresetNameTakenTip"_T
                                     : havePick
                                         ? "$FR_Looks_PresetSaveAsLookTip"_T
                                         : "$FR_Looks_PresetPickTip"_T);
                }
            }
            FUCK::EndChild();
        }
            FUCK::PopID();
        }  // presetsTab

        // ---- popups, at page scope ------------------------------------------
        //
        // ⚠ OUTSIDE THE TAB BAR, WHICH PUSHES AN ID OF ITS OWN. Opened HERE
        // and begun HERE, after every child AND the bar above have ended, so
        // OpenPopup and BeginPopup hash against the same id stack (OS-29).
        if (g_requestMenu) {
            g_requestMenu = false;
            OS::ui::OpenContextMenu("look_row");
        }
        if (OS::ui::BeginContextMenu("look_row")) {
            if (OS::ui::ContextMenuItem("$FR_Apply"_T)) {
                // The same press as the detail pane's Apply, boxes included:
                // the menu applies what the pane shows, not a second recipe.
                SelectRow(g_menuFor);
                auto& store = ProfileStore::GetSingleton();
                store.Load();
                if (const auto entry = store.Find(g_menuFor); entry && player) {
                    ProfileApply::Apply(player, entry->profile, g_boxes);
                    EditorStyle::PlayUISound("UIMenuOK");
                }
            }
            // ⚠ SELECTS AND FOCUSES RATHER THAN OPENING ANYTHING. The menu can
            // be raised on a row that is not the selected one, so it has to
            // move the pane before the field it is aiming at is the right
            // field.
            if (OS::ui::ContextMenuItem("$FR_Looks_Rename"_T)) {
                SelectRow(g_menuFor);
                g_focusName = true;
            }
            if (OS::ui::ContextMenuItem("$FR_Delete"_T)) {
                g_actionFor     = g_menuFor;
                g_requestDelete = true;
            }
            OS::ui::EndContextMenu();
        }

        if (g_requestDelete) {
            g_requestDelete = false;
            OS::ui::OpenModal("delete_look");
        }
        if (OS::ui::BeginModal("delete_look")) {
            const auto* victim = FindRow(g_actionFor);
            FUCK::Text(victim && victim->faceCaptured
                           ? "$FR_Looks_DeleteConfirmFace"_T
                           : "$FR_Looks_DeleteConfirm"_T,
                       g_actionFor.c_str());
            FUCK::Spacing();
            OS::ui::CentreTwoButtons("$FR_Delete"_T, "$FR_Cancel"_T);
            if (ChamferPanel::Button("$FR_Delete"_T).clicked) {
                std::string error;
                std::string jslot;
                if (ProfileStore::GetSingleton().Delete(g_actionFor, error,
                                                        &jslot)) {
                    if (!jslot.empty()) {
                        DeleteCapturedJslot(jslot);
                    }
                    EditorStyle::PlayUISound("UIMenuCancel");
                } else {
                    spdlog::warn("ProfilesUI: delete of '{}' refused: {}",
                                 g_actionFor, error);
                }
                Refresh();
                FUCK::CloseCurrentPopup();
            }
            FUCK::SameLine();
            if (ChamferPanel::Button("$FR_Cancel"_T).clicked) {
                FUCK::CloseCurrentPopup();
            }
            FUCK::EndPopup();
        }

        if (g_requestOverwrite) {
            g_requestOverwrite = false;
            OS::ui::OpenModal("overwrite_look");
        }
        if (OS::ui::BeginModal("overwrite_look")) {
            FUCK::Text("$FR_Looks_OverwriteConfirm"_T, g_actionFor.c_str());
            FUCK::Spacing();
            OS::ui::CentreTwoButtons("$FR_Looks_Save"_T, "$FR_Cancel"_T);
            if (ChamferPanel::Button("$FR_Looks_Save"_T).clicked) {
                if (player) {
                    BeginCapture(player, g_actionFor);
                }
                FUCK::CloseCurrentPopup();
            }
            FUCK::SameLine();
            if (ChamferPanel::Button("$FR_Cancel"_T).clicked) {
                FUCK::CloseCurrentPopup();
            }
            FUCK::EndPopup();
        }
    }

}  // namespace OS::ProfilesUI
