#include "EncodeCompare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>

#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/History.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifdef MONTAGE_WITH_VMAF
extern "C" {
#include <libvmaf/libvmaf.h>
}
#endif

namespace montage {

// ---- the metrics -------------------------------------------------------------------------------------------------

double planeMse(const QualityPicture& a, const QualityPicture& b, int plane) {
    const std::vector<uint16_t>& x = a.plane[plane];
    const std::vector<uint16_t>& y = b.plane[plane];
    const size_t n = std::min(x.size(), y.size());
    if (n == 0) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const int64_t d = int64_t(x[i]) - int64_t(y[i]);
        sum += uint64_t(d * d);
    }
    return double(sum) / double(n);
}

double psnrFromMse(double mse, int peak) {
    if (mse <= 0) return std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(double(peak) * double(peak) / mse);
}

void signalAt(const QualityPicture& p, int x, int y, double rgb[3]) {
    const double peak = p.peak();
    if (p.rgb) {
        const size_t i = size_t(y) * size_t(p.width) + size_t(x);
        // Planar RGB is stored G, B, R.
        rgb[0] = p.plane[2][i] / peak;
        rgb[1] = p.plane[0][i] / peak;
        rgb[2] = p.plane[1][i] / peak;
        return;
    }
    const int cw = p.planeWidth(1);
    const size_t yi = size_t(y) * size_t(p.width) + size_t(x);
    const size_t ci = size_t(y >> p.log2ChromaH) * size_t(cw) + size_t(x >> p.log2ChromaW);
    const double scale = double(1 << (p.bits - 8));
    double Y = p.plane[0][yi], Cb = p.plane[1][ci], Cr = p.plane[2][ci];
    if (p.fullRange) {
        Y /= peak;
        Cb = (Cb - 128 * scale) / peak;
        Cr = (Cr - 128 * scale) / peak;
    } else {
        Y = (Y - 16 * scale) / (219 * scale);
        Cb = (Cb - 128 * scale) / (224 * scale);
        Cr = (Cr - 128 * scale) / (224 * scale);
    }
    const double kr = p.kr, kb = p.kb, kg = 1 - kr - kb;
    rgb[0] = Y + 2 * (1 - kr) * Cr;
    rgb[2] = Y + 2 * (1 - kb) * Cb;
    rgb[1] = (Y - kr * rgb[0] - kb * rgb[2]) / kg;
}

double rgbPsnr(const QualityPicture& a, const QualityPicture& b) {
    const double mse = rgbMse(a, b);
    if (std::isnan(mse)) return kNoScore;
    return mse <= 0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / mse);
}

double rgbMse(const QualityPicture& a, const QualityPicture& b) {
    if (a.width != b.width || a.height != b.height || a.width <= 0) return kNoScore;
    double sum = 0;
    for (int y = 0; y < a.height; ++y) {
        double row = 0;
        for (int x = 0; x < a.width; ++x) {
            double p[3], q[3];
            signalAt(a, x, y, p);
            signalAt(b, x, y, q);
            for (int c = 0; c < 3; ++c) {
                const double d = std::clamp(p[c], 0.0, 1.0) - std::clamp(q[c], 0.0, 1.0);
                row += d * d;
            }
        }
        sum += row;
    }
    return sum / (3.0 * a.width * a.height);
}

namespace {

// FFmpeg's ssim_end1x: one 8x8 window's SSIM from its four 4x4 blocks' sums.
float ssimEnd(int64_t s1, int64_t s2, int64_t ss, int64_t s12, int max) {
    const int64_t c1 = int64_t(.01 * .01 * max * max * 64 + .5);
    const int64_t c2 = int64_t(.03 * .03 * max * max * 64 * 63 + .5);
    const int64_t vars = ss * 64 - s1 * s1 - s2 * s2;
    const int64_t covar = s12 * 64 - s1 * s2;
    return float(2 * s1 * s2 + c1) * float(2 * covar + c2) / (float(s1 * s1 + s2 * s2 + c1) * float(vars + c2));
}

}  // namespace

double planeSsim(const QualityPicture& a, const QualityPicture& b, int plane) {
    const int w = a.planeWidth(plane), h = a.planeHeight(plane);
    if (w != b.planeWidth(plane) || h != b.planeHeight(plane)) return kNoScore;
    const int bw = w >> 2, bh = h >> 2;
    if (bw < 2 || bh < 2) return kNoScore;
    const uint16_t* pa = a.plane[plane].data();
    const uint16_t* pb = b.plane[plane].data();
    // The 4x4 blocks' sums for two strips of rows, as FFmpeg keeps them.
    std::vector<std::array<int64_t, 4>> sum0(size_t(bw) + 3), sum1(size_t(bw) + 3);
    auto strip = [&](int z, std::vector<std::array<int64_t, 4>>& out) {
        for (int bx = 0; bx < bw; ++bx) {
            int64_t s1 = 0, s2 = 0, ss = 0, s12 = 0;
            for (int y = 0; y < 4; ++y) {
                const size_t row = size_t(4 * z + y) * size_t(w) + size_t(4 * bx);
                for (int x = 0; x < 4; ++x) {
                    const int64_t p = pa[row + size_t(x)], q = pb[row + size_t(x)];
                    s1 += p;
                    s2 += q;
                    ss += p * p + q * q;
                    s12 += p * q;
                }
            }
            out[size_t(bx)] = {s1, s2, ss, s12};
        }
    };
    const int max = a.peak();
    const bool deep = a.bits > 8;
    double total = 0;
    int z = 0;
    auto* s0 = &sum0;
    auto* s1 = &sum1;
    for (int y = 1; y < bh; ++y) {
        for (; z <= y; ++z) {
            std::swap(s0, s1);
            strip(z, *s0);
        }
        // FFmpeg adds a row up in float for more than 8 bits (ssim_endn_16bit) and in double for 8 (ssim_endn_8bit).
        double rowD = 0;
        float rowF = 0;
        for (int i = 0; i < bw - 1; ++i) {
            const auto& a0 = (*s0)[size_t(i)];
            const auto& a1 = (*s0)[size_t(i) + 1];
            const auto& b0 = (*s1)[size_t(i)];
            const auto& b1 = (*s1)[size_t(i) + 1];
            const float v = ssimEnd(a0[0] + a1[0] + b0[0] + b1[0], a0[1] + a1[1] + b0[1] + b1[1], a0[2] + a1[2] + b0[2] + b1[2],
                                    a0[3] + a1[3] + b0[3] + b1[3], max);
            if (deep) rowF += v;
            else rowD += v;
        }
        total += deep ? double(rowF) : rowD;
    }
    return total / (double(bw - 1) * double(bh - 1));
}

namespace {

// Wang's 11-tap Gaussian window, σ 1.5, normalised.
const std::array<double, 11>& gaussian11() {
    static const std::array<double, 11> g = [] {
        std::array<double, 11> k{};
        double sum = 0;
        for (int i = 0; i < 11; ++i) sum += k[size_t(i)] = std::exp(-double((i - 5) * (i - 5)) / (2 * 1.5 * 1.5));
        for (double& v : k) v /= sum;
        return k;
    }();
    return g;
}

// One scale's mean SSIM and mean contrast-structure term (Wang's ssim_index, 'valid' filtering: (w-10) x (h-10)
// windows). The Gaussian is applied across each row, then down the columns, a ring of the last 11 filtered rows kept
// (not whole planes of each moment: those are gigabytes at UHD across a computer's threads).
void ssimScale(const std::vector<float>& x, const std::vector<float>& y, int w, int h, double L, double& ssim, double& cs) {
    const auto& g = gaussian11();
    const int ow = w - 10, oh = h - 10;
    const size_t OW = size_t(ow);
    std::vector<double> ring(11 * 5 * OW);  // per row: x, y, x², y², xy filtered across
    auto across = [&](int row) {
        double* r = &ring[size_t(row % 11) * 5 * OW];
        const float* px = &x[size_t(row) * size_t(w)];
        const float* py = &y[size_t(row) * size_t(w)];
        for (int ox = 0; ox < ow; ++ox) {
            double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
            for (int k = 0; k < 11; ++k) {
                const double gk = g[size_t(k)], u = px[ox + k], v = py[ox + k];
                a += gk * u;
                b += gk * v;
                aa += gk * (u * u);
                bb += gk * (v * v);
                ab += gk * (u * v);
            }
            r[size_t(ox)] = a, r[OW + size_t(ox)] = b, r[2 * OW + size_t(ox)] = aa, r[3 * OW + size_t(ox)] = bb, r[4 * OW + size_t(ox)] = ab;
        }
    };
    for (int row = 0; row < 10; ++row) across(row);
    const double c1 = (0.01 * L) * (0.01 * L), c2 = (0.03 * L) * (0.03 * L);
    double sumS = 0, sumC = 0;
    for (int oy = 0; oy < oh; ++oy) {
        across(oy + 10);
        for (int ox = 0; ox < ow; ++ox) {
            double m[5] = {0, 0, 0, 0, 0};
            for (int k = 0; k < 11; ++k) {
                const double* r = &ring[size_t((oy + k) % 11) * 5 * OW + size_t(ox)];
                const double gk = g[size_t(k)];
                for (int q = 0; q < 5; ++q) m[q] += gk * r[size_t(q) * OW];
            }
            const double mx = m[0], my = m[1];
            const double vx = m[2] - mx * mx, vy = m[3] - my * my, cov = m[4] - mx * my;
            const double c = (2 * cov + c2) / (vx + vy + c2);
            sumC += c;
            sumS += (2 * mx * my + c1) / (mx * mx + my * my + c1) * c;
        }
    }
    const double n = double(ow) * double(oh);
    ssim = sumS / n;
    cs = sumC / n;
}

// Halves a picture as Wang's code does: a 2x2 mean (edges repeated), every other sample.
// (Values stay exact in floats: 16-bit codes, then means of four, four times over.)
std::vector<float> halve(const std::vector<float>& src, int w, int h, int& ow, int& oh) {
    ow = w / 2;
    oh = h / 2;
    std::vector<float> out(size_t(ow) * size_t(oh));
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            const int x0 = 2 * x, y0 = 2 * y, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
            out[size_t(y) * size_t(ow) + size_t(x)] = 0.25f * (src[size_t(y0) * size_t(w) + size_t(x0)] + src[size_t(y0) * size_t(w) + size_t(x1)] +
                                                             src[size_t(y1) * size_t(w) + size_t(x0)] + src[size_t(y1) * size_t(w) + size_t(x1)]);
        }
    return out;
}

}  // namespace

