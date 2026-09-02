#include "CbpcArmorClass.h"

#include "Outfit.h"
#include "Settings.h"  // cbpcBouncePercent: the want is a setting now, not a literal
#include "OutfitSession.h"
#include "StyleRef.h"
#include "VmCall.h"  // a static call that refuses instead of dereferencing null

#include <atomic>

namespace OS::CbpcArmorClass {

    namespace {

        inline constexpr std::uint32_t kChestBit = 2;  // biped slot 32
        inline constexpr const char*   kProfile  = "FittingRoom";

        // -1 = never pushed; CBPC starts every actor un-interpolated, so the
        // first evaluation only dispatches if it wants something above zero.
        int              g_lastPercent{ -1 };
        std::atomic_bool g_assertQueued{ false };
        std::atomic<Support> g_support{ Support::kUnknown };

        // What a shown chest piece is worth right now. ⚠ CLAMPED AGAIN HERE rather
        // than trusted from Settings: this is the last frame before the number
        // reaches another mod's script, and the settings panel is not the only
        // writer of that field.
        [[nodiscard]] int WantedPercent() {
            const int v = Settings::GetSingleton().cbpcBouncePercent;
            return v < 0 ? 0 : (v > 100 ? 100 : v);
        }

        // Does the active outfit SHOW something on the chest? A styled piece
        // or a hide both mean the worn armor is not what the player sees;
        // passthrough and no-outfit mean it is.
        bool ShownChestOwnsTheRead(RE::Actor* a_player) {
            const auto outfit =
                OutfitSession::GetSingleton().ActiveOutfitFor(a_player);
            if (!outfit) {
                return false;
            }
            const auto& entry = outfit->EntryFor(kChestBit);
            switch (entry.kind) {
            case SlotEntry::Kind::kHide:
                return true;
            case SlotEntry::Kind::kStyle:
                // A style that no longer resolves renders nothing new, so the
                // worn piece keeps the read.
                return StyleRef::Resolve(entry.style) != nullptr;
            default:
                return false;
            }
        }

        class InterpolationAnswer final
            : public RE::BSScript::IStackCallbackFunctor {
        public:
            void operator()(RE::BSScript::Variable) override {
                spdlog::debug("CbpcArmorClass: ApplyBounceInterpolation answered.");
            }
            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
        };

        void DispatchInterpolation(RE::Actor* a_actor, int a_percentage) {
            auto* const vm =
                RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                return;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new InterpolationAnswer
            };
            auto* const args = RE::MakeFunctionArguments(
                static_cast<RE::Actor*>(a_actor), RE::BSFixedString(kProfile),
                static_cast<std::int32_t>(a_percentage));
            // ⚠⚠ NEVER vm->DispatchStaticCall DIRECTLY HERE. This exact line
            // crashed every player who had a cbpcPluginScript.pex without the
            // CBPC build that carries ApplyBounceInterpolation, on every
            // slot 32 outfit and on nothing else, in 1.1.2 and 1.1.3. The
            // reason the engine cannot be trusted with the question is at
            // VmCall.h.
            const bool ok = VmCall::Static(
                vm, "CBPCPluginScript", "ApplyBounceInterpolation", args, cb);
            spdlog::info(
                "CbpcArmorClass: ApplyBounceInterpolation('{}', {}) {} - the "
                "shown chest {} the bounce read.",
                kProfile, a_percentage,
                ok ? "dispatched" : "REFUSED (CBPC absent or too old)",
                a_percentage > 0 ? "owns" : "hands back");
            // ⚠ THE REFUSAL IS THE ONLY HONEST SIGNAL WE GET. CBPC exposes no
            // version query, so "the VM had no such script" is what tells the
            // settings panel to grey the slider and say why.
            g_support.store(ok ? Support::kPresent : Support::kAbsent,
                            std::memory_order_relaxed);
        }

        struct EquipSink final : RE::BSTEventSink<RE::TESEquipEvent> {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* a_event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override {
                auto* const player = RE::PlayerCharacter::GetSingleton();
                if (!a_event || !player || a_event->actor.get() != player) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (g_assertQueued.exchange(true, std::memory_order_acq_rel)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (auto* const tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] {
                        g_assertQueued.store(false, std::memory_order_release);
                        AssertPlayer();
                    });
                } else {
                    g_assertQueued.store(false, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        EquipSink g_equipSink;

    }  // namespace

    Support SupportState() { return g_support.load(std::memory_order_relaxed); }

    void Install() {
        if (auto* const holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
        }
        spdlog::info(
            "CbpcArmorClass: equip sink registered - while an outfit shows a "
            "chest piece, ApplyBounceInterpolation('{}') overrides the worn "
            "armor's amplitude class per-actor.",
            kProfile);
    }

    void AssertPlayer() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const int want = ShownChestOwnsTheRead(player) ? WantedPercent() : 0;
        if (want == g_lastPercent) {
            return;
        }
        // 0 is also skipped when nothing was ever pushed: CBPC starts the
        // actor un-interpolated and a 0-push would only spend a VM call.
        if (want == 0 && g_lastPercent == -1) {
            g_lastPercent = 0;
            return;
        }
        g_lastPercent = want;
        DispatchInterpolation(player, want);
    }

    void Reassert() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const int want = ShownChestOwnsTheRead(player) ? WantedPercent() : 0;
        g_lastPercent = want;
        if (want > 0) {
            DispatchInterpolation(player, want);
        }
    }

}  // namespace OS::CbpcArmorClass
