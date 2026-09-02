#include "PCH.h"

#include "EditorWindow.h"

#include "DirectEntry.h"  // the summoned host, dismissed on close

#include "ApparelPreviewSignal.h"  // suspend/resume AP's hover preview
#include "BuildChannel.h"
#include "CameraFrame.h"  // OS-97 point the world camera at the edit target
#include "CameraProbe.h"  // OS-97 temporary field probe - delete with the module
#include "DyeGpu.h"       // OS-139 temporary compute probe - delete with the module
#include "OverlayThumbs.h" // the overlay picker's thumbnail handles
#include "PreviewCache.h" // the grid's drain, pending release and close hook
#include "DyeTexture.h"  // OS-139 rung 3: render-thread pump for the tint cache
#include "OverlayBake.h"  // OS-209: render-thread pump for the overlay bake
#include "EditorGate.h"   // ShouldCloseOnOwnEscape - who owns this Escape
#include "ChamferPanel.h"  // the plate this window paints over FLICK's own
#include "EditorStyle.h"  // PlayUISound
#include "EditorUI.h"
#include "FramePolicy.h"  // carved or plain, and who decides
#include "FsmpProbe.h"    // temporary FSMP-in-FR field probe - delete with the instrument strip
#include "FuckCompat.h"   // ui::StyleColor
#include "IconImages.h"   // the corner art the plate is cut to
#include "HeadPart.h"     // DiscoveredSlots, warmed and logged on the first open
#include "HostGuard.h"    // the one list of menus this window may live over
#include "LoreModule.h"  // the Seamstone gate, asked here as well as at the hotkey
#include "InputListener.h"  // OS-80 ApplyEditorCameraDrag
#include "MenuStudioApi.h"  // the appearance button rides this window's edges
#include "ControllerSupport.h"  // the pad bindings, deferred to a future update
#include "PadFocus.h"       // an inert stub; see the header for what it replaced
#include "SamCompat.h"
#include "Settings.h"
#include "SceneGuard.h"
#include "StyleCatalog.h"
#include "WorldWatch.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h
#include "FUCK_API.h"

#include <imgui.h>  // ImGuiHoveredFlags_AnyWindow (FUCK owns the context; the flag is a pure enum)

#include <algorithm>  // std::clamp
#include <atomic>
#include <cstdint>   // the nav probes' bit masks
#include <iterator>  // std::size over the watched-key table
#include <string>

namespace {



