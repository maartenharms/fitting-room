# Build the overlay location table by reading what each pack REGISTERS with
# RaceMenu, rather than by guessing from a file name.
#
# ⚠⚠ RaceMenu'S PER LOCATION LISTS ARE AUTHORED, NOT DERIVED, and that is why
# this file exists. skee64.dll contains no texture enumeration at all: the only
# overlay paths in it are the "Body [Ovl{}]" node templates and the default
# texture. The lists live in Papyrus, in RaceMenuBase's four arrays
# (_textures_body, _textures_hand, _textures_feet, _textures_face), which each
# pack fills from its own OnBodyPaintRequest and friends by calling
# AddBodyPaint, AddHandPaint, AddFeetPaint, AddFacePaint or AddWarpaint.
# Extracted from RaceMenu.bsa with BSArch and read on 2026-08-16.
#
# So there is no folder convention and no name rule to recover. Measured on the
# reference load order: of 2047 installed overlay textures, 1323 carry no
# location word in the file name, and the same pack directories (Community
# Overlays CO 2 and CO 3) are used on all four locations by the installed
# presets. A name rule would hide most of the library from every location.
#
# What this does instead is read the pack's own compiled Papyrus and record
# which list each texture was registered into. Anything no script mentions is
# absent from the table, and the page SHOWS it everywhere rather than hiding it:
# a texture missing from a list is invisible and unexplainable, one offered in
# the wrong place is merely untidy.
#
#   .\tools\scan-paint-registrations.ps1
#   .\tools\scan-paint-registrations.ps1 -Mods <path> -Out <path> -Verbose
#
# ⚠ RUN IT AGAIN WHEN THE REFERENCE LOAD ORDER CHANGES. The table ships as data
# and describes the packs that were installed when it was generated.

[CmdletBinding()]
param(
    [string]$Mods = 'C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods',
    [string]$Out  = "$PSScriptRoot\..\dist\SKSE\Plugins\FittingRoom\Overlays\overlay-locations.json",
    [string]$Bsa  = 'C:\Games\Nolvus\Instances\Nolvus Awakening\TOOLS\BSArch\BSArch.exe'
)

$ErrorActionPreference = 'Stop'

# ---- the .pex reader -------------------------------------------------------
#
# ⚠ SKYRIM'S .pex IS BIG ENDIAN. Fallout 4's is not, and a reader written for
# that one parses this header into nonsense without failing: the magic still
# matches because it is byte compared. Verified against a real file, whose
# first eight bytes are FA 57 C0 DE 03 02 00 01 - magic, major 3, minor 2,
# game id 1.

class PexReader {
    [byte[]]$B
    [int]$P

    PexReader([byte[]]$bytes) { $this.B = $bytes; $this.P = 0 }

    [byte] U8() { $v = $this.B[$this.P]; $this.P += 1; return $v }
    [int]  U16() { $v = ([int]$this.B[$this.P] -shl 8) -bor [int]$this.B[$this.P + 1]; $this.P += 2; return $v }
    [uint32] U32() {
        $v = ([uint32]$this.B[$this.P] -shl 24) -bor ([uint32]$this.B[$this.P + 1] -shl 16) -bor
             ([uint32]$this.B[$this.P + 2] -shl 8) -bor [uint32]$this.B[$this.P + 3]
        $this.P += 4
        return $v
    }
    [void] Skip([int]$n) { $this.P += $n }
    [string] Wstring() {
        $len = $this.U16()
        $s = [System.Text.Encoding]::GetEncoding(1252).GetString($this.B, $this.P, $len)
        $this.P += $len
        return $s
    }
}

# Operand counts, from the Papyrus opcode table. The three call opcodes take
# their fixed operands and then a variadic list whose length arrives as an
# integer VarData.
$script:Arity = @(
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3,   #  0 nop .. 9 imod
    2, 2, 2, 2, 2,                  # 10 not, ineg, fneg, assign, cast
    3, 3, 3, 3, 3,                  # 15 cmp_eq .. 19 cmp_ge
    1, 2, 2,                        # 20 jmp, jmpt, jmpf
    3, 2, 3,                        # 23 callmethod, 24 callparent, 25 callstatic
    1, 3, 3, 3,                     # 26 return, strcat, propget, propset
    2, 2, 3, 3, 4, 4                # 30 array_create .. 35 array_rfindelement
)
$script:Variadic = @{ 23 = $true; 24 = $true; 25 = $true }

