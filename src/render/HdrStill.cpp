#include "HdrStill.h"

#include <QByteArray>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <vector>

#include "ColorSpace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
}

namespace montage {

namespace {

uint32_t crc32(const char* data, size_t n) {
    static uint32_t table[256];
    static const bool made = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)made;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ uint8_t(data[i])) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void put16(QByteArray& b, unsigned v) {
    b.append(char(v >> 8));
    b.append(char(v));
}
void put32(QByteArray& b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xffff);
}
uint32_t get32(const QByteArray& b, qsizetype at) {
    return uint32_t(uint8_t(b[at])) << 24 | uint32_t(uint8_t(b[at + 1])) << 16 | uint32_t(uint8_t(b[at + 2])) << 8 | uint8_t(b[at + 3]);
}
uint32_t get16(const QByteArray& b, qsizetype at) { return uint32_t(uint8_t(b[at])) << 8 | uint8_t(b[at + 1]); }

QByteArray chunk(const char* type, const QByteArray& data) {
    QByteArray b;
    put32(b, uint32_t(data.size()));
    QByteArray body(type, 4);
    body += data;
    b += body;
    put32(b, crc32(body.constData(), size_t(body.size())));
    return b;
}

uint32_t units(double nits) { return uint32_t(std::clamp(std::lround(nits * 10000), 0L, long(INT32_MAX))); }

// The mastering display Montage assumes (as its HDR10 video says): P3 D65 primaries, `peak` nits, 0.0001 at black.
struct Mastering {
    double primaries[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};  // R, G, B
    double white[2] = {0.3127, 0.3290};
};

// ISO base media file boxes (AVIF's HEIF structure).
QByteArray box(const char* type, const QByteArray& data) {
    QByteArray b;
    put32(b, uint32_t(8 + data.size()));
    b.append(type, 4);
    return b + data;
}

struct Box {
    qsizetype at = 0, size = 0, body = 0;  // where it starts, its whole size, where its contents start
    QByteArray type;
    qsizetype end() const { return at + size; }
};

// The boxes in [from, to) of `b`; false when one is malformed.
bool boxes(const QByteArray& b, qsizetype from, qsizetype to, std::vector<Box>& out) {
    out.clear();
    while (from < to) {
        if (to - from < 8) return false;
        uint64_t size = get32(b, from);
        qsizetype head = 8;
        if (size == 1) {
            if (to - from < 16) return false;
            size = uint64_t(get32(b, from + 8)) << 32 | get32(b, from + 12);
            head = 16;
        } else if (size == 0) {
            size = uint64_t(to - from);
        }
        if (size < uint64_t(head) || size > uint64_t(to - from)) return false;
        out.push_back({from, qsizetype(size), from + head, b.mid(from + 4, 4)});
        from += qsizetype(size);
    }
    return true;
}

const Box* find(const std::vector<Box>& list, const char* type) {
    for (const Box& x : list)
        if (x.type == type) return &x;
    return nullptr;
}

// A big-endian unsigned field of `bytes` (0-8) at `at`.
uint64_t getN(const QByteArray& b, qsizetype at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | uint8_t(b[at + i]);
    return v;
}
void setN(QByteArray& b, qsizetype at, int bytes, uint64_t v) {
    for (int i = bytes - 1; i >= 0; --i, v >>= 8) b[at + i] = char(v & 0xff);
}

// The item properties of an AVIF's meta box (its contents from `meta`), in order; false when it has none.
bool avifProperties(const QByteArray& b, const Box& meta, std::vector<Box>& props) {
    std::vector<Box> kids, iprp;
    if (meta.body + 4 > meta.end() || !boxes(b, meta.body + 4, meta.end(), kids)) return false;  // (a full box)
    const Box* p = find(kids, "iprp");
    if (!p || !boxes(b, p->body, p->end(), iprp)) return false;
    const Box* ipco = find(iprp, "ipco");
    return ipco && boxes(b, ipco->body, ipco->end(), props);
}

