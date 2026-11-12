#!/usr/bin/env bash
# Builds SVT-JPEG-XS (BSD-2-Clause-Patent), the JPEG XS encoder and decoder Montage reads and writes JPEG XS with, from
# a pinned tag checked against its commit: a static library, its headers, its pkg-config file and its own command-line
# encoder and decoder (SvtJpegxsEncApp, SvtJpegxsDecApp), which the JPEG XS tests compare Montage's frames with, and
# <prefix>/licenses (its licence and that of the cpuinfo it carries), which the packages' licences folder takes.
# Montage finds it with -DSVTJPEGXS_ROOT=<prefix> (or pkg-config); CI and the packages build it with this script.
#
#   scripts/build-svt-jpegxs.sh [prefix]     (default /opt/svt-jpegxs; set TMPDIR to put the sources and build elsewhere)
#
# Needs CMake, Ninja and a C compiler, and NASM (2.14 or later) on x86-64. v0.10.0 is the first release that builds on
# AArch64 and macOS. A prefix that already has this version is left as it is.
set -euo pipefail

TAG=v0.10.0
COMMIT=01bc7cc7123d2e4a3e330632fabec873f3cee62b
PREFIX=${1:-/opt/svt-jpegxs}
STAMP="SVT-JPEG-XS $TAG"

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$STAMP" ] && [ -f "$PREFIX/include/svt-jpegxs/SvtJpegxsEnc.h" ] && [ -f "$PREFIX/licenses/LICENSE.md" ]; then
  echo "$STAMP is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
CMAKE_PREFIX=$PREFIX
command -v cygpath > /dev/null && CMAKE_PREFIX=$(cygpath -m "$PREFIX")  # (MSYS2: the native CMake's own spelling)

git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/OpenVisualCloud/SVT-JPEG-XS.git "$tmp/src"
got=$(git -C "$tmp/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "SVT-JPEG-XS $TAG is commit $got, expected $COMMIT" >&2; exit 1; }

# (Static and position-independent, so it links into Montage's engine library; without link-time optimisation, whose
# objects another compiler version could not read; the sample programs built, as the tests use them.)
cmake -S "$tmp/src" -B "$tmp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$CMAKE_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DJPEGXS_LTO=OFF \
  -DBUILD_APPS=ON -DBUILD_TESTING=OFF > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" --target install >> "$tmp/build.log" 2>&1 \
  || { tail -200 "$tmp/build.log" >&2; exit 1; }
[ -f "$PREFIX/lib/pkgconfig/SvtJpegxs.pc" ] || { echo "SVT-JPEG-XS installed no pkg-config file" >&2; exit 1; }
for app in SvtJpegxsEncApp SvtJpegxsDecApp; do
  [ -e "$PREFIX/bin/$app" ] || [ -e "$PREFIX/bin/$app.exe" ] || { echo "SVT-JPEG-XS installed no $app" >&2; exit 1; }
done
# Its licence, and that of cpuinfo (BSD-2-Clause), which it compiles in on x86-64, for the packages' licences folder.
mkdir -p "$PREFIX/licenses/cpuinfo"
cp "$tmp/src/LICENSE.md" "$PREFIX/licenses/"
cp "$tmp/src/third_party/cpuinfo/LICENSE" "$PREFIX/licenses/cpuinfo/"
echo "$STAMP" > "$PREFIX/VERSION"
echo "Built $STAMP in $PREFIX"
