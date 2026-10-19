#include "AutoTrims.h"

#include <algorithm>
#include <cmath>

#include "ColorSpace.h"
#include "Compositor.h"
#include "TrimPasses.h"

namespace montage {

namespace {

// Light in a log histogram: 1/32 stop bins from 2^-12 nits up.
constexpr int kBinsPerStop = 32, kLowStop = -12, kBins = (14 - kLowStop) * kBinsPerStop;
constexpr double kGreyFloor = 0.005;  // nits: the darkest counts as this in the log-average
constexpr double kBlackNits = 1e-4;   // nits: under this a pixel is black (bars), left out

int binOf(double nits) {
    if (nits <= 0) return 0;
    return std::clamp(int(std::floor((std::log2(nits) - kLowStop) * kBinsPerStop)), 0, kBins - 1);
}

double nitsOfBin(int b) { return std::exp2(kLowStop + (b + 0.5) / kBinsPerStop); }

double percentile(const std::vector<double>& hist, double total, double fraction) {
    double seen = 0;
    for (int b = 0; b < kBins; ++b) {
        seen += hist[size_t(b)];
        if (seen >= fraction * total) return nitsOfBin(b);
    }
    return nitsOfBin(kBins - 1);
}

struct Tally {
    std::vector<double> y = std::vector<double>(kBins, 0), top = std::vector<double>(kBins, 0);
    double logSum = 0, count = 0, peak = 0;
    int frames = 0;
    FrameTime from = -1, to = -1;
    std::vector<FrameTime> at;  // the frames it is the shot at
};

}  // namespace

bool measureShotLight(const Project& p, const Sequence& s, FrameTime from, FrameTime to, std::vector<ShotLight>& out,
                      std::string* error, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                      int framesPerShot, int width) {
    out.clear();
    const ColorSpace& master = sequenceColorSpace(s);
    if (!master.hdr()) {
        if (error) *error = "Automatic trims are made from an HDR master (PQ or HLG); see Sequence Settings";
        return false;
    }
    from = std::max<FrameTime>(0, from);
    if (to <= from) to = s.duration();
    // Which frames each shot is on screen.
    std::map<Id, Tally> shots;
    std::vector<Id> order;
    for (FrameTime f = from; f < to; ++f)
        if (const Clip* c = trimShot(s, f)) {
            auto [it, added] = shots.try_emplace(c->id);
            if (added) order.push_back(c->id), it->second.from = f;
            it->second.at.push_back(f);
            it->second.to = f + 1;
        }
    if (shots.empty()) {
        if (error) *error = "No shots to analyse";
        return false;
    }
    const ColorSpace& pq = *findColorSpace("rec2100pq");
    std::vector<float> nits(65536);
    for (size_t i = 0; i < nits.size(); ++i) nits[i] = float(codeToNits(pq, double(i) / 65535));
    const auto lightOf = [&](float v) { return nits[size_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))]; };
    RenderOptions o;
    o.scale = std::clamp(double(std::max(16, width)) / std::max(1, s.width), 0.05, 1.0);
    o.highQuality = true;
    o.cancel = cancel;
    int total = 0, done = 0;
    for (const auto& [id, t] : shots) total += std::min<int>(framesPerShot, int(t.at.size()));
    for (Id id : order) {
        Tally& t = shots[id];
        const int n = std::max(1, std::min<int>(framesPerShot, int(t.at.size())));
        for (int k = 0; k < n; ++k) {
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return false;
            }
            // (evenly across the shot, the middle of each stretch)
            const FrameTime f = t.at[std::min(t.at.size() - 1, size_t((k + 0.5) * double(t.at.size()) / n))];
            Image img = renderProgramFrame(p, s, f, o);
            if (master.transfer != Transfer::Pq) convertColor(img, master, pq, 1000);
            for (int y = 0; y < img.height; ++y) {
                const float* px = img.row(y);
                for (int x = 0; x < img.width; ++x, px += 4) {
                    const float a = px[3];
                    double r = 0, g = 0, b = 0;  // (uncovered: black, as programme frames are over black)
                    if (a > 0) r = lightOf(px[0] / a), g = lightOf(px[1] / a), b = lightOf(px[2] / a);
                    const double lum = 0.2627 * r + 0.6780 * g + 0.0593 * b, mx = std::max({r, g, b});
                    if (mx < kBlackNits) continue;  // (letterbox bars and what nothing covers are not the shot's light)
                    t.y[size_t(binOf(lum))] += 1;
                    t.top[size_t(binOf(mx))] += 1;
                    t.logSum += std::log(std::max(lum, kGreyFloor));
                    t.count += 1;
                    t.peak = std::max(t.peak, mx);
                }
            }
            ++t.frames;
            if (progress) progress(double(++done) / std::max(1, total));
        }
    }
    for (Id id : order) {
        const Tally& t = shots[id];
        if (t.count <= 0) continue;
        ShotLight l;
        l.clip = id;
        l.from = t.from, l.to = t.to;
        l.logAverage = std::exp(t.logSum / t.count);
        l.low = percentile(t.y, t.count, 0.01);
        l.high = std::min(percentile(t.top, t.count, 0.999), t.peak);
        l.peak = t.peak;
        l.frames = t.frames;
        out.push_back(l);
    }
    return true;
}

