#include "Capture.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
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
    {"prores_proxy", "Apple ProRes 422 Proxy", "prores_ks", "proxy", AV_PIX_FMT_YUV422P10LE},
    {"prores_lt", "Apple ProRes 422 LT", "prores_ks", "lt", AV_PIX_FMT_YUV422P10LE},
    {"prores", "Apple ProRes 422", "prores_ks", "standard", AV_PIX_FMT_YUV422P10LE},
    {"prores_hq", "Apple ProRes 422 HQ", "prores_ks", "hq", AV_PIX_FMT_YUV422P10LE},
    {"dnxhr_lb", "Avid DNxHR LB", "dnxhd", "dnxhr_lb", AV_PIX_FMT_YUV422P},
    {"dnxhr_sq", "Avid DNxHR SQ", "dnxhd", "dnxhr_sq", AV_PIX_FMT_YUV422P},
    {"dnxhr_hq", "Avid DNxHR HQ", "dnxhd", "dnxhr_hq", AV_PIX_FMT_YUV422P},
    {"dnxhr_hqx", "Avid DNxHR HQX (10-bit)", "dnxhd", "dnxhr_hqx", AV_PIX_FMT_YUV422P10LE},
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
    // Packets from the inputs' readers.
    std::mutex qm;
    std::condition_variable qc;
    std::deque<Item> queue;
    // When a blocked read gives up (each input's own).
    std::atomic<int64_t> deadline[2] = {0, 0};
    void fail(const std::string& e) {
        std::lock_guard lock(m);
        if (st.error.empty()) st.error = e;
    }
};

namespace {

struct Interrupt {
    Recorder::State* s;
    int input;
};

int interrupted(void* opaque) {
    auto* i = static_cast<Interrupt*>(opaque);
    if (i->s->stopping) return 1;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    return now > i->s->deadline[i->input].load() ? 1 : 0;
}

int64_t msFromNow(double seconds) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() + int64_t(seconds * 1000);
}

// An opened input and its decoders.
struct Input {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    int videoStream = -1, audioStream = -1;
    Interrupt irq{};
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
    if (in.videoStream >= 0 && !(in.video = openDecoder(in.fmt->streams[in.videoStream], error))) return false;
    if (in.audioStream >= 0 && !(in.audio = openDecoder(in.fmt->streams[in.audioStream], error))) in.audioStream = -1;
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
    std::string error;
    bool ok = false;
    std::vector<std::thread> readers;
    Input inputs[2];
    AVFormatContext* oc = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* conv = av_frame_alloc();
    AVFrame* last = av_frame_alloc();  // the latest picture as encoded (repeated for frames missing)
    AVFrame* decoded = av_frame_alloc();
    AVFrame* sound = av_frame_alloc();
    AVPacket* out = av_packet_alloc();
    bool headerWritten = false, told = false, timedOut = false;
    AVRational fps{25, 1};
    double t0 = 0;               // the first picture's time (seconds, its input's clock)
    int64_t nextFrame = 0;       // the next picture's number in the file
    int64_t nextSample = 0;      // the next sound sample's
    bool audioStarted = false;
    const bool mov = endsWith(cs.path, ".mov");
    int64_t frameLimit = 0;

    auto write = [&](AVCodecContext* enc, AVStream* st, AVFrame* f) -> bool {
        int err = avcodec_send_frame(enc, f);
        if (err < 0 && err != AVERROR_EOF) {
            error = "Cannot encode: " + avError(err);
            return false;
        }
        while ((err = avcodec_receive_packet(enc, out)) >= 0) {
            av_packet_rescale_ts(out, enc->time_base, st->time_base);
            out->stream_index = st->index;
            if ((err = av_interleaved_write_frame(oc, out)) < 0) {
                error = "Cannot write " + cs.path + ": " + avError(err);
                return false;
            }
        }
        return err == AVERROR(EAGAIN) || err == AVERROR_EOF;
    };

