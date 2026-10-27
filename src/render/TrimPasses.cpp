#include "TrimPasses.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "ColorSpace.h"
#include "Compositor.h"

namespace montage {

namespace {

bool overlay(const Clip& c) {
    // Graphics over the picture (titles, shapes, OGraf, adjustment layers, waveforms) are not shots.
    return c.isGenerator() && c.generator.type != "color" && c.generator.type != "gradient" && c.generator.type != "bars";
}

double sourcePeak(const Sequence& s) {
    const ColorSpace& from = sequenceColorSpace(s);
    return from.transfer == Transfer::Pq ? std::clamp(s.hdrPeakNits, 100.0, 10000.0) : from.transfer == Transfer::Hlg ? 1000 : 100;
}

double targetPeak(const ColorSpace& space, double peakNits) {
    return space.transfer == Transfer::Pq ? std::clamp(peakNits, 100.0, 10000.0) : space.transfer == Transfer::Hlg ? 1000 : 100;
}

// BT.2390's EETF on PQ code values normalised to the source's peak.
double eetf(double e, double maxLum) {
    const double ks = std::max(0.0, 1.5 * maxLum - 0.5);
    if (e < ks) return e;
    const double t = (e - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * maxLum;
}

// Mid-tone detail: luma less a wide blur of it, at about 1/64 of the width.
std::vector<float> lumaDetail(const Image& img, const double w[3]) {
    const int W = img.width, H = img.height;
    std::vector<float> y(size_t(W) * size_t(H));
    for (int r = 0; r < H; ++r) {
        const float* p = img.row(r);
        for (int x = 0; x < W; ++x, p += 4) {
            const float a = p[3] > 0 ? p[3] : 1;
            y[size_t(r) * W + x] = float((w[0] * p[0] + w[1] * p[1] + w[2] * p[2]) / a);
        }
    }
    const int f = std::max(1, W / 128), sw = (W + f - 1) / f, sh = (H + f - 1) / f;
    std::vector<float> small(size_t(sw) * size_t(sh), 0), n(small.size(), 0);
    for (int r = 0; r < H; ++r)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(r / f) * sw + size_t(x / f);
            small[i] += y[size_t(r) * W + x];
            n[i] += 1;
        }
    for (size_t i = 0; i < small.size(); ++i) small[i] /= std::max(1.0f, n[i]);
    // Twice a 5 x 5 box: near a Gaussian.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> out(small.size());
        for (int r = 0; r < sh; ++r)
            for (int x = 0; x < sw; ++x) {
                float acc = 0;
                int count = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int yy = r + dy, xx = x + dx;
                        if (yy < 0 || yy >= sh || xx < 0 || xx >= sw) continue;
                        acc += small[size_t(yy) * sw + xx];
                        ++count;
                    }
                out[size_t(r) * sw + x] = acc / float(std::max(1, count));
            }
        small = std::move(out);
    }
    for (int r = 0; r < H; ++r) {
        const double fy = std::clamp((r + 0.5) / f - 0.5, 0.0, double(sh - 1));
        const int y0 = int(fy), y1 = std::min(sh - 1, y0 + 1);
        const double ty = fy - y0;
        for (int x = 0; x < W; ++x) {
            const double fx = std::clamp((x + 0.5) / f - 0.5, 0.0, double(sw - 1));
            const int x0 = int(fx), x1 = std::min(sw - 1, x0 + 1);
            const double tx = fx - x0;
            const double top = small[size_t(y0) * sw + x0] * (1 - tx) + small[size_t(y0) * sw + x1] * tx;
            const double bottom = small[size_t(y1) * sw + x0] * (1 - tx) + small[size_t(y1) * sw + x1] * tx;
            float& v = y[size_t(r) * W + x];
            v -= float(top * (1 - ty) + bottom * ty);
        }
    }
    return y;
}

}  // namespace

