#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// The disk cache's image writer. WIC deliberately: this project had no image
// codec of its own before this file (verified 2026-08-09, no stb, no
// DirectXTex, no windowscodecs link anywhere), and FUCK already decodes
// through WIC in this same process, so the machinery is warm and proven
// compatible with these threads.
namespace OS::PreviewPng {

    // Encode tightly packed RGBA8 straight to a PNG file. False on any failure,
    // with a half-written target removed rather than left behind.
    //
    // ⚠⚠ NO .tmp AND NO RENAME, AND THAT IS A CRASH FIX RATHER THAN A
    // SIMPLIFICATION. The rename this used to end with crashed inside Mod
    // Organizer's MoveFileExW hook in the field (2026-08-16, access violation
    // in usvfs_x64.dll on a sex switch, which rebuilds every card at once). The
    // note in the .cpp carries the stack and what replaces the atomicity.
    [[nodiscard]] bool WriteRgba(const std::filesystem::path& a_dest,
                                 const std::vector<std::uint8_t>& a_rgba,
                                 std::uint32_t a_width, std::uint32_t a_height);

}  // namespace OS::PreviewPng
