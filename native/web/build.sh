#!/bin/sh
# Builds public/ss-web.wasm (see ss_web.cpp): our lossless sound (FLAC), spatial sound and cryptography for
# browsers (and the server) – the apps' own C++ code, no library.
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
$cc $flags -I$root/native/common -I$root/native/core -std=c++17 -fno-exceptions -fno-rtti -c "$here/ss_web.cpp" -o "$tmp/ss_web.o"
# the lossless sound: the apps' own FLAC code
$cc $flags -std=c++17 -fno-exceptions -fno-rtti -c "$root/native/common/lossless.cpp" -o "$tmp/lossless.o"
# spatial sound: the apps' own head model
$cc $flags -std=c++17 -fno-exceptions -fno-rtti -c "$root/native/common/spatial.cpp" -o "$tmp/spatial.o"
# our own cryptography: the apps' code (AES bitsliced on WebAssembly's SIMD – every browser that has
# WebCodecs has that too)
$cc $flags -O3 -msimd128 -std=c++17 -fno-exceptions -fno-rtti -c "$root/native/core/crypto_core.cpp" -o "$tmp/crypto_core.o"
$cc --target=wasm32-wasi -mexec-model=reactor -nostdlib++ -Wl,--gc-sections -Wl,--strip-all -Wl,-z,stack-size=1048576 \
  -o "$out" "$tmp/ss_web.o" "$tmp/lossless.o" "$tmp/spatial.o" "$tmp/crypto_core.o"
echo "$out: $(wc -c <"$out") bytes"
