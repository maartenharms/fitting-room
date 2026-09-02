#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The skin changer's engine-free half: what a skin pack is, how a pack's file
// finds the shape it belongs on, and the one rule about node names that skee's
// armour overrides impose.
//
// ⚠⚠ A SKIN IS A FOLDER OF REPLACER FILES AND NOTHING MORE. Every skin mod
// ever shipped is a REPLACER: it drops femalebody_1.dds, femalebody_1_msn.dds
// and the rest over the vanilla paths, so a player can only ever have one
// installed at a time. This page lets several coexist by giving each its own
// folder under textures\FittingRoom\skins\<Pack>\, and it applies one by
// matching FILE NAMES: a shape whose slot currently reads any path ending in
// femalebody_1.dds takes the pack's femalebody_1.dds in that slot, and a slot
// whose file name the pack does not carry is left alone. No table of parts,
// sexes or suffixes: the names on the live material are the whole vocabulary,
// which is what makes a UBE pack (femalebody_1_d.dds, _n.dds) and a 3BA pack
// (femalebody_1.dds, _msn.dds) both work without either being described here.
//
// ⚠⚠ AND THAT IS WHY IT IS ARMOUR OVERRIDES, NOT skee's SKIN OVERRIDES.
// MEASURED 2026-08-18 in skee64's OverrideInterface.cpp: a skin override is
// keyed by armour SLOT MASK and lands on EVERY FaceGenRGBTint geometry in the
// addon. On this rig's 3BA body that is three shapes and two texture sets:
// `3BA` wears femalebody_1.dds and `3BA_Vagina` / `3BA_Anus` wear
// femalebody_etc_v2_1.dds (read out of BodySlide (Nude)'s femalebody_1.nif),
// so a slot-mask override would paint the body diffuse onto the genital
// shapes. An ARMOUR override is keyed by (armour, addon, node) and is applied
// by skee at every attach of that addon, which is precise enough to leave the
// etc shapes alone and persistent enough to survive a re-equip.
namespace OS::SkinPlan {

    // Where packs live, relative to textures\, in the form an override wants.
    inline constexpr std::string_view kSkinsDir = "FittingRoom\\skins\\";

    // The BASE SKIN's identity where a pack id is wanted and there is no pack:
    // the star on its card (user 2026-08-18: "we should be able to favorite it
    // as well"). Angle brackets cannot appear in a Windows folder name, so no
    // pack under the skins root can ever be spelled this; the store still
    // spells "no pack" as the empty string, and this never reaches it.
    inline constexpr std::string_view kBaseSkinId = "<base>";

    // One pack: a folder name and every .dds it holds, by lower-cased file
    // name. A std::map rather than an unordered one so the listing a test or
    // a log prints is stable.
    struct Pack {
        std::string                        id;  // the folder name, as found
        std::map<std::string, std::string> files;  // lower name -> override path

        [[nodiscard]] std::size_t Count() const { return files.size(); }
        friend bool operator==(const Pack&, const Pack&) = default;
    };

