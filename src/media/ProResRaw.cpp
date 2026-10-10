#include "ProResRaw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "Image.h"
#include "render/ColorSpace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#ifdef MONTAGE_PRORES_RAW
#include <libavutil/raw_color_params.h>
#endif
}

namespace montage {

namespace {

void mul3(const double a[9], const double b[9], double out[9]) {
    double t[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    std::copy(t, t + 9, out);
}

bool invert3(const double m[9], double out[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12) return false;
    const double k = 1 / det;
    out[0] = (e * i - f * h) * k, out[1] = (c * h - b * i) * k, out[2] = (b * f - c * e) * k;
    out[3] = (f * g - d * i) * k, out[4] = (a * i - c * g) * k, out[5] = (c * d - a * f) * k;
    out[6] = (d * h - e * g) * k, out[7] = (b * g - a * h) * k, out[8] = (a * e - b * d) * k;
    return true;
}

// A light's chromaticity on the Planckian locus (Kim et al.'s cubic fit, 1667 K to 25000 K).
void planckian(double kelvin, double& x, double& y) {
    const double t = std::clamp(kelvin, 1667.0, 25000.0);
    x = t <= 4000 ? -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910
                  : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
    y = t <= 2222   ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
        : t <= 4000 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                    : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
}

// Bradford chromatic adaptation in XYZ from one white to another.
void bradford(double x1, double y1, double x2, double y2, double out[9]) {
    static const double B[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    double Binv[9];
    invert3(B, Binv);
    auto cone = [&](double x, double y, double c[3]) {
        const double X = x / y, Y = 1, Z = (1 - x - y) / y;
        for (int i = 0; i < 3; ++i) c[i] = B[i * 3] * X + B[i * 3 + 1] * Y + B[i * 3 + 2] * Z;
    };
    double s[3], d[3];
    cone(x1, y1, s);
    cone(x2, y2, d);
    const double D[9] = {d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
    double t[9];
    mul3(D, B, t);
    mul3(Binv, t, out);
}

// ACEScct in 16 bits of a linear value: below the toe by its line, above it from a table on the float's exponent and
// top 11 mantissa bits (taken at each bin's middle: within 1.5 of 65535).
class AcesCctTable {
public:
    AcesCctTable() {
        base_ = bits(kToe) >> 12;
        const uint32_t top = bits(256.0f) >> 12;
        table_.resize(top - base_);
        for (uint32_t i = 0; i < table_.size(); ++i) {
            const uint32_t b = ((base_ + i) << 12) | 0x800;
            float v;
            std::memcpy(&v, &b, sizeof v);
            table_[i] = encode((std::log2(double(v)) + 9.72) / 17.52);
        }
    }
    uint16_t operator()(float lin) const {
        if (!(lin > kToe)) return encode(10.5402377416545 * double(lin) + 0.0729055341958355);  // (and NaN: 0)
        if (lin >= 222.0f) return 65535;  // ACEScct's 1.0
        return table_[(bits(lin) >> 12) - base_];
    }

private:
    static constexpr float kToe = 0.0078125f;
    static uint32_t bits(float f) {
        uint32_t b;
        std::memcpy(&b, &f, sizeof b);
        return b;
    }
    static uint16_t encode(double v) { return v > 0 ? uint16_t(std::lround(std::min(v, 1.0) * 65535.0)) : 0; }
    uint32_t base_ = 0;
    std::vector<uint16_t> table_;
};

const AcesCctTable& acesCctTable() {
    static const AcesCctTable table;
    return table;
}

}  // namespace

bool proResRawAvailable() {
#ifdef MONTAGE_PRORES_RAW
    return true;
#else
    return false;
#endif
}

void proResRawSize(int width, int height, const int crop[4], bool half, int& outWidth, int& outHeight) {
    const int left = std::clamp(crop[0], 0, width), top = std::clamp(crop[1], 0, height);
    const int right = std::max(left, width - std::clamp(crop[2], 0, width)), bottom = std::max(top, height - std::clamp(crop[3], 0, height));
    if (!half) {
        outWidth = right - left;
        outHeight = bottom - top;
        return;
    }
    // Half size: whole 2x2 cells (an odd margin keeps one more column or row on the left and top, one less on the right
    // and bottom).
    outWidth = std::max(0, (right & ~1) - (left & ~1)) / 2;
    outHeight = std::max(0, (bottom & ~1) - (top & ~1)) / 2;
}

void proResRawBalance(const ProResRawColor& color, const RawSettings& settings, double mul[3]) {
    mul[0] = std::max(1e-6, color.wbRed), mul[1] = 1, mul[2] = std::max(1e-6, color.wbBlue);
    if (settings.temperature > 0) {
        // The chosen light's white as the camera sees it once balanced as shot (the camera's matrix takes the as-shot
        // light to D65), made the new white: c = camToXyz^-1 · Bradford(as shot → D65) · XYZ(light).
        double ax = 0.3127, ay = 0.3290, kx, ky;
        if (color.cct > 0) planckian(color.cct, ax, ay);
        planckian(settings.temperature, kx, ky);
        double adapt[9], inv[9];
        bradford(ax, ay, 0.3127, 0.3290, adapt);
        if (invert3(color.camToXyz, inv)) {
            const double xyz[3] = {kx / ky, 1, (1 - kx - ky) / ky};
            double a[3], c[3];
            for (int r = 0; r < 3; ++r) a[r] = adapt[r * 3] * xyz[0] + adapt[r * 3 + 1] * xyz[1] + adapt[r * 3 + 2] * xyz[2];
            for (int r = 0; r < 3; ++r) c[r] = inv[r * 3] * a[0] + inv[r * 3 + 1] * a[1] + inv[r * 3 + 2] * a[2];
            if (c[0] > 1e-6 && c[1] > 1e-6 && c[2] > 1e-6)
                for (int i = 0; i < 3; ++i) mul[i] *= c[1] / c[i];  // (green kept)
        }
    }
    mul[1] *= std::pow(2.0, -settings.tint / 150.0);  // plus is magenta: less green
}

void developProResRaw(const uint16_t* mosaic, int width, int height, ptrdiff_t stride, const ProResRawColor& color,
                      const RawSettings& settings, const int crop[4], uint16_t* rgb, ptrdiff_t rgbStride) {
    const bool half = settings.half;
    int outW = 0, outH = 0;
    proResRawSize(width, height, crop, half, outW, outH);
    if (outW <= 0 || outH <= 0 || !mosaic || !rgb) return;
    const int left = std::clamp(crop[0], 0, width), top = std::clamp(crop[1], 0, height);

    // The white balance on the mosaic (as shot, or the chosen light and tint), scaled so the smallest multiplier is 1:
    // every channel then clips where it saturates at no more than 1, so blown areas are white whatever the balance
    // (bright saturated colours lose what lies past the first channel's saturation, as LibRaw's Clip does). The
    // matrix takes the scaling back out.
    double mul[3];
    proResRawBalance(color, settings, mul);
    const double lift = 1 / std::min({mul[0], mul[1], mul[2]});
    for (double& m : mul) m *= lift;
    // One matrix from balanced camera RGB to AP1: the camera's to XYZ, then XYZ to AP1, with the gain and exposure.
    double m[9], ap1ToXyz[9], xyzToAp1[9];
    std::copy(color.camToXyz, color.camToXyz + 9, m);
    primariesToXyz(Primaries::Ap1, ap1ToXyz);
    invert3(ap1ToXyz, xyzToAp1);
    mul3(xyzToAp1, m, m);
    const double scale = std::max(0.0, color.gain) * std::pow(2.0, settings.exposure) / lift;
    float k[9];
    for (int i = 0; i < 9; ++i) k[i] = float(m[i] * scale);

    // Sites: the sensor's range to 0-1, balanced, clipped at 1.
    const float black = float(color.black) * 65535.0f, scaleSite = 1.0f / (float(std::max(1e-6, color.white - color.black)) * 65535.0f);
    const float mulR = float(mul[0]) * scaleSite, mulG = float(mul[1]) * scaleSite, mulB = float(mul[2]) * scaleSite;
    auto norm = [black](uint16_t v, float mul) { return std::clamp((float(v) - black) * mul, 0.0f, 1.0f); };
    const AcesCctTable& cct = acesCctTable();
    auto encode = [&](const float c[3], uint16_t* o) {
        for (int i = 0; i < 3; ++i) o[i] = cct(k[i * 3] * c[0] + k[i * 3 + 1] * c[1] + k[i * 3 + 2] * c[2]);
    };
    if (half) {
        // Each 2x2 cell one pixel, straight from the mosaic: its red, its two greens averaged, its blue.
        const int x0 = left & ~1, y0 = top & ~1;
        parallelRows(outH, [&](int r0, int r1) {
            for (int oy = r0; oy < r1; ++oy) {
                const uint16_t* a = mosaic + ptrdiff_t(y0 + oy * 2) * stride + x0;
                const uint16_t* b = a + stride;
                uint16_t* row = rgb + ptrdiff_t(oy) * rgbStride;
                for (int ox = 0; ox < outW; ++ox) {
                    const float c[3] = {norm(a[ox * 2], mulR), 0.5f * (norm(a[ox * 2 + 1], mulG) + norm(b[ox * 2], mulG)), norm(b[ox * 2 + 1], mulB)};
                    encode(c, row + size_t(ox) * 3);
                }
            }
        });
        return;
    }
    // Full size: the picture's sites and two around it (the recommended crop's margin is real sensor data) into a
    // plane of floats once, then a bilinear demosaic; past the mosaic's own edge, sites mirror on whole cells.
    const int px0 = std::max(0, left - 2), py0 = std::max(0, top - 2);
    const int px1 = std::min(width, left + outW + 2), py1 = std::min(height, top + outH + 2);
    const int pw = px1 - px0, ph = py1 - py0;
    std::vector<float> plane(size_t(pw) * size_t(ph));
    parallelRows(ph, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            const int ay = py0 + y;
            const uint16_t* in = mosaic + ptrdiff_t(ay) * stride + px0;
            float* o = &plane[size_t(y) * size_t(pw)];
            const float m0 = (ay & 1) ? mulG : mulR, m1 = (ay & 1) ? mulB : mulG;  // RGGB: R G on even rows, G B on odd
            for (int x = 0; x < pw; ++x) o[x] = norm(in[x], ((px0 + x) & 1) ? m1 : m0);
        }
    });
    auto mirror = [](int v, int n) { return v < 0 ? -v : v >= n ? 2 * n - 2 - v : v; };
    auto site = [&](int ax, int ay) {
        ax = std::clamp(mirror(ax, width), px0, px1 - 1), ay = std::clamp(mirror(ay, height), py0, py1 - 1);
        return plane[size_t(ay - py0) * size_t(pw) + size_t(ax - px0)];
    };
    parallelRows(outH, [&](int r0, int r1) {
        for (int oy = r0; oy < r1; ++oy) {
            uint16_t* row = rgb + ptrdiff_t(oy) * rgbStride;
            const int y = top + oy;
            for (int ox = 0; ox < outW; ++ox) {
                // Bilinear: the site's own colour, the others averaged from the nearest sites of theirs.
                const int x = left + ox;
                const int ch = (y & 1) ? ((x & 1) ? 2 : 1) : ((x & 1) ? 1 : 0);
                const float self = site(x, y);
                float c[3];
                if (ch == 1) {
                    // Green: red and blue are beside it, one pair across the row and one down the column.
                    const float across = 0.5f * (site(x - 1, y) + site(x + 1, y));
                    const float down = 0.5f * (site(x, y - 1) + site(x, y + 1));
                    const bool redRow = (y & 1) == 0;
                    c[0] = redRow ? across : down;
                    c[1] = self;
                    c[2] = redRow ? down : across;
                } else {
                    const float cross = 0.25f * (site(x - 1, y) + site(x + 1, y) + site(x, y - 1) + site(x, y + 1));
                    const float diag = 0.25f * (site(x - 1, y - 1) + site(x + 1, y - 1) + site(x - 1, y + 1) + site(x + 1, y + 1));
                    c[0] = ch == 0 ? self : diag;
                    c[1] = cross;
                    c[2] = ch == 0 ? diag : self;
                }
                encode(c, row + size_t(ox) * 3);
            }
        }
    });
}

void developProResRaw(const uint16_t* mosaic, int width, int height, ptrdiff_t stride, const ProResRawColor& color,
                      const RawSettings& settings, const int crop[4], DevelopedRaw& out) {
    proResRawSize(width, height, crop, settings.half, out.width, out.height);
    out.width = std::max(0, out.width), out.height = std::max(0, out.height);
    out.rgb.assign(size_t(out.width) * size_t(out.height) * 3, 0);
    developProResRaw(mosaic, width, height, stride, color, settings, crop, out.rgb.data(), ptrdiff_t(out.width) * 3);
}

bool parseProResRawFrame(const uint8_t* data, size_t size, ProResRawInfo& out) {
    // The frame header as FFmpeg's decoder reads it: the frame's size, "prrf", the header's length, then (from the
    // header's start) a reserved byte, the version, the vendor, width and height, the recommended crop (left, right,
    // top, bottom), the Bayer pattern, the sensel range, the white balance (red, blue), the colour matrix, the gain and
    // the white balance in kelvin. Black is 0x100.
    auto be16 = [&](size_t o) { return (uint32_t(data[o]) << 8) | data[o + 1]; };
    auto be32 = [&](size_t o) { return (uint32_t(data[o]) << 24) | (uint32_t(data[o + 1]) << 16) | (uint32_t(data[o + 2]) << 8) | data[o + 3]; };
    if (!data || size < 10 || std::memcmp(data + 4, "prrf", 4) != 0) return false;
    const size_t headerLength = be16(8);
    if (headerLength < 62 || 8 + headerLength > size) return false;
    const size_t h = 10, end = 8 + headerLength;  // (the length counts its own two bytes)
    auto f32 = [&](size_t o, double fallback) {
        if (h + o + 4 > end) return fallback;
        const uint32_t b = be32(h + o);
        float v;
        std::memcpy(&v, &b, sizeof v);
        return std::isfinite(v) ? double(v) : fallback;
    };
    const int w = int(be16(h + 6)), ht = int(be16(h + 8));
    if (w <= 0 || ht <= 0 || (w & 1) || (ht & 1) || (be16(h + 14) & 3) != 0) return false;  // RGGB only, as FFmpeg
    out.crop[0] = data[h + 10], out.crop[2] = data[h + 11], out.crop[1] = data[h + 12], out.crop[3] = data[h + 13];
    out.color.black = 256.0 / 65535;
    out.color.white = (double(be16(h + 16)) + 256.0) / 65535;
    out.color.wbRed = f32(18, 1);
    out.color.wbBlue = f32(22, 1);
    for (int i = 0; i < 9; ++i) out.color.camToXyz[i] = f32(26 + size_t(i) * 4, (i % 4 == 0) ? 1 : 0);
    out.color.gain = f32(62, 1);
    out.color.cct = h + 68 <= end ? be16(h + 66) : 0;
    if (!(out.color.wbRed > 0)) out.color.wbRed = 1;
    if (!(out.color.wbBlue > 0)) out.color.wbBlue = 1;
    if (!(out.color.gain > 0)) out.color.gain = 1;
    proResRawSize(w, ht, out.crop, false, out.width, out.height);
    return true;
}

bool proResRawColorOf(const AVFrame* frame, ProResRawColor& color, int crop[4]) {
#ifdef MONTAGE_PRORES_RAW
    const AVFrameSideData* sd = frame ? av_frame_get_side_data(frame, AV_FRAME_DATA_RAW_COLOR_PARAMS) : nullptr;
    if (!sd || size_t(sd->size) < sizeof(AVRawColorParams)) return false;
    const auto* p = reinterpret_cast<const AVRawColorParams*>(sd->data);
    if (p->type != AV_RAW_COLOR_PARAMS_PRORES_RAW) return false;
    auto q = [](AVRational r) { return r.den ? av_q2d(r) : 0.0; };
    color.black = q(p->black_level);
    color.white = q(p->white_level);
    color.cct = unsigned(std::max(0, int(p->wb_cct)));
    const AVProResRawColorParams& pr = p->codec.prores_raw;
    color.wbRed = q(pr.wb_red);
    color.wbBlue = q(pr.wb_blue);
    color.gain = q(pr.gain);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) color.camToXyz[r * 3 + c] = q(pr.color_matrix[r][c]);
    // Not said, or nonsense: as shot is no balance, unity gain.
    if (!(color.white > color.black)) color.black = 256.0 / 65535, color.white = 61568.0 / 65535;
    if (!(color.wbRed > 0)) color.wbRed = 1;
    if (!(color.wbBlue > 0)) color.wbBlue = 1;
    if (!(color.gain > 0)) color.gain = 1;
    crop[0] = int(frame->crop_left), crop[1] = int(frame->crop_top), crop[2] = int(frame->crop_right), crop[3] = int(frame->crop_bottom);
    return true;
#else
    (void)frame, (void)color, (void)crop;
    return false;
#endif
}

AVFrame* developProResRawFrame(const AVFrame* in, const RawSettings& settings, int targetWidth, int targetHeight) {
#ifdef MONTAGE_PRORES_RAW
    if (!in || in->format != AV_PIX_FMT_BAYER_RGGB16 || !in->data[0]) return nullptr;
    ProResRawColor color;
    int crop[4];
    if (!proResRawColorOf(in, color, crop)) return nullptr;
    RawSettings s = settings;
    int w = 0, h = 0;
    proResRawSize(in->width, in->height, crop, false, w, h);
    // Shown at half the size or less: each 2x2 cell one pixel (a quarter of the work, and as good a picture there).
    if (targetWidth > 0 && targetHeight > 0 && targetWidth * 2 <= w && targetHeight * 2 <= h) s.half = true;
    proResRawSize(in->width, in->height, crop, s.half, w, h);
    if (w <= 0 || h <= 0) return nullptr;
    AVFrame* out = av_frame_alloc();
    if (!out) return nullptr;
    out->format = AV_PIX_FMT_RGB48;
    out->width = w;
    out->height = h;
    out->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(out, 0) < 0) {
        av_frame_free(&out);
        return nullptr;
    }
    out->pts = in->pts;
    out->best_effort_timestamp = in->best_effort_timestamp;
    developProResRaw(reinterpret_cast<const uint16_t*>(in->data[0]), in->width, in->height, in->linesize[0] / 2, color, s, crop,
                     reinterpret_cast<uint16_t*>(out->data[0]), out->linesize[0] / 2);
    return out;
#else
    (void)in, (void)settings, (void)targetWidth, (void)targetHeight;
    return nullptr;
#endif
}

bool probeProResRaw(const std::string& path, ProResRawInfo& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
#ifdef MONTAGE_PRORES_RAW
    // The first frame's header says it all: no decoding.
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return fail("Cannot open " + path);
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmtGuard(fmt, [](AVFormatContext* f) { avformat_close_input(&f); });
    if (avformat_find_stream_info(fmt, nullptr) < 0) return fail("Cannot read " + path);
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0 || fmt->streams[stream]->codecpar->codec_id != AV_CODEC_ID_PRORES_RAW) return fail("Not ProRes RAW");
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    for (int packets = 0; packets < 64 && av_read_frame(fmt, pkt.get()) >= 0; ++packets) {
        const bool ours = pkt->stream_index == stream;
        const bool ok = ours && parseProResRawFrame(pkt->data, size_t(pkt->size), out);
        av_packet_unref(pkt.get());
        if (ours) return ok ? true : fail("The first ProRes RAW frame's header could not be read");
    }
    return fail("No ProRes RAW frame");
#else
    (void)path, (void)out;
    return fail("This FFmpeg cannot read ProRes RAW's colour (FFmpeg 9 is needed)");
#endif
}

}  // namespace montage
