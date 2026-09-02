# -*- coding: utf-8 -*-
"""Build the proposed-dye-locks reference page from the shipped pack and rules."""
import json, re, collections, io, os

# ⚠ DERIVED, AND IT USED TO BE A LITERAL POINTING INTO
# .claude\worktrees\outfit-dye. The worktrees were retired on 2026-08-13, so
# that path stopped existing and this script stopped running at all - it would
# have failed on its first open, in a tool nothing else calls, which is exactly
# the kind of rot nobody notices. Every other tool in here derives its root the
# same way.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dye-locks.html")


def load(rel):
    s = io.open(os.path.join(ROOT, rel), encoding="utf-8").read()
    s = re.sub(r"//.*$", "", s, flags=re.M)
    s = re.sub(r",(\s*[}\]])", r"\1", s)
    return json.loads(s)


pack = load(r"dist\SKSE\Plugins\FittingRoom\Dyes\eso.json")["dyes"]
rules = load(r"dist\SKSE\Plugins\FittingRoom\Unlocks\eso.json")["dyes"]

# ORDER MATTERS: explore is tested before clear, matching the spec's bucketing.
# ORDER MATTERS TWICE OVER. explore is tested before clear, and civilwar before
# clear as well: "Vanquisher of the Covenant" and "Daggerfall Covenant Conqueror"
# are Alliance War PvP, and a bare Vanquisher|Conqueror pattern swallows six of
# them into the dungeon bucket.
BUCKETS = [
    ("levelkeep", r"Level \d+ Hero|Anniversary|Commemorative|New Life|Celebrant"),
    ("explore", r"Master Explorer|Explorer$|Pathfinder|Cave Delver|Skyshard"),
    ("civilwar", r"Alliance War|Emperor|Grand Standard-Bearer|Warlord$|Favored$"
                 r"|Cyrodiil Champion|\b(Covenant|Dominion|Pact)\b"),
    ("clear", r"Delver$|Vanquisher|Conqueror|Completed$|Veteran |Time Trial|Weekly Trial|Challenges$"),
]

# dye name -> (form name, confidence), from the two authored mapping files.
def _load_map(fname):
    out = {}
    path = os.path.join(ROOT, "tools", "data", fname)
    if os.path.exists(path):
        for line in io.open(path, encoding="utf-8").read().splitlines()[1:]:
            p = line.split("\t")
            if len(p) >= 6:
                out[p[0]] = (p[4], p[5])
    return out


CLEARED = _load_map("dye_locks_cleared.tsv")
STORY = _load_map("dye_locks_story.tsv")

# dye name -> "<stat> >= N", from tools/data/dye_locks_counters.tsv
COUNTERS = {}
_cp = os.path.join(ROOT, "tools", "data", "dye_locks_counters.tsv")
if os.path.exists(_cp):
    for _line in io.open(_cp, encoding="utf-8").read().splitlines()[1:]:
        _p = _line.split("	")
        if len(_p) >= 6:
            COUNTERS[_p[0]] = "%s ≥ %s" % (_p[4], _p[5])

PARKED = {
    "Associate of the Fighters Guild": "C00 Take Up Arms",
    "Student of the Mages Guild": "MG01 First Lessons",
    "Recruit of the Undaunted": "Dungeons Cleared \u2265 N",
    "Lycanthropy": "Werewolf Transformations \u2265 1",
    "Vampirism": "Days as a Vampire \u2265 1",
    "Soul Shriven in Coldharbour": "DA10 The House of Horrors",
    "Arch-Mage": "MG08 The Eye of Magnus",
    "Vampirism Master": "DLC1VQ02 Bloodline",
    "Lycanthropy Master": "Werewolf Transformations \u2265 N",
}

# name -> renamed display name, from tools/data/dye_renames.tsv
RENAMES = {}
_tsv = os.path.join(ROOT, "tools", "data", "dye_renames.tsv")
if os.path.exists(_tsv):
    for _line in io.open(_tsv, encoding="utf-8").read().splitlines()[1:]:
        _p = _line.split("\t")
        if len(_p) >= 2 and _p[0] != _p[1]:
            RENAMES[_p[0]] = _p[1]

