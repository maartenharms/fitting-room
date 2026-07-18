#include "PCH.h"

#include "AutoPresets.h"
#include "BipedHooks.h"
#include "Collection.h"
#include "SamCompat.h"
#include "CrashGuard.h"
#include "Diagnostics.h"
#include "EditorWindow.h"
#include "Favorites.h"
#include "ImGuiOverlay.h"
#include "InputListener.h"
#include "LoreModule.h"
#include "MenuButton.h"
#include "Migration.h"
#include "OutfitSession.h"
#include "Persistence.h"
#include "PresetStore.h"
#include "RecentMods.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "SettingsUI.h"
#include "StyleCatalog.h"

namespace {
    constexpr auto kLogName = "FittingRoom.log";
    // Keep in sync with project(... VERSION) in CMakeLists.txt and vcpkg.json; used only for the load log line.
    constexpr auto kVersion = "0.1.2";

    void SetupLog() {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= kLogName;

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                // "Fitting Room" rename: move user data from the old OutfitSlots path
                // before anything reads it (Settings INI, then the library below).
                OS::Migration::RunOnce();
                OS::Settings::GetSingleton().Load();
                OS::OutfitSession::GetSingleton().SetBlocklist(
                    OS::Settings::GetSingleton().slotBlocklist);
                // Global outfit library (persists across saves) - before any
                // save load so legacy co-save migration sees the right state.
                OS::Persistence::LoadLibraryFileAtStartup();
                // Before the catalog: promote any style whose preview crashed
                // last session to the crashers list so Build/RefreshFit flag it.
                OS::CrashGuard::LoadAtStartup();
                // Starred looks (global, load-order independent) - read before
                // the editor can open; the browser filter reads this set.
                OS::Favorites::LoadAtStartup();
                // Last launch's plugin set - Build() diffs against it to flag
                // styles from newly-added mods (OS-26), then re-baselines.
                OS::RecentMods::LoadAtStartup();
                OS::StyleCatalog::GetSingleton().Build();
                OS::PresetStore::GetSingleton().Load();
                OS::Diagnostics::WarnOnConflicts();
                OS::Collection::GetSingleton().Register();
                OS::ImGuiOverlay::GetSingleton().RegisterMenuGuard();
                OS::MenuButton::Register();
                OS::LoreModule::Init();
                // Scene coexistence (OStim etc.) - listens for scene mod-events
                // to suspend transmog; reads its config from Settings::Load above.
                OS::SceneGuard::Init();
                // In-game config panel (FUCK sidebar tool, soft dependency).
                OS::SettingsUI::Register();
                // The editor as a FUCK IWindow (Phase 3). Must follow
                // SettingsUI::Register (which calls FUCK::Connect).
                OS::EditorWindow::Register();
                // Mod-event bridge: OutfitSlots_Open/_Close/_Toggle (the SAM
                // addon and any mod can open the editor via SendModEvent).
                OS::SamCompat::Register();
                OS::InputListener::Register();
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                // After the co-save (if any) has loaded: fold the CURRENT
                // inventory into the collection - covers saves that predate
                // the collection record.
                OS::Collection::GetSingleton().SeedFromPlayerInventory();
                // The save decides the player's race (UBE etc. are custom
                // races) - only now can style fit be evaluated.
                OS::StyleCatalog::GetSingleton().EnsureFitCurrent();
                // Discovered presets depend on fit (which pieces render on this
                // character), so generate only now, after EnsureFitCurrent.
                OS::AutoPresets::GetSingleton().Generate();
                OS::LoreModule::OnPostLoadGame();
                break;
            default:
                break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    spdlog::info("Fitting Room v{} loading...", kVersion);

    // Universal SE + AE build. Every engine address is runtime-resolved
    // (REL::RelocationID); the two mid-function hook offsets are verified on
    // SE 1.5.97 and next-gen AE (1.6.1170), and the install-time 0xE8 byte
    // checks fail safe on any runtime where a site does not match. Gate to
    // next-gen AE (1.6.1130+): the moved worn-mask offset (+0x80) was verified
    // against 1.6.1170, so pre-next-gen AE would need its own re-verify.
    const auto ver       = a_skse->RuntimeVersion();
    const bool supported = (ver == SKSE::RUNTIME_SSE_1_5_97) ||
                           (ver >= REL::Version(1, 6, 1130, 0));
    if (!supported) {
        spdlog::error("Unsupported Skyrim runtime {}. Fitting Room needs SE 1.5.97 or AE 1.6.1130 and later; not loading.",
                      ver.string());
        return false;
    }

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(256);

    // Install the two biped-rebuild hooks. Trampoline must be allocated first.
    OS::BipedHooks::Install();

    // Co-save (de)serialization must be registered before kDataLoaded.
    OS::Persistence::Register();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        spdlog::error("Failed to register SKSE messaging listener; aborting load.");
        return false;
    }

    spdlog::info("Fitting Room loaded.");
    return true;
}
