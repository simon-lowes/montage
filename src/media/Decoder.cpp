#include "Vector.h"
#include "CameraRaw.h"
#include "Decoder.h"

#include "FieldRecorder.h"
#include "ImageSequence.h"
#include "ProResRaw.h"
#include "Psd.h"
#include "audio/TimeStretch.h"
#include "core/Ambisonics.h"
#include "SpatialAudio.h"
#include "core/Interpretation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
#include <libavutil/timecode.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "HwAccel.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

// Keep FFmpeg quiet unless something is actually wrong.
const bool kQuietLogs = [] {
    av_log_set_level(AV_LOG_ERROR);
    return true;
}();

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool isStillFormat(const AVFormatContext* fmt) {
    if (!fmt || !fmt->iformat || !fmt->iformat->name) return false;
    std::string n = fmt->iformat->name;
    if (n == "image2" && fmt->url && av_filename_number_test(fmt->url)) return false;  // a numbered sequence plays
    if (n == "image2" || n == "image2pipe") return true;
    return n.size() > 5 && n.compare(n.size() - 5, 5, "_pipe") == 0;
}

int bestVideoStream(AVFormatContext* fmt) {
    int best = -1;
    int64_t bestArea = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
        int64_t area = int64_t(st->codecpar->width) * st->codecpar->height;
        if (area > bestArea) {
            bestArea = area;
            best = int(i);
        }
    }
    return best;
}

// 360° footage: the stream's spherical mapping (MP4 sv3d, Matroska Projection), "equirect" for the whole sphere.
std::string streamProjection(const AVStream* st) {
    const uint8_t* data = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data, AV_PKT_DATA_SPHERICAL);
    if (sd) data = sd->data;
#else
    data = av_stream_get_side_data(st, AV_PKT_DATA_SPHERICAL, nullptr);
#endif
    if (!data) return {};
    const auto* map = reinterpret_cast<const AVSphericalMapping*>(data);
    if (map->projection == AV_SPHERICAL_EQUIRECTANGULAR) return "equirect";
    // VR180: a tile of the sphere a quarter of the way in from each side (0.32 fixed point), the whole height.
    if (map->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE && map->bound_top == 0 && map->bound_bottom == 0) {
        const double cut = (double(map->bound_left) + double(map->bound_right)) / 4294967296.0;
        if (std::fabs(cut - 0.5) < 0.01) return "vr180";
    }
    return {};
}

// Stereoscopic 3D footage: how the stream's stereo metadata (MP4 st3d, Matroska StereoMode)
// packs the eyes, "sbs" or "tb", and whether the right eye comes first. A frame-compatible file (each eye squeezed to
// half the frame, as 3D TV and Blu-ray masters are) is told apart by its eyes' shape; 360° stereo is never squeezed.
std::string streamStereo(const AVStream* st, bool& inverted, bool spherical) {
    inverted = false;
    const uint8_t* data = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data, AV_PKT_DATA_STEREO3D);
    if (sd) data = sd->data;
#else
    data = av_stream_get_side_data(st, AV_PKT_DATA_STEREO3D, nullptr);
#endif
    if (!data) return {};
    const auto* s3d = reinterpret_cast<const AVStereo3D*>(data);
    inverted = (s3d->flags & AV_STEREO3D_FLAG_INVERT) != 0;
    // The shape as shown (a stored width with non-square pixels is widened by them).
    const AVRational sar = st->sample_aspect_ratio.num > 0 ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
    const double w = st->codecpar->width * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0), h = std::max(1, st->codecpar->height);
    if (s3d->type == AV_STEREO3D_SIDEBYSIDE) return !spherical && w / 2 / h < 1.0 ? "sbs_half" : "sbs";
    if (s3d->type == AV_STEREO3D_TOPBOTTOM) return !spherical && w / (h / 2) > 2.4 ? "tb_half" : "tb";
    return {};
}

// One eye's size as shown, from the frame's.
void stereoEyeSize(const std::string& layout, int& w, int& h) {
    if (layout == "sbs") w = std::max(1, w / 2);
    else if (layout == "tb") h = std::max(1, h / 2);
}

int streamRotation(const AVStream* st) {
    const uint8_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd) matrix = sd->data;
#else
    matrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
    if (!matrix) return 0;
    double theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (std::isnan(theta)) return 0;
    theta -= 360.0 * std::floor(theta / 360.0 + 0.9 / 360.0);
    int r = int(std::lround(theta / 90.0)) * 90 % 360;
    return r < 0 ? r + 360 : r;
}

double fileOrigin(const AVFormatContext* fmt) {
    return fmt->start_time == AV_NOPTS_VALUE ? 0.0 : double(fmt->start_time) / AV_TIME_BASE;
}

