// Montage — smart rendering of long-GOP exports (see LongGopCopy.h).
#include "LongGopCopy.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <map>
#include <set>

#include "core/Interpretation.h"
#include "media/Decoder.h"
#include "render/ColorSpace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
}

namespace montage {

namespace {

constexpr int kH264 = AV_CODEC_ID_H264, kHevc = AV_CODEC_ID_HEVC, kMpeg2 = AV_CODEC_ID_MPEG2VIDEO;

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string averr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

// ---- NAL units ----

// The NAL units of a packet whose units each follow their length (`size` bytes, big-endian); false if they overrun it.
template <class F>
bool forEachNal(const uint8_t* d, size_t n, int size, F&& f) {
    size_t i = 0;
    while (i + size_t(size) <= n) {
        size_t len = 0;
        for (int k = 0; k < size; ++k) len = len << 8 | d[i + size_t(k)];
        i += size_t(size);
        if (len == 0 || i + len > n) return false;
        f(d + i, len);
        i += len;
    }
    return i == n;
}

// The NAL units of Annex B bytes (between start codes, trailing zeros dropped).
std::vector<std::pair<const uint8_t*, size_t>> annexBNals(const uint8_t* d, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;
                if (end > start) out.push_back({d + start, end - start});
            }
            i += 3;
            start = i;
            continue;
        }
        ++i;
    }
    if (start != SIZE_MAX && start < n) {
        size_t end = n;
        while (end > start && d[end - 1] == 0) --end;
        if (end > start) out.push_back({d + start, end - start});
    }
    return out;
}

void putNal(std::vector<uint8_t>& out, const uint8_t* d, size_t n, int size) {
    for (int k = size - 1; k >= 0; --k) out.push_back(uint8_t(n >> (8 * k)));
    out.insert(out.end(), d, d + n);
}

// Annex B bytes as length-prefixed units; false when a unit is too long for `size` bytes.
bool toLengthPrefixed(const uint8_t* d, size_t n, int size, std::vector<uint8_t>& out) {
    for (const auto& [p, len] : annexBNals(d, n)) {
        if (size < 4 && len >= (size_t(1) << (8 * size))) return false;
        putNal(out, p, len, size);
    }
    return true;
}

