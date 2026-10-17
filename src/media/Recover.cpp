#include "Recover.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <tuple>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace montage {

namespace {

using u8 = unsigned char;

uint16_t be16(const u8* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const u8* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const u8* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const u8* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const u8* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

constexpr uint32_t fcc(const char (&s)[5]) {
    return uint32_t(u8(s[0])) << 24 | uint32_t(u8(s[1])) << 16 | uint32_t(u8(s[2])) << 8 | u8(s[3]);
}

void put8(QByteArray& b, unsigned v) { b.append(char(v & 0xff)); }
void put16(QByteArray& b, unsigned v) { put8(b, v >> 8), put8(b, v); }
void put32(QByteArray& b, uint32_t v) { put16(b, v >> 16), put16(b, v & 0xffff); }
void put64(QByteArray& b, uint64_t v) { put32(b, uint32_t(v >> 32)), put32(b, uint32_t(v)); }
void set32(QByteArray& b, int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (24 - 8 * i));
}
void set64(QByteArray& b, int at, uint64_t v) { set32(b, at, uint32_t(v >> 32)), set32(b, at + 4, uint32_t(v)); }

QByteArray box(uint32_t type, const QByteArray& payload) {
    QByteArray b;
    put32(b, uint32_t(8 + payload.size()));
    put32(b, type);
    return b + payload;
}
QByteArray fullBox(uint32_t type, int version, const QByteArray& payload) {
    QByteArray b;
    put32(b, uint32_t(version) << 24);
    return box(type, b + payload);
}

QString fourccText(uint32_t f) {
    QString s;
    for (int i = 3; i >= 0; --i) {
        const char c = char(f >> (8 * i));
        s += c >= 32 && c < 127 ? QChar(c) : QChar('?');
    }
    return s.trimmed();
}

// An ISO BMFF box: where it starts, where its contents start and where it ends.
struct Box {
    uint32_t type = 0;
    int64_t start = 0, body = 0, end = 0;
};

// The boxes in [b, e), up to the first that does not fit.
std::vector<Box> children(const u8* d, int64_t b, int64_t e) {
    std::vector<Box> out;
    int64_t p = b;
    while (p + 8 <= e) {
        uint64_t size = be32(d + p);
        int64_t hdr = 8;
        if (size == 1) {
            if (p + 16 > e) break;
            size = be64(d + p + 8);
            hdr = 16;
        } else if (size == 0) {
            size = uint64_t(e - p);
        }
        if (size < uint64_t(hdr) || size > uint64_t(e - p)) break;
        out.push_back({be32(d + p + 4), p, p + hdr, p + int64_t(size)});
        p += int64_t(size);
    }
    return out;
}
const Box* child(const std::vector<Box>& v, uint32_t type) {
    for (const Box& b : v)
        if (b.type == type) return &b;
    return nullptr;
}

// Exp-Golomb and plain bits over the start of a NAL unit (emulation prevention cannot occur in the few bits read).
struct Bits {
    const u8* p;
    int64_t n;
    int64_t bit = 0;
    int get() {
        if (bit >= n * 8) return -1;
        const int v = p[bit >> 3] >> (7 - (bit & 7)) & 1;
        ++bit;
        return v;
    }
    int64_t ue() {
        int zeros = 0;
        for (;;) {
            const int b = get();
            if (b < 0 || zeros > 31) return -1;
            if (b) break;
            ++zeros;
        }
        int64_t v = 0;
        for (int i = 0; i < zeros; ++i) {
            const int b = get();
            if (b < 0) return -1;
            v = v << 1 | b;
        }
        return (int64_t(1) << zeros) - 1 + v;
    }
};

enum class Codec { Other, H264, Hevc, ProRes, Aac, Pcm };

// What the reference clip says about one of its tracks.
struct RefTrack {
    bool video = false;
    QByteArray trak;  // the whole trak box
    QByteArray stsd;  // its sample descriptions
    uint32_t entry = 0;
    Codec codec = Codec::Other;
    uint32_t timescale = 0;
    uint32_t delta = 1;      // the usual sample duration
    bool ctts = false, stss = false;
    uint32_t constSize = 0;  // stsz's one sample size; 0 = each its own
    uint32_t maxSize = 0;
    int64_t mediaTime = -1;  // the first edit's start in the media; -1 = no edit list
    // H.264 and HEVC
    int nalLen = 4;
    std::set<int> pps;       // picture parameter set ids in the configuration
    std::set<int> types;     // NAL unit types seen
    QByteArray extradata;    // avcC or hvcC contents; AAC's AudioSpecificConfig
    // sound
    int channels = 0, rate = 0;
    int step = 0;            // bytes per PCM frame
    std::set<int> aacHeads;  // each AAC frame's first byte >> 1 (its first element and tag)
    std::vector<int64_t> chunkSizes;  // PCM chunk sizes in bytes, most common first
    int64_t maxChunk = 0;
};

struct Reference {
    QByteArray ftyp, mvhd;
    uint32_t movieTimescale = 600;
    int video = -1, audio = -1;  // in tracks
    std::vector<RefTrack> tracks;
    std::vector<int64_t> otherSizes, gapSizes;  // chunks of tracks not rebuilt, and bytes between chunks; most common first
    int64_t maxOther = 0;
    std::map<std::pair<int, int>, int> transitions;  // chunk kind followed by chunk kind, counted
    int dropped = 0;  // picture and sound tracks after the first of each, which are not rebuilt
};

enum Kind { KVideo = 0, KAudio = 1, KOther = 2, KGap = 3, KStart = 4, KResync = 5 };

std::vector<int64_t> byFrequency(const std::vector<int64_t>& v, size_t limit) {
    std::map<int64_t, int> count;
    for (int64_t x : v)
        if (x > 0) ++count[x];
    std::vector<std::pair<int, int64_t>> order;
    for (const auto& [x, n] : count) order.push_back({-n, x});
    std::sort(order.begin(), order.end());
    std::vector<int64_t> out;
    for (const auto& o : order) {
        if (out.size() >= limit) break;
        out.push_back(o.second);
    }
    return out;
}

// The AudioSpecificConfig in an esds box's descriptors.
QByteArray ascFromEsds(const u8* p, const u8* e) {
    auto length = [&](const u8*& q) {
        int64_t n = 0;
        for (int i = 0; i < 4 && q < e; ++i) {
            const u8 b = *q++;
            n = n << 7 | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        return n;
    };
    p += 4;  // version and flags
    while (p + 2 <= e) {
        const u8 tag = *p++;
        const int64_t len = length(p);
        const u8* body = p;
        if (len < 0 || len > e - body) return {};
        if (tag == 3) {
            if (len < 3) return {};
            const u8 flags = body[2];
            p = body + 3;
            if (flags & 0x80) p += 2;
            if ((flags & 0x40) && p < e) p += 1 + *p;
            if (flags & 0x20) p += 2;
        } else if (tag == 4) {
            p = body + 13;
        } else if (tag == 5) {
            return QByteArray(reinterpret_cast<const char*>(body), int(len));
        } else {
            p = body + len;
        }
    }
    return {};
}

// The offset of `type` inside [b, e) as a box's type field (a box starts four bytes before it), or -1.
int64_t findType(const u8* d, int64_t b, int64_t e, uint32_t type) {
    for (int64_t p = b + 4; p + 4 <= e; ++p)
        if (be32(d + p) == type && be32(d + p - 4) >= 8 && int64_t(be32(d + p - 4)) <= e - (p - 4)) return p - 4;
    return -1;
}

// Reads the sample description of a track into `t`.
void readSampleEntry(const u8* d, const Box& stsd, RefTrack& t) {
    const int64_t e0 = stsd.body + 8;
    if (e0 + 16 > stsd.end) return;
    const int64_t eEnd = std::min<int64_t>(stsd.end, e0 + be32(d + e0));
    t.entry = be32(d + e0 + 4);
    const uint32_t f = t.entry;
    if (t.video) {
        if (f == fcc("avc1") || f == fcc("avc3")) {
            t.codec = Codec::H264;
            const int64_t c = findType(d, e0 + 8, eEnd, fcc("avcC"));
            if (c >= 0) {
                const int64_t b = c + 8, e = c + be32(d + c);
                t.extradata = QByteArray(reinterpret_cast<const char*>(d + b), int(e - b));
                if (e - b >= 7) {
                    t.nalLen = (d[b + 4] & 3) + 1;
                    int64_t p = b + 6;
                    for (int i = 0, n = d[b + 5] & 31; i < n && p + 2 <= e; ++i) p += 2 + be16(d + p);
                    if (p < e) {
                        const int n = d[p++];
                        for (int i = 0; i < n && p + 2 <= e; ++i) {
                            const int len = be16(d + p);
                            if (p + 2 + len > e || len < 2) break;
                            Bits bits{d + p + 3, len - 1};
                            const int64_t id = bits.ue();
                            if (id >= 0) t.pps.insert(int(id));
                            p += 2 + len;
                        }
                    }
                }
            }
        } else if (f == fcc("hvc1") || f == fcc("hev1")) {
            t.codec = Codec::Hevc;
            const int64_t c = findType(d, e0 + 8, eEnd, fcc("hvcC"));
            if (c >= 0) {
                const int64_t b = c + 8, e = c + be32(d + c);
                t.extradata = QByteArray(reinterpret_cast<const char*>(d + b), int(e - b));
                if (e - b >= 23) {
                    t.nalLen = (d[b + 21] & 3) + 1;
                    int64_t p = b + 23;
                    for (int a = 0, arrays = d[b + 22]; a < arrays && p + 3 <= e; ++a) {
                        const int type = d[p] & 0x3f;
                        const int n = be16(d + p + 1);
                        p += 3;
                        for (int i = 0; i < n && p + 2 <= e; ++i) {
                            const int len = be16(d + p);
                            if (p + 2 + len > e) break;
                            if (type == 34 && len > 2) {
                                Bits bits{d + p + 4, len - 2};
                                const int64_t id = bits.ue();
                                if (id >= 0) t.pps.insert(int(id));
                            }
                            p += 2 + len;
                        }
                    }
                }
            }
        } else if (f == fcc("apch") || f == fcc("apcn") || f == fcc("apcs") || f == fcc("apco") || f == fcc("ap4h") ||
                   f == fcc("ap4x")) {
            t.codec = Codec::ProRes;
        }
        return;
    }
    // Sound: the QuickTime and ISO layouts share their first fields.
    if (e0 + 36 > eEnd) return;
    const int version = be16(d + e0 + 16);
    t.channels = be16(d + e0 + 24);
    const int bits = be16(d + e0 + 26);
    t.rate = int(be32(d + e0 + 32) >> 16);
    int bytesPerFrame = t.channels * bits / 8;
    if (version == 1 && e0 + 52 <= eEnd) {
        bytesPerFrame = int(be32(d + e0 + 44));
    } else if (version == 2 && e0 + 72 <= eEnd) {
        double rate;
        const uint64_t r = be64(d + e0 + 40);
        std::memcpy(&rate, &r, 8);
        t.rate = rate > 0 && rate < 1e7 ? int(std::lround(rate)) : 0;
        t.channels = int(std::min<uint32_t>(be32(d + e0 + 48), 1024));
        bytesPerFrame = int(be32(d + e0 + 64));
    }
    if (f == fcc("mp4a")) {
        const int64_t c = findType(d, e0 + 28, eEnd, fcc("esds"));
        if (c >= 0) {
            t.extradata = ascFromEsds(d + c + 8, d + c + be32(d + c));
            if (!t.extradata.isEmpty()) t.codec = Codec::Aac;
        }
        return;
    }
    static const uint32_t pcm[] = {fcc("lpcm"), fcc("sowt"), fcc("twos"), fcc("in24"), fcc("in32"), fcc("fl32"),
                                   fcc("fl64"), fcc("ipcm"), fcc("fpcm"), fcc("raw "), fcc("NONE")};
    if (std::find(std::begin(pcm), std::end(pcm), f) == std::end(pcm)) return;
    t.codec = Codec::Pcm;
    if (f == fcc("ipcm") || f == fcc("fpcm")) {
        const int64_t c = findType(d, e0 + 28, eEnd, fcc("pcmC"));
        if (c >= 0 && c + 14 <= eEnd) bytesPerFrame = t.channels * d[c + 13] / 8;
    } else if (f == fcc("in24")) {
        bytesPerFrame = 3 * t.channels;
    } else if (f == fcc("in32") || f == fcc("fl32")) {
        bytesPerFrame = 4 * t.channels;
    } else if (f == fcc("fl64")) {
        bytesPerFrame = 8 * t.channels;
    }
    t.step = bytesPerFrame;
}

}  // namespace

// ---- The damaged media data, walked sample by sample ----

namespace {

// Trial decoding of AAC: the length of a raw frame is the least number of bytes a fresh decoder makes a frame of
// (fewer leave it short of its END element).
class AacProbe {
public:
    ~AacProbe() {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }
    bool open(const RefTrack& a) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(size_t(a.extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(ctx_->extradata, a.extradata.constData(), size_t(a.extradata.size()));
        ctx_->extradata_size = int(a.extradata.size());
        if (a.rate > 0) ctx_->sample_rate = a.rate;
        if (a.channels > 0) av_channel_layout_default(&ctx_->ch_layout, a.channels);
        ctx_->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM | AV_EF_BUFFER;
        ctx_->log_level_offset = AV_LOG_DEBUG;  // its complaints about the bytes tried are expected
        if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return pkt_ && frame_;
    }
    bool decodes(const u8* data, int len) {
        avcodec_flush_buffers(ctx_);
        buf_.assign(size_t(len) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(buf_.data(), data, size_t(len));
        av_packet_unref(pkt_);
        pkt_->data = buf_.data();
        pkt_->size = len;
        if (avcodec_send_packet(ctx_, pkt_) < 0) return false;
        const bool ok = avcodec_receive_frame(ctx_, frame_) == 0 && frame_->nb_samples > 0;
        av_frame_unref(frame_);
        return ok;
    }
    // The frame at `d`, with `avail` bytes there: its length, or 0.
    int frameAt(const u8* d, int64_t avail, int maxLen) {
        int hi = int(std::min<int64_t>(avail, maxLen));
        if (hi < 2 || !decodes(d, hi)) return 0;
        int lo = 1;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (decodes(d, mid))
                hi = mid;
            else
                lo = mid + 1;
        }
        return hi;
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::vector<u8> buf_;
};

struct Item {
    int kind;  // KVideo, KAudio, or something skipped
    int64_t offset, size;
    bool key = false;
};

class Walker {
public:
    Walker(const u8* d, int64_t begin, int64_t end, const Reference& ref) : d_(d), begin_(begin), end_(end), ref_(ref) {
        if (ref.video >= 0) v_ = &ref.tracks[size_t(ref.video)];
        if (ref.audio >= 0) a_ = &ref.tracks[size_t(ref.audio)];
        if (v_) cap_ = int64_t(v_->maxSize) * 3 + (256 << 10);
        if (a_ && a_->codec == Codec::Aac) aacMax_ = int(std::clamp<int64_t>(std::max<int64_t>(a_->maxSize * 2, 768 * std::max(1, a_->channels)), 1024, 16384));
        if (a_ && a_->codec == Codec::Pcm) {
            const int64_t twoSeconds = int64_t(a_->step) * std::max(a_->rate, 8000) * 2;
            pcmScan_ = std::max(a_->maxChunk * 4, twoSeconds);
        }
        zeroFrom_ = end;
        while (zeroFrom_ > begin && d[zeroFrom_ - 1] == 0) --zeroFrom_;
    }
    bool openAac() { return !a_ || a_->codec != Codec::Aac || aac_.open(*a_); }
    void setVideoCap(int64_t cap) { cap_ = cap; }

    // One NAL unit at p: its size with its length prefix, 0 when not plausible. Past the end only when asked.
    int64_t nal(int64_t p, uint32_t* head, int* type, bool pastEnd = false) const {
        const bool hevc = v_->codec == Codec::Hevc;
        const int L = v_->nalLen;
        if (p + L + 2 > end_) return 0;
        uint64_t len = 0;
        for (int i = 0; i < L; ++i) len = len << 8 | d_[p + i];
        const int64_t total = L + int64_t(len);
        if (len < (hevc ? 3u : 2u) || total > cap_) return 0;
        if (!pastEnd && p + total > end_) return 0;
        const u8 b0 = d_[p + L], b1 = d_[p + L + 1];
        if (b0 & 0x80) return 0;
        int t;
        if (hevc) {
            t = b0 >> 1 & 0x3f;
            const int layer = (b0 & 1) << 5 | b1 >> 3, tid1 = b1 & 7;
            if (layer != 0 || tid1 == 0) return 0;
            const bool known = t <= 9 || (t >= 16 && t <= 21) || (t >= 32 && t <= 40) || v_->types.count(t);
            if (!known) return 0;
            if ((t == 32 || t == 33 || (t >= 16 && t <= 21)) && tid1 != 1) return 0;
            *head = uint32_t(b0) << 8 | b1;
        } else {
            t = b0 & 31;
            const int ref = b0 >> 5 & 3;
            const bool known = t == 1 || t == 5 || (t >= 6 && t <= 12) || v_->types.count(t);
            if (!known) return 0;
            if ((t == 5 || t == 7 || t == 8) && ref == 0) return 0;
            if ((t == 6 || t == 9 || t == 10 || t == 11 || t == 12) && ref != 0) return 0;
            *head = b0;
        }
        *type = t;
        return total;
    }
    bool vcl(int type) const {
        return v_->codec == Codec::Hevc ? type < 32 : (type == 1 || type == 5 || type == 20);
    }
    // A slice's header: 1 when it starts a picture, 0 when it continues one, -1 when it is not plausible.
    int sliceStart(int64_t p, int type) const {
        const int L = v_->nalLen;
        if (v_->codec == Codec::Hevc) {
            Bits b{d_ + p + L + 2, std::min<int64_t>(end_ - (p + L + 2), 16)};
            const int first = b.get();
            if (first < 0) return -1;
            if (first) {
                if (type >= 16 && type <= 23) b.get();
                const int64_t pps = b.ue();
                if (pps < 0 || pps > 63 || (!v_->pps.empty() && !v_->pps.count(int(pps)))) return -1;
            }
            return first;
        }
        if (type == 20) return 0;
        Bits b{d_ + p + L + 1, std::min<int64_t>(end_ - (p + L + 1), 16)};
        const int64_t firstMb = b.ue(), sliceType = b.ue(), pps = b.ue();
        if (firstMb < 0 || sliceType < 0 || sliceType > 9 || pps < 0 || pps > 255) return -1;
        if (type == 5 && sliceType % 5 != 2 && sliceType % 5 != 4) return -1;
        if (!v_->pps.empty() && !v_->pps.count(int(pps))) return -1;
        return firstMb == 0 ? 1 : 0;
    }
    bool keyType(int type) const { return v_->codec == Codec::Hevc ? (type >= 16 && type <= 21) : type == 5; }

    // A whole picture (access unit) at p: its size, 0 when there is none. `first` gets its first NAL header.
    int64_t au(int64_t p, bool* key) const {
        if (!v_) return 0;
        if (v_->codec == Codec::ProRes) {
            if (p + 28 > end_ || be32(d_ + p + 4) != fcc("icpf")) return 0;
            const int64_t size = be32(d_ + p);
            const int hs = be16(d_ + p + 8);
            if (size < 28 || size > cap_ || p + size > end_ || hs < 20 || hs > size - 8) return 0;
            *key = true;
            return size;
        }
        const bool hevc = v_->codec == Codec::Hevc;
        int64_t q = p;
        bool picture = false, k = false;
        for (int nals = 0; q < end_ && nals < 1024; ++nals) {
            uint32_t head;
            int type;
            const int64_t n = nal(q, &head, &type);
            if (!n) {
                // A slice of this picture cut off by the end of the file leaves it incomplete.
                if (picture && nal(q, &head, &type, true) && vcl(type) && sliceStart(q, type) == 0) return 0;
                break;
            }
            if (vcl(type)) {
                const int start = sliceStart(q, type);
                if (start < 0) {
                    if (!picture) return 0;
                    break;
                }
                if (!picture) {
                    if (!start) return 0;
                    picture = true;
                } else if (start) {
                    break;
                }
                k = k || keyType(type);
            } else if (picture) {
                // After the picture only filler, suffix SEI and end markers belong to it.
                const bool after = hevc ? (type == 36 || type == 37 || type == 38 || type == 40)
                                        : (type == 10 || type == 11 || type == 12);
                if (!after) break;
            } else {
                const bool aud = hevc ? type == 35 : type == 9;
                if (aud && nals > 0) return 0;
                const bool before = hevc ? (type >= 32 && type <= 39 && type != 36 && type != 37 && type != 38)
                                         : (type == 6 || type == 7 || type == 8 || type == 9 || type == 13 || type == 14 ||
                                            type == 15 || v_->types.count(type));
                if (!before) return 0;
            }
            q += n;
            if (q - p > cap_) return 0;
        }
        if (!picture) return 0;
        *key = k;
        return q - p;
    }
    // A picture starting at q.
    bool strictVideo(int64_t q) const {
        bool key;
        return v_ && q < end_ && au(q, &key);
    }
    // The start of a picture that the end of the file cuts off.
    bool cutVideo(int64_t q) const {
        if (!v_ || q >= end_) return false;
        if (v_->codec == Codec::ProRes)
            return q + 8 <= end_ && be32(d_ + q + 4) == fcc("icpf") && q + int64_t(be32(d_ + q)) > end_ &&
                   int64_t(be32(d_ + q)) <= cap_;
        uint32_t head;
        int type;
        const int64_t n = nal(q, &head, &type, true);
        return n && q + n > end_;
    }
    int aacAt(int64_t q) {
        if (!a_ || a_->codec != Codec::Aac || q >= end_) return 0;
        if (!a_->aacHeads.empty() && !a_->aacHeads.count(d_[q] >> 1)) return 0;
        const auto it = aacCache_.find(q);
        if (it != aacCache_.end()) return it->second;
        const int n = aac_.frameAt(d_ + q, end_ - q, aacMax_);
        if (aacCache_.size() > (1u << 20)) aacCache_.clear();
        aacCache_[q] = n;
        return n;
    }
    // Whether what starts at q looks like the next thing in the file.
    bool followOk(int64_t q, int depth) {
        if (q == end_ || q >= zeroFrom_) return q <= end_;
        if (q > end_) return false;
        if (strictVideo(q) || cutVideo(q) || aacAt(q) > 0) return true;
        if (depth > 0) {
            for (const auto* sizes : {&ref_.otherSizes, &ref_.gapSizes})
                for (int64_t s : *sizes)
                    if (q + s <= end_ && followOk(q + s, depth - 1)) return true;
        }
        return false;
    }
    // The file ends at q, cut short in the middle of something.
    bool endsAt(int64_t q) {
        if (q >= zeroFrom_ || end_ - q < 8) return true;
        if (cutVideo(q)) return true;
        if (a_ && a_->codec == Codec::Aac && end_ - q <= aacMax_ && (a_->aacHeads.empty() || a_->aacHeads.count(d_[q] >> 1)))
            return true;
        if (a_ && a_->codec == Codec::Pcm && end_ - q < a_->step) return true;
        return ref_.maxOther > 0 && end_ - q < ref_.maxOther;
    }

    struct Cand {
        int kind;
        int64_t size;
        bool key = false;
    };
    // The things that could start at p, most likely first: a picture (the strongest test) and then by what follows
    // `prev` in the reference.
    std::vector<Cand> candidates(int64_t p, int prev, bool resync) {
        std::vector<Cand> out;
        bool key = false;
        if (const int64_t n = au(p, &key)) out.push_back({KVideo, n, key});
        std::vector<int> order = {KAudio, KOther, KGap};
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
            const auto cx = ref_.transitions.find({prev, x}), cy = ref_.transitions.find({prev, y});
            return (cx == ref_.transitions.end() ? 0 : cx->second) > (cy == ref_.transitions.end() ? 0 : cy->second);
        });
        // A chunk kind the reference never has after `prev` (when it has enough chunks to say) is not tried, and
        // nothing is tried where a picture starts that the end of the file cuts off.
        int fromPrev = 0;
        for (const auto& [t, c] : ref_.transitions)
            if (t.first == prev) fromPrev += c;
        const bool cut = cutVideo(p);
        for (int k : order) {
            if (cut) break;
            const bool sameChunk = k == prev && k == KAudio && a_ && a_->codec == Codec::Aac;  // frames, not chunks
            if (fromPrev >= 20 && !sameChunk && !ref_.transitions.count({prev, k})) continue;
            if (k == KAudio && a_ && a_->codec == Codec::Aac) {
                const int n = aacAt(p);
                if (n > 0 && followOk(p + n, 1)) out.push_back({KAudio, n});
            } else if (k == KAudio && a_ && a_->codec == Codec::Pcm) {
                const int64_t step = a_->step;
                if (!v_) {
                    const int64_t n = (end_ - p) / step * step;
                    if (n > 0) out.push_back({KAudio, n});
                    continue;
                }
                std::set<int64_t> seen;
                for (int64_t s : a_->chunkSizes)
                    if (p + s <= end_ && followOk(p + s, 1) && seen.insert(s).second) out.push_back({KAudio, s});
                int found = 0;
                for (int64_t s = step; s <= pcmScan_ && p + s <= end_ && found < 4; s += step) {
                    if ((s / step & 4095) == 0 && !poll(p)) break;
                    if (!seen.count(s) && (strictVideo(p + s) || followOk(p + s, 1)) && seen.insert(s).second) {
                        out.push_back({KAudio, s});
                        ++found;
                    }
                }
                // The sound the file ends in, once there have been pictures.
                if (prev != KStart && end_ - p <= pcmScan_) {
                    const int64_t n = (end_ - p) / step * step;
                    if (n > 0 && !seen.count(n)) out.push_back({KAudio, n});
                }
            } else if (k == KOther || k == KGap) {
                for (int64_t s : k == KOther ? ref_.otherSizes : ref_.gapSizes)
                    if (p + s <= end_ && followOk(p + s, 0)) out.push_back({k, s});
            }
        }
        if (resync && out.empty()) {
            // Past damage: the next place two pictures (or a sound frame and what follows it) start.
            for (int64_t q = p + 1; q < end_; ++q) {
                if (((q - p) & 0xffff) == 0 && !poll(q)) break;
                bool k2;
                if (strictVideo(q)) {
                    const int64_t n = au(q, &k2);
                    if (strictVideo(q + n) || followOk(q + n, 1)) {
                        out.push_back({KResync, q - p});
                        break;
                    }
                } else if (!v_ && aacAt(q) > 0 && aacAt(q + aacAt(q)) > 0) {
                    out.push_back({KResync, q - p});
                    break;
                }
            }
        }
        return out;
    }

