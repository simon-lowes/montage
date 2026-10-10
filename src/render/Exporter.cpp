#include "Exporter.h"
#include "core/AudioDescription.h"

#include "core/ColorGroups.h"
#include "core/ClipAnimation.h"
#include "core/Interpretation.h"
#include "core/Chapters.h"
#include "core/Surround.h"

#include <QImage>
#include <QString>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "Processing.h"
#include "media/HwAccel.h"
#include "media/Loudness.h"
#include "media/SuperScale.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/spherical.h>
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
    if (c == "cfhd") return s.alpha ? AV_PIX_FMT_GBRAP12LE : AV_PIX_FMT_YUV422P10LE;
    if (c == "v210") return AV_PIX_FMT_YUV422P10LE;
    if (c == "ffv1") return s.alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
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
        v.push_back(preset("Apple ProRes 422", "mov", "Standard ProRes, 10-bit 4:2:2", "prores_ks", "pcm_s24le", 0, "", "standard"));
        v.push_back(preset("Apple ProRes 422 LT", "mov", "Lighter ProRes for offline / proxies", "prores_ks", "pcm_s16le", 0, "", "lt"));
        v.push_back(preset("Apple ProRes 422 Proxy", "mov", "The smallest ProRes, for offline editing", "prores_ks", "pcm_s16le", 0, "", "proxy"));
        {
            ExportPreset p = preset("Apple ProRes 4444 (alpha)", "mov", "Keeps transparency for graphics", "prores_ks", "pcm_s24le", 0, "", "4444");
            p.settings.alpha = true;
            v.push_back(p);
            ExportPreset xq = preset("Apple ProRes 4444 XQ (alpha)", "mov", "The highest-quality ProRes, with transparency", "prores_ks",
                                     "pcm_s24le", 0, "", "4444xq");
            xq.settings.alpha = true;
            v.push_back(xq);
        }
        v.push_back(preset("Avid DNxHR HQ", "mov", "Avid-friendly 8-bit 4:2:2 intermediate", "dnxhd", "pcm_s24le", 0, "", "dnxhr_hq"));
        v.push_back(preset("Avid DNxHR SQ", "mov", "Avid 8-bit 4:2:2, standard quality", "dnxhd", "pcm_s24le", 0, "", "dnxhr_sq"));
        v.push_back(preset("Avid DNxHR LB", "mov", "Avid 8-bit 4:2:2, low bandwidth for offline", "dnxhd", "pcm_s16le", 0, "", "dnxhr_lb"));
        v.push_back(preset("Avid DNxHR HQX (10-bit)", "mov", "Avid 10-bit 4:2:2 for finishing and HDR", "dnxhd", "pcm_s24le", 0, "", "dnxhr_hqx"));
        v.push_back(preset("Avid DNxHR 444 (10-bit)", "mov", "Avid 10-bit 4:4:4 for finishing and keying", "dnxhd", "pcm_s24le", 0, "", "dnxhr_444"));
        {
            ExportPreset cf = preset("GoPro CineForm", "mov", "Wavelet intermediate, 10-bit 4:2:2 (12-bit RGB with alpha)", "cfhd", "pcm_s24le", 0, "");
            v.push_back(cf);
            cf.name = "GoPro CineForm (alpha)";
            cf.description = "CineForm 12-bit RGBA, keeping transparency";
            cf.settings.alpha = true;
            v.push_back(cf);
        }
        v.push_back(preset("FFV1 (lossless archive)", "mkv", "Mathematically lossless 10-bit 4:2:2 with FLAC sound, for archiving", "ffv1", "flac", 0,
                           "", "", "yuv422p10le"));
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
            ExportPreset dnx = broadcast(preset("Avid DNxHR HQ (MXF)", "mxf", "DNxHR HQ in MXF OP1a, each channel a mono track (Avid's layout)", "dnxhd",
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
    }();
    return presets;
}

const ExportPreset* findExportPreset(const std::string& name) {
    for (const auto& p : exportPresets())
        if (p.name == name) return &p;
    return nullptr;
}

