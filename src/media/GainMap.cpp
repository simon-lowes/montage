#include "GainMap.h"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QRect>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "render/ColorSpace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace montage {

namespace {

constexpr double kSdrWhite = 203;  // nits: SDR white in HDR (BT.2408), linear 1.0
const QByteArray kXmpNamespace("http://ns.adobe.com/xap/1.0/\0", 29);
const QByteArray kIsoNamespace("urn:iso:std:iso:ts:21496:-1\0", 28);
const QByteArray kMpfSignature("MPF\0", 4);

double srgbToLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// BT.709's inverse OETF (libavif's reading of a picture tagged with BT.709's transfer).
double bt709ToLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v < 4.5 * 0.018053968510807 ? v / 4.5 : std::pow((v + 0.09929682680944) / 1.09929682680944, 1 / 0.45);
}

uint32_t be32(const QByteArray& b, qsizetype at) {
    return uint32_t(uint8_t(b[at])) << 24 | uint32_t(uint8_t(b[at + 1])) << 16 | uint32_t(uint8_t(b[at + 2])) << 8 | uint8_t(b[at + 3]);
}
unsigned be16(const QByteArray& b, qsizetype at) { return unsigned(uint8_t(b[at])) << 8 | uint8_t(b[at + 1]); }
void put16(QByteArray& b, unsigned v) {
    b.append(char(v >> 8));
    b.append(char(v));
}
void put32(QByteArray& b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xffff);
}

// A JPEG's segments before its picture data, read from the file at `from` (the rest skipped): the marker, where its
// contents start in the file, and (for APP1 and APP2, where metadata is) the contents.
struct Segment {
    int marker;
    qint64 at;
    QByteArray data;
};
bool segments(QFile& f, qint64 from, std::vector<Segment>& out) {
    out.clear();
    uchar m[4];
    if (!f.seek(from) || f.read(reinterpret_cast<char*>(m), 2) != 2 || m[0] != 0xff || m[1] != 0xd8) return false;
    while (out.size() < 1000) {
        if (f.read(reinterpret_cast<char*>(m), 2) != 2 || m[0] != 0xff) return !out.empty();
        int marker = m[1];
        while (marker == 0xff) {  // (fill)
            if (!f.getChar(reinterpret_cast<char*>(m))) return false;
            marker = m[0];
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (marker == 0xd9 || f.read(reinterpret_cast<char*>(m + 2), 2) != 2) return !out.empty();
        const qint64 len = (qint64(m[2]) << 8 | m[3]) - 2, at = f.pos();
        if (len < 0 || at + len > f.size()) return false;
        Segment s{marker, at, {}};
        if (marker == 0xe1 || marker == 0xe2) {
            s.data = f.read(len);
            if (s.data.size() != len) return false;
        } else if (!f.seek(at + len)) {
            return false;
        }
        out.push_back(std::move(s));
        if (marker == 0xda) return true;  // the picture data follows
    }
    return !out.empty();
}

bool startsWith(const Segment& s, const QByteArray& sig) { return s.data.startsWith(sig); }

// XMP's properties of Adobe's gain map namespace, under whatever prefix the file binds it to.
const QString kHdrgm = QStringLiteral("http://ns.adobe.com/hdr-gain-map/1.0/");
QString xmpPrefix(const QString& xmp, const QString& uri) {
    const QRegularExpression re(QStringLiteral("xmlns:([A-Za-z_][\\w.-]*)\\s*=\\s*[\"']") + QRegularExpression::escape(uri) + "[\"']");
    const QRegularExpressionMatch match = re.match(xmp);
    return match.hasMatch() ? match.captured(1) : QString();
}
// A property's text, as an attribute or an element ("" without it).
QString xmpText(const QString& xmp, const QString& prefix, const char* name) {
    const QString p = QRegularExpression::escape(prefix + ":" + name);
    QRegularExpressionMatch match = QRegularExpression("\\b" + p + "\\s*=\\s*([\"'])(.*?)\\1").match(xmp);
    if (match.hasMatch()) return match.captured(2);
    match = QRegularExpression("<" + p + ">(.*?)</" + p + ">", QRegularExpression::DotMatchesEverythingOption).match(xmp);
    return match.hasMatch() ? match.captured(1) : QString();
}
// A value, or a value per channel (rdf:Seq for three).
std::vector<double> xmpValues(const QString& xmp, const QString& prefix, const char* name) {
    const QString body = xmpText(xmp, prefix, name);
    if (body.isEmpty()) return {};
    if (!body.contains(QLatin1String("<rdf:li"))) return {body.trimmed().toDouble()};
    std::vector<double> v;
    QRegularExpressionMatchIterator it =
        QRegularExpression("<rdf:li[^>]*>(.*?)</rdf:li>", QRegularExpression::DotMatchesEverythingOption).globalMatch(body);
    while (it.hasNext() && v.size() < 3) v.push_back(it.next().captured(1).trimmed().toDouble());
    return v;
}

void setChannels(double (&dst)[3], const std::vector<double>& v) {
    if (v.empty()) return;
    for (int c = 0; c < 3; ++c) dst[c] = v[std::min(size_t(c), v.size() - 1)];
}

bool readXmpMetadata(const QByteArray& bytes, GainMapMetadata& m) {
    const QString xmp = QString::fromUtf8(bytes), g = xmpPrefix(xmp, kHdrgm);
    if (g.isEmpty() || xmpText(xmp, g, "Version").isEmpty()) return false;
    // (gain map minimum and maximum, and the capacities, are log2; gamma and offsets as they are)
    std::vector<double> v = xmpValues(xmp, g, "GainMapMax");
    if (v.empty()) return false;
    setChannels(m.gainMapMax, v);
    for (double& x : m.gainMapMin) x = 0;
    setChannels(m.gainMapMin, xmpValues(xmp, g, "GainMapMin"));
    for (double& x : m.gamma) x = 1;
    setChannels(m.gamma, xmpValues(xmp, g, "Gamma"));
    for (double& o : m.offsetSdr) o = 1 / 64.0;
    setChannels(m.offsetSdr, xmpValues(xmp, g, "OffsetSDR"));
    for (double& o : m.offsetHdr) o = 1 / 64.0;
    setChannels(m.offsetHdr, xmpValues(xmp, g, "OffsetHDR"));
    v = xmpValues(xmp, g, "HDRCapacityMin");
    m.hdrCapacityMin = v.empty() ? 0 : v[0];
    v = xmpValues(xmp, g, "HDRCapacityMax");
    m.hdrCapacityMax = v.empty() ? m.gainMapMax[0] : v[0];
    m.baseIsHdr = xmpText(xmp, g, "BaseRenditionIsHDR").trimmed().compare(QLatin1String("True"), Qt::CaseInsensitive) == 0;
    m.useBaseColorSpace = true;
    return true;
}

// What the primary picture's XMP says: whether it names a gain map (Adobe's namespace, or a container item for one),
// and the map's length when its directory gives it (else 0).
bool xmpNamesGainMap(const QByteArray& bytes, long long& length) {
    const QString xmp = QString::fromUtf8(bytes);
    const QRegularExpressionMatch item = QRegularExpression("\\bSemantic\\s*=\\s*[\"']GainMap[\"']").match(xmp);
    if (item.hasMatch()) {
        // (the item's element: Length may come before Semantic or after)
        const qsizetype open = xmp.lastIndexOf('<', item.capturedStart()), close = xmp.indexOf('>', item.capturedEnd());
        const QRegularExpressionMatch len =
            QRegularExpression("\\bLength\\s*=\\s*[\"'](\\d+)[\"']").match(xmp.mid(open, close < 0 ? -1 : close - open));
        if (open >= 0 && len.hasMatch()) length = len.captured(1).toLongLong();
    }
    return item.hasMatch() || !xmpPrefix(xmp, kHdrgm).isEmpty();
}

// ISO 21496-1's metadata: versions, then (unless it is the primary picture's marker alone) flags and fractions.
bool readIsoMetadata(const QByteArray& b, qsizetype at, qsizetype end, GainMapMetadata& m) {
    if (at + 5 > end || be16(b, at) != 0) return false;  // (minimum version 0)
    const int flags = uint8_t(b[at + 4]);
    at += 5;
    const int channels = flags & 0x80 ? 3 : 1;
    const bool common = flags & 0x08;
    const qsizetype need = common ? 12 + qsizetype(channels) * 20 : 16 + qsizetype(channels) * 40;
    if (at + need > end) return false;
    auto s32 = [&](qsizetype p) { return double(int32_t(be32(b, p))); };
    auto u32 = [&](qsizetype p) { return double(be32(b, p)); };
    auto frac = [](double n, double d) { return d != 0 ? n / d : 0.0; };
    double fields[3][5];
    if (common) {
        const double d = u32(at);
        m.hdrCapacityMin = frac(u32(at + 4), d);
        m.hdrCapacityMax = frac(u32(at + 8), d);
        for (int c = 0; c < channels; ++c) {
            const qsizetype p = at + 12 + qsizetype(c) * 20;
            fields[c][0] = frac(s32(p), d);
            fields[c][1] = frac(s32(p + 4), d);
            fields[c][2] = frac(u32(p + 8), d);
            fields[c][3] = frac(s32(p + 12), d);
            fields[c][4] = frac(s32(p + 16), d);
        }
    } else {
        m.hdrCapacityMin = frac(u32(at), u32(at + 4));
        m.hdrCapacityMax = frac(u32(at + 8), u32(at + 12));
        for (int c = 0; c < channels; ++c) {
            const qsizetype p = at + 16 + qsizetype(c) * 40;
            fields[c][0] = frac(s32(p), u32(p + 4));
            fields[c][1] = frac(s32(p + 8), u32(p + 12));
            fields[c][2] = frac(u32(p + 16), u32(p + 20));
            fields[c][3] = frac(s32(p + 24), u32(p + 28));
            fields[c][4] = frac(s32(p + 32), u32(p + 36));
        }
    }
    for (int c = 0; c < 3; ++c) {
        const double* f = fields[std::min(c, channels - 1)];
        m.gainMapMin[c] = f[0];
        m.gainMapMax[c] = f[1];
        m.gamma[c] = f[2] > 0 ? f[2] : 1;
        m.offsetSdr[c] = f[3];
        m.offsetHdr[c] = f[4];
    }
    m.baseIsHdr = flags & 0x04;
    m.useBaseColorSpace = flags & 0x40;
    return true;
}

// MPF's second picture: where it starts in the file and how long it is.
bool mpfSecond(const Segment& s, long long& offset, long long& length) {
    const QByteArray& b = s.data;
    const qsizetype tiff = 4, end = b.size();
    if (tiff + 8 > end) return false;
    const bool big = b.mid(tiff, 2) == "MM";
    if (!big && b.mid(tiff, 2) != "II") return false;
    auto u16 = [&](qsizetype p) { return big ? be16(b, p) : unsigned(uint8_t(b[p])) | unsigned(uint8_t(b[p + 1])) << 8; };
    auto u32 = [&](qsizetype p) {
        return big ? be32(b, p) : uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
    };
    const qsizetype ifd = tiff + qsizetype(u32(tiff + 4));
    if (ifd < tiff || ifd + 2 > end) return false;
    const unsigned count = u16(ifd);
    for (unsigned i = 0; i < count && ifd + 2 + qsizetype(i + 1) * 12 <= end; ++i) {
        const qsizetype tag = ifd + 2 + qsizetype(i) * 12;
        if (u16(tag) != 0xb002) continue;
        const qsizetype entries = tiff + qsizetype(u32(tag + 8));
        const uint32_t n = u32(tag + 4) / 16;
        if (n < 2 || entries < tiff || entries + 32 > end) return false;
        length = u32(entries + 16 + 4);
        offset = s.at + tiff + u32(entries + 16 + 8);  // (from the TIFF header)
        return length > 0;
    }
    return false;
}

// A display picture's light per channel, relative to SDR white, in its own primaries.
void hdrLinear(const ColorSpace& space, const float* px, double out[3]) {
    for (int c = 0; c < 3; ++c) out[c] = std::max(0.0, codeToNits(space, std::clamp(double(px[c]), 0.0, 1.0)));
    if (space.transfer == Transfer::Hlg) {
        // HLG on a 1000-nit display: each channel's scene light times the scene luminance to the gamma less one.
        double e[3];
        for (int c = 0; c < 3; ++c) e[c] = std::pow(out[c] / 1000, 1 / 1.2);
        const double ys = 0.2627 * e[0] + 0.6780 * e[1] + 0.0593 * e[2];
        for (int c = 0; c < 3; ++c) out[c] = 1000 * std::pow(std::max(ys, 0.0), 0.2) * e[c];
    }
    for (int c = 0; c < 3; ++c) out[c] /= kSdrWhite;
}

void multiply(const double m[9], double v[3]) {
    const double r = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], g = m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                 b = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    v[0] = r, v[1] = g, v[2] = b;
}

