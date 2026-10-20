#include "HdrStill.h"

#include <QByteArray>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageWriter>
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

// HEIF's mastering display (mdcv: primaries G, B, R in 0.00002, white, luminance in 0.0001 nits; Montage's display at
// `peakNits`) and light level (clli, whole nits) properties.
QByteArray mdcvBox(double peakNits) {
    const Mastering m;
    QByteArray mdcv;
    for (int i : {1, 2, 0})
        for (double v : m.primaries[i]) put16(mdcv, unsigned(std::lround(v * 50000)));
    for (double v : m.white) put16(mdcv, unsigned(std::lround(v * 50000)));
    put32(mdcv, units(peakNits));
    put32(mdcv, units(0.0001));
    return box("mdcv", mdcv);
}
QByteArray clliBox(double maxCll, double maxFall) {
    QByteArray clli;
    put16(clli, unsigned(std::clamp(std::lround(maxCll), 0L, 65535L)));
    put16(clli, unsigned(std::clamp(std::lround(maxFall), 0L, 65535L)));
    return box("clli", clli);
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

    // The new properties: mdcv and clli.
    const QByteArray added = mdcvBox(peakNits) + clliBox(maxCll, maxFall);
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

// The ipco indexes (from 1) of the properties of a HEIF's primary item (pitm), from its meta box's contents.
bool primaryProperties(const QByteArray& b, const Box& meta, std::vector<unsigned>& out) {
    std::vector<Box> kids, iprp;
    if (meta.body + 4 > meta.end() || !boxes(b, meta.body + 4, meta.end(), kids)) return false;
    const Box *pitm = find(kids, "pitm"), *p = find(kids, "iprp");
    if (!pitm || pitm->body + 6 > pitm->end() || !p || !boxes(b, p->body, p->end(), iprp)) return false;
    const bool longIds = b[pitm->body] != 0;
    if (longIds && pitm->body + 8 > pitm->end()) return false;
    const uint32_t primary = longIds ? get32(b, pitm->body + 4) : get16(b, pitm->body + 4);
    const Box* ipma = find(iprp, "ipma");
    if (!ipma || ipma->body + 8 > ipma->end()) return false;
    const int version = uint8_t(b[ipma->body]);
    const bool wide = get32(b, ipma->body) & 1;
    qsizetype at = ipma->body + 8;
    for (uint32_t n = get32(b, ipma->body + 4); n > 0; --n) {
        if (at + (version < 1 ? 3 : 5) > ipma->end()) return false;
        const uint32_t id = version < 1 ? get16(b, at) : get32(b, at);
        at += version < 1 ? 2 : 4;
        const int count = uint8_t(b[at++]);
        if (at + count * (wide ? 2 : 1) > ipma->end()) return false;
        for (int i = 0; i < count; ++i, at += wide ? 2 : 1)
            if (id == primary) out.push_back(wide ? get16(b, at) & 0x7fff : uint8_t(b[at]) & 0x7f);
    }
    return true;
}

// An AVIF's (or other HEIF's) primary item's properties: its colr of type nclx, clli and mdcv. (Not another item's: a gain-map still's tone-mapped item has the HDR rendition's.)
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
    std::vector<unsigned> primary;
    if (meta.isEmpty() || !boxes(meta, 0, meta.size(), top) || top.size() != 1 || !avifProperties(meta, top[0], props) ||
        !primaryProperties(meta, top[0], primary))
        return false;
    bool colour = false;
    for (size_t i = 0; i < props.size(); ++i) {
        const Box& p = props[i];
        const qsizetype n = p.end() - p.body;
        if (std::find(primary.begin(), primary.end(), unsigned(i + 1)) == primary.end()) continue;
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
    std::string why;
    return writeStillFile(path, out, why) || fail(why);
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

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

// Encodes `pic` with the first of `encoders` that takes it into a file at `path` by FFmpeg's `muxer` (an AVIF, or an MP4
// for the HEVC picture of a HEIF); with a mastering display of `peakNits` and light levels when `peakNits` > 0.
bool encodeStill(const Av1Picture& pic, int quality, std::initializer_list<const char*> encoders, const char* muxer, const std::string& path,
                 std::string& why, double peakNits = 0, double maxCll = 0, double maxFall = 0) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, muxer, path.c_str()) < 0 || !oc)
        return why = std::string("This build of FFmpeg cannot write ") + muxer, false;
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
    const std::string kind = std::string(muxer) == "avif" ? "AVIF" : "HEVC picture";
    why = std::string("No ") + (std::string(muxer) == "avif" ? "AV1" : "HEVC") + " encoder in this build of FFmpeg";
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
        else if (std::string(name) == "libx265") av_dict_set_int(&opts, "crf", std::clamp(crf * 51 / 63, 0, 51), 0);
        else av_dict_set_int(&opts, "crf", crf, 0);
        if (std::string(name) == "libx265") av_dict_set(&opts, "x265-params", "log-level=error:info=0", 0);
        if (std::string(name) == "libaom-av1") av_dict_set(&opts, "still-picture", "1", 0);
        const int rc = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (rc >= 0) {
            opened = true;
            break;
        }
        why = std::string("The encoder (") + name + ") would not take a " + std::to_string(pic.width) + " x " + std::to_string(pic.height) +
              (pic.bits > 8 ? " 10-bit" : "") + " picture" +
              (pic.bits > 8 && std::string(name) == "libx265" ? " (this build of x265 may have no 10-bit support)" : "");
    }
    if (!opened) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0) return why = "Cannot add the picture to " + path, false;
    st->time_base = ctx->time_base;
    if (std::string(muxer) != "avif") st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');  // (parameter sets in hvcC alone)
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
    int rc = 0;
    if (!(oc->oformat->flags & AVFMT_NOFILE) && (rc = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0)
        return why = "Cannot write " + path + ": " + averr(rc), false;
    if ((rc = avformat_write_header(oc, nullptr)) < 0) return why = "Cannot write the " + kind + "'s header: " + averr(rc), false;
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
        if (src.size() != size_t(w) * h) return why = "Cannot encode the " + kind, false;
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
            if (r < 0) return rc = r, false;
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            if ((rc = av_interleaved_write_frame(oc, pkt)) < 0) return false;
        }
    };
    if (!((rc = avcodec_send_frame(ctx, frame)) >= 0 && drain() && (rc = avcodec_send_frame(ctx, nullptr)) >= 0 && drain() &&
          (rc = av_write_trailer(oc)) >= 0))
        return why = "Cannot encode the " + kind + ": " + averr(rc), false;
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
    // Encoded into a temporary file (so a failed encode leaves any file at `path` as it was), then written whole, an HDR
    // one with its light levels and mastering display as properties of the image.
    QTemporaryDir dir;
    if (!dir.isValid()) return fail("Cannot make a temporary folder");
    const std::string encoded = dir.filePath("still.avif").toStdString();
    if (!encodeStill(ycbcr420(img, space, hdr ? 10 : 8), quality, {"libsvtav1", "libaom-av1", "librav1e"}, "avif", encoded, why,
                     hdr ? peakNits : 0, maxCll, maxFall))
        return fail(why);
    QFile in(QString::fromStdString(encoded));
    if (!in.open(QIODevice::ReadOnly)) return fail("Cannot read back " + encoded);
    QByteArray bytes = in.readAll();
    in.close();
    const bool added = hdr && addAvifLightLevels(bytes, peakNits, maxCll, maxFall);
    if (!writeStillFile(path, bytes, why)) return fail(why);
    if (stated) *stated = added;
    return true;
}

