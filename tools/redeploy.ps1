# Put this branch's built DLL and its string table into its own MO2 slot, then
# report which slot the game will actually load, whose strings it will read, and
# whether the DATA the plugin reads is there at all. Builds nothing and runs no
# CMake.
#
# Each Fitting Room branch owns a slot under MODS\mods named
# "Fitting Room [<branch leaf>]". A slot holds SKSE\Plugins\FittingRoom.dll,
# Interface\Translations\Fitting Room_<LANG>.txt,
# SKSE\Plugins\FittingRoom\icons and SKSE\Plugins\FittingRoom\PreviewFilters;
# the esp, scripts, presets, dye packs and unlock rules all come from the base
# "Fitting Room" mod, which stays enabled below every slot.
#
# ⚠ THE ROW ICONS JOINED THE SLOT FOR THE STRING TABLE'S EXACT REASON. They are
# not shared player content the way a dye pack is: the DLL names each PNG by
# filename, in the same commit as the code that draws it, so a branch that adds
# or renames one needs its own copy or the row silently falls back to a glyph.
# Sharing one copy in the base mod would mean whichever branch hand copied last
# owned it, which is the failure this script already exists to prevent for text.
#
# Enable exactly one slot in MO2. That is the switch for which branch is under
# test, and nothing a build does can change it.
#
#   & cmd.exe /c "tools\build.bat"     -> build, deploys nowhere
#   .\tools\redeploy.ps1               -> stage into this branch's slot, report
#   .\tools\redeploy.ps1 -VerifyOnly   -> report only, copy nothing
#
# ⚠ THE STRING TABLE MOVED INTO THE SLOT AND THIS HEADER USED TO SAY THE
# OPPOSITE. It said a slot must never carry one, because a branch's file can be
# missing keys another branch added. That was right about the danger and wrong
# about where the danger lives. SHARING is what breaks it. With one copy in the
# base mod, whichever branch hand copies last owns it and the other branch's
# keys are gone; measured on the live install rather than argued, that file
# carries $FR_HairStyleTooManyBones, which belongs to npc-weapon-transmog, and
# lacks the thirteen keys the dye economy added here, so it is already a hand
# merged blend of two branches and correct for neither. A string table is not
# shared content the way a dye pack is. It is the DLL's own table, written in
# the same commit as the code that reads it, and every key in it is asked for by
# exactly one build. One copy per slot means the slot that wins the DLL carries
# the table for that DLL, and no branch can reach another's.
#
# The reason this was worth fixing rather than documenting: the copy step simply
# did not exist, so the file went stale in silence. Nothing read it, nothing
# checked it, and the symptom surfaces as a raw $FR_Dye_LockedHeader in a pane a
# very long way from the deploy step that skipped it.
#
# ⚠ THAT A SLOT CAN OVERRIDE Interface\Translations WAS MEASURED, NOT ASSUMED.
# Under MO2 it is ordinary Data: USVFS merges it and resolves conflicts by
# priority like any other loose file. On this install 130 translation paths are
# carried by two to four enabled mods each, including skyui_se_english.txt
# across four and FUCK_ENGLISH.txt across the two runtime variants of FUCK
# itself, and Fitting Room_ENGLISH.txt is already in that contest between the
# base mod and a disabled FittingRoom-0.1.2. The override is not a hope, it is
# what the rest of the load order does all day.
#
# ⚠ THE DYE PACKS AND THE UNLOCK RULES STAY OUT OF THE SLOT, AND THAT IS STILL
# THE DESIGN. They are shared player content, so copying them from here would
# give every worktree a chance to overwrite the others, and unlike a string
# table they write permanently into a co-save. A Dyes directory reachable with
# no Unlocks beside it makes every rarity match no tier, no tier means no
# requirement, and the whole palette is written into the co-save of every
# character that loads, because unlocks are add only. Measured on the shipped
# files: rules report scanned=0 failed=0 healthy=TRUE and a level 1 character is
# granted all 318 colours, with a single warning line as the only sign. That is
# the difference that decides what a slot may carry. The worst a wrong string
# table can do is print a key.
#
# ⚠ AND THE NEW FAILURE MODE THIS CREATES, WHICH IS WHY THE CHECK BELOW EXISTS.
# A slot that carries a DLL and no string table now loses the strings to
# whatever sits below it, which is another branch's slot or the base mod, so the
# pane fills with raw keys while the DLL is exactly right. Staging cannot see
# that; only the load order can. So the winner is measured the same way the
# DLL's is, its keys are diffed against this branch's dist, and a shortfall is
# refused by name.

