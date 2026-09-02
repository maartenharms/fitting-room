#pragma once

#include <cstddef>  // std::ptrdiff_t - every offset here is one
#include <cstdint>
#include <cstring>  // std::memcpy - the probe reads engine fields by offset

// Is the GAME reading typed characters right now?
//
// ⚠ THE EDITOR HOTKEY IS A PRINTABLE LETTER, so it has to stand down while
// something else is taking letters. The gate knew about exactly one case, by
// menu name (the console), and Screen Archer Menu's own text fields were not
// covered at all: typing a name with a 'y' in it opened the editor over the
// top of what was being typed (user 2026-08-11). A menu-name list can never
// cover this, because a text field is a thing INSIDE a menu rather than a menu
// of its own - SAM is open either way.
//
// The engine already keeps the answer. Every menu that accepts text raises
// ControlMap::textEntryCount through AllowTextInput and drops it again when the
// field loses focus, and that counter is what makes the rest of the game's
// bindings go quiet while you type. Our own sink is the RAW device feed,
// upstream of the control map, so it never inherited that quiet - which is
// precisely why the editor was the one thing still firing.
//
// ⚠⚠ AND THE COUNTER CANNOT BE READ AT THE OFFSET CommonLib DECLARES. On AE
// 1.6.1170 every ControlMap member past controlMap[] sits 8 bytes later than
// the header says, because the runtime's context array has 18 entries where
// INPUT_CONTEXT_ID::kTotal declares 17. Reading `cm->textEntryCount` there
// returns a neighbouring field, and that fault reads as healthy - it is the
// same shift that once emptied the context stack through ToggleControls and
// cost three sessions of diagnosis aimed at the wrong layer.
//
// So the base is FOUND rather than assumed, once, by looking for the shape of
// the two BSTArrays that sit between the context array and the counter. That is
// a structural check rather than a version key: it needs no runtime table, it
// answers correctly on any build that keeps this layout, and on a build that
// does not it answers "I do not know" instead of a wrong number.
//
// ⚠ THE WHOLE DECISION IS IN THIS HEADER, taking a plain byte pointer, for the
// reason EditorGate.h and DyeRamp.h name: TextEntry.cpp cannot be compiled
// without an engine, and a rule left where no test can reach it reaches the
// field unexercised. The .cpp does the cast and the logging and nothing else.
namespace OS::TextEntry {

    // Offsets WITHIN the ControlMap object, measured from the base of the first
    // of the two arrays. These deltas are the part that does NOT move: what
    // moves is where the pair starts, because the context array in front of it
    // is a different length on different runtimes.
    //
    //   base + 0x00  linkedMappings        BSTArray<LinkedMapping>
    //   base + 0x18  contextPriorityStack  BSTArray<InputContextID>
    //   base + 0x30  enabledControls       u32
    //   base + 0x34  unk11C                u32
    //   base + 0x38  textEntryCount        i8
    inline constexpr std::ptrdiff_t kStackFromBase = 0x18;
    inline constexpr std::ptrdiff_t kCountFromBase = 0x38;

    // The two places the pair can start: where the header puts it (SE, and any
    // AE build whose context array really is kTotal long) and one pointer later
    // (AE 1.6.1170, whose array has one more entry).
    //
    // ⚠ CANDIDATES RATHER THAN A VERSION KEY, and that is the whole point. A
    // table keyed on the runtime version is a promise about builds nobody has
    // run; this asks the object in front of us what shape it is.
    inline constexpr std::ptrdiff_t kBaseCandidates[] = { 0xE8, 0xF0 };

    // Anything above this is a counter that has run away rather than a stack of
    // focused text fields. Menus nest, so the ceiling is not one; it is not
    // hundreds either.
    inline constexpr std::int32_t kMaxSaneCount = 64;

    // Does this triple read like a live BSTArray?
    //
    // BSTArray is { _data @0x00, _capacity @0x08, _size @0x10 } - the canonical
    // SKSE tArray shape - so at the WRONG base every field lands on a
    // neighbour's, and the neighbours here are a pointer and two small counts.
    // Reading a count as `_data` gives a tiny non-null "pointer"; reading a
    // pointer as `_capacity` or `_size` gives an implausibly huge count. Both
    // are refused below, which is what makes one candidate win and the other
    // lose rather than both looking plausible.
    //
    // ⚠ AN EMPTY ARRAY IS LEGAL AND IS NOT A REJECTION. A null _data with a
    // zero capacity is what a never-filled array looks like, and refusing it
    // would make the answer depend on how far through startup we asked.
    [[nodiscard]] constexpr bool LooksLikeArray(std::uint64_t a_data,
                                                std::uint32_t a_capacity,
                                                std::uint32_t a_size) {
        // A user-mode heap pointer, never a count that happened to land here.
        // 0x10000 is the bottom of the address space Windows will ever hand
        // out, so anything below it is a small integer wearing a pointer's
        // clothes.
        constexpr std::uint64_t kLowestPointer = 0x10000;
        // Both arrays here are engine-sized: the context stack is a handful of
        // entries and the linked-mapping table is dozens. A four-figure ceiling
        // is far above either and far below a pointer's low half.
        constexpr std::uint32_t kMaxEntries = 4096;

        if (a_capacity > kMaxEntries || a_size > kMaxEntries) {
            return false;
        }
        if (a_size > a_capacity) {
            return false;
        }
        if (a_capacity == 0) {
            return a_data == 0 && a_size == 0;
        }
        return a_data >= kLowestPointer;
    }

    [[nodiscard]] inline bool ArrayAt(const std::uint8_t* a_at) {
        std::uint64_t data{};
        std::uint32_t capacity{};
        std::uint32_t size{};
        std::memcpy(&data, a_at, sizeof(data));
        std::memcpy(&capacity, a_at + 0x08, sizeof(capacity));
        std::memcpy(&size, a_at + 0x10, sizeof(size));
        return LooksLikeArray(data, capacity, size);
    }

    // Which candidate base actually holds the two arrays, or 0 when the answer
    // is not exactly one of them.
    //
    // ⚠ BOTH FITTING HAS TO BE A REFUSAL rather than a coin toss. A wrong base
    // reads a byte that means something else, and the fault that follows is a
    // hotkey which stops working for no visible reason - the failure mode this
    // whole probe exists to avoid.
    [[nodiscard]] inline std::ptrdiff_t ResolveArrayBase(const std::uint8_t* a_object) {
        std::ptrdiff_t found = 0;
        int            hits  = 0;
        for (const auto candidate : kBaseCandidates) {
            if (ArrayAt(a_object + candidate) &&
                ArrayAt(a_object + candidate + kStackFromBase)) {
                found = candidate;
                ++hits;
            }
        }
        return hits == 1 ? found : 0;
    }

    // Whether a counter read at the resolved offset means "somebody is typing".
    //
    // ⚠ A NONSENSE VALUE IS "NOT TYPING", NEVER "TYPING". This read is the only
    // thing between the player and a hotkey that has gone dead, so every way of
    // being wrong has to fail toward the behaviour that shipped.
    [[nodiscard]] constexpr bool CountMeansTyping(std::int32_t a_count) {
        return a_count > 0 && a_count <= kMaxSaneCount;
    }

    // Is a text field taking keystrokes? False whenever the counter could not
    // be located, which is the safe direction: the hotkey behaves exactly as it
    // did before this existed rather than dying on a number we cannot read.
    //
    // Any thread. Resolves the offset on the first call and caches it.
    [[nodiscard]] bool Active();

}  // namespace OS::TextEntry
