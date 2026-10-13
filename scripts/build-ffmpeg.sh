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
# Needs the external libraries below (Homebrew: x264 x265 libvpx svt-av1 dav1d opus lame openjpeg srt librist nasm;
# MSYS2 UCRT64: x264 x265 libvpx svt-av1 dav1d opus lame openjpeg2 srt librist nasm amf-headers ffnvcodec-headers libvpl).
#
# The build is GPL-2.0-or-later (--enable-gpl, for x264 and x265) and never --enable-nonfree. Encoders Montage does
# not offer for Dolby and DTS formats are left out.
set -euo pipefail

VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
PREFIX=${1:?prefix}
STAGE=${2:?stage}

FLAGS=(
  --enable-shared --disable-static --disable-doc --disable-debug --disable-ffplay
  --enable-gpl
  --enable-libx264 --enable-libx265 --enable-libvpx --enable-libsvtav1 --enable-libdav1d --enable-libopus
  --enable-libmp3lame --enable-libopenjpeg --enable-libsrt --enable-librist
  --disable-encoder=ac3,ac3_fixed,ac3_mf,eac3,dca,truehd,mlp
)
case "$(uname -s)" in
  MINGW* | MSYS*) FLAGS+=(--enable-amf --enable-nvenc --enable-libvpl) ;;
  Darwin) FLAGS+=(--enable-videotoolbox --enable-audiotoolbox) ;;
esac

SHARE="$PREFIX/share/montage-ffmpeg"
CONFIGURE="./configure --prefix=$PREFIX ${FLAGS[*]}"
if [ "$(cat "$STAGE$SHARE/configure.txt" 2>/dev/null || true)" = "FFmpeg $VERSION: $CONFIGURE" ]; then
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
(cd "$tmp/ffmpeg-$VERSION" && ./configure --prefix="$PREFIX" "${FLAGS[@]}" > "$tmp/configure.log" 2>&1) ||
  { tail -n 40 "$tmp/configure.log" >&2; tail -n 60 "$tmp/ffmpeg-$VERSION/ffbuild/config.log" >&2 || true; exit 1; }
make -C "$tmp/ffmpeg-$VERSION" -j"$jobs" > "$tmp/make.log" 2>&1 || { tail -n 60 "$tmp/make.log" >&2; exit 1; }
rm -rf "$STAGE"
make -C "$tmp/ffmpeg-$VERSION" install DESTDIR="$STAGE" > /dev/null

mkdir -p "$STAGE$SHARE"
for f in LICENSE.md COPYING.GPLv2 COPYING.GPLv3 COPYING.LGPLv2.1 COPYING.LGPLv3; do cp "$tmp/ffmpeg-$VERSION/$f" "$STAGE$SHARE/"; done
cp "$tmp/$tarball" "$STAGE$SHARE/"
# The staged program finds the staged libraries only by these paths (its install names point at <prefix>).
DYLD_LIBRARY_PATH="$STAGE$PREFIX/lib" LD_LIBRARY_PATH="$STAGE$PREFIX/lib" PATH="$STAGE$PREFIX/bin:$PATH" \
  "$STAGE$PREFIX/bin/ffmpeg" -hide_banner -buildconf > "$STAGE$SHARE/buildconf.txt" 2> /dev/null || true
echo "FFmpeg $VERSION: $CONFIGURE" > "$STAGE$SHARE/configure.txt"
echo "FFmpeg $VERSION built and staged in $STAGE$PREFIX"
