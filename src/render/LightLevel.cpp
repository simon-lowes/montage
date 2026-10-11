#include "LightLevel.h"

#include <algorithm>
#include <cmath>

#include "render/Compositor.h"

namespace montage {

namespace {

constexpr int kCodes = 65536;

inline int code16(float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * float(kCodes - 1))); }

}  // namespace

LightMeter::LightMeter(const ColorSpace& space) : space_(&space) {
    valid_ = codeToNits(space, 1.0) > 0;
    if (!valid_) return;
    nits_.resize(kCodes);
    for (int i = 0; i < kCodes; ++i) {
        const double v = double(i) / (kCodes - 1);
        // HLG keeps scene light (its OOTF needs all three channels); the others their light in nits.
        nits_[size_t(i)] = float(space.transfer == Transfer::Hlg ? toLinear(Transfer::Hlg, v) : codeToNits(space, v));
    }
}

void LightMeter::measure(const Image& img, double& peak, double& average) const {
    peak = average = 0;
    if (!valid_ || img.width <= 0 || img.height <= 0) return;
    const bool hlg = space_->transfer == Transfer::Hlg;
    double sum = 0;
    for (int y = 0; y < img.height; ++y) {
        const float* p = img.row(y);
        double rowSum = 0;
        for (int x = 0; x < img.width; ++x, p += 4) {
            double n;
            if (!hlg) {
                n = nits_[size_t(code16(std::max({p[0], p[1], p[2]})))];
            } else {
                const double r = nits_[size_t(code16(p[0]))], g = nits_[size_t(code16(p[1]))], b = nits_[size_t(code16(p[2]))];
                const double ys = 0.2627 * r + 0.6780 * g + 0.0593 * b;
                n = ys > 0 ? 1000.0 * std::pow(ys, 0.2) * std::max({r, g, b}) : 0.0;
            }
            peak = std::max(peak, n);
            rowSum += n;
        }
        sum += rowSum;
    }
    average = sum / (double(img.width) * img.height);
}

void LightMeter::add(const Image& img, FrameTime f, LightLevels& into) const {
    double peak = 0, average = 0;
    measure(img, peak, average);
    if (into.maxCllFrame < 0 || peak > into.maxCll) into.maxCll = peak, into.maxCllFrame = f;
    if (into.maxFallFrame < 0 || average > into.maxFall) into.maxFall = average, into.maxFallFrame = f;
    ++into.frames;
}

bool measureLightLevels(const Project& p, const Sequence& s, FrameTime from, FrameTime to, LightLevels& out, std::string* error,
                        double scale, const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out = LightLevels{};
    const ColorSpace& space = sequenceColorSpace(s);
    const LightMeter meter(space);
    if (!meter.valid()) {
        if (error) *error = "Light levels are measured in display spaces, not " + space.label;
        return false;
    }
    from = std::max<FrameTime>(0, from);
    if (to <= from) to = s.duration();
    if (to <= from) {
        if (error) *error = "The sequence is empty";
        return false;
    }
    RenderOptions o;
    o.scale = std::clamp(scale, 0.05, 1.0);
    o.highQuality = true;
    for (FrameTime f = from; f < to; ++f) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        meter.add(renderProgramFrame(p, s, f, o), f, out);
        if (progress && ((f - from) % 5 == 0 || f + 1 == to)) progress(double(f - from + 1) / double(to - from));
    }
    return true;
}

void hdr10LightLevels(const LightLevels& l, unsigned& maxCll, unsigned& maxFall) {
    maxCll = unsigned(std::clamp(std::lround(l.maxCll), 1L, 10000L));
    maxFall = unsigned(std::clamp(std::lround(l.maxFall), 1L, long(maxCll)));
}

}  // namespace montage
