#include "ScrollReset.h"
#include "RulesUI.h"

#include "ChamferPanel.h"  // the frame's own cut, for panels that meet it
#include "EditorUI.h"      // DrawHideOutfitButton: the one Hide outfit control
#include "IconImages.h"    // the nine-slice frame art
#include "FuckCompat.h"
#include "FoldAll.h"  // expand/collapse all, shared with every other page
#include "Icons.h"
#include "RuleEngine.h"  // AnyRuleCanApply - whether a pin has anything to hold off
#include "RuleModel.h"
#include "Settings.h"  // castHoldSeconds, so the Casting help says the real number
#include "RuleStore.h"
#include "SlotMask.h"
#include "StyleRef.h"
#include "Tutorial.h"  // the page's anchors and its one action
#include "WorldWatch.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Thread contract: Draw runs on the render thread (ImGuiOverlay::PresentThunk,
// same host as EditorUI::Draw). Every rule-state read below goes through
// RuleStore::Merged()/Snapshot() or WorldWatch::GetPublished() - all
// lock-and-copy - and every mutation goes through RuleStore::WithRules or
// RuleStore::SetPackRuleEnabled, followed by
// WorldWatch::RequestEvaluationForUserEdit() - the user-edit variant
// throughout this file, never the plain one, so a hand edit is not held for
// the dwell window that exists to damp world churn.
// No pointer or reference into store state is held across a frame boundary,
// mirroring EditorUI.cpp's own PresetStore/OutfitSession discipline.

namespace OS::RulesUI {

    using namespace OS::Rules;

    namespace {

        // ---- palette (matches EditorUI.cpp's established semantics: red =
        // broken/removed, gold = highlighted/active - see that file's kUnfitText/
        // kGold. Can't reuse those directly - they are anonymous-namespace-local
        // to EditorUI.cpp's translation unit - so the same values are repeated
        // here rather than invented fresh, to keep one visual language.) --------
        constexpr ImVec4 kBadColor  { 0.80f, 0.34f, 0.27f, 1.0f };  // invalid rule / error glyph
        constexpr ImVec4 kLiveColor { 1.0f, 0.82f, 0.2f, 1.0f };    // condition currently true; pin banner

        // ---- slot table (overlay popup rows + the Worn Slot condition's
        // picker) --------------------------------------------------------
        // A plain-literal duplicate of EditorUI.cpp's kSlots table (bit +
        // icon + English label; no translation key). That table is
        // anonymous-namespace-local to EditorUI.cpp's translation unit and
        // cannot be reached from here, and this file's newer UI text is
        // plain-literal throughout (matching EditorUI.cpp's own newest
        // controls, e.g. "Delete Export") rather than adding untranslated
        // keys to dist/Interface/Translations. Kept in the same bit order so
        // the two tables are easy to diff if the slot layout ever changes.
        struct SlotInfo {
            std::uint32_t bit;
            std::uint16_t icon;
            const char*   label;
        };
        constexpr SlotInfo kSlotInfo[] = {
            { 0, Icons::kMask, "Head" },
            { 1, Icons::kHelmet, "Helmet" },
            { 2, Icons::kBody, "Body" },
            { 3, Icons::kMitten, "Hands" },
            { 4, Icons::kHand, "Forearms" },
            { 5, Icons::kGem, "Amulet" },
            { 6, Icons::kRing, "Ring" },
            { 7, Icons::kShoe, "Feet" },
            { 8, Icons::kSocks, "Calves" },
            { 9, Icons::kShield, "Shield" },
            { 10, Icons::kFeather, "Tail" },
            { 11, Icons::kUser, "Long Hair" },
            { 12, Icons::kCrown, "Circlet" },
            { 13, Icons::kEar, "Ears" },
            { 14, Icons::kSmile, "Face / Mouth" },
            { 15, Icons::kCube, "Neck" },
            { 16, Icons::kVest, "Chest (outer)" },
            { 17, Icons::kBack, "Back" },
            { 18, Icons::kCube, "Misc" },
            { 19, Icons::kCube, "Pelvis (outer)" },
            { 20, Icons::kSkull, "Decapitated Head" },
            { 21, Icons::kSkullX, "Decapitate" },
            { 22, Icons::kCube, "Pelvis (under)" },
            { 23, Icons::kCube, "Leg (right)" },
            { 24, Icons::kCube, "Leg (left)" },
            { 25, Icons::kSmile, "Face (alt)" },
            { 26, Icons::kBody, "Chest (under)" },
            { 27, Icons::kCube, "Shoulder" },
            { 28, Icons::kHand, "Arm (left)" },
            { 29, Icons::kHand, "Arm (right)" },
            { 30, Icons::kCube, "Misc 2" },
            { 31, Icons::kMagic, "FX" },
        };

