#!/usr/bin/env bash
# Builds SVT-AV1 (BSD-3-Clause-Clear), the AV1 encoder whose film grain tables the grain tests check Montage's against,
# from a pinned tag checked against its commit: its library, headers and pkg-config file, its own command-line encoder
# (SvtAv1EncApp, whose --fgs-table reads a film grain table as Montage writes it), and <prefix>/licenses (its licence).
# Montage itself writes AV1's film grain parameters into the frame headers whichever encoder made them
# (render/Av1Headers.h), so it builds and runs without this; CI builds it for the tests, which find SvtAv1EncApp
# through MONTAGE_TEST_SVTAV1=<prefix>/bin.
#
#   scripts/build-svt-av1.sh [prefix]     (default /opt/svt-av1; set TMPDIR to put the sources and build elsewhere)
#
# The project's GitHub repository is read: its GitLab mirror stops at v4.3.0.
#
# Needs CMake, Ninja, a C and C++ compiler, and NASM (2.14 or later) on x86-64. A prefix that already has this version
# is left as it is.
set -euo pipefail

TAG=v4.3.1
COMMIT=462771f87694e959cc1d3f9addde23d7015662e3
PREFIX=${1:-/opt/svt-av1}
STAMP="SVT-AV1 $TAG"

app() { [ -e "$PREFIX/bin/SvtAv1EncApp" ] || [ -e "$PREFIX/bin/SvtAv1EncApp.exe" ]; }
if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$STAMP" ] && app && [ -f "$PREFIX/licenses/LICENSE.md" ]; then
  echo "$STAMP is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
CMAKE_PREFIX=$PREFIX
command -v cygpath > /dev/null && CMAKE_PREFIX=$(cygpath -m "$PREFIX")  # (MSYS2: the native CMake's own spelling)

git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/AOMediaCodec/SVT-AV1.git "$tmp/src"
got=$(git -C "$tmp/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "SVT-AV1 $TAG is commit $got, expected $COMMIT" >&2; exit 1; }

# (Static, the encoder alone with its application; no unit tests.)
cmake -S "$tmp/src" -B "$tmp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$CMAKE_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_DEC=OFF -DBUILD_APPS=ON > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" --target install >> "$tmp/build.log" 2>&1 \
  || { tail -200 "$tmp/build.log" >&2; exit 1; }
app || { echo "SVT-AV1 installed no SvtAv1EncApp" >&2; exit 1; }
mkdir -p "$PREFIX/licenses"
cp "$tmp/src/LICENSE.md" "$PREFIX/licenses/"
echo "$STAMP" > "$PREFIX/VERSION"
echo "Built $STAMP in $PREFIX"