namespace {

// A full box: version, flags and contents.
QByteArray fullBox(const char* type, int version, uint32_t flags, const QByteArray& body) {
    QByteArray b;
    put32(b, uint32_t(version) << 24 | flags);
    return box(type, b + body);
}

// A coded picture's own properties for a HEIF item: its size (ispe), its channels' bits (pixi) and its colour (colr
// nclx, H.273 code points, full range).
std::vector<QByteArray> pictureProperties(int width, int height, int bits, int primaries, int transfer, int matrix) {
    QByteArray ispe, pixi, colr("nclx", 4);
    put32(ispe, uint32_t(width));
    put32(ispe, uint32_t(height));
    pixi.append(char(3)).append(char(bits)).append(char(bits)).append(char(bits));
    put16(colr, unsigned(primaries));
    put16(colr, unsigned(transfer));
    put16(colr, unsigned(matrix));
    colr.append(char(0x80));
    return {fullBox("ispe", 0, 0, ispe), fullBox("pixi", 0, 0, pixi), box("colr", colr)};
}

// The HEVC picture and its decoder configuration (hvcC) of an MP4 FFmpeg wrote with one frame, as a HEIF item.
bool readMp4Hevc(const std::string& path, AvifCodedItem& out) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly) || f.size() > (1ll << 30)) return false;
    const QByteArray b = f.readAll();
    std::vector<Box> level;
    if (!boxes(b, 0, b.size(), level)) return false;
    const Box* at = nullptr;
    for (const char* type : {"moov", "trak", "mdia", "minf", "stbl"}) {
        at = find(level, type);
        if (!at || !boxes(b, at->body, at->end(), level)) return false;
    }
    const Box *stsd = find(level, "stsd"), *stsz = find(level, "stsz"), *stco = find(level, "stco"), *co64 = find(level, "co64");
    std::vector<Box> entries, children;
    if (!stsd || !stsz || (!stco && !co64) || !boxes(b, stsd->body + 8, stsd->end(), entries) || entries.size() != 1 ||
        (entries[0].type != "hvc1" && entries[0].type != "hev1") || entries[0].body + 78 > entries[0].end() ||
        !boxes(b, entries[0].body + 78, entries[0].end(), children))
        return false;
    const Box* hvcC = find(children, "hvcC");
    if (!hvcC || stsz->body + 12 > stsz->end() || (stco ? stco->body + 12 : co64->body + 16) > (stco ? stco : co64)->end()) return false;
    const uint64_t size = get32(b, stsz->body + 4) ? get32(b, stsz->body + 4) : (stsz->body + 16 <= stsz->end() ? get32(b, stsz->body + 12) : 0);
    const uint64_t offset = stco ? get32(b, stco->body + 8) : getN(b, co64->body + 8, 8);
    if (size == 0 || offset > uint64_t(b.size()) || size > uint64_t(b.size()) - offset) return false;
    out = AvifCodedItem();
    out.type = "hvc1";
    out.data = b.mid(qsizetype(offset), qsizetype(size));
    out.properties = {b.mid(hvcC->at, hvcC->size)};
    return true;
}