rows = []
for d in pack:
    ach = d.get("esoUnlock") or ""
    cur = rules.get(d["id"])
    cl = cur if isinstance(cur, list) else ([cur] if cur else [])
    kinds = sorted({c.get("type") for c in cl if isinstance(c, dict)})
    grp = None
    if ach in PARKED:
        grp = "parked"
    elif kinds == ["skill"]:
        grp = "skill"
    elif "deed" in kinds:
        grp = "deed"
    elif kinds == ["level"]:
        for name, pat in BUCKETS:
            if re.search(pat, ach, re.I):
                grp = name
                break
        else:
            grp = "story"
    if not grp or grp == "levelkeep":
        continue

    lvl = next((c.get("min") for c in cl if isinstance(c, dict) and c.get("type") == "level"), None)
    sk = next((c for c in cl if isinstance(c, dict) and c.get("type") == "skill"), None)
    dd = next((c for c in cl if isinstance(c, dict) and c.get("type") == "deed"), None)

    if grp == "skill":
        lock = "%s %d" % (sk["skill"], sk["min"])
    elif grp == "deed":
        lock = "%s \u2265 %d" % (dd["deed"], dd["min"])
    elif grp == "parked":
        lock = COUNTERS.get(d["name"], PARKED[ach])
    elif grp == "explore":
        lock = COUNTERS.get(d["name"], "Locations Discovered \u2265 N")
    elif grp == "clear":
        hit = CLEARED.get(d["name"])
        lock = ("%s cleared" % hit[0]) if hit else "a named location is cleared"
    elif grp == "civilwar":
        lock = COUNTERS.get(d["name"], "Civil War Quests Completed ≥ N")
    else:
        hit = STORY.get(d["name"])
        lock = hit[0] if hit else "a named story quest"

    conf = (CLEARED.get(d["name"]) or STORY.get(d["name"]) or (None, None))[1]
    rows.append({"n": d["name"], "h": d["hex"], "r": d["rarity"],
                 "a": ach or "(none)", "g": grp, "l": lvl, "k": lock, "c": conf})

SEC = [
    ("story", "a named story quest", "story quest", "PROPOSED",
     "Hero of, Savior of, Champion of, and the one-off story achievements. All 47 are mapped in tools/data/dye_locks_story.tsv and verified against the form index."),
    ("clear", "a named location is cleared", "location", "PROPOSED",
     "ESO dungeon and trial clears. All 33 are mapped to a specific Skyrim ruin in tools/data/dye_locks_cleared.tsv, verified against the form index. Reads BGSLocation::IsCleared, the one genuinely new condition kind in the spec."),
    ("explore", "Locations Discovered", "counter", "PROPOSED",
     "ESO zone-completion achievements. Skyrim has no zone tracker, so these count places found. 471 discoverable map markers exist across the masters and the highest threshold here is 132, so the top of this ladder is 28 percent of everything."),
    ("civilwar", "Civil War Quests Completed", "civil war", "PROPOSED",
     "ESO's Alliance War rank ladder. These were going to take a named quest until the form index showed the chain forks the whole way down (CW01A Joining the Legion against CW01B Joining the Stormcloaks), and a rule is an AND with no OR. A count is side neutral. 21 named Civil War quests exist but CW01A/CW01B, CW02A/CW02B and CWResolution01/02 are sided pairs, so one character reaches roughly 8 to 12 and nothing here gates above 8."),
    ("parked", "currently free, by accident", "parked", "PROPOSED",
     "Guilds, lycanthropy, vampirism and Coldharbour. The economy spec parked these for want of a verified form id, so six of the nine are free from character creation today."),
    ("skill", "a trained skill", "skill", "SHIPPED",
     "Already shipped and not moving. A smith who has worked a metal has a reason to know its colour."),
    ("deed", "channels dyed", "deed", "SHIPPED",
     "Already shipped. Our own counter, stored in the co-save."),
]

