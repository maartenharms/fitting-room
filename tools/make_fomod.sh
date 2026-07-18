#!/usr/bin/env bash
# Build the Fitting Room FOMOD installer zip (the release artifact).
#
# Fitting Room is one DLL that loads on both SE 1.5.97 and AE 1.6.1130+, so there
# is no SE/AE file choice. The FOMOD is a branded page with ONE choice: the
# starting setup, Lore-friendly (the Seamstone ESP + gold cost + the stone as
# the key) or Free-form (no plugin, no gold). Everything else installs always.
#
# Package layout (zip root): fomod/{info.xml,ModuleConfig.xml}, Images/, core/
# (game files, installed to Data), optional/lore/ (the ESP, installed only for
# the Lore-friendly setup), settings/{lore,freeform}/ (the starting INI, one
# per setup), + LICENSE and a short README.txt at the root (NOT installed).
# Docs single-source-of-truth is GitHub; the download carries only LICENSE
# (GPL) + a README.txt pointer, no CHANGELOG/KNOWN-ISSUES/THIRD-PARTY.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^project(FittingRoom VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DLL="$ROOT/build/release/FittingRoom.dll"
STAGE="$ROOT/release/fomod-stage"
ZIP="$ROOT/release/FittingRoom-$VER.zip"

[ -f "$DLL" ] || { echo "no DLL at $DLL - build first"; exit 1; }

rm -rf "$STAGE" "$ZIP"

# FOMOD metadata + banner (ModuleConfig references Images\fittingroom.png).
mkdir -p "$STAGE/fomod" "$STAGE/Images"
cp "$ROOT/fomod/info.xml" "$ROOT/fomod/ModuleConfig.xml" "$STAGE/fomod/"
cp "$ROOT/fomod/banner.png" "$STAGE/Images/fittingroom.png"

# core: the game files that install to Data (DLL + editor fonts + sample preset +
# the SAM-integration script). dist/ mirrors the mod-folder tree.
mkdir -p "$STAGE/core/SKSE/Plugins"
cp "$DLL" "$STAGE/core/SKSE/Plugins/"
cp -r "$ROOT/dist/SKSE/Plugins/FittingRoom" "$STAGE/core/SKSE/Plugins/FittingRoom"
cp -r "$ROOT/dist/Scripts" "$STAGE/core/Scripts"
cp -r "$ROOT/dist/Interface" "$STAGE/core/Interface"  # FLICK translation (Fitting Room_ENGLISH.txt)

# optional: the Seamstone lore item ESP (Lore-friendly setup only).
mkdir -p "$STAGE/optional/lore"
cp "$ROOT/dist/optional/FittingRoomLore.esp" "$STAGE/optional/lore/"

# Starting-settings flavors, BOTH derived from the ONE documented template so
# the comments never fork. The template ships the lore values; free-form flips
# the two gates and its own header line. Fitting Room writes its INI itself on
# first run when none exists, so these only preseed the choice.
mkdir -p "$STAGE/settings/lore" "$STAGE/settings/freeform"
cp "$ROOT/dist/optional/settings/FittingRoom.ini" "$STAGE/settings/lore/FittingRoom.ini"
sed -e 's/^bUseGold=1/bUseGold=0/' \
    -e 's/^bRequireSeamstone=1/bRequireSeamstone=0/' \
    -e 's/^; Starting settings: LORE-FRIENDLY.*/; Starting settings: FREE-FORM (no gold, no Seamstone gate)./' \
    "$ROOT/dist/optional/settings/FittingRoom.ini" > "$STAGE/settings/freeform/FittingRoom.ini"
# Guard: a renamed key would otherwise silently ship a lore INI labeled free-form.
grep -q '^bUseGold=0' "$STAGE/settings/freeform/FittingRoom.ini" ||
    { echo "free-form flavor: bUseGold substitution failed"; exit 1; }
grep -q '^bRequireSeamstone=0' "$STAGE/settings/freeform/FittingRoom.ini" ||
    { echo "free-form flavor: bRequireSeamstone substitution failed"; exit 1; }
grep -q '^; Starting settings: FREE-FORM' "$STAGE/settings/freeform/FittingRoom.ini" ||
    { echo "free-form flavor: header substitution failed"; exit 1; }
grep -q '^bUseGold=1' "$STAGE/settings/lore/FittingRoom.ini" ||
    { echo "lore flavor: template no longer ships bUseGold=1"; exit 1; }

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
