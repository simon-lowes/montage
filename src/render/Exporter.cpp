#include "Exporter.h"

#include <QImage>
#include <QString>
#include <algorithm>
#include <cmath>

#include "Compositor.h"
#include "Processing.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace montage {

namespace {

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

ExportPreset preset(std::string name, std::string ext, std::string desc, std::string vcodec, std::string acodec, int crf,
                    std::string speed, std::string profile = {}, std::string pix = {}) {
    ExportPreset p;
    p.name = std::move(name);
    p.extension = std::move(ext);
    p.description = std::move(desc);
    p.settings.videoCodec = std::move(vcodec);
    p.settings.audioCodec = std::move(acodec);
    p.settings.crf = crf;
    p.settings.preset = std::move(speed);
    p.settings.profile = std::move(profile);
    p.settings.pixFmt = std::move(pix);
    return p;
}

AVPixelFormat defaultPixFmt(const ExportSettings& s, const AVCodec* codec) {
    if (!s.pixFmt.empty()) {
        AVPixelFormat f = av_get_pix_fmt(s.pixFmt.c_str());
        if (f != AV_PIX_FMT_NONE) return f;
    }
    const std::string& c = s.videoCodec;
    if (c == "prores_ks") return s.alpha || s.profile.rfind("4444", 0) == 0 ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "dnxhd") return (s.profile == "dnxhr_444") ? AV_PIX_FMT_YUV444P10LE
                             : (s.profile == "dnxhr_hqx") ? AV_PIX_FMT_YUV422P10LE
                                                          : AV_PIX_FMT_YUV422P;
    if (c == "mjpeg") return AV_PIX_FMT_YUVJ420P;
    if (c == "libvpx-vp9" && s.alpha) return AV_PIX_FMT_YUVA420P;
    if (c == "png") return AV_PIX_FMT_RGBA;
    (void)codec;
    return AV_PIX_FMT_YUV420P;
}

