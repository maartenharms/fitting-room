#include "OverlayLocations.h"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <memory>

namespace OS::OverlayLocations {

    namespace {

        using Table = std::unordered_map<std::string, Mask>;

        // ⚠⚠ SWAPPED, NEVER EDITED IN PLACE. See the note above LoadReport in
        // the header: a RaceMenu trip reloads this on the main thread while the
        // overlay scan may be walking it on its own. The same shape
        // OverlayTextures holds its snapshot in, for the same reason.
        std::atomic<std::shared_ptr<const Table>> g_table{ std::make_shared<const Table>() };
        std::atomic<bool>                         g_loaded{ false };

        [[nodiscard]] std::shared_ptr<const Table> Snapshot() {
            return g_table.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::string Lower(std::string_view a_in) {
            std::string out{ a_in };
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        // ⚠ THE WORDS ARE MEASURED, NOT IMAGINED. Every one of these was taken
        // off the reference load order's 2047 installed textures rather than
        // guessed at, and the four groups are as wide as the evidence and no
        // wider. "sock", "stocking" and "pantyhose" are here because ZMD's pack
        // is the only thing on this rig that puts art on the feet by name; the
        // makeup words are here because a pack of eyeshadows names no part of
        // the head anywhere in its paths.
        struct Token {
            std::string_view      word;
            OverlayPlan::Location location;
        };

        constexpr Token kTokens[]{
            { "head", OverlayPlan::Location::kFace },
            { "face", OverlayPlan::Location::kFace },
            { "lip", OverlayPlan::Location::kFace },
            { "brow", OverlayPlan::Location::kFace },
            { "eye", OverlayPlan::Location::kFace },
            { "makeup", OverlayPlan::Location::kFace },
            { "freckle", OverlayPlan::Location::kFace },
            { "warpaint", OverlayPlan::Location::kFace },
            { "blush", OverlayPlan::Location::kFace },
            { "cheek", OverlayPlan::Location::kFace },
            { "mouth", OverlayPlan::Location::kFace },
            { "nose", OverlayPlan::Location::kFace },

            { "body", OverlayPlan::Location::kBody },
            { "torso", OverlayPlan::Location::kBody },
            { "belly", OverlayPlan::Location::kBody },
            { "chest", OverlayPlan::Location::kBody },
            { "back", OverlayPlan::Location::kBody },
            { "breast", OverlayPlan::Location::kBody },
            { "leg", OverlayPlan::Location::kBody },
            { "arm", OverlayPlan::Location::kBody },
            { "spine", OverlayPlan::Location::kBody },
            { "nipple", OverlayPlan::Location::kBody },
            { "pubic", OverlayPlan::Location::kBody },
            { "stomach", OverlayPlan::Location::kBody },

            { "hand", OverlayPlan::Location::kHands },
            { "finger", OverlayPlan::Location::kHands },
            { "nail", OverlayPlan::Location::kHands },
            { "palm", OverlayPlan::Location::kHands },
            { "wrist", OverlayPlan::Location::kHands },

            { "feet", OverlayPlan::Location::kFeet },
            { "foot", OverlayPlan::Location::kFeet },
            { "toe", OverlayPlan::Location::kFeet },
            { "sock", OverlayPlan::Location::kFeet },
            { "stocking", OverlayPlan::Location::kFeet },
            { "pantyhose", OverlayPlan::Location::kFeet },
            { "ankle", OverlayPlan::Location::kFeet },
        };

        [[nodiscard]] Mask TokensIn(std::string_view a_lowered) {
            Mask mask = kNone;
            for (const auto& token : kTokens) {
                if (a_lowered.find(token.word) != std::string_view::npos) {
                    mask |= Bit(token.location);
                }
            }
            return mask;
        }

    }  // namespace

    std::string Key(std::string_view a_overridePath) {
        std::string out = Lower(a_overridePath);
        for (auto& ch : out) {
            if (ch == '/') {
                ch = '\\';
            }
        }
        while (!out.empty() && out.front() == '\\') {
            out.erase(out.begin());
        }
        constexpr std::string_view kPrefix = "textures\\";
        if (out.size() > kPrefix.size() && out.compare(0, kPrefix.size(), kPrefix) == 0) {
            out.erase(0, kPrefix.size());
        }
        return out;
    }

    Mask FromLists(std::string_view a_lists) {
        Mask        mask = kNone;
        std::size_t at   = 0;
        while (at <= a_lists.size()) {
            const auto comma = a_lists.find(',', at);
            const auto end   = comma == std::string_view::npos ? a_lists.size() : comma;
            auto       word  = a_lists.substr(at, end - at);
            while (!word.empty() && word.front() == ' ') {
                word.remove_prefix(1);
            }
            while (!word.empty() && word.back() == ' ') {
                word.remove_suffix(1);
            }
            if (word == "body") {
                mask |= Bit(OverlayPlan::Location::kBody);
            } else if (word == "hands") {
                mask |= Bit(OverlayPlan::Location::kHands);
            } else if (word == "feet") {
                mask |= Bit(OverlayPlan::Location::kFeet);
            } else if (word == "face") {
                mask |= Bit(OverlayPlan::Location::kFace);
            } else if (word == "warp" || word == "warpaint") {
                // ⚠ RECORDED, NOT PLACED. This is the Makeup section's art and
                // it is still not one of the four overlay locations, so it sets
                // a bit outside kAll and every location test below is unmoved.
                mask |= kWarpaint;
            }
            // Anything unknown falls through on purpose. See the note above
            // FromLists in the header.
            if (comma == std::string_view::npos) {
                break;
            }
            at = comma + 1;
        }
        return mask;
    }

    std::string MaskToLists(Mask a_mask) {
        // ⚠ A FIXED ORDER, AND IT IS THE GENERATOR'S. Two scrapes of the same
        // rig have to be the same file or a diff of them stops meaning
        // "RaceMenu changed its mind" and starts meaning "an unordered map
        // iterated differently".
        std::string out;
        const auto  add = [&out](std::string_view a_word) {
            if (!out.empty()) {
                out += ',';
            }
            out += a_word;
        };
        if (Allows(a_mask, OverlayPlan::Location::kBody)) {
            add("body");
        }
        if (Allows(a_mask, OverlayPlan::Location::kHands)) {
            add("hands");
        }
        if (Allows(a_mask, OverlayPlan::Location::kFeet)) {
            add("feet");
        }
        if (Allows(a_mask, OverlayPlan::Location::kFace)) {
            add("face");
        }
        if (IsWarpaint(a_mask)) {
            add("warp");
        }
        return out;
    }

    std::vector<std::string> PathsInEntry(std::string_view a_entry) {
        std::vector<std::string> out;
        const auto               sep = a_entry.find(";;");
        if (sep == std::string_view::npos) {
            // ⚠ NOT READ AS A BARE PATH. A scraped row outranks the shipped
            // table, so a line this reader does not understand must add
            // nothing rather than add a guess.
            return out;
        }
        auto rest = a_entry.substr(sep + 2);
        while (!rest.empty()) {
            const auto bar   = rest.find('|');
            const auto piece = rest.substr(0, bar == std::string_view::npos ? rest.size() : bar);
            if (!piece.empty()) {
                out.emplace_back(piece);
            }
            if (bar == std::string_view::npos) {
                break;
            }
            rest = rest.substr(bar + 1);
        }
        return out;
    }

    Mask FromPathTokens(std::string_view a_overridePath) {
        const auto key = Key(a_overridePath);
        // ⚠ THE FILE NAME FIRST AND ALONE, and only then the folders. A pack
        // that sorts its art into Face and Hands folders names the files
        // anything it likes, and a pack that names its files "52 Body F" sits
        // in a folder called after the pack. Mixing the two into one search
        // gives a Community Overlays face texture the "body" its FOLDER carries
        // and offers it on the torso as well.
        const auto cut  = key.find_last_of('\\');
        const auto name = cut == std::string::npos ? std::string_view{ key }
                                                   : std::string_view{ key }.substr(cut + 1);
        if (const auto mask = TokensIn(name); mask != kNone) {
            return mask;
        }
        if (cut == std::string::npos) {
            return kNone;
        }
        return TokensIn(std::string_view{ key }.substr(0, cut));
    }

    std::string MaskTwinOf(std::string_view a_overridePath) {
        auto key = Key(a_overridePath);
        constexpr std::string_view kExt = ".dds";
        if (key.size() <= kExt.size() ||
            key.compare(key.size() - kExt.size(), kExt.size(), kExt) != 0) {
            return {};
        }
        const auto stem = std::string_view{ key }.substr(0, key.size() - kExt.size());
        // ⚠ THE SPACED FORM IS THE ONE THAT ACTUALLY OCCURS. Every one of the
        // 96 pairs measured on the reference rig separates with a space, as in
        // "face freckles 4 m.dds" beside "face freckles 4.dds". The underscore
        // and the spelt-out forms are here because they cost nothing and a pack
        // that used one would otherwise be missed; none of them is a guess that
        // can hide anything on its own, since the registrations still decide.
        for (const std::string_view suffix : { " m", "_m", " mask", "_mask" }) {
            if (stem.size() <= suffix.size() ||
                stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            std::string twin{ stem.substr(0, stem.size() - suffix.size()) };
            if (twin.empty() || twin.back() == '\\') {
                return {};  // the whole name was the suffix
            }
            twin += kExt;
            return twin;
        }
        return {};
    }

    bool Known(std::string_view a_overridePath) {
        const auto table = Snapshot();
        return table->find(Key(a_overridePath)) != table->end();
    }

    bool MaskTwinIsRedundant(std::string_view a_overridePath, std::string_view a_twinPath) {
        if (a_twinPath.empty()) {
            return false;
        }
        // ⚠ BOTH HALVES MUST BE IN THE TABLE. Unknown means no pack ever said
        // anything about the file, and guessing from the name is exactly what
        // would delete Community Overlays 3's male art.
        //
        // ⚠ ONE SNAPSHOT FOR BOTH LOOKUPS. Taking two would let a reload land
        // between them and answer the pair out of two different tables, which
        // is the one way this could say "redundant" about a file whose twin no
        // longer exists.
        const auto table = Snapshot();
        const auto self  = table->find(Key(a_overridePath));
        const auto twin  = table->find(Key(a_twinPath));
        if (self == table->end() || twin == table->end()) {
            return false;
        }
        // This one is placed on no location, which for a file in the table means
        // its pack registered it as a warpaint. The twin is real overlay art.
        //
        // ⚠ THE TEST IS "NO LOCATION", NOT "NO BITS AT ALL", now that a warpaint
        // registration sets a bit of its own. Before that it could only be
        // spelt as kNone, and leaving it spelt that way would have stopped every
        // mask twin being recognised the moment the bit started being stored.
        return !HasLocation(self->second) && HasLocation(twin->second);
    }

    Mask Resolve(Mask a_registered, std::string_view a_overridePath) {
        // ⚠ THE FALL-THROUGH KEYS ON A LOCATION AND NOT ON A NON-EMPTY MASK. A
        // warpaint-only texture carries kWarpaint and no location, and it must
        // still reach the name rule below exactly as it did when the warpaint
        // word was dropped on the floor. Shep's 264 body tattoos are registered
        // that way and a test of `a_registered != kNone` would have pinned every
        // one of them to nothing.
        if (HasLocation(a_registered)) {
            return LocationBits(a_registered);
        }
        if (const auto mask = FromPathTokens(a_overridePath); mask != kNone) {
            return mask;
        }
        return kAll;
    }

    namespace {

        // The file name at the end of a by_source key: after the last path
        // separator, or after the '!' the generator writes between an archive
        // and the script inside it. Lower cased, since the fixup is typed by
        // hand and the pack's own casing is nobody's business.
        [[nodiscard]] std::string ScriptOf(std::string_view a_source) {
            const auto cut = a_source.find_last_of("\\/!");
            const auto tail = cut == std::string_view::npos ? a_source : a_source.substr(cut + 1);
            return Lower(tail);
        }

        struct Fixup {
            std::string script;     // lower cased file name
            Mask        locations;  // location bits only
        };

        // Read the corrections file. Empty when it is missing or unreadable,
        // and the reason goes into a_note for the load line.
        [[nodiscard]] std::vector<Fixup> ReadFixups(const std::filesystem::path& a_file,
                                                    std::string&                 a_note) {
            std::vector<Fixup> out;
            std::ifstream      in{ a_file };
            if (!in) {
                a_note = "no fixups file at " + a_file.string() + ".";
                return out;
            }
            Json::Value             root;
            Json::String            errors;
            Json::CharReaderBuilder builder;
            if (!Json::parseFromStream(builder, in, &root, &errors)) {
                a_note = a_file.string() + " did not parse (" + errors + "), nothing corrected.";
                return out;
            }
            const auto& list = root["fixups"];
            if (!list.isArray()) {
                a_note = a_file.string() + " has no fixups array, nothing corrected.";
                return out;
            }
            for (const auto& item : list) {
                if (!item.isObject() || !item["script"].isString() ||
                    !item["warpaint_is"].isString()) {
                    continue;
                }
                const auto locations = LocationBits(FromLists(item["warpaint_is"].asString()));
                // A fixup that names no location says nothing and is skipped
                // rather than refused: the rest of the file still applies.
                if (locations == kNone) {
                    continue;
                }
                out.push_back(Fixup{ Lower(item["script"].asString()), locations });
            }
            return out;
        }

        // Read a paints object into a map, the shipped table's shape and the
        // scrape's alike. Empty when the file is missing or will not parse, and
        // the reason goes into a_note.
        [[nodiscard]] bool ReadPaints(const std::filesystem::path& a_file,
                                      Json::Value&                 a_root,
                                      std::string&                 a_note) {
            std::ifstream in{ a_file };
            if (!in) {
                a_note = "missing";
                return false;
            }
            Json::String            errors;
            Json::CharReaderBuilder builder;
            if (!Json::parseFromStream(builder, in, &a_root, &errors)) {
                a_note = "did not parse (" + errors + ")";
                return false;
            }
            if (!a_root["paints"].isObject()) {
                a_note = "has no paints object";
                return false;
            }
            return true;
        }

        LoadReport LoadImpl(const std::filesystem::path& a_file,
                            const std::filesystem::path* a_scraped,
                            const std::filesystem::path* a_fixups) {
            // ⚠ THE LIVE TABLE IS NOT EMPTIED FIRST. It used to be cleared at
            // the top, which was harmless when this ran once before anything
            // read it. Now that a RaceMenu trip reloads under a running game,
            // clearing first would give the scan thread an empty table for the
            // length of the parse and publish a snapshot with the filter off.
            // The new map is built to one side and swapped in at the end; a
            // load that fails swaps in an empty one, which is the same answer
            // the clear used to give.
            Table table;
            g_loaded.store(false, std::memory_order_release);

            LoadReport report;

            std::ifstream in{ a_file };
            if (!in) {
                g_table.store(std::make_shared<const Table>(), std::memory_order_release);
                report.diagnostic = "no table at " + a_file.string() +
                                    ", so the picker falls back to names and then to showing "
                                    "everything.";
                return report;
            }

            Json::Value             root;
            Json::String            errors;
            Json::CharReaderBuilder builder;
            if (!Json::parseFromStream(builder, in, &root, &errors)) {
                // ⚠ A BROKEN TABLE IS NOT A BROKEN PAGE. Refusing to load it
                // leaves the picker on the tier below, which shows MORE art
                // rather than less, so this is a line in the log and nothing
                // else.
                g_table.store(std::make_shared<const Table>(), std::memory_order_release);
                report.diagnostic =
                    a_file.string() + " did not parse (" + errors + "), so the picker falls "
                    "back to names.";
                return report;
            }

            const auto& paints = root["paints"];
            if (!paints.isObject()) {
                g_table.store(std::make_shared<const Table>(), std::memory_order_release);
                report.diagnostic = a_file.string() + " has no paints object.";
                return report;
            }
            for (const auto& name : paints.getMemberNames()) {
                // Our own file, so this should never fire; guarded anyway,
                // because the cost of being wrong about that is the whole
                // process and the cost of being right is one comparison.
                if (!paints[name].isString()) {
                    continue;
                }
                const auto mask = FromLists(paints[name].asString());
                // ⚠ STORED EVEN WHEN THE MASK IS EMPTY. A warpaint-only texture
                // IS in the table, and the difference between "the pack said
                // warpaint" and "no pack ever mentioned this" is worth keeping:
                // only the second is a gap the generator should be re-run for.
                table.insert_or_assign(Key(name), mask);
            }

            // ---- this rig's own RaceMenu, over the top ---------------------
            //
            // ⚠⚠ OUTRANKS, WHICH MEANS REPLACES AND NOT WIDENS. See the note
            // above LoadFrom's three-path form: the shipped table is a snapshot
            // of the reference rig and this is what RaceMenu actually listed
            // here. Where the two disagree the scrape is right by construction,
            // including when it is narrower.
            std::string scrapeNote;
            if (a_scraped) {
                Json::Value scrapedRoot;
                std::string why;
                if (!ReadPaints(*a_scraped, scrapedRoot, why)) {
                    // ⚠ A MISSING SCRAPE IS THE NORMAL STATE and says so
                    // quietly; a broken one is worth a word, because the file is
                    // written by this build and not by a person.
                    scrapeNote = why == "missing"
                                     ? std::string{ "No scrape yet; open RaceMenu once to "
                                                    "read this rig's own lists." }
                                     : a_scraped->filename().string() + " " + why +
                                           ", so only the shipped table was used.";
                } else {
                    const auto& scraped = scrapedRoot["paints"];
                    // ⚠⚠ TYPE-CHECKED, AND THE SHIPPED TABLE ABOVE ALWAYS WAS.
                    // This path was not, and it is the one path of the two whose
                    // FILE DIFFERS PER RIG: the shipped table is ours and
                    // identical for everybody, while this is written from
                    // whatever RaceMenu listed on that machine. jsoncpp throws
                    // rather than returning false here - getMemberNames() on a
                    // non-object and asString() on a non-string are both a
                    // Json::LogicError - and an uncaught throw is a CRT
                    // fail-fast, which no crash logger can see. So the failure
                    // this omission bought was "clicking the overlays section
                    // crashes my game, with no crash log", reported twice in
                    // August 2026. A row we cannot read is skipped and counted;
                    // it is not worth the process.
                    // ⚠ THE CONTAINER IS ALREADY SAFE AND ONLY THE VALUES WERE
                    // NOT. ReadPaints refuses the file unless root["paints"] is
                    // an object, so getMemberNames() here cannot throw and a
                    // guard around it would be dead code. It says nothing about
                    // what the members hold, which is the hole.
                    for (const auto& name : scraped.getMemberNames()) {
                        if (!scraped[name].isString()) {
                            ++report.scrapedSkipped;
                            continue;
                        }
                        const auto mask = FromLists(scraped[name].asString());
                        const auto key  = Key(name);
                        ++report.scraped;
                        const auto at = table.find(key);
                        if (at == table.end()) {
                            ++report.scrapedAdded;
                        } else if (at->second != mask) {
                            ++report.scrapedOverrode;
                        }
                        table.insert_or_assign(key, mask);
                    }
                    scrapeNote = std::to_string(report.scraped) + " row(s) scraped from this "
                                 "rig's RaceMenu: " +
                                 std::to_string(report.scrapedAdded) + " the table had never "
                                 "heard of, " +
                                 std::to_string(report.scrapedOverrode) + " it disagreed with.";
                    if (report.scrapedSkipped != 0) {
                        scrapeNote += " " + std::to_string(report.scrapedSkipped) +
                                      " row(s) were not text and were skipped.";
                    }
                }
            }

            // ---- the corrections, keyed by script -------------------------
            std::string fixupNote;
            if (a_fixups) {
                const auto  fixups   = ReadFixups(*a_fixups, fixupNote);
                const auto& bySource = root["by_source"];
                if (!fixups.empty() && !bySource.isObject()) {
                    // A table from before by_source existed: the fixups have
                    // nothing to key on and the line says so.
                    fixupNote = a_file.string() +
                                " has no by_source object, so the fixups cannot be applied; "
                                "re-run tools/scan-paint-registrations.ps1.";
                } else if (!fixups.empty()) {
                    for (const auto& fixup : fixups) {
                        bool matched = false;
                        for (const auto& source : bySource.getMemberNames()) {
                            if (ScriptOf(source) != fixup.script) {
                                continue;
                            }
                            matched = true;
                            for (const auto& key : bySource[source]) {
                                if (!key.isString()) {
                                    continue;
                                }
                                const auto at = table.find(Key(key.asString()));
                                // ⚠ ONLY A ROW NO PACK PLACED. A fixup widens
                                // a warpaint-only registration and touches
                                // nothing a pack put on a location itself, and
                                // nothing the scrape placed either: by the time
                                // this runs the scrape has already had its say,
                                // so a row RaceMenu itself put on a location is
                                // as untouchable here as one the table did.
                                if (at == table.end() || HasLocation(at->second)) {
                                    continue;
                                }
                                at->second = static_cast<Mask>(at->second | fixup.locations);
                                ++report.corrected;
                            }
                        }
                        if (matched) {
                            ++report.fixups;
                        }
                    }
                    fixupNote = std::to_string(report.corrected) + " corrected by " +
                                std::to_string(report.fixups) + " fixup(s) from " +
                                a_fixups->filename().string() + ".";
                }
            }

            for (const auto& [key, mask] : table) {
                if (HasLocation(mask)) {
                    ++report.placed;
                }
            }
            report.entries = table.size();
            // ⚠ THE SWAP IS THE LAST THING, AND THE FLAG GOES AFTER IT. A
            // reader that sees Loaded() true must be able to find the rows it
            // promises.
            g_table.store(std::make_shared<const Table>(std::move(table)),
                          std::memory_order_release);
            g_loaded.store(true, std::memory_order_release);
            report.loaded  = true;
            report.diagnostic = std::to_string(report.entries) + " textures in the table, " +
                                std::to_string(report.placed) +
                                " of them placed on at least one location.";
            if (!scrapeNote.empty()) {
                report.diagnostic += " " + scrapeNote;
            }
            if (!fixupNote.empty()) {
                report.diagnostic += " " + fixupNote;
            }
            return report;
        }

    }  // namespace

    LoadReport LoadFrom(const std::filesystem::path& a_file) {
        return LoadImpl(a_file, nullptr, nullptr);
    }

    LoadReport LoadFrom(const std::filesystem::path& a_file,
                        const std::filesystem::path& a_fixups) {
        return LoadImpl(a_file, nullptr, &a_fixups);
    }

    LoadReport LoadFrom(const std::filesystem::path& a_file,
                        const std::filesystem::path& a_scraped,
                        const std::filesystem::path& a_fixups) {
        return LoadImpl(a_file, &a_scraped, &a_fixups);
    }

    std::filesystem::path TableFile() {
        return "Data/SKSE/Plugins/FittingRoom/Overlays/overlay-locations.json";
    }

    std::filesystem::path ScrapedFile() {
        return "Data/SKSE/Plugins/FittingRoom/Overlays/overlay-locations-scraped.json";
    }

    std::filesystem::path FixupsFile() {
        return "Data/SKSE/Plugins/FittingRoom/Overlays/overlay-locations-fixups.json";
    }

    LoadReport Load() { return LoadFrom(TableFile(), ScrapedFile(), FixupsFile()); }

    Mask Registered(std::string_view a_overridePath) {
        const auto table = Snapshot();
        const auto at    = table->find(Key(a_overridePath));
        return at == table->end() ? kNone : at->second;
    }

    std::vector<std::string> TablePaths() {
        const auto               table = Snapshot();
        std::vector<std::string> out;
        out.reserve(table->size());
        for (const auto& [path, mask] : *table) {
            out.push_back(path);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> WarpaintPaths() {
        const auto               table = Snapshot();
        std::vector<std::string> out;
        for (const auto& [path, mask] : *table) {
            if (IsWarpaint(mask)) {
                out.push_back(path);
            }
        }
        // ⚠ SORTED, BECAUSE AN UNORDERED MAP IS NOT AN ORDER. Handing the grid
        // the table's iteration order would give the same install a different
        // card layout on different runs, and a picker whose contents move
        // between sessions is one the player cannot learn.
        std::sort(out.begin(), out.end());
        return out;
    }

    Mask For(std::string_view a_overridePath) {
        return Resolve(Registered(a_overridePath), a_overridePath);
    }

    bool Loaded() { return g_loaded.load(std::memory_order_acquire); }

}  // namespace OS::OverlayLocations
