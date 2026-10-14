#include "audio/Ltc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage {

namespace {

// Bits 64 to 79, in the order they are sent.
constexpr uint8_t kSync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
// Where each user-bit group starts.
constexpr int kUserGroups[8] = {4, 12, 20, 28, 36, 44, 52, 60};

int field(const uint8_t* bits, int from, int count) {
    int v = 0;
    for (int i = 0; i < count; ++i) v |= int(bits[from + i] & 1) << i;
    return v;
}

void put(uint8_t* bits, int from, int count, int v) {
    for (int i = 0; i < count; ++i) bits[from + i] = uint8_t((v >> i) & 1);
}

int baseOf(Rational fps) {
    const double r = fps.toDouble();
    return r < 24.5 ? 24 : r < 27.5 ? 25 : 30;
}

}  // namespace

std::array<uint8_t, 80> ltcBits(const LtcFrame& f, int base) {
    std::array<uint8_t, 80> b{};
    put(b.data(), 0, 4, f.frames % 10);
    put(b.data(), 8, 2, f.frames / 10);
    b[10] = f.dropFrame;
    b[11] = f.colourFrame;
    put(b.data(), 16, 4, f.seconds % 10);
    put(b.data(), 24, 3, f.seconds / 10);
    put(b.data(), 32, 4, f.minutes % 10);
    put(b.data(), 40, 3, f.minutes / 10);
    put(b.data(), 48, 4, f.hours % 10);
    put(b.data(), 56, 2, f.hours / 10);
    for (int g = 0; g < 8; ++g) put(b.data(), kUserGroups[g], 4, int((f.userBits >> (4 * g)) & 0xF));
    std::memcpy(b.data() + 64, kSync, 16);
    const int polarity = base == 25 ? 59 : 27;
    if (std::count(b.begin(), b.end(), uint8_t(0)) % 2) b[size_t(polarity)] = 1;
    return b;
}

bool ltcFromBits(const uint8_t* bits, LtcFrame& out) {
    const int fu = field(bits, 0, 4), ft = field(bits, 8, 2), su = field(bits, 16, 4), st = field(bits, 24, 3);
    const int mu = field(bits, 32, 4), mt = field(bits, 40, 3), hu = field(bits, 48, 4), ht = field(bits, 56, 2);
    if (fu > 9 || su > 9 || st > 5 || mu > 9 || mt > 5 || hu > 9) return false;
    LtcFrame f;
    f.frames = ft * 10 + fu;
    f.seconds = st * 10 + su;
    f.minutes = mt * 10 + mu;
    f.hours = ht * 10 + hu;
    if (f.frames > 29 || f.hours > 23) return false;
    f.dropFrame = bits[10] & 1;
    f.colourFrame = bits[11] & 1;
    for (int g = 0; g < 8; ++g) f.userBits |= uint32_t(field(bits, kUserGroups[g], 4)) << (4 * g);
    out = f;
    return true;
}

int64_t ltcFrameNumber(const LtcFrame& f, int base) {
    const int64_t minutes = int64_t(f.hours) * 60 + f.minutes;
    int64_t n = (minutes * 60 + f.seconds) * base + f.frames;
    if (f.dropFrame && base == 30) n -= 2 * (minutes - minutes / 10);
    return n;
}

int64_t ltcFramesPerDay(int base, bool dropFrame) { return dropFrame && base == 30 ? 2589408 : int64_t(base) * 86400; }

LtcFrame ltcFrameAt(int64_t number, int base, bool dropFrame) {
    dropFrame = dropFrame && base == 30;
    const int64_t day = ltcFramesPerDay(base, dropFrame);
    int64_t n = ((number % day) + day) % day;
    if (dropFrame) {
        const int64_t d = n / 17982, m = n % 17982;
        n += 18 * d + 2 * ((m - 2) / 1798);  // (m below 2 adds nothing: C++ division truncates toward zero)
    }
    LtcFrame f;
    f.dropFrame = dropFrame;
    f.frames = int(n % base);
    n /= base;
    f.seconds = int(n % 60);
    n /= 60;
    f.minutes = int(n % 60);
    f.hours = int(n / 60);
    return f;
}

std::string ltcLabel(const LtcFrame& f) {
    char s[16];
    std::snprintf(s, sizeof s, "%02d:%02d:%02d%c%02d", f.hours, f.minutes, f.seconds, f.dropFrame ? ';' : ':', f.frames);
    return s;
}

