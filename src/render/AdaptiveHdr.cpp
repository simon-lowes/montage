// Montage — adaptive HDR metadata (SMPTE ST 2094-50) in pictures and files. See AdaptiveHdr.h.
#include "AdaptiveHdr.h"

#include <QCryptographicHash>
#include <QFloat16>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include "render/Compositor.h"
#include "render/Hdr10Plus.h"
#include "render/Processing.h"
#include "render/TrimPasses.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace montage {

namespace {

constexpr double kReferenceWhite = 203;  // BT.2408's, which Montage's HDR spaces put SDR white at

void apply3(const double m[9], const double in[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
}

uint64_t leb128(const uint8_t*& p, const uint8_t* end, bool& ok) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        if (p >= end) {
            ok = false;
            return 0;
        }
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return v;
    }
    return v;
}

// The SEI messages of an HEVC SEI NAL unit's payload (after its two-byte header), emulation prevention removed.
void seiT35(const uint8_t* d, size_t n, const std::function<void(const uint8_t*, size_t)>& fn) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && d[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = d[i] == 0 ? zeros + 1 : 0;
        rbsp.push_back(d[i]);
    }
    size_t i = 0;
    while (i + 2 <= rbsp.size() && rbsp[i] != 0x80) {
        size_t type = 0, size = 0;
        while (i < rbsp.size() && rbsp[i] == 0xff) type += 255, ++i;
        if (i >= rbsp.size()) return;
        type += rbsp[i++];
        while (i < rbsp.size() && rbsp[i] == 0xff) size += 255, ++i;
        if (i >= rbsp.size()) return;
        size += rbsp[i++];
        if (i + size > rbsp.size()) return;
        if (type == 4) fn(rbsp.data() + i, size);  // user data registered by ITU-T T.35
        i += size;
    }
}

int nalLengthSize(const AVCodecParameters* par) {
    // An hvcC record's lengthSizeMinusOne; Annex B start codes otherwise.
    return par->codec_id == AV_CODEC_ID_HEVC && par->extradata_size > 22 && par->extradata[0] == 1 ? (par->extradata[21] & 3) + 1 : 0;
}

}  // namespace

// ---- In files -----------------------------------------------------------------------------------------------------

void forEachT35(const uint8_t* d, size_t n, bool av1, int lengthSize, const std::function<void(const uint8_t*, size_t)>& fn) {
    if (!d || n == 0) return;
    if (av1) {
        const uint8_t* p = d;
        const uint8_t* end = d + n;
        while (p < end) {
            const uint8_t header = *p++;
            const int type = (header >> 3) & 0xf;
            if (header & 0x04) ++p;  // extension
            bool ok = true;
            const uint64_t size = (header & 0x02) ? leb128(p, end, ok) : uint64_t(end - p);
            if (!ok || p > end || size > uint64_t(end - p)) return;
            if (type == 5) {  // OBU_METADATA
                const uint8_t* q = p;
                const uint8_t* e = p + size;
                const uint64_t metadataType = leb128(q, e, ok);
                // (less the OBU's trailing bits: a one bit, then zeros to the end)
                while (e > q && e[-1] == 0) --e;
                if (e > q && e[-1] == 0x80) --e;
                if (ok && metadataType == 4 && q < e) fn(q, size_t(e - q));  // ITU-T T.35
            }
            p += size;
        }
        return;
    }
    auto nal = [&](const uint8_t* u, size_t len) {
        if (len < 3) return;
        const int type = (u[0] >> 1) & 0x3f;
        if (type == 39 || type == 40) seiT35(u + 2, len - 2, fn);  // prefix and suffix SEI
    };
    if (lengthSize > 0) {
        size_t i = 0;
        while (i + size_t(lengthSize) <= n) {
            size_t len = 0;
            for (int k = 0; k < lengthSize; ++k) len = (len << 8) | d[i + size_t(k)];
            i += size_t(lengthSize);
            if (len > n - i) return;
            nal(d + i, len);
            i += len;
        }
        return;
    }
    // Start codes: each unit runs to the next one.
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 3 <= n; ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t e = i;
                while (e > start && d[e - 1] == 0) --e;
                nal(d + start, e - start);
            }
            start = i + 3;
            i += 2;
        }
    if (start != SIZE_MAX && start < n) nal(d + start, n - start);
}