// Exp-Golomb reading over a NAL unit's payload (emulation prevention bytes taken out).
struct Bits {
    std::vector<uint8_t> b;
    size_t pos = 0;
    Bits(const uint8_t* d, size_t n) {
        int zeros = 0;
        for (size_t i = 0; i < n && b.size() < 64; ++i) {
            if (zeros >= 2 && d[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = d[i] == 0 ? zeros + 1 : 0;
            b.push_back(d[i]);
        }
    }
    int bit() {
        if (pos >= b.size() * 8) return -1;
        const int v = (b[pos / 8] >> (7 - pos % 8)) & 1;
        ++pos;
        return v;
    }
    int64_t ue() {
        int z = 0;
        for (;;) {
            const int v = bit();
            if (v < 0 || z > 31) return -1;
            if (v) break;
            ++z;
        }
        int64_t v = 0;
        for (int k = 0; k < z; ++k) {
            const int x = bit();
            if (x < 0) return -1;
            v = v << 1 | x;
        }
        return (int64_t(1) << z) - 1 + v;
    }
};

int h264SpsId(const uint8_t* nal, size_t n) {
    if (n < 5) return -1;
    Bits b(nal + 4, n - 4);  // (after the header byte, profile, constraints and level)
    return int(b.ue());
}

int h264PpsId(const uint8_t* nal, size_t n) {
    if (n < 2) return -1;
    Bits b(nal + 1, n - 1);
    return int(b.ue());
}

// An avcC record, taken apart.
struct Avcc {
    uint8_t profile = 0, compat = 0, level = 0;
    int lengthSize = 4;
    std::vector<std::vector<uint8_t>> sps, pps;
    std::vector<uint8_t> tail;  // (High profiles' chroma format and bit depths)
};

bool parseAvcc(const uint8_t* d, size_t n, Avcc& a) {
    if (n < 7 || d[0] != 1) return false;
    a.profile = d[1], a.compat = d[2], a.level = d[3];
    a.lengthSize = (d[4] & 3) + 1;
    size_t i = 6;
    for (int k = 0, count = d[5] & 31; k < count; ++k) {
        if (i + 2 > n) return false;
        const size_t len = size_t(d[i]) << 8 | d[i + 1];
        i += 2;
        if (i + len > n) return false;
        a.sps.emplace_back(d + i, d + i + len);
        i += len;
    }
    if (i >= n) return false;
    for (int k = 0, count = d[i++]; k < count; ++k) {
        if (i + 2 > n) return false;
        const size_t len = size_t(d[i]) << 8 | d[i + 1];
        i += 2;
        if (i + len > n) return false;
        a.pps.emplace_back(d + i, d + i + len);
        i += len;
    }
    a.tail.assign(d + i, d + n);
    return !a.sps.empty() && !a.pps.empty();
}

std::vector<uint8_t> buildAvcc(const Avcc& a) {
    std::vector<uint8_t> o{1, a.profile, a.compat, a.level, uint8_t(0xfc | (a.lengthSize - 1)), uint8_t(0xe0 | a.sps.size())};
    for (const auto& s : a.sps) {
        o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
        o.insert(o.end(), s.begin(), s.end());
    }
    o.push_back(uint8_t(a.pps.size()));
    for (const auto& s : a.pps) {
        o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
        o.insert(o.end(), s.begin(), s.end());
    }
    o.insert(o.end(), a.tail.begin(), a.tail.end());
    return o;
}

// An hvcC record's VPS, SPS and PPS units (in that order) and its length size.
bool parseHvcc(const uint8_t* d, size_t n, std::vector<std::vector<uint8_t>>& sets, int& lengthSize) {
    if (n < 23 || d[0] != 1) return false;
    lengthSize = (d[21] & 3) + 1;
    std::vector<std::vector<uint8_t>> byType[3];
    size_t i = 23;
    for (int a = 0, arrays = d[22]; a < arrays; ++a) {
        if (i + 3 > n) return false;
        const int type = d[i] & 0x3f;
        const int count = d[i + 1] << 8 | d[i + 2];
        i += 3;
        for (int k = 0; k < count; ++k) {
            if (i + 2 > n) return false;
            const size_t len = size_t(d[i]) << 8 | d[i + 1];
            i += 2;
            if (i + len > n) return false;
            if (type >= 32 && type <= 34) byType[type - 32].emplace_back(d + i, d + i + len);
            i += len;
        }
    }
    sets.clear();
    for (auto& t : byType) sets.insert(sets.end(), t.begin(), t.end());
    return !byType[1].empty() && !byType[2].empty();
}

// ---- What a packet holds ----

enum : uint8_t {
    kCopyPoint = 1,  // a GOP may start here (H.264 IDR; HEVC IRAP; MPEG-2 GOP header with an I-picture)
    kIdr = 2,        // and nothing after it refers to anything before it (H.264 IDR, HEVC IDR, MPEG-2 closed GOP)
    kRasl = 4,       // HEVC: a RASL picture
};

uint8_t packetKind(int codec, const uint8_t* d, size_t n, int lengthSize) {
    uint8_t kind = 0;
    if (codec == kH264) {
        forEachNal(d, n, lengthSize, [&](const uint8_t* nal, size_t) {
            if ((nal[0] & 31) == 5) kind |= kCopyPoint | kIdr;
        });
    } else if (codec == kHevc) {
        bool seen = false;
        forEachNal(d, n, lengthSize, [&](const uint8_t* nal, size_t len) {
            if (len < 2 || seen) return;
            const int type = (nal[0] >> 1) & 0x3f;
            if (type >= 32) return;  // (not a picture's slice)
            seen = true;
            if (type >= 16 && type <= 23) kind |= kCopyPoint;
            if (type == 19 || type == 20) kind |= kIdr;
            if (type == 8 || type == 9) kind |= kRasl;
        });
    } else if (codec == kMpeg2) {
        bool seq = false, gop = false, closed = false;
        int type = 0;
        for (size_t i = 0; i + 8 <= n; ++i) {
            if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
            if (d[i + 3] == 0xb3) seq = true;
            else if (d[i + 3] == 0xb8) {
                gop = true;
                closed = (d[i + 7] >> 6) & 1;
            } else if (d[i + 3] == 0x00) {
                type = (d[i + 5] >> 3) & 7;
                break;
            }
        }
        // (with a sequence header: the GOP is then decoded with its own matrices and settings, whatever came before)
        if (seq && gop && type == 1) kind |= kCopyPoint | (closed ? kIdr : 0);
    }
    return kind;
}

// Whether the stream is shown turned (a display matrix with a rotation): its pictures are then decoded turned, as
// coded they are not.
bool turned(const AVStream* st) {
    const uint8_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    if (const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_DISPLAYMATRIX))
        matrix = sd->data;
#else
    matrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
    if (!matrix) return false;
    const double theta = av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    return !std::isnan(theta) && std::fabs(std::remainder(theta, 360.0)) > 0.5;
}

const char* x264Profile(int profile) {
    switch (profile & 0xff) {
    case 66: return "baseline";
    case 77: return "main";
    case 100: return "high";
    case 110: return "high10";
    case 122: return "high422";
    case 244: return "high444";
    default: return nullptr;
    }
}

}  // namespace

// ---- The plan ----

struct LongGopCopier::Packet {
    int64_t dts = 0, pts = 0;
    int64_t frame = 0;  // the source's frame (display order)
    int size = 0;
    uint8_t kind = 0;
    bool key = false;
};

struct LongGopCopier::Gop {
    size_t begin = 0, end = 0;  // packets [begin, end), decode order
    int64_t lo = 0, hi = 0;     // the frames it shows
    int64_t entryLo = 0;        // the first it shows when started after pictures not its own (HEVC: RASL left out)
    bool whole = false;         // its frames run on, one each
    bool entryWhole = false;
    bool idr = false;
    int64_t bytes = 0;
};

struct LongGopCopier::Source {
    Id media = 0;
    AVFormatContext* fmt = nullptr;
    int stream = -1;
    AVRational tb{1, 1};
    int64_t start = 0, ticks = 1;
    std::vector<uint8_t> config;              // the decoder configuration (avcC, hvcC)
    std::vector<uint8_t> sets;                // HEVC: its VPS, SPS and PPS, length-prefixed
    Avcc avcc;                                // H.264
    int profile = 0, level = 0;
    AVPacket* held = nullptr;                 // read, not yet used
    int64_t nextDts = AV_NOPTS_VALUE;         // the packet a plain read gives next
    bool usable = false;
    ~Source() {
        av_packet_free(&held);
        if (fmt) avformat_close_input(&fmt);
    }
};