std::vector<float> encodeLtc(int64_t start, int64_t count, Rational fps, bool dropFrame, int sampleRate, float level, uint32_t userBits) {
    std::vector<float> out;
    if (count <= 0 || sampleRate <= 0 || !fps.valid()) return out;
    const int base = baseOf(fps);
    dropFrame = dropFrame && base == 30 && fps.den != 1;
    const double perWord = double(sampleRate) * fps.den / fps.num;
    const double bit = perWord / 80;
    out.assign(size_t(std::ceil(perWord * double(count))), 0.0f);
    // Each sample is the square wave's average over a window about 25 µs wide (rising 10 to 90 % in 0.8 of it), so
    // the edges are neither too steep nor aliased.
    const double w = std::max(1.0, 31.25e-6 * sampleRate);
    double level0 = -1;  // the level just before the word starts
    std::vector<double> edges;
    for (int64_t k = 0; k < count; ++k) {
        LtcFrame f = ltcFrameAt(start + k, base, dropFrame);
        f.userBits = userBits;
        const auto bits = ltcBits(f, base);
        const double t0 = double(k) * perWord;
        edges.clear();
        for (int j = 0; j < 80; ++j) {
            edges.push_back(t0 + j * bit);
            if (bits[size_t(j)]) edges.push_back(t0 + (j + 0.5) * bit);
        }
        edges.push_back(double(k + 1) * perWord);  // the next word's first edge
        const int64_t from = int64_t(std::ceil(t0)), to = std::min<int64_t>(int64_t(out.size()), int64_t(std::ceil(double(k + 1) * perWord)));
        size_t e = 0;
        for (int64_t i = from; i < to; ++i) {
            const double a = double(i) - w / 2, b = double(i) + w / 2;
            while (e < edges.size() && edges[e] <= a) ++e;
            double lv = (e % 2 == 0) ? level0 : -level0;  // the level at a
            double sum = 0, at = a;
            for (size_t x = e; x < edges.size() && edges[x] < b; ++x) {
                sum += lv * (edges[x] - at);
                at = edges[x];
                lv = -lv;
            }
            sum += lv * (b - at);
            out[size_t(i)] = float(level * sum / w);
        }
        if ((edges.size() - 1) % 2) level0 = -level0;
    }
    return out;
}

LtcDecoder::LtcDecoder(int sampleRate) : rate_(std::max(1, sampleRate)) {
    minBit_ = 0.33e-3 * rate_;
    maxBit_ = 0.66e-3 * rate_;
}

void LtcDecoder::write(const float* samples, size_t n) {
    const double dcRate = 1 - std::exp(-2 * M_PI * 20.0 / rate_);
    const double decay = std::exp(-1.0 / (0.02 * rate_));
    for (size_t i = 0; i < n; ++i, ++index_) {
        dc_ += (samples[i] - dc_) * dcRate;
        const float y = float(samples[i] - dc_);
        envelope_ = std::max(double(std::fabs(y)), envelope_ * decay);
        if ((prev_ < 0 && y >= 0) || (prev_ > 0 && y <= 0)) crossing_ = double(index_ - 1) + prev_ / (prev_ - y);
        prev_ = y;
        const double threshold = std::max(0.2 * envelope_, 0.002);
        if (y > threshold && state_ != 1) {
            if (state_ != 0 && crossing_ >= 0) edge(crossing_);
            state_ = 1;
        } else if (y < -threshold && state_ != -1) {
            if (state_ != 0 && crossing_ >= 0) edge(crossing_);
            state_ = -1;
        }
    }
}

std::vector<LtcFrame> LtcDecoder::take() { return std::exchange(out_, {}); }

void LtcDecoder::edge(double t) {
    if (lastEdge_ < 0) {
        lastEdge_ = t;
        return;
    }
    const double d = t - lastEdge_;
    lastEdge_ = t;
    int kind = 0;  // 2 a whole bit, 1 half of one, 0 neither
    if (period_ <= 0) {
        if (d >= minBit_ && d <= maxBit_) kind = 2, period_ = d;
        else if (d >= minBit_ / 2 && d < minBit_) kind = 1, period_ = 2 * d;
    } else if (std::fabs(d - period_) < 0.25 * period_) {
        kind = 2;
    } else if (std::fabs(d - period_ / 2) < 0.25 * period_) {
        kind = 1;
    }
    if (kind == 0) {  // noise, a dropout or the signal stopping: start again
        period_ = 0;
        held_ = 0;
        halfStart_ = -1;
        return;
    }
    period_ += 0.1 * ((kind == 2 ? d : 2 * d) - period_);
    period_ = std::clamp(period_, minBit_ * 0.8, maxBit_ * 1.2);
    if (kind == 2) {
        if (halfStart_ >= 0) held_ = 0, halfStart_ = -1;  // a half bit without its pair: what was held is wrong
        bit(0, t - d, t);
    } else if (halfStart_ < 0) {
        halfStart_ = t - d;
    } else {
        bit(1, halfStart_, t);
        halfStart_ = -1;
    }
}

void LtcDecoder::bit(int value, double from, double to) {
    if (held_ == 80) {
        std::memmove(bits_.data(), bits_.data() + 1, 79);
        std::memmove(times_.data(), times_.data() + 1, 80 * sizeof(double));
        --held_;
    }
    bits_[size_t(held_)] = uint8_t(value);
    times_[size_t(held_)] = from;
    ++held_;
    times_[size_t(held_)] = to;
    if (held_ < 80 || std::memcmp(bits_.data() + 64, kSync, 16) != 0) return;
    LtcFrame f;
    if (ltcFromBits(bits_.data(), f)) {
        f.start = times_[0];
        f.end = times_[80];
        out_.push_back(f);
    }
    held_ = 0;
}

}  // namespace montage