const std::vector<TrimTarget>& trimTargetPresets() {
    static const std::vector<TrimTarget> presets{
        {"sdr", "SDR Rec.709 (100 nits)", "rec709", 100},
        {"hdr10-1000", "HDR10 1000 nits", "rec2100pq", 1000},
        {"hdr10-600", "HDR10 600 nits", "rec2100pq", 600},
        {"hdr10-300", "HDR10 300 nits", "rec2100pq", 300},
        {"hlg", "HLG (1000 nits)", "rec2100hlg", 1000},
    };
    return presets;
}

const TrimTarget* findTrimTarget(const Sequence& s, const std::string& id) {
    for (const TrimTarget& t : s.trimTargets)
        if (t.id == id) return &t;
    return nullptr;
}

const ColorSpace* trimTargetDisplay(const Sequence& s, const TrimTarget& t) {
    const ColorSpace* c = findColorSpace(t.colorSpace);
    ToneMapping tm = ToneMapping::Montage;
    if (c && !sceneReferredWorking(s) && parseToneMapping(t.toneMapping, tm)) c = &withToneMapping(*c, tm);
    return c;
}

const ColorSpace& trimTargetSpace(const Sequence& s, const std::string& id) {
    if (const TrimTarget* t = findTrimTarget(s, id))
        if (const ColorSpace* c = trimTargetDisplay(s, *t)) return *c;
    return sequenceColorSpace(s);
}

double sdrHeadroomNits(const ColorSpace& sdr) {
    const double white = toneMappingWhiteNits(sdr.toneMapping);
    return white > 0 ? white : kSdrHeadroomNits;
}

double trimTargetPeak(const Sequence& s, const std::string& id) {
    if (const TrimTarget* t = findTrimTarget(s, id)) return targetPeak(trimTargetSpace(s, id), t->peakNits);
    return s.hdrPeakNits;
}

const std::vector<TrimControl>& trimControls() {
    static const std::vector<TrimControl> controls{
        {"lift", "Lift", 0, -0.5, 0.5},
        {"gamma", "Gamma", 0, -1, 1},
        {"gain", "Gain", 1, 0, 2},
        {"saturation", "Saturation", 1, 0, 2},
        {"highlights", "Highlight Roll-off", 0, 0, 1},
        {"mid_detail", "Mid-tone Contrast", 0, -1, 1},
        {"peak", "Shot Peak (nits)", 0, 0, 10000},
    };
    return controls;
}

Effect makeTrim() {
    Effect e;
    e.type = "trim";
    for (const TrimControl& c : trimControls()) e.params[c.name] = Param(c.def);
    return e;
}

bool isNeutralTrim(const Effect& trim, FrameTime t) {
    if (!trim.enabled) return true;
    for (const TrimControl& c : trimControls())
        if (std::fabs(trim.p(c.name, t, c.def) - c.def) > 1e-9) return false;
    return true;
}

const Clip* trimShot(const Sequence& s, FrameTime t) {
    const std::vector<bool> mattes = hiddenMatteTracks(s, t);
    for (int i = int(s.videoTracks.size()) - 1; i >= 0; --i) {
        const Track& track = s.videoTracks[size_t(i)];
        if (track.muted || mattes[size_t(i)]) continue;
        for (const Clip& c : track.clips)
            if (c.enabled && c.contains(t) && !overlay(c)) return &c;
    }
    return nullptr;
}

void rollOffPq(Image& img, double fromNits, double toNits) {
    if (img.empty() || toNits >= fromNits) return;
    const ColorSpace& pq = *findColorSpace("rec2100pq");
    const double top = nitsToCode(pq, fromNits), maxLum = nitsToCode(pq, toNits) / top;
    // The brightest channel's gain in light, by its code value (a 4097-point table).
    constexpr int kN = 4096;
    std::vector<float> gain(kN + 1);
    for (int i = 0; i <= kN; ++i) {
        const double code = double(i) / kN, e = std::min(code / top, 1.0);
        const double in = codeToNits(pq, code), out = codeToNits(pq, eetf(e, maxLum) * top);
        gain[size_t(i)] = float(in > 1e-6 ? out / in : 1);
    }
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                const double mx = std::clamp(std::max({p[0], p[1], p[2]}) / a, 0.0f, 1.0f) * kN;
                const int i = std::min(kN - 1, int(mx));
                const double g = gain[size_t(i)] + (gain[size_t(i) + 1] - gain[size_t(i)]) * (mx - i);
                if (g >= 0.99999) continue;
                for (int k = 0; k < 3; ++k) p[k] = float(nitsToCode(pq, codeToNits(pq, std::clamp(p[k] / a, 0.0f, 1.0f)) * g) * a);
            }
        }
    });
}

