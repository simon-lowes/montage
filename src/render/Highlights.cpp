#include "Highlights.h"

#include <algorithm>
#include <cmath>

#include "core/Transcript.h"
#include "core/VisualIndex.h"
#include "media/Decoder.h"

namespace montage {

namespace {

constexpr double kStep = kHighlightStep;  // seconds per score

bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

// Each window's loudness above the clip's median, 0..1 (20 dB above is 1).
std::vector<double> soundScores(const std::string& path, size_t windows) {
    std::vector<double> out(windows, 0.0);
    AudioBufferPtr a = decodeAudio(path, 16000);
    if (!a || a->samples.empty()) return out;
    std::vector<double> db(windows, -120.0);
    const size_t per = size_t(kStep * a->sampleRate);
    for (size_t w = 0; w < windows; ++w) {
        double acc = 0;
        size_t n = 0;
        for (size_t i = w * per; i < (w + 1) * per && i < size_t(a->frames()); ++i, ++n)
            acc += double(a->samples[i * 2]) * a->samples[i * 2] + double(a->samples[i * 2 + 1]) * a->samples[i * 2 + 1];
        if (n) db[w] = 10 * std::log10(acc / (2.0 * n) + 1e-12);
    }
    std::vector<double> sorted = db;
    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    for (size_t w = 0; w < windows; ++w) out[w] = std::clamp((db[w] - median) / 20, 0.0, 1.0);
    return out;
}

// How much changes in the picture over each window (small frames, twice a window), 0..1 against the clip's busiest.
std::vector<double> motionScores(const std::string& path, size_t windows, const std::atomic<bool>* cancel) {
    std::vector<double> out(windows, 0.0);
    VideoDecoder dec;
    if (!dec.open(path)) return out;
    std::vector<float> prev;
    std::vector<double> diff(windows * 2, 0.0);
    for (size_t k = 0; k < windows * 2; ++k) {
        if ((k & 15) == 0 && cancelled(cancel)) return out;
        const double t = (double(k) + 0.5) * kStep / 2;
        if (t >= dec.duration()) break;
        Frame16Ptr f = dec.frameAt(t, 64, 36);
        if (!f) continue;
        const Image img = toImage(*f);
        std::vector<float> lum(size_t(img.width) * size_t(img.height));
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                const float* p = img.at(x, y);
                lum[size_t(y) * size_t(img.width) + size_t(x)] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            }
        if (prev.size() == lum.size()) {
            double d = 0;
            for (size_t i = 0; i < lum.size(); ++i) d += std::fabs(lum[i] - prev[i]);
            diff[k] = d / double(lum.size());
        }
        prev = std::move(lum);
    }
    for (size_t w = 0; w < windows; ++w) out[w] = std::max(diff[w * 2], diff[w * 2 + 1]);
    // A cut is one huge jump in a single step; it is not liveliness. Clip each to a little above the 90th percentile.
    std::vector<double> sorted = out;
    std::sort(sorted.begin(), sorted.end());
    const double ref = std::max(1e-4, sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.9))]);
    for (double& v : out) v = std::min(1.0, v / (ref * 1.25));
    return out;
}

// Where a cut at `t` should go: off any word it would split (to the nearer edge, within half a second).
double offWords(const Transcript* tr, double t, bool start) {
    if (!tr) return t;
    for (const auto& seg : tr->segments)
        for (const auto& w : seg.words)
            if (t > w.start && t < w.end) {
                const double to = start ? w.start : w.end;
                return std::fabs(to - t) <= 0.5 ? to : t;
            }
    return t;
}

}  // namespace

std::vector<double> highlightCurve(const Project& p, const MediaItem& m, const HighlightOptions& o, const std::atomic<bool>* cancel) {
    if (m.kind != MediaKind::Video || m.duration <= 0) return {};
    const size_t windows = size_t(std::ceil(m.duration / kStep));
    const std::vector<double> sound = m.hasAudio ? soundScores(m.path, windows) : std::vector<double>(windows, 0.0);
    const std::vector<double> motion = motionScores(m.path, windows, cancel);
    std::vector<double> look(windows, 0.0);
    if (!o.lookFor.empty()) {
        for (const ShotMatch& sm : findShots(p, o.lookFor, 50))
            if (sm.media == m.id)
                for (size_t w = size_t(std::max(0.0, sm.start / kStep)); w < windows && double(w) * kStep <= sm.end; ++w)
                    look[w] = std::max(look[w], double(sm.score));
        // Relative: the best match is 1, a poor one near 0.
        const double hi = *std::max_element(look.begin(), look.end());
        for (double& v : look) v = hi > 0 ? std::clamp((v - hi * 0.8) / (hi * 0.2), 0.0, 1.0) : 0.0;
    }
    std::vector<double> score(windows);
    for (size_t w = 0; w < windows; ++w) score[w] = o.soundWeight * sound[w] + o.motionWeight * motion[w] + o.lookWeight * look[w];
    // Smoothed over a second either side, so a moment is a stretch, not a spike.
    std::vector<double> smooth(windows, 0.0);
    for (size_t w = 0; w < windows; ++w) {
        double acc = 0, n = 0;
        for (int d = -2; d <= 2; ++d) {
            const long j = long(w) + d;
            if (j < 0 || j >= long(windows)) continue;
            const double wt = 3 - std::abs(d);
            acc += score[size_t(j)] * wt, n += wt;
        }
        smooth[w] = acc / n;
    }
    return smooth;
}

