// Pure-logic tests for the one list of menus the editor may be hosted by.
// No engine, no RE:: types - HostGuard.h keeps the names as literals precisely
// so this suite can run, and HostGuard.cpp static_asserts them against the
// engine's own constants so the literals cannot drift.
#include "HostGuard.h"

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

int main() {
    using namespace OS::HostGuard;

    const std::string sam = "ScreenArcherMenu";

    {  // Nothing open is no host, and that is what refuses an open.
        const auto h = ClassifyHost(false, false, false, false, sam);
        CHECK(!h.Present());
        CHECK(!h.isSam);
        CHECK(h.name.empty());
    }
    {  // Each of the three on its own.
        const auto inv = ClassifyHost(true, false, false, false, sam);
        CHECK(inv.Present());
        CHECK(inv.name == "InventoryMenu");
        CHECK(!inv.isSam);

        const auto magic = ClassifyHost(false, false, true, false, sam);
        CHECK(magic.Present());
        CHECK(magic.name == "MagicMenu");
        CHECK(!magic.isSam);

        const auto s = ClassifyHost(false, false, false, true, sam);
        CHECK(s.Present());
        CHECK(s.name == sam);
        CHECK(s.isSam);

        const auto grid = ClassifyHost(false, true, false, false, sam);
        CHECK(grid.Present());
        CHECK(grid.name == "GridInventoryMenu");
        CHECK(!grid.isSam);
    }
    {  // Grid Inventory REPLACES the vanilla inventory, so the interesting case
       // is the one the field actually hits: nothing vanilla open, the grid up,
       // and the editor allowed. The refusal this fixes was HostMenuOpen()
       // answering false there and the hotkey logging "neither the inventory
       // nor Screen Archer Menu is open".
        CHECK(ClassifyHost(false, true, false, false, sam).Present());
        // It outranks SAM and the magic menu, the same way the vanilla
        // inventory does, and loses to the vanilla inventory itself on the rig
        // where both somehow open.
        CHECK(ClassifyHost(false, true, true, true, sam).name == "GridInventoryMenu");
        CHECK(ClassifyHost(true, true, false, false, sam).name == "InventoryMenu");
        // ⚠ THE NAME IS CARRIED VERBATIM because it is what the close gate hands
        // back. Grid Inventory draws with ImGui and has no Scaleform movie to
        // restore, but the host still has to be nameable: a host that reads as
        // "present" with no name is the split Host::Present exists to close.
        CHECK(!ClassifyHost(false, true, false, false, sam).name.empty());
    }
    {  // ⚠ THE PRECEDENCE, WHICH IS THE ONE THING A REWRITE COULD SILENTLY
       // INVERT. The code this replaced read "SAM and not the inventory", so
       // the inventory has always won; a SAM verdict for an inventory-hosted
       // editor would leave the camera to SAM while SAM is not framing
       // anything, and would alpha-hide the wrong menu on top of that.
        const auto both = ClassifyHost(true, false, false, true, sam);
        CHECK(both.name == "InventoryMenu");
        CHECK(!both.isSam);

        const auto all = ClassifyHost(true, false, true, true, sam);
        CHECK(all.name == "InventoryMenu");
        CHECK(!all.isSam);

        const auto magicOverSam = ClassifyHost(false, false, true, true, sam);
        CHECK(magicOverSam.name == "MagicMenu");
        CHECK(!magicOverSam.isSam);
    }
    {  // ⚠ AN UNNAMED SAM IS NOT A HOST. sSamMenuName empty means SAM
       // compatibility is switched off; a host with an empty name would read as
       // "no host" to Present() anyway, so the two answers must not disagree -
       // a gate saying "open allowed" while the alpha-hide has no menu to name
       // is exactly the split this struct exists to close.
        const auto none = ClassifyHost(false, false, false, true, "");
        CHECK(!none.Present());
        CHECK(!none.isSam);
        // And the vanilla hosts are unaffected by SAM being switched off.
        CHECK(ClassifyHost(true, false, false, true, "").name == "InventoryMenu");
        CHECK(ClassifyHost(false, false, true, true, "").name == "MagicMenu");
    }
    {  // A renamed SAM is carried through verbatim, because the name is what
       // gets its alpha handed back.
        const auto renamed = ClassifyHost(false, false, false, true, "SAMMenu");
        CHECK(renamed.name == "SAMMenu");
        CHECK(renamed.isSam);
    }
    {  // Present() is the gate, so it has to agree with the name in every
       // combination rather than only in the ones spelled out above.
        for (bool inv : { false, true }) {
            for (bool grid : { false, true }) {
                for (bool magic : { false, true }) {
                    for (bool s : { false, true }) {
                        const auto h        = ClassifyHost(inv, grid, magic, s, sam);
                        const bool expected = inv || grid || magic || s;
                        CHECK(h.Present() == expected);
                        CHECK(h.Present() == !h.name.empty());
                        // isSam is never set without SAM being the answer.
                        CHECK(!h.isSam || h.name == sam);
                    }
                }
            }
        }
    }

    if (g_failures == 0) {
        std::printf("HostGuardTests: all checks passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
