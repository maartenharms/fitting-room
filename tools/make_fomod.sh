#!/usr/bin/env bash
# Build the Fitting Room FOMOD installer zip (the release artifact).
#
# Fitting Room is one DLL that loads on both SE 1.5.97 and AE 1.6.1130+, so there
# is no SE/AE file choice. The FOMOD is a branded page with ONE choice: the
# starting setup, Lore-friendly or Free-form.
#
# ⚠⚠ THAT CHOICE IS SETTINGS AND NOTHING ELSE SINCE 2026-08-11. Both options
# install byte-identical files and differ only in which FittingRoom.ini is
# written. The ESP, the Seamstone's icon rule and the lore unlock rules used to
# ride the Lore-friendly branch, which let an installer page decide whether the
# playstyle switch in the in-game panel could ever work: a player who installed
# Free-form and later turned Lore-friendly on got a Seamstone requirement
# gating nothing, a charge meter for a stone they could not buy, and 80 dye
# colours back on their rarity tier. There is no `optional/` tree any more.
#
# Package layout (zip root): fomod/{info.xml,ModuleConfig.xml}, Images/, core/
# (every game file, installed to Data, the ESP included),
# settings/<setup>-<colours>/ (the starting INI, one per combination), + LICENSE and a
# short README.txt at the root (NOT installed). Docs single-source-of-truth is
# GitHub; the download carries only LICENSE (GPL) + a README.txt pointer, no
# CHANGELOG/KNOWN-ISSUES/THIRD-PARTY.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^project(FittingRoom VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DLL="$ROOT/build/release/FittingRoom.dll"
STAGE="$ROOT/release/fomod-stage"
ZIP="$ROOT/release/FittingRoom-$VER.zip"
# A zip built with the banner standing in for missing installer pictures is a
# test artifact and is named as one, so it can never be uploaded by mistake as
# the release. See the picture check below.
if [ "${FR_ALLOW_MISSING_SHOTS:-0}" = "1" ]; then
    ZIP="$ROOT/release/FittingRoom-$VER-TEST-no-shots.zip"
fi

[ -f "$DLL" ] || { echo "no DLL at $DLL - build first"; exit 1; }

# ⚠⚠ A DIAGNOSTIC BUILD CAN NEVER WEAR THE RELEASE NAME. It writes a second
# copy of every session's log for every player who installs it and forces the
# appearance watch on, and the one way that reaches Nexus is a packaging run
# against a build tree somebody left configured with FR_DIAG=ON. The DLL is
# asked directly rather than the cache, because the cache is not what ships.
# Same rule and same spelling as the missing-shots refusal above: rename the
# artifact, do not refuse the build.
#
# ⚠ The pattern is version-free on purpose. Menu Studio's read "1.1.5-diag"
# until 1.1.7 and would have waved a 1.1.6 diag build straight through.
if grep -qa -- "-diag" "$DLL" 2>/dev/null ||
   grep -qa "DIAGNOSTIC BUILD" "$DLL" 2>/dev/null; then
    ZIP="${ZIP%.zip}-DIAG-do-not-upload.zip"
    echo "*** the DLL is a DIAGNOSTIC build: naming the archive $(basename "$ZIP")"
fi