    // Hide/show a menu's 2D content WITHOUT ShowMenus(false). ShowMenus(false) also
    // hides the game cursor that FUCK draws on (Fuzzles: "we rely on the game
    // cursor"), so instead we zero the target menu's Scaleform root alpha - the
    // separate cursor menu and the 3D SPIM character are untouched. AS2 _alpha = 0..100.
    template <class Name>
    void SetMenuRootAlpha(const Name& a_menu, double a_alpha) {
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto menu = ui->GetMenu(a_menu); menu && menu->uiMovie) {
                menu->uiMovie->SetVariable("_root._alpha", RE::GFxValue(a_alpha));
            }
        }
    }

    // ⚠⚠ GRID INVENTORY DRAWS ITSELF, SO THE ALPHA CALL ABOVE CANNOT REACH
    // IT. That menu is customRendering with no uiMovie, so SetMenuRootAlpha is a
    // silent no-op on it and the grid stayed drawn UNDER the editor. Reported
    // again 2026-08-28, with the cursor coming out beneath our window for the
    // same reason: the grid was still live and still owned the pointer.
    //
    // ⛔⛔ 'GISU' ONLY. DO NOT SEND kHide, AND THE AUTHOR SAYING IT IS SAFE IS
    // NOT ENOUGH - THAT WAS TRIED AND THE FIELD REFUSED IT TWICE.
    //
    // The author, 2026-08-28: "Done - kHide now suppresses instead of closing.
    // kHide -> the menu stays on the stack, stops drawing and stops consuming
    // input. IsMenuOpen("GridInventoryMenu") stays true... kForceHide -> actually
    // closes it." On that word this sent kHide on open and kShow on close.
    //
    // ⚠⚠ THAT BEHAVIOUR IS NOT IN THE INSTALLED 1.5.1 BINARY. Field 2026-08-28
    // 20:04, five attempts out of five, GridInventory v1-5-1-0:
    //
    //     20:04:27.591  asked it to stop drawing ('GISU' then kHide)
    //     20:04:27.608  HostGuard: the editor's host menu closed underneath it
    //
    // 17ms, which is the same 18ms the 2026-08-27 attempt measured. The player
    // saw the editor for a split second, then heard and saw the grid re-open
    // (our own kShow on the close path) with the editor gone. Whatever build
    // carries the change, it is not the one on this machine, so the ONLY thing
    // that has ever been proven about kHide here is that it closes the grid.
    //
    // ⛔ AND NEVER kForceHide. On the author's own account that is the message
    // that closes even in the fixed build, so it fails on every build there is.
    //
    // What 'GISU' costs instead: the grid's safety net revokes the suppression
    // a flat 178ms later, every open. The net asks the Skyrim menu stack what is
    // above the grid, and a FLICK overlay is not a menu, so it correctly sees
    // nothing above and restores. That is a visible flicker and a live grid
    // under the editor. It is the lesser failure by a distance - the editor
    // stays open and usable, which under kHide it does not.
    //
    // ⚠ AN UNKNOWN MESSAGE ID IS WHAT MAKES THIS SAFE ON EVERY BUILD. A grid
    // that has never heard of 'GISU' does nothing with it, so the worst case is
    // the behaviour we already had rather than a closed menu and a stranded
    // editor. Nothing here needs to know which build is installed, and any
    // change that does need to know is a change that has to prove it first.
    void SuppressGridInventory(bool a_suppress) {
        // The author's shape, verbatim. structSize and abiVersion are the
        // handshake: the receiver refuses anything it does not recognise, so
        // both have to be exactly what it was told to expect.
        constexpr std::uint32_t kMsgSuppressUI = 0x47495355;  // 'GISU'
        struct SuppressUI {
            std::uint32_t structSize;
            std::uint32_t abiVersion;
            std::uint32_t suppress;
        };
        if (auto* const messaging = SKSE::GetMessagingInterface()) {
            SuppressUI msg{ static_cast<std::uint32_t>(sizeof(SuppressUI)), 1u,
                            a_suppress ? 1u : 0u };
            messaging->Dispatch(kMsgSuppressUI, &msg,
                                static_cast<std::uint32_t>(sizeof(msg)), "GridInventory");
        }
        spdlog::info("Grid Inventory: asked it to {} ('GISU'). An older build "
                     "ignores this and stays drawn.",
                     a_suppress ? "stop drawing" : "come back");

        // ⚠⚠ THE EXPERIMENT, OFF BY DEFAULT, AND IT IS OFF FOR A MEASURED
        // REASON. 'GISU' on 1.5.2 stops the grid DRAWING and does not stop it
        // taking the mouse: the board is invisible and its old area still eats
        // a click-drag, so the camera will not turn there (user, 2026-08-29).
        //
        // kHide is the message the author says stops both: "the menu stays on
        // the stack, stops drawing and stops consuming input". The prohibition
        // above stands on FIVE OUT OF FIVE field attempts against v1-5-1-0,
        // where kHide closed the grid in 17ms and took the editor with it. That
        // is evidence about 1.5.1 and about nothing else, and the rule beside
        // it says a change that depends on the installed build has to prove it
        // first. This is how it gets proven: a switch a player can turn on for
        // one session, with the failure it is watching for written down.
        //
        // ⛔ IF THE EDITOR CLOSES THE INSTANT IT OPENS, THIS IS WHY. Turn it
        // back off. That is the 1.5.1 failure reproducing on a build that was
        // supposed to have fixed it, and the answer is to report it rather than
        // to keep the switch.
        //
        // ⛔ AND NEVER kForceHide, on any build. The author's own account is
        // that it closes the menu even in the fixed build.
        if (!OS::Settings::GetSingleton().gridInventoryHide) {
            return;
        }
        if (auto* const ui = RE::UI::GetSingleton()) {
            if (auto* const q = RE::UIMessageQueue::GetSingleton()) {
                q->AddMessage(OS::HostGuard::kGridInventoryMenu,
                              a_suppress ? RE::UI_MESSAGE_TYPE::kHide
                                         : RE::UI_MESSAGE_TYPE::kShow,
                              nullptr);
                spdlog::warn("Grid Inventory: EXPERIMENT, also sent {} "
                             "(bGridInventoryHide=1). If the editor closes "
                             "immediately, this is the 1.5.1 failure and the "
                             "switch should go back off. Grid open now: {}.",
                             a_suppress ? "kHide" : "kShow",
                             ui->IsMenuOpen(OS::HostGuard::kGridInventoryMenu));
            }
        }
    }

    class EditorIWindow : public FUCK::IWindow {
    public:
        const char* Id() const override { return "OutfitSlotsEditor"; }
        const char* Title() const override {
            static const auto title = OS::BuildChannel::Label();
            return title.c_str();
        }

        bool IsOpen() const override { return open_.load(std::memory_order_relaxed); }
        // Cached in Draw() for GetFlags's passthrough gate; also the camera
        // drag's "did this click land on the world" test (InputListener).
        bool CursorOverUI() const { return cursorOverUI_.load(std::memory_order_relaxed); }
        // SAM framed this shot, so SAM owns the camera (OS-80c). Read from the
        // input thread by InputListener's drag gate and from the render thread
        // by GetFlags's wheel term, which is why the flag is atomic.
        bool OpenedFromSam() const { return openedFromSam_.load(std::memory_order_relaxed); }

        void SetOpen(bool a_open) override {
            const bool was = open_.exchange(a_open, std::memory_order_relaxed);
            if (a_open == was) {
                return;
            }
            // The rules engine's editor gate (Task 11): the editor is a FUCK
            // IWindow, not a game menu, so it fires no MenuOpenCloseEvent of
            // its own - this call is WorldWatch's only signal that it opened
            // or closed. Runs on the main thread already (Toggle/RequestOpen/
            // RequestClose all marshal SetOpen through SKSE::GetTaskInterface).
            OS::WorldWatch::SetEditorOpen(a_open);
            if (a_open) {
                // open_ is already true (exchange above) so FUCK will start
                // calling Draw on the PRESENT thread immediately - but OnOpen
                // (which populates g_target / g_targetLibrary, a std::string +
                // std::optional + OutfitLibrary) has not run yet. Gate Draw on
                // ready_ so it early-outs until OnOpen completes, instead of
                // reading half-written editor state. Cleared here, set true
                // right after OnOpen; a scene-refuse below leaves it false.
                ready_.store(false, std::memory_order_relaxed);
                // Every open gets a fresh attempt and, if it throws again, one
                // more log line. A player who fixes the file that threw should
                // not have to restart the game to find out.
                drawFaulted_.store(false, std::memory_order_relaxed);
                // Refuse during a scene (OStim etc.): transmog is suspended and
                // the editor would hijack the scene camera/input.
                if (OS::SceneGuard::Active()) {
                    open_.store(false, std::memory_order_relaxed);
                    RE::DebugNotification("You can't edit outfits during a scene.");
                    return;
                }
                forceLayout_.store(true, std::memory_order_relaxed);  // OS-54: re-apply standardized geometry on the first Draw
                // ⚠ THE HOTKEY THAT OPENED US IS STILL DOWN ON THE FIRST DRAW.
                // Draw reads the editor hotkey off FUCK's key state and closes
                // on its press EDGE; seeding "was down" here means the press
                // that opened the editor cannot be the press that closes it,
                // and the first close needs a release in between. Render-thread
                // owned after this, written here on the main thread before the
                // first Draw can run (ready_ is still false).
                hotkeyWasDown_ = true;
                // Re-evaluate style fit if the race changed (RaceMenu); must run
                // before FUCK's Present thread draws rows (fitsBody read unsynced).
                OS::StyleCatalog::GetSingleton().EnsureFitCurrent();
                // Head-part slots this load order invented (horns, ears). The
                // walk runs once for the process and caches, so this is a
                // no-op on every open after the first; it sits here rather than
                // at load so a player who never opens the editor never pays for
                // it. The log line it prints on that first open is how a "my
                // horns are not listed" report gets diagnosed, because it says
                // whether the slot was found at all.
                (void)OS::HeadPart::DiscoveredSlots();
                OS::EditorStyle::PlayUISound("UIMenuOK");
                // ⚠⚠ ASKED ONCE, RECORDED, AND NEVER ASKED AGAIN THIS SESSION.
                // Both facts below come out of the same answer: which menu was
                // hidden, and whether that menu is SAM. Deriving them separately
                // is how a mark and its eraser come to disagree - the close used
                // to re-derive the host from a bool and hand the alpha back to
                // InventoryMenu whatever had actually been hidden, which with
                // only two possible hosts happened to be right and with three
                // would leave the real host invisible for the rest of the
                // session.
                const auto host    = OS::HostGuard::CurrentHost();
                const bool fromSam = host.isSam;
                hiddenHost_        = host.name;
                openedFromSam_.store(fromSam, std::memory_order_relaxed);
                // ⚠ RE-ARMED HERE, NOT ONLY AT kDataLoaded. MenuControls asks
                // the LAST registered handler first, and SAM registers its own
                // when its menu opens, so a guard added once at load spends the
                // whole session behind SAM. See SamCompat::ArmEscapeGuard.
                if (fromSam) {
                    OS::SamCompat::ArmEscapeGuard();
                }
                // NPC-target seam (spec §5, moved here from the legacy overlay's
                // ImGuiOverlay::OnOpen). The editor's "Editing:" selector now
                // threads a per-actor target through staging/fit/Apply (Task 8),
                // but the SAM entry point still opens on the PLAYER: EditorUI::
                // OnOpen resets g_target to the player every open. A future stage
                // would, when openedFromSam_, read SamCompat's selected refr here
                // and PRESELECT that NPC as the target (the SAM-framed camera makes
                // the follower's live updates genuinely visible). Deferred - this
                // is the single documented handoff point for SAM target preselect.
                // Apparel Preview: clear the hover preview AND stay suspended
                // for the editor's lifetime. The one-shot legacy clear was not
                // enough: the alpha-hidden inventory still hit-tests, so item
                // highlights kept moving under the editor and AP previewed
                // each one over the staged look. See ApparelPreviewSignal.h.
                // Take the vanilla-stage claim first. Apparel Preview clears
                // its own claim when it receives the suspension message below;
                // this ordering keeps at least one owner live across the handoff.
                if (!fromSam) {
                    menuStudioItemPreviewClaimHeld_ =
                        OS::MenuStudioApi::SetItemPreviewSuppressed(true);
                }
                OS::NotifyApparelPreview(true);
                // In SAM the shot is already framed - leave the camera alone.
                if (!fromSam) {
                    if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                        wasFirstPerson_ = cam->IsInFirstPerson();
                        cam->ForceThirdPerson();
                    }
                }
                // Hide the 2D UI of the menu we opened over so only the world + the
                // SPIM character show behind the editor. NOT ShowMenus(false): that
                // also hides the game cursor FUCK relies on (Fuzzles) - instead we zero
                // the menu's Scaleform root alpha, leaving the cursor menu and the 3D
                // character intact. kRenderDuringTM keeps our window drawing.
                //
                // ⚠ BY THE RECORDED NAME, NOT BY A BRANCH. The close below reads
                // the same field, so whatever gets hidden here is what gets
                // handed back, and a fourth host would need no edit on either
                // side. Empty means no host was up, which the open gates make
                // unreachable - the alpha call would be a silent no-op anyway,
                // since GetMenu on a closed menu returns null.
                if (!hiddenHost_.empty()) {
                    SetMenuRootAlpha(hiddenHost_, 0.0);
                }
                // ⚠ ONLY WHEN THE GRID IS THE HOST. Suppressing it from under
                // an inventory or magic-menu open would be asking a menu that is
                // not on screen to hide, and the restore below would then be a
                // show nobody asked for.
                if (hiddenHost_ == OS::HostGuard::kGridInventoryMenu) {
                    SuppressGridInventory(true);
                }
                // ⚠⚠ THE ALPHA CALL ABOVE DOES NOTHING TO THE GRID, WHICH IS
                // WHY THE LINE ABOVE THIS ONE EXISTS. That menu renders itself
                // (customRendering=true, no uiMovie), so SetMenuRootAlpha is a
                // silent no-op on it and the grid stayed drawn under the editor
                // until the author gave us a lever that reaches it.
                //
                // ⛔ THE ONLY THING THAT GOES OUT IS 'GISU'. Not kHide, not
                // kForceHide; the field history for both lives at
                // SuppressGridInventory's definition rather than here, so there
                // is one account of it and not two. Read it before sending this
                // menu anything new.
                //
                // ⚠ WHAT A MESSAGE THE GRID CLOSES ON COSTS US. IsMenuOpen goes
                // false, and HostGuard closes the editor because its host went -
                // measured at 17ms and 18ms on two separate days. That leaves
                // the player holding the grid with no editor, and the only
                // alternative to closing would be an editor up with NO menu
                // under it, which is precisely the failure HostGuard exists to
                // prevent: Escape then reaches the game, the editor never sees
                // SetOpen(false), and CameraFrame::Tick keeps re-asserting the
                // retarget while the player has gameplay control.
                // The 2D alpha-hide leaves the floating 3D item preview (the
                // rotating model) visible; clear it so only the SPIM character
                // shows behind the editor.
                //
                // ⚠ THE SAM TERM AND NOT AN INVENTORY TERM, and it has to stay
                // that way. Inventory3DHooks::ShouldHide gates the appender
                // detour on exactly this question and its own ⚠ says why: two
                // hiders of one preview that disagree about WHEN leave a window
                // where neither one covers it. The magic menu drives the same
                // manager - UpdateMagic3D is one of the appender's two call
                // sites - so it belongs on the hiding side with the inventory
                // rather than beside SAM.
                if (!fromSam) {
                    if (auto* inv3d = RE::Inventory3DManager::GetSingleton()) {
                        inv3d->Clear3D();
                    }
                }
                // ⚠⚠ THE POPULATION STEP IS THE OTHER HALF OF THE DRAW GUARD,
                // AND IT IS THE HALF THE REPORT POINTS AT. OnOpen reads the
                // outfit library, the target's name and the files behind them:
                // names in the game's own codepage and JSON somebody may have
                // edited by hand. A throw here reaches terminate, which is a
                // fail-fast, which no crash logger can see, so the player gets
                // a silent close the moment they open the editor and nothing to
                // send us. Refuse the open and say why instead.
                try {
                    OS::EditorUI::OnOpen();
                } catch (const std::exception& e) {
                    spdlog::critical(
                        "EditorWindow: populating the editor threw, so the open "
                        "is refused rather than allowed to end the process as a "
                        "fail-fast with no crash log. The exception was: {}",
                        e.what() ? e.what() : "a std::exception with no message");
                    if (const auto logger = spdlog::default_logger()) {
                        logger->flush();
                    }
                    // ready_ stays false, so no frame can read the half-written
                    // state this left behind. ⚠ THE TEARDOWN NEEDS ITS OWN
                    // GUARD: it is re-entering SetOpen from inside SetOpen, and
                    // it runs OnClose against state OnOpen only half built, so
                    // it is exactly the place a second throw would come from.
                    // One escaping here would undo the whole point of the first
                    // catch.
                    try {
                        SetOpen(false);
                    } catch (...) {
                        open_.store(false, std::memory_order_relaxed);
                        spdlog::critical("EditorWindow: the teardown after that "
                                         "threw as well; the window is marked "
                                         "closed and nothing further is run.");
                    }
                    return;
                } catch (...) {
                    spdlog::critical(
                        "EditorWindow: populating the editor threw something "
                        "that does not derive from std::exception, so the open "
                        "is refused.");
                    if (const auto logger = spdlog::default_logger()) {
                        logger->flush();
                    }
                    try {
                        SetOpen(false);
                    } catch (...) {
                        open_.store(false, std::memory_order_relaxed);
                    }
                    return;
                }
                ready_.store(true, std::memory_order_release);  // editor state populated - Draw may now render
                // OS-97: arm AFTER OnOpen, so the frame count starts from a
                // populated g_target and sample #1 can already name the target.
                OS::CameraProbe::Arm();
                FUCK::ForceCursor(true);  // belt-and-suspenders; the game cursor stays now
                // Menu Studio's 'Change your appearance' button is born
                // hidden on its strip and exists exactly while this window
                // does (the author's call: appearance work belongs to the
                // styling context, not to a bare inventory). AFTER the
                // scene-refuse above, so a refused open reveals nothing.
                //
                // ⚠ ALWAYS VISIBLE, AND REFUSED RATHER THAN HIDDEN WHEN THE
                // DOOR FEE IS OUT OF REACH. It hid itself for one build and the
                // field verdict was that the tile should stay and read as
                // locked (user 2026-08-07): a button that vanishes tells the
                // player nothing, and worse, it vanished only at editor-open
                // time, so spending the last of the charge INSIDE the editor
                // left it sitting there fully live. EditorUI publishes the
                // enablement and re-publishes it whenever the answer moves.
                OS::MenuStudioApi::SetAppearanceButtonVisible(true);
                // The same edge, the same boundary, one line further: Menu
                // Studio's wake gate holds its bubble down over a plain
                // inventory and lifts it when an owner says its own window is
                // open. This window is that owner, and this call is the only
                // way it can be known - see the IWindow note at the top of
                // this function for why no MenuOpenCloseEvent carries it.
                // Inert on a Menu Studio without the gate, and inert on one
                // whose player left the setting off.
                OS::MenuStudioApi::SetOwnerContext(true);
                OS::EditorUI::PublishLooksMenuAffordability(true);
                // After the scene-refuse above, so a refused open never probes.
                OS::FsmpProbe::OnEditorToggle(true);
            } else {
                OS::FsmpProbe::OnEditorToggle(false);
                // ⚠ THE DRAGGED WIDTH IS COMMITTED HERE, NOT AS IT MOVES. Draw
                // runs on the PRESENT thread and writes the live value every
                // frame; an INI write per frame of a drag would be absurd, and
                // this close path is the main thread.
                OS::Settings::GetSingleton().Save();
                ready_.store(false, std::memory_order_release);  // stop Draw rendering before OnClose tears state down
                OS::CameraProbe::Disarm();  // OS-97: before OnClose clears g_target
                // Backstop. EditorUI::OnClose releases the camera too, but a
                // world camera left pointed at a follower is the worst failure
                // this feature can produce - the player would walk around with
                // the shot on someone else - so the release also happens on the
                // outer close path, where nothing can early-return past it.
                // Release is idempotent and no-ops when nothing was retargeted.
                OS::CameraFrame::Release();
                FUCK::ForceCursor(false);
                OS::EditorStyle::PlayUISound("UIMenuCancel");
                // Apparel Preview may resume its hover preview - the editor no
                // longer owns the look. Its own menu-close handling backstops
                // this if we never get here (see ApparelPreviewSignal.h).
                OS::NotifyApparelPreview(false);
                // The styling context is going away, so its button goes too.
                OS::MenuStudioApi::SetAppearanceButtonVisible(false);
                // ...and the studio with it, back down to a vanilla inventory
                // for as long as this window stays shut. Withdrawn on every
                // close rather than only on the last one, because Menu Studio
                // clears its owner set when the covered menu closes and this
                // side republishes on the next open anyway.
                //
                // ⚠ POINTER, NOT MEASURED. CameraProbe::Disarm and
                // CameraFrame::Release above already ran, so the withdrawal
                // lands after this window's own camera teardown. If a field run
                // shows the world unfreezing or the lights dropping while the
                // editor is still tearing down, moving this to the END of this
                // branch is the first thing to try. Measure before reordering.
                OS::MenuStudioApi::SetOwnerContext(false);
                OS::EditorUI::OnClose();
                // The preview cache flushes its manifest and reports its
                // harness with the editor it only ever runs inside.
                OS::PreviewCache::OnEditorClose();
                // Restore the 2D of the menu we alpha-hid on open - THE ONE WE
                // HID, read back off the record rather than worked out again
                // from the current stack. By the time this runs the host has
                // often already gone (HostGuard closes the editor because the
                // menu closed underneath it), so there is nothing left to ask;
                // GetMenu returns null there and the call is a no-op, which is
                // correct for a movie that is being torn down anyway.
                const bool wasFromSam = openedFromSam_.load(std::memory_order_relaxed);
                if (!hiddenHost_.empty()) {
                    SetMenuRootAlpha(hiddenHost_, 100.0);
                    // ⚠ BEFORE THE CLEAR, because the clear is what forgets
                    // which host this was. The author asks for the show to be
                    // sent rather than left to their safety net, which costs a
                    // fraction of a second before the grid comes back.
                    if (hiddenHost_ == OS::HostGuard::kGridInventoryMenu) {
                        SuppressGridInventory(false);
                    }
                    hiddenHost_.clear();
                }
                if (auto* cam = RE::PlayerCamera::GetSingleton();
                    cam && wasFirstPerson_ && !wasFromSam) {
                    cam->ForceFirstPerson();
                }
                // Release from the recorded debt, never by re-deriving the
                // current SAM state or export availability. A SAM open never
                // acquired this claim, so it has nothing to give back here.
                if (menuStudioItemPreviewClaimHeld_) {
                    (void)OS::MenuStudioApi::SetItemPreviewSuppressed(false);
                    menuStudioItemPreviewClaimHeld_ = false;
                }
                // ⚠ LAST, AND ONLY FOR A HOST THIS MOD SUMMONED. The
                // direct-entry key opens the inventory to have something to be
                // hosted by, so closing the editor has to take it away again or
                // the key is a one-way door into a menu the player never asked
                // for (their call, 2026-08-18). A menu they opened themselves is
                // untouched: DirectEntry knows which it was and nothing here
                // re-derives it. After the alpha restore above, so the menu is
                // handed back intact before it is dismissed.
                OS::DirectEntry::OnEditorClosed();
            }
        }

        FUCK::WindowFlags GetFlags() const override {
            // A solid panel (NO kNoBackground - that left everything except the two
            // child panels transparent). The SPIM character sits to the right,
            // outside the window. No vanity drift; render over the open game menu
            // (kRenderDuringTM) while the shim alpha-hides the inventory's 2D (NOT
            // ShowMenus(false), which also hid the game cursor FUCK relies on). NOT
            // kCloseOnGameMenu - we open BECAUSE a game menu is up. kIgnoreUserScale
            // opts the editor out of FUCK's global user content-scale (per Fuzzles), so
            // the widgets aren't inflated past vanilla ImGui; density is instead our own
            // "UI size" slider (SetWindowFontScale in EditorUI).
            //
            // NO kCloseOnEsc: it also maps controller B/Circle to close, which nuked the
            // whole editor instead of doing panel back-nav (per Fuzzles, "close on esc
            // covers back too"). Draw() handles Start + Esc close instead, leaving B for
            // FUCK's default back-nav. Whether dropping the flag fully frees B is
            // empirical - verify in-game.
            FUCK::WindowFlags flags = FUCK::WindowFlags::kBlockVanity |
                                      FUCK::WindowFlags::kRenderDuringTM |
                                      FUCK::WindowFlags::kIgnoreUserScale;
            // Standardized layout by default (user): lock position, size and chrome so
            // the editor always opens the same. kNoDecoration also drops the FUCK title
            // bar so "Outfit Slots" isn't shown twice (title bar + our own header). The
            // gear's "Lock window" toggle clears these for users who want to move/resize.
            //
            // ⚠⚠ UNLOCKING GIVES UP THE RIGHT EDGE AND NOTHING ELSE. It used to
            // hand over position and both axes, and neither was worth having:
            // the panel is flush to the left edge and the full height of the
            // display, so dragging the bottom only lifted the footer off the
            // screen and dragging it sideways left a gap down the side. The
            // field asked for the right edge alone (2026-08-10). kNoMove and
            // kCustomPosition therefore stay on in BOTH states, which is also
            // what makes the left and bottom borders inert: ImGui resizes them
            // by moving the origin, and an origin re-asserted every frame puts
            // it straight back.
            //
            // ⚠⚠ IMGUI'S OWN RESIZE IS THE ONLY THING THAT MOVES THIS WINDOW,
            // MEASURED AFTER THREE BUILDS ASSUMED OTHERWISE. FUCK owns the
            // size and re-asserts it, so FUCK::SetWindowSize called from
            // inside Draw does nothing once kNoResize is set: a hand-rolled
            // grip tracked the cursor perfectly and wrote a width every frame
            // while the window sat frozen at 470 (field 2026-08-10, the
            // width-grip line). GetDefaultSize is no help either, being asked
            // only when the window first appears, which is where that 470 came
            // from. FUCK exports no size-constraint call and no way to set its
            // stored size, and reaching for ImGui:: directly binds to OUR
            // context rather than FUCK's, which is a CTD already paid for.
            //
            // So unlocking hands the resize back to ImGui and the height is
            // pinned in Draw instead. ⚠ THE RIGHT BORDER ALONE ALREADY MOVES
            // ONLY THE WIDTH: ImGui changes one axis per border and both only
            // at a corner, so the vertical wobble is the bottom-right GRIP
            // being grabbed rather than the edge.
            flags = flags | FUCK::WindowFlags::kNoMove |
                    FUCK::WindowFlags::kNoDecoration |
                    FUCK::WindowFlags::kCustomPosition;
            if (OS::Settings::GetSingleton().lockLayout) {
                flags = flags | FUCK::WindowFlags::kNoResize;
            }
            // Character rotation, Fuzzles' RaceMenu-Enhancer pattern (TIGHTENED after a
            // field round). kPassInputToGame is "allows player control while open", so we
            // hand it to the game ONLY while the cursor is over the character (outside
            // every FUCK window, cached in Draw) AND a mouse button is held - i.e. an
            // actual rotate-drag. The first field build gated on hover alone, which (a)
            // let the game hide the cursor the whole time the pointer sat over the
            // character half and (b) leaked Esc to the game (closing the inventory but not
            // the editor). Requiring a held button confines passthrough to the drag: the
            // cursor stays visible while hovering, and Esc closes the editor normally.
            // IsMouseDown is read fresh here (not cached) so the button-down edge is not
            // lost to a frame of latency and rotation still starts cleanly. Passthrough is
            // gated at all because the UNCONDITIONAL flag (ba7fbbb) CTD'd the Papyrus VM
            // over a live InventoryMenu (execute-at-0x0 in SKI_PlayerLoadGameAlias,
            // crash-2026-07-14-01-17-30).
            // Controller (OS-53, user re-ask): ALSO pass through while the RIGHT STICK is
            // deflected - the vanilla InventoryMenu's rotate input - so the stick rotates
            // the character instead of only scrolling the FUCK list. Confined to the
            // gesture the same way the mouse path is (stick centred / button up =>
            // passthrough off), which is what made the mouse case safe vs the CTD-prone
            // unconditional flag. EXPERIMENTAL: if FUCK owns the right stick for scroll they
            // may fight, or the alpha-hidden menu may not rotate - field-test; a clean
            // "give me the right stick" is likely a Fuzzles ask.
            // OS-80d: UNDER SAM THE GATE IS HOVER-ONLY, NOT BUTTON-HELD, and that is
            // the whole fix. The button-held form above is SELF-DEFEATING for another
            // mod's camera: passthrough cannot open until a button is ALREADY down, but
            // the button-DOWN event is what the closed gate blocks. FLICK's hook
            // (Hooks::ProcessInputQueue, FUCK.dll RVA 0xaa880) is all-or-nothing per
            // poll - passthrough on calls the original with the queue untouched, off
            // walks and filters it - so the press is consumed and only later MOVES get
            // through. SAM never sees a drag begin, so it never tracks one.
            //
            // That is exactly the field report, across every round: "sometimes it did
            // move if i held both mouse buttons down but it was janky". The FIRST
            // button's press is always eaten; pressing a SECOND while the gate is
            // already open lets that one through, so the drag half-starts. Two-button
            // partial success was never odd - it was the symptom naming its own cause.
            //
            // Hovering the world strip is therefore the gesture we gate on when SAM
            // owns the camera: the gate is open BEFORE the press, so SAM receives
            // down, move and up as one coherent gesture. This also subsumes the wheel
            // (no latch needed - the gate is already open when the tick arrives), which
            // is why OS-80c's wheelActive_ machinery is gone again.
            //
            // Kept button-held for the NO-SAM case, where the tightening still earns
            // its keep (hover-gated passthrough there hid the cursor over the character
            // and leaked Esc to the inventory) and where we drive the camera ourselves
            // anyway. Under SAM, Esc reaches the game while hovering. SAM's
            // MenuOpenCloseEvent sink closes this hosted editor if SAM consumes
            // that Escape first, so the two windows cannot become detached.
            const bool overUI      = cursorOverUI_.load(std::memory_order_relaxed);
            const bool mouseRotate = !overUI && (FUCK::IsMouseDown(0) || FUCK::IsMouseDown(1));
            const bool samCamera   = !overUI && openedFromSam_.load(std::memory_order_relaxed);
            if (mouseRotate || samCamera || rStickActive_.load(std::memory_order_relaxed)) {
                flags = flags | FUCK::WindowFlags::kPassInputToGame;
            }
            return flags;
        }

        // Narrow enough to sit beside SkyUI, never so thin the content cannot
        // lay out. Scaled by the resolution like every other bound here.
        static constexpr float kMinWidth = 260.0f;

        ImVec2 GetDefaultSize() const override {
            const ImVec2 d = FUCK::GetDisplaySize();
            // ⚠ THE CLAMP IS IN READABLE PIXELS, NOT RAW ONES, AND THAT IS THE
            // WHOLE 4K BUG. Written as a flat 840..1230 it was tuned at 1080p,
            // where 60% of the width lands inside the range and the clamp never
            // bites. It bites at everything larger: 1440p and 4K both collapse
            // to exactly 1230, while the height below takes the FULL display.
            // So the window went from 1.07 wide-to-tall at 1080p to 0.57 at
            // 3840x2160 - a tall thin column on the widest screen anyone owns,
            // reported as "very thin UI" and as panes not showing up, because a
            // page laid out in two halves has nowhere to put the second one.
            //
            // Content does not shrink to match: GetResolutionScale is 2.0 at 4K
            // against 1.0 at 1080p, so everything in that column is drawn twice
            // the size inside the same 1230 pixels it had two resolutions ago.
            // Scaling the bounds by the same factor is what keeps the clamp
            // meaning "between about six and nine inches of text" at every
            // resolution instead of "between 840 and 1230 dots".
            //
            // ⚠ 1080p IS BYTE-FOR-BYTE UNCHANGED, which is the point: the scale
            // is 1.0 there, so the bounds are the old numbers and the window is
            // the same 1152 it always was. Only the resolutions where the clamp
            // was already overriding the 0.60 intent move.
            const float  s = std::max(1.0f, FUCK::GetResolutionScale());
            // ⚠ 0.466 IS SkyUI'S RIGHT EDGE, MEASURED RATHER THAN CHOSEN. The
            // ask was for the panel to line up with SkyUI's border instead of
            // ending somewhere near it, and that number is not derivable from
            // here: SkyUI is a Scaleform movie and the plugin cannot query its
            // layout. So it was measured the only honest way available, by
            // dragging the right edge onto SkyUI's border and reading the
            // width back out of the INI: 1192 on a 2560 wide display
            // (2026-08-10). A FRACTION rather than those pixels, because
            // Scaleform lays SkyUI out against the stage and a saved 1192
            // would land somewhere else on every other monitor.
            //
            // It sits just above the old lower clamp at every resolution, so
            // the narrow-layout floor still bounds it and nothing collapses.
            const float  w = std::clamp(d.x * 0.466f, 840.0f * s, 1230.0f * s);
            // Full display height keeps the host frame itself outside the visible
            // edge. EditorUI reserves its own bottom breathing room, so footer
            // controls no longer rely on exposing that harsh outer border.
            const float h = d.y > 96.0f ? d.y : 720.0f;
            // ⚠⚠ THE DRAGGED WIDTH BELONGS HERE, NOT ONLY IN Draw, and this is
            // what two dead builds of the grip were actually failing on.
            // kCustomPosition makes FUCK ask this override every frame and
            // apply what it says, so a width written inside Draw was stamped
            // over by the default on the very next frame. The instrument said
            // it outright: the stored width read 2098 while the window sat at
            // exactly 1192, which is this function's own computed default
            // (field 2026-08-10). Before the flag went unconditional an
            // unlocked window did not carry it, which is why dragging used to
            // stick and stopped when the flag did.
            const auto& cfg = OS::Settings::GetSingleton();
            if (!cfg.lockLayout && cfg.uiWidth > 0.0f) {
                return ImVec2(std::clamp(cfg.uiWidth, kMinWidth * s, d.x), h);
            }
            return ImVec2(w, h);
        }
        // Flush to the display edge. The content has its own internal padding.
        //
        // OS-80e: under SAM, anchor RIGHT instead. SAM's own UI lives on the right and
        // we alpha-hide it - but HIDDEN IS NOT GONE. Its region still hit-tests, so it
        // eats camera drags exactly like a visible panel would, and with our panel on
        // the LEFT the only draggable area left was a thin strip between the two dead
        // zones (user: "the only way to move is by putting cursor in the middle which
        // isn't ideal"). Parking our panel ON TOP of SAM's UI merges the two dead zones
        // into one and leaves the whole left side contiguous and free for the camera.
        // Size is unchanged, so the free area is whatever our 60%-clamped width leaves.
        ImVec2 GetDefaultPos() const override {
            if (openedFromSam_.load(std::memory_order_relaxed)) {
                const ImVec2 d = FUCK::GetDisplaySize();
                const float  w = GetDefaultSize().x;
                return ImVec2(d.x > w ? d.x - w : 0.0f, 0.0f);
            }
            return ImVec2(0.0f, 0.0f);
        }

        // ⚠ THIS EXISTS BECAUSE Draw IS NOT ENOUGH, AND THE FIELD PROVED IT.
        // FUCK gates Draw on IsOpen(), so OS-139's texture pump only ran while
        // the editor was up. DyeTexture.h asserted that was harmless on the
        // reasoning that a colour can only be CHOSEN with the editor open. That
        // reasoning was wrong: the cache lives in memory and the SAVE holds the
        // colour, so loading a game makes every stored colour a first-time
        // colour again with the editor shut. The 2026-08-05 run hit it exactly
        // that way - dyed armour came back undyed and only appeared after the
        // dye pane was opened and a colour re-picked.
        //
        // ⚠ WHETHER FUCK CALLS THIS WITH THE WINDOW CLOSED IS THE OPEN QUESTION,
        // and the log line below is how the next run answers it rather than
        // this comment asserting it. If it turns out to be gated on IsOpen too,
        // the pump needs a Present hook of its own and that is a bigger change
        // with the chained-hook hazards that come with one.
        void RenderOverlay() override {
            OS::DyeTexture::Pump();
            // OS-209: the overlay bake shares the render thread and the reason
            // for being here. Its commits arrive from OverlayApi::Write, which a
            // save load can drive with the editor shut, so it must not be gated
            // on the window either.
            OS::OverlayBake::Pump();
            static std::atomic<bool> s_said{ false };
            if (!open_.load(std::memory_order_relaxed) &&
                !s_said.exchange(true, std::memory_order_relaxed)) {
                spdlog::info("DyeTexture: RenderOverlay runs with the editor CLOSED, so the "
                             "pump is reachable without it.");
            }
        }

        // ⚠⚠ AN EXCEPTION OUT OF THE EDITOR USED TO KILL THE GAME WITH NO CRASH
        // LOG, AND THAT IS THE WHOLE REASON THIS WRAPPER EXISTS. An uncaught
        // throw reaches std::terminate, terminate is a CRT fail-fast
        // (0xC0000409), and a fail-fast goes to Windows Error Reporting WITHOUT
        // passing through the unhandled-exception filter that Crash Logger and
        // Trainwreck hook. The player gets a silent close and hands us nothing.
        // Reported 2026-08-29 as "a weird crash that doesn't generate any crash
        // logs whenever I try to access the fitting room from my inventory".
        //
        // ⚠ THE EDITOR IS THE RIGHT PLACE FOR THIS AND THE REST OF THE MOD IS
        // NOT. Everything drawn here is text and files the player owns and we
        // do not: item and mod names in whatever codepage the game was built
        // for, folder and preset names somebody typed in their own language,
        // hand-edited JSON. Any of it can throw somewhere none of our tests
        // reach, because no test compiles the presentation layer. Elsewhere a
        // throw is a bug that should be loud.
        //
        // ⚠ THIS IS CONTAINMENT, NOT A DIAGNOSIS, and the log line is the point
        // of it as much as the survival is. It converts a silent process kill
        // into a named exception in FittingRoom.log and an editor that refuses
        // to be open, which is the difference between an unanswerable report
        // and a fixable one.
        void Draw() override {
            if (drawFaulted_.load(std::memory_order_relaxed)) {
                return;
            }
            try {
                DrawContents();
            } catch (const std::exception& e) {
                OnDrawFault(e.what() ? e.what() : "a std::exception with no message");
            } catch (...) {
                OnDrawFault("an exception that does not derive from std::exception");
            }
        }

        // Latch, say so, and take the editor down without taking the game with
        // it. Close is queued rather than immediate: this runs on the present
        // thread and SetOpen touches RE::UI and the camera.
        void OnDrawFault(const char* a_what) {
            drawFaulted_.store(true, std::memory_order_relaxed);
            spdlog::critical(
                "EditorWindow: drawing the editor threw and the editor is "
                "closing itself rather than letting the exception reach "
                "terminate, which would end the process as a fail-fast with no "
                "crash log. The exception was: {}",
                a_what);
            if (const auto logger = spdlog::default_logger()) {
                logger->flush();
            }
            // ⚠ QUEUED, NOT CALLED. This runs on the present thread and SetOpen
            // touches RE::UI and the camera, which is the same main-thread rule
            // every other close on this window obeys.
            if (auto* const task = SKSE::GetTaskInterface()) {
                task->AddTask([this] { SetOpen(false); });
            }
        }

        void DrawContents() {
            // OS-54: force our standardized geometry on the first frame after open (and
            // on any FUCK re-appear). CONFIRMED cause: FUCK persists per-window geometry
            // (x/y/w/h) keyed by Id() in ...\overwrite\FUCKs\FUCK\tools\Outfit Slots.json
            // and RESTORES it on cold boot, so our GetDefaultSize/Pos (only honoured when
            // the window first initialises with no saved entry) are ignored - the saved
            // h/y seated the footer (Apply/Close) off the bottom. kCustomPosition did not
            // spare us (x/y are saved regardless). The gear's "Lock window" toggle only
            // fixed it because flipping the flags made FUCK re-apply the defaults; do that
            // ourselves every open. FUCK applies saved geometry once-on-appear (the toggle
            // fix STICKS for the session), so this in-Draw SetWindowSize(...,Always) wins
            // and holds. Set BEFORE drawing content so the layout uses the corrected size
            // (no open-frame flicker). Gated on lockLayout so an unlocked user's manual
            // size/pos is preserved. cond=0 == ImGuiCond_Always. The forceLayout_ latch
            // (set in SetOpen) makes the first post-open frame deterministic even if
            // IsWindowAppearing does not fire on the hosted window.
            // The gear's "Reset window position" fires resetGeometry_ - unconditional of
            // lockLayout, so it also snaps an unlocked/moved window back to the default.
            // (The preview cache's release and drain moved OUT of Draw and
            // into DyeTexture's Present thunk - PumpPreviews below - after
            // field round 3: D3D and FUCK image traffic inside FUCK's own UI
            // pass hid the whole editor on exactly the loading frames.)
            const bool resetGeom = resetGeometry_.exchange(false, std::memory_order_relaxed);
            auto&      cfg     = OS::Settings::GetSingleton();
            const bool fresh   = forceLayout_.exchange(false, std::memory_order_relaxed) ||
                               FUCK::IsWindowAppearing();
            // ⚠⚠ THE STORED WIDTH IS FORGOTTEN BEFORE THE DEFAULT IS ASKED FOR,
            // AND THE ORDER IS THE WHOLE BUTTON. GetDefaultSize answers with
            // the dragged width when there is one, so clearing it afterwards
            // left `def` holding the very width the reset was meant to undo
            // and the window was dutifully set to the size it already was.
            // Reset did nothing at all, with no error anywhere (field
            // 2026-08-10).
            if (resetGeom) {
                cfg.uiWidth = 0.0f;
            }
            const ImVec2 def = GetDefaultSize();
            if (resetGeom || (cfg.lockLayout && fresh)) {
                FUCK::SetWindowSize(def);
                FUCK::SetWindowPos(GetDefaultPos());
            } else if (!cfg.lockLayout) {
                // Keep the width ImGui's resize just produced and put the
                // height back. ⚠ THIS IS A CORRECTION, NOT OWNERSHIP: it
                // lands after Begin has already laid the frame out at the
                // dragged size, so a corner drag shows the height move and
                // snap back on release. The right BORDER moves only the
                // width and looks clean; see the flags above for why there
                // is no way to forbid the corner from here.
                const ImVec2 cur = FUCK::GetWindowSize();
                const float  s   = std::max(1.0f, FUCK::GetResolutionScale());
                float        w   = cur.x > 1.0f ? cur.x : def.x;
                if (fresh && cfg.uiWidth > 0.0f) {
                    w = cfg.uiWidth;
                }
                w = std::clamp(w, kMinWidth * s, FUCK::GetDisplaySize().x);
                FUCK::SetWindowSize(ImVec2(w, def.y));
                // ⚠⚠ THE WIDTH IS STORED BEFORE THE POSITION IS ASKED FOR, AND
                // UNDER SAM THAT ORDER IS THE WHOLE BEHAVIOUR. SAM anchors this
                // window to the RIGHT edge of the display, so GetDefaultPos
                // subtracts the width from it, and GetDefaultSize answers with
                // this very value. Storing it afterwards positioned the frame
                // with the PREVIOUS width, so every drag placed the window one
                // frame behind its own size and the right edge crawled instead
                // of staying flush.
                cfg.uiWidth = w;
                FUCK::SetWindowPos(GetDefaultPos());
            }

            // Draw the editor content only once OnOpen has populated g_target /
            // g_targetLibrary (ready_). FUCK gates Draw on IsOpen(), which flips
            // true BEFORE OnOpen runs on the main thread, so without this a
            // present-thread frame landing mid-OnOpen would read half-written
            // editor state. Geometry above is target-independent, so it still runs.
            // OS-139's compute probe. ⚠ THIS IS THE RENDER THREAD AND THAT IS
            // WHY IT IS HERE: the D3D11 immediate context it touches is not
            // thread safe, and FUCK calls Draw from its Present hook.
            //
            // ⚠ IT WAS IN ImGuiOverlay::PresentThunk FIRST AND THAT NEVER RAN.
            // That class hooks Present itself, but nothing outside its own file
            // has called it since ddb5c4a made the editor a FUCK IWindow, so the
            // hook is never installed and the thunk is dead code. The probe
            // logged nothing on a run where the editor was opened four times,
            // which is what a call site that does not execute looks like from a
            // log. Anything else needing a render-thread hook belongs here too.
            //
            // Outside the ready_ gate deliberately: the probe reads no editor
            // state, only the device.
            OS::DyeGpu::RunProbeOnce();

            // OS-139 rung 3. Builds at most a few queued tinted diffuses per
            // frame and returns immediately when the queue is empty, which is
            // every frame outside a dye apply.
            //
            // ⚠ HERE FOR THE SAME REASON THE PROBE IS: this is the live render
            // thread and the immediate context may not be touched from anywhere
            // else. Outside the ready_ gate deliberately, because it reads no
            // editor state.
            //
            // ⚠ THAT THIS ONLY RUNS WITH THE EDITOR OPEN IS NOT A GAP. A build
            // is only ever needed the first time a colour is chosen, which is
            // something that cannot happen with the editor closed. Re-applying
            // an already-built colour after a cell change is a cache hit on the
            // game thread and needs nothing from here.
            OS::DyeTexture::Pump();
            OS::OverlayBake::Pump();

            // ⚠ PROBE (field 2026-08-27): "the top and bottom we have ugly
            // bits where the panel isn't full vertical so we have an awkward
            // space of changing transparency panels, this isn't the case with
            // Vel'dun". The plate below runs on the carved path only, so under
            // FLICK's own theme nothing of ours paints a ground and every strip
            // where our panels stop shows the theme's window fill instead.
            //
            // That fill has never been read. Nothing on disk says FLICK's
            // default WindowBg is translucent or that Vel'dun's is not, and the
            // candidate fix turns on the numbers: kNoBackground makes us the
            // only painter, which also takes FLICK's own outside border away,
            // and Vel'dun must not move. So the border and the padding are here
            // too, and one run with a preset switch in it answers all of it.
            //
            // ⚠⚠ THROUGH OS::ui::StyleColor AND NOTHING ELSE. FuckCompat remaps
            // the colour enum from WindowBg on, so a raw read is silently wrong
            // on exactly the slots this asks about.
            //
            // ⚠ ON CHANGE, NOT ONCE, so switching preset in game prints both
            // themes in one session and the report is a comparison. Delete with
            // the fix.
            {
                const ImVec4 win   = OS::ui::StyleColor(ImGuiCol_WindowBg);
                const ImVec4 child = OS::ui::StyleColor(ImGuiCol_ChildBg);
                const ImVec4 bord  = OS::ui::StyleColor(ImGuiCol_Border);
                const bool   plain = OS::ChamferPanel::PlainShape();

                static ImVec4 lastWin{ -1.0f, -1.0f, -1.0f, -1.0f };
                static bool   lastPlain = false;
                static bool   seen      = false;
                if (!seen || plain != lastPlain || win.x != lastWin.x ||
                    win.y != lastWin.y || win.z != lastWin.z || win.w != lastWin.w) {
                    seen         = true;
                    lastWin      = win;
                    lastPlain    = plain;
                    const ImVec2 pad = OS::ui::WindowPadding();
                    spdlog::info(
                        "editor plate probe: shape {}, WindowBg {:.3f} {:.3f} {:.3f} "
                        "{:.3f}, ChildBg {:.3f} {:.3f} {:.3f} {:.3f}, Border {:.3f} "
                        "{:.3f} {:.3f} {:.3f}, WindowBorderSize {:.2f}, WindowRounding "
                        "{:.2f}, WindowPadding {:.1f} {:.1f}",
                        plain ? "plain" : "carved", win.x, win.y, win.z, win.w, child.x,
                        child.y, child.z, child.w, bord.x, bord.y, bord.z, bord.w,
                        FUCK::GetStyleVar(ImGuiStyleVar_WindowBorderSize),
                        FUCK::GetStyleVar(ImGuiStyleVar_WindowRounding), pad.x, pad.y);
                }
            }

            // ⚠⚠ OUR PLATE, PAINTED BEFORE ANY CONTENT, BECAUSE TWO PAINTERS OF
            // ONE APPEARANCE ALWAYS DRIFT. This window deliberately keeps FLICK's
            // own background (see GetFlags), so FLICK draws the plate at the
            // THEME's WindowRounding while our chrome is carved to a different
            // shape entirely. On Vel'dun the two happen to agree, because it is
            // angular and rounds nothing; on a theme that rounds its window the
            // theme's corner sits outside ours and the fringe between the two
            // radii is visible as a defect (field 2026-08-14, with a screenshot
            // of the corner). Painting our own plate over it, first thing,
            // leaves exactly one shape at the corner whatever the theme does.
            //
            // ⚠ OPAQUE ENOUGH TO COVER, and it has to be: this is painted ON TOP
            // of FLICK's plate rather than instead of it, so a translucent ground
            // would let the theme's corner show through the very place it is
            // being hidden. WindowBg with its alpha floored is the same trade the
            // action bar's TileBackdrop makes for the same reason.
            //
            // ⚠ NOT the frame. Only the ground and the corner it is cut to; the
            // editor's own header and panels draw their own edges.
            // ⚠⚠ ONLY WHEN OUR SHAPE DIFFERS FROM THE ONE FLICK ALREADY DREW.
            // On the plain path FLICK's own window background IS the correct
            // plate, so painting ours over it stacks two translucent blacks,
            // which reads darker than either, and leaves FLICK's own corner
            // showing past ours as a step (field 2026-08-14, "transparent black
            // bits that overlay the edges"). The fix for a second painter is not
            // a better second painter.
            if (!OS::ChamferPanel::PlainShape()) {
                const auto  pos  = FUCK::GetWindowPos();
                const auto  size = FUCK::GetWindowSize();
                const ImVec2 mn{ pos.x, pos.y };
                const ImVec2 mx{ pos.x + size.x, pos.y + size.y };
                ImVec4 ground = OS::ui::StyleColor(ImGuiCol_WindowBg);
                ground.w      = std::max(ground.w, 0.94f);
                // ⚠ NO SHAPE BRANCH HERE ANY MORE. It used to pick carved or
                // plain itself, which made this the only surface in the editor
                // that followed the setting while every button and tab inside it
                // stayed carved. ChamferPanel's own draw calls decide now, so
                // this asks for a filled panel and gets whichever shape the rest
                // of the editor is wearing.
                if (const auto tex = OS::IconImages::FrameFill()) {
                    OS::ChamferPanel::FillImage(tex, mn, mx, ground,
                                                OS::ChamferPanel::ArtCorner(mn, mx));
                } else {
                    OS::ChamferPanel::Fill(mn, mx, ground,
                                           OS::ChamferPanel::CutFor(mn, mx));
                }
            }

            if (ready_.load(std::memory_order_acquire)) {
                // ⚠ BEFORE EditorUI::Draw, because the draw CONSUMES the flick
                // this latches. Sampled after it, every hop would land a frame
                // late and a flick made on the last frame before a close would
                // be spent on the next open.
                //
                // ⚠ THE ONE CALL SITE. See PollEditorSticks: a second caller
                // doubles the look rate and eats one flick of every pair.
                OS::PollEditorSticks();
                // ⚠⚠ ONE GROUND FOR THE WHOLE EDITOR, AND THE NUMBERS ARE WHY.
                // Field 2026-08-27: "the top and bottom we have ugly bits where
                // the panel isn't full vertical so we have an awkward space of
                // changing transparency panels, this isn't the case with
                // Vel'dun". The probe read both presets: Vel'dun draws ChildBg
                // at alpha 0.000, FLICK's own theme at 0.680. This page nests 22
                // child windows two to four deep, so under FLICK's default every
                // one of them lays another translucent black over the last and
                // the fill climbs 0.68, 0.90, 0.97. The strips above the header
                // and below the footer are simply the places no child covers, so
                // they stay at one layer while everything beside them is at
                // three. That is the banding, and it was never the window plate.
                //
                // ⚠ A NO-OP ON VEL'DUN BY MEASUREMENT, NOT BY ARGUMENT. Vel'dun
                // is already at zero, so this writes the value it already has
                // and the preset the field signed off cannot move.
                //
                // ⚠ THE PANELS DO NOT GO BARE. Everything in this editor that
                // wants a fill paints its own through ChamferPanel, which is
                // exactly how Vel'dun has always looked right with ChildBg off.
                //
                // ⚠ AROUND THE CALL, NOT INSIDE IT. EditorUI::Draw has nine
                // early returns and all of them come back here, so one push and
                // one pop bracket every one of them.
                OS::ui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                OS::EditorUI::Draw();
                FUCK::PopStyleColor();
                // OS-97 temporary field probe. Costs one relaxed atomic per frame
                // when disarmed and queues a main-thread task on four of them.
                // Gated on ready_ for the same reason Draw is: it reads g_target.
                OS::CameraProbe::Tick();
                OS::CameraFrame::Tick();  // re-assert the retarget against three other writers
            }
            // Cache "is the cursor over our UI" for GetFlags()'s conditional
            // kPassInputToGame (see there). Over any FUCK window, or while a widget is
            // active (typing / holding a slider) => the panel owns input; otherwise the
            // cursor is on the character and the rotation drag should reach the game.
            const bool overUI =
                FUCK::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || FUCK::IsAnyItemActive();
            cursorOverUI_.store(overUI, std::memory_order_relaxed);

            // OS-80: do NOT drive the camera from here. A Draw-thread drag was
            // tried on 2026-07-19 and REVERTED the same night: InputListener's
            // sink already drives it (proven in field - "camera drag: LMB down
            // ... ARMED" logs from that sink), so this was a SECOND writer on
            // top of it, and kPassInputToGame below hands the same gesture to the
            // game as a third. The user felt it immediately: "i can get the
            // camera to move but only if i hold left click and then right click
            // at the same time, it's odd, not at all like SAM".
            //
            // The reasoning that produced it was the real error: an earlier
            // session logged no drag lines while the editor was drawing, and that
            // ABSENCE was read as proof the sink could not run. It only meant the
            // gesture had not happened. Do not re-add a driver here without first
            // proving from a log that the sink is genuinely silent DURING a
            // completed drag. See [[read-evidence-before-mechanism]].

            // ⚠⚠ OS-53's RIGHT-STICK PASSTHROUGH GATE IS GONE, AND IT NEVER ONCE
            // OPENED. It cached FUCK::IsKeyDown on the four RStick nav keys and
            // its own comment allowed for FUCK not surfacing them ("then it's a
            // Fuzzles ask"). MEASURED 2026-08-15 from the field log: over 134
            // navprobe lines covering every control on the pad, `RStick*` and
            // `LStick*` appear ZERO times. So this stored false for its whole
            // life, GetFlags never saw it, and the right stick has always needed
            // Show Player In Inventory's R3 hold - which is exactly what the
            // field reported ("right stick we still have to hold down to
            // rotate"). The stick is read from the raw device sink now, where it
            // genuinely arrives, and it drives the camera directly rather than
            // asking for passthrough. See InputListener.h.
            rStickActive_.store(false, std::memory_order_relaxed);

            // OS-80d: the wheel needs NO latch any more. OS-80c latched a wheel window
            // to work around the gate opening a frame late, but under SAM the gate is
            // now hover-gated in GetFlags, so it is already open when the tick lands
            // and the whole queue - wheel included - reaches SAM untouched.

            // NOTE: deliberately still BUTTON-based, not the hover gate above. This
            // drives ForceCursor, and the cursor should stay visible while merely
            // hovering the character; only an actual drag should let the game hide it.
            const bool passing = !overUI && (FUCK::IsMouseDown(0) || FUCK::IsMouseDown(1));

            // Re-assert the cursor every non-drag frame: ShowMenus(false) keeps
            // re-hiding the game cursor, so the one-shot ForceCursor(true) on open does
            // not stick (field: cursor invisible). During an actual rotate-drag we let
            // the game hide it.
            //
            // ⚠ A HIDDEN GAME CURSOR IS NOT A TRACKED CURSOR, which is worth
            // keeping written down even though nothing here depends on it any
            // more: hide it and the engine recentres the pointer, so it stops
            // moving entirely. Visible and movable are the same switch. That is
            // what killed the pointer experiment on 2026-08-15.
            if (!passing) {
                FUCK::ForceCursor(true);
            }

            // AE: the inventory's floating item-3D loads via an async
            // NewInventoryMenuItemLoadTask, so the one-shot Clear3D on open can
            // land BEFORE the model does and the item card pops back in over
            // the editor (Ivy, 1.6.1170 field report; on SE the model is
            // already loaded at open, so this stays a one-time no-op there).
            // Re-clear whenever models are present. Draw runs on the render
            // thread: the loadedModels-size read is a benign race, and the
            // actual Clear3D is queued onto the main thread (engine UI state).
            // ⚠ THIS RE-ASSERT IS REACTIVE AND THEREFORE ALWAYS LATE, WHICH IS
            // A DIFFERENT PROBLEM FROM THE ONE IT WAS WRITTEN FOR. It cured the
            // model landing after a one-shot clear and STAYING. It cannot cure
            // a model landing and being shown for a frame or two before this
            // notices, because the chain is: engine loads -> engine renders ->
            // Draw sees loadedModels non-empty -> task queued -> main thread
            // clears. Two hops, minimum, and both after the first render.
            //
            // The field reports split-second flashes on hover (2026-08-06),
            // which is exactly that shape. Clearing more often does not shorten
            // the chain; the cure is to stop it DRAWING rather than to clear it
            // after it has drawn, which means suppressing
            // Inventory3DManager::Render while the editor is open. That is a new
            // engine hook and is deliberately not smuggled in here.
            //
            // ⚠⚠ OS-173/OS-154: EVERYTHING ABOVE THIS LINE IS THE ORIGINAL
            // REASONING AND ITS MECHANISM WAS WRONG. Kept because two engine
            // hooks were built on it before the decompile settled it, and the
            // wrong turn is worth more written down than deleted.
            //
            // `Clear3D` DOES NOT EMPTY `loadedModels`. Decompiled on AE
            // (id 51759, rva 0x927C40): its `this+0x58` is a BSSpinLock, not the
            // array, which is at 0x60 with its count at 0x148; the two helpers
            // around the body are lock and unlock, and the call between them
            // clears the ExtraDataList at `originalExtra` (0x40). The only other
            // thing it does is OR bit 0 into `loadedModels[count-1].spModel->
            // flags` at +0xF4, which CommonLib names NiAVObject::Flag::kHidden.
            // Its last call takes a GLOBAL rather than the manager, so it cannot
            // reach the array either.
            //
            // ⚠ SO THERE IS NO WRITE WAR AND THERE NEVER WAS. Nothing Fitting
            // Room calls empties that array, so `!loadedModels.empty()` is
            // PERMANENTLY true once anything has been hovered, and this block
            // re-fired every frame re-setting a bit that was already set. The
            // ~145/s OS-173 measured was right; reading it as a rival writer was
            // not. Field-confirmed twice: with the load path detoured, zero
            // loads were refused across five editor opens and the clears carried
            // on regardless.
            //
            // So the predicate now asks the question `Clear3D` actually answers:
            // is the model it would hide already hidden? That gates exactly the
            // operation performed, so nothing that used to be hidden stops being
            // hidden - the first clear after any change still runs, and only the
            // provably redundant repeats are skipped.
            //
            // ⚠ `Clear3D` HIDES ONLY `loadedModels[count-1]`, while
            // `FUN_1409281a0` attaches EVERY loaded model to the
            // UI3DSceneManager scene, and the array is a ring capped at 7
            // (matching BSTSmallArray<LoadedInventoryModel, 7>). So the guard
            // and the action both cover the WHOLE array rather than the last
            // entry: `visible` counts every unhidden model, and the task hides
            // any that Clear3D's single-entry teardown does not reach.
            //
            // ⚠⚠ `extra` IS A MEASUREMENT, NOT A FIX, AND THE DISTINCTION IS THE
            // POINT. Reading UpdateMagic3D (AE 51758) suggests the engine
            // already calls Clear3D on the OUTGOING model before appending the
            // incoming one and unhides the incoming one on a cache hit, which
            // would make "at most one entry is ever unhidden" an invariant and
            // `extra` permanently 0. That has NOT been observed, only read, and
            // this file has already been wrong twice about this manager from a
            // decompile. So the loop is written to be correct either way and to
            // REPORT which world we are in: a nonzero `extra` in the field means
            // several attached models really can be visible at once and this is
            // load-bearing, while a persistent 0 means Clear3D was always
            // sufficient and the flash lies elsewhere. Do not delete the loop on
            // the strength of the reasoning alone; delete it on the strength of
            // the log.
            if (!openedFromSam_.load(std::memory_order_relaxed)) {
                if (auto* inv3d = RE::Inventory3DManager::GetSingleton()) {
                    // Render-thread read of engine state, like the size read it
                    // replaces: a benign race, and the writes stay on the main
                    // thread in the queued task below.
                    const auto& models = inv3d->GetRuntimeData().loadedModels;
                    const auto  count  = models.size();
                    // ⚠ A null spModel counts as NOTHING TO HIDE rather than as
                    // work to do. Clear3D dereferences that pointer without a
                    // null check, so the engine does not produce one; treating
                    // it as work is what would re-open the every-frame loop.
                    // ⚠ GetFlags(), NOT the bare `flags` member: that member is
                    // compiled out on the VR layout, and Fitting Room ships one
                    // universal DLL, so touching it directly does not build.
                    // Same trap as GetModelData() vs the bare modelBound.
                    std::uint32_t visible = 0;
                    for (const auto& m : models) {
                        const auto* node = m.spModel.get();
                        if (node && !node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                            ++visible;
                        }
                    }
                    if (visible != 0 &&
                        !item3dClearQueued_.exchange(true, std::memory_order_relaxed)) {
                        SKSE::GetTaskInterface()->AddTask([this, count, visible]() {
                            if (open_.load(std::memory_order_relaxed)) {
                                if (auto* i3d = RE::Inventory3DManager::GetSingleton()) {
                                    // The engine's own teardown first: it hides
                                    // the last entry AND drops the item's
                                    // ExtraDataList, which we must not
                                    // reimplement.
                                    i3d->Clear3D();
                                    // Then whatever it left visible. ⚠ The raw
                                    // bit rather than SetAppCulled, because the
                                    // bit is exactly what Clear3D itself sets,
                                    // so this cannot diverge from the engine's
                                    // own idea of hidden - and it needs no
                                    // second Address Library id to go wrong.
                                    std::uint32_t extra = 0;
                                    for (auto& m : i3d->GetRuntimeData().loadedModels) {
                                        auto* node = m.spModel.get();
                                        if (node &&
                                            !node->GetFlags().any(
                                                RE::NiAVObject::Flag::kHidden)) {
                                            node->GetFlags().set(
                                                RE::NiAVObject::Flag::kHidden);
                                            ++extra;
                                        }
                                    }
                                    const auto n = item3dClears_.fetch_add(
                                                       1, std::memory_order_relaxed) +
                                                   1;
                                    // ⚠ A flood here means the guard regressed
                                    // to something permanently true again, which
                                    // is the exact defect this replaced.
                                    spdlog::info(
                                        "Inventory3D: hid the item preview ({} model(s) "
                                        "loaded, {} visible, {} beyond Clear3D's reach, {} "
                                        "time(s) this session). More than a few of these per "
                                        "editor session means the guard is stuck true again; "
                                        "a persistent 0 beyond-reach means Clear3D alone was "
                                        "always enough.",
                                        count, visible, extra, n);
                                }
                            }
                            item3dClearQueued_.store(false, std::memory_order_relaxed);
                        });
                    }
                }
            }

            // Close on Start or Esc - kCloseOnEsc is dropped (see GetFlags) so B/Circle
            // stays free for FUCK's back-nav. RequestClose queues onto the main thread
            // (SetOpen touches RE::UI / the camera).
            //
            // ⚠ NOT WHILE FLICK'S OWN PANEL IS UP. Both windows draw in one
            // ImGui frame off one key state, so a single Escape closed FLICK's
            // settings AND the editor under it. See
            // EditorGate::ShouldCloseOnOwnEscape for why the answer is read
            // over two frames rather than one.
            //
            // Start is inside the guard with Escape on purpose: the panel is
            // controller-navigable, so Start over it means the same thing.
            const bool flickMenu = FUCK::IsMenuOpen();
            const bool ownEscape = OS::EditorGate::ShouldCloseOnOwnEscape(
                flickMenu, flickMenuWasOpen_);
            flickMenuWasOpen_ = flickMenu;
            // ⚠⚠ B CLOSES THE EDITOR, AND IT IS ONLY READABLE BECAUSE NOTHING
            // HERE IS NAV-ELIGIBLE. ImGui spends B on nav cancel wherever it has
            // a cursor, which is what made this unreadable for three rounds ("i
            // can't exit out of the outfit slot pane"). With no nav items there
            // is no cancel to collide with, so B is simply "leave", and it asks
            // on the way out so unpaid work is not dropped.
            // ⚠ DEFERRED WITH THE REST OF THE PAD BINDINGS (ControllerSupport.h).
            // B-to-close is from 2026-08-15 and was never field tested. Start and
            // Escape are untouched, so a controller can still leave the editor.
            const bool padBack = OS::ControllerSupport::kEnabled &&
                                 FUCK::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
            // ⚠⚠ THE EDITOR HOTKEY CLOSES FROM HERE, NOT ONLY FROM THE INPUT
            // SINK (user 2026-08-18: "the hotkey to enter FR when i press it
            // while FR is open doesn't always close FR consistently"). The
            // sink in InputListener answers the key with Toggle, and that sink
            // is STARVED while FLICK owns input, which is whenever the cursor is
            // over the panel: InputListener's own notes record it for the mouse
            // (OS-80) and the sticks. So the key closed the editor when the
            // cursor happened to be off the panel and did nothing when it was
            // on it, which reads as "sometimes". FUCK's key state is fed from
            // the same hook that starves the sink, so it is read here beside
            // Escape, on the press edge (IsInputDown is level, so the edge is
            // ours), seeded down at open so the opening press cannot close, and
            // not while a text field has the key (the default binding is a
            // printable letter). Both routes end in RequestCloseAsking, which is
            // idempotent, so the frames where the sink is fed and both fire do
            // not double up.
            const auto hotkeyDik  = OS::Settings::GetSingleton().editorKeyDIK;
            const bool hotkeyDown = hotkeyDik != 0 && FUCK::IsInputDown(hotkeyDik);
            const bool hotkeyEdge = hotkeyDown && !hotkeyWasDown_;
            hotkeyWasDown_        = hotkeyDown;
            if (ownEscape && (padBack || FUCK::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
                              FUCK::IsKeyPressed(ImGuiKey_Escape, false))) {
                // Asking: this is a deliberate exit, so unpaid work gets a
                // question rather than being dropped.
                OS::EditorWindow::RequestCloseAsking();
            } else if (ownEscape && hotkeyEdge && !FUCK::IsAnyItemActive()) {
                // The line is what separates "the key was seen here" from "the
                // sink happened to be fed", the two ways this can close.
                spdlog::info("editor hotkey: closing from the draw loop (the input sink is "
                             "starved while FLICK owns input).");
                OS::EditorWindow::RequestCloseAsking();
            }

        }

        // Snap back to GetDefaultSize/Pos next frame (the gear's "Reset window position").
        // Render-thread safe - just sets an atomic Draw() reads.
        void RequestResetGeometry() { resetGeometry_.store(true, std::memory_order_relaxed); }

        // Stage this window's state again without closing it, for an apply that
        // moved the character out from under the staged copy.
        //
        // ⚠ THE SAME ready_ DANCE THE OPEN PATH DOES, and for the same reason:
        // OnOpen writes the editor's state on the MAIN thread while Draw reads
        // it on the present thread, so Draw has to stand down for the length of
        // it or a frame lands mid-write. MAIN THREAD ONLY; RequestRestage is
        // the caller that guarantees it.
        void Restage() {
            if (!open_.load(std::memory_order_relaxed)) {
                return;
            }
            ready_.store(false, std::memory_order_release);
            // ⚠⚠ AND THE FIT, WHICH THIS PATH USED TO SKIP. SetOpen runs
            // EnsureFitCurrent before OnOpen; the re-stage did not, so the one
            // event most likely to have changed the player's race and sex, an
            // apply carrying a look, was also the one that never re-evaluated
            // them. It showed up on the skin cards: the mannequin's parts are
            // resolved inside RefreshFitFor, so an import male-to-female left
            // every card drawing a male body for the rest of the session (field
            // 2026-08-27, the last Mannequin line said "sex M" eight seconds
            // before the character went back to female and none followed).
            //
            // The "editor must be closed" note on EnsureFitCurrent is about
            // Draw reading fitsBody unsynchronized, and ready_ is already false
            // above, so Draw is standing down for exactly this reason. Cheap
            // when the race and sex did not move, which is most applies.
            OS::StyleCatalog::GetSingleton().EnsureFitCurrent();
            OS::EditorUI::OnOpen();
            ready_.store(true, std::memory_order_release);
        }

        // The preview pump's gate, read from DyeTexture's Present thunk: work
        // only while the editor is open AND populated. ready_ drops FIRST on
        // the close path (SetOpen), so the pump stands down a present before
        // OnEditorClose tears the store's surroundings down.
        bool WantsPreviewPump() const {
            return open_.load(std::memory_order_relaxed) &&
                   ready_.load(std::memory_order_acquire);
        }

    private:
        std::atomic<bool> open_{ false };
        std::atomic<bool> ready_{ false };        // set true AFTER OnOpen populates editor state; Draw early-outs until then
        // An exception escaped Draw once, so the editor is on its way out and
        // must not be drawn again. Latched rather than counted: a throw that
        // repeats every frame would write a log line every frame.
        std::atomic<bool> drawFaulted_{ false };
        std::atomic<bool> cursorOverUI_{ true };  // cached in Draw(); gates kPassInputToGame (safe default: no passthrough)
        std::atomic<bool> forceLayout_{ false };  // OS-54: set on open; Draw re-applies GetDefaultSize/Pos on the first post-open frame
        std::atomic<bool> rStickActive_{ false }; // OS-53: right stick deflected => rotate passthrough (cached in Draw)
        std::atomic<bool> resetGeometry_{ false };// gear "Reset window position" => re-apply defaults next frame
        std::atomic<bool> item3dClearQueued_{ false };  // AE async item-3D re-clear: one queued main-thread task at a time
        // How often that re-clear has actually fired. Session-lifetime and
        // never reset, deliberately: the question it answers is a RATE against
        // the flashes the player reports, and zeroing it per editor session
        // would make a hover-heavy session look the same as a quiet one.
        std::atomic<std::uint32_t> item3dClears_{ 0 };
        bool              wasFirstPerson_{ false };
        // The menu whose Scaleform root alpha this window zeroed on open, and
        // the only thing the close puts back. Empty while shut.
        //
        // ⚠ NOT ATOMIC, AND IT DOES NOT WANT TO BE. Written and read only in
        // SetOpen, which is main-thread on every path (Toggle, RequestOpen and
        // RequestClose all marshal through the SKSE task interface). The atomic
        // beside it exists because the render and input threads ask about SAM
        // every frame; nothing off the main thread asks this.
        std::string       hiddenHost_;
        // Main-thread debt: true only when Menu Studio accepted this editor's
        // non-SAM item-preview suppression claim.
        bool              menuStudioItemPreviewClaimHeld_{ false };
        // FLICK's own panel, as of the PREVIOUS drawn frame. Plain bool, not an
        // atomic: written and read only by Draw, on the render thread, like
        // wasFirstPerson_ above.
        bool              flickMenuWasOpen_{ false };
        bool              hotkeyWasDown_{ false };  // the editor hotkey's last level, for its press edge (Draw)
        std::atomic<bool> openedFromSam_{ false };
    };

    EditorIWindow g_editorWindow;
}

