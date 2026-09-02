#include "SetDetector.h"

#include "SlotMask.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>

namespace OS::SetDetector {

    namespace {
        std::string Lower(std::string_view a_s) {
            std::string out(a_s);
            std::ranges::transform(out, out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        // Split on non-alphanumerics; drop anything inside [] or () (author
        // tags like "[VANILLA]", "(1)"). Input is lowered by the caller.
        std::vector<std::string> Tokenize(std::string_view a_s) {
            std::string flat;
            int         depth = 0;
            for (char c : a_s) {
                if (c == '[' || c == '(') { ++depth; continue; }
                if (c == ']' || c == ')') { if (depth > 0) --depth; continue; }
                if (depth > 0) { continue; }
                flat += c;
            }
            std::vector<std::string> toks;
            std::string              cur;
            for (char c : flat) {
                if (std::isalnum(static_cast<unsigned char>(c))) {
                    cur += c;
                } else if (!cur.empty()) {
                    toks.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) { toks.push_back(cur); }
            return toks;
        }

        bool AllDigits(const std::string& a_t, std::size_t a_from) {
            if (a_from >= a_t.size()) { return false; }
            for (std::size_t i = a_from; i < a_t.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(a_t[i]))) { return false; }
            }
            return true;
        }

        // "12", "v2", "mk3" - a numeric/version token, never a set identity.
        bool IsNumberToken(const std::string& a_t) {
            if (a_t.empty()) { return false; }
            if (AllDigits(a_t, 0)) { return true; }
            if (a_t[0] == 'v' && AllDigits(a_t, 1)) { return true; }
            if (a_t.size() > 2 && a_t[0] == 'm' && a_t[1] == 'k' && AllDigits(a_t, 2)) {
                return true;
            }
            return false;
        }

        // Words that name a SLOT, not a set. Stripped so pieces of one set
        // share a stem across slots.
        const std::set<std::string> kSlotNouns = {
            // Head
            "hood", "helm", "helmet", "mask", "circlet", "hat", "cap", "crown",
            "cowl", "headgear", "headwear", "hair", "wig",
            // Feet / legs
            "boots", "boot", "shoes", "shoe", "greaves", "greave", "sabatons",
            "sabaton", "slippers", "heels", "sandals", "socks", "stockings",
            "feet", "foot", "legs", "leg", "calves", "calf", "anklet", "anklets",
            "gaiters",
            // Hands / arms
            "gloves", "glove", "gauntlets", "gauntlet", "bracers", "bracer",
            "cuffs", "cuff", "vambraces", "vambrace", "arms", "arm", "hands",
            "hand", "sleeves", "sleeve", "wristband", "wristbands", "wristguard",
            "armlet", "armlets", "bracelet", "bracelets", "spaulders", "pauldron",
            "pauldrons",
            // Body / torso
            "cuirass", "armor", "armour", "robe", "robes", "dress", "gown",
            "tunic", "shirt", "jerkin", "coat", "chestpiece", "breastplate",
            "vest", "top", "body", "bodysuit", "torso", "chest", "bra", "corset",
            "bikini", "harness", "straps", "strap", "wrap", "wraps",
            // Waist / lower
            "outfit", "clothes", "clothing", "pants", "trousers", "breeches",
            "skirt", "skirts", "leggings", "bottom", "bottoms", "briefs", "thong",
            "panties", "loincloth", "kilt", "waist", "belt", "sash", "faulds",
            "tassets", "garter", "garters", "pelvis", "groin", "hips", "thighs",
            "underwear", "undies",
            // Shoulders / back / cloak / jewelry / neck
            "shoulder", "shoulders", "back", "cape", "cloak", "ring", "amulet",
            "necklace", "pendant", "torc", "neck", "collar", "choker", "earring",
            "earrings", "shield",
        };

        // Words that name a VARIANT (a color/weight/index) - stripped so color
        // variants of one set collapse together. Material words (iron, elven…)
        // are deliberately NOT here: within a plugin they bind a set.
        const std::set<std::string> kVariantWords = {
            "red", "blue", "green", "black", "white", "brown", "grey", "gray",
            "gold", "golden", "silver", "purple", "pink", "crimson", "emerald",
            "ruby", "sapphire", "teal", "cyan", "orange", "yellow", "azure",
            "violet", "ivory", "ebon", "dark", "light", "heavy", "lite",
        };

        bool IsNoise(const std::string& a_t) {
            return kSlotNouns.contains(a_t) || kVariantWords.contains(a_t) ||
                   IsNumberToken(a_t);
        }

        // Weapon class words are removed only from the END of a weapon name.
        // That distinction preserves real set identities such as "Blades"
        // while still mapping "Blades Sword" back to the "blades" armor set.
        const std::set<std::string> kWeaponTailNouns = {
            "weapon", "weapons", "sword", "swords", "greatsword", "greatswords",
            "dagger", "daggers", "blade", "axe", "axes", "battleaxe", "battleaxes",
            "waraxe", "waraxes", "mace", "maces", "warhammer", "warhammers",
            "hammer", "hammers", "bow", "bows", "longbow", "longbows", "crossbow",
            "crossbows", "staff", "staves", "arrow", "arrows", "bolt", "bolts",
            "quiver", "quivers",
        };

        std::string WeaponNameStem(std::string_view a_name) {
            std::vector<std::string> kept;
            for (const auto& token : Tokenize(Lower(a_name))) {
                if (token.size() > 1 && !IsNoise(token)) {
                    kept.push_back(token);
                }
            }
            bool removedClass = false;
            while (!kept.empty() && kWeaponTailNouns.contains(kept.back())) {
                kept.pop_back();
                removedClass = true;
            }
            if (removedClass && !kept.empty()) {
                const bool compoundModifier =
                    kept.back() == "war" || kept.back() == "battle" || kept.back() == "great";
                if (compoundModifier) {
                    kept.pop_back();
                }
            }
            std::string out;
            for (const auto& token : kept) {
                if (!out.empty()) {
                    out += ' ';
                }
                out += token;
            }
            return out;
        }

        // Major-slot category masks (editor slots -> bits). Head folds
        // head/hair-helmet/circlet; hands folds forearms; feet folds calves.
        const std::uint32_t kHeadMask = MaskForEditorSlot(30) | MaskForEditorSlot(31) |
                                        MaskForEditorSlot(42);
        const std::uint32_t kBodyCat  = MaskForEditorSlot(32);
        const std::uint32_t kHandsCat = MaskForEditorSlot(33) | MaskForEditorSlot(34);
        const std::uint32_t kFeetCat  = MaskForEditorSlot(37) | MaskForEditorSlot(38);

        // How many of {head, body, hands, feet} a slot mask touches (0-4).
        int MajorCoverage(std::uint32_t a_mask) {
            int n = 0;
            if (a_mask & kHeadMask) { ++n; }
            if (a_mask & kBodyCat)  { ++n; }
            if (a_mask & kHandsCat) { ++n; }
            if (a_mask & kFeetCat)  { ++n; }
            return n;
        }

        // Body-anchored and covering >=2 majors - the bar to be a standalone set.
        // A cluster that fails this is an orphan candidate for prefix-merging.
        bool QualifiesAsSet(std::uint32_t a_mask) {
            return (a_mask & kBodyCat) && MajorCoverage(a_mask) >= 2;
        }

        std::uint32_t UnionMaskOf(const std::vector<const DetectStyle*>& a_pieces) {
            std::uint32_t m = 0;
            for (const auto* p : a_pieces) { m |= p->slotMask; }
            return m;
        }

        // a_prefix is a WHOLE-WORD prefix of a_full ("abyss" of "abyss arms",
        // not "abys" of "abyss"). Used to attach an orphaned slot piece
        // ("gladiator pelvis") to the set it extends ("gladiator").
        bool IsWordPrefix(const std::string& a_prefix, const std::string& a_full) {
            if (a_prefix.size() >= a_full.size()) {
                return false;
            }
            return a_full.compare(0, a_prefix.size(), a_prefix) == 0 &&
                   a_full[a_prefix.size()] == ' ';
        }

        // How many variant tokens a name carries - the base piece (fewest) is
        // the representative when a slot has several candidates.
        int VariantScore(std::string_view a_name) {
            int n = 0;
            for (const auto& t : Tokenize(Lower(a_name))) {
                if (kVariantWords.contains(t) || IsNumberToken(t)) { ++n; }
            }
            return n;
        }

        // Title-case a lowered stem for display ("college of winterhold").
        std::string TitleCase(std::string_view a_stem) {
            std::string out(a_stem);
            bool        start = true;
            for (char& c : out) {
                if (c == ' ') { start = true; continue; }
                if (start) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                start = false;
            }
            return out;
        }

        // One chosen piece per slot -> a set, or nullopt if it does not clear
        // the body-anchored >=2-majors bar.
        //
        // ⚠ THE BASE ROW AND EVERY VARIANT ROW COME THROUGH HERE. A variant
        // row is the same set with one slot's piece swapped, so it has to be
        // measured the same way: its own coverage, its own fit verdict. Two
        // builders would drift on the first change to either.
        std::optional<DetectedSet> MakeSet(
            const std::map<std::uint32_t, const DetectStyle*>& a_rep,
            const std::map<std::uint32_t, int>& a_alts, std::string a_name,
            std::string_view a_cleanPlugin) {
            std::uint32_t unionMask = 0;
            for (const auto& [bit, p] : a_rep) { unionMask |= p->slotMask; }
            if (!(unionMask & kBodyCat) || MajorCoverage(unionMask) < 2) {
                return std::nullopt;  // not body-anchored / too thin
            }
            DetectedSet set;
            set.source    = std::string(a_cleanPlugin);
            set.coverage  = MajorCoverage(unionMask);
            set.fullyFits = true;
            for (const auto& [bit, p] : a_rep) {
                set.outfit.SetStyle(bit, p->key);
                if (!p->fits) { set.fullyFits = false; }
                const auto it = a_alts.find(bit);
                if (it != a_alts.end() && it->second > 0) {
                    set.variants.emplace_back(bit, it->second);
                }
            }
            set.name        = std::move(a_name);
            set.outfit.name = set.name;
            return set;
        }

        // Tokens WITHOUT the bracket-stripping Tokenize does. The stem drops
        // "[OBI]" and "(no sleeves)" because they never identify a SET, but
        // what tells two variants of one set apart is routinely nothing else.
        std::vector<std::string> TokenizeKeepingBrackets(std::string_view a_s) {
            std::vector<std::string> toks;
            std::string              cur;
            for (char c : a_s) {
                if (std::isalnum(static_cast<unsigned char>(c))) {
                    cur += c;
                } else if (!cur.empty()) {
                    toks.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) { toks.push_back(cur); }
            return toks;
        }

        // What makes an alternate piece different from the set's base piece:
        // the base's words removed from the alternate's, in the alternate's own
        // order. "Callisto Armor" against "Callisto Armor (no sleeves)" gives
        // "No Sleeves". Empty when the two names are the same, which is the
        // caller's cue to fall back to the whole name.
        std::string VariantLabel(std::string_view a_base, std::string_view a_alt) {
            std::vector<std::string> pool = TokenizeKeepingBrackets(Lower(a_base));
            std::string              out;
            for (const auto& t : TokenizeKeepingBrackets(Lower(a_alt))) {
                // ⚠ A MULTISET DIFFERENCE, NOT A SET ONE. A name that repeats a
                // word ("Steel Steel Plate") would otherwise lose both copies
                // and the label would claim a difference that is not there.
                const auto it = std::ranges::find(pool, t);
                if (it != pool.end()) {
                    pool.erase(it);
                    continue;
                }
                if (!out.empty()) { out += ' '; }
                out += t;
            }
            return TitleCase(out);
        }

        // One cluster of same-plugin, same-stem, same-type pieces -> a set, or
        // nullopt if it does not clear the body-anchored >=2-majors bar. Picks
        // one representative per slot (the base of any variant set) and counts
        // the alternates. An empty display name falls back to the plugin name.
        //
        // a_altPieces, if given, receives the alternates that lost each slot,
        // which is what the variant rows are built from.
        std::optional<DetectedSet> AssembleSet(
            const std::vector<const DetectStyle*>& a_pieces,
            std::string_view a_displayName, std::string_view a_cleanPlugin,
            std::map<std::uint32_t, std::vector<const DetectStyle*>>* a_altPieces = nullptr,
            std::map<std::uint32_t, const DetectStyle*>* a_reps = nullptr) {
            std::map<std::uint32_t, const DetectStyle*> rep;
            std::map<std::uint32_t, int>                alts;
            for (const auto* p : a_pieces) {
                // ⚠⚠ A GENERATED SET NEVER OWNS SLOT 52. See kBitGenitals in
                // SlotMask.h: it is the community's schlong slot, and a set
                // that owns it strips TNG's cover the moment it is worn. This
                // pass INVENTS sets from name clusters, so a mod shipping a
                // slot-52 piece under a matching stem would hand every wearer
                // of that set a change nobody asked for. The player's own
                // hand-built outfits are untouched; only what we generate is.
                if (p->primaryBit == kBitGenitals) {
                    continue;
                }
                auto it = rep.find(p->primaryBit);
                if (it == rep.end()) {
                    rep[p->primaryBit]  = p;
                    alts[p->primaryBit] = 0;
                } else {
                    ++alts[p->primaryBit];
                    const auto* cur = it->second;
                    // Prefer a FITTING piece, then the base (fewest variant
                    // tokens), then alphabetical - a fitting variant renders where
                    // an unfit one would not.
                    const int  pc = VariantScore(p->name), cc = VariantScore(cur->name);
                    const bool better = p->fits != cur->fits
                                            ? p->fits
                                            : (pc != cc ? pc < cc : p->name < cur->name);
                    if (better) {
                        it->second = p;
                    }
                }
            }
            if (a_reps) {
                *a_reps = rep;
            }
            if (a_altPieces) {
                // Everything that is not the representative of its slot. Built
                // after the loop rather than inside it, because which piece
                // represents a slot is not settled until the last one is seen.
                for (const auto* p : a_pieces) {
                    const auto it = rep.find(p->primaryBit);
                    if (it != rep.end() && it->second != p) {
                        (*a_altPieces)[p->primaryBit].push_back(p);
                    }
                }
            }

            const std::string stem = NameStem(a_displayName);
            return MakeSet(rep, alts,
                           stem.empty() ? std::string(a_cleanPlugin) : TitleCase(stem),
                           a_cleanPlugin);
        }

        // The body-slot key that identifies a set for dedup.
        StyleRefKey BodyKeyOf(const DetectedSet& a_set) {
            const auto& e = a_set.outfit.EntryFor(kBitBody);
            return e.kind == SlotEntry::Kind::kStyle ? e.style : StyleRefKey{};
        }

        // The browser's order: grouped by plugin, fitting sets first, then the
        // fullest, then by name. Named rather than written twice because
        // CompleteFromOutfits changes coverage after Detect has already sorted
        // on it, and two copies of a comparator drift.
        bool SetOrder(const DetectedSet& a, const DetectedSet& b) {
            if (a.source != b.source) { return a.source < b.source; }
            if (a.fullyFits != b.fullyFits) { return a.fullyFits; }  // fitting sets first
            if (a.coverage != b.coverage) { return a.coverage > b.coverage; }
            return a.name < b.name;
        }

        // StyleRefKey is comparable but not ordered (it defaults operator== and
        // nothing else), and this is the one place that wants it in a map.
        // Ordered on the form id first because that is what actually
        // discriminates; the plugin name breaks the ties between load orders.
        struct KeyLess {
            bool operator()(const StyleRefKey& a, const StyleRefKey& b) const {
                if (a.localFormID != b.localFormID) {
                    return a.localFormID < b.localFormID;
                }
                return a.modName < b.modName;
            }
        };
        using KeyIndex = std::map<StyleRefKey, const DetectStyle*, KeyLess>;

        // The union of every slot the set's chosen pieces occupy.
        std::uint32_t SetUnionMask(const DetectedSet& a_set, const KeyIndex& a_byKey) {
            std::uint32_t m = 0;
            a_set.outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
                if (const auto it = a_byKey.find(a_key); it != a_byKey.end()) {
                    m |= it->second->slotMask;
                }
            });
            return m;
        }
    }

