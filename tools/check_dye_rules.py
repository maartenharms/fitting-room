"""Check the shipped unlock rules against the shipped dye packs.

    python tools\\check_dye_rules.py

Exits non-zero and names every problem it found. Point it somewhere else to
check a candidate file before it is copied in:

    python tools\\check_dye_rules.py --rules some\\dir --dyes some\\other\\dir

WHY THIS EXISTS AT ALL. A dye resolves to its own entry in "dyes", else to its
rarity's entry in "tiers", else to NO RULE, and no rule is an empty condition
list, which AllSatisfied answers true for. So every way of losing a rule ends
at the same place: the colour is free at level 1, promotion writes it into the
character's co-save, and unlocks are add only, so fixing the file afterwards
does not take it back on anyone who loaded the broken build. Locking is the
recoverable direction; freeing is not. Every check below guards the direction
that cannot be taken back.

The plugin cannot make these checks itself. An unmatched rarity, an override
key no dye claims and a threshold a new character already meets are all
legitimate things for a THIRD PARTY rules pack to contain, so the loader
reports them and gates nothing, deliberately. This is the authoring side,
where they are all bugs.

⚠ THIS SCRIPT IS NOT THE LOADER AND MUST NEVER BE MORE PERMISSIVE THAN IT.
Python's json is a different parser from jsoncpp and every place the two
disagree has cost something: duplicate keys, integral reals, and mins past
2^32-1 are each handled here for that reason and each is commented where it
happens. The invariants the LOADER itself proves against the shipped files
live in tests/test_dyerules.cpp, which runs on every build with no Python at
all; this covers the authoring mistakes a load cannot see.

A script rather than a note in a commit body, for the reason the same mistake
was already made once here: a check run once by hand does not survive the next
regeneration of eso.json, and does not exist for whoever edits the rules next.
"""
import argparse
import glob
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DYES = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Dyes")
RULES = os.path.join(ROOT, "dist", "SKSE", "Plugins", "FittingRoom", "Unlocks")
# make_form_index.py builds this by reading the game's own masters, so it is the
# only thing on this machine that can tell a real editor id from an invented
# one. Absent is not a failure: a checkout without it skips CHECK M and says so.
FORM_INDEX = os.path.join(ROOT, "tools", "data", "form_index.json")
CONDITIONS = os.path.join(ROOT, "src", "DyeConditions.cpp")
UNLOCKS_H = os.path.join(ROOT, "src", "DyeUnlocks.h")
# make_stat_index.py builds this by reading SkyrimSE.exe, so it is the only
# thing on this machine that can tell a real misc stat name from an invented
# one. Absent is not a failure: a checkout without it skips CHECK S and says so.
STAT_INDEX = os.path.join(ROOT, "tools", "data", "stat_index.json")

# What separates a Skyrim misc stat from one of Fitting Room's own counters
# inside the one "deed" clause kind. Must stay byte-identical to
# kStatDeedPrefix in src/DyeStatDeed.h.
STAT_DEED_PREFIX = "stat:"

# The two top level keys DyeRuleSet::MergeFromJson reads. The schema is CLOSED:
# a third key fails the whole file, which freezes promotion until it is
# removed, and that is the only way a section misspelled "dye" can be detected.
ALLOWED_ROOT_KEYS = {"tiers", "dyes"}

# The ONE key allowed to gate nothing, as (section, key).
#
# Common is deliberately [{"type":"always"}]: 64 colours the design hands out
# at character creation. Every other key that gates nothing is the shape a bad
# edit leaves behind, and it frees a whole rarity at once. This used to be a
# note anywhere at all, which let a tier softened to "always" or to [] pass a
# green check: measured on the shipped tree, "Rare": [{"type":"always"}] takes
# a level 1 character's grant from 66 to 150 and "Uncommon": [] takes it to
# 163, and every one of those is written permanently into the co-save.
#
# Narrow rather than removed, because the reason the note existed is still
# right: a script a release has to be run around is worth less than every check
# in it. One allowlisted key costs nothing and refuses all the rest.
FREE_BY_DESIGN = {("tiers", "Common")}

# jsoncpp's Value::isUInt() is what the loader gates every "min" on, and it is
# true for a REAL whose value is integral and in range. So 35.0 loads as 35 and
# refusing it here would be a false alarm that sends an author chasing a
# working file. Measured: "min": 35.0 loads with rulesRejected 0 and gates at
# 35 exactly like the integer.
#
# The upper bound is the half that matters. Python integers are unbounded, so a
# min past 2^32-1 reads fine here and is REFUSED by the loader, which turns the
# rule into kNever and LOCKS its colour. Measured: "Rare": level 4294967296
# passed this script and took a level 40 character from 251 colours to 167.
MAX_MIN = 4294967295

# A character starts every skill at 15 and racial bonuses add up to 10 more, so
# 25 is the highest base a level 1 character can have. A threshold at or below
# that is met at character creation and granted permanently on the first load,
# which is a gate that reads like one and is not.
MIN_SKILL_THRESHOLD = 26

