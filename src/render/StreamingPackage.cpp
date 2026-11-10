#include "StreamingPackage.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "core/Captions.h"
#include "core/Chapters.h"
#include "render/Ac3.h"
#include "render/ColorSpace.h"
#include "render/Scte35.h"
#include "render/TsSegments.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace montage {

namespace {

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool writeFile(const std::string& path, const void* data, size_t n) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(static_cast<const char*>(data), qint64(n)) == qint64(n) && f.flush();
}
bool writeFile(const std::string& path, const std::string& text) { return writeFile(path, text.data(), text.size()); }

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

// A quoted-string attribute in a playlist: no double quotes or line breaks.
std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s)
        if (c != '"' && c != '\n' && c != '\r') o += c;
    return o + "\"";
}

// The payload of the first box of this type in an MP4 file's bytes (sample entries' configuration boxes).
std::vector<uint8_t> boxPayload(const std::vector<uint8_t>& mp4, const char* type) {
    for (size_t i = 4; i + 4 <= mp4.size(); ++i) {
        if (std::memcmp(&mp4[i], type, 4) != 0) continue;
        const uint32_t size = uint32_t(mp4[i - 4]) << 24 | uint32_t(mp4[i - 3]) << 16 | uint32_t(mp4[i - 2]) << 8 | mp4[i - 1];
        if (size < 8 || i - 4 + size > mp4.size()) continue;
        return {mp4.begin() + long(i + 4), mp4.begin() + long(i - 4 + size)};
    }
    return {};
}

uint32_t be32(const uint8_t* d) { return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3]; }

// A fragment's samples presented at their own times: its first sample (in decode order) at `firstPts`. MP4 muxers
// that cannot shift decode times themselves count presentation from the first decode time, which with reordered
// pictures (B-frames) starts a picture or two early; the composition offsets are then lowered by that much (as signed,
// version 1 offsets), which CMAF allows, so every rendition's segments start at the same presentation times.
bool presentAt(uint8_t* d, size_t n, int64_t firstPts, std::string* why) {
    auto boxes = [&](size_t from, size_t to, auto&& each) {
        for (size_t at = from; at + 8 <= to;) {
            const uint32_t size = be32(d + at);
            if (size < 8 || at + size > to) return;
            each(at, size_t(size));
            at += size;
        }
    };
    bool ok = true;
    boxes(0, n, [&](size_t moof, size_t moofSize) {
        if (std::memcmp(d + moof + 4, "moof", 4) != 0) return;
        boxes(moof + 8, moof + moofSize, [&](size_t traf, size_t trafSize) {
            if (std::memcmp(d + traf + 4, "traf", 4) != 0) return;
            int64_t decode = -1, shift = 0;
            bool first = true;
            boxes(traf + 8, traf + trafSize, [&](size_t box, size_t size) {
                const uint8_t* b = d + box + 8;
                if (std::memcmp(d + box + 4, "tfdt", 4) == 0 && size >= 16)
                    decode = b[0] == 1 && size >= 20 ? int64_t(uint64_t(be32(b + 4)) << 32 | be32(b + 8)) : int64_t(be32(b + 4));
                if (std::memcmp(d + box + 4, "trun", 4) != 0 || size < 16 || decode < 0) return;
                const uint32_t flags = be32(b) & 0xffffff;
                const uint32_t count = be32(b + 4);
                size_t at = box + 16 + (flags & 0x1 ? 4 : 0) + (flags & 0x4 ? 4 : 0);
                const size_t stride = 4 * ((flags & 0x100 ? 1 : 0) + (flags & 0x200 ? 1 : 0) + (flags & 0x400 ? 1 : 0) + (flags & 0x800 ? 1 : 0));
                const size_t ctsAt = 4 * ((flags & 0x100 ? 1 : 0) + (flags & 0x200 ? 1 : 0) + (flags & 0x400 ? 1 : 0));
                if (at + stride * count > box + size) return void(ok = false);
                const bool hasCts = flags & 0x800;
                auto cts = [&](size_t i) { const uint32_t v = be32(d + at + i * stride + ctsAt); return b[0] == 1 ? int64_t(int32_t(v)) : int64_t(v); };
                if (first) {
                    shift = decode + (hasCts && count ? cts(0) : 0) - firstPts;
                    first = false;
                }
                if (shift == 0) return;
                if (!hasCts) return void(ok = false);
                for (size_t i = 0; i < count; ++i) {
                    const int64_t v = cts(i) - shift;
                    if (v < INT32_MIN || v > INT32_MAX) return void(ok = false);
                    const uint32_t u = uint32_t(int32_t(v));
                    uint8_t* w = d + at + i * stride + ctsAt;
                    w[0] = uint8_t(u >> 24), w[1] = uint8_t(u >> 16), w[2] = uint8_t(u >> 8), w[3] = uint8_t(u);
                }
                d[box + 8] = 1;  // (signed offsets)
            });
        });
    });
    if (!ok && why) *why = "cannot set a fragment's presentation times";
    return ok;
}

// Where the mdat box's payload starts in a fragment (moof, then mdat).
int64_t mdatPayload(const uint8_t* d, size_t n) {
    size_t at = 0;
    while (at + 8 <= n) {
        const uint32_t size = uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
        if (std::memcmp(d + at + 4, "mdat", 4) == 0) return int64_t(at + 8);
        if (size < 8) return -1;
        at += size;
    }
    return -1;
}

// Bits a second at the peak, as HLS defines it (the busiest run of whole segments lasting half to one and a half times
// the target duration, so a short segment at a split cannot inflate it), from each segment's bytes and seconds.
int64_t peakRate(const std::vector<std::pair<double, double>>& segs) {
    double longest = 0;
    for (const auto& s : segs) longest = std::max(longest, s.second);
    const double target = std::max(1.0, std::round(longest));
    double peak = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        double bytes = 0, length = 0;
        for (size_t j = i; j < segs.size() && length <= target * 1.5; ++j) {
            bytes += segs[j].first, length += std::max(0.0, segs[j].second);
            if (length >= target * 0.5 && length <= target * 1.5) peak = std::max(peak, bytes * 8 / length);
        }
    }
    if (peak == 0)  // (no such run: a package shorter than half the target)
        for (const auto& s : segs)
            if (s.second > 0) peak = std::max(peak, s.first * 8 / s.second);
    return int64_t(std::ceil(peak));
}

int64_t averageRate(const std::vector<std::pair<double, double>>& segs) {
    double bytes = 0, length = 0;
    for (const auto& s : segs) bytes += s.first, length += std::max(0.0, s.second);
    return length > 0 ? int64_t(std::ceil(bytes * 8 / length)) : 0;
}

