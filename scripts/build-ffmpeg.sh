#!/usr/bin/env bash
# Builds the FFmpeg that Montage's macOS and Windows packages bundle, from FFmpeg's release tarball with a fixed
# configuration, instead of a package manager's build (which carries whatever codecs and libraries it chose).
#
#   scripts/build-ffmpeg.sh <prefix> <stage>
#
# Configures FFmpeg for <prefix> and installs it under <stage><prefix> (make install DESTDIR=<stage>), so CI can cache
# the stage and copy it into place: on macOS a prefix of its own, on Windows (MSYS2) /ucrt64 itself, in place of
# MSYS2's FFmpeg, so Qt Multimedia's FFmpeg plugin uses this build too. A stage that already holds this build is left
# as it is. Also installs, under <prefix>/share/montage-ffmpeg, FFmpeg's licence files, its configure line and the
# source tarball, which packaging copies into the licences folder and the release's sources.
#
# OpenAPV (BSD-3-Clause), APV's encoder, is built first from its release tag and linked into libavcodec statically
# (--enable-liboapv); its licence goes in that share folder too, and its command-line encoder and decoder (for the
# APV tests, not packaged) under <stage>/oapv-apps.
#
# Needs the external libraries below (Homebrew: x264 x265 libvpx svt-av1 dav1d opus lame openjpeg srt librist nasm;
# MSYS2 UCRT64: x264 x265 libvpx svt-av1 dav1d opus lame openjpeg2 srt librist nasm amf-headers ffnvcodec-headers libvpl),
# and git, CMake and Ninja for OpenAPV.
#
# The build is GPL-2.0-or-later (--enable-gpl, for x264 and x265) and never --enable-nonfree. Encoders Montage does
# not offer for Dolby and DTS formats are left out.
set -euo pipefail

VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
OAPV_TAG=v0.2.1.3
OAPV_COMMIT=390b5605c55ea5723802062a00876d67134f71d9
PREFIX=${1:?prefix}
STAGE=${2:?stage}

FLAGS=(
  --enable-shared --disable-static --disable-doc --disable-debug --disable-ffplay
  --enable-gpl
  --enable-libx264 --enable-libx265 --enable-libvpx --enable-libsvtav1 --enable-libdav1d --enable-libopus
  --enable-libmp3lame --enable-libopenjpeg --enable-libsrt --enable-librist --enable-liboapv
  --disable-encoder=ac3,ac3_fixed,ac3_mf,eac3,dca,truehd,mlp
)
case "$(uname -s)" in
  MINGW* | MSYS*) FLAGS+=(--enable-amf --enable-nvenc --enable-libvpl) ;;
  Darwin)
    FLAGS+=(--enable-videotoolbox --enable-audiotoolbox)
    # LAME has no pkg-config file, and Homebrew's prefix is not on the compiler's own paths.
    lame=$(brew --prefix lame)
    FLAGS+=(--extra-cflags=-I$lame/include --extra-ldflags=-L$lame/lib)
    ;;
esac

SHARE="$PREFIX/share/montage-ffmpeg"
CONFIGURE="./configure --prefix=$PREFIX ${FLAGS[*]}"
BUILT="FFmpeg $VERSION: $CONFIGURE
OpenAPV $OAPV_TAG ($OAPV_COMMIT), linked statically"
if [ "$(cat "$STAGE$SHARE/configure.txt" 2>/dev/null || true)" = "$BUILT" ]; then
  echo "FFmpeg $VERSION is already staged in $STAGE"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tarball="ffmpeg-$VERSION.tar.xz"
curl -fsSL --retry 3 --max-time 600 -o "$tmp/$tarball" "https://ffmpeg.org/releases/$tarball"
if command -v sha256sum > /dev/null; then got=$(sha256sum "$tmp/$tarball" | cut -d' ' -f1); else got=$(shasum -a 256 "$tmp/$tarball" | cut -d' ' -f1); fi
[ "$got" = "$SHA256" ] || { echo "$tarball: checksum $got, expected $SHA256" >&2; exit 1; }
tar -xJf "$tmp/$tarball" -C "$tmp"

jobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)
# OpenAPV: a static, position-independent library (inside the shared libavcodec), and its apps.
git -c advice.detachedHead=false clone -q --depth 1 --branch "$OAPV_TAG" https://github.com/AcademySoftwareFoundation/openapv.git "$tmp/openapv"
got=$(git -C "$tmp/openapv" rev-parse HEAD)
[ "$got" = "$OAPV_COMMIT" ] || { echo "OpenAPV $OAPV_TAG is commit $got, expected $OAPV_COMMIT" >&2; exit 1; }
cmake -S "$tmp/openapv" -B "$tmp/openapv/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$tmp/oapv" \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DOAPV_BUILD_SHARED_LIB=OFF -DOAPV_BUILD_STATIC_LIB=ON \
  -DOAPV_BUILD_APPS=ON -DOAPV_APP_STATIC_BUILD=ON -DENABLE_TESTS=OFF > "$tmp/openapv.log" 2>&1 &&
  cmake --build "$tmp/openapv/build" -j"$jobs" >> "$tmp/openapv.log" 2>&1 &&
  cmake --install "$tmp/openapv/build" >> "$tmp/openapv.log" 2>&1 || { tail -n 40 "$tmp/openapv.log" >&2; exit 1; }
# Its pkg-config file names the shared library's folder and the headers' own; FFmpeg includes <oapv/oapv.h> and links
# the static library, which needs the maths library and POSIX threads (winpthreads under MinGW).
OAPV=(--extra-cflags="-I$tmp/oapv/include -DOAPV_STATIC_DEFINE" --extra-ldflags="-L$tmp/oapv/lib/oapv" --extra-libs="-lm -lpthread")
export PKG_CONFIG_PATH="$tmp/oapv/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
(cd "$tmp/ffmpeg-$VERSION" && ./configure --prefix="$PREFIX" "${FLAGS[@]}" "${OAPV[@]}" > "$tmp/configure.log" 2>&1) ||
  { tail -n 40 "$tmp/configure.log" >&2; tail -n 60 "$tmp/ffmpeg-$VERSION/ffbuild/config.log" >&2 || true; exit 1; }
make -C "$tmp/ffmpeg-$VERSION" -j"$jobs" > "$tmp/make.log" 2>&1 || { tail -n 60 "$tmp/make.log" >&2; exit 1; }
rm -rf "$STAGE"
make -C "$tmp/ffmpeg-$VERSION" install DESTDIR="$STAGE" > /dev/null

mkdir -p "$STAGE$SHARE"
for f in LICENSE.md COPYING.GPLv2 COPYING.GPLv3 COPYING.LGPLv2.1 COPYING.LGPLv3; do cp "$tmp/ffmpeg-$VERSION/$f" "$STAGE$SHARE/"; done
cp "$tmp/openapv/LICENSE" "$STAGE$SHARE/OpenAPV-LICENSE.txt"
mkdir -p "$STAGE/oapv-apps"
for f in "$tmp"/oapv/bin/oapv_app_enc* "$tmp"/oapv/bin/oapv_app_dec*; do cp "$f" "$STAGE/oapv-apps/"; done
cp "$tmp/$tarball" "$STAGE$SHARE/"
# The staged program finds the staged libraries only by these paths (its install names point at <prefix>).
DYLD_LIBRARY_PATH="$STAGE$PREFIX/lib" LD_LIBRARY_PATH="$STAGE$PREFIX/lib" PATH="$STAGE$PREFIX/bin:$PATH" \
  "$STAGE$PREFIX/bin/ffmpeg" -hide_banner -buildconf > "$STAGE$SHARE/buildconf.txt" 2> /dev/null || true
echo "$BUILT" > "$STAGE$SHARE/configure.txt"
echo "FFmpeg $VERSION built and staged in $STAGE$PREFIX"