    // The samples from `start`, by depth-first search with backtracking; false when cancelled or nothing fits.
    bool walk(int64_t start, bool resync, std::vector<Item>& path, const std::function<bool(double)>& progress) {
        struct Frame {
            int64_t pos;
            int prev;
            std::vector<Cand> cands;
            size_t next = 0;
        };
        std::vector<Frame> stack;
        std::unordered_set<int64_t> dead;
        path.clear();
        progress_ = &progress;
        stack.push_back({start, KStart, candidates(start, KStart, resync)});
        int64_t steps = 0;
        while (!stack.empty()) {
            if (cancelled_) return false;
            Frame& f = stack.back();
            if ((++steps & 1023) == 0 && progress && !progress(double(f.pos - begin_) / double(std::max<int64_t>(1, end_ - begin_))))
                return false;
            if (f.next < f.cands.size()) {
                const Cand c = f.cands[f.next++];
                const int64_t q = f.pos + c.size;
                if (dead.count(q)) continue;
                path.push_back({c.kind, f.pos, c.size, c.key});
                const int prev = c.kind == KVideo || c.kind == KAudio || c.kind == KOther || c.kind == KGap ? c.kind : KGap;
                stack.push_back({q, prev, candidates(q, prev, resync)});
                continue;
            }
            if (resync || endsAt(f.pos)) {
                end_ = f.pos;  // what is left is not whole
                return true;
            }
            dead.insert(f.pos);
            stack.pop_back();
            if (!path.empty()) path.pop_back();
        }
        return false;
    }
    int64_t end() const { return end_; }
    bool cancelled() const { return cancelled_; }

private:
    // Asks the caller whether to go on, during a long scan at `at`.
    bool poll(int64_t at) {
        if (!cancelled_ && progress_ && *progress_ && !(*progress_)(double(at - begin_) / double(std::max<int64_t>(1, end_ - begin_))))
            cancelled_ = true;
        return !cancelled_;
    }

