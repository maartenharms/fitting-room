#include "BuildChannel.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
    int g_failures = 0;

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #expr << '\n'; \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)
}

int main() {
    using namespace OS::BuildChannel;

    // ⚠ THREE CONFIGURATIONS, BECAUSE THE FEATURE AND THE CHANNEL ARE SEPARATE
    // AXES. The middle one is the shape the development line actually ships:
    // the Body Studio workbench compiled in while every path and the co-save
    // owner stay exactly as the release build leaves them. It is the case that
    // regresses silently, because a build with the feature accidentally
    // dragging the isolated channel along still runs, still logs, and simply
    // cannot see any outfit the player already saved.

#if FR_TEST_EXPECT_DEV
    // Isolated development channel: feature on, every identity moved.
    CHECK(kBodyStudioDev);
    CHECK(kBodyStudio);  // the channel must carry the feature it exists to test
    CHECK(kBuildId == "test-build");
    CHECK(DataRoot().generic_string() ==
          "Data/SKSE/Plugins/FittingRoom.BodyStudioDev/test-build");
    CHECK(IniPath() == DataPath("FittingRoom.ini"));
    CHECK(CustomBodyPresetRoot() == DataPath("BodyPresets"));
    CHECK(BodySlideExportPath().filename() ==
          "FittingRoom.BodyStudioDev.test-build.xml");
    CHECK(LogName() == "FittingRoom.BodyStudioDev.test-build.log");
    CHECK(PreviousLogName() == "FittingRoom.BodyStudioDev.test-build.prev.log");
    CHECK(kSerializationUniqueId == static_cast<std::uint32_t>('FBSD'));
#elif defined(FR_BODY_STUDIO)
    // The development line's build: feature on, channel untouched. Every
    // assertion below is deliberately identical to the release branch except
    // kBodyStudio itself.
    CHECK(!kBodyStudioDev);
    CHECK(kBodyStudio);
    CHECK(kBuildId == "release");
    CHECK(DataRoot().generic_string() == "Data/SKSE/Plugins/FittingRoom");
    CHECK(IniPath().generic_string() == "Data/SKSE/Plugins/FittingRoom.ini");
    CHECK(CustomBodyPresetRoot().generic_string() ==
          "Data/SKSE/Plugins/FittingRoom/BodyPresets");
    CHECK(BodySlideExportPath().generic_string() ==
          "Data/CalienteTools/BodySlide/SliderPresets/FittingRoom.xml");
    CHECK(LogName() == "FittingRoom.log");
    CHECK(PreviousLogName() == "FittingRoom.prev.log");
    // ⚠ THE ONE THAT MATTERS. An 'FBSD' here means the build cannot read a
    // single outfit written by any previous Fitting Room, which presents as
    // total data loss rather than as a build flag.
    CHECK(kSerializationUniqueId == static_cast<std::uint32_t>('OSLT'));
#else
    // Release: neither axis. No Body Studio in the binary at all.
    CHECK(!kBodyStudioDev);
    CHECK(!kBodyStudio);
    CHECK(kBuildId == "release");
    CHECK(DataRoot().generic_string() == "Data/SKSE/Plugins/FittingRoom");
    CHECK(IniPath().generic_string() == "Data/SKSE/Plugins/FittingRoom.ini");
    CHECK(BodySlideExportPath().generic_string() ==
          "Data/CalienteTools/BodySlide/SliderPresets/FittingRoom.xml");
    CHECK(LogName() == "FittingRoom.log");
    CHECK(PreviousLogName() == "FittingRoom.prev.log");
    CHECK(kSerializationUniqueId == static_cast<std::uint32_t>('OSLT'));
#endif

    if (g_failures == 0) {
        std::cout << "BuildChannelTests: all passed\n";
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
