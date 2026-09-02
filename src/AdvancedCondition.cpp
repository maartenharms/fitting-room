#include "AdvancedCondition.h"

#include "AdvancedConditionParse.h"

#include <unordered_map>

// ============================================================================
// Task 8b - the real Advanced-condition parser. This is NOT the Task 8a stub
// anymore (see git history for that: it refused every clause outright so
// Task 11's WorldWatch/snapshot build and the Rules tab could compile and be
// field-tested against the ten typed condition kinds without this, the only
// part of the feature that can crash the game rather than merely misbehave,
// in the mix).
//
// Clause text -> RE::TESCondition via SCRIPT_FUNCTION::LocateScriptCommand
// and a hand-built one-item CTDA chain. ADVANCED CONDITIONS ARE
// PARAMETERLESS ONLY: `entry->numParams != 0` refuses the clause outright,
// and nothing is ever written into FUNCTION_DATA::params[0]/[1] - they are
// left null. This is a scope reduction from an earlier version of this file
// that DID accept parameters, kept here as the record of why not:
//
// An earlier revision resolved a form-shaped token ("Plugin.esp|0xID") via
// TESDataHandler and passed the resulting TESForm* straight into params[],
// with arity checked against SCRIPT_FUNCTION::numParams and per-parameter
// shape cross-checked against SCRIPT_PARAMETER::paramType (numeric vs.
// form-shaped). Review found the gap that check does not close: ANY
// resolvable form satisfies "this token is form-shaped", regardless of its
// concrete FormType. A function wanting a Faction* hand-fed a weapon's form
// ID gets a valid, non-null, semantically wrong TESForm* - Bethesda's
// condition-function bodies cast without checking, so that is type
// confusion, potentially a vtable call through a mismatched object. The
// unsafe input needs no special knowledge to construct: any
// Plugin.esp|0xID naming a real form of the wrong kind, which is easy to
// hit by guessing a plausible low hex ID in a large plugin. Closing that
// gap properly needs a per-function SCRIPT_PARAM_TYPE -> FormType table -
// confident knowledge of dozens of parameter types across a 2011 engine, a
// bar this project could not clear with confidence (the six-entry numeric
// allowlist that revision used was already at the edge of what could be
// asserted, and some entries were not independently verifiable). Refusing
// every parameter instead eliminates the params[] hazard class outright:
// with nothing ever written there, the blind-deref surface this whole task
// was deferred for does not exist for this file to get wrong. The remaining
// parameterless surface - IsSneaking, IsInCombat, IsSwimming, IsInInterior,
// IsSprinting, IsWeaponOut and friends - is still genuinely useful, and the
// ten typed condition kinds already cover location, weather, time of day,
// combat and worn slots. This is reversible: parameters can come back later
// behind a real type table, with the crash surface understood, rather than
// shipped now on inference. See AdvancedConditionParse.h for the pure
// tokenizer, which still recognises a form-shaped token - only to name it in
// a clear refusal message, never to accept it.
//
// Two hazards remain and are both still handled:
//  1. Arity is checked against the REAL SCRIPT_FUNCTION::numParams; any
//     function that is not parameterless is refused before anything is
//     built, so params[] is never written.
//  2. The FunctionID-equals-table-index assumption is sanity-checked once,
//     at first use, against a known entry (GetDistance must land at index
//     1); if it does not hold, Advanced is disabled for the rest of the
//     session rather than build conditions against a wrong mapping.
//  3. TESCondition/TESConditionItem both carry TES_HEAP_REDEFINE_NEW() - both
//     are allocated with plain `new` (routes to the game allocator) and
//     freed through a plain `delete` in the shared_ptr's deleter, never
//     std::make_shared (which would construct the object inside a
//     std::allocator-owned block instead of the game heap).
//
// Tokenizing and shape-checking the clause text - splitting on whitespace,
// matching the operator, recognising "Plugin.esp|0xID" well enough to name
// it in a refusal message - is pure logic with no RE:: dependency, so it
// lives apart in AdvancedConditionParse.h and has offline test coverage
// (tests/test_advancedconditionparse.cpp). This file only does the parts
// that genuinely need the engine: resolving the function table entry,
// building the TESCondition, and evaluating it. It does NOT go into any test
// target - it names RE:: types throughout.
// ============================================================================

namespace OS::AdvancedCondition {

    namespace {

        // ---- hazard 2: sanity-check the table-index assumption once --------

        bool g_sanityChecked = false;
        bool g_disabled      = false;

        constexpr const char* kDisabledMessage =
            "Advanced conditions are disabled: this game build's condition-function table did "
            "not match what this parser expects (see log).";