struct Output {
    AVFormatContext* oc = nullptr;
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    bool headerWritten = false;
    ~Output() {
        sws_freeContext(sws);
        av_frame_free(&vframe);
        av_frame_free(&aframe);
        av_packet_free(&pkt);
        avcodec_free_context(&vctx);
        avcodec_free_context(&actx);
        if (oc) {
            if (!(oc->oformat->flags & AVFMT_NOFILE) && oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
};

int drain(Output& o, AVCodecContext* ctx, AVStream* st) {
    for (;;) {
        int rc = avcodec_receive_packet(ctx, o.pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return rc;
        av_packet_rescale_ts(o.pkt, ctx->time_base, st->time_base);
        o.pkt->stream_index = st->index;
        rc = av_interleaved_write_frame(o.oc, o.pkt);
        if (rc < 0) return rc;
    }
}

}  // namespace

const std::vector<ExportPreset>& exportPresets() {
    static const std::vector<ExportPreset> presets = [] {
        std::vector<ExportPreset> v;
        v.push_back(preset("H.264 - High Quality", "mp4", "Mastering-grade H.264 (CRF 16), AAC 320 kbps", "libx264", "aac", 16, "slow"));
        v.push_back(preset("H.264 - YouTube / Vimeo", "mp4", "Web delivery, CRF 20", "libx264", "aac", 20, "medium"));
        v.push_back(preset("H.264 - Fast Draft", "mp4", "Quick review copy", "libx264", "aac", 28, "veryfast"));
        v.push_back(preset("H.265 / HEVC", "mp4", "Half the size of H.264 at similar quality", "libx265", "aac", 22, "medium"));
        v.push_back(preset("H.265 / HEVC 10-bit", "mp4", "10-bit HEVC for HDR-ready masters", "libx265", "aac", 20, "medium", "", "yuv420p10le"));
        v.push_back(preset("Apple ProRes 422 HQ", "mov", "Intermediate / mastering, 10-bit 4:2:2", "prores_ks", "pcm_s24le", 0, "", "hq"));
        v.push_back(preset("Apple ProRes 422 LT", "mov", "Lighter ProRes for offline / proxies", "prores_ks", "pcm_s16le", 0, "", "lt"));
        {
            ExportPreset p = preset("Apple ProRes 4444 (alpha)", "mov", "Keeps transparency for graphics", "prores_ks", "pcm_s24le", 0, "", "4444");
            p.settings.alpha = true;
            v.push_back(p);
        }
        v.push_back(preset("Avid DNxHR HQ", "mov", "Avid-friendly 8-bit 4:2:2 intermediate", "dnxhd", "pcm_s24le", 0, "", "dnxhr_hq"));
        v.push_back(preset("VP9 (WebM)", "webm", "Open web format, Opus audio", "libvpx-vp9", "libopus", 32, "good"));
        v.push_back(preset("AV1 (SVT-AV1)", "mp4", "Next-generation efficiency", "libsvtav1", "aac", 32, "8"));
        v.push_back(preset("Audio - WAV 24-bit", "wav", "Uncompressed mixdown", "none", "pcm_s24le", 0, ""));
        v.push_back(preset("Audio - AAC (M4A)", "m4a", "Compressed mixdown", "none", "aac", 0, ""));
        return v;
    }();
    return presets;
}

const ExportPreset* findExportPreset(const std::string& name) {
    for (const auto& p : exportPresets())
        if (p.name == name) return &p;
    return nullptr;
}

bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    FrameTime in = s.in >= 0 ? s.in : 0;
    FrameTime out = s.out >= 0 ? s.out : seq.duration();
    if (out <= in) return fail("Nothing to export: the range is empty");
    const bool wantVideo = s.videoCodec != "none" && !s.videoCodec.empty();
    const bool wantAudio = s.audioCodec != "none" && !s.audioCodec.empty();
    if (!wantVideo && !wantAudio) return fail("No video or audio codec selected");

    Output o;
    int rc = avformat_alloc_output_context2(&o.oc, nullptr, nullptr, s.path.c_str());
    if (rc < 0 || !o.oc) return fail("Unknown output format for " + s.path);
    o.pkt = av_packet_alloc();
    const AVRational fps{seq.fps.num, seq.fps.den};
    int W = s.width > 0 ? s.width : seq.width;
    int H = s.height > 0 ? s.height : seq.height;
    W += W & 1;  // most codecs need even dimensions
    H += H & 1;
    const int sr = s.sampleRate > 0 ? s.sampleRate : seq.sampleRate;

    if (wantVideo) {
        const AVCodec* codec = avcodec_find_encoder_by_name(s.videoCodec.c_str());
        if (!codec) return fail("Video encoder not available: " + s.videoCodec);
        o.vst = avformat_new_stream(o.oc, nullptr);
        o.vctx = avcodec_alloc_context3(codec);
        o.vctx->width = W;
        o.vctx->height = H;
        o.vctx->time_base = av_inv_q(fps);
        o.vctx->framerate = fps;
        o.vctx->sample_aspect_ratio = AVRational{1, 1};
        o.vctx->pix_fmt = defaultPixFmt(s, codec);
        o.vctx->gop_size = s.gop > 0 ? s.gop : std::max(1, int(std::lround(seq.fpsValue() * 2)));
        if (s.gop == 1) o.vctx->max_b_frames = 0;
        o.vctx->color_primaries = AVCOL_PRI_BT709;
        o.vctx->color_trc = AVCOL_TRC_BT709;
        o.vctx->colorspace = AVCOL_SPC_BT709;
        o.vctx->color_range = s.videoCodec == "mjpeg" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        o.vctx->thread_count = 0;
        if (s.videoBitrate > 0) o.vctx->bit_rate = s.videoBitrate;
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) o.vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        const std::string& c = s.videoCodec;
        if (c == "libx264" || c == "libx265") {
            if (s.videoBitrate <= 0) av_dict_set_int(&opts, "crf", s.crf, 0);
            if (!s.preset.empty()) av_dict_set(&opts, "preset", s.preset.c_str(), 0);
            if (c == "libx265") {
                av_dict_set(&opts, "x265-params", "log-level=error", 0);
                o.vctx->codec_tag = MKTAG('h', 'v', 'c', '1');  // plays in QuickTime / Apple devices
            }
        } else if (c == "prores_ks") {
            av_dict_set(&opts, "profile", s.profile.empty() ? "hq" : s.profile.c_str(), 0);
            av_dict_set(&opts, "vendor", "apl0", 0);
        } else if (c == "dnxhd") {
            av_dict_set(&opts, "profile", s.profile.empty() ? "dnxhr_hq" : s.profile.c_str(), 0);
        } else if (c == "libvpx-vp9") {
            if (s.videoBitrate <= 0) {
                av_dict_set_int(&opts, "crf", s.crf, 0);
                o.vctx->bit_rate = 0;
            }
            av_dict_set(&opts, "row-mt", "1", 0);
            av_dict_set(&opts, "deadline", s.preset.empty() ? "good" : s.preset.c_str(), 0);
            av_dict_set(&opts, "cpu-used", "4", 0);
        } else if (c == "libsvtav1") {
            if (s.videoBitrate <= 0) av_dict_set_int(&opts, "crf", s.crf, 0);
            av_dict_set(&opts, "preset", s.preset.empty() ? "8" : s.preset.c_str(), 0);
        } else if (c == "mjpeg") {
            o.vctx->flags |= AV_CODEC_FLAG_QSCALE;
            o.vctx->global_quality = FF_QP2LAMBDA * 3;
        }
        rc = avcodec_open2(o.vctx, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) return fail("Cannot open video encoder: " + averr(rc));
        avcodec_parameters_from_context(o.vst->codecpar, o.vctx);
        o.vst->time_base = o.vctx->time_base;
        o.vst->avg_frame_rate = fps;
        o.vframe = av_frame_alloc();
        o.vframe->format = o.vctx->pix_fmt;
        o.vframe->width = W;
        o.vframe->height = H;
        o.vframe->color_range = o.vctx->color_range;
        o.vframe->colorspace = o.vctx->colorspace;
        if ((rc = av_frame_get_buffer(o.vframe, 0)) < 0) return fail("Out of memory");
    }

    int audioFrameSize = 1024;
    if (wantAudio) {
        const AVCodec* codec = avcodec_find_encoder_by_name(s.audioCodec.c_str());
        if (!codec) return fail("Audio encoder not available: " + s.audioCodec);
        o.ast = avformat_new_stream(o.oc, nullptr);
        o.actx = avcodec_alloc_context3(codec);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&o.actx->ch_layout, &stereo);
        o.actx->sample_rate = sr;
        if (s.audioCodec == "libopus" && sr != 48000) o.actx->sample_rate = 48000;
        // Pick a supported sample format, preferring float planar.
        AVSampleFormat want = AV_SAMPLE_FMT_FLTP;
        if (s.audioCodec == "pcm_s16le") want = AV_SAMPLE_FMT_S16;
        else if (s.audioCodec == "pcm_s24le") want = AV_SAMPLE_FMT_S32;
        else if (s.audioCodec == "libopus") want = AV_SAMPLE_FMT_FLT;
        o.actx->sample_fmt = want;
        if (codec->sample_fmts) {
            bool ok = false;
            for (const AVSampleFormat* f = codec->sample_fmts; *f != AV_SAMPLE_FMT_NONE; ++f) ok |= (*f == want);
            if (!ok) o.actx->sample_fmt = codec->sample_fmts[0];
        }
        if (s.audioCodec == "aac" || s.audioCodec == "libopus") o.actx->bit_rate = s.audioBitrate;
        o.actx->time_base = AVRational{1, o.actx->sample_rate};
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) o.actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((rc = avcodec_open2(o.actx, codec, nullptr)) < 0) return fail("Cannot open audio encoder: " + averr(rc));
        avcodec_parameters_from_context(o.ast->codecpar, o.actx);
        o.ast->time_base = o.actx->time_base;
        if (o.actx->frame_size > 0) audioFrameSize = o.actx->frame_size;
        o.aframe = av_frame_alloc();
    }
    if (o.actx && o.actx->sample_rate != sr && s.audioCodec != "libopus") return fail("Unsupported sample rate");

    if (!(o.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((rc = avio_open(&o.oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("Cannot write " + s.path + ": " + averr(rc));
    }
    av_dict_set(&o.oc->metadata, "encoder", "Montage", 0);
    if ((rc = avformat_write_header(o.oc, nullptr)) < 0) return fail("Cannot write header: " + averr(rc));
    o.headerWritten = true;

    const int mixRate = o.actx ? o.actx->sample_rate : sr;
    Sequence mixSeq = seq;  // mixer runs at the encoder's rate
    mixSeq.sampleRate = mixRate;
    AudioMixer mixer;
    std::vector<float> fifo;
    std::vector<float> mixBuf;
    int64_t audioPts = 0;
    int64_t audioCursor = int64_t(std::llround(double(in) * mixRate / seq.fpsValue()));
    std::vector<uint16_t> rgba16;

    auto encodeAudio = [&](bool final) -> bool {
        while (fifo.size() >= size_t(audioFrameSize) * 2 || (final && !fifo.empty())) {
            int n = std::min<int>(audioFrameSize, int(fifo.size() / 2));
            av_frame_unref(o.aframe);
            o.aframe->nb_samples = n;
            o.aframe->format = o.actx->sample_fmt;
            o.aframe->sample_rate = o.actx->sample_rate;
            av_channel_layout_copy(&o.aframe->ch_layout, &o.actx->ch_layout);
            if (av_frame_get_buffer(o.aframe, 0) < 0) return false;
            const float* srcp = fifo.data();
            switch (o.actx->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP: {
                    auto* l = reinterpret_cast<float*>(o.aframe->data[0]);
                    auto* r = reinterpret_cast<float*>(o.aframe->data[1]);
                    for (int i = 0; i < n; ++i) {
                        l[i] = srcp[i * 2];
                        r[i] = srcp[i * 2 + 1];
                    }
                    break;
                }
                case AV_SAMPLE_FMT_FLT:
                    std::copy(srcp, srcp + n * 2, reinterpret_cast<float*>(o.aframe->data[0]));
                    break;
                case AV_SAMPLE_FMT_S16: {
                    auto* d = reinterpret_cast<int16_t*>(o.aframe->data[0]);
                    for (int i = 0; i < n * 2; ++i) d[i] = int16_t(std::lround(std::clamp(srcp[i], -1.0f, 1.0f) * 32767.0f));
                    break;
                }
                case AV_SAMPLE_FMT_S32: {
                    auto* d = reinterpret_cast<int32_t*>(o.aframe->data[0]);
                    for (int i = 0; i < n * 2; ++i)
                        d[i] = int32_t(std::llround(double(std::clamp(srcp[i], -1.0f, 1.0f)) * 2147483647.0));
                    break;
                }
                case AV_SAMPLE_FMT_S16P: {
                    auto* l = reinterpret_cast<int16_t*>(o.aframe->data[0]);
                    auto* r = reinterpret_cast<int16_t*>(o.aframe->data[1]);
                    for (int i = 0; i < n; ++i) {
                        l[i] = int16_t(std::lround(std::clamp(srcp[i * 2], -1.0f, 1.0f) * 32767.0f));
                        r[i] = int16_t(std::lround(std::clamp(srcp[i * 2 + 1], -1.0f, 1.0f) * 32767.0f));
                    }
                    break;
                }
                default: return false;
            }
            o.aframe->pts = audioPts;
            audioPts += n;
            fifo.erase(fifo.begin(), fifo.begin() + n * 2);
            if (avcodec_send_frame(o.actx, o.aframe) < 0) return false;
            if (drain(o, o.actx, o.ast) < 0) return false;
        }
        return true;
    };

    const int64_t total = out - in;
    RenderOptions ro;
    ro.scale = double(W) / seq.width;
    ro.highQuality = true;
    ro.useProxies = s.useProxies;
    for (FrameTime f = in; f < out; ++f) {
        if (cancel && cancel->load()) return fail("Export cancelled");
        if (wantVideo) {
            Image img = s.alpha ? renderSequenceFrame(p, seq, f, ro) : renderProgramFrame(p, seq, f, ro);
            toRgba16(img, rgba16);
            AVPixelFormat srcFmt = AV_PIX_FMT_RGBA64LE;
            o.sws = sws_getCachedContext(o.sws, img.width, img.height, srcFmt, W, H, o.vctx->pix_fmt,
                                         SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
            if (!o.sws) return fail("Cannot convert to the encoder pixel format");
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(o.vctx->pix_fmt);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB))
                sws_setColorspaceDetails(o.sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709),
                                         o.vctx->color_range == AVCOL_RANGE_JPEG ? 1 : 0, 0, 1 << 16, 1 << 16);
            if (av_frame_make_writable(o.vframe) < 0) return fail("Out of memory");
            const uint8_t* srcData[4] = {reinterpret_cast<const uint8_t*>(rgba16.data()), nullptr, nullptr, nullptr};
            int srcStride[4] = {img.width * 8, 0, 0, 0};
            sws_scale(o.sws, srcData, srcStride, 0, img.height, o.vframe->data, o.vframe->linesize);
            o.vframe->pts = f - in;
            if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("Video encoding failed: " + averr(rc));
            if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing video failed: " + averr(rc));
        }
        if (wantAudio) {
            int64_t target = int64_t(std::llround(double(f + 1) * mixRate / seq.fpsValue()));
            int n = int(target - audioCursor);
            if (n > 0) {
                mixBuf.resize(size_t(n) * 2);
                mixer.mix(p, mixSeq, audioCursor, n, mixBuf.data());
                audioCursor = target;
                fifo.insert(fifo.end(), mixBuf.begin(), mixBuf.end());
                if (!encodeAudio(false)) return fail("Audio encoding failed");
            }
        }
        if (progress && ((f - in) % 5 == 0 || f + 1 == out)) progress(double(f - in + 1) / double(total), f);
    }
    if (wantAudio) {
        if (!encodeAudio(true)) return fail("Audio encoding failed");
        avcodec_send_frame(o.actx, nullptr);
        drain(o, o.actx, o.ast);
    }
    if (wantVideo) {
        avcodec_send_frame(o.vctx, nullptr);
        drain(o, o.vctx, o.vst);
    }
    if ((rc = av_write_trailer(o.oc)) < 0) return fail("Cannot finalise file: " + averr(rc));
    return true;
}

bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error) {
    RenderOptions ro;
    ro.highQuality = true;
    Image img = renderProgramFrame(p, seq, t, ro);
    std::vector<uint8_t> rgba = toRgba8(img);
    QImage qi(rgba.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888);
    if (!qi.save(QString::fromStdString(path))) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace montage