        [[nodiscard]] const SlotInfo* FindSlot(std::uint32_t a_bit) {
            for (const auto& s : kSlotInfo) {
                if (s.bit == a_bit) {
                    return &s;
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::string SlotLabel(std::uint32_t a_bit) {
            const auto* s = FindSlot(a_bit);
            return s ? s->label : ("Slot " + std::to_string(a_bit));
        }

        // ---- condition kind labels (add-condition popup + chip text) -----
        constexpr std::pair<ConditionKind, const char*> kConditionKindLabels[] = {
            { ConditionKind::kLocation, "Location" },
            { ConditionKind::kRegion, "Region" },
            { ConditionKind::kCell, "Cell" },
            { ConditionKind::kInterior, "Interior" },
            { ConditionKind::kWeather, "Weather" },
            { ConditionKind::kTimeOfDay, "Time of Day" },
            { ConditionKind::kCombat, "Combat" },
            { ConditionKind::kCasting, "Casting" },
            { ConditionKind::kSneaking, "Sneaking" },
            { ConditionKind::kSwimming, "Swimming" },
            { ConditionKind::kMounted, "Mounted" },
            { ConditionKind::kDialogue, "Talking" },
            { ConditionKind::kVampire, "Vampire" },
            { ConditionKind::kWornSlot, "Worn Slot" },
            { ConditionKind::kAdvanced, "Advanced" },
        };

        [[nodiscard]] const char* KindLabel(ConditionKind a_kind) {
            for (const auto& [k, label] : kConditionKindLabels) {
                if (k == a_kind) {
                    return label;
                }
            }
            return "?";
        }

        // ---- FormKey helpers -----------------------------------------------
        // Mirrors WorldWatch.cpp's private MakeFormKey / StyleRef::Make: guard
        // on GetFile(0) before GetLocalFormID, never on IsDynamicForm() - a
        // runtime-created (0xFF) form derefs a null file pointer inside
        // GetLocalFormID and crashes (the project's own documented
        // GetLocalFormID pitfall). Duplicated rather than shared because the
        // originals are anonymous-namespace-local to their own translation
        // units.
        [[nodiscard]] bool MakeFormKey(RE::TESForm* a_form, Rules::FormKey& a_out) {
            if (!a_form) {
                return false;
            }
            auto* file = a_form->GetFile(0);
            if (!file) {
                return false;
            }
            a_out.modName     = std::string{ file->GetFilename() };
            a_out.localFormID = a_form->GetLocalFormID();
            return true;
        }

        // "LocTypeDraugrCrypt" -> "draugr crypt". Free-standing rather than
        // shared with anything: this is display-only camel-case splitting,
        // not a data transform any other file needs.
        [[nodiscard]] std::string FriendlyLocType(std::string a_editorId) {
            constexpr std::string_view kPrefix = "LocType";
            if (a_editorId.starts_with(kPrefix)) {
                a_editorId.erase(0, kPrefix.size());
            }
            std::string out;
            out.reserve(a_editorId.size() + 4);
            for (std::size_t i = 0; i < a_editorId.size(); ++i) {
                const unsigned char ch = static_cast<unsigned char>(a_editorId[i]);
                if (i > 0 && std::isupper(ch)) {
                    out += ' ';
                }
                out += static_cast<char>(std::tolower(ch));
            }
            return out.empty() ? "unnamed" : out;
        }

        [[nodiscard]] std::string LocationDisplayName(const Rules::FormKey& a_key) {
            if (a_key.Empty()) {
                return "(not set)";
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* kw = dh ? dh->LookupForm<RE::BGSKeyword>(a_key.localFormID, a_key.modName) : nullptr;
            if (!kw) {
                return "(missing)";
            }
            const char* edid = kw->GetFormEditorID();
            return FriendlyLocType(edid ? edid : "");
        }

        [[nodiscard]] std::string CellDisplayName(const Rules::FormKey& a_key) {
            if (a_key.Empty()) {
                return "(not set)";
            }
            auto* dh   = RE::TESDataHandler::GetSingleton();
            auto* cell = dh ? dh->LookupForm<RE::TESObjectCELL>(a_key.localFormID, a_key.modName) : nullptr;
            if (!cell) {
                return "(missing)";
            }
            if (const char* full = cell->GetFullName(); full && *full) {
                return full;
            }
            if (const char* ed = cell->GetFormEditorID(); ed && *ed) {
                return ed;
            }
            return "(unnamed cell)";
        }

        // A region has no full name, only an editor id (REGN carries no FULL
        // record), so unlike CellDisplayName there is no friendlier string to
        // fall back FROM - the editor id is the best available label.
        [[nodiscard]] std::string RegionDisplayName(const Rules::FormKey& a_key) {
            if (a_key.Empty()) {
                return "(not set)";
            }
            auto* dh  = RE::TESDataHandler::GetSingleton();
            auto* reg = dh ? dh->LookupForm<RE::TESRegion>(a_key.localFormID, a_key.modName)
                           : nullptr;
            if (!reg) {
                return "(missing)";
            }
            const char* edid = reg->GetFormEditorID();
            return (edid && *edid) ? edid : "(unnamed region)";
        }

        // "22:00 to 6:00" - matches the design doc's own example formatting
        // (no leading zero on a single-digit hour, always ":00"/":MM").
        [[nodiscard]] std::string FormatHour(float a_hour) {
            int hh = static_cast<int>(std::floor(a_hour));
            int mm = static_cast<int>(std::lround((a_hour - static_cast<float>(hh)) * 60.0f));
            if (mm >= 60) {
                mm = 0;
                ++hh;
            }
            hh = ((hh % 24) + 24) % 24;
            return mm == 0 ? fmt::format("{}:00", hh) : fmt::format("{}:{:02d}", hh, mm);
        }

        [[nodiscard]] std::string Capitalize(std::string a_s) {
            if (!a_s.empty()) {
                a_s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(a_s[0])));
            }
            return a_s;
        }

        // The lowercase base phrase ("in city", "raining", "22:00 to 6:00",
        // "advanced: getdistance..."); ChipText below applies negate/
        // capitalization on top so both examples in the design doc ("In
        // city", "Not raining") fall out of one rule: negate prepends "Not "
        // to the phrase as-is, otherwise the phrase's first letter is
        // capitalized (touching only that one character, so free-form text -
        // an Advanced clause, a cell/outfit/slot proper name - is never
        // mangled past its first letter).
        [[nodiscard]] std::string ChipPhrase(const Condition& a_c) {
            switch (a_c.kind) {
                case ConditionKind::kLocation:
                    return "in " + LocationDisplayName(a_c.form);
                case ConditionKind::kCell:
                    return "in cell " + CellDisplayName(a_c.form);
                case ConditionKind::kRegion:
                    return "in region " + RegionDisplayName(a_c.form);
                case ConditionKind::kInterior:
                    return "indoors";
                case ConditionKind::kWeather:
                    return a_c.weather == WeatherFlag::kRaining ? "raining" : "snowing";
                case ConditionKind::kTimeOfDay:
                    return FormatHour(a_c.startHour) + " to " + FormatHour(a_c.endHour);
                case ConditionKind::kCombat:
                    return "in combat";
                case ConditionKind::kSneaking:
                    return "sneaking";
                case ConditionKind::kSwimming:
                    return "swimming";
                case ConditionKind::kMounted:
                    return "mounted";
                case ConditionKind::kDialogue:
                    return "talking to someone";
                case ConditionKind::kVampire:
                    return "a vampire";
                case ConditionKind::kCasting:
                    return "casting";
                case ConditionKind::kWornSlot:
                    return "wearing " + SlotLabel(a_c.slotBit);
                case ConditionKind::kAdvanced:
                    return "advanced: " +
                           (a_c.advancedText.empty() ? std::string("(empty)") : a_c.advancedText);
            }
            return "?";
        }

        [[nodiscard]] std::string ChipText(const Condition& a_c) {
            const std::string phrase = ChipPhrase(a_c);
            return a_c.negate ? ("Not " + phrase) : Capitalize(phrase);
        }

        // ---- rule id generation --------------------------------------------
        // Rule::id has no generator elsewhere in the codebase (hand-authored
        // JSON just writes a string); RuleCodec's duplicate-id rule silently
        // drops the LATER rule sharing an id, so a collision here would
        // orphan the just-created rule with no visible error. Checked against
        // the full merged set (save rules AND packs) rather than only
        // g_current, since a pack could plausibly ship an id in the same
        // "rule-..." shape.
        [[nodiscard]] std::string GenerateRuleId(const RuleSet& a_existing) {
            static std::atomic<std::uint32_t> counter{ 0 };
            for (;;) {
                const auto  now = std::chrono::steady_clock::now().time_since_epoch().count();
                std::string candidate =
                    fmt::format("rule-{:x}-{:x}", static_cast<std::uint64_t>(now),
                                counter.fetch_add(1, std::memory_order_relaxed));
                const bool collides =
                    std::ranges::any_of(a_existing, [&](const Rule& r) { return r.id == candidate; });
                if (!collides) {
                    return candidate;
                }
            }
        }

        // ---- "what currently matches" (status strip) -----------------------
        // Reads WorldWatch::GetPublished().decision.matchedRuleId - the
        // engine's own PickWinner result, published on every evaluate
        // regardless of outcome (Task 13 review finding 1). An earlier
        // version of this file re-derived its own winner by walking `merged`
        // with the same enabled/invalid/priority rule PickWinner uses; that
        // was already wrong the day it was written, not just a future drift
        // risk: RuleStore::Merged()'s `invalid` field is never set for a
        // pack rule with a refused Advanced clause (PersistAdvancedValidity
        // only writes it back for save-owned rules - packs are read-only),
        // so a local re-implementation reading Merged() would show a
        // permanently-refused pack rule as the live winner while the engine
        // never applies it.
        [[nodiscard]] const Rule* FindRuleById(const RuleSet& a_rules, const std::string& a_id) {
            if (a_id.empty()) {
                return nullptr;
            }
            for (const auto& r : a_rules) {
                if (r.id == a_id) {
                    return &r;
                }
            }
            return nullptr;
        }

        struct InvalidInfo {
            bool        invalid = false;
            std::string reason;
        };

        // Combines a rule's own persisted invalid/invalidReason (correct and
        // complete for a save-owned rule - missing-outfit and Advanced both
        // land there) with the published per-evaluate Advanced-invalidity
        // map (the only place a PACK rule's Advanced invalidity is visible
        // at all; see WorldWatch::Published::advancedInvalid). Checking the
        // rule's own field first means this never disagrees with a
        // save-owned rule's already-correct display.
        [[nodiscard]] InvalidInfo ResolveInvalid(const Rule& a_rule,
                                                 const std::map<std::string, std::string>& a_advancedInvalid) {
            if (a_rule.invalid) {
                return { true, a_rule.invalidReason };
            }
            if (const auto it = a_advancedInvalid.find(a_rule.id); it != a_advancedInvalid.end()) {
                return { true, it->second };
            }
            return {};
        }

        // ---- glyph helpers (overlay popup rows) -----------------------------
        // A self-contained duplicate of EditorUI.cpp's glyphButton/glyphIcon/
        // drawCenteredGlyph idiom (icon, state, clear) rather than a call into
        // that file: the originals are lambdas local to EditorUI::Draw(),
        // capturing that function's own locals (g_staged, undo history, slot
        // coverage) which have no meaning for a Rule's Overlay map. Same
        // visual shape, independent data model - a different translation
        // unit besides.
        float SlotIconWidth() { return OS::ui::FontSize() * 1.6f; }

        void DrawCenteredGlyph(const ImVec2& a_p0, float a_boxW, float a_boxH, std::uint16_t a_glyph,
                               const ImVec4& a_col) {
            const std::string g   = Icons::Utf8(a_glyph);
            const ImVec2      gsz = FUCK::CalcTextSize(g.c_str());
            OS::ui::TextAt(ImVec2(a_p0.x + (a_boxW - gsz.x) * 0.5f, a_p0.y + (a_boxH - OS::ui::FontSize()) * 0.5f),
                           OS::ui::Col(a_col), g.c_str(), g.c_str() + g.size());
        }

        // Frameless fixed-width glyph button: centered glyph + hover fill,
        // excluded from nav (mouse-only). Returns true on click.
        //
        // ⚠ a_ownHoverFill IS OFF WHEN THE ICON SITS IN A ROW THAT PAINTS ITS
        // OWN BAND, or the row draws two rectangles of two widths and the eye
        // reads the narrow one. Same argument EditorUI.cpp settled for the
        // outfit rows ("we can only have one").
        //
        // ⚠⚠ a_hovered HANDS THE ICON'S HOVER BACK, and a row that answers the
        // right mouse button needs it. The icon is a separate item submitted
        // before the label, so a right click on the SYMBOL lands on something
        // the label's IsItemHovered knows nothing about. EditorUI paid for that
        // one in the field (user 2026-08-17) after its comment had already
        // claimed the whole row was the target.
        bool GlyphButton(const char* a_id, std::uint16_t a_glyph, const ImVec4& a_col, const char* a_tip,
                         bool a_ownHoverFill = true, bool* a_hovered = nullptr) {
            const float  boxW = SlotIconWidth();
            const float  boxH = FUCK::GetFrameHeight();
            const ImVec2 p0   = FUCK::GetCursorScreenPos();
            FUCK::PushItemFlag(FUCK::ItemFlags::kNoNav, true);
            const bool clicked = FUCK::InvisibleButton(a_id, ImVec2(boxW, boxH));
            FUCK::PopItemFlag();
            const bool hovered = FUCK::IsItemHovered();
            if (a_hovered) {
                *a_hovered = hovered;
            }
            if (hovered) {
                FUCK::SetTooltip(a_tip);
                if (a_ownHoverFill) {
                    OS::ui::RectFilled(p0, ImVec2(p0.x + boxW, p0.y + boxH),
                                       OS::ui::Col(ImGuiCol_ButtonHovered), OS::ui::FrameRounding());
                }
            }
            DrawCenteredGlyph(p0, boxW, boxH, a_glyph, a_col);
            return clicked;
        }

        // ---- baseline alignment against a framed widget ----------------------
        //
        // ⚠ Measured off the real item rect, never assumed - the same trap this
        // file's row widths already sprang. FUCK::Checkbox is a composite: it
        // draws a taller frame than FUCK::GetFrameHeight() reports and centers
        // its own label inside that, while handing ImGui's line bookkeeping the
        // STOCK frame-padded text baseline. Plain text placed after SameLine()
        // therefore rides visibly high - that is exactly why "Currently matched"
        // sat above "Auto switching enabled", and why the seed result text sat
        // above the buttons it follows. FUCK::AlignTextToFramePadding() does not
        // fix it, because the number it aligns to is the one that is wrong.
        // Reading the rect back gives whatever the widget actually drew.
        [[nodiscard]] float TextYCenteredOnLastItem() {
            const ImVec2 mn = FUCK::GetItemRectMin();
            const ImVec2 mx = FUCK::GetItemRectMax();
            return mn.y + ((mx.y - mn.y) - OS::ui::FontSize()) * 0.5f;
        }

        // SameLine() snaps the cursor Y back to the line's start (ImGui sets it
        // from CursorPosPrevLine), so the correction has to be re-applied after
        // every SameLine on the strip, not once at the front.
        void SameLineAtY(float a_y) {
            FUCK::SameLine();
            const ImVec2 p = FUCK::GetCursorScreenPos();
            FUCK::SetCursorScreenPos(ImVec2(p.x, a_y));
        }

        // ---- mutation helpers ------------------------------------------------
        // Review finding 4: the caller could not previously tell a hit from
        // a miss, so a pack id (or a rule deleted by a concurrent edit this
        // same frame) paid for a full RequestEvaluation - indistinguishable
        // from a real edit - on every no-op. `found` makes that
        // distinguishable; RequestEvaluation only fires when something
        // actually changed. RuleStore::WithRules itself still unconditionally
        // queues a mirror save even on a no-op callback - that is its own
        // documented contract (RuleStore.h), not something this function
        // overrides.
        void MutateRule(const std::string& a_ruleId, const std::function<void(Rule&)>& a_fn) {
            bool found = false;
            RuleStore::WithRules([&](RuleSet& a_rules) {
                for (auto& r : a_rules) {
                    if (r.id == a_ruleId) {
                        a_fn(r);
                        found = true;
                        return;
                    }
                }
            });
            if (!found) {
                spdlog::debug(
                    "RulesUI: MutateRule('{}') found no save-owned rule with that id (a pack row, "
                    "or deleted by a concurrent edit this same frame); no-op.",
                    a_ruleId);
                return;
            }
            WorldWatch::RequestEvaluationForUserEdit();
        }

        // Set when a rule should be scrolled into view on the next Draw. Lives
        // here rather than beside the list code because both "+ New Rule" and
        // "Copy to my rules" set it, and the copy path is defined well above
        // the list.
        std::string g_scrollToRuleId;

        constexpr const char* kRuleDragType = "FR_RULE";

        // ---- rule order == rule priority ------------------------------------
        //
        // The list is the priority order, top wins, because RuleEngine::PickWinner
        // keeps the rule with the LARGEST priority. So after any move the first
        // row must hold the biggest number.
        //
        // ⚠ Renumbering descends from size()-1 rather than using some fresh
        // range, and that is deliberate: pack rules are compared on this same
        // priority scale but are read-only and cannot be dragged, so spreading
        // save-owned rules into a different range would silently change how the
        // two interleave. 0..n-1 is also what hand-set priorities already looked
        // like, so an existing save's relative order survives the first drag.
        void RenumberByPositionLocked(RuleSet& a_rules) {
            const int n = static_cast<int>(a_rules.size());
            for (int i = 0; i < n; ++i) {
                a_rules[static_cast<std::size_t>(i)].priority = n - 1 - i;
            }
        }

        // ---- the way out of read-only ---------------------------------------
        //
        // A pack rule that is ALMOST right - the right conditions but the wrong
        // slot, which is exactly what the helmet pack hits on a hood, a circlet
        // or a full-face mask on slot 30 - previously left the user nothing to
        // do but switch it off. This lands an editable duplicate in their own
        // rules.
        //
        // A FRESH id, never the pack's: RuleStore::Merged drops the LATER of two
        // rules sharing an id, so reusing it would make the copy and its
        // original fight over which survives the merge, with the loser silently
        // vanishing. packName is cleared because that field is what marks a rule
        // read-only, and invalid/invalidReason are reset because they are loader
        // output about the ORIGINAL, not properties of the copy.
        //
        // The pack original is deliberately left ENABLED. Two rules briefly
        // agreeing is harmless - the copy lands in the save-owned priority range
        // and so outranks it - whereas silently switching off something the user
        // may have wanted to keep is not this button's call to make.
        void CopyPackRuleToOwn(const Rule& a_packRule) {
            const std::string newId = GenerateRuleId(RuleStore::Merged());
            Rule              copy  = a_packRule;
            copy.id = newId;
            // The name is kept verbatim. An earlier version appended " (copy)",
            // which is bookkeeping the user has to delete by hand on every
            // import - and it is not even true after the first edit. The pack
            // original stays visible in its own section, so the two are already
            // told apart by where they sit rather than by their names.
            copy.enabled = true;
            copy.packName.clear();
            copy.invalid = false;
            copy.invalidReason.clear();
            RuleStore::WithRules([c = std::move(copy)](RuleSet& rules) mutable {
                // Top of the list, same as "+ New Rule": an import is a
                // deliberate action and belongs where it can be seen and where
                // it outranks what it was copied from. The save-owned range
                // (0..n-1) already sits above the shipped pack's negative
                // priorities, so it beat the original either way - this just
                // puts it where the eye lands.
                rules.insert(rules.begin(), std::move(c));
                RenumberByPositionLocked(rules);
            });
            WorldWatch::RequestEvaluationForUserEdit();
            g_scrollToRuleId = newId;
        }

        std::optional<std::size_t> IndexOfLocked(const RuleSet& a_rules, const std::string& a_id) {
            for (std::size_t i = 0; i < a_rules.size(); ++i) {
                if (a_rules[i].id == a_id) {
                    return i;
                }
            }
            return std::nullopt;
        }

        // Move a_draggedId into the slot a_targetId currently occupies.
        void ReorderRule(const std::string& a_draggedId, const std::string& a_targetId) {
            if (a_draggedId == a_targetId) {
                return;
            }
            bool moved = false;
            RuleStore::WithRules([&](RuleSet& a_rules) {
                const auto from = IndexOfLocked(a_rules, a_draggedId);
                const auto to   = IndexOfLocked(a_rules, a_targetId);
                if (!from || !to) {
                    return;  // a pack row, or deleted by a concurrent edit this frame
                }
                Rule carried = std::move(a_rules[*from]);
                a_rules.erase(a_rules.begin() + static_cast<std::ptrdiff_t>(*from));
                // Removing an element ahead of the target shifts the target down one.
                std::size_t dest = *to;
                if (*to > *from) {
                    --dest;
                }
                a_rules.insert(a_rules.begin() + static_cast<std::ptrdiff_t>(dest),
                               std::move(carried));
                RenumberByPositionLocked(a_rules);
                moved = true;
            });
            if (moved) {
                WorldWatch::RequestEvaluationForUserEdit();
            }
        }

        // Drop past the last card. Without this the list has no way to express
        // "put it at the bottom": ReorderRule always lands the dragged rule
        // BEFORE its target (see the index math above - true in both
        // directions), so dropping on the last row means second-to-last, and
        // the final slot is unreachable by dragging alone.
        void MoveRuleToEnd(const std::string& a_ruleId) {
            bool moved = false;
            RuleStore::WithRules([&](RuleSet& a_rules) {
                const auto from = IndexOfLocked(a_rules, a_ruleId);
                if (!from || *from + 1 == a_rules.size()) {
                    return;  // a pack row, deleted this frame, or already last
                }
                Rule carried = std::move(a_rules[*from]);
                a_rules.erase(a_rules.begin() + static_cast<std::ptrdiff_t>(*from));
                a_rules.push_back(std::move(carried));
                RenumberByPositionLocked(a_rules);
                moved = true;
            });
            if (moved) {
                WorldWatch::RequestEvaluationForUserEdit();
            }
        }

        // A bright line across the drop edge, drawn on the FOREGROUND list.
        // It has to be the screen/foreground list, not the window list: the row
        // draws its whole-row InvisibleButton first and then rewinds the cursor
        // to paint the real widgets on top, so anything queued here from the
        // drop target would end up UNDER the card it is meant to annotate.
        void DrawDropIndicator(const ImVec2& a_min, const ImVec2& a_max) {
            const float thickness = std::max(2.0f, OS::ui::FontSize() * 0.14f);
            const float y         = a_min.y - OS::ui::ItemSpacing().y * 0.5f;
            FUCK::DrawScreenRectFilled(ImVec2(a_min.x, y - thickness * 0.5f),
                                       ImVec2(a_max.x, y + thickness * 0.5f),
                                       OS::ui::Col(kLiveColor), thickness * 0.5f);
        }

        // Reads a rule id back out of a drag payload. Trusts the declared size
        // over the terminator, so a malformed payload cannot walk off the end.
        [[nodiscard]] std::string PayloadRuleId(const ImGuiPayload& a_payload) {
            if (!a_payload.Data || a_payload.DataSize <= 0) {
                return {};
            }
            const auto*       raw = static_cast<const char*>(a_payload.Data);
            const std::size_t len = strnlen(raw, static_cast<std::size_t>(a_payload.DataSize));
            return std::string(raw, len);
        }

        // Attaches a drop target to whatever widget was just drawn, so a row can
        // be dropped on in more than one place without the caller repeating this.
        //
        // kAcceptPeekOnly (= kAcceptBeforeDelivery | kAcceptNoDrawDefaultRect)
        // rather than a plain accept, for two reasons the old version got wrong:
        //   - The default accept rect is a filled box spanning the whole row,
        //     and it was drawn even when hovering the DRAGGED row itself, where
        //     the drop is a no-op (ReorderRule early-returns on an equal id).
        //     It read as "drop here" over the one target that does nothing.
        //   - Nothing showed WHERE the card would land. Peeking before delivery
        //     lets the insertion line be drawn while the button is still held,
        //     which is the part that makes a reorder feel aimed rather than
        //     guessed.
        // IsDelivery() then separates "hovering" from "released here", so the
        // store is still only touched on an actual drop.
        void AcceptRuleDropOnLastItem(const std::string& a_targetRuleId) {
            const ImVec2 itemMin = FUCK::GetItemRectMin();
            const ImVec2 itemMax = FUCK::GetItemRectMax();
            if (!FUCK::BeginDragDropTarget()) {
                return;
            }
            if (const ImGuiPayload* payload =
                    FUCK::AcceptDragDropPayload(kRuleDragType, FUCK::DragDropFlags::kAcceptPeekOnly)) {
                const std::string draggedId = PayloadRuleId(*payload);
                if (!draggedId.empty() && draggedId != a_targetRuleId) {
                    if (payload->IsDelivery()) {
                        ReorderRule(draggedId, a_targetRuleId);
                    } else {
                        DrawDropIndicator(itemMin, itemMax);
                    }
                }
            }
            FUCK::EndDragDropTarget();
        }

        // The same, for the strip below the last card: drops land at the end.
        void AcceptRuleDropAtEndOnLastItem() {
            const ImVec2 itemMin = FUCK::GetItemRectMin();
            const ImVec2 itemMax = FUCK::GetItemRectMax();
            if (!FUCK::BeginDragDropTarget()) {
                return;
            }
            if (const ImGuiPayload* payload =
                    FUCK::AcceptDragDropPayload(kRuleDragType, FUCK::DragDropFlags::kAcceptPeekOnly)) {
                const std::string draggedId = PayloadRuleId(*payload);
                if (!draggedId.empty()) {
                    if (payload->IsDelivery()) {
                        MoveRuleToEnd(draggedId);
                    } else {
                        DrawDropIndicator(itemMin, itemMax);
                    }
                }
            }
            FUCK::EndDragDropTarget();
        }

        // ---- sticky edit buffers (review finding 2) -------------------------
        //
        // FUCK::DragFloat2/InputText fire every frame the value changes - a
        // held drag or a held key repeat previously called MutateRule (a
        // WithRules call, which unconditionally queues a mirror save) and
        // RequestEvaluation (a full world evaluation) once per RENDERED
        // FRAME, not once per gesture: a 2-second drag at 60fps was ~120
        // rules.json/outfits.json write cycles and 120 evaluations. The
        // idiom this editor already uses elsewhere (EditorUI.cpp's UI-scale
        // slider, its outfit-name field) is to let the widget update a live
        // buffer every frame for DISPLAY, and commit to the store only on
        // FUCK::IsItemDeactivatedAfterEdit().
        //
        // The buffer must be seeded from the store once and then left alone
        // while its field is active - NOT re-derived from a fresh store
        // snapshot every frame, even though the rest of this file does fetch
        // one every frame for display. Two different reasons, one per widget
        // family:
        //   - InputText: a keystroke on frame N updates only that frame's
        //     local copy (the commit is now deferred), so re-deriving from
        //     the STORE on frame N+1 - the deactivation frame, if the user
        //     simply clicks away with no further keystroke - would read the
        //     STALE pre-edit value and commit that instead of what was typed.
        //   - DragFloat2 (or any Drag/Slider): ImGui's own drag math reads
        //     the caller's value fresh every frame and adds only the
        //     not-yet-consumed remainder of the mouse delta (the consumed
        //     part is tracked internally, per active id) - so the caller's
        //     value must already equal what the widget itself output last
        //     frame, or the drag fights its own accumulator.
        //
        // At most one of these is ever "hot": ImGui has only one focused item
        // at a time, so one slot per field kind - keyed by which row/
        // condition currently owns it - is enough; no per-row storage needed.
        struct NameEditState {
            std::string ruleId;  // owner of `buffer`; empty = nothing being edited
            std::string buffer;
        };
        NameEditState g_nameEdit;

        struct ConditionEditState {
            std::string key;  // "ruleId#index" owning the fields below; empty = none
            float       hours[2] = { 0.0f, 0.0f };
            std::string advancedText;
        };
        ConditionEditState g_conditionEdit;

        [[nodiscard]] std::string ConditionKey(const std::string& a_ruleId, std::size_t a_index) {
            return a_ruleId + "#" + std::to_string(a_index);
        }

        // ---- LocType keyword cache (review finding 7) -----------------------
        // Built once, lazily, on first use - not once per popup-open and
        // certainly not once per rendered frame the popup happens to be
        // visible (that walked every loaded keyword form, potentially tens
        // of thousands on a large modlist, on the render thread). Keywords
        // are loaded at kDataLoaded and do not change mid-session, so a
        // single ever-built cache is strictly cheaper than a per-open
        // rebuild and no less correct. `built` is only set once the scan
        // actually ran against a live TESDataHandler, so a too-early call
        // (should not happen - the editor requires a loaded save) retries
        // instead of permanently caching an empty list.
        struct LocTypeCache {
            bool                        built = false;
            std::vector<std::string>    labels;
            std::vector<Rules::FormKey> keys;
        };
        LocTypeCache g_locTypeCache;

        void BuildLocTypeCacheOnce() {
            if (g_locTypeCache.built) {
                return;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            for (auto* kw : dh->GetFormArray<RE::BGSKeyword>()) {
                if (!kw) {
                    continue;
                }
                const char* edid = kw->GetFormEditorID();
                if (!edid || !std::string_view(edid).starts_with("LocType")) {
                    continue;
                }
                Rules::FormKey key;
                if (!MakeFormKey(kw, key)) {
                    continue;
                }
                g_locTypeCache.labels.push_back(FriendlyLocType(edid));
                g_locTypeCache.keys.push_back(key);
            }
            g_locTypeCache.built = true;
        }

        // ---- region cache ----------------------------------------------------
        // Same once-lazily-built shape and the same reasoning as
        // g_locTypeCache: regions load at kDataLoaded and do not change
        // mid-session, and `built` is only set once a live TESDataHandler
        // actually answered, so a too-early call retries rather than caching an
        // empty list forever.
        //
        // Sorted, unlike the LocType cache, because a large modlist carries
        // hundreds of regions in form-array order (which is load order, i.e.
        // arbitrary) and the picker is a flat list. Sorted as PAIRS so the two
        // parallel vectors cannot come apart.
        struct RegionCache {
            bool                        built = false;
            std::vector<std::string>    labels;
            std::vector<Rules::FormKey> keys;
        };
        RegionCache g_regionCache;

        void BuildRegionCacheOnce() {
            if (g_regionCache.built) {
                return;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            std::vector<std::pair<std::string, Rules::FormKey>> rows;
            for (auto* reg : dh->GetFormArray<RE::TESRegion>()) {
                if (!reg) {
                    continue;
                }
                Rules::FormKey key;
                if (!MakeFormKey(reg, key)) {
                    continue;
                }
                const char* edid = reg->GetFormEditorID();
                rows.emplace_back((edid && *edid) ? edid : "(unnamed region)", key);
            }
            std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.first < b.first; });
            g_regionCache.labels.reserve(rows.size());
            g_regionCache.keys.reserve(rows.size());
            for (auto& [label, key] : rows) {
                g_regionCache.labels.push_back(std::move(label));
                g_regionCache.keys.push_back(std::move(key));
            }
            g_regionCache.built = true;
        }

        // ---- overlay popup ------------------------------------------------
        // ⚠⚠ THE OVERLAY POPUP IS OPENED ONE FRAME LATE, ON PURPOSE. Its only
        // entry point is "Other slots..." inside the "+ Effect" popup, and a
        // popup is its own WINDOW: an OpenPopup issued in there hashes the name
        // against the "+ Effect" window's id stack, while the BeginPopup below
        // runs in the rule row's window under PushID(rule.id). Two different
        // ids, so the open never landed and the item read as dead. Same trap the
        // tutorial cards paid for once already ("a popup hashes its name against
        // the id stack, so the two were different popups"), one level up.
        //
        // The rule id is parked here instead and consumed beside the BeginPopup,
        // where the id stack is the one that matters.
        std::string g_openOverlayForRule;

        void DrawOverlayPopup(const std::string& a_ruleId, const Overlay& a_overlay) {
            const std::string popupId = "rules_overlay_" + a_ruleId;
            if (!FUCK::BeginPopup(popupId.c_str())) {
                return;
            }
            FUCK::TextDisabled("%s", "Overlay: hide or restyle single slots on top of the base.");
            FUCK::Separator();
            FUCK::BeginChild("overlay_rows", ImVec2(OS::ui::FontSize() * 20.0f, OS::ui::FontSize() * 18.0f),
                             false);
            // ⚠⚠ THE WHOLE ROW IS ONE TARGET, SYMBOL AND TEXT ALIKE (user
            // 2026-08-27: "I can only select the slots from clicking their
            // symbol"). Left click toggles the slot, right click clears it, and
            // the X button is gone with them: a third hit area in the row's own
            // gutter was what made the row ambiguous in the first place, and the
            // outfit rows settled that argument already.
            for (const auto& slot : kSlotInfo) {
                const auto it       = a_overlay.find(slot.bit);
                const bool hasEntry = it != a_overlay.end();
                const bool hidden   = hasEntry && it->second.kind == SlotEntry::Kind::kHide;
                const bool isStyle  = hasEntry && it->second.kind == SlotEntry::Kind::kStyle;
                // ⚠ "Shown" IS ITS OWN STATE AND USED TO READ AS "No override".
                // A kPassthrough entry is an explicit do-not-hide that beats a
                // lower-priority rule's hide (RuleCodec's own note), so a row
                // carrying one was saying the opposite of what it holds.
                const bool shown     = hasEntry && it->second.kind == SlotEntry::Kind::kPassthrough;
                const bool neverHide = ((kNeverHideMask >> slot.bit) & 1u) != 0;

                FUCK::PushID(static_cast<int>(slot.bit));

                std::string state = "No override";
                if (hidden) {
                    state = "Hidden";
                } else if (shown) {
                    state = "Shown";
                } else if (isStyle) {
                    if (auto* armo = StyleRef::Resolve(it->second.style)) {
                        const char* nm = armo->GetName();
                        state          = std::string("Style: ") + (nm ? nm : "");
                    } else {
                        state = "Style: (missing)";
                    }
                }
                const std::string caption = std::string(slot.label) + ": " + state;

                // ⚠ THE TIP TEACHES THE RIGHT CLICK, because a gesture with no
                // visible affordance is one nobody finds by looking, and taking
                // the X away removed the only mark that said a row could be
                // cleared. It names the slot first for the reason the outfit
                // rows do: the symbol is what the eye lands on and it was the
                // one part of the row that could not say which slot it was.
                std::string tip = std::string(slot.label) + " (slot " +
                                  std::to_string(slot.bit + 30u) + ")";
                if (neverHide) {
                    tip += "\nThis slot cannot be hidden";
                } else {
                    tip += hidden ? "\nLeft click to show it again"
                                  : "\nLeft click to hide it";
                }
                if (hasEntry) {
                    tip += "\nRight click to clear this slot";
                }

                // ⚠⚠ THE BAND IS PAINTED BEFORE ANYTHING IS SUBMITTED INTO IT,
                // and that forces a hand test: RectFilled goes onto the draw
                // list in submission order, so a fill drawn at the end of the
                // row from the items' own hover flags lands on top of the text
                // it is meant to sit behind. At the point it has to be drawn,
                // none of those items exists to be asked.
                //
                // ⚠⚠ FUCK::GetMousePos AND A HAND-WRITTEN RECT TEST, NEVER
                // ImGui::IsMouseHoveringRect. That call CTD'd the editor on its
                // first field run (EXCEPTION_ACCESS_VIOLATION, 2026-08-06): this
                // file draws in FUCK's context inside FUCK.dll, an ImGui:: call
                // from here reads OUR GImGui, and that context has no frame in
                // progress. EditorUI.cpp carries the full note. The cost is the
                // clip test, so a row scrolled out of the child can answer true;
                // harmless for a fill, which FUCK clips anyway, and deliberately
                // NOT used for the clicks below.
                const ImVec2 bandP0 = FUCK::GetCursorScreenPos();
                const float  bandW  = FUCK::GetContentRegionAvail().x;
                const float  rowH   = FUCK::GetFrameHeight();
                const ImVec2 bandP1(bandP0.x + bandW, bandP0.y + rowH);
                const ImVec2 mouse = FUCK::GetMousePos();
                if (!(mouse.x < bandP0.x || mouse.x > bandP1.x || mouse.y < bandP0.y ||
                      mouse.y > bandP1.y)) {
                    OS::ui::RectFilled(bandP0, bandP1, OS::ui::Col(ImGuiCol_HeaderHovered),
                                       OS::ui::FrameRounding());
                }

                const ImVec4 iconCol = hidden ? kBadColor : FUCK::GetStyleColorVec4(ImGuiCol_Text);
                bool         iconHovered = false;
                const bool   iconClicked =
                    GlyphButton("##vis", slot.icon, iconCol, tip.c_str(),
                                /*ownHoverFill*/ false, &iconHovered);
                FUCK::SameLine(0, OS::ui::ItemSpacing().x);

                // ⚠ AN InvisibleButton PLUS DRAWN TEXT, NOT A Selectable. FUCK
                // paints a border around its own widgets from its own theme and
                // no style push reaches it, so a Selectable here would put a
                // second box inside the band. Same idiom GlyphButton above uses.
                //
                // ⚠ Y COMES FROM THE BAND, NOT FROM THE CURSOR. The icon draws
                // its glyph vertically centred through TextAt, which submits an
                // item at that centred Y, and SameLine hands the line's Y back
                // from the last item submitted. Everything after the icon
                // inherits the glyph's offset and rides low unless it is pinned
                // to the band.
                const ImVec2 labelP0(FUCK::GetCursorScreenPos().x, bandP0.y);
                FUCK::SetCursorScreenPos(labelP0);
                const float labelW = bandP1.x - labelP0.x;
                // ⚠ QUERY THE ITEM BEFORE DRAWING THE TEXT: TextAt goes through
                // TextUnformatted, which submits an item of its own, so an
                // IsItemHovered after it asks about the text and not about the
                // button underneath. Width clamped to 1, because ImGui asserts
                // on a zero-size InvisibleButton and a narrow popup can get
                // there.
                FUCK::PushItemFlag(FUCK::ItemFlags::kNoNav, true);
                const bool rowClicked =
                    FUCK::InvisibleButton("##row", ImVec2(labelW > 1.0f ? labelW : 1.0f, rowH));
                FUCK::PopItemFlag();
                const bool rowHovered = FUCK::IsItemHovered();
                if (rowHovered) {
                    FUCK::SetTooltip(tip.c_str());
                }
                OS::ui::TextAt(ImVec2(labelP0.x, bandP0.y + (rowH - OS::ui::FontSize()) * 0.5f),
                               OS::ui::Col(FUCK::GetStyleColorVec4(ImGuiCol_Text)), caption.c_str(),
                               caption.c_str() + caption.size());

                if ((rowClicked || iconClicked) && !neverHide) {
                    MutateRule(a_ruleId,
                               [&](Rule& r) { ToggleOverlayHide(r.overlay, slot.bit); });
                }
                // ⚠ IsMouseClicked ON THE HOVERS, NOT IsItemClicked. A row is
                // several submitted items and the last one submitted is not the
                // one under the cursor. Both terms are ImGui item hovers, so
                // both are clipped to the scrolling child, unlike the band's
                // hand test above.
                if ((rowHovered || iconHovered) && hasEntry && FUCK::IsMouseClicked(1)) {
                    MutateRule(a_ruleId, [&](Rule& r) { r.overlay.erase(slot.bit); });
                }
                FUCK::PopID();
            }
            // ⚠⚠ ONE ITEM AFTER THE LAST ROW, AND IT IS NOT DECORATION.
            // OS::ui::TextAt restores the cursor when it is done, which leaves
            // ImGui holding a SetCursorPos that no item has cleared. Its own
            // note says a following real item clears the flag, and every row
            // but the last one has the next row's button to do it; the last row
            // has EndChild, which is where ImGui says so out loud:
            //
            //   In window '##Popup_.../overlay_rows_...': Code uses
            //   SetCursorPos()/SetCursorScreenPos() to extend window/parent
            //   boundaries. Please submit an item e.g. Dummy() afterwards in
            //   order to grow window/parent boundaries.
            //
            // Field 2026-08-27, and the rows themselves worked throughout: this
            // is the warning asking for exactly this line, at zero size so it
            // costs the child no height.
            FUCK::Dummy(ImVec2(0.0f, 0.0f));
            FUCK::EndChild();
            FUCK::EndPopup();
        }

        // ---- one condition's edit popup -------------------------------------
        void DrawConditionEditorPopup(const std::string& a_ruleId, std::size_t a_index, const Condition& a_c) {
            const std::string popupId = fmt::format("rules_cond_{}_{}", a_ruleId, a_index);
            if (!FUCK::BeginPopup(popupId.c_str())) {
                return;
            }
            FUCK::TextDisabled("%s", KindLabel(a_c.kind));
            FUCK::Separator();

            bool negate = a_c.negate;
            // ⚠ LABEL ONLY (user 2026-08-16: "Not (negate)" reads as jargon).
            // The model field stays `RuleCondition::negate` and RuleCodec.cpp
            // still serialises it under that name: renaming either would orphan
            // every saved rule and every pack on disk.
            if (FUCK::Checkbox("Invert", &negate, false, false)) {
                MutateRule(a_ruleId, [&](Rule& r) {
                    if (a_index < r.conditions.size()) {
                        r.conditions[a_index].negate = negate;
                    }
                });
            }
            FUCK::Spacing();

            switch (a_c.kind) {
                case ConditionKind::kLocation: {
                    BuildLocTypeCacheOnce();
                    const auto& labels = g_locTypeCache.labels;
                    const auto& keys   = g_locTypeCache.keys;
                    int         current = -1;
                    for (std::size_t i = 0; i < keys.size(); ++i) {
                        if (keys[i] == a_c.form) {
                            current = static_cast<int>(i);
                            break;
                        }
                    }
                    FUCK::TextUnformatted(("Currently: " + LocationDisplayName(a_c.form)).c_str());
                    if (labels.empty()) {
                        FUCK::TextDisabled("%s", "No LocType keywords found in the loaded data.");
                    } else {
                        int sel = current;
                        FUCK::SetNextItemWidth(OS::ui::FontSize() * 16.0f);
                        if (FUCK::Combo("##loctype", &sel, labels) && sel != current && sel >= 0 &&
                            sel < static_cast<int>(keys.size())) {
                            const Rules::FormKey chosen = keys[static_cast<std::size_t>(sel)];
                            MutateRule(a_ruleId, [&, chosen](Rule& r) {
                                if (a_index < r.conditions.size()) {
                                    r.conditions[a_index].form = chosen;
                                }
                            });
                        }
                    }
                    break;
                }
                case ConditionKind::kRegion: {
                    BuildRegionCacheOnce();
                    const auto& labels = g_regionCache.labels;
                    const auto& keys   = g_regionCache.keys;
                    int         current = -1;
                    for (std::size_t i = 0; i < keys.size(); ++i) {
                        if (keys[i] == a_c.form) {
                            current = static_cast<int>(i);
                            break;
                        }
                    }
                    FUCK::TextUnformatted(("Currently: " + RegionDisplayName(a_c.form)).c_str());
                    if (labels.empty()) {
                        FUCK::TextDisabled("%s", "No regions found in the loaded data.");
                    } else {
                        int sel = current;
                        FUCK::SetNextItemWidth(OS::ui::FontSize() * 16.0f);
                        // ComboWithFilter, not Combo: a large modlist carries
                        // hundreds of regions and they are named by editor id,
                        // so scrolling to "WeatherTundraMarsh" is hopeless
                        // without typing.
                        if (FUCK::ComboWithFilter("##region", &sel, labels) && sel != current &&
                            sel >= 0 && sel < static_cast<int>(keys.size())) {
                            const Rules::FormKey chosen = keys[static_cast<std::size_t>(sel)];
                            MutateRule(a_ruleId, [&, chosen](Rule& r) {
                                if (a_index < r.conditions.size()) {
                                    r.conditions[a_index].form = chosen;
                                }
                            });
                        }
                    }
                    FUCK::TextDisabled("%s",
                                       "Regions are exterior-only, and overlap: a cell can sit in "
                                       "several at once.");
                    break;
                }
                case ConditionKind::kCell: {
                    FUCK::TextUnformatted(("Currently: " + CellDisplayName(a_c.form)).c_str());
                    const auto cellBtn = ChamferPanel::Button("Use my current cell");
                    if (cellBtn.clicked) {
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        auto* cell   = player ? player->GetParentCell() : nullptr;
                        Rules::FormKey key;
                        if (cell && MakeFormKey(cell, key)) {
                            MutateRule(a_ruleId, [&, key](Rule& r) {
                                if (a_index < r.conditions.size()) {
                                    r.conditions[a_index].form = key;
                                }
                            });
                        }
                    }
                    if (cellBtn.hovered) {
                        FUCK::SetTooltip("Set this condition to the cell you are standing in right now");
                    }
                    break;
                }
                case ConditionKind::kWeather: {
                    std::vector<std::string> labels{ "Raining", "Snowing" };
                    int sel = a_c.weather == WeatherFlag::kSnowing ? 1 : 0;
                    const int before = sel;
                    FUCK::SetNextItemWidth(OS::ui::FontSize() * 10.0f);
                    if (FUCK::Combo("##weather", &sel, labels) && sel != before) {
                        const auto chosen = sel == 1 ? WeatherFlag::kSnowing : WeatherFlag::kRaining;
                        MutateRule(a_ruleId, [&, chosen](Rule& r) {
                            if (a_index < r.conditions.size()) {
                                r.conditions[a_index].weather = chosen;
                            }
                        });
                    }
                    break;
                }
                case ConditionKind::kTimeOfDay: {
                    const std::string key = ConditionKey(a_ruleId, a_index);
                    float             hours[2];
                    if (g_conditionEdit.key == key) {
                        hours[0] = g_conditionEdit.hours[0];
                        hours[1] = g_conditionEdit.hours[1];
                    } else {
                        hours[0] = a_c.startHour;
                        hours[1] = a_c.endHour;
                    }
                    // ⚠ Sliders, not DragFloat2. A drag maps mouse TRAVEL to
                    // value, so at 0.1 per pixel a 0..24 range saturates after
                    // 240 pixels - and each half of a DragFloat2 is only about
                    // six characters wide, so any ordinary mouse movement threw
                    // the hour to 0 or 24. That is the reported symptom exactly.
                    // A slider maps widget WIDTH to the range instead, so the
                    // whole day is always reachable whatever the UI scale.
                    bool              active = false, commit = false, released = false;
                    const char* const kLabels[2] = { "Start hour", "End hour" };
                    for (int i = 0; i < 2; ++i) {
                        FUCK::SetNextItemWidth(OS::ui::FontSize() * 12.0f);
                        FUCK::SliderFloat(kLabels[i], &hours[i], 0.0f, 24.0f, "%.1f");
                        active   = active || FUCK::IsItemActive();
                        commit   = commit || FUCK::IsItemDeactivatedAfterEdit();
                        released = released || FUCK::IsItemDeactivated();
                    }
                    if (active) {
                        g_conditionEdit.key      = key;
                        g_conditionEdit.hours[0] = hours[0];
                        g_conditionEdit.hours[1] = hours[1];
                    }
                    if (commit) {
                        const float start = hours[0];
                        const float end   = hours[1];
                        MutateRule(a_ruleId, [&, start, end](Rule& r) {
                            if (a_index < r.conditions.size()) {
                                r.conditions[a_index].startHour = start;
                                r.conditions[a_index].endHour   = end;
                            }
                        });
                    }
                    // Only drop the sticky buffer once NEITHER slider is being
                    // held - releasing one to grab the other must not discard
                    // the edit in between.
                    if (released && !active) {
                        g_conditionEdit.key.clear();
                    }
                    FUCK::TextDisabled("%s", "A start after the end wraps across midnight.");
                    break;
                }
                case ConditionKind::kWornSlot: {
                    std::vector<std::string> labels;
                    labels.reserve(std::size(kSlotInfo));
                    for (const auto& s : kSlotInfo) {
                        labels.push_back(s.label);
                    }
                    int sel = -1;
                    for (std::size_t i = 0; i < std::size(kSlotInfo); ++i) {
                        if (kSlotInfo[i].bit == a_c.slotBit) {
                            sel = static_cast<int>(i);
                            break;
                        }
                    }
                    const int before = sel;
                    FUCK::SetNextItemWidth(OS::ui::FontSize() * 14.0f);
                    if (FUCK::Combo("##wornslot", &sel, labels) && sel != before && sel >= 0) {
                        const std::uint32_t bit = kSlotInfo[static_cast<std::size_t>(sel)].bit;
                        MutateRule(a_ruleId, [&, bit](Rule& r) {
                            if (a_index < r.conditions.size()) {
                                r.conditions[a_index].slotBit = bit;
                            }
                        });
                    }
                    break;
                }
                case ConditionKind::kAdvanced: {
                    const std::string key = ConditionKey(a_ruleId, a_index);
                    std::string       text =
                        (g_conditionEdit.key == key) ? g_conditionEdit.advancedText : a_c.advancedText;
                    FUCK::SetNextItemWidth(OS::ui::FontSize() * 24.0f);
                    FUCK::InputText("##advtext", &text);
                    if (FUCK::IsItemActive()) {
                        g_conditionEdit.key          = key;
                        g_conditionEdit.advancedText = text;
                    }
                    if (FUCK::IsItemDeactivatedAfterEdit()) {
                        MutateRule(a_ruleId, [&, text](Rule& r) {
                            if (a_index < r.conditions.size()) {
                                r.conditions[a_index].advancedText = text;
                            }
                        });
                    }
                    if (FUCK::IsItemDeactivated()) {
                        g_conditionEdit.key.clear();
                    }
                    // ⚠ This used to advertise "FunctionName [param [param]]",
                    // which the parser refuses outright - parameters were
                    // removed deliberately, because nothing checked a resolved
                    // form's type against what the parameter wanted. Help text
                    // describing a syntax that always errors is worse than none.
                    FUCK::TextDisabled("%s", "FunctionName <op> <value>, parameterless only");
                    FUCK::TextDisabled("%s",
                                       "e.g. IsSneaking == 1, IsInInterior == 0, GetLevel >= 20");
                    FUCK::TextDisabled("%s", "Operators: ==  !=  >=  <=");
                    break;
                }
                case ConditionKind::kDialogue:
                    FUCK::TextDisabled("%s", "True while the dialogue menu is up.");
                    FUCK::TextDisabled("%s", "Nothing else to set. The toggle above is the whole clause.");
                    break;
                case ConditionKind::kVampire:
                    FUCK::TextDisabled("%s", "True while your race carries the Vampire keyword.");
                    FUCK::TextDisabled("%s", "Nothing else to set. The toggle above is the whole clause.");
                    break;
                case ConditionKind::kCasting:
                    // ⚠ THE ONLY CONDITION WHOSE MEANING DEPENDS ON A SETTING,
                    // so the panel says the number rather than describing a
                    // window whose size the reader cannot see. A player whose
                    // robe stays on too long needs to know which key to turn.
                    FUCK::TextDisabled("%s", "True while you are casting a spell.");
                    FUCK::TextDisabled("%s",
                                       "A spell you hold down keeps this true for as long "
                                       "as you hold it.");
                    FUCK::TextDisabled(
                        "It stays true for %.0fs after you stop ([Rules] fCastHoldSeconds), "
                        "so a run of separate casts does not flicker the outfit.",
                        static_cast<double>(Settings::GetSingleton().castHoldSeconds));
                    FUCK::TextDisabled("%s",
                                       "Spells a script gives you do not count. Nothing was cast.");
                    break;
                case ConditionKind::kInterior:
                case ConditionKind::kCombat:
                case ConditionKind::kSneaking:
                case ConditionKind::kSwimming:
                case ConditionKind::kMounted:
                    FUCK::TextDisabled("%s", "Nothing else to set. The toggle above is the whole clause.");
                    break;
            }

            FUCK::Separator();
            if (ChamferPanel::Button("Remove condition").clicked) {
                MutateRule(a_ruleId, [&](Rule& r) {
                    if (a_index < r.conditions.size()) {
                        r.conditions.erase(r.conditions.begin() + static_cast<std::ptrdiff_t>(a_index));
                    }
                });
                FUCK::CloseCurrentPopup();
            }
            FUCK::EndPopup();
        }

        // ---- wrapping flow layout for the condition chips --------------------
        //
        // One chip per line was fine at one clause and unreadable at five: the
        // stack of framed boxes grew taller than the card describing it. This
        // packs chips left to right and breaks at the card's right edge.
        //
        // The wrap decision has to be made BEFORE the item is submitted, since
        // the decision IS whether to call SameLine - so a width has to be
        // predicted. Predict/Commit split keeps that honest: Commit reads the
        // real ImGui rect back, so the pen never drifts on a bad guess, and it
        // calibrates framePad off the FIRST chip actually drawn. The first chip
        // needs no prediction (nothing to wrap against), so from the second
        // chip on the prediction is exact rather than assumed - the same
        // discipline as the row's trailing widths, which this tab learned the
        // hard way when a guessed frame padding pushed buttons off the edge.
        struct ChipFlow {
            float left     = 0.0f;   // screen X a wrapped line restarts at
            float right    = 0.0f;   // screen X the flow must not cross
            float spacing  = 0.0f;
            float pen      = 0.0f;   // screen X just past the last item drawn
            float framePad = -1.0f;  // both sides together; <0 until measured
            bool  first    = true;

            [[nodiscard]] float Predict(const char* a_text) const {
                const float pad = framePad >= 0.0f ? framePad : OS::ui::FramePadding().x * 2.0f;
                return FUCK::CalcTextSize(a_text).x + pad;
            }

            void Place(float a_width) {
                if (first) {
                    first = false;
                    return;  // nothing on this line yet, so nothing to wrap against
                }
                if (pen + spacing + a_width <= right) {
                    FUCK::SameLine();
                }
                // Otherwise fall through: the cursor is already on the next line.
            }

            void Commit(const char* a_text) {
                pen = FUCK::GetItemRectMax().x;
                if (framePad < 0.0f) {
                    framePad = std::max(
                        0.0f, FUCK::GetItemRectSize().x - FUCK::CalcTextSize(a_text).x);
                }
            }
        };

        // ---- one condition chip ---------------------------------------------
        void DrawConditionChip(const std::string& a_ruleId, std::size_t a_index, const Condition& a_c,
                               const WorldSnapshot& a_snapshot, bool a_editable) {
            const bool   live  = Matches(a_c, a_snapshot, a_ruleId, a_index);
            const ImVec4 color = live ? kLiveColor : FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled);
            // ⚠⚠ A PUSHED ID NOW, NOT A "###" SUFFIX, and the swap is forced by
            // the button rather than chosen. The problem it solves is unchanged:
            // the visible text changes every time the condition's negate or
            // params change - that IS the live display - so an id hashed from
            // the label would drift with it, and "###" pinned the id to the
            // stable "chip<index>" part alone.
            //
            // A chamfered button cannot take that spelling. It measures its
            // label with CalcTextSize and draws it with TextUnformatted, and
            // NEITHER strips a hash suffix: ImGui only hides text after "##"
            // inside the widgets that know to ask, and these two calls are the
            // raw ones. The chip would have been sized for "Worn: Steel###chip3"
            // and drawn saying it. PushID over the index gives the same stable
            // identity by the other route, which is the one the primitive
            // leaves open.
            const std::string label = ChipText(a_c);
            FUCK::PushID(static_cast<int>(a_index));

            // ⚠⚠ THE DEMOTION PUSH IS GONE BECAUSE IT NEVER RAN. It read
            // GetStyleColorVec4(ImGuiCol_Button) and pushed the same index back,
            // and both were the WRONG SLOT: FUCK.dll is built on a newer imgui
            // that inserts two entries after TextDisabled, so our Button is 22
            // and FUCK's is 24 (measured four ways, FuckCompat.h). Chips have
            // therefore always drawn at the theme's full button weight, and the
            // 40% alpha the comment describes has never been on screen.
            //
            // ⚠ SO IT IS DROPPED RATHER THAN CORRECTED. Routing it through
            // OS::ui::PushStyleColor would make the demotion real for the first
            // time and change how every rule card reads, which is a look nobody
            // has asked for and nobody has seen. If the hierarchy the comment
            // wanted is still wanted, that is a deliberate change with a field
            // round behind it, not a side effect of a button conversion.
            //
            // The TEXT push stays and is correct: ImGuiCol_Text is 0, below the
            // insertion point, so it needs no translation - and a chamfered
            // button reads the live text colour, so a live chip still carries
            // its gold.
            FUCK::PushStyleColor(ImGuiCol_Text, color);
            const auto chipBtn = ChamferPanel::Button(label.c_str());
            FUCK::PopStyleColor();
            if (chipBtn.hovered) {
                FUCK::SetTooltip(live ? "Currently true" : "Currently false");
            }
            if (chipBtn.clicked && a_editable) {
                FUCK::OpenPopup(fmt::format("rules_cond_{}_{}", a_ruleId, a_index).c_str());
            }
            if (a_editable) {
                DrawConditionEditorPopup(a_ruleId, a_index, a_c);
            }
            // ⚠ AFTER THE POPUP, NOT BEFORE IT. OpenPopup and BeginPopup both
            // hash their id against the CURRENT id stack, so the open above and
            // the begin inside DrawConditionEditorPopup have to see the same
            // one. Popping early would open one popup and begin a different
            // one, which is the trap OS-29 recorded for the outfit tab's X.
            FUCK::PopID();
        }

        // ---- add-condition popup --------------------------------------------
        // ⚠ ONE ROW OWNS THE TUTORIAL RINGS, AND IT IS THE FIRST EDITABLE ONE.
        // Every card draws its own "+ Condition" and "+ Effect", so publishing
        // from all of them would leave the anchor describing whichever rule was
        // drawn last. Reset at the top of Draw, claimed by the first save-owned
        // card, and never claimed by a pack rule: a pack rule is read-only, so
        // asking the player to press a button on one is asking for nothing.
        bool g_tutorialRowClaimed = false;

        void DrawAddConditionButton(const std::string& a_ruleId, bool a_tutorialRow) {
            const std::string popupId = "rules_addcond_" + a_ruleId;
            if (ChamferPanel::Button("+ Condition").clicked) {
                FUCK::OpenPopup(popupId.c_str());
            }
            if (a_tutorialRow) {
                Tutorial::PublishAnchor(Tutorial::Anchor::kRuleAddCondition,
                                        FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            }
            if (!FUCK::BeginPopup(popupId.c_str())) {
                return;
            }
            for (const auto& [kind, label] : kConditionKindLabels) {
                if (FUCK::Selectable(label)) {
                    MutateRule(a_ruleId, [&](Rule& r) {
                        Condition c;
                        c.kind = kind;
                        if (kind == ConditionKind::kTimeOfDay) {
                            // A fresh 0..0 clause is always-false (Matches:
                            // start<=end degenerates to hour>=0 && hour<0);
                            // 0..24 is always-true instead, matching the "an
                            // empty condition list matches" default-open
                            // stance (RuleModel.h) rather than silently
                            // making the whole rule unwinnable until the
                            // user notices and fixes the hours.
                            c.startHour = 0.0f;
                            c.endHour   = 24.0f;
                        }
                        r.conditions.push_back(std::move(c));
                    });
                    Tutorial::NotifyAction(Tutorial::Action::kAddedCondition);
                    FUCK::CloseCurrentPopup();
                }
            }
            FUCK::EndPopup();
        }

        // Draws a rule's condition chips as a wrapped flow, plus (when editable)
        // the trailing "+ Condition" button. Shared by both row kinds so a pack
        // rule's conditions read exactly like a save-owned rule's.
        // a_leftX is where the chips line up, in window space, and it is passed
        // rather than indented to.
        //
        // ⚠ FUCK::Indent() USED TO DO THIS AND IT STOPPED LINING UP WITH
        // ANYTHING. Indent shifts from the window's own indent, which has no
        // relationship to where this row drew its name, so the two only ever
        // agreed by luck. Removing the library row's checkbox moved the name
        // left, the indent did not move with it, and the clauses were left
        // stranded a long way out with a gap in front of them.
        //
        // Aligning to the NAME is what was meant all along: the clauses read as
        // belonging to the rule above them because they start where it starts.
        void DrawConditionFlow(const Rule& a_rule, const WorldSnapshot& a_snapshot, bool a_editable,
                               float a_rightEdge, float a_spacing, float a_leftX) {
            if (a_rule.conditions.empty() && !a_editable) {
                return;  // a pack rule with no clauses has nothing to show
            }
            FUCK::SetCursorPosX(a_leftX);
            ChipFlow flow;
            flow.left    = FUCK::GetCursorScreenPos().x;
            flow.right   = a_rightEdge;
            flow.spacing = a_spacing;
            flow.pen     = flow.left;

            for (std::size_t i = 0; i < a_rule.conditions.size(); ++i) {
                const std::string text = ChipText(a_rule.conditions[i]);
                flow.Place(flow.Predict(text.c_str()));
                DrawConditionChip(a_rule.id, i, a_rule.conditions[i], a_snapshot, a_editable);
                flow.Commit(text.c_str());
            }
            if (a_editable) {
                constexpr const char* kAddLabel = "+ Condition";
                flow.Place(flow.Predict(kAddLabel));
                DrawAddConditionButton(a_rule.id, /*tutorialRow*/ false);
                flow.Commit(kAddLabel);
            }
        }

        // ---- headgear quick control ------------------------------------------
        //
        // The common case, promoted out of the raw overlay map. Hiding a helmet
        // previously meant knowing that biped slot 31 exists, finding it in a
        // 32-row popup, and knowing that "hide" there is what "helmet off"
        // means. The overlay stays for everything else; this is the one shape
        // that earns a control of its own, because an entire class of rule
        // (Helmet Toggle's whole feature set) is nothing but this.
        //
        // ⚠ Slot 30 is deliberately ABSENT. It is the full-face-mask slot
        // (dragon priest masks), and hiding it culls the ENTIRE head node rather
        // than a head part - the headless bug this project already shipped
        // (OS-70, plus the Krosis/Ahzidal reports). Ordinary helmets, hoods and
        // circlets live on 31/41/42, which are safe. Hiding a slot nothing
        // occupies is a no-op, so covering all three costs nothing and spares
        // the user having to know which one their own headgear uses - the exact
        // thing that made the shipped pack look broken.
        constexpr std::uint32_t kHeadgearBits[] = {
            BitForEditorSlot(31),  // hair / helmet
            BitForEditorSlot(41),  // long hair
            BitForEditorSlot(42),  // circlet
        };

        // Three states, and all three are meaningful only because composition is
        // additive: kPassthrough is ignored by Compose, so a higher-priority
        // rule carrying one overrides a lower-priority rule's kHide. That is
        // what makes "Shown" a real instruction rather than just "no opinion".
        enum class HeadgearState { kDefault = 0, kHidden, kShown };

        [[nodiscard]] HeadgearState HeadgearStateOf(const Overlay& a_overlay) {
            // Read off the helmet slot alone. The setter always writes all three
            // together, so the only way they disagree is a hand edit in the
            // Overlay popup - and that popup's own count keeps such an edit
            // visible on the row regardless of what this control says.
            const auto it = a_overlay.find(kHeadgearBits[0]);
            if (it == a_overlay.end()) {
                return HeadgearState::kDefault;
            }
            if (it->second.kind == SlotEntry::Kind::kHide) {
                return HeadgearState::kHidden;
            }
            if (it->second.kind == SlotEntry::Kind::kPassthrough) {
                return HeadgearState::kShown;
            }
            return HeadgearState::kDefault;  // a style entry; the popup owns those
        }

        void SetHeadgearState(const std::string& a_ruleId, HeadgearState a_state) {
            MutateRule(a_ruleId, [a_state](Rule& r) {
                for (const auto bit : kHeadgearBits) {
                    const auto it = r.overlay.find(bit);
                    if (a_state == HeadgearState::kDefault) {
                        // Only clear what this control could have written. A
                        // kStyle entry on a head slot came from the Overlay
                        // popup and is not this control's to throw away.
                        if (it != r.overlay.end() &&
                            it->second.kind != SlotEntry::Kind::kStyle) {
                            r.overlay.erase(it);
                        }
                        continue;
                    }
                    r.overlay[bit] = a_state == HeadgearState::kHidden
                                         ? SlotEntry{ SlotEntry::Kind::kHide, {} }
                                         : SlotEntry{};  // kPassthrough == "show it"
                }
            });
            // Clearing back to default is a removal, not an effect, so only the
            // two states that WRITE one satisfy the tutorial's Then step.
            if (a_state != HeadgearState::kDefault) {
                Tutorial::NotifyAction(Tutorial::Action::kAddedEffect);
            }
        }

        // "Helmet, Circlet", or empty when the player's head slots are
        // bare. Read from the published worn mask, so the card can answer "will
        // this actually do anything to what I am wearing right now" without the
        // user going and looking.
        [[nodiscard]] std::string WornHeadgearSummary(std::uint32_t a_wornMask) {
            std::string out;
            for (const auto bit : kHeadgearBits) {
                if (((a_wornMask >> bit) & 1u) != 0) {
                    if (!out.empty()) {
                        out += ", ";
                    }
                    out += SlotLabel(bit);
                }
            }
            return out;
        }

        // ---- effects: what a rule DOES, as chips -----------------------------
        //
        // The card used to carry three separate controls for this: a base combo,
        // a headgear button, and an Overlay button. The last two reported the
        // SAME slots - a card could read "Hidden" and "Overlay (3)" side by
        // side, which is one idea in two vocabularies and is what made the tab
        // feel convoluted.
        //
        // They are one list. A rule's effect on the character is a base plus an
        // overlay, and both are expressible as chips that read like the
        // condition chips directly above them: the card becomes "WHEN these,
        // THEN those" rather than a row of dissimilar widgets. Nothing changes
        // underneath - these edit exactly the Base and Overlay the engine
        // already consumes.
        struct EffectChip {
            enum class Kind : std::uint8_t { kBase, kHeadgear, kSlot };
            Kind          kind{ Kind::kBase };
            std::uint32_t bit{ 0 };  // kSlot only
            std::string   text;
        };

        [[nodiscard]] std::vector<EffectChip> BuildEffectChips(const Rule& a_rule) {
            std::vector<EffectChip> out;
            if (a_rule.base.kind == BaseKind::kRealGear) {
                out.push_back({ EffectChip::Kind::kBase, 0, "Wear your real gear" });
            } else if (a_rule.base.kind == BaseKind::kOutfit) {
                out.push_back({ EffectChip::Kind::kBase, 0,
                                "Wear \"" + a_rule.base.outfitName + "\"" });
            }
            // kKeep contributes no chip at all: "keep the current look" is what a
            // rule does by saying nothing, and under additive composition that is
            // literally true - a rule with no base leaves the outfit to whichever
            // other rule names one.

            // The three headgear slots collapse into ONE chip when they agree,
            // which is the whole point of the group. A head slot carrying
            // something else (a restyle, say) falls through to its own chip
            // below rather than being swallowed.
            const auto              hg = HeadgearStateOf(a_rule.overlay);
            std::set<std::uint32_t> grouped;
            if (hg != HeadgearState::kDefault) {
                const auto want = hg == HeadgearState::kHidden ? SlotEntry::Kind::kHide
                                                               : SlotEntry::Kind::kPassthrough;
                for (const auto bit : kHeadgearBits) {
                    const auto it = a_rule.overlay.find(bit);
                    if (it != a_rule.overlay.end() && it->second.kind == want) {
                        grouped.insert(bit);
                    }
                }
                out.push_back({ EffectChip::Kind::kHeadgear, 0,
                                hg == HeadgearState::kHidden ? "Hide headgear" : "Show headgear" });
            }

            for (const auto& [bit, entry] : a_rule.overlay) {
                if (grouped.count(bit) != 0) {
                    continue;
                }
                if (entry.kind == SlotEntry::Kind::kHide) {
                    out.push_back({ EffectChip::Kind::kSlot, bit, "Hide " + SlotLabel(bit) });
                } else if (entry.kind == SlotEntry::Kind::kPassthrough) {
                    out.push_back({ EffectChip::Kind::kSlot, bit, "Show " + SlotLabel(bit) });
                } else if (entry.kind == SlotEntry::Kind::kStyle) {
                    const char* nm = nullptr;
                    if (auto* armo = StyleRef::Resolve(entry.style)) {
                        nm = armo->GetName();
                    }
                    out.push_back({ EffectChip::Kind::kSlot, bit,
                                    "Restyle " + SlotLabel(bit) + " to " +
                                        (nm && *nm ? nm : "(missing)") });
                }
            }
            return out;
        }

        void SetBase(const std::string& a_ruleId, Base a_base) {
            MutateRule(a_ruleId, [b = std::move(a_base)](Rule& r) { r.base = b; });
            // The tutorial's "give it something to do" step. Every route that
            // writes an effect notifies, because the card does not care WHICH
            // effect the player reached for, only that the Then row now says
            // something. A no-op unless a step is waiting for exactly this.
            Tutorial::NotifyAction(Tutorial::Action::kAddedEffect);
        }

        void SetSlotEntry(const std::string& a_ruleId, std::uint32_t a_bit,
                          std::optional<SlotEntry> a_entry) {
            MutateRule(a_ruleId, [a_bit, a_entry](Rule& r) {
                if (a_entry) {
                    r.overlay[a_bit] = *a_entry;
                } else {
                    r.overlay.erase(a_bit);
                }
            });
            if (a_entry) {
                Tutorial::NotifyAction(Tutorial::Action::kAddedEffect);
            }
        }

        // The outfit list, offered wherever a base can be chosen. Shared by the
        // "+ Effect" popup and an existing base chip's edit popup so the two
        // cannot drift.
        void DrawOutfitChoices(const std::string& a_ruleId,
                               const std::vector<std::string>& a_libNames,
                               const Base&                     a_current) {
            // ⚠⚠ PushID PER ROW, BECAUSE TWO OUTFITS MAY SHARE A NAME. An
            // ImGui id is hashed from the label, so a library holding two
            // outfits both called "Callisto (Heavy)" draws two Selectables with
            // one id: ImGui's own conflict overlay appears over the game, and
            // hover and click land on whichever it resolved first. Field
            // 2026-08-14, with a screenshot of the two rows and the warning.
            // Nothing about the library forbids a duplicate name, so this is the
            // ordinary case rather than a corrupt one.
            //
            // ⚠ THE INDEX AND NOT THE NAME. Hashing the name is what already
            // collided; the position in the list is the only thing that is
            // distinct by construction.
            //
            // ⚠ Popped before the loop's next turn and before anything else is
            // submitted. A PushID left open would reparent every id after it,
            // which is the trap the swatch popup paid for.
            for (std::size_t i = 0; i < a_libNames.size(); ++i) {
                const auto& name = a_libNames[i];
                const bool  selected =
                    a_current.kind == BaseKind::kOutfit && a_current.outfitName == name;
                FUCK::PushID(static_cast<int>(i));
                if (FUCK::Selectable(("Wear \"" + name + "\"").c_str(), selected)) {
                    Base b;
                    b.kind       = BaseKind::kOutfit;
                    b.outfitName = name;
                    SetBase(a_ruleId, std::move(b));
                    FUCK::CloseCurrentPopup();
                }
                FUCK::PopID();
            }
            if (FUCK::Selectable("Wear your real gear", a_current.kind == BaseKind::kRealGear)) {
                Base b;
                b.kind = BaseKind::kRealGear;
                SetBase(a_ruleId, std::move(b));
                FUCK::CloseCurrentPopup();
            }
        }

        void DrawEffectEditPopup(const std::string& a_ruleId, const EffectChip& a_chip,
                                 const Rule&                     a_rule,
                                 const std::vector<std::string>& a_libNames, std::size_t a_index) {
            const std::string popupId = fmt::format("rules_effect_{}_{}", a_ruleId, a_index);
            if (!FUCK::BeginPopup(popupId.c_str())) {
                return;
            }
            switch (a_chip.kind) {
                case EffectChip::Kind::kBase:
                    DrawOutfitChoices(a_ruleId, a_libNames, a_rule.base);
                    FUCK::Separator();
                    if (FUCK::Selectable("Remove (keep the current look)")) {
                        SetBase(a_ruleId, Base{});  // kKeep
                        FUCK::CloseCurrentPopup();
                    }
                    break;
                case EffectChip::Kind::kHeadgear: {
                    const auto current = HeadgearStateOf(a_rule.overlay);
                    if (FUCK::Selectable("Hide headgear", current == HeadgearState::kHidden)) {
                        SetHeadgearState(a_ruleId, HeadgearState::kHidden);
                        FUCK::CloseCurrentPopup();
                    }
                    if (FUCK::Selectable("Show headgear", current == HeadgearState::kShown)) {
                        SetHeadgearState(a_ruleId, HeadgearState::kShown);
                        FUCK::CloseCurrentPopup();
                    }
                    FUCK::Separator();
                    if (FUCK::Selectable("Remove")) {
                        SetHeadgearState(a_ruleId, HeadgearState::kDefault);
                        FUCK::CloseCurrentPopup();
                    }
                    break;
                }
                case EffectChip::Kind::kSlot: {
                    const auto it      = a_rule.overlay.find(a_chip.bit);
                    const bool hidden  = it != a_rule.overlay.end() &&
                                        it->second.kind == SlotEntry::Kind::kHide;
                    const bool shown   = it != a_rule.overlay.end() &&
                                       it->second.kind == SlotEntry::Kind::kPassthrough;
                    const bool neverHide = ((kNeverHideMask >> a_chip.bit) & 1u) != 0;
                    if (!neverHide && FUCK::Selectable(("Hide " + SlotLabel(a_chip.bit)).c_str(),
                                                       hidden)) {
                        SetSlotEntry(a_ruleId, a_chip.bit,
                                     SlotEntry{ SlotEntry::Kind::kHide, {} });
                        FUCK::CloseCurrentPopup();
                    }
                    if (FUCK::Selectable(("Show " + SlotLabel(a_chip.bit)).c_str(), shown)) {
                        SetSlotEntry(a_ruleId, a_chip.bit, SlotEntry{});
                        FUCK::CloseCurrentPopup();
                    }
                    FUCK::Separator();
                    if (FUCK::Selectable("Remove")) {
                        SetSlotEntry(a_ruleId, a_chip.bit, std::nullopt);
                        FUCK::CloseCurrentPopup();
                    }
                    break;
                }
            }
            FUCK::EndPopup();
        }

        void DrawAddEffectButton(const Rule& a_rule, const std::vector<std::string>& a_libNames,
                                 std::uint32_t a_wornMask, bool a_tutorialRow) {
            const std::string popupId = "rules_addeffect_" + a_rule.id;
            if (ChamferPanel::Button("+ Effect").clicked) {
                FUCK::OpenPopup(popupId.c_str());
            }
            if (a_tutorialRow) {
                Tutorial::PublishAnchor(Tutorial::Anchor::kRuleAddEffect,
                                        FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            }
            if (!FUCK::BeginPopup(popupId.c_str())) {
                return;
            }
            if (a_rule.base.kind == BaseKind::kKeep) {
                DrawOutfitChoices(a_rule.id, a_libNames, a_rule.base);
                FUCK::Separator();
            }
            if (HeadgearStateOf(a_rule.overlay) == HeadgearState::kDefault) {
                if (FUCK::Selectable("Hide headgear")) {
                    SetHeadgearState(a_rule.id, HeadgearState::kHidden);
                    FUCK::CloseCurrentPopup();
                }
                if (FUCK::Selectable("Show headgear")) {
                    SetHeadgearState(a_rule.id, HeadgearState::kShown);
                    FUCK::CloseCurrentPopup();
                }
                if (FUCK::IsItemHovered()) {
                    // The one place the slot detail still belongs: it explains
                    // what "headgear" covers, and answers "will this touch what
                    // I have on" from the live worn mask.
                    const std::string worn = WornHeadgearSummary(a_wornMask);
                    FUCK::SetTooltip(
                        ("Helmets, hoods and circlets (biped slots 31, 41 and 42).\n"
                         "You are currently wearing: " +
                         (worn.empty() ? std::string("nothing on those slots") : worn) +
                         "\nFull-face masks on slot 30 are not covered")
                            .c_str());
                }
                FUCK::Separator();
            }
            if (FUCK::Selectable("Other slots...")) {
                FUCK::CloseCurrentPopup();
                // ⚠ NOT OpenPopup HERE. See g_openOverlayForRule: this call site
                // is inside the "+ Effect" popup's own window, so the id it would
                // hash is not the id DrawOverlayPopup begins.
                g_openOverlayForRule = a_rule.id;
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("Hide or restyle any individual slot");
            }
            FUCK::EndPopup();
        }

        // The base picker combo lived here. It is gone: choosing an outfit is
        // now an effect chip like any other, and the outfit list it used to own
        // is DrawOutfitChoices above, shared by the "+ Effect" popup and an
        // existing base chip's edit popup so the two cannot drift apart.

        // ---- vertical room on the rule cards ---------------------------------
        //
        // A skin that pads its frames tighter than Vel'dun stacks the cards and
        // their chip rows on top of each other (Fuzzles, 2026-09-02, with a
        // screenshot: "this needs some padding"). Both gaps below are FLOORED
        // rather than set, so a roomy theme keeps its own numbers and a tight
        // one gets no less than this, and both are in em so the editor's own
        // scale carries them.
        //
        // ⚠ STYLE VARS, NOT INDENTS. FLICK's default indent spacing is 106.7
        // and a bare FUCK::Indent() is a cliff; these move nothing sideways.
        // ImGuiStyleVar_ indices are the same on both sides of the FUCK
        // boundary (FuckCompat.h: only the colour enum is shifted).
        [[nodiscard]] float RowRoom() { return OS::ui::FontSize() * 0.3f; }

        // Floor ItemSpacing.y for one scope: every line break inside a card,
        // which is the header row to When, When to Then and each wrapped chip
        // line, gets at least RowRoom(). The x half is left exactly as it is,
        // because the row's measured widths spend it.
        //
        // RAII rather than a push and a pop at each end, because the editable
        // row returns early when its delete button fires and a pop that had to
        // be remembered on that path would be forgotten on the next one.
        struct CardSpacing {
            CardSpacing() {
                const ImVec2 cur = OS::ui::ItemSpacing();
                FUCK::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                   ImVec2(cur.x, std::max(cur.y, RowRoom())));
            }
            ~CardSpacing() { FUCK::PopStyleVar(); }
            CardSpacing(const CardSpacing&)            = delete;
            CardSpacing& operator=(const CardSpacing&) = delete;
        };

        // ---- one save-owned (editable) rule row -----------------------------
        void DrawEditableRule(const Rule& a_rule, const WorldSnapshot& a_snapshot,
                             const std::vector<std::string>& a_libNames,
                             const std::map<std::string, std::string>& a_advancedInvalid,
                             bool a_isWinner) {
            FUCK::PushID(a_rule.id.c_str());
            const CardSpacing roomy;
            // First editable card of the frame carries the tutorial's rings.
            // Claimed here rather than by index, because pack rules are drawn
            // through a different function and must never claim it: a pack rule
            // is read-only, so ringing its "+ Condition" asks for nothing.
            const bool tutorialRow = !g_tutorialRowClaimed;
            g_tutorialRowClaimed   = true;

            // Row widths are measured, not estimated. The first attempt guessed
            // frame padding as half an em and guessed FUCK::Stepper's width from
            // its label, and both were wrong enough that the Overlay and delete
            // buttons still ran off the right edge. Here the grip is drawn first
            // and its real size read back, which gives this theme's actual frame
            // padding, and every trailing widget is one this file draws itself,
            // so its width is known rather than assumed.
            const float em      = OS::ui::FontSize();
            const float spacing = OS::ui::ItemSpacing().x;
            const float rowLeft = FUCK::GetCursorPos().x;
            const float avail   = FUCK::GetContentRegionAvail().x;

            const std::string gripTxt  = Icons::Utf8(Icons::kGrip);
            const std::string trashTxt = Icons::Utf8(Icons::kTrash);
            const std::string warnTxt  = Icons::Utf8(Icons::kWarning);
            // ⚠ The drop target has to be ONE item spanning the whole row, not a
            // target attached to each widget in turn. Attaching them per widget
            // was tried and only the grip ever accepted a drop, because
            // BeginDragDropTarget binds to the last submitted item and an
            // InputText or Combo does not register the hovered-rect status it
            // needs while another widget owns the active id mid-drag.
            //
            // An invisible button submitted FIRST does register it: the flag the
            // drop target reads is rect-based, so it is true anywhere over the
            // row no matter what gets drawn on top afterwards. AllowOverlap lets
            // the real widgets keep their own clicks and hover.
            const ImVec2 rowOrigin = FUCK::GetCursorScreenPos();
            FUCK::InvisibleButton("##rowdrop", ImVec2(std::max(1.0f, avail), FUCK::GetFrameHeight()),
                                  ImGuiButtonFlags_AllowOverlap);
            AcceptRuleDropOnLastItem(a_rule.id);
            FUCK::SetCursorScreenPos(rowOrigin);  // rewind and draw the row on top

            // Drag handle. BeginDragDropSource binds to the item drawn just
            // before it, so the grip has to be a real hit-testable widget and
            // has to come first.
            //
            // ⚠⚠ THE LAST FUCK::Button IN THE MOD, AND IT STAYS ONE. Every
            // other site converted to ChamferPanel::Button; this one cannot,
            // for two reasons that are both silent when broken.
            //
            // BeginDragDropSource binds to the LAST SUBMITTED ITEM's id, and a
            // chamfered button seals its footprint with a Dummy, whose id is 0.
            // Converting this kills rule reordering outright with nothing
            // logged and nothing to see except a grip that no longer drags.
            //
            // And the row's whole width arithmetic is measured off it: gripW
            // below is this button's own size, framePad is derived from it, and
            // buttonW() spends that on every control further along the row. A
            // chamfered button's rect is a different number, so the row would
            // relay itself against a padding it does not have.
            //
            // The heights still agree, which is why the mixture is not a
            // crooked row: ChamferPanel::FrameWidgetHeight MEASURES a live
            // FLICK widget, and FUCK::Button is one. Only the corner differs.
            FUCK::PushStyleColor(ImGuiCol_Text, FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled));
            FUCK::Button(gripTxt.c_str());
            FUCK::PopStyleColor();
            const float gripW = FUCK::GetItemRectSize().x;
            // What the theme really pads a framed widget by, both sides.
            const float framePad =
                std::max(0.0f, (gripW - FUCK::CalcTextSize(gripTxt.c_str()).x) * 0.5f);
            const auto buttonW = [&](const char* a_text) {
                return FUCK::CalcTextSize(a_text).x + framePad * 2.0f;
            };

            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("Drag to reorder, or use the arrows. Higher in the list wins");
            }
            // The card in flight. The stock preview was a bare Text in an
            // unpadded default tooltip window, which landed square on top of
            // the row being dragged and read as a stray label rather than as
            // the thing under the cursor. Padding is pushed around
            // BeginDragDropSource because the preview window is opened INSIDE
            // that call and reads WindowPadding at Begin time; popping outside
            // the if keeps the push/pop balanced when no drag is active.
            FUCK::PushStyleVar(ImGuiStyleVar_WindowPadding,
                               ImVec2(em * 0.55f, em * 0.35f));
            if (FUCK::BeginDragDropSource(FUCK::DragDropFlags::kNone)) {
                FUCK::SetDragDropPayload(kRuleDragType, a_rule.id.c_str(), a_rule.id.size() + 1);
                FUCK::PushStyleColor(ImGuiCol_Text, kLiveColor);
                FUCK::TextUnformatted((gripTxt + "  " + a_rule.name).c_str());
                FUCK::PopStyleColor();
                FUCK::EndDragDropSource();
            }
            FUCK::PopStyleVar();
            FUCK::SameLine();

            bool enabled = a_rule.enabled;
            if (FUCK::Checkbox("##enabled", &enabled, false, false)) {
                MutateRule(a_rule.id, [&](Rule& r) { r.enabled = enabled; });
            }
            FUCK::SameLine();

            // ---- state, said in words -------------------------------------
            //
            // ⚠ The gold wash alone defined "applying right now" by CONTRAST,
            // which left a rule whose conditions simply are not met with no
            // signal at all. A first-time user reads an unmarked card as broken
            // rather than as waiting - reported from a fresh pair of eyes, and
            // it is exactly the wrong conclusion to invite, because most rules
            // are waiting most of the time BY DESIGN.
            //
            // So no state is defined by absence any more. Every card says which
            // of the three it is, and the wash is reinforcement for scanning
            // rather than the only carrier of the fact.
            const char*  stateTxt = RuleStateWord(a_rule.enabled, a_isWinner);
            const ImVec4 stateCol = (a_rule.enabled && a_isWinner)
                                        ? kLiveColor
                                        : FUCK::GetStyleColorVec4(ImGuiCol_TextDisabled);
            // Reserve the widest so the row does not shift as a rule starts and
            // stops matching - which it does constantly, and a twitching row
            // would be worse than the problem being fixed.
            float stateWidest = 0.0f;
            for (const char* s : { "Off", "Active", "Waiting" }) {
                stateWidest = std::max(stateWidest, FUCK::CalcTextSize(s).x);
            }

            // The header row carries IDENTITY only now - grip, enabled, name,
            // state, warning, delete. The base picker, the headgear button and
            // the Overlay button all moved into the "Then" flow below, because
            // they were three different widget shapes for one idea (what this
            // rule does) and two of them reported the same slots twice over.
            // Split so the warning slot and the delete button can be pinned
            // absolutely, independent of how wide the state word happens to be.
            // Reserving the widest label positioned the GROUP correctly but the
            // text was still submitted at its own width, so a SameLine after it
            // put the trash somewhere different for "Off" than for "Waiting" -
            // and a delete button that moves under the cursor as a rule starts
            // matching is a misclick waiting to happen.
            const float tailAfterState = FUCK::CalcTextSize(warnTxt.c_str()).x +
                                         buttonW(trashTxt.c_str()) + spacing;
            const float trailing       = stateWidest + spacing + tailAfterState;
            const float consumed = FUCK::GetCursorPos().x - rowLeft;
            const float fields   = std::max(em * 8.0f, avail - consumed - trailing - spacing * 2.0f);

            {
                std::string name =
                    (g_nameEdit.ruleId == a_rule.id) ? g_nameEdit.buffer : a_rule.name;
                FUCK::SetNextItemWidth(fields);
                // The winning rule's name in the same gold the status strip uses
                // for it, so "Currently matched: X" and the card claiming to be
                // X are visibly the same claim. Names are not unique - the
                // screenshot that opened this pass had two rules called "New
                // Rule" - so the strip alone could not identify a row.
                if (a_isWinner) {
                    FUCK::PushStyleColor(ImGuiCol_Text, kLiveColor);
                }
                FUCK::InputText("##name", &name);
                if (a_isWinner) {
                    FUCK::PopStyleColor();
                }
                if (FUCK::IsItemActive()) {
                    g_nameEdit.ruleId = a_rule.id;
                    g_nameEdit.buffer = name;
                }
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    MutateRule(a_rule.id, [&](Rule& r) { r.name = name; });
                }
                if (FUCK::IsItemDeactivated()) {
                    g_nameEdit.ruleId.clear();
                }
            }
            // Pin the trailing group flush to the right edge rather than letting
            // it land wherever the name field happened to end.
            FUCK::SameLine();
            FUCK::SetCursorPosX(rowLeft + avail - trailing);