// A HEIF file's parts: items (ids from 1, in order) with their data and property boxes (essential or not), references
// between them, and an 'altr' group (entities in order of preference).
struct HeifItem {
    QByteArray type;
    QByteArray data;
    std::vector<std::pair<QByteArray, bool>> properties;
    bool hidden = false;
    QByteArray contentType;  // (a 'mime' item's)
};
struct HeifRef {
    const char* type;
    unsigned from;
    std::vector<unsigned> to;
};

// The file: ftyp (`brands`: major, then compatible ones), meta (hdlr, pitm, iloc, iinf, iref, iprp, grpl) and mdat, with
// item 1 the primary.
bool heifFile(const std::vector<QByteArray>& brands, const std::vector<HeifItem>& items, const std::vector<HeifRef>& refs,
              const std::vector<unsigned>& altr, QByteArray& file, std::string& why) {
    if (items.size() >= 65535) return why = "Too many pictures for this HEIF writer", false;
    std::vector<QByteArray> props;
    auto index = [&](const QByteArray& p) {
        for (size_t i = 0; i < props.size(); ++i)
            if (props[i] == p) return int(i + 1);
        props.push_back(p);
        return int(props.size());
    };
    QByteArray ipma;
    put32(ipma, uint32_t(std::count_if(items.begin(), items.end(), [](const HeifItem& i) { return !i.properties.empty(); })));
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].properties.empty()) continue;
        put16(ipma, unsigned(i + 1));
        ipma.append(char(items[i].properties.size()));
        for (const auto& [p, essential] : items[i].properties) ipma.append(char((essential ? 0x80 : 0) | index(p)));
    }
    if (props.size() > 127) return why = "Too many properties", false;
    QByteArray ipco;
    for (const QByteArray& p : props) ipco += p;
    QByteArray hdlr;
    put32(hdlr, 0);
    hdlr.append("pict", 4);
    hdlr.append(QByteArray(12, '\0'));
    hdlr.append('\0');
    QByteArray pitm, iinf, iref, grpl;
    put16(pitm, 1);
    put16(iinf, unsigned(items.size()));
    for (size_t i = 0; i < items.size(); ++i) {
        QByteArray b;
        put16(b, unsigned(i + 1));
        put16(b, 0);
        b.append(items[i].type.left(4));
        b.append('\0');
        if (!items[i].contentType.isEmpty()) b += items[i].contentType + '\0';
        iinf += fullBox("infe", 2, items[i].hidden ? 1 : 0, b);
    }
    for (const HeifRef& r : refs) {
        QByteArray b;
        put16(b, r.from);
        put16(b, unsigned(r.to.size()));
        for (unsigned to : r.to) put16(b, to);
        iref += box(r.type, b);
    }
    if (!altr.empty()) {
        QByteArray g;
        put32(g, uint32_t(items.size() + 1));  // group id (unlike any item's)
        put32(g, uint32_t(altr.size()));
        for (unsigned e : altr) put32(g, e);
        grpl = box("grpl", fullBox("altr", 0, 0, g));
    }
    // Item locations: offsets into the file, known once the boxes before mdat are (their sizes do not depend on them).
    auto iloc = [&](uint32_t mdatData) {
        QByteArray b;
        b.append(char(0x44));  // offsets and lengths of four bytes
        b.append(char(0x00));  // no base offset
        put16(b, unsigned(items.size()));
        uint32_t at = mdatData;
        for (size_t i = 0; i < items.size(); ++i) {
            put16(b, unsigned(i + 1));
            put16(b, 0);  // data reference: this file
            put16(b, 1);  // one extent
            put32(b, at);
            put32(b, uint32_t(items[i].data.size()));
            at += uint32_t(items[i].data.size());
        }
        return fullBox("iloc", 0, 0, b);
    };
    QByteArray ftyp = brands.front();
    put32(ftyp, 0);
    for (size_t i = 1; i < brands.size(); ++i) ftyp += brands[i];
    auto metaBox = [&](uint32_t mdatData) {
        return fullBox("meta", 0, 0,
                       fullBox("hdlr", 0, 0, hdlr) + fullBox("pitm", 0, 0, pitm) + iloc(mdatData) + fullBox("iinf", 0, 0, iinf) +
                           (refs.empty() ? QByteArray() : fullBox("iref", 0, 0, iref)) +
                           box("iprp", box("ipco", ipco) + fullBox("ipma", 0, 0, ipma)) + grpl);
    };
    const QByteArray head = box("ftyp", ftyp);
    const qsizetype metaSize = metaBox(0).size();
    QByteArray payload;
    for (const HeifItem& i : items) payload += i.data;
    if (uint64_t(head.size() + metaSize + 8 + payload.size()) > 0xffffffffu) return why = "Too large for this HEIF writer", false;
    file = head + metaBox(uint32_t(head.size() + metaSize + 8)) + box("mdat", payload);
    return true;
}

