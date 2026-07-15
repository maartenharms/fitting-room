#include "EditorUI.h"

#include "AutoPresets.h"
#include "Collection.h"
#include "CrashGuard.h"
#include "EditorStyle.h"
#include "Favorites.h"
#include "Icons.h"
#include "EditorWindow.h"
#include "LoreModule.h"
#include "OutfitSession.h"
#include "PresetStore.h"
#include "Settings.h"
#include "SlotMask.h"
#include "StyleCatalog.h"
#include "StyleRef.h"

#include "FuckCompat.h"  // FUCK:: wrapper + OS::ui:: bridges for the ImGui deltas

#include <imgui.h>
#include <imgui_internal.h>  // PushItemFlag(FUCK::ItemFlags::kNoNav) - one nav stop per slot row
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

// Thread contract: Draw runs on the Present-hook (render) thread. Every library
// read goes through a SnapshotLibrary() copy taken this frame; every mutation
// goes through WithLibrary (locked) and never holds an Outfit* into library_
// across a mutation. The staged outfit is the live-preview channel: mutations
// call UpdateStaging, which refreshes the player on the main thread via the
// SKSE task queue - the render hook then reads the staged outfit through the
// same path as a committed one (a preview is not a second mechanism).

namespace OS::EditorUI {

    namespace {
        struct SlotRow {
            std::uint32_t bit;
            std::uint16_t icon;   // Font Awesome glyph shown in place of the slot number
            const char*   label;  // English source (reference)
            const char*   key;    // translation key; shown via FUCK::Translate(row.key)
        };

        // EVERY biped slot (mods use nearly all of them - pauldrons, skirts,
        // backpacks, capes…), each with a SkyUI-style icon. The community slot
        // number (bit + 30) lives in the row tooltip now, not the label. Shield
        // (39) is deliberately absent (kNeverTouchMask); anything else a given
        // setup must not touch belongs in the INI blocklist.
        constexpr SlotRow kSlots[] = {
            { 0, Icons::kMask, "Head", "$FR_Slot_Head" },
            { 1, Icons::kHelmet, "Hair / Helmet", "$FR_Slot_Hair" },
            { 2, Icons::kBody, "Body", "$FR_Slot_Body" },
            { 3, Icons::kMitten, "Hands", "$FR_Slot_Hands" },
            { 4, Icons::kHand, "Forearms", "$FR_Slot_Forearms" },
            { 5, Icons::kGem, "Amulet", "$FR_Slot_Amulet" },
            { 6, Icons::kRing, "Ring", "$FR_Slot_Ring" },
            { 7, Icons::kShoe, "Feet", "$FR_Slot_Feet" },
            { 8, Icons::kSocks, "Calves", "$FR_Slot_Calves" },
            { 10, Icons::kFeather, "Tail", "$FR_Slot_Tail" },
            { 11, Icons::kUser, "Long Hair", "$FR_Slot_LongHair" },
            { 12, Icons::kCrown, "Circlet", "$FR_Slot_Circlet" },
            { 13, Icons::kEar, "Ears", "$FR_Slot_Ears" },
            { 14, Icons::kSmile, "Face / Mouth", "$FR_Slot_Face" },
            { 15, Icons::kCube, "Neck", "$FR_Slot_Neck" },
            { 16, Icons::kVest, "Chest (outer)", "$FR_Slot_ChestOuter" },
            { 17, Icons::kBack, "Back", "$FR_Slot_Back" },
            { 18, Icons::kCube, "Misc", "$FR_Slot_Misc" },
            { 19, Icons::kCube, "Pelvis (outer)", "$FR_Slot_PelvisOuter" },
            { 20, Icons::kSkull, "Decapitated Head", "$FR_Slot_DecapHead" },
            { 21, Icons::kSkullX, "Decapitate", "$FR_Slot_Decapitate" },
            { 22, Icons::kCube, "Pelvis (under)", "$FR_Slot_PelvisUnder" },
            { 23, Icons::kCube, "Leg (right)", "$FR_Slot_LegR" },
            { 24, Icons::kCube, "Leg (left)", "$FR_Slot_LegL" },
            { 25, Icons::kSmile, "Face (alt)", "$FR_Slot_FaceAlt" },
            { 26, Icons::kBody, "Chest (under)", "$FR_Slot_ChestUnder" },
            { 27, Icons::kCube, "Shoulder", "$FR_Slot_Shoulder" },
            { 28, Icons::kHand, "Arm (left)", "$FR_Slot_ArmL" },
            { 29, Icons::kHand, "Arm (right)", "$FR_Slot_ArmR" },
            { 30, Icons::kCube, "Misc 2", "$FR_Slot_Misc2" },
            { 31, Icons::kMagic, "FX", "$FR_Slot_FX" },
        };

        Outfit        g_staged;
        // Per-slot entry stashed when the user hides a slot, so the Hide/Show
        // button restores the prior style/gear instead of destroying it (a
        // Hide→Show round trip must net zero Apply cost). Reset on open.
        std::array<SlotEntry, kBitCount> g_preHide{};
        // Undo/redo timeline over the staged outfit (OS-21). Reset to the
        // opened/switched outfit; a snapshot is recorded on every real edit
        // (through Push), never on transient hover-preview.
        EditHistory   g_history;
        std::uint32_t g_selectedBit   = kBitBody;
        bool          g_focusStyleList = false;  // controller: jump nav to the style panel after a slot pick (user)
        char          g_search[128]   = {};
        bool          g_dirty         = false;
        bool          g_collectedOnly = true;   // default from Settings at OnOpen
        bool          g_favoritesOnly = false;  // browser filter: only starred looks (session-local)
        bool          g_hideUnfit     = true;   // browser filter: hide body-unfit (red) rows (session-local, default on)
        int           g_armorType     = -1;     // browser filter: -1 any, 0 light, 1 heavy, 2 clothing
        float         g_uiScale       = 1.0f;   // live editor scale (io.FontGlobalScale); from Settings
        bool          g_hoverPreview  = true;   // preview a style just by hovering its row (from Settings)
        StyleRefKey   g_hoverKey;               // style currently hover-previewed (empty = none)
        StyleRefKey   g_hoverPending;           // debounced hover candidate
        double        g_hoverPendingSince = 0.0;
        bool          g_showClass     = true;   // browser columns (the gear config); persist across opens
        bool          g_showSource    = true;   // the Mod (ESP) column
        std::uint32_t g_wornMask      = 0;      // worn-at-open, for empty-slot dimming
        std::uint32_t g_matchMask     = 0;      // slots with search hits (highlight)
        std::string   g_lastMatchQuery;
        bool          g_lastMatchCollected = true;
        int           g_lastMatchArmorType = -1;
        bool          g_lastMatchFavorites = false;
        int           g_forceSelect        = -1;  // tab index to force-select for one frame
        bool          g_justOpened         = false;  // suppress tab-activate on the open frame
        std::uint64_t g_gold               = 0;   // player gold at open (input is modal)

        // Showcases (read-only preset browser). While the tab is open the
        // staging channel shows the clicked preset; g_staged (the edit
        // buffer) is untouched and is re-staged on the way out.
        bool        g_showcasesOpen  = false;
        int         g_pendingDelete  = -1;   // outfit index whose tab-X delete is being confirmed
        int         g_showcaseSel    = -1;   // index into this frame's Snapshot()
        int         g_showcaseSource = 1;    // 0 = Curated (authored), 1 = Discovered (auto); default Discovered (OS-56)
        char        g_showcaseSearch[128] = {};
        std::string g_footNote;              // transient footer note (e.g. export path)
        double      g_footNoteUntil = 0.0;

        // May-not-fit rows (OS-3): muted blood red - readable on the near-
        // black panel, unmistakably "broken" next to parchment and gold.
        constexpr ImVec4 kUnfitText{ 0.80f, 0.34f, 0.27f, 1.0f };

        // Journal-gold highlight: search hits, favorited stars, the NEW badge,
        // and the Apply gold cost. A deliberate highlight colour (Fuzzles' theme
        // note: override the theme sparingly, for highlights on text or icons
        // only - these qualify). One named constant so the palette is explicit.
        constexpr ImVec4 kGold{ 1.0f, 0.82f, 0.2f, 1.0f };

        // Case-insensitive substring find, for the in-row match highlight.
        std::size_t FindCI(std::string_view a_hay, std::string_view a_needle) {
            if (a_needle.empty() || a_needle.size() > a_hay.size()) {
                return std::string_view::npos;
            }
            const auto lower = [](char a_c) {
                return std::tolower(static_cast<unsigned char>(a_c));
            };
            for (std::size_t i = 0; i + a_needle.size() <= a_hay.size(); ++i) {
                std::size_t j = 0;
                while (j < a_needle.size() && lower(a_hay[i + j]) == lower(a_needle[j])) {
                    ++j;
                }
                if (j == a_needle.size()) {
                    return i;
                }
            }
            return std::string_view::npos;
        }

        // The slots everyone actually uses; Advanced reveals the rest. Slots
        // with an active Style/Hide entry always show regardless.
        constexpr std::uint32_t kDefaultSlotMask =
            MaskForEditorSlot(30) | MaskForEditorSlot(31) | MaskForEditorSlot(32) |
            MaskForEditorSlot(33) | MaskForEditorSlot(35) | MaskForEditorSlot(36) |
            MaskForEditorSlot(37) | MaskForEditorSlot(42) | MaskForEditorSlot(46) |
            MaskForEditorSlot(47);

        void Push() {
            OutfitSession::GetSingleton().UpdateStaging(g_staged);
            g_dirty = true;
            // Every real edit funnels through here (style pick, Remove/Show,
            // real-gear, Random) - and ONLY real edits do; transient
            // hover-preview pushes straight through UpdateStaging(tmp). So this
            // is the one place undo history is recorded (OS-21).
            g_history.Record(g_staged);
        }

        // Restore a snapshot from the undo timeline onto the staged buffer.
        // Preserves the CURRENT name (renames are a separate, non-undoable
        // path) and pushes through UpdateStaging directly - NOT Push() - so
        // the restore is never itself recorded as a fresh edit.
        void ApplyHistory(OutfitSession& a_session, const Outfit& a_state) {
            const std::string keepName = g_staged.name;
            g_staged      = a_state;
            g_staged.name = keepName;
            CrashGuard::ClearPreviewing();
            g_hoverKey     = StyleRefKey{};
            g_hoverPending = StyleRefKey{};
            a_session.UpdateStaging(g_staged);
            g_dirty = true;  // reconciled against ChangedSlotCount in the footer
            EditorStyle::PlayUISound("UIMenuFocus");
        }

