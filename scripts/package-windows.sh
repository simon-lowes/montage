#!/usr/bin/env bash
# Turns an MSYS2 (UCRT64) CMake build of Montage into a portable folder and zip.
#
#   scripts/package-windows.sh [build-dir]   -> dist/Montage/ and dist/Montage-<version>-windows-x64.zip
#
# Run from an MSYS2 UCRT64 shell. packaging/windows/montage.iss turns
# dist/Montage into an installer.
set -euo pipefail

BUILD=${1:-build}
VERSION=$(sed -n 's/^project(Montage VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
OUT=dist/Montage
ZIP="Montage-$VERSION-windows-x64.zip"

rm -rf "$OUT" "dist/$ZIP"
mkdir -p "$OUT"
cp "$BUILD/src/app/montage.exe" "$BUILD/src/app/montage-plugin-probe.exe" "$BUILD/src/montage-cli.exe" "$OUT/"
# ONNX Runtime goes beside the programs explicitly: Windows has an older
# onnxruntime.dll of its own in System32, which ldd (and the loader) would
# otherwise pick. Its own dependencies are collected below.
for dll in "$BUILD"/src/app/onnxruntime.dll "$BUILD"/src/app/libonnxruntime.dll; do
  if [ -e "$dll" ]; then cp "$dll" "$OUT/"; fi
done

WINDEPLOYQT=$(command -v windeployqt6 || command -v windeployqt || ls /ucrt64/share/qt6/bin/windeployqt*.exe | head -1)
"$WINDEPLOYQT" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$OUT/montage.exe"
# Qt's own translations, for its dialogs and buttons in the language chosen in Window › Language.
mkdir -p "$OUT/translations"
cp /ucrt64/share/qt6/translations/qtbase_*.qm "$OUT/translations/" 2>/dev/null || echo "Qt's translations not found; its dialogs stay in English" >&2

# montage-cli runs on the headless "offscreen" platform plugin; windeployqt only adds qwindows.
OFFSCREEN=$(find /ucrt64 -path '*/plugins/platforms/qoffscreen.dll' 2>/dev/null | head -1 || true)
[ -n "$OFFSCREEN" ] || { echo "qoffscreen.dll not found" >&2; exit 1; }
mkdir -p "$OUT/platforms"
cp "$OFFSCREEN" "$OUT/platforms/"

# windeployqt only handles Qt. Copy every other DLL the programs and the Qt
# plugins load from MSYS2 (FFmpeg and its codecs, the GCC runtime...).
# ldd lists dependencies recursively; repeat until nothing new appears.
# (One file per ldd call: ldd fails on the odd DLL it cannot read, which must
# neither stop the script nor hide the dependencies of the other files.)
for _ in 1 2 3 4; do
  before=$(find "$OUT" -name '*.dll' | wc -l)
  deps=$(find "$OUT" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n1 ldd 2>/dev/null |
    awk '$3 ~ /^\/ucrt64\/bin\// {print $3}' | sort -u || true)
  for dll in $deps; do
    [ -e "$OUT/$(basename "$dll")" ] || cp "$dll" "$OUT/"
  done
  if [ "$(find "$OUT" -name '*.dll' | wc -l)" -eq "$before" ]; then break; fi
done

# An FFmpeg DLL that nothing provides means a Qt plugin was built against another FFmpeg than the bundled one
# (the workflow checks MSYS2's is the same release), and the plugin would not load.
missing=$(find "$OUT" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n1 ldd 2>/dev/null |
  awk '/not found/ {print $1}' | sort -u || true)
if echo "$missing" | grep -Eqi '^(av|sw|postproc)'; then
  echo "Bundled files need FFmpeg libraries that are not there: $missing" >&2
  exit 1
fi
[ -z "$missing" ] || echo "Note: libraries not found (not FFmpeg's): $missing"

cp README.md LICENSE "$OUT/"

# The licences folder (Help › Licences…): Montage's and its compiled-in parts' (package-licences.sh), then each
# bundled DLL's, by the MSYS2 package it came from, in BUNDLED.txt with its version, licence and source. With
# MONTAGE_SOURCES_DIR set (a release), each package's MSYS2 source archive and the FFmpeg tarball are put there too.
LIC="$OUT/licenses"
FFSHARE=/ucrt64/share/montage-ffmpeg
[ -d "$FFSHARE" ] || FFSHARE=""
C2PA=$(sed -n 's/^C2PA_ROOT:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -1)
[ -z "$C2PA" ] || C2PA=$(cygpath -u "$C2PA")
scripts/package-licences.sh "$LIC" "$BUILD" "$C2PA" "$FFSHARE"
SOURCES=${MONTAGE_SOURCES_DIR:-}
[ -z "$SOURCES" ] || mkdir -p "$SOURCES"
owners="$(mktemp)"
(cd "$OUT" && find . -name '*.dll' | sed 's|^\./||') | while read -r rel; do
  name=$(basename "$rel")
  case "$rel" in
    */*) src="/ucrt64/share/qt6/plugins/$rel" ;;
    *) src="/ucrt64/bin/$name" ;;
  esac
  pkg=$(pacman -Qqo "$src" 2> /dev/null || true)
  if [ -z "$pkg" ] && [ -n "$FFSHARE" ] && [[ "$name" =~ ^(av|sw|postproc) ]]; then pkg=ffmpeg; fi
  echo "${pkg:-unknown} $name"
done | sort > "$owners"
index="$LIC/BUNDLED.txt"
{
  echo "Libraries bundled in Montage $VERSION for Windows (x64), with their licences (the folder of the same name"
  echo "here) and where their source comes from. Montage itself: GPL-3.0-or-later, see montage/LICENSE."
  echo
} > "$index"
for pkg in $(cut -d' ' -f1 "$owners" | sort -u); do
  files=$(awk -v p="$pkg" '$1 == p { printf "%s ", $2 }' "$owners")
  if [ "$pkg" = ffmpeg ]; then
    ffversion=$(sed -n 's/^FFmpeg \([^:]*\):.*/\1/p' "$FFSHARE/configure.txt")
    {
      echo "ffmpeg $ffversion: GPL-2.0-or-later (this build; its configure line is ffmpeg/configure.txt)"
      echo "  source: https://ffmpeg.org/releases/ffmpeg-$ffversion.tar.xz, built by scripts/build-ffmpeg.sh"
      echo "  files: $files"
    } >> "$index"
    [ -z "$SOURCES" ] || cp "$FFSHARE"/ffmpeg-*.tar.xz "$SOURCES/"
    continue
  fi
  if [ "$pkg" = unknown ]; then
    echo "not from an MSYS2 package (built with Montage): $files" >> "$index"
    continue
  fi
  version=$(pacman -Q "$pkg" | cut -d' ' -f2)
  base=$(awk '/^%BASE%$/ { getline; print; exit }' "/var/lib/pacman/local/$pkg-$version/desc")
  real=${pkg#mingw-w64-ucrt-x86_64-}
  mkdir -p "$LIC/$real"
  [ -d "/ucrt64/share/licenses/$real" ] && cp -R "/ucrt64/share/licenses/$real/." "$LIC/$real/"
  url="https://repo.msys2.org/mingw/sources/$base-$version.src.tar.zst"
  {
    echo "$real $version: $(pacman -Qi "$pkg" | sed -n 's/^Licenses *: //p')$([ -n "$(ls "$LIC/$real")" ] || echo ' (no licence file in the package)')"
    echo "  source: $url (MSYS2 package $pkg)"
    echo "  files: $files"
  } >> "$index"
  if [ -n "$SOURCES" ] && [ ! -e "$SOURCES/$(basename "$url")" ]; then
    curl -fsSL --retry 3 --max-time 900 -o "$SOURCES/$(basename "$url")" "$url"
  fi
done
rm -f "$owners"

# Smoke test: the bundled tool runs with only the bundled DLLs on PATH.
PATH="$(pwd)/$OUT:/c/Windows/System32:/c/Windows" "$OUT/montage-cli.exe" presets > /dev/null

(cd dist && zip -qr9 "$ZIP" Montage)
echo "Wrote dist/$ZIP ($(du -h "dist/$ZIP" | cut -f1))"
