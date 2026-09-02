#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The skins this load order installed and then hid.
//
// Every skin mod ever shipped is a REPLACER: it drops femalebody_1.dds and its
// family over the vanilla paths. Install four and the player can see exactly
// one, because usvfs presents one file per path and the losing copies are not
// in the virtual tree at all. No amount of std::filesystem walking finds them.
//
// ⚠⚠ THE WAY THROUGH IS THE HANDLE, NOT THE TREE. usvfs hooks CreateFileW and
// does NOT hook GetFinalPathNameByHandleW, so opening the winner and asking the
// kernel what the handle is returns the real file in MODS\mods\<mod>\. From
// there the mods root is derivable and the sibling folders can be read
// directly, because they are ordinary directories that no VFS is hiding.
// SkinApi already does the first half to name the Base Skin card.
//
// ⛔ AND THE RIVAL CANNOT SIMPLY BE POINTED AT. SkinImport's header carries the
// measurement: an absolute texture path is collapsed to its Data relative tail
// by BSResource::EnsurePathPrefix, so handing the engine a loser's real path
// draws the winner. Wearing one means putting its bytes at a different virtual
// path, which is what Link does with a hard link and no copying.
//
// ⚠ MOD ORGANIZER ONLY, AND IT SAYS SO. Vortex deploys by hard link into Data
// with no \mods\ layer, so there is nothing to walk and the scan reports that it
// found nothing rather than inventing a list. A wrong list is worse than none.
namespace OS::SkinRivals {

    // One installed skin mod that is not the one currently drawing.
    struct Rival {
        std::string              mod;        // the folder name, as its author spelled it
        std::vector<std::string> relatives;  // the texture paths it supplies

        friend bool operator==(const Rival&, const Rival&) = default;
    };

    struct Snapshot {
        std::vector<Rival> rivals;    // sorted by mod, case folded
        std::string        modsRoot;  // empty when the layout is not Mod Organizer's
        std::string        diagnostic;
        std::uint64_t      generation{ 0 };

        // Whether the scan could run at all. False means no rival list is
        // possible on this install, which the page must SAY rather than draw as
        // an empty result that reads like "you have one skin".
        [[nodiscard]] bool Usable() const { return !modsRoot.empty(); }
    };

    // Walk on a detached thread, as SkinPacks::RequestScan does and for the same
    // reason: the editor draws every frame and this opens a file per texture and
    // then reads a directory per installed mod.
    //
    // a_virtualPaths is SkinApi::FitInfo::paths, the spellings the character's
    // live skin shapes read. a_bodyDiffuse is SkinApi::FitInfo::defaultPath,
    // the body's own diffuse.
    //
    // ⛔⛔ THE BODY DIFFUSE IS THE WHOLE DEFINITION OF "A SKIN MOD" HERE, and
    // the first build had no such test. It offered any mod supplying ANY of the
    // paths above, and those paths are every texture slot on every head
    // geometry: cubemaps, detail maps, hair, brows, eyes, mouth. The field got
    // a grid holding ENB Dynamic Cubemaps (one cubemap), Modpocalypse (two
    // detail maps), KS Hairdos, Tullius Hair, a mouth texture and Fitting Room
    // itself, none of which change a skin, and several of which linked into
    // packs that drew identically to the skin already worn.
    //
    // ⚠ A HEAD-ONLY SKIN REPLACER WILL NOT BE FOUND, which is the accepted
    // cost. The alternative is a table of known skin file names, and
    // SkinPlan's header explains at length why this mod does not keep one: the
    // names on the live material are the whole vocabulary, and a table is a
    // second reader that goes stale the day a body mod invents a suffix.
    void RequestScan(std::vector<std::string> a_virtualPaths, std::string a_bodyDiffuse);

    [[nodiscard]] std::shared_ptr<const Snapshot> Get();
    [[nodiscard]] bool                            Scanning();

    // Hard link one rival's files into the skins folder, so the existing pack
    // pipeline can see them. Returns how many links landed; a_why carries the
    // reason when that is zero.
    //
    // ⚠⚠ A LINKED SKIN IS NOT VISIBLE UNTIL THE GAME RESTARTS. usvfs builds its
    // virtual tree when the process starts, so a file added to a mod folder now
    // is not in that tree now. The card has to say so; a player who links a skin
    // and sees nothing change will conclude the feature is broken.
    std::size_t Link(std::string_view a_mod, std::string& a_why);

    // Whether Link has already run for this mod in this session, which is what
    // separates "installed, link it" from "linked, restart to wear".
    [[nodiscard]] bool Linked(std::string_view a_mod);

    // How many hard links Link actually CREATED for this mod in this session, as
    // opposed to finding already there from an earlier one.
    //
    // ⚠⚠ NON-ZERO ON A MOD THAT IS ALREADY A PACK MEANS HALF VISIBLE, which is
    // the worst of the three states this file can leave a skin in. A pack whose
    // folder is entirely new is merely absent until the restart and its card says
    // so; a pack that EXISTED and gained files wears what usvfs saw at launch and
    // silently drops the rest, which the field met as a body that swapped and a
    // face that did not.
    [[nodiscard]] std::size_t FreshLinks(std::string_view a_mod);

}  // namespace OS::SkinRivals
