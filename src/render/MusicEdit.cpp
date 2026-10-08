#include "MusicEdit.h"

#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "core/MediaLog.h"

namespace montage {

namespace {

constexpr int kAnalysisRate = 22050;

std::mutex gBeatsMutex;
std::map<std::string, BeatGrid> gBeats;  // by file

const char* const kBarPrefix = "Bar ";

// A beat's place on the timeline for this clip (or -1 if the clip does not play it).
FrameTime timelineFrame(const Sequence& s, const Clip& c, double seconds) {
    const double src = seconds * s.fpsValue();  // sequence frames into the media
    if (c.reverse || c.speed <= 0) return -1;
    const double t = double(c.start) + (src - c.sourceIn) / c.speed;
    if (t < double(c.start) - 1e-6 || t >= double(c.end())) return -1;
    return FrameTime(std::llround(t));
}

}  // namespace

edit::Result cutToBeat(Project& p, Sequence& s, const Clip& music, const BeatGrid& g, const std::vector<Id>& media, int every, bool bars,
                       int videoTrack) {
    if (media.empty()) return edit::Result::fail("Choose the clips to cut to the music");
    if (g.empty()) return edit::Result::fail("No beat was found in the music");
    every = std::max(1, every);
    // Where the cuts fall on the timeline.
    std::vector<FrameTime> cuts;
    const std::vector<double>& marks = bars && !g.downbeats.empty() ? g.downbeats : g.beats;
    for (size_t i = 0; i < marks.size(); ++i) {
        const FrameTime t = timelineFrame(s, music, marks[i]);
        if (t >= 0 && (cuts.empty() || t > cuts.back())) cuts.push_back(t);
    }
    std::vector<FrameTime> at;
    for (size_t i = 0; i < cuts.size(); i += size_t(every)) at.push_back(cuts[i]);
    if (at.empty()) return edit::Result::fail("The music clip plays none of its beats");
    // The last piece runs on to the end of the music, if that is at least half a piece.
    const FrameTime end = music.end();
    const FrameTime typical = at.size() > 1 ? (at.back() - at.front()) / FrameTime(at.size() - 1) : end - at.front();
    if (end - at.back() >= std::max<FrameTime>(1, typical / 2)) at.push_back(end);
    while (int(s.videoTracks.size()) <= videoTrack) edit::addTrack(p, s, TrackKind::Video);
    edit::Result all;
    all.ok = true;
    size_t next = 0;
    for (size_t i = 0; i + 1 < at.size(); ++i) {
        const FrameTime len = at[i + 1] - at[i];
        // The next clip long enough for this piece (stills always are).
        const MediaItem* m = nullptr;
        for (size_t tries = 0; tries < media.size() && !m; ++tries) {
            const MediaItem* cand = p.findMedia(media[next % media.size()]);
            ++next;
            if (!cand) continue;
            const bool still = cand->kind == MediaKind::Image;
            if (still || cand->duration * s.fpsValue() >= double(len)) m = cand;
        }
        if (!m) continue;
        const double frames = m->kind == MediaKind::Image ? double(len) : m->duration * s.fpsValue();
        const double in = m->kind == MediaKind::Image ? 0.0 : std::floor((frames - double(len)) / 2);
        const edit::Result r = edit::placeMedia(p, s, m->id, at[i], in, in + double(len), {TrackKind::Video, videoTrack},
                                                {TrackKind::Audio, -1}, false);
        if (!r.ok) return r;
        all.created.insert(all.created.end(), r.created.begin(), r.created.end());
    }
    if (all.created.empty()) return edit::Result::fail("None of the clips is long enough for a piece");
    return all;
}

bool mediaBeats(const Project& p, Id mediaId, BeatGrid& out, const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* m = p.findMedia(mediaId);
    if (!m || !m->hasAudio || m->path.empty()) {
        if (error) *error = "The clip has no sound to find a beat in";
        return false;
    }
    {
        std::lock_guard lock(gBeatsMutex);
        if (auto it = gBeats.find(m->path); it != gBeats.end()) {
            out = it->second;
            return true;
        }
    }
    std::string err;
    BeatGrid g = detectBeatsInFile(m->path, cancel, &err);
    if (g.empty()) {
        if (error) *error = err.empty() ? "No beat was found" : err;
        return false;
    }
    std::lock_guard lock(gBeatsMutex);
    gBeats[m->path] = g;
    out = g;
    return true;
}

int addBeatMarkers(Sequence& s, const Clip& c, const BeatGrid& g, bool everyBeat) {
    // Beat markers made before for this clip's range go first.
    auto ours = [&](const Marker& mk) {
        if (mk.t < c.start || mk.t >= c.end()) return false;
        const QString n = QString::fromStdString(mk.name);
        static const QRegularExpression beatName(QStringLiteral("^\\d+\\.\\d+$"));
        return n.startsWith(QLatin1String(kBarPrefix)) || beatName.match(n).hasMatch();
    };
    s.markers.erase(std::remove_if(s.markers.begin(), s.markers.end(), ours), s.markers.end());
    const int colour = std::max(0, labelFromName("Yellow"));
    int added = 0, bar = 0, beatInBar = 0;
    size_t next = 0;
    for (double b : g.beats) {
        // Bars count from the first downbeat; beats before it are a pickup.
        if (next < g.downbeats.size() && std::fabs(g.downbeats[next] - b) < 1e-6) {
            ++bar;
            beatInBar = 1;
            ++next;
        } else {
            ++beatInBar;
        }
        if (bar == 0 || (!everyBeat && beatInBar != 1)) continue;
        const FrameTime t = timelineFrame(s, c, b);
        if (t < 0) continue;
        Marker mk;
        mk.t = t;
        mk.name = everyBeat ? std::to_string(bar) + "." + std::to_string(beatInBar) : kBarPrefix + std::to_string(bar);
        mk.color = colour;
        s.markers.push_back(mk);
        ++added;
    }
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return added;
}

bool analyzeMusicFit(const Project& p, const Sequence& s, const Clip& c, FrameTime target, MusicFit& fit,
                     const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->path.empty()) {
        if (error) *error = "Select a music clip";
        return false;
    }
    if (std::fabs(c.speed - 1) > 1e-9 || c.reverse) {
        if (error) *error = "Fit Music works on clips at normal speed";
        return false;
    }
    std::vector<float> all;
    if (!decodeMono(m->path, kAnalysisRate, all, cancel, error)) return false;
    // The part of the music the clip uses now.
    const double fps = s.fpsValue();
    const double in = c.sourceIn / fps, out = (c.sourceIn + double(c.duration)) / fps;
    const size_t a = std::min(all.size(), size_t(std::max(0.0, in) * kAnalysisRate));
    const size_t z = std::min(all.size(), size_t(std::max(0.0, out) * kAnalysisRate));
    if (z <= a + size_t(kAnalysisRate) * 4) {
        if (error) *error = "The clip is too short to re-edit";
        return false;
    }
    const std::vector<float> part(all.begin() + long(a), all.begin() + long(z));
    const BeatGrid g = detectBeats(part, kAnalysisRate, cancel);
    if (g.downbeats.size() < 4) {
        if (error) *error = "No steady beat was found in the clip";
        return false;
    }
    fit = fitMusic(part, kAnalysisRate, g, double(target) / fps, {}, cancel);
    if (!fit.ok()) {
        if (error) *error = "The music could not be fitted to that length";
        return false;
    }
    const double offset = double(a) / kAnalysisRate;
    for (MusicSegment& seg : fit.segments) seg.in += offset, seg.out += offset;
    return true;
}

