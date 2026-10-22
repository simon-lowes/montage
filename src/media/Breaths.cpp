#include "Breaths.h"

#include <algorithm>
#include <cmath>

namespace montage {

TimeSpans findBreaths(const AudioBuffer& audio, const Transcript* transcript, double from, double to, const std::atomic<bool>* cancel) {
    TimeSpans out;
    const int nch = std::max(1, audio.channels);
    const int rate = audio.sampleRate;
    if (rate < 8000) return out;
    const int64_t first = std::clamp<int64_t>(int64_t(std::floor(from * rate)), 0, audio.frames());
    const int64_t last = to < 0 ? audio.frames() : std::clamp<int64_t>(int64_t(std::ceil(to * rate)), first, audio.frames());
    const int64_t n = last - first;
    const int64_t hop = rate / 100, win = rate * 3 / 100;  // 10 ms frames, 30 ms windows
    if (n < win * 4) return out;
    std::vector<float> x(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double v = 0;
        for (int c = 0; c < std::min(nch, 2); ++c) v += audio.samples[size_t(first + i) * size_t(nch) + size_t(c)];
        x[size_t(i)] = float(v / std::min(nch, 2));
    }
    const int64_t frames = (n - win) / hop + 1;
    std::vector<double> level(static_cast<size_t>(frames)), crossings(static_cast<size_t>(frames));
    for (int64_t f = 0; f < frames; ++f) {
        const float* w = x.data() + f * hop;
        double e = 0;
        int64_t zc = 0;
        for (int64_t i = 0; i < win; ++i) {
            e += double(w[i]) * w[i];
            if (i && (w[i] >= 0) != (w[i - 1] >= 0)) ++zc;
        }
        level[size_t(f)] = 10 * std::log10(e / double(win) + 1e-12);
        crossings[size_t(f)] = double(zc) / double(win) * rate / 2;  // the frequency the crossings would make, Hz
    }
    // The room: the 10th percentile of the levels.
    std::vector<double> all = level;
    std::nth_element(all.begin(), all.begin() + long(all.size() / 10), all.end());
    const double room = all[all.size() / 10];
    // Voiced: the window repeats itself at a pitch period (normalised autocorrelation over 0.6), judged at about
    // 16 kHz (a pitch needs no more), and only where the sound is over the room (quieter can be neither).
    const int d = std::max(1, rate / 16000);
    std::vector<float> y(static_cast<size_t>(n / d));
    for (size_t i = 0; i < y.size(); ++i) {
        double v = 0;
        for (int j = 0; j < d; ++j) v += x[i * size_t(d) + size_t(j)];
        y[i] = float(v / d);
    }
    const double rate2 = double(rate) / d;
    const int minLag = int(rate2 / 400), maxLag = int(rate2 / 80);
    const int64_t win2 = win / d, m = win2 - maxLag;
    std::vector<char> voiced(static_cast<size_t>(frames), 0);
    std::vector<double> speech;
    for (int64_t f = 0; f < frames; ++f) {
        if ((f & 255) == 0 && cancel && cancel->load()) return {};
        if (level[size_t(f)] <= room + 6 || f * hop / d + win2 > int64_t(y.size())) continue;
        const float* w = y.data() + f * hop / d;
        double best = 0;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            double xy = 0, xx = 0, yy = 0;
            for (int64_t i = 0; i < m; ++i) xy += double(w[i]) * w[i + lag], xx += double(w[i]) * w[i], yy += double(w[i + lag]) * w[i + lag];
            if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
        }
        if ((voiced[size_t(f)] = best > 0.6)) speech.push_back(level[size_t(f)]);
    }
    // The speech: the 95th percentile of the voiced frames.
    if (speech.empty()) return out;
    std::nth_element(speech.begin(), speech.begin() + long(speech.size() * 95 / 100), speech.end());
    const double loud = speech[speech.size() * 95 / 100];
    // Inside a word (with a little either side): never a breath.
    const double t0 = double(first) / rate;
    std::vector<char> inWord(static_cast<size_t>(frames), 0);
    if (transcript)
        for (const auto& seg : transcript->segments)
            for (const auto& w : seg.words) {
                const int64_t a = std::max<int64_t>(0, int64_t(std::floor((w.start - 0.04 - t0) * 100)));
                const int64_t b = std::min<int64_t>(frames, int64_t(std::ceil((w.end + 0.04 - t0) * 100)));
                for (int64_t f = a; f < b; ++f) inWord[size_t(f)] = 1;
            }
    const bool words = transcript && !transcript->empty();
    auto breathy = [&](int64_t f) {
        return !voiced[size_t(f)] && !inWord[size_t(f)] && level[size_t(f)] > room + 6 && level[size_t(f)] < loud - 10 &&
               crossings[size_t(f)] < 3000;
    };
    const double shortest = words ? 0.12 : 0.15, longest = 0.9;
    for (int64_t f = 0; f < frames;) {
        if (!breathy(f)) {
            ++f;
            continue;
        }
        // A run, a single stray frame inside it allowed.
        int64_t g = f + 1;
        while (g < frames && (breathy(g) || (g + 1 < frames && breathy(g + 1) && !voiced[size_t(g)]))) ++g;
        const double a = double(f * hop + win / 2 - hop / 2) / rate, b = double((g - 1) * hop + win / 2 + hop / 2) / rate;
        if (b - a >= shortest && b - a <= longest) out.push_back({t0 + a, t0 + b});
        f = g;
    }
    return out;
}

