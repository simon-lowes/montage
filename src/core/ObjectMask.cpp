#include "ObjectMask.h"

#include <QByteArray>
#include <algorithm>
#include <cmath>

namespace montage {

// The frame on screen at `seconds` (as the decoder shows it).
int64_t ObjectMask::frameAt(double seconds) const { return fps > 0 ? int64_t(std::floor(seconds * fps + 1e-6)) : 0; }

bool ObjectMask::logits(int64_t frame, std::vector<float>& out) const {
    auto it = frames.find(frame);
    return it != frames.end() && unpackObjectLogits(it->second, out);
}

bool ObjectMask::logitsAt(double seconds, std::vector<float>& out) const {
    if (fps <= 0 || frames.empty()) return false;
    return logits(frameAt(seconds), out);
}

std::string packObjectLogits(const float* logits) {
    constexpr int n = kObjectGrid * kObjectGrid;
    QByteArray raw(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i) {
        const float v = std::isfinite(logits[i]) ? logits[i] : -16.f;
        raw[i] = char(int8_t(std::clamp<long>(std::lround(v * 8), -127, 127)));
    }
    const QByteArray z = qCompress(raw, 9);
    return std::string(z.constData(), size_t(z.size()));
}

bool unpackObjectLogits(const std::string& packed, std::vector<float>& out) {
    constexpr int n = kObjectGrid * kObjectGrid;
    const QByteArray raw = qUncompress(reinterpret_cast<const uchar*>(packed.data()), qsizetype(packed.size()));
    if (raw.size() != n) return false;
    out.resize(n);
    for (int i = 0; i < n; ++i) out[size_t(i)] = float(int8_t(raw[i])) / 8.f;
    return true;
}

double objectCoverage(const std::vector<float>& logits) {
    if (logits.empty()) return 0;
    size_t in = 0;
    for (float v : logits) in += v > 0;
    return double(in) / double(logits.size());
}

}  // namespace montage