// Adds mastering display (mdcv) and light level (clli) properties to an AVIF written by FFmpeg, which writes neither,
// for its primary item: the boxes grow and the image data moves, so its locations (iloc) move with it. False (the file
// as it was) for a layout this does not know.
bool addAvifLightLevels(QByteArray& b, double peakNits, double maxCll, double maxFall) {
    std::vector<Box> top, kids, iprp, props;
    if (!boxes(b, 0, b.size(), top) || find(top, "moov")) return false;
    const Box* meta = find(top, "meta");
    if (!meta || meta->body + 4 > meta->end() || !boxes(b, meta->body + 4, meta->end(), kids)) return false;
    const Box *iloc = find(kids, "iloc"), *pitm = find(kids, "pitm"), *iprpBox = find(kids, "iprp");
    if (!iloc || !pitm || !iprpBox || !boxes(b, iprpBox->body, iprpBox->end(), iprp)) return false;
    const Box *ipco = find(iprp, "ipco"), *ipma = find(iprp, "ipma");
    if (!ipco || !ipma || ipma->at < ipco->end() || !boxes(b, ipco->body, ipco->end(), props)) return false;
    // (Not for an FFmpeg that writes them itself, nor where transformative properties, which must come after descriptive
    // ones, are associated.)
    for (const Box& x : props)
        if (x.type == "clli" || x.type == "mdcv" || x.type == "irot" || x.type == "imir" || x.type == "clap") return false;
    for (const Box* x : {meta, iprpBox, ipco, ipma})
        if (x->body - x->at != 8) return false;  // (32-bit sizes, which grow in place)
    // The primary item.
    if (pitm->body + 6 > pitm->end()) return false;
    const int pitmVersion = uint8_t(b[pitm->body]);
    const uint64_t primary = pitmVersion == 0 ? get16(b, pitm->body + 4) : getN(b, pitm->body + 4, 4);

    // The new properties: mdcv (primaries G, B, R in 0.00002, white, luminance in 0.0001 nits) and clli (whole nits).
    const Mastering m;
    QByteArray mdcv;
    for (int i : {1, 2, 0})
        for (double v : m.primaries[i]) put16(mdcv, unsigned(std::lround(v * 50000)));
    for (double v : m.white) put16(mdcv, unsigned(std::lround(v * 50000)));
    put32(mdcv, units(peakNits));
    put32(mdcv, units(0.0001));
    QByteArray clli;
    put16(clli, unsigned(std::clamp(std::lround(maxCll), 0L, 65535L)));
    put16(clli, unsigned(std::clamp(std::lround(maxFall), 0L, 65535L)));
    const QByteArray added = box("mdcv", mdcv) + box("clli", clli);
    const int first = int(props.size()) + 1;  // their indices (1-based)

    // The associations, with the primary item's two more.
    qsizetype at = ipma->body;
    if (at + 8 > ipma->end()) return false;
    const int ipmaVersion = uint8_t(b[at]);
    const bool wideIndex = uint8_t(b[at + 3]) & 1;
    if (!wideIndex && first + 1 > 127) return false;
    const uint32_t entries = get32(b, at + 4);
    QByteArray assoc = b.mid(at, 8);
    at += 8;
    bool found = false;
    for (uint32_t e = 0; e < entries; ++e) {
        const int idBytes = ipmaVersion < 1 ? 2 : 4;
        if (at + idBytes + 1 > ipma->end()) return false;
        const uint64_t item = getN(b, at, idBytes);
        const int count = uint8_t(b[at + idBytes]);
        const qsizetype len = idBytes + 1 + qsizetype(count) * (wideIndex ? 2 : 1);
        if (at + len > ipma->end()) return false;
        QByteArray entry = b.mid(at, len);
        if (item == primary) {
            if (count > 253) return false;
            entry[idBytes] = char(count + 2);
            for (int k = 0; k < 2; ++k) {
                if (wideIndex) put16(entry, unsigned(first + k));
                else entry.append(char(first + k));
            }
            found = true;
        }
        assoc += entry;
        at += len;
    }
    if (!found) return false;
    const qsizetype grow = qsizetype(added.size()) + (qsizetype(assoc.size()) - (ipma->end() - ipma->body));

    // Item locations in this file at or after the insertion move by `grow`.
    const qsizetype insertAt = ipco->end();
    QByteArray out = b;
    at = iloc->body;
    if (at + 6 > iloc->end()) return false;
    const int ilocVersion = uint8_t(b[at]);
    if (ilocVersion > 2 || at + 6 + (ilocVersion < 2 ? 2 : 4) > iloc->end()) return false;
    const int offsetSize = uint8_t(b[at + 4]) >> 4, lengthSize = uint8_t(b[at + 4]) & 15;
    const int baseSize = uint8_t(b[at + 5]) >> 4, indexSize = ilocVersion >= 1 ? uint8_t(b[at + 5]) & 15 : 0;
    at += 6;
    const int idBytes = ilocVersion < 2 ? 2 : 4;
    const uint64_t items = getN(b, at, idBytes);
    at += idBytes;
    for (uint64_t i = 0; i < items; ++i) {
        if (at + idBytes + (ilocVersion >= 1 ? 2 : 0) + 2 + baseSize + 2 > iloc->end()) return false;
        at += idBytes;
        int method = 0;
        if (ilocVersion >= 1) method = get16(b, at) & 15, at += 2;
        const unsigned dataRef = get16(b, at);
        at += 2;
        const qsizetype baseAt = at;
        const uint64_t base = getN(b, at, baseSize);
        at += baseSize;
        const unsigned extents = get16(b, at);
        at += 2;
        const bool here = method == 0 && dataRef == 0;  // (in this file, by offset)
        bool movedBase = false;
        for (unsigned e = 0; e < extents; ++e) {
            if (at + indexSize + offsetSize + lengthSize > iloc->end()) return false;
            at += indexSize;
            const uint64_t offset = getN(b, at, offsetSize);
            if (here && base + offset >= uint64_t(insertAt)) {
                if (baseSize > 0) {
                    if (base + grow >= (uint64_t(1) << (8 * std::min(baseSize, 7)))) return false;
                    if (!movedBase) setN(out, baseAt, baseSize, base + grow), movedBase = true;
                } else {
                    if (offsetSize == 0 || offset + grow >= (uint64_t(1) << (8 * offsetSize))) return false;
                    setN(out, at, offsetSize, offset + grow);
                }
            }
            at += offsetSize + lengthSize;
        }
    }
    if (iloc->at >= insertAt) return false;  // (its own place would move too)

    for (const Box* x : {meta, iprpBox})
        if (uint64_t(x->size) + uint64_t(grow) > 0xffffffffu) return false;
    // The boxes holding the additions grow (meta holds iprp, which holds ipco and ipma).
    setN(out, meta->at, 4, uint64_t(meta->size + grow));
    setN(out, iprpBox->at, 4, uint64_t(iprpBox->size + grow));
    setN(out, ipco->at, 4, uint64_t(ipco->size + added.size()));
    setN(out, ipma->at, 4, uint64_t(8 + assoc.size()));
    b = out.left(insertAt) + added + out.mid(insertAt, ipma->body - insertAt) + assoc + out.mid(ipma->end());
    return true;
}

}  // namespace

