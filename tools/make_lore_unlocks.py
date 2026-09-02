"""Generate Unlocks/lore.json from the three lock TSVs.

The lore pack gates colours on things a character DID in Skyrim: a named quest
completed, a named location cleared, or one of Skyrim's own misc stat counters
past a threshold. It composes with the shipped eso.json without any code:
DyeRules merges Unlocks/*.json later-file-wins, so a key here replaces the same
key there and every colour this file says nothing about keeps whatever eso.json
gave it.

⚠ IT SHIPS IN core AND EVERY INSTALL MERGES IT. This docstring said "only on the
Lore-friendly FOMOD path" until 2026-08-13, which stopped being true when
make_fomod.sh moved the file into core and started asserting it is there.

⚠ THIS FILE ONLY EVER TIGHTENS. Every rule it writes replaces a rarity-tier
rule, and a named form is a harder thing to have done than reaching a level. A
key added here that loosens its colour is a bug, and check_dye_rules.py is what
catches it.

⚠ AN EDITOR ID IS NEVER GUESSED. Every row is resolved through
tools/data/form_index.json, which is generated from the game's own masters. An
unverified id parses, never matches, and locks its colour for the life of the
character with no log line: three Dragonborn ids were wrong on the first
authoring pass and the index caught all three.

⚠⚠ A STAT NAME IS NEVER GUESSED EITHER, AND ITS FAILURE IS WORSE THAN A FORM'S.
Game.QueryStat answers int 0 for a name the engine does not know, which is byte
for byte the answer an honest zero gives: nothing throws, nothing logs, and the
colour locks for the life of every character. So every one is resolved through
tools/data/stat_index.json, read out of SkyrimSE.exe by make_stat_index.py.
Three of the eight questline counters carry a word or a mark nobody guesses -
"The Companions Quests Completed", "Thieves' Guild Quests Completed" and "The
Dark Brotherhood Quests Completed" - and all three were typed the obvious way
first.

The counter rows were authored on 2026-08-08 and sat unwritable until
2026-08-13, because reading a Skyrim stat from an SKSE plugin was believed
impossible: CommonLibSSE-NG has no MiscStatManager, and the economy spec drew
"so the stats are not reachable" from that. True about the native, wrong about
the game. The route is the Papyrus VM. src/DyeStats.cpp is that route and this
is what it unblocked.
"""
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_eso_palette import slug  # the one slug rule, never a second copy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tools", "data")
INDEX = os.path.join(DATA, "form_index.json")
PALETTE = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Dyes",
                       "eso.json")
OUT = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Unlocks",
                   "lore.json")

SRC_TSV = os.path.join(DATA, "eso_dyes.tsv")
STORY = os.path.join(DATA, "dye_locks_story.tsv")
CLEARED = os.path.join(DATA, "dye_locks_cleared.tsv")
COUNTERS = os.path.join(DATA, "dye_locks_counters.tsv")
STAT_INDEX = os.path.join(DATA, "stat_index.json")

# What tells a Skyrim misc stat from one of Fitting Room's own counters inside
# the one "deed" clause kind. Byte-identical to kStatDeedPrefix in
# src/DyeStatDeed.h and to STAT_DEED_PREFIX in check_dye_rules.py.
STAT_DEED_PREFIX = "stat:"


def read_tsv(path, want_header):
    lines = [l.rstrip("\r\n") for l in io.open(path, encoding="utf-8") if l.strip()]
    header = lines[0].split("\t")
    if header != want_header:
        sys.exit("%s: unexpected header %r" % (os.path.basename(path), header))
    rows = []
    for lineno, line in enumerate(lines[1:], start=2):
        parts = line.split("\t")
        if len(parts) != len(want_header):
            sys.exit("%s line %d: %d fields, expected %d"
                     % (os.path.basename(path), lineno, len(parts),
                        len(want_header)))
        rows.append((lineno, dict(zip(header, parts))))
    return rows


