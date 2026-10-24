#!/usr/bin/env bash
# Builds the Windows helpers with MinGW-w64: ss-audio.exe (5.1/7.1 system audio) and
# ss-capture.exe (screen capture + NVIDIA NVENC for direct mode).
# Linux: apt-get install g++-mingw-w64-x86-64   ·   Windows (MSYS2): pacman -S mingw-w64-x86_64-gcc
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -s -static -static-libgcc -static-libstdc++ \
  -o ss-audio.exe ss-audio.cpp -lole32 -lavrt -lwinmm
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -s -static -static-libgcc -static-libstdc++ \
  -o ss-capture.exe ss-capture.cpp -ld3d11 -ldxgi -lavrt -luser32
echo "built $(pwd)/ss-audio.exe $(pwd)/ss-capture.exe"