// A coded picture's properties as a HEIF item takes them: av1C, hvcC and clap essential, any light levels left out (the
// item's own are added).
std::vector<std::pair<QByteArray, bool>> itemProperties(const std::vector<QByteArray>& from) {
    std::vector<std::pair<QByteArray, bool>> out;
    for (const QByteArray& p : from) {
        const QByteArray type = p.mid(4, 4);
        if (type != "clli" && type != "mdcv") out.push_back({p, type == "av1C" || type == "hvcC" || type == "clap"});
    }
    return out;
}

// `pic` at 4:2:0 filled out to an even width and height with its last column and row (its Cb and Cr planes already
// cover them).
Av1Picture evenPicture(Av1Picture pic) {
    const int W = pic.width, H = pic.height, EW = W + (W & 1), EH = H + (H & 1);
    if (pic.layout != Av1Layout::Yuv420 || (EW == W && EH == H)) return pic;
    std::vector<uint16_t> y(size_t(EW) * EH);
    for (int r = 0; r < EH; ++r)
        for (int c = 0; c < EW; ++c) y[size_t(r) * EW + c] = pic.planes[0][size_t(std::min(r, H - 1)) * W + size_t(std::min(c, W - 1))];
    pic.planes[0] = std::move(y);
    pic.width = EW, pic.height = EH;
    return pic;
}

