#include "PCH.h"

#include "VersionCheck.h"

#include <unordered_set>

// WHY THIS FILE EXISTS
//
// Fitting Room used to refuse every runtime that was not 1.5.97 or 1.6.1130+,
// which is what users on the mid 1.6 builds (1.6.640 and friends) saw as
// SKSE's "reported as incompatible during load" dialog. Menu Studio failed the
// same way in the same screenshot, and the cause is the same: the gate was
// written when the AE half of the address table had been verified on exactly
// one binary, and "verified on one build" became "refuse every other build".
//
// The ids were never the problem. Checked offline against every shipped
// Address Library database (docs/research/callsites.py and Menu Studio's
// midversion_ids.py), every id used here resolves on every AE build from
// 1.6.317 up. What does not survive a version change is the other kind of
// address: the hand-measured BYTE OFFSET INSIDE a function - 24725+0x1EF and
// friends. Nothing re-points those.
//
// So they are hints now, and the real identity of a call site is what it
// CALLS. LocateCall tries the hint and, if the byte there is not an E8 whose
// target is the known callee, scans the containing function for the call that
// is.
//
// ⚠ AND FOR ONE SITE, WHAT IT CALLS IS NOT ENOUGH.
//
// On AE the rebuild parent 24725 calls the visit function 16096 TWICE - at
// +0xFC and at +0x1EF - and only +0x1EF is the worn pass. +0xFC is a checker
// pass over a different visitor, and hooking it would put Fitting Room's
// injection on the wrong traversal. A "unique call to the callee" rule (which
// is all Menu Studio needs) therefore finds two hits here and, correctly,
// refuses to guess - which would leave the core hook uninstalled.
//
// The discriminator is the visitor each call is handed, loaded by a
// rip-relative lea shortly before it. Measured on both binaries
// (docs/research/wornpass_anchor.py):
//
//     SE  24231  +0x81  <- lea +0x4E  -> 0x15A0598  = worn visitor vtable 241890
//     AE  24725  +0xFC  <- lea +0xE3  -> 0x17E54C0  = a checker, id 195854
//     AE  24725  +0x1EF <- lea +0x19E -> 0x17E5488  = worn visitor vtable 195851
//
// So the site is identified by a PAIR - callee plus the visitor handed to it -
// and both halves are Address Library ids, so both are re-pointed per build.
// A pair is also a much stronger identity than either half alone, which is the
// real reason to prefer it even where a single anchor would do.

namespace {
    constexpr std::size_t kScanWindow = 0x1000;
    constexpr std::size_t kPadRun     = 4;
    // How far back from a call to look for the lea that sets up its argument.
    // The measured distances are 0x33, 0x19 and 0x51 bytes; 0x60 covers them
    // with room to spare without reaching back into a previous call's setup.
    constexpr std::size_t kAnchorLookback = 0x60;

    bool           g_ran{ false };
    bool           g_criticalOk{ false };
    std::ptrdiff_t g_wornPass{ 0 };
    std::ptrdiff_t g_wornMask{ 0 };
    std::ptrdiff_t g_inputBlock{ 0 };
    std::ptrdiff_t g_weaponPlayer{ 0 };
    std::ptrdiff_t g_weaponNpc{ 0 };
    std::ptrdiff_t g_quiver{ 0 };

    // ---- the address table -------------------------------------------------
    // Callers, callees and the visitor vtable, all as (SE, AE) id pairs. The
    // callees were resolved from the real binaries rather than trusted from a
    // comment: docs/research/callsites.py follows the E8 at each site and maps
    // the target back to an id.
    namespace Ids {
        // ImGuiOverlay input-block hook.
        inline constexpr REL::RelocationID InputDispatch{ 67315, 68617 };      // caller
        inline constexpr REL::RelocationID InputDispatchInner{ 67355, 68655 };  // callee

        // BipedHooks worn pass (the injection).
        inline constexpr REL::RelocationID WornPassParent{ 24231, 24725 };  // caller
        inline constexpr REL::RelocationID WornPassVisit{ 15856, 16096 };   // callee
        inline constexpr REL::RelocationID WornVisitorVtbl{ 241890, 195851 };  // anchor

