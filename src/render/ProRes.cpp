// Montage — ProRes pictures one at a time (render/ProRes.h).
#include "render/ProRes.h"

#include "media/ProResEncoder.h"

#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace montage::prores {

namespace {

const char* const kNames[] = {"", "proxy", "lt", "422", "hq", "4444", "4444xq"};
const char* const kTitles[] = {"", "ProRes 422 Proxy", "ProRes 422 LT", "ProRes 422", "ProRes 422 HQ", "ProRes 4444", "ProRes 4444 XQ"};

// The scaler's matrix for a matrix_coefficients code (1 BT.709, 9 BT.2020 non-constant luminance, 5 and 6 BT.601).
int swsMatrix(int code) {
    switch (code) {
    case 9:
    case 10: return SWS_CS_BT2020;
    case 5:
    case 6: return SWS_CS_ITU601;
    default: return SWS_CS_ITU709;
    }
}

bool failWith(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

}  // namespace

int profileOf(const std::string& name) {
    for (int i = 1; i <= 6; ++i)
        if (name == kNames[i]) return i;
    if (name == "standard") return 3;
    return 0;
}

std::string profileName(int profile) { return profile >= 1 && profile <= 6 ? kNames[profile] : ""; }
std::string profileTitle(int profile) { return profile >= 1 && profile <= 6 ? kTitles[profile] : ""; }

bool parseFrameHeader(const uint8_t* p, size_t n, FrameHeader& out) {
    // frame_size, 'icpf', then the frame header: its size, a reserved byte, the version, the encoder's id, the size,
    // chroma format and interlace mode, aspect ratio and frame rate codes, the three colour codes, the alpha type.
    if (n < 26 || std::memcmp(p + 4, "icpf", 4) != 0) return false;
    out.frameSize = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    const int headerSize = p[8] << 8 | p[9];
    if (headerSize < 18 || size_t(8 + headerSize) > n) return false;
    out.width = p[16] << 8 | p[17];
    out.height = p[18] << 8 | p[19];
    out.chroma = p[20] >> 6;
    out.interlace = (p[20] >> 2) & 3;
    out.primaries = p[22];
    out.transfer = p[23];
    out.matrix = p[24];
    out.alpha = p[25] & 15;
    return true;
}

bool colourCodes(const std::string& c, ColourCodes& out) {
    if (c == "rec709") out = {1, 1, 1};
    else if (c == "p3d65-pq") out = {12, 16, 1};
    else if (c == "rec2020-pq") out = {9, 16, 9};
    else if (c == "rec2020-hlg") out = {9, 18, 9};
    else return false;
    return true;
}

bool encoderAvailable() { return avcodec_find_encoder_by_name("prores_ks") != nullptr; }

// R'G'B' (16 bits, full range) to Y'CbCr at 12 bits through a matrix_coefficients code's matrix, narrow range (Y' 256
// to 3760, the colour differences 256 to 3840) or full (0 to 4095, the differences about 2048), as ITU-T H.273 has it.
void toYCbCr12(const uint16_t* rgb, int width, int height, int matrix, bool fullRange, std::vector<uint16_t>& y, std::vector<uint16_t>& cb,
               std::vector<uint16_t>& cr) {
    const double kr = matrix == 9 || matrix == 10 ? 0.2627 : matrix == 5 || matrix == 6 ? 0.299 : 0.2126;
    const double kb = matrix == 9 || matrix == 10 ? 0.0593 : matrix == 5 || matrix == 6 ? 0.114 : 0.0722;
    const double kg = 1 - kr - kb;
    const double ys = fullRange ? 4095 : 3504, yo = fullRange ? 0 : 256, cs = fullRange ? 4095 : 3584;
    const size_t n = size_t(width) * size_t(height);
    y.resize(n);
    cb.resize(n);
    cr.resize(n);
    const int threads = std::max(1, std::min(int(std::thread::hardware_concurrency()), height / 64 + 1));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (size_t i = n * size_t(t) / size_t(threads), end = n * size_t(t + 1) / size_t(threads); i < end; ++i) {
                const double r = rgb[i * 3] / 65535.0, g = rgb[i * 3 + 1] / 65535.0, b = rgb[i * 3 + 2] / 65535.0;
                const double l = kr * r + kg * g + kb * b;
                y[i] = uint16_t(std::clamp(std::lround(yo + ys * l), 0L, 4095L));
                cb[i] = uint16_t(std::clamp(std::lround(2048 + cs * (b - l) / (2 * (1 - kb))), 0L, 4095L));
                cr[i] = uint16_t(std::clamp(std::lround(2048 + cs * (r - l) / (2 * (1 - kr))), 0L, 4095L));
            }
        });
    for (std::thread& t : pool) t.join();
}

