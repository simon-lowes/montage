#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

#include "Decoder.h"
#include "core/EditOps.h"
#include "render/Exporter.h"

namespace montage {

namespace {

constexpr int kW = 64, kH = 36;  // analysis resolution
constexpr int kBins = 16;

struct Signature {
    std::vector<float> hist;   // kBins per channel (Y, Cb, Cr)
    std::vector<float> luma;   // kW * kH
};

Signature signatureOf(const Frame16& f) {
    Signature s;
    s.hist.assign(kBins * 3, 0.0f);
    s.luma.resize(size_t(f.width) * size_t(f.height));
    const float k = 1.0f / 65535.0f;
    for (int i = 0; i < f.width * f.height; ++i) {
        float r = f.px[size_t(i) * 4] * k, g = f.px[size_t(i) * 4 + 1] * k, b = f.px[size_t(i) * 4 + 2] * k;
        float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float cb = (b - y) / 1.8556f + 0.5f, cr = (r - y) / 1.5748f + 0.5f;
        s.luma[size_t(i)] = y;
        s.hist[size_t(std::clamp(int(y * kBins), 0, kBins - 1))] += 1;
        s.hist[size_t(kBins + std::clamp(int(cb * kBins), 0, kBins - 1))] += 1;
        s.hist[size_t(2 * kBins + std::clamp(int(cr * kBins), 0, kBins - 1))] += 1;
    }
    float n = float(f.width * f.height);
    for (float& v : s.hist) v /= n;
    return s;
}

// 0 (identical) .. 1 (completely different).
double difference(const Signature& a, const Signature& b) {
    double h = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) h += std::fabs(a.hist[i] - b.hist[i]);
    h /= 6.0;  // three histograms, each L1 distance <= 2
    double p = 0;
    for (size_t i = 0; i < a.luma.size(); ++i) p += std::fabs(a.luma[i] - b.luma[i]);
    p /= double(a.luma.size());
    return std::clamp(0.6 * h + 0.4 * std::min(1.0, p * 3.0), 0.0, 1.0);
}

}  // namespace

std::vector<double> detectSceneCuts(const std::string& path, double sensitivity, const AnalysisProgress& progress,
                                    const std::atomic<bool>* cancel, std::string* error) {
    std::vector<double> cuts;
    VideoDecoder dec;
    if (!dec.open(path, error)) return cuts;
    if (dec.isStill()) return cuts;
    const double fps = dec.fps() > 0 ? dec.fps() : 25.0;
    const double duration = dec.duration();
    const int64_t frames = int64_t(std::floor(duration * fps));
    sensitivity = std::clamp(sensitivity, 0.0, 1.0);
    const double absThreshold = 0.45 - 0.33 * sensitivity;  // 0.45 (strict) .. 0.12 (sensitive)
    const double minGap = 0.4;                               // seconds between cuts
    std::deque<double> recent;
    Signature prev;
    bool havePrev = false;
    double lastCut = -1e9;
    for (int64_t i = 0; i < frames; ++i) {
        if (cancel && cancel->load()) return {};
        double t = double(i) / fps;
        Frame16Ptr f = dec.frameAt(t, kW, kH);
        if (!f) break;
        Signature sig = signatureOf(*f);
        if (havePrev) {
            double d = difference(prev, sig);
            double avg = recent.empty() ? 0.0 : std::accumulate(recent.begin(), recent.end(), 0.0) / double(recent.size());
            // A cut is a spike well above the recent motion level.
            if (d > absThreshold && d > avg * 3.0 + 0.05 && t - lastCut >= minGap) {
                cuts.push_back(t);
                lastCut = t;
            }
            recent.push_back(d);
            if (recent.size() > 12) recent.pop_front();
        }
        prev = std::move(sig);
        havePrev = true;
        if (progress && (i % 15 == 0)) progress(double(i) / double(std::max<int64_t>(1, frames)));
    }
    if (progress) progress(1.0);
    return cuts;
}

bool createProxy(const std::string& source, const std::string& proxyPath, int maxWidth, const AnalysisProgress& progress,
                 const std::atomic<bool>* cancel, std::string* error) {
    MediaItem m;
    if (!probeMedia(source, m, error)) return false;
    if (!m.hasVideo || m.kind != MediaKind::Video) {
        if (error) *error = "Proxies are only made for video files";
        return false;
    }
    // Render the clip through a sequence of its own size and rate.
    Project p = makeDefaultProject();
    Sequence& s = *p.active();
    s.width = m.width;
    s.height = m.height;
    s.fps = m.fps.valid() ? m.fps : Rational{30, 1};
    m.id = p.newId();
    p.media.push_back(m);
    edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
    ExportSettings st;
    st.path = proxyPath;
    st.videoCodec = "libx264";
    st.audioCodec = "none";
    st.crf = 23;
    st.preset = "veryfast";
    st.gop = 1;  // intra-only: every frame is a seek point
    double scale = std::min(1.0, double(maxWidth) / std::max(1, m.width));
    st.width = std::max(2, int(std::lround(m.width * scale)) & ~1);
    st.height = std::max(2, int(std::lround(m.height * scale)) & ~1);
    return exportSequence(p, s, st, [&](double f, FrameTime) { if (progress) progress(f); }, cancel, error);
}

}  // namespace montage
