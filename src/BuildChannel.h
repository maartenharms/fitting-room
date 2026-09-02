#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef FR_BUILD_ID
#define FR_BUILD_ID "release"
#endif

#ifndef FR_DATA_SUBDIR
#define FR_DATA_SUBDIR "FittingRoom"
#endif

namespace OS::BuildChannel {

    // ⚠ TWO AXES, AND CONFLATING THEM IS THE MISTAKE THIS SPLIT FIXES.
    //
    // kBodyStudio says the authoring workbench is COMPILED IN. kBodyStudioDev
    // says the build runs on an ISOLATED DATA CHANNEL: its own data root, its
    // own INI, its own log and BodySlide export names, and 'FBSD' rather than
    // 'OSLT' as the co-save owner.
    //
    // One flag used to mean both, so the only way to get the Body Studio page
    // was to take the isolated channel with it. That build could not read the
    // live FittingRoom.ini or any outfit written under 'OSLT', so turning the
    // feature on read as "all my outfits and settings are gone". They were
    // not gone, they were addressed to a different owner, but that is no
    // comfort while looking at an empty editor.
    //
    // The development line wants the feature WITHOUT the isolation: one build
    // with the dye work and Body Studio in it, still reading the same INI and
    // the same co-save records it always did. The isolated channel remains for
    // what it was actually for, which is a field matrix that must not touch
    // release data.
    //
    // The channel implies the feature; the feature does not imply the channel.
#if defined(FR_BODY_STUDIO_DEV)
    inline constexpr bool kBodyStudioDev = true;
#else
    inline constexpr bool kBodyStudioDev = false;
#endif

#if defined(FR_BODY_STUDIO) || defined(FR_BODY_STUDIO_DEV)
    inline constexpr bool kBodyStudio = true;
#else
    inline constexpr bool kBodyStudio = false;
#endif

    static_assert(!kBodyStudioDev || kBodyStudio,
                  "the isolated channel must carry the feature it exists to test");

    inline constexpr std::string_view kBuildId{ FR_BUILD_ID };
    inline constexpr std::string_view kDataSubdir{ FR_DATA_SUBDIR };

    [[nodiscard]] inline std::filesystem::path DataRoot() {
        return std::filesystem::path{ "Data/SKSE/Plugins" } / kDataSubdir;
    }

    [[nodiscard]] inline std::filesystem::path DataPath(std::string_view a_relative) {
        return DataRoot() / std::filesystem::path{ a_relative };
    }

    [[nodiscard]] inline std::filesystem::path IniPath() {
        if constexpr (kBodyStudioDev) {
            return DataPath("FittingRoom.ini");
        }
        // The release INI predates the data directory and deliberately stays
        // beside it. Keeping this branch explicit is what makes the default
        // build path and existing installs behavior-neutral.
        return "Data/SKSE/Plugins/FittingRoom.ini";
    }

    [[nodiscard]] inline std::filesystem::path CustomBodyPresetRoot() {
        return DataPath("BodyPresets");
    }

    [[nodiscard]] inline std::filesystem::path BodySlideExportPath() {
        if constexpr (kBodyStudioDev) {
            return std::filesystem::path{ "Data/CalienteTools/BodySlide/SliderPresets" } /
                   ("FittingRoom.BodyStudioDev." + std::string{ kBuildId } + ".xml");
        }
        return "Data/CalienteTools/BodySlide/SliderPresets/FittingRoom.xml";
    }

    [[nodiscard]] inline std::string LogName() {
        if constexpr (kBodyStudioDev) {
            return "FittingRoom.BodyStudioDev." + std::string{ kBuildId } + ".log";
        }
        return "FittingRoom.log";
    }

    [[nodiscard]] inline std::string PreviousLogName() {
        if constexpr (kBodyStudioDev) {
            return "FittingRoom.BodyStudioDev." + std::string{ kBuildId } + ".prev.log";
        }
        return "FittingRoom.prev.log";
    }

    [[nodiscard]] inline std::string Label() {
        if constexpr (kBodyStudioDev) {
            return "Fitting Room - Body Studio Dev " + std::string{ kBuildId };
        }
        return "Fitting Room";
    }

    // SKSE permits one serialization owner per loaded plugin. The development
    // channel gets a stable identity distinct from release's historical OSLT,
    // so it cannot read or overwrite production co-save records. Dev builds
    // share FBSD intentionally: only one virtual FittingRoom.dll may win a
    // launch, and sharing the channel permits upgrade testing across builds.
    inline constexpr std::uint32_t kSerializationUniqueId =
        kBodyStudioDev ? static_cast<std::uint32_t>('FBSD')
                       : static_cast<std::uint32_t>('OSLT');

}  // namespace OS::BuildChannel
