#!/usr/bin/env python3
# Writes small.sofa (and small.zip holding it, as SADIE II packs theirs): a made-up head (AES69 SimpleFreeFieldHRIR, 44.1 kHz, 26 directions, 32 taps) whose ear nearer the
# sound hears it earlier and louder, for media/Sofa.cpp's tests. Needs the netCDF4 module.
import math, os
import netCDF4
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
f = netCDF4.Dataset(os.path.join(here, "small.sofa"), "w", format="NETCDF4")
attrs = {
    "Conventions": "SOFA", "Version": "2.1", "SOFAConventions": "SimpleFreeFieldHRIR", "SOFAConventionsVersion": "1.0",
    "APIName": "Montage test fixture", "APIVersion": "1.0", "DataType": "FIR", "RoomType": "free field",
    "Title": "Montage test head", "DateCreated": "2026-10-13 00:00:00", "DateModified": "2026-10-13 00:00:00",
    "AuthorContact": "-", "Organization": "-", "License": "CC0", "ApplicationName": "-", "ApplicationVersion": "-",
    "Comment": "-", "History": "-", "References": "-", "Origin": "-", "DatabaseName": "Montage",
    "ListenerShortName": "test",
}
for k, v in attrs.items():
    f.setncattr(k, v)
# Directions: the six axes, the twelve edge midpoints and the eight corners of a cube (azimuth anticlockwise).
dirs = []
for v in [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]:
    dirs.append(v)
h = math.sqrt(0.5)
for a in range(3):
    for s1 in (h, -h):
        for s2 in (h, -h):
            p = [0.0, 0.0, 0.0]
            p[(a + 1) % 3], p[(a + 2) % 3] = s1, s2
            dirs.append(tuple(p))
t = math.sqrt(1 / 3)
for x in (t, -t):
    for y in (t, -t):
        for z in (t, -t):
            dirs.append((x, y, z))
M, R, N, E = len(dirs), 2, 32, 1
for name, size in [("M", M), ("R", R), ("N", N), ("E", E), ("I", 1), ("C", 3)]:
    f.createDimension(name, size)

def var(name, dims, values, **va):
    x = f.createVariable(name, "f8", dims)
    x[:] = values
    for k, v in va.items():
        x.setncattr(k, v)

var("ListenerPosition", ("I", "C"), [[0, 0, 0]], Type="cartesian", Units="metre")
var("ListenerUp", ("I", "C"), [[0, 0, 1]])
var("ListenerView", ("I", "C"), [[1, 0, 0]], Type="cartesian", Units="metre")
var("ReceiverPosition", ("R", "C", "I"), [[[0], [0.09], [0]], [[0], [-0.09], [0]]], Type="cartesian", Units="metre")
var("EmitterPosition", ("E", "C", "I"), [[[0], [0], [0]]], Type="cartesian", Units="metre")
src = [[math.degrees(math.atan2(y, x)), math.degrees(math.asin(z)), 1.2] for x, y, z in dirs]
var("SourcePosition", ("M", "C"), src, Type="spherical", Units="degree, degree, metre")
ir = np.zeros((M, R, N))
for m, (x, y, z) in enumerate(dirs):
    ir[m, 0, 4 + round(5 * (1 - y))] = 0.6 + 0.4 * y  # left ear (+Y)
    ir[m, 1, 4 + round(5 * (1 + y))] = 0.6 - 0.4 * y
var("Data.IR", ("M", "R", "N"), ir)
var("Data.SamplingRate", ("I",), [44100], Units="hertz")
var("Data.Delay", ("I", "R"), [[0, 0]])
f.close()
import zipfile
with zipfile.ZipFile(os.path.join(here, "small.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(here, "small.sofa"), "small_HRIR_SOFA/small_48K_test.sofa")