    [[nodiscard]] inline std::string Lower(std::string_view a_text) {
        std::string out{ a_text };
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // The last segment of a path, lower-cased, whichever separator it uses.
    // Empty in gives empty out.
    [[nodiscard]] inline std::string FileName(std::string_view a_path) {
        const auto cut = a_path.find_last_of("\\/");
        return Lower(cut == std::string_view::npos ? a_path : a_path.substr(cut + 1));
    }

    // The pack folder a scanned file belongs to: the first segment under the
    // skins root. Empty when the path is not under it or sits loose at the
    // root, and a loose file belongs to no pack.
    [[nodiscard]] inline std::string PackIdOf(std::string_view a_overridePath) {
        const auto lowered = Lower(a_overridePath);
        const auto root    = Lower(kSkinsDir);
        if (lowered.rfind(root, 0) != 0) {
            return {};
        }
        const auto rest = a_overridePath.substr(root.size());
        const auto sep  = rest.find('\\');
        if (sep == std::string_view::npos || sep == 0) {
            return {};
        }
        return std::string{ rest.substr(0, sep) };
    }

    // Fold a scanned override path into its pack, if it has one. Returns the
    // pack id it went into, or empty when it went nowhere.
    //
    // ⚠ A NAME THAT APPEARS TWICE IN ONE PACK KEEPS THE FIRST. Two files of
    // one name in two sub folders of a pack cannot both be "the" femalebody_1,
    // and a scan order dependent winner is worse than a stable one; the scan
    // logs the count so a pack author can see it.
    [[nodiscard]] inline std::string AddFile(std::vector<Pack>& a_packs,
                                             std::string_view   a_overridePath) {
        const auto id = PackIdOf(a_overridePath);
        if (id.empty()) {
            return {};
        }
        const auto name = FileName(a_overridePath);
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".dds") != 0) {
            return {};
        }
        auto it = std::find_if(a_packs.begin(), a_packs.end(),
                               [&](const Pack& a_p) { return a_p.id == id; });
        if (it == a_packs.end()) {
            a_packs.push_back(Pack{ id, {} });
            it = std::prev(a_packs.end());
        }
        it->files.emplace(name, std::string{ a_overridePath });
        return id;
    }

    // The SAME body diffuse for the other sex: the path with only its file
    // name's sex prefix swapped, or empty when the name carries neither.
    //
    // ⚠⚠ THE SKIN GATE NEEDED TWO KEYS AND HAD ONE, and a field case is what
    // showed it. SkinRivals counts a mod as a skin only if it replaces the
    // character's body diffuse, which is right (a mod sharing one cubemap is
    // not a skin) and was one file too narrow. UBE puts a futa body's penis on
    // malebody_1_d.dds while the body itself is femalebody_1_d.dds, and skins
    // that ship the two halves as SEPARATE mods exist: 'Zhizhen Female Skin -
    // UBE' carries the female half and 'Zhizhen Female Skin - UBE Penis Patch'
    // carries only malebody_1_d.dds and malebody_1_n.dds. The patch replaces a
    // body diffuse of this character and was thrown away for replacing the
    // wrong one, so the schlong could never follow a skin switch to Zhizhen
    // while Loona and Jada, which ship both halves in one folder, worked.
    //
    // ⚠ AND THE PACK MODEL ALREADY AGREED. Fits() says in as many words that
    // "a partial pack (a body only, a head only) is still a pack"; only the
    // rival scan refused to create one.
    //
    // ⚠ THE FILE NAME ONLY, NEVER THE DIRECTORY. A vanilla-path body lives
    // under texturesctors\characteremale\, so a swap over the whole
    // string would rewrite the FOLDER and ask about a file nobody has. The
    // directory is copied through untouched.
    [[nodiscard]] inline std::string SexSiblingOf(std::string_view a_relative) {
        const auto name = FileName(a_relative);
        if (name.empty()) {
            return {};
        }
        const auto dir = a_relative.substr(0, a_relative.size() - name.size());
        std::string swapped;
        if (name.rfind("female", 0) == 0) {
            swapped = name.substr(2);  // female... -> male...
        } else if (name.rfind("male", 0) == 0) {
            swapped = "fe" + name;     // male... -> female...
        } else {
            return {};
        }
        return std::string{ dir } + swapped;
    }

    // What the pack has for a slot that currently shows a_currentPath, or empty.
    [[nodiscard]] inline std::string Match(const Pack& a_pack, std::string_view a_currentPath) {
        const auto name = FileName(a_currentPath);
        if (name.empty()) {
            return {};
        }
        const auto it = a_pack.files.find(name);
        return it == a_pack.files.end() ? std::string{} : it->second;
    }

    // Whether a path is one of this plugin's own staged files. Such a path can
    // never name what the load order gives a character, because we put it
    // there, and three places have to know that: the default's name, the rival
    // list, and the guard that says when neither can be answered.
    [[nodiscard]] inline bool IsOwnFile(std::string_view a_path) {
        return Lower(a_path).find(Lower(kSkinsDir)) != std::string::npos;
    }

    // ---- which packs fit this character ---------------------------------------
    //
    // ⚠ A PACK MADE FOR ANOTHER BODY IS HIDDEN (user 2026-08-18: "hide ube skin
    // card on 3ba"). The rule is the match rule turned around: the character's
    // live skin shapes read some set of file names (femalebody_1_d.dds on UBE,
    // femalebody_1.dds on 3BA and CBBE, femalehead_d.dds, ...), and a pack that
    // carries NONE of those names would change nothing when clicked, so it is
    // not offered. A pack that carries even one is offered, because a partial
    // pack (a body only, a head only) is still a pack. An empty name set means
    // "not measured yet" and offers everything, never nothing.
    [[nodiscard]] inline bool Fits(const Pack& a_pack, const std::vector<std::string>& a_liveNames) {
        if (a_liveNames.empty()) {
            return true;
        }
        for (const auto& name : a_liveNames) {
            if (a_pack.files.contains(Lower(name))) {
                return true;
            }
        }
        return false;
    }

    // ---- what the game's own skin is called ---------------------------------
    //
    // The default card wants a name (user 2026-08-18: "is there a way we can
    // find a name of the skin used in game default"). Under Mod Organizer the
    // file a slot reads lives in MODS\mods\<mod name>\textures\..., and that
    // mod name IS the skin's name as the player knows it. So: the path the
    // engine's own file open resolved to (GetFinalPathNameByHandle on the
    // handle usvfs redirected), and the segment after "\mods\" if there is
    // one. Without one (Vortex, a manual install, or usvfs answering with the
    // virtual path), the first folder under textures\ names the family when it
    // is not the vanilla "actors" tree ("!UBE" reads as UBE), and the vanilla
    // tree names nothing: the game's own textures are the game's own.
    //
    // Pure, so the parse is pinned; the resolve lives in SkinApi.
    [[nodiscard]] inline std::string DefaultNameFromPath(std::string_view a_realPath,
                                                         std::string_view a_virtualPath) {
        const auto real = Lower(a_realPath);
        // "\mods\<name>\textures\" in the real path: Mod Organizer's layout.
        // ⚠ THE OCCURRENCE FOLLOWED BY textures\, NOT THE FIRST. Nolvus keeps
        // its mods under "...\MODS\mods\<name>\", so the first "\mods\" names
        // the folder "mods" (measured by the test the moment it was written).
        // A mod's root always has textures\ straight under it, so that is
        // the segment that says which one is the mod.
        constexpr std::string_view kMods = "\\mods\\";
        constexpr std::string_view kTex  = "\\textures\\";
        for (auto at = real.find(kMods); at != std::string::npos;
             at = real.find(kMods, at + 1)) {
            const auto start = at + kMods.size();
            const auto end   = real.find('\\', start);
            if (end == std::string::npos || end == start) {
                continue;
            }
            if (real.compare(end, kTex.size(), kTex) == 0) {
                // The original spelling, not the lowered one: a mod is named
                // by its author.
                return std::string{ a_realPath.substr(start, end - start) };
            }
        }
        // Fallback: the first folder under textures\ in the virtual path.
        const auto virt  = Lower(a_virtualPath);
        constexpr std::string_view kTexRoot = "textures\\";
        auto at = virt.rfind(kTexRoot);
        if (at == std::string::npos) {
            return {};
        }
        const auto start = at + kTexRoot.size();
        const auto end   = virt.find('\\', start);
        if (end == std::string::npos || end == start) {
            return {};
        }
        std::string_view folder = a_virtualPath.substr(start, end - start);
        if (Lower(folder) == "actors") {
            return {};  // the vanilla tree: the game's own textures
        }
        while (!folder.empty() && (folder.front() == '!' || folder.front() == '_')) {
            folder.remove_prefix(1);  // "!UBE" and "_Foo" are load-order sorting, not names
        }
        return std::string{ folder };
    }

    // ---- the node name rule --------------------------------------------------
    //
    // ⚠⚠ MEASURED in skee64's OverrideInterface.cpp, OverrideApplicator::Apply,
    // which is what runs at armour attach:
    //
    //   objectName(m_geometryList.size() == 1 ? "" : geometry->m_name)
    //
    // An addon with exactly ONE geometry is looked up under the EMPTY name and
    // an addon with more is looked up under each geometry's own name. The
    // hands and feet NIFs on this rig are one shape each (artHands, Feet), so an
    // override stored under "artHands" would never be found at attach, and one
    // stored under "" would never be found on the 3BA body.
    //
    // ⚠ AND THE COUNT MOVES. skee's overlays are cloned INTO the addon's tree
    // at attach, so the same hands addon counts one geometry on the attach that
    // happens during a revert (overlays deferred) and four on a normal one
    // (three overlays installed first). Nothing written under one count is
    // safe under the other, so a shape that is the addon's only REAL geometry
    // gets both keys. The empty key is harmless when the count is higher: the
    // on demand path resolves "" to the addon's root node, which is not a
    // geometry, and skee's SetShaderProperty does nothing on a non geometry.
    [[nodiscard]] inline bool IsOverlayNode(std::string_view a_name) {
        return a_name.find("[Ovl") != std::string_view::npos ||
               a_name.find("[SOvl") != std::string_view::npos;
    }

    [[nodiscard]] inline std::vector<std::string> NodeKeys(std::string_view a_geometryName,
                                                           std::size_t a_realGeometryCount) {
        std::vector<std::string> out;
        out.emplace_back(a_geometryName);
        if (a_realGeometryCount == 1) {
            out.emplace_back();
        }
        return out;
    }

    // ---- what the store remembers per actor ----------------------------------
    //
    // ⚠ TRACKED SO IT CAN BE TAKEN OFF AGAIN. skee stores an armour override
    // for as long as the save lives and offers no way to enumerate ours, so a
    // pack that was written onto a cuirass the actor has since taken off would
    // come back with the cuirass long after the player pressed Default. Every
    // (armour, addon, node, slot) this mod writes is remembered under the actor
    // so Default can remove exactly those and nothing another mod wrote.
    //
    // ⚠ AND THE ORIGINAL PATH RIDES WITH IT, which is what makes Default and a
    // pack switch cost no rebuild. Removing a stored override repaints nothing
    // (the same trap OverlayApi::Clear names), and the engine will not say what
    // a slot showed before it was overridden. The observer writes each override
    // exactly once, at an attach where skee had nothing of ours to apply, so
    // the path the slot shows at that moment IS the original, and it is written
    // down here so the slot can be put back by a direct property write rather
    // than by reloading the actor's whole 3D.
    struct Written {
        std::string   armorMod;
        std::uint32_t armorLocal{ 0 };
        std::string   addonMod;
        std::uint32_t addonLocal{ 0 };
        std::string   node;
        std::uint8_t  slot{ 0 };
        std::string   original;

        friend bool operator==(const Written&, const Written&) = default;
    };

    // Whether two rows name the same override, whatever they hold.
    [[nodiscard]] inline bool SameKey(const Written& a_lhs, const Written& a_rhs) {
        return a_lhs.armorMod == a_rhs.armorMod && a_lhs.armorLocal == a_rhs.armorLocal &&
               a_lhs.addonMod == a_rhs.addonMod && a_lhs.addonLocal == a_rhs.addonLocal &&
               a_lhs.node == a_rhs.node && a_lhs.slot == a_rhs.slot;
    }

    // ---- what a pack change does to one row -----------------------------------
    //
    // ⚠⚠ A DETACHED ADDON IS NOT A GONE ONE, and telling them apart is the
    // whole point of this. A race switch strips the character; a clearing apply
    // landing in that gap used to drop every row while it had nothing to
    // restore them onto. The rows are the only record of the ORIGINAL path
    // behind each override, so dropping them leaves the addons coming back in
    // this plugin's own files with nothing naming what was underneath, and from
    // then on the body can be neither repainted nor put back. Field
    // 2026-08-27: `20 removed over 0 worn addon(s)`, then `0 added ... Body
    // slots none` on every apply after it.
    //
    // ⚠ THIS IS A DIFFERENT QUESTION FROM "does the form still resolve". A row
    // whose armour left the LOAD ORDER is genuinely unrestorable and the caller
    // drops it before asking this one. Here the form resolves and the addon
    // simply is not on the actor this second.
    //
    // ⚠ AND CLEARING WHILE DRESSED STILL CLEARS. A worn row with no file in the
    // new pack is removed and its original written straight back, which is what
    // replace-on-apply's own `Apply(player, "")` depends on.
    enum class RowFate : std::uint8_t {
        Move,    // the new pack carries a file of this name: rewrite the override
        Remove,  // it does not and the addon is here: take the override off, put
                 // the original back, and let the row go
        Hold,    // it does not and the addon is away: take the override off and
                 // keep the row, because it is all that names the original
    };

    // ⚠ THE ORIGINAL AND ONE BOOLEAN, so the head and the bare body ask this
    // through the same door as the armour rows. They differ only in what
    // "present" means: an armour row asks whether its addon is attached, a node
    // row whether its named geometry is on the actor.
    [[nodiscard]] inline RowFate FateOf(const Pack* a_pack, std::string_view a_original,
                                        bool a_present) {
        if (a_pack && !Match(*a_pack, a_original).empty()) {
            return RowFate::Move;
        }
        // A row that never recorded an original holds nothing worth keeping, so
        // there is no gap for it to survive.
        return (a_present || a_original.empty()) ? RowFate::Remove : RowFate::Hold;
    }

    [[nodiscard]] inline RowFate FateOf(const Pack* a_pack, const Written& a_row, bool a_worn) {
        return FateOf(a_pack, a_row.original, a_worn);
    }

    // ---- the head ------------------------------------------------------------
    //
    // ⚠⚠ THE HEAD IS NODE OVERRIDES, NOT ARMOUR OVERRIDES, because it is not
    // armour: the face, mouth and their overlay clones hang under the actor's
    // BSFaceGenNiNode and belong to no addon. skee's node channel is keyed by
    // (actor, sex, geometry name) and its texture key goes through the same
    // NIOVTaskUpdateTexture the body's does, with the facegen material's own
    // slot map (MEASURED 2026-08-18, skee64 ShaderUtilities.cpp
    // GetTextureFromIndex: 0 diffuse, 1 normal, 2 subsurface, 3 detail, 6 the
    // RENDERED tint composite). So a pack's femalehead_d.dds lands in slot 0
    // and the tint composite the face system paints stays where it is: skin
    // tone and makeup survive, which is what a replacer on disk gets too.
    //
    // ⚠ AND IT IS RE-PAINTED AFTER EVERY HEAD BUILD BY THIS MOD, because skee
    // applies node overrides at load and on demand and never when the engine
    // rebuilds a head (MEASURED: ActorUpdateManager::Flush and SKEEHooks.cpp,
    // where UpdateHeadState reinstalls face OVERLAYS and nothing else).
    // HeadBuildHook already runs on the far side of every head build; the
    // face rows ride the same task the head dye does.
    //
    // A head row is what the body's Written is without the armour: the
    // geometry name skee looked the override up under, the slot, and the path
    // the slot showed before it was written.
    struct HeadWritten {
        std::string  node;
        std::uint8_t slot{ 0 };
        std::string  original;

        friend bool operator==(const HeadWritten&, const HeadWritten&) = default;
    };

    [[nodiscard]] inline bool SameKey(const HeadWritten& a_lhs, const HeadWritten& a_rhs) {
        return a_lhs.node == a_rhs.node && a_lhs.slot == a_rhs.slot;
    }

    // ---- bare skin -----------------------------------------------------------
    //
    // ⚠⚠ A BODY NOBODY IS WEARING IS REACHED THE WAY THE FACE IS. `written`
    // above is armour overrides and needs an addon to hang on, so a naked
    // character had nothing for a pack to paint: field 2026-08-27, "when
    // changing the skin now only the head changes", with `worn 0x0 drawn 0x0`
    // beside every apply. Bare skin hangs under the actor's own root and
    // belongs to no addon, which is the head's situation one limb further
    // down, so it takes the head's route and the head's row shape.
    //
    // ⚠ IT IS A THIRD LIST RATHER THAN MORE HEAD ROWS. They are the same three
    // fields and the same node channel, but the head list is walked by the
    // head-build repaint and read as "the face" in the log and in the rival
    // scan, and a body row arriving in it would be silently counted as a face.
    struct ActorSkin {
        std::string              pack;  // empty means the game's own skin
        std::vector<Written>     written;
        std::vector<HeadWritten> head;
        std::vector<HeadWritten> bare;

        friend bool operator==(const ActorSkin&, const ActorSkin&) = default;
    };

    [[nodiscard]] inline const Written* FindKey(const std::vector<Written>& a_rows,
                                                 const Written&              a_row) {
        const auto it = std::find_if(a_rows.begin(), a_rows.end(),
                                     [&](const Written& a_w) { return SameKey(a_w, a_row); });
        return it == a_rows.end() ? nullptr : &*it;
    }

    [[nodiscard]] inline const HeadWritten* FindKey(const std::vector<HeadWritten>& a_rows,
                                                     const HeadWritten&              a_row) {
        const auto it = std::find_if(a_rows.begin(), a_rows.end(),
                                     [&](const HeadWritten& a_w) { return SameKey(a_w, a_row); });
        return it == a_rows.end() ? nullptr : &*it;
    }

}  // namespace OS::SkinPlan