QByteArray jpegBytes(const QImage& q, int quality) {
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "jpeg");
    w.setQuality(std::clamp(quality, 0, 100));
    w.setOptimizedWrite(true);
    return w.write(q) ? out : QByteArray();
}

// A JPEG at full chroma resolution (4:4:4) through FFmpeg's encoder: a gain map is three separate channels, which 4:2:0
// would blur into each other. `qscale` 1 (finest) to 31.
QByteArray jpeg444(const QImage& rgb, int qscale) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return {};
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    QByteArray out;
    if (ctx && frame && pkt) {
        ctx->width = rgb.width();
        ctx->height = rgb.height();
        ctx->pix_fmt = AV_PIX_FMT_YUV444P;
        ctx->color_range = AVCOL_RANGE_JPEG;  // (full range: JFIF's)
        ctx->time_base = {1, 25};
        ctx->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_BITEXACT;  // (no encoder comment)
        ctx->global_quality = FF_QP2LAMBDA * std::clamp(qscale, 1, 31);
        ctx->qmin = ctx->qmax = std::clamp(qscale, 1, 31);
        frame->format = ctx->pix_fmt;
        frame->color_range = AVCOL_RANGE_JPEG;
        frame->width = ctx->width;
        frame->height = ctx->height;
        frame->quality = ctx->global_quality;
        frame->pts = 0;
        if (avcodec_open2(ctx, codec, nullptr) >= 0 && av_frame_get_buffer(frame, 0) >= 0) {
            // JFIF's Y'CbCr (BT.601, full range).
            for (int y = 0; y < rgb.height(); ++y) {
                const uchar* s = rgb.constScanLine(y);
                uint8_t *Y = frame->data[0] + size_t(y) * size_t(frame->linesize[0]), *U = frame->data[1] + size_t(y) * size_t(frame->linesize[1]),
                        *V = frame->data[2] + size_t(y) * size_t(frame->linesize[2]);
                for (int x = 0; x < rgb.width(); ++x, s += 3) {
                    const double r = s[0], g = s[1], b = s[2];
                    Y[x] = uint8_t(std::clamp(std::lround(0.299 * r + 0.587 * g + 0.114 * b), 0L, 255L));
                    U[x] = uint8_t(std::clamp(std::lround(128 - 0.168736 * r - 0.331264 * g + 0.5 * b), 0L, 255L));
                    V[x] = uint8_t(std::clamp(std::lround(128 + 0.5 * r - 0.418688 * g - 0.081312 * b), 0L, 255L));
                }
            }
            if (avcodec_send_frame(ctx, frame) >= 0 && avcodec_send_frame(ctx, nullptr) >= 0 && avcodec_receive_packet(ctx, pkt) >= 0)
                out = QByteArray(reinterpret_cast<const char*>(pkt->data), pkt->size);
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

QByteArray segment(int marker, const QByteArray& data) {
    QByteArray s;
    s.append(char(0xff)).append(char(marker));
    put16(s, unsigned(data.size() + 2));
    return s + data;
}

// A JPEG with `extra` segments after its SOI (and its JFIF APP0, which must come first).
QByteArray withSegments(const QByteArray& jpeg, const QByteArray& extra) {
    qsizetype at = 2;
    if (jpeg.size() > 6 && uint8_t(jpeg[2]) == 0xff && uint8_t(jpeg[3]) == 0xe0) at = 4 + be16(jpeg, 4);
    return jpeg.left(at) + extra + jpeg.mid(at);
}

QByteArray number(double v) { return QByteArray::number(v, 'g', 9); }

// ISO 21496-1's metadata for one set of channels, each fraction with its own denominator (the form every reader
// takes: a common-denominator flag is in some readers' drafts only).
QByteArray isoMetadata(const GainMapMetadata& m) {
    constexpr uint32_t d = 1000000;
    auto n = [&](double v) { return uint32_t(int32_t(std::lround(std::clamp(v, -2000.0, 2000.0) * d))); };
    QByteArray b;
    put16(b, 0);  // minimum version
    put16(b, 0);  // writer version
    b.append(char((m.useBaseColorSpace ? 0x40 : 0) | (m.baseIsHdr ? 0x04 : 0)));
    for (double v : {m.hdrCapacityMin, m.hdrCapacityMax}) put32(b, n(std::max(v, 0.0))), put32(b, d);
    for (double v : {m.gainMapMin[0], m.gainMapMax[0], m.gamma[0], m.offsetSdr[0], m.offsetHdr[0]}) put32(b, n(v)), put32(b, d);
    return b;
}

}  // namespace

bool readGainMapJpeg(const std::string& path, GainMapJpeg& out) {
    out = GainMapJpeg();
    QFile f(QString::fromStdString(path));
    std::vector<Segment> segs;
    if (!f.open(QIODevice::ReadOnly) || !segments(f, 0, segs)) return false;
    bool marked = false;
    long long length = 0;
    for (const Segment& s : segs) {
        if (s.marker == 0xe1 && startsWith(s, kXmpNamespace)) marked = xmpNamesGainMap(s.data.mid(kXmpNamespace.size()), length) || marked;
        else if (s.marker == 0xe2 && startsWith(s, kIsoNamespace)) marked = true;
        else if (s.marker == 0xe2 && startsWith(s, kMpfSignature)) mpfSecond(s, out.mapOffset, out.mapLength);
    }
    if (!marked) return false;
    const qint64 size = f.size();
    if (out.mapLength <= 0 && length > 0 && length < size) out.mapOffset = size - length, out.mapLength = length;
    if (out.mapOffset <= 0 || out.mapLength <= 0 || out.mapOffset + out.mapLength > size) return false;
    if (!segments(f, out.mapOffset, segs)) return false;
    GainMapMetadata xmp;
    for (const Segment& s : segs) {
        if (s.marker == 0xe2 && startsWith(s, kIsoNamespace) && readIsoMetadata(s.data, kIsoNamespace.size(), s.data.size(), out.metadata))
            out.iso = true;
        else if (s.marker == 0xe1 && startsWith(s, kXmpNamespace) && readXmpMetadata(s.data.mid(kXmpNamespace.size()), xmp))
            out.xmp = true;
    }
    if (!out.iso && out.xmp) out.metadata = xmp;
    return out.iso || out.xmp;
}

namespace {

// A base's code value (0-1) as linear light, by its H.273 transfer: sRGB's curve (13, and unstated), BT.709's (1, 6, 14,
// 15), gamma 2.2 (4) or 2.8 (5), or linear (8). False for others.
bool baseTransferKnown(int transfer) {
    return transfer == 2 || transfer == 13 || transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15 || transfer == 4 ||
           transfer == 5 || transfer == 8;
}
double baseToLinear(int transfer, double v) {
    switch (transfer) {
    case 1: case 6: case 14: case 15: return bt709ToLinear(v);
    case 4: return std::pow(v, 2.2);
    case 5: return std::pow(v, 2.8);
    case 8: return v;
    default: return srgbToLinear(v);
    }
}

// Each row of the HDR rendition of `base` (RGB32, its code values by `transfer`) and `map` (Grayscale8 or RGB888): linear
// light relative to SDR white, three values a pixel, handed to `row(y, values)` (rows in parallel). The map applies in
// the base's primaries, else (`toMath`, a 3 x 3 matrix) in those the base is converted to first, which the values are in.
template <typename Row>
void gainedRows(const QImage& b, int transfer, const double* toMath, const QImage& g, const GainMapMetadata& m, double headroom,
                const Row& row) {
    const int W = b.width(), H = b.height(), mw = g.width(), mh = g.height(), step = g.format() == QImage::Format_Grayscale8 ? 1 : 3;
    if (W <= 0 || H <= 0 || mw <= 0 || mh <= 0) return;
    // How much of the map applies: all of it from its capacity's maximum up.
    double weight = 1;
    if (headroom < m.hdrCapacityMax)
        weight = m.hdrCapacityMax > m.hdrCapacityMin
                     ? std::clamp((headroom - m.hdrCapacityMin) / (m.hdrCapacityMax - m.hdrCapacityMin), 0.0, 1.0)
                     : 0.0;
    double gain[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v) {
            const double t = std::pow(v / 255.0, 1 / (m.gamma[c] > 0 ? m.gamma[c] : 1.0));
            gain[c][v] = std::exp2((m.gainMapMin[c] + (m.gainMapMax[c] - m.gainMapMin[c]) * t) * weight);
        }
    double lin[256];
    for (int v = 0; v < 256; ++v) lin[v] = baseToLinear(transfer, v / 255.0);
    parallelRows(H, [&](int y0, int y1) {
        std::vector<double> out(size_t(W) * 3);
        for (int y = y0; y < y1; ++y) {
            const QRgb* s = reinterpret_cast<const QRgb*>(b.constScanLine(y));
            // The map's sample here (bilinear, pixel centres aligned).
            const double my = std::clamp((y + 0.5) * mh / H - 0.5, 0.0, double(mh - 1));
            const int y0m = int(my), y1m = std::min(y0m + 1, mh - 1);
            const double fy = my - y0m;
            const uchar *r0 = g.constScanLine(y0m), *r1 = g.constScanLine(y1m);
            double* d = out.data();
            for (int x = 0; x < W; ++x, d += 3) {
                const double mx = std::clamp((x + 0.5) * mw / W - 0.5, 0.0, double(mw - 1));
                const int x0m = int(mx), x1m = std::min(x0m + 1, mw - 1);
                const double fx = mx - x0m;
                double sdr[3] = {lin[qRed(s[x])], lin[qGreen(s[x])], lin[qBlue(s[x])]};
                if (toMath) multiply(toMath, sdr);
                for (int c = 0; c < 3; ++c) {
                    const int k = step == 1 ? 0 : c;
                    const double a = gain[c][r0[x0m * step + k]] * (1 - fx) + gain[c][r0[x1m * step + k]] * fx;
                    const double bb = gain[c][r1[x0m * step + k]] * (1 - fx) + gain[c][r1[x1m * step + k]] * fx;
                    d[c] = (sdr[c] + m.offsetSdr[c]) * (a * (1 - fy) + bb * fy) - m.offsetHdr[c];
                }
            }
            row(y, out.data());
        }
    });
}

QImage asRgb32(const QImage& q) {
    return q.format() == QImage::Format_RGB32 || q.format() == QImage::Format_ARGB32 ? q : q.convertToFormat(QImage::Format_RGB32);
}
QImage asMap(const QImage& q) {
    return q.format() == QImage::Format_Grayscale8 || q.format() == QImage::Format_RGB888 ? q : q.convertToFormat(QImage::Format_RGB888);
}

// The HDR rendition at full boost as 16-bit PQ (BT.2020) code values, from a base in `from` primaries (the map applied
// in `math` ones).
std::shared_ptr<GainMapPicture> hdrRendition(const QImage& base, int transfer, Primaries from, Primaries math, const QImage& map,
                                             const GainMapMetadata& meta) {
    double m[9], toMath[9];
    primariesMatrix(math, Primaries::Bt2020, m);
    primariesMatrix(from, math, toMath);
    const ColorSpace& space = *findColorSpace("rec2100pq");
    auto out = std::make_shared<GainMapPicture>();
    out->width = base.width();
    out->height = base.height();
    out->rgba.resize(size_t(out->width) * size_t(out->height) * 4);
    std::vector<double> peaks(size_t(out->height), 0.0);
    gainedRows(base, transfer, from == math ? nullptr : toMath, map, meta, 1e9, [&](int y, const double* s) {
        uint16_t* d = out->rgba.data() + size_t(y) * size_t(out->width) * 4;
        for (int x = 0; x < out->width; ++x, s += 3, d += 4) {
            double v[3] = {s[0], s[1], s[2]};
            multiply(m, v);
            for (int c = 0; c < 3; ++c) {
                const double nits = std::clamp(v[c] * kSdrWhite, 0.0, 10000.0);
                peaks[size_t(y)] = std::max(peaks[size_t(y)], nits);
                d[c] = uint16_t(std::lround(std::clamp(nitsToCode(space, nits), 0.0, 1.0) * 65535));
            }
            d[3] = 65535;
        }
    });
    out->maxNits = out->height > 0 ? *std::max_element(peaks.begin(), peaks.end()) : 0;
    return out;
}

// A file's HDR rendition decoded once per version of it (decoders open it again and again: the media pool,
// thumbnails); the last two kept, as a picture is large.
std::shared_ptr<const GainMapPicture> cachedRendition(const std::string& path, const char* kind,
                                                      const std::function<std::shared_ptr<const GainMapPicture>()>& decode) {
    static std::mutex lock;
    static std::deque<std::pair<std::string, std::shared_ptr<const GainMapPicture>>> known;
    const QFileInfo fi(QString::fromStdString(path));
    const std::string key = std::string(kind) + '|' + path + '|' + std::to_string(fi.size()) + '|' + std::to_string(fi.lastModified().toMSecsSinceEpoch());
    {
        std::lock_guard<std::mutex> g(lock);
        for (const auto& [k, picture] : known)
            if (k == key) return picture;
    }
    std::shared_ptr<const GainMapPicture> out = decode();
    if (!out) return out;
    std::lock_guard<std::mutex> g(lock);
    for (const auto& [k, picture] : known)
        if (k == key) return picture;  // (decoded meanwhile on another thread)
    known.emplace_back(key, out);
    if (known.size() > 2) known.pop_front();
    return out;
}

}  // namespace

void applyGainMap(const QImage& base, const QImage& map, const GainMapMetadata& m, Image& linear, double headroom) {
    const QImage b = asRgb32(base), g = asMap(map);
    linear = Image(b.width(), b.height(), Image::Uninitialized{});
    gainedRows(b, 13, nullptr, g, m, headroom, [&](int y, const double* v) {
        float* d = linear.row(y);
        for (int x = 0; x < b.width(); ++x, v += 3, d += 4) d[0] = float(v[0]), d[1] = float(v[1]), d[2] = float(v[2]), d[3] = 1;
    });
}

std::shared_ptr<const GainMapPicture> decodeGainMapJpeg(const std::string& path, std::string* error) {
    std::string why;
    auto out = cachedRendition(path, "gainmap", [&]() -> std::shared_ptr<const GainMapPicture> {
        GainMapJpeg info;
        if (!readGainMapJpeg(path, info)) return why = "No gain map in " + path, nullptr;
        if (info.metadata.baseIsHdr) return why = "A gain map to SDR from an HDR base is not read", nullptr;
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly)) return why = "Cannot read " + path, nullptr;
        QImage base = asRgb32(QImage::fromData(f.read(info.mapOffset), "JPEG"));
        const QImage map = asMap(f.seek(info.mapOffset) ? QImage::fromData(f.read(info.mapLength), "JPEG") : QImage());
        if (base.isNull() || map.isNull()) return why = "Cannot decode " + path + " (too large, or damaged)", nullptr;
        // From the base's primaries: Display P3 or sRGB's; a picture in others is put into Display P3 first.
        Primaries from = Primaries::Bt709;
        if (const QColorSpace cs = base.colorSpace(); cs.isValid() && cs.primaries() == QColorSpace::Primaries::DciP3D65) {
            from = Primaries::P3D65;
        } else if (cs.isValid() && cs.primaries() != QColorSpace::Primaries::SRgb) {
            base.convertToColorSpace(QColorSpace(QColorSpace::DisplayP3));
            from = Primaries::P3D65;
        }
        return hdrRendition(base, 13, from, from, map, info.metadata);
    });
    if (!out && error) *error = why;
    return out;
}

