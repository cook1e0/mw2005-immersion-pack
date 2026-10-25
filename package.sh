#!/bin/sh
# Assembles dist/mw2005-immersion-pack-<version>.zip from the three builds (run vr/build.cmd,
# ffb/build.sh and gameplay/build.sh first).
# Usage: ./package.sh <Ultimate ASI Loader x86 dinput8.dll> <version, e.g. v0.11.0> <widescreen fix folder>
# The widescreen fix folder is NFSMostWanted.WidescreenFix.zip from ThirteenAG/WidescreenFixesPack's
# "nfsmw" release, extracted; its .asi and .tpk ship, with release/NFSMostWanted.WidescreenFix.ini.
# (The VR mod sets the game's physics rate through that fix's SimRate.)
# NFSMW_SETTINGS=<folder with nfsmw_*.ini> packages those settings (F4 menu, bindings, seats,
# physics presets)
# instead of the defaults, e.g. a game's scripts folder, so unpacking keeps that setup.
# NFSMW_NO_SETTINGS=1 leaves every .ini out (the widescreen fix's too): an update that keeps the
# settings already installed.
set -e
cd "$(dirname "$0")"
LOADER="$1"
VERSION="$2"
WSFIX="$3"
USAGE="usage: ./package.sh path/to/Ultimate-ASI-Loader/dinput8.dll vX.Y.Z path/to/NFSMostWanted.WidescreenFix"
[ -f "$LOADER" ] || { echo "$USAGE"; exit 1; }
[ -f "$WSFIX/scripts/NFSMostWanted.WidescreenFix.asi" ] || { echo "$USAGE (the extracted zip, with scripts\ inside)"; exit 1; }
case "$VERSION" in
    v[0-9]*) ;;
    *) echo "version must start with v and a digit (e.g. v0.11.0)"; exit 1 ;;
esac
case "$VERSION" in
    *[!a-zA-Z0-9._-]*) echo "invalid version: use letters, digits, dots, underscores or hyphens"; exit 1 ;;
esac
OUT="dist/mw2005-immersion-pack-$VERSION"
ZIP="$OUT.zip"
rm -rf "$OUT" "$ZIP"
mkdir -p "$OUT/scripts"
cp "$LOADER" "$OUT/dinput8.dll"
cp vr/bin/d3d9.dll "$OUT/scripts/nfsmw_vr.asi"
cp ffb/build/dinput8.dll "$OUT/scripts/nfsmw_ffb.asi"
cp gameplay/build/dsound.dll "$OUT/scripts/nfsmw_gameplay.asi"
cp "$WSFIX/scripts/NFSMostWanted.WidescreenFix.asi" "$WSFIX/scripts/NFSMostWanted.WidescreenFix.tpk" "$OUT/scripts/"
cp vr/bin/nfsmw_vr_host.exe vr/bin/nfsmw_crashdump.exe ffb/build/ffbtool.exe "$OUT/"
if [ -n "$NFSMW_NO_SETTINGS" ]; then
    echo "settings: none (an update: the installed .ini files stay)"
else
    cp vr/nfsmw_vr.ini vr/nfsmw_vr_edits.ini ffb/nfsmw_ffb.ini gameplay/nfsmw_gameplay.ini gameplay/nfsmw_physics_presets.ini "$OUT/scripts/"
    cp release/NFSMostWanted.WidescreenFix.ini "$OUT/scripts/"
fi
if [ -n "$NFSMW_SETTINGS" ] && [ -z "$NFSMW_NO_SETTINGS" ]; then
    for ini in nfsmw_vr.ini nfsmw_vr_edits.ini nfsmw_ffb.ini nfsmw_gameplay.ini nfsmw_physics_presets.ini; do
        if [ -f "$NFSMW_SETTINGS/$ini" ]; then
            cp "$NFSMW_SETTINGS/$ini" "$OUT/scripts/$ini"
            echo "settings: $ini from $NFSMW_SETTINGS"
        fi
    done
fi
cp release/README.txt release/FIXES.md release/LICENSE-asi-loader.txt release/LICENSE-widescreen-fix.txt LICENSE "$OUT/"
cp gameplay/third_party/imgui/LICENSE.txt "$OUT/LICENSE-imgui.txt"
PS=pwsh; command -v pwsh >/dev/null || PS=powershell
$PS -NoProfile -Command "Compress-Archive -Path '$OUT/*' -DestinationPath '$ZIP'"
echo "packaged $ZIP"
