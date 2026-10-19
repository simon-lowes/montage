#include "HdrStill.h"

#include <QByteArray>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <vector>

#include "ColorSpace.h"
#include "media/GainMap.h"

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

namespace {

// A picture for the AV1 encoder: Y'CbCr at 4:2:0 or 4:4:4 (Cb and Cr at half size for 4:2:0), or R, G and B
// (identity matrix), full range, with the colour it is tagged with.
enum class Av1Layout { Yuv420, Yuv444, Gbr };
struct Av1Picture {
    int width = 0, height = 0, bits = 8;
    Av1Layout layout = Av1Layout::Yuv420;
    std::vector<uint16_t> planes[3];
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
    AVColorSpace matrix = AVCOL_SPC_BT709;
};

AVColorPrimaries avPrimaries(Primaries p) {
    return p == Primaries::Bt2020 ? AVCOL_PRI_BT2020 : p == Primaries::P3D65 ? AVCOL_PRI_SMPTE432 : AVCOL_PRI_BT709;
}

// `img` (code values, over black) as Y'CbCr 4:2:0 by the BT.2020 non-constant luminance matrix for its primaries, else
// BT.709's; tagged with `space` (its transfer replaced by `transfer` when given).
Av1Picture ycbcr420(const Image& img, const ColorSpace& space, int bits, AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED) {
    Av1Picture pic;
    const int maxCode = (1 << bits) - 1;
    const int W = img.width, H = img.height, CW = (W + 1) / 2, CH = (H + 1) / 2;
    const bool wide = space.primaries == Primaries::Bt2020;
    const double kr = wide ? 0.2627 : 0.2126, kb = wide ? 0.0593 : 0.0722, kg = 1 - kr - kb;
    pic.width = W, pic.height = H, pic.bits = bits;
    std::vector<uint16_t>&Y = pic.planes[0], &U = pic.planes[1], &V = pic.planes[2];
    Y.resize(size_t(W) * H), U.resize(size_t(CW) * CH), V.resize(size_t(CW) * CH);
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
    pic.primaries = avPrimaries(space.primaries);
    pic.transfer = transfer != AVCOL_TRC_UNSPECIFIED ? transfer
                   : space.transfer == Transfer::Pq    ? AVCOL_TRC_SMPTE2084
                   : space.transfer == Transfer::Hlg   ? AVCOL_TRC_ARIB_STD_B67
                   : space.transfer == Transfer::Srgb  ? AVCOL_TRC_IEC61966_2_1
                                                       : AVCOL_TRC_BT709;
    pic.matrix = wide ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    return pic;
}

// An 8-bit RGB picture (a gain map) as R, G and B, or as Y'CbCr 4:4:4 or 4:2:0 by BT.601's matrix.
Av1Picture rgbPicture(const QImage& rgb888, Av1Layout layout) {
    Av1Picture pic;
    const int W = rgb888.width(), H = rgb888.height(), CW = layout == Av1Layout::Yuv420 ? (W + 1) / 2 : W,
              CH = layout == Av1Layout::Yuv420 ? (H + 1) / 2 : H;
    pic.width = W, pic.height = H, pic.layout = layout;
    pic.planes[0].resize(size_t(W) * H);
    for (int i = 1; i < 3; ++i) pic.planes[i].resize(layout == Av1Layout::Gbr ? size_t(W) * H : size_t(CW) * CH);
    std::vector<double> cb, cr;
    if (layout == Av1Layout::Yuv420) cb.resize(size_t(W) * H), cr.resize(size_t(W) * H);
    for (int y = 0; y < H; ++y) {
        const uchar* s = rgb888.constScanLine(y);
        for (int x = 0; x < W; ++x, s += 3) {
            const size_t i = size_t(y) * W + x;
            if (layout == Av1Layout::Gbr) {
                for (int c = 0; c < 3; ++c) pic.planes[c][i] = s[c];
                continue;
            }
            const double r = s[0], g = s[1], b = s[2], luma = 0.299 * r + 0.587 * g + 0.114 * b;
            pic.planes[0][i] = uint16_t(std::clamp(std::lround(luma), 0L, 255L));
            const double u = 128 + (b - luma) / 1.772, v = 128 + (r - luma) / 1.402;
            if (layout == Av1Layout::Yuv444) {
                pic.planes[1][i] = uint16_t(std::clamp(std::lround(u), 0L, 255L));
                pic.planes[2][i] = uint16_t(std::clamp(std::lround(v), 0L, 255L));
            } else {
                cb[i] = u, cr[i] = v;
            }
        }
    }
    if (layout == Av1Layout::Yuv420)
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                double sb = 0, sr = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const size_t i = size_t(std::min(H - 1, 2 * y + dy)) * W + size_t(std::min(W - 1, 2 * x + dx));
                        sb += cb[i], sr += cr[i];
                    }
                pic.planes[1][size_t(y) * CW + x] = uint16_t(std::clamp(std::lround(sb / 4), 0L, 255L));
                pic.planes[2][size_t(y) * CW + x] = uint16_t(std::clamp(std::lround(sr / 4), 0L, 255L));
            }
    pic.primaries = AVCOL_PRI_BT709;
    pic.transfer = AVCOL_TRC_IEC61966_2_1;
    pic.matrix = layout == Av1Layout::Gbr ? AVCOL_SPC_RGB : AVCOL_SPC_SMPTE170M;
    return pic;
}

