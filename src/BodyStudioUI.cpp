#include "BodyStudioUI.h"

#include "BodyPresetStore.h"
#include "BodySlideCatalog.h"
#include "BodyStudioDraft.h"
#include "BodyStudioLayout.h"
#include "BodyWeight.h"
#include "FoldAll.h"  // expand/collapse all, shared with every other page
#include "DefaultBody.h"
#include "BuildChannel.h"
#include "Settings.h"
#include "EditorStyle.h"
#include "ChamferPanel.h"  // the cut-corner button: FUCK::Button cannot be either
#include "FuckCompat.h"
#include "BodyMeshPath.h"
#include "BodyCardScene.h"   // one preset -> the scene that photographs it
#include "PreviewCardUI.h"   // the same card the appearance pane draws
#include "Icons.h"
#include "Tutorial.h"  // action steps and the anchors they ring
#include "UndoRedoKeys.h"  // Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y, shared with every other page
#include "EditorUI.h"  // FocusRightPaneOnGamepad: picking a preset opens the pane

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace OS::BodyStudioUI {

    namespace {
        enum class SourceFilter : std::uint8_t { kAll, kInstalled, kCustom };
        enum class Endpoint : std::uint8_t { kBoth, kSmall, kBig };

        struct State {
            SourceFilter filter{ SourceFilter::kAll };
            Endpoint endpoint{ Endpoint::kBoth };
            std::string search;
            std::string sliderSearch;
            std::optional<BodyPreset> edit;
            std::optional<BodyPreset> emptySource;
            BodyPreset saved;
            bool sourceCustom{ false };
            bool emptyDraft{ false };
            bool narrowEditor{ false };
            std::vector<BodyPreset> undo;
            std::vector<BodyPreset> redo;
            std::string status{ "Choose an installed preset to customize." };
            std::string pendingDeleteId;
            std::vector<std::string> allowedNames;

            // ---- the body this character arrived on ------------------------
            // Captured once per subject; see BuiltBody in BodyPreset.h for why
            // it must not be re-read while the page is open. builtPreset is an
            // OBody name the catalog resolves; builtCustom is filled instead
            // when the character wears a preset this mod authored, which
            // already knows its own set and family.
            std::string builtMesh;    // the body mesh the subject is wearing
            std::string builtLogged;  // the last verdict written to the log
        } g;

        constexpr std::size_t kUndoLimit = 64;
        inline const std::vector<std::string> kSourceFilterLabels{
            "All", "Installed", "Custom"
        };
        inline const std::vector<std::string> kBlankSourceFilterLabels(3);
        inline const std::vector<std::string> kEndpointLabels{
            "Both", "Small", "Big"
        };
        inline const std::vector<std::string> kBlankEndpointLabels(3);

        [[nodiscard]] std::string Lower(std::string_view a_value) {
            std::string out(a_value);
            std::ranges::transform(out, out.begin(), [](unsigned char a_char) {
                return static_cast<char>(std::tolower(a_char));
            });
            return out;
        }

        [[nodiscard]] bool Matches(std::string_view a_name) {
            return g.search.empty() || Lower(a_name).find(Lower(g.search)) != std::string::npos;
        }

        [[nodiscard]] bool Dirty() {
            return g.edit && *g.edit != g.saved;
        }

        void Remember(const BodyPreset& a_before) {
            if (!g.undo.empty() && g.undo.back() == a_before) return;
            g.undo.push_back(a_before);
            if (g.undo.size() > kUndoLimit) g.undo.erase(g.undo.begin());
            g.redo.clear();
        }

        void Preview(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy) {
            if (!g.edit || !a_actor) return;
            BodyStudioProof::QueueCustomPreset(a_actor, *g.edit, a_policy);
        }

        void RestoreSaved(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy) {
            if (!a_actor || !g.edit) return;
            if (g.emptyDraft) {
                Preview(a_actor, a_policy);
            } else if (g.sourceCustom && BodyPresetApplicable(g.saved)) {
                BodyStudioProof::QueueCustomPreset(a_actor, g.saved, a_policy);
            } else if (!g.edit->sourcePreset.empty()) {
                BodyStudioProof::QueueInstalled(a_actor, g.edit->sourcePreset, a_policy);
            }
        }

        void RestoreEmptySource(RE::Actor* a_actor,
                                BodyStudioProof::ORefitPolicy a_policy) {
            if (!a_actor || !g.emptySource) return;
            if (!g.emptySource->id.empty()) {
                BodyStudioProof::QueueCustomPreset(a_actor, *g.emptySource, a_policy);
            } else if (!g.emptySource->sourcePreset.empty()) {
                BodyStudioProof::QueueInstalled(a_actor, g.emptySource->sourcePreset,
                                                a_policy);
            }
        }

        void OpenInstalled(const BodyCatalogPreset& a_item, RE::Actor* a_actor,
                           BodyStudioProof::ORefitPolicy a_policy, bool a_narrow) {
            if (!a_item.authorable) {
                g.status = a_item.diagnostic;
                return;
            }
            g.edit = a_item.seed;
            g.edit->id.clear();
            g.edit->name += " Custom";
            g.saved = *g.edit;
            g.sourceCustom = false;
            g.emptyDraft = false;
            g.emptySource.reset();
            g.undo.clear();
            g.redo.clear();
            g.narrowEditor = a_narrow;
            g.status = "Installed reference loaded. Move a slider to claim a live custom preview.";
            BodyStudioProof::QueueInstalled(a_actor, a_item.seed.sourcePreset, a_policy);
        }

        void OpenCustom(const BodyPreset& a_preset, RE::Actor* a_actor,
                        BodyStudioProof::ORefitPolicy a_policy, bool a_narrow) {
            g.edit = a_preset;
            g.saved = a_preset;
            g.sourceCustom = true;
            g.emptyDraft = false;
            g.emptySource.reset();
            g.undo.clear();
            g.redo.clear();
            g.narrowEditor = a_narrow;
            g.status = "Custom preset loaded.";
            Preview(a_actor, a_policy);
        }

        void OpenEmpty(const BodyPreset& a_source, RE::Actor* a_actor,
                       BodyStudioProof::ORefitPolicy a_policy, bool a_narrow) {
            auto draft = MakeEmptyBodyDraft(a_source);
            if (!draft) {
                g.status = "Choose a compatible body before starting an empty preset.";
                return;
            }
            g.edit = std::move(*draft);
            g.saved = *g.edit;
            g.sourceCustom = false;
            g.emptyDraft = true;
            g.emptySource = a_source;
            g.undo.clear();
            g.redo.clear();
            g.narrowEditor = a_narrow;
            g.status = "Empty body loaded. Adjust any slider, then save the preset.";
            Preview(a_actor, a_policy);
        }

        [[nodiscard]] const BodyPreset* ResolveEmptySource(
            bool a_actorFemale, const BodySlideCatalogSnapshot& a_catalog,
            const std::vector<BodyPreset>& a_custom,
            std::string_view a_currentInstalledPreset,
            std::string_view a_currentCustomPresetId) {
            const auto usable = [a_actorFemale](const BodyPreset& a_preset) {
                return BodySexCompatible(a_preset.sex, a_actorFemale) &&
                       CanMakeEmptyBodyDraft(a_preset);
            };
            if (g.emptyDraft && g.emptySource && usable(*g.emptySource)) {
                return &*g.emptySource;
            }
            if (g.edit && usable(*g.edit)) {
                return &*g.edit;
            }
            if (!a_currentCustomPresetId.empty()) {
                const auto custom = std::ranges::find_if(
                    a_custom, [&](const BodyPreset& a_preset) {
                        return a_preset.id == a_currentCustomPresetId;
                    });
                if (custom != a_custom.end() && usable(*custom)) return &*custom;
            }
            if (!a_currentInstalledPreset.empty()) {
                if (const auto* installed = a_catalog.Find(a_currentInstalledPreset);
                    installed && installed->authorable && usable(installed->seed)) {
                    return &installed->seed;
                }
            }
            return nullptr;
        }

        // Match the Hair/ORefit enum row: the native control owns its arrows,
        // controller focus and hit box, while the regular editor font owns the
        // centred value. FLICK otherwise forces its large display font here.
        template <typename E>
        void DrawEnumStepper(const char* a_label, E* a_value,
                             const std::vector<std::string>& a_labels,
                             const std::vector<std::string>& a_blanks,
                             const char* a_tooltip) {
            if (FUCK::EnumStepper(a_label, a_value, a_blanks, false)) {
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            const int current = std::clamp(static_cast<int>(*a_value), 0,
                                           static_cast<int>(a_labels.size()) - 1);
            const char* text = a_labels[static_cast<std::size_t>(current)].c_str();
            const bool hovered = FUCK::IsItemHovered();
            const ImVec2 min = FUCK::GetItemRectMin();
            const ImVec2 max = FUCK::GetItemRectMax();
            const ImVec2 size = FUCK::CalcTextSize(text);
            OS::ui::TextAt(ImVec2(std::floor(min.x + (max.x - min.x - size.x) * 0.5f),
                                  std::floor(min.y + (max.y - min.y - size.y) * 0.5f)),
                           OS::ui::Col(ImGuiCol_Text), text);
            if (hovered && a_tooltip) FUCK::SetTooltip(a_tooltip);
        }

        void DrawSourceFilter() {
            DrawEnumStepper("Source", &g.filter, kSourceFilterLabels,
                            kBlankSourceFilterLabels,
                            "Choose which body preset sources to show");
        }

        // Which body is this character on? Answered once per subject and then
        // left alone.
        //
        // ⚠ ONCE, AND NOT BECAUSE THE READ IS DEAR. Every row on this page
        // previews itself onto the character as you click it, so asking the
        // engine each frame would answer with whatever was clicked last: pick
        // one preset and the list rearranges under the cursor, and picking a
        // second could hide the first. The question is which body they walked
        // in wearing.
        //
        // Which body is this character wearing? Read straight off the
        // character, every frame, and it is cheap enough to do that.
        //
        // ⚠⚠ NO CAPTURE AND NO TRIGGER ANY MORE, AND THAT IS THE POINT. This
        // asked OBody which preset was assigned, which meant every click in the
        // list below became the answer, because clicking previews by PUTTING
        // THE PRESET ON the character. Two rounds of freezing that value went
        // by before the source was the thing that changed: capture-once, then a
        // race-change trigger, both working exactly as designed and both
        // holding a polluted answer. A mesh path cannot be polluted, because
        // nothing this mod does writes one, so there is nothing left to freeze
        // against. See BodyMeshPath.h.
        void CaptureBuiltBody(RE::Actor* a_actor) {
            g.builtMesh = BuiltBodyMeshKey(a_actor);
        }

        [[nodiscard]] BuiltBody ResolveBuiltBody(
            const BodySlideCatalogSnapshot& a_catalog) {
            BuiltBody out;
            out.meshKey = g.builtMesh;
            // See the same line in EditorUI: two bodies can build to one path,
            // so the family still has to be available to prove a misfit.
            out.family = a_catalog.FamilyForMesh(g.builtMesh);
            // The set arm of BodyFitCompatible was unreachable while this
            // stayed empty; see the same line in EditorUI::BodyFitFilterNow.
            out.sourceSet = std::string{ a_catalog.SetForMesh(g.builtMesh) };
            return out;
        }

        struct FitFilter {
            BuiltBody   built;
            bool        active{ false };
            std::size_t hidden{ 0 };
        };

        // ⚠⚠ A FILTER THAT WOULD EMPTY THE LIST DOES NOT RUN, and this backstop
        // is the point rather than a nicety. Every rule in BodyFitCompatible
        // fails open at an unknown, but "known and every single preset
        // disagrees" is not an unknown - it is a confident answer that leaves
        // the player staring at a page with nothing on it and no way to tell
        // whether their presets are gone. An install where nothing matches is
        // an install where the classification is wrong, not one where the
        // player owns no usable bodies.
        [[nodiscard]] FitFilter ResolveFitFilter(
            const BodySlideCatalogSnapshot& a_catalog,
            const std::vector<BodyPreset>&  a_custom) {
            FitFilter out;
            // ⚠ THE SETTING IS READ FIRST AND NOTHING BELOW IT RUNS. Off means
            // no walk of the list and every preset offered, which is the same
            // shape an unknown body already produces. Settings.h says why it
            // defaults on.
            if (!Settings::GetSingleton().bodyFitFilter) {
                return out;
            } else {
            out.built = ResolveBuiltBody(a_catalog);
            if (!out.built.Known()) {
                return out;
            }
            std::size_t kept = 0;
            // A custom preset carries no mesh of its own; its slider set
            // names one. See BodyCatalogPreset::outputMesh.
            for (const auto& preset : a_custom) {
                if (BodyFitCompatible(preset, a_catalog.MeshForSet(preset.sourceSet),
                                      out.built)) {
                    ++kept;
                } else {
                    ++out.hidden;
                }
            }
            for (const auto& item : a_catalog.presets) {
                if (BodyFitCompatible(item.seed, item.outputMesh, out.built)) {
                    ++kept;
                } else {
                    ++out.hidden;
                }
            }
            if (kept == 0 || out.hidden == 0) {
                out.hidden = 0;
                return out;
            }
            out.active = true;
            return out;
            }
        }

        // ⚠ ONE LINE, AND ONLY WHEN THE ANSWER MOVES. This runs every frame, so
        // it remembers what it last said. It exists because the first field
        // report on this filter was "it did not update", and there was no way
        // to tell a stale capture from a preset OBody had not reclassified.
        // Now the log says which preset the character is on, what it resolved
        // to, and how many rows that took away.
        void LogFitFilter(const FitFilter& a_fit) {
            const auto line = OS::ui::FormatF(
                "Bodies: body mesh '%s', family %s; filter %s, %zu hidden",
                g.builtMesh.empty() ? "(unknown)" : g.builtMesh.c_str(),
                BodyFamilyName(a_fit.built.family), a_fit.active ? "ON" : "off",
                a_fit.hidden);
            if (line == g.builtLogged) {
                return;
            }
            g.builtLogged = line;
            spdlog::info("{}", line);
        }

        // One preset's card in the library grid. The click and the tooltip
        // stay with the caller, which is the only part that differs between
        // the two sections.
        //
        // ⚠ THE SCENE IS A LOCAL AND THAT IS SAFE, because DrawPreviewCard
        // reads the pointer synchronously and the request copies what it
        // needs. Holding one across frames would be the bug.
        [[nodiscard]] OS::PreviewCardUI::PreviewCardResult DrawLibraryCard(
            const BodyCardScene::Context& a_ctx, const std::string& a_name,
            const std::vector<BodySliderValue>& a_sliders, const std::string& a_setName,
            float a_side, bool a_selected, std::size_t a_order) {
            const auto scene = BodyCardScene::Build(a_ctx, a_name, a_sliders, a_setName);
            OS::PreviewCardUI::PreviewCardDesc d;
            // ⚠ A BODY PRESET HAS NO FORM, so the id stack is scoped by a hash
            // of its name, the same way the appearance pane scopes it, WITH THE
            // ORDINAL MIXED IN: two installed preset files can carry the same
            // display name, and two cards hashing alike are two visible items
            // on one ImGui id (the "conflicting ID" debug flash, field
            // 2026-08-11). Favourites key elsewhere and never see this.
            d.pushId = static_cast<std::uint32_t>(
                (PreviewGrid::Fnv1a64(a_name) ^
                 (static_cast<std::uint64_t>(a_order) * 0x9E3779B97F4A7C15ull)) &
                0xFFFFFFFFull);
            d.side      = a_side;
            d.name      = &a_name;
            d.nameCol   = OS::ui::Col(ImGuiCol_Text);
            d.selected  = a_selected;
            d.orderHint = static_cast<std::uint32_t>(a_order);
            d.scene     = scene.ok ? &scene.id : nullptr;
            const auto r = OS::PreviewCardUI::DrawPreviewCard(d);
            if (r.hovered && scene.noCardKey) {
                // The cross says what it means here rather than only in a log.
                char reason[512]{};
                std::snprintf(reason, sizeof reason, FUCK::Translate(scene.noCardKey),
                              a_setName.empty() ? "?" : a_setName.c_str());
                OS::ui::SetTooltipF("%s\n%s", a_name.c_str(), reason);
            }
            return r;
        }

        void DrawLibrary(RE::Actor* a_actor, bool a_actorFemale,
                         BodyStudioProof::ORefitPolicy a_policy,
                         const BodySlideCatalogSnapshot& a_catalog,
                         const std::vector<BodyPreset>& a_custom, bool a_narrow,
                         std::string_view a_currentInstalledPreset,
                         std::string_view a_currentCustomPresetId) {
            FUCK::TextDisabled("BODY PRESETS");

            const auto searchIcon = Icons::Utf8(Icons::kSearch);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(searchIcon.c_str());
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            FUCK::InputText("##body_search", &g.search);
            ChamferPanel::NoteFrameWidgetHeight();
            if (FUCK::IsItemHovered() && g.search.empty()) {
                FUCK::SetTooltip("Search body presets");
            }
            DrawSourceFilter();

            // ⚠ CARDS ONLY WHERE THERE IS ROOM FOR THEM. This library is a
            // sidebar beside the slider editor, and in the compact layout it
            // is the whole page for one breath before the editor replaces it.
            // A grid of thumbnails in a column that narrow is worse than the
            // list it replaced, so the list stays as the fallback rather than
            // being deleted.
            const bool useCards = Settings::GetSingleton().previewGrid && !a_narrow;
            const float cardSide =
                OS::ui::FontSize() * Settings::GetSingleton().previewCardScale;
            // Once per frame: it walks the mannequin's parts and reads the
            // character's weight, and a grid callback runs per card.
            const auto bodyCtx = BodyCardScene::MakeContext(&a_catalog, a_actor);

            // ⚠ NO NOTICE ABOVE THE LIST. One was drawn here and taken back out
            // (user 2026-08-08). The count of what is hidden is a line of
            // apology over a list that is simply correct, and it sat on the one
            // strip of this pane that is always visible.
            CaptureBuiltBody(a_actor);
            const auto fit = ResolveFitFilter(a_catalog, a_custom);
            LogFitFilter(fit);
            FUCK::Separator();

            FUCK::BeginChild("body_library_rows", ImVec2(0, 0), false);
            if (BodySlideCatalog::GetSingleton().Scanning()) {
                FUCK::TextDisabled("Scanning BodySlide projects...");
            }
            if (g.filter != SourceFilter::kInstalled) {
                const auto* emptySource = ResolveEmptySource(
                    a_actorFemale, a_catalog, a_custom, a_currentInstalledPreset,
                    a_currentCustomPresetId);
                const bool emptyVisible = Matches("Empty body");
                std::size_t matches = 0;
                for (const auto& preset : a_custom) {
                    if (fit.active &&
                        !BodyFitCompatible(preset, a_catalog.MeshForSet(preset.sourceSet),
                                           fit.built)) {
                        continue;
                    }
                    if (Matches(preset.name)) ++matches;
                }
                if (OS::ui::FramedHeader("Custom", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (emptyVisible) {
                        FUCK::PushID("empty_body_draft");
                        FUCK::BeginDisabled(!emptySource);
                        if (FUCK::Selectable("Empty body", g.emptyDraft) && emptySource) {
                            OS::Tutorial::NotifyAction(
                                OS::Tutorial::Action::kPickedBodyPreset);
                            OpenEmpty(*emptySource, a_actor, a_policy, a_narrow);
                            EditorStyle::PlayUISound("UIMenuFocus");
                        }
                        FUCK::EndDisabled();
                        if (FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            if (emptySource) {
                                OS::ui::SetTooltipF(
                                    "Start with every slider at zero\n%s / %s",
                                    BodySexName(emptySource->sex),
                                    BodyFamilyName(emptySource->family));
                            } else {
                                FUCK::SetTooltip(
                                    "Choose an installed or custom body first so the empty "
                                    "body uses the correct slider set");
                            }
                        }
                        FUCK::PopID();
                    }
                    if (matches == 0 && !emptyVisible) {
                        FUCK::TextDisabled(g.search.empty() ? "No custom presets yet."
                                                            : "No custom presets match this search.");
                    }
                    std::vector<const BodyPreset*> customShown;
                    if (useCards) {
                        for (const auto& preset : a_custom) {
                            if (fit.active &&
                                !BodyFitCompatible(
                                    preset, a_catalog.MeshForSet(preset.sourceSet),
                                    fit.built)) {
                                continue;
                            }
                            if (!Matches(preset.name)) continue;
                            if (!BodySexCompatible(preset.sex, a_actorFemale)) continue;
                            customShown.push_back(&preset);
                        }
                        OS::PreviewCardUI::DrawCardGrid(
                            customShown.size(), cardSide, [&](std::size_t a_i, float a_side) {
                                const auto& preset = *customShown[a_i];
                                const auto  r      = DrawLibraryCard(
                                    bodyCtx, preset.name, preset.sliders,
                                    preset.sourceSet, a_side,
                                    g.edit && g.edit->id == preset.id, a_i);
                                if (r.clicked) {
                                    OpenCustom(preset, a_actor, a_policy, a_narrow);
                                    EditorStyle::PlayUISound("UIMenuFocus");
                                    OS::Tutorial::NotifyAction(
                                        OS::Tutorial::Action::kPickedBodyPreset);
                                }
                            });
                    }
                    for (const auto& preset : a_custom) {
                        if (useCards) break;
                        if (fit.active &&
                            !BodyFitCompatible(preset, a_catalog.MeshForSet(preset.sourceSet),
                                               fit.built)) {
                            continue;
                        }
                        if (!Matches(preset.name)) continue;
                        FUCK::PushID(preset.id.c_str());
                        const bool compatible = BodySexCompatible(preset.sex, a_actorFemale);
                        FUCK::BeginDisabled(!compatible);
                        if (FUCK::Selectable(preset.name.c_str(),
                                             g.edit && g.edit->id == preset.id)) {
                            OpenCustom(preset, a_actor, a_policy, a_narrow);
                            EditorStyle::PlayUISound("UIMenuFocus");
                            // A pad has no way back to the pane this just
                            // filled, so send it there.
                            OS::EditorUI::FocusRightPaneOnGamepad();
                            // The page is empty until this happens, which is
                            // what its first tutorial step asks for.
                            OS::Tutorial::NotifyAction(
                                OS::Tutorial::Action::kPickedBodyPreset);
                        }
                        FUCK::EndDisabled();
                        if (FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            OS::ui::SetTooltipF("%s / %s\nSource: %s",
                                                BodySexName(preset.sex),
                                                BodyFamilyName(preset.family),
                                                preset.sourcePreset.c_str());
                        }
                        FUCK::PopID();
                    }
                }
            }
            if (g.filter != SourceFilter::kCustom) {
                // ⚠⚠ THE COUNT AND THE GRID ASK THE SAME QUESTION NOW. This
                // counter used to test the fit filter and the search only,
                // while the card grid below dropped every row that was also
                // non-authorable or the wrong sex. An install whose surviving
                // rows were all unusable therefore drew an EMPTY grid with no
                // "No installed presets found." underneath it, because the
                // count had happily counted the rows the grid then deleted. Two
                // readers of one answer drift in the gap between them.
                const auto passesFilters = [&](const BodyCatalogPreset& a_item) {
                    return (!fit.active ||
                            BodyFitCompatible(a_item.seed, a_item.outputMesh,
                                              fit.built)) &&
                           Matches(a_item.seed.name);
                };
                // The card layout can only draw a row it can open; the list
                // layout draws it disabled with its diagnostic on the tooltip,
                // which is why the two counts differ by design.
                const auto usable = [&](const BodyCatalogPreset& a_item) {
                    return a_item.authorable &&
                           BodySexCompatible(a_item.seed.sex, a_actorFemale);
                };
                std::size_t matches = 0;
                for (const auto& item : a_catalog.presets) {
                    if (!passesFilters(item)) continue;
                    if (useCards && !usable(item)) continue;
                    ++matches;
                }
                if (OS::ui::FramedHeader("Installed", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (matches == 0) {
                        FUCK::TextDisabled(g.search.empty() ? "No installed presets found."
                                                            : "No installed presets match this search.");
                    }
                    std::vector<const BodyCatalogPreset*> installedShown;
                    if (useCards) {
                        for (const auto& item : a_catalog.presets) {
                            if (!passesFilters(item) || !usable(item)) continue;
                            installedShown.push_back(&item);
                        }
                        OS::PreviewCardUI::DrawCardGrid(
                            installedShown.size(), cardSide, [&](std::size_t a_i, float a_side) {
                                const auto& item     = *installedShown[a_i];
                                const bool  selected = g.edit && !g.sourceCustom &&
                                                      !g.emptyDraft &&
                                                      g.edit->sourcePreset ==
                                                          item.seed.sourcePreset;
                                const auto r = DrawLibraryCard(
                                    bodyCtx, item.seed.name, item.seed.sliders,
                                    item.seed.sourceSet, a_side, selected, a_i);
                                if (r.clicked) {
                                    OS::Tutorial::NotifyAction(
                                        OS::Tutorial::Action::kPickedBodyPreset);
                                    OpenInstalled(item, a_actor, a_policy, a_narrow);
                                    EditorStyle::PlayUISound("UIMenuFocus");
                                }
                            });
                    }
                    for (const auto& item : a_catalog.presets) {
                        if (useCards) break;
                        if (!passesFilters(item)) continue;
                        FUCK::PushID(item.seed.name.c_str());
                        const bool compatible = usable(item);
                        FUCK::BeginDisabled(!compatible);
                        const bool selected = g.edit && !g.sourceCustom && !g.emptyDraft &&
                                              g.edit->sourcePreset == item.seed.sourcePreset;
                        if (FUCK::Selectable(item.seed.name.c_str(), selected)) {
                            OS::Tutorial::NotifyAction(
                                OS::Tutorial::Action::kPickedBodyPreset);
                            OpenInstalled(item, a_actor, a_policy, a_narrow);
                            EditorStyle::PlayUISound("UIMenuFocus");
                        }
                        FUCK::EndDisabled();
                        if (FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            OS::ui::SetTooltipF("%s\n%s / %s\n%s",
                                                item.seed.sourceSet.c_str(),
                                                BodySexName(item.seed.sex),
                                                BodyFamilyName(item.seed.family),
                                                compatible ? "Customize"
                                                           : item.diagnostic.c_str());
                        }
                        FUCK::PopID();
                    }
                }
            }
            FUCK::EndChild();
        }

        // Same quiet, centred empty state as Outfit mode: two short disabled
        // lines, no heading and no explanatory wall of text.
        void DrawEmptyEditorPrompt() {
            const std::array<const char*, 2> lines{
                "Pick a preset on the left to begin.",
                "Its body sliders appear here."
            };
            const float availableWidth = FUCK::GetContentRegionAvail().x;
            const float availableHeight = FUCK::GetContentRegionAvail().y;
            const float lineHeight = OS::ui::FontSize() + OS::ui::ItemSpacing().y;
            const float gap = OS::ui::WindowPadding().y;
            const float blockHeight = lineHeight * static_cast<float>(lines.size()) + gap;
            const float baseX = FUCK::GetCursorPos().x;
            if (availableHeight > blockHeight) {
                FUCK::SetCursorPosY(FUCK::GetCursorPos().y +
                                    (availableHeight - blockHeight) * 0.5f);
            }
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const float textWidth = FUCK::CalcTextSize(lines[index]).x;
                FUCK::SetCursorPosX(
                    baseX + std::max(0.0f, (availableWidth - textWidth) * 0.5f));
                FUCK::TextDisabled("%s", lines[index]);
                if (index + 1 < lines.size()) FUCK::Dummy(ImVec2(0.0f, gap));
            }
        }

        void DrawEndpointMode() {
            DrawEnumStepper("Edit values", &g.endpoint, kEndpointLabels,
                            kBlankEndpointLabels,
                            "Choose which BodySlide endpoint each slider changes");
        }

        // ⚠ THE SAME PER-CHARACTER WEIGHT THE OUTFIT BODY PANE EDITS, not a
        // second setting. There is only one value and it is the engine's
        // (TESNPC::weight), so the two pages cannot drift apart.
        //
        // ⚠ AND IT BELONGS BESIDE "Edit values" SPECIFICALLY. This page authors
        // the SMALL and BIG endpoint of every slider; weight is where between
        // those two the character actually sits. Editing endpoints without
        // being able to move the weight means authoring blind at one end of a
        // range you cannot see the middle of.
        void DrawWeight(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy) {
            if (!a_actor) {
                return;
            }
            const bool  canEdit = OS::BodyWeight::CanEdit(a_actor);
            const float actual  = OS::BodyWeight::Of(a_actor);

            // Latched edit buffer; see EditorUI's copy for why binding a slider
            // straight to the live actor value cannot work.
            static std::uint32_t s_owner{ 0 };
            static float         s_edit{ 0.0f };
            static bool          s_editing{ false };
            static float         s_appliedTo{ 0.0f };

            if (const std::uint32_t owner = a_actor->GetFormID(); s_owner != owner) {
                s_owner   = owner;
                s_editing = false;
            }
            if (!s_editing) {
                s_edit = actual;
            }

            if (!canEdit) {
                FUCK::BeginDisabled();
            }
            FUCK::SetNextItemWidth(-1.0f);
            FUCK::SliderFloat("##bs_weight", &s_edit, 0.0f, 100.0f, "$FR_BodyWeight"_T);
            const bool active  = FUCK::IsItemActive();
            const bool hovered = FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (!canEdit) {
                FUCK::EndDisabled();
            }
            // Disabled case only; see the note on the outfit pane's copy of this
            // slider for why the explanatory tooltip went and this one stayed.
            if (hovered && !canEdit) {
                FUCK::SetTooltip("$FR_BodyWeightShared"_T);
            }

            // ⚠ Preview, NOT RestoreStagedBody. The outfit pane puts back
            // whatever the OUTFIT names; this page has an unsaved edit buffer
            // open, and re-applying the saved body under the author would throw
            // their in-progress work off the character. Preview re-applies the
            // buffer, which is what rebuilds the plan at the new weight here.
            const auto push = [&](float a_value) {
                OS::BodyWeight::Set(a_actor, a_value);
                Preview(a_actor, a_policy);
            };
            if (active) {
                if (!s_editing) {
                    s_editing   = true;
                    s_appliedTo = actual;
                }
                if (canEdit && std::lround(s_edit) != std::lround(s_appliedTo)) {
                    s_appliedTo = s_edit;
                    push(s_edit);
                }
            } else if (s_editing && !FUCK::IsAnyItemActive()) {
                s_editing = false;
                if (s_edit != actual) {
                    push(s_edit);
                }
            }
        }

        void DrawEditor(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy,
                        float a_minimum, float a_maximum, bool a_narrow) {
            if (!g.edit) {
                DrawEmptyEditorPrompt();
                return;
            }
            auto& edit = *g.edit;
            if (a_narrow && ChamferPanel::Button("<  Body presets").clicked) {
                if (g.emptyDraft) {
                    RestoreEmptySource(a_actor, a_policy);
                } else if (Dirty()) {
                    RestoreSaved(a_actor, a_policy);
                }
                g.narrowEditor = false;
                return;
            }
            if (a_narrow) FUCK::Separator();

            FUCK::TextDisabled(g.sourceCustom ? "CUSTOM PRESET" : "NEW CUSTOM PRESET");
            FUCK::SetNextItemWidth(-1.0f);
            const BodyPreset beforeName = edit;
            const bool bodyNameEdited = FUCK::InputText("##body_preset_name", &edit.name);
            ChamferPanel::NoteFrameWidgetHeight();
            if (bodyNameEdited) Remember(beforeName);
            FUCK::TextDisabled("%s  /  %s  /  Source: %s", BodySexName(edit.sex),
                               BodyFamilyName(edit.family), edit.sourcePreset.c_str());
            FUCK::Separator();

            DrawEndpointMode();
            DrawWeight(a_actor, a_policy);

            const auto searchIcon = Icons::Utf8(Icons::kSearch);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(searchIcon.c_str());
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            FUCK::InputText("##body_slider_search", &g.sliderSearch);
            if (FUCK::IsItemHovered() && g.sliderSearch.empty()) {
                FUCK::SetTooltip("Search body sliders");
            }

            std::vector<std::string> categories;
            for (const auto& slider : edit.sliders) {
                if (std::ranges::find(categories, slider.category) == categories.end()) {
                    categories.push_back(slider.category);
                }
            }
            FUCK::BeginChild("body_slider_rows", ImVec2(0, 0), false);
            for (const auto& category : categories) {
                bool categoryMatches = false;
                for (const auto& slider : edit.sliders) {
                    if (slider.category == category &&
                        (g.sliderSearch.empty() ||
                         Lower(slider.displayName).find(Lower(g.sliderSearch)) !=
                             std::string::npos ||
                         Lower(slider.name).find(Lower(g.sliderSearch)) !=
                             std::string::npos)) {
                        categoryMatches = true;
                        break;
                    }
                }
                if (!categoryMatches ||
                    !OS::ui::FramedHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    continue;
                }
                if (!FUCK::BeginTable(("slider_table_" + category).c_str(), 3,
                                      FUCK::TableFlags::kRowBg |
                                          FUCK::TableFlags::kBordersInnerH |
                                          FUCK::TableFlags::kSizingStretchProp)) {
                    continue;
                }
                FUCK::TableSetupColumn("Name", FUCK::TableColumnFlags::kWidthStretch, 0.42f);
                FUCK::TableSetupColumn("Value", FUCK::TableColumnFlags::kWidthStretch, 0.50f);
                FUCK::TableSetupColumn("Reset", FUCK::TableColumnFlags::kWidthFixed,
                                       OS::ui::FontSize() * 2.0f);
                for (auto& slider : edit.sliders) {
                    if (slider.category != category ||
                        (!g.sliderSearch.empty() &&
                         Lower(slider.displayName).find(Lower(g.sliderSearch)) ==
                             std::string::npos &&
                         Lower(slider.name).find(Lower(g.sliderSearch)) ==
                             std::string::npos)) {
                        continue;
                    }
                    FUCK::PushID(slider.name.c_str());
                    FUCK::TableNextRow();
                    FUCK::TableNextColumn();
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted(slider.displayName.c_str());
                    if (FUCK::IsItemHovered()) {
                        OS::ui::SetTooltipF("BodySlide slider: %s\nSmall %.1f / Big %.1f",
                                            slider.name.c_str(), slider.smallValue,
                                            slider.bigValue);
                    }
                    FUCK::TableNextColumn();
                    FUCK::SetNextItemWidth(-1.0f);
                    const BodyPreset before = edit;
                    float value = g.endpoint == Endpoint::kSmall ? slider.smallValue
                                : g.endpoint == Endpoint::kBig ? slider.bigValue
                                : slider.smallValue +
                                      (slider.bigValue - slider.smallValue) * 0.5f;
                    if (FUCK::SliderFloat("##value", &value, a_minimum, a_maximum, "%.1f")) {
                        Remember(before);
                        if (g.endpoint != Endpoint::kBig) slider.smallValue = value;
                        if (g.endpoint != Endpoint::kSmall) slider.bigValue = value;
                        Preview(a_actor, a_policy);
                    }
                    FUCK::TableNextColumn();
                    const auto resetSlider =
                        ChamferPanel::IconButton(Icons::Utf8(Icons::kTimes).c_str());
                    if (resetSlider.clicked) {
                        Remember(edit);
                        if (g.endpoint != Endpoint::kBig) slider.smallValue = 0.0f;
                        if (g.endpoint != Endpoint::kSmall) slider.bigValue = 0.0f;
                        Preview(a_actor, a_policy);
                    }
                    if (resetSlider.hovered) {
                        FUCK::SetTooltip("Reset this slider");
                    }
                    FUCK::PopID();
                }
                FUCK::EndTable();
            }
            FUCK::EndChild();
        }

        // ⚠ THE STEP ITSELF, LIFTED OUT OF THE BUTTON, so the keyboard chord and
        // the click are one edit rather than two copies that drift (2026-08-18).
        // Safe on an empty stack and with no draft open: the buttons are greyed
        // in the first case and `g.edit` is null in the second, but a chord is
        // gated by neither.
        void Undo(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy) {
            if (g.undo.empty() || !g.edit) {
                return;
            }
            g.redo.push_back(*g.edit);
            *g.edit = g.undo.back();
            g.undo.pop_back();
            Preview(a_actor, a_policy);
        }

        void Redo(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy) {
            if (g.redo.empty() || !g.edit) {
                return;
            }
            g.undo.push_back(*g.edit);
            *g.edit = g.redo.back();
            g.redo.pop_back();
            Preview(a_actor, a_policy);
        }

        void DrawTransactionBar(RE::Actor* a_actor, BodyStudioProof::ORefitPolicy a_policy,
                                bool a_narrow) {
            FUCK::Separator();
            const float footerLeft = FUCK::GetCursorPos().x;
            const float footerWidth = FUCK::GetContentRegionAvail().x;
            // ⚠ FIRST ON THE BAR, AND IT IS EditorUI'S BUTTON. This page used to
            // carry a Hide outfit checkbox stacked above the sliders, which is
            // the "top part of bodies page" the user asked to be rid of on
            // 2026-08-19: it wanted the one control on the bottom bar of every
            // page. The bool and the drawing stay in EditorUI, so this page
            // still does not own either.
            OS::EditorUI::DrawHideOutfitButton();
            FUCK::SameLine();
            const bool dirty = Dirty();
            // ⚠ EVERY CONDITION IS PASSED AS WELL AS WRAPPED, down this whole
            // footer. ImGui's disabling refuses the click but does not reach
            // ChamferPanel's draw calls, so a button converted without its flag
            // goes dead while still looking live. The three tooltips here are
            // the disabled-state kind, and they keep working because the button
            // reports hover with AllowWhenDisabled.
            const bool undoDisabled = g.undo.empty();
            FUCK::BeginDisabled(undoDisabled);
            const auto undoBtn = ChamferPanel::IconButton(
                Icons::Utf8(Icons::kUndo).c_str(), undoDisabled);
            FUCK::EndDisabled();
            if (undoBtn.clicked) {
                Undo(a_actor, a_policy);
            }
            if (undoBtn.hovered) {
                FUCK::SetTooltip("Undo body edit");
            }
            FUCK::SameLine();
            const bool redoDisabled = g.redo.empty();
            FUCK::BeginDisabled(redoDisabled);
            const auto redoBtn = ChamferPanel::IconButton(
                Icons::Utf8(Icons::kRedo).c_str(), redoDisabled);
            FUCK::EndDisabled();
            if (redoBtn.clicked) {
                Redo(a_actor, a_policy);
            }
            if (redoBtn.hovered) {
                FUCK::SetTooltip("Redo body edit");
            }
            FUCK::SameLine();
            FUCK::BeginDisabled(!dirty);
            const auto discardBtn = ChamferPanel::Button("Discard", 0.0f, !dirty);
            FUCK::EndDisabled();
            if (discardBtn.clicked) {
                g.edit = g.saved;
                g.undo.clear();
                g.redo.clear();
                RestoreSaved(a_actor, a_policy);
                g.status = "Changes discarded.";
            }
            FUCK::SameLine();
            const bool commitDisabled = !g.edit || g.edit->name.empty();
            FUCK::BeginDisabled(commitDisabled);
            const auto commitBtn = ChamferPanel::Button(
                BodyStudioCommitLabel(g.sourceCustom), 0.0f, commitDisabled);
            FUCK::EndDisabled();
            if (commitBtn.clicked && g.edit) {
                std::string error;
                if (BodyPresetStore::GetSingleton().Save(*g.edit, error)) {
                    g.saved = *g.edit;
                    g.sourceCustom = true;
                    g.emptyDraft = false;
                    g.emptySource.reset();
                    g.status = "Custom preset saved.";
                    EditorStyle::PlayUISound("UIMenuOK");
                } else {
                    g.status = "Save failed: " + error;
                }
            }
            FUCK::SameLine();
            if (ChamferPanel::Button("Export XML").clicked) {
                std::string error;
                if (BodyPresetStore::GetSingleton().ExportBodySlideXml(
                        BuildChannel::BodySlideExportPath(), error)) {
                    g.status = "BodySlide XML exported: " +
                               BuildChannel::BodySlideExportPath().string();
                } else {
                    g.status = "Export failed: " + error;
                }
            }
            if (g.edit && g.sourceCustom) {
                FUCK::SameLine();
                if (ChamferPanel::IconButton(Icons::Utf8(Icons::kTrash).c_str()).clicked) {
                    g.pendingDeleteId = g.edit->id;
                    OS::ui::OpenModal("delete_body_preset");
                }
            }

            // ---- default body -------------------------------------------------
            // ⚠ ON THE FOOTER ROW, NOT A ROW OF ITS OWN (user 2026-08-06: "where
            // is the set as default button? i don't see it"). It was drawn after
            // a Separator, which put it on the next line, and that line is below
            // the pane's bottom edge and clipped away. This footer is a fixed
            // strip and anything that has to be reachable belongs on it.
            //
            // ⚠ ONE BUTTON THAT CHANGES WHAT IT DOES, rather than a Set beside a
            // Clear. Two buttons cost twice the width on a row that is already
            // full, and only ever one of them applies: a preset either is this
            // character's default or is not.
            //
            // ⚠ THE PRESET HAS TO EXIST ON DISK BEFORE IT CAN BE A DEFAULT. A
            // default is stored as a REFERENCE (a custom preset's stable id, or
            // an installed preset's name), so pointing one at an unsaved edit
            // buffer would record an id that resolves to nothing on the next
            // load and silently fall the character back to their baseline.
            if (a_actor) {
                const bool hasCustom = g.edit && g.sourceCustom && !g.edit->id.empty();
                const bool hasSource = g.edit && !g.edit->sourcePreset.empty();
                const auto current   = OS::DefaultBody::For(a_actor);
                const bool isDefault =
                    hasCustom ? (!current.customId.empty() && current.customId == g.edit->id)
                              : (hasSource && current.customId.empty() &&
                                 current.installed == g.edit->sourcePreset);
                const bool canSet = !dirty && (hasCustom || hasSource);

                FUCK::SameLine();
                // ⚠⚠ ChamferPanel::Button, AND THE MARGIN IS WHY AS MUCH AS THE
                // CORNER. This is the last control on its row, so it is the one
                // that meets the panel's right edge, and FUCK::Button sizes
                // itself as label plus 2 * (8 * scale) while never reading
                // FramePadding - so it is wider than anything measuring it would
                // predict and it ran out past the edge (user 2026-08-12, "set as
                // default button in some rows can also not have padding").
                // ChamferPanel's button measures by the formula everything else
                // here uses, so the row can hold it.
                //
                // ⚠ THE CONDITION IS PASSED AS WELL AS WRAPPED. ImGui's
                // disabling refuses the click but does not reach our draw calls,
                // so without the flag this would go dead while still looking
                // live. The tooltip below is the whole point of the disabled
                // state here - it says what to do to enable it - and it keeps
                // working because ChamferPanel::Button reports hover with
                // AllowWhenDisabled.
                // ⚠ A GLYPH NOW (user 2026-08-12, "set as default works and has
                // decent padding, but this button should become a symbol
                // actually"). The width fix above did its job and this is the
                // next step rather than a replacement for it: a square button
                // cannot run past an edge at all, whatever a translation does to
                // the words. The thumbtack pair and the reason it is not a tick
                // are recorded at Icons::kThumbtack; the two words move into the
                // tooltip below, which is where the editor puts a label it has
                // stopped drawing.
                const bool defaultDisabled = !isDefault && !canSet;
                FUCK::BeginDisabled(defaultDisabled);
                const auto defaultBtn = ChamferPanel::IconButton(
                    Icons::Utf8(isDefault ? Icons::kTimes : Icons::kThumbtack).c_str(),
                    defaultDisabled);
                FUCK::EndDisabled();
                if (defaultBtn.clicked) {
                    if (isDefault) {
                        OS::DefaultBody::Clear(a_actor);
                        g.status = "Default body cleared; this character falls back to "
                                   "their own body.";
                    } else {
                        OS::DefaultBody::Set(a_actor, hasCustom ? "" : g.edit->sourcePreset,
                                             hasCustom ? g.edit->id : "");
                        g.status = "Default body set for this character.";
                    }
                }
                if (defaultBtn.hovered) {
                    // House rule: a tooltip never ends in a period, including
                    // the multi-sentence ones. See CLAUDE.md.
                    //
                    // The name leads it now that the button is a glyph, the same
                    // shape the dye row's resets and the two head part twins use.
                    FUCK::SetTooltip(
                        (std::string(isDefault ? "Clear default" : "Set as default") +
                         "\n" +
                         (isDefault ? "This is this character's default body. Clearing it "
                                      "puts them back on the body they had before Fitting "
                                      "Room touched them"
                          : dirty   ? "Save your changes first. A default points at a saved "
                                      "preset, so an unsaved edit has nothing to point at"
                          : canSet  ? "Wear this whenever the outfit you have on does not "
                                      "name a body of its own. It belongs to the character, "
                                      "so it follows them across saves"
                                    : "Save this preset first, or pick one to customize"))
                            .c_str());
                }
            }
            if (OS::ui::BeginModal("delete_body_preset")) {
                FUCK::TextWrapped("Delete this custom body preset? Its file goes with it.");
                // Centred like every other confirm box; see the outfit delete
                // modal for why the three delete boxes were the ones missing it.
                OS::ui::CentreTwoButtons("Delete", "Cancel");
                if (ChamferPanel::Button("Delete").clicked) {
                    std::string error;
                    if (BodyPresetStore::GetSingleton().Delete(g.pendingDeleteId, error)) {
                        const auto source = g.edit ? g.edit->sourcePreset : std::string{};
                        if (a_actor && !source.empty()) {
                            BodyStudioProof::QueueInstalled(a_actor, source, a_policy);
                        }
                        g.edit.reset();
                        g.saved = {};
                        g.sourceCustom = false;
                        g.emptyDraft = false;
                        g.emptySource.reset();
                        g.status = "Custom preset deleted.";
                    } else {
                        g.status = "Delete failed: " + error;
                    }
                    g.pendingDeleteId.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::SameLine();
                if (ChamferPanel::Button("Cancel").clicked) {
                    g.pendingDeleteId.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndPopup();
            }
            const char* status = dirty ? "Unsaved Changes"
                               : g.edit && g.sourceCustom ? "Custom preset saved"
                                                         : "Select or create a custom preset";
            if (!a_narrow) {
                FUCK::SameLine();
            } else {
                const float statusWidth = FUCK::CalcTextSize(status).x;
                FUCK::SetCursorPosX(footerLeft +
                                    std::max(0.0f, (footerWidth - statusWidth) * 0.5f));
            }
            FUCK::AlignTextToFramePadding();
            if (dirty) {
                FUCK::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f),
                                  "%s", status);
            } else if (g.edit && g.sourceCustom) {
                FUCK::TextDisabled("%s", status);
            } else {
                FUCK::TextDisabled("%s", status);
            }
            if (FUCK::IsItemHovered() && !g.status.empty()) {
                FUCK::SetTooltip(g.status.c_str());
            }
        }
    }  // namespace

    void OnOpen(const std::vector<std::string>& a_installedPresetNames) {
        g = State{};
        g.allowedNames = a_installedPresetNames;
        BodyPresetStore::GetSingleton().Load();
        BodySlideCatalog::GetSingleton().RequestScan(a_installedPresetNames);
    }

    void OnPresetListChanged(const std::vector<std::string>& a_installedPresetNames) {
        g.allowedNames = a_installedPresetNames;
        BodySlideCatalog::GetSingleton().RequestScan(a_installedPresetNames);
        g.edit.reset();
        g.emptySource.reset();
        g.saved = {};
        g.emptyDraft = false;
        g.undo.clear();
        g.redo.clear();
        g.narrowEditor = false;
    }

    bool HasUnsavedChanges() { return Dirty(); }

    void OnClose(RE::Actor* a_actor) {
        if (a_actor && g.emptyDraft) {
            RestoreEmptySource(a_actor, {});
        } else if (a_actor && g.edit && Dirty()) {
            RestoreSaved(a_actor, {});
        }
        auto allowedNames = std::move(g.allowedNames);
        g = State{};
        g.allowedNames = std::move(allowedNames);
    }

    void Draw(RE::Actor* a_actor, bool a_actorFemale,
              BodyStudioProof::ORefitPolicy a_policy,
              std::string_view a_currentInstalledPreset,
              std::string_view a_currentCustomPresetId) {
        const auto catalog = BodySlideCatalog::GetSingleton().Snapshot();
        const auto custom = BodyPresetStore::GetSingleton().Snapshot();
        const bool narrow = BodyStudioLayout::UseCompact(
            FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad,
            FUCK::GetContentRegionAvail().x, OS::ui::FontSize());
        const float footerH = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f +
                              OS::ui::FramePadding().y * 2.0f +
                              std::max(2.0f, FUCK::GetResolutionScale() * 2.0f) +
                              (narrow ? FUCK::GetFrameHeightWithSpacing() : 0.0f);
        const float bodyH = std::max(1.0f, FUCK::GetContentRegionAvail().y - footerH);

        if (narrow) {
            FUCK::BeginChild("body_workbench_compact", ImVec2(0, bodyH), true);
            if (g.narrowEditor && g.edit) {
                DrawEditor(a_actor, a_policy, catalog->sliderMinimum,
                           catalog->sliderMaximum, true);
            } else {
                DrawLibrary(a_actor, a_actorFemale, a_policy, *catalog, custom, true,
                            a_currentInstalledPreset, a_currentCustomPresetId);
            }
            FUCK::EndChild();
        } else {
            const float gap = OS::ui::ItemSpacing().x;
            const float width = FUCK::GetContentRegionAvail().x;
            const float libraryW =
                BodyStudioLayout::LibraryWidth(width, OS::ui::FontSize());
            // Same visible split as the Outfit and Dye workbenches: a bounded
            // navigation pane beside a bounded detail pane, both persistent.
            FUCK::BeginChild("body_library", ImVec2(libraryW, bodyH), true);
            DrawLibrary(a_actor, a_actorFemale, a_policy, *catalog, custom, false,
                        a_currentInstalledPreset, a_currentCustomPresetId);
            FUCK::EndChild();
            // The half the page is empty without, which is what its first
            // tutorial step asks the player to use.
            OS::Tutorial::PublishAnchor(OS::Tutorial::Anchor::kBodyLibrary,
                                        FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            FUCK::SameLine(0.0f, gap);
            FUCK::BeginChild("body_editor", ImVec2(0, bodyH), true);
            DrawEditor(a_actor, a_policy, catalog->sliderMinimum,
                       catalog->sliderMaximum, false);
            FUCK::EndChild();
        }
        DrawTransactionBar(a_actor, a_policy, narrow);

        // The keyboard's half of the bar above, through the same two functions
        // the buttons call. After the bar and outside both panes, matching where
        // the outfit page resolves its own.
        switch (OS::ui::UndoRedo::Poll()) {
            case OS::ui::UndoRedo::Action::kNone:
                break;
            case OS::ui::UndoRedo::Action::kUndo:
                Undo(a_actor, a_policy);
                break;
            case OS::ui::UndoRedo::Action::kRedo:
                Redo(a_actor, a_policy);
                break;
        }

        const auto proof = BodyStudioProof::Snapshot(a_actor);
        if (proof.message != g.status && proof.activeCustom) {
            // Keep the persistent transaction message readable; runtime detail
            // is available as a tooltip without causing the footer to jitter.
            if (FUCK::IsItemHovered()) OS::ui::SetTooltipF("%s", proof.message.c_str());
        }
    }

}  // namespace OS::BodyStudioUI
