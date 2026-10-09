#include "AudioReactive.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <complex>

#include "core/Automation.h"
#include "core/Effects.h"
#include "media/MediaPool.h"

namespace montage {

std::vector<float> trackSoundAt(const Project& p, const Sequence& seq, int track, double t, int samples, int rate) {
    std::vector<float> out(size_t(std::max(0, samples)), 0.0f);
    const double fps = seq.fpsValue();
    for (size_t k = 0; k < seq.audioTracks.size(); ++k) {
        if (track > 0 && int(k) != track - 1) continue;
        const Track& tr = seq.audioTracks[k];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips) {
            if (!c.enabled || t < double(c.start) || t >= double(c.end()) || !c.mediaId) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !m->hasAudio || m->path.empty()) continue;
            AudioBufferPtr buf = MediaPool::instance().audio(audioKey(m->path, c.channels), rate);
            if (!buf || buf->samples.empty()) continue;
            const int64_t n = buf->frames();
            const double centre = c.sourceAt(t - double(c.start)) * rate / fps;
            const double step = c.ramped() ? c.speedAt(t - double(c.start)) : c.speed;
            for (int i = 0; i < samples; ++i) {
                const double pos = c.reverse ? centre - (i - samples / 2) * step : centre + (i - samples / 2) * step;
                const int64_t s = int64_t(pos);
                if (s < 0 || s >= n) continue;
                out[size_t(i)] += 0.5f * (buf->samples[size_t(s) * 2] + buf->samples[size_t(s) * 2 + 1]);
            }
        }
    }
    return out;
}

std::vector<float> spectrum(const std::vector<float>& x) {
    const size_t n = x.size();
    if (n < 2 || (n & (n - 1))) return {};
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n - 1)));
    // Iterative radix-2.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wn = 1;
            for (size_t k = 0; k < len / 2; ++k, wn *= w) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wn;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
    std::vector<float> mag(n / 2);
    for (size_t i = 0; i < n / 2; ++i) mag[i] = float(std::abs(a[i]) * 4.0 / double(n));  // a full-scale sine reads about 1
    return mag;
}

namespace {

constexpr int kRate = 48000;

// Bar heights 0..1, log-spaced from 40 Hz to 16 kHz, in dB over a 60 dB range.
std::vector<double> barLevels(const std::vector<float>& mag, int bars, double gainDb) {
    std::vector<double> v(size_t(bars), 0.0);
    if (mag.empty()) return v;
    const double binHz = double(kRate) / double(mag.size() * 2);
    for (int b = 0; b < bars; ++b) {
        const double f0 = 40 * std::pow(400.0, double(b) / bars), f1 = 40 * std::pow(400.0, double(b + 1) / bars);
        const size_t i0 = std::min(mag.size() - 1, size_t(std::max(1.0, std::floor(f0 / binHz))));
        const size_t i1 = std::max(i0 + 1, std::min(mag.size(), size_t(std::ceil(f1 / binHz))));
        double peak = 0;
        for (size_t i = i0; i < i1; ++i) peak = std::max(peak, double(mag[i]));
        const double db = 20 * std::log10(std::max(peak, 1e-9)) + gainDb;
        v[size_t(b)] = std::clamp((db + 60) / 60, 0.0, 1.0);
    }
    return v;
}

size_t powerOfTwoFor(double ms) {
    size_t n = 256;
    while (double(n) < ms / 1000.0 * kRate && n < 16384) n <<= 1;
    return n;
}

}  // namespace

