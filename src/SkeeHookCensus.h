#pragma once
#include "PCH.h"

#include "HookSite.h"

#include <Windows.h>
#undef GetObject

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

// Who owns the entries OverlayFix detours inside skee64, read rather than
// assumed.
//
// OverlayFix locates every one of its skee hooks by a HARDCODED OFFSET from
// skee64.dll's base. Not a signature scan and not the Address Library, so the
// build is pinned to one skee64 layout and a skee64 that does not match puts
// the detour on unrelated bytes. Several of its hooks are also compile-time
// gated (DISMEMBER_CRASH_FIX_ALPHA, MORPHCACHE_SHRINK_WORKAROUND,
// CRASH_FIX_ALPHA), so which ones a shipped build actually installs is not
// answerable from the source at all.
//
// Both questions are answered by reading the first bytes of each entry and
// naming whoever the jump lands in. That is all this does: twelve reads, once,
// no hook and no write. `docs/re/2026-08-24-overlayfix-hook-table.md` carries
// the table this fills in.
//
// ⚠ THE OFFSETS ARE OverlayFix's, NOT OURS, and they are the one thing here
// that can go stale. When they do, the report says so by naming skee64 itself
// as the owner of every row, which is the same answer as "OverlayFix installed
// nothing" and has to be read with the version in hand.
namespace OS::SkeeHookCensus {

    struct Site {
        std::uintptr_t offset;
        const char*    name;
        const char*    gate;
    };

    // Read from OverlayFix 1.74.0.0-prerelease's GameEventHandler.cpp. The gate
    // column is the INI key or macro that decides whether the hook installs.
    inline constexpr Site kSites[] = {
        { 0x1CD70, "ApplyMorphs", "parallelmorphfix" },
        { 0x167B0, "UpdateMorphs", "parallelmorphfix" },
        { 0x1D280, "CacheShrink", "MORPHCACHE_SHRINK_WORKAROUND" },
        { 0xC68D0, "UpdateNodeTransforms", "paralleltransformfix" },
        { 0xC72C0, "SetNodeTransforms", "paralleltransformfix" },
        { 0xD04D0, "InstallOverlay", "DISMEMBER_CRASH_FIX_ALPHA" },
        { 0xD22A0, "Overlay", "DISMEMBER_CRASH_FIX_ALPHA" },
        { 0xD23F0, "Overlay2", "DISMEMBER_CRASH_FIX_ALPHA" },
        { 0x127E70, "SetShaderProperty", "none, always installed" },
        { 0x129680, "UpdateWorldDataTask", "ragdollfix" },
        { 0x133330, "SkeletonOnAttach", "paralleltransformfix" },
    };

