#include "Exporter.h"
#include "TrimPasses.h"
#include "media/SpatialAudio.h"
#include "render/SpatialVideo.h"
#include "core/AudioDescription.h"
#include "core/Provenance.h"
#include "media/ContentCredentials.h"

#include "core/ColorGroups.h"
#include "core/Expression.h"
#include "core/ClipAnimation.h"
#include "core/Interpretation.h"
#include "core/Cea708.h"
#include "core/Teletext.h"
#include "render/BroadcastMxf.h"
#include "core/Chapters.h"
#include "core/Surround.h"
#include "render/RateConversion.h"
#include "render/Retime.h"
#include "audio/TimeStretch.h"
#include "media/Rife.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <algorithm>
#include <functional>
#include <optional>
#include <thread>
#include <array>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <map>
#include <memory>
#include <mutex>

#include "ColorSpace.h"
#include "Compositor.h"
#include "ImageSubtitles.h"
#include "Hdr10Plus.h"
#include "AdaptiveHdr.h"
#include "HdrStill.h"
#include "media/GainMap.h"
#include "LightLevel.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "Processing.h"
#include "media/HwAccel.h"
#include "media/Loudness.h"
#include "render/DialogueLoudness.h"
#include "render/LoudnessReport.h"
#include "media/SuperScale.h"
#include "media/ThreadScheduling.h"
#include "media/Decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace montage {

namespace {

// The delivery space an export asks for (none: the sequence's), with its tone mapping when it is SDR (not for a
// sequence graded scene-referred, which goes through its output transform).
const ColorSpace* deliverySpaceAsked(const Sequence& seq, const ExportSettings& s) {
    const ColorSpace* c = findColorSpace(s.colorSpace);
    ToneMapping t = ToneMapping::Montage;
    return c && !sceneReferredWorking(seq) && parseToneMapping(s.toneMapping, t) ? &withToneMapping(*c, t) : c;
}

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
    if (c == "cfhd") return s.alpha ? AV_PIX_FMT_GBRAP12LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "v210") return AV_PIX_FMT_YUV422P10LE;
    if (c == "ffv1") return s.alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "liboapv") return s.alpha ? AV_PIX_FMT_YUVA444P12LE : s.profile == "4444" ? AV_PIX_FMT_YUV444P12LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "mjpeg") return AV_PIX_FMT_YUVJ420P;
    if (c == "libvpx-vp9" && s.alpha) return AV_PIX_FMT_YUVA420P;
    if (c == "png") return s.alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    if (c == "tiff") return AV_PIX_FMT_RGB48LE;
    if (c == "exr") return s.alpha ? AV_PIX_FMT_GBRAPF32LE : AV_PIX_FMT_GBRPF32LE;
    if (c == "dpx") return AV_PIX_FMT_GBRP10LE;
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
    // Called with each video packet before it is written (pts in the encoder's time base); false fails the export.
    std::function<bool(AVPacket*)> videoPacket;
    // Spatial video: the video "encoder" gives raw pictures (the eyes side by side), which this codes as MV-HEVC.
    std::unique_ptr<SpatialVideoEncoder> spatial;
    std::string spatialError;  // (what x265 said, when it failed)
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

// MV-HEVC coded pictures written as the video stream's packets.
int writeSpatial(Output& o, std::vector<SpatialVideoEncoder::Packet>& coded) {
    for (SpatialVideoEncoder::Packet& c : coded) {
        AVPacket* p = av_packet_alloc();
        if (!p || av_new_packet(p, int(c.data.size())) < 0) {
            av_packet_free(&p);
            return AVERROR(ENOMEM);
        }
        std::memcpy(p->data, c.data.data(), c.data.size());
        p->pts = c.pts;
        p->dts = c.dts;
        if (c.key) p->flags |= AV_PKT_FLAG_KEY;
        p->stream_index = o.vst->index;
        av_packet_rescale_ts(p, o.vctx->time_base, o.vst->time_base);
        const int rc = av_interleaved_write_frame(o.oc, p);
        av_packet_free(&p);
        if (rc < 0) return rc;
    }
    coded.clear();
    return 0;
}

// Why writing video failed: x265's own words for spatial video.
std::string videoError(const Output& o, int rc) { return o.spatialError.empty() ? averr(rc) : o.spatialError; }

int drain(Output& o, AVCodecContext* ctx, AVStream* st) {
    for (;;) {
        int rc = avcodec_receive_packet(ctx, o.pkt);
        if (rc == AVERROR_EOF && o.spatial && st == o.vst) {
            std::vector<SpatialVideoEncoder::Packet> coded;
            return o.spatial->flush(coded, &o.spatialError) ? writeSpatial(o, coded) : AVERROR_EXTERNAL;
        }
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return rc;
        if (o.spatial && st == o.vst) {
            // (The eyes side by side, raw: coded as the two views.)
            std::vector<SpatialVideoEncoder::Packet> coded;
            const bool ok = o.spatial->encode(o.pkt->data, size_t(o.pkt->size), o.pkt->pts, coded, &o.spatialError);
            av_packet_unref(o.pkt);
            if (!ok) return AVERROR_EXTERNAL;
            if ((rc = writeSpatial(o, coded)) < 0) return rc;
            continue;
        }
        if (st == o.vst && o.videoPacket && !o.videoPacket(o.pkt)) return AVERROR(EINVAL);
        av_packet_rescale_ts(o.pkt, ctx->time_base, st->time_base);
        o.pkt->stream_index = st->index;
        rc = av_interleaved_write_frame(o.oc, o.pkt);
        if (rc < 0) return rc;
    }
}

}  // namespace

namespace {

std::vector<ExportPreset> makePresets(bool everyBuild) {
    {
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
        v.push_back(preset("ProRes 422 HQ", "mov", "Intermediate / mastering, 10-bit 4:2:2", "prores_ks", "pcm_s24le", 0, "", "hq"));
        v.push_back(preset("ProRes 422", "mov", "Standard ProRes, 10-bit 4:2:2", "prores_ks", "pcm_s24le", 0, "", "standard"));
        v.push_back(preset("ProRes 422 LT", "mov", "Lighter ProRes for offline / proxies", "prores_ks", "pcm_s16le", 0, "", "lt"));
        v.push_back(preset("ProRes 422 Proxy", "mov", "The smallest ProRes, for offline editing", "prores_ks", "pcm_s16le", 0, "", "proxy"));
        {
            ExportPreset p = preset("ProRes 4444 (alpha)", "mov", "Keeps transparency for graphics", "prores_ks", "pcm_s24le", 0, "", "4444");
            p.settings.alpha = true;
            v.push_back(p);
            ExportPreset xq = preset("ProRes 4444 XQ (alpha)", "mov", "The highest-quality ProRes, with transparency", "prores_ks",
                                     "pcm_s24le", 0, "", "4444xq");
            xq.settings.alpha = true;
            v.push_back(xq);
        }
        v.push_back(preset("DNxHR HQ", "mov", "Avid-friendly 8-bit 4:2:2 intermediate", "dnxhd", "pcm_s24le", 0, "", "dnxhr_hq"));
        v.push_back(preset("DNxHR SQ", "mov", "8-bit 4:2:2, standard quality", "dnxhd", "pcm_s24le", 0, "", "dnxhr_sq"));
        v.push_back(preset("DNxHR LB", "mov", "8-bit 4:2:2, low bandwidth for offline", "dnxhd", "pcm_s16le", 0, "", "dnxhr_lb"));
        v.push_back(preset("DNxHR HQX (10-bit)", "mov", "10-bit 4:2:2 for finishing and HDR", "dnxhd", "pcm_s24le", 0, "", "dnxhr_hqx"));
        v.push_back(preset("DNxHR 444 (10-bit)", "mov", "10-bit 4:4:4 for finishing and keying", "dnxhd", "pcm_s24le", 0, "", "dnxhr_444"));
        {
            ExportPreset cf = preset("CineForm", "mov", "Wavelet intermediate, 10-bit 4:2:2 (12-bit RGB with alpha)", "cfhd", "pcm_s24le", 0, "");
            v.push_back(cf);
            cf.name = "CineForm (alpha)";
            cf.description = "CineForm 12-bit RGBA, keeping transparency";
            cf.settings.alpha = true;
            v.push_back(cf);
        }
        v.push_back(preset("FFV1 (lossless archive)", "mkv", "Mathematically lossless 10-bit 4:2:2 with FLAC sound, for archiving", "ffv1", "flac", 0,
                           "", "", "yuv422p10le"));
        if (everyBuild || avcodec_find_encoder_by_name("liboapv")) {
            // APV (Advanced Professional Video, an open intra-frame codec; OpenAPV's encoder): listed where FFmpeg has
            // the encoder. Quality is a QP from 0 (best) to 63, or a bitrate.
            v.push_back(preset("APV 422 10-bit", "mov", "Open intra-frame intermediate, 10-bit 4:2:2", "liboapv", "pcm_s24le", 20, "", "", "yuv422p10le"));
            v.push_back(preset("APV 422 10-bit (MP4)", "mp4", "APV 10-bit 4:2:2 in MP4, AAC sound", "liboapv", "aac", 20, "", "", "yuv422p10le"));
            ExportPreset a = preset("APV 4444 12-bit (alpha)", "mov", "APV 12-bit 4:4:4 with transparency, for graphics and finishing", "liboapv",
                                    "pcm_s24le", 16, "", "4444");
            a.settings.alpha = true;
            v.push_back(a);
        }
        v.push_back(preset("Uncompressed 10-bit (v210)", "mov", "Uncompressed 10-bit 4:2:2, for broadcast ingest", "v210", "pcm_s24le", 0, ""));
        {
            // Broadcast delivery in MXF OP1a: 1080 lines, 48 kHz 24-bit sound with each channel its own mono track.
            auto broadcast = [](ExportPreset p, int tracks) {
                p.settings.width = 1920;
                p.settings.height = 1080;
                p.settings.sampleRate = 48000;
                p.settings.monoAudioTracks = tracks;
                p.settings.smartRender = false;
                return p;
            };
            v.push_back(broadcast(preset("XDCAM HD422 (MXF)", "mxf", "Sony's broadcast MPEG-2: 1080 lines, 4:2:2 at 50 Mb/s, eight mono 24-bit tracks",
                                         "mpeg2video", "pcm_s24le", 0, "", "", "yuv422p"),
                                  8));
            v.push_back(broadcast(preset("AVC-Intra 100 (MXF)", "mxf", "10-bit 4:2:2 intra H.264 at 100 Mb/s, four mono 24-bit tracks (as the DPP's AS-11 asks)",
                                         "libx264", "pcm_s24le", 0, "", "avci100", "yuv422p10le"),
                                  4));
            ExportPreset dnx = broadcast(preset("DNxHR HQ (MXF)", "mxf", "DNxHR HQ in MXF OP1a, each channel a mono track (Avid's layout)", "dnxhd",
                                                "pcm_s24le", 0, "", "dnxhr_hq"),
                                         2);
            dnx.settings.width = dnx.settings.height = 0;  // DNxHR takes any size
            v.push_back(dnx);
        }
        v.push_back(preset("VP9 (WebM)", "webm", "Open web format, Opus audio", "libvpx-vp9", "libopus", 32, "good"));
        v.push_back(preset("AV1 (SVT-AV1)", "mp4", "Next-generation efficiency", "libsvtav1", "aac", 32, "8"));
        {
            // Short-form social video: the platforms' -14 LUFS loudness, kept under -1 dBTP.
            ExportPreset p = preset("Social - TikTok / Reels / Shorts", "mp4",
                                    "H.264 at the platforms' loudness (-14 LUFS); for a vertical video, Auto Reframe the sequence first",
                                    "libx264", "aac", 18, "medium");
            p.settings.loudnessTarget = -14;
            p.settings.audioBitrate = 256000;
            v.push_back(p);
            ExportPreset y = preset("Social - YouTube (-14 LUFS)", "mp4", "H.264 for YouTube, normalised to its -14 LUFS", "libx264", "aac",
                                    18, "slow");
            y.settings.loudnessTarget = -14;
            v.push_back(y);
        }
        {
            ExportPreset g = preset("Animated GIF", "gif", "Loops; 480 px wide, 15 fps, its own 256-colour palette, dithered", "gif",
                                    "none", 0, "");
            v.push_back(g);
            ExportPreset png = preset("PNG Sequence", "png", "One numbered PNG a frame, with transparency (name_000000.png...)", "png",
                                      "none", 0, "");
            png.settings.alpha = true;
            v.push_back(png);
            v.push_back(preset("TIFF Sequence (16-bit)", "tif", "One numbered 16-bit TIFF a frame, for finishing and VFX", "tiff", "none", 0,
                               ""));
            v.push_back(preset("OpenEXR Sequence (half float)", "exr",
                               "One numbered half-float OpenEXR a frame in scene-linear light, values above white kept: for compositing",
                               "exr", "none", 0, ""));
            v.push_back(preset("DPX Sequence (10-bit)", "dpx", "One numbered 10-bit DPX a frame, for film and broadcast finishing", "dpx",
                               "none", 0, ""));
        }
        v.push_back(preset("Audio - WAV 24-bit", "wav", "Uncompressed mixdown", "none", "pcm_s24le", 0, ""));
        v.push_back(preset("Audio - AAC (M4A)", "m4a", "Compressed mixdown", "none", "aac", 0, ""));
        return v;
    }
}

}  // namespace

const std::vector<ExportPreset>& exportPresets() {
    static const std::vector<ExportPreset> presets = makePresets(false);
    return presets;
}

std::vector<ExportPreset> exportPresetsOfEveryBuild() { return makePresets(true); }

const ExportPreset* findExportPreset(const std::string& name) {
    for (const auto& p : exportPresets())
        if (p.name == name) return &p;
    // The names the ProRes, DNxHR and CineForm presets had before they lost their makers' names ("Apple ProRes 422 HQ",
    // "Avid DNxHR HQ (MXF)", "GoPro CineForm"), so saved settings, scripts and MCP calls still find them.
    for (const char* maker : {"Apple ", "Avid ", "GoPro "}) {
        const size_t n = std::strlen(maker);
        if (name.size() > n && name.compare(0, n, maker) == 0) return findExportPreset(name.substr(n));
    }
    return nullptr;
}

namespace {

// FFmpeg's layout for one of core/Surround.h's, channels in the same order: 5.1.x on the back pair as FFmpeg's own
// 5.1.2 and 5.1.4, 7.1.2's overhead pair at the sides (Dolby's bed). False for a layout it has no channels for.
bool ffmpegLayout(const std::string& name, AVChannelLayout& out) {
    const uint64_t top = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT, rear = AV_CH_TOP_BACK_LEFT | AV_CH_TOP_BACK_RIGHT;
    uint64_t mask = 0;
    if (name == "stereo") mask = AV_CH_LAYOUT_STEREO;
    else if (name == "5.1") mask = AV_CH_LAYOUT_5POINT1;
    else if (name == "7.1") mask = AV_CH_LAYOUT_7POINT1;
    else if (name == "5.1.2") mask = AV_CH_LAYOUT_5POINT1_BACK | top;
    else if (name == "5.1.4") mask = AV_CH_LAYOUT_5POINT1_BACK | top | rear;
    else if (name == "7.1.2") mask = AV_CH_LAYOUT_7POINT1 | AV_CH_TOP_SIDE_LEFT | AV_CH_TOP_SIDE_RIGHT;
    else if (name == "7.1.4") mask = AV_CH_LAYOUT_7POINT1 | top | rear;
    else if (layoutChannels(name) == 2) mask = AV_CH_LAYOUT_STEREO;  // what core/Surround.h makes of any other name
    AVChannelLayout l{};
    if (!mask || av_channel_layout_from_mask(&l, mask) < 0 || l.nb_channels != layoutChannels(name)) return false;
    out = l;
    return true;
}

// ---- Animated GIF -----------------------------------------------------------------

// Median cut over a 5-bit-a-channel histogram: up to `colours` entries, each its box's weighted mean.
std::vector<std::array<uint8_t, 3>> medianCut(const std::vector<uint32_t>& hist, int colours) {
    struct Bin {
        uint8_t c[3];
        uint32_t n;
    };
    std::vector<Bin> bins;
    for (uint32_t i = 0; i < hist.size(); ++i)
        if (hist[i]) bins.push_back({{uint8_t(i >> 10), uint8_t((i >> 5) & 31), uint8_t(i & 31)}, hist[i]});
    std::vector<std::pair<size_t, size_t>> boxes{{0, bins.size()}};  // ranges of `bins`
    while (int(boxes.size()) < colours) {
        // The box with the widest spread (weighted by how much it holds) is split at its median.
        double best = -1;
        size_t which = 0;
        int axis = 0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            const auto [lo, hi] = boxes[b];
            if (hi - lo < 2) continue;
            int mn[3] = {31, 31, 31}, mx[3] = {0, 0, 0};
            double count = 0;
            for (size_t i = lo; i < hi; ++i) {
                for (int k = 0; k < 3; ++k) mn[k] = std::min<int>(mn[k], bins[i].c[k]), mx[k] = std::max<int>(mx[k], bins[i].c[k]);
                count += bins[i].n;
            }
            for (int k = 0; k < 3; ++k) {
                const double score = (mx[k] - mn[k]) * std::sqrt(count);
                if (score > best) best = score, which = b, axis = k;
            }
        }
        if (best <= 0) break;
        auto [lo, hi] = boxes[which];
        std::sort(bins.begin() + long(lo), bins.begin() + long(hi), [axis](const Bin& a, const Bin& b) { return a.c[axis] < b.c[axis]; });
        double total = 0, run = 0;
        for (size_t i = lo; i < hi; ++i) total += bins[i].n;
        size_t cut = lo + 1;
        for (size_t i = lo; i < hi - 1; ++i) {
            run += bins[i].n;
            cut = i + 1;
            if (run >= total / 2) break;
        }
        boxes[which] = {lo, cut};
        boxes.push_back({cut, hi});
    }
    std::vector<std::array<uint8_t, 3>> palette;
    for (const auto& [lo, hi] : boxes) {
        double acc[3] = {0, 0, 0}, n = 0;
        for (size_t i = lo; i < hi; ++i)
            for (int k = 0; k < 3; ++k) acc[k] += (bins[i].c[k] * 8 + 4.0) * bins[i].n;
        for (size_t i = lo; i < hi; ++i) n += bins[i].n;
        if (n > 0) palette.push_back({uint8_t(std::lround(acc[0] / n)), uint8_t(std::lround(acc[1] / n)), uint8_t(std::lround(acc[2] / n))});
    }
    if (palette.empty()) palette.push_back({0, 0, 0});
    return palette;
}

// The frame as 8-bit RGB in the display space, at W x H.
std::vector<uint8_t> gifFrame(const Project& p, const Sequence& seq, FrameTime f, int W, int H, const ColorSpace& outSpace) {
    RenderOptions ro;
    ro.scale = double(W) / seq.width;
    ro.highQuality = true;
    // (graded scene-referred: through the output transform for the GIF's own space)
    const bool scene = sceneReferredWorking(seq);
    if (scene) ro.displaySpace = outSpace.id;
    Image img = renderProgramFrame(p, seq, f, ro);
    if (img.width != W || img.height != H) img = resizeImage(img, W, H);
    if (!scene) convertColor(img, sequenceColorSpace(seq), outSpace, std::clamp(seq.hdrPeakNits, 100.0, 10000.0));
    std::vector<uint8_t> rgb(size_t(W) * size_t(H) * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* q = img.at(x, y);
            for (int k = 0; k < 3; ++k) rgb[(size_t(y) * W + x) * 3 + k] = uint8_t(std::lround(std::clamp(q[k], 0.0f, 1.0f) * 255));
        }
    return rgb;
}

