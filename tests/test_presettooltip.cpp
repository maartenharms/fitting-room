// The Presets page is one pane of cards now, so this tooltip IS the detail
// column that used to sit beside it. These tests hold two separate things: the
// bug that started the work, and the fact that nothing the column carried was
// quietly dropped on the way into a hover.

#include "PresetTooltip.h"

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

namespace T = OS::PresetTooltip;

// The shipped $FR_PresetBy, verbatim, two spaces and all.
static constexpr const char* kBy = "by %s  (%s)";

static bool Has(const std::string& a_hay, const std::string& a_needle) {
    return a_hay.find(a_needle) != std::string::npos;
}

static int LineCount(const std::string& a_text) {
    if (a_text.empty()) {
        return 0;
    }
    int n = 1;
    for (const char c : a_text) {
        if (c == '\n') {
            ++n;
        }
    }
    return n;
}

// ---- the bug -----------------------------------------------------------

// A Discovered preset carries the literal placeholder "discovered" as its
// file, on purpose, so the byline can read "(discovered)". Printed alone it was
// the whole tooltip, which is what the user saw.
static void TestDiscoveredPlaceholderNeverStandsAlone() {
    T::Facts f;
    f.author = "Dawnguard";
    f.file   = "discovered";

    const auto tip = T::Compose(f, kBy);
    CHECK(tip != "discovered");
    CHECK(tip == "by Dawnguard  (discovered)");
    CHECK(Has(tip, "Dawnguard"));
}

// The same preset with pieces on it: the placeholder still never opens the
// tooltip on its own, and the byline stays the first line.
static void TestDiscoveredWithPiecesOpensOnTheByline() {
    T::Facts f;
    f.author = "Dawnguard";
    f.file   = "discovered";
    f.pieces = { { "Chest: Vampire Armor" }, { "Feet: Vampire Boots" } };

    const auto tip = T::Compose(f, kBy);
    CHECK(tip.rfind("by Dawnguard  (discovered)", 0) == 0);
    CHECK(Has(tip, "Chest: Vampire Armor"));
}

static void TestAuthorFallsBackTheWayTheDetailPaneDid() {
    T::Facts f;
    f.file = "MyKnight.json";

    CHECK(T::Compose(f, kBy) == "by unknown  (MyKnight.json)");
}

// Nothing to say at all says nothing, rather than an empty byline shell.
static void TestNoAuthorAndNoFileDropsTheByline() {
    T::Facts f;
    CHECK(T::Compose(f, kBy).empty());

    f.description = "A look";
    CHECK(T::Compose(f, kBy) == "A look");
}

// ---- what the detail column carried ------------------------------------

static void TestThePieceListSurvives() {
    T::Facts f;
    f.author = "Skyrim.esm";
    f.file   = "discovered";
    f.pieces = { { "Chest: Steel Cuirass" },
                 { "Hands: Hidden" },
                 { "Feet: <missing plugin: Boots.esp>" } };

    const auto tip = T::Compose(f, kBy);
    CHECK(Has(tip, "Chest: Steel Cuirass"));
    CHECK(Has(tip, "Hands: Hidden"));
    CHECK(Has(tip, "Feet: <missing plugin: Boots.esp>"));
}

// The column coloured an unfit row red. A tooltip has no colour, so the marker
// is the only thing that says WHICH piece the count is about.
static void TestUnfitPiecesAreMarkedInPlace() {
    T::Facts f;
    f.author = "Obi";
    f.file   = "discovered";
    f.pieces = { { "Chest: Abyss Robes", true, "no UBE armature" },
                 { "Feet: Abyss Boots" } };

    const auto tip = T::Compose(f, kBy);
    CHECK(Has(tip, "Chest: Abyss Robes  (may not fit: no UBE armature)"));
    CHECK(Has(tip, "Feet: Abyss Boots"));
    CHECK(!Has(tip, "Feet: Abyss Boots  (may not fit"));
    CHECK(Has(tip, "1 piece may not fit your body"));
}