bool readAdaptiveHdr(const std::string& path, std::vector<AdaptiveHdrScene>& scenes, std::string* error, bool whole) {
    scenes.clear();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        if (error) *error = "Cannot open " + path;
        return false;
    }
    struct Close {
        AVFormatContext*& f;
        ~Close() { avformat_close_input(&f); }
    } close{fmt};
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) return false;
    AVStream* st = fmt->streams[v];
    const AVCodecID codec = st->codecpar->codec_id;
    if (codec != AV_CODEC_ID_HEVC && codec != AV_CODEC_ID_AV1) return false;
    const bool av1 = codec == AV_CODEC_ID_AV1;
    const int lengthSize = nalLengthSize(st->codecpar);
    AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = AVRational{30, 1};
    const double fps = av_q2d(fr), tb = av_q2d(st->time_base);
    // (frames counted from the file's start, as the decoder's media time is)
    const double origin = fmt->start_time != AV_NOPTS_VALUE ? double(fmt->start_time) / AV_TIME_BASE : 0.0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != v) fmt->streams[i]->discard = AVDISCARD_ALL;
    // Each frame's message, in display order (packets come in decode order).
    std::map<int64_t, std::vector<uint8_t>> messages;
    int64_t frames = 0, last = -1;
    AVPacket* pkt = av_packet_alloc();
    constexpr int64_t kFirstFrames = 120;  // a file without any in these is taken to have none
    // Read on import, so only the first ten minutes there (the rest when it is needed).
    const int64_t kMaxFrames = whole ? INT64_MAX : std::llround(600 * fps);
    bool cut = false;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == v) {
            const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const int64_t index = ts != AV_NOPTS_VALUE ? std::llround((double(ts) * tb - origin) * fps) : frames;
            if (index >= kMaxFrames) {
                av_packet_unref(pkt);
                cut = true;
                break;
            }
            last = std::max(last, index);
            ++frames;
            forEachT35(pkt->data, size_t(pkt->size), av1, lengthSize, [&](const uint8_t* t35, size_t size) {
                if (isAdaptiveHdrT35(t35, size)) messages[index].assign(t35, t35 + size);
            });
        }
        av_packet_unref(pkt);
        if (messages.empty() && frames >= kFirstFrames) break;
    }
    av_packet_free(&pkt);
    if (messages.empty()) return false;
    const int64_t end = cut ? std::min(kMaxFrames, last + 1) : std::max(last + 1, frames);
    // A message lasts until the next one; the same message over and over (or one that tone maps alike) is one scene.
    const std::vector<uint8_t>* current = nullptr;
    for (auto it = messages.begin(); it != messages.end(); ++it) {
        if (current && *current == it->second) continue;
        AdaptiveHdr md;
        if (!parseAdaptiveHdrT35(it->second.data(), it->second.size(), md)) {
            current = nullptr;
            continue;
        }
        if (current && !scenes.empty() && adaptiveHdrAlike(scenes.back().md, md)) {
            current = &it->second;
            continue;
        }
        if (!scenes.empty() && scenes.back().end > it->first) scenes.back().end = it->first;
        AdaptiveHdrScene sc;
        sc.start = std::max<int64_t>(0, it->first);
        sc.end = end;
        sc.md = md;
        scenes.push_back(std::move(sc));
        current = &it->second;
    }
    std::erase_if(scenes, [](const AdaptiveHdrScene& s) { return s.end <= s.start; });
    if (cut && !scenes.empty()) scenes.back().key = kAdaptiveHdrMore;
    return !scenes.empty();
}

