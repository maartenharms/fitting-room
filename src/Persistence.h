#pragma once

namespace OS::Persistence {
    // Call once from SKSEPluginLoad, before kDataLoaded.
    void Register();

    // Collections model: each SAVE owns its outfit library (written into the co-save
    // 'LIBR' record), so edits/deletes made on another save can't make outfits vanish.
    // Data/SKSE/Plugins/FittingRoom/outfits.json is the shared/global library - the
    // default a fresh game starts from and the fallback for pre-collections saves that
    // have no 'LIBR' record yet (they gain one the next time they are saved).

    // Load the global library into the session. Call at kDataLoaded, before
    // any save loads.
    void LoadLibraryFileAtStartup();

    // Debounced, main-thread-marshaled save of the global library. Safe to
    // call from any thread; OutfitSession invokes it after every mutation.
    void QueueLibrarySave();
}
