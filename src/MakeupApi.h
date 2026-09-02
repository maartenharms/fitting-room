#pragma once

#include "MakeupPlan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

// The player's tint mask layers, which is what makeup actually is.
//
// ⚠⚠ THE PLAYER AND NOBODY ELSE, AND THE ENGINE DREW THAT LINE BEFORE WE DID.
// An NPC's tint layers live on the ACTOR BASE, so editing a follower's would
// edit a TESNPC record other actors may share and would persist into the save in
// a way nothing else in this mod does. The player carries its OWN list instead,
// on the live object, and no base record is touched to read or write it. User's
// call 2026-08-16, and the engine agrees with it: only the player has this list.
//
// ⚠⚠ THE LIST OFFSET IS NOT THE ONE CommonLibSSE-NG PUBLISHES, AND THIS IS THE
// TRAP THIS FILE EXISTS TO HOLD. PlayerCharacter.h declares tintMasks at 0xB10
// for both runtimes, inside a block guarded only by ENABLE_SKYRIM_VR. MEASURED
// in Ghidra, from two independent engine functions on each build:
//
//   SE 1.5.97  GetTintMask 39612 / GetNumTints 39614:  data 0xB10, size 0xB20
//   AE 1.6.1170 GetTintMask 40698 / GetNumTints 40700: data 0xB18, size 0xB28
//
// and a third AE site, the character dump that prints "TintMask%d:%08X:%f",
// reads the same 0xB18. So the published member is the SE layout and is eight
// bytes early on AE, which is the runtime this mod is developed against. Using
// it directly would read a neighbouring field as an array pointer.
//
// ⚠ CommonLibSSE-NG ALSO HAS NO TintMask DEFINITION. The class is a forward
// declaration in PlayerCharacter.h and TESNPC.h and nowhere else, so the layout
// below is ours and is measured rather than borrowed. TESNPC::Layer, which does
// have a definition, is the RECORD form and a different struct entirely: it
// carries a tint index and a preset and no texture at all.
namespace OS::MakeupApi {

    // Why the page cannot work, so it can say something true. Same shape
    // OverlayApi::Status uses.
    enum class Status : std::uint8_t {
        kNoPlayer,      // no player singleton yet
        kNotThePlayer,  // the edit target is a follower, which has no such list
        kEmptyList,     // the player has one and it is empty
        kReady,
    };

    [[nodiscard]] Status GetStatus(RE::Actor* a_actor);

    // Whether this actor is the one actor that has a tint list.
    [[nodiscard]] bool IsPlayer(RE::Actor* a_actor);

    // The slots this character actually has, in list order.
    //
    // ⚠ NOT CACHED ACROSS CHARACTERS AND NOT ASSUMED TO BE FIFTEEN. The list is
    // built from the race, so its length is race dependent and the measured
    // presets run past index 24. A layer list built from kTypeCount would hide
    // whatever a race adds beyond it.
    [[nodiscard]] std::vector<MakeupPlan::Layer> Layers(RE::Actor* a_actor);

    // Every layer's current appearance, read straight off the live masks.
    //
    // ⚠ READS ON THE CALLING THREAD, the same call OverlayApi::Read makes and
    // for the same reason: these are plain field reads rather than scenegraph
    // work, and a marshaled read arrives a frame after the controls were drawn
    // from defaults. Nothing here allocates or mutates.
    [[nodiscard]] MakeupPlan::Snapshot Read(RE::Actor* a_actor);

    // Write a batch of layers and re-tint the face ONCE, at the end.
    //
    // ⚠⚠ THE RETINT IS A REBAKE AND NEVER GOES INSIDE THE LOOP. MEASURED:
    // RELOCATION_ID(51521, 52396), found by the string "Player face tint" which
    // it assigns as the name of the texture it BUILDS. It walks the player's
    // head parts for the one whose type field at +0x6C is 1, reaches its
    // geometry's shader property and hands it a rebuilt texture. Every one of
    // the engine's own tint natives follows a write with exactly one of these.
    // Calling it per slider frame would rebuild a texture per frame, which is
    // the same rule OverlayApi::Write holds for SetNodeProperties.
    //
    // ⚠ SAFE TO CALL FROM THE RENDER THREAD. The editor draws inside FLICK's
    // Present hook, so this marshals onto the game thread by handle. Nothing
    // here dereferences the actor on the calling thread.
    //
    // a_indices are positions in the list Layers() returned. Indices the list
    // does not have are skipped rather than refused, because a target switch can
    // land a stale index here.
    void Write(RE::Actor* a_actor, const std::vector<std::size_t>& a_indices,
               const MakeupPlan::Snapshot& a_state);

