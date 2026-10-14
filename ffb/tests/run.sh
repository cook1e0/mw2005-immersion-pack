#!/bin/sh
set -e
cd "$(dirname "$0")/.."
export PATH=/c/msys64/mingw32/bin:$PATH
mkdir -p build
g++ -std=c++17 -O2 -Wall -static -o build/menu_input_tests.exe tests/menu_input_tests.cpp
g++ -std=c++17 -O2 -Wall -Wno-delete-non-virtual-dtor -Wno-stringop-truncation -static \
    -Wl,--image-base,0x400000 -Wl,--disable-dynamicbase -o build/device_input_tests.exe tests/device_input_tests.cpp -ldxguid
build/menu_input_tests.exe
build/device_input_tests.exe
g++ -std=c++17 -O2 -Wall -Wno-delete-non-virtual-dtor -Wno-stringop-truncation -static \
    -o build/ffb_tests.exe tests/ffb_tests.cpp -ldxguid -lwinmm
build/ffb_tests.exe