bool exportGif(const Project& p, const Sequence& seq, const ExportSettings& s, FrameTime in, FrameTime out, const ExportProgress& progress,
               const std::atomic<bool>* cancel, std::string* error, bool& opened) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const int W = std::max(2, s.width > 0 ? s.width : std::min(seq.width, 480));
    const int H = std::max(2, s.height > 0 ? s.height : int(std::lround(double(W) * seq.height / seq.width)));
    const double fps = s.fps > 0 ? s.fps : std::min(15.0, seq.fpsValue());
    const double step = seq.fpsValue() / fps;  // sequence frames per GIF frame
    const int64_t frames = std::max<int64_t>(1, int64_t(std::floor(double(out - in) / step + 1e-9)));
    const ColorSpace* chosen = deliverySpaceAsked(seq, s);
    const ColorSpace& seqSpace = sequenceColorSpace(seq);
    const ColorSpace& outSpace = chosen && !chosen->sceneReferred && !chosen->hdr() ? *chosen
                                 : !seqSpace.sceneReferred && !seqSpace.hdr()      ? seqSpace
                                                                                    : *findColorSpace("rec709");
    auto frameAt = [&](int64_t k) { return in + FrameTime(std::floor(double(k) * step + 1e-9)); };
    // Pass 1: the palette, from up to 32 frames spread over the range.
    std::vector<uint32_t> hist(1 << 15, 0);
    const int64_t samples = std::min<int64_t>(32, frames);
    for (int64_t i = 0; i < samples; ++i) {
        if (cancel && cancel->load()) return fail("Export cancelled");
        const std::vector<uint8_t> rgb = gifFrame(p, seq, frameAt(i * frames / samples), W, H, outSpace);
        for (size_t j = 0; j < rgb.size(); j += 3) ++hist[size_t(rgb[j] >> 3) << 10 | size_t(rgb[j + 1] >> 3) << 5 | size_t(rgb[j + 2] >> 3)];
        if (progress) progress(0.1 * double(i + 1) / double(samples), in);
    }
    const auto palette = medianCut(hist, 256);
    // Every 5-bit colour's nearest palette entry.
    std::vector<uint8_t> nearest(1 << 15);
    for (int i = 0; i < (1 << 15); ++i) {
        const int r = (i >> 10) * 8 + 4, g = ((i >> 5) & 31) * 8 + 4, b = (i & 31) * 8 + 4;
        int best = 0, bestD = 1 << 30;
        for (size_t k = 0; k < palette.size(); ++k) {
            const int dr = r - palette[k][0], dg = g - palette[k][1], db = b - palette[k][2];
            const int d = 2 * dr * dr + 4 * dg * dg + db * db;
            if (d < bestD) bestD = d, best = int(k);
        }
        nearest[size_t(i)] = uint8_t(best);
    }
    Output o;
    int rc = avformat_alloc_output_context2(&o.oc, nullptr, "gif", s.path.c_str());
    if (rc < 0 || !o.oc) return fail("Cannot write a GIF to " + s.path);
    o.pkt = av_packet_alloc();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_GIF);
    if (!codec) return fail("This build of FFmpeg has no GIF encoder");
    o.vst = avformat_new_stream(o.oc, nullptr);
    o.vctx = avcodec_alloc_context3(codec);
    o.vctx->width = W;
    o.vctx->height = H;
    o.vctx->time_base = av_inv_q(av_d2q(fps, 100000));
    o.vctx->framerate = av_d2q(fps, 100000);
    o.vctx->pix_fmt = AV_PIX_FMT_PAL8;
    if ((rc = avcodec_open2(o.vctx, codec, nullptr)) < 0) return fail("Cannot open the GIF encoder: " + averr(rc));
    avcodec_parameters_from_context(o.vst->codecpar, o.vctx);
    o.vst->time_base = o.vctx->time_base;
    if ((rc = avio_open(&o.oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0) return fail("Cannot write " + s.path + ": " + averr(rc));
    opened = true;
    if ((rc = avformat_write_header(o.oc, nullptr)) < 0) return fail("Cannot write header: " + averr(rc));
    o.headerWritten = true;
    o.vframe = av_frame_alloc();
    o.vframe->format = AV_PIX_FMT_PAL8;
    o.vframe->width = W;
    o.vframe->height = H;
    if (av_frame_get_buffer(o.vframe, 0) < 0) return fail("Out of memory");
    // Ordered (Bayer) dithering: the pattern stays put from frame to frame, so it does not crawl.
    static const uint8_t bayer[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
                                        {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                        {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
    for (int64_t k = 0; k < frames; ++k) {
        if (cancel && cancel->load()) return fail("Export cancelled");
        const std::vector<uint8_t> rgb = gifFrame(p, seq, frameAt(k), W, H, outSpace);
        if (av_frame_make_writable(o.vframe) < 0) return fail("Out of memory");
        auto* pal = reinterpret_cast<uint32_t*>(o.vframe->data[1]);
        for (int i = 0; i < 256; ++i) {
            const auto& c = palette[size_t(std::min<int>(i, int(palette.size()) - 1))];
            pal[i] = 0xff000000u | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
        }
        for (int y = 0; y < H; ++y) {
            uint8_t* row = o.vframe->data[0] + size_t(y) * size_t(o.vframe->linesize[0]);
            for (int x = 0; x < W; ++x) {
                const int d = (bayer[y & 7][x & 7] - 32) / 4;  // -8..7: about a 5-bit step
                const uint8_t* q = &rgb[(size_t(y) * W + x) * 3];
                auto ch = [&](int v) { return std::clamp(v + d, 0, 255) >> 3; };
                row[x] = nearest[size_t(ch(q[0])) << 10 | size_t(ch(q[1])) << 5 | size_t(ch(q[2]))];
            }
        }
        o.vframe->pts = k;
        if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("GIF encoding failed: " + averr(rc));
        if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing the GIF failed: " + averr(rc));
        if (progress) progress(0.1 + 0.9 * double(k + 1) / double(frames), frameAt(k));
    }
    if ((rc = avcodec_send_frame(o.vctx, nullptr)) < 0 || (rc = drain(o, o.vctx, o.vst)) < 0) return fail("Finishing the GIF failed: " + averr(rc));
    if ((rc = av_write_trailer(o.oc)) < 0) return fail("Cannot finalise file: " + averr(rc));
    return true;
}

// Chapter markers in [in, out) as the container's chapters (MP4 and MOV write both QuickTime and Nero chapters).
void addChapters(AVFormatContext* oc, const Sequence& seq, FrameTime in, FrameTime out) {
    const std::string fmt = oc->oformat->name;
    if (fmt.find("mov") == std::string::npos && fmt.find("mp4") == std::string::npos && fmt.find("matroska") == std::string::npos &&
        fmt.find("webm") == std::string::npos)
        return;
    const std::vector<Chapter> chapters = chaptersOf(seq, in, out);
    if (chapters.empty()) return;
    oc->chapters = static_cast<AVChapter**>(av_calloc(chapters.size(), sizeof(AVChapter*)));
    if (!oc->chapters) return;
    const AVRational tb{int(seq.fps.den), int(seq.fps.num)};
    for (const Chapter& c : chapters) {
        auto* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
        if (!ch) break;
        ch->id = int64_t(oc->nb_chapters) + 1;
        ch->time_base = tb;
        ch->start = c.start;
        ch->end = c.end;
        av_dict_set(&ch->metadata, "title", c.title.c_str(), 0);
        oc->chapters[oc->nb_chapters++] = ch;
    }
}

// ---- Smart rendering --------------------------------------------------------------------------------------------
// Frames that are one untouched clip of footage already in the export's intra-frame codec (ProRes, DNxHR) are
// copied from the source file instead of being decoded and encoded again, as Premiere's and Final Cut's smart
// rendering do: faster, and exactly the original pictures.

bool isIntraCodec(const std::string& encoder) { return encoder == "prores_ks" || encoder == "dnxhd"; }

// The clip's transform leaves the picture as it is at clip frame `local` (the source fills the frame 1:1).
bool identityMotion(const Clip& c, FrameTime local) {
    if (hasClipAnimation(c)) return false;  // animated on top of its transform
    if (usesExpressions(c.motion)) return false;
    const EffectInfo* info = findEffectInfo(c.motion.empty() ? "transform" : c.motion.type);
    if (!info) return false;
    for (const ParamInfo& pi : info->params) {
        if (pi.name == "fit") continue;  // any fit is 1:1 when the sizes match (checked by the caller)
        if (pi.kind == ParamKind::Color) {
            if (std::fabs(c.motion.p(pi.name + ".r", local, pi.def) - pi.def) > 1e-9 ||
                std::fabs(c.motion.p(pi.name + ".g", local, pi.defG) - pi.defG) > 1e-9 ||
                std::fabs(c.motion.p(pi.name + ".b", local, pi.defB) - pi.defB) > 1e-9)
                return false;
            continue;
        }
        if (std::fabs(c.motion.p(pi.name, local, pi.def) - pi.def) > 1e-9) return false;
    }
    return true;
}

class SmartRenderer {
public:
    // `profile` is the export's ProRes or DNxHR profile option ("hq", "4444", "dnxhr_sq"...).
    SmartRenderer(const Project& p, const Sequence& seq, const ColorSpace& seqSpace, AVCodecContext* vctx, AVStream* vst, bool alpha,
                  const std::string& profile)
        : p_(p), seq_(seq), seqSpace_(seqSpace), vctx_(vctx), vst_(vst), alpha_(alpha) {
        // The FourCC a ProRes flavour is stored under, and DNxHR's profile numbers.
        static const std::map<std::string, uint32_t> prores{{"proxy", MKTAG('a', 'p', 'c', 'o')}, {"lt", MKTAG('a', 'p', 'c', 's')},
                                                            {"standard", MKTAG('a', 'p', 'c', 'n')}, {"hq", MKTAG('a', 'p', 'c', 'h')},
                                                            {"4444", MKTAG('a', 'p', '4', 'h')}, {"4444xq", MKTAG('a', 'p', '4', 'x')}};
        static const std::map<std::string, int> dnxhr{{"dnxhr_lb", 1}, {"dnxhr_sq", 2}, {"dnxhr_hq", 3}, {"dnxhr_hqx", 4}, {"dnxhr_444", 5}};
        if (vctx->codec_id == AV_CODEC_ID_PRORES) {
            auto it = prores.find(profile.empty() ? "hq" : profile);
            tag_ = it != prores.end() ? it->second : 0;
        } else {
            auto it = dnxhr.find(profile.empty() ? "dnxhr_hq" : profile);
            profile_ = it != dnxhr.end() ? it->second : -1;
        }
    }
    ~SmartRenderer() {
        for (auto& [id, src] : sources_)
            if (src.fmt) avformat_close_input(&src.fmt);
        av_packet_free(&pkt_);
    }
    int copied() const { return copied_; }

    // Copies frame f's picture from its source into the output if it can; true if it did.
    bool copy(FrameTime f, FrameTime in, AVFormatContext* oc) {
        const Clip* c = onlyClip(f);
        if (!c) return false;
        Source* src = source(*c);
        if (!src) return false;
        const double srcFrame = c->sourceFrameAt(f);  // the media's frame, in sequence frames (same rate)
        const int64_t index = std::llround(srcFrame);
        if (std::fabs(srcFrame - double(index)) > 1e-6 || index < 0) return false;
        if (!readFrame(*src, index)) return false;
        pkt_->stream_index = vst_->index;
        pkt_->pts = pkt_->dts = av_rescale_q(f - in, vctx_->time_base, vst_->time_base);
        pkt_->duration = av_rescale_q(1, vctx_->time_base, vst_->time_base);
        pkt_->flags |= AV_PKT_FLAG_KEY;
        pkt_->pos = -1;
        if (av_interleaved_write_frame(oc, pkt_) < 0) return false;
        ++copied_;
        return true;
    }

private:
    struct Source {
        AVFormatContext* fmt = nullptr;
        int stream = -1;
        AVRational tb{1, 1};
        int64_t start = 0;
        int64_t ticksPerFrame = 1;  // in tb
        int64_t next = -1;          // the frame index a plain read returns next
        bool usable = false;
    };

    // The one visible clip at f when nothing else in the program touches it.
    const Clip* onlyClip(FrameTime f) const {
        const Clip* found = nullptr;
        for (size_t i = 0; i < seq_.videoTracks.size(); ++i) {
            const Track& t = seq_.videoTracks[i];
            if (t.muted) continue;
            for (const Clip& c : t.clips) {
                if (!c.contains(f) || !c.enabled) continue;
                if (found) return nullptr;
                found = &c;
            }
            for (const Transition& tr : t.transitions) {
                FrameTime a, b;
                if (edit::transitionRange(t, tr, a, b) && f >= a && f < b) return nullptr;
            }
        }
        bool grouped = false;  // (a colour group's grades run on it)
        if (found)
            for (const ColorGroup* g : colorGroupsOf(seq_, *found)) grouped = grouped || !g->pre.empty() || !g->post.empty();
        if (!found || found->isGenerator() || !found->effects.empty() || !found->grade.empty() || grouped ||
            found->blendMode != "normal" || found->speed != 1.0 ||
            found->reverse || found->ramped() || !identityMotion(*found, f - found->start))
            return nullptr;
        return found;
    }

    Source* source(const Clip& c) {
        auto it = sources_.find(c.mediaId);
        if (it != sources_.end()) return it->second.usable ? &it->second : nullptr;
        Source& src = sources_[c.mediaId];
        const MediaItem* m = p_.findMedia(c.mediaId);
        if (!m || m->kind != MediaKind::Video || m->path.empty()) return nullptr;
        if (mediaColorSpace(*m).id != seqSpace_.id) return nullptr;  // would be converted
        if (!interpretationOf(*m).empty()) return nullptr;            // read differently from its packets
        if (!m->stereo.empty()) return nullptr;                       // one eye of each picture is shown
        if (openMediaInput(&src.fmt, m->path) < 0) return nullptr;
        if (avformat_find_stream_info(src.fmt, nullptr) < 0) return nullptr;
        src.stream = av_find_best_stream(src.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (src.stream < 0) return nullptr;
        const AVStream* st = src.fmt->streams[src.stream];
        const AVCodecParameters* par = st->codecpar;
        const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(par->format));
        // The same pictures the encoder would make: codec, size, sampling, rate, progressive, the same flavour.
        const bool same = par->codec_id == vctx_->codec_id && par->width == vctx_->width && par->height == vctx_->height &&
                          par->format == vctx_->pix_fmt && av_cmp_q(rate, vctx_->framerate) == 0 &&
                          (par->field_order == AV_FIELD_PROGRESSIVE || par->field_order == AV_FIELD_UNKNOWN) &&
                          (alpha_ || !(d && (d->flags & AV_PIX_FMT_FLAG_ALPHA))) && sameFlavour(*par);
        if (!same || seq_.width != par->width || seq_.height != par->height) return nullptr;
        src.tb = st->time_base;
        src.start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        src.ticksPerFrame = av_rescale_q(1, av_inv_q(rate), src.tb);
        if (src.ticksPerFrame <= 0) return nullptr;
        if (!pkt_) pkt_ = av_packet_alloc();
        src.usable = true;
        return &src;
    }

    // ProRes and DNxHR come in flavours (Proxy, LT, 422, HQ, 4444; SQ, HQ, HQX, 444): only the same one may mix.
    bool sameFlavour(const AVCodecParameters& par) const {
        if (vctx_->codec_id == AV_CODEC_ID_PRORES) return tag_ && par.codec_tag == tag_;
        return profile_ >= 0 && par.profile == profile_;
    }

    bool readFrame(Source& src, int64_t index) {
        const int64_t want = src.start + index * src.ticksPerFrame;
        if (src.next != index) {
            if (av_seek_frame(src.fmt, src.stream, want, AVSEEK_FLAG_BACKWARD) < 0) return false;
            src.next = -1;
        }
        for (int guard = 0; guard < 10000; ++guard) {
            av_packet_unref(pkt_);
            if (av_read_frame(src.fmt, pkt_) < 0) return false;
            if (pkt_->stream_index != src.stream) continue;
            const int64_t ts = pkt_->pts != AV_NOPTS_VALUE ? pkt_->pts : pkt_->dts;
            if (ts == AV_NOPTS_VALUE) return false;
            if (ts + src.ticksPerFrame / 2 < want) continue;  // before the frame (after a backward seek)
            if (ts - src.ticksPerFrame / 2 > want) return false;  // a gap in the source
            src.next = index + 1;
            return true;
        }
        return false;
    }

    const Project& p_;
    const Sequence& seq_;
    const ColorSpace& seqSpace_;
    AVCodecContext* vctx_;
    AVStream* vst_;
    bool alpha_;
    std::map<Id, Source> sources_;
    AVPacket* pkt_ = nullptr;
    int copied_ = 0;
    uint32_t tag_ = 0;
    int profile_ = -1;
};

bool exportImpl(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                const std::atomic<bool>* cancel, std::string* error, bool& opened, std::string* encoderUsed, int* smartRendered,
                LightLevels* light) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    FrameTime in = s.in >= 0 ? s.in : 0;
    FrameTime out = s.out >= 0 ? s.out : seq.duration();
    if (seq.duration() > 0) out = std::min(out, seq.duration());  // nothing past the end of the cut
    if (out <= in) return fail("Nothing to export: the range is empty");
    const bool wantVideo = s.videoCodec != "none" && !s.videoCodec.empty();
    const bool wantAudio = s.audioCodec != "none" && !s.audioCodec.empty();
    if (!wantVideo && !wantAudio) return fail("No video or audio codec selected");

    // A trim pass: the target's pictures, in its space.
    const TrimTarget* trim = s.trimTarget.empty() ? nullptr : findTrimTarget(seq, s.trimTarget);
    if (!s.trimTarget.empty() && !trim) return fail("The sequence has no trim target " + s.trimTarget);
    if (trim && wantVideo && (s.videoCodec == "gif" || s.videoCodec == "exr"))
        return fail("A trim pass is delivered as video or PNG, TIFF or DPX frames, not " + s.videoCodec);
    // Delivered at another frame rate: the programme (seq, frames [in, out)) rendered and mixed as it is, written as the
    // delivered sequence (dseq, frames [din, dout)): its rate, captions and markers.
    RateConversion conv;
    if (std::string why; !planRateConversion(seq, s, conv, &why)) return fail(why);
    const Sequence deliveredSeq = conv.active() ? deliveredSequence(seq, conv, in, out) : Sequence{};
    const Sequence& dseq = conv.active() ? deliveredSeq : seq;
    const FrameTime din = conv.active() ? 0 : in, dout = conv.active() ? deliveredFrames(conv, out - in) : out;
    // Coded as fields: asked for, or pulldown into them.
    const bool interlacedOut = s.interlaced || conv.fields;
    if (s.videoCodec == "gif") return exportGif(p, seq, s, in, out, progress, cancel, error, opened);
    // Image sequences: numbered files, no sound.
    const bool imageSequence = s.videoCodec == "png" || s.videoCodec == "tiff" || s.videoCodec == "exr" || s.videoCodec == "dpx";
    const bool floatFrames = s.videoCodec == "exr";  // written straight from the float picture, not through 16 bits
    // (by the OpenEXR library for another compression or more layers)
    const bool exrLibrary = floatFrames && (!s.exrCompression.empty() || s.exrLayers);
    if ((!s.exrCompression.empty() || s.exrLayers) && wantVideo) {
        if (!floatFrames) return fail("OpenEXR compression and layers are for OpenEXR frames");
        if (!exrAvailable()) return fail("This build of Montage writes OpenEXR only through FFmpeg (ZIP compression, one layer)");
        const auto names = exrCompressions();
        if (!s.exrCompression.empty() && std::find(names.begin(), names.end(), s.exrCompression) == names.end())
            return fail("Unknown OpenEXR compression " + s.exrCompression);
    }
    std::string outPath = s.path;
    if (imageSequence) {
        if (wantAudio) return fail("An image sequence has no sound: choose no audio codec");
        if (outPath.find('%') == std::string::npos) {
            const size_t slash = outPath.find_last_of("/\\"), dot = outPath.find_last_of('.');
            const size_t at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : outPath.size();
            outPath.insert(at, "_%06d");
        }
    }
    Output o;
    int rc = avformat_alloc_output_context2(&o.oc, nullptr, imageSequence ? "image2" : s.format.empty() ? nullptr : s.format.c_str(),
                                            outPath.c_str());
    if (rc < 0 || !o.oc) return fail("Unknown output format for " + s.path);
    if (imageSequence) av_opt_set_int(o.oc->priv_data, "start_number", s.startNumber, 0);
    o.pkt = av_packet_alloc();
    const AVRational fps{dseq.fps.num, dseq.fps.den};
    // A stereoscopic sequence: its eyes packed as asked (two eyes across or down make the frame twice the size).
    StereoView stereoView = StereoView::Left;
    // Spatial video renders the eyes side by side, each then coded as a view of its own.
    const bool spatial = seq.stereo3d && s.stereo == "mv" && wantVideo;
    if (seq.stereo3d && !stereoViewFromName(s.stereo.empty() || s.stereo == "mv" ? "sbs" : s.stereo, stereoView))
        return fail("Unknown stereo output " + s.stereo + " (sbs, sbs_half, tb, tb_half, left, right, anaglyph or mv)");
    if (stereoView == StereoView::Difference) return fail("The eyes' difference is for checking, not delivering");
    int across = 1, down = 1;
    stereoPacking(stereoView, across, down);
    // A chosen size is each eye's.
    int W = (s.width > 0 ? s.width : seq.width) * across;
    int H = (s.height > 0 ? s.height : seq.height) * down;
    W += W & 1;  // most codecs need even dimensions
    H += H & 1;
    if (across == 2) W += 2 * ((W / 2) & 1);  // each eye even too
    if (down == 2) H += 2 * ((H / 2) & 1);
    const int sr = s.sampleRate > 0 ? s.sampleRate : seq.sampleRate;

    // (A trim pass's pictures are in its target's space and delivered so.)
    const ColorSpace& seqSpace = trim ? trimTargetSpace(seq, trim->id) : sequenceColorSpace(seq);
    const ColorSpace* chosen = trim ? &seqSpace : deliverySpaceAsked(seq, s);
    // OpenEXR holds scene-linear light: Linear Rec.709 unless a linear (or any scene-referred) space is asked for.
    // DPX can hold camera log too (a VFX pull of log footage stays log).
    const bool logOk = s.videoCodec == "dpx";
    const ColorSpace& outSpace = floatFrames ? (chosen ? *chosen : *findColorSpace("linear-rec709"))
                                 : chosen && (logOk || !chosen->sceneReferred) ? *chosen
                                                                               : seqSpace;
    const bool pq = outSpace.transfer == Transfer::Pq;
    const double peakNits = std::clamp(trim ? trimTargetPeak(seq, trim->id) : seq.hdrPeakNits, 100.0, 10000.0);
    // Light levels stated up front: the sequence's analysed ones (the master's), else the mastering peak (and an average
    // under it).
    const bool analysed = seq.hdrMaxCll > 0 && !trim;
    const int maxCll = analysed ? int(std::clamp(std::lround(seq.hdrMaxCll), 1L, 10000L)) : int(std::lround(peakNits));
    const int maxFall = analysed ? int(std::clamp(std::lround(seq.hdrMaxFall), 1L, long(maxCll))) : int(std::lround(std::min(peakNits, 400.0)));
    // What is rendered is measured as it goes (HDR only): the file gets those where its format allows.
    LightLevels measured;
    const LightMeter meter(outSpace);
    const bool measure = wantVideo && outSpace.hdr() && meter.valid();
    // HDR10+ (measured once everything is open, just before the frames, so a missing encoder fails first).
    const bool wantHdr10 = s.hdr10Plus && wantVideo && pq && !floatFrames;
    std::vector<Hdr10PlusScene> hdr10;
    // Adaptive HDR metadata (ST 2094-50) likewise, for PQ and HLG.
    const bool wantAdaptive = s.adaptiveHdr && wantVideo && outSpace.hdr() && !floatFrames;
    std::vector<AdaptiveHdrScene> adaptive;
    double renderFrom = 0;  // where the render's share of the progress starts
    if (wantVideo) {
        std::string c = s.videoCodec;
        const bool hardware = c == "hw_h264" || c == "hw_hevc";
        if (hardware) {
            const std::string family = c.substr(3);
            c = pickHwEncoder(family, W, H, fps.num, fps.den);
            if (c.empty()) c = family == "hevc" ? "libx265" : "libx264";  // no hardware encoder here
        }
        if (spatial) {
            // MV-HEVC: FFmpeg's raw "encoder" hands over each picture with both eyes, coded by x265's multiview
            // encoder (render/SpatialVideo.h).
            if (s.videoCodec != "libx265" && s.videoCodec != "hw_hevc") return fail("Spatial video is MV-HEVC: choose H.265 (HEVC)");
            const std::string fmt = o.oc->oformat->name;
            if (fmt.find("mov") == std::string::npos && fmt.find("mp4") == std::string::npos) return fail("Spatial video goes in MOV or MP4");
            if (outSpace.hdr()) return fail("Spatial video is written in 8-bit SDR: export in Rec.709 or Display P3");
            if (s.alpha || interlacedOut || s.cea608) return fail("Spatial video has no alpha, fields or CEA-608 captions");
            c = "rawvideo";
        }
        if (encoderUsed) *encoderUsed = spatial ? std::string("x265 (MV-HEVC)") : c;
        // FFmpeg's CineForm encoder reads memory it never wrote below a picture whose height is not a multiple of 8
        // (Valgrind shows it at 320 x 180, not at 184 or 1080), so the same export came out different each time.
        if (c == "cfhd" && H % 8 != 0)
            return fail("CineForm needs a height divisible by 8 (1080, 720, 2160...): set the export size to one");
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
        o.vctx->gop_size = s.gop > 0 ? s.gop : std::max(1, int(std::lround(dseq.fpsValue() * 2)));
        if (s.gop == 1) o.vctx->max_b_frames = 0;
        const ColorTags tags = colorTags(outSpace);
        o.vctx->color_primaries = tags.primaries;
        o.vctx->color_trc = tags.trc;
        o.vctx->colorspace = tags.matrix;
        o.vctx->color_range = c == "mjpeg" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        // (Raw pictures for spatial video are only copied: frames in flight on many threads would just hold memory.)
        o.vctx->thread_count = spatial ? 1 : 0;
        // Smart rendering interleaves copied packets with encoded ones, so the encoder must not hold frames back.
        if (s.smartRender && isIntraCodec(c)) o.vctx->thread_type = FF_THREAD_SLICE;
        if (s.videoBitrate > 0) o.vctx->bit_rate = s.videoBitrate;
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) o.vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (interlacedOut) {
            // Coded as two fields, the top first: a progressive picture's two halves (PsF), as 1080i deliveries ask.
            // (FFmpeg codes DNxHR, the only DNx Montage writes, progressive only; FFV1 keeps the woven frame losslessly,
            // as archives keep interlaced tape, its field order in the file.)
            if (c != "mpeg2video" && c != "libx264" && c != "prores_ks" && c != "ffv1")
                return fail("Interlaced output is for MPEG-2, H.264 (AVC-Intra), ProRes and FFV1, not " + c);
            o.vctx->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
            o.vctx->field_order = AV_FIELD_TT;
        }
        AVDictionary* opts = nullptr;
        const bool hwEncoder = hasSuffix(c, "_videotoolbox") || hasSuffix(c, "_nvenc") || hasSuffix(c, "_qsv") ||
                               hasSuffix(c, "_amf") || hasSuffix(c, "_mf");
        if (hwEncoder) {
            // Hardware encoders are rate controlled: aim for a bitrate by pixel rate.
            if (s.videoBitrate <= 0) {
                const double bitsPerPixel = c.rfind("hevc", 0) == 0 ? 0.10 : 0.16;
                o.vctx->bit_rate = int64_t(std::clamp(double(W) * H * dseq.fpsValue() * bitsPerPixel, 2e6, 150e6));
            }
            if (hasSuffix(c, "_videotoolbox")) av_dict_set(&opts, "allow_sw", "1", 0);
            if (hasSuffix(c, "_nvenc")) {
                av_dict_set(&opts, "preset", "p5", 0);
                av_dict_set(&opts, "rc", "vbr", 0);
            }
            if (hasSuffix(c, "_amf")) av_dict_set(&opts, "quality", "quality", 0);
            if (hasSuffix(c, "_mf")) av_dict_set(&opts, "hw_encoding", "1", 0);  // not Microsoft's software MFT
            if (c.rfind("hevc", 0) == 0) o.vctx->codec_tag = MKTAG('h', 'v', 'c', '1');
        } else if (c == "libx264" && s.profile == "avci100") {
            // AVC-Intra 100: x264 sets every encoding parameter the class requires (1080 or 720 lines, 10-bit 4:2:2).
            av_dict_set(&opts, "avcintra-class", "100", 0);
            o.vctx->gop_size = 1;
        } else if (c == "mpeg2video") {
            // XDCAM HD422: 4:2:2 MPEG-2 at a constant 50 Mb/s, a long GOP of 12 (15 at 30 and 60 frames a second), two
            // B-frames, 10-bit DC precision, as Sony's discs and broadcasters' ingest expect.
            const int64_t rate = s.videoBitrate > 0 ? s.videoBitrate : 50000000;
            o.vctx->bit_rate = o.vctx->rc_min_rate = o.vctx->rc_max_rate = rate;
            o.vctx->rc_buffer_size = 17825792;
            o.vctx->rc_initial_buffer_occupancy = 17825792;
            o.vctx->gop_size = s.gop > 0 ? s.gop : (std::lround(dseq.fpsValue()) % 25 == 0 ? 12 : 15);
            o.vctx->max_b_frames = 2;
            o.vctx->intra_dc_precision = 2;
            o.vctx->qmin = 1;
            o.vctx->qmax = 28;  // the non-linear scale's largest code (a quantiser of 112)
            av_dict_set(&opts, "intra_vlc", "1", 0);
            av_dict_set(&opts, "non_linear_quant", "1", 0);
        } else if (c == "libx264" || c == "libx265") {
            if (s.cea608) av_dict_set(&opts, "a53cc", "1", 0);
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
                                  static_cast<long long>(std::llround(peakNits * 10000)), maxCll, maxFall);
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
        } else if (c == "liboapv") {
            // A QP (s.crf, 0-63) unless a bitrate is set; OpenAPV's threads, one a core up to its 32.
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(o.vctx->pix_fmt);
            if (!d || d->log2_chroma_h) {
                av_dict_free(&opts);
                return fail("APV takes 4:2:2 or 4:4:4 pictures");
            }
            if (s.videoBitrate <= 0) av_dict_set_int(&opts, "qp", std::clamp(s.crf, 0, 63), 0);
            if (!s.preset.empty()) av_dict_set(&opts, "preset", s.preset.c_str(), 0);
            // The profile from the pictures: FFmpeg 8.0's wrapper leaves OpenAPV at 4:2:2 10-bit unless told (later
            // ones choose it themselves, the same).
            const std::string profile = std::string(d->nb_components == 1 ? "400" : d->log2_chroma_w ? "422" : (d->flags & AV_PIX_FMT_FLAG_ALPHA) ? "4444" : "444") +
                                        (d->comp[0].depth > 10 ? "-12" : "-10");
            av_dict_set(&opts, "oapv-params", ("profile=" + profile).c_str(), 0);
            o.vctx->thread_count = std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
            o.vctx->gop_size = 1;
            // APV's chroma sits with the first luma sample (FFmpeg's decoder says so of every APV picture), so it is
            // made there (see the conversion below).
            o.vctx->chroma_sample_location = AVCHROMA_LOC_TOPLEFT;
        } else if (c == "ffv1") {
            // The archival settings (as the Library of Congress recommends): version 3, intra-only, per-slice CRCs.
            av_dict_set(&opts, "level", "3", 0);
            av_dict_set(&opts, "slicecrc", "1", 0);
            o.vctx->gop_size = 1;
        } else if (c == "exr") {
            av_dict_set(&opts, "format", "half", 0);  // 16-bit float, as VFX houses take plates
            av_dict_set(&opts, "compression", "zip16", 0);
        } else if (c == "mjpeg") {
            o.vctx->flags |= AV_CODEC_FLAG_QSCALE;
            o.vctx->global_quality = FF_QP2LAMBDA * 3;
        }
        if (spatial) o.vctx->pix_fmt = AV_PIX_FMT_YUV420P;  // (x265's multiview encoder is 8-bit 4:2:0)
        // 4:2:2 chroma co-sited with the first luma sample of each pair, as Rec. 601 and 709 have it (and as the
        // decoder reads untagged 4:2:2); made there by the conversion below.
        if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(o.vctx->pix_fmt);
            d && !(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3 && d->log2_chroma_w == 1 && d->log2_chroma_h == 0 &&
            o.vctx->chroma_sample_location == AVCHROMA_LOC_UNSPECIFIED)
            o.vctx->chroma_sample_location = AVCHROMA_LOC_LEFT;
        {
            const KeepThreadScheduling keep;  // (SVT-AV1 would leave this thread real-time, and the workers it starts later)
            rc = avcodec_open2(o.vctx, codec, &opts);
        }
        av_dict_free(&opts);
        if (rc < 0) return fail("Cannot open video encoder: " + averr(rc));
        avcodec_parameters_from_context(o.vst->codecpar, o.vctx);
        o.vst->time_base = o.vctx->time_base;
        o.vst->avg_frame_rate = fps;
        if (spatial) {
            SpatialVideoSettings sv;
            sv.width = W / 2;
            sv.height = H;
            sv.fpsNum = fps.num;
            sv.fpsDen = fps.den;
            sv.crf = s.crf;
            sv.bitrate = s.videoBitrate;
            sv.preset = s.preset;
            sv.keyint = s.gop;
            sv.primaries = o.vctx->color_primaries;
            sv.transfer = o.vctx->color_trc;
            sv.matrix = o.vctx->colorspace;
            std::string e;
            if (!(o.spatial = SpatialVideoEncoder::open(sv, &e))) return fail(e);
            // The stream as the muxer sees it: HEVC, one eye's size, the base view's parameter sets (for the hvcC).
            AVCodecParameters* par = o.vst->codecpar;
            par->codec_id = AV_CODEC_ID_HEVC;
            par->codec_tag = MKTAG('h', 'v', 'c', '1');
            par->width = W / 2;
            par->bit_rate = s.videoBitrate;
            par->bits_per_coded_sample = 0;
            const std::vector<uint8_t>& headers = o.spatial->baseHeaders();
            av_freep(&par->extradata);
            par->extradata = static_cast<uint8_t*>(av_mallocz(headers.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!par->extradata) return fail("Out of memory");
            std::memcpy(par->extradata, headers.data(), headers.size());
            par->extradata_size = int(headers.size());
        }
        // Packed stereoscopic 3D: stereo metadata, so players and headsets show the eyes apart (MP4 st3d, written
        // only when asked to go beyond the standard, as sv3d is; Matroska StereoMode).
        const bool packed = seq.stereo3d && !spatial && stereoView != StereoView::Left && stereoView != StereoView::Right && stereoView != StereoView::Anaglyph;
        if (packed) {
            if (AVStereo3D* s3d = av_stereo3d_alloc()) {
                s3d->type = stereoView == StereoView::SideBySide || stereoView == StereoView::SideBySideHalf ? AV_STEREO3D_SIDEBYSIDE
                                                                                                            : AV_STEREO3D_TOPBOTTOM;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 27, 100)
                // A zeroed view reads as "packed", which newer FFmpeg also writes as Apple's view box (vexu) that its
                // own reader then lets override st3d's packing: unspecified, st3d alone says how the eyes lie.
                s3d->view = AV_STEREO3D_VIEW_UNSPEC;
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
                if (!av_packet_side_data_add(&o.vst->codecpar->coded_side_data, &o.vst->codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_STEREO3D, s3d, sizeof(AVStereo3D), 0))
                    av_free(s3d);
#else
                if (av_stream_add_side_data(o.vst, AV_PKT_DATA_STEREO3D, reinterpret_cast<uint8_t*>(s3d), sizeof(AVStereo3D)) < 0) av_free(s3d);
#endif
            }
            o.oc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
        }
        if (seq.spherical) {
            // A 360° sequence: equirectangular spherical metadata, so players and YouTube show it as 360°
            // (MP4 sv3d and st3d boxes, which FFmpeg writes only when asked to go beyond the standard and never into MOV;
            // Matroska Projection).
            size_t size = 0;
            if (AVSphericalMapping* map = av_spherical_alloc(&size)) {
                map->projection = AV_SPHERICAL_EQUIRECTANGULAR;
                if (seq.vr180) {
                    // VR180: the front half of the sphere, a quarter of the way in from each side (0.32 fixed point).
                    map->projection = AV_SPHERICAL_EQUIRECTANGULAR_TILE;
                    map->bound_left = map->bound_right = 0x40000000u;
                }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
                if (!av_packet_side_data_add(&o.vst->codecpar->coded_side_data, &o.vst->codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_SPHERICAL, map, size, 0))
                    av_free(map);
#else
                if (av_stream_add_side_data(o.vst, AV_PKT_DATA_SPHERICAL, reinterpret_cast<uint8_t*>(map), size) < 0) av_free(map);
#endif
            }
            o.oc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
        }
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
            cl.MaxCLL = unsigned(maxCll);
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
    // An audio encoder and its stream, in the sequence's layout (or stereo); "" or why not.
    auto openAudio = [&](AVCodecContext*& actx, AVStream*& ast, bool mono = false) -> std::string {
        const AVCodec* codec = avcodec_find_encoder_by_name(s.audioCodec.c_str());
        if (!codec) return "Audio encoder not available: " + s.audioCodec;
        ast = avformat_new_stream(o.oc, nullptr);
        actx = avcodec_alloc_context3(codec);
        // The sequence's layout (the channel order core/Surround.h uses is FFmpeg's), or stereo. exportSequence has
        // already folded a layout the codec cannot carry; one FFmpeg cannot describe is an error, never a stereo stream
        // fed every channel.
        AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
        if (!s.downmixStereo && ambisonicLayout(seq.audioLayout)) {
            // An ambisonic field: named as one for PCM, as four speakers (4.0, in ACN order) for AAC (first order only,
            // exportAudioLayout), which knows no other four-channel layout; the SA3D box written afterwards says what
            // they are.
            if (s.audioCodec.rfind("pcm_", 0) == 0) {
                layout = AVChannelLayout{};
                layout.order = AV_CHANNEL_ORDER_AMBISONIC;
                layout.nb_channels = layoutChannels(seq.audioLayout);
            } else {
                layout = AV_CHANNEL_LAYOUT_4POINT0;
            }
        } else if (!s.downmixStereo && !ffmpegLayout(seq.audioLayout, layout)) {
            return "This FFmpeg cannot describe the " + seq.audioLayout + " channel layout";
        }
        if (mono) layout = AV_CHANNEL_LAYOUT_MONO;
        av_channel_layout_copy(&actx->ch_layout, &layout);
        actx->sample_rate = sr;
        if (s.audioCodec == "libopus" && sr != 48000) actx->sample_rate = 48000;
        // Pick a supported sample format, preferring float planar.
        AVSampleFormat want = AV_SAMPLE_FMT_FLTP;
        if (s.audioCodec == "pcm_s16le") want = AV_SAMPLE_FMT_S16;
        else if (s.audioCodec == "pcm_s24le") want = AV_SAMPLE_FMT_S32;
        else if (s.audioCodec == "libopus") want = AV_SAMPLE_FMT_FLT;
        actx->sample_fmt = want;
        const AVSampleFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const void* cfg = nullptr;
        if (avcodec_get_supported_config(actx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, nullptr) >= 0)
            fmts = static_cast<const AVSampleFormat*>(cfg);
#else
        fmts = codec->sample_fmts;
#endif
        if (fmts) {
            bool ok = false;
            for (const AVSampleFormat* f = fmts; *f != AV_SAMPLE_FMT_NONE; ++f) ok |= (*f == want);
            if (!ok) actx->sample_fmt = fmts[0];
        }
        const bool field = !mono && !s.downmixStereo && ambisonicLayout(seq.audioLayout);
        if (s.audioCodec == "aac" || s.audioCodec == "libopus")
            actx->bit_rate = s.audioBitrate * (field ? layoutChannels(seq.audioLayout) / 2 : 1);  // per pair of channels
        actx->time_base = AVRational{1, actx->sample_rate};
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        if (field && s.audioCodec == "aac") {
            // The field's channels are not left and right: joint stereo coding (mid/side, intensity) and noise
            // substitution would mix or replace their differences, which are the directions.
            av_dict_set(&opts, "aac_ms", "0", 0);
            av_dict_set(&opts, "aac_is", "0", 0);
            av_dict_set(&opts, "aac_pns", "0", 0);
        }
        rc = avcodec_open2(actx, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) return "Cannot open audio encoder: " + averr(rc);
        avcodec_parameters_from_context(ast->codecpar, actx);
        ast->time_base = actx->time_base;
        if (actx->frame_size > 0) audioFrameSize = actx->frame_size;
        return {};
    };
    // A stream's title and language tag.
    auto tagStream = [](AVStream* st, const std::string& title, const std::string& language) {
        if (!title.empty()) {
            av_dict_set(&st->metadata, "title", title.c_str(), 0);
            av_dict_set(&st->metadata, "handler_name", title.c_str(), 0);  // MP4 and MOV keep a track's name there
        }
        if (const char* lang = iso639_2(language)) av_dict_set(&st->metadata, "language", lang, 0);
    };
    // Further audio streams (a master's M&E, dialogue, each dub): each hears its tracks, and only its role when given.
    struct ExtraAudio {
        AVCodecContext* ctx = nullptr;
        AVStream* st = nullptr;
        AVFrame* frame = nullptr;
        AudioMixer mixer;
        Sequence seq;
        std::vector<bool> tracks;
        bool normalise = false;
        double gain = 1;
        std::unique_ptr<PeakLimiter> limiter;
        int64_t delay = 0;
        std::vector<float> fifo;
        int64_t pts = 0, cursor = 0;
        std::unique_ptr<SpeedStream> speed;  // a speed change's, reading this stream's sound
        ~ExtraAudio() {
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
        }
    };
    std::vector<std::unique_ptr<ExtraAudio>> extras;
    // Channels mixed: the sequence's layout, or stereo.
    const int layoutChannels = s.downmixStereo ? 2 : montage::layoutChannels(seq.audioLayout);
    // Mono tracks (broadcast MXF): the mix's channels, each extra stream's, then silence.
    struct MonoOut {
        AVCodecContext* ctx = nullptr;
        AVStream* st = nullptr;
        AVFrame* frame = nullptr;
        std::vector<float> fifo;
        int64_t pts = 0;
        ~MonoOut() {
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
        }
    };
    std::vector<std::unique_ptr<MonoOut>> monos;
    const bool monoTracks = wantAudio && s.monoAudioTracks > 0;
    if (monoTracks) {
        // Each channel named for its speaker, as core/Surround.h names them (7.1's fifth and sixth are the back pair,
        // Lb Rb, then the sides, Ls Rs, in FFmpeg's order).
        const auto& speakers = layoutSpeakers(s.downmixStereo ? std::string("stereo") : seq.audioLayout);
        auto channelName = [&](int c) -> std::string { return c < int(speakers.size()) ? speakers[size_t(c)].name : std::to_string(c + 1); };
        const int used = layoutChannels * int(1 + s.extraAudio.size());
        const int count = std::max(s.monoAudioTracks, used);
        for (int i = 0; i < count; ++i) {
            auto m = std::make_unique<MonoOut>();
            if (const std::string why = openAudio(m->ctx, m->st, true); !why.empty()) return fail(why);
            m->frame = av_frame_alloc();
            const size_t stream = size_t(i / layoutChannels);
            if (i >= used) tagStream(m->st, "Silence", "");
            else if (stream == 0) tagStream(m->st, (s.audioName.empty() ? std::string("Mix") : s.audioName) + " " + channelName(i % layoutChannels), s.audioLanguage);
            else tagStream(m->st, s.extraAudio[stream - 1].name + " " + channelName(i % layoutChannels), s.extraAudio[stream - 1].language);
            monos.push_back(std::move(m));
        }
    }
    if (wantAudio) {
        if (!monoTracks) {
            if (const std::string why = openAudio(o.actx, o.ast); !why.empty()) return fail(why);
            o.aframe = av_frame_alloc();
            tagStream(o.ast, s.audioName, s.audioLanguage);
            if (!s.extraAudio.empty()) o.ast->disposition |= AV_DISPOSITION_DEFAULT;
        }
        for (const ExportSettings::AudioStream& want : s.extraAudio) {
            auto e = std::make_unique<ExtraAudio>();
            if (!monoTracks) {  // with mono tracks the stream's channels go to theirs
                if (const std::string why = openAudio(e->ctx, e->st); !why.empty()) return fail(why);
                e->frame = av_frame_alloc();
                tagStream(e->st, want.name, want.language);
                if (want.descriptions) e->st->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED | AV_DISPOSITION_DESCRIPTIONS;
            }
            e->mixer.setTrackMask(want.tracks);
            e->tracks = want.tracks;
            e->normalise = want.normalise;
            e->seq = seq;
            for (const std::string& r : want.unmute) std::erase(e->seq.mutedRoles, r);
            if (!want.role.empty()) {
                // ("Music+Effects": either, unless a role has that very name)
                auto roleOf = [](const Clip& c) { return c.role.empty() ? std::string("No Role") : c.role; };
                bool named = false;
                for (const Track& t : seq.audioTracks)
                    for (const Clip& c : t.clips) named = named || roleOf(c) == want.role;
                std::vector<std::string> roles;
                for (size_t from = 0; !named;) {
                    const size_t plus = want.role.find('+', from);
                    std::string r = want.role.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
                    r.erase(0, r.find_first_not_of(' '));
                    r.erase(r.find_last_not_of(' ') + 1);
                    roles.push_back(r);
                    if (plus == std::string::npos) break;
                    from = plus + 1;
                }
                if (named) roles = {want.role};
                for (Track& t : e->seq.audioTracks)
                    for (Clip& c : t.clips)
                        if (std::find(roles.begin(), roles.end(), roleOf(c)) == roles.end()) c.enabled = false;
            }
            extras.push_back(std::move(e));
        }
    }
    if (o.actx && o.actx->sample_rate != sr && s.audioCodec != "libopus") return fail("Unsupported sample rate");

    // Captions as subtitle streams, in the text format the container takes: the chosen track, then any others asked for.
    const CaptionTrack* captions = (s.embedCaptions || s.burnInCaptions) ? captionTrackFor(dseq, s.captionTrack) : nullptr;
    // (burnt into the programme's pictures, on its own frames)
    const CaptionTrack* burnCaptions = s.burnInCaptions ? captionTrackFor(seq, s.captionTrack) : nullptr;
    struct CaptionOut {
        const CaptionTrack* track = nullptr;
        AVCodecContext* ctx = nullptr;
        AVStream* st = nullptr;
        size_t next = 0;
        std::vector<SubtitlePacket> pictures;  // image subtitles, coded ahead (times from the export's start)
        bool image = false;
        ~CaptionOut() { avcodec_free_context(&ctx); }
    };
    std::vector<std::unique_ptr<CaptionOut>> captionOuts;
    std::vector<const CaptionTrack*> embedded;
    if (s.embedCaptions && captions && !captions->captions.empty()) embedded.push_back(captions);
    if (s.embedCaptions)
        for (Id id : s.extraCaptions)
            for (const CaptionTrack& t : dseq.captionTracks)
                if (t.id == id && &t != captions && !t.captions.empty()) embedded.push_back(&t);
    for (const CaptionTrack* track : embedded) {
        auto out = std::make_unique<CaptionOut>();
        out->track = track;
        AVCodecID id = AV_CODEC_ID_NONE;
        const bool ts = std::string(o.oc->oformat->name) == "mpegts";
        auto holds = [&](AVCodecID c) { return avformat_query_codec(o.oc->oformat, c, FF_COMPLIANCE_NORMAL) == 1 || (ts && c == AV_CODEC_ID_DVB_SUBTITLE); };
        if (s.captionImages.empty())
            for (AVCodecID c : {AV_CODEC_ID_MOV_TEXT, AV_CODEC_ID_SUBRIP, AV_CODEC_ID_WEBVTT})
                if (holds(c)) {
                    id = c;
                    break;
                }
        // As pictures (render/ImageSubtitles.h) when asked, or where only pictures go (MPEG-TS: DVB subtitles).
        std::string images = s.captionImages;
        if (images.empty() && id == AV_CODEC_ID_NONE && ts) images = "dvb";
        if (!images.empty()) {
            ImageSubtitleFormat f;
            if (!imageSubtitleFormatFromId(images, f) || f == ImageSubtitleFormat::ImscImage)
                return fail("Caption pictures are pgs, dvb or dvd, not " + images);
            id = f == ImageSubtitleFormat::Pgs ? AV_CODEC_ID_HDMV_PGS_SUBTITLE : f == ImageSubtitleFormat::Dvb ? AV_CODEC_ID_DVB_SUBTITLE : AV_CODEC_ID_DVD_SUBTITLE;
            if (!holds(id)) return fail(std::string("This file type cannot hold ") + (f == ImageSubtitleFormat::Pgs ? "PGS" : f == ImageSubtitleFormat::Dvb ? "DVB" : "DVD") +
                                        " subtitles (Matroska holds all three, MPEG-TS DVB)");
            std::vector<uint8_t> extra;
            std::string err;
            if (!encodeImageSubtitles(*track, dseq.fps, f, W / across, H / down, out->pictures, &extra, &err, din, dout)) return fail(err);
            out->image = true;
            out->st = avformat_new_stream(o.oc, nullptr);
            out->st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            out->st->codecpar->codec_id = id;
            out->st->codecpar->width = W / across;
            out->st->codecpar->height = H / down;
            if (!extra.empty()) {
                out->st->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                std::copy(extra.begin(), extra.end(), out->st->codecpar->extradata);
                out->st->codecpar->extradata_size = int(extra.size());
            }
            out->st->time_base = AVRational{1, 90000};
            tagStream(out->st, track->name, track->language);
            if (captionOuts.empty() && embedded.size() > 1) out->st->disposition |= AV_DISPOSITION_DEFAULT;
            captionOuts.push_back(std::move(out));
            continue;
        }
        if (id == AV_CODEC_ID_NONE) return fail("This file type cannot hold captions; export them as a sidecar file instead");
        const AVCodec* codec = avcodec_find_encoder(id);
        if (!codec) return fail("Caption encoder not available");
        out->ctx = avcodec_alloc_context3(codec);
        out->ctx->time_base = AVRational{1, 1000};
        const size_t headerLen = std::char_traits<char>::length(kAssHeader);
        out->ctx->subtitle_header = static_cast<uint8_t*>(av_mallocz(headerLen + 1));
        std::copy(kAssHeader, kAssHeader + headerLen, out->ctx->subtitle_header);
        out->ctx->subtitle_header_size = int(headerLen);
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) out->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((rc = avcodec_open2(out->ctx, codec, nullptr)) < 0) return fail("Cannot open caption encoder: " + averr(rc));
        out->st = avformat_new_stream(o.oc, nullptr);
        avcodec_parameters_from_context(out->st->codecpar, out->ctx);
        out->st->time_base = out->ctx->time_base;
        tagStream(out->st, track->name, track->language);
        if (captionOuts.empty() && embedded.size() > 1) out->st->disposition |= AV_DISPOSITION_DEFAULT;
        out->next = captionIndexAt(*track, din);
        captionOuts.push_back(std::move(out));
    }

    if (!(o.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((rc = avio_open(&o.oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("Cannot write " + s.path + ": " + averr(rc));
        opened = true;
    }
    av_dict_set(&o.oc->metadata, "encoder", "Montage", 0);
    if (!s.startTimecode.empty()) av_dict_set(&o.oc->metadata, "timecode", s.startTimecode.c_str(), 0);
    for (const auto& [key, value] : s.formatMetadata) av_dict_set(&o.oc->metadata, key.c_str(), value.c_str(), 0);
    if (s.chapters) addChapters(o.oc, dseq, din, dout);
    if ((rc = avformat_write_header(o.oc, nullptr)) < 0) return fail("Cannot write header: " + averr(rc));
    o.headerWritten = true;

    const int mixRate = o.actx ? o.actx->sample_rate : !monos.empty() ? monos[0]->ctx->sample_rate : sr;
    Sequence mixSeq = seq;  // mixer runs at the encoder's rate
    mixSeq.sampleRate = mixRate;
    // Channels written: the layout's, or 2 (a surround sequence folded down, or a stereo one).
    const int nch = o.actx ? o.actx->ch_layout.nb_channels : !monos.empty() ? layoutChannels : 2;
    std::vector<double> weights;
    if (nch > 2)
        for (const Speaker& sp : layoutSpeakers(seq.audioLayout)) weights.push_back(sp.loudnessWeight);
    else
        weights = {1.0, 1.0};
    auto mixBlock = [&](AudioMixer& m, int64_t pos, int frames, float* dst) {
        if (nch > 2) m.mixLayout(p, mixSeq, pos, frames, dst);
        else m.mix(p, mixSeq, pos, frames, dst);
    };
    AudioMixer mixer;
    mixer.setTrackMask(s.audioTracks);
    std::vector<float> fifo;
    std::vector<float> mixBuf;
    int64_t audioPts = 0;
    int64_t audioCursor = int64_t(std::llround(double(in) * mixRate / seq.fpsValue()));
    // The loudness report's meters, fed what is delivered.
    std::unique_ptr<LoudnessMeter> reportMeter;
    std::unique_ptr<LeqMMeter> reportLeqm;
    if (wantAudio && s.loudnessReport) {
        reportMeter = std::make_unique<LoudnessMeter>(mixRate);
        const std::vector<double> gains = leqmChannelGains(nch > 2 ? seq.audioLayout : std::string("stereo"), nch);
        reportLeqm = std::make_unique<LeqMMeter>(mixRate, nch, gains.data());
    }
    for (auto& e : extras) {  // every stream starts where the mix does
        e->cursor = audioCursor;
        e->seq.sampleRate = mixRate;
    }
    // A speed change: each stream's sound read through its own speed change, from where the programme starts (the
    // cursors then count delivered samples).
    const int64_t audioStart = audioCursor;
    std::unique_ptr<SpeedStream> mainSpeed;
    if (conv.mode == RateConversion::Mode::Speed) {
        // (the speed change reads a little ahead: nothing after the range's end is heard)
        const int64_t audioEnd = int64_t(std::llround(double(out) * mixRate / seq.fpsValue()));
        auto silenceAfterEnd = [nch, audioEnd](int64_t at, int frames, float* dst) {
            if (at + frames > audioEnd)
                std::fill(dst + size_t(std::clamp<int64_t>(audioEnd - at, 0, frames)) * size_t(nch), dst + size_t(frames) * size_t(nch), 0.0f);
        };
        int64_t mainAt = audioStart;
        mainSpeed = std::make_unique<SpeedStream>(nch, mixRate, conv.speed(), conv.keepPitch, [&, mainAt, silenceAfterEnd](int frames, float* dst) mutable {
            mixBlock(mixer, mainAt, frames, dst);
            silenceAfterEnd(mainAt, frames, dst);
            mainAt += frames;
        });
        for (auto& x : extras) {
            ExtraAudio* e = x.get();
            int64_t at = audioStart;
            e->speed = std::make_unique<SpeedStream>(nch, mixRate, conv.speed(), conv.keepPitch, [&, e, at, silenceAfterEnd](int frames, float* dst) mutable {
                if (nch > 2) e->mixer.mixLayout(p, e->seq, at, frames, dst);
                else e->mixer.mix(p, e->seq, at, frames, dst);
                silenceAfterEnd(at, frames, dst);
                at += frames;
            });
        }
    }
    // The next `frames` of the mix (or an extra stream's) from `pos`: the programme's, or through its speed change.
    auto mixMain = [&](int64_t pos, int frames, float* dst) {
        if (mainSpeed) mainSpeed->read(frames, dst);
        else mixBlock(mixer, pos, frames, dst);
    };
    auto mixExtra = [&](ExtraAudio& e, int64_t pos, int frames, float* dst) {
        if (e.speed) e.speed->read(frames, dst);
        else if (nch > 2) e.mixer.mixLayout(p, e.seq, pos, frames, dst);
        else e.mixer.mix(p, e.seq, pos, frames, dst);
    };
    // Where the mix has reached by the end of delivered frame `f` (in samples from the programme's start, as the cursors
    // count them).
    auto audioUntil = [&](FrameTime f) {
        if (!conv.active()) return int64_t(std::llround(double(f + 1) * mixRate / seq.fpsValue()));
        return audioStart + int64_t(std::llround(double(f - din + 1) * mixRate / dseq.fpsValue()));
    };
    std::vector<uint16_t> rgba16;
    // Loudness normalisation: measure the whole mix first, then play it through a gain and a limiter.
    double normGain = 1;
    std::unique_ptr<PeakLimiter> limiter;
    int64_t limiterDelay = 0;
    // The dialogue the normalisation measured (for the report to mark the same).
    std::vector<std::pair<double, double>> normalisedSpeech;
    SpeechGate normalisedGate = SpeechGate::Auto;
    bool normalisedByDialogue = false;
    if (wantAudio && s.loudnessTarget < 0) {
        const int64_t end = int64_t(std::llround(double(out) * mixRate / seq.fpsValue()));
        std::vector<float> chunk(size_t(8192) * size_t(nch));
        // The dialogue's loudness (Netflix's), or everything's.
        auto dialogueGain = [&](const Sequence& sq, const std::vector<bool>& tracks, double& gain, DialogueLoudness* found = nullptr) {
            DialogueLoudnessOptions o;
            o.tracks = tracks;
            o.in = in;
            o.out = out;
            o.layout = nch > 2;
            const DialogueLoudness d = measureDialogueLoudness(p, sq, o, {}, cancel);
            if (d.valid) gain = std::pow(10.0, (s.loudnessTarget - d.value()) / 20.0);
            if (found) *found = d;
            return !(cancel && cancel->load());
        };
        if (s.dialogueGated) {
            DialogueLoudness found;
            if (!dialogueGain(mixSeq, s.audioTracks, normGain, &found)) return fail("Cancelled");
            normalisedSpeech = std::move(found.speech);
            normalisedGate = found.gate;
            normalisedByDialogue = true;
        } else {
            AudioMixer meterMixer;
            meterMixer.setTrackMask(s.audioTracks);
            LoudnessMeter meter(mixRate);
            for (int64_t pos = audioCursor; pos < end;) {
                if (cancel && cancel->load()) return fail("Cancelled");
                const int n = int(std::min<int64_t>(8192, end - pos));
                mixBlock(meterMixer, pos, n, chunk.data());
                meter.addChannels(chunk.data(), n, nch, weights.data());
                pos += n;
            }
            const LoudnessResult measured = meter.result();
            if (measured.valid) normGain = std::pow(10.0, (s.loudnessTarget - measured.integrated) / 20.0);
        }
        // A little under the ceiling: the limiter sees samples, and true peaks fall between them.
        limiter = std::make_unique<PeakLimiter>(mixRate, s.peakCeiling - 0.5, 5, 80, nch);
        limiterDelay = limiter->latency();
        // Fill the limiter's look-ahead so its output starts at the first sample.
        std::vector<float> prime(size_t(limiterDelay) * size_t(nch));
        mixMain(audioCursor, int(limiterDelay), prime.data());
        for (float& v : prime) v = float(v * normGain);
        limiter->process(prime.data(), prime.data(), int(limiterDelay));
        audioCursor += limiterDelay;
        // Streams of the whole programme (the described one) likewise, each measured on its own.
        for (auto& e : extras) {
            if (!e->normalise) continue;
            if (s.dialogueGated) {
                if (!dialogueGain(e->seq, e->tracks, e->gain)) return fail("Cancelled");
            } else {
                AudioMixer m;
                m.setTrackMask(e->tracks);
                LoudnessMeter em(mixRate);
                for (int64_t pos = e->cursor; pos < end;) {
                    if (cancel && cancel->load()) return fail("Cancelled");
                    const int n = int(std::min<int64_t>(8192, end - pos));
                    if (nch > 2) m.mixLayout(p, e->seq, pos, n, chunk.data());
                    else m.mix(p, e->seq, pos, n, chunk.data());
                    em.addChannels(chunk.data(), n, nch, weights.data());
                    pos += n;
                }
                const LoudnessResult r = em.result();
                if (r.valid) e->gain = std::pow(10.0, (s.loudnessTarget - r.integrated) / 20.0);
            }
            e->limiter = std::make_unique<PeakLimiter>(mixRate, s.peakCeiling - 0.5, 5, 80, nch);
            e->delay = e->limiter->latency();
            std::vector<float> ep(size_t(e->delay) * size_t(nch));
            mixExtra(*e, e->cursor, int(e->delay), ep.data());
            for (float& v : ep) v = float(v * e->gain);
            e->limiter->process(ep.data(), ep.data(), int(e->delay));
            e->cursor += e->delay;
        }
    }

    auto encodeFifo = [&](AVCodecContext* actx, AVStream* ast, AVFrame* aframe, std::vector<float>& fifo, int64_t& audioPts,
                          bool final) -> bool {
        const size_t nc = size_t(actx->ch_layout.nb_channels);
        while (fifo.size() >= size_t(audioFrameSize) * nc || (final && !fifo.empty())) {
            int n = std::min<int>(audioFrameSize, int(fifo.size() / nc));
            av_frame_unref(aframe);
            aframe->nb_samples = n;
            aframe->format = actx->sample_fmt;
            aframe->sample_rate = actx->sample_rate;
            av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
            if (av_frame_get_buffer(aframe, 0) < 0) return false;
            const float* srcp = fifo.data();
            switch (actx->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP:
                    for (size_t c = 0; c < nc; ++c) {
                        auto* d = reinterpret_cast<float*>(aframe->data[c]);
                        for (int i = 0; i < n; ++i) d[i] = srcp[size_t(i) * nc + c];
                    }
                    break;
                case AV_SAMPLE_FMT_FLT:
                    std::copy(srcp, srcp + size_t(n) * nc, reinterpret_cast<float*>(aframe->data[0]));
                    break;
                case AV_SAMPLE_FMT_S16: {
                    auto* d = reinterpret_cast<int16_t*>(aframe->data[0]);
                    for (size_t i = 0; i < size_t(n) * nc; ++i) d[i] = int16_t(std::lround(std::clamp(srcp[i], -1.0f, 1.0f) * 32767.0f));
                    break;
                }
                case AV_SAMPLE_FMT_S32: {
                    auto* d = reinterpret_cast<int32_t*>(aframe->data[0]);
                    for (size_t i = 0; i < size_t(n) * nc; ++i)
                        d[i] = int32_t(std::llround(double(std::clamp(srcp[i], -1.0f, 1.0f)) * 2147483647.0));
                    break;
                }
                case AV_SAMPLE_FMT_S16P:
                    for (size_t c = 0; c < nc; ++c) {
                        auto* d = reinterpret_cast<int16_t*>(aframe->data[c]);
                        for (int i = 0; i < n; ++i) d[i] = int16_t(std::lround(std::clamp(srcp[size_t(i) * nc + c], -1.0f, 1.0f) * 32767.0f));
                    }
                    break;
                default: return false;
            }
            aframe->pts = audioPts;
            audioPts += n;
            fifo.erase(fifo.begin(), fifo.begin() + long(size_t(n) * nc));
            if (avcodec_send_frame(actx, aframe) < 0) return false;
            if (drain(o, actx, ast) < 0) return false;
        }
        return true;
    };
    auto encodeAudio = [&](bool final) { return encodeFifo(o.actx, o.ast, o.aframe, fifo, audioPts, final); };

    // Writes the captions that start before frame `until` (export range only), on every caption stream.
    std::vector<uint8_t> subBuf(1 << 16);
    auto writeCaptions = [&](FrameTime until, bool last = false) -> bool {
        const double msPerFrame = 1000.0 / dseq.fpsValue();
        for (auto& co : captionOuts) {
            if (!co->image) continue;
            // Pictures: each packet whose time has come (at the end, the one clearing the last caption too).
            const int64_t limit = int64_t(std::llround(double(until - din) * 90000.0 / dseq.fpsValue()));
            for (; co->next < co->pictures.size() && (co->pictures[co->next].pts < limit || (last && co->pictures[co->next].pts <= limit)); ++co->next) {
                const SubtitlePacket& sp = co->pictures[co->next];
                av_packet_unref(o.pkt);
                if (av_new_packet(o.pkt, int(sp.data.size())) < 0) return false;
                std::copy(sp.data.begin(), sp.data.end(), o.pkt->data);
                o.pkt->pts = o.pkt->dts = av_rescale_q(sp.pts, AVRational{1, 90000}, co->st->time_base);
                o.pkt->duration = av_rescale_q(sp.duration, AVRational{1, 90000}, co->st->time_base);
                o.pkt->flags |= AV_PKT_FLAG_KEY;
                o.pkt->stream_index = co->st->index;
                if (av_interleaved_write_frame(o.oc, o.pkt) < 0) return false;
            }
        }
        for (auto& co : captionOuts)
        if (!co->image)
        for (; co->next < co->track->captions.size() && co->track->captions[co->next].start < until; ++co->next) {
            const Caption& c = co->track->captions[co->next];
            const FrameTime a = std::max(c.start, din), b = std::min(c.end, dout);
            if (b <= a) continue;
            const int64_t startMs = std::llround(double(a - din) * msPerFrame);
            const int64_t durMs = std::max<int64_t>(1, std::llround(double(b - a) * msPerFrame));
            AVSubtitle sub{};
            sub.format = 1;  // text
            sub.pts = av_rescale_q(startMs, AVRational{1, 1000}, AV_TIME_BASE_Q);
            sub.end_display_time = uint32_t(durMs);
            sub.num_rects = 1;
            sub.rects = static_cast<AVSubtitleRect**>(av_mallocz(sizeof(AVSubtitleRect*)));
            sub.rects[0] = static_cast<AVSubtitleRect*>(av_mallocz(sizeof(AVSubtitleRect)));
            sub.rects[0]->type = SUBTITLE_ASS;
            const int keypad = captionKeypad(c);  // players that read alignment tags keep its place
            sub.rects[0]->ass = av_strdup(
                ("0,0,Default,,0,0,0,," + (keypad != 2 ? "{\\an" + std::to_string(keypad) + "}" : std::string()) + assText(c.text)).c_str());
            const int n = avcodec_encode_subtitle(co->ctx, subBuf.data(), int(subBuf.size()), &sub);
            avsubtitle_free(&sub);
            if (n < 0) return false;
            av_packet_unref(o.pkt);
            if (av_new_packet(o.pkt, n) < 0) return false;
            std::copy(subBuf.begin(), subBuf.begin() + n, o.pkt->data);
            o.pkt->pts = o.pkt->dts = av_rescale_q(startMs, AVRational{1, 1000}, co->st->time_base);
            o.pkt->duration = av_rescale_q(durMs, AVRational{1, 1000}, co->st->time_base);
            o.pkt->stream_index = co->st->index;
            if (av_interleaved_write_frame(o.oc, o.pkt) < 0) return false;
        }
        return true;
    };

    const int64_t total = dout - din;
    // The burn-in logo, loaded once.
    const QImage watermark = s.burnIn.watermark.empty() ? QImage() : QImage(QString::fromStdString(s.burnIn.watermark));
    if (!s.burnIn.watermark.empty() && watermark.isNull()) return fail("Cannot read the watermark image " + s.burnIn.watermark);
    RenderOptions ro;
    ro.scale = double(W / across) / seq.width;
    ro.highQuality = true;
    ro.useProxies = s.useProxies;
    ro.gpu = s.gpu;
    // Graded scene-referred, the frames are made for the space delivered, through its own output transform (a trim
    // pass's target's, its trim after it; a scene-referred one gets the working space's light).
    const bool sceneGraded = sceneReferredWorking(seq);
    const ColorSpace* sceneLight = sceneGraded && trim ? &linearColorSpace(workingColorSpace(seq).primaries) : nullptr;
    if (sceneGraded) {
        ro.outputSpace = trim ? seqSpace.id : outSpace.id;
        ro.outputPeak = peakNits;
        ro.sceneLight = trim != nullptr;
    }
    const ColorSpace& frameSpace = sceneGraded && !trim ? outSpace : seqSpace;  // (the frames' space, once made)
    ro.stereoView = stereoView;
    // A frame an effect could not finish (its model missing or failing) fails the export rather than going out without it.
    RenderIssues issues;
    ro.issues = &issues;
    ro.cancel = cancel;
    // Smart rendering: only where nothing would change the source's pictures for the whole export.
    std::unique_ptr<SmartRenderer> smart;
    if (wantVideo && s.smartRender && o.vctx && isIntraCodec(o.vctx->codec ? o.vctx->codec->name : "") && W == seq.width && H == seq.height && !seq.stereo3d &&
        !s.useProxies && !s.burnInCaptions && !s.cea608 && !interlacedOut && !s.burnIn.any() && sameColorSpace(outSpace, seqSpace) && !trim && !sceneGraded &&
        !conv.active())
        smart = std::make_unique<SmartRenderer>(p, seq, seqSpace, o.vctx, o.vst, s.alpha, s.profile);
    // CEA-608 (and CTA-708) captions in the video: each frame's A/53 cc_data (core/Cea708.h).
    std::vector<std::vector<CcTriplet>> ccFrames;
    if (s.cea608 && wantVideo) {
        const std::string vc = o.vctx && o.vctx->codec ? o.vctx->codec->name : "";
        if (o.vctx->codec_id != AV_CODEC_ID_H264 && o.vctx->codec_id != AV_CODEC_ID_HEVC)
            return fail("CEA-608 captions go in H.264 or HEVC video, not " + vc);
        if (const CaptionTrack* t = captionTrackFor(dseq, s.captionTrack)) {
            std::vector<Caption> part;
            for (Caption c : t->captions) {
                if (c.end <= din || c.start >= dout) continue;
                c.start = std::max(c.start, din) - din;
                c.end = std::min(c.end, dout) - din;
                part.push_back(std::move(c));
            }
            ccFrames = captionCcData(part, dseq.fps, dout - din, true, s.cea708, t->style);
        } else {
            ccFrames = captionCcData({}, dseq.fps, dout - din, true, s.cea708);
        }
    }
    auto attach608 = [&](FrameTime rel) {
        if (!s.cea608) return true;
        av_frame_remove_side_data(o.vframe, AV_FRAME_DATA_A53_CC);
        if (rel < 0 || size_t(rel) >= ccFrames.size()) return true;
        std::vector<uint8_t> cc;
        for (const CcTriplet& t : ccFrames[size_t(rel)]) cc.insert(cc.end(), t.begin(), t.end());
        if (cc.empty()) return true;
        AVFrameSideData* sd = av_frame_new_side_data(o.vframe, AV_FRAME_DATA_A53_CC, cc.size());
        if (!sd) return false;
        std::memcpy(sd->data, cc.data(), cc.size());
        return true;
    };
    const bool burns = (s.burnInCaptions && captions) || s.burnIn.any();
    const bool packedEyes = seq.stereo3d && stereoView != StereoView::Left && stereoView != StereoView::Right && stereoView != StereoView::Anaglyph;
    // At another rate, captions and burn-ins go on the delivered frames (so an in-between does not ghost them, the
    // timecode counts the delivered frames and a woven frame shows one); packed eyes are then converted each on its own.
    const bool burnDelivered = burns && conv.active();
    const bool eyesApart = burnDelivered && packedEyes;
    auto renderEye = [&](FrameTime f, StereoView v) {
        RenderOptions eye = ro;
        eye.stereoView = v;
        Image img = renderProgramFrame(p, seq, f, eye);
        if (trim) applyTrimPass(img, seq, f, *trim, sceneLight, true);
        return img;
    };
    // A frame of the programme, in the output space: rendered, with captions and burn-ins unless they go on the
    // delivered frames; `eye` 1 or 2 is the left or right eye alone.
    auto programmeFrame = [&](FrameTime f, int eye = 0) {
        auto burn = [&](Image& img) {
            if (burnCaptions) drawCaption(img, *burnCaptions, f, &frameSpace);
            if (s.burnIn.any()) drawBurnIns(img, p, seq, f, s.burnIn, watermark.isNull() ? nullptr : &watermark, &frameSpace);
        };
        Image img;
        if (eye) {
            img = renderEye(f, eye == 1 ? StereoView::Left : StereoView::Right);
        } else if (burns && packedEyes) {
            // Packed eyes: captions and burn-ins on each eye, so they sit on the screen in both rather than across
            // the join.
            Image left = renderEye(f, StereoView::Left), right = renderEye(f, StereoView::Right);
            burn(left);
            burn(right);
            img = combineStereo(left, right, stereoView);
        } else {
            img = s.alpha && !seq.stereo3d ? renderSequenceFrame(p, seq, f, ro) : renderProgramFrame(p, seq, f, ro);
            if (trim) applyTrimPass(img, seq, f, *trim, sceneLight, true);
            if (!burnDelivered) burn(img);
        }
        convertColor(img, frameSpace, outSpace, peakNits);
        return img;
    };
    // Delivered at another rate: the programme's pictures kept while the frames round them use them again (a standards
    // conversion's in-betweens and its cut test, pulldown's fields), each with its look.
    static std::atomic<uint64_t> exportRuns{0};
    const uint64_t exportRun = ++exportRuns;
    std::map<std::pair<FrameTime, int>, std::shared_ptr<const Image>> held;
    std::map<FrameTime, ShotLook> looks;
    auto heldFrame = [&](FrameTime f, int eye = 0) -> std::shared_ptr<const Image> {
        f = std::clamp<FrameTime>(f, in, out - 1);
        if (auto it = held.find({f, eye}); it != held.end()) return it->second;
        while (held.size() >= (eyesApart ? 10u : 5u)) held.erase(held.begin());
        auto img = std::make_shared<const Image>(programmeFrame(f, eye));
        held[{f, eye}] = img;
        return img;
    };
    auto lookOf = [&](FrameTime f) -> const ShotLook& {
        if (auto it = looks.find(f); it != looks.end()) return it->second;
        while (looks.size() >= 8) looks.erase(looks.begin());
        return looks[f] = shotLook(*heldFrame(f, eyesApart ? 1 : 0));
    };
    // Whether a cut falls between programme frames f and f + 1: an edit there, or two pictures that are plainly
    // different shots (a cut inside a clip, as a programme imported whole has).
    auto cutAfter = [&](FrameTime f) {
        if (editBetween(seq, f + 1)) return true;
        // (copies: each lookOf can drop the looks kept before it)
        const ShotLook a = lookOf(f), b = lookOf(f + 1);
        const std::optional<ShotLook> before = f > in ? std::optional<ShotLook>(lookOf(f - 1)) : std::nullopt;
        const std::optional<ShotLook> after = f + 2 < out ? std::optional<ShotLook>(lookOf(f + 2)) : std::nullopt;
        return looksLikeCut(a, b, before ? &*before : nullptr, after ? &*after : nullptr);
    };
    // The frame a conversion makes for delivered frame o (of one eye, or both).
    auto convertedFrame = [&](FrameTime o, int eye) -> Image {
        const DeliveredPicture pic = deliveredPicture(conv, o - din, out - in);
        const FrameTime a = in + pic.a, b = in + pic.b;
        if (pic.t <= 0 || pic.woven || b == a) return *heldFrame(a, eye);
        if (cutAfter(a)) return *heldFrame(pic.t < 0.5 ? a : b, eye);  // never a blend of two shots
        const std::shared_ptr<const Image> A = heldFrame(a, eye), B = heldFrame(b, eye);
        // (the flow and in-between caches outlive the export: keyed by this export, the eye and the size)
        const std::string key = "export#" + std::to_string(exportRun) + "#" + std::to_string(eye) + "#" + std::to_string(A->width) + "x" +
                                std::to_string(A->height) + "#" + std::to_string(a);
        Image mid;
        if (rifeAvailable() && rifeModel().installed() && cachedRife(*A, *B, pic.t, key, mid)) return mid;
        return interpolateFrames(*A, *B, pic.t, key);
    };
    // Captions and burn-ins on delivered frame o, in the output space, with its timecode counted at the delivered rate
    // from where the range starts in the programme (non-drop-frame for pulldown, as film transfers count, so each A
    // frame falls on a frame number ending in 0 or 5).
    const FrameTime deliveredTimecode = conv.active() ? deliveredFrameOf(conv, in) : 0;
    const bool dropFrame = conv.mode != RateConversion::Mode::Pulldown;
    auto burnOn = [&](Image& img, FrameTime o) {
        if (s.burnInCaptions && captions) drawCaption(img, *captions, o, &outSpace);
        if (s.burnIn.any()) drawBurnIns(img, p, dseq, o, s.burnIn, watermark.isNull() ? nullptr : &watermark, &outSpace, deliveredTimecode + o, dropFrame);
    };
    auto finished = [&](FrameTime o, const std::function<Image(int)>& eyeFrame) {
        if (!burnDelivered) return eyeFrame(0);
        if (!eyesApart) {
            Image img = eyeFrame(0);
            burnOn(img, o);
            return img;
        }
        Image left = eyeFrame(1), right = eyeFrame(2);
        burnOn(left, o);
        burnOn(right, o);
        return combineStereo(left, right, stereoView);
    };
    // A frame as delivered: the programme's, or (at another rate) the frame its conversion makes. Pulldown's woven frames
    // are their first field's frame here, the second field woven in after (secondFieldOf).
    auto deliveredFrame = [&](FrameTime o) -> Image {
        if (!conv.active()) return programmeFrame(o);
        return finished(o, [&](int eye) { return convertedFrame(o, eye); });
    };
    // The picture a woven frame's second field comes from, with the frame's own captions and burn-ins.
    auto secondFieldOf = [&](FrameTime o, const DeliveredPicture& pic) {
        return finished(o, [&](int eye) { return *heldFrame(in + pic.b, eye); });
    };
    // HDR10+: each scene's metadata for the frames exported. The sequence's analysis is used where it still matches
    // the cut (not when burn-ins or packed eyes change the picture from what was analysed); the rest is measured now on
    // the frames as delivered, as the first part of the progress.
    if (wantHdr10) {
        const bool reuse = !burns && !packedEyes && !trim;  // (the analysis is the master's)
        FrameTime toMeasure = 0;
        renderFrom = 0.3;
        std::string e;
        const auto part = [&](double f) {
            if (progress) progress(renderFrom * f, in + FrameTime(std::floor(f * double(out - in - 1))));
        };
        // (at another rate, the delivered frames are measured)
        if (!hdr10PlusForRange(p, dseq, din, dout, outSpace, peakNits, reuse && !conv.active(), hdr10, &e, part, cancel, deliveredFrame, &toMeasure))
            return fail(cancel && cancel->load() ? std::string("Export cancelled") : "Dynamic HDR metadata analysis failed: " + e);
        if (toMeasure == 0) renderFrom = 0;
        // In the video itself: an SEI message in each HEVC access unit, a metadata OBU in each AV1 temporal unit, the
        // scene's values on every frame of it.
        if (!hdr10.empty() && o.vctx && (o.vctx->codec_id == AV_CODEC_ID_HEVC || o.vctx->codec_id == AV_CODEC_ID_AV1)) {
            const bool av1 = o.vctx->codec_id == AV_CODEC_ID_AV1;
            // Packets carry NAL units after start codes, or after lengths when the encoder's header is an hvcC record.
            const int lengthSize = !av1 && o.vctx->extradata_size > 22 && o.vctx->extradata[0] == 1 ? (o.vctx->extradata[21] & 3) + 1 : 0;
            auto messages = std::make_shared<std::vector<std::vector<uint8_t>>>();
            for (const Hdr10PlusScene& sc : hdr10) messages->push_back(hdr10PlusT35(sc));
            const auto scenes = std::make_shared<std::vector<Hdr10PlusScene>>(hdr10);
            const FrameTime first = din;
            o.videoPacket = [scenes, messages, av1, lengthSize, first](AVPacket* pkt) {
                if (pkt->pts == AV_NOPTS_VALUE) return false;
                const FrameTime f = first + pkt->pts;
                const auto it = std::upper_bound(scenes->begin(), scenes->end(), f,
                                                 [](FrameTime t, const Hdr10PlusScene& sc) { return t < sc.end; });
                if (it == scenes->end() || f < it->start) return false;
                const auto& msg = (*messages)[size_t(it - scenes->begin())];
                return av1 ? addHdr10PlusObu(pkt, msg) : addHdr10PlusSei(pkt, msg, lengthSize);
            };
        }
    }
    // Adaptive HDR metadata: each scene fitted from a few of its frames and their SDR renditions (the sequence's analysis
    // where it still matches), as part of the progress; then a message on every frame of HEVC and AV1 video.
    if (wantAdaptive && o.vctx && (o.vctx->codec_id == AV_CODEC_ID_HEVC || o.vctx->codec_id == AV_CODEC_ID_AV1)) {
        const double share = 0.1;
        const double before = renderFrom;
        renderFrom += share;
        std::string e;
        FrameTime toMeasure = 0;
        const auto part = [&](double f) {
            if (progress) progress(before + share * f, in);
        };
        // (Delivered at another rate: the programme is measured and its scenes put on the delivered frames.)
        // (The pictures as graded, without burn-ins: the sequence's own analysis serves unless a trim pass is delivered.)
        if (!adaptiveHdrForRange(p, seq, in, out, outSpace, peakNits, true, adaptive, &e, part, cancel,
                                 trim && trimTargetSpace(seq, trim->id).hdr() ? trim : nullptr, &toMeasure))
            return fail(cancel && cancel->load() ? std::string("Export cancelled") : "Adaptive HDR metadata analysis failed: " + e);
        if (toMeasure == 0) renderFrom = before;
        if (conv.active())
            for (AdaptiveHdrScene& sc : adaptive) {
                sc.start = din + deliveredFrameOf(conv, sc.start - in);
                sc.end = sc.end >= out ? dout : din + deliveredFrameOf(conv, sc.end - in);
            }
        std::erase_if(adaptive, [](const AdaptiveHdrScene& sc) { return sc.end <= sc.start; });
        if (!adaptive.empty()) {
            const bool av1 = o.vctx->codec_id == AV_CODEC_ID_AV1;
            const int lengthSize = !av1 && o.vctx->extradata_size > 22 && o.vctx->extradata[0] == 1 ? (o.vctx->extradata[21] & 3) + 1 : 0;
            auto messages = std::make_shared<std::vector<std::vector<uint8_t>>>();
            for (const AdaptiveHdrScene& sc : adaptive) messages->push_back(adaptiveHdrT35(sc.md));
            const auto scenes = std::make_shared<std::vector<AdaptiveHdrScene>>(adaptive);
            const FrameTime first = din;
            std::function<bool(AVPacket*)> before10 = o.videoPacket;  // (HDR10+'s, when both are carried)
            o.videoPacket = [before10, scenes, messages, av1, lengthSize, first](AVPacket* pkt) {
                if (before10 && !before10(pkt)) return false;
                if (pkt->pts == AV_NOPTS_VALUE) return true;
                const FrameTime f = first + pkt->pts;
                const auto it = std::upper_bound(scenes->begin(), scenes->end(), f,
                                                 [](FrameTime t, const AdaptiveHdrScene& sc) { return t < sc.end; });
                if (it == scenes->end() || f < it->start) return true;
                const auto& msg = (*messages)[size_t(it - scenes->begin())];
                return av1 ? addHdr10PlusObu(pkt, msg) : addHdr10PlusSei(pkt, msg, lengthSize);
            };
        }
    }
    // Pulldown's second fields: the frame they come from, in the encoder's format.
    AVFrame* secondField = nullptr;
    struct FreeFrame {
        AVFrame*& f;
        ~FreeFrame() { av_frame_free(&f); }
    } freeSecond{secondField};
    for (FrameTime f = din; f < dout; ++f) {
        if (cancel && cancel->load()) return fail("Export cancelled");
        if (!writeCaptions(f + 1)) return fail("Writing captions failed");
        if (wantVideo && smart && smart->copy(f, in, o.oc)) {
            // copied from the source
        } else if (wantVideo) {
            Image img = deliveredFrame(f);
            if (issues.incomplete) return fail(cancel && cancel->load() ? std::string("Export cancelled") : issues.reason());
            if (measure) meter.add(img, f, measured);
            if (exrLibrary) {
                // Through the OpenEXR library: the picture as half floats, premultiplied as OpenEXR files are (and as the
                // layers copied beside it and Montage's own reader take them), then any more layers, under the file name
                // FFmpeg's image writer would give it.
                if (img.width != W || img.height != H) img = resizeImage(img, W, H);
                std::vector<ExrOutLayer> layers(1);
                layers[0].channels = s.alpha ? std::vector<std::string>{"R", "G", "B", "A"} : std::vector<std::string>{"R", "G", "B"};
                const size_t nc = layers[0].channels.size();
                layers[0].data.resize(size_t(W) * size_t(H) * nc);
                parallelRows(H, [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        const float* src = img.row(y);
                        float* dst = &layers[0].data[size_t(y) * size_t(W) * nc];
                        for (int x = 0; x < W; ++x, src += 4, dst += nc) {
                            dst[0] = src[0];
                            dst[1] = src[1];
                            dst[2] = src[2];
                            if (s.alpha) dst[3] = src[3];
                        }
                    }
                });
                std::map<std::string, std::string> attributes;
                std::string why;
                if (s.exrLayers && !s.exrLayers(f, W, H, layers, attributes, why)) return fail(why);
                char name[4096];
                if (av_get_frame_filename2(name, sizeof name, outPath.c_str(), s.startNumber + int(f - din), 0) < 0)
                    return fail("The file name " + outPath + " has no frame number");
                if (!writeExr(name, W, H, layers, s.exrCompression.empty() ? "zip" : s.exrCompression, attributes, &why))
                    return fail(why);
            } else if (floatFrames && img.width == W && img.height == H) {
                // Planar float, green-blue-red(-alpha), unpremultiplied, values above 1 kept.
                if (av_frame_make_writable(o.vframe) < 0) return fail("Out of memory");
                const bool alphaPlane = o.vctx->pix_fmt == AV_PIX_FMT_GBRAPF32LE;
                for (int y = 0; y < H; ++y) {
                    const float* src = img.row(y);
                    auto* g = reinterpret_cast<float*>(o.vframe->data[0] + size_t(y) * size_t(o.vframe->linesize[0]));
                    auto* b = reinterpret_cast<float*>(o.vframe->data[1] + size_t(y) * size_t(o.vframe->linesize[1]));
                    auto* r = reinterpret_cast<float*>(o.vframe->data[2] + size_t(y) * size_t(o.vframe->linesize[2]));
                    auto* a = alphaPlane ? reinterpret_cast<float*>(o.vframe->data[3] + size_t(y) * size_t(o.vframe->linesize[3])) : nullptr;
                    for (int x = 0; x < W; ++x, src += 4) {
                        const float al = src[3], k = al > 1e-6f ? 1.0f / al : 0.0f;
                        r[x] = alphaPlane ? src[0] * k : src[0];
                        g[x] = alphaPlane ? src[1] * k : src[1];
                        b[x] = alphaPlane ? src[2] * k : src[2];
                        if (a) a[x] = al;
                    }
                }
                o.vframe->pts = f - din;
                if (interlacedOut) o.vframe->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
                if (!attach608(f - din)) return fail("Out of memory");
                if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("Video encoding failed: " + averr(rc));
                if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing video failed: " + videoError(o, rc));
            } else {
            toRgba16(img, rgba16);
            AVPixelFormat srcFmt = AV_PIX_FMT_RGBA64LE;
            const int swsFlags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP;
            if (!o.sws && (o.vctx->chroma_sample_location == AVCHROMA_LOC_TOPLEFT || o.vctx->chroma_sample_location == AVCHROMA_LOC_LEFT)) {
                // Chroma made where the encoder's format sites it (otherwise FFmpeg's scaler centres it, as the
                // decoder reads untagged 4:2:0 pictures). The cached context below keeps this one, as it matches.
                o.sws = sws_alloc_context();
                if (o.sws) {
                    av_opt_set_int(o.sws, "srcw", img.width, 0);
                    av_opt_set_int(o.sws, "srch", img.height, 0);
                    av_opt_set_int(o.sws, "src_format", srcFmt, 0);
                    av_opt_set_int(o.sws, "dstw", W, 0);
                    av_opt_set_int(o.sws, "dsth", H, 0);
                    av_opt_set_int(o.sws, "dst_format", o.vctx->pix_fmt, 0);
                    av_opt_set_int(o.sws, "sws_flags", swsFlags, 0);
                    av_opt_set_int(o.sws, "dst_h_chr_pos", 0, 0);
                    // (in 1/256 luma pixels: left, and top or centred between the lines it spans, as the decoder takes it)
                    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(o.vctx->pix_fmt);
                    const int spans = d ? 1 << d->log2_chroma_h : 1;
                    av_opt_set_int(o.sws, "dst_v_chr_pos", o.vctx->chroma_sample_location == AVCHROMA_LOC_TOPLEFT ? 0 : (spans - 1) * 128, 0);
                    if (sws_init_context(o.sws, nullptr, nullptr) < 0) {
                        sws_freeContext(o.sws);
                        o.sws = nullptr;
                    }
                }
            }
            o.sws = sws_getCachedContext(o.sws, img.width, img.height, srcFmt, W, H, o.vctx->pix_fmt, swsFlags, nullptr, nullptr, nullptr);
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
            if (const DeliveredPicture pic = conv.active() ? deliveredPicture(conv, f - din, out - in) : DeliveredPicture{}; pic.woven) {
                // Pulldown's B C and C D frames: the second (bottom) field from the next picture, woven in the encoder's
                // format, each plane's lines alternating (4:2:0 chroma too, as FFmpeg's telecine and field matching
                // weave it).
                Image second = secondFieldOf(f, pic);
                if (second.width != img.width || second.height != img.height) second = resizeImage(second, img.width, img.height);
                toRgba16(second, rgba16);
                const uint8_t* secondData[4] = {reinterpret_cast<const uint8_t*>(rgba16.data()), nullptr, nullptr, nullptr};
                if (!secondField) {
                    secondField = av_frame_alloc();
                    if (!secondField) return fail("Out of memory");
                    secondField->format = o.vframe->format;
                    secondField->width = W;
                    secondField->height = H;
                    if (av_frame_get_buffer(secondField, 0) < 0) return fail("Out of memory");
                }
                sws_scale(o.sws, secondData, srcStride, 0, img.height, secondField->data, secondField->linesize);
                const AVPixFmtDescriptor* fd = av_pix_fmt_desc_get(AVPixelFormat(o.vframe->format));
                for (int pl = 0; pl < 4 && o.vframe->data[pl]; ++pl) {
                    const int bytes = av_image_get_linesize(AVPixelFormat(o.vframe->format), W, pl);
                    const int rows = (pl == 1 || pl == 2) && fd ? AV_CEIL_RSHIFT(H, fd->log2_chroma_h) : H;
                    for (int y = 1; y < rows; y += 2)
                        std::memcpy(o.vframe->data[pl] + size_t(y) * size_t(o.vframe->linesize[pl]),
                                    secondField->data[pl] + size_t(y) * size_t(secondField->linesize[pl]), size_t(bytes));
                }
            }
            o.vframe->pts = f - din;
            if (interlacedOut) o.vframe->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
            if (!attach608(f - din)) return fail("Out of memory");
            if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("Video encoding failed: " + averr(rc));
            if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing video failed: " + videoError(o, rc));
            }
        }
        if (wantAudio) {
            int64_t target = audioUntil(f) + limiterDelay;
            int n = int(target - audioCursor);
            if (n > 0) {
                mixBuf.resize(size_t(n) * size_t(nch));
                mixMain(audioCursor, n, mixBuf.data());
                if (limiter) {
                    for (float& v : mixBuf) v = float(v * normGain);
                    limiter->process(mixBuf.data(), mixBuf.data(), n);
                }
                audioCursor = target;
                if (reportMeter) {
                    reportMeter->addChannels(mixBuf.data(), n, nch, weights.data());
                    reportLeqm->add(mixBuf.data(), n);
                }
                if (monos.empty()) {
                    fifo.insert(fifo.end(), mixBuf.begin(), mixBuf.end());
                    if (!encodeAudio(false)) return fail("Audio encoding failed");
                } else {
                    // The mix's channels to their tracks; the tracks after every stream's get silence.
                    const size_t used = size_t(nch) * (1 + extras.size());
                    for (size_t k = 0; k < monos.size(); ++k) {
                        std::vector<float>& q = monos[k]->fifo;
                        if (k < size_t(nch))
                            for (int i = 0; i < n; ++i) q.push_back(mixBuf[size_t(i) * size_t(nch) + k]);
                        else if (k >= used) q.insert(q.end(), size_t(n), 0.0f);
                    }
                }
            }
            // The other streams, sample for sample with the mix (stems keep their levels; the described stream is
            // normalised and limited as the mix is).
            for (auto& e : extras) {
                const int64_t until = audioUntil(f) + e->delay;
                const int m = int(until - e->cursor);
                if (m <= 0) continue;
                mixBuf.resize(size_t(m) * size_t(nch));
                mixExtra(*e, e->cursor, m, mixBuf.data());
                if (e->limiter) {
                    for (float& v : mixBuf) v = float(v * e->gain);
                    e->limiter->process(mixBuf.data(), mixBuf.data(), m);
                }
                e->cursor = until;
                if (!e->ctx) {
                    const size_t first = size_t(nch) * size_t(1 + (&e - extras.data()));
                    for (size_t c = 0; c < size_t(nch); ++c)
                        for (int i = 0; i < m; ++i) monos[first + c]->fifo.push_back(mixBuf[size_t(i) * size_t(nch) + c]);
                    continue;
                }
                e->fifo.insert(e->fifo.end(), mixBuf.begin(), mixBuf.end());
                if (!encodeFifo(e->ctx, e->st, e->frame, e->fifo, e->pts, false)) return fail("Audio encoding failed");
            }
            for (auto& mo : monos)
                if (!encodeFifo(mo->ctx, mo->st, mo->frame, mo->fifo, mo->pts, false)) return fail("Audio encoding failed");
        }
        if (progress && ((f - din) % 5 == 0 || f + 1 == dout))
            progress(renderFrom + (1 - renderFrom) * double(f - din + 1) / double(total),
                     conv.active() ? in + FrameTime(double(f - din) * double(out - in) / double(total)) : f);
    }
    if (!writeCaptions(dout, true)) return fail("Writing captions failed");
    // Flush the encoders; errors here (e.g. a full disk) must fail the export.
    if (wantAudio) {
        if (o.actx) {
            if (!encodeAudio(true)) return fail("Audio encoding failed");
            if ((rc = avcodec_send_frame(o.actx, nullptr)) < 0 || (rc = drain(o, o.actx, o.ast)) < 0)
                return fail("Finishing audio failed: " + averr(rc));
        }
        for (auto& mo : monos) {
            if (!encodeFifo(mo->ctx, mo->st, mo->frame, mo->fifo, mo->pts, true)) return fail("Audio encoding failed");
            if ((rc = avcodec_send_frame(mo->ctx, nullptr)) < 0 || (rc = drain(o, mo->ctx, mo->st)) < 0)
                return fail("Finishing audio failed: " + averr(rc));
        }
        for (auto& e : extras) {
            if (!e->ctx) continue;
            if (!encodeFifo(e->ctx, e->st, e->frame, e->fifo, e->pts, true)) return fail("Audio encoding failed");
            if ((rc = avcodec_send_frame(e->ctx, nullptr)) < 0 || (rc = drain(o, e->ctx, e->st)) < 0)
                return fail("Finishing audio failed: " + averr(rc));
        }
    }
    if (wantVideo) {
        if ((rc = avcodec_send_frame(o.vctx, nullptr)) < 0 || (rc = drain(o, o.vctx, o.vst)) < 0)
            return fail("Finishing video failed: " + videoError(o, rc));
    }
    // HDR10 light levels as measured (MP4 and MOV write their clli box with the index, at the end).
    if (pq && measured.frames > 0 && o.vst && !(smart && smart->copied() > 0))  // copied frames go unmeasured
        if (const AVPacketSideData* sd = av_packet_side_data_get(o.vst->codecpar->coded_side_data, o.vst->codecpar->nb_coded_side_data,
                                                                 AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
            auto* cl = reinterpret_cast<AVContentLightMetadata*>(sd->data);
            hdr10LightLevels(measured, cl->MaxCLL, cl->MaxFALL);
        }
    if ((rc = av_write_trailer(o.oc)) < 0) return fail("Cannot finalise file: " + averr(rc));
    if (o.oc->pb && o.oc->pb->error < 0) return fail("Writing the file failed: " + averr(o.oc->pb->error));
    if (o.spatial) {
        // The second view's configuration and Apple's stereo boxes, which FFmpeg does not write.
        if ((rc = avio_closep(&o.oc->pb)) < 0) return fail("Writing the file failed: " + averr(rc));
        std::string e;
        if (!writeSpatialVideoBoxes(s.path, o.spatial->lhvcRecord(), s.spatialBaseline, s.spatialFieldOfView, &e)) return fail(e);
    }
    // The HDR10+ metadata beside the file too (named after it, an image sequence's frame number left out). The video
    // is done by now: a JSON that cannot be written is warned about, never a reason to lose the export.
    if (!hdr10.empty()) {
        const std::string stem = exportSidecarStem(s.path);
        std::string e;
        if (!writeHdr10PlusJson(stem + ".hdr10plus.json", hdr10, din, dout, &e)) qWarning("Dynamic HDR metadata JSON not written beside %s: %s", s.path.c_str(), e.c_str());
    }
    // The loudness report beside the file (like the JSON: not written is warned about, the export kept).
    if (reportMeter) {
        // The dialogue as the normalisation found it, else as the sequence marks it (no listening). A speed change
        // delivers the sound faster: its marks go to delivered time and the readings back to timeline time.
        SpeechGate gate = normalisedGate;
        const double fps = seq.fpsValue(), start = double(in) / fps;
        std::vector<std::pair<double, double>> speech =
            normalisedByDialogue ? normalisedSpeech : timelineSpeech(p, seq, gate, s.audioTracks, start, double(out) / fps);
        const double speed = conv.mode == RateConversion::Mode::Speed ? conv.speed() : 1.0;
        for (auto& [a, b] : speech) a = start + (a - start) / speed, b = start + (b - start) / speed;
        DialogueLoudness d = loudnessOfMeter(*reportMeter, reportLeqm.get(), start, mixRate, speech, gate);
        if (speed != 1.0) {
            for (LoudnessLogPoint& pt : d.log) pt.t = start + (pt.t - start) * speed;
            for (auto& [a, b] : d.speech) a = start + (a - start) * speed, b = start + (b - start) * speed;
        }
        LoudnessReportSpec spec;
        spec.title = seq.name;
        if (s.loudnessTarget < 0) {
            spec.target = s.loudnessTarget;
            spec.dialogueGated = s.dialogueGated;
            spec.tolerance = s.dialogueGated ? 2 : 1;
            spec.peakCeiling = s.peakCeiling;
        }
        spec.leqmLimit = s.leqmLimit;
        const std::string base = loudnessReportBase(s.path);
        std::string e;
        if (!writeLoudnessCsv(base + ".csv", d, spec, seq, &e) || !writeLoudnessPdf(base + ".pdf", d, spec, seq, &e))
            qWarning("Loudness report not written beside %s: %s", s.path.c_str(), e.c_str());
    }
    if (smartRendered) *smartRendered = smart ? smart->copied() : 0;
    if (light) *light = measured;
    return true;
}

}  // namespace

std::string exportSidecarStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
    std::string stem = path.substr(0, dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : path.size());
    if (const size_t pct = stem.find('%'); pct != std::string::npos && (slash == std::string::npos || pct > slash)) {
        size_t cut = pct;
        if (cut > 0 && (stem[cut - 1] == '_' || stem[cut - 1] == '.' || stem[cut - 1] == '-')) --cut;
        stem.erase(cut);
    }
    return stem;
}

std::string loudnessReportBase(const std::string& path) { return exportSidecarStem(path) + ".loudness"; }

std::string hdr10PlusCarriage(const Sequence& seq, const ExportSettings& s) {
    const std::string& c = s.videoCodec;
    if (c.empty() || c == "none" || c == "gif" || c == "exr") return {};
    const ColorSpace* chosen = findTrimTarget(seq, s.trimTarget) ? &trimTargetSpace(seq, s.trimTarget) : deliverySpaceAsked(seq, s);
    const ColorSpace& out = chosen && !chosen->sceneReferred ? *chosen : sequenceColorSpace(seq);
    if (out.transfer != Transfer::Pq) return {};
    const bool video = c == "libx265" || c == "hw_hevc" || c.rfind("hevc_", 0) == 0 || c == "libsvtav1" || c == "libaom-av1" ||
                       c == "librav1e" || c.rfind("av1_", 0) == 0;
    return video ? "video" : "json";
}

bool adaptiveHdrCarried(const Sequence& seq, const ExportSettings& s) {
    const std::string& c = s.videoCodec;
    if (!s.adaptiveHdr || c.empty() || c == "none" || c == "gif" || c == "exr") return false;
    const ColorSpace* chosen = findTrimTarget(seq, s.trimTarget) ? &trimTargetSpace(seq, s.trimTarget) : deliverySpaceAsked(seq, s);
    const ColorSpace& out = chosen && !chosen->sceneReferred ? *chosen : sequenceColorSpace(seq);
    return out.hdr() && (c == "libx265" || c == "hw_hevc" || c.rfind("hevc_", 0) == 0 || c == "libsvtav1" || c == "libaom-av1" ||
                         c == "librav1e" || c.rfind("av1_", 0) == 0);
}

bool exportCodecCarries(const std::string& codecName, const std::string& layout) {
    AVChannelLayout want{};
    if (!ffmpegLayout(layout, want)) return false;
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName.c_str());
    if (!codec) return true;  // the export itself says the encoder is missing
    // Encoders list some layouts and check others only when they open (FFmpeg's AAC takes 7.1 but not 5.1.2), so the
    // answer is an encoder opened with the layout, once per codec and layout.
    static std::mutex mutex;
    static std::map<std::string, bool> known;
    const std::string key = codecName + '/' + layout;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = known.find(key); it != known.end()) return it->second;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    av_channel_layout_copy(&ctx->ch_layout, &want);
    ctx->sample_rate = 48000;
    ctx->time_base = AVRational{1, 48000};
    const AVSampleFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* cfg = nullptr;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, nullptr) >= 0)
        fmts = static_cast<const AVSampleFormat*>(cfg);