int cicpPrimaries(const ColorSpace& space) {
    switch (space.primaries) {
        case Primaries::Bt709: return 1;
        case Primaries::Bt2020: return 9;
        case Primaries::P3D65: return 12;
        default: return -1;
    }
}

int cicpTransfer(const ColorSpace& space) {
    switch (space.transfer) {
        case Transfer::Bt1886: return 1;
        case Transfer::Srgb: return 13;
        case Transfer::Pq: return 16;
        case Transfer::Hlg: return 18;
        default: return -1;
    }
}

namespace {

bool readPngHdrInfo(QFile& f, StillHdrInfo& out) {
    // The chunks before the image data (where all three must be).
    for (int n = 0; n < 1000; ++n) {
        const QByteArray head = f.read(8);
        if (head.size() < 8) break;
        const uint32_t len = get32(head, 0);
        const QByteArray type = head.mid(4, 4);
        if (type == "IDAT" || type == "IEND" || len > (1u << 24)) break;
        const QByteArray data = f.read(len);
        if (data.size() != qsizetype(len)) break;
        f.read(4);  // CRC
        if (type == "cICP" && len == 4) {
            out.primaries = uint8_t(data[0]);
            out.transfer = uint8_t(data[1]);
            out.matrix = uint8_t(data[2]);
            out.fullRange = data[3] != 0;
        } else if ((type == "cLLI" || type == "cLLi") && len == 8) {  // (the draft's names too)
            out.maxCll = get32(data, 0) / 10000.0;
            out.maxFall = get32(data, 4) / 10000.0;
        } else if ((type == "mDCV" || type == "mDCv") && len == 24) {
            out.masteringMaxNits = get32(data, 16) / 10000.0;
            out.masteringMinNits = get32(data, 20) / 10000.0;
        }
    }
    return true;
}

// An AVIF's (or other HEIF's) properties: the first colr of type nclx, clli and mdcv.
bool readAvifHdrInfo(QFile& f, StillHdrInfo& out) {
    QByteArray meta;
    for (qint64 at = 0, n = 0; at + 8 <= f.size() && n < 64; ++n) {  // (meta comes first in a still)
        f.seek(at);
        const QByteArray head = f.read(16);
        if (head.size() < 8) return false;
        uint64_t size = get32(head, 0);
        if (size == 1 && head.size() == 16) size = uint64_t(get32(head, 8)) << 32 | get32(head, 12);
        else if (size == 0) size = uint64_t(f.size() - at);
        if (size < 8 || size > uint64_t(f.size() - at)) return false;
        if (head.mid(4, 4) == "meta") {
            if (size > (16u << 20)) return false;
            f.seek(at);
            meta = f.read(qint64(size));
            break;
        }
        at += qint64(size);
    }
    std::vector<Box> top, props;
    if (meta.isEmpty() || !boxes(meta, 0, meta.size(), top) || top.size() != 1 || !avifProperties(meta, top[0], props)) return false;
    bool colour = false;
    for (const Box& p : props) {
        const qsizetype n = p.end() - p.body;
        if (p.type == "colr" && !colour && n >= 11 && meta.mid(p.body, 4) == "nclx") {
            out.primaries = int(get16(meta, p.body + 4));
            out.transfer = int(get16(meta, p.body + 6));
            out.matrix = int(get16(meta, p.body + 8));
            out.fullRange = uint8_t(meta[p.body + 10]) & 0x80;
            colour = true;
        } else if (p.type == "clli" && n >= 4) {
            out.maxCll = get16(meta, p.body);
            out.maxFall = get16(meta, p.body + 2);
        } else if (p.type == "mdcv" && n >= 24) {
            out.masteringMaxNits = get32(meta, p.body + 16) / 10000.0;
            out.masteringMinNits = get32(meta, p.body + 20) / 10000.0;
        }
    }
    return true;
}

}  // namespace

