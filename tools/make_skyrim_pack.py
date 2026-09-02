"""Generate the Skyrim colour pack and its unlock rules from one TSV.

    python tools\\make_skyrim_pack.py

Writes BOTH halves of a dye, which is the point: a colour and the deed that
earns it are one authoring decision and splitting them across two hand-edited
files is how a rule comes to name an id no colour claims, or a colour ships with
no rule and is therefore FREE.

    dist/SKSE/Plugins/FittingRoom/Dyes/skyrim.json      the colours
    dist/SKSE/Plugins/FittingRoom/Unlocks/skyrim.json   what each one costs

Source: tools/data/skyrim_dyes.tsv. Edit that, not the JSON.

WHAT THIS PACK IS. Colours named for Skyrim's own world and earned by doing
Skyrim's own things: the Daedric Princes, the main quest, the faction lines, the
great ruins, and the counters the game keeps on the player's behalf. The eso
pack mirrors ESO's palette and its economy was mined out of ESO achievements;
this one starts from the Skyrim end instead.

⚠⚠ AN ID IS FOREVER AND IT IS DERIVED FROM THE NAME. Unlocks are stored by id,
the id is "skyrim:" plus the slugged name, and renaming a colour after release
therefore detaches every stored unlock and every painted garment on every save,
SILENTLY. Change the "name" column of a shipped row and you have orphaned it.
Add rows freely; rename nothing.

⚠⚠ NOTHING IS TYPED FROM MEMORY. Every quest and location is resolved through
tools/data/form_index.json and every stat name through tools/data/stat_index.json,
both generated from the game itself. This refuses to write a row it cannot
resolve, because the failure it prevents is unrecoverable in one direction and
invisible in both:

  * A form id that does not resolve is never satisfied, so its colour is locked
    for the life of every character, and the locked swatch quietly falls back to
    naming the plugin instead of the quest.
  * A stat name the engine does not know answers int 0 through Game.QueryStat,
    which is byte for byte the answer an honest zero gives. Nothing throws,
    nothing logs.

⚠ A LOCATION MUST BE ONE THE GAME EVER MARKS CLEARED. Dungeons, caves, forts,
ruins and camps clear. Cities, towns, holds, farms and open exteriors do not, so
a locationCleared clause on one locks its colour permanently and this script
cannot tell the difference: the flag is set at runtime, not authored in the
master. That check is the author's, and it is why every row carries reasoning.
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
SRC = os.path.join(DATA, "skyrim_dyes.tsv")
FORM_INDEX = os.path.join(DATA, "form_index.json")
STAT_INDEX = os.path.join(DATA, "stat_index.json")
DYES_DIR = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Dyes")
RULES_DIR = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Unlocks")
OUT_DYES = os.path.join(DYES_DIR, "skyrim.json")
OUT_RULES = os.path.join(RULES_DIR, "skyrim.json")

PREFIX = "skyrim:"
STAT_DEED_PREFIX = "stat:"

HEADER = ["name", "hex", "rarity", "family", "gate_kind", "gate_ref",
          "gate_min", "hex2", "mode", "gloss", "sheen", "flake",
          "confidence", "reasoning"]

# ⚠ hex2 WITHOUT A mode STAYS FLAT, and that is the loader's rule, not this
# script's: DyePalette stores both stops but only ramps between them when a mode
# says to. So a row with a second stop and no mode is a colour that looks exactly
# like a plain one, which is the shape a half-finished edit leaves behind.
# Refused below rather than shipped looking ordinary.
MODES = {"nacre", "iridescent"}

# ⚠ THE GRID FILES A COLOUR BY RARITY AND TWO OF ESO'S FIVE MEAN SOMETHING ELSE
# IN THIS UI. "Material" and "Dye Stamp" are the metallic-finish and store
# buckets, so a flat colour wearing one lands in the wrong group and the player
# cannot find it. Refused here rather than left to a reviewer's eye.
RARITIES = {"Common", "Uncommon", "Rare"}

HEX_RE = re.compile(r"^[0-9A-F]{6}$")


def read_tsv(path):
    if not os.path.exists(path):
        sys.exit("no %s. Nothing to generate." % path)
    lines = [l.rstrip("\r\n") for l in io.open(path, encoding="utf-8")
             if l.strip()]
    header = lines[0].split("\t")
    if header != HEADER:
        sys.exit("%s: unexpected header %r, wanted %r"
                 % (os.path.basename(path), header, HEADER))
    rows = []
    for lineno, line in enumerate(lines[1:], start=2):
        parts = line.split("\t")
        if len(parts) != len(HEADER):
            sys.exit("%s line %d: %d fields, expected %d"
                     % (os.path.basename(path), lineno, len(parts), len(HEADER)))
        rows.append((lineno, dict(zip(HEADER, parts))))
    return rows


def load_existing_ids_and_hexes():
    """Every id, name and hex already shipped, so a new row cannot collide.

    ⚠ AN ID COLLISION IS NOT A DUPLICATE COLOUR, IT IS A STOLEN RULE. DyePalette
    reports collisions and keeps one entry; whichever loses, the unlock rule
    keyed to that id now gates a colour its author never saw. Names and hexes
    are softer - two colours a shade apart are merely bad curation - so those
    are reported and not refused.
    """
    ids, names, hexes = set(), {}, {}
    for path in sorted(os.listdir(DYES_DIR)):
        if not path.endswith(".json") or path == os.path.basename(OUT_DYES):
            continue
        raw = io.open(os.path.join(DYES_DIR, path), encoding="utf-8-sig").read()
        raw = re.sub(r"^\s*//.*$", "", raw, flags=re.M)
        for d in json.loads(raw).get("dyes", []):
            ids.add(d["id"])
            names[d["name"].lower()] = d["id"]
            hexes[d.get("hex", "").upper()] = d["id"]
    return ids, names, hexes


def main():
    forms = json.load(io.open(FORM_INDEX, encoding="utf-8"))
    # ⚠ KEYED BY KIND AS WELL AS EDID. A quest and a location can carry the same
    # editor id, and resolving one against the other builds a rule that can
    # never be satisfied, silently, in both directions.
    by_kind = {}
    for f in forms:
        by_kind.setdefault(f["kind"], {})[f["edid"]] = f

    if not os.path.exists(STAT_INDEX):
        sys.exit("no %s, so no stat name can be verified and a wrong one locks "
                 "its colour forever with nothing logged. Run "
                 "tools/make_stat_index.py first."
                 % os.path.basename(STAT_INDEX))
    stats = set(json.load(io.open(STAT_INDEX, encoding="utf-8"))["names"])

    taken_ids, taken_names, taken_hexes = load_existing_ids_and_hexes()

    rows = read_tsv(SRC)
    dyes, rules, problems, notes = [], [], [], []
    seen_ids, seen_hexes = set(), {}
    low = 0

    for lineno, r in rows:
        at = "line %d" % lineno
        name = r["name"].strip()
        if not name:
            problems.append("%s: no name" % at)
            continue
        did = PREFIX + slug(name)
        hexv = r["hex"].strip().upper()

        if not HEX_RE.match(hexv):
            problems.append("%s: %r hex %r is not six hex digits"
                            % (at, name, r["hex"]))
            continue
        if r["rarity"] not in RARITIES:
            problems.append("%s: %r rarity %r is not one of %s. Material and "
                            "Dye Stamp are the metallic and store BUCKETS in "
                            "the grid, so a flat colour wearing one is filed "
                            "where nobody looks for it."
                            % (at, name, r["rarity"], ", ".join(sorted(RARITIES))))
            continue
        if did in taken_ids or did in seen_ids:
            problems.append("%s: id %r is already taken. An id collision does "
                            "not duplicate a colour, it hands one colour's "
                            "unlock rule to another." % (at, did))
            continue
        if name.lower() in taken_names:
            notes.append("%s: %r is also the name of %s. Legal, since the id is "
                         "the identity, but two swatches reading the same in "
                         "the grid is a curation bug."
                         % (at, name, taken_names[name.lower()]))
        if hexv in taken_hexes:
            notes.append("%s: %r is %s, the same colour as %s"
                         % (at, name, hexv, taken_hexes[hexv]))
        if hexv in seen_hexes:
            notes.append("%s: %r is %s, the same colour as %r earlier in this "
                         "file" % (at, name, hexv, seen_hexes[hexv]))

        kind = r["gate_kind"].strip()
        ref = r["gate_ref"].strip()
        clause = None

        if kind in ("quest", "locationCleared"):
            want = "QUST" if kind == "quest" else "LCTN"
            form = by_kind.get(want, {}).get(ref)
            if form is None:
                problems.append("%s: %s %r is not a %s the masters define, so a "
                                "rule naming it can never be satisfied and %r "
                                "would be locked for every character, forever"
                                % (at, kind, ref, want, name))
                continue
            if not form.get("name"):
                problems.append("%s: %s %r has no full name in the masters, so "
                                "the locked swatch could only name its plugin, "
                                "which is the uninformative line this whole "
                                "feature exists to remove" % (at, kind, ref))
                continue
            clause = ('{ "type": "%s", "plugin": "%s", "formId": "%s" }'
                      % (kind, form["plugin"], form["formId"]))
            comment = "%s (%s)" % (form["name"], r["confidence"])
        elif kind == "stat":
            if ref not in stats:
                # ⚠ THE NEAR MISS IS NAMED. This is where somebody has typed
                # "Thieves Guild" for "Thieves' Guild" and would otherwise spend
                # an evening wondering why one colour never arrives.
                close = sorted(
                    s for s in stats
                    if s.lower().replace("'", "").replace("the ", "") ==
                    ref.lower().replace("'", "").replace("the ", ""))
                hint = (". Did you mean %s?" % " or ".join(repr(c) for c in close)
                        if close else "")
                problems.append("%s: %r is not a misc stat the engine knows, so "
                                "Game.QueryStat answers 0 for it forever and %r "
                                "never unlocks. Compared exactly, apostrophes "
                                "and any leading \"The\" included%s"
                                % (at, ref, name, hint))
                continue
            try:
                m = int(r["gate_min"])
            except ValueError:
                problems.append("%s: stat gate needs a whole number gate_min, "
                                "not %r" % (at, r["gate_min"]))
                continue
            # A counter starts at 0 and only rises, so min 0 is "always" spelt
            # in a way that hides it: the colour would be free and permanent on
            # the first load.
            if m < 1:
                problems.append("%s: gate_min %d is met before anything is "
                                "done, so %r would be free" % (at, m, name))
                continue
            clause = ('{ "type": "deed", "deed": "%s%s", "min": %d }'
                      % (STAT_DEED_PREFIX, ref, m))
            comment = "%s %d (%s)" % (ref, m, r["confidence"])
        else:
            problems.append("%s: gate_kind %r is not quest, locationCleared or "
                            "stat" % (at, kind))
            continue

        if r["confidence"].strip().lower() == "low":
            low += 1
        # ---- the optional pearlescent finish -------------------------------
        #
        # ⚠ THE PREMIUM REWARDS WEAR A FINISH, NOT A BRIGHTER FLAT COLOUR
        # (user 2026-08-13). A questline ending, a Daedric artifact and a big
        # counter pay out one of these; everything else is flat.
        #
        # ⚠ AND A FINISH MOVES THE SWATCH OUT OF ITS RARITY GROUP. DyeBucketOf
        # files anything carrying a mode, a second stop, flake or a gloss under
        # "Metallic" whatever its rarity says, so these land beside the fr:
        # specials rather than among the flat Rares. That is the intent - the
        # player should find the earned finishes together - but it does mean
        # rarity here prices the unlock and no longer places the swatch.
        finish, bad = {}, None
        h2 = r["hex2"].strip().upper()
        mode = r["mode"].strip()
        sheen = r["sheen"].strip().upper()
        if h2 and not HEX_RE.match(h2):
            bad = "hex2 %r is not six hex digits" % r["hex2"]
        elif sheen and not HEX_RE.match(sheen):
            bad = "sheen %r is not six hex digits" % r["sheen"]
        elif mode and mode not in MODES:
            bad = "mode %r is not one of %s" % (mode, ", ".join(sorted(MODES)))
        elif h2 and not mode:
            bad = ("hex2 %s is set with no mode, so the loader keeps both stops "
                   "and never ramps between them: the swatch ships looking "
                   "exactly like a flat colour" % h2)
        elif mode and not h2:
            bad = ("mode %r is set with no hex2, so there is no second stop to "
                   "ramp to" % mode)
        if bad is None:
            for key, raw in (("gloss", r["gloss"]), ("flake", r["flake"])):
                if not raw.strip():
                    continue
                try:
                    v = int(raw)
                except ValueError:
                    bad = "%s %r is not a whole number" % (key, raw)
                    break
                if not 0 <= v <= 255:
                    bad = "%s %d is outside 0 to 255" % (key, v)
                    break
                finish[key] = v
        if bad is not None:
            problems.append("%s: %r %s" % (at, name, bad))
            continue
        if h2:
            finish["hex2"] = h2
            finish["mode"] = mode
        if sheen:
            finish["sheen"] = sheen

        seen_ids.add(did)
        seen_hexes[hexv] = name
        dyes.append((did, name, hexv, r["rarity"], r["family"], finish))
        rules.append((did, clause, comment))

    if problems:
        for p in problems:
            print("PROBLEM %s" % p)
        sys.exit("%d row(s) did not resolve; NOTHING WRITTEN. A partial write "
                 "here would ship a colour with no rule, which is free and "
                 "permanent." % len(problems))

    for n in notes:
        print("note %s" % n)

    # ---- the colours -------------------------------------------------------
    body = []
    for did, name, hexv, rarity, family, finish in dyes:
        # The key order the loader documents and pearl.json already uses, so a
        # diff between the two packs is about values rather than layout.
        extra = ""
        if "hex2" in finish:
            extra += ' "hex2": "%s", "mode": "%s",' % (finish["hex2"], finish["mode"])
        if "gloss" in finish:
            extra += ' "gloss": %d,' % finish["gloss"]
        if "sheen" in finish:
            extra += ' "sheen": "%s",' % finish["sheen"]
        if "flake" in finish:
            extra += ' "flake": %d,' % finish["flake"]
        body.append('    { "id": "%s", "name": "%s", "hex": "%s",%s '
                    '"rarity": "%s" },  // %s'
                    % (did, name, hexv, extra, rarity, family))
    body[-1] = body[-1].replace(" },  //", " }   //", 1)
    io.open(OUT_DYES, "w", encoding="utf-8", newline="\n").write(
        DYES_HEADER + "{\n  \"dyes\": [\n" + "\n".join(body) + "\n  ]\n}\n")

    # ---- what they cost ----------------------------------------------------
    body = []
    for did, clause, comment in sorted(rules):
        body.append('    "%s": [ %s ],  // %s' % (did, clause, comment))
    body[-1] = body[-1].replace(" ],  //", " ]   //", 1)
    io.open(OUT_RULES, "w", encoding="utf-8", newline="\n").write(
        RULES_HEADER + "{\n  \"dyes\": {\n" + "\n".join(body) + "\n  }\n}\n")

    kinds = {}
    for _, clause, _ in rules:
        k = ("stat" if '"deed"' in clause
             else "quest" if '"quest"' in clause else "locationCleared")
        kinds[k] = kinds.get(k, 0) + 1
    special = sum(1 for d in dyes if d[5])
    print("wrote %d colour(s) and %d rule(s): %s"
          % (len(dyes), len(rules),
             ", ".join("%d %s" % (v, k) for k, v in sorted(kinds.items()))))
    print("%d of the colours carry a pearlescent finish; %d are flat"
          % (special, len(dyes) - special))
    print("%d of them are marked low confidence and are free picks" % low)


DYES_HEADER = """// Fitting Room, the Skyrim colour pack.
//
// GENERATED by tools/make_skyrim_pack.py from tools/data/skyrim_dyes.tsv,
// together with ../Unlocks/skyrim.json. Edit the TSV, not either JSON.
//
// Colours named for Skyrim's own world and earned by doing Skyrim's own things,
// as against ../Dyes/eso.json, which mirrors ESO's palette and whose economy
// was mined out of ESO achievements. The comment after each line is the family
// the colour belongs to.
//
// ⚠⚠ THE ID IS DERIVED FROM THE NAME AND IS THE IDENTITY. Unlocks are stored by
// id, so renaming a colour here detaches every stored unlock of it and every
// garment already painted with it, on every save, with nothing logged. Add
// freely; rename nothing.
"""

RULES_HEADER = """// Fitting Room, what the Skyrim colour pack costs.
//
// GENERATED by tools/make_skyrim_pack.py from tools/data/skyrim_dyes.tsv,
// together with ../Dyes/skyrim.json. Edit the TSV, not either JSON.
//
// ⚠ ONE RULE PER COLOUR IN THIS FILE, ALWAYS, and it is not a coincidence: the
// generator writes exactly one clause per row. Every colour here is NEW, so
// there is no tier being displaced and nothing to be stricter than - a colour
// this file forgot would simply fall to its rarity tier and be reachable at a
// level, which is what the whole pack exists to avoid.
//
// ⚠ EVERY QUEST AND LOCATION ID WAS RESOLVED THROUGH tools/data/form_index.json
// AND EVERY "stat:" NAME THROUGH tools/data/stat_index.json, both generated
// from the game itself. The generator refuses to write a row it cannot resolve.
// That refusal is the only defence there is: an unresolvable form is never
// satisfied, and an unknown stat name answers int 0 through Game.QueryStat,
// which is byte for byte what an honest zero answers.
//
// ⚠ THE "stat:" VALUES ARRIVE ~180 ms AFTER THE ASK, on the Papyrus VM's own
// thread, so the promotion pass at kPostLoadGame reads them all as 0 and runs a
// second time when they land. A colour gated on one is earned on that second
// pass. See src/DyeStats.cpp.
"""


if __name__ == "__main__":
    main()
