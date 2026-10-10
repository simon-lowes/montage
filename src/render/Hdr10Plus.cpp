#include "Hdr10Plus.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

#include "render/Compositor.h"
#include "render/RenderCache.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace montage {

namespace {

constexpr int kCodes = 65536;
constexpr int kCoarse = 32;
// The percentages HDR10+ sends (ST 2094-40 §8.5.4): 99 stands for 99.98 %, and 5 and 10 carry fixed values.
constexpr int kPercentages[9] = {1, 5, 10, 25, 50, 75, 90, 95, 99};
constexpr double kMeasured[7] = {1, 25, 50, 75, 90, 95, 99.98};  // what Hdr10PlusScene::percentiles holds

inline int code16(float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * float(kCodes - 1))); }

// Linearised light as ST 2094-40 codes it: 0.00001 of 10000 cd/m² (0.1 cd/m²) steps, 17 bits.
uint32_t linearCode(double nits) { return uint32_t(std::clamp<long>(std::lround(nits * 10), 0, 100000)); }

// The nine distribution values for a scene: the measured percentiles where they go, and the fixed values the
// standard puts at 5 and 10 % (0 and 0.00255).
void distribution(const Hdr10PlusScene& s, uint32_t (&v)[9]) {
    v[0] = linearCode(s.percentiles[0]);
    v[1] = 0;
    v[2] = 255;
    for (int i = 3; i < 9; ++i) v[i] = linearCode(s.percentiles[i - 2]);
}

class Bits {
public:
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (fill_ == 0) bytes.push_back(0);
            if ((v >> i) & 1) bytes.back() |= uint8_t(0x80 >> fill_);
            fill_ = (fill_ + 1) & 7;
        }
    }
    std::vector<uint8_t> bytes;

private:
    int fill_ = 0;
};

// Emulation prevention for an HEVC NAL unit's payload: no 00 00 followed by 00..03.
std::vector<uint8_t> escape(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

void writeLeb128(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

bool readLeb128(const uint8_t* p, const uint8_t* end, uint64_t& v, int& len) {
    v = 0;
    len = 0;
    for (int i = 0; i < 8 && p + i < end; ++i) {
        v |= uint64_t(p[i] & 0x7f) << (7 * i);
        len = i + 1;
        if (!(p[i] & 0x80)) return true;
    }
    return false;
}

// Replaces the packet's data with `data`, keeping its timing, flags and side data.
bool replaceData(AVPacket* pkt, const std::vector<uint8_t>& data) {
    AVPacket* tmp = av_packet_alloc();
    if (!tmp) return false;
    if (av_new_packet(tmp, int(data.size())) < 0 || av_packet_copy_props(tmp, pkt) < 0) {
        av_packet_free(&tmp);
        return false;
    }
    std::copy(data.begin(), data.end(), tmp->data);
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, tmp);
    av_packet_free(&tmp);
    return true;
}

std::string sceneKey(const std::vector<QByteArray>& frameKeys, size_t first, size_t last, const ColorSpace& out, double peakNits) {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray("montage-hdr10plus-1#") + QByteArray::fromStdString(out.id) + '#' + QByteArray::number(peakNits, 'g', 10));
    for (size_t i = first; i < last; ++i) {
        h.addData(frameKeys[i].isEmpty() ? QByteArray("-") : frameKeys[i]);
        h.addData(QByteArray(1, '|'));
    }
    return h.result().toHex().toStdString();
}

QByteArray analysisFrameKey(const Project& p, const Sequence& s, FrameTime f) {
    RenderOptions o;
    o.scale = 1.0;
    o.highQuality = true;
    return frameKey(p, s, f, o);
}

}  // namespace

// ---- Measuring ------------------------------------------------------------------------------------------------------

Hdr10PlusMeter::Hdr10PlusMeter(const ColorSpace& space) {
    valid_ = space.transfer == Transfer::Pq;
    if (!valid_) return;
    nits_.resize(kCodes);
    for (int i = 0; i < kCodes; ++i) nits_[size_t(i)] = float(codeToNits(space, double(i) / (kCodes - 1)));
    hist_.assign(kCodes, 0);
}

