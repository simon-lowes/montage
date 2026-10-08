#include "VoiceMatch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>

#include "AudioFx.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Decoder.h"

namespace montage {

namespace {

constexpr int kFft = 4096;
constexpr int kHop = 2048;
constexpr int kRate = 48000;

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// How much a band counts in the fit: the voice's core most, the extremes less.
double weight(double hz) { return hz < 150 || hz > 8000 ? 0.5 : 1.0; }

fx::ParametricEq makeEq(const VoiceEq& g, double rate) {
    fx::ParametricEq eq;
    eq.set(rate, {120, g.lowDb, 1}, {300, g.b1Db, 0.9}, {1200, g.b2Db, 0.9}, {4000, g.b3Db, 0.9}, {8000, g.highDb, 1}, 0);
    return eq;
}

VoiceEq fromGains(const std::array<double, 5>& v) {
    VoiceEq g;
    g.lowDb = v[0], g.b1Db = v[1], g.b2Db = v[2], g.b3Db = v[3], g.highDb = v[4];
    return g;
}

// Weighted RMS of a difference after removing its weighted mean (shape only).
double shapeRms(const std::vector<double>& d) {
    const auto& bands = voiceBands();
    double wsum = 0, mean = 0;
    for (size_t i = 0; i < d.size(); ++i) wsum += weight(bands[i]), mean += weight(bands[i]) * d[i];
    mean /= wsum;
    double acc = 0;
    for (size_t i = 0; i < d.size(); ++i) acc += weight(bands[i]) * (d[i] - mean) * (d[i] - mean);
    return std::sqrt(acc / wsum);
}

// Solves the small symmetric system a x = b (Gaussian elimination with pivoting).
template <size_t N>
std::array<double, N> solve(std::array<std::array<double, N>, N> a, std::array<double, N> b) {
    for (size_t c = 0; c < N; ++c) {
        size_t piv = c;
        for (size_t r = c + 1; r < N; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        std::swap(a[c], a[piv]);
        std::swap(b[c], b[piv]);
        if (std::fabs(a[c][c]) < 1e-12) continue;
        for (size_t r = 0; r < N; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (size_t k = c; k < N; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    std::array<double, N> x{};
    for (size_t i = 0; i < N; ++i) x[i] = std::fabs(a[i][i]) > 1e-12 ? b[i] / a[i][i] : 0;
    return x;
}

}  // namespace

const std::vector<double>& voiceBands() {
    static const std::vector<double> bands = {100,  125,  160,  200,  250,  315,  400,  500,  630,  800, 1000,
                                              1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000};
    return bands;
}

std::vector<double> speechSpectrum(const std::vector<float>& mono, int rate) {
    const auto& bands = voiceBands();
    std::vector<double> out(bands.size(), -120.0);
    if (mono.size() < size_t(kFft)) return out;
    std::vector<double> window(kFft);
    for (int i = 0; i < kFft; ++i) window[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * i / kFft);
    std::vector<std::vector<double>> frames;
    std::vector<double> energy;
    std::vector<std::complex<double>> buf(kFft);
    for (size_t start = 0; start + kFft <= mono.size(); start += kHop) {
        for (int i = 0; i < kFft; ++i) buf[size_t(i)] = mono[start + size_t(i)] * window[size_t(i)];
        fft(buf);
        std::vector<double> bandPower(bands.size(), 0.0);
        double total = 0;
        for (size_t b = 0; b < bands.size(); ++b) {
            const double lo = bands[b] / std::pow(2.0, 1.0 / 6), hi = bands[b] * std::pow(2.0, 1.0 / 6);
            const int i0 = std::max(1, int(std::ceil(lo * kFft / rate))), i1 = std::min(kFft / 2, int(std::floor(hi * kFft / rate)));
            for (int i = i0; i <= i1; ++i) bandPower[b] += std::norm(buf[size_t(i)]);
            if (i1 < i0) bandPower[b] = std::norm(buf[size_t(std::clamp(int(std::lround(bands[b] * kFft / rate)), 1, kFft / 2))]);
            total += bandPower[b];
        }
        frames.push_back(std::move(bandPower));
        energy.push_back(total);
    }
    // The louder 40 % of the frames: speech rather than pauses and room tone.
    std::vector<double> sorted = energy;
    std::sort(sorted.begin(), sorted.end());
    const double threshold = sorted[size_t(double(sorted.size()) * 0.6)];
    std::vector<double> sum(bands.size(), 0.0);
    int used = 0;
    for (size_t f = 0; f < frames.size(); ++f) {
        if (energy[f] < threshold || energy[f] <= 0) continue;
        for (size_t b = 0; b < bands.size(); ++b) sum[b] += frames[f][b];
        ++used;
    }
    if (used == 0) return out;
    for (size_t b = 0; b < bands.size(); ++b) out[b] = 10 * std::log10(std::max(1e-20, sum[b] / used));
    return out;
}

std::vector<double> withVoiceEq(const std::vector<double>& spectrum, const VoiceEq& g, double rate) {
    const fx::ParametricEq eq = makeEq(g, rate);
    std::vector<double> out = spectrum;
    const auto& bands = voiceBands();
    for (size_t b = 0; b < out.size() && b < bands.size(); ++b) out[b] += eq.responseDb(bands[b]);
    return out;
}

VoiceEq fitVoiceEq(const std::vector<double>& target, const std::vector<double>& reference, double rate) {
    const auto& bands = voiceBands();
    const size_t n = bands.size();
    VoiceEq g;
    if (target.size() != n || reference.size() != n) return g;
    std::vector<double> d(n);
    for (size_t b = 0; b < n; ++b) d[b] = std::clamp(reference[b] - target[b], -30.0, 30.0);
    g.beforeDb = shapeRms(d);
    // Gauss-Newton on the five gains plus a level offset (the level is not matched, only the shape),
    // with a little damping so gains stay modest when the data do not need them.
    std::array<double, 5> x{};
    for (int iter = 0; iter < 6; ++iter) {
        const fx::ParametricEq eq = makeEq(fromGains(x), rate);
        std::vector<double> r(n);
        for (size_t b = 0; b < n; ++b) r[b] = d[b] - eq.responseDb(bands[b]);
        // Jacobian by finite differences (1 dB).
        std::array<std::vector<double>, 5> J;
        for (size_t k = 0; k < 5; ++k) {
            std::array<double, 5> xp = x;
            xp[k] += 1;
            const fx::ParametricEq e2 = makeEq(fromGains(xp), rate);
            J[k].resize(n);
            for (size_t b = 0; b < n; ++b) J[k][b] = e2.responseDb(bands[b]) - eq.responseDb(bands[b]);
        }
        // Unknowns: 5 gain steps and the offset.
        std::array<std::array<double, 6>, 6> A{};
        std::array<double, 6> rhs{};
        for (size_t b = 0; b < n; ++b) {
            const double w = weight(bands[b]);
            std::array<double, 6> row{J[0][b], J[1][b], J[2][b], J[3][b], J[4][b], 1.0};
            for (size_t i = 0; i < 6; ++i) {
                rhs[i] += w * row[i] * r[b];
                for (size_t j = 0; j < 6; ++j) A[i][j] += w * row[i] * row[j];
            }
        }
        for (size_t i = 0; i < 5; ++i) A[i][i] += 0.05;  // damping, so steps stay small where the data say little
        const auto step = solve<6>(A, rhs);
        double moved = 0;
        for (size_t k = 0; k < 5; ++k) {
            const double nx = std::clamp(x[k] + step[k], -12.0, 12.0);
            moved = std::max(moved, std::fabs(nx - x[k]));
            x[k] = nx;
        }
        if (moved < 0.01) break;
    }
    const VoiceEq fitted = fromGains(x);
    g.lowDb = fitted.lowDb, g.b1Db = fitted.b1Db, g.b2Db = fitted.b2Db, g.b3Db = fitted.b3Db, g.highDb = fitted.highDb;
    const std::vector<double> after = withVoiceEq(target, g, rate);
    std::vector<double> rest(n);
    for (size_t b = 0; b < n; ++b) rest[b] = reference[b] - after[b];
    g.afterDb = shapeRms(rest);
    return g;
}

Effect voiceEqEffect(Project& p, const VoiceEq& g) {
    Effect e = makeEffect(p, "parametric_eq");
    e.params["low_hz"] = Param(120.0);
    e.params["low_db"] = Param(g.lowDb);
    e.params["b1_hz"] = Param(300.0);
    e.params["b1_db"] = Param(g.b1Db);
    e.params["b1_q"] = Param(0.9);
    e.params["b2_hz"] = Param(1200.0);
    e.params["b2_db"] = Param(g.b2Db);
    e.params["b2_q"] = Param(0.9);
    e.params["b3_hz"] = Param(4000.0);
    e.params["b3_db"] = Param(g.b3Db);
    e.params["b3_q"] = Param(0.9);
    e.params["high_hz"] = Param(8000.0);
    e.params["high_db"] = Param(g.highDb);
    e.strings["match"] = "voice";
    return e;
}

bool clipSpeechSpectrum(const Project& p, const Sequence& s, const Clip& c, std::vector<double>& out, std::string* error) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->path.empty()) {
        if (error) *error = "The clip has no sound";
        return false;
    }
    AudioBufferPtr buf = decodeAudio(m->path, kRate, error);
    if (!buf) return false;
    const double fps = s.fpsValue();
    double a = c.sourceFrameAt(c.start) / fps, b = c.sourceFrameAt(c.end() - 1) / fps;
    if (a > b) std::swap(a, b);
    const int64_t first = std::clamp<int64_t>(int64_t(a * kRate), 0, buf->frames());
    const int64_t last = std::clamp<int64_t>(int64_t((b + 1 / fps) * kRate), first, buf->frames());
    if (last - first < kRate) {
        if (error) *error = "The clip is too short to measure its voice";
        return false;
    }
    std::vector<float> mono(size_t(last - first));
    for (int64_t i = first; i < last; ++i) mono[size_t(i - first)] = 0.5f * (buf->samples[size_t(i) * 2] + buf->samples[size_t(i) * 2 + 1]);
    out = speechSpectrum(mono, kRate);
    return true;
}

void applyVoiceEq(Project& p, Clip& c, const VoiceEq& eq) {
    c.effects.erase(std::remove_if(c.effects.begin(), c.effects.end(),
                                   [](const Effect& e) { return e.type == "parametric_eq" && e.s("match") == "voice"; }),
                    c.effects.end());
    c.effects.insert(c.effects.begin(), voiceEqEffect(p, eq));
}

int matchVoices(Project& p, Sequence& s, const std::vector<Id>& clips, const std::vector<double>& reference, std::string* error) {
    int n = 0;
    for (Id id : clips) {
        auto loc = edit::locate(s, id);
        if (!loc || loc->track.kind != TrackKind::Audio) continue;
        Clip* c = edit::clipById(s, id);
        // Measured without an earlier match, so matching again does not stack.
        std::vector<double> spectrum;
        if (!c || !clipSpeechSpectrum(p, s, *c, spectrum, error)) continue;
        applyVoiceEq(p, *c, fitVoiceEq(spectrum, reference));
        ++n;
    }
    return n;
}

}  // namespace montage
