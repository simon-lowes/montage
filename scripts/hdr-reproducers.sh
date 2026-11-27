#!/usr/bin/env bash
# Runs the two reproducers in tests/repro that the HDR viewer's checks under Weston rest on, and fails when either
# fault is no longer seen, so that a fixed Mesa or Weston is noticed (and the workaround for it can go):
#   - tests/repro/mesa-lastfragdata.c: under Mesa's llvmpipe, a GLSL ES 1.00 fragment shader that reads gl_LastFragData
#     and writes gl_FragColor writes zero (why Weston 16's blending in its shaders shows every window black, and why
#     scripts/wayland-hdr-check.sh hides GL_EXT_shader_framebuffer_fetch_non_coherent from Weston);
#   - tests/repro/weston-dropped-transformation.c: under Weston (scripts/build-weston.sh) with colour management on and
#     a BT.2100 PQ output, a shown surface described anew is not drawn until it is mapped again: the screen keeps its
#     old picture, and Weston's paint node log says the window was skipped (why app/WaylandColor maps the HDR surface
#     again when its description changes).
# Each is built here with the system's compiler (EGL and GLES 2 for the first; libwayland-client, and wayland-scanner
# with the prefix's wayland-protocols, for the second) and its output kept in the work directory.
#
#   scripts/hdr-reproducers.sh <weston prefix> <work directory>
set -euo pipefail
weston_prefix=$(realpath "$1")
work=$(realpath -m "$2")
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$work"
export XDG_RUNTIME_DIR="$work/runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
failed=0

echo "::group::Mesa: gl_LastFragData with gl_FragColor"
cc -std=c99 -O1 -Wall -o "$work/mesa-lastfragdata" "$here/tests/repro/mesa-lastfragdata.c" -lEGL -lGLESv2 -lm
mesa=0
LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless "$work/mesa-lastfragdata" | tee "$work/mesa-lastfragdata.txt" || mesa=$?
echo "::endgroup::"
case "$mesa" in
  0) echo "Mesa's fault is seen ($(sed -n 2p "$work/mesa-lastfragdata.txt"))" ;;
  1) echo "Mesa's fault is no longer seen: Weston's blending in its shaders may work now (see tests/repro/mesa-lastfragdata.c)" >&2
     failed=1 ;;
  *) echo "The Mesa reproducer could not run (status $mesa)" >&2
     failed=1 ;;
esac

echo "::group::Weston: a surface described anew"
protocols="$weston_prefix/share/wayland-protocols"
gen="$work/protocol"
mkdir -p "$gen"
for xml in "$protocols/staging/color-management/color-management-v1.xml" "$protocols/stable/xdg-shell/xdg-shell.xml"; do
  base=$(basename "$xml" .xml)
  wayland-scanner client-header "$xml" "$gen/$base-client-protocol.h"
  wayland-scanner private-code "$xml" "$gen/$base-protocol.c"
done
cc -std=gnu11 -O1 -Wall -I"$gen" -o "$work/weston-dropped-transformation" "$here/tests/repro/weston-dropped-transformation.c" \
  "$gen/color-management-v1-protocol.c" "$gen/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client)
printf '#!/bin/sh\nLD_LIBRARY_PATH="%s/lib" exec "%s/bin/weston-screenshooter" "$@"\n' "$weston_prefix" "$weston_prefix" > "$work/screenshooter"
chmod +x "$work/screenshooter"
# The output as scripts/wayland-hdr-check.sh drives it: BT.2100 PQ (BT.2020, reference white 203 nits, a 1000-nit
# target), Weston's GL renderer on llvmpipe blending in a buffer of its own (the extension hidden: the fault above).
printf '[core]\ncolor-management=true\n\n[output]\nname=headless\neotf-mode=st2084\ncolorimetry-mode=bt2020rgb\n' > "$work/weston.ini"
printf 'color-profile=montage-hdr\n\n[color-profile]\nname=montage-hdr\nprim_named=bt2020\ntf_named=st2084\n' >> "$work/weston.ini"
printf 'min_lum=0.005\nmax_lum=10000\nref_lum=203\ntarget_min_lum=0.005\ntarget_max_lum=1000\n' >> "$work/weston.ini"
socket=montage-hdr-repro
rm -rf "$work/shots"
mkdir -p "$work/shots"
LD_LIBRARY_PATH="$weston_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" LIBGL_ALWAYS_SOFTWARE=1 \
  MESA_EXTENSION_OVERRIDE="-GL_EXT_shader_framebuffer_fetch_non_coherent" "$weston_prefix/bin/weston" \
  --backend=headless --fake-seat --renderer=gl --width=1280 --height=720 --shell=kiosk --socket="$socket" \
  --config="$work/weston.ini" --debug --log="$work/weston.log" --logger-scopes=log,proto,gl-renderer-paint-nodes &
