// Montage — ProRes pictures one at a time (render/ProRes.h).
#include "render/ProRes.h"

#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

struct Encoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int width = 0, height = 0;
    ~Impl() {
        sws_freeContext(sws);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
    }
};

Encoder::Encoder() = default;
Encoder::~Encoder() = default;

bool Encoder::open(int width, int height, int profile, const std::string& imfColour, std::string* error) {
    d_.reset();
    ColourCodes codes;
    if (!colourCodes(imfColour, codes)) return failWith(error, "ProRes pictures here are rec709, p3d65-pq, rec2020-pq or rec2020-hlg");
    if (profile < 1 || profile > 6) return failWith(error, "Unknown ProRes profile");
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || width % 2) return failWith(error, "ProRes pictures here are an even width up to 8192");
    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) return failWith(error, "This build of FFmpeg has no ProRes encoder");
    auto d = std::make_unique<Impl>();
    d->width = width;
    d->height = height;
    d->ctx = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    d->packet = av_packet_alloc();
    if (!d->ctx || !d->frame || !d->packet) return failWith(error, "Out of memory");
    const AVPixelFormat format = fourFourFour(profile) ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV422P10LE;
    AVCodecContext* c = d->ctx;
    c->width = width;
    c->height = height;
    c->pix_fmt = format;
    c->time_base = {1, 24};
    // (prores_ks takes its profile from its own option, not the context's; FFmpeg numbers them 0 Proxy ... 5 4444 XQ)
    c->profile = profile - 1;
    if (av_opt_set_int(c->priv_data, "profile", profile - 1, 0) < 0) return failWith(error, "FFmpeg's ProRes encoder has no such profile");
    c->color_primaries = AVColorPrimaries(codes.primaries);
    c->color_trc = AVColorTransferCharacteristic(codes.transfer);
    c->colorspace = AVColorSpace(codes.matrix);
    c->color_range = AVCOL_RANGE_MPEG;
    c->field_order = AV_FIELD_PROGRESSIVE;
    c->thread_count = std::max(1, int(std::thread::hardware_concurrency()));
    c->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(c, codec, nullptr) < 0) return failWith(error, "FFmpeg's ProRes encoder would not open");
    d->frame->format = format;
    d->frame->width = width;
    d->frame->height = height;
    d->frame->color_primaries = c->color_primaries;
    d->frame->color_trc = c->color_trc;
    d->frame->colorspace = c->colorspace;
    d->frame->color_range = AVCOL_RANGE_MPEG;
    if (av_frame_get_buffer(d->frame, 0) < 0) return failWith(error, "Out of memory");
    d->sws = sws_getContext(width, height, AV_PIX_FMT_RGB48LE, width, height, format, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP | SWS_BITEXACT,
                            nullptr, nullptr, nullptr);
    if (!d->sws) return failWith(error, "Cannot convert pictures for ProRes");
    // Full-range R'G'B' to narrow-range Y'CbCr through the colour's matrix.
    sws_setColorspaceDetails(d->sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(swsMatrix(codes.matrix)), 0, 0, 1 << 16, 1 << 16);
    d_ = std::move(d);
    return true;
}

bool Encoder::encode(const uint16_t* rgb, std::vector<uint8_t>& out, std::string* error) {
    if (!d_) return failWith(error, "The ProRes encoder is not open");
    if (av_frame_make_writable(d_->frame) < 0) return failWith(error, "Out of memory");
    const uint8_t* src[4] = {reinterpret_cast<const uint8_t*>(rgb), nullptr, nullptr, nullptr};
    const int stride[4] = {d_->width * 6, 0, 0, 0};
    sws_scale(d_->sws, src, stride, 0, d_->height, d_->frame->data, d_->frame->linesize);
    d_->frame->pts = d_->frame->pts + 1;
    if (avcodec_send_frame(d_->ctx, d_->frame) < 0) return failWith(error, "FFmpeg's ProRes encoder refused a picture");
    // (slice threads only: each picture's packet comes back before the next is sent)
    const int r = avcodec_receive_packet(d_->ctx, d_->packet);
    if (r < 0) return failWith(error, "FFmpeg's ProRes encoder gave no frame");
    out.assign(d_->packet->data, d_->packet->data + d_->packet->size);
    av_packet_unref(d_->packet);
    return true;
}

bool decode(const uint8_t* data, size_t size, std::vector<uint16_t>& rgb, int& width, int& height, std::string* error) {
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
    if (c && packet && frame) {
        c->thread_count = 1;  // (one frame at a time, often several decoding at once)
        if (avcodec_open2(c, codec, nullptr) >= 0 && av_new_packet(packet, int(size)) >= 0) {
            std::memcpy(packet->data, data, size);
            packet->flags |= AV_PKT_FLAG_KEY;
            if (avcodec_send_packet(c, packet) >= 0 && (avcodec_receive_frame(c, frame) >= 0 ||
                                                        (avcodec_send_packet(c, nullptr) >= 0 && avcodec_receive_frame(c, frame) >= 0))) {
                width = frame->width;
                height = frame->height;
                sws = sws_getContext(width, height, AVPixelFormat(frame->format), width, height, AV_PIX_FMT_RGB48LE,
                                     SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_BITEXACT, nullptr, nullptr, nullptr);
                if (sws) {
                    sws_setColorspaceDetails(sws, sws_getCoefficients(swsMatrix(h.matrix)), frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
                                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
                    rgb.resize(size_t(width) * size_t(height) * 3);
                    uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(rgb.data()), nullptr, nullptr, nullptr};
                    const int stride[4] = {width * 6, 0, 0, 0};
                    sws_scale(sws, frame->data, frame->linesize, 0, height, dst, stride);
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
