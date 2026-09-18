#include "InputListener.h"

#include "ProfileProbe.h"  // W1 spike, dies with its key when the Looks page lands
#include "DirectEntry.h"

#include "EditorGate.h"
#include "EditorWindow.h"
#include "HostGuard.h"  // the one list of menus the editor may be hosted by
#include "ImGuiOverlay.h"
#include "LoreModule.h"
#include "OutfitSession.h"
#include "Requip.h"      // the requip transition's driver (OS-206)
#include "RequipDiff.h"  // which slots the cycle actually changed
#include "Settings.h"
#include "TextEntry.h"
#include "WorldWatch.h"

#include <cmath>   // std::fmod - the free-camera angle wrap
#include <optional>

namespace OS {

    namespace {

        // OS-73. Drive the FREE camera (tfc) directly from the drag, because in
        // the NO-SAM case nothing else will: the engine does not route look
        // input to the camera while a menu context owns it, so with the editor
        // open over the inventory this is the only camera control there is.
        //
        // OS-80c, 2026-07-19: NOT under SAM. This path is now gated off whenever
        // EditorWindow::OpenedFromSam(), because SAM is an orbit+pan+FOV camera
        // and this is free-look - a different interaction that CANNOT be tuned
        // into feeling like SAM by adjusting sensitivity or signs. Worse, SAM
        // orbits by writing translation and rotation on the same FreeCameraState
        // instance, so both writers race the two floats below. The gate lives at
        // the LMB arm in ProcessEvent; see the note there for the field evidence.
        //
        // Correcting an over-claim in the previous version of this comment: alpha-
        // hiding SAM's 2D does NOT make SAM's camera controls unreachable. They are
        // raw mouse gestures, not menu buttons, and kPassInputToGame delivers them.
        // The user reaching SAM's camera at all through the passthrough is what
        // proved it.
        //
        // ONLY the free camera. In third person the editor already hands the
        // gesture to the game (kPassInputToGame in EditorWindow::GetFlags,
        // granted for precisely this drag), and the vanilla menu rotates the
        // character itself - writing the camera here as well would apply the
        // drag twice, the SPIM double-rotation failure. The free camera does
        // not double-apply for the same reason it needed this at all: the
        // passed-through look never reaches it.
        //
        // FreeCameraState is absent from this CommonLib snapshot, so the layout
        // is BYTE-VERIFIED off both shipped binaries rather than assumed - the
        // previous (never-executed) version of this code had it wrong:
        //   +0x30  NiPoint3 translation   (GetTranslation reads 0x30/0x34/0x38)
        //   +0x3C  float    pitch         (GetRotation reads 0x3C, engine SUBTRACTS the vertical axis)
        //   +0x40  float    yaw           (GetRotation reads 0x40, engine ADDS the horizontal axis)
        // Identical on SE 1.5.97 (vtable 0x16A9F50) and AE 1.6.1170 (0x18EF2E8),
        // read out of FreeCameraState's own GetRotation/GetTranslation and its
        // update helper (SE 140848AA0 / AE 1408E0640). The old +0x2C would have
        // written yaw into translation.x and teleported the camera.
        //
        // Signs and wrapping mirror that helper exactly, so a drag feels like
        // ordinary free-camera look: the engine wraps BOTH fields into
        // [0, 2pi) each frame and clamps neither (the free camera is meant to
        // loop over the top), so we wrap too rather than inventing a pitch limit.
        // OS-80 diagnostics. Every gate in this path used to decline SILENTLY,
        // so a field report of "the camera will not move with SAM" could not say
        // WHICH gate refused - and the three candidates need opposite fixes. One
        // line per gesture (reset on the LMB edge, never per mouse-move) names
        // the reason. Reads as noise in a log only until it saves a round trip.
        bool g_dragDiagDone{ false };