AVPixelFormat dejpeg(AVPixelFormat f, bool& fullRange) {
    switch (f) {
        case AV_PIX_FMT_YUVJ420P: fullRange = true; return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: fullRange = true; return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: fullRange = true; return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: fullRange = true; return AV_PIX_FMT_YUV440P;
        default: return f;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Probe

int openMediaInput(AVFormatContext** fmt, const std::string& decorated) {
    // An interpretation (core/Interpretation.h) changes how frames are read, not which file is opened.
    const std::string path = uninterpretedPath(decorated);
    ImageSequence seq;
    if (parseImageSequencePath(path, seq)) {
        // A numbered image sequence: FFmpeg's image2 reader from its first number at its frame rate.
        AVDictionary* o = nullptr;
        av_dict_set_int(&o, "start_number", seq.first, 0);
        av_dict_set(&o, "framerate", (std::to_string(seq.fps.num) + "/" + std::to_string(seq.fps.den)).c_str(), 0);
        av_dict_set(&o, "pattern_type", "sequence", 0);
        const int rc = avformat_open_input(fmt, seq.pattern.c_str(), av_find_input_format("image2"), &o);
        av_dict_free(&o);
        return rc;
    }
    return avformat_open_input(fmt, path.c_str(), nullptr, nullptr);
}

bool readEmbeddedCaptions(const std::string& path, Rational fps, std::vector<Caption>& out, const std::function<void(double)>& progress,
                          const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    AVFormatContext* fmt = nullptr;
    int rc = openMediaInput(&fmt, path);
    if (rc < 0) return fail("Cannot open " + path + ": " + averr(rc));
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmtGuard(fmt, [](AVFormatContext* f) { avformat_close_input(&f); });
    if ((rc = avformat_find_stream_info(fmt, nullptr)) < 0) return fail(averr(rc));
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return fail("No video stream");
    AVStream* st = fmt->streams[stream];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return fail("No decoder for the video");
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ctx(avcodec_alloc_context3(codec), [](AVCodecContext* c) { avcodec_free_context(&c); });
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->thread_count = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    if ((rc = avcodec_open2(ctx.get(), codec, nullptr)) < 0) return fail("Cannot open the video decoder: " + averr(rc));
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
    const double tb = av_q2d(st->time_base);
    const int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const double duration = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
    std::vector<std::pair<double, uint16_t>> pairs;
    auto take = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const double t = pts == AV_NOPTS_VALUE ? 0 : double(pts - start) * tb;
            if (const AVFrameSideData* sd = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_A53_CC)) {
                int k = 0;  // field-1 pairs in this frame, a 29.97th of a second apart
                for (size_t i = 0; i + 2 < size_t(sd->size); i += 3) {
                    const uint8_t head = sd->data[i];
                    if (!(head & 0x04) || (head & 0x03) != 0) continue;  // not valid, or not field 1 (CC1/CC2)
                    pairs.push_back({t + double(k++) * 1001.0 / 30000.0, uint16_t((sd->data[i + 1] << 8) | sd->data[i + 2])});
                }
            }
            av_frame_unref(frame.get());
        }
    };
    int64_t packets = 0;
    while (av_read_frame(fmt, pkt.get()) >= 0) {
        if (pkt->stream_index == stream) {
            if ((++packets & 31) == 0) {
                if (cancel && cancel->load()) return fail("Cancelled");
                if (progress && duration > 0 && pkt->pts != AV_NOPTS_VALUE) progress(std::clamp(double(pkt->pts - start) * tb / duration, 0.0, 1.0));
            }
            if (avcodec_send_packet(ctx.get(), pkt.get()) >= 0) take();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(ctx.get(), nullptr);
    take();
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const bool any = std::any_of(pairs.begin(), pairs.end(), [](const auto& p) { return (p.second & 0x7f7f) != 0; });
    if (!any) return fail("The video carries no CEA-608 captions");
    // A conformed video's captions keep to its frames.
    if (Interpretation in; parseInterpretation(path, in) && in.conformed())
        for (auto& pair : pairs) pair.first /= in.timeScale();
    std::string err;
    if (!captionsFrom608(pairs, fps, out, &err)) return fail("The video's CEA-608 data holds no captions");
    return true;
}

bool probeMedia(const std::string& path, MediaItem& out, std::string* error) {
    Interpretation in;
    parseInterpretation(path, in);
    const std::string file = uninterpretedPath(path);
    if (isVectorPath(file)) {
        VectorInfo vi;
        if (!openVector(file, vi, error)) return false;
        MediaItem m = out;
        m.path = path;
        if (m.name.empty()) m.name = file.substr(file.find_last_of("/\\") + 1);
        m.hasVideo = true;
        m.hasAudio = false;
        m.width = vi.width;
        m.height = vi.height;
        m.videoCodec = vi.animated ? "lottie" : "svg";
        if (vi.animated) {
            m.kind = MediaKind::Video;
            m.duration = vi.duration;
            const double f = vi.fps;
            m.fps = std::fabs(f - std::round(f)) < 1e-3 ? Rational{int(std::lround(f)), 1} : Rational{int(std::lround(f * 1001)), 1001};
        } else {
            m.kind = MediaKind::Image;
            m.duration = 0;
        }
        out = m;
        return true;
    }
    {
        std::string file;
        int layer = -1;
        if (parsePsdLayerPath(path, file, layer)) {
            PsdInfo info;
            if (!readPsdInfo(file, info, error)) return false;
            if (layer >= int(info.layers.size())) {
                if (error) *error = "The Photoshop file has no layer " + std::to_string(layer);
                return false;
            }
            MediaItem m = out;
            m.path = path;
            if (m.name.empty()) m.name = info.layers[size_t(layer)].name;
            m.kind = MediaKind::Image;
            m.hasVideo = true;
            m.hasAudio = false;
            m.width = info.width;
            m.height = info.height;
            m.duration = 0;
            m.videoCodec = "psd";
            out = m;
            return true;
        }
    }
    // A CinemaDNG run: numbered camera RAW frames, played as a video at the run's rate.
    if (ImageSequence seq; rawAvailable() && parseImageSequencePath(file, seq) && isRawPath(seq.pattern)) {
        RawInfo ri;
        if (!probeRaw(imageSequenceFrame(seq, seq.first), ri, error)) return false;
        MediaItem m = out;
        m.path = path;
        if (m.name.empty()) m.name = imageSequenceName(seq);
        m.kind = MediaKind::Video;
        m.hasVideo = true;
        m.hasAudio = false;
        m.width = ri.width;
        m.height = ri.height;
        m.fps = seq.fps;
        m.duration = seq.frames() / seq.fps.toDouble();
        m.videoCodec = "dng";
        if (!ri.camera.empty() && !m.metadata.count("camera")) m.metadata["camera"] = ri.camera;
        out = m;
        return true;
    }
    if (rawAvailable() && isRawPath(file)) {
        RawInfo ri;
        if (!probeRaw(file, ri, error)) return false;
        MediaItem m = out;
        m.path = path;
        if (m.name.empty()) m.name = file.substr(file.find_last_of("/\\") + 1);
        m.kind = MediaKind::Image;
        m.hasVideo = true;
        m.hasAudio = false;
        m.width = ri.width;
        m.height = ri.height;
        m.duration = 0;
        m.videoCodec = "raw";
        if (!ri.camera.empty() && !m.metadata.count("camera")) m.metadata["camera"] = ri.camera;
        out = m;
        return true;
    }
    AVFormatContext* fmt = nullptr;
    int rc = openMediaInput(&fmt, path);
    if (rc < 0) {
        if (error) *error = "Cannot open " + mediaFileOnDisk(path) + ": " + averr(rc);
        return false;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        if (error) *error = "Cannot read stream info: " + averr(rc);
        avformat_close_input(&fmt);
        return false;
    }
    MediaItem m = out;
    m.path = path;
    // Not std::filesystem: on Windows it would read the UTF-8 path in the ANSI code page.
    if (m.name.empty()) m.name = file.substr(file.find_last_of("/\\") + 1);
    int v = bestVideoStream(fmt);
    int a = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    m.hasVideo = v >= 0;
    m.hasAudio = a >= 0;
    if (!m.hasVideo && !m.hasAudio) {
        if (error) *error = "No audio or video streams in " + path;
        avformat_close_input(&fmt);
        return false;
    }
    double dur = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0.0;
    // When and on what it was recorded, from the container's tags (or a stream's).
    auto tag = [&](const char* key) -> std::string {
        if (const AVDictionaryEntry* e = av_dict_get(fmt->metadata, key, nullptr, 0)) return e->value;
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (const AVDictionaryEntry* e = av_dict_get(fmt->streams[i]->metadata, key, nullptr, 0)) return e->value;
        return {};
    };
    if (m.created.empty()) {
        m.created = tag("com.apple.quicktime.creationdate");
        if (m.created.empty()) m.created = tag("creation_time");
        if (m.created.empty()) m.created = tag("date");
    }
    if (!m.metadata.count("device")) {
        std::string make = tag("com.apple.quicktime.make"), model = tag("com.apple.quicktime.model");
        if (make.empty()) make = tag("make");
        if (model.empty()) model = tag("model");
        std::string device = model.rfind(make, 0) == 0 ? model : make + (make.empty() || model.empty() ? "" : " ") + model;
        if (!device.empty()) m.metadata["device"] = device;
    }
    if (m.hasVideo) {
        AVStream* st = fmt->streams[v];
        const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
        m.videoCodec = d ? d->name : "unknown";
        const char* pri = av_color_primaries_name(st->codecpar->color_primaries);
        const char* trc = av_color_transfer_name(st->codecpar->color_trc);
        m.colorSpace = colorSpaceFromTags(pri ? pri : "", trc ? trc : "");
        if (const std::string proj = streamProjection(st); !proj.empty()) m.projection = proj;
        if (m.colorSpace == "rec709") m.colorSpace.clear();
        int w = st->codecpar->width, h = st->codecpar->height;
#ifdef MONTAGE_PRORES_RAW
        // ProRes RAW is developed to ACEScct (the scene's whole range), at its size less the recommended crop.
        if (st->codecpar->codec_id == AV_CODEC_ID_PRORES_RAW) {
            m.colorSpace = "acescct";
            if (ProResRawInfo info; probeProResRaw(file, info)) w = info.width, h = info.height;
        }
#endif
        bool inverted = false;
        m.stereo = stereoLayout(streamStereo(st, inverted, !m.projection.empty()), in);
        stereoEyeSize(m.stereo, w, h);
        AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
        if (in.par > 0) w = int(std::lround(double(w) * in.par));  // Interpret Footage's pixel aspect
        else if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = int(std::lround(double(w) * sar.num / sar.den));
        if (streamRotation(st) % 180 != 0) std::swap(w, h);
        m.width = std::max(1, w);
        m.height = h;
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        if (fr.num > 0 && fr.den > 0) m.fps = Rational{fr.num, fr.den};
        // Conformed: each frame shown for 1 / the new rate, and the start timecode still names the same frame.
        if (in.conformed()) {
            m.fps = in.fps;
            fr = AVRational{in.fps.num, in.fps.den};
        }
        // Start timecode (camera files carry it in the stream, the container or a tmcd track).
        const AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0);
        if (!tc) tc = av_dict_get(fmt->metadata, "timecode", nullptr, 0);
        for (unsigned i = 0; !tc && i < fmt->nb_streams; ++i) tc = av_dict_get(fmt->streams[i]->metadata, "timecode", nullptr, 0);
        AVTimecode parsed;
        if (tc && fr.num > 0 && fr.den > 0 && av_timecode_init_from_string(&parsed, fr, tc->value, nullptr) == 0) {
            m.timecode = double(parsed.start) * fr.den / fr.num;
            if (std::strchr(tc->value, ';')) m.metadata["timecode_drop"] = "1";  // (so logs written of it say drop-frame too)
        }
        if (dur <= 0 && st->duration > 0) dur = double(st->duration) * av_q2d(st->time_base);
    }
    if (m.hasAudio) {
        AVStream* st = fmt->streams[a];
        const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
        m.audioCodec = d ? d->name : "unknown";
        m.sampleRate = st->codecpar->sample_rate;
        m.channels = st->codecpar->ch_layout.nb_channels;
        // Ambisonic sound: the file's spatial audio metadata (MP4 SA3D) gives its layout as ambisonic components.
        if (st->codecpar->ch_layout.order == AV_CHANNEL_ORDER_AMBISONIC && m.channels >= kFoaChannels)
            m.ambisonic = std::max(1, int(std::sqrt(double(m.channels) + 1e-9)) - 1);
        // (Reading AAC to find its layout replaces that with the decoder's speakers: the box itself is read then.)
        if (!m.ambisonic && m.channels >= kFoaChannels && std::strstr(fmt->iformat->name, "mov"))
            m.ambisonic = readSpatialAudioBox(uninterpretedPath(path));
        if (dur <= 0 && st->duration > 0) dur = double(st->duration) * av_q2d(st->time_base);
        m.audioStreams.clear();
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                m.audioStreams.push_back(std::max(1, fmt->streams[i]->codecpar->ch_layout.nb_channels));
        if (m.audioStreams.size() < 2) m.audioStreams.clear();
    }
    if (m.hasVideo && isStillFormat(fmt)) {
        m.kind = MediaKind::Image;
        m.duration = 0;
        m.hasAudio = false;
    } else if (m.hasVideo) {
        m.kind = MediaKind::Video;
        m.duration = dur / in.timeScale();
    } else {
        m.kind = MediaKind::Audio;
        m.duration = dur;
    }
    // A numbered image sequence: its frames at the rate it was given.
    if (ImageSequence seq; parseImageSequencePath(path, seq) && m.hasVideo) {
        m.kind = MediaKind::Video;
        m.fps = seq.fps;
        m.duration = seq.frames() / seq.fps.toDouble();
        if (out.name.empty()) m.name = imageSequenceName(seq);
    }
    // A field recorder's WAV: its timecode stamp, scene, take, notes and channel names.
    if (fmt->iformat && std::strstr(fmt->iformat->name, "wav")) {
        FieldRecording rec;
        if (readFieldRecording(path, rec)) applyFieldRecording(rec, m);
    }
    avformat_close_input(&fmt);
    out = m;
    return true;
}