        // WeaponHooks: the equip function is called TWICE from one parent,
        // once for the player and once for an NPC, with nothing to tell the
        // two calls apart - so they are located by ORDINAL. The callee id is
        // spelled out in WeaponHooks.cpp's own warning: "anyone adding a
        // direct RelocationID for it must write RelocationID(15526, 15703)".
        inline constexpr REL::RelocationID WeaponEquipParent{ 15506, 15683 };  // caller
        inline constexpr REL::RelocationID WeaponEquipCallee{ 15526, 15703 };  // callee
        // The quiver goes through a DIFFERENT parent, so its call to the same
        // callee is unique and needs no ordinal.
        inline constexpr REL::RelocationID QuiverParent{ 15511, 15688 };  // caller

        // BipedHooks worn-mask shim.
        inline constexpr REL::RelocationID WornMaskParent{ 24220, 24724 };   // caller
        inline constexpr REL::RelocationID GetWornMask{ 15806, 16044 };      // callee
    }

    // Hand-measured hints. Fast path only - see the header comment.
    std::ptrdiff_t WornPassHint() { return REL::Relocate(std::ptrdiff_t{ 0x81 }, std::ptrdiff_t{ 0x1EF }); }
    std::ptrdiff_t WornMaskHint() { return REL::Relocate(std::ptrdiff_t{ 0x7C }, std::ptrdiff_t{ 0x80 }); }
    std::ptrdiff_t InputBlockHint() { return std::ptrdiff_t{ 0x7B }; }  // same on both
    std::ptrdiff_t WeaponPlayerHint() { return REL::Relocate(std::ptrdiff_t{ 0x17F }, std::ptrdiff_t{ 0x2B1 }); }
    std::ptrdiff_t WeaponNpcHint() { return REL::Relocate(std::ptrdiff_t{ 0x1D0 }, std::ptrdiff_t{ 0x2FA }); }
    std::ptrdiff_t QuiverHint() { return REL::Relocate(std::ptrdiff_t{ 0x141 }, std::ptrdiff_t{ 0x199 }); }

    // ---- database ----------------------------------------------------------
    // Built from IDDatabase::Offset2ID because that is the only PUBLIC route to
    // the id table. It copies and sorts the whole database (~380k entries) and
    // we throw the sort away, which is wasteful but happens once at startup.
    const std::unordered_set<std::uint64_t>& IdSet() {
        static const std::unordered_set<std::uint64_t> set = [] {
            std::unordered_set<std::uint64_t> out;
            const REL::IDDatabase::Offset2ID  db{};
            out.reserve(db.size());
            for (const auto& entry : db) {
                out.insert(entry.id);
            }
            return out;
        }();
        return set;
    }

    std::pair<std::uintptr_t, std::uintptr_t> TextRange() {
        const auto seg = REL::Module::get().segment(REL::Segment::textx);
        return { seg.address(), seg.address() + seg.size() };
    }

    std::uintptr_t CallTarget(std::uintptr_t a_site) {
        const auto rel = *reinterpret_cast<const std::int32_t*>(a_site + 1);
        return static_cast<std::uintptr_t>(a_site + 5 + rel);
    }