    // SizeOfImage from the PE header at the module base, so a stale offset is
    // refused instead of faulting on a read past the end of the mapping.
    [[nodiscard]] inline std::size_t ImageSize(std::uintptr_t a_base) {
        if (a_base == 0) {
            return 0;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return 0;
        }
        const auto* const nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return 0;
        }
        return nt->OptionalHeader.SizeOfImage;
    }

    // ⚠⚠ THE TABLE ABOVE IS A GUESS AND THE SCAN BELOW IS NOT. r80 read all
    // eleven named entries and found a jump at NONE of them, with most of the
    // entry bytes landing mid-instruction (`24 20`, `58 8B 45 30`, `46 08`),
    // which is what a WRONG OFFSET looks like rather than what an uninstalled
    // hook looks like. OverlayFix carries per-skee-build offset tables and
    // picks one at runtime; the constants here came from one branch of its
    // source and this rig's skee64 is evidently not the build that branch is
    // for. A census that can only check offsets it guessed can never answer
    // the question.
    //
    // So the real instrument is the other way round: walk skee64's executable
    // bytes and report every jump whose TARGET lands inside OverlayFix. That
    // finds a hook wherever it was installed and needs no offset at all. A
    // stray E9 in data would have to point its rel32 into one particular
    // 700 KB module to be a false positive, which is selective enough to trust.
    inline void ScanForForeignJumps(std::uintptr_t a_skee, std::size_t a_skeeSize,
                                    std::uintptr_t a_other, const char* a_otherName) {
        const auto otherSize = ImageSize(a_other);
        if (a_other == 0 || otherSize == 0) {
            return;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_skee);
        const auto* const nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_skee + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        std::uint32_t found = 0;
        for (std::uint16_t sec = 0; sec < nt->FileHeader.NumberOfSections; ++sec, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }
            const auto begin = section->VirtualAddress;
            const auto end   = std::min<std::size_t>(
                begin + section->Misc.VirtualSize, a_skeeSize);
            const auto* const code = reinterpret_cast<const std::uint8_t*>(a_skee);
            for (std::size_t i = begin; i + 6 <= end; ++i) {
                std::uintptr_t target = 0;
                if (code[i] == 0xE9) {
                    std::int32_t rel = 0;
                    std::memcpy(&rel, code + i + 1, sizeof(rel));
                    target = a_skee + i + 5 + static_cast<std::intptr_t>(rel);
                } else if (code[i] == 0xFF && code[i + 1] == 0x25) {
                    std::int32_t disp = 0;
                    std::memcpy(&disp, code + i + 2, sizeof(disp));
                    const auto slot = a_skee + i + 6 + static_cast<std::intptr_t>(disp);
                    // ⚠ THE SLOT IS BOUNDS-CHECKED BEFORE IT IS READ. A jmp
                    // qword ptr [rip+disp] found by scanning is not necessarily
                    // a real instruction, so its disp32 is not necessarily a
                    // real address, and dereferencing whatever it names would
                    // be this instrument crashing the game it is measuring.
                    if (slot < a_skee || slot + sizeof(std::uintptr_t) > a_skee + a_skeeSize) {
                        continue;
                    }
                    std::memcpy(&target, reinterpret_cast<const void*>(slot), sizeof(target));
                } else {
                    continue;
                }
                if (target < a_other || target >= a_other + otherSize) {
                    continue;
                }
                ++found;
                if (found <= 32) {
                    spdlog::info("SkeeHookCensus: skee64+0x{:X} jumps into {}+0x{:X} "
                                 "[{}].",
                                 i, a_otherName, target - a_other,
                                 OS::HookSite::BytesAt(a_skee + i, 8));
                }
            }
        }
        if (found > 32) {
            spdlog::info("SkeeHookCensus: {} more jump(s) into {} not listed.",
                         found - 32, a_otherName);
        }
        spdlog::info("SkeeHookCensus: {} jump(s) from skee64's code land inside {}.",
                     found, a_otherName);
    }

    inline void Run() {
        const auto skee =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleA("skee64.dll"));
        const auto overlayFix =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleA("OverlayFix.dll"));
        if (skee == 0) {
            spdlog::info("SkeeHookCensus: skee64.dll is not loaded, so there is "
                         "nothing to read.");
            return;
        }
        const auto size = ImageSize(skee);
        spdlog::info("SkeeHookCensus: skee64 base 0x{:X}, image {} bytes; "
                     "OverlayFix base 0x{:X}{}.",
                     skee, size, overlayFix,
                     overlayFix == 0 ? " (NOT LOADED)" : "");

        std::uint32_t detoured = 0;
        std::uint32_t byOverlayFix = 0;
        for (const auto& site : kSites) {
            if (size != 0 && site.offset >= size) {
                spdlog::warn("SkeeHookCensus: {} at +0x{:X} is PAST THE END of this "
                             "skee64 ({} bytes), so OverlayFix's offset does not "
                             "belong to the build this rig runs.",
                             site.name, site.offset, size);
                continue;
            }
            const auto addr   = skee + site.offset;
            const auto target = OS::HookSite::JmpTargetAt(addr);
            const auto bytes  = OS::HookSite::BytesAt(addr, 8);
            if (target == 0) {
                spdlog::info("SkeeHookCensus: {} at +0x{:X} (0x{:X}) is NOT detoured; "
                             "entry reads [{}]. Gate: {}.",
                             site.name, site.offset, addr, bytes, site.gate);
                continue;
            }
            ++detoured;
            const auto owner = OS::HookSite::OwnerOf(target);
            const bool mine  = overlayFix != 0 && owner.base == overlayFix;
            if (mine) {
                ++byOverlayFix;
            }
            spdlog::info("SkeeHookCensus: {} at +0x{:X} (0x{:X}) is DETOURED into {} "
                         "(+0x{:X}); entry reads [{}]. Gate: {}.",
                         site.name, site.offset, addr, owner.name,
                         owner.base != 0 ? target - owner.base : 0u, bytes, site.gate);
        }
        spdlog::info("SkeeHookCensus: {} of {} entries carry a jump, {} of them into "
                     "OverlayFix. An entry with no jump is one its build did not "
                     "install, either because its gate is off or because the offset "
                     "no longer names that function.",
                     detoured, std::size(kSites), byOverlayFix);

        // And the answer that does not depend on those offsets being right.
        ScanForForeignJumps(skee, size, overlayFix, "OverlayFix.dll");
        ScanForForeignJumps(
            skee, size,
            reinterpret_cast<std::uintptr_t>(GetModuleHandleA("FittingRoom.dll")),
            "FittingRoom.dll");
    }

}  // namespace OS::SkeeHookCensus
