#include "PCH.h"

#include "MenuHandlerVtable.h"

#include <array>

namespace OS::MenuHandlerVtable {
    namespace {
        // The two entries 1.7.99 put ahead of the handlers. The game hands
        // them a MotionGestureEvent and a SixaxisEvent; both are declined.
        bool DeclineEvent(RE::MenuEventHandler*, void*) { return false; }

        // One rebuilt table per handler object, in storage the game can never
        // outlive. Two handlers exist today; the pool says so should a third
        // arrive without a row.
        struct Table {
            std::array<void*, 8> slots{};
        };
        std::array<Table, 4> g_tables{};
        std::size_t          g_used{ 0 };

        void** VtableOf(Impl* a_handler) { return *reinterpret_cast<void***>(a_handler); }
    }

    const char* Install(Impl* a_handler) {
        if (!a_handler) {
            return "no handler";
        }
        if (!REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
            return "1.5.97/1.6 layout, as compiled";
        }
        for (std::size_t i = 0; i < g_used; ++i) {
            if (VtableOf(a_handler) == g_tables[i].slots.data()) {
                return "1.7.99 layout, already rebuilt";
            }
        }
        if (g_used >= g_tables.size()) {
            return "NO TABLE LEFT for the 1.7.99 layout, so the handler keeps the 1.6 one";
        }
        auto* const built = VtableOf(a_handler);
        auto&       table = g_tables[g_used++];
        table.slots[0] = built[0];                                 // the deleting destructor
        table.slots[1] = built[1];                                 // CanProcess
        table.slots[2] = reinterpret_cast<void*>(&DeclineEvent);  // ProcessMotionGesture
        table.slots[3] = reinterpret_cast<void*>(&DeclineEvent);  // ProcessSixaxis
        table.slots[4] = built[2];                                 // ProcessKinect
        table.slots[5] = built[3];                                 // ProcessThumbstick
        table.slots[6] = built[4];                                 // ProcessMouseMove
        table.slots[7] = built[5];                                 // ProcessButton
        *reinterpret_cast<void***>(a_handler) = table.slots.data();
        return "1.7.99 layout, rebuilt with two declined entries ahead of the four handlers";
    }
}
