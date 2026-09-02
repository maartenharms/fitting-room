#pragma once

#include "PreviewManifest.h"

#include <cstdint>
#include <filesystem>
#include <string>

// The thumbnail store on disk: PNG files under a sharded root plus one JSON
// manifest. Everything here runs on the render thread only, from the preview
// cache's drain, so no lock; a comment at each entry point says so.
namespace OS::PreviewDiskCache {

    struct Hit {
        PreviewManifest::Status status{ PreviewManifest::Status::kMissing };
        std::filesystem::path   file;           // absolute, set when kReady
        std::string             failureReason;  // set when kFailed
    };

    // What the store knows about a disk key. A kStale entry reports kMissing
    // so the card re-renders instead of reading as failed, and a hash whose
    // stored key differs is a detected collision, reported missing and
    // logged once.
    [[nodiscard]] Hit Lookup(const std::string& a_diskKey);

    // The absolute path the PNG for this key must be written to.
    [[nodiscard]] std::filesystem::path PathFor(const std::string& a_diskKey);

    void MarkReady(const std::string& a_diskKey, std::uint32_t a_w, std::uint32_t a_h);
    void MarkFailed(const std::string& a_diskKey, const std::string& a_reason);

    // The manifest write is BATCHED: mutations set a dirty flag and this
    // flushes it. The drain calls it when its queue goes idle and the editor
    // close calls it too; a per-thumbnail rewrite of a thousand-entry JSON
    // would be a guaranteed hitch on first browse.
    void FlushIfDirty();

    // The settings button: wipe every image and the manifest, start empty.
    // The escape hatch persisted failures need, since a mesh fixed on disk
    // stays blocked until its failure entry goes.
    void RebuildAll();

}  // namespace OS::PreviewDiskCache