function Read-VarData([PexReader]$r, [string[]]$strings) {
    $type = $r.U8()
    switch ($type) {
        0 { return @{ Kind = 'null';   Value = $null } }
        1 { return @{ Kind = 'ident';  Value = $strings[$r.U16()] } }
        2 { return @{ Kind = 'string'; Value = $strings[$r.U16()] } }
        3 { $v = $r.U32(); return @{ Kind = 'int'; Value = [int]$v } }
        4 { $r.Skip(4); return @{ Kind = 'float'; Value = $null } }
        5 { $v = $r.U8(); return @{ Kind = 'bool'; Value = [bool]$v } }
        default { throw "unknown VarData type $type at offset $($r.P)" }
    }
}

function Read-Function([PexReader]$r, [string[]]$strings, [System.Collections.ArrayList]$calls) {
    $r.Skip(2)          # return type
    $r.Skip(2)          # doc string
    $r.Skip(4)          # user flags
    $r.Skip(1)          # flags
    $n = $r.U16(); $r.Skip($n * 4)   # params:  name + type
    $n = $r.U16(); $r.Skip($n * 4)   # locals:  name + type
    $count = $r.U16()
    for ($i = 0; $i -lt $count; ++$i) {
        $op = $r.U8()
        if ($op -ge $script:Arity.Count) { throw "unknown opcode $op at offset $($r.P)" }
        $args = @()
        for ($a = 0; $a -lt $script:Arity[$op]; ++$a) { $args += (Read-VarData $r $strings) }
        if ($script:Variadic.ContainsKey([int]$op)) {
            $vc = Read-VarData $r $strings
            if ($vc.Kind -ne 'int') { throw "variadic count was $($vc.Kind) at offset $($r.P)" }
            for ($a = 0; $a -lt $vc.Value; ++$a) { $args += (Read-VarData $r $strings) }
        }
        # callmethod's first operand is the method name; callstatic's second is.
        if ($op -eq 23 -or $op -eq 24) {
            $null = $calls.Add(@{ Name = [string]$args[0].Value; Args = $args })
        } elseif ($op -eq 25) {
            $null = $calls.Add(@{ Name = [string]$args[1].Value; Args = $args })
        }
    }
}

function Get-PexCalls([string]$path) {
    $r = [PexReader]::new([System.IO.File]::ReadAllBytes($path))
    if ($r.B.Length -lt 8 -or $r.B[0] -ne 0xFA -or $r.B[1] -ne 0x57 -or
        $r.B[2] -ne 0xC0 -or $r.B[3] -ne 0xDE) {
        throw 'not a .pex'
    }
    $r.Skip(4)              # magic
    $r.Skip(1); $r.Skip(1)  # major, minor
    $r.Skip(2)              # game id
    $r.Skip(8)              # compilation time
    $null = $r.Wstring()    # source file
    $null = $r.Wstring()    # user
    $null = $r.Wstring()    # machine

    $count = $r.U16()
    $strings = New-Object string[] $count
    for ($i = 0; $i -lt $count; ++$i) { $strings[$i] = $r.Wstring() }

    if ($r.U8() -ne 0) {
        $r.Skip(8)                      # modification time
        $fns = $r.U16()
        for ($i = 0; $i -lt $fns; ++$i) {
            $r.Skip(2 + 2 + 2 + 1)      # object, state, function, type
            $lines = $r.U16()
            $r.Skip($lines * 2)
        }
    }
    $flags = $r.U16()
    for ($i = 0; $i -lt $flags; ++$i) { $r.Skip(3) }   # name + flag index

    $calls = New-Object System.Collections.ArrayList
    $objects = $r.U16()
    for ($o = 0; $o -lt $objects; ++$o) {
        $r.Skip(2)                       # object name
        $null = $r.U32()                 # size, not needed: everything is read
        $r.Skip(2 + 2 + 4 + 2)           # parent, doc, user flags, auto state

        $vars = $r.U16()
        for ($i = 0; $i -lt $vars; ++$i) {
            $r.Skip(2 + 2 + 4)
            $null = Read-VarData $r $strings
        }
        $props = $r.U16()
        for ($i = 0; $i -lt $props; ++$i) {
            $r.Skip(2 + 2 + 2 + 4)
            $pf = $r.U8()
            if ($pf -band 4) {
                $r.Skip(2)               # auto var name
            } else {
                if (($pf -band 5) -eq 1) { Read-Function $r $strings $calls }
                if (($pf -band 6) -eq 2) { Read-Function $r $strings $calls }
            }
        }
        $states = $r.U16()
        for ($i = 0; $i -lt $states; ++$i) {
            $r.Skip(2)                   # state name
            $sf = $r.U16()
            for ($j = 0; $j -lt $sf; ++$j) {
                $r.Skip(2)               # function name
                Read-Function $r $strings $calls
            }
        }
    }
    return $calls
}