weston=$!
trap 'kill "$weston" 2> /dev/null || true' EXIT
for i in $(seq 600); do
  [ -S "$XDG_RUNTIME_DIR/$socket" ] && break
  kill -0 "$weston" 2> /dev/null || break
  sleep 0.1
done
client=0
if [ -S "$XDG_RUNTIME_DIR/$socket" ]; then
  WAYLAND_DISPLAY="$socket" SCREENSHOOTER="$work/screenshooter" "$work/weston-dropped-transformation" "$work/shots" || client=$?
else
  echo "Weston did not start:" >&2
  tail -50 "$work/weston.log" >&2 || true
  client=2
fi
sleep 1
kill "$weston" 2> /dev/null || true
wait "$weston" 2> /dev/null || true
trap - EXIT
echo "::endgroup::"
if [ "$client" != 0 ]; then
  echo "The Weston reproducer could not run (status $client)" >&2
  failed=1
else
  # The middle of each step's screenshot, as a fraction of white (8 or 16 bits a sample, as Weston wrote it).
  verdict=0
  python3 -I - "$work/shots" "$work/weston.log" << 'EOF' || verdict=$?
import glob, os, struct, sys, zlib

def middle(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    at, idat, w = 8, b'', 0
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b'IHDR':
            w, h, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        at += 12 + n
    assert colour in (2, 6) and depth in (8, 16) and interlace == 0, (path, colour, depth, interlace)
    bpp = (3 if colour == 2 else 4) * depth // 8
    stride = w * bpp
    raw = zlib.decompress(idat)
    prev = bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if y == h // 2:
            px = line[(w // 2) * bpp:(w // 2) * bpp + bpp]
            top = 255.0 if depth == 8 else 65535.0
            vals = [px[i] if depth == 8 else px[2 * i] << 8 | px[2 * i + 1] for i in range(3)]
            return [v / top for v in vals]
        prev = line

seen = {}
for step in ('step-1', 'step-2', 'step-2-later', 'step-3'):
    shots = sorted(glob.glob(os.path.join(sys.argv[1], step, '*.png')))
    if not shots:
        print(f'{step}: no screenshot', file=sys.stderr)
        sys.exit(2)
    seen[step] = max(middle(shots[-1]))
    print(f'{step}: the middle at {seen[step]:.3f} of white')
# Step 1 shows a dark grey (0x40) and steps 2 and 3 a light grey (0xc0), so a surface drawn at step 2 shows the light
# grey there, and one left out keeps step 1's dark grey (or black, where nothing was drawn before).
light = lambda s: abs(seen[s] - seen['step-3']) < 0.05
if not (0.1 < seen['step-1'] < seen['step-3'] - 0.2):
    print('The surface was not shown dark when first described, and light when mapped again: not the fault described',
          file=sys.stderr)
    sys.exit(2)
# And Weston's own account (its gl-renderer-paint-nodes scope): the reproducer's window left out of a repaint because
# its colour transformation was not there.
skipped, node = 0, ''
for line in open(sys.argv[2], errors='replace'):
    if 'Paint node' in line:
        node = line
    elif 'skipped repaint: shader config failure' in line and "Weston's dropped transformation" in node:
        skipped += 1
print(f"Weston left the window out of {skipped} repaint(s): shader config failure")
if not light('step-2') and not light('step-2-later') and skipped:
    print("Weston's fault is seen: the surface described anew is not drawn until it is mapped again "
          f"(the screen kept {seen['step-2']:.3f} where the new picture is {seen['step-3']:.3f})")
    sys.exit(0)
if light('step-2') and light('step-2-later') and not skipped:
    print("Weston's fault is no longer seen: a surface described anew is drawn (see tests/repro/weston-dropped-transformation.c)",
          file=sys.stderr)
    sys.exit(1)
print("The screenshots and Weston's log disagree about whether the surface described anew was drawn", file=sys.stderr)
sys.exit(2)
EOF
  if [ "$verdict" != 0 ]; then
    grep -E 'image_description|set_image_description|paint node|color' "$work/weston.log" | tail -60 >&2 || true
    failed=1
  fi
fi
exit "$failed"