        const char* KindLabel(const SlotEntry& a_entry) {
            switch (a_entry.kind) {
                case SlotEntry::Kind::kHide:
                    return "$FR_StateHidden"_T;
                case SlotEntry::Kind::kStyle:
                    return "Styled";
                default:
                    return "$FR_StateEquipped"_T;
            }
        }

        const char* SlotLabelForBit(std::uint32_t a_bit) {
            for (const auto& row : kSlots) {
                if (row.bit == a_bit) {
                    return FUCK::Translate(row.key);
                }
            }
            return nullptr;  // shield (39) - never in an outfit
        }

        // Make the staged outfit the library's active outfit, creating one when
        // the library is empty - CommitStaging writes only into an ACTIVE
        // outfit, so Apply on a fresh library would otherwise silently drop it.
        void EnsureActiveOutfit(OutfitSession& a_session) {
            a_session.WithLibrary([&](OutfitLibrary& a_lib) {
                if (a_lib.ActiveIndex() < 0) {
                    const int idx = a_lib.Create(g_staged.name);
                    if (idx >= 0) {
                        a_lib.Activate(static_cast<std::size_t>(idx));
                    }
                }
            });
        }

        // Transient footer note (export path, etc.) - shown in both footer
        // variants until its timestamp lapses.
        void DrawFootNote() {
            if (g_footNote.empty()) {
                return;
            }
            if (FUCK::GetTime() >= g_footNoteUntil) {
                g_footNote.clear();
                return;
            }
            FUCK::SameLine();
            FUCK::TextDisabled("%s", g_footNote.c_str());
        }