#else
    fmts = codec->sample_fmts;
#endif
    ctx->sample_fmt = fmts ? fmts[0] : AV_SAMPLE_FMT_FLTP;
    const bool ok = avcodec_open2(ctx, codec, nullptr) >= 0;
    avcodec_free_context(&ctx);
    std::lock_guard<std::mutex> lock(mutex);
    known[key] = ok;
    return ok;
}

std::string exportAudioLayout(const std::string& layout, const std::string& codec) {
    if (codec.empty() || codec == "none") return layout;
    // An ambisonic field goes into PCM, or its first order into AAC (as YouTube takes it: AAC has no layouts for 9 or
    // 16 channels); anything else hears it as stereo.
    if (ambisonicLayout(layout)) {
        if (codec.rfind("pcm_", 0) == 0) return layout;
        return codec == "aac" || codec == "libfdk_aac" || codec == "aac_at" ? "ambix" : "stereo";
    }
    if (exportCodecCarries(codec, layout)) return layout;
    const std::string ear = earLevelLayout(layout);
    if (ear != layout && exportCodecCarries(codec, ear)) return ear;
    if (layoutChannels(layout) > 6 && exportCodecCarries(codec, "5.1")) return "5.1";
    return "stereo";
}

bool containerCarriesStreams(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* e : {".mp4", ".m4v", ".m4a", ".mov", ".mkv", ".mka", ".mxf", ".webm", ".ts", ".m2ts"})
        if (ext == e) return true;
    return false;
}

