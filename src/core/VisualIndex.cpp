#include "VisualIndex.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Model.h"

namespace montage {

void VisualIndex::add(double time, const std::vector<float>& e) {
    Sample s;
    s.time = time;
    float peak = 0;
    for (float v : e) peak = std::max(peak, std::fabs(v));
    s.scale = peak > 0 ? peak / 127.f : 1.f;
    s.values.resize(e.size());
    for (size_t i = 0; i < e.size(); ++i) s.values[i] = int8_t(std::clamp<long>(std::lround(e[i] / s.scale), -127, 127));
    samples.push_back(std::move(s));
}

float VisualIndex::similarity(size_t i, const std::vector<float>& q) const {
    const Sample& s = samples[i];
    double dot = 0, len = 0;
    const size_t n = std::min(q.size(), s.values.size());
    for (size_t k = 0; k < n; ++k) {
        const double v = double(s.values[k]) * s.scale;
        dot += v * q[k];
        len += v * v;
    }
    return len > 0 ? float(dot / std::sqrt(len)) : 0.f;
}

// Each sample as base64 of its scale (float, little-endian) then its values.
std::string visualIndexToJson(const VisualIndex& v) {
    QJsonArray times, data;
    for (const auto& s : v.samples) {
        times.append(s.time);
        QByteArray b(int(sizeof(float) + s.values.size()), Qt::Uninitialized);
        std::memcpy(b.data(), &s.scale, sizeof(float));
        std::memcpy(b.data() + sizeof(float), s.values.data(), s.values.size());
        data.append(QString::fromLatin1(b.toBase64()));
    }
    const QJsonObject o{{"model", QString::fromStdString(v.model)}, {"step", v.step}, {"times", times}, {"samples", data}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

bool visualIndexFromJson(const std::string& json, VisualIndex& out) {
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
    if (o.isEmpty()) return false;
    VisualIndex v;
    v.model = o.value("model").toString().toStdString();
    v.step = o.value("step").toDouble();
    const QJsonArray times = o.value("times").toArray(), data = o.value("samples").toArray();
    if (times.size() != data.size()) return false;
    for (qsizetype i = 0; i < times.size(); ++i) {
        const QByteArray b = QByteArray::fromBase64(data[i].toString().toLatin1());
        if (b.size() <= qsizetype(sizeof(float))) return false;
        VisualIndex::Sample s;
        s.time = times[i].toDouble();
        std::memcpy(&s.scale, b.constData(), sizeof(float));
        s.values.resize(size_t(b.size()) - sizeof(float));
        std::memcpy(s.values.data(), b.constData() + sizeof(float), s.values.size());
        v.samples.push_back(std::move(s));
    }
    out = std::move(v);
    return true;
}

std::vector<ShotMatch> findShots(const Project& p, const std::vector<float>& query, size_t max) {
    std::vector<ShotMatch> all;
    for (const MediaItem& m : p.media) {
        if (!m.visual || m.visual->samples.empty()) continue;
        const VisualIndex& v = *m.visual;
        std::vector<float> score(v.samples.size());
        for (size_t i = 0; i < score.size(); ++i) score[i] = v.similarity(i, query);
        // Moments: local runs within a little of their peak (CLIP scores sit in a narrow band).
        std::vector<bool> used(score.size(), false);
        std::vector<size_t> order(score.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return score[a] > score[b]; });
        const double half = (v.step > 0 ? v.step : 1.0) / 2;
        for (size_t peak : order) {
            if (used[peak]) continue;
            size_t a = peak, b = peak;
            const float floorScore = score[peak] - 0.015f;
            while (a > 0 && !used[a - 1] && score[a - 1] >= floorScore) --a;
            while (b + 1 < score.size() && !used[b + 1] && score[b + 1] >= floorScore) ++b;
            for (size_t k = a; k <= b; ++k) used[k] = true;
            ShotMatch s;
            s.media = m.id;
            s.start = std::max(0.0, v.samples[a].time - half);
            s.end = m.duration > 0 ? std::min(m.duration, v.samples[b].time + half) : v.samples[b].time + half;
            s.best = v.samples[peak].time;
            s.score = score[peak];
            all.push_back(s);
        }
    }
    std::sort(all.begin(), all.end(), [](const ShotMatch& a, const ShotMatch& b) { return a.score > b.score; });
    if (all.size() > max) all.resize(max);
    return all;
}

}  // namespace montage
