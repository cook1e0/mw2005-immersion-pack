#!/bin/sh
# Assembles dist/nfsmw05-mod.zip from the three builds (run vr/build.cmd, ffb/build.sh and
# gameplay/build.sh first). Usage: ./package.sh <Ultimate ASI Loader x86 dinput8.dll>
set -e
cd "$(dirname "$0")"
LOADER="$1"
[ -f "$LOADER" ] || { echo "usage: ./package.sh path/to/Ultimate-ASI-Loader/dinput8.dll"; exit 1; }
OUT=dist/nfsmw05-mod
rm -rf "$OUT" dist/nfsmw05-mod.zip
mkdir -p "$OUT/scripts"
cp "$LOADER" "$OUT/dinput8.dll"
cp vr/bin/d3d9.dll "$OUT/scripts/nfsmw_vr.asi"
cp ffb/build/dinput8.dll "$OUT/scripts/nfsmw_ffb.asi"
cp gameplay/build/dsound.dll "$OUT/scripts/nfsmw_gameplay.asi"
cp vr/bin/nfsmw_vr_host.exe ffb/build/ffbtool.exe "$OUT/"
cp vr/nfsmw_vr.ini ffb/nfsmw_ffb.ini gameplay/nfsmw_gameplay.ini "$OUT/scripts/"
cp release/README.txt release/LICENSE-asi-loader.txt LICENSE "$OUT/"
cp gameplay/third_party/imgui/LICENSE.txt "$OUT/LICENSE-imgui.txt"
pwsh -NoProfile -Command "Compress-Archive -Path '$OUT/*' -DestinationPath 'dist/nfsmw05-mod.zip'"
echo "packaged dist/nfsmw05-mod.zip"