    const u8* d_;
    int64_t begin_, end_;
    const Reference& ref_;
    const RefTrack* v_ = nullptr;
    const RefTrack* a_ = nullptr;
    int64_t cap_ = 0, pcmScan_ = 0, zeroFrom_ = 0;
    int aacMax_ = 0;
    AacProbe aac_;
    std::unordered_map<int64_t, int> aacCache_;
    const std::function<bool(double)>* progress_ = nullptr;
    bool cancelled_ = false;
};

// ---- The reference clip ----

bool readReference(const u8* d, int64_t n, Reference& ref, QString* error) {
    const auto top = children(d, 0, n);
    const Box* moov = child(top, fcc("moov"));
    if (!moov) {
        *error = QObject::tr("The reference clip has no index either; choose a clip that plays");
        return false;
    }
    if (const Box* f = child(top, fcc("ftyp"))) ref.ftyp = QByteArray(reinterpret_cast<const char*>(d + f->start), int(f->end - f->start));
    const auto mv = children(d, moov->body, moov->end);
    const Box* mvhd = child(mv, fcc("mvhd"));
    if (!mvhd || mvhd->end - mvhd->body < 20 || (d[mvhd->body] == 1 && mvhd->end - mvhd->body < 28)) {
        *error = QObject::tr("The reference clip's index cannot be read");
        return false;
    }
    ref.mvhd = QByteArray(reinterpret_cast<const char*>(d + mvhd->start), int(mvhd->end - mvhd->start));
    ref.movieTimescale = be32(d + mvhd->body + (d[mvhd->body] == 1 ? 20 : 12));
    if (!ref.movieTimescale) ref.movieTimescale = 600;

    struct ChunkAt {
        int64_t offset, bytes;
        int kind;
    };
    std::vector<ChunkAt> layout;
    std::vector<int64_t> otherChunks;
    QStringList unknown;
    for (const Box& tb : mv) {
        if (tb.type != fcc("trak")) continue;
        const auto tc = children(d, tb.body, tb.end);
        const Box* mdia = child(tc, fcc("mdia"));
        if (!mdia) continue;
        const auto mc = children(d, mdia->body, mdia->end);
        const Box *mdhd = child(mc, fcc("mdhd")), *hdlr = child(mc, fcc("hdlr")), *minf = child(mc, fcc("minf"));
        if (!mdhd || !hdlr || !minf || hdlr->end - hdlr->body < 12 || mdhd->end - mdhd->body < 20 ||
            (d[mdhd->body] == 1 && mdhd->end - mdhd->body < 28) || tb.end - tb.start > (256 << 20))
            continue;
        const uint32_t handler = be32(d + hdlr->body + 8);
        const auto ic = children(d, minf->body, minf->end);
        const Box* stbl = child(ic, fcc("stbl"));
        if (!stbl) continue;
        const auto sc = children(d, stbl->body, stbl->end);
        const Box *stsd = child(sc, fcc("stsd")), *stts = child(sc, fcc("stts")), *stsc = child(sc, fcc("stsc")),
                  *stsz = child(sc, fcc("stsz")), *stco = child(sc, fcc("stco")), *co64 = child(sc, fcc("co64"));
        if (!stsd || !stsc || !stsz || (!stco && !co64)) continue;

        RefTrack t;
        t.video = handler == fcc("vide");
        const bool sound = handler == fcc("soun");
        t.trak = QByteArray(reinterpret_cast<const char*>(d + tb.start), int(tb.end - tb.start));
        t.stsd = QByteArray(reinterpret_cast<const char*>(d + stsd->start), int(stsd->end - stsd->start));
        t.timescale = be32(d + mdhd->body + (d[mdhd->body] == 1 ? 20 : 12));
        t.ctts = child(sc, fcc("ctts")) != nullptr;
        t.stss = child(sc, fcc("stss")) != nullptr;
        if (t.video || sound) readSampleEntry(d, *stsd, t);
        if (sound && t.rate <= 0) t.rate = int(std::min<uint32_t>(t.timescale, 10000000));
        // PCM by the frame: stsz's one size, unless it is QuickTime's 1 (each sample a frame of `step` bytes).
        t.constSize = stsz->end - stsz->body >= 12 ? be32(d + stsz->body + 4) : 0;
        if (t.codec == Codec::Pcm) {
            if (t.constSize > 1) t.step = t.constSize > (1u << 20) ? 0 : int(t.constSize);
            if (t.step <= 0 || t.step > (1 << 20)) t.codec = Codec::Other;
        }
        if (stts && stts->end - stts->body >= 8) {
            std::map<uint32_t, uint64_t> count;
            const uint32_t entries = be32(d + stts->body + 4);
            for (uint32_t i = 0; i < entries && stts->body + 16 + 8 * int64_t(i) <= stts->end; ++i)
                count[be32(d + stts->body + 12 + 8 * i)] += be32(d + stts->body + 8 + 8 * i);
            uint64_t best = 0;
            for (const auto& [delta, c] : count)
                if (c > best) best = c, t.delta = delta;
        }
        if (const Box* edts = child(tc, fcc("edts"))) {
            const auto ec = children(d, edts->body, edts->end);
            if (const Box* elst = child(ec, fcc("elst")); elst && elst->end - elst->body >= 8) {
                const bool v1 = d[elst->body] == 1;
                const uint32_t entries = be32(d + elst->body + 4);
                const int64_t es = v1 ? 20 : 12;
                for (uint32_t i = 0; i < entries && elst->body + 8 + es * (i + 1) <= elst->end; ++i) {
                    const u8* e = d + elst->body + 8 + es * i;
                    const int64_t mt = v1 ? int64_t(be64(e + 8)) : int64_t(int32_t(be32(e + 4)));
                    if (mt >= 0) {
                        t.mediaTime = mt;
                        break;
                    }
                }
            }
        }
        // Sample sizes and chunks, as far as the tables are whole and point into the file.
        const int64_t unit = t.constSize ? (t.codec == Codec::Pcm ? t.step : t.constSize) : 0;  // each sample's bytes; 0 = its own
        std::vector<uint32_t> sizes;
        uint64_t count = stsz->end - stsz->body >= 12 ? be32(d + stsz->body + 8) : 0;
        if (!t.constSize) {
            for (uint64_t i = 0; i < count && stsz->body + 16 + 4 * int64_t(i) <= stsz->end; ++i) {
                sizes.push_back(be32(d + stsz->body + 12 + 4 * i));
                t.maxSize = std::max(t.maxSize, sizes.back());
            }
            count = sizes.size();
        } else {
            count = std::min<uint64_t>(count, uint64_t(n / std::max<int64_t>(1, unit)));
            t.maxSize = uint32_t(std::min<int64_t>(unit, 0xffffffffLL));
        }
        auto entries = [&](const Box* b) { return b->end - b->body >= 8 ? be32(d + b->body + 4) : 0u; };
        std::vector<int64_t> offsets;  // -1 where a chunk points outside the file
        auto offset = [&](uint64_t o) { return o < uint64_t(n) ? int64_t(o) : int64_t(-1); };
        if (co64) {
            for (uint32_t i = 0, e = entries(co64); i < e && co64->body + 16 + 8 * int64_t(i) <= co64->end; ++i)
                offsets.push_back(offset(be64(d + co64->body + 8 + 8 * i)));
        } else {
            for (uint32_t i = 0, e = entries(stco); i < e && stco->body + 12 + 4 * int64_t(i) <= stco->end; ++i)
                offsets.push_back(offset(be32(d + stco->body + 8 + 4 * i)));
        }
        std::vector<std::pair<uint32_t, uint32_t>> runs;  // first chunk (from 1), samples per chunk
        for (uint32_t i = 0, e = entries(stsc); i < e && stsc->body + 20 + 12 * int64_t(i) <= stsc->end; ++i)
            runs.push_back({be32(d + stsc->body + 8 + 12 * i), be32(d + stsc->body + 12 + 12 * i)});
        const bool keep = (t.video && t.codec != Codec::Other && ref.video < 0) ||
                          (sound && t.codec != Codec::Other && ref.audio < 0);
        if ((t.video || sound) && !keep && t.codec != Codec::Other) ++ref.dropped;
        const int kind = keep ? (t.video ? KVideo : KAudio) : KOther;
        std::vector<int64_t> samples;  // offsets of the first few hundred samples
        std::vector<uint32_t> sampleSizes;
        uint64_t s = 0;
        std::vector<int64_t> chunkBytes;
        for (size_t c = 0, r = 0; c < offsets.size() && s < count; ++c) {
            while (r + 1 < runs.size() && runs[r + 1].first <= c + 1) ++r;
            const uint64_t spc = std::min<uint64_t>(runs.empty() ? 1 : runs[r].second, count - s);
            int64_t bytes = 0;
            if (unit) {
                bytes = int64_t(spc) * unit;
                for (uint64_t i = 0; i < spc && samples.size() < 2000 && offsets[c] >= 0; ++i)
                    samples.push_back(offsets[c] + int64_t(i) * unit), sampleSizes.push_back(uint32_t(unit));
            } else {
                for (uint64_t i = 0; i < spc; ++i) {
                    if (samples.size() < 2000 && offsets[c] >= 0 && offsets[c] + bytes < n)
                        samples.push_back(offsets[c] + bytes), sampleSizes.push_back(sizes[s + i]);
                    bytes += sizes[s + i];
                }
            }
            s += spc;
            if (offsets[c] < 0 || offsets[c] + bytes > n) continue;
            layout.push_back({offsets[c], bytes, kind});
            chunkBytes.push_back(bytes);
            if (kind == KOther) otherChunks.push_back(bytes);
        }
        if (keep && t.codec == Codec::Pcm) {
            t.chunkSizes = byFrequency(chunkBytes, 8);
            for (int64_t b : chunkBytes) t.maxChunk = std::max(t.maxChunk, b);
        }
        if (keep && t.codec == Codec::Aac)
            for (size_t i = 0; i < samples.size(); ++i)
                if (samples[i] < n && sampleSizes[i] > 0) t.aacHeads.insert(d[samples[i]] >> 1);
        if (keep && (t.codec == Codec::H264 || t.codec == Codec::Hevc)) {
            const bool hevc = t.codec == Codec::Hevc;
            for (size_t i = 0; i < samples.size(); ++i) {
                const int64_t b = samples[i], e = std::min<int64_t>(n, b + sampleSizes[i]);
                for (int64_t p = b; p + t.nalLen + 2 <= e;) {
                    uint64_t len = 0;
                    for (int k = 0; k < t.nalLen; ++k) len = len << 8 | d[p + k];
                    const u8 b0 = d[p + t.nalLen];
                    t.types.insert(hevc ? b0 >> 1 & 0x3f : b0 & 31);
                    if (!len) break;
                    p += t.nalLen + int64_t(len);
                }
            }
        }
        if ((t.video || sound) && t.codec == Codec::Other && t.entry) unknown << fourccText(t.entry);
        if (keep) {
            (t.video ? ref.video : ref.audio) = int(ref.tracks.size());
            ref.tracks.push_back(t);
        }
    }
    if (ref.video < 0 && ref.audio < 0) {
        *error = unknown.isEmpty() ? QObject::tr("The reference clip has no H.264, HEVC, ProRes, AAC or PCM track to rebuild from")
                                   : QObject::tr("Montage cannot rebuild %1 media yet (H.264, HEVC, ProRes, AAC and PCM)").arg(unknown.join(", "));
        return false;
    }
    // The order of chunks in the file, and what lies between them.
    std::sort(layout.begin(), layout.end(), [](const ChunkAt& a, const ChunkAt& b) { return a.offset < b.offset; });
    std::vector<int64_t> gaps;
    int prev = KStart;
    int64_t at = -1;
    if (const Box* mdat = child(top, fcc("mdat"))) at = mdat->body;
    for (const ChunkAt& c : layout) {
        if (at >= 0 && c.offset > at) {
            gaps.push_back(c.offset - at);
            ++ref.transitions[{prev, KGap}];
            prev = KGap;
        }
        ++ref.transitions[{prev, c.kind}];
        prev = c.kind;
        at = c.offset + c.bytes;
    }
    ref.otherSizes = byFrequency(otherChunks, 8);
    ref.gapSizes = byFrequency(gaps, 8);
    for (int64_t s : otherChunks) ref.maxOther = std::max(ref.maxOther, s);
    for (int64_t s : gaps) ref.maxOther = std::max(ref.maxOther, s);
    return true;
}

// ---- The new index ----

// An mvhd, tkhd or mdhd made and changed at `when` (seconds since 1904).
QByteArray withTimes(const QByteArray& b, uint64_t when) {
    QByteArray out = b;
    const int h = be32(reinterpret_cast<const u8*>(b.constData())) == 1 ? 16 : 8;
    if (out.size() < h + 4) return out;
    const bool v1 = out[h] == 1;
    if (out.size() < h + (v1 ? 20 : 12)) return out;
    if (v1)
        set64(out, h + 4, when), set64(out, h + 12, when);
    else
        set32(out, h + 4, uint32_t(when)), set32(out, h + 8, uint32_t(when));
    return out;
}

QByteArray withDuration(const QByteArray& b, uint32_t type, uint64_t duration) {
    QByteArray out = b;
    const int h = be32(reinterpret_cast<const u8*>(b.constData())) == 1 ? 16 : 8;
    if (out.size() < h + 4) return out;
    const bool v1 = out[h] == 1;
    const int at = type == fcc("tkhd") ? (v1 ? h + 28 : h + 20) : (v1 ? h + 24 : h + 16);
    if (out.size() < at + (v1 ? 8 : 4)) return out;
    if (v1)
        set64(out, at, duration);
    else
        set32(out, at, uint32_t(std::min<uint64_t>(duration, 0xffffffffu)));
    return out;
}

struct TrackOut {
    const RefTrack* ref;
    QByteArray stbl;
    uint64_t mediaDuration = 0;  // in the track's timescale
    int64_t mediaTime = -1;      // the edit's start; -1 = no edit list
    uint64_t presented = 0;      // the edit's length, in the track's timescale
};

QByteArray tableStbl(const RefTrack& t, const std::vector<Item>& items, int kind, int64_t shift,
                     const std::vector<int>& ranks, int64_t delay, uint64_t* samplesOut) {
    // Chunks: runs of this track's items with nothing between them.
    std::vector<std::pair<int64_t, uint32_t>> chunks;  // offset, samples
    std::vector<uint32_t> sizes;
    std::vector<uint32_t> keys;
    uint64_t samples = 0;
    int64_t lastEnd = -1;
    bool keyTables = t.stss;
    for (const Item& it : items) {
        if (it.kind != kind) {
            lastEnd = -1;
            continue;
        }
        uint32_t n = 1;
        if (t.codec == Codec::Pcm) n = uint32_t(it.size / std::max(1, t.step));  // frames
        if (it.offset == lastEnd && !chunks.empty())
            chunks.back().second += n;
        else
            chunks.push_back({it.offset + shift, n});
        if (t.codec != Codec::Pcm) {
            sizes.push_back(uint32_t(it.size));
            if (it.key) keys.push_back(uint32_t(samples + 1));
        }
        samples += n;
        lastEnd = it.offset + it.size;
    }
    *samplesOut = samples;
    QByteArray b;
    // stsd as the reference has it.
    b += t.stsd;
    QByteArray p;
    put32(p, 1);
    put32(p, uint32_t(samples));
    put32(p, t.delta);
    b += fullBox(fcc("stts"), 0, p);
    if (!ranks.empty()) {
        QByteArray c;
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (size_t i = 0; i < ranks.size(); ++i) {
            const uint32_t off = uint32_t((int64_t(ranks[i]) + delay - int64_t(i)) * t.delta);
            if (!runs.empty() && runs.back().second == off)
                ++runs.back().first;
            else
                runs.push_back({1, off});
        }
        put32(c, uint32_t(runs.size()));
        for (const auto& r : runs) put32(c, r.first), put32(c, r.second);
        b += fullBox(fcc("ctts"), 0, c);
    }
    if (keyTables && t.video) {
        QByteArray s;
        put32(s, uint32_t(keys.size()));
        for (uint32_t k : keys) put32(s, k);
        b += fullBox(fcc("stss"), 0, s);
    }
    QByteArray sc;
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (size_t i = 0; i < chunks.size(); ++i)
        if (runs.empty() || runs.back().second != chunks[i].second) runs.push_back({uint32_t(i + 1), chunks[i].second});
    put32(sc, uint32_t(runs.size()));
    for (const auto& r : runs) put32(sc, r.first), put32(sc, r.second), put32(sc, 1);
    b += fullBox(fcc("stsc"), 0, sc);
    QByteArray sz;
    if (t.codec == Codec::Pcm) {
        put32(sz, t.constSize ? t.constSize : uint32_t(t.step));
        put32(sz, uint32_t(samples));
    } else {
        put32(sz, 0);
        put32(sz, uint32_t(sizes.size()));
        for (uint32_t s : sizes) put32(sz, s);
    }
    b += fullBox(fcc("stsz"), 0, sz);
    const bool wide = !chunks.empty() && std::any_of(chunks.begin(), chunks.end(), [](const auto& c) { return c.first > 0xffffffffLL; });
    QByteArray co;
    put32(co, uint32_t(chunks.size()));
    for (const auto& c : chunks) wide ? put64(co, uint64_t(c.first)) : put32(co, uint32_t(c.first));
    b += fullBox(wide ? fcc("co64") : fcc("stco"), 0, co);
    return box(fcc("stbl"), b);
}

QByteArray rebuildTrak(const TrackOut& o, uint32_t movieTimescale, uint64_t when) {
    const u8* d = reinterpret_cast<const u8*>(o.ref->trak.constData());
    const int64_t n = o.ref->trak.size();
    const auto tc = children(d, be32(d) == 1 ? 16 : 8, n);
    auto raw = [&](const Box& b) { return QByteArray(reinterpret_cast<const char*>(d + b.start), int(b.end - b.start)); };
    const uint64_t movieDuration = o.ref->timescale ? o.presented * movieTimescale / o.ref->timescale : 0;
    QByteArray out;
    for (const Box& b : tc) {
        if (b.type == fcc("tkhd")) {
            out += withDuration(withTimes(raw(b), when), fcc("tkhd"), movieDuration);
            if (o.mediaTime >= 0) {
                QByteArray e;
                put32(e, 1);
                const bool v1 = movieDuration > 0xffffffffu || o.mediaTime > 0x7fffffff;
                if (v1)
                    put64(e, movieDuration), put64(e, uint64_t(o.mediaTime));
                else
                    put32(e, uint32_t(movieDuration)), put32(e, uint32_t(o.mediaTime));
                put32(e, 0x00010000);
                out += box(fcc("edts"), fullBox(fcc("elst"), v1 ? 1 : 0, e));
            }
        } else if (b.type == fcc("edts") || b.type == fcc("tref")) {
            continue;
        } else if (b.type == fcc("mdia")) {
            QByteArray m;
            for (const Box& c : children(d, b.body, b.end)) {
                if (c.type == fcc("mdhd")) {
                    m += withDuration(withTimes(raw(c), when), fcc("mdhd"), o.mediaDuration);
                } else if (c.type == fcc("minf")) {
                    QByteArray i;
                    for (const Box& x : children(d, c.body, c.end)) i += x.type == fcc("stbl") ? o.stbl : raw(x);
                    m += box(fcc("minf"), i);
                } else {
                    m += raw(c);
                }
            }
            out += box(fcc("mdia"), m);
        } else {
            out += raw(b);
        }
    }
    return box(fcc("trak"), out);
}

// The presentation order of the pictures (decode order in, rank out); empty when the decoder does not give it.
std::vector<int> presentationRanks(const RefTrack& v, const u8* d, const std::vector<Item>& items,
                                   const std::function<bool(double)>& progress, bool* cancelled) {
    std::vector<int> ranks;
    const AVCodec* codec = avcodec_find_decoder(v.codec == Codec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) return ranks;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->extradata = static_cast<uint8_t*>(av_mallocz(size_t(v.extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(ctx->extradata, v.extradata.constData(), size_t(v.extradata.size()));
    ctx->extradata_size = int(v.extradata.size());
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->skip_idct = AVDISCARD_ALL;
    ctx->log_level_offset = AV_LOG_DEBUG;
    ctx->thread_count = 0;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<int64_t> order;
    std::vector<const Item*> pictures;
    for (const Item& it : items)
        if (it.kind == KVideo) pictures.push_back(&it);
    if (avcodec_open2(ctx, codec, nullptr) >= 0) {
        auto drain = [&] {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                order.push_back(frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp);
                av_frame_unref(frame);
            }
        };
        for (size_t i = 0; i < pictures.size(); ++i) {
            if (progress && (i & 255) == 0 && !progress(double(i) / double(pictures.size()))) {
                *cancelled = true;
                break;
            }
            if (av_new_packet(pkt, int(pictures[i]->size)) < 0) break;
            std::memcpy(pkt->data, d + pictures[i]->offset, size_t(pictures[i]->size));
            pkt->pts = pkt->dts = int64_t(i);
            if (pictures[i]->key) pkt->flags |= AV_PKT_FLAG_KEY;
            const int r = avcodec_send_packet(ctx, pkt);
            av_packet_unref(pkt);
            if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA) break;
            drain();
        }
        avcodec_send_packet(ctx, nullptr);
        drain();
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (order.size() != pictures.size()) return ranks;
    ranks.assign(pictures.size(), -1);
    for (size_t k = 0; k < order.size(); ++k) {
        if (order[k] < 0 || order[k] >= int64_t(ranks.size()) || ranks[size_t(order[k])] >= 0) return {};
        ranks[size_t(order[k])] = int(k);
    }
    return ranks;
}

class Mapped {
public:
    explicit Mapped(const QString& path) : file_(path) {
        if (file_.open(QIODevice::ReadOnly) && file_.size() > 0) data_ = file_.map(0, file_.size());
    }
    const u8* data() const { return data_; }
    int64_t size() const { return file_.size(); }

private:
    QFile file_;
    u8* data_ = nullptr;
};

bool writeRange(QSaveFile& out, const u8* d, int64_t b, int64_t e, const std::function<bool(double)>& progress,
                double from, double to) {
    const int64_t piece = 16 << 20;
    for (int64_t p = b; p < e; p += piece) {
        const int64_t n = std::min(piece, e - p);
        if (out.write(reinterpret_cast<const char*>(d + p), n) != n) return false;
        if (progress && !progress(from + (to - from) * double(p + n - b) / double(std::max<int64_t>(1, e - b)))) return false;
    }
    return true;
}

bool repairMovie(const QString& damaged, const QString& reference, const QString& output, RepairReport& report,
                 QString* error, const std::function<bool(double)>& progress) {
    Mapped rf(reference), df(damaged);
    if (!rf.data()) return *error = QObject::tr("The reference clip cannot be read"), false;
    if (!df.data()) return *error = QObject::tr("The file cannot be read"), false;
    Reference ref;
    if (!readReference(rf.data(), rf.size(), ref, error)) return false;
    const u8* d = df.data();
    const int64_t n = df.size();

    // The boxes before the media data, and where the data runs.
    std::vector<Box> before;
    int64_t begin = -1, end = n;
    for (int64_t p = 0; p + 8 <= n;) {
        uint64_t size = be32(d + p);
        int64_t hdr = 8;
        const uint32_t type = be32(d + p + 4);
        if (size == 1 && p + 16 <= n) size = be64(d + p + 8), hdr = 16;
        if (type == fcc("mdat")) {
            begin = p + hdr;
            // Its size, when the camera wrote one and a box follows it (else the data runs on to the end).
            if (size >= uint64_t(hdr) && size < uint64_t(n - p) - 8) {
                const int64_t q = p + int64_t(size);
                const uint32_t next = be32(d + q + 4);
                bool named = true;
                for (int i = 0; i < 4; ++i) named = named && std::isprint(int(next >> (8 * i) & 0xff));
                if (named) end = q;  // (perhaps a moov the file was cut in)
            } else if (size == uint64_t(n - p)) {
                end = n;
            }
            break;
        }
        if (size < uint64_t(hdr) || size > uint64_t(n - p)) break;
        before.push_back({type, p, p + hdr, p + int64_t(size)});
        p += int64_t(size);
    }
    if (begin < 0) {
        const int64_t at = findType(d, 0, std::min<int64_t>(n, 64 << 20), fcc("mdat"));
        if (at < 0) return *error = QObject::tr("No media data was found in the file"), false;
        begin = at + 8;
    }

    Walker w(d, begin, end, ref);
    if (!w.openAac()) return *error = QObject::tr("The AAC decoder cannot be opened"), false;
    const RefTrack* v = ref.video >= 0 ? &ref.tracks[size_t(ref.video)] : nullptr;
    const RefTrack* a = ref.audio >= 0 ? &ref.tracks[size_t(ref.audio)] : nullptr;
    if (v && v->codec != Codec::ProRes && v->types.empty())
        return *error = QObject::tr("The reference clip's pictures cannot be read"), false;

    auto walkProgress = [&](double f) { return !progress || progress(0.7 * f); };
    std::vector<Item> items;
    bool resynced = false;
    if (!w.walk(begin, false, items, walkProgress)) {
        if (w.cancelled() || (progress && !progress(0))) return *error = QObject::tr("Cancelled"), false;
        Walker w2(d, begin, end, ref);
        if (!w2.openAac() || !w2.walk(begin, true, items, walkProgress))
            return *error = w2.cancelled() || (progress && !progress(0)) ? QObject::tr("Cancelled")
                                                                          : QObject::tr("No samples matching the reference clip were found"),
                   false;
        resynced = true;
    }
    int videoCount = 0, audioItems = 0;
    int64_t lastEnd = begin, skipped = 0;
    for (const Item& it : items) {
        videoCount += it.kind == KVideo;
        audioItems += it.kind == KAudio;
        if (it.kind == KResync) skipped += it.size;
        lastEnd = std::max(lastEnd, it.offset + it.size);
    }
    // A reference with pictures that finds none is from another camera or other settings (its sound alone could match
    // by chance).
    if ((!videoCount && !audioItems) || (v && !videoCount))
        return *error = QObject::tr("No pictures matching the reference clip were found: choose a clip from the same camera, recorded "
                                    "with the same settings"),
               false;

    // Composition offsets from the decoder's picture order.
    std::vector<int> ranks;
    int64_t delay = 0;
    if (v && v->ctts && (v->codec == Codec::H264 || v->codec == Codec::Hevc)) {
        bool cancelled = false;
        ranks = presentationRanks(*v, d, items, [&](double f) { return !progress || progress(0.7 + 0.2 * f); }, &cancelled);
        if (cancelled) return *error = QObject::tr("Cancelled"), false;
        if (ranks.empty())
            report.notes << QObject::tr("The pictures' display order could not be found; they play in decode order");
        for (size_t i = 0; i < ranks.size(); ++i) delay = std::max<int64_t>(delay, int64_t(i) - ranks[i]);
    }

    // The new file: the boxes before the media data, the media data as far as it is whole, and the new index.
    const int64_t dataLen = lastEnd - begin;
    QByteArray head;
    bool haveFtyp = false;
    for (const Box& b : before) {
        if (b.type == fcc("moov")) continue;
        haveFtyp = haveFtyp || b.type == fcc("ftyp");
        head += QByteArray(reinterpret_cast<const char*>(d + b.start), int(b.end - b.start));
    }
    if (!haveFtyp) head.prepend(ref.ftyp);
    const bool large = dataLen + 8 > 0xffffffffLL;
    if (large) {
        put32(head, 1);
        put32(head, fcc("mdat"));
        put64(head, uint64_t(dataLen + 16));
    } else {
        put32(head, uint32_t(dataLen + 8));
        put32(head, fcc("mdat"));
    }
    const int64_t shift = head.size() - begin;

    std::vector<TrackOut> outs;
    uint64_t movieDuration = 0;
    for (size_t i = 0; i < ref.tracks.size(); ++i) {
        const RefTrack& t = ref.tracks[i];
        const int kind = int(i) == ref.video ? KVideo : KAudio;
        TrackOut o{&t, {}, 0, -1, 0};
        uint64_t samples = 0;
        o.stbl = tableStbl(t, items, kind, shift, kind == KVideo ? ranks : std::vector<int>{}, delay, &samples);
        if (!samples) continue;
        o.mediaDuration = samples * t.delta;
        if (kind == KVideo) {
            report.videoFrames = int(samples);
            o.presented = o.mediaDuration;
            if (delay > 0) o.mediaTime = delay * t.delta;
        } else {
            report.audioSamples = t.codec == Codec::Aac ? int64_t(samples) * t.delta : int64_t(samples * t.delta);
            if (t.timescale && t.rate > 0 && int(t.timescale) != t.rate)
                report.audioSamples = int64_t(double(report.audioSamples) * t.rate / t.timescale);
            const int64_t priming = std::max<int64_t>(0, t.mediaTime);
            o.presented = o.mediaDuration > uint64_t(priming) ? o.mediaDuration - uint64_t(priming) : 0;
            if (t.mediaTime > 0) o.mediaTime = t.mediaTime;
        }
        if (t.timescale) movieDuration = std::max(movieDuration, o.presented * ref.movieTimescale / t.timescale);
        outs.push_back(std::move(o));
    }
    // Made when the damaged file's recording started (its last change, less its length), not when the reference was;
    // the reference's user data (its date, place and the like) is left out.
    const qint64 changed = QFileInfo(damaged).lastModified().toSecsSinceEpoch();
    const uint64_t when = uint64_t(std::max<qint64>(0, changed - qint64(movieDuration / ref.movieTimescale) + 2082844800));
    QByteArray moov = withDuration(withTimes(ref.mvhd, when), fcc("mvhd"), movieDuration);
    for (const TrackOut& o : outs) moov += rebuildTrak(o, ref.movieTimescale, when);
    moov = box(fcc("moov"), moov);

    QSaveFile out(output);
    if (!out.open(QIODevice::WriteOnly)) return *error = QObject::tr("%1 cannot be written").arg(output), false;
    if (out.write(head) != head.size() || !writeRange(out, d, begin, lastEnd, progress, 0.9, 0.99) ||
        out.write(moov) != moov.size()) {
        out.cancelWriting();
        return *error = progress && !progress(0.99) ? QObject::tr("Cancelled") : QObject::tr("%1 cannot be written").arg(output), false;
    }
    if (!out.commit()) return *error = QObject::tr("%1 cannot be written").arg(output), false;

    report.kind = RepairKind::Movie;
    report.bytesUsed = dataLen;
    report.bytesLeft = end - lastEnd;
    report.seconds = double(movieDuration) / ref.movieTimescale;
    if (v && v->delta) report.timecode = sidecarTimecode(damaged, double(v->timescale) / v->delta);
    if (resynced)
        report.notes << QObject::tr("%1 damaged bytes in the middle were skipped").arg(skipped);
    if (a && !report.audioSamples) report.notes << QObject::tr("No sound was found, only pictures");
    if (ref.dropped)
        report.notes << QObject::tr("The reference clip has %n more picture or sound track(s); only the first of each is rebuilt",
                                    nullptr, ref.dropped);
    return true;
}


// ---- WAV, BWF and RF64 ----

struct WavLayout {
    bool rf64 = false;
    int64_t ds64 = -1;       // the ds64 (or a JUNK placeholder for it) chunk's start
    int64_t shortDs64 = -1;  // a ds64 too short for its sizes, which becomes JUNK
    int64_t dataHeader = -1;  // the data chunk's header
    int64_t dataSize = -1;    // as it should be
    int64_t after = -1;       // the chunks after the data that are whole run from here to the end
    int blockAlign = 1;
    bool riffWrong = false, dataWrong = false;
};

// Where a WAV's chunks are and which sizes are wrong.
bool readWav(const u8* d, int64_t n, WavLayout& w, QString* error) {
    if (n < 12 || (std::memcmp(d, "RIFF", 4) && std::memcmp(d, "RF64", 4) && std::memcmp(d, "BW64", 4)) ||
        std::memcmp(d + 8, "WAVE", 4))
        return *error = QObject::tr("This is not a WAV file"), false;
    w.rf64 = std::memcmp(d, "RIFF", 4) != 0;
    uint64_t riff = le32(d + 4), ds64Data = 0;
    int64_t p = 12;
    while (p + 8 <= n) {
        const u8* c = d + p;
        uint64_t size = le32(c + 4);
        if ((!std::memcmp(c, "ds64", 4) || (!std::memcmp(c, "JUNK", 4) && p == 12)) && size >= 28 && w.ds64 < 0) {
            w.ds64 = p;
            if (!std::memcmp(c, "ds64", 4) && p + 8 + 16 <= n) riff = le64(c + 8), ds64Data = le64(c + 16);
        } else if (!std::memcmp(c, "ds64", 4)) {
            w.shortDs64 = p;
            if (size >= 16 && p + 8 + 16 <= n) riff = le64(c + 8), ds64Data = le64(c + 16);
        }
        if (!std::memcmp(c, "fmt ", 4) && p + 8 + 16 <= n) w.blockAlign = std::max<int>(1, c[8 + 12] | c[8 + 13] << 8);
        if (!std::memcmp(c, "data", 4)) {
            w.dataHeader = p;
            if (w.rf64 && size == 0xffffffffu) size = ds64Data;
            const int64_t start = p + 8, avail = n - start;
            // Whole when it fits and what follows it is whole chunks to the end.
            bool whole = size > 0 && size != 0xffffffffu && int64_t(size) <= avail;
            if (whole) {
                // (A few stray bytes at the very end are allowed.)
                int64_t q = start + int64_t(size) + int64_t(size & 1);
                while (whole && q + 8 <= n) {
                    const uint64_t s = le32(d + q + 4);
                    whole = std::isprint(d[q]) && q + 8 + int64_t(s) <= n;
                    q += 8 + int64_t(s) + int64_t(s & 1);
                }
                whole = whole && q <= n + 1;
            }
            if (whole) {
                w.dataSize = int64_t(size);
                w.after = start + int64_t(size) + int64_t(size & 1);
            } else {
                w.dataSize = avail / w.blockAlign * w.blockAlign;
                w.after = n;
                w.dataWrong = true;
            }
            break;
        }
        if (size > uint64_t(n - p - 8)) break;
        p += 8 + int64_t(size) + int64_t(size & 1);
        // Some writers leave out the pad byte after a chunk of odd size.
        auto id = [&](int64_t q) { return q + 4 <= n && std::isalnum(d[q]) && std::isprint(d[q + 1]) && std::isprint(d[q + 2]) && std::isprint(d[q + 3]); };
        if ((size & 1) && !id(p) && id(p - 1)) --p;
    }
    if (w.dataHeader < 0) return *error = QObject::tr("The WAV file has no sound data"), false;
    w.riffWrong = int64_t(riff) + 8 != n;
    return true;
}

bool repairWav(const QString& damaged, const QString& output, RepairReport& report, QString* error,
               const std::function<bool(double)>& progress) {
    Mapped f(damaged);
    if (!f.data()) return *error = QObject::tr("The file cannot be read"), false;
    const u8* d = f.data();
    WavLayout w;
    if (!readWav(d, f.size(), w, error)) return false;
    // Everything before the data as it is, then the data at its true size, then the whole chunks after it.
    const int64_t dataStart = w.dataHeader + 8;
    if (w.dataHeader > (64 << 20)) return *error = QObject::tr("The WAV file's chunks before its sound are too large"), false;
    QByteArray head(reinterpret_cast<const char*>(d), int(w.dataHeader));
    if (w.shortDs64 >= 0) std::memcpy(head.data() + w.shortDs64, "JUNK", 4);
    if (f.size() - w.after > (64 << 20)) return *error = QObject::tr("The WAV file's chunks after its sound are too large"), false;
    const QByteArray tail = w.after < f.size() ? QByteArray(reinterpret_cast<const char*>(d + w.after), int(f.size() - w.after)) : QByteArray();
    const QByteArray pad = (w.dataSize & 1) ? QByteArray(1, '\0') : QByteArray();
    auto le32put = [](QByteArray& b, int at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (8 * i));
    };
    auto le64put = [&](QByteArray& b, int at, uint64_t v) { le32put(b, at, uint32_t(v)), le32put(b, at + 4, uint32_t(v >> 32)); };
    int64_t total = head.size() + 8 + w.dataSize + pad.size() + tail.size();
    const bool big = total - 8 > 0xffffffffLL || w.dataSize > 0xffffffffLL;
    if (big && w.ds64 < 0) {
        // No placeholder: a ds64 goes in straight after WAVE.
        QByteArray ds("ds64\x1c\0\0\0", 8);
        ds += QByteArray(28, '\0');
        head.insert(12, ds);
        w.ds64 = 12;
        total += ds.size();
    }
    if (big) {
        std::memcpy(head.data(), "RF64", 4);
        le32put(head, 4, 0xffffffffu);
        std::memcpy(head.data() + w.ds64, "ds64", 4);
        le64put(head, int(w.ds64 + 8), uint64_t(total - 8));
        le64put(head, int(w.ds64 + 16), uint64_t(w.dataSize));
        le64put(head, int(w.ds64 + 24), uint64_t(w.dataSize / w.blockAlign));
        le32put(head, int(w.ds64 + 32), 0);
    } else {
        std::memcpy(head.data(), "RIFF", 4);
        le32put(head, 4, uint32_t(total - 8));
        if (w.ds64 >= 0) std::memcpy(head.data() + w.ds64, "JUNK", 4);
    }
    QByteArray dh("data", 4);
    put32(dh, 0);
    le32put(dh, 4, big ? 0xffffffffu : uint32_t(w.dataSize));
    QSaveFile out(output);
    if (!out.open(QIODevice::WriteOnly)) return *error = QObject::tr("%1 cannot be written").arg(output), false;
    if (out.write(head) != head.size() || out.write(dh) != dh.size() ||
        !writeRange(out, d, dataStart, dataStart + w.dataSize, progress, 0, 0.99) || out.write(pad) != pad.size() ||
        out.write(tail) != tail.size()) {
        out.cancelWriting();
        return *error = progress && !progress(0.99) ? QObject::tr("Cancelled") : QObject::tr("%1 cannot be written").arg(output), false;
    }
    if (!out.commit()) return *error = QObject::tr("%1 cannot be written").arg(output), false;
    report.kind = RepairKind::Wav;
    report.bytesUsed = w.dataSize;
    report.bytesLeft = w.dataWrong ? f.size() - dataStart - w.dataSize : 0;
    // The rate, for the length.
    int rate = 0, frame = w.blockAlign;
    for (int64_t p = 12; p + 16 <= w.dataHeader;) {
        if (!std::memcmp(d + p, "fmt ", 4)) rate = int(le32(d + p + 12));
        const uint32_t s = le32(d + p + 4);
        p += 8 + int64_t(s) + (s & 1);
    }
    report.audioSamples = w.dataSize / std::max(1, frame);
    if (rate > 0) report.seconds = double(report.audioSamples) / rate;
    if (big) report.notes << QObject::tr("Written as RF64 (past 4 GB)");
    return true;
}

// ---- MXF ----

const u8 kPartitionKey[13] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0d, 0x01, 0x02, 0x01, 0x01};
const u8 kIndexKey[16] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53, 0x01, 0x01, 0x0d, 0x01, 0x02, 0x01, 0x01, 0x10, 0x01, 0x00};
const u8 kFillKey[16] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00};

