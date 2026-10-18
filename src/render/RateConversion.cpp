#include "RateConversion.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "Exporter.h"

namespace montage {

namespace {

bool near(Rational r, double v) { return r.valid() && std::fabs(r.toDouble() - v) < 1e-3; }

// a * b / c for programme and delivered frame counts (rounded to the nearest).
int64_t scaled(int64_t v, int64_t mul, int64_t div) {
    if (div <= 0) return v;
    const long double x = (long double)v * (long double)mul / (long double)div;
    return int64_t(std::llround(double(x)));
}

}  // namespace

double RateConversion::speed() const { return mode == Mode::Speed && from.valid() && to.valid() ? to.toDouble() / from.toDouble() : 1.0; }

void RateConversion::speedRatio(int64_t& num, int64_t& den) const {
    if (mode != Mode::Speed || !from.valid() || !to.valid()) {
        num = den = 1;
        return;
    }
    num = int64_t(to.num) * from.den;
    den = int64_t(to.den) * from.num;
    const int64_t g = std::gcd(num, den);
    if (g > 1) num /= g, den /= g;
}

bool planRateConversion(const Sequence& seq, const ExportSettings& s, RateConversion& out, std::string* error) {
    out = RateConversion{};
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (s.rateChange.empty()) return true;
    out.from = seq.fps;
    out.to = s.rate;
    out.keepPitch = s.rateKeepPitch;
    if (s.videoCodec == "gif") return fail("An animated GIF has a frame rate of its own: leave the frame rate as it is");
    if (s.rateChange == "pulldown") {
        out.mode = RateConversion::Mode::Pulldown;
        const bool film = near(seq.fps, 24000.0 / 1001.0) || near(seq.fps, 24.0);
        if (!film) return fail("Pulldown is added to 23.976 or 24 frame sequences");
        if (s.cadence != "2:3" && s.cadence != "2:2:2:4") return fail("The pulldown cadence is 2:3 or 2:2:2:4");
        out.cadence2224 = s.cadence == "2:2:2:4";
        const double f = seq.fps.toDouble();
        if (!out.to.valid()) out.to = near(seq.fps, 24.0) ? Rational{30, 1} : Rational{30000, 1001};
        if (std::fabs(out.to.toDouble() - f * 1.25) < 1e-3) out.fields = true;
        else if (std::fabs(out.to.toDouble() - f * 2.5) >= 1e-3)
            return fail("With pulldown a " + std::string(near(seq.fps, 24.0) ? "24" : "23.976") + " sequence goes out at " +
                        (near(seq.fps, 24.0) ? "30 (interlaced) or 60" : "29.97 (interlaced) or 59.94"));
        return true;
    }
    if (s.rateChange != "speed" && s.rateChange != "convert")
        return fail("Unknown frame rate change \"" + s.rateChange + "\" (pulldown, speed or convert)");
    out.mode = s.rateChange == "speed" ? RateConversion::Mode::Speed : RateConversion::Mode::Convert;
    if (!out.to.valid() || out.to.toDouble() < 1 || out.to.toDouble() > 240) return fail("Give the frame rate to deliver at (1 to 240)");
    if (out.to == seq.fps || std::fabs(out.to.toDouble() - seq.fpsValue()) < 1e-6) return fail("The sequence is already at that frame rate");
    if (out.mode == RateConversion::Mode::Speed) {
        const double k = out.to.toDouble() / seq.fpsValue();
        if (k < 0.5 || k > 2) return fail("A speed change goes from half to twice the speed");
    }
    return true;
}

int64_t deliveredFrames(const RateConversion& c, int64_t frames) {
    if (frames <= 0) return 0;
    switch (c.mode) {
        case RateConversion::Mode::None:
        case RateConversion::Mode::Speed: return frames;
        case RateConversion::Mode::Pulldown: return c.fields ? scaled(frames, 5, 4) : scaled(frames, 5, 2);
        case RateConversion::Mode::Convert:
            return std::max<int64_t>(1, scaled(frames, int64_t(c.to.num) * c.from.den, int64_t(c.to.den) * c.from.num));
    }
    return frames;
}

int64_t deliveredFrameOf(const RateConversion& c, int64_t f) {
    switch (c.mode) {
        case RateConversion::Mode::None:
        case RateConversion::Mode::Speed: return f;
        case RateConversion::Mode::Pulldown: {
            // The first delivered frame showing it (in fields A A, B B, B C, C D: each the frame where its first field is).
            const int64_t g = f >= 0 ? f / 4 : -((-f + 3) / 4), r = f - g * 4;
            if (c.fields) return g * 5 + r;
            static const int64_t at23[4] = {0, 2, 5, 7}, at2224[4] = {0, 2, 4, 6};
            return g * 10 + (c.cadence2224 ? at2224[r] : at23[r]);
        }
        case RateConversion::Mode::Convert: return scaled(f, int64_t(c.to.num) * c.from.den, int64_t(c.to.den) * c.from.num);
    }
    return f;
}

DeliveredPicture deliveredPicture(const RateConversion& c, int64_t o, int64_t frames) {
    DeliveredPicture d;
    const int64_t last = std::max<int64_t>(0, frames - 1);
    auto clampTo = [&](int64_t v) { return std::clamp<int64_t>(v, 0, last); };
    switch (c.mode) {
        case RateConversion::Mode::None:
        case RateConversion::Mode::Speed: d.a = d.b = clampTo(o); break;
        case RateConversion::Mode::Pulldown: {
            if (c.fields) {
                const int64_t base = (o / 5) * 4;
                const int j = int(o % 5);
                if (c.cadence2224) {
                    d.a = d.b = clampTo(base + std::min(j, 3));  // A A, B B, C C, D D, D D
                } else {
                    static const int first[5] = {0, 1, 1, 2, 3}, second[5] = {0, 1, 2, 3, 3};  // A A, B B, B C, C D, D D
                    d.a = clampTo(base + first[j]);
                    d.b = clampTo(base + second[j]);
                    d.woven = d.a != d.b;
                }
            } else {
                // Each frame repeated: 2, 3, 2, 3 times (2:3) or 2, 2, 2, 4.
                static const int at23[10] = {0, 0, 1, 1, 1, 2, 2, 3, 3, 3}, at2224[10] = {0, 0, 1, 1, 2, 2, 3, 3, 3, 3};
                const int64_t base = (o / 10) * 4;
                const int j = int(o % 10);
                d.a = d.b = clampTo(base + (c.cadence2224 ? at2224[j] : at23[j]));
            }
            break;
        }
        case RateConversion::Mode::Convert: {
            // Programme position of the delivered frame: o * from / to, exactly where it lands on a frame.
            const int64_t num = o * int64_t(c.from.num) * c.to.den, den = int64_t(c.from.den) * c.to.num;
            const int64_t whole = den > 0 ? num / den : o;
            d.a = clampTo(whole);
            d.t = den > 0 ? double(num - whole * den) / double(den) : 0.0;
            d.b = clampTo(whole + 1);
            if (d.b == d.a || d.t < 1e-9) d.t = 0, d.b = d.a;
            break;
        }
    }
    return d;
}

Sequence deliveredSequence(const Sequence& seq, const RateConversion& c, FrameTime in, FrameTime out) {
    Sequence d = seq;
    if (!c.active()) return d;
    d.fps = c.to;
    // (the range's end is the delivered end, which rounds the last frames up)
    auto at = [&](FrameTime f) { return f >= out ? deliveredFrames(c, out - in) : deliveredFrameOf(c, f - in); };
    for (TrackRef r : allTracks(d))
        for (Clip& clip : trackAt(d, r)->clips) {
            const FrameTime start = deliveredFrameOf(c, clip.start - in), end = at(clip.start + clip.duration);
            clip.start = start;
            clip.duration = std::max<FrameTime>(1, end - start);
        }
    for (CaptionTrack& t : d.captionTracks) {
        std::vector<Caption> kept;
        for (Caption cap : t.captions) {
            if (cap.end <= in || cap.start >= out) continue;
            cap.start = at(std::max(cap.start, in));
            cap.end = std::max(cap.start + 1, at(std::min(cap.end, out)));
            kept.push_back(std::move(cap));
        }
        t.captions = std::move(kept);
    }
    std::vector<Marker> markers;
    for (Marker m : d.markers) {
        if (m.t < in || m.t >= out) continue;
        const FrameTime e = m.duration > 0 ? at(std::min(m.t + m.duration, out)) : 0;
        m.t = at(m.t);
        if (m.duration > 0) m.duration = std::max<FrameTime>(1, e - m.t);
        markers.push_back(std::move(m));
    }
    d.markers = std::move(markers);
    d.inPoint = d.outPoint = -1;
    d.playhead = 0;
    return d;
}

bool editBetween(const Sequence& seq, FrameTime f) {
    for (const Track& t : seq.videoTracks) {
        if (t.muted) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || (c.start != f && c.start + c.duration != f)) continue;
            bool covered = false;
            for (const Transition& tr : t.transitions) {
                const Clip* a = nullptr;
                for (const Clip& x : t.clips)
                    if (x.id == tr.clipA || (tr.clipA == 0 && x.id == tr.clipB)) a = &x;
                if (!a) continue;
                // (centred on the edit: the outgoing clip's end, or the incoming one's start for a fade in)
                const FrameTime edit = tr.clipA ? a->start + a->duration : a->start;
                covered |= edit == f && tr.duration > 0;
            }
            if (!covered) return true;
        }
    }
    return false;
}