            {
                FUCK::AlignTextToFramePadding();
                FUCK::TextColored(stateCol, "%s", stateTxt);
                if (FUCK::IsItemHovered()) {
                    // "Waiting" gets the longest explanation on purpose: it is
                    // the state that was being misread, and the reassurance is
                    // the whole point of saying it out loud.
                    // ⚠ NO FULL STOP ON THE LAST LINE. Standing rule: a tooltip
                    // never ends in one, and these three were the exception.
                    FUCK::SetTooltip(
                        !a_rule.enabled
                            ? "Off. This rule is switched off and will never apply.\n"
                              "Use the checkbox on the left to turn it back on"
                        : a_isWinner
                            ? "Active. This rule is switched on AND its conditions are true "
                              "right now,\nso it is part of what you are wearing. Several rules "
                              "can be active at\nonce, and they combine"
                            : "Waiting. This rule is switched on, its conditions are just not "
                              "true at\nthe moment, so it is standing by. Nothing is wrong with "
                              "it: most rules\nare waiting most of the time. Check its When row "
                              "to see what it is\nwaiting for");
                }
                // Not a bare SameLine: jump to the reserved position so the
                // warning slot and the delete button sit at the same X on every
                // row, whatever this row's state word is.
                FUCK::SameLine();
                FUCK::SetCursorPosX(rowLeft + avail - tailAfterState);
            }