# The shipped unlock rules, and this build REFUSES TO PACKAGE WITHOUT THEM.
#
# dist/.../Dyes/eso.json carries a "rarity" on all 306 colours, and from Task 6
# on, the plugin promotes earned colours into the player's co-save on every
# load. With no rules file, every one of those rarities matches no tier, and no
# tier means no requirement: the whole palette is handed out at level 1 and
# written permanently into that character's save, because unlocks are add only.
# Fixing the packaging afterwards does not take it back, on any character who
# loaded the broken build.
#
# Same shape as the DLL check above, and for a worse consequence. This repo has
# shipped a stale zip twice, so it is a refusal rather than a warning nobody
# reads.
#
# NOT RELAXED NOW THAT THE FILE EXISTS, which was the obvious move and the
# wrong one. An earlier version of this comment said the check would pass and
# stay quiet forever once Task 7 landed, and that is exactly the problem: "the
# directory is not empty" became trivially true the moment a file was dropped
# in, while the thing it was written to prevent, a shipped colour that is free
# for no reason an author chose, did not become any harder. So the existence
# test keeps its own message, and a second refusal below asks whether the rules
# actually cover the palette.
RULES_DIR="$ROOT/dist/SKSE/Plugins/FittingRoom/Unlocks"
ls "$RULES_DIR"/*.json >/dev/null 2>&1 || {
    echo "no unlock rules in $RULES_DIR"
    echo "The dye palette ships rarities and the plugin promotes on load, so"
    echo "packaging without rules frees all 306 colours PERMANENTLY on every"
    echo "character who loads this build. Land Task 7 of the dye unlocks plan"
    echo "before releasing."
    exit 1
}

# The rules have to COVER the palette, which having a file does not establish.
# A rarity with no tier, an override keyed to an id no dye claims and a
# threshold a new character already meets all resolve to a colour handed out at
# level 1, all three are silent in the plugin on purpose, since a third party
# rules pack may legitimately contain any of them, and all three are permanent
# once a save has stored them. Checked here rather than trusted, because the
# palette is GENERATED and the rules are hand written, so a regeneration can
# invent a rarity nobody has authored a tier for.
#
# Run rather than reimplemented in shell: the script parses both sides and
# reads the skill name table out of src/DyeConditions.cpp, so there is no
# second copy of any of it to drift.
#
# ⚠ EITHER SPELLING OF THE INTERPRETER. This said "python" alone, which is
# correct on the machine it was written on and on no minimal Linux or macOS
# box, where python3 is the only name. Refusing to package for want of an alias
# is a refusal with no safety in it.
PY=""
for candidate in python3 python; do
    command -v "$candidate" >/dev/null 2>&1 && { PY="$candidate"; break; }
done
[ -n "$PY" ] || {
    echo "neither python3 nor python is on PATH, so the unlock rules cannot be checked."
    echo "Refusing to package rather than shipping them unread."
    exit 1
}
"$PY" "$ROOT/tools/check_dye_rules.py" || {
    echo
    echo "The unlock rules do not cover the dye packs, and every problem above"
    echo "frees a colour PERMANENTLY on every character who loads this build."
    echo "Fix dist/SKSE/Plugins/FittingRoom/Unlocks before releasing."
    exit 1
}

# The installer pictures, checked BEFORE the old zip is removed. One in-game
# screenshot per option lives in fomod/images/ under the name
# fomod/ModuleConfig.xml asks for; the shot list is docs/release/fomod-shots.md.
#
# ⚠ EVERY IMAGE THE XML NAMES HAS TO BE IN THE ZIP, and the check reads the
# XML rather than a list kept here, so adding an option with a new picture
# cannot forget the file: a mod manager shows a broken image or nothing for a
# missing path, and neither says so anywhere a release check would read. A
# missing shot REFUSES to package, and it refuses here, ahead of the rm below,
# so a refused run leaves the previous zip where it was. FR_ALLOW_MISSING_SHOTS=1
# turns the refusal into a warning and ships the banner in the missing
# picture's place, for a test zip before the screenshots exist; a release build
# never sets it.
#
# ⚠⚠ AND A BANNER SAVED UNDER A SHOT'S NAME IS A MISSING SHOT, which is the hole
# this check had until 2026-08-31. FR_ALLOW_MISSING_SHOTS copies the banner into
# the missing picture's place inside the STAGE, which is right; what went wrong
# is that somebody copied it into fomod/images/ instead, where it is a file, so
# the existence test above passed for the rest of time. face-page.png arrived
# that way with 1.1.3's face step and shipped as a real picture in 1.1.4, byte
# for byte the banner, with nothing anywhere saying so. Comparing the bytes is
# the whole fix: a stand-in is allowed in the zip and is not allowed in the
# source tree.
BANNER_SUM=""
if [ -f "$ROOT/fomod/banner.png" ]; then
    BANNER_SUM="$(md5sum "$ROOT/fomod/banner.png" | cut -d' ' -f1)"
fi
# The pictures that ARE the banner on purpose, by name. The face page asks no
# question (both answers install the same files and set no flag), so the
# author decided on 2026-09-09 that its picture is the banner and not a
# screenshot. A name here is a decision on record; a banner under any other
# name is still the stand-in the check below refuses.
BANNER_BY_DESIGN=("face-page.png")
STAND_IN=()
missing=0
while IFS= read -r img; do
    # if/then rather than `[ ] && continue`: under set -e a false test at the
    # end of an && list aborts the run, and this loop must reach its report.
    if [ -f "$ROOT/fomod/images/$img" ]; then
        if [ -n "$BANNER_SUM" ] &&
           [ "$(md5sum "$ROOT/fomod/images/$img" | cut -d' ' -f1)" = "$BANNER_SUM" ]; then
            by_design=0
            for allowed in "${BANNER_BY_DESIGN[@]}"; do
                if [ "$allowed" = "$img" ]; then
                    by_design=1
                fi
            done
            if [ "$by_design" = 1 ]; then
                echo "fomod/images/$img is the banner by design (docs/release/fomod-shots.md)"
            elif [ "${FR_ALLOW_MISSING_SHOTS:-0}" = "1" ]; then
                echo "WARNING: fomod/images/$img IS the banner, not a screenshot (test zip only)"
            else
                echo "fomod/images/$img is byte for byte fomod/banner.png, so it is the"
                echo "  banner standing in rather than a screenshot of that option."
                missing=1
            fi
        fi
        continue
    fi
    if [ "${FR_ALLOW_MISSING_SHOTS:-0}" = "1" ]; then
        echo "WARNING: fomod/images/$img is missing, shipping the banner in its place (test zip only)"
        STAND_IN+=("$img")
    else
        echo "missing installer picture: fomod/images/$img (named by fomod/ModuleConfig.xml)"
        missing=1
    fi
done < <(sed -n 's/.*<image path="Images\\\([^"]*\)".*/\1/p' "$ROOT/fomod/ModuleConfig.xml" | sort -u)
[ "$missing" = 0 ] || {
    echo "The shot list is docs/release/fomod-shots.md. Take the pictures, drop them"
    echo "in fomod/images/, and package again. FR_ALLOW_MISSING_SHOTS=1 builds a"
    echo "test zip with the banner standing in."
    exit 1
}

