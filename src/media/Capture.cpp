#include "Capture.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <vector>

#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timecode.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#ifdef MONTAGE_WITH_AVDEVICE
#include <libavdevice/avdevice.h>
#endif
}

namespace montage {

namespace {

using Clock = std::chrono::steady_clock;

struct Codec {
    const char* id;
    const char* name;
    const char* encoder;
    const char* profile;
    AVPixelFormat pix;
};
const Codec kCodecs[] = {
    {"prores_proxy", "ProRes 422 Proxy", "prores_ks", "proxy", AV_PIX_FMT_YUV422P10LE},
    {"prores_lt", "ProRes 422 LT", "prores_ks", "lt", AV_PIX_FMT_YUV422P10LE},
    {"prores", "ProRes 422", "prores_ks", "standard", AV_PIX_FMT_YUV422P10LE},
    {"prores_hq", "ProRes 422 HQ", "prores_ks", "hq", AV_PIX_FMT_YUV422P10LE},
    {"dnxhr_lb", "DNxHR LB", "dnxhd", "dnxhr_lb", AV_PIX_FMT_YUV422P},
    {"dnxhr_sq", "DNxHR SQ", "dnxhd", "dnxhr_sq", AV_PIX_FMT_YUV422P},
    {"dnxhr_hq", "DNxHR HQ", "dnxhd", "dnxhr_hq", AV_PIX_FMT_YUV422P},
    {"dnxhr_hqx", "DNxHR HQX (10-bit)", "dnxhd", "dnxhr_hqx", AV_PIX_FMT_YUV422P10LE},
};

const Codec* findCodec(const std::string& id) {
    for (const Codec& c : kCodecs)
        if (id == c.id) return &c;
    return nullptr;
}

std::string avError(int err) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, b, sizeof b);
    return b;
}

bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::char_traits<char>::length(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}

// A packet read from one of the inputs (null: that input ended, with `error` when it failed).
struct Item {
    int input = 0;
    AVPacket* packet = nullptr;
    std::string error;
};

}  // namespace

struct Recorder::State {
    CaptureSettings settings;
    std::atomic<bool> stopping{false}, running{false};
    mutable std::mutex m;
    CaptureStatus st;
    // Packets from the inputs' readers, and their bytes.
    std::mutex qm;
    std::condition_variable qc;
    std::deque<Item> queue;
    int64_t queuedBytes = 0;
    // When a blocked read gives up (each input's own).
    std::atomic<int64_t> deadline[2] = {0, 0};
};