const AdaptiveHdr* adaptiveHdrAt(const MediaItem& m, double seconds, bool* standIn) {
    if (!m.adaptiveHdr || m.adaptiveHdr->empty()) return nullptr;
    const double fps = m.fps.valid() ? m.fps.toDouble() : 30;
    const FrameTime f = FrameTime(std::llround(std::max(0.0, seconds) * fps));
    const std::vector<AdaptiveHdrScene>* list = m.adaptiveHdr.get();
    // Past the part read on import: the whole file, read once in the background from the first time the clip is asked
    // about (so playback seldom waits for it), and kept for as long as Montage runs, for each version of the file.
    if (list->back().key == kAdaptiveHdrMore) {
        using Scenes = std::shared_ptr<const std::vector<AdaptiveHdrScene>>;
        static std::mutex mutex;
        static std::map<std::string, std::shared_future<Scenes>> wholeFiles;
        std::shared_future<Scenes> whole;
        {
            std::error_code ec;
            const std::filesystem::path p(std::u8string(m.path.begin(), m.path.end()));
            const auto size = std::filesystem::file_size(p, ec);
            const auto time = std::filesystem::last_write_time(p, ec);
            const std::string key = m.path + '|' + std::to_string((unsigned long long)size) + '|' +
                                    std::to_string((long long)time.time_since_epoch().count());
            const std::lock_guard<std::mutex> lock(mutex);
            auto& slot = wholeFiles[key];
            if (!slot.valid()) {
                std::promise<Scenes> promise;
                slot = promise.get_future().share();
                std::thread([path = m.path, promise = std::move(promise)]() mutable {
                    auto read = std::make_shared<std::vector<AdaptiveHdrScene>>();
                    readAdaptiveHdr(path, *read, nullptr, true);
                    promise.set_value(std::move(read));
                }).detach();
            }
            whole = slot;
        }
        if (f >= list->back().end) {
            // (Not waited for by a view that cannot wait: the last scene read stands in until the rest is.)
            if (standIn && whole.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                *standIn = true;
                return &list->back().md;
            }
            list = whole.get().get();
        }
    }
    const auto& scenes = *list;
    auto it = std::upper_bound(scenes.begin(), scenes.end(), f, [](FrameTime t, const AdaptiveHdrScene& s) { return t < s.start; });
    if (it == scenes.begin()) return nullptr;
    --it;
    return f < it->end ? &it->md : nullptr;
}

// ---- In pictures --------------------------------------------------------------------------------------------------

double spaceHeadroom(const ColorSpace& space, double peakNits) {
    if (space.transfer == Transfer::Pq) return std::max(0.0, std::log2(peakNits / kReferenceWhite));
    if (space.transfer == Transfer::Hlg) return std::log2(1000 / kReferenceWhite);
    return 0;
}

void adaptiveGainMatrices(const AdaptiveHdr& md, Primaries p, double toGain[9], double fromGain[9]) {
    double gainToXyz[9], xyzToGain[9], pToXyz[9], xyzToP[9];
    chromaticitiesToXyz(md.chroma, gainToXyz);
    invertMatrix3(gainToXyz, xyzToGain);
    primariesToXyz(p, pToXyz);
    invertMatrix3(pToXyz, xyzToP);
    multiplyMatrix3(xyzToGain, pToXyz, toGain);
    multiplyMatrix3(xyzToP, gainToXyz, fromGain);
}

bool adaptiveConverts(const AdaptiveHdr& md, const ColorSpace& from, const ColorSpace& to, double peakNits) {
    if (from.sceneReferred || to.sceneReferred) return false;
    if (from.hdr() && std::fabs(md.referenceWhite - kReferenceWhite) > 0.5) return true;
    return adaptiveToneMaps(md, spaceHeadroom(to, peakNits));
}

namespace {
// convertPixelAdaptive with its matrices worked out.
struct AdaptivePixel {
    AdaptivePixel(const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits) : from(from), md(md), to(to) {
        double unused[9];
        adaptiveGainMatrices(md, from.primaries, toGain, unused);
        adaptiveGainMatrices(md, to.primaries, unused, fromGain);
        headroom = spaceHeadroom(to, peakNits);
        scale = from.hdr() ? kReferenceWhite / md.referenceWhite : 1;
        toneMaps = adaptiveToneMaps(md, headroom);
        rollOff = !toneMaps && from.hdr() && !to.hdr();
    }
    void operator()(float rgb[3]) const {
        double l[3], c[3];
        displayLight(from, rgb, l);
        apply3(toGain, l, c);
        for (double& v : c) v *= scale;
        if (toneMaps) adaptiveToneMap(md, c, headroom);
        apply3(fromGain, c, l);
        for (double& v : l) v = std::max(v, 0.0);
        if (rollOff) {
            // (as convertColor maps HDR into SDR: the brightest channel rolled off above 75 % of white, hue kept)
            const double mx = std::max({l[0], l[1], l[2]});
            constexpr double knee = 0.75;
            if (mx > knee) {
                const double out = knee + (1 - knee) * (1 - std::exp(-(mx - knee) / (1 - knee)));
                for (double& v : l) v *= out / mx;
            }
        }
        const double top = std::exp2(headroom);
        for (double& v : l) v = std::min(v, top);
        encodeDisplayLight(to, l, rgb);
    }
    const ColorSpace& from;
    const AdaptiveHdr& md;
    const ColorSpace& to;
    double toGain[9], fromGain[9];
    double headroom = 0, scale = 1;
    bool toneMaps = false, rollOff = false;
};
}  // namespace

