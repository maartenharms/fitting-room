// Pure-logic tests for the direct-entry key. No engine, no RE:: types.
#include "DirectEntry.h"

#include <cstdio>
#include <initializer_list>  // the sweeps below range over braced lists

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::DirectEntry;

    {  // ⚠⚠ UNASSIGNED IS THE SHIPPED STATE AND IT MUST DO NOTHING AT ALL.
        // Every other input is swept under it, because "unbound" is the one
        // answer that cannot depend on what else is true: this key exists in
        // every save whether or not anyone has bound it.
        for (bool editorOpen : { false, true }) {
            for (bool hostOpen : { false, true }) {
                for (bool loreOk : { false, true }) {
                    for (bool typing : { false, true }) {
                        for (bool pending : { false, true }) {
                            Ask ask;
                            ask.bound         = false;
                            ask.editorOpen    = editorOpen;
                            ask.hostOpen      = hostOpen;
                            ask.loreOk        = loreOk;
                            ask.typing        = typing;
                            ask.summonPending = pending;
                            CHECK(Decide(ask) == Entry::kIgnore);
                        }
                    }
                }
            }
        }
    }

    {  // Bound, in the world, nothing in the way: summon a host and open on it.
        Ask ask;
        ask.bound = true;
        CHECK(Decide(ask) == Entry::kSummonHost);
    }

    {  // A host is already up (the player opened their inventory themselves),
        // so there is nothing to summon and the editor opens where it stands.
        Ask ask;
        ask.bound    = true;
        ask.hostOpen = true;
        CHECK(Decide(ask) == Entry::kOpenHere);
    }

    {  // ⚠⚠ THE CLOSE EDGE OUTRANKS EVERY REFUSAL. A player inside the editor
        // leaves with the key that opened it, whatever the lore gate says now:
        // a Seamstone sold or dropped while the editor is open would otherwise
        // lock the door from the inside.
        for (bool hostOpen : { false, true }) {
            for (bool loreOk : { false, true }) {
                for (bool pending : { false, true }) {
                    Ask ask;
                    ask.bound         = true;
                    ask.editorOpen    = true;
                    ask.hostOpen      = hostOpen;
                    ask.loreOk        = loreOk;
                    ask.summonPending = pending;
                    CHECK(Decide(ask) == Entry::kCloseEditor);
                }
            }
        }
        // ⚠ AND TYPING DOES NOT TRAP THEM EITHER, which is the one case where
        // this rule and the open-side rule disagree on purpose: a text field
        // inside the editor is the editor's own, and the close edge is how the
        // player gets out of it.
        Ask typingInside;
        typingInside.bound      = true;
        typingInside.editorOpen = true;
        typingInside.typing     = true;
        CHECK(Decide(typingInside) == Entry::kCloseEditor);
    }

    {  // Typing beats opening. A bind can be a printable letter, and a name
        // being typed into Screen Archer Menu or a console line belongs to
        // whoever asked for it.
        for (bool hostOpen : { false, true }) {
            for (bool loreOk : { false, true }) {
                Ask ask;
                ask.bound    = true;
                ask.typing   = true;
                ask.hostOpen = hostOpen;
                ask.loreOk   = loreOk;
                CHECK(Decide(ask) == Entry::kIgnore);
            }
        }
    }

    {  // Lore mode without the Seamstone refuses, in the world and over a host
        // alike, and the refusal is its own answer rather than kIgnore so the
        // caller can log which of the two happened. The field cannot tell "the
        // key did nothing" from "the key never arrived" without that line.
        Ask world;
        world.bound  = true;
        world.loreOk = false;
        CHECK(Decide(world) == Entry::kRefusedLore);

        Ask hosted;
        hosted.bound    = true;
        hosted.loreOk   = false;
        hosted.hostOpen = true;
        CHECK(Decide(hosted) == Entry::kRefusedLore);
    }

    {  // ⚠⚠ A SECOND PRESS WHILE THE INVENTORY IS ON ITS WAY MUST NOT ASK
        // AGAIN. The menu arrives a frame or two later and the open rides its
        // event, so a double tap would queue two shows and leave the second
        // pending after the first was consumed.
        Ask ask;
        ask.bound         = true;
        ask.summonPending = true;
        CHECK(Decide(ask) == Entry::kIgnore);

        // ⚠ BUT PENDING NEVER BLOCKS A HOST THAT IS ALREADY THERE. If the menu
        // landed and the flag has not been cleared yet, the press that follows
        // opens rather than doing nothing, which is the difference between a
        // key that feels instant and one that eats the second press.
        Ask arrived;
        arrived.bound         = true;
        arrived.summonPending = true;
        arrived.hostOpen      = true;
        CHECK(Decide(arrived) == Entry::kOpenHere);
    }

    if (g_failures == 0) {
        std::printf("DirectEntryTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