bool planGainMap(const QImage& seen, Primaries basePrimaries, const Image& hdr, const ColorSpace& hdrSpace, GainMapMetadata& meta,
                 std::vector<float>& target) {
    const QImage base = seen.convertToFormat(QImage::Format_RGB888);
    const int W = base.width(), H = base.height();
    if (W != hdr.width || H != hdr.height || W <= 0 || H <= 0) return false;
    // Each pixel's gain (log2, per channel) from the base to the HDR rendition, in the base's primaries.
    double m[9];
    primariesMatrix(hdrSpace.primaries, basePrimaries, m);
    meta = GainMapMetadata();
    const double off = 1 / 64.0;
    std::vector<float> gains(size_t(W) * H * 3);
    std::vector<double> lo(size_t(H), 1e9), hi(size_t(H), -1e9);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uchar* s = base.constScanLine(y);
            const float* h = hdr.row(y);
            float* g = gains.data() + size_t(y) * W * 3;
            for (int x = 0; x < W; ++x, s += 3, h += 4, g += 3) {
                double v[3];
                hdrLinear(hdrSpace, h, v);
                multiply(m, v);
                for (int c = 0; c < 3; ++c) {
                    const double gain = std::log2((std::max(v[c], 0.0) + off) / (srgbToLinear(s[c] / 255.0) + off));
                    g[c] = float(gain);
                    lo[size_t(y)] = std::min(lo[size_t(y)], gain);
                    hi[size_t(y)] = std::max(hi[size_t(y)], gain);
                }
            }
        }
    });
    double gmin = *std::min_element(lo.begin(), lo.end()), gmax = *std::max_element(hi.begin(), hi.end());
    if (gmax - gmin < 1e-3) gmax = gmin + 1e-3;
    for (int c = 0; c < 3; ++c) {
        meta.gainMapMin[c] = gmin;
        meta.gainMapMax[c] = gmax;
        meta.gamma[c] = 1;
        meta.offsetSdr[c] = meta.offsetHdr[c] = off;
    }
    meta.hdrCapacityMin = 0;
    meta.hdrCapacityMax = std::max(gmax, 1e-3);
    // (In place: at the largest sizes a second full-size copy is gigabytes.)
    target = std::move(gains);
    for (float& t : target) t = float(std::clamp((t - gmin) / (gmax - gmin), 0.0, 1.0) * 255);
    return true;
}