void applyTrimPass(Image& img, const Sequence& s, FrameTime t, const TrimTarget& target, const ColorSpace* from, bool exact) {
    const Clip* shot = trimShot(s, t);
    const auto it = shot ? shot->trims.find(target.id) : decltype(shot->trims)::const_iterator{};
    applyTrim(img, s, target, shot && it != shot->trims.end() ? &it->second : nullptr, shot ? t - shot->start : 0, from, exact);
}

void applyTrim(Image& img, const Sequence& s, const TrimTarget& target, const Effect* trimmed, FrameTime local, const ColorSpace* scene,
               bool exact) {
    const ColorSpace& from = sequenceColorSpace(s);
    const ColorSpace* to = trimTargetDisplay(s, target);
    // (A frame in scene light is always rendered for a display: the delivery space when the target names none known.)
    if (!to && scene && scene->sceneReferred) to = &from;
    if (img.empty() || !to) return;
    if (scene && scene->sceneReferred) {
        // (graded scene-referred: the target's own output transform, then its trim)
        outputTransform(img, *scene, *to, targetPeak(*to, target.peakNits), outputTransformKind(s), exact);
    } else {
        // 1. The master on the target's display: PQ above its peak rolled off first, then into its space. The
        // rolled-off light runs from the shot's own peak where its trim gives one, else the master's.
        const double peak = targetPeak(*to, target.peakNits), master = sourcePeak(s);
        const double shotPeak = trimmed && trimmed->enabled ? trimmed->p("peak", local, 0) : 0;
        const double fromPeak = shotPeak > 0 ? std::min(shotPeak, master) : master;
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        // (light above `top`, the shot's own brightest, held at the target's peak: no roll-off under it)
        auto rollOff = [&](double top, double toNits) {
            if (top > toNits) {
                rollOffPq(img, top, toNits);
                return;
            }
            const float most = float(nitsToCode(pq, toNits));
            for (size_t i = 0; i + 3 < img.px.size(); i += 4)  // (premultiplied: the colour under its alpha times the code)
                for (size_t k = 0; k < 3; ++k) img.px[i + k] = std::min(img.px[i + k], most * img.px[i + 3]);
        };
        if (!to->hdr() && from.hdr() && shotPeak > 0) {
            // SDR from the shot's peak: its light rolled off (in PQ) to what SDR shows just short of white, then tone
            // mapped. The roll-off is in the target's primaries, so a colour its smaller gamut makes brighter is rolled
            // off too.
            const ColorSpace rolled{"trim-pq-" + to->id, "", to->primaries, Transfer::Pq, false};
            convertColor(img, from, rolled, master);
            if (const double headroom = sdrHeadroomNits(*to); fromPeak > headroom) rollOffPq(img, fromPeak, headroom);
            convertColor(img, rolled, *to, master);
        } else if (to->transfer == Transfer::Pq && from.hdr() && (fromPeak > peak || master > peak)) {
            // (into PQ at the master's peak, then rolled off there: HLG's highlights too)
            convertColor(img, from, *to, master);
            rollOff(fromPeak, peak);
        } else {
            // (a PQ master into HLG rolled off to its 1000 nits first, on PQ's own code values)
            if (from.transfer == Transfer::Pq && to->hdr() && master > peak) rollOff(fromPeak, peak);
            convertColor(img, from, *to, to->transfer == Transfer::Pq ? peak : master);
        }
    }
    // 2. The shot's trim, on the display's code values.
    if (!trimmed || isNeutralTrim(*trimmed, local)) return;
    const Effect& trim = *trimmed;
    const double lift = trim.p("lift", local, 0), gamma = trim.p("gamma", local, 0), gain = trim.p("gain", local, 1);
    const double sat = trim.p("saturation", local, 1), roll = std::clamp(trim.p("highlights", local, 0), 0.0, 1.0);
    const double detail = trim.p("mid_detail", local, 0);
    const double expo = std::pow(2.0, -gamma), knee = 1 - 0.6 * roll;
    const double w709[3] = {0.2126, 0.7152, 0.0722}, w2020[3] = {0.2627, 0.6780, 0.0593};
    const double* w = to->primaries == Primaries::Bt2020 ? w2020 : w709;
    const std::vector<float> fine = std::fabs(detail) > 1e-9 ? lumaDetail(img, w) : std::vector<float>{};
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                double c[3];
                for (int k = 0; k < 3; ++k) {
                    double v = std::clamp(double(p[k] / a), 0.0, 1.0);
                    v = (v + lift * (1 - v)) * gain;
                    c[k] = std::pow(std::max(v, 0.0), expo);
                }
                const double luma = w[0] * c[0] + w[1] * c[1] + w[2] * c[2];
                double eased = sat;
                if (sat > 1) {
                    // More saturation eases into what is left under white (from 90 % code, or the brightest channel
                    // where it is already higher) rather than clipping the brightest channel, which would turn its hue.
                    const double mx = std::max({c[0], c[1], c[2]}), spread = mx - luma, raised = mx + (sat - 1) * spread;
                    const double knee = std::max(mx, 0.9);
                    if (spread > 1e-6 && raised > knee) {
                        const double room = std::max(0.0, 0.99 - knee);
                        const double top = room > 1e-9 ? knee + room * (1 - std::exp(-(raised - knee) / room)) : knee;
                        eased = 1 + std::max(0.0, top - mx) / spread;
                    }
                }
                for (double& v : c) v = luma + eased * (v - luma);
                if (roll > 0) {
                    const double mx = std::max({c[0], c[1], c[2]});
                    if (mx > knee) {
                        const double out = knee + (1 - knee) * std::tanh((mx - knee) / (1 - knee));
                        for (double& v : c) v *= out / mx;
                    }
                }
                if (!fine.empty()) {
                    // (strongest in the mid-tones, none at black or white)
                    const double m = std::clamp(luma, 0.0, 1.0), d = detail * 4 * m * (1 - m) * fine[size_t(y) * img.width + x];
                    for (double& v : c) v += d;
                }
                for (int k = 0; k < 3; ++k) p[k] = float(std::clamp(c[k], 0.0, 1.0) * a);
            }
        }
    });
}

