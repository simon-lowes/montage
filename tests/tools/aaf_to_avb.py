#!/usr/bin/env python3
"""Writes a Media Composer bin (with pyavb) holding what an AAF file holds (read
with pyaaf2): its master, file, tape and composition mobs, their tracks, and
each composition's clips, fillers, transitions and motion effects, as Media
Composer keeps them. Picture files' mobs point at their media as Media
Composer's own do (a media stream locator: found by mob ID), sound files' by
path. With --extras, also a subclip, custom columns on the first master clip
and a locator on the first clip of the first composition.
Usage: aaf_to_avb.py in.aaf out.avb [--extras]
Used by test_media when MONTAGE_TEST_PYAVB names a Python with pyavb and pyaaf2."""
import sys
from urllib.parse import unquote, urlparse

import aaf2
import avb
from avb.mobid import MobID

KINDS = {"Picture": "picture", "Sound": "sound", "Timecode": "timecode", "Edgecode": "edgecode"}


def rate(r):
    r = float(r)
    return int(r) if r == int(r) else r


def kind_of(slot):
    return KINDS.get(slot.media_kind, "picture")


def track_numbers(mob):
    """Each AAF slot ID of a mob as Media Composer numbers its tracks: by kind, from 1."""
    out, counts = {}, {}
    for slot in mob.slots:
        k = kind_of(slot)
        counts[k] = counts.get(k, 0) + 1
        out[slot.slot_id] = counts[k]
    return out


class Converter:
    def __init__(self, f, src):
        self.f = f
        self.numbers = {str(m.mob_id): track_numbers(m) for m in src.content.mobs}

    def component(self, seg, edit_rate, kind):
        f, k = self.f, type(seg).__name__
        if k == "Sequence":
            c = f.create.Sequence(edit_rate=edit_rate, media_kind=kind)
            for part in seg.components:
                c.components.append(self.component(part, edit_rate, kind))
            return c
        if k == "Filler":
            c = f.create.Filler(edit_rate=edit_rate, media_kind=kind)
            c.length = seg.length
            return c
        if k == "Timecode":
            c = f.create.Timecode(edit_rate=edit_rate, media_kind="timecode")
            c.length = seg.length
            c.start = seg.start
            c.fps = seg.fps
            c.flags = 0
            return c
        if k == "SourceClip":
            c = f.create.SourceClip(edit_rate=edit_rate, media_kind=kind)
            c.length = seg.length
            c.start_time = seg.start
            target = str(seg["SourceID"].value)
            c.mob_id = MobID(target)
            c.track_id = self.numbers.get(target, {}).get(seg["SourceMobSlotID"].value, 0)
            return c
        if k == "Transition":
            c = f.create.TransitionEffect(edit_rate=edit_rate, media_kind=kind)
            c.effect_id = "EFF2_BLEND_DISSOLVE" if kind == "picture" else ""
            c.length = seg.length
            c.cutpoint = seg["CutPoint"].value
            for name in ("left_length", "right_length", "info_current", "info_smooth", "info_version", "info_color_item",
                         "info_quality", "info_is_reversed"):
                setattr(c, name, 0)
            for name in ("info_aspect_on", "info_force_software", "info_never_hardware"):
                setattr(c, name, False)
            c.keyframes = None
            c.tracks.append(f.create.Track())
            c.tracks.append(f.create.Track())
            return c
        if k == "OperationGroup" and seg.operation.name == "Motion Control":
            c = f.create.MotionEffect(edit_rate=edit_rate, media_kind=kind)
            c.length = seg.length
            ratio = next(p.value for p in seg.parameters if p.name == "Speed Ratio")
            c.speed_ratio = [ratio.numerator, ratio.denominator]
            c.phase_offset = 0
            t = f.create.Track()
            t.index = 1
            t.component = self.component(seg.segments[0], edit_rate, kind)
            c.tracks.append(t)
            return c
        raise ValueError("cannot convert " + k)

    def tracks(self, src, dst):
        numbers = self.numbers[str(src.mob_id)]
        for slot in src.slots:
            t = self.f.create.Track()
            t.index = numbers[slot.slot_id]
            t.component = self.component(slot.segment, rate(slot["EditRate"].value), kind_of(slot))
            dst.tracks.append(t)

    def mob(self, src):
        f, k = self.f, type(src).__name__
        m = f.create.Composition(mob_type=k)
        m.mob_id = MobID(str(src.mob_id))
        m.name = src.name or ""
        self.tracks(src, m)
        m.length = max([s.segment.length for s in src.slots if s.media_kind != "Timecode"] or [0])
        if k == "SourceMob":
            d = src.descriptor
            dk = type(d).__name__
            if dk == "TapeDescriptor":
                m.descriptor = f.create.TapeDescriptor()
                m.descriptor.mob_kind = 2
            else:
                url = d["Locator"][0]["URLString"].value
                path = unquote(urlparse(url).path)
                if dk == "PCMDescriptor":
                    m.descriptor = f.create.PCMADescriptor()
                    m.descriptor.channels = 1
                    m.descriptor.quantization_bits = 24
                    m.descriptor.sample_rate = rate(d["SampleRate"].value)
                    for name in ("locked", "has_peak_envelope_data"):
                        setattr(m.descriptor, name, False)
                    for name in ("audio_ref_level", "electro_spatial_formulation", "dial_norm", "coding_format",
                                 "sequence_offset", "peak_envelope_version", "peak_envelope_format",
                                 "points_per_peak_value", "peak_envelope_block_size", "peak_channel_count",
                                 "peak_frame_count", "peak_of_peaks_offset", "peak_envelope_timestamp"):
                        setattr(m.descriptor, name, 0)
                    m.descriptor.block_align = 3
                    m.descriptor.average_bps = 3 * int(m.descriptor.sample_rate)
                    loc = f.create.MacFileLocator()
                    loc.path = path
                    loc.path_posix = path
                    loc.path_utf8 = path
                else:
                    m.descriptor = f.create.CDCIDescriptor()
                    loc = f.create.MSMLocator()
                    loc.last_known_volume = "Media"
                    loc.last_known_volume_utf8 = "Media"
                    loc.domain_type = 0
                    loc.mob_id = MobID(str(src.mob_id))
                m.descriptor.mob_kind = 1
                m.descriptor.length = d["Length"].value
                m.descriptor.edit_rate = rate(d["SampleRate"].value)
                m.descriptor.locator = loc
        return m


