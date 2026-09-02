#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

// Finding the skins a load order installed but hid, and deciding where each
// one's files have to be put so the engine can read them.
//
// ⚠⚠ AN ABSOLUTE TEXTURE PATH DOES NOT FAIL. IT SILENTLY LOADS THE WINNER, and
// that measurement is what this whole file exists to work around. MEASURED
// 2026-08-26 in both binaries: the texture loader resolves every path through
// BSResource::EnsurePathPrefix (AE id 69822, SE id 68471, identical decompiles,
// named and commented in the Ghidra project) as
//     EnsurePathPrefix(buf, 0x104, path, "textures\\")
// reached from BSTextureDB::NiTextureDBForwarded::vf2 and vf3. A path that does
// not already start with the prefix is scanned for a separator followed by
// "textures" followed by a separator, and on a hit the function returns a
// POINTER INTO THE MIDDLE of the caller's string. So
//     C:\...\MODS\mods\Bijin Skin\textures\actors\character\female\femalebody_1.dds
// collapses to
//     textures\actors\character\female\femalebody_1.dds
// which is Data relative, which under Mod Organizer resolves to whichever mod
// WINS the conflict. Handing the engine a losing skin's real path would look
// like it worked and would draw the skin the player already had.
//
// ⛔ SO "REFERENCE IN PLACE" CANNOT MEAN "GIVE THE ENGINE THE REAL PATH". The
// bytes have to become reachable at a DIFFERENT virtual path under Data\
// textures\, and the cheapest way to do that is a hard link into the folder
// this mod already owns. That costs no disk and leaves SkinPacks::ScanRoot,
// SkinPlan::PackIdOf, SkinPlan::Fits and the card grid untouched: a linked
// skin arrives as an ordinary pack.
//
// ⚠ EVERYTHING HERE IS PURE, and that is deliberate. The two calls that touch
// the world (CreateFileW to resolve a real path, CreateHardLinkW to make the
// link) live at the edge in SkinApi, so the parsing that decides WHERE a link
// goes can be pinned by a test the way SkinPlan::DefaultNameFromPath is.
namespace OS::SkinImport {

    namespace detail {

        [[nodiscard]] inline std::string Lower(std::string_view a_text) {
            std::string out{ a_text };
            for (auto& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        inline constexpr std::string_view kMods = "\\mods\\";
        inline constexpr std::string_view kTex  = "\\textures\\";

        // The offset just past the "\mods\" that names a real mod folder, and
        // the offset of the separator that ends that folder's name.
        //
        // ⚠⚠ THE OCCURRENCE FOLLOWED BY textures\, NOT THE FIRST ONE. Nolvus
        // keeps its mods under "...\MODS\mods\", so the first "\mods\" names
        // the folder called "mods" and the mod name would come back as "mods".
        // SkinPlan::DefaultNameFromPath had to learn this the same way and its
        // comment records the measurement.
        struct Split {
            std::size_t start{ std::string::npos };  // first character of the mod name
            std::size_t end{ std::string::npos };    // the separator after it
            [[nodiscard]] bool Ok() const { return start != std::string::npos; }
        };

        [[nodiscard]] inline Split SplitAtMod(std::string_view a_realPath) {
            const auto real = Lower(a_realPath);
            for (auto at = real.find(kMods); at != std::string::npos;
                 at      = real.find(kMods, at + 1)) {
                const auto start = at + kMods.size();
                const auto end   = real.find('\\', start);
                if (end == std::string::npos || end == start) {
                    continue;
                }
                if (real.compare(end, kTex.size(), kTex) == 0) {
                    return Split{ start, end };
                }
            }
            return {};
        }

    }  // namespace detail

    // Which mod really supplies a file, and what it is called inside that mod.
    //
    // `relative` always begins "textures\", because that is the only shape
    // SplitAtMod accepts: a mod whose file is not under textures\ cannot be
    // matched against a rival by relative path, so it is not an owner this
    // feature can use.
    struct Owner {
        std::string mod;       // the folder name, spelled as its author spelled it
        std::string relative;  // "textures\actors\character\female\femalebody_1.dds"

        [[nodiscard]] bool Ok() const { return !mod.empty() && !relative.empty(); }
        friend bool        operator==(const Owner&, const Owner&) = default;
    };

    // ⚠ EMPTY IS THE HONEST ANSWER FOR VORTEX AND FOR A MANUAL INSTALL. Vortex
    // deploys by hard link into Data with no \mods\ layer, so nothing here can
    // find a rival and the caller must say it found nothing rather than guess.
    // A wrong requirements list is worse than none.
    [[nodiscard]] inline Owner OwnerOf(std::string_view a_realPath) {
        const auto split = detail::SplitAtMod(a_realPath);
        if (!split.Ok()) {
            return {};
        }
        Owner out;
        out.mod      = std::string{ a_realPath.substr(split.start, split.end - split.start) };
        out.relative = std::string{ a_realPath.substr(split.end + 1) };
        return out;
    }

    // Everything up to and including the "\mods\" that owns this file, so the
    // siblings can be listed. Empty on the same terms as OwnerOf.
    [[nodiscard]] inline std::string ModsRootOf(std::string_view a_realPath) {
        const auto split = detail::SplitAtMod(a_realPath);
        if (!split.Ok()) {
            return {};
        }
        return std::string{ a_realPath.substr(0, split.start) };
    }

    // Where the same file would live inside a rival mod. The caller asks the
    // filesystem whether it is really there; a rival that has it is competing
    // for this character's skin.
    [[nodiscard]] inline std::string CandidatePath(std::string_view a_modsRoot,
                                                   std::string_view a_modName,
                                                   std::string_view a_relative) {
        std::string out{ a_modsRoot };
        if (!out.empty() && out.back() != '\\' && out.back() != '/') {
            out.push_back('\\');
        }
        out += a_modName;
        out.push_back('\\');
        out += a_relative;
        return out;
    }

    // Where the hard link goes: under the skins folder, in a folder named after
    // the mod, keeping the rest of the source path.
    //
    // ⚠ THE LEADING textures\ COMES OFF because the skins directory is already
    // under one, and ⚠⚠ THE REST IS MIRRORED RATHER THAN FLATTENED because two
    // source folders can hold the same leaf name (a female and a male
    // femalebody_1.dds is the ordinary case). SkinPlan::AddFile keeps the first
    // of two same-named files in one pack and only counts the loss, so
    // flattening would silently drop half a skin. ScanRoot recurses and
    // PackIdOf takes only the FIRST segment under the skins root, so the mod
    // name stays the pack id however deep the mirrored tail goes.
    [[nodiscard]] inline std::string LinkTarget(std::string_view a_skinsDir,
                                                std::string_view a_modName,
                                                std::string_view a_relative) {
        constexpr std::string_view kTexRoot = "textures\\";
        std::string_view           rel      = a_relative;
        if (rel.size() > kTexRoot.size() &&
            detail::Lower(rel.substr(0, kTexRoot.size())) == kTexRoot) {
            rel.remove_prefix(kTexRoot.size());
        }
        std::string out{ a_skinsDir };
        while (!out.empty() && (out.back() == '\\' || out.back() == '/')) {
            out.pop_back();
        }
        out.push_back('\\');
        out += a_modName;
        out.push_back('\\');
        out += rel;
        return out;
    }

}  // namespace OS::SkinImport
