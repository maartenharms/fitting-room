#pragma once
#include "PCH.h"

#include <Windows.h>  // GetModuleHandleEx, to name whoever displaced a call site
// ⚠ Windows.h defines GetObject as GetObjectW, which then eats
// InventoryEntryData::GetObject at any call site below this include with an
// error that blames CommonLib rather than this header.
#undef GetObject

#include <string>

// Finding a call site to hook, on a load order where somebody else may have
// hooked it first.
//
// ⚠ SHARED BECAUSE TWO FILES NEEDED THE SAME LOCATOR AND A SECOND COPY IS A
// SECOND THING THAT DRIFTS. `ItemCardCharge` hooks six item-card populate
// calls and `SeamstoneRecharge` hooks three calls inside the recharge apply;
// both need the same "find the lone E8 that targets this callee, refuse if
// there are several" rule, and both need to say whose hook they are chaining
// below when the anchor cannot match.
namespace OS::HookSite {

    // Which loaded module owns an address, as BOTH halves of the answer.
    //
    // ⚠ THE BASE IS THERE SO A CALLER CAN ASK THE PE HEADER A QUESTION. Naming
    // the module is enough to turn "my hook did not install" into a fact, but a
    // caller that has to decide whether it TRUSTS the module needs its build
    // identity and the address's offset within it, and both need the base.
    // A base of 0 means no loaded module claims the address, which in a hook
    // chain almost always means a trampoline page.
    struct Owner {
        std::uintptr_t base;
        std::string    name;
    };

    [[nodiscard]] inline Owner OwnerOf(std::uintptr_t a_addr) {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(a_addr), &mod) ||
            !mod) {
            return { 0, "<unowned, probably a trampoline>" };
        }
        const auto base = reinterpret_cast<std::uintptr_t>(mod);
        char       path[MAX_PATH]{};
        if (GetModuleFileNameA(mod, path, MAX_PATH) == 0) {
            return { base, "<unnamed module>" };
        }
        const std::string full{ path };
        const auto        slash = full.find_last_of("\\/");
        return { base, slash == std::string::npos ? full : full.substr(slash + 1) };
    }

    // The name half, for the log. A call site whose target has left
    // SkyrimSE.exe was taken over by another plugin, and naming it is what
    // makes that readable in the field.
    [[nodiscard]] inline std::string OwningModule(std::uintptr_t a_addr) {
        return OwnerOf(a_addr).name;
    }

    // The first a_count bytes at an address, for a log line that has to show
    // its work. A "the entry is not patched" claim nobody can check is worth
    // very little when the next question is always "so what IS there".
    [[nodiscard]] inline std::string BytesAt(std::uintptr_t a_addr, std::size_t a_count) {
        std::string out;
        for (std::size_t i = 0; i < a_count; ++i) {
            out += fmt::format("{:02X} ", *reinterpret_cast<const std::uint8_t*>(a_addr + i));
        }
        return out;
    }

    // Where the jump at a_addr lands, or 0 when the entry does not start with
    // one this understands.
    //
    // ⚠ TWO SHAPES, AND THE SECOND ONE IS THE WHOLE REASON THIS FUNCTION GREW.
    // SafetyHook writes E9 rel32. The plugin that took a head-build entry from
    // us on 2026-08-12 writes FF 25 disp32 - jmp qword ptr [rip+disp32], the
    // six-byte absolute form Detours uses - and the first version of this only
    // decoded E9, so it reported "no E9 at the entry at all" and named nobody.
    // Reading one byte further would have said who. Both forms are decoded now,
    // and the absolute one is followed through its pointer slot, because the
    // module that owns the SLOT is not the module that owns the CODE.
    //
    // ⚠ SHARED, AND IT LIVED IN HeadBuildHook.cpp's ANONYMOUS NAMESPACE until
    // 2026-08-17. FsmpBridge needs the same decode to prove FSMP owns an entry
    // before cooperating through it, and a second copy of a branch decoder is
    // a second thing that learns about a jump shape late.
    [[nodiscard]] inline std::uintptr_t JmpTargetAt(std::uintptr_t a_addr) {
        const auto* const at = reinterpret_cast<const std::uint8_t*>(a_addr);
        if (at[0] == 0xE9) {
            std::int32_t rel = 0;
            std::memcpy(&rel, at + 1, sizeof(rel));
            return a_addr + 5 + static_cast<std::intptr_t>(rel);
        }
        if (at[0] == 0xFF && at[1] == 0x25) {
            std::int32_t disp = 0;
            std::memcpy(&disp, at + 2, sizeof(disp));
            const auto     slot   = a_addr + 6 + static_cast<std::intptr_t>(disp);
            std::uintptr_t target = 0;
            std::memcpy(&target, reinterpret_cast<const void*>(slot), sizeof(target));
            return target;
        }
        return 0;
    }

    // ⚠ IDENTIFY THE SITE BY WHAT IT CALLS, NEVER BY A BYTE OFFSET.
    // Hand-measured offsets are not portable and nothing re-points them; both
    // caller and callee are Address Library ids, so both move with the build.
    // Returns the offset of the single E8 whose target is a_callee, or 0 for
    // none.
    //
    // ⚠ REFUSES ON MORE THAN ONE MATCH rather than taking the first. A locator
    // that silently picks one of several candidates is guessing, and this
    // project has already been bitten by a parent that calls its callee twice
    // with different arguments.
    [[nodiscard]] inline std::size_t FindLoneCallTo(std::uintptr_t a_caller,
                                                    std::uintptr_t a_callee,
                                                    const char*    a_who) {
        constexpr std::size_t kMaxScan = 0x600;
        const auto* const     bytes    = reinterpret_cast<const std::uint8_t*>(a_caller);
        std::size_t           found    = 0;
        std::size_t           hits     = 0;
        std::size_t           pad      = 0;
        for (std::size_t i = 0; i + 5 <= kMaxScan; ++i) {
            // Stop at inter-function padding so the scan cannot wander into the
            // next function and hook a call that is not ours.
            pad = bytes[i] == 0xCC ? pad + 1 : 0;
            if (pad >= 4) {
                break;
            }
            if (bytes[i] != 0xE8) {
                continue;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, bytes + i + 1, sizeof(rel));
            if (a_caller + i + 5 + static_cast<std::intptr_t>(rel) == a_callee) {
                ++hits;
                found = i;
            }
        }
        if (hits > 1) {
            spdlog::warn("{}: {} calls to the same callee in the caller at 0x{:X}; a locator "
                         "that picks one of several is guessing, so no hook there.",
                         a_who, hits, a_caller);
            return 0;
        }
        return hits == 1 ? found : 0;
    }

}  // namespace OS::HookSite