    // Put the whole list back to a snapshot, writing only what differs.
    void Restore(RE::Actor* a_actor, const MakeupPlan::Snapshot& a_from,
                 const MakeupPlan::Snapshot& a_to);

    // Repaint the BODY from whatever the live list's skin tone layer holds now.
    //
    // ⚠⚠ TWO PAINTERS OF ONE APPEARANCE. The SkinTone layer feeds the FACE
    // bake; the body reads TESNPC::bodyTintColor through Actor::UpdateSkinColor,
    // and nothing keeps them in step by itself. Write() does this for a batch it
    // wrote; this is for the case where nobody wrote at all and the list changed
    // underneath us, which is exactly what a look with a race switch does: skee
    // installs a whole tint list of its own and the body is left on the colour
    // the engine's race change painted from the OLD race's slot.
    //
    // ⚠ Marshals onto the game thread by handle, like Write. Safe from the
    // render thread; a no-op for anyone but the player.
    //
    // ⚠⚠ IT STANDS ASIDE WHEN THE LIVE LIST IS NOT THIS RACE'S. A list left
    // over from another race carries that race's skin tone, and painting the
    // body from it is how a Nord's white ends up under a dark elf head.
    void SyncBodyToSkinTone(RE::Actor* a_actor);

    // Paint the body from a colour the caller already has, with no live list
    // involved at all.
    //
    // ⚠⚠ THIS IS THE ROUTE ACROSS A RACE SWITCH. MEASURED r37: the live tint
    // list stays the OLD race's, so it has no slot the new skin tone belongs
    // in, while the FACE is right anyway because skee's preset apply paints it
    // from the preset's own baked tint texture. The captured skin tone in the
    // look is the only honest source for the body, and SetSkinFromTint reads
    // nothing but the colour and the alpha, so a scratch mask carries it.
    void PaintBodyFromTint(RE::Actor* a_actor, const OverlayPlan::Rgb& a_tint,
                           float a_strength);

    // ---- holding a skin tone against the head build -------------------------
    //
    // ⚠⚠ A HEAD BUILD RESTORES THE TINT LIST AND TAKES THE BODY WITH IT.
    // MEASURED r39: the captured tone holds at +0.25 s, +0.5 s and +1 s and is
    // gone by +2 s, on the far side of a head build, with the live slot back at
    // alpha 255 and the body back to the old race's colour. The character's own
    // saved tint layers are what it is restored from, so a single write can
    // never be the whole answer. This is the same posture the hair colour has
    // had since July: hold the value and put it back after each build.
    void HoldSkinTone(const OverlayPlan::Rgb& a_tint, float a_strength);
    void ReleaseSkinTone();  // a load restores the character's own colour
    [[nodiscard]] bool HoldsSkinTone();
    // What is held, for the co-save. False when nothing is, and the outputs are
    // untouched then.
    //
    // ⚠⚠ THE HOLD USED TO BE SESSION STATE AND THAT IS WHY A LOAD CAME BACK
    // WHITE. Field 2026-08-25 08:00: a plain load with no look applied, the
    // save's own tint list arriving correct at `skintone=(136,177,198)`, a head
    // build 27 s later, and the body back to the vanilla race's (167,134,122)
    // for the rest of the session. The reassert directly below was already
    // wired to that build and already ran; it stood down without a word because
    // `HoldsSkinTone()` was false, since only a look apply ever set it and the
    // revert had cleared it. The machinery was right and had nothing to hold.
    [[nodiscard]] bool HeldSkinTone(OverlayPlan::Rgb& a_tint, float& a_strength);
    // ⚠ MAIN THREAD, from the head-build hook. Cheap and silent when the held
    // value is already the one on the mask.
    void ReassertSkinTone(RE::Actor* a_actor);

    // ⚠⚠ A REASSERT CANNOT ANSWER THIS ONE, AND THAT IS THE WHOLE REASON THIS
    // EXISTS. ReassertSkinTone puts the colour back in the tint MASK and repaints
    // bodyTintColor; neither is what the head wears. The head wears a baked
    // TEXTURE, and when skee re-binds its preset file over that slot the mask is
    // still perfectly correct, so the reassert early-returns and the head stays
    // wrong forever. Compare the identity of what is bound, not the value of
    // what fed it.
    //
    // ⚠⚠ NO HEAD BUILD FIRES FOR THE FLIP THIS WATCHES, AND THAT IS A
    // NARROWER CLAIM THAN IT FIRST READ. Field 2026-08-26 20:52: the bake is
    // displaced with the material pointer STEADY and no head build on either
    // side, which is a re-bind and is what this trigger is for. The same log's
    // 20:54 round is a different fault wearing the same symptom: eight rebuilds,
    // the material pointer CHANGING every time, and a HeadBuildHook line within
    // a second of every one. ArmFaceRebake owns that half and is not blind to
    // it; read mat= in the HEAD TEXTURE line to tell the two apart before
    // reaching for either. This is the second trigger, and it watches instead of
    // waiting to be told.
    //
    // Arming is COALESCING: each call pushes the check out again, so a slider
    // drag costs one rebake after it settles rather than one per frame. The
    // delay exists because OverlayFix pushes skee's work onto DELAYED tasks, so
    // the re-bind lands about a second after the edit that provoked it.
    void ArmDelayedFaceRebake();

