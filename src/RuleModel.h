#pragma once

// Pure data model for the outfit rules engine. Names no engine types: this
// header compiles into the pure-logic test executables, same discipline as
// Outfit.h and RefreshGate.h.

#include "Outfit.h"  // SlotEntry, StyleRefKey - reused verbatim

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace OS::Rules {

    // A load-order-independent form reference. Same shape and same reason as
    // StyleRefKey: a raw FormID frozen into a rules file would retarget the
    // moment the user's load order changes.
    struct FormKey {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool Empty() const { return modName.empty() && localFormID == 0; }
        friend bool operator==(const FormKey&, const FormKey&) = default;
        friend auto operator<=>(const FormKey&, const FormKey&) = default;
    };

    enum class ConditionKind : std::uint8_t {
        kLocation = 0,
        kCell,
        kInterior,
        kWeather,
        kTimeOfDay,
        kCombat,
        kSneaking,
        kSwimming,
        kMounted,
        kWornSlot,
        kAdvanced,
        // Appended after kAdvanced deliberately. Nothing serializes the numeric
        // value - RuleCodec writes the kind's NAME, and the co-save stores that
        // same JSON as text (Persistence.cpp's 'RULE' record) - so the position
        // is free, but keeping the existing values put means a log or a
        // debugger reading an older note still lines up.
        //
        // ⚠ RuleCodec::KindFromName walks 0..LAST by hand. Adding here means
        // moving that bound, or the new kinds decode as "unknown" and every
        // clause using them is silently dropped on load.
        kDialogue,
        kRegion,
        kVampire,
        // ⚠⚠ THE ONLY CONDITION WHOSE ARRIVAL AND DEPARTURE COME FROM DIFFERENT
        // PLACES, and that is what shapes everything about it. Every kind above
        // is a state the snapshot reads off the world. This one arrives as an
        // EVENT (TESSpellCastEvent, which WorldWatch's sink turns into an
        // immediate evaluate, because a robe that appears a heartbeat after the
        // fireball is a robe that appears for no reason) and departs as a
        // STATE, because no event fires when a cast ends.
        //
        // So the snapshot's `casting` is "the player is casting, or stopped
        // less than [Rules] fCastHoldSeconds ago". The first half is a live
        // engine read, which is the only thing that can tell a spell held down
        // from one thrown once; the second is a short tail across the gap
        // between casts. WorldWatch::CastingHeld owns both.
        //
        // ⚠ THE DEPARTURE COSTS MORE ATTENTION THAN THE ARRIVAL, and it went
        // out first on 2026-08-13 with none. Nothing fires when a cast ends, so
        // the return trip needs its own arrangements: WorldWatch's 100ms poller
        // watches for it, and RuleEngine's WantsBypass cases this kind so the
        // return is not dwell-gated on top. Field report, 2026-08-14: "it takes
        // so long to switch back when we stop casting".
        kCasting,
    };

    enum class WeatherFlag : std::uint8_t { kRaining = 0, kSnowing = 1 };

    struct Condition {
        ConditionKind kind{ ConditionKind::kInterior };
        bool          negate{ false };

        FormKey       form;                              // kLocation, kCell, kRegion
        WeatherFlag   weather{ WeatherFlag::kRaining };   // kWeather
        float         startHour{ 0.0f };                 // kTimeOfDay
        float         endHour{ 0.0f };                   // kTimeOfDay
        std::uint32_t slotBit{ 0 };                      // kWornSlot
        std::string   advancedText;                      // kAdvanced

        friend bool operator==(const Condition&, const Condition&) = default;
    };

    // What a rule puts on the character. The three values exist so the two
    // special cases have distinct encodings: an empty outfit name used to mean
    // both "leave the look alone" and "wear real gear", which is ambiguous.
    enum class BaseKind : std::uint8_t { kKeep = 0, kRealGear = 1, kOutfit = 2 };

    struct Base {
        BaseKind    kind{ BaseKind::kKeep };
        std::string outfitName;  // kOutfit only

        friend bool operator==(const Base&, const Base&) = default;
    };

    using Overlay = std::map<std::uint32_t, SlotEntry>;  // slot bit -> entry

    struct Rule {
        std::string            id;
        std::string            name;
        bool                   enabled{ true };
        int                    priority{ 0 };
        Base                   base;
        Overlay                overlay;
        std::vector<Condition> conditions;

        // Set by the loader, not serialized: a rule whose Advanced text will
        // not parse, or whose form refs are gone, is kept but never wins.
        bool        invalid{ false };
        std::string invalidReason;

        // Set by the store for pack rules; empty for the save's own rules.
        std::string packName;
    };

    using RuleSet = std::vector<Rule>;  // already in merged evaluation order

    struct WorldSnapshot {
        std::vector<FormKey> locationKeywords;  // current location + parents
        std::vector<FormKey> regions;           // every region the player's cell is in
        FormKey              cell;
        bool                 interior{ false };
        // The vanilla dialogue menu is up. ⚠ Named for the MENU, not for a
        // scene: a scene mod (OStim etc.) suspends the whole override through
        // SceneGuard and never reaches an evaluation at all.
        bool                 inDialogue{ false };
        bool                 vampire{ false };
        bool                 raining{ false };
        bool                 snowing{ false };
        float                gameHour{ 0.0f };
        bool                 inCombat{ false };
        // The player cast something within the hold window. See kCasting: this
        // is a latch WorldWatch keeps, not a live "is casting" read, because
        // the engine samples state and a cast is an instant.
        bool                 casting{ false };
        bool                 sneaking{ false };
        bool                 swimming{ false };
        bool                 mounted{ false };
        std::uint32_t        wornSlotMask{ 0 };
        // Advanced results, resolved engine-side before the pure evaluation.
        // Key is (rule id, clause index within that rule's condition list).
        std::map<std::pair<std::string, std::size_t>, bool> advanced;
        double                                             nowSeconds{ 0.0 };
    };

    enum class Outcome : std::uint8_t {
        kNoChange = 0,  // the derived target already is the applied state
        kApply,
        kSuppressed,    // dwell window still open
        kPaused,        // user pinned a look
    };

    struct Decision {
        Outcome     outcome{ Outcome::kNoChange };
        std::string ruleId;   // kApply/kSuppressed; empty = the no-rule target
        Base        base;     // kApply/kSuppressed
        Overlay     overlay;  // kApply/kSuppressed
        float       dwellRemaining{ 0.0f };  // kSuppressed
        std::string pinnedName;              // kPaused; empty = real gear

        // The id PickWinner actually chose this evaluation, set on every
        // outcome EXCEPT kPaused (the pinned early-return in Evaluate never
        // reaches PickWinner) - including kNoChange, which otherwise leaves
        // ruleId/base/overlay at their empty defaults because the derived
        // target already matches what's applied. A reader that wants "what
        // currently matches" independent of dwell/pin/no-change collapsing
        // (the Rules tab's status strip and chip coloring) reads this
        // instead of re-deriving its own copy of PickWinner's tie-break
        // logic against a merged rule list that may disagree with the
        // engine about which rules are invalid (a pack rule's Advanced-
        // clause invalidity is never persisted back to RuleStore - see
        // WorldWatch.cpp's PersistAdvancedValidity - so Merged() alone
        // cannot answer this correctly for a pack rule). Empty when no rule
        // matches.
        std::string matchedRuleId;

        // EVERY rule matching this evaluation, highest priority first, not just
        // the one that set the base. Overlays compose additively across all of
        // them (see Evaluate), so "which rule is live" genuinely has more than
        // one answer and a UI that marks only matchedRuleId would be claiming
        // the others are doing nothing. Empty when nothing matches, and - like
        // matchedRuleId - never filled on kPaused, because the pinned
        // early-return never reaches the composition step.
        std::vector<std::string> activeRuleIds;

        // The overlay the engine is composing RIGHT NOW, and which rule won
        // each slot.
        //
        // Distinct from `overlay` above, which is only filled on kApply and
        // kSuppressed: these are set on EVERY evaluation, the steady-state
        // kNoChange included, for the same reason matchedRuleId is. The outfit
        // editor reads them to explain why a slot looks the way it does - a
        // helmet rule hiding slot 31 otherwise makes the helmet row look
        // inexplicably empty while you are browsing helmets. Empty on kPaused:
        // the pinned early-return never reaches the composition step, and
        // nothing is being composed while paused.
        Overlay                              activeOverlay;
        std::map<std::uint32_t, std::string> overlaySource;  // slot bit -> rule id
    };

    // Per slot, an overlay entry beats the base outfit's entry. Composition
    // produces an OUTFIT, not a side container, because Outfit is the type
    // every downstream player consumer already takes: ComputeDisplaySet(const
    // Outfit&), VisitStyles' ForEachStyle walk, and DisplayBody's torso masks
    // all read one. Composing into a new map-shaped type would have forced a
    // conversion at each of those three sites.
    //
    // An overlay over a DEFAULT-constructed Outfit yields an outfit carrying
    // only the overlay entries: that is the kKeep-over-real-gear case, and it
    // is why the hooks engage on "the composed outfit shows something" rather
    // than on "an outfit is active".
    [[nodiscard]] inline Outfit Compose(const Outfit& a_base, const Overlay& a_overlay) {
        Outfit out = a_base;
        for (const auto& [bit, entry] : a_overlay) {
            if (bit >= kBitCount) {
                continue;
            }
            if (entry.kind == SlotEntry::Kind::kStyle) {
                out.SetStyle(bit, entry.style);
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                out.SetHide(bit);
            }
        }
        return out;
    }

    // Reversible hide toggle for one overlay slot: Outfit.h's ToggleHideSlot in
    // the Rules model's own shape, and it exists for the same reason.
    //
    // ⚠⚠ THE HIDE CARRIES THE STYLE IT COVERS, so the round trip is lossless
    // and a click on a styled row cannot throw the style away. The model
    // already expects a hide to hold a key it does not act on: SameEntry
    // delegates to SlotDiffers precisely because "a kHide entry can carry a
    // stale style key", and Compose above reads the KIND and never the key.
    // RuleCodec writes the covered key beside "hide" so the file is lossless
    // too; an older build reads "hide" first and simply ignores it.
    //
    // ⚠ A kPassthrough ("Shown") holds no key, so hiding it and showing it
    // again lands on "No override" rather than back on Shown. That is two
    // deliberate clicks and the row says what it ended up as after each, so it
    // is not a silent loss. Do not fold a third carried kind in to "fix" it:
    // the entry has one key and there is nowhere honest to put a second.
    inline void ToggleOverlayHide(Overlay& a_overlay, std::uint32_t a_bit) {
        if (a_bit >= kBitCount) {
            return;
        }
        const auto it = a_overlay.find(a_bit);
        if (it == a_overlay.end()) {
            a_overlay[a_bit] = SlotEntry{ SlotEntry::Kind::kHide, {} };
            return;
        }
        if (it->second.kind != SlotEntry::Kind::kHide) {
            it->second = SlotEntry{ SlotEntry::Kind::kHide, it->second.style };
            return;
        }
        if (!it->second.style.Empty()) {
            it->second = SlotEntry{ SlotEntry::Kind::kStyle, it->second.style };
        } else {
            a_overlay.erase(it);
        }
    }

    // Overlay equality, hand-written on purpose. SlotEntry has NO operator==
    // (Outfit.h defines the free function SlotDiffers instead) because a kHide
    // entry can carry a stale style key that must not count as a difference: a
    // defaulted comparison would see two identical hides as unequal and turn a
    // kNoChange into a spurious re-apply plus refresh. This delegates to
    // SlotDiffers (Outfit.h:500) rather than re-deriving that truth table, so
    // the two cannot silently drift apart if SlotEntry::Kind gains a value or
    // SlotDiffers' rule changes - the same hazard Outfit.h:293-300 documents.
    [[nodiscard]] inline bool SameEntry(const SlotEntry& a, const SlotEntry& b) {
        return !SlotDiffers(a, b);
    }

    [[nodiscard]] inline bool SameOverlay(const Overlay& a, const Overlay& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (const auto& [bit, entry] : a) {
            const auto it = b.find(bit);
            if (it == b.end() || !SameEntry(entry, it->second)) {
                return false;
            }
        }
        return true;
    }

    // Whether one clause holds against the snapshot. a_ruleId and a_index
    // identify the clause for kAdvanced, whose truth was resolved engine-side
    // during snapshot build (this header cannot evaluate a TESCondition).
    [[nodiscard]] inline bool Matches(const Condition& a_c, const WorldSnapshot& a_s,
                                       const std::string& a_ruleId, std::size_t a_index) {
        bool hit = false;
        // NOT exhaustive by compiler enforcement: this project builds without
        // /w14062 and without /WX, so a 12th ConditionKind added without a
        // matching case here compiles silently and falls through to
        // hit = false. Update this switch by hand when ConditionKind grows.
        switch (a_c.kind) {
        case ConditionKind::kLocation:
            // Guarded like kCell: an unset form must never match, not even a
            // blank keyword entry. Whether the glue can produce one is not
            // this function's business to assume.
            if (!a_c.form.Empty()) {
                for (const auto& k : a_s.locationKeywords) {
                    if (k == a_c.form) {
                        hit = true;
                        break;
                    }
                }
            }
            break;
        case ConditionKind::kCell:
            hit = !a_c.form.Empty() && a_s.cell == a_c.form;
            break;
        case ConditionKind::kRegion:
            // Same any-of shape and same unset guard as kLocation: a cell can
            // sit in several overlapping regions at once, and an unset form
            // must never match.
            if (!a_c.form.Empty()) {
                for (const auto& r : a_s.regions) {
                    if (r == a_c.form) {
                        hit = true;
                        break;
                    }
                }
            }
            break;
        case ConditionKind::kDialogue:
            hit = a_s.inDialogue;
            break;
        case ConditionKind::kVampire:
            hit = a_s.vampire;
            break;
        case ConditionKind::kInterior:
            hit = a_s.interior;
            break;
        case ConditionKind::kWeather:
            hit = a_c.weather == WeatherFlag::kRaining ? a_s.raining : a_s.snowing;
            break;
        case ConditionKind::kTimeOfDay:
            // Inclusive start, exclusive end. start > end wraps midnight, so
            // 22 to 6 is the union of [22,24) and [0,6).
            hit = a_c.startHour <= a_c.endHour
                      ? (a_s.gameHour >= a_c.startHour && a_s.gameHour < a_c.endHour)
                      : (a_s.gameHour >= a_c.startHour || a_s.gameHour < a_c.endHour);
            break;
        case ConditionKind::kCombat:
            hit = a_s.inCombat;
            break;
        case ConditionKind::kCasting:
            hit = a_s.casting;
            break;
        case ConditionKind::kSneaking:
            hit = a_s.sneaking;
            break;
        case ConditionKind::kSwimming:
            hit = a_s.swimming;
            break;
        case ConditionKind::kMounted:
            hit = a_s.mounted;
            break;
        case ConditionKind::kWornSlot:
            hit = a_c.slotBit < kBitCount &&
                  (a_s.wornSlotMask & (1u << a_c.slotBit)) != 0;
            break;
        case ConditionKind::kAdvanced: {
            const auto it = a_s.advanced.find({ a_ruleId, a_index });
            hit           = it != a_s.advanced.end() && it->second;
            break;
        }
        }
        return a_c.negate ? !hit : hit;
    }

    // AND over every clause. An empty condition list matches: that is a rule
    // the user wants on unconditionally, and it is still gated by priority.
    [[nodiscard]] inline bool AllMatch(const Rule& a_rule, const WorldSnapshot& a_s) {
        for (std::size_t i = 0; i < a_rule.conditions.size(); ++i) {
            if (!Matches(a_rule.conditions[i], a_s, a_rule.id, i)) {
                return false;
            }
        }
        return true;
    }

    // The identity the engine compares and the log latch keys on. Two rules
    // can carry the same visible target; comparing targets rather than rule
    // ids is what makes "same look, different rule" a kNoChange.
    struct Target {
        std::string ruleId;
        Base        base;
        Overlay     overlay;

        // NOT `= default`: see SameOverlay above.
        friend bool operator==(const Target& a, const Target& b) {
            return a.ruleId == b.ruleId && a.base == b.base &&
                   SameOverlay(a.overlay, b.overlay);
        }
    };

    // Rewrites every rule in a_rules whose kOutfit base names a_from to name
    // a_to instead, and returns how many changed. Pure - no locking, no
    // I/O - so RuleStore::RenameOutfitEverywhere (RuleStore.cpp, which adds
    // the lock and the pinned-name half) and RuleModelTests can share this
    // exact logic: a rename bug reproduced in-game is reproducible here
    // too, which the wiring-level test in test_persistence.cpp cannot do on
    // its own. A no-op (returns 0, mutates nothing) for an empty a_from or
    // when a_from == a_to - neither is a real rename.
    [[nodiscard]] inline std::size_t RenameBasesIn(RuleSet& a_rules, const std::string& a_from,
                                                    const std::string& a_to) {
        if (a_from.empty() || a_from == a_to) {
            return 0;
        }
        std::size_t changed = 0;
        for (auto& rule : a_rules) {
            if (rule.base.kind == BaseKind::kOutfit && rule.base.outfitName == a_from) {
                rule.base.outfitName = a_to;
                ++changed;
            }
        }
        return changed;
    }

    // Which of the three words a rule row shows: Off, Active or Waiting.
    //
    // ⚠ ONE DEFINITION FOR BOTH ROW KINDS. The editable card and the Rule
    // Library row each spelled this ternary out for itself, with the pack row's
    // comment asserting it was "the same three-state word as an editable card".
    // Two copies of a claim that they agree is how they stop agreeing. The
    // tooltips stay separate on purpose, because a pack rule's switch is per
    // save and an editable rule's is not, so they have different things to say.
    //
    // ⚠ OFF WINS OVER ACTIVE. A switched-off rule cannot be the winner, and a
    // row that answered "Active" off a stale active set would be telling the
    // player a disabled rule is dressing them.
    [[nodiscard]] inline constexpr const char* RuleStateWord(bool a_enabled, bool a_isWinner) {
        if (!a_enabled) {
            return "Off";
        }
        return a_isWinner ? "Active" : "Waiting";
    }

}  // namespace OS::Rules