# ⚠⚠ THE INSTALLER COPY IS UNISEX. The player's character can be anyone, so no
# string in ModuleConfig.xml refers to them as "her" or "she". One sentence about
# interface mods carried it in all three installers at once (user, 2026-08-28);
# two were fixed by hand and the third was missed, so it is checked here instead
# of remembered. Ahead of the rm below, so a refusal leaves the old zip alone.
#
# ⚠ ONLY ModuleConfig.xml. nexus/DESCRIPTION.bbcode legitimately thanks a real
# person as "her" and must not be swept up in this.
if grep -qiE '\b(her|hers|she|herself)\b' "$ROOT/fomod/ModuleConfig.xml"; then
    echo "fomod/ModuleConfig.xml calls the player's character 'her'. The installer copy is unisex:"
    grep -niE '\b(her|hers|she|herself)\b' "$ROOT/fomod/ModuleConfig.xml"
    echo "Use 'your character', or they/them."
    exit 1
fi

# ⚠⚠ THE SKINS FOLDER HAS TO EXIST IN dist AND HAVE A FILE IN IT. Ahead of the
# rm, like the checks above, so a refusal leaves the old zip alone. The staging
# assertion further down catches a copy that went wrong; this one catches the
# authoring mistake that shipped the bug in the first place, which was simply
# that dist never had the folder at all.
[ -f "$ROOT/dist/textures/FittingRoom/skins/README.txt" ] || {
    echo "dist/textures/FittingRoom/skins/README.txt is missing."
    echo "That folder is where the Overlays page hard-links skins to, and"
    echo "SkinRivals resolves its real path by opening it. With no mod supplying"
    echo "it, every skin link is refused. A zip cannot carry an empty directory,"
    echo "so the folder needs a file in it to survive packaging."
    exit 1
}

rm -rf "$STAGE" "$ZIP"

# FOMOD metadata + banner (ModuleConfig references Images\fittingroom.png).
mkdir -p "$STAGE/fomod" "$STAGE/Images"
cp "$ROOT/fomod/info.xml" "$ROOT/fomod/ModuleConfig.xml" "$STAGE/fomod/"
cp "$ROOT/fomod/banner.png" "$STAGE/Images/fittingroom.png"

# The per-option pictures: in-game screenshots, one per installer option, kept
# in fomod/images/ under the names ModuleConfig.xml asks for. The shot list,
# what each one shows and how to take it, is docs/release/fomod-shots.md.
# The check ran above, before anything was deleted; this is only the copy.
if ls "$ROOT/fomod/images/"*.png >/dev/null 2>&1; then
    cp "$ROOT/fomod/images/"*.png "$STAGE/Images/"
fi
# ${arr[@]+"${arr[@]}"}: an empty array under set -u is "unbound" on older
# bashes, and this array is empty on every release build.
for img in ${STAND_IN[@]+"${STAND_IN[@]}"}; do
    cp "$ROOT/fomod/banner.png" "$STAGE/Images/$img"
done

# core: the game files that install to Data (DLL + editor fonts + sample preset +
# the SAM-integration script). dist/ mirrors the mod-folder tree.
mkdir -p "$STAGE/core/SKSE/Plugins"
cp "$DLL" "$STAGE/core/SKSE/Plugins/"
cp -r "$ROOT/dist/SKSE/Plugins/FittingRoom" "$STAGE/core/SKSE/Plugins/FittingRoom"
# ⚠⚠ THE LORE UNLOCK RULES ARE IN core NOW, AND THEY USED TO BE CUT BACK OUT OF
# IT. The old line here deleted Unlocks/lore.json from core so it could ride the
# Lore-friendly choice, on the argument that a free-form install must not "take
# the lore locks in silence". That argument does not survive contact with what
# unlock rules actually do: a rule can only ever LOCK, a colour with NO rule is
# FREE, and with bDyeUnlocks off nothing is gated at all. So the file changed
# nothing for a free-form player and cost a Lore-friendly one their whole
# economy the moment they had installed the other way round - 80 colours falling
# back to their rarity tier, with the locked swatch reciting "Level 35" where it
# should be naming a quest (user 2026-08-11).
#
# ⚠ AND NOTHING IN IT NEEDS THE ESP. All 80 rules name base-game or DLC quests
# and locations; not one references FittingRoomLore.esp. It shipped beside the
# plugin because it was written in the same week, not because it depends on it.
[ -f "$STAGE/core/SKSE/Plugins/FittingRoom/Unlocks/eso.json" ] ||
    { echo "core lost Unlocks/eso.json, which every install needs"; exit 1; }
