# -*- coding: utf-8 -*-
"""Flag dye names that mean nothing in Skyrim.

Three verdicts:
  RENAME  names an ESO-only mechanic, faction, class, place or a real world event
  KEEP    shared TES lore, or a plain descriptive colour
  LOOK    matched a term that is ambiguous, so a human decides

Prints the worksheet. Decides nothing on its own.
"""
import collections, io, json, os, re

ROOT = r"C:\Studios\Mod Studio\Fitting Room\.claude\worktrees\outfit-dye"
HERE = os.path.dirname(os.path.abspath(__file__))


def load(rel):
    s = io.open(os.path.join(ROOT, rel), encoding="utf-8").read()
    s = re.sub(r"//.*$", "", s, flags=re.M)
    s = re.sub(r",(\s*[}\]])", r"\1", s)
    return json.loads(s)


# ESO-only. Each entry is (regex, category, why).
RENAME = [
    (r"^Rank \d+ Materials$", "mechanic", "ESO crafting material tier, no Skyrim equivalent"),
    (r"\bAnniversary\b|\bCommemorative\b", "real world", "a real world ESO birthday"),
    (r"\bNew Life\b", "real world", "ESO seasonal event"),
    (r"\b(Covenant|Dominion|Pact)\b", "alliance", "ESO's three player alliances"),
    (r"\b(Aldmeri|Daggerfall|Ebonheart)\b", "alliance", "ESO alliance by its long name"),
    # ⚠ Tribune, Praefect/Prefect, Legate and Quaestor are ALSO Imperial Legion
    # ranks in Skyrim, so they are handled by KEEP below and must not appear here.
    # Only the ranks ESO invented are renameable.
    (r"\b(Palatine|Overlord|Warlord)('s)?\b", "AvA rank",
     "ESO Alliance War rank with no Skyrim equivalent"),
    (r"\b(Dragonknight|Warden|Arcanist)s?('s|s')?\b", "ESO class", "class that exists only in ESO"),
    (r"\b(Craglorn|Eyevea|Artaeum|Fargrave|Galen|Murkmire|Bangkorai|Rivenspire|Glenumbra"
     r"|Stormhaven|Auridon|Grahtwood|Greenshade|Malabal|Deshaan|Shadowfen|Stonefalls"
     r"|Betnikh|Khenarthi|Bleakrock|Foyen|Wrothgar|Necrom|Solstice|High Isle)\b",
     "ESO place", "zone that exists only in ESO"),
    (r"\b(Undaunted|Scaled Court|Worm Cult|Maormer|Sload)\b", "ESO group",
     "faction that exists only in ESO"),
    (r"\bSoul Shriven\b", "ESO concept", "ESO's Coldharbour prologue"),
    (r"\bSkyshard\b|\bDye Stamp\b|\bCrown\b", "mechanic", "ESO progression or store mechanic"),
]

# Terms that LOOK ESO but are shared TES lore, tested BEFORE the rename list.
KEEP = [
    (r"\b(Telvanni|Hlaalu|Indoril|Redoran|Dres|Sadras)\b", "Great House, shared lore"),
    (r"\b(Hist|Welkynd|Varla|Nirnroot|Ayleid|Dwemer|Falmer|Daedr|Aedr)\w*\b", "shared lore"),
    (r"\b(Dibella|Kynareth|Talos|Mara|Stendarr|Julianos|Akatosh|Arkay|Zenithar|Sithis|Molag|Boethiah|Namira|Sanguine|Vaermina|Sheogorath|Malacath|Meridia|Nocturnal|Peryite|Hermaeus|Hircine|Clavicus|Mephala|Azura|Jyggalag)\w*\b", "divine or daedric prince"),
    (r"\b(Coldharbour|Oblivion|Aetherius|Sovngarde|Mundus|Nirn|Tamriel|Cyrodiil|Skyrim"
     r"|Morrowind|Elsweyr|Summerset|Valenwood|Hammerfell|Blackreach|Solstheim|Vvardenfell)\b",
     "shared TES place"),
    (r"\b(Thalmor|Silver Hand|Forsworn|Vigilant|Companions)\b", "Skyrim faction"),
    (r"\b(Nord|Breton|Redguard|Orc|Orsimer|Khajiit|Argonian|Altmer|Bosmer|Dunmer|Imperial"
     r"|Senche|Alfiq|Nedic|Atmoran|Snow Elf|Chimer)\w*\b", "shared race or culture"),
    (r"\b(Stalhrim|Ebony|Malachite|Moonstone|Quicksilver|Corundum|Orichalc\w*|Dwarven|Elven"
     r"|Glass|Steel|Iron|Silver|Gold|Leather|Hide|Chitin|Nordic|Bonemold)\b", "material Skyrim has"),
    (r"\b(Necromancer|Sorcerer|Nightblade|Templar)s?('s|s')?\b",
     "class name that predates ESO in TES"),
    (r"\b(Tribune|Praefect|Prefect|Legate|Quaestor)('s)?\b",
     "Imperial Legion rank in Skyrim itself"),
]

pack = load(r"dist\SKSE\Plugins\FittingRoom\Dyes\eso.json")["dyes"]
rules = load(r"dist\SKSE\Plugins\FittingRoom\Unlocks\eso.json")["dyes"]

rows, tally = [], collections.Counter()
for d in pack:
    name = d["name"]
    keep_reason = next((why for pat, why in KEEP if re.search(pat, name, re.I)), None)
    hit = next(((cat, why) for pat, cat, why in RENAME if re.search(pat, name, re.I)), None)

    if hit and keep_reason:
        verdict, cat, why = "LOOK", hit[0], "%s / but %s" % (hit[1], keep_reason)
    elif hit:
        verdict, cat, why = "RENAME", hit[0], hit[1]
    else:
        verdict, cat, why = "KEEP", "-", keep_reason or "descriptive colour"

    rows.append({"id": d["id"], "name": name, "hex": d["hex"], "rarity": d["rarity"],
                 "ach": d.get("esoUnlock") or "", "verdict": verdict, "cat": cat, "why": why})
    tally[(verdict, cat)] += 1

io.open(os.path.join(HERE, "lore_pass.json"), "w", encoding="utf-8").write(
    json.dumps(rows, ensure_ascii=False, indent=1))

print("306 names classified\n")
for (v, c), n in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0])):
    print("  %-7s %-12s %3d" % (v, c, n))
print("\n  %-7s %d" % ("RENAME", sum(n for (v, _), n in tally.items() if v == "RENAME")))
print("  %-7s %d" % ("LOOK", sum(n for (v, _), n in tally.items() if v == "LOOK")))
print("  %-7s %d" % ("KEEP", sum(n for (v, _), n in tally.items() if v == "KEEP")))

for want in ("RENAME", "LOOK"):
    print("\n=== %s ===" % want)
    for r in sorted([x for x in rows if x["verdict"] == want], key=lambda x: (x["cat"], x["name"])):
        print("  %-11s #%s %-9s %-30s %s"
              % (r["cat"], r["hex"], r["rarity"][:9], r["name"][:30], r["why"][:44]))
