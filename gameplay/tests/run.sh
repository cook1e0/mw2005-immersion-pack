#!/bin/sh
set -e
cd "$(dirname "$0")/.."
export PATH=/c/msys64/mingw32/bin:$PATH
mkdir -p build
g++ -std=c++17 -O2 -Wall -static -Wl,--image-base,0x400000 -Wl,--disable-dynamicbase \
    -o build/regression_tests.exe tests/regression_tests.cpp -ldxguid -lwinmm
build/regression_tests.exe
g++ -std=c++17 -O2 -Wall -Wno-unused-function -static -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -o build/physics_profiles.exe tests/physics_profiles.cpp -lwinmm
build/physics_profiles.exe