namespace OS::EditorWindow {

    void Register() {
        // SettingsUI::Register already called FUCK::Connect; RegisterWindow is a
        // no-op if FUCK is absent (no editor without FUCK.dll).
        FUCK::RegisterWindow(&g_editorWindow);
        spdlog::info("EditorWindow: registered as a FUCK IWindow (editor hotkey opens it).");
    }

    bool IsOpen() { return g_editorWindow.IsOpen(); }

    void PumpPreviews() {
        // Once per present, called by DyeTexture's thunk OUTSIDE FUCK's whole
        // UI pass, whichever side of it that hook chained onto. It lived at
        // the top and bottom of Draw first, and the offscreen render, the
        // FUCK::Image loads and the eviction releases all ran INSIDE FUCK's
        // draw pass; the whole editor vanished on exactly the loading frames
        // (field 2026-08-09, twice reported). Release first, then drain, the
        // same order Draw used: pendingRelease holds the PREVIOUS present's
        // evictions, whose draw data FUCK has fully rendered by now.
        if (!g_editorWindow.WantsPreviewPump()) {
            return;
        }
        OS::PreviewCache::ReleasePending();
        OS::PreviewCache::Drain();
        // The overlay picker's thumbnails, on the same terms and for the same
        // field reason. Its decode happens on a worker thread, but turning the
        // finished PNG into a FUCK handle is FUCK image traffic and belongs out
        // here with the rest of it rather than inside the UI pass.
        OS::OverlayThumbs::Pump();
    }