double msSsim(const QualityPicture& a, const QualityPicture& b) {
    static const double weights[5] = {0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
    if (a.width != b.width || a.height != b.height) return kNoScore;
    int w = a.width, h = a.height;
    int scales = 0;
    for (int s = 0, m = std::min(w, h); s < 5 && m >= 11; ++s, m /= 2) scales = s + 1;
    if (scales == 0) return kNoScore;
    double wsum = 0;
    for (int s = 0; s < scales; ++s) wsum += weights[s];
    std::vector<float> x(a.plane[0].begin(), a.plane[0].end()), y(b.plane[0].begin(), b.plane[0].end());
    const double L = a.peak();
    double result = 1;
    for (int s = 0; s < scales; ++s) {
        double ssim = 0, cs = 0;
        ssimScale(x, y, w, h, L, ssim, cs);
        const double wgt = weights[s] / wsum;
        if (s + 1 < scales) {
            result *= std::pow(std::max(0.0, cs), wgt);
            int nw = 0, nh = 0;
            x = halve(x, w, h, nw, nh);
            y = halve(y, w, h, nw, nh);
            w = nw;
            h = nh;
        } else {
            result *= std::pow(std::max(0.0, ssim), wgt);
        }
    }
    return result;
}

namespace {

double pqEotf(double e) {  // PQ code value -> cd/m²
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32,
                 c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1 / m2);
    return 10000 * std::pow(std::max(0.0, p - c1) / (c2 - c3 * p), 1 / m1);
}

double pqInverse(double nits) {  // cd/m² -> PQ code value
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32,
                 c3 = 2392.0 / 4096 * 32;
    const double y = std::pow(std::max(0.0, nits) / 10000, m1);
    return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

double hlgInverseOetf(double e) {  // HLG code value -> scene light 0..1
    const double a = 0.17883277, b = 1 - 4 * a, c = 0.5 - a * std::log(4 * a);
    e = std::clamp(e, 0.0, 1.0);
    return e <= 0.5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12;
}

Primaries primariesNamed(const std::string& p) {
    if (p == "bt2020") return Primaries::Bt2020;
    if (p == "p3") return Primaries::P3D65;
    return Primaries::Bt709;
}

}  // namespace

void nitsOfSignal(const QualityPicture& like, const double rgb[3], double nits[3]) {
    double light[3];
    if (like.transfer == "pq") {
        for (int c = 0; c < 3; ++c) light[c] = pqEotf(rgb[c]);
    } else if (like.transfer == "hlg") {
        // BT.2100's OOTF on a 1000-nit display: system gamma 1.2.
        double s[3];
        for (int c = 0; c < 3; ++c) s[c] = hlgInverseOetf(rgb[c]);
        const double ys = 0.2627 * s[0] + 0.6780 * s[1] + 0.0593 * s[2];
        const double g = ys > 0 ? std::pow(ys, 0.2) : 0;
        for (int c = 0; c < 3; ++c) light[c] = 1000 * g * s[c];
    } else {
        // BT.1886 with a black of 0: white at 100 cd/m².
        for (int c = 0; c < 3; ++c) light[c] = 100 * std::pow(std::clamp(rgb[c], 0.0, 1.0), 2.4);
    }
    const Primaries p = primariesNamed(like.primaries);
    if (p == Primaries::Bt2020) {
        for (int c = 0; c < 3; ++c) nits[c] = light[c];
        return;
    }
    static double m709[9], mP3[9];
    static const bool init = [] {
        primariesMatrix(Primaries::Bt709, Primaries::Bt2020, m709);
        primariesMatrix(Primaries::P3D65, Primaries::Bt2020, mP3);
        return true;
    }();
    (void)init;
    const double* m = p == Primaries::P3D65 ? mP3 : m709;
    for (int r = 0; r < 3; ++r) nits[r] = m[r * 3] * light[0] + m[r * 3 + 1] * light[1] + m[r * 3 + 2] * light[2];
}

void ictcpFromNits(const double rgb[3], double ictcp[3]) {
    // BT.2100: RGB (BT.2020) to LMS, each through PQ, then to ICtCp.
    const double L = (1688 * rgb[0] + 2146 * rgb[1] + 262 * rgb[2]) / 4096;
    const double M = (683 * rgb[0] + 2951 * rgb[1] + 462 * rgb[2]) / 4096;
    const double S = (99 * rgb[0] + 309 * rgb[1] + 3688 * rgb[2]) / 4096;
    const double l = pqInverse(L), m = pqInverse(M), s = pqInverse(S);
    ictcp[0] = 0.5 * l + 0.5 * m;
    ictcp[1] = (6610 * l - 13613 * m + 7003 * s) / 4096;
    ictcp[2] = (17933 * l - 17390 * m - 543 * s) / 4096;
}

void deltaItp(const QualityPicture& a, const QualityPicture& b, double& mean, double& largest) {
    mean = largest = kNoScore;
    if (a.width != b.width || a.height != b.height || a.width <= 0) return;
    double sum = 0, top = 0;
    for (int y = 0; y < a.height; ++y) {
        double row = 0;
        for (int x = 0; x < a.width; ++x) {
            double sa[3], sb[3], na[3], nb[3], ia[3], ib[3];
            signalAt(a, x, y, sa);
            signalAt(b, x, y, sb);
            if (sa[0] == sb[0] && sa[1] == sb[1] && sa[2] == sb[2] && a.transfer == b.transfer && a.primaries == b.primaries) continue;
            nitsOfSignal(a, sa, na);
            nitsOfSignal(b, sb, nb);
            ictcpFromNits(na, ia);
            ictcpFromNits(nb, ib);
            const double dI = ia[0] - ib[0], dT = 0.5 * (ia[1] - ib[1]), dP = ia[2] - ib[2];
            const double e = 720 * std::sqrt(dI * dI + dT * dT + dP * dP);
            row += e;
            top = std::max(top, e);
        }
        sum += row;
    }
    mean = sum / (double(a.width) * double(a.height));
    largest = top;
}

unsigned qualityMetricFromName(const std::string& n) {
    if (n == "psnr") return MetricPsnr;
    if (n == "ssim") return MetricSsim;
    if (n == "ms_ssim" || n == "ms-ssim" || n == "msssim") return MetricMsSsim;
    if (n == "delta_ictcp" || n == "deltaictcp" || n == "ictcp" || n == "delta_itp") return MetricDeltaItp;
    if (n == "vmaf") return MetricVmaf;
    return 0;
}

bool vmafAvailable() {
#ifdef MONTAGE_WITH_VMAF
    return true;
#else
    return false;
#endif
}

// ---- reading files -----------------------------------------------------------------------------------------------

namespace {

// The planar format with this layout (FFmpeg's yuv4xxp[NN]le or gbrp[NN]le), or AV_PIX_FMT_NONE.
AVPixelFormat planarFormat(bool rgb, int log2w, int log2h, int bits) {
    for (const AVPixFmtDescriptor* d = av_pix_fmt_desc_next(nullptr); d; d = av_pix_fmt_desc_next(d)) {
        if (d->nb_components != 3 || (d->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL |
                                                   AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_ALPHA)))
            continue;
        if (!(d->flags & AV_PIX_FMT_FLAG_PLANAR) || bool(d->flags & AV_PIX_FMT_FLAG_RGB) != rgb) continue;
        if (!rgb && (d->log2_chroma_w != log2w || d->log2_chroma_h != log2h)) continue;
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c)
            ok = d->comp[c].depth == bits && d->comp[c].shift == 0 && d->comp[c].offset == 0 && d->comp[c].plane == c &&
                 d->comp[c].step == (bits > 8 ? 2 : 1);
        if (!ok) continue;
        // Not the JPEG full-range aliases (yuvj420p...).
        if (std::strstr(d->name, "yuvj")) continue;
        return av_pix_fmt_desc_get_id(d);
    }
    return AV_PIX_FMT_NONE;
}

std::string transferOf(int trc) {
    if (trc == AVCOL_TRC_SMPTE2084) return "pq";
    if (trc == AVCOL_TRC_ARIB_STD_B67) return "hlg";
    return "bt1886";
}

std::string primariesOf(int p) {
    if (p == AVCOL_PRI_BT2020) return "bt2020";
    if (p == AVCOL_PRI_SMPTE432) return "p3";
    return "bt709";
}

void matrixOf(int cs, int height, double& kr, double& kb) {
    switch (cs) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: kr = 0.299; kb = 0.114; return;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: kr = 0.2627; kb = 0.0593; return;
    case AVCOL_SPC_BT709: kr = 0.2126; kb = 0.0722; return;
    default:
        // Untagged: as players guess, SD as BT.601 and the rest BT.709.
        if (height < 720) { kr = 0.299; kb = 0.114; }
        else { kr = 0.2126; kb = 0.0722; }
    }
}

int swsColorspace(double kr) {
    if (std::abs(kr - 0.299) < 1e-3) return SWS_CS_ITU601;
    if (std::abs(kr - 0.2627) < 1e-3) return SWS_CS_BT2020;
    return SWS_CS_ITU709;
}