# ---- which call means which list ------------------------------------------
#
# ⚠ AddWarpaint IS NOT AN OVERLAY REGISTRATION and it is recorded separately.
# A warpaint goes into the head's tint mask layer, which is a different system
# from the [Ovl] nodes this page drives. Whether a warpaint-only pack should be
# offered on the Face location is a judgement the table leaves to the loader by
# recording the fact rather than a verdict.

$script:Calls = @{
    'addbodypaint'   = 'body';   'addbodypaintex'   = 'body'
    'addhandpaint'   = 'hands';  'addhandpaintex'   = 'hands'
    'addfeetpaint'   = 'feet';   'addfeetpaintex'   = 'feet'
    'addfacepaint'   = 'face';   'addfacepaintex'   = 'face'
    'addwarpaint'    = 'warp';   'addwarpaintex'    = 'warp'
}

function Add-Registration($table, $bySource, [string]$list, [string]$raw, [string]$origin) {
    if ([string]::IsNullOrWhiteSpace($raw)) { return }
    $p = $raw -replace '/', '\'
    $p = $p.TrimStart('\')
    if ($p -notmatch '\.dds$') { return }
    # The table is keyed the way the override wants the path: relative to
    # textures\, backslashes, lower case for comparison.
    if ($p -match '^(?i)textures\\') { $p = $p.Substring(9) }
    $key = $p.ToLowerInvariant()
    if (-not $table.ContainsKey($key)) { $table[$key] = New-Object System.Collections.Generic.HashSet[string] }
    $null = $table[$key].Add($list)
    # ⚠ WHICH SCRIPT SAID IT IS KEPT TOO, per texture. The loader's corrections
    # file names a registration script and says what that script's warpaint
    # registrations really are (Shep's two collections register 264 BODY
    # tattoos with AddWarpaint), and a correction keyed by script name needs
    # the script's own list of textures to apply to. The mod folder in front
    # of the script name changes from rig to rig; the script's file name does
    # not, and the loader matches on that tail.
    if (-not $bySource.ContainsKey($origin)) { $bySource[$origin] = New-Object System.Collections.Generic.HashSet[string] }
    $null = $bySource[$origin].Add($key)
}

# ---- walk the load order ---------------------------------------------------

# The registration names, as raw ASCII, for the cheap pre-filter that decides
# whether a file is worth parsing at all.
$script:Needles = @('AddBodyPaint', 'AddHandPaint', 'AddFeetPaint', 'AddFacePaint', 'AddWarpaint')

# Search a file for any of a set of ASCII needles without holding it in memory.
# The overlap is one needle length short of the buffer so a match straddling a
# chunk boundary is still found.
function Test-FileContainsAscii([string]$path, [string[]]$needles) {
    $max = 0
    foreach ($n in $needles) { if ($n.Length -gt $max) { $max = $n.Length } }
    $size = 8MB
    $buf = New-Object byte[] ($size + $max)
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $carry = 0
        while ($true) {
            $read = $fs.Read($buf, $carry, $size)
            if ($read -le 0) { return $false }
            $have = $carry + $read
            $txt = [System.Text.Encoding]::ASCII.GetString($buf, 0, $have)
            foreach ($n in $needles) { if ($txt.Contains($n)) { return $true } }
            $carry = [Math]::Min($max, $have)
            [System.Array]::Copy($buf, $have - $carry, $buf, 0, $carry)
        }
    } finally { $fs.Dispose() }
    return $false
}

$table    = @{}
$bySource = @{}
$sources  = New-Object System.Collections.ArrayList
$failed   = New-Object System.Collections.ArrayList