QByteArray encodeGainMap(const std::vector<float>& target, int W, int H, const std::function<QByteArray(const QImage&, QImage&)>& roundTrip) {
    // An encoder's Y'CbCr and quantising move each channel by a code value or so, a percent of light at this range, so
    // what a decoder will read back is fed back into the codes asked for (twice), keeping the closest.
    if (target.size() != size_t(W) * size_t(H) * 3) return {};
    std::vector<float> ask = target;
    QByteArray out;
    double best = 1e300;
    for (int pass = 0; pass < 3; ++pass) {
        QImage map(W, H, QImage::Format_RGB888);
        for (int y = 0; y < H; ++y) {
            uchar* d = map.scanLine(y);
            const float* a = ask.data() + size_t(y) * W * 3;
            for (int i = 0; i < W * 3; ++i) d[i] = uchar(std::clamp(std::lround(a[i]), 0L, 255L));
        }
        QImage back;
        const QByteArray bytes = roundTrip(map, back);
        back = back.convertToFormat(QImage::Format_RGB888);
        if (bytes.isEmpty() || back.size() != map.size()) break;
        double error = 0;
        for (int y = 0; y < H; ++y) {
            const uchar* r = back.constScanLine(y);
            const size_t row = size_t(y) * W * 3;
            for (int i = 0; i < W * 3; ++i) {
                const float e = target[row + size_t(i)] - r[i];
                error += double(e) * e;
                ask[row + size_t(i)] = std::clamp(ask[row + size_t(i)] + e, -8.0f, 263.0f);
            }
        }
        if (error < best) best = error, out = bytes;
    }
    return out;
}

QByteArray isoGainMapMetadata(const GainMapMetadata& m) { return isoMetadata(m); }

QByteArray adobeGainMapXmp(const GainMapMetadata& m) {
    QByteArray xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Montage\">"
                     "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                     "<rdf:Description xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\"";
    xmp += " hdrgm:GainMapMin=\"" + number(m.gainMapMin[0]) + "\" hdrgm:GainMapMax=\"" + number(m.gainMapMax[0]) + "\"";
    xmp += " hdrgm:Gamma=\"" + number(m.gamma[0]) + "\"";
    xmp += " hdrgm:OffsetSDR=\"" + number(m.offsetSdr[0]) + "\" hdrgm:OffsetHDR=\"" + number(m.offsetHdr[0]) + "\"";
    xmp += " hdrgm:HDRCapacityMin=\"" + number(m.hdrCapacityMin) + "\" hdrgm:HDRCapacityMax=\"" + number(m.hdrCapacityMax) + "\"";
    xmp += QByteArray(" hdrgm:BaseRenditionIsHDR=\"") + (m.baseIsHdr ? "True" : "False") + "\"/></rdf:RDF></x:xmpmeta>";
    return xmp;
}

