#!/usr/bin/env bash
# Turns a CMake build of Montage into a self-contained, drag-to-install DMG.
#
#   scripts/package-macos.sh [build-dir]     -> dist/Montage-<version>-macos-<arch>.dmg
#
# Needs Qt's macdeployqt (Homebrew: brew install qtbase). The app is ad-hoc signed,
# not notarised: on first launch macOS asks the user to allow it in
# System Settings > Privacy & Security ("Open Anyway").
set -euo pipefail

BUILD=${1:-build}
APP="$BUILD/src/app/Montage.app"
VERSION=$(sed -n 's/^project(Montage VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
ARCH=$(uname -m)
DMG="dist/Montage-$VERSION-macos-$ARCH.dmg"

[ -d "$APP" ] || { echo "No app bundle at $APP; build first." >&2; exit 1; }
MACDEPLOYQT=$(command -v macdeployqt || echo "$(brew --prefix qtbase)/bin/macdeployqt")

# The command-line tool ships inside the bundle, next to the app binary, with
# the headless "offscreen" platform plugin it runs on (macdeployqt only adds cocoa).
cp "$BUILD/src/montage-cli" "$APP/Contents/MacOS/"
OFFSCREEN=$(find -L "$(brew --prefix)/share/qt" "$(brew --prefix qtbase)" -name libqoffscreen.dylib 2>/dev/null | head -1 || true)
[ -n "$OFFSCREEN" ] || { echo "libqoffscreen.dylib not found" >&2; exit 1; }
mkdir -p "$APP/Contents/PlugIns/platforms"
cp "$OFFSCREEN" "$APP/Contents/PlugIns/platforms/"

# Copies Qt, FFmpeg and every other non-system library into the bundle and
# rewrites the load paths to point there.
# -libpath: Homebrew installs each Qt module in its own prefix, so plugins
# reach frameworks (QtSvg...) through rpaths only the shared lib dir resolves.
"$MACDEPLOYQT" "$APP" -always-overwrite -executable="$APP/Contents/MacOS/montage-cli" \
  -executable="$APP/Contents/PlugIns/platforms/libqoffscreen.dylib" -libpath="$(brew --prefix)/lib"

# Qt's SVG image and icon plugins need QtSvg, which macdeployqt cannot find in
# Homebrew's layout. Montage uses no SVG, so leave them out.
rm -f "$APP/Contents/PlugIns/imageformats/libqsvg.dylib" "$APP/Contents/PlugIns/iconengines/libqsvgicon.dylib"

# Every library the bundle loads must be inside it: nothing from Homebrew (or
# the app only works on this machine) and no bundle-relative path that is
# missing. (A library's own install name is listed too but is not loaded.)
# (A function: macOS's bash 3.2 cannot parse a case statement inside $(...).)
unresolved_libraries() {
  find "$APP" -type f \( -perm -u+x -o -name '*.dylib' \) | while read -r f; do
    id=$(otool -D "$f" 2>/dev/null | sed -n 2p)
    otool -L "$f" 2>/dev/null | sed 1d | awk '{print $1}' | while read -r dep; do
      [ "$dep" = "$id" ] && continue
      case "$dep" in
        /opt/homebrew/* | /usr/local/*) echo "$f -> $dep (outside the bundle)"; continue ;;
        @executable_path/*) p="$APP/Contents/MacOS/${dep#@executable_path/}" ;;
        @loader_path/*) p="$(dirname "$f")/${dep#@loader_path/}" ;;
        @rpath/*) p="$APP/Contents/Frameworks/${dep#@rpath/}" ;;
        *) continue ;;
      esac
      [ -e "$p" ] || echo "$f -> $dep (missing)"
    done
  done
}
problems=$(unresolved_libraries || true)
if [ -n "$problems" ]; then
  echo "Bundle libraries do not resolve:" >&2
  echo "$problems" | sort -u >&2
  exit 1
fi

# macdeployqt's edits break the linker's signatures, and Apple Silicon refuses
# to run unsigned code, so re-sign everything ad hoc.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Smoke test: the bundled tool loads the bundled FFmpeg.
"$APP/Contents/MacOS/montage-cli" presets > /dev/null

# DMG with the app and an Applications shortcut to drag it onto.
mkdir -p dist
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "Montage $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$STAGE"
echo "Wrote $DMG ($(du -h "$DMG" | cut -f1))"