void Hdr10PlusMeter::close() {
    if (pixels_ == 0) return;
    cur_.average = sum_ / double(pixels_);
    for (int i = 0; i < 7; ++i) {
        // The smallest value with at least that share of the pixels at or below it.
        const uint64_t need = uint64_t(std::ceil(kMeasured[i] / 100.0 * double(pixels_)));
        uint64_t seen = 0;
        int code = kCodes - 1;
        for (int c = 0; c < kCodes; ++c) {
            seen += hist_[size_t(c)];
            if (seen >= std::max<uint64_t>(1, need)) {
                code = c;
                break;
            }
        }
        cur_.percentiles[i] = nits_[size_t(code)];
    }
    done_.push_back(cur_);
    std::fill(hist_.begin(), hist_.end(), 0);
    sum_ = 0;
    pixels_ = 0;
}

void Hdr10PlusMeter::add(const Image& img, FrameTime f, bool cut) {
    if (!valid_ || img.empty()) return;
    // The frame's spread of brightest channels, coarsely, to see a cut within a clip.
    coarse_.assign(kCoarse, 0);
    std::vector<uint64_t>& frameHist = frame_;
    frameHist.assign(kCodes, 0);
    double frameMax[3] = {0, 0, 0}, frameSum = 0;
    for (int y = 0; y < img.height; ++y) {
        const float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 4) {
            const int r = code16(p[0]), g = code16(p[1]), b = code16(p[2]);
            const int m = std::max({r, g, b});
            frameMax[0] = std::max<double>(frameMax[0], nits_[size_t(r)]);
            frameMax[1] = std::max<double>(frameMax[1], nits_[size_t(g)]);
            frameMax[2] = std::max<double>(frameMax[2], nits_[size_t(b)]);
            frameSum += nits_[size_t(m)];
            ++frameHist[size_t(m)];
            coarse_[size_t(m * kCoarse / kCodes)] += 1;
        }
    }
    const double n = double(img.width) * img.height;
    for (double& c : coarse_) c /= n;
    bool change = false;
    if (!lastCoarse_.empty() && pixels_ > 0 && f - cur_.start >= minSceneFrames) {
        double d = 0;
        for (int i = 0; i < kCoarse; ++i) d += std::fabs(coarse_[size_t(i)] - lastCoarse_[size_t(i)]);
        change = d / 2 > 0.5;  // over half the picture moved to other brightnesses at once
    }
    lastCoarse_ = coarse_;
    if (pixels_ > 0 && (cut || change || f != next_)) {
        close();
        cur_ = Hdr10PlusScene{};
    }
    if (pixels_ == 0) cur_.start = f;
    cur_.end = f + 1;
    next_ = f + 1;
    for (int c = 0; c < 3; ++c) cur_.maxScl[c] = std::max(cur_.maxScl[c], frameMax[c]);
    sum_ += frameSum;
    pixels_ += uint64_t(n);
    for (int c = 0; c < kCodes; ++c) hist_[size_t(c)] += frameHist[size_t(c)];
}

std::vector<Hdr10PlusScene> Hdr10PlusMeter::scenes() {
    close();
    cur_ = Hdr10PlusScene{};
    return done_;
}

std::vector<FrameTime> hdr10PlusCuts(const Sequence& s, FrameTime from, FrameTime to) {
    std::vector<FrameTime> cuts;
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips)
            for (FrameTime e : {c.start, c.end()})
                if (e > from && e < to) cuts.push_back(e);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    return cuts;
}

std::string hdr10PlusFramesKey(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits) {
    std::vector<QByteArray> keys;
    for (FrameTime f = from; f < to; ++f) keys.push_back(analysisFrameKey(p, s, f));
    return sceneKey(keys, 0, keys.size(), out, peakNits);
}