// ⚠ THE REASON IS CARRIED BECAUSE NOTHING ELSE CAN CARRY IT. The column gave
// it out through a per-piece hover and a tooltip cannot open another tooltip,
// so it either rides the line or it is gone. A piece with no reason still
// marks, since the flag is what the count trusts.
static void TestFitReasonRidesTheLine() {
    T::Facts sexed;
    sexed.pieces = { { "Chest: Steel Cuirass", true, "no female mesh" } };
    CHECK(Has(T::Compose(sexed, kBy),
              "Chest: Steel Cuirass  (may not fit: no female mesh)"));

    T::Facts crashed;
    crashed.pieces = { { "Head: Odd Helm", true, "crashed the preview last time" } };
    CHECK(Has(T::Compose(crashed, kBy),
              "Head: Odd Helm  (may not fit: crashed the preview last time)"));

    T::Facts bare;
    bare.pieces    = { { "Feet: Boots", true } };
    const auto tip = T::Compose(bare, kBy);
    CHECK(Has(tip, "Feet: Boots  (may not fit)"));
    CHECK(Has(tip, "1 piece may not fit your body"));
}

// ⚠ THE COUNT IS THE MARKED PIECES, NOT THE EXPORT HEALTH. The old hover took
// its unfit count from exportHealth, which returns nothing unless the Exported
// tab is up, so a Discovered preset with three pieces that do not fit said
// nothing at all. The detail column ran its own count over every source, and
// this is that count.
static void TestUnfitCountComesFromTheListSoEverySourceGetsIt() {
    T::Facts f;
    f.author     = "Dawnguard";
    f.file       = "discovered";  // NOT an export
    f.pieces = { { "A: one", true }, { "B: two", true }, { "C: three" } };

    const auto tip = T::Compose(f, kBy);
    CHECK(Has(tip, "2 pieces may not fit your body"));
}

static void TestUnfitCountPluralises() {
    T::Facts f;
    f.pieces = { { "A: one", true } };
    CHECK(Has(T::Compose(f, kBy), "1 piece may not fit your body"));
    CHECK(!Has(T::Compose(f, kBy), "1 pieces"));
}

static void TestMissingPluginsPluraliseAndJoin() {
    T::Facts one;
    one.missingPlugins = { "A.esp" };
    CHECK(Has(T::Compose(one, kBy), "Missing required plugin: A.esp"));

    T::Facts two;
    two.missingPlugins = { "A.esp", "B.esp" };
    const auto tip = T::Compose(two, kBy);
    CHECK(Has(tip, "Missing required plugins: A.esp, B.esp"));
    // The half of the column's prose that changes what the player expects to
    // happen, rather than the half explaining why the row is still listed.
    CHECK(Has(tip, "The preset applies without them"));
}

// The caller formats this one, since $FR_Requires is translated. What is held
// here is that it lands in the warning block at all, and the join it is built
// from.
static void TestRequiresLineSurvives() {
    CHECK(T::JoinCommas({ "Obi.esp", "Dint999.esp" }) == "Obi.esp, Dint999.esp");
    CHECK(T::JoinCommas({ "Obi.esp" }) == "Obi.esp");
    CHECK(T::JoinCommas({}).empty());

    T::Facts f;
    f.requiresLine = "Requires: Obi.esp, Dint999.esp";
    CHECK(Has(T::Compose(f, kBy), "Requires: Obi.esp, Dint999.esp"));
}

static void TestOwnershipFractionSurvives() {
    T::Facts f;
    f.loreIncomplete = true;
    f.ownedPieces    = 3;
    f.totalPieces    = 5;
    CHECK(Has(T::Compose(f, kBy), "Lore-friendly collection: 3/5 pieces owned"));

    T::Facts owned;
    owned.ownedPieces = 5;
    owned.totalPieces = 5;
    CHECK(!Has(T::Compose(owned, kBy), "Lore-friendly"));
}

// Card-only, and the reason it exists: a half-built picture looks exactly like
// a smaller outfit.
static void TestPictureNotes() {
    T::Facts none;
    none.hasScene = false;
    CHECK(Has(T::Compose(none, kBy),
              "No picture: none of its pieces are installed"));

    T::Facts part;
    part.sceneComplete = false;
    part.sceneResolved = 3;
    part.sceneTotal    = 9;
    CHECK(Has(T::Compose(part, kBy), "Picture shows 3 of 9 pieces"));

    T::Facts whole;
    CHECK(!Has(T::Compose(whole, kBy), "Picture shows"));
}

