#include "EditorUI.h"

#include "AutoPresets.h"
#include "Collection.h"
#include "CrashGuard.h"
#include "EditTargetLabel.h"
#include "EditorGate.h"
#include "EditorStyle.h"
#include "Favorites.h"
#include "FooterNotice.h"
#include "Icons.h"
#include "EditorWindow.h"
#include "LoreModule.h"
#include "NpcAssignments.h"
#include "ObodyApi.h"
#include "OutfitSession.h"
#include "OutfitTabAdd.h"
#include "OutfitTabs.h"
#include "PresetPreviewPolicy.h"
#include "PresetStore.h"
#include "Settings.h"
#include "ShowcaseTabs.h"
#include "SlotMask.h"
#include "StyleCatalog.h"
#include "StyleRef.h"
#include "WeaponSlots.h"

#include "FuckCompat.h"  // FUCK:: wrapper + OS::ui:: bridges for the ImGui deltas

#include <imgui.h>
#include <imgui_internal.h>  // PushItemFlag(FUCK::ItemFlags::kNoNav) - one nav stop per slot row
#include <imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

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
        // is an armor slot presented under Weapons & Shields; its hide control
        // is disabled separately. Anything else a setup must not touch belongs
        // in the INI blocklist.
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
            { 9, Icons::kShield, "Shield", "$FR_Slot_Shield" },
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
        // Undo/redo timeline over the staged outfit (OS-21). Reset to the
        // opened/switched outfit; a snapshot is recorded on every real edit
        // (through Push), never on transient hover-preview.
        EditHistory   g_history;
        std::uint32_t g_selectedBit   = kBitBody;
        // The weapon-dimension counterpart to g_selectedBit (Task 8). The two
        // are MUTUALLY EXCLUSIVE: selecting a weapon row sets this without
        // touching g_selectedBit, selecting an armor row clears this back to
        // nullopt. Every browser consumer (Query, Random, click, hover-
        // preview, row highlight) checks this FIRST and only falls back to
        // g_selectedBit when it is empty - a weapon selection takes
        // precedence, per the spec.
        std::optional<WeaponClass> g_selectedWeapon;
        WeaponHand                 g_selectedWeaponHand = WeaponHand::Both;
        bool          g_focusStyleList = false;  // controller: jump nav to the style panel after a slot pick (user)
        char          g_search[128]   = {};
        bool          g_dirty         = false;
        bool          g_favoritesOnly = false;  // browser filter: only starred looks (session-local)
        bool          g_hideUnfit     = true;   // browser filter: hide body-unfit (red) rows (session-local, default on)
        int           g_armorType     = -1;     // browser filter: -1 any, 0 light, 1 heavy, 2 clothing
        float         g_uiScale       = Settings::kUiScaleDefault;  // complete editor
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
        int           g_forceSelect = OutfitTabs::kNoForcedSelection;
        // The name field is being typed in RIGHT NOW. The tab label is the
        // outfit name, so every keystroke changes the tab's ImGui id and the bar
        // would lose its selection (and fire the tab-switch side effect on some
        // other index) mid-rename. While this is set the bar pins the active tab
        // and ignores switches. See the tab loop.
        bool          g_renaming           = false;
        bool          g_justOpened         = false;  // suppress tab-activate on the open frame
        std::uint64_t g_gold               = 0;   // player gold at open (input is modal)

        // OBody preset names for the CURRENT target's sex, cached at open and
        // on every target switch.
        //
        // ⚠ NEVER FETCHED FROM Draw(). Two reasons, either one sufficient:
        // ObodyApi::PresetNames allocates a string per preset (hundreds, on a
        // big BodySlide setup) and Draw runs every rendered frame; and the API
        // is only safe to touch while OBody is ready, which it stops being
        // around saves. Snapshotting at open matches how g_wornMask and g_gold
        // are handled, and editor input is modal so it cannot go stale.
        std::vector<std::string> g_bodyPresets;

        // ---- preset ownership (lore-friendly gating) ------------------------
        // A Discovered preset is generated from EVERY installed style, with no
        // reference to the collection - so in lore-friendly mode the Presets
        // tab handed out complete outfits assembled from mods the player had
        // never encountered, straight past the earn-it loop that the whole
        // mode is built on. These two put presets back inside that economy
        // without removing the tab (user decision, 2026-07-22).
        struct PresetOwnership {
            int owned{ 0 };
            int total{ 0 };
            [[nodiscard]] bool Complete() const { return owned == total; }
        };

        [[nodiscard]] PresetOwnership OwnedPieces(const Outfit& a_outfit) {
            PresetOwnership   r;
            const auto&       coll = Collection::GetSingleton();
            const auto        tally = [&](const StyleRefKey& a_key) {
                ++r.total;
                if (auto* f = StyleRef::ResolveAny(a_key); f && coll.Knows(f->GetFormID())) {
                    ++r.owned;
                }
            };
            a_outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) { tally(a_key); });
            a_outfit.ForEachWeaponStyle([&](WeaponClass, const StyleRefKey& a_key) { tally(a_key); });
            return r;
        }

        // ---- "Editing:" target dimension (Task 8) --------------------------
        // Who the editor is dressing. The default (npc == nullopt) is the
        // PLAYER, and a player-only session never touches the dropdown, so the
        // whole editor behaves EXACTLY as before. An NPC target threads through
        // staging (BeginStaging(handle)), the per-target fit cache
        // (RefreshFitFor), the worn-mask snapshot (target actor's GetWornArmor)
        // and Apply (UpsertNpcLibrary + RequestRefreshActor). `loaded == false`
        // is an "(away)" persisted assignee - selectable READ-ONLY (view / clear
        // the assignment), since an unloaded actor can't be previewed or kicked.
        struct EditTarget {
            std::optional<NpcKey> npc;                       // nullopt == the player
            RE::ActorHandle       handle;                    // player / NPC handle; empty for "(away)"
            std::string           label;                     // dropdown display text (may carry " (away)")
            std::string           displayName;               // name only, no "(away)" suffix (footer hints)
            bool                  loaded{ true };            // false == "(away)" (not currently high-process)
            RE::TESRace*          race{ nullptr };           // for RefreshFitFor on switch
            int                   sexIdx{ RE::SEXES::kMale };// for RefreshFitFor on switch
            std::uint32_t         baseFormID{ 0 };
            Outfit                equippedPreview;           // live worn armor, mannequin only
        };

        EditTarget g_target;         // current target (default = player)
        int        g_targetIndex = 0;  // index into g_targetList of g_target

        // The NPC target's OWN working library (the player's lives in
        // OutfitSession). Loaded on switch from SnapshotNpcAssignments; the tab
        // bar / rename / add / delete operate on THIS while an NPC is targeted,
        // and Apply upserts the whole thing. Unused while targeting the player.
        OutfitLibrary g_targetLibrary;

        // The dropdown roster, built on the MAIN thread at open (ForEachHighActor
        // must not run on the FUCK present thread). Immutable once published;
        // Draw (render thread) atomic-loads it and never mutates it - so a
        // consumer holding the shared_ptr for a frame is race-free. Editor input
        // is modal, so the at-open roster stays complete for the session (no new
        // follower can be recruited while the menu is up).
        std::atomic<std::shared_ptr<const std::vector<EditTarget>>> g_targetList;

        // The per-target fit cache (StyleCatalog::fitsBody) is re-evaluated on
        // the MAIN thread when the target switches - it MUST finish before the
        // render thread draws catalog rows again (the same fitsBody-unsynced
        // contract EnsureFitCurrent documents). While false, Draw skips the
        // style browser (the only fitsBody reader) and shows a brief "Loading"
        // placeholder, so no row is ever drawn against a half-rebuilt cache.
        // True at open (SetOpen already ran EnsureFitCurrent for the player).
        std::atomic<bool> g_fitReady{ true };

        // Showcases (read-only preset browser). While the tab is open the
        // staging channel shows the clicked preset; g_staged (the edit
        // buffer) is untouched and is re-staged on the way out.
        bool        g_showcasesOpen  = false;
        int         g_pendingDelete  = -1;   // outfit index whose tab-X delete is being confirmed
        int         g_showcaseSel    = -1;   // index into this frame's Snapshot()
        int         g_showcaseHover  = -1;   // transiently previewed row, never a saved selection
        int         g_showcaseHoverPending = -1;
        double      g_showcaseHoverSince = 0.0;
        ShowcaseTabs::State g_showcaseTabs;  // defaults to Discovered (OS-56)
        char        g_showcaseSearch[128] = {};
        std::string g_footNote;              // transient footer note (e.g. export path)
        double      g_footNoteUntil = 0.0;
        std::string g_pendingExportDelete;   // plain filename from the Exported source

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
            return nullptr;  // unknown/reserved biped bit
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

        // ---- target routing (Task 8) ---------------------------------------
        // The editor's whole tab/rename/add/delete/footer machinery reads and
        // writes "the current target's library". For the player that IS the
        // session library (persisted to outfits.json); for an NPC it is the
        // editor-held g_targetLibrary (per-save co-save state, upserted on Apply
        // and on structural edits). These three routers keep one draw path for
        // both, so the player-only session is byte-for-byte the old behavior.

        [[nodiscard]] bool TargetIsPlayer() { return !g_target.npc.has_value(); }

        [[nodiscard]] bool PlayerMannequinHasWeaponClass(WeaponClass a_class) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* biped  = player ? player->GetCurrentBiped().get() : nullptr;
            const auto slot = BipedSlotForClass(a_class);
            if (!biped || slot >= static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal)) {
                return false;
            }
            return ClassOfWeaponForm(biped->objects[slot].item) == a_class;
        }

        // A consistent copy of the current target's library for this frame's draw.
        [[nodiscard]] OutfitLibrary CurrentLibrarySnapshot(OutfitSession& a_session) {
            return TargetIsPlayer() ? a_session.SnapshotLibrary() : g_targetLibrary;
        }

        // Mutate the current target's library. Player: through the session's
        // locked WithLibrary (which also queues the outfits.json save). NPC:
        // directly on g_targetLibrary (render-thread-owned) then upsert the whole
        // library into the session's co-save map - NEVER WithLibrary (hard rule:
        // assignment mutations must not write outfits.json). UpsertNpcLibrary
        // rebuilds the render snapshot; it deliberately does NOT kick a visual
        // refresh (structural edits like a rename don't change the look, and the
        // live look is driven by staging until Apply).
        template <class Fn>
        void WithCurrentLibrary(OutfitSession& a_session, Fn&& a_fn) {
            if (TargetIsPlayer()) {
                a_session.WithLibrary(std::forward<Fn>(a_fn));
            } else {
                std::forward<Fn>(a_fn)(g_targetLibrary);
                if (g_target.npc) {
                    a_session.UpsertNpcLibrary(*g_target.npc, g_targetLibrary);
                }
            }
        }

        // Begin staging on the current target. NPC staging also supplies the
        // player-only inventory viewport's transient mannequin.
        void BeginStagingCurrent(OutfitSession& a_session, const Outfit& a_from) {
            if (TargetIsPlayer()) {
                a_session.BeginStaging(a_from);
            } else {
                a_session.BeginStaging(
                    g_target.handle, a_from, g_target.equippedPreview);
            }
        }

        // Capture a loaded actor's real worn armor on the main thread. One
        // style entry per unique ARMO is enough because ApplyArmorAddon stages
        // that armor's complete slot coverage. This Outfit is render-only.
        [[nodiscard]] Outfit SnapshotEquippedArmor(RE::Actor* a_actor) {
            Outfit out;
            if (!a_actor) {
                return out;
            }
            std::unordered_set<RE::TESObjectARMO*> seen;
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
                auto* armo = a_actor->GetWornArmor(static_cast<Slot>(1u << bit));
                if (!armo || !seen.insert(armo).second) {
                    continue;
                }
                StyleRefKey key;
                if (StyleRef::Make(armo, key)) {
                    out.SetStyle(bit, key);
                }
            }
            return out;
        }

        // The immutable first tab is not an Outfit. It is the library's
        // existing inactive state, so selecting it cannot be renamed, edited,
        // deleted, exported, or consume one of the saved outfit slots.
        void SelectEquippedGear(OutfitSession& a_session) {
            WithCurrentLibrary(a_session, [](OutfitLibrary& a_lib) {
                a_lib.Deactivate();
            });
            g_staged      = Outfit{};
            g_staged.name = "$FR_StateEquipped"_T;
            BeginStagingCurrent(a_session, g_staged);
            g_dirty       = false;
            g_renaming    = false;
            g_forceSelect = OutfitTabs::kForceEquippedGear;
            g_history.Reset(g_staged);
        }

        // NPC counterpart to EnsureActiveOutfit: make sure g_targetLibrary has
        // an active outfit before an NPC Apply writes the staged buffer into it.
        void EnsureActiveOutfitNpc() {
            if (g_targetLibrary.ActiveIndex() < 0) {
                const int idx = g_targetLibrary.Create(g_staged.name.empty() ? "Outfit 1"
                                                                             : g_staged.name);
                if (idx >= 0) {
                    g_targetLibrary.Activate(static_cast<std::size_t>(idx));
                }
            }
        }

        // Transient footer note for actionable failures/short statuses - shown
        // in both footer variants until its timestamp lapses. Export success
        // uses a short localized message; filesystem paths remain excluded
        // because this row also owns the right-aligned Saved/Close cluster.
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

        [[nodiscard]] std::vector<JsonCodec::Preset> CurrentPresetSnapshot() {
            switch (g_showcaseTabs.source) {
                case ShowcaseTabs::kDiscovered:
                    return AutoPresets::GetSingleton().Snapshot();
                case ShowcaseTabs::kExported:
                    return PresetStore::GetSingleton().SnapshotExports();
                case ShowcaseTabs::kCurated:
                default:
                    return PresetStore::GetSingleton().Snapshot();
            }
        }

        // FLICK's current theme frames an expanded collapsing header but drops
        // the frame when it closes. Add the same persistent border after the
        // item is submitted so every accordion keeps a stable visual box.
        bool FramedCollapsingHeader(const char* a_label, int a_flags = 0) {
            const bool   open = FUCK::CollapsingHeader(a_label, a_flags);
            if (open) {
                return true;  // the host theme already frames expanded headers
            }
            const ImVec2 min  = FUCK::GetItemRectMin();
            const ImVec2 max  = FUCK::GetItemRectMax();
            const float  t    = std::max(1.0f, FUCK::GetResolutionScale());
            const ImU32  col  = OS::ui::Col(ImGuiCol_Border);
            OS::ui::RectFilled(min, ImVec2(max.x, min.y + t), col);
            OS::ui::RectFilled(ImVec2(min.x, max.y - t), max, col);
            OS::ui::RectFilled(min, ImVec2(min.x + t, max.y), col);
            OS::ui::RectFilled(ImVec2(max.x - t, min.y), max, col);
            return open;
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
            struct ExportHealth {
                std::vector<std::string> missingPlugins;
                int                      unfitPieces{ 0 };
            };
            const auto exportHealth = [&](const JsonCodec::Preset& a_preset) {
                ExportHealth health;
                if (g_showcaseTabs.source != ShowcaseTabs::kExported) {
                    return health;
                }
                auto* dh = RE::TESDataHandler::GetSingleton();
                const auto addMissing = [&](const std::string& a_plugin) {
                    if (a_plugin.empty() ||
                        std::ranges::find(health.missingPlugins, a_plugin) !=
                            health.missingPlugins.end()) {
                        return;
                    }
                    if (!dh || !dh->LookupModByName(a_plugin)) {
                        health.missingPlugins.push_back(a_plugin);
                    }
                };
                for (const auto& req : a_preset.requires_) {
                    addMissing(req);
                }
                a_preset.outfit.ForEachStyle(
                    [&](std::uint32_t, const StyleRefKey& a_key) {
                        addMissing(a_key.modName);  // covers legacy exports without requires
                        if (auto* armo = StyleRef::Resolve(a_key);
                            armo &&
                            StyleCatalog::EvaluateFitFor(
                                armo, g_target.race, g_target.sexIdx) !=
                                FitReason::kFits) {
                            ++health.unfitPieces;
                        }
                    });
                a_preset.outfit.ForEachWeaponStyle(
                    [&](WeaponClass, const StyleRefKey& a_key) {
                        addMissing(a_key.modName);
                    });
                return health;
            };
            std::vector<ExportHealth> exportHealths;
            exportHealths.reserve(a_presets.size());
            for (const auto& preset : a_presets) {
                exportHealths.push_back(exportHealth(preset));
            }
            // Discovered (auto-detected), Curated (author-shipped), and
            // Exported (the player's reusable saved presets) feed one browser.
            const auto sourceTab = [&](const char* a_label, int a_src, const char* a_tip) {
                const int flags = ShowcaseTabs::ShouldForce(g_showcaseTabs, a_src)
                                      ? ImGuiTabItemFlags_SetSelected
                                      : 0;
                const bool active = FUCK::BeginTabItem(a_label, flags);
                if (FUCK::IsItemHovered()) {
                    OS::ui::SetTooltipF("%s", a_tip);
                }
                if (ShowcaseTabs::ObserveActive(g_showcaseTabs, a_src, active)) {
                    g_showcaseSel    = -1;
                    g_showcaseHover  = -1;
                    g_showcaseHoverPending = -1;
                    BeginStagingCurrent(a_session, g_staged);
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                if (active) {
                    FUCK::EndTabItem();
                }
            };
            if (FUCK::BeginTabBar("showcase_sources", ImGuiTabBarFlags_FittingPolicyScroll)) {
                sourceTab("$FR_Discovered"_T, ShowcaseTabs::kDiscovered,
                          "$FR_DiscoveredTip"_T);
                sourceTab("$FR_Curated"_T, ShowcaseTabs::kCurated,
                          "$FR_CuratedTip"_T);
                sourceTab("Exported", ShowcaseTabs::kExported,
                          "Outfits you exported. Save them to any player or follower library.");
                FUCK::EndTabBar();
                ShowcaseTabs::FinishTabBar(g_showcaseTabs);
            }

            // RB / LB cycles all preset sources. Gated on !IsAnyItemActive so the search keeps its
            // keys. Safe here - the showcases block returns before the outfit LB/LB
            // handler, so the shoulders are read once. The new source takes effect next
            // frame (this frame's list was fetched for the old one), matching a tab click.
            if (!FUCK::IsAnyItemActive()) {
                const bool next = FUCK::IsKeyPressed(ImGuiKey_GamepadR1, false);
                const bool prev = FUCK::IsKeyPressed(ImGuiKey_GamepadL1, false);
                if (next || prev) {
                    const int requested =
                        ShowcaseTabs::Cycle(g_showcaseTabs.source, next);
                    ShowcaseTabs::Request(g_showcaseTabs, requested);
                    g_showcaseSel          = -1;
                    g_showcaseHover        = -1;
                    g_showcaseHoverPending = -1;
                    BeginStagingCurrent(a_session, g_staged);
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
            }
            FUCK::Separator();

            const float rescanW = FUCK::CalcTextSize("$FR_Rescan"_T).x +
                                  OS::ui::FramePadding().x * 2.0f;
            // A button ending exactly at a child content maximum loses the
            // outside half of its frame stroke to that child's clip rect.
            const float trailingFrameClearance = OS::ui::FramePadding().x;
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(Icons::Utf8(Icons::kSearch).c_str());
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-(rescanW + OS::ui::ItemSpacing().x +
                                     trailingFrameClearance));
            FUCK::InputText("##showcase_search", g_showcaseSearch, sizeof(g_showcaseSearch));
            FUCK::SameLine();
            if (FUCK::Button("$FR_Rescan"_T)) {
                if (g_showcaseTabs.source == ShowcaseTabs::kDiscovered) {
                    // Pull in anything obtained since load before rebuilding the
                    // discovered sets, so Rescan reflects the real inventory (not
                    // just the possibly-stale collection).
                    Collection::GetSingleton().SeedFromPlayerInventory();
                    AutoPresets::RequestRescan();
                } else {
                    PresetStore::RequestRescan();
                }
                g_showcaseSel = -1;
                g_showcaseHover = -1;
                g_showcaseHoverPending = -1;
                BeginStagingCurrent(a_session, g_staged);
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                const char* tip =
                    g_showcaseTabs.source == ShowcaseTabs::kDiscovered
                        ? "Re-scan your armor mods for outfit sets."
                        : g_showcaseTabs.source == ShowcaseTabs::kExported
                              ? "Re-read your exported outfit presets."
                              : "Re-read the Presets folder.\nAuthors: drop a file in, "
                                "click, see it.";
                OS::ui::SetTooltipF("%s", tip);
            }
            FUCK::Separator();

            // Only the result list scrolls. Source tabs, search and Rescan stay
            // fixed at the top even after browsing deep into a large preset set.
            FUCK::BeginChild("showcase_list", ImVec2(listW, -a_footerH), true);

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
            int         frameHover = -1;
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
                    groupOpen = FramedCollapsingHeader(header.c_str(),
                                                       ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!groupOpen) {
                    continue;  // collapsed plugin - skip its rows
                }
                FUCK::PushID(static_cast<int>(i));
                FUCK::Indent();
                // Lore-friendly only. In free-form the collection is not a
                // gate at all, so a preset is exactly what it says it is.
                const bool gated = Settings::GetSingleton().collectionOnly;
                const auto own   = gated ? OwnedPieces(p.outfit) : PresetOwnership{};
                const bool partial = gated && !own.Complete();
                const auto& health = exportHealths[i];
                const bool highlight = PresetPreviewPolicy::HighlightPresetRow(
                    g_showcaseTabs.source == ShowcaseTabs::kExported,
                    health.missingPlugins.size(),
                    static_cast<std::size_t>(health.unfitPieces), partial);
                if (highlight) {
                    // Same muted treatment the unfit style rows use, so this
                    // reads as an established idiom rather than a new state.
                    FUCK::PushStyleColor(ImGuiCol_Text, kUnfitText);
                }
                const bool clicked = FUCK::Selectable(
                    p.name.c_str(), g_showcaseSel == static_cast<int>(i));
                const bool hovered = FUCK::IsItemHovered();
                if (clicked) {
                    g_showcaseSel = static_cast<int>(i);
                    g_showcaseHover = -1;
                    g_showcaseHoverPending = -1;
                    CrashGuard::ClearPreviewing();  // whole preset - no single style to blame
                    // Preview is always the complete preset. Lore-friendly mode
                    // gates only saving it into My Outfits, never trying it on.
                    BeginStagingCurrent(a_session, p.outfit);
                    EditorStyle::PlayUISound("UIMenuOK");
                }
                if (highlight) {
                    FUCK::PopStyleColor();
                }
                if (hovered) {
                    frameHover = static_cast<int>(i);
                    std::string tip = p.file;
                    if (!health.missingPlugins.empty()) {
                        tip += "\nMissing required plugin";
                        tip += health.missingPlugins.size() == 1 ? ": " : "s: ";
                        for (const auto& plugin : health.missingPlugins) {
                            tip += plugin == health.missingPlugins.front()
                                       ? plugin
                                       : ", " + plugin;
                        }
                    }
                    if (health.unfitPieces > 0) {
                        tip += "\n";
                        tip += std::to_string(health.unfitPieces);
                        tip += health.unfitPieces == 1
                                   ? " piece may not fit this body."
                                   : " pieces may not fit this body.";
                    }
                    if (partial) {
                        tip += "\nLore-friendly collection: ";
                        tip += std::to_string(own.owned) + "/" +
                               std::to_string(own.total) + " pieces owned.";
                    }
                    OS::ui::SetTooltipF("%s", tip.c_str());
                }
                FUCK::Unindent();
                FUCK::PopID();
            }
            if (!any) {
                if (a_presets.empty() &&
                    g_showcaseTabs.source == ShowcaseTabs::kDiscovered) {
                    FUCK::TextDisabled("%s", "$FR_NoDiscovered"_T);
                } else if (a_presets.empty() &&
                           g_showcaseTabs.source == ShowcaseTabs::kExported) {
                    FUCK::TextDisabled("%s", "No exported outfits yet.");
                } else {
                    FUCK::TextDisabled("%s", "$FR_NoMatches"_T);
                }
            }

            // Preset hover mirrors style-row hover: debounce the expensive
            // rebuild, stage transiently without changing the clicked selection,
            // and restore that selection or the edit buffer when the pointer leaves.
            if (g_hoverPreview) {
                const double now = FUCK::GetTime();
                if (frameHover >= 0 && frameHover != g_showcaseSel) {
                    if (frameHover != g_showcaseHover) {
                        if (frameHover != g_showcaseHoverPending) {
                            g_showcaseHoverPending = frameHover;
                            g_showcaseHoverSince   = now;
                        } else if (now - g_showcaseHoverSince >= 0.18) {
                            CrashGuard::ClearPreviewing();
                            BeginStagingCurrent(
                                a_session,
                                a_presets[static_cast<std::size_t>(frameHover)].outfit);
                            g_showcaseHover = frameHover;
                        }
                    }
                } else {
                    g_showcaseHoverPending = -1;
                    if (g_showcaseHover >= 0) {
                        CrashGuard::ClearPreviewing();
                        if (g_showcaseSel >= 0 &&
                            g_showcaseSel < static_cast<int>(a_presets.size())) {
                            BeginStagingCurrent(
                                a_session,
                                a_presets[static_cast<std::size_t>(g_showcaseSel)].outfit);
                        } else {
                            BeginStagingCurrent(a_session, g_staged);
                        }
                        g_showcaseHover = -1;
                    }
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
                    FUCK::PushFontScaled(title, g_uiScale);
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
                const auto& selectedHealth =
                    exportHealths[static_cast<std::size_t>(g_showcaseSel)];
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
                        // Same fit flag as the style browser (OS-3), judged
                        // against the CURRENT target's race + sex (Task 8): a
                        // preset piece that fits the player may not fit a
                        // custom-race follower and vice versa, and the tooltip
                        // wording (FitReasonText) already reflects whichever
                        // target RefreshFitFor last cached on the switch.
                        if (const auto reason =
                                StyleCatalog::EvaluateFitFor(armo, g_target.race, g_target.sexIdx);
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
                        if (g_showcaseTabs.source == ShowcaseTabs::kExported) {
                            FUCK::TextColored(kUnfitText, "$FR_PieceMissing"_T, label,
                                               entry.style.modName.c_str());
                        } else {
                            FUCK::TextDisabled("$FR_PieceMissing"_T, label,
                                                entry.style.modName.c_str());
                        }
                    }
                }
                if (!selectedHealth.missingPlugins.empty()) {
                    std::string joined;
                    for (const auto& plugin : selectedHealth.missingPlugins) {
                        joined += joined.empty() ? plugin : ", " + plugin;
                    }
                    FUCK::Spacing();
                    FUCK::TextColored(
                        kUnfitText, "Missing required plugin%s: %s",
                        selectedHealth.missingPlugins.size() == 1 ? "" : "s",
                        joined.c_str());
                    FUCK::TextWrapped(
                        "%s",
                        "This export remains available so you can inspect or delete it. "
                        "Missing pieces are not applied.");
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
                FUCK::TextWrapped("%s",
                    g_showcaseTabs.source == ShowcaseTabs::kExported
                        ? "Exported outfits are your reusable presets. Select one to "
                          "preview it, then save it into the current player or follower library."
                        : "Presets are reusable looks. Previews are free, and nothing "
                          "changes until you save one to an outfit library.");
            }
            FUCK::EndChild();
        }
    }

    // Defined below, called by SwitchTarget above it. Declared HERE, at file
    // scope, not inside the anonymous namespace with g_bodyPresets: a
    // declaration in there and the definition out here are two different
    // functions, and the call site sees both.
    void RefreshBodyPresets();

    // Build the "Editing:" roster on the MAIN thread (OnOpen runs there, via
    // SetOpen's SKSE task): Player first, then live teammates, then persisted
    // "(away)" assignees whose base is not currently loaded. ForEachHighActor
    // MUST NOT run on the FUCK present thread (BSTArray mutation on process
    // transition), which is why this is an at-open, main-thread build; editor
    // input is modal, so the roster stays complete for the session. Publishes
    // an immutable vector the render thread atomic-loads race-free.
    void BuildTargetList() {
        auto& session = OutfitSession::GetSingleton();
        auto  list    = std::make_shared<std::vector<EditTarget>>();

        auto* player = RE::PlayerCharacter::GetSingleton();

        // 1) Player - always first, always the default.
        {
            EditTarget pl;
            pl.npc         = std::nullopt;
            pl.loaded      = true;
            pl.label       = "$FR_Target_Player"_T;
            pl.displayName = pl.label;
            if (player) {
                pl.handle = player->GetHandle();
                pl.race   = player->GetRace();
                pl.sexIdx = StyleCatalog::SexIdxOf(player->GetActorBase());
                if (auto* base = player->GetActorBase()) {
                    pl.baseFormID = base->GetFormID();
                }
            }
            list->push_back(std::move(pl));
        }

        // 2) Live teammates: IsPlayerTeammate && !IsDead && has a non-dynamic
        //    base. Dynamic/FF actors are excluded (spec: no persistent identity
        //    to key an assignment on). Names are disambiguated by plugin only on
        //    collision (BuildDisambiguatedLabels).
        std::vector<EditTarget>        teammates;
        std::vector<TargetLabelInput>  labelInputs;
        std::unordered_set<std::uint32_t> loadedBases;
        if (auto* processLists = RE::ProcessLists::GetSingleton()) {
            processLists->ForEachHighActor(
                [&](RE::Actor& a_actor) -> RE::BSContainer::ForEachResult {
                    if (!a_actor.IsPlayerTeammate() || a_actor.IsDead()) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    auto* base = a_actor.GetActorBase();
                    if (!base || base->IsDynamicForm()) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    EditTarget t;
                    t.handle     = a_actor.GetHandle();
                    t.race       = a_actor.GetRace();
                    t.sexIdx     = StyleCatalog::SexIdxOf(base);
                    t.baseFormID = base->GetFormID();
                    t.loaded     = true;
                    t.equippedPreview = SnapshotEquippedArmor(&a_actor);
                    NpcKey key;
                    if (auto* file = base->GetFile(0)) {
                        key.modName = std::string{ file->GetFilename() };
                    }
                    key.localFormID = base->GetLocalFormID();
                    t.npc           = key;

                    const char* nm = base->GetName();
                    labelInputs.push_back({ nm ? nm : "", key.modName });
                    teammates.push_back(std::move(t));
                    loadedBases.insert(base->GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
        }

        // 3) "(away)" assignees: persisted assignments whose base isn't loaded.
        //    Resolvable base -> real name + race/sex from the base record;
        //    unresolved plugin -> a plugin-derived placeholder (kept verbatim,
        //    still selectable so the user can clear it). Add them to the same
        //    disambiguation pool as the teammates before the "(away)" suffix.
        auto* dh = RE::TESDataHandler::GetSingleton();
        for (const auto& [key, rec] : session.SnapshotNpcAssignments()) {
            RE::TESNPC* base =
                (dh && !key.modName.empty())
                    ? dh->LookupForm<RE::TESNPC>(key.localFormID, key.modName)
                    : nullptr;
            if (base && loadedBases.contains(base->GetFormID())) {
                continue;  // currently loaded -> already listed as a teammate
            }
            EditTarget t;
            t.npc    = key;
            t.loaded = false;  // (away)
            // handle stays empty: an unloaded actor can't be previewed/kicked.
            std::string name;
            if (base) {
                t.race       = base->GetRace();
                t.sexIdx     = StyleCatalog::SexIdxOf(base);
                t.baseFormID = base->GetFormID();
                if (const char* nm = base->GetName()) {
                    name = nm;
                }
            }
            if (name.empty()) {
                name = key.modName.empty() ? std::string{ "?" } : key.modName;
            }
            labelInputs.push_back({ name, key.modName });
            teammates.push_back(std::move(t));  // "(away)" suffix applied below
        }

        // Disambiguate the whole non-player pool, then tag "(away)" entries.
        const auto labels = BuildDisambiguatedLabels(labelInputs);
        for (std::size_t i = 0; i < teammates.size(); ++i) {
            teammates[i].displayName = labels[i];
            teammates[i].label       = labels[i];
            if (!teammates[i].loaded) {
                teammates[i].label += "$FR_Target_Away"_T;  // suffix on the dropdown label only
            }
            list->push_back(std::move(teammates[i]));
        }

        g_targetList.store(std::move(list), std::memory_order_release);
    }

    // Switch the whole editor context to a new target. Runs on the render
    // thread (called from the dropdown handler in Draw), so it owns the
    // render-thread editor state (g_staged / g_history / g_targetLibrary /
    // g_target) directly; only the fit re-cache + worn snapshot - which touch
    // engine state and the global fit cache - are marshaled to the main thread,
    // gated by g_fitReady so the browser never draws mid-rebuild. Mirrors the
    // outfit-tab-switch discipline: discard staging, clear the preview, load the
    // target's library, reset history, begin staging on the new target.
    void SwitchTarget(OutfitSession& a_session, int a_newIndex) {
        const auto list = g_targetList.load(std::memory_order_acquire);
        if (!list || a_newIndex < 0 || a_newIndex >= static_cast<int>(list->size()) ||
            a_newIndex == g_targetIndex) {
            return;
        }
        const EditTarget t = (*list)[static_cast<std::size_t>(a_newIndex)];

        CrashGuard::ClearPreviewing();
        a_session.DiscardStaging();
        g_hoverKey       = StyleRefKey{};
        g_hoverPending   = StyleRefKey{};
        g_selectedWeapon = std::nullopt;  // armor is the default dimension per target
        g_selectedWeaponHand = WeaponHand::Both;

        if (!t.npc) {
            // Player: same as OnOpen's player path. Never touch g_targetLibrary.
            const auto snap = a_session.SnapshotLibrary();
            if (const auto* active = snap.Active()) {
                g_staged = *active;
            } else {
                g_staged      = Outfit{};
                g_staged.name = "$FR_StateEquipped"_T;
            }
            g_targetLibrary = OutfitLibrary{};
            g_forceSelect =
                OutfitTabs::ForcedSelectionForActive(snap.ActiveIndex());
        } else {
            // NPC / (away): load the base's saved library, or a fresh one.
            const auto assignments = a_session.SnapshotNpcAssignments();
            if (const auto it = assignments.find(*t.npc); it != assignments.end()) {
                g_targetLibrary = it->second.library;
            } else {
                g_targetLibrary = OutfitLibrary{};
            }
            if (const auto* active = g_targetLibrary.Active()) {
                g_staged = *active;
            } else {
                g_staged      = Outfit{};
                g_staged.name = "$FR_StateEquipped"_T;
            }
            g_forceSelect =
                OutfitTabs::ForcedSelectionForActive(g_targetLibrary.ActiveIndex());
        }

        g_dirty      = false;
        g_history.Reset(g_staged);
        g_justOpened = true;  // mute the tab-activate for the switch frame (as at open)
        g_target     = t;
        g_targetIndex = a_newIndex;
        // After g_target, before anything draws: the preset list is per-sex.
        RefreshBodyPresets();

        BeginStagingCurrent(a_session, g_staged);

        // Marshal the per-target fit re-cache + worn-mask snapshot to the main
        // thread; flip g_fitReady only once both complete so the render thread
        // never draws catalog rows against a half-rebuilt fitsBody cache.
        g_fitReady.store(false, std::memory_order_release);
        const bool            isPlayer = !t.npc.has_value();
        RE::TESRace* const    race     = t.race;
        const int             sexIdx   = t.sexIdx;
        const RE::ActorHandle handle   = t.handle;
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([isPlayer, race, sexIdx, handle] {
                try {
                    if (isPlayer) {
                        StyleCatalog::GetSingleton().RefreshFit();
                    } else {
                        StyleCatalog::GetSingleton().RefreshFitFor(race, sexIdx);
                    }
                    // Worn-at-open mask from the TARGET actor (player or a loaded
                    // follower); an "(away)" target has no live actor -> 0.
                    std::uint32_t worn  = 0;
                    RE::Actor*    actor = nullptr;
                    if (isPlayer) {
                        actor = RE::PlayerCharacter::GetSingleton();
                    } else {
                        auto ptr = handle.get();
                        actor    = ptr.get();
                    }
                    if (actor) {
                        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                        for (std::uint32_t bit = 0; bit < 32; ++bit) {
                            if (actor->GetWornArmor(static_cast<Slot>(1u << bit))) {
                                worn |= 1u << bit;
                            }
                        }
                    }
                    g_wornMask = worn;
                } catch (const std::exception& e) {
                    spdlog::error("EditorUI: target-switch fit re-cache threw: {}", e.what());
                } catch (...) {
                    spdlog::error("EditorUI: target-switch fit re-cache threw (non-standard).");
                }
                g_fitReady.store(true, std::memory_order_release);
            });
        } else {
            g_fitReady.store(true, std::memory_order_release);
        }
    }

    // Re-snapshot the OBody preset list for whoever is being dressed now.
    // Called at open and after every target switch: the list is sex-specific,
    // and switching from a male follower to a female one must not leave the
    // combo offering presets that cannot apply.
    void RefreshBodyPresets() {
        g_bodyPresets.clear();
        if (!ObodyApi::Available()) {
            return;  // absent or momentarily unready - the section says so
        }
        g_bodyPresets = ObodyApi::PresetNames(g_target.sexIdx == RE::SEXES::kFemale);
    }

    void OnOpen() {
        auto& session   = OutfitSession::GetSingleton();
        // The name field cannot still be live across an open, and a stale true
        // would pin the tab bar and swallow tab clicks for the whole session -
        // the name row that clears it does not draw while Presets are open or
        // while the library is empty.
        g_renaming = false;

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

        g_uiScale       = std::clamp(Settings::GetSingleton().uiScale,
                                     Settings::kUiScaleMin,
                                     Settings::kUiScaleMax);
        g_hoverPreview  = Settings::GetSingleton().hoverPreview;
        g_hoverKey      = StyleRefKey{};
        g_hoverPending  = StyleRefKey{};
        g_armorType     = -1;   // "All types" each open
        g_selectedWeapon = std::nullopt;  // armor is the default browser dimension each open (Task 8)
        g_selectedWeaponHand = WeaponHand::Both;
        g_favoritesOnly = false;  // "Favorites" filter is session-local, off each open
        g_hideUnfit     = true;   // hide body-unfit (red) rows by default each open (best UX per user)
        g_matchMask     = 0;
        g_lastMatchQuery.clear();
        g_lastMatchArmorType = -1;
        g_lastMatchFavorites = false;
        g_showcasesOpen = false;
        g_showcaseSel   = -1;
        g_showcaseHover = -1;
        g_showcaseHoverPending = -1;
        g_pendingDelete = -1;
        g_footNote.clear();

        // Reset the "Editing:" target to the PLAYER and (re)build the roster on
        // this main thread (SetOpen runs OnOpen via the SKSE task queue, so
        // ForEachHighActor is safe here and not on the FUCK present thread).
        // Every open starts on the player, so a session that never opens the
        // dropdown is byte-for-byte the old behavior. The player's fit cache is
        // already current - SetOpen called EnsureFitCurrent before this.
        g_targetLibrary = OutfitLibrary{};
        g_fitReady.store(true, std::memory_order_release);
        BuildTargetList();
        if (const auto list = g_targetList.load(std::memory_order_acquire); list && !list->empty()) {
            g_target = (*list)[0];  // the player entry
        } else {
            g_target = EditTarget{};
        }
        g_targetIndex = 0;

        const auto snap = session.SnapshotLibrary();
        if (const auto* active = snap.Active()) {
            g_staged = *active;
        } else {
            g_staged      = Outfit{};
            g_staged.name = "$FR_StateEquipped"_T;
        }
        g_dirty = false;
        g_history.Reset(g_staged);  // undo baseline = the outfit we opened on
        // The tab bar restores ITS OWN remembered selection on reopen (usually
        // the first tab) - force it onto the ACTIVE outfit instead, and mute
        // the switch handler for the opening frame so the stale selection can
        // never activate a different outfit than the one being worn.
        g_forceSelect = OutfitTabs::ForcedSelectionForActive(snap.ActiveIndex());
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
        RefreshBodyPresets();
        session.BeginStaging(g_staged);
    }

    void OnClose() {
        // Closing without Apply reverts: nothing is committed until the button.
        g_renaming = false;  // see OnOpen
        CrashGuard::ClearPreviewing();
        g_hoverKey     = StyleRefKey{};
        g_hoverPending = StyleRefKey{};
        g_showcaseHover = -1;
        g_showcaseHoverPending = -1;
        OutfitSession::GetSingleton().DiscardStaging();
        g_dirty = false;

        // Restore the PLAYER's fit cache: the editor may have switched it to an
        // NPC target via RefreshFitFor (Task 8). Guard on a live player: with no
        // player, RefreshFit() funnels through RefreshFitFor(nullptr, ...), whose
        // "is this the player" test latches fitSubjectIsPlayer_ = false - which
        // would then stop EnsureFitCurrent from ever self-healing the player's
        // cache (Task 7). Skipping the restore when there is no player is
        // harmless (nothing to restore to; the next open re-runs EnsureFitCurrent).
        if (RE::PlayerCharacter::GetSingleton()) {
            StyleCatalog::GetSingleton().RefreshFit();
        }
        // Next open starts on the player (OnOpen rebuilds the roster anyway; this
        // keeps any stray Draw between close and the next open on the player).
        g_target      = EditTarget{};
        g_targetIndex = 0;
        g_fitReady.store(true, std::memory_order_release);
    }

    void Draw() {
        auto& session = OutfitSession::GetSingleton();
        // The current target's library: the player's (session) or the NPC's
        // editor-held working copy (Task 8). Every tab / rename / add / delete /
        // footer read below goes through this one snapshot, so the player-only
        // path is unchanged and an NPC target transparently edits its own library.
        const auto snap = CurrentLibrarySnapshot(session);
        const bool away = g_target.npc.has_value() && !g_target.loaded;
        bool       actualGear = snap.ActiveIndex() < 0;
        const bool browseCollectedOnly = EditorGate::BrowseCollectedOnly(
            Settings::GetSingleton().collectionOnly);

        // The editor draws as a FUCK IWindow: FUCK owns the window chrome, size,
        // position and Present, so we draw content only. The game UI is hidden
        // while open and the right side of the screen is the Show-Player-In-Menus
        // character. Close via the editor hotkey / Esc (kCloseOnEsc).
        // UI size is not routed through SetWindowFontScale because it did not
        // visibly resize glyphs on this FUCK build. PushFontScaled is the
        // supported path and is scoped around the complete editor below.

        // Controller: keep the nav highlight asserted while a gamepad is the active
        // input so it stays in panel-nav mode instead of dropping into FUCK's
        // mouse-cursor mode (field: "weird cursor mode"). Best-effort; version-gated
        // no-op on older FUCK - if it doesn't hold, this becomes a Fuzzles question.
        if (FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad) {
            FUCK::SetNavCursorVisible(true);
        }

        // Scale the complete editor from one root scope. The previous local
        // font push covered only the style-browser child, so the two columns
        // disagreed about row height, spacing and the meaning of "UI size".
        auto* editorFont = FUCK::GetFont(FUCK::Font::kRegular);
        if (editorFont) {
            FUCK::PushFontScaled(editorFont, g_uiScale);
        }

        // The host window itself must never become a second scroll container.
        // All intentional scrolling lives in bounded list children below. This
        // fill child keeps header and footer fixed and prevents the outer panel
        // from moving them offscreen with the mouse wheel.
        FUCK::BeginChild("editor_fixed_root", ImVec2(0, 0), false,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Title (left). Large display font; AlignTextToFramePadding lines it up with the
        // top-right button cluster drawn on the same row.
        if (auto* titleFont = FUCK::GetFont(FUCK::Font::kLarge)) {
            FUCK::PushFontScaled(titleFont, g_uiScale);
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
        const auto  targetList = g_targetList.load(std::memory_order_acquire);
        const bool  showTarget = !g_showcasesOpen && targetList && targetList->size() > 1;
        const char* pLabel     = g_showcasesOpen ? "$FR_Outfits"_T : "$FR_Presets"_T;
        const float gearW = FUCK::CalcTextSize(Icons::Utf8(Icons::kGear).c_str()).x +
                            OS::ui::FramePadding().x * 2.0f;
        const float presetsW =
            hasPresets ? FUCK::CalcTextSize(pLabel).x + OS::ui::FramePadding().x * 2.0f : 0.0f;
        const float gap = hasPresets ? OS::ui::ItemSpacing().x : 0.0f;
        const float clusterX = FUCK::GetWindowSize().x - OS::ui::WindowPadding().x - gearW -
                               gap - presetsW;

        // Editing target lives on the title row immediately left of Presets.
        // Moving it here saves a full vertical row when followers are present.
        if (showTarget) {
            std::vector<std::string> labels;
            labels.reserve(targetList->size());
            for (const auto& t : *targetList) {
                labels.push_back(t.label);
            }
            const float comboW  = OS::ui::FontSize() * 12.0f;
            const float labelW  = FUCK::CalcTextSize("$FR_Editing"_T).x;
            const float targetW = labelW + OS::ui::ItemSpacing().x + comboW;
            const float targetX = std::max(FUCK::GetCursorPos().x + OS::ui::ItemSpacing().x,
                                           clusterX - OS::ui::ItemSpacing().x - targetW);
            FUCK::SameLine(targetX);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Editing"_T);
            FUCK::SameLine();
            int cur = (g_targetIndex >= 0 &&
                       g_targetIndex < static_cast<int>(labels.size()))
                          ? g_targetIndex
                          : 0;
            FUCK::SetNextItemWidth(comboW);
            if (FUCK::Combo("##editing_target", &cur, labels) && cur != g_targetIndex) {
                SwitchTarget(session, cur);
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_EditingTip"_T);
            }
        }

        FUCK::SameLine(clusterX);
        if (hasPresets) {
            // Presets (author showcases): toggle the read-only presets browser.
            if (FUCK::Button(pLabel)) {
                if (g_showcasesOpen) {
                    // Back to the outfits: re-assert the edit buffer.
                    g_showcasesOpen = false;
                    g_showcaseSel   = -1;
                    session.SetPresetPreviewSuppression(false);
                    BeginStagingCurrent(session, g_staged);
                } else {
                    g_showcasesOpen = true;
                    g_showcaseSel   = -1;  // staging keeps the edit buffer until a click
                    ShowcaseTabs::Request(g_showcaseTabs, g_showcaseTabs.source);
                    session.SetPresetPreviewSuppression(true);
                }
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(
                    g_showcasesOpen
                        ? "$FR_BackToOutfits"_T
                        : "Browse discovered, curated, and exported outfit presets.");
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
            if (FUCK::SliderFloat("$FR_UiSize"_T, &g_uiScale,
                                  Settings::kUiScaleMin,
                                  Settings::kUiScaleMax, "%.2fx")) {
                Settings::GetSingleton().uiScale = g_uiScale;  // live
            }
            if (FUCK::IsItemDeactivatedAfterEdit()) {
                Settings::GetSingleton().Save();  // persist on release
            }
            if (FUCK::Checkbox("$FR_PreviewOnHover"_T, &g_hoverPreview)) {
                Settings::GetSingleton().hoverPreview = g_hoverPreview;
                Settings::GetSingleton().Save();
                if (!g_hoverPreview && !g_hoverKey.Empty()) {  // turning it off drops any preview
                    CrashGuard::ClearPreviewing();
                    OutfitSession::GetSingleton().UpdateStaging(g_staged);
                    g_hoverKey = StyleRefKey{};
                }
                if (!g_hoverPreview && g_showcaseHover >= 0) {
                    const auto presets = CurrentPresetSnapshot();
                    if (g_showcaseSel >= 0 &&
                        g_showcaseSel < static_cast<int>(presets.size())) {
                        BeginStagingCurrent(
                            session, presets[static_cast<std::size_t>(g_showcaseSel)].outfit);
                    } else {
                        BeginStagingCurrent(session, g_staged);
                    }
                    g_showcaseHover = -1;
                    g_showcaseHoverPending = -1;
                }
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_PreviewOnHoverTip"_T);
            }
            // Style-list columns moved here (OS-30) from a separate gear in the
            // browser - one settings home. Name is always shown.
            FUCK::Separator();
            FUCK::TextDisabled("%s", "$FR_StyleColumns"_T);
            FUCK::Checkbox("$FR_ColClass"_T, &g_showClass);
            FUCK::Checkbox("$FR_ColPlugin"_T, &g_showSource);
            FUCK::Separator();
            if (FUCK::Checkbox("$FR_LockWindow"_T, &Settings::GetSingleton().lockLayout)) {
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
            float tabTop    = 0.0f;
            float tabHeight = 0.0f;
            bool  forcedSelectionObserved = false;
            // TAB PADDING (field 2026-07-18, second pass). The first attempt
            // padded every label out to the WIDEST name in the library. That
            // was a misread of the report: Feet of Skyrim's bar - the reference
            // it was made against - is evenly PADDED, not equal width;
            // "General" is visibly wider than "Feet" there. Equalising made
            // four outfits fill the whole bar with no way to scroll sideways.
            //
            // So: natural per-label width, one uniform inset. Getting there took
            // three field rounds, and they pinned down two facts about FLICK's
            // tab item that are NOT standard ImGui behaviour. Both are recorded
            // because everything here follows from them:
            //
            //  1. IT IGNORES FramePadding. Round three drew the label with no
            //     spaces and a 10px FramePadding push, and the text came out
            //     flush against the tab border. So the style stack is not a
            //     lever here at all - the push was dead code and is gone.
            //  2. IT MEASURES THE "###" ID SUFFIX AS VISIBLE WIDTH. Round two
            //     used "###outfit" (9 chars) and produced a large gap on the
            //     RIGHT ONLY; round three shortened it to "###o" (4 chars) and
            //     the gap shrank in proportion. A suffix is invisible but still
            //     counted, so it lands as dead space after the name.
            //
            // Together those mean the inset can only come from spaces, and no
            // amount of space juggling can balance a tab that carries a hidden
            // suffix on one side: matching ~28px of dead right-hand width would
            // need ~28px of leading spaces, i.e. the "way too large" tabs the
            // first round was reported for. So the suffix is gone entirely and
            // the padding is symmetric spaces.
            //
            // Dropping "###" costs the id pinning it provided - the label is the
            // identity again, so a rename changes it mid-type. That is handled
            // where it belongs, in the tab loop, via g_renaming (PushID(i) still
            // keeps two outfits with the SAME name from colliding).
            // ROUND FIVE (2026-07-18): BOTH facts above are WRONG. Settled by
            // disassembling FLICK rather than by a sixth field guess - FUCK.dll
            // ships a PDB beside it, and ImGui::TabItemCalcSize (RVA 0x12bc40 in
            // FUCK 1.6) reads:
            //
            //     call  ...                 ; CalcTextSize(label, NULL, r9b=1)
            //     movss xmm1, [rdi+0xcc4]   ; style.FramePadding.x
            //     addss xmm3, [rsp+0x40]    ; + label_size.x
            //
            //  -> r9b=1 IS hide_text_after_double_hash, so it DOES strip "###".
            //     Fact 2 was a misdiagnosis.
            //  -> it reads FramePadding.x and applies it twice (+1). Tabs size
            //     exactly like stock ImGui. Fact 1 was a misdiagnosis too.
            //
            // Round three's "FramePadding does nothing" was a UNIT bug, not an
            // API limit: it pushed a RAW 10.0f for x while passing the SCALED
            // OS::ui::FramePadding().y for y. FLICK scales its style, so a raw
            // 10 is small - the tabs came out tight, which read as "ignored".
            //
            // So the never-tested combination is the obvious one: NO spaces and
            // NO override - tabs inset by the same FramePadding as every other
            // widget in the panel, which is what "regular" means and what ImGui
            // does by default. The spaces were a workaround for a bug that was
            // never there, and each round of tuning them fought the real cause.
            constexpr int kTabPadSpaces = 0;
            // One-shot evidence so a sixth round is arithmetic, not eyeballing:
            // the live FramePadding, the scales behind it, and a measured label.
            {
                static bool s_loggedTabMetrics = false;
                if (!s_loggedTabMetrics) {
                    s_loggedTabMetrics      = true;
                    const ImVec2 fp         = OS::ui::FramePadding();
                    const ImVec2 sampleSize = FUCK::CalcTextSize("Abyss");
                    spdlog::debug(
                        "EditorUI tab metrics: FramePadding=({:.2f},{:.2f}) res={:.3f} "
                        "global={:.3f} user={:.3f} textW(\"Abyss\")={:.2f} -> expected tab "
                        "width {:.2f}",
                        fp.x, fp.y, FUCK::GetResolutionScale(), FUCK::GetGlobalScale(),
                        FUCK::GetUserScale(), sampleSize.x, sampleSize.x + fp.x * 2.0f + 1.0f);
                }
            }
            // Immutable tab 0: the actor's actual equipped gear. This maps to
            // OutfitLibrary::Deactivate(), not to an empty mutable Outfit, so
            // it sits outside the saved-outfit cap and has no edit actions.
            const std::string equippedLabel =
                Icons::Utf8(Icons::kRealGear) + " " + "$FR_StateEquipped"_T;
            const bool equippedPin = OutfitTabs::ShouldForceEquipped(
                snap.ActiveIndex() < 0, g_forceSelect);
            FUCK::PushStyleColor(
                ImGuiCol_Text, FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool equippedActive = FUCK::BeginTabItem(
                equippedLabel.c_str(),
                equippedPin ? ImGuiTabItemFlags_SetSelected : 0);
            {
                const ImVec2 tabMin = FUCK::GetItemRectMin();
                const ImVec2 tabMax = FUCK::GetItemRectMax();
                tabTop              = tabMin.y;
                tabHeight           = tabMax.y - tabMin.y;
            }
            if (equippedActive) {
                const bool accept = OutfitTabs::ShouldAcceptActivation(
                    /*reported Equipped*/ -1, g_forceSelect);
                forcedSelectionObserved =
                    forcedSelectionObserved ||
                    (accept && g_forceSelect == OutfitTabs::kForceEquippedGear);
                if (accept && snap.ActiveIndex() >= 0 && !g_justOpened && !g_renaming) {
                    SelectEquippedGear(session);
                    actualGear = true;
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                FUCK::EndTabItem();
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_ResetOutfitTip"_T);
            }
            FUCK::PopStyleColor();

            for (std::size_t i = 0; i < snap.Count(); ++i) {
                const auto* o = snap.At(i);
                FUCK::PushID(static_cast<int>(i));  // duplicate names must not collide
                // Padding is spaces and nothing else - see the block above.
                const std::string tabLabel =
                    std::string(kTabPadSpaces, ' ') + o->name + std::string(kTabPadSpaces, ' ');
                // Pin the active tab while the name field is live: without the
                // "###" id the label IS the identity, so a keystroke would
                // otherwise drop the selection (and fire the switch below on
                // whichever index inherited it).
                const bool pin = g_renaming ? (snap.ActiveIndex() == static_cast<int>(i))
                                            : (g_forceSelect == static_cast<int>(i));
                const ImGuiTabItemFlags flags = pin ? ImGuiTabItemFlags_SetSelected : 0;
                // FUCK tab items have no built-in close X; outfit deletion is
                // the Delete button in the name row (OS-40).
                const bool active = FUCK::BeginTabItem(tabLabel.c_str(), flags);
                if (active) {
                    const bool accept = OutfitTabs::ShouldAcceptActivation(
                        static_cast<int>(i), g_forceSelect);
                    forcedSelectionObserved =
                        forcedSelectionObserved ||
                        (accept && g_forceSelect == static_cast<int>(i));
                    if (accept && snap.ActiveIndex() != static_cast<int>(i) &&
                        !g_justOpened && !g_renaming) {
                        WithCurrentLibrary(session, [&](OutfitLibrary& lib) { lib.Activate(i); });
                        g_staged = *o;  // from this frame's snapshot copy
                        BeginStagingCurrent(session, g_staged);
                        actualGear = false;
                        g_dirty = false;
                        g_history.Reset(g_staged);  // undo is per-outfit - fresh timeline
                        EditorStyle::PlayUISound("UIMenuFocus");
                    }
                    FUCK::EndTabItem();
                }
                FUCK::PopID();
            }
            FUCK::EndTabBar();
            // "+" is deliberately NOT a Button and NOT a fake BeginTabItem.
            // The fake tab was already tried and double-created when selection
            // changed. A normal FLICK Button has a taller framed box than its
            // tab item and SameLine lets that box increase the complete row's
            // height. Submit one exact-tab-height hit target instead, anchored
            // to the measured first tab, then paint it with tab colours. Both
            // height and glyph baseline remain correct at every host/UI scale.
            if (snap.Count() < kMaxOutfits) {
                FUCK::SameLine();
                const ImVec2 addCursor = FUCK::GetCursorScreenPos();
                const ImVec2 plusSize  = FUCK::CalcTextSize("+");
                const auto layout = OutfitTabAdd::Measure(
                    tabHeight, plusSize.x, plusSize.y, OS::ui::FramePadding().x);
                FUCK::SetCursorScreenPos(ImVec2(addCursor.x, tabTop));
                const bool addClicked =
                    FUCK::InvisibleButton("##add_outfit", ImVec2(layout.width, layout.height));
                const bool addHovered = FUCK::IsItemHovered();
                const bool addHeld    = FUCK::IsItemActive();
                const ImVec2 addMin   = FUCK::GetItemRectMin();
                const ImVec2 addMax   = FUCK::GetItemRectMax();
                const float grey = OutfitTabAdd::IdleGrey() +
                                   (addHovered ? 0.08f : 0.0f) -
                                   (addHeld ? 0.12f : 0.0f);
                const ImVec4 fill{ grey, grey, grey * 0.98f, 1.0f };
                const float radius =
                    OutfitTabAdd::CornerRadius(FUCK::GetResolutionScale());
                OS::ui::RectFilled(addMin, addMax, OS::ui::Col(fill), radius);
                FUCK::DrawRect(addMin, addMax,
                               FUCK::GetStyleColorVec4(ImGuiCol_Border),
                               radius,
                               std::max(1.0f, FUCK::GetResolutionScale()));
                OS::ui::TextAt(ImVec2(addMin.x + layout.textX, addMin.y + layout.textY),
                               OS::ui::Col(ImGuiCol_Text), "+");
                if (addClicked) {
                    Outfit fresh;
                    fresh.name = "Outfit " + std::to_string(snap.Count() + 1);
                    WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
                        const int idx = lib.Create(fresh.name);
                        if (idx >= 0) {
                            lib.Activate(static_cast<std::size_t>(idx));
                        }
                    });
                    g_staged = fresh;
                    BeginStagingCurrent(session, g_staged);
                    actualGear   = false;
                    g_dirty       = false;
                    g_history.Reset(g_staged);
                    g_forceSelect = static_cast<int>(snap.Count());  // the new tab
                    EditorStyle::PlayUISound("UIMenuOK");
                }
            }
            // Consume only after FLICK actually reports the requested tab
            // active. Merely rendering it is insufficient: on add-from-
            // Equipped, the old tab is reported first for one frame.
            if (forcedSelectionObserved) {
                g_forceSelect = OutfitTabs::kNoForcedSelection;
            }
            // FittingPolicyScroll supplies edge arrows but does not translate
            // an ordinary vertical mouse wheel into horizontal tab movement.
            // While the pointer is on this row, wheel down selects the
            // next/right tab and wheel up the previous/left tab regardless of
            // whether every tab currently fits. Selection uses the existing
            // one-shot SetSelected path, so ImGui scrolls the chosen tab into
            // view when necessary.
            {
                const ImVec2 windowPos  = FUCK::GetWindowPos();
                const ImVec2 windowSize = FUCK::GetWindowSize();
                const float  padX       = OS::ui::WindowPadding().x;
                const float  stripMinX  = windowPos.x + padX;
                const float  stripMaxX  = windowPos.x + windowSize.x - padX;
                const ImVec2 mouse      = FUCK::GetMousePos();
                const bool stripHovered =
                    mouse.x >= stripMinX && mouse.x <= stripMaxX &&
                    mouse.y >= tabTop && mouse.y <= tabTop + tabHeight;
                const auto request = OutfitTabs::WheelCycleRequest(
                    actualGear ? -1 : CurrentLibrarySnapshot(session).ActiveIndex(),
                    static_cast<int>(snap.Count()), FUCK::GetMouseWheel(),
                    stripHovered);
                if (request) {
                    g_forceSelect =
                        OutfitTabs::ForcedSelectionForActive(*request);
                }
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
                WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
                    newActive = lib.ActiveIndex();
                    if (del < 0 || del >= static_cast<int>(lib.Count())) {
                        return;
                    }
                    newActive =
                        lib.RemoveAndSelectNeighbor(static_cast<std::size_t>(del));
                });
                const auto s2 = CurrentLibrarySnapshot(session);
                if (const auto* a2 = s2.Active()) {
                    g_staged = *a2;
                    BeginStagingCurrent(session, g_staged);
                    actualGear   = false;
                    g_dirty      = false;
                    g_history.Reset(g_staged);
                    g_forceSelect =
                        OutfitTabs::ForcedSelectionForActive(newActive);
                } else {
                    // The deleted outfit must not survive in the staged NPC
                    // channel for the rest of this frame. Equipped gear is the
                    // deterministic landing state for a now-empty library.
                    SelectEquippedGear(session);
                    actualGear = true;
                }
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
        if (!g_showcasesOpen && !actualGear && snap.ActiveIndex() >= 0) {
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
                WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
                    if (lib.ActiveIndex() >= 0) {
                        lib.Rename(static_cast<std::size_t>(lib.ActiveIndex()), g_staged.name);
                    }
                });
            }
            // The tab bar draws EARLIER in the frame than this row, so it reads
            // last frame's value - which is exactly right: the keystroke that
            // renamed the tab is the one whose next frame must not lose it.
            // Read immediately after the field so it is THIS item's state.
            g_renaming = FUCK::IsItemActive();
            if (FUCK::IsItemDeactivatedAfterEdit() && isBlank(g_staged.name)) {
                if (const auto* a = snap.At(static_cast<std::size_t>(snap.ActiveIndex()))) {
                    g_staged.name = a->name;  // last committed (non-blank) name
                }
                if (isBlank(g_staged.name)) {
                    g_staged.name = "Outfit";
                }
                WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
                    if (lib.ActiveIndex() >= 0) {
                        lib.Rename(static_cast<std::size_t>(lib.ActiveIndex()), g_staged.name);
                    }
                });
            }
            FUCK::PopID();
        }
        // Export + Reset + Hide All - per-outfit actions, right-aligned
        // on the name row. (Random moved into the style panel, by the filters.)
        if (!g_showcasesOpen && !actualGear) {
            const std::string trashLabel = Icons::Utf8(Icons::kTrash);
            const bool canDelete =
                OutfitTabs::CanDeleteSaved(snap.ActiveIndex(), snap.Count());
            // Hide All doubles as Show All (OS-47): when every editable slot is
            // already hidden, the button clears them back to equipped gear.
            const std::uint32_t forbidden =
                kNeverHideMask | Settings::GetSingleton().slotBlocklist;
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
                g_footNote = FooterNotice::ExportResult(
                    path, "$FR_Exported"_T, "$FR_ExportFail"_T);
                g_footNoteUntil = FUCK::GetTime() + (path.empty() ? 6.0 : 3.0);
                if (!path.empty()) {
                    PresetStore::RequestRescan();
                    // Make the new reusable preset the source shown next time
                    // the player opens Presets, without interrupting editing.
                    ShowcaseTabs::Request(g_showcaseTabs, ShowcaseTabs::kExported);
                }
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
                // Task 8 note: g_staged = Outfit{} also zeroes weaponEntries_ -
                // this is INCIDENTAL to the whole-outfit replace, not a
                // deliberate extension of Reset to weapons (Reset/Hide All are
                // documented armor-only this stage). It reads correctly either
                // way: "Reset" means back to real gear across the whole
                // outfit, weapons included.
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
                // never-hide slots (shield) + the INI blocklist so the Apply
                // cost counts only real changes. Staged + undoable.
                // Armor-only by construction (loops kSlots, an armor-bit
                // table) - weapons have no hide affordance to bulk-toggle
                // (spec §3a) and are deliberately not extended here.
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

        if (!g_showcasesOpen) {
            FUCK::Separator();
        }
        // Everything below is font-relative: hardcoded pixel heights clipped
        // the footer/labels at larger fonts and resolutions.
        const float footerH = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f +
                              OS::ui::FramePadding().y * 2.0f +
                              std::max(2.0f, FUCK::GetResolutionScale() * 2.0f);

        // ---- Showcases mode: browse + save, then out - the editor body
        // below never runs while the read-only browser is open. ------------
        if (g_showcasesOpen) {
            const auto presets = CurrentPresetSnapshot();
            if (g_showcaseSel >= static_cast<int>(presets.size())) {
                g_showcaseSel = -1;  // a rescan shrank the list
            }
            DrawShowcases(session, presets, footerH);

            FUCK::Separator();
            const bool haveSel = g_showcaseSel >= 0 &&
                                 g_showcaseSel < static_cast<int>(presets.size());
            const bool full           = snap.Count() >= kMaxOutfits;
            const bool collectionOnly = Settings::GetSingleton().collectionOnly;
            PresetOwnership ownership;
            if (haveSel && collectionOnly) {
                ownership = OwnedPieces(presets[static_cast<std::size_t>(g_showcaseSel)].outfit);
            }
            const bool ownsEveryPiece = !collectionOnly || ownership.Complete();
            const bool canSave = EditorGate::CanSaveShowcase(
                haveSel, full, collectionOnly, ownsEveryPiece);
            FUCK::BeginDisabled(!canSave);
            const char* savePresetLabel =
                EditorGate::PresetSaveLabel(TargetIsPlayer());
            if (FUCK::Button(savePresetLabel)) {
                const auto& p      = presets[static_cast<std::size_t>(g_showcaseSel)];
                int         newIdx = -1;
                WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
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
                    session.SetPresetPreviewSuppression(false);
                    BeginStagingCurrent(session, g_staged);
                    g_history.Reset(g_staged);
                    EditorStyle::PlayUISound("UIMenuOK");
                }
            }
            FUCK::EndDisabled();
            if (FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (full) {
                    OS::ui::SetTooltipF("$FR_LibraryFull"_T, kMaxOutfits, kMaxOutfits);
                } else if (haveSel && collectionOnly && !ownsEveryPiece) {
                    const auto& p = presets[static_cast<std::size_t>(g_showcaseSel)];
                    OS::ui::SetTooltipF("$FR_PresetPartial"_T, ownership.owned,
                                        ownership.total, p.file.c_str());
                }
            }
            if (g_showcaseTabs.source == ShowcaseTabs::kExported) {
                const bool liveHaveSel =
                    g_showcaseSel >= 0 &&
                    g_showcaseSel < static_cast<int>(presets.size());
                FUCK::SameLine();
                FUCK::BeginDisabled(!liveHaveSel);
                if (FUCK::Button("Delete Export")) {
                    g_pendingExportDelete =
                        presets[static_cast<std::size_t>(g_showcaseSel)].file;
                    FUCK::OpenPopup("delete_export");
                }
                FUCK::EndDisabled();
            }
            if (FUCK::BeginPopupModal("delete_export", nullptr,
                                      FUCK::WindowFlags::kAutoResize)) {
                FUCK::Text("Delete exported preset '%s'?",
                           g_pendingExportDelete.c_str());
                FUCK::Spacing();
                if (FUCK::Button("$FR_Delete"_T)) {
                    const bool removed =
                        PresetStore::DeleteExport(g_pendingExportDelete);
                    g_footNote = removed ? "Export deleted."
                                         : "Could not delete export. See FittingRoom.log.";
                    g_footNoteUntil = FUCK::GetTime() + 6.0;
                    if (removed) {
                        PresetStore::RequestRescan();
                        g_showcaseSel = -1;
                        g_showcaseHover = -1;
                        g_showcaseHoverPending = -1;
                        BeginStagingCurrent(session, g_staged);
                    }
                    g_pendingExportDelete.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::SameLine();
                if (FUCK::Button("$FR_Cancel"_T)) {
                    g_pendingExportDelete.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndPopup();
            }
            FUCK::SameLine();
            FUCK::AlignTextToFramePadding();  // sit the status text on the button baseline (like Apply/Saved)
            if (full) {
                FUCK::Text("$FR_LibraryFull"_T, kMaxOutfits, kMaxOutfits);
            } else if (g_showcaseSel >= 0 && g_showcaseSel < static_cast<int>(presets.size())) {
                // Re-check g_showcaseSel LIVE (not the stale `haveSel` from before
                // the button handler): saving to the current target sets
                // g_showcaseSel = -1, and reusing haveSel here indexed
                // presets[(size_t)-1] =
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
            FUCK::EndChild();  // editor_fixed_root
            if (editorFont) {
                FUCK::PopFont();
            }
            return;
        }

        // Equipped gear is an immutable baseline, not an empty Outfit. Keep
        // the state in the tab row for deterministic mouse/controller
        // navigation, but replace the pointless disabled editor panels with a
        // concise explanation. The rounded plus remains the creation
        // affordance, including when the saved library is empty.
        if (actualGear) {
            FUCK::BeginChild("equipped_baseline", ImVec2(0, -footerH), true,
                             ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse);
            const float messageW = OS::ui::FontSize() * 30.0f;
            const float startX = std::max(
                OS::ui::WindowPadding().x,
                (FUCK::GetContentRegionAvail().x - messageW) * 0.5f);
            FUCK::SetCursorPosX(startX);
            FUCK::SetCursorPosY(std::max(
                OS::ui::WindowPadding().y,
                FUCK::GetContentRegionAvail().y * 0.30f));
            if (auto* title = FUCK::GetFont(FUCK::Font::kLarge)) {
                FUCK::PushFontScaled(title, g_uiScale);
                FUCK::TextUnformatted("Equipped gear");
                FUCK::PopFont();
            } else {
                FUCK::TextUnformatted("Equipped gear");
            }
            FUCK::SetCursorPosX(startX);
            FUCK::PushTextWrapPos(startX + messageW);
            FUCK::TextDisabled(
                "This is the actor's real equipment with no Fitting Room outfit "
                "applied. Select a saved outfit tab to edit it, or use + to create one.");
            FUCK::PopTextWrapPos();
            FUCK::EndChild();

            FUCK::Separator();
            FUCK::BeginDisabled(true);
            FUCK::Button("$FR_StateEquipped"_T);
            FUCK::EndDisabled();
            DrawFootNote();

            const char* closeLabel = "$FR_Close"_T;
            const float closeW = FUCK::CalcTextSize(closeLabel).x +
                                 OS::ui::FramePadding().x * 2.0f;
            FUCK::SameLine();
            FUCK::SetCursorPosX(FUCK::GetWindowSize().x - closeW -
                                OS::ui::WindowPadding().x);
            if (FUCK::Button(closeLabel)) {
                EditorWindow::RequestClose();
            }
            FUCK::EndChild();  // editor_fixed_root
            if (editorFont) {
                FUCK::PopFont();
            }
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
        // An "(away)" assignee is read-only (Task 8 / spec §5): grey out the whole
        // edit surface so its saved outfit can be viewed but not changed; only
        // Remove assignment stays live. Equipped gear uses the same disabled
        // surface because it is an immutable baseline, not a saved Outfit.
        const bool readOnly = away || actualGear;
        FUCK::BeginDisabled(readOnly);
        // NavFlattened: fold this child's gamepad/keyboard nav into the parent
        // so the D-pad crosses freely between the slot panel, the style browser,
        // the header and the footer - otherwise nav is trapped inside one child
        // window (user: "can't navigate the menu with a controller fully").
        FUCK::BeginChild("slots", ImVec2(slotsW, -footerH),
                          true);

        // Slot highlight while searching: which slots have matching styles.
        const bool searching = g_search[0] != '\0';
        if (searching &&
            (g_lastMatchQuery != g_search ||
             g_lastMatchCollected != browseCollectedOnly ||
             g_lastMatchArmorType != g_armorType || g_lastMatchFavorites != g_favoritesOnly)) {
            g_lastMatchQuery     = g_search;
            g_lastMatchCollected = browseCollectedOnly;
            g_lastMatchArmorType = g_armorType;
            g_lastMatchFavorites = g_favoritesOnly;
            g_matchMask          = StyleCatalog::GetSingleton().MatchMask(
                g_search, browseCollectedOnly, g_armorType, g_favoritesOnly);
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

        // Draw one glyph centered in the fixed bookend box whose top-left is p0.
        // The single source of the icon-box centering math, shared by the
        // interactive glyphButton and the decorative glyphIcon so the two bookend
        // shapes can never drift.
        const auto drawCenteredGlyph = [&](const ImVec2& a_p0, std::uint16_t a_glyph,
                                           const ImVec4& a_col) {
            const std::string g   = Icons::Utf8(a_glyph);
            const ImVec2       gsz = FUCK::CalcTextSize(g.c_str());
            OS::ui::TextAt(ImVec2(a_p0.x + (slotIconW - gsz.x) * 0.5f,
                                  a_p0.y + (slotRowH - OS::ui::FontSize()) * 0.5f),
                           OS::ui::Col(a_col), g.c_str(), g.c_str() + g.size());
        };

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
            drawCenteredGlyph(p0, a_glyph, a_col);
            return clicked;
        };

        // Same fixed bookend box and centered-glyph visual as glyphButton, but
        // with no InvisibleButton underneath - weapon rows have nothing to
        // toggle (hide is deferred to v2, spec §3a), so the leading icon is
        // decorative only. FUCK::Dummy reserves the layout box (so a following
        // SameLine still lines the label up with drawSlotRow's rows) without
        // registering a clickable/hoverable/focusable item - and, submitting
        // with id 0, is never a nav target, so no kNoNav guard is needed here.
        const auto glyphIcon = [&](std::uint16_t a_glyph, const ImVec4& a_col) {
            const ImVec2 p0 = FUCK::GetCursorScreenPos();
            FUCK::Dummy(ImVec2(slotIconW, slotRowH));
            drawCenteredGlyph(p0, a_glyph, a_col);
        };

        // Weapons-accordion leading icon per class, indexed by WeaponClass
        // (WeaponSlots.h enum order == the required UI row order, Sword..
        // Bolts). Melee classes with no distinct free-solid glyph REUSE an
        // already-audited icon above instead of risking an unverified
        // codepoint rendering as tofu (OS-35/OS-62 lesson; see Icons.h for
        // which of the five new codepoints are still pending the screenshot
        // pass called out in the design spec).
        static constexpr std::uint16_t kWeaponIcon[kWeaponClassCount] = {
            Icons::kSkullX,          // Sword - reused: no free-solid blade glyph; closest is
                                     // the "lethal weapon" association of skull-crossbones
            Icons::kHand,            // Dagger - reused: closest is the "hand weapon"/fist glyph
            Icons::kSkull,           // WarAxe - reused: beheading association (echoes the
                                     // existing Decapitate slot's use of the sibling glyph)
            Icons::kCube,            // Mace - reused: no thematic glyph; the documented
                                     // generic fallback
            Icons::kCube,            // Greatsword - reused: same, no thematic glyph available
            Icons::kHammer,          // BattleaxeWarhammer - NEW: literal "warhammer" match
            Icons::kBullseye,        // Bow - NEW: archery-target pun
            Icons::kCrosshairs,      // Crossbow - NEW: precision-aim pun
            Icons::kMagic,           // Staff - reused: a magic staff, ideal existing fit
            Icons::kLocationArrow,   // Arrows - NEW: arrow-shape pun
            Icons::kBolt,            // Bolts - NEW: literal wordplay, ubiquitous free-solid icon
        };
        // Completeness guard: the array is sized to the enum, so a new WeaponClass
        // would zero-fill its slot (glyph 0 = tofu) instead of failing to compile.
        // Reject a missing entry at build time - add its icon above.
        static_assert([] {
            for (auto glyph : kWeaponIcon) {
                if (glyph == 0) {
                    return false;
                }
            }
            return true;
        }(), "kWeaponIcon is missing a glyph for a WeaponClass");

        const auto drawSlotRow = [&](const SlotRow& row) {
            const auto& entry    = g_staged.EntryFor(row.bit);
            const bool  hasEntry = entry.kind != SlotEntry::Kind::kPassthrough;
            const bool  hidden   = entry.kind == SlotEntry::Kind::kHide;
            const bool  allowHide = !StyleRequiresWornItem(row.bit);

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
                ToggleHideSlot(g_staged, row.bit);
                Push();
                EditorStyle::PlayUISound("UIMenuOK");
            };

            FUCK::PushID(static_cast<int>(row.bit));

            // --- slot icon (mouse: click toggles hide / show) ---
            const ImVec4 iconCol = hidden ? kUnfitText : FUCK::GetStyleColorVec4(ImGuiCol_Text);
            if (allowHide) {
                if (glyphButton("##vis", row.icon, iconCol,
                                hidden ? "Show this slot again (restore its piece)"
                                       : "Hide this slot (leave it bare)")) {
                    toggleHide();
                }
            } else {
                glyphIcon(row.icon, iconCol);  // shield styling only; no hide affordance
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
            // Only glow "selected" when armor is the active browser dimension -
            // a weapon selection takes precedence (g_selectedWeapon set), so no
            // armor row should read as selected while browsing weapon styles.
            if (FUCK::Selectable(caption.c_str(), !g_selectedWeapon && g_selectedBit == row.bit, 0,
                                  ImVec2(labelW > 0.0f ? labelW : 0.0f, slotRowH))) {
                g_selectedBit    = row.bit;
                g_selectedWeapon = std::nullopt;  // armor selection takes back precedence
                g_selectedWeaponHand = WeaponHand::Both;
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
                if (allowHide && FUCK::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
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

        // Weapon class row (Task 8): [class icon (decorative)] [label: style
        // name or equipped] [X]. A PARALLEL row idiom to drawSlotRow, not an
        // overload - the shapes differ enough (no hide affordance per §3a, a
        // different Outfit accessor, no armor slot bit) that sharing one
        // lambda would need more branching than duplication costs.
        const auto drawWeaponRow = [&](WeaponClass a_class) {
            const bool selectedClass = g_selectedWeapon == a_class;
            const auto editHand =
                selectedClass && SupportsHandOverrides(a_class)
                    ? g_selectedWeaponHand
                    : WeaponHand::Both;
            const auto& entry =
                g_staged.ResolvedWeaponEntryFor(a_class, editHand);
            const bool hasEntry =
                entry.kind != SlotEntry::Kind::kPassthrough;
            const bool hasHandOverride =
                editHand != WeaponHand::Both &&
                g_staged.WeaponOverrideFor(a_class, editHand).has_value();
            const bool showClear = hasEntry || hasHandOverride;

            const char* label   = FUCK::Translate(ClassLabelKey(a_class));
            std::string caption = std::string(label) + ": ";
            if (editHand == WeaponHand::Right) {
                caption = std::string(label) + " [" + "$FR_HandRight"_T + "]: ";
            } else if (editHand == WeaponHand::Left) {
                caption = std::string(label) + " [" + "$FR_HandLeft"_T + "]: ";
            }
            if (entry.kind == SlotEntry::Kind::kStyle) {
                // Arrows/Bolts resolve through the ammo dimension, every other
                // class through the weapon one (mirrors the render hook's own
                // class-based routing). A resolve failure (plugin gone) shows
                // MissingShort; a resolved form with no name shows "" - same
                // two-case split as the armor caption above, mirrored exactly.
                const bool  isAmmo   = a_class == WeaponClass::Arrows || a_class == WeaponClass::Bolts;
                bool        resolved = false;
                const char* nm       = nullptr;
                if (isAmmo) {
                    if (auto* ammo = StyleRef::ResolveAmmo(entry.style)) {
                        resolved = true;
                        nm       = ammo->GetName();
                    }
                } else if (auto* weap = StyleRef::ResolveWeapon(entry.style)) {
                    resolved = true;
                    nm       = weap->GetName();
                }
                caption += resolved ? (nm ? nm : "") : "$FR_MissingShort"_T;  // nm null guard: std::string + null is UB
            } else {
                // kPassthrough - and, forward-compat, a kHide entry loaded from
                // a hand-edited or forward-dated outfit: hide is deferred to
                // v2 and falls through to vanilla at render (spec §3a), so
                // both read the same here. NEVER "$FR_StateHidden" - a weapon
                // row has no hide affordance and must not claim one is active.
                caption += "$FR_StateEquipped"_T;
            }
            if (editHand != WeaponHand::Both &&
                !g_staged.WeaponOverrideFor(a_class, editHand)) {
                caption += " (";
                caption += "$FR_HandInherited"_T;
                caption += ")";
            }

            const auto clearEntry = [&] {
                CrashGuard::ClearPreviewing();
                if (ClearWeaponHandActionFor(editHand, hasHandOverride) ==
                    WeaponHandClearAction::InheritBoth) {
                    g_staged.ClearWeaponHandOverride(a_class, editHand);
                } else {
                    g_staged.SetWeaponPassthroughForSelection(
                        a_class, editHand);
                }
                Push();
                EditorStyle::PlayUISound("UIMenuCancel");
            };

            FUCK::PushID(static_cast<int>(a_class));

            // --- leading glyph: decorative class icon, no toggle (spec §3a) ---
            glyphIcon(kWeaponIcon[static_cast<std::size_t>(a_class)],
                      FUCK::GetStyleColorVec4(ImGuiCol_Text));

            // --- label: the row's single nav stop; click/A selects the class ---
            FUCK::SameLine(0, OS::ui::ItemSpacing().x);
            const float labelW = FUCK::GetContentRegionAvail().x -
                                 (showClear ? slotIconW + OS::ui::ItemSpacing().x : 0.0f);
            if (FUCK::Selectable(caption.c_str(), g_selectedWeapon == a_class, 0,
                                  ImVec2(labelW > 0.0f ? labelW : 0.0f, slotRowH))) {
                if (g_selectedWeapon != a_class) {
                    g_selectedWeaponHand = WeaponHand::Both;
                }
                g_selectedWeapon = a_class;  // takes precedence over the armor selection
                if (FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad) {
                    g_focusStyleList = true;
                }
            }
            const bool rowFocused = FUCK::IsItemFocused();
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF("$FR_WSlotTip"_T, label, BipedSlotForClass(a_class));
            }

            // Gamepad: Y = clear only - X (hide/show) does not apply to weapon
            // rows, there is nothing to toggle (spec §3a; deliberately left
            // unbound, unlike drawSlotRow's X-face-button handler above).
            if (rowFocused && showClear &&
                FUCK::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
                clearEntry();
            }

            // --- X: explicit hand overrides inherit Both; otherwise use the
            // real weapon. This single compact affordance replaces the old
            // side button that could not fit beside the native stepper.
            if (showClear) {
                const char* clearTip =
                    ClearWeaponHandActionFor(editHand, hasHandOverride) ==
                            WeaponHandClearAction::InheritBoth
                        ? "Remove this hand override and use the Both style."
                        : "Clear to your real weapon";
                FUCK::SameLine(0, OS::ui::ItemSpacing().x);
                if (glyphButton("##clr", Icons::kTimes,
                                FUCK::GetStyleColorVec4(ImGuiCol_Text),
                                clearTip)) {
                    clearEntry();
                }
            }

            // Same-class dual wield stays compact: selecting a one-handed
            // class reveals one native controller-friendly Both/Right/Left
            // stepper directly under that class instead of tripling every
            // weapon row. Both is the backward-compatible fallback.
            if (selectedClass && SupportsHandOverrides(a_class)) {
                // Mirror ORefit's safe scoped-font treatment. FLICK's native
                // EnumStepper owns the label, arrows, hit boxes and controller
                // behavior, but its center helper forces the large font. Give
                // it blank values and paint only the current value with the
                // editor's regular UI-scaled font.
                static const std::vector<std::string> kHandKeys{
                    "$FR_HandBoth", "$FR_HandRight", "$FR_HandLeft"
                };
                static const std::vector<std::string> kBlankHandValues(3);
                FUCK::EnumStepper("$FR_WeaponHand"_T, &g_selectedWeaponHand,
                                  kBlankHandValues, false);
                const int handCur =
                    std::clamp(static_cast<int>(g_selectedWeaponHand), 0, 2);
                const char* handText = FUCK::Translate(
                    kHandKeys[static_cast<std::size_t>(handCur)].c_str());
                const bool   handHovered = FUCK::IsItemHovered();
                const ImVec2 handMin     = FUCK::GetItemRectMin();
                const ImVec2 handMax     = FUCK::GetItemRectMax();
                const ImVec2 handSize    = FUCK::CalcTextSize(handText);
                const ImVec2 handPos{
                    std::floor(handMin.x +
                               (handMax.x - handMin.x - handSize.x) * 0.5f),
                    std::floor(handMin.y +
                               (handMax.y - handMin.y - handSize.y) * 0.5f)
                };
                OS::ui::TextAt(handPos, OS::ui::Col(ImGuiCol_Text), handText);
                if (handHovered) {
                    OS::ui::SetTooltipF(
                        "%s",
                        "Both is the fallback style. Right and Left can use "
                        "different styles or the real weapon.");
                }
            }
            FUCK::PopID();
        };

        // Two accordions replace the old "All slots" toggle: the common set is
        // open by default; the rest live under "Extra Slots" (auto-opened when
        // the outfit already uses one, so an active slot is never buried).
        if (FramedCollapsingHeader("$FR_RegularSlots"_T,
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& row : kSlots) {
                if (row.bit != kBitShield && isRegular(row.bit)) {
                    drawSlotRow(row);
                }
            }
        }
        // Weapons accordion (Task 8): one row per WeaponClass, in enum/UI
        // order (Sword..Bolts - WeaponSlots.h). Default OPEN, unlike Extra
        // Slots' active-only default: this is a brand-new feature and an
        // outfit almost never has a weapon entry yet on a fresh install, so
        // gating on "already in use" would leave it permanently collapsed
        // and undiscoverable for most players.
        // PushID("weapons") scopes the whole accordion so each row's
        // PushID(int(class)) (0..10) can't collide with an armor row's
        // PushID(bit) (also 0..31) in the SAME "slots" child - they are
        // sibling ID scopes without this wrapper.
        //
        // ORDER (user, 2026-07-22): Weapons sits ABOVE Extra Slots. Regular ->
        // Weapons -> Extra reads as most-used to least-used; Extra Slots is the
        // long tail and belongs at the bottom.
        if (FramedCollapsingHeader("$FR_Sec_Weapons"_T,
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            FUCK::PushID("weapons");
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                drawWeaponRow(static_cast<WeaponClass>(i));
            }
            for (const auto& row : kSlots) {
                if (row.bit == kBitShield) {
                    drawSlotRow(row);
                    break;
                }
            }

            FUCK::PopID();
        }
        bool hasActiveExtra = false;
        for (const auto& row : kSlots) {
            if (row.bit != kBitShield && !isRegular(row.bit) &&
                g_staged.EntryFor(row.bit).kind != SlotEntry::Kind::kPassthrough) {
                hasActiveExtra = true;
                break;
            }
        }
        if (FramedCollapsingHeader("$FR_ExtraSlots"_T,
                                   hasActiveExtra ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            for (const auto& row : kSlots) {
                if (row.bit != kBitShield && !isRegular(row.bit)) {
                    drawSlotRow(row);
                }
            }
        }

        // ---- Body accordion (OBody NG) --------------------------------------
        // Require a usable sex-compatible preset snapshot: OBody installed with no bodies for
        // this character should not leave behind a dead category.
        // Followers use actor-scoped OBody policies and a baseline persisted in
        // their NpcRecord. The player mannequin mirrors these staged body fields
        // only while follower editing is active; closing or switching targets
        // restores the player's own saved body.
        if (!g_bodyPresets.empty()) {
            if (FramedCollapsingHeader(
                    "$FR_Sec_Body"_T,
                    AnyBodyEntry(g_staged) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                FUCK::PushID("body");
                if (!ObodyApi::Available()) {
                    // Present but not ready - OBody goes unready around saves.
                    // Say so rather than drawing an empty combo, which would
                    // read as "you have no presets".
                    FUCK::TextDisabled("$FR_BodyBusy"_T);
                } else {
                    // Preset combo. Index 0 is "(no change)" - the empty
                    // preset name - so the list is always offset by one.
                    std::vector<std::string> labels;
                    labels.reserve(g_bodyPresets.size() + 1);
                    labels.emplace_back("$FR_BodyNoChange"_T);
                    for (const auto& p : g_bodyPresets) {
                        labels.push_back(p);
                    }
                    int cur = 0;
                    if (!g_staged.obodyPreset.empty()) {
                        for (std::size_t i = 0; i < g_bodyPresets.size(); ++i) {
                            if (g_bodyPresets[i] == g_staged.obodyPreset) {
                                cur = static_cast<int>(i) + 1;
                                break;
                            }
                        }
                        // cur stays 0 when the saved preset is no longer
                        // installed. The VALUE is deliberately left alone -
                        // silently rewriting it to "(no change)" on open would
                        // destroy the setting just because a mod was
                        // temporarily disabled. The mismatch line below says so.
                    }
                    // The section title already says Body, so call the field
                    // Presets and keep its searchable control on the same row.
                    // The hover tooltip still carries arbitrary long names.
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted("$FR_Presets"_T);
                    FUCK::SameLine();
                    // ComboWithFilter, not Combo: a real setup has HUNDREDS of
                    // presets (field: 140 female / 49 male), unusable as a flat
                    // scroll list.
                    FUCK::SetNextItemWidth(-FLT_MIN);  // fill the remaining row
                    if (FUCK::ComboWithFilter("##body_preset", &cur, labels, 12)) {
                        g_staged.obodyPreset =
                            (cur <= 0) ? std::string{}
                                       : g_bodyPresets[static_cast<std::size_t>(cur - 1)];
                        Push();  // stage, mark dirty, record for undo
                    }
                    // The full name, always - the control clips it whenever the
                    // author was generous with words, and there is no width at
                    // which that stops being true.
                    if (FUCK::IsItemHovered() && !g_staged.obodyPreset.empty()) {
                        OS::ui::SetTooltipF("%s", g_staged.obodyPreset.c_str());
                    }
                    if (cur == 0 && !g_staged.obodyPreset.empty()) {
                        FUCK::TextDisabled("%s", "$FR_BodyPresetMissing"_T);
                        if (FUCK::IsItemHovered()) {
                            OS::ui::SetTooltipF("%s", g_staged.obodyPreset.c_str());
                        }
                    }

                    // Keep FLICK's native EnumStepper: its label, arrows,
                    // controller focus, cycling and hit boxes were the desired
                    // UI. FLICK's CenteredTextWithArrows implementation alone
                    // unconditionally pushes its LARGE font and exposes no
                    // value-font parameter. Feed the native control an empty
                    // center string, then paint only that value in the editor's
                    // already-scaled regular font. Geometry and interaction
                    // remain entirely native; no global/window scale changes.
                    static const std::vector<std::string> kORefitKeys{
                        "$FR_BodyORefitDefault", "$FR_BodyORefitOn",
                        "$FR_BodyORefitOff"
                    };
                    static const std::vector<std::string> kBlankORefitValues(3);
                    if (FUCK::EnumStepper("$FR_BodyORefit"_T,
                                          &g_staged.orefit,
                                          kBlankORefitValues,
                                          false)) {
                        Push();
                    }
                    const int refitCur =
                        std::clamp(static_cast<int>(g_staged.orefit), 0, 2);
                    const char* refitText =
                        FUCK::Translate(kORefitKeys[static_cast<std::size_t>(refitCur)].c_str());
                    const bool   refitHovered = FUCK::IsItemHovered();
                    const ImVec2 refitMin     = FUCK::GetItemRectMin();
                    const ImVec2 refitMax     = FUCK::GetItemRectMax();
                    const ImVec2 refitSize    = FUCK::CalcTextSize(refitText);
                    const ImVec2 refitPos{
                        std::floor(refitMin.x +
                                   (refitMax.x - refitMin.x - refitSize.x) * 0.5f),
                        std::floor(refitMin.y +
                                   (refitMax.y - refitMin.y - refitSize.y) * 0.5f)
                    };
                    OS::ui::TextAt(refitPos, OS::ui::Col(ImGuiCol_Text), refitText);
                    if (refitHovered) {
                        OS::ui::SetTooltipF(
                            "%s", "Auto follows the torso shown by Fitting Room. "
                                  "Use On or Off to override revealing outfits or "
                                  "special OBody rules.");
                    }
                }
                FUCK::PopID();
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
        // Target-switch fit gate (Task 8): the browser reads item->fitsBody /
        // fitReason, which the main-thread re-cache (RefreshFitFor) rewrites on
        // a target switch. Draw NO rows until that finishes (g_fitReady), so the
        // render thread never reads a half-rebuilt fit cache - the same
        // fitsBody-unsynchronized contract EnsureFitCurrent documents. True at
        // open and for a player-only session, so the old path is unchanged. The
        // whole browser body below is enclosed by this guard.
        if (!g_fitReady.load(std::memory_order_acquire)) {
            FUCK::AlignTextToFramePadding();
            FUCK::TextDisabled("$FR_SwitchingTo"_T, g_target.displayName.c_str());
        } else {
        // (Search moved to the shared bar above both panels; "Real gear"/Clear
        // moved to a per-slot button in the slots panel.)
        // These TWO keep the manual alignFar/labelLeft args on purpose: they
        // are one horizontal filter row joined by SameLine below, and alignFar
        // would have both chase the right edge and tear the row apart.
        // Every STACKED checkbox in this mod now takes FLICK's defaults
        // instead, which is what lines them up with the sliders (Fuzzles).
        // Collected is not a filter control: the playstyle is authoritative.
        // Free-form always queries everything; lore-friendly always queries
        // the collection.
        // Favorites filter (OS-22): only styles you have starred. Composes with
        // playstyle visibility and the armor-type filter. Session-local.
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
        // Random (user): rolls a random fitting style for the SELECTED slot (or,
        // with a weapon class selected, within that class - Task 8). Collected
        // used to be a third filter toggle, which made this row too crowded;
        // now that playstyle owns collection visibility, Favorites + Hide
        // unfit + right-aligned Random fit as the intended single row.
        {
            const std::string randLabel = Icons::Utf8(Icons::kDice) + " " + "$FR_Random"_T;
            const float        randW     = FUCK::CalcTextSize(randLabel.c_str()).x +
                                OS::ui::FramePadding().x * 2.0f;
            FUCK::SameLine();
            const float trailingFrameClearance = OS::ui::FramePadding().x;
            FUCK::SetCursorPosX(std::max(FUCK::GetCursorPos().x,
                                          (FUCK::GetCursorPos().x +
                                           FUCK::GetContentRegionAvail().x) -
                                              randW - trailingFrameClearance));
            if (FUCK::Button(randLabel.c_str())) {
                static std::uint32_t s_rng = 0;
                if (s_rng == 0) {
                    s_rng = ::GetTickCount() | 1u;
                }
                s_rng = s_rng * 1664525u + 1013904223u;  // LCG - plenty for a dice roll
                const auto all = StyleCatalog::GetSingleton().Query(
                    g_selectedBit, "", browseCollectedOnly, g_armorType,
                    g_favoritesOnly, g_selectedWeapon);
                std::vector<const StyleItem*> ok;
                for (const auto* s : all) {
                    if (s->fitsBody && s->fitReason != FitReason::kCrashed) {
                        ok.push_back(s);
                    }
                }
                if (!ok.empty()) {
                    CrashGuard::ClearPreviewing();
                    const auto& picked = *ok[s_rng % ok.size()];
                    if (g_selectedWeapon) {
                        g_staged.SetWeaponStyleForSelection(
                            *g_selectedWeapon, picked.key, g_selectedWeaponHand);
                    } else {
                        g_staged.SetStyle(g_selectedBit, picked.key);
                    }
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
        // Armor-type filter (Skyrim has three classes; no "medium") remains on
        // its own row below. It is meaningless for weapons, so it hides while
        // browsing the weapon dimension.
        if (!g_selectedWeapon) {
            const char* kTypeLabels[] = { "$FR_TypeAll"_T, "$FR_TypeLight"_T, "$FR_TypeHeavy"_T,
                                          "$FR_TypeClothing"_T };
            int         typeCombo = g_armorType + 1;  // -1..2 -> 0..3
            FUCK::SetNextItemWidth(OS::ui::FontSize() * 6.5f);
            if (FUCK::Combo("##armortype", &typeCombo, kTypeLabels, IM_ARRAYSIZE(kTypeLabels))) {
                g_armorType = typeCombo - 1;
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_TypeTip"_T);
            }
        }
        // (Column show/hide moved to the header settings gear - OS-30.)
        FUCK::Separator();

        auto results = StyleCatalog::GetSingleton().Query(
            g_selectedBit, g_search, browseCollectedOnly, g_armorType,
            g_favoritesOnly, g_selectedWeapon);
        if (g_hideUnfit) {  // hide body-unfit (red) rows; crashers stay (blocked on click).
                             // No-op for weapons - they are always kFits (no armature to fail).
            std::erase_if(results, [](const StyleItem* s) { return !s->fitsBody; });
        }
        FUCK::TextDisabled(browseCollectedOnly ? "$FR_CountCollected"_T : "$FR_CountStyle"_T,
                            results.size(), results.size() == 1 ? "" : "s");
        if (!TargetIsPlayer() && g_selectedWeapon &&
            !PlayerMannequinHasWeaponClass(*g_selectedWeapon)) {
            FUCK::TextColored(
                kUnfitText,
                "Mannequin preview needs the player to have a matching %s equipped.",
                FUCK::Translate(ClassLabelKey(*g_selectedWeapon)));
            if (FUCK::IsItemHovered()) {
                OS::ui::SetTooltipF(
                    "%s",
                    "Weapon transmog replaces an existing model-load event. "
                    "Fitting Room does not add preview items to your inventory; "
                    "the follower's live preview still applies when that actor "
                    "has a matching equipped or visible weapon.");
            }
        }
        const auto searchLen = std::strlen(g_search);

        // Class column: armor's Light/Heavy/Clothing, or a weapon's class label
        // when the item is a weapon style (Task 8) - the same column serves
        // both dimensions since only one is ever queried into `results` at once.
        const auto classLabel = [](const StyleItem* a_item) -> const char* {
            if (a_item->IsWeapon()) {
                return FUCK::Translate(ClassLabelKey(*a_item->weaponClass));
            }
            switch (a_item->armorType) {
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
                const bool selected = g_selectedWeapon
                                          ? g_staged.ResolvedWeaponEntryFor(
                                                *g_selectedWeapon,
                                                g_selectedWeaponHand).style == item->key
                                          : g_staged.EntryFor(g_selectedBit).style == item->key;
                const bool unfit    = !item->fitsBody;
                FUCK::PushID(static_cast<int>(item->form->GetFormID()));
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
                    FUCK::TextColored(dimCol, "%s", classLabel(item));
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
                        if (g_selectedWeapon) {
                            g_staged.SetWeaponStyleForSelection(
                                *g_selectedWeapon, item->key,
                                g_selectedWeaponHand);
                        } else {
                            g_staged.SetStyle(g_selectedBit, item->key);
                        }
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
            const auto&  cur = g_selectedWeapon ? g_staged.ResolvedWeaponEntryFor(
                                                      *g_selectedWeapon,
                                                      g_selectedWeaponHand)
                                                 : g_staged.EntryFor(g_selectedBit);
            const bool   alreadyChosen =
                cur.kind == SlotEntry::Kind::kStyle && cur.style == frameHoverKey;
            if (frameAnyHover && !alreadyChosen) {
                if (!(frameHoverKey == g_hoverKey)) {
                    if (!(frameHoverKey == g_hoverPending)) {
                        g_hoverPending      = frameHoverKey;
                        g_hoverPendingSince = now;
                    } else if (now - g_hoverPendingSince >= 0.18) {
                        Outfit tmp = g_staged;  // preview a variant WITHOUT editing the buffer
                        if (g_selectedWeapon) {
                            tmp.SetWeaponStyleForSelection(
                                *g_selectedWeapon, frameHoverKey,
                                g_selectedWeaponHand);
                        } else {
                            tmp.SetStyle(g_selectedBit, frameHoverKey);
                        }
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
        }  // end g_fitReady gate (Task 8): browser body only drawn once fit is current
        FUCK::EndChild();
        FUCK::EndDisabled();  // end the current target's read-only wrap

        // Keyboard undo/redo (OS-21): Ctrl+Z = undo, Ctrl+Y or Ctrl+Shift+Z =
        // redo. Gated on !WantTextInput so an active search/rename field keeps
        // ImGui's own text-edit undo. Resolved once per frame, outside the
        // panels, so a button and a shortcut can't both fire on one edit.
        {
            const bool ctrl  = FUCK::IsModifierPressed(FUCK::Modifier::kCtrl);
            const bool shift = FUCK::IsModifierPressed(FUCK::Modifier::kShift);
            if (ctrl && !readOnly && !FUCK::IsAnyItemActive()) {
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
            if (count > 0 && !away && !FUCK::IsAnyItemActive()) {
                const bool next = FUCK::IsKeyPressed(ImGuiKey_GamepadR1, false);
                const bool prev = FUCK::IsKeyPressed(ImGuiKey_GamepadL1, false);
                if (next || prev) {
                    const int newIdx =
                        OutfitTabs::Cycle(actualGear ? -1 : snap.ActiveIndex(), count, next);
                    if (newIdx < 0) {
                        SelectEquippedGear(session);
                        actualGear = true;
                    } else {
                        WithCurrentLibrary(session, [&](OutfitLibrary& lib) {
                            lib.Activate(static_cast<std::size_t>(newIdx));
                        });
                        if (const auto* o = snap.At(static_cast<std::size_t>(newIdx))) {
                            g_staged = *o;
                        }
                        BeginStagingCurrent(session, g_staged);
                        actualGear    = false;
                        g_dirty       = false;
                        g_history.Reset(g_staged);
                        g_forceSelect = newIdx;  // the tab bar follows next frame
                    }
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
            }
        }

        // ---- footer ---------------------------------------------------------
        FUCK::Separator();
        // Lore mode charges gold per CHANGED slot on Apply (preview stays
        // free - the ESO model). Diff the staged outfit against the committed
        // one; without an active outfit everything staged counts as changed.
        std::uint32_t changed    = 0;
        bool          bodyEdited = false;
        {
            static const Outfit kEmpty;
            const auto*         committed = snap.Active();
            const auto&         base      = committed ? *committed : kEmpty;
            changed                       = ChangedSlotCount(base, g_staged);
            bodyEdited                    = BodyDiffers(base, g_staged);
        }
        // Editing a slot back to its committed value undoes the edit: no
        // changed slots means nothing to apply or revert. Without this the
        // dirty flag stays stuck true after a Hide→Show round trip, leaving
        // Apply enabled at "0 gold" and the status reading "Unsaved changes".
        //
        // ⚠ BODY EDITS COUNT HERE BUT NOT IN `changed`. This line is doing two
        // jobs: reconciling the dirty flag, and (via `changed`) pricing Apply.
        // A body-only edit has zero changed SLOTS, so keying dirty on `changed`
        // alone force-cleared the flag the same frame Push() set it - Apply
        // stayed greyed out and the setting could never be committed, which is
        // exactly the "Body is not applied / no Apply to press" report. The
        // gold cost still uses `changed`, so a body preset remains free.
        g_dirty = g_dirty && (changed > 0 || bodyEdited);

        const auto          costPer    = Settings::GetSingleton().goldPerSlot;
        // Gold cost is its own toggle now (was LoreModule::Active()); it needs
        // no lore ESP - the charge math never touches the Seamstone.
        const bool          charging   = Settings::GetSingleton().useGold && costPer > 0;
        // The frame-start snapshot can still name the previous outfit after a
        // tab/controller switch later in this same frame. g_dirty is already
        // false for a saved selection, so pricing only pending edits prevents
        // that harmless snapshot mismatch flashing as a gold charge.
        const std::uint64_t cost =
            PendingGoldCost(g_dirty, charging, changed, costPer);
        const bool          cantAfford = charging && cost > g_gold;

        // Apply's label is just "Apply" now; the gold cost shows beside the
        // button in colour (A1), not baked into the label.
        // Undo / redo (OS-21), in the footer action bar next to Apply (moved
        // here from the slots panel). Keyboard Ctrl+Z / Ctrl+Y also work.
        FUCK::BeginDisabled(!g_history.CanUndo() || readOnly);
        if (FUCK::Button(Icons::Utf8(Icons::kUndo).c_str())) {
            ApplyHistory(session, g_history.Undo());
        }
        FUCK::EndDisabled();
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_UndoTip"_T);
        }
        FUCK::SameLine();
        FUCK::BeginDisabled(!g_history.CanRedo() || readOnly);
        if (FUCK::Button(Icons::Utf8(Icons::kRedo).c_str())) {
            ApplyHistory(session, g_history.Redo());
        }
        FUCK::EndDisabled();
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("$FR_RedoTip"_T);
        }
        FUCK::SameLine();

        if (away) {
            // An "(away)" assignee is READ-ONLY (spec §5): the actor isn't loaded,
            // so there is nothing to preview or kick. Apply is replaced by Remove
            // assignment (view / clear). The assignment still persists and applies
            // on its own the next time that follower loads.
            if (FUCK::Button("$FR_RemoveAssignment"_T)) {
                if (g_target.npc) {
                    session.RemoveNpcAssignment(*g_target.npc);
                }
                EditorStyle::PlayUISound("UIMenuCancel");
                SwitchTarget(session, 0);  // drop back to the player
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_RemoveAssignmentTip"_T);
            }
        } else if (actualGear) {
            FUCK::BeginDisabled(true);
            FUCK::Button("$FR_StateEquipped"_T);
            FUCK::EndDisabled();
        } else {
            FUCK::BeginDisabled(!g_dirty || cantAfford);
            if (FUCK::Button("$FR_Apply"_T)) {
                if (TargetIsPlayer()) {
                    // Player: commit the staged outfit into the active library
                    // outfit and keep editing it (persists to outfits.json).
                    EnsureActiveOutfit(session);
                    session.CommitStaging();
                    session.BeginStaging(g_staged);  // keep editing the same outfit
                } else {
                    // NPC: write the staged outfit into the target's active outfit,
                    // upsert the WHOLE library into the co-save map (NEVER
                    // WithLibrary / outfits.json), and refresh the loaded actor -
                    // UpsertNpcLibrary does NOT kick a refresh, so we must
                    // (Task 6 note). Gold is still charged to the player below.
                    EnsureActiveOutfitNpc();
                    const int idx = g_targetLibrary.ActiveIndex();
                    if (idx >= 0) {
                        if (auto* o = g_targetLibrary.At(static_cast<std::size_t>(idx))) {
                            *o = g_staged;
                        }
                    }
                    if (g_target.npc) {
                        session.UpsertNpcLibrary(*g_target.npc, g_targetLibrary);
                    }
                    session.RequestRefreshActor(g_target.handle);
                    BeginStagingCurrent(session, g_staged);  // keep editing
                }
                g_dirty = false;
                if (cost > 0) {
                    // Gold is ALWAYS the player's, NPC edits included (spec §5 /
                    // USER-CHECK #2): the player is the curator paying the tab.
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
        }

        // Gold cost beside the button, not baked into its label (A1): a coins
        // glyph + the amount in journal-gold. Shown whenever there is a charge,
        // including while Apply is disabled for "Not enough gold", so the price
        // stays visible. Gold is a deliberate highlight (theme audit A8: OK).
        if (g_dirty && cost > 0) {
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
        const char* const statusText = actualGear   ? "$FR_StateEquipped"_T
                                       : !g_dirty     ? "$FR_Saved"_T
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
        FUCK::EndChild();  // editor_fixed_root
        if (editorFont) {
            FUCK::PopFont();
        }
    }

}  // namespace OS::EditorUI
