#include "MergeClips.h"

#include <algorithm>
#include <cmath>

namespace montage {

Id mergeClips(Project& p, Id videoId, const std::vector<Id>& sounds, const std::vector<double>& offsets, const MergeOptions& o,
              std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return Id(0);
    };
    const MediaItem* vp = p.findMedia(videoId);
    if (!vp || !vp->hasVideo || vp->kind != MediaKind::Video) return fail("Choose a camera clip (a video file)");
    if (sounds.empty() || offsets.size() != sounds.size()) return fail("Choose the recorder's sound files to merge");
    for (Id id : sounds) {
        const MediaItem* m = p.findMedia(id);
        if (!m || !m->hasAudio || m->kind == MediaKind::Sequence) return fail("Only sound files can be merged with the picture");
        if (id == videoId) return fail("A clip cannot be merged with itself");
    }
    const MediaItem video = *vp;
    const Sequence* active = p.active();
    const Rational fps = video.fps.valid() ? video.fps : active ? active->fps : Rational{30, 1};
    const int w = video.width > 0 ? video.width : active ? active->width : 1920;
    const int h = video.height > 0 ? video.height : active ? active->height : 1080;
    const bool camera = o.keepCameraAudio && video.hasAudio;
    const std::string name = o.name.empty() ? video.name + " (merged)" : o.name;
    Sequence seq = makeSequence(p, name, w, h, fps, 1, int(sounds.size()) + (camera ? 1 : 0));
    if (active) {
        seq.sampleRate = active->sampleRate;
        seq.colorSpace = active->colorSpace;
        seq.hdrPeakNits = active->hdrPeakNits;
    }
    Clip picture = makeClip(p, video, TrackKind::Video, seq);
    picture.start = 0;
    const FrameTime length = picture.duration;
    seq.videoTracks[0].name = video.name;
    seq.videoTracks[0].clips.push_back(picture);
    const double f = seq.fpsValue();
    MediaItem item;
    for (size_t i = 0; i < sounds.size(); ++i) {
        const MediaItem sound = *p.findMedia(sounds[i]);
        Clip c = makeClip(p, sound, TrackKind::Audio, seq);
        // Lined up with the picture, then cut to it: sound from before the picture starts is skipped.
        FrameTime at = FrameTime(std::llround(offsets[i] * f));
        if (at < 0) {
            c.sourceIn += -at;
            c.duration -= -at;
            at = 0;
        }
        c.start = at;
        c.duration = std::min(c.duration, length - at);
        if (c.duration > 0) seq.audioTracks[i].clips.push_back(c);
        seq.audioTracks[i].name = sound.name;
        // Logging the recorder knows and the camera does not.
        for (const char* key : {"scene", "take", "tape", "comment", "circled"})
            if (auto it = sound.metadata.find(key); it != sound.metadata.end() && !video.metadata.count(key)) item.metadata[key] = it->second;
    }
    if (camera) {
        Track& t = seq.audioTracks[sounds.size()];
        Clip c = makeClip(p, video, TrackKind::Audio, seq);
        c.start = 0;
        t.clips.push_back(c);
        t.name = video.name + " (camera)";
        t.muted = true;
    }
    if (std::all_of(seq.audioTracks.begin(), seq.audioTracks.end(), [](const Track& t) { return t.clips.empty() || t.muted; }))
        return fail("The sound does not overlap the picture");
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = name;
    item.sequenceId = seq.id;
    item.hasVideo = true;
    item.hasAudio = true;
    item.width = w;
    item.height = h;
    item.fps = fps;
    item.duration = double(length) / f;
    item.bin = video.bin;
    item.timecode = video.timecode;
    item.created = video.created;
    item.rating = video.rating;
    item.label = video.label;
    item.keywords = video.keywords;
    for (const auto& [k, v] : video.metadata) item.metadata[k] = v;
    item.metadata["merged"] = "Yes";
    p.sequences.push_back(std::move(seq));
    p.media.push_back(item);
    return item.id;
}

bool timecodeOffset(const Project& p, Id video, Id sound, double& offset) {
    const MediaItem* v = p.findMedia(video);
    const MediaItem* a = p.findMedia(sound);
    if (!v || !a || v->timecode < 0 || a->timecode < 0) return false;
    // Across midnight a day's timecode wraps; the nearer reading wins.
    double d = a->timecode - v->timecode;
    if (d > 43200) d -= 86400;
    if (d < -43200) d += 86400;
    const double overlap = std::min(v->duration, d + a->duration) - std::max(0.0, d);
    if (overlap <= 0) return false;
    offset = d;
    return true;
}

std::vector<SoundMatch> matchByTimecode(const Project& p, const std::vector<Id>& videos, const std::vector<Id>& sounds) {
    std::vector<SoundMatch> out;
    for (Id v : videos) {
        const MediaItem* vm = p.findMedia(v);
        if (!vm) continue;
        SoundMatch best;
        double most = 0;
        for (Id a : sounds) {
            double d = 0;
            if (!timecodeOffset(p, v, a, d)) continue;
            const double overlap = std::min(vm->duration, d + p.findMedia(a)->duration) - std::max(0.0, d);
            if (overlap > most) most = overlap, best = {v, a, d};
        }
        if (best.sound) out.push_back(best);
    }
    return out;
}

bool isMergedClip(const Project& p, Id media) {
    const MediaItem* m = p.findMedia(media);
    return m && m->kind == MediaKind::Sequence && m->metadata.count("merged");
}

}  // namespace montage
