#!/usr/bin/env bash
# Runs the HDR viewer under Weston (scripts/build-weston.sh): headless, its GL renderer, one 1280x720 output, the kiosk
# shell. Twice: with colour management on and the output driven as BT.2100 PQ (BT.2020, reference white 203 nits, a
# 1000-nit target), where test_app's hdrViewerOnWayland checks the surface's description in Weston's protocol log,
# its 10-bit buffers, the surface's own pixels and the PQ codes in Weston's screenshot; then with colour management
# off, where the viewer stays SDR and says why. Both runs first check Weston's screenshot of the SDR viewer. A skipped
# test fails the check, and so does a Weston that died or did not stop cleanly.
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
  # --fake-seat: the headless backend has no seat without it, and Weston 16.0.2's kiosk shell forgets a closed window
  # only through its seat's focus: with none, the output keeps pointing at the freed window (desktop_surface_removed
  # in kiosk-shell.c) and Weston crashes when the next window is mapped, as the test's viewer is after the main window
  # is hidden. Desktops always have a seat.
  # The log also has what Weston's renderer draws each surface with (its buffer's format, its colour transformation and
  # the shaders made for it), for when its screenshot is not what the test expects.
  # Weston 16 blends in its fragment shaders where the GL driver can read the framebuffer back in a shader, and under
  # Mesa's llvmpipe that showed every window black on the PQ output, SDR ones too. Without that one extension Weston
  # blends in a half-float buffer of its own, as its earlier releases always did.
  LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    MESA_EXTENSION_OVERRIDE="-GL_EXT_shader_framebuffer_fetch_non_coherent" "$weston_prefix/bin/weston" \
    --backend=headless --fake-seat --renderer=gl --width=1280 --height=720 --shell=kiosk --socket="$socket" \
    --config="$dir/weston.ini" --debug --log="$dir/weston.log" --logger-scopes=log,proto,gl-renderer-paint-nodes,color-lcms-transformations,gl-shader-generator &
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
    kill "$weston" 2> /dev/null || true
    wait "$weston" 2> /dev/null || true
    weston=""
    failed=1
    return
  fi
  echo "::group::$name"
  local status=0
  WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 QTEST_FUNCTION_TIMEOUT=900000 \
    MONTAGE_TEST_WESTON_LOG="$dir/weston.log" MONTAGE_TEST_WESTON_CM="$cm" XDG_PICTURES_DIR="$dir/shots" \
    MONTAGE_TEST_WESTON_SCREENSHOOTER="$work/screenshooter" QT_LOGGING_RULES="qt.rhi.general=true" \
    "$test_app" hdrViewerOnWayland -o "$dir/result.txt,txt" -o -,txt || status=1
  echo "::endgroup::"
  if grep -q '^SKIP' "$dir/result.txt" 2> /dev/null; then
    grep '^SKIP' -A1 "$dir/result.txt"
    status=1
  fi
  # Weston must have lived through the test (a test that passes beside a dead compositor proved nothing), and stop
  # when asked, with nothing wrong to report.
  local ended=0
  sleep 1  # (the test's connection closed and taken in: Weston says itself that it stops badly with windows open)
  if kill -0 "$weston" 2> /dev/null; then
    kill "$weston" 2> /dev/null || true
    wait "$weston" 2> /dev/null || ended=$?
    if [ "$ended" != 0 ]; then
      echo "Weston did not stop cleanly with colour management $cm (status $ended)" >&2
      status=1
    fi
  else
    wait "$weston" 2> /dev/null || ended=$?
    echo "Weston died during the test with colour management $cm (status $ended)" >&2
    status=1
  fi
  weston=""
  if [ "$status" != 0 ]; then
    echo "hdrViewerOnWayland failed with colour management $cm; Weston's log:" >&2
    grep -v ' -> \| client ' "$dir/weston.log" | tail -200 >&2 || true
    grep -E 'color|image_description|create_buffer|create_immed|\.create\(' "$dir/weston.log" | tail -60 >&2 || true
    echo "The last of Weston's protocol log:" >&2
    grep -v 'wl_registry@.*global(\|wl_shm@.*format(' "$dir/weston.log" | tail -40 >&2 || true
    failed=1
  fi
}

run on true
run off false
exit "$failed"
