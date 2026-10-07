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
cp "$BUILD/src/app/montage.exe" "$BUILD/src/montage-cli.exe" "$OUT/"

WINDEPLOYQT=$(command -v windeployqt6 || command -v windeployqt || ls /ucrt64/share/qt6/bin/windeployqt*.exe | head -1)
"$WINDEPLOYQT" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$OUT/montage.exe"

# windeployqt only handles Qt. Copy every other DLL the programs and the Qt
# plugins load from MSYS2 (FFmpeg and its codecs, the GCC runtime...).
# ldd lists dependencies recursively; repeat until nothing new appears.
for _ in 1 2 3 4; do
  before=$(find "$OUT" -name '*.dll' | wc -l)
  find "$OUT" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 ldd 2>/dev/null |
    awk '$3 ~ /^\/ucrt64\/bin\// {print $3}' | sort -u | while read -r dll; do
      [ -e "$OUT/$(basename "$dll")" ] || cp "$dll" "$OUT/"
    done
  [ "$(find "$OUT" -name '*.dll' | wc -l)" -eq "$before" ] && break
done

cp README.md "$OUT/README.md"

# Smoke test: the bundled tool runs with only the bundled DLLs on PATH.
PATH="$(pwd)/$OUT:/c/Windows/System32:/c/Windows" "$OUT/montage-cli.exe" presets > /dev/null

(cd dist && zip -qr9 "$ZIP" Montage)
echo "Wrote dist/$ZIP ($(du -h "dist/$ZIP" | cut -f1))"