struct Encoder::Impl {
    std::unique_ptr<OwnEncoder> own;  // (12 bits)
    int matrix = 1;
    std::vector<uint16_t> y, cb, cr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int width = 0, height = 0;
    Coding coding;
    ~Impl() {
        sws_freeContext(sws);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
    }
};

Encoder::Encoder() = default;
Encoder::~Encoder() = default;

bool Encoder::open(int width, int height, int profile, const std::string& imfColour, const Coding& coding, std::string* error) {
    d_.reset();
    ColourCodes codes;
    if (!colourCodes(imfColour, codes)) return failWith(error, "ProRes pictures here are rec709, p3d65-pq, rec2020-pq or rec2020-hlg");
    if (profile < 1 || profile > 6) return failWith(error, "Unknown ProRes profile");
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || width % 2) return failWith(error, "ProRes pictures here are an even width up to 8192");
    if (coding.fields < 0 || coding.fields > 2) return failWith(error, "Fields are 0 (progressive), 1 (the top first) or 2 (the bottom first)");
    if (coding.fields && height % 2) return failWith(error, "Interlaced ProRes pictures here have an even height");
    if (coding.alpha && !fourFourFour(profile)) return failWith(error, "Only ProRes 4444 and 4444 XQ carry alpha");
    if (coding.bits != 10 && coding.bits != 12) return failWith(error, "ProRes pictures here are 10 or 12 bits");
    if (coding.bits == 12) {
        // Montage's own encoder: 4444 and 4444 XQ at 12 bits.
        if (!fourFourFour(profile)) return failWith(error, "12-bit ProRes is 4444 or 4444 XQ");
        auto d = std::make_unique<Impl>();
        d->width = width;
        d->height = height;
        d->coding = coding;
        d->matrix = codes.matrix;
        OwnSettings os;
        os.width = width;
        os.height = height;
        os.profile = profile;
        os.fields = coding.fields;
        os.primaries = codes.primaries;
        os.transfer = codes.transfer;
        os.matrix = codes.matrix;
        os.alpha = coding.alpha;
        os.frameRateCode = frameRateCode(coding.fps);
        os.threads = coding.threads;
        d->own = std::make_unique<OwnEncoder>();
        if (!d->own->open(os, error)) return false;
        d_ = std::move(d);
        return true;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) return failWith(error, "This build of FFmpeg has no ProRes encoder");
    auto d = std::make_unique<Impl>();
    d->width = width;
    d->height = height;
    d->coding = coding;
    d->ctx = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    d->packet = av_packet_alloc();
    if (!d->ctx || !d->frame || !d->packet) return failWith(error, "Out of memory");
    const AVPixelFormat format = coding.alpha ? AV_PIX_FMT_YUVA444P10LE : fourFourFour(profile) ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV422P10LE;
    const AVColorRange range = coding.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    AVCodecContext* c = d->ctx;
    c->width = width;
    c->height = height;
    c->pix_fmt = format;
    c->time_base = {1, 24};
    // (prores_ks takes its profile from its own option, not the context's; FFmpeg numbers them 0 Proxy ... 5 4444 XQ)
    c->profile = profile - 1;
    if (av_opt_set_int(c->priv_data, "profile", profile - 1, 0) < 0) return failWith(error, "FFmpeg's ProRes encoder has no such profile");
    if (coding.alpha && av_opt_set_int(c->priv_data, "alpha_bits", 16, 0) < 0) return failWith(error, "FFmpeg's ProRes encoder codes no alpha");
    c->color_primaries = AVColorPrimaries(codes.primaries);
    c->color_trc = AVColorTransferCharacteristic(codes.transfer);
    c->colorspace = AVColorSpace(codes.matrix);
    c->color_range = range;
    c->field_order = coding.fields == 1 ? AV_FIELD_TT : coding.fields == 2 ? AV_FIELD_BB : AV_FIELD_PROGRESSIVE;
    // (two pictures a frame, each a field, the first in time first: RDD 36's interlace_mode 1 or 2)
    if (coding.fields) c->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
    c->thread_count = std::max(1, int(std::thread::hardware_concurrency()));
    c->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(c, codec, nullptr) < 0) return failWith(error, "FFmpeg's ProRes encoder would not open");
    d->frame->format = format;
    d->frame->width = width;
    d->frame->height = height;
    d->frame->color_primaries = c->color_primaries;
    d->frame->color_trc = c->color_trc;
    d->frame->colorspace = c->colorspace;
    d->frame->color_range = range;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    if (coding.fields) d->frame->flags |= AV_FRAME_FLAG_INTERLACED | (coding.fields == 1 ? AV_FRAME_FLAG_TOP_FIELD_FIRST : 0);