function Scan-PexFile([string]$path, [string]$origin) {
    try { $calls = Get-PexCalls $path }
    catch { $null = $failed.Add("$origin : $($_.Exception.Message)"); return }
    $hits = 0
    foreach ($c in $calls) {
        if (-not $c.Name) { continue }
        $list = $script:Calls[$c.Name.ToLowerInvariant()]
        if (-not $list) { continue }
        foreach ($a in $c.Args) {
            if ($a.Kind -eq 'string') { Add-Registration $table $bySource $list $a.Value $origin; ++$hits }
        }
    }
    if ($hits -gt 0) { $null = $sources.Add($origin) }
}

Write-Host "scanning loose scripts under $Mods"
$pex = Get-ChildItem -LiteralPath $Mods -Recurse -File -Filter *.pex -ErrorAction SilentlyContinue
foreach ($f in $pex) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $txt = [System.Text.Encoding]::ASCII.GetString($bytes)
    if ($txt -notmatch 'Add(Body|Hand|Feet|Face|War)[Pp]aint') { continue }
    Scan-PexFile $f.FullName $f.FullName.Substring($Mods.Length + 1)
}

# ⚠ THE PACKED SCRIPTS COUNT TOO. Community Overlays 1, SFO and WNB ship their
# registration script inside a BSA, and a scan of loose files alone reports
# those three packs as unregistered and therefore unplaceable.
if (Test-Path -LiteralPath $Bsa) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("frpex_" + [System.Guid]::NewGuid().ToString('N'))
    foreach ($archive in (Get-ChildItem -LiteralPath $Mods -Recurse -File -Filter *.bsa -ErrorAction SilentlyContinue)) {
        # ⚠ STREAMED, NOT ReadAllBytes. A load order holds multi gigabyte
        # archives and reading one whole into a .NET string threw
        # OutOfMemoryException on the first run of this script.
        if (-not (Test-FileContainsAscii $archive.FullName $script:Needles)) { continue }
        Write-Host "unpacking $($archive.Name)"
        $null = New-Item -ItemType Directory -Force -Path $tmp
        & $Bsa unpack $archive.FullName $tmp | Out-Null
        foreach ($f in (Get-ChildItem -LiteralPath $tmp -Recurse -File -Filter *.pex -ErrorAction SilentlyContinue)) {
            Scan-PexFile $f.FullName ($archive.FullName.Substring($Mods.Length + 1) + '!' + $f.Name)
        }
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "BSArch not found at $Bsa, packed scripts skipped" -ForegroundColor Yellow
}

# ---- report and write ------------------------------------------------------

$byList = @{}
foreach ($k in $table.Keys) { foreach ($l in $table[$k]) { $byList[$l] = [int]$byList[$l] + 1 } }
""
"scripts that registered something : $($sources.Count)"
"textures placed                   : $($table.Count)"
foreach ($l in ($byList.Keys | Sort-Object)) { "  {0,-6} {1}" -f $l, $byList[$l] }
if ($failed.Count -gt 0) {
    ""
    "UNREADABLE ($($failed.Count)):"
    $failed | ForEach-Object { "  $_" }
}

$entries = [ordered]@{}
foreach ($k in ($table.Keys | Sort-Object)) {
    $entries[$k] = (($table[$k] | Sort-Object) -join ',')
}

# The same sources again, each with the keys it registered, sorted so two runs
# over the same load order write the same file.
$perSource = [ordered]@{}
foreach ($s in ($sources | Sort-Object)) {
    if ($bySource.ContainsKey($s)) { $perSource[$s] = @($bySource[$s] | Sort-Object) }
    else { $perSource[$s] = @() }
}

$doc = [ordered]@{
    note      = 'Generated by tools/scan-paint-registrations.ps1. Each paints key is a texture path relative to textures\, lower case, backslashes. The value lists the RaceMenu paint lists the pack registered it into: body, hands, feet, face, warp. A texture absent from this file is offered on every location. by_source lists, per registration script, the keys that script registered; overlay-locations-fixups.json corrects a script by its file name.'
    sources   = @($sources | Sort-Object)
    by_source = $perSource
    paints    = $entries
}

$dir = Split-Path -Parent $Out
if (-not (Test-Path -LiteralPath $dir)) { $null = New-Item -ItemType Directory -Force -Path $dir }
$json = $doc | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($Out, $json, (New-Object System.Text.UTF8Encoding($false)))
""
"written: $Out"