// AVIF (HEIF) with a gain map: a tone-mapped derived item ('tmap', whose data is ISO 21496-1's metadata after a version
// byte) made from the base picture and the gain map ('dimg', in that order), the base the primary item.
namespace {

struct HeifBox {
    qint64 at = 0, size = 0, body = 0;
    QByteArray type;
    qint64 end() const { return at + size; }
};
bool heifBoxes(const QByteArray& b, qint64 from, qint64 to, std::vector<HeifBox>& out) {
    out.clear();
    while (from < to && out.size() < 10000) {
        if (to - from < 8) return false;
        uint64_t size = be32(b, from);
        qint64 head = 8;
        if (size == 1) {
            if (to - from < 16) return false;
            size = uint64_t(be32(b, from + 8)) << 32 | be32(b, from + 12);
            head = 16;
        } else if (size == 0) {
            size = uint64_t(to - from);
        }
        if (size < uint64_t(head) || size > uint64_t(to - from)) return false;
        out.push_back({from, qint64(size), from + head, b.mid(from + 4, 4)});
        from += qint64(size);
    }
    return from == to;
}
const HeifBox* heifFind(const std::vector<HeifBox>& list, const char* type) {
    for (const HeifBox& x : list)
        if (x.type == type) return &x;
    return nullptr;
}
uint64_t beN(const QByteArray& b, qint64 at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | uint8_t(b[at + i]);
    return v;
}

struct AvifItem {
    uint32_t id = 0;
    QByteArray type;
    int method = 0;  // where its data is: 0 the file, 1 the meta box's idat
    uint64_t base = 0;
    std::vector<std::pair<uint64_t, uint64_t>> extents;  // offset, length
    std::vector<int> properties;                         // its property boxes (in the file's), in order
    bool associated = false;                             // (listed in ipma, which lists an item once)
    std::vector<uint32_t> dimg;                          // the items it is derived from
};
struct AvifFile {
    uint32_t primary = 0;
    std::map<uint32_t, AvifItem> items;
    std::vector<QByteArray> properties;  // the property boxes, whole
    QByteArray idat;
    const AvifItem* item(uint32_t id) const {
        const auto it = items.find(id);
        return it == items.end() ? nullptr : &it->second;
    }
    // An item's first property of a type.
    const QByteArray* property(const AvifItem& item, const char* type) const {
        for (int i : item.properties)
            if (properties[size_t(i)].mid(4, 4) == type) return &properties[size_t(i)];
        return nullptr;
    }
};

// An AVIF's items: its meta box read (the top-level boxes skipped by their sizes, so the pictures are not read).
bool readAvifFile(QFile& f, AvifFile& out) {
    out = AvifFile();
    QByteArray meta;
    for (qint64 at = 0; at + 8 <= f.size() && meta.isEmpty();) {
        if (!f.seek(at)) return false;
        const QByteArray head = f.read(16);
        if (head.size() < 8) return false;
        uint64_t size = be32(head, 0);
        if (size == 1 && head.size() == 16) size = uint64_t(be32(head, 8)) << 32 | be32(head, 12);
        else if (size == 0) size = uint64_t(f.size() - at);
        if (size < 8 || size > uint64_t(f.size() - at)) return false;
        if (at == 0 && head.mid(4, 4) != "ftyp") return false;
        if (head.mid(4, 4) == "meta") {
            if (size > (16u << 20) || !f.seek(at)) return false;
            meta = f.read(qint64(size));
            if (meta.size() != qint64(size)) return false;
        }
        at += qint64(size);
    }
    std::vector<HeifBox> top, kids;
    if (meta.isEmpty() || !heifBoxes(meta, 0, meta.size(), top) || top.size() != 1 || top[0].body + 4 > top[0].end() ||
        !heifBoxes(meta, top[0].body + 4, top[0].end(), kids))  // (a full box)
        return false;
    // (An item by id, added when new; null past a few thousand, more than a still has.)
    auto byId = [&](uint32_t id) -> AvifItem* {
        if (out.items.size() >= 4096 && !out.items.count(id)) return nullptr;
        AvifItem& i = out.items[id];
        i.id = id;
        return &i;
    };
    if (const HeifBox* p = heifFind(kids, "pitm"); p && p->body + 6 <= p->end())
        out.primary = uint8_t(meta[p->body]) == 0 ? uint32_t(beN(meta, p->body + 4, 2)) : (p->body + 8 <= p->end() ? be32(meta, p->body + 4) : 0);
    if (const HeifBox* idat = heifFind(kids, "idat")) out.idat = meta.mid(idat->body, idat->end() - idat->body);
    // Items and their types.
    const HeifBox* iinf = heifFind(kids, "iinf");
    if (!iinf || iinf->body + 6 > iinf->end()) return false;
    const qint64 entriesAt = iinf->body + 4 + (uint8_t(meta[iinf->body]) == 0 ? 2 : 4);
    std::vector<HeifBox> infes;
    if (entriesAt > iinf->end() || !heifBoxes(meta, entriesAt, iinf->end(), infes)) return false;
    for (const HeifBox& e : infes) {
        if (e.type != "infe" || e.body + 4 > e.end()) continue;
        const int v = uint8_t(meta[e.body]);
        if (v < 2) continue;
        const int idBytes = v == 2 ? 2 : 4;
        if (e.body + 4 + idBytes + 2 + 4 > e.end()) return false;
        AvifItem* found = byId(uint32_t(beN(meta, e.body + 4, idBytes)));
        if (!found) return false;
        AvifItem& item = *found;
        item.type = meta.mid(e.body + 4 + idBytes + 2, 4);
    }
    // Where their data is.
    if (const HeifBox* iloc = heifFind(kids, "iloc")) {
        qint64 at = iloc->body;
        if (at + 8 > iloc->end()) return false;
        const int v = uint8_t(meta[at]);
        if (v > 2) return false;
        const int offsetSize = uint8_t(meta[at + 4]) >> 4, lengthSize = uint8_t(meta[at + 4]) & 15;
        const int baseSize = uint8_t(meta[at + 5]) >> 4, indexSize = v >= 1 ? uint8_t(meta[at + 5]) & 15 : 0;
        // (Sizes of 0, 4 or 8 bytes; a length is stated, so every extent takes bytes of the box.)
        auto size = [](int n) { return n == 0 || n == 4 || n == 8; };
        if (!size(offsetSize) || lengthSize == 0 || !size(lengthSize) || !size(baseSize) || !size(indexSize)) return false;
        at += 6;
        const int idBytes = v < 2 ? 2 : 4;
        if (at + idBytes > iloc->end()) return false;
        const uint64_t count = beN(meta, at, idBytes);
        at += idBytes;
        for (uint64_t i = 0; i < count; ++i) {
            if (at + idBytes + (v >= 1 ? 2 : 0) + 2 + baseSize + 2 > iloc->end()) return false;
            AvifItem* found = byId(uint32_t(beN(meta, at, idBytes)));
            if (!found) return false;
            AvifItem& item = *found;
            at += idBytes;
            if (v >= 1) item.method = int(beN(meta, at, 2) & 15), at += 2;
            if (beN(meta, at, 2) != 0) item.method = -1;  // (in another file)
            at += 2;
            item.base = beN(meta, at, baseSize);
            at += baseSize;
            const uint64_t extents = beN(meta, at, 2);
            at += 2;
            for (uint64_t e = 0; e < extents; ++e) {
                if (at + indexSize + offsetSize + lengthSize > iloc->end()) return false;
                at += indexSize;
                const uint64_t offset = beN(meta, at, offsetSize), length = beN(meta, at + offsetSize, lengthSize);
                at += offsetSize + lengthSize;
                item.extents.push_back({offset, length});
            }
        }
    }
    // What they are derived from.
    if (const HeifBox* iref = heifFind(kids, "iref"); iref && iref->body + 4 <= iref->end()) {
        const int idBytes = uint8_t(meta[iref->body]) == 0 ? 2 : 4;
        std::vector<HeifBox> refs;
        if (!heifBoxes(meta, iref->body + 4, iref->end(), refs)) return false;
        for (const HeifBox& r : refs) {
            if (r.type != "dimg" || r.body + idBytes + 2 > r.end()) continue;
            AvifItem* found = byId(uint32_t(beN(meta, r.body, idBytes)));
            if (!found) return false;
            AvifItem& item = *found;
            const uint64_t n = beN(meta, r.body + idBytes, 2);
            if (r.body + idBytes + 2 + qint64(n) * idBytes > r.end()) return false;
            for (uint64_t k = 0; k < n; ++k) item.dimg.push_back(uint32_t(beN(meta, r.body + idBytes + 2 + qint64(k) * idBytes, idBytes)));
        }
    }
    // Their properties.
    std::vector<HeifBox> iprp, props;
    const HeifBox* iprpBox = heifFind(kids, "iprp");
    if (!iprpBox || !heifBoxes(meta, iprpBox->body, iprpBox->end(), iprp)) return false;
    const HeifBox *ipco = heifFind(iprp, "ipco"), *ipma = heifFind(iprp, "ipma");
    if (!ipco || !ipma || !heifBoxes(meta, ipco->body, ipco->end(), props)) return false;
    for (const HeifBox& b : props) out.properties.push_back(meta.mid(b.at, b.size));
    qint64 at = ipma->body;
    if (at + 8 > ipma->end()) return false;
    const int v = uint8_t(meta[at]);
    const bool wide = uint8_t(meta[at + 3]) & 1;
    const uint64_t entries = be32(meta, at + 4);
    at += 8;
    for (uint64_t e = 0; e < entries; ++e) {
        const int idBytes = v < 1 ? 2 : 4;
        if (at + idBytes + 1 > ipma->end()) return false;
        AvifItem* found = byId(uint32_t(beN(meta, at, idBytes)));
        if (!found) return false;
        AvifItem& item = *found;
        if (item.associated) return false;
        item.associated = true;
        const int n = uint8_t(meta[at + idBytes]);
        at += idBytes + 1;
        if (at + qint64(n) * (wide ? 2 : 1) > ipma->end()) return false;
        for (int k = 0; k < n; ++k) {
            const unsigned index = wide ? unsigned(beN(meta, at, 2) & 0x7fff) : unsigned(uint8_t(meta[at]) & 0x7f);
            at += wide ? 2 : 1;
            if (index >= 1 && index <= props.size()) item.properties.push_back(int(index - 1));
        }
    }
    return true;
}

// An item's data, its extents joined (up to 1 GiB).
QByteArray itemData(QFile& f, const AvifFile& a, const AvifItem& item) {
    QByteArray out;
    for (const auto& [offset, length] : item.extents) {
        if (length > (1u << 30) || uint64_t(out.size()) + length > (1u << 30)) return {};
        // (Within `limit`, without overflowing.)
        auto within = [&](uint64_t limit) { return item.base <= limit && offset <= limit - item.base && length <= limit - item.base - offset; };
        if (item.method == 1) {
            if (!within(uint64_t(a.idat.size()))) return {};
            out += a.idat.mid(qsizetype(item.base + offset), qsizetype(length));
        } else if (item.method == 0) {
            if (!within(uint64_t(f.size())) || !f.seek(qint64(item.base + offset))) return {};
            const QByteArray part = f.read(qint64(length));
            if (part.size() != qint64(length)) return {};
            out += part;
        } else {
            return {};
        }
    }
    return out;
}

// An item's colour (H.273 code points; 0 where not known): its colr property's (nclx), else what its ICC profile's
// primaries and curve are. False without either.
struct ItemColour {
    int primaries = 0, transfer = 0, matrix = 0;
    bool full = true;
    bool nclx = false;  // (an ICC profile states no matrix or range)
};
bool itemColour(const AvifFile& a, const AvifItem& item, ItemColour& out) {
    const QByteArray* icc = nullptr;
    for (int i : item.properties) {
        const QByteArray& c = a.properties[size_t(i)];
        if (c.mid(4, 4) != "colr" || c.size() < 12) continue;
        if (c.mid(8, 4) == "nclx" && c.size() >= 8 + 4 + 7) {
            out.primaries = int(beN(c, 12, 2)), out.transfer = int(beN(c, 14, 2)), out.matrix = int(beN(c, 16, 2));
            out.full = uint8_t(c[18]) & 0x80;
            out.nclx = true;
            return true;
        }
        if ((c.mid(8, 4) == "prof" || c.mid(8, 4) == "rICC") && !icc) icc = &c;
    }
    if (!icc) return false;
    const QColorSpace cs = QColorSpace::fromIccProfile(icc->mid(12));
    if (!cs.isValid()) return false;
    out = ItemColour();
    out.primaries = cs.primaries() == QColorSpace::Primaries::SRgb        ? 1
                    : cs.primaries() == QColorSpace::Primaries::DciP3D65 ? 12
                                                                          : 0;
    const double gamma = cs.gamma();
    out.transfer = cs.transferFunction() == QColorSpace::TransferFunction::SRgb     ? 13
                   : cs.transferFunction() == QColorSpace::TransferFunction::Linear ? 8
                   : cs.transferFunction() != QColorSpace::TransferFunction::Gamma ? 0
                   : std::fabs(gamma - 2.2) < 0.02                                 ? 4
                   : std::fabs(gamma - 2.8) < 0.02                                 ? 5
                                                                                   : 0;
    return true;
}

// Montage's primaries for H.273's (1 BT.709, 9 BT.2020, 12 Display P3; unstated as BT.709); false for others.
bool knownPrimaries(int cicp, Primaries& out) {
    if (cicp == 1 || cicp == 2) return out = Primaries::Bt709, true;
    if (cicp == 9) return out = Primaries::Bt2020, true;
    if (cicp == 12) return out = Primaries::P3D65, true;
    return false;
}

// A coded picture (an item's data, av01 with its av1C property or hvc1 with its hvcC) as 8-bit RGB, or as 16-bit RGBA
// (`deep`, for pictures of more than 8 bits): full-range code values in the picture's own colour.
// (Its colour from its own colr, else `inherited` (a grid's), else what the coded picture says; its size as its ispe
// says.)
QImage decodeCoded(const AvifFile& a, const AvifItem& item, const QByteArray& data, bool deep, std::string& why,
                   const ItemColour* inherited = nullptr) {
    const bool hevc = item.type == "hvc1";
    const QByteArray* config = a.property(item, hevc ? "hvcC" : "av1C");
    if ((item.type != "av01" && !hevc) || !config || config->size() < 12 || data.isEmpty()) return why = "Not a picture Montage reads", QImage();
    // (FFmpeg's own AV1 decoder needs hardware decoding: dav1d or libaom decode in software.)
    const AVCodec* codec = hevc ? avcodec_find_decoder(AV_CODEC_ID_HEVC) : avcodec_find_decoder_by_name("libdav1d");
    if (!codec && !hevc) codec = avcodec_find_decoder_by_name("libaom-av1");
    const bool software = codec;
    if (!codec && !hevc) codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
    if (!codec) return why = hevc ? "No HEVC decoder in this build of FFmpeg" : "No AV1 decoder in this build of FFmpeg", QImage();
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    QImage out;
    why = software ? "Cannot decode the picture" : "Cannot decode the AV1 picture (this build of FFmpeg has no software AV1 decoder: dav1d or libaom)";
    if (ctx && pkt && frame) {
        const int extra = int(config->size() - 8);
        ctx->extradata = static_cast<uint8_t*>(av_mallocz(size_t(extra) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (ctx->extradata) {
            std::memcpy(ctx->extradata, config->constData() + 8, size_t(extra));
            ctx->extradata_size = extra;
        }
        if (ctx->extradata && avcodec_open2(ctx, codec, nullptr) >= 0 && av_new_packet(pkt, int(data.size())) >= 0) {
            std::memcpy(pkt->data, data.constData(), size_t(data.size()));
            pkt->flags |= AV_PKT_FLAG_KEY;
            if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_send_packet(ctx, nullptr) >= 0 && avcodec_receive_frame(ctx, frame) >= 0) {
                int matrix = int(frame->colorspace);
                bool full = frame->color_range == AVCOL_RANGE_JPEG;
                // (An nclx's matrix 2, unspecified, leaves the coded picture's.)
                if (ItemColour c; itemColour(a, item, c) && c.nclx) matrix = c.matrix != 2 ? c.matrix : matrix, full = c.full;
                else if (inherited && inherited->nclx) matrix = inherited->matrix != 2 ? inherited->matrix : matrix, full = inherited->full;
                const QByteArray* ispe = a.property(item, "ispe");
                if (ispe && ispe->size() >= 20 && (int64_t(beN(*ispe, 12, 4)) != frame->width || int64_t(beN(*ispe, 16, 4)) != frame->height))
                    frame->width = 0;  // (refused below)
                const AVPixelFormat to = deep ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGB24;
                sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), frame->width, frame->height, to,
                                     SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
                if (sws && frame->width > 0) {
                    const int coefficients = matrix >= 1 && matrix <= 10 && matrix != 2 && matrix != 3 ? matrix : SWS_CS_DEFAULT;
                    sws_setColorspaceDetails(sws, sws_getCoefficients(coefficients), full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                             1 << 16, 1 << 16);
                    out = QImage(frame->width, frame->height, deep ? QImage::Format_RGBA64 : QImage::Format_RGB888);
                    uint8_t* dst[4] = {out.bits(), nullptr, nullptr, nullptr};
                    const int stride[4] = {int(out.bytesPerLine()), 0, 0, 0};
                    if (out.isNull() || sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride) != frame->height) out = QImage();
                    else why.clear();
                }
            }
        }
    }
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return out;
}