edit::Result applyMusicFit(Project& p, Sequence& s, Id clipId, const MusicFit& fit, double crossfadeSeconds) {
    const auto loc = edit::locate(s, clipId);
    if (!loc) return edit::Result::fail("Unknown clip");
    if (loc->track.kind != TrackKind::Audio) return edit::Result::fail("Fit Music works on audio clips");
    if (edit::linkedClips(s, clipId).size() > 1) return edit::Result::fail("Unlink the clip from its picture first");
    if (!fit.ok()) return edit::Result::fail("Nothing to apply");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return edit::Result::fail("Track is locked");
    const Clip original = t->clips[loc->index];
    const double fps = s.fpsValue();
    const FrameTime limit = edit::sourceLimit(p, s, original);
    // Remove it, with its transitions, then lay the pieces in from its start.
    if (auto r = edit::removeClips(p, s, {clipId}, false); !r.ok) return r;
    edit::Result res;
    FrameTime at = original.start;
    double carry = 0;  // frames the cuts run past the segment ends (|carry| <= 0.5)
    for (size_t k = 0; k < fit.segments.size(); ++k) {
        const MusicSegment& seg = fit.segments[k];
        Clip piece = original;
        piece.id = p.newId();
        piece.linkGroup = 0;
        piece.start = at;
        piece.sourceIn = std::max(0.0, seg.in * fps + carry);
        FrameTime len = std::max<FrameTime>(1, FrameTime(std::llround((seg.out - seg.in) * fps - carry)));
        if (double(limit) < 1e15) len = std::min<FrameTime>(len, std::max<FrameTime>(1, FrameTime(std::floor(double(limit) - piece.sourceIn))));
        piece.duration = len;
        carry = piece.sourceIn + double(len) - seg.out * fps;
        const edit::Result r = edit::overwrite(p, s, loc->track, piece);
        if (!r.ok) return r;
        res.created.push_back(piece.id);
        at += len;
    }
    // Short equal-power crossfades over the joins (the media runs on past each cut).
    const FrameTime xf = std::max<FrameTime>(2, FrameTime(std::lround(crossfadeSeconds * fps)));
    for (size_t k = 0; k + 1 < res.created.size(); ++k) edit::addTransition(p, s, res.created[k], edit::Edge::Out, "crossfade", xf);
    res.applied = at - original.end();
    return res;
}

}  // namespace montage
