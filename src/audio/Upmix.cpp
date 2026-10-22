#include "Upmix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/RoomResponse.h"
#include "core/Surround.h"

namespace montage {

namespace {

constexpr double kSmoothing = 0.15;  // seconds the powers are averaged over
constexpr int kSpread = 4;           // and bins either side
// What that averaging leaves of the squared coherence of two unrelated noises (measured at 48 kHz; fewer frames are
// averaged at lower rates, more at higher), taken off so they read as unrelated.
constexpr double kCoherenceBias = 0.014;
constexpr double kLfeTop = 120;      // Hz: the LFE takes what is under this, fading out over the 40 Hz above
// Each destination's three all-pass delays (ms), all different, so no two speakers behind sound alike.
constexpr double kAllPassMs[8][3] = {{4.7, 8.9, 13.1}, {5.3, 9.7, 12.3}, {6.1, 7.9, 14.3}, {3.9, 10.3, 11.9},
                                     {4.3, 9.1, 13.7}, {5.9, 8.3, 12.7}, {6.7, 7.3, 14.9}, {3.7, 10.9, 11.3}};
constexpr float kAllPassGain = 0.5f;

}  // namespace

float Upmixer::AllPass::run(float x) {
    if (line.empty()) return x;
    const float delayed = line[at];
    const float v = x + kAllPassGain * delayed;
    line[at] = v;
    if (++at == line.size()) at = 0;
    return delayed - kAllPassGain * v;
}

Upmixer::Upmixer(std::string layout, int rate) : layout_(std::move(layout)), rate_(std::max(1, rate)) {
    nch_ = layoutChannels(layout_);
    ambisonic_ = ambisonicLayout(layout_);
    const auto& sp = layoutSpeakers(layout_);
    if (!ambisonic_)
        for (size_t c = 0; c < sp.size(); ++c) {
            const std::string name = sp[c].name;
            if (sp[c].lfe) lfeCh_ = int(c);
            else if (name == "C") centreCh_ = int(c);
            else if (name == "L") leftCh_ = int(c);
            else if (name == "R") rightCh_ = int(c);
        }
    window_.resize(kFft);
    for (int i = 0; i < kFft; ++i) window_[size_t(i)] = float(std::sqrt(0.5 - 0.5 * std::cos(2 * M_PI * i / kFft)));

    // What each channel adds to the fold-down's left and right.
    std::vector<std::pair<float, float>> fold(static_cast<size_t>(nch_));
    for (int c = 0; c < nch_; ++c) {
        std::vector<float> unit(size_t(nch_), 0.0f);
        unit[size_t(c)] = 1;
        float lr[2];
        downmixToStereo(layout_, unit.data(), 1, lr);
        fold[size_t(c)] = {lr[0], lr[1]};
    }
    auto unitAt = [&](int c) {
        std::vector<float> g(size_t(nch_), 0.0f);
        if (c >= 0) g[size_t(c)] = 1;
        return g;
    };

    // The direct sound's place: amplitudes p (left) and q (right) in the stereo, p² + q² = 1.
    narrow_.assign(kPlaces, std::vector<float>(size_t(nch_), 0.0f));
    wide_ = narrow_;
    for (int i = 0; i < kPlaces; ++i) {
        const double t = double(i) / (kPlaces - 1), theta = t * M_PI / 2, p = std::cos(theta), q = std::sin(theta);
        auto& n = narrow_[size_t(i)];
        auto& w = wide_[size_t(i)];
        if (ambisonic_) {
            n = panGains(layout_, -30 + 60 * t, 1, 0);
            w = panGains(layout_, -90 + 180 * t, 1, 0);
        } else if (centreCh_ >= 0 && leftCh_ >= 0 && rightCh_ >= 0) {
            // Left, centre and right such that the fold-down (the centre 3 dB down into each side) is p and q.
            if (p >= q) {
                n[size_t(leftCh_)] = float(p - q);
                n[size_t(centreCh_)] = float(std::sqrt(2.0) * q);
            } else {
                n[size_t(rightCh_)] = float(q - p);
                n[size_t(centreCh_)] = float(std::sqrt(2.0) * p);
            }
            w[size_t(leftCh_)] = float(p);
            w[size_t(rightCh_)] = float(q);
        } else if (nch_ >= 2) {
            n[0] = w[0] = float(p);
            n[1] = w[1] = float(q);
        }
    }
    if (ambisonic_) {
        frontLeft_ = panGains(layout_, -30, 1, 0);
        frontRight_ = panGains(layout_, 30, 1, 0);
        dialogueGains_ = panGains(layout_, 0, 1, 0);
    } else {
        frontLeft_ = unitAt(leftCh_ >= 0 ? leftCh_ : 0);
        frontRight_ = unitAt(rightCh_ >= 0 ? rightCh_ : std::min(1, nch_ - 1));
        if (centreCh_ >= 0) dialogueGains_ = unitAt(centreCh_);
        else {  // (stereo: both sides, which fold back to the same)
            dialogueGains_.assign(size_t(nch_), 0.0f);
            for (int c = 0; c < std::min(2, nch_); ++c) dialogueGains_[size_t(c)] = float(std::sqrt(0.5));
        }
    }
    for (int c = 0; c < nch_; ++c) {
        bool used = false;
        for (int i = 0; i < kPlaces; ++i) used = used || narrow_[size_t(i)][size_t(c)] != 0 || wide_[size_t(i)][size_t(c)] != 0;
        if (used || frontLeft_[size_t(c)] != 0 || frontRight_[size_t(c)] != 0) front_.push_back(c);
    }

    // Behind and above: each side's speakers there (or, in a field, a direction each side, at the ear and 45° up).
    if (ambisonic_) {
        for (int above = 0; above < 2; ++above)
            for (int side = 0; side < 2; ++side) {
                Destination d;
                d.side = side;
                d.above = above;
                d.gains = panGains(layout_, side ? 110 : -110, 1, above ? 0.5 : 0);
                dest_.push_back(std::move(d));
            }
    } else if (nch_ > 2) {
        for (size_t c = 0; c < sp.size(); ++c) {
            if (sp[c].lfe) continue;
            const bool above = sp[c].elevation > 0;
            if (!above && std::fabs(sp[c].angle) < 60) continue;
            Destination d;
            d.side = sp[c].angle < 0 ? 0 : 1;
            d.above = above;
            d.gains = unitAt(int(c));
            dest_.push_back(std::move(d));
        }
    }
    // Each one's share of its side's power behind (or above), divided by what the fold-down keeps of it, so the
    // fold-down hears the ambient sound at the level it had.
    for (Destination& d : dest_) {
        int peers = 0;
        for (const Destination& o : dest_) peers += o.side == d.side && o.above == d.above;
        double fl = 0, fr = 0;
        for (int c = 0; c < nch_; ++c) {
            fl += d.gains[size_t(c)] * fold[size_t(c)].first;
            fr += d.gains[size_t(c)] * fold[size_t(c)].second;
        }
        const double kept = fl * fl + fr * fr;
        d.share = kept > 1e-6 ? float(1.0 / (peers * kept)) : 0.0f;
    }
    reset();
}

void Upmixer::reset() {
    for (auto* v : {&inL_, &inR_, &dlgL_, &dlgR_}) v->assign(size_t(kFft + kHop), 0.0f);
    sinceFrame_ = 0;
    const size_t bins = kFft / 2 + 1;
    pLL_.assign(bins, 0.0);
    pRR_.assign(bins, 0.0);
    pLR_.assign(bins, 0.0);
    primed_ = false;
    acc_.assign(front_.size() + 3, std::vector<float>(size_t(kFft), 0.0f));
    const size_t stride = size_t(nch_) + 2;
    ready_.assign(size_t(kHop) * stride, 0.0f);  // the first hop's worth comes out silent: the latency
    readAt_ = 0;
    const size_t longest = size_t(0.05 * rate_) + 1;
    for (auto& line : delayLine_) line.assign(longest, 0.0f);
    delayAt_ = 0;
    for (size_t j = 0; j < dest_.size(); ++j)
        for (int k = 0; k < 3; ++k) {
            AllPass& a = dest_[j].stages[k];
            a.line.assign(size_t(std::max(1L, std::lround(kAllPassMs[j % 8][k] * rate_ / 1000))), 0.0f);
            a.at = 0;
        }
}

void Upmixer::process(const float* stereo, const float* dialogue, int frames, const UpmixSettings& s, float* out,
                      float* delayedStereo) {
    const size_t stride = size_t(nch_) + 2;
    for (int i = 0; i < frames; ++i) {
        const size_t at = size_t(kFft + sinceFrame_);
        inL_[at] = stereo[2 * size_t(i)];
        inR_[at] = stereo[2 * size_t(i) + 1];
        dlgL_[at] = dialogue ? dialogue[2 * size_t(i)] : 0.0f;
        dlgR_[at] = dialogue ? dialogue[2 * size_t(i) + 1] : 0.0f;
        if (++sinceFrame_ == kHop) {
            for (auto* v : {&inL_, &inR_, &dlgL_, &dlgR_}) std::memmove(v->data(), v->data() + kHop, sizeof(float) * kFft);
            sinceFrame_ = 0;
            frame(s);
        }
        const float* f = &ready_[readAt_];
        std::copy(f, f + nch_, out + size_t(i) * size_t(nch_));
        if (delayedStereo) {
            delayedStereo[2 * size_t(i)] = f[nch_];
            delayedStereo[2 * size_t(i) + 1] = f[nch_ + 1];
        }
        readAt_ += stride;
    }
    if (readAt_ > 0) {
        ready_.erase(ready_.begin(), ready_.begin() + std::ptrdiff_t(readAt_));
        readAt_ = 0;
    }
}

void Upmixer::frame(const UpmixSettings& s) {
    const int bins = kFft / 2 + 1;
    // Both channels in one transform: left as the real part, right as the imaginary.
    std::vector<std::complex<float>> z(kFft);
    for (int n = 0; n < kFft; ++n) z[size_t(n)] = {inL_[size_t(n)] * window_[size_t(n)], inR_[size_t(n)] * window_[size_t(n)]};
    fftInPlace(z, false);
    const double alpha = primed_ ? std::exp(-double(kHop) / (kSmoothing * rate_)) : 0.0;
    primed_ = true;
    const double width = std::clamp(s.centreWidth, 0.0, 1.0);
    const double bias = std::min(0.5, kCoherenceBias * 48000 / rate_);
    const bool lcr = !ambisonic_ && centreCh_ >= 0 && leftCh_ >= 0 && rightCh_ >= 0;
    const double depth = dest_.empty() ? 0.0 : std::clamp(s.surroundDepth, 0.0, 1.0);
    const float frontAmbient = float(std::sqrt(1 - depth));
    const size_t nf = front_.size();
    // The spectra made: each front channel, then the ambient left and right, then the LFE.
    std::vector<std::vector<std::complex<float>>> y(nf + 3, std::vector<std::complex<float>>(size_t(bins)));
    const double df = double(rate_) / kFft;
    std::vector<std::complex<double>> xs(size_t(bins) * 2);
    for (int k = 0; k < bins; ++k) {
        const std::complex<float> zk = z[size_t(k)], zn = std::conj(z[size_t((kFft - k) % kFft)]);
        const std::complex<double> xl((zk + zn) * 0.5f), xr((zk - zn) * std::complex<float>(0, -0.5f));
        xs[2 * size_t(k)] = xl;
        xs[2 * size_t(k) + 1] = xr;
        pLL_[size_t(k)] = alpha * pLL_[size_t(k)] + (1 - alpha) * std::norm(xl);
        pRR_[size_t(k)] = alpha * pRR_[size_t(k)] + (1 - alpha) * std::norm(xr);
        pLR_[size_t(k)] = alpha * pLR_[size_t(k)] + (1 - alpha) * xl * std::conj(xr);
    }
    // Averaged over the neighbouring bins too (running sums).
    std::vector<double> sLL(size_t(bins) + 1, 0.0), sRR(size_t(bins) + 1, 0.0);
    std::vector<std::complex<double>> sLR(size_t(bins) + 1);
    for (int k = 0; k < bins; ++k) {
        sLL[size_t(k) + 1] = sLL[size_t(k)] + pLL_[size_t(k)];
        sRR[size_t(k) + 1] = sRR[size_t(k)] + pRR_[size_t(k)];
        sLR[size_t(k) + 1] = sLR[size_t(k)] + pLR_[size_t(k)];
    }
    for (int k = 0; k < bins; ++k) {
        const std::complex<double> xl = xs[2 * size_t(k)], xr = xs[2 * size_t(k) + 1];
        const size_t lo = size_t(std::max(0, k - kSpread)), hi = size_t(std::min(bins, k + kSpread + 1));
        const double ll = sLL[hi] - sLL[lo], rr = sRR[hi] - sRR[lo];
        const double lr2 = std::max(0.0, std::norm(sLR[hi] - sLR[lo]) - bias * ll * rr) / (1 - bias);
        const double ambient = std::max(0.0, (ll + rr - std::sqrt((ll - rr) * (ll - rr) + 4 * lr2)) / 2);
        const double dl = std::max(0.0, ll - ambient), dr = std::max(0.0, rr - ambient);
        const std::complex<double> directL = xl * (ll > 1e-20 ? dl / ll : 0.0), directR = xr * (rr > 1e-20 ? dr / rr : 0.0);
        const std::complex<double> ambientL = xl - directL, ambientR = xr - directR;
        // The direct sound as one: its power, the phase of the two together (or the louder's where they cancel).
        const double power = std::norm(directL) + std::norm(directR);
        std::complex<double> source;
        if (power > 1e-24) {
            const std::complex<double> sum = directL + directR;
            const std::complex<double> phase = std::norm(sum) > 0.01 * power ? sum : std::norm(directL) >= std::norm(directR) ? directL : directR;
            source = phase / std::abs(phase) * std::sqrt(power);
        }
        const double place = (dl + dr > 1e-24 ? std::atan2(std::sqrt(dr), std::sqrt(dl)) / (M_PI / 2) : 0.5) * (kPlaces - 1);
        const int i0 = std::min(kPlaces - 2, int(place));
        const double u = place - i0;
        // Left, centre and right: what both sides have in common, in phase, goes to the centre and is taken from each
        // side as much as the centre folds back into it, so the fold-down is the direct sound exactly, whatever the
        // phase or time between the sides (out of phase, it stays at the sides).
        std::complex<double> common;
        if (lcr) {
            const double al = std::abs(directL), ar = std::abs(directR), as = std::abs(directL + directR);
            if (al > 0 && ar > 0 && as > 0) {
                const double inPhase = std::max(0.0, std::real(directL * std::conj(directR)) / (al * ar));
                common = (directL + directR) / as * (std::min(al, ar) * inPhase * (1 - width));
            }
        }
        for (size_t j = 0; j < nf; ++j) {
            const size_t c = size_t(front_[j]);
            const std::complex<double> ambientHere = (ambientL * double(frontLeft_[c]) + ambientR * double(frontRight_[c])) * double(frontAmbient);
            if (lcr) {
                const std::complex<double> direct = int(c) == centreCh_ ? common * std::sqrt(2.0)
                                                    : int(c) == leftCh_ ? directL - common
                                                    : int(c) == rightCh_ ? directR - common
                                                                         : std::complex<double>();
                y[j][size_t(k)] = std::complex<float>(direct + ambientHere);
                continue;
            }
            const double n = narrow_[size_t(i0)][c] * (1 - u) + narrow_[size_t(i0 + 1)][c] * u;
            const double w = wide_[size_t(i0)][c] * (1 - u) + wide_[size_t(i0 + 1)][c] * u;
            const double g = n * (1 - width) + w * width;
            y[j][size_t(k)] = std::complex<float>(source * g + ambientHere);
        }
        y[nf][size_t(k)] = std::complex<float>(ambientL);
        y[nf + 1][size_t(k)] = std::complex<float>(ambientR);
        if (s.lfe && lfeCh_ >= 0) {
            const double hz = k * df;
            const double g = hz <= kLfeTop - 20 ? 1.0 : hz >= kLfeTop + 20 ? 0.0 : 0.5 + 0.5 * std::cos((hz - (kLfeTop - 20)) / 40 * M_PI);
            y[nf + 2][size_t(k)] = std::complex<float>((xl + xr) * (g * std::sqrt(0.5)));
        }
    }
    // Back to sound two spectra at a time (one as the real part, one as the imaginary), windowed and overlap-added
    // (square-root Hann twice at 75 % overlap sums to 2).
    const size_t streams = s.lfe && lfeCh_ >= 0 ? nf + 3 : nf + 2;
    const float scale = 0.5f / kFft;
    for (size_t a = 0; a < streams; a += 2) {
        const size_t b = a + 1 < streams ? a + 1 : streams;
        for (int k = 0; k < kFft; ++k) {
            const int m = k <= kFft / 2 ? k : kFft - k;
            std::complex<float> ya = y[a][size_t(m)], yb = b < streams ? y[b][size_t(m)] : std::complex<float>();
            if (m == 0 || m == kFft / 2) ya = ya.real(), yb = yb.real();
            if (k > kFft / 2) ya = std::conj(ya), yb = std::conj(yb);
            z[size_t(k)] = ya + std::complex<float>(0, 1) * yb;
        }
        fftInPlace(z, true);
        for (int n = 0; n < kFft; ++n) {
            const float w = window_[size_t(n)] * scale;
            acc_[a][size_t(n)] += z[size_t(n)].real() * w;
            if (b < streams) acc_[b][size_t(n)] += z[size_t(n)].imag() * w;
        }
    }
    // The first hop is finished: it is the window's first hop of input too (the dialogue and the delayed stereo).
    const size_t stride = size_t(nch_) + 2;
    const size_t first = ready_.size();
    ready_.resize(first + size_t(kHop) * stride, 0.0f);
    const int delay = std::clamp(int(std::lround(s.delayMs * rate_ / 1000)), 0, int(delayLine_[0].size()) - 1);
    bool above = false;
    for (const Destination& d : dest_) above = above || d.above;
    const double height = above ? std::clamp(s.height, 0.0, 1.0) : 0.0;
    std::vector<float> level(dest_.size());
    for (size_t j = 0; j < dest_.size(); ++j) level[j] = float(std::sqrt(depth * dest_[j].share * (dest_[j].above ? height : 1 - height)));
    for (int n = 0; n < kHop; ++n) {
        float* o = &ready_[first + size_t(n) * stride];
        for (size_t j = 0; j < nf; ++j) o[front_[j]] = acc_[j][size_t(n)];
        if (s.lfe && lfeCh_ >= 0) o[lfeCh_] += acc_[nf + 2][size_t(n)];
        const size_t len = delayLine_[0].size();
        for (int side = 0; side < 2; ++side) delayLine_[side][delayAt_] = acc_[nf + size_t(side)][size_t(n)];
        const size_t from = (delayAt_ + len - size_t(delay)) % len;
        delayAt_ = (delayAt_ + 1) % len;
        for (size_t j = 0; j < dest_.size(); ++j) {
            Destination& d = dest_[j];
            float v = delayLine_[d.side][from];
            for (AllPass& a : d.stages) v = a.run(v);
            v *= level[j];
            if (v != 0)
                for (int c = 0; c < nch_; ++c) o[c] += v * d.gains[size_t(c)];
        }
        const float dl = dlgL_[size_t(n)], dr = dlgR_[size_t(n)];
        if (dl != 0 || dr != 0) {
            const float mono = (dl + dr) * float(std::sqrt(0.5));
            for (int c = 0; c < nch_; ++c) o[c] += mono * dialogueGains_[size_t(c)];
        }
        o[nch_] = inL_[size_t(n)] + dl;
        o[nch_ + 1] = inR_[size_t(n)] + dr;
    }
    for (auto& a : acc_) {
        std::memmove(a.data(), a.data() + kHop, sizeof(float) * (kFft - kHop));
        std::fill(a.end() - kHop, a.end(), 0.0f);
    }
}

}  // namespace montage
