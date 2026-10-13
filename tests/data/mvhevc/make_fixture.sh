#!/bin/bash
# Writes spatial.mov: MV-HEVC (two views, 128x72, ten frames at 25 fps, no B-frames), the left eye a red square at x 20,
# the right a blue one at x 32, on dark grey. Needs FFmpeg 8 and x265 4.1 built with -DENABLE_MULTIVIEW=ON (used only
# to make this file). The track is tagged hev1 so each view's parameter sets stay in the stream.
# And spatial-eyes.mov: the same stream with an Apple eyes box (vexu, eyes, stri) saying it holds both eyes, the views
# the other way round (eye_views_reversed), so the left eye is the blue one.
set -e
cd "$(dirname "$0")"
ffmpeg -v error -f lavfi -i "color=c=0x202020:s=128x72:r=25:d=0.4,drawbox=x=20:y=20:w=24:h=24:c=red:t=fill[l];color=c=0x202020:s=128x72:r=25:d=0.4,drawbox=x=32:y=20:w=24:h=24:c=blue:t=fill[r];[l][r]hstack" \
    -pix_fmt yuv420p -f rawvideo sbs.yuv -y
x265 --input sbs.yuv --input-res 128x72 --fps 25 --num-views 2 --format 1 --preset ultrafast --bframes 0 --log-level error -o spatial.hevc
ffmpeg -v error -fflags +genpts -f hevc -r 25 -i spatial.hevc -c copy -tag:v hev1 spatial.mov -y
rm -f sbs.yuv spatial.hevc
python3 - <<'PY'
import struct
data = bytearray(open("spatial.mov", "rb").read())
stri = struct.pack(">I4sIB", 13, b"stri", 0, 0x0B)  # left and right eye views, reversed
eyes = struct.pack(">I4s", 8 + len(stri), b"eyes") + stri
vexu = struct.pack(">I4s", 8 + len(eyes), b"vexu") + eyes
def find(start, end, kind):
    at = start
    while at < end:
        size, k = struct.unpack(">I4s", data[at:at + 8])
        if k == kind: return at, size
        at += size
    raise SystemExit("no " + kind.decode())
grown = []  # the boxes holding the sample entry, each grows by the new box
at, size = find(0, len(data), b"moov")
assert data.find(b"mdat") < at, "moov must follow mdat (the chunk offsets stay as they are)"
grown.append(at)
for kind in (b"trak", b"mdia", b"minf", b"stbl", b"stsd"):
    at, size = find(at + 8, at + size, kind)
    grown.append(at)
entry = at + 16  # stsd: version, flags and entry count, then the hev1 entry
esize = struct.unpack(">I", data[entry:entry + 4])[0]
grown.append(entry)
data[entry + esize:entry + esize] = vexu
for box in grown:
    struct.pack_into(">I", data, box, struct.unpack(">I", data[box:box + 4])[0] + len(vexu))
open("spatial-eyes.mov", "wb").write(data)
PY
