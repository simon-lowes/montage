#include "Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
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

bool probeMedia(const std::string& path, MediaItem& out, std::string* error) {
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (error) *error = "Cannot open " + path + ": " + averr(rc);
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
    if (m.name.empty()) m.name = path.substr(path.find_last_of("/\\") + 1);
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
        if (m.colorSpace == "rec709") m.colorSpace.clear();
        int w = st->codecpar->width, h = st->codecpar->height;
        AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = int(std::lround(double(w) * sar.num / sar.den));
        if (streamRotation(st) % 180 != 0) std::swap(w, h);
        m.width = w;
        m.height = h;
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        if (fr.num > 0 && fr.den > 0) m.fps = Rational{fr.num, fr.den};
        // Start timecode (camera files carry it in the stream, the container or a tmcd track).
        const AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0);
        if (!tc) tc = av_dict_get(fmt->metadata, "timecode", nullptr, 0);
        for (unsigned i = 0; !tc && i < fmt->nb_streams; ++i) tc = av_dict_get(fmt->streams[i]->metadata, "timecode", nullptr, 0);
        AVTimecode parsed;
        if (tc && fr.num > 0 && fr.den > 0 && av_timecode_init_from_string(&parsed, fr, tc->value, nullptr) == 0)
            m.timecode = double(parsed.start) * fr.den / fr.num;
        if (dur <= 0 && st->duration > 0) dur = double(st->duration) * av_q2d(st->time_base);
    }
    if (m.hasAudio) {
        AVStream* st = fmt->streams[a];
        const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
        m.audioCodec = d ? d->name : "unknown";
        m.sampleRate = st->codecpar->sample_rate;
        m.channels = st->codecpar->ch_layout.nb_channels;
        if (dur <= 0 && st->duration > 0) dur = double(st->duration) * av_q2d(st->time_base);
    }
    if (m.hasVideo && isStillFormat(fmt)) {
        m.kind = MediaKind::Image;
        m.duration = 0;
        m.hasAudio = false;
    } else if (m.hasVideo) {
        m.kind = MediaKind::Video;
        m.duration = dur;
    } else {
        m.kind = MediaKind::Audio;
        m.duration = dur;
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
}

bool VideoDecoder::open(const std::string& path, std::string* error) {
    close();
    path_ = path;
    int rc = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
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
    if (!openCodec(hwDecodeMode() == HwDecodeMode::Auto && !still_, error)) {
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
    AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = int(std::lround(double(w) * sar.num / sar.den));
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

Frame16Ptr VideoDecoder::convert(const AVFrame* f, double pts, int w, int h, bool hq) {
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
    if (rotation_) {
        auto rotated = std::make_shared<Frame16>(rotateFrame(*out, rotation_));
        rotated->pts = pts;
        return rotated;
    }
    return out;
}

Frame16Ptr VideoDecoder::frameAt(double t, int targetW, int targetH, bool highQuality) {
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
                seek(0);
                if (decodeNext(cur_)) {
                    haveCur_ = true;
                    curPts_ = 0;
                }
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

AudioBufferPtr decodeAudio(const std::string& path, int sampleRate, std::string* error,
                           const std::atomic<bool>* cancel) {
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (error) *error = averr(rc);
        return nullptr;
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
        return nullptr;
    }
    const AVCodec* codec = nullptr;
    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        if (error) *error = "No audio stream";
        return nullptr;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != stream) fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream* st = fmt->streams[stream];
    c.ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c.ctx, st->codecpar);
    c.ctx->pkt_timebase = st->time_base;
    if ((rc = avcodec_open2(c.ctx, codec, nullptr)) < 0) {
        if (error) *error = averr(rc);
        return nullptr;
    }
    if (c.ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&c.ctx->ch_layout, std::max(1, c.ctx->ch_layout.nb_channels));
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    rc = swr_alloc_set_opts2(&c.swr, &stereo, AV_SAMPLE_FMT_FLT, sampleRate, &c.ctx->ch_layout, c.ctx->sample_fmt,
                             c.ctx->sample_rate, 0, nullptr);
    if (rc < 0 || swr_init(c.swr) < 0) {
        if (error) *error = "Cannot initialise resampler";
        return nullptr;
    }
    c.pkt = av_packet_alloc();
    c.frame = av_frame_alloc();
    auto buf = std::make_shared<AudioBuffer>();
    buf->sampleRate = sampleRate;
    if (st->duration > 0)
        buf->samples.reserve(size_t(double(st->duration) * av_q2d(st->time_base) * sampleRate * 2.0 + 4096));
    const double origin = fileOrigin(fmt);
    const double tb = av_q2d(st->time_base);
    bool first = true;
    int64_t dropLeading = 0;
    std::vector<float> tmp;

    auto append = [&](const uint8_t** in, int nb) {
        int maxOut = swr_get_out_samples(c.swr, nb);
        if (maxOut <= 0) return;
        tmp.resize(size_t(maxOut) * 2);
        uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
        int got = swr_convert(c.swr, outPtr, maxOut, in, nb);
        if (got <= 0) return;
        int64_t start = 0;
        if (dropLeading > 0) {
            start = std::min<int64_t>(dropLeading, got);
            dropLeading -= start;
        }
        buf->samples.insert(buf->samples.end(), tmp.begin() + start * 2, tmp.begin() + int64_t(got) * 2);
    };
    auto handleFrame = [&]() {
        if (first) {
            first = false;
            int64_t ts = c.frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = c.frame->pts;
            double startSec = ts == AV_NOPTS_VALUE ? 0.0 : double(ts) * tb - origin;
            int64_t offset = int64_t(std::llround(startSec * sampleRate));
            if (offset > 0) buf->samples.insert(buf->samples.end(), size_t(offset) * 2, 0.0f);
            else dropLeading = -offset;
        }
        append(const_cast<const uint8_t**>(c.frame->extended_data), c.frame->nb_samples);
    };

    bool eof = false;
    while (true) {
        if (cancel && cancel->load()) return nullptr;
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
    buf->samples.shrink_to_fit();
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
        for (int64_t s = s0; s < s1; ++s) {
            float v = 0.5f * (buf.samples[size_t(s) * 2] + buf.samples[size_t(s) * 2 + 1]);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        p->minmax[size_t(b) * 2] = lo;
        p->minmax[size_t(b) * 2 + 1] = hi;
    }
    return p;
}

}  // namespace montage
