#include "FsmpBridge.h"

#include "HookSite.h"  // JmpTargetAt, BytesAt, OwningModule

#include <Windows.h>

#include <cstring>

namespace OS::FsmpBridge {

    namespace {
        const Build*   g_build = nullptr;
        Coop           g_route = Coop::kNone;
        std::uintptr_t g_smpBase{ 0 };
        std::uint32_t  g_smpSize{ 0 };

        // The entry a flavour's detour has to sit on, resolved for this
        // runtime. ⚠ Function-local, never at namespace scope: REL::Module has
        // to be ready, which is the convention every other relocation in this
        // codebase follows.
        [[nodiscard]] std::uintptr_t EntryAddress(Coop a_coop) {
            const auto ids = EntryFor(a_coop);
            if (ids.seId == 0 || ids.aeId == 0) {
                return 0;
            }
            return REL::Relocation<std::uintptr_t>{ REL::RelocationID(ids.seId, ids.aeId) }
                .address();
        }

        [[nodiscard]] bool InsideSmp(std::uintptr_t a_addr) {
            return g_smpBase != 0 && a_addr >= g_smpBase &&
                   a_addr < g_smpBase + static_cast<std::uintptr_t>(g_smpSize);
        }

        // A loaded module's build identity, the pair every table in this
        // component keys on.
        //
        // ⚠ SizeOfImage, NEVER the size on disk. They differ by a lot (MFEE is
        // 1,059,328 bytes on disk and 0x109000 mapped), and a row keyed on the
        // wrong one simply never matches.
        struct PeIdentity {
            std::uint32_t stamp;
            std::uint32_t size;
        };

        [[nodiscard]] PeIdentity IdentityOf(std::uintptr_t a_base) {
            // Same reasoning as Init(): the address came from
            // GetModuleHandleEx, so this is a loader-validated mapped image and
            // a 64-bit process cannot have loaded a PE32 one.
            const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
            const auto* const nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
            return { nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage };
        }

        // Follow at most TWO jump hops from an address, reporting both so the
        // log can show its work.
        //
        // ⚠ TWO, because Detours and SafetyHook both write E9 to a jump island
        // when the detour is out of rel32 range and the island holds the FF 25
        // absolute. Returns 0 when there is no jump this understands, which is
        // the shape a real function body has and is therefore a refusal, not an
        // error.
        [[nodiscard]] std::uintptr_t WalkJumps(std::uintptr_t a_from, std::uintptr_t& a_hop1,
                                               std::uintptr_t& a_hop2) {
            a_hop1 = HookSite::JmpTargetAt(a_from);
            a_hop2 = 0;
            if (a_hop1 == 0) {
                return 0;
            }
            if (InsideSmp(a_hop1)) {
                return a_hop1;
            }
            a_hop2 = HookSite::JmpTargetAt(a_hop1);
            return a_hop2 != 0 ? a_hop2 : a_hop1;
        }

        // What trying to step across ONE foreign link found, kept as data so
        // the armed line and the refusal line can each say exactly what
        // happened without re-deriving any of it.
        struct ForeignStep {
            const ForeignLink* link{ nullptr };
            std::string        landedIn;
            std::uintptr_t     continuation{ 0 };
            std::uintptr_t     hop1{ 0 };
            std::uintptr_t     hop2{ 0 };
            std::uintptr_t     dest{ 0 };
        };

        // Resolve who owns the address the entry walk reached, and if it is a
        // module whose detour has been READ, continue the walk from that
        // detour's saved original.
        //
        // ⚠ THIS IS THE ONLY THING THE FOREIGN TABLE BUYS. It does not decide
        // anything: it hands back another address, and the caller still has to
        // find hdtsmp64 at the end of it.
        [[nodiscard]] ForeignStep StepAcrossForeign(std::uintptr_t a_dest, Coop a_coop) {
            ForeignStep  s;
            const auto   owner = HookSite::OwnerOf(a_dest);
            s.landedIn = owner.name;
            if (owner.base == 0) {
                // No loaded module claims it, so there is no build identity to
                // key a row on. In a hook chain that is a trampoline page, and
                // a trampoline is not something anybody can read ahead of time.
                return s;
            }
            const auto id = IdentityOf(owner.base);
            s.link        = MatchForeign(owner.name, id.stamp, id.size,
                                         static_cast<std::uint32_t>(a_dest - owner.base), a_coop);
            if (!s.link) {
                return s;
            }
            // The row has just been verified against this exact image, so its
            // RVAs describe this binary. The bounds check is still here because
            // a mistyped row should refuse rather than read outside the mapping.
            if (s.link->continuationRva + sizeof(std::uintptr_t) > id.size) {
                return s;
            }
            std::memcpy(&s.continuation,
                        reinterpret_cast<const void*>(owner.base + s.link->continuationRva),
                        sizeof(s.continuation));
            if (s.continuation != 0) {
                s.dest = WalkJumps(s.continuation, s.hop1, s.hop2);
            }
            return s;
        }
    }