class FileReader {
public:
    ~FileReader() {
        sws_freeContext(sws_);
        sws_freeContext(toRgb_);
        av_frame_free(&rgb_);
        av_frame_free(&frame_);
        av_frame_free(&conv_);
        av_packet_free(&pkt_);
        avcodec_free_context(&dec_);
        avformat_close_input(&fmt_);
    }

    bool open(const std::string& path, std::string* error) {
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return fail(error, "cannot open " + path);
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return fail(error, "cannot read " + path);
        const AVCodec* codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) return fail(error, "no picture in " + path);
        dec_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec_, fmt_->streams[stream_]->codecpar);
        // Frame threads would hand frames out late but in order; slices keep the picture identical either way.
        dec_->thread_count = 0;
        if (avcodec_open2(dec_, codec, nullptr) < 0) return fail(error, "cannot decode " + path);
        frame_ = av_frame_alloc();
        conv_ = av_frame_alloc();
        rgb_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!frame_ || !conv_ || !rgb_ || !pkt_) return fail(error, "out of memory");
        const AVRational r = fmt_->streams[stream_]->avg_frame_rate.num > 0 ? fmt_->streams[stream_]->avg_frame_rate
                                                                             : fmt_->streams[stream_]->r_frame_rate;
        rate_ = r.num > 0 && r.den > 0 ? av_q2d(r) : 0;
        return true;
    }

    // How many frames the stream has, by its count or its duration; -1 if it cannot say.
    int64_t length() const {
        const AVStream* st = fmt_->streams[stream_];
        if (st->nb_frames > 0) return st->nb_frames;
        if (rate_ > 0 && st->duration > 0 && st->duration != AV_NOPTS_VALUE) return std::llround(double(st->duration) * av_q2d(st->time_base) * rate_);
        if (rate_ > 0 && fmt_->duration > 0) return std::llround(double(fmt_->duration) / AV_TIME_BASE * rate_);
        return -1;
    }
    double rate() const { return rate_; }
    // When a frame is shown (seconds), or NaN without a timestamp.
    bool clockMayJump() const { return fmt_ && fmt_->iformat && (fmt_->iformat->flags & AVFMT_TS_DISCONT); }
    double timeOf(const AVFrame* f) const {
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (ts == AV_NOPTS_VALUE) return std::numeric_limits<double>::quiet_NaN();
        return double(ts) * av_q2d(fmt_->streams[stream_]->time_base);
    }

    // The next frame decoded, or null at the end. Data the decoder refuses (a damaged packet) is noted against the
    // next frame it gives, rather than ending the stream there.
    AVFrame* nextFrame(bool& damaged) {
        damaged = false;
        while (true) {
            const int r = avcodec_receive_frame(dec_, frame_);
            if (r == 0) {
                damaged = pendingDamage_ || frame_->decode_error_flags != 0 || (frame_->flags & AV_FRAME_FLAG_CORRUPT);
                pendingDamage_ = false;
                return frame_;
            }
            if (r == AVERROR_EOF) return nullptr;
            if (r != AVERROR(EAGAIN)) {
                if (++errors_ > 1000) return nullptr;  // (a stream that is nothing but errors)
                pendingDamage_ = true;
                if (flushed_) return nullptr;
                continue;
            }
            if (flushed_) return nullptr;
            const int rd = av_read_frame(fmt_, pkt_);
            if (rd < 0) {
                if (rd != AVERROR_EOF) pendingDamage_ = true;
                avcodec_send_packet(dec_, nullptr);
                flushed_ = true;
                continue;
            }
            if (pkt_->stream_index == stream_) {
                const int sent = avcodec_send_packet(dec_, pkt_);
                if (sent < 0 && sent != AVERROR(EAGAIN)) pendingDamage_ = true;
            }
            av_packet_unref(pkt_);
        }
    }

    // The frame as a QualityPicture: its own layout (where it is planar already, or the nearest planar one), or
    // `like`'s.
    bool toPicture(const AVFrame* f, const QualityPicture* like, const CompareOptions& o, QualityPicture& out) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        if (!d) return false;
        const bool srcRgb = d->flags & AV_PIX_FMT_FLAG_RGB;
        const int srcBits = std::clamp(int(d->comp[0].depth), 8, 16);
        // The frame's own matrix and range: as tagged, an untagged matrix going with BT.2020 primaries when those are what
        // the picture is taken to have.
        const std::string transfer = !o.assumeTransfer.empty() ? o.assumeTransfer : transferOf(f->color_trc);
        const std::string primaries = !o.assumePrimaries.empty() ? o.assumePrimaries : primariesOf(f->color_primaries);
        double srcKr = 0, srcKb = 0;
        matrixOf(f->colorspace, f->height, srcKr, srcKb);
        if (f->colorspace == AVCOL_SPC_UNSPECIFIED && primaries == "bt2020") srcKr = 0.2627, srcKb = 0.0593;
        const bool srcFull = f->color_range == AVCOL_RANGE_JPEG || std::strstr(d->name, "yuvj") != nullptr;
        QualityPicture p;
        if (like) {
            p.width = like->width;
            p.height = like->height;
            p.bits = like->bits;
            p.log2ChromaW = like->log2ChromaW;
            p.log2ChromaH = like->log2ChromaH;
            p.rgb = like->rgb;
            p.fullRange = like->fullRange;
            p.chromaLeft = like->chromaLeft;
            p.kr = like->kr;
            p.kb = like->kb;
        } else {
            p.width = f->width;
            p.height = f->height;
            p.bits = srcBits;
            p.rgb = srcRgb;
            p.log2ChromaW = srcRgb ? 0 : d->log2_chroma_w;
            p.log2ChromaH = srcRgb ? 0 : d->log2_chroma_h;
            p.fullRange = srcFull;
            p.chromaLeft = f->chroma_location != AVCHROMA_LOC_CENTER && f->chroma_location != AVCHROMA_LOC_TOP &&
                           f->chroma_location != AVCHROMA_LOC_BOTTOM;
            p.kr = srcKr, p.kb = srcKb;
        }
        p.transfer = transfer;
        p.primaries = primaries;
        p.frameRate = rate_;
        const AVPixelFormat target = planarFormat(p.rgb, p.log2ChromaW, p.log2ChromaH, p.bits);
        if (target == AV_PIX_FMT_NONE) return false;
        const AVFrame* src = f;
        // (converted where the layout differs, or the range or matrix of Y′CbCr samples does)
        const bool sameCoding = srcRgb ? p.rgb : !p.rgb && srcFull == p.fullRange && std::abs(srcKr - p.kr) < 1e-4 && std::abs(srcKb - p.kb) < 1e-4;
        if (!sameCoding && !srcRgb && !p.rgb) {
            // Y′CbCr to Y′CbCr of another range or matrix: through 16-bit R′G′B′ (the scaler goes straight from one Y′CbCr
            // to the other without converting either when the formats match), so each side's coding is honoured.
            av_frame_unref(rgb_);
            rgb_->format = AV_PIX_FMT_RGB48LE;
            rgb_->width = f->width;
            rgb_->height = f->height;
            if (av_frame_get_buffer(rgb_, 0) < 0) return false;
            toRgb_ = sws_getCachedContext(toRgb_, f->width, f->height, AVPixelFormat(f->format), f->width, f->height, AV_PIX_FMT_RGB48LE,
                                          SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_BITEXACT, nullptr, nullptr, nullptr);
            sws_ = sws_getCachedContext(sws_, f->width, f->height, AV_PIX_FMT_RGB48LE, p.width, p.height, target,
                                        SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP | SWS_BITEXACT, nullptr, nullptr, nullptr);
            if (!toRgb_ || !sws_) return false;
            sws_setColorspaceDetails(toRgb_, sws_getCoefficients(swsColorspace(srcKr)), srcFull ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                     1 << 16, 1 << 16);
            sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(swsColorspace(p.kr)), p.fullRange ? 1 : 0, 0,
                                     1 << 16, 1 << 16);
            sws_scale(toRgb_, f->data, f->linesize, 0, f->height, rgb_->data, rgb_->linesize);
            av_frame_unref(conv_);
            conv_->format = target;
            conv_->width = p.width;
            conv_->height = p.height;
            if (av_frame_get_buffer(conv_, 0) < 0) return false;
            sws_scale(sws_, rgb_->data, rgb_->linesize, 0, f->height, conv_->data, conv_->linesize);
            src = conv_;
        } else if (f->format != target || f->width != p.width || f->height != p.height || !sameCoding) {
            sws_ = sws_getCachedContext(sws_, f->width, f->height, AVPixelFormat(f->format), p.width, p.height, target,
                                        SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_BITEXACT, nullptr, nullptr, nullptr);
            if (!sws_) return false;
            sws_setColorspaceDetails(sws_, sws_getCoefficients(swsColorspace(srcKr)), srcFull ? 1 : 0,
                                     sws_getCoefficients(swsColorspace(p.kr)), p.fullRange ? 1 : 0, 0, 1 << 16, 1 << 16);
            av_frame_unref(conv_);
            conv_->format = target;
            conv_->width = p.width;
            conv_->height = p.height;
            if (av_frame_get_buffer(conv_, 0) < 0) return false;
            sws_scale(sws_, f->data, f->linesize, 0, f->height, conv_->data, conv_->linesize);
            src = conv_;
        }
        for (int c = 0; c < 3; ++c) {
            const int w = p.planeWidth(c), h = p.planeHeight(c);
            p.plane[c].resize(size_t(w) * size_t(h));
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = src->data[c] + size_t(y) * size_t(src->linesize[c]);
                uint16_t* dst = &p.plane[c][size_t(y) * size_t(w)];
                if (p.bits > 8) std::memcpy(dst, row, size_t(w) * 2);
                else for (int x = 0; x < w; ++x) dst[x] = row[x];
            }
        }
        out = std::move(p);
        return true;
    }

