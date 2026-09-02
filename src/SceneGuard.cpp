#include "SceneGuard.h"

#include "OutfitSession.h"
#include "Settings.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

namespace OS::SceneGuard {

    namespace {
        std::atomic<bool> g_active{ false };

        // Case-insensitive membership test of a mod-event name against a
        // comma-separated list (whitespace around each entry is trimmed).
        // Scene events are rare (start/end), so re-splitting the short config
        // string per event is cheap and always reflects the latest INI.
        [[nodiscard]] bool NameInList(const char* a_name, const std::string& a_csv) {
            if (!a_name || !*a_name) {
                return false;
            }
            std::size_t i = 0;
            while (i <= a_csv.size()) {
                const std::size_t comma = a_csv.find(',', i);
                const std::size_t stop  = comma == std::string::npos ? a_csv.size() : comma;
                std::size_t b = i;
                std::size_t e = stop;
                while (b < e && std::isspace(static_cast<unsigned char>(a_csv[b]))) {
                    ++b;
                }
                while (e > b && std::isspace(static_cast<unsigned char>(a_csv[e - 1]))) {
                    --e;
                }
                if (e > b && _stricmp(a_csv.substr(b, e - b).c_str(), a_name) == 0) {
                    return true;
                }
                if (comma == std::string::npos) {
                    break;
                }
                i = comma + 1;
            }
            return false;
        }

        void SetActive(bool a_on) {
            bool expected = !a_on;
            if (!g_active.compare_exchange_strong(expected, a_on)) {
                return;  // already in that state - ignore duplicate start/end
            }
            spdlog::info("SceneGuard: transmog {} (scene {}).",
                         a_on ? "SUSPENDED" : "resumed", a_on ? "started" : "ended");
            // Re-render the player so the change shows at once instead of
            // waiting for the framework's next biped rebuild. RequestRefresh
            // marshals to the main thread (safe from this event handler).
            OutfitSession::RequestRefresh();
        }

        struct SceneSink : RE::BSTEventSink<SKSE::ModCallbackEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const SKSE::ModCallbackEvent* a_event,
                RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto& s    = Settings::GetSingleton();
                const char* name = a_event->eventName.c_str();
                // ⚠⚠ EVERY MOD EVENT BY NAME, AND IT IS HERE BECAUSE GUESSING COST
                // A ROUND. SexLab's own scripts carry the literals
                // HookAnimationStarting and HookAnimationEnd, both were configured,
                // and neither fired: the framework appends the registering hook's
                // NAME to them, and whether it also sends a bare global form could
                // not be settled off disk. One scene with this line on settles it
                // for good, and a name we have never seen is the only thing that
                // can. Debug level, so it costs a disabled log call otherwise.
                spdlog::debug("SceneGuard: mod event '{}'.", name ? name : "(null)");
                if (NameInList(name, s.sceneSuspendEvents)) {
                    SetActive(true);
                } else if (NameInList(name, s.sceneResumeEvents)) {
                    SetActive(false);
                }
                // ⚠⚠ NOT A SCENE EVENT, AND IT IS HERE BECAUSE THIS IS THE ONLY
                // MOD-EVENT SINK WE OWN. OBody announces its body rebuild by this
                // name and that rebuild clears our push-up morph key, measured
                // twenty-nine times out of twenty-nine on 2026-08-27. Handing the
                // edge to OutfitSession costs one string compare on an event
                // stream we are already walking, and adding a second sink for one
                // name would be the worse trade. ⚠ NOT configurable: this is
                // OBody's own event name and not a compatibility list.
                if (name && std::strcmp(name, "Obody_ApplyMorph") == 0) {
                    OutfitSession::NoteBodyRebuilt();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        SceneSink g_sink;
    }

    bool Active() noexcept { return g_active.load(std::memory_order_relaxed); }

    void Init() {
        const auto& s = Settings::GetSingleton();
        if (!s.sceneCompat) {
            spdlog::info("SceneGuard: disabled ([Scene] bSceneCompat=0).");
            return;
        }
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            source->AddEventSink(&g_sink);
            spdlog::info("SceneGuard: active - suspend on [{}], resume on [{}].",
                         s.sceneSuspendEvents, s.sceneResumeEvents);
        }
    }

}  // namespace OS::SceneGuard