// ---------------------------------------------------------------------------
// VideoDecoder

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::freeCodec() {
    avcodec_free_context(&ctx_);
    if (hwSlot_) releaseHwDecoderSlot();
    hwSlot_ = false;
    hwPixFmt_ = -1;
    hwName_.clear();
}

void VideoDecoder::close() {
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    av_frame_free(&cur_);
    av_frame_free(&next_);
    av_frame_free(&hwTransfer_);
    av_packet_free(&pkt_);
    freeCodec();
    hwBroken_ = false;
    if (fmt_) avformat_close_input(&fmt_);
    haveCur_ = haveNext_ = false;
    stillFrame_.reset();
    vector_.reset();
    if (raw_) av_frame_free(&raw_);
    rawSequence_.reset();
    rawFrame_ = -1;
    proResRaw_ = false;
}

bool VideoDecoder::open(const std::string& path, std::string* error) {
    close();
    path_ = path;
    Interpretation in;
    parseInterpretation(path, in);
    timeScale_ = in.timeScale();
    par_ = in.par;
    alpha_ = in.alpha;
    fields_ = in.fields;
    interp_ = in;
    if (isVectorPath(uninterpretedPath(path))) {
        VectorInfo vi;
        vector_ = openVector(uninterpretedPath(path), vi, error);
        if (!vector_) return false;
        dispW_ = vi.width;
        dispH_ = vi.height;
        fps_ = vi.animated ? vi.fps : 25.0;
        duration_ = vi.duration;
        still_ = !vi.animated;
        rotation_ = 0;
        origin_ = 0;
        curPts_ = nextPts_ = -1;
        return true;
    }
    {
        std::string file;
        int layer = -1;
        if (parsePsdLayerPath(path, file, layer)) {
            // One layer of a Photoshop file: a still the size of the canvas, transparent round the layer.
            PsdInfo info;
            std::vector<uint16_t> rgba;
            if (!readPsdPixels(file, layer, info, rgba, error)) return false;
            raw_ = av_frame_alloc();
            if (!raw_) return false;
            raw_->format = AV_PIX_FMT_RGBA64;  // host byte order
            raw_->width = info.width;
            raw_->height = info.height;
            raw_->color_range = AVCOL_RANGE_JPEG;
            if (av_frame_get_buffer(raw_, 0) < 0) {
                if (error) *error = "Out of memory reading " + path;
                close();
                return false;
            }
            for (int y = 0; y < info.height; ++y)
                std::memcpy(raw_->data[0] + size_t(y) * size_t(raw_->linesize[0]), rgba.data() + size_t(y) * size_t(info.width) * 4,
                            size_t(info.width) * 4 * sizeof(uint16_t));
            dispW_ = info.width;
            dispH_ = info.height;
            fps_ = 25.0;
            duration_ = 0;
            still_ = true;
            rotation_ = 0;
            origin_ = 0;
            curPts_ = nextPts_ = -1;
            return true;
        }
    }
    rawSettings_ = RawSettings{in.rawExposure, in.rawTemperature, in.rawTint,
                               in.rawHighlights == "blend" ? 1 : in.rawHighlights == "rebuild" ? 2 : 0, in.rawHalf};
    // A CinemaDNG run (numbered camera RAW frames): each frame developed when it is shown.
    if (ImageSequence seq; rawAvailable() && parseImageSequencePath(uninterpretedPath(path), seq) && isRawPath(seq.pattern)) {
        RawInfo info;
        if (!probeRaw(imageSequenceFrame(seq, seq.first), info, error)) return false;
        rawSequence_ = std::make_unique<ImageSequence>(seq);
        rawFrame_ = -1;
        dispW_ = info.width;
        dispH_ = info.height;
        fps_ = seq.fps.toDouble();
        duration_ = seq.frames() / fps_;
        still_ = false;
        rotation_ = 0;
        origin_ = 0;
        curPts_ = nextPts_ = -1;
        return true;
    }
    if (rawAvailable() && isRawPath(uninterpretedPath(path))) {
        RawImage img;
        if (!developRaw(uninterpretedPath(path), img, error, rawSettings_) || !loadRaw(img, error)) {
            close();
            return false;
        }
        // A half-size decode is shown at the full size.
        dispW_ = rawSettings_.half ? img.width * 2 : img.width;
        dispH_ = rawSettings_.half ? img.height * 2 : img.height;
        fps_ = 25.0;
        duration_ = 0;
        still_ = true;
        rotation_ = 0;
        origin_ = 0;
        curPts_ = nextPts_ = -1;
        return true;
    }
    int rc = openMediaInput(&fmt_, path);
    if (rc < 0) {
        if (error) *error = "Cannot open " + path + ": " + averr(rc);
        return false;
    }
    if ((rc = avformat_find_stream_info(fmt_, nullptr)) < 0) {
        if (error) *error = averr(rc);
        close();
        return false;
    }
    stream_ = bestVideoStream(fmt_);
    if (stream_ < 0) {
        if (error) *error = "No video stream";
        close();
        return false;
    }
    AVStream* st = fmt_->streams[stream_];
    still_ = isStillFormat(fmt_);
#ifdef MONTAGE_PRORES_RAW
    // ProRes RAW: a Bayer mosaic and its colour, developed here (media/ProResRaw.h), in software.
    proResRaw_ = st->codecpar->codec_id == AV_CODEC_ID_PRORES_RAW;
#endif
    if (!openCodec(hwDecodeMode() == HwDecodeMode::Auto && !still_ && !proResRaw_, error)) {
        close();
        return false;
    }
    // Discard non-video streams at the demuxer level.
    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
        if (int(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;
    pkt_ = av_packet_alloc();
    cur_ = av_frame_alloc();
    next_ = av_frame_alloc();
    timeBase_ = av_q2d(st->time_base);
    origin_ = fileOrigin(fmt_);
    AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
    fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    duration_ = fmt_->duration > 0 ? double(fmt_->duration) / AV_TIME_BASE : 0;
    rotation_ = streamRotation(st);
    int w = st->codecpar->width, h = st->codecpar->height;
    // (less its recommended crop, which the first frame says)
    if (ProResRawInfo info; proResRaw_ && probeProResRaw(uninterpretedPath(path), info)) w = info.width, h = info.height;
    // Stereoscopic footage: one eye is read, the one asked for (the file's right-first packing and Interpret Footage's
    // swap each turn them round).
    bool inverted = false;
    stereo_ = stereoLayout(streamStereo(st, inverted, !streamProjection(st).empty()), interp_);
    eye_ = (interp_.eye != 0) != (inverted != interp_.swapEyes) ? 1 : 0;
    stereoEyeSize(stereo_, w, h);
    AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
    if (par_ > 0) w = int(std::lround(double(w) * par_));
    else if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = int(std::lround(double(w) * sar.num / sar.den));
    if (rotation_ % 180 != 0) std::swap(w, h);
    dispW_ = std::max(1, w);
    dispH_ = std::max(1, h);
    eof_ = false;
    curPts_ = nextPts_ = -1;
    return true;
}

bool VideoDecoder::openCodec(bool tryHardware, std::string* error) {
    AVStream* st = fmt_->streams[stream_];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (error) *error = "Unsupported video codec";
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx_, st->codecpar);
    ctx_->pkt_timebase = st->time_base;
    // The mosaic whole: its crop is cut after it is developed, on whole 2x2 cells (FFmpeg's would shift the pattern).
    if (proResRaw_) ctx_->apply_cropping = 0;
    // Hardware: the first device of this platform's list that the codec supports.
    for (const std::string& name : tryHardware ? hwDeviceCandidates() : std::vector<std::string>{}) {
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(name.c_str());
        if (type == AV_HWDEVICE_TYPE_NONE) continue;
        const AVCodecHWConfig* config = nullptr;
        for (int i = 0; (config = avcodec_get_hw_config(codec, i)); ++i)
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type) break;
        if (!config) continue;
        AVBufferRef* device = hwDevice(type);
        if (!device || !acquireHwDecoderSlot()) continue;
        hwSlot_ = true;
        ctx_->hw_device_ctx = av_buffer_ref(device);
        hwPixFmt_ = config->pix_fmt;
        hwName_ = name;
        break;
    }
    if (hwPixFmt_ >= 0) {
        ctx_->opaque = this;
        ctx_->get_format = reinterpret_cast<AVPixelFormat (*)(AVCodecContext*, const AVPixelFormat*)>(&VideoDecoder::pickFormat);
        ctx_->thread_count = 1;
    } else {
        // Many decoders can be open at once; bound each one's thread pool.
        ctx_->thread_count = int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
        ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        // ProRes RAW (every frame a key frame) decodes its tiles in parallel; FFmpeg 9.0's frame threading would run
        // them one after another, and hold each seek's first frame back until eight were decoded.
        if (proResRaw_) ctx_->thread_type = FF_THREAD_SLICE;
    }
    int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0 && hwPixFmt_ >= 0) {
        freeCodec();
        return openCodec(false, error);
    }
    if (rc < 0) {
        if (error) *error = "Cannot open decoder: " + averr(rc);
        freeCodec();
        return false;
    }
    return true;
}