private:
    static bool fail(std::string* error, const std::string& why) {
        if (error) *error = why;
        return false;
    }
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* conv_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* rgb_ = nullptr;
    SwsContext* sws_ = nullptr;
    SwsContext* toRgb_ = nullptr;
    int stream_ = -1;
    bool flushed_ = false, pendingDamage_ = false;
    int errors_ = 0;
    double rate_ = 0;
};

}  // namespace

PictureSource openPictureFile(const std::string& path, const QualityPicture* like, const CompareOptions& o, std::string* error) {
    auto reader = std::make_shared<FileReader>();
    if (!reader->open(path, error)) return {};
    std::shared_ptr<QualityPicture> shape = like ? std::make_shared<QualityPicture>(*like) : nullptr;
    if (shape) for (auto& pl : shape->plane) pl.clear();
    return [reader, shape, o](QualityPicture& out) {
        bool damaged = false;
        AVFrame* f = reader->nextFrame(damaged);
        if (!f || !reader->toPicture(f, shape.get(), o, out)) return false;
        out.damaged = damaged;
        out.time = reader->timeOf(f);
        out.clockMayJump = reader->clockMayJump();
        return true;
    };
}

int64_t pictureFileLength(const std::string& path) {
    FileReader reader;
    return reader.open(path, nullptr) ? reader.length() : -1;
}

bool probePictureFile(const std::string& path, QualityPicture& out, const CompareOptions& o, std::string* error) {
    PictureSource src = openPictureFile(path, nullptr, o, error);
    if (!src) return false;
    if (!src(out)) {
        if (error) *error = "no picture could be decoded from " + path;
        return false;
    }
    return true;
}

// ---- comparing ---------------------------------------------------------------------------------------------------

namespace {

constexpr int kThumbWidth = 64;

// A small picture of the luma (or green), 0..1, for the alignment.
std::vector<float> thumbnail(const QualityPicture& p) {
    const int tw = std::min(kThumbWidth, p.width);
    const int th = std::max(1, int(std::lround(double(tw) * p.height / std::max(1, p.width))));
    std::vector<float> t(size_t(tw) * size_t(th), 0.0f);
    const double peak = p.peak();
    for (int ty = 0; ty < th; ++ty) {
        const int y0 = ty * p.height / th, y1 = std::max(y0 + 1, (ty + 1) * p.height / th);
        for (int tx = 0; tx < tw; ++tx) {
            const int x0 = tx * p.width / tw, x1 = std::max(x0 + 1, (tx + 1) * p.width / tw);
            double s = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) s += p.plane[0][size_t(y) * size_t(p.width) + size_t(x)];
            t[size_t(ty) * size_t(tw) + size_t(tx)] = float(s / (double(x1 - x0) * double(y1 - y0)) / peak);
        }
    }
    return t;
}

FrameQuality measure(const QualityPicture& ref, const QualityPicture& enc, unsigned metrics) {
    FrameQuality q;
    if (metrics & MetricPsnr) {
        q.mseY = planeMse(enc, ref, 0);
        q.mseU = planeMse(enc, ref, 1);
        q.mseV = planeMse(enc, ref, 2);
        q.mseRgb = rgbMse(ref, enc);
        q.psnrRgb = q.mseRgb <= 0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / q.mseRgb);
        if (enc.rgb) {
            // (planes G, B and R: the "luma" figures over all three, as for R′G′B′ there is no luma)
            q.mseY = (q.mseY + q.mseU + q.mseV) / 3;
            q.mseU = q.mseV = 0;
        } else {
            q.psnrU = psnrFromMse(q.mseU, enc.peak());
            q.psnrV = psnrFromMse(q.mseV, enc.peak());
        }
        q.psnrY = psnrFromMse(q.mseY, enc.peak());
    }
    if (metrics & MetricSsim) {
        double all = 0, weight = 0;
        for (int c = 0; c < 3; ++c) {
            const double s = planeSsim(enc, ref, c);
            if (c == 0) q.ssim = s;
            const double wgt = double(enc.planeWidth(c)) * enc.planeHeight(c);
            if (std::isnan(s)) continue;
            all += s * wgt;
            weight += wgt;
        }
        q.ssimAll = weight > 0 ? all / weight : kNoScore;
        if (enc.rgb) q.ssim = q.ssimAll;
    }
    if (metrics & MetricMsSsim) q.msSsim = msSsim(enc, ref);
    if (metrics & MetricDeltaItp) deltaItp(ref, enc, q.deltaItp, q.deltaItpMax);
    return q;
}

// Runs fn(i) for i in [0, n) on up to `threads` threads; false if memory ran out in any.
bool parallelFor(int n, int threads, const std::function<void(int)>& fn) {
    threads = std::clamp(threads, 1, std::max(1, n));
    std::atomic<bool> ok{true};
    auto run = [&](int i) {
        try {
            fn(i);
        } catch (const std::bad_alloc&) {
            ok = false;
        }
    };
    if (threads == 1) {
        for (int i = 0; i < n && ok; ++i) run(i);
        return ok;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i; ok && (i = next.fetch_add(1)) < n;) run(i);
        });
    for (std::thread& t : pool) t.join();
    return ok;
}

std::string formatName(const QualityPicture& p) {
    std::string s;
    if (p.rgb) s = "R′G′B′";
    else if (p.log2ChromaW == 1 && p.log2ChromaH == 1) s = "4:2:0";
    else if (p.log2ChromaW == 1 && p.log2ChromaH == 0) s = "4:2:2";
    else if (p.log2ChromaW == 0 && p.log2ChromaH == 0) s = "4:4:4";
    else s = "Y′CbCr";
    return s + " " + std::to_string(p.bits) + "-bit";
}

#ifdef MONTAGE_WITH_VMAF
void signalToPicture(const Image& img, const QualityPicture& like, QualityPicture& out);

// VMAF through libvmaf: pictures fed in order, scores read back at the end.
class VmafScorer {
public:
    ~VmafScorer() {
        if (model_) vmaf_model_destroy(model_);
        if (vmaf_) vmaf_close(vmaf_);
    }
    bool open(const std::string& modelPath, int threads, std::string* error) {
        VmafConfiguration cfg{};
        cfg.log_level = VMAF_LOG_LEVEL_NONE;
        cfg.n_threads = unsigned(std::max(0, threads));
        cfg.n_subsample = 1;
        if (vmaf_init(&vmaf_, cfg) < 0) return fail(error, "libvmaf would not start");
        VmafModelConfig mc{};
        mc.name = "vmaf";
        if (vmaf_model_load_from_path(&model_, &mc, modelPath.c_str()) < 0) return fail(error, "cannot load the VMAF model " + modelPath);
        if (vmaf_use_features_from_model(vmaf_, model_) < 0) return fail(error, "the VMAF model's features are not built in");
        return true;
    }
    bool add(const QualityPicture& ref, const QualityPicture& enc) {
        VmafPicture r{}, e{};
        if (!toVmaf(ref, r)) return false;
        if (!toVmaf(enc, e)) {
            vmaf_picture_unref(&r);
            return false;
        }
        if (vmaf_read_pictures(vmaf_, &r, &e, index_++) == 0) return true;
        // (refused, libvmaf leaves them to be let go of here)
        vmaf_picture_unref(&r);
        vmaf_picture_unref(&e);
        return false;
    }
    bool finish(std::vector<double>& scores) {
        if (vmaf_read_pictures(vmaf_, nullptr, nullptr, 0) < 0) return false;
        scores.assign(index_, kNoScore);
        for (unsigned i = 0; i < index_; ++i) {
            double s = 0;
            if (vmaf_score_at_index(vmaf_, model_, &s, i) == 0) scores[i] = s;
        }
        return true;
    }

private:
    static bool fail(std::string* error, const std::string& why) {
        if (error) *error = why;
        return false;
    }
    // (only the samplings libvmaf takes: 4:2:0, 4:2:2 and 4:4:4; each plane copied as far as libvmaf's sizes it, which
    // round odd chroma sizes down where Montage's round up)
    static bool toVmaf(const QualityPicture& in, VmafPicture& out) {
        if (in.rgb || !((in.log2ChromaW == 1 && in.log2ChromaH <= 1) || (in.log2ChromaW == 0 && in.log2ChromaH == 0))) return false;
        // libvmaf knows PQ but not HLG: HLG is given as the PQ picture of the light it shows on a 1000-nit display.
        QualityPicture pq;
        if (in.transfer == "hlg") pq = hlgAsPq(in);
        const QualityPicture& p = in.transfer == "hlg" ? pq : in;
        VmafPixelFormat fmt = p.log2ChromaH ? VMAF_PIX_FMT_YUV420P : p.log2ChromaW ? VMAF_PIX_FMT_YUV422P : VMAF_PIX_FMT_YUV444P;
        if (vmaf_picture_alloc(&out, fmt, unsigned(p.bits), unsigned(p.width), unsigned(p.height)) < 0) return false;
        for (int c = 0; c < 3; ++c) {
            const int w = p.planeWidth(c), h = std::min(p.planeHeight(c), int(out.h[c])), cw = std::min(w, int(out.w[c]));
            for (int y = 0; y < h; ++y) {
                uint8_t* row = static_cast<uint8_t*>(out.data[c]) + size_t(y) * size_t(out.stride[c]);
                const uint16_t* src = &p.plane[c][size_t(y) * size_t(w)];
                if (p.bits > 8) std::memcpy(row, src, size_t(cw) * 2);
                else for (int x = 0; x < cw; ++x) row[x] = uint8_t(src[x]);
            }
        }
        out.color.range = p.fullRange ? VMAF_COLOR_RANGE_FULL : VMAF_COLOR_RANGE_LIMITED;
        out.color.primaries = p.primaries == "bt2020" ? VMAF_COLOR_PRIMARIES_BT2020
                              : p.primaries == "p3"   ? VMAF_COLOR_PRIMARIES_SMPTE432
                                                      : VMAF_COLOR_PRIMARIES_BT709;
        out.color.trc = p.transfer == "pq" ? VMAF_COLOR_TRC_SMPTE2084 : VMAF_COLOR_TRC_BT709;
        out.color.matrix = std::abs(p.kr - 0.2627) < 1e-3 ? VMAF_COLOR_MATRIX_BT2020_NCL : VMAF_COLOR_MATRIX_BT709;
        return true;
    }
    static QualityPicture hlgAsPq(const QualityPicture& p) {
        Image img(p.width, p.height, Image::Uninitialized{});
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x) {
                double rgb[3], nits[3];
                signalAt(p, x, y, rgb);
                nitsOfSignal(p, rgb, nits);
                float* px = img.at(x, y);
                for (int c = 0; c < 3; ++c) px[c] = float(pqInverse(nits[c]));
                px[3] = 1;
            }
        QualityPicture like = p;
        like.transfer = "pq";
        like.primaries = "bt2020";
        like.kr = 0.2627;
        like.kb = 0.0593;
        QualityPicture out;
        signalToPicture(img, like, out);
        return out;
    }
    VmafContext* vmaf_ = nullptr;
    VmafModel* model_ = nullptr;
    unsigned index_ = 0;
};
#endif