[ -f "$STAGE/core/SKSE/Plugins/FittingRoom/Unlocks/lore.json" ] ||
    { echo "core lost Unlocks/lore.json, so a Lore-friendly player would get the"
      echo "rarity tiers instead of the authored quest and location rules."; exit 1; }
cp -r "$ROOT/dist/Scripts" "$STAGE/core/Scripts"
cp -r "$ROOT/dist/Interface" "$STAGE/core/Interface"  # FLICK translation (Fitting Room_ENGLISH.txt)

# ⚠⚠ THE SKINS FOLDER, AND IT HAS NEVER SHIPPED BEFORE 1.1.2. The Skin section
# of the Overlays page links a whole-body replacer in by hard-linking it into
# Data/textures/FittingRoom/skins, and SkinRivals resolves the REAL path of that
# folder by opening the virtual one. No mod supplying it means no handle, no
# real path, and every link refused with "the skins folder could not be opened"
# (field 2026-08-28, on a clean 1.1.2 install).
#
# It worked on the author's rig for the worst possible reason: the base
# "Fitting Room" mod that redeploy keeps enabled below every branch slot has the
# folder, made by hand on 2026-08-18 and never added to dist. So the feature
# shipped in 1.1.0 and has been broken in every zip since, while working
# perfectly in every test. SkinRivals.cpp's own comment names that base mod.
#
# ⚠ THE README.txt IS LOAD-BEARING, not documentation that happens to sit there.
# A zip cannot carry an empty directory and a FOMOD installer will not create
# one, so the folder only survives packaging because a file is inside it.
cp -r "$ROOT/dist/textures" "$STAGE/core/textures"
[ -f "$STAGE/core/textures/FittingRoom/skins/README.txt" ] ||
    { echo "core has no textures/FittingRoom/skins - every skin link would be"
      echo "refused on a clean install. The folder needs a file in it to survive"
      echo "the zip; see dist/textures/FittingRoom/skins/README.txt."; exit 1; }