def main():
    forms = json.load(io.open(INDEX, encoding="utf-8"))
    # ⚠ KEYED BY KIND AS WELL AS EDID. A quest and a location can carry the
    # same editor id, and resolving a location row against a QUST would build a
    # rule that can never be satisfied.
    by_kind = {}
    for f in forms:
        by_kind.setdefault(f["kind"], {})[f["edid"]] = f

    # ⚠ ABSENT IS NOT A FAILURE HERE, IT IS A REFUSAL TO GUESS. Without the
    # index this generator cannot tell a real stat name from an invented one,
    # and an invented one locks its colour forever. So the counter rows are
    # written unchecked only if the index is genuinely missing, and the run says
    # so loudly; check_dye_rules.py checks them again before anything packages.
    stats = None
    if os.path.exists(STAT_INDEX):
        stats = set(json.load(io.open(STAT_INDEX, encoding="utf-8"))["names"])
    else:
        print("WARNING no %s, so no stat name is verified. Run "
              "tools/make_stat_index.py." % os.path.basename(STAT_INDEX))

    palette = json.load(io.open(PALETTE, encoding="utf-8"))["dyes"]
    by_id = {d["id"]: d for d in palette}

    # ⚠ MATCHED THROUGH THE FROZEN COLUMN, NOT THE SHIPPED NAME, AND THAT IS
    # THE WHOLE REASON THE SPLIT EXISTS. These lock rows were authored against
    # ESO's own names, and 41 of those colours have since been renamed for
    # Skyrim. eso_dyes.tsv's "name" column is the one that never moves and is
    # what the id is derived from, so it is what a row is resolved through.
    # display_name is accepted too, so a row written after a rename also works.
    src = [l.rstrip("\r\n") for l in io.open(SRC_TSV, encoding="utf-8")
           if l.strip()]
    if src[0].split("\t")[:2] != ["name", "display_name"]:
        sys.exit("%s: unexpected header" % os.path.basename(SRC_TSV))
    by_name = {}
    for line in src[1:]:
        parts = line.split("\t")
        frozen, display = parts[0], parts[1]
        did = "eso:" + slug(frozen)
        entry = by_id.get(did)
        if entry is None:
            continue
        by_name[frozen] = entry
        if display:
            by_name[display] = entry

    rules = {}
    problems = []
    low = 0

    def add(lineno, source, dye_name, hexv, kind_key, edid, form_kind,
            confidence):
        nonlocal low
        at = "%s line %d" % (source, lineno)
        dye = by_name.get(dye_name)
        if dye is None:
            problems.append("%s: no colour named %r in the shipped pack"
                            % (at, dye_name))
            return
        if dye["hex"].upper() != hexv.upper():
            # ⚠ THE HEX IS THE PROOF THE ROW STILL MEANS WHAT IT SAID. The
            # reasoning column was written about a specific swatch; a hex that
            # has moved means the row is now about a different one.
            problems.append("%s: %r is %s in the pack, the row says %s"
                            % (at, dye_name, dye["hex"], hexv))
            return
        form = by_kind.get(form_kind, {}).get(edid)
        if form is None:
            problems.append("%s: %s %r is not in the form index, so a rule "
                            "naming it would never match and would lock %r "
                            "forever" % (at, form_kind, edid, dye_name))
            return
        if dye["id"] in rules:
            problems.append("%s: %r already has a rule in this file" % (at, dye["id"]))
            return
        if confidence.strip().lower() == "low":
            low += 1
        rules[dye["id"]] = {
            "clause": '{ "type": "%s", "plugin": "%s", "formId": "%s" }'
                      % (kind_key, form["plugin"], form["formId"]),
            "kind": kind_key,
            "comment": "%s (%s)" % (form["name"], confidence),
        }

    def add_counter(lineno, dye_name, hexv, stat, minimum, note):
        """One colour gated on a Skyrim misc stat counter.

        ⚠⚠ THE STAT NAME IS RESOLVED THROUGH THE INDEX, EXACTLY AS AN EDITOR ID
        IS, AND FOR A WORSE FAILURE. Game.QueryStat answers int 0 for a name the
        engine does not know, which is the answer an honest zero gives, so a
        typo locks its colour for the life of every character and logs nothing
        anywhere. Three of the eight questline counters carry a word or an
        apostrophe nobody guesses. Refusing to write an unresolvable name is the
        only defence that exists.
        """
        at = "counters line %d" % lineno
        dye = by_name.get(dye_name)
        if dye is None:
            problems.append("%s: no colour named %r in the shipped pack"
                            % (at, dye_name))
            return
        if dye["hex"].upper() != hexv.upper():
            problems.append("%s: %r is %s in the pack, the row says %s"
                            % (at, dye_name, dye["hex"], hexv))
            return
        if stats is not None and stat not in stats:
            problems.append("%s: %r is not a misc stat the engine knows, so it "
                            "would read 0 forever and lock %r for every "
                            "character. Compared exactly, apostrophes and any "
                            "leading \"The\" included." % (at, stat, dye_name))
            return
        try:
            m = int(minimum)
        except ValueError:
            problems.append("%s: min %r is not a whole number" % (at, minimum))
            return
        # ⚠ A COUNTER STARTS AT 0 AND ONLY RISES, so min 0 is "always" spelt in a
        # way that hides it, and it would hand the colour over permanently on
        # the first load. Same floor check_dye_rules.py applies.
        if m < 1:
            problems.append("%s: min %d is met before anything is done, so %r "
                            "would be free and permanent" % (at, m, dye_name))
            return
        if dye["id"] in rules:
            problems.append("%s: %r already has a rule in this file"
                            % (at, dye["id"]))
            return
        rules[dye["id"]] = {
            "clause": '{ "type": "deed", "deed": "%s%s", "min": %d }'
                      % (STAT_DEED_PREFIX, stat, m),
            "kind": "deed",
            "comment": "%s %d%s" % (stat, m, (" - " + note) if note else ""),
        }

    for lineno, r in read_tsv(STORY, ["dye_name", "hex", "eso_achievement",
                                      "quest_edid", "quest_name", "confidence",
                                      "reasoning"]):
        add(lineno, "story", r["dye_name"], r["hex"], "quest", r["quest_edid"],
            "QUST", r["confidence"])

    for lineno, r in read_tsv(CLEARED, ["dye_name", "hex", "eso_achievement",
                                        "location_edid", "location_name",
                                        "confidence", "reasoning"]):
        add(lineno, "cleared", r["dye_name"], r["hex"], "locationCleared",
            r["location_edid"], "LCTN", r["confidence"])

    # ⚠ THE "keep_level" COLUMN IS READ AND NOT USED, deliberately. It records
    # the level each of these colours sat behind while nothing could read a
    # Skyrim counter, so it is the fallback this file exists to retire. Kept in
    # the TSV as the record of what the ladder used to be; a generator that
    # emitted it would be undoing the change.
    for lineno, r in read_tsv(COUNTERS, ["dye_name", "hex", "rarity",
                                         "eso_achievement", "stat", "min",
                                         "keep_level", "note"]):
        add_counter(lineno, r["dye_name"], r["hex"], r["stat"], r["min"],
                    r["note"])

    if problems:
        for p in problems:
            print("PROBLEM %s" % p)
        sys.exit("%d row(s) did not resolve; nothing written" % len(problems))

    body = []
    for did in sorted(rules):
        r = rules[did]
        body.append('    "%s": [ %s ],  // %s'
                    % (did, r["clause"], r["comment"]))
    # Trailing comma on the last entry is not valid, and the loader's parser
    # does not forgive it.
    body[-1] = body[-1].replace("} ],  //", "} ]   //", 1)

    text = HEADER + "{\n  \"dyes\": {\n" + "\n".join(body) + "\n  }\n}\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    io.open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s" % OUT)
    print("%d rule(s): %d quest, %d location, %d stat counter"
          % (len(rules),
             sum(1 for r in rules.values() if r["kind"] == "quest"),
             sum(1 for r in rules.values() if r["kind"] == "locationCleared"),
             sum(1 for r in rules.values() if r["kind"] == "deed")))
    print("%d of them are marked low confidence and are free picks" % low)