bool isPartition(const u8* k) { return !std::memcmp(k, kPartitionKey, 13) && k[13] >= 2 && k[13] <= 4 && k[15] == 0; }
bool isRip(const u8* k) { return !std::memcmp(k, kPartitionKey, 13) && k[13] == 0x11 && k[14] == 0x01; }
bool isIndexSegment(const u8* k) { return !std::memcmp(k, kIndexKey, 13) && k[13] == 0x10 && k[14] == 0x01; }
bool isEssence(const u8* k) {
    return be32(k) == 0x060e2b34 && k[4] == 0x01 && k[5] == 0x02 && k[6] == 0x01 && be32(k + 8) == 0x0d010301;
}
// A generic container's system item (SMPTE 385, 394), which starts its content package when there is one.
bool isSystemItem(const u8* k) {
    return be32(k) == 0x060e2b34 && k[4] == 0x02 && (k[5] == 0x05 || k[5] == 0x53) && be32(k + 8) == 0x0d010301 &&
           (k[12] == 0x04 || k[12] == 0x14);
}
bool isMetadataSet(const u8* k) { return be32(k) == 0x060e2b34 && k[4] == 0x02 && k[5] == 0x53 && be32(k + 8) == 0x0d010101; }

struct Klv {
    int64_t at = 0, value = 0, end = 0;
};
// The KLV at p; false when it does not fit in the file.
bool klvAt(const u8* d, int64_t n, int64_t p, Klv& k) {
    if (p + 17 > n) return false;
    uint64_t len = d[p + 16];
    int64_t v = p + 17;
    if (len & 0x80) {
        const int bytes = int(len & 0x7f);
        if (bytes == 0 || bytes > 8 || v + bytes > n) return false;
        len = 0;
        for (int i = 0; i < bytes; ++i) len = len << 8 | d[v + i];
        v += bytes;
    }
    if (len > uint64_t(n - v)) return false;
    k = {p, v, v + int64_t(len)};
    return true;
}
// The local tags of a set: tag -> where its value starts and its length.
std::vector<std::tuple<int, int64_t, int>> localTags(const u8* d, const Klv& k) {
    std::vector<std::tuple<int, int64_t, int>> out;
    for (int64_t p = k.value; p + 4 <= k.end;) {
        const int tag = be16(d + p), len = be16(d + p + 2);
        if (p + 4 + len > k.end) break;
        out.push_back({tag, p + 4, len});
        p += 4 + len;
    }
    return out;
}

