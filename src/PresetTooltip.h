#pragma once

// The preset hover's whole text, composed from facts the pane has already
// resolved.
//
// ⚠⚠ THIS IS THE DETAIL PANE. The Presets page used to be a 16-em list beside
// a text column, and the column carried the byline, the description, the
// per-piece slot list, the missing-plugin warning, the unfit count and the
// requires line. The page is one pane of cards now (user 2026-08-12, "we don't
// really need all the text on the side for presets... one pane just showing the
// cards or the list of fits is good"), so everything that column said has to be
// said here or it is gone. Read that list before deleting a clause.
//
// ⚠⚠ AND THE BYLINE IS WHY THIS IS A UNIT RATHER THAN A LAMBDA. The old hover
// opened with `p.file` alone, and a Discovered preset's file is the literal
// placeholder "discovered" (AutoPresets.cpp, deliberately, so the detail pane's
// byline read "by Dawnguard (discovered)"). A perfectly healthy preset
// therefore had a one-word tooltip saying "discovered" (user 2026-08-12, "some
// tooltips are just 'discovered' for some reason"). The placeholder is fine;
// printing it with nothing around it was the bug, and its only defence is a
// test that asks what the finished string says.
//
// Pure: no ImGui, no game, no translation lookup. The caller resolves the
// armour names and hands the translated format strings in, which is what keeps
// this testable off the rig.

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace OS::PresetTooltip {

    // What the hover is owed about one preset. Every field is optional; an
    // absent one drops its line rather than printing an empty one.
    struct Facts {
        // The preset's name, and it ALWAYS leads.
        //
        // ⚠⚠ IT USED TO LEAD ONLY WHEN THE CARD HAD TRIMMED IT, on the
        // reasoning that an untrimmed name is already legible an inch away and
        // repeating it is noise. That was the card-view idiom from back when
        // this hover was two lines, and against a hover that is now a whole
        // detail block it reads as a bug: a long name got a title and a short
        // one did not, so two cards side by side had differently shaped
        // tooltips for no reason a player could see (user 2026-08-12, "some
        // preset tooltips show their outfit title, others don't like falmer
        // for some reason"). A detail pane has a title. One rule, no
        // judgement calls about which names are long enough to deserve one.
        std::string name;

        // The byline's two halves, formatted here rather than by the caller so
        // the placeholder case above is inside the tested unit. An empty author
        // falls back the way the detail pane's did.
        std::string author;
        std::string file;

        std::string description;

        // One dressed slot. `line` is formatted by the caller, which owns the
        // slot labels and the `_T` operator: "Chest: Ebony Cuirass",
        // "Hands: Hidden", "Feet: <missing plugin: X>".
        //
        // ⚠ THE DETAIL PANE COLOURED AN UNFIT ROW RED AND A TOOLTIP HAS NO
        // COLOUR, so `unfit` is marked in the line itself. It is also what the
        // count below is counting, so a piece flagged here is never missing
        // from the total. `reason` is the fit check's own words, which the
        // pane used to hand out through a per-piece hover; nothing else in the
        // tooltip carries it, and a tooltip cannot open another tooltip.
        struct Piece {
            std::string line;
            bool        unfit{ false };
            std::string reason;
        };
        std::vector<Piece> pieces;

        std::vector<std::string> missingPlugins;

        // "Requires: A.esp, B.esp", formatted by the caller because
        // $FR_Requires is a translated key and this file cannot look one up.
        // The rest of the warning block is raw English, matching the tooltip
        // it joins; re-hardcoding a string that HAD a translation would have
        // been the one place this rework lost ground.
        std::string requiresLine;

        // Lore-friendly ownership, and the gate that decides it is shown.
        bool loreIncomplete{ false };
        int  ownedPieces{ 0 };
        int  totalPieces{ 0 };

        // Card only: what the picture actually managed to put on. A preset
        // whose plugins are half absent draws three pieces of a nine-piece
        // outfit and looks exactly like a three-piece outfit, which a row
        // never had to worry about because a row shows no picture.
        bool hasScene{ true };
        bool sceneComplete{ true };
        int  sceneResolved{ 0 };
        int  sceneTotal{ 0 };
    };

    // ⚠ A TOOLTIP NEITHER WRAPS NOR SCROLLS, WHICH THE DETAIL PANE DID BOTH
    // OF. The column ran a preset's description through TextWrapped inside a
    // fixed-width child and put a scrollbar on the overflow; a tooltip sizes
    // itself to its longest line and its tallest stack, so the same paragraph
    // arrives as one line wider than the screen. These two numbers are what
    // replaces the column's width and its scrollbar.
    //
    // Only the description needs them. Every other field is a slot label, a
    // plugin name or a counted phrase, all of them short by construction; the
    // description is the one thing a mod author writes free-hand.
    inline constexpr std::size_t kWrapColumns   = 72;
    inline constexpr std::size_t kMaxDescription = 320;

    // Greedy word wrap. Breaks on spaces, respects newlines already in the
    // text, and never splits a word that is itself longer than the column
    // (a path or a plugin name), since a hard split there is less readable
    // than one long line.
    [[nodiscard]] inline std::string Wrap(const std::string& a_text,
                                          std::size_t        a_columns) {
        if (a_columns == 0 || a_text.size() <= a_columns) {
            return a_text;
        }
        std::string out;
        std::size_t lineLen = 0;
        std::size_t i       = 0;
        while (i < a_text.size()) {
            if (a_text[i] == '\n') {
                out += '\n';
                lineLen = 0;
                ++i;
                continue;
            }
            std::size_t end = a_text.find_first_of(" \n", i);
            if (end == std::string::npos) {
                end = a_text.size();
            }
            const std::size_t wordLen = end - i;
            if (lineLen != 0 && lineLen + 1 + wordLen > a_columns) {
                out += '\n';
                lineLen = 0;
            } else if (lineLen != 0) {
                out += ' ';
                ++lineLen;
            }
            out.append(a_text, i, wordLen);
            lineLen += wordLen;
            i = end;
            while (i < a_text.size() && a_text[i] == ' ') {
                ++i;  // the break consumed the space
            }
        }
        return out;
    }

    // Cut an over-long description to a whole word and say it was cut.
    [[nodiscard]] inline std::string Clip(const std::string& a_text,
                                          std::size_t        a_limit) {
        if (a_text.size() <= a_limit) {
            return a_text;
        }
        std::size_t cut = a_text.find_last_of(' ', a_limit);
        if (cut == std::string::npos || cut == 0) {
            cut = a_limit;
        }
        // Never split a UTF-8 sequence: back off to the lead byte.
        while (cut > 0 &&
               (static_cast<unsigned char>(a_text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        std::string out = a_text.substr(0, cut);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
        return out + "...";
    }

    // Join a list the way the pane's warnings did: "a, b, c".
    [[nodiscard]] inline std::string JoinCommas(
        const std::vector<std::string>& a_items) {
        std::string joined;
        for (const auto& item : a_items) {
            joined += joined.empty() ? item : ", " + item;
        }
        return joined;
    }

    // "by %s  (%s)", the detail pane's own format, with the detail pane's own
    // fallback for a preset that names no author.
    //
    // ⚠ THE FILE IS PRINTED WHATEVER IT SAYS, placeholder included. That is
    // the point: "(discovered)" is meaningful beside an author and meaningless
    // alone, and no source in this mod produces an empty file, so the empty
    // case is left reading exactly as the shipped detail pane left it.
    [[nodiscard]] inline std::string Byline(const char* a_format,
                                            const std::string& a_author,
                                            const std::string& a_file) {
        if (!a_format || !*a_format) {
            return {};
        }
        if (a_author.empty() && a_file.empty()) {
            return {};
        }
        const char* author = a_author.empty() ? "unknown" : a_author.c_str();
        char        buf[512]{};
        std::snprintf(buf, sizeof(buf), a_format, author, a_file.c_str());
        return buf;
    }

    // The finished hover text.
    //
    // ⚠ A TOOLTIP NEVER ENDS IN A PERIOD, and this one is assembled from
    // clauses that cannot know whether they are last: which of the pieces, the
    // missing plugins, the unfit count, the requires line, the collection
    // fraction and the picture note lands at the end depends entirely on what
    // is wrong with the preset. Taking one stop off the finished string is that
    // rule written once instead of at every branch.
    [[nodiscard]] inline std::string Compose(const Facts&  a_facts,
                                             const char*   a_bylineFormat) {
        std::string tip;
        const auto  line = [&](const std::string& a_text) {
            if (a_text.empty()) {
                return;
            }
            if (!tip.empty()) {
                tip += '\n';
            }
            tip += a_text;
        };
        // A blank line only ever separates two blocks that both have content,
        // so an outfit with no warnings never ends on a dangling gap.
        const auto blank = [&] {
            if (!tip.empty()) {
                tip += '\n';
            }
        };

        line(a_facts.name);
        line(Byline(a_bylineFormat, a_facts.author, a_facts.file));
        line(Wrap(Clip(a_facts.description, kMaxDescription), kWrapColumns));

        int unfit = 0;
        if (!a_facts.pieces.empty()) {
            blank();
            for (const auto& piece : a_facts.pieces) {
                if (!piece.unfit) {
                    line(piece.line);
                    continue;
                }
                ++unfit;
                line(piece.reason.empty()
                         ? piece.line + "  (may not fit)"
                         : piece.line + "  (may not fit: " + piece.reason + ")");
            }
        }

        // Everything that is WRONG, below a gap, so a healthy preset reads as
        // a description and a list and a broken one says so in its own block.
        std::string warnings;
        const auto  warn = [&](const std::string& a_text) {
            if (a_text.empty()) {
                return;
            }
            if (!warnings.empty()) {
                warnings += '\n';
            }
            warnings += a_text;
        };

        if (!a_facts.missingPlugins.empty()) {
            warn(std::string("Missing required plugin") +
                 (a_facts.missingPlugins.size() == 1 ? ": " : "s: ") +
                 JoinCommas(a_facts.missingPlugins));
            warn("The preset applies without them");
        }
        if (unfit > 0) {
            warn(std::to_string(unfit) + (unfit == 1 ? " piece" : " pieces") +
                 " may not fit your body");
        }
        warn(a_facts.requiresLine);
        if (a_facts.loreIncomplete) {
            warn("Lore-friendly collection: " +
                 std::to_string(a_facts.ownedPieces) + "/" +
                 std::to_string(a_facts.totalPieces) + " pieces owned");
        }
        if (!a_facts.hasScene) {
            warn("No picture: none of its pieces are installed");
        } else if (!a_facts.sceneComplete) {
            warn("Picture shows " + std::to_string(a_facts.sceneResolved) +
                 " of " + std::to_string(a_facts.sceneTotal) + " pieces");
        }

        if (!warnings.empty()) {
            blank();
            line(warnings);
        }

        // ⚠ AN ELLIPSIS IS NOT A FULL STOP, and the rule is about full stops.
        // Clip's "..." can be the last thing in the tooltip when a runaway
        // description is the only field, and popping one dot off it leaves
        // ".." reading as a typo rather than as a truncation. The card's own
        // name trimming uses the same three dots for the same meaning.
        const bool ellipsis =
            tip.size() >= 3 && tip.compare(tip.size() - 3, 3, "...") == 0;
        if (!tip.empty() && tip.back() == '.' && !ellipsis) {
            tip.pop_back();
        }
        return tip;
    }

}  // namespace OS::PresetTooltip