namespace {

// Packets waiting beyond this many bytes: pictures are dropped (whole runs, to the next keyframe) until it halves.
constexpr int64_t kQueueBytes = int64_t(512) << 20;

struct Interrupt {
    Recorder::State* s;
    int input;
};

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

int interrupted(void* opaque) {
    auto* i = static_cast<Interrupt*>(opaque);
    if (i->s->stopping) return 1;
    return nowMs() > i->s->deadline[i->input].load() ? 1 : 0;
}

int64_t msFromNow(double seconds) { return nowMs() + int64_t(seconds * 1000); }

// An opened input and its decoders. What the recording needs of its streams is copied out before its reader starts
// (the reader's demuxer may add streams as it goes).
struct Input {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    int videoStream = -1, audioStream = -1;
    Interrupt irq{};
    AVRational videoTb{1, 1}, audioTb{1, 1}, rate{0, 1};
    ~Input() {
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (fmt) avformat_close_input(&fmt);
    }
};

AVCodecContext* openDecoder(AVStream* st, std::string* error) {
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        if (error) *error = std::string("No decoder for ") + avcodec_get_name(st->codecpar->codec_id);
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    c->thread_count = 0;  // (as many as there are cores: HD and UHD feeds keep up)
    c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (const int err = avcodec_open2(c, dec, nullptr); err < 0) {
        if (error) *error = "Cannot decode the feed: " + avError(err);
        avcodec_free_context(&c);
    }
    return c;
}

bool openInput(Recorder::State* s, int index, const CaptureSource& src, bool wantVideo, bool wantAudio, Input& in, std::string* error) {
    const AVInputFormat* ifmt = nullptr;
    if (!src.device.empty()) {
#ifdef MONTAGE_WITH_AVDEVICE
        ifmt = av_find_input_format(src.device.c_str());
#endif
        if (!ifmt) {
            if (error) *error = "This build cannot open " + src.device + " devices";
            return false;
        }
    }
    AVDictionary* opts = nullptr;
    for (const auto& [k, v] : src.options) av_dict_set(&opts, k.c_str(), v.c_str(), 0);
    // (a stream starts sooner when its format is found from a little of it)
    if (src.device.empty()) {
        if (!av_dict_get(opts, "probesize", nullptr, 0)) av_dict_set(&opts, "probesize", "1000000", 0);
        if (!av_dict_get(opts, "analyzeduration", nullptr, 0)) av_dict_set(&opts, "analyzeduration", "1000000", 0);
    }
    in.fmt = avformat_alloc_context();
    in.irq = {s, index};
    in.fmt->interrupt_callback = {interrupted, &in.irq};
    // Devices are read without blocking (their reads do not look at the interrupt), so stopping is never held up.
    if (!src.device.empty()) in.fmt->flags |= AVFMT_FLAG_NONBLOCK;
    s->deadline[index] = msFromNow(s->settings.openTimeout);
    int err = avformat_open_input(&in.fmt, src.url.c_str(), ifmt, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        in.fmt = nullptr;
        if (error) *error = s->stopping ? "Stopped" : "Cannot open " + src.url + ": " + avError(err);
        return false;
    }
    s->deadline[index] = msFromNow(s->settings.openTimeout);
    if ((err = avformat_find_stream_info(in.fmt, nullptr)) < 0) {
        if (error) *error = "Cannot read " + src.url + ": " + avError(err);
        return false;
    }
    if (wantVideo) in.videoStream = av_find_best_stream(in.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (wantAudio) in.audioStream = av_find_best_stream(in.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (wantVideo && in.videoStream < 0) {
        if (error) *error = src.url + " has no picture";
        return false;
    }
    if (in.videoStream >= 0) {
        AVStream* st = in.fmt->streams[in.videoStream];
        if (!(in.video = openDecoder(st, error))) return false;
        in.videoTb = st->time_base;
        in.rate = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
    }
    if (in.audioStream >= 0) {
        AVStream* st = in.fmt->streams[in.audioStream];
        if (!(in.audio = openDecoder(st, nullptr))) in.audioStream = -1;
        else in.audioTb = st->time_base;
    }
    for (unsigned i = 0; i < in.fmt->nb_streams; ++i)
        if (int(i) != in.videoStream && int(i) != in.audioStream) in.fmt->streams[i]->discard = AVDISCARD_ALL;
    return true;
}

std::string timeOfDay(AVRational fps) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    const int frame = std::min(int(std::lround(av_q2d(fps))) - 1, int(ms * av_q2d(fps) / 1000));
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, std::max(0, frame));
    return b;
}

// The broadcast rates a feed's rate is taken as when it is within half a percent of one (a rate estimated from
// timestamps, 2997/100, is 30000/1001); MXF records only these.
const AVRational kRates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48000, 1001}, {48, 1},
                             {50, 1}, {60000, 1001}, {60, 1}, {100, 1}, {120000, 1001}, {120, 1}};

AVRational standardRate(AVRational r, bool* standard) {
    *standard = false;
    if (r.num <= 0 || r.den <= 0) return r;
    for (const AVRational& k : kRates)
        if (std::fabs(av_q2d(r) / av_q2d(k) - 1) < 0.005) {
            *standard = true;
            return k;
        }
    return r;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> captureCodecs() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const Codec& c : kCodecs)
        if (avcodec_find_encoder_by_name(c.encoder)) out.push_back({c.id, c.name});
    return out;
}

std::vector<std::string> captureDeviceKinds() {
    std::vector<std::string> out;
#ifdef MONTAGE_WITH_AVDEVICE
    avdevice_register_all();
    for (const char* name : {"v4l2", "alsa", "pulse", "avfoundation", "dshow", "lavfi"})
        if (av_find_input_format(name)) out.push_back(name);
#endif
    return out;
}

Recorder::Recorder() : s_(std::make_unique<State>()) {}

Recorder::~Recorder() { stop(); }