    // The head-build edge, and it does NOT require a held tone. A build
    // discards the tint composite outright, and what it discarded was the
    // character's OWN face: rebuilding it from the live tint list is what a
    // fresh start would have done anyway, which is the same reasoning
    // RebakeFaceTint already carries for the load edge.
    //
    // ⚠⚠ THE ONE SHOT A LOAD OWES CANNOT COVER THIS. Field 2026-08-26
    // 20:54: the owed rebake landed on the first build after the load and the
    // face was right, then EIGHT more builds followed over three and a half
    // minutes, each with a new material pointer and an unnamed tint, and the
    // debt was long spent. The face carried no composite at all from 20:54:06
    // to the end of the session.
    void ArmFaceRebakeAfterHeadBuild();

    // Called from the world tick. Does nothing at all unless armed, rebakes only
    // when the head is measurably no longer wearing our bake, and gives up after
    // a bounded number of looks so it can never sit in a fight with skee.
    void RunDelayedFaceRebake(RE::Actor* a_actor);

    // Disarm the face watch and keep it disarmed for a_milliseconds.
    //
    // ⚠⚠ FOR A CALLER THAT PUT THE FACE THERE ON PURPOSE. The watch judges a
    // face by the name in its tint slot, and a look the user just imported and a
    // preset skee re-bound behind our back are the same name in the same slot.
    // Only the caller knows which of the two it is, so the deliberate one says
    // so. MakeupPlan::kFaceWatchQuietAfterApplyMs is the measured window.
    void StandDownFaceWatch(int a_milliseconds);

    // ---- the character editor's trip, for the held tone ---------------------
    //
    // ⚠⚠ RACEMENU TAKES THE SKIN COLOUR AND NOTHING GAVE IT BACK (user
    // 2026-08-24: "if i load a looks preset and then later open Racemenu the
    // skin color can get overwritten by racemenu"). HeadBuildHook stands the
    // re-assert down for the whole time that menu is open, which is right: the
    // editor's own slider drives the same engine painter and two writers would
    // fight. But the editor's head build restores the tint list from the
    // character's saved layers, so the look's tone is gone by the time it
    // closes, and the close edge re-asserted the hair and the head parts and
    // never this.
    //
    // ⚠⚠ UNCONDITIONAL, AND AN OPEN-EDGE CAPTURE WAS TRIED AND IS WRONG. The
    // appealing version samples the live slot at open, compares at close, and
    // treats a CHANGED slot as a colour the player picked in the editor so
    // theirs wins. It cannot work, and the reason is the same head build this
    // whole function exists for: the editor's own rebuild restores the tint
    // list from the character's saved layers, which moves the slot with no
    // player input at all. At open the slot holds OUR tone, because the
    // re-assert has been putting it there after every build; at close it holds
    // the character's own. "Changed" is therefore the ORDINARY case, the
    // comparison would drop the hold every time, and the bug it was meant to
    // refine would survive untouched.
    //
    // So this matches the hair: what Fitting Room drives, Fitting Room puts
    // back. The known cost is that a skin colour picked inside the editor while
    // a look's tone is held gets reverted on close. Telling those two apart
    // needs a signal nobody has yet, not a cleverer comparison of the two we
    // have.
    //
    // A no-op when nothing is held. ⚠ MAIN THREAD, from the menu sink.
    void NoteHeadEditorClosed(RE::Actor* a_actor);

    // Mirror the live tint list into the character's SAVED tint layers, the
    // copy on the player's own actor base that the character editor's init
    // rebuilds the list from and the vanilla save carries.
    //
    // ⚠⚠ THE THIRD COPY, AND THE ONE THAT PUT UMBRAEL'S MAKEUP BACK ON NORD 3
    // THE MOMENT RACEMENU OPENED (field 2026-09-02 01:47). MakeupPlan's
    // PlanSavedLayers carries the Ghidra measurement. Called at the end of
    // every write batch and after the tone re-assert; stands aside while the
    // character editor is open, because RaceMenu owns the list in there, and
    // the close edge passes a_editorClosing because the menu still reports
    // itself open inside its own close event. Player only. ⚠ MAIN THREAD.
    void SyncSavedLayers(RE::Actor* a_actor, bool a_editorClosing);

