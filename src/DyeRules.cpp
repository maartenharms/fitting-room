#include "DyeRules.h"

#include "BuildChannel.h"
#include "DyeStatDeed.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace OS {

    namespace {
        const auto kRulesDir = BuildChannel::DataPath("Unlocks").string();
        // Same cap DyePalette uses: a hand written rules file is a few KB, and
        // parseFromStream reads the whole file into memory before it parses a
        // byte.
        constexpr std::uintmax_t kMaxRulesFileBytes = 256 * 1024;

        std::mutex g_lock;
        DyeRuleSet g_rules;
        // Guarded by g_lock, written by Load beside g_rules so the data and
        // the "trust it" bit can never disagree. Starts false: nothing has been
        // read yet, so nothing may be granted.
        bool       g_healthy = false;

        // ⚠ A FIFTH local copy, on purpose. Lower lives in an anonymous
        // namespace in DyePalette.cpp, PresetStore.cpp, AutoPresets.cpp and
        // SetDetector.cpp, so none of them is visible here and the loop below
        // will not compile without one. Following the codebase rather than
        // hoisting a shared header for four lines; if that ever gets hoisted,
        // this goes with the other four.
        // Count a failed file and record why, capping the message list.
        // filesFailed carries the true count, so a directory of hundreds of bad
        // files costs hundreds of increments and twenty strings rather than
        // hundreds of spdlog::error lines on the load path.
        void Fail(DyeRules::LoadReport& a_report, std::string a_why) {
            ++a_report.filesFailed;
            if (a_report.failures.size() < DyeRules::kMaxReportedFailures) {
                a_report.failures.push_back(std::move(a_why));
            } else if (a_report.failures.size() == DyeRules::kMaxReportedFailures) {
                a_report.failures.emplace_back("... further failures not listed");
            }
        }

        // Say something about a file that did NOT fail. Capped like Fail, and
        // for the same reason: a directory of files must not put hundreds of
        // lines on the load path.
        //
        // ⚠ IT COUNTS NOTHING. A note must never touch filesFailed, because
        // filesFailed is the promotion gate and freezing a player's whole
        // palette over a legitimately empty section would be the same
        // over-reaction as freezing over an unreadable readme.
        void Note(DyeRules::LoadReport& a_report, std::string a_what) {
            if (a_report.notes.size() < DyeRules::kMaxReportedFailures) {
                a_report.notes.push_back(std::move(a_what));
            } else if (a_report.notes.size() == DyeRules::kMaxReportedFailures) {
                a_report.notes.emplace_back("... further notes not listed");
            }
        }

        std::string Lower(std::string a_s) {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char a_c) {
                return static_cast<char>(std::tolower(a_c));
            });
            return a_s;
        }

        // Merge one JSON object of name -> condition array into a map, counting
        // rejects. Shared by "tiers" and "dyes", which have identical shape.
        // a_what names the section for the empty-key notes, since the caller
        // cannot tell a tier name from a dye id after the fact.
        void MergeSection(
            const Json::Value& a_section, const char* a_what,
            std::map<std::string, std::vector<DyeCondition>, std::less<>>& a_into,
            DyeRuleSet::MergeResult& a_result) {
            // Nothing to walk, and getMemberNames() would throw. ⚠ This return
            // REPORTS NOTHING and must not be asked to: it fires for a section
            // that is simply absent, which is fine, and for one that is a
            // mis-edit, which is not. MergeFromJson has already told those two
            // apart into tiersState and dyesState, and Load turns a mis-edit
            // into a file failure from there.
            if (!a_section.isObject()) {
                return;
            }
            for (const auto& key : a_section.getMemberNames()) {
                std::vector<DyeCondition> conds;
                if (!ConditionsFromJson(a_section[key], conds)) {
                    ++a_result.rejected;
                    a_result.rejectedKeys.push_back(key);
                    // ⚠ STORE kNever, do NOT skip the key. Skipping would leave
                    // the dye with no rule, which resolves as FREE, and a freed
                    // colour is promoted into the save and survives the typo
                    // being fixed. Locking is the reversible direction: fix the
                    // file, reload, promotion runs again.
                    a_into[key] = { DyeCondition{ DyeCondKind::kNever } };
                    continue;
                }
                // ⚠ PARSED FINE AND GATES NOTHING. AllSatisfied of an empty
                // list is true, so this key hands out every dye it covers. It
                // is the documented way to write "deliberately free", and on a
                // TIER it frees a whole rarity at once, so it is said out loud
                // and never counted as a failure. Recorded before the move,
                // which empties conds.
                if (conds.empty()) {
                    a_result.emptyKeys.push_back(std::string(a_what) + " \"" +
                                                 key + "\"");
                }
                a_into[key] = std::move(conds);
            }
        }
    }  // namespace

    DyeRuleSet::MergeResult DyeRuleSet::MergeFromJson(const Json::Value& a_root) {
        MergeResult result;
        // jsoncpp's operator[] throws Json::LogicError on anything that is not
        // an object or null, so a root that is not an object is dropped whole
        // rather than reached into. Both sections stay kAbsent, which is what
        // tells Load this file contributed nothing despite parsing.
        if (!a_root.isObject()) {
            return result;
        }
        const auto& tiers = a_root["tiers"];
        const auto& dyes  = a_root["dyes"];
        // ⚠ EACH SECTION ON ITS OWN. One shared bool used to be enough for
        // both, so a valid "tiers" answered for a broken "dyes" and the file
        // loaded clean with half of itself missing.
        //
        // ⚠ isMember, NOT isNull. operator[] answers with the shared null
        // value both for a key that is absent and for one written literally
        // null, and those are different edits: the first file never claimed to
        // have the section, the second wrote it and got it wrong.
        const auto classify = [&a_root](const char*        a_key,
                                        const Json::Value& a_value) {
            if (!a_root.isMember(a_key)) {
                return MergeResult::Section::kAbsent;
            }
            return a_value.isObject() ? MergeResult::Section::kObject
                                      : MergeResult::Section::kWrongType;
        };
        result.tiersState = classify("tiers", tiers);
        result.dyesState  = classify("dyes", dyes);
        // Only meaningful where the section really is an object. size() on a
        // string or a number would answer something, and it would mean nothing.
        if (result.tiersState == MergeResult::Section::kObject) {
            result.tiersKeys = tiers.size();
        }
        if (result.dyesState == MergeResult::Section::kObject) {
            result.dyesKeys = dyes.size();
        }
        // ⚠ A CLOSED SCHEMA, and it is the only way a misspelled section can
        // be seen at all. Neither state above changes when "dyes" is written
        // "dye": dyes reads kAbsent and tiers reads kObject, exactly like a
        // legitimate file that only defines tiers. The stray key is the whole
        // evidence, so it is collected rather than ignored.
        for (const auto& key : a_root.getMemberNames()) {
            if (key != "tiers" && key != "dyes") {
                result.unknownKeys.push_back(key);
            }
        }
        MergeSection(tiers, "tier", tiers_, result);
        MergeSection(dyes, "dye", dyes_, result);
        return result;
    }

    // ⚠ THE ONE RESOLUTION. For() and Covers() were two hand-written copies of
    // this walk with a comment between them asking that they stay in step, and
    // a comment cannot hold that line: the day a third fallback is added to
    // one, the other keeps answering for two. The failing direction is not
    // symmetric either. A Covers that missed something would only add a
    // spurious warning; a Covers that says true for something unauthored
    // silences the only warning there is, and the dye it was quiet about is
    // already permanent in the save by then.
    const std::vector<DyeCondition>* DyeRuleSet::Find(
        std::string_view a_id, std::string_view a_rarity) const {
        if (const auto it = dyes_.find(a_id); it != dyes_.end()) {
            return &it->second;
        }
        if (!a_rarity.empty()) {
            if (const auto it = tiers_.find(a_rarity); it != tiers_.end()) {
                return &it->second;
            }
        }
        return nullptr;
    }

    std::vector<DyeCondition> DyeRuleSet::For(std::string_view a_id,
                                              std::string_view a_rarity) const {
        const auto* found = Find(a_id, a_rarity);
        return found ? *found : std::vector<DyeCondition>{};
    }

    bool DyeRuleSet::Covers(std::string_view a_id,
                            std::string_view a_rarity) const {
        // Kept as its own function rather than an out parameter on For():
        // For() answers a question about one dye and this answers one about the
        // FILE, and the caller that needs this one also needs it for dyes it is
        // not going to promote.
        return Find(a_id, a_rarity) != nullptr;
    }

    std::vector<std::string> DyeRuleSet::DyeKeys() const {
        std::vector<std::string> keys;
        keys.reserve(dyes_.size());
        for (const auto& [id, conds] : dyes_) {
            keys.push_back(id);
        }
        return keys;   // sorted: dyes_ is a std::map, so the report is stable
    }

    namespace {
        // Both collectors walk the same two maps and differ only in which kind
        // they are looking for and which field they take, so they share this.
        template <typename Pick>
        void CollectRefs(const std::map<std::string, std::vector<DyeCondition>,
                                        std::less<>>& a_map,
                         DyeCondKind a_kind, Pick a_pick,
                         std::set<QuestRef>& a_refs) {
            for (const auto& [name, conds] : a_map) {
                for (const auto& c : conds) {
                    if (c.kind == a_kind) {
                        a_refs.insert(a_pick(c));
                    }
                }
            }
        }
    }  // namespace

    std::set<QuestRef> DyeRuleSet::QuestRefs() const {
        std::set<QuestRef> refs;
        const auto pick = [](const DyeCondition& a_c) { return a_c.quest; };
        CollectRefs(tiers_, DyeCondKind::kQuest, pick, refs);
        CollectRefs(dyes_, DyeCondKind::kQuest, pick, refs);
        return refs;
    }

    std::set<QuestRef> DyeRuleSet::LocationRefs() const {
        std::set<QuestRef> refs;
        const auto pick = [](const DyeCondition& a_c) { return a_c.location; };
        CollectRefs(tiers_, DyeCondKind::kLocationCleared, pick, refs);
        CollectRefs(dyes_, DyeCondKind::kLocationCleared, pick, refs);
        return refs;
    }

    std::set<std::string, std::less<>> DyeRuleSet::StatNames() const {
        std::set<std::string, std::less<>> names;
        // ⚠ NOT CollectRefs. That template collects a QuestRef per matching
        // clause and every kDeed clause carries an empty one, so reusing it
        // here would return a set containing exactly one empty ref. The shape
        // is similar and the field is not.
        //
        // ⚠ AND IT FILTERS. Only the deeds carrying the "stat:" prefix are
        // Skyrim's; channelsDyed is ours and dispatching it would ask the
        // engine for a counter it has never heard of. StatNameFromDeed is the
        // one place that rule lives.
        const auto collect =
            [&names](const std::map<std::string, std::vector<DyeCondition>,
                                    std::less<>>& a_map) {
                for (const auto& [key, conds] : a_map) {
                    for (const auto& c : conds) {
                        if (c.kind != DyeCondKind::kDeed) {
                            continue;
                        }
                        if (const auto stat = StatNameFromDeed(c.deed)) {
                            names.emplace(*stat);
                        }
                    }
                }
            };
        collect(tiers_);
        collect(dyes_);
        return names;
    }

    void DyeRuleSet::Clear() {
        tiers_.clear();
        dyes_.clear();
    }

    namespace DyeRules {

        LoadReport Load() {
            // Sorted filename order, so "a later file wins a key" is a
            // deterministic statement rather than whatever the filesystem
            // handed back that day. Same rule DyePalette::Load follows.
            LoadReport                         report;
            std::vector<std::filesystem::path> files;

            // ⚠ EVERY error_code HERE IS LOAD BEARING, and the first draft
            // dropped all of them. An unreadable Unlocks directory then
            // reported filesScanned 0, filesFailed 0, no failures: a clean load
            // of nothing. Promotion's gate opens on filesFailed, g_rules
            // publishes empty, every dye resolves free because empty means
            // free, and all 306 are written permanently into the player's save
            // with no log line at all.
            //
            // This shape was copied from DyePalette::Load, which has the same
            // blind spot and does not care: there the consequence is an empty
            // colour list, visible at a glance and persisted nowhere. Copying
            // the shape without re-deriving the consequence is what made it a
            // defect here.
            std::error_code existsEc;
            const bool      present = std::filesystem::exists(kRulesDir, existsEc);
            if (existsEc) {
                // Could not even ask. NOT the same as absent, so it must not
                // read as "the user removed their rules".
                Fail(report, std::string(kRulesDir) +
                                 ": could not be checked, " + existsEc.message());
            } else if (!present) {
                // ⚠ ABSENT AND BROKEN ARRIVE HERE IDENTICALLY. exists()
                // follows a reparse point through to its target, so a junction
                // or symlink whose target is gone answers false with the error
                // code CLEAR: byte for byte the answer a player who never
                // installed rules gets. That answer means "everything free,
                // nothing warns", deliberately, so a broken install must not
                // be allowed to borrow it. Mod managers make this ordinary:
                // MO2's USVFS and Vortex both deploy by linking, and mklink /J
                // needs no elevation.
                //
                // symlink_status does NOT follow the final link, so it still
                // sees the NAME. A junction reports file_type::junction and
                // exists() on that status is true even while the target is
                // gone, which is the discriminator.
                //
                // ⚠ linkEc IS PASSED AND DELIBERATELY NOT READ. It is here
                // only to select the non-throwing overload.
                //
                // A set linkEc is the ordinary absent case: measured on MSVC, a
                // plain missing Unlocks answers ERROR_FILE_NOT_FOUND here, and
                // ERROR_PATH_NOT_FOUND when its parent is missing too. An
                // earlier comment claimed the !linkEc conjunct was what kept
                // that case clean. It was not. Re-measured on MSVC: both absent
                // shapes come back file_type::not_found, so exists(linkSt) is
                // ALREADY false and decides them on its own, while the dangling
                // junction comes back file_type::junction with linkEc CLEAR.
                //
                // Conjoining !linkEc therefore changed nothing except in the
                // one case where the name is known to exist in some form and an
                // error is set anyway. There it suppressed the check, which is
                // the direction that frees dyes, so it is gone.
                std::error_code linkEc;
                const auto      linkSt =
                    std::filesystem::symlink_status(kRulesDir, linkEc);
                if (std::filesystem::exists(linkSt)) {
                    Fail(report,
                         std::string(kRulesDir) +
                             ": the name exists but its target could not be "
                             "resolved, so the rules could not be read");
                }
            }

            std::error_code ec;
            if (!existsEc && present) {
                // ⚠ An explicit loop with increment(ec), NOT a range-based for.
                // directory_iterator's range-based for always advances through
                // the THROWING operator++, even when the iterator itself was
                // constructed with an error_code. This is copied from
                // DyePalette::Load, which documents the same trap; a throw here
                // reaches kDataLoaded with no handler above it.
                std::filesystem::directory_iterator       it(kRulesDir, ec);
                const std::filesystem::directory_iterator end;
                while (!ec && it != end) {
                    // A per-entry code, separate from the scan's own: one entry
                    // this process cannot stat is dealt with alone, never
                    // mistaken for the directory ending.
                    //
                    // ⚠ AND IT IS READ. It was collected and dropped until
                    // 2026-08-02, one line under the fix that existed to stop
                    // exactly that, which made a skipped entry invisible: no
                    // failure, no count, no log line. A directory holding
                    // nothing but a dangling link named eso.json then reported
                    // identically to an empty one, which is the very pair the
                    // check above refuses to confuse.
                    std::error_code entryEc;
                    const auto&     entry   = *it;
                    const bool      regular = entry.is_regular_file(entryEc);
                    // ⚠ THE NAME DECIDES, NOT THE STAT. extension() is string
                    // work on what the iterator already handed back, so it is
                    // still right for an entry that could not be examined at
                    // all. That is what lets a dangling link named eso.json be
                    // reported instead of silently skipped.
                    const bool wantsJson =
                        Lower(entry.path().extension().string()) == ".json";
                    if (!wantsJson) {
                        // Deliberately silent, entryEc and all. Unlocks lives
                        // in Data, where MO2 leaves meta.ini and users leave
                        // readme.txt and backup folders, and none of those can
                        // free a dye. Freezing the whole palette over an
                        // unreadable readme would spend the one gate that has
                        // to keep meaning something on a false alarm.
                    } else if (entryEc) {
                        Fail(report, entry.path().filename().string() +
                                         ": could not be examined, " +
                                         entryEc.message());
                    } else if (!regular) {
                        // No error, simply not a file. An archive that
                        // extracted eso.json as a FOLDER lands here, and
                        // skipping it loses every rule in it and frees every
                        // dye it covered.
                        Fail(report, entry.path().filename().string() +
                                         ": named like a rules file but is not "
                                         "a regular file");
                    } else {
                        files.push_back(entry.path());
                    }
                    it.increment(ec);
                }
                // ⚠ Set by the iterator's constructor OR by any increment. A
                // mid-scan failure is the same bug with a partial file list:
                // the files already collected load, the rest silently do not
                // exist, and every dye they covered goes free.
                if (ec) {
                    Fail(report, std::string(kRulesDir) + ": scan failed, " +
                                     ec.message());
                }
            }
            std::sort(files.begin(), files.end());

            DyeRuleSet loaded;
            for (const auto& path : files) {
                ++report.filesScanned;
                const auto name = path.filename().string();

                // ⚠ The cap is ADVISORY, not a defence. file_size runs on the
                // path and parseFromStream then reads whatever is there now.
                // That race is fine here: this guards against an accidental
                // huge file in the player's own Data folder, not against
                // anyone crafting one.
                std::error_code sizeEc;
                const auto      size = std::filesystem::file_size(path, sizeEc);
                if (sizeEc) {
                    // Split from the size case deliberately. "unreadable or too
                    // large" is the same collapse this module refuses to make
                    // one level up, and a user cannot tell from it which
                    // happened.
                    Fail(report, name + ": could not be measured, " + sizeEc.message());
                    continue;
                }
                if (size > kMaxRulesFileBytes) {
                    Fail(report, name + ": " + std::to_string(size) +
                                     " bytes, over the " +
                                     std::to_string(kMaxRulesFileBytes) +
                                     " byte cap");
                    continue;
                }

                std::ifstream in(path);
                if (!in) {
                    Fail(report, name + ": could not be opened");
                    continue;
                }

                Json::Value             root;
                Json::CharReaderBuilder rb;
                std::string             errs;
                // ⚠ NOT the jsoncpp defaults, and the defaults are the sixth
                // way this module has silently freed a whole palette.
                //
                // failIfExtra defaults to FALSE, which means everything after
                // the root value is discarded without comment. One extra "}"
                // that closes the object early leaves the entire rest of the
                // file as trailing text, and so does a file that ended up as
                // two concatenated objects. Measured on the shipped shape: the
                // "dyes" section vanishes, the parse returns TRUE, filesFailed
                // stays 0, Healthy() stays true, and eso:master-gold drops onto
                // a Common tier that is [{"type":"always"}], free at level 1.
                //
                // rejectDupKeys defaults to FALSE, which keeps only the LAST
                // value for a repeated key. The tier block is the most copy
                // pasted thing in a rules file, so a duplicated "tiers" replaces
                // the whole tier table with whatever the second one holds, and
                // every colour gated by rarity resolves free.
                //
                // ⚠ NEITHER IS VISIBLE TO THE CLOSED SCHEMA BELOW. In the
                // trailing-text cases the lost content never becomes JSON at
                // all, so getMemberNames() never returns it and unknownKeys is
                // empty. In the duplicate cases the parser collapses the keys
                // before MergeFromJson is ever called, so MergeResult sees one
                // well formed section that happens to be the wrong one. The
                // reader is the only layer that can see any of the four.
                //
                // ⚠ allowTrailingCommas is deliberately LEFT ALONE. A trailing
                // comma parses to exactly the object the author meant, so
                // rejecting it would freeze a whole palette over a typo that
                // cannot free a single dye. Strictness here is only worth
                // spending where the file can lose content.
                //
                // allowComments stays on, so real // and /* */ comments still
                // parse before, inside and after the root object.
                rb["failIfExtra"]   = true;
                rb["rejectDupKeys"] = true;
                try {
                    if (!Json::parseFromStream(rb, in, &root, &errs)) {
                        Fail(report, name + ": " + errs);
                        continue;
                    }
                } catch (const std::exception& e) {
                    // jsoncpp's nesting guard THROWS past its stack limit
                    // rather than returning false. Same catch DyePalette::Load
                    // carries, and for the same reason: a few KB of brackets
                    // must not crash the game at kDataLoaded.
                    Fail(report, name + ": " + e.what());
                    continue;
                }
                const auto merged = loaded.MergeFromJson(root);
                report.rulesRejected += merged.rejected;
                report.rejectedKeys.insert(report.rejectedKeys.end(),
                                           merged.rejectedKeys.begin(),
                                           merged.rejectedKeys.end());

                // ⚠ Parsed cleanly and still lost rules. All three routes
                // below look exactly like success from the parser, and every
                // one of them leaves dyes it should have covered with no rule,
                // which means FREE. Counted as a FILE failure so promotion
                // freezes rather than writing the whole palette permanently
                // into the player's save.
                //
                // Ordered most specific first, because the message is the only
                // thing the user gets and one is reported per file.
                using Section = DyeRuleSet::MergeResult::Section;
                if (merged.tiersState == Section::kWrongType ||
                    merged.dyesState == Section::kWrongType) {
                    // ⚠ PER SECTION, and it comes before the SawSection check
                    // on purpose. One bool used to answer for both sections,
                    // so the shape that actually ships, a file with BOTH of
                    // them, could never report a mis-edit: a healthy "tiers"
                    // answered for a broken "dyes" and the file loaded clean
                    // with half of itself gone. Asking SawSection first would
                    // rebuild that mask, because a healthy sibling makes it
                    // true.
                    //
                    // Name the section too. "one of your sections is wrong"
                    // leaves the user bisecting a file we already measured.
                    const char* which =
                        merged.tiersState != Section::kWrongType
                            ? "\"dyes\" is present but not an object"
                        : merged.dyesState != Section::kWrongType
                            ? "\"tiers\" is present but not an object"
                            : "\"tiers\" and \"dyes\" are present but not "
                              "objects";
                    Fail(report, name + ": " + which);
                } else if (!merged.unknownKeys.empty()) {
                    // ⚠ THE PLAN'S OWN ACCEPTANCE TEST, and nothing else
                    // catches it. Renaming "dyes" to "dye" leaves tiers a
                    // valid object and dyes merely absent, which is what a
                    // legitimate tiers-only file looks like, so the stray key
                    // is the only evidence there is. Measured on the shipped
                    // file's shape: without this, all 28 per-dye overrides
                    // vanish, ten of them fall onto a Common tier that is
                    // [{"type":"always"}], and eso:master-gold goes free at
                    // level 1.
                    std::string why = name + ": unknown top level key \"" +
                                      merged.unknownKeys.front() + "\"";
                    if (merged.unknownKeys.size() > 1) {
                        why += " and " +
                               std::to_string(merged.unknownKeys.size() - 1) +
                               " more";
                    }
                    why += ", only \"tiers\" and \"dyes\" are read";
                    Fail(report, std::move(why));
                } else if (!merged.SawSection()) {
                    // The oldest route: no usable section at all. A root that
                    // is a JSON array or a string, or an empty object.
                    Fail(report, name + ": parsed, but has no tiers or dyes object");
                } else {
                    // ⚠ THE FILE IS FINE AND MAY STILL HAVE FREED THINGS. A
                    // present but EMPTY section is spec-compliant, and writing
                    // one deliberately is a legitimate way to say "this file
                    // opts into the format and gates nothing". It is also
                    // exactly what a section looks like after a bad edit
                    // emptied it, and either way every dye that section would
                    // have covered resolves FREE.
                    //
                    // Not a failure, on purpose: a legitimate author must not
                    // have the whole palette frozen on them. Not silent either,
                    // which is the mistake this module keeps making.
                    if (merged.tiersState == Section::kObject &&
                        merged.tiersKeys == 0) {
                        Note(report, name +
                                         ": \"tiers\" is present and empty, no "
                                         "tier rules from this file");
                    }
                    if (merged.dyesState == Section::kObject &&
                        merged.dyesKeys == 0) {
                        Note(report, name +
                                         ": \"dyes\" is present and empty, no "
                                         "dye rules from this file");
                    }
                    // ⚠ THE SAME THING ONE GRAIN FINER, and it used to be the
                    // half nobody said. An empty SECTION got a note; an empty
                    // RULE inside a healthy section got nothing, while freeing
                    // every dye it covers just as completely. On a tier that is
                    // a whole rarity at once.
                    for (const auto& key : merged.emptyKeys) {
                        Note(report, name + ": " + key +
                                         " is present and empty, so nothing "
                                         "gates it");
                    }
                }
            }

            {
                std::scoped_lock l(g_lock);
                g_rules   = std::move(loaded);
                // Set under the SAME lock as the data it describes, so no
                // reader can see a rule set without the bit that says whether
                // to believe it.
                g_healthy = report.filesFailed == 0;
            }
            return report;
        }

        DyeRuleSet SnapshotUnchecked() {
            std::scoped_lock l(g_lock);
            return g_rules;
        }

        bool Healthy() {
            std::scoped_lock l(g_lock);
            return g_healthy;
        }

        Checked SnapshotChecked() {
            // ⚠ ONE acquisition. Healthy() then Snapshot() is two, and a Load
            // landing between them can hand a caller the trust bit from before
            // a reload with the rules from after a failed one.
            std::scoped_lock l(g_lock);
            return Checked{ g_rules, g_healthy };
        }

    }  // namespace DyeRules

}  // namespace OS
