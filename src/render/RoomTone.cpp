#include "RoomTone.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <fstream>
#include <limits>
#include <random>

#include "core/EditOps.h"
#include "media/Decoder.h"

namespace montage {

namespace {

using cd = std::complex<double>;

void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2 : -2) * M_PI / double(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (cd& x : a) x /= double(n);
}

std::vector<double> hann(int n) {
    std::vector<double> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);
    return w;
}

}  // namespace

bool learnRoomTone(const std::vector<float>& stereo, int sampleRate, RoomToneProfile& out, std::string* error) {
    out = RoomToneProfile{};
    out.sampleRate = sampleRate;
    const int n = out.fftSize, hop = n / 2;
    const int64_t total = int64_t(stereo.size() / 2);
    if (total < n * 4) {
        if (error) *error = "The sound is too short to learn its room tone";
        return false;
    }
    // Every frame's level; the quietest that are not digital silence are the room.
    struct Frame {
        int64_t at;
        double rms;
    };
    std::vector<Frame> frames;
    for (int64_t at = 0; at + n <= total; at += hop) {
        double e = 0;
        for (int i = 0; i < n; ++i) {
            const double l = stereo[size_t(at + i) * 2], r = stereo[size_t(at + i) * 2 + 1];
            e += 0.5 * (l * l + r * r);
        }
        frames.push_back({at, std::sqrt(e / n)});
    }
    std::vector<Frame> live;
    for (const Frame& f : frames)
        if (f.rms > 1e-5) live.push_back(f);  // above -100 dBFS
    if (live.size() < 8) {
        if (error) *error = "There is no background sound to learn (the clip is silent)";
        return false;
    }
    // The room is every frame within 6 dB of the quietest tenth: all of the background between words (taking only the
    // very quietest would read it low), none of the speech.
    std::sort(live.begin(), live.end(), [](const Frame& a, const Frame& b) { return a.rms < b.rms; });
    const double floor = live[live.size() / 10].rms;
    size_t take = 0;
    while (take < live.size() && live[take].rms <= floor * 2) ++take;
    live.resize(std::max<size_t>(std::min<size_t>(8, live.size()), take));
    const std::vector<double> w = hann(n);
    const size_t bins = size_t(n / 2 + 1);
    std::vector<double> pm(bins, 0), ps(bins, 0);
    double energy = 0;
    std::vector<cd> m(static_cast<size_t>(n)), sd(static_cast<size_t>(n));
    for (const Frame& f : live) {
        for (int i = 0; i < n; ++i) {
            const double l = stereo[size_t(f.at + i) * 2], r = stereo[size_t(f.at + i) * 2 + 1];
            m[size_t(i)] = cd(0.5 * (l + r) * w[size_t(i)], 0);
            sd[size_t(i)] = cd(0.5 * (l - r) * w[size_t(i)], 0);
        }
        fft(m, false);
        fft(sd, false);
        for (size_t k = 0; k < bins; ++k) {
            pm[k] += std::norm(m[k]);
            ps[k] += std::norm(sd[k]);
        }
        energy += f.rms * f.rms;
    }
    out.mid.resize(bins);
    out.side.resize(bins);
    for (size_t k = 0; k < bins; ++k) {
        out.mid[k] = std::sqrt(pm[k] / double(live.size()));
        out.side[k] = std::sqrt(ps[k] / double(live.size()));
    }
    out.mid[0] = out.side[0] = 0;  // no offset
    out.rms = std::sqrt(energy / double(live.size()));
    out.frames = int(live.size());
    return true;
}

