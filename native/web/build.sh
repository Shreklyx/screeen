#!/bin/sh
# Builds public/ss-web.wasm (see ss_web.cpp): our lossless sound (FLAC) and spatial sound for browsers –
# the apps' own C++ code, no library.
#
#   native/web/build.sh [out.wasm]
#
# Needs clang with wasm-ld, wasi-libc and clang's wasm32 runtime
# (Ubuntu: apt install clang lld wasi-libc libclang-rt-18-dev-wasm32).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/public/ss-web.wasm}
cc=${CC:-clang}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

flags="--target=wasm32-wasi -O2 -DNDEBUG -w"
$cc $flags -I$root/native/common -std=c++17 -fno-exceptions -fno-rtti -c "$here/ss_web.cpp" -o "$tmp/ss_web.o"
# the lossless sound: the apps' own FLAC code
$cc $flags -std=c++17 -fno-exceptions -fno-rtti -c "$root/native/common/lossless.cpp" -o "$tmp/lossless.o"
# spatial sound: the apps' own head model
$cc $flags -std=c++17 -fno-exceptions -fno-rtti -c "$root/native/common/spatial.cpp" -o "$tmp/spatial.o"
$cc --target=wasm32-wasi -mexec-model=reactor -nostdlib++ -Wl,--gc-sections -Wl,--strip-all -Wl,-z,stack-size=1048576 \
  -o "$out" "$tmp/ss_web.o" "$tmp/lossless.o" "$tmp/spatial.o"
echo "$out: $(wc -c <"$out") bytes"