struct LongGopCopier::Run {
    int source = -1;
    FrameTime e0 = 0;      // the export's frame (relative) showing source frame s0
    int64_t s0 = 0, len = 0;
    std::vector<Packet> packets;
    std::vector<Gop> gops;
};

struct LongGopCopier::Copy {
    int run = -1;
    size_t gop = 0;
    FrameTime lo = 0, hi = 0;  // export frames shown (relative, inclusive)
    bool dropRasl = false;
    bool broken = false;  // HEVC: its CRA picture marked a broken link (BLA), for what came before is not its own
    int64_t bytes = 0;
};

LongGopCopier::~LongGopCopier() {
    avcodec_free_context(&ctx_);
    av_packet_free(&pkt_);
}

std::unique_ptr<LongGopCopier> LongGopCopier::plan(const Project& p, const Sequence& seq, FrameTime in, FrameTime out, const ColorSpace& seqSpace,
                                                   const LongGopEncoder& encoder, AVFormatContext* oc, AVStream* vst, std::string* why) {
    auto none = [&](const std::string& w) -> std::unique_ptr<LongGopCopier> {
        if (why) *why = w;
        return nullptr;
    };
    const AVCodecContext* m = encoder.main;
    if (!m || !m->codec || !oc || !vst || out <= in) return none("no video");
    const std::string name = m->codec->name;
    const int codec = m->codec_id;
    if (!(name == "libx264" && codec == kH264) && !(name == "libx265" && codec == kHevc) && !(name == "mpeg2video" && codec == kMpeg2))
        return none("copies H.264 (x264), HEVC (x265) and MPEG-2, not " + name);
    const std::string format = oc->oformat->name;
    const bool isoFile = format.find("mp4") != std::string::npos || format.find("mov") != std::string::npos;
    if (codec == kMpeg2 ? !(isoFile || format == "mxf" || format.rfind("mxf", 0) == 0) : !isoFile)
        return none("copies into MP4 and MOV (MPEG-2: MXF and MOV)");
    const bool interlaced = m->field_order != AV_FIELD_PROGRESSIVE && m->field_order != AV_FIELD_UNKNOWN;
    if (interlaced && codec != kMpeg2) return none("copies interlaced MPEG-2 only");

    std::unique_ptr<LongGopCopier> lg(new LongGopCopier);
    lg->enc_ = encoder;
    lg->oc_ = oc;
    lg->vst_ = vst;
    lg->codec_ = codec;
    lg->frames_ = out - in;
    lg->gop_ = std::max(1, m->gop_size);

    // The runs: untouched frames of one clip, its source's frames one after another.
    for (FrameTime f = in; f < out; ++f) {
        const Clip* c = untouchedClipAt(seq, f);
        if (!c) continue;
        const double sf = c->sourceFrameAt(f);
        const int64_t s = std::llround(sf);
        if (std::fabs(sf - double(s)) > 1e-6 || s < 0) continue;
        Run* last = lg->runs_.empty() ? nullptr : &lg->runs_.back();
        if (last && last->e0 + last->len == f - in && last->s0 + last->len == s && lg->sources_[size_t(last->source)]->media == c->mediaId) {
            ++last->len;
            continue;
        }
        int src = -1;
        for (size_t i = 0; i < lg->sources_.size(); ++i)
            if (lg->sources_[i]->media == c->mediaId) src = int(i);
        if (src < 0) {
            lg->sources_.push_back(std::make_unique<Source>());
            lg->sources_.back()->media = c->mediaId;
            src = int(lg->sources_.size()) - 1;
        }
        Run r;
        r.source = src;
        r.e0 = f - in;
        r.s0 = s;
        r.len = 1;
        lg->runs_.push_back(std::move(r));
    }
    if (lg->runs_.empty()) return none("no untouched footage");

    // The sources: the same pictures the encoder would make, in a file whose packets can be taken as they are.
    std::set<int> spsIds, ppsIds;
    std::map<int, std::vector<uint8_t>> spsById, ppsById;
    for (auto& sp : lg->sources_) {
        Source& src = *sp;
        const MediaItem* mi = p.findMedia(src.media);
        if (!mi || mi->kind != MediaKind::Video || mi->path.empty()) continue;
        if (mediaColorSpace(*mi).id != seqSpace.id) continue;  // (would be converted)
        if (!interpretationOf(*mi).empty() || !mi->stereo.empty()) continue;
        if (openMediaInput(&src.fmt, mi->path) < 0) continue;
        if (avformat_find_stream_info(src.fmt, nullptr) < 0) continue;
        src.stream = av_find_best_stream(src.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (src.stream < 0) continue;
        for (unsigned i = 0; i < src.fmt->nb_streams; ++i)
            if (int(i) != src.stream) src.fmt->streams[i]->discard = AVDISCARD_ALL;
        const AVStream* st = src.fmt->streams[src.stream];
        const AVCodecParameters* par = st->codecpar;
        const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        const bool progressive = par->field_order == AV_FIELD_PROGRESSIVE || par->field_order == AV_FIELD_UNKNOWN;
        if (par->codec_id != codec || par->width != m->width || par->height != m->height || par->format != m->pix_fmt ||
            av_cmp_q(rate, m->framerate) != 0 || seq.width != par->width || seq.height != par->height)
            continue;
        if (interlaced ? par->field_order != m->field_order : !progressive) continue;
        // Shown as coded: not turned, square or the same pixels, the same range (a full-range HEVC is yuv420p as a
        // limited one is).
        const AVRational sar = par->sample_aspect_ratio.num > 0 ? par->sample_aspect_ratio : AVRational{1, 1};
        const AVRational msar = m->sample_aspect_ratio.num > 0 ? m->sample_aspect_ratio : AVRational{1, 1};
        if (turned(st) || av_cmp_q(sar, msar) != 0) continue;
        if (par->color_range == AVCOL_RANGE_JPEG ? m->color_range != AVCOL_RANGE_JPEG : m->color_range == AVCOL_RANGE_JPEG) continue;
        src.tb = st->time_base;
        // (frames counted from the file's start, as the decoder counts them: the earliest of its streams)
        src.start = src.fmt->start_time != AV_NOPTS_VALUE ? av_rescale_q(src.fmt->start_time, AV_TIME_BASE_Q, src.tb) : 0;
        src.ticks = av_rescale_q(1, av_inv_q(rate), src.tb);
        if (src.ticks <= 0 || av_cmp_q(av_mul_q(AVRational{int(src.ticks), 1}, src.tb), av_inv_q(rate)) != 0) continue;
        src.profile = par->profile;
        src.level = par->level;
        src.config.assign(par->extradata, par->extradata + par->extradata_size);
        if (codec == kH264) {
            // Its parameter sets beside the others' in the one avcC: none under an id another holds differently.
            if (!parseAvcc(src.config.data(), src.config.size(), src.avcc)) continue;
            if (lg->firstSource_ >= 0 && src.avcc.lengthSize != lg->lengthSize_) continue;
            bool clash = false;
            for (const auto& s : src.avcc.sps) {
                const int id = h264SpsId(s.data(), s.size());
                if (id < 0 || id > 31 || (spsById.count(id) && spsById[id] != s)) clash = true;
            }
            for (const auto& s : src.avcc.pps) {
                const int id = h264PpsId(s.data(), s.size());
                if (id < 0 || id > 255 || (ppsById.count(id) && ppsById[id] != s)) clash = true;
            }
            if (clash) continue;
            for (const auto& s : src.avcc.sps) spsById[h264SpsId(s.data(), s.size())] = s;
            for (const auto& s : src.avcc.pps) ppsById[h264PpsId(s.data(), s.size())] = s;
            if (lg->firstSource_ < 0) lg->lengthSize_ = src.avcc.lengthSize;
        } else if (codec == kHevc) {
            std::vector<std::vector<uint8_t>> sets;
            int size = 4;
            if (!parseHvcc(src.config.data(), src.config.size(), sets, size)) continue;
            if (lg->firstSource_ >= 0 && size != lg->lengthSize_) continue;
            if (lg->firstSource_ < 0) lg->lengthSize_ = size;
            for (const auto& s : sets) putNal(src.sets, s.data(), s.size(), size);
        }
        src.held = av_packet_alloc();
        if (!src.held) continue;
        src.usable = true;
        if (lg->firstSource_ < 0) lg->firstSource_ = int(&sp - lg->sources_.data());
    }
    if (lg->firstSource_ < 0) return none("no source coded as the export is");

    // Each run's packets, from the copy point before it to the one after it.
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return none("out of memory");
    std::vector<int64_t> gopLengths;
    for (Run& r : lg->runs_) {
        Source& src = *lg->sources_[size_t(r.source)];
        if (!src.usable || r.len < 2) continue;
        const int64_t s1 = r.s0 + r.len;
        const int64_t target = src.start + std::max<int64_t>(0, r.s0 - 4) * src.ticks;
        if (av_seek_frame(src.fmt, src.stream, target, AVSEEK_FLAG_BACKWARD) < 0 &&
            av_seek_frame(src.fmt, src.stream, src.start, AVSEEK_FLAG_BACKWARD) < 0)
            continue;
        src.nextDts = AV_NOPTS_VALUE;
        av_packet_unref(src.held);
        bool seenCopyPoint = false, bad = false;
        for (;;) {
            av_packet_unref(pkt);
            if (av_read_frame(src.fmt, pkt) < 0) break;
            if (pkt->stream_index != src.stream) continue;
            if (pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE) {
                bad = true;
                break;
            }
            Packet k;
            k.dts = pkt->dts;
            k.pts = pkt->pts;
            k.size = pkt->size;
            k.key = pkt->flags & AV_PKT_FLAG_KEY;
            const int64_t rel = pkt->pts - src.start;
            k.frame = rel >= 0 ? (rel + src.ticks / 2) / src.ticks : -1 - (-rel - 1 + src.ticks / 2) / src.ticks;
            if (std::llabs(rel - k.frame * src.ticks) > src.ticks / 4) {  // (not on a frame)
                bad = true;
                break;
            }
            k.kind = packetKind(codec, pkt->data, size_t(pkt->size), lg->lengthSize_);
            if ((k.kind & kCopyPoint) && seenCopyPoint && k.frame >= s1) break;  // (the GOP after the run's)
            seenCopyPoint = seenCopyPoint || (k.kind & kCopyPoint);
            r.packets.push_back(k);
        }
        src.nextDts = AV_NOPTS_VALUE;  // (where the reader is now is not kept)
        if (bad) {
            r.packets.clear();
            continue;
        }
        // GOPs, from each copy point to the next (the scan stopped at the file's end or at a copy point: the last is whole).
        for (size_t i = 0; i < r.packets.size(); ++i) {
            if (!(r.packets[i].kind & kCopyPoint)) continue;
            size_t j = i + 1;
            while (j < r.packets.size() && !(r.packets[j].kind & kCopyPoint)) ++j;
            Gop g;
            g.begin = i, g.end = j;
            g.idr = r.packets[i].kind & kIdr;
            std::vector<int64_t> all, entry;
            for (size_t k = i; k < j; ++k) {
                all.push_back(r.packets[k].frame);
                if (!(r.packets[k].kind & kRasl)) entry.push_back(r.packets[k].frame);
                g.bytes += r.packets[k].size;
            }
            auto runsOn = [](std::vector<int64_t>& v, int64_t& lo, int64_t& hi) {
                if (v.empty()) return false;
                std::sort(v.begin(), v.end());
                lo = v.front(), hi = v.back();
                return hi - lo + 1 == int64_t(v.size()) && std::adjacent_find(v.begin(), v.end()) == v.end();
            };
            int64_t elo = 0, ehi = 0;
            g.whole = runsOn(all, g.lo, g.hi);
            g.entryWhole = runsOn(entry, elo, ehi) && ehi == g.hi;
            g.entryLo = elo;
            if (codec == kMpeg2 && !g.idr) {
                // An open GOP with no leading B-pictures needs nothing before it.
                const int64_t first = r.packets[i].frame;
                if (g.lo == first) g.idr = true;
            }
            r.gops.push_back(g);
            gopLengths.push_back(g.hi - g.lo + 1);
            i = j - 1;
        }
    }
    av_packet_free(&pkt);
    if (!gopLengths.empty()) {
        std::nth_element(gopLengths.begin(), gopLengths.begin() + gopLengths.size() / 2, gopLengths.end());
        lg->gop_ = int(std::clamp<int64_t>(gopLengths[gopLengths.size() / 2], 1, 600));
    }

    // Which GOPs are copied: wholly inside a run, and decodable where they land.
    FrameTime lastCopyEnd = -2;  // the last copied frame so far
    for (size_t ri = 0; ri < lg->runs_.size(); ++ri) {
        const Run& r = lg->runs_[ri];
        const int64_t s1 = r.s0 + r.len;
        size_t prev = SIZE_MAX;  // the run's GOP copied just before
        for (size_t gi = 0; gi < r.gops.size(); ++gi) {
            const Gop& g = r.gops[gi];
            Copy c;
            c.run = int(ri);
            c.gop = gi;
            c.bytes = g.bytes;
            bool ok = false;
            if (prev != SIZE_MAX && prev + 1 == gi && r.e0 + (g.lo - r.s0) == lastCopyEnd + 1) {
                // After the GOP before it, as in the source.
                ok = g.whole && g.lo >= r.s0 && g.hi < s1;
                c.lo = r.e0 + (g.lo - r.s0);
            } else {
                const bool hevcCra = codec == kHevc && !g.idr;
                const int64_t lo = hevcCra ? g.entryLo : g.lo;
                ok = (hevcCra ? g.entryWhole : g.whole) && lo >= r.s0 && g.hi < s1;
                c.lo = r.e0 + (lo - r.s0);
                const bool afterCopy = c.lo == lastCopyEnd + 1, atStart = c.lo == 0;
                if (ok && hevcCra) {
                    // A CRA after pictures not its own: its RASL pictures go, and it is marked a broken link (BLA), so
                    // the decoder starts afresh there, as HEVC means splices to be made.
                    c.dropRasl = true;
                    c.broken = true;
                } else if (ok && codec == kMpeg2 && !g.idr) {
                    // Its leading B-pictures refer to the frame before them: that frame shown, encoded, just before.
                    ok = !afterCopy && !atStart && g.lo - 1 >= r.s0;
                }
            }
            if (!ok) {
                prev = SIZE_MAX;
                continue;
            }
            c.hi = r.e0 + (g.hi - r.s0);
            if (c.dropRasl) {
                c.bytes = 0;
                for (size_t k = g.begin; k < g.end; ++k)
                    if (!(r.packets[k].kind & kRasl)) c.bytes += r.packets[k].size;
            }
            lg->copies_.push_back(c);
            lastCopyEnd = c.hi;
            prev = gi;
        }
    }
    if (lg->copies_.empty()) return none("no whole GOP of a source is untouched");

    // The decode delay: the most any picture is decoded ahead of where it is shown (the encoders': their B-pictures).
    lg->depth_ = codec == kMpeg2 ? std::max(0, m->max_b_frames) : 3;
    for (const Copy& c : lg->copies_) {
        const Run& r = lg->runs_[size_t(c.run)];
        const Gop& g = r.gops[c.gop];
        const int64_t first = c.lo - r.e0 + r.s0;  // its first frame shown, in the source
        int64_t i = 0;
        for (size_t k = g.begin; k < g.end; ++k) {
            if (c.dropRasl && (r.packets[k].kind & kRasl)) continue;
            lg->depth_ = int(std::max<int64_t>(lg->depth_, i - (r.packets[k].frame - first)));
            ++i;
        }
    }
    // H.264: the encoder's parameter sets under an id no source's has.
    if (codec == kH264) {
        for (int id = 0; id < 32; ++id)
            if (!spsById.count(id) && !ppsById.count(id)) {
                lg->spsId_ = id;
                break;
            }
        if (lg->spsId_ < 0) return none("the sources use every parameter set id");
        std::string e;
        AVCodecContext* probe = lg->openEncoder(4000000, &e);
        if (!probe) return none(e);
        Avcc encoderSets;
        for (const auto& [d, n] : annexBNals(probe->extradata, size_t(std::max(0, probe->extradata_size)))) {
            const int type = d[0] & 31;
            if (type == 7 && h264SpsId(d, n) == lg->spsId_) encoderSets.sps.emplace_back(d, d + n);
            if (type == 8 && h264PpsId(d, n) == lg->spsId_) encoderSets.pps.emplace_back(d, d + n);
        }
        avcodec_free_context(&probe);
        if (encoderSets.sps.size() != 1 || encoderSets.pps.size() != 1) return none("x264 did not take the parameter set id");
        for (const auto* list : {&encoderSets.sps, &encoderSets.pps})
            for (const auto& s : *list) putNal(lg->encoderSets_, s.data(), s.size(), lg->lengthSize_);
        lg->activeEncoderSets_ = lg->encoderSets_;
        // The decoder configuration: every source's sets and the encoder's.
        Avcc all = lg->sources_[size_t(lg->firstSource_)]->avcc;
        all.sps.clear(), all.pps.clear();
        for (const auto& [id, s] : spsById) all.sps.push_back(s);
        for (const auto& [id, s] : ppsById) all.pps.push_back(s);
        all.sps.push_back(encoderSets.sps[0]);
        all.pps.push_back(encoderSets.pps[0]);
        if (all.sps.size() > 31 || all.pps.size() > 255) return none("too many parameter sets");
        lg->sources_[size_t(lg->firstSource_)]->config = buildAvcc(all);
    }
    if (codec == kHevc) lg->firstSource_ = lg->runs_[size_t(lg->copies_.front().run)].source;
    lg->pkt_ = av_packet_alloc();
    if (!lg->pkt_) return none("out of memory");
    return lg;
}

bool LongGopCopier::prepareStream(std::string* error) {
    AVCodecParameters* par = vst_->codecpar;
    if (codec_ == kH264 || codec_ == kHevc) {
        const Source& first = *sources_[size_t(firstSource_)];
        const std::vector<uint8_t>& config = first.config;
        av_freep(&par->extradata);
        par->extradata_size = 0;
        par->extradata = static_cast<uint8_t*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) return fail(error, "Out of memory");
        std::memcpy(par->extradata, config.data(), config.size());
        par->extradata_size = int(config.size());
        par->profile = first.profile;
        par->level = first.level;
        if (codec_ == kHevc) par->codec_tag = MKTAG('h', 'e', 'v', '1');  // (parameter sets in the video)
    }
    return true;
}