struct MxfPartition {
    int64_t at = 0, packEnd = 0, essence = -1;  // essence: where its essence container bytes start
    QByteArray pack;                            // the whole partition pack
    int kind = 0;                               // 2 header, 3 body, 4 footer
    uint32_t kag = 1, indexSid = 0, bodySid = 0;
    uint64_t headerBytes = 0, indexBytes = 0, bodyOffset = 0;
};
struct MxfLayout {
    int64_t run = 0;  // run-in before the header partition
    std::vector<MxfPartition> parts;
    int64_t footer = -1;
    std::vector<int64_t> units, picture, pictureLen;  // each content package's start and its picture element
    std::vector<int> unitPart;
    int64_t cut = 0;  // the end of the last whole content package
    uint32_t indexSid = 0, bodySid = 0;
    int64_t indexed = 0;        // content packages the file's own index segments cover
    uint32_t cbrBytes = 0;      // a constant edit unit size from an index segment
    int rateNum = 0, rateDen = 1;
    std::vector<std::pair<int64_t, int64_t>> durations;  // header metadata Duration fields: where, and what rate
    std::vector<std::pair<int, int>> durationRates;
};

// Where an MXF file's header partition starts (after a run-in of up to 64 KiB), or -1 when it is not MXF.
int64_t mxfStart(const u8* d, int64_t n) {
    for (int64_t q = 0; q + 16 <= std::min<int64_t>(n, 65536 + 16); ++q)
        if (isPartition(d + q) && d[q + 13] == 2) return q;
    return -1;
}