ShotLook shotLook(const Image& img) {
    constexpr int kW = 64, kH = 36, kBins = 16;
    ShotLook s;
    s.hist.assign(kBins * 3, 0.0f);
    s.luma.assign(size_t(kW) * kH, 0.0f);
    if (img.empty()) return s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            // (the middle of each cell)
            const int sx = std::min(img.width - 1, int((x + 0.5) * img.width / kW));
            const int sy = std::min(img.height - 1, int((y + 0.5) * img.height / kH));
            const float* p = img.at(sx, sy);
            const float r = std::clamp(p[0], 0.0f, 1.0f), g = std::clamp(p[1], 0.0f, 1.0f), b = std::clamp(p[2], 0.0f, 1.0f);
            const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float cb = (b - l) / 1.8556f + 0.5f, cr = (r - l) / 1.5748f + 0.5f;
            s.luma[size_t(y) * kW + size_t(x)] = l;
            // (shared between the two nearest bins, so a flat picture changing a little is not taken for a new one)
            auto add = [&](int at, float v) {
                const float pos = std::clamp(v * kBins - 0.5f, 0.0f, float(kBins - 1));
                const int lo = std::min(int(pos), kBins - 2);
                s.hist[size_t(at + lo)] += 1 - (pos - float(lo));
                s.hist[size_t(at + lo + 1)] += pos - float(lo);
            };
            add(0, l);
            add(kBins, cb);
            add(2 * kBins, cr);
        }
    for (float& v : s.hist) v /= float(kW * kH);
    return s;
}

double shotDifference(const ShotLook& a, const ShotLook& b) {
    if (a.hist.size() != b.hist.size() || a.luma.size() != b.luma.size() || a.luma.empty()) return 0;
    double h = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) h += std::fabs(a.hist[i] - b.hist[i]);
    h /= 6.0;  // three histograms, each L1 distance <= 2
    double p = 0;
    for (size_t i = 0; i < a.luma.size(); ++i) p += std::fabs(a.luma[i] - b.luma[i]);
    p /= double(a.luma.size());
    return std::clamp(0.6 * h + 0.4 * std::min(1.0, p * 3.0), 0.0, 1.0);
}

bool looksLikeCut(const ShotLook& a, const ShotLook& b, const ShotLook* before, const ShotLook* after) {
    const double d = shotDifference(a, b);
    double around = 0;
    if (before) around = std::max(around, shotDifference(*before, a));
    if (after) around = std::max(around, shotDifference(b, *after));
    return d > 0.2 && d > around * 3 + 0.05;
}

}  // namespace montage