            if (const auto info = ResolveInvalid(a_rule, a_advancedInvalid); info.invalid) {
                FUCK::TextColored(kBadColor, "%s", warnTxt.c_str());
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip(info.reason.empty() ? "This rule is invalid" : info.reason.c_str());
                }
            } else {
                // Hold the slot so the row does not shift when a rule flips valid.
                FUCK::Dummy(ImVec2(FUCK::CalcTextSize(warnTxt.c_str()).x, 1.0f));
            }

            FUCK::SameLine();
            const auto deleteBtn = ChamferPanel::IconButton(Icons::Utf8(Icons::kTrash).c_str());
            if (deleteBtn.clicked) {
                const std::string id = a_rule.id;
                RuleStore::WithRules([id](RuleSet& rules) {
                    std::erase_if(rules, [&](const Rule& r) { return r.id == id; });
                });
                WorldWatch::RequestEvaluationForUserEdit();
                FUCK::PopID();
                return;  // this row's data no longer exists - stop here
            }
            if (deleteBtn.hovered) {
                FUCK::SetTooltip("Delete this rule");
            }

            // ---- When / Then --------------------------------------------------
            //
            // The card reads as one sentence. Conditions were already a wrapped
            // flow of chips and were the part of this tab nobody had trouble
            // with, so effects borrow the same shape rather than inventing a
            // fourth widget vocabulary. Both flows start at the same X so the
            // two rows line up under each other.
            const float labelW = std::max(FUCK::CalcTextSize("When").x,
                                          FUCK::CalcTextSize("Then").x) +
                                 spacing * 2.0f;
            const float flowRight = rowOrigin.x + avail;

