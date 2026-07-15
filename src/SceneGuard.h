#pragma once
#include "PCH.h"

// Scene-framework coexistence. While another mod (OStim and the like) runs an
// animated scene, SUSPEND the transmog render override so the player shows
// their real / undressed gear instead of the styled outfit, and refuse to open
// the editor. Scenes are recognized by Papyrus mod-event NAMES (SendModEvent),
// which makes this mod-agnostic and configurable: any framework announces its
// scene boundaries by name, and the recognized names live in the INI
// ([Scene] sSuspendEvents / sResumeEvents; defaults cover OStim NG plus a
// stable OutfitSlots-specific pair). A C++ plugin integrates the same way - by
// sending a configured mod event - so this IS the compatibility API; no
// versioned interface is needed.
namespace OS::SceneGuard {

    // True while a scene has asked us to stand down. Read on the render thread
    // (OutfitSession::EffectiveLocked) and the input thread - a lock-free atomic.
    [[nodiscard]] bool Active() noexcept;

    // Register the ModCallbackEvent sink. Call once at kDataLoaded, AFTER
    // Settings::Load (it reads the event-name configuration).
    void Init();

}  // namespace OS::SceneGuard
