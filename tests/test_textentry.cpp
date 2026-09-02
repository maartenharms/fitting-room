// Pure-logic tests for the ControlMap text-entry probe. No engine, no RE::
// types: the header does the whole decision against a byte pointer precisely so
// this suite can build the two real layouts by hand and prove the probe picks
// the right one.
#include "TextEntry.h"

#include <array>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    // Big enough for either layout with room past the counter.
    using Object = std::array<std::uint8_t, 0x180>;

    // A value with a LARGE low half, which is what makes a pointer read as an
    // absurd count when a wrong base lands on it. A fake pointer whose low
    // dword happened to be small would make this suite agree with a probe that
    // does not actually discriminate.
    constexpr std::uint64_t kHeapPointer = 0x000001F2ABCD0000ull;

    void PutU64(Object& a_obj, std::ptrdiff_t a_at, std::uint64_t a_value) {
        std::memcpy(a_obj.data() + a_at, &a_value, sizeof(a_value));
    }

    void PutU32(Object& a_obj, std::ptrdiff_t a_at, std::uint32_t a_value) {
        std::memcpy(a_obj.data() + a_at, &a_value, sizeof(a_value));
    }

    void PutArray(Object& a_obj, std::ptrdiff_t a_at, std::uint64_t a_data,
                  std::uint32_t a_capacity, std::uint32_t a_size) {
        PutU64(a_obj, a_at, a_data);
        PutU32(a_obj, a_at + 0x08, a_capacity);
        PutU32(a_obj, a_at + 0x10, a_size);
    }

    // ControlMap as the runtime lays it out. a_contexts is the length of
    // controlMap[], which is the ONE thing that differs between the two
    // runtimes and the reason the members behind it move at all: 17 on the
    // layout CommonLibSSE-NG declares, 18 on AE 1.6.1170.
    Object MakeControlMap(std::size_t a_contexts, std::int8_t a_textEntryCount,
                          std::uint32_t a_stackSize = 2) {
        Object obj{};
        obj.fill(0);
        // The vtable and the head of the object. Anything plausible: nothing
        // reads it, but leaving it zero would make an accidental match easier
        // than the real thing ever is.
        PutU64(obj, 0x00, kHeapPointer);
        constexpr std::ptrdiff_t kContextArray = 0x60;
        for (std::size_t i = 0; i < a_contexts; ++i) {
            PutU64(obj, kContextArray + static_cast<std::ptrdiff_t>(i * 8),
                   kHeapPointer + i * 0x100);
        }
        const auto base =
            kContextArray + static_cast<std::ptrdiff_t>(a_contexts * 8);
        PutArray(obj, base, kHeapPointer, 64, 54);                    // linkedMappings
        PutArray(obj, base + 0x18, kHeapPointer, 8, a_stackSize);     // priority stack
        PutU32(obj, base + 0x30, 0x1);                                // enabledControls
        PutU32(obj, base + 0x34, 0x0);                                // unk11C
        obj[static_cast<std::size_t>(base + 0x38)] =
            static_cast<std::uint8_t>(a_textEntryCount);
        return obj;
    }

    std::int8_t CountAt(const Object& a_obj, std::ptrdiff_t a_base) {
        return static_cast<std::int8_t>(
            a_obj[static_cast<std::size_t>(a_base + OS::TextEntry::kCountFromBase)]);
    }

}  // namespace

int main() {
    using namespace OS::TextEntry;

    {  // The array predicate, on its own.
        CHECK(LooksLikeArray(kHeapPointer, 8, 2));
        CHECK(LooksLikeArray(0, 0, 0));  // never filled is still an array
        // A count read where the pointer should be.
        CHECK(!LooksLikeArray(64, 54, 2));
        // A pointer read where a count should be.
        CHECK(!LooksLikeArray(kHeapPointer, 0xABCD0000u, 2));
        CHECK(!LooksLikeArray(kHeapPointer, 8, 0xABCD0000u));
        // More entries than the array can hold is not an array.
        CHECK(!LooksLikeArray(kHeapPointer, 8, 9));
        // A capacity with nowhere to live.
        CHECK(!LooksLikeArray(0, 8, 2));
    }

    {  // The layout CommonLibSSE-NG declares: 17 contexts, arrays at 0xE8.
        const auto obj = MakeControlMap(17, 0);
        CHECK(ResolveArrayBase(obj.data()) == 0xE8);
        CHECK(CountAt(obj, 0xE8) == 0);
    }

    {  // AE 1.6.1170: one more context, everything behind it eight bytes later.
       //
       // ⚠⚠ THIS IS THE CASE THAT MADE THE PROBE NECESSARY, AND THE BYTE THE
       // DECLARED OFFSET WOULD HAVE READ IS WORSE THAN GARBAGE. On this layout
       // 0x120 is the LOW BYTE OF enabledControls, which is 1 in free roam -
       // so a build that trusted the header would have decided the player was
       // typing at all times and killed the hotkey outright, with no crash and
       // no log line to say why. That is the shape of this whole family of
       // faults: the wrong read is not obviously wrong, it is plausible.
        const auto obj = MakeControlMap(18, 3);
        CHECK(ResolveArrayBase(obj.data()) == 0xF0);
        CHECK(CountAt(obj, 0xF0) == 3);
        CHECK(obj[0x120] == 0x1);                          // enabledControls, not the count
        CHECK(CountMeansTyping(static_cast<std::int32_t>(obj[0x120])));  // and it lies
        // The SE layout has the counter exactly where the header says, so the
        // probe has to agree with the header there rather than "fixing" it.
        const auto se = MakeControlMap(17, 3);
        CHECK(0xE8 + kCountFromBase == 0x120);
        CHECK(se[0x120] == 3);
    }

    {  // A counter that is actually raised, at both bases, read back whole.
        for (const std::int8_t typed : { std::int8_t{ 1 }, std::int8_t{ 2 } }) {
            CHECK(CountAt(MakeControlMap(17, typed), 0xE8) == typed);
            CHECK(CountAt(MakeControlMap(18, typed), 0xF0) == typed);
        }
    }

    {  // An empty priority stack still resolves. The probe runs on the first
       // hotkey press rather than at load, but "the stack happens to be empty"
       // must not be the thing that decides the answer.
        CHECK(ResolveArrayBase(MakeControlMap(17, 0, 0).data()) == 0xE8);
        CHECK(ResolveArrayBase(MakeControlMap(18, 0, 0).data()) == 0xF0);
    }

    {  // Neither candidate fits: refuse rather than guess. A run of zeroes is
       // the degenerate case where both candidates read as empty arrays, and
       // two hits has to answer the same as none.
        Object zeros{};
        zeros.fill(0);
        CHECK(ResolveArrayBase(zeros.data()) == 0);
        // Garbage that is not array-shaped at either base.
        Object noise{};
        noise.fill(0xCD);
        CHECK(ResolveArrayBase(noise.data()) == 0);
    }

    {  // What a resolved counter means.
        CHECK(!CountMeansTyping(0));
        CHECK(CountMeansTyping(1));
        CHECK(CountMeansTyping(kMaxSaneCount));
        // ⚠ EVERY WAY OF BEING WRONG FAILS TOWARD THE OLD BEHAVIOUR. A
        // negative byte (a counter that has been decremented past zero by some
        // other mod's unbalanced call) and a runaway one both have to read as
        // "not typing", or the hotkey dies and nothing says why.
        CHECK(!CountMeansTyping(-1));
        CHECK(!CountMeansTyping(kMaxSaneCount + 1));
        CHECK(!CountMeansTyping(127));
    }

    if (g_failures == 0) {
        std::printf("test_textentry: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