bool analyseHdr10Plus(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                      std::vector<Hdr10PlusScene>& scenes, std::string* error, const std::function<void(double)>& progress,
                      const std::atomic<bool>* cancel, LightLevels* light) {
    scenes.clear();
    if (light) *light = LightLevels{};
    Hdr10PlusMeter meter(out);
    if (!meter.valid()) {
        if (error) *error = "HDR10+ metadata is for PQ (HDR10) pictures, not " + out.label;
        return false;
    }
    from = std::max<FrameTime>(0, from);
    if (to <= from) to = s.duration();
    if (to <= from) {
        if (error) *error = "The sequence is empty";
        return false;
    }
    meter.minSceneFrames = std::max(2, int(std::lround(s.fpsValue() / 2)));
    const std::vector<FrameTime> cuts = hdr10PlusCuts(s, from, to);
    const ColorSpace& space = sequenceColorSpace(s);
    RenderOptions o;
    o.scale = 1.0;
    o.highQuality = true;
    const LightMeter lightMeter(out);
    std::vector<QByteArray> keys;
    size_t nextCut = 0;
    for (FrameTime f = from; f < to; ++f) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        Image img = renderProgramFrame(p, s, f, o);
        convertColor(img, space, out, peakNits);
        const bool cut = nextCut < cuts.size() && cuts[nextCut] == f;
        if (cut) ++nextCut;
        meter.add(img, f, cut);
        if (light) lightMeter.add(img, f, *light);
        keys.push_back(analysisFrameKey(p, s, f));
        if (progress && ((f - from) % 5 == 0 || f + 1 == to)) progress(double(f - from + 1) / double(to - from));
    }
    scenes = meter.scenes();
    for (Hdr10PlusScene& sc : scenes) sc.key = sceneKey(keys, size_t(sc.start - from), size_t(sc.end - from), out, peakNits);
    return !scenes.empty();
}

std::vector<Hdr10PlusScene> storedHdr10Plus(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out,
                                            double peakNits) {
    std::vector<Hdr10PlusScene> part;
    if (to <= from) return part;
    FrameTime at = from;
    for (const Hdr10PlusScene& sc : s.hdr10Plus) {
        if (sc.end <= at || sc.start >= to) continue;
        if (sc.start > at) return {};  // a gap
        if (hdr10PlusFramesKey(p, s, sc.start, sc.end, out, peakNits) != sc.key) return {};  // changed since
        Hdr10PlusScene c = sc;
        c.start = std::max(c.start, from);
        c.end = std::min(c.end, to);
        part.push_back(c);
        at = c.end;
        if (at >= to) break;
    }
    if (at < to) return {};
    return part;
}

// ---- Writing --------------------------------------------------------------------------------------------------------

std::vector<uint8_t> hdr10PlusT35(const Hdr10PlusScene& sc) {
    Bits b;
    b.put(0xB5, 8);    // itu_t_t35_country_code: United States
    b.put(0x003C, 16);  // terminal_provider_code: Samsung
    b.put(0x0001, 16);  // terminal_provider_oriented_code
    b.put(4, 8);        // application_identifier: ST 2094-40
    b.put(1, 8);        // application_version (HDR10+)
    b.put(1, 2);        // num_windows: the whole frame
    b.put(0, 27);       // targeted_system_display_maximum_luminance: none (profile A)
    b.put(0, 1);        // targeted_system_display_actual_peak_luminance_flag
    for (double m : sc.maxScl) b.put(linearCode(m), 17);
    b.put(linearCode(sc.average), 17);
    uint32_t v[9];
    distribution(sc, v);
    b.put(9, 4);  // num_distribution_maxrgb_percentiles
    for (int i = 0; i < 9; ++i) {
        b.put(uint32_t(kPercentages[i]), 7);
        b.put(v[i], 17);
    }
    b.put(0, 10);  // fraction_bright_pixels: not calculated
    b.put(0, 1);   // mastering_display_actual_peak_luminance_flag
    b.put(0, 1);   // tone_mapping_flag: none (profile A)
    b.put(0, 1);   // color_saturation_mapping_flag
    return b.bytes;  // (the last byte zero-padded)
}