# Skyrim starts everyone at level 1, so a "level" gate has to ask for 2 before
# it asks for anything. Same shape as the skill floor.
MIN_LEVEL_THRESHOLD = 2

# A deed counter starts at 0 and only ever goes up, so min 0 is "always" spelt
# in a way that hides it.
MIN_DEED_THRESHOLD = 1


def strip_json_comments(text):
    """Remove // and /* */ comments, leaving string literals alone.

    jsoncpp keeps allowComments on and DyeRules::Load leaves it on, so the
    shipped rules file is commented with real comments rather than a fake
    "_comment" key, which the closed schema would reject. Python's json does
    not accept them, so they come out here.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                elif text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            stop = n if end < 0 else end + 2
            # ⚠ Keep the newlines, which this branch CLAIMED to do and did not
            # until 2026-08-02: it emitted nothing at all, so every line after a
            # multi-line /* */ comment was reported at the wrong line number and
            # an author was sent to the wrong place in their own file. The line
            # branch above needs no equivalent, since it stops at the newline
            # and leaves it to be copied out normally.
            out.append("\n" * text.count("\n", i, stop))
            i = stop
            continue
        out.append(c)
        i += 1
    return "".join(out)


def skill_names():
    """The eighteen skill names a rules file may write, read out of the C++.

    Deliberately NOT a second hand-typed list. SkillByName is the only thing
    that decides which names parse, and a copy of it here would be one more
    pair of facts that can disagree.
    """
    src = io.open(CONDITIONS, encoding="utf-8").read()
    block = re.search(r"kSkills\{(.*?)\}\s*\}\s*;", src, re.S)
    if not block:
        sys.exit("could not find the kSkills table in %s" % CONDITIONS)
    names = re.findall(r'\{\s*"([A-Za-z]+)"\s*,\s*\d+\s*\}', block.group(1))
    if len(names) != 18:
        sys.exit("expected 18 skills in %s, found %d" % (CONDITIONS, len(names)))
    return set(names)


def deed_names():
    """The deed names a rules file may write, read out of the C++.

    Same argument the skill table gets, for a mistake with a worse ending. A
    deed clause naming a counter nothing produces is not rejected by the
    loader: the name parses, Satisfied reads the missing counter as 0, and the
    colour LOCKS silently for as long as the min is above zero. Measured on the
    shipped tree with the counter wired: "channelsdyed" for "channelsDyed"
    takes a maxed character from 318 colours to 286, and neither the log nor
    this script said a word about it.

    The producers are the kDeed* constants in DyeUnlocks.h. Anything a rules
    file spells that is not one of them counts nothing, forever.
    """
    src = io.open(UNLOCKS_H, encoding="utf-8").read()
    names = re.findall(
        r'constexpr\s+const\s+char\*\s+kDeed\w+\s*=\s*"([^"]+)"\s*;', src)
    if not names:
        sys.exit("could not find any kDeed* constant in %s" % UNLOCKS_H)
    return set(names)


def stat_names():
    """The Skyrim misc stat names a rules file may write, read out of the game.

    ⚠⚠ THIS IS THE ONLY PLACE A WRONG STAT NAME IS CATCHABLE AT ALL. A deed
    named "stat:<something>" is dispatched to Game.QueryStat, and a name the
    engine does not know answers int 0. Not an error, not a null, not a log
    line: byte for byte the answer an honest zero gives. So the colour locks for
    the life of every character and nothing anywhere says so. Measured
    2026-08-08 with a deliberately bogus control beside two real reads.

    ⚠⚠ AND THE NAMES ARE NOT WHAT ANYONE TYPES. Three of the eight questline
    counters carry a word or a mark nobody guesses:

        "The Companions Quests Completed"        not "Companions ..."
        "Thieves' Guild Quests Completed"        not "Thieves Guild ..."
        "The Dark Brotherhood Quests Completed"  not "Dark Brotherhood ..."

    All three were typed the obvious way on the first authoring pass and all
    three would have shipped a colour nobody could ever earn.

    Same argument the skill table and the form index get, and the same shape:
    generated from the game rather than kept by hand. Absent is not a failure, a
    checkout without the index skips this and says so - the index is built from
    a file only a machine with the game installed has.
    """
    if not os.path.exists(STAT_INDEX):
        return None
    with io.open(STAT_INDEX, encoding="utf-8") as fh:
        return set(json.load(fh).get("names", []))


def split_stat_deed(name):
    """The stat inside a deed name, or None for one of our own counters.

    ⚠ THE SAME RULE AS StatNameFromDeed IN src/DyeStatDeed.h, INCLUDING THE
    REFUSAL TO TRIM. If this tidied a name the C++ passes through, the script
    would bless a string the engine then fails to recognise, which is the exact
    failure it exists to prevent. A leading space inside the name is a typo that
    has to fail HERE, loudly, rather than in a player's save, silently.
    """
    if not isinstance(name, str) or not name.startswith(STAT_DEED_PREFIX):
        return None
    rest = name[len(STAT_DEED_PREFIX):]
    return rest or None


def no_duplicate_keys(pairs):
    """object_pairs_hook that refuses a repeated key.

    DyeRules::Load sets rejectDupKeys, so jsoncpp FAILS a file that repeats
    one and progression freezes until it is fixed. Python's json does the
    opposite by default and quietly keeps the LAST value, so without this hook
    the most copy pasted thing in a rules file, a second "tiers" block, would
    read clean here and refuse to load in the game. Checking the shipped file
    with a more forgiving parser than the one that ships is how a check comes
    to mean nothing.
    """
    seen, out = set(), {}
    for k, v in pairs:
        if k in seen:
            raise ValueError("duplicate key %r. The reader sets rejectDupKeys, "
                             "so this fails the whole file and freezes "
                             "progression" % k)
        seen.add(k)
        out[k] = v
    return out


def load_json(path):
    raw = io.open(path, encoding="utf-8-sig").read()
    return json.loads(strip_json_comments(raw),
                      object_pairs_hook=no_duplicate_keys)


# ABSENT IS NOT null, and .get() cannot tell them apart. A pack that never
# wrote a "rarity" is saying "free", which vanilla.json's twelve rely on. One
# that wrote "rarity": null wrote the key and got it wrong, which DyePalette
# counts as a dropped rarity for exactly that reason. Collapsing the two here
# would give the second a clean bill of health.
ABSENT = object()


def load_packs(dyes_dir, problems):
    """Every dye in every pack, as (id, rarity, source file).

    ⚠ A pack that does not parse is a NAMED refusal, not a traceback. It was a
    bare load_json until 2026-08-02, so a duplicate key in a Dyes file, the one
    thing the hook above exists to catch, came out as a stack trace from inside
    the json module with the pack's name nowhere in it. The rules side already
    got the named message; both sides matter equally, since a pack that fails
    here is a pack whose rarities go unchecked.
    """
    entries, files = [], sorted(glob.glob(os.path.join(dyes_dir, "*.json")))
    if not files:
        sys.exit("no dye packs in %s" % dyes_dir)
    for path in files:
        name = os.path.basename(path)
        try:
            root = load_json(path)
        except ValueError as e:
            problems.append("%s: does not parse, %s" % (name, e))
            continue
        if not isinstance(root, dict):
            problems.append("%s: the root is not an object" % name)
            continue
        for dye in root.get("dyes", []):
            if not isinstance(dye, dict):
                problems.append("%s: a dye entry is %s, not an object"
                                % (name, type(dye).__name__))
                continue
            rarity = dye["rarity"] if "rarity" in dye else ABSENT
            entries.append((dye.get("id"), rarity, name))
    return entries


def parse_min(value):
    """A "min" as the LOADER reads it, or None.

    ConditionFromJson gates every min on jsoncpp's Value::isUInt(), which is
    true for an integer in [0, 2^32-1] AND for a REAL whose value is integral
    and in that range. Both halves of that were wrong here: 35.0 was refused
    although it loads and gates at 35, and a min past 2^32-1 was accepted
    although the loader refuses the rule and LOCKS the colour.
    """
    if isinstance(value, bool):
        return None                      # JSON true is not 1 to jsoncpp
    if isinstance(value, float):
        if not value.is_integer():
            return None
        value = int(value)
    if not isinstance(value, int):
        return None
    if value < 0 or value > MAX_MIN:
        return None
    return value


def gates_nothing(conds):
    """Whether a rule hands out every colour it covers.

    ⚠ PER RULE, NOT PER CLAUSE, which is what it was until 2026-08-02. Every
    clause has to hold, so one "always" beside a real gate frees nothing at
    all, and saying "nothing gates it" about [{"type":"always"},
    {"type":"level","min":35}] was simply false: that rule gates at 35.
    """
    return all(isinstance(c, dict) and c.get("type") == "always" for c in conds)


def dimension(cond):
    """What a clause gates ON, or None for a clause that gates nothing.

    Two clauses share a dimension when they can be ordered by their min alone:
    the same skill, the same deed, the same quest, or both being level. Nothing
    else in here is comparable, and see check_strictness for why that is said
    rather than guessed at.
    """
    if not isinstance(cond, dict):
        return None
    kind = cond.get("type")
    if kind == "level":
        return ("level",)
    if kind == "skill":
        return ("skill", cond.get("skill"))
    if kind == "deed":
        return ("deed", cond.get("deed"))
    if kind in ("quest", "locationCleared"):
        # ⚠ THE KIND IS PART OF THE DIMENSION. A quest and a location can carry
        # the same plugin and local id, and they are not the same requirement,
        # so folding them together would let one be compared against the other
        # as though it were the same axis.
        form = cond.get("formId")
        form = form.lower().lstrip("0x") if isinstance(form, str) else form
        return (kind, cond.get("plugin"), form)
    return None                          # "always", and anything unparseable


def describe(cond):
    """One clause in the words a rules file writes it in."""
    if not isinstance(cond, dict):
        return repr(cond)
    kind = cond.get("type")
    if kind == "level":
        return "level %s" % cond.get("min")
    if kind == "skill":
        return "%s %s" % (cond.get("skill"), cond.get("min"))
    if kind == "deed":
        return "deed %s %s" % (cond.get("deed"), cond.get("min"))
    if kind == "quest":
        return "quest %s %s" % (cond.get("plugin"), cond.get("formId"))
    if kind == "locationCleared":
        return "cleared %s %s" % (cond.get("plugin"), cond.get("formId"))
    return str(kind)


def check_conditions(name, section, key, conds, skills, deeds, stats, problems,
                     notes):
    """One rule's clauses, against what ConditionFromJson actually accepts.

    A clause this rejects becomes kNever in the plugin, which LOCKS the colour,
    so most of these cannot free anything. They are here because a lock nobody
    meant is still a bug, and because the log line that would have said so
    arrives on a player's machine rather than this one.

    THE TWO SPELLINGS OF "DELIBERATELY FREE" ARE REFUSED EVERYWHERE EXCEPT ONE
    KEY. [] and a rule made only of "always" both hand out every colour they
    cover, permanently, and the Common tier is the one place the design means
    it. This was a note anywhere at all until 2026-08-02, on the argument that
    a script a release has to be run around is worth less than every check in
    it. That argument was right and its scope was not: measured on the shipped
    tree, "Rare": [{"type":"always"}] passes with exit 0 and takes a level 1
    character from 66 colours to 150, and "Uncommon": [] passes and takes it to
    163. Allowlisting the one key the design actually wants keeps the shipped
    file green and refuses every other spelling of the same edit.
    """
    where = "%s: %s %r" % (name, section[:-1], key)
    allowed_free = (section, key) in FREE_BY_DESIGN
    if not isinstance(conds, list):
        problems.append("%s: rule is %s, not a list"
                        % (where, type(conds).__name__))
        return
    if not conds or gates_nothing(conds):
        how = "empty" if not conds else "nothing but \"always\""
        if allowed_free:
            notes.append("%s: rule is %s, so nothing gates it. This is the one "
                         "key the design means that for." % (where, how))
        else:
            problems.append("%s: rule is %s, so every colour it covers is free "
                            "at level 1 and permanent once a save has stored "
                            "it. Only %s may gate nothing."
                            % (where, how,
                               ", ".join("%s %r" % (s[:-1], k)
                                         for s, k in sorted(FREE_BY_DESIGN))))
        return
    # ⚠⚠ ONE REQUIREMENT PER COLOUR, AND A NAMED DEED OUTRANKS A NUMBER
    # (user 2026-08-11). A rule is all_of, so pairing "Smithing 82" with "Hail
    # Sithis!" makes the swatch recite two lines, and the number is always the
    # duller of the two: nobody remembers a colour as a skill level, they
    # remember it as the one the Brotherhood pays you. Ten shipped rules were
    # written that way and all ten dropped their skill clause.
    #
    # ⚠ THIS REFUSES A SHAPE, NOT A LOOSENING. Both halves are legitimate on
    # their own; the pack author has to pick which one the colour is ABOUT.
    # Two quests, or a level and a skill, are left alone - the fault named here
    # is a number standing behind a deed and adding nothing a player would
    # repeat back to you.
    SPECIFIC = ("quest", "locationCleared", "deed")
    NUMERIC = ("level", "skill")
    kinds = [c.get("type") for c in conds if isinstance(c, dict)]
    named = [k for k in kinds if k in SPECIFIC]
    numeric = [k for k in kinds if k in NUMERIC]
    if named and numeric:
        problems.append(
            "%s: names %s AND %s in one rule, and every clause must hold, so "
            "the locked swatch recites both. Keep the %s and drop the %s: one "
            "requirement per colour."
            % (where, "/".join(sorted(set(named))), "/".join(sorted(set(numeric))),
               "/".join(sorted(set(named))), "/".join(sorted(set(numeric)))))

    for i, c in enumerate(conds):
        at = "%s clause %d" % (where, i)
        if not isinstance(c, dict):
            problems.append("%s: not an object" % at)
            continue
        kind = c.get("type")
        if kind == "always":
            # It frees nothing on its own here, because gates_nothing above
            # already handled the rule that is only "always". Still worth a
            # line: it is dead weight, and it is what half of a bad edit that
            # WOULD have freed the rule looks like.
            notes.append("%s: \"always\" beside real clauses, which gates "
                         "nothing and is not what frees the rule. Every clause "
                         "must hold, so this one is dead weight." % at)
        elif kind == "level":
            m = parse_min(c.get("min"))
            if m is None:
                problems.append("%s: level needs an integer min the loader can "
                                "read, 0 to %d. jsoncpp refuses anything else "
                                "and the rule becomes kNever, which LOCKS the "
                                "colour." % (at, MAX_MIN))
            elif m < MIN_LEVEL_THRESHOLD:
                problems.append("%s: level min %d is met at character creation, "
                                "so the colour is free and permanent on the "
                                "first load" % (at, m))
        elif kind == "skill":
            name_, m = c.get("skill"), parse_min(c.get("min"))
            if name_ not in skills:
                problems.append("%s: %r is not one of Skyrim's eighteen skills, "
                                "so the whole rule is rejected and the colour "
                                "locks" % (at, name_))
            if m is None:
                problems.append("%s: skill needs an integer min the loader can "
                                "read, 0 to %d. jsoncpp refuses anything else "
                                "and the rule becomes kNever, which LOCKS the "
                                "colour." % (at, MAX_MIN))
            elif m < MIN_SKILL_THRESHOLD:
                problems.append("%s: skill min %d is at or below the highest "
                                "base a level 1 character can have (15 plus up "
                                "to 10 racial), so the colour is free and "
                                "permanent on the first load" % (at, m))
        elif kind == "deed":
            name_, m = c.get("deed"), parse_min(c.get("min"))
            # ⚠ THE NAME IS CHECKED AGAINST THE PRODUCERS, not merely for being
            # a non-empty string. A deed clause naming a counter nothing writes
            # parses fine, reads 0 forever, and LOCKS its colour in silence.
            # That is the one failure in this file with no log line anywhere:
            # the loader cannot know which names have producers and does not
            # try.
            #
            # ⚠⚠ AND THERE ARE TWO PRODUCERS BEHIND THE ONE CLAUSE KIND. A name
            # starting "stat:" is Skyrim's, fetched through Game.QueryStat;
            # anything else is ours, counted in DyeUnlocks. Both answer 0 for a
            # name they do not know and both lock the colour, so both vocabu-
            # laries are checked, each against its own index.
            if not isinstance(name_, str) or not name_:
                problems.append("%s: deed needs a non-empty name" % at)
            elif name_ == STAT_DEED_PREFIX:
                problems.append("%s: %r names no stat. The engine answers 0 for "
                                "an empty name exactly as it does for a real "
                                "counter sitting at zero, so this locks its "
                                "colour and logs nothing." % (at, name_))
            elif split_stat_deed(name_) is not None:
                # CHECK S. The stat name, byte for byte, against the table read
                # out of the game's own executable.
                stat = split_stat_deed(name_)
                if stats is None:
                    notes.append("%s: %r is a Skyrim stat and there is no "
                                 "tools/data/stat_index.json to check it "
                                 "against, so CHECK S is skipped for it. Run "
                                 "tools/make_stat_index.py." % (at, stat))
                elif stat not in stats:
                    # ⚠ THE NEAR MISSES ARE NAMED, because this is exactly where
                    # an author has typed "Thieves Guild" for "Thieves' Guild"
                    # and is about to spend an hour wondering why one colour
                    # never unlocks.
                    close = sorted(
                        s for s in stats
                        if s.lower().replace("'", "").replace("the ", "") ==
                        stat.lower().replace("'", "").replace("the ", ""))
                    hint = (". Did you mean %s?" % " or ".join(repr(c) for c in close)
                            if close else "")
                    problems.append(
                        "%s: %r is not a misc stat the engine knows, so "
                        "Game.QueryStat answers 0 for it forever and the colour "
                        "never unlocks. The name is compared exactly, "
                        "apostrophes and any leading \"The\" included%s"
                        % (at, stat, hint))
            elif name_ not in deeds:
                problems.append("%s: %r is not a deed anything counts (%s "
                                "defines %s), and it does not start %r either, "
                                "so it reads 0 forever and the colour never "
                                "unlocks"
                                % (at, name_, os.path.basename(UNLOCKS_H),
                                   ", ".join(sorted(deeds)), STAT_DEED_PREFIX))
            if m is None:
                problems.append("%s: deed needs an integer min the loader can "
                                "read, 0 to %d. jsoncpp refuses anything else "
                                "and the rule becomes kNever, which LOCKS the "
                                "colour." % (at, MAX_MIN))
            elif m < MIN_DEED_THRESHOLD:
                problems.append("%s: deed min %d is met before anything is "
                                "done" % (at, m))
        elif kind in ("quest", "locationCleared"):
            # ⚠ NEITHER CAN BE MET AT CHARACTER CREATION, which is why there is
            # no threshold check here to match the level and deed arms. A quest
            # is not complete and a location is not cleared on a new save, so
            # the only thing that can be wrong is the reference itself.
            #
            # ⚠ AND THE REFERENCE IS NOT RESOLVED HERE. This script does not
            # read the game's masters, so it cannot tell a real editor id from
            # an invented one. tools/make_lore_unlocks.py does that against
            # form_index.json and refuses to write a row it cannot resolve,
            # which is where a wrong id is actually caught.
            plugin, form = c.get("plugin"), c.get("formId")
            if not isinstance(plugin, str) or not plugin:
                problems.append("%s: %s needs a non-empty plugin" % (at, kind))
            if not isinstance(form, str) or not re.match(
                    r"^(0[xX])?[0-9a-fA-F]{1,8}$", form or ""):
                problems.append("%s: %s formId %r is not hex, optionally "
                                "0x prefixed, of at most eight digits"
                                % (at, kind, form))
        elif kind == "never":
            problems.append("%s: \"never\" is refused by the loader on purpose" % at)
        else:
            problems.append("%s: unknown type %r, so the whole rule is rejected "
                            "and the colour locks" % (at, kind))


def load_form_index(path, notes):
    """Editor ids and full names for every form make_form_index.py could read.

    Returns a dict keyed by (plugin lowercased, form id with leading zeroes
    stripped), or None when the index is not on this machine.
    """
    if not os.path.exists(path):
        notes.append("no tools/data/form_index.json, so CHECK M is skipped and "
                     "no quest or location id is verified here. Run "
                     "tools/make_form_index.py to get it back.")
        return None
    out = {}
    with io.open(path, encoding="utf-8") as fh:
        for entry in json.load(fh):
            plugin = (entry.get("plugin") or "").lower()
            form = (entry.get("formId") or "").upper().lstrip("0") or "0"
            out[(plugin, form)] = entry
    return out


def check_form_refs(name, section, key, conds, index, problems):
    """CHECK M. Every quest and location id names a real form of the right kind.

    ⚠⚠ THIS SCRIPT USED TO REFUSE TO DO THIS ON PURPOSE, and what changed is
    what the ids are FOR. They used to be a second clause behind a skill level,
    so a wrong one cost a colour nobody could reach and nothing else. Since
    2026-08-11 a quest or a location is the WHOLE rule and the locked swatch
    NAMES it, so one wrong id now costs two things at once: the colour is
    unreachable for the life of every character, and the tooltip quietly falls
    back to naming the plugin instead of the quest, which reads as the feature
    working badly rather than as a broken rule.

    ⚠ THE KIND IS CHECKED, NOT ONLY THE ID. A "quest" clause pointing at a
    location resolves through LookupForm<TESQuest> to nothing, so it is never
    satisfied and never named, and both halves fail silently. The two id spaces
    overlap freely, so this is not a hypothetical.

    ⚠ AND A FORM WITH NO FULL NAME IS A PROBLEM HERE TOO. Skyrim has plenty of
    unnamed system quests. Gating on one is legitimate and naming it is not
    possible, so the swatch falls back to its plugin - which is exactly the
    uninformative line this feature exists to remove.
    """
    if index is None:
        return
    where = "%s: %s %r" % (name, section[:-1], key)
    for i, c in enumerate(conds):
        if not isinstance(c, dict):
            continue
        kind = c.get("type")
        if kind not in ("quest", "locationCleared"):
            continue
        plugin, form = c.get("plugin"), c.get("formId")
        if not isinstance(plugin, str) or not isinstance(form, str):
            continue                    # already reported by check_conditions
        at = "%s clause %d" % (where, i)
        want = "QUST" if kind == "quest" else "LCTN"
        entry = index.get((plugin.lower(),
                           form.upper().replace("0X", "").lstrip("0") or "0"))
        if entry is None:
            problems.append("%s: %s %s is not a form the game's masters "
                            "define, so it can never be satisfied and the "
                            "colour locks for the life of every character"
                            % (at, plugin, form))
        elif entry.get("kind") != want:
            problems.append("%s: %s %s is a %s (%s), and a %r clause looks up a "
                            "%s, so it resolves to nothing"
                            % (at, plugin, form, entry.get("kind"),
                               entry.get("edid"), kind, want))
        elif not entry.get("name"):
            problems.append("%s: %s %s (%s) has no name in the masters, so the "
                            "locked swatch can only name its plugin"
                            % (at, plugin, form, entry.get("edid")))


def check_strictness(key, rarity, conds, tier, problems, unordered):
    """An override against the tier it displaces.

    ⚠ THE SAFETY ARGUMENT FOR ORPHANED OVERRIDE KEYS RESTS ON THIS AND ON
    NOTHING ELSE. A per dye key that no palette id claims falls back to its
    rarity tier, and the plugin reports that and gates nothing on it, on the
    stated grounds that "every shipped override is stricter than the tier it
    overrides, so an orphan is always a downgrade". That was written in three
    headers and enforced in no code. Measured: rewriting eso:void-pitch, a Rare
    whose tier is level 35, from deed 100 to level 2 passed with exit 0 and the
    real loader honoured it.

    WHAT "STRICTER" MEANS HERE, since it is not a total order. A tier clause
    and an override clause are compared only when they gate the same DIMENSION:
    both level, the same skill, the same deed, or the same quest. On a shared
    dimension the override's min must be at least the tier's, and falling short
    is a REFUSAL.

    A dimension the override does not mention at all is REPORTED AND NOT
    REFUSED. "Smithing 100" against "level 35" cannot be ordered without a
    model of how fast a character levels, which is a design judgement this
    script has no business making and would get wrong for a crafting build.
    Most shipped overrides are exactly that shape, so refusing them would mean
    either changing rules nobody asked to change or adding a suppression list,
    and a suppression list is how a check comes to mean nothing. They are
    listed once, together, so the claim above reads as what it is: proven on
    the dimensions that can be compared, and taken on trust elsewhere.

    Deliberately not a count. This sentence said "nineteen" from when there
    were 29 overrides until there were 220 and 63 of that shape, which is the
    same way a comment starts describing code that no longer exists. The run
    prints the real number every time, so stating it here buys nothing and
    goes stale on its own.
    """
    if not isinstance(conds, list) or not isinstance(tier, list):
        return
    for tc in tier:
        dim = dimension(tc)
        if dim is None:
            continue                     # "always" imposes nothing to clear
        matched = [c for c in conds if dimension(c) == dim]
        if not matched:
            unordered.append("%r (%s) gates on %s, its tier gates on %s"
                             % (key, rarity,
                                " and ".join(describe(c) for c in conds),
                                describe(tc)))
            continue
        # Every clause has to hold, so several on one dimension mean the
        # highest wins. A min this script cannot read is already a problem
        # above; treat it as 0 here so it cannot also pass as strict.
        tier_min = parse_min(tc.get("min")) or 0
        best = max((parse_min(c.get("min")) or 0) for c in matched)
        if best < tier_min:
            problems.append(
                "override %r (%s) gates on %s, which is WEAKER than its own "
                "tier's %s. An override may only tighten: the whole reason an "
                "orphaned override key is reported and not fatal is that its "
                "dye falls back to a tier that asks for more, not less."
                % (key, rarity, describe(matched[0]), describe(tc)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dyes", default=DYES)
    ap.add_argument("--rules", default=RULES)
    args = ap.parse_args()

    problems, notes, unordered = [], [], []
    skills = skill_names()
    deeds = deed_names()
    stats = stat_names()
    if stats is None:
        notes.append("no tools/data/stat_index.json, so CHECK S is skipped and "
                     "no Skyrim misc stat name is verified here. Run "
                     "tools/make_stat_index.py to get it back.")
    form_index = load_form_index(FORM_INDEX, notes)

    pack = load_packs(args.dyes, problems)
    ids = {d for d, _, _ in pack}
    # An absent rarity is how a pack says "free", and vanilla.json's twelve
    # rely on it. Anything else that comes to nothing, a blank string, a null,
    # a number, is a rarity someone WROTE and got wrong, and it resolves to the
    # same "no tier, so no requirement" as an absent one without meaning it.
    # The generator already refuses a blank or space padded cell; a
    # hand-maintained pack has no generator.
    rarities = {}
    for did, rar, src in pack:
        if rar is ABSENT:
            continue
        if isinstance(rar, str) and rar.strip():
            rarities.setdefault(rar, []).append(did)
        else:
            problems.append("%s: dye %r wrote a rarity of %r, which comes to "
                            "nothing, which reads as no requirement" % (src, did, rar))
    print("dye packs: %d colours, %d with a rarity, buckets %s"
          % (len(pack), sum(len(v) for v in rarities.values()),
             ", ".join("%s %d" % (k, len(v)) for k, v in sorted(rarities.items()))))

    files = sorted(glob.glob(os.path.join(args.rules, "*.json")))
    if not files:
        problems.append("%s holds no *.json. With no rules at all every colour "
                        "resolves free and is written permanently into the "
                        "co-save of every character that loads." % args.rules)

    tiers, dye_rules = {}, {}
    missing_by_file = {}
    for path in files:
        name = os.path.basename(path)
        try:
            root = load_json(path)
        except ValueError as e:
            problems.append("%s: does not parse, %s" % (name, e))
            continue
        if not isinstance(root, dict):
            problems.append("%s: the root is not an object" % name)
            continue

        # CHECK B. Exactly the two allowed keys, no more and no fewer.
        #
        # A stray key fails the whole file in the plugin, which freezes
        # progression: recoverable, but a shipping bug. A MISSING section is
        # the half that frees, and it is the shape a bad edit leaves behind: no
        # "tiers" hands out every rarity gated colour, and no "dyes" drops the
        # overrides onto their tiers, ten of them onto Common, which is always.
        extra = sorted(set(root) - ALLOWED_ROOT_KEYS)
        if extra:
            problems.append("%s: unknown top level key(s) %s. The schema is "
                            "closed, so this file fails to load and progression "
                            "freezes until it is removed" % (name, extra))
        # ⚠⚠ MISSING IS JUDGED ACROSS THE WHOLE MERGE, NOT PER FILE, AND THAT
        # IS WHAT COMPOSITION MEANS. DyeRules merges Unlocks/*.json
        # later-file-wins, so a pack that only overrides individual colours
        # legitimately carries "dyes" and no "tiers": lore.json is exactly that
        # shape and inherits every tier from eso.json. Failing it per file would
        # make the composition the loader is built for unshippable.
        #
        # What still has to hold is that SOMETHING provides each section. A
        # merge with no tiers at all frees every rarity-gated colour, which is
        # the failure this check exists for, and it is reported after every file
        # has been read rather than here.
        for absent in sorted(ALLOWED_ROOT_KEYS - set(root)):
            missing_by_file.setdefault(absent, []).append(name)

        for section, into in (("tiers", tiers), ("dyes", dye_rules)):
            # "in", NOT .get(). A section written literally null is present
            # and wrong, which the plugin reports as kWrongType and fails the
            # file for; .get() hands back None for that and for a section that
            # was never written, and the missing check above cannot see it
            # either, since the key IS in root. It would have passed clean here
            # and failed to load in the game.
            if section not in root:
                continue
            body = root[section]
            if not isinstance(body, dict):
                problems.append("%s: %r is present but not an object, so every "
                                "rule in it is lost and every colour it covered "
                                "goes free" % (name, section))
                continue
            for key, conds in body.items():
                check_conditions(name, section, key, conds, skills, deeds,
                                 stats, problems, notes)
                if isinstance(conds, list):
                    check_form_refs(name, section, key, conds, form_index,
                                    problems)
                into[key] = conds

    # CHECK A. Every rarity the packs use has a tier.
    #
    # DyeRuleSet::For falls through to an empty condition vector for a rarity
    # with no tier, and an empty vector is satisfied, so an unmatched rarity
    # means FREE for the whole bucket. The lookup is an exact byte match, so
    # "Dye stamp" against a pack that says "Dye Stamp" frees all 32 of them.
    # Now that every file has been read: a section no file provided is the
    # real failure, and it is reported once rather than once per file.
    for absent, names in sorted(missing_by_file.items()):
        provided = (tiers if absent == "tiers" else dye_rules)
        if not provided:
            problems.append("no %r section in any rules file (%s carr%s none), "
                            "so nothing is gated by it at all"
                            % (absent, ", ".join(names),
                               "ies" if len(names) == 1 else "y"))
        else:
            notes.append("%s carr%s no %r section, which is legitimate: the "
                         "merge takes it from the other file(s). Only worth a "
                         "second look if that was not intended"
                         % (", ".join(names),
                            "ies" if len(names) == 1 else "y", absent))

    for rar in sorted(rarities):
        if rar not in tiers:
            problems.append("rarity %r has no tier, so its %d colours have no "
                            "requirement at all and go free permanently on the "
                            "first load. The lookup is an exact byte match, so "
                            "check the spacing and the casing against %s"
                            % (rar, len(rarities[rar]), sorted(tiers)))

    # A tier nobody uses is inert, which is safe. Worth a line anyway: it is
    # what a rarity typed on the RULES side rather than the pack side looks
    # like, and the check above cannot see that one.
    for tier in sorted(set(tiers) - set(rarities)):
        notes.append("tier %r matches no rarity in the packs, so it gates "
                     "nothing" % tier)

    # The plan's own check. A rule keyed to an id no dye claims is INERT, which
    # is the safe direction, but an orphan hands its colour back to its rarity
    # tier, and ten of the shipped overrides sit on Common, which is always.
    # CHECK C below is what makes "hands it back" mean "to something looser".
    for key in sorted(set(dye_rules) - ids):
        problems.append("override %r matches no dye in %s, so it gates nothing "
                        "and the colour it was written for falls back to its "
                        "rarity tier" % (key, os.path.basename(args.dyes)))

    # CHECK C. An override must be at least as strict as the tier it displaces.
    #
    # This is the check the orphan report above LEANS ON. "An orphan is always a
    # downgrade" is only true while every override asks for more than its tier,
    # and until now that was an assertion in three headers with nothing behind
    # it. See check_strictness for what "stricter" is taken to mean and what it
    # deliberately declines to decide.
    rarity_of = {}
    for did, rar, _ in pack:
        if isinstance(rar, str) and rar.strip():
            rarity_of[did] = rar
    for key in sorted(set(dye_rules) & ids):
        rar = rarity_of.get(key)
        if rar is None or rar not in tiers:
            continue            # no tier to displace: reported above already
        check_strictness(key, rar, dye_rules[key], tiers[rar], problems,
                         unordered)

    print("rules: %d tier(s), %d override(s), across %d file(s)"
          % (len(tiers), len(dye_rules), len(files)))

    if unordered:
        print("\n%d override(s) gate on a dimension their tier does not use, so "
              "\"stricter than its tier\" is taken on trust for these rather "
              "than proven:" % len(unordered))
        for u in unordered:
            print("  - %s" % u)

    if notes:
        print("\n%d note(s), free by design or free by accident and this "
              "cannot tell which:" % len(notes))
        for n in notes:
            print("  - %s" % n)

    if problems:
        print("\n%d problem(s):" % len(problems))
        for p in problems:
            print("  - %s" % p)
        return 1
    print("OK: every rarity has a tier, every override names a real dye and is "
          "no weaker than it on any dimension both gate, every deed has a "
          "producer, and no threshold is met at character creation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
