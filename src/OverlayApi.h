#pragma once

#include "OverlayPlan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

// RaceMenu's Overlay and Override interfaces, for the Overlays page.
//
// ⚠ TWO INTERFACES, NOT ONE, AND THE PAGE NEEDS BOTH. Overlay owns the slots:
// how many there are, what their nodes are called, and installing them on a
// character. Override owns what a slot looks like: its texture, its tint and its
// alpha are node overrides, not overlay calls. Either one missing leaves the
// page unable to work, so acquisition treats them as a pair.
//
// ⚠ BOTH ARE REGISTERED, CHECKED AGAINST THE SHIPPED BINARY. skee's main.cpp
// calls AddInterface for eleven names and "Overlay" and "Override" are the first
// two; the eleven also sit in the string pool of the installed skee64.dll
// 0.4.20.0. This is the check NodeTransformApi.h asks every new interface to
// make, after FacePresetApi was built on a declared but unregistered one.
//
// ⚠ NOTHING HERE TOUCHES A KEY OTHER MODS WRITE. A node override is stored per
// node and per key rather than under a named owner the way body morphs and
// transforms are, so there is no key to scope. What takes its place is that the
// page only ever addresses overlay nodes it got from GetOverlayFormat, and only
// the three keys OverlayPlan names. It never enumerates or clears a node it did
// not compose itself.
namespace OS::OverlayApi {

    // Why the page cannot work, so it can say something true rather than blame
    // an absent RaceMenu for a RaceMenu that answered. Same distinction
    // RaceMenuMorphApi draws, and for the same field reason: another mod's
    // bundled skee64.dll winning the overwrite looks nothing like RaceMenu
    // being missing, and no message about installing it would help.
    enum class Status : std::uint8_t {
        kNotRequested,
        kNoMessaging,
        kRaceMenuAbsent,   // no interface map came back
        kNoOverlay,        // it answered, with no Overlay entry
        kNoOverride,       // it answered, with no Override entry
        kTooOld,           // it answered, below the floor
        kOverlaysDisabled, // it answered, and the user has overlays turned off
        kReady,
    };

    // Acquire both interfaces. Call once at kPostPostLoad: skee does not answer
    // earlier, and requesting from kPostLoad silently gets nothing.
    void Request();

    [[nodiscard]] Status        GetStatus();
    [[nodiscard]] bool          Available();
    [[nodiscard]] std::uint32_t OverlayVersion();
    [[nodiscard]] std::uint32_t OverrideVersion();

    // The layer list this install actually has, read through GetOverlayCount and
    // GetOverlayFormat.
    //
    // ⚠ READ FROM THE USER'S skee64.ini AND CACHED ONCE. The counts cannot
    // change without a restart, and the page asks for them every frame.
    [[nodiscard]] const std::vector<OverlayPlan::Layer>& Layers();

    // Whether the character has overlay nodes at all, and putting them there if
    // not.
    //
    // ⚠ INSTALLING IS NOT FREE AND IS NOT DONE ON A HUNCH. AddOverlays clones
    // the skin geometry once per slot, so it runs when the page is opened on a
    // character that has none, and never per frame or per edit.
    [[nodiscard]] bool HasOverlays(RE::Actor* a_actor);
    void               Install(RE::Actor* a_actor);

    // Ask skee to call us back whenever it installs an overlay node.
    //
    // OverlayReconcile (OS-233) hangs off that callback: it arms its repair
    // when the fire is a Face install on the player, because that fire IS
    // RaceMenu's post-load pass, one frame after it has taken the body clones
    // and failed to rebuild them. That is why the registration is
    // unconditional.
    //
    // ⚠⚠ CALL AT kDataLoaded, NOT FROM Request. Request runs at kPostPostLoad,
    // which is where skee first answers, and Settings::Load runs at
    // kDataLoaded. An earlier version of this asked an INI question here and
    // the field round of 2026-08-18 20:51 logged `registration not attempted`
    // with the key plainly bound, because it was asked one message too early.
    //
    // A no-op unless the interfaces are ready.
    void               RegisterInstallCallback();

    // What one layer currently holds.
    //
    // ⚠ READS ON THE CALLING THREAD, unlike every write below. These are map
    // lookups in skee's own store rather than scenegraph work, and the page
    // needs them in the frame it opens; a marshaled read arrives a frame after
    // the controls were already drawn from defaults, which is the flicker the
    // hair colour picker used to have.
    [[nodiscard]] OverlayPlan::LayerState Read(RE::Actor*         a_actor,
                                               const std::string& a_node);

