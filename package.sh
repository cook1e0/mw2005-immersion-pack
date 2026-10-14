#!/bin/sh
# Assembles dist/nfsmw05-mod-<version>.zip from the three builds (run vr/build.cmd,
# ffb/build.sh and gameplay/build.sh first).
# Usage: ./package.sh <Ultimate ASI Loader x86 dinput8.dll> <version, e.g. v0.11.0>
# NFSMW_SETTINGS=<folder with nfsmw_*.ini> packages those settings (F4 menu, bindings, seats)
# instead of the defaults, e.g. a game's scripts folder, so unpacking keeps that setup.
set -e
cd "$(dirname "$0")"
LOADER="$1"
VERSION="$2"
[ -f "$LOADER" ] || { echo "usage: ./package.sh path/to/Ultimate-ASI-Loader/dinput8.dll vX.Y.Z"; exit 1; }
case "$VERSION" in
    v[0-9]*) ;;
    *) echo "version must start with v and a digit (e.g. v0.11.0)"; exit 1 ;;
esac
case "$VERSION" in
    *[!a-zA-Z0-9._-]*) echo "invalid version: use letters, digits, dots, underscores or hyphens"; exit 1 ;;
esac
OUT="dist/nfsmw05-mod-$VERSION"
ZIP="$OUT.zip"
rm -rf "$OUT" "$ZIP"
mkdir -p "$OUT/scripts"
cp "$LOADER" "$OUT/dinput8.dll"
cp vr/bin/d3d9.dll "$OUT/scripts/nfsmw_vr.asi"
cp ffb/build/dinput8.dll "$OUT/scripts/nfsmw_ffb.asi"
cp gameplay/build/dsound.dll "$OUT/scripts/nfsmw_gameplay.asi"
cp vr/bin/nfsmw_vr_host.exe vr/bin/nfsmw_crashdump.exe ffb/build/ffbtool.exe "$OUT/"
cp vr/nfsmw_vr.ini ffb/nfsmw_ffb.ini gameplay/nfsmw_gameplay.ini "$OUT/scripts/"
if [ -n "$NFSMW_SETTINGS" ]; then
    for ini in nfsmw_vr.ini nfsmw_ffb.ini nfsmw_gameplay.ini; do
        if [ -f "$NFSMW_SETTINGS/$ini" ]; then
            cp "$NFSMW_SETTINGS/$ini" "$OUT/scripts/$ini"
            echo "settings: $ini from $NFSMW_SETTINGS"
        fi
    done
fi
cp release/README.txt release/FIXES.md release/LICENSE-asi-loader.txt LICENSE "$OUT/"
cp gameplay/third_party/imgui/LICENSE.txt "$OUT/LICENSE-imgui.txt"
PS=pwsh; command -v pwsh >/dev/null || PS=powershell
$PS -NoProfile -Command "Compress-Archive -Path '$OUT/*' -DestinationPath '$ZIP'"
echo "packaged $ZIP"
