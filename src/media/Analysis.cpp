#include "Analysis.h"

#include <QImage>
#include <QString>
#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

#include "Decoder.h"
#include "SuperScale.h"
#include "core/Effects.h"
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

bool createSuperScaled(const std::string& source, const std::string& dest, int factor, double strength,
                       const AnalysisProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    if (!upscalerAvailable() || !upscaleModel().installed()) {
        if (error) *error = upscalerAvailable() ? "The Super Scale model is not downloaded" : "This build has no ONNX Runtime";
        return false;
    }
    MediaItem m;
    if (!probeMedia(source, m, error)) return false;
    if (!m.hasVideo || m.width <= 0 || m.height <= 0) {
        if (error) *error = "Super Scale needs a video or a still";
        return false;
    }
    factor = std::clamp(factor, 2, 4);
    if (m.kind == MediaKind::Image) {
        VideoDecoder dec;
        if (!dec.open(source, error)) return false;
        Frame16Ptr f = dec.frameAt(0);
        if (!f) {
            if (error) *error = "Cannot read " + source;
            return false;
        }
        Image big;
        if (!superScale(toImage(*f), f->width * factor, f->height * factor, big, strength, error, cancel)) return false;
        if (progress) progress(1.0);
        QImage out(toRgba8(big).data(), big.width, big.height, big.width * 4, QImage::Format_RGBA8888);
        if (!out.save(QString::fromStdString(dest))) {
            if (error) *error = "Cannot write " + dest;
            return false;
        }
        return true;
    }
    // A video: rendered through a sequence of the new size, the clip with Super Scale on it.
    Project p = makeDefaultProject();
    Sequence& s = *p.active();
    s.width = (m.width * factor) & ~1;
    s.height = (m.height * factor) & ~1;
    s.fps = m.fps.valid() ? m.fps : Rational{30, 1};
    m.id = p.newId();
    p.media.push_back(m);
    edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
    if (s.videoTracks.empty() || s.videoTracks[0].clips.empty()) {
        if (error) *error = "Nothing to scale in " + source;
        return false;
    }
    Effect e = makeEffect(p, "super_scale");
    e.params["strength"] = Param(std::clamp(strength, 0.0, 1.0) * 100);
    s.videoTracks[0].clips[0].effects.push_back(e);
    const std::string ext = dest.substr(dest.find_last_of('.') + 1);
    const ExportPreset* pr = findExportPreset(ext == "mov" ? "Apple ProRes 422 HQ" : "H.264 - High Quality");
    ExportSettings st = pr ? pr->settings : ExportSettings{};
    st.path = dest;
    st.width = s.width;
    st.height = s.height;
    if (!m.hasAudio) st.audioCodec = "none";
    return exportSequence(p, s, st, [&](double f, FrameTime) { if (progress) progress(f); }, cancel, error);
}

}  // namespace montage