bool exportCarriesCredentials(const ExportSettings& s) {
    // One file of a kind that holds them (not numbered frames, nor a GIF).
    static const std::set<std::string> frames{"png", "tiff", "exr", "dpx", "gif"};
    return contentCredentialsFormat(s.path) && !frames.count(s.videoCodec);
}

namespace {

std::string lowered(std::string t) {
    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

// The ancillary packets, AS-11 metadata and parts of a broadcast MXF delivery for the sequence's range.
bool broadcastMxfOptions(const Sequence& seq, const ExportSettings& s, FrameTime in, FrameTime out, BroadcastMxfOptions& o,
                         std::string* error) {
    o.shim = s.mxfDelivery;
    o.startTimecode = s.startTimecode;
    o.clipName = seq.name;
    const int64_t frames = out - in;
    const CaptionTrack* track = captionTrackFor(seq, s.captionTrack);
    std::vector<Caption> part;
    if (track)
        for (Caption c : track->captions) {
            if (c.end <= in || c.start >= out) continue;
            c.start = std::max(c.start, in) - in;
            c.end = std::min(c.end, out) - in;
            part.push_back(std::move(c));
        }
    const bool interlaced = s.interlaced || s.mxfDelivery == "as11-dpp";
    if ((s.ancCaptions || s.ancTeletext) && frames > 0) {
        o.anc.assign(size_t(frames), {});
        if (s.ancCaptions) {
            const auto cc = captionCcData(part, seq.fps, frames, true, true, track ? track->style : CaptionStyle{});
            for (int64_t f = 0; f < frames; ++f) o.anc[size_t(f)].push_back({9, 0x61, 0x01, captionCdp(cc[size_t(f)], seq.fps, uint16_t(f))});
        }
        if (s.ancTeletext) {
            const auto fields = teletextSubtitleFields(part, seq.fps, frames, s.teletextPage, interlaced);
            uint16_t sequence = 0;
            const int perFrame = interlaced ? 2 : 1;
            for (int64_t f = 0; f < frames; ++f)
                for (int k = 0; k < perFrame; ++k)
                    o.anc[size_t(f)].push_back({uint16_t(k ? 575 : 12), 0x43, 0x02,
                                                op47Sdp(fields[size_t(f * perFrame + k)], sequence++, k == 0)});
        }
    }
    if (s.mxfDelivery.rfind("as11", 0) != 0) return true;
    // The metadata given, by framework.
    std::set<std::string> core(as11CoreProperties().begin(), as11CoreProperties().end());
    std::set<std::string> dpp(ukDppProperties().begin(), ukDppProperties().end());
    for (const std::string& line : s.as11Metadata) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) eq = line.find(':');
        auto trim = [](std::string t) {
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
            return t;
        };
        if (trim(line).empty()) continue;
        if (eq == std::string::npos) {
            if (error) *error = "AS-11 metadata is Name=value: " + line;
            return false;
        }
        std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (name.rfind("AS11", 0) == 0) name.erase(0, 4);
        else if (name.rfind("UKDPP", 0) == 0) name.erase(0, 5);
        if (core.count(name)) o.core[name] = value;
        else if (dpp.count(name)) o.dpp[name] = value;
        else {
            if (error) *error = "Not an AS-11 or UK DPP property: " + name;
            return false;
        }
    }
    auto def = [](std::map<std::string, std::string>& m, const char* k, const std::string& v) {
        if (!m.count(k)) m[k] = v;
    };
    def(o.core, "ProgrammeTitle", seq.name);
    std::string language = s.audioLanguage;
    if (language.empty() && !seq.captionTracks.empty()) language = seq.captionTracks.front().language;
    const char* lang3 = iso639_2(language);
    def(o.core, "PrimaryAudioLanguage", lang3 ? lang3 : "und");
    const bool captioned = track && (s.ancCaptions || s.ancTeletext);
    def(o.core, "ClosedCaptionsPresent", captioned ? "true" : "false");
    if (captioned) {
        def(o.core, "ClosedCaptionsType", "0");  // hard of hearing
        const char* c3 = iso639_2(track->language);
        if (c3) def(o.core, "ClosedCaptionsLanguage", c3);
    }
    // Markers for the DPP's line-up and clock, and the parts.
    std::vector<MxfPart> parts;
    int64_t lineUp = -1, identClock = -1;
    for (const Marker& m : seq.markers) {
        if (m.t < in || m.t >= out) continue;
        const std::string n = lowered(m.name);
        auto starts = [&](const char* w) { return n.rfind(w, 0) == 0; };
        if (m.duration > 0 && starts("part")) {
            const int64_t end = std::min<int64_t>(m.t + m.duration, out);
            parts.push_back({m.t - in, end - m.t, 0, 0});
        } else if (starts("line-up") || starts("line up") || starts("lineup") || starts("bars")) {
            lineUp = m.t - in;
        } else if (starts("clock") || starts("ident")) {
            identClock = m.t - in;
        }
    }
    std::sort(parts.begin(), parts.end(), [](const MxfPart& a, const MxfPart& b) { return a.start < b.start; });
    for (size_t i = 0; i < parts.size(); ++i) {
        parts[i].number = int(i + 1);
        parts[i].total = int(parts.size());
        if (i + 1 < parts.size() && parts[i].start + parts[i].duration > parts[i + 1].start) {
            if (error) *error = "The part markers overlap";
            return false;
        }
    }
    o.parts = parts;
    if (s.mxfDelivery == "as11-dpp" || !o.dpp.empty()) {
        if (lineUp >= 0) def(o.dpp, "LineUpStart", std::to_string(lineUp));
        if (identClock >= 0) def(o.dpp, "IdentClockStart", std::to_string(identClock));
        def(o.dpp, "AudioLoudnessStandard", std::fabs(s.loudnessTarget + 23) < 0.05 ? "1" : "0");  // EBU R 128
        def(o.dpp, "AudioDescriptionPresent", s.describedStream && hasDescriptionClips(seq) ? "true" : "false");
        def(o.dpp, "OpenCaptionsPresent", s.burnInCaptions && track ? "true" : "false");
        if (s.burnInCaptions && track)
            if (const char* c3 = iso639_2(track->language)) def(o.dpp, "OpenCaptionsLanguage", c3);
        def(o.dpp, "PictureRatio", "16/9");
    }
    const std::vector<std::string> missing = missingAs11(o);
    if (!missing.empty()) {
        std::string list;
        for (const std::string& m : missing) list += (list.empty() ? "" : ", ") + m;
        if (error) *error = "The UK DPP's metadata needs " + list;
        return false;
    }
    return true;
}