void convertPixelAdaptive(float rgb[3], const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits) {
    AdaptivePixel(from, md, to, peakNits)(rgb);
}

std::shared_ptr<const Lut3D> adaptiveConversionLut(const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits) {
    const std::vector<uint8_t> payload = adaptiveHdrPayload(md);
    std::string key = from.id + "|" + to.id + "|" + std::to_string(std::lround(peakNits)) + "|";
    key.append(payload.begin(), payload.end());
    key += std::to_string(md.referenceWhite);
    static std::mutex m;
    static std::list<std::pair<std::string, std::shared_ptr<const Lut3D>>> cache;  // most recent first
    {
        std::lock_guard lock(m);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) {
                cache.splice(cache.begin(), cache, it);
                return it->second;
            }
    }
    auto lut = std::make_shared<Lut3D>();
    const int n = 65;
    lut->size = n;
    lut->data.resize(size_t(n) * n * n * 3);
    const AdaptivePixel convert(from, md, to, peakNits);
    parallelRows(n, [&](int b0, int b1) {
        for (int b = b0; b < b1; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    float px[3] = {float(r) / (n - 1), float(g) / (n - 1), float(b) / (n - 1)};
                    convert(px);
                    float* d = &lut->data[(size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3];
                    d[0] = px[0], d[1] = px[1], d[2] = px[2];
                }
    });
    std::lock_guard lock(m);
    cache.emplace_front(key, lut);
    if (cache.size() > 16) cache.pop_back();
    return lut;
}

void convertColorAdaptive(Image& img, const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits) {
    if (img.empty()) return;
    const std::shared_ptr<const Lut3D> lut = adaptiveConversionLut(from, md, to, peakNits);
    const AdaptivePixel convert(from, md, to, peakNits);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                if (c[0] >= 0 && c[0] <= 1 && c[1] >= 0 && c[1] <= 1 && c[2] >= 0 && c[2] <= 1) {
                    lut->apply(c[0], c[1], c[2]);
                } else {
                    for (float& v : c) v = std::isfinite(v) ? v : 0.0f;
                    convert(c);
                }
                p[0] = c[0] * a, p[1] = c[1] * a, p[2] = c[2] * a;
            }
        }
    });
}

void adaptiveViewImage(std::vector<uint16_t>& rgba, const AdaptiveHdr& md, double headroom) {
    const double h = std::log2(std::max(headroom, 1.0));
    if (!adaptiveToneMaps(md, h) || rgba.empty()) return;
    double toGain[9], fromGain[9];
    adaptiveGainMatrices(md, Primaries::Bt709, toGain, fromGain);
    // Curves on the brightest component alone (as Montage makes them) give one gain for the whole colour, the same in
    // any primaries: a table of it by that component's stops, 1/64 stop apart, from -12 to +8.
    const bool brightest = std::all_of(md.gains.begin(), md.gains.end(), [](const AdaptiveGainCurve& g) {
        return g.mix[0] == 0 && g.mix[1] == 0 && g.mix[2] == 0 && g.mix[3] == 1 && g.mix[4] == 0 && g.mix[5] == 0;
    });
    constexpr int kSteps = 64, kLow = -12, kHigh = 8;
    std::vector<float> table;
    if (brightest) {
        table.resize(size_t((kHigh - kLow) * kSteps + 1));
        for (size_t i = 0; i < table.size(); ++i) {
            const double v = std::exp2(double(i) / kSteps + kLow), c[3] = {v, v, v};
            double g[3];
            adaptiveGain(md, c, h, g);
            table[i] = float(std::exp2(g[0]));
        }
    }
    const size_t pixels = rgba.size() / 4;
    const size_t chunk = 4096;
    parallelRows(int((pixels + chunk - 1) / chunk), [&](int c0, int c1) {
        std::vector<float> f(chunk * 4);
        for (int c = c0; c < c1; ++c) {
            const size_t first = size_t(c) * chunk, count = std::min(chunk, pixels - first);
            qFloatFromFloat16(f.data(), reinterpret_cast<const qfloat16*>(&rgba[first * 4]), qsizetype(count * 4));
            for (size_t i = 0; i < count; ++i) {
                float* p = &f[i * 4];
                const double l[3] = {p[0], p[1], p[2]};
                double g[3], o[3];
                apply3(toGain, l, g);
                if (brightest) {
                    const double mx = std::max({g[0], g[1], g[2]});
                    double k = 1;
                    if (mx > 0) {
                        const double at = std::clamp((std::log2(mx) - kLow) * kSteps, 0.0, double(table.size() - 1));
                        const size_t j = std::min(size_t(at), table.size() - 2);
                        k = table[j] + (table[j + 1] - table[j]) * (at - double(j));
                    }
                    for (int q = 0; q < 3; ++q) o[q] = l[q] * k;
                } else {
                    adaptiveToneMap(md, g, h);
                    apply3(fromGain, g, o);
                }
                for (int k = 0; k < 3; ++k) p[k] = float(std::min(o[k], headroom));
            }
            qFloatToFloat16(reinterpret_cast<qfloat16*>(&rgba[first * 4]), f.data(), qsizetype(count * 4));
        }
    });
}

