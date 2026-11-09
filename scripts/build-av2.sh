#!/usr/bin/env bash
# Builds AV2's reference software, AVM (the AV2 encoder Montage exports with, and its avmdec, which the AV2 tests decode
# Montage's files with), and VideoLAN's dav2d (the AV2 decoder Montage reads with), from pinned tags checked against
# their commits. Montage uses them only when configured with -DMONTAGE_WITH_AV2=ON and -DAV2_ROOT=<prefix>; packages
# leave AV2 out, as both licences are flagged (THIRD_PARTY_NOTICES).
#
#   scripts/build-av2.sh [prefix]     (default /opt/av2; set TMPDIR to put the sources and build trees elsewhere)
#
# Needs CMake, Ninja, Meson, NASM and a C/C++ compiler. Installs <prefix>/lib (static libavm and libdav2d, built as
# position-independent code), <prefix>/include, <prefix>/lib/pkgconfig and <prefix>/bin/{avmenc,avmdec,dav2d}. A prefix
# that already has these versions is left as it is.
set -euo pipefail

AVM_TAG=v1.0.0
AVM_COMMIT=966a7d7cd6fcf60360caf5dc413b2aeeb65e144d
DAV2D_TAG=0.0.1
DAV2D_COMMIT=0430370c7f84de6b81839785c5e5411a9d39dcec
PREFIX=${1:-/opt/av2}
STAMP="avm $AVM_TAG, dav2d $DAV2D_TAG"

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$STAMP" ] && [ -x "$PREFIX/bin/avmdec" ] && [ -x "$PREFIX/bin/avmenc" ]; then
  echo "$STAMP is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)

fetch() {  # url tag commit dir
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$4"
  local got
  got=$(git -C "$4" rev-parse HEAD)
  [ "$got" = "$3" ] || { echo "$1 $2 is commit $got, expected $3" >&2; exit 1; }
}

fetch https://code.videolan.org/videolan/dav2d.git "$DAV2D_TAG" "$DAV2D_COMMIT" "$tmp/dav2d"
meson setup "$tmp/dav2d-build" "$tmp/dav2d" --buildtype=release --default-library=static -Db_staticpic=true \
  -Denable_tests=false -Denable_tools=true --prefix="$PREFIX" --libdir=lib > "$tmp/dav2d.log" 2>&1 \
  && ninja -C "$tmp/dav2d-build" install >> "$tmp/dav2d.log" 2>&1 || { cat "$tmp/dav2d.log" >&2; exit 1; }
rm -rf "$tmp/dav2d" "$tmp/dav2d-build"

# (Without its encoder's learned pruning, which would build TensorFlow Lite into libavm and every program linking it:
# the encoder decides partitions by search alone, and its streams are AV2 as before.)
fetch https://github.com/AOMediaCodec/avm.git "$AVM_TAG" "$AVM_COMMIT" "$tmp/avm"
cmake -S "$tmp/avm" -B "$tmp/avm-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib -DCONFIG_PIC=1 -DENABLE_TESTS=0 -DENABLE_DOCS=0 -DENABLE_TOOLS=0 -DENABLE_EXAMPLES=1 \
  -DENABLE_TESTDATA=0 -DENABLE_NASM=1 -DCONFIG_ML_PART_SPLIT=0 -DCONFIG_DIP_EXT_PRUNING=0 -DCONFIG_TENSORFLOW_LITE=0 \
  > "$tmp/avm.log" 2>&1 \
  && cmake --build "$tmp/avm-build" -j "$(nproc)" --target install >> "$tmp/avm.log" 2>&1 || { cat "$tmp/avm.log" >&2; exit 1; }

echo "$STAMP" > "$PREFIX/VERSION"
echo "Built $STAMP in $PREFIX"