        // "On first use", per the plan - not at DLL load, not per save load
        // (ClearCache() deliberately does not reset this: the game binary's
        // function table layout cannot change mid-process, so re-checking it
        // on every save load would just repeat the same answer forever).
        bool AdvancedUsable() {
            if (g_sanityChecked) {
                return !g_disabled;
            }
            g_sanityChecked = true;

            auto* base = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
            auto* dist = RE::SCRIPT_FUNCTION::LocateScriptCommand("GetDistance");
            const std::ptrdiff_t index = (base && dist) ? (dist - base) : -1;
            if (index != 1) {
                g_disabled = true;
                spdlog::error(
                    "AdvancedCondition: LocateScriptCommand(\"GetDistance\") landed at table "
                    "index {} instead of the expected 1 - the FunctionID-equals-table-index "
                    "mapping this parser relies on does not hold on this game build. Advanced "
                    "conditions are disabled for the rest of this session rather than build "
                    "conditions against a wrong mapping.",
                    index);
            }
            return !g_disabled;
        }

        // ---- cache -------------------------------------------------------
        //
        // Keyed on (rule id, clause index, clause text) per AdvancedCondition
        // .h. Caches the PARSE OUTCOME, success or failure alike: a clause
        // that will never parse (a typo'd function name, or one that takes
        // parameters) would otherwise re-run LocateScriptCommand every
        // evaluate. Not evicted except by ClearCache() - entries are small
        // and only accumulate as fast as a human edits Advanced text boxes,
        // and ClearCache() already bounds it at every save load/revert.

        struct CacheKey {
            std::string ruleId;
            std::size_t clauseIndex{ 0 };
            std::string text;

            friend bool operator==(const CacheKey&, const CacheKey&) = default;
        };

        struct CacheKeyHash {
            std::size_t operator()(const CacheKey& a_key) const noexcept {
                std::size_t h = std::hash<std::string>{}(a_key.ruleId);
                h             = h * 31 + std::hash<std::size_t>{}(a_key.clauseIndex);
                h             = h * 31 + std::hash<std::string>{}(a_key.text);
                return h;
            }
        };

        struct CacheEntry {
            std::shared_ptr<RE::TESCondition> condition;  // null on parse failure
            std::string                       error;      // set when condition is null
        };

        std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> g_cache;

        // ---- operator mapping ----------------------------------------------

        [[nodiscard]] RE::CONDITION_ITEM_DATA::OpCode ToEngineOpCode(
            AdvancedConditionParse::Op a_op) {
            using Op     = AdvancedConditionParse::Op;
            using OpCode = RE::CONDITION_ITEM_DATA::OpCode;
            switch (a_op) {
            case Op::kEq:
                return OpCode::kEqualTo;
            case Op::kNe:
                return OpCode::kNotEqualTo;
            case Op::kGt:
                return OpCode::kGreaterThan;
            case Op::kGe:
                return OpCode::kGreaterThanOrEqualTo;
            case Op::kLt:
                return OpCode::kLessThan;
            case Op::kLe:
                return OpCode::kLessThanOrEqualTo;
            }
            return OpCode::kEqualTo;  // unreachable - Op has no other value
        }

    }  // namespace

    std::shared_ptr<RE::TESCondition> Parse(const std::string& a_text, std::string& a_error) {
        if (!AdvancedUsable()) {
            a_error = kDisabledMessage;
            return nullptr;
        }

        const auto parsed = AdvancedConditionParse::Parse(a_text, a_error);
        if (!parsed) {
            return nullptr;  // a_error already set by the pure parser
        }

        auto* entry = RE::SCRIPT_FUNCTION::LocateScriptCommand(parsed->functionName);
        if (!entry) {
            a_error = fmt::format("Unknown condition function '{}'", parsed->functionName);
            return nullptr;
        }
        if (!entry->conditionFunction) {
            a_error = fmt::format("'{}' cannot be used as a condition", parsed->functionName);
            return nullptr;
        }
        // Parameterless only - see the file banner for why. This is the
        // hazard-1 check: refuse before anything is built rather than write
        // to FUNCTION_DATA::params at all.
        if (entry->numParams != 0) {
            a_error = fmt::format(
                "'{}' takes parameters, which are not supported. Only parameterless conditions "
                "can be used here.",
                parsed->functionName);
            return nullptr;
        }

        auto* base = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
        if (!base) {
            a_error = kDisabledMessage;
            return nullptr;
        }
        const auto functionId = static_cast<RE::FUNCTION_DATA::FunctionID>(entry - base);

        // hazard 3: plain `new` routes through TES_HEAP_REDEFINE_NEW() to the
        // game allocator for both types below.
        auto* item                        = new RE::TESConditionItem();
        item->next                        = nullptr;  // one clause, not a chain
        item->data.comparisonValue.f      = parsed->value;
        item->data.functionData.function  = functionId;
        item->data.functionData.params[0] = nullptr;  // parameterless only - never written
        item->data.functionData.params[1] = nullptr;  // parameterless only - never written
        item->data.flags.isOR             = false;
        item->data.flags.global           = false;  // comparisonValue.f, never .g
        item->data.flags.opCode           = ToEngineOpCode(parsed->op);
        item->data.object                 = RE::CONDITIONITEMOBJECT::kSelf;

        auto* condition = new RE::TESCondition();
        condition->head = item;

        // Never std::make_shared here: it would construct TESCondition
        // inside a std::allocator-owned block instead of the game heap. The
        // deleter is a plain `delete`, which routes through TESCondition's
        // own operator delete (the game allocator) and, via its destructor,
        // owns freeing the TESConditionItem chain the same way.
        return std::shared_ptr<RE::TESCondition>(condition, [](RE::TESCondition* a_c) { delete a_c; });
    }

