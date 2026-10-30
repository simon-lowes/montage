#!/usr/bin/env bash
# Starts a package's licences folder with what every package carries, whatever it bundles: Montage's own licence,
# exception and third-party notices, the licences of the headers vendored in third_party/, of the libraries compiled
# into Montage (from the build's _deps), of the C2PA library's Rust crates, and of the FFmpeg build.
#
#   scripts/package-licences.sh <licenses-dir> <build-dir> [c2pa-prefix] [ffmpeg-share-dir]
#
# scripts/package-macos.sh and package-windows.sh then add the bundled shared libraries' licences and write
# BUNDLED.txt, the index Help › Licences… shows.
set -euo pipefail

OUT=${1:?licenses dir}
BUILD=${2:?build dir}
C2PA=${3:-}
FFSHARE=${4:-}

rm -rf "$OUT"
mkdir -p "$OUT/montage/third_party" "$OUT/compiled"
cp LICENSE LICENSE-EXCEPTION THIRD_PARTY_NOTICES.md "$OUT/montage/"
cp -R LICENSES "$OUT/montage/"
for d in third_party/*/; do
  name=$(basename "$d")
  mkdir -p "$OUT/montage/third_party/$name"
  find "$d" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'README.montage' \) -exec cp {} "$OUT/montage/third_party/$name/" \;
done

# Libraries compiled into Montage: every licence file of their sources, by path (ThorVG's include RapidJSON's and
# JerryScript's; OpenEXR's its bundled OpenJPH, libdeflate and zstd; bmx's its libMXF and libMXF++, and the
# cmake-git-version-tracking each of the three compiles in; libdatachannel's its libjuice, usrsctp, libSRTP, Plog and
# nlohmann/json). Lua's tarball has none; its licence is LICENSES/MIT-Lua.txt.
for dep in whisper llama rnnoise thorvg openexr imath libvmx bmx liburiparser bmx_git_version libmxf_git_version libmxfpp_git_version ascfdl jsoncons libdatachannel; do
  src="$BUILD/_deps/$dep-src"
  [ -d "$src" ] || continue
  (cd "$src" && find . -type f \( -iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'LICENCE*' -o -iname 'COPYING' -o -iname 'COPYING.*' \) \
      -not -path './.git/*' -not -path '*/test*/*' -not -path '*/examples/*' -not -path './bindings/*') |
    while read -r f; do
      mkdir -p "$OUT/compiled/$dep/$(dirname "$f")"
      cp "$src/$f" "$OUT/compiled/$dep/$f"
    done
done

# The ASC's FDL library's NOTICE (Apache-2.0 asks for it to go with the licence).
if [ -f "$BUILD/_deps/ascfdl-src/NOTICE" ]; then
  mkdir -p "$OUT/compiled/ascfdl"
  cp "$BUILD/_deps/ascfdl-src/NOTICE" "$OUT/compiled/ascfdl/"
fi

# x265, built by CMakeLists.txt with its multiview encoder for spatial video.
if [ -f "$BUILD/montage_x265_build-prefix/src/montage_x265_build/COPYING" ]; then
  mkdir -p "$OUT/compiled/x265"
  cp "$BUILD/montage_x265_build-prefix/src/montage_x265_build/COPYING" "$OUT/compiled/x265/"
fi

if [ -n "$C2PA" ] && [ -d "$C2PA/licenses" ]; then cp -R "$C2PA/licenses" "$OUT/c2pa"; fi

if [ -n "$FFSHARE" ] && [ -d "$FFSHARE" ]; then
  mkdir -p "$OUT/ffmpeg"
  cp "$FFSHARE"/LICENSE.md "$FFSHARE"/COPYING.* "$FFSHARE"/configure.txt "$OUT/ffmpeg/"
  [ -s "$FFSHARE/buildconf.txt" ] && cp "$FFSHARE/buildconf.txt" "$OUT/ffmpeg/"
  # OpenAPV (BSD-3-Clause), linked into FFmpeg's libavcodec.
  [ -s "$FFSHARE/OpenAPV-LICENSE.txt" ] && cp "$FFSHARE/OpenAPV-LICENSE.txt" "$OUT/ffmpeg/"
fi
echo "Licences for Montage, its vendored headers and compiled-in libraries are in $OUT"