bool addHdr10PlusSei(AVPacket* pkt, const std::vector<uint8_t>& t35, int lengthSize) {
    if (!pkt || !pkt->data || pkt->size <= 0 || t35.empty() || lengthSize < 0 || lengthSize > 4) return false;
    // sei_message: payloadType 4 (user data registered by ITU-T T.35), its size, the message; then trailing bits.
    std::vector<uint8_t> rbsp{4};
    size_t size = t35.size();
    while (size >= 255) rbsp.push_back(255), size -= 255;
    rbsp.push_back(uint8_t(size));
    rbsp.insert(rbsp.end(), t35.begin(), t35.end());
    rbsp.push_back(0x80);
    std::vector<uint8_t> nal{0x4E, 0x01};  // prefix SEI (type 39), layer 0, temporal id 0
    const std::vector<uint8_t> body = escape(rbsp);
    nal.insert(nal.end(), body.begin(), body.end());
    const uint8_t* d = pkt->data;
    const size_t n = size_t(pkt->size);
    // Find the first slice (VCL NAL types 0-31) and where its unit starts.
    size_t insertAt = n;
    if (lengthSize == 0) {
        size_t i = 0;
        while (i + 3 <= n) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                const size_t start = i > 0 && d[i - 1] == 0 ? i - 1 : i;  // a four-byte start code
                if (i + 3 < n && ((d[i + 3] >> 1) & 0x3f) < 32) {
                    insertAt = start;
                    break;
                }
                i += 3;
            } else {
                ++i;
            }
        }
    } else {
        size_t i = 0;
        while (i + size_t(lengthSize) < n) {
            size_t len = 0;
            for (int k = 0; k < lengthSize; ++k) len = (len << 8) | d[i + size_t(k)];
            if (((d[i + size_t(lengthSize)] >> 1) & 0x3f) < 32) {
                insertAt = i;
                break;
            }
            i += size_t(lengthSize) + len;
        }
    }
    if (insertAt >= n) return false;
    std::vector<uint8_t> out(d, d + insertAt);
    if (lengthSize == 0) {
        out.insert(out.end(), {0, 0, 0, 1});
    } else {
        for (int k = lengthSize - 1; k >= 0; --k) out.push_back(uint8_t(nal.size() >> (8 * k)));
    }
    out.insert(out.end(), nal.begin(), nal.end());
    out.insert(out.end(), d + insertAt, d + n);
    return replaceData(pkt, out);
}

bool addHdr10PlusObu(AVPacket* pkt, const std::vector<uint8_t>& t35) {
    if (!pkt || !pkt->data || pkt->size <= 0 || t35.empty()) return false;
    // The temporal unit's shown frame is its last frame (or frame header) OBU: the metadata goes just before it.
    const uint8_t* d = pkt->data;
    const uint8_t* end = d + pkt->size;
    const uint8_t* p = d;
    const uint8_t* lastFrame = nullptr;
    while (p < end) {
        const uint8_t header = *p;
        const int type = (header >> 3) & 0xf;
        const bool extension = header & 0x04, hasSize = header & 0x02;
        const uint8_t* q = p + 1 + (extension ? 1 : 0);
        if (q > end) return false;
        uint64_t size = uint64_t(end - q);
        if (hasSize) {
            int len = 0;
            if (!readLeb128(q, end, size, len)) return false;
            q += len;
        }
        if (size > uint64_t(end - q)) return false;
        if (type == 3 || type == 6) lastFrame = p;  // OBU_FRAME_HEADER, OBU_FRAME
        p = q + size;
        if (!hasSize) break;  // (only the last OBU may run to the end)
    }
    if (!lastFrame) return false;
    std::vector<uint8_t> payload;
    writeLeb128(payload, 4);  // metadata_type: ITU-T T.35
    payload.insert(payload.end(), t35.begin(), t35.end());
    payload.push_back(0x80);  // trailing bits
    std::vector<uint8_t> out(d, lastFrame);
    out.push_back(0x2A);  // OBU_METADATA (5), with a size field
    writeLeb128(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), lastFrame, end);
    return replaceData(pkt, out);
}