    // TRUE when some `lea r64, [rip+disp32]` in the kAnchorLookback bytes
    // before a_site resolves to a_anchor.
    //
    // Encoding: REX (0x48 plain, 0x4C when the destination is r8-r15), 0x8D,
    // then a modrm whose mod=00 and rm=101, which is the rip-relative form.
    // Masking with 0xC7 tests exactly those two fields and ignores the reg
    // field, so any destination register matches.
    bool HasAnchor(std::uintptr_t a_site, std::uintptr_t a_anchor, std::uintptr_t a_lowBound) {
        const auto from = (std::max)(a_lowBound, a_site > kAnchorLookback ? a_site - kAnchorLookback : 0);
        for (auto at = from; at + 7 <= a_site; ++at) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(at);
            if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) {
                continue;
            }
            const auto disp = *reinterpret_cast<const std::int32_t*>(at + 3);
            if (static_cast<std::uintptr_t>(at + 7 + disp) == a_anchor) {
                return true;
            }
        }
        return false;
    }

    struct Located {
        std::ptrdiff_t offset{ 0 };
        const char*    how{ "absent" };
    };

    // TRUE on the two runtimes whose call-site offsets were measured by hand
    // against the real binary. On those the hint is KNOWN correct, which is
    // what lets LocateCall fall back to it - see the chaining case there.
    bool HintIsHandMeasured() {
        const auto v = REL::Module::get().version();
        return v == REL::Version(1, 5, 97, 0) || v >= REL::Version(1, 6, 1130, 0);
    }

    // Find the call to a_callee inside the function at a_start.
    //
    // Three ways to say WHICH call, in descending order of how much they
    // prove. Use the strongest one a site allows:
    //
    //  - nothing extra: the call must be UNIQUE. A locator that picks one of
    //    several candidates is guessing, so more than one match refuses.
    //  - a_anchor: the call must also be preceded by a rip-relative lea
    //    loading that address. This is what separates the worn pass from the
    //    checker pass on AE, where both call the same function.
    //  - a_ordinal (0-based): take the Nth match in address order. WEAKEST,
    //    and only for sites where the callee is genuinely called more than
    //    once and nothing distinguishes the calls. WeaponHooks is the case:
    //    15506/15683 calls the equip function twice, once for the player and
    //    once for an NPC, and neither call is handed a distinguishing operand
    //    (verified - Tools/re/allmods_callsites.py finds no rip-relative lea
    //    in front of either). What makes it usable rather than a coin flip is
    //    that the ORDER is the same on both binaries we can check: player
    //    first, NPC second, on SE (+0x17F, +0x1D0) and on AE (+0x2B1, +0x2FA).
    //    Still better than the raw byte offset it replaces, because it
    //    survives the function moving or growing; it only breaks if the two
    //    calls swap places, which would also break the hand-measured offsets.
    Located LocateCall(std::uintptr_t a_start, std::uintptr_t a_callee, std::ptrdiff_t a_hint,
                       std::uintptr_t a_anchor = 0, int a_ordinal = -1) {
        if (a_start == 0 || a_callee == 0) {
            return {};
        }
        const auto [textLo, textHi] = TextRange();
        if (a_start < textLo || a_start >= textHi) {
            return {};
        }

        const auto matches = [&](std::uintptr_t a_site) {
            return *reinterpret_cast<const std::uint8_t*>(a_site) == 0xE8 &&
                   CallTarget(a_site) == a_callee &&
                   (a_anchor == 0 || HasAnchor(a_site, a_anchor, a_start));
        };

        if (a_hint != 0 && a_start + static_cast<std::uintptr_t>(a_hint) + 5 < textHi &&
            matches(a_start + static_cast<std::uintptr_t>(a_hint))) {
            return { a_hint, "hand-measured offset" };
        }

        const auto*    code = reinterpret_cast<const std::uint8_t*>(a_start);
        const auto     room = static_cast<std::size_t>(textHi - a_start);
        const auto     span = (std::min)(kScanWindow, room > 5 ? room - 5 : 0);
        std::ptrdiff_t found = 0;
        std::ptrdiff_t nth = 0;
        int            hits = 0;
        std::size_t    pad = 0;

        for (std::size_t i = 0; i < span; ++i) {
            if (code[i] == 0xCC) {
                if (++pad >= kPadRun) {
                    break;  // walked off the end of the function
                }
                continue;
            }
            pad = 0;
            if (code[i] == 0xE8 && matches(a_start + i)) {
                if (hits == a_ordinal) {
                    nth = static_cast<std::ptrdiff_t>(i);
                }
                found = static_cast<std::ptrdiff_t>(i);
                ++hits;
            }
        }

        if (a_ordinal >= 0) {
            // An ordinal site EXPECTS more than one match, so uniqueness is
            // not the test - having the one we asked for is. Fewer matches
            // than expected means the function changed shape and the index no
            // longer means what it meant when it was measured.
            if (hits > a_ordinal) {
                return { nth, "located by scan (ordinal)" };
            }
            spdlog::error("  only {} matching call(s), needed at least {} - the function's "
                          "shape changed; not guessing.", hits, a_ordinal + 1);
            return {};
        }
        if (hits == 1) {
            return { found, "located by scan" };
        }
        if (hits > 1) {
            spdlog::error("  {} calls match inside the function - ambiguous, refusing to guess.",
                          hits);
            return {};
        }

        // ⚠ NO TARGET MATCH IS NOT THE SAME AS NO CALL SITE. THIS FILE IS
        // WHERE THAT WAS LEARNED, ON THE FIRST FIELD RUN.
        //
        // The 1.6.1170 log said "input block: NOT FOUND inside the containing
        // function" for a site the clean binary shows as a plain E8 to the
        // expected callee (68617+0x7B -> 68655, verified byte for byte in
        // docs/research/callsites.py). The only way both are true: ANOTHER MOD
        // in that load order had already hooked it, so its E8 points at that
        // mod's trampoline and no longer names the callee at all.
        //
        // The old code byte-checked E8 and wrote, which chains onto the other
        // mod and is what every SKSE plugin relies on. Refusing instead was a
        // straight regression - it silently turned the input block off in
        // exactly the load orders where it used to work, and in Menu Studio's
        // twin of this function it would have refused to LOAD THE PLUGIN.
        //
        // Gated on the runtime, because that is what makes it safe: on 1.5.97
        // and 1.6.1130+ the hint was measured against the real binary, so an
        // E8 there IS the site. On the mid versions the hint is a guess, and a
        // guess that lands on an E8 by luck is how you write five bytes into
        // the middle of an unrelated instruction - so there the target must
        // still match.
        if (a_hint != 0 && HintIsHandMeasured() &&
            a_start + static_cast<std::uintptr_t>(a_hint) + 5 < textHi &&
            *reinterpret_cast<const std::uint8_t*>(a_start + a_hint) == 0xE8) {
            return { a_hint, "hand-measured offset, ALREADY HOOKED by another mod - chaining" };
        }
        return {};
    }

    // ---- reporting ---------------------------------------------------------
    struct Entry {
        const char*              name;
        const REL::RelocationID& id;
    };

    const Entry kTable[] = {
        { "input dispatch (caller)",   Ids::InputDispatch },
        { "input dispatch (callee)",   Ids::InputDispatchInner },
        { "worn-pass parent (caller)", Ids::WornPassParent },
        { "worn-pass visit (callee)",  Ids::WornPassVisit },
        { "worn visitor vtable",       Ids::WornVisitorVtbl },
        { "worn-mask parent (caller)", Ids::WornMaskParent },
        { "GetWornMask (callee)",      Ids::GetWornMask },
        { "weapon equip parent",       Ids::WeaponEquipParent },
        { "weapon equip (callee)",     Ids::WeaponEquipCallee },
        { "quiver parent",             Ids::QuiverParent },
    };

    std::string HexBytes(std::uintptr_t a_addr, std::size_t a_count) {
        const auto [textLo, textHi] = TextRange();
        std::string out;
        for (std::size_t i = 0; i < a_count; ++i) {
            const auto at = a_addr + i;
            if (at < textLo || at >= textHi) {
                break;  // only .text is guaranteed readable here
            }
            out += fmt::format("{:02X} ", *reinterpret_cast<const std::uint8_t*>(at));
        }
        return out.empty() ? "??" : out;
    }

    void ReportEntry(const Entry& a_entry) {
        const auto id = a_entry.id.id();
        if (!OS::VersionCheck::IdOk(id)) {
            spdlog::error("  {:<28} id {} ABSENT from this build's Address Library.",
                          a_entry.name, id);
            return;
        }
        const auto addr = a_entry.id.address();
        const auto off  = a_entry.id.offset();
        const auto [textLo, textHi] = TextRange();
        if (addr >= textLo && addr < textHi) {
            spdlog::info("  {:<28} id {:<6} rva 0x{:06X}  [{}]", a_entry.name, id, off,
                         HexBytes(addr, 8));
        } else {
            // The visitor vtable lives in .rdata, so this is normal for it and
            // suspicious for everything else.
            spdlog::info("  {:<28} id {:<6} rva 0x{:06X}  (not .text)", a_entry.name, id, off);
        }
    }

    // Resolve a site, guarding every address() behind IdOk: CommonLib's
    // report_and_fail is a message box, not an exception, and a self-check
    // that can kill the process is not a self-check.
    Located Resolve(const char* a_label, const REL::RelocationID& a_caller,
                    const REL::RelocationID& a_callee, std::ptrdiff_t a_hint,
                    const REL::RelocationID* a_anchor = nullptr, int a_ordinal = -1) {
        if (!OS::VersionCheck::IdOk(a_caller) || !OS::VersionCheck::IdOk(a_callee) ||
            (a_anchor && !OS::VersionCheck::IdOk(*a_anchor))) {
            spdlog::error("  {}: an id is missing from this build; site NOT located.", a_label);
            return {};
        }
        const auto found = LocateCall(a_caller.address(), a_callee.address(), a_hint,
                                      a_anchor ? a_anchor->address() : 0, a_ordinal);
        if (found.offset != 0) {
            spdlog::info("  {}: +0x{:X} ({}).", a_label, found.offset, found.how);
        } else {
            spdlog::error("  {}: NOT FOUND inside the containing function.", a_label);
        }
        return found;
    }
}