param([switch]$VerifyOnly)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# ⚠⚠ THE SLOT IS NAMED FOR THE BRANCH, NOT FOR THE DIRECTORY. This read the
# directory leaf while every branch had a worktree of its own, so the two were
# the same string and nothing distinguished them. The worktrees were retired on
# 2026-08-13 and the leaf became "Fitting Room", which would have staged into a
# slot called "Fitting Room [Fitting Room]": a folder MO2 has never heard of,
# sitting in no profile, while the game went on loading the real slot with an
# older DLL in it. Staging would have reported success and changed nothing the
# game could see.
#
# The branch is also the thing the slot was always really keyed on. A slot
# exists so one branch's DLL and string table cannot reach another's, and that
# is a property of the branch, not of where it happens to be checked out.
$branch = & git -C $root rev-parse --abbrev-ref HEAD 2>$null
if ($LASTEXITCODE -ne 0 -or -not $branch) {
    Write-Host "Could not read the git branch in '$root', so the MO2 slot cannot be named." -ForegroundColor Red
    Write-Host "Nothing was staged. Guessing the slot is how a deploy lands somewhere the game never looks." -ForegroundColor Red
    exit 1
}
$branch = $branch.Trim()
if ($branch -eq 'HEAD') {
    Write-Host "HEAD is detached, so there is no branch to name a slot after." -ForegroundColor Red
    Write-Host "Nothing was staged. Check out a branch first." -ForegroundColor Red
    exit 1
}
# feat/outfit-dye -> outfit-dye, which is the slot name MO2 already carries.
$branchLeaf = Split-Path -Leaf $branch
$slotName = "Fitting Room [$branchLeaf]"
$instance = 'C:\Games\Nolvus\Instances\Nolvus Awakening'
$modsDir  = Join-Path $instance 'MODS\mods'
$slotDll  = Join-Path $modsDir "$slotName\SKSE\Plugins\FittingRoom.dll"

# ⚠ dist, NOT build\deploy, and the asymmetry with the DLL above is deliberate.
# The DLL has to come from the build output because only that is the thing that
# was compiled. The string table is not compiled: CMake copies it out of dist
# verbatim, the FOMOD ships it out of dist, and dist is what git tracks, so dist
# is the source and build\deploy is a copy of it. Reading dist also means this
# still works with no build tree at all, and it errs in the harmless direction:
# a key the file has and the DLL never asks for costs nothing, while a key the
# DLL asks for and the file lacks is the whole bug. Whole directory rather than
# one filename so a second language needs no edit here.
$trSrcDir = Join-Path $root 'dist\Interface\Translations'
$slotTrDir = Join-Path $modsDir "$slotName\Interface\Translations"

# Same source-of-truth argument as the string table above: dist is what git
# tracks and what the FOMOD ships, so dist is what gets staged.
$iconSrcDir  = Join-Path $root 'dist\SKSE\Plugins\FittingRoom\icons'
$slotIconDir = Join-Path $modsDir "$slotName\SKSE\Plugins\FittingRoom\icons"

# ⚠ THE PREVIEW SCENE FIXUPS JOIN THE SLOT ON THE ROW ICONS' ARGUMENT, NOT THE
# DYE PACKS'. A fixup names a model path and a node inside it, and it ships in
# the same commit as the extractor rule it exists to correct, so a branch that
# adds one needs its own copy. Nothing in it reaches a co-save: the worst a
# wrong fixup can do is render a card badly, which is the same blast radius as
# a missing PNG and nothing like the 318 colours a stray Unlocks folder writes
# into a character permanently.
#
# It stayed out until now and cost a whole round for it. The Dawnbreaker fixup
# was hand copied into the slot, the census below reported it present, and the
# next fixup file will be forgotten the same way.
$scopeSrcDir  = Join-Path $root 'dist\SKSE\Plugins\FittingRoom\PreviewFilters'
$slotScopeDir = Join-Path $modsDir "$slotName\SKSE\Plugins\FittingRoom\PreviewFilters"

# ⚠ THE OVERLAY LOCATION TABLE JOINS THE SLOT ON THE PREVIEW FIXUPS' ARGUMENT,
# NOT THE DYE PACKS'. It is generated by tools/scan-paint-registrations.ps1 in
# the same commit as the reader that consumes it, so a branch that changes the
# format needs its own copy or the picker reads another branch's file. Nothing
# in it reaches a co-save: the worst a stale one can do is offer a texture on a
# location its pack did not name, which is untidy and not destructive.
#
# ⚠ AND ITS ABSENCE IS INVISIBLE FROM IN GAME, which is the reason it is staged
# at all rather than left to the base mod. With no table the picker still works
# and still shows every texture, so a deploy that forgot it looks exactly like a
# deploy where the filter simply had nothing to say.
$ovlSrcDir  = Join-Path $root 'dist\SKSE\Plugins\FittingRoom\Overlays'
$slotOvlDir = Join-Path $modsDir "$slotName\SKSE\Plugins\FittingRoom\Overlays"