double trimmedGrey(const Sequence& s, const TrimTarget& target, const Effect* trim, double nits) {
    Image px(1, 1);
    const float v = float(nitsToCode(sequenceColorSpace(s), nits));
    float* q = px.at(0, 0);
    q[0] = q[1] = q[2] = v;
    q[3] = 1;
    applyTrim(px, s, target, trim, 0);
    return px.px[1];
}

bool hasOwnTrim(const Clip& c, const std::string& target) {
    const auto it = c.trims.find(target);
    if (it == c.trims.end()) return false;
    for (const auto& [name, prm] : it->second.params)
        if (prm.varies()) return true;
    // (one switched off counts by its values: it is being compared against, not given up)
    Effect on = it->second;
    on.enabled = true;
    return !isNeutralTrim(on, 0);
}

std::vector<AutoTrim> solveAutoTrims(const Sequence& s, const std::string& targetId, const std::vector<ShotLight>& shots) {
    std::vector<AutoTrim> out;
    const TrimTarget* target = findTrimTarget(s, targetId);
    const ColorSpace* to = target ? findColorSpace(target->colorSpace) : nullptr;
    const ColorSpace& from = sequenceColorSpace(s);
    if (!to || !from.hdr()) return out;
    const double master = from.transfer == Transfer::Pq ? std::clamp(s.hdrPeakNits, 100.0, 10000.0) : 1000;
    const double peak = trimTargetPeak(s, targetId);
    // Each colour group's median mid-grey (in stops).
    std::map<Id, std::vector<double>> groups;
    for (const ShotLight& l : shots)
        if (const Clip* c = edit::clipById(s, l.clip); c && c->colorGroup) groups[c->colorGroup].push_back(std::log2(l.logAverage));
    std::map<Id, double> medians;
    for (auto& [g, stops] : groups) {
        std::sort(stops.begin(), stops.end());
        const size_t n = stops.size();
        medians[g] = n % 2 ? stops[n / 2] : (stops[n / 2 - 1] + stops[n / 2]) / 2;
    }
    for (const ShotLight& l : shots) {
        const Clip* c = edit::clipById(s, l.clip);
        if (!c) continue;
        AutoTrim a;
        a.clip = l.clip;
        a.target = targetId;
        a.midGrey = l.logAverage;
        // Pulled to within an eighth of a stop of its group's median (a shot more than two stops off it is lit so).
        double stop = std::log2(l.logAverage);
        if (const auto m = medians.find(c->colorGroup); c->colorGroup && m != medians.end() && std::fabs(stop - m->second) <= 2)
            stop = m->second + std::clamp(stop - m->second, -0.125, 0.125);
        a.aimedGrey = std::exp2(stop);
        a.greyCode = trimmedGrey(s, *target, nullptr, a.aimedGrey);
        Effect trim = makeTrim();
        auto set = [&](const char* name, double v) {
            trim.params[name] = Param(v);
            a.values[name] = v;
        };
        // The highlight roll-off from the shot's own brightest (all but its brightest 0.1 %).
        const double headroom = to->hdr() ? peak : kSdrHeadroomNits;
        if (master > headroom) {
            const double top = std::clamp(l.high, 1.0, master);
            if (to->hdr() || top > headroom) set("peak", std::round(top));
            // Saturation lost to the roll-off made up, up to 10 % three stops over.
            if (top > headroom) {
                const double sat = 1 + 0.1 * std::clamp(std::log2(top / headroom) / 3, 0.0, 1.0);
                if (sat > 1.005) set("saturation", std::round(sat * 1000) / 1000);
            }
        }
        // SDR shadows a display's black would swallow (under 2.5 % code) lifted a little.
        if (!to->hdr()) {
            const double low = trimmedGrey(s, *target, nullptr, l.low);
            if (low < 0.025) {
                const double lift = std::clamp((0.025 - low) * 0.5, 0.0, 0.03);
                if (lift > 0.0005) set("lift", std::round(lift * 10000) / 10000);
            }
        }
        // Gamma last: the mid-grey back where it is aimed.
        const double m0 = trimmedGrey(s, *target, &trim, l.logAverage);
        if (m0 > 1e-4 && m0 < 0.9999 && a.greyCode > 1e-4 && a.greyCode < 0.9999) {
            const double gamma = std::clamp(-std::log2(std::log(a.greyCode) / std::log(m0)), -1.0, 1.0);
            if (std::fabs(gamma) > 0.0005) set("gamma", std::round(gamma * 10000) / 10000);
        }
        if (!a.values.empty()) out.push_back(std::move(a));
    }
    return out;
}

namespace edit {

Result fillAutoTrims(Sequence& s, const std::vector<AutoTrim>& trims) {
    Result res;
    for (const AutoTrim& a : trims) {
        const auto loc = locate(s, a.clip);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        Track* t = trackAt(s, loc->track);
        if (!t || t->locked) continue;
        Clip& c = t->clips[loc->index];
        if (hasOwnTrim(c, a.target)) continue;
        c.trims.erase(a.target);  // (a neutral one)
        if (!setTrim(s, a.clip, a.target, a.values, c.start).ok) continue;
        ++res.applied;
    }
    if (res.applied == 0) return Result::fail(trims.empty() ? "No shots need a trim" : "Every shot already has a trim of its own");
    return res;
}

}  // namespace edit

}  // namespace montage