// ⚠ THE TITLE ALWAYS LEADS, whatever the name's length. It used to lead only
// when the card had trimmed it, which gave "Dawnguard (Light)" a title and
// "Falmer" none, and two cards side by side had differently shaped tooltips for
// no reason a player could see.
static void TestTheTitleAlwaysLeads() {
    T::Facts shortName;
    shortName.name   = "Falmer";
    shortName.author = "Dawnguard";
    shortName.file   = "discovered";
    CHECK(T::Compose(shortName, kBy).rfind("Falmer\nby Dawnguard", 0) == 0);

    T::Facts longName;
    longName.name   = "A Very Long Preset Name Indeed";
    longName.author = "Dawnguard";
    longName.file   = "discovered";
    CHECK(T::Compose(longName, kBy)
              .rfind("A Very Long Preset Name Indeed\nby Dawnguard", 0) == 0);

    // Both shapes are the same shape, which is the whole point.
    CHECK(LineCount(T::Compose(shortName, kBy)) ==
          LineCount(T::Compose(longName, kBy)));

    // A preset with no name at all still opens on the byline rather than on a
    // blank line.
    T::Facts nameless;
    nameless.author = "Dawnguard";
    nameless.file   = "discovered";
    CHECK(T::Compose(nameless, kBy).rfind("by ", 0) == 0);
}

// ---- the house rule ----------------------------------------------------

static void TestNeverEndsInAPeriod() {
    T::Facts prose;
    prose.description = "A heavy set for the road.";
    CHECK(T::Compose(prose, kBy) == "A heavy set for the road");

    // Whichever clause happens to land last, and which one that is depends
    // entirely on what is wrong with the preset.
    T::Facts lore;
    lore.loreIncomplete = true;
    lore.ownedPieces    = 1;
    lore.totalPieces    = 4;
    const auto tip = T::Compose(lore, kBy);
    CHECK(!tip.empty() && tip.back() != '.');

    T::Facts pieces;
    pieces.pieces = { { "Chest: Steel Cuirass" } };
    const auto ptip = T::Compose(pieces, kBy);
    CHECK(!ptip.empty() && ptip.back() != '.');
}

// ---- shape -------------------------------------------------------------

// A blank line separates two blocks that BOTH have content. A tooltip that
// opens on an empty line or trails one is the failure this catches.
static void TestNoDanglingBlankLines() {
    T::Facts only;
    only.pieces = { { "Chest: Steel Cuirass" } };
    CHECK(T::Compose(only, kBy) == "Chest: Steel Cuirass");

    T::Facts warn;
    warn.requiresLine = "Requires: A.esp";
    CHECK(T::Compose(warn, kBy) == "Requires: A.esp");

    T::Facts both;
    both.author    = "Dawnguard";
    both.file      = "discovered";
    both.pieces    = { { "Chest: Steel Cuirass" } };
    both.requiresLine = "Requires: A.esp";
    const auto tip = T::Compose(both, kBy);
    CHECK(tip ==
          "by Dawnguard  (discovered)\n"
          "\n"
          "Chest: Steel Cuirass\n"
          "\n"
          "Requires: A.esp");
    CHECK(tip.front() != '\n');
    CHECK(tip.back() != '\n');
}

// A full outfit's hover is long because the column it replaces was long, but it
// should not be unbounded by accident. Six dressed slots plus a byline plus two
// warnings is the realistic worst case a Discovered set produces.
static void TestAFullPresetStaysReadable() {
    T::Facts f;
    f.author      = "Obi";
    f.file        = "discovered";
    f.description = "The Abyss set.";
    f.pieces      = { { "Chest: Abyss Robes" }, { "Hands: Abyss Gloves" },
                      { "Feet: Abyss Boots" },  { "Head: Abyss Hood" },
                      { "Ring: Abyss Ring" },   { "Amulet: Abyss Amulet" } };
    f.requiresLine = "Requires: Obi.esp";

    const auto tip = T::Compose(f, kBy);
    // byline, description, blank, 6 pieces, blank, requires.
    CHECK(LineCount(tip) == 11);
    CHECK(tip.back() != '.');
}

// ---- width and length --------------------------------------------------