        void ApplyFreeCameraDrag(float a_dx, float a_dy) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->currentState) {
                if (!g_dragDiagDone) {
                    g_dragDiagDone = true;
                    spdlog::info("camera drag: no PlayerCamera/currentState - drag ignored.");
                }
                return;
            }
            auto* const state = cam->currentState.get();
            if (state->id != RE::CameraState::kFree) {
                if (!g_dragDiagDone) {
                    g_dragDiagDone = true;
                    spdlog::info(
                        "camera drag: camera state is {}, not kFree(3) - drag ignored. "
                        "If SAM framed this shot, that number IS SAM's camera, and this "
                        "path's free-camera-only scope is why nothing moves.",
                        static_cast<int>(state->id));
                }
                return;
            }
            if (!g_dragDiagDone) {
                g_dragDiagDone = true;
                spdlog::info("camera drag: driving the free camera (state 3) - "
                             "gates passed, writing pitch/yaw.");
            }
            constexpr float kTwoPi = 6.2831853f;
            const float     s      = Settings::GetSingleton().cameraDragSensitivity;
            auto* const     rot =
                reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(state) + 0x3C);

            float pitch = rot[0] - a_dy * s;
            float yaw   = rot[1] + a_dx * s;
            pitch       = std::fmod(pitch, kTwoPi);
            yaw         = std::fmod(yaw, kTwoPi);
            if (pitch < 0.0f) {
                pitch += kTwoPi;
            }
            if (yaw < 0.0f) {
                yaw += kTwoPi;
            }
            rot[0] = pitch;
            rot[1] = yaw;
        }

        // ---- the sticks, read raw ----------------------------------------
        //
        // See InputListener.h for the measurement that put them here: FUCK
        // surfaces neither stick, so neither the pane hop nor the rotation gate
        // could ever have fired from an ImGui key.
        //
        // ⚠⚠ THE LEFT STICK IS NOT READ AT ALL, AND THAT IS THE DECISION
        // RATHER THAN AN OVERSIGHT. It has driven a pane hop, an item stepper and
        // a mouse pointer over the course of 2026-08-15 and every one of them was
        // rejected in the field. User's call at the end of that day: "i don't
        // care anymore about extensive controller support, just have basic". So
        // the pad turns the character, changes page, changes outfit and closes,
        // and choosing things is the mouse's job. NoteLeftStick is gone with the
        // models it fed.

        // ⚠⚠ THE RIGHT STICK IS PUBLISHED, NOT APPLIED, AND THE FIRST CUT
        // APPLIED IT TO THE WRONG THING. It drove ApplyFreeCameraDrag, which
        // refuses any camera state but kFree by design, and MEASURED in the
        // field log: "camera drag: camera state is 9, not kFree(3) - drag
        // ignored". State 9 is kThirdPerson (RE/P/PlayerCamera.h), which is what
        // this editor runs in, so the stick could never have turned anything
        // (user: "i have to hold right stick to rotate char still").
        //
        // The editor spends it instead, on its SUBJECT, because the subject can
        // be a follower and this file has no idea who that is. See
        // EditorUI's ApplySubjectSpin.
        //
        // ⚠⚠ SKIPPED WHILE R3 IS HELD, WHICH IS SHOW PLAYER IN INVENTORY'S OWN
        // GESTURE. SPII rotates on R3 plus the stick; rotating here as well
        // would apply the same motion twice on exactly the frames the player is
        // using SPII's binding. That is the SPIM double-rotation failure this
        // file's camera note already names, arriving through a second door.
        std::atomic<float> g_lookX{ 0.0f };  // right stick X, waiting to be spent
        std::atomic<float> g_lookY{ 0.0f };  // right stick Y, the same

        // Is R3 held, read off the device's own XInput button mask.
        //
        // ⚠⚠ AN OFFSET READ, AND IT IS BOUNDED BY SHAPE RATHER THAN BY VERSION.
        // The vcpkg CommonLib snapshot this project builds against splits the
        // second XINPUT_STATE block into `unk104` and a one-byte `prevState`, so
        // there is no named field to ask; CommonLibSSE-NG maps the same bytes as
        // XINPUT_STATE{ dwPacketNumber @0x100, wButtons @0x104 }, which is
        // XInput's own layout and is why 0x104 is the mask. Both snapshots
        // static_assert the type at 0x128, so this is not a version guess.
        //
        // ⚠ REFUSED BY SHAPE IF IT DOES NOT LOOK LIKE A BUTTON MASK. XInput
        // leaves bits 10 and 11 undefined and defines nothing above 0x8000, so a
        // read carrying anything else is not the field this thinks it is, and
        // the honest answer is then "no idea", which costs only the guard.
        [[nodiscard]] bool RightThumbHeld(const RE::BSWin32GamepadDevice* a_pad) {
            constexpr std::uint16_t kDefined   = 0xF3FF;  // every bit XInput names
            constexpr std::uint16_t kRightStick = 0x0080;  // XINPUT_GAMEPAD_RIGHT_THUMB
            const auto mask = *reinterpret_cast<const std::uint16_t*>(
                reinterpret_cast<const std::byte*>(a_pad) + 0x104);
            if ((mask & static_cast<std::uint16_t>(~kDefined)) != 0) {
                return false;
            }
            return (mask & kRightStick) != 0;
        }

        void NoteRightStick(float a_x, float a_y, bool a_r3Held) {
            // ⚠ THE DEADZONE IS PER AXIS, NOT ON THE VECTOR. Turning and zooming
            // are separate gestures on one stick, and a shared magnitude test
            // would let a hard push left leak a little zoom into the shot.
            constexpr float kRest = 0.15f;
            if (a_r3Held || !Settings::GetSingleton().cameraDragWhileOpen) {
                g_lookX.store(0.0f, std::memory_order_relaxed);
                g_lookY.store(0.0f, std::memory_order_relaxed);
                return;
            }
            // ⚠ REPLACED, NOT ACCUMULATED. These are rates, not distances: the
            // poll and the spend both run once a frame, so the newest sample IS
            // this frame's motion. Summing would make a frame the editor skipped
            // turn twice as far as one it drew.
            g_lookX.store(std::fabs(a_x) < kRest ? 0.0f : a_x, std::memory_order_relaxed);
            g_lookY.store(std::fabs(a_y) < kRest ? 0.0f : a_y, std::memory_order_relaxed);
        }

    }  // namespace

    float TakeEditorLookX() { return g_lookX.exchange(0.0f, std::memory_order_relaxed); }

    float TakeEditorLookY() { return g_lookY.exchange(0.0f, std::memory_order_relaxed); }

    // ⚠⚠ POLLED OFF THE DEVICE, NOT RECEIVED AS AN EVENT, AND THE FIRST CUT OF
    // THIS WAS AN EVENT SINK THAT NEVER FIRED ONCE. MEASURED 2026-08-15: with
    // the sink handler in place and the editor open on a pad, `navring: flick`
    // appeared ZERO times in a full session log. The reason is written at the
    // top of this file in the OS-80 note and it applies to every device, not
    // just the mouse: the editor is a FLICK IWindow, FLICK owns input while its
    // window is up, and this BSInputEvent sink is starved for the duration. The
    // camera drag was stranded by exactly this and moved to a Draw-thread call
    // (ApplyEditorCameraDrag); the sticks follow it.
    //
    // ⚠ THE DEVICE OBJECT IS NOT PART OF THAT ARGUMENT. BSWin32GamepadDevice
    // holds the last polled XInput axes as plain floats, written by the engine's
    // device poll, which is upstream of anything a menu or an overlay can
    // consume. Reading them asks the hardware what it is doing rather than
    // asking who was allowed to hear about it.
    //
    // ⚠⚠ TWO CommonLib SNAPSHOTS NAME THESE FIELDS OPPOSITELY AND BOTH COMPILE.
    // The vcpkg copy this project builds against calls 0x0F0 `curLX` and 0x118
    // `prevLX`; CommonLibSSE-NG calls 0x0F0 `previousLX` and 0x118 `currentLX`,
    // and NG's reading is the coherent one - it maps 0x0D8 and 0x100 as two
    // XINPUT_STATE blocks, which puts the packet number and the button mask
    // where XInput's own struct has them. Both agree the type is 0x128 and both
    // static_assert it, so the OFFSETS are not in doubt; only which block is
    // this frame. It does not matter here: the two are one poll apart, and a
    // flick gesture and a look rate cannot tell one frame from the next. Named
    // fields are used rather than raw offsets so the compiler keeps this honest.
    //
    // ⚠ ONE CALLER, ONCE A FRAME, FROM THE EDITOR'S OWN DRAW. Called twice a
    // frame it would double the rotation rate and eat one flick of the pair.
    void PollEditorSticks() {
        auto* const idm = RE::BSInputDeviceManager::GetSingleton();
        if (!idm || !idm->IsGamepadConnected()) {
            return;
        }
        auto* const pad = skyrim_cast<RE::BSWin32GamepadDevice*>(idm->GetGamepad());
        if (!pad) {
            return;
        }
        NoteRightStick(pad->GetRuntimeData().currentRX, pad->GetRuntimeData().currentRY,
                       RightThumbHeld(pad));
    }

    // OS-80 ROOT CAUSE. The editor runs as a FLICK IWindow, and FLICK owns the
    // mouse while its window is up, so the BSInputEvent sink further down never
    // receives the click that used to drive this. The drag was architecturally
    // stranded when the editor moved off Fitting Room's own ImGuiOverlay - the
    // sink code is intact and correct and simply never runs, which is why the
    // field symptom was total silence rather than a wrong rotation.
    //
    // EditorWindow::Draw calls this instead, from inside FLICK's own frame where
    // the mouse delta is real (FUCK::GetMouseDelta, FLICK's ImGui context - NOT
    // ImGui::GetIO(), which is a different context in this process). The camera
    // work itself is unchanged and still uses the byte-verified offsets.
    void ApplyEditorCameraDrag(float a_dx, float a_dy) { ApplyFreeCameraDrag(a_dx, a_dy); }

    InputListener& InputListener::GetSingleton() {
        static InputListener instance;
        return instance;
    }

    void InputListener::Register() {
        const auto& settings = Settings::GetSingleton();
        auto&       self     = GetSingleton();
        self.editorKey_      = settings.editorKeyDIK;
        self.editorPad_      = settings.editorGamepadButton;
        self.nextKey_        = settings.nextOutfitKeyDIK;
        self.directKey_      = settings.directEntryKeyDIK;
        self.profileProbeKey_ = settings.profileProbeKeyDIK;

        // ⚠⚠ THE SINK IS REGISTERED WHATEVER IS BOUND, AND IT USED NOT TO
        // BE. This ran once at kDataLoaded and returned early when nothing was
        // bound, which was defensible while the editor key shipped bound to Y.
        // Two of the three bindings are unassigned by default now, so a player
        // who clears the third and then binds one in the settings panel would
        // have been talking to a sink that was never installed: the dropdown
        // would take the key, the INI would keep it, and nothing would happen
        // until the next launch. The cost of always registering is one
        // comparison per input event against three zeroes.
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(static_cast<RE::BSTEventSink<RE::InputEvent*>*>(&self));
            spdlog::info("InputListener registered (editor DIK 0x{:X}, pad 0x{:X}, "
                         "next-outfit DIK 0x{:X}, direct-entry DIK 0x{:X}).",
                         self.editorKey_, self.editorPad_, self.nextKey_,
                         self.directKey_);
        }
    }

    void InputListener::CycleOutfit() {
        auto&       session = OutfitSession::GetSingleton();
        std::string name;
        bool        changed = false;
        // ⚠ TAKEN BEFORE THE CYCLE MUTATES ANYTHING, and that ordering is the
        // whole feature (OS-206). WithLibrary below changes which outfit is
        // active, so a snapshot taken next to the refresh would be the new
        // state compared against itself and ChangedMask would return zero on
        // every swap. The failure is silent: the outfit still changes and the
        // flourish simply never plays.
        auto* const requipPlayer = RE::PlayerCharacter::GetSingleton();
        const auto  requipBefore =
            OS::RequipDiff::SnapshotActive(session.ActiveOutfitFor(requipPlayer));
        // The outfit the cycle landed on, copied out under the lock. Empty means
        // it landed on Base gear. Needed because the look push must be called
        // OUTSIDE WithLibrary: it touches engine state and the callback runs with
        // the session lock held.
        std::optional<Outfit> nowActive;
        session.WithLibrary([&](OutfitLibrary& lib) {
            const auto count = lib.Count();
            if (count == 0) {
                return;
            }
            const int before = lib.ActiveIndex();
            const int next   = lib.CycleIncludingEquipped(true);
            changed          = next != before;
            if (next < 0) {
                name = "Base gear";
            } else if (const auto* o = lib.At(static_cast<std::size_t>(next))) {
                name = o->name;
                nowActive = *o;
            }
        });
        if (changed && !name.empty()) {
            // The quick-switch changes which outfit the player is showing without
            // ever going through the editor's staging paths, so it has to state
            // the player's LOOK itself: colour and head parts are pushed into
            // the actor base, not pulled out of the display set the way hair
            // visibility is. Before the refresh, which is what carries it to the
            // screen - the same order every staging path in OutfitSession.cpp
            // keeps.
            //
            // ⚠⚠ THE WHOLE LOOK, THROUGH THE SESSION'S ONE DOOR, AND NOT THE
            // COLOUR BY HAND (OS-229). This called HairColor::Push directly,
            // which was the whole look when it was written, and every dimension
            // added since (hair style, eyes, brows, facial hair, horns) never
            // learned about the hotkey: field 2026-08-18 22:51, seven
            // quick-switches, seven colour pushes, not one hair style, and the
            // head only caught up when the editor opened. The ladder still
            // decides inside: cycling back to Base gear is the no-outfit case,
            // and a character with defaults keeps them there.
            session.PushPlayerLookFor(nowActive ? &*nowActive : nullptr);
            // OS-206: the refresh is handed to the flourish rather than called,
            // so it lands at the peak of the burn instead of at the start of it.
            // Requip::Begin runs it immediately when the feature is off, when
            // nothing visible changed, or when no shape could be armed, so the
            // outfit change cannot be swallowed by the effect.
            OS::Requip::Begin(requipPlayer,
                              OS::RequipDiff::ChangedMask(
                                  requipBefore, OS::RequipDiff::SnapshotActive(nowActive)),
                              [] { OutfitSession::RequestRefresh(); });
            RE::SendHUDMessage::ShowHUDMessage(("Outfit: " + name).c_str());
            spdlog::info("quick-switch: '{}' activated.", name);
            // The hotkey is a hand pick just like a tab click - pin. Always
            // the player: CycleOutfit drives the player's own OutfitSession
            // and RE::PlayerCharacter directly, never an editor NPC target.
            // Use the real outfit name (empty for "landed on Base gear"),
            // not the "Base gear" display string used in the notification.
            OS::WorldWatch::NotifyManualPick(nowActive ? nowActive->name : "");
        }
    }

    RE::BSEventNotifyControl InputListener::ProcessEvent(
        RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) {
        if (!a_events) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto& overlay = ImGuiOverlay::GetSingleton();
        for (auto* e = *a_events; e; e = e->next) {
            const auto* btn = e->AsButtonEvent();

            if (btn && btn->IsDown()) {
                const auto device = btn->GetDevice();
                const auto code   = btn->GetIDCode();
                // OS-207: enter the editor with no menu open. ⚠ BEFORE THE
                // EDITOR KEY'S OWN GATE AND SEPARATE FROM IT. That gate refuses
                // unless a host menu is already up, which is the one thing this
                // key exists to do something about, so sharing it would mean
                // teaching it a second answer for the same question.
                if (device == RE::INPUT_DEVICE::kKeyboard && directKey_ &&
                    code == directKey_) {
                    OS::DirectEntry::Fire();
                    continue;
                }
                // W1 spike. Unbound in every shipped INI, so this line costs
                // one comparison against zero; the probe's own log lines are
                // its whole interface.
                if (device == RE::INPUT_DEVICE::kKeyboard && profileProbeKey_ &&
                    code == profileProbeKey_) {
                    OS::ProfileProbe::OnKey();
                    continue;
                }
                const bool editorHit =
                    (device == RE::INPUT_DEVICE::kKeyboard && editorKey_ && code == editorKey_) ||
                    (device == RE::INPUT_DEVICE::kGamepad && editorPad_ && code == editorPad_);
                if (editorHit) {
                    // Opening requires a permitted context: a vanilla menu the
                    // editor is composed around (it needs the Show-Player-In-
                    // Menus character behind it) OR Screen Archer Menu
                    // (screenarchery). Lore mode adds the Seamstone requirement
                    // on top. Closing is allowed any time except while typing
                    // (the key is a printable letter).
                    //
                    // ⚠ THE LIST LIVES IN HostGuard AND NOWHERE ELSE. This was
                    // one of six copies of the same two lines, and the close
                    // gate's job is to agree with this one exactly - the two
                    // ways they can disagree are an editor that shuts itself the
                    // frame it opens and a world camera nailed to a follower
                    // while the player has gameplay control.
                    auto* ui = RE::UI::GetSingleton();
                    const bool canOpenHere = HostGuard::HostMenuOpen();
                    // ⚠ BOTH HALVES, AND THE MENU NAME IS THE ONE THAT CANNOT
                    // FAIL. IsMenuOpen(Console) is safe on either runtime and
                    // covers the case that was reported first; it stays.
                    //
                    // ⚠ IT WAS NEVER ENOUGH ON ITS OWN, though, and a menu-name
                    // list could not be made enough: a text field is a thing
                    // INSIDE a menu, so typing a name into Screen Archer Menu
                    // left SAM open, the context permitted, and a printable
                    // hotkey free to open the editor over what was being typed
                    // (user 2026-08-11). TextEntry::Active asks the engine's own
                    // counter instead, which every menu that takes text raises,
                    // and it locates that counter rather than trusting the
                    // offset CommonLib declares - see TextEntry.h for why that
                    // offset is wrong here and why being wrong about it reads
                    // as healthy. It answers false when it cannot read the
                    // counter, so this line is never worse than the one it
                    // replaces.
                    const bool gameWantsText =
                        (ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) ||
                        TextEntry::Active();
                    // Through LoreModule now rather than composed here. It was
                    // the only site that composed it, which is how the button
                    // and the Papyrus path came to check nothing.
                    const bool seamstoneOk = LoreModule::GateSatisfied();
                    // The editor hotkey opens/closes the FUCK IWindow (Phase 3).
                    switch (EditorGate::DecideGate(EditorWindow::IsOpen(),
                                                   EditorWindow::WantsTextInput(),
                                                   canOpenHere, seamstoneOk,
                                                   gameWantsText)) {
                        case EditorGate::GateAction::kClose:
                        case EditorGate::GateAction::kOpen:
                            EditorWindow::Toggle();
                            break;
                        case EditorGate::GateAction::kNeedContext:
                            // ⚠ SILENT ON PURPOSE (user 2026-08-11), the same
                            // call kNeedSeamstone got below and for the same
                            // reason. This used to put "Open your inventory or
                            // Screen Archer Menu to enter Fitting Room." in the
                            // corner, which is the mod talking about itself on
                            // a key the player may have pressed by accident,
                            // and it fired on every stray press for the whole
                            // life of the save rather than only while the
                            // binding was new.
                            //
                            // With nothing drawn the key does nothing outside a
                            // permitted context, which is what an unbound key
                            // does everywhere else in the game.
                            //
                            // ⚠ THE LOG LINE STAYS, for kNeedSeamstone's
                            // reason: "the hotkey does nothing" and "the key
                            // never arrived" are opposite faults and the log is
                            // the only thing that tells them apart.
                            spdlog::info("editor hotkey: refused, neither the inventory "
                                         "nor Screen Archer Menu is open. Nothing shown, "
                                         "by design.");
                            break;
                        case EditorGate::GateAction::kNeedSeamstone:
                            // ⚠ SILENT ON PURPOSE (user 2026-08-07). This used to
                            // put "You need a seamstone. Farengar of Dragonsreach
                            // sells one." on screen, which TELLS a player about an
                            // item and a merchant they may never have met, from a
                            // key they may have pressed by accident. That is the
                            // opposite of lore friendly: it is the mod announcing
                            // itself and handing out a quest marker in a corner
                            // notification.
                            //
                            // A player who has never found the Seamstone should
                            // not learn it exists this way. With nothing drawn,
                            // the key simply does nothing, exactly as it would if
                            // the mod were not installed, and the item stays
                            // something you discover in the world.
                            //
                            // ⚠ THE LOG LINE STAYS, and it is not a compromise on
                            // the above: nothing in it reaches the player, and
                            // without it a field report of "the hotkey does
                            // nothing" cannot be told apart from the key never
                            // being seen at all, which are opposite faults.
                            spdlog::info("editor hotkey: refused, the lore module is "
                                         "active and the player is not carrying the "
                                         "Seamstone. Nothing shown, by design.");
                            break;
                        case EditorGate::GateAction::kIgnore:
                            // ⚠ DEBUG, NOT INFO, AND THAT IS THE HOLD-DOWN
                            // CASE. This fires once per press and a player
                            // typing a name presses the letter as often as the
                            // name contains it, so an info line here would bury
                            // the log in the exact session it exists to explain.
                            // It still separates "the key was eaten because you
                            // were typing" from "the key never arrived", which
                            // is the pair no other line tells apart.
                            if (gameWantsText) {
                                spdlog::debug("editor hotkey: ignored, the game is "
                                              "taking typed characters.");
                            }
                            break;
                    }
                    continue;
                }
            }

            // ⚠ NO STICK HANDLING HERE, AND THAT IS MEASURED RATHER THAN
            // ASSUMED. A ThumbstickEvent arm was written in this loop on
            // 2026-08-15 and logged nothing at all over a full session with the
            // editor open on a pad, because FLICK owns input while its window is
            // up and this sink is starved for the duration - the same fact the
            // OS-80 note at the top of this file records for the mouse. The
            // sticks are polled off the device instead; see PollEditorSticks.

            // OS-73 camera drag, editor-only. Latch on the LMB edges, apply on
            // mouse move. This sink is the raw device feed, upstream of the
            // menu control map, so it still sees both while a menu context owns
            // input - which is the whole reason the free camera can be driven
            // from here at all. Everything below is inert with the editor shut.
            if (EditorWindow::IsOpen() && Settings::GetSingleton().cameraDragWhileOpen) {
                if (btn && btn->GetDevice() == RE::INPUT_DEVICE::kMouse &&
                    btn->GetIDCode() == 0) {
                    if (btn->IsDown()) {
                        // OS-80c: when SAM framed the shot it OWNS the camera.
                        // SAM orbits by writing translation AND rotation on the
                        // very FreeCameraState object this drag writes pitch/yaw
                        // into, so arming here is a second writer racing SAM on
                        // +0x3C/+0x40 every mouse-move. That is field-proven, not
                        // theory: the 2026-07-19 02:46 log shows every gate PASSED
                        // and the write happening, and the shot still felt "not at
                        // all like SAM". Decline instead; kPassInputToGame already
                        // hands the gesture to SAM, now uncontested.
                        const bool samOwnsCamera = EditorWindow::OpenedFromSam();
                        const bool overUI        = EditorWindow::CursorOverUI();
                        worldDrag_               = !overUI && !samOwnsCamera;
                        // OS-80: a fresh gesture, so let the camera path speak
                        // once more. This line also proves the sink SEES the
                        // click at all - if it is ABSENT from a failing session
                        // then the input-block hook consumed the event upstream
                        // and every camera gate below is innocent.
                        g_dragDiagDone = false;
                        spdlog::info("camera drag: LMB down, cursorOverUI={} -> {}.",
                                     overUI,
                                     worldDrag_      ? "ARMED"
                                     : samOwnsCamera ? "declined (SAM owns the camera)"
                                                     : "declined (click was over a panel)");
                    } else if (btn->IsUp()) {
                        worldDrag_ = false;
                    }
                } else if (worldDrag_ &&
                           e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    if (const auto* move = static_cast<const RE::MouseMoveEvent*>(
                            e->AsIDEvent())) {
                        ApplyFreeCameraDrag(static_cast<float>(move->mouseInputX),
                                            static_cast<float>(move->mouseInputY));
                    }
                }
            } else {
                worldDrag_ = false;  // never resume a drag across a close
            }

            if (overlay.IsOpen()) {
                // Fallback feed for the one frame before the input-block hook
                // installs (first Present after first open). Once installed, the
                // hook empties the event list before this sink runs, so while
                // open this sink receives nothing and the real feed happens in
                // ImGuiOverlay::HandleModalInput. See InputDispatchHook.
                overlay.FeedEvent(e);
                continue;
            }

            if (!btn || !btn->IsDown()) {
                continue;
            }
            if (btn->GetDevice() == RE::INPUT_DEVICE::kKeyboard && nextKey_ &&
                btn->GetIDCode() == nextKey_) {
                CycleOutfit();
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace OS
