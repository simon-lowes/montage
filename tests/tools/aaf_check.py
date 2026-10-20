#!/usr/bin/env python3
"""Reads an AAF file with pyaaf2 (an independent reader) and prints what it
finds as JSON: the top-level composition's tracks and their components, each
source clip followed through its master mob to the file it plays. After
--mxf, MXF files whose package IDs it reports too (pyaaf2's MXF reader).
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
        out.append({"type": "gain", "op": seg.operation.name, "op_id": str(seg.operation.auid), "length": seg.length, "params": params,
                    "inputs": inner})
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
        desc = filemob.descriptor
        entry["master_id"] = str(master.mob_id)
        entry["file_id"] = str(filemob.mob_id)
        entry["file_slot"] = slot.segment.slot_id
        entry["descriptor"] = type(desc).__name__
        if "ContainerFormat" in desc.keys():
            entry["container"] = str(desc["ContainerFormat"].value.auid)
        if "Compression" in desc.keys():
            entry["compression"] = str(desc["Compression"].value)
        if "ImageAspectRatio" in filemob.descriptor.keys():
            entry["aspect"] = str(filemob.descriptor["ImageAspectRatio"].value)
        # A file recorded as a source with a timecode ("tape" mob): where the file starts in it.
        fseg = filemob.slots[0].segment
        tape = fseg.mob if type(fseg).__name__ == "SourceClip" else None
        if tape is not None and any(type(ts.segment).__name__ == "Timecode" for ts in tape.slots):
            entry["tape_tc"] = fseg.start
        out.append(entry)
    else:
        out.append({"type": kind})

mxf_files = sys.argv[sys.argv.index("--mxf") + 1:] if "--mxf" in sys.argv else []

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
    if mxf_files:
        import aaf2.mxf
        result["mxf"] = {}
        for path in mxf_files:
            m = aaf2.mxf.MXFFile(path)
            ids = {"material": [], "source": [], "slots": {}}
            for package in m.packages():
                kind = "material" if isinstance(package, aaf2.mxf.MXFMaterialPackage) else "source"
                ids[kind].append(str(package.mob_id))
                # Each track's ID and what it carries (the data kind of its sequence's first component).
                slots = {}
                for t in package.iter_strong_refs("Slots"):
                    seq = m.resolve(t.data.get("Segment"))
                    comps = [m.resolve(r) for r in seq.data.get("Components", [])] if seq else []
                    slots[str(t.data.get("SlotID"))] = comps[0].data.get("DataDef") if comps else None
                ids["slots"][str(package.mob_id)] = slots
            result["mxf"][path] = ids
    print(json.dumps(result))