// ---- Writing ----

bool LongGopCopier::write(AVPacket* pkt, FrameTime display, std::string* error) {
    const AVRational tb = enc_.main->time_base;
    pkt->pts = display;
    pkt->dts = decoded_++ - depth_;
    if (pkt->dts > pkt->pts) return fail(error, "Smart rendering: a picture would be decoded after it is shown");
    pkt->duration = 1;
    pkt->pos = -1;
    pkt->stream_index = vst_->index;
    av_packet_rescale_ts(pkt, tb, vst_->time_base);
    const int rc = av_interleaved_write_frame(oc_, pkt);
    if (rc < 0) return fail(error, "Writing video failed: " + averr(rc));
    return true;
}

int64_t LongGopCopier::partBitrate(FrameTime a, FrameTime b) const {
    // The copied GOPs either side of [a, b): their bits a frame at the export's rate.
    const double fps = av_q2d(enc_.main->framerate);
    int64_t bytes = 0, frames = 0;
    for (const Copy& c : copies_)
        if (c.hi + 1 == a || c.lo == b) bytes += c.bytes, frames += c.hi - c.lo + 1;
    if (frames == 0)
        for (const Copy& c : copies_) bytes += c.bytes, frames += c.hi - c.lo + 1;
    if (frames == 0) return 4000000;
    return std::clamp<int64_t>(int64_t(double(bytes) * 8.0 / double(frames) * fps), 100000, 800000000);
}

