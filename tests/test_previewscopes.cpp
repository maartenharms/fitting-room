// Per-path preview scope tests (OS-191). No engine, no D3D, no NIF: the
// pattern match, the precedence and the disk-key tag are string arithmetic.
//
// ⚠ The real names in here are the CENSUS's, not invented ones. The rule this
// ships with was chosen against a complete scan of the LDD Lili Collection's
// meshes, and the near misses that make the choice non-obvious (Corset,
// Cardigan, LiliCollection itself) are pinned below so a future widening of
// the pattern breaks a test instead of a card.
#include "PreviewScopes.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS::PreviewScopes;

namespace {
    // Folded exactly as PreviewGrid::FoldPath would leave them.
    constexpr const char* kLiliDress  = "meshes/ldd/lili collection/lg/dress_0.nif";
    constexpr const char* kLiliUbe    = "meshes/!ube/ldd/lili collection/lg/choker_0.nif";
    constexpr const char* kOtherArmour = "meshes/armor/daedric/cuirass_0.nif";

    ScopeSet Shipped() {
        ScopeSet set;
        set.scopes.push_back(Scope{ "ldd/lili collection/", { "*_col*" }, {} });
        return set;
    }
}  // namespace

int main() {
    {  // the glob, all four forms
        CHECK(MatchesPattern("colskirtlegs", "colskirtlegs"));
        CHECK(!MatchesPattern("colskirtlegs", "colskirt"));
        CHECK(MatchesPattern("colskirtlegs", "col*"));
        CHECK(!MatchesPattern("ldd_colskirtlegs", "col*"));
        CHECK(MatchesPattern("ldd_colskirtlegs", "*legs"));
        CHECK(!MatchesPattern("ldd_colskirtlegs", "*feet"));
        CHECK(MatchesPattern("ldd_colskirtlegs", "*_col*"));
        CHECK(MatchesPattern("anything at all", "*"));
        // ⚠ An empty pattern matches NOTHING. The generous reading is
        // "everything", and its failure mode is a blank card that reads as a
        // broken previewer rather than as a bad rule.
        CHECK(!MatchesPattern("colskirtlegs", ""));
    }

    {  // ⚠⚠ THE FOUR PROXIES THE CENSUS FOUND, AND THE PIECES BESIDE THEM.
       // The choker is the one the field reported; the three skirt hulls ride
       // more than three times as many NIFs and nobody saw them, which is why
       // fixing the reported name alone would have looked like a fix.
        const auto set = Shipped();
        for (const char* proxy : { "ldd_lilicollection_colchokerneck",
                                   "ldd_lilicollection_colskirtfeet",
                                   "ldd_lilicollection_colskirtlegs",
                                   "ldd_lilicollection_colskirtpelvis" }) {
            CHECK(VerdictFor(set, kLiliDress, proxy) == Verdict::kHide);
        }
        // ⚠ THE NEAR MISSES. "col" as a bare substring appears inside
        // LiliCollection itself, so a looser pattern hides the whole mod.
        for (const char* piece : { "ldd_lilicollection_corset",
                                   "ldd_lilicollection_cardigan",
                                   "ldd_lilicollection_panties",
                                   "ldd_lilicollection_pendant",
                                   "ldd_lilicollection_ribbon",
                                   "ldd_lilicollection_boots",
                                   "ldd_lilicollection_rings" }) {
            CHECK(VerdictFor(set, kLiliDress, piece) == Verdict::kNoOpinion);
        }
    }

    {  // ⚠ A SUBSTRING PATH MATCH, NOT A PREFIX, so the UBE conversion's
       // meshes/!ube/... variant is covered by the same one-line scope. A
       // prefix match would have needed a second entry, and whoever added the
       // first would not have known to.
        const auto set = Shipped();
        CHECK(VerdictFor(set, kLiliUbe, "ldd_lilicollection_colchokerneck") ==
              Verdict::kHide);
        // Nothing outside the scope's path is touched, which is the whole
        // reason this is scoped rather than a global token: the load order has
        // 84,343 NIFs and no census could cover them all.
        CHECK(VerdictFor(set, kOtherArmour, "ldd_lilicollection_colchokerneck") ==
              Verdict::kNoOpinion);
        CHECK(VerdictFor(set, kOtherArmour, "some_col_thing") == Verdict::kNoOpinion);
    }

    {  // show beats hide, and ACROSS scopes rather than only within one: two
       // files may cover the same path, and a rescue that only won inside its
       // own file would depend on which file loaded first
        ScopeSet set;
        set.scopes.push_back(Scope{ "ldd/lili collection/", { "*_col*" }, {} });
        set.scopes.push_back(Scope{ "ldd/lili collection/", {}, { "*_colskirtlegs" } });
        CHECK(VerdictFor(set, kLiliDress, "ldd_lilicollection_colskirtlegs") ==
              Verdict::kShow);
        CHECK(VerdictFor(set, kLiliDress, "ldd_lilicollection_colskirtfeet") ==
              Verdict::kHide);

        // Order-independent: the same two scopes the other way round.
        ScopeSet flipped;
        flipped.scopes.push_back(Scope{ "ldd/lili collection/", {}, { "*_colskirtlegs" } });
        flipped.scopes.push_back(Scope{ "ldd/lili collection/", { "*_col*" }, {} });
        CHECK(VerdictFor(flipped, kLiliDress, "ldd_lilicollection_colskirtlegs") ==
              Verdict::kShow);
    }

    {  // ⚠ A SCOPE WITH NO PATH COVERS NOTHING. The loader drops these, and
       // this pins the reason rather than trusting the loader to stay that
       // way: the generous reading applies one mod's hide list to the whole
       // catalog.
        ScopeSet set;
        set.scopes.push_back(Scope{ "", { "*" }, {} });
        CHECK(VerdictFor(set, kLiliDress, "anything") == Verdict::kNoOpinion);
        CHECK(ScopeTagFor(set, kLiliDress).empty());
    }

    {  // ⚠⚠ THE KEY TAG IS EMPTY FOR EVERY SCENE NO SCOPE COVERS, which is
       // what keeps its thumbnail from being orphaned. The alternative was a
       // renderer bump, which rebuilds the WHOLE cache, and the phase 1 rule
       // restated through phase 3 is that orphaning it all is not acceptable.
        const auto set = Shipped();
        CHECK(ScopeTagFor(set, kOtherArmour).empty());
        CHECK(ScopeTagFor(ScopeSet{}, kLiliDress).empty());

        const auto tag = ScopeTagFor(set, kLiliDress);
        CHECK(tag.size() == 8);
        CHECK(ScopeTagFor(set, kLiliUbe) == tag);   // same rules, same tag
    }

    {  // editing the rules re-keys the cards they cover, so a hand-tuned fixup
       // rebuilds exactly what it affects and nothing else
        const auto before = ScopeTagFor(Shipped(), kLiliDress);

        ScopeSet widened;
        widened.scopes.push_back(Scope{ "ldd/lili collection/", { "*_col*", "*_ribbon" }, {} });
        CHECK(ScopeTagFor(widened, kLiliDress) != before);

        // ⚠ MOVING A PATTERN FROM hide TO show MUST CHANGE THE TAG, or the
        // rescued piece reuses the thumbnail built while it was hidden. The
        // two lists are hashed with a separator between them for exactly this.
        ScopeSet hideOne;
        hideOne.scopes.push_back(Scope{ "ldd/lili collection/", { "*_colskirtlegs" }, {} });
        ScopeSet showOne;
        showOne.scopes.push_back(Scope{ "ldd/lili collection/", {}, { "*_colskirtlegs" } });
        CHECK(ScopeTagFor(hideOne, kLiliDress) != ScopeTagFor(showOne, kLiliDress));
    }

    if (g_failures == 0) {
        std::printf("PreviewScopesTests: all passed\n");
        return 0;
    }
    std::printf("PreviewScopesTests: %d failure(s)\n", g_failures);
    return 1;
}