[[maybe_unused]] std::string vmafModelFor(const QualityPicture& p, const CompareOptions& o) {
    if (!o.vmafModel.empty()) return o.vmafModel;
    QString dir = QString::fromStdString(o.vmafModelDir);
    if (dir.isEmpty()) dir = qEnvironmentVariable("MONTAGE_VMAF_MODELS");
    if (dir.isEmpty() && QCoreApplication::instance()) dir = QCoreApplication::applicationDirPath() + "/vmaf-models";
    const bool hdr = p.transfer == "pq" || p.transfer == "hlg";
    const bool uhd = p.height > 1440;
    const QString name = hdr ? QStringLiteral("hdr_vmaf_v1.1.8/hdr_vmaf_v1.1.8_%1.json").arg(uhd ? "3d0h_2160" : "3d0h")
                             : QStringLiteral("vmaf_v1.0.16/vmaf_v1.0.16_%1.json").arg(uhd ? "3d0h_2160" : "3d0h");
    for (const QString& candidate : {dir + "/" + name, dir + "/" + QFileInfo(name).fileName()})
        if (QFileInfo::exists(candidate)) return candidate.toStdString();
    return (dir + "/" + name).toStdString();
}

double meanOf(const std::vector<FrameQuality>& f, double FrameQuality::*m) {
    double s = 0;
    int n = 0;
    for (const FrameQuality& q : f)
        if (!std::isnan(q.*m)) {
            s += q.*m;
            ++n;
        }
    return n ? s / n : kNoScore;
}

double worstOf(const std::vector<FrameQuality>& f, double FrameQuality::*m, bool lowIsBad) {
    double w = kNoScore;
    for (const FrameQuality& q : f) {
        const double v = q.*m;
        if (std::isnan(v)) continue;
        if (std::isnan(w) || (lowIsBad ? v < w : v > w)) w = v;
    }
    return w;
}

}  // namespace