// One rendition written as CMAF: an initialisation segment, then a fragment (moof + mdat) per media segment, each its
// own file, through FFmpeg's MP4 muxer with fragments cut where asked.
class CmafTrack {
public:
    struct Segment {
        int64_t start = INT64_MAX, end = INT64_MIN;  // presentation, in timescale
        int64_t bytes = 0, iframeBytes = 0;          // the file's size; the moof and the first sample's
        int64_t firstSample = 0;
        int64_t firstPts = 0;  // the first sample's, in decode order
        std::string file;
    };
    ~CmafTrack() {
        if (oc_) {
            if (oc_->pb) {
                uint8_t* b = nullptr;
                avio_close_dyn_buf(oc_->pb, &b);
                av_free(b);
                oc_->pb = nullptr;
            }
            avformat_free_context(oc_);
        }
    }
    // `files` false: the fragments are made (their times and sizes known) but not written.
    bool open(const std::string& dir, const AVCodecParameters* par, AVRational tb, std::string* error, bool files = true) {
        dir_ = dir;
        files_ = files;
        // (AC-3 and E-AC-3 sample entries describe the stream from its first frames: the moov waits for them)
        delayed_ = par->codec_id == AV_CODEC_ID_AC3 || par->codec_id == AV_CODEC_ID_EAC3;
        if (avformat_alloc_output_context2(&oc_, nullptr, "mp4", nullptr) < 0 || !oc_) return fail(error, "cannot make an MP4 muxer");
        oc_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;  // (Opus in MP4 on older FFmpeg)
        st_ = avformat_new_stream(oc_, nullptr);
        if (!st_ || avcodec_parameters_copy(st_->codecpar, par) < 0) return fail(error, "cannot add the stream");
        st_->codecpar->codec_tag = par->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h', 'v', 'c', '1') : 0;
        st_->time_base = tb;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags",
                    delayed_ ? "+frag_custom+empty_moov+delay_moov+default_base_moof+negative_cts_offsets+skip_trailer"
                             : "+frag_custom+empty_moov+default_base_moof+negative_cts_offsets+skip_trailer",
                    0);
        if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return fail(error, averr(rc));
        const int rc = avformat_write_header(oc_, &opts);
        av_dict_free(&opts);
        if (rc < 0) return fail(error, "cannot start the MP4 fragments: " + averr(rc));
        uint8_t* b = nullptr;
        const int n = avio_close_dyn_buf(oc_->pb, &b);
        oc_->pb = nullptr;
        init.assign(b, b + std::max(0, n));
        av_free(b);
        timescale = st_->time_base;
        if (!delayed_ && files_ && !writeFile(dir_ + "/init.mp4", init.data(), init.size())) return fail(error, "cannot write " + dir_ + "/init.mp4");
        return true;
    }
    // Takes a packet whose times are in `from`.
    bool write(AVPacket* pkt, AVRational from, std::string* error) {
        if (!oc_->pb) {
            if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return fail(error, averr(rc));
            segments.emplace_back();
            segments.back().firstSample = pkt->size;
            av_packet_rescale_ts(pkt, from, st_->time_base);
            segments.back().firstPts = pkt->pts;
            from = st_->time_base;
        }
        av_packet_rescale_ts(pkt, from, st_->time_base);
        pkt->stream_index = 0;
        Segment& s = segments.back();
        s.start = std::min(s.start, pkt->pts);
        s.end = std::max(s.end, pkt->pts + std::max<int64_t>(pkt->duration, 0));
        if (int rc = av_write_frame(oc_, pkt); rc < 0) return fail(error, "cannot write a sample: " + averr(rc));
        return true;
    }
    bool open() const { return oc_ && oc_->pb; }
    bool cut(std::string* error) {
        if (!oc_ || !oc_->pb) return true;
        int64_t head = 0;
        if (delayed_ && init.empty()) {
            av_write_frame(oc_, nullptr);  // (the first flush writes the header alone: ftyp and moov)
            head = avio_tell(oc_->pb);
        }
        av_write_frame(oc_, nullptr);  // the fragment
        uint8_t* full = nullptr;
        const int total = avio_close_dyn_buf(oc_->pb, &full);
        oc_->pb = nullptr;
        if (head > 0) {
            init.assign(full, full + std::min<int64_t>(head, std::max(0, total)));
            if (files_ && !writeFile(dir_ + "/init.mp4", init.data(), init.size())) {
                av_free(full);
                return fail(error, "cannot write " + dir_ + "/init.mp4");
            }
        }
        uint8_t* b = full + std::min<int64_t>(head, std::max(0, total));
        const int n = int(std::max<int64_t>(0, total - head));
        Segment& s = segments.back();
        char name[32];
        std::snprintf(name, sizeof name, "seg%05d.m4s", int(segments.size()));
        s.file = name;
        s.bytes = std::max(0, n);
        std::string why;
        if (s.bytes > 0 && !presentAt(b, size_t(s.bytes), s.firstPts, &why)) {
            av_free(full);
            return fail(error, why);
        }
        const int64_t payload = mdatPayload(b, size_t(s.bytes));
        s.iframeBytes = payload >= 0 ? payload + s.firstSample : s.bytes;
        const bool ok = !files_ || writeFile(dir_ + "/" + name, b, size_t(s.bytes));
        av_free(full);
        if (!ok) return fail(error, "cannot write " + dir_ + "/" + name);
        if (s.bytes == 0) return fail(error, "an empty fragment");
        return true;
    }
    bool finish(std::string* error) {
        if (!cut(error)) return false;
        if (avio_open_dyn_buf(&oc_->pb) >= 0) {
            av_write_trailer(oc_);
            uint8_t* b = nullptr;
            avio_close_dyn_buf(oc_->pb, &b);
            av_free(b);
            oc_->pb = nullptr;
        }
        return true;
    }
    double seconds(int64_t t) const { return double(t) * timescale.num / timescale.den; }
    // Bits a second: the peak as HLS defines it, and over all.
    std::vector<std::pair<double, double>> sizes() const {
        std::vector<std::pair<double, double>> v;
        for (const Segment& s : segments) v.emplace_back(double(s.bytes), seconds(std::max<int64_t>(0, s.end - s.start)));
        return v;
    }
    int64_t peakBitrate() const { return peakRate(sizes()); }
    int64_t averageBitrate() const { return averageRate(sizes()); }

    std::vector<uint8_t> init;
    std::vector<Segment> segments;
    AVRational timescale{1, 1};

private:
    bool fail(std::string* error, const std::string& why) {
        if (error) *error = why;
        return false;
    }
    std::string dir_;
    bool files_ = true, delayed_ = false;
    AVFormatContext* oc_ = nullptr;
    AVStream* st_ = nullptr;
};

// A rendition's MPEG-TS segments for HLS, beside its CMAF ones (render/TsSegments.h).
struct TsTrack {
    TsSegmenter mux;
    struct Segment {
        int64_t bytes = 0, iframeBytes = 0;
        double seconds = 0;
    };
    std::vector<Segment> segments;
    std::string dir;
    bool write(AVPacket* pkt, AVRational from, std::string* error) {
        AVPacket* copy = av_packet_clone(pkt);
        if (!copy) return false;
        const bool ok = mux.write(copy, from.num, from.den, error);
        av_packet_free(&copy);
        return ok;
    }
    bool cut(std::string* error) {
        if (!mux.started()) return true;
        std::vector<uint8_t> bytes;
        if (!mux.end(bytes, error)) return false;
        char name[32];
        std::snprintf(name, sizeof name, "/seg%05d.ts", int(segments.size() + 1));
        if (!writeFile(dir + name, bytes.data(), bytes.size())) {
            if (error) *error = "cannot write " + dir + name;
            return false;
        }
        segments.push_back({int64_t(bytes.size()), mux.iframeBytes, double(mux.finish - mux.start) / 90000});
        return true;
    }
    std::vector<std::pair<double, double>> sizes() const {
        std::vector<std::pair<double, double>> v;
        for (const Segment& s : segments) v.emplace_back(double(s.bytes), s.seconds);
        return v;
    }
};

struct Opened {
    AVFormatContext* fmt = nullptr;
    ~Opened() { avformat_close_input(&fmt); }
};

