#include "OverlaysUI.h"

#include "BodyStudioLayout.h"  // the same two-pane sizing every workbench uses
#include "ChamferPanel.h"      // the editor's one button primitive
#include "ColourPicker.h"     // the mod's one colour picker, shared with Dye
#include "EditorStyle.h"
#include "Favorites.h"       // the star on a card, and the Favorites filter
// ⚠⚠ EVERY ACCORDION ON THIS PAGE GOES THROUGH OS::ui::FramedHeader NOW,
// AND NONE OF THEM DID. The page has called FoldAll::Menu() at its tail
// the whole time, so right-clicking a section here opened the expand-all
// menu and then nothing obeyed it: the menu asks through
// FoldAll::Before/After, and ten bare FUCK::CollapsingHeader calls asked
// nothing (user 2026-08-26, "some like in Overlays page right pane do not
// have this feature"). The shared header carries both halves, so a
// section added later cannot forget one of them, and it brings the shut
// state frame every other page already had.
#include "FoldAll.h"
#include "FuckCompat.h"
#include "Icons.h"
#include "MakeupApi.h"
#include "MakeupPlan.h"
#include "Mannequin.h"       // BodyPath, SubjectExtras: the parts a skin card is built from
#include "OverlayApi.h"
#include "OverlayBake.h"     // OS-209: the transient a slider drag shows
#include "OverlayLocations.h"
#include "OverlayPlan.h"
#include "OverlayReconcile.h"  // StoreGeneration + the clone count: the page's second sensor
#include "OverlayTextures.h"
#include "OverlayTransform.h"
#include "EditorNotice.h"   // the centred empty-pane hint
#include "EditorUI.h"       // DrawHideOutfitButton: the one Hide outfit control
#include "EditorWindow.h"   // RequestClose, for the trip into RaceMenu
#include "HostGuard.h"      // which menu hosts the editor, so the trip can dismiss it
#include "OverlayThumbs.h"
#include "PreviewCardUI.h"  // the mod's one card and its grid
#include "RaceTint.h"       // what the race says each tint slot is for
#include "Settings.h"       // the picker's filter is a setting, not a page control
#include "SkinApi.h"        // the skin section: which pack the actor wears
#include "SkinCardScene.h"  // the card that photographs a pack on this character
#include "SkinPacks.h"      // and which packs this rig has
#include "SkinRivals.h"     // and which skins it installed and then overrode
#include "Tutorial.h"       // anchors and actions for the page's tutorial
#include "UndoRedoKeys.h"   // Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y, shared with every other page

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace OS::OverlaysUI {

    namespace {

        // The drag payload's type tag. One per list, as RulesUI's is.
        constexpr const char* kLayerDragType = "FR_OVL_LAYER";

        struct State {
            std::uint32_t owner{ 0 };
            bool          loaded{ false };
            int           selected{ -1 };  // index into OverlayApi::Layers()
            // ⚠⚠ THE ROWS ARE A COPY OF SOMEBODY ELSE'S STORE AND IT MOVES
            // UNDERNEATH THEM (user 2026-08-24: "overlay layers get
            // cleared/hidden in the Overlays page after loading racemenu
            // presets, they can be on the body visually but not in the overlays
            // page, very odd"). Read once on page entry was the whole of the
            // freshness policy, and `LoadCharacterPresetEx` replaces skee's
            // entire override store with the preset's own, which a bare face
            // preset does not carry. This is the generation the rows were read
            // at; when OverlayReconcile's has moved past it, they are re-read.
            std::uint32_t readAtGeneration{ 0 };
            // What the BODY is actually wearing, counted at the same moment the
            // rows were read. The page has never had a second sensor, so an
            // empty store and a bare body were the same picture; they are not
            // the same thing and the player can see the difference.
            int           paintedClones{ -1 };

            // Parallel to OverlayApi::Layers(), never indexed independently.
            std::vector<OverlayPlan::LayerState> layers;

            std::string search;
            // The skin cards' own search, and the page's Favorites filter
            // (user 2026-08-18: a search field for the skin cards, and stars
            // on cards). One filter for the three grids on this page, because
            // the checkbox sits beside whichever search box is on screen and a
            // control must not mean two things depending on the pane. Session
            // local and off on every open, exactly as the editor's is.
            std::string skinSearch;
            bool        favoritesOnly{ false };
            bool        texturesRequested{ false };
            // Which row is in flight, so the list can dim it and only draw an
            // insertion line inside the location the drag started in. Cleared
            // on drop and whenever no drag is active.
            int         dragFrom{ -1 };
            // The picker's remembered hue and saturation, and whether a drag on
            // it is in progress. See ColourPicker::Carry.
            ColourPicker::Carry tintCarry{};
            bool                tintDragging{ false };
            // ⚠⚠ ONE LATCH PER SLIDER, AND THEY WERE MISSING ENTIRELY. Field,
            // 2026-08-16: "i can't undo change when i adjust strength". Both of
            // these sliders wrote through to a layer and called Push without
            // ever calling Remember, so a Strength drag took no history step at
            // all. Two symptoms, and the second is worse than the first: as the
            // FIRST edit on the page it left Undo greyed out with nothing on the
            // stack, and after an earlier edit the NEXT Remember snapshotted a
            // state that already contained the Strength change, so one Undo
            // rolled back the earlier action and left Strength where it was.
            // ⚠ A LATCH RATHER THAN A CALL AT THE TOP OF THE BRANCH, because a
            // slider reports a change on most frames it moves and sixty steps
            // for one gesture make Undo look like it does nothing.
            bool                alphaDragging{ false };
            bool                glowDragging{ false };
            // The two Finish sliders share one latch (OS-226 S1), the way the
            // four Position sliders do: one history step per gesture, whichever
            // of the pair moved.
            bool                finishDragging{ false };
            // The four Position sliders share one latch (OS-209): one history
            // step per gesture whichever of them moved, and the commit is the
            // gesture's release, not a frame.
            bool                posDragging{ false };
            // ⚠⚠ THE HEX FIELDS NEED A LATCH FOR A DIFFERENT REASON, and
            // without one they quietly destroy the history the sliders were just
            // taught to write. A ColorEdit3 returns true on every CHARACTER
            // edited, so typing one six digit colour pushed up to six whole page
            // snapshots. kUndoLimit is 64, so about eleven typed colours evicted
            // every real step off the bottom of the stack and Undo walked back
            // one keystroke at a time, which reads as Undo doing nothing.
            // ⚠ CLEARED ON IsAnyItemActive AND NEVER ON A LAST-ITEM QUERY. The
            // commit query the sliders use is measured dead under a ColorEdit3
            // in this FUCK build, because it is several sub-items.
            bool                hexEditing{ false };
            bool                glowHexEditing{ false };
            // Whole-page snapshots. See the note above Remember.
            //
            // ⚠⚠ A STEP CARRIES BOTH HALVES OF THE PAGE. Makeup started with no
            // history at all, on the argument that it can be read back and so
            // deserves a true Revert instead. The field disagreed and was right
            // (2026-08-16: "i can't undo change when i adjust strength"): Revert
            // answers "put it all back", and undo answers "take back the last
            // thing I did", and a page with only the first makes every
            // experiment cost the whole session's work.
            //
            // One history rather than two, because the bar has one Undo button
            // and a control must not mean two different things depending on
            // which half of the page was touched last. A step that moved only
            // one half restores the other to a value it already holds, which
            // costs nothing: both apply paths write only what differs.
            struct Step {
                std::vector<OverlayPlan::LayerState> layers;
                MakeupPlan::Snapshot                 makeup;
                // The skin pack, a third half of the same step and for the
                // same reason: one Undo button, one history. See the skin
                // block at the foot of this struct.
                std::string                          skin;
            };
            std::vector<Step> undo;
            std::vector<Step> redo;
            bool        narrowPicker{ false };  // compact layout: showing the picker
            // The layer whose content changed this frame, for the flash. See
            // TakeChangedNode in the header for why the page cannot flash it.
            std::string changedNode;

            // ---- makeup: a fifth section, and a different system ------------
            //
            // ⚠⚠ THIS IS NOT A FIFTH LOCATION AND NOTHING BELOW MAY TREAT IT AS
            // ONE. The four above are [Ovl] node overrides skee owns; these are
            // the head's TINT MASK layers, which live on the player object, are
            // written as plain fields and are made visible by a face retint. The
            // two share this page and share nothing else, which is why the state
            // is parallel rather than folded into `layers`.
            //
            // ⚠ THE SELECTION IS SEPARATE AND THE TWO ARE MUTUALLY EXCLUSIVE.
            // One index into two unrelated lists is how a makeup slider ends up
            // writing an overlay node.
            std::vector<MakeupPlan::Layer> makeupLayers;
            MakeupPlan::Snapshot           makeup;
            // ⚠⚠ WHAT THE RACE SAYS EACH SLOT IS FOR, read once per character
            // beside the layers themselves. This is what a Reset puts back and
            // what the Dirt library is built from, and it is parallel to the two
            // above by index. See RaceTint.
            std::vector<RaceTint::Slot>    makeupRace;
            // ⚠⚠ THE STATE THE PLAYER WALKED IN WITH, AND IT IS A MEASUREMENT
            // RATHER THAN A REMEMBERED CLAIM. The overlays half keeps a history
            // because skee cannot be asked what an override held before it was
            // written. Tint masks CAN be read back, so arrival is simply read
            // and one button puts it back. That is worth more here than a stack,
            // and it is the user's call (2026-08-16).
            MakeupPlan::Snapshot makeupArrival;
            bool                 makeupLoaded{ false };
            int                  makeupSelected{ -1 };
            std::string          makeupSearch;
            ColourPicker::Carry  makeupCarry{};
            // ⚠⚠ TWO LATCHES, NOT ONE, BECAUSE THE TWO CONTROLS COMMIT
            // DIFFERENTLY. A slider can say when its edit ENDED
            // (IsItemDeactivatedAfterEdit), whatever device drove it. A
            // ColorEdit3 cannot: it is several sub-items and the query is
            // measured dead under it in this FUCK build, so the picker has only
            // the mouse to go on. Sharing one flag would make the slider's
            // commit depend on the mouse it may never have used.
            bool                 makeupDragging{ false };         // the picker
            bool                 makeupStrengthDragging{ false };  // the slider
            // ⚠ THE POSITION SLIDERS GET THEIR OWN LATCH, and FlushMakeup's gate
            // names it too. The comment there says a new makeup control brings a
            // new latch; this is that control (OS-209 on makeup). Four sliders
            // share one flag because they are one gesture as far as the write is
            // concerned: whichever of them is moving, the layer is mid-edit and
            // must not pay a bake and a face retint per frame.
            bool                 makeupPosDragging{ false };
            // The identity of the list the snapshot was read from. See
            // MakeupApi::Fingerprint: a RaceMenu preset load rebuilds the list
            // under an open page, and writes into the old indices go nowhere a
            // player can see.
            std::uint64_t        makeupFingerprint{ 0 };

            // ---- skin: the base layer, and a third system ------------------
            //
            // ⚠⚠ NOT A LOCATION AND NOT A TINT MASK. The four locations are
            // [Ovl] node overrides on clones of the skin; the skin itself is the
            // texture set of the body, hands and feet shapes, and this page
            // changes it through skee's ARMOUR overrides via SkinApi. It sits on
            // this page because it is the layer everything else here is painted
            // over, and a player looking for their skin looks where their body
            // paint is.
            //
            // ⚠ THE SELECTION IS A THIRD EXCLUSIVE, held the same way makeup
            // is: SelectSkin clears the other two, and either of them clears
            // this.
            bool        skinSelected{ false };
            // The pack the actor wears per SkinApi, empty for the game's own
            // skin. Read on open and on every target switch, and it is what a
            // history step carries.
            std::string skinCurrent;
            // ⚠⚠ A LINK THAT FAILED HAS TO BE VISIBLE. The reason itself stays
            // in the log rather than on screen, because a Windows error phrased
            // in a C++ string literal is a piece of user-facing copy that never
            // goes through the translation file and so is never reviewed. The
            // page says that it failed and where to look; the log says why.
            bool        skinLinkFailed{ false };
        } g;

        [[nodiscard]] const std::vector<OverlayPlan::Layer>& Layers() {
            return OverlayApi::Layers();
        }

        [[nodiscard]] bool Selected() {
            return g.selected >= 0 && g.selected < static_cast<int>(Layers().size());
        }

        // ---- makeup helpers -------------------------------------------------

        [[nodiscard]] bool MakeupSelected() {
            return g.makeupSelected >= 0 &&
                   g.makeupSelected < static_cast<int>(g.makeup.size());
        }

        // ⚠ ONE SELECTION ACROSS TWO LISTS, HELD BY CLEARING THE OTHER. The two
        // halves index unrelated things, so a page that let both be live would
        // draw one editor while the shot driver framed the other, and a stale
        // index left behind is a write aimed at whatever now sits at it.
        void SelectOverlay(int a_index) {
            g.selected       = a_index;
            g.makeupSelected = -1;
            g.skinSelected   = false;
        }

        [[nodiscard]] bool SkinSelected() { return g.skinSelected; }

        void SelectSkin() {
            g.skinSelected   = true;
            g.selected       = -1;
            g.makeupSelected = -1;
        }

        // ⚠⚠ MAKEUP DOES NOT FLASH, AND IT CANNOT. MEASURED 2026-08-16, which
        // is why this says so rather than leaving a gap somebody fills again.
        //
        // A flash marks a SHAPE. Makeup has none: it is painted into the head's
        // TINT TEXTURE, and the head is one geometry. The walk was narrowed as
        // far as it can be narrowed, to the kFaceGen shapes alone, and the log
        // shows it doing exactly that on a heavily modded head:
        //
        //   lit 2 of 21 shape(s) on the head (makeup)
        //
        // 2 of 21, with thirteen hair parts, the eyes, the lashes and the brows
        // all correctly skipped. And the whole head still washed white, because
        // those two shapes ARE the whole head. There is no smaller region to
        // light, so there is no version of this that marks a lip colour.
        //
        // User's call after seeing it: no flash rather than a wrong one.

        void SelectMakeup(int a_index) {
            g.makeupSelected = a_index;
            g.selected       = -1;
            g.skinSelected   = false;
            // Picking a layer reports too, which is what makes clicking down the
            // list light the face each time. Same call the overlay rows make.
        }

        void ReadMakeup(RE::Actor* a_actor) {
            g.makeupLayers = MakeupApi::Layers(a_actor);
            g.makeup       = MakeupApi::Read(a_actor);
            g.makeupRace   = RaceTint::Slots(a_actor);
            g.makeupFingerprint = MakeupApi::Fingerprint(a_actor);
            // The reload retype (race as the type authority, field
            // 2026-08-16) moved INTO MakeupApi::Layers on 2026-08-22, after
            // ProfileCapture stored all-'freckles' types off the same call
            // this page was quietly correcting for itself. One source, every
            // reader; g.makeupRace stays for the page's own display.
            // ⚠ ARRIVAL IS TAKEN FROM THE SAME READ, not from a second one. Two
            // reads a frame apart can disagree if anything else touched the
            // masks between them, and a Revert to a state the player never saw
            // is worse than no Revert at all.
            g.makeupArrival = g.makeup;
            g.makeupLoaded  = true;
        }

        [[nodiscard]] bool MakeupDirty() {
            return !MakeupPlan::Differences(g.makeupArrival, g.makeup).empty();
        }

        // Write one layer through. A CLICK may use this directly.
        //
        // ⚠⚠ EVERY CALL COSTS A FACE RETINT AND THE RETINT REBUILDS A TEXTURE.
        // That is fine for a click and ruinous for a drag, which is why nothing
        // that drags calls this. See QueueMakeup just below.
        void PushMakeup(RE::Actor* a_actor, std::size_t a_index) {
            MakeupApi::Write(a_actor, { a_index }, g.makeup);
        }

        // ⚠⚠ A DRAG WRITES ONCE, WHEN IT ENDS, AND THIS IS NOT THE SAME PROBLEM
        // THE OVERLAYS HALF HAS. There, a per-frame write costs a node override
        // and a deferred scenegraph push, so the page writes live and the player
        // sees the colour move under the cursor. Here a per-frame write costs a
        // texture REBAKE per frame. The measured recipe is a batch of writes
        // followed by exactly one retint, so a slider records which layer it
        // touched and the flush below sends it when the mouse comes up.
        //
        // The cost is stated plainly rather than hidden: makeup does not follow
        // the cursor while a slider is moving, it lands when the slider is let
        // go. Making it live would mean a texture rebuild per frame.
        int g_makeupPending{ -1 };

        void QueueMakeup(std::size_t a_index) {
            g_makeupPending = static_cast<int>(a_index);
        }

        void FlushMakeup(RE::Actor* a_actor) {
            // ⚠⚠ THE GATE ASKS WHETHER A GESTURE IS IN FLIGHT, NOT WHETHER
            // THE MOUSE IS DOWN, and the difference bit both ways. Asking the
            // mouse, a strength edit made from the keyboard reads "not
            // dragging" on every frame, so this fired on EVERY frame of the
            // edit and paid a face retint, a full texture rebake, for each one:
            // exactly the cost QueueMakeup exists to avoid. The other way, a
            // gesture still held when the page stops drawing never reached a
            // flush at all, and the re-read on the way back in discarded the
            // value with nothing anywhere saying so.
            //
            // The picker's latch is the one consulted here because the picker
            // is the control that cannot report its own commit. The slider
            // flushes itself the moment its edit ends, below.
            // ⚠⚠ EVERY LATCH, NOT JUST THE PICKER'S. Splitting the two
            // controls apart left this asking only about the picker, so a
            // STRENGTH drag passed the gate on every frame and paid a face
            // retint, a full texture rebake, for each one. That is the exact
            // cost the deferral exists to avoid, reintroduced by the fix meant
            // to protect it. A new makeup control gets a new latch and it goes
            // here too.
            if (g_makeupPending < 0 || g.makeupDragging || g.makeupStrengthDragging ||
                g.makeupPosDragging) {
                return;
            }
            const auto index = static_cast<std::size_t>(g_makeupPending);
            g_makeupPending  = -1;
            if (index < g.makeup.size()) {
                PushMakeup(a_actor, index);
                // ⚠ THE DRAG'S ONE FLASH, AND IT FIRES HERE RATHER THAN IN THE
                // CONTROL. The overlays half deliberately does not flash from
                // its colour and alpha controls, on the grounds that the layer
                // is already lit by the thing being dragged. Makeup has the
                // opposite problem: the write is DEFERRED to the end of the
                // drag, so without this there is no moment that says the change
                // landed. One flush is one gesture, so this is still one pulse.
            }
        }

        void ReadAll(RE::Actor* a_actor) {
            const auto& layers = Layers();
            g.layers.assign(layers.size(), OverlayPlan::LayerState{});
            for (std::size_t i = 0; i < layers.size(); ++i) {
                g.layers[i] = OverlayApi::Read(a_actor, layers[i].node);
            }
            // ⚠ STAMPED WITH THE READ, NOT WITH THE PAGE OPEN. The two differ
            // the moment a re-read happens mid-session, and a stamp taken at
            // open would make every later read look stale for ever.
            g.readAtGeneration = OverlayReconcile::StoreGeneration();
            // The second sensor, taken beside the first so the pair describes
            // one moment. -1 means "no 3D to count", which is not zero.
            g.paintedClones =
                (a_actor && a_actor == RE::PlayerCharacter::GetSingleton())
                    ? OverlayReconcile::CountPlayerBodyClones()
                    : -1;
        }

        // Whether the store said nothing while the body is still wearing art.
        // The two readings come from ReadAll and describe the same instant.
        //
        // ⚠ BODY ROWS AGAINST BODY CLONES, NOT EVERY ROW. CountPlayerBodyClones
        // counts `Body [ ... Ovl ... ]` and nothing else, so weighing it against
        // the whole layer list would let one occupied FACE row answer a question
        // about the body and hide the notice in exactly the partial state worth
        // reporting. A preset load empties every location at once, so the two
        // spellings agree in the reported case and disagree only where the
        // narrow one is right.
        [[nodiscard]] bool StoreLostThePaint() {
            if (g.paintedClones <= 0) {
                return false;  // nothing painted, or nothing countable
            }
            const auto& layers = Layers();
            const auto  n      = std::min(layers.size(), g.layers.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (layers[i].location == OverlayPlan::Location::kBody &&
                    OverlayPlan::Occupied(g.layers[i])) {
                    return false;  // the store still owns a body layer
                }
            }
            return true;
        }

        void EnsureLoaded(RE::Actor* a_actor) {
            if (!g.texturesRequested) {
                g.texturesRequested = true;
                OverlayTextures::RequestScan();
            }
            const std::uint32_t owner = a_actor ? a_actor->GetFormID() : 0u;
            if (g.loaded && g.owner == owner) {
                // ⚠ AND THE ROWS ARE RE-READ WHEN THE STORE MOVED, which is the
                // half this early-out used to be missing. A RaceMenu preset
                // loaded while this page is open rewrites skee's store, and
                // without this the rows kept describing the world before it
                // until the player left the page and came back. Only the rows:
                // the selection, the searches and the skin pane are the
                // player's own state and a store rewrite is no reason to
                // disturb them.
                if (OverlayReconcile::StoreGeneration() != g.readAtGeneration) {
                    ReadAll(a_actor);
                }
                return;
            }
            g.owner  = owner;
            g.loaded = true;
            g.selected = -1;
            // The skin half: what this actor wears is read fresh, and the pack
            // folder is rescanned on every open rather than once a session,
            // unlike the overlay art above. A skins folder is a handful of
            // files, and the one thing a player does after reading "no packs
            // found" is make one and come back.
            g.skinSelected   = false;
            g.skinCurrent    = SkinApi::Current(a_actor);
            g.skinLinkFailed = false;
            SkinPacks::RequestScan();
            // And what fits this character, plus what the default is called:
            // a game-thread walk of the live skin shapes, read by the cards.
            SkinApi::RequestFit(a_actor);
            // ⚠ THE MAKEUP HALF IS RE-READ ON EVERY TARGET SWITCH TOO, and its
            // arrival snapshot with it. Carrying a snapshot across a switch would
            // let Revert write the character the player was editing a moment ago
            // onto the one they are editing now, which is the same hazard the
            // history below is cleared for.
            g.makeupSelected = -1;
            // ⚠ THE PENDING FLASH REPORT DIES WITH THE TARGET TOO. A report
            // queued on the frame before a switch is drained after it, and the
            // register it feeds lights a CHARACTER: the new one, for a change
            // made to the old one.
            g.changedNode.clear();
            g.makeupLoaded   = false;
            // ⚠⚠ THE GESTURE LATCHES DIE HERE TOO. These three clear only on
            // a widget's own commit, unlike the two mouse-latched ones which
            // self-heal on any mouse-up. A gesture interrupted by the editor
            // closing or by a target switch would otherwise leave its latch
            // set, and the NEXT gesture on that control would silently take no
            // history step: an undo bug that appears one edit later than the
            // thing that caused it.
            g.alphaDragging          = false;
            g.glowDragging           = false;
            g.finishDragging         = false;
            g.hexEditing             = false;
            g.glowHexEditing         = false;
            g.makeupDragging         = false;
            g.makeupStrengthDragging = false;
            g.makeupPosDragging      = false;
            g.makeupLayers.clear();
            g.makeup.clear();
            g.makeupArrival.clear();
            g.makeupRace.clear();
            g_makeupPending = -1;
            // ⚠ A HISTORY DESCRIBES A CHARACTER. Carrying it across a target
            // switch would let an undo write a follower's layers onto whoever
            // is being edited now.
            g.undo.clear();
            g.redo.clear();

            // ⚠ INSTALL BEFORE READING, AND ONLY ONCE PER CHARACTER. A follower
            // who has never worn an overlay has no overlay nodes at all, so
            // every read comes back empty and every write lands on a node that
            // is not there. The install is deferred inside OverlayApi, so this
            // frame still reads empty and the next one does not, which is why
            // the page is honest about a character it has just installed rather
            // than pretending the slots are ready.
            if (!OverlayApi::HasOverlays(a_actor)) {
                OverlayApi::Install(a_actor);
            }
            ReadAll(a_actor);

            // ⚠ NOTHING IS INSTALLED FOR MAKEUP AND NOTHING CAN BE. The tint
            // list is built by the engine from the race and every character who
            // has one has it in full, so there is no empty state to fill: a
            // target with no list is a target that is not the player, which the
            // section says rather than tries to fix.
            if (MakeupApi::IsPlayer(a_actor)) {
                ReadMakeup(a_actor);
            }
        }

        void Push(RE::Actor* a_actor, std::size_t a_index) {
            const auto& layers = Layers();
            if (a_index >= layers.size() || a_index >= g.layers.size()) {
                return;
            }
            OverlayApi::Write(a_actor, layers[a_index].node, g.layers[a_index]);
            // OS-233: an edit after a failed repair earns the repair one more
            // attempt, and arms its check either way.
            if (MakeupApi::IsPlayer(a_actor)) {
                OverlayReconcile::NoteOverlayEdit();
            }
        }

        // ⚠⚠ THE ONE PLACE A FLASH IS ASKED FOR. Called wherever a layer is
        // picked, or starts or stops wearing something. It reports at most one
        // node per frame and the editor takes it once, so a single user action
        // is a single flash however many of these fire.
        //
        // ⚠ NOT FROM UNDO, REDO OR CLEAR EVERY LAYER, deliberately. Each of
        // those moves a whole page at once, and there is no single layer for a
        // flash to mean: lighting the last one the diff happened to touch would
        // point at an arbitrary slot. Nor from the colour and alpha controls,
        // which change a layer that is already lit by the thing being dragged.
        void NoteChanged(std::size_t a_index) {
            const auto& layers = Layers();
            if (a_index < layers.size()) {
                g.changedNode = layers[a_index].node;
            }
        }

        // ---- undo and redo --------------------------------------------------
        //
        // ⚠ THE WHOLE PAGE IS ONE STEP, NOT ONE LAYER. A reorder moves several
        // slots at once and clearing a location moves as many again, so a
        // per-layer history would need a step that sometimes means one thing
        // and sometimes several, and undoing a drag would take as many presses
        // as it shifted rows.
        //
        // ⚠ AND IT IS THE PAGE'S STATE, NOT skee's. There is no reading back
        // what an override was before it was written, so the only place a
        // previous appearance exists is here. That is also why this history is
        // dropped when the subject changes: it describes a character.
        constexpr std::size_t kUndoLimit = 64;

        // Write out only what actually differs, so an undo costs the engine the
        // handful of layers the step touched rather than all fifteen.
        void ApplyDiff(RE::Actor* a_actor, const std::vector<OverlayPlan::LayerState>& a_from,
                       const std::vector<OverlayPlan::LayerState>& a_to) {
            const auto& layers = Layers();
            for (std::size_t i = 0; i < layers.size() && i < a_to.size(); ++i) {
                if (i < a_from.size() && a_from[i] == a_to[i]) {
                    continue;
                }
                if (OverlayPlan::Occupied(a_to[i])) {
                    OverlayApi::Write(a_actor, layers[i].node, a_to[i]);
                } else {
                    OverlayApi::Clear(a_actor, layers[i].node);
                }
            }
            if (MakeupApi::IsPlayer(a_actor)) {
                OverlayReconcile::NoteOverlayEdit();  // OS-233, see Push
            }
        }

        // Call BEFORE mutating g.layers or g.makeup. Anything that changes what
        // the character wears goes through this, or that change cannot be undone.
        void Remember() {
            g.undo.push_back(State::Step{ g.layers, g.makeup, g.skinCurrent });
            if (g.undo.size() > kUndoLimit) {
                g.undo.erase(g.undo.begin());
            }
            g.redo.clear();
        }

        // Put a whole step back on the character: the overlay half through
        // skee, the makeup half through the engine's tint masks.
        //
        // ⚠ THE MAKEUP HALF DIFFS INSIDE Restore, so a step that never touched
        // makeup writes nothing and costs no retint. That matters more here than
        // it looks: the retint REBUILDS A TEXTURE, so an undo that pushed all
        // fifteen tint layers every time would make the button expensive enough
        // to feel broken.
        void ApplyStep(RE::Actor* a_actor, const State::Step& a_from,
                       const State::Step& a_to) {
            ApplyDiff(a_actor, a_from.layers, a_to.layers);
            if (MakeupApi::IsPlayer(a_actor)) {
                MakeupApi::Restore(a_actor, a_from.makeup, a_to.makeup);
            }
            // The skin half diffs the same way: a step that never touched the
            // pack writes nothing, and one that did puts the pack it recorded
            // back through the one painter.
            if (a_from.skin != a_to.skin) {
                SkinApi::Apply(a_actor, a_to.skin);
            }
        }

        void Undo(RE::Actor* a_actor) {
            if (g.undo.empty()) {
                return;
            }
            auto previous = g.undo.back();
            g.undo.pop_back();
            const State::Step from{ g.layers, g.makeup, g.skinCurrent };
            g.redo.push_back(from);
            g.layers      = previous.layers;
            g.makeup      = previous.makeup;
            g.skinCurrent = previous.skin;
            // ⚠ A QUEUED DRAG IS DROPPED BY AN UNDO. Without this, a strength
            // drag that had not been flushed yet would land AFTER the undo and
            // put the undone value straight back, which reads as the button
            // doing nothing at all.
            g_makeupPending = -1;
            ApplyStep(a_actor, from, previous);
        }

        void Redo(RE::Actor* a_actor) {
            if (g.redo.empty()) {
                return;
            }
            auto next = g.redo.back();
            g.redo.pop_back();
            const State::Step from{ g.layers, g.makeup, g.skinCurrent };
            g.undo.push_back(from);
            g.layers        = next.layers;
            g.makeup        = next.makeup;
            g.skinCurrent   = next.skin;
            g_makeupPending = -1;
            ApplyStep(a_actor, from, next);
        }

        [[nodiscard]] std::size_t UsedIn(OverlayPlan::Location a_location) {
            const auto& layers = Layers();
            std::size_t used   = 0;
            for (std::size_t i = 0; i < layers.size() && i < g.layers.size(); ++i) {
                if (layers[i].location == a_location && OverlayPlan::Occupied(g.layers[i])) {
                    ++used;
                }
            }
            return used;
        }

        // ---- the left pane: the layers themselves ---------------------------

        // Move one layer's whole appearance onto another slot, pushing the ones
        // between it and its destination along by one.
        //
        // ⚠ THE CONTENT MOVES, THE SLOT DOES NOT. A slot's identity is its node
        // name, which skee owns and which nothing here may rename, so reordering
        // is a rotation of what the slots HOLD. That also makes the order real
        // rather than cosmetic: the number beside a layer is its draw order, and
        // dragging it up genuinely repaints it later, over the ones below.
        //
        // ⚠ EVERY SLOT THE ROTATION TOUCHED IS REWRITTEN, not just the two ends.
        // The pair-swap version of this was wrong in a way that survives review:
        // dragging across three slots left the middle one holding the texture it
        // had before, so the list read correctly and the character did not.
        void ReorderWithin(RE::Actor* a_actor, std::size_t a_from, std::size_t a_to) {
            if (a_from == a_to || a_from >= g.layers.size() || a_to >= g.layers.size()) {
                return;
            }
            Remember();
            const auto moved = g.layers[a_from];
            if (a_from < a_to) {
                for (std::size_t i = a_from; i < a_to; ++i) {
                    g.layers[i] = g.layers[i + 1];
                }
            } else {
                for (std::size_t i = a_from; i > a_to; --i) {
                    g.layers[i] = g.layers[i - 1];
                }
            }
            g.layers[a_to] = moved;
            NoteChanged(a_to);

            const auto lo = std::min(a_from, a_to);
            const auto hi = std::max(a_from, a_to);
            for (std::size_t i = lo; i <= hi; ++i) {
                if (OverlayPlan::Occupied(g.layers[i])) {
                    Push(a_actor, i);
                } else {
                    // A slot the rotation emptied has to be told so. Leaving it
                    // is how a texture appears twice after a drag.
                    OverlayApi::Clear(a_actor, Layers()[i].node);
                }
            }
            g.selected = static_cast<int>(a_to);
        }

        // ---- the tint mask sections, which are three kinds of thing ----------
        //
        // ⚠⚠ A FIFTH KIND OF THING RATHER THAN A FIFTH LOCATION, sharing this
        // pane and nothing else. It is drawn in the same accordion the four
        // locations use because that is where a player looks for "what is
        // painted on this character", and everything under it goes through
        // MakeupApi rather than through skee.
        //
        // ⚠⚠ AND IT IS THREE ACCORDIONS RATHER THAN ONE, BECAUSE THEY ARE THREE
        // DIFFERENT CONTROL SETS. MEASURED out of the RACE records: every
        // structural type is one slot per character whose texture is the SHAPE
        // of that feature on that face, so a texture picker on the Lips slot
        // offers art that can only ever be the wrong shape for it. Only war
        // paint and dirt are multi-slot libraries. Renaming the section alone
        // would have left that picker exactly where it was.

        // The race's word on one slot, or nothing when there is none to have.
        //
        // ⚠⚠ THE TYPES MUST AGREE OR THE POSITION MAPPING IS WRONG. The live
        // list and the race list are matched by index, which is how the engine
        // builds one from the other, but nothing enforces it: another mod can
        // insert a layer and every slot after it would then be offered its
        // neighbour's default texture and colour. A disagreement means no
        // default rather than a plausible wrong one.
        [[nodiscard]] const RaceTint::Slot* RaceSlot(std::size_t a_index) {
            if (a_index >= g.makeupRace.size() || !g.makeupRace[a_index].known) {
                return nullptr;
            }
            const auto live =
                a_index < g.makeupLayers.size() ? g.makeupLayers[a_index].type : 0u;
            const auto* const slot = &g.makeupRace[a_index];
            return slot->type == live ? slot : nullptr;
        }

        [[nodiscard]] MakeupPlan::Category CategoryAt(std::size_t a_index) {
            const auto type =
                a_index < g.makeupLayers.size() ? g.makeupLayers[a_index].type : 0u;
            return MakeupPlan::CategoryOf(type);
        }

        void DrawMakeupRow(RE::Actor* a_actor, std::size_t a_index) {
            const auto type =
                a_index < g.makeupLayers.size() ? g.makeupLayers[a_index].type : 0u;
            const auto category = MakeupPlan::CategoryOf(type);
            const bool occupied = MakeupPlan::Occupied(g.makeup[a_index]);
            // ⚠⚠ NO CLEAR ON A FACE SLOT, WHICH IS THE WHOLE POINT OF THE
            // SPLIT. Clearing SkinTone does not remove makeup, it removes the
            // character's complexion, and emptying Neck removes the neck blend.
            // These are not accessories to take off. The Reset in the editor is
            // what a face slot has instead.
            const bool clearable = occupied && MakeupPlan::AllowsClear(category);

            // ⚠ THE TYPE NAMES THE ROW AND THE ART DISAMBIGUATES IT. A race can
            // carry several layers of one type, so the type alone would give a
            // column of rows all reading "War paint" with no way to tell which
            // is which.
            std::string label = std::string{ FUCK::Translate(MakeupPlan::LabelKeyFor(type)) };
            if (occupied) {
                label += "  " + MakeupPlan::DisplayName(g.makeup[a_index].texture);
            }

            FUCK::PushID(static_cast<int>(a_index) + 10000);
            if (!occupied) {
                FUCK::PushStyleColor(ImGuiCol_Text, OS::ui::StyleColor(ImGuiCol_TextDisabled));
            }
            const float rowW = FUCK::GetContentRegionAvail().x;
            // ⚠ THE ROW ONLY GIVES UP THE WIDTH WHEN SOMETHING IS GOING THERE.
            // Reserving the clear column on a face row would leave a ragged gap
            // down the section that nothing ever fills.
            const float clearW =
                clearable ? OS::ui::FontSize() + OS::ui::ItemSpacing().x : 0.0f;
            if (FUCK::Selectable(label.c_str(), g.makeupSelected == static_cast<int>(a_index), 0,
                                 ImVec2(std::max(1.0f, rowW - clearW), 0.0f))) {
                SelectMakeup(static_cast<int>(a_index));
                g.narrowPicker = false;
                // A makeup row is a layer row too; the tutorial's ask is
                // satisfied by either kind.
                Tutorial::NotifyAction(Tutorial::Action::kSelectedOverlayLayer);
            }
            if (!occupied) {
                FUCK::PopStyleColor();
            }

            // ⚠⚠ CLEARING A MAKEUP LAYER IS THE STRENGTH GOING TO ZERO AND NOT
            // THE TEXTURE BEING TAKEN OFF. Every slot carries art at all times
            // because the race gave it some, so blanking the path would leave a
            // layer with no texture at all, which is a state the engine never
            // produces and nothing here could put back.
            if (clearable && FUCK::IsItemClicked(1)) {
                Remember();
                g.makeup[a_index].strength = 0.0f;
                PushMakeup(a_actor, a_index);
            }
            if (FUCK::IsItemHovered() && occupied) {
                std::string tip = g.makeup[a_index].texture;
                if (clearable) {
                    tip += "\n";
                    tip += FUCK::Translate("$FR_Mk_RightClear");
                }
                FUCK::SetTooltip(tip.c_str());
            }
            if (clearable) {
                FUCK::SameLine();
                if (FUCK::Selectable(Icons::Utf8(Icons::kTimes).c_str(), false, 0,
                                     ImVec2(OS::ui::FontSize(), 0.0f))) {
                    Remember();
                    g.makeup[a_index].strength = 0.0f;
                    PushMakeup(a_actor, a_index);
                }
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip("$FR_Mk_ClearLayer"_T);
                }
            }
            FUCK::PopID();
        }

        // One section, drawn only when the character has slots of that kind.
        //
        // ⚠ AN EMPTY SECTION IS ABSENT RATHER THAN EMPTY. Every race measured
        // carries face, war paint and dirt slots, so in practice this hides the
        // "other" heading, which exists for types no build has heard of and is
        // exactly the heading a player should never see on a normal character.
        void DrawMakeupSection(RE::Actor* a_actor, MakeupPlan::Category a_category) {
            std::vector<std::size_t> rows;
            for (std::size_t i = 0; i < g.makeup.size(); ++i) {
                if (CategoryAt(i) == a_category) {
                    rows.push_back(i);
                }
            }
            if (rows.empty()) {
                return;
            }
            const auto worn = MakeupPlan::UsedIn(g.makeup, g.makeupLayers, a_category);
            // The same "###" trick the four locations use: the visible half
            // carries the count and the id half is stable, so the open state
            // survives both a language change and a layer being filled.
            const auto header =
                std::string{ FUCK::Translate(MakeupPlan::CategoryLabelKey(a_category)) } +
                "   " + std::to_string(worn) + " / " + std::to_string(rows.size()) +
                "###ovl_mk_" + MakeupPlan::CategoryId(a_category);
            // ⚠ OPEN ON ARRIVAL, LIKE THE FOUR ABOVE (user 2026-08-16). A shut
            // accordion hides that the section exists at all.
            const bool open =
                OS::ui::FramedHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            // ⚠ THE HINT IS A TOOLTIP ON THE HEADER, NOT A LINE UNDER IT (user
            // 2026-08-17). It answers "what are these" once, which is a question
            // a player asks on the way past and not every time they open the
            // section, and a permanent paragraph pushed the first row down.
            // ⚠ ASKED BEFORE THE EARLY RETURN, so a SHUT face section still
            // answers it. That is the one state where the rows are not there to
            // explain themselves.
            if (a_category == MakeupPlan::Category::kFace && FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_Mk_FaceNote"_T);
            }
            if (!open) {
                return;
            }
            for (const auto row : rows) {
                DrawMakeupRow(a_actor, row);
            }
        }
        //
        // The trip into RaceMenu, from inside the editor. The editor is HOSTED
        // over a menu it does not own (HostGuard), and the RaceSex Menu is a
        // full-screen editor of its own, so the order is: close ourselves, hide
        // the host, show RaceMenu, all on the UI queue so they land in that
        // order. A SAM host is left alone: it is a context, not a vanilla menu,
        // and SamCompat owns that seam. Nothing here waits for RaceMenu to
        // close; the next editor open re-reads whatever it built.
        //
        // ⚠ ONE FUNCTION, because it is about to have two callers: the makeup
        // list's "no layers yet" note today, and the "read RaceMenu's lists"
        // button of the overlay-placement scrape (OS-219 route B) next. Two
        // spellings of the same trip would drift in exactly the step order that
        // matters.
        void OpenRaceMenuFromEditor() {
            auto* const q = RE::UIMessageQueue::GetSingleton();
            if (!q) {
                return;
            }
            OS::EditorWindow::RequestClose();
            const auto host = HostGuard::CurrentHost();
            if (host.Present() && !host.isSam) {
                q->AddMessage(host.name, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            q->AddMessage(RE::RaceSexMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            spdlog::info("OverlaysUI: opening RaceMenu from the editor (host '{}' {}).",
                         host.name, host.Present() && !host.isSam ? "hidden first" : "left alone");
        }

        // ⚠ NO DRAG TO REORDER HERE, AND IT IS NOT AN OVERSIGHT. Overlay layers
        // are a stack whose ORDER is the feature. Tint mask layers are not: each
        // one has a fixed type the engine looks it up by, and moving a lip
        // colour into the eyeliner slot is not a reorder, it is putting the
        // wrong art on the wrong part of a face with no way to say so.
        void DrawMakeupList(RE::Actor* a_actor) {
            // ⚠⚠ THE SECTION IS ABSENT FOR A FOLLOWER RATHER THAN GREYED, and
            // that is a change of mind worth recording. Greying it out would
            // promise a control that can never light up on that target: an NPC's
            // tint layers live on the ACTOR BASE, so this feature cannot be
            // built for them without editing a record other actors share. An
            // accordion that is never openable is a worse answer than a line
            // saying which target it needs, so the header still draws and says
            // so.
            const bool player = MakeupApi::IsPlayer(a_actor);
            if (!player && !g.makeupLoaded) {
                const auto header = std::string{ FUCK::Translate("$FR_Mk_Section") } +
                                    "###ovl_loc_makeup";
                if (OS::ui::FramedHeader(header.c_str())) {
                    OS::ui::TextDisabledWrapped("$FR_Mk_PlayerOnly"_T);
                }
                return;
            }
            if (!g.makeupLoaded) {
                return;
            }

            // ⚠⚠ A CHARACTER WHO HAS NEVER BEEN THROUGH THE CHARACTER EDITOR
            // HAS NO TINT LIST AT ALL, and every section is absent because every
            // section is empty (user 2026-08-18: "i also made a new character and
            // noticed that there wasn't a makeup or dirt or other overlay
            // accordion. when i switched gender with racemenu and back then there
            // was"). MEASURED off that log: the first RaceMenu visit found the
            // player's base with no brows and no hair colour, a new game the
            // editor had not seen yet; the game builds the player's tint layers
            // from the race the first time the RaceSex Menu opens, and the sex
            // flip was incidental. This page does not build them itself: it
            // would be doing chargen's job with the alpha defaults guessed, and
            // RaceMenu does not rebuild a list that is already there, so a wrong
            // guess would be the face until someone flipped sex and back. Said
            // plainly, with the trip offered.
            if (g.makeup.empty()) {
                const auto header = std::string{ FUCK::Translate("$FR_Mk_Section") } +
                                    "###ovl_loc_makeup";
                if (OS::ui::FramedHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    OS::ui::TextDisabledWrapped("$FR_Mk_NotBuilt"_T);
                    if (ChamferPanel::Button("$FR_Mk_OpenRaceMenu"_T).clicked) {
                        OpenRaceMenuFromEditor();
                    }
                }
                return;
            }

            // ⚠⚠ THE LIST CAN BE REBUILT UNDER AN OPEN PAGE AND WAS. A
            // RaceMenu preset load replaces the player's tint list; the field
            // round spent after one could move colours and strengths and see
            // nothing, because every write went to indices into the list that
            // had been thrown away. The fingerprint is the mask POINTERS, so
            // our own writes do not trip it and a rebuild cannot miss it.
            // Selection and the pending drag die with the old list: an index
            // into a list that no longer exists is not a selection worth
            // keeping.
            if (MakeupApi::Fingerprint(a_actor) != g.makeupFingerprint) {
                spdlog::info(
                    "Makeup: the tint list changed under the page (a preset load or another "
                    "mod), so it was re-read and the selection dropped.");
                g.makeupSelected = -1;
                g_makeupPending  = -1;
                g.makeupDragging = false;
                g.makeupStrengthDragging = false;
                g.makeupPosDragging      = false;
                ReadMakeup(a_actor);
            }

            // Where the makeup half begins, for the tutorial's last card. The
            // publish is below, once the sections have drawn and the span is
            // known.
            const ImVec2 makeupTop = FUCK::GetCursorScreenPos();

            const auto used = MakeupPlan::UsedIn(g.makeup);
            // ⚠⚠ THE COUNT IS AGAINST THE CEILING, NOT AGAINST THE LIST. It
            // used to read "14 / 108", and 108 is the number of SLOTS the race
            // happens to carry, which is not a budget and told the player
            // nothing. Fifteen is the number that matters, because the sixteenth
            // crashes the game. See MakeupPlan::kMaxWornLayers.
            //
            // ⚠⚠ AND THE FACE'S SHARE IS NAMED, because that is where the
            // budget actually goes. A Nord carries 30 slots and may wear 15, and
            // the structural ones are worn by default: most of the allowance is
            // spent on the character's own face before any war paint goes on. A
            // player who reads "13 / 15" with no idea that eleven of them are
            // their lips and cheeks will hit the ceiling without understanding
            // why.
            const auto faceUsed =
                MakeupPlan::UsedIn(g.makeup, g.makeupLayers, MakeupPlan::Category::kFace);
            std::string budget = std::string{ FUCK::Translate("$FR_Mk_Budget") } + "  " +
                                 std::to_string(used) + " / " +
                                 std::to_string(MakeupPlan::kMaxWornLayers);
            if (faceUsed != 0) {
                budget += "  (" + std::to_string(faceUsed) + " " +
                          FUCK::Translate("$FR_Mk_BudgetFace") + ")";
            }
            OS::ui::TextDisabledWrapped(budget.c_str());

            // ⚠⚠ SAID WHERE THE LAYERS ARE, not in a footer. A player who has
            // run out of budget is looking at this list trying to add another
            // one, and the answer belongs under their cursor. The over-ceiling
            // case is separate and louder, because in that state NOTHING can be
            // written at all until they turn some off, and a page that silently
            // refused every edit would read as broken.
            if (used > MakeupPlan::kMaxWornLayers) {
                OS::ui::TextDisabledWrapped("$FR_Mk_OverCeiling"_T);
            } else if (MakeupPlan::RemainingLayers(g.makeup) == 0) {
                OS::ui::TextDisabledWrapped("$FR_Mk_AtCeiling"_T);
            }

            for (const auto& info : MakeupPlan::kCategories) {
                DrawMakeupSection(a_actor, info.category);
            }
            // The whole makeup half, budget line to the last section. Inside a
            // scrolled child this is wherever the sections really are this
            // frame, which is exactly what a ring may point at.
            const ImVec2 makeupEnd = FUCK::GetCursorScreenPos();
            Tutorial::PublishAnchor(
                Tutorial::Anchor::kMakeupSections, makeupTop,
                ImVec2(makeupTop.x + FUCK::GetContentRegionAvail().x, makeupEnd.y));
        }

        // ---- the skin section: the layer under every layer -------------------
        //
        // One ROW on the left, named by what is worn, and the CARDS on the
        // right (user 2026-08-18: "present it as cards that you pick instead
        // of how it is right now"). The row is what a layer row is: it names
        // the thing and selects it for editing; the right pane is where the
        // choosing happens, exactly as a layer's texture is chosen there. A
        // click on a card both selects and applies, because applying IS the
        // edit here, the same as picking a texture for an overlay is.
        //
        // ⚠ THE PACK IS THE CARD, NOT A FILE, so there is no file picker: a
        // pack is a folder of files matched by name (SkinPlan.h), and which of
        // its files land where is decided against the live shapes rather than
        // chosen here. The card's picture is the same rule applied to the
        // character's own parts (SkinCardScene.h).
        void ChooseSkin(RE::Actor* a_actor, const std::string& a_packId) {
            SelectSkin();
            if (a_packId == g.skinCurrent) {
                return;
            }
            Remember();
            g.skinCurrent = a_packId;
            SkinApi::Apply(a_actor, a_packId);
        }

        // Where a skin's textures come from, which is what its tooltip says
        // (user 2026-08-18: "make the tooltip for skin just say the source of
        // the skin texture"), the way an overlay card's tooltip is the file's
        // path and a layer row's is its texture. A pack's source is its folder
        // under the skins root; the base skin's is the body diffuse the game
        // gives this character, as SkinApi::Fit read it off the slot (the
        // recorded original while a pack is worn), and "Base Skin" only when
        // nothing has been measured yet.
        [[nodiscard]] std::string SkinSourceOf(RE::Actor* a_actor, const std::string& a_packId) {
            if (!a_packId.empty()) {
                return "textures\\" + std::string{ SkinPlan::kSkinsDir } + a_packId;
            }
            const auto fit = SkinApi::Fit(a_actor);
            return fit.defaultPath.empty() ? std::string{ FUCK::Translate("$FR_Skin_Default") }
                                           : fit.defaultPath;
        }

        void DrawSkinSection(RE::Actor* a_actor) {
            // ⚠ AN ACCORDION OF ITS OWN, like the location headers above the
            // layers (user 2026-08-18: "put skin into its own accordion, we
            // might expand on later"). One row under it today, the skin the
            // character wears; the header is what more rows go under.
            const std::string header =
                std::string{ FUCK::Translate("$FR_Skin_Section") } + "###ovl_skin_h";
            const bool open = OS::ui::FramedHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            // The hint is a tooltip on the header, as the makeup section's is
            // and for the reason recorded there (user 2026-08-17).
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_Skin_SectionTip"_T);
            }
            if (!open) {
                return;
            }
            // The row names what is worn, and the base skin by the mod it comes
            // from when that could be told (SkinApi::Fit), so the row and the
            // card say the same thing. "###" for the same reason every other
            // row here carries one: the visible half changes with the pack and
            // the language.
            std::string shown = g.skinCurrent;
            if (shown.empty()) {
                const auto fit = SkinApi::Fit(a_actor);
                shown = fit.defaultName.empty() ? std::string{ FUCK::Translate("$FR_Skin_Default") }
                                                : fit.defaultName;
            }
            // ⚠⚠ ALIGNED TO THE LAYER ROWS, NOT INDENTED BY THE THEME. This was
            // a bare FUCK::Indent(), which takes the theme's IndentSpacing, and
            // FLICK's default preset answers 106.7 at 1.33 scale: the skin name
            // started most of the way across the pane and ran off the right edge
            // (user 2026-08-27, "the item in the skin list here going off to the
            // right in vanilla theme"). Vel'dun's number is small, which is why
            // it looked right there and nowhere else.
            //
            // ⚠ THE REFERENT IS THE ROW BELOW IT. Every layer row in DrawLayerList
            // starts with the grip glyph and two spaces, so stepping in by exactly
            // that width puts this name where their names are and the skin reads as
            // the first row of the same list. RulesUI::DrawConditionFlow says the
            // same thing about indents that answer to nothing.
            const float rowIndent =
                FUCK::CalcTextSize((Icons::Utf8(Icons::kGrip) + "  ").c_str()).x;
            FUCK::Indent(rowIndent);
            if (FUCK::Selectable((shown + "###ovl_skin").c_str(), SkinSelected())) {
                SelectSkin();
            }
            // ⚠ A RIGHT CLICK PUTS THE BASE SKIN BACK (user 2026-08-18: "if we
            // right click on the skin slot on the left pane we reset to our
            // default skin"), the same gesture a layer row and a makeup row
            // answer with "take it off". Not through ChooseSkin: that selects
            // the row as well, and a right click that also moved the selection
            // would be doing two things for one press. Nothing to do when the
            // base skin is already worn.
            if (FUCK::IsItemClicked(1) && !g.skinCurrent.empty()) {
                Remember();
                g.skinCurrent.clear();
                SkinApi::Apply(a_actor, {});
            }
            if (FUCK::IsItemHovered()) {
                // The source, and how to be rid of a pack, the layer rows'
                // shape: nothing on a row says a right click does anything.
                std::string tip = SkinSourceOf(a_actor, g.skinCurrent);
                if (!g.skinCurrent.empty()) {
                    tip += "\n";
                    tip += FUCK::Translate("$FR_Skin_RightBase");
                }
                FUCK::SetTooltip(tip.c_str());
            }
            // ⚠ THE SAME WIDTH BACK OUT. Unindent takes one, and a pair that
            // disagrees leaves every row after this one stranded.
            FUCK::Unindent(rowIndent);
        }

        void DrawLayerList(RE::Actor* a_actor, bool a_narrow) {
            const auto& layers = Layers();

            // ⚠⚠ THE PAGE SAYS SO WHEN IT DISAGREES WITH THE BODY (user
            // 2026-08-24: "they can be on the body visually but not in the
            // overlays page, very odd"). Loading a RaceMenu preset replaces
            // skee's whole override store with the preset's own, and a bare
            // face preset carries no overlays, so the record goes while the
            // clones on the body keep drawing: they are geometry with a
            // material and nothing repaints them from the store. Every row then
            // read empty and the page had no way to say why.
            //
            // ⚠ IT REPORTS, IT DOES NOT REPAIR. Re-applying the look is what
            // puts the record back, and that is the player's call: the art on
            // the body right now may be the one they want, and OverlayReconcile
            // already stands its blank pass down for exactly that reason. A
            // button here that silently rewrote the store would be a second
            // opinion about which of two states is correct.
            if (StoreLostThePaint()) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_StoreLost"_T);
                FUCK::Spacing();
            }

            DrawSkinSection(a_actor);

            // ⚠ THE FLIGHT ENDS WITH THE BUTTON, NOT ONLY WITH A DROP. A drag
            // released over the pane's empty space, another location's rows or
            // outside the window never reaches an accept, and without this the
            // source row would stay dimmed until the next successful drag.
            if (g.dragFrom >= 0 && !FUCK::IsMouseDown(0)) {
                g.dragFrom = -1;
            }

            for (const auto& info : OverlayPlan::kLocations) {
                const std::size_t used = UsedIn(info.location);
                std::size_t       total = 0;
                for (const auto& layer : layers) {
                    if (layer.location == info.location) {
                        ++total;
                    }
                }
                if (total == 0) {
                    continue;
                }

                // ⚠ A STABLE ID BEHIND "###", BECAUSE THE LABEL IS TRANSLATED.
                // A collapsing header keys its open state on the hash of its
                // label, so without this every section would spring shut when
                // the player switched language, and the count in the label would
                // reset it every time a layer was filled or emptied. The id half
                // is the location's own stable identifier.
                const auto header = std::string{ FUCK::Translate(info.labelKey) } + "   " +
                                    std::to_string(used) + " / " + std::to_string(total) +
                                    "###ovl_loc_" + info.id;
                // ⚠ ALL FOUR OPEN ON ARRIVAL (user 2026-08-16). Only Face
                // opened at first, on the theory that four short bars beat one
                // long scroll; the field verdict was that a shut accordion
                // hides how many slots a location even has, which is the one
                // thing the header exists to say.
                const int flags = ImGuiTreeNodeFlags_DefaultOpen;
                if (!OS::ui::FramedHeader(header.c_str(), flags)) {
                    continue;
                }

                for (std::size_t i = 0; i < layers.size(); ++i) {
                    if (layers[i].location != info.location) {
                        continue;
                    }
                    const bool occupied =
                        i < g.layers.size() && OverlayPlan::Occupied(g.layers[i]);

                    // ⚠ THE LABEL CARRIES THE LAYER NUMBER EVEN WHEN IT HAS A
                    // NAME. Layering is the whole feature and the number is the
                    // order, so an occupied slot that showed only its texture
                    // name would hide the one property the player is arranging.
                    const auto grip = Icons::Utf8(Icons::kGrip);
                    std::string label = grip + "  " + std::to_string(layers[i].index + 1) +
                                        ". ";
                    label += occupied ? OverlayPlan::DisplayName(g.layers[i].texture)
                                      : std::string{ FUCK::Translate("$FR_Ovl_Empty") };

                    FUCK::PushID(static_cast<int>(i));
                    // ⚠ THE ROW BEING DRAGGED IS DIMMED WHILE IT IS IN FLIGHT,
                    // so the list shows where the layer came FROM as well as
                    // where it is going. Without it a drag reads as one
                    // highlighted row and no sense of movement at all.
                    const bool inFlight = g.dragFrom == static_cast<int>(i);
                    if (!occupied || inFlight) {
                        FUCK::PushStyleColor(ImGuiCol_Text,
                                             OS::ui::StyleColor(ImGuiCol_TextDisabled));
                    }
                    // ⚠ THE SELECTABLE IS NARROWED TO LEAVE THE CLEAR CONTROL
                    // ROOM, rather than the control being laid over it. The two
                    // are separate hit targets and the drag calls below bind to
                    // the LAST SUBMITTED ITEM, so the control has to come after
                    // both of them and the Selectable has to stop short.
                    //
                    // ⚠⚠ THE WIDTH IS RESERVED WHETHER OR NOT THE ROW HAS ONE
                    // (user 2026-08-16: "the clear button ruins the row height,
                    // the row height should be the same with or without the
                    // button"). An empty row that took the full width put its
                    // label on a different grid from its neighbours, so the
                    // column jumped as layers filled and emptied.
                    const float rowW   = FUCK::GetContentRegionAvail().x;
                    const float clearW = OS::ui::FontSize() + OS::ui::ItemSpacing().x;
                    if (FUCK::Selectable(label.c_str(), g.selected == static_cast<int>(i), 0,
                                         ImVec2(std::max(1.0f, rowW - clearW), 0.0f))) {
                        SelectOverlay(static_cast<int>(i));
                        g.narrowPicker = false;
                        // ⚠ THE SELECTION REPORTS TOO, so this page is the ONLY
                        // thing that arms the flash. The shot driver used to arm
                        // it as well, off its own selection test, and a reorder
                        // tripped both and flashed twice.
                        NoteChanged(i);
                        Tutorial::NotifyAction(Tutorial::Action::kSelectedOverlayLayer);
                    }
                    if (!occupied || inFlight) {
                        FUCK::PopStyleColor();
                    }

                    // ---- drag to reorder ---------------------------------
                    //
                    // ⚠ THE SELECTABLE IS THE SOURCE AND THE TARGET BOTH, which
                    // works here and did not work for the rules list. A rule row
                    // is several widgets and BeginDragDropTarget binds to the
                    // last submitted item, so that one needs an invisible button
                    // under the whole row. A layer row is one Selectable, so it
                    // is already the single hit-testable item both calls want.
                    // ⚠⚠ RIGHT CLICK CLEARS, AND IT IS SAFE ONLY BECAUSE THE
                    // HISTORY EXISTS. A destructive action on a click nobody
                    // aims carefully would be the wrong trade on its own; with
                    // undo one press away it is the fastest way to strip a
                    // layer, which is what the field asked for. It is checked
                    // straight after the Selectable because IsItemClicked binds
                    // to the last submitted item.
                    if (occupied && FUCK::IsItemClicked(1)) {
                        Remember();
                        g.layers[i] = OverlayPlan::LayerState{};
                        OverlayApi::Clear(a_actor, layers[i].node);
                        NoteChanged(i);
                    }

                    const ImVec2 rowMin = FUCK::GetItemRectMin();
                    const ImVec2 rowMax = FUCK::GetItemRectMax();

                    // ⚠ NO PREVIEW BOX ON THE CURSOR (user 2026-08-16: "i do
                    // not need to see the box, i just need to see the
                    // insertion"). It carried the layer's name, which was
                    // meant to say what was in flight and instead said what
                    // the dimmed source row and the insertion line already
                    // say, twice, in a box that covers the rows being aimed
                    // at. kSourceNoPreviewTooltip suppresses the window, so
                    // nothing is submitted between Begin and End.
                    if (occupied &&
                        FUCK::BeginDragDropSource(
                            FUCK::DragDropFlags::kSourceNoPreviewTooltip)) {
                        const int payload = static_cast<int>(i);
                        FUCK::SetDragDropPayload(kLayerDragType, &payload, sizeof(payload));
                        g.dragFrom = static_cast<int>(i);
                        FUCK::EndDragDropSource();
                    }
                    if (FUCK::BeginDragDropTarget()) {
                        // ⚠ THE INSERTION POINT IS WHERE IN THE ROW THE CURSOR
                        // IS, NOT WHICH ROW IT IS OVER. A highlighted row says
                        // "something will happen here" and leaves the player to
                        // guess whether the layer lands above or below it, which
                        // is the whole question when the thing being arranged is
                        // an order. Above the midpoint inserts before this row,
                        // below it inserts after.
                        const float mid    = (rowMin.y + rowMax.y) * 0.5f;
                        const bool  before = FUCK::GetMousePos().y < mid;
                        const bool  sameLocation =
                            g.dragFrom >= 0 &&
                            g.dragFrom < static_cast<int>(layers.size()) &&
                            layers[static_cast<std::size_t>(g.dragFrom)].location ==
                                info.location;
                        if (sameLocation) {
                            const float y = before ? rowMin.y : rowMax.y;
                            const float t = std::max(2.0f, FUCK::GetResolutionScale() * 2.0f);
                            // ⚠ THE ACCENT AT FULL STRENGTH, NOT ButtonActive.
                            // The theme's gold is #DABD80 and ButtonActive is
                            // that colour at 0.45 alpha, so the line came out
                            // washed against the panel and read as a hover
                            // rather than as the thing about to happen (user
                            // 2026-08-16). SeparatorActive is the same gold at
                            // full alpha, and it is the slot that already means
                            // "a line, and it is the live one".
                            // ⚠ THE FULL ROW, NOT THE SELECTABLE'S RECT. The
                            // Selectable stops short of the clear button, and a
                            // line that stopped with it would leave a notch at
                            // the right of every insertion point.
                            FUCK::DrawRectFilled(
                                ImVec2(rowMin.x, y - t * 0.5f),
                                ImVec2(rowMin.x + rowW, y + t * 0.5f),
                                OS::ui::StyleColor(ImGuiCol_SeparatorActive), t * 0.5f);
                        }
                        if (const auto* accepted =
                                FUCK::AcceptDragDropPayload(kLayerDragType)) {
                            int from = -1;
                            std::memcpy(&from, accepted->Data, sizeof(from));
                            // ⚠ WITHIN ONE LOCATION ONLY. A face texture on a
                            // foot slot is not a reorder, it is a different
                            // picture in the wrong place, and the node names
                            // make the two impossible to confuse anywhere else.
                            if (from >= 0 && from < static_cast<int>(layers.size()) &&
                                layers[static_cast<std::size_t>(from)].location ==
                                    info.location) {
                                // The row's own index is "insert before"; one
                                // past it is "insert after", clamped so the last
                                // row's lower half is still a legal target.
                                std::size_t to = i;
                                if (!before && to + 1 < layers.size() &&
                                    layers[to + 1].location == info.location) {
                                    ++to;
                                }
                                ReorderWithin(a_actor, static_cast<std::size_t>(from), to);
                            }
                            g.dragFrom = -1;
                        }
                        FUCK::EndDragDropTarget();
                    }

                    if (FUCK::IsItemHovered() && occupied) {
                        // The path, and how to be rid of it. The right click
                        // survives the button below and is still worth naming:
                        // it is the fast way once you know it, and nothing on a
                        // row says a right click does anything.
                        const std::string tip = g.layers[i].texture + "\n" +
                                                FUCK::Translate("$FR_Ovl_RightClear");
                        FUCK::SetTooltip(tip.c_str());
                    }

                    // ⚠⚠ A VISIBLE WAY TO EMPTY A LAYER, AND IT REPLACES A LIST
                    // ENTRY RATHER THAN JOINING ONE (user 2026-08-16). Emptying
                    // used to be a "Nothing" row at the top of the texture
                    // picker, which meant the answer to "how do I take this
                    // off" lived on the other side of the page, inside the
                    // thing you use to put something on. The button is on the
                    // layer, where the layer is.
                    //
                    // ⚠ AFTER THE DRAG CALLS, NEVER BEFORE. Both of them bind
                    // to the last submitted item, so a button drawn first would
                    // take the drag and the row would stop being draggable.
                    // ⚠⚠ A SELECTABLE AND NOT ChamferPanel::IconButton, AND THE
                    // HEIGHT IS THE WHOLE REASON. A chamfered icon button is an
                    // exact square of the FRAME widget height, which is a text
                    // line plus FramePadding twice, so putting one beside a
                    // Selectable made every occupied row taller than every empty
                    // one and the list stepped in and out as layers filled. A
                    // Selectable with a zero height takes the same line height
                    // as the one it sits beside, by construction rather than by
                    // arithmetic that has to be kept in step.
                    if (occupied) {
                        FUCK::SameLine();
                        if (FUCK::Selectable(Icons::Utf8(Icons::kTimes).c_str(), false, 0,
                                             ImVec2(OS::ui::FontSize(), 0.0f))) {
                            Remember();
                            g.layers[i] = OverlayPlan::LayerState{};
                            OverlayApi::Clear(a_actor, layers[i].node);
                            NoteChanged(i);
                        }
                        if (FUCK::IsItemHovered()) {
                            FUCK::SetTooltip("$FR_Ovl_ClearLayer"_T);
                        }
                    }
                    FUCK::PopID();
                }
            }

            DrawMakeupList(a_actor);

            // ---- the fix-it button (OS-219 route B) ------------------------
            //
            // ⚠⚠ THE TABLE IS THE DEFAULT AND THE SCRAPE IS THE BUTTON, which
            // is the user's own framing of the TITS route (2026-08-18: "table as
            // the default, scrape as the 'this is wrong, fix it' button"). The
            // shipped table is a snapshot of the reference rig's registration
            // scripts, so a pack this rig has and that one did not is shown on
            // every page; this reads the lists RaceMenu itself builds HERE.
            //
            // ⚠ ONE CONTROL, PAGE LEVEL, DRAWN ONCE. The field verdict of
            // 2026-08-16 was that this page had filled up with controls and
            // explanations for a filter that is right nearly always, which is
            // why Show all art moved into settings; a button repeated inside
            // every layer's picker would be that mistake again. It sits at the
            // foot of the list pane with the sections, not in the picker.
            //
            // ⚠ AND IT IS NOT THE ONLY WAY IN. The capture runs by itself on
            // every RaceMenu open, so a player who never presses this still gets
            // the reading the first time they visit the character editor. The
            // button exists for the player who has just installed a pack and
            // wants the answer now.
            // ⚠ THE RESULT'S OWN hovered, NOT IsItemHovered(). A chamfered
            // button submits its LABEL as the last ImGui item, so an
            // IsItemHovered() after this call asks about the text and the
            // tooltip would only appear over the glyphs. ChamferPanel.h says so
            // above ButtonResult; this is the one caller that had to be told
            // twice.
            const auto readLists = ChamferPanel::Button("$FR_Ovl_ReadRaceMenu"_T);
            if (readLists.clicked) {
                OpenRaceMenuFromEditor();
            }
            if (readLists.hovered) {
                FUCK::SetTooltip("$FR_Ovl_ReadRaceMenuTip"_T);
            }

            if (a_narrow && (Selected() || MakeupSelected() || SkinSelected()) &&
                ChamferPanel::Button("$FR_Ovl_ToLayer"_T).clicked) {
                g.narrowPicker = true;
            }
        }

        // ---- the transaction bar ---------------------------------------------
        //
        // ⚠ THE SAME STRIP EVERY OTHER PAGE HAS, AND THAT IS THE WHOLE POINT
        // (user 2026-08-16, with a screenshot of the Shape page's bar). Undo and
        // redo sat at the top of the left pane and Clear every layer at its
        // foot, which made one page carry a third arrangement of the same kind
        // of control. ShapeUI::DrawTransactionBar is what this copies, down to
        // the footer height Draw reserves before it sizes the panes.
        //
        // ⚠⚠ EVERY CONDITION IS A LOCAL AND IT IS PASSED TWICE, ONCE TO
        // BeginDisabled AND ONCE TO THE BUTTON. ImGui's disabling reaches the
        // InvisibleButton under a chamfered button, so the click is refused, but
        // it does NOT reach the fill and the outline, which are ours and which
        // nothing multiplies. A converted button inside BeginDisabled alone goes
        // dead while still looking live. See the same note in ShapeUI.
        void DrawTransactionBar(RE::Actor* a_actor) {
            FUCK::Separator();

            // ⚠ FIRST ON THE BAR, AND IT IS EditorUI'S BUTTON, the same call in
            // the same place as Body Studio's and Shape's bars (user 2026-08-19:
            // "in the bottom bar of all pages"). A skin, an overlay and a tattoo
            // are all under the clothes, which is why this page had the control
            // at all; the move is about where it is, not whether.
            OS::EditorUI::DrawHideOutfitButton();
            FUCK::SameLine();

            const bool noUndo  = g.undo.empty();
            const auto undoTxt = Icons::Utf8(Icons::kUndo);
            FUCK::BeginDisabled(noUndo);
            const auto undoBtn = ChamferPanel::IconButton(undoTxt.c_str(), noUndo);
            if (undoBtn.clicked) {
                Undo(a_actor);
            }
            FUCK::EndDisabled();
            // ⚠ THE RESULT'S HOVER, AND IT STILL ANSWERS WHILE DISABLED. These
            // tooltips say why a control is greyed, so a hover that went quiet
            // under BeginDisabled would silence them in the one state they were
            // written for.
            if (undoBtn.hovered) {
                FUCK::SetTooltip("$FR_Ovl_Undo"_T);
            }

            FUCK::SameLine();
            const bool noRedo  = g.redo.empty();
            const auto redoTxt = Icons::Utf8(Icons::kRedo);
            FUCK::BeginDisabled(noRedo);
            const auto redoBtn = ChamferPanel::IconButton(redoTxt.c_str(), noRedo);
            if (redoBtn.clicked) {
                Redo(a_actor);
            }
            FUCK::EndDisabled();
            if (redoBtn.hovered) {
                FUCK::SetTooltip("$FR_Ovl_Redo"_T);
            }

            FUCK::SameLine();
            // ⚠ GREYED WHEN THERE IS NOTHING TO CLEAR, which the old placement
            // was not. A live button that empties nothing still takes a history
            // step, so an undo afterwards appears to do nothing at all.
            const auto& layers = Layers();
            bool        anyUsed = false;
            for (std::size_t i = 0; i < layers.size() && i < g.layers.size(); ++i) {
                if (OverlayPlan::Occupied(g.layers[i])) {
                    anyUsed = true;
                    break;
                }
            }
            FUCK::BeginDisabled(!anyUsed);
            const auto clearBtn =
                ChamferPanel::Button("$FR_Ovl_ClearAll"_T, 0.0f, !anyUsed);
            if (clearBtn.clicked) {
                Remember();
                for (std::size_t i = 0; i < layers.size() && i < g.layers.size(); ++i) {
                    if (!OverlayPlan::Occupied(g.layers[i])) {
                        continue;
                    }
                    g.layers[i] = OverlayPlan::LayerState{};
                    OverlayApi::Clear(a_actor, layers[i].node);
                }
            }
            FUCK::EndDisabled();
            if (clearBtn.hovered) {
                FUCK::SetTooltip("$FR_Ovl_ClearAllTip"_T);
            }

            // ---- and the makeup half's own control ---------------------------
            //
            // ⚠⚠ REVERT IS A SEPARATE BUTTON AND IT IS NOT UNDO. The three
            // controls above move [Ovl] node overrides through a history the
            // page keeps, because skee cannot be asked what an override held
            // before it was written. This one restores a MEASUREMENT: the tint
            // masks as they were read when the page was opened. Folding it into
            // Undo would give one control two meanings and let a press aimed at
            // an overlay step land on a face, which is the cost of the two
            // systems sharing a page.
            //
            // ⚠ IT IS ABSENT RATHER THAN GREYED WHEN THE TARGET IS NOT THE
            // PLAYER, matching the section itself. A permanently dead button on
            // a bar of live ones reads as broken rather than as unavailable.
            if (g.makeupLoaded) {
                FUCK::SameLine();
                const bool dirty = MakeupDirty();
                FUCK::BeginDisabled(!dirty);
                const auto revertBtn =
                    ChamferPanel::Button("$FR_Mk_Revert"_T, 0.0f, !dirty);
                if (revertBtn.clicked) {
                    // ⚠ REVERT IS A STEP LIKE ANY OTHER. It throws away every
                    // makeup edit of the session in one press, which is exactly
                    // the press worth being able to take back.
                    Remember();
                    // ⚠ ONE WRITE AND ONE RETINT FOR THE WHOLE REVERT, which is
                    // what Restore's diff is for. Sending every slot would cost
                    // a texture rebuild for layers that never moved.
                    MakeupApi::Restore(a_actor, g.makeup, g.makeupArrival);
                    g.makeup        = g.makeupArrival;
                    g_makeupPending = -1;
                }
                FUCK::EndDisabled();
                if (revertBtn.hovered) {
                    FUCK::SetTooltip("$FR_Mk_RevertTip"_T);
                }
            }
        }

        // ---- the right pane: one layer ---------------------------------------

        // The search box every picker on this page opens with, and the
        // Favorites filter beside it. One shape for the three grids so the
        // control is met the same way wherever it is met; the checkbox is the
        // editor's own (`$FR_Favorites`, session-local).
        void DrawSearchRow(const char* a_id, std::string& a_text, const char* a_tipKey) {
            const auto searchIcon = Icons::Utf8(Icons::kSearch);
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted(searchIcon.c_str());
            FUCK::SameLine();
            // The box takes what the checkbox leaves: its label, its square,
            // and the spacing either side.
            const char* favLabel = FUCK::Translate("$FR_Favorites");
            const float favW = FUCK::CalcTextSize(favLabel).x + FUCK::GetFrameHeight() +
                               OS::ui::ItemSpacing().x * 3.0f;
            const float boxW = FUCK::GetContentRegionAvail().x - favW;
            FUCK::SetNextItemWidth(boxW > OS::ui::FontSize() * 4.0f ? boxW : -1.0f);
            FUCK::InputText(a_id, &a_text);
            if (FUCK::IsItemHovered() && a_text.empty()) {
                FUCK::SetTooltip(FUCK::Translate(a_tipKey));
            }
            FUCK::SameLine();
            if (FUCK::Checkbox(favLabel, &g.favoritesOnly, false, false)) {
                EditorStyle::PlayUISound("UIMenuFocus");
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_FavoritesTip"_T);
            }
        }

        void DrawTexturePicker(RE::Actor* a_actor, std::size_t a_index) {
            const auto snap = OverlayTextures::Get();

            DrawSearchRow("##ovl_search", g.search, "$FR_Ovl_Search");

            if (OverlayTextures::Scanning() && (!snap || snap->entries.empty())) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_Scanning"_T);
                return;
            }
            if (!snap || snap->entries.empty()) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_NoTextures"_T);
                return;
            }

            FUCK::BeginChild("ovl_texture_list", ImVec2(0, 0), false);

            // ⚠ THERE IS NO "NOTHING" ROW HERE ANY MORE (user 2026-08-16).
            // Emptying a layer sat at the top of the picker on the theory that
            // "none" is one of the answers to which texture. It is not: it is
            // an answer to which LAYER, and putting it here meant the way to
            // take something off lived on the far side of the page, inside the
            // thing you use to put something on. It is the cross on the layer's
            // own row now, beside the right click that has always done it.

            std::string       search = g.search;
            std::transform(search.begin(), search.end(), search.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // ---- the cards -------------------------------------------------
            //
            // ⚠ THE SHARED CARD AND THE SHARED GRID, NOT A TILE OF OUR OWN.
            // The first version of this pane hand-rolled its own tile and the
            // field verdict was that it did not look or feel like a card
            // anywhere else in the mod, which it did not: the corner cut, the
            // name band, the hover and selection treatment and the fitted
            // column maths all live in DrawPreviewCard and DrawCardGrid, and a
            // tile drawn beside them can only ever approximate them. The
            // picture comes from PreviewCardDesc::flat; everything else about
            // the card is now whatever the styles grid is.
            // ⚠ BIGGER THAN THE STYLES GRID'S TILE, and asked for (user
            // 2026-08-16). An overlay card is a flat texture rather than a
            // rendered model, so the thing being judged is fine detail spread
            // across a whole body or face: freckles and thin linework read as
            // noise at the size a helmet is recognisable at.
            const float side = OS::ui::FontSize() * 5.6f;

            // Filtered once and grouped, so each pack gets its own heading with
            // a properly laid out grid under it rather than one long run.
            struct Group {
                std::string                              folder;
                std::vector<const OverlayTextures::Entry*> items;
            };
            // ⚠⚠ A BODY OVERLAY IS NOT OFFERED FOR A FACE SLOT, and the rule
            // is the pack's own registration rather than anything derived from
            // the file (user 2026-08-16). OverlayLocations carries the whole
            // measurement, including why a folder rule and a name rule are both
            // wrong on this library. What it cannot place is SHOWN.
            const auto location = Layers()[a_index].location;
            // ⚠ THE SWITCH LIVES IN SETTINGS NOW, not beside the search box. It
            // was here so a player who could not find their texture would see
            // why without leaving the page, and the field verdict was that the
            // page had filled up with controls and explanations for a filter
            // that is right nearly always.
            const bool showAll = Settings::GetSingleton().overlayShowAllArt;

            // ⚠⚠ REGISTERED FOR THIS LOCATION FIRST, THEN A BAND FOR THE REST,
            // and this is the answer to the feet grid showing body art (field
            // 2026-08-16). MEASURED cause: 85 of the table's 1846 rows name feet
            // at all, so on that location almost everything on screen arrived
            // through the fallback. The fallback is not the bug and must not be
            // removed: 1323 of 2047 installed textures carry no location word,
            // and hiding them was rejected. What changes is the ORDER and that
            // the rest is labelled, so a player looking at feet art sees the
            // feet art first and can still scroll to everything else. User's
            // call 2026-08-16.
            std::vector<Group> placed;
            std::vector<Group> unsorted;
            std::size_t        keptOutAsMakeup = 0;
            for (const auto& entry : snap->entries) {
                // ⚠⚠ A TINT MASK IS NEVER OFFERED HERE. It is head art the
                // engine composites through its own shader; put one in an [Ovl]
                // node override and it paints a lip shaped mask on a body. The
                // scan carries both libraries now and this is the wall between
                // them, so it holds even under Show all art.
                if (entry.tintMask) {
                    continue;
                }
                if (!showAll && entry.maskTwin) {
                    continue;
                }
                // ⚠⚠ MAKEUP IS WHAT A PACK REGISTERED AS A WARPAINT AND NOWHERE
                // ELSE (OS-219, user 2026-08-18: "we still have other items
                // Lovely Makeup that are in the face slot list"). This is
                // RaceMenu's own rule: its face-overlay list is only what the
                // packs put there with AddFacePaint, and AddWarpaint feeds the
                // war paint tab instead. MEASURED on the reference table
                // (regenerated 2026-08-18: 2263 rows, 36 scripts): every one of
                // the 1389 warp rows is warp-only, Lovely's 26 among them, and
                // until this line they reached the picker through the name rule
                // ("makeup" in the path says Face) or the show-everything
                // fallback. The white-mask verdict below cannot catch them,
                // because Lovely's blush is pink.
                //
                // ⚠ THE COST IS A PACK THAT REGISTERED IN THE WRONG LIST, and
                // that is what overlay-locations-fixups.json is for: Shep's 264
                // body tattoos are AddWarpaint too, and the fixup gives them
                // body back at table load, so `registered` HAS a location here
                // and they pass. Show all art still offers everything.
                if (!showAll && entry.warpaint &&
                    !OverlayLocations::HasLocation(entry.registered)) {
                    ++keptOutAsMakeup;
                    continue;
                }
                // ⚠⚠ AND THE ASSET ITSELF GETS A VETO, WHICH IS THE ONLY RULE
                // THAT CAUGHT EVERY CASE. Two rounds of name and registration
                // rules still left black cards on the page, because a pack that
                // ships no script and names its mask copy anything at all
                // cannot be reasoned about. A decoded thumbnail with no
                // transparency in it is a tint mask whatever it is called.
                //
                // ⚠ kUnknown SHOWS. The verdict arrives with the decode, so
                // treating "not yet" as "hide" would open the picker on an
                // empty grid that fills in.
                const auto alphaKind = OverlayThumbs::AlphaKind(entry.path);
                if (!showAll && alphaKind == OverlayThumbs::Alpha::kMask) {
                    continue;
                }
                // ⚠⚠ AND A MASK WITH A SHAPE IS MAKEUP TOO (OS-218, user
                // 2026-08-18: LDD's eye circles "are pure white when we add
                // them to face paint, but they work fine when they are in the
                // makeup layer", and RaceMenu's own face-overlay list never
                // offers them). White RGB under the alpha is how a tint mask is
                // authored; an [Ovl] node paints it white and cannot darken it
                // (measured 2026-08-16). Kept out of this picker unless some
                // pack registered the file as an OVERLAY somewhere, in which
                // case the pack's word wins and a white tattoo stays a tattoo.
                // The makeup pickers still get it, which is where it works.
                if (!showAll && alphaKind == OverlayThumbs::Alpha::kWhiteMask &&
                    !OverlayLocations::HasLocation(entry.registered)) {
                    continue;
                }
                if (!showAll && !OverlayLocations::Allows(entry.locations, location)) {
                    continue;
                }
                // The Favorites filter: starred art only, by the same key the
                // star writes.
                if (g.favoritesOnly &&
                    !Favorites::IsFavoriteLine(Favorites::OverlayKey(entry.path))) {
                    continue;
                }
                if (!search.empty()) {
                    std::string haystack = entry.folder + " " + entry.name;
                    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                                   [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    if (haystack.find(search) == std::string::npos) {
                        continue;
                    }
                }
                // ⚠ THE TABLE'S OWN VERDICT, NOT THE RESOLVED ONE. `locations`
                // has already had the name rule and the show-everything
                // fallback applied to it, so by then art a pack really placed
                // here is indistinguishable from art nobody placed anywhere,
                // which is exactly the difference this band is drawing.
                auto& into =
                    OverlayLocations::Allows(entry.registered, location) ? placed : unsorted;
                if (into.empty() || into.back().folder != entry.folder) {
                    into.push_back(Group{ entry.folder, {} });
                }
                into.back().items.push_back(&entry);
            }

            // ⚠ NO COUNT OF WHAT WAS HIDDEN (user 2026-08-16: "we also don't
            // need that hint"). It said how many textures the filter had taken
            // out and it was two more lines of explanation above a grid, every
            // time, for a filter that is right nearly always. What it was
            // guarding against, a player unable to find installed art, is now
            // answered by the switch in settings and by the log line the table
            // writes on load.
            //
            // The makeup rule's count goes to the LOG instead, once per change,
            // so the field can read how many the rule kept out without a line
            // on the page. It moves when the scan republishes or Show all art
            // flips, and both are worth a line.
            {
                static std::uint64_t s_saidGeneration = 0;
                static std::size_t   s_saidKeptOut    = static_cast<std::size_t>(-1);
                if (s_saidGeneration != snap->generation || s_saidKeptOut != keptOutAsMakeup) {
                    s_saidGeneration = snap->generation;
                    s_saidKeptOut    = keptOutAsMakeup;
                    spdlog::info("OverlaysUI: {} texture(s) kept out of the overlays picker as "
                                 "makeup (registered as war paint and nowhere else, OS-219); "
                                 "Show all art is {}",
                                 keptOutAsMakeup, showAll ? "on" : "off");
                }
            }
            if (placed.empty() && unsorted.empty()) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_NoMatch"_T);
                FUCK::EndChild();
                return;
            }

            // ⚠ THE BAND IS PART OF THE PACK HEADING'S ID. Two bands can hold
            // the same pack, and an id that was only the folder would give the
            // two headings one open state and one ImGui item id.
            const auto drawGroups = [&](const std::vector<Group>& a_groups,
                                        const char*               a_band) {
            for (const auto& group : a_groups) {
                // ⚠⚠ EACH PACK IS AN ACCORDION (user 2026-08-16). The headings
                // were plain text, so a library of thirty packs was one scroll
                // with no way to put any of it away, and the pack you wanted
                // was as far down as the ones you did not. The same
                // "###" trick the location headers use: the visible half
                // carries the count and the id half is the folder, so the open
                // state survives the count changing and does not key on a
                // translated word.
                const std::string label =
                    (group.folder.empty() ? std::string{ FUCK::Translate("$FR_Ovl_Loose") }
                                          : group.folder) +
                    "   " + std::to_string(group.items.size()) + "###ovl_pack_" + a_band +
                    group.folder;
                // ⚠ OPEN ON ARRIVAL, the same call the location list made and
                // for the same reason: a shut accordion hides how much a pack
                // even has, and a picker that opens on a column of closed bars
                // has hidden the thing it exists to show.
                if (!OS::ui::FramedHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    continue;
                }
                PreviewCardUI::DrawCardGrid(
                    group.items.size(), side,
                    [&](std::size_t a_i, float a_fitted) {
                        const auto& entry = *group.items[a_i];
                        PreviewCardUI::PreviewCardDesc desc;
                        // The path is the identity here, the way a FormID is
                        // for a style, and two packs can ship the same name.
                        desc.pushId = static_cast<std::uint32_t>(
                            std::hash<std::string>{}(entry.path));
                        desc.side       = a_fitted;
                        // ⚠⚠ A STENCIL SAYS SO, BECAUSE ONLY Show all art LETS ONE
                        // THROUGH AND IT LOOKS EXACTLY LIKE ART UNTIL IT IS ON.
                        // These are the second, black backgrounded copy a pack
                        // ships beside a design. The filter above hides them
                        // normally, so before this the toggle handed the player
                        // 184 cards on the reference rig that paint a black face
                        // and no way at all to tell which ones (user 2026-08-26,
                        // after "a lot of overlays have pure black face").
                        //
                        // ⚠ DIMMED AND LABELLED RATHER THAN BADGED. A corner
                        // badge has to be learned; a greyed name reading (mask)
                        // needs no legend, and the card's three corners are
                        // already spoken for by the star, the new mark and the
                        // pin.
                        std::string maskLabel;
                        if (entry.maskTwin) {
                            maskLabel = OS::ui::FormatF("$FR_Ovl_MaskName"_T,
                                                        entry.name.c_str());
                        }
                        desc.name       = entry.maskTwin ? &maskLabel : &entry.name;
                        desc.nameCol    = OS::ui::Col(
                            entry.maskTwin ? OS::ui::StyleColor(ImGuiCol_TextDisabled)
                                           : OS::ui::StyleColor(ImGuiCol_Text));
                        desc.scene      = nullptr;
                        desc.flat       = true;
                        desc.flatImage  = OverlayThumbs::Texture(entry.path);
                        desc.flatFailed = OverlayThumbs::Failed(entry.path);
                        desc.selected   = g.layers[a_index].hasTexture &&
                                        g.layers[a_index].texture == entry.path;
                        // The star (user 2026-08-18): keyed by the texture's
                        // path, so the same file is one star in both pickers.
                        const auto favKey = Favorites::OverlayKey(entry.path);
                        desc.showStar     = true;
                        desc.favourite    = Favorites::IsFavoriteLine(favKey);

                        const auto r = PreviewCardUI::DrawPreviewCard(desc);
                        // The star eats its own click, the rows' rule: the
                        // card is one button, so the toggle and the pick would
                        // otherwise both fire off a single press.
                        if (r.starHovered && r.clicked) {
                            Favorites::ToggleLine(favKey);
                            EditorStyle::PlayUISound("UIMenuFocus");
                            return;
                        }
                        if (r.clicked) {
                            Remember();
                            auto& layer      = g.layers[a_index];
                            layer.hasTexture = true;
                            layer.texture    = entry.path;
                            // New art starts where its author drew it (OS-209).
                            // A position belongs to the art it was set on, and
                            // carrying one across would show the new tattoo
                            // shifted by the old one's slider.
                            //
                            // ⚠⚠ UNLESS THE PLAYER ASKED FOR THE OPPOSITE. Keep
                            // offset is the whole of this branch: on, the
                            // layer's transform is left exactly where the
                            // sliders had it and the incoming art is baked at
                            // those numbers by the Push below, which is the same
                            // write a slider release makes. There is no carried
                            // value living anywhere else, so the two paths
                            // cannot drift.
                            if (!Settings::GetSingleton().overlayKeepOffset) {
                                layer.transform = OverlayTransform::Transform{};
                            }
                            // A layer that has never been painted has no alpha
                            // or tint stored, and a texture arriving with alpha
                            // zero would look like nothing happened. Give it the
                            // visible defaults on the way in, once.
                            if (!layer.hasAlpha) {
                                layer.hasAlpha = true;
                                layer.alpha    = 1.0f;
                            }
                            if (!layer.hasTint) {
                                layer.hasTint = true;
                                layer.tint    = OverlayPlan::Rgb{ 255, 255, 255 };
                            }
                            Push(a_actor, a_index);
                            NoteChanged(a_index);
                            Tutorial::NotifyAction(Tutorial::Action::kPickedOverlayTexture);
                        }
                        if (r.hovered) {
                            // The path first, which is all this tooltip has ever
                            // been, and then what a stencil is when that is what
                            // the card is.
                            std::string tip = entry.path;
                            if (entry.maskTwin) {
                                tip += "\n";
                                tip += "$FR_Ovl_MaskTip"_T;
                            }
                            FUCK::SetTooltip(tip.c_str());
                        }
                    });
            }
            };

            drawGroups(placed, "p");
            // ⚠ THE BAND IS DRAWN ONLY WHEN THERE IS SOMETHING ON BOTH SIDES OF
            // IT. On a location where every pack registered its art, a heading
            // announcing the leftovers with nothing under it is noise; on one
            // where nobody registered anything, the whole grid is the leftovers
            // and calling that out says nothing either.
            if (!unsorted.empty() && !placed.empty()) {
                FUCK::Spacing();
                FUCK::Separator();
                std::size_t rest = 0;
                for (const auto& group : unsorted) {
                    rest += group.items.size();
                }
                const auto band = std::string{ FUCK::Translate("$FR_Ovl_Unsorted") } + "   " +
                                  std::to_string(rest);
                OS::ui::TextDisabledWrapped(band.c_str());
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip("$FR_Ovl_UnsortedTip"_T);
                }
            }
            drawGroups(unsorted, "u");
            FUCK::EndChild();
            // The tutorial's pick-a-card step rings the grid itself rather
            // than the pane around it, so the colour controls above it stay
            // out of the spotlight.
            Tutorial::PublishAnchor(Tutorial::Anchor::kOverlayTextures,
                                    FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
        }

        // ---- the makeup half of the right pane -------------------------------

        // ⚠⚠ THE PICKER'S SOURCE IS THE PACKS' OWN WARPAINT REGISTRATIONS, and
        // that is the whole reason this art finally has a home. Of the 1846 rows
        // the generated table carries, 1062 are AddWarpaint rather than any of
        // the four overlay lists, and they are the makeup packs by name:
        // Koralina, Female Makeup Suite, LDD, Lovely and Pretty, Lamenthia, Obi,
        // SkFO. The overlays picker offered them only because it shows what it
        // cannot place.
        //
        // ⚠ THE MASK TWIN RULE DOES NOT APPLY HERE AND MUST NOT BE COPIED IN. A
        // mask twin is hidden from the OVERLAYS picker because it is a tint mask
        // sitting beside real overlay art, and a tint mask is exactly what this
        // section wants. The black backed card is the correct card here.
        // ⚠⚠ AND THE LIBRARY IS THE CATEGORY'S, NOT ONE LIBRARY FOR EVERYTHING.
        // War paint takes the packs' registrations, dirt takes the game's own
        // dirt masks, and a face slot never gets here at all. Offering one list
        // to all three is what put makeup pack art in front of the Lips slot.
        [[nodiscard]] bool InMakeupLibrary(const OverlayTextures::Entry& a_entry,
                                           MakeupPlan::Category          a_category) {
            switch (a_category) {
            case MakeupPlan::Category::kWarPaint:
                return a_entry.warpaint;
            case MakeupPlan::Category::kDirt: {
                // ⚠ A NAME RULE, AND IT IS THE ONLY ONE AVAILABLE. No pack
                // registration says "this is dirt": the four Papyrus lists and
                // AddWarpaint are the whole vocabulary. What the race record
                // gives is the vanilla art by name, FemaleHeadDirt_01 through
                // _03 and their male twins, and they are in the tint mask
                // library now that the scan walks it. Anything else a player has
                // is one Show all art away.
                if (!a_entry.tintMask) {
                    return false;
                }
                std::string name = a_entry.name;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return name.find("dirt") != std::string::npos;
            }
            default:
                // A type nothing has heard of gets both libraries rather than
                // an empty grid, which is the same call the page makes
                // everywhere else about art it cannot place.
                return a_entry.warpaint || a_entry.tintMask;
            }
        }

        void DrawMakeupPicker(RE::Actor* a_actor, std::size_t a_index,
                              MakeupPlan::Category a_category) {
            const auto snap = OverlayTextures::Get();

            DrawSearchRow("##mk_search", g.makeupSearch, "$FR_Ovl_Search");

            if (OverlayTextures::Scanning() && (!snap || snap->entries.empty())) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_Scanning"_T);
                return;
            }
            if (!snap || snap->entries.empty()) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_NoTextures"_T);
                return;
            }

            // ⚠ SAID WHERE THE CARDS ARE. A player at the ceiling who has
            // selected an unworn layer is about to click art that cannot land,
            // and the layer list's copy of this is off to the left behind their
            // cursor.
            if (!MakeupPlan::MayWear(g.makeup, a_index)) {
                OS::ui::TextDisabledWrapped("$FR_Mk_AtCeilingPick"_T);
            }

            FUCK::BeginChild("mk_texture_list", ImVec2(0, 0), false);

            std::string search = g.makeupSearch;
            std::transform(search.begin(), search.end(), search.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // ⚠ THE SAME SETTING TURNS THIS FILTER OFF TOO. A player who cannot
            // find art they know they installed has one switch to reach for
            // rather than one per page, and the reason it can be needed is the
            // same on both: a pack that ships no registration script says
            // nothing about itself, and warpaint art from such a pack is
            // invisible to a rule that reads registrations.
            const bool showAll = Settings::GetSingleton().overlayShowAllArt;

            struct Group {
                std::string                               folder;
                std::vector<const OverlayTextures::Entry*> items;
            };
            std::vector<Group> groups;
            for (const auto& entry : snap->entries) {
                if (!showAll && !InMakeupLibrary(entry, a_category)) {
                    continue;
                }
                if (g.favoritesOnly &&
                    !Favorites::IsFavoriteLine(Favorites::OverlayKey(entry.path))) {
                    continue;
                }
                if (!search.empty()) {
                    std::string haystack = entry.folder + " " + entry.name;
                    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                                   [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    if (haystack.find(search) == std::string::npos) {
                        continue;
                    }
                }
                if (groups.empty() || groups.back().folder != entry.folder) {
                    groups.push_back(Group{ entry.folder, {} });
                }
                groups.back().items.push_back(&entry);
            }

            if (groups.empty()) {
                OS::ui::TextDisabledWrapped("$FR_Ovl_NoMatch"_T);
                FUCK::EndChild();
                return;
            }

            const float side = OS::ui::FontSize() * 5.6f;
            for (const auto& group : groups) {
                const std::string label =
                    (group.folder.empty() ? std::string{ FUCK::Translate("$FR_Ovl_Loose") }
                                          : group.folder) +
                    "   " + std::to_string(group.items.size()) + "###mk_pack_" + group.folder;
                if (!OS::ui::FramedHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    continue;
                }
                PreviewCardUI::DrawCardGrid(
                    group.items.size(), side, [&](std::size_t a_i, float a_fitted) {
                        const auto& entry = *group.items[a_i];
                        PreviewCardUI::PreviewCardDesc desc;
                        desc.pushId = static_cast<std::uint32_t>(
                            std::hash<std::string>{}("mk" + entry.path));
                        desc.side       = a_fitted;
                        desc.name       = &entry.name;
                        desc.nameCol    = OS::ui::Col(ImGuiCol_Text);
                        desc.scene      = nullptr;
                        desc.flat       = true;
                        desc.flatImage  = OverlayThumbs::Texture(entry.path);
                        desc.flatFailed = OverlayThumbs::Failed(entry.path);
                        desc.selected   = g.makeup[a_index].hasTexture &&
                                        g.makeup[a_index].texture == entry.path;
                        // The same star the overlay picker wears, on the same
                        // key: one file, one star.
                        const auto favKey = Favorites::OverlayKey(entry.path);
                        desc.showStar     = true;
                        desc.favourite    = Favorites::IsFavoriteLine(favKey);

                        const auto r = PreviewCardUI::DrawPreviewCard(desc);
                        if (r.starHovered && r.clicked) {
                            Favorites::ToggleLine(favKey);
                            EditorStyle::PlayUISound("UIMenuFocus");
                            return;
                        }
                        // ⚠ PICKING ART FOR A LAYER THAT IS ALREADY WORN IS
                        // ALWAYS ALLOWED, because it changes the art rather than
                        // the count. Only switching a new one ON is budgeted.
                        if (r.clicked && !MakeupPlan::MayWear(g.makeup, a_index)) {
                            // Refused in silence HERE because the pane says why
                            // above the grid and the layer list says it again.
                            // A third copy on the click would be noise.
                        } else if (r.clicked) {
                            Remember();
                            auto& layer      = g.makeup[a_index];
                            layer.hasTexture = true;
                            layer.texture    = entry.path;
                            // New art starts where its author drew it, the same
                            // rule the overlays picker holds, and the SAME
                            // setting turns it off. One toggle governs both
                            // families because it answers one question: does a
                            // position belong to the art or to the layer. Two
                            // switches saying that in two places would be a
                            // preference with two homes.
                            if (!Settings::GetSingleton().overlayKeepOffset) {
                                layer.transform = OverlayTransform::Transform{};
                            }
                            // ⚠ A LAYER AT ZERO STRENGTH GETS ONE ON THE WAY IN.
                            // Picking art for a slot that is off would otherwise
                            // read as nothing happening at all, because zero
                            // strength is exactly what "off" means here.
                            if (layer.strength <= 0.0f) {
                                layer.strength = 1.0f;
                            }
                            // A click, not a drag, so it lands now.
                            PushMakeup(a_actor, a_index);
                            Tutorial::NotifyAction(Tutorial::Action::kPickedOverlayTexture);
                        }
                        if (r.hovered) {
                            FUCK::SetTooltip(entry.path.c_str());
                        }
                    });
            }
            FUCK::EndChild();
            // Same anchor as the overlays grid: whichever card grid is on
            // screen is the one the pick-a-card step means.
            Tutorial::PublishAnchor(Tutorial::Anchor::kOverlayTextures,
                                    FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
        }

        void DrawMakeupEditor(RE::Actor* a_actor) {
            const auto index    = static_cast<std::size_t>(g.makeupSelected);
            auto&      state    = g.makeup[index];
            const auto type     = index < g.makeupLayers.size() ? g.makeupLayers[index].type : 0u;
            const auto category = MakeupPlan::CategoryOf(type);

            const auto title = std::string{ FUCK::Translate(MakeupPlan::LabelKeyFor(type)) };
            FUCK::TextUnformatted(title.c_str());
            FUCK::Separator();
            if (category == MakeupPlan::Category::kFace) {
                // ⚠ SAID ONCE, WHERE THE MISSING CONTROL WOULD HAVE BEEN. A
                // player who has used RaceMenu knows these carry a colour and
                // nothing else; a player who has not is looking at a pane with
                // no picker in it and deserves the reason rather than a gap.
                OS::ui::TextDisabledWrapped("$FR_Mk_FaceSlotNote"_T);
            }

            float rgb[3]{ static_cast<float>(state.tint.r) / 255.0f,
                          static_cast<float>(state.tint.g) / 255.0f,
                          static_cast<float>(state.tint.b) / 255.0f };
            const auto toBytes = [&](const float a_rgb[3]) {
                return OverlayPlan::Rgb{
                    static_cast<std::uint8_t>(std::clamp(a_rgb[0], 0.0f, 1.0f) * 255.0f),
                    static_cast<std::uint8_t>(std::clamp(a_rgb[1], 0.0f, 1.0f) * 255.0f),
                    static_cast<std::uint8_t>(std::clamp(a_rgb[2], 0.0f, 1.0f) * 255.0f)
                };
            };

            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Ovl_Tint"_T);
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            if (FUCK::ColorEdit3("##mk_tint", rgb,
                                 ImGuiColorEditFlags_DisplayHex |
                                     ImGuiColorEditFlags_NoSmallPreview)) {
                if (!g.hexEditing) {
                    g.hexEditing = true;
                    Remember();
                }
                state.tint = toBytes(rgb);
                // Typed or stepped rather than dragged, so it lands now.
                PushMakeup(a_actor, index);
            }
            if (g.hexEditing && !FUCK::IsAnyItemActive()) {
                g.hexEditing = false;
            }

            {
                float picked[3]{ rgb[0], rgb[1], rgb[2] };
                if (ColourPicker::Draw(g.makeupCarry, rgb, picked)) {
                    // ⚠ ONE STEP PER DRAG, NOT PER FRAME, which is the same
                    // rule the overlays half holds with g.tintDragging. A drag
                    // reports a new colour every frame it moves, and remembering
                    // each one fills the stack with a smear of near identical
                    // colours and makes one undo look like it did nothing.
                    if (!g.makeupDragging) {
                        g.makeupDragging = true;
                        Remember();
                    }
                    state.tint       = toBytes(picked);
                    // ⚠ QUEUED, NOT PUSHED. Every write here ends in a face
                    // retint that rebuilds a texture, so a drag that wrote per
                    // frame would rebuild per frame. See QueueMakeup.
                    QueueMakeup(index);
                }
            }
            // ⚠ THE PICKER'S LATCH COMES DOWN ON THE MOUSE, because a
            // ColorEdit3 cannot be asked whether its edit ended. Once it is
            // clear, the tail call at the end of Draw sends the one write.
            if (g.makeupDragging && !FUCK::IsMouseDown(0)) {
                g.makeupDragging = false;
            }

            float strength = state.strength;
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Mk_Strength"_T);
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            // ⚠⚠ THE SLIDER IS THE OTHER WAY A LAYER GETS SWITCHED ON, since
            // "worn" here means strength above zero. Disabling it outright would
            // also stop the player turning a layer DOWN, which is exactly what
            // someone at the ceiling needs to do, so it stays live and the raise
            // is what gets refused below.
            const bool mayWear = MakeupPlan::MayWear(g.makeup, index);
            if (FUCK::SliderFloat("##mk_strength", &strength, 0.0f, 1.0f, "%.2f")) {
                if (!mayWear && strength > 0.0f) {
                    // Pinned at off. The reason is already on screen twice, in
                    // the layer list and above the picker.
                    strength = 0.0f;
                }
                // ⚠ ONE STEP PER GESTURE, TAKEN WHEN IT STARTS, before the
                // value moves. Taking it at commit time instead would snapshot a
                // state that already holds the new value, and Undo would restore
                // the very change it was asked to remove.
                if (!g.makeupStrengthDragging) {
                    g.makeupStrengthDragging = true;
                    Remember();
                }
                state.strength = MakeupPlan::ClampStrength(strength);
                QueueMakeup(index);
            }
            // ⚠⚠ THE SLIDER COMMITS ITSELF, whatever device drove it. This
            // fires once when the edit ends, which is what keeps a keyboard edit
            // from paying a texture rebake per frame and what keeps a held
            // gesture from being lost if the page stops drawing.
            if (FUCK::IsItemDeactivatedAfterEdit()) {
                g.makeupStrengthDragging = false;
                FlushMakeup(a_actor);
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("$FR_Mk_StrengthTip"_T);
            }

            // ---- reset, which is what a face slot has instead of a clear ----
            //
            // ⚠⚠ IT PUTS THE RACE'S OWN DEFAULT BACK, and on SkinTone that is
            // the RACE's complexion rather than the one this character was made
            // with. That is what "default" means here and the tooltip says so.
            // It is a history step like any other, so a press that turns out to
            // be wrong is one Undo away, and the page's Revert still holds
            // everything back to arrival.
            //
            // ⚠ ABSENT RATHER THAN GREYED WHEN THE RACE NAMES NO DEFAULT.
            // MEASURED: 757 of 1445 vanilla slots carry a TIND colour and 649 of
            // the 688 without it are war paint, which has a clear instead. A
            // dead button on every war paint slot would be a control that is
            // never the answer.
            const auto* const raceSlot = RaceSlot(index);
            if (raceSlot && (raceSlot->hasTint || !raceSlot->texture.empty())) {
                const auto resetBtn = ChamferPanel::Button("$FR_Mk_Reset"_T);
                if (resetBtn.clicked) {
                    Remember();
                    if (raceSlot->hasTint) {
                        state.tint = raceSlot->tint;
                    }
                    // ⚠ THE TEXTURE GOES BACK TOO, and on a face slot that is
                    // normally a no-op because nothing can change it. It matters
                    // for a slot another mod repainted and for the war paint and
                    // dirt slots, where Reset is the way back to the art the
                    // character was born with.
                    if (!raceSlot->texture.empty()) {
                        state.hasTexture = true;
                        state.texture    = raceSlot->texture;
                    }
                    PushMakeup(a_actor, index);
                }
                if (resetBtn.hovered) {
                    FUCK::SetTooltip("$FR_Mk_ResetTip"_T);
                }
            }

            // ⚠⚠ NO TEXTURE PICKER ON A FACE SLOT. Its art is the race's own
            // mask, the SHAPE of that feature on that face, so every texture in
            // the library is the wrong shape for it. This is the control the
            // whole split exists to take away.
            if (!MakeupPlan::AllowsTexture(category)) {
                return;
            }

            // ---- position: the offset sliders (OS-209 on makeup) ---------------
            //
            // ⚠⚠ THE SAME BAKE AS AN OVERLAY'S, AND A DIFFERENT COMMIT. No key
            // and no field moves a tint mask any more than one moves an overlay,
            // so what a slider does here is resample the art at the transform
            // into a file and put that file's path on the mask. What differs is
            // WHEN. An overlay can show a moving slider live, because its art
            // sits on an [Ovl] material this mod can write per frame; a face
            // tint has no such material. The engine composites the masks in a
            // RETINT, a full face texture rebuild, which is the cost the whole
            // makeup half already defers a gesture for. So these land on release,
            // exactly as the strength slider does, and the tooltip says so rather
            // than leaving the player to wonder why the face is not following.
            //
            // ⚠ AND NEVER ON A FACE SLOT. The guard above already returned: that
            // slot's art is the race's own feature mask, and moving it is a
            // different feature from positioning art the player chose.
            //
            // ⚠⚠ THE WHOLE SECTION SITS UNDER ITS OWN ID, and it is not
            // decoration. This section's Reset and the SLOT's Reset above it are
            // both on screen at once and both read "Reset", so without this they
            // are one widget as far as ImGui is concerned: it raised
            // "2 visible items with conflicting ID" on the field's first look
            // (2026-08-31) and pressing either would have driven whichever it
            // resolved to. A PushID is the fix the error itself asks for, and it
            // goes around the section rather than on the one button so anything
            // added here later cannot collide with the slot's controls either.
            if (MakeupPlan::Occupied(state)) {
                FUCK::PushID("mk_pos");
                const bool moved = !OverlayTransform::IsIdentity(state.transform);
                const auto header =
                    std::string{ FUCK::Translate("$FR_Ovl_Position") } + "   " +
                    std::string{ FUCK::Translate(moved ? "$FR_Ovl_PositionMoved"
                                                       : "$FR_Ovl_PositionAsIs") } +
                    "###mk_pos";
                if (OS::ui::FramedHeader(header.c_str())) {
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Mk_PositionTip"_T);
                    }
                    auto&      t      = state.transform;
                    const auto slider = [&](const char* a_id, const char* a_labelKey,
                                            const char* a_tipKey, float& a_value, float a_min,
                                            float a_max, const char* a_fmt) {
                        FUCK::AlignTextToFramePadding();
                        FUCK::TextUnformatted(FUCK::Translate(a_labelKey));
                        FUCK::SameLine();
                        FUCK::SetNextItemWidth(-1.0f);
                        if (FUCK::SliderFloat(a_id, &a_value, a_min, a_max, a_fmt)) {
                            // One step per gesture, taken when it starts, the
                            // rule every control on this page holds.
                            if (!g.makeupPosDragging) {
                                g.makeupPosDragging = true;
                                Remember();
                            }
                            t = OverlayTransform::Clamp(t);
                            QueueMakeup(index);
                        }
                        // ⚠ THE SLIDER COMMITS ITSELF, whatever device drove it,
                        // and the QUANTISE happens here rather than per frame:
                        // the cache key is the quantised transform, so rounding
                        // on the way out is what stops a drag spawning a file a
                        // frame.
                        if (FUCK::IsItemDeactivatedAfterEdit()) {
                            g.makeupPosDragging = false;
                            t                   = OverlayTransform::Quantise(t);
                            QueueMakeup(index);
                            FlushMakeup(a_actor);
                        }
                        if (FUCK::IsItemHovered()) {
                            FUCK::SetTooltip(FUCK::Translate(a_tipKey));
                        }
                    };
                    slider("##mk_pos_x", "$FR_Ovl_OffsetX", "$FR_Mk_OffsetXTip", t.offsetX,
                           OverlayTransform::kOffsetMin, OverlayTransform::kOffsetMax, "%.3f");
                    slider("##mk_pos_y", "$FR_Ovl_OffsetY", "$FR_Ovl_OffsetYTip", t.offsetY,
                           OverlayTransform::kOffsetMin, OverlayTransform::kOffsetMax, "%.3f");
                    slider("##mk_pos_s", "$FR_Ovl_Scale", "$FR_Ovl_ScaleTip", t.scale,
                           OverlayTransform::kScaleMin, OverlayTransform::kScaleMax, "%.2fx");
                    slider("##mk_pos_r", "$FR_Ovl_Rotation", "$FR_Ovl_RotationTip",
                           t.rotationDeg, OverlayTransform::kRotMin, OverlayTransform::kRotMax,
                           "%.0f");
                    // The same toggle the overlays page carries, and the same
                    // setting behind it: it governs both pickers.
                    if (FUCK::Checkbox("$FR_Ovl_KeepOffset"_T,
                                       &Settings::GetSingleton().overlayKeepOffset)) {
                        Settings::GetSingleton().Save();
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_KeepOffsetTip"_T);
                    }
                    // ⚠ ONE STEP AND ONE WRITE, offered only when there is
                    // something to reset, so the button cannot take a history
                    // step that changes nothing.
                    FUCK::BeginDisabled(!moved);
                    if (ChamferPanel::Button("$FR_Ovl_PositionReset"_T).clicked && moved) {
                        Remember();
                        t = OverlayTransform::Transform{};
                        QueueMakeup(index);
                        FlushMakeup(a_actor);
                    }
                    FUCK::EndDisabled();
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Mk_PositionResetTip"_T);
                    }
                }
                FUCK::PopID();
            }

            FUCK::Spacing();
            FUCK::Separator();
            DrawMakeupPicker(a_actor, index, category);
        }

        // ---- the skin editor: what the chosen pack is made of ----------------
        //
        // There is nothing to adjust on a skin, so the right pane explains
        // rather than edits: which files the worn pack carries, and for the
        // game's own skin, how a pack is made. The rows on the left are the
        // controls.
        // ⚠ THE SHARED CARD AND THE SHARED GRID, for the overlay picker's
        // reason: a tile drawn beside them can only approximate them. The
        // picture is a rendered scene here (SkinCardScene.h), the character's
        // own parts with the pack's files swapped in by the live rule, so the
        // card shows the pack on THIS character and the game's own skin is a
        // card like the rest rather than a note.
        void DrawSkinEditor(RE::Actor* a_actor) {
            FUCK::TextUnformatted("$FR_Skin_Section"_T);
            FUCK::Separator();
            if (!SkinApi::Available()) {
                OS::ui::TextDisabledWrapped(FUCK::Translate(SkinApi::UnavailableKey()));
                return;
            }

            const auto snap = SkinPacks::Get();
            // What fits this character and what the default is called, both
            // measured on the game thread when the page opened on them.
            const auto fit = SkinApi::Fit(a_actor);

            DrawSearchRow("##skin_search", g.skinSearch, "$FR_Skin_Search");
            std::string search = g.skinSearch;
            std::transform(search.begin(), search.end(), search.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // The parts every card is built from, resolved once per frame:
            // the torso first, then the head, hands and feet the fit walk
            // resolved for this character. Empty on a target with no skin,
            // which draws every card as its cross rather than as a lie.
            std::vector<std::string> parts;
            if (const auto body = Mannequin::BodyPath(); !body.empty()) {
                parts.push_back(body);
            }
            for (auto& extra : Mannequin::SubjectExtras()) {
                parts.push_back(std::move(extra));
            }

            // The rows of the grid: the game's own skin first, then a worn
            // pack this rig does not have (a save from another install, or a
            // folder renamed since; drawn as a cross and never hidden, or the
            // row on the left would name a pack no card explains), then each
            // installed pack in scan order.
            struct Card {
                const SkinPlan::Pack* pack{ nullptr };
                std::string           name;
                bool                  missing{ false };
                // ⚠⚠ A RIVAL IS NOT A PACK YET. It is a skin mod this load
                // order installed and then overrode, so its files are not in
                // the virtual tree at all: no preview can be photographed from
                // them, no star can be kept for something that cannot be worn,
                // and the scan hard links it for you rather than the card
                // waiting on a press.
                bool                  rival{ false };
                bool                  linked{ false };  // the scan already hard linked it
                std::string           mod;  // the folder name, without the suffix
                // ⚠⚠ HALF VISIBLE, WHICH IS NOT THE SAME AS ABSENT. A pack
                // whose folder is entirely new does not draw at all until the
                // restart, and the rival card above says so. A pack that
                // ALREADY existed and gained files this session draws, wears,
                // and quietly leaves out everything usvfs did not see at
                // launch. Non-zero is how many files are waiting.
                std::size_t           waiting{ 0 };
            };
            std::vector<Card> cards;
            // ⚠ THE BASE SKIN CARD IS NAMED AFTER THE SKIN THE LOAD ORDER GIVES
            // (user 2026-08-18: "is there a way we can find a name of the skin
            // used in game default"). Under Mod Organizer that is the mod
            // folder the body's diffuse really lives in (MEASURED 2026-08-18
            // 17:06: usvfs redirects the open and GetFinalPathNameByHandle
            // reports the real file, 'UBE 2.0 Jada 8.1 Skin'); elsewhere the
            // family folder; failing both, "Base Skin". SkinApi::Fit measured
            // it and the log named which answer it was.
            const std::string baseName =
                fit.defaultName.empty() ? std::string{ FUCK::Translate("$FR_Skin_Default") }
                                        : fit.defaultName;
            // ⚠ A CARD LIKE THE OTHERS (user 2026-08-18: "we should be able to
            // favorite it as well"): it carries a star under kBaseSkinId, so
            // the Favorites filter and the search see it as they see a pack.
            // Always shown while it is worn, the worn pack's own rule, so the
            // row on the left never names a card that is not there.
            {
                const bool worn = g.skinCurrent.empty();
                bool       show = worn;
                if (!worn) {
                    show = !(g.favoritesOnly &&
                             !Favorites::IsFavoriteLine(
                                 Favorites::SkinKey(SkinPlan::kBaseSkinId)));
                    if (show && !search.empty()) {
                        std::string hay = baseName;
                        std::transform(hay.begin(), hay.end(), hay.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        show = hay.find(search) != std::string::npos;
                    }
                }
                if (show) {
                    cards.push_back(Card{ nullptr, baseName, false });
                }
            }
            if (!g.skinCurrent.empty() && !SkinPacks::Find(*snap, g.skinCurrent)) {
                cards.push_back(Card{ nullptr, g.skinCurrent, true });
            }
            // ⚠ A PACK MADE FOR ANOTHER BODY IS HIDDEN (user 2026-08-18: "hide
            // ube skin card on 3ba"): SkinPlan::Fits against the file names the
            // character's live skin shapes read. The worn pack always shows,
            // whatever it fits, so the row on the left never names a card that
            // is not there; the show-all-art switch shows them all, the same
            // switch that lifts every other filter on this page; and an
            // unmeasured fit hides nothing.
            const bool showAll = Settings::GetSingleton().overlayShowAllArt;
            std::size_t hiddenForBody = 0;
            for (const auto& pack : snap->packs) {
                const bool worn = pack.id == g.skinCurrent;
                if (!worn && !showAll && !SkinPlan::Fits(pack, fit.names)) {
                    ++hiddenForBody;
                    continue;
                }
                if (!worn && g.favoritesOnly &&
                    !Favorites::IsFavoriteLine(Favorites::SkinKey(pack.id))) {
                    continue;
                }
                if (!worn && !search.empty()) {
                    std::string hay = pack.id;
                    std::transform(hay.begin(), hay.end(), hay.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (hay.find(search) == std::string::npos) {
                        continue;
                    }
                }
                Card card{ &pack, pack.id, false };
                card.waiting = SkinRivals::FreshLinks(pack.id);
                cards.push_back(std::move(card));
            }
            // ---- the skins this load order installed and then hid ----------
            //
            // ⚠⚠ A RIVAL LINKED IN AN EARLIER SESSION IS A PACK NOW, and the
            // scan still reports it, because it still IS a sibling mod carrying
            // these files. Skipping any rival a pack already answers to is what
            // stops it drawing twice, and it self-corrects across exactly the
            // restart the link needs anyway.
            const auto rivals = SkinRivals::Get();
            if (rivals) {
                for (const auto& rival : rivals->rivals) {
                    if (SkinPacks::Find(*snap, rival.mod)) {
                        continue;
                    }
                    // ⚠⚠ THE BASE SKIN'S OWN MOD IS ALREADY A CARD. The scan
                    // deliberately stops excluding winners, because a skin that
                    // wins one file out of twenty-six is still a skin you want
                    // to try; the single mod that must not draw twice is the
                    // one the Base Skin card is named after, and this is the
                    // only place that name is known.
                    if (_stricmp(rival.mod.c_str(), baseName.c_str()) == 0) {
                        continue;
                    }
                    // ⚠ NOTHING UNLINKED CAN BE A FAVOURITE, so the favourites
                    // filter hides every rival rather than drawing a row of
                    // cards that cannot carry the star the filter is about.
                    if (g.favoritesOnly) {
                        continue;
                    }
                    if (!search.empty()) {
                        std::string hay = rival.mod;
                        std::transform(hay.begin(), hay.end(), hay.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        if (hay.find(search) == std::string::npos) {
                            continue;
                        }
                    }
                    Card card;
                    card.mod    = rival.mod;
                    card.rival  = true;
                    // ⚠ THE SCAN LINKS THESE ITSELF NOW, so the two states are
                    // "linked, waiting on a restart" and "the link failed".
                    // Neither is a thing to click, which is the point.
                    card.linked = SkinRivals::Linked(rival.mod);
                    card.name   = rival.mod + " " +
                                  std::string{ card.linked
                                                   ? FUCK::Translate("$FR_Skin_RivalLinkedSuffix")
                                                   : FUCK::Translate("$FR_Skin_RivalSuffix") };
                    cards.push_back(std::move(card));
                }
            }
            if (snap->packs.empty() && (!rivals || rivals->rivals.empty())) {
                OS::ui::TextDisabledWrapped(SkinPacks::Scanning() ? "$FR_Skin_Scanning"_T
                                                                  : "$FR_Skin_NoPacks"_T);
            }
            // ⚠⚠ MOD ORGANIZER ONLY, AND IT SAYS SO RATHER THAN DRAWING
            // NOTHING. Vortex deploys by hard link straight into Data with no
            // mods layer, so no rival can be found there at all. An empty
            // result with no explanation reads as "you have one skin
            // installed", which for most load orders is simply false.
            // ⚠⚠ AND TWO REASONS ARRIVE HERE, NOT ONE. The scan is also
            // unusable when the body slot still reads a file this plugin wrote
            // and no stored row names what was under it, which has nothing to
            // do with a mod manager. The field caught the page telling a Mod
            // Organizer user they were not running one (2026-08-27).
            if (rivals && !rivals->Usable() && !SkinRivals::Scanning()) {
                OS::ui::TextDisabledWrapped(fit.ownFileUnderneath ? "$FR_Skin_RivalsOwnFile"_T
                                                                  : "$FR_Skin_RivalsMO2"_T);
            }
            if (g.skinLinkFailed) {
                OS::ui::TextDisabledWrapped("$FR_Skin_RivalFailed"_T);
            }
            // Once per (rig, count), so a pack author whose folder vanished
            // from the grid can read why without a second round.
            if (hiddenForBody != 0) {
                static std::size_t s_saidFor = static_cast<std::size_t>(-1);
                if (s_saidFor != hiddenForBody) {
                    s_saidFor = hiddenForBody;
                    spdlog::info("OverlaysUI: {} skin pack(s) hidden because none of their file "
                                 "names is on this character's skin ({} live name(s)); the "
                                 "show-all-art setting shows them anyway.",
                                 hiddenForBody, fit.names.size());
                }
            }

            // The overlay picker's size, for the same reason it grew: the thing
            // being judged is a skin over a whole figure, and pores, freckles
            // and scales read as noise at the size a helmet is recognisable at.
            const float side = OS::ui::FontSize() * 5.6f;

            FUCK::BeginChild("skin_cards", ImVec2(0, 0), false);
            PreviewCardUI::DrawCardGrid(
                cards.size(), side, [&](std::size_t a_i, float a_fitted) {
                    const auto& card = cards[a_i];
                    // ⚠ THE SCENE IS A LOCAL AND THAT IS SAFE: DrawPreviewCard
                    // reads the pointer synchronously and the request copies
                    // what it needs (the body library does the same).
                    const auto scene = SkinCardScene::Build(parts, card.pack);
                    PreviewCardUI::PreviewCardDesc desc;
                    // A pack has no form; the id stack is scoped by a hash of
                    // its name with the ordinal mixed in, the body library's
                    // recipe against two cards hashing alike.
                    desc.pushId = static_cast<std::uint32_t>(
                        (PreviewGrid::Fnv1a64(card.name) ^
                         (static_cast<std::uint64_t>(a_i) * 0x9E3779B97F4A7C15ull)) &
                        0xFFFFFFFFull);
                    desc.side    = a_fitted;
                    desc.name    = &card.name;
                    desc.nameCol = OS::ui::Col(ImGuiCol_Text);
                    // ⚠ A RIVAL HAS NO PREVIEW AND THAT IS NOT A BUG. Its files
                    // are outside the virtual tree until it is linked, so there
                    // is nothing the card could photograph; it draws as a plate
                    // with a name, like a pack whose folder went missing.
                    desc.scene   = (card.missing || card.rival || parts.empty()) ? nullptr
                                                                                 : &scene;
                    // ⚠⚠ A CROSS READS AS AN ERROR AND THESE ARE NOT ERRORS
                    // (user 2026-08-26: "rather than an X maybe a refresh
                    // symbol so the user knows they have to restart the game").
                    // A linked skin is fine, it is simply not in the virtual
                    // tree until the next launch. A link that FAILED keeps the
                    // cross, because that one really is wrong.
                    static const std::string kRestart = Icons::Utf8(Icons::kRedo);
                    desc.emptyGlyph = (card.rival && card.linked) ? kRestart.c_str() : nullptr;
                    // ⚠ THE SAME ARROW ON A CARD THAT DOES HAVE A PICTURE. The
                    // empty glyph above stands IN for the picture and cannot say
                    // this, because a half visible pack photographs perfectly well
                    // out of the files that ARE there. That is exactly why it needs
                    // saying: nothing about the card looks wrong.
                    desc.needsRestart = card.waiting != 0;
                    // ⚠ THE GLOW IS WHAT IS WORN, the same meaning it has on
                    // the gear grid, and this pane has no separate "selected":
                    // clicking a card wears it.
                    desc.selected = card.missing  ? true
                                    : card.rival  ? false
                                    : card.pack   ? card.pack->id == g.skinCurrent
                                                  : g.skinCurrent.empty();
                    // The star, on every card that can be worn: a pack under
                    // its id, the base skin under kBaseSkinId. A missing pack
                    // is not a thing to click, so no star.
                    const std::string favKey =
                        (card.missing || card.rival) ? std::string{}
                        : card.pack                  ? Favorites::SkinKey(card.pack->id)
                                                     : Favorites::SkinKey(SkinPlan::kBaseSkinId);
                    desc.showStar  = !card.missing && !card.rival;
                    desc.favourite = !favKey.empty() && Favorites::IsFavoriteLine(favKey);
                    const auto r = PreviewCardUI::DrawPreviewCard(desc);
                    if (r.starHovered && r.clicked && !favKey.empty()) {
                        Favorites::ToggleLine(favKey);
                        EditorStyle::PlayUISound("UIMenuFocus");
                        return;
                    }
                    // ⚠⚠ CLICKING A RIVAL LINKS IT AND DOES NOT WEAR IT, because
                    // wearing it is impossible until usvfs has seen the files,
                    // which it only does at launch. Saying that out loud in the
                    // log matters: a player who links a skin and sees nothing
                    // change will conclude the feature is broken.
                    // ⚠ CLICKING A LINKED RIVAL DOES NOTHING ON PURPOSE. It is
                    // already linked and it cannot be worn until the game
                    // restarts, so wearing it now is not a thing the click
                    // could honestly do. A FAILED one retries, which is the
                    // only case where a press has an outcome.
                    if (r.clicked && card.rival) {
                        if (card.linked) {
                            return;
                        }
                        std::string why;
                        if (SkinRivals::Link(card.mod, why) == 0) {
                            spdlog::warn("OverlaysUI: could not link '{}': {}.", card.mod, why);
                            g.skinLinkFailed = true;
                            EditorStyle::PlayUISound("UIMenuCancel");
                        } else {
                            g.skinLinkFailed = false;
                            EditorStyle::PlayUISound("UIMenuOK");
                        }
                        return;
                    }
                    if (r.clicked && !card.missing) {
                        ChooseSkin(a_actor, card.pack ? card.pack->id : std::string{});
                    }
                    if (r.hovered) {
                        // ⚠⚠ THE WHOLE NAME FIRST, BECAUSE THE CARD ONLY HAS
                        // ROOM FOR PART OF IT (user 2026-08-26: "make sure that
                        // the tooltip tells us the full name of the card, as
                        // often they get cut out"). Skin mods are named at
                        // length, "[LRO] UBE Scarlet SB 1.02" and
                        // "Joola UBE Racemenu Preset and Skin", and the card
                        // cuts to three dots, so the hover is the only place
                        // the rest of the name exists at all.
                        //
                        // ⚠ GATED ON nameTrimmed, the same shape the styles grid
                        // and the looks grid already use: a name that fits is
                        // already on screen and repeating it in the hover is a
                        // line the reader has to skip past to reach the answer.
                        std::string tip;
                        if (r.nameTrimmed) {
                            tip = card.name;
                            tip += '\n';
                        }
                        if (card.missing) {
                            tip += "$FR_Skin_MissingTip"_T;
                            FUCK::SetTooltip(tip.c_str());
                        } else if (card.rival) {
                            tip += card.linked ? "$FR_Skin_RivalLinkedTip"_T
                                               : "$FR_Skin_RivalTip"_T;
                            FUCK::SetTooltip(tip.c_str());
                        } else {
                            // ⚠ THE SOURCE AND NOTHING ELSE (user 2026-08-18:
                            // "make the tooltip for skin just say the source of
                            // the skin texture"). The explanation of what a
                            // pack does and how a pack is made lives on the
                            // section's tip and the page's how-to; the card's
                            // tip is the file path, as an overlay card's is.
                            // ⚠ THE RESTART LEADS. It is the answer to the question
                            // the player is about to ask, and the source path is not.
                            if (card.waiting != 0) {
                                tip += "$FR_Skin_PartialTip"_T;
                                tip += '\n';
                            }
                            tip += SkinSourceOf(a_actor,
                                                card.pack ? card.pack->id : std::string{});
                            FUCK::SetTooltip(tip.c_str());
                        }
                    }
                });
            FUCK::EndChild();
        }

        void DrawLayerEditor(RE::Actor* a_actor, bool a_narrow) {
            if (a_narrow && ChamferPanel::Button("$FR_Ovl_ToLayers"_T).clicked) {
                g.narrowPicker = false;
            }
            if (SkinSelected()) {
                DrawSkinEditor(a_actor);
                return;
            }
            if (MakeupSelected()) {
                DrawMakeupEditor(a_actor);
                return;
            }
            if (!Selected()) {
                // Centred in both axes, the same empty-pane hint the outfit
                // page's right half uses. A left-aligned line at the top of an
                // otherwise blank panel reads as a label for something missing
                // rather than as an instruction.
                EditorNotice::DrawCentred({ FUCK::Translate("$FR_Ovl_PickLayer"),
                                            FUCK::Translate("$FR_Ovl_PickLayer2") });
                return;
            }
            const auto  index = static_cast<std::size_t>(g.selected);
            const auto& layer = Layers()[index];
            auto&       state = g.layers[index];

            const auto title = std::string{ FUCK::Translate(
                                   OverlayPlan::kLocations[OverlayPlan::Slot(layer.location)]
                                       .labelKey) } +
                               "  " + std::to_string(layer.index + 1);
            FUCK::TextUnformatted(title.c_str());
            FUCK::Separator();

            // ---- colour and alpha, the two controls a layer always has ------

            float rgb[3]{ static_cast<float>(state.tint.r) / 255.0f,
                          static_cast<float>(state.tint.g) / 255.0f,
                          static_cast<float>(state.tint.b) / 255.0f };
            const auto toBytes = [&](const float a_rgb[3]) {
                return OverlayPlan::Rgb{
                    static_cast<std::uint8_t>(std::clamp(a_rgb[0], 0.0f, 1.0f) * 255.0f),
                    static_cast<std::uint8_t>(std::clamp(a_rgb[1], 0.0f, 1.0f) * 255.0f),
                    static_cast<std::uint8_t>(std::clamp(a_rgb[2], 0.0f, 1.0f) * 255.0f)
                };
            };

            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Ovl_Tint"_T);
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            // The hex field, kept for typing an exact value and reading one out.
            if (FUCK::ColorEdit3("##ovl_tint", rgb,
                                 ImGuiColorEditFlags_DisplayHex |
                                     ImGuiColorEditFlags_NoSmallPreview)) {
                if (!g.hexEditing) {
                    g.hexEditing = true;
                    Remember();
                }
                state.hasTint = true;
                state.tint    = toBytes(rgb);
                Push(a_actor, index);
            }
            if (g.hexEditing && !FUCK::IsAnyItemActive()) {
                g.hexEditing = false;
            }

            // ⚠ THE SAME PICKER THE DYE PANE HAS, AND IT IS OFFERED IN BOTH
            // PLAYSTYLES (user 2026-08-16). Dye hides it under lore-friendly
            // because a colour there has to be earned; body paint is not part of
            // that economy, so gating it would import an unlock rule from a
            // system this page has nothing to do with. Sharing the WIDGET while
            // the two callers answer the gate differently is exactly the split
            // ColourPicker.h describes.
            {
                float picked[3]{ rgb[0], rgb[1], rgb[2] };
                if (ColourPicker::Draw(g.tintCarry, rgb, picked)) {
                    // ⚠ ONE HISTORY STEP PER DRAG, NOT PER FRAME. A drag
                    // reports a new colour every frame it moves, and remembering
                    // each one would fill the undo stack with a smear of near
                    // identical colours and make one undo look like it did
                    // nothing. The step is taken when the drag STARTS.
                    if (!g.tintDragging) {
                        g.tintDragging = true;
                        Remember();
                    }
                    state.hasTint = true;
                    state.tint    = toBytes(picked);
                    Push(a_actor, index);
                }
            }
            if (g.tintDragging && !FUCK::IsMouseDown(0)) {
                g.tintDragging = false;
            }

            float alpha = state.alpha;
            FUCK::AlignTextToFramePadding();
            FUCK::TextUnformatted("$FR_Ovl_Alpha"_T);
            FUCK::SameLine();
            FUCK::SetNextItemWidth(-1.0f);
            if (FUCK::SliderFloat("##ovl_alpha", &alpha, 0.0f, 1.0f, "%.2f")) {
                // ⚠ THE STEP IS TAKEN WHEN THE DRAG STARTS, before the value
                // moves. Same rule g.tintDragging holds for the picker below and
                // ShapeUI holds for every shape slider.
                if (!g.alphaDragging) {
                    g.alphaDragging = true;
                    Remember();
                }
                state.hasAlpha = true;
                state.alpha    = OverlayPlan::ClampAlpha(alpha);
                Push(a_actor, index);
            }
            // ⚠ THE COMMIT QUERY IS SAFE ON A SLIDER AND IS MEASURED DEAD UNDER
            // A ColorEdit3 in this FUCK build, so it is used here and never on
            // the colour fields. See the note at EditorUI.cpp on the dye picker.
            if (FUCK::IsItemDeactivatedAfterEdit()) {
                g.alphaDragging = false;
            }

            // ---- the glow, which is a second colour ------------------------
            //
            // ⚠⚠ IT IS NOT THE SAME COLOUR AS THE ONE ABOVE, and a page that
            // treated it as one could not reach a black overlay on half the
            // library. The tint colours the diffuse; this colours what the
            // shape EMITS, and a material that emits keeps emitting whatever
            // the diffuse was tinted to.
            //
            // ⚠ FOLDED AWAY, because the answer is right at zero nearly always
            // and an open section would put two more controls above the picker
            // on every visit. The header carries the strength so a layer that
            // IS glowing says so without being opened.
            {
                const auto header =
                    std::string{ FUCK::Translate("$FR_Ovl_Glow") } + "   " +
                    (state.glowStrength > 0.0f
                         ? OS::ui::FormatF("%.0f%%",
                                           OverlayPlan::GlowPercent(state.glowStrength))
                         : std::string{ FUCK::Translate("$FR_Ovl_GlowOff") }) +
                    "###ovl_glow";
                if (OS::ui::FramedHeader(header.c_str())) {
                    float glowRgb[3]{ static_cast<float>(state.glow.r) / 255.0f,
                                      static_cast<float>(state.glow.g) / 255.0f,
                                      static_cast<float>(state.glow.b) / 255.0f };
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted("$FR_Ovl_GlowColour"_T);
                    FUCK::SameLine();
                    FUCK::SetNextItemWidth(-1.0f);
                    // ⚠ THE HEX FIELD ALONE, NOT A SECOND FULL PICKER. One
                    // picker on a pane is a control; two is a colour editor,
                    // and the glow is the rarer of the two by a long way.
                    if (FUCK::ColorEdit3("##ovl_glow_col", glowRgb,
                                         ImGuiColorEditFlags_DisplayHex)) {
                        if (!g.glowHexEditing) {
                            g.glowHexEditing = true;
                            Remember();
                        }
                        state.glow = toBytes(glowRgb);
                        Push(a_actor, index);
                    }
                    if (g.glowHexEditing && !FUCK::IsAnyItemActive()) {
                        g.glowHexEditing = false;
                    }
                    float percent = OverlayPlan::GlowPercent(state.glowStrength);
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted("$FR_Ovl_GlowStrength"_T);
                    FUCK::SameLine();
                    FUCK::SetNextItemWidth(-1.0f);
                    // ⚠ A PERCENTAGE, AND THE HUNDRED IS NOT RaceMenu'S 25.5
                    // (user 2026-08-16: "if even .1 is strong then 25.5 is
                    // overkill"). The stored value is still RaceMenu's multiple,
                    // because that is what goes on the wire and what its own UI
                    // reads back; only the slider's units and its ceiling are
                    // ours. See kGlowStrengthMax for why full travel is 1.0.
                    if (FUCK::SliderFloat("##ovl_glow_mult", &percent, 0.0f, 100.0f,
                                          "%.0f%%")) {
                        if (!g.glowDragging) {
                            g.glowDragging = true;
                            Remember();
                        }
                        state.glowStrength = OverlayPlan::GlowFromPercent(percent);
                        Push(a_actor, index);
                    }
                    if (FUCK::IsItemDeactivatedAfterEdit()) {
                        g.glowDragging = false;
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_GlowStrengthTip"_T);
                    }
                }
            }

            // ---- the finish: how shiny the layer is (OS-226 S1) -------------
            //
            // ⚠⚠ NO MATERIAL IS TOUCHED HERE AND THAT IS THE WHOLE POINT OF THE
            // STAGE. Glossiness and specular strength are skee's own keys 2 and
            // 3, so skee writes them, keeps them in its co-save and repaints
            // them at every install. A finish set here survives a save, a
            // reload and a RaceMenu trip with nothing from this mod running.
            //
            // ⚠ FOLDED, LIKE THE GLOW, and for the same reason: most layers
            // never want one, and two more sliders above the picker on every
            // visit would cost every layer for the few that do. The header
            // carries the state so a layer that HAS a finish says so closed.
            //
            // ⚠⚠ HIDDEN SINCE 2026-08-19, BY THE USER'S CALL ("since we can't
            // get the Finish to work for overlays hide it for now"). The stage
            // is built and plumbed end to end (the log reads `APPLIED:
            // specPower=500.0 specScale=10.000` on the live material) and the
            // pixels do not move on this rig: a vanilla material float is not a
            // delivery mechanism where a skin shader replacement owns the
            // shading, and the user does not want Community Shaders touched. Two
            // sliders that visibly do nothing came out of the panel; the state,
            // the push and the eraser stay, so a layer that already carries a
            // finish keeps it and flipping this constant puts the panel back
            // unchanged. A compile-time constant and not an INI key on purpose:
            // this is a decision about the shipped panel, not a field probe
            // (a-spike-key-must-die-when-its-feature-lands).
            constexpr bool kFinishSectionShown = false;
            if constexpr (kFinishSectionShown) {
                const auto header =
                    std::string{ FUCK::Translate("$FR_Ovl_Finish") } + "   " +
                    (state.hasFinish
                         ? OS::ui::FormatF("%.0f%%", OverlayPlan::GlossPercent(state.gloss))
                         : std::string{ FUCK::Translate("$FR_Ovl_FinishSkin") }) +
                    "###ovl_finish";
                if (OS::ui::FramedHeader(header.c_str())) {
                    // ⚠ THE SLIDERS OPEN ON THE MATERIAL'S OWN NUMBERS WHEN THE
                    // LAYER HAS NO FINISH, not on zero. 30 and 3 were measured
                    // off all nine installed layers on 2026-08-18 and are what
                    // the RaceMenu template parses to, so the first drag moves
                    // away from where the layer actually is instead of jumping.
                    float glossPct = OverlayPlan::GlossPercent(state.gloss);
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted("$FR_Ovl_FinishGloss"_T);
                    FUCK::SameLine();
                    FUCK::SetNextItemWidth(-1.0f);
                    // ⚠ A PERCENTAGE OVER A SQUARED CURVE, the same shape the
                    // glow slider settled on. The stored value is RaceMenu's
                    // own glossiness because that is what goes on the wire; only
                    // the units and the curve are ours. See kGlossMax.
                    if (FUCK::SliderFloat("##ovl_gloss", &glossPct, 0.0f, 100.0f, "%.0f%%")) {
                        if (!g.finishDragging) {
                            g.finishDragging = true;
                            Remember();
                        }
                        state.hasFinish = true;
                        state.gloss     = OverlayPlan::GlossFromPercent(glossPct);
                        Push(a_actor, index);
                    }
                    if (FUCK::IsItemDeactivatedAfterEdit()) {
                        g.finishDragging = false;
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_FinishGlossTip"_T);
                    }

                    float specular = state.specular;
                    FUCK::AlignTextToFramePadding();
                    FUCK::TextUnformatted("$FR_Ovl_FinishSpecular"_T);
                    FUCK::SameLine();
                    FUCK::SetNextItemWidth(-1.0f);
                    // ⚠ LINEAR, AND NOT FOR WANT OF CONSISTENCY. The whole range
                    // is ten units with the default at three; a curve there would
                    // be ceremony rather than control.
                    if (FUCK::SliderFloat("##ovl_specular", &specular, 0.0f,
                                          OverlayPlan::kSpecularMax, "%.1f")) {
                        if (!g.finishDragging) {
                            g.finishDragging = true;
                            Remember();
                        }
                        state.hasFinish = true;
                        state.specular  = OverlayPlan::ClampSpecular(specular);
                        Push(a_actor, index);
                    }
                    if (FUCK::IsItemDeactivatedAfterEdit()) {
                        g.finishDragging = false;
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_FinishSpecularTip"_T);
                    }

                    // ⚠ THE ERASER ASKS THE SLIDERS' OWN QUESTION. Both sliders
                    // set hasFinish and this clears it, and the header above
                    // reads the same field, so there is exactly one answer to
                    // "does this layer carry a finish of ours"
                    // (a-mark-and-its-eraser-must-ask-one-question). Clearing it
                    // makes the write REMOVE both keys, which is what puts the
                    // skin's own finish back rather than a number we chose.
                    FUCK::BeginDisabled(!state.hasFinish);
                    const auto resetFinish = ChamferPanel::Button("$FR_Ovl_FinishReset"_T);
                    FUCK::EndDisabled();
                    if (resetFinish.clicked && state.hasFinish) {
                        Remember();
                        state.hasFinish = false;
                        state.gloss     = OverlayPlan::kDefaultGloss;
                        state.specular  = OverlayPlan::kDefaultSpecular;
                        Push(a_actor, index);
                    }
                    // ⚠ .hovered AND NOT IsItemHovered(). ChamferPanel::Button
                    // submits its LABEL last, so a last-item query after one
                    // asks about the text rather than about the button.
                    if (resetFinish.hovered) {
                        FUCK::SetTooltip("$FR_Ovl_FinishResetTip"_T);
                    }
                }
            }

            // ---- position: the offset sliders (OS-209) ------------------------
            //
            // ⚠⚠ A DRAG SHOWS AN IN-MEMORY BAKE AND A RELEASE WRITES A FILE. No
            // skee key moves an overlay (measured, see OverlayBake.h), so what
            // a slider does is resample the art at the transform. While the
            // mouse is down that goes into a texture this mod keeps and puts on
            // the [Ovl] material directly, one build per frame; on release the
            // same picture is baked to disk and its path goes into key 9 through
            // Push, and skee owns it from there. Undo restores the previous
            // transform through the same Push.
            //
            // ⚠ ONLY ON AN OCCUPIED LAYER. There is nothing to move on an
            // empty one, and a transform with no art would bake nothing.
            if (OverlayPlan::Occupied(state)) {
                const bool moved = !OverlayTransform::IsIdentity(state.transform);
                const auto header =
                    std::string{ FUCK::Translate("$FR_Ovl_Position") } + "   " +
                    std::string{ FUCK::Translate(moved ? "$FR_Ovl_PositionMoved"
                                                       : "$FR_Ovl_PositionAsIs") } +
                    "###ovl_pos";
                if (OS::ui::FramedHeader(header.c_str())) {
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_PositionTip"_T);
                    }
                    auto& t        = state.transform;
                    bool  changed  = false;
                    bool  released = false;
                    const auto slider = [&](const char* a_id, const char* a_labelKey,
                                            const char* a_tipKey, float& a_value, float a_min,
                                            float a_max, const char* a_fmt) {
                        FUCK::AlignTextToFramePadding();
                        FUCK::TextUnformatted(FUCK::Translate(a_labelKey));
                        FUCK::SameLine();
                        FUCK::SetNextItemWidth(-1.0f);
                        if (FUCK::SliderFloat(a_id, &a_value, a_min, a_max, a_fmt)) {
                            // The step is taken when the gesture STARTS, the
                            // rule every slider on this page holds.
                            if (!g.posDragging) {
                                g.posDragging = true;
                                Remember();
                            }
                            changed = true;
                        }
                        if (FUCK::IsItemDeactivatedAfterEdit()) {
                            released = true;
                        }
                        if (FUCK::IsItemHovered()) {
                            FUCK::SetTooltip(FUCK::Translate(a_tipKey));
                        }
                    };
                    slider("##ovl_pos_x", "$FR_Ovl_OffsetX", "$FR_Ovl_OffsetXTip", t.offsetX,
                           OverlayTransform::kOffsetMin, OverlayTransform::kOffsetMax, "%.3f");
                    slider("##ovl_pos_y", "$FR_Ovl_OffsetY", "$FR_Ovl_OffsetYTip", t.offsetY,
                           OverlayTransform::kOffsetMin, OverlayTransform::kOffsetMax, "%.3f");
                    slider("##ovl_pos_s", "$FR_Ovl_Scale", "$FR_Ovl_ScaleTip", t.scale,
                           OverlayTransform::kScaleMin, OverlayTransform::kScaleMax, "%.2fx");
                    slider("##ovl_pos_r", "$FR_Ovl_Rotation", "$FR_Ovl_RotationTip",
                           t.rotationDeg, OverlayTransform::kRotMin, OverlayTransform::kRotMax,
                           "%.0f");
                    if (changed) {
                        t = OverlayTransform::Clamp(t);
                        OverlayBake::Preview(a_actor->GetHandle(), layer.node, state.texture, t);
                    }
                    if (released) {
                        g.posDragging = false;
                        t             = OverlayTransform::Quantise(t);
                        Push(a_actor, index);
                    }
                    // ⚠ THE TOGGLE LIVES HERE, BESIDE THE NUMBERS IT KEEPS, and
                    // not in the settings panel where bOverlayShowAllArt went.
                    // That one is a picker filter set once; this is a mode a
                    // player flips mid-session, between one design and the next,
                    // and it only means anything while the sliders it governs
                    // are on screen. It writes the setting straight through so
                    // it survives a restart, the pattern the window lock uses.
                    //
                    // ⚠ IT CHANGES NOTHING NOW, ONLY THE NEXT PICK. Toggling it
                    // takes no history step and pushes nothing, because the
                    // layer in front of you already has the transform it has.
                    if (FUCK::Checkbox("$FR_Ovl_KeepOffset"_T,
                                       &Settings::GetSingleton().overlayKeepOffset)) {
                        Settings::GetSingleton().Save();
                    }
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_KeepOffsetTip"_T);
                    }
                    // ⚠ A RESET IS ONE STEP AND ONE PUSH, offered only when there
                    // is something to reset, so the button cannot take a history
                    // step that changes nothing.
                    FUCK::BeginDisabled(!moved);
                    if (ChamferPanel::Button("$FR_Ovl_PositionReset"_T).clicked && moved) {
                        Remember();
                        t = OverlayTransform::Transform{};
                        Push(a_actor, index);
                    }
                    FUCK::EndDisabled();
                    if (FUCK::IsItemHovered()) {
                        FUCK::SetTooltip("$FR_Ovl_PositionResetTip"_T);
                    }
                }
            }

            // ---- the normal map, deferred -----------------------------------
            //
            // ⚠⚠ THE CONTROL IS NOT DRAWN AND THE CODE BEHIND IT IS UNTOUCHED
            // (user 2026-08-16: "for now let's hide the give this layer its own
            // normal map, we will defer custom normal map supported overlays in
            // a future update"). It works: skee bounds-checks the texture slot
            // and passes it straight through, so slot 1 is reachable and six
            // installed presets already ship one. What it lacks is anywhere for
            // a player to GET a normal map, since most packs ship none and the
            // field would be a path to type and a guess to correct.
            //
            // ⚠ READING AND WRITING IT STAYS ON. A layer that already carries a
            // normal, from a preset or from RaceMenu, keeps it: OverlayApi still
            // reads slot 1 into the state and writes it back, so hiding the
            // control cannot quietly strip anything. Only the authoring is
            // gone, and OverlayPlan::GuessNormal is what comes back with it.

            FUCK::Spacing();
            FUCK::Separator();
            DrawTexturePicker(a_actor, index);
        }

        // ---- why the page cannot work, when it cannot ------------------------

        [[nodiscard]] const char* UnavailableKey() {
            switch (OverlayApi::GetStatus()) {
                case OverlayApi::Status::kTooOld:
                    return "$FR_Ovl_TooOld";
                case OverlayApi::Status::kNoOverlay:
                case OverlayApi::Status::kNoOverride:
                    return "$FR_Ovl_Missing";
                case OverlayApi::Status::kOverlaysDisabled:
                    return "$FR_Ovl_Disabled";
                case OverlayApi::Status::kNotRequested:
                case OverlayApi::Status::kNoMessaging:
                case OverlayApi::Status::kRaceMenuAbsent:
                case OverlayApi::Status::kReady:
                    break;
            }
            return "$FR_Ovl_NoRaceMenu";
        }

    }  // namespace

    const char* SelectedOverlayNode() {
        if (!Selected()) {
            return "";
        }
        return Layers()[static_cast<std::size_t>(g.selected)].node.c_str();
    }

    std::string TakeChangedNode() {
        // Taken, not read: the editor arms one flash per report, and a value
        // left behind would arm another on the next frame and every frame after
        // it, which is the "shape left glowing" failure the flash register's
        // own note warns about.
        std::string out;
        out.swap(g.changedNode);
        return out;
    }

    std::string ShotSelectionKey() {
        // ⚠ MAKEUP GETS ITS OWN PREFIX, so moving between a face overlay and a
        // makeup layer is a change of selection rather than two lists producing
        // the same string. That is the same reason this page prefixes at all:
        // two selections that stringify alike leave the shot on whatever was
        // framed before.
        if (MakeupSelected()) {
            return "mk:" + std::to_string(g.makeupSelected);
        }
        // The skin is one selection whatever pack is worn: choosing another
        // pack changes the character, not the shot, and the body is already
        // framed.
        if (SkinSelected()) {
            return "skin";
        }
        if (!Selected()) {
            return "ovl:none";
        }
        // The layer's own index, so moving between two layers of one location
        // re-frames as well. That looks redundant while both give the same
        // node, and it is what brings the shot back after the player has
        // orbited away and clicked another layer.
        return "ovl:" + std::to_string(g.selected);
    }

    const char* ShotNode() {
        // ⚠ EVERY MAKEUP LAYER IS A FACE SHOT, whatever its type. The tint mask
        // system paints the head and nothing else: the retint reaches the head
        // part whose type field is 1 and hands it a rebuilt texture, so even the
        // neck and dirt layers are on that mesh.
        if (MakeupSelected()) {
            return "NPC Head [Head]";
        }
        // A skin is the whole body, so it takes the body overlays' shot: the
        // torso, framed loosely enough to read the arms too.
        if (SkinSelected()) {
            return "NPC Spine2 [Spn2]";
        }
        if (!Selected()) {
            return "";
        }
        // ⚠ THE SAME NODES AND CLOSENESSES THE SLOT ROWS ALREADY USE, taken
        // from ShotFocusForRegion rather than picked afresh. A face overlay and
        // a helmet are the same shot, and two subsystems choosing their own
        // idea of "the head" is how one of them ends up 31 units low, which the
        // note on RequestWholeBodyShot records the cost of.
        switch (Layers()[static_cast<std::size_t>(g.selected)].location) {
            case OverlayPlan::Location::kFace:
                return "NPC Head [Head]";
            case OverlayPlan::Location::kBody:
                return "NPC Spine2 [Spn2]";
            case OverlayPlan::Location::kHands:
                return "NPC R Hand [RHnd]";
            case OverlayPlan::Location::kFeet:
                return "NPC L Foot [Lft ]";
        }
        return "";
    }

    float ShotCloseness() {
        // Tighter than a face overlay, because makeup is the finest work on the
        // page: an eyeliner or a lip colour is judged at a scale where a shot
        // framed for a whole face paint reads as a smudge.
        if (MakeupSelected()) {
            return 0.95f;
        }
        if (SkinSelected()) {
            return 0.3f;  // the body overlays' own closeness, for the same shot
        }
        if (!Selected()) {
            return -1.0f;
        }
        switch (Layers()[static_cast<std::size_t>(g.selected)].location) {
            case OverlayPlan::Location::kFace:
                return 0.85f;
            // ⚠ LOOSER THAN THE TORSO SLOT ROWS, and deliberately. A cuirass is
            // one piece on the chest, but a body overlay is painted across the
            // whole torso and often down the arms, so a shot tight enough to
            // judge a pauldron cuts off half of what was just applied.
            case OverlayPlan::Location::kBody:
                return 0.3f;
            case OverlayPlan::Location::kHands:
                return 0.8f;
            case OverlayPlan::Location::kFeet:
                return 0.75f;
        }
        return -1.0f;
    }

    void OnOpen(RE::Actor* a_actor) {
        g.loaded        = false;
        g.favoritesOnly = false;  // session-local, off each open, as the editor's is
        if (OverlayApi::Available()) {
            EnsureLoaded(a_actor);
        }
    }

    void Draw(RE::Actor* a_actor) {
        if (!OverlayApi::Available()) {
            // ⚠ THE REASON, NOT ONE CATCH-ALL. "RaceMenu is not loaded" sent a
            // playtester hunting a phantom install problem once already, on a
            // list where RaceMenu was loaded and had answered the exchange. The
            // cause that actually happens is another mod's bundled skee64.dll
            // winning the overwrite, and no message about installing RaceMenu
            // could ever point at it.
            OS::ui::TextDisabledWrapped(FUCK::Translate(UnavailableKey()));
            return;
        }
        if (!a_actor) {
            OS::ui::TextDisabledWrapped("$FR_Ovl_NoTarget"_T);
            return;
        }
        EnsureLoaded(a_actor);

        // The same workbench as Shape and Body Studio, down to the helper that
        // sizes it. A page that invents its own arrangement costs the player a
        // second thing to learn for no reason.
        const bool narrow = BodyStudioLayout::UseCompact(
            FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad,
            FUCK::GetContentRegionAvail().x, OS::ui::FontSize());
        // ⚠ THE FOOTER IS RESERVED BEFORE THE PANES ARE SIZED, and the
        // arithmetic is ShapeUI's rather than a fresh guess. A bar drawn after
        // panes that already took the whole height is a bar pushed off the
        // bottom of the page.
        const float footerH = FUCK::GetFrameHeightWithSpacing() +
                              OS::ui::ItemSpacing().y * 2.0f +
                              OS::ui::FramePadding().y * 2.0f +
                              std::max(2.0f, FUCK::GetResolutionScale() * 2.0f);
        const float bodyH = std::max(1.0f, FUCK::GetContentRegionAvail().y - footerH);

        if (narrow) {
            FUCK::BeginChild("ovl_workbench_compact", ImVec2(0, bodyH), true);
            if (g.narrowPicker) {
                DrawLayerEditor(a_actor, true);
            } else {
                DrawLayerList(a_actor, true);
            }
            FUCK::EndChild();
            // In the compact arrangement the one child is whichever half is
            // showing, so the anchor follows it: the list rings as the layer
            // pane, the editor as the texture pane, and the one that is not on
            // screen publishes nothing, which is what keeps its ring away.
            if (g.narrowPicker) {
                Tutorial::PublishAnchor(Tutorial::Anchor::kOverlayTextures,
                                        FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            } else {
                Tutorial::PublishAnchor(Tutorial::Anchor::kOverlayLayers,
                                        FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            }
        } else {
            const float gap    = OS::ui::ItemSpacing().x;
            const float width  = FUCK::GetContentRegionAvail().x;
            const float listW  = BodyStudioLayout::LibraryWidth(width, OS::ui::FontSize());
            FUCK::BeginChild("ovl_layers", ImVec2(listW, bodyH), true);
            DrawLayerList(a_actor, false);
            FUCK::EndChild();
            // The whole left pane. The tutorial's first cards are about the
            // list as a thing, so the child's own rect is the honest ring.
            Tutorial::PublishAnchor(Tutorial::Anchor::kOverlayLayers,
                                    FUCK::GetItemRectMin(), FUCK::GetItemRectMax());
            FUCK::SameLine(0.0f, gap);
            FUCK::BeginChild("ovl_layer", ImVec2(0, bodyH), true);
            DrawLayerEditor(a_actor, false);
            FUCK::EndChild();
        }
        DrawTransactionBar(a_actor);

        // The keyboard's half of the bar above, through the same two functions
        // the buttons call. ⚠ BEFORE FlushMakeup, so an undo taken by chord
        // drops the queued drag exactly as the button's does - Undo clears the
        // pending makeup write for the reason its own note gives, and flushing
        // first would land the undone value straight back.
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

        // ⚠ AFTER THE PANES, so a drag that ended this frame is sent once and
        // with the value the control left behind rather than the one it held
        // when the frame started. See QueueMakeup for why a drag cannot write
        // live: every write here ends in a texture rebuild.
        FlushMakeup(a_actor);
    }

}  // namespace OS::OverlaysUI