EncodeComparison compareSources(const std::function<PictureSource()>& openReference, const std::function<PictureSource()>& openEncode,
                                const CompareOptions& o, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                                int64_t referenceLength) {
    EncodeComparison res;
    auto cancelled = [&] { return cancel && cancel->load(); };
    int threads = o.threads > 0 ? o.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    // 1. The alignment, on small pictures of the first frames.
    const int search = std::max(0, o.search);
    const int alignN = std::max(1, o.alignFrames);
    std::vector<std::vector<float>> refThumbs, encThumbs;
    QualityPicture shape;
    {
        PictureSource ref = openReference(), enc = openEncode();
        if (!ref || !enc) {
            res.error = "the reference or the encode could not be opened";
            return res;
        }
        QualityPicture p;
        for (int i = 0; i < alignN + search && enc(p); ++i) {
            if (i == 0) shape = p;
            encThumbs.push_back(thumbnail(p));
        }
        for (int i = 0; i < alignN + search && !cancelled() && ref(p); ++i) refThumbs.push_back(thumbnail(p));
    }
    if (cancelled()) {
        res.error = "cancelled";
        return res;
    }
    if (encThumbs.empty() || refThumbs.empty()) {
        res.error = encThumbs.empty() ? "the encode has no pictures" : "the reference has no pictures";
        return res;
    }
    double best = std::numeric_limits<double>::infinity();
    int bestOffset = 0;
    for (int a = 0; a <= 2 * search; ++a) {
        // 0, 1, -1, 2, -2...: on a tie the smaller offset stays.
        const int d = (a % 2 ? 1 : -1) * ((a + 1) / 2);
        double sum = 0;
        int pairs = 0;
        for (int i = std::max(0, d); i < int(encThumbs.size()) && pairs < alignN; ++i) {
            const int j = i - d;
            if (j < 0 || j >= int(refThumbs.size())) continue;
            const auto& x = encThumbs[size_t(i)];
            const auto& y = refThumbs[size_t(j)];
            if (x.size() != y.size()) continue;
            double s = 0;
            for (size_t k = 0; k < x.size(); ++k) s += double(x[k] - y[k]) * double(x[k] - y[k]);
            sum += s / double(x.size());
            ++pairs;
        }
        // At least half the frames asked for (or all there are) must overlap.
        const int needed = std::min(alignN, int(std::min(encThumbs.size(), refThumbs.size()))) / 2;
        if (pairs == 0 || pairs < needed) continue;
        const double cost = sum / pairs;
        if (cost < best - 1e-12) {
            best = cost;
            bestOffset = d;
        }
    }
    res.offset = bestOffset;
    res.alignCost = std::isfinite(best) ? best : 0;
    res.width = shape.width;
    res.height = shape.height;
    res.bits = shape.bits;
    res.format = formatName(shape);

    // 2. Frame by frame.
    PictureSource ref = openReference(), enc = openEncode();
    if (!ref || !enc) {
        res.error = "the reference or the encode could not be opened again";
        return res;
    }
    QualityPicture skip;
    for (int i = 0; i < std::max(0, res.offset); ++i)
        if (!enc(skip)) break;
    for (int i = 0; i < std::max(0, -res.offset); ++i)
        if (!ref(skip)) break;
    // (as many frames at once as there are threads, fewer for big pictures: each pair held is up to 12 bytes a pixel
    // (two 16-bit 4:4:4 pictures), with MS-SSIM's working space about 12 more; about 2 GB in all)
    {
        const double perFrame = double(shape.width) * shape.height * ((o.metrics & MetricMsSsim) ? 24 : 12) + 1;
        threads = std::clamp(int(double(size_t(2) << 30) / perFrame), 1, threads);
    }
    const bool wantVmaf = (o.metrics & MetricVmaf) && !shape.rgb &&
                          ((shape.log2ChromaW == 1 && shape.log2ChromaH <= 1) || (shape.log2ChromaW == 0 && shape.log2ChromaH == 0));
#ifdef MONTAGE_WITH_VMAF
    std::unique_ptr<VmafScorer> vmaf;
    if (wantVmaf) {
        vmaf = std::make_unique<VmafScorer>();
        res.vmafModel = vmafModelFor(shape, o);
        std::string e;
        if (!vmaf->open(res.vmafModel, threads, &e)) {
            res.error = e;
            return res;
        }
    }
#else
    if (wantVmaf) res.vmafModel = "";
#endif
    const int batch = std::max(1, threads);
    const FrameTime firstIndex = std::max(0, -res.offset);
    // Frames are matched by the encode's timestamps where it has them (a dropped or repeated frame, or a variable
    // rate, keeps the rest in step), else one after another.
    int64_t refRead = 0;  // reference frames read since the offset
    // (each frame's place from the gap since the one before, so a rate a little off, as Matroska's estimate, never adds up)
    double encPrev = std::numeric_limits<double>::quiet_NaN();
    int64_t encPlace = 0;
    const double encRate = shape.frameRate;
    bool more = true, encodeEnded = false, referenceEnded = false;
    std::vector<QualityPicture> refs(static_cast<size_t>(batch)), encs(static_cast<size_t>(batch));
    std::vector<FrameTime> shows(static_cast<size_t>(batch));
    while (more) {
        if (cancelled()) {
            res.error = "cancelled";
            return res;
        }
        int n = 0;
        try {
            while (n < batch) {
                if (o.maxFrames > 0 && int(res.frames.size()) + n >= o.maxFrames) {
                    more = false;
                    break;
                }
                QualityPicture& e = encs[size_t(n)];
                encodeEnded = !enc(e);
                if (encodeEnded) {
                    more = false;
                    break;
                }
                if (e.damaged) ++res.damagedFrames;
                if (encRate > 0 && !std::isnan(e.time)) {
                    if (!std::isnan(encPrev)) {
                        int64_t step = std::llround((e.time - encPrev) * encRate);
                        // (in a file whose clock can start again, as a transport stream's can at a discontinuity, a
                        // jump of more than a second either way is that, not a second of pictures dropped or shown
                        // again: the next picture follows on)
                        if (e.clockMayJump && (step > std::max<int64_t>(2, std::llround(encRate)) || step < -1)) step = 1;
                        encPlace += std::max<int64_t>(0, step);
                    }
                    encPrev = e.time;
                    const int64_t want = encPlace;
                    if (want < refRead) {  // (shown again, or out of step: left out)
                        ++res.repeatedFrames;
                        continue;
                    }
                    while (refRead < want && !referenceEnded) {  // (frames the encode does not have)
                        referenceEnded = !ref(skip);
                        if (referenceEnded) break;
                        ++res.droppedFrames;
                        ++refRead;
                    }
                }
                referenceEnded = referenceEnded || !ref(refs[size_t(n)]);
                if (referenceEnded) {
                    more = false;
                    break;
                }
                shows[size_t(n)] = firstIndex + FrameTime(refRead++);
                if (!e.sameLayout(refs[size_t(n)])) {
                    res.error = "the reference could not be brought to the encode's size and format";
                    return res;
                }
                ++n;
            }
        } catch (const std::bad_alloc&) {
            res.error = "there is not memory enough to compare pictures this size";
            return res;
        }
        std::vector<FrameQuality> got(static_cast<size_t>(n));
        if (!parallelFor(n, threads, [&](int i) { got[size_t(i)] = measure(refs[size_t(i)], encs[size_t(i)], o.metrics); })) {
            res.error = "there is not memory enough to compare pictures this size";
            return res;
        }
        if (cancelled()) {  // (a render cut short is not the picture)
            res.error = "cancelled";
            return res;
        }
        for (int i = 0; i < n; ++i) {
            got[size_t(i)].frame = shows[size_t(i)];
#ifdef MONTAGE_WITH_VMAF
            if (vmaf && !vmaf->add(refs[size_t(i)], encs[size_t(i)])) {
                res.error = "libvmaf refused a picture";
                return res;
            }
#endif
            res.frames.push_back(got[size_t(i)]);
        }
        if (progress) progress(o.maxFrames > 0 ? std::min(1.0, double(res.frames.size()) / o.maxFrames) : -1);
    }
#ifdef MONTAGE_WITH_VMAF
    if (vmaf) {
        std::vector<double> scores;
        if (!vmaf->finish(scores)) {
            res.error = "libvmaf could not score the pictures";
            return res;
        }
        for (size_t i = 0; i < res.frames.size() && i < scores.size(); ++i) res.frames[i].vmaf = scores[i];
        res.vmafScored = true;
    }
#endif
    if (cancelled()) {
        res.error = "cancelled";
        return res;
    }
    if (res.frames.empty()) {
        res.error = "the encode and the reference have no frames in common";
        return res;
    }
    // How long each is, past the offset: the one that went on is read to its end to count (a known reference length
    // is taken as it is), unless only so many frames were asked for.
    const int64_t compared = int64_t(res.frames.size());
    if (o.maxFrames <= 0 || compared < o.maxFrames) {
        QualityPicture rest;
        int64_t extraEnc = 0, extraRef = 0;
        if (referenceEnded) {
            extraEnc = 1;  // (the one read when the reference ran out)
            while (!cancelled() && enc(rest)) ++extraEnc;
            extraRef = 0;
        } else if (encodeEnded) {
            if (referenceLength >= 0) extraRef = std::max<int64_t>(0, referenceLength - std::max(0, -res.offset) - refRead);
            else
                while (!cancelled() && ref(rest)) ++extraRef;
        }
        if (cancelled()) {
            res.error = "cancelled";
            return res;
        }
        res.encodeFrames = compared + res.repeatedFrames + extraEnc;
        res.referenceFrames = refRead + extraRef;
        if (extraRef > 0)
            res.warnings.push_back("the encode ends " + std::to_string(extraRef) + (extraRef == 1 ? " frame" : " frames") + " before the reference");
        if (extraEnc > 0)
            res.warnings.push_back("the encode runs " + std::to_string(extraEnc) + (extraEnc == 1 ? " frame" : " frames") + " past the reference");
    }
    if (res.offset > 0)
        res.warnings.push_back("the encode has " + std::to_string(res.offset) + (res.offset == 1 ? " frame" : " frames") + " before the reference's first");
    else if (res.offset < 0)
        res.warnings.push_back("the encode starts " + std::to_string(-res.offset) + (res.offset == -1 ? " frame" : " frames") + " into the reference");
    if (res.droppedFrames > 0)
        res.warnings.push_back("the encode leaves out " + std::to_string(res.droppedFrames) + (res.droppedFrames == 1 ? " frame" : " frames") +
                               " of the reference (matched by its timestamps)");
    if (res.repeatedFrames > 0)
        res.warnings.push_back("the encode shows " + std::to_string(res.repeatedFrames) + (res.repeatedFrames == 1 ? " frame" : " frames") +
                               " again or out of step (left out)");
    if (res.damagedFrames > 0)
        res.warnings.push_back("the decoder reported damage in " + std::to_string(res.damagedFrames) + (res.damagedFrames == 1 ? " frame" : " frames") +
                               " of the encode");
    // 3. The summary (the mean PSNRs from the mean squared errors, as FFmpeg's average).
    if (o.metrics & MetricPsnr) {
        res.mean.mseY = meanOf(res.frames, &FrameQuality::mseY);
        res.mean.mseU = meanOf(res.frames, &FrameQuality::mseU);
        res.mean.mseV = meanOf(res.frames, &FrameQuality::mseV);
        res.mean.mseRgb = meanOf(res.frames, &FrameQuality::mseRgb);
        res.mean.psnrY = psnrFromMse(res.mean.mseY, shape.peak());
        if (!shape.rgb) {
            res.mean.psnrU = psnrFromMse(res.mean.mseU, shape.peak());
            res.mean.psnrV = psnrFromMse(res.mean.mseV, shape.peak());
        }
        res.mean.psnrRgb = res.mean.mseRgb <= 0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / res.mean.mseRgb);
    }
    res.mean.ssim = meanOf(res.frames, &FrameQuality::ssim);
    res.mean.ssimAll = meanOf(res.frames, &FrameQuality::ssimAll);
    res.mean.msSsim = meanOf(res.frames, &FrameQuality::msSsim);
    res.mean.deltaItp = meanOf(res.frames, &FrameQuality::deltaItp);
    res.mean.deltaItpMax = worstOf(res.frames, &FrameQuality::deltaItpMax, false);
    res.mean.vmaf = meanOf(res.frames, &FrameQuality::vmaf);
    res.worst.psnrY = worstOf(res.frames, &FrameQuality::psnrY, true);
    res.worst.psnrRgb = worstOf(res.frames, &FrameQuality::psnrRgb, true);
    res.worst.ssim = worstOf(res.frames, &FrameQuality::ssim, true);
    res.worst.msSsim = worstOf(res.frames, &FrameQuality::msSsim, true);
    res.worst.deltaItp = worstOf(res.frames, &FrameQuality::deltaItp, false);
    res.worst.deltaItpMax = res.mean.deltaItpMax;
    res.worst.vmaf = worstOf(res.frames, &FrameQuality::vmaf, true);
    // The worst frames: by PSNR (of R′G′B′) where measured, else SSIM, else VMAF, else ΔICtCp. Identical frames are not listed.
    std::vector<size_t> order(res.frames.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto key = [&](const FrameQuality& q) {
        // R′G′B′ PSNR covers the chroma too: an encode can keep the luma exact and still change the colour.
        if (o.metrics & MetricPsnr) return std::isnan(q.psnrRgb) ? q.psnrY : q.psnrRgb;
        if (o.metrics & MetricSsim) return q.ssim;
        if (res.vmafScored) return q.vmaf;
        if (o.metrics & MetricMsSsim) return q.msSsim;
        return -q.deltaItp;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const double ka = key(res.frames[a]), kb = key(res.frames[b]);
        if (std::isnan(ka) != std::isnan(kb)) return std::isnan(kb);
        return ka < kb;
    });
    for (size_t i = 0; i < order.size() && int(res.worstFrames.size()) < o.worstCount; ++i) {
        const FrameQuality& q = res.frames[order[i]];
        const double k = key(q);
        if (std::isnan(k) || std::isinf(k) || ((o.metrics & MetricPsnr) == 0 && (o.metrics & MetricSsim) && k >= 1)) continue;
        res.worstFrames.push_back(q.frame);
    }
    res.ok = true;
    if (progress) progress(1);
    return res;
}

EncodeComparison compareEncodeFiles(const std::string& reference, const std::string& encode, const CompareOptions& o,
                                    const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    QualityPicture like;
    std::string e;
    if (!probePictureFile(encode, like, o, &e)) {
        EncodeComparison r;
        r.error = e;
        return r;
    }
    for (auto& pl : like.plane) pl.clear();
    // (frame for frame only at the same rate)
    QualityPicture first;
    if (probePictureFile(reference, first, o, &e) && first.frameRate > 0 && like.frameRate > 0 &&
        std::abs(first.frameRate / like.frameRate - 1) > 1e-4) {
        EncodeComparison r;
        char buf[160];
        std::snprintf(buf, sizeof buf, "the encode runs at %.3f frames a second, the reference at %.3f", like.frameRate, first.frameRate);
        r.error = buf;
        return r;
    }
    return compareSources([&] { return openPictureFile(reference, &like, o); }, [&] { return openPictureFile(encode, nullptr, o); }, o,
                          progress, cancel);
}

