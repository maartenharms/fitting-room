#include "PCH.h"

#include "ProfileProbe.h"

#include <optional>

#include "ProfileApply.h"
#include "ProfileCapture.h"
#include "ProfilePlan.h"
#include "ProfileStore.h"

namespace OS::ProfileProbe {

    namespace {
        constexpr const char* kLookName = "F11 Look";
        // Unset until the session's first press, which reads the store: a
        // look already on disk means this launch is the APPLY half of the
        // cross-save acceptance (capture on one save, quit, apply on
        // another), and capturing first would overwrite the thing under
        // test. Within a session the presses alternate as before.
        std::optional<bool> g_applyNext;
    }  // namespace

    void OnKey() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) {
            spdlog::warn("ProfileProbe: no loaded player, press ignored.");
            return;
        }
        if (!g_applyNext.has_value()) {
            auto& store = ProfileStore::GetSingleton();
            store.Load();
            g_applyNext = store.Find(kLookName).has_value();
            if (*g_applyNext) {
                spdlog::info("ProfileProbe: '{}' is already on disk, so the "
                             "first press this session applies it.", kLookName);
            }
        }
        if (!*g_applyNext) {
            spdlog::info("ProfileProbe: capturing '{}'. Press again to apply "
                         "it back.", kLookName);
            auto& store = ProfileStore::GetSingleton();
            store.Load();
            ProfileCapture::Capture(player, kLookName, true);
            g_applyNext = true;
            return;
        }
        auto& store = ProfileStore::GetSingleton();
        store.Load();
        const auto entry = store.Find(kLookName);
        if (!entry) {
            // The capture's face half may still be settling; the profile file
            // lands with it. Staying on apply keeps the next press meaningful.
            spdlog::info("ProfileProbe: '{}' is not on disk yet (the capture "
                         "may still be settling); press again.", kLookName);
            return;
        }
        for (const auto& line : entry->dropped) {
            spdlog::info("ProfileProbe: {}", line);
        }
        ProfileApply::Apply(player, entry->profile, ProfilePlan::AllOn());
        g_applyNext = false;
    }

}  // namespace OS::ProfileProbe