// ---- Making it ----------------------------------------------------------------------------------------------------

namespace {
constexpr int kPeakBinsPerStop = 64, kPeakLow = -12, kPeakHigh = 7;
constexpr int kGainBinsPerStop = 8, kGainLow = -6, kGainHigh = 6;
constexpr size_t kMaxPerBin = 4096;

int chromaModeFor(Primaries p) { return p == Primaries::Bt709 ? 0 : p == Primaries::P3D65 ? 1 : 2; }

// A curve through these points as the message carries them (x to 1/1000, y to 1/10000), slopes by PCHIP.
AdaptiveGainCurve curveThrough(const std::vector<std::pair<double, double>>& pts) {
    AdaptiveGainCurve g;
    for (const auto& [x, y] : pts) {
        const double qx = std::round(x * 1000) / 1000, qy = std::round(y * 10000) / 10000;
        if (!g.x.empty() && qx <= g.x.back()) continue;
        g.x.push_back(qx);
        g.y.push_back(qy);
    }
    g.pchip = true;
    pchipSlopes(g);
    return g;
}
}  // namespace

AdaptiveHdrFitter::AdaptiveHdrFitter(const ColorSpace& hdr, const ColorSpace& sdr) : hdr_(hdr), sdr_(sdr) {
    base_.toneMap = true;
    base_.chromaMode = chromaModeFor(sdr.primaries);
    sanitizeAdaptiveHdr(base_);  // (sets the chromaticities)
    double unused[9];
    adaptiveGainMatrices(base_, hdr.primaries, toGainHdr_, unused);
    adaptiveGainMatrices(base_, sdr.primaries, toGainSdr_, unused);
    peak_.assign(size_t((kPeakHigh - kPeakLow) * kPeakBinsPerStop), 0);
    gains_.resize(size_t((kGainHigh - kGainLow) * kGainBinsPerStop));
}

void AdaptiveHdrFitter::add(const Image& hdr, const Image& sdr) {
    if (hdr.empty() || hdr.width != sdr.width || hdr.height != sdr.height) return;
    const int stride = std::max(1, int(std::sqrt(double(hdr.width) * hdr.height / 65536.0)));
    for (int y = 0; y < hdr.height; y += stride) {
        const float* ph = hdr.row(y);
        const float* ps = sdr.row(y);
        for (int x = 0; x < hdr.width; x += stride) {
            const float* a = ph + size_t(x) * 4;
            const float* b = ps + size_t(x) * 4;
            if (a[3] < 0.99f || b[3] < 0.99f) continue;
            const float ca[3] = {a[0] / a[3], a[1] / a[3], a[2] / a[3]}, cb[3] = {b[0] / b[3], b[1] / b[3], b[2] / b[3]};
            double l[3], gh[3], gs[3];
            displayLight(hdr_, ca, l);
            apply3(toGainHdr_, l, gh);
            displayLight(sdr_, cb, l);
            apply3(toGainSdr_, l, gs);
            const double mh = std::max({gh[0], gh[1], gh[2]}), ms = std::max({gs[0], gs[1], gs[2]});
            if (!(mh > 0) || !std::isfinite(mh)) continue;
            const double stops = std::log2(mh);
            const int pb = std::clamp(int(std::floor((stops - kPeakLow) * kPeakBinsPerStop)), 0, int(peak_.size()) - 1);
            ++peak_[size_t(pb)];
            ++samples_;
            if (stops < kGainLow || !(ms > 1e-6)) continue;
            const int gb = std::min(int((stops - kGainLow) * kGainBinsPerStop), int(gains_.size()) - 1);
            auto& bin = gains_[size_t(gb)];
            const std::pair<float, float> g{float(stops), float(std::log2(ms / mh))};
            if (bin.size() < kMaxPerBin) bin.push_back(g);
            else bin[samples_ % kMaxPerBin] = g;
        }
    }
}

