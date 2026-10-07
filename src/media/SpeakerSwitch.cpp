#include "SpeakerSwitch.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Decoder.h"
#include "core/Multicam.h"

namespace montage {

namespace {
constexpr int kRate = 4000;           // speech energy sits well below 2 kHz
constexpr double kWindow = 0.05;      // seconds per analysis window
constexpr int kSmooth = 6;            // windows averaged (0.3 s)
}  // namespace

std::vector<std::pair<FrameTime, int>> speakerAngleChanges(const Project& p, const Sequence& mc, const AutoSwitchOptions& o,
                                                           std::string* error, const std::atomic<bool>* cancel) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return std::vector<std::pair<FrameTime, int>>{};
    };
    const int angles = int(mc.videoTracks.size());
    std::vector<int> listen = o.listen;
    if (listen.empty())
        for (int a = 0; a < angles; ++a) listen.push_back(angleAudioTrack(mc, a));
    listen.resize(size_t(angles), -1);
    const double fps = mc.fpsValue();
    const double seconds = double(mc.duration()) / fps;
    const int windows = int(std::ceil(seconds / kWindow));
    if (windows <= 0) return fail("The multicam clip is empty");

    // Mean square level per window of each listened-to track (decoded once per file).
    std::map<int, std::vector<double>> energy;
    std::map<std::string, AudioBufferPtr> decoded;
    for (int a = 0; a < angles; ++a) {
        const int track = listen[size_t(a)];
        if (track < 0 || track >= int(mc.audioTracks.size()) || energy.count(track)) continue;
        std::vector<double> e(size_t(windows), 0.0);
        for (const Clip& c : mc.audioTracks[size_t(track)].clips) {
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || m->path.empty() || !m->hasAudio) continue;
            AudioBufferPtr& buf = decoded[m->path];
            std::string err;
            if (!buf) buf = decodeAudio(m->path, kRate, &err, cancel);
            if (cancel && cancel->load()) return fail("Cancelled");
            if (!buf) return fail("Cannot read the sound of " + m->name + (err.empty() ? "" : ": " + err));
            const int64_t n = buf->frames();
            const double gain = std::pow(10.0, c.audio.p("gain_db", 0) / 20.0);
            const int w0 = std::max(0, int(double(c.start) / fps / kWindow));
            const int w1 = std::min(windows, int(std::ceil(double(c.end()) / fps / kWindow)));
            for (int w = w0; w < w1; ++w) {
                // Timeline seconds of this window to source samples.
                const double t = w * kWindow;
                const double src = c.sourceIn / fps + (t - double(c.start) / fps) * c.speed;
                const int64_t s0 = int64_t(src * kRate), s1 = s0 + int64_t(kWindow * kRate * c.speed);
                double sum = 0;
                int64_t count = 0;
                for (int64_t s = std::max<int64_t>(0, s0); s < std::min(s1, n); ++s, ++count) {
                    const double v = 0.5 * (buf->samples[size_t(s) * 2] + buf->samples[size_t(s) * 2 + 1]) * gain;
                    sum += v * v;
                }
                if (count) e[size_t(w)] += sum / double(count);
            }
        }
        // Smooth over a few windows so syllables and breaths do not flip the choice.
        std::vector<double> sm(e.size(), 0.0);
        double run = 0;
        for (size_t i = 0; i < e.size(); ++i) {
            run += e[i];
            if (i >= kSmooth) run -= e[i - kSmooth];
            sm[i] = run / double(std::min<size_t>(i + 1, kSmooth));
        }
        energy[track] = std::move(sm);
    }
    if (energy.empty()) return fail("No angle has a microphone to listen to: choose one for each speaker's angle");

    // Who has the floor in each window: -1 nobody, -2 several people, else the angle.
    auto db = [](double ms) { return 10 * std::log10(ms + 1e-12); };
    std::vector<int> floor(size_t(windows), -1);
    for (int w = 0; w < windows; ++w) {
        int best = -1;
        double bestDb = -1e9, secondDb = -1e9;
        for (int a = 0; a < angles; ++a) {
            const int track = listen[size_t(a)];
            if (track < 0 || !energy.count(track)) continue;
            const double d = db(energy[track][size_t(w)]);
            if (d > bestDb) {
                secondDb = bestDb;
                bestDb = d;
                best = a;
            } else if (d > secondDb) {
                secondDb = d;
            }
        }
        if (best < 0 || bestDb < o.silenceDb) floor[size_t(w)] = -1;
        else floor[size_t(w)] = bestDb - secondDb >= o.marginDb ? best : -2;
    }

    // Shots: follow the floor, hold each shot for the minimum, ignore blips.
    const int minShot = std::max(1, int(std::lround(o.minShotSeconds / kWindow)));
    const int hold = std::max(1, int(std::lround(o.holdSeconds / kWindow)));
    auto wanted = [&](int w, int current) {
        const int f = floor[size_t(w)];
        if (f >= 0) return f;
        return o.wideAngle >= 0 ? o.wideAngle : current;
    };
    int current = -1;
    for (int w = 0; w < windows && current < 0; ++w)
        if (floor[size_t(w)] >= 0) current = floor[size_t(w)];
    if (current < 0) current = o.wideAngle >= 0 ? o.wideAngle : 0;
    if (o.wideAngle >= 0 && floor[0] < 0) current = o.wideAngle;
    std::vector<std::pair<FrameTime, int>> changes = {{0, current}};
    int shotStart = 0;
    int candidate = current, candidateStart = 0;
    for (int w = 0; w < windows; ++w) {
        const int want = wanted(w, current);
        if (want != candidate) {
            candidate = want;
            candidateStart = w;
        }
        if (candidate == current) continue;
        if (w - candidateStart + 1 < hold) continue;  // not yet sure
        // Cut where the new speaker started, but not before the shot has run its minimum.
        const int at = std::max(candidateStart, shotStart + minShot);
        if (at > w) continue;
        changes.push_back({FrameTime(std::llround(at * kWindow * fps)), candidate});
        current = candidate;
        shotStart = at;
    }
    return changes;
}

}  // namespace montage