// A broadcast MXF delivery: rendered by FFmpeg beside the file, then rewrapped into it.
bool exportBroadcastMxf(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                        const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed, int* smartRendered, LightLevels* light) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string& d = s.mxfDelivery;
    if (d != "rdd9" && d != "as11-dpp" && d != "as11-x2" && d != "as11-x8") return fail("Unknown broadcast MXF delivery \"" + d + "\"");
    if (!broadcastMxfAvailable()) return fail("This build of Montage cannot write broadcast MXF (it has no bmx)");
    if (lowered(std::filesystem::path(s.path).extension().string()) != ".mxf") return fail("Broadcast MXF deliveries are .mxf files");
    const bool mpeg2 = d == "rdd9" || d == "as11-x8";
    if (mpeg2 && s.videoCodec != "mpeg2video") return fail("RDD 9 and AS-11 X8 carry MPEG-2 HD 4:2:2 video");
    if (!mpeg2 && !(s.videoCodec == "libx264" && s.profile == "avci100"))
        return fail("AS-11 UK DPP HD and X2 carry AVC-Intra 100 video");
    // (delivered at 25: the sequence's rate, or sped or converted to it)
    if (d == "as11-dpp" && !((s.rateChange.empty() ? seq.fps : s.rate) == Rational{25, 1} && s.rateChange != "pulldown"))
        return fail("UK DPP HD is 25 frames a second (1080i25)");
    if (d == "as11-dpp" && ((s.width > 0 ? s.width : seq.width) != 1920 || (s.height > 0 ? s.height : seq.height) != 1080))
        return fail("UK DPP HD is 1920 by 1080 (1080i25)");
    if (s.audioCodec != "pcm_s24le" && s.audioCodec != "pcm_s16le") return fail("Broadcast MXF sound is PCM");
    if (s.videoCodec == "none" || s.audioCodec == "none") return fail("Broadcast MXF deliveries have picture and sound");
    if (mpeg2 && s.monoAudioTracks != 2 && s.monoAudioTracks != 4 && s.monoAudioTracks != 8)
        return fail("RDD 9 carries 2, 4 or 8 mono sound tracks");
    FrameTime in = s.in >= 0 ? s.in : 0;
    FrameTime out = s.out >= 0 ? s.out : seq.duration();
    if (seq.duration() > 0) out = std::min(out, seq.duration());
    // Delivered at another rate: the file's frames, captions, markers and parts are the delivered sequence's.
    RateConversion conv;
    if (std::string why; !planRateConversion(seq, s, conv, &why)) return fail(why);
    const Sequence delivered = conv.active() ? deliveredSequence(seq, conv, in, out) : seq;
    ExportSettings described = s;
    if (conv.fields) described.interlaced = true;
    const FrameTime din = conv.active() ? 0 : in, dout = conv.active() ? deliveredFrames(conv, out - in) : out;
    BroadcastMxfOptions o;
    if (!broadcastMxfOptions(delivered, described, din, dout, o, error)) return false;
    if (!checkAs11Metadata(o, error)) return false;  // before the render, not after it

    ExportSettings staged = s;
    staged.mxfDelivery.clear();
    staged.path = s.path + ".render.mxf";
    if (d == "as11-dpp") staged.interlaced = true;
    auto render = [&](double f, FrameTime t) {
        if (progress) progress(f * 0.97, t);
    };
    std::error_code ec;
    const std::filesystem::path stagedPath = std::filesystem::u8path(staged.path);
    const bool ok = exportSequence(p, seq, staged, render, cancel, error, encoderUsed, smartRendered, light);
    if (!ok) {
        std::filesystem::remove(stagedPath, ec);
        return false;
    }
    std::string why;
    const bool rewrapped = rewrapBroadcastMxf(staged.path, s.path, o, &why, [&](double f) {
        if (progress) progress(0.97 + 0.03 * f, in + FrameTime(std::lround(f * double(out - in))));  // (programme frames)
        return !(cancel && cancel->load());
    });
    std::filesystem::remove(stagedPath, ec);
    if (!rewrapped) return fail(why);
    if (progress) progress(1.0, out);
    return true;
}

}  // namespace

bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed, int* smartRendered,
                    LightLevels* light, bool* keptUnsigned) {
    if (!s.mxfDelivery.empty()) {
        if (keptUnsigned) *keptUnsigned = false;
        if (!s.credentialCertificate.empty()) {
            if (error) *error = "Content Credentials cannot go in this kind of file";
            return false;
        }
        return exportBroadcastMxf(p, seq, s, progress, cancel, error, encoderUsed, smartRendered, light);
    }
    bool opened = false;
    if (keptUnsigned) *keptUnsigned = false;
    if (smartRendered) *smartRendered = 0;
    if (light) *light = LightLevels{};
    if (!s.credentialCertificate.empty()) {  // (refused before rendering rather than after)
        std::string why;
        if (!contentCredentialsAvailable()) why = "This build of Montage cannot sign with Content Credentials";
        else if (!exportCarriesCredentials(s)) why = "Content Credentials cannot go in this kind of file";
        else if (!checkCredentialSigner({s.credentialCertificate, s.credentialKey, {}}, &why)) why = "Cannot sign with Content Credentials: " + why;
        if (!why.empty()) {
            if (error) *error = why;
            return false;
        }
    }
    // Channels the codec cannot carry fold down (an immersive mix in AAC to its ear-level 7.1 or 5.1), as the export
    // dialog's summary says, rather than failing or turning into stereo. Down to stereo it is the stereo fold-down of the
    // whole mix. Mono tracks carry a channel each, whatever the codec.
    const std::string layout = s.downmixStereo || s.monoAudioTracks > 0 ? seq.audioLayout : exportAudioLayout(seq.audioLayout, s.audioCodec);
    // A described master: the mix without the descriptions, then the programme with them as a stream of its own.
    const bool described = s.describedStream && hasDescriptionClips(seq) && !s.audioCodec.empty() && s.audioCodec != "none" &&
                           containerCarriesStreams(s.path);
    bool ok;
    if (layout != seq.audioLayout || described) {
        Sequence work = seq;
        ExportSettings settings = s;
        if (layoutChannels(layout) <= 2 && layoutChannels(seq.audioLayout) > 2) settings.downmixStereo = true;
        else work.audioLayout = layout;
        if (described) {
            if (!edit::roleMuted(work, kDescriptionRole)) work.mutedRoles.push_back(kDescriptionRole);
            // Streams that would hear only the descriptions (their role, or tracks holding nothing else) are silent
            // in a mix without them: left out.
            std::erase_if(settings.extraAudio, [&](const ExportSettings::AudioStream& a) {
                if (a.role == kDescriptionRole) return true;
                if (!a.role.empty()) return false;
                bool other = false;
                for (size_t t = 0; t < work.audioTracks.size(); ++t)
                    if (a.tracks.empty() || (t < a.tracks.size() && a.tracks[t]))
                        for (const Clip& c : work.audioTracks[t].clips) other |= c.role != kDescriptionRole;
                return !other;
            });
            ExportSettings::AudioStream ad;
            ad.name = s.describedName.empty() ? std::string("Audio Description") : s.describedName;
            ad.language = s.audioLanguage;
            ad.unmute = {kDescriptionRole};
            ad.normalise = true;
            ad.descriptions = true;
            settings.extraAudio.push_back(std::move(ad));
            if (settings.audioName.empty()) settings.audioName = "Programme";
        }
        ok = exportImpl(p, work, settings, progress, cancel, error, opened, encoderUsed, smartRendered, light);
    } else {
        ok = exportImpl(p, seq, s, progress, cancel, error, opened, encoderUsed, smartRendered, light);
    }
    // An ambisonic mix in MP4 or MOV: the Spatial Audio box, so YouTube and players hear it as a field.
    if (ok && ambisonicLayout(layout) && !s.downmixStereo && !s.audioCodec.empty() && s.audioCodec != "none") {
        std::string ext = std::filesystem::path(s.path).extension().string();
        for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        // The export is good either way: without the box players hear four channels rather than a field, which is no
        // reason to throw the file away.
        std::string why;
        if ((ext == ".mp4" || ext == ".m4a" || ext == ".mov" || ext == ".m4v") && !writeSpatialAudioBox(s.path, ambisonicOrder(layout), &why))
            qWarning("Spatial audio metadata not added to %s: %s", s.path.c_str(), why.c_str());
    }
    // Never leave a truncated file behind (the output is closed by now), but
    // don't touch an existing file if we failed before writing to it.
    if (!ok && opened) std::remove(s.path.c_str());
    if (ok && !s.credentialCertificate.empty()) {
        const Provenance made = sequenceProvenance(p, seq, s.in, s.out);
        CredentialOptions o;
        o.allowAiTraining = s.credentialAllowAiTraining;
        o.sources = made.sources;
        std::string why;
        if (!signContentCredentials(s.path, made, {s.credentialCertificate, s.credentialKey, {}}, o, &why)) {
            // The render is kept (it may have taken hours), unsigned, and said to be: it can be signed again later.
            if (error) *error = "Could not sign it with Content Credentials (" + why + "); the export is kept, unsigned, at " + s.path;
            if (keptUnsigned) *keptUnsigned = true;
            return false;
        }
    }
    return ok;
}