// Whether an MXF file has its footer partition, by its random index pack at the end or its header's pointer to it
// (without walking the file).
bool mxfHasFooter(const u8* d, int64_t n, int64_t run) {
    auto footerAt = [&](uint64_t at) {
        return at > 0 && at < uint64_t(n) && uint64_t(run) + at + 16 <= uint64_t(n) && isPartition(d + run + at) &&
               d[run + int64_t(at) + 13] == 4;
    };
    if (n >= 20 + 12) {
        const uint32_t len = be32(d + n - 4);
        if (len >= 32 && len <= n && isRip(d + n - len) && footerAt(be64(d + n - 12))) return true;
    }
    Klv k;
    return klvAt(d, n, run, k) && k.end - k.value >= 32 && footerAt(be64(d + k.value + 24));
}

bool readMxf(const u8* d, int64_t n, MxfLayout& m, QString* error) {
    int64_t p = mxfStart(d, n);
    if (p < 0) return *error = QObject::tr("This is not an MXF file"), false;
    m.run = p;
    QByteArray firstKey;
    int perUnit = 0, inUnit = 0;
    int64_t unitEnd = -1;
    std::vector<int64_t> wholeEnds;  // the end of each content package seen whole so far
    // Header metadata: sets by InstanceUID, for the edit rates of their durations.
    std::map<QByteArray, std::pair<int, int>> trackRateOfSequence;  // sequence uid -> its track's edit rate
    std::map<QByteArray, QByteArray> sequenceOfComponent;
    struct DurationAt {
        int64_t at;
        QByteArray uid;
        int sampleNum = 0, sampleDen = 0;  // a descriptor's own rate
    };
    std::vector<DurationAt> durs;
    std::vector<std::pair<int, int>> rates;
    struct Segment {
        int64_t start, duration, end;
    };
    std::vector<Segment> segments;  // the file's own VBR index segments
    Klv k;
    while (klvAt(d, n, p, k)) {
        const u8* key = d + p;
        if (isPartition(key)) {
            MxfPartition part;
            part.at = p;
            part.packEnd = k.end;
            part.kind = key[13];
            part.pack = QByteArray(reinterpret_cast<const char*>(d + p), int(k.end - p));
            if (k.end - k.value >= 88) {
                const u8* v = d + k.value;
                part.kag = std::clamp<uint32_t>(be32(v + 4), 1, 1 << 20);
                part.headerBytes = be64(v + 32);
                part.indexBytes = be64(v + 40);
                part.indexSid = be32(v + 48);
                part.bodyOffset = be64(v + 52);
                part.bodySid = be32(v + 60);
            }
            if (part.kind == 4) m.footer = p;
            m.parts.push_back(part);
        } else if (isRip(key)) {
            break;
        } else if (isIndexSegment(key)) {
            int64_t start = 0, duration = 0;
            uint32_t bytes = 0, sid = 0;
            for (const auto& [tag, at, len] : localTags(d, k)) {
                if (tag == 0x3f0b && len == 8) m.rateNum = int(be32(d + at)), m.rateDen = int(be32(d + at + 4));
                if (tag == 0x3f0c && len == 8) start = int64_t(be64(d + at));
                if (tag == 0x3f0d && len == 8) duration = int64_t(be64(d + at));
                if (tag == 0x3f05 && len == 4) bytes = be32(d + at);
                if (tag == 0x3f06 && len == 4) sid = be32(d + at);
            }
            if (sid) m.indexSid = sid;
            if (bytes) m.cbrBytes = bytes;
            else if (start >= 0 && duration > 0) segments.push_back({start, duration, k.end});
        } else if (isMetadataSet(key) && m.parts.size() == 1) {
            QByteArray uid, seq;
            std::vector<QByteArray> comps;
            int rn = 0, rd = 0, sn = 0, sd = 0;
            std::vector<int64_t> here;
            int64_t container = -1;
            for (const auto& [tag, at, len] : localTags(d, k)) {
                if (tag == 0x3c0a && len == 16) uid = QByteArray(reinterpret_cast<const char*>(d + at), 16);
                if (tag == 0x4803 && len == 16) seq = QByteArray(reinterpret_cast<const char*>(d + at), 16);
                if (tag == 0x4b01 && len == 8) rn = int(be32(d + at)), rd = int(be32(d + at + 4));
                if (tag == 0x3001 && len == 8) sn = int(be32(d + at)), sd = int(be32(d + at + 4));
                if (tag == 0x0202 && len == 8) here.push_back(at);
                if (tag == 0x3002 && len == 8) container = at;
                if (tag == 0x1001 && len >= 8)
                    for (int64_t i = 0, c = be32(d + at); i < c && at + 8 + 16 * (i + 1) <= at + len; ++i)
                        comps.push_back(QByteArray(reinterpret_cast<const char*>(d + at + 8 + 16 * i), 16));
            }
            if (rn > 0 && rd > 0) {
                rates.push_back({rn, rd});
                if (!seq.isEmpty()) trackRateOfSequence[seq] = {rn, rd};
            }
            for (const QByteArray& c : comps) sequenceOfComponent[c] = uid;
            if (!comps.empty()) sequenceOfComponent[uid] = uid;
            for (int64_t at : here) durs.push_back({at, uid});
            if (container >= 0) durs.push_back({container, uid, sn > 0 ? sn : -1, sd > 0 ? sd : 1});
        } else if (isEssence(key) || isSystemItem(key)) {
            MxfPartition& part = m.parts.back();
            if (part.essence < 0) {
                const int64_t counted = part.packEnd + int64_t(part.headerBytes + part.indexBytes);
                part.essence = counted > part.packEnd && counted <= p ? counted : p;
                if (!m.bodySid) m.bodySid = part.bodySid;
            }
            const QByteArray thisKey(reinterpret_cast<const char*>(key), 16);
            if (firstKey.isEmpty()) firstKey = thisKey;
            if (thisKey == firstKey) {
                // The content package before is whole: the next one starts.
                if (!m.units.empty()) {
                    if (!perUnit) perUnit = inUnit;
                    wholeEnds.push_back(unitEnd);
                }
                m.units.push_back(p);
                m.unitPart.push_back(int(m.parts.size()) - 1);
                m.picture.push_back(-1);
                m.pictureLen.push_back(0);
                inUnit = 0;
            }
            ++inUnit;
            unitEnd = k.end;
            if ((key[12] == 0x05 || key[12] == 0x15) && m.picture.back() < 0) {
                m.picture.back() = k.value;
                m.pictureLen.back() = k.end - k.value;
            }
        }
        p = k.end;
    }
    if (m.parts.empty()) return *error = QObject::tr("This is not an MXF file"), false;
    // The last content package counts when it has as many elements as the first, or the file has its footer.
    if (!m.units.empty() && perUnit && inUnit >= perUnit) wholeEnds.push_back(unitEnd);
    if (!m.units.empty() && !perUnit && m.footer >= 0) wholeEnds.push_back(unitEnd);
    m.units.resize(wholeEnds.size());
    m.unitPart.resize(wholeEnds.size());
    m.picture.resize(wholeEnds.size());
    m.pictureLen.resize(wholeEnds.size());
    if (m.units.empty()) return *error = QObject::tr("The MXF file has no whole frames"), false;
    m.cut = wholeEnds.back();
    // The packages the file's own index segments cover, from the first on without a gap, in what is kept.
    std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
    for (const Segment& sg : segments)
        if (sg.end <= m.cut && sg.start <= m.indexed) m.indexed = std::max(m.indexed, sg.start + sg.duration);
    m.indexed = std::min<int64_t>(m.indexed, int64_t(m.units.size()));
    // The edit rate of the content packages: the index's, else the slowest track's.
    if (m.rateNum <= 0 || m.rateDen <= 0) {
        m.rateNum = 0;
        for (const auto& r : rates)
            if (!m.rateNum || double(r.first) / r.second < double(m.rateNum) / m.rateDen) m.rateNum = r.first, m.rateDen = r.second;
        if (!m.rateNum) m.rateNum = 25, m.rateDen = 1;
    }
    for (const DurationAt& du : durs) {
        std::pair<int, int> rate{m.rateNum, m.rateDen};
        if (du.sampleNum > 0) {
            rate = {du.sampleNum, du.sampleDen};
        } else if (du.sampleNum == 0) {
            const auto s = sequenceOfComponent.find(du.uid);
            if (s != sequenceOfComponent.end()) {
                const auto t = trackRateOfSequence.find(s->second);
                if (t != trackRateOfSequence.end()) rate = t->second;
            }
        }
        m.durations.push_back({du.at, 0});
        m.durationRates.push_back(rate);
    }
    return true;
}

