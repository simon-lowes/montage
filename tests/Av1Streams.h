// Montage tests — AV1 streams for the film grain tests: pictures encoded by an AV1 encoder of this FFmpeg, and decoded
// by dav1d through FFmpeg, either with the stream's film grain put on by the decoder or left off and its parameters
// given beside each picture.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/film_grain_params.h>
#include <libavutil/pixdesc.h>
}

#include "render/Av1FilmGrain.h"

namespace av1streams {

struct Stream {
    std::vector<std::vector<uint8_t>> packets;  // a temporal unit each, in decoding order
    std::vector<uint8_t> config;                // the encoder's codec configuration
};

// `frames` pictures of w by h (4:2:0, `depth` bits), each drawn by `draw` (planes of the frame to fill), encoded by the
// named encoder with these options.
inline bool encode(const char* encoder, const std::vector<std::pair<std::string, std::string>>& options, int w, int h, int depth, int frames,
                   const std::function<void(int, AVFrame*)>& draw, Stream& out, std::string* error) {
    out = Stream{};
    auto no = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) return no("no such encoder");
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w, c->height = h;
    c->pix_fmt = depth > 8 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
    c->time_base = AVRational{1, 24}, c->framerate = AVRational{24, 1};
    c->gop_size = 250;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* opts = nullptr;
    for (const auto& o : options) av_dict_set(&opts, o.first.c_str(), o.second.c_str(), 0);
    const int opened = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (opened < 0) {
        avcodec_free_context(&c);
        return no("the encoder did not open");
    }
    if (c->extradata_size > 0) out.config.assign(c->extradata, c->extradata + c->extradata_size);
    AVFrame* f = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    f->format = c->pix_fmt, f->width = w, f->height = h;
    bool ok = av_frame_get_buffer(f, 0) >= 0;
    auto take = [&] {
        while (ok && avcodec_receive_packet(c, pkt) >= 0) {
            out.packets.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; ok && i < frames; ++i) {
        ok = av_frame_make_writable(f) >= 0;
        if (!ok) break;
        draw(i, f);
        f->pts = i;
        ok = avcodec_send_frame(c, f) >= 0;
        take();
    }
    if (ok) avcodec_send_frame(c, nullptr);
    take();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    return ok && int(out.packets.size()) == frames ? true : no("the encoder did not give a packet for each picture");
}

struct Picture {
    int width = 0, height = 0, depth = 8;
    bool subX = true, subY = true;
    std::vector<uint16_t> plane[3];
    bool hasGrain = false;        // its film grain parameters came beside it
    montage::Av1FilmGrain grain;  // (as a frame header carries them)
    montage::Av1GrainPicture forGrain() {
        montage::Av1GrainPicture p;
        for (int i = 0; i < 3; ++i) p.plane[i] = plane[i].empty() ? nullptr : plane[i].data(), p.stride[i] = i ? (width + int(subX)) >> int(subX) : width;
        p.width = width, p.height = height, p.bitDepth = depth, p.subX = subX, p.subY = subY, p.mono = plane[1].empty();
        return p;
    }
};

// FFmpeg's film grain side data as a frame header's parameters.
inline montage::Av1FilmGrain grainOf(const AVFilmGrainParams* p) {
    montage::Av1FilmGrain g;
    const AVFilmGrainAOMParams& a = p->codec.aom;
    g.apply = true;
    g.seed = uint16_t(p->seed);
    g.numY = a.num_y_points;
    for (int i = 0; i < g.numY; ++i) g.yValue[i] = a.y_points[i][0], g.yScaling[i] = a.y_points[i][1];
    g.chromaFromLuma = a.chroma_scaling_from_luma;
    g.numCb = a.num_uv_points[0], g.numCr = a.num_uv_points[1];
    for (int i = 0; i < g.numCb; ++i) g.cbValue[i] = a.uv_points[0][i][0], g.cbScaling[i] = a.uv_points[0][i][1];
    for (int i = 0; i < g.numCr; ++i) g.crValue[i] = a.uv_points[1][i][0], g.crScaling[i] = a.uv_points[1][i][1];
    g.scalingShift = a.scaling_shift;
    g.arLag = a.ar_coeff_lag;
    for (int i = 0; i < 24; ++i) g.arY[i] = a.ar_coeffs_y[i];
    for (int i = 0; i < 25; ++i) g.arCb[i] = a.ar_coeffs_uv[0][i], g.arCr[i] = a.ar_coeffs_uv[1][i];
    g.arShift = a.ar_coeff_shift;
    g.grainScaleShift = a.grain_scale_shift;
    g.cbMult = a.uv_mult[0] + 128, g.cbLumaMult = a.uv_mult_luma[0] + 128, g.cbOffset = a.uv_offset[0] + 256;
    g.crMult = a.uv_mult[1] + 128, g.crLumaMult = a.uv_mult_luma[1] + 128, g.crOffset = a.uv_offset[1] + 256;
    g.overlap = a.overlap_flag;
    g.clipRestricted = a.limit_output_range;
    return g;
}

// A stream's pictures as dav1d decodes them: with the film grain put on (`grainBeside` false), or without it and its
// parameters beside each picture.
inline bool decode(const Stream& s, bool grainBeside, std::vector<Picture>& out) {
    out.clear();
    const AVCodec* codec = avcodec_find_decoder_by_name("libdav1d");
    if (!codec) return false;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!s.config.empty()) {
        c->extradata = static_cast<uint8_t*>(av_mallocz(s.config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(c->extradata, s.config.data(), s.config.size());
        c->extradata_size = int(s.config.size());
    }
    if (grainBeside) c->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
    c->thread_count = 2;
    if (avcodec_open2(c, codec, nullptr) < 0) {
        avcodec_free_context(&c);
        return false;
    }
    AVFrame* fr = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    bool ok = true;
    auto take = [&] {
        while (avcodec_receive_frame(c, fr) >= 0) {
            Picture d;
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(fr->format));
            d.width = fr->width, d.height = fr->height, d.depth = desc->comp[0].depth;
            d.subX = desc->log2_chroma_w != 0, d.subY = desc->log2_chroma_h != 0;
            for (int p = 0; p < 3 && p < desc->nb_components; ++p) {
                const int w = p ? (d.width + int(d.subX)) >> int(d.subX) : d.width, h = p ? (d.height + int(d.subY)) >> int(d.subY) : d.height;
                d.plane[p].resize(size_t(w) * size_t(h));
                for (int y = 0; y < h; ++y) {
                    const uint8_t* row = fr->data[p] + ptrdiff_t(y) * fr->linesize[p];
                    for (int x = 0; x < w; ++x) {
                        uint16_t v = row[x];
                        if (d.depth > 8) std::memcpy(&v, row + 2 * x, 2);
                        d.plane[p][size_t(y) * size_t(w) + size_t(x)] = v;
                    }
                }
            }
            if (const AVFrameSideData* sd = av_frame_get_side_data(fr, AV_FRAME_DATA_FILM_GRAIN_PARAMS)) {
                d.hasGrain = true;
                d.grain = grainOf(reinterpret_cast<const AVFilmGrainParams*>(sd->data));
            }
            out.push_back(std::move(d));
            av_frame_unref(fr);
        }
    };
    for (const auto& p : s.packets) {
        if (av_new_packet(pkt, int(p.size())) < 0) {
            ok = false;
            break;
        }
        std::memcpy(pkt->data, p.data(), p.size());
        if (avcodec_send_packet(c, pkt) < 0) ok = false;
        av_packet_unref(pkt);
        take();
    }
    avcodec_send_packet(c, nullptr);
    take();
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&c);
    return ok;
}

}  // namespace av1streams
