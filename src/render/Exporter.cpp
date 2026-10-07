#include "Exporter.h"

#include <QImage>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/EditOps.h"
#include "Processing.h"
#include "media/HwAccel.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>
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

bool hasSuffix(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// `c` is the encoder that will run (hardware families already resolved).
AVPixelFormat defaultPixFmt(const ExportSettings& s, const std::string& c) {
    if (hasSuffix(c, "_qsv") || hasSuffix(c, "_mf")) return AV_PIX_FMT_NV12;
    if (!s.pixFmt.empty()) {
        AVPixelFormat f = av_get_pix_fmt(s.pixFmt.c_str());
        if (f != AV_PIX_FMT_NONE) return f;
    }
    if (c == "prores_ks") return s.alpha || s.profile.rfind("4444", 0) == 0 ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "dnxhd") return (s.profile == "dnxhr_444") ? AV_PIX_FMT_YUV444P10LE
                             : (s.profile == "dnxhr_hqx") ? AV_PIX_FMT_YUV422P10LE
                                                          : AV_PIX_FMT_YUV422P;
    if (c == "mjpeg") return AV_PIX_FMT_YUVJ420P;
    if (c == "libvpx-vp9" && s.alpha) return AV_PIX_FMT_YUVA420P;
    if (c == "png") return AV_PIX_FMT_RGBA;
    return AV_PIX_FMT_YUV420P;
}

struct ColorTags {
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic trc = AVCOL_TRC_BT709;
    AVColorSpace matrix = AVCOL_SPC_BT709;
};
ColorTags colorTags(const ColorSpace& c) {
    ColorTags t;
    if (c.primaries == Primaries::Bt2020) {
        t.primaries = AVCOL_PRI_BT2020;
        t.matrix = AVCOL_SPC_BT2020_NCL;
        t.trc = AVCOL_TRC_BT2020_10;
    } else if (c.primaries == Primaries::P3D65) {
        t.primaries = AVCOL_PRI_SMPTE432;
    }
    if (c.transfer == Transfer::Pq) t.trc = AVCOL_TRC_SMPTE2084;
    else if (c.transfer == Transfer::Hlg) t.trc = AVCOL_TRC_ARIB_STD_B67;
    else if (c.transfer == Transfer::Srgb) t.trc = AVCOL_TRC_IEC61966_2_1;
    return t;
}

// The encoder's pixel formats, or nullptr if it does not say.
const AVPixelFormat* supportedPixFmts(const AVCodecContext* ctx, const AVCodec* codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* cfg = nullptr;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, nullptr) >= 0)
        return static_cast<const AVPixelFormat*>(cfg);
    return nullptr;
#else
    (void)ctx;
    return codec->pix_fmts;
#endif
}

// HDR wants at least 10 bits: the encoder's 10-bit counterpart of `f`, if it has one.
AVPixelFormat tenBitFormat(AVPixelFormat f, const AVCodecContext* ctx, const AVCodec* codec) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    if (!d || d->comp[0].depth >= 10) return f;
    const AVPixelFormat* fmts = supportedPixFmts(ctx, codec);
    if (!fmts) return f;
    const bool chroma422 = d->log2_chroma_h == 0 && d->log2_chroma_w == 1;
    const AVPixelFormat wanted[] = {chroma422 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE,
                                    AV_PIX_FMT_YUV420P10LE};
    for (AVPixelFormat w : wanted)
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == w) return w;
    return f;
}

// ISO 639-2 code for a caption track's ISO 639-1 language (MP4 and MKV store three letters).
const char* iso639_2(const std::string& code) {
    static const std::pair<const char*, const char*> table[] = {
        {"en", "eng"}, {"es", "spa"}, {"fr", "fra"}, {"de", "deu"}, {"it", "ita"}, {"pt", "por"}, {"nl", "nld"},
        {"pl", "pol"}, {"sv", "swe"}, {"da", "dan"}, {"no", "nor"}, {"fi", "fin"}, {"cs", "ces"}, {"el", "ell"},
        {"tr", "tur"}, {"ru", "rus"}, {"uk", "ukr"}, {"ar", "ara"}, {"he", "heb"}, {"hi", "hin"}, {"id", "ind"},
        {"vi", "vie"}, {"th", "tha"}, {"ja", "jpn"}, {"ko", "kor"}, {"zh", "zho"}, {"cy", "cym"}, {"ga", "gle"}};
    for (const auto& [two, three] : table)
        if (code == two) return three;
    return code.size() == 3 ? code.c_str() : nullptr;
}

