#!/usr/bin/env python3
"""Writes an AAF laid out as Media Composer lays one out, for Montage's AAF import test (needs pyaaf2 and ffprobe).

A 25 fps composition, "Scene 4 Cut", AMA-linked to a movie with picture and stereo sound: a picture track with a
10-frame filler, a 40-frame clip from frame 5, a 10-frame dissolve cut halfway and a 30-frame clip from frame 60
that fades out over its last 10 frames (a dissolve to filler); two sound tracks (the movie's two channels) under the
first clip, the second turned down 6 dB by an Audio Gain effect; and a marker at frame 20.

With --nested: a top-level composition "Main" whose picture and two sound tracks play, after a 10-frame filler,
40 frames of a nested composition "Nest" (the movie's picture and sound from frame 5).

Usage: make_aaf.py out.aaf movie.mov [--nested]
"""
import json
import os
import subprocess
import sys

import aaf2

out, movie = sys.argv[1], os.path.abspath(sys.argv[2])
meta = json.loads(subprocess.check_output(["ffprobe", "-v", "quiet", "-of", "json", "-show_format", "-show_streams", movie]))
nested = len(sys.argv) > 3 and sys.argv[3] == "--nested"


def dissolve_def(f):
    d = f.create.OperationDef("0c3bea40-fc05-11d2-8a29-0050040ef7d2", "VideoDissolve_2", "Video Dissolve")
    d.media_kind = "picture"
    d["NumberInputs"].value = 2
    f.dictionary.register_def(d)
    return d


with aaf2.open(out, "w") as f:
    master = f.content.create_ama_link(movie, meta)[0]
    picture = [s.slot_id for s in master.slots if s.media_kind == "Picture"]
    sound = [s.slot_id for s in master.slots if s.media_kind == "Sound"]

    if nested:
        nest = f.create.CompositionMob("Nest")
        f.content.mobs.append(nest)
        v = nest.create_picture_slot(25)
        v.segment.components.append(master.create_source_clip(slot_id=picture[0], start=5, length=40))
        nest_sound = []
        for slot_id in sound[:2]:
            a = nest.create_sound_slot(25)
            a.segment.components.append(master.create_source_clip(slot_id=slot_id, start=5, length=40))
            nest_sound.append(a.slot_id)
        main = f.create.CompositionMob("Main")
        main.usage = "Usage_TopLevel"
        f.content.mobs.append(main)
        mv = main.create_picture_slot(25)
        mv.segment.components.append(f.create.Filler("picture", 10))
        mv.segment.components.append(nest.create_source_clip(slot_id=v.slot_id, start=0, length=40))
        for slot_id in nest_sound:
            ma = main.create_sound_slot(25)
            ma.segment.components.append(f.create.Filler("sound", 10))
            ma.segment.components.append(nest.create_source_clip(slot_id=slot_id, start=0, length=40))
    else:
        comp = f.create.CompositionMob("Scene 4 Cut")
        comp.usage = "Usage_TopLevel"
        f.content.mobs.append(comp)

        pic = comp.create_picture_slot(25)
        pic.name = "V1"
        seq = pic.segment
        seq.components.append(f.create.Filler("picture", 10))
        seq.components.append(master.create_source_clip(slot_id=picture[0], start=5, length=40))
        dissolve = dissolve_def(f)
        transition = f.create.Transition("picture", 10)
        transition["OperationGroup"].value = f.create.OperationGroup(dissolve, 10)
        transition["CutPoint"].value = 5
        seq.components.append(transition)
        seq.components.append(master.create_source_clip(slot_id=picture[0], start=60, length=30))
        fade = f.create.Transition("picture", 10)
        fade["OperationGroup"].value = f.create.OperationGroup(dissolve, 10)
        fade["CutPoint"].value = 5
        seq.components.append(fade)
        seq.components.append(f.create.Filler("picture", 10))

        gain = f.create.OperationDef("9d2ea894-0968-11d3-8a38-0050040ef7d2", "MonoAudioGain", "Audio Gain")
        gain.media_kind = "sound"
        gain["NumberInputs"].value = 1
        f.dictionary.register_def(gain)
        amplitude = f.create.ParameterDef("e4962321-2267-11d3-8a4c-0050040ef7d2", "Amplitude", "Level", "Rational")
        f.dictionary.register_def(amplitude)
        for i, slot_id in enumerate(sound[:2]):
            snd = comp.create_sound_slot(25)
            snd.name = "A%d" % (i + 1)
            s = snd.segment
            s.components.append(f.create.Filler("sound", 10))
            clip = master.create_source_clip(slot_id=slot_id, start=5, length=40)
            if i == 1:
                group = f.create.OperationGroup(gain, 40)
                group.segments.append(clip)
                value = f.create.ConstantValue(amplitude, 0.5)
                group.parameters.append(value)
                s.components.append(group)
            else:
                s.components.append(clip)

        events = comp.create_empty_sequence_slot(25, media_kind="DescriptiveMetadata")
        marker = f.create.DescriptiveMarker()
        marker["Position"].value = 20
        marker["Comment"].value = "Check focus"
        marker["DescribedSlots"].value = set([1])
        events.segment.components.append(marker)