int VideoDecoder::pickFormat(AVCodecContext* ctx, const int* formats) {
    auto* self = static_cast<VideoDecoder*>(ctx->opaque);
    for (const int* f = formats; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == self->hwPixFmt_) return *f;
    // The device cannot decode this stream (profile, size...): software it is.
    self->hwName_.clear();
    for (const int* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(*f));
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    return formats[0];
}

bool VideoDecoder::decodeNext(AVFrame* into) {
    for (;;) {
        int rc = avcodec_receive_frame(ctx_, into);
        if (rc == 0 && hwPixFmt_ >= 0 && into->format == hwPixFmt_) {
            // Copy the hardware frame to memory for the CPU pipeline.
            if (!hwTransfer_) hwTransfer_ = av_frame_alloc();
            av_frame_unref(hwTransfer_);
            if (av_hwframe_transfer_data(hwTransfer_, into, 0) < 0 || av_frame_copy_props(hwTransfer_, into) < 0) {
                hwBroken_ = true;  // frameAt reopens the stream in software
                av_frame_unref(into);
                return false;
            }
            av_frame_unref(into);
            av_frame_move_ref(into, hwTransfer_);
        }
        if (rc == 0) return true;
        if (rc == AVERROR_EOF) return false;
        if (rc != AVERROR(EAGAIN)) return false;
        if (eof_) return false;
        rc = av_read_frame(fmt_, pkt_);
        if (rc < 0) {
            eof_ = true;
            avcodec_send_packet(ctx_, nullptr);  // drain
            continue;
        }
        if (pkt_->stream_index == stream_) avcodec_send_packet(ctx_, pkt_);
        av_packet_unref(pkt_);
    }
}

bool VideoDecoder::seek(double t) {
    int64_t ts = int64_t(std::floor((t + origin_) / timeBase_));
    int rc = av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) rc = av_seek_frame(fmt_, -1, int64_t((t + origin_) * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx_);
    eof_ = false;
    haveCur_ = haveNext_ = false;
    curPts_ = nextPts_ = -1;
    return rc >= 0;
}

void setFieldDominance(AVFrame* f, int dominance) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    f->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    if (dominance) f->flags |= AV_FRAME_FLAG_INTERLACED | (dominance == 1 ? AV_FRAME_FLAG_TOP_FIELD_FIRST : 0);
#else
    f->interlaced_frame = dominance != 0;
    f->top_field_first = dominance == 1;
#endif
}

void interpretAlpha(Frame16& f, const std::string& mode) {
    const bool ignore = mode == "ignore", invert = mode == "invert", premultiplied = mode == "premultiplied";
    if (!ignore && !invert && !premultiplied) return;
    parallelRows(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t* p = f.px.data() + size_t(y) * size_t(f.width) * 4;
            for (int x = 0; x < f.width; ++x, p += 4) {
                if (ignore) p[3] = 65535;
                else if (invert) p[3] = uint16_t(65535 - p[3]);
                else if (p[3] == 0) p[0] = p[1] = p[2] = 0;
                else if (p[3] < 65535)  // colour that was multiplied by alpha, back to straight
                    for (int c = 0; c < 3; ++c) p[c] = uint16_t(std::min<uint32_t>(65535, (uint32_t(p[c]) * 65535 + p[3] / 2) / p[3]));
            }
        }
    });
}

