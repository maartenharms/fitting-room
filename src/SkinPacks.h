#pragma once

#include "SkinPlan.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The skin packs installed on this load order, read off disk.
//
// A pack is a folder under textures\FittingRoom\skins\ holding replacer files
// under their own names (femalebody_1.dds, femalebody_1_msn.dds and so on).
// The scan does not care what a file IS: SkinPlan matches it to a shape by
// name at apply time, so this is a walk and a fold and nothing else.
//
// ⚠ LOOSE FILES ONLY, the same limit OverlayTextures records for the same
// reason: std::filesystem cannot see into a BSA. Nobody ships a skin pack in
// one for this mod yet, since the folder is this mod's own convention.
namespace OS::SkinPacks {

    struct Snapshot {
        std::vector<SkinPlan::Pack> packs;  // sorted by id, case folded
        std::size_t                 filesSeen{ 0 };
        std::string                 diagnostic;
        std::uint64_t               generation{ 0 };
    };

    // Walk the disk and publish a new snapshot on a detached thread, as
    // OverlayTextures does and for the same reason: the editor draws every
    // frame and a large pack is thousands of files.
    void RequestScan();

    [[nodiscard]] std::shared_ptr<const Snapshot> Get();
    [[nodiscard]] bool                            Scanning();

    // The snapshot, walking the disk FIRST if nothing has published one yet.
    // For callers that cannot survive an empty answer: an apply asked for a
    // pack by name has no way to tell "no such pack" from "nobody has looked
    // yet", and answering the first when it means the second loses the skin
    // the player asked for. Costs one folder walk, once per session at most.
    [[nodiscard]] std::shared_ptr<const Snapshot> EnsureScanned();

    // The pack with this id in a snapshot, or null.
    [[nodiscard]] const SkinPlan::Pack* Find(const Snapshot& a_snap, std::string_view a_id);

    // The pure half, exposed for the scan and for a test to drive with a
    // temporary tree rather than the live install.
    [[nodiscard]] Snapshot ScanRoot(const std::filesystem::path& a_texturesRoot);

}  // namespace OS::SkinPacks
