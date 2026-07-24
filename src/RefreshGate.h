#pragma once

#include <cstdint>

namespace OS::RefreshGate {

    enum class StagedUpdate {
        kNone,
        kBodyOnly,
        kEquipment,
    };

    // A body preset/ORefit edit does not change a biped object. Sending it
    // through AIProcess::UpdateEquipment needlessly rebuilds every worn armor
    // addon before OBody runs, exposing unrelated malformed armor to Skyrim's
    // unsafe skinning path. Keep that path for actual slot changes only.
    [[nodiscard]] constexpr StagedUpdate ClassifyStagedUpdate(
        bool a_bodyDiffers, std::uint32_t a_changedSlots) {
        if (a_changedSlots != 0) {
            return StagedUpdate::kEquipment;
        }
        return a_bodyDiffers ? StagedUpdate::kBodyOnly : StagedUpdate::kNone;
    }

    // Pure state machine behind the player refresh deferral. Observing a block
    // freezes visual rebuilds until the block ends and one real-time second has
    // elapsed. With no observed block, refreshes remain immediate.
    class BlockingCooldown {
    public:
        [[nodiscard]] bool Ready(bool a_blocking, double a_nowSeconds) {
            if (a_blocking) {
                sawBlocking_ = true;
                releasedAt_  = -1.0;
                return false;
            }
            if (!sawBlocking_) {
                return true;
            }
            if (releasedAt_ < 0.0) {
                releasedAt_ = a_nowSeconds;
                return false;
            }
            if (a_nowSeconds - releasedAt_ < 1.0) {
                return false;
            }
            sawBlocking_ = false;
            releasedAt_  = -1.0;
            return true;
        }

    private:
        bool   sawBlocking_{ false };
        double releasedAt_{ -1.0 };
    };

}  // namespace OS::RefreshGate