// A grid item's output size, its rows and columns of tiles and their size; false when it is not one Montage reads: its
// tiles AV1 or HEVC, all of one size (their ispe) and just covering the output.
bool gridLayout(QFile& f, const AvifFile& a, const AvifItem& item, int& width, int& height, int& rows, int& columns, QSize* tile = nullptr) {
    const QByteArray g = itemData(f, a, item);
    if (g.size() < 8 || g[0] != 0) return false;  // (version 0)
    const bool wide = uint8_t(g[1]) & 1;
    if (g.size() < (wide ? 12 : 8)) return false;
    rows = uint8_t(g[2]) + 1, columns = uint8_t(g[3]) + 1;
    const uint64_t w = beN(g, 4, wide ? 4 : 2), h = beN(g, wide ? 8 : 6, wide ? 4 : 2);
    if (w == 0 || h == 0 || w > 65535 || h > 65535 || w * h > (1u << 27) || item.dimg.size() != size_t(rows) * size_t(columns)) return false;
    int64_t tw = -1, th = -1;
    for (uint32_t id : item.dimg) {
        const AvifItem* t = a.item(id);
        const QByteArray* ispe = t ? a.property(*t, "ispe") : nullptr;
        if (!t || (t->type != "av01" && t->type != "hvc1") || !ispe || ispe->size() < 20) return false;
        const int64_t iw = int64_t(beN(*ispe, 12, 4)), ih = int64_t(beN(*ispe, 16, 4));
        if (tw < 0) tw = iw, th = ih;
        if (iw != tw || ih != th) return false;
    }
    if (tw <= 0 || th <= 0 || columns * tw < int64_t(w) || (columns - 1) * tw >= int64_t(w) || rows * th < int64_t(h) || (rows - 1) * th >= int64_t(h))
        return false;
    width = int(w), height = int(h);
    if (tile) *tile = QSize(int(tw), int(th));
    return true;
}

// A clean aperture (clap) in a picture `W` x `H`: the part it keeps; false when it is not one inside the picture.
bool cleanAperture(const QByteArray& p, int W, int H, QRect& out) {
    if (p.size() < 8 + 32) return false;
    const auto n = [&](int i) { return double(int32_t(beN(p, 8 + i * 4, 4))); };
    const auto u = [&](int i) { return double(beN(p, 8 + i * 4, 4)); };
    if (u(1) == 0 || u(3) == 0 || u(5) == 0 || u(7) == 0) return false;
    const double cw = u(0) / u(1), ch = u(2) / u(3), cx = n(4) / u(5) + (W - 1) / 2.0, cy = n(6) / u(7) + (H - 1) / 2.0;
    const long long left = std::llround(cx - (cw - 1) / 2), top = std::llround(cy - (ch - 1) / 2), w = std::llround(cw), h = std::llround(ch);
    if (w <= 0 || h <= 0 || left < 0 || top < 0 || left + w > W || top + h > H) return false;
    out = QRect(int(left), int(top), int(w), int(h));
    return true;
}

// An item's size as shown: its own (a grid's output, else its ispe), cropped to its clean aperture and turned as its
// properties say.
bool shownSize(QFile& f, const AvifFile& a, const AvifItem& item, int& width, int& height) {
    if (item.type == "grid") {
        int rows = 0, columns = 0;
        if (!gridLayout(f, a, item, width, height, rows, columns)) return false;
    } else if (const QByteArray* ispe = a.property(item, "ispe"); ispe && ispe->size() >= 20) {
        const uint64_t w = beN(*ispe, 12, 4), h = beN(*ispe, 16, 4);
        if (w > 65535 || h > 65535 || w * h > (1u << 27)) return false;  // (as large as a grid Montage reads)
        width = int(w), height = int(h);
    } else {
        return false;
    }
    for (int i : item.properties) {
        const QByteArray& p = a.properties[size_t(i)];
        if (p.mid(4, 4) == "clap") {
            QRect r;
            if (!cleanAperture(p, width, height, r)) return false;
            width = r.width(), height = r.height();
        } else if (p.mid(4, 4) == "irot" && p.size() >= 9 && (p[8] & 1)) {
            std::swap(width, height);
        }
    }
    return width > 0 && height > 0;
}