std::vector<HighlightMoment> findHighlights(const Project& p, const std::vector<Id>& media, const HighlightOptions& o,
                                            const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                                            std::string* error) {
    struct Scored {
        Id media;
        std::vector<double> score;
        double duration;
    };
    std::vector<Scored> all;
    for (size_t i = 0; i < media.size(); ++i) {
        if (cancelled(cancel)) return {};
        const MediaItem* m = p.findMedia(media[i]);
        if (!m || m->kind != MediaKind::Video || m->duration < o.minLength) continue;
        std::vector<double> curve = highlightCurve(p, *m, o, cancel);
        if (curve.empty()) continue;
        all.push_back({m->id, std::move(curve), m->duration});
        if (progress) progress(double(i + 1) / double(media.size()));
    }
    if (all.empty()) {
        if (error) *error = "None of the clips is a video long enough for a highlight";
        return {};
    }
    // Peaks, best first, each a few seconds round its window (longer for a stronger, wider peak).
    std::vector<HighlightMoment> picked;
    double total = 0;
    std::vector<std::vector<bool>> used;
    for (const Scored& s : all) used.emplace_back(s.score.size(), false);
    while (total < o.seconds - 0.25) {
        double best = 0;
        size_t bi = 0, bw = 0;
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t w = 0; w < all[i].score.size(); ++w)
                if (!used[i][w] && all[i].score[w] > best) best = all[i].score[w], bi = i, bw = w;
        if (best <= 0.02) break;
        const Scored& s = all[bi];
        // Grow out from the peak while the score stays above half of it.
        size_t lo = bw, hi = bw;
        const double want = std::min(o.maxLength, std::max(o.minLength, o.seconds - total));
        while (double(hi - lo + 1) * kStep < want) {
            const bool canLo = lo > 0 && !used[bi][lo - 1], canHi = hi + 1 < s.score.size() && !used[bi][hi + 1];
            if (!canLo && !canHi) break;
            const double sl = canLo ? s.score[lo - 1] : -1, sh = canHi ? s.score[hi + 1] : -1;
            if (std::max(sl, sh) < best * 0.5 && double(hi - lo + 1) * kStep >= o.minLength) break;
            if (sh >= sl) ++hi;
            else --lo;
        }
        for (size_t w = lo; w <= hi; ++w) used[bi][w] = true;
        // Keep a little air between moments.
        if (lo > 0) used[bi][lo - 1] = true;
        if (hi + 1 < s.score.size()) used[bi][hi + 1] = true;
        HighlightMoment m;
        m.media = s.media;
        m.in = double(lo) * kStep;
        m.out = std::min(s.duration, double(hi + 1) * kStep);
        if (m.out - m.in < o.minLength) continue;
        const MediaItem* mi = p.findMedia(s.media);
        const Transcript* tr = mi && mi->transcript ? mi->transcript.get() : nullptr;
        m.in = offWords(tr, m.in, true);
        m.out = offWords(tr, m.out, false);
        m.score = best;
        picked.push_back(m);
        total += m.out - m.in;
    }
    // In the order they happened.
    auto order = [&](Id id) { return size_t(std::find(media.begin(), media.end(), id) - media.begin()); };
    std::sort(picked.begin(), picked.end(), [&](const HighlightMoment& a, const HighlightMoment& b) {
        return order(a.media) != order(b.media) ? order(a.media) < order(b.media) : a.in < b.in;
    });
    if (picked.empty() && error) *error = "Nothing stood out: the footage is quiet and still throughout";
    return picked;
}

edit::Result layoutHighlights(Project& p, Sequence& s, const std::vector<HighlightMoment>& moments, FrameTime at, int videoTrack,
                              int audioTrack) {
    edit::Result all;
    all.ok = true;
    const double fps = s.fpsValue();
    while (int(s.videoTracks.size()) <= videoTrack) edit::addTrack(p, s, TrackKind::Video);
    while (int(s.audioTracks.size()) <= audioTrack) edit::addTrack(p, s, TrackKind::Audio);
    for (const HighlightMoment& m : moments) {
        const double in = std::round(m.in * fps), out = std::round(m.out * fps);
        if (out <= in) continue;
        const edit::Result r = edit::placeMedia(p, s, m.media, at, in, out, {TrackKind::Video, videoTrack}, {TrackKind::Audio, audioTrack}, false);
        if (!r.ok) return r;
        all.created.insert(all.created.end(), r.created.begin(), r.created.end());
        at += FrameTime(out - in);
    }
    if (all.created.empty()) return edit::Result::fail("No highlights to lay out");
    return all;
}

Id makeHighlightSequence(Project& p, const std::vector<HighlightMoment>& moments, const std::string& name) {
    int w = 1920, h = 1080;
    Rational fps{30, 1};
    const Sequence* like = p.active();
    if (like) {
        w = like->width, h = like->height, fps = like->fps;
    } else if (!moments.empty()) {
        if (const MediaItem* m = p.findMedia(moments.front().media); m && m->width > 0) {
            w = m->width, h = m->height;
            if (m->fps.valid()) fps = m->fps;
        }
    }
    Sequence s = makeSequence(p, name, w, h, fps, 1, 1);
    if (like) {
        s.sampleRate = like->sampleRate;
        s.colorSpace = like->colorSpace;
        s.hdrPeakNits = like->hdrPeakNits;
    }
    if (!layoutHighlights(p, s, moments).ok) return 0;
    MediaItem item;
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = s.name;
    item.sequenceId = s.id;
    item.hasVideo = item.hasAudio = true;
    item.width = s.width;
    item.height = s.height;
    item.fps = s.fps;
    const Id id = s.id;
    p.sequences.push_back(std::move(s));
    p.media.push_back(std::move(item));
    return id;
}

}  // namespace montage