bool Recorder::start(const CaptureSettings& settings, std::string* error) {
    stop();
    if (!findCodec(settings.codec) || !avcodec_find_encoder_by_name(findCodec(settings.codec)->encoder)) {
        if (error) *error = "Unknown codec " + settings.codec;
        return false;
    }
    if (!endsWith(settings.path, ".mov") && !endsWith(settings.path, ".mxf")) {
        if (error) *error = "Record to a .mov or .mxf file";
        return false;
    }
    if (settings.video.url.empty()) {
        if (error) *error = "Name the feed to record";
        return false;
    }
#ifdef MONTAGE_WITH_AVDEVICE
    avdevice_register_all();
#endif
    s_ = std::make_unique<State>();
    s_->settings = settings;
    s_->running = true;
    s_->st.recording = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Recorder::stop() {
    if (!s_) return;
    s_->stopping = true;
    s_->qc.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Recorder::wait(double seconds) {
    const auto until = Clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000));
    while (s_ && s_->running && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (s_ && !s_->running && thread_.joinable()) thread_.join();
    return !s_ || !s_->running;
}

bool Recorder::recording() const { return s_ && s_->running; }

CaptureStatus Recorder::status() const {
    std::lock_guard lock(s_->m);
    CaptureStatus st = s_->st;
    st.recording = s_->running;
    return st;
}

void Recorder::run() {
    State* s = s_.get();
    const CaptureSettings& cs = s->settings;
    const Codec* codec = findCodec(cs.codec);
    std::string fatal, warning;
    std::vector<std::thread> readers;
    Input inputs[2];
    AVFormatContext* oc = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    SwsContext* sws = nullptr;
    int swsKey[5] = {0, 0, -1, -1, -1};  // the source size, format, colour space and range sws_ is set for
    SwrContext* swr = nullptr;
    AVChannelLayout swrLayout{};
    int swrFormat = -1, swrRate = 0;
    bool swrFailed = false;
    AVFrame* last = nullptr;  // the latest picture as encoded (repeated for frames missing)
    AVFrame* decoded = av_frame_alloc();
    AVFrame* sound = av_frame_alloc();
    AVPacket* out = av_packet_alloc();
    bool headerWritten = false, fileMade = false, told = false, timedOut = false;
    AVRational fps{25, 1};
    double t0 = 0;               // the first picture's time (seconds, its input's clock), moved on a jump
    int64_t nextFrame = 0;       // the next picture's number in the file
    int64_t nextSample = 0;      // the next sound sample's
    int64_t skipSamples = 0;     // sound that came early, to drop
    bool audioStarted = false;
    double audioBase = 0;        // a sound device's: its first timestamp and the sample it was placed at
    int64_t audioBaseSample = 0;
    const bool mov = endsWith(cs.path, ".mov");
    int64_t frameLimit = 0;
    int64_t soundEnd = INT64_MAX;  // (once stopped: the pictures' end)
    int width = 0, height = 0;
    int colorSpace = AVCOL_SPC_BT709;
    bool interlaced = false, topFirst = true;

    auto write = [&](AVCodecContext* enc, AVStream* st, AVFrame* f) -> bool {
        int err = avcodec_send_frame(enc, f);
        if (err < 0 && err != AVERROR_EOF) {
            fatal = "Cannot encode: " + avError(err);
            return false;
        }
        while ((err = avcodec_receive_packet(enc, out)) >= 0) {
            av_packet_rescale_ts(out, enc->time_base, st->time_base);
            out->stream_index = st->index;
            if ((err = av_interleaved_write_frame(oc, out)) < 0) {
                fatal = "Cannot write " + cs.path + ": " + avError(err);
                return false;
            }
        }
        return err == AVERROR(EAGAIN) || err == AVERROR_EOF;
    };
    auto silence = [&](int64_t count) -> bool {
        while (count > 0) {
            AVFrame* quiet = av_frame_alloc();
            quiet->format = aenc->sample_fmt;
            quiet->sample_rate = 48000;
            av_channel_layout_copy(&quiet->ch_layout, &aenc->ch_layout);
            quiet->nb_samples = int(std::min<int64_t>(count, 48000));
            if (av_frame_get_buffer(quiet, 0) < 0) {
                av_frame_free(&quiet);
                return false;
            }
            av_samples_set_silence(quiet->data, 0, quiet->nb_samples, quiet->ch_layout.nb_channels, AVSampleFormat(quiet->format));
            quiet->pts = nextSample;
            nextSample += quiet->nb_samples;
            count -= quiet->nb_samples;
            const bool ok = write(aenc, ast, quiet);
            av_frame_free(&quiet);
            if (!ok) return false;
        }
        return true;
    };

    // Opens the file at the first picture (its size, rate and timecode known then).
    auto begin = [&](const AVFrame* f, Input& in) -> bool {
        bool standard = false;
        fps = standardRate(in.rate, &standard);
        if (fps.num <= 0 || fps.den <= 0 || av_q2d(fps) > 240 || av_q2d(fps) < 1) fps = {25, 1}, standard = true;
        if (!mov && !standard) {
            char b[32];
            std::snprintf(b, sizeof b, "%.3g", av_q2d(fps));
            fatal = std::string("MXF records at the broadcast rates (23.976 to 120 fps); this feed runs at ") + b + " fps: record it to QuickTime";
            return false;
        }
        frameLimit = cs.seconds > 0 ? int64_t(std::llround(cs.seconds * av_q2d(fps))) : 0;
        std::string tc = cs.timecode;
        if (tc.empty())
            if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_S12M_TIMECODE); sd && sd->size >= 8) {
                const auto* v = reinterpret_cast<const uint32_t*>(sd->data);
                if (v[0] >= 1) {
                    char b[AV_TIMECODE_STR_SIZE];
                    av_timecode_make_smpte_tc_string2(b, fps, v[1], 0, 0);
                    tc = b;
                    AVTimecode check;
                    if (av_timecode_init_from_string(&check, fps, tc.c_str(), nullptr) < 0) tc.clear();  // (the feed's does not fit its rate)
                }
            }
        if (tc.empty()) tc = timeOfDay(fps);
        if (AVTimecode check; av_timecode_init_from_string(&check, fps, tc.c_str(), nullptr) < 0) {
            char b[32];
            std::snprintf(b, sizeof b, "%.3g", av_q2d(fps));
            fatal = "The timecode " + tc + " does not fit the feed's " + b + " fps";
            return false;
        }
        int err = avformat_alloc_output_context2(&oc, nullptr, nullptr, cs.path.c_str());
        if (err < 0 || !oc) {
            fatal = "Cannot write " + cs.path + ": " + avError(err);
            return false;
        }
        width = f->width, height = f->height;
#ifdef AV_FRAME_FLAG_INTERLACED
        interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        topFirst = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
#else
        interlaced = f->interlaced_frame != 0;
        topFirst = f->top_field_first != 0;
#endif
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
        colorSpace = !rgb && f->colorspace != AVCOL_SPC_UNSPECIFIED && f->colorspace != AVCOL_SPC_RGB ? f->colorspace
                     : height >= 720                                                                    ? AVCOL_SPC_BT709
                                                                                                        : AVCOL_SPC_SMPTE170M;
        const AVCodec* enc = avcodec_find_encoder_by_name(codec->encoder);
        venc = avcodec_alloc_context3(enc);
        venc->width = width;
        venc->height = height;
        venc->pix_fmt = codec->pix;
        venc->time_base = av_inv_q(fps);
        venc->framerate = fps;
        venc->sample_aspect_ratio = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : AVRational{1, 1};
        venc->color_primaries = f->color_primaries != AVCOL_PRI_UNSPECIFIED ? f->color_primaries : colorSpace == AVCOL_SPC_BT709 ? AVCOL_PRI_BT709 : AVCOL_PRI_UNSPECIFIED;
        venc->color_trc = f->color_trc != AVCOL_TRC_UNSPECIFIED ? f->color_trc : colorSpace == AVCOL_SPC_BT709 ? AVCOL_TRC_BT709 : AVCOL_TRC_UNSPECIFIED;
        venc->colorspace = AVColorSpace(colorSpace);
        venc->color_range = AVCOL_RANGE_MPEG;
        venc->thread_count = 0;
        venc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (interlaced) {
            venc->field_order = topFirst ? AV_FIELD_TT : AV_FIELD_BB;
            venc->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
        }
        av_opt_set(venc->priv_data, "profile", codec->profile, 0);
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((err = avcodec_open2(venc, enc, nullptr)) < 0) {
            fatal = std::string("Cannot encode ") + codec->name + " at " + std::to_string(width) + "x" + std::to_string(height) + ": " + avError(err);
            return false;
        }
        if (!(vst = avformat_new_stream(oc, nullptr))) {
            fatal = "Out of memory";
            return false;
        }
        avcodec_parameters_from_context(vst->codecpar, venc);
        vst->time_base = venc->time_base;
        vst->avg_frame_rate = fps;
        av_dict_set(&vst->metadata, "timecode", tc.c_str(), 0);
        // Its sound: the video source's, else a sound device of its own (its format known from the first sound).
        const Input& ain = inputs[1].fmt ? inputs[1] : inputs[0];
        if (ain.audioStream >= 0 && ain.audio) {
            const AVCodec* pcm = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
            aenc = avcodec_alloc_context3(pcm);
            aenc->sample_fmt = AV_SAMPLE_FMT_S32;
            aenc->sample_rate = 48000;
            const int channels = std::min(16, std::max(1, ain.audio->ch_layout.nb_channels));
            if (ain.audio->ch_layout.nb_channels == channels && ain.audio->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_copy(&aenc->ch_layout, &ain.audio->ch_layout);
            else
                av_channel_layout_default(&aenc->ch_layout, channels);
            aenc->time_base = {1, 48000};
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(aenc, pcm, nullptr) < 0 || !(ast = avformat_new_stream(oc, nullptr))) {
                // (the picture is recorded without it)
                avcodec_free_context(&aenc);
                warning = "The sound could not be recorded";
            } else {
                avcodec_parameters_from_context(ast->codecpar, aenc);
                ast->time_base = aenc->time_base;
            }
        }
        av_dict_set(&oc->metadata, "timecode", tc.c_str(), 0);
        if (!(oc->oformat->flags & AVFMT_NOFILE)) {
            if ((err = avio_open(&oc->pb, cs.path.c_str(), AVIO_FLAG_WRITE)) < 0) {
                fatal = "Cannot write " + cs.path + ": " + avError(err);
                return false;
            }
            fileMade = true;
        }
        AVDictionary* mopts = nullptr;
        if (mov) {
            // Fragments half a second long: the file opens while it grows, and a crash keeps what was written.
            av_dict_set(&mopts, "movflags", "+empty_moov+default_base_moof", 0);
            av_dict_set(&mopts, "frag_duration", "500000", 0);
        }
        err = avformat_write_header(oc, &mopts);
        av_dict_free(&mopts);
        if (err < 0) {
            fatal = "Cannot write " + cs.path + ": " + avError(err);
            return false;
        }
        headerWritten = true;
        std::lock_guard lock(s->m);
        s->st.width = width;
        s->st.height = height;
        s->st.fps = av_q2d(fps);
        s->st.timecode = tc;
        s->st.channels = aenc ? aenc->ch_layout.nb_channels : 0;
        return true;
    };

    auto putPicture = [&](AVFrame* f, Input& in) -> bool {
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (!headerWritten) {
            // (a feed joined between keyframes: its pictures start once they decode whole)
            if (f->decode_error_flags || (f->flags & AV_FRAME_FLAG_CORRUPT)) return true;
            if (!begin(f, in)) return false;
            t0 = ts != AV_NOPTS_VALUE ? double(ts) * av_q2d(in.videoTb) : 0;
        }
        if (frameLimit > 0 && nextFrame >= frameLimit) {
            timedOut = true;
            return true;
        }
        double t = ts != AV_NOPTS_VALUE ? double(ts) * av_q2d(in.videoTb) : t0 + double(nextFrame) / av_q2d(fps);
        // A jump in the feed's clock (an encoder restarted, a stream switched): taken as going on from here.
        const double late = t - (t0 + double(nextFrame) / av_q2d(fps));
        if (late > 2 || late < -1) t0 = t - double(nextFrame) / av_q2d(fps);
        const int64_t at = int64_t(std::llround((t - t0) * av_q2d(fps)));
        if (at < nextFrame) {
            std::lock_guard lock(s->m);
            ++s->st.dropped;
            return true;
        }
        // The picture in the file's format (at its size, should the feed's change).
        const int key[5] = {f->width, f->height, f->format, f->colorspace, f->color_range};
        if (!sws || !std::equal(key, key + 5, swsKey)) {
            sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), width, height, codec->pix, SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!sws) {
                fatal = std::string("Cannot convert the feed's pictures (") + av_get_pix_fmt_name(AVPixelFormat(f->format)) + ")";
                return false;
            }
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
            const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
            const int srcSpace = !rgb && f->colorspace != AVCOL_SPC_UNSPECIFIED && f->colorspace != AVCOL_SPC_RGB ? f->colorspace : colorSpace;
            const bool srcFull = rgb || f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P ||
                                 f->format == AV_PIX_FMT_YUVJ444P;
            sws_setColorspaceDetails(sws, sws_getCoefficients(srcSpace), srcFull ? 1 : 0, sws_getCoefficients(colorSpace), 0, 0, 1 << 16, 1 << 16);
            std::copy(key, key + 5, swsKey);
        }
        AVFrame* pic = av_frame_alloc();
        pic->format = codec->pix;
        pic->width = width;
        pic->height = height;
        if (av_frame_get_buffer(pic, 0) < 0) {
            av_frame_free(&pic);
            fatal = "Out of memory";
            return false;
        }
        sws_scale(sws, f->data, f->linesize, 0, f->height, pic->data, pic->linesize);
        if (interlaced) {
#ifdef AV_FRAME_FLAG_INTERLACED
            pic->flags |= AV_FRAME_FLAG_INTERLACED | (topFirst ? AV_FRAME_FLAG_TOP_FIELD_FIRST : 0);
#else
            pic->interlaced_frame = 1;
            pic->top_field_first = topFirst;
#endif
        }
        // Missing pictures: the last one again (a second at most at a time; no further than a set length).
        for (int64_t gap = 0; nextFrame < at && gap < int64_t(av_q2d(fps)) + 1 && last && (frameLimit == 0 || nextFrame < frameLimit); ++gap) {
            AVFrame* again = av_frame_clone(last);
            again->pts = nextFrame++;
            const bool ok = write(venc, vst, again);
            av_frame_free(&again);
            if (!ok) {
                av_frame_free(&pic);
                return false;
            }
            std::lock_guard lock(s->m);
            ++s->st.repeated;
        }
        nextFrame = std::max(nextFrame, at);
        if (frameLimit > 0 && nextFrame >= frameLimit) {
            av_frame_free(&pic);
            timedOut = true;
            return true;
        }
        pic->pts = nextFrame++;
        AVFrame* e = av_frame_clone(pic);
        const bool ok = write(venc, vst, e);
        av_frame_free(&e);
        av_frame_free(&last);
        last = pic;
        if (!ok) return false;
        {
            std::lock_guard lock(s->m);
            s->st.frames = nextFrame;
            s->st.seconds = double(nextFrame) / av_q2d(fps);
        }
        if (!told && mov && nextFrame > int64_t(av_q2d(fps)) + 1) {
            avio_flush(oc->pb);
            told = true;
            if (onFirstFragment) onFirstFragment();
        }
        if (frameLimit > 0 && nextFrame >= frameLimit) timedOut = true;
        return true;
    };

    auto putSound = [&](AVFrame* f, bool sameInput, AVRational tb) -> bool {
        if (!headerWritten || !aenc) return true;
        const int64_t endSample = std::min(soundEnd, frameLimit > 0 ? int64_t(std::llround(double(frameLimit) / av_q2d(fps) * 48000)) : INT64_MAX);
        if (nextSample >= endSample) return true;
        // (made again should the feed's sound change: a broadcast's 2.0 to 5.1...)
        AVChannelLayout layout{};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0)
            av_channel_layout_default(&layout, std::max(1, f->ch_layout.nb_channels));
        else
            av_channel_layout_copy(&layout, &f->ch_layout);
        if (!swr || f->format != swrFormat || f->sample_rate != swrRate || av_channel_layout_compare(&layout, &swrLayout) != 0) {
            swr_free(&swr);
            av_channel_layout_uninit(&swrLayout);
            av_channel_layout_copy(&swrLayout, &layout);
            swrFormat = f->format, swrRate = f->sample_rate;
            swr_alloc_set_opts2(&swr, &aenc->ch_layout, aenc->sample_fmt, 48000, &layout, AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
            const int outChannels = aenc->ch_layout.nb_channels;
            if (swr && layout.nb_channels > outChannels) {
                // (more than 16: the first 16, each as it is)
                std::vector<double> matrix(size_t(outChannels) * size_t(layout.nb_channels), 0.0);
                for (int c = 0; c < outChannels; ++c) matrix[size_t(c) * size_t(layout.nb_channels) + size_t(c)] = 1.0;
                swr_set_matrix(swr, matrix.data(), layout.nb_channels);
            }
            if (swr && swr_init(swr) < 0) swr_free(&swr);
            if (!swr && !swrFailed) {
                swrFailed = true;
                if (warning.empty()) warning = "Some of the sound could not be converted";
            }
        }
        av_channel_layout_uninit(&layout);
        if (!swr || f->sample_rate <= 0) return true;
        const int outCount = swr_get_out_samples(swr, f->nb_samples);
        if (outCount <= 0) return true;
        sound->format = aenc->sample_fmt;
        sound->sample_rate = 48000;
        av_channel_layout_copy(&sound->ch_layout, &aenc->ch_layout);
        sound->nb_samples = outCount;
        if (av_frame_get_buffer(sound, 0) < 0) return false;
        const int n = swr_convert(swr, sound->data, outCount, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        if (n <= 0) {
            av_frame_unref(sound);
            return true;
        }
        sound->nb_samples = n;
        // Where it belongs: by its time against the pictures' (the same feed's clock), or a sound device's against its
        // own first sound's, placed where the picture had got to then.
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        const double t = ts != AV_NOPTS_VALUE ? double(ts) * av_q2d(tb) : std::nan("");
        int64_t expected = nextSample;
        if (!audioStarted && !sameInput) {
            audioBase = std::isnan(t) ? 0 : t;
            audioBaseSample = int64_t(std::llround(double(nextFrame) / av_q2d(fps) * 48000));
        }
        if (!std::isnan(t)) expected = sameInput ? int64_t(std::llround((t - t0) * 48000)) : audioBaseSample + int64_t(std::llround((t - audioBase) * 48000));
        const int64_t diff = expected - nextSample;
        if (!audioStarted || (std::llabs(diff) > 4800 && std::llabs(diff) < 96000)) {
            // (exactly at the start; after it, gaps and overlaps of more than a tenth of a second, so jitter is left be;
            // past two seconds a jump in the clock, which the pictures take up)
            if (diff > 0 && !silence(std::min<int64_t>(diff, std::min<int64_t>(48000 * 10, endSample - nextSample)))) {
                av_frame_unref(sound);
                return false;
            }
            if (diff < 0) skipSamples += -diff;
        }
        audioStarted = true;
        const int skip = int(std::min<int64_t>(skipSamples, n));
        skipSamples -= skip;
        if (skip >= n || nextSample >= endSample) {
            av_frame_unref(sound);
            return true;
        }
        AVFrame* use = sound;
        AVFrame* trimmed = nullptr;
        if (skip > 0) {
            trimmed = av_frame_alloc();
            trimmed->format = sound->format;
            trimmed->sample_rate = 48000;
            av_channel_layout_copy(&trimmed->ch_layout, &sound->ch_layout);
            trimmed->nb_samples = n - skip;
            av_frame_get_buffer(trimmed, 0);
            av_samples_copy(trimmed->data, sound->data, 0, skip, n - skip, sound->ch_layout.nb_channels, AVSampleFormat(sound->format));
            use = trimmed;
        }
        use->nb_samples = int(std::min<int64_t>(use->nb_samples, endSample - nextSample));
        use->pts = nextSample;
        nextSample += use->nb_samples;
        const bool ok = write(aenc, ast, use);
        av_frame_free(&trimmed);
        av_frame_unref(sound);
        return ok;
    };

    // One packet decoded: pictures (unless only sound is wanted now) and sound.
    auto decode = [&](Item& item, bool soundOnly) -> bool {
        Input& in = inputs[item.input];
        const bool isVideo = item.packet->stream_index == in.videoStream;
        if (isVideo && soundOnly) return true;
        AVCodecContext* dec = isVideo ? in.video : in.audio;
        bool ok = true;
        if (dec && avcodec_send_packet(dec, item.packet) >= 0)
            while (ok && avcodec_receive_frame(dec, isVideo ? decoded : sound) >= 0) {
                if (isVideo) {
                    ok = putPicture(decoded, in);
                    av_frame_unref(decoded);
                    if (timedOut) break;
                } else {
                    AVFrame* a = av_frame_clone(sound);
                    av_frame_unref(sound);
                    ok = putSound(a, item.input == 0, in.audioTb);
                    av_frame_free(&a);
                }
            }
        return ok;
    };

    // Opens the feed (and a sound device of its own), then reads each on a thread of its own.
    bool opened = openInput(s, 0, cs.video, true, !cs.audio, inputs[0], &fatal);
    if (opened && cs.audio) opened = openInput(s, 1, *cs.audio, false, true, inputs[1], &fatal);
    if (opened) {
        for (int i = 0; i < 2; ++i) {
            if (!inputs[i].fmt) continue;
            readers.emplace_back([s, i, &inputs] {
                Input& in = inputs[i];
                const double patience = std::max(5.0, s->settings.openTimeout);
                int64_t heard = nowMs();
                bool dropping = false;
                while (!s->stopping) {
                    s->deadline[i] = msFromNow(patience);
                    AVPacket* p = av_packet_alloc();
                    const int err = av_read_frame(in.fmt, p);
                    if (err == AVERROR(EAGAIN)) {
                        // (a device with nothing yet: asked again shortly, given up after a while of nothing)
                        av_packet_free(&p);
                        if (nowMs() - heard < int64_t(patience * 1000)) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }
                    }
                    Item item;
                    item.input = i;
                    if (err < 0) {
                        av_packet_free(&p);
                        if (!s->stopping && err != AVERROR_EOF)
                            item.error = err == AVERROR(EAGAIN) ? "The feed stopped sending" : "The feed stopped: " + avError(err);
                        std::lock_guard lock(s->qm);
                        s->queue.push_back(item);
                        s->qc.notify_all();
                        return;
                    }
                    heard = nowMs();
                    if (p->stream_index != in.videoStream && p->stream_index != in.audioStream) {
                        av_packet_free(&p);
                        continue;
                    }
                    item.packet = p;
                    std::lock_guard lock(s->qm);
                    if (p->stream_index == in.videoStream) {
                        // Falling behind: pictures dropped, whole runs to the next keyframe (sound never).
                        if (s->queuedBytes > kQueueBytes) dropping = true;
                        else if (dropping && s->queuedBytes < kQueueBytes / 2 && (p->flags & AV_PKT_FLAG_KEY)) dropping = false;
                        if (dropping) {
                            av_packet_free(&item.packet);
                            continue;
                        }
                    }
                    s->queuedBytes += p->size;
                    s->queue.push_back(item);
                    s->qc.notify_all();
                }
            });
        }
        bool ok = true;
        int ended = 0;
        const int live = inputs[1].fmt ? 2 : 1;
        while (ok && !timedOut) {
            Item item;
            {
                std::unique_lock lock(s->qm);
                s->qc.wait_for(lock, std::chrono::milliseconds(200), [&] { return !s->queue.empty() || s->stopping; });
                if (s->stopping) break;
                if (s->queue.empty()) continue;
                item = s->queue.front();
                s->queue.pop_front();
                if (item.packet) s->queuedBytes -= item.packet->size;
            }
            if (!item.packet) {
                if (!item.error.empty()) {
                    // (a sound device lost: the picture goes on without it)
                    if (item.input == 1) warning = "The sound device stopped:" + item.error.substr(item.error.find(':') + 1);
                    else warning = item.error;
                    std::lock_guard lock(s->m);
                    s->st.warning = warning;
                }
                if (++ended >= live || item.input == 0) break;  // (the picture ended: so does the recording)
                continue;
            }
            ok = decode(item, false);
            av_packet_free(&item.packet);
        }
        // Stopped: the sound already read up to the picture's end kept.
        s->stopping = true;
        for (std::thread& t : readers) t.join();
        readers.clear();
        soundEnd = int64_t(std::llround(double(nextFrame) / av_q2d(fps) * 48000));
        while (ok && headerWritten && aenc) {
            Item item;
            {
                std::lock_guard lock(s->qm);
                if (s->queue.empty()) break;
                item = s->queue.front();
                s->queue.pop_front();
            }
            if (item.packet) ok = decode(item, true);
            av_packet_free(&item.packet);
        }
        // Pictures and sound end together.
        if (ok && headerWritten && aenc) {
            const int64_t end = int64_t(std::llround(double(nextFrame) / av_q2d(fps) * 48000));
            if (nextSample < end) silence(end - nextSample);
        }
    }
    s->stopping = true;
    for (std::thread& t : readers) t.join();
    {
        std::lock_guard lock(s->qm);
        for (Item& i : s->queue) av_packet_free(&i.packet);
        s->queue.clear();
        s->queuedBytes = 0;
    }
    if (headerWritten) {
        // The encoders emptied and the file finished (a QuickTime file already holds all but the last fragment).
        if (venc) write(venc, vst, nullptr);
        if (aenc) write(aenc, ast, nullptr);
        const int err = av_write_trailer(oc);
        if (err < 0 && fatal.empty()) fatal = "Cannot finish " + cs.path + ": " + avError(err);
    } else if (fatal.empty()) {
        fatal = warning.empty() ? "No picture came from the feed" : warning;
    }
    if (oc) {
        if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
    // (nothing usable: the file begun is taken away again)
    if (fileMade && !headerWritten) QFile::remove(QString::fromStdString(cs.path));
    avcodec_free_context(&venc);
    avcodec_free_context(&aenc);
    sws_freeContext(sws);
    swr_free(&swr);
    av_channel_layout_uninit(&swrLayout);
    av_frame_free(&last);
    av_frame_free(&decoded);
    av_frame_free(&sound);
    av_packet_free(&out);
    const bool fine = headerWritten && (fatal.empty() || fatal == "Stopped");
    {
        std::lock_guard lock(s->m);
        s->st.finished = true;
        s->st.recording = false;
        if (!fine && s->st.error.empty()) s->st.error = fatal;
        s->st.warning = warning;
    }
    s->running = false;
    if (onFinished) onFinished(fine, fine ? warning : fatal);
}

}  // namespace montage