std::string isoDate(const QDateTime& t) { return t.toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzzZ").toStdString(); }

int64_t ticks(FrameTime frames, Rational fps) { return int64_t(std::llround(double(frames) * 90000.0 * fps.den / fps.num)); }

std::string pathName(const std::string& s) {
    std::string o;
    for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
    return o.empty() ? "und" : o;
}

std::string codecFamily(const std::string& codec) {
    return codec == "libx265" ? "hevc" : codec == "libsvtav1" ? "av1" : "h264";
}

// The configuration box an MP4 init segment carries for this codec.
std::string configBox(AVCodecID id) { return id == AV_CODEC_ID_H264 ? "avcC" : id == AV_CODEC_ID_HEVC ? "hvcC" : id == AV_CODEC_ID_AV1 ? "av1C" : ""; }

std::string codecNameOf(const AVCodecParameters* par, const CmafTrack& t) {
    const char* name = avcodec_get_name(par->codec_id);
    const std::string box = configBox(par->codec_id);
    return streamingCodecName(name ? name : "", box.empty() ? std::vector<uint8_t>{} : boxPayload(t.init, box.c_str()), par->profile);
}

struct VideoOut {
    StreamingRung rung;
    std::string name;  // folder
    int width = 0, height = 0;
    AVCodecContext* enc = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    std::unique_ptr<CmafTrack> track;
    std::unique_ptr<TsTrack> ts;
    size_t next = 1;  // the segment whose first frame comes next
    std::string codecs, range;
    ~VideoOut() {
        avcodec_free_context(&enc);
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
};

struct AudioOut {
    std::string name, label, language;
    bool main = false, described = false;
    int channels = 2, sampleRate = 48000;
    int stream = -1;
    AVRational tb{1, 1};
    std::unique_ptr<CmafTrack> track;
    std::unique_ptr<TsTrack> ts;
    size_t next = 1;
    int64_t offset = AV_NOPTS_VALUE;  // the first sample's time, which becomes 0
    std::string codecs;
};

struct TextOut {
    std::string name, label, language;
    std::vector<Caption> captions;  // from the range's start
    CaptionStyle style;
};

}  // namespace

std::vector<StreamingRung> defaultLadder(int width, int height, const std::string& codec, bool hdr) {
    std::vector<StreamingRung> out;
    if (width <= 0 || height <= 0) return out;
    const double aspect = double(width) / height;
    std::vector<int> heights = {height};
    for (int h : {1440, 1080, 720, 540, 360})
        if (h < height && heights.size() < 5) heights.push_back(h);
    const double scale = (codec == "libx264" ? 1.0 : 0.65) * (hdr ? 1.25 : 1.0);
    for (int h : heights) {
        StreamingRung r;
        r.codec = codec;
        r.height = h & ~1;
        r.width = int(std::lround(h * aspect / 2)) * 2;
        // About 0.1 bits a pixel at 25 to 30 frames a second for H.264.
        r.bitrate = int64_t(std::llround(double(r.width) * r.height * 2.6 * scale / 1000)) * 1000;
        out.push_back(r);
    }
    return out;
}

std::vector<StreamingAdBreak> streamingAdBreaks(const Sequence& seq, FrameTime in, FrameTime out) {
    std::vector<StreamingAdBreak> v;
    for (const Marker& m : seq.markers)
        if (m.adBreak && m.t >= in && m.t < out) v.push_back({m.t - in, std::clamp<FrameTime>(m.duration, 0, out - m.t), 0, m.name});
    std::stable_sort(v.begin(), v.end(), [](const StreamingAdBreak& a, const StreamingAdBreak& b) { return a.at < b.at; });
    for (size_t i = 0; i < v.size(); ++i) v[i].id = uint32_t(i + 1);
    return v;
}

std::vector<FrameTime> streamingSegments(const Sequence& seq, FrameTime in, FrameTime out, double seconds, bool cutAligned,
                                         const std::vector<FrameTime>& splitsAsked) {
    std::vector<FrameTime> starts;
    const FrameTime len = out - in;
    if (len <= 0) return starts;
    const double fps = seq.fpsValue() > 0 ? seq.fpsValue() : 25;
    const FrameTime n = std::max<FrameTime>(1, std::llround(std::clamp(seconds, 0.5, 60.0) * fps));
    const FrameTime second = std::max<FrameTime>(1, std::llround(fps));
    std::vector<FrameTime> splits;
    for (FrameTime f : splitsAsked)
        if (f > 0 && f < len) splits.push_back(f);
    std::sort(splits.begin(), splits.end());
    splits.erase(std::unique(splits.begin(), splits.end()), splits.end());
    std::vector<FrameTime> cuts;
    if (cutAligned) {
        for (const Track& t : seq.videoTracks)
            for (const Clip& c : t.clips)
                for (FrameTime f : {c.start, c.start + c.duration})
                    if (f > in && f < out) cuts.push_back(f - in);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    }
    FrameTime s = 0;
    starts.push_back(0);
    while (true) {
        FrameTime e = s + n;
        if (cutAligned) {
            FrameTime best = -1;
            for (FrameTime c : cuts)
                if (c >= s + n / 2 && c <= s + n + n / 2 && (best < 0 || std::llabs(c - (s + n)) < std::llabs(best - (s + n)))) best = c;
            if (best > s) e = best;
        }
        const auto sp = std::upper_bound(splits.begin(), splits.end(), s);
        if (sp != splits.end()) {
            if (*sp <= e) e = *sp;                                    // a split comes first
            else if (*sp - e < second && *sp - s <= n + n / 2) e = *sp;  // or so soon after that the next would be tiny
        }
        if (e >= len) break;
        // No last segment shorter than a second (unless a split makes it so).
        const auto later = std::upper_bound(splits.begin(), splits.end(), e);
        if (len - e < second && std::find(splits.begin(), splits.end(), e) == splits.end() && (later == splits.end() || *later >= len)) break;
        starts.push_back(e);
        s = e;
    }
    return starts;
}

std::string streamingCodecName(const std::string& codec, const std::vector<uint8_t>& c, int profile) {
    char buf[96];
    if (codec == "h264") {
        if (c.size() < 4) return "";
        std::snprintf(buf, sizeof buf, "avc1.%02X%02X%02X", c[1], c[2], c[3]);
        return buf;
    }
    if (codec == "hevc") {
        if (c.size() < 13) return "";
        const int space = c[1] >> 6, tier = (c[1] >> 5) & 1, idc = c[1] & 31;
        uint32_t compat = uint32_t(c[2]) << 24 | uint32_t(c[3]) << 16 | uint32_t(c[4]) << 8 | c[5], reversed = 0;
        for (int i = 0; i < 32; ++i) reversed |= ((compat >> i) & 1u) << (31 - i);
        std::string s = "hvc1.";
        if (space) s += char('A' + space - 1);
        std::snprintf(buf, sizeof buf, "%d.%X.%c%d", idc, reversed, tier ? 'H' : 'L', c[12]);
        s += buf;
        int last = 11;
        while (last >= 6 && c[size_t(last)] == 0) --last;
        for (int i = 6; i <= last; ++i) {
            std::snprintf(buf, sizeof buf, ".%X", c[size_t(i)]);
            s += buf;
        }
        return s;
    }
    if (codec == "av1") {
        if (c.size() < 3) return "";
        const int prof = c[1] >> 5, level = c[1] & 31, tier = c[2] >> 7, high = (c[2] >> 6) & 1, twelve = (c[2] >> 5) & 1;
        std::snprintf(buf, sizeof buf, "av01.%d.%02d%c.%02d", prof, level, tier ? 'H' : 'M', high ? (twelve ? 12 : 10) : 8);
        return buf;
    }
    if (codec == "aac") {
        std::snprintf(buf, sizeof buf, "mp4a.40.%d", profile >= 0 ? profile + 1 : 2);
        return buf;
    }
    if (codec == "opus") return "opus";  // (as the Opus-in-MP4 mapping names it; browsers want it lower case)
    if (codec == "ac3") return "ac-3";   // (ETSI TS 102 366 Annex F's names)
    if (codec == "eac3") return "ec-3";
    return "";
}

namespace {

// The package's own record of its rendition folders, so a later export knows which folders are its to replace.
constexpr const char* kPackageRecord = ".montage-package";

bool isRenditionName(const QString& name) {
    static const QRegularExpression ours(QStringLiteral("^(v\\d+_\\d+p_(h264|hevc|av1)|[as]\\d+_[A-Za-z0-9_-]+)$"));
    return ours.match(name).hasMatch();
}

// A folder with nothing in it but what a rendition writes (no subfolders).
bool holdsOnlyRenditionFiles(const QString& path) {
    QDir d(path);
    if (!d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()) return false;
    static const QRegularExpression file(QStringLiteral("^(seg\\d+(\\.\\d+)?\\.(m4s|vtt|ts)|init\\.mp4|playlist\\.m3u8|iframes\\.m3u8|all\\.(vtt|ttml))(\\.tmp)?$"));  // (.tmp: a live package's, left half written)
    static const QRegularExpression junk(QStringLiteral("^(\\.DS_Store|Thumbs\\.db|desktop\\.ini)$"));
    bool any = false;  // (an empty folder, or one of only junk, is not a rendition's: left alone)
    for (const QString& f : d.entryList(QDir::Files | QDir::Hidden | QDir::System)) {
        if (file.match(f).hasMatch()) any = true;
        else if (!junk.match(f).hasMatch()) return false;
    }
    return any;
}

}  // namespace

bool streamingRenditionFolder(const std::string& path) { return holdsOnlyRenditionFiles(QString::fromStdString(path)); }

bool exportStreamingPackage(const Project& p, const Sequence& seq, const StreamingSettings& s, const ExportProgress& progress,
                            const std::atomic<bool>* cancel, std::string* error, StreamingResult* result) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    auto cancelled = [&] { return cancel && cancel->load(); };
    auto report = [&](double f) {
        if (progress) progress(std::clamp(f, 0.0, 1.0), 0);
    };
    StreamingResult res;
    if (s.folder.empty()) return fail("Choose a folder for the package");
    if (!s.hls && !s.dash) return fail("Choose HLS, DASH or both");
    const FrameTime in = std::max<FrameTime>(0, s.in), out = s.out >= 0 ? std::min(s.out, seq.duration()) : seq.duration();
    if (out <= in) return fail("The range is empty");
    const Rational fps = seq.fps.valid() ? seq.fps : Rational{25, 1};
    const FrameTime len = out - in;
    const ColorSpace* space = findColorSpace(s.colorSpace.empty() ? seq.colorSpace : s.colorSpace);
    const bool hdr = space && space->hdr();
    std::vector<StreamingRung> rungs = s.rungs.empty() ? defaultLadder(seq.width, seq.height, "libx264", hdr) : s.rungs;
    for (const StreamingRung& r : rungs) {
        if (r.codec != "libx264" && r.codec != "libx265" && r.codec != "libsvtav1") return fail("A rung's codec must be libx264, libx265 or libsvtav1");
        if (!avcodec_find_encoder_by_name(r.codec.c_str())) return fail("This build has no " + r.codec + " encoder");
        if (r.width <= 0 || r.bitrate <= 0) return fail("Each rung needs a width and a bit rate");
    }
    if (rungs.empty()) return fail("The ladder is empty");
    if (s.segmentFormat != "cmaf" && s.segmentFormat != "ts") return fail("Segments are cmaf or ts");
    // MPEG-TS segments, for HLS (DASH keeps CMAF's): H.264, with AAC, AC-3 or E-AC-3 (HLS players take HEVC in CMAF only).
    const bool ts = s.segmentFormat == "ts" && s.hls;
    if (ts) {
        for (const StreamingRung& r : rungs)
            if (r.codec != "libx264")
                return fail(std::string("MPEG-TS segments carry H.264, not ") + (r.codec == "libx265" ? "HEVC" : "AV1") + ": choose CMAF segments for those rungs");
        if (s.audioCodec == "libopus") return fail("MPEG-TS segments carry AAC, AC-3 or E-AC-3 sound, not Opus");
    }
    // (with TS segments and no DASH, the CMAF fragments are only measured)
    const bool cmafFiles = !ts || s.dash;
    if (hdr && std::any_of(rungs.begin(), rungs.end(), [](const StreamingRung& r) { return r.codec == "libx264"; }))
        res.warnings.push_back("HDR in H.264 (10-bit) plays on few devices and browsers: HEVC or AV1 rungs play far more widely");

    // Ad breaks and the segments (split at each break's start and end).
    res.breaks = s.adBreaks ? streamingAdBreaks(seq, in, out) : std::vector<StreamingAdBreak>{};
    std::vector<FrameTime> splits;
    for (const StreamingAdBreak& b : res.breaks) splits.push_back(b.at), splits.push_back(b.at + b.duration);
    res.segments = streamingSegments(seq, in, out, s.segmentSeconds, s.cutAligned, splits);
    const std::vector<FrameTime>& segs = res.segments;
    auto segEnd = [&](size_t i) { return i + 1 < segs.size() ? segs[i + 1] : len; };

    QDir root(QString::fromStdString(s.folder));
    if (!root.mkpath(".")) return fail("Cannot make the folder " + s.folder);
    const std::string finalFolder = root.absolutePath().toStdString();
    // The package is made in a folder of its own inside, and only moved into place once whole: a failed or cancelled
    // export leaves an earlier package as it was.
    const QString stagingName = QStringLiteral(".montage-package-new");
    QDir(root.filePath(stagingName)).removeRecursively();
    if (!root.mkpath(stagingName)) return fail("Cannot make " + finalFolder + "/" + stagingName.toStdString());
    const std::string folder = root.filePath(stagingName).toStdString();
    struct Staging {
        QString dir;
        ~Staging() {
            if (!dir.isEmpty()) QDir(dir).removeRecursively();
        }
    } staging{QString::fromStdString(folder)};
    const std::string work = finalFolder + "/work";
    if (!QDir().mkpath(QString::fromStdString(work))) return fail("Cannot make " + work);
    struct WorkFiles {
        std::vector<std::string> files;
        std::string dir;
        bool keep = false;
        ~WorkFiles() {
            if (keep) return;
            for (const std::string& f : files) QFile::remove(QString::fromStdString(f));
            QDir().rmdir(QString::fromStdString(dir));
        }
    } workFiles{{}, work, s.keepWork};
    // A rendition's folder (new, in the staging folder).
    auto renditionDir = [&](const std::string& name) -> std::string {
        QDir d(QString::fromStdString(folder + "/" + name));
        if (!d.mkpath(".")) return {};
        return d.absolutePath().toStdString();
    };

    // 1. The sequence rendered once, near transparently, in the delivery space (10-bit HEVC when HDR).
    ExportSettings m;
    m.path = work + "/render.mkv";
    m.videoCodec = hdr ? "libx265" : "libx264";
    m.crf = 6;
    m.preset = s.preset.empty() ? "fast" : s.preset;
    m.gop = int(std::max<FrameTime>(1, std::llround(fps.toDouble())));
    m.audioCodec = "none";
    m.in = in, m.out = out;
    m.colorSpace = s.colorSpace, m.toneMapping = s.toneMapping;
    m.chapters = false, m.cea708 = false, m.embedCaptions = false, m.burnInCaptions = false;
    m.useProxies = s.useProxies;
    workFiles.files.push_back(m.path);
    const bool sound = !s.audioCodec.empty() && s.audioCodec != "none";
    const double renderShare = sound ? 0.45 : 0.5, soundShare = sound ? 0.05 : 0.0;
    {
        std::string why;
        if (!exportSequence(p, seq, m, [&](double f, FrameTime) { report(f * renderShare); }, cancel, &why)) return fail(why.empty() ? "Cancelled" : why);
    }
    if (cancelled()) return fail("Cancelled");

    // 2. The sound: the mix and each other stream, in one file, one stream each.
    std::vector<AudioOut> audio;
    Opened soundFile;
    if (sound) {
        ExportSettings a;
        a.path = work + "/sound.mp4";
        a.videoCodec = "none";
        a.audioCodec = s.audioCodec;
        a.audioBitrate = s.audioBitrate;
        a.sampleRate = 48000;
        a.in = in, a.out = out;
        a.chapters = false, a.cea708 = false, a.embedCaptions = false;
        a.audioName = s.audioName.empty() ? "Main" : s.audioName;
        a.audioLanguage = s.audioLanguage;
        if (a.audioLanguage.empty()) a.audioLanguage = seq.captionTracks.empty() ? "en" : seq.captionTracks.front().language;
        a.extraAudio = s.extraAudio;
        workFiles.files.push_back(a.path);
        std::string why;
        if (!exportSequence(p, seq, a, [&](double f, FrameTime) { report(renderShare + f * soundShare); }, cancel, &why))
            return fail(why.empty() ? "Cancelled" : why);
        if (avformat_open_input(&soundFile.fmt, a.path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(soundFile.fmt, nullptr) < 0)
            return fail("Cannot read the sound back");
        int k = 0;
        for (unsigned i = 0; i < soundFile.fmt->nb_streams; ++i) {
            const AVStream* st = soundFile.fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            AudioOut ao;
            ao.stream = int(i);
            ao.tb = st->time_base;
            ao.main = k == 0;
            const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
            const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
            // (the language as given: ISO 639-1, as HLS and DASH want it)
            ao.language = k == 0 ? a.audioLanguage : size_t(k - 1) < s.extraAudio.size() ? s.extraAudio[size_t(k - 1)].language : "";
            if (ao.language.empty() && lang) ao.language = lang->value;
            if (ao.language.empty()) ao.language = "und";
            const std::string given = k == 0 ? a.audioName : size_t(k - 1) < s.extraAudio.size() ? s.extraAudio[size_t(k - 1)].name : "";
            ao.label = !given.empty() ? given : title ? title->value : "Audio " + std::to_string(k + 1);
            for (int n = 2; std::any_of(audio.begin(), audio.end(), [&](const AudioOut& o) { return o.label == ao.label; }); ++n)
                ao.label = (!given.empty() ? given : title ? std::string(title->value) : "Audio " + std::to_string(k + 1)) + " " + std::to_string(n);
            ao.described = (st->disposition & AV_DISPOSITION_DESCRIPTIONS) != 0;
            ao.channels = st->codecpar->ch_layout.nb_channels;
            ao.sampleRate = st->codecpar->sample_rate;
            ao.name = "a" + std::to_string(k + 1) + "_" + pathName(ao.language);
            audio.push_back(std::move(ao));
            ++k;
        }
        if (audio.empty()) return fail("The sound has no streams");
    }

    // 3. Each rung encoded from the render, in one pass over it, every segment starting on a forced keyframe.
    Opened render;
    if (avformat_open_input(&render.fmt, m.path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(render.fmt, nullptr) < 0)
        return fail("Cannot read the render back");
    const int vs = av_find_best_stream(render.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) return fail("The render has no picture");
    const AVStream* vst = render.fmt->streams[vs];
    const AVCodec* decoder = avcodec_find_decoder(vst->codecpar->codec_id);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> dec(avcodec_alloc_context3(decoder), [](AVCodecContext* c) { avcodec_free_context(&c); });
    if (!decoder || !dec || avcodec_parameters_to_context(dec.get(), vst->codecpar) < 0 || avcodec_open2(dec.get(), decoder, nullptr) < 0)
        return fail("Cannot decode the render");
    const AVCodecParameters* rp = vst->codecpar;
    const AVPacketSideData* mastering = nullptr;
    const AVPacketSideData* lightLevel = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    mastering = av_packet_side_data_get(rp->coded_side_data, rp->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    lightLevel = av_packet_side_data_get(rp->coded_side_data, rp->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
#endif
    const bool pq = rp->color_trc == AVCOL_TRC_SMPTE2084, hlg = rp->color_trc == AVCOL_TRC_ARIB_STD_B67;
    const std::string range = pq ? "PQ" : hlg ? "HLG" : "SDR";
    FrameTime longest = 1;
    for (size_t i = 0; i < segs.size(); ++i) longest = std::max(longest, segEnd(i) - segs[i]);

    // A rung's encoder. SVT-AV1 will not start a keyframe on request in bit-rate mode, so an AV1 rung is given a new
    // encoder for each segment instead (the same settings, so the same sequence header).
    auto openEncoder = [&](VideoOut& v, std::string* why) -> bool {
        const StreamingRung& r = v.rung;
        const AVCodec* codec = avcodec_find_encoder_by_name(r.codec.c_str());
        avcodec_free_context(&v.enc);
        v.enc = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!v.enc) return *why = "Montage was built without the " + r.codec + " encoder", false;
        AVCodecContext* e = v.enc;
        e->width = v.width, e->height = v.height;
        e->time_base = {fps.den, fps.num};
        e->framerate = {fps.num, fps.den};
        e->sample_aspect_ratio = {1, 1};
        const bool tenBit = hdr || av_pix_fmt_desc_get(AVPixelFormat(rp->format))->comp[0].depth > 8;
        e->pix_fmt = r.codec == "libx264" && !hdr ? AV_PIX_FMT_YUV420P : tenBit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        e->color_primaries = rp->color_primaries, e->color_trc = rp->color_trc, e->colorspace = rp->color_space;
        e->color_range = rp->color_range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : rp->color_range;
        e->chroma_sample_location = AVCHROMA_LOC_LEFT;
        e->bit_rate = r.bitrate;
        // (SVT-AV1 4 refuses a peak rate outside its constant-quality mode, so an AV1 rung's peak is left to its VBR)
        if (r.codec != "libsvtav1") e->rc_max_rate = r.bitrate * 3 / 2;
        e->rc_buffer_size = int(std::min<int64_t>(INT32_MAX, r.bitrate * 2));
        e->gop_size = int(longest * 2 + 1);  // (keyframes only where forced)
        e->keyint_min = 1;
        e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        const std::string preset = s.preset.empty() ? "fast" : s.preset;
        if (r.codec == "libsvtav1") {
            // (SVT-AV1's presets are numbers, 0 the slowest)
            static const std::map<std::string, int> svt = {{"ultrafast", 12}, {"superfast", 11}, {"veryfast", 10}, {"faster", 9}, {"fast", 8},
                                                           {"medium", 6}, {"slow", 5}, {"slower", 4}, {"veryslow", 3}, {"placebo", 2}};
            const auto it = svt.find(preset);
            av_dict_set(&opts, "preset", it != svt.end() ? std::to_string(it->second).c_str() : preset.c_str(), 0);
        } else {
            av_dict_set(&opts, "preset", preset.c_str(), 0);
        }
        if (r.codec == "libx264") {
            av_dict_set(&opts, "forced-idr", "1", 0);
            av_dict_set(&opts, "x264-params", "scenecut=0:open-gop=0", 0);
        } else if (r.codec == "libx265") {
            av_dict_set(&opts, "forced-idr", "1", 0);
            std::string params = "log-level=error:scenecut=0:open-gop=0";
            if (pq) {
                params += ":hdr10=1";  // (parameter sets in the sample entry only, as hvc1 says)
                if (mastering && mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
                    const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(mastering->data);
                    auto q = [](AVRational v, double k) { return std::llround(av_q2d(v) * k); };
                    char buf[200];
                    std::snprintf(buf, sizeof buf, ":master-display=G(%lld,%lld)B(%lld,%lld)R(%lld,%lld)WP(%lld,%lld)L(%lld,%lld)",
                                  q(md->display_primaries[1][0], 50000), q(md->display_primaries[1][1], 50000), q(md->display_primaries[2][0], 50000),
                                  q(md->display_primaries[2][1], 50000), q(md->display_primaries[0][0], 50000), q(md->display_primaries[0][1], 50000),
                                  q(md->white_point[0], 50000), q(md->white_point[1], 50000), q(md->max_luminance, 10000), q(md->min_luminance, 10000));
                    params += buf;
                }
                if (lightLevel && lightLevel->size >= sizeof(AVContentLightMetadata)) {
                    const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(lightLevel->data);
                    params += ":max-cll=" + std::to_string(cl->MaxCLL) + "," + std::to_string(cl->MaxFALL);
                }
            }
            av_dict_set(&opts, "x265-params", params.c_str(), 0);
        } else {
            av_dict_set(&opts, "svtav1-params", "scd=0", 0);
            e->flags |= AV_CODEC_FLAG_CLOSED_GOP;
        }
        const int rc = avcodec_open2(e, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) return *why = "Cannot open the " + r.codec + " encoder: " + averr(rc), false;
        return true;
    };

    std::vector<std::unique_ptr<VideoOut>> video;
    std::set<std::string> names;
    for (const StreamingRung& r : rungs) {
        auto v = std::make_unique<VideoOut>();
        v->rung = r;
        v->width = std::min(r.width, rp->width) & ~1;
        v->height = r.height > 0 ? std::min(r.height, rp->height) & ~1 : int(std::lround(double(v->width) * rp->height / rp->width / 2)) * 2;
        if (v->width != r.width || (r.height > 0 && v->height != r.height))
            res.warnings.push_back("A " + std::to_string(r.width) + " wide rung is larger than the picture: made at its size");
        std::string name = "v" + std::to_string(video.size() + 1) + "_" + std::to_string(v->height) + "p_" + codecFamily(r.codec);
        v->name = name;
        names.insert(name);
        std::string opened;
        if (!openEncoder(*v, &opened)) return fail(opened);
        AVCodecContext* e = v->enc;
        v->frame = av_frame_alloc();
        v->pkt = av_packet_alloc();
        v->frame->format = e->pix_fmt, v->frame->width = e->width, v->frame->height = e->height;
        if (av_frame_get_buffer(v->frame, 0) < 0) return fail("Out of memory");
        v->sws = sws_alloc_context();
        av_opt_set_int(v->sws, "srcw", rp->width, 0);
        av_opt_set_int(v->sws, "srch", rp->height, 0);
        av_opt_set_int(v->sws, "src_format", rp->format, 0);
        av_opt_set_int(v->sws, "dstw", e->width, 0);
        av_opt_set_int(v->sws, "dsth", e->height, 0);
        av_opt_set_int(v->sws, "dst_format", e->pix_fmt, 0);
        av_opt_set_int(v->sws, "sws_flags", SWS_BICUBIC | SWS_ACCURATE_RND, 0);
        // (chroma sited as the render has it and as the rung is tagged: left, centred between its two lines)
        const bool srcLeft = rp->chroma_location != AVCHROMA_LOC_CENTER;
        av_opt_set_int(v->sws, "src_h_chr_pos", srcLeft ? 0 : 128, 0);
        av_opt_set_int(v->sws, "src_v_chr_pos", 128, 0);
        av_opt_set_int(v->sws, "dst_h_chr_pos", 0, 0);
        av_opt_set_int(v->sws, "dst_v_chr_pos", 128, 0);
        if (sws_init_context(v->sws, nullptr, nullptr) < 0) return fail("Cannot scale the render");
        const std::string dir = renditionDir(v->name);
        if (dir.empty()) return fail("Cannot make " + folder + "/" + v->name);
        std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> par(avcodec_parameters_alloc(), [](AVCodecParameters* q) { avcodec_parameters_free(&q); });
        avcodec_parameters_from_context(par.get(), e);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
        for (const AVPacketSideData* sd : {mastering, lightLevel})
            if (sd)
                if (AVPacketSideData* copy = av_packet_side_data_new(&par->coded_side_data, &par->nb_coded_side_data, sd->type, sd->size, 0))
                    std::memcpy(copy->data, sd->data, sd->size);
#endif
        v->track = std::make_unique<CmafTrack>();
        std::string why;
        if (!v->track->open(dir, par.get(), e->time_base, &why, cmafFiles)) return fail(why);
        if (ts) {
            v->ts = std::make_unique<TsTrack>();
            v->ts->dir = dir;
            if (!v->ts->mux.open(par.get(), &why)) return fail(why);
        }
        v->codecs = codecNameOf(par.get(), *v->track);
        v->range = range;
        video.push_back(std::move(v));
    }

    auto take = [&](VideoOut& v) -> bool {
        while (true) {
            const int rc = avcodec_receive_packet(v.enc, v.pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return fail("Encoding failed: " + averr(rc));
            if (v.pkt->duration <= 0) v.pkt->duration = 1;
            if (v.next < segs.size() && v.pkt->pts >= segs[v.next]) {
                if (v.pkt->pts != segs[v.next] || !(v.pkt->flags & AV_PKT_FLAG_KEY))
                    return fail("The " + v.rung.codec + " encoder did not start segment " + std::to_string(v.next + 1) + " on a keyframe");
                std::string why;
                if (!v.track->cut(&why) || (v.ts && !v.ts->cut(&why))) return fail(why);
                ++v.next;
            } else if (!v.track->open() && !(v.pkt->flags & AV_PKT_FLAG_KEY)) {
                return fail("The " + v.rung.codec + " encoder did not start on a keyframe");
            }
            std::string why;
            if (v.ts && !v.ts->write(v.pkt, v.enc->time_base, &why)) return fail(why);
            if (!v.track->write(v.pkt, v.enc->time_base, &why)) return fail(why);
            av_packet_unref(v.pkt);
        }
    };
    {
        std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* q) { av_packet_free(&q); });
        std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* q) { av_frame_free(&q); });
        FrameTime index = 0;
        auto encodeFrame = [&](const AVFrame* src) -> bool {
            if (index >= len) return true;
            const bool start = std::binary_search(segs.begin(), segs.end(), index);
            for (auto& v : video) {
                if (start && index > 0 && v->rung.codec == "libsvtav1") {
                    // (the segment's frames all out, then a new encoder whose first frame is a keyframe)
                    avcodec_send_frame(v->enc, nullptr);
                    if (!take(*v)) return false;
                    const std::vector<uint8_t> header(v->enc->extradata, v->enc->extradata + v->enc->extradata_size);
                    std::string why;
                    if (!openEncoder(*v, &why)) return fail(why);
                    if (header != std::vector<uint8_t>(v->enc->extradata, v->enc->extradata + v->enc->extradata_size))
                        return fail("The " + v->rung.codec + " encoder changed its sequence header between segments");
                }
                if (av_frame_make_writable(v->frame) < 0) return fail("Out of memory");
                sws_scale(v->sws, src->data, src->linesize, 0, src->height, v->frame->data, v->frame->linesize);
                v->frame->pts = index;
                v->frame->pict_type = start ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
                v->frame->color_primaries = v->enc->color_primaries, v->frame->color_trc = v->enc->color_trc;
                v->frame->colorspace = v->enc->colorspace, v->frame->color_range = v->enc->color_range;
                v->frame->chroma_location = AVCHROMA_LOC_LEFT;
                if (int rc = avcodec_send_frame(v->enc, v->frame); rc < 0) return fail("Encoding failed: " + averr(rc));
                if (!take(*v)) return false;
            }
            ++index;
            report(renderShare + soundShare + (1 - renderShare - soundShare) * 0.95 * double(index) / double(len));
            return !cancelled() || fail("Cancelled");
        };
        auto drainDecoder = [&]() -> bool {
            while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                const bool ok = encodeFrame(frame.get());
                av_frame_unref(frame.get());
                if (!ok) return false;
            }
            return true;
        };
        while (av_read_frame(render.fmt, pkt.get()) >= 0) {
            if (pkt->stream_index == vs) avcodec_send_packet(dec.get(), pkt.get());
            av_packet_unref(pkt.get());
            if (!drainDecoder()) return false;
        }
        avcodec_send_packet(dec.get(), nullptr);
        if (!drainDecoder()) return false;
        if (index != len) res.warnings.push_back("The render had " + std::to_string(index) + " frames of " + std::to_string(len));
        for (auto& v : video) {
            avcodec_send_frame(v->enc, nullptr);
            if (!take(*v)) return false;
            std::string why;
            if (!v->track->finish(&why) || (v->ts && !v->ts->cut(&why))) return fail(why);
            if (v->ts && v->ts->segments.size() != segs.size()) return fail(v->name + " came out with a different number of TS segments");
            if (v->track->segments.size() != segs.size())
                return fail(v->name + " came out with " + std::to_string(v->track->segments.size()) + " segments, not " + std::to_string(segs.size()));
        }
    }

    // 4. The sound's streams copied into fragments at the same times.
    if (sound) {
        for (AudioOut& ao : audio) {
            const std::string dir = renditionDir(ao.name);
            if (dir.empty()) return fail("Cannot make " + folder + "/" + ao.name);
            ao.track = std::make_unique<CmafTrack>();
            std::string why;
            if (!ao.track->open(dir, soundFile.fmt->streams[ao.stream]->codecpar, ao.tb, &why, cmafFiles)) return fail(why);
            if (ts) {
                ao.ts = std::make_unique<TsTrack>();
                ao.ts->dir = dir;
                if (!ao.ts->mux.open(soundFile.fmt->streams[ao.stream]->codecpar, &why)) return fail(why);
            }
            ao.codecs = codecNameOf(soundFile.fmt->streams[ao.stream]->codecpar, *ao.track);
        }
        std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* q) { av_packet_free(&q); });
        while (av_read_frame(soundFile.fmt, pkt.get()) >= 0) {
            for (AudioOut& ao : audio) {
                if (pkt->stream_index != ao.stream) continue;
                // The encoder's priming (before 0, which the sound file's edit list hides) is left out: CMAF fragments
                // carry no edit list, and their times count from the first sample written, so that one starts at 0.
                if (pkt->pts == AV_NOPTS_VALUE) pkt->pts = pkt->dts;
                if (pkt->pts == AV_NOPTS_VALUE || pkt->pts + std::max<int64_t>(pkt->duration, 0) <= 0) continue;
                if (ao.offset == AV_NOPTS_VALUE) ao.offset = pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                pkt->pts -= ao.offset;
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= ao.offset;
                const double t = double(pkt->pts) * ao.tb.num / ao.tb.den;
                while (ao.next < segs.size() && t >= double(segs[ao.next]) * fps.den / fps.num - 1e-6) {
                    std::string why;
                    if (!ao.track->cut(&why) || (ao.ts && !ao.ts->cut(&why))) return fail(why);
                    ++ao.next;
                }
                std::string why;
                if (ao.ts && !ao.ts->write(pkt.get(), ao.tb, &why)) return fail(why);
                if (!ao.track->write(pkt.get(), ao.tb, &why)) return fail(why);
            }
            av_packet_unref(pkt.get());
        }
        for (AudioOut& ao : audio) {
            std::string why;
            if (!ao.track->finish(&why) || (ao.ts && !ao.ts->cut(&why))) return fail(why);
        }
    }
    if (cancelled()) return fail("Cancelled");

    // 5. Captions (each track with captions in the range) and chapters.
    std::vector<TextOut> texts;
    if (s.captions)
        for (const CaptionTrack& t : seq.captionTracks) {
            TextOut to;
            for (Caption c : t.captions) {
                if (c.end <= in || c.start >= out) continue;
                c.start = std::max(c.start, in) - in;
                c.end = std::min(c.end, out) - in;
                to.captions.push_back(std::move(c));
            }
            if (to.captions.empty()) continue;
            to.language = t.language.empty() ? "und" : t.language;
            to.label = t.name.empty() ? to.language : t.name;
            // (NAME is unique within an HLS group: a second unnamed track of a language is numbered)
            for (int n = 2; std::any_of(texts.begin(), texts.end(), [&](const TextOut& o) { return o.label == to.label; }); ++n)
                to.label = (t.name.empty() ? to.language : t.name) + " " + std::to_string(n);
            to.style = t.style;
            to.name = "s" + std::to_string(texts.size() + 1) + "_" + pathName(to.language);
            texts.push_back(std::move(to));
        }
    auto vttWithMap = [ts](std::string vtt) {
        // (cue times are the presentation's, from 0, as the media's are, or from kTsOffset in TS segments:
        // X-TIMESTAMP-MAP says so)
        const size_t nl = vtt.find('\n');
        vtt.insert(nl == std::string::npos ? vtt.size() : nl + 1,
                   "X-TIMESTAMP-MAP=MPEGTS:" + std::to_string(ts ? kTsOffset : 0) + ",LOCAL:00:00:00.000\n");
        return vtt;
    };
    for (TextOut& t : texts) {
        const std::string dir = renditionDir(t.name);
        if (dir.empty()) return fail("Cannot make " + folder + "/" + t.name);
        if (s.hls)
            for (size_t i = 0; i < segs.size(); ++i) {
                std::vector<Caption> part;
                for (const Caption& c : t.captions)
                    if (c.start < segEnd(i) && c.end > segs[i]) part.push_back(c);
                char name[32];
                std::snprintf(name, sizeof name, "/seg%05d.vtt", int(i + 1));
                if (!writeFile(dir + name, vttWithMap(captionsToVtt(part, fps)))) return fail("Cannot write " + dir + name);
            }
        if (s.dash) {
            if (!writeFile(dir + "/all.vtt", captionsToVtt(t.captions, fps))) return fail("Cannot write " + dir + "/all.vtt");
            if (!writeFile(dir + "/all.ttml", captionsToTtml(t.captions, fps, t.language, t.style))) return fail("Cannot write " + dir + "/all.ttml");
        }
    }
    bool chapters = false;
    if (s.chapters) {
        std::vector<Caption> cues;
        for (const Chapter& c : chaptersOf(seq, in, out)) {
            Caption cue;
            cue.start = c.start, cue.end = c.end, cue.text = c.title;
            cues.push_back(cue);
        }
        if (!cues.empty()) {
            if (!writeFile(folder + "/chapters.vtt", captionsToVtt(cues, fps))) return fail("Cannot write " + folder + "/chapters.vtt");
            chapters = true;
        }
    }

    // 6. SCTE 35 cues: an out point at each break's start (with its length) and an in point at its end.
    struct CuePair {
        StreamingAdBreak b;
        std::string outHex, inHex, outBase64, inBase64;
    };
    std::vector<CuePair> cues;
    for (const StreamingAdBreak& b : res.breaks) {
        scte35::Cue o, i;
        const int64_t at = ticks(b.at, fps), dur = ticks(b.duration, fps);
        if (s.cueCommand == "time_signal") {
            o.command = i.command = scte35::Cue::TimeSignal;
            o.sapType = i.sapType = 0;  // (a closed GOP starts there)
            o.ptsTime = at, i.ptsTime = at + dur;
            scte35::Segmentation so, si;
            so.eventId = si.eventId = b.id;
            so.typeId = 0x34, si.typeId = 0x35;  // provider placement opportunity start and end
            so.segmentNum = so.segmentsExpected = si.segmentNum = si.segmentsExpected = 1;
            so.duration = b.duration > 0 ? dur : -1;
            o.segmentations = {so};
            i.segmentations = {si};
        } else {
            o.sapType = i.sapType = 0;
            o.eventId = i.eventId = b.id;
            o.outOfNetwork = true, i.outOfNetwork = false;
            o.ptsTime = at, i.ptsTime = at + dur;
            o.breakDuration = b.duration > 0 ? dur : -1;
            o.uniqueProgramId = i.uniqueProgramId = 1;
        }
        const std::vector<uint8_t> ob = scte35::encode(o), ib = scte35::encode(i);
        cues.push_back({b, scte35::toHex(ob), scte35::toHex(ib), scte35::toBase64(ob), scte35::toBase64(ib)});
    }

    // 7. HLS.
    const QDateTime start = s.startDate.empty() ? QDateTime::currentDateTimeUtc() : QDateTime::fromString(QString::fromStdString(s.startDate), Qt::ISODateWithMs);
    if (!start.isValid()) return fail("The start date is not an ISO 8601 date and time");
    auto seconds = [&](FrameTime f) { return double(f) * fps.den / fps.num; };
    auto dateAt = [&](FrameTime f) { return isoDate(start.addMSecs(qint64(std::llround(seconds(f) * 1000)))); };
    auto mediaPlaylist = [&](const CmafTrack* track, bool frames, bool iframes, const std::string& ext, const TsTrack* tsTrack = nullptr) {
        // Segment lengths: the frames' (video and captions), or the samples' (sound).
        std::vector<double> lengths;
        const size_t count = frames || !track ? segs.size() : tsTrack ? tsTrack->segments.size() : track->segments.size();
        for (size_t i = 0; i < count; ++i) {
            if (frames || !track) {
                lengths.push_back(seconds(segEnd(i) - segs[i]));
            } else if (tsTrack) {
                lengths.push_back(tsTrack->segments[i].seconds);
            } else {
                const auto& g = track->segments;
                lengths.push_back(track->seconds((i + 1 < g.size() ? g[i + 1].start : g[i].end) - g[i].start));
            }
        }
        double longestSeg = 0;
        for (double l : lengths) longestSeg = std::max(longestSeg, l);
        std::string m3u = "#EXTM3U\n#EXT-X-VERSION:" + std::string(tsTrack && !iframes ? "3" : iframes && tsTrack ? "4" : "7") +
                          "\n#EXT-X-TARGETDURATION:" + std::to_string(std::max<long long>(1, std::llround(longestSeg))) +
                          "\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-INDEPENDENT-SEGMENTS\n";
        if (iframes) m3u += "#EXT-X-I-FRAMES-ONLY\n";
        if (track && !tsTrack) m3u += "#EXT-X-MAP:URI=\"init.mp4\"\n";
        m3u += "#EXT-X-PROGRAM-DATE-TIME:" + dateAt(0) + "\n";
        for (size_t i = 0; i < count; ++i) {
            if (!iframes)
                for (const CuePair& c : cues) {
                    // (each tag before the segment its time falls in; START-DATE carries the exact time)
                    auto here = [&](FrameTime t) { return t >= segs[i] && (t < segEnd(i) || (i + 1 == count && t == len)); };
                    const std::string id = "ID=\"break-" + std::to_string(c.b.id) + "\",START-DATE=\"" + dateAt(c.b.at) + "\"";
                    if (here(c.b.at) && c.b.duration > 0)
                        m3u += "#EXT-X-DATERANGE:" + id + ",PLANNED-DURATION=" + fmt("%.3f", seconds(c.b.duration)) + ",SCTE35-OUT=" + c.outHex + "\n";
                    if (here(c.b.at + c.b.duration) && c.b.duration > 0)
                        m3u += "#EXT-X-DATERANGE:" + id + ",DURATION=" + fmt("%.3f", seconds(c.b.duration)) + ",SCTE35-IN=" + c.inHex + "\n";
                    if (here(c.b.at) && c.b.duration == 0)
                        m3u += "#EXT-X-DATERANGE:" + id + ",DURATION=0.000,SCTE35-OUT=" + c.outHex + ",SCTE35-IN=" + c.inHex + "\n";
                }
            m3u += "#EXTINF:" + fmt("%.5f", lengths[i]) + ",\n";
            char name[32];
            std::snprintf(name, sizeof name, "seg%05d.%s", int(i + 1), ext.c_str());
            if (iframes) m3u += "#EXT-X-BYTERANGE:" + std::to_string(tsTrack ? tsTrack->segments[i].iframeBytes : track->segments[i].iframeBytes) + "@0\n";
            m3u += std::string(name) + "\n";
        }
        return m3u + "#EXT-X-ENDLIST\n";
    };
    // (HLS's bit rates are its segments': TS ones when it has them)
    auto hlsPeak = [&](const CmafTrack& c, const TsTrack* t) { return t ? peakRate(t->sizes()) : c.peakBitrate(); };
    auto hlsAverage = [&](const CmafTrack& c, const TsTrack* t) { return t ? averageRate(t->sizes()) : c.averageBitrate(); };
    int64_t audioPeak = 0, audioAverage = 0;
    for (const AudioOut& ao : audio)
        audioPeak = std::max(audioPeak, hlsPeak(*ao.track, ao.ts.get())), audioAverage = std::max(audioAverage, hlsAverage(*ao.track, ao.ts.get()));
    const std::string ext = ts ? "ts" : "m4s";
    if (s.hls) {
        for (auto& v : video) {
            if (!writeFile(folder + "/" + v->name + "/playlist.m3u8", mediaPlaylist(v->track.get(), true, false, ext, v->ts.get()))) return fail("Cannot write a playlist");
            if (s.iframePlaylists && !writeFile(folder + "/" + v->name + "/iframes.m3u8", mediaPlaylist(v->track.get(), true, true, ext, v->ts.get())))
                return fail("Cannot write an I-frame playlist");
        }
        for (const AudioOut& ao : audio)
            if (!writeFile(folder + "/" + ao.name + "/playlist.m3u8", mediaPlaylist(ao.track.get(), false, false, ext, ao.ts.get()))) return fail("Cannot write a playlist");
        for (const TextOut& t : texts)
            if (!writeFile(folder + "/" + t.name + "/playlist.m3u8", mediaPlaylist(nullptr, true, false, "vtt"))) return fail("Cannot write a playlist");
        std::string m3u = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n";
        if (chapters) m3u += "#EXT-X-SESSION-DATA:DATA-ID=\"org.montage.chapters\",URI=\"chapters.vtt\",FORMAT=RAW\n";
        for (const AudioOut& ao : audio) {
            m3u += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=" + quoted(ao.label) + ",LANGUAGE=" + quoted(ao.language) +
                   ",DEFAULT=" + (ao.main ? "YES" : "NO") + ",AUTOSELECT=" + (ao.described ? "NO" : "YES") + ",CHANNELS=\"" + std::to_string(ao.channels) + "\"";
            if (ao.described) m3u += ",CHARACTERISTICS=\"public.accessibility.describes-video\"";
            m3u += ",URI=\"" + ao.name + "/playlist.m3u8\"\n";
        }
        for (const TextOut& t : texts)
            m3u += "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=" + quoted(t.label) + ",LANGUAGE=" + quoted(t.language) +
                   ",DEFAULT=NO,AUTOSELECT=YES,URI=\"" + t.name + "/playlist.m3u8\"\n";
        const std::string audioCodecs = audio.empty() ? "" : "," + audio.front().codecs;
        for (auto& v : video) {
            m3u += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(hlsPeak(*v->track, v->ts.get()) + audioPeak) +
                   ",AVERAGE-BANDWIDTH=" + std::to_string(hlsAverage(*v->track, v->ts.get()) + audioAverage) + ",CODECS=\"" + v->codecs + audioCodecs +
                   "\",RESOLUTION=" + std::to_string(v->width) + "x" + std::to_string(v->height) + ",FRAME-RATE=" + fmt("%.3f", fps.toDouble()) +
                   ",VIDEO-RANGE=" + v->range + (audio.empty() ? "" : ",AUDIO=\"audio\"") + (texts.empty() ? "" : ",SUBTITLES=\"subs\"") +
                   ",CLOSED-CAPTIONS=NONE\n" + v->name + "/playlist.m3u8\n";
        }
        if (s.iframePlaylists)
            for (auto& v : video) {
                int64_t peak = 0;
                for (size_t i = 0; i < segs.size(); ++i)
                    peak = std::max<int64_t>(peak, int64_t(std::ceil((v->ts ? v->ts->segments[i].iframeBytes : v->track->segments[i].iframeBytes) * 8 /
                                                                     seconds(segEnd(i) - segs[i]))));
                m3u += "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=" + std::to_string(peak) + ",CODECS=\"" + v->codecs + "\",RESOLUTION=" +
                       std::to_string(v->width) + "x" + std::to_string(v->height) + ",VIDEO-RANGE=" + v->range + ",URI=\"" + v->name + "/iframes.m3u8\"\n";
            }
        res.hls = folder + "/master.m3u8";
        if (!writeFile(res.hls, m3u)) return fail("Cannot write " + res.hls);
    }

    // 8. DASH.
    if (s.dash) {
        auto timeline = [](const CmafTrack& t, const std::string& name) {
            std::string x = "<SegmentTemplate timescale=\"" + std::to_string(t.timescale.den / std::max(1, t.timescale.num)) +
                            "\" initialization=\"" + name + "/init.mp4\" media=\"" + name + "/seg$Number%05d$.m4s\" startNumber=\"1\">\n<SegmentTimeline>\n";
            for (size_t i = 0; i < t.segments.size();) {
                const int64_t at = t.segments[i].start;
                const int64_t d = (i + 1 < t.segments.size() ? t.segments[i + 1].start : t.segments[i].end) - at;
                size_t r = 0;
                while (i + r + 1 < t.segments.size()) {
                    const size_t j = i + r + 1;
                    const int64_t dj = (j + 1 < t.segments.size() ? t.segments[j + 1].start : t.segments[j].end) - t.segments[j].start;
                    if (dj != d || t.segments[j].start != at + int64_t(r + 1) * d) break;
                    ++r;
                }
                x += "<S t=\"" + std::to_string(at) + "\" d=\"" + std::to_string(d) + "\"" + (r ? " r=\"" + std::to_string(r) + "\"" : "") + "/>\n";
                i += r + 1;
            }
            return x + "</SegmentTimeline>\n</SegmentTemplate>\n";
        };
        double longestSeg = 0;
        for (size_t i = 0; i < segs.size(); ++i) longestSeg = std::max(longestSeg, seconds(segEnd(i) - segs[i]));
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" "
                        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" type=\"static\" mediaPresentationDuration=\"PT" +
                        fmt("%.3f", seconds(len)) + "S\" minBufferTime=\"PT" + fmt("%.3f", longestSeg) + "S\">\n<Period id=\"0\" start=\"PT0S\">\n";
        if (!cues.empty()) {
            x += "<EventStream schemeIdUri=\"urn:scte:scte35:2014:xml+bin\" timescale=\"90000\">\n";
            int id = 1;
            for (const CuePair& c : cues) {
                const int64_t at = ticks(c.b.at, fps), dur = ticks(c.b.duration, fps);
                x += "<Event presentationTime=\"" + std::to_string(at) + "\" duration=\"" + std::to_string(dur) + "\" id=\"" + std::to_string(id++) +
                     "\"><Signal xmlns=\"http://www.scte.org/schemas/35/2016\"><Binary>" + c.outBase64 + "</Binary></Signal></Event>\n";
                x += "<Event presentationTime=\"" + std::to_string(at + dur) + "\" duration=\"0\" id=\"" + std::to_string(id++) +
                     "\"><Signal xmlns=\"http://www.scte.org/schemas/35/2016\"><Binary>" + c.inBase64 + "</Binary></Signal></Event>\n";
            }
            x += "</EventStream>\n";
        }
        int set = 1;
        std::map<std::string, std::vector<VideoOut*>> families;
        std::vector<std::string> familyOrder;
        for (auto& v : video) {
            const std::string f = codecFamily(v->rung.codec);
            if (!families.count(f)) familyOrder.push_back(f);
            families[f].push_back(v.get());
        }
        for (const std::string& f : familyOrder) {
            x += "<AdaptationSet id=\"" + std::to_string(set++) + "\" contentType=\"video\" mimeType=\"video/mp4\" segmentAlignment=\"true\" "
                 "startWithSAP=\"1\" frameRate=\"" + std::to_string(fps.num) + "/" + std::to_string(fps.den) + "\">\n";
            if (pq || hlg) {
                x += "<SupplementalProperty schemeIdUri=\"urn:mpeg:mpegB:cicp:ColourPrimaries\" value=\"" + std::to_string(int(rp->color_primaries)) + "\"/>\n";
                x += std::string(pq ? "<EssentialProperty" : "<SupplementalProperty") + " schemeIdUri=\"urn:mpeg:mpegB:cicp:TransferCharacteristics\" value=\"" +
                     std::to_string(int(rp->color_trc)) + "\"/>\n";
                x += "<SupplementalProperty schemeIdUri=\"urn:mpeg:mpegB:cicp:MatrixCoefficients\" value=\"" + std::to_string(int(rp->color_space)) + "\"/>\n";
            }
            for (VideoOut* v : families[f])
                x += "<Representation id=\"" + v->name + "\" bandwidth=\"" + std::to_string(v->track->peakBitrate()) + "\" codecs=\"" + v->codecs +
                     "\" width=\"" + std::to_string(v->width) + "\" height=\"" + std::to_string(v->height) + "\" sar=\"1:1\">\n" + timeline(*v->track, v->name) +
                     "</Representation>\n";
            x += "</AdaptationSet>\n";
        }
        for (const AudioOut& ao : audio) {
            x += "<AdaptationSet id=\"" + std::to_string(set++) + "\" contentType=\"audio\" mimeType=\"audio/mp4\" lang=\"" + xmlEscape(ao.language) +
                 "\" segmentAlignment=\"true\">\n<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"" +
                 (ao.described ? "description" : ao.main ? "main" : "alternate") + "\"/>\n<Label>" + xmlEscape(ao.label) + "</Label>\n";
            x += "<Representation id=\"" + ao.name + "\" bandwidth=\"" + std::to_string(ao.track->peakBitrate()) + "\" codecs=\"" + ao.codecs +
                 "\" audioSamplingRate=\"" + std::to_string(ao.sampleRate) + "\">\n" +
                 // (AC-3 and E-AC-3 in the scheme ETSI TS 102 366 defines, as DASH-IF and DVB-DASH ask; the rest in MPEG's CICP)
                 (ao.codecs == "ac-3" || ao.codecs == "ec-3"
                      ? ac3DashChannelConfiguration(ao.channels)
                      : "<AudioChannelConfiguration schemeIdUri=\"urn:mpeg:mpegB:cicp:ChannelConfiguration\" value=\"" +
                            std::to_string(ao.channels == 8 ? 12 : ao.channels) + "\"/>") +
                 "\n" + timeline(*ao.track, ao.name) + "</Representation>\n</AdaptationSet>\n";
        }
        for (const TextOut& t : texts) {
            for (const char* kind : {"vtt", "ttml"}) {
                const bool vtt = std::strcmp(kind, "vtt") == 0;
                x += "<AdaptationSet id=\"" + std::to_string(set++) + "\" contentType=\"text\" mimeType=\"" + (vtt ? "text/vtt" : "application/ttml+xml") +
                     "\" lang=\"" + xmlEscape(t.language) + "\">\n<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"subtitle\"/>\n<Label>" + xmlEscape(t.label) +
                     "</Label>\n<Representation id=\"" + t.name + "_" + kind + "\" bandwidth=\"256\"" + (vtt ? "" : " codecs=\"im1t\"") + ">\n<BaseURL>" + t.name +
                     "/all." + kind + "</BaseURL>\n</Representation>\n</AdaptationSet>\n";
            }
        }
        x += "</Period>\n</MPD>\n";
        res.dash = folder + "/manifest.mpd";
        if (!writeFile(res.dash, x)) return fail("Cannot write " + res.dash);
    }
    for (auto& v : video) res.renditions.push_back(v->name);
    for (const AudioOut& ao : audio) res.renditions.push_back(ao.name);
    for (const TextOut& t : texts) res.renditions.push_back(t.name);
    // Into place. Only folders an earlier package made are replaced or removed (those its record lists, or, for a
    // package from before records, those holding nothing but a rendition's files); any other folder of a rendition's
    // name stops the export with the earlier package untouched.
    std::set<std::string> before;
    {
        QFile rec(root.filePath(QString::fromLatin1(kPackageRecord)));
        if (rec.open(QIODevice::ReadOnly))
            for (const QByteArray& line : rec.readAll().split('\n'))
                if (const QString n = QString::fromUtf8(line).trimmed(); isRenditionName(n)) before.insert(n.toStdString());
    }
    auto earlier = [&](const QString& name) {
        return QFileInfo(root.filePath(name)).isDir() && (before.count(name.toStdString()) || holdsOnlyRenditionFiles(root.filePath(name)));
    };
    for (const std::string& n : res.renditions) {
        const QString q = QString::fromStdString(n);
        if (QFileInfo::exists(root.filePath(q)) && !earlier(q))
            return fail("The folder already has a \"" + n + "\" that this package did not make: choose an empty folder");
    }
    // Into place: what the earlier package had there moved aside first, then the new package moved in; if any move
    // fails (a player holding a segment open, say), every move is undone, so the earlier package is left whole. What
    // an earlier export could not put back is kept where its message said (an aside folder still holding anything is
    // never reused or removed here): this one moves aside into a folder of its own.
    QString asideName = QStringLiteral(".montage-package-old");
    for (int n = 2; QFileInfo::exists(root.filePath(asideName)) && !root.rmdir(asideName); ++n)
        asideName = QStringLiteral(".montage-package-old-%1").arg(n);
    if (!root.mkpath(asideName)) return fail("Cannot make " + finalFolder + "/" + asideName.toStdString());
    struct Aside {
        QString dir;
        ~Aside() {
            if (!dir.isEmpty()) QDir(dir).removeRecursively();
        }
    } aside{root.filePath(asideName)};
    std::vector<std::pair<QString, QString>> moved;  // (from, to), relative to the folder
    auto move = [&](const QString& from, const QString& to) {
        if (!root.rename(from, to)) return false;
        moved.emplace_back(from, to);
        return true;
    };
    auto undo = [&](const std::string& why) {
        bool back = true;
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) back = root.rename(it->second, it->first) && back;
        if (back) return fail(why);
        // (the earlier package's files that could not be put back are kept where they were moved to, not deleted with
        // the folder made for the move; only the new package's go back to the staging folder, which goes)
        aside.dir.clear();
        return fail(why + "; the earlier package's files that could not be put back are in " + finalFolder + "/" + asideName.toStdString());
    };
    std::vector<QString> moving;
    for (const char* f : {"master.m3u8", "manifest.mpd", "chapters.vtt"}) moving.push_back(QString::fromLatin1(f));
    for (const std::string& n : res.renditions) moving.push_back(QString::fromStdString(n));
    for (const QString& q : moving)
        if (QFileInfo::exists(root.filePath(q)) && !move(q, asideName + "/" + q))
            return undo("Cannot move the earlier " + q.toStdString() + " aside in " + finalFolder + " (is a player using it?)");
    for (const QString& q : moving)
        if (QFileInfo::exists(root.filePath(stagingName + "/" + q)) && !move(stagingName + "/" + q, q))
            return undo("Cannot move " + q.toStdString() + " into " + finalFolder);
    // An earlier package's renditions this one did not make (a longer ladder, another language) are removed.
    for (const QString& d : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (isRenditionName(d) && std::find(res.renditions.begin(), res.renditions.end(), d.toStdString()) == res.renditions.end() && earlier(d))
            QDir(root.filePath(d)).removeRecursively();
    {
        std::string rec;
        for (const std::string& n : res.renditions) rec += n + "\n";
        writeFile(finalFolder + "/" + kPackageRecord, rec);
    }
    if (!res.hls.empty()) res.hls = finalFolder + "/master.m3u8";
    if (!res.dash.empty()) res.dash = finalFolder + "/manifest.mpd";
    report(1);
    if (result) *result = std::move(res);
    return true;
}

}  // namespace montage