bool readStillHdrInfo(const std::string& path, StillHdrInfo& out) {
    out = StillHdrInfo();
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray sig = f.read(12);
    if (sig.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8))) {
        f.seek(8);
        return readPngHdrInfo(f, out);
    }
    if (sig.size() == 12 && sig.mid(4, 4) == "ftyp") return readAvifHdrInfo(f, out);
    return false;
}

bool writeHdrPng(const Image& img, const ColorSpace& space, double peakNits, double maxCll, double maxFall, const std::string& path,
                 std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (img.empty()) return fail("Nothing to write");
    std::vector<uint16_t> px;
    toRgba16(img, px);
    const QImage q(reinterpret_cast<const uchar*>(px.data()), img.width, img.height, img.width * 8, QImage::Format_RGBA64);
    QByteArray png;
    {
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "png");
        if (!w.write(q)) return fail("Cannot encode the PNG");
    }
    if (!png.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8))) return fail("Cannot encode the PNG");
    // The chunks as Qt wrote them, without any colour chunk of its own; ours go straight after IHDR (cICP must come
    // before the image data, and overrides any other).
    QByteArray out = png.left(8);
    const int primaries = cicpPrimaries(space), transfer = cicpTransfer(space);
    const bool hdr = space.hdr() && primaries > 0 && transfer > 0;
    for (qsizetype at = 8; at + 12 <= png.size();) {
        const uint32_t len = get32(png, at);
        const QByteArray type = png.mid(at + 4, 4);
        const qsizetype next = at + 12 + qsizetype(len);
        if (next > png.size()) return fail("Cannot encode the PNG");
        if (!(hdr && (type == "gAMA" || type == "sRGB" || type == "iCCP" || type == "cHRM"))) out += png.mid(at, next - at);
        if (type == "IHDR" && hdr) {
            QByteArray cicp;
            cicp.append(char(primaries)).append(char(transfer)).append(char(0)).append(char(1));
            out += chunk("cICP", cicp);
            const Mastering m;
            QByteArray mdcv;
            for (const auto& xy : m.primaries)
                for (double v : xy) put16(mdcv, unsigned(std::lround(v * 50000)));
            for (double v : m.white) put16(mdcv, unsigned(std::lround(v * 50000)));
            put32(mdcv, units(peakNits));
            put32(mdcv, units(0.0001));
            out += chunk("mDCV", mdcv);
            QByteArray clli;
            put32(clli, units(maxCll));
            put32(clli, units(maxFall));
            out += chunk("cLLI", clli);
        }
        at = next;
    }
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(out) != out.size()) return fail("Cannot write " + path);
    return true;
}

