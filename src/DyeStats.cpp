#include "PCH.h"

#include "DyeStats.h"

#include "VmCall.h"  // a static call that refuses instead of dereferencing null

#include <mutex>
#include <utility>

namespace OS::DyeStats {

    namespace {

        std::mutex                                     g_lock;
        std::unordered_map<std::string, std::uint32_t> g_values;
        // ⚠ THE GENERATION IS THE WHOLE DEFENCE AGAINST CROSS-CHARACTER GRANTS.
        // Answers arrive ~180 ms after the ask and a load screen is longer than
        // one dispatch, so save A's counters can very easily still be in flight
        // when save B starts asking. Landing A's "Dungeons Cleared 84" in B's
        // map would unlock colours on B permanently, off a playthrough that is
        // not theirs, and add-only means nothing takes it back.
        //
        // A generation stamped on each callback and compared on arrival is the
        // cheapest thing that closes it. Not a cancel: there is no way to
        // cancel a dispatched call, so the answer is discarded on arrival
        // instead.
        std::uint64_t g_generation = 0;
        std::size_t   g_outstanding = 0;
        // Held rather than called inline, because it must run once the LAST
        // answer lands and any answer might be the last.
        std::function<void()> g_onSettled;

        // Declared before its one caller, which is the callback below; defined
        // after, beside the state it reads.
        void LogValues();

        // One dispatched Game.QueryStat, and where its answer goes.
        //
        // ⚠ REF COUNTED BY THE ENGINE, so it is allocated with new and handed
        // to a BSTSmartPointer, never stack-allocated and never deleted here.
        class StatCallback : public RE::BSScript::IStackCallbackFunctor {
        public:
            StatCallback(std::string a_name, std::uint64_t a_generation) :
                name_(std::move(a_name)), generation_(a_generation) {}

            void operator()(RE::BSScript::Variable a_result) override {
                // ⚠ THIS RUNS ON THE VM'S THREAD, with the dispatching call
                // long off the stack. That is what makes taking g_lock here
                // safe, and it is also why nothing in this function may touch
                // DyeUnlocks: its mutex is plain and non-reentrant, and the
                // hang that taught this project that lesson had no crash and no
                // log line to find it by.
                std::function<void()> settled;
                {
                    std::scoped_lock lock(g_lock);
                    if (generation_ != g_generation) {
                        // Somebody else's character. Discarded unread; see the
                        // generation comment above.
                        return;
                    }
                    if (a_result.IsInt()) {
                        const auto v = a_result.GetSInt();
                        // ⚠ NEGATIVE CLAMPS TO 0 RATHER THAN WRAPPING. The
                        // thresholds are unsigned and a wrapped -1 is
                        // 4294967295, which satisfies every gate ever written
                        // and grants the lot, permanently. No stat should ever
                        // be negative; if one is, withholding is the answer
                        // that can be taken back.
                        g_values[name_] = v > 0 ? static_cast<std::uint32_t>(v) : 0u;
                    } else {
                        // Not an int. Game.QueryStat returns one for every
                        // counter, so this is a name that resolved to something
                        // else entirely. Absent reads 0, which withholds.
                        spdlog::warn("Dye stats: '{}' answered a non-integer, so "
                                     "it counts as 0 and its colours stay locked.",
                                     name_);
                    }
                    if (g_outstanding > 0 && --g_outstanding == 0) {
                        settled = std::move(g_onSettled);
                        g_onSettled = nullptr;
                        LogValues();
                    }
                }
                if (!settled) {
                    return;
                }
                // ⚠ MARSHALLED TO THE MAIN THREAD, NEVER RUN HERE. The settle
                // callback re-runs promotion, which reads the player, walks the
                // palette and writes the co-save record. None of that is the VM
                // thread's business.
                //
                // ⚠ ONE AddTask, AND IT DOES NOT RE-QUEUE ITSELF. The SKSE task
                // queue drains in the same pass it is added from, so a task
                // that adds another task from inside itself is a hard freeze.
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([settled = std::move(settled)]() { settled(); });
                } else {
                    spdlog::warn("Dye stats: every value arrived but there is no "
                                 "task interface to run the promotion pass on, "
                                 "so the colours they gate wait for the next "
                                 "load.");
                }
            }

            bool CanSave() const override { return false; }

            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            // ⚠ AN OWNED STRING, not a view into the rule set. The rules are a
            // snapshot the caller may drop the moment Request returns, and this
            // object outlives that by however long the VM takes to answer.
            std::string   name_;
            std::uint64_t generation_{ 0 };
        };

