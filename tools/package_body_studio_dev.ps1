[CmdletBinding()]
param(
    [string]$BuildId
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

# ⚠ THE BRANCH LIST IS THE POINT, NOT feat/body-studio BY NAME. The guard
# exists so a development package can name the exact source it came from, and
# after the integration merge the Body Studio sources live on the development
# line rather than on the feature branch alone. Pinning the old name here does
# not keep the package honest, it just makes it unbuildable from the branch
# that now carries the code. Add a branch when the work moves; never drop the
# check, or a package can be cut from an arbitrary tree.
$allowedBranches = @('feat/outfit-dye', 'feat/body-studio')
$sourceBranch = (git branch --show-current).Trim()
if ($allowedBranches -notcontains $sourceBranch) {
    throw "Body Studio development packages may only be built from $($allowedBranches -join ' or '); current branch is '$sourceBranch'."
}
if (git status --porcelain) {
    throw 'Commit or remove working-tree changes before packaging; the manifest must identify exact source.'
}

$sourceCommit = (git rev-parse HEAD).Trim()
if (-not $BuildId) {
    $BuildId = $sourceCommit.Substring(0, 12)
}
if ($BuildId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $BuildId -eq 'release') {
    throw "Invalid development build id '$BuildId'."
}

& "$PSScriptRoot\build_body_studio_dev.bat" $BuildId
if ($LASTEXITCODE -ne 0) {
    throw "Body Studio development build failed; see tools/build_body_studio_dev.log."
}

$deploy = Join-Path $repoRoot 'build/body-studio-dev/deploy'
$releaseRoot = Join-Path $repoRoot 'release/body-studio-dev'
$packageName = "Fitting Room - Body Studio Dev $BuildId"
$stage = Join-Path $releaseRoot $packageName
$zip = Join-Path $releaseRoot "$packageName.zip"

New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -Path (Join-Path $deploy '*') -Destination $stage -Recurse -Force

# Shared, read-only integration assets. These intentionally keep their release
# virtual names; the development channel differs only where state or identity
# can leak. Mod managers may report identical-file conflicts for these assets.
Copy-Item -LiteralPath (Join-Path $repoRoot 'dist/Scripts') -Destination $stage -Recurse
$inventoryInjectorTarget = Join-Path $stage 'SKSE/Plugins/InventoryInjector'
New-Item -ItemType Directory -Force -Path $inventoryInjectorTarget | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'dist/SKSE/Plugins/InventoryInjector/FittingRoom.json') -Destination $inventoryInjectorTarget

$devRoot = Join-Path $stage "SKSE/Plugins/FittingRoom.BodyStudioDev/$BuildId"
$expectedDll = Join-Path $stage 'SKSE/Plugins/FittingRoom.dll'
if (-not (Test-Path -LiteralPath $expectedDll -PathType Leaf)) {
    throw 'Development package is missing the one virtual FittingRoom.dll.'
}
$dlls = @(Get-ChildItem -LiteralPath $stage -Filter '*.dll' -File -Recurse)
if ($dlls.Count -ne 1 -or $dlls[0].FullName -ne $expectedDll) {
    throw 'Development package must contain exactly one DLL, at SKSE/Plugins/FittingRoom.dll.'
}
if (-not (Test-Path -LiteralPath $devRoot -PathType Container)) {
    throw "Development data root is missing: $devRoot"
}
$devBuildRoots = @(Get-ChildItem -LiteralPath (Split-Path -Parent $devRoot) -Directory)
if ($devBuildRoots.Count -ne 1 -or $devBuildRoots[0].FullName -ne $devRoot) {
    throw 'Development package contains a stale or unexpected build-specific data root.'
}
if ((Test-Path -LiteralPath (Join-Path $stage 'SKSE/Plugins/FittingRoom')) -or
    (Test-Path -LiteralPath (Join-Path $stage 'SKSE/Plugins/FittingRoom.ini'))) {
    throw 'Development package contains a release data or INI path.'
}

$manifest = @"
Fitting Room - Body Studio Dev
Channel/build: body-studio-dev/$BuildId
Source commit: $sourceCommit
Source branch: $sourceBranch

Compatibility status: 3BA, UBE, and HIMBO ownership field validation is required.
The production Body Studio gate remains closed until the proof matrix passes.

Install this as a separate mod in MO2/Vortex. Its mutable data, INI, logs,
custom-preset root, XML export name, and SKSE serialization identity are
isolated from release. All builds intentionally provide the same virtual
SKSE/Plugins/FittingRoom.dll: enable/order exactly one winning DLL for a game
launch; never rename it or load two Fitting Room DLLs in one process.
"@
Set-Content -LiteralPath (Join-Path $stage 'BODY-STUDIO-DEV.txt') -Value $manifest -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs/testing/body-studio-ownership-proof.md') -Destination $stage

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Packaged: $zip"
Write-Host "Source: $sourceCommit"
Write-Host "Compatibility: 3BA/UBE/HIMBO proof field validation required; production gate closed."
