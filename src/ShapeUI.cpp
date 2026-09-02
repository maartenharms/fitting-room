#include "ShapeUI.h"

#include "BodyMorphCatalog.h"
#include "BodyMorphPresets.h"
#include "BodyStudioLayout.h"
#include "ChamferPanel.h"  // the editor's one button primitive
#include "EditorStyle.h"
#include "EditorUI.h"  // DrawHideOutfitButton: the one Hide outfit control
#include "FuckCompat.h"
#include "FoldAll.h"  // expand/collapse all, shared with every other page
#include "Icons.h"
#include "NodeTransformApi.h"
#include "ObodyApi.h"
#include "RaceMenuMorphApi.h"
#include "REAugments.h"   // GetActorSkin, the body system's own identity
#include "ShapeOverlay.h"  // and StyleCatalog::SexIdxOf, for the ARMA model index
#include "StyleCatalog.h"
#include "Tutorial.h"  // action steps and the anchors they ring
#include "UndoRedoKeys.h"  // Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y, shared with every other page

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OS::ShapeUI {

    namespace {

        using ShapeOverlay::kSliderCount;
        using ShapeOverlay::kSliders;

        // ⚠ THE RANGE IS SAM'S, DELIBERATELY. Screen Archer Menu's own body
        // morph slider runs -100..200 (sam/menu/BodyMorphs.yaml), and a
        // BodySlide value is a percentage of the same thing RaceMenu's SetMorph
        // takes as a fraction. Matching it means a shape written here and
        // opened in SAM shows the same number in the same place, and a slider
        // pushed to its end here is not silently clipped there.
        constexpr float kMorphMin = -1.0f;
        constexpr float kMorphMax = 2.0f;

        constexpr std::size_t kUndoLimit = 64;

        // Everything the page can change about a character, in one value.
        //
        // ⚠ ONE STRUCT FOR BOTH HALVES, so undo, discard and a saved shape all
        // mean the same thing whichever sliders were touched. A separate history
        // per half would let Undo step back through proportions while the body
        // morphs stayed where a later edit put them, which is the sort of state
        // nobody can reason about after three clicks.
        struct Edit {
            ShapeOverlay::Values         nodes{ ShapeOverlay::DefaultValues() };
            std::map<std::string, float> morphs;  // ordered: a saved file is stable

            friend bool operator==(const Edit&, const Edit&) = default;
        };

        struct State {
            std::uint32_t owner{ 0 };
            bool          loaded{ false };

            Edit current;
            Edit saved;  // what the character had when the page opened; Discard's target

            std::vector<Edit> undo;
            std::vector<Edit> redo;
            Edit              dragBefore;
            bool              dragOpen{ false };

            std::string   search;
            std::string   scannedFor;
            // The skin the catalogue was scanned for, beside the preset. Two
            // characters with no preset at all share the empty string, so the
            // preset on its own cannot tell one body from another.
            std::uint32_t scannedSkin{ 0 };
            std::uint64_t readGeneration{ 0 };

            std::string newShapeName;
            std::string pendingDeleteId;
            std::string pendingDeleteName;
            bool        requestDeletePopup{ false };  // set in the library, opened at top level
            bool        shapesLoaded{ false };
            bool        narrowLibrary{ false };  // compact layout: showing the library
            // What the export and delete buttons on a saved shape row actually
            // measured last frame, spacing included. See DrawLibrary.
            float       rowTrailingW{ 0.0f };
        } g;

        [[nodiscard]] const char* Tr(const char* a_key) { return FUCK::Translate(a_key); }

        // The subject's SKIN mesh paths, which is what the morph filter reads
        // the tris of. Resolved here on the game thread and handed to the scan,
        // because the scan runs detached and an actor's skin is engine state.
        //
        // ⚠ GENITALS ARE KEPT, UNLIKE Mannequin's COLLECTOR. That one drops
        // biped slot 52 because a schlong on a shop card was jarring; here the
        // mesh is a body part like any other and UBE's own category file
        // devotes 92 sliders to it, so dropping it would grey out 92 controls
        // that work.
        //
        // ⚠ THE HEAD IS NOT COLLECTED. Body sliders do not reach it, the head
        // has no BodySlide tri, and asking for one only costs a stat call.
        [[nodiscard]] std::vector<std::string> WornSkinMeshes(RE::Actor* a_actor) {
            std::vector<std::string> out;
            if (!a_actor) {
                return out;
            }
            auto* const skin = REAug::GetActorSkin(a_actor);
            auto* const race = a_actor->GetRace();
            auto* const npc  = a_actor->GetActorBase();
            if (!skin || !race || !npc) {
                return out;
            }
            const int sexIdx = StyleCatalog::SexIdxOf(npc);
            // The armour parent race too, for the reason Mannequin gives: a
            // custom race commonly inherits its body through that link, and a
            // collector without it hands back nothing on exactly the characters
            // most likely to be on a modded body.
            RE::TESRace* const armorRace = race->armorParentRace;
            for (auto* arma : skin->armorAddons) {
                if (!arma) {
                    continue;
                }
                const bool raceValid =
                    arma->IsValidRace(race) ||
                    (armorRace && armorRace != race && arma->IsValidRace(armorRace));
                if (!raceValid) {
                    continue;
                }
                const char* path = arma->bipedModels[sexIdx].GetModel();
                if (path && *path) {
                    out.emplace_back(path);
                }
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> CatalogueNames() {
            std::vector<std::string> names;
            const auto               snap = BodyMorphCatalog::Get();
            if (!snap) {
                return names;
            }
            names.reserve(BodyMorphCatalogParse::CountSliders(snap->categories));
            for (const auto& cat : snap->categories) {
                for (const auto& slider : cat.sliders) {
                    names.push_back(slider.name);
                }
            }
            return names;
        }

        [[nodiscard]] std::vector<std::pair<std::string, float>> AsPairs(
            const std::map<std::string, float>& a_morphs) {
            return { a_morphs.begin(), a_morphs.end() };
        }

        [[nodiscard]] bool Dirty() { return g.current != g.saved; }

        void Remember(const Edit& a_before) {
            if (a_before == g.current) {
                return;
            }
            g.undo.push_back(a_before);
            if (g.undo.size() > kUndoLimit) {
                g.undo.erase(g.undo.begin());
            }
            g.redo.clear();
        }

        // ---- pushing the whole edit at the character ------------------------

        void PushNode(RE::Actor* a_actor, std::size_t a_index) {
            const auto result = NodeTransformApi::Apply(
                a_actor, ShapeOverlay::PlanFor(a_index, g.current.nodes[a_index]));
            if (!result.available) {
                return;
            }
            spdlog::debug("Shape: '{}' {} -> {:.3f} ({} written, {} cleared).",
                          a_actor && a_actor->GetName() ? a_actor->GetName() : "(unnamed)",
                          kSliders[a_index].id, g.current.nodes[a_index],
                          result.bonesWritten, result.bonesCleared);
        }

        void PushMorph(RE::Actor* a_actor, const std::string& a_name, float a_value) {
            if (a_value == 0.0f) {
                g.current.morphs.erase(a_name);
            } else {
                g.current.morphs[a_name] = a_value;
            }
            const auto result = RaceMenuMorphApi::SetShapeMorph(a_actor, a_name, a_value);
            if (!result.available) {
                return;
            }
            spdlog::debug("Shape: morph '{}' -> {:.3f} on '{}'.", a_name, a_value,
                          a_actor && a_actor->GetName() ? a_actor->GetName() : "(unnamed)");
        }

        // Put the whole of g.current on the character in one go. Used by undo,
        // redo, discard and by wearing a saved shape, all of which change many
        // values at once and none of which can afford one refresh each.
        void PushAll(RE::Actor* a_actor) {
            for (std::size_t i = 0; i < kSliderCount; ++i) {
                (void)NodeTransformApi::Apply(
                    a_actor, ShapeOverlay::PlanFor(i, g.current.nodes[i]));
            }
            std::vector<RaceMenuMorphApi::MorphValue> values;
            values.reserve(g.current.morphs.size());
            for (const auto& [name, value] : g.current.morphs) {
                values.push_back(RaceMenuMorphApi::MorphValue{ name, value });
            }
            (void)RaceMenuMorphApi::ApplyShape(a_actor, values);
        }

        // ---- loading --------------------------------------------------------

        void EnsureLoaded(RE::Actor* a_actor) {
            if (!g.shapesLoaded) {
                g.shapesLoaded = true;
                BodyMorphPresets::Reload();
            }
            const std::uint32_t owner = a_actor ? a_actor->GetFormID() : 0u;
            if (g.loaded && g.owner == owner) {
                return;
            }
            g.owner  = owner;
            g.loaded = true;
            g.current.nodes = ShapeOverlay::ReadBack(NodeTransformApi::Read(a_actor));
            g.current.morphs.clear();
            g.undo.clear();
            g.redo.clear();
            g.dragOpen = false;
            g.pendingDeleteId.clear();

            // ⚠ THE CATALOGUE IS RESCANNED WHEN THE BODY PRESET CHANGES, NOT
            // ONLY WHEN THE SUBJECT DOES. The preset is what names the slider
            // set the filter runs against, so a follower on a different body
            // than the player needs a different list, and so does the player
            // after switching to an outfit that names another body.
            const auto preset = ObodyApi::Available()
                                    ? ObodyApi::AssignedPreset(a_actor)
                                    : std::string{};
            // ⚠⚠ AND ON THE SKIN TOO, NOW THAT THE MESHES ARE THE FILTER. Two
            // characters can share a preset - most obviously by both having
            // none - and wear different bodies, and keying the scan on the
            // preset alone would hand the second one the first one's slider
            // list. The skin is what identifies the body system, which is the
            // same reason StyleCatalog's fit cache keys on it.
            auto* const     skin    = REAug::GetActorSkin(a_actor);
            const auto      skinKey = skin ? skin->GetFormID() : 0u;
            const auto      worn    = WornSkinMeshes(a_actor);
            if (preset != g.scannedFor || skinKey != g.scannedSkin ||
                !BodyMorphCatalog::Get()->categoryFilesRead) {
                g.scannedFor  = preset;
                g.scannedSkin = skinKey;
                BodyMorphCatalog::RequestScan(preset, worn);
            }
            g.readGeneration = 0;
            g.saved          = g.current;
        }

        // ⚠ THE MORPH VALUES CANNOT BE READ IN EnsureLoaded AND THIS IS WHY.
        // The catalogue is scanned on a background thread, so on the frame the
        // page opens there are no names to ask about yet, and a character with
        // a shape already on them would draw a row of zeros. The first drag
        // would then write one of those zeros over a real value. Re-read
        // whenever a new snapshot lands instead, which is what the generation
        // counter is for.
        //
        // ⚠ THE DISCARD BASELINE MOVES WITH IT. Reading late and leaving
        // g.saved on the empty pre-scan value would make Discard mean "wipe
        // whatever shape this character already had", which is the opposite of
        // what the button says.
        void EnsureMorphsRead(RE::Actor* a_actor) {
            const auto snap = BodyMorphCatalog::Get();
            if (!snap || snap->generation == g.readGeneration) {
                return;
            }
            g.readGeneration = snap->generation;
            g.current.morphs.clear();
            // ⚠⚠ READ WITH THE UNFILTERED VOCABULARY, DRAW WITH THE FILTERED
            // ONE. PushAll clears our whole RaceMenu key and rewrites it from
            // this map, so a morph the character already carries under a name
            // the filter dropped would not be hidden by reading narrow - it
            // would be DELETED by the next undo, discard or worn shape. It is
            // carried here, written back untouched, and simply never drawn.
            for (const auto& m : RaceMenuMorphApi::ReadShape(a_actor, snap->allNames)) {
                g.current.morphs[m.name] = m.value;
            }
            g.saved = g.current;
            g.undo.clear();
            g.redo.clear();
        }

        // ---- the store watch ------------------------------------------------
        //
        // ⚠⚠ FIELD 2026-08-16: "hideoutfit always resets the slider, but when i
        // adjust the slider it returns again." The log put a body pass between
        // the toggle and the drag - `Body Studio proof: transition=...->installed
        // preset='Stoppu_2'` on EVERY toggle, both ways - which re-assigns the
        // OBody preset with ForceImmediateApplicationOfMorphs. RaceMenu is meant
        // to SUM our key with OBody's, so on paper the shape survives that. On
        // screen it does not.
        //
        // Two causes fit and they take different fixes: OBody's re-apply WIPES
        // every key including ours, or our key survives in the store and the
        // mesh was rebuilt after the last morph pass. So this watches the store.
        // Every half second while the page is open it reads back what RaceMenu
        // holds under our key for the morphs the page holds, and when the store
        // has lost one it re-pushes the whole shape and says so ONCE, naming the
        // morph and both values. A reset with no line under it is the mesh, not
        // the store.
        //
        // ⚠ THE RE-PUSH IS THE PAGE KEEPING ITS OWN PROMISE, not a workaround
        // for whoever cleared it. The slider still shows 0.5, so 0.5 is what the
        // character should be wearing; a page that showed one thing while the
        // body wore another is the failure being reported. Who clears it, and
        // whether that happens outside this page too, is what the log line is
        // for, and it is the next question rather than this one.
        //
        // ⚠ ONE LINE PER EPISODE. The flag re-arms only after a clean read, so a
        // store that keeps being cleared prints once per clearing rather than
        // twice a second.
        void WatchStore(RE::Actor* a_actor) {
            static int  s_tick     = 0;
            static bool s_reported = false;
            if (!g.loaded || !a_actor || g.current.morphs.empty()) {
                return;
            }
            if (++s_tick % 30 != 0) {
                return;
            }
            std::vector<std::string> names;
            names.reserve(g.current.morphs.size());
            for (const auto& [name, value] : g.current.morphs) {
                if (value != 0.0f) {  // a zero is REMOVED from the store by design
                    names.push_back(name);
                }
            }
            if (names.empty()) {
                return;
            }
            std::map<std::string, float> stored;
            for (const auto& m : RaceMenuMorphApi::ReadShape(a_actor, names)) {
                stored[m.name] = m.value;
            }
            const char* lostName  = nullptr;
            float       lostPage  = 0.0f;
            float       lostStore = 0.0f;
            std::size_t lostCount = 0;
            for (const auto& name : names) {
                const float page = g.current.morphs[name];
                const auto  it   = stored.find(name);
                const float have = it == stored.end() ? 0.0f : it->second;
                if (std::abs(page - have) > 0.001f) {
                    if (!lostName) {
                        lostName  = name.c_str();
                        lostPage  = page;
                        lostStore = have;
                    }
                    ++lostCount;
                }
            }
            if (lostCount == 0) {
                s_reported = false;
                return;
            }
            if (!s_reported) {
                s_reported = true;
                spdlog::warn("Shape: RaceMenu's store LOST {} of {} morph(s) the page holds on "
                             "'{}' - first '{}' page {:.3f} store {:.3f}. Re-pushing the shape.",
                             lostCount, names.size(),
                             a_actor->GetName() ? a_actor->GetName() : "(unnamed)", lostName,
                             lostPage, lostStore);
            }
            PushAll(a_actor);
        }

        // ---- the saved shape library (left pane) -----------------------------

        void WearShape(RE::Actor* a_actor, const BodyMorphPresets::Shape& a_shape) {
            const Edit before = g.current;
            g.current.morphs.clear();
            for (const auto& [name, value] : a_shape.values) {
                if (value != 0.0f) {
                    g.current.morphs[name] = value;
                }
            }
            Remember(before);
            PushAll(a_actor);
            // ⚠ WEARING A SHAPE COMMITS IT. A click on a library entry is a
            // deliberate "this is the shape I want", so it becomes the state
            // leaving the page puts back rather than the state leaving the page
            // undoes. Without this the library would be unusable: every shape
            // you wore would be taken off again the moment you left.
            g.saved = g.current;
            spdlog::info("Shape: wore '{}' on '{}' ({} morph(s)).", a_shape.name,
                         a_actor->GetName() ? a_actor->GetName() : "(unnamed)",
                         g.current.morphs.size());
            EditorStyle::PlayUISound("UIMenuOK");
        }

        void DrawLibrary(RE::Actor* a_actor, bool a_narrow) {
            FUCK::TextDisabled("$FR_Shape_ShapesHeading"_T);
            if (a_narrow && ChamferPanel::Button("$FR_Shape_BackToSliders"_T).clicked) {
                g.narrowLibrary = false;
            }
            FUCK::Separator();

            const auto library = BodyMorphPresets::Get();
            FUCK::BeginChild("shape_library_rows", ImVec2(0, 0), false);
            if (!library || library->shapes.empty()) {
                OS::ui::TextDisabledWrapped("$FR_Shape_NoShapes"_T);
            }
            for (const auto& shape : library ? library->shapes
                                             : std::vector<BodyMorphPresets::Shape>{}) {
                FUCK::PushID(shape.id.c_str());

                const auto trash = Icons::Utf8(Icons::kTrash);
                const auto share = Icons::Utf8(Icons::kFileImport);
                // ⚠ THE TRAILING BUTTONS ARE MEASURED, NOT PREDICTED, AND THAT
                // IS THE THIRD ATTEMPT AT THIS ROW. The first reserved twice
                // the TRASH glyph's width for two buttons whose labels are
                // different glyphs; the second reserved each glyph's own width
                // plus FramePadding. Both were still cut off in the field
                // (user 2026-08-07, twice), because the width of a FUCK::Button
                // is FUCK's to decide and there is no sized Button overload to
                // pin it. Predicting it from our own style vars is a guess at
                // another mod's widget metrics.
                //
                // So the row asks instead: draw the buttons, take the rect they
                // ended up with, and spend it on the name NEXT frame. The list
                // redraws every frame, so a one-frame-stale reservation is a
                // reservation that is right from the second frame the pane is
                // ever open and self-corrects if the theme, the font or the UI
                // scale changes underneath it.
                //
                // ⚠ AND IT CANNOT OSCILLATE. A Button sizes itself to its own
                // label, so the measurement does not depend on the name width
                // it feeds; it settles after one frame rather than hunting.
                const float spacing = OS::ui::ItemSpacing().x;
                // Frame one only, before anything has been measured. Deliberately
                // an OVER-estimate: too much reserved is a slightly short name,
                // too little is the clipped button this whole note is about.
                //
                // ⚠ FrameWidgetHeight, NOT GetFrameHeight, now that the two
                // trailing controls are IconButtons: an icon button is an exact
                // SQUARE of the framed-widget height, so its own height is its
                // width and the guess is exact rather than approximate. The
                // ImGui number is 13px short of it on this rig.
                const float trailingGuess =
                    (ChamferPanel::FrameWidgetHeight() + spacing) * 2.0f;
                const float trailingW =
                    g.rowTrailingW > 0.0f ? g.rowTrailingW : trailingGuess;

                // ⚠ A Selectable AND NOT A Button, because only Selectable takes
                // a size here. A Button sizes itself to its label, so a shape
                // called "A" and one called "Something Long" would put their
                // export and delete controls in different places down the same
                // column.
                //
                // ⚠ AND IT IS GIVEN THE FRAME HEIGHT EXPLICITLY. A Selectable
                // left to size itself is one line of text tall and a Button is a
                // frame tall, so the row came out as a short name beside two
                // taller icons with their tops and bottoms not lining up with
                // anything (user 2026-08-07, "the buttons don't match the height
                // and the row is awkward"). One height for all three, taken from
                // the same source the buttons use, and the row is a row.
                //
                // ⚠⚠ AND THAT SOURCE IS ChamferPanel::FrameWidgetHeight NOW,
                // not GetFrameHeight, because that is what the two buttons
                // beside it became. The identical mistake shipped once already
                // and cost a field round: GetFrameHeight is fontSize plus twice
                // FramePadding.y, which is 36 here, while a FLICK-shaped widget
                // pads by 8 * scale a side and comes out near 49. A Selectable
                // left on the ImGui number would sit 13px short of its own row
                // with the tops level and the bottoms not, which is exactly the
                // report this comment block was already about.
                const float rowH  = ChamferPanel::FrameWidgetHeight();
                const float nameW =
                    std::max(1.0f, FUCK::GetContentRegionAvail().x - trailingW);
                if (FUCK::Selectable(shape.name.c_str(), false, 0, ImVec2(nameW, rowH))) {
                    WearShape(a_actor, shape);
                }
                if (FUCK::IsItemHovered()) {
                    // ⚠ THE SET IS NAMED IN THE TOOLTIP RATHER THAN ENFORCED. A
                    // shape built on another body mostly still means something,
                    // since bodies share slider names, and refusing it outright
                    // would block the case where the user knows better.
                    //
                    // Built by hand because FUCK::SetTooltip is not variadic.
                    std::string tip = Tr("$FR_Shape_ApplyTip");
                    tip += "\n";
                    tip += Tr("$FR_Shape_BuiltFor");
                    tip += ": ";
                    tip += shape.set.empty() ? Tr("$FR_Shape_UnknownSet") : shape.set;
                    tip += "\n";
                    tip += std::to_string(shape.values.size());
                    tip += " ";
                    tip += Tr("$FR_Shape_SliderWord");
                    FUCK::SetTooltip(tip.c_str());
                }

                FUCK::SameLine();
                // Where the trailing block starts, for the measurement below.
                // Taken AFTER the SameLine, so the gap in front of it is not in
                // this span and is added back explicitly.
                const float trailingX0 = FUCK::GetCursorScreenPos().x;
                const auto  shareBtn   = ChamferPanel::IconButton(share.c_str());
                if (shareBtn.clicked) {
                    BodyMorphPresets::SetExported(shape.id, !shape.exported);
                }
                // ⚠ THE RESULT'S HOVER, NOT IsItemHovered. A chamfered button
                // draws its label through OS::ui::TextAt, which submits a real
                // item, so the last-item record after this call describes the
                // GLYPH and not the button under it.
                if (shareBtn.hovered) {
                    FUCK::SetTooltip(shape.exported ? "$FR_Shape_Unexport"_T
                                                    : "$FR_Shape_Export"_T);
                }

                FUCK::SameLine();
                // ⚠ A MODAL, NOT A BUTTON THAT CHANGES ITS OWN LABEL. The
                // two-click version relabelled itself to ask, which made the
                // button grow and shoved the row's layout sideways under the
                // cursor (user 2026-08-07). Deleting an outfit already asks in a
                // popup, and so does deleting a Body Studio preset; this is the
                // same question and gets the same shape.
                //
                // ⚠ THE POPUP IS REQUESTED HERE AND OPENED AT THE TOP LEVEL,
                // which is why this sets a flag rather than calling OpenPopup.
                // OpenPopup and BeginPopupModal hash their id against the
                // CURRENT id stack: this button sits inside a child AND inside
                // PushID(shape.id), and the modal is begun outside both, so
                // opening here produced a different popup from the one being
                // begun and the trash button did nothing (user 2026-08-07,
                // "sometimes the delete button of shapes don't work"). Same trap
                // OS-29 recorded for the outfit tab's X.
                const auto deleteBtn = ChamferPanel::IconButton(trash.c_str());
                // ⚠⚠ THE BUTTON'S OWN RIGHT EDGE, OFF THE RESULT. This used to
                // read GetItemRectMax and had a note about reading it before the
                // tooltip, because the last-item record is one global and a
                // tooltip would overwrite it. That hazard is gone and a worse
                // one replaced it: a chamfered button's label goes through
                // OS::ui::TextAt, which submits an item of its own, so the
                // last-item record after this call is the GLYPH's rect. The row
                // would have reserved the width of a trash character and clipped
                // both buttons - the very failure the note above spent three
                // attempts on. The result carries the rect precisely so no
                // caller has to ask.
                g.rowTrailingW = (deleteBtn.max.x - trailingX0) + spacing;
                if (deleteBtn.clicked) {
                    g.pendingDeleteId   = shape.id;
                    g.pendingDeleteName = shape.name;
                    g.requestDeletePopup = true;
                }
                if (deleteBtn.hovered) {
                    FUCK::SetTooltip("$FR_Shape_DeleteTip"_T);
                }
                FUCK::PopID();
            }
            FUCK::EndChild();
        }

        // ---- the editor (right pane) ----------------------------------------

        // One slider row: name, control, reset. The same three-column table Body
        // Studio's slider list uses, so the two pages read as one product.
        //
        // ⚠ TEMPLATED ON THE CALLBACK RATHER THAN TAKING A std::function. A body
        // can describe four hundred sliders and this runs once per visible row
        // per frame; a type-erased callback would be an allocation on each of
        // them for nothing.
        template <typename Fn>
        void DrawSliderRow(const char* a_id, const char* a_label, const char* a_tip,
                           float* a_value, float a_min, float a_max, float a_default,
                           Fn&& a_onChange) {
            FUCK::PushID(a_id);
            FUCK::TableNextRow();
            FUCK::TableNextColumn();
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(a_label);
            if (a_tip && *a_tip && FUCK::IsItemHovered()) {
                FUCK::SetTooltip(a_tip);
            }
            FUCK::TableNextColumn();
            FUCK::SetNextItemWidth(-1.0f);
            if (FUCK::SliderFloat("##value", a_value, a_min, a_max, "%.2f")) {
                // ⚠ THE SNAPSHOT IS TAKEN ON THE FIRST FRAME OF A DRAG, NOT ON
                // EVERY CHANGE. A drag reports a change on most frames it moves,
                // so remembering each one would fill the undo stack with sixty
                // steps of one gesture and Undo would appear to do nothing.
                if (!g.dragOpen) {
                    g.dragBefore = g.current;
                    g.dragOpen   = true;
                    // ⚠ INSIDE THE FIRST-FRAME GUARD, not beside a_onChange. A
                    // drag reports a change on most frames it moves, and a
                    // tutorial step that satisfied itself sixty times would play
                    // its cue sixty times. This branch is already the one that
                    // means "a gesture has started".
                    OS::Tutorial::NotifyAction(OS::Tutorial::Action::kMovedShapeSlider);
                }
                a_onChange();
            }
            if (FUCK::IsItemDeactivatedAfterEdit()) {
                a_onChange();
                if (g.dragOpen) {
                    const Edit before = g.dragBefore;
                    g.dragOpen        = false;
                    Remember(before);
                }
            }
            FUCK::TableNextColumn();
            const auto resetBtn =
                ChamferPanel::IconButton(Icons::Utf8(Icons::kTimes).c_str());
            if (resetBtn.clicked) {
                const Edit before = g.current;
                *a_value          = a_default;
                a_onChange();
                Remember(before);
            }
            if (resetBtn.hovered) {
                FUCK::SetTooltip("$FR_Shape_ResetSlider"_T);
            }
            FUCK::PopID();
        }

        [[nodiscard]] bool BeginSliderTable(const char* a_id) {
            if (!FUCK::BeginTable(a_id, 3,
                                  FUCK::TableFlags::kRowBg |
                                      FUCK::TableFlags::kBordersInnerH |
                                      FUCK::TableFlags::kSizingStretchProp)) {
                return false;
            }
            FUCK::TableSetupColumn("Name", FUCK::TableColumnFlags::kWidthStretch, 0.42f);
            FUCK::TableSetupColumn("Value", FUCK::TableColumnFlags::kWidthStretch, 0.50f);
            FUCK::TableSetupColumn("Reset", FUCK::TableColumnFlags::kWidthFixed,
                                   OS::ui::FontSize() * 2.0f);
            return true;
        }

        void DrawProportions(RE::Actor* a_actor) {
            auto lastGroup = kSliders[0].group;
            for (std::size_t i = 0; i < kSliderCount; ++i) {
                const bool newGroup = i == 0 || kSliders[i].group != lastGroup;
                if (newGroup) {
                    if (i != 0) {
                        FUCK::EndTable();
                    }
                    lastGroup = kSliders[i].group;
                    FUCK::TextDisabled("%s", ShapeOverlay::GroupLabel(lastGroup));
                    if (!BeginSliderTable(ShapeOverlay::GroupLabel(lastGroup))) {
                        return;
                    }
                }
                DrawSliderRow(kSliders[i].id, Tr(kSliders[i].key),
                              "$FR_Shape_SliderTip"_T, &g.current.nodes[i],
                              kSliders[i].minScale, kSliders[i].maxScale,
                              ShapeOverlay::kDefaultScale,
                              [a_actor, i] { PushNode(a_actor, i); });
            }
            FUCK::EndTable();
        }

        void DrawMorphs(RE::Actor* a_actor, const BodyMorphCatalog::Snapshot& a_snap) {
            const bool searching = !g.search.empty();
            for (const auto& cat : a_snap.categories) {
                std::vector<const BodyMorphCatalogParse::SliderEntry*> shown;
                for (const auto& slider : cat.sliders) {
                    if (BodyMorphCatalogParse::Matches(slider, g.search)) {
                        shown.push_back(&slider);
                    }
                }
                if (shown.empty()) {
                    continue;
                }
                const std::string heading =
                    cat.name + "  (" + std::to_string(shown.size()) + ")";

                // ⚠ SetNextItemOpen ONLY WHILE SEARCHING. Its default condition
                // is Always, so calling it unconditionally re-asserts the state
                // every frame: passing false forced every group shut again the
                // instant the click that opened it was processed, and no
                // accordion on this page could be opened at all (field
                // 2026-08-07). A search still opens every group that matched.
                // ⚠ THE FOLD REQUEST OUTRANKS THE SEARCH, and only because it
                // cannot last: it is cleared at the end of this frame, so a
                // search that is still running re-asserts its own open on the
                // next one and a "collapse all" mid-search reads as a flicker
                // rather than as a fight. Both obey the same rule the note
                // above records - assert on ONE frame, never every frame.
                if (!OS::ui::FoldAll::Before() && searching) {
                    FUCK::SetNextItemOpen(true);
                }
                const bool open = FUCK::CollapsingHeader(heading.c_str());
                // ⚠ READ BEFORE THE `continue`, so a COLLAPSED accordion still
                // carries the menu.
                OS::ui::FoldAll::After();
                if (!open) {
                    continue;
                }
                if (!BeginSliderTable(("morphs_" + cat.name).c_str())) {
                    continue;
                }
                for (const auto* slider : shown) {
                    const std::string name = slider->name;
                    const auto        it    = g.current.morphs.find(name);
                    float             value = it == g.current.morphs.end() ? 0.0f : it->second;
                    DrawSliderRow(name.c_str(), slider->display.c_str(),
                                  slider->name.c_str(), &value, kMorphMin, kMorphMax,
                                  0.0f, [a_actor, name, &value] {
                                      PushMorph(a_actor, name, value);
                                  });
                }
                FUCK::EndTable();
            }
        }

        void DrawEditor(RE::Actor* a_actor, bool a_narrow) {
            if (a_narrow && ChamferPanel::Button("$FR_Shape_ToLibrary"_T).clicked) {
                g.narrowLibrary = true;
            }
            // ⚠ THE INTRO LINE IS GONE, AND THE TUTORIAL IS WHY (user
            // 2026-08-08). "These sliders belong to the character" was standing
            // help text: it explained the page once and then sat above it for
            // every visit afterwards, which is what a tutorial card is for and
            // a page is not. $FR_Tut_Shape1 says the same thing, once, to
            // someone who has not been told yet.
            //
            // The key stays in the translations file. Nothing else reads it,
            // and deleting a translated string is a cost for every translator
            // to save nothing here.

            const auto searchIcon = Icons::Utf8(Icons::kSearch);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(searchIcon.c_str());
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            FUCK::InputText("##morph_search", &g.search);
            if (FUCK::IsItemHovered() && g.search.empty()) {
                FUCK::SetTooltip("$FR_Shape_Search"_T);
            }
            FUCK::Separator();

            FUCK::BeginChild("shape_slider_rows", ImVec2(0, 0), false);

            FUCK::TextDisabled("$FR_Shape_BodyHeading"_T);
            if (!RaceMenuMorphApi::Available()) {
                // ⚠ THE REASON, NOT ONE CATCH-ALL. This branch used to reuse
                // "RaceMenu is not loaded" for every failure, which is a lie on
                // the only load orders that reach it: RaceMenu answered the
                // exchange and handed us NiTransform in the same millisecond,
                // and a user told it was not loaded goes and reinstalls a mod
                // that was never missing. The one cause that actually happens
                // in the field is an older bundled skee64.dll winning the
                // overwrite, so that case says so and names the file.
                switch (RaceMenuMorphApi::GetStatus()) {
                    case RaceMenuMorphApi::Status::kTooOld:
                        OS::ui::TextDisabledWrapped("$FR_Shape_MorphsOld"_T);
                        break;
                    case RaceMenuMorphApi::Status::kNoBodyMorph:
                        OS::ui::TextDisabledWrapped("$FR_Shape_MorphsMissing"_T);
                        break;
                    default:
                        // ⚠ THE DEFAULT ARM IS THE HONEST ONE NOW. It reached
                        // here for kRaceMenuAbsent, which is the sentence, and
                        // also for kNotRequested and kNoMessaging, which are
                        // not; the transform half above had the same fault and
                        // the field found it first.
                        OS::ui::TextDisabledWrapped(
                            FUCK::Translate(NodeTransformApi::UnavailableKey()));
                        break;
                }
            } else {
                const auto snap = BodyMorphCatalog::Get();
                if (BodyMorphCatalog::Scanning() && (!snap || snap->categories.empty())) {
                    OS::ui::TextDisabledWrapped("$FR_Shape_Scanning"_T);
                } else if (!snap || snap->categories.empty()) {
                    OS::ui::TextDisabledWrapped("$FR_Shape_NoSliders"_T);
                } else {
                    EnsureMorphsRead(a_actor);
                    // ⚠ SAID OUT LOUD WHEN THE LIST IS UNFILTERED. A morph the
                    // built body lacks is stored by RaceMenu and moves nothing,
                    // so silence would read as "this slider is broken" rather
                    // than "we could not tell which body you built".
                    if (!snap->Filtered()) {
                        OS::ui::TextDisabledWrapped("$FR_Shape_Unfiltered"_T);
                    }
                    DrawMorphs(a_actor, *snap);
                }
            }

            // ⚠ IN THE SAME PANE AS THE MORPHS, AND NOW UNDER THEM (user
            // 2026-08-07, then 2026-08-08). The first of those calls brought
            // proportions in from a column of their own, which had made the
            // page two unrelated lists side by side rather than one editing
            // surface with a library beside it. That half stands. What has
            // moved is the order: these are eleven bone scales and the body
            // sliders are what the page is for, so proportions sit below the
            // body and behind a fold rather than above it taking the first
            // screenful.
            //
            // Hidden while searching, unchanged: the search is over the body's
            // slider names, so a proportions header over a list of results that
            // can never contain one is pointing at nothing.
            if (g.search.empty()) {
                FUCK::Spacing();
                // ⚠ A STABLE ID, BECAUSE THE LABEL IS TRANSLATED. A collapsing
                // header keys its open state on the hash of its label, so
                // without this the section would silently spring shut whenever
                // the translation changed or the player switched language, and
                // "###" is what makes ImHashStr restart on the id alone.
                //
                // ⚠ THE "###" WARNING IN FuckCompat.h DOES NOT APPLY HERE. That
                // one is about popups, where OpenPopup and BeginPopupModal are
                // two separate call sites that have to hash alike and "###"
                // breaks the match. A header is one call site, so it is the
                // right tool; "##" would not do, since it still hashes the
                // translated prefix in front of it.
                //
                // ⚠ AND SetNextItemOpen IS NOT CALLED AT ALL. Its default
                // condition is Always, so calling it every frame re-asserts the
                // state and the section cannot be opened by clicking it. That
                // is not theory: it happened on this page and the note in
                // DrawMorphs records the field report.
                const std::string header =
                    std::string("$FR_Shape_ProportionsHeading"_T) + "###shape_proportions";
                // ⚠ THE ONE-SHOT IS THE EXCEPTION THE NOTE ABOVE ALLOWS FOR.
                // "SetNextItemOpen is not called at all" was right while there
                // was no fold-all; a request that clears itself at the end of
                // the frame does not re-assert, so it cannot take this header's
                // click away from it.
                // ⚠ THE ONE-SHOT IS THE EXCEPTION THE NOTE ABOVE ALLOWS FOR. A
                // request that expires at the frame boundary does not re-assert,
                // so it cannot take this header's own click away from it.
                (void)OS::ui::FoldAll::Before();
                const bool propsOpen = FUCK::CollapsingHeader(header.c_str());
                OS::ui::FoldAll::After();
                if (propsOpen) {
                    DrawProportions(a_actor);
                }
            }

            // ⚠ NO Menu() CALL HERE. It is drawn once per frame from the
            // editor's page tail, out where both EndChilds have run, so every
            // page opens it at one id-stack depth. A second call from inside
            // this child would be a second owner of one popup.
            FUCK::EndChild();
            // The whole slider surface. A single slider would be the wrong
            // target: which one is there depends on the body installed.
            OS::Tutorial::PublishAnchor(OS::Tutorial::Anchor::kShapeSliders,
                                        FUCK::GetItemRectMin(),
                                        FUCK::GetItemRectMax());
        }

        // ---- the transaction bar --------------------------------------------

        // ⚠ THE STEP ITSELF, LIFTED OUT OF THE BUTTON, so the keyboard chord and
        // the click cannot be two different edits. They were one block inside
        // `if (undoBtn.clicked)` until the shortcut arrived (2026-08-18), and a
        // second copy under the key handler is exactly the drift this file's
        // other notes keep describing. Safe on an empty stack: the button is
        // greyed then, but a chord is not.
        void Undo(RE::Actor* a_actor) {
            if (g.undo.empty()) {
                return;
            }
            g.redo.push_back(g.current);
            g.current = g.undo.back();
            g.undo.pop_back();
            PushAll(a_actor);
        }

        void Redo(RE::Actor* a_actor) {
            if (g.redo.empty()) {
                return;
            }
            g.undo.push_back(g.current);
            g.current = g.redo.back();
            g.redo.pop_back();
            PushAll(a_actor);
        }

        void DrawTransactionBar(RE::Actor* a_actor) {
            FUCK::Separator();
            // ⚠ FIRST ON THE BAR, AND IT IS EditorUI'S BUTTON, the same call in
            // the same place as Body Studio's and Overlays' bars (user
            // 2026-08-19: "in the bottom bar of all pages"). It was a checkbox
            // stacked above these sliders until then.
            OS::EditorUI::DrawHideOutfitButton();
            FUCK::SameLine();
            const bool dirty = Dirty();

            // ⚠⚠ EVERY CONDITION IS A LOCAL AND IT IS PASSED TWICE, ONCE TO
            // BeginDisabled AND ONCE TO THE BUTTON. ImGui's disabling reaches
            // the InvisibleButton underneath a chamfered button, so the click is
            // refused, but it does NOT reach the fill and the outline: those are
            // ours and nothing multiplies their alpha. A converted button inside
            // BeginDisabled alone would go dead while still looking live. The
            // local exists so the two arguments cannot drift, and because some
            // of these bodies mutate the very thing the condition tests.
            const bool noUndo = g.undo.empty();
            FUCK::BeginDisabled(noUndo);
            const auto undoBtn =
                ChamferPanel::IconButton(Icons::Utf8(Icons::kUndo).c_str(), noUndo);
            if (undoBtn.clicked) {
                Undo(a_actor);
            }
            FUCK::EndDisabled();
            // ⚠ THE RESULT'S HOVER, AND IT STILL ANSWERS WHILE DISABLED. These
            // tooltips exist to say why a control is greyed, so a hover that
            // went quiet under BeginDisabled would silence them in exactly the
            // state they were written for. ChamferPanel::Button reads
            // IsItemHovered with AllowWhenDisabled for that reason.
            if (undoBtn.hovered) {
                FUCK::SetTooltip("$FR_Shape_Undo"_T);
            }

            FUCK::SameLine();
            const bool noRedo = g.redo.empty();
            FUCK::BeginDisabled(noRedo);
            const auto redoBtn =
                ChamferPanel::IconButton(Icons::Utf8(Icons::kRedo).c_str(), noRedo);
            if (redoBtn.clicked) {
                Redo(a_actor);
            }
            FUCK::EndDisabled();
            if (redoBtn.hovered) {
                FUCK::SetTooltip("$FR_Shape_Redo"_T);
            }

            FUCK::SameLine();
            FUCK::BeginDisabled(!dirty);
            const auto discardBtn =
                ChamferPanel::Button("$FR_Shape_Discard"_T, 0.0f, !dirty);
            if (discardBtn.clicked) {
                g.current = g.saved;
                g.undo.clear();
                g.redo.clear();
                PushAll(a_actor);
            }
            FUCK::EndDisabled();
            if (discardBtn.hovered) {
                FUCK::SetTooltip("$FR_Shape_DiscardTip"_T);
            }

            FUCK::SameLine();
            // ⚠ A SHAPE WITH NOTHING IN IT IS NOT SAVEABLE. It would write a
            // file the user cannot tell apart from the others afterwards.
            const bool noMorphs = g.current.morphs.empty();
            FUCK::BeginDisabled(noMorphs);
            const auto createBtn =
                ChamferPanel::Button("$FR_Shape_Create"_T, 0.0f, noMorphs);
            if (createBtn.clicked) {
                g.newShapeName.clear();
                OS::ui::OpenModal("create_shape");
            }
            FUCK::EndDisabled();
            if (createBtn.hovered) {
                FUCK::SetTooltip(noMorphs ? "$FR_Shape_SaveEmptyTip"_T
                                          : "$FR_Shape_CreateTip"_T);
            }

            FUCK::SameLine();
            const bool anyNode = ShapeOverlay::AnyAdjusted(g.current.nodes);
            FUCK::BeginDisabled(!anyNode);
            if (ChamferPanel::Button("$FR_Shape_Reset"_T, 0.0f, !anyNode).clicked) {
                const Edit before = g.current;
                g.current.nodes   = ShapeOverlay::DefaultValues();
                (void)NodeTransformApi::ClearOwned(a_actor);
                Remember(before);
            }
            FUCK::EndDisabled();

            FUCK::SameLine();
            FUCK::BeginDisabled(noMorphs);
            if (ChamferPanel::Button("$FR_Shape_ResetBody"_T, 0.0f, noMorphs).clicked) {
                const Edit before = g.current;
                g.current.morphs.clear();
                (void)RaceMenuMorphApi::ClearShape(a_actor);
                Remember(before);
            }
            FUCK::EndDisabled();

            if (g.requestDeletePopup) {
                g.requestDeletePopup = false;
                OS::ui::OpenModal("delete_shape");
            }

            if (OS::ui::BeginModal("create_shape")) {
                FUCK::TextWrapped("$FR_Shape_CreatePrompt"_T);
                FUCK::SetNextItemWidth(OS::ui::FontSize() * 18.0f);
                FUCK::InputText("##new_shape_name", &g.newShapeName);
                FUCK::Spacing();
                const bool canCreate = !g.newShapeName.empty();
                // ⚠ CentreTwoButtons IS EXACT FOR THESE, not the estimate its
                // own comment describes. It measures each label plus
                // FramePadding.x twice, which is precisely how a chamfered
                // button sizes itself; the uncertainty it warns about was about
                // FUCK::Button, whose width is FUCK's to decide, and it stops
                // applying the moment a site converts.
                OS::ui::CentreTwoButtons(Tr("$FR_Shape_Create"), Tr("$FR_Cancel"));
                FUCK::BeginDisabled(!canCreate);
                if (ChamferPanel::Button("$FR_Shape_Create"_T, 0.0f, !canCreate).clicked) {
                    const auto snap = BodyMorphCatalog::Get();
                    if (BodyMorphPresets::Save(g.newShapeName,
                                               snap ? snap->resolvedSet : std::string{},
                                               AsPairs(g.current.morphs))) {
                        // Saving commits, for the reason WearShape gives: you
                        // do not name a shape you did not mean to keep.
                        g.saved = g.current;
                    }
                    g.newShapeName.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndDisabled();
                FUCK::SameLine();
                if (ChamferPanel::Button("$FR_Cancel"_T).clicked) {
                    g.newShapeName.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndPopup();
            }

            if (OS::ui::BeginModal("delete_shape")) {
                std::string prompt = Tr("$FR_Shape_DeletePrompt");
                prompt += "\n\n";
                prompt += g.pendingDeleteName;
                FUCK::TextWrapped(prompt.c_str());
                FUCK::Spacing();
                OS::ui::CentreTwoButtons(Tr("$FR_Shape_DeleteConfirm"), Tr("$FR_Cancel"));
                if (ChamferPanel::Button("$FR_Shape_DeleteConfirm"_T).clicked) {
                    BodyMorphPresets::Delete(g.pendingDeleteId);
                    g.pendingDeleteId.clear();
                    g.pendingDeleteName.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::SameLine();
                if (ChamferPanel::Button("$FR_Cancel"_T).clicked) {
                    g.pendingDeleteId.clear();
                    g.pendingDeleteName.clear();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndPopup();
            }
        }

    }  // namespace

    void OnOpen(RE::Actor* a_actor) {
        g.loaded = false;
        EnsureLoaded(a_actor);
    }

    void OnClose(RE::Actor* a_actor) {
        // ⚠ THE PREVIEW IS PUT BACK, NOT LEFT ON THE CHARACTER. See the header
        // for why dragging is a preview and what commits it.
        if (g.loaded && a_actor && Dirty()) {
            g.current = g.saved;
            PushAll(a_actor);
            spdlog::info("Shape: left the page with an uncommitted edit on '{}'; "
                         "put back what the page found.",
                         a_actor->GetName() ? a_actor->GetName() : "(unnamed)");
        }
        // ⚠ AND THE PAGE FORGETS. Coming back re-reads the character rather
        // than redrawing whatever was last on screen, which is what makes
        // Discard's baseline mean "the way you found them" on every visit.
        g.loaded = false;
        g.undo.clear();
        g.redo.clear();
        g.dragOpen = false;
    }

    bool HasUnsavedChanges() { return g.loaded && Dirty(); }

    void Draw(RE::Actor* a_actor) {
        if (!NodeTransformApi::Available()) {
            // ⚠⚠ THE KEY COMES FROM THE STATUS, NOT FROM THE BOOL. This printed
            // "RaceMenu is not loaded" whenever the interface was missing for
            // any reason, and on Skyrim SE that sentence is false: RaceMenu is
            // loaded and its NiTransform is older than the floor this build
            // holds. The field reported it as a bug in the detection (Nexus,
            // 2026-08-29 onward) because the page gave them no other reading.
            OS::ui::TextDisabledWrapped(
                FUCK::Translate(NodeTransformApi::UnavailableKey()));
            return;
        }
        if (!a_actor) {
            OS::ui::TextDisabledWrapped("$FR_Shape_NoTarget"_T);
            return;
        }
        // ⚠ THE SAME NOTICE THE BODY ROW CARRIES, ON THE PAGE THAT SHARES
        // ITS PROBLEM (user 2026-08-28). OBody is not up after a coc from the
        // main menu, and this page reads AssignedPreset from it, so the library
        // comes up with nothing to say and no reason given. Present rather than
        // Available on purpose: a load order with no OBody at all is not
        // waiting for anything and must not be told to reload.
        if (ObodyApi::Present() && !ObodyApi::Available()) {
            OS::ui::TextDisabledWrapped("$FR_BodyBusy"_T);
        }
        EnsureLoaded(a_actor);
        WatchStore(a_actor);

        // ⚠ THE SAME WORKBENCH AS BODY STUDIO, DOWN TO THE HELPER THAT SIZES IT
        // (user 2026-08-07: "have the right panel like in body studio for the
        // shape too ... so the layout is similar to body studio"). A library
        // beside a bounded editor with a transaction bar under both is what the
        // Outfit, Dye and Body Studio pages already are; a page that invents its
        // own arrangement costs the user a second thing to learn for no reason.
        const bool narrow = BodyStudioLayout::UseCompact(
            FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad,
            FUCK::GetContentRegionAvail().x, OS::ui::FontSize());
        const float footerH = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f +
                              OS::ui::FramePadding().y * 2.0f +
                              std::max(2.0f, FUCK::GetResolutionScale() * 2.0f);
        const float bodyH = std::max(1.0f, FUCK::GetContentRegionAvail().y - footerH);

        if (narrow) {
            FUCK::BeginChild("shape_workbench_compact", ImVec2(0, bodyH), true);
            if (g.narrowLibrary) {
                DrawLibrary(a_actor, true);
            } else {
                DrawEditor(a_actor, true);
            }
            FUCK::EndChild();
        } else {
            const float gap      = OS::ui::ItemSpacing().x;
            const float width    = FUCK::GetContentRegionAvail().x;
            const float libraryW = BodyStudioLayout::LibraryWidth(width, OS::ui::FontSize());
            FUCK::BeginChild("shape_library", ImVec2(libraryW, bodyH), true);
            DrawLibrary(a_actor, false);
            FUCK::EndChild();
            FUCK::SameLine(0.0f, gap);
            FUCK::BeginChild("shape_editor", ImVec2(0, bodyH), true);
            DrawEditor(a_actor, false);
            FUCK::EndChild();
        }
        DrawTransactionBar(a_actor);

        // The keyboard's half of the bar above, through the same two functions
        // the buttons call. After the bar and outside both panes, which is where
        // the outfit page resolves its own: one place, one step per frame.
        switch (OS::ui::UndoRedo::Poll()) {
            case OS::ui::UndoRedo::Action::kNone:
                break;
            case OS::ui::UndoRedo::Action::kUndo:
                Undo(a_actor);
                break;
            case OS::ui::UndoRedo::Action::kRedo:
                Redo(a_actor);
                break;
        }
    }

}  // namespace OS::ShapeUI
