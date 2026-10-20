#!/usr/bin/env bash
# Builds libavif's avifgainmaputil (BSD-2-Clause; with libpng, zlib and libjpeg, which its build fetches, and the
# system's dav1d), the reference gain-map AVIF reader the HDR stills test tone maps Montage's files with. Used only by
# tests; nothing of it is linked into Montage.
#
#   scripts/build-avifgainmaputil.sh [prefix]     (default /opt/libavif)
#
# Needs dav1d's headers (Debian and Ubuntu: libdav1d-dev). Installs <prefix>/bin/avifgainmaputil. A prefix that
# already has this version is left as it is.
set -euo pipefail

TAG=v1.3.0
COMMIT=1aadfad932c98c069a1204261b1856f81f3bc199
PREFIX=${1:-/opt/libavif}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$TAG" ] && [ -x "$PREFIX/bin/avifgainmaputil" ]; then
  echo "libavif $TAG is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/AOMediaCodec/libavif.git "$tmp/src"
got=$(git -C "$tmp/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "libavif $TAG is commit $got, expected $COMMIT" >&2; exit 1; }

cmake -S "$tmp/src" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DAVIF_BUILD_APPS=ON \
  -DAVIF_CODEC_DAV1D=SYSTEM -DAVIF_JPEG=LOCAL -DAVIF_ZLIBPNG=LOCAL -DAVIF_LIBYUV=OFF -DAVIF_LIBXML2=OFF > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc)" --target avifgainmaputil >> "$tmp/build.log" 2>&1 || { tail -50 "$tmp/build.log" >&2; exit 1; }
mkdir -p "$PREFIX/bin"
cp "$tmp/build/avifgainmaputil" "$PREFIX/bin/avifgainmaputil"
echo "$TAG" > "$PREFIX/VERSION"
echo "Built avifgainmaputil $TAG in $PREFIX"