            const auto beginFlowRow = [&](const char* a_label) {
                FUCK::AlignTextToFramePadding();
                FUCK::TextDisabled("%s", a_label);
                FUCK::SameLine();
                FUCK::SetCursorPosX(rowLeft + labelW);
                ChipFlow f;
                f.left    = FUCK::GetCursorScreenPos().x;
                f.right   = flowRight;
                f.spacing = spacing;
                f.pen     = f.left;
                return f;
            };

            {
                ChipFlow flow = beginFlowRow("When");
                for (std::size_t i = 0; i < a_rule.conditions.size(); ++i) {
                    const std::string text = ChipText(a_rule.conditions[i]);
                    flow.Place(flow.Predict(text.c_str()));
                    DrawConditionChip(a_rule.id, i, a_rule.conditions[i], a_snapshot,
                                      /*editable=*/true);
                    flow.Commit(text.c_str());
                }
                constexpr const char* kAdd = "+ Condition";
                flow.Place(flow.Predict(kAdd));
                DrawAddConditionButton(a_rule.id, tutorialRow);
                flow.Commit(kAdd);
                if (a_rule.conditions.empty()) {
                    // An empty condition list matches EVERYTHING (RuleModel.h's
                    // default-open stance). That is a footgun worth naming on
                    // the card rather than leaving to be discovered.
                    flow.Place(flow.Predict("always"));
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextDisabled("%s", "always");
                    flow.Commit("always");
                }
            }
            {
                ChipFlow   flow    = beginFlowRow("Then");
                const auto effects = BuildEffectChips(a_rule);
                for (std::size_t i = 0; i < effects.size(); ++i) {
                    const auto& fx = effects[i];
                    flow.Place(flow.Predict(fx.text.c_str()));
                    // ⚠ A PUSHED ID AND NO "###", and the dead demotion push is
                    // gone. Both for the reasons spelled out in full on the
                    // condition chip: a chamfered button neither measures nor
                    // draws a label with a hash suffix stripped, and the
                    // ImGuiCol_Button push was landing two slots off FUCK's own
                    // enum and has never been visible.
                    FUCK::PushID(static_cast<int>(i));
                    const auto chipBtn = ChamferPanel::Button(fx.text.c_str());
                    if (chipBtn.clicked) {
                        FUCK::OpenPopup(
                            fmt::format("rules_effect_{}_{}", a_rule.id, i).c_str());
                    }
                    DrawEffectEditPopup(a_rule.id, fx, a_rule, a_libNames, i);
                    FUCK::PopID();
                    // ⚠ THE FLOW STILL MEASURES THE LAST ITEM, and that still
                    // works: a chamfered button seals its footprint with a Dummy
                    // of its own exact rect, so GetItemRectMax after one is the
                    // BUTTON's edge rather than its label's. The flow has to stay
                    // generic anyway - it commits plain TextDisabled chips too.
                    flow.Commit(fx.text.c_str());
                }
                constexpr const char* kAddFx = "+ Effect";
                flow.Place(flow.Predict(kAddFx));
                DrawAddEffectButton(a_rule, a_libNames, a_snapshot.wornSlotMask, tutorialRow);
                flow.Commit(kAddFx);
                if (effects.empty()) {
                    flow.Place(flow.Predict("nothing yet"));
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextDisabled("%s", "nothing yet");
                    flow.Commit("nothing yet");
                }
            }