    // Opens the file at the first picture (its size, rate and timecode known then).
    auto begin = [&](const AVFrame* f, Input& in) -> bool {
        AVStream* ist = in.fmt->streams[in.videoStream];
        fps = ist->avg_frame_rate.num > 0 && ist->avg_frame_rate.den > 0 ? ist->avg_frame_rate : ist->r_frame_rate;
        if (fps.num <= 0 || fps.den <= 0 || av_q2d(fps) > 240 || av_q2d(fps) < 1) fps = {25, 1};
        frameLimit = cs.seconds > 0 ? int64_t(std::llround(cs.seconds * av_q2d(fps))) : 0;
        std::string tc = cs.timecode;
        if (tc.empty())
            if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_S12M_TIMECODE); sd && sd->size >= 8) {
                const auto* v = reinterpret_cast<const uint32_t*>(sd->data);
                if (v[0] >= 1) {
                    char b[AV_TIMECODE_STR_SIZE];
                    av_timecode_make_smpte_tc_string2(b, fps, v[1], 0, 0);
                    tc = b;
                }
            }
        if (tc.empty()) tc = timeOfDay(fps);
        int err = avformat_alloc_output_context2(&oc, nullptr, nullptr, cs.path.c_str());
        if (err < 0 || !oc) {
            error = "Cannot write " + cs.path + ": " + avError(err);
            return false;
        }
        const AVCodec* enc = avcodec_find_encoder_by_name(codec->encoder);
        venc = avcodec_alloc_context3(enc);
        venc->width = f->width;
        venc->height = f->height;
        venc->pix_fmt = codec->pix;
        venc->time_base = av_inv_q(fps);
        venc->framerate = fps;
        venc->sample_aspect_ratio = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : AVRational{1, 1};
        venc->color_primaries = f->color_primaries;
        venc->color_trc = f->color_trc;
        venc->colorspace = f->colorspace;
        venc->color_range = AVCOL_RANGE_MPEG;
        av_opt_set(venc->priv_data, "profile", codec->profile, 0);
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((err = avcodec_open2(venc, enc, nullptr)) < 0) {
            error = std::string("Cannot encode ") + codec->name + " at " + std::to_string(f->width) + "x" + std::to_string(f->height) + ": " + avError(err);
            return false;
        }
        vst = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(vst->codecpar, venc);
        vst->time_base = venc->time_base;
        vst->avg_frame_rate = fps;
        av_dict_set(&vst->metadata, "timecode", tc.c_str(), 0);
        // Its sound: the video source's, else a sound device of its own.
        Input& ain = inputs[1].fmt ? inputs[1] : inputs[0];
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
            if ((err = avcodec_open2(aenc, pcm, nullptr)) < 0) {
                error = "Cannot encode the sound: " + avError(err);
                return false;
            }
            ast = avformat_new_stream(oc, nullptr);
            avcodec_parameters_from_context(ast->codecpar, aenc);
            ast->time_base = aenc->time_base;
            AVChannelLayout inLayout{};
            if (ain.audio->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || ain.audio->ch_layout.nb_channels <= 0)
                av_channel_layout_default(&inLayout, std::max(1, ain.audio->ch_layout.nb_channels));
            else
                av_channel_layout_copy(&inLayout, &ain.audio->ch_layout);
            swr_alloc_set_opts2(&swr, &aenc->ch_layout, aenc->sample_fmt, 48000, &inLayout, ain.audio->sample_fmt,
                                ain.audio->sample_rate, 0, nullptr);
            if (swr && inLayout.nb_channels > channels) {
                // (more than 16: the first 16, each as it is)
                std::vector<double> matrix(size_t(channels) * size_t(inLayout.nb_channels), 0.0);
                for (int c = 0; c < channels; ++c) matrix[size_t(c) * size_t(inLayout.nb_channels) + size_t(c)] = 1.0;
                swr_set_matrix(swr, matrix.data(), inLayout.nb_channels);
            }
            av_channel_layout_uninit(&inLayout);
            if (!swr || swr_init(swr) < 0) {
                error = "Cannot convert the sound";
                return false;
            }
        }
        av_dict_set(&oc->metadata, "timecode", tc.c_str(), 0);
        if (!(oc->oformat->flags & AVFMT_NOFILE) && (err = avio_open(&oc->pb, cs.path.c_str(), AVIO_FLAG_WRITE)) < 0) {
            error = "Cannot write " + cs.path + ": " + avError(err);
            return false;
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
            error = "Cannot write " + cs.path + ": " + avError(err);
            return false;
        }
        headerWritten = true;
        sws = sws_getContext(f->width, f->height, AVPixelFormat(f->format), f->width, f->height, codec->pix, SWS_BICUBIC, nullptr, nullptr, nullptr);
        conv->format = codec->pix;
        conv->width = f->width;
        conv->height = f->height;
        av_frame_get_buffer(conv, 0);
        std::lock_guard lock(s->m);
        s->st.width = f->width;
        s->st.height = f->height;
        s->st.fps = av_q2d(fps);
        s->st.timecode = tc;
        s->st.channels = aenc ? aenc->ch_layout.nb_channels : 0;
        return true;
    };

    auto putPicture = [&](AVFrame* f, Input& in) -> bool {
        const AVRational tb = in.fmt->streams[in.videoStream]->time_base;
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        double t = ts != AV_NOPTS_VALUE ? double(ts) * av_q2d(tb) : double(nextFrame) / av_q2d(fps);
        if (!headerWritten) {
            if (!begin(f, in)) return false;
            t0 = t;
        }
        if (f->width != conv->width || f->height != conv->height) return true;  // (a change of size mid-feed: skipped)
        const int64_t at = int64_t(std::llround((t - t0) * av_q2d(fps)));
        if (at < nextFrame) {
            std::lock_guard lock(s->m);
            ++s->st.dropped;
            return true;
        }
        av_frame_make_writable(conv);
        sws_scale(sws, f->data, f->linesize, 0, f->height, conv->data, conv->linesize);
        // Missing pictures: the last one again (a second at most at a time).
        for (int64_t gap = 0; nextFrame < at && gap < int64_t(av_q2d(fps)) + 1 && last->data[0]; ++gap) {
            last->pts = nextFrame++;
            if (!write(venc, vst, last)) return false;
            std::lock_guard lock(s->m);
            ++s->st.repeated;
        }
        nextFrame = std::max(nextFrame, at);
        av_frame_unref(last);
        av_frame_ref(last, conv);
        AVFrame* e = av_frame_clone(conv);
        e->pts = nextFrame++;
        const bool ok = write(venc, vst, e);
        av_frame_free(&e);
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

    auto putSound = [&](AVFrame* f, Input& in, bool sameInput) -> bool {
        if (!headerWritten || !aenc || !swr) return true;
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
        int skip = 0;
        if (!audioStarted) {
            audioStarted = true;
            // Placed by its time against the first picture's (from the same feed), else from when the picture began.
            int64_t offset = 0;
            if (sameInput) {
                const AVRational tb = in.fmt->streams[in.audioStream]->time_base;
                const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
                if (ts != AV_NOPTS_VALUE) offset = int64_t(std::llround((double(ts) * av_q2d(tb) - t0) * 48000));
            } else {
                offset = int64_t(std::llround(double(nextFrame) / av_q2d(fps) * 48000));
            }
            if (offset > 0) {
                // (silence before it)
                AVFrame* quiet = av_frame_alloc();
                quiet->format = aenc->sample_fmt;
                quiet->sample_rate = 48000;
                av_channel_layout_copy(&quiet->ch_layout, &aenc->ch_layout);
                quiet->nb_samples = int(std::min<int64_t>(offset, 48000 * 10));
                av_frame_get_buffer(quiet, 0);
                av_samples_set_silence(quiet->data, 0, quiet->nb_samples, quiet->ch_layout.nb_channels, AVSampleFormat(quiet->format));
                quiet->pts = 0;
                nextSample = quiet->nb_samples;
                const bool ok = write(aenc, ast, quiet);
                av_frame_free(&quiet);
                if (!ok) return false;
            } else {
                skip = int(std::min<int64_t>(-offset, n));
            }
        }
        if (skip >= n) {
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
        // (no further than the pictures: a set length ends both together)
        if (frameLimit > 0) {
            const int64_t end = int64_t(std::llround(double(frameLimit) / av_q2d(fps) * 48000));
            if (nextSample >= end) {
                av_frame_free(&trimmed);
                av_frame_unref(sound);
                return true;
            }
            use->nb_samples = int(std::min<int64_t>(use->nb_samples, end - nextSample));
        }
        use->pts = nextSample;
        nextSample += use->nb_samples;
        const bool ok = write(aenc, ast, use);
        av_frame_free(&trimmed);
        av_frame_unref(sound);
        return ok;
    };

    // Opens the feed (and a sound device of its own), then reads each on a thread of its own.
    bool opened = openInput(s, 0, cs.video, true, !cs.audio, inputs[0], &error);
    if (opened && cs.audio) opened = openInput(s, 1, *cs.audio, false, true, inputs[1], &error);
    if (opened) {
        for (int i = 0; i < 2; ++i) {
            if (!inputs[i].fmt) continue;
            readers.emplace_back([s, i, &inputs] {
                Input& in = inputs[i];
                while (!s->stopping) {
                    s->deadline[i] = msFromNow(std::max(5.0, s->settings.openTimeout));
                    AVPacket* p = av_packet_alloc();
                    const int err = av_read_frame(in.fmt, p);
                    Item item;
                    item.input = i;
                    if (err < 0) {
                        av_packet_free(&p);
                        if (!s->stopping && err != AVERROR_EOF) item.error = "The feed stopped: " + avError(err);
                        std::lock_guard lock(s->qm);
                        s->queue.push_back(item);
                        s->qc.notify_all();
                        return;
                    }
                    if (p->stream_index != in.videoStream && p->stream_index != in.audioStream) {
                        av_packet_free(&p);
                        continue;
                    }
                    item.packet = p;
                    std::lock_guard lock(s->qm);
                    if (s->queue.size() > 2000) {  // (falling behind: the oldest go)
                        av_packet_free(&s->queue.front().packet);
                        s->queue.pop_front();
                    }
                    s->queue.push_back(item);
                    s->qc.notify_all();
                }
            });
        }
        ok = true;
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
            }
            if (!item.packet) {
                if (!item.error.empty()) error = item.error;
                if (++ended >= live || item.input == 0) break;  // (the picture ended: so does the recording)
                continue;
            }
            Input& in = inputs[item.input];
            const bool isVideo = item.packet->stream_index == in.videoStream;
            AVCodecContext* dec = isVideo ? in.video : in.audio;
            if (dec && avcodec_send_packet(dec, item.packet) >= 0)
                while (ok && avcodec_receive_frame(dec, isVideo ? decoded : sound) >= 0) {
                    if (isVideo) {
                        ok = putPicture(decoded, in);
                        av_frame_unref(decoded);
                    } else {
                        AVFrame* a = av_frame_clone(sound);
                        av_frame_unref(sound);
                        ok = putSound(a, in, item.input == 0);
                        av_frame_free(&a);
                    }
                    if (timedOut) break;
                }
            av_packet_free(&item.packet);
        }
    }
    s->stopping = true;
    for (std::thread& t : readers) t.join();
    {
        std::lock_guard lock(s->qm);
        for (Item& i : s->queue) av_packet_free(&i.packet);
        s->queue.clear();
    }
    if (headerWritten) {
        // The encoders emptied and the file finished (a QuickTime file already holds all but the last fragment).
        if (venc) write(venc, vst, nullptr);
        if (aenc) write(aenc, ast, nullptr);
        const int err = av_write_trailer(oc);
        if (err < 0 && error.empty()) error = "Cannot finish " + cs.path + ": " + avError(err);
    } else if (error.empty()) {
        error = "No picture came from the feed";
    }
    if (oc) {
        if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
    avcodec_free_context(&venc);
    avcodec_free_context(&aenc);
    sws_freeContext(sws);
    swr_free(&swr);
    av_frame_free(&conv);
    av_frame_free(&last);
    av_frame_free(&decoded);
    av_frame_free(&sound);
    av_packet_free(&out);
    const bool fine = headerWritten && (error.empty() || error == "Stopped");
    {
        std::lock_guard lock(s->m);
        s->st.finished = true;
        s->st.recording = false;
        if (!fine && s->st.error.empty()) s->st.error = error;
    }
    s->running = false;
    if (onFinished) onFinished(fine, fine ? std::string() : error);
}

}  // namespace montage
