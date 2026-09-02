"""Build tools/data/stat_index.json: every misc stat name the engine knows.

    python tools\\make_stat_index.py

WHY THIS EXISTS, AND IT IS THE SAME REASON make_form_index.py DOES. A rules
file can gate a colour on a Skyrim misc stat, which reaches the game through
Game.QueryStat, and a stat name the engine does not know returns int 0. Not an
error, not a log line, not a distinguishable value: the same 0 an honest zero
gives. So a typo locks its colour for the life of every character and nothing
anywhere says so. Measured 2026-08-08: 'Fitting Room Not A Real Stat' answered
int 0 beside two real reads in the same millisecond.

⚠⚠ AND THE NAMES ARE NOT WHAT YOU WOULD TYPE. Three of the eight questline
counters carry a word or a mark no one guesses:

    "The Companions Quests Completed"        not "Companions ..."
    "Thieves' Guild Quests Completed"        not "Thieves Guild ..."
    "The Dark Brotherhood Quests Completed"  not "Dark Brotherhood ..."

Each of those three was typed the obvious way first and each would have locked
its colours permanently. That is what this file buys.

HOW IT READS THEM. The names sit in one contiguous NUL-terminated run in
SkyrimSE.exe's read-only data, in the order the engine registers them. This
walks that run from an anchor rather than from a hardcoded offset, because an
offset is a fact about one build and the shape is a fact about the table. Two
anchors are required to agree on the same run before anything is written, so a
coincidental match on one string cannot produce an index.

⚠ THE EXE IS READ, NEVER WRITTEN, and nothing at runtime reads this file: it is
authoring-side only, consumed by tools/check_dye_rules.py.

⚠ WHAT THIS PROVES AND WHAT IT DOES NOT. Present in the table means the engine
knows the name, which is the failure this guards. It does NOT prove the counter
is one a character can raise, nor what exactly it counts. Those are questions
for the field, and the ones we gate on are logged with their values on the
first load so the answer arrives from a real save rather than from a guess.
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tools", "data", "stat_index.json")

DEFAULT_EXE = (r"C:\Program Files (x86)\Steam\steamapps\common"
               r"\Skyrim Special Edition\SkyrimSE.exe")

# The run's first and last entries. Both must be found, and both must land in
# the SAME walked run, or nothing is written: one anchor matching somewhere
# else in a 37 MB binary is a coincidence, two bracketing one run is the table.
#
# ⚠ THESE ARE THE ENDS OF THE TABLE, NOT A PREFERENCE. "Locations Discovered"
# is the first stat the engine registers and "StalhrimItemsCrafted" is the last
# of the Dragonborn additions. If a future build reorders them this refuses to
# write rather than writing a truncated index, which is the direction that
# fails safe: a missing index skips the check loudly, a short one passes a
# typo.
FIRST = b"Locations Discovered"
LAST = b"StalhrimItemsCrafted"

# A run ends at the first stretch of padding longer than this. The table's own
# entries are aligned to 8 and 16 bytes, so short NUL stretches are internal.
MAX_PAD = 24

# Anything outside this is not a stat name. The table is plain ASCII.
NAME_RE = re.compile(rb"^[A-Za-z][A-Za-z0-9 '\-]{2,63}$")


def walk_run(buf, start):
    """Every NUL-terminated ASCII string in the run containing offset start.

    Walks backward to the run's head and then forward to its tail, so the
    caller may anchor anywhere inside it. Returns [(offset, text)].
    """
    n = len(buf)

    # Backward to the head: step over the padding before each entry, then over
    # the entry itself, and stop when what precedes is not one.
    head = start
    while head > 0:
        p = head - 1
        pad = 0
        while p > 0 and buf[p] == 0 and pad <= MAX_PAD:
            p -= 1
            pad += 1
        if pad == 0 or pad > MAX_PAD:
            break
        end = p + 1                      # one past the previous string's text
        q = end
        while q > 0 and 0x20 <= buf[q - 1] <= 0x7E:
            q -= 1
        if q == end or not NAME_RE.match(buf[q:end]):
            break
        head = q

    out = []
    p = head
    while p < n:
        while p < n and buf[p] == 0:     # padding between entries
            p += 1
        q = p
        while q < n and 0x20 <= buf[q] <= 0x7E:
            q += 1
        if q == p or q >= n or buf[q] != 0:
            break
        text = buf[p:q]
        if not NAME_RE.match(text):
            break
        out.append((p, text.decode("ascii")))
        # The gap to the next entry is padding; a long one ends the run.
        r = q
        while r < n and buf[r] == 0 and r - q <= MAX_PAD + 1:
            r += 1
        if r - q > MAX_PAD:
            break
        p = r
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        sys.exit("no game executable at %s. Point --exe at SkyrimSE.exe."
                 % args.exe)
    buf = io.open(args.exe, "rb").read()

    first = buf.find(FIRST + b"\x00")
    last = buf.find(LAST + b"\x00")
    if first < 0 or last < 0:
        sys.exit("could not find %r and %r in %s, so the stat table is not "
                 "where this expects it. Nothing written."
                 % (FIRST.decode(), LAST.decode(), os.path.basename(args.exe)))

    run = walk_run(buf, first)
    names = [t for _, t in run]
    offsets = {t: o for o, t in run}

    # ⚠ BOTH ANCHORS IN ONE RUN, checked rather than assumed. This is the whole
    # guard against walking a different table that happens to start with the
    # same words.
    if FIRST.decode() not in offsets or LAST.decode() not in offsets:
        sys.exit("walked a run of %d string(s) from %r but it does not also "
                 "contain %r, so this is not the stat table. Nothing written."
                 % (len(names), FIRST.decode(), LAST.decode()))
    if offsets[LAST.decode()] < offsets[FIRST.decode()]:
        sys.exit("%r sits before %r in the walked run, which is not the table's "
                 "order. Nothing written." % (LAST.decode(), FIRST.decode()))

    # Duplicates would make "is this a stat" ambiguous and there are none in a
    # good read, so a repeat means the walk ran past the table into something
    # else. Refuse rather than dedupe: deduping hides exactly that.
    if len(set(names)) != len(names):
        dupes = sorted({t for t in names if names.count(t) > 1})
        sys.exit("the walked run repeats %s, so it is not one table. Nothing "
                 "written." % ", ".join(repr(d) for d in dupes))

    payload = {
        "source": os.path.basename(args.exe),
        "bytes": len(buf),
        "firstOffset": "%08X" % offsets[FIRST.decode()],
        "names": names,
    }
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(payload, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print("%d stat name(s) from %s at %s -> %s"
          % (len(names), payload["source"], payload["firstOffset"],
             os.path.relpath(args.out, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
