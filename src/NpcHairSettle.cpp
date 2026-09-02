#include "NpcHairSettle.h"

#include "FuckCompat.h"
#include "NpcHair.h"

namespace OS::NpcHairSettle {

    namespace {

        // Invisible on purpose: 1x1, no background, no decoration, input
        // passed through. Draw submits no widgets; the window exists so that
        // FUCK's gameplay frame calls into NpcHair while ladders are pending
        // and skips this object entirely while none are.
        class SettleTicker final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "NpcHairSettleTicker"; }
            const char* Title() const override { return "Fitting Room Hair Settle"; }

            // ⚠ A DATA QUERY, NEVER USER STATE, the card window's contract.
            // Lock-free: FUCK asks every frame for the life of the session.
            bool IsOpen() const override { return NpcHair::SettleChecksPending(); }
            void SetOpen(bool) override {}

            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // No kCloseOnGameMenu: a menu that pauses the world pauses
                // the un-hider too, so a ladder firing under one is a no-op
                // walk, and a ladder STALLED by one would just fire late.
                return static_cast<F>(static_cast<unsigned>(F::kNoDecoration) |
                                      static_cast<unsigned>(F::kNoBackground) |
                                      static_cast<unsigned>(F::kNoMove) |
                                      static_cast<unsigned>(F::kNoResize) |
                                      static_cast<unsigned>(F::kPassInputToGame) |
                                      static_cast<unsigned>(F::kRenderDuringTM));
            }

            ImVec2 GetDefaultSize() const override { return ImVec2(1.0f, 1.0f); }
            ImVec2 GetDefaultPos() const override { return ImVec2(0.0f, 0.0f); }

            void Draw() override { NpcHair::TickSettleChecks(); }
        };

        SettleTicker g_window;

    }  // namespace

    void Register() {
        // FUCK::RegisterWindow no-ops when the interface is absent, and the
        // degradation is stated where the ladders arm: they never drain, and
        // the mod behaves exactly as it did before the settle fix existed.
        FUCK::RegisterWindow(&g_window);
        spdlog::info("NpcHairSettle: ticker registered, so a styled follower's "
                     "settle checks run with the editor shut.");
    }

}  // namespace OS::NpcHairSettle
