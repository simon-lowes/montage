#include "GainMap.h"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "render/ColorSpace.h"

extern "C" {
#include <libavcodec/avcodec.h>
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

// Each row of the HDR rendition of `base` (RGB32) and `map` (Grayscale8 or RGB888): linear light relative to SDR white,
// three values a pixel in the base's primaries, handed to `row(y, values)` (rows in parallel).
template <typename Row>
void gainedRows(const QImage& b, const QImage& g, const GainMapMetadata& m, double headroom, const Row& row) {
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
    for (int v = 0; v < 256; ++v) lin[v] = srgbToLinear(v / 255.0);
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
                const int px[3] = {qRed(s[x]), qGreen(s[x]), qBlue(s[x])};
                for (int c = 0; c < 3; ++c) {
                    const int k = step == 1 ? 0 : c;
                    const double a = gain[c][r0[x0m * step + k]] * (1 - fx) + gain[c][r0[x1m * step + k]] * fx;
                    const double bb = gain[c][r1[x0m * step + k]] * (1 - fx) + gain[c][r1[x1m * step + k]] * fx;
                    d[c] = (lin[px[c]] + m.offsetSdr[c]) * (a * (1 - fy) + bb * fy) - m.offsetHdr[c];
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

}  // namespace

void applyGainMap(const QImage& base, const QImage& map, const GainMapMetadata& m, Image& linear, double headroom) {
    const QImage b = asRgb32(base), g = asMap(map);
    linear = Image(b.width(), b.height(), Image::Uninitialized{});
    gainedRows(b, g, m, headroom, [&](int y, const double* v) {
        float* d = linear.row(y);
        for (int x = 0; x < b.width(); ++x, v += 3, d += 4) d[0] = float(v[0]), d[1] = float(v[1]), d[2] = float(v[2]), d[3] = 1;
    });
}

std::shared_ptr<const GainMapPicture> decodeGainMapJpeg(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::shared_ptr<const GainMapPicture>();
    };
    // Decoded once per version of the file (decoders open it again and again: the media pool, thumbnails); the last
    // two kept, as a picture is large.
    static std::mutex lock;
    static std::deque<std::pair<std::string, std::shared_ptr<const GainMapPicture>>> known;
    const QFileInfo fi(QString::fromStdString(path));
    const std::string key = path + '|' + std::to_string(fi.size()) + '|' + std::to_string(fi.lastModified().toMSecsSinceEpoch());
    {
        std::lock_guard<std::mutex> g(lock);
        for (const auto& [k, picture] : known)
            if (k == key) return picture;
    }
    GainMapJpeg info;
    if (!readGainMapJpeg(path, info)) return fail("No gain map in " + path);
    if (info.metadata.baseIsHdr) return fail("A gain map to SDR from an HDR base is not read");
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
    QImage base = asRgb32(QImage::fromData(f.read(info.mapOffset), "JPEG"));
    const QImage map = asMap(f.seek(info.mapOffset) ? QImage::fromData(f.read(info.mapLength), "JPEG") : QImage());
    if (base.isNull() || map.isNull()) return fail("Cannot decode " + path + " (too large, or damaged)");
    // Into BT.2020 (from the base's primaries: Display P3 or sRGB's; a picture in others is put into Display P3 first)
    // and PQ.
    Primaries from = Primaries::Bt709;
    if (const QColorSpace cs = base.colorSpace(); cs.isValid() && cs.primaries() == QColorSpace::Primaries::DciP3D65) {
        from = Primaries::P3D65;
    } else if (cs.isValid() && cs.primaries() != QColorSpace::Primaries::SRgb) {
        base.convertToColorSpace(QColorSpace(QColorSpace::DisplayP3));
        from = Primaries::P3D65;
    }
    double m[9];
    primariesMatrix(from, Primaries::Bt2020, m);
    const ColorSpace& space = *findColorSpace("rec2100pq");
    auto out = std::make_shared<GainMapPicture>();
    out->width = base.width();
    out->height = base.height();
    out->rgba.resize(size_t(out->width) * size_t(out->height) * 4);
    std::vector<double> peaks(size_t(out->height), 0.0);
    gainedRows(base, map, info.metadata, 1e9, [&](int y, const double* s) {
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
    out->maxNits = *std::max_element(peaks.begin(), peaks.end());
    std::lock_guard<std::mutex> g(lock);
    known.emplace_back(key, out);
    if (known.size() > 2) known.pop_front();
    return out;
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

    // Each pixel's gain (log2, per channel) from the base to the HDR rendition, in the base's primaries.
    double m[9];
    primariesMatrix(hdrSpace.primaries, p3 ? Primaries::P3D65 : Primaries::Bt709, m);
    GainMapMetadata meta;
    const double off = 1 / 64.0;
    std::vector<float> gains(size_t(W) * H * 3);
    std::vector<double> lo(size_t(H), 1e9), hi(size_t(H), -1e9);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uchar* s = seen.constScanLine(y);
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

    // The map in 8 bits. JPEG's Y'CbCr rounds each channel by a code value or so, a percent of light at this range, so
    // what a decoder will read back is fed back into the codes asked for (twice), keeping the closest.
    std::vector<float> target(gains.size()), ask(gains.size());
    for (size_t i = 0; i < gains.size(); ++i) ask[i] = target[i] = float(std::clamp((gains[i] - gmin) / (gmax - gmin), 0.0, 1.0) * 255);
    QByteArray mapJpeg;
    double best = 1e300;
    for (int pass = 0; pass < 3; ++pass) {
        QImage map(W, H, QImage::Format_RGB888);
        for (int y = 0; y < H; ++y) {
            uchar* d = map.scanLine(y);
            const float* a = ask.data() + size_t(y) * W * 3;
            for (int i = 0; i < W * 3; ++i) d[i] = uchar(std::clamp(std::lround(a[i]), 0L, 255L));
        }
        const QByteArray bytes = jpeg444(map, quality >= 95 ? 1 : 2);
        const QImage back = QImage::fromData(bytes, "JPEG").convertToFormat(QImage::Format_RGB888);
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
        if (error < best) best = error, mapJpeg = bytes;
    }
    if (mapJpeg.isEmpty()) return fail("Cannot encode the gain map");

    // The map's picture: its XMP (Adobe's gain map properties) and the ISO 21496-1 metadata.
    QByteArray xmp2 = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Montage\">"
                      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                      "<rdf:Description xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\"";
    xmp2 += " hdrgm:GainMapMin=\"" + number(gmin) + "\" hdrgm:GainMapMax=\"" + number(gmax) + "\" hdrgm:Gamma=\"1\"";
    xmp2 += " hdrgm:OffsetSDR=\"" + number(off) + "\" hdrgm:OffsetHDR=\"" + number(off) + "\"";
    xmp2 += " hdrgm:HDRCapacityMin=\"0\" hdrgm:HDRCapacityMax=\"" + number(meta.hdrCapacityMax) + "\"";
    xmp2 += " hdrgm:BaseRenditionIsHDR=\"False\"/></rdf:RDF></x:xmpmeta>";
    const QByteArray second = withSegments(mapJpeg, segment(0xe1, kXmpNamespace + xmp2) + segment(0xe2, kIsoNamespace + isoMetadata(meta)));

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
    QFile out(QString::fromStdString(path));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(file) != file.size()) return fail("Cannot write " + path);
    if (written) *written = meta;
    return true;
}

}  // namespace montage