# Read a translation file the way FUCK does and hand back its key set. The
# format is pinned: UTF-16LE with a BOM, $KEY<TAB>Value, CRLF. The encoding is
# sniffed rather than assumed because getting it wrong is a known way to break
# one of these silently, and a PowerShell Get-Content / Set-Content round trip
# is exactly how a file loses its BOM.
function Get-TranslationKeys {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $enc  = 'utf-16le'
        $text = [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
    } elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $enc  = 'utf-8 with BOM'
        $text = [System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
    } else {
        $enc  = 'no BOM'
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    }
    $keys = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in ($text -split "\r?\n")) {
        $l = $line.Trim()
        if (-not $l.StartsWith('$')) { continue }
        $k = ($l -split "`t")[0].Trim()
        if ($k) { [void]$keys.Add($k) }
    }
    return [pscustomobject]@{ Encoding = $enc; Keys = $keys; Bytes = $bytes }
}

# Where this tree actually builds to. The cache is the truth: build.bat
# passes OUTPUT_FOLDER on every configure, but a worktree that predates that
# may still be relying on the CMakeLists default, which is the live folder.
$built = $null
$cache = Join-Path $root 'build\release\CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $line = Select-String -LiteralPath $cache -Pattern '^OUTPUT_FOLDER[^=]*=(.*)$' | Select-Object -First 1
    if ($line) {
        $candidate = Join-Path $line.Matches[0].Groups[1].Value 'SKSE\Plugins\FittingRoom.dll'
        if (Test-Path -LiteralPath $candidate) { $built = $candidate }
    }
}
if (-not $built) {
    $fallback = Join-Path $root 'build\release\FittingRoom.dll'
    if (Test-Path -LiteralPath $fallback) { $built = $fallback }
}

$trWasNew = $false
$iconWasNew = $false
$scopeWasNew = $false
$ovlWasNew = $false