// Encodes `pic` into an AVIF file at `path` (FFmpeg's muxer) with the first of `encoders` that takes it; with a
// mastering display of `peakNits` and light levels when `peakNits` > 0.
bool encodeAvif(const Av1Picture& pic, int quality, std::initializer_list<const char*> encoders, const std::string& path,
                std::string& why, double peakNits = 0, double maxCll = 0, double maxFall = 0) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "avif", path.c_str()) < 0 || !oc) return why = "This build of FFmpeg cannot write AVIF", false;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = av_packet_alloc();
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
    if (!pkt) return why = "Out of memory", false;
    const AVPixelFormat format = pic.layout == Av1Layout::Gbr      ? (pic.bits > 8 ? AV_PIX_FMT_GBRP10LE : AV_PIX_FMT_GBRP)
                                 : pic.layout == Av1Layout::Yuv444 ? (pic.bits > 8 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P)
                                                                   : (pic.bits > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P);
    why = "No AV1 encoder in this build of FFmpeg";
    // (A small still is quick at the slower presets.)
    const int crf = std::clamp(int(std::lround((100 - std::clamp(quality, 0, 100)) * 0.63)), 0, 63);
    bool opened = false;
    for (const char* name : encoders) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;
        avcodec_free_context(&ctx);
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return why = "Out of memory", false;
        ctx->width = pic.width;
        ctx->height = pic.height;
        ctx->pix_fmt = format;
        ctx->time_base = {1, 1};
        ctx->framerate = {1, 1};
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->color_primaries = pic.primaries;
        ctx->color_trc = pic.transfer;
        ctx->colorspace = pic.matrix;
        ctx->gop_size = 1;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        if (std::string(name) == "librav1e") av_dict_set_int(&opts, "qp", crf * 4, 0);
        else av_dict_set_int(&opts, "crf", crf, 0);
        if (std::string(name) == "libaom-av1") av_dict_set(&opts, "still-picture", "1", 0);
        const int rc = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (rc >= 0) {
            opened = true;
            break;
        }
        why = std::string("The AV1 encoder (") + name + ") would not take a " + std::to_string(pic.width) + " x " + std::to_string(pic.height) +
              " picture";
    }
    if (!opened) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0) return why = "Cannot write " + path, false;
    st->time_base = ctx->time_base;
    if (peakNits > 0) {
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
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return why = "Cannot write " + path, false;
    if (avformat_write_header(oc, nullptr) < 0) return why = "Cannot write " + path, false;
    frame = av_frame_alloc();
    if (!frame) return why = "Out of memory", false;
    frame->format = ctx->pix_fmt;
    frame->width = pic.width;
    frame->height = pic.height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->pts = 0;
    if (av_frame_get_buffer(frame, 0) < 0) return why = "Out of memory", false;
    // (FFmpeg's planar RGB is G, B, R.)
    const int order[3] = {pic.layout == Av1Layout::Gbr ? 1 : 0, pic.layout == Av1Layout::Gbr ? 2 : 1, pic.layout == Av1Layout::Gbr ? 0 : 2};
    for (int i = 0; i < 3; ++i) {
        const std::vector<uint16_t>& src = pic.planes[order[i]];
        const int w = pic.layout == Av1Layout::Yuv420 && i > 0 ? (pic.width + 1) / 2 : pic.width;
        const int h = pic.layout == Av1Layout::Yuv420 && i > 0 ? (pic.height + 1) / 2 : pic.height;
        if (src.size() != size_t(w) * h) return why = "Cannot encode the AVIF", false;
        for (int y = 0; y < h; ++y) {
            if (pic.bits > 8) {
                auto* d = reinterpret_cast<uint16_t*>(frame->data[i] + size_t(y) * size_t(frame->linesize[i]));
                std::memcpy(d, src.data() + size_t(y) * w, size_t(w) * 2);
            } else {
                uint8_t* d = frame->data[i] + size_t(y) * size_t(frame->linesize[i]);
                for (int x = 0; x < w; ++x) d[x] = uint8_t(src[size_t(y) * w + x]);
            }
        }
    }
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
    if (!(avcodec_send_frame(ctx, frame) >= 0 && drain() && avcodec_send_frame(ctx, nullptr) >= 0 && drain() && av_write_trailer(oc) >= 0))
        return why = "Cannot encode the AVIF", false;
    why.clear();
    return true;
}

}  // namespace