namespace {

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
    Image img = renderProgramFrame(p, seq, f, ro);
    if (img.width != W || img.height != H) img = resizeImage(img, W, H);
    convertColor(img, sequenceColorSpace(seq), outSpace, std::clamp(seq.hdrPeakNits, 100.0, 10000.0));
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
    const ColorSpace* chosen = findColorSpace(s.colorSpace);
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
        const ColorGroup* group = found ? colorGroupOf(seq_, *found) : nullptr;
        if (!found || found->isGenerator() || !found->effects.empty() || (group && (!group->pre.empty() || !group->post.empty())) ||
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

    if (s.videoCodec == "gif") return exportGif(p, seq, s, in, out, progress, cancel, error, opened);
    // Image sequences: numbered files, no sound.
    const bool imageSequence = s.videoCodec == "png" || s.videoCodec == "tiff" || s.videoCodec == "exr" || s.videoCodec == "dpx";
    const bool floatFrames = s.videoCodec == "exr";  // written straight from the float picture, not through 16 bits
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
    int rc = avformat_alloc_output_context2(&o.oc, nullptr, imageSequence ? "image2" : nullptr, outPath.c_str());
    if (rc < 0 || !o.oc) return fail("Unknown output format for " + s.path);
    if (imageSequence) av_opt_set_int(o.oc->priv_data, "start_number", s.startNumber, 0);
    o.pkt = av_packet_alloc();
    const AVRational fps{seq.fps.num, seq.fps.den};
    int W = s.width > 0 ? s.width : seq.width;
    int H = s.height > 0 ? s.height : seq.height;
    W += W & 1;  // most codecs need even dimensions
    H += H & 1;
    const int sr = s.sampleRate > 0 ? s.sampleRate : seq.sampleRate;

    const ColorSpace& seqSpace = sequenceColorSpace(seq);
    const ColorSpace* chosen = findColorSpace(s.colorSpace);
    // OpenEXR holds scene-linear light: Linear Rec.709 unless a linear (or any scene-referred) space is asked for.
    // DPX can hold camera log too (a VFX pull of log footage stays log).
    const bool logOk = s.videoCodec == "dpx";
    const ColorSpace& outSpace = floatFrames ? (chosen ? *chosen : *findColorSpace("linear-rec709"))
                                 : chosen && (logOk || !chosen->sceneReferred) ? *chosen
                                                                               : seqSpace;
    const bool pq = outSpace.transfer == Transfer::Pq;
    const double peakNits = std::clamp(seq.hdrPeakNits, 100.0, 10000.0);
    // Light levels stated up front: the sequence's analysed ones, else the mastering peak (and an average under it).
    const bool analysed = seq.hdrMaxCll > 0;
    const int maxCll = analysed ? int(std::clamp(std::lround(seq.hdrMaxCll), 1L, 10000L)) : int(std::lround(peakNits));
    const int maxFall = analysed ? int(std::clamp(std::lround(seq.hdrMaxFall), 1L, long(maxCll))) : int(std::lround(std::min(peakNits, 400.0)));
    // What is rendered is measured as it goes (HDR only): the file gets those where its format allows.
    LightLevels measured;
    const LightMeter meter(outSpace);
    const bool measure = wantVideo && outSpace.hdr() && meter.valid();
    if (wantVideo) {
        std::string c = s.videoCodec;
        const bool hardware = c == "hw_h264" || c == "hw_hevc";
        if (hardware) {
            const std::string family = c.substr(3);
            c = pickHwEncoder(family, W, H, fps.num, fps.den);
            if (c.empty()) c = family == "hevc" ? "libx265" : "libx264";  // no hardware encoder here
        }
        if (encoderUsed) *encoderUsed = c;
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
        o.vctx->gop_size = s.gop > 0 ? s.gop : std::max(1, int(std::lround(seq.fpsValue() * 2)));
        if (s.gop == 1) o.vctx->max_b_frames = 0;
        const ColorTags tags = colorTags(outSpace);
        o.vctx->color_primaries = tags.primaries;
        o.vctx->color_trc = tags.trc;
        o.vctx->colorspace = tags.matrix;
        o.vctx->color_range = c == "mjpeg" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        o.vctx->thread_count = 0;
        // Smart rendering interleaves copied packets with encoded ones, so the encoder must not hold frames back.
        if (s.smartRender && isIntraCodec(c)) o.vctx->thread_type = FF_THREAD_SLICE;
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
            o.vctx->gop_size = s.gop > 0 ? s.gop : (std::lround(seq.fpsValue()) % 25 == 0 ? 12 : 15);
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
        rc = avcodec_open2(o.vctx, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) return fail("Cannot open video encoder: " + averr(rc));
        avcodec_parameters_from_context(o.vst->codecpar, o.vctx);
        o.vst->time_base = o.vctx->time_base;
        o.vst->avg_frame_rate = fps;
        if (seq.spherical) {
            // A 360° sequence: equirectangular spherical metadata, so players and YouTube show it as 360°
            // (MP4/MOV sv3d and st3d boxes, which FFmpeg writes only when asked to go beyond the standard; Matroska
            // Projection).
            size_t size = 0;
            if (AVSphericalMapping* map = av_spherical_alloc(&size)) {
                map->projection = AV_SPHERICAL_EQUIRECTANGULAR;
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
        // The sequence's layout (the channel order core/Surround.h uses is FFmpeg's), or stereo.
        AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
        if (!s.downmixStereo && seq.audioLayout == "5.1") layout = AV_CHANNEL_LAYOUT_5POINT1;
        if (!s.downmixStereo && seq.audioLayout == "7.1") layout = AV_CHANNEL_LAYOUT_7POINT1;
        if (!s.downmixStereo && immersiveLayout(seq.audioLayout)) {
            // FFmpeg's overhead layouts (5.1.x on the back pair, 7.1.2's pair at the sides), whose native order is the
            // order core/Surround.h mixes in.
            const uint64_t top = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT, rear = AV_CH_TOP_BACK_LEFT | AV_CH_TOP_BACK_RIGHT;
            uint64_t mask = 0;
            if (seq.audioLayout == "5.1.2") mask = AV_CH_LAYOUT_5POINT1_BACK | top;
            else if (seq.audioLayout == "5.1.4") mask = AV_CH_LAYOUT_5POINT1_BACK | top | rear;
            else if (seq.audioLayout == "7.1.2") mask = AV_CH_LAYOUT_7POINT1 | AV_CH_TOP_SIDE_LEFT | AV_CH_TOP_SIDE_RIGHT;
            else if (seq.audioLayout == "7.1.4") mask = AV_CH_LAYOUT_7POINT1 | top | rear;
            AVChannelLayout immersive{};
            if (!mask || av_channel_layout_from_mask(&immersive, mask) < 0 || immersive.nb_channels != layoutChannels(seq.audioLayout))
                return "This FFmpeg cannot describe the " + seq.audioLayout + " layout";
            layout = immersive;
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
        if (s.audioCodec == "aac" || s.audioCodec == "libopus") actx->bit_rate = s.audioBitrate;
        actx->time_base = AVRational{1, actx->sample_rate};
        if (o.oc->oformat->flags & AVFMT_GLOBALHEADER) actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((rc = avcodec_open2(actx, codec, nullptr)) < 0) return "Cannot open audio encoder: " + averr(rc);
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
        std::vector<float> fifo;
        int64_t pts = 0, cursor = 0;
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
            }
            e->mixer.setTrackMask(want.tracks);
            e->seq = seq;
            for (const std::string& r : want.unmute) std::erase(e->seq.mutedRoles, r);
            if (!want.role.empty())
                for (Track& t : e->seq.audioTracks)
                    for (Clip& c : t.clips)
                        if ((c.role.empty() ? std::string("No Role") : c.role) != want.role) c.enabled = false;
            extras.push_back(std::move(e));
        }
    }
    if (o.actx && o.actx->sample_rate != sr && s.audioCodec != "libopus") return fail("Unsupported sample rate");

    // Captions as subtitle streams, in the text format the container takes: the chosen track, then any others asked for.
    const CaptionTrack* captions = (s.embedCaptions || s.burnInCaptions) ? captionTrackFor(seq, s.captionTrack) : nullptr;
    struct CaptionOut {
        const CaptionTrack* track = nullptr;
        AVCodecContext* ctx = nullptr;
        AVStream* st = nullptr;
        size_t next = 0;
        ~CaptionOut() { avcodec_free_context(&ctx); }
    };
    std::vector<std::unique_ptr<CaptionOut>> captionOuts;
    std::vector<const CaptionTrack*> embedded;
    if (s.embedCaptions && captions && !captions->captions.empty()) embedded.push_back(captions);
    if (s.embedCaptions)
        for (Id id : s.extraCaptions)
            for (const CaptionTrack& t : seq.captionTracks)
                if (t.id == id && &t != captions && !t.captions.empty()) embedded.push_back(&t);
    for (const CaptionTrack* track : embedded) {
        auto out = std::make_unique<CaptionOut>();
        out->track = track;
        AVCodecID id = AV_CODEC_ID_NONE;
        for (AVCodecID c : {AV_CODEC_ID_MOV_TEXT, AV_CODEC_ID_SUBRIP, AV_CODEC_ID_WEBVTT})
            if (avformat_query_codec(o.oc->oformat, c, FF_COMPLIANCE_NORMAL) == 1) {
                id = c;
                break;
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
        out->next = captionIndexAt(*track, in);
        captionOuts.push_back(std::move(out));
    }

    if (!(o.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((rc = avio_open(&o.oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("Cannot write " + s.path + ": " + averr(rc));
        opened = true;
    }
    av_dict_set(&o.oc->metadata, "encoder", "Montage", 0);
    if (!s.startTimecode.empty()) av_dict_set(&o.oc->metadata, "timecode", s.startTimecode.c_str(), 0);
    if (s.chapters) addChapters(o.oc, seq, in, out);
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
    std::vector<uint16_t> rgba16;
    // Loudness normalisation: measure the whole mix first, then play it through a gain and a limiter.
    double normGain = 1;
    std::unique_ptr<PeakLimiter> limiter;
    int64_t limiterDelay = 0;
    if (wantAudio && s.loudnessTarget < 0) {
        AudioMixer meterMixer;
        meterMixer.setTrackMask(s.audioTracks);
        LoudnessMeter meter(mixRate);
        const int64_t end = int64_t(std::llround(double(out) * mixRate / seq.fpsValue()));
        std::vector<float> chunk(size_t(8192) * size_t(nch));
        for (int64_t pos = audioCursor; pos < end;) {
            if (cancel && cancel->load()) return fail("Cancelled");
            const int n = int(std::min<int64_t>(8192, end - pos));
            mixBlock(meterMixer, pos, n, chunk.data());
            meter.addChannels(chunk.data(), n, nch, weights.data());
            pos += n;
        }
        const LoudnessResult measured = meter.result();
        if (measured.valid) normGain = std::pow(10.0, (s.loudnessTarget - measured.integrated) / 20.0);
        // A little under the ceiling: the limiter sees samples, and true peaks fall between them.
        limiter = std::make_unique<PeakLimiter>(mixRate, s.peakCeiling - 0.5, 5, 80, nch);
        limiterDelay = limiter->latency();
        // Fill the limiter's look-ahead so its output starts at the first sample.
        std::vector<float> prime(size_t(limiterDelay) * size_t(nch));
        mixBlock(mixer, audioCursor, int(limiterDelay), prime.data());
        for (float& v : prime) v = float(v * normGain);
        limiter->process(prime.data(), prime.data(), int(limiterDelay));
        audioCursor += limiterDelay;
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
    auto writeCaptions = [&](FrameTime until) -> bool {
        const double msPerFrame = 1000.0 / seq.fpsValue();
        for (auto& co : captionOuts)
        for (; co->next < co->track->captions.size() && co->track->captions[co->next].start < until; ++co->next) {
            const Caption& c = co->track->captions[co->next];
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

    const int64_t total = out - in;
    // The burn-in logo, loaded once.
    const QImage watermark = s.burnIn.watermark.empty() ? QImage() : QImage(QString::fromStdString(s.burnIn.watermark));
    if (!s.burnIn.watermark.empty() && watermark.isNull()) return fail("Cannot read the watermark image " + s.burnIn.watermark);
    RenderOptions ro;
    ro.scale = double(W) / seq.width;
    ro.highQuality = true;
    ro.useProxies = s.useProxies;
    // Smart rendering: only where nothing would change the source's pictures for the whole export.
    std::unique_ptr<SmartRenderer> smart;
    if (wantVideo && s.smartRender && o.vctx && isIntraCodec(o.vctx->codec ? o.vctx->codec->name : "") && W == seq.width && H == seq.height &&
        !s.useProxies && !s.burnInCaptions && !s.cea608 && !s.burnIn.any() && outSpace.id == seqSpace.id)
        smart = std::make_unique<SmartRenderer>(p, seq, seqSpace, o.vctx, o.vst, s.alpha, s.profile);
    // CEA-608 captions in the video: the pairs due in each frame's span of the 29.97 Hz line-21 clock, padding between.
    std::vector<Cc608Pair> cc608;
    size_t ccNext = 0;
    if (s.cea608 && wantVideo) {
        const std::string vc = o.vctx && o.vctx->codec ? o.vctx->codec->name : "";
        if (o.vctx->codec_id != AV_CODEC_ID_H264 && o.vctx->codec_id != AV_CODEC_ID_HEVC)
            return fail("CEA-608 captions go in H.264 or HEVC video, not " + vc);
        if (const CaptionTrack* t = captionTrackFor(seq, s.captionTrack)) {
            std::vector<Caption> part;
            for (Caption c : t->captions) {
                if (c.end <= in || c.start >= out) continue;
                c.start = std::max(c.start, in) - in;
                c.end = std::min(c.end, out) - in;
                part.push_back(std::move(c));
            }
            cc608 = captionsTo608(part, seq.fps);
        }
    }
    auto attach608 = [&](FrameTime rel) {
        if (!s.cea608) return true;
        av_frame_remove_side_data(o.vframe, AV_FRAME_DATA_A53_CC);
        const double rate = 30000.0 / 1001.0, fpsv = seq.fpsValue();
        const int64_t from = int64_t(std::floor(double(rel) / fpsv * rate + 1e-6)), to = int64_t(std::floor(double(rel + 1) / fpsv * rate + 1e-6));
        std::vector<uint8_t> cc;
        for (int64_t slot = from; slot < to; ++slot) {
            uint16_t pair = 0x8080;  // padding (null characters with parity)
            while (ccNext < cc608.size() && cc608[ccNext].frame < slot) ++ccNext;
            if (ccNext < cc608.size() && cc608[ccNext].frame == slot) pair = cc608[ccNext++].pair;
            cc.insert(cc.end(), {uint8_t(0xfc), uint8_t(pair >> 8), uint8_t(pair & 0xff)});  // valid, field 1
        }
        if (cc.empty()) return true;
        AVFrameSideData* sd = av_frame_new_side_data(o.vframe, AV_FRAME_DATA_A53_CC, cc.size());
        if (!sd) return false;
        std::memcpy(sd->data, cc.data(), cc.size());
        return true;
    };
    for (FrameTime f = in; f < out; ++f) {
        if (cancel && cancel->load()) return fail("Export cancelled");
        if (!writeCaptions(f + 1)) return fail("Writing captions failed");
        if (wantVideo && smart && smart->copy(f, in, o.oc)) {
            // copied from the source
        } else if (wantVideo) {
            Image img = s.alpha ? renderSequenceFrame(p, seq, f, ro) : renderProgramFrame(p, seq, f, ro);
            if (s.burnInCaptions && captions) drawCaption(img, *captions, f, &seqSpace);
            if (s.burnIn.any()) drawBurnIns(img, p, seq, f, s.burnIn, watermark.isNull() ? nullptr : &watermark, &seqSpace);
            convertColor(img, seqSpace, outSpace, peakNits);
            if (measure) meter.add(img, f, measured);
            if (floatFrames && img.width == W && img.height == H) {
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
                o.vframe->pts = f - in;
                if (!attach608(f - in)) return fail("Out of memory");
                if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("Video encoding failed: " + averr(rc));
                if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing video failed: " + averr(rc));
            } else {
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
            if (!attach608(f - in)) return fail("Out of memory");
            if ((rc = avcodec_send_frame(o.vctx, o.vframe)) < 0) return fail("Video encoding failed: " + averr(rc));
            if ((rc = drain(o, o.vctx, o.vst)) < 0) return fail("Writing video failed: " + averr(rc));
            }
        }
        if (wantAudio) {
            int64_t target = int64_t(std::llround(double(f + 1) * mixRate / seq.fpsValue())) + limiterDelay;
            int n = int(target - audioCursor);
            if (n > 0) {
                mixBuf.resize(size_t(n) * size_t(nch));
                mixBlock(mixer, audioCursor, n, mixBuf.data());
                if (limiter) {
                    for (float& v : mixBuf) v = float(v * normGain);
                    limiter->process(mixBuf.data(), mixBuf.data(), n);
                }
                audioCursor = target;
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
            // The other streams, sample for sample with the mix (not normalised: stems keep their levels).
            for (auto& e : extras) {
                const int64_t until = int64_t(std::llround(double(f + 1) * mixRate / seq.fpsValue()));
                const int m = int(until - e->cursor);
                if (m <= 0) continue;
                mixBuf.resize(size_t(m) * size_t(nch));
                e->seq.sampleRate = mixRate;
                if (nch > 2) e->mixer.mixLayout(p, e->seq, e->cursor, m, mixBuf.data());
                else e->mixer.mix(p, e->seq, e->cursor, m, mixBuf.data());
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
        if (progress && ((f - in) % 5 == 0 || f + 1 == out)) progress(double(f - in + 1) / double(total), f);
    }
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
            return fail("Finishing video failed: " + averr(rc));
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
    if (smartRendered) *smartRendered = smart ? smart->copied() : 0;
    if (light) *light = measured;
    return true;
}

}  // namespace

int maxAudioChannels(const std::string& codec) {
    if (codec.rfind("pcm_", 0) == 0) return 64;
    if (codec == "ac3" || codec == "eac3" || codec == "ac3_fixed") return 6;
    if (codec == "libmp3lame" || codec == "mp3" || codec == "mp2" || codec == "libtwolame") return 2;
    return 8;
}

std::string exportAudioLayout(const std::string& layout, const std::string& codec) {
    if (codec.empty()) return layout;
    const int most = maxAudioChannels(codec);
    if (layoutChannels(layout) <= most) return layout;
    const std::string ear = earLevelLayout(layout);
    if (layoutChannels(ear) <= most) return ear;
    return most >= 6 ? "5.1" : "stereo";
}

bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed, int* smartRendered,
                    LightLevels* light) {
    bool opened = false;
    if (smartRendered) *smartRendered = 0;
    if (light) *light = LightLevels{};
    // Channels a codec cannot carry fold down (an immersive mix to its ear-level layout, for AAC and the like).
    const std::string layout = s.downmixStereo ? seq.audioLayout : exportAudioLayout(seq.audioLayout, s.audioCodec);
    // A described master: the mix without the descriptions, then the programme with them as a stream of its own.
    const bool described = s.describedStream && hasDescriptionClips(seq) && !s.audioCodec.empty();
    bool ok;
    if (layout != seq.audioLayout || described) {
        Sequence work = seq;
        work.audioLayout = layout;
        ExportSettings settings = s;
        if (described) {
            if (!edit::roleMuted(work, kDescriptionRole)) work.mutedRoles.push_back(kDescriptionRole);
            ExportSettings::AudioStream ad;
            ad.name = s.describedName.empty() ? std::string("Audio Description") : s.describedName;
            ad.language = s.audioLanguage;
            ad.unmute = {kDescriptionRole};
            settings.extraAudio.push_back(std::move(ad));
            if (settings.audioName.empty()) settings.audioName = "Programme";
        }
        ok = exportImpl(p, work, settings, progress, cancel, error, opened, encoderUsed, smartRendered, light);
    } else {
        ok = exportImpl(p, seq, s, progress, cancel, error, opened, encoderUsed, smartRendered, light);
    }
    // Never leave a truncated file behind (the output is closed by now), but
    // don't touch an existing file if we failed before writing to it.
    if (!ok && opened) std::remove(s.path.c_str());
    return ok;
}

std::vector<ExportSettings::AudioStream> stemStreams(const Sequence& seq, int groups) {
    std::vector<ExportSettings::AudioStream> out;
    const std::string language = seq.captionTracks.empty() ? std::string() : seq.captionTracks.front().language;
    const size_t n = seq.audioTracks.size();
    auto hasClips = [&](size_t i) { return !seq.audioTracks[i].muted && !seq.audioTracks[i].clips.empty(); };
    if (groups == StemsByRole) {
        Sequence named = seq;
        for (Track& t : named.audioTracks)
            for (Clip& c : t.clips)
                if (c.role.empty()) c.role = "No Role";
        for (const std::string& r : edit::sequenceRoles(named)) {
            if (edit::roleMuted(seq, r)) continue;
            bool used = false;
            for (size_t i = 0; i < n && !used; ++i)
                if (hasClips(i))
                    for (const Clip& c : named.audioTracks[i].clips) used = used || (c.enabled && c.role == r);
            if (used) out.push_back({r, language, {}, r});
        }
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

bool renderClipVideo(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress, const std::atomic<bool>* cancel) {
    const Clip* c = edit::clipById(seq, clip);
    if (!c) {
        if (error) *error = "No such clip";
        return false;
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
    ExportSettings st;
    st.path = path;
    st.videoCodec = "prores_ks";
    st.profile = "4444";  // with its transparency
    st.alpha = true;
    st.audioCodec = "none";
    st.smartRender = false;
    st.in = 0;
    st.out = copy.duration;
    return exportSequence(p, alone, st, progress, cancel, error);
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