# ⚠⚠ IN core IS NOT INSTALLED. requiredInstallFiles names what the installer
# copies, one entry per top-level item, and anything in core that it does not
# name is carried in the download and then dropped on the floor. That is the
# 2026-08-29 bug exactly: textures/ was added to core, the zip carried it, the
# FOMOD installed SKSE, Scripts, Interface and the ESP, and the skins folder
# never reached the mod folder. The field saw the identical error message from a
# zip that demonstrably contained the fix.
#
# So this is checked rather than remembered, for EVERY top-level entry rather
# than for textures alone. A new folder in core now has to be declared or the
# package refuses.
for entry in "$STAGE/core"/*; do
    name="$(basename "$entry")"
    if ! grep -qF "core\\$name" "$STAGE/fomod/ModuleConfig.xml"; then
        echo "core/$name is in the zip but fomod/ModuleConfig.xml never installs it."
        echo "Add it to <requiredInstallFiles>, or it ships and is discarded:"
        echo "    <folder source=\"core\\$name\" destination=\"$name\" priority=\"0\"/>"
        exit 1
    fi
done

# The Seamstone lore item ESP. IN core SINCE 2026-08-11: it used to ride the
# Lore-friendly choice, which meant an installer page decided whether the
# playstyle switch in the settings panel could ever work. Turning Lore-friendly
# on after a free-form install left the player with a Seamstone requirement
# gating nothing and a charge meter for a stone they could not buy.
#
# ⚠ THE COST IS A LIGHT SLOT, NOT A REAL ONE, and that is what makes this
# defensible rather than merely convenient. Checked here rather than trusted,
# because an accidental un-flagging would start eating one of the 254: byte 8 of
# the TES4 header is the record flags, and 0x200 is ESL.
cp "$ROOT/dist/optional/FittingRoomLore.esp" "$STAGE/core/FittingRoomLore.esp"
"$PY" - "$STAGE/core/FittingRoomLore.esp" <<'ESL' || exit 1
import struct, sys
with open(sys.argv[1], "rb") as fh:
    sig, size, flags = struct.unpack("<4sII", fh.read(12))
if sig != b"TES4":
    sys.exit("FittingRoomLore.esp does not start with a TES4 header")
if not flags & 0x200:
    sys.exit("FittingRoomLore.esp is NOT ESL-flagged (flags 0x%08X). It now ships to\n"
             "every install, so an un-flagged copy would spend one of the 254 regular\n"
             "load order slots on someone who chose Free-form." % flags)
ESL
# The Seamstone's inventory icon rule, which HAD NEVER BEEN PACKAGED AT ALL
# before 2026-08-08. The rule has been in dist since 2026-08-05 and no player
# received it, because core copies dist/SKSE/Plugins/FittingRoom and nothing
# copied its InventoryInjector sibling.
#
# ⚠ THE FILENAME IS LOAD BEARING AND MUST STAY EQUAL TO THE ESP's. Inventory
# Injector only reads a rules file when a plugin of the SAME BASENAME is
# loaded - every file it reported reading matches one (Skyrim.esm,
# Dawnguard.esm, I4IconAddon.esp, "Bard Hero - Doom Lute.esp"). It was called
# FittingRoom.json against FittingRoomLore.esp, so it was skipped in silence:
# I4's log lists what it READ and simply never mentioned it.
#
# It goes to core beside the ESP it names. I4 itself is a SOFT dependency: it
# reads every .json in that folder and is simply not there to read this one if
# the player has not installed it.
mkdir -p "$STAGE/core/SKSE/Plugins/InventoryInjector"
cp "$ROOT/dist/SKSE/Plugins/InventoryInjector/FittingRoomLore.json" \
   "$STAGE/core/SKSE/Plugins/InventoryInjector/"

# Starting-settings flavors, BOTH derived from the ONE documented template so
# the comments never fork. The template ships the lore values; free-form flips
# the two gates and its own header line. Fitting Room writes its INI itself on
# first run when none exists, so these only preseed the choice.
# ⚠⚠ TWO INDEPENDENT AXES NOW, SO FOUR FILES, AND THEY ARE GENERATED RATHER
# THAN MAINTAINED. What styling COSTS and whether colours must be EARNED are
# separate questions: "dressing up is free but the rare dyes are earned" and its
# opposite are both reasonable, and bundling them meant one of the two was
# always wrong for somebody. A loop over the combinations is the only way to add
# an axis without the file count becoming a maintenance problem, and every flip
# is guarded, because a substitution that stops biting ships a file labelled one
# thing and holding another in silence.
#
# ⚠⚠ AND FREE-FORM DID NOT ACTUALLY FREE THE COLOURS UNTIL NOW. Its installer
# page has always promised "every look and colour you have installed is yours
# from the start", and bDyeUnlocks was absent from the template, so the code
# default of 1 governed and colours still had to be earned. The page was
# describing a state the file it wrote did not produce. The template carries the
# key now, which is what makes it flippable at all.
#
# ⚠⚠ AND IT HAPPENED A SECOND TIME, WITH bCollectionOnly, REPORTED FROM A FRESH
# FOMOD INSTALL (user 2026-08-27): pick Freeform and the mod still comes up lore
# friendly. The playstyle is FIVE fields, and the two preset buttons in the
# settings panel have set all five since 2026-08-11 for exactly this reason, but
# the template only ever carried four. bCollectionOnly was absent, its code
# default is 1, so a Freeform install kept the collection filter on: the browser
# offered only looks the character had already owned the gear for, and applying
# any other was refused. That IS the lore-friendly half of "earn it, then wear
# it", installed by the page that promised the opposite.
#
# ⚠ THE RULE THIS KEEPS BREAKING: a playstyle field the panel's presets set is a
# field the template has to carry. A key the template omits is not neutral, it
# is whatever the C++ default happens to be, and the defaults were written for
# the other playstyle. Adding a field to the presets and not to the template
# leaves the installer a version behind in silence.
# ⚠⚠ THE PAGES AXIS IS GONE AND THE TEMPLATE IS THE ONLY ANSWER NOW (user
# 2026-08-28). It asked which of the eight sidebar tiles to start with and
# offered three bundles. Two of the three hid working pages from a player who
# had not seen one yet, to save them a sidebar the editor's own Pages tab
# empties in a second, so the installer stopped asking. Twelve flavours became
# four.
#
# ⚠⚠ AND THAT TURNED EIGHT KEYS INTO AN UNGUARDED DEFAULT, which is the exact
# failure this file has already shipped twice: bDyeUnlocks, then bCollectionOnly,
# both absent from the template and both therefore whatever the C++ default
# happened to be that month. A key the installer no longer writes is not
# neutral. The template carries all eight at 1 and the loop below reads every
# one of them back.
# ⚠⚠ A THIRD AXIS SINCE 2026-08-29, SO TWELVE FLAVOURS RATHER THAN FOUR. The
# face question is its own because it is orthogonal to both the others: a
# lore-friendly player who earns their dyes has no more opinion about whether
# the mod rewrites their RaceMenu face than a free-form one does, and folding it
# into either would have made the answer wrong for somebody. Same reasoning that
# split setup from colours above, and the loop is what keeps twelve honest.
# The installer stamp names the zip and the answers, so the plugin can tell a
# new install from a file it has already seeded. One per package run.
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
for setup in lore freeform; do
  for colours in earn free; do
   for face in safe full; do
    dir="$STAGE/settings/$setup-$colours-$face"
    mkdir -p "$dir"
    out="$dir/FittingRoom.ini"

    # ⚠⚠ EVERY FLAVOUR GOES THROUGH ONE sed, INCLUDING THE ONE THAT CHANGES
    # NOTHING, and that is not tidiness. The template is CRLF. A flavour built
    # with `cp` keeps CRLF and one built with `sed -i` comes out LF, so the two
    # differ on every line while holding identical settings. The diff guard
    # below caught exactly that and refused to package, which is the only reason
    # it is not in the zip: a 190-line difference between two files that are
    # supposed to differ by one key is invisible from the outside.
    #
    # Building the expression list and applying it once, always, keeps the line
    # endings identical across all four and makes the guard's counts mean what
    # they say.
    seds=()
    if [ "$setup" = "freeform" ]; then
      seds+=(-e 's/^iCostMode=2/iCostMode=0/')
      seds+=(-e 's/^bRequireSeamstone=1/bRequireSeamstone=0/')
      seds+=(-e 's/^bRequireWornForStyles=1/bRequireWornForStyles=0/')
      seds+=(-e 's/^bCollectionOnly=1/bCollectionOnly=0/')
      seds+=(-e 's/^; Starting settings: LORE-FRIENDLY.*/; Starting settings: FREE-FORM (no cost, no Seamstone gate, dress out of nothing)./')
    fi
    if [ "$colours" = "free" ]; then
      seds+=(-e 's/^bDyeUnlocks=1/bDyeUnlocks=0/')
    fi
    # ⚠ THE TEMPLATE SHIPS THE FULL ANSWER SINCE 1.1.7 (user 2026-09-02), so
    # 'full' substitutes nothing and is guarded against the template drifting
    # instead, the way the lore side is, and 'safe' is the one that flips.
    #
    # ⚠⚠ TWO ANSWERS, NOT THREE, SINCE 2026-08-29 (user). The middle one hid the
    # Looks page while leaving the other two keys safe, and it turned out not to
    # be a separate position anyone holds: the Looks page IS the face feature,
    # so hiding it belongs with turning the other two off rather than beside
    # it. Three keys, one decision.
    if [ "$face" = "safe" ]; then
      seds+=(-e 's/^bReassertAppearance=1/bReassertAppearance=0/')
      seds+=(-e 's/^bLooksRaceMenu=1/bLooksRaceMenu=0/')
      seds+=(-e 's/^bLooks=1/bLooks=0/')
    fi
    # ⚠ EVERY FLAVOUR CARRIES ITS OWN STAMP, the template one included, and
    # the plugin seeds from it exactly once per install (InstallerSeed.h).
    seds+=(-e "s/^sInstallerStamp=TEMPLATE/sInstallerStamp=$VER-$setup-$colours-$face-$STAMP/")
    # A no-op expression so the array is never empty and sed is always the tool
    # that wrote the file.
    seds+=(-e 's/^$/^/; s/^\^$//')
    sed "${seds[@]}" "$ROOT/dist/optional/settings/FittingRoom.ini" > "$out"

    if [ "$setup" = "freeform" ]; then
      grep -q '^iCostMode=0' "$out" ||
        { echo "$setup-$colours: iCostMode substitution failed"; exit 1; }
      grep -q '^bRequireSeamstone=0' "$out" ||
        { echo "$setup-$colours: bRequireSeamstone substitution failed"; exit 1; }
      grep -q '^bRequireWornForStyles=0' "$out" ||
        { echo "$setup-$colours: bRequireWornForStyles substitution failed"; exit 1; }
      grep -q '^bCollectionOnly=0' "$out" ||
        { echo "$setup-$colours: bCollectionOnly substitution failed"; exit 1; }
      grep -q '^; Starting settings: FREE-FORM' "$out" ||
        { echo "$setup-$colours: header substitution failed"; exit 1; }
    else
      # ⚠ THE LORE SIDE IS GUARDED TOO, in the other direction. It is a copy of
      # the template, so the only way it can be wrong is the template itself
      # drifting, and that is exactly the failure nothing else here would catch.
      grep -q '^iCostMode=2' "$out" ||
        { echo "$setup-$colours: template no longer ships iCostMode=2"; exit 1; }
      grep -q '^bRequireWornForStyles=1' "$out" ||
        { echo "$setup-$colours: template no longer ships bRequireWornForStyles=1"; exit 1; }
      grep -q '^bCollectionOnly=1' "$out" ||
        { echo "$setup-$colours: template no longer ships bCollectionOnly=1"; exit 1; }
    fi

    if [ "$colours" = "free" ]; then
      grep -q '^bDyeUnlocks=0' "$out" ||
        { echo "$setup-$colours: bDyeUnlocks substitution failed"; exit 1; }
    else
      grep -q '^bDyeUnlocks=1' "$out" ||
        { echo "$setup-$colours: template no longer ships bDyeUnlocks=1"; exit 1; }
    fi

    # ⚠⚠ THE FACE AXIS, GUARDED IN BOTH DIRECTIONS. These keys decide whether
    # the mod may rewrite a face somebody built in RaceMenu, and the cost of
    # getting it wrong is the fault that put two players on a rollback in one
    # night. The template ships them ON since 1.1.7, so the guard now catches a
    # silent drift back to 0 (which would hand every new install the old
    # answer) and a 'safe' substitution that stopped landing.
    case "$face" in
      safe)
        grep -q '^bReassertAppearance=0' "$out" ||
          { echo "$setup-$colours-$face: bReassertAppearance substitution failed"; exit 1; }
        grep -q '^bLooksRaceMenu=0' "$out" ||
          { echo "$setup-$colours-$face: bLooksRaceMenu substitution failed"; exit 1; }
        grep -q '^bLooks=0' "$out" ||
          { echo "$setup-$colours-$face: bLooks substitution failed"; exit 1; }
        ;;
      full)
        grep -q '^bReassertAppearance=1' "$out" ||
          { echo "$setup-$colours-$face: template no longer ships bReassertAppearance=1"; exit 1; }
        grep -q '^bLooksRaceMenu=1' "$out" ||
          { echo "$setup-$colours-$face: template no longer ships bLooksRaceMenu=1"; exit 1; }
        grep -q '^bLooks=1' "$out" ||
          { echo "$setup-$colours-$face: template no longer ships bLooks=1"; exit 1; }
        ;;
    esac

    # ⚠⚠ THE COLOUR REPAIR IS NOT PART OF THE FACE QUESTION AND MUST BE ON IN
    # EVERY FLAVOUR, safe included. It swaps no head part; all it does is put a
    # skin tone and a hair colour back after the engine repaints a head over
    # them, which a walk through any door triggers. Folding it in with the face
    # settings is precisely what shipped the 1.1.3/1.1.4 regression where a head
    # stopped matching its body after an interior switch, so this guard exists
    # to stop it being folded back in by whoever next edits the face answers.
    grep -q '^bKeepColoursAfterRebuild=1' "$out" ||
      { echo "$setup-$colours-$face: bKeepColoursAfterRebuild must ship =1 in every flavour"; exit 1; }

    # ⚠⚠ NEITHER OF THESE IS AN AXIS, AND THEY ARE GUARDED FOR THAT REASON. Both
    # are identical in all twelve, so no substitution can break them and only
    # the template drifting can - which is exactly the failure that put a player
    # on an editor scale nobody had chosen. fUiScale had never been shipped at
    # all until 2026-08-27: an absent key takes the DLL's default, that default
    # has been three different numbers, and Settings::Save pins whichever one
    # was current into the file for good.
    grep -q '^fUiScale=0.63' "$out" ||
      { echo "$setup-$colours: template no longer ships fUiScale=0.63"; exit 1; }
    # ⚠ iFrameStyle IS HERE FOR THE REASON GIVEN ABOVE, and it was missing
    # until 2026-08-27. An absent key meant every fresh install took whichever
    # default the DLL happened to carry that month, and this one has been 1,
    # then 0, then 1 again.
    grep -q '^iFrameStyle=1' "$out" ||
      { echo "$setup-$colours: template no longer ships iFrameStyle=1"; exit 1; }
    # ⚠ THE STAMP TRACKS THE HIGHEST MIGRATION EVER SHIPPED, and it does not
    # come back down when one is deleted. It went to 2 with the frame-style
    # migration, that migration is gone, and 2 stayed. It is 3 as of
    # 2026-08-29, for the Seamstone experience rate, and 4 as of 2026-09-02,
    # for the face keys' one-time flip: a template claiming less would tell
    # a fresh install it had been through fewer than it has, and a fresh
    # 'safe' answer under 3 would be flipped on by the migration it predates.
    grep -q '^iSettingsVersion=4' "$out" ||
      { echo "$setup-$colours-$face: template no longer ships iSettingsVersion=4"; exit 1; }
    grep -q "^sInstallerStamp=$VER-$setup-$colours-$face-$STAMP" "$out" ||
      { echo "$setup-$colours-$face: sInstallerStamp substitution failed"; exit 1; }
    # ⚠⚠ ALL EIGHT SIDEBAR PAGES, READ BACK ONE AT A TIME. The installer
    # stopped asking on 2026-08-28, so nothing else here would notice the
    # template losing one, and a page key absent from the INI does not mean
    # "leave it alone", it means whatever the DLL defaults to.
    #
    # ⚠ bLooks IS AN AXIS NOW and is checked by the face case above instead.
    # Leaving it in this list would fail every 'outfits' flavour, which is the
    # one that deliberately ships the page off.
    for page in bStyles bDye bBodies bShape bOverlays bPresets bRules; do
      grep -q "^$page=1" "$out" ||
        { echo "$setup-$colours-$face: template no longer ships $page=1 (every page ships on)"; exit 1; }
    done
   done
  done