int fieldDominance(const AVFrame* f) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    if (!(f->flags & AV_FRAME_FLAG_INTERLACED)) return 0;
    return (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 1 : 2;
#else
    if (!f->interlaced_frame) return 0;
    return f->top_field_first ? 1 : 2;
#endif
}

namespace {

template <typename T>
bool deinterlacePlane(uint8_t* data, int linesize, int width, int height, bool keepTop, int threshold) {
    bool changed = false;
    for (int y = keepTop ? 1 : 0; y < height; y += 2) {
        T* c = reinterpret_cast<T*>(data + size_t(y) * size_t(linesize));
        const T* a = reinterpret_cast<const T*>(data + size_t(y > 0 ? y - 1 : y + 1) * size_t(linesize));
        const T* b = reinterpret_cast<const T*>(data + size_t(y + 1 < height ? y + 1 : y - 1) * size_t(linesize));
        for (int x = 0; x < width; ++x) {
            const int lo = std::min<int>(a[x], b[x]) - threshold, hi = std::max<int>(a[x], b[x]) + threshold;
            if (c[x] < lo || c[x] > hi) {
                c[x] = T((int(a[x]) + int(b[x]) + 1) / 2);
                changed = true;
            }
        }
    }
    return changed;
}

}  // namespace

bool deinterlaceFrame(AVFrame* f) {
    const int dominance = fieldDominance(f);
    if (!dominance || f->height < 3) return false;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_RGB)) ||
        d->comp[0].depth > 16 || av_frame_make_writable(f) < 0)
        return false;
    const bool wide = d->comp[0].depth > 8;
    const int threshold = 8 << std::max(0, d->comp[0].depth - 8);
    bool changed = false;
    for (int p = 0; p < 4 && f->data[p]; ++p) {
        const int bytes = av_image_get_linesize(AVPixelFormat(f->format), f->width, p);
        if (bytes <= 0) continue;
        const int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) : f->height;
        changed |= wide ? deinterlacePlane<uint16_t>(f->data[p], f->linesize[p], bytes / 2, rows, dominance == 1, threshold)
                        : deinterlacePlane<uint8_t>(f->data[p], f->linesize[p], bytes, rows, dominance == 1, threshold);
    }
    return changed;
}

Frame16Ptr VideoDecoder::convert(const AVFrame* in, double pts, int w, int h, bool hq) {
    // ProRes RAW is developed first (RGB48, ACEScct), at half size when it is shown that small.
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> developed(nullptr, [](AVFrame* fr) { av_frame_free(&fr); });
    if (proResRaw_) {
        const int tw = w > 0 ? w : dispW_, th = h > 0 ? h : dispH_;
        developed.reset(developProResRawFrame(in, rawSettings_, rotation_ % 180 ? th : tw, rotation_ % 180 ? tw : th));
        if (developed) in = developed.get();
    }
    // Interlaced pictures are deinterlaced on a copy first, in their own format.
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> deint(nullptr, [](AVFrame* fr) { av_frame_free(&fr); });
    const AVFrame* f = in;
    // Interpret Footage can say the frames are progressive, or which field comes first, whatever their flags say.
    const int dominance = fields_ == "progressive" ? 0 : fields_ == "upper" ? 1 : fields_ == "lower" ? 2 : fieldDominance(in);
    if (dominance) {
        deint.reset(av_frame_clone(in));
        if (deint) setFieldDominance(deint.get(), dominance);
        if (deint && deinterlaceFrame(deint.get())) f = deint.get();
    }
    // Stereoscopic footage: the eye's half of the frame (squeezed halves are stretched back by the scaling below).
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> eyeFrame(nullptr, [](AVFrame* fr) { av_frame_free(&fr); });
    if (!stereo_.empty() && f->width > 1 && f->height > 1) {
        eyeFrame.reset(av_frame_clone(f));
        if (AVFrame* e = eyeFrame.get()) {
            // Where the eyes meet, on a whole chroma sample (an odd offset would split packed 4:2:2 pairs).
            const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(AVPixelFormat(e->format));
            if (stereo_.rfind("sbs", 0) == 0) {
                int half = e->width / 2;
                if (pd && pd->log2_chroma_w > 0) half &= ~1;
                if (eye_) e->crop_left += size_t(half);
                else e->crop_right += size_t(e->width - half);
            } else {
                int half = e->height / 2;
                if (pd && pd->log2_chroma_h > 0) half &= ~1;
                if (eye_) e->crop_top += size_t(half);
                else e->crop_bottom += size_t(e->height - half);
            }
            if (av_frame_apply_cropping(e, AV_FRAME_CROP_UNALIGNED) == 0) f = e;
        }
    }
    if (w <= 0) w = dispW_;
    if (h <= 0) h = dispH_;
    int sw = (rotation_ % 180) ? h : w;
    int sh = (rotation_ % 180) ? w : h;
    bool fullRange = f->color_range == AVCOL_RANGE_JPEG;
    AVPixelFormat srcFmt = dejpeg(AVPixelFormat(f->format), fullRange);
    const int flags = hq ? (SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT) : SWS_BILINEAR;
    const int key[6] = {f->width, f->height, int(srcFmt), sw, sh, flags};
    if (!sws_ || !std::equal(key, key + 6, swsKey_)) {
        // A slice-threaded context: converting a 4K frame to 16-bit RGBA is
        // otherwise a large share of a frame's time on one core.
        if (sws_) sws_freeContext(sws_);
        sws_ = sws_alloc_context();
        if (!sws_) return nullptr;
        av_opt_set_int(sws_, "srcw", f->width, 0);
        av_opt_set_int(sws_, "srch", f->height, 0);
        av_opt_set_int(sws_, "src_format", srcFmt, 0);
        av_opt_set_int(sws_, "dstw", sw, 0);
        av_opt_set_int(sws_, "dsth", sh, 0);
        av_opt_set_int(sws_, "dst_format", AV_PIX_FMT_RGBA64LE, 0);
        av_opt_set_int(sws_, "sws_flags", flags, 0);
        av_opt_set_int(sws_, "threads", std::clamp(int(std::thread::hardware_concurrency()), 1, 8), 0);
        if (sws_init_context(sws_, nullptr, nullptr) < 0) {
            sws_freeContext(sws_);
            sws_ = nullptr;
            return nullptr;
        }
        std::copy(key, key + 6, swsKey_);
    }
    int cs = SWS_CS_DEFAULT;
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: cs = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: cs = SWS_CS_ITU601; break;
        case AVCOL_SPC_SMPTE240M: cs = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
        default: cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(srcFmt);
    bool isRgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
    if (!isRgb)
        sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT),
                                 1, 0, 1 << 16, 1 << 16);
    auto out = std::make_shared<Frame16>();
    out->width = sw;
    out->height = sh;
    out->pts = pts;
    out->px.resize(size_t(sw) * size_t(sh) * 4);
    uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(out->px.data()), nullptr, nullptr, nullptr};
    int dstStride[4] = {sw * 8, 0, 0, 0};
    // The threaded path needs reference-counted frames (decoders give those);
    // the source is relabelled with the format the context was made for.
    bool converted = false;
    if (f->buf[0]) {
        AVFrame* src = av_frame_clone(f);
        AVFrame* dstFrame = av_frame_alloc();
        if (src && dstFrame) {
            src->format = srcFmt;
            dstFrame->format = AV_PIX_FMT_RGBA64LE;
            dstFrame->width = sw;
            dstFrame->height = sh;
            // Our buffer, wrapped without ownership (a frame without buf[0] would be
            // given a buffer of the scaler's own).
            dstFrame->buf[0] = av_buffer_create(dst[0], size_t(dstStride[0]) * size_t(sh), [](void*, uint8_t*) {}, nullptr, 0);
            dstFrame->data[0] = dst[0];
            dstFrame->linesize[0] = dstStride[0];
            converted = dstFrame->buf[0] && sws_scale_frame(sws_, dstFrame, src) >= 0 && dstFrame->data[0] == dst[0];
        }
        av_frame_free(&src);
        av_frame_free(&dstFrame);
    }
    if (!converted) sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
    if (!alpha_.empty()) interpretAlpha(*out, alpha_);
    out->pts = pts / timeScale_;
    if (rotation_) {
        auto rotated = std::make_shared<Frame16>(rotateFrame(*out, rotation_));
        rotated->pts = out->pts;
        return rotated;
    }
    return out;
}