// Each content package's index flags, from its picture: random access and, for MPEG-2, its place in display order and
// its prediction (as FFmpeg's MXF muxer indexes them, which readers expect).
struct UnitIndex {
    int8_t temporal = 0, keyOffset = 0;
    u8 flags = 0x80;
};
std::vector<UnitIndex> indexUnits(const u8* d, const MxfLayout& m) {
    const size_t n = m.units.size();
    std::vector<UnitIndex> out(n);
    std::vector<int> tref(n, -1);
    std::vector<bool> intra(n, true), sequence(n, false);
    bool coded = false, closedGop = false;  // (until a P picture, after which it no longer matters)
    for (size_t i = 0; i < n; ++i) {
        const int64_t p = m.picture[i], len = std::min<int64_t>(m.pictureLen[i], 8192);
        if (p < 0) continue;
        const u8* v = d + p;
        bool mpeg2 = false, avc = false, closed = false;
        int type = 1;
        for (int64_t q = 0; q + 8 <= len; ++q) {
            if (v[q] || v[q + 1] || v[q + 2] != 1) continue;
            const u8 code = v[q + 3];
            if (code == 0xb3) sequence[i] = true, mpeg2 = true;
            if (code == 0xb8) closed = closedGop = v[q + 7] & 0x40, mpeg2 = true;
            if (code == 0x00) {
                mpeg2 = true;
                tref[i] = v[q + 4] << 2 | v[q + 5] >> 6;
                type = v[q + 5] >> 3 & 7;
                break;
            }
            if (!mpeg2 && (code & 0x80) == 0 && ((code & 31) == 1 || (code & 31) == 5)) {
                avc = true;
                type = (code & 31) == 5 ? 1 : 2;
                break;
            }
            if (!mpeg2 && (code & 0x80) == 0 && (code & 31) == 9) avc = true;
        }
        if (!mpeg2 && !avc) continue;  // intra: every package a random access point
        coded = true;
        intra[i] = type == 1;
        if (avc) {
            out[i].flags = type == 1 ? 0x80 : 0x22;
        } else {
            // I: a sequence header (0x40), random access when its GOP is closed; P forward; B both ways, or only
            // backward in a closed GOP.
            out[i].flags = type == 1 ? u8((sequence[i] ? 0x40 : 0) | (sequence[i] && closed ? 0x80 : 0)) : type == 2 ? 0x22 : 0x33;
            if (type == 3 && closedGop) out[i].flags = 0x13;
            if (type == 2) closedGop = false;
        }
    }
    if (!coded) return out;
    // MPEG-2 reordering: the entry at a display slot names where that picture is stored.
    for (size_t key = 0; key < n;) {
        size_t end = key + 1;
        while (end < n && !intra[end]) ++end;
        for (size_t i = key; i < end; ++i) {
            const int slot = int(i - key);
            for (size_t j = key; j < end; ++j)
                if (tref[j] == slot) {
                    out[i].temporal = int8_t(std::clamp<int>(int(j) - int(i), -128, 127));
                    break;
                }
        }
        key = end;
    }
    // Key frame offsets: a B picture predicted both ways counts back to the key frame before the one its GOP opens with
    // until a P picture follows that one.
    int key = 0, lastKey = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!(out[i].flags & 0x33)) lastKey = key, key = int(i);
        if ((out[i].flags & 0x30) == 0x30) {
            out[i].keyOffset = int8_t(std::clamp(lastKey - int(i), -128, 0));
        } else {
            out[i].keyOffset = int8_t(std::clamp(key - int(i), -128, 0));
            if (out[i].flags & 0x20) lastKey = key;
        }
        if (!(out[i].flags & 0x33) && (out[i].flags & 0x40) && !out[i].temporal) out[i].flags |= 0x80;
    }
    return out;
}

QByteArray berKlv(const u8* key, const QByteArray& value) {
    QByteArray b(reinterpret_cast<const char*>(key), 16);
    put8(b, 0x83);
    put8(b, unsigned(value.size()) >> 16);
    put16(b, unsigned(value.size()) & 0xffff);
    return b + value;
}
QByteArray fill(int64_t from, uint32_t kag) {
    if (kag <= 1) return {};
    int64_t gap = (kag - from % kag) % kag;
    if (gap == 0) return {};
    while (gap < 20) gap += kag;
    return berKlv(kFillKey, QByteArray(int(gap - 20), '\0'));
}

