#!/usr/bin/env bash
# Runs the HDR viewer under Weston (scripts/build-weston.sh): headless, its GL renderer, one 1280x720 output, the kiosk
# shell. Three times: with colour management on and the output driven as BT.2100 PQ (BT.2020, reference white 203
# nits, a 1000-nit target), where test_app's hdrViewerOnWayland checks the surface's description in Weston's protocol
# log, its 10-bit buffers, the surface's own pixels and the PQ codes in Weston's screenshot, and liveHdrOnWayland the
# PQ codes of a live BT.2100 PQ ST 2110 feed on the Source monitor in Weston's screenshot; with colour management on
# and an SDR output, where a viewer that reads what the compositor prefers stays SDR and says the display is not HDR
# (one described by Qt is shown, fitted by Weston); then with colour management off, where the viewer stays SDR and
# says why. Every run first checks Weston's screenshot of the SDR viewer. A skipped test fails the check, and so does a
# Weston that died or did not stop cleanly. Then a fourth time under a Weston with two outputs, one SDR and one PQ (its
# Wayland backend, each output a window of an outer headless Weston; both with the desktop shell), where test_app's
# hdrViewerOnTwoOutputs moves the viewer from one output to the other and reads its enter events in Weston's protocol
# log. Last, with Qt's reconnection on, the PQ output's Weston is killed under hdrViewerWhenWestonGoes and a second
# Weston (an SDR output) takes Qt's connection: Montage must stay up, the viewer SDR with the reason, shown by the second.
# The same checks serve a build with Montage's binding of the protocol and one where Qt describes the surface
# (-DMONTAGE_WAYLAND_BINDING=OFF, Qt 6.10.2 or later).
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
outer=""
cleanup() {
  [ -z "$weston" ] || kill "$weston" 2> /dev/null || true
  [ -z "$outer" ] || kill "$outer" 2> /dev/null || true
}
# The "on" run's output: BT.2100 PQ (BT.2020, reference white 203 nits, a 1000-nit target).
pq_ini() {  # file
  printf '[core]\ncolor-management=true\n\n[output]\nname=headless\neotf-mode=st2084\ncolorimetry-mode=bt2020rgb\n' > "$1"
  printf 'color-profile=montage-hdr\n\n[color-profile]\nname=montage-hdr\nprim_named=bt2020\ntf_named=st2084\n' >> "$1"
  printf 'min_lum=0.005\nmax_lum=10000\nref_lum=203\ntarget_min_lum=0.005\ntarget_max_lum=1000\n' >> "$1"
}
trap cleanup EXIT

# Waits for a Weston's socket: up to a minute, as the first Weston of a run loads Mesa and its LLVM from a cold disk,
# which has taken 17 s on a busy runner.
started() {  # pid socket log
  local i
  for i in $(seq 600); do
    [ -S "$XDG_RUNTIME_DIR/$2" ] && return 0
    kill -0 "$1" 2> /dev/null || break
    sleep 0.1
  done
  echo "Weston did not start ($2):" >&2
  tail -50 "$3" >&2 || true
  return 1
}
# Stops a Weston that must have lived through the test, and must stop cleanly.
stopped() {  # pid what
  local ended=0
  if kill -0 "$1" 2> /dev/null; then
    kill "$1" 2> /dev/null || true
    wait "$1" 2> /dev/null || ended=$?
    [ "$ended" = 0 ] && return 0
    echo "$2 did not stop cleanly (status $ended)" >&2
  else
    wait "$1" 2> /dev/null || ended=$?
    echo "$2 died during the test (status $ended)" >&2
  fi
  return 1
}