namespace {

// The stretch of media (seconds) clip frame f plays: forwards from where it shows, or backwards to it.
std::pair<double, double> frameSpan(const Clip& c, double f, double fps) {
    const double a = c.sourceAt(f) / fps, b = c.reverse ? a + c.speed / fps : c.sourceAt(f + 1) / fps;
    return {std::min(a, b), std::max(a, b)};
}

// The stretch of media (seconds) a clip plays.
std::pair<double, double> sourceSpan(const Clip& c, double fps) {
    const auto [a0, a1] = frameSpan(c, 0, fps);
    const auto [b0, b1] = frameSpan(c, double(c.duration - 1), fps);
    return {std::min(a0, b0), std::max(a1, b1)};
}

// The clip-local time at which source position x (sequence frames) is heard (a reversed clip's frame begins at the
// top of what it plays).
double localHeard(const Clip& c, double x) { return c.localForSource(x) + (c.reverse ? 1 : 0); }

}  // namespace

TimeSpans breathsInClip(const Project& p, const Sequence& s, const Clip& c, std::string* error, const std::atomic<bool>* cancel,
                        std::map<std::string, AudioBufferPtr>* decoded) {
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || !m->hasAudio) {
        if (error) *error = "The clip has no sound";
        return {};
    }
    std::string key = m->path;
    for (int ch : c.channels) key += "|" + std::to_string(ch);
    AudioBufferPtr audio = decoded && decoded->count(key) ? decoded->at(key) : nullptr;
    if (!audio) audio = decodeAudio(m->path, 48000, error, cancel, c.channels);
    if (!audio) return {};
    if (decoded) (*decoded)[key] = audio;
    // The clip's stretch and a second either side (so a breath at its ends is judged whole).
    const auto [a, b] = sourceSpan(c, s.fpsValue());
    TimeSpans out;
    for (const auto& span : findBreaths(*audio, m->transcript.get(), a - 1, b + 1, cancel))
        if (span.second > a && span.first < b) out.push_back(span);
    return out;
}

bool volumeTakesBreathKeys(const Clip& c) {
    const auto it = c.audio.params.find("gain_db");
    return it == c.audio.params.end() || (it->second.expr.empty() && it->second.repeat == Repeat::Hold);
}

std::pair<double, double> timelineSpan(const Clip& c, const Sequence& s, double from, double to) {
    const double fps = s.fpsValue();
    const auto [a, b] = sourceSpan(c, fps);
    if (to <= a || from >= b) return {-1, -1};
    const double la = localHeard(c, std::max(from, a) * fps), lb = localHeard(c, std::min(to, b) * fps);
    return {(double(c.start) + std::min(la, lb)) / fps, (double(c.start) + std::max(la, lb)) / fps};
}

int lowerBreaths(Clip& c, const Sequence& s, const TimeSpans& breaths, double amountDb) {
    if (breaths.empty() || amountDb >= 0 || c.duration < 3 || !volumeTakesBreathKeys(c)) return 0;
    const double fps = s.fpsValue();
    Param& gain = c.audio.params["gain_db"];
    const Param before = gain;
    // The clip frames each breath covers whole (whatever the clip's speed or direction): those near where the clip
    // plays its ends, checked one by one.
    std::vector<std::pair<FrameTime, FrameTime>> spans;
    for (const auto& [a, b] : breaths) {
        const double la = localHeard(c, a * fps), lb = localHeard(c, b * fps);
        const FrameTime lo = std::max<FrameTime>(0, FrameTime(std::floor(std::min(la, lb))) - 2);
        const FrameTime hi = std::min<FrameTime>(c.duration - 1, FrameTime(std::ceil(std::max(la, lb))) + 2);
        FrameTime first = -1, last = -1;
        for (FrameTime f = lo; f <= hi; ++f) {
            const auto [t1, t2] = frameSpan(c, double(f), fps);
            if (t1 >= a && t2 <= b + 0.5 / fps) {
                if (first < 0) first = f;
                last = f;
            }
        }
        if (first > 0 && last + 1 < c.duration) spans.push_back({first, last});
    }
    // Breaths that touch (a stray voiced frame between them) become one dip, so neither's keys undo the other's.
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size();)
        if (spans[i].first <= spans[i - 1].second + 2) {
            spans[i - 1].second = std::max(spans[i - 1].second, spans[i].second);
            spans.erase(spans.begin() + long(i));
        } else {
            ++i;
        }
    int count = 0;
    for (const auto& [first, last] : spans) {
        const double outside = std::min(before.keyedAt(first - 1), before.keyedAt(last + 1));
        if (before.keyedAt((first + last) / 2) <= outside + amountDb + 0.01) continue;  // already down
        const double lo = before.keyedAt(first - 1), hi = before.keyedAt(last + 1);
        // The line after the breath carries on as it went (a key before it set how it moves on).
        Interp after = Interp::Linear;
        for (const Keyframe& k : before.keys)
            if (k.t <= last + 1) after = k.interp;
        if (!gain.animated()) gain.addKey(0, gain.value);  // (the level before the first breath, held)
        for (Keyframe& k : gain.keys)  // keys already inside the breath go down with it
            if (k.t > first && k.t < last) k.v += amountDb;
        gain.addKey(first - 1, lo);
        gain.addKey(first, before.keyedAt(first) + amountDb);
        gain.addKey(last, before.keyedAt(last) + amountDb);
        gain.addKey(last + 1, hi, after);
        ++count;
    }
    if (!count) gain = before;
    return count;
}

}  // namespace montage
