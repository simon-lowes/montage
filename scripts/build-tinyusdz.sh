#!/usr/bin/env bash
# Builds tinyusdz (Apache-2.0, a USD reader written apart from Montage's own and from Pixar's) and a small reader of
# Montage's own, tests/tools/usd_readback.cc, which the 3D compositing test reads Montage's USD exports back with: the
# cards' transforms at chosen time codes and their pictures' texels, through tinyusdz's scene converter. Used only by
# tests; nothing of it is linked into Montage.
#
#   scripts/build-tinyusdz.sh [prefix]     (default /opt/tinyusdz; Linux)
#
# Installs <prefix>/bin/usd_readback. A prefix that already has this version is left as it is.
set -euo pipefail

COMMIT=c9f177148f7dcddde33f4f8d6e82599f151d8803
PREFIX=${1:-/opt/tinyusdz}
here=$(cd "$(dirname "$0")/.." && pwd)
stamp="$COMMIT $(sha256sum "$here/tests/tools/usd_readback.cc" | cut -c1-16)"

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$stamp" ] && [ -x "$PREFIX/bin/usd_readback" ]; then
  echo "tinyusdz $COMMIT is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src="$tmp/tinyusdz"
git init -q "$src"
git -C "$src" remote add origin https://github.com/lighttransport/tinyusdz.git
git -C "$src" -c advice.detachedHead=false fetch -q --depth 1 origin "$COMMIT"
git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
got=$(git -C "$src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "tinyusdz is commit $got, expected $COMMIT" >&2; exit 1; }

# Only the library, with its readers, composition and scene converter (USDVOL: the converter calls into it).
opts=(-DCMAKE_BUILD_TYPE=Release -DLIGHTUSD_USE_CCACHE=OFF -DLIGHTUSD_BUILD_SHARED_LIBS=OFF -DLIGHTUSD_BUILD_TESTS=OFF
      -DLIGHTUSD_BUILD_EXAMPLES=OFF -DLIGHTUSD_BUILD_TOOLS=OFF -DLIGHTUSD_BUILD_BENCHMARKS=OFF
      -DLIGHTUSD_WITH_USDMTLX=OFF -DLIGHTUSD_WITH_JSON=OFF -DLIGHTUSD_WITH_AUDIO=OFF -DLIGHTUSD_WITH_EXR=OFF
      -DLIGHTUSD_WITH_TIFF=OFF -DLIGHTUSD_WITH_USD_TO_GLTF=OFF -DLIGHTUSD_WITH_USDOBJ=OFF -DLIGHTUSD_WITH_USDFBX=OFF
      -DLIGHTUSD_WITH_USDVOX=OFF -DLIGHTUSD_WITH_MCP_SERVER=OFF -DLIGHTUSD_WITH_QJS=OFF -DLIGHTUSD_WITH_TEXTOOLS=OFF
      -DLIGHTUSD_WITH_USDVOL=ON)
cmake -S "$src" -B "$tmp/build" "${opts[@]}" > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc)" --target lightusd_static >> "$tmp/build.log" 2>&1 \
  || { tail -200 "$tmp/build.log" >&2; exit 1; }
lib=$(find "$tmp/build" -name 'liblightusd_static.a' | head -1)
[ -n "$lib" ] || { echo "tinyusdz's static library was not built" >&2; exit 1; }
mkdir -p "$PREFIX/bin"
"${CXX:-c++}" -std=c++17 -O1 -I"$src/src" -I"$src/src/external" "$here/tests/tools/usd_readback.cc" "$lib" \
  -lpthread -ldl -o "$PREFIX/bin/usd_readback"
echo "$stamp" > "$PREFIX/VERSION"
echo "Built tinyusdz $COMMIT and usd_readback in $PREFIX"