namespace OS::VersionCheck {
    bool IdOk(std::uint64_t a_id) {
        return a_id != 0 && IdSet().contains(a_id);
    }

    bool IdOk(const REL::RelocationID& a_id) {
        return IdOk(a_id.id());
    }

    bool           CriticalOk() { return g_criticalOk; }
    std::ptrdiff_t WornPassCallOffset() { return g_wornPass; }
    std::ptrdiff_t WornMaskCallOffset() { return g_wornMask; }
    std::ptrdiff_t InputBlockCallOffset() { return g_inputBlock; }
    std::ptrdiff_t WeaponPlayerCallOffset() { return g_weaponPlayer; }
    std::ptrdiff_t WeaponNpcCallOffset() { return g_weaponNpc; }
    std::ptrdiff_t QuiverCallOffset() { return g_quiver; }

    void Run() {
        if (g_ran) {
            return;
        }
        g_ran = true;

        spdlog::info("--- address self-check, runtime {} ---", REL::Module::get().version().string());
        spdlog::info("  reading Address Library...");
        spdlog::info("  Address Library: {} ids.", IdSet().size());

        for (const auto& entry : kTable) {
            ReportEntry(entry);
        }

        // The anchor is what separates the worn pass from the checker pass on
        // AE. Passing it on SE too costs nothing and keeps one code path.
        g_wornPass = Resolve("worn pass", Ids::WornPassParent, Ids::WornPassVisit, WornPassHint(),
                             &Ids::WornVisitorVtbl).offset;
        g_wornMask = Resolve("worn-mask shim", Ids::WornMaskParent, Ids::GetWornMask,
                             WornMaskHint()).offset;
        g_inputBlock = Resolve("input block", Ids::InputDispatch, Ids::InputDispatchInner,
                               InputBlockHint()).offset;
        // Ordinal 0 = player, 1 = NPC. Verified in that order on both
        // binaries; see LocateCall's note on why an ordinal is acceptable here
        // and nowhere else.
        g_weaponPlayer = Resolve("weapon equip (player)", Ids::WeaponEquipParent,
                                 Ids::WeaponEquipCallee, WeaponPlayerHint(), nullptr, 0).offset;
        g_weaponNpc    = Resolve("weapon equip (NPC)", Ids::WeaponEquipParent,
                                 Ids::WeaponEquipCallee, WeaponNpcHint(), nullptr, 1).offset;
        g_quiver       = Resolve("quiver equip", Ids::QuiverParent, Ids::WeaponEquipCallee,
                                 QuiverHint()).offset;

        // The two biped hooks ARE Fitting Room - without them nothing is worn
        // differently and the mod is an inert editor window. The input-block
        // hook only affects how the editor eats input, so it degrades rather
        // than disqualifies.
        g_criticalOk = g_wornPass != 0 && g_wornMask != 0;
        spdlog::info("--- self-check {} ---", g_criticalOk ? "PASSED" : "FAILED");
    }
}