AdaptiveHdr AdaptiveHdrFitter::result() const {
    AdaptiveHdr md = base_;
    uint64_t total = 0;
    for (uint32_t c : peak_) total += c;
    if (total == 0) return md;
    // The picture's headroom: its brightest component, but for the brightest one pixel in ten thousand (the foot of
    // its bin, so a display with the picture's own peak is never tone mapped).
    uint64_t above = 0;
    int top = int(peak_.size()) - 1;
    while (top > 0 && (above += peak_[size_t(top)]) <= total / 10000) --top;
    const double peak = double(top) / kPeakBinsPerStop + kPeakLow;
    md.baseline = std::clamp(std::round(peak * 10000) / 10000, 0.0, 6.0);
    if (md.baseline < 1.0 / 64) {
        md.baseline = 0;
        return md;  // (no brighter than SDR white: nothing to tone map)
    }
    // Each bin's typical light and gain (their medians: the gain rises or falls with the light), up to the headroom,
    // where it has enough pixels to say.
    const double xTop = std::exp2(md.baseline);
    std::vector<std::pair<double, double>> measured;
    for (const auto& bin : gains_) {
        if (bin.size() < 8) continue;
        std::vector<float> lx, g;
        for (const auto& [a, b] : bin) lx.push_back(a), g.push_back(b);
        const auto mid = std::ptrdiff_t(bin.size() / 2);
        std::nth_element(lx.begin(), lx.begin() + mid, lx.end());
        std::nth_element(g.begin(), g.begin() + mid, g.end());
        const double x = std::exp2(double(lx[size_t(mid)]));
        if (x >= xTop * 0.97) break;
        measured.emplace_back(x, std::clamp(double(g[size_t(mid)]), -6.0, 0.0));
    }
    if (measured.empty()) measured.emplace_back(std::min(1.0, xTop / 2), 0);
    // At the headroom, the SDR rendition at its white (ST 2094-50's 6.2.3: its peak goes to its own headroom, 0), or
    // as far below it as the brightest measured light shows it (a trim that holds highlights down).
    double endY = -md.baseline;
    if (measured.back().first >= xTop / 2) endY = std::min(endY, measured.back().second + std::log2(measured.back().first / xTop));
    const std::pair<double, double> end{xTop, std::max(endY, -6.0)};
    // Points added where the curve through those chosen so far is furthest off, up to 32.
    std::vector<size_t> chosen{0};
    auto points = [&] {
        std::vector<std::pair<double, double>> pts;
        std::vector<size_t> sorted = chosen;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i : sorted) pts.push_back(measured[i]);
        pts.push_back(end);
        // Light out must not fall as light in rises.
        for (size_t i = 1; i < pts.size(); ++i) pts[i].second = std::max(pts[i].second, pts[i - 1].second - std::log2(pts[i].first / pts[i - 1].first));
        for (auto& p : pts) p.second = std::min(p.second, 0.0);
        return pts;
    };
    AdaptiveGainCurve curve = curveThrough(points());
    while (chosen.size() + 1 < 32) {
        double worst = 0.004;
        size_t at = SIZE_MAX;
        for (size_t i = 0; i < measured.size(); ++i) {
            if (std::find(chosen.begin(), chosen.end(), i) != chosen.end()) continue;
            const double e = std::fabs(adaptiveGainCurve(curve, measured[i].first) - measured[i].second);
            if (e > worst) worst = e, at = i;
        }
        if (at == SIZE_MAX) break;
        chosen.push_back(at);
        curve = curveThrough(points());
    }
    md.alternate = {0};
    md.gains = {curve};
    return md;
}

const TrimTarget* adaptiveSdrTarget(const Sequence& s) {
    for (const TrimTarget& t : s.trimTargets) {
        const ColorSpace* cs = findColorSpace(t.colorSpace);
        if (cs && !cs->hdr() && !cs->sceneReferred) return &t;
    }
    return nullptr;
}