if (-not $VerifyOnly) {
    if (-not $built) {
        Write-Host "No built DLL found for '$branch'. Run tools\build.bat first." -ForegroundColor Red
        exit 1
    }

    # ⚠ THE STRING TABLE GOES FIRST, BEFORE THE DLL, and the CMake POST_BUILD
    # step orders itself the same way for the same reason. The DLL is the only
    # thing here the running game holds a lock on, so its copy is the one that
    # can fail and take the rest of the run with it. Text lands either way, and
    # a run that got the strings in and refused on the DLL leaves the install
    # closer to right than one that refused before writing anything.
    if (Test-Path -LiteralPath $trSrcDir) {
        New-Item -ItemType Directory -Force -Path $slotTrDir | Out-Null
        foreach ($src in (Get-ChildItem -LiteralPath $trSrcDir -Filter *.txt -File)) {
            $dst = Join-Path $slotTrDir $src.Name
            if (-not (Test-Path -LiteralPath $dst)) { $trWasNew = $true }
            # Copy-Item is byte exact, which is the point: the BOM and the CRLF
            # survive. Never rebuild one of these through Get-Content.
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            $t = Get-Item -LiteralPath $dst
            "staged into '$slotName': {0}, {1} bytes, {2}" -f $t.Name, $t.Length, $t.LastWriteTime
        }
    } else {
        Write-Host "No dist\Interface\Translations in '$branch', so no strings were staged." -ForegroundColor Yellow
    }

    # The row icons, before the DLL for the same reason the strings are: these
    # are not the file the running game holds a lock on, so they land even on a
    # run that then refuses on the DLL copy.
    if (Test-Path -LiteralPath $iconSrcDir) {
        New-Item -ItemType Directory -Force -Path $slotIconDir | Out-Null
        $n = 0
        foreach ($src in (Get-ChildItem -LiteralPath $iconSrcDir -File)) {
            $dst = Join-Path $slotIconDir $src.Name
            # ⚠ PER FILE, not just per directory. MO2's cached listing does not
            # know a path it has never seen, and one NEW PNG dropped into a
            # directory it already knows is the same trap as a new directory -
            # the file is simply absent from the virtual tree. Checking only the
            # directory missed exactly that case the first time.
            if (-not (Test-Path -LiteralPath $dst)) { $iconWasNew = $true }
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            $n++
        }
        "staged into '$slotName': {0} row icon file(s)" -f $n
        # ⚠ STAGING NEVER PRUNES. A PNG this branch stops shipping stays in the
        # slot until it is deleted by hand; harmless, since nothing loads a file
        # the DLL does not name, but the slot is not a mirror of dist.
        $stale = @(Get-ChildItem -LiteralPath $slotIconDir -File |
                   Where-Object { -not (Test-Path -LiteralPath (Join-Path $iconSrcDir $_.Name)) })
        foreach ($s in $stale) {
            Write-Host "  stale in the slot, not in dist: $($s.Name)" -ForegroundColor Yellow
        }
    }

    # The preview scene fixups, on the same terms as the icons above: before the
    # DLL, per file rather than per directory, and never pruning.
    if (Test-Path -LiteralPath $scopeSrcDir) {
        New-Item -ItemType Directory -Force -Path $slotScopeDir | Out-Null
        $n = 0
        foreach ($src in (Get-ChildItem -LiteralPath $scopeSrcDir -Filter *.json -File)) {
            $dst = Join-Path $slotScopeDir $src.Name
            if (-not (Test-Path -LiteralPath $dst)) { $scopeWasNew = $true }
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            $n++
        }
        "staged into '$slotName': {0} preview fixup file(s)" -f $n
        # ⚠ A STALE FIXUP HERE IS NOT HARMLESS THE WAY A STALE PNG IS. Nothing
        # loads a PNG the DLL does not name, but every *.json in this directory
        # is loaded and applied by path, so one this branch stopped shipping
        # goes on rewriting scenes and its cards go on keying around it.
        $stale = @(Get-ChildItem -LiteralPath $slotScopeDir -Filter *.json -File |
                   Where-Object { -not (Test-Path -LiteralPath (Join-Path $scopeSrcDir $_.Name)) })
        foreach ($s in $stale) {
            Write-Host "  stale fixup still loading, not in dist: $($s.Name)" -ForegroundColor Yellow
        }
    }

    # The overlay location table, on the same terms as the fixups above.
    if (Test-Path -LiteralPath $ovlSrcDir) {
        New-Item -ItemType Directory -Force -Path $slotOvlDir | Out-Null
        foreach ($src in (Get-ChildItem -LiteralPath $ovlSrcDir -Filter *.json -File)) {
            $dst = Join-Path $slotOvlDir $src.Name
            if (-not (Test-Path -LiteralPath $dst)) { $ovlWasNew = $true }
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            $t = Get-Item -LiteralPath $dst
            "staged into '$slotName': {0}, {1} bytes" -f $t.Name, $t.Length
        }
    } else {
        Write-Host "No dist\SKSE\Plugins\FittingRoom\Overlays in '$branch'. The Overlays picker will offer every texture on every location." -ForegroundColor Yellow
    }

    # The FSMP wig-xml overrides: the stripped xml copies and the map that
    # points at them, on the same terms as the overlay table.
    $fsmpSrcDir  = Join-Path $root 'dist\SKSE\Plugins\FittingRoom\fsmp'
    $slotFsmpDir = Join-Path $modsDir "$slotName\SKSE\Plugins\FittingRoom\fsmp"
    if (Test-Path -LiteralPath $fsmpSrcDir) {
        New-Item -ItemType Directory -Force -Path $slotFsmpDir | Out-Null
        $n = 0
        foreach ($src in (Get-ChildItem -LiteralPath $fsmpSrcDir -File)) {
            $dst = Join-Path $slotFsmpDir $src.Name
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            $n++
        }
        "staged into '$slotName': {0} fsmp override file(s)" -f $n
    }

    # The CBPC bounce interpolation profile: the per-actor amplitude override
    # ApplyBounceInterpolation('FittingRoom') reads. CBPC loads it at game
    # start from SKSE\Plugins, so it rides the slot beside the DLL.
    $cbpcSrc = Join-Path $root 'dist\SKSE\Plugins\CBPCBounceinterpolationconfig_FittingRoom.txt'
    if (Test-Path -LiteralPath $cbpcSrc) {
        $cbpcDst = Join-Path $modsDir "$slotName\SKSE\Plugins\CBPCBounceinterpolationconfig_FittingRoom.txt"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cbpcDst) | Out-Null
        Copy-Item -LiteralPath $cbpcSrc -Destination $cbpcDst -Force
        "staged into '$slotName': CBPC bounce interpolation profile"
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $slotDll) | Out-Null

    # The PDB, beside the DLL, because that is where a crash logger looks for
    # it. Without it a trainwreck report is an offset like "FittingRoom.dll
    # +007823C" and nothing more, which is what it was on 2026-08-05.
    #
    # Copied BEFORE the DLL for the same reason the strings are: the running
    # game locks the DLL and we want everything else already in place when that
    # copy is the one that fails. Missing is not an error - an older build tree
    # has no PDB and the deploy must still work.
    # ⚠ TWO PLACES, because the DLL and the PDB do not land together. $built is
    # normally the CMake OUTPUT_FOLDER copy, and CMake's copy step takes the DLL
    # only - the linker leaves the PDB back in build\release. Looking beside the
    # DLL alone found nothing and reported "no PDB in the build tree" while a
    # 40 MB one sat right there.
    $builtPdb = [IO.Path]::ChangeExtension($built, '.pdb')
    if (-not (Test-Path -LiteralPath $builtPdb)) {
        $builtPdb = Join-Path $root 'build\release\FittingRoom.pdb'
    }
    if (Test-Path -LiteralPath $builtPdb) {
        $slotPdb = [IO.Path]::ChangeExtension($slotDll, '.pdb')
        try {
            Copy-Item -LiteralPath $builtPdb -Destination $slotPdb -Force
        } catch {
            Write-Host "  PDB copy failed (crash logs will show offsets, not names): $($_.Exception.Message)" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  no PDB in the build tree; crash logs will show offsets, not names." -ForegroundColor Yellow
    }

    try {
        Copy-Item -LiteralPath $built -Destination $slotDll -Force
    } catch {
        # The DLL is the one file here the running game locks, which is why the
        # strings above are copied before it rather than after.
        Write-Host "Copy failed - is Skyrim running? $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
    $f = Get-Item -LiteralPath $slotDll
    "staged into '$slotName': {0} bytes, {1}" -f $f.Length, $f.LastWriteTime
    ""
}

# Which slot wins. modlist.txt is reverse priority: the earlier a mod appears,
# the higher its priority, so the FIRST enabled Fitting Room entry is the one
# whose FittingRoom.dll the game loads.
$profileName = $null
$ini = Join-Path $instance 'MO2\ModOrganizer.ini'
if (Test-Path -LiteralPath $ini) {
    $m = Select-String -LiteralPath $ini -Pattern 'selected_profile=@ByteArray\((.*)\)' | Select-Object -First 1
    if ($m) { $profileName = $m.Matches[0].Groups[1].Value }
}
if (-not $profileName) {
    Write-Host "Could not read the active MO2 profile, so NONE of the load-order, dye-data or string-table checks below ran. Nothing here says the deploy is good." -ForegroundColor Yellow
    exit 0
}

$modlist = Join-Path $instance "MODS\profiles\$profileName\modlist.txt"
"MO2 profile: $profileName"

if (Get-Process -Name 'ModOrganizer' -ErrorAction SilentlyContinue) {
    Write-Host "MO2 is running, so modlist.txt is whatever it was at launch. Anything you changed in the MO2 window since is not reflected below." -ForegroundColor Yellow
    if ($trWasNew) {
        # ⚠ A PATH THAT IS NEW TO A MOD FOLDER IS A DIFFERENT CASE FROM ONE THAT
        # WAS OVERWRITTEN. MO2 builds the virtual tree it hands USVFS from its
        # own cached directory listing, so overwriting a file it already knows
        # about takes effect immediately, while a file it has never seen may not
        # be in the tree at all. Everything below reads the disk, so it will
        # happily report a winner the game cannot see.
        Write-Host "Interface\Translations is NEW in '$slotName'. Refresh MO2 (F5) before launching, or the game may not see it however green this run looks." -ForegroundColor Yellow
    }
    if ($iconWasNew) {
        # Same cached-listing trap as the string table above, and it fails
        # quietly rather than loudly: a row whose PNG the game cannot see just
        # draws its old glyph, so the deploy looks fine and the icons look
        # unchanged.
        Write-Host "SKSE\Plugins\FittingRoom\icons is NEW in '$slotName'. Refresh MO2 (F5) before launching, or the weapon rows will fall back to their glyphs with nothing to say why." -ForegroundColor Yellow
    }
    if ($scopeWasNew) {
        # Quieter still than the icons: a fixup the game cannot see leaves the
        # card exactly as broken as it was before the fixup was written, so the
        # deploy looks fine and the fix looks wrong.
        Write-Host "SKSE\Plugins\FittingRoom\PreviewFilters is NEW in '$slotName'. Refresh MO2 (F5) before launching, or the card the fixup was written for will come back unchanged." -ForegroundColor Yellow
    }
    if ($ovlWasNew) {
        # The quietest of the lot: a table the game cannot see leaves the picker
        # working and unfiltered, which is what it did before the table existed.
        Write-Host "SKSE\Plugins\FittingRoom\Overlays is NEW in '$slotName'. Refresh MO2 (F5) before launching, or the Overlays picker will go on offering every texture on every location." -ForegroundColor Yellow
    }
}

# ⚠⚠ 'Fitting ?Room', NOT 'Fitting Room'. The pattern required the space, so
# every row MO2 named from a released archive - FittingRoom-1.1.5,
# FittingRoom-1.1.4, FittingRoom-1.0.0 - was invisible to the winner search
# below while being perfectly visible to the game. On 2026-08-29 an installed
# FittingRoom-1.1.5 sat above the dev slot carrying its own DLL, and this script
# printed "The game will load 'Fitting Room [outfit-dye]'. That is this branch."
# in green while the release DLL was the one that would load. A deploy check
# that cannot see half the mods called Fitting Room is worse than no check,
# because it is believed.
$entries = Select-String -LiteralPath $modlist -Pattern '^[+-]Fitting ?Room'
if (-not $entries) {
    Write-Host "No Fitting Room entries in modlist.txt. Restart MO2 so it picks up the new slots." -ForegroundColor Yellow
    Write-Host "Neither the dye-data check nor the string-table check below ran." -ForegroundColor Yellow
    exit 0
}

$winner = $null
foreach ($e in $entries) {
    $enabled = $e.Line.StartsWith('+')
    $name    = $e.Line.Substring(1)
    $dll     = Join-Path $modsDir "$name\SKSE\Plugins\FittingRoom.dll"
    if (Test-Path -LiteralPath $dll) {
        $size = (Get-Item -LiteralPath $dll).Length
        $has  = "$size bytes"
    } else {
        $has = "no DLL"
    }
    if ($enabled) { $state = 'enabled ' } else { $state = 'disabled' }
    "  {0}  {1,-38} {2}" -f $state, $name, $has
    if ($enabled -and $null -eq $winner -and (Test-Path -LiteralPath $dll)) { $winner = $name }
}

# ⚠ BOTH VERDICTS ARE MEASURED BEFORE EITHER IS ACTED ON. Exiting on the DLL
# check first would hide the data check behind it, and the two fail for
# unrelated reasons: a wrong slot is a load order the user fixes in the MO2
# window, and missing rules are a copy they never made. Whoever is reading this
# output wants both facts from one run.
$dllOk = $false
""
if ($null -eq $winner) {
    Write-Host "Nothing enabled carries a DLL. The game will load no Fitting Room plugin." -ForegroundColor Red
} elseif ($winner -ne $slotName) {
    Write-Host "The game will load '$winner', NOT this branch ('$slotName')." -ForegroundColor Red
    Write-Host "Move '$slotName' above it in MO2, or disable '$winner'." -ForegroundColor Red
} else {
    Write-Host "The game will load '$winner'. That is this branch." -ForegroundColor Green
    $dllOk = $true
}

# ---- the data the plugin reads --------------------------------------------
#
# ⚠ EVERY ENABLED MOD, not only the Fitting Room entries. USVFS merges the
# whole Data tree, so Dyes and Unlocks may legitimately arrive from different
# mods and either could be named anything, and a string table can be won by
# something with no Fitting Room in its name at all. Scoping this to entries
# matching "Fitting Room" would answer for the mod rather than for the game, and
# the game is what is being checked. 246 enabled mods cost about 90 ms here.
#
# The real Data folder is included at the bottom of the list. It loses every
# conflict to MO2, but a file sitting in it is still reachable, and a hand
# copy landing there instead of in a mod folder is exactly the mistake this
# check exists to catch.
#
# ONE WALK, BOTH QUESTIONS. The order this list is built in IS the priority
# order, so the first hit down it is the winner, which is the same rule the DLL
# block above uses on the same file.
$dyeCarriers = @()
$ruleCarriers = @()
$scopeCarriers = @()
$iconCarriers = @()
$ovlCarriers = @()
$sources = @()
$trSources = @()
foreach ($line in (Get-Content -LiteralPath $modlist)) {
    if ($line.StartsWith('+')) {
        $n = $line.Substring(1)
        $sources   += ,@($n, (Join-Path $modsDir ($n + '\SKSE\Plugins\FittingRoom')))
        $trSources += ,@($n, (Join-Path $modsDir ($n + '\Interface\Translations')))
    }
}
$sources   += ,@('the real Data folder', (Join-Path $instance 'STOCK GAME\Data\SKSE\Plugins\FittingRoom'))
$trSources += ,@('the real Data folder', (Join-Path $instance 'STOCK GAME\Data\Interface\Translations'))
foreach ($s in $sources) {
    if (-not (Test-Path -LiteralPath $s[1])) { continue }
    # ⚠ A DIRECTORY IS NOT A FILE. An Unlocks folder someone emptied loads as
    # cleanly as one that was never there, and frees exactly as much, so the
    # test is whether a *.json is in it.
    if (Get-ChildItem -LiteralPath (Join-Path $s[1] 'Dyes') -Filter *.json -File -ErrorAction SilentlyContinue) {
        $dyeCarriers += $s[0]
    }
    if (Get-ChildItem -LiteralPath (Join-Path $s[1] 'Unlocks') -Filter *.json -File -ErrorAction SilentlyContinue) {
        $ruleCarriers += $s[0]
    }
    if (Get-ChildItem -LiteralPath (Join-Path $s[1] 'icons') -Filter *.png -File -ErrorAction SilentlyContinue) {
        $iconCarriers += $s[0]
    }
    # Preview scene fixups (OS-191). Same "a directory is not a file" test as
    # the two above: the plugin logs the scope COUNT, so an empty folder and a
    # missing one read identically in the log, and a card still showing an SMP
    # proxy looks like a broken rule rather than a file that never arrived.
    if (Get-ChildItem -LiteralPath (Join-Path $s[1] 'PreviewFilters') -Filter *.json -File -ErrorAction SilentlyContinue) {
        $scopeCarriers += $s[0]
    }
    # The overlay location table. Same "a directory is not a file" test: the
    # plugin logs a row COUNT, so an empty folder and a missing one read the
    # same way, and an unfiltered picker looks like a filter with no opinion.
    if (Get-ChildItem -LiteralPath (Join-Path $s[1] 'Overlays') -Filter *.json -File -ErrorAction SilentlyContinue) {
        $ovlCarriers += $s[0]
    }
}

# ---- the strings, which this slot now carries ------------------------------
#
# ⚠ THE COPY ABOVE IS NOT THE ANSWER TO THIS QUESTION. Staging puts the table
# in the slot; the load order decides whether the game reads it. A slot sitting
# below another Fitting Room slot loses its strings to that one and shows a pane
# full of raw keys with a completely correct DLL loaded, which is the failure
# this whole block exists to name out loud.
#
# The diff is against THIS branch's dist, so what it measures is exactly the
# thing that matters: a key the running build asks for that the winning file
# does not answer renders as $FR_Something on screen. Nothing here can corrupt
# anything, unlike the Unlocks check below, so it is reported as what it is.
$trOk = $true
$trLines = @()
if (-not (Test-Path -LiteralPath $trSrcDir)) {
    $trLines += ,@('Yellow', "No dist\Interface\Translations in '$branch', so nothing could be checked.")
} else {
    foreach ($want in (Get-ChildItem -LiteralPath $trSrcDir -Filter *.txt -File)) {
        $mine = Get-TranslationKeys $want.FullName
        $carriers = @()
        foreach ($t in $trSources) {
            $p = Join-Path $t[1] $want.Name
            if (Test-Path -LiteralPath $p) { $carriers += ,@($t[0], $p) }
        }
        if (-not $carriers) {
            $trOk = $false
            $trLines += ,@('Red', "REFUSING: nothing enabled carries '$($want.Name)'. Every string in the pane will render as its raw key.")
            continue
        }
        $winName = $carriers[0][0]
        $live    = Get-TranslationKeys $carriers[0][1]
        $missing = @($mine.Keys | Where-Object { -not $live.Keys.Contains($_) } | Sort-Object)
        "strings '{0}': {1} wins with {2} of this build's {3} keys" -f `
            $want.Name, $winName, ($mine.Keys.Count - $missing.Count), $mine.Keys.Count
        foreach ($c in $carriers) {
            if ($c[0] -eq $winName) { continue }
            "  behind it: {0}" -f $c[0]
        }
        # ⚠ ENCODING FIRST, AND IT SHORT CIRCUITS THE REST. Unlike the three
        # verdicts this script holds back and prints together, these are not
        # independent: a file in the wrong encoding loads as nothing, so its key
        # list is not what the pane will show and a key count printed beside it
        # would be describing a file the game never read. Fix the encoding, run
        # again, and the diff below starts meaning something.
        if ($live.Encoding -ne 'utf-16le') {
            $trOk = $false
            $trLines += ,@('Red', "REFUSING: '$winName' carries '$($want.Name)' as $($live.Encoding), and FUCK reads these as UTF-16LE with a BOM. It will load as nothing at all, so every string in the pane renders as its raw key.")
            $trLines += ,@('Yellow', "Nothing else about that file was judged. A Get-Content / Set-Content round trip in PowerShell is the usual way one of these loses its BOM; copy it byte for byte instead.")
        } elseif ($missing.Count -gt 0) {
            $trOk = $false
            $trLines += ,@('Red', "REFUSING: '$winName' wins '$($want.Name)' and is missing $($missing.Count) key(s) this build asks for.")
            $show = $missing
            if ($show.Count -gt 12) { $show = $show[0..11] }
            foreach ($k in $show) { $trLines += ,@('Red', "    $k") }
            if ($missing.Count -gt 12) { $trLines += ,@('Red', "    ... and $($missing.Count - 12) more") }
            $trLines += ,@('Yellow', "Each of those renders on screen as its own key. Nothing is at risk beyond the text, so this is a run you can still play; it is just not a run whose screenshots mean anything.")
            if ($winName -ne $slotName) {
                # ⚠ TWO DIFFERENT FIXES AND THEY ARE NOT INTERCHANGEABLE. A slot
                # with no copy of the file needs a staging run; a slot that has
                # one and is simply outranked needs the MO2 window, and telling
                # someone to re-stage a file that is already there sends them to
                # run a command that changes nothing and reports the same thing.
                if (Test-Path -LiteralPath (Join-Path $slotTrDir $want.Name)) {
                    $trLines += ,@('Yellow', "'$slotName' already carries this file and just sits below '$winName'. Move it above '$winName' in MO2, or disable '$winName'.")
                } else {
                    $trLines += ,@('Yellow', "'$slotName' carries no copy of this file at all. Run this script without -VerifyOnly to stage it, then put '$slotName' above '$winName' in MO2.")
                }
            }
        } elseif ($winName -ne $slotName) {
            # Not a refusal: the keys are all there, so this run is readable.
            # Still worth a line, because it is winning by accident and the next
            # key this branch adds is the one that breaks it.
            $trLines += ,@('Yellow', "'$winName' wins '$($want.Name)', not '$slotName'. It happens to carry every key this build needs, so the pane reads correctly, but the next key added here will not be in it.")
        } elseif ((Get-FileHash -LiteralPath $want.FullName -Algorithm SHA256).Hash -ne
                  (Get-FileHash -LiteralPath $carriers[0][1] -Algorithm SHA256).Hash) {
            # Same keys, different bytes, so some VALUES differ. A key diff
            # cannot see this and it is the shape a hand edit leaves.
            $trLines += ,@('Yellow', "'$($want.Name)' has every key but does not match dist byte for byte, so some values differ. Harmless if that was deliberate.")
        } else {
            $trLines += ,@('Green', "'$slotName' wins '$($want.Name)' and it matches dist exactly.")
        }
    }
}

""
"dye packs from:    {0}" -f $(if ($dyeCarriers)  { $dyeCarriers -join ', ' }  else { 'nothing enabled' })
"unlock rules from: {0}" -f $(if ($ruleCarriers) { $ruleCarriers -join ', ' } else { 'nothing enabled' })
"scene fixups from: {0}" -f $(if ($scopeCarriers) { $scopeCarriers -join ', ' } else { 'nothing enabled' })
"row icons from:    {0}" -f $(if ($iconCarriers) { $iconCarriers -join ', ' } else { 'nothing enabled' })
"overlay table from:{0}" -f $(if ($ovlCarriers) { ' ' + ($ovlCarriers -join ', ') } else { ' nothing enabled' })

# Not a refusal, on the row icons' argument: the picker works either way and
# errs towards showing more art. Worth a line because it is invisible from in
# game, and "the filter did nothing" is otherwise indistinguishable from "the
# table did not load".
if (-not $ovlCarriers) {
    Write-Host "No overlay location table is reachable, so the Overlays picker will offer every texture on every location." -ForegroundColor Yellow
} elseif ($ovlCarriers[0] -ne $slotName) {
    Write-Host "The overlay location table comes from '$($ovlCarriers[0])', not '$slotName'. Fine while both ship the same format; a change to it here will not be read." -ForegroundColor Yellow
}

# Not a refusal. A row with no PNG draws the glyph it drew before the pictures
# existed, so this only ever costs looks - but it is invisible from in game, and
# "the icons did not change" is otherwise indistinguishable from "the icons did
# not load".
if (-not $iconCarriers) {
    Write-Host "No row icons are reachable, so the weapon and shield rows will draw their old Font Awesome glyphs." -ForegroundColor Yellow
} elseif ($iconCarriers[0] -ne $slotName) {
    Write-Host "Row icons come from '$($iconCarriers[0])', not '$slotName'. Fine while both ship the same filenames; a PNG this branch adds will not be in it." -ForegroundColor Yellow
}

$dataOk = $true
if ($dyeCarriers -and -not $ruleCarriers) {
    $dataOk = $false
    ""
    Write-Host "REFUSING: a Dyes directory is reachable and no Unlocks directory is." -ForegroundColor Red
    Write-Host "Every dye carries a rarity, every rarity resolves through a tier, and a" -ForegroundColor Red
    Write-Host "rarity with no tier has no requirement at all. Promotion runs on every load," -ForegroundColor Red
    Write-Host "so the whole palette is written permanently into the co-save of whatever" -ForegroundColor Red
    Write-Host "character you test with. Unlocks are add only: fixing this afterwards takes" -ForegroundColor Red
    Write-Host "none of it back, and the save is the thing that is spoiled, not the install." -ForegroundColor Red
    ""
    Write-Host "Copy BOTH directories into the base 'Fitting Room' mod, not into a slot:" -ForegroundColor Yellow
    Write-Host "  Copy-Item -Recurse -Force '$root\dist\SKSE\Plugins\FittingRoom\Dyes'    '$modsDir\Fitting Room\SKSE\Plugins\FittingRoom\'" -ForegroundColor Yellow
    Write-Host "  Copy-Item -Recurse -Force '$root\dist\SKSE\Plugins\FittingRoom\Unlocks' '$modsDir\Fitting Room\SKSE\Plugins\FittingRoom\'" -ForegroundColor Yellow
    Write-Host "Those two only. The string table is staged per slot by this script and must" -ForegroundColor Yellow
    Write-Host "not be hand copied into the base mod: doing that is what left the base copy a" -ForegroundColor Yellow
    Write-Host "blend of two branches and correct for neither." -ForegroundColor Yellow
} elseif ($ruleCarriers -and -not $dyeCarriers) {
    # Harmless, and worth a line: rules with no colours to gate is what a
    # half-finished copy of the pair looks like from the other side.
    Write-Host "Unlock rules are reachable and no dye packs are, so the rules gate nothing. Check the Dyes copy." -ForegroundColor Yellow
} elseif (-not $dyeCarriers) {
    Write-Host "Neither Dyes nor Unlocks is reachable, so the dye pane will have no colours. Nothing is at risk, but there is nothing to field test either." -ForegroundColor Yellow
} else {
    Write-Host "Dye packs and unlock rules are both reachable." -ForegroundColor Green
}

# ⚠ HELD BACK TO HERE ON PURPOSE, the same rule the two verdicts above follow.
# All three are measured before any of them is acted on, because they fail for
# unrelated reasons and whoever is reading this output wants every fact from one
# run rather than the first one that happened to exit.
""
foreach ($l in $trLines) { Write-Host $l[1] -ForegroundColor $l[0] }

if ($dllOk -and $dataOk -and $trOk) { exit 0 }
exit 1