            // Reachable from "+ Effect > Other slots...". It has no button of
            // its own on the row any more, but the popup itself is unchanged.
            // The open is issued HERE rather than at the Selectable, because
            // this is the id stack DrawOverlayPopup's BeginPopup sees.
            if (g_openOverlayForRule == a_rule.id) {
                g_openOverlayForRule.clear();
                FUCK::OpenPopup(("rules_overlay_" + a_rule.id).c_str());
            }
            DrawOverlayPopup(a_rule.id, a_rule.overlay);

            FUCK::PopID();
        }

        // ---- one pack (read-only) rule row -----------------------------------
        // ⚠ NO a_isWinner PARAMETER, AND ITS ABSENCE IS THE POINT. A library
        // rule cannot be the winner: RuleEngine skips anything carrying a
        // packName. Taking the flag and ignoring it would leave the next reader
        // hunting for the branch that uses it; not taking it says there is none.
        void DrawPackRule(const Rule& a_rule,
                          const WorldSnapshot& a_snapshot,
                          const std::map<std::string, std::string>& a_advancedInvalid) {
            FUCK::PushID(a_rule.id.c_str());
            const CardSpacing roomy;

            const ImVec2 rowOrigin = FUCK::GetCursorScreenPos();
            const float  rowLeft   = FUCK::GetCursorPos().x;  // window space, for SetCursorPosX
            const float  avail     = FUCK::GetContentRegionAvail().x;
            const float  spacing   = OS::ui::ItemSpacing().x;

            // ⚠ NO TICK AND NO STATE WORD ON A LIBRARY ROW, because a library
            // rule does not run. It used to have both, and the field showed what
            // that meant: a pack rule marked Active, in gold, dressing a player
            // who never chose it. The Rule Library is a CATALOG now. RuleEngine
            // refuses any rule carrying a packName, Import is the only thing you
            // can do with one, and importing clears the pack tag so it becomes an
            // ordinary rule of yours that runs like the rest (OS-121).
            //
            // The row keeps the diamond, the name, the pack tag and what the rule
            // WOULD do, because that is what you read to decide whether to import.

            // ⚠ NEVER GOLD. Gold means "this is what you are wearing right
            // now", and a library rule cannot be: RuleEngine skips anything
            // carrying a packName. The winner branch that used to be here is
            // GONE rather than left unreachable, because a library row drawn in
            // the live colour is the exact thing the field reported.
            const float nameX = FUCK::GetCursorPos().x;
            // ⚠ THIS IS WHAT ALIGNS THE IMPORT BUTTON, and it is why that button
            // no longer moves itself. The row is text followed by a framed
            // widget, and items on a SameLine run are TOP-aligned, so FontSize
            // text beside a FrameHeight button leaves the two centres half a
            // FramePadding.y apart. Raising the button by hand fixed the
            // arithmetic and broke the layout: a button lifted above the line's
            // own top overhangs the row above it, which is exactly what the
            // field saw it do against the section hint. Offsetting the TEXT down
            // to the frame's baseline is the other way round, and it costs the
            // row nothing because the button already sets the line height.
            FUCK::AlignTextToFramePadding();
            FUCK::BeginDisabled(true);
            FUCK::TextUnformatted(a_rule.name.c_str());
            FUCK::EndDisabled();
            FUCK::SameLine();
            FUCK::TextDisabled("[pack: %s]", a_rule.packName.c_str());

            FUCK::SameLine();
            const char* baseLabel = a_rule.base.kind == BaseKind::kRealGear ? "Real gear"
                                    : a_rule.base.kind == BaseKind::kOutfit
                                        ? a_rule.base.outfitName.c_str()
                                        : "Keep current look";
            FUCK::TextDisabled("-> %s", baseLabel);

            // Overlay was invisible on a pack row entirely - no button, no
            // count - which is how a helmet-hiding pack could look like it did
            // nothing at all. There is no popup to open (the rows are
            // read-only), so the slots go in a tooltip instead.
            if (!a_rule.overlay.empty()) {
                FUCK::SameLine();
                FUCK::TextDisabled("+ overlay (%d)", static_cast<int>(a_rule.overlay.size()));
                if (FUCK::IsItemHovered()) {
                    std::string tip = "Hides or restyles:";
                    for (const auto& [bit, entry] : a_rule.overlay) {
                        tip += "\n  " + SlotLabel(bit) + " (slot " + std::to_string(bit + 30u) +
                               ")" + (entry.kind == SlotEntry::Kind::kHide ? " - hidden" : " - restyled");
                    }
                    tip += "\n\nWearing headgear on a different slot? Copy this rule and change it.";
                    FUCK::SetTooltip(tip.c_str());
                }
            }

            if (const auto info = ResolveInvalid(a_rule, a_advancedInvalid); info.invalid) {
                FUCK::SameLine();
                FUCK::TextColored(kBadColor, "%s", Icons::Utf8(Icons::kWarning).c_str());
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip(info.reason.empty() ? "This rule is invalid" : info.reason.c_str());
                }
            }

            // The way OUT of read-only. A pack rule that is nearly right - the
            // right conditions but the wrong slot, which is exactly what the
            // helmet pack hits on a hood or a circlet - previously left the
            // user with nothing to do but switch it off. Copying lands an
            // editable duplicate in their own rules, and switching the pack
            // original off is left to them rather than done here: two rules
            // briefly agreeing is harmless (the copy takes priority), whereas
            // silently disabling something they might have wanted to keep is
            // not.
            {
                const std::string kImportIcon = Icons::Utf8(Icons::kFileImport);
                // ⚠ RIGHT-ALIGNED TO THE CARD EDGE, so the buttons form one
                // column rather than tracking the end of each rule's summary
                // text. Following the text was tried and the rows read as
                // ragged, since every rule name and base is a different length
                // (user 2026-08-06).
                //
                // ⚠ Width MEASURED, not derived from the style var. The first
                // version computed it as text + FramePadding().x * 2 and the
                // button ran off the right edge - a FUCK::Button is not
                // necessarily text plus the theme's frame padding, which is the
                // same assumption that already cost this tab a round trip on the
                // row's trailing group. g_importBtnW is calibrated from the real
                // rect the first time one is drawn; until then the style-var
                // estimate is used, so at worst the very first frame is off.
                //
                // The glyph is narrower than the word it replaced, so the number
                // changes; the discipline does not.
                static float g_importBtnW = -1.0f;
                const float  predicted =
                    g_importBtnW > 0.0f
                        ? g_importBtnW
                        : FUCK::CalcTextSize(kImportIcon.c_str()).x +
                              OS::ui::FramePadding().x * 2.0f;
                // ⚠ A PLAIN SameLine, AND THE VERTICAL FIX IS NOT HERE ANY MORE.
                // It moved to the AlignTextToFramePadding at the top of the row,
                // which offsets the TEXT down onto the frame baseline instead of
                // lifting the BUTTON up off the line. Lifting the button
                // centred it correctly and made it overhang the row above, which
                // is what the field saw against the section hint. Do not
                // reintroduce a SameLineAtY here.
                FUCK::SameLine();
                FUCK::SetCursorPosX(
                    std::max(FUCK::GetCursorPos().x, rowLeft + avail - predicted));
                const auto importBtn = ChamferPanel::IconButton(kImportIcon.c_str());
                if (importBtn.clicked) {
                    CopyPackRuleToOwn(a_rule);
                }
                // ⚠ THE BUTTON'S OWN WIDTH, OFF THE RESULT, because this is a
                // measured reservation that the NEXT frame lays out against.
                // Reading it from the last item would work by accident here (a
                // chamfered button seals its footprint with a Dummy of its own
                // rect) and the result says it on purpose.
                g_importBtnW = importBtn.max.x - importBtn.min.x;
                if (importBtn.hovered) {
                    // Short, because the icon no longer says the word. What the
                    // copy lets you change was three clauses the user had already
                    // worked out by the time they hovered.
                    FUCK::SetTooltip("Copy into your own rules, where you can edit it");
                }
            }

            DrawConditionFlow(a_rule, a_snapshot, /*editable=*/false, rowOrigin.x + avail,
                              spacing, nameX);

            FUCK::PopID();
        }

        // ---- "Load shared rules" feedback ------------------------------------
        // Review finding 5 moved the import itself onto the main thread (it
        // does synchronous file I/O), so the result has to cross back to the
        // render thread through a lock rather than a plain global - the same
        // shape as WorldWatch::GetPublished(). FUCK::GetTime() is only ever
        // called from the render thread (PumpLoadSeedResult, invoked from
        // Draw()); the main-thread task below never touches FUCK:: at all.
        std::mutex  g_loadSeedResultLock;
        bool        g_loadSeedResultPending = false;
        std::string g_loadSeedResultText;

        void PostLoadSeedResult(std::string a_text) {
            std::scoped_lock l(g_loadSeedResultLock);
            g_loadSeedResultPending = true;
            g_loadSeedResultText    = std::move(a_text);
        }

        // Render-thread-only display state, latched from the pending result
        // (if any) once per Draw() call.
        std::string g_loadSeedDisplayText;
        double      g_loadSeedDisplayUntil = 0.0;

        void PumpLoadSeedResult() {
            std::string text;
            {
                std::scoped_lock l(g_loadSeedResultLock);
                if (!g_loadSeedResultPending) {
                    return;
                }
                text                    = std::move(g_loadSeedResultText);
                g_loadSeedResultText.clear();
                g_loadSeedResultPending = false;
            }
            g_loadSeedDisplayText  = std::move(text);
            g_loadSeedDisplayUntil = FUCK::GetTime() + 6.0;
        }

        // "+ New Rule" and "Import" both insert at the FRONT, so the newest
        // thing you did is the first thing you see and outranks what is below
        // it. g_scrollToRuleId (declared above, shared by both paths) still
        // carries the eye to the new card, which matters when the list was
        // scrolled away from the top.

        // ---- the first-rule guide ------------------------------------------
        //
        // ⚠⚠ THE PAGE EXPLAINED WHAT A RULE IS AND NEVER HELPED ANYONE MAKE
        // ONE (user 2026-08-16: a tester struggled with this page for the whole
        // tutorial). "+ New Rule" produces an EMPTY rule, and an empty rule
        // matches everything and does nothing, so the first thing a new player
        // met was a card with two blanks and no clue which blank came first.
        //
        // A starter fills both halves at once. The sentence on the card reads
        // completely the moment it appears, which turns the next step from
        // construction into editing, and editing something that already works
        // is the thing people do without being taught.
        //
        // ⚠ THE CONDITIONS OFFERED HERE TAKE NO PARAMETER, deliberately.
        // Indoors, in combat and sneaking are read straight off the world
        // snapshot, so a starter needs no form picker, cannot be built pointing
        // at a location the player has never been to, and cannot go invalid
        // when a plugin moves.
        struct Starter {
            const char*   label;
            const char*   ruleName;
            ConditionKind kind;
            const char*   tip;
        };

        void CreateStarterRule(const Starter& a_starter, const std::string& a_outfitName) {
            // Merged() BEFORE WithRules: taking it inside would ask the store
            // for the merged set while holding its own lock.
            const std::string newId = GenerateRuleId(RuleStore::Merged());
            RuleStore::WithRules([&](RuleSet& rules) {
                Rule r;
                r.id      = newId;
                r.name    = a_starter.ruleName;
                r.enabled = true;
                Condition c;
                c.kind = a_starter.kind;
                r.conditions.push_back(c);
                // No outfit saved yet is a legitimate state: the rule still
                // arrives with its When filled in and the card's own "+ Effect"
                // is then the one obvious blank. A base naming an outfit that
                // does not exist would be worse than none.
                if (!a_outfitName.empty()) {
                    r.base.kind       = BaseKind::kOutfit;
                    r.base.outfitName = a_outfitName;
                }
                // Front, for the reason "+ New Rule" documents: top is where a
                // new rule is easiest to find.
                rules.insert(rules.begin(), std::move(r));
                RenumberByPositionLocked(rules);
            });
            WorldWatch::RequestEvaluationForUserEdit();
            g_scrollToRuleId = newId;
            // The tutorial's one ask on this page. A no-op unless a step is
            // waiting for exactly this, so no guard is needed here.
            Tutorial::NotifyAction(Tutorial::Action::kMadeRule);
        }

        void DrawFirstRuleGuide(const std::vector<std::string>& a_outfitNames) {
            const std::string outfit =
                a_outfitNames.empty() ? std::string{} : a_outfitNames.front();

            FUCK::TextWrapped(
                "%s", "A rule is one sentence: WHEN something is true, THEN wear this. "
                      "Start from one of these and the card comes with both halves "
                      "filled in, ready to edit.");
            FUCK::Spacing();

            static constexpr Starter kStarters[] = {
                { "When I am indoors", "Indoors", ConditionKind::kInterior,
                  "Makes a rule that fires the moment you step inside" },
                { "When I am in combat", "In combat", ConditionKind::kCombat,
                  "Makes a rule that fires when a fight starts" },
                { "When I am sneaking", "Sneaking", ConditionKind::kSneaking,
                  "Makes a rule that fires while you are crouched" },
            };
            const ImVec2 startersMin = FUCK::GetCursorScreenPos();
            float        startersMaxX = startersMin.x;
            float        startersMaxY = startersMin.y;
            for (std::size_t i = 0; i < std::size(kStarters); ++i) {
                const auto& starter = kStarters[i];
                if (i > 0) {
                    FUCK::SameLine();
                }
                if (ChamferPanel::Button(starter.label).clicked) {
                    CreateStarterRule(starter, outfit);
                }
                if (FUCK::IsItemHovered()) {
                    // No full stop at the end: the standing tooltip rule.
                    const std::string tip =
                        outfit.empty()
                            ? std::string(starter.tip) +
                                  ".\nYou have no saved outfits yet, so it arrives with "
                                  "nothing to wear and the card asks for it"
                            : std::string(starter.tip) + ".\nIt will wear '" + outfit +
                                  "', and the card lets you pick another";
                    FUCK::SetTooltip(tip.c_str());
                }
                // The row's real rect, read back off the buttons rather than
                // predicted from their labels, so the tutorial ring lands on
                // them at any font size or scale.
                const ImVec2 btnMax = FUCK::GetItemRectMax();
                startersMaxX        = std::max(startersMaxX, btnMax.x);
                startersMaxY        = std::max(startersMaxY, btnMax.y);
            }
            Tutorial::PublishAnchor(Tutorial::Anchor::kRuleStarters, startersMin,
                                    ImVec2(startersMaxX, startersMaxY));
            FUCK::Spacing();
            FUCK::TextDisabled(
                "%s", "Or press + New Rule above to start from a blank one. A rule with no "
                      "conditions matches everything.");
        }

    }  // namespace

    void Draw(const std::vector<std::string>& a_playerOutfitNames) {
        PumpLoadSeedResult();

        const auto published   = WorldWatch::GetPublished();
        const auto engineState = RuleStore::GetEngineState();
        const auto merged      = RuleStore::Merged();

        // ---- pin banner ----
        //
        // ⚠ TWO DIFFERENT PINS WEAR THIS BANNER AND ONLY ONE OF THEM NEEDS A
        // BUTTON (user 2026-08-06: "do we really even need this resume button").
        // The answer turned out to be yes, but only for one of the two, and
        // showing it for both is what made it read as a chore.
        //
        //   * Picked an outfit in the editor. Since OS-149 that pin lifts by
        //     itself the moment the editor closes, so a Resume button asks the
        //     player to do a job that is already done. It became a thing to
        //     dismiss rather than a thing to read. This case gets a sentence
        //     saying what is about to happen, and no control at all.
        //
        //   * Picked an outfit with the quick-switch HOTKEY. That path pins
        //     from outside the editor entirely, so nothing marks the session as
        //     having changed an outfit and the pin outlives every editor close.
        //     It is a deliberate "stop switching me" and it needs a way out, so
        //     it keeps the button. Same for a pin restored from a save.
        //
        // ⚠ THE STATE IS NOT WORTH REMOVING, ONLY THE BUTTON IS. Without the
        // pin, opening the Rules tab after picking an outfit would let a rule
        // dress you while you are reading it: the editor gate deliberately does
        // NOT apply on this tab, so rules can land live here. See
        // WorldWatch::SetRulesViewOpen.
        //   * No rule can ever apply. Then the pin holds nothing off, and the
        //     banner was announcing a pause on nothing with a button to leave a
        //     state that has no effect (user 2026-08-08). This one gets no
        //     banner AND no pin: it stands down here rather than merely hiding,
        //     because telling the player the engine is free while quietly
        //     keeping it pinned is the kind of half-truth this file is careful
        //     about everywhere else.
        //
        // ⚠ STANDING DOWN FROM A DRAW IS SAFE HERE FOR ONE SPECIFIC REASON, not
        // as a general licence. Resume() is exactly what the button below does
        // on the same thread, and it clears `pinned`, so the next frame reads
        // false and this cannot re-fire. It reaches no rule because there is no
        // rule to reach.
        //
        // ⚠ AND IT IS THE RIGHT SEAM DESPITE ONLY RUNNING WHEN THE TAB IS OPEN.
        // The pin's only effect is gating rule application, which is already a
        // no-op with nothing to apply, so the state is inert until somebody
        // looks at it. This tab is where it is displayed and where it misleads.
        // ⚠ Do NOT "improve" this by refusing the pin in NotifyManualPick: that
        // function pins unconditionally on purpose, and SetEngineEnabled relies
        // on a pin being there to clear when the engine flips on.
        if (engineState.pinned) {
            const bool selfClearing = WorldWatch::PinResolvesOnEditorExit();
            const std::string what =
                engineState.pinnedName.empty()
                    ? std::string("your real gear")
                    : fmt::format("'{}'", engineState.pinnedName);
            if (!Rules::AnyRuleCanApply(merged)) {
                WorldWatch::Resume();
            } else if (selfClearing) {
                FUCK::TextColored(
                    kLiveColor, "%s",
                    fmt::format("Wearing {} while you work. Your rules take over again "
                                "when you close Fitting Room.",
                                what)
                        .c_str());
                FUCK::Separator();
            } else {
                // ⚠ THE TEXT IS ALIGNED TO THE BUTTON, NOT THE OTHER WAY ROUND
                // (user 2026-08-08: the Resume button "is not aligned with the
                // text to the left of it"). A framed Button is taller than a
                // line of text, so a bare SameLine leaves the sentence sitting
                // on the button's top edge. AlignTextToFramePadding pushes the
                // text down by the frame padding BEFORE it is submitted, which
                // sets the line height for everything after it.
                //
                // ⚠ IT HAS TO COME BEFORE THE TEXT. It moves the CURRENT line's
                // text baseline offset, so calling it after the sentence would
                // align nothing and shift the button instead. This is the
                // opposite case from the status strip below, where the widget
                // comes first and TextYCenteredOnLastItem measures it.
                FUCK::AlignTextToFramePadding();
                FUCK::TextColored(
                    kLiveColor, "%s",
                    fmt::format("Auto switching paused. Wearing {} manually.", what).c_str());
                FUCK::SameLine();
                const auto resumeBtn = ChamferPanel::Button("Resume");
                if (resumeBtn.clicked) {
                    WorldWatch::Resume();
                }
                if (resumeBtn.hovered) {
                    FUCK::SetTooltip("Turn auto switching back on");
                }
                FUCK::Separator();
            }
        }

        // ---- status strip ----
        {
            bool engineOn = engineState.engineEnabled;
            if (FUCK::Checkbox("Auto switching enabled", &engineOn, false, false)) {
                WorldWatch::SetEngineEnabled(engineOn);
            }
            // Everything after this sits on the checkbox's own measured centre
            // line - see TextYCenteredOnLastItem for why SameLine alone is not
            // enough here.
            const float textY = TextYCenteredOnLastItem();

            SameLineAtY(textY);
            FUCK::TextDisabled("|");
            SameLineAtY(textY);
            FUCK::TextDisabled("%s", "Currently matched:");
            SameLineAtY(textY);
            if (engineState.pinned) {
                FUCK::TextDisabled("%s", "(paused)");
            } else if (const auto* winner =
                           FindRuleById(merged, published.decision.matchedRuleId)) {
                // Gold, matching the live-condition chips: this is the one line
                // that says what the engine is actually doing, and two rules can
                // easily share a name, so it needs to read as a value rather
                // than as more label text.
                FUCK::TextColored(kLiveColor, "%s", winner->name.c_str());
                // Overlays compose, so more than one rule can be live. Naming
                // only the top one made the tab look like the others had lost,
                // when in fact they were still contributing slots.
                const auto extra = published.decision.activeRuleIds.size();
                if (extra > 1) {
                    SameLineAtY(textY);
                    FUCK::TextDisabled("(+%d more)", static_cast<int>(extra - 1));
                    if (FUCK::IsItemHovered()) {
                        std::string tip = "Every rule matching right now, highest priority first.\n"
                                          "The topmost one that names an outfit sets it; all of "
                                          "them contribute their overlay.";
                        for (const auto& id : published.decision.activeRuleIds) {
                            const auto* r = FindRuleById(merged, id);
                            tip += "\n  " + (r ? r->name : id);
                        }
                        FUCK::SetTooltip(tip.c_str());
                    }
                }
            } else {
                FUCK::TextDisabled("%s", "(none)");
            }
        }
        FUCK::Separator();

        // ---- toolbar: the two list-level actions, above the cards they act on
        // ("+ New Rule" used to sit under the list, below the fold on any save
        // with more than a screenful of rules).
        const auto newRuleBtn = ChamferPanel::Button("+ New Rule");
        // The tutorial's make-a-rule ring. On the toolbar whatever the list
        // holds, which is exactly why the step anchors here rather than on the
        // starters: those only exist while the page is empty.
        Tutorial::PublishAnchor(Tutorial::Anchor::kNewRuleButton, newRuleBtn.min,
                                newRuleBtn.max);
        if (newRuleBtn.clicked) {
            const std::string newId = GenerateRuleId(merged);
            RuleStore::WithRules([newId](RuleSet& rules) {
                Rule r;
                r.id      = newId;
                r.name    = "New Rule";
                r.enabled = true;
                // ⚠ Front, not back. This appended for a long time, on the
                // reasoning that a rule with no conditions matches EVERYTHING
                // (RuleModel.h's default-open stance) and so inserting one at
                // top priority would change what the player is wearing the
                // instant the button was pressed.
                //
                // That was true under winner-takes-all, where the single
                // highest-priority match supplied the base as well. It stopped
                // being true when composition went additive: a fresh rule names
                // no base and carries no overlay, so it contributes nothing
                // wherever it sits, and RuleEngine::Evaluate does not even
                // report it as live until it is given an effect. Top is simply
                // where a new rule is easiest to find and fill in.
                rules.insert(rules.begin(), std::move(r));
                RenumberByPositionLocked(rules);
            });
            WorldWatch::RequestEvaluationForUserEdit();
            g_scrollToRuleId = newId;
            // ⚠ THE BLANK ROUTE COUNTS TOO. The tutorial asks for a starter,
            // but a player who reaches for this button instead has done the
            // thing the step is about, and a step that ignored them would sit
            // waiting for a control they had just walked past.
            Tutorial::NotifyAction(Tutorial::Action::kMadeRule);
        }
        // Measured before the hover branch so the number cannot depend on
        // whether the pointer happened to be over the button this frame. (ImGui
        // does restore the last-item record when a tooltip window ends -
        // ParentLastItemDataBackup - so measuring after would in fact work; this
        // is ordering for the sake of not having to know that, not a fix.)
        const float toolbarTextY = TextYCenteredOnLastItem();
        if (newRuleBtn.hovered) {
            // ⚠ IT SAID "at the bottom of the list" AND THE CODE INSERTS AT THE
            // FRONT. The insert moved to the front deliberately (see the block
            // above); the tooltip describing it did not move with it.
            FUCK::SetTooltip("Add a blank rule at the top of the list. A rule with no "
                             "conditions matches everything, and one with no effect does "
                             "nothing until you give it one");
        }

        FUCK::SameLine();
        if (ChamferPanel::Button("Load shared rules").clicked) {
            // Review finding 5: LoadSeed does synchronous file I/O (and
            // possibly a rename-aside on a rejected file) - that has no
            // business running on the render thread inside PresentThunk,
            // the same discipline every other mutation in this feature
            // already follows. Posted to the main thread; the result comes
            // back through PostLoadSeedResult/PumpLoadSeedResult above.
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([] {
                    RuleSet seed;
                    if (RuleStore::LoadSeed(seed)) {
                        // Review finding 3: check against the FULL merged
                        // set (save rules AND packs), not just the save's
                        // own - the same reasoning GenerateRuleId documents
                        // above. Otherwise a seed id colliding with a PACK
                        // id gets appended anyway, and RuleStore::Merged()
                        // then silently drops the pack rule (a warn, not
                        // this button's problem to cause).
                        std::set<std::string> existing;
                        for (const auto& r : RuleStore::Merged()) {
                            existing.insert(r.id);
                        }
                        std::size_t added = 0;
                        RuleStore::WithRules([&](RuleSet& rules) {
                            for (auto& r : seed) {
                                if (existing.insert(r.id).second) {
                                    rules.push_back(std::move(r));
                                    ++added;
                                }
                            }
                        });
                        WorldWatch::RequestEvaluationForUserEdit();
                        PostLoadSeedResult(added > 0
                                               ? fmt::format("Imported {} rule(s).", added)
                                               : "The shared seed had nothing new to add.");
                    } else {
                        PostLoadSeedResult("No shared rules.json seed found. See FittingRoom.log.");
                    }
                });
            }
        }
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("Import rules.json into this save (skips ids you already have)");
        }
        if (!g_loadSeedDisplayText.empty() && FUCK::GetTime() < g_loadSeedDisplayUntil) {
            // Centred on the buttons rather than SameLine'd flat against them -
            // same measured correction as the status strip above.
            SameLineAtY(toolbarTextY);
            FUCK::TextDisabled("%s", g_loadSeedDisplayText.c_str());
        }

        // ---- rule list ----
        // Full remaining height: the actions that used to be pinned under it
        // are in the toolbar above now, so nothing has to be reserved for them.
        // ⚠⚠ NO ImGui BORDER ANY MORE, AND THE OUTLINE IS DRAWN BELOW INSTEAD.
        // The border flag was `true`, which paints a square rect, and this child
        // takes the full remaining height - so its bottom corners land exactly
        // where the editor's frame has a SCOOP bitten out of it. A square corner
        // inside a curved one, a pixel apart, at the busiest part of the chrome
        // (user 2026-08-12, "the corner of the rules panel clashes with the
        // outer frame corner cut out").
        //
        // ⚠ A CUT OUTLINE IS ONLY HONEST WHEN NOTHING SQUARE IS FILLED UNDER IT,
        // which is the rule that ruled this out for every FLICK-drawn widget.
        // This child passes it: the theme's ChildBg is #1D1A1700, alpha zero, so
        // there is no fill under the line at all and the corners have nothing to
        // poke out from behind.
        g_tutorialRowClaimed = false;  // one claimant per frame; see the flag's note

        const ImVec2 listMin = FUCK::GetCursorScreenPos();
        const ImVec2 listAvail = FUCK::GetContentRegionAvail();
        // ⚠ THE BAR IS RESERVED BEFORE THE LIST IS SIZED, and the arithmetic is
        // the editor's own rather than a fresh guess: Body Studio, Shape and
        // Overlays all reserve their footer with these five terms. A bar drawn
        // after a list that already took the whole height is a bar pushed off
        // the bottom of the page.
        //
        // This page had no bottom bar at all until 2026-08-19, when the user
        // asked for the Hide outfit button on the bar of EVERY page. The button
        // is the only thing on it; a rules list has no transaction of its own.
        const float footerH = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f +
                              OS::ui::FramePadding().y * 2.0f +
                              std::max(2.0f, FUCK::GetResolutionScale() * 2.0f);
        FUCK::BeginChild("rules_list", ImVec2(0, -footerH), false);
        if (OS::ScrollReset::Pending()) {
            FUCK::SetScrollHereY(0.0f);
        }
        // The cards, for the two read cards that talk about them. Published
        // from the child's own rect rather than from a card's, because a ring
        // around ONE card would be pointing at whichever rule happened to be
        // first rather than at what the sentence is about.
        // ⚠ THE RESERVED BAR COMES OFF THE RING TOO. listAvail is what was free
        // BEFORE the footer was taken, so ringing it would draw a rectangle that
        // reaches past the bottom of the list it is pointing at.
        Tutorial::PublishAnchor(
            Tutorial::Anchor::kRuleList, listMin,
            ImVec2(listMin.x + listAvail.x,
                   listMin.y + std::max(1.0f, listAvail.y - footerH)));
        if (merged.empty()) {
            // ⚠ THE GREY ONE-LINER IS GONE. It said "No rules yet. Add one
            // above, or load the shared seed", which names two controls and
            // teaches neither: the first makes an empty card and the second
            // wants a file most players do not have. See DrawFirstRuleGuide.
            DrawFirstRuleGuide(a_playerOutfitNames);
        }
        // Merged() puts the save's own rules first, in their stored order, and
        // appends packs after, so the save-owned position is just a running
        // count. It is passed in rather than recomputed per row because the
        // order stepper needs the total as well.
        std::size_t ownedCount = 0;
        std::size_t packCount  = 0;
        for (const auto& rule : merged) {
            (rule.packName.empty() ? ownedCount : packCount) += 1;
        }

        // ⚠ Nothing wins while the engine is pinned. Evaluate's pinned early
        // return never reaches the composition step (Decision::activeRuleIds
        // documents exactly this), so the published ids are whatever the last
        // unpinned pass left behind - marking a card with them would claim a
        // rule is live while the strip beside it says "(paused)".
        //
        // The SET, not the single winner. Overlays compose additively now, so
        // several rules are genuinely live at once - a helmet rule hiding a slot
        // over the outfit rule that chose the clothes. Marking only
        // matchedRuleId would show one gold card and imply the other was doing
        // nothing, which is the confusion this whole model exists to remove.
        std::set<std::string> activeIds;
        if (!engineState.pinned) {
            for (const auto& id : published.decision.activeRuleIds) {
                activeIds.insert(id);
            }
        }

        // A wash, not a fill: the card has to stay readable through it, and it
        // sits behind an InputText and a Combo that carry their own frame
        // colours.
        const ImU32 winnerBg =
            OS::ui::Col(ImVec4(kLiveColor.x, kLiveColor.y, kLiveColor.z, 0.13f));

        // One table, one row per rule. This is what lets a card have a real
        // background: TableSetBgColor paints behind the row's content, and the
        // row auto-sizes to whatever the card draws, so no height has to be
        // predicted anywhere. That matters - a background must be submitted
        // BEFORE the content it sits behind, so the obvious group-then-fill
        // approach would have needed a predicted height, and predicted geometry
        // is precisely the trap this tab already sprang twice on widths.
        // kBordersInnerH replaces the per-row Separator the list used to emit.
        // Room between one card and the next. CellPadding is what a table puts
        // above and below each row's content, so flooring its y half is the gap
        // between cards without touching anything inside them, and the inner
        // border line lands in the middle of it. See RowRoom.
        {
            const ImVec2 cell = FUCK::GetStyleVarVec(ImGuiStyleVar_CellPadding);
            FUCK::PushStyleVar(ImGuiStyleVar_CellPadding,
                               ImVec2(cell.x, std::max(cell.y, RowRoom())));
        }
        if (FUCK::BeginTable("rules_table", 1,
                             FUCK::TableFlags::kNoSavedSettings |
                                 FUCK::TableFlags::kBordersInnerH)) {
            FUCK::TableSetupColumn("##rule", FUCK::TableColumnFlags::kWidthStretch);

            const auto beginRow = [&](bool a_winner) {
                FUCK::TableNextRow();
                FUCK::TableNextColumn();
                if (a_winner) {
                    FUCK::TableSetBgColor(FUCK::TableBgTarget::kRowBg0, winnerBg);
                }
            };

            // Headers only when there are packs. On an install with none - most
            // of them - a lone "Your rules" over the only list present is a
            // label, not a section.
            const bool sectioned = packCount > 0;

            // A drop strip past the last save-owned card. Without it the bottom
            // slot is unreachable by dragging: ReorderRule always lands the
            // dragged rule BEFORE its target, so dropping on the last card only
            // ever means second-to-last. Emitted at the owned/pack boundary
            // rather than at the very end, because pack rules are read-only and
            // are not part of the order being edited.
            // Declared before emitTailZone so the lambda can see it: a collapsed
            // Your-rules section has no order to edit, so the drop strip that
            // exists purely to make the last slot reachable by dragging would be
            // a gap under a shut header.
            bool       ownedOpen    = true;
            bool       tailZoneDone = false;
            const auto emitTailZone = [&] {
                if (tailZoneDone || ownedCount == 0 || !ownedOpen) {
                    return;
                }
                tailZoneDone = true;
                beginRow(false);
                FUCK::InvisibleButton("##droptail",
                                      ImVec2(std::max(1.0f, FUCK::GetContentRegionAvail().x),
                                             std::max(4.0f, OS::ui::ItemSpacing().y * 2.0f)));
                AcceptRuleDropAtEndOnLastItem();
            };

            // ⚠ AN ACCORDION LIKE THE LIBRARY BELOW, NOT A SeparatorText (user
            // 2026-08-06). Two sections one above the other where only one of
            // them collapses reads as an inconsistency rather than as a
            // deliberate difference, and the rules you are editing are the ones
            // most worth being able to fold away once the list is long.
            //
            // ⚠ DefaultOpen, AND THAT IS THE WHOLE POINT OF PUTTING IT IN ONE.
            // These are the rules that actually run. A collapsed section here on
            // arrival would hide the tab's own subject, which is the opposite of
            // the Library's default.
            //
            // No click sound, matching the Rule Library header below it. The dye
            // pane's sections do play one, but EditorStyle is not reachable from
            // this file and the two headers on this tab agreeing with each other
            // matters more than agreeing with another tab.
            //
            // Still only when there ARE packs: on an install with none, a lone
            // header over the only list present is a label, not a section.
            if (sectioned && ownedCount > 0) {
                beginRow(false);
                // Fold-all reaches this page too. See FoldAll.h; the After call
                // comes before anything reads ownedOpen, so a collapsed section
                // still carries the right-click menu.
                (void)OS::ui::FoldAll::Before();
                ownedOpen = FUCK::CollapsingHeader("Your rules",
                                                   ImGuiTreeNodeFlags_DefaultOpen);
                OS::ui::FoldAll::After();
            }

            // Packs collapse, and start collapsed. They are read-only reference
            // - the only thing you can do to one is switch it off or Import it -
            // so they do not earn permanent screen space above the rules you are
            // actually editing. The count stays in the header so a collapsed
            // section still says what is in there, and a live pack rule is not
            // silently hidden: the "(+N more)" readout in the status strip
            // counts it whether this is open or shut.
            //
            // ImGui remembers the open state per header id, so it survives
            // leaving and re-entering the tab within a session.
            bool packsOpen      = false;
            bool packHeaderDone = false;
            for (const auto& rule : merged) {
                const bool isWinner = activeIds.count(rule.id) != 0;
                if (rule.packName.empty()) {
                    if (!ownedOpen) {
                        continue;  // header drawn above, rows suppressed
                    }
                    beginRow(isWinner);
                    DrawEditableRule(rule, published.snapshot, a_playerOutfitNames,
                                     published.advancedInvalid, isWinner);
                    if (!g_scrollToRuleId.empty() && rule.id == g_scrollToRuleId) {
                        FUCK::SetScrollHereY(0.5f);
                        g_scrollToRuleId.clear();
                    }
                } else {
                    emitTailZone();  // the save-owned run just ended
                    if (!packHeaderDone) {
                        packHeaderDone = true;
                        beginRow(false);
                        // "Rule Library", not "Library": this mod already uses
                        // "library" for the OUTFIT library throughout
                        // (OutfitLibrary, SnapshotLibrary, WithLibrary), so the
                        // bare word would name two different things one tab
                        // apart. Also not "From packs" - that named the
                        // plumbing (a pack is a json file on disk) rather than
                        // what the section is to the person reading it.
                        (void)OS::ui::FoldAll::Before();
                        packsOpen = FUCK::CollapsingHeader(
                            fmt::format("Rule Library ({})", packCount).c_str());
                        OS::ui::FoldAll::After();
                        // ⚠ STILL SAYS WHAT THESE ROWS ARE, because a field
                        // report read them as rules already running and the
                        // reading was fair: every pack rule ships switched on,
                        // so the section opens on a column of ticked boxes with
                        // nothing saying a tick is a switch rather than a light
                        // (OS-121).
                        //
                        // ⚠ A TOOLTIP NOW, AND SHORTER (user 2026-08-06). Three
                        // lines of prose wedged between the header and the first
                        // row pushed the rules themselves down the pane and was
                        // read once and then read past forever. The tick-is-not-
                        // a-light point is the whole job; how importing works is
                        // already on the Import button's own tooltip, which is
                        // where someone asking that question is pointing.
                        if (FUCK::IsItemHovered()) {
                            FUCK::SetTooltip(
                                "Rules that came with installed packs. "
                                "None are running until you import one");
                        }
                    }
                    if (!packsOpen) {
                        continue;  // header drawn, rows suppressed
                    }
                    beginRow(isWinner);
                    DrawPackRule(rule, published.snapshot, published.advancedInvalid);
                }
            }
            emitTailZone();  // no pack rules at all, so the boundary is the end
            FUCK::EndTable();
        }
        FUCK::PopStyleVar();  // CellPadding, pushed above BeginTable
        // A rule deleted before its scroll landed would otherwise leave the
        // request pending forever, stealing the scroll off the next new rule.
        g_scrollToRuleId.clear();
        FUCK::EndChild();
        // The outline the border flag used to paint, cut to the frame's own
        // curve. Drawn AFTER EndChild because that is when the child's rect is
        // the last item and can be read back; from inside, GetItemRect would
        // describe whatever row happened to be submitted last.
        //
        // ⚠ ArtCorner FOR BOTH THE SHAPE AND THE CLAMP. This panel is large, so
        // it lands on the full corner and matches the frame exactly; the same
        // call on a small panel would clamp itself rather than swallowing it.
        // The fallback keeps the old square line rather than dropping it, since
        // a list with no boundary at all is worse than one with a square corner.
        {
            const ImVec2 listMin = FUCK::GetItemRectMin();
            const ImVec2 listMax = FUCK::GetItemRectMax();
            const ImVec4 listCol = OS::ui::StyleColor(ImGuiCol_Border);
            if (const auto frameTex = IconImages::Frame()) {
                ChamferPanel::FrameImage(frameTex, listMin, listMax, listCol,
                                         ChamferPanel::ArtCorner(listMin, listMax));
            } else {
                ChamferPanel::Stroke(listMin, listMax, listCol,
                                     ChamferPanel::CutFor(listMin, listMax),
                                     std::max(1.0f, FUCK::GetResolutionScale()));
            }
        }

        // ---- the bottom bar ------------------------------------------------
        // ⚠ AFTER THE OUTLINE, NOT BEFORE IT. That block reads GetItemRect to
        // find the list it is drawing around, so a button submitted first would
        // be the item it measured and the frame would ring the button.
        FUCK::Separator();
        OS::EditorUI::DrawHideOutfitButton();
    }

}  // namespace OS::RulesUI