namespace {

// A rendered frame (R′G′B′ signal, 0..1) as `like`'s Y′CbCr or R′G′B′ planes.
void signalToPicture(const Image& img, const QualityPicture& like, QualityPicture& out) {
    out = like;
    const double peak = like.peak();
    const double scale = double(1 << (like.bits - 8));
    const int w = like.width, h = like.height;
    auto clampCode = [&](double v) { return uint16_t(std::clamp(std::lround(v), 0L, long(like.peak()))); };
    auto pixel = [&](int x, int y, double rgb[3]) {
        const float* p = img.at(std::min(x, img.width - 1), std::min(y, img.height - 1));
        for (int c = 0; c < 3; ++c) rgb[c] = std::clamp(double(p[c]), 0.0, 1.0);
    };
    for (int c = 0; c < 3; ++c) out.plane[c].assign(size_t(like.planeWidth(c)) * size_t(like.planeHeight(c)), 0);
    if (like.rgb) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double rgb[3];
                pixel(x, y, rgb);
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                out.plane[0][i] = clampCode(rgb[1] * peak);
                out.plane[1][i] = clampCode(rgb[2] * peak);
                out.plane[2][i] = clampCode(rgb[0] * peak);
            }
        return;
    }
    const double kr = like.kr, kb = like.kb, kg = 1 - kr - kb;
    auto ycc = [&](const double rgb[3], double& Y, double& Cb, double& Cr) {
        Y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
        Cb = (rgb[2] - Y) / (2 * (1 - kb));
        Cr = (rgb[0] - Y) / (2 * (1 - kr));
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double rgb[3], Y, Cb, Cr;
            pixel(x, y, rgb);
            ycc(rgb, Y, Cb, Cr);
            out.plane[0][size_t(y) * size_t(w) + size_t(x)] = clampCode(like.fullRange ? Y * peak : (16 + 219 * Y) * scale);
        }
    // Chroma: the mean of the block of pixels each sample covers (centred siting), or, sited with the left luma
    // sample as the encode says (and as Montage's own exports are), across that sample and its two neighbours
    // (1, 2, 1) in each row of the block: so siting alone is not counted as the encode's loss.
    const int cw = like.planeWidth(1), ch = like.planeHeight(1), sx = 1 << like.log2ChromaW, sy = 1 << like.log2ChromaH;
    const bool left = like.chromaLeft && sx == 2;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            double cb = 0, cr = 0, n = 0;
            for (int dy = 0; dy < sy; ++dy) {
                if (y * sy + dy >= h) continue;
                auto add = [&](int px, double weight) {
                    double rgb[3], Y, Cb, Cr;
                    pixel(std::clamp(px, 0, w - 1), y * sy + dy, rgb);
                    ycc(rgb, Y, Cb, Cr);
                    cb += weight * Cb;
                    cr += weight * Cr;
                    n += weight;
                };
                if (left) {
                    add(x * 2 - 1, 1), add(x * 2, 2), add(x * 2 + 1, 1);
                } else {
                    for (int dx = 0; dx < sx; ++dx)
                        if (x * sx + dx < w) add(x * sx + dx, 1);
                }
            }
            cb /= std::max(1e-9, n);
            cr /= std::max(1e-9, n);
            const size_t i = size_t(y) * size_t(cw) + size_t(x);
            out.plane[1][i] = clampCode(like.fullRange ? (cb * peak + 128 * scale) : (128 + 224 * cb) * scale);
            out.plane[2][i] = clampCode(like.fullRange ? (cr * peak + 128 * scale) : (128 + 224 * cr) * scale);
        }
}

}  // namespace

EncodeComparison compareEncodeWithSequence(const Project& p, const Sequence& s, FrameTime in, FrameTime out, const std::string& encode,
                                           const CompareOptions& o, const std::function<void(double)>& progress,
                                           const std::atomic<bool>* cancel, FrameTime encodeStartsAt) {
    EncodeComparison r;
    QualityPicture like;
    if (!probePictureFile(encode, like, o, &r.error)) return r;
    for (auto& pl : like.plane) pl.clear();
    if (like.frameRate > 0 && s.fpsValue() > 0 && std::abs(like.frameRate / s.fpsValue() - 1) > 1e-4) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "the encode runs at %.3f frames a second, the sequence at %.3f", like.frameRate, s.fpsValue());
        r.error = buf;
        return r;
    }
    // The sequence's frames carry its delivery space.
    const ColorSpace& space = sequenceColorSpace(s);
    QualityPicture seqLike = like;
    seqLike.transfer = space.transfer == Transfer::Pq ? "pq" : space.transfer == Transfer::Hlg ? "hlg" : "bt1886";
    seqLike.primaries = space.primaries == Primaries::Bt2020 ? "bt2020" : space.primaries == Primaries::P3D65 ? "p3" : "bt709";
    const FrameTime end = out < 0 ? s.duration() : out;
    if (end <= in) {
        r.error = "nothing to compare in that range";
        return r;
    }
    if (s.width <= 0 || like.width <= 0 || std::abs(double(like.height) / like.width - double(s.height) / s.width) > 0.02) {
        r.error = "the encode's frame (" + std::to_string(like.width) + "x" + std::to_string(like.height) + ") is not the sequence's shape (" +
                  std::to_string(s.width) + "x" + std::to_string(s.height) + ")";
        return r;
    }
    RenderOptions ro;
    ro.highQuality = true;
    ro.slowEffects = true;
    ro.scale = double(like.width) / s.width;
    ro.cancel = cancel;
    auto openSeq = [&, ro, seqLike]() -> PictureSource {
        auto t = std::make_shared<FrameTime>(in);
        return [&p, &s, ro, seqLike, t, end, cancel](QualityPicture& pic) {
            if (*t >= end || (cancel && cancel->load())) return false;
            Image img = renderProgramFrame(p, s, (*t)++, ro);
            if (img.empty() || (cancel && cancel->load())) return false;
            // (the render at the encode's own size exactly: a frame a pixel off by rounding, or of a slightly other shape)
            if (img.width != seqLike.width || img.height != seqLike.height) {
                Image sized(seqLike.width, seqLike.height, Image::Uninitialized{});
                const std::vector<float> resampled = resampleRgba(img.px.data(), img.width, img.height, seqLike.width, seqLike.height);
                std::copy(resampled.begin(), resampled.end(), sized.px.begin());
                img = std::move(sized);
            }
            signalToPicture(img, seqLike, pic);
            return true;
        };
    };
    // A whole delivery checked over a range: its frames before the range passed over.
    const FrameTime skip = encodeStartsAt >= 0 && encodeStartsAt < in ? in - encodeStartsAt : 0;
    auto openEnc = [&]() -> PictureSource {
        PictureSource src = openPictureFile(encode, nullptr, o);
        if (!src || skip <= 0) return src;
        auto left = std::make_shared<FrameTime>(skip);
        return [src, left](QualityPicture& pic) {
            for (; *left > 0; --*left)
                if (!src(pic)) return false;
            return src(pic);
        };
    };
    CompareOptions oo = o;
    if (oo.maxFrames <= 0 || oo.maxFrames > int(end - in)) oo.maxFrames = int(end - in);
    r = compareSources(openSeq, openEnc, oo, progress, cancel, end - in);
    // (the encode counted to its end, as maxFrames stops it at the range's)
    if (r.ok && int64_t(r.frames.size()) >= oo.maxFrames) {
        const int64_t length = pictureFileLength(encode);
        if (length >= 0) {
            const int64_t after = length - skip - std::max(0, r.offset) - int64_t(r.frames.size());
            // (a whole delivery checked over a range goes on past it, as it should)
            if (after > 0 && (skip == 0 && out < 0))
                r.warnings.push_back("the encode runs " + std::to_string(after) + (after == 1 ? " frame" : " frames") + " past the sequence");
        }
    }
    for (FrameQuality& q : r.frames) q.frame += in;
    for (FrameTime& f : r.worstFrames) f += in;
    for (std::string& w : r.warnings)
        for (size_t at; (at = w.find("the reference")) != std::string::npos;) w.replace(at, 13, "the sequence");
    return r;
}

std::vector<QualitySpan> qualityOverLimits(const EncodeComparison& c, const QualityLimits& l) {
    std::vector<QualitySpan> spans;
    char buf[160];
    for (const FrameQuality& q : c.frames) {
        std::string why;
        if (l.minPsnr > 0 && !std::isnan(q.psnrY) && q.psnrY < l.minPsnr) {
            std::snprintf(buf, sizeof buf, "PSNR %.2f dB under %.2f", q.psnrY, l.minPsnr);
            why = buf;
        }
        if (l.minVmaf > 0 && !std::isnan(q.vmaf) && q.vmaf < l.minVmaf) {
            std::snprintf(buf, sizeof buf, "VMAF %.2f under %.2f", q.vmaf, l.minVmaf);
            why += (why.empty() ? "" : "; ") + std::string(buf);
        }
        if (l.maxDeltaItp > 0 && !std::isnan(q.deltaItp) && q.deltaItp > l.maxDeltaItp) {
            std::snprintf(buf, sizeof buf, "ΔICtCp %.2f over %.2f", q.deltaItp, l.maxDeltaItp);
            why += (why.empty() ? "" : "; ") + std::string(buf);
        }
        if (why.empty()) continue;
        if (!spans.empty() && spans.back().end == q.frame) {
            spans.back().end = q.frame + 1;
            continue;  // the first frame's reading names the stretch
        }
        spans.push_back({q.frame, q.frame + 1, why});
    }
    return spans;
}