// Text subtitle encoders take ASS events and need an ASS header with a Default style.
const char* kAssHeader =
    "[Script Info]\r\nScriptType: v4.00+\r\nPlayResX: 384\r\nPlayResY: 288\r\nScaledBorderAndShadow: yes\r\n\r\n"
    "[V4+ Styles]\r\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
    "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
    "MarginR, MarginV, Encoding\r\nStyle: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1"
    "\r\n\r\n[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";

std::string assText(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\n') out += "\\N";
        else if (c == '\\' || c == '{' || c == '}') (out += '\\') += c;
        else if (c != '\r') out += c;
    }
    return out;
}

struct Output {
    AVFormatContext* oc = nullptr;
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    AVCodecContext* sctx = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    AVStream* sst = nullptr;
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
        avcodec_free_context(&sctx);
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
        v.push_back(preset("H.264 - Hardware", "mp4",
                           "Fast export on the GPU / media engine (VideoToolbox, NVENC, Quick Sync, AMF); x264 if there is none",
                           "hw_h264", "aac", 20, "medium"));
        v.push_back(preset("H.265 - Hardware", "mp4", "Hardware HEVC (VideoToolbox, NVENC, Quick Sync, AMF); x265 if there is none",
                           "hw_hevc", "aac", 22, "medium"));
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

namespace {

bool exportImpl(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                const std::atomic<bool>* cancel, std::string* error, bool& opened, std::string* encoderUsed) {
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

    const ColorSpace& seqSpace = sequenceColorSpace(seq);
    const ColorSpace* chosen = findColorSpace(s.colorSpace);
    const ColorSpace& outSpace = chosen && !chosen->sceneReferred ? *chosen : seqSpace;
    const bool pq = outSpace.transfer == Transfer::Pq;
    const double peakNits = std::clamp(seq.hdrPeakNits, 100.0, 10000.0);
    const int maxFall = int(std::lround(std::min(peakNits, 400.0)));
    if (wantVideo) {
        std::string c = s.videoCodec;
        const bool hardware = c == "hw_h264" || c == "hw_hevc";
        if (hardware) {
            const std::string family = c.substr(3);
            c = pickHwEncoder(family, W, H, fps.num, fps.den);
            if (c.empty()) c = family == "hevc" ? "libx265" : "libx264";  // no hardware encoder here
        }
        if (encoderUsed) *encoderUsed = c;
        const AVCodec* codec = avcodec_find_encoder_by_name(c.c_str());
        if (!codec) return fail("Video encoder not available: " + c);
        o.vst = avformat_new_stream(o.oc, nullptr);
        o.vctx = avcodec_alloc_context3(codec);
        o.vctx->width = W;
        o.vctx->height = H;
        o.vctx->time_base = av_inv_q(fps);
        o.vctx->framerate = fps;
        o.vctx->sample_aspect_ratio = AVRational{1, 1};
        o.vctx->pix_fmt = defaultPixFmt(s, c);
        if (outSpace.hdr() && s.pixFmt.empty()) o.vctx->pix_fmt = tenBitFormat(o.vctx->pix_fmt, o.vctx, codec);
        o.vctx->gop_size = s.gop > 0 ? s.gop : std::max(1, int(std::lround(seq.fpsValue() * 2)));
        if (s.gop == 1) o.vctx->max_b_frames = 0;
        const ColorTags tags = colorTags(outSpace);
        o.vctx->color_primaries = tags.primaries;
        o.vctx->color_trc = tags.trc;
        o.vctx->colorspace = tags.matrix;
        o.vctx->color_range = c == "mjpeg" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        o.vctx->thread_count = 0;
        if (s.videoBitrate > 0) o.vctx->bit_rate = s.videoBitrate;
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) o.vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        const bool hwEncoder = hasSuffix(c, "_videotoolbox") || hasSuffix(c, "_nvenc") || hasSuffix(c, "_qsv") ||
                               hasSuffix(c, "_amf") || hasSuffix(c, "_mf");
        if (hwEncoder) {
            // Hardware encoders are rate controlled: aim for a bitrate by pixel rate.
            if (s.videoBitrate <= 0) {
                const double bitsPerPixel = c.rfind("hevc", 0) == 0 ? 0.10 : 0.16;
                o.vctx->bit_rate = int64_t(std::clamp(double(W) * H * seq.fpsValue() * bitsPerPixel, 2e6, 150e6));
            }
            if (hasSuffix(c, "_videotoolbox")) av_dict_set(&opts, "allow_sw", "1", 0);
            if (hasSuffix(c, "_nvenc")) {
                av_dict_set(&opts, "preset", "p5", 0);
                av_dict_set(&opts, "rc", "vbr", 0);
            }
            if (hasSuffix(c, "_amf")) av_dict_set(&opts, "quality", "quality", 0);
            if (hasSuffix(c, "_mf")) av_dict_set(&opts, "hw_encoding", "1", 0);  // not Microsoft's software MFT
            if (c.rfind("hevc", 0) == 0) o.vctx->codec_tag = MKTAG('h', 'v', 'c', '1');
        } else if (c == "libx264" || c == "libx265") {
            if (s.videoBitrate <= 0) av_dict_set_int(&opts, "crf", s.crf, 0);
            if (!s.preset.empty()) av_dict_set(&opts, "preset", s.preset.c_str(), 0);
            if (c == "libx265") {
                std::string params = "log-level=error";
                if (pq) {
                    // HDR10: mastering display (P3-D65 at the sequence's peak) and light levels.
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                                  ":hdr10=1:repeat-headers=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)"
                                  "WP(15635,16450)L(%lld,1):max-cll=%d,%d",
                                  static_cast<long long>(std::llround(peakNits * 10000)), int(std::lround(peakNits)), maxFall);
                    params += buf;
                }
                av_dict_set(&opts, "x265-params", params.c_str(), 0);
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
        o.vframe->color_primaries = o.vctx->color_primaries;
        o.vframe->color_trc = o.vctx->color_trc;
        if ((rc = av_frame_get_buffer(o.vframe, 0)) < 0) return fail("Out of memory");
        if (pq) {
            // The same HDR10 metadata on the stream (MP4/MOV mdcv and clli boxes, MKV
            // colour elements) and on every frame (for encoders that read it there).
            AVMasteringDisplayMetadata md{};
            const double prim[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};  // R, G, B
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 2; ++j) md.display_primaries[i][j] = av_make_q(int(std::lround(prim[i][j] * 50000)), 50000);
            md.white_point[0] = av_make_q(15635, 50000);
            md.white_point[1] = av_make_q(16450, 50000);
            md.max_luminance = av_make_q(int(std::lround(peakNits)), 1);
            md.min_luminance = av_make_q(1, 10000);
            md.has_primaries = md.has_luminance = 1;
            AVContentLightMetadata cl{};
            cl.MaxCLL = unsigned(std::lround(peakNits));
            cl.MaxFALL = unsigned(maxFall);
            if (AVPacketSideData* sd = av_packet_side_data_new(&o.vst->codecpar->coded_side_data, &o.vst->codecpar->nb_coded_side_data,
                                                               AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof md, 0))
                std::memcpy(sd->data, &md, sizeof md);
            if (AVPacketSideData* sd = av_packet_side_data_new(&o.vst->codecpar->coded_side_data, &o.vst->codecpar->nb_coded_side_data,
                                                               AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof cl, 0))
                std::memcpy(sd->data, &cl, sizeof cl);
            if (AVMasteringDisplayMetadata* f = av_mastering_display_metadata_create_side_data(o.vframe)) *f = md;
            if (AVContentLightMetadata* f = av_content_light_metadata_create_side_data(o.vframe)) *f = cl;
        }
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
        const AVSampleFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const void* cfg = nullptr;
        if (avcodec_get_supported_config(o.actx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, nullptr) >= 0)
            fmts = static_cast<const AVSampleFormat*>(cfg);
#else
        fmts = codec->sample_fmts;
#endif
        if (fmts) {
            bool ok = false;
            for (const AVSampleFormat* f = fmts; *f != AV_SAMPLE_FMT_NONE; ++f) ok |= (*f == want);
            if (!ok) o.actx->sample_fmt = fmts[0];
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

    // Captions as a subtitle stream, in the text format the container takes.
    const CaptionTrack* captions = (s.embedCaptions || s.burnInCaptions) ? captionTrackFor(seq, s.captionTrack) : nullptr;
    if (s.embedCaptions && captions && !captions->captions.empty()) {
        AVCodecID id = AV_CODEC_ID_NONE;
        for (AVCodecID c : {AV_CODEC_ID_MOV_TEXT, AV_CODEC_ID_SUBRIP, AV_CODEC_ID_WEBVTT})
            if (avformat_query_codec(o.oc->oformat, c, FF_COMPLIANCE_NORMAL) == 1) {
                id = c;
                break;
            }
        if (id == AV_CODEC_ID_NONE) return fail("This file type cannot hold captions; export them as a sidecar file instead");
        const AVCodec* codec = avcodec_find_encoder(id);
        if (!codec) return fail("Caption encoder not available");
        o.sctx = avcodec_alloc_context3(codec);
        o.sctx->time_base = AVRational{1, 1000};
        const size_t headerLen = std::char_traits<char>::length(kAssHeader);
        o.sctx->subtitle_header = static_cast<uint8_t*>(av_mallocz(headerLen + 1));
        std::copy(kAssHeader, kAssHeader + headerLen, o.sctx->subtitle_header);
        o.sctx->subtitle_header_size = int(headerLen);
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) o.sctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((rc = avcodec_open2(o.sctx, codec, nullptr)) < 0) return fail("Cannot open caption encoder: " + averr(rc));
        o.sst = avformat_new_stream(o.oc, nullptr);
        avcodec_parameters_from_context(o.sst->codecpar, o.sctx);
        o.sst->time_base = o.sctx->time_base;
        if (const char* lang = iso639_2(captions->language)) av_dict_set(&o.sst->metadata, "language", lang, 0);
        if (!captions->name.empty()) av_dict_set(&o.sst->metadata, "title", captions->name.c_str(), 0);
    }

    if (!(o.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((rc = avio_open(&o.oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("Cannot write " + s.path + ": " + averr(rc));
        opened = true;
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

    // Writes the captions that start before frame `until` (export range only).
    size_t nextCaption = captions && o.sst ? captionIndexAt(*captions, in) : 0;
    std::vector<uint8_t> subBuf(1 << 16);
    auto writeCaptions = [&](FrameTime until) -> bool {
        if (!o.sst) return true;
        const double msPerFrame = 1000.0 / seq.fpsValue();
        for (; nextCaption < captions->captions.size() && captions->captions[nextCaption].start < until; ++nextCaption) {
            const Caption& c = captions->captions[nextCaption];
            const FrameTime a = std::max(c.start, in), b = std::min(c.end, out);
            if (b <= a) continue;
            const int64_t startMs = std::llround(double(a - in) * msPerFrame);
            const int64_t durMs = std::max<int64_t>(1, std::llround(double(b - a) * msPerFrame));
            AVSubtitle sub{};
            sub.format = 1;  // text
            sub.pts = av_rescale_q(startMs, AVRational{1, 1000}, AV_TIME_BASE_Q);
            sub.end_display_time = uint32_t(durMs);
            sub.num_rects = 1;
            sub.rects = static_cast<AVSubtitleRect**>(av_mallocz(sizeof(AVSubtitleRect*)));
            sub.rects[0] = static_cast<AVSubtitleRect*>(av_mallocz(sizeof(AVSubtitleRect)));
            sub.rects[0]->type = SUBTITLE_ASS;
            sub.rects[0]->ass = av_strdup(("0,0,Default,,0,0,0,," + assText(c.text)).c_str());
            const int n = avcodec_encode_subtitle(o.sctx, subBuf.data(), int(subBuf.size()), &sub);
            avsubtitle_free(&sub);
            if (n < 0) return false;
            av_packet_unref(o.pkt);
            if (av_new_packet(o.pkt, n) < 0) return false;
            std::copy(subBuf.begin(), subBuf.begin() + n, o.pkt->data);
            o.pkt->pts = o.pkt->dts = av_rescale_q(startMs, AVRational{1, 1000}, o.sst->time_base);
            o.pkt->duration = av_rescale_q(durMs, AVRational{1, 1000}, o.sst->time_base);
            o.pkt->stream_index = o.sst->index;
            if (av_interleaved_write_frame(o.oc, o.pkt) < 0) return false;
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
        if (!writeCaptions(f + 1)) return fail("Writing captions failed");
        if (wantVideo) {
            Image img = s.alpha ? renderSequenceFrame(p, seq, f, ro) : renderProgramFrame(p, seq, f, ro);
            if (s.burnInCaptions && captions) drawCaption(img, *captions, f, &seqSpace);
            convertColor(img, seqSpace, outSpace, peakNits);
            toRgba16(img, rgba16);
            AVPixelFormat srcFmt = AV_PIX_FMT_RGBA64LE;
            o.sws = sws_getCachedContext(o.sws, img.width, img.height, srcFmt, W, H, o.vctx->pix_fmt,
                                         SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
            if (!o.sws) return fail("Cannot convert to the encoder pixel format");
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(o.vctx->pix_fmt);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB))
                sws_setColorspaceDetails(o.sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                         sws_getCoefficients(o.vctx->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU709),
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
    // Flush the encoders; errors here (e.g. a full disk) must fail the export.
    if (wantAudio) {
        if (!encodeAudio(true)) return fail("Audio encoding failed");
        if ((rc = avcodec_send_frame(o.actx, nullptr)) < 0 || (rc = drain(o, o.actx, o.ast)) < 0)
            return fail("Finishing audio failed: " + averr(rc));
    }
    if (wantVideo) {
        if ((rc = avcodec_send_frame(o.vctx, nullptr)) < 0 || (rc = drain(o, o.vctx, o.vst)) < 0)
            return fail("Finishing video failed: " + averr(rc));
    }
    if ((rc = av_write_trailer(o.oc)) < 0) return fail("Cannot finalise file: " + averr(rc));
    if (o.oc->pb && o.oc->pb->error < 0) return fail("Writing the file failed: " + averr(o.oc->pb->error));
    return true;
}

}  // namespace

bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed) {
    bool opened = false;
    bool ok = exportImpl(p, seq, s, progress, cancel, error, opened, encoderUsed);
    // Never leave a truncated file behind (the output is closed by now), but
    // don't touch an existing file if we failed before writing to it.
    if (!ok && opened) std::remove(s.path.c_str());
    return ok;
}

bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress, const std::atomic<bool>* cancel) {
    const Clip* c = edit::clipById(seq, clip);
    if (!c) {
        if (error) *error = "No such clip";
        return false;
    }
    // The clip alone, at the start of an otherwise empty copy of the sequence.
    Sequence alone = seq;
    alone.videoTracks.assign(1, Track{});
    alone.videoTracks[0].kind = TrackKind::Video;
    alone.audioTracks.assign(1, Track{});
    alone.audioTracks[0].kind = TrackKind::Audio;
    alone.buses.clear();
    alone.masterEffects.clear();
    alone.masterVolumeDb = 0;
    alone.captionTracks.clear();
    Clip copy = *c;
    copy.start = 0;
    copy.linkGroup = 0;
    copy.audio.params.clear();  // volume and pan stay live on the clip
    alone.audioTracks[0].clips.push_back(copy);
    ExportSettings st;
    st.path = path;
    st.videoCodec = "none";
    st.audioCodec = "pcm_s24le";
    st.in = 0;
    st.out = copy.duration;
    return exportSequence(p, alone, st, progress, cancel, error);
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
