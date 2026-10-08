#!/usr/bin/env python3
"""Reads an AAF file with pyaaf2 (an independent reader) and prints what it
finds as JSON: the top-level composition's tracks and their components, each
source clip followed through its master mob to the WAV file it plays.
Used by test_media when MONTAGE_TEST_PYAAF2 names a Python with pyaaf2."""
import json, sys
import aaf2

def walk(seg, out):
    kind = type(seg).__name__
    if kind == "Sequence":
        for c in seg.components:
            walk(c, out)
    elif kind == "OperationGroup":
        params = {}
        for p in seg.parameters:
            pv = type(p).__name__
            if pv == "ConstantValue":
                params["constant"] = float(p.value)
            else:
                params["points"] = [[float(cp.time), float(cp.value)] for cp in p["PointList"]]
        inner = []
        for s in seg.segments:
            walk(s, inner)
        out.append({"type": "gain", "op": seg.operation.name, "length": seg.length, "params": params, "inputs": inner})
    elif kind == "Transition":
        out.append({"type": "transition", "length": seg.length, "cut": seg["CutPoint"].value, "op": seg["OperationGroup"].value.operation.name})
    elif kind == "Filler":
        out.append({"type": "filler", "length": seg.length})
    elif kind == "SourceClip":
        master = seg.mob
        slot = master.slot_at(seg.slot_id)
        filemob = slot.segment.mob
        loc = filemob.descriptor["Locator"][0]["URLString"].value
        entry = {"type": "clip", "length": seg.length, "start": seg.start, "slot": seg.slot_id,
                 "master": master.name, "file": loc, "rate": str(filemob.descriptor["SampleRate"].value),
                 "samples": filemob.descriptor["Length"].value}
        for k, key in (("fade_in", "FadeInLength"), ("fade_out", "FadeOutLength")):
            if key in seg.keys():
                entry[k] = seg[key].value
        out.append(entry)
    else:
        out.append({"type": kind})

with aaf2.open(sys.argv[1], "r") as f:
    comps = list(f.content.toplevel())
    result = {"compositions": len(comps), "mobs": len(list(f.content.mobs)), "tracks": []}
    for slot in comps[0].slots:
        seg = slot.segment
        if type(seg).__name__ == "Timecode":
            result["timecode"] = {"start": seg.start, "fps": seg.fps, "length": seg.length}
            continue
        comps_out = []
        walk(seg, comps_out)
        result["tracks"].append({"name": slot.name, "rate": str(slot.edit_rate), "physical": slot["PhysicalTrackNumber"].value,
                                 "length": seg.length, "components": comps_out})
    print(json.dumps(result))