bool writeAvifStill(const Image& img, const ColorSpace& space, int quality, double peakNits, double maxCll, double maxFall,
                    const std::string& path, std::string* error, bool* stated) {
    if (stated) *stated = false;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (img.empty()) return fail("Nothing to write");
    const bool hdr = space.hdr();
    std::string why;
    if (!encodeAvif(ycbcr420(img, space, hdr ? 10 : 8), quality, {"libsvtav1", "libaom-av1", "librav1e"}, path, why, hdr ? peakNits : 0,
                    maxCll, maxFall))
        return fail(why);
    if (!hdr) return true;
    // Its light levels and mastering display, as properties of the image (the file replaced whole).
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

bool writeGainMapAvif(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      double maxCll, double maxFall, const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (sdr.empty() || sdr.width != hdr.width || sdr.height != hdr.height) return fail("Nothing to write");
    if (!hdrSpace.hdr() || sdrSpace.hdr()) return fail("A gain map needs an SDR picture and an HDR one");
    if (sdrSpace.primaries != Primaries::Bt709 && sdrSpace.primaries != Primaries::P3D65)
        return fail("The SDR picture of a gain-map AVIF is in Rec.709 or Display P3 primaries");
    const int W = sdr.width, H = sdr.height;
    QTemporaryDir dir;
    if (!dir.isValid()) return fail("Cannot make a temporary folder");
    const std::string basePath = dir.filePath("base.avif").toStdString();
    std::string why;
    // The base (8-bit, tagged with sRGB's curve, which is how gain map readers linearise it), encoded first: the map is
    // made against what decoders will see of it.
    if (!encodeAvif(ycbcr420(sdr, sdrSpace, 8, AVCOL_TRC_IEC61966_2_1), quality, {"libsvtav1", "libaom-av1", "librav1e"}, basePath, why))
        return fail(why);
    const QImage seen = decodeAvifPicture(basePath, &why);
    AvifCodedItem base, map;
    if (seen.isNull() || seen.width() != W || seen.height() != H || !readAvifCodedItem(basePath, base))
        return fail("Cannot read back the AVIF's picture: " + why);
    GainMapMetadata meta;
    std::vector<float> target;
    if (!planGainMap(seen, sdrSpace.primaries, hdr, hdrSpace, meta, target)) return fail("Nothing to write");
    // The map in 8 bits: R, G and B as they are (identity matrix) where the encoder takes them, else Y'CbCr 4:4:4, else
    // 4:2:0 (SVT-AV1's only one).
    int attempt = 0;
    const QByteArray mapFile = encodeGainMap(target, W, H, [&](const QImage& m, QImage& back) -> QByteArray {
        const std::string mapPath = dir.filePath(QStringLiteral("map%1.avif").arg(attempt++)).toStdString();
        if (!encodeAvif(rgbPicture(m, Av1Layout::Gbr), std::max(quality, 90), {"libaom-av1"}, mapPath, why) &&
            !encodeAvif(rgbPicture(m, Av1Layout::Yuv444), std::max(quality, 90), {"librav1e", "libaom-av1"}, mapPath, why) &&
            !encodeAvif(rgbPicture(m, Av1Layout::Yuv420), std::max(quality, 90), {"libsvtav1", "libaom-av1", "librav1e"}, mapPath, why))
            return {};
        back = decodeAvifPicture(mapPath, &why);
        return QByteArray::fromStdString(mapPath);
    });
    if (mapFile.isEmpty() || !readAvifCodedItem(mapFile.toStdString(), map)) return fail("Cannot encode the gain map: " + why);

    // The file (as libavif writes one): the base (item 1, primary), the 'tmap' item (2) derived from it and the map,
    // and the map (3, hidden), the tone-mapped item preferred over the base in an 'altr' group; and Adobe's gain map
    // properties as XMP (4) describing the map, beside ISO 21496-1's.
    auto full = [](const char* type, int version, uint32_t flags, const QByteArray& body) {
        QByteArray b;
        put32(b, uint32_t(version) << 24 | flags);
        return box(type, b + body);
    };
    QByteArray tmap(1, '\0');  // (version 0)
    tmap += isoGainMapMetadata(meta);
    const QByteArray xmp = adobeGainMapXmp(meta);
    // Properties: the base's and the map's as FFmpeg wrote them, and the tone-mapped image's own (its size, and the
    // HDR rendition's colour and light levels).
    std::vector<QByteArray> props;
    auto index = [&](const QByteArray& p) {
        for (size_t i = 0; i < props.size(); ++i)
            if (props[i] == p) return int(i + 1);
        props.push_back(p);
        return int(props.size());
    };
    struct Assoc {
        int index;
        bool essential;
    };
    std::vector<Assoc> assoc[3];
    auto take = [&](const std::vector<QByteArray>& from, std::vector<Assoc>& to) {
        for (const QByteArray& p : from) {
            const QByteArray type = p.mid(4, 4);
            if (type == "clli" || type == "mdcv") continue;
            to.push_back({index(p), type == "av1C"});
        }
    };
    take(base.properties, assoc[0]);
    take(map.properties, assoc[2]);
    QByteArray ispe;
    put32(ispe, uint32_t(W));
    put32(ispe, uint32_t(H));
    QByteArray colr("nclx", 4);
    put16(colr, unsigned(cicpPrimaries(hdrSpace)));
    put16(colr, unsigned(cicpTransfer(hdrSpace)));
    put16(colr, 6);  // (BT.601: not coded, libavif's default)
    colr.append(char(0x80));  // full range
    QByteArray clli;
    put16(clli, unsigned(std::clamp(std::lround(maxCll), 0L, 65535L)));
    put16(clli, unsigned(std::clamp(std::lround(maxFall), 0L, 65535L)));
    assoc[1].push_back({index(full("ispe", 0, 0, ispe)), false});
    assoc[1].push_back({index(box("colr", colr)), false});
    if (maxCll > 0) assoc[1].push_back({index(box("clli", clli)), false});
    if (props.size() > 127) return fail("Too many properties");
    QByteArray ipco;
    for (const QByteArray& p : props) ipco += p;
    QByteArray ipma;
    put32(ipma, 3);
    for (int i = 0; i < 3; ++i) {
        put16(ipma, unsigned(i + 1));
        ipma.append(char(assoc[i].size()));
        for (const Assoc& a : assoc[i]) ipma.append(char((a.essential ? 0x80 : 0) | a.index));
    }
    QByteArray hdlr;
    put32(hdlr, 0);
    hdlr.append("pict", 4);
    hdlr.append(QByteArray(12, '\0'));
    hdlr.append('\0');
    QByteArray pitm;
    put16(pitm, 1);
    QByteArray iinf;
    put16(iinf, 4);
    auto infe = [&](unsigned id, const char* type, bool hidden, const QByteArray& contentType = {}) {
        QByteArray b;
        put16(b, id);
        put16(b, 0);
        b.append(type, 4);
        b.append('\0');
        if (!contentType.isEmpty()) b += contentType + '\0';
        return full("infe", 2, hidden ? 1 : 0, b);
    };
    iinf += infe(1, "av01", false) + infe(2, "tmap", false) + infe(3, "av01", true) + infe(4, "mime", false, "application/rdf+xml");
    QByteArray dimg, cdsc;
    put16(dimg, 2);
    put16(dimg, 2);
    put16(dimg, 1);
    put16(dimg, 3);
    put16(cdsc, 4);
    put16(cdsc, 1);
    put16(cdsc, 3);
    QByteArray altr;
    put32(altr, 4);  // group id (unlike any item's)
    put32(altr, 2);
    put32(altr, 2);
    put32(altr, 1);
    // Item locations: offsets into the file, known once the boxes before mdat are (their sizes do not depend on them).
    auto iloc = [&](uint32_t mdatData) {
        QByteArray b;
        b.append(char(0x44));  // offsets and lengths of four bytes
        b.append(char(0x00));  // no base offset
        put16(b, 4);
        uint32_t at = mdatData;
        for (const auto& [id, size] :
             {std::pair<unsigned, qsizetype>{1, base.data.size()}, {2, tmap.size()}, {3, map.data.size()}, {4, xmp.size()}}) {
            put16(b, id);
            put16(b, 0);  // data reference: this file
            put16(b, 1);  // one extent
            put32(b, at);
            put32(b, uint32_t(size));
            at += uint32_t(size);
        }
        return full("iloc", 0, 0, b);
    };
    QByteArray ftyp("avif", 4);
    put32(ftyp, 0);
    ftyp.append("avifmif1miaftmap", 16);
    auto metaBox = [&](uint32_t mdatData) {
        return full("meta", 0, 0,
                    full("hdlr", 0, 0, hdlr) + full("pitm", 0, 0, pitm) + iloc(mdatData) + full("iinf", 0, 0, iinf) +
                        full("iref", 0, 0, box("dimg", dimg) + box("cdsc", cdsc)) + box("iprp", box("ipco", ipco) + full("ipma", 0, 0, ipma)) +
                        box("grpl", full("altr", 0, 0, altr)));
    };
    const QByteArray head = box("ftyp", ftyp);
    const qsizetype metaSize = metaBox(0).size();
    const QByteArray payload = base.data + tmap + map.data + xmp;
    if (uint64_t(head.size() + metaSize + 8 + payload.size()) > 0xffffffffu) return fail("Too large for this AVIF writer");
    const QByteArray file = head + metaBox(uint32_t(head.size() + metaSize + 8)) + box("mdat", payload);
    QSaveFile out(QString::fromStdString(path));
    if (!out.open(QIODevice::WriteOnly) || out.write(file) != file.size() || !out.commit()) return fail("Cannot write " + path);
    return true;
}

}  // namespace montage
