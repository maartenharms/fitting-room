#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace RE {
    class BSFaceGenNiNode;
    class BSGeometry;
    class NiNode;
}

namespace OS::FsmpBridge {

    // Which engine entry an FSMP build detours, and therefore which call of
    // ours cooperates with it. Two lines, two codebases under one mod page.
    //
    //   kSkinSingleEntry  The 2.5 line: true entry detours on BOTH
    //                     SkinSingleGeometry and SkinAllGeometry. Its
    //                     SkinSingle handler dispatches an event and returns
    //                     with no call to the stored original, which is what
    //                     makes one entry call per geometry the cooperative
    //                     pass. ⚠ Its SkinALL handler DOES call the original
    //                     for every non-player skeleton, which is why the FMD
    //                     shield has to stay on across every publish here.
    //
    //   kSkinAllEntry     The 4.0 line: SkinSingleGeometry's ENTRY is
    //                     untouched (one internal call site is rewritten
    //                     instead), so an entry call runs vanilla engine code
    //                     and stretches the strand bones. SkinAllGeometry is
    //                     still entry-detoured through the BSFaceGenNiNode
    //                     vtable, and the hook never calls either stored
    //                     original for anyone. So the WHOLE-HEAD call is the
    //                     cooperative pass, and it must carry the FMD.
    //
    // docs/re/fsmp-cooperative-hair.md is the authority for both.
    enum class Coop : std::uint8_t { kNone = 0, kSkinSingleEntry, kSkinAllEntry };

    // A known hdtsmp64.dll build, identified the way MS's FsmpDrive does it:
    // PE TimeDateStamp + SizeOfImage. ⚠ A row here asserts exactly one thing:
    // this build's head hooks honour the contract its FLAVOUR names, verified
    // in docs/re/fsmp-cooperative-hair.md. Add rows only with that
    // verification, never from a version number, and never by assuming one CPU
    // variant speaks for its siblings.
    struct Build {
        std::uint32_t timeDateStamp;
        std::uint32_t sizeOfImage;
        Coop          coop;
        const char*   name;
    };