// A clean aperture (clap) keeping the top left `width` x `height` of a picture `codedWidth` x `codedHeight`.
QByteArray clapBox(int width, int height, int codedWidth, int codedHeight) {
    QByteArray b;
    put32(b, uint32_t(width)), put32(b, 1), put32(b, uint32_t(height)), put32(b, 1);
    // (Its centre's offset from the picture's, in halves.)
    put32(b, uint32_t(int32_t(width - codedWidth))), put32(b, 2), put32(b, uint32_t(int32_t(height - codedHeight))), put32(b, 2);
    return box("clap", b);
}

// A descriptive property added to an item's: before any transformative one (clap, irot, imir), which come last.
void addDescriptive(std::vector<std::pair<QByteArray, bool>>& properties, const std::pair<QByteArray, bool>& p) {
    auto at = std::find_if(properties.begin(), properties.end(), [](const std::pair<QByteArray, bool>& q) {
        const QByteArray type = q.first.mid(4, 4);
        return type == "clap" || type == "irot" || type == "imir";
    });
    properties.insert(at, p);
}

// HEVC into a HEIF item: encoded into an MP4 in `dir`, whose picture and hvcC are taken, with the picture's own
// properties. (At 4:2:0 an odd width or height is filled out to an even one, which x265 needs, and cropped back by a
// clean aperture.)
bool hevcItem(const Av1Picture& source, int quality, const QTemporaryDir& dir, const QString& name, AvifCodedItem& item, std::string& why) {
    const Av1Picture pic = evenPicture(source);
    const std::string mp4 = dir.filePath(name + ".mp4").toStdString();
    if (!encodeStill(pic, quality, {"libx265"}, "mp4", mp4, why)) return false;
    if (!readMp4Hevc(mp4, item)) return why = "Cannot read back the HEVC picture", false;
    for (QByteArray& p : pictureProperties(pic.width, pic.height, pic.bits, int(pic.primaries), int(pic.transfer), int(pic.matrix)))
        item.properties.push_back(p);
    if (pic.width != source.width || pic.height != source.height) item.properties.push_back(clapBox(source.width, source.height, pic.width, pic.height));
    return true;
}

