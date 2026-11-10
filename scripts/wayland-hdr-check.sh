#!/usr/bin/env bash
# Runs the HDR viewer under Weston (scripts/build-weston.sh): headless, its GL renderer, one 1280x720 output, the kiosk
# shell. Twice: with colour management on and the output driven as BT.2100 PQ (BT.2020, reference white 203 nits, a
# 1000-nit target), where test_app's hdrViewerOnWayland checks the surface's description in Weston's protocol log,
# its 10-bit buffers and the PQ codes in Weston's screenshot; then with colour management off, where the viewer stays
# SDR and says why. A skipped test fails the check.
#
#   scripts/wayland-hdr-check.sh <weston prefix> <test_app> <work directory>
set -euo pipefail
weston_prefix=$(realpath "$1")
test_app=$(realpath "$2")
work=$(realpath -m "$3")
mkdir -p "$work"
export XDG_RUNTIME_DIR="$work/runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"

# Weston's screenshooter, with Weston's own libraries (the test runs it).
printf '#!/bin/sh\nLD_LIBRARY_PATH="%s/lib" exec "%s/bin/weston-screenshooter" "$@"\n' "$weston_prefix" "$weston_prefix" > "$work/screenshooter"
chmod +x "$work/screenshooter"

weston=""
cleanup() { [ -z "$weston" ] || kill "$weston" 2> /dev/null || true; }
trap cleanup EXIT

failed=0
run() {  # name color-management(true|false)
  local name=$1 cm=$2 dir="$work/$1"
  rm -rf "$dir"
  mkdir -p "$dir/shots"
  if [ "$cm" = true ]; then
    cat > "$dir/weston.ini" << EOF
[core]
color-management=true

[output]
name=headless
eotf-mode=st2084
colorimetry-mode=bt2020rgb
color-profile=montage-hdr

[color-profile]
name=montage-hdr
prim_named=bt2020
tf_named=st2084
min_lum=0.005
max_lum=10000
ref_lum=203
target_min_lum=0.005
target_max_lum=1000
EOF
  else
    printf '[core]\ncolor-management=false\n' > "$dir/weston.ini"
  fi
  local socket="montage-hdr-$name"
  # (Weston's own libraries from its prefix; the test runs with the system's)
  LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$weston_prefix/bin/weston" \
    --backend=headless --renderer=gl --width=1280 --height=720 --shell=kiosk --socket="$socket" --config="$dir/weston.ini" \
    --debug --log="$dir/weston.log" --logger-scopes=log,proto &
  weston=$!
  local i
  for i in $(seq 100); do
    [ -S "$XDG_RUNTIME_DIR/$socket" ] && break
    kill -0 "$weston" 2> /dev/null || break
    sleep 0.1
  done
  if [ ! -S "$XDG_RUNTIME_DIR/$socket" ]; then
    echo "Weston did not start ($name):" >&2
    tail -50 "$dir/weston.log" >&2 || true
    failed=1
    return
  fi
  echo "::group::$name"
  local status=0
  WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 QTEST_FUNCTION_TIMEOUT=900000 \
    MONTAGE_TEST_WESTON_LOG="$dir/weston.log" MONTAGE_TEST_WESTON_CM="$cm" XDG_PICTURES_DIR="$dir/shots" \
    MONTAGE_TEST_WESTON_SCREENSHOOTER="$work/screenshooter" \
    "$test_app" hdrViewerOnWayland -o "$dir/result.txt,txt" -o -,txt || status=1
  echo "::endgroup::"
  if grep -q '^SKIP' "$dir/result.txt" 2> /dev/null; then
    grep '^SKIP' -A1 "$dir/result.txt"
    status=1
  fi
  if [ "$status" != 0 ]; then
    echo "hdrViewerOnWayland failed with colour management $cm; Weston's log:" >&2
    grep -v ' -> \| client ' "$dir/weston.log" | tail -80 >&2 || true
    grep -E 'color|image_description|create_buffer|create_immed|\.create\(' "$dir/weston.log" | tail -60 >&2 || true
    failed=1
  fi
  kill "$weston" 2> /dev/null || true
  wait "$weston" 2> /dev/null || true
  weston=""
}

run on true
run off false
exit "$failed"
