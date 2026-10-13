#!/bin/sh
# Builds dsound.dll and hshiftertool.exe (32-bit) with MSYS2 mingw32. Usage: ./build.sh [install]
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw32/bin:$PATH
mkdir -p build
FLAGS="-std=c++17 -O2 -Wall -Wno-unused-function -m32 -march=i686 -static -static-libgcc -static-libstdc++"
g++ $FLAGS -shared -o build/dsound.dll \
    src/dllmain.cpp src/common.cpp src/devices.cpp src/input.cpp src/game.cpp src/hud.cpp src/ui.cpp \
    src/dsound.def -ldxguid -lwinmm -lgdi32 -Wl,--enable-stdcall-fixup -Wl,--kill-at -s
g++ $FLAGS -o build/hshiftertool.exe \
    tools/hshiftertool.cpp src/devices.cpp src/common.cpp -ldxguid -s
echo "built build/dsound.dll build/hshiftertool.exe"
if [ "$1" = "install" ]; then
    GAME="${NFSMW_DIR:-/c/Games/NFSMW}"
    cp build/dsound.dll build/hshiftertool.exe "$GAME/"
    [ -f "$GAME/nfsmw_hshifter.ini" ] || cp nfsmw_hshifter.ini "$GAME/"
    echo "installed to $GAME"
fi