done

# ⚠⚠ THE TWELVE MUST DIFFER ONLY WHERE THE AXES SAY THEY DO, and the count is
# derived rather than typed. lore-earn-safe is the template untouched and the
# other eleven are measured against it: a freeform setup moves five lines (four
# settings and its header), free colours move one, an outfits face moves one
# (bLooks) and a full face moves two (both Compat keys). diff prints two lines
# per changed line, so the expected total is that sum doubled.
#
# ⚠ THIS GUARD HAS EARNED ITS KEEP ONCE ALREADY. It caught a flavour built with
# cp instead of sed, which held identical settings and differed on all 190 lines
# because one was CRLF and the other LF. A number nobody can derive is a number
# nobody notices going wrong, which is why this one is computed.
for setup in lore freeform; do
  for colours in earn free; do
   for face in safe full; do
    flavour="$setup-$colours-$face"
    [ "$flavour" = "lore-earn-safe" ] && continue
    want=0
    [ "$setup" = "freeform" ] && want=$((want + 5))
    [ "$colours" = "free" ] && want=$((want + 1))
    [ "$face" = "full" ] && want=$((want + 3))
    # The installer stamp names the flavour, so it differs on every one.
    want=$((want + 1))
    want=$((want * 2))
    n="$(diff "$STAGE/settings/lore-earn-safe/FittingRoom.ini" \
              "$STAGE/settings/$flavour/FittingRoom.ini" |
         grep -c '^[<>]' || true)"
    [ "$n" = "$want" ] ||
      { echo "settings/$flavour differs from lore-earn-safe by $n lines, expected $want"; exit 1; }
   done
  done