// An item's picture (decodeCoded's, a grid's tiles put together), turned and mirrored as its properties say.
// An item's transformations applied to its picture: in the order listed, a crop to its clean aperture, a turn
// anticlockwise, a mirror about a vertical or horizontal axis. False (with `why`) when they cannot be.
bool applyItemTransforms(const AvifFile& a, const AvifItem& item, QImage& out, std::string& why) {
    for (int i : item.properties) {
        const QByteArray& p = a.properties[size_t(i)];
        const QByteArray type = p.mid(4, 4);
        if (out.isNull() || p.size() < 9) continue;
        if (type == "clap") {
            QRect r;
            if (!cleanAperture(p, out.width(), out.height(), r)) return why = "The picture's clean aperture is outside it", false;
            out = out.copy(r);
        } else if (type == "irot") {
            const int turns = p[8] & 3;  // (exact turns: QTransform's -180 is not)
            if (turns == 2) out = out.transformed(QTransform().rotate(180));
            else if (turns) out = out.transformed(QTransform().rotate(turns == 1 ? 270 : 90));
        } else if (type == "imir") {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
            out = out.flipped((p[8] & 1) ? Qt::Vertical : Qt::Horizontal);
#else
            out = (p[8] & 1) ? out.mirrored(false, true) : out.mirrored(true, false);
#endif
        }
    }
    return true;
}

QImage decodeItem(QFile& f, const AvifFile& a, const AvifItem& item, bool deep, std::string& why) {
    QImage out;
    if (item.type == "grid") {
        int W = 0, H = 0, rows = 0, columns = 0;
        QSize tileSize;
        if (!gridLayout(f, a, item, W, H, rows, columns, &tileSize)) return why = "Not a picture grid Montage reads", QImage();
        ItemColour colour;
        const bool coloured = itemColour(a, item, colour);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                const AvifItem* tile = a.item(item.dimg[size_t(r) * size_t(columns) + size_t(c)]);
                if (!tile) return why = "A tile of the picture is missing", QImage();
                const QImage t = decodeCoded(a, *tile, itemData(f, a, *tile), deep, why, coloured ? &colour : nullptr);
                if (t.isNull()) return QImage();
                if (out.isNull()) {
                    out = QImage(W, H, t.format());
                    if (out.isNull()) return why = "The picture is too large", QImage();
                    out.fill(Qt::black);
                }
                if (t.size() != tileSize || t.format() != out.format()) return why = "The picture's tiles differ in size", QImage();
                const int x0 = c * tileSize.width(), y0 = r * tileSize.height();
                const int n = std::min(tileSize.width(), W - x0), bytes = t.depth() / 8;
                for (int y = 0; n > 0 && y < tileSize.height() && y0 + y < H; ++y)
                    std::memcpy(out.scanLine(y0 + y) + size_t(x0) * bytes, t.constScanLine(y), size_t(n) * bytes);
            }
    } else {
        out = decodeCoded(a, item, itemData(f, a, item), deep, why);
    }
    if (!out.isNull() && !applyItemTransforms(a, item, out, why)) return QImage();
    return out;
}

// The item a still shows (its primary), when it is a picture Montage reads: AV1, HEVC, or a grid of either.
const AvifItem* primaryPicture(const AvifFile& a) {
    const AvifItem* item = a.item(a.primary);
    if (!item || (item->type != "av01" && item->type != "hvc1" && item->type != "grid")) return nullptr;
    if (item->type == "grid") {
        const AvifItem* tile = item->dimg.empty() ? nullptr : a.item(item->dimg[0]);
        if (!tile || (tile->type != "av01" && tile->type != "hvc1")) return nullptr;
    }
    return item;
}

// A gain map Montage reads: the tone-mapped item, its base and map, its metadata, the base's primaries and transfer,
// and the primaries the map applies in. False for a file without one, or with one to SDR from an HDR base, or in
// colour Montage does not know.
struct GainMapAvif {
    const AvifItem *tmap = nullptr, *base = nullptr, *map = nullptr;
    GainMapMetadata meta;
    Primaries basePrimaries = Primaries::Bt709, mathPrimaries = Primaries::Bt709;
    int transfer = 13;
};
bool findGainMap(QFile& f, const AvifFile& a, GainMapAvif& out) {
    for (const auto& [id, i] : a.items)
        if (i.type == "tmap" && i.dimg.size() == 2) {
            out.tmap = &i;
            out.base = a.item(i.dimg[0]);
            out.map = a.item(i.dimg[1]);
            break;
        }
    auto coded = [](const AvifItem* i) { return i && (i->type == "av01" || i->type == "hvc1" || i->type == "grid"); };
    if (!out.tmap || !coded(out.base) || !coded(out.map)) return false;
    const QByteArray data = itemData(f, a, *out.tmap);
    if (data.size() < 2 || data[0] != 0 || !readIsoMetadata(data, 1, data.size(), out.meta) || out.meta.baseIsHdr) return false;  // (version 0)
    ItemColour base;
    if (!itemColour(a, *out.base, base)) base.primaries = 1, base.transfer = 13;  // (unstated: sRGB)
    if (!knownPrimaries(base.primaries, out.basePrimaries) || !baseTransferKnown(base.transfer)) return false;
    out.transfer = base.transfer;
    out.mathPrimaries = out.basePrimaries;
    if (!out.meta.useBaseColorSpace) {
        // (In the HDR rendition's primaries, as the tone-mapped item states them.)
        ItemColour hdr;
        if (!itemColour(a, *out.tmap, hdr) || !knownPrimaries(hdr.primaries, out.mathPrimaries)) return false;
    }
    return true;
}

}  // namespace

bool readHeifStill(const std::string& path, HeifStillInfo& out) {
    out = HeifStillInfo();
    QFile f(QString::fromStdString(path));
    AvifFile a;
    if (!f.open(QIODevice::ReadOnly) || !readAvifFile(f, a)) return false;
    const AvifItem* item = primaryPicture(a);
    if (!item) return false;
    const AvifItem* coded = item->type == "grid" ? a.item(item->dimg[0]) : item;
    out.grid = item->type == "grid";
    out.codec = coded->type == "hvc1" ? "hevc" : "av1";
    if (!shownSize(f, a, *item, out.width, out.height)) return false;
    ItemColour c;
    if (itemColour(a, *item, c) || itemColour(a, *coded, c)) out.primaries = c.primaries, out.transfer = c.transfer;
    const QByteArray* clli = a.property(*item, "clli");
    if (!clli) clli = a.property(*coded, "clli");
    if (clli && clli->size() >= 12) out.maxCll = double(beN(*clli, 8, 2)), out.maxFall = double(beN(*clli, 10, 2));
    GainMapAvif g;
    out.gainMap = findGainMap(f, a, g);
    if (const QByteArray* hdr = out.gainMap ? a.property(*g.tmap, "clli") : nullptr; hdr && hdr->size() >= 12)
        out.hdrMaxCll = double(beN(*hdr, 8, 2)), out.hdrMaxFall = double(beN(*hdr, 10, 2));
    return out.width > 0 && out.height > 0;
}

bool writeStillFile(const std::string& path, const QByteArray& bytes, std::string& why) {
    const QString name = QString::fromStdString(path);
    {
        // (In place only where the file beside it could not be made or put in its place: on any other failure, such
        // as a full disk, the file at `path` is left as it was.)
        QSaveFile f(name);
        const bool made = f.open(QIODevice::WriteOnly);
        if (made && f.write(bytes) == bytes.size() && f.commit()) return true;
        if (made && f.error() != QFileDevice::RenameError) return why = "Cannot write " + path + ": " + f.errorString().toStdString(), false;
    }
    QFile f(name);
    const bool opened = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (opened && f.write(bytes) == bytes.size() && f.flush()) {
        f.close();
        if (f.error() == QFileDevice::NoError) return true;
    }
    why = "Cannot write " + path + ": " + f.errorString().toStdString();
    f.close();
    if (opened) QFile::remove(name);
    return false;
}

