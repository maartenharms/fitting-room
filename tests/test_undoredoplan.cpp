#include "UndoRedoPlan.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace P = OS::UndoRedoPlan;
using A     = P::Action;

int main() {
    // ---- the three bindings, which are the whole contract ------------------
    CHECK(P::Decide(/*ctrl*/ true, /*shift*/ false, /*z*/ true, /*y*/ false) == A::kUndo);
    CHECK(P::Decide(true, false, false, true) == A::kRedo);
    CHECK(P::Decide(true, true, true, false) == A::kRedo);

    // ⚠ THE ONE THAT ACTUALLY BREAKS. Z is in both chords, so an implementation
    // that answers "z pressed?" before it asks about shift turns the redo
    // binding into a second undo: the player presses Ctrl+Shift+Z to come back
    // and goes one step further away instead. Pinned above and restated here so
    // the reason survives a refactor of the branch order.
    CHECK(P::Decide(true, true, true, false) != A::kUndo);

    // ---- without the modifier there is nothing to do -----------------------
    // Plain Z and plain Y belong to whatever else reads the keyboard.
    CHECK(P::Decide(false, false, true, false) == A::kNone);
    CHECK(P::Decide(false, false, false, true) == A::kNone);
    CHECK(P::Decide(false, true, true, true) == A::kNone);

    // ---- the modifier alone is not a step ----------------------------------
    CHECK(P::Decide(true, false, false, false) == A::kNone);
    CHECK(P::Decide(true, true, false, false) == A::kNone);

    // ---- both keys in one frame -------------------------------------------
    // Undo wins, because the unshifted Z is the more specific chord and the
    // alternative is a frame that redoes while the player is holding undo.
    CHECK(P::Decide(true, false, true, true) == A::kUndo);
    // With shift held neither Z nor Y can mean undo, so this is redo.
    CHECK(P::Decide(true, true, true, true) == A::kRedo);

    // ---- Ctrl+Shift+Y is still redo ---------------------------------------
    // Nobody presses it deliberately, but Y is unambiguous and refusing it
    // because a modifier the binding does not care about is down would be a
    // dead key for no reason.
    CHECK(P::Decide(true, true, false, true) == A::kRedo);

    // ---- constexpr, so a wrong answer can fail the BUILD -------------------
    static_assert(P::Decide(true, false, true, false) == A::kUndo);
    static_assert(P::Decide(true, true, true, false) == A::kRedo);
    static_assert(P::Decide(false, false, true, false) == A::kNone);

    if (g_failures == 0) {
        std::printf("UndoRedoPlanTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
