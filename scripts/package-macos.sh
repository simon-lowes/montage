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
# The plugin scanner's helper, loaded from beside the app binary.
cp "$BUILD/src/app/montage-plugin-probe" "$APP/Contents/MacOS/"
OFFSCREEN=$(find -L "$(brew --prefix)/share/qt" "$(brew --prefix qtbase)" -name libqoffscreen.dylib 2>/dev/null | head -1 || true)
[ -n "$OFFSCREEN" ] || { echo "libqoffscreen.dylib not found" >&2; exit 1; }
mkdir -p "$APP/Contents/PlugIns/platforms"
cp "$OFFSCREEN" "$APP/Contents/PlugIns/platforms/"

# Copies Qt, FFmpeg and every other non-system library into the bundle and
# rewrites the load paths to point there.
# -libpath: Homebrew installs each Qt module in its own prefix, so plugins
# reach frameworks (QtSvg...) through rpaths only the shared lib dir resolves.
"$MACDEPLOYQT" "$APP" -always-overwrite -executable="$APP/Contents/MacOS/montage-cli" \
  -executable="$APP/Contents/MacOS/montage-plugin-probe" \
  -executable="$APP/Contents/PlugIns/platforms/libqoffscreen.dylib" -libpath="$(brew --prefix)/lib"

# Qt's SVG image and icon plugins need QtSvg, which macdeployqt cannot find in
# Homebrew's layout. Montage uses no SVG, so leave them out.
rm -f "$APP/Contents/PlugIns/imageformats/libqsvg.dylib" "$APP/Contents/PlugIns/iconengines/libqsvgicon.dylib"

# The licences folder (Help › Licences…): Montage's and its compiled-in parts' (package-licences.sh), then each
# bundled library's, by the Homebrew formula it came from, in BUNDLED.txt with its version, licence and source.
# With MONTAGE_SOURCES_DIR set (a release), each formula's source and the FFmpeg tarball are put there too.
LIC="$APP/Contents/Resources/licenses"
cache_value() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" | head -1; }
avcodec=$(cache_value pkgcfg_lib_FFMPEG_avcodec)
FFSHARE="$(dirname "$(dirname "$avcodec")")/share/montage-ffmpeg"
[ -d "$FFSHARE" ] || FFSHARE=""
scripts/package-licences.sh "$LIC" "$BUILD" "$(cache_value C2PA_ROOT)" "$FFSHARE"
BREW=$(brew --prefix)
SOURCES=${MONTAGE_SOURCES_DIR:-}
[ -z "$SOURCES" ] || mkdir -p "$SOURCES"
index="$LIC/BUNDLED.txt"
{
  echo "Libraries bundled in Montage $VERSION for macOS ($ARCH), with their licences (the folder of the same name"
  echo "here) and where their source comes from. Montage itself: GPL-3.0-or-later, see montage/LICENSE."
  echo
} > "$index"
if [ -n "$FFSHARE" ]; then
  ffversion=$(sed -n 's/^FFmpeg \([^:]*\):.*/\1/p' "$FFSHARE/configure.txt")
  {
    echo "ffmpeg $ffversion: GPL-2.0-or-later (this build; its configure line is ffmpeg/configure.txt)"
    echo "  source: https://ffmpeg.org/releases/ffmpeg-$ffversion.tar.xz, built by scripts/build-ffmpeg.sh"
    echo "  files: $(cd "$APP/Contents/Frameworks" && ls -d libav* libsw* 2> /dev/null | tr '\n' ' ')"
  } >> "$index"
  [ -z "$SOURCES" ] || cp "$FFSHARE"/ffmpeg-*.tar.xz "$SOURCES/"
fi
formulae=$(
  {
    find "$APP/Contents/Frameworks" -maxdepth 1 \( -name '*.dylib' -o -name '*.framework' \) -exec basename {} \; |
      while read -r n; do find "$BREW/Cellar" -maxdepth 4 -name "$n" -print -quit; done
    find "$APP/Contents/PlugIns" -name '*.dylib' -exec basename {} \; |
      while read -r n; do find "$BREW"/Cellar/qt* -path '*/plugins/*' -name "$n" -print -quit; done
  } | sed "s|^$BREW/Cellar/||" | cut -d/ -f1,2 | sort -u
)
for fv in $formulae; do
  f=${fv%%/*}
  mkdir -p "$LIC/$f"
  find "$BREW/Cellar/$fv" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) \
    -exec cp {} "$LIC/$f/" \;
  info=$(brew info --json=v2 "$f")
  {
    echo "$f ${fv#*/}: $(echo "$info" | jq -r '.formulae[0].license // "see its files"')$([ -n "$(ls "$LIC/$f")" ] || echo ' (no licence file in its keg)')"
    echo "  source: $(echo "$info" | jq -r '.formulae[0].urls.stable.url') (Homebrew formula $f)"
  } >> "$index"
  if [ -n "$SOURCES" ]; then
    brew fetch --build-from-source --retry "$f" > /dev/null
    cp "$(brew --cache --build-from-source "$f")" "$SOURCES/"
    brew cat "$f" > "$SOURCES/$f.rb"
  fi
done

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
# hdiutil can fail with "Resource busy" while macOS's disk image helper is still busy (often on CI runners): retry.
for attempt in 1 2 3 4 5; do
  if hdiutil create -volname "Montage $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"; then break; fi
  if [ "$attempt" = 5 ]; then echo "hdiutil failed 5 times" >&2; exit 1; fi
  echo "hdiutil failed (attempt $attempt); retrying in $((attempt * 5)) s" >&2
  sleep $((attempt * 5))
done
rm -rf "$STAGE"
echo "Wrote $DMG ($(du -h "$DMG" | cut -f1))"