Image renderAudioVisualiser(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h) {
    const Effect& g = c.generator;
    const FrameTime lt = t - c.start;
    QImage qi(w, h, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        pa.setOpacity(std::clamp(g.p("opacity", lt, 100) / 100.0, 0.0, 1.0));
        const QColor col = QColor::fromRgbF(float(std::clamp(g.p("color.r", lt, 1), 0.0, 1.0)), float(std::clamp(g.p("color.g", lt, 1), 0.0, 1.0)),
                                            float(std::clamp(g.p("color.b", lt, 1), 0.0, 1.0)));
        const int style = int(std::lround(g.p("style", lt, 0)));
        const int bars = std::clamp(int(std::lround(g.p("bars", lt, 32))), 4, 128);
        const double bw = w * std::clamp(g.p("width", lt, 80), 5.0, 100.0) / 100.0, bh = h * std::clamp(g.p("height", lt, 30), 5.0, 100.0) / 100.0;
        const double cx = w / 2.0, cy = h * std::clamp(g.p("center_y", lt, 70), 0.0, 100.0) / 100.0;
        const double gain = g.p("sensitivity", lt, 0);
        const size_t n = powerOfTwoFor(g.p("window", lt, 60));
        const std::vector<float> sound = trackSoundAt(p, seq, int(std::lround(g.p("track", lt, 1))), double(t), int(n), kRate);
        if (style == 2) {
            // The waveform across the width, centred on its line.
            QPainterPath path;
            const int points = std::max(16, int(bw / 2));
            for (int i = 0; i < points; ++i) {
                const size_t k = size_t(double(i) / (points - 1) * double(n - 1));
                const double v = std::clamp(double(sound[k]) * std::pow(10.0, gain / 20) * 2, -1.0, 1.0);
                const QPointF pt(cx - bw / 2 + bw * i / (points - 1), cy - v * bh / 2);
                i ? path.lineTo(pt) : path.moveTo(pt);
            }
            pa.setPen(QPen(col, std::max(1.0, h / 270.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            pa.drawPath(path);
        } else {
            const std::vector<double> level = barLevels(spectrum(sound), bars, gain);
            pa.setPen(Qt::NoPen);
            pa.setBrush(col);
            if (style == 3) {
                // Round a circle, each bar pointing out.
                const double r = bh * 0.25, len = bh * 0.25;
                const double thick = std::max(1.0, 2 * M_PI * r / bars * 0.6);
                for (int b = 0; b < bars; ++b) {
                    const double a = 2 * M_PI * b / bars - M_PI / 2, l = std::max(thick, len * level[size_t(b)]);
                    pa.save();
                    pa.translate(cx + r * std::cos(a), cy + r * std::sin(a));
                    pa.rotate(a * 180 / M_PI);
                    pa.drawRoundedRect(QRectF(0, -thick / 2, l, thick), thick / 2, thick / 2);
                    pa.restore();
                }
            } else {
                const double slot = bw / bars, thick = slot * 0.7;
                for (int b = 0; b < bars; ++b) {
                    const double l = std::max(thick * 0.5, bh * level[size_t(b)]);
                    const double x = cx - bw / 2 + slot * b + (slot - thick) / 2;
                    const QRectF r = style == 1 ? QRectF(x, cy - l / 2, thick, l) : QRectF(x, cy + bh / 2 - l, thick, l);
                    pa.drawRoundedRect(r, thick / 3, thick / 3);
                }
            }
        }
    }
    Image img(w, h);
    const float k = 1.0f / 255.0f;
    for (int y = 0; y < h; ++y) {
        const uchar* src = qi.constScanLine(y);
        float* d = img.row(y);
        for (int x = 0; x < w * 4; ++x) d[x] = src[x] * k;
    }
    return img;
}

namespace edit {

Result animateToAudio(Project& p, Sequence& s, Id clipId, Id effect, const std::string& param, int track, AudioBand band, double low,
                      double high) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    Effect* e = nullptr;
    if (effect == 0) {
        if (c->motion.empty()) c->motion = makeEffect(p, "transform");
        e = &c->motion;
    } else {
        for (Effect& x : c->effects)
            if (x.id == effect) e = &x;
        if (c->generator.id == effect && !c->generator.empty()) e = &c->generator;
    }
    if (!e) return Result::fail("No such effect on the clip");
    const EffectInfo* info = findEffectInfo(e->type);
    if (!info || std::none_of(info->params.begin(), info->params.end(), [&](const ParamInfo& pi) { return pi.name == param; }))
        return Result::fail("\"" + param + "\" is not a setting of " + (info ? info->displayName : e->type));
    if (track < 0 || track > int(s.audioTracks.size())) return Result::fail("No such audio track");
    // The level a frame: the band's energy over a 2048-sample window.
    const size_t n = 2048;
    const double binHz = double(kRate) / double(n);
    const double f0 = band == AudioBand::Low ? 20 : band == AudioBand::Mid ? 250 : band == AudioBand::High ? 4000 : 20;
    const double f1 = band == AudioBand::Low ? 250 : band == AudioBand::Mid ? 4000 : 20000;
    std::vector<double> level;
    for (FrameTime f = 0; f < c->duration; ++f) {
        const std::vector<float> mag = spectrum(trackSoundAt(p, s, track, double(c->start + f), int(n), kRate));
        double energy = 0;
        for (size_t i = size_t(f0 / binHz); i < mag.size() && double(i) * binHz < f1; ++i) energy += double(mag[i]) * mag[i];
        level.push_back(std::sqrt(energy));
    }
    const double loudest = level.empty() ? 0 : *std::max_element(level.begin(), level.end());
    // Below -60 dBFS (a full-scale sine is 1) there is nothing to follow.
    if (loudest < 1e-3) return Result::fail(band == AudioBand::All ? "The track is silent under the clip" : "The track has no sound in that band under the clip");
    Param& prm = e->params[param];
    prm.keys.clear();
    for (size_t f = 0; f < level.size(); ++f) prm.keys.push_back({FrameTime(f), low + (high - low) * level[f] / loudest});
    thinKeys(prm.keys, std::max(1e-6, std::fabs(high - low) * 0.01));
    return {};
}

}  // namespace edit

}  // namespace montage
