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

        // One cluster of same-plugin, same-stem, same-type pieces -> a set, or
        // nullopt if it does not clear the body-anchored >=2-majors bar. Picks
        // one representative per slot (the base of any variant set) and counts
        // the alternates. An empty display name falls back to the plugin name.
        std::optional<DetectedSet> AssembleSet(const std::vector<const DetectStyle*>& a_pieces,
                                               std::string_view a_displayName,
                                               std::string_view a_cleanPlugin) {
            std::map<std::uint32_t, const DetectStyle*> rep;
            std::map<std::uint32_t, int>                alts;
            for (const auto* p : a_pieces) {
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

            std::uint32_t unionMask = 0;
            for (const auto& [bit, p] : rep) { unionMask |= p->slotMask; }
            if (!(unionMask & kBodyCat) || MajorCoverage(unionMask) < 2) {
                return std::nullopt;  // not body-anchored / too thin
            }

            DetectedSet set;
            set.source    = std::string(a_cleanPlugin);
            set.coverage  = MajorCoverage(unionMask);
            set.fullyFits = true;
            for (const auto& [bit, p] : rep) {
                set.outfit.SetStyle(bit, p->key);
                if (!p->fits) { set.fullyFits = false; }
                if (alts[bit] > 0) { set.variants.emplace_back(bit, alts[bit]); }
            }
            const std::string stem = NameStem(a_displayName);
            set.name        = stem.empty() ? std::string(a_cleanPlugin) : TitleCase(stem);
            set.outfit.name = set.name;
            return set;
        }

        // The body-slot key that identifies a set for dedup.
        StyleRefKey BodyKeyOf(const DetectedSet& a_set) {
            const auto& e = a_set.outfit.EntryFor(kBitBody);
            return e.kind == SlotEntry::Kind::kStyle ? e.style : StyleRefKey{};
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
                Cluster* best = nullptr;
                for (auto& [bk, B] : clusters) {
                    if (&B == &A || B.stem.empty() || B.source != A.source ||
                        B.armorType != A.armorType) {
                        continue;
                    }
                    if (IsWordPrefix(B.stem, A.stem) &&
                        (!best || B.stem.size() > best->stem.size())) {
                        best = &B;
                    }
                }
                if (best) {
                    best->pieces.insert(best->pieces.end(), A.pieces.begin(),
                                        A.pieces.end());
                    clusters.erase(ait);
                }
            }
        }

        // Assemble each cluster. Residual clusters (empty stem) emit only when
        // the whole plugin is small enough to be "one outfit" (anti-Frankenstein).
        std::vector<DetectedSet>  out;
        std::vector<std::string>  outStemKey;  // parallel: source\x1f stem
        std::vector<std::uint8_t> outType;     // parallel: armorType
        int                       residualClusters = 0, residualDropped = 0;
        for (auto& [key, c] : clusters) {
            if (c.stem.empty()) { ++residualClusters; }
            if (c.stem.empty() &&
                pluginPieces[c.source] > static_cast<int>(a_opts.maxResidualPieces)) {
                ++residualDropped;
                continue;  // large pack - dropping the nameless leftovers avoids junk
            }
            const std::string display = c.stem.empty() ? std::string{} : c.displayName;
            if (auto set = AssembleSet(c.pieces, display, CleanPluginName(c.source))) {
                out.push_back(std::move(*set));
                outStemKey.push_back(c.source + '\x1f' + c.stem);
                outType.push_back(c.armorType);
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
        std::ranges::sort(out, [](const DetectedSet& a, const DetectedSet& b) {
            if (a.source != b.source) { return a.source < b.source; }
            if (a.fullyFits != b.fullyFits) { return a.fullyFits; }  // fitting sets first
            if (a.coverage != b.coverage) { return a.coverage > b.coverage; }
            return a.name < b.name;
        });

        if (a_stats) {
            a_stats->clusters         = static_cast<int>(clusters.size());
            a_stats->residualClusters = residualClusters;
            a_stats->residualDropped  = residualDropped;
            a_stats->qualified        = qualifiedCount;
            a_stats->deduped          = qualifiedCount - static_cast<int>(out.size());
        }
        return out;
    }

}  // namespace OS::SetDetector