done
# ⚠⚠ EVERY FLAVOUR MUST BE NAMED BY A conditionalFileInstalls PATTERN, and this
# is checked rather than trusted. A generated folder with no pattern behind it
# installs NO ini at all, which does not fall back to anything: it leaves the
# player on whatever default the DLL carries and ignores the answers they just
# gave, in silence. Twelve folders and twelve patterns, derived from the same
# three lists that built them. Menu Studio's packager learned this on twenty.
for setup in lore freeform; do
  for colours in earn free; do
   for face in safe full; do
    flavour="$setup-$colours-$face"
    grep -q "settings\\\\$flavour\\\\FittingRoom.ini" "$ROOT/fomod/ModuleConfig.xml" ||
      { echo "ModuleConfig.xml has no conditionalFileInstalls pattern installing"
        echo "settings\\$flavour\\FittingRoom.ini, so that combination of answers"
        echo "would install no ini and silently ignore what the player chose."
        exit 1; }
    # ⚠ TWICE: once as the live ini and once as installer.ini, the copy the
    # plugin seeds from when a leftover in Overwrite shadows the live one.
    [ "$(grep -c "settings\\\\$flavour\\\\FittingRoom.ini" "$ROOT/fomod/ModuleConfig.xml")" = "2" ] ||
      { echo "settings\\$flavour\\FittingRoom.ini must be installed twice in ModuleConfig.xml:"
        echo "as SKSE\\Plugins\\FittingRoom.ini and as SKSE\\Plugins\\FittingRoom\\installer.ini."
        exit 1; }
   done
  done
