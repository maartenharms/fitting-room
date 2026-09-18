// Offline half of the FSMP bridge: the build table. The engine half
// (module lookup, the SkinSingleGeometry call) is field-verified in the
// OS-99 rounds; nothing here loads a DLL.
#include "FsmpBridge.h"

#include <cstdio>
#include <iterator>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::FsmpBridge;

    // The builds this load order can actually load, fingerprints read from
    // disk 2026-08-01 (Task 1 amendment).
    const auto* slot32 = MatchBuild(0x6813A7EDu, 0x837000u);
    CHECK(slot32 != nullptr);

    const auto* flex = MatchBuild(0x6986353Bu, 0x95F000u);
    CHECK(flex != nullptr);

    const auto* se = MatchBuild(0x665702B5u, 0x843000u);
    CHECK(se != nullptr);

    // All three 2.5-line builds take the SkinSingleGeometry entry route.
    CHECK(CoopOf(slot32) == Coop::kSkinSingleEntry);
    CHECK(CoopOf(flex) == Coop::kSkinSingleEntry);
    CHECK(CoopOf(se) == Coop::kSkinSingleEntry);

    // ⚠ THE INVERSION, 2026-08-17. This build used to be asserted ABSENT,
    // because its SkinSingleGeometry "hook" is a call-site rewrite and an entry
    // call therefore stretches the strand bones. That reason has not changed
    // and is not being waived: it is now ENCODED, as the flavour that never
    // calls that entry. See docs/re/fsmp-cooperative-hair.md.
    const auto* smp401 = MatchBuild(0x6A4A9ECCu, 0x40F000u);
    CHECK(smp401 != nullptr);
    CHECK(CoopOf(smp401) == Coop::kSkinAllEntry);

    // ⚠ THE SECOND INVERSION, 2026-09-03, and it obeyed the rule the first one
    // set rather than waiving it: read the DLL, then move the assertion. This
    // guard is what made that the only way in, and it fired on the first build
    // after the rows went in. All three siblings ship inside the 4.0.1 archive
    // with their own PDBs, and a sweep of every named function in each found
    // the two stored originals written once in BSFaceGenNiNodeHooks::Hook and
    // handed to DetourAttach in InstallLowPriority, with no call and no jmp
    // through either slot anywhere in the image. That is the flavour's whole
    // promise. docs/re/fsmp-cooperative-hair.md carries the per-variant
    // offsets and the method.
    const auto* smp401sse2 = MatchBuild(0x6A4A9FDBu, 0x412000u);
    const auto* smp401avx  = MatchBuild(0x6A4A9FD9u, 0x414000u);
    const auto* smp401avx2 = MatchBuild(0x6A4A9FE0u, 0x412000u);
    CHECK(smp401sse2 != nullptr);
    CHECK(smp401avx != nullptr);
    CHECK(smp401avx2 != nullptr);
    CHECK(CoopOf(smp401sse2) == Coop::kSkinAllEntry);
    CHECK(CoopOf(smp401avx) == Coop::kSkinAllEntry);
    CHECK(CoopOf(smp401avx2) == Coop::kSkinAllEntry);

    // ⚠⚠ SSE2 AND AVX2 SHARE A SizeOfImage AND DIFFER ONLY IN THE STAMP
    // (0x412000 both, 0x6A4A9FDB against 0x6A4A9FE0). The pair is the
    // identity and neither field alone will do, which a table keyed on one of
    // them would get silently wrong rather than loudly.
    CHECK(smp401sse2 != smp401avx2);

    // ⚠ THE 3.x LINE IS STILL ABSENT AND A USER ON IT IS REFUSED ON PURPOSE.
    // This is 3.5.0 SSE2, whose fingerprint Menu Studio's FsmpDrive already
    // carries for driving the world. Knowing a fingerprint is not knowing what
    // its head hooks do, which is the whole reason the siblings above needed a
    // read, so a row must never be copied across from that table. Encoded here
    // because copying one is exactly the shortcut this file exists to catch.
    CHECK(MatchBuild(0x6A46CEC4u, 0x3CD000u) == nullptr);

    // Same stamp, wrong size: not a match. Unknown entirely: not a match.
    CHECK(MatchBuild(0x6813A7EDu, 0x400000u) == nullptr);
    CHECK(MatchBuild(0x12345678u, 0x99999u) == nullptr);

    // Totality: no fingerprint, no route.
    CHECK(CoopOf(nullptr) == Coop::kNone);

    // The id pairs, field by field, and never equal to each other. A paste
    // error between the two flavours fails offline instead of in the field.
    CHECK(EntryFor(Coop::kSkinSingleEntry) == (Entry{ 26406, 26987 }));
    CHECK(EntryFor(Coop::kSkinAllEntry) == (Entry{ 26405, 26986 }));
    CHECK(EntryFor(Coop::kNone) == (Entry{ 0, 0 }));
    CHECK(!(EntryFor(Coop::kSkinSingleEntry) == EntryFor(Coop::kSkinAllEntry)));

    // ⚠ SE AND AE ARE SEPARATE NUMBERINGS, which is the trap FSMP's own
    // Offsets.h fell into by shipping an AE-space id that resolves to a
    // different function on SE. Encoded as a test rather than a comment.
    CHECK(EntryFor(Coop::kSkinSingleEntry).seId != EntryFor(Coop::kSkinSingleEntry).aeId);
    CHECK(EntryFor(Coop::kSkinAllEntry).seId != EntryFor(Coop::kSkinAllEntry).aeId);
    CHECK(FaceVtableIds().seId != FaceVtableIds().aeId);

    // The asymmetric gating decision, as a test: only the route that publishes
    // with the FMD present has to observe its detour before arming.
    CHECK(RequiresOwnershipProof(Coop::kSkinAllEntry));
    CHECK(!RequiresOwnershipProof(Coop::kSkinSingleEntry));
    CHECK(!RequiresOwnershipProof(Coop::kNone));

    // The one place the vtable displacement is allowed to exist: derived from
    // the INDEX, never measured as an offset.
    CHECK(kFixSkinInstancesVIdx * 8 == 0x1F0);

    // Table hygiene: every row carries a real flavour, and no two rows share a
    // fingerprint. A duplicate with two flavours is a silent routing bug.
    for (const auto& b : kBuilds) {
        CHECK(b.coop == Coop::kSkinSingleEntry || b.coop == Coop::kSkinAllEntry);
        CHECK(b.name != nullptr);
    }
    for (std::size_t i = 0; i < std::size(kBuilds); ++i) {
        for (std::size_t j = i + 1; j < std::size(kBuilds); ++j) {
            CHECK(!(kBuilds[i].timeDateStamp == kBuilds[j].timeDateStamp &&
                    kBuilds[i].sizeOfImage == kBuilds[j].sizeOfImage));
        }
    }

    // ---- the foreign-link table (OS-247) -------------------------------
    //
    // ⚠ THE ROW EXISTS BECAUSE THE DETOUR WAS READ, and every field in it is a
    // fact out of docs/re/mfee-foreign-hook-chain.md. The identity is MFEE's PE
    // TimeDateStamp and SizeOfImage, the landing is the detour at 0x63F30, the
    // continuation is the .data global DetourAttach left the trampoline in.
    const auto* mfee = MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u,
                                    0x63F30u, Coop::kSkinAllEntry);
    CHECK(mfee != nullptr);
    if (mfee) {
        CHECK(mfee->continuationRva == 0xFA2C0u);
        CHECK(mfee->readOn != nullptr);
    }

    // Module names come out of GetModuleFileName and their case is not ours to
    // predict, so the match is case-insensitive on the name and exact on
    // everything else.
    CHECK(MatchForeign("mufacialexpressionextended.dll", 0x69BE80D5u, 0x109000u, 0x63F30u,
                       Coop::kSkinAllEntry) == mfee);
    CHECK(MatchForeign("MUFACIALEXPRESSIONEXTENDED.DLL", 0x69BE80D5u, 0x109000u, 0x63F30u,
                       Coop::kSkinAllEntry) == mfee);
    CHECK(!SameModuleName("a.dll", "ab.dll"));
    CHECK(SameModuleName("", ""));

    // ⚠⚠ AN MFEE UPDATE MUST STOP MATCHING. Both halves of the identity move
    // when the DLL is rebuilt and both RVAs in the row are build specific, so a
    // row that survived an update would be reading stale offsets out of a new
    // binary. Asserted rather than trusted.
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D6u, 0x109000u, 0x63F30u,
                       Coop::kSkinAllEntry) == nullptr);  // rebuilt: new stamp
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x10A000u, 0x63F30u,
                       Coop::kSkinAllEntry) == nullptr);  // rebuilt: new size

    // ⚠⚠ THE RVA IS PART OF THE KEY. A chain that lands anywhere in MFEE other
    // than the one function that was disassembled is a hook nobody has read,
    // wearing a name we recognise. It must miss.
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u, 0x64250u,
                       Coop::kSkinAllEntry) == nullptr);  // its ChangeHeadPart detour
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u, 0x63F31u,
                       Coop::kSkinAllEntry) == nullptr);  // one byte into it

    // The row was read against ONE entry, so it cannot be spent on the other.
    // The 2.5 route never consults this table, and a row that answered for it
    // would be claiming a read of a hook that does not exist: MFEE touches
    // SkinAllGeometry only.
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u, 0x63F30u,
                       Coop::kSkinSingleEntry) == nullptr);
    CHECK(MatchForeign("MuFacialExpressionExtended.dll", 0x69BE80D5u, 0x109000u, 0x63F30u,
                       Coop::kNone) == nullptr);

    // A module nobody has read misses, which is the entire default.
    CHECK(MatchForeign("SomeOtherPlugin.dll", 0x69BE80D5u, 0x109000u, 0x63F30u,
                       Coop::kSkinAllEntry) == nullptr);

    // Table hygiene, same rules as kBuilds: every row carries a real flavour,
    // names its module, records WHEN it was read, and no two rows share an
    // identity. A row without a read date is a row nobody can audit.
    for (const auto& f : kForeignLinks) {
        CHECK(f.module != nullptr && *f.module != '\0');
        CHECK(f.name != nullptr);
        CHECK(f.readOn != nullptr && *f.readOn != '\0');
        CHECK(f.coop == Coop::kSkinSingleEntry || f.coop == Coop::kSkinAllEntry);
        CHECK(f.detourRva != 0);
        CHECK(f.continuationRva != 0);
        // The continuation is a data global and the detour is code, so they
        // cannot be the same address, and both must sit inside the image.
        CHECK(f.detourRva != f.continuationRva);
        CHECK(f.detourRva < f.sizeOfImage);
        CHECK(f.continuationRva < f.sizeOfImage);
    }
    for (std::size_t i = 0; i < std::size(kForeignLinks); ++i) {
        for (std::size_t j = i + 1; j < std::size(kForeignLinks); ++j) {
            CHECK(!(kForeignLinks[i].timeDateStamp == kForeignLinks[j].timeDateStamp &&
                    kForeignLinks[i].sizeOfImage == kForeignLinks[j].sizeOfImage &&
                    kForeignLinks[i].detourRva == kForeignLinks[j].detourRva));
        }
    }

    if (g_failures == 0) {
        std::printf("all FsmpBridge tests passed\n");
    }
    return g_failures;
}