namespace edit {

Result addTrimTarget(Sequence& s, TrimTarget target) {
    const ColorSpace* space = findColorSpace(target.colorSpace);
    if (!space || space->sceneReferred || space->toneMapping != ToneMapping::Montage)
        return Result::fail("A trim target is a display space (Rec.709, Rec.2100 PQ or HLG...)");
    ToneMapping tm;
    if (!parseToneMapping(target.toneMapping, tm)) return Result::fail("No tone mapping " + target.toneMapping + " (bt2446a or bt2446c)");
    if (tm != ToneMapping::Montage && space->hdr()) return Result::fail("Tone mapping is for an SDR target");
    if (tm != ToneMapping::Montage && sceneReferredWorking(s))
        return Result::fail("A sequence graded scene-referred goes into SDR through its output transform");
    target.peakNits = targetPeak(*space, target.peakNits);
    if (target.id.empty()) target.id = space->hdr() ? space->id + "-" + std::to_string(int(std::lround(target.peakNits))) : space->id;
    if (target.name.empty())
        target.name = space->transfer == Transfer::Pq ? space->label + " " + std::to_string(int(std::lround(target.peakNits))) + " nits" : space->label;
    const std::string base = target.id;
    for (int n = 2; findTrimTarget(s, target.id); ++n) target.id = base + "-" + std::to_string(n);
    s.trimTargets.push_back(std::move(target));
    return {};
}

Result setTrimTargetToneMapping(Sequence& s, const std::string& id, const std::string& toneMapping) {
    const TrimTarget* found = findTrimTarget(s, id);
    if (!found) return Result::fail("No trim target " + id);
    ToneMapping tm;
    if (!parseToneMapping(toneMapping, tm)) return Result::fail("No tone mapping " + toneMapping + " (bt2446a or bt2446c)");
    const ColorSpace* space = findColorSpace(found->colorSpace);
    if (tm != ToneMapping::Montage && (!space || space->hdr())) return Result::fail("Tone mapping is for an SDR target");
    if (tm != ToneMapping::Montage && sceneReferredWorking(s))
        return Result::fail("A sequence graded scene-referred goes into SDR through its output transform");
    for (TrimTarget& t : s.trimTargets)
        if (t.id == id) t.toneMapping = toneMapping;
    return {};
}

Result removeTrimTarget(Sequence& s, const std::string& id) {
    if (!findTrimTarget(s, id)) return Result::fail("No trim target " + id);
    std::erase_if(s.trimTargets, [&](const TrimTarget& t) { return t.id == id; });
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips) c.trims.erase(id);
    return {};
}

