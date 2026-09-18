#pragma once

// What happens to the preview grid's hand-built BSStream once a parse is
// over, pinned where a test can read it. NifModelLoader.cpp carries the
// evidence; this file carries the rule.
//
// The engine's destructor (NiStream::~NiStream, AE 70325) deletes the input
// stream it finds in iStr and walks every object the parse allocated. A parse
// the engine FAULTED out of (an SEH exception inside LoadStream, swallowed by
// the loader's __try) leaves both in a state that destructor was never
// written for: the input slot still pointing at the loader's own stack
// object, a block half built. Field 2026-09-15, crash-2026-09-15-12-01-44.log:
// `call [rax]` with rax 0 at 70325+0x3A, a UBE Outfit Studio export on the
// stack. So a faulted parse is abandoned, its buffer leaked once, and every
// other outcome gets the destructor the engine's own loader would run.
namespace OS::NifStreamTeardown {

    enum class Parse {
        kNotRun,   // the file never opened, or Load was never called
        kRefused,  // the engine returned false and cleaned up on its way out
        kFaulted,  // an SEH exception escaped the engine's parser
        kLoaded,   // the tree is out and referenced; the stream can go
    };

    enum class Teardown {
        kEngineDestructor,
        kAbandon,
    };

    [[nodiscard]] constexpr Teardown For(Parse a_parse) noexcept {
        return a_parse == Parse::kFaulted ? Teardown::kAbandon
                                          : Teardown::kEngineDestructor;
    }

}  // namespace OS::NifStreamTeardown