    std::string NameStem(std::string_view a_displayName) {
        std::string out;
        for (const auto& t : Tokenize(Lower(a_displayName))) {
            // Drop slot/variant/number words and single-char junk (the "s" that
            // "Obi's" tokenizes to, stray initials) - they never identify a set.
            if (t.size() <= 1 || IsNoise(t)) { continue; }
            if (!out.empty()) { out += ' '; }
            out += t;
        }
        return out;
    }

    std::string CleanPluginName(std::string_view a_source) {
        std::string base(a_source);
        if (const auto dot = base.find_last_of('.'); dot != std::string::npos) {
            const std::string ext = Lower(base.substr(dot + 1));
            if (ext == "esp" || ext == "esm" || ext == "esl") {
                base = base.substr(0, dot);
            }
        }
        // Split on separators and at camelCase boundaries (lower->Upper).
        std::vector<std::string> toks;
        std::string              cur;
        const auto               flush = [&] {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        };
        for (char c : base) {
            if (c == '_' || c == '-' || c == ' ') { flush(); continue; }
            if (std::isupper(static_cast<unsigned char>(c)) && !cur.empty() &&
                std::islower(static_cast<unsigned char>(cur.back()))) {
                flush();
            }
            cur += c;
        }
        flush();

        std::string out;
        for (auto& t : toks) {
            if (IsNumberToken(Lower(t))) { continue; }  // drop v2/version tokens
            std::string tc = Lower(t);
            tc[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tc[0])));
            if (!out.empty()) { out += ' '; }
            out += tc;
        }
        return out.empty() ? std::string(a_source) : out;
    }

    std::vector<DetectedSet> Detect(const std::vector<DetectStyle>& a_styles,
                                    const Options& a_opts, Stats* a_stats) {
        // One cluster of same-plugin, same-stem ("" = residual), same-type pieces.
        struct Cluster {
            std::vector<const DetectStyle*> pieces;
            std::string                     displayName;  // first piece's, for naming
            std::string                     source;       // raw plugin filename
            std::string                     stem;         // "" for residual pieces
            std::uint8_t                    armorType{ 0 };
        };
        std::map<std::string, Cluster> clusters;
        std::map<std::string, int>     pluginPieces;  // total pieces per raw source
        std::map<std::string, int>     typeBits;      // "source\x1fstem" -> OR of 1<<type

        for (const auto& s : a_styles) {
            ++pluginPieces[s.source];
            std::string stem = NameStem(s.name);
            if (stem.empty()) { stem = NameStem(s.edid); }  // best-effort EDID fallback
            const std::string key = s.source + '\x1f' + stem + '\x1f' +
                                    std::to_string(s.armorType);
            auto& c = clusters[key];
            if (c.pieces.empty()) {
                c.displayName = s.name;
                c.source      = s.source;
                c.stem        = stem;
                c.armorType   = s.armorType;
            }
            c.pieces.push_back(&s);
            typeBits[s.source + '\x1f' + stem] |= (1 << s.armorType);
        }

        // Prefix-merge: attach an orphaned slot piece to the set it belongs to.
        // A cluster that can't stand alone (not body-anchored / <2 majors) but
        // whose stem WORD-extends a same-plugin, same-type cluster's stem - e.g.
        // "abyss arms" under "abyss", "gladiator pelvis" under "gladiator" - is a
        // piece whose slot word we didn't recognize. Merge it in so the set is
        // complete, WITHOUT growing the dictionary. A real standalone set is
        // never merged (it qualifies), so distinct sets stay distinct.
        {
            std::vector<std::string> keys;
            keys.reserve(clusters.size());
            for (auto& [k, c] : clusters) {
                keys.push_back(k);
            }
            // Longest stems first, so a piece attaches to its MOST specific parent.
            std::ranges::sort(keys, [&clusters](const std::string& a, const std::string& b) {
                return clusters.at(a).stem.size() > clusters.at(b).stem.size();
            });
            for (const auto& ak : keys) {
                auto ait = clusters.find(ak);
                if (ait == clusters.end()) {
                    continue;
                }
                const Cluster& A = ait->second;
                if (A.stem.empty() || QualifiesAsSet(UnionMaskOf(A.pieces))) {
                    continue;  // residual, or a real standalone set - not an orphan
                }
                // ⚠⚠ THE ARMOUR TYPE IS NOT ALLOWED TO SPLIT A SET, and it was
                // doing exactly that. A circlet is authored as CLOTHING by
                // convention even when the armour it belongs to is heavy:
                // Callisto ships `_Fuse00_CircletOutfitCallisto` (slot 42,
                // clothing) beside a heavy body, gauntlets and boots, and the
                // cluster key carries the type, so the circlet became a
                // cluster of its own, failed to qualify, and the set was shown
                // with no head piece at all (field 2026-08-20, the user's
                // Callisto card). The same convention costs modded sets their
                // circlet across the load order.
                //
                // ⚠ AN EXACT STEM COUNTS AS WELL AS AN EXTENSION, and it has
                // to: "Callisto Circlet" stems to "callisto", the same as the
                // body, so there is nothing to extend. Only a cluster that
                // CANNOT stand alone reaches here (the guard above), so a real
                // clothing set with the same stem still qualifies and is never
                // swallowed.
                //
                // ⚠ SAME TYPE WINS. With a light and a heavy set under one
                // stem, an orphan of that type belongs to its own; only an
                // orphan of a third type has to cross.
                Cluster*    best     = nullptr;
                bool        bestType = false;
                for (auto& [bk, B] : clusters) {
                    if (&B == &A || B.stem.empty() || B.source != A.source) {
                        continue;
                    }
                    if (!IsWordPrefix(B.stem, A.stem) && B.stem != A.stem) {
                        continue;
                    }
                    const bool sameType = B.armorType == A.armorType;
                    if (!best || (sameType && !bestType) ||
                        (sameType == bestType && B.stem.size() > best->stem.size())) {
                        best     = &B;
                        bestType = sameType;
                    }
                }
                if (best) {
                    best->pieces.insert(best->pieces.end(), A.pieces.begin(),
                                        A.pieces.end());
                    clusters.erase(ait);
                }
            }
        }

        // ⚠ THE TYPE SUFFIX IS ABOUT THE SETS THAT SURVIVE, NOT THE PIECES.
        // It exists to keep a light set and a heavy set of one name apart, and
        // it was counting a stray clothing circlet as a whole second set: the
        // Callisto card read "Callisto (Heavy)" when nothing called Callisto
        // was anything else. Recounting after the merges asks the question the
        // suffix is actually for.
        typeBits.clear();
        for (const auto& [key, c] : clusters) {
            if (!c.stem.empty()) {
                typeBits[c.source + '\x1f' + c.stem] |= (1 << c.armorType);
            }
        }

        // Assemble each cluster. Residual clusters (empty stem) emit only when
        // the whole plugin is small enough to be "one outfit" (anti-Frankenstein).
        std::vector<DetectedSet>  out;
        std::vector<std::string>  outStemKey;  // parallel: source\x1f stem
        std::vector<std::uint8_t> outType;     // parallel: armorType
        int                       residualClusters = 0, residualDropped = 0;
        int                       variantRows = 0, variantsDropped = 0;
        for (auto& [key, c] : clusters) {
            if (c.stem.empty()) { ++residualClusters; }
            if (c.stem.empty() &&
                pluginPieces[c.source] > static_cast<int>(a_opts.maxResidualPieces)) {
                ++residualDropped;
                continue;  // large pack - dropping the nameless leftovers avoids junk
            }
            const std::string display     = c.stem.empty() ? std::string{} : c.displayName;
            const std::string cleanPlugin = CleanPluginName(c.source);
            std::map<std::uint32_t, std::vector<const DetectStyle*>> altPieces;
            std::map<std::uint32_t, const DetectStyle*>              reps;
            auto set = AssembleSet(c.pieces, display, cleanPlugin, &altPieces, &reps);
            if (!set) {
                continue;
            }
            const std::string baseName = set->name;
            set->sourcePlugin          = c.source;
            set->stem                  = c.stem;
            out.push_back(std::move(*set));
            outStemKey.push_back(c.source + '\x1f' + c.stem);
            outType.push_back(c.armorType);

            // ⚠⚠ ONE ROW PER ALTERNATE PIECE, NOT ONE PER COMBINATION. The
            // user's call 2026-08-20 was a browser row per variant, and the
            // product is what would make that unaffordable: a set with four
            // bodies and three boots is twelve rows of a product and seven of a
            // sum. Every slot stays editable once a preset is applied, so the
            // sum reaches anything the product would.
            //
            // ⚠ THE ORDER IS THE REPRESENTATIVE'S OWN RULE, so which alternate
            // comes first does not depend on catalog order: fewest variant
            // tokens, then by name.
            std::vector<const DetectStyle*> flatAlts;
            for (const auto& [bit, v] : altPieces) {
                flatAlts.insert(flatAlts.end(), v.begin(), v.end());
            }
            std::ranges::sort(flatAlts, [](const DetectStyle* a, const DetectStyle* b) {
                const int as = VariantScore(a->name), bs = VariantScore(b->name);
                if (as != bs) { return as < bs; }
                return a->name < b->name;
            });
            std::set<std::string> usedNames{ baseName };
            std::size_t           made = 0;
            for (const auto* alt : flatAlts) {
                if (made >= a_opts.maxVariantRows) {
                    // ⚠ NOT SILENT. A pack of forty recolours would otherwise
                    // bury its own plugin group, and a capped browser and a
                    // broken one look the same from the outside.
                    ++variantsDropped;
                    continue;
                }
                auto        swapped      = reps;
                const auto* was          = reps.at(alt->primaryBit);
                swapped[alt->primaryBit] = alt;
                std::string label        = VariantLabel(was->name, alt->name);
                if (label.empty()) {
                    label = TitleCase(Lower(alt->name));
                }
                std::string name = baseName + " - " + label;
                while (usedNames.contains(name)) {
                    name += " +";
                }
                auto variant = MakeSet(swapped, {}, name, cleanPlugin);
                if (!variant) {
                    continue;
                }
                usedNames.insert(name);
                variant->sourcePlugin = c.source;
                variant->stem         = c.stem;
                out.push_back(std::move(*variant));
                outStemKey.push_back(c.source + '\x1f' + c.stem);
                outType.push_back(c.armorType);
                ++made;
                ++variantRows;
            }
        }

        // Heavy/Light disambiguation: a name whose (source, stem) spans more than
        // one armor type gets a type suffix so the two sets are distinguishable.
        static const char* kTypeLabel[] = { " (Light)", " (Heavy)", " (Clothing)" };
        for (std::size_t i = 0; i < out.size(); ++i) {
            const int bits = typeBits[outStemKey[i]];
            if ((bits & (bits - 1)) != 0 && outType[i] < 3) {  // >1 type present
                out[i].name += kTypeLabel[outType[i]];
                out[i].outfit.name = out[i].name;
            }
        }

        const int qualifiedCount = static_cast<int>(out.size());

        // Drop sets already shipped/owned (match on the body-slot key).
        if (!a_opts.excludeBodyKeys.empty()) {
            std::vector<DetectedSet> kept;
            kept.reserve(out.size());
            for (auto& s : out) {
                const StyleRefKey bk = BodyKeyOf(s);
                if (!bk.Empty() &&
                    std::ranges::find(a_opts.excludeBodyKeys, bk) !=
                        a_opts.excludeBodyKeys.end()) {
                    continue;
                }
                kept.push_back(std::move(s));
            }
            out = std::move(kept);
        }

        // Group by plugin, fullest first, then by name - the browser groups by
        // `source` and shows this order within each mod.
        std::ranges::sort(out, SetOrder);

        if (a_stats) {
            a_stats->clusters         = static_cast<int>(clusters.size());
            a_stats->residualClusters = residualClusters;
            a_stats->residualDropped  = residualDropped;
            a_stats->qualified        = qualifiedCount;
            a_stats->deduped          = qualifiedCount - static_cast<int>(out.size());
            a_stats->variantRows      = variantRows;
            a_stats->variantsDropped  = variantsDropped;
        }
        return out;
    }

    std::size_t CompleteFromOutfits(std::vector<DetectedSet>& a_sets,
                                    const std::vector<DetectOutfit>& a_outfits,
                                    const std::vector<DetectStyle>& a_styles,
                                    std::vector<Completion>* a_log,
                                    std::vector<Completion>* a_declined) {
        if (a_sets.empty() || a_outfits.empty()) {
            return 0;
        }
        std::size_t completed = 0;
        KeyIndex byKey;
        for (const auto& s : a_styles) {
            byKey.emplace(s.key, &s);
        }

        for (auto& set : a_sets) {
            const StyleRefKey bodyKey = BodyKeyOf(set);
            if (bodyKey.Empty()) {
                continue;  // nothing to anchor on, so nothing may be added
            }
            // ⚠⚠ GATHER FIRST, FILL SECOND, AND THAT ORDER IS THE FIX. Until
            // 2026-08-20 this wrote the slot the moment a record offered it,
            // so the FIRST record reached won outright and the rest were never
            // consulted. On a real load order a generic cuirass is named by
            // dozens of NPC outfit records that each pair it with whatever
            // that NPC happened to wear, and the field got exactly what that
            // predicts: Iron gained a DWARVEN helmet, Ebony an IRON one,
            // Stormcloak an IMPERIAL OFFICER'S, and a single Twisted Faith
            // Mask reached a dozen unrelated sets off one shared body.
            //
            // ⚠ AGREEMENT IS THE SIGNAL, not the count of records. A guard
            // set's boot is named the same way by every record that dresses
            // that guard, so it survives; a bandit's hat is one record's
            // opinion against another's, so the slot stays empty and the set
            // reads as the incomplete thing it honestly is.
            struct Candidate {
                const DetectStyle* piece{ nullptr };
                bool               contested{ false };
            };
            std::map<std::uint32_t, Candidate> candidates;
            for (const auto& record : a_outfits) {
                if (std::ranges::find(record.pieces, bodyKey) == record.pieces.end()) {
                    continue;  // ⚠ the identity guard; see the header
                }
                for (const auto& key : record.pieces) {
                    const auto it = byKey.find(key);
                    if (it == byKey.end()) {
                        continue;  // dropped by the catalog (crash risk, wrong sex)
                    }
                    const DetectStyle& piece = *it->second;
                    // ⚠ NEVER THE BODY, even when it is a different record. The
                    // body piece IS the set's identity here, so replacing it
                    // would silently turn one set into another and take the
                    // anchor this whole match was made on with it.
                    if (piece.primaryBit == kBitBody || !piece.fits) {
                        continue;
                    }
                    // ⚠⚠ AND NEVER SLOT 52. An outfit record naming a piece
                    // there is describing an NPC, not proposing a transmog; the
                    // two that reached the field ('Triss DLC Necklace', 'Elden
                    // Smalls') would each have stripped a HIMBO male's cover.
                    if (piece.primaryBit == kBitGenitals) {
                        continue;
                    }
                    // ⚠⚠ AND NEVER JEWELLERY. An amulet or a ring in an outfit
                    // record is what that NPC is wearing, not what the armour
                    // is. Measured over the five masters: every single-record
                    // agreement on slot 35 or 36 is a personal effect or a
                    // quest item, and the field caught four of them on one
                    // screen (Amulet of Dibella on Hide, Ogmund's Amulet of
                    // Talos and Nightweaver's Band on Scaled). 18 amulets and
                    // 4 rings in the masters, none of them set pieces.
                    if (piece.primaryBit == kBitAmulet ||
                        piece.primaryBit == kBitRing) {
                        continue;
                    }
                    if (set.outfit.EntryFor(piece.primaryBit).kind !=
                        SlotEntry::Kind::kPassthrough) {
                        continue;  // the clusterer already chose here
                    }
                    auto& slot = candidates[piece.primaryBit];
                    if (slot.piece == nullptr) {
                        slot.piece = &piece;
                    } else if (slot.piece->key != piece.key) {
                        slot.contested = true;
                    }
                }
            }
            // ⚠⚠ THE SLOTS THE SET ALREADY OCCUPIES, WHOLE MASKS AND NOT JUST
            // PRIMARY BITS, and this is the fix for the clipping the field
            // reported on 2026-08-20. A helmet declares the whole head cluster:
            // `ExecutionHood` is slots 30, 31, 42 and 43, while `ArmorScaledHelmet`
            // is 31 and 42. Asking only whether slot 30 was free said yes and
            // put a second helmet on a head that already had one. The engine
            // will not equip that pair at all; the card drew both.
            //
            // ⚠ IT GROWS AS THE PASS FILLS, so two candidates in the same pass
            // cannot collide with each other either.
            std::uint32_t occupied = SetUnionMask(set, byKey);
            bool          filled   = false;
            for (const auto& [bit, slot] : candidates) {
                // ⚠ REPORTED, NEVER SILENT, AND WITH ITS REASON. A slot this
                // pass declined is indistinguishable in the field from a slot
                // no record ever offered, and the two refusals below are just
                // as indistinguishable from each other without the reason.
                const auto decline = [&](Completion::Reason a_why) {
                    if (a_declined) {
                        a_declined->push_back({ set.name, slot.piece->name, bit, a_why });
                    }
                };
                if (slot.contested) {
                    decline(Completion::Reason::kContested);
                    continue;
                }
                if ((slot.piece->slotMask & occupied) != 0) {
                    decline(Completion::Reason::kOverlaps);
                    continue;
                }
                set.outfit.SetStyle(bit, slot.piece->key);
                occupied |= slot.piece->slotMask;
                filled = true;
                if (a_log) {
                    a_log->push_back({ set.name, slot.piece->name, bit,
                                       Completion::Reason::kFilled });
                }
            }
            if (filled) {
                ++completed;
                // Coverage decides the browser's order within a plugin, so a
                // set that just gained a slot has to be re-counted or it sorts
                // as the thinner thing it used to be.
                set.coverage = MajorCoverage(SetUnionMask(set, byKey));
            }
        }
        std::ranges::sort(a_sets, SetOrder);
        return completed;
    }

    void LinkWeapons(std::vector<DetectedSet>& a_sets,
                     const std::vector<DetectWeapon>& a_weapons) {
        for (const auto& weapon : a_weapons) {
            const std::string nameStem = WeaponNameStem(weapon.name);
            const std::string edidStem = WeaponNameStem(weapon.edid);
            for (auto& set : a_sets) {
                if (set.stem.empty() || set.sourcePlugin != weapon.source ||
                    (set.stem != nameStem && set.stem != edidStem)) {
                    continue;
                }
                // StyleCatalog order is deterministic, so the first matching
                // look is the representative when a mod ships several variants.
                if (set.outfit.WeaponEntryFor(weapon.weaponClass).kind ==
                    SlotEntry::Kind::kPassthrough) {
                    set.outfit.SetWeaponStyle(weapon.weaponClass, weapon.key);
                }
            }
        }
    }

}  // namespace OS::SetDetector