std::shared_ptr<const GainMapPicture> decodeHeifStill(const std::string& path, std::string* error) {
    std::string why;
    auto out = cachedRendition(path, "still", [&]() -> std::shared_ptr<const GainMapPicture> {
        QFile f(QString::fromStdString(path));
        AvifFile a;
        const AvifItem* item = nullptr;
        if (!f.open(QIODevice::ReadOnly) || !readAvifFile(f, a) || !(item = primaryPicture(a))) return why = "Not a HEIF still Montage reads: " + path, nullptr;
        QImage q = decodeItem(f, a, *item, true, why);
        if (q.isNull()) return why = "Cannot decode " + path + ": " + why, nullptr;
        if (q.format() != QImage::Format_RGBA64) q = q.convertToFormat(QImage::Format_RGBA64);
        auto pic = std::make_shared<GainMapPicture>();
        pic->width = q.width();
        pic->height = q.height();
        pic->rgba.resize(size_t(q.width()) * size_t(q.height()) * 4);
        for (int y = 0; y < q.height(); ++y)
            std::memcpy(pic->rgba.data() + size_t(y) * size_t(q.width()) * 4, q.constScanLine(y), size_t(q.width()) * 4 * sizeof(uint16_t));
        return pic;
    });
    if (!out && error) *error = why;
    return out;
}

bool readGainMapHeif(const std::string& path, GainMapMetadata* metadata) {
    QFile f(QString::fromStdString(path));
    AvifFile a;
    GainMapAvif g;
    if (!f.open(QIODevice::ReadOnly) || !readAvifFile(f, a) || !findGainMap(f, a, g)) return false;
    if (metadata) *metadata = g.meta;
    return true;
}

std::shared_ptr<const GainMapPicture> decodeGainMapHeif(const std::string& path, std::string* error) {
    std::string why;
    auto out = cachedRendition(path, "gainmap", [&]() -> std::shared_ptr<const GainMapPicture> {
        QFile f(QString::fromStdString(path));
        AvifFile a;
        GainMapAvif g;
        if (!f.open(QIODevice::ReadOnly) || !readAvifFile(f, a) || !findGainMap(f, a, g)) return why = "No gain map Montage reads in " + path, nullptr;
        const QImage base = decodeItem(f, a, *g.base, false, why);
        const QImage map = base.isNull() ? QImage() : decodeItem(f, a, *g.map, false, why);
        if (base.isNull() || map.isNull()) return why = "Cannot decode " + path + ": " + why, nullptr;
        auto pic = hdrRendition(asRgb32(base), g.transfer, g.basePrimaries, g.mathPrimaries, map, g.meta);
        // The tone-mapped item's own crop, turn and mirror. (Writers give the base the same ones, so the picture
        // shown without the map matches; when the base has its own they have been applied already.)
        auto transforms = [&](const AvifItem& item) {
            for (int i : item.properties)
                if (const QByteArray t = a.properties[size_t(i)].mid(4, 4); t == "clap" || t == "irot" || t == "imir") return true;
            return false;
        };
        if (pic && g.tmap && transforms(*g.tmap) && !transforms(*g.base)) {
            QImage t = QImage(reinterpret_cast<const uchar*>(pic->rgba.data()), pic->width, pic->height, pic->width * 8,
                              QImage::Format_RGBA64)
                           .copy();
            // (Only when they keep the picture's size, as a half turn or a mirror does: the still's size is the base's,
            // and a bad crop leaves the picture as it is rather than failing it.)
            std::string ignored;
            if (applyItemTransforms(a, *g.tmap, t, ignored) && t.width() == pic->width && t.height() == pic->height) {
                t = t.convertToFormat(QImage::Format_RGBA64);
                for (int y = 0; y < t.height(); ++y)
                    std::memcpy(pic->rgba.data() + size_t(y) * size_t(t.width()) * 4, t.constScanLine(y), size_t(t.width()) * 8);
            }
        }
        return pic;
    });
    if (!out && error) *error = why;
    return out;
}

QImage decodeHeifPicture(const std::string& path, std::string* error) {
    std::string why = "Not a HEIF still";
    QFile f(QString::fromStdString(path));
    AvifFile a;
    QImage out;
    if (f.open(QIODevice::ReadOnly) && readAvifFile(f, a))
        if (const AvifItem* item = primaryPicture(a)) out = decodeItem(f, a, *item, false, why);
    if (out.isNull() && error) *error = why;
    return out;
}

bool readHeifCodedItem(const std::string& path, AvifCodedItem& out) {
    out = AvifCodedItem();
    QFile f(QString::fromStdString(path));
    AvifFile a;
    if (!f.open(QIODevice::ReadOnly) || !readAvifFile(f, a)) return false;
    const AvifItem* item = a.item(a.primary);
    if (!item || (item->type != "av01" && item->type != "hvc1") || !a.property(*item, item->type == "hvc1" ? "hvcC" : "av1C")) return false;
    out.type = item->type;
    out.data = itemData(f, a, *item);
    for (int i : item->properties) out.properties.push_back(a.properties[size_t(i)]);
    return !out.data.isEmpty();
}

bool writeGainMapJpeg(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      const std::string& path, std::string* error, GainMapMetadata* written) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (sdr.empty() || sdr.width != hdr.width || sdr.height != hdr.height) return fail("Nothing to write");
    if (!hdrSpace.hdr() || sdrSpace.hdr()) return fail("A gain map needs an SDR picture and an HDR one");
    if (sdrSpace.primaries != Primaries::Bt709 && sdrSpace.primaries != Primaries::P3D65)
        return fail("The SDR picture of a gain-map JPEG is in Rec.709 or Display P3 primaries");
    const int W = sdr.width, H = sdr.height;

    // The base, encoded first: the map is made against what decoders will see of it.
    QImage base(W, H, QImage::Format_RGBA8888);
    toRgba8(sdr, base.bits(), size_t(base.bytesPerLine()));
    base = base.convertToFormat(QImage::Format_RGB888);
    const bool p3 = sdrSpace.primaries == Primaries::P3D65;
    if (p3) base.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QByteArray baseJpeg = jpegBytes(base, quality);
    const QImage seen = QImage::fromData(baseJpeg, "JPEG").convertToFormat(QImage::Format_RGB888);
    if (baseJpeg.isEmpty() || seen.width() != W || seen.height() != H) return fail("Cannot encode the JPEG");
    GainMapMetadata meta;
    std::vector<float> target;
    if (!planGainMap(seen, p3 ? Primaries::P3D65 : Primaries::Bt709, hdr, hdrSpace, meta, target)) return fail("Nothing to write");
    // The map in 8 bits, at full chroma resolution.
    const QByteArray mapJpeg = encodeGainMap(target, W, H, [&](const QImage& map, QImage& back) {
        const QByteArray bytes = jpeg444(map, quality >= 95 ? 1 : 2);
        back = QImage::fromData(bytes, "JPEG");
        return bytes;
    });
    if (mapJpeg.isEmpty()) return fail("Cannot encode the gain map");

    // The map's picture: its XMP (Adobe's gain map properties) and the ISO 21496-1 metadata.
    const QByteArray second =
        withSegments(mapJpeg, segment(0xe1, kXmpNamespace + adobeGainMapXmp(meta)) + segment(0xe2, kIsoNamespace + isoMetadata(meta)));

    // The primary picture: XMP naming the map and its length, ISO 21496-1's version (the file has a map), and MPF.
    QByteArray xmp1 = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Montage\">"
                      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                      "<rdf:Description xmlns:Container=\"http://ns.google.com/photos/1.0/container/\" "
                      "xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\" "
                      "xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\">"
                      "<Container:Directory><rdf:Seq>"
                      "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>"
                      "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\" Item:Length=\"";
    xmp1 += QByteArray::number(second.size()) + "\"/></rdf:li></rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>";
    QByteArray iso1;
    put16(iso1, 0);
    put16(iso1, 0);
    const QByteArray before = segment(0xe1, kXmpNamespace + xmp1) + segment(0xe2, kIsoNamespace + iso1);
    // MPF (CIPA DC-007, big-endian): its index of two pictures, offsets from its TIFF header.
    auto mpf = [](uint32_t primarySize, uint32_t secondOffset, uint32_t secondSize) {
        QByteArray b = kMpfSignature + QByteArray("MM\0\x2a", 4);
        put32(b, 8);
        put16(b, 3);
        put16(b, 0xb000), put16(b, 7), put32(b, 4), b.append("0100", 4);
        put16(b, 0xb001), put16(b, 4), put32(b, 1), put32(b, 2);
        put16(b, 0xb002), put16(b, 7), put32(b, 32), put32(b, 50);
        put32(b, 0);
        put32(b, 0x20030000), put32(b, primarySize), put32(b, 0), put16(b, 0), put16(b, 0);
        put32(b, 0), put32(b, secondSize), put32(b, secondOffset), put16(b, 0), put16(b, 0);
        return segment(0xe2, b);
    };
    qsizetype appAt = 2;  // (after the JFIF APP0, as withSegments puts them)
    if (baseJpeg.size() > 6 && uint8_t(baseJpeg[2]) == 0xff && uint8_t(baseJpeg[3]) == 0xe0) appAt = 4 + be16(baseJpeg, 4);
    const qsizetype primarySize = baseJpeg.size() + before.size() + mpf(0, 0, 0).size();
    const qsizetype tiff = appAt + before.size() + 8;  // (MPF's segment last: marker, length, signature, then its header)
    const QByteArray primary =
        withSegments(baseJpeg, before + mpf(uint32_t(primarySize), uint32_t(primarySize - tiff), uint32_t(second.size())));
    const QByteArray file = primary + second;
    std::string why;
    if (!writeStillFile(path, file, why)) return fail(why);
    if (written) *written = meta;
    return true;
}

}  // namespace montage
