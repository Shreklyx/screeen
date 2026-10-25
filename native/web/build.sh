#!/bin/sh
# Builds public/ss-web.wasm (see ss_web.cpp): BearSSL's X25519 and libopus' decoder for browsers.
#
#   native/web/build.sh [out.wasm]          SS_WEB_TEST=1: with an Opus encoder too (for the tests)
#
# Needs clang with wasm-ld, wasi-libc and clang's wasm32 runtime
# (Ubuntu: apt install clang lld wasi-libc libclang-rt-18-dev-wasm32). libopus 1.5.2 is downloaded
# (the version and checksum the apps use) unless OPUS_DIR points at its sources.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/public/ss-web.wasm}
cc=${CC:-clang}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

opus=${OPUS_DIR:-}
if [ -z "$opus" ]; then
  curl -fsSL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz -o "$tmp/opus.tar.gz"
  echo "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1  $tmp/opus.tar.gz" | sha256sum -c - >/dev/null
  tar -xzf "$tmp/opus.tar.gz" -C "$tmp"
  opus=$tmp/opus-1.5.2
fi

# the files a make variable lists (opus_sources.mk, celt_sources.mk, silk_sources.mk)
list() { sed -n "/^$2 *=/,/^\$/p" "$opus/$1" | tr -d '\\' | sed "s/^$2 *=//" | tr -s ' \t\n' '\n' | grep '\.c$' | sed "s|^|$opus/|"; }
srcs="$(list opus_sources.mk OPUS_SOURCES) $(list opus_sources.mk OPUS_SOURCES_FLOAT) $(list celt_sources.mk CELT_SOURCES) $(list silk_sources.mk SILK_SOURCES) $(list silk_sources.mk SILK_SOURCES_FLOAT)"
bearssl=$root/native/third_party/bearssl
srcs="$srcs $bearssl/src/ec/ec_c25519_m31.c $bearssl/src/codec/ccopy.c"

flags="--target=wasm32-wasi -O2 -DNDEBUG -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINT -DHAVE_LRINTF -I$opus/include -I$opus/celt -I$opus/silk -I$opus/silk/float -I$bearssl/inc -I$bearssl/src -w"
[ "${SS_WEB_TEST:-}" = 1 ] && flags="$flags -DSS_WEB_TEST"
objs=""
n=0
for s in $srcs; do
  n=$((n + 1))
  $cc $flags -c "$s" -o "$tmp/$n.o"
  objs="$objs $tmp/$n.o"
done
$cc $flags -std=c++17 -fno-exceptions -fno-rtti -c "$here/ss_web.cpp" -o "$tmp/ss_web.o"
$cc --target=wasm32-wasi -mexec-model=reactor -nostdlib++ -Wl,--gc-sections -Wl,--strip-all -Wl,-z,stack-size=1048576 \
  -o "$out" "$tmp/ss_web.o" $objs
echo "$out: $(wc -c <"$out") bytes"