    void Init() {
        const HMODULE mod = ::GetModuleHandleW(L"hdtsmp64.dll");
        if (!mod) {
            spdlog::info("FsmpBridge: hdtsmp64.dll not loaded; SMP styles "
                         "take the engine gates.");
            return;
        }
        // No e_magic/Signature checks on purpose: GetModuleHandle returns a
        // loader-validated mapped image, and a 64-bit process cannot load a
        // PE32 DLL, so IMAGE_NT_HEADERS64 is the only shape this can be.
        const auto  base = reinterpret_cast<std::uintptr_t>(mod);
        const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
        const auto stamp = nt->FileHeader.TimeDateStamp;
        const auto size  = nt->OptionalHeader.SizeOfImage;
        g_smpBase        = base;
        g_smpSize        = size;
        g_build          = MatchBuild(stamp, size);
        if (!g_build) {
            spdlog::warn("FsmpBridge: unknown hdtsmp64.dll build (stamp "
                         "0x{:08X} size 0x{:X}); staying dormant. SMP styles "
                         "take the engine gates. A new build needs its head "
                         "hooks verified before a row is added, and the row "
                         "carries the FLAVOUR it verified "
                         "(docs/re/fsmp-cooperative-hair.md).",
                         stamp, size);
            return;
        }
        if (!RequiresOwnershipProof(g_build->coop)) {
            // ⚠ ARMED ON THE FINGERPRINT ALONE, and that is deliberate rather
            // than lazy. This is the field-confirmed working path; a new and
            // unproven byte test must never be able to switch it off. See
            // RequiresOwnershipProof for the asymmetry.
            g_route = g_build->coop;
            spdlog::info("FsmpBridge: recognized {}; SMP styles take FSMP's "
                         "head-part path (route: SkinSingleGeometry entry).",
                         g_build->name);
            return;
        }
        spdlog::info("FsmpBridge: recognized {}; route pending ownership proof.",
                     g_build->name);
        ProveRoute();
    }