bool writeAvifStill(const Image& img, const ColorSpace& space, int quality, double peakNits, double maxCll, double maxFall,
                    const std::string& path, std::string* error, bool* stated) {
    if (stated) *stated = false;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (img.empty()) return fail("Nothing to write");
    const bool hdr = space.hdr();
    const int bits = hdr ? 10 : 8;
    const int maxCode = (1 << bits) - 1;
    const int W = img.width, H = img.height, CW = (W + 1) / 2, CH = (H + 1) / 2;
    // Y'CbCr, full range, by the space's matrix (BT.2020 non-constant luminance for its primaries, else BT.709).
    const bool wide = space.primaries == Primaries::Bt2020;
    const double kr = wide ? 0.2627 : 0.2126, kb = wide ? 0.0593 : 0.0722, kg = 1 - kr - kb;
    std::vector<uint16_t> Y(size_t(W) * H), U(size_t(CW) * CH), V(size_t(CW) * CH);
    std::vector<double> cb(size_t(W) * H), cr(size_t(W) * H);
    for (int y = 0; y < H; ++y) {
        const float* p = img.row(y);
        for (int x = 0; x < W; ++x, p += 4) {
            const double r = std::clamp(p[0], 0.f, 1.f), g = std::clamp(p[1], 0.f, 1.f), b = std::clamp(p[2], 0.f, 1.f);
            const double luma = kr * r + kg * g + kb * b;
            Y[size_t(y) * W + x] = uint16_t(std::lround(luma * maxCode));
            cb[size_t(y) * W + x] = (b - luma) / (2 * (1 - kb));
            cr[size_t(y) * W + x] = (r - luma) / (2 * (1 - kr));
        }
    }
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) {
            double sb = 0, sr = 0;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = std::min(W - 1, 2 * x + dx), yy = std::min(H - 1, 2 * y + dy);
                    sb += cb[size_t(yy) * W + xx], sr += cr[size_t(yy) * W + xx], ++n;
                }
            const double mid = (maxCode + 1) / 2.0;
            U[size_t(y) * CW + x] = uint16_t(std::clamp(std::lround(sb / n * maxCode + mid), 0L, long(maxCode)));
            V[size_t(y) * CW + x] = uint16_t(std::clamp(std::lround(sr / n * maxCode + mid), 0L, long(maxCode)));
        }

    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "avif", path.c_str()) < 0 || !oc) return fail("This build of FFmpeg cannot write AVIF");
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = av_packet_alloc();
    bool ok = false;
    if (!pkt) {
        avformat_free_context(oc);
        return fail("Out of memory");
    }
    std::string why = "No AV1 encoder in this build of FFmpeg";
    struct Cleanup {
        AVFormatContext*& oc;
        AVCodecContext*& ctx;
        AVFrame*& frame;
        AVPacket*& pkt;
        ~Cleanup() {
            av_frame_free(&frame);
            av_packet_free(&pkt);
            avcodec_free_context(&ctx);
            if (oc && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    } cleanup{oc, ctx, frame, pkt};
    // (A small still is quick at the slower presets.)
    const int crf = std::clamp(int(std::lround((100 - std::clamp(quality, 0, 100)) * 0.63)), 0, 63);
    for (const char* name : {"libsvtav1", "libaom-av1", "librav1e"}) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;
        avcodec_free_context(&ctx);
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return fail("Out of memory");
        ctx->width = W;
        ctx->height = H;
        ctx->pix_fmt = hdr ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        ctx->time_base = {1, 1};
        ctx->framerate = {1, 1};
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->color_primaries = space.primaries == Primaries::Bt2020 ? AVCOL_PRI_BT2020
                               : space.primaries == Primaries::P3D65 ? AVCOL_PRI_SMPTE432
                                                                     : AVCOL_PRI_BT709;
        ctx->color_trc = space.transfer == Transfer::Pq    ? AVCOL_TRC_SMPTE2084
                         : space.transfer == Transfer::Hlg ? AVCOL_TRC_ARIB_STD_B67
                         : space.transfer == Transfer::Srgb ? AVCOL_TRC_IEC61966_2_1
                                                            : AVCOL_TRC_BT709;
        ctx->colorspace = wide ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
        ctx->gop_size = 1;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        if (std::string(name) == "librav1e") av_dict_set_int(&opts, "qp", crf * 4, 0);
        else av_dict_set_int(&opts, "crf", crf, 0);
        if (std::string(name) == "libaom-av1") av_dict_set(&opts, "still-picture", "1", 0);
        const int rc = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (rc >= 0) {
            why.clear();
            break;
        }
        why = std::string("The AV1 encoder (") + name + ") would not take a " + std::to_string(W) + " x " + std::to_string(H) + " picture";
    }
    if (!why.empty()) return fail(why);
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0) return fail("Cannot write " + path);
    st->time_base = ctx->time_base;
    if (hdr) {
        AVMasteringDisplayMetadata md{};
        const Mastering m;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j) md.display_primaries[i][j] = av_make_q(int(std::lround(m.primaries[i][j] * 50000)), 50000);
        md.white_point[0] = av_make_q(15635, 50000);
        md.white_point[1] = av_make_q(16450, 50000);
        md.max_luminance = av_make_q(int(std::lround(peakNits)), 1);
        md.min_luminance = av_make_q(1, 10000);
        md.has_primaries = md.has_luminance = 1;
        AVContentLightMetadata cl{};
        cl.MaxCLL = unsigned(std::lround(maxCll));
        cl.MaxFALL = unsigned(std::lround(maxFall));
        if (AVPacketSideData* sd = av_packet_side_data_new(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof md, 0))
            std::memcpy(sd->data, &md, sizeof md);
        if (AVPacketSideData* sd = av_packet_side_data_new(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof cl, 0))
            std::memcpy(sd->data, &cl, sizeof cl);
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return fail("Cannot write " + path);
    if (avformat_write_header(oc, nullptr) < 0) return fail("Cannot write " + path);
    frame = av_frame_alloc();
    if (!frame) return fail("Out of memory");
    frame->format = ctx->pix_fmt;
    frame->width = W;
    frame->height = H;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->pts = 0;
    if (av_frame_get_buffer(frame, 0) < 0) return fail("Out of memory");
    auto plane = [&](int i, const std::vector<uint16_t>& src, int w, int h) {
        for (int y = 0; y < h; ++y) {
            if (hdr) {
                auto* d = reinterpret_cast<uint16_t*>(frame->data[i] + size_t(y) * size_t(frame->linesize[i]));
                std::memcpy(d, src.data() + size_t(y) * w, size_t(w) * 2);
            } else {
                uint8_t* d = frame->data[i] + size_t(y) * size_t(frame->linesize[i]);
                for (int x = 0; x < w; ++x) d[x] = uint8_t(src[size_t(y) * w + x]);
            }
        }
    };
    plane(0, Y, W, H);
    plane(1, U, CW, CH);
    plane(2, V, CW, CH);
    auto drain = [&]() -> bool {
        for (;;) {
            const int r = avcodec_receive_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(oc, pkt) < 0) return false;
        }
    };
    ok = avcodec_send_frame(ctx, frame) >= 0 && drain() && avcodec_send_frame(ctx, nullptr) >= 0 && drain() && av_write_trailer(oc) >= 0;
    if (!ok) return fail("Cannot encode the AVIF");
    if (!hdr) return true;
    // Its light levels and mastering display, as properties of the image (the file replaced whole).
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    QFile in(QString::fromStdString(path));
    if (!in.open(QIODevice::ReadOnly)) return fail("Cannot read back " + path);
    QByteArray bytes = in.readAll();
    in.close();
    if (!addAvifLightLevels(bytes, peakNits, maxCll, maxFall)) return true;
    QSaveFile out(QString::fromStdString(path));
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) return fail("Cannot write " + path);
    if (stated) *stated = true;
    return true;
}

}  // namespace montage
