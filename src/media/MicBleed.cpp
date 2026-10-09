#include "MicBleed.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Decoder.h"
#include "MediaPool.h"

namespace montage {

namespace {

constexpr int kRate = 16000;
constexpr double kWindow = 0.02;  // seconds per level
constexpr double kSilent = -120;

}  // namespace

std::vector<Spans> bleedSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const BleedOptions& o,
                              std::string* error, const std::atomic<bool>* cancel) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<Spans>{};
    };
    if (tracks.size() < 2) return fail("Mic bleed is removed across two tracks or more, a mic each");
    const double fps = s.fpsValue();
    const size_t windows = size_t(std::ceil(double(s.duration()) / fps / kWindow));
    if (windows == 0) return fail("The sequence is empty");
    // Each track's level, 20 ms at a time (silent where it has no clip).
    std::vector<std::vector<double>> level(tracks.size(), std::vector<double>(windows, kSilent));
    std::vector<std::vector<bool>> covered(tracks.size(), std::vector<bool>(windows, false));
    std::map<std::string, AudioBufferPtr> decoded;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        if (tracks[ti] < 0 || tracks[ti] >= int(s.audioTracks.size())) return fail("No such audio track");
        for (const Clip& c : s.audioTracks[size_t(tracks[ti])].clips) {
            const MediaItem* m = c.enabled ? p.findMedia(c.mediaId) : nullptr;
            if (!m || !m->hasAudio || m->path.empty()) continue;
            AudioBufferPtr& buf = decoded[audioKey(m->path, c.channels)];
            std::string err;
            if (!buf) buf = decodeAudio(m->path, kRate, &err, cancel, c.channels);
            if (cancel && cancel->load()) return fail("Cancelled");
            if (!buf) return fail("Cannot read the sound of " + m->name + (err.empty() ? "" : ": " + err));
            const double gain = std::pow(10.0, c.audio.p("gain_db", 0) / 20.0);
            const int64_t n = buf->frames();
            const double cs = double(c.start) / fps, ce = double(c.end()) / fps;
            for (size_t w = size_t(std::max(0.0, cs / kWindow)); w < windows && double(w) * kWindow < ce; ++w) {
                const double t0 = double(w) * kWindow;
                const double local = (t0 - cs) * fps;
                if (local < 0) continue;
                const double a = c.sourceAt(local) / fps, b = c.sourceAt(std::min(local + kWindow * fps, double(c.duration))) / fps;
                const int64_t s0 = int64_t(std::min(a, b) * kRate), s1 = std::max(s0 + 1, int64_t(std::max(a, b) * kRate));
                double sum = 0;
                int64_t count = 0;
                for (int64_t k = std::max<int64_t>(0, s0); k < std::min(s1, n); ++k, ++count) {
                    const double v = 0.5 * (buf->samples[size_t(k) * 2] + buf->samples[size_t(k) * 2 + 1]) * gain;
                    sum += v * v;
                }
                if (count) level[ti][w] = std::max(level[ti][w], 10 * std::log10(sum / double(count) + 1e-12));
                covered[ti][w] = true;
            }
        }
    }
    // Each mic's own floor: its quiet tenth, plus 6 dB, and never under the absolute floor.
    std::vector<double> floor(tracks.size(), o.floorDb);
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        std::vector<double> mine;
        for (size_t w = 0; w < windows; ++w)
            if (covered[ti][w]) mine.push_back(level[ti][w]);
        if (mine.empty()) continue;
        std::nth_element(mine.begin(), mine.begin() + long(mine.size() / 10), mine.end());
        floor[ti] = std::max(o.floorDb, mine[mine.size() / 10] + 6);
    }
    // Speaking: above its floor and within the margin of the loudest mic; held open a moment after.
    std::vector<Spans> out(tracks.size());
    const size_t hold = size_t(std::lround(o.hold / kWindow));
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        std::vector<bool> open(windows, false);
        for (size_t w = 0; w < windows; ++w) {
            double loudest = kSilent;
            for (size_t tj = 0; tj < tracks.size(); ++tj) loudest = std::max(loudest, level[tj][w]);
            if (level[ti][w] > floor[ti] && level[ti][w] >= loudest - o.marginDb)
                for (size_t k = w; k < std::min(windows, w + 1 + hold); ++k) open[k] = true;
        }
        // The quiet stretches where it has a clip, long enough to dip.
        size_t w = 0;
        while (w < windows) {
            if (!covered[ti][w] || open[w]) {
                ++w;
                continue;
            }
            size_t e = w;
            while (e < windows && covered[ti][e] && !open[e]) ++e;
            const double a = double(w) * kWindow, b = double(e) * kWindow;
            if (b - a >= o.minDip) out[ti].emplace_back(a, b);
            w = e;
        }
    }
    return out;
}

int removeMicBleed(Sequence& s, const std::vector<int>& tracks, const std::vector<Spans>& dips, const BleedOptions& o) {
    DuckOptions d;
    d.amountDb = o.reductionDb;
    d.fadeDown = o.fadeDown;
    d.fadeUp = o.fadeUp;
    int changed = 0;
    for (size_t ti = 0; ti < tracks.size() && ti < dips.size(); ++ti) {
        if (tracks[ti] < 0 || tracks[ti] >= int(s.audioTracks.size())) continue;
        // The fades sit inside each quiet stretch: down after the speaker stops, up just before they start.
        Spans inside;
        for (const auto& [a, b] : dips[ti])
            if (b - o.fadeUp > a + o.fadeDown) inside.emplace_back(a + o.fadeDown, b - o.fadeUp);
        for (Clip& c : s.audioTracks[size_t(tracks[ti])].clips)
            if (duckClip(c, s, inside, d)) ++changed;
    }
    return changed;
}

}  // namespace montage