HEADER = """// Fitting Room, the lore unlock rules.
//
// ⚠ SHIPS IN core, SO EVERY INSTALL MERGES IT. This file's header claimed the
// opposite until 2026-08-13 - "ships ONLY on the Lore-friendly FOMOD path" -
// and that stopped being true when make_fomod.sh moved it into core, which it
// now asserts on every package. The old arrangement cost a Lore-friendly player
// their whole economy if they had installed the other way round first, and it
// changed nothing for anyone else: a rule can only ever LOCK, a colour with no
// rule is FREE, and with bDyeUnlocks off nothing is gated at all.
//
// GENERATED by tools/make_lore_unlocks.py from tools/data/dye_locks_story.tsv,
// dye_locks_cleared.tsv and dye_locks_counters.tsv. Edit those, not this.
//
// HOW IT COMPOSES. DyeRules merges Unlocks/*.json later-file-wins, so a key
// here replaces the same key in eso.json outright and any colour this file does
// not name keeps whatever eso.json gave it. There is no merging of CLAUSES: the
// list here is the whole rule.
//
// EVERY RULE HERE TIGHTENS. Each one replaces a rarity-tier rule, and doing a
// named quest or clearing a named place is a harder thing to have done than
// reaching a level. check_dye_rules.py proves that on every run.
//
// THE COMMENT AFTER EACH LINE NAMES THE FORM AND ITS CONFIDENCE. "low" means
// the pairing is a free pick that reads well rather than something the source
// material states, and the reasoning for each is in the TSV. They are not
// wrong, they are chosen; do not read them as researched.
//
// A form that does not resolve, because its plugin is absent, is not an error:
// the condition simply reads unsatisfied and the colour stays locked. That is
// the fail-closed direction, and it is why the generator refuses to write an
// editor id the form index does not know.
//
// ⚠⚠ THE "stat:" DEEDS ARE SKYRIM'S OWN MISC COUNTERS AND THEY ARE THE ONE
// THING HERE WITH NO RUNTIME SAFETY NET AT ALL. They reach the game through
// Game.QueryStat, which answers int 0 for a name the engine does not know -
// byte for byte the answer an honest zero gives. Nothing throws, nothing logs,
// and the colour locks for the life of every character. So every name is
// resolved through tools/data/stat_index.json, read out of SkyrimSE.exe, by
// this generator AND again by tools/check_dye_rules.py. Three of the eight
// questline counters carry a word or an apostrophe nobody guesses.
//
// ⚠ AND THEIR VALUES ARRIVE ~180 ms AFTER THE ASK, on the VM's own thread. The
// promotion pass at kPostLoadGame therefore reads every one of them as 0 and
// runs a second time when the answers land. A colour gated here is earned on
// that second pass, not the first.
"""


if __name__ == "__main__":
    main()
