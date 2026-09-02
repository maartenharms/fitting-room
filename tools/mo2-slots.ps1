# Register the per-branch Fitting Room slots in the active MO2 profile, and
# pick which one is enabled.
#
# Run this with MO2 CLOSED. MO2 holds modlist.txt in memory and rewrites it on
# exit, so anything edited while it is open is discarded. The script refuses to
# run rather than let that happen.
#
#   .\tools\mo2-slots.ps1                       -> show what it would do
#   .\tools\mo2-slots.ps1 -Enable auto-rules -Apply
#
# The slots go directly above the base "Fitting Room" mod, which stays enabled
# and keeps providing the esp, scripts, icons, presets and translations. A slot
# holds only FittingRoom.dll. Exactly one slot is enabled at a time.

param(
    [string]$Enable = 'auto-rules',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

$worktrees = @('auto-rules', 'npc-weapon-transmog', 'outfit-dye', 'dye-spike', 'requip', 'overlays')
$instance  = 'C:\Games\Nolvus\Instances\Nolvus Awakening'
$baseEntry = 'Fitting Room'

if ($worktrees -notcontains $Enable) {
    Write-Host "-Enable must be one of: $($worktrees -join ', ')" -ForegroundColor Red
    exit 1
}

if (Get-Process -Name 'ModOrganizer' -ErrorAction SilentlyContinue) {
    Write-Host "MO2 is running. Close it fully, then run this again." -ForegroundColor Red
    Write-Host "MO2 rewrites modlist.txt from memory when it exits, so an edit made now would be thrown away." -ForegroundColor Red
    exit 1
}

$ini = Join-Path $instance 'MO2\ModOrganizer.ini'
$m = Select-String -LiteralPath $ini -Pattern 'selected_profile=@ByteArray\((.*)\)' | Select-Object -First 1
if (-not $m) {
    Write-Host "Could not read selected_profile from $ini" -ForegroundColor Red
    exit 1
}
$profileName = $m.Matches[0].Groups[1].Value
$modlist = Join-Path $instance "MODS\profiles\$profileName\modlist.txt"
"profile : $profileName"
"modlist : $modlist"
""

$lines = [System.IO.File]::ReadAllLines($modlist)

# Drop any slot lines already present, so re-running just re-sorts rather than
# accumulating duplicates.
$slotNames = @()
foreach ($w in $worktrees) { $slotNames += "Fitting Room [$w]" }
$kept = @()
foreach ($line in $lines) {
    $bare = $line
    if ($bare.Length -gt 0 -and ($bare[0] -eq '+' -or $bare[0] -eq '-')) { $bare = $bare.Substring(1) }
    if ($slotNames -notcontains $bare) { $kept += $line }
}

$anchor = -1
for ($i = 0; $i -lt $kept.Count; $i++) {
    $bare = $kept[$i]
    if ($bare.Length -gt 0 -and ($bare[0] -eq '+' -or $bare[0] -eq '-')) { $bare = $bare.Substring(1) }
    if ($bare -eq $baseEntry) { $anchor = $i; break }
}
if ($anchor -lt 0) {
    Write-Host "No '$baseEntry' entry in modlist.txt. Nothing to anchor the slots to." -ForegroundColor Red
    exit 1
}
if (-not $kept[$anchor].StartsWith('+')) {
    Write-Host "The base '$baseEntry' mod is DISABLED. Enable it in MO2 first: the slots only carry the DLL." -ForegroundColor Red
    exit 1
}

# Earlier line means higher priority, so the enabled slot goes first.
$insert = @()
$insert += "+Fitting Room [$Enable]"
foreach ($w in $worktrees) {
    if ($w -ne $Enable) { $insert += "-Fitting Room [$w]" }
}

$out = @()
if ($anchor -gt 0) { $out += $kept[0..($anchor - 1)] }
$out += $insert
$out += $kept[$anchor..($kept.Count - 1)]

"planned, in priority order:"
foreach ($l in $insert) { "  $l" }
"  $($kept[$anchor])   <- base, keeps everything that is not the DLL"
""

if (-not $Apply) {
    Write-Host "Dry run. Re-run with -Apply to write it." -ForegroundColor Yellow
    exit 0
}

$stamp  = Get-Date -Format 'yyyyMMdd-HHmm'
$backup = "$modlist.slots-$stamp.bak"
Copy-Item -LiteralPath $modlist -Destination $backup -Force
"backed up to $backup"

# Retire the DLL sitting in the base mod. Left there it is a silent fallback:
# with no slot enabled the game would quietly load whichever branch happened to
# copy over it last, which is the failure this whole arrangement exists to end.
# Moved aside, not deleted, and every branch's build is staged in its own slot
# anyway.
$baseDll = Join-Path $instance "MODS\mods\$baseEntry\SKSE\Plugins\FittingRoom.dll"
if (Test-Path -LiteralPath $baseDll) {
    $aside = "$baseDll.retired-$stamp"
    Move-Item -LiteralPath $baseDll -Destination $aside -Force
    "base mod's DLL moved aside to $aside"
    "  the base now provides only the esp, scripts, icons, presets and translations"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllLines($modlist, $out, $utf8NoBom)
Write-Host "modlist.txt updated. '$Enable' is the enabled slot." -ForegroundColor Green
Write-Host "Start MO2 and confirm the four slots sit directly above 'Fitting Room'." -ForegroundColor Green