    bool ProveRoute() {
        if (!g_build || !RequiresOwnershipProof(g_build->coop)) {
            return g_route != Coop::kNone;
        }
        if (g_route != Coop::kNone) {
            return true;  // latched; the positive is never re-tested
        }

        const auto entry = EntryAddress(g_build->coop);
        if (entry == 0) {
            spdlog::warn("FsmpBridge: SkinAllGeometry did not resolve on this runtime, "
                         "so the 4.0 route cannot be proved. Staying dormant.");
            return false;
        }

        // ---- half one: does FSMP own the entry -------------------------
        //
        // ⚠ OWNERSHIP, NOT "SOMEBODY PATCHED IT". A branch that leaves
        // hdtsmp64's image is the case that would otherwise publish a
        // whole-head call with the FMD present into a chain nobody has read.
        // Two hops, because Detours writes E9 to a jump island when the detour
        // is out of rel32 range and the island holds the FF 25 absolute.
        std::uintptr_t hop1 = 0;
        std::uintptr_t hop2 = 0;
        std::uintptr_t dest = WalkJumps(entry, hop1, hop2);

        // ⚠⚠ ONE READ FOREIGN LINK, AND THE BAR IS UNCHANGED EITHER WAY. When
        // a third mod detours this same entry it owns the bytes, and a byte
        // walk cannot see through its detour BODY. That was the whole of
        // OS-247. The answer is not to accept the foreign landing; it is to
        // read that detour offline, record what it does, and then step across
        // it to the saved original it forwards through. The test after this
        // block is still "did we end up inside hdtsmp64", unchanged.
        //
        // ⚠ EXACTLY ONE LINK IS STEPPED. A second foreign hook stacked on top
        // of the first is a chain nobody has measured in the field, and it
        // refuses here and names the module so the next stint knows what to
        // read. Growing this to a loop is a decision to be taken with a log in
        // hand, not in advance.
        ForeignStep foreign;
        if (dest != 0 && !InsideSmp(dest)) {
            foreign = StepAcrossForeign(dest, g_build->coop);
            if (foreign.link) {
                dest = foreign.dest;
            }
        }
        const bool owned = dest != 0 && InsideSmp(dest);

        // ---- half two: does our call still land on those bytes ---------
        //
        // We dispatch through the vtable slot, so the entry being owned says
        // nothing until the slot is shown to still point at it.
        // ⚠ THE SLOT ADDRESS IS DERIVED FROM THE INDEX, and the arithmetic has
        // an independent check: the RE notes record the two slot addresses as
        // SE .rdata 0x1601AB8 and AE 0x18468B0, and vtable + 0x3E*8 reproduces
        // both from the vtable ids below (SE 0x16018C8, AE 0x18466C0).
        const auto                            vtIds = FaceVtableIds();
        const REL::Relocation<std::uintptr_t> vt{
            REL::RelocationID(vtIds.seId, vtIds.aeId)
        };
        const auto     slotAddr = vt.address() + kFixSkinInstancesVIdx * sizeof(std::uintptr_t);
        std::uintptr_t slot     = 0;
        std::memcpy(&slot, reinterpret_cast<const void*>(slotAddr), sizeof(slot));
        const bool slotOk = slot == entry;

        if (owned && slotOk) {
            g_route = g_build->coop;
            if (foreign.link) {
                // ⚠ THE ARMED LINE NAMES THE CHAIN IT TRUSTED AND WHEN IT WAS
                // READ. A field log has to be able to answer that on its own,
                // because "the route armed" means something different when
                // something else is sitting on the entry.
                spdlog::info("FsmpBridge: route armed: SkinAllGeometry entry 0x{:X} via {} "
                             "(read {}) -> continuation 0x{:X} -> 0x{:X} owned by {}, vf62 "
                             "slot intact. The foreign link forwards unconditionally and was "
                             "disassembled; docs/re/mfee-foreign-hook-chain.md.",
                             entry, foreign.link->module, foreign.link->readOn,
                             foreign.continuation, dest, HookSite::OwningModule(dest));
            } else {
                spdlog::info("FsmpBridge: route armed: SkinAllGeometry entry 0x{:X} owned by {} "
                             "(jmp -> 0x{:X}), vf62 slot intact.",
                             entry, HookSite::OwningModule(dest), dest);
            }
            return true;
        }

        // ⚠ THE NEGATIVE IS NOT CACHED. FSMP installs its head hooks from its
        // own low-priority pass, so a no at kDataLoaded is not a no forever;
        // plugin.cpp asks again at kPostLoadGame.
        // ⚠ THE REFUSAL HAS TO SAY WHAT IT FOUND AT THE FOREIGN END, because
        // "reading it is what changes the answer" is only actionable if the log
        // names the module to read.
        std::string foreignNote;
        if (foreign.link) {
            foreignNote = fmt::format(
                " Stepped across {} (read {}); its continuation 0x{:X} walked to 0x{:X} ({}), "
                "which is not inside hdtsmp64, so FSMP is not underneath this chain.",
                foreign.link->module, foreign.link->readOn, foreign.continuation, foreign.dest,
                foreign.dest ? HookSite::OwningModule(foreign.dest) : "-");
        } else if (!foreign.landedIn.empty()) {
            foreignNote = fmt::format(
                " The chain left the engine and ended in {}, which is not a foreign link "
                "anybody has disassembled, so it is refused BY DESIGN rather than trusted. "
                "Reading that detour offline and rowing it in kForeignLinks is what changes "
                "this answer; docs/re/mfee-foreign-hook-chain.md is the worked example. Do not "
                "reorder or remove that mod on our account.",
                foreign.landedIn);
        }
        spdlog::warn("FsmpBridge: the 4.0 route is NOT armed and {} stays dormant. "
                     "entry 0x{:X} first bytes [{}], hop1 0x{:X} ({}), hop2 0x{:X} ({}), "
                     "vf62 slot 0x{:X} ({}). owned={} slotIntact={}{}",
                     g_build->name, entry, HookSite::BytesAt(entry, 8), hop1,
                     hop1 ? HookSite::OwningModule(hop1) : "-", hop2,
                     hop2 ? HookSite::OwningModule(hop2) : "-", slot,
                     slot ? HookSite::OwningModule(slot) : "-", owned, slotOk, foreignNote);
        return false;
    }