// The SDR base and its gain map to `hdr` as AV1 or HEVC pictures, and the HEIF file around them (as libavif writes one):
// the base (item 1, primary), the 'tmap' item (2) derived from it and the map, and the map (3, hidden), the tone-mapped
// item preferred over the base in an 'altr' group; and Adobe's gain map properties as XMP (4) describing the map,
// beside ISO 21496-1's.
bool writeGainMapStill(bool hevc, const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                       double maxCll, double maxFall, const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const char* kind = hevc ? "HEIF" : "AVIF";
    if (sdr.empty() || sdr.width != hdr.width || sdr.height != hdr.height) return fail("Nothing to write");
    if (!hdrSpace.hdr() || sdrSpace.hdr()) return fail("A gain map needs an SDR picture and an HDR one");
    if (sdrSpace.primaries != Primaries::Bt709 && sdrSpace.primaries != Primaries::P3D65)
        return fail(std::string("The SDR picture of a gain-map ") + kind + " is in Rec.709 or Display P3 primaries");
    const int W = sdr.width, H = sdr.height;
    QTemporaryDir dir;
    if (!dir.isValid()) return fail("Cannot make a temporary folder");
    std::string why;
    // The base (8-bit, tagged with sRGB's curve, which is how gain map readers linearise it), encoded first: the map is
    // made against what decoders will see of it.
    AvifCodedItem base, map;
    QImage seen;
    const Av1Picture basePicture = ycbcr420(sdr, sdrSpace, 8, AVCOL_TRC_IEC61966_2_1);
    auto readBack = [&](const AvifCodedItem& item, QImage& back) {
        // (Through a file of its own, as a reader sees it.)
        QByteArray file;
        const std::string at = dir.filePath(QStringLiteral("back%1.heif").arg(qHash(item.data))).toStdString();
        QFile f(QString::fromStdString(at));
        if (!heifFile({"mif1", "mif1"}, {{item.type, item.data, itemProperties(item.properties)}}, {}, {}, file, why) ||
            !f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(file) != file.size())
            return false;
        f.close();
        back = decodeHeifPicture(at, &why);
        return !back.isNull();
    };
    if (hevc) {
        if (!hevcItem(basePicture, quality, dir, "base", base, why) || !readBack(base, seen)) return fail(why);
    } else {
        const std::string basePath = dir.filePath("base.avif").toStdString();
        if (!encodeStill(basePicture, quality, {"libsvtav1", "libaom-av1", "librav1e"}, "avif", basePath, why)) return fail(why);
        seen = decodeHeifPicture(basePath, &why);
        if (!seen.isNull() && !readHeifCodedItem(basePath, base)) seen = QImage();
    }
    if (seen.isNull() || seen.width() != W || seen.height() != H) return fail("Cannot read back the picture: " + why);
    GainMapMetadata meta;
    std::vector<float> target;
    if (!planGainMap(seen, sdrSpace.primaries, hdr, hdrSpace, meta, target)) return fail("Nothing to write");
    // The map in 8 bits: in AV1, R, G and B as they are (identity matrix) where the encoder takes them, else Y'CbCr
    // 4:4:4, else 4:2:0 (SVT-AV1's only one); in HEVC, Y'CbCr 4:2:0 (Main profile, which every HEVC decoder takes). (Each
    // attempt kept; the closest is used.)
    std::vector<AvifCodedItem> attempts;
    const QByteArray chosen = encodeGainMap(target, W, H, [&](const QImage& m, QImage& back) -> QByteArray {
        AvifCodedItem item;
        const QString name = QStringLiteral("map%1").arg(attempts.size());
        if (hevc) {
            if (!hevcItem(rgbPicture(m, Av1Layout::Yuv420), std::max(quality, 90), dir, name, item, why) || !readBack(item, back)) return {};
        } else {
            const std::string mapPath = dir.filePath(name + ".avif").toStdString();
            if (!encodeStill(rgbPicture(m, Av1Layout::Gbr), std::max(quality, 90), {"libaom-av1"}, "avif", mapPath, why) &&
                !encodeStill(rgbPicture(m, Av1Layout::Yuv444), std::max(quality, 90), {"librav1e", "libaom-av1"}, "avif", mapPath, why) &&
                !encodeStill(rgbPicture(m, Av1Layout::Yuv420), std::max(quality, 90), {"libsvtav1", "libaom-av1", "librav1e"}, "avif", mapPath, why))
                return {};
            back = decodeHeifPicture(mapPath, &why);
            if (!readHeifCodedItem(mapPath, item)) return {};
        }
        attempts.push_back(item);
        return QByteArray::number(qulonglong(attempts.size() - 1));
    });
    bool ok = false;
    const qulonglong which = chosen.toULongLong(&ok);
    if (!ok || which >= attempts.size()) return fail("Cannot encode the gain map: " + why);
    map = attempts[which];

    // The tone-mapped image's data (version 0, ISO 21496-1's metadata) and its own properties: its size, and the HDR
    // rendition's colour (matrix BT.601, which libavif's tone mapping re-encodes with) and light levels.
    QByteArray tmap(1, '\0');
    tmap += isoGainMapMetadata(meta);
    std::vector<std::pair<QByteArray, bool>> tmapProperties;
    for (const QByteArray& p : pictureProperties(W, H, 10, cicpPrimaries(hdrSpace), cicpTransfer(hdrSpace), 6))
        if (p.mid(4, 4) != "pixi") tmapProperties.push_back({p, false});
    if (maxCll > 0) tmapProperties.push_back({clliBox(maxCll, maxFall), false});
    std::vector<HeifItem> items = {{base.type, base.data, itemProperties(base.properties)},
                                   {"tmap", tmap, tmapProperties},
                                   {map.type, map.data, itemProperties(map.properties), true},
                                   {"mime", adobeGainMapXmp(meta), {}, false, "application/rdf+xml"}};
    const std::vector<QByteArray> brands = hevc ? std::vector<QByteArray>{"heic", "mif1", "heic", "heix", "tmap"}
                                                : std::vector<QByteArray>{"avif", "avif", "mif1", "miaf", "tmap"};
    QByteArray file;
    if (!heifFile(brands, items, {{"dimg", 2, {1, 3}}, {"cdsc", 4, {3}}}, {2, 1}, file, why)) return fail(why);
    return writeStillFile(path, file, why) || fail(why);
}

}  // namespace