def main():
    src_path, dst_path = sys.argv[1], sys.argv[2]
    extras = "--extras" in sys.argv
    with aaf2.open(src_path, "r") as src, avb.open() as f:
        conv = Converter(f, src)
        mobs = [conv.mob(m) for m in src.content.mobs]
        if extras:
            master = next(m for m in mobs if m.mob_type == "MasterMob" and m.name.endswith(".mov"))
            user = f.create.Attributes()
            user["Scene"] = "12A"
            user["Take"] = "3"
            user["Comments"] = "Best take"
            master.attributes["_USER"] = user
            # A subclip of its second to fifth frames.
            sub = f.create.Composition(mob_type="MasterMob")
            sub.name = master.name + ".Sub.01"
            sub.usage_code = 2
            t = f.create.Track()
            t.index = 1
            t.component = f.create.SourceClip(edit_rate=25, media_kind="picture")
            t.component.length = 4
            t.component.start_time = 2
            t.component.track_id = 1
            t.component.mob_id = master.mob_id
            sub.tracks.append(t)
            sub.length = 4
            mobs.append(sub)
            comp = next(m for m in mobs if m.mob_type == "CompositionMob")
            clip = next(t.component for t in comp.tracks if t.component.media_kind == "picture").components[0]
            crumbs = f.create.TimeCrumbList()
            marker = f.create.Marker()
            marker.mob_id = comp.mob_id
            marker.position = 0
            marker.comp_offset = 3
            marker.color = [0, 0, 65535]
            marker.attributes = f.create.Attributes()
            marker.attributes["_ATN_CRM_COM"] = "Check focus"
            marker.attributes["_ATN_CRM_COLOR"] = "Blue"
            marker.attributes["_ATN_CRM_USER"] = "editor"
            crumbs.append(marker)
            clip.attributes["_TMP_CRM"] = crumbs
        for m in mobs:
            f.content.add_mob(m)
        f.write(dst_path)


if __name__ == "__main__":
    main()