std::string hdr10PlusJson(const std::vector<Hdr10PlusScene>& scenes, FrameTime from, FrameTime to) {
    // One entry per frame, each scene's values repeated over its frames, as hdr10plus_tool writes and x265 reads.
    QJsonArray info, firsts, counts;
    int id = 0;
    for (const Hdr10PlusScene& sc : scenes) {
        const FrameTime a = std::max(sc.start, from), b = std::min(sc.end, to);
        if (b <= a) continue;
        QJsonObject lum;
        lum["AverageRGB"] = double(linearCode(sc.average));
        lum["MaxScl"] = QJsonArray{double(linearCode(sc.maxScl[0])), double(linearCode(sc.maxScl[1])), double(linearCode(sc.maxScl[2]))};
        uint32_t v[9];
        distribution(sc, v);
        QJsonArray index, values;
        for (int i = 0; i < 9; ++i) index.append(kPercentages[i]), values.append(double(v[i]));
        lum["LuminanceDistributions"] = QJsonObject{{"DistributionIndex", index}, {"DistributionValues", values}};
        firsts.append(double(a - from));
        counts.append(double(b - a));
        for (FrameTime f = a; f < b; ++f) {
            QJsonObject e;
            e["LuminanceParameters"] = lum;
            e["NumberOfWindows"] = 1;
            e["TargetedSystemDisplayMaximumLuminance"] = 0;
            e["SceneFrameIndex"] = double(f - a);
            e["SceneId"] = id;
            e["SequenceFrameIndex"] = double(f - from);
            info.append(e);
        }
        ++id;
    }
    QJsonObject root;
    root["JSONInfo"] = QJsonObject{{"HDR10plusProfile", "A"}, {"Version", "1.0"}};
    root["SceneInfo"] = info;
    root["SceneInfoSummary"] = QJsonObject{{"SceneFirstFrameIndex", firsts}, {"SceneFrameNumbers", counts}};
    root["ToolInfo"] = QJsonObject{{"Tool", "Montage"}, {"Version", "1.0"}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool writeHdr10PlusJson(const std::string& path, const std::vector<Hdr10PlusScene>& scenes, FrameTime from, FrameTime to,
                        std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    const std::string json = hdr10PlusJson(scenes, from, to);
    if (f.write(json.data(), qint64(json.size())) != qint64(json.size())) {
        if (error) *error = "Writing " + path + " failed";
        return false;
    }
    return true;
}

bool readHdr10PlusJson(const std::string& path, std::vector<Hdr10PlusScene>& scenes, std::string* error) {
    scenes.clear();
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    const QJsonArray info = doc.object().value("SceneInfo").toArray();
    if (pe.error != QJsonParseError::NoError || info.isEmpty()) {
        if (error) *error = path + " is not HDR10+ metadata JSON";
        return false;
    }
    int lastId = -1;
    for (const auto& v : info) {
        const QJsonObject e = v.toObject();
        const int id = e.value("SceneId").toInt();
        const FrameTime frame = FrameTime(e.value("SequenceFrameIndex").toDouble());
        if (id == lastId && !scenes.empty()) {
            scenes.back().end = frame + 1;
            continue;
        }
        lastId = id;
        const QJsonObject lum = e.value("LuminanceParameters").toObject();
        Hdr10PlusScene sc;
        sc.start = frame;
        sc.end = frame + 1;
        const QJsonArray mx = lum.value("MaxScl").toArray();
        for (int c = 0; c < 3 && c < mx.size(); ++c) sc.maxScl[c] = mx.at(c).toDouble() / 10;
        sc.average = lum.value("AverageRGB").toDouble() / 10;
        const QJsonObject dist = lum.value("LuminanceDistributions").toObject();
        const QJsonArray index = dist.value("DistributionIndex").toArray(), values = dist.value("DistributionValues").toArray();
        for (int i = 0; i < index.size() && i < values.size(); ++i) {
            const int pc = index.at(i).toInt();
            const double nits = values.at(i).toDouble() / 10;
            const int slot = pc == 1 ? 0 : pc == 25 ? 1 : pc == 50 ? 2 : pc == 75 ? 3 : pc == 90 ? 4 : pc == 95 ? 5 : pc == 99 ? 6 : -1;
            if (slot >= 0) sc.percentiles[slot] = nits;
        }
        scenes.push_back(sc);
    }
    return true;
}

}  // namespace montage