    // ⚠ INSTRUMENTS, debug level, one line each. The worn entries of the live
    // list with its origin, and the base's saved layers that carry strength.
    // Placed on the character editor's edges and its head builds on
    // 2026-09-02 to say WHEN a previous character's makeup re-enters the
    // list and in WHICH list, after the saved-layers mirror did not stop it.
    void DumpWorn(RE::Actor* a_actor, const char* a_where);
    void DumpSavedLayers(RE::Actor* a_actor, const char* a_where);

    // The worn-set watch: the worn set of the live list, read every heartbeat
    // and logged the moment it changes.
    //
    // ⚠⚠ AND SINCE 03:1x IT ANSWERS, because the writer it caught is RaceMenu
    // replaying a per-actor copy of the tints by index about a second after
    // every load and every LoadCharacterEx, a copy no write of ours reaches.
    // A change that was not ours puts the record back, unless an apply is in
    // flight (its own steps own the list) or the character editor is open
    // past its first three seconds (the user owns the list in there; the first
    // seconds belong to the editor's init, which is the same replay). Our own
    // writes refresh the watch through NoteOwnWrite so they never trigger it.
    // ⚠ MAIN THREAD, from WorldWatch's heartbeat.
    void NoteHeartbeat(RE::Actor* a_actor);
    void NoteOwnWrite(RE::Actor* a_actor);
    void NoteEditorOpened();

    // The character editor closed on this list: it is remembered as RaceMenu's
    // copy, because the copy IS the list at the last RaceSexMenu close
    // (measured 03:43 to 03:44), and the watch tells that copy's replay inside
    // the next visit from the user's own preset load by comparing content.
    // ⚠ MAIN THREAD, after the close edge's own writes.
    void NoteEditorClosed(RE::Actor* a_actor);

    // Rebuild the player's live tint list to the current race's slots, the way
    // the character editor's init does it, through the engine's own function
    // on both builds (SE 39610 / AE 40696). Runs when this apply switched race
    // or sex, or when the list is another race's length
    // (MakeupPlan::NeedsRaceRebuild); returns whether it ran.
    //
    // ⚠⚠ THE r35 CURE, MEASURED 2026-09-02 03:44:42: a race-switching apply
    // left the Nord's 34 slot list on a 108 slot race for the whole session,
    // so the makeup step, the body sync and the record all refused to write
    // into it and the look had no makeup until RaceMenu opened and its init
    // rebuilt the list. ⚠ DESTRUCTIVE ON PURPOSE: afterwards every mask wears
    // the race's own art at its default, our owned textures included, which is
    // why it runs inside an apply, after the race is committed and before the
    // makeup step writes. Player only, game thread.
    bool RebuildListForRace(RE::Actor* a_actor, bool a_switched);

    // Rebuild the player's baked face tint texture from the live tint list.
    //
    // ⚠⚠ THE CROSS-SAVE BLEED IS THIS TEXTURE. r39: after loading an earlier
    // save the tint list holds that character's own colour, skee's overlay
    // store is empty and the head's texture set names the vanilla head file,
    // and the face still wears the abandoned look. The baked texture is the
    // only store left, and nothing rebuilds it on a load.
    void RebakeFaceTint();
    void ArmFaceRebake();      // at kPostLoadGame; also opens the 20 s window
    void RunOwedFaceRebake();  // from the head-build hook, so it lands after the build
    void CancelFaceRebake();   // an apply owns the face; debt and window are void
    // True while the post-load window is open: late head builds that re-bind a
    // preset tint FILE for the wrong race still owe a rebake (r47).
    [[nodiscard]] bool FaceRebakeWindowOpen();

    // Which of the player's two tint lists the reads above are using and how
    // long each one is, as a phrase for a log line. See MakeupApi.cpp: the
    // engine prefers an installed overlay over the base array, and the two
    // disagreeing after a race switch is the whole of the mismatch bug.
    [[nodiscard]] std::string ListOrigin(RE::Actor* a_actor);

    // A cheap identity of the live list: its length folded with every mask
    // pointer. Same value means the same list; a changed value means something
    // rebuilt or edited it under the page.
    //
    // ⚠⚠ THE LIST IS REBUILT UNDER THE PAGE AND THAT IS A MEASURED EVENT, NOT
    // A THEORY. Field 2026-08-16: a RaceMenu preset load replaced the player's
    // tint list while the editor held a snapshot of the old one, and every
    // write after that landed on indices into a list that no longer existed.
    // The page polls this per frame while the makeup section draws; a mismatch
    // means re-read, not write.
    [[nodiscard]] std::uint64_t Fingerprint(RE::Actor* a_actor);

}  // namespace OS::MakeupApi