// ⚠ A TOOLTIP NEITHER WRAPS NOR SCROLLS. The column did both, so a paragraph
// that read as five lines in a fixed-width child arrives here as one line
// wider than the screen unless it is broken here.
static void TestLongDescriptionsWrap() {
    T::Facts f;
    f.description =
        "A heavy set of plate for the road north, cut for a soldier who "
        "expects weather and does not expect company.";

    const auto  tip   = T::Compose(f, kBy);
    std::size_t start = 0;
    CHECK(LineCount(tip) > 1);
    for (;;) {
        const auto nl  = tip.find('\n', start);
        const auto end = nl == std::string::npos ? tip.size() : nl;
        CHECK(end - start <= T::kWrapColumns);
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
}

// A word longer than the column is left alone: a hard split through a plugin
// name or a path is worse to read than one long line.
static void TestWrapNeverSplitsAWord() {
    const std::string beast(90, 'x');
    CHECK(T::Wrap(beast, 20) == beast);
    CHECK(T::Wrap("short " + beast, 20) == "short\n" + beast);
}

static void TestWrapKeepsExistingNewlines() {
    CHECK(T::Wrap("one\ntwo", 72) == "one\ntwo");
    CHECK(T::Wrap("", 72).empty());
    CHECK(T::Wrap("under", 72) == "under");
    // Past the early-out, where the newline branch actually runs.
    CHECK(T::Wrap("aaa bbb\nccc ddd", 5) == "aaa\nbbb\nccc\nddd");
}

// The one field a mod author writes free-hand, and the only one that can run
// away with the tooltip's height.
static void TestRunawayDescriptionIsClipped() {
    T::Facts f;
    f.description = std::string(1200, 'a') + " tail";

    const auto tip = T::Compose(f, kBy);
    CHECK(tip.size() < 1024);
    CHECK(!Has(tip, "tail"));
    // ⚠ AND THE ELLIPSIS SURVIVES THE NO-TRAILING-STOP RULE. A clipped
    // description can be the last thing in the tooltip, and popping one dot
    // off "..." leaves ".." reading as a typo.
    CHECK(Has(tip, "..."));
    CHECK(tip.substr(tip.size() - 3) == "...");

    // A description that fits is untouched, ellipsis and all.
    T::Facts small;
    small.description = "A short one";
    CHECK(T::Compose(small, kBy) == "A short one");
}

static void TestClipLandsOnAWordBoundary() {
    CHECK(T::Clip("alpha beta gamma delta", 12) == "alpha beta...");
    CHECK(T::Clip("short", 40) == "short");
}

// A format the translator broke should not take the editor with it.
static void TestNullFormatIsSurvivable() {
    T::Facts f;
    f.author = "Dawnguard";
    f.file   = "discovered";
    f.pieces = { { "Chest: Steel Cuirass" } };

    const auto tip = T::Compose(f, nullptr);
    CHECK(tip == "Chest: Steel Cuirass");
}

int main() {
    TestDiscoveredPlaceholderNeverStandsAlone();
    TestDiscoveredWithPiecesOpensOnTheByline();
    TestAuthorFallsBackTheWayTheDetailPaneDid();
    TestNoAuthorAndNoFileDropsTheByline();
    TestThePieceListSurvives();
    TestUnfitPiecesAreMarkedInPlace();
    TestFitReasonRidesTheLine();
    TestUnfitCountComesFromTheListSoEverySourceGetsIt();
    TestUnfitCountPluralises();
    TestMissingPluginsPluraliseAndJoin();
    TestRequiresLineSurvives();
    TestOwnershipFractionSurvives();
    TestPictureNotes();
    TestTheTitleAlwaysLeads();
    TestNeverEndsInAPeriod();
    TestNoDanglingBlankLines();
    TestAFullPresetStaysReadable();
    TestLongDescriptionsWrap();
    TestWrapNeverSplitsAWord();
    TestWrapKeepsExistingNewlines();
    TestRunawayDescriptionIsClipped();
    TestClipLandsOnAWordBoundary();
    TestNullFormatIsSurvivable();

    if (g_failures == 0) {
        std::printf("PresetTooltipTests: all passed\n");
        return 0;
    }
    std::printf("PresetTooltipTests: %d failure(s)\n", g_failures);
    return 1;
}