#else
    d->frame->interlaced_frame = coding.fields != 0;
    d->frame->top_field_first = coding.fields == 1;
#endif
    if (av_frame_get_buffer(d->frame, 0) < 0) return failWith(error, "Out of memory");
    // (the colour planes only: alpha is filled in from its own plane)
    const AVPixelFormat colourFormat = coding.alpha ? AV_PIX_FMT_YUV444P10LE : format;
    d->sws = sws_getContext(width, height, AV_PIX_FMT_RGB48LE, width, height, colourFormat, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP | SWS_BITEXACT,
                            nullptr, nullptr, nullptr);
    if (!d->sws) return failWith(error, "Cannot convert pictures for ProRes");
    // Full-range R'G'B' to Y'CbCr (narrow range, or full for QE.2) through the colour's matrix.
    sws_setColorspaceDetails(d->sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(swsMatrix(codes.matrix)), coding.fullRange ? 1 : 0, 0,
                             1 << 16, 1 << 16);
    d_ = std::move(d);
    return true;
}

bool Encoder::encode(const uint16_t* rgb, const uint16_t* alpha, std::vector<uint8_t>& out, std::string* error) {
    if (!d_) return failWith(error, "The ProRes encoder is not open");
    if (alpha && !d_->coding.alpha) return failWith(error, "The ProRes encoder was opened without alpha");
    if (d_->own) {
        toYCbCr12(rgb, d_->width, d_->height, d_->matrix, d_->coding.fullRange, d_->y, d_->cb, d_->cr);
        PlanarPicture pic;
        pic.y = d_->y.data();
        pic.cb = d_->cb.data();
        pic.cr = d_->cr.data();
        pic.yStride = pic.cStride = d_->width;
        pic.alpha = alpha;
        pic.aStride = d_->width;
        return d_->own->encode(pic, out, error);
    }
    if (av_frame_make_writable(d_->frame) < 0) return failWith(error, "Out of memory");
    const uint8_t* src[4] = {reinterpret_cast<const uint8_t*>(rgb), nullptr, nullptr, nullptr};
    const int stride[4] = {d_->width * 6, 0, 0, 0};
    sws_scale(d_->sws, src, stride, 0, d_->height, d_->frame->data, d_->frame->linesize);
    if (d_->coding.alpha) {
        // 16 bits to the encoder's 10 (it widens them to the 16 it codes losslessly); none given: opaque.
        for (int y = 0; y < d_->height; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(d_->frame->data[3] + size_t(y) * size_t(d_->frame->linesize[3]));
            const uint16_t* a = alpha ? alpha + size_t(y) * size_t(d_->width) : nullptr;
            for (int x = 0; x < d_->width; ++x) row[x] = a ? uint16_t((uint32_t(a[x]) * 1023 + 32767) / 65535) : uint16_t(1023);
        }
    }
    d_->frame->pts = d_->frame->pts + 1;
    if (avcodec_send_frame(d_->ctx, d_->frame) < 0) return failWith(error, "FFmpeg's ProRes encoder refused a picture");
    // (slice threads only: each picture's packet comes back before the next is sent)
    const int r = avcodec_receive_packet(d_->ctx, d_->packet);
    if (r < 0) return failWith(error, "FFmpeg's ProRes encoder gave no frame");
    out.assign(d_->packet->data, d_->packet->data + d_->packet->size);
    av_packet_unref(d_->packet);
    return true;
}

