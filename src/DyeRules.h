#pragma once

#include "DyeConditions.h"

#include <json/json.h>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // What each dye requires, read from
    // Data/SKSE/Plugins/FittingRoom/Unlocks/*.json.
    //
    // Rules live apart from colours because the colour packs are GENERATED from
    // tools/data/eso_dyes.tsv and a hand edit to a generated file dies on the
    // next run.
    //
    // Pure: no engine types, so resolution is provable in a test executable.
    class DyeRuleSet {
    public:
        struct MergeResult {
            std::size_t rejected{ 0 };
            // ⚠ WHICH keys, not just how many. A user whose tier locked and
            // whose log says "1 rule rejected" has to bisect their own file to
            // find it. The count alone is the least useful half of what we
            // already know at the point of failure.
            std::vector<std::string> rejectedKeys;

            // How one of the two top level sections turned up.
            //
            // ⚠ kWrongType IS NOT A FLAVOUR OF kAbsent, and collapsing them
            // was a real defect. A file with no "dyes" key never claimed to
            // have one. A file whose "dyes" is a string, a number or a literal
            // null wrote one and got it wrong, and every dye it was supposed
            // to cover falls back to "no rule", which means FREE.
            enum class Section { kAbsent, kObject, kWrongType };

            // ⚠ PER SECTION, and that is the whole point. Until 2026-08-02 one
            // shared bool answered for both, so a healthy "tiers" masked a
            // mis-edited "dyes" entirely. The shape that actually ships has
            // BOTH sections, so {"tiers": {valid}, "dyes": "oops"} loaded
            // clean: all 28 per-dye overrides gone, ten of them dropped onto a
            // Common tier that is [{"type":"always"}], free at level 1 and
            // written permanently into the co-save.
            Section tiersState{ Section::kAbsent };
            Section dyesState{ Section::kAbsent };

            // How many keys each section held, counted only where the state
            // above is kObject. Zero on a present but EMPTY section, which is
            // legitimate authorship and must not fail the file, but is also
            // indistinguishable in its effect from a section something emptied
            // by accident. Load turns that into a NOTE rather than a failure,
            // because this module's recurring problem is silence.
            std::size_t tiersKeys{ 0 };
            std::size_t dyesKeys{ 0 };

            // ⚠ Every top level key that is neither "tiers" nor "dyes", and
            // THE ONLY THING THAT CATCHES A MISSPELLED SECTION. The states
            // above cannot: "dyes" written "dye" leaves dyes kAbsent and tiers
            // kObject, which is character for character what a legitimate
            // tiers-only file looks like. The stray key is the only evidence
            // that the author meant something by it.
            //
            // ⚠ SO THE SCHEMA IS CLOSED, DELIBERATELY. A rules file may hold
            // those two keys and nothing else. There is no way to tell a
            // comment from a typo'd section name from in here, and guessing
            // wrong on a typo frees dyes permanently while guessing wrong on a
            // comment only locks them until the key is removed. Grow this list
            // if the format ever grows a key, and old builds refusing new files
            // is the intended outcome, not a regression.
            //
            // ⚠ AND IT COSTS NOTHING TO COMMENT A RULES FILE, which an earlier
            // version of this note got wrong by claiming it cost the "_comment"
            // convention. jsoncpp has allowComments on by default and Load
            // leaves it on, so real // and /* */ comments parse before, inside
            // and after the root object. Measured under the stricter reader
            // flags Load sets, not assumed. The fake-comment key was never
            // needed, so closing the schema takes nothing away and there is no
            // authoring cost to weigh against the guarantee.
            std::vector<std::string> unknownKeys;

            // Keys whose rule parsed to an EMPTY condition list, named as
            // "tier \"Common\"" or "dye \"eso:x\"" so the caller does not have
            // to guess which section they came from.
            //
            // ⚠ EMPTY IS FREE, AT A FINER GRAIN THAN tiersKeys/dyesKeys. Those
            // two catch a whole section someone emptied; this catches one key
            // inside a healthy section, which has exactly the same effect on
            // every dye it covers and was reported nowhere at all. "eso:x": []
            // is the documented way to say "deliberately free", so this is a
            // NOTE and never a failure, the same trade the empty section makes.
            std::vector<std::string> emptyKeys;

            // Whether either section was present AND an object, so the file
            // contributed something it could contribute.
            //
            // ⚠ DERIVED, never stored. It used to be a separate bool assigned
            // beside the states, which is one more pair of facts that can
            // disagree in a module whose entire job is that they must not.
            //
            // ⚠ TRUE IS NOT A CLEAN BILL OF HEALTH, and reading it as one is
            // what made the shipped shape unprotected. On its own it catches a
            // root that is a JSON array or a string, and an object with
            // neither key. It misses BOTH of the mis-edits that a file with a
            // healthy sibling section can carry: a wrong-typed section, which
            // tiersState and dyesState report, and a misspelled one, which
            // only unknownKeys reports. Anything deciding whether to trust the
            // file has to read all three, and Load does.
            [[nodiscard]] bool SawSection() const {
                return tiersState == Section::kObject ||
                       dyesState == Section::kObject;
            }
        };

        // Merge one file's "tiers" and "dyes" sections into this set.
        //
        // ⚠ MERGE, not load, and the name says so. Calling it LoadFromJson
        // reads like replace-then-replace when two files are folded in, which
        // is the opposite of what it does.
        //
        // Later files win on a key already claimed, which makes a load order
        // over the Unlocks directory meaningful. Load() reads in sorted
        // filename order so that is deterministic.
        //
        // ⚠ A later file's BROKEN rule also wins, replacing an earlier file's
        // good one with a lock. That is deliberate: the later author meant to
        // override, and a lock is the recoverable direction. It does mean one
        // broken third party file can lock a tier the shipped file defined
        // correctly, which is why rejects are counted and logged.
        [[nodiscard]] MergeResult MergeFromJson(const Json::Value& a_root);

        // The rule for one dye: its own entry, else its rarity's, else empty.
        //
        // ⚠ EMPTY MEANS FREE. A pack that never opted into an economy, and any
        // rarity with no tier entry, both land here. That is the deliberate
        // default: a mod author who shipped colours should not have them
        // silently locked by a rules file they never saw.
        [[nodiscard]] std::vector<DyeCondition> For(std::string_view a_id,
                                                    std::string_view a_rarity) const;

        // Whether anything was AUTHORED for this dye at all: its own entry, or
        // a tier entry for its rarity.
        //
        // ⚠ NOT DERIVABLE FROM For(). For() answers with an empty vector both
        // for a dye an author deliberately left free and for one whose rarity
        // matched no tier, and those are the same value with opposite
        // meanings. The tier lookup is an exact byte match on the rarity
        // string, so a file spelling it "Dye stamp" against a palette that
        // says "Dye Stamp" resolves to no rule, which means FREE, for all 32
        // of them.
        //
        // ⚠ THIS GATES NOTHING AND MUST NOT START TO. An unknown rarity
        // staying free is the documented fallback, because a third party pack
        // may ship a bucket this mod never heard of and locking it would spend
        // someone else's colours on our guess. All this answers is whether
        // promotion should SAY so, which is the only defence against a typo
        // that is otherwise completely silent.
        //
        // An empty rarity answers on the per-dye entry alone. Shipping a
        // colour with no rarity is the supported way to say "free".
        //
        // ⚠ SHARES ONE LOOKUP WITH For(), and that is now a mechanism rather
        // than a promise. Both go through the private Find below. They were
        // hand-mirrored until 2026-08-02 and a comment asked the next editor to
        // keep them in step, which is not something a comment can do: the day
        // For() grows a third fallback, a hand-mirrored Covers keeps answering
        // for two, and the direction that hurts is Covers saying true for
        // something unauthored, because that is what silences the only warning
        // there is.
        [[nodiscard]] bool Covers(std::string_view a_id,
                                  std::string_view a_rarity) const;

        // Every key authored in a "dyes" section, in sorted order.
        //
        // ⚠ THE ONLY WAY AN ORPHANED OVERRIDE CAN BE SEEN. A per-dye key that
        // no palette id claims, from a typo or a renamed id, is silent at every
        // other layer: it merges cleanly, it is not an unknown top level key,
        // and the dye it was meant to gate quietly falls back to its RARITY
        // TIER. Covers() then answers true off that tier, so the unmatched
        // rarity report cannot see it either. An orphan is a downgrade rather
        // than a lock, which is the direction that cannot be taken back, and
        // thirteen of the 220 sit on Common, which is [{"type":"always"}]:
        // free at level 1, permanent, no log line. See DyePromotion.h for how
        // much of "every shipped override is stricter than its tier" is
        // actually checked, and by what.
        //
        // ⚠ REPORT ONLY. Promotion names these and gates nothing on them, on
        // the same argument the unmatched rarity report rests on: a rules pack
        // may legitimately cover a colour pack the user has not installed, and
        // freezing someone's palette over that would spend the one gate that
        // has to keep meaning something.
        //
        // ⚠ OWNED STRINGS, not views into dyes_. Same trade DyePalette::Find
        // makes and for the same reason: a view stays valid only as long as the
        // rule set it came from, and this set is a snapshot a caller may
        // outlive. Twenty nine short strings once per load buys safe by
        // construction instead of safe by timing.
        [[nodiscard]] std::vector<std::string> DyeKeys() const;

        // Every quest any rule mentions. The bridge resolves these and only
        // these, rather than walking every quest in the load order.
        [[nodiscard]] std::set<QuestRef> QuestRefs() const;

        // Every location any rule mentions, resolved by the bridge the same
        // way and for the same reason.
        [[nodiscard]] std::set<QuestRef> LocationRefs() const;

        // Every SKYRIM MISC STAT any rule mentions, with the "stat:" prefix
        // already off, so the bridge dispatches exactly these names and no
        // others.
        //
        // ⚠ ONLY the deeds carrying that prefix. Fitting Room's own counters
        // (channelsDyed) are produced here, not by the engine, and asking
        // Game.QueryStat for one would answer 0 - the same 0 an honest zero
        // gives - and lock its colour. StatNameFromDeed in DyeStatDeed.h is the
        // single place that tells the two vocabularies apart.
        //
        // ⚠ A DISPATCH COSTS ~180 ms OF LATENCY EACH, so this being the rules'
        // OWN list rather than the whole hundred-odd table is the difference
        // between a dozen calls on a load and a hundred. Same argument
        // QuestRefs makes about walking the load order.
        //
        // Transparent comparator so a std::string_view looks up without
        // allocating, which is how the bridge reads it back per clause.
        [[nodiscard]] std::set<std::string, std::less<>> StatNames() const;

        [[nodiscard]] std::size_t TierCount() const { return tiers_.size(); }
        [[nodiscard]] std::size_t DyeCount() const { return dyes_.size(); }

        void Clear();

    private:
        // The one resolution, so For() and Covers() cannot drift apart. Null
        // means nothing was authored; a non-null pointer to an EMPTY vector
        // means an author deliberately wrote [], which is free and is NOT the
        // same answer.
        [[nodiscard]] const std::vector<DyeCondition>* Find(
            std::string_view a_id, std::string_view a_rarity) const;

        std::map<std::string, std::vector<DyeCondition>, std::less<>> tiers_;
        std::map<std::string, std::vector<DyeCondition>, std::less<>> dyes_;
    };

    namespace DyeRules {

        // ⚠ filesFailed and rulesRejected are NOT the same failure and must not
        // be collapsed into one number.
        //
        // A rejected RULE is contained: that one key gets kNever and its dye
        // locks, which is recoverable by fixing the file.
        //
        // A failed FILE is not contained: none of its keys exist at all, so
        // every dye it covered falls through to "no rule", which means FREE.
        // One stray comma in eso.json would otherwise unlock all 306 colours,
        // permanently, on every character loaded while it was broken. Promotion
        // refuses to run when filesFailed is non-zero for exactly that reason.
        struct LoadReport {
            std::size_t filesScanned{ 0 };
            std::size_t filesFailed{ 0 };
            std::size_t rulesRejected{ 0 };
            // One entry per failed file, "name: reason", for the log. Capped:
            // filesFailed carries the true count, and a directory of hundreds
            // of bad files would otherwise put hundreds of error lines on the
            // load path.
            std::vector<std::string> failures;
            // Which rule keys were rejected and therefore locked, so the log
            // can name them instead of leaving the user to bisect their file.
            std::vector<std::string> rejectedKeys;
            // ⚠ NOT failures, and they must never reach filesFailed. A file
            // can load perfectly and still contribute nothing, and a present
            // but empty section is the spec-compliant way to do that. It is a
            // legitimate thing to author, so it does not freeze anything; it is
            // also exactly what an accidentally emptied section looks like, and
            // in both cases every dye that section covered resolves FREE. Say
            // so rather than letting it pass in silence. Capped the same way
            // failures are.
            std::vector<std::string> notes;
        };

        inline constexpr std::size_t kMaxReportedFailures = 20;

        // Scan the directory. MAIN THREAD ONLY: kDataLoaded or a queued task.
        [[nodiscard]] LoadReport Load();

        // Consistent copy for anything off the main thread.
        //
        // ⚠ Whether to BELIEVE it is not in here. Use SnapshotChecked for
        // anything that will act on the answer; this one is for a caller that
        // genuinely does not care, such as gathering QuestRefs for the bridge
        // to resolve.
        //
        // ⚠ THE NAME IS THE WARNING, and that is the whole reason it is this
        // long. It was called Snapshot until 2026-08-02, which reads as the
        // default and pairs with a separate Healthy() call so naturally that
        // the plan's own promotion snippet was written that way. Two
        // acquisitions is the straddle SnapshotChecked exists to prevent, and
        // an implementer transcribing a plan does not stop to re-derive that.
        // Renaming it makes the old two-call shape fail to COMPILE, which is
        // the only failure mode that reliably reaches them.
        //
        // ⚠ DyeUnlocks::Snapshot() is a different function in a different
        // module and keeps its name.
        [[nodiscard]] DyeRuleSet SnapshotUnchecked();

        // Whether the last Load trusted what it read, meaning no file failed.
        //
        // ⚠ THE GATE LIVES WITH THE DATA ON PURPOSE. The first draft kept it
        // only in the LoadReport, which meant parking a copy in a plugin.cpp
        // global that exactly one caller remembered to consult. Anything else
        // holding a Snapshot, and plan three's pane needs one to grey out
        // locked swatches, would get a rule set saying "everything is free"
        // with no way to know it was lying.
        //
        // False means: do not grant anything off this. It does NOT mean the
        // snapshot is empty, and it does not mean the player has no rules; it
        // means some of them could not be read, so what is missing is unknown.
        //
        // ⚠ On its own this only answers for the load that HAS ALREADY
        // FINISHED. Pair it with a snapshot through SnapshotChecked rather
        // than calling both.
        [[nodiscard]] bool Healthy();

        // A rule set and the bit that says whether to believe it, read under
        // ONE lock.
        struct Checked {
            DyeRuleSet rules;
            bool       healthy{ false };
        };

        // ⚠ TAKE BOTH AT ONCE OR THE PAIR MEANS NOTHING. Healthy() then
        // Snapshot() is two acquisitions, and off the main thread a Load can
        // land between them. The dangerous straddle is healthy TRUE from
        // before a reload paired with rules from after a FAILED one: the
        // caller then grants off a set it was told to trust, which is the one
        // direction this module cannot take back, because unlocks are sticky.
        //
        // Storage has always guaranteed the pairing, both are written under
        // g_lock together. Only the API let a caller take them apart.
        [[nodiscard]] Checked SnapshotChecked();

    }  // namespace DyeRules

}  // namespace OS