bool VideoDecoder::loadRaw(const RawImage& img, std::string* error) {
    if (!raw_ || raw_->width != img.width || raw_->height != img.height) {
        if (raw_) av_frame_free(&raw_);
        raw_ = av_frame_alloc();
        if (!raw_) return false;
        raw_->format = AV_PIX_FMT_RGB48;  // host byte order, as LibRaw writes it
        raw_->width = img.width;
        raw_->height = img.height;
        raw_->color_range = AVCOL_RANGE_JPEG;
        if (av_frame_get_buffer(raw_, 0) < 0) {
            if (error) *error = "Out of memory developing " + path_;
            av_frame_free(&raw_);
            return false;
        }
    }
    for (int y = 0; y < img.height; ++y)
        std::memcpy(raw_->data[0] + size_t(y) * size_t(raw_->linesize[0]), img.rgb.data() + size_t(y) * size_t(img.width) * 3,
                    size_t(img.width) * 3 * sizeof(uint16_t));
    return true;
}

Frame16Ptr VideoDecoder::frameAt(double t, int targetW, int targetH, bool highQuality) {
    if (rawSequence_) {
        // The frame of the run shown at `t`, developed once and kept while it is asked for again.
        const ImageSequence& seq = *rawSequence_;
        const int n = seq.first + std::clamp(int(std::floor(std::max(0.0, t) * fps_ + 1e-6)), 0, seq.frames() - 1);
        if (targetW <= 0) targetW = dispW_;
        if (targetH <= 0) targetH = dispH_;
        if (n != rawFrame_) {
            RawImage img;
            std::string err;
            if (!developRaw(imageSequenceFrame(seq, n), img, &err, rawSettings_) || !loadRaw(img, &err)) return nullptr;
            rawFrame_ = n;
            stillFrame_.reset();
        }
        if (!stillFrame_ || stillFrame_->width != targetW || stillFrame_->height != targetH)
            stillFrame_ = convert(raw_, double(n - seq.first) / fps_, targetW, targetH, true);
        curPts_ = double(n - seq.first) / fps_;
        return stillFrame_;
    }
    if (vector_) {
        Frame16Ptr f = renderVector(*vector_, t, targetW, targetH);
        if (f) curPts_ = f->pts;
        return f;
    }
    if (raw_) {
        if (targetW <= 0) targetW = dispW_;
        if (targetH <= 0) targetH = dispH_;
        if (!stillFrame_ || stillFrame_->width != targetW || stillFrame_->height != targetH) stillFrame_ = convert(raw_, 0, targetW, targetH, true);
        curPts_ = 0;
        return stillFrame_;
    }
    t *= timeScale_;  // a conformed file's own time
    if (hwBroken_) {
        // A hardware frame could not be read back: continue in software.
        hwBroken_ = false;
        freeCodec();
        if (!openCodec(false, nullptr)) return nullptr;
        seek(std::max(0.0, t));
    }
    if (!ctx_) return nullptr;
    if (targetW <= 0) targetW = dispW_;
    if (targetH <= 0) targetH = dispH_;

    if (still_) {
        if (!stillFrame_ || stillFrame_->width != targetW || stillFrame_->height != targetH) {
            if (!haveCur_) {
                // The picture as opened; else sought back to; else from a fresh open
                // (FFmpeg's image demuxer reads a JPEG as finished after a seek).
                haveCur_ = decodeNext(cur_);
                if (!haveCur_) {
                    seek(0);
                    haveCur_ = decodeNext(cur_);
                }
                if (!haveCur_ && open(std::string(path_), nullptr)) haveCur_ = decodeNext(cur_);
                if (haveCur_) curPts_ = 0;
            }
            if (!haveCur_) return nullptr;
            stillFrame_ = convert(cur_, 0, targetW, targetH, true);
        }
        return stillFrame_;
    }

    const double fd = 1.0 / std::max(1.0, fps_);
    const double eps = fd * 0.25;
    t = std::max(0.0, t);
    auto ptsOf = [&](const AVFrame* f, double prev) {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts == AV_NOPTS_VALUE) return prev < 0 ? 0.0 : prev + fd;
        return double(ts) * timeBase_ - origin_;
    };

    // Same frame as last time?
    if (haveCur_ && curPts_ <= t + eps && haveNext_ && nextPts_ > t + eps)
        return convert(cur_, curPts_, targetW, targetH, highQuality);

    bool sequential = haveCur_ && t + eps >= curPts_ && t - curPts_ < 2.0;
    if (!sequential) {
        auto isKey = [](const AVFrame* f) {
#ifdef AV_FRAME_FLAG_KEY
            return (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
            return f->key_frame != 0;
#endif
        };
        static const double backoff[] = {0.0, 1.0, 3.0, 10.0, 1e9};
        for (double b : backoff) {
            double from = std::max(0.0, t - b);
            seek(from);
            // Some containers (e.g. MPEG-TS) seek to packets that are not
            // keyframes, or past the last decodable frame. Skip to the first
            // keyframe; if none arrives before the target, retry from earlier.
            bool gotKey = false;
            while (decodeNext(next_)) {
                double pts = ptsOf(next_, -1);
                if (isKey(next_) || from <= 0) {
                    gotKey = true;
                    nextPts_ = pts;
                    break;
                }
                av_frame_unref(next_);
                if (pts > t + eps) break;
            }
            if (!gotKey) continue;
            haveNext_ = true;
            if (nextPts_ <= t + eps || from <= 0) break;  // landed before the target (or at the start)
        }
    }

    for (;;) {
        if (!haveNext_) {
            if (!decodeNext(next_)) break;
            haveNext_ = true;
            nextPts_ = ptsOf(next_, haveCur_ ? curPts_ : -1);
        }
        if (nextPts_ <= t + eps) {
            std::swap(cur_, next_);
            haveCur_ = true;
            curPts_ = nextPts_;
            haveNext_ = false;
            av_frame_unref(next_);
        } else {
            break;
        }
    }
    if (haveCur_) return convert(cur_, curPts_, targetW, targetH, highQuality);
    if (haveNext_) return convert(next_, nextPts_, targetW, targetH, highQuality);
    return nullptr;
}

Frame16 rotateFrame(const Frame16& f, int degrees) {
    Frame16 out;
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees == 0) return f;
    bool swap = degrees % 180 != 0;
    out.width = swap ? f.height : f.width;
    out.height = swap ? f.width : f.height;
    out.pts = f.pts;
    out.px.resize(f.px.size());
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            int nx, ny;
            if (degrees == 90) {
                nx = f.height - 1 - y;
                ny = x;
            } else if (degrees == 180) {
                nx = f.width - 1 - x;
                ny = f.height - 1 - y;
            } else {
                nx = y;
                ny = f.width - 1 - x;
            }
            std::memcpy(&out.px[(size_t(ny) * size_t(out.width) + size_t(nx)) * 4],
                        &f.px[(size_t(y) * size_t(f.width) + size_t(x)) * 4], 8);
        }
    return out;
}

// ---------------------------------------------------------------------------
// Audio

