#!/usr/bin/env bash
# Builds Weston (MIT), the Wayland reference compositor, with its colour management (LittleCMS) and the headless
# backend, and wayland-protocols (MIT) 1.46 for it and for Montage, from pinned tags checked against their commits. A
# test tool only: CI runs the HDR viewer under it (scripts/wayland-hdr-check.sh); nothing of it goes into Montage or
# its packages. Montage's own binding of the colour management protocol is generated from <prefix>/share/wayland-protocols
# (configure with -DMONTAGE_WAYLAND_PROTOCOLS_DIR=<prefix>/share/wayland-protocols).
#
#   scripts/build-weston.sh [prefix]     (default /opt/weston; set TMPDIR to put the sources and build elsewhere)
#
# Needs Meson, Ninja, a C compiler, wayland-scanner and the development files of libwayland, pixman, xkbcommon,
# libinput, libevdev, libudev, libdrm, LittleCMS 2, EGL, GLES 2, GBM, cairo and libpng. A prefix that already has these
# versions is left as it is.
set -euo pipefail

PROTOCOLS_TAG=1.46
PROTOCOLS_COMMIT=6141e1154303dadd5c3e480bc4a16e26f1dcb2af
WESTON_TAG=16.0.2
WESTON_COMMIT=530ae6c769d2fd55ea5157542654810be81a3193
PREFIX=${1:-/opt/weston}
STAMP="wayland-protocols $PROTOCOLS_TAG, Weston $WESTON_TAG"

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$STAMP" ] && [ -x "$PREFIX/bin/weston" ] && [ -x "$PREFIX/bin/weston-screenshooter" ] \
   && [ -f "$PREFIX/share/wayland-protocols/staging/color-management/color-management-v1.xml" ]; then
  echo "$STAMP is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)

fetch() {  # url tag commit dir
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$4"
  local got
  got=$(git -C "$4" rev-parse HEAD)
  [ "$got" = "$3" ] || { echo "$1 $2 is commit $got, expected $3" >&2; exit 1; }
}
build() {  # name src build [meson options]
  local name=$1 src=$2 out=$3
  shift 3
  { meson setup "$out" "$src" --prefix "$PREFIX" --libdir lib --buildtype release "$@" && meson install -C "$out"; } > "$tmp/$name.log" 2>&1 \
    || { tail -200 "$tmp/$name.log" >&2; exit 1; }
}

fetch https://gitlab.freedesktop.org/wayland/wayland-protocols.git "$PROTOCOLS_TAG" "$PROTOCOLS_COMMIT" "$tmp/wayland-protocols"
build wayland-protocols "$tmp/wayland-protocols" "$tmp/build-protocols" -Dtests=false
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Only what the check uses: the headless backend, the GL renderer, colour management, the kiosk shell (which gives
# the one window the whole output) and the screenshooter.
fetch https://gitlab.freedesktop.org/wayland/weston.git "$WESTON_TAG" "$WESTON_COMMIT" "$tmp/weston"
build weston "$tmp/weston" "$tmp/build-weston" \
  -Dbackend-drm=false -Dbackend-headless=true -Dbackend-pipewire=false -Dbackend-rdp=false -Dbackend-vnc=false \
  -Dbackend-wayland=false -Dbackend-x11=false -Dbackend-default=headless -Drenderer-gl=true -Drenderer-vulkan=false \
  -Dxwayland=false -Dsystemd=false -Dshell-desktop=false -Dshell-ivi=false -Dshell-lua=false -Dshell-kiosk=true \
  -Dcolor-management-lcms=true -Dimage-jpeg=false -Dimage-webp=false -Dtests=false -Ddemo-clients=false \
  -Dsimple-clients=[] -Dtools=[]
for f in bin/weston bin/weston-screenshooter share/wayland-protocols/staging/color-management/color-management-v1.xml; do
  [ -e "$PREFIX/$f" ] || { echo "No $f in $PREFIX" >&2; exit 1; }
done
echo "$STAMP" > "$PREFIX/VERSION"