Result setTrim(Sequence& s, Id clipId, const std::string& target, const std::map<std::string, double>& values, FrameTime at, bool keyframe) {
    if (!findTrimTarget(s, target)) return Result::fail("No trim target " + target);
    const auto loc = locate(s, clipId);
    if (!loc || loc->track.kind != TrackKind::Video) return Result::fail("Trims are for picture clips");
    Track* t = trackAt(s, loc->track);
    if (!t || t->locked) return Result::fail("Its track is locked");
    Clip& c = t->clips[loc->index];
    for (const auto& [name, v] : values)
        if (std::none_of(trimControls().begin(), trimControls().end(), [&](const TrimControl& k) { return name == k.name; }))
            return Result::fail("No trim control " + name + " (lift, gamma, gain, saturation, highlights, mid_detail, peak)");
    auto [it, added] = c.trims.try_emplace(target, makeTrim());
    for (const TrimControl& k : trimControls())
        if (const auto v = values.find(k.name); v != values.end()) {
            Param& prm = it->second.params[k.name];
            const double value = std::clamp(v->second, k.min, k.max);
            if (keyframe && !prm.animated() && at != c.start) prm.addKey(0, prm.value);  // (from where it was)
            if (keyframe) prm.addKey(at - c.start, value);
            else prm.set(at - c.start, value);
        }
    return {};
}

Result resetTrims(Sequence& s, const std::vector<Id>& clips, const std::string& target) {
    Result res;
    for (Id id : clips) {
        const auto loc = locate(s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        Track* t = trackAt(s, loc->track);
        if (!t || t->locked) continue;
        Clip& c = t->clips[loc->index];
        if (target.empty()) c.trims.clear();
        else c.trims.erase(target);
        ++res.applied;
    }
    if (res.applied == 0) return Result::fail("No picture clips to reset");
    return res;
}

Result copyTrim(Sequence& s, Id from, const std::vector<Id>& to, const std::string& target) {
    if (!findTrimTarget(s, target)) return Result::fail("No trim target " + target);
    const Clip* src = clipById(s, from);
    if (!src) return Result::fail("No such clip");
    const auto trim = src->trims.find(target);
    const std::optional<Effect> copied = trim != src->trims.end() ? std::optional<Effect>(trim->second) : std::nullopt;
    Result res;
    for (Id id : to) {
        if (id == from) continue;
        const auto loc = locate(s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        Track* t = trackAt(s, loc->track);
        if (!t || t->locked) continue;
        Clip& c = t->clips[loc->index];
        if (copied) c.trims[target] = *copied;
        else c.trims.erase(target);
        ++res.applied;
    }
    if (res.applied == 0) return Result::fail("No picture clips to copy onto");
    return res;
}

}  // namespace edit

}  // namespace montage
