#include "Interpret.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "CameraRaw.h"
#include "Decoder.h"
#include "ImageSequence.h"

namespace montage {

Rational fileFrameRate(const MediaItem& m) {
    if (m.kind != MediaKind::Video) return Rational{0, 1};
    const Interpretation now = interpretationOf(m);
    return now.conformed() ? now.fileFps : m.fps;
}

bool isRawMedia(const MediaItem& m) {
    const std::string file = uninterpretedPath(m.path);
    if (ImageSequence seq; parseImageSequencePath(file, seq)) return isRawPath(seq.pattern);
    return isRawPath(file);
}

namespace edit {

namespace {

// Every media time in `t` multiplied by `ratio`.
std::shared_ptr<const Transcript> retimed(const Transcript& t, double ratio) {
    auto out = std::make_shared<Transcript>(t);
    for (TranscriptSegment& g : out->segments) {
        g.start *= ratio, g.end *= ratio;
        for (TranscriptWord& w : g.words) w.start *= ratio, w.end *= ratio;
    }
    return out;
}

}  // namespace

Result interpretFootage(Project& p, Id media, Interpretation i) {
    MediaItem* m = p.findMedia(media);
    if (m && m->subclipOf) {
        media = m->subclipOf;  // a subclip is read as its media is
        m = p.findMedia(media);
    }
    if (!m || m->path.empty() || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || !m->hasVideo)
        return Result::fail("Interpret Footage is for videos and stills");
    if (i.fps.valid() && (i.fps.toDouble() < 1 || i.fps.toDouble() > 1000)) return Result::fail("The frame rate must be 1 to 1000");
    if (i.par != 0 && (i.par < 0.1 || i.par > 10)) return Result::fail("The pixel aspect must be 0.1 to 10");
    if (!validAlphaMode(i.alpha)) return Result::fail("Alpha is straight, premultiplied, ignore or invert");
    if (!validFieldOrder(i.fields)) return Result::fail("Fields are progressive, upper or lower");
    if (i.alpha == "straight") i.alpha.clear();
    if (!validRawHighlights(i.rawHighlights)) return Result::fail("Highlights are clip, blend or rebuild");
    if (i.rawHighlights == "clip") i.rawHighlights.clear();
    if (i.hasRaw() && !isRawMedia(*m)) return Result::fail("Camera RAW settings are for camera RAW stills and CinemaDNG");
    if (i.rawExposure < -5 || i.rawExposure > 5) return Result::fail("Exposure must be -5 to +5 stops");
    if (i.rawTemperature != 0 && (i.rawTemperature < 2000 || i.rawTemperature > 25000)) return Result::fail("The temperature must be 2000 to 25000 K");
    if (i.rawTint < -150 || i.rawTint > 150) return Result::fail("The tint must be -150 to 150");
    if (!validStereoLayout(i.stereo)) return Result::fail("Stereo 3D is none, sbs, sbs_half, tb or tb_half");
    i.eye = 0;  // which eye is read is the renderer's choice, never the media's
    const bool still = m->kind == MediaKind::Image;
    if (still) i.fps = Rational{0, 1}, i.fields.clear(), i.keepPitch = false, i.stereo.clear(), i.swapEyes = false;
    // An image sequence's rate is part of its own path.
    bool changed = false;
    if (ImageSequence seq; parseImageSequencePath(m->path, seq)) {
        if (i.fps.valid() && !(i.fps == seq.fps)) {
            const Result r = setImageSequenceRate(p, media, i.fps);
            if (!r.ok) return r;
            m = p.findMedia(media);
            changed = true;
        }
        i.fps = Rational{0, 1};
    }
    i.fileFps = fileFrameRate(*m);
    if (!i.conformed()) i.fps = i.fileFps = Rational{0, 1}, i.keepPitch = false;
    const std::string path = interpretedPath(m->path, i);
    if (path == m->path) return changed ? Result{} : Result::fail("");
    MediaItem probe;
    std::string err;
    if (!probeMedia(path, probe, &err)) return Result::fail(err.empty() ? "It cannot be read that way" : err);
    // New media seconds per old one, so everything keeps to the same frame.
    const double ratio = !still && m->fps.valid() && probe.fps.valid() ? m->fps.toDouble() / probe.fps.toDouble() : 1.0;
    m->path = path;
    m->proxyPath.clear();
    m->width = probe.width;
    m->height = probe.height;
    m->stereo = probe.stereo;
    if (!still) {
        m->fps = probe.fps;
        m->duration = probe.duration;
        if (m->timecode >= 0) m->timecode *= ratio;
    }
    if (ratio != 1.0) {
        for (Sequence& s : p.sequences)
            for (TrackRef r : allTracks(s))
                for (Clip& c : trackAt(s, r)->clips) {
                    if (c.mediaId != media) continue;
                    c.sourceIn *= ratio;
                    for (Marker& mk : c.markers) {
                        mk.t = FrameTime(std::llround(double(mk.t) * ratio));
                        mk.duration = FrameTime(std::llround(double(mk.duration) * ratio));
                    }
                    const double available = m->duration * s.fpsValue() - c.sourceIn;
                    if (c.sourceExtent() > available) c.duration = std::max<FrameTime>(1, FrameTime(std::floor(available / c.speed)));
                }
        if (m->transcript) m->transcript = retimed(*m->transcript, ratio);
        if (m->visual) {
            auto v = std::make_shared<VisualIndex>(*m->visual);
            v->step *= ratio;
            for (VisualIndex::Sample& smp : v->samples) smp.time *= ratio;
            m->visual = v;
        }
        if (m->faces) {
            auto f = std::make_shared<FaceIndex>(*m->faces);
            f->step *= ratio;
            for (FaceIndex::Face& face : f->faces) face.time *= ratio;
            m->faces = f;
        }
    }
    // Its subclips are read the same way, over the same frames.
    for (MediaItem& sub : p.media) {
        if (sub.subclipOf != media) continue;
        sub.subclipIn *= ratio, sub.subclipOut *= ratio;
        sub.path = m->path;
        sub.proxyPath.clear();
        sub.width = m->width;
        sub.height = m->height;
        sub.stereo = m->stereo;
        if (!still) {
            sub.fps = m->fps;
            sub.duration = sub.subclipOut - sub.subclipIn;
            if (m->timecode >= 0) sub.timecode = m->timecode + sub.subclipIn;
        }
    }
    return {};
}

}  // namespace edit

}  // namespace montage