namespace {

// One audio stream decoded whole to float at `sampleRate`, aligned so sample 0 is media time 0: the best stream
// mixed to stereo (ordinal -1), or the ordinal-th audio stream with its own channels. `channels` says how many.
bool decodeAudioStream(const std::string& path, int sampleRate, int ordinal, std::vector<float>& samples, int& channels,
                       std::string* error, const std::atomic<bool>* cancel, AVChannelLayout* layoutOut = nullptr) {
    AVFormatContext* fmt = nullptr;
    int rc = openMediaInput(&fmt, path);
    if (rc < 0) {
        if (error) *error = averr(rc);
        return false;
    }
    struct Cleanup {
        AVFormatContext** fmt;
        AVCodecContext* ctx = nullptr;
        SwrContext* swr = nullptr;
        AVPacket* pkt = nullptr;
        AVFrame* frame = nullptr;
        ~Cleanup() {
            av_frame_free(&frame);
            av_packet_free(&pkt);
            swr_free(&swr);
            avcodec_free_context(&ctx);
            avformat_close_input(fmt);
        }
    } c{&fmt};
    if ((rc = avformat_find_stream_info(fmt, nullptr)) < 0) {
        if (error) *error = averr(rc);
        return false;
    }
    const AVCodec* codec = nullptr;
    int stream = -1;
    if (ordinal < 0) {
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    } else {
        for (unsigned i = 0, n = 0; i < fmt->nb_streams && stream < 0; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && int(n++) == ordinal) stream = int(i);
        if (stream >= 0) codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    }
    if (stream < 0 || !codec) {
        if (error) *error = "No audio stream";
        return false;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != stream) fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream* st = fmt->streams[stream];
    c.ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c.ctx, st->codecpar);
    c.ctx->pkt_timebase = st->time_base;
    if ((rc = avcodec_open2(c.ctx, codec, nullptr)) < 0) {
        if (error) *error = averr(rc);
        return false;
    }
    if (c.ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&c.ctx->ch_layout, std::max(1, c.ctx->ch_layout.nb_channels));
    if (layoutOut) av_channel_layout_copy(layoutOut, &c.ctx->ch_layout);
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    const AVChannelLayout* outLayout = ordinal < 0 ? &stereo : &c.ctx->ch_layout;
    channels = outLayout->nb_channels;
    rc = swr_alloc_set_opts2(&c.swr, outLayout, AV_SAMPLE_FMT_FLT, sampleRate, &c.ctx->ch_layout, c.ctx->sample_fmt,
                             c.ctx->sample_rate, 0, nullptr);
    if (rc < 0 || swr_init(c.swr) < 0) {
        if (error) *error = "Cannot initialise resampler";
        return false;
    }
    c.pkt = av_packet_alloc();
    c.frame = av_frame_alloc();
    const size_t nch = size_t(channels);
    samples.clear();
    if (st->duration > 0) samples.reserve(size_t(double(st->duration) * av_q2d(st->time_base) * sampleRate * double(nch) + 4096));
    const double origin = fileOrigin(fmt);
    const double tb = av_q2d(st->time_base);
    bool first = true;
    int64_t dropLeading = 0;
    std::vector<float> tmp;

    auto append = [&](const uint8_t** in, int nb) {
        int maxOut = swr_get_out_samples(c.swr, nb);
        if (maxOut <= 0) return;
        tmp.resize(size_t(maxOut) * nch);
        uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
        int got = swr_convert(c.swr, outPtr, maxOut, in, nb);
        if (got <= 0) return;
        int64_t start = 0;
        if (dropLeading > 0) {
            start = std::min<int64_t>(dropLeading, got);
            dropLeading -= start;
        }
        samples.insert(samples.end(), tmp.begin() + start * int64_t(nch), tmp.begin() + int64_t(got) * int64_t(nch));
    };
    auto handleFrame = [&]() {
        if (first) {
            first = false;
            int64_t ts = c.frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = c.frame->pts;
            double startSec = ts == AV_NOPTS_VALUE ? 0.0 : double(ts) * tb - origin;
            int64_t offset = int64_t(std::llround(startSec * sampleRate));
            if (offset > 0) samples.insert(samples.end(), size_t(offset) * nch, 0.0f);
            else dropLeading = -offset;
        }
        append(const_cast<const uint8_t**>(c.frame->extended_data), c.frame->nb_samples);
    };

    bool eof = false;
    while (true) {
        if (cancel && cancel->load()) return false;
        rc = avcodec_receive_frame(c.ctx, c.frame);
        if (rc == 0) {
            handleFrame();
            continue;
        }
        if (rc == AVERROR_EOF) break;
        if (rc != AVERROR(EAGAIN)) break;
        if (eof) break;
        rc = av_read_frame(fmt, c.pkt);
        if (rc < 0) {
            eof = true;
            avcodec_send_packet(c.ctx, nullptr);
            continue;
        }
        if (c.pkt->stream_index == stream) avcodec_send_packet(c.ctx, c.pkt);
        av_packet_unref(c.pkt);
    }
    append(nullptr, 0);  // flush the resampler
    samples.shrink_to_fit();
    return true;
}

}  // namespace

void checkStereoMedia(Project& p) {
    // ProRes RAW media read by another build: developed to ACEScct where this FFmpeg gives its colour, left to FFmpeg
    // (and not ACEScct) where it does not; their size and colour space come from probing again.
    for (MediaItem& m : p.media) {
        if (m.videoCodec != "prores_raw" || m.path.empty() || m.subclipOf || (m.colorSpace == "acescct") == proResRawAvailable()) continue;
        MediaItem fresh = m;
        fresh.colorSpace.clear();
        if (!probeMedia(m.path, fresh)) continue;
        m.colorSpace = fresh.colorSpace;
        m.width = fresh.width;
        m.height = fresh.height;
        for (MediaItem& sub : p.media)
            if (sub.subclipOf == m.id) sub.colorSpace = m.colorSpace, sub.width = m.width, sub.height = m.height;
    }
    if (p.stereoChecked) return;
    p.stereoChecked = true;
    for (MediaItem& m : p.media) {
        if (m.kind != MediaKind::Video || m.path.empty() || m.subclipOf || !m.stereo.empty()) continue;
        // The header alone says (no stream analysis), so a project of many files opens quickly.
        AVFormatContext* fmt = nullptr;
        if (openMediaInput(&fmt, m.path) < 0) continue;
        std::string layout;
        for (unsigned i = 0; i < fmt->nb_streams && layout.empty(); ++i) {
            const AVStream* st = fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
            bool inverted = false;
            Interpretation in;
            parseInterpretation(m.path, in);
            layout = stereoLayout(streamStereo(st, inverted, !streamProjection(st).empty()), in);
        }
        avformat_close_input(&fmt);
        MediaItem fresh;
        if (layout.empty() || !probeMedia(m.path, fresh)) continue;
        m.stereo = fresh.stereo;
        m.width = fresh.width;
        m.height = fresh.height;
        for (MediaItem& sub : p.media)
            if (sub.subclipOf == m.id) sub.stereo = m.stereo, sub.width = m.width, sub.height = m.height;
    }
}