        // What LogValues printed last time, so the tick logs movement rather
        // than the world. Cleared with g_values in Forget(): a new character
        // gets a full dump and never diffs against the old one's numbers.
        std::unordered_map<std::string, std::uint32_t> g_logged;

        // Called with g_lock HELD, from the arm that saw the last answer land.
        //
        // The full one-line-per-stat dump was built for its field round and
        // the round happened (2026-08-22: 30 of 30 asked counters answered
        // with live values). The first pass after a load still dumps them
        // all, so that check stays one log read away; the 30-second ticks
        // after it print only counters that MOVED, because thirty unchanged
        // lines per tick were most of the field log's bulk.
        void LogValues() {
            const bool  first = g_logged.empty();
            std::size_t moved = 0;
            for (const auto& [name, value] : g_values) {
                const auto it = g_logged.find(name);
                if (first || it == g_logged.end() || it->second != value) {
                    spdlog::info("Dye stats: '{}' = {}", name, value);
                    ++moved;
                }
            }
            g_logged = g_values;
            if (!first && moved == 0) {
                spdlog::debug("Dye stats: {} counter(s), none moved.",
                              g_values.size());
            }
        }

    }  // namespace

    void Request(const std::set<std::string, std::less<>>& a_names,
                 std::function<void()>                     a_onSettled) {
        // ⚠⚠ THE FORGET COMES FIRST, BEFORE THE EMPTY TEST AND BEFORE THE VM
        // LOOKUP, AND BOTH ORDERINGS WERE WRONG THE FIRST TIME. Either early
        // return used to leave the OUTGOING character's counters sitting in the
        // map. kPreLoadGame clears them on the load path, but a new game
        // started from the main menu after a save was loaded fires kNewGame and
        // no kPreLoadGame at all, so that path had nothing clearing it: the new
        // character would have been promoted against the old one's stats, and a
        // counter that is too high grants a colour permanently.
        //
        // Forgetting unconditionally makes the failure "nothing is known",
        // which withholds, instead of "somebody else's numbers", which grants.
        Forget();

        if (a_names.empty()) {
            return;
        }

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            spdlog::warn("Dye stats: no Papyrus VM, so {} stat(s) cannot be "
                         "read and every colour gating on one stays locked "
                         "this session.",
                         a_names.size());
            return;
        }

        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(g_lock);
            generation    = ++g_generation;
            g_outstanding = a_names.size();
            g_onSettled   = std::move(a_onSettled);
        }

        std::size_t refused = 0;
        for (const auto& name : a_names) {
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new StatCallback(name, generation)
            };
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(name));
            if (!VmCall::Static(vm, "Game", "QueryStat", args, cb)) {
                ++refused;
            }
        }

        if (refused == 0) {
            spdlog::info("Dye stats: asked for {} counter(s); answers land on "
                         "the VM thread and promotion runs again when they do.",
                         a_names.size());
            return;
        }

        // ⚠ A REFUSED DISPATCH NEVER CALLS BACK, so its share of the
        // outstanding count has to come off here or the settle callback waits
        // for an answer that is not coming and the second promotion pass never
        // runs. Handled after the loop rather than inside it so the count is
        // only ever adjusted once, under one lock.
        std::function<void()> settled;
        {
            std::scoped_lock lock(g_lock);
            if (generation != g_generation) {
                return;              // superseded while we were dispatching
            }
            g_outstanding = refused > g_outstanding ? 0 : g_outstanding - refused;
            if (g_outstanding == 0) {
                settled     = std::move(g_onSettled);
                g_onSettled = nullptr;
            }
        }
        spdlog::warn("Dye stats: the VM refused {} of {} dispatch(es). Those "
                     "counters read 0 this session, so the colours they gate "
                     "stay locked.",
                     refused, a_names.size());
        if (settled) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([settled = std::move(settled)]() { settled(); });
            }
        }
    }

    std::unordered_map<std::string, std::uint32_t> Values() {
        std::scoped_lock lock(g_lock);
        return g_values;
    }

    void Forget() {
        std::scoped_lock lock(g_lock);
        g_values.clear();
        g_logged.clear();
        // ⚠ THE GENERATION MOVES TOO. Clearing the map alone would let an
        // answer still in flight refill it a moment later with the character we
        // just forgot.
        ++g_generation;
        g_outstanding = 0;
        g_onSettled   = nullptr;
    }

}  // namespace OS::DyeStats
