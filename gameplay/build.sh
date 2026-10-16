#!/bin/sh
# Builds dsound.dll (32-bit) with MSYS2 mingw32. Usage: ./build.sh [install]
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw32/bin:$PATH
mkdir -p build
FLAGS="-std=c++17 -O2 -Wall -Wno-unused-function -m32 -march=i686 -static -static-libgcc -static-libstdc++"
# Dear ImGui (third_party/imgui, unmodified): compiled once into build/imgui
IMGUI="-Ithird_party/imgui -DIMGUI_USE_BGRA_PACKED_COLOR -DIMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS"
mkdir -p build/imgui
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    [ build/imgui/$f.o -nt third_party/imgui/$f.cpp ] || g++ $FLAGS $IMGUI -c -o build/imgui/$f.o third_party/imgui/$f.cpp
done
g++ $FLAGS $IMGUI -shared -o build/dsound.dll \
    src/dllmain.cpp src/common.cpp src/devices.cpp src/input.cpp src/game.cpp src/hud.cpp src/physics.cpp src/vfx.cpp src/traffic.cpp src/ui.cpp src/controls.cpp src/flow.cpp \
    build/imgui/imgui.o build/imgui/imgui_draw.o build/imgui/imgui_tables.o build/imgui/imgui_widgets.o \
    src/dsound.def -ldxguid -lwinmm -lgdi32 -lpsapi -Wl,--enable-stdcall-fixup -Wl,--kill-at -s
echo "built build/dsound.dll"
if [ "$1" = "install" ]; then
    GAME="${NFSMW_DIR:-/c/Games/NFSMW}"
    # Into whichever layout is installed: scripts/nfsmw_gameplay.asi (ASI loader) or dsound.dll
    # (the mod moves an ini left next to speed.exe into scripts/ itself)
    if [ -f "$GAME/scripts/nfsmw_gameplay.asi" ]; then
        cp build/dsound.dll "$GAME/scripts/nfsmw_gameplay.asi"
        [ -f "$GAME/scripts/nfsmw_gameplay.ini" ] || [ -f "$GAME/nfsmw_gameplay.ini" ] || cp nfsmw_gameplay.ini "$GAME/scripts/"
    else
        cp build/dsound.dll "$GAME/"
        [ -f "$GAME/nfsmw_gameplay.ini" ] || cp nfsmw_gameplay.ini "$GAME/"
    fi
    echo "installed to $GAME"
fi