AVCodecContext* LongGopCopier::openEncoder(int64_t bitrate, std::string* error) const {
    const AVCodecContext* m = enc_.main;
    const AVCodec* codec = avcodec_find_encoder_by_name(m->codec->name);
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!c) {
        fail(error, std::string("Video encoder not available: ") + m->codec->name);
        return nullptr;
    }
    c->width = m->width;
    c->height = m->height;
    c->time_base = m->time_base;
    c->framerate = m->framerate;
    c->sample_aspect_ratio = m->sample_aspect_ratio;
    c->pix_fmt = m->pix_fmt;
    c->color_primaries = m->color_primaries;
    c->color_trc = m->color_trc;
    c->colorspace = m->colorspace;
    c->color_range = m->color_range;
    c->chroma_sample_location = m->chroma_sample_location;
    c->field_order = m->field_order;
    c->flags |= m->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME);
    c->thread_count = 0;
    c->gop_size = gop_;
    AVDictionary* opts = nullptr;
    const Source& first = *sources_[size_t(firstSource_)];
    if (codec_ == kH264 || codec_ == kHevc) {
        // The export's quality, held under the rate of the GOPs copied either side (over a second). (Aiming at that
        // rate instead overshoots it several times in a part this short, its rate control starting low.)
        if (enc_.bitrate > 0) c->bit_rate = std::min(enc_.bitrate, bitrate);
        else av_dict_set_int(&opts, "crf", enc_.crf, 0);
        c->rc_max_rate = bitrate;
        c->rc_buffer_size = int(std::min<int64_t>(bitrate, INT_MAX));
    }
    if (codec_ == kH264) {
        // Closed GOPs (x264's own), the source's profile and level, its parameter sets under their own id, kept out of
        // the pictures (they go in the decoder configuration).
        c->max_b_frames = 3;
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (const char* profile = x264Profile(first.profile)) av_dict_set(&opts, "profile", profile, 0);
        if (first.level > 0) c->level = first.level;
        if (!enc_.preset.empty()) av_dict_set(&opts, "preset", enc_.preset.c_str(), 0);
        av_dict_set(&opts, "x264-params", ("sps-id=" + std::to_string(spsId_)).c_str(), 0);
    } else if (codec_ == kHevc) {
        c->max_b_frames = 3;
        std::string params = enc_.x265Params.empty() ? std::string("log-level=error") : enc_.x265Params;
        // (closed GOPs; no settings text with each keyframe's repeated headers)
        params += ":open-gop=0:repeat-headers=1:info=0:bframes=3:keyint=" + std::to_string(gop_);
        if (first.level > 0) {
            char lv[32];
            std::snprintf(lv, sizeof lv, ":level-idc=%g", first.level / 30.0);
            params += lv;
        }
        av_dict_set(&opts, "x265-params", params.c_str(), 0);
        if (!enc_.preset.empty()) av_dict_set(&opts, "preset", enc_.preset.c_str(), 0);
    } else {
        // MPEG-2: the export's own rate control (XDCAM HD422's constant rate) and GOP.
        c->bit_rate = m->bit_rate;
        c->rc_min_rate = m->rc_min_rate;
        c->rc_max_rate = m->rc_max_rate;
        c->rc_buffer_size = m->rc_buffer_size;
        c->rc_initial_buffer_occupancy = m->rc_initial_buffer_occupancy;
        c->gop_size = m->gop_size;
        c->max_b_frames = m->max_b_frames;
        c->intra_dc_precision = m->intra_dc_precision;
        c->qmin = m->qmin;
        c->qmax = m->qmax;
        av_dict_set(&opts, "intra_vlc", "1", 0);
        av_dict_set(&opts, "non_linear_quant", "1", 0);
    }
    const int rc = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        avcodec_free_context(&c);
        fail(error, "Cannot open the video encoder for the parts around edits: " + averr(rc));
        return nullptr;
    }
    return c;
}

