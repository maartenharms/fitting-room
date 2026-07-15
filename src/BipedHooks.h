#pragma once

namespace OS {

    class BipedHooks {
    public:
        static void Install();
        static bool AllInstalled();

        // Diagnostics: counts player worn-staging passes, so a refresh that
        // silently fails to re-run the pass is visible in the log.
        static std::uint64_t PlayerWornPassCount();

    private:
        static void InstallInjection();     // 24231+0x81
        static void InstallWornMaskShim();  // 24220+0x7C

        static inline bool injectionOk_{ false };
        static inline bool maskShimOk_{ false };
    };

}  // namespace OS
