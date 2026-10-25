#!/usr/bin/env python3
"""Writes anim.gltf: one triangle on a node animated three ways, for the 3D model tests.

The node's translation runs LINEAR through (0, 0, 0), (1, 0, 0) and (1, 2, 0) at 0, 1 and 2 s; its rotation STEPs
through no turn, a quarter turn about y and a half turn about y at the same times; its mesh's one morph target moves
the first corner 1 along z, its weight running LINEAR from 0 at 0 s to 1 at 2 s.
"""
import base64
import json
import math
import struct
from pathlib import Path


def pack(fmt, values):
    return b"".join(struct.pack("<" + fmt, *v) for v in values)


positions = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
target = [(0, 0, 1), (0, 0, 0), (0, 0, 0)]
times = [(0.0,), (1.0,), (2.0,)]
translations = [(0, 0, 0), (1, 0, 0), (1, 2, 0)]
s = math.sqrt(0.5)
rotations = [(0, 0, 0, 1), (0, s, 0, s), (0, 1, 0, 0)]
weight_times = [(0.0,), (2.0,)]
weights = [(0.0,), (1.0,)]

parts = [pack("fff", positions), pack("fff", target), pack("f", times), pack("fff", translations), pack("ffff", rotations),
         pack("f", weight_times), pack("f", weights)]
views, accessors, offset = [], [], 0
kinds = [("VEC3", 3), ("VEC3", 3), ("SCALAR", 3), ("VEC3", 3), ("VEC4", 3), ("SCALAR", 2), ("SCALAR", 2)]
for data, (kind, count) in zip(parts, kinds):
    views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
    accessor = {"bufferView": len(views) - 1, "componentType": 5126, "count": count, "type": kind}
    if len(accessors) in (0, 1):
        accessor["min"] = [min(p[k] for p in (positions if len(accessors) == 0 else target)) for k in range(3)]
        accessor["max"] = [max(p[k] for p in (positions if len(accessors) == 0 else target)) for k in range(3)]
    if len(accessors) in (2, 5):
        src = times if len(accessors) == 2 else weight_times
        accessor["min"], accessor["max"] = [min(t[0] for t in src)], [max(t[0] for t in src)]
    accessors.append(accessor)
    offset += len(data)
blob = b"".join(parts)

gltf = {
    "asset": {"version": "2.0", "generator": "Montage test fixture"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "moving", "mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}], "weights": [0]}],
    "animations": [{
        "name": "move",
        "samplers": [
            {"input": 2, "output": 3, "interpolation": "LINEAR"},
            {"input": 2, "output": 4, "interpolation": "STEP"},
            {"input": 5, "output": 6, "interpolation": "LINEAR"},
        ],
        "channels": [
            {"sampler": 0, "target": {"node": 0, "path": "translation"}},
            {"sampler": 1, "target": {"node": 0, "path": "rotation"}},
            {"sampler": 2, "target": {"node": 0, "path": "weights"}},
        ],
    }],
    "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
    "bufferViews": views,
    "accessors": accessors,
}
Path(__file__).with_name("anim.gltf").write_text(json.dumps(gltf, indent=1) + "\n")