std::vector<ExportSettings::AudioStream> stemStreams(const Sequence& seq, int groups) {
    std::vector<ExportSettings::AudioStream> out;
    const std::string language = seq.captionTracks.empty() ? std::string() : seq.captionTracks.front().language;
    const size_t n = seq.audioTracks.size();
    auto hasClips = [&](size_t i) { return !seq.audioTracks[i].muted && !seq.audioTracks[i].clips.empty(); };
    // A role heard: not muted, with an enabled clip on a track that plays ("No Role" for clips without one).
    auto heard = [&](const std::string& r) {
        if (edit::roleMuted(seq, r)) return false;
        for (size_t i = 0; i < n; ++i)
            if (hasClips(i))
                for (const Clip& c : seq.audioTracks[i].clips)
                    if (c.enabled && (c.role.empty() ? std::string("No Role") : c.role) == r) return true;
        return false;
    };
    if (groups == StemsDialogueAndME) {
        // (each left out when none of its roles is heard; the M&E names only those that are)
        if (heard("Dialogue")) out.push_back({"Dialogue", language, {}, "Dialogue"});
        std::string me;
        for (const char* r : {"Music", "Effects"})
            if (heard(r)) me += (me.empty() ? "" : "+") + std::string(r);
        if (!me.empty()) out.push_back({"M&E", language, {}, me});
    } else if (groups == StemsByRole) {
        Sequence named = seq;
        for (Track& t : named.audioTracks)
            for (Clip& c : t.clips)
                if (c.role.empty()) c.role = "No Role";
        for (const std::string& r : edit::sequenceRoles(named))
            if (heard(r)) out.push_back({r, language, {}, r});
    } else {
        for (size_t i = 0; i < n; ++i) {
            if (!hasClips(i)) continue;
            std::vector<bool> mask(n, false);
            mask[i] = true;
            out.push_back({seq.audioTracks[i].name.empty() ? "A" + std::to_string(i + 1) : seq.audioTracks[i].name, language, mask, {}});
        }
    }
    return out;
}