    bool WantsTextInput() { return FUCK::IsAnyItemActive(); }

    bool CursorOverUI() { return g_editorWindow.CursorOverUI(); }

    bool OpenedFromSam() { return g_editorWindow.OpenedFromSam(); }

    void ResetGeometry() { g_editorWindow.RequestResetGeometry(); }

    void Toggle() {
        // The hotkey's close edge is a deliberate exit like Escape, so it asks
        // too. Opening is unchanged.
        if (g_editorWindow.IsOpen()) {
            RequestCloseAsking();
            return;
        }
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { g_editorWindow.SetOpen(!g_editorWindow.IsOpen()); });
        }
    }

    void RequestOpen() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                if (g_editorWindow.IsOpen()) {
                    return;
                }
                // ⚠ SAME GATE AS THE HOTKEY, AND IT WAS MISSING HERE TOO. This
                // is the Papyrus entry (OutfitSlotsSAM.OpenEditor), so without
                // it a lore-mode player who cannot press the key open could
                // still be handed the editor by a script. Silent on refusal.
                if (!OS::LoreModule::GateSatisfied()) {
                    spdlog::info("EditorWindow: open refused, the lore module is active "
                                 "and the player is not carrying the Seamstone. Nothing "
                                 "shown, by design.");
                    return;
                }
                if (OS::HostGuard::HostMenuOpen()) {
                    g_editorWindow.SetOpen(true);
                } else {
                    // Silent, the same call the hotkey's kNeedContext arm got
                    // (user 2026-08-11). This is the Papyrus entry, so a script
                    // asked for the editor in the open world and the player may
                    // not have done anything at all; a corner notification
                    // there is the mod answering a question nobody asked.
                    spdlog::info("EditorWindow: open refused, no menu this editor can be "
                                 "hosted by is open. Nothing shown, by design.");
                }
            });
        }
    }

    void RequestClose() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                if (g_editorWindow.IsOpen()) {
                    g_editorWindow.SetOpen(false);
                }
            });
        }
    }

    void RequestRestage() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                if (!g_editorWindow.IsOpen()) {
                    return;
                }
                spdlog::info("EditorWindow: re-staging in place (an apply moved "
                             "the character under the staged copy).");
                g_editorWindow.Restage();
            });
        }
    }

    void RequestCloseAsking() {
        if (!g_editorWindow.IsOpen()) {
            return;
        }
        if (OS::EditorUI::CloseWouldLoseWork()) {
            // ⚠ NOT QUEUED, AND NOT A CLOSE. The modal is drawn by the editor,
            // so the window has to stay open for it to appear at all; queueing a
            // close beside it would take the window away from under its own
            // question. The flag is read by the next drawn frame.
            OS::EditorUI::RequestCloseConfirm();
            return;
        }
        RequestClose();
    }

}  // namespace OS::EditorWindow