std::string adaptiveHdrFramesKey(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits) {
    // The pictures, and every trim each frame takes (the SDR rendition comes from one, an HDR delivery from another).
    std::string trims;
    for (const TrimTarget& t : s.trimTargets) trims += t.id + "," + t.colorSpace + "," + std::to_string(t.peakNits) + ";";
    for (FrameTime f = from; f < to; ++f) {
        const Clip* shot = trimShot(s, f);
        if (!shot || shot->trims.empty()) continue;
        trims += std::to_string(f) + ":";
        for (const auto& [id, e] : shot->trims) {
            trims += id;
            for (const TrimControl& c : trimControls()) trims += "," + std::to_string(e.p(c.name, f - shot->start, c.def));
        }
    }
    // (a hash that is the same on every platform, so a project's analysis is reused wherever it is opened)
    return hdr10PlusFramesKey(p, s, from, to, out, peakNits) + ":" +
           QCryptographicHash::hash(QByteArray::fromStdString(trims), QCryptographicHash::Sha1).toHex().toStdString();
}

bool analyseAdaptiveHdr(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                        std::vector<AdaptiveHdrScene>& scenes, std::string* error, const std::function<void(double)>& progress,
                        const std::atomic<bool>* cancel, const TrimTarget* hdrTrim) {
    scenes.clear();
    if (!out.hdr()) {
        if (error) *error = "Adaptive HDR metadata (ST 2094-50) is for PQ and HLG pictures, not " + out.label;
        return false;
    }
    from = std::max<FrameTime>(0, from);
    if (to <= from) to = s.duration();
    if (to <= from) {
        if (error) *error = "The sequence is empty";
        return false;
    }
    // Scenes: the cut's edits, none shorter than half a second (unless the range is).
    std::vector<FrameTime> bounds{from};
    const FrameTime minFrames = std::max<FrameTime>(2, std::llround(s.fpsValue() / 2));
    for (FrameTime c : hdr10PlusCuts(s, from, to))
        if (c - bounds.back() >= minFrames && to - c >= minFrames) bounds.push_back(c);
    bounds.push_back(to);
    const ColorSpace& space = sequenceColorSpace(s);
    const TrimTarget* sdrTarget = adaptiveSdrTarget(s);
    const ColorSpace& sdrSpace = sdrTarget ? trimTargetSpace(s, sdrTarget->id) : rec709Space();
    const ColorSpace& hdrFrom = hdrTrim ? trimTargetSpace(s, hdrTrim->id) : space;
    RenderOptions o;
    o.scale = std::min(1.0, 960.0 / std::max(1, s.width));  // (enough pixels for the statistics)
    o.highQuality = true;
    const bool graded = sceneReferredWorking(s);
    auto rendered = [&](FrameTime f, const std::string& trim, const ColorSpace* to, double peak) {
        RenderOptions r = o;
        r.trimTarget = trim;
        if (to) r.outputSpace = to->id, r.outputPeak = peak;
        return renderProgramFrame(p, s, f, r);
    };
    constexpr int kFramesPerScene = 4;
    const size_t count = bounds.size() - 1;
    for (size_t i = 0; i < count; ++i) {
        const FrameTime a = bounds[i], b = bounds[i + 1];
        AdaptiveHdrFitter fit(out, sdrSpace);
        const int n = int(std::min<FrameTime>(kFramesPerScene, b - a));
        for (int k = 0; k < n; ++k) {
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return false;
            }
            const FrameTime f = a + FrameTime((double(k) + 0.5) * double(b - a) / n);
            Image hdr, sdr;
            if (graded) {
                // Graded scene-referred, each picture as its delivery makes it: through its own output transform.
                hdr = hdrTrim ? rendered(f, hdrTrim->id, nullptr, 0) : rendered(f, {}, &out, peakNits);
                if (hdrTrim) convertColor(hdr, hdrFrom, out, peakNits);
                sdr = sdrTarget ? rendered(f, sdrTarget->id, nullptr, 0) : rendered(f, {}, &sdrSpace, 0);
            } else {
                const Image master = renderProgramFrame(p, s, f, o);
                hdr = master, sdr = master;
                if (hdrTrim) applyTrimPass(hdr, s, f, *hdrTrim);
                convertColor(hdr, hdrFrom, out, peakNits);
                if (sdrTarget) applyTrimPass(sdr, s, f, *sdrTarget);
                else convertColor(sdr, space, sdrSpace, s.hdrPeakNits);
            }
            fit.add(hdr, sdr);
            if (progress) progress((double(i) + double(k + 1) / n) / double(count));
        }
        AdaptiveHdrScene sc;
        sc.start = a;
        sc.end = b;
        sc.md = fit.result();
        sc.key = adaptiveHdrFramesKey(p, s, a, b, out, peakNits);
        scenes.push_back(std::move(sc));
    }
    return !scenes.empty();
}