bool exportStems(const Project& p, const Sequence& seq, const ExportSettings& s, int grouping, std::vector<StemFile>* written,
                 const ExportProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    // The groups: each track, each bus's tracks (and the ones going straight to the master), or each role.
    std::vector<std::pair<std::string, std::vector<bool>>> groups;
    const size_t n = seq.audioTracks.size();
    auto hasClips = [&](size_t i) { return !seq.audioTracks[i].muted && !seq.audioTracks[i].clips.empty(); };
    // By role: every track plays, with every role but the stem's muted ("No Role" standing for clips without one).
    static const std::string kNoRole = "No Role";
    Sequence byRole;
    std::vector<std::string> roles;
    if (grouping == StemsByRole) {
        byRole = seq;
        for (Track& t : byRole.audioTracks)
            for (Clip& c : t.clips)
                if (c.role.empty()) c.role = kNoRole;
        for (const std::string& r : edit::sequenceRoles(byRole)) {
            if (edit::roleMuted(seq, r)) continue;
            bool used = false;
            for (size_t i = 0; i < n && !used; ++i)
                if (hasClips(i))
                    for (const Clip& c : byRole.audioTracks[i].clips) used = used || (c.enabled && c.role == r);
            if (!used) continue;
            roles.push_back(r);
            groups.push_back({r, {}});
        }
    } else if (grouping != StemsByBus) {
        for (size_t i = 0; i < n; ++i) {
            if (!hasClips(i)) continue;
            std::vector<bool> mask(n, false);
            mask[i] = true;
            const std::string name = seq.audioTracks[i].name.empty() ? "A" + std::to_string(i + 1) : seq.audioTracks[i].name;
            groups.push_back({name, mask});
        }
    } else {
        std::vector<bool> main(n, false);
        bool anyMain = false;
        for (size_t i = 0; i < n; ++i) {
            const bool routed = std::any_of(seq.buses.begin(), seq.buses.end(), [&](const Bus& b) { return b.id == seq.audioTracks[i].output; });
            if (!routed && hasClips(i)) main[i] = anyMain = true;
        }
        if (anyMain) groups.push_back({"Main", main});
        for (const Bus& b : seq.buses) {
            std::vector<bool> mask(n, false);
            bool any = false;
            for (size_t i = 0; i < n; ++i)
                if (seq.audioTracks[i].output == b.id && hasClips(i)) mask[i] = any = true;
            if (any && !b.muted) groups.push_back({b.name, mask});
        }
    }
    if (groups.empty()) {
        if (error) *error = "There is no audio to split into stems";
        return false;
    }
    const std::filesystem::path base(s.path);
    const std::string stem = base.stem().string();
    for (size_t g = 0; g < groups.size(); ++g) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        std::string safe = groups[g].first;
        for (char& ch : safe)
            if (std::string("/\\:*?\"<>|").find(ch) != std::string::npos) ch = '_';
        ExportSettings one = s;
        one.path = (base.parent_path() / (stem + " - " + safe + ".wav")).string();
        one.videoCodec = "none";
        one.audioCodec = "pcm_s24le";
        one.loudnessTarget = 0;  // stems keep their levels, so they add back up to the mix
        one.loudnessReport = false;  // (the delivery's report is the mix's)
        one.describedStream = false;  // one stream each
        one.extraAudio.clear();
        one.burnInCaptions = one.embedCaptions = false;
        one.burnIn = {};
        one.audioTracks = groups[g].second;
        const ExportProgress part = progress ? ExportProgress([&, g](double f, FrameTime t) { progress((double(g) + f) / double(groups.size()), t); })
                                             : ExportProgress();
        if (grouping == StemsByRole) {
            byRole.mutedRoles.clear();
            for (const std::string& r : edit::sequenceRoles(byRole))
                if (r != roles[g]) byRole.mutedRoles.push_back(r);
        }
        if (!exportSequence(p, grouping == StemsByRole ? byRole : seq, one, part, cancel, error)) return false;
        if (written) written->push_back({groups[g].first, one.path});
    }
    return true;
}

bool apvEncoderAvailable() { return avcodec_find_encoder_by_name("liboapv") != nullptr; }

bool apvDecoderAvailable() { return avcodec_find_decoder_by_name("apv") != nullptr; }

namespace {

// A node graph's clip alone on its track: the clips there its Track nodes follow are kept, on a muted track of their own
// at the end (out of sight, where the track inputs' numbers do not reach), for the nodes to read their movement.
void keepFollowedClips(Sequence& alone, const Track& real, const Clip& c) {
    Track kept;
    kept.kind = TrackKind::Video;
    kept.muted = true;
    for (const Clip& o : real.clips)
        if (o.id != c.id && std::any_of(c.composite.nodes.begin(), c.composite.nodes.end(), [&](const CompositeNode& n) { return n.follow == o.id; }))
            kept.clips.push_back(o);
    if (!kept.clips.empty()) alone.videoTracks.push_back(std::move(kept));
}

}  // namespace

bool renderClipVideo(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress, const std::atomic<bool>* cancel, bool apv) {
    const Clip* c = edit::clipById(seq, clip);
    if (!c) {
        if (error) *error = "No such clip";
        return false;
    }
    ExportSettings st;
    st.path = path;
    st.videoCodec = apv ? "liboapv" : "prores_ks";
    st.profile = "4444";  // with its transparency
    st.alpha = true;
    st.crf = 12;  // (APV's QP: near lossless, as ProRes 4444 is)
    st.audioCodec = "none";
    st.smartRender = false;
    const auto loc = edit::locate(seq, clip);
    if (!c->composite.empty() && loc && loc->track.kind == TrackKind::Video) {
        // A node graph's clip at the sequence's size over its own span, with the other tracks there (muted) for its
        // track inputs, and the clips it follows, so the render is the graph's picture as it plays.
        // Its movement stays (it is inside the graph's picture); Render and Replace leaves the rendered clip still. The
        // clips its Track nodes follow stay on its track, outside the span rendered.
        Sequence whole = seq;
        for (Track& t : whole.videoTracks) t.muted = true;
        Track& own = whole.videoTracks[size_t(loc->track.index)];
        own.muted = false;
        own.transitions.clear();
        std::erase_if(own.clips, [&](const Clip& o) {
            return o.id != c->id && std::none_of(c->composite.nodes.begin(), c->composite.nodes.end(), [&](const CompositeNode& n) { return n.follow == o.id; });
        });
        for (Clip& o : own.clips)
            if (o.id == c->id) o.linkGroup = 0, o.blendMode = "normal";
        whole.audioTracks.clear();
        whole.buses.clear();
        whole.captionTracks.clear();
        whole.markers.clear();
        st.in = c->start;
        st.out = c->end();
        return exportSequence(p, whole, st, progress, cancel, error);
    }
    // The clip alone, at its media's own size (a generator at the sequence's), its transform left live on the clip.
    Sequence alone = seq;
    const MediaItem* m = c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    if (m && m->width > 0 && m->height > 0) alone.width = m->width + (m->width & 1), alone.height = m->height + (m->height & 1);
    alone.videoTracks.assign(1, Track{});
    alone.videoTracks[0].kind = TrackKind::Video;
    alone.audioTracks.clear();
    alone.buses.clear();
    alone.captionTracks.clear();
    alone.markers.clear();
    Clip copy = *c;
    copy.start = 0;
    copy.linkGroup = 0;
    copy.blendMode = "normal";
    copy.motion = makeEffect("transform", c->motion.id);
    alone.videoTracks[0].clips.push_back(copy);
    st.in = 0;
    st.out = copy.duration;
    return exportSequence(p, alone, st, progress, cancel, error);
}

bool bakeCompositeClips(Project& p, Id seqId, const std::string& output, std::string* error, int* baked,
                        const std::atomic<bool>* cancel) {
    if (baked) *baked = 0;
    const Sequence* found = p.findSequence(seqId);
    if (!found) return true;
    std::vector<Id> clips;
    for (const Track& t : found->videoTracks)
        for (const Clip& c : t.clips)
            if (!c.composite.empty()) clips.push_back(c.id);
    if (clips.empty()) return true;
    const QFileInfo out(QString::fromStdString(output));
    const QString folder = out.absolutePath() + "/" + out.completeBaseName() + " Composites";
    if (!QDir().mkpath(folder)) {
        if (error) *error = "Cannot make " + folder.toStdString();
        return false;
    }
    for (Id id : clips) {
        const Sequence& seq = *p.findSequence(seqId);
        const auto loc = edit::locate(seq, id);
        const Clip* c = edit::clipById(seq, id);
        if (!loc || !c) continue;
        // The clip alone on its track, the other tracks there for its graph's track inputs but not shown.
        Sequence alone = seq;
        for (Track& t : alone.videoTracks) t.muted = true;
        Track& own = alone.videoTracks[size_t(loc->track.index)];
        own.muted = false;
        // With handles where a transition reaches past its ends, as the interchange file's dissolves need them.
        FrameTime head = 0, tail = 0;
        const Track& real = seq.videoTracks[size_t(loc->track.index)];
        for (const Transition& tr : real.transitions) {
            FrameTime a = 0, b = 0;
            if (!edit::transitionRange(real, tr, a, b)) continue;
            if (tr.clipB == id) head = std::max(head, c->start - a);
            if (tr.clipA == id) tail = std::max(tail, b - c->end());
        }
        head = std::clamp<FrameTime>(head, 0, c->start);
        own.transitions.clear();
        Clip copy = *c;
        copy.blendMode = "normal";
        copy.linkGroup = 0;
        if (!copy.reverse) copy.sourceIn -= double(head) * copy.speed;
        copy.start -= head;
        copy.duration += head + tail;
        edit::shiftKeyframes(copy, head);
        own.clips.assign(1, copy);
        keepFollowedClips(alone, real, *c);  // (after the last use of `own`: this can add a track)
        alone.audioTracks.clear();
        alone.buses.clear();
        alone.captionTracks.clear();
        alone.markers.clear();
        std::string name;
        for (char ch : c->name) name += std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == ' ' ? ch : '_';
        ExportSettings st;
        st.path = (folder + "/" + QString::fromStdString(name.empty() ? std::string("Clip") : name) + " " + QString::number(id) + ".mov").toStdString();
        st.videoCodec = "prores_ks";
        st.profile = "4444";  // with its transparency
        st.alpha = true;
        st.crf = 12;
        st.audioCodec = "none";
        st.smartRender = false;
        st.in = copy.start;
        st.out = copy.end();
        std::string err;
        if (!exportSequence(p, alone, st, {}, cancel, &err)) {
            if (error) *error = "Cannot render the composite of \"" + c->name + "\": " + err;
            return false;
        }
        MediaItem m;
        m.id = p.newId();
        if (!probeMedia(st.path, m, &err)) {
            if (error) *error = err;
            return false;
        }
        p.media.push_back(m);
        // (looked up again: the media list grew, the sequence did not move)
        // A plain clip of the file in its place: everything else it did (effects, grade, colour group, animations, its
        // place in the frame) is in the picture.
        Clip* k = edit::clipById(*p.findSequence(seqId), id);
        Clip plain;
        plain.id = k->id;
        plain.name = k->name;
        plain.start = k->start;
        plain.duration = k->duration;
        plain.enabled = k->enabled;
        plain.blendMode = k->blendMode;
        plain.linkGroup = k->linkGroup;
        plain.colorLabel = k->colorLabel;
        plain.mediaId = m.id;
        plain.sourceIn = double(head);
        plain.motion = makeEffect("transform", k->motion.id);
        *k = std::move(plain);
        if (baked) ++*baked;
    }
    return true;
}

bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress, const std::atomic<bool>* cancel) {
    const Clip* c = edit::clipById(seq, clip);
    if (!c) {
        if (error) *error = "No such clip";
        return false;
    }
    // The clip alone where it is, on a plain track of its own; the other audio tracks stay (muted, with their
    // inserts) so a compressor or gate on the clip still hears the track it is keyed by.
    Sequence alone = seq;
    alone.videoTracks.assign(1, Track{});
    alone.videoTracks[0].kind = TrackKind::Video;
    const auto loc = edit::locate(seq, clip);
    Track own;
    own.kind = TrackKind::Audio;
    own.id = loc && loc->track.kind == TrackKind::Audio ? seq.audioTracks[size_t(loc->track.index)].id : 0;
    for (Track& t : alone.audioTracks) {
        t.muted = true;
        t.solo = false;
        t.output = 0;
    }
    Clip copy = *c;
    copy.linkGroup = 0;
    copy.audio.params.clear();  // volume and pan stay live on the clip
    own.clips.push_back(copy);
    bool placed = false;
    for (Track& t : alone.audioTracks)
        if (own.id && t.id == own.id) t = own, placed = true;
    if (!placed) alone.audioTracks.push_back(own);
    alone.buses.clear();
    alone.masterEffects.clear();
    alone.masterVolumeDb = 0;
    alone.captionTracks.clear();
    ExportSettings st;
    st.path = path;
    st.videoCodec = "none";
    st.audioCodec = "pcm_s24le";
    st.in = copy.start;
    st.out = copy.start + copy.duration;
    return exportSequence(p, alone, st, progress, cancel, error);
}

bool stillRendition(const Sequence& seq, const std::string& rendition, RenderOptions& ro, const ColorSpace*& space, double& peak,
                    std::string* error) {
    const ColorSpace& master = sequenceColorSpace(seq);
    space = &master;
    peak = seq.hdrPeakNits;
    if (rendition == "master") return true;
    if (!rendition.empty()) {
        if (!findTrimTarget(seq, rendition)) {
            if (error) *error = "The sequence has no trim target " + rendition;
            return false;
        }
        ro.trimTarget = rendition;
        space = &trimTargetSpace(seq, rendition);
        peak = trimTargetPeak(seq, rendition);
        return true;
    }
    if (!master.hdr()) return true;
    // SDR screens: the grade's own SDR trim when it has one.
    for (const TrimTarget& tt : seq.trimTargets)
        if (const ColorSpace* c = findColorSpace(tt.colorSpace); c && !c->hdr() && !c->sceneReferred) {
            ro.trimTarget = tt.id;
            space = c;
            peak = trimTargetPeak(seq, tt.id);
            return true;
        }
    ro.displaySpace = rec709Space().id;
    space = &rec709Space();
    peak = 100;
    return true;
}

bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error,
                 const StillOptions& options, StillResult* result) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    RenderOptions ro;
    ro.highQuality = true;
    const ColorSpace* space = nullptr;
    double peak = 0;
    if (!stillRendition(seq, options.rendition, ro, space, peak, error)) return false;
    const std::string ext = lowered(std::filesystem::path(path).extension().string());
    const bool avif = ext == ".avif";
    const bool jpeg = ext == ".jpg" || ext == ".jpeg";
    const bool heif = ext == ".heic" || ext == ".heif" || ext == ".hif";
    if (space->hdr() && ext != ".png" && !avif && !heif && !jpeg)
        return fail("An HDR still is written as PNG, AVIF, HEIF or JPEG (with a gain map)");
    Image img = renderProgramFrame(p, seq, t, ro);
    if (img.empty()) return fail("Nothing to render at this frame");
    StillResult res;
    res.colorSpace = space->id;
    const bool gainMap = space->hdr() && (jpeg || ((avif || heif) && options.gainMap));
    if (gainMap) {
        res.bits = 8;
        res.gainMap = true;
    }
    if (space->hdr() && !jpeg) {  // (a JPEG states no light levels)
        if (!gainMap) res.bits = avif || heif ? 10 : 16;
        LightLevels l;
        if (ro.trimTarget.empty() && seq.hdrMaxCll > 0) {
            l.maxCll = seq.hdrMaxCll;
            l.maxFall = seq.hdrMaxFall;
        } else if (LightMeter meter(*space); meter.valid()) {
            meter.measure(img, l.maxCll, l.maxFall);
        }
        unsigned cll = 0, fall = 0;
        hdr10LightLevels(l, cll, fall);
        res.maxCll = cll, res.maxFall = fall;
    }
    if (gainMap) {
        // The SDR rendition as the base (the SDR trim, else the viewer's tone mapping), the map to this one.
        RenderOptions sro;
        sro.highQuality = true;
        const ColorSpace* sdrSpace = nullptr;
        double sdrPeak = 0;
        if (!stillRendition(seq, "", sro, sdrSpace, sdrPeak, error)) return false;
        if (sdrSpace->hdr()) sro.displaySpace = rec709Space().id, sdrSpace = &rec709Space();
        Image sdr = renderProgramFrame(p, seq, t, sro);
        // A gain map's base says Rec.709 or Display P3: a trim to other primaries goes into Rec.709.
        if (sdrSpace->primaries != Primaries::Bt709 && sdrSpace->primaries != Primaries::P3D65) {
            convertColor(sdr, *sdrSpace, rec709Space(), sdrPeak);
            sdrSpace = &rec709Space();
        }
        if (jpeg   ? !writeGainMapJpeg(sdr, *sdrSpace, img, *space, options.quality, path, error)
            : avif ? !writeGainMapAvif(sdr, *sdrSpace, img, *space, options.quality, res.maxCll, res.maxFall, path, error)
                   : !writeGainMapHeif(sdr, *sdrSpace, img, *space, options.quality, res.maxCll, res.maxFall, path, error))
            return false;
    } else if (avif) {
        bool stated = false;
        if (!writeAvifStill(img, *space, options.quality, peak, res.maxCll, res.maxFall, path, error, &stated)) return false;
        if (!stated) res.maxCll = res.maxFall = 0;
    } else if (heif) {
        if (!writeHeifStill(img, *space, options.quality, peak, res.maxCll, res.maxFall, path, error)) return false;
    } else if (space->hdr()) {
        if (!writeHdrPng(img, *space, peak, res.maxCll, res.maxFall, path, error)) return false;
    } else {
        std::vector<uint8_t> rgba = toRgba8(img);
        QImage qi(rgba.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888);
        const bool lossy = ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
        if (!qi.save(QString::fromStdString(path), nullptr, lossy ? std::clamp(options.quality, 0, 100) : -1))
            return fail("Cannot write " + path);
    }
    if (result) *result = res;
    return true;
}

}  // namespace montage
