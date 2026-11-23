#include "LiveTake.h"

#include <algorithm>
#include <cmath>

namespace montage {

void LiveTake::pass(qint64& first, FrameTime& from) const {
    first = 0;
    from = at;
    if (loopOut <= loopIn || fps <= 0) return;
    const qint64 lead = qint64(std::llround(double(preroll) / fps * rate));
    const double length = double(loopOut - loopIn) / fps * rate;
    if (frames <= lead || length < 1) return;
    const double k = std::floor(double(frames - lead) / length);
    first = lead + qint64(std::llround(k * length));
    if (first >= frames) first = lead + qint64(std::llround(std::max(0.0, k - 1) * length));
    from = loopIn;
}

LiveTakes& LiveTakes::instance() {
    static LiveTakes takes;
    return takes;
}

void LiveTakes::begin(LiveTake take) {
    end(take.owner);
    take.frames = 0;
    take.channels = std::max(1, take.channels);
    if (take.tracks.empty()) take.tracks.push_back(0);
    take.peaks.assign(take.tracks.size(), Peaks{});
    for (Peaks& p : take.peaks) {
        p.samplesPerBucket = 256;
        p.sampleRate = take.rate;
    }
    takes_.push_back(std::move(take));
    told_.invalidate();
    emit changed();
}

void LiveTakes::setLoop(const QObject* owner, FrameTime in, FrameTime out, FrameTime preroll) {
    for (LiveTake& t : takes_)
        if (t.owner == owner) t.loopIn = in, t.loopOut = out, t.preroll = preroll;
}

void LiveTakes::add(const QObject* owner, const float* interleaved, qint64 frames) {
    for (LiveTake& t : takes_) {
        if (t.owner != owner || frames <= 0) continue;
        const bool grouped = t.peaks.size() > 1;
        const size_t ch = size_t(t.channels);
        for (size_t k = 0; k < t.peaks.size(); ++k) {
            Peaks& p = t.peaks[k];
            const size_t buckets = size_t((t.frames + frames + p.samplesPerBucket - 1) / p.samplesPerBucket);
            p.minmax.resize(buckets * 2, 0.0f);
            for (qint64 i = 0; i < frames; ++i) {
                const float* s = interleaved + size_t(i) * ch;
                // (as computePeaks reads the file: a grouped take's channel; a stereo take's two sides' mean; a mono
                // take as it is read, at -3 dB on each side; else the first)
                const float v = grouped ? s[std::min(k, ch - 1)] : ch == 2 ? 0.5f * (s[0] + s[1]) : ch == 1 ? float(M_SQRT1_2) * s[0] : s[0];
                const size_t b = size_t((t.frames + i) / p.samplesPerBucket);
                p.minmax[b * 2] = std::min(p.minmax[b * 2], v);
                p.minmax[b * 2 + 1] = std::max(p.minmax[b * 2 + 1], v);
            }
        }
        t.frames += frames;
        // (drawn again no more than thirty times a second, however small the device's blocks)
        if (!told_.isValid() || told_.elapsed() >= 33) {
            told_.start();
            emit changed();
        }
        return;
    }
}

void LiveTakes::end(const QObject* owner) {
    const auto it = std::remove_if(takes_.begin(), takes_.end(), [&](const LiveTake& t) { return t.owner == owner; });
    if (it == takes_.end()) return;
    takes_.erase(it, takes_.end());
    emit changed();
}

const LiveTake* LiveTakes::takeOf(const QObject* owner) const {
    for (const LiveTake& t : takes_)
        if (t.owner == owner) return &t;
    return nullptr;
}

}  // namespace montage