namespace {

std::string fmt(double v, int decimals) {
    if (std::isnan(v)) return "";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string timecodeOf(FrameTime f, const Sequence* s) {
    if (!s) return std::to_string(f);
    return formatTimecode(f, s->fps);
}

}  // namespace

bool writeQualityCsv(const std::string& path, const EncodeComparison& c, const Sequence* s, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    std::fprintf(f, "# Compare Encode\n# format,%s,%dx%d\n# offset,%d\n# frames,%zu\n", c.format.c_str(), c.width, c.height, c.offset,
                 c.frames.size());
    std::fprintf(f, "# mean,psnr_y %s,psnr_rgb %s,ssim %s,ms_ssim %s,delta_ictcp %s,vmaf %s\n", fmt(c.mean.psnrY, 3).c_str(),
                 fmt(c.mean.psnrRgb, 3).c_str(), fmt(c.mean.ssim, 6).c_str(), fmt(c.mean.msSsim, 6).c_str(), fmt(c.mean.deltaItp, 4).c_str(),
                 fmt(c.mean.vmaf, 4).c_str());
    std::fprintf(f, "# worst,psnr_y %s,ssim %s,ms_ssim %s,delta_ictcp %s,vmaf %s\n", fmt(c.worst.psnrY, 3).c_str(), fmt(c.worst.ssim, 6).c_str(),
                 fmt(c.worst.msSsim, 6).c_str(), fmt(c.worst.deltaItp, 4).c_str(), fmt(c.worst.vmaf, 4).c_str());
    if (!c.vmafModel.empty()) std::fprintf(f, "# vmaf_model,%s\n", c.vmafModel.c_str());
    std::fprintf(f, "frame,timecode,psnr_y,psnr_u,psnr_v,psnr_rgb,ssim_y,ssim_all,ms_ssim,delta_ictcp,delta_ictcp_max,vmaf\n");
    for (const FrameQuality& q : c.frames)
        std::fprintf(f, "%lld,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", (long long)q.frame, timecodeOf(q.frame, s).c_str(), fmt(q.psnrY, 4).c_str(),
                     fmt(q.psnrU, 4).c_str(), fmt(q.psnrV, 4).c_str(), fmt(q.psnrRgb, 4).c_str(), fmt(q.ssim, 6).c_str(),
                     fmt(q.ssimAll, 6).c_str(), fmt(q.msSsim, 6).c_str(), fmt(q.deltaItp, 4).c_str(), fmt(q.deltaItpMax, 4).c_str(),
                     fmt(q.vmaf, 4).c_str());
    const bool ok = std::fclose(f) == 0;
    if (!ok && error) *error = "cannot write " + path;
    return ok;
}

bool writeQualityPdf(const std::string& path, const EncodeComparison& c, const std::string& title, const QualityLimits& l,
                     const Sequence* s, std::string* error) {
    QPdfWriter writer(QString::fromStdString(path));
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(0, 0, 0, 0)));
    writer.setTitle(QStringLiteral("Encode quality report"));
    writer.setCreator(QStringLiteral("Montage"));
    QPainter p;
    if (!p.begin(&writer)) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    // (in points on an A4 page, 595 x 842)
    const int W = 595, H = 842, margin = 48;
    p.setWindow(0, 0, W, H);
    p.setViewport(0, 0, writer.width(), writer.height());
    QFont big(QStringLiteral("Helvetica"));
    big.setPixelSize(18);
    big.setBold(true);
    QFont body(QStringLiteral("Helvetica"));
    body.setPixelSize(10);
    QFont small(QStringLiteral("Helvetica"));
    small.setPixelSize(8);
    int y = margin;
    p.setFont(big);
    p.drawText(margin, y + 18, QStringLiteral("Encode quality: %1").arg(QString::fromStdString(title)));
    y += 34;
    p.setFont(body);
    std::vector<std::pair<std::string, std::string>> rows = {
        {"Encode", c.format + ", " + std::to_string(c.width) + "x" + std::to_string(c.height)},
        {"Frames compared", std::to_string(c.frames.size()) + (c.offset ? " (encode offset " + std::to_string(c.offset) + " frames)" : "")},
    };
    auto add = [&](const char* name, double mean, double worst, int dec, const char* unit) {
        if (std::isnan(mean) && std::isnan(worst)) return;
        rows.push_back({name, "mean " + fmt(mean, dec) + unit + ", worst frame " + fmt(worst, dec) + unit});
    };
    add("PSNR (luma)", c.mean.psnrY, c.worst.psnrY, 2, " dB");
    add("PSNR (R'G'B')", c.mean.psnrRgb, c.worst.psnrRgb, 2, " dB");
    add("SSIM (luma)", c.mean.ssim, c.worst.ssim, 5, "");
    add("MS-SSIM", c.mean.msSsim, c.worst.msSsim, 5, "");
    add("Delta ICtCp (BT.2124)", c.mean.deltaItp, c.worst.deltaItp, 3, "");
    add("VMAF", c.mean.vmaf, c.worst.vmaf, 2, "");
    if (c.vmafScored) rows.push_back({"VMAF model", QFileInfo(QString::fromStdString(c.vmafModel)).fileName().toStdString()});
    for (const auto& [name, value] : rows) {
        p.drawText(margin, y + 10, QString::fromStdString(name));
        p.drawText(margin + 150, y + 10, QString::fromStdString(value));
        y += 15;
    }
    y += 12;
    // The graph: PSNR of each frame (dark blue, dB on the left), and VMAF where scored (green, 0 to 100 on the right).
    const QRectF g(margin + 30, y, W - 2 * margin - 60, 220);
    double lo = 100, hi = 0;
    for (const FrameQuality& q : c.frames)
        if (std::isfinite(q.psnrY)) {
            lo = std::min(lo, q.psnrY);
            hi = std::max(hi, q.psnrY);
        }
    if (hi < lo) lo = 20, hi = 60;
    lo = std::floor(std::min(lo, l.minPsnr > 0 ? l.minPsnr : lo) / 5) * 5;
    hi = std::max(lo + 5, std::ceil(hi / 5) * 5);
    const size_t n = std::max<size_t>(1, c.frames.size());
    auto xOf = [&](size_t i) { return g.left() + (n == 1 ? 0.5 : double(i) / double(n - 1)) * g.width(); };
    auto yOf = [&](double v) { return g.bottom() - (std::clamp(v, lo, hi) - lo) / (hi - lo) * g.height(); };
    p.setFont(small);
    p.setPen(QPen(QColor(200, 200, 200), 0.5));
    for (double v = lo; v <= hi + 1e-9; v += 5) {
        p.drawLine(QPointF(g.left(), yOf(v)), QPointF(g.right(), yOf(v)));
        p.drawText(QRectF(margin, yOf(v) - 5, 26, 10), Qt::AlignRight | Qt::AlignVCenter, QString::number(v));
    }
    if (l.minPsnr > 0) {
        p.setPen(QPen(QColor(220, 60, 50), 0.8, Qt::DashLine));
        p.drawLine(QPointF(g.left(), yOf(l.minPsnr)), QPointF(g.right(), yOf(l.minPsnr)));
    }
    QPainterPath psnr, vmaf;
    bool startedP = false, startedV = false;
    for (size_t i = 0; i < c.frames.size(); ++i) {
        const FrameQuality& q = c.frames[i];
        if (!std::isnan(q.psnrY)) {
            const QPointF at(xOf(i), std::isinf(q.psnrY) ? g.top() : yOf(q.psnrY));
            if (!startedP) psnr.moveTo(at), startedP = true;
            else psnr.lineTo(at);
        }
        if (!std::isnan(q.vmaf)) {
            const QPointF at(xOf(i), g.bottom() - std::clamp(q.vmaf, 0.0, 100.0) / 100 * g.height());
            if (!startedV) vmaf.moveTo(at), startedV = true;
            else vmaf.lineTo(at);
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(30, 80, 170), 0.8));
    p.drawPath(psnr);
    if (startedV) {
        p.setPen(QPen(QColor(40, 150, 70), 0.8));
        p.drawPath(vmaf);
        p.setPen(QPen(QColor(40, 150, 70), 0.5));
        for (int v = 0; v <= 100; v += 20)
            p.drawText(QRectF(g.right() + 4, g.bottom() - v / 100.0 * g.height() - 5, 26, 10), Qt::AlignLeft | Qt::AlignVCenter, QString::number(v));
    }
    p.setPen(QPen(Qt::black, 0.6));
    p.drawRect(g);
    if (!c.frames.empty()) {
        p.drawText(QRectF(g.left(), g.bottom() + 2, 120, 10), Qt::AlignLeft, QString::fromStdString(timecodeOf(c.frames.front().frame, s)));
        p.drawText(QRectF(g.right() - 120, g.bottom() + 2, 120, 10), Qt::AlignRight, QString::fromStdString(timecodeOf(c.frames.back().frame, s)));
    }
    p.drawText(QRectF(g.left(), g.bottom() + 14, g.width(), 10), Qt::AlignLeft,
               startedV ? QStringLiteral("PSNR of the luma (blue), dB; VMAF (green), 0 to 100") : QStringLiteral("PSNR of the luma, dB"));
    y = int(g.bottom()) + 40;
    // The worst frames, then the stretches past the limits.
    p.setFont(body);
    p.drawText(margin, y + 10, QStringLiteral("Worst frames:"));
    y += 16;
    for (FrameTime f : c.worstFrames) {
        const auto it = std::find_if(c.frames.begin(), c.frames.end(), [f](const FrameQuality& q) { return q.frame == f; });
        if (it == c.frames.end()) continue;
        std::string what = "PSNR " + fmt(it->psnrY, 2) + " dB";
        if (!std::isnan(it->ssim)) what += ", SSIM " + fmt(it->ssim, 4);
        if (!std::isnan(it->deltaItp)) what += ", Delta ICtCp " + fmt(it->deltaItp, 2);
        if (!std::isnan(it->vmaf)) what += ", VMAF " + fmt(it->vmaf, 2);
        p.drawText(margin, y + 10, QString::fromStdString(timecodeOf(f, s)));
        p.drawText(margin + 150, y + 10, QString::fromStdString(what));
        y += 14;
    }
    y += 10;
    const std::vector<QualitySpan> over = qualityOverLimits(c, l);
    const bool limits = l.minPsnr > 0 || l.minVmaf > 0 || l.maxDeltaItp > 0;
    if (limits) {
        p.drawText(margin, y + 10, over.empty() ? QStringLiteral("Nothing past the limits.") : QStringLiteral("Past the limits:"));
        y += 18;
        for (const QualitySpan& o : over) {
            if (y > H - margin) {
                writer.newPage();
                y = margin;
            }
            p.drawText(margin, y + 10, QString::fromStdString(timecodeOf(o.first, s) + " to " + timecodeOf(o.end, s)));
            QString text = QString::fromStdString(o.text);
            text.replace(QStringLiteral("ΔICtCp"), QStringLiteral("Delta ICtCp"));
            p.drawText(margin + 150, y + 10, text);
            y += 14;
        }
    }
    p.end();
    return true;
}

}  // namespace montage
