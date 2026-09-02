#pragma once

#include "ProfileCodec.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // The profile library: one JSON file per named look under
    // Data/SKSE/Plugins/FittingRoom/Profiles (through BuildChannel::DataPath,
    // so dev and release channels split exactly as BodyPresets do). Files by
    // design, like presets - no co-save record; the co-save keeps owning
    // per-save state.
    //
    // Filesystem only. The store never dispatches CharGen and never touches a
    // jslot: Delete hands the captured jslot's name back to the caller, and
    // the runtime half that deletes it stays behind the phase 0 gates.
    //
    // Profiles are keyed by NAME, case-insensitively - decision 4 made them a
    // library of looks, so the name IS the identity and a rename is a new
    // file. The filename goes through the preset sanitizer plus a uniquifier
    // loop on collision; the export path's silent trunc overwrite is the
    // recorded gap this store does not repeat.
    class ProfileStore {
    public:
        static ProfileStore& GetSingleton();

        explicit ProfileStore(std::filesystem::path a_root);

        // One loaded file: the profile, where it lives, whether a rewrite is
        // allowed, and the block drops its parse produced (the caller logs
        // them once; the store stays silent like BodyPresetStore).
        struct Entry {
            ProfileCodec::Profile    profile;
            std::filesystem::path    file;
            bool                     rewritable{ true };
            std::vector<std::string> dropped;
        };

        void Load();
        [[nodiscard]] std::vector<Entry> Snapshot() const;
        [[nodiscard]] std::optional<Entry> Find(std::string_view a_name) const;
        [[nodiscard]] std::size_t RejectedCount() const;
        [[nodiscard]] bool NameAvailable(std::string_view a_name,
                                         std::string_view a_exceptName = {}) const;

        // Save writes a new file for a new name and atomically replaces the
        // existing file for a known one. ⚠ A file whose known blocks all
        // failed to parse is NEVER rewritten: Save refuses and names it, so a
        // broken-but-newer file cannot be flattened into what this build
        // could read of it.
        [[nodiscard]] bool Save(const ProfileCodec::Profile& a_profile,
                                std::string& a_error);

        // A rename rewrites the file under the new name and removes the old
        // one. The captured jslot keeps whatever name it was saved under; the
        // face block's jslot field is the pointer, not the profile name.
        [[nodiscard]] bool Rename(std::string_view a_name,
                                  std::string_view a_newName,
                                  std::string& a_error);

        // Deleting a profile whose face block says "captured" also owes
        // CharGen.DeleteCharacter on its FR_ jslot; a_capturedJslot receives
        // that name (empty when the face is absent or referenced) so the
        // runtime caller can settle the debt. A referenced jslot is the
        // player's own and its name is never handed out for deletion.
        [[nodiscard]] bool Delete(std::string_view a_name, std::string& a_error,
                                  std::string* a_capturedJslot = nullptr);

    private:
        [[nodiscard]] std::filesystem::path NewPathFor(
            const std::string& a_name) const;

        std::filesystem::path root_;
        mutable std::mutex    lock_;
        std::vector<Entry>    entries_;
        std::size_t           rejected_{ 0 };
    };

}  // namespace OS