AudioBufferPtr decodeAmbisonic(const std::string& path, int sampleRate, std::string* error, const std::atomic<bool>* cancel) {
    // A conformed file plays at its new speed, or stretched to its new length with its pitch kept, as decodeAudio does.
    if (Interpretation in; parseInterpretation(path, in) && in.conformed()) {
        const double k = in.timeScale();
        if (!in.keepPitch) {
            const int rate = int(std::clamp(std::llround(sampleRate / k), 1000LL, 1LL << 30));
            AudioBufferPtr played = decodeAmbisonic(uninterpretedPath(path), rate, error, cancel);
            if (!played) return nullptr;
            auto out = std::make_shared<AudioBuffer>(*played);
            out->sampleRate = sampleRate;
            return out;
        }
        AudioBufferPtr src = decodeAmbisonic(uninterpretedPath(path), sampleRate, error, cancel);
        if (!src) return nullptr;
        const int hop = stretchHop(sampleRate);
        const int64_t outFrames = int64_t(std::llround(double(src->frames()) / k));
        std::vector<double> positions(size_t(outFrames / hop + 2));
        for (size_t j = 0; j < positions.size(); ++j) positions[j] = double(j) * hop * k;
        auto out = std::make_shared<AudioBuffer>();
        wsolaStretch(*src, positions, hop, outFrames, *out);
        out->sampleRate = sampleRate;
        return out;
    }
    // The stream the stereo mix comes from, with its own channels.
    int ordinal = -1;
    {
        AVFormatContext* fmt = nullptr;
        if (int rc = openMediaInput(&fmt, path); rc < 0) {
            if (error) *error = averr(rc);
            return nullptr;
        }
        if (avformat_find_stream_info(fmt, nullptr) >= 0) {
            const int best = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            for (unsigned i = 0, k = 0; best >= 0 && i < fmt->nb_streams; ++i) {
                if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                if (int(i) == best) {
                    ordinal = int(k);
                    break;
                }
                ++k;
            }
        }
        avformat_close_input(&fmt);
    }
    if (ordinal < 0) {
        if (error) *error = "No audio stream";
        return nullptr;
    }
    std::vector<float> raw;
    int n = 0;
    AVChannelLayout layout{};
    if (!decodeAudioStream(path, sampleRate, ordinal, raw, n, error, cancel, &layout)) return nullptr;
    if (n < kFoaChannels) {
        av_channel_layout_uninit(&layout);
        if (error) *error = "Ambisonic sound needs at least four channels";
        return nullptr;
    }
    // ACN order (W, Y, Z, X) as decoded: a decoder that names four channels as speakers (AAC's 4.0) gives them in its
    // own channel order, which is the order FFmpeg's encoders took them in (and Montage's exports write).
    const int map[4] = {0, 1, 2, 3};
    av_channel_layout_uninit(&layout);
    auto buf = std::make_shared<AudioBuffer>();
    buf->sampleRate = sampleRate;
    buf->channels = kFoaChannels;
    const size_t frames = raw.size() / size_t(n);
    buf->samples.resize(frames * 4);
    for (size_t i = 0; i < frames; ++i)
        for (int k = 0; k < 4; ++k) buf->samples[i * 4 + size_t(k)] = raw[i * size_t(n) + size_t(map[k])];
    return buf;
}

AudioBufferPtr decodeAudio(const std::string& path, int sampleRate, std::string* error, const std::atomic<bool>* cancel,
                           const std::vector<int>& channels) {
    // A conformed video's sound plays with its frames: at the new speed, or stretched to the new length with its pitch
    // kept (core/Interpretation.h).
    if (Interpretation in; parseInterpretation(path, in) && in.conformed()) {
        const double k = in.timeScale();  // seconds of the file per second of media time
        const std::string file = uninterpretedPath(path);
        if (!in.keepPitch) {
            // Read as if recorded at sampleRate / k: each second of the file becomes 1 / k seconds here.
            const int rate = int(std::clamp(std::llround(sampleRate / k), 1000LL, 1LL << 30));
            AudioBufferPtr played = decodeAudio(file, rate, error, cancel, channels);
            if (!played) return nullptr;
            auto out = std::make_shared<AudioBuffer>(*played);
            out->sampleRate = sampleRate;
            return out;
        }
        AudioBufferPtr src = decodeAudio(file, sampleRate, error, cancel, channels);
        if (!src) return nullptr;
        const int hop = stretchHop(sampleRate);
        const int64_t outFrames = int64_t(std::llround(double(src->frames()) / k));
        std::vector<double> positions(size_t(outFrames / hop + 2));
        for (size_t j = 0; j < positions.size(); ++j) positions[j] = double(j) * hop * k;
        auto out = std::make_shared<AudioBuffer>();
        wsolaStretch(*src, positions, hop, outFrames, *out);
        out->sampleRate = sampleRate;
        return out;
    }
    auto buf = std::make_shared<AudioBuffer>();
    buf->sampleRate = sampleRate;
    int n = 0;
    if (channels.empty()) {
        if (!decodeAudioStream(path, sampleRate, -1, buf->samples, n, error, cancel)) return nullptr;
        return buf;
    }
    // Chosen channels: each audio stream they come from, decoded with its own channels, then routed to stereo.
    std::vector<int> streams;  // channels per audio stream
    {
        AVFormatContext* fmt = nullptr;
        if (int rc = openMediaInput(&fmt, path); rc < 0) {
            if (error) *error = averr(rc);
            return nullptr;
        }
        if (avformat_find_stream_info(fmt, nullptr) >= 0)
            for (unsigned i = 0; i < fmt->nb_streams; ++i)
                if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                    streams.push_back(std::max(1, fmt->streams[i]->codecpar->ch_layout.nb_channels));
        avformat_close_input(&fmt);
    }
    std::map<int, std::pair<std::vector<float>, int>> decoded;  // stream ordinal -> samples, channels
    auto locate = [&](int ch, int& stream, int& local) {
        for (stream = 0; stream < int(streams.size()); ++stream) {
            if (ch < streams[size_t(stream)]) return (local = ch, true);
            ch -= streams[size_t(stream)];
        }
        return false;
    };
    for (int ch : channels) {
        int stream = 0, local = 0;
        if (ch < 0 || !locate(ch, stream, local)) {
            if (error) *error = "The file has no audio channel " + std::to_string(ch + 1);
            return nullptr;
        }
        if (decoded.count(stream)) continue;
        auto& [samples, count] = decoded[stream];
        if (!decodeAudioStream(path, sampleRate, stream, samples, count, error, cancel)) return nullptr;
    }
    int64_t frames = 0;
    for (const auto& [stream, d] : decoded) frames = std::max<int64_t>(frames, int64_t(d.first.size()) / std::max(1, d.second));
    buf->samples.assign(size_t(frames) * 2, 0.0f);
    for (size_t k = 0; k < channels.size(); ++k) {
        int stream = 0, local = 0;
        locate(channels[k], stream, local);
        const auto& [samples, count] = decoded.at(stream);
        const int64_t have = int64_t(samples.size()) / std::max(1, count);
        const bool centre = channels.size() == 1;
        for (int64_t i = 0; i < have; ++i) {
            const float v = samples[size_t(i * count + local)];
            if (centre || k % 2 == 0) buf->samples[size_t(i) * 2] += v;
            if (centre || k % 2 == 1) buf->samples[size_t(i) * 2 + 1] += v;
        }
    }
    return buf;
}

PeaksPtr computePeaks(const AudioBuffer& buf, int samplesPerBucket) {
    auto p = std::make_shared<Peaks>();
    p->samplesPerBucket = std::max(1, samplesPerBucket);
    p->sampleRate = buf.sampleRate;
    int64_t frames = buf.frames();
    int64_t buckets = (frames + p->samplesPerBucket - 1) / p->samplesPerBucket;
    p->minmax.resize(size_t(buckets) * 2);
    for (int64_t b = 0; b < buckets; ++b) {
        float lo = 0, hi = 0;
        int64_t s0 = b * p->samplesPerBucket, s1 = std::min(frames, s0 + p->samplesPerBucket);
        const size_t nch = size_t(std::max(1, buf.channels));
        for (int64_t s = s0; s < s1; ++s) {
            // Stereo: the two sides' mean; an ambisonic field: its omnidirectional W.
            float v = nch == 2 ? 0.5f * (buf.samples[size_t(s) * 2] + buf.samples[size_t(s) * 2 + 1]) : buf.samples[size_t(s) * nch];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        p->minmax[size_t(b) * 2] = lo;
        p->minmax[size_t(b) * 2 + 1] = hi;
    }
    return p;
}

}  // namespace montage