        // The Showcases body: left = searchable preset list grouped by
        // author, right = detail pane for the selected preset. Clicking a
        // preset stages it (the same live-preview channel as editing - a
        // preview is not a second mechanism); g_staged is deliberately NOT
        // touched, so leaving the tab restores the edit buffer.
        void DrawShowcases(OutfitSession& a_session,
                           const std::vector<JsonCodec::Preset>& a_presets,
                           float a_footerH) {
            const float listW = OS::ui::FontSize() * 16.0f;
            FUCK::BeginChild("showcase_list", ImVec2(listW, -a_footerH),
                              true);
            // Discovered (auto-detected from the player's armor mods) vs Curated
            // (author-shipped). The two feed the same grouped list. Discovered is
            // leftmost + the default (OS-56); each sub-tab carries its own tooltip.
            const auto sourceTab = [&](const char* a_label, int a_src, const char* a_tip) {
                const ImVec2 sz(FUCK::CalcTextSize(a_label).x + OS::ui::FramePadding().x * 2.0f, 0.0f);
                if (FUCK::Selectable(a_label, g_showcaseSource == a_src, 0, sz)) {
                    g_showcaseSource = a_src;
                    g_showcaseSel    = -1;
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                if (FUCK::IsItemHovered()) {
                    OS::ui::SetTooltipF("%s", a_tip);
                }
            };
            sourceTab("$FR_Discovered"_T, 1, "$FR_DiscoveredTip"_T);
            FUCK::SameLine();
            sourceTab("$FR_Curated"_T, 0, "$FR_CuratedTip"_T);

            // OS-56: RB / LB switch the Discovered <-> Curated sub-tabs on a controller
            // (mirrors the outfit-tab shoulder switch). Two sources today, so both
            // shoulders just flip; written as a cycle to stay correct if a third source
            // is ever added. Gated on !IsAnyItemActive so the search field keeps its
            // keys. Safe here - the showcases block returns before the outfit LB/LB
            // handler, so the shoulders are read once. The new source takes effect next
            // frame (this frame's list was fetched for the old one), matching a tab click.
            if (!FUCK::IsAnyItemActive()) {
                const bool next = FUCK::IsKeyPressed(ImGuiKey_GamepadR1, false);
                const bool prev = FUCK::IsKeyPressed(ImGuiKey_GamepadL1, false);
                if (next || prev) {
                    constexpr int kSources = 2;
                    g_showcaseSource       = next ? (g_showcaseSource + 1) % kSources
                                                  : (g_showcaseSource - 1 + kSources) % kSources;
                    g_showcaseSel          = -1;
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
            }
            FUCK::Separator();

            const float rescanW = FUCK::CalcTextSize("$FR_Rescan"_T).x +
                                  OS::ui::FramePadding().x * 2.0f;
            FUCK::SetNextItemWidth(-(rescanW + OS::ui::ItemSpacing().x));
            FUCK::InputText("##showcase_search", g_showcaseSearch, sizeof(g_showcaseSearch));
            FUCK::SameLine();
            if (FUCK::Button("$FR_Rescan"_T)) {
                if (g_showcaseSource == 1) {
                    // Pull in anything obtained since load before rebuilding the
                    // discovered sets, so Rescan reflects the real inventory (not
                    // just the possibly-stale collection).
                    Collection::GetSingleton().SeedFromPlayerInventory();
                    AutoPresets::RequestRescan();
                } else {
                    PresetStore::RequestRescan();
                }
                g_showcaseSel = -1;
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(g_showcaseSource == 1
                                      ? "Re-scan your armor mods for outfit sets."
                                      : "Re-read the Presets folder.\nAuthors: drop a "
                                        "file in, click, see it.");
            }
            FUCK::Separator();

            // The store is sorted by author (= mod/plugin for Discovered), so
            // groups are contiguous. Each plugin is a collapsible accordion whose
            // header shows the name + match count; searching auto-expands them.
            const std::string_view q{ g_showcaseSearch };
            const auto             matches = [&](const JsonCodec::Preset& p) {
                return q.empty() || FindCI(p.name, q) != std::string_view::npos ||
                       FindCI(p.author, q) != std::string_view::npos ||
                       FindCI(p.description, q) != std::string_view::npos;
            };
            std::map<std::string, int> matchCount;  // per author (= plugin)
            for (const auto& p : a_presets) {
                if (matches(p)) {
                    ++matchCount[p.author];
                }
            }

            std::string lastAuthor{ "\x01" };  // never a real author
            bool        groupOpen = false;
            bool        any       = false;
            for (std::size_t i = 0; i < a_presets.size(); ++i) {
                const auto& p = a_presets[i];
                if (!matches(p)) {
                    continue;
                }
                any = true;
                if (p.author != lastAuthor) {  // new plugin group
                    lastAuthor = p.author;
                    const std::string name = p.author.empty() ? "(unknown)" : p.author;
                    const std::string header = name + " (" +
                                               std::to_string(matchCount[p.author]) +
                                               ")###grp" + p.author;
                    if (!q.empty()) {  // a live search reveals every matching group
                        FUCK::SetNextItemOpen(true, ImGuiCond_Always);
                    }
                    groupOpen = FUCK::CollapsingHeader(header.c_str(),
                                                        ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!groupOpen) {
                    continue;  // collapsed plugin - skip its rows
                }
                FUCK::PushID(static_cast<int>(i));
                FUCK::Indent();
                if (FUCK::Selectable(p.name.c_str(),
                                      g_showcaseSel == static_cast<int>(i))) {
                    g_showcaseSel = static_cast<int>(i);
                    CrashGuard::ClearPreviewing();  // whole preset - no single style to blame
                    a_session.BeginStaging(p.outfit);  // try it on, live
                    EditorStyle::PlayUISound("UIMenuOK");
                }
                if (FUCK::IsItemHovered()) {
                    OS::ui::SetTooltipF("%s", p.file.c_str());
                }
                FUCK::Unindent();
                FUCK::PopID();
            }
            if (!any) {
                if (a_presets.empty() && g_showcaseSource == 1) {
                    FUCK::TextDisabled("%s", "$FR_NoDiscovered"_T);
                } else {
                    FUCK::TextDisabled("%s", "$FR_NoMatches"_T);
                }
            }
            FUCK::EndChild();

            FUCK::SameLine();
            FUCK::BeginChild("showcase_detail", ImVec2(0, -a_footerH),
                              true);
            if (g_showcaseSel >= 0 &&
                g_showcaseSel < static_cast<int>(a_presets.size())) {
                const auto& p = a_presets[static_cast<std::size_t>(g_showcaseSel)];
                if (auto* title = FUCK::GetFont(FUCK::Font::kLarge)) {
                    FUCK::PushFont(title);
                    FUCK::TextUnformatted(p.name.c_str());
                    FUCK::PopFont();
                } else {
                    FUCK::TextUnformatted(p.name.c_str());
                }
                FUCK::TextDisabled("$FR_PresetBy"_T,
                                    p.author.empty() ? "unknown" : p.author.c_str(),
                                    p.file.c_str());
                if (!p.description.empty()) {
                    FUCK::Spacing();
                    FUCK::TextWrapped("%s", p.description.c_str());
                }
                FUCK::Spacing();
                FUCK::Separator();
                int unfitPieces = 0;
                for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
                    const auto& entry = p.outfit.EntryFor(bit);
                    if (entry.kind == SlotEntry::Kind::kPassthrough) {
                        continue;
                    }
                    const char* label = SlotLabelForBit(bit);
                    if (!label) {
                        continue;
                    }
                    if (entry.kind == SlotEntry::Kind::kHide) {
                        FUCK::Text("$FR_PieceHidden"_T, label);
                    } else if (auto* armo = StyleRef::Resolve(entry.style)) {
                        // Same fit flag as the style browser (OS-3): race + sex.
                        if (const auto reason = StyleCatalog::EvaluateFit(armo);
                            reason != FitReason::kFits) {
                            ++unfitPieces;
                            FUCK::TextColored(kUnfitText, "%s: %s", label,
                                               armo->GetName());
                            if (FUCK::IsItemHovered()) {
                                OS::ui::SetTooltipF("$FR_MayNotFit"_T,
                                                  entry.style.modName.c_str(),
                                                  StyleCatalog::FitReasonText(reason).c_str());
                            }
                        } else {
                            FUCK::Text("%s: %s", label, armo->GetName());
                            if (FUCK::IsItemHovered()) {
                                OS::ui::SetTooltipF("%s", entry.style.modName.c_str());
                            }
                        }
                    } else {
                        // An optional-integration piece whose plugin is not
                        // installed: inert, the rest of the fit still works.
                        FUCK::TextDisabled("$FR_PieceMissing"_T, label,
                                            entry.style.modName.c_str());
                    }
                }
                if (unfitPieces > 0) {
                    FUCK::Spacing();
                    FUCK::TextColored(kUnfitText, "%d piece%s may not fit your body",
                                       unfitPieces, unfitPieces == 1 ? "" : "s");
                }
                if (!p.requires_.empty()) {
                    std::string joined;
                    for (const auto& r : p.requires_) {
                        joined += joined.empty() ? r : ", " + r;
                    }
                    FUCK::Spacing();
                    FUCK::TextDisabled("$FR_Requires"_T, joined.c_str());
                }
            } else {
                FUCK::TextDisabled("%s", "$FR_SelectPreset"_T);
                FUCK::Spacing();
                FUCK::TextWrapped(
                    "Presets are curated fits shipped by armor mods. Previews are "
                    "free, and nothing changes until you save one to your outfits.");
            }
            FUCK::EndChild();
        }
    }

    void OnOpen() {
        auto& session   = OutfitSession::GetSingleton();

        // Re-sync the known-looks collection with the player's current inventory.
        // The live container-changed sink misses some acquisition paths (crafting
        // and tempering, some script- or quest-granted gear), and the load-time
        // seed only runs once, so without this a look you obtained since load is
        // not learned until the next reload. SeedFromPlayerInventory is idempotent
        // (already-known looks are skipped) and just walks the inventory once, so
        // it is cheap to run on every open; the rescan rebuilds the discovered
        // sets from the refreshed collection.
        Collection::GetSingleton().SeedFromPlayerInventory();
        AutoPresets::RequestRescan();

        g_collectedOnly = Settings::GetSingleton().collectionOnly;
        g_uiScale       = std::clamp(Settings::GetSingleton().uiScale, 0.8f, 1.6f);
        g_hoverPreview  = Settings::GetSingleton().hoverPreview;
        g_hoverKey      = StyleRefKey{};
        g_hoverPending  = StyleRefKey{};
        g_armorType     = -1;   // "All types" each open
        g_favoritesOnly = false;  // "Favorites" filter is session-local, off each open
        g_hideUnfit     = true;   // hide body-unfit (red) rows by default each open (best UX per user)
        g_matchMask     = 0;
        g_preHide.fill(SlotEntry{});   // no per-slot un-hide memory yet this session
        g_lastMatchQuery.clear();
        g_lastMatchArmorType = -1;
        g_lastMatchFavorites = false;
        g_showcasesOpen = false;
        g_showcaseSel   = -1;
        g_pendingDelete = -1;
        g_footNote.clear();

        // Fresh library: create a ready "Outfit 1" so the editor never opens
        // onto an empty tab bar.
        if (session.SnapshotLibrary().Count() == 0) {
            session.WithLibrary([](OutfitLibrary& a_lib) {
                const int idx = a_lib.Create("Outfit 1");
                if (idx >= 0) {
                    a_lib.Activate(static_cast<std::size_t>(idx));
                }
            });
        }

        const auto snap = session.SnapshotLibrary();
        if (const auto* active = snap.Active()) {
            g_staged = *active;
        } else {
            g_staged      = Outfit{};
            g_staged.name = "Outfit 1";
        }
        g_dirty = false;
        g_history.Reset(g_staged);  // undo baseline = the outfit we opened on
        // The tab bar restores ITS OWN remembered selection on reopen (usually
        // the first tab) - force it onto the ACTIVE outfit instead, and mute
        // the switch handler for the opening frame so the stale selection can
        // never activate a different outfit than the one being worn.
        g_forceSelect = snap.ActiveIndex();
        g_justOpened  = true;

        // Worn + gold snapshots for dimming and the lore-mode Apply cost.
        // Input is modal while the editor is open, so neither can change.
        g_wornMask = 0;
        g_gold     = 0;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                if (player->GetWornArmor(static_cast<Slot>(1u << bit))) {
                    g_wornMask |= 1u << bit;
                }
            }
            // NEVER Actor::GetGoldAmount(): this CommonLibSSE-NG build routes
            // it through BGSDefaultObjectManager::GetObject(DefaultObjectID),
            // which mis-reads the inline objectInit[] array at +0xB80 as a
            // bool POINTER and dereferences 0x0101010101010101 - instant CTD
            // (proven: crash-2026-07-11-19-54-07). Count the gold form
            // directly; it is the same form the Apply deduction removes.
            if (auto* goldForm = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F)) {
                const auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& a_o) { return &a_o == goldForm; });
                if (const auto it = inv.find(goldForm);
                    it != inv.end() && it->second.first > 0) {
                    g_gold = static_cast<std::uint64_t>(it->second.first);
                }
            }
        }
        session.BeginStaging(g_staged);
    }

    void OnClose() {
        // Closing without Apply reverts: nothing is committed until the button.
        CrashGuard::ClearPreviewing();
        g_hoverKey     = StyleRefKey{};
        g_hoverPending = StyleRefKey{};
        OutfitSession::GetSingleton().DiscardStaging();
        g_dirty = false;
    }

    void Draw() {
        auto&      session = OutfitSession::GetSingleton();
        const auto snap    = session.SnapshotLibrary();

        // The editor draws as a FUCK IWindow: FUCK owns the window chrome, size,
        // position and Present, so we draw content only. The game UI is hidden
        // while open and the right side of the screen is the Show-Player-In-Menus
        // character. Close via the editor hotkey / Esc (kCloseOnEsc).
        // (UI-size is no longer routed through SetWindowFontScale - it didn't visibly
        // resize glyphs on this FUCK build. g_uiScale now drives the style-browser font
        // push below, so the "UI size" slider actually changes the table text density.)

        // Controller: keep the nav highlight asserted while a gamepad is the active
        // input so it stays in panel-nav mode instead of dropping into FUCK's
        // mouse-cursor mode (field: "weird cursor mode"). Best-effort; version-gated
        // no-op on older FUCK - if it doesn't hold, this becomes a Fuzzles question.
        if (FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad) {
            FUCK::SetNavCursorVisible(true);
        }

        // Title (left). Large display font; AlignTextToFramePadding lines it up with the
        // top-right button cluster drawn on the same row.
        if (auto* titleFont = FUCK::GetFont(FUCK::Font::kLarge)) {
            FUCK::PushFont(titleFont);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(g_showcasesOpen ? "PRESETS" : "FITTING ROOM");
            FUCK::PopFont();
        } else {
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(g_showcasesOpen ? "PRESETS" : "FITTING ROOM");
        }

        // Top-right cluster: [Presets] [gear], gear rightmost. The gear moved here, out
        // of the slot beside the title (its box never lined up with the large-font title;
        // this supersedes the OS-57 sizing attempt - user's call). Right-aligned as a
        // group so the gear hugs the right edge. The gear is always shown; Presets only
        // when presets exist. The settings popup follows the gear button below, so it
        // opens the same frame and positions near the gear.
        const bool  hasPresets = PresetStore::GetSingleton().Count() > 0;
        const char* pLabel     = g_showcasesOpen ? "$FR_Outfits"_T : "$FR_Presets"_T;
        const float gearW = FUCK::CalcTextSize(Icons::Utf8(Icons::kGear).c_str()).x +
                            OS::ui::FramePadding().x * 2.0f;
        const float presetsW =
            hasPresets ? FUCK::CalcTextSize(pLabel).x + OS::ui::FramePadding().x * 2.0f : 0.0f;
        const float gap = hasPresets ? OS::ui::ItemSpacing().x : 0.0f;
        FUCK::SameLine(FUCK::GetWindowSize().x - OS::ui::WindowPadding().x - gearW - gap -
                       presetsW);
        if (hasPresets) {
            // Presets (author showcases): toggle the read-only presets browser.
            if (FUCK::Button(pLabel)) {
                if (g_showcasesOpen) {
                    // Back to the outfits: re-assert the edit buffer.
                    g_showcasesOpen = false;
                    g_showcaseSel   = -1;
                    session.BeginStaging(g_staged);
                } else {
                    g_showcasesOpen = true;
                    g_showcaseSel   = -1;  // staging keeps the edit buffer until a click
                }
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(g_showcasesOpen ? "$FR_BackToOutfits"_T
                                                    : "$FR_BrowsePresets"_T);
            }
            FUCK::SameLine();
        }
        // Editor settings (gear): a live UI-size slider so the panel can be scaled while
        // it's open, without the SKSE menu framework. No "##id" suffix: FUCK sizes the
        // button to the FULL label (## included), so a suffix makes a single-glyph button
        // wide; the glyph is a unique id on its own.
        if (FUCK::Button(Icons::Utf8(Icons::kGear).c_str())) {
            FUCK::OpenPopup("##editor_settings");
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_GearTip"_T);
        }
        if (FUCK::BeginPopup("##editor_settings")) {
            FUCK::TextDisabled("%s", "$FR_EditorSettings"_T);
            FUCK::Separator();
            FUCK::SetNextItemWidth(OS::ui::FontSize() * 8.0f);
            if (FUCK::SliderFloat("$FR_UiSize"_T, &g_uiScale, 0.8f, 1.6f, "%.2fx")) {
                Settings::GetSingleton().uiScale = g_uiScale;  // live
            }
            if (FUCK::IsItemDeactivatedAfterEdit()) {
                Settings::GetSingleton().Save();  // persist on release
            }
            if (FUCK::Checkbox("$FR_PreviewOnHover"_T, &g_hoverPreview, false, false)) {
                Settings::GetSingleton().hoverPreview = g_hoverPreview;
                Settings::GetSingleton().Save();
                if (!g_hoverPreview && !g_hoverKey.Empty()) {  // turning it off drops any preview
                    CrashGuard::ClearPreviewing();
                    OutfitSession::GetSingleton().UpdateStaging(g_staged);
                    g_hoverKey = StyleRefKey{};
                }
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_PreviewOnHoverTip"_T);
            }
            // Style-list columns moved here (OS-30) from a separate gear in the
            // browser - one settings home. Name is always shown.
            FUCK::Separator();
            FUCK::TextDisabled("%s", "$FR_StyleColumns"_T);
            FUCK::Checkbox("$FR_ColClass"_T, &g_showClass, false, false);
            FUCK::Checkbox("$FR_ColPlugin"_T, &g_showSource, false, false);
            FUCK::Separator();
            if (FUCK::Checkbox("$FR_LockWindow"_T, &Settings::GetSingleton().lockLayout, false, false)) {
                Settings::GetSingleton().Save();
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(
                    "$FR_LockWindowTip"_T);
            }
            if (FUCK::Button("$FR_ResetWindow"_T)) {
                OS::EditorWindow::ResetGeometry();
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_ResetWindowTip"_T);
            }
            FUCK::EndPopup();
        }
        FUCK::Spacing();

        // ---- outfit tabs (hidden while browsing Presets) --------------------
        bool requestDeletePopup = false;  // tab-X sets this; OpenPopup runs at window scope below
        if (!g_showcasesOpen &&
            FUCK::BeginTabBar("outfits", ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (std::size_t i = 0; i < snap.Count(); ++i) {
                const auto* o = snap.At(i);
                FUCK::PushID(static_cast<int>(i));  // duplicate names must not collide
                // "###" pins the tab's ImGui identity to the INDEX, not the
                // visible name - renaming used to change the tab's ID, which
                // dropped the tab-bar selection back to the first tab (the
                // reported reorder-on-rename / wrong-rename behavior).
                // Spaces pad the label off the tab's left/right border (FUCK draws the
                // tab text flush to the edge); "###" keeps the id pinned to the index.
                const std::string tabLabel = " " + o->name + " ###outfit";
                const ImGuiTabItemFlags flags =
                    g_forceSelect == static_cast<int>(i) ? ImGuiTabItemFlags_SetSelected : 0;
                // The tab's own X is the delete control (never on the last
                // outfit - the library is never empty).
                // FUCK tab items have no built-in close X; outfit deletion is
                // the Delete button in the name row (OS-40).
                if (FUCK::BeginTabItem(tabLabel.c_str(), flags)) {
                    if (snap.ActiveIndex() != static_cast<int>(i) && !g_justOpened) {
                        session.WithLibrary([&](OutfitLibrary& lib) { lib.Activate(i); });
                        g_staged = *o;  // from this frame's snapshot copy
                        session.BeginStaging(g_staged);
                        g_dirty = false;
                        g_history.Reset(g_staged);  // undo is per-outfit - fresh timeline
                        EditorStyle::PlayUISound("UIMenuFocus");
                    }
                    FUCK::EndTabItem();
                }
                FUCK::PopID();
            }
            FUCK::EndTabBar();
            // "+" new-outfit: plain button after the tab bar. (A fake BeginTabItem was
            // tried so it would align like a tab, but it double-created on select AND
            // still read awkwardly - reverted. "+" alignment is an OPEN polish item.)
            if (snap.Count() < kMaxOutfits) {
                FUCK::SameLine();
                if (FUCK::Button("+")) {
                    Outfit fresh;
                    fresh.name = "Outfit " + std::to_string(snap.Count() + 1);
                    session.WithLibrary([&](OutfitLibrary& lib) {
                        const int idx = lib.Create(fresh.name);
                        if (idx >= 0) {
                            lib.Activate(static_cast<std::size_t>(idx));
                        }
                    });
                    g_staged = fresh;
                    session.BeginStaging(g_staged);
                    g_dirty       = false;
                    g_history.Reset(g_staged);
                    g_forceSelect = static_cast<int>(snap.Count());  // the new tab
                    EditorStyle::PlayUISound("UIMenuOK");
                }
            }
            // Consume once the target tab actually rendered; a tab created
            // THIS frame (index == snap.Count()) renders next frame.
            if (g_forceSelect >= 0 && g_forceSelect < static_cast<int>(snap.Count())) {
                g_forceSelect = -1;
            }
            g_justOpened = false;  // the open-frame mute lasts one tab-bar pass
        }

        // Delete confirm. OpenPopup MUST be called here at WINDOW scope, not
        // inside BeginTabBar (which pushes its own id) - that hashed the popup
        // id under the tab bar while this modal is hashed at window scope, so
        // they never matched and the tab X appeared to do nothing (OS-29).
        if (requestDeletePopup) {
            FUCK::OpenPopup("delete_outfit");
        }
        if (FUCK::BeginPopupModal("delete_outfit", nullptr,
                                   FUCK::WindowFlags::kAutoResize)) {
            const auto* victim = g_pendingDelete >= 0
                                     ? snap.At(static_cast<std::size_t>(g_pendingDelete))
                                     : nullptr;
            FUCK::Text("$FR_DeleteConfirm"_T, victim ? victim->name.c_str() : "?");
            FUCK::Spacing();
            if (FUCK::Button("$FR_Delete"_T)) {
                const int del       = g_pendingDelete;
                int       newActive = -1;
                session.WithLibrary([&](OutfitLibrary& lib) {
                    if (del < 0 || del >= static_cast<int>(lib.Count())) {
                        return;
                    }
                    lib.Remove(static_cast<std::size_t>(del));
                    if (lib.Count() == 0) {
                        newActive = lib.Create("Outfit 1");  // never empty
                    } else {
                        newActive = lib.ActiveIndex();  // Remove() adjusted it
                        if (newActive < 0) {
                            newActive = std::min(del, static_cast<int>(lib.Count()) - 1);
                        }
                    }
                    if (newActive >= 0) {
                        lib.Activate(static_cast<std::size_t>(newActive));
                    }
                });
                const auto s2 = session.SnapshotLibrary();
                if (const auto* a2 = s2.Active()) {
                    g_staged = *a2;
                }
                session.BeginStaging(g_staged);
                g_dirty         = false;
                g_history.Reset(g_staged);
                g_forceSelect   = newActive;
                g_pendingDelete = -1;
                EditorStyle::PlayUISound("UIMenuCancel");
                FUCK::CloseCurrentPopup();
            }
            FUCK::SameLine();
            if (FUCK::Button("$FR_Cancel"_T)) {
                g_pendingDelete = -1;
                FUCK::CloseCurrentPopup();
            }
            FUCK::EndPopup();
        }

        // Rename the active outfit in place (the spec's outfits are NAMED).
        // PushID gives each outfit its OWN InputText identity - without it,
        // switching tabs mid-edit kept the previous field's live edit state
        // and the rename landed on the wrong outfit. The rename also reads the
        // active index LIVE under the lock, never from this frame's snapshot.
        // The whole row belongs to the outfit editor - hidden while browsing
        // Showcases (read-only mode).
        if (!g_showcasesOpen && snap.ActiveIndex() >= 0) {
            const auto isBlank = [](const std::string& s) {
                return s.find_first_not_of(" \t\r\n") == std::string::npos;
            };
            FUCK::PushID(snap.ActiveIndex());
            // "Name" label to the LEFT of the field (ImGui's built-in label sits
            // to the right); the field itself carries a hidden ## label.
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Name"_T);
            FUCK::SameLine();
            // Stretch the name field toward the right-aligned action cluster (Export /
            // Reset / Hide All / trash) instead of a fixed 220px (user). Reserve
            // the cluster's footprint (the same widths it right-aligns with below) and
            // fill up to it; never shrink below the old 220.
            {
                const float rExp   = FUCK::CalcTextSize("$FR_Export"_T).x + OS::ui::FramePadding().x * 2.0f;
                const float rClr   = FUCK::CalcTextSize("$FR_ResetOutfit"_T).x + OS::ui::FramePadding().x * 2.0f;
                const float rHide  = FUCK::CalcTextSize("$FR_HideAll"_T).x + OS::ui::FramePadding().x * 2.0f;
                const float rTrash = FUCK::CalcTextSize(Icons::Utf8(Icons::kTrash).c_str()).x +
                                     OS::ui::FramePadding().x * 2.0f;
                const float clusterLeft = FUCK::GetWindowSize().x - rExp - rClr - rHide - rTrash -
                                          4.0f * OS::ui::ItemSpacing().x - OS::ui::WindowPadding().x;
                const float nameW =
                    std::max(220.0f, clusterLeft - FUCK::GetCursorPos().x - OS::ui::ItemSpacing().x);
                FUCK::SetNextItemWidth(nameW);
            }
            // An outfit name is NAMED and must never be empty - rename only on a
            // non-blank edit, and if the field is left blank restore the last
            // good name (or a default) when it loses focus.
            if (FUCK::InputText("##Name", &g_staged.name) && !isBlank(g_staged.name)) {
                session.WithLibrary([&](OutfitLibrary& lib) {
                    if (lib.ActiveIndex() >= 0) {
                        lib.Rename(static_cast<std::size_t>(lib.ActiveIndex()), g_staged.name);
                    }
                });
            }
            if (FUCK::IsItemDeactivatedAfterEdit() && isBlank(g_staged.name)) {
                if (const auto* a = snap.At(static_cast<std::size_t>(snap.ActiveIndex()))) {
                    g_staged.name = a->name;  // last committed (non-blank) name
                }
                if (isBlank(g_staged.name)) {
                    g_staged.name = "Outfit";
                }
                session.WithLibrary([&](OutfitLibrary& lib) {
                    if (lib.ActiveIndex() >= 0) {
                        lib.Rename(static_cast<std::size_t>(lib.ActiveIndex()), g_staged.name);
                    }
                });
            }
            FUCK::PopID();
        }
        // Export + Reset + Hide All - per-outfit actions, right-aligned
        // on the name row. (Random moved into the style panel, by the filters.)
        if (!g_showcasesOpen) {
            const std::string trashLabel = Icons::Utf8(Icons::kTrash);
            const bool  canDelete = snap.Count() > 1;  // never delete the last outfit
            // Hide All doubles as Show All (OS-47): when every editable slot is
            // already hidden, the button clears them back to equipped gear.
            const std::uint32_t forbidden =
                kNeverTouchMask | Settings::GetSingleton().slotBlocklist;
            bool allHidden = true;
            for (const auto& row : kSlots) {
                if ((forbidden >> row.bit) & 1u) {
                    continue;
                }
                if (g_staged.EntryFor(row.bit).kind != SlotEntry::Kind::kHide) {
                    allHidden = false;
                    break;
                }
            }
            const char* hideLabel = allHidden ? "$FR_ShowAll"_T : "$FR_HideAll"_T;
            const float expW  = FUCK::CalcTextSize("$FR_Export"_T).x + OS::ui::FramePadding().x * 2.0f;
            const float clrW  = FUCK::CalcTextSize("$FR_ResetOutfit"_T).x + OS::ui::FramePadding().x * 2.0f;
            const float hideW = FUCK::CalcTextSize(hideLabel).x + OS::ui::FramePadding().x * 2.0f;
            const float delW  = canDelete
                ? FUCK::CalcTextSize(trashLabel.c_str()).x + OS::ui::FramePadding().x * 2.0f
                : 0.0f;
            if (snap.ActiveIndex() >= 0) {
                FUCK::SameLine();  // stay on the name row
            }
            FUCK::SetCursorPosX(std::max(FUCK::GetCursorPos().x,
                                          FUCK::GetWindowSize().x - expW - clrW - hideW - delW -
                                              (canDelete ? 3.0f : 2.0f) * OS::ui::ItemSpacing().x -
                                              OS::ui::WindowPadding().x - OS::ui::ItemSpacing().x));
            if (FUCK::Button("$FR_Export"_T)) {
                const auto path = PresetStore::ExportOutfit(g_staged);
                g_footNote      = path.empty() ? "$FR_ExportFail"_T
                                               : "$FR_SharedTo"_T + path;
                g_footNoteUntil = FUCK::GetTime() + 6.0;
                EditorStyle::PlayUISound("UIMenuOK");
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF("$FR_ExportTip"_T);
            }
            FUCK::SameLine();
            if (FUCK::Button("$FR_ResetOutfit"_T)) {
                // Reset every slot to real worn gear AND unhide everything (renamed
                // "Clear Outfit" -> "Reset" per user). An empty Outfit is all-passthrough
                // (SlotEntry defaults to kPassthrough), so this reverts styles AND clears
                // any Hidden slots back to equipped gear in one go. The bulk version of the
                // per-slot X. Keeps the outfit and its name, and STAGES the
                // change (so the preview shows real gear and Apply commits the
                // emptied outfit). The old behavior only Deactivated the library
                // and discarded staging, leaving g_staged untouched - so the
                // slots kept their styles and re-selecting the tab brought them
                // back: it never actually cleared the outfit.
                CrashGuard::ClearPreviewing();
                const std::string keepName = g_staged.name;
                g_staged      = Outfit{};
                g_staged.name = keepName;
                Push();  // preview -> real gear, mark dirty, record for undo
                EditorStyle::PlayUISound("UIMenuCancel");
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_ResetOutfitTip"_T);
            }
            FUCK::SameLine();
            if (FUCK::Button(hideLabel)) {
                // Hide All / Show All (OS-47). Hide All sets every editable slot
                // to Hidden (renders invisible); once all are hidden the button
                // reads Show All and clears them back to equipped gear. Skips the
                // never-touch slots (shield) + the INI blocklist so the Apply
                // cost counts only real changes. Staged + undoable.
                CrashGuard::ClearPreviewing();
                for (const auto& row : kSlots) {
                    if ((forbidden >> row.bit) & 1u) {
                        continue;
                    }
                    if (allHidden) {
                        g_staged.SetPassthrough(row.bit);  // Show All -> equipped gear
                    } else {
                        g_staged.SetHide(row.bit);
                    }
                }
                Push();  // preview -> all hidden / all shown, dirty, undoable
                EditorStyle::PlayUISound("UIMenuCancel");
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(allHidden
                                      ? "$FR_ShowAllTip"_T
                                      : "$FR_HideAllTip"_T);
            }
            // Delete the ACTIVE outfit - a controller-reachable delete (the
            // tab's built-in X is mouse-hover-only, so a gamepad can never
            // reach it, OS field report). Opens the same confirm modal.
            if (canDelete) {
                FUCK::SameLine();
                if (FUCK::Button(trashLabel.c_str())) {
                    g_pendingDelete = snap.ActiveIndex();
                    // Window-scoped OpenPopup matches BeginPopupModal (OS-29);
                    // drawn above this, so the modal appears next frame.
                    FUCK::OpenPopup("delete_outfit");
                }
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip("$FR_DeleteOutfitTip"_T);
                }
            }
        }

        FUCK::Separator();
        // Everything below is font-relative: hardcoded pixel heights clipped
        // the footer/labels at larger fonts and resolutions.
        const float footerH   = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f;

        // ---- Showcases mode: browse + save, then out - the editor body
        // below never runs while the read-only browser is open. ------------
        if (g_showcasesOpen) {
            const auto presets = (g_showcaseSource == 1)
                                     ? AutoPresets::GetSingleton().Snapshot()
                                     : PresetStore::GetSingleton().Snapshot();
            if (g_showcaseSel >= static_cast<int>(presets.size())) {
                g_showcaseSel = -1;  // a rescan shrank the list
            }
            DrawShowcases(session, presets, footerH);

            FUCK::Separator();
            const bool haveSel = g_showcaseSel >= 0;
            const bool full    = snap.Count() >= kMaxOutfits;
            FUCK::BeginDisabled(!haveSel || full);
            if (FUCK::Button("$FR_SaveToOutfits"_T)) {
                const auto& p      = presets[static_cast<std::size_t>(g_showcaseSel)];
                int         newIdx = -1;
                session.WithLibrary([&](OutfitLibrary& lib) {
                    newIdx = lib.Create(p.name);
                    if (newIdx >= 0) {
                        *lib.At(static_cast<std::size_t>(newIdx)) = p.outfit;
                        lib.Activate(static_cast<std::size_t>(newIdx));
                    }
                });
                if (newIdx >= 0) {
                    // The copy is the active outfit now; leave Presets so the
                    // outfit tab bar returns and the forced selection lands.
                    g_showcasesOpen = false;
                    g_showcaseSel   = -1;
                    g_staged        = p.outfit;
                    g_dirty         = false;
                    g_forceSelect   = newIdx;
                    session.BeginStaging(g_staged);
                    g_history.Reset(g_staged);
                    EditorStyle::PlayUISound("UIMenuOK");
                }
            }
            FUCK::EndDisabled();
            FUCK::SameLine();
            FUCK::AlignTextToFramePadding();  // sit the status text on the button baseline (like Apply/Saved)
            if (full) {
                FUCK::Text("$FR_LibraryFull"_T, kMaxOutfits, kMaxOutfits);
            } else if (g_showcaseSel >= 0 && g_showcaseSel < static_cast<int>(presets.size())) {
                // Re-check g_showcaseSel LIVE (not the stale `haveSel` from before
                // the button handler): "Save to my outfits" sets g_showcaseSel =
                // -1, and reusing haveSel here indexed presets[(size_t)-1] =
                // presets[SIZE_MAX] - an out-of-bounds heap read that CTD'd
                // intermittently (heap-layout dependent) on every preset save.
                FUCK::Text("$FR_TryingOn"_T,
                            presets[static_cast<std::size_t>(g_showcaseSel)].name.c_str());
            } else {
                FUCK::TextDisabled("%s", "$FR_ClickPreset"_T);
            }
            DrawFootNote();

            FUCK::SameLine();
            const char* schint  = "Presets (top-right) to go back  ·  Esc / Start  Close";
            const float schintW = FUCK::CalcTextSize(schint).x;
            FUCK::SetCursorPosX(FUCK::GetWindowSize().x - schintW -
                                 OS::ui::WindowPadding().x);
            FUCK::TextDisabled("%s", schint);
            return;
        }

        // Shared search bar, full-width above BOTH panels (OS-20). The search
        // always drove both - it filters the style table AND gold-highlights
        // the slots that have matching styles - but sitting inside the style
        // panel it read as "search styles only". Hoisting it here makes the
        // "filters everything below" scope visually obvious.
        FUCK::AlignTextToFramePadding();
        FUCK::TextUnformatted(Icons::Utf8(Icons::kSearch).c_str());
        FUCK::SameLine();
        // Clear-X: when there's a query, shrink the field and add an X to clear it.
        const bool  hasSearch = g_search[0] != '\0';
        const float clearW =
            FUCK::CalcTextSize(Icons::Utf8(Icons::kTimes).c_str()).x + OS::ui::FramePadding().x * 2.0f;
        FUCK::SetNextItemWidth(hasSearch ? -(clearW + OS::ui::ItemSpacing().x) : -FLT_MIN);
        FUCK::InputText("##search", g_search, sizeof(g_search));
        if (hasSearch) {
            FUCK::SameLine();
            if (FUCK::Button(Icons::Utf8(Icons::kTimes).c_str())) {
                g_search[0] = '\0';
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_ClearSearch"_T);
            }
        }
        FUCK::Spacing();

        // Controller cheat-sheet (OS-36) - only while a gamepad is in use, so it
        // never clutters the mouse experience. The slot rows are one nav stop
        // each; the face buttons cover the icon/X a controller can't land on.
        if ((FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad)) {
            FUCK::TextDisabled("%s", "$FR_ControllerHints"_T);
            FUCK::Spacing();
        }

        const float slotsW = OS::ui::FontSize() * 13.0f;
        // NavFlattened: fold this child's gamepad/keyboard nav into the parent
        // so the D-pad crosses freely between the slot panel, the style browser,
        // the header and the footer - otherwise nav is trapped inside one child
        // window (user: "can't navigate the menu with a controller fully").
        FUCK::BeginChild("slots", ImVec2(slotsW, -footerH),
                          true);

        // Slot highlight while searching: which slots have matching styles.
        const bool searching = g_search[0] != '\0';
        if (searching &&
            (g_lastMatchQuery != g_search || g_lastMatchCollected != g_collectedOnly ||
             g_lastMatchArmorType != g_armorType || g_lastMatchFavorites != g_favoritesOnly)) {
            g_lastMatchQuery     = g_search;
            g_lastMatchCollected = g_collectedOnly;
            g_lastMatchArmorType = g_armorType;
            g_lastMatchFavorites = g_favoritesOnly;
            g_matchMask          = StyleCatalog::GetSingleton().MatchMask(
                g_search, g_collectedOnly, g_armorType, g_favoritesOnly);
        }

        const auto isRegular = [](std::uint32_t a_bit) {
            return ((kDefaultSlotMask >> a_bit) & 1u) != 0;
        };

        // Slot row (OS-31/32; OS-36/37 controller + alignment pass):
        //   [slot icon]  [label: state]  [X]
        //   • the SLOT ICON toggles the slot's visibility (hide <-> show).
        //   • the LABEL selectable picks the slot for the style browser.
        //   • the trailing "X" clears the slot back to your equipped gear,
        //     shown only when there's a style/hide to clear.
        //
        // OS-37 (alignment): the icon and the X are drawn by the SAME frameless
        // glyph-button helper - identical fixed-width box, vertical centering,
        // and hover fill - so the three elements read as one cohesive row (the
        // old X was a framed text button that stuck out). Both bookend boxes are
        // the uniform icon width, lining every label up (OS-31).
        //
        // OS-36 (controller): a row must be ONE nav stop, or gamepad "down" from
        // a symbol lands on the neighbouring row's label (three focusable items
        // per row = a confusing 2-D grid). The LABEL selectable is the sole nav
        // item; the icon and X carry FUCK::ItemFlags::kNoNav (still mouse-clickable,
        // just off the gamepad/keyboard nav grid). While a row is focused, face
        // buttons stand in for those two clicks: X = hide/show, Y = clear (A
        // selects, via the selectable's own activate). See the gamepad hint.
        //
        // A removed slot reads red (icon + label), matching the browser's unfit
        // red (OS-23) and winning over the search glow; search hits otherwise
        // glow gold; empty+unworn slots dim.
        const float slotRowH  = FUCK::GetFrameHeight();
        const float slotIconW = OS::ui::FontSize() * 1.6f;  // uniform bookend box (OS-31/37)

        // Frameless fixed-width glyph button: centered glyph + hover fill,
        // excluded from nav (mouse-only). Returns true on click. Shared by the
        // leading slot icon and the trailing clear-X so both look identical.
        const auto glyphButton = [&](const char* a_id, std::uint16_t a_glyph,
                                     const ImVec4& a_col, const char* a_tip) -> bool {
            const ImVec2 p0 = FUCK::GetCursorScreenPos();
            FUCK::PushItemFlag(FUCK::ItemFlags::kNoNav, true);
            const bool clicked = FUCK::InvisibleButton(a_id, ImVec2(slotIconW, slotRowH));
            FUCK::PopItemFlag();
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF("%s", a_tip);
                OS::ui::RectFilled(p0, ImVec2(p0.x + slotIconW, p0.y + slotRowH),
                                   OS::ui::Col(ImGuiCol_ButtonHovered), OS::ui::FrameRounding());
            }
            const std::string g   = Icons::Utf8(a_glyph);
            const ImVec2       gsz = FUCK::CalcTextSize(g.c_str());
            OS::ui::TextAt(ImVec2(p0.x + (slotIconW - gsz.x) * 0.5f,
                                  p0.y + (slotRowH - OS::ui::FontSize()) * 0.5f),
                           OS::ui::Col(a_col), g.c_str(), g.c_str() + g.size());
            return clicked;
        };

        const auto drawSlotRow = [&](const SlotRow& row) {
            const auto& entry    = g_staged.EntryFor(row.bit);
            const bool  hasEntry = entry.kind != SlotEntry::Kind::kPassthrough;
            const bool  hidden   = entry.kind == SlotEntry::Kind::kHide;

            std::string caption = std::string(FUCK::Translate(row.key)) + ": " + KindLabel(entry);
            if (entry.kind == SlotEntry::Kind::kStyle) {
                if (auto* armo = StyleRef::Resolve(entry.style)) {
                    const char* nm = armo->GetName();  // guard: std::string + null is UB
                    caption = std::string(FUCK::Translate(row.key)) + ": " + (nm ? nm : "");
                } else {
                    caption = std::string(FUCK::Translate(row.key)) + ": " + "$FR_MissingShort"_T;
                }
            }

            const bool matched = searching && ((g_matchMask >> row.bit) & 1u);
            const bool empty   = !hasEntry && !((g_wornMask >> row.bit) & 1u);

            const auto clearSlot = [&] {
                CrashGuard::ClearPreviewing();
                g_staged.SetPassthrough(row.bit);
                Push();
                EditorStyle::PlayUISound("UIMenuCancel");
            };
            const auto toggleHide = [&] {
                CrashGuard::ClearPreviewing();
                ToggleHideSlot(g_staged, g_preHide, row.bit);
                Push();
                EditorStyle::PlayUISound("UIMenuOK");
            };

            FUCK::PushID(static_cast<int>(row.bit));

            // --- slot icon (mouse: click toggles hide / show) ---
            const ImVec4 iconCol = hidden ? kUnfitText : FUCK::GetStyleColorVec4(ImGuiCol_Text);
            if (glyphButton("##vis", row.icon, iconCol,
                            hidden ? "Show this slot again (restore its piece)"
                                   : "Hide this slot (leave it bare)")) {
                toggleHide();
            }

            // --- label: the row's single nav stop; click/A selects the slot ---
            FUCK::SameLine(0, OS::ui::ItemSpacing().x);
            const float labelW = FUCK::GetContentRegionAvail().x -
                                 (hasEntry ? slotIconW + OS::ui::ItemSpacing().x : 0.0f);
            int pushed = 0;
            if (hidden) {
                FUCK::PushStyleColor(ImGuiCol_Text, kUnfitText);
                ++pushed;
            } else if (matched) {
                FUCK::PushStyleColor(ImGuiCol_Text, kGold);  // gold: search hit
                ++pushed;
            } else if (empty) {
                FUCK::PushStyleColor(ImGuiCol_Text, FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ++pushed;
            }
            if (FUCK::Selectable(caption.c_str(), g_selectedBit == row.bit, 0,
                                  ImVec2(labelW > 0.0f ? labelW : 0.0f, slotRowH))) {
                g_selectedBit = row.bit;
                // Controller UX (user): picking a slot on a gamepad jumps nav focus to the
                // style panel so you can choose a style straight away. Mouse users click
                // the style directly and don't want the jump, so gate on the gamepad.
                if (FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad) {
                    g_focusStyleList = true;
                }
            }
            const bool rowFocused = FUCK::IsItemFocused();  // gamepad/keyboard nav is here
            if (pushed) {
                FUCK::PopStyleColor(pushed);
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF("$FR_SlotTip"_T, FUCK::Translate(row.key), row.bit + 30);  // number for mod pages
            }

            // Gamepad shortcuts on the focused row (OS-36): X = hide/show,
            // Y = clear. They stand in for the NoNav icon/X a controller can't
            // land on. repeat=false so a held button fires once.
            if (rowFocused) {
                if (FUCK::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {  // X / Square
                    toggleHide();
                } else if (hasEntry && FUCK::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {  // Y
                    clearSlot();
                }
            }

            // --- X: clear the slot back to equipped gear (mouse) ---
            if (hasEntry) {
                FUCK::SameLine(0, OS::ui::ItemSpacing().x);
                if (glyphButton("##clr", Icons::kTimes,
                                FUCK::GetStyleColorVec4(ImGuiCol_Text),
                                "Clear to your equipped gear")) {
                    clearSlot();
                }
            }
            FUCK::PopID();
        };

        // Two accordions replace the old "All slots" toggle: the common set is
        // open by default; the rest live under "Extra Slots" (auto-opened when
        // the outfit already uses one, so an active slot is never buried).
        if (FUCK::CollapsingHeader("$FR_RegularSlots"_T, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& row : kSlots) {
                if (isRegular(row.bit)) {
                    drawSlotRow(row);
                }
            }
        }
        bool hasActiveExtra = false;
        for (const auto& row : kSlots) {
            if (!isRegular(row.bit) &&
                g_staged.EntryFor(row.bit).kind != SlotEntry::Kind::kPassthrough) {
                hasActiveExtra = true;
                break;
            }
        }
        if (FUCK::CollapsingHeader("$FR_ExtraSlots"_T,
                                    hasActiveExtra ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            for (const auto& row : kSlots) {
                if (!isRegular(row.bit)) {
                    drawSlotRow(row);
                }
            }
        }
        // The last row's clear-X glyph-button leaves the layout cursor moved by
        // OS::ui::TextAt(restore=true) with no item after it; when every slot has a
        // clear-X (e.g. after "Hide All") that dangling SetCursorPos is the last op
        // before EndChild and trips ImGui's "submit an item to grow boundaries" warning.
        // A zero-size Dummy consumes it. Safe here (unlike the in-row Dummy trap noted
        // in the FLICK handoff) because nothing follows it to be pushed onto the wrong line.
        FUCK::Dummy(ImVec2(0.0f, 0.0f));
        FUCK::EndChild();

        // ---- style browser --------------------------------------------------
        FUCK::SameLine();
        FUCK::BeginChild("styles", ImVec2(0, -footerH),
                          true);  // one nav space (see "slots")
        // Denser browser text (user). SetWindowFontScale didn't visibly resize glyphs on
        // this FUCK build, so per Fuzzles the density lever is an explicit smaller font:
        // push the regular font at g_uiScale x the current line height for the whole
        // browser (filters + table). This makes the "UI size" slider actually change the
        // table text density (default 0.9). Harmless no-op if FUCK::PushFont(size) has no
        // effect (flag: still large -> a Fuzzles Q). Popped just before EndChild.
        auto* styleFont = FUCK::GetFont(FUCK::Font::kRegular);
        if (styleFont) {
            FUCK::PushFont(styleFont, OS::ui::FontSize() * g_uiScale);
        }
        // (Search moved to the shared bar above both panels; "Real gear"/Clear
        // moved to a per-slot button in the slots panel.)
        if (FUCK::Checkbox("$FR_Collected"_T, &g_collectedOnly, false, false)) {
            EditorStyle::PlayUISound("UIMenuFocus");
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_CollectedTip"_T);
        }
        // Favorites filter (OS-22): only styles you have starred. Composes with
        // Collected and the armor-type filter. Session-local (off each open).
        FUCK::SameLine();
        if (FUCK::Checkbox("$FR_Favorites"_T, &g_favoritesOnly, false, false)) {
            EditorStyle::PlayUISound("UIMenuFocus");
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_FavoritesTip"_T);
        }
        FUCK::SameLine();
        if (FUCK::Checkbox("$FR_HideUnfit"_T, &g_hideUnfit, false, false)) {
            EditorStyle::PlayUISound("UIMenuFocus");
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_HideUnfitTip"_T);
        }
        // (New-mod styles aren't filtered - they sort to the top with a gold
        // NEW badge; OS-26. Unfit styles are always shown, tinted red.)
        // (Random button moved onto the armor-type row below - it was being cut off
        //  crammed onto the 3-checkbox filters row, user.)
        // Armor-type filter (Skyrim has three classes; no "medium") on its own
        // row - three filter checkboxes already fill the first row.
        const char* kTypeLabels[] = { "$FR_TypeAll"_T, "$FR_TypeLight"_T, "$FR_TypeHeavy"_T, "$FR_TypeClothing"_T };
        int                typeCombo = g_armorType + 1;  // -1..2 -> 0..3
        FUCK::SetNextItemWidth(OS::ui::FontSize() * 6.5f);
        if (FUCK::Combo("##armortype", &typeCombo, kTypeLabels, IM_ARRAYSIZE(kTypeLabels))) {
            g_armorType = typeCombo - 1;
            EditorStyle::PlayUISound("UIMenuFocus");
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_TypeTip"_T);
        }
        // Random (user): rolls a random fitting style for the SELECTED slot. Right-
        // aligned on the armor-type row (it was cut off crammed onto the 3-checkbox
        // filters row); obeys the same filters (collected / favorites / armor type).
        {
            const std::string randLabel = Icons::Utf8(Icons::kDice) + " " + "$FR_Random"_T;
            const float        randW     = FUCK::CalcTextSize(randLabel.c_str()).x +
                                OS::ui::FramePadding().x * 2.0f;
            FUCK::SameLine();
            FUCK::SetCursorPosX(std::max(FUCK::GetCursorPos().x,
                                          (FUCK::GetCursorPos().x + FUCK::GetContentRegionAvail().x) - randW));
            if (FUCK::Button(randLabel.c_str())) {
                static std::uint32_t s_rng = 0;
                if (s_rng == 0) {
                    s_rng = ::GetTickCount() | 1u;
                }
                s_rng = s_rng * 1664525u + 1013904223u;  // LCG - plenty for a dice roll
                const auto all = StyleCatalog::GetSingleton().Query(
                    g_selectedBit, "", g_collectedOnly, g_armorType, g_favoritesOnly);
                std::vector<const StyleItem*> ok;
                for (const auto* s : all) {
                    if (s->fitsBody && s->fitReason != FitReason::kCrashed) {
                        ok.push_back(s);
                    }
                }
                if (!ok.empty()) {
                    CrashGuard::ClearPreviewing();
                    g_staged.SetStyle(g_selectedBit, ok[s_rng % ok.size()]->key);
                    Push();
                    EditorStyle::PlayUISound("UIMenuOK");
                } else {
                    g_footNote      = "$FR_NoFitting"_T;
                    g_footNoteUntil = FUCK::GetTime() + 4.0;
                    EditorStyle::PlayUISound("UIMenuCancel");
                }
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_RandomTip"_T);
            }
        }
        // (Column show/hide moved to the header settings gear - OS-30.)
        FUCK::Separator();

        auto results = StyleCatalog::GetSingleton().Query(g_selectedBit, g_search, g_collectedOnly,
                                                          g_armorType, g_favoritesOnly);
        if (g_hideUnfit) {  // hide body-unfit (red) rows; crashers stay (blocked on click)
            std::erase_if(results, [](const StyleItem* s) { return !s->fitsBody; });
        }
        FUCK::TextDisabled(g_collectedOnly ? "$FR_CountCollected"_T : "$FR_CountStyle"_T,
                            results.size(), results.size() == 1 ? "" : "s");
        const auto searchLen = std::strlen(g_search);

        const auto classLabel = [](std::uint8_t a_t) -> const char* {
            switch (a_t) {
                case 0:  return "$FR_TypeLight"_T;
                case 1:  return "$FR_TypeHeavy"_T;
                default: return "$FR_TypeClothing"_T;
            }
        };

        // Hover-preview: the row hovered THIS frame (skipping crashers) drives
        // a transient preview once the mouse rests on it (resolved after the
        // table). Declared out here so it survives past EndTable.
        StyleRefKey frameHoverKey;
        bool        frameAnyHover = false;

        // The style list is a sortable table (SkyUI-style): click a header to
        // sort by that column, click again to flip the arrow. Rows span both
        // columns and stay selectable/clickable; unfit rows tint red and the
        // search substring glows gold inside the Name cell.
        enum Col : ImGuiID { kColName = 0, kColClass = 1, kColSource = 2 };
        // No SortTristate: the header toggles only ascending<->descending (a
        // two-state up/down arrow), never an unsorted state.
        // FUCK::TableFlags lacks ScrollY/PadOuterX (the parent child scrolls) and
        // TableSetupScrollFreeze (so the header still scrolls with the list -
        // requested from Fuzzles). TableSetColumnEnabled also has no FUCK equivalent,
        // so the gear's Class/Plugin show-hide is honored by rebuilding the table with
        // only the enabled columns (the count + which TableSetupColumns we emit).
        // kHideable is dropped so the gear checkboxes are the single source of truth
        // (no competing right-click-header hide); sorting keys off ColumnUserID, so a
        // hidden column just drops out of the sort specs.
        const int  nCols       = 1 + (g_showClass ? 1 : 0) + (g_showSource ? 1 : 0);
        const auto kTableFlags = FUCK::TableFlags::kSortable | FUCK::TableFlags::kRowBg |
                                 FUCK::TableFlags::kBordersInnerH |
                                 FUCK::TableFlags::kSizingStretchProp;
        if (FUCK::BeginTable("styles_tbl", nCols, kTableFlags, ImVec2(0.0f, 0.0f))) {
            FUCK::TableSetupColumn("$FR_Name"_T,
                                    FUCK::TableColumnFlags::kWidthStretch |
                                        FUCK::TableColumnFlags::kDefaultSort |
                                        FUCK::TableColumnFlags::kNoHide,
                                    0.56f, kColName);
            if (g_showClass) {
                FUCK::TableSetupColumn("$FR_ColClass"_T, FUCK::TableColumnFlags::kWidthStretch, 0.16f,
                                        kColClass);
            }
            if (g_showSource) {
                FUCK::TableSetupColumn("$FR_ColPlugin"_T, FUCK::TableColumnFlags::kWidthStretch, 0.28f,
                                        kColSource);
            }
            FUCK::TableHeadersRow();

            // Query() already returns Name-ascending; re-sort only when the
            // user picks a different column/direction.
            if (auto* specs = FUCK::GetTableSortSpecs();
                specs && specs->SpecsCount > 0 && specs->Specs) {
                const auto& s   = specs->Specs[0];
                const bool  asc = s.SortDirection != ImGuiSortDirection_Descending;
                std::ranges::sort(results, [&](const StyleItem* a, const StyleItem* b) {
                    if (a->isRecent != b->isRecent) {
                        return a->isRecent;  // NEW-mod styles stay pinned to the top (OS-26)
                    }
                    int cmp = 0;
                    if (s.ColumnUserID == kColClass) {
                        cmp = static_cast<int>(a->armorType) - static_cast<int>(b->armorType);
                    } else if (s.ColumnUserID == kColSource) {
                        cmp = a->source.compare(b->source);
                    }
                    if (cmp == 0) {
                        cmp = a->name.compare(b->name);  // Name is the tiebreak everywhere
                    }
                    return asc ? cmp < 0 : cmp > 0;
                });
            }

            // Controller (user): after a gamepad slot pick, land nav on the first style
            // row so the right panel takes focus. One-shot - consume the flag here.
            bool focusFirstStyle = g_focusStyleList;
            g_focusStyleList     = false;
            for (const auto* item : results) {
                const bool selected = g_staged.EntryFor(g_selectedBit).style == item->key;
                const bool unfit    = !item->fitsBody;
                FUCK::PushID(static_cast<int>(item->armo->GetFormID()));
                FUCK::TableNextRow();
                FUCK::TableNextColumn();  // Name column (first)
                if (focusFirstStyle) {
                    FUCK::SetKeyboardFocusHere();  // move controller nav onto the first style row
                    focusFirstStyle = false;
                }

                // A background selectable spanning the row does the clicking;
                // the favorite star and the Name text are drawn OVER it. The
                // star is handled by a MANUAL hit-test on its rect - the earlier
                // SetItemAllowOverlap + InvisibleButton approach did not register
                // the click in-game - so a click landing on the star toggles the
                // favorite and is suppressed from selecting the row. The name is
                // hand-drawn so the search match can glow gold and unfit rows
                // can tint red.
                bool clicked =
                    FUCK::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns);
                const bool   hovered = FUCK::IsItemHovered();
                // Controller: X / Square on the focused style row toggles favorite
                // (the mouse uses the star hit-rect, which a controller can't reach).
                if (FUCK::IsItemFocused() &&
                    FUCK::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
                    Favorites::Toggle(item->key);
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                const ImVec2 rmin    = FUCK::GetItemRectMin();
                const ImVec2 rmax    = FUCK::GetItemRectMax();
                if (hovered && item->fitReason != FitReason::kCrashed) {
                    frameHoverKey = item->key;  // crashers are never hover-previewed
                    frameAnyHover = true;
                }

                // Favorite star (OS-22): a hit-zone at the left of the Name
                // cell. Gold = starred; grey otherwise (brightening on hover).
                const float starW    = OS::ui::FontSize() + OS::ui::ItemSpacing().x;
                const bool  fav      = Favorites::IsFavorite(item->key);
                const ImVec2 mp       = FUCK::GetMousePos();
                const bool   overStar = hovered && mp.x >= rmin.x && mp.x < rmin.x + starW &&
                                        mp.y >= rmin.y && mp.y < rmax.y;
                if (clicked && overStar) {  // the click was on the star: toggle, don't apply
                    Favorites::Toggle(item->key);
                    EditorStyle::PlayUISound("UIMenuFocus");
                    clicked = false;
                }
                if (overStar) {
                    FUCK::SetTooltip(fav ? "$FR_Unfavorite"_T : "$FR_Favorite"_T);
                }
                {
                    const std::string star = Icons::Utf8(Icons::kStar);
                    const ImVec2      ssz  = FUCK::CalcTextSize(star.c_str());
                    const float       sx   = rmin.x + (starW - ssz.x) * 0.5f;
                    const float       sy   = rmin.y + (rmax.y - rmin.y - OS::ui::FontSize()) * 0.5f;
                    const ImU32       scol =
                        fav ? OS::ui::Col(kGold)  // gold/yellow when favorited
                            : OS::ui::Col(overStar ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                    OS::ui::TextAt(ImVec2(sx, sy), scol, star.c_str(), star.c_str() + star.size(), false);
                }
                {
                    float       x  = rmin.x + starW;  // the name begins past the star
                    const float y  = rmin.y + (rmax.y - rmin.y - OS::ui::FontSize()) * 0.5f;
                    const auto  drawSeg = [&](std::string_view a_seg, ImU32 a_col) {
                        if (!a_seg.empty()) {
                            OS::ui::TextAt(ImVec2(x, y), a_col, a_seg.data(),
                                           a_seg.data() + a_seg.size(), false);
                            x += FUCK::CalcTextSize(a_seg.data(), a_seg.data() + a_seg.size()).x;
                        }
                    };
                    const auto colText = unfit ? OS::ui::Col(kUnfitText)
                                               : OS::ui::Col(ImGuiCol_Text);
                    const auto pos = searching ? FindCI(item->name, g_search)
                                               : std::string_view::npos;
                    if (pos == std::string_view::npos) {
                        drawSeg(item->name, colText);
                    } else {
                        const std::string_view n{ item->name };
                        drawSeg(n.substr(0, pos), colText);
                        drawSeg(n.substr(pos, searchLen),
                                OS::ui::Col(kGold));  // gold: search hit
                        drawSeg(n.substr(pos + searchLen), colText);
                    }
                    // Gold "NEW" badge for styles from newly-added mods (OS-26).
                    if (item->isRecent) {
                        drawSeg("$FR_NewBadge"_T, OS::ui::Col(kGold));  // gold
                    }
                }

                const ImVec4 dimCol = unfit ? kUnfitText
                                            : FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled);
                if (g_showClass && FUCK::TableNextColumn()) {
                    FUCK::TextColored(dimCol, "%s", classLabel(item->armorType));
                }
                if (g_showSource && FUCK::TableNextColumn()) {
                    FUCK::TextColored(dimCol, "%s", item->source.c_str());
                }

                if (clicked) {  // star clicks were already suppressed above
                    if (item->fitReason == FitReason::kCrashed) {
                        // Empirically crashes the skinning pass - block the
                        // re-preview (delete crashed_styles.txt to re-enable).
                        RE::DebugNotification(
                            "This style crashed the preview before and is blocked.");
                        EditorStyle::PlayUISound("UIMenuCancel");
                    } else {
                        // Blame THIS style if the rebuild it triggers crashes
                        // the engine's skinning (CrashGuard marks it pending
                        // around the kick; a survived crash flags it next
                        // launch).
                        CrashGuard::SetPreviewing(item->key);
                        g_staged.SetStyle(g_selectedBit, item->key);
                        Push();
                        // Commit ends any hover-preview so leaving the row does
                        // not revert the click.
                        g_hoverKey     = StyleRefKey{};
                        g_hoverPending = StyleRefKey{};
                        EditorStyle::PlayUISound("UIMenuOK");
                        spdlog::debug("EditorUI: staged style '{}' ({}) fit={}.", item->name,
                                      item->source, item->fitsBody);
                    }
                }
                if (hovered && !overStar) {  // the star shows its own tooltip
                    if (unfit) {
                        OS::ui::SetTooltipF("$FR_MayNotFit"_T, item->source.c_str(),
                                          StyleCatalog::FitReasonText(item->fitReason).c_str());
                    } else {
                        OS::ui::SetTooltipF("%s", item->source.c_str());
                    }
                }
                FUCK::PopID();
            }
            FUCK::EndTable();
        }

        // Hover-preview resolve: rest the mouse on a row (~0.18s) and it
        // previews on the character transiently - over the render channel only,
        // never touching the g_staged edit buffer - so moving away restores the
        // real look and a click commits it. Skips the already-selected style and
        // crashers (filtered above).
        if (g_hoverPreview) {
            const double now = FUCK::GetTime();
            const auto&  cur = g_staged.EntryFor(g_selectedBit);
            const bool   alreadyChosen =
                cur.kind == SlotEntry::Kind::kStyle && cur.style == frameHoverKey;
            if (frameAnyHover && !alreadyChosen) {
                if (!(frameHoverKey == g_hoverKey)) {
                    if (!(frameHoverKey == g_hoverPending)) {
                        g_hoverPending      = frameHoverKey;
                        g_hoverPendingSince = now;
                    } else if (now - g_hoverPendingSince >= 0.18) {
                        Outfit tmp = g_staged;  // preview a variant WITHOUT editing the buffer
                        tmp.SetStyle(g_selectedBit, frameHoverKey);
                        CrashGuard::SetPreviewing(frameHoverKey);
                        session.UpdateStaging(tmp);
                        g_hoverKey = frameHoverKey;
                    }
                }
            } else {
                g_hoverPending = StyleRefKey{};
                if (!g_hoverKey.Empty()) {  // left the rows - restore the real edit buffer
                    CrashGuard::ClearPreviewing();
                    session.UpdateStaging(g_staged);
                    g_hoverKey = StyleRefKey{};
                }
            }
        }
        if (styleFont) {
            FUCK::PopFont();
        }
        FUCK::EndChild();

        // Keyboard undo/redo (OS-21): Ctrl+Z = undo, Ctrl+Y or Ctrl+Shift+Z =
        // redo. Gated on !WantTextInput so an active search/rename field keeps
        // ImGui's own text-edit undo. Resolved once per frame, outside the
        // panels, so a button and a shortcut can't both fire on one edit.
        {
            const bool ctrl  = FUCK::IsModifierPressed(FUCK::Modifier::kCtrl);
            const bool shift = FUCK::IsModifierPressed(FUCK::Modifier::kShift);
            if (ctrl && !FUCK::IsAnyItemActive()) {
                const bool zPressed = FUCK::IsKeyPressed(ImGuiKey_Z, false);
                const bool yPressed = FUCK::IsKeyPressed(ImGuiKey_Y, false);
                if (zPressed && !shift && g_history.CanUndo()) {
                    ApplyHistory(session, g_history.Undo());
                } else if ((yPressed || (zPressed && shift)) && g_history.CanRedo()) {
                    ApplyHistory(session, g_history.Redo());
                }
            }
        }

        // RB / LB cycle outfits on a controller (the shoulder buttons are fed to
        // ImGui but nothing else consumes them). Once per frame, outside the
        // panels; gated on !WantTextInput so a rename field keeps its keys. The
        // switch mirrors a tab click (discards unsaved staging the same way).
        {
            const int count = static_cast<int>(snap.Count());
            if (count > 1 && !FUCK::IsAnyItemActive()) {
                const bool next = FUCK::IsKeyPressed(ImGuiKey_GamepadR1, false);
                const bool prev = FUCK::IsKeyPressed(ImGuiKey_GamepadL1, false);
                if (next || prev) {
                    const int base   = snap.ActiveIndex() >= 0 ? snap.ActiveIndex() : 0;
                    const int newIdx = next ? (base + 1) % count : (base - 1 + count) % count;
                    session.WithLibrary([&](OutfitLibrary& lib) {
                        lib.Activate(static_cast<std::size_t>(newIdx));
                    });
                    if (const auto* o = snap.At(static_cast<std::size_t>(newIdx))) {
                        g_staged = *o;
                    }
                    session.BeginStaging(g_staged);
                    g_dirty       = false;
                    g_history.Reset(g_staged);
                    g_forceSelect = newIdx;  // the tab bar follows next frame
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
            }
        }

        // ---- footer ---------------------------------------------------------
        FUCK::Separator();
        // Lore mode charges gold per CHANGED slot on Apply (preview stays
        // free - the ESO model). Diff the staged outfit against the committed
        // one; without an active outfit everything staged counts as changed.
        std::uint32_t changed = 0;
        {
            static const Outfit kEmpty;
            const auto*         committed = snap.Active();
            const auto&         base      = committed ? *committed : kEmpty;
            changed                       = ChangedSlotCount(base, g_staged);
        }
        // Editing a slot back to its committed value undoes the edit: no
        // changed slots means nothing to apply or revert. Without this the
        // dirty flag stays stuck true after a Hide→Show round trip, leaving
        // Apply enabled at "0 gold" and the status reading "Unsaved changes".
        g_dirty = g_dirty && changed > 0;

        const auto          costPer    = Settings::GetSingleton().goldPerSlot;
        // Gold cost is its own toggle now (was LoreModule::Active()); it needs
        // no lore ESP - the charge math never touches the Seamstone.
        const bool          charging   = Settings::GetSingleton().useGold && costPer > 0;
        const std::uint64_t cost       = charging ? std::uint64_t(changed) * costPer : 0;
        const bool          cantAfford = charging && cost > g_gold;

        // Apply's label is just "Apply" now; the gold cost shows beside the
        // button in colour (A1), not baked into the label.
        // Undo / redo (OS-21), in the footer action bar next to Apply (moved
        // here from the slots panel). Keyboard Ctrl+Z / Ctrl+Y also work.
        FUCK::BeginDisabled(!g_history.CanUndo());
        if (FUCK::Button(Icons::Utf8(Icons::kUndo).c_str())) {
            ApplyHistory(session, g_history.Undo());
        }
        FUCK::EndDisabled();
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_UndoTip"_T);
        }
        FUCK::SameLine();
        FUCK::BeginDisabled(!g_history.CanRedo());
        if (FUCK::Button(Icons::Utf8(Icons::kRedo).c_str())) {
            ApplyHistory(session, g_history.Redo());
        }
        FUCK::EndDisabled();
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_RedoTip"_T);
        }
        FUCK::SameLine();

        FUCK::BeginDisabled(!g_dirty || cantAfford);
        if (FUCK::Button("$FR_Apply"_T)) {
            EnsureActiveOutfit(session);
            session.CommitStaging();
            session.BeginStaging(g_staged);  // keep editing the same outfit
            g_dirty = false;
            if (cost > 0) {
                g_gold -= cost;
                const auto amount = static_cast<std::int32_t>(
                    std::min<std::uint64_t>(cost, INT32_MAX));
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([amount] {  // inventory mutation: main thread
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);
                        if (player && gold) {
                            player->RemoveItem(gold, amount, RE::ITEM_REMOVE_REASON::kRemove,
                                               nullptr, nullptr);
                        }
                    });
                }
            }
            EditorStyle::PlayUISound("UIMenuOK");
        }
        FUCK::EndDisabled();

        // Gold cost beside the button, not baked into its label (A1): a coins
        // glyph + the amount in journal-gold. Shown whenever there is a charge,
        // including while Apply is disabled for "Not enough gold", so the price
        // stays visible. Gold is a deliberate highlight (theme audit A8: OK).
        if (cost > 0) {
            FUCK::SameLine();
            FUCK::AlignTextToFramePadding();
            FUCK::TextColored(kGold, "%s %llu", Icons::Utf8(Icons::kCoins).c_str(),
                              static_cast<unsigned long long>(cost));
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF("$FR_CostTip"_T,
                                    static_cast<unsigned long long>(cost), changed,
                                    changed == 1 ? "" : "s");
            }
        }
        DrawFootNote();

        // Right-aligned [status] [Close] cluster (A2). The unsaved / not-enough
        // state sits beside Close in red so it reads as an alert; "Saved" is muted
        // and calm. Reserve the status width in the right-align math too, or the
        // label would overrun Close (window width minus the whole cluster). Close
        // is the discoverable exit for mouse AND controller (navigate to it, press
        // A); on the gamepad B is FUCK panel-back nav, so the key is Esc / Start.
        const char* const statusText = !g_dirty     ? "$FR_Saved"_T
                                        : cantAfford ? "$FR_NoGold"_T
                                                     : "$FR_Unsaved"_T;
        const ImVec4 statusCol = g_dirty ? kUnfitText
                                         : FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const char*  closeLabel = "$FR_Close"_T;
        const float  statusW    = FUCK::CalcTextSize(statusText).x;
        const float  closeW     = FUCK::CalcTextSize(closeLabel).x +
                                  OS::ui::FramePadding().x * 2.0f;
        const float  clusterW   = statusW + OS::ui::ItemSpacing().x + closeW;
        FUCK::SameLine();
        FUCK::SetCursorPosX(FUCK::GetWindowSize().x - clusterW -
                            OS::ui::WindowPadding().x);
        FUCK::AlignTextToFramePadding();
        FUCK::TextColored(statusCol, "%s", statusText);
        FUCK::SameLine();
        if (FUCK::Button(closeLabel)) {
            EditorWindow::RequestClose();
        }
    }

}  // namespace OS::EditorUI