CSS = """
:root{--paper:#F5F5F4;--card:#FFFFFF;--ink:#191B1D;--ink2:#5E6266;--ink3:#8A8E92;
--rule:#E2E2DF;--rule2:#EFEFEC;--accent:#3D4E6B;--chipring:rgba(0,0,0,.20);
--prop:#7A5B2E;--propbg:#F3EADA;--ship:#3A5A46;--shipbg:#E4EEE7;}
@media (prefers-color-scheme:dark){:root{--paper:#141517;--card:#1B1D20;--ink:#E9E9E6;--ink2:#A0A4A8;
--ink3:#71767A;--rule:#2C2F33;--rule2:#232629;--accent:#9DB4D4;--chipring:rgba(255,255,255,.28);
--prop:#D8B57C;--propbg:#33291A;--ship:#95C4A8;--shipbg:#1E2E25;}}
:root[data-theme="dark"]{--paper:#141517;--card:#1B1D20;--ink:#E9E9E6;--ink2:#A0A4A8;--ink3:#71767A;
--rule:#2C2F33;--rule2:#232629;--accent:#9DB4D4;--chipring:rgba(255,255,255,.28);
--prop:#D8B57C;--propbg:#33291A;--ship:#95C4A8;--shipbg:#1E2E25;}
:root[data-theme="light"]{--paper:#F5F5F4;--card:#FFFFFF;--ink:#191B1D;--ink2:#5E6266;--ink3:#8A8E92;
--rule:#E2E2DF;--rule2:#EFEFEC;--accent:#3D4E6B;--chipring:rgba(0,0,0,.20);
--prop:#7A5B2E;--propbg:#F3EADA;--ship:#3A5A46;--shipbg:#E4EEE7;}
*{box-sizing:border-box}
body{margin:0;background:var(--paper);color:var(--ink);
font-family:ui-sans-serif,-apple-system,"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;
font-size:15px;line-height:1.55;-webkit-font-smoothing:antialiased}
.mono{font-family:ui-monospace,"Cascadia Mono",Consolas,"SF Mono",Menlo,monospace}
.wrap{max-width:1120px;margin:0 auto;padding:40px 24px 96px;display:flex;flex-direction:column;gap:34px}
header{display:flex;flex-direction:column;gap:14px;border-bottom:2px solid var(--ink);padding-bottom:22px}
.eyebrow{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:11px;letter-spacing:.16em;
text-transform:uppercase;color:var(--ink3)}
h1{margin:0;font-size:clamp(1.7rem,3.6vw,2.3rem);font-weight:640;letter-spacing:-.022em;
text-wrap:balance;line-height:1.15}
.lede{margin:0;max-width:64ch;color:var(--ink2);font-size:.95rem}
.note{margin:0;max-width:70ch;font-size:.85rem;color:var(--ink2);
border-left:2px solid var(--accent);padding-left:12px}
.tally{display:flex;flex-wrap:wrap;gap:0;border:1px solid var(--rule);border-radius:4px;
overflow:hidden;background:var(--card)}
.tally div{flex:1 1 116px;padding:11px 14px;border-right:1px solid var(--rule2);
display:flex;flex-direction:column;gap:2px}
.tally div:last-child{border-right:0}
.tally b{font-size:1.3rem;font-weight:640;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.tally span{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:10px;
letter-spacing:.1em;text-transform:uppercase;color:var(--ink3)}
.tools{display:flex;flex-wrap:wrap;gap:10px;align-items:center}
input[type=search]{flex:1 1 240px;min-width:0;padding:9px 12px;border:1px solid var(--rule);
border-radius:4px;background:var(--card);color:var(--ink);font:inherit;font-size:.875rem}
input[type=search]:focus-visible{outline:2px solid var(--accent);outline-offset:1px;border-color:transparent}
.count{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:12px;
color:var(--ink3);font-variant-numeric:tabular-nums}
section{display:flex;flex-direction:column;gap:12px}
.shead{display:flex;flex-wrap:wrap;gap:10px;align-items:baseline}
.shead h2{margin:0;font-size:1.02rem;font-weight:620;letter-spacing:-.012em}
.tag{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:10px;letter-spacing:.1em;
padding:2px 7px;border-radius:3px;text-transform:uppercase;font-weight:600}
.tag.p{color:var(--prop);background:var(--propbg)}
.tag.s{color:var(--ship);background:var(--shipbg)}
.n{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:12px;color:var(--ink3);
font-variant-numeric:tabular-nums;margin-left:auto}
.sdesc{margin:0;max-width:74ch;font-size:.85rem;color:var(--ink2)}
.scroll{overflow-x:auto;border:1px solid var(--rule);border-radius:5px;background:var(--card)}
table{border-collapse:collapse;width:100%;min-width:660px}
th{text-align:left;font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:10px;
letter-spacing:.11em;text-transform:uppercase;color:var(--ink3);font-weight:600;
padding:9px 12px;border-bottom:1px solid var(--rule);white-space:nowrap}
td{padding:7px 12px;border-bottom:1px solid var(--rule2);font-size:.83rem;vertical-align:middle}
tbody tr:last-child td{border-bottom:0}
tbody tr:hover{background:color-mix(in srgb,var(--accent) 7%,transparent)}
.sw{width:30px;height:22px;border-radius:3px;display:block;box-shadow:inset 0 0 0 1px var(--chipring)}
.cswatch{width:1%;padding-right:0}
.chex{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:11.5px;color:var(--ink2);
font-variant-numeric:tabular-nums;white-space:nowrap}
.cname{font-weight:520;white-space:nowrap}
.conf{display:inline-block;width:6px;height:6px;border-radius:50%;margin-left:7px;vertical-align:middle}
.conf.ch{background:var(--ship)}.conf.cm{background:var(--prop)}
.conf.cl{background:transparent;box-shadow:inset 0 0 0 1px var(--ink3)}
.was{display:block;font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:10px;
letter-spacing:.02em;color:var(--prop);font-weight:400;text-transform:none}
.crar{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:10px;letter-spacing:.07em;
text-transform:uppercase;color:var(--ink3);white-space:nowrap}
.cach{color:var(--ink2);font-size:.79rem}
.clock{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:11.5px;white-space:nowrap}
.clvl{font-family:ui-monospace,"Cascadia Mono",Consolas,monospace;font-size:11px;color:var(--ink3);
font-variant-numeric:tabular-nums;text-align:right}
.empty{padding:16px 12px;color:var(--ink3);font-size:.85rem}
footer{border-top:1px solid var(--rule);padding-top:18px;color:var(--ink3);font-size:.8rem;max-width:76ch}
@media (max-width:640px){.wrap{padding:28px 14px 64px}.cach,th.hach{display:none}}
"""