    // One layer's complete appearance, written as node overrides and then
    // pushed with a single SetNodeProperties.
    //
    // ⚠ AN OVERRIDE AND NEVER A DIRECT SHADER WRITE, and the normal is the
    // reason this is stated rather than assumed. Overlay install and
    // ResetOverlay both copy texture slots 1 through 8 back off the skin
    // material, so anything written straight onto the shader is wiped the next
    // time either runs. An override survives because skee reapplies it, which
    // also keeps one painter of this appearance rather than two.
    //
    // ⚠ SAFE TO CALL FROM THE RENDER THREAD. The editor draws through FUCK's
    // Present hook, so the work is marshaled onto the game thread by handle, as
    // NodeTransformApi::Apply does. Nothing here dereferences the actor on the
    // calling thread.
    //
    // ⚠ A MOVED LAYER (OS-209, a non-identity `transform`) IS WRITTEN LATER, NOT
    // NOW: key 9 gets the BAKED path, and that goes in only once OverlayBake
    // says the file exists, which is at once for a bake already on disk and
    // after a render-thread build and a worker-thread write for a new one. The
    // rest of the layer goes in with it. Read decodes the baked path back.
    void Write(RE::Actor* a_actor, const std::string& a_node,
               const OverlayPlan::LayerState& a_state);

    // Put a layer back to empty: our three keys removed, and the diffuse set to
    // the texture skee itself uses for a reset slot.
    //
    // ⚠ NOT RevertOverlay, DELIBERATELY. That call needs an armor and addon
    // mask for body, hands and feet, and a partType and shaderType for the
    // face, none of which the page has a measured value for. Removing the
    // overrides we wrote and restoring the default diffuse is the same visible
    // result through calls whose arguments are known. Revert stays available for
    // whoever measures those masks later.
    void Clear(RE::Actor* a_actor, const std::string& a_node);

    // Reapply every node override the character holds, once, with nothing
    // written first.
    //
    // ⚠ FOR A CALLER THAT CHANGED THE SCENEGRAPH RATHER THAN THE STORE. Every
    // write above ends with its own push, so nothing that edits a layer needs
    // this. Overlay1P does: it adds `[Ovl]` nodes skee has never painted, and
    // skee's walk finds them by name on both roots and puts the stored
    // appearance on them. Deferred, like every other push here.
    void PushNodeProperties(RE::Actor* a_actor);

    // Coalescing form of the push above, for a caller that repaints the SKIN.
    //
    // ⚠⚠ THE ENGINE REPAINTS OUR OVERLAYS WITH THE BODY COLOUR AND SKEE'S
    // STORE NEVER HEARS ABOUT IT. skee clones the skin geometry to carry a
    // layer and the clone keeps the SKIN flags, so `Actor::UpdateSkinColor`
    // walks it along with everything else the body wears and puts
    // `bodyTintColor` on it. The layer's own colour is still correct in the
    // store and in the editor, which is exactly why touching that colour in the
    // page appears to fix it: the write ends with a push of its own. Field
    // 2026-08-26, at 23:59: change the skin tone and every body overlay follows
    // it while the page still shows the colour the layer is supposed to have.
    //
    // So a skin repaint asks for the stored appearance to be put back after the
    // engine has finished. ⚠ COALESCING, because a colour drag repaints on
    // every frame and skee's push is a whole scenegraph walk.
    void ArmNodePropertyPush(RE::Actor* a_actor);

    // Run the armed push if it is due. Called from the world tick, and silent
    // unless a skin repaint armed it.
    void RunNodePropertyPush();

    // Silence the push for a_milliseconds and drop anything already armed,
    // because something that outranks skee's store has just written the layers.
    //
    // ⚠⚠ THE CALLER IS A LOOK APPLY AND THE REASON IS ORDER, NOT CORRECTNESS.
    // The push re-asserts what skee holds; the overlays step decides what the
    // layers should be. Both are right about their own question and the apply
    // armed the push through its skin step one line before the overlays step
    // ran, so the stale answer landed last. See
    // OverlayPlan::kNodePushQuietAfterApplyMs for the log that measured it.
    //
    // ⚠ NOT A WAY TO TURN THE PUSH OFF. A skin-tone drag still needs it, which
    // is the report it was written for, so this is a window and never a flag.
    void StandDownNodePush(int a_milliseconds);

}  // namespace OS::OverlayApi
