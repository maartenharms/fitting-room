#include "CbpcRefresh.h"

#include "CbpcArmorClass.h"  // Reassert: the ladder's rebuild may wipe the push
#include "VmCall.h"  // a static call that refuses instead of dereferencing null

#include <chrono>
#include <thread>

namespace OS::CbpcRefresh {

    namespace {

        // Answer sink for the fire-and-forget dispatch; the VM requires a
        // functor and the answer is worth one debug line.
        class RefreshAnswer final : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit RefreshAnswer(const char* a_native) : native(a_native) {}

            void operator()(RE::BSScript::Variable) override {
                spdlog::debug("CbpcRefresh: {} answered.", native);
            }
            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            const char* native;
        };

        void DispatchGlobal(const char* a_native) {
            auto* const vm =
                RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                return;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new RefreshAnswer(a_native)
            };
            auto* const args = RE::MakeFunctionArguments();
            const bool  ok =
                VmCall::Static(vm, "CBPCPluginScript", a_native, args, cb);
            spdlog::info("CbpcRefresh: CBPCPluginScript.{} {}.", a_native,
                         ok ? "dispatched" : "REFUSED");
        }

        void DispatchBoth() {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            auto* const vm =
                RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!player || !vm) {
                return;
            }
            for (const char* native : { "RefreshActorBounceSettings",
                                        "RefreshActorCollisionSettings" }) {
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                    new RefreshAnswer(native)
                };
                auto* const args =
                    RE::MakeFunctionArguments(static_cast<RE::Actor*>(player));
                const bool ok =
                    VmCall::Static(vm, "CBPCPluginScript", native, args, cb);
                spdlog::info(
                    "CbpcRefresh: CBPCPluginScript.{} {} - CBPC's per-actor "
                    "bone list is built once and a sex flip alone never "
                    "rebuilds it (r50, its own log: 'Prisoner - Female' "
                    "updating the male kit).",
                    native, ok ? "dispatched for the player" :
                                 "REFUSED (CBPC absent or too old); the body "
                                 "stays still until a save load");
            }
        }

        // r54: the stop/start cycle revived the FLAT entries (NPC L/R Butt
        // bounced) and left the chained ones still - L Breast01-03, the only
        // bones the UBE body skins its breast vertices to (NPC L/R Breast is
        // not even in the skin's bone list, which is why r52 read it
        // "computing" while the screen never moved). cbp.dll sinks
        // OnNiNodeUpdate, and RaceMenu fires this very event at menu close -
        // the one path that ever birthed the chains. Send it ourselves.
        void SendNiNodeUpdate() {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            auto* const source = SKSE::GetNiNodeUpdateEventSource();
            if (!player || !source) {
                return;
            }
            SKSE::NiNodeUpdateEvent evt{ player };
            source->SendEvent(&evt);
            spdlog::info(
                "CbpcRefresh: NiNodeUpdate sent for the player - the per-actor "
                "rescan CBPC sinks (the RaceMenu-close path fires the same "
                "event; the stop/start cycle alone left the breast chain "
                "unbuilt in r54).");
        }

    }  // namespace

    void QueueAfterSwitch() {
        // One detached watcher posting SINGLE tasks; never a task re-queueing
        // a task. 2.5 s clears the apply's head-build storm and the settle
        // refresh, so CBPC rebuilds against the settled skeleton.
        //
        // ⚠ The StopPhysics/StartPhysics cycle is GONE, and not because it
        // misbehaved: CBPCPluginScript.psc declares both as
        // (Actor, String nodeName) and the old dispatch passed no arguments,
        // so the pair never ran at all. Which retro-dates r51's "the Refresh
        // pair cures nothing" to the wig-holder era and leaves the Refresh
        // pair as the actual list rebuilder. The NiNodeUpdate stays, ONCE,
        // at the apply settle only - firing it during an equip 3D storm is
        // what froze r57.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            if (auto* const tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { DispatchBoth(); });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (auto* const tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { SendNiNodeUpdate(); });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (auto* const tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { CbpcArmorClass::Reassert(); });
            }
        }).detach();
    }

}  // namespace OS::CbpcRefresh