bool LongGopCopier::flushPending(std::string* error) {
    if (!havePending_) return true;
    havePending_ = false;
    AVPacket* out = av_packet_alloc();
    if (!out || av_new_packet(out, int(pending_.size())) < 0) {
        av_packet_free(&out);
        return fail(error, "Out of memory");
    }
    std::memcpy(out->data, pending_.data(), pending_.size());
    if (pendingKey_) out->flags |= AV_PKT_FLAG_KEY;
    const bool ok = write(out, pendingPts_, error);
    av_packet_free(&out);
    return ok;
}

bool LongGopCopier::receive(std::string* error) {
    for (;;) {
        const int rc = avcodec_receive_packet(ctx_, pkt_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
        if (!flushPending(error)) {
            av_packet_unref(pkt_);
            return false;
        }
        pending_.clear();
        if (codec_ == kH264 || codec_ == kHevc) {
            if (codec_ == kH264 && firstOfPart_ && activeEncoderSets_ != partSets_)
                pending_ = partSets_, activeEncoderSets_ = partSets_;  // (this part's sets differ: given in the picture)
            if (!toLengthPrefixed(pkt_->data, size_t(pkt_->size), lengthSize_, pending_)) {
                av_packet_unref(pkt_);
                return fail(error, "Smart rendering: a coded picture is too large for the source's NAL unit lengths");
            }
        } else {
            pending_.assign(pkt_->data, pkt_->data + pkt_->size);
        }
        firstOfPart_ = false;
        pendingPts_ = partStart_ + pkt_->pts;
        pendingKey_ = pkt_->flags & AV_PKT_FLAG_KEY;
        havePending_ = true;
        av_packet_unref(pkt_);
    }
}

bool LongGopCopier::startPart(FrameTime rel, std::string* error) {
    partStart_ = rel;
    partEnd_ = cursor_ < copies_.size() ? copies_[cursor_].lo : frames_;
    ctx_ = openEncoder(codec_ == kMpeg2 ? 0 : partBitrate(partStart_, partEnd_), error);
    if (!ctx_) return false;
    firstOfPart_ = true;
    partSets_.clear();
    if (codec_ == kH264) {
        for (const auto& [d, n] : annexBNals(ctx_->extradata, size_t(std::max(0, ctx_->extradata_size)))) {
            const int type = d[0] & 31;
            if (type == 7 || type == 8) putNal(partSets_, d, n, lengthSize_);
        }
    }
    ++parts_;
    return true;
}

bool LongGopCopier::endPart(std::string* error) {
    if (!ctx_) return true;
    int rc = avcodec_send_frame(ctx_, nullptr);
    if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
    if (!receive(error)) return false;
    avcodec_free_context(&ctx_);
    return flushPending(error);
}

bool LongGopCopier::encode(AVFrame* frame, FrameTime rel, std::string* error) {
    if (!ctx_ && !startPart(rel, error)) return false;
    frame->pts = rel - partStart_;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    const int rc = avcodec_send_frame(ctx_, frame);
    if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
    if (!receive(error)) return false;
    if (rel + 1 >= partEnd_) return endPart(error);
    return true;
}

bool LongGopCopier::finish(std::string* error) { return endPart(error); }

int LongGopCopier::copy(FrameTime rel, std::string* error) {
    while (cursor_ < copies_.size() && copies_[cursor_].hi < rel) ++cursor_;
    if (cursor_ >= copies_.size() || rel < copies_[cursor_].lo) return 0;
    if (rel == copies_[cursor_].lo) {
        if (!endPart(error)) return -1;  // (a part ends where a copy starts; done already unless frames were skipped)
        if (!writeGop(copies_[cursor_], error)) return -1;
    }
    return 1;
}

bool LongGopCopier::writeGop(const Copy& c, std::string* error) {
    const Run& r = runs_[size_t(c.run)];
    const Gop& g = r.gops[c.gop];
    Source& src = *sources_[size_t(r.source)];
    const int64_t want = r.packets[g.begin].dts;
    // To the GOP's first packet: read on when the reader is there, else seek (to it, or before it and read on).
    if (src.nextDts != want) {
        av_packet_unref(src.held);
        bool found = false;
        for (int attempt = 0; attempt < 3 && !found; ++attempt) {
            const int64_t target = attempt == 2 ? src.start : want - int64_t(attempt) * int64_t(gop_ + 4) * src.ticks;
            if (av_seek_frame(src.fmt, src.stream, target, AVSEEK_FLAG_BACKWARD) < 0) continue;
            for (;;) {
                av_packet_unref(src.held);
                if (av_read_frame(src.fmt, src.held) < 0) break;
                if (src.held->stream_index != src.stream) continue;
                if (src.held->dts == want) {
                    found = true;
                    break;
                }
                if (src.held->dts > want) break;  // (landed after it)
            }
        }
        if (!found) {
            av_packet_unref(src.held);
            return fail(error, "Smart rendering: cannot find a GOP again in its source");
        }
    }
    bool first = true;
    for (size_t k = g.begin; k < g.end; ++k) {
        // The packet: the one held, else the next read.
        if (!src.held->data) {
            for (;;) {
                if (av_read_frame(src.fmt, src.held) < 0) return fail(error, "Smart rendering: the source ended early");
                if (src.held->stream_index == src.stream) break;
                av_packet_unref(src.held);
            }
        }
        const Packet& pk = r.packets[k];
        if (src.held->dts != pk.dts || src.held->size != pk.size) {
            av_packet_unref(src.held);
            src.nextDts = AV_NOPTS_VALUE;
            return fail(error, "Smart rendering: a source changed while it was exported");
        }
        if (c.dropRasl && (pk.kind & kRasl)) {
            av_packet_unref(src.held);
            continue;
        }
        const FrameTime display = r.e0 + (pk.frame - r.s0);
        AVPacket* out = av_packet_alloc();
        if (!out) return fail(error, "Out of memory");
        const bool prefix = first && codec_ == kHevc;
        if (prefix) {
            // The source's parameter sets in front of every copied GOP, so it decodes whatever the decoder held before
            // (another source's, or an encoded part's under the same ids, as after a seek).
            if (av_new_packet(out, int(src.sets.size()) + src.held->size) < 0) {
                av_packet_free(&out);
                return fail(error, "Out of memory");
            }
            std::memcpy(out->data, src.sets.data(), src.sets.size());
            std::memcpy(out->data + src.sets.size(), src.held->data, size_t(src.held->size));
            out->flags = src.held->flags;
            av_packet_unref(src.held);
        } else {
            av_packet_move_ref(out, src.held);
        }
        if (first && c.broken) {
            // The CRA's slices as BLA_W_RADL (its RASL pictures are left out): only the NAL unit type changes.
            if (av_packet_make_writable(out) < 0) {
                av_packet_free(&out);
                return fail(error, "Out of memory");
            }
            forEachNal(out->data, size_t(out->size), lengthSize_, [](const uint8_t* nal, size_t len) {
                if (len >= 2 && ((nal[0] >> 1) & 0x3f) == 21) const_cast<uint8_t*>(nal)[0] = uint8_t(17 << 1 | (nal[0] & 0x81));
            });
        }
        out->flags &= AV_PKT_FLAG_KEY | AV_PKT_FLAG_DISPOSABLE;
        av_packet_free_side_data(out);
        const bool ok = write(out, display, error);
        av_packet_free(&out);
        if (!ok) return false;
        first = false;
        ++copied_;
    }
    src.nextDts = g.end < r.packets.size() ? r.packets[g.end].dts : AV_NOPTS_VALUE;
    return true;
}

}  // namespace montage