bool Encoder::encodeFields(const uint16_t* first, const uint16_t* firstAlpha, const uint16_t* second, const uint16_t* secondAlpha,
                           std::vector<uint8_t>& out, std::string* error) {
    if (!d_) return failWith(error, "The ProRes encoder is not open");
    if (!d_->coding.fields) return failWith(error, "The ProRes encoder was opened for progressive frames");
    // (the first field's lines, the top ones when the top comes first, from the first picture; the others from the second)
    const int W = d_->width, H = d_->height;
    const int firstParity = d_->coding.fields == 1 ? 0 : 1;
    std::vector<uint16_t> rgb(size_t(W) * size_t(H) * 3), alpha;
    const bool withAlpha = d_->coding.alpha && (firstAlpha || secondAlpha);
    if (withAlpha) alpha.resize(size_t(W) * size_t(H));
    for (int y = 0; y < H; ++y) {
        const bool fromFirst = (y & 1) == firstParity;
        const uint16_t* src = (fromFirst ? first : second) + size_t(y) * size_t(W) * 3;
        std::copy(src, src + size_t(W) * 3, rgb.begin() + std::ptrdiff_t(size_t(y) * size_t(W) * 3));
        if (withAlpha) {
            const uint16_t* a = fromFirst ? firstAlpha : secondAlpha;
            uint16_t* dst = alpha.data() + size_t(y) * size_t(W);
            if (a) std::copy(a + size_t(y) * size_t(W), a + size_t(y + 1) * size_t(W), dst);
            else std::fill(dst, dst + W, uint16_t(65535));
        }
    }
    return encode(rgb.data(), withAlpha ? alpha.data() : nullptr, out, error);
}

std::vector<uint8_t> Encoder::lastQuantization() const { return d_ && d_->own ? d_->own->lastQuantization() : std::vector<uint8_t>{}; }

bool decode(const uint8_t* data, size_t size, std::vector<uint16_t>& rgb, int& width, int& height, std::string* error) {
    return decode(data, size, false, rgb, nullptr, width, height, error);
}

bool decode(const uint8_t* data, size_t size, bool fullRange, std::vector<uint16_t>& rgb, std::vector<uint16_t>* alpha, int& width, int& height,
            std::string* error) {
    FrameHeader h;
    if (!parseFrameHeader(data, size, h)) return failWith(error, "Not a ProRes frame");
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PRORES);
    if (!codec) return failWith(error, "This build of FFmpeg has no ProRes decoder");
    AVCodecContext* c = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    bool ok = false;
    std::string why = "FFmpeg could not decode the ProRes frame";
    if (alpha) alpha->clear();
    if (c && packet && frame) {
        c->thread_count = 1;  // (one frame at a time, often several decoding at once)
        // (FFmpeg decodes at the depth the profile's tag names: 4:4:4 at 12 bits, as the 4444 profiles are)
        c->codec_tag = h.chroma == 3 ? MKTAG('a', 'p', '4', 'h') : MKTAG('a', 'p', 'c', 'h');
        if (avcodec_open2(c, codec, nullptr) >= 0 && av_new_packet(packet, int(size)) >= 0) {
            std::memcpy(packet->data, data, size);
            packet->flags |= AV_PKT_FLAG_KEY;
            if (avcodec_send_packet(c, packet) >= 0 && (avcodec_receive_frame(c, frame) >= 0 ||
                                                        (avcodec_send_packet(c, nullptr) >= 0 && avcodec_receive_frame(c, frame) >= 0))) {
                width = frame->width;
                height = frame->height;
                const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
                sws = sws_getContext(width, height, AVPixelFormat(frame->format), width, height, AV_PIX_FMT_RGB48LE,
                                     SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_BITEXACT, nullptr, nullptr, nullptr);
                if (sws && pd) {
                    sws_setColorspaceDetails(sws, sws_getCoefficients(swsMatrix(h.matrix)), fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                             1 << 16, 1 << 16);
                    rgb.resize(size_t(width) * size_t(height) * 3);
                    uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(rgb.data()), nullptr, nullptr, nullptr};
                    const int stride[4] = {width * 6, 0, 0, 0};
                    sws_scale(sws, frame->data, frame->linesize, 0, height, dst, stride);
                    if (alpha && (pd->flags & AV_PIX_FMT_FLAG_ALPHA) && frame->data[3]) {
                        // (10 or 12 bits widened to 16)
                        const int depth = pd->comp[3].depth;
                        alpha->resize(size_t(width) * size_t(height));
                        for (int y = 0; y < height; ++y) {
                            const auto* row = reinterpret_cast<const uint16_t*>(frame->data[3] + size_t(y) * size_t(frame->linesize[3]));
                            for (int x = 0; x < width; ++x) {
                                const uint32_t v = row[x];
                                (*alpha)[size_t(y) * size_t(width) + size_t(x)] = uint16_t(depth >= 16 ? v : (v << (16 - depth)) | (v >> (2 * depth - 16)));
                            }
                        }
                    }
                    ok = true;
                }
            }
        }
    }
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&c);
    return ok || failWith(error, why);
}

}  // namespace montage::prores