JS = """
(function(){
  var q=document.getElementById('q'), cnt=document.getElementById('cnt'),
      rows=Array.prototype.slice.call(document.querySelectorAll('tbody tr')),
      secs=Array.prototype.slice.call(document.querySelectorAll('section'));
  function run(){
    var v=q.value.trim().toLowerCase(), shown=0;
    rows.forEach(function(r){
      var m = !v || r.getAttribute('data-t').indexOf(v) > -1;
      r.hidden = !m; if(m) shown++;
    });
    secs.forEach(function(s){
      var any = Array.prototype.slice.call(s.querySelectorAll('tbody tr'))
                 .some(function(r){ return !r.hidden; });
      s.querySelector('.empty').hidden = any;
    });
    cnt.textContent = shown + ' shown';
  }
  q.addEventListener('input', run);
})();
"""


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


tally = collections.Counter(r["g"] for r in rows)
o = []
o.append("<title>Fitting Room \u2014 proposed dye locks</title>")
o.append("<style>%s</style>" % CSS)
o.append('<div class="wrap">')
o.append('<header><div class="eyebrow">Fitting Room &middot; lore dye unlocks &middot; 2026-08-06</div>'
         "<h1>Proposed dye locks</h1>"
         '<p class="lede">Every colour that would earn a lock other than a plain player level, grouped by '
         "the mechanism that unlocks it. 221 of the 306 shipped dyes.</p>"
         '<p class="note">The <strong>mechanism</strong> is decided for every row. A row naming a specific '
         "quest or counter is settled; a row reading &ldquo;a named story quest&rdquo; still needs one chosen "
         "by hand, which is 89 remaining decisions. Colours showing a second line under the name are part of "
         "the lore rename pass: 39 of the 306 names describe an ESO mechanic, faction or event that means "
         "nothing in Skyrim.</p>")