failed=0
run() {  # name color-management(true: a PQ output|sdr: an SDR output|false: off)
  local name=$1 cm=$2 dir="$work/$1"
  rm -rf "$dir"
  mkdir -p "$dir/shots"
  if [ "$cm" = sdr ]; then
    printf '[core]\ncolor-management=true\n' > "$dir/weston.ini"
  elif [ "$cm" = true ]; then
    pq_ini "$dir/weston.ini"
  else
    printf '[core]\ncolor-management=false\n' > "$dir/weston.ini"
  fi
  local socket="montage-hdr-$name"
  # (Weston's own libraries from its prefix; the test runs with the system's)
  # --fake-seat: the headless backend has no seat without it, and Weston 16.0.2's kiosk shell forgets a closed window
  # only through its seat's focus: with none, the output keeps pointing at the freed window (desktop_surface_removed
  # in kiosk-shell.c) and Weston crashes when the next window is mapped, as the test's viewer is after the main window
  # is hidden. Desktops always have a seat.
  # The log also has what Weston's renderer draws each surface with (its buffer's format and its colour
  # transformation), for when its screenshot is not what the test expects.
  # Weston 16 blends in its fragment shaders where the GL driver can read the framebuffer back in a shader, and under
  # Mesa's llvmpipe that showed every window black on the PQ output, SDR ones too. Without that one extension Weston
  # blends in a half-float buffer of its own, as its earlier releases always did.
  LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    MESA_EXTENSION_OVERRIDE="-GL_EXT_shader_framebuffer_fetch_non_coherent" "$weston_prefix/bin/weston" \
    --backend=headless --fake-seat --renderer=gl --width=1280 --height=720 --shell=kiosk --socket="$socket" \
    --config="$dir/weston.ini" --debug --log="$dir/weston.log" --logger-scopes=log,proto,gl-renderer-paint-nodes,color-lcms-transformations &
  weston=$!
  # (Up to a minute: the first Weston of a run loads Mesa and its LLVM from a cold disk, which has taken 17 s on a
  # busy runner.)
  local i
  for i in $(seq 600); do
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
  # (with the PQ output, the live HDR feed on the Source monitor too: liveHdrOnWayland)
  local fns="hdrViewerOnWayland"
  [ "$cm" = true ] && fns="$fns liveHdrOnWayland"
  WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 QTEST_FUNCTION_TIMEOUT=900000 \
    MONTAGE_TEST_WESTON_LOG="$dir/weston.log" MONTAGE_TEST_WESTON_CM="$cm" XDG_PICTURES_DIR="$dir/shots" \
    MONTAGE_TEST_WESTON_SCREENSHOOTER="$work/screenshooter" QT_LOGGING_RULES="qt.rhi.general=true" \
    "$test_app" $fns -o "$dir/result.txt,txt" -o -,txt || status=1
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
    echo "$fns failed with colour management $cm; Weston's log:" >&2
    grep -v ' -> \| client ' "$dir/weston.log" | tail -200 >&2 || true
    grep -E 'color|image_description|create_buffer|create_immed|\.create\(' "$dir/weston.log" | tail -60 >&2 || true
    echo "The last of Weston's protocol log:" >&2
    grep -v 'wl_registry@.*global(\|wl_shm@.*format(' "$dir/weston.log" | tail -40 >&2 || true
    failed=1
  fi
}

# Two outputs: the inner Weston's wayland0 SDR (sRGB, as Weston makes one by itself) and wayland1 PQ, as the "on" run's
# output. They differ in size, so that a window moved full screen from one to the other is placed afresh, and so that
# neither of the inner Weston's windows can cover the other in the outer one (a covered window is sent no frames, and
# the inner Weston's output in it would stop).
run_two() {
  local dir="$work/two"
  rm -rf "$dir"
  mkdir -p "$dir"
  local shell
  shell=$(printf '[shell]\nlocking=false\nanimation=none\nclose-animation=none\nstartup-animation=none\nfocus-animation=none\npanel-position=none\nbackground-color=0xff202020')
  printf '[core]\ncolor-management=false\n\n%s\n' "$shell" > "$dir/outer.ini"
  {
    printf '[core]\ncolor-management=true\n\n%s\n\n' "$shell"
    printf '[output]\nname=wayland0\nmode=1280x640\n\n'
    printf '[output]\nname=wayland1\nmode=1024x720\ncolor-profile=montage-hdr\n\n'
    printf '[color-profile]\nname=montage-hdr\nprim_named=bt2020\ntf_named=st2084\nmin_lum=0.005\nmax_lum=10000\nref_lum=203\n'
    printf 'target_min_lum=0.005\ntarget_max_lum=1000\n'
  } > "$dir/weston.ini"
  local status=0
  # The outer Weston: headless, the CPU's renderer (it only shows the inner one's two windows), the desktop shell.
  LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$weston_prefix/bin/weston" \
    --backend=headless --fake-seat --renderer=pixman --width=2400 --height=800 --shell=desktop --socket=montage-hdr-outer \
    --config="$dir/outer.ini" --log="$dir/outer.log" &
  outer=$!
  started "$outer" montage-hdr-outer "$dir/outer.log" || status=1
  if [ "$status" = 0 ]; then
    # The inner one, on its Wayland backend in the outer one: its GL renderer and colour management, as in the other
    # runs.
    LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      MESA_EXTENSION_OVERRIDE="-GL_EXT_shader_framebuffer_fetch_non_coherent" "$weston_prefix/bin/weston" \
      --backend=wayland --display=montage-hdr-outer --output-count=2 --renderer=gl --shell=desktop --socket=montage-hdr-two \
      --config="$dir/weston.ini" --debug --log="$dir/weston.log" --logger-scopes=log,proto &
    weston=$!
    started "$weston" montage-hdr-two "$dir/weston.log" || status=1
  fi
  if [ "$status" = 0 ]; then
    echo "::group::two"
    WAYLAND_DISPLAY=montage-hdr-two QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 QTEST_FUNCTION_TIMEOUT=900000 \
      MONTAGE_TEST_WESTON_LOG="$dir/weston.log" MONTAGE_TEST_WESTON_CM=two QT_LOGGING_RULES="qt.rhi.general=true" \
      "$test_app" hdrViewerOnTwoOutputs -o "$dir/result.txt,txt" -o -,txt || status=1
    echo "::endgroup::"
    if grep -q '^SKIP' "$dir/result.txt" 2> /dev/null; then
      grep '^SKIP' -A1 "$dir/result.txt"
      status=1
    fi
    sleep 1  # (the test's connection closed and taken in)
  fi
  if [ -n "$weston" ]; then
    stopped "$weston" "The Weston with two outputs" || status=1
    weston=""
  fi
  if [ -n "$outer" ]; then
    stopped "$outer" "The outer Weston" || status=1
    outer=""
  fi
  if [ "$status" != 0 ]; then
    echo "hdrViewerOnTwoOutputs failed; the outer Weston's log:" >&2
    tail -60 "$dir/outer.log" >&2 || true
    echo "The inner Weston's log:" >&2
    grep -v ' -> \| client ' "$dir/weston.log" | tail -150 >&2 || true
    echo "The last of its protocol log:" >&2
    grep -E 'enter[(]|leave[(]|fullscreen|configure[(]|image_description|wl_output@[0-9]+[.]name' "$dir/weston.log" | tail -60 >&2 || true
    failed=1
  fi
}

