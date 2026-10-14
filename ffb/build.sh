#!/bin/sh
# Builds dinput8.dll (32-bit) with MSYS2 mingw32. Usage: ./build.sh [install]
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw32/bin:$PATH
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wno-unused-function -Wno-delete-non-virtual-dtor -Wno-stringop-truncation -Wno-sign-compare -m32 -march=i686 -mfpmath=387 \
    -shared -static -static-libgcc -static-libstdc++ \
    -o build/dinput8.dll \
    src/dllmain.cpp src/common.cpp src/dinput_wrap.cpp src/ffb_engine.cpp src/ffb_model.cpp src/ffb_device.cpp src/telemetry.cpp \
    src/dinput8.def -ldxguid -lwinmm -Wl,--enable-stdcall-fixup -Wl,--kill-at -s
g++ -std=c++17 -O2 -Wall -m32 -static -static-libgcc -static-libstdc++ \
    -o build/ffbtool.exe tools/ffbtool.cpp -ldinput8 -ldxguid -s
echo "built build/dinput8.dll build/ffbtool.exe"
if [ "$1" = "install" ]; then
    GAME="${NFSMW_DIR:-/c/Games/NFSMW}"
    # Into whichever layout is installed: scripts/nfsmw_ffb.asi (ASI loader) or dinput8.dll
    # (the mod moves an ini left next to speed.exe into scripts/ itself)
    if [ -f "$GAME/scripts/nfsmw_ffb.asi" ]; then
        cp build/dinput8.dll "$GAME/scripts/nfsmw_ffb.asi"
        [ -f "$GAME/scripts/nfsmw_ffb.ini" ] || [ -f "$GAME/nfsmw_ffb.ini" ] || cp nfsmw_ffb.ini "$GAME/scripts/"
    else
        cp build/dinput8.dll "$GAME/"
        [ -f "$GAME/nfsmw_ffb.ini" ] || cp nfsmw_ffb.ini "$GAME/"
    fi
    echo "installed to $GAME"
fi
