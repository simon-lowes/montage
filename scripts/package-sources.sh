#!/usr/bin/env bash
# Puts a release's sources in one archive beside its packages: Montage's own source at this commit, and what
# scripts/package-macos.sh or package-windows.sh collected in MONTAGE_SOURCES_DIR (the FFmpeg tarball and the source
# of every library the package bundles), so the release carries the Corresponding Source its GPL and LGPL parts ask for.
#
#   MONTAGE_SOURCES_DIR=release-sources scripts/package-sources.sh <platform>   -> dist/Montage-<version>-<platform>-sources.tar
set -euo pipefail

PLATFORM=${1:?platform, e.g. macos-arm64}
SOURCES=${MONTAGE_SOURCES_DIR:?MONTAGE_SOURCES_DIR is not set}
VERSION=$(sed -n 's/^project(Montage VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -d "$SOURCES" ] || { echo "$SOURCES does not exist: package first" >&2; exit 1; }

git archive --format=tar.gz --prefix="montage-$VERSION/" -o "$SOURCES/montage-$VERSION.tar.gz" HEAD
cat > "$SOURCES/README.txt" << EOF
The source code of Montage $VERSION ($PLATFORM package) and of the libraries the package bundles.

montage-$VERSION.tar.gz   Montage itself (GPL-3.0-or-later), with the scripts that build the packages
                          (scripts/build-ffmpeg.sh builds FFmpeg; scripts/package-*.sh bundle the libraries).
ffmpeg-*.tar.xz           FFmpeg, as released; licenses/ffmpeg/configure.txt in the package has its configuration.
everything else           each bundled library's source as its package manager builds it: an MSYS2 source
                          archive (Windows), or a Homebrew formula and its source (macOS).

The package's licenses/BUNDLED.txt lists which library each file comes from.
EOF
mkdir -p dist
out="dist/Montage-$VERSION-$PLATFORM-sources.tar"
tar -cf "$out" -C "$SOURCES" .
echo "Wrote $out ($(du -h "$out" | cut -f1))"
