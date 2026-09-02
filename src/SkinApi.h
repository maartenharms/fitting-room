#pragma once

#include "PersistenceCodec.h"  // SkinRow

#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

// The skin changer's engine half: which pack an actor wears, and the skee
// armour overrides that put it on their skin.
//
// ⚠⚠ ONE PAINTER, AND IT IS THE ATTACH OBSERVER. RaceMenu's ActorUpdateManager
// tells a registered IAddonAttachmentInterface about every armour addon the
// engine attaches to an actor, after skee itself has applied whatever it
// already stores for that addon. That is the one seam a naked body, a
// re-equipped cuirass, a save load and a cell change all pass through, so it is
// where this mod decides, per FaceGenRGBTint shape and per texture slot, whether
// the actor's pack has a file of that slot's name and writes the override if
// it does. skee stores the override, serialises it in its own co-save and
// re-applies it at every later attach of that addon, so once written this mod
// never repaints anything: it only ever adds an override that is missing.
//
// ⚠ ARMOUR OVERRIDES, NOT SKIN OVERRIDES, and SkinPlan.h carries the
// measurement: a skin override is by slot mask and would paint the body diffuse
// onto 3BA's genital shapes.
//
// ⚠ THE HEAD IS THE EXCEPTION TO "NEVER REPAINTS": it is not armour, so its
// files are NODE overrides on the face geometries by name, and skee never
// re-applies those after a head build. ReapplyHead below runs on the far side
// of every build (HeadBuildHook) and paints them back. SkinPlan.h, the head.
//
// ⚠ THIS MOD REMEMBERS WHAT IT WROTE (the 'SKIN' record), because skee cannot
// enumerate its store and offers no "put it back": Default removes exactly the
// overrides listed here and writes each slot's remembered original straight
// onto the worn shape, so neither a switch nor a reset needs the actor's 3D
// rebuilt.
namespace OS::SkinApi {

    enum class Status : std::uint8_t {
        kNotRequested,
        kNoMessaging,
        kRaceMenuAbsent,
        kNoOverride,
        kNoUpdateManager,
        kTooOld,
        kReady,
    };

    // Acquire the Override interface and the ActorUpdateManager, and register
    // the attach observer. kPostPostLoad, beside OverlayApi::Request.
    void Request();

    [[nodiscard]] Status GetStatus();
    [[nodiscard]] bool   Available();
    // A translation key naming why the feature cannot work, for the page.
    [[nodiscard]] const char* UnavailableKey();

    // The pack this actor wears per this mod's store, empty for the game's own
    // skin. Any thread.
    [[nodiscard]] std::string Current(RE::Actor* a_actor);

    // Put a pack on, or with an empty id take it off. Marshalled onto the game
    // thread by handle, so it is safe from the editor's present thread.
    //
    // What it does: rewrites every override this mod holds for the actor to the
    // new pack's file of the same name, or removes it and writes the remembered
    // original back where the new pack has no such file, then walks the worn
    // addons and adds whatever the new pack has that nothing was written for
    // yet. Addons the actor is not wearing are covered when they attach.
    void Apply(RE::Actor* a_actor, const std::string& a_packId);

    // Put the worn pack's head files back onto a head the engine has just
    // rebuilt. Game thread, on the far side of the build (HeadBuildHook's
    // queued task); a no-op for an actor wearing no pack.
    //
    // ⚠ THE HEAD IS THE ONE PART THIS MOD REPAINTS, and SkinPlan.h says why:
    // the face is node overrides, skee re-applies those at load and on demand
    // and never after a head build, so a face that comes out of a rebuild
    // wears the NIF's own paths with skee's store still saying ours are on it.
    void ReapplyHead(RE::Actor* a_actor);