bool writeHeifStill(const Image& img, const ColorSpace& space, int quality, double peakNits, double maxCll, double maxFall,
                    const std::string& path, std::string* error, int tile) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (img.empty()) return fail("Nothing to write");
    const bool hdr = space.hdr();
    const int W = img.width, H = img.height, bits = hdr ? 10 : 8;
    if (W > 65535 || H > 65535 || int64_t(W) * H > (1 << 27)) return fail("Too large for this HEIF writer");  // (as Montage reads them)
    // (A picture larger than HEVC level 5.1 takes, which phones and graphics cards decode, goes in tiles of 2048.)
    if (tile <= 0 && (int64_t(W) * H > 8912896 || std::max(W, H) > 8192)) tile = 2048;
    tile += tile & 1;  // (tiles at 4:2:0 even)
    QTemporaryDir dir;
    if (!dir.isValid()) return fail("Cannot make a temporary folder");
    std::string why;
    std::vector<std::pair<QByteArray, bool>> light;
    if (hdr) light = {{mdcvBox(peakNits), false}, {clliBox(maxCll, maxFall), false}};
    std::vector<HeifItem> items;
    std::vector<HeifRef> refs;
    if (tile <= 0 || (tile >= W && tile >= H)) {
        AvifCodedItem item;
        if (!hevcItem(ycbcr420(img, space, bits), quality, dir, "still", item, why)) return fail(why);
        items.push_back({item.type, item.data, itemProperties(item.properties)});
        for (const auto& p : light) addDescriptive(items[0].properties, p);
    } else {
        // A grid (item 1) of tiles the same size, those at the right and bottom edges filled out with their last
        // column and row.
        const int columns = (W + tile - 1) / tile, rows = (H + tile - 1) / tile;
        if (columns > 256 || rows > 256) return fail("Too large for this HEIF writer");
        QByteArray grid;
        grid.append(char(0)).append(char(0)).append(char(rows - 1)).append(char(columns - 1));
        put16(grid, unsigned(W));
        put16(grid, unsigned(H));
        items.push_back({"grid", grid, {}});
        for (const QByteArray& p : pictureProperties(W, H, bits, 0, 0, 0))
            if (p.mid(4, 4) != "colr") items[0].properties.push_back({p, false});
        refs.push_back({"dimg", 1, {}});
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                Image part(tile, tile, Image::Uninitialized{});
                for (int y = 0; y < tile; ++y) {
                    const float* s = img.row(std::min(H - 1, r * tile + y));
                    float* d = part.row(y);
                    for (int x = 0; x < tile; ++x) std::memcpy(d + x * 4, s + std::min(W - 1, c * tile + x) * 4, 4 * sizeof(float));
                }
                AvifCodedItem item;
                if (!hevcItem(ycbcr420(part, space, bits), quality, dir, QStringLiteral("tile%1").arg(items.size()), item, why)) return fail(why);
                items.push_back({item.type, item.data, itemProperties(item.properties), true});
                refs[0].to.push_back(unsigned(items.size()));
            }
        // (The grid's colour, as its tiles'.)
        for (const auto& [p, essential] : items[1].properties)
            if (p.mid(4, 4) == "colr") addDescriptive(items[0].properties, {p, false});
        for (const auto& p : light) items[0].properties.push_back(p);
    }
    QByteArray file;
    if (!heifFile(hdr ? std::vector<QByteArray>{"heix", "mif1", "heix"} : std::vector<QByteArray>{"heic", "mif1", "heic"}, items, refs, {},
                  file, why))
        return fail(why);
    return writeStillFile(path, file, why) || fail(why);
}

bool writeGainMapAvif(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      double maxCll, double maxFall, const std::string& path, std::string* error) {
    return writeGainMapStill(false, sdr, sdrSpace, hdr, hdrSpace, quality, maxCll, maxFall, path, error);
}

bool writeGainMapHeif(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      double maxCll, double maxFall, const std::string& path, std::string* error) {
    return writeGainMapStill(true, sdr, sdrSpace, hdr, hdrSpace, quality, maxCll, maxFall, path, error);
}

}  // namespace montage
