"""Turn tools/data/eso_dyes.tsv into a Fitting Room dye pack.

Writes dist/SKSE/Plugins/FittingRoom/Dyes/eso.json.

The pack carries five keys the loader reads today (id, name, hex, rarity, and
an omitted cost so [Lore] iDyeCost applies) plus one it does not: hue.
DyeFromJson reads only the keys it names and never validates the key set, so
hue rides along harmlessly.

esoUnlock is the sixth, and it is PROVENANCE that nothing evaluates. It names
an ESO achievement, which is not a Skyrim one. What a colour actually
requires is authored by hand in Unlocks/, checked by tools/check_dye_rules.py,
and has nothing to do with this column.

Ids are eso:<slug> and are the identity, so the slug rule must never change
once a save has stored an unlock against one. Run from the worktree root:

    python tools\\make_eso_palette.py
"""
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "data", "eso_dyes.tsv")
OUT = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Dyes",
                   "eso.json")

HEX_RE = re.compile(r"^[0-9A-F]{6}$")

# A rarity must be non-empty and carry no surrounding whitespace. Both failures
# are silent all the way to a player's save.
#
# A blank cell ships as "rarity": "", which is byte for byte what an absent key
# produces, and the loader reads that as no tier, which the unlock rules read as
# no requirement. Surrounding whitespace is worse than it looks: the tier lookup
# is an exact byte match, so "Dye Stamp " matches no tier and frees every colour
# in the bucket at once.
#
# Deliberately NOT a whitelist of the five known buckets. A pack is allowed to
# invent a rarity, and an unknown one staying free is the documented fallback;
# what is being caught here is a cell nobody filled in.
RARITY_RE = re.compile(r"^\S(?:.*\S)?$")


def slug(name):
    """Lowercase, non-alphanumerics to single dashes, no leading or trailing.

    Apostrophes vanish rather than becoming dashes, so "Dragonknights' Blood"
    is dragonknights-blood and not dragonknights--blood.
    """
    s = name.lower().replace("'", "").replace("’", "")
    s = re.sub(r"[^a-z0-9]+", "-", s)
    return s.strip("-")


def main():
    with io.open(SRC, "r", encoding="utf-8", newline="") as f:
        lines = [l.rstrip("\r\n") for l in f if l.strip()]

    header = lines[0].split("\t")
    if header != ["name", "display_name", "hue", "rarity", "hex",
                  "eso_achievement"]:
        sys.exit("unexpected header: %r" % (header,))

    dyes, seen, shown = [], {}, {}
    for lineno, line in enumerate(lines[1:], start=2):
        parts = line.split("\t")
        if len(parts) != 6:
            sys.exit("line %d has %d fields, expected 6" % (lineno, len(parts)))
        name, display, hue, rarity, hexv, ach = parts

        if not HEX_RE.match(hexv):
            sys.exit("line %d: %r is not six uppercase hex digits" % (lineno, hexv))

        if not RARITY_RE.match(rarity):
            sys.exit("line %d: rarity %r is blank or has surrounding "
                     "whitespace. A blank one ships as \"rarity\": \"\", which "
                     "is what an absent key looks like, and the unlock rules "
                     "read that as no requirement: the colour is free at level "
                     "1 and permanent once a save stores it."
                     % (lineno, rarity))

        did = "eso:" + slug(name)
        if did in seen:
            sys.exit("line %d: id %r collides with line %d (%r vs %r)"
                     % (lineno, did, seen[did][0], seen[did][1], name))
        seen[did] = (lineno, name)

        # ⚠⚠ THE SHIPPED NAME AND THE ID COME FROM DIFFERENT COLUMNS, AND
        # THAT SPLIT IS THE WHOLE REASON THIS COLUMN EXISTS. The id is
        # eso:<slug of name>, and DyePalette.h stores an unlock BY ID, so
        # editing the column that feeds the slug silently detaches every
        # unlock a player has earned and every garment they have painted.
        # name is therefore frozen for the life of the pack and is never
        # shown to anybody; display_name is what players read and is free to
        # change. An empty display_name means the two agree.
        label = display if display else name
        if label in shown:
            sys.exit("line %d: display name %r is already used on line %d. Two "
                     "colours with one name are indistinguishable in the "
                     "palette, and the rename pass is exactly where that "
                     "happens: 'Rank 10 Materials' was going to become "
                     "'Daedric Red', which another colour already ships."
                     % (lineno, label, shown[label]))
        shown[label] = lineno

        entry = {"id": did, "name": label, "hex": hexv, "hue": hue,
                 "rarity": rarity}
        # PROVENANCE, NEVER EVALUATED, and the name says so. It records the
        # ESO achievement that handed the colour over there, which is not a
        # Skyrim one and cannot be a rule here. Under its old name, "unlock",
        # it read like the thing that gates the dye, sitting one key away from
        # the rarity that really does.
        #
        # Dye Stamps are Crown Store purchases in ESO, not achievement
        # rewards, so there is no achievement to record for them. Null rather
        # than the literal "Dye Stamps only", which is a UESP note and not the
        # name of anything.
        entry["esoUnlock"] = None if ach == "Dye Stamps only" else ach
        dyes.append(entry)

    # ⚠⚠ THE BASELINE CHECK, AND IT HAS TO RUN BEFORE THE WRITE. Every id in
    # the pack that is already shipped must still be here. An id that vanishes
    # is not a cosmetic diff: it is every save that earned that colour losing
    # it, permanently and silently, because unlocks are stored by id and are
    # add-only. Renaming through display_name cannot trip this, which is the
    # point of proving it on every run rather than trusting the rule.
    #
    # A NEW id is fine and passes: adding colours is what this file is for.
    if os.path.exists(OUT):
        with io.open(OUT, "r", encoding="utf-8") as f:
            before = {d["id"] for d in json.load(f).get("dyes", [])}
        after = {d["id"] for d in dyes}
        lost = sorted(before - after)
        if lost:
            sys.exit(
                "%d id(s) in the shipped pack are not in this build, and an id "
                "is how a save remembers an earned colour. Every save holding "
                "one loses it silently. Did a name in the 'name' column change? "
                "That column feeds the slug and must never move; put the new "
                "wording in 'display_name'. Missing: %s"
                % (len(lost), ", ".join(lost[:10]) +
                   (", and %d more" % (len(lost) - 10) if len(lost) > 10 else "")))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"dyes": dyes}, f, indent=2, ensure_ascii=False)
        f.write("\n")

    by_rarity = {}
    for d in dyes:
        by_rarity[d["rarity"]] = by_rarity.get(d["rarity"], 0) + 1
    print("wrote %s" % OUT)
    print("%d dyes, %d bytes" % (len(dyes), os.path.getsize(OUT)))
    for r in sorted(by_rarity):
        print("  %-10s %d" % (r, by_rarity[r]))
    print("%d name an ESO achievement, %d do not"
          % (sum(1 for d in dyes if d["esoUnlock"]),
             sum(1 for d in dyes if not d["esoUnlock"])))


if __name__ == "__main__":
    main()