    // Bind one texture file into the player's FACE TINT COMPOSITE slot, on
    // every face geometry that wears a faceGen material. Returns true only if
    // something was written.
    //
    // ⚠⚠ THIS IS WHY IT IS A NODE OVERRIDE AND NOT A MATERIAL WRITE. Shader
    // materials are pooled: BSShaderProperty::SetMaterial dedups and refcounts,
    // so two loads of one NIF can share a single instance, and writing
    // `tintTexture` in place is the OS-192 crash shape. skee's override channel
    // takes the file by path and lets the engine do the binding.
    //
    // The slot is 6, skee's `renderedTexture` for kShaderType_FaceGen, which is
    // the tint composite the engine otherwise builds from the 34-slot list. A
    // look whose face is a baked export has no layers in that list, which is
    // the whole reason this exists. See LookFaceTint.h.
    //
    // ⚠ Nothing re-applies a node override after a head build, so the caller
    // has to run this from HeadBuildHook for as long as the file should stay,
    // exactly as ReapplyHead above does for a skin pack.
    bool ApplyFaceTintFile(RE::Actor* a_actor, const std::string& a_path);

    // Drop that override again. ⚠ REMOVING A STORED VALUE REPAINTS NOTHING, the
    // same trap the armour rows name, so the engine's own composite only comes
    // back on the next build or rebake; the caller owns that ordering.
    void ClearFaceTintFile(RE::Actor* a_actor);


    // ---- what fits this character, and what the default is called ---------
    //
    // Measured on the game thread by request, read by the page any time.
    // `names` is every file name the character's live skin shapes read (body,
    // hands, feet and face, every slot), which is what SkinPlan::Fits judges a
    // pack against; `defaultName` is what the game's own skin is called
    // (SkinPlan::DefaultNameFromPath), empty when nothing could name it;
    // `defaultPath` is the body diffuse the game gives this character, as the
    // slot reads it (the recorded original while a pack is worn), which is
    // the base skin's source and what its tooltip says. `measured` is false
    // until the walk has run, and an unmeasured answer offers every pack and
    // names nothing.
    struct FitInfo {
        std::vector<std::string> names;
        std::string              defaultName;
        std::string              defaultPath;
        bool                     measured{ false };

        // ⚠ THE PATHS BEHIND THOSE NAMES, as the slots spell them. The rival
        // scan needs one per texture to learn which mod really supplies it,
        // and reading a slot at all is only possible here, on the game thread.
        //
        // ⚠⚠ COLLECTED HERE AND RESOLVED ELSEWHERE, DELIBERATELY. Turning each
        // one into a real file is a CreateFileW, and a character's skin reads a
        // couple of dozen distinct textures. Doing that here would put two
        // dozen file opens in a frame for a list nothing is waiting on, so the
        // walk keeps the cheap half (string reads) and the scan thread pays for
        // the opens.
        std::vector<std::string> paths;

        // ⚠⚠ WHY THE DEFAULT HAS NO NAME, WHEN IT HAS NONE. Two things leave
        // `defaultName` empty and the page has to say different words for
        // them: no virtual file system to look the path up in, and a body slot
        // that still reads a file this plugin wrote with no stored row naming
        // what was under it. Field 2026-08-27: the page had one message, so a
        // user running Mod Organizer was told they were not.
        bool ownFileUnderneath{ false };
    };

    // Queue the walk for this actor. Any thread; the page calls it when it
    // opens on a target. Cheap to call again: the walk is a few dozen string
    // reads and one file open.
    void RequestFit(RE::Actor* a_actor);

    // The last measurement for this actor, or an unmeasured FitInfo.
    [[nodiscard]] FitInfo Fit(RE::Actor* a_actor);

    // ---- persistence: the 'SKIN' record ------------------------------------
    [[nodiscard]] std::vector<SkinRow> Snapshot();
    void                               Restore(std::vector<SkinRow> a_rows);
    void                               Revert();

    // ---- the path edge, for the rival scan ----------------------------------
    //
    // `OpenablePath` turns a slot's spelling into something CreateFile can open
    // from the game's working directory; texture sets carry three spellings and
    // the engine takes all of them. `RealPathOf` opens that and asks the kernel
    // what the handle actually is, which under Mod Organizer is the real file
    // in MODS\mods\<mod>\ because usvfs hooks CreateFileW and does NOT hook
    // GetFinalPathNameByHandleW. Pass a_directory for a folder, which cannot be
    // opened without backup semantics. Empty when nothing could be opened.
    [[nodiscard]] std::string OpenablePath(std::string_view a_slotPath);
    [[nodiscard]] std::string RealPathOf(const std::string& a_openable,
                                         bool               a_directory = false);

}  // namespace OS::SkinApi