bool repairMxf(const QString& damaged, const QString& output, RepairReport& report, QString* error,
               const std::function<bool(double)>& progress) {
    Mapped f(damaged);
    if (!f.data()) return *error = QObject::tr("The file cannot be read"), false;
    const u8* d = f.data();
    MxfLayout m;
    if (!readMxf(d, f.size(), m, error)) return false;
    const MxfPartition& header = m.parts.front();
    Klv hk;
    if (!klvAt(reinterpret_cast<const u8*>(header.pack.constData()), header.pack.size(), 0, hk) || hk.end - hk.value < 88)
        return *error = QObject::tr("The MXF header cannot be read"), false;
    const int64_t units = int64_t(m.units.size());
    std::vector<const MxfPartition*> kept;
    for (const MxfPartition& p : m.parts)
        if (p.at < m.cut && p.kind != 4) kept.push_back(&p);

    // The footer: its partition pack, an index table for what the file's own segments do not cover, and the RIP.
    const uint32_t kag = header.kag;
    const QByteArray align = fill(m.cut - m.run, kag);
    const int64_t footerAt = m.cut + align.size() - m.run;
    const uint32_t indexSid = m.indexSid ? m.indexSid : (m.bodySid == 2 ? 3 : 2);
    const uint32_t bodySid = m.bodySid ? m.bodySid : 1;
    QByteArray index;
    auto segment = [&](int64_t start, int64_t count, const QByteArray& entries) {
        QByteArray v;
        put16(v, 0x3c0a), put16(v, 16);
        const QByteArray uid = QUuid::createUuid().toRfc4122();
        v += uid;
        put16(v, 0x3f0b), put16(v, 8), put32(v, uint32_t(m.rateNum)), put32(v, uint32_t(m.rateDen));
        put16(v, 0x3f0c), put16(v, 8), put64(v, uint64_t(start));
        put16(v, 0x3f0d), put16(v, 8), put64(v, uint64_t(count));
        put16(v, 0x3f05), put16(v, 4), put32(v, entries.isEmpty() ? m.cbrBytes : 0);
        put16(v, 0x3f06), put16(v, 4), put32(v, indexSid);
        put16(v, 0x3f07), put16(v, 4), put32(v, bodySid);
        put16(v, 0x3f08), put16(v, 1), put8(v, 0);
        put16(v, 0x3f0e), put16(v, 1), put8(v, 0);
        if (!entries.isEmpty()) {
            put16(v, 0x3f0a), put16(v, unsigned(8 + entries.size()));
            put32(v, uint32_t(entries.size() / 11)), put32(v, 11);
            v += entries;
        }
        index += berKlv(kIndexKey, v);
    };
    if (m.cbrBytes) {
        segment(0, units, {});
    } else {
        const std::vector<UnitIndex> flags = indexUnits(d, m);
        for (int64_t s = m.indexed; s < units; s += 4096) {
            QByteArray entries;
            for (int64_t i = s; i < std::min<int64_t>(units, s + 4096); ++i) {
                const MxfPartition& part = m.parts[size_t(m.unitPart[size_t(i)])];
                put8(entries, u8(flags[size_t(i)].temporal));
                put8(entries, u8(flags[size_t(i)].keyOffset));
                put8(entries, flags[size_t(i)].flags);
                put64(entries, part.bodyOffset + uint64_t(m.units[size_t(i)] - part.essence));
            }
            segment(s, std::min<int64_t>(4096, units - s), entries);
        }
    }
    QByteArray footerValue = header.pack.mid(int(hk.value));
    const int64_t prevPart = kept.back()->at - m.run;
    u8 footerKey[16];
    std::memcpy(footerKey, kPartitionKey, 13);
    footerKey[13] = 0x04, footerKey[14] = 0x04, footerKey[15] = 0x00;
    QByteArray afterPack = fill(footerAt + 20 + footerValue.size(), kag);
    QByteArray afterIndex = fill(footerAt + 20 + footerValue.size() + afterPack.size() + index.size(), kag);
    set64(footerValue, 8, uint64_t(footerAt));
    set64(footerValue, 16, uint64_t(prevPart));
    set64(footerValue, 24, uint64_t(footerAt));
    set64(footerValue, 32, 0);
    set64(footerValue, 40, uint64_t(index.size() + afterIndex.size()));
    set32(footerValue, 48, indexSid);
    set64(footerValue, 52, 0);
    set32(footerValue, 60, 0);
    QByteArray footer = berKlv(footerKey, footerValue) + afterPack + index + afterIndex;
    // The random index pack: every partition and where it is.
    QByteArray rip;
    for (const MxfPartition* p : kept) put32(rip, p->bodySid), put64(rip, uint64_t(p->at - m.run));
    put32(rip, 0), put64(rip, uint64_t(footerAt));
    u8 ripKey[16];
    std::memcpy(ripKey, kPartitionKey, 13);
    ripKey[13] = 0x11, ripKey[14] = 0x01, ripKey[15] = 0x00;
    put32(rip, uint32_t(16 + 4 + rip.size() + 4));
    footer += berKlv(ripKey, rip);

    // What changes in the copied part: each partition pack names the footer, the header is closed and complete, and
    // the durations are the frames there are.
    std::vector<std::pair<int64_t, QByteArray>> patches;
    for (const MxfPartition* p : kept) {
        Klv pk;
        if (!klvAt(d, f.size(), p->at, pk) || pk.end - pk.value < 88) continue;
        QByteArray at8;
        put64(at8, uint64_t(footerAt));
        patches.push_back({pk.value + 24, at8});
    }
    patches.push_back({header.at + 14, QByteArray(1, char(0x04))});
    for (size_t i = 0; i < m.durations.size(); ++i) {
        const auto [num, den] = m.durationRates[i];
        const int64_t dur = int64_t(std::floor(double(units) * double(num) * m.rateDen / (double(den) * m.rateNum) + 1e-6));
        QByteArray v;
        put64(v, uint64_t(dur));
        patches.push_back({m.durations[i].first, v});
    }
    std::sort(patches.begin(), patches.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    QSaveFile out(output);
    if (!out.open(QIODevice::WriteOnly)) return *error = QObject::tr("%1 cannot be written").arg(output), false;
    int64_t at = 0;
    bool ok = true;
    for (const auto& [where, bytes] : patches) {
        if (where < at || where + bytes.size() > m.cut) continue;
        ok = ok && writeRange(out, d, at, where, progress, 0.99 * double(at) / double(m.cut), 0.99 * double(where) / double(m.cut)) &&
             out.write(bytes) == bytes.size();
        at = where + bytes.size();
    }
    ok = ok && writeRange(out, d, at, m.cut, progress, 0.99 * double(at) / double(m.cut), 0.99) &&
         out.write(align) == align.size() && out.write(footer) == footer.size();
    if (!ok) {
        out.cancelWriting();
        return *error = progress && !progress(0.99) ? QObject::tr("Cancelled") : QObject::tr("%1 cannot be written").arg(output), false;
    }
    if (!out.commit()) return *error = QObject::tr("%1 cannot be written").arg(output), false;
    report.kind = RepairKind::Mxf;
    report.videoFrames = int(units);
    report.seconds = double(units) * m.rateDen / m.rateNum;
    report.bytesUsed = m.cut - m.units.front();
    report.bytesLeft = f.size() - m.cut;
    return true;
}

bool opensWithStreams(const QString& path) {
    AVFormatContext* fmt = nullptr;
    const QByteArray p = path.toUtf8();
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) return false;
    bool ok = avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->nb_streams > 0;
    for (unsigned i = 0; ok && i < fmt->nb_streams; ++i)
        ok = fmt->streams[i]->codecpar->codec_id != AV_CODEC_ID_NONE;
    avformat_close_input(&fmt);
    return ok;
}

}  // namespace

RepairCheck checkRecording(const QString& path, bool probe) {
    RepairCheck c;
    Mapped f(path);
    if (!f.data()) {
        c.problem = QObject::tr("The file cannot be read");
        return c;
    }
    const u8* d = f.data();
    const int64_t n = f.size();
    c.opens = probe && opensWithStreams(path);
    QString error;
    if (n >= 12 && (!std::memcmp(d, "RIFF", 4) || !std::memcmp(d, "RF64", 4) || !std::memcmp(d, "BW64", 4)) &&
        !std::memcmp(d + 8, "WAVE", 4)) {
        WavLayout w;
        if (!readWav(d, n, w, &error)) {
            c.problem = error;
        } else if (w.dataWrong) {
            c.kind = RepairKind::Wav;
            c.problem = QObject::tr("The sound data's size was not written (the recording was interrupted)");
        } else if (w.riffWrong) {
            c.kind = RepairKind::Wav;
            c.problem = QObject::tr("The file's size in its header is wrong");
        }
        return c;
    }
    if (const int64_t run = mxfStart(d, n); run >= 0) {
        if (!mxfHasFooter(d, n, run)) {
            c.kind = RepairKind::Mxf;
            c.problem = QObject::tr("The MXF file has no footer (the recording was interrupted)");
        }
        return c;
    }
    static const uint32_t starts[] = {fcc("ftyp"), fcc("wide"), fcc("free"), fcc("skip"), fcc("mdat"), fcc("moov"), fcc("pnot"), fcc("uuid")};
    if (n >= 8 && std::find(std::begin(starts), std::end(starts), be32(d + 4)) != std::end(starts)) {
        bool moov = false, mdat = false;
        for (int64_t p = 0; p + 8 <= n;) {
            uint64_t size = be32(d + p);
            int64_t hdr = 8;
            const uint32_t type = be32(d + p + 4);
            if (size == 1 && p + 16 <= n) size = be64(d + p + 8), hdr = 16;
            if (size == 0) size = uint64_t(n - p);
            const bool fits = size >= uint64_t(hdr) && size <= uint64_t(n - p);
            if (type == fcc("mdat")) mdat = true;
            if (type == fcc("moov") && fits && !children(d, p + hdr, p + int64_t(size)).empty()) moov = true;
            if (!fits) break;
            p += int64_t(size);
        }
        if (!moov && mdat) {
            c.kind = RepairKind::Movie;
            c.needsReference = true;
            c.problem = QObject::tr("The file has no index (its recording stopped before it was written)");
        } else if (!moov) {
            c.problem = QObject::tr("The file has neither an index nor media data");
        }
        return c;
    }
    if (probe && !c.opens) c.problem = QObject::tr("Montage does not know how to repair this kind of file");
    return c;
}

double sidecarTimecode(const QString& path, double fps) {
    const QFileInfo fi(path);
    const QString base = fi.completeBaseName();
    QStringList names;
    for (const QString& ext : {QStringLiteral("XML"), QStringLiteral("xml")}) {
        names << base + "M01." + ext << base + "." + ext;
    }
    for (const QString& name : names) {
        QFile x(fi.dir().filePath(name));
        if (!x.open(QIODevice::ReadOnly)) continue;
        QXmlStreamReader r(&x);
        double rate = fps;
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement()) continue;
            const QString el = r.name().toString();
            if (el == "LtcChangeTable") {
                const double tc = r.attributes().value("tcFps").toDouble();
                if (tc > 0) rate = tc;
            } else if (el == "LtcChange") {
                // Sony: four BCD bytes, frames first, with flag bits above the digits.
                const QString v = r.attributes().value("value").toString();
                bool ok = false;
                const uint32_t bcd = v.toUInt(&ok, 16);
                if (!ok || v.size() != 8 || rate <= 0) continue;
                auto digits = [](unsigned b) { return int((b >> 4) * 10 + (b & 15)); };
                const int ff = digits(bcd >> 24 & 0x3f), ss = digits(bcd >> 16 & 0x7f), mm = digits(bcd >> 8 & 0x7f),
                          hh = digits(bcd & 0x3f);
                return ((hh * 60.0 + mm) * 60 + ss) + ff / std::round(rate);
            } else if (el.compare("StartTimecode", Qt::CaseInsensitive) == 0) {
                const auto m = QRegularExpression("^(\\d+):(\\d+):(\\d+)[:;.](\\d+)$").match(r.readElementText().trimmed());
                if (m.hasMatch() && rate > 0)
                    return ((m.captured(1).toInt() * 60.0 + m.captured(2).toInt()) * 60 + m.captured(3).toInt()) +
                           m.captured(4).toInt() / std::round(rate);
            }
        }
    }
    return -1;
}

bool sameFile(const QString& a, const QString& b) {
    const QFileInfo fa(a), fb(b);
    QString pa = fa.exists() ? fa.canonicalFilePath() : fa.absoluteFilePath(), pb = fb.exists() ? fb.canonicalFilePath() : fb.absoluteFilePath();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return pa.compare(pb, Qt::CaseInsensitive) == 0;  // (their file systems mostly ignore case)
#else
    return pa == pb;
#endif
}

QString repairedPath(const QString& damaged) {
    const QFileInfo fi(damaged);
    const QString base = fi.completeBaseName(), ext = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    QString path = fi.dir().filePath(base + "_repaired" + ext);
    for (int i = 2; QFileInfo::exists(path); ++i) path = fi.dir().filePath(QString("%1_repaired_%2%3").arg(base).arg(i).arg(ext));
    return path;
}

bool repairRecording(const QString& damaged, const QString& reference, const QString& output, RepairReport* report,
                     QString* error, const std::function<bool(double)>& progress) {
    RepairReport local;
    RepairReport& r = report ? *report : local;
    r = RepairReport{};
    QString err;
    QString& e = error ? *error : err;
    if (sameFile(output, damaged) || (!reference.isEmpty() && sameFile(output, reference)))
        return e = QObject::tr("Save the repair as a new file: the damaged file and the good clip are kept as they are"), false;
    const RepairCheck check = checkRecording(damaged, false);
    bool ok = false;
    switch (check.kind) {
    case RepairKind::Movie:
        if (reference.isEmpty()) return e = QObject::tr("A good clip from the same camera is needed to rebuild this file"), false;
        ok = repairMovie(damaged, reference, output, r, &e, progress);
        break;
    case RepairKind::Wav:
        ok = repairWav(damaged, output, r, &e, progress);
        break;
    case RepairKind::Mxf:
        ok = repairMxf(damaged, output, r, &e, progress);
        break;
    default:
        e = check.problem.isEmpty() ? QObject::tr("Montage cannot repair this file") : check.problem;
        return false;
    }
    return ok;
}

}  // namespace montage