# Weston killed: the first Weston (PQ, as the "on" run's) is killed by the test itself; the second (an SDR output, as
# the "sdr" run's) runs from the start, so Qt finds it the moment it connects again (Qt 6 ends the program if no
# compositor answers then).
run_gone() {
  local dir="$work/gone"
  rm -rf "$dir"
  mkdir -p "$dir/shots"
  pq_ini "$dir/weston.ini"
  printf '[core]\ncolor-management=true\n' > "$dir/next.ini"
  local status=0 next="" name
  for name in weston next; do
    LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      MESA_EXTENSION_OVERRIDE="-GL_EXT_shader_framebuffer_fetch_non_coherent" "$weston_prefix/bin/weston" \
      --backend=headless --fake-seat --renderer=gl --width=1280 --height=720 --shell=kiosk --socket="montage-hdr-gone-$name" \
      --config="$dir/$name.ini" --debug --log="$dir/$name.log" --logger-scopes=log,proto &
    if [ "$name" = weston ]; then weston=$!; else next=$!; outer=$next; fi
    started "$!" "montage-hdr-gone-$name" "$dir/$name.log" || status=1
  done
  if [ "$status" = 0 ]; then
    echo "::group::gone"
    WAYLAND_DISPLAY=montage-hdr-gone-weston QT_WAYLAND_RECONNECT=1 QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 \
      QTEST_FUNCTION_TIMEOUT=900000 MONTAGE_TEST_WESTON_LOG="$dir/weston.log" MONTAGE_TEST_WESTON_PID="$weston" \
      MONTAGE_TEST_WESTON_NEXT=montage-hdr-gone-next XDG_PICTURES_DIR="$dir/shots" MONTAGE_TEST_WESTON_SCREENSHOOTER="$work/screenshooter" \
      QT_LOGGING_RULES="qt.rhi.general=true;qt.qpa.wayland=true" \
      "$test_app" hdrViewerWhenWestonGoes -o "$dir/result.txt,txt" -o -,txt || status=1
    echo "::endgroup::"
    if grep -q '^SKIP' "$dir/result.txt" 2> /dev/null; then
      grep '^SKIP' -A1 "$dir/result.txt"
      status=1
    fi
    sleep 1
  fi
  # The first must have been killed by the test (and so not be running), the second must have lived through it.
  if [ -n "$weston" ]; then
    if kill -0 "$weston" 2> /dev/null; then
      echo "The test did not kill the first Weston" >&2
      kill "$weston" 2> /dev/null || true
      status=1
    fi
    wait "$weston" 2> /dev/null || true
    weston=""
  fi
  if [ -n "$next" ]; then
    stopped "$next" "The Weston that took over" || status=1
    outer=""
  fi
  if [ "$status" != 0 ]; then
    echo "hdrViewerWhenWestonGoes failed; the second Weston's log:" >&2
    grep -v ' -> \| client ' "$dir/next.log" | tail -100 >&2 || true
    echo "The last of the first Weston's protocol log:" >&2
    grep -v 'wl_registry@.*global(\|wl_shm@.*format(' "$dir/weston.log" | tail -30 >&2 || true
    failed=1
  fi
}

run on true
run sdr sdr
run off false
run_two
run_gone
exit "$failed"