    bool IsAvailable() { return g_route != Coop::kNone; }

    Coop Route() { return g_route; }

    namespace {

        // The 2.5 route's one call. Private now: a caller that reached the
        // public surface while the 4.0 route is armed must not get here, and a
        // private function with a flavour guard is how that is made
        // unreachable rather than merely documented.
        void SkinSingleEntry(RE::BSFaceGenNiNode* a_head, RE::NiNode* a_skeleton,
                             RE::BSGeometry* a_geometry) {
            if (g_route != Coop::kSkinSingleEntry) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    spdlog::warn("FsmpBridge: a SkinSingleGeometry entry call was reached on "
                                 "the {} route and was DROPPED. On the 4.0 line that entry is "
                                 "vanilla engine code and would stretch the strand bones "
                                 "(warned once per session).",
                                 g_route == Coop::kSkinAllEntry ? "SkinAllGeometry" : "dormant");
                }
                return;
            }
            if (!a_head || !a_skeleton || !a_geometry) {
                return;
            }

            // Derived in docs/re/fsmp-cooperative-hair.md: SE 1.5.97 0x3D8840,
            // AE 1.6.1170 0x432410, both round-tripped and prologue-checked.
            // ⚠ Locate-by-id, never by byte offset (ids are portable). Resolved
            // lazily here rather than at namespace scope, so the address lookup
            // runs well after REL::Module is ready - the same convention every
            // other relocation in this codebase follows (WeaponHooks.cpp,
            // REAugments.cpp: a function-local REL::Relocation, never one
            // constructed during DLL static initialization).
            // The trailing void* is a fourth engine argument FSMP's 2.5-line hook
            // ignores; passing null matches how FSMP's own walk calls it.
            using SkinSingle_t = void (*)(RE::BSFaceGenNiNode*, RE::NiNode*,
                                          RE::BSGeometry*, void*);
            const auto ids = EntryFor(Coop::kSkinSingleEntry);
            static const REL::Relocation<SkinSingle_t> SkinSingleGeometry{
                REL::RelocationID(ids.seId, ids.aeId)
            };
            SkinSingleGeometry(a_head, a_skeleton, a_geometry, nullptr);
        }

    }  // namespace

    void CooperativeSkin(RE::BSFaceGenNiNode* a_head, RE::NiNode* a_skeleton,
                         std::span<const std::string> a_fmdGeomNames) {
        // ⚠ A dormant call is a CONTRACT VIOLATION, not a quiet no-op: the
        // header forbids it because without the detour these addresses are the
        // engine originals, and the caller who reached here believed
        // IsAvailable() said yes. Dropped, but say so once, so a mis-gated
        // caller reads as this line in the field log instead of as an
        // unexplained stretch.
        if (g_route == Coop::kNone) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("FsmpBridge: CooperativeSkin called while dormant; a caller "
                             "violated the IsAvailable() gate. The call was dropped "
                             "(warned once per session).");
            }
            return;
        }
        if (!a_head || !a_skeleton) {
            return;
        }

        if (g_route == Coop::kSkinAllEntry) {
            // ⚠ EXACTLY ONE WHOLE-HEAD CALL. On this line FSMP's hook sweeps
            // the face node's direct tri-shape children into its own
            // SkinSingleGeometry handler and never calls either stored
            // original, so this one call IS the cooperative pass for every
            // geometry at once. The names are not needed to make it happen;
            // the caller logs coverage from them.
            a_head->FixSkinInstances(a_skeleton, false);
            return;
        }

        // The 2.5 route, unchanged in behaviour: resolve each name and call
        // immediately, one at a time. ⚠ Resolve-then-call stays adjacent so no
        // raw BSGeometry* is held across a call that mutates the scenegraph.
        for (const auto& name : a_fmdGeomNames) {
            const RE::BSFixedString nm{ name.c_str() };
            auto* const             obj  = a_head->GetObjectByName(nm);
            auto* const             geom = obj ? obj->AsGeometry() : nullptr;
            if (geom) {
                SkinSingleEntry(a_head, a_skeleton, geom);
            }
        }
    }

    void NudgeSingle(RE::BSFaceGenNiNode* a_head, RE::NiNode* a_skeleton,
                     RE::BSGeometry* a_geometry) {
        SkinSingleEntry(a_head, a_skeleton, a_geometry);
    }

}  // namespace OS::FsmpBridge