    void Resolve(Rules::RuleSet& a_rules, Rules::WorldSnapshot& a_snapshot) {
        // Resolve runs from BuildSnapshot, on the same engine-adjacent path
        // every BSTEventSink in this project wraps in try/catch: nothing may
        // unwind into engine frames. This does not protect against a bad
        // dereference (that is what hazards 1/2 above are for) - only
        // against an exception (bad_alloc, etc.) escaping into the caller.
        try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                // BuildSnapshot already gates on the player existing before
                // it ever reaches this call; defensive only.
                return;
            }

            for (auto& rule : a_rules) {
                // Tracked per RULE, not applied per clause: a rule can carry
                // more than one Advanced clause, and deciding invalid/
                // invalidReason clause-by-clause would let a later clause's
                // success erase an earlier clause's failure in the same
                // pass. Decided once, after every clause in this rule has
                // been checked.
                bool        anyAdvanced = false;
                bool        allParsed   = true;
                std::string firstFailureReason;

                for (std::size_t i = 0; i < rule.conditions.size(); ++i) {
                    if (rule.conditions[i].kind != Rules::ConditionKind::kAdvanced) {
                        continue;
                    }
                    anyAdvanced = true;

                    CacheKey key{ rule.id, i, rule.conditions[i].advancedText };
                    auto     it = g_cache.find(key);
                    if (it == g_cache.end()) {
                        CacheEntry entry;
                        entry.condition = Parse(key.text, entry.error);
                        it              = g_cache.emplace(std::move(key), std::move(entry)).first;
                    }

                    const CacheEntry& cached = it->second;
                    if (!cached.condition) {
                        allParsed = false;
                        if (firstFailureReason.empty()) {
                            firstFailureReason = cached.error;
                        }
                        a_snapshot.advanced[{ rule.id, i }] = false;
                        continue;
                    }

                    RE::TESObjectREFR* actor            = player;
                    a_snapshot.advanced[{ rule.id, i }] = cached.condition->IsTrue(actor, actor);
                }

                if (anyAdvanced) {
                    if (!allParsed) {
                        rule.invalid       = true;
                        rule.invalidReason = firstFailureReason;
                    } else {
                        // A fixed typo must not stay stuck invalid until a
                        // save reload: this is the only place that can tell
                        // a clause now parses, since PersistAdvancedValidity
                        // writes back whatever this function decided every
                        // pass. Scoped to rules that actually carry an
                        // Advanced clause (the same "anyAdvanced" gate
                        // PersistAdvancedValidity itself uses via
                        // hasAdvanced), so this never touches a rule with no
                        // Advanced clause at all. It CAN still clear an
                        // invalid/invalidReason that HandleMissingOutfit set
                        // for an unrelated reason (a missing outfit) on a
                        // rule that also happens to carry an Advanced
                        // clause - narrow, pre-existing overlap between two
                        // independent invalidation sources sharing one
                        // field, not something this fix resolves.
                        rule.invalid = false;
                        rule.invalidReason.clear();
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("AdvancedCondition::Resolve threw: {}", e.what());
        } catch (...) {
            spdlog::error("AdvancedCondition::Resolve threw a non-standard exception.");
        }
    }

    void ClearCache() {
        // Deliberately does not reset g_sanityChecked/g_disabled - see
        // AdvancedUsable's comment: that is a property of the game binary,
        // not of any particular save.
        g_cache.clear();
    }

}  // namespace OS::AdvancedCondition
