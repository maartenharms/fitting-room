#include "DyePalette.h"

#include "BuildChannel.h"
#include "DyeBlend.h"   // the blend key's spelling, shared with the My Dyes writer

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace OS::DyePalette {

    namespace {
        const auto kDyesDir = BuildChannel::DataPath("Dyes");

        // Mirrors PresetStore's kMaxPresetBytes: a hand-written dye pack is a
        // few KB; anything bigger is not one of ours, and parseFromStream
        // reads the whole file into memory before it parses a single byte,
        // so a huge file dropped in Dyes/ is a std::bad_alloc risk down the
        // same load path a corrupted file already needs guarding on.
        constexpr std::uintmax_t kMaxDyeFileBytes = 256 * 1024;

        std::mutex       g_lock;
        std::vector<Dye> g_dyes;

        // Case-insensitive extension match, the same as PresetStore's Lower,
        // so EbonyPack.JSON is not silently skipped the way an exact-case
        // ".json" compare would skip it. Presets/ and Dyes/ are the two
        // directories this mod invites other mods to drop files into.
        std::string Lower(std::string a_s) {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char a_c) {
                return static_cast<char>(std::tolower(a_c));
            });
            return a_s;
        }

        // Whether the entry WROTE a "rarity" and it came to nothing, which
        // DyeFromJson silently turns into no rarity at all.
        //
        // ⚠ ABSENT IS NOT MALFORMED, and the difference is the whole point of
        // asking. vanilla.json's twelve dyes carry no "rarity" and are meant to
        // be free, so treating absence as a fault would warn every player about
        // the shipped file behaving correctly. A key that is present and came
        // to nothing is a pack author's mistake, and its cost is that every
        // colour it touched is handed out with no requirement.
        //
        // ⚠ AND AN EMPTY STRING IS ONE OF THOSE. It asked only !isString()
        // until 2026-08-02, and "" IS a string, so a rarity that was typed and
        // left blank was the one malformed shape counted nowhere: DyeFromJson
        // stores "", byte for byte what an absent key stores, promotion then
        // skips Covers() because the rarity is empty, and the dye is free and
        // permanent with no line in the log at any layer. Wrote the key and
        // left it blank is the same author error as wrote the key and got the
        // type wrong, and it lands in exactly the same place. This project's
        // own generator can produce it: tools/make_eso_palette.py copies the
        // TSV rarity column through, so a blank cell ships as "rarity": "".
        //
        // isMember, not isNull: operator[] answers with the shared null value
        // both for an absent key and for one written literally null, and
        // "rarity": null is a wrong type someone typed.
        [[nodiscard]] bool RarityDropped(const Json::Value& a_json) {
            // isObject first, because isMember throws on anything else. Callers
            // only reach here for an entry DyeFromJson accepted, so it is
            // always an object; the guard is so that stays true if it is ever
            // called from somewhere else.
            if (!a_json.isObject() || !a_json.isMember("rarity")) {
                return false;
            }
            const auto& rarity = a_json["rarity"];
            return !rarity.isString() || rarity.asString().empty();
        }
    }  // namespace

    bool DyeFromJson(const Json::Value& a_json, std::uint32_t a_defaultCost,
                     Dye& a_out) {
        if (!a_json.isObject()) {
            return false;
        }
        const auto& id   = a_json["id"];
        const auto& name = a_json["name"];
        const auto& hex  = a_json["hex"];
        if (!id.isString() || id.asString().empty()) {
            return false;
        }
        if (!name.isString() || name.asString().empty()) {
            return false;
        }
        if (!hex.isString()) {
            return false;
        }
        // The one strict six-digit rule, shared with hair tints, the outfit
        // dyes array and the scheme store. A half-read colour would be applied
        // to the player's gear.
        const auto colour = JsonCodec::ColourFromHex(hex.asString());
        if (!colour.set) {
            return false;
        }
        // ---- the special-dye keys, VALIDATED BEFORE THE FIRST WRITE ---------
        //
        // ⚠ EVERY ONE OF THESE IS OPTIONAL AND EVERY DEFAULT IS THE OLD
        // BEHAVIOUR. 318 shipped dyes across eso.json and vanilla.json carry none
        // of them, so absence has to mean "exactly as before" rather than "zero".
        // The `a_out.colour = colour` below is what delivers that: `colour` is a
        // fresh DyeChannel, so it resets the mode, the second stop and both
        // finish blocks to their struct defaults and no stale value can be
        // inherited from whatever the caller's buffer arrived carrying.
        //
        // ⚠ AND THEY ARE PARSED UP HERE RATHER THAN BESIDE THEIR WRITES, WHICH IS
        // THE HEADER'S CONTRACT AND NOT A STYLE CHOICE. A refused entry must
        // leave a_out alone, and the writes start on the next line, so a
        // validation failure below them would return false having already
        // replaced the caller's id, name and colour.
        //
        // ⚠ THE SAME SIX-DIGIT VALIDATOR AS hex, DELIBERATELY THE SHARED ONE. A
        // second strictness that drifted from JsonCodec's would mean a colour
        // that round trips as a dye's primary and not as its second stop.
        DyeChannel second{};
        if (a_json.isMember("hex2")) {
            // A colour, so a wrong type is refused exactly as a wrong-typed hex
            // is. That is the split this file already draws: a malformed COLOUR
            // costs the entry, a malformed piece of METADATA costs only itself.
            if (!a_json["hex2"].isString()) {
                return false;
            }
            second = JsonCodec::ColourFromHex(a_json["hex2"].asString());
            if (!second.set) {
                return false;
            }
        }
        DyeChannel sheen{};
        if (a_json.isMember("sheen")) {
            if (!a_json["sheen"].isString()) {
                return false;
            }
            sheen = JsonCodec::ColourFromHex(a_json["sheen"].asString());
            if (!sheen.set) {
                return false;
            }
        }
        // ⚠ AN UNKNOWN OR WRONG-TYPED MODE IS FLAT, NOT A REFUSAL. A dye pack
        // written against a later version should lose its ramp, not its colour,
        // which is the same trade the rarity handling below already makes.
        std::uint8_t mode = 0;
        if (a_json["mode"].isString()) {
            const auto m = a_json["mode"].asString();
            mode         = m == "nacre" ? 1u : (m == "iridescent" ? 2u : 0u);
        }
        // ⚠ AND THE BLEND ON EXACTLY THE MODE'S TERMS. An unknown or wrong-typed
        // spelling DEFERS to the install's setting, which is what all 318
        // shipped dyes already do by naming no key at all - so a typo costs the
        // dye its blend rather than its colour, and a pack written against a
        // later build loses the same one thing.
        //
        // ⚠ PARSED UP HERE WITH THE OTHERS, which is this function's contract:
        // a refused entry must leave a_out alone, and the writes start below.
        std::uint8_t blend = 0;
        if (a_json["blend"].isString()) {
            blend = static_cast<std::uint8_t>(
                OS::DyeBlend::ChoiceFromName(a_json["blend"].asString()));
        }
        a_out.id     = id.asString();
        a_out.name   = name.asString();
        a_out.colour = colour;
        // ⚠ hex2 WITHOUT A MODE STAYS FLAT, AND THE STOPS ARE STILL STORED. Two
        // stops mean nothing without a ramp to run them through, so promoting to
        // nacre on their presence alone would let a typo change how a dye looks.
        // Keeping the bytes means adding the mode later is a one-word edit rather
        // than a re-author.
        a_out.colour.mode      = mode;
        a_out.colour.blend     = blend;
        a_out.colour.secondSet = second.set;
        a_out.colour.r2        = second.r;
        a_out.colour.g2        = second.g;
        a_out.colour.b2        = second.b;
        // ⚠ THE FINISH FIELDS ARE NOT NEW STORAGE, THEY ARE AN UNFINISHED FEATURE.
        // The record, both presence bits and ResolveDyeMaterial have shipped since
        // the Body Studio work; what never existed is a way to AUTHOR them, so
        // colour.palette is empty on every channel in the field. This is the
        // parser that turns a dormant struct into burnished gold.
        //
        // ⚠ TWO PRESENCE BITS SET INDEPENDENTLY, which is the fault DyeMaterial's
        // own header records: under one bit a gloss with no sheen wrote
        // specularColor black and put the shape's highlight out.
        // Metallic flake, the sparkle knob. Clamped like gloss; absent or
        // wrong-typed is none, which is every dye authored before it existed.
        if (a_json["flake"].isIntegral()) {
            a_out.colour.flake =
                static_cast<std::uint8_t>(std::clamp(a_json["flake"].asInt(), 0, 255));
        }
        if (a_json["gloss"].isIntegral()) {
            a_out.colour.palette.glossSet = true;
            // Clamped rather than cast, so a pack author typing 400 gets the
            // ceiling instead of 144.
            a_out.colour.palette.gloss =
                static_cast<std::uint8_t>(std::clamp(a_json["gloss"].asInt(), 0, 255));
        }
        if (sheen.set) {
            a_out.colour.palette.sheenSet = true;
            a_out.colour.palette.sheenR   = sheen.r;
            a_out.colour.palette.sheenG   = sheen.g;
            a_out.colour.palette.sheenB   = sheen.b;
        }
        a_out.cost   = a_json["cost"].isUInt()
                           ? static_cast<std::uint32_t>(a_json["cost"].asUInt())
                           : a_defaultCost;
        // Optional and non-fatal. A malformed rarity costs the dye its tier,
        // which the rules resolve as free; dropping the whole entry would cost
        // the player the colour, and that is the worse trade.
        //
        // ⚠ ASSIGNED UNCONDITIONALLY, including the empty case, because a_out
        // is the CALLER's buffer and this function has no idea what is in it.
        // An "only if present" write would hand an untiered dye whatever
        // rarity the buffer arrived carrying, and inheriting Common is
        // inheriting free.
        //
        // ⚠ The comment here used to justify that with "a_out is one buffer
        // reused for every entry in a file", and DyesFromJson does not do
        // that: it declares a fresh Dye inside its loop. The contract is what
        // makes the unconditional write necessary, not the one caller's
        // current shape, and the test suite reuses a buffer deliberately to
        // hold that contract.
        //
        // ⚠ THE FALSE PATH LEAVES EVERY FIELD STALE, id included, and nothing
        // guards it. The header says so ("leaves a_out alone"), and DyesFromJson
        // is safe because it only reads a_out after a true, but a caller that
        // reuses a buffer and ignores the return reads the PREVIOUS dye's id
        // with no sign anything went wrong. Left as documented contract rather
        // than clearing on failure, since clearing would silently change what
        // "leaves a_out alone" means for anyone relying on it.
        //
        // ⚠ Free is where a dropped rarity lands, so DyesFromJson counts it.
        // Non-fatal must not mean unmentioned.
        a_out.rarity = a_json["rarity"].isString()
                           ? a_json["rarity"].asString()
                           : std::string{};
        return true;
    }

    EntryTally DyesFromJson(const Json::Value& a_root, std::uint32_t a_defaultCost,
                            std::vector<Dye>& a_out) {
        // jsoncpp's operator[] only tolerates object or null on the left and
        // throws Json::LogicError for anything else (an array, a string, a
        // number). A parseable file whose root is not an object must be
        // dropped whole, the same as an unparseable one, never crash reaching
        // for ["dyes"].
        EntryTally tally;
        if (!a_root.isObject()) {
            return tally;
        }
        const auto& dyes = a_root["dyes"];
        if (!dyes.isArray()) {
            return tally;
        }
        for (const auto& entry : dyes) {
            Dye d;
            if (DyeFromJson(entry, a_defaultCost, d)) {
                // ⚠ Only for an entry that SURVIVED. A rejected dye is not in
                // the palette, so promotion never sees it and it cannot be
                // freed; counting it would put a warning in the log about a
                // colour the player does not have.
                if (RarityDropped(entry)) {
                    ++tally.raritiesDropped;
                }
                a_out.push_back(std::move(d));
            } else {
                ++tally.rejected;
            }
        }
        return tally;
    }

    bool MergeDye(std::vector<Dye>& a_into, const Dye& a_dye) {
        const auto clash = std::any_of(
            a_into.begin(), a_into.end(),
            [&](const Dye& d) { return d.id == a_dye.id; });
        if (clash) {
            return false;
        }
        a_into.push_back(a_dye);
        return true;
    }

    LoadReport Load(std::uint32_t a_defaultCost) {
        // Files are read in SORTED filename order, so "the first one wins" on
        // an id collision is a deterministic statement rather than whatever the
        // filesystem happened to hand back.
        std::vector<std::filesystem::path> files;
        std::error_code                    ec;
        if (std::filesystem::exists(kDyesDir, ec)) {
            // An explicit loop with increment(ec), not a range-based for.
            // directory_iterator's range-based for always advances through
            // the throwing operator++, even when the iterator itself was
            // constructed with an error_code; increment(ec) is the only way
            // to keep a mid-scan failure from throwing.
            std::filesystem::directory_iterator       it(kDyesDir, ec);
            const std::filesystem::directory_iterator end;
            while (!ec && it != end) {
                // A per-entry error_code, separate from the one guarding the
                // scan itself: one entry this process cannot stat (a broken
                // symlink, a file another process has locked) is skipped on
                // its own, never mistaken for the whole directory ending.
                std::error_code entryEc;
                const auto&     entry = *it;
                if (entry.is_regular_file(entryEc) &&
                    Lower(entry.path().extension().string()) == ".json") {
                    files.push_back(entry.path());
                }
                it.increment(ec);
            }
        }
        std::sort(files.begin(), files.end());

        LoadReport       report;
        std::vector<Dye> loaded;
        for (const auto& path : files) {
            ++report.filesScanned;
            const auto file = path.filename().string();
            // Everything from here down is one file's worth of work, wrapped
            // whole: jsoncpp's own nesting guard (default stackLimit 1000
            // recursive readValue() calls) THROWS Json::RuntimeError rather
            // than returning false once a file nests a few hundred levels
            // past that, e.g. "Exceeded stackLimit in readValue()." Without
            // this, that exception reaches Load's caller (kDataLoaded) with
            // no handler anywhere above it, and the game crashes on a pack
            // file that is a few KB of nothing but brackets. One bad file
            // must never take the rest of the directory, or the game, down
            // with it, the same rule every other guard in this function
            // already follows.
            try {
                std::error_code sizeEc;
                if (const auto size = std::filesystem::file_size(path, sizeEc);
                    !sizeEc && size > kMaxDyeFileBytes) {
                    report.fileSkips.push_back(
                        "'" + file + "': " + std::to_string(size) + " bytes (cap " +
                        std::to_string(kMaxDyeFileBytes) + ")");
                    continue;
                }

                std::ifstream in(path);
                if (!in) {
                    report.fileSkips.push_back("'" + file + "': could not open");
                    continue;
                }

                Json::Value             root;
                Json::CharReaderBuilder rb;
                std::string             errs;
                if (!Json::parseFromStream(rb, in, &root, &errs)) {
                    report.fileSkips.push_back(
                        "'" + file + "': not valid JSON (" +
                        (errs.empty() ? "unknown error" : errs) + ")");
                    continue;  // an unparseable file is skipped whole, never half applied
                }

                std::vector<Dye> fromFile;
                const auto       tally = DyesFromJson(root, a_defaultCost, fromFile);
                report.entriesRejected += tally.rejected;
                report.raritiesDropped += tally.raritiesDropped;
                // The My Dyes mark (spec 2026-08-09): origin is a fact about
                // the FILE, so it is stamped here where the file is known
                // rather than inside the parser, which never sees one.
                const bool isCustom = Lower(file) == "custom.json";
                for (auto& d : fromFile) {
                    d.custom = isCustom;
                    if (MergeDye(loaded, d)) {
                        ++report.dyesAccepted;
                    } else {
                        ++report.idCollisions;
                    }
                }
            } catch (const std::exception& e) {
                report.fileSkips.push_back("'" + file + "': " + e.what());
            }
        }

        std::scoped_lock l(g_lock);
        g_dyes = std::move(loaded);
        return report;
    }

    std::vector<Dye> Snapshot() {
        std::scoped_lock l(g_lock);
        return g_dyes;
    }

    std::size_t Count() {
        std::scoped_lock l(g_lock);
        return g_dyes.size();
    }

    std::optional<Dye> FindIn(const std::vector<Dye>& a_dyes, std::string_view a_id) {
        const auto it = std::find_if(
            a_dyes.begin(), a_dyes.end(),
            [&](const Dye& d) { return d.id == a_id; });
        return it == a_dyes.end() ? std::nullopt : std::optional<Dye>{ *it };
    }

    std::optional<Dye> Find(std::string_view a_id) {
        std::scoped_lock l(g_lock);
        return FindIn(g_dyes, a_id);
    }

    AppliedMatch FindAppliedIn(const std::vector<Dye>& a_dyes, const DyeChannel& a_channel) {
        AppliedMatch out{};
        // ⚠ AN UNSET CHANNEL MATCHES NOTHING, and the guard is inside
        // ChannelCarriesPaletteDye rather than here, so a caller reaching that
        // function directly gets the same answer. Left explicit anyway because
        // walking 458 entries to conclude "the player has not dyed this" is a
        // walk with a known result.
        if (!a_channel.set) {
            return out;
        }
        for (const auto& d : a_dyes) {
            if (!ChannelCarriesPaletteDye(a_channel, d.colour)) {
                continue;
            }
            ++out.matches;
            // ⚠ KEEP WALKING PAST THE FIRST HIT. Returning here would make the
            // count meaningless and would silently break the tie by file order,
            // which is exactly what the header refuses to do. 458 entries of a
            // trivial comparison, once per drawn tooltip, is not worth trading
            // a correct count for.
            if (out.matches == 1) {
                out.dye = d;
            } else {
                out.dye.reset();
            }
        }
        return out;
    }

    HueKey HueKeyOf(const DyeChannel& a_colour) {
        HueKey key{};
        const int r = a_colour.r;
        const int g = a_colour.g;
        const int b = a_colour.b;

        const int max = std::max({ r, g, b });
        const int min = std::min({ r, g, b });
        key.value     = static_cast<std::uint8_t>(max);

        const int chroma = max - min;
        // ⚠ EXACTLY ZERO, NOT A TOLERANCE. These are bytes an author typed as
        // six hex digits, so a grey IS three equal bytes and there is no
        // measurement error to absorb. A tolerance here would drag genuinely
        // desaturated colours, the dusty roses and the sage greens, out of the
        // wheel and into the grey run where a player would not look for them.
        if (chroma == 0) {
            key.neutral    = true;
            key.hue        = 0;
            key.saturation = 0;
            return key;
        }

        // Standard HSV saturation, scaled to a byte. max is nonzero here: it
        // cannot be, because chroma is above zero and max is the larger side.
        key.saturation = static_cast<std::uint8_t>((chroma * 255) / max);

        // Sixths of the wheel, 0..255 rather than 0..360, so the whole key is
        // integers and two runs cannot reorder themselves between frames the
        // way a float compare can. 43 is 256/6 rounded, which is close enough
        // for an ordering that only has to look sorted.
        int sixth = 0;
        if (max == r) {
            sixth = ((g - b) * 43) / chroma;
            if (sixth < 0) {
                sixth += 256;  // the wedge below red wraps to the top of the wheel
            }
        } else if (max == g) {
            sixth = 85 + ((b - r) * 43) / chroma;
        } else {
            sixth = 171 + ((r - g) * 43) / chroma;
        }
        key.hue = static_cast<std::uint8_t>(sixth & 0xFF);
        return key;
    }

    bool HueKeyLess(const HueKey& a_l, const HueKey& a_r) {
        if (a_l.neutral != a_r.neutral) {
            return !a_l.neutral;  // colours first, the grey run behind them
        }
        if (!a_l.neutral) {
            if (a_l.hue != a_r.hue) {
                return a_l.hue < a_r.hue;
            }
            if (a_l.saturation != a_r.saturation) {
                return a_l.saturation > a_r.saturation;  // strong before washed out
            }
        }
        return a_l.value > a_r.value;  // light before dark, and the greys' only rule
    }

}  // namespace OS::DyePalette