done

# ⚠ THE ONE FLAVOUR KEY THAT CHANGES WHAT RENDERS, and the loop above guards it
# in both directions. The other two decide what a thing costs and who may open
# the editor; bRequireWornForStyles decides whether a saved outfit draws AT ALL,
# so a free-form INI that kept the lore value would leave a new player's
# transmog invisible over the bare slots they were told to expect it on.
#
# ⚠ THE LORE SIDE SHIPS iCostMode=2, THE SEAMSTONE'S CHARGE (OS-129). A fresh
# stone starts EMPTY and an empty stone blocks styling outright, so this default
# is only honest while there is a way to fill it. There are two now: the
# editor's Refill button and, since 1.0.0, the stone's own item card in your
# inventory. If BOTH are ever removed or gated, this goes back to 1 in the same
# commit, or a new lore install gets an editor that cannot commit anything.

# Docs at the ARCHIVE ROOT (not installed to Data). Single-source-of-truth is
# GitHub: ship LICENSE (GPL requires it in the download) + a short README.txt
# pointer only. Full README/CHANGELOG/KNOWN-ISSUES/THIRD-PARTY live on GitHub.
cp "$ROOT/LICENSE" "$STAGE/"
cat > "$STAGE/README.txt" <<'EOF'
Fitting Room
ESO-style transmog for Skyrim: wear your saved outfits as a pure appearance
layer over your real gear. Stats, enchantments and weight never change.

Documentation, changelog, source code and issue tracker:
  https://github.com/maartenharms/fitting-room

Licensed under GPL-3.0 (see LICENSE).
EOF

(cd "$STAGE" && powershell -NoProfile -Command \
    "Compress-Archive -Path * -DestinationPath '$(cygpath -w "$ZIP")' -Force")

echo "packaged FOMOD: $ZIP"
unzip -l "$ZIP"