bool adaptiveHdrForRange(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                         bool reuse, std::vector<AdaptiveHdrScene>& scenes, std::string* error, const std::function<void(double)>& progress,
                         const std::atomic<bool>* cancel, const TrimTarget* hdrTrim, FrameTime* measured) {
    scenes.clear();
    if (measured) *measured = 0;
    if (to <= from) return true;
    std::vector<AdaptiveHdrScene> kept;
    if (reuse && !hdrTrim)  // (stored scenes are the master's)
        for (const AdaptiveHdrScene& sc : s.adaptiveHdr) {
            if (sc.end <= from || sc.start >= to || sc.end > s.duration()) continue;
            if (adaptiveHdrFramesKey(p, s, sc.start, sc.end, out, peakNits) != sc.key) continue;
            AdaptiveHdrScene c = sc;
            c.start = std::max(c.start, from);
            c.end = std::min(c.end, to);
            kept.push_back(c);
        }
    std::vector<std::pair<FrameTime, FrameTime>> gaps;
    FrameTime at = from;
    for (const AdaptiveHdrScene& c : kept) {
        if (c.start > at) gaps.push_back({at, c.start});
        at = std::max(at, c.end);
    }
    if (at < to) gaps.push_back({at, to});
    FrameTime total = 0, done = 0;
    for (const auto& [a, b] : gaps) total += b - a;
    for (const auto& [a, b] : gaps) {
        std::vector<AdaptiveHdrScene> part;
        const FrameTime len = b - a;
        const auto partProgress = [&](double f) {
            if (progress && total > 0) progress((double(done) + f * double(len)) / double(total));
        };
        if (!analyseAdaptiveHdr(p, s, a, b, out, peakNits, part, error, partProgress, cancel, hdrTrim)) return false;
        kept.insert(kept.end(), part.begin(), part.end());
        done += len;
    }
    std::sort(kept.begin(), kept.end(), [](const AdaptiveHdrScene& x, const AdaptiveHdrScene& y) { return x.start < y.start; });
    scenes = std::move(kept);
    if (measured) *measured = total;
    return true;
}

void mergeAdaptiveHdrScenes(std::vector<AdaptiveHdrScene>& stored, const std::vector<AdaptiveHdrScene>& fresh) {
    if (fresh.empty()) return;
    const FrameTime a = fresh.front().start, b = fresh.back().end;
    std::erase_if(stored, [&](const AdaptiveHdrScene& o) { return o.end > a && o.start < b; });
    stored.insert(stored.end(), fresh.begin(), fresh.end());
    std::sort(stored.begin(), stored.end(), [](const AdaptiveHdrScene& x, const AdaptiveHdrScene& y) { return x.start < y.start; });
}

const AdaptiveHdr* storedAdaptiveHdrAt(const std::shared_ptr<const Project>& p, const Sequence& s, FrameTime t) {
    if (!p || s.adaptiveHdr.empty()) return nullptr;
    auto it = std::upper_bound(s.adaptiveHdr.begin(), s.adaptiveHdr.end(), t, [](FrameTime f, const AdaptiveHdrScene& sc) { return f < sc.start; });
    if (it == s.adaptiveHdr.begin()) return nullptr;
    --it;
    if (t >= it->end) return nullptr;
    // Checked against the cut once per project snapshot (each edit makes a new one).
    static std::mutex m;
    static std::weak_ptr<const Project> checkedFor;
    static std::map<std::pair<Id, FrameTime>, bool> checked;
    std::pair<Id, FrameTime> key{s.id, it->start};
    {
        std::lock_guard lock(m);
        if (checkedFor.lock() != p) checked.clear(), checkedFor = p;
        if (auto c = checked.find(key); c != checked.end()) return c->second ? &it->md : nullptr;
    }
    const bool ok = adaptiveHdrFramesKey(*p, s, it->start, it->end, sequenceColorSpace(s), std::clamp(s.hdrPeakNits, 100.0, 10000.0)) == it->key;
    std::lock_guard lock(m);
    if (checkedFor.lock() == p) checked[key] = ok;
    return ok ? &it->md : nullptr;
}

}  // namespace montage
