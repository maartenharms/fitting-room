#pragma once

// Per-path hide and show rules for preview scenes (OS-191), the "per-mod
// fixups" the preview grid spec calls for: one broken armour pack handled
// without a code change.
//
// ⚠⚠ "PER-MOD" IS THE SPEC'S WORD AND IT IS NOT AVAILABLE AT RUNTIME. MO2 and
// Vortex flatten every mod into one virtual Data/, so there is no mod identity
// to key on; what the extractor can see is a model PATH. A scope therefore
// matches a folded path SUBSTRING, which also has the property a prefix match
// would not: "ldd/lili collection/" catches both
// meshes/ldd/lili collection/... and the UBE conversion's
// meshes/!ube/ldd/lili collection/... .
//
// Pure, for PreviewFilter.h's reason: no test compiles the engine side, so
// every decidable rule lives in a header and is pinned by test.
//
// Inputs are folded by the caller (PreviewGrid::FoldPath): lowercase, forward
// slashes. One fold in the subsystem.

#include "PreviewGrid.h"  // FoldPath, SceneIdentity (both pure)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace OS::PreviewScopes {

    // One fixup: the paths it covers and what to do inside them.
    struct Scope {
        // Folded substring of the model path. Empty matches nothing, never
        // everything: a typo that emptied this field would otherwise apply a
        // mod-specific hide list to the whole catalog.
        std::string              path;
        std::vector<std::string> hide;
        std::vector<std::string> show;
    };

    struct ScopeSet {
        std::vector<Scope> scopes;
    };

    enum class Verdict : std::uint8_t {
        kNoOpinion,  // no scope covers this, the default rules decide alone
        kHide,
        kShow,
    };

    // A deliberately small glob: a leading and/or trailing '*' and nothing
    // else. Anything richer is a regex engine living in a data file, and
    // nobody has asked for one.
    //
    //   "colskirtlegs"   exact
    //   "col*"           prefix
    //   "*_col"          suffix
    //   "*_col*"         substring
    //   "*"              everything in this scope
    [[nodiscard]] inline bool MatchesPattern(std::string_view a_folded,
                                             std::string_view a_pattern) {
        if (a_pattern.empty()) {
            // An empty pattern matches nothing. Same reasoning as an empty
            // scope path: the failure mode of "matches everything" is a
            // silently blank card, which reads as a broken previewer rather
            // than as a bad rule.
            return false;
        }
        const bool openLeft  = a_pattern.front() == '*';
        const bool openRight = a_pattern.size() > 1 && a_pattern.back() == '*';
        std::string_view core = a_pattern;
        if (openLeft) {
            core.remove_prefix(1);
        }
        if (openRight && !core.empty()) {
            core.remove_suffix(1);
        }
        if (core.empty()) {
            // "*" or "**": the scope's author asking for all of it.
            return true;
        }
        if (openLeft && openRight) {
            return a_folded.find(core) != std::string_view::npos;
        }
        if (openLeft) {
            return a_folded.size() >= core.size() &&
                   a_folded.compare(a_folded.size() - core.size(), core.size(), core) == 0;
        }
        if (openRight) {
            return a_folded.compare(0, core.size(), core) == 0;
        }
        return a_folded == core;
    }

    [[nodiscard]] inline bool ScopeCovers(const Scope&     a_scope,
                                          std::string_view a_foldedModelPath) {
        return !a_scope.path.empty() &&
               a_foldedModelPath.find(a_scope.path) != std::string_view::npos;
    }

    // ⚠ SHOW BEATS HIDE, the spec's rule, and it is decided across every
    // covering scope rather than within one: two files may cover the same
    // path, and a rescue that only won inside its own file would depend on
    // which file loaded first.
    //
    // ⚠ WHAT A SHOW MAY BEAT IS PINNED AT THE CALL SITE, NOT HERE. It rescues
    // the helper-token default, which is the documented over-match ("a real
    // piece named HelperPlate would disappear"), and it must never rescue the
    // embedded-body or hands/feet rules: those are correctness rules tied to
    // slots and body doubles rather than name guesses, and a rescue there puts
    // a second body back inside the garment.
    [[nodiscard]] inline Verdict VerdictFor(const ScopeSet&  a_set,
                                            std::string_view a_foldedModelPath,
                                            std::string_view a_foldedName) {
        bool hidden = false;
        for (const auto& scope : a_set.scopes) {
            if (!ScopeCovers(scope, a_foldedModelPath)) {
                continue;
            }
            for (const auto& pattern : scope.show) {
                if (MatchesPattern(a_foldedName, pattern)) {
                    return Verdict::kShow;
                }
            }
            for (const auto& pattern : scope.hide) {
                if (MatchesPattern(a_foldedName, pattern)) {
                    hidden = true;
                    break;
                }
            }
        }
        return hidden ? Verdict::kHide : Verdict::kNoOpinion;
    }

    // The tag a scene's disk key carries for this path, empty when no scope
    // covers it.
    //
    // ⚠⚠ THE EMPTY CASE IS THE WHOLE POINT AND IS PINNED BY TEST. A scene
    // outside every scope must contribute exactly the bytes it did before this
    // feature existed, so its thumbnail is not orphaned. Bumping the renderer
    // version would rebuild the entire cache instead, and the phase 1 rule,
    // restated through phase 3, is that orphaning the whole cache is not
    // acceptable.
    //
    // ⚠ IT FINGERPRINTS THE RULES, NOT WHAT THEY HID. Capture knows the model
    // paths; the geometry names only exist once the NIF loads on the render
    // thread. So this can rebuild a card the rules turn out not to change, and
    // can never leave a stale card that they do, which is the conservative
    // direction of the two.
    [[nodiscard]] inline std::string ScopeTagFor(const ScopeSet&  a_set,
                                                 std::string_view a_foldedModelPath) {
        // FNV-1a, 32 bit. A hash rather than the rules themselves because this
        // goes in a FILENAME: a key carrying "*_col*" verbatim would have to
        // answer for every character a filesystem dislikes.
        std::uint32_t h       = 2166136261u;
        bool          covered = false;
        const auto    eat     = [&h](std::string_view a_s) {
            for (const char c : a_s) {
                h ^= static_cast<std::uint8_t>(c);
                h *= 16777619u;
            }
            // A separator, so ["ab","c"] and ["a","bc"] do not collide.
            h ^= 0xFFu;
            h *= 16777619u;
        };
        for (const auto& scope : a_set.scopes) {
            if (!ScopeCovers(scope, a_foldedModelPath)) {
                continue;
            }
            covered = true;
            eat(scope.path);
            for (const auto& p : scope.hide) {
                eat(p);
            }
            // ⚠ The two lists are separated, so moving a pattern from hide to
            // show changes the tag. Without this a rescue would reuse the
            // thumbnail built while the piece was hidden.
            eat("|show|");
            for (const auto& p : scope.show) {
                eat(p);
            }
        }
        if (!covered) {
            return {};
        }
        char out[9]{};
        std::snprintf(out, sizeof out, "%08x", h);
        return std::string{ out };
    }

    // Every path's tag for one scene, or an EMPTY vector when no scope covers
    // any of them.
    //
    // ⚠ ONE HOME FOR THE STAMP. The identity is built at three separate call
    // sites in EditorUI and the key is needed by two more; a tag computed at
    // any of them independently is a tag that can disagree with the others,
    // and a key that disagrees with itself between the request and the read
    // is a card that never resolves.
    [[nodiscard]] inline std::vector<std::string> TagsFor(
        const ScopeSet& a_set, const std::vector<std::string>& a_modelPaths) {
        std::vector<std::string> tags(a_modelPaths.size());
        bool                     any = false;
        for (std::size_t i = 0; i < a_modelPaths.size(); ++i) {
            tags[i] = ScopeTagFor(a_set, PreviewGrid::FoldPath(a_modelPaths[i]));
            any     = any || !tags[i].empty();
        }
        if (!any) {
            // ⚠ EMPTY, NOT A VECTOR OF EMPTY STRINGS. The key fold reads the
            // vector's size, and the scope-less pin is that such a scene folds
            // to exactly its old bytes.
            return {};
        }
        return tags;
    }

    // ---- the loaded set (PreviewScopes.cpp, engine side) ---------------

    // Read Data/SKSE/Plugins/FittingRoom/PreviewFilters/*.json, replacing the
    // live set. Returns how many scopes are held. Files are read in sorted
    // order so two covering the same path resolve identically on every
    // machine. A file that will not parse costs its own scopes and nothing
    // else: these are hand-authored fixups, so a third party shipping a broken
    // one must not disarm the ones that work.
    std::size_t Load();

    // A consistent copy, the contract every other store in this codebase has.
    // Take it once per pass, never once per geometry.
    [[nodiscard]] ScopeSet Snapshot();

}  // namespace OS::PreviewScopes