o.append('<div class="tally">')
for key, _lock, short, _st, _d in SEC:
    o.append("<div><b>%d</b><span>%s</span></div>" % (tally[key], esc(short)))
o.append("</div>")
o.append('<div class="tools"><input type="search" id="q" '
         'placeholder="Filter by colour, achievement or lock&hellip;" aria-label="Filter dyes">'
         '<span class="count" id="cnt">221 shown</span></div>')
o.append("</header>")

for key, lockdesc, _short, status, desc in SEC:
    sub = [r for r in rows if r["g"] == key]
    o.append('<section data-sec="%s">' % key)
    o.append('<div class="shead"><span class="tag %s">%s</span><h2>%s</h2>'
             '<span class="n">%d colours</span></div>'
             % ("s" if status == "SHIPPED" else "p", status, esc(lockdesc), len(sub)))
    o.append('<p class="sdesc">%s</p>' % esc(desc))
    o.append('<div class="scroll"><table><thead><tr><th class="cswatch"></th><th>Hex</th>'
             "<th>Colour</th><th>Rarity</th><th class=\"hach\">Awarded in ESO for</th>"
             '<th>Proposed lock</th><th style="text-align:right">Was</th></tr></thead><tbody>')
    for r in sorted(sub, key=lambda x: (x["l"] or 0, x["n"])):
        was = ("level %d" % r["l"]) if r["l"] else ("tier" if key == "parked" else "\u2014")
        newname = RENAMES.get(r["n"])
        hay = esc((r["n"] + " " + (newname or "") + " " + r["a"] + " " + r["k"] + " " + r["r"]).lower())
        if newname:
            namecell = ('%s<span class="was">was %s</span>' % (esc(newname), esc(r["n"])))
        else:
            namecell = esc(r["n"])
        o.append('<tr data-t="%s"><td class="cswatch"><span class="sw" style="background:#%s"></span></td>'
                 '<td class="chex">#%s</td><td class="cname">%s</td><td class="crar">%s</td>'
                 '<td class="cach hach">%s</td><td class="clock">%s%s</td><td class="clvl">%s</td></tr>'
                 % (hay, r["h"], r["h"], namecell, esc(r["r"]), esc(r["a"]), esc(r["k"]),
   ('<span class="conf c%s" title="%s confidence"></span>' % (r["c"][0], r["c"])) if r.get("c") else "",
   was))
    o.append('</tbody></table><div class="empty" hidden>No colour in this group matches the filter.</div>'
             "</div></section>")

o.append("<footer>Colour values are the shipped hex from <span class=\"mono\">Dyes/eso.json</span>. "
         "&ldquo;Was&rdquo; is the rule a colour carries today: a player level for most, and for the nine "
         "parked ones a rarity tier that leaves six of them free from character creation. Skill and deed "
         "rows already ship and appear here because they are locks, not because they change.</footer>")
o.append("</div>")
o.append("<script>%s</script>" % JS)

io.open(OUT, "w", encoding="utf-8").write("\n".join(o))
print("rows %d -> %s (%d bytes)" % (len(rows), OUT, os.path.getsize(OUT)))
print(dict(tally))