    inline constexpr Build kBuilds[] = {
        // The two AE builds that can win the MO2 conflict on this instance,
        // both 2.5-line with true entry detours on SkinSingle/SkinAll
        // (binary-verified, docs/re/fsmp-cooperative-hair.md).
        { 0x6813A7EDu, 0x837000u, Coop::kSkinSingleEntry,
          "HDT-SMP Slot 32 Fix 1.6 (AE, 2.5 line)" },
        { 0x6986353Bu, 0x95F000u, Coop::kSkinSingleEntry,
          "HDT-SMP Flex for AE 1.6.1170 (2.5 line)" },
        // The 2.5.0 SE build; the spike read this line's source directly.
        // Armed for the delegated SE side once its field pass happens.
        { 0x665702B5u, 0x843000u, Coop::kSkinSingleEntry,
          "Faster HDT-SMP 2.5.0 (SE, Nexus 57339)" },
        // ⚠ THE 4.0 LINE WAS EXCLUDED HERE UNTIL 2026-08-17, and the reason it
        // was excluded is the reason it can be rowed now. Its
        // SkinSingleGeometry "hook" is a call-site rewrite, so an entry call
        // lands in vanilla engine code and stretches. That fact does not touch
        // vfunc 0x3E, so this row arrives with a route that never calls the
        // SkinSingle entry at all. Read with its shipped PDB; it is the DLL
        // this rig loads.
        { 0x6A4A9ECCu, 0x40F000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.0.1 (AE, AVX-512)" },
        // ⚠⚠ ITS THREE CPU SIBLINGS, READ 2026-09-03 AND ROWED HERE. They
        // had been left out because this flavour's safety rests on "the hook
        // never calls the engine original", which a fingerprint cannot say,
        // and none of them had been disassembled. A Nexus report of the
        // SMP-hair refusal paid for the read, and it cost nothing to run: the
        // 4.0.1 archive already in MODS\downloads carries all four CPU builds
        // WITH their PDBs.
        //
        // MEASURED per variant from its own shipped PDB, method and numbers in
        // docs/re/fsmp-cooperative-hair.md. Across all ~10,500 functions of
        // each build, every reference to _SkinAllGeometry_Orig and
        // _SkinSingleGeometry is a STORE in BSFaceGenNiNodeHooks::Hook plus
        // the lea handing the slot address to DetourAttach in
        // InstallLowPriority. No call and no jmp through either slot exists in
        // any of the four, which is the contract itself rather than a proxy
        // for it. All four embed the same ids (vtable SE 252410 / AE 200333
        // for the SkinAll entry detour, SE 26466 / AE 27061 for the
        // SkinSingle call site) and the SkinSingle ENTRY ids 26406/26987
        // appear in none of them, so all four carry the call-site-not-entry
        // shape the AVX-512 build was verified to have.
        { 0x6A4A9FDBu, 0x412000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.0.1 (AE, SSE2)" },
        { 0x6A4A9FD9u, 0x414000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.0.1 (AE, AVX)" },
        { 0x6A4A9FE0u, 0x412000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.0.1 (AE, AVX2)" },
        // 4.1.1, ALL FOUR CPU builds, added 2026-08-29 after the user updated
        // to it and found physics had stopped in Menu Studio's menus. Same 4.x
        // line, so the same flavour.
        //
        // ⚠ THE FOUR SHIP INSIDE ONE DOWNLOAD, which is why the mod page lists
        // a single 4.1.1 file: the archive holds raw-vs2022-windows, -avx,
        // -avx2 and -avx512 and the installer picks one. Labels are the
        // archive's own folder names; an instruction census confirms the ends
        // of the range but cannot separate AVX from AVX2, so it does not decide
        // them. Their layouts were read per variant for Menu Studio's
        // FsmpDrive in the same pass, each from its own shipped PDB.
        //
        // ⚠ THIS ROW IS SAFE TO ADD ON THE LINE ALONE AND THE 2.5 ROWS ABOVE
        // WOULD NOT BE, which is the whole asymmetry RequiresOwnershipProof
        // exists for. kSkinAllEntry does not arm on the fingerprint: ProveRoute
        // walks the SkinAllGeometry entry at runtime and refuses unless the
        // chain lands inside hdtsmp64's own image. So if 4.1.1 turns out not to
        // have 4.0's hook shape, this stays dormant, which is exactly where it
        // sits today with no row at all. A kSkinSingleEntry row arms on the
        // fingerprint and would need the offline read first.
        //
        // The layouts WERE read rather than assumed: identical member offsets
        // to 4.0.1 across all four, only the three RVAs moved per variant.
        { 0x6A8F44A7u, 0x41A000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.1.1 (AE, SSE2)" },
        { 0x6A8F44A9u, 0x41B000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.1.1 (AE, AVX)" },
        { 0x6A8F44CAu, 0x419000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.1.1 (AE, AVX2)" },
        { 0x6A8F449Eu, 0x417000u, Coop::kSkinAllEntry,
          "Faster HDT-SMP 4.1.1 (AE, AVX-512)" },
        // ⚠⚠ THE 3.x LINE STILL HAS NO ROW, AND THAT IS A LIVE REPORT RATHER
        // THAN A GAP NOBODY HAS WALKED INTO. A user on 3.0.4 is refused with
        // $FR_HairStyleSmpUnavailable, and the refusal is correct rather than a
        // bug: no 3.x build has had its head hooks read, and none is on this
        // rig to read. Rowing one needs its DLL and PDB in hand first, the
        // same way the 4.0.1 siblings above were paid for.
    };

    // The engine entry whose detour a flavour depends on.
    //
    // ⚠ ON kSkinAllEntry THIS IS NOT THE CALL TARGET. That call goes through
    // the vtable slot; this is the address whose BYTES the ownership proof
    // reads. Keeping the pair as data rather than as two literals at two call
    // sites is what stops the SE and AE numberings being pasted across.
    struct Entry {
        int seId;
        int aeId;

        [[nodiscard]] constexpr bool operator==(const Entry&) const = default;
    };

    [[nodiscard]] constexpr Entry EntryFor(Coop a_coop) {
        switch (a_coop) {
            case Coop::kSkinSingleEntry:
                return { 26406, 26987 };  // BSFaceGenNiNode::SkinSingleGeometry
            case Coop::kSkinAllEntry:
                return { 26405, 26986 };  // BSFaceGenNiNode::SkinAllGeometry
            default:
                return { 0, 0 };
        }
    }

    // The BSFaceGenNiNode vtable, for the slot half of the proof.
    [[nodiscard]] constexpr Entry FaceVtableIds() { return { 252410, 200333 }; }

    // FixSkinInstances is vfunc 0x3E. ⚠ DERIVED FROM THE INDEX, never measured
    // as an offset: RTTI labels the same slot BSFaceGenNiNode::vf62, and
    // 62 * 8 == 0x1F0, which is the displacement FSMP's own installer loads.
    inline constexpr std::size_t kFixSkinInstancesVIdx = 0x3E;

    // Whether a flavour may only arm once the detour has been OBSERVED at
    // runtime rather than inferred from a fingerprint.
    //
    // ⚠⚠ TRUE FOR THE 4.0 FLAVOUR ONLY, AND THE ASYMMETRY IS THE POINT. That
    // route publishes a whole-head call with the FMD present, so a missing
    // detour there means the vanilla worker walks facegen model data it must
    // not, which is a crash. On the 2.5 flavour a wrong byte answer would only
    // switch off a feature that is field confirmed working, so it is not
    // allowed to. This is a named function so it cannot be tidied into an if.
    [[nodiscard]] constexpr bool RequiresOwnershipProof(Coop a_coop) {
        return a_coop == Coop::kSkinAllEntry;
    }

    // Pure table lookup, offline-testable.
    [[nodiscard]] inline const Build* MatchBuild(std::uint32_t a_stamp,
                                                 std::uint32_t a_size) {
        for (const auto& b : kBuilds) {
            if (b.timeDateStamp == a_stamp && b.sizeOfImage == a_size) {
                return &b;
            }
        }
        return nullptr;
    }

    [[nodiscard]] constexpr Coop CoopOf(const Build* a_build) {
        return a_build ? a_build->coop : Coop::kNone;
    }

    // A THIRD mod that sits between the engine entry and FSMP, and that
    // somebody has DISASSEMBLED.
    //
    // ⚠⚠ THIS TABLE DOES NOT LOWER THE BAR, IT MOVES THE READ. The scar above
    // the hop walk in ProveRoute still stands: never publish a whole-head call
    // with the FMD present into a chain nobody has read. A row here is the
    // record that somebody DID read one link of it, and the proof still has to
    // walk that link's continuation and still has to land inside hdtsmp64. A
    // row buys a step, never a verdict.
    //
    // ⚠ A ROW ASSERTS ONE THING: this exact build's detour forwards to its
    // saved original unconditionally, first, with the arguments untouched, so
    // stepping across it changes nothing about who ultimately runs. Add rows
    // only with that verification written down, the way docs/re does it, and
    // never from a mod version or a changelog.
    struct ForeignLink {
        const char*   module;           // as OwningModule reports it
        std::uint32_t timeDateStamp;    // PE identity, same keying as Build
        std::uint32_t sizeOfImage;      // ⚠ SizeOfImage, NOT the file size
        std::uint32_t detourRva;        // where the chain must land, exactly
        std::uint32_t continuationRva;  // the global the detour forwards through
        Coop          coop;             // the entry this row was read against
        const char*   name;
        const char*   readOn;           // when the disassembly was done
    };

    inline constexpr ForeignLink kForeignLinks[] = {
        // Mu Facial Expression Extended, read 2026-08-21,
        // docs/re/mfee-foreign-hook-chain.md. Microsoft Detours on the
        // SkinAllGeometry entry; the detour at 0x63F30 calls its saved
        // original as its first act with no predicate anywhere ahead of it,
        // then notifies its observers with the result discarded.
        // DetourAttach left the trampoline address in the .data global at
        // 0xFA2C0, and nothing else in the image writes it.
        { "MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u, 0x63F30u,
          0xFA2C0u, Coop::kSkinAllEntry,
          "Mu Facial Expression Extended (Detours, forwards unconditionally)",
          "2026-08-21" },
    };

    // ASCII case-insensitive compare, for module file names. Spelled out rather
    // than taken from <cctype> because this header is included by the offline
    // tests and by the plugin, and locale-sensitive tolower has no business
    // deciding whether two DLL names match.
    [[nodiscard]] constexpr bool SameModuleName(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i];
            char y = b[i];
            if (x >= 'A' && x <= 'Z') {
                x = static_cast<char>(x - 'A' + 'a');
            }
            if (y >= 'A' && y <= 'Z') {
                y = static_cast<char>(y - 'A' + 'a');
            }
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    // Pure table lookup, offline-testable, and deliberately strict on all five
    // fields.
    //
    // ⚠ THE RVA IS PART OF THE KEY, not decoration. A chain that lands anywhere
    // in this module OTHER than the function that was read is a hook nobody has
    // disassembled wearing a name we happen to recognise, and the whole point of
    // the row is that it names one function in one build.
    [[nodiscard]] inline const ForeignLink* MatchForeign(std::string_view a_module,
                                                         std::uint32_t    a_stamp,
                                                         std::uint32_t    a_size,
                                                         std::uint32_t    a_rva,
                                                         Coop             a_coop) {
        for (const auto& f : kForeignLinks) {
            if (f.timeDateStamp == a_stamp && f.sizeOfImage == a_size &&
                f.detourRva == a_rva && f.coop == a_coop &&
                SameModuleName(f.module, a_module)) {
                return &f;
            }
        }
        return nullptr;
    }

    // kDataLoaded: resolve hdtsmp64.dll, fingerprint it, arm or stay dormant.
    void Init();

    // Observe the detour and arm the 4.0 route if it is really there.
    // Idempotent, latches only the positive, and never caches a negative.
    //
    // ⚠ CALLED AGAIN AT kPostLoadGame, because FSMP installs its head hooks
    // from its own low-priority pass and we cannot order ourselves after it at
    // kDataLoaded. Both call sites are the main thread and both are before the
    // player can open the editor, so IsAvailable() is still a constant to every
    // reader that matters, including the FUCK present thread.
    bool ProveRoute();

    [[nodiscard]] bool IsAvailable();

    // Which route armed, or kNone.
    [[nodiscard]] Coop Route();

    // The cooperative pass, expressed as a route rather than as one call.
    //
    //   kSkinSingleEntry  one SkinSingleGeometry entry call per named geometry,
    //                     each resolved immediately before its call.
    //   kSkinAllEntry     exactly ONE faceNode->FixSkinInstances(skeleton,
    //                     false). The names are used only for the coverage log.
    //
    // ⚠⚠ CALL ONCE PER PUBLISH, NEVER ONCE PER PART. SkinAllGeometry's body is
    // a one-level sweep of every tri-shape child, so N calls cost N squared
    // name resolutions against the skeleton.
    //
    // ⚠ NEVER call when IsAvailable() is false. Callers hold the game thread
    // (OnGameThread), same as every other head mutation.
    void CooperativeSkin(RE::BSFaceGenNiNode* a_head, RE::NiNode* a_skeleton,
                         std::span<const std::string> a_fmdGeomNames);

    // The teardown reclaim, 2.5 route ONLY. On the 4.0 route the publish that
    // precedes every teardown site is itself a whole-head sweep, and
    // processGeometry's first act is cleanHead, so the reclaim has already
    // happened and it covered every tracked part rather than one.
    void NudgeSingle(RE::BSFaceGenNiNode* a_head, RE::NiNode* a_skeleton,
                     RE::BSGeometry* a_geometry);
}