std::vector<float> synthesizeRoomTone(const RoomToneProfile& prof, int64_t frames, uint32_t seed) {
    std::vector<float> out(size_t(std::max<int64_t>(0, frames)) * 2, 0.0f);
    if (!prof.valid() || frames <= 0) return out;
    const int n = prof.fftSize, hop = n / 4;
    const size_t bins = size_t(n / 2 + 1);
    const std::vector<double> w = hann(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> phase(0, 2 * M_PI);
    const int64_t padded = frames + n;
    std::vector<double> mid(size_t(padded), 0), side(size_t(padded), 0);
    std::vector<cd> buf(static_cast<size_t>(n));
    auto frame = [&](const std::vector<double>& mag, std::vector<double>& dst, int64_t at) {
        for (size_t k = 0; k < bins; ++k) {
            const cd v = std::polar(mag[k], phase(rng));
            buf[k] = v;
            if (k > 0 && k < size_t(n / 2)) buf[size_t(n) - k] = std::conj(v);
        }
        buf[size_t(n / 2)] = cd(buf[size_t(n / 2)].real(), 0);
        fft(buf, true);
        for (int i = 0; i < n; ++i) dst[size_t(at + i)] += buf[size_t(i)].real() * w[size_t(i)];
    };
    for (int64_t at = -n; at < padded - n; at += hop) {
        if (at < 0) continue;
        frame(prof.mid, mid, at);
        frame(prof.side, side, at);
    }
    // Overlap-added noise is steady once the frames overlap fully (from n onwards): level it to the room's.
    const int64_t offset = n;
    double e = 0;
    int64_t count = 0;
    for (int64_t i = 0; i < frames && offset + i < padded; ++i) {
        const double m = mid[size_t(offset + i)], s = side[size_t(offset + i)];
        e += m * m + s * s;  // = (l² + r²) / 2
        ++count;
    }
    const double rms = count ? std::sqrt(e / double(count)) : 0;
    const double gain = rms > 1e-12 ? prof.rms / rms : 0;
    const int64_t fade = std::max<int64_t>(1, prof.sampleRate / 100);
    for (int64_t i = 0; i < frames; ++i) {
        const int64_t j = offset + i;
        double g = gain;
        if (i < fade) g *= double(i) / double(fade);
        if (frames - 1 - i < fade) g *= double(frames - 1 - i) / double(fade);
        const double m = j < padded ? mid[size_t(j)] : 0, s = j < padded ? side[size_t(j)] : 0;
        out[size_t(i) * 2] = float((m + s) * g);
        out[size_t(i) * 2 + 1] = float((m - s) * g);
    }
    return out;
}

bool clipRoomTone(const Project& p, const Sequence& s, const Clip& c, RoomToneProfile& out, std::string* error) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->path.empty()) {
        if (error) *error = "The clip has no sound to learn the room from";
        return false;
    }
    constexpr int kRate = 48000;
    AudioBufferPtr buf = decodeAudio(m->path, kRate, error, nullptr, c.channels);
    if (!buf) return false;
    const double fps = s.fpsValue();
    double a = c.sourceFrameAt(c.start) / fps, b = c.sourceFrameAt(c.end() - 1) / fps;
    if (a > b) std::swap(a, b);
    const int64_t first = std::clamp<int64_t>(int64_t(a * kRate), 0, buf->frames());
    const int64_t last = std::clamp<int64_t>(int64_t((b + 1 / fps) * kRate), first, buf->frames());
    const std::vector<float> part(buf->samples.begin() + std::ptrdiff_t(first * 2), buf->samples.begin() + std::ptrdiff_t(last * 2));
    return learnRoomTone(part, kRate, out, error);
}

bool gapAt(const Sequence& s, TrackRef track, FrameTime at, TrackGap& out) {
    const Track* t = trackAt(s, track);
    if (!t) return false;
    out = TrackGap{};
    out.end = std::numeric_limits<FrameTime>::max();
    for (const Clip& c : t->clips) {
        if (c.contains(at)) return false;
        if (c.end() <= at && c.end() >= out.start) out.start = c.end(), out.before = c.id;
        if (c.start > at && c.start < out.end) out.end = c.start, out.after = c.id;
    }
    if (out.end == std::numeric_limits<FrameTime>::max()) out.end = std::max(at + 1, s.duration());
    return out.end > out.start;
}

bool writeStereoWav(const std::string& path, const std::vector<float>& stereo, int sampleRate, std::string* error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(stereo.size() * 3);
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(uint32_t(sampleRate)),
        u32(uint32_t(sampleRate) * 6), u16(6), u16(24), f.write("data", 4), u32(bytes);
    for (float v : stereo) {
        const int32_t q = int32_t(std::lround(double(std::clamp(v, -1.0f, 1.0f)) * 8388607.0));
        const char b[3] = {char(q & 0xff), char((q >> 8) & 0xff), char((q >> 16) & 0xff)};
        f.write(b, 3);
    }
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace montage
