#include "LivePackage.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/Surround.h"
#include "media/Loudness.h"
#include "render/Ac3.h"
#include "render/TsSegments.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace montage {

namespace {

using Clock = std::chrono::steady_clock;

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s)
        if (c != '"' && c != '\n' && c != '\r') o += c;
    return o + "\"";
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : std::string(1, c);
    return o;
}

std::string isoDate(int64_t ms) { return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzzZ").toStdString(); }

int64_t wallMs() { return QDateTime::currentMSecsSinceEpoch(); }

// Written whole under a temporary name, then renamed into place: a player never reads half of it.
bool writeAtomically(const std::string& path, const void* data, size_t n) {
    const std::string tmp = path + ".tmp";
    {
        QFile f(QString::fromStdString(tmp));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        if (f.write(static_cast<const char*>(data), qint64(n)) != qint64(n)) return false;
    }
    // (rename replaces at once where it can; where it cannot (Windows), the old one goes first)
#ifndef _WIN32
    if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
#endif
    QFile::remove(QString::fromStdString(path));
    return QFile::rename(QString::fromStdString(tmp), QString::fromStdString(path));
}
bool writeAtomically(const std::string& path, const std::string& text) { return writeAtomically(path, text.data(), text.size()); }

std::vector<uint8_t> boxPayload(const uint8_t* d, size_t n, const char* type) {
    for (size_t i = 4; i + 4 <= n; ++i) {
        if (std::memcmp(d + i, type, 4) != 0) continue;
        const uint32_t size = uint32_t(d[i - 4]) << 24 | uint32_t(d[i - 3]) << 16 | uint32_t(d[i - 2]) << 8 | d[i - 1];
        if (size < 8 || i - 4 + size > n) continue;
        return {d + i + 4, d + i - 4 + size};
    }
    return {};
}

std::string pathName(const std::string& s) {
    std::string o;
    for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
    return o.empty() ? "und" : o;
}

std::string segName(int64_t k, const char* ext) {
    char b[48];
    std::snprintf(b, sizeof b, "seg%05lld.%s", static_cast<long long>(k), ext);
    return b;
}
std::string partName(int64_t k, int j) {
    char b[48];
    std::snprintf(b, sizeof b, "seg%05lld.%d.m4s", static_cast<long long>(k), j + 1);
    return b;
}

// One rendition's CMAF: the initialisation segment, then chunks (moof + mdat), each appended to its segment's file
// as it is made (and, with parts, written as a part file too).
class LiveCmaf {
public:
    ~LiveCmaf() {
        if (oc_) {
            if (oc_->pb) {
                uint8_t* b = nullptr;
                avio_close_dyn_buf(oc_->pb, &b);
                av_free(b);
                oc_->pb = nullptr;
            }
            avformat_free_context(oc_);
        }
    }
    bool open(const std::string& dir, const AVCodecParameters* par, AVRational tb, std::string* why) {
        dir_ = dir;
        delayed_ = par->codec_id == AV_CODEC_ID_AC3 || par->codec_id == AV_CODEC_ID_EAC3;
        if (avformat_alloc_output_context2(&oc_, nullptr, "mp4", nullptr) < 0 || !oc_) return *why = "cannot make an MP4 muxer", false;
        oc_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        st_ = avformat_new_stream(oc_, nullptr);
        if (!st_ || avcodec_parameters_copy(st_->codecpar, par) < 0) return *why = "cannot add the stream", false;
        st_->codecpar->codec_tag = par->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h', 'v', 'c', '1') : 0;
        st_->time_base = tb;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags",
                    delayed_ ? "+frag_custom+empty_moov+delay_moov+default_base_moof+skip_trailer" : "+frag_custom+empty_moov+default_base_moof+skip_trailer", 0);
        if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return *why = averr(rc), false;
        const int rc = avformat_write_header(oc_, &opts);
        av_dict_free(&opts);
        if (rc < 0) return *why = "cannot start the MP4 fragments: " + averr(rc), false;
        uint8_t* b = nullptr;
        const int n = avio_close_dyn_buf(oc_->pb, &b);
        oc_->pb = nullptr;
        init.assign(b, b + std::max(0, n));
        av_free(b);
        timescale = st_->time_base;
        if (!delayed_ && !writeAtomically(dir_ + "/init.mp4", init.data(), init.size())) return *why = "cannot write " + dir_ + "/init.mp4", false;
        return true;
    }
    bool chunkOpen() const { return oc_ && oc_->pb; }
    // A packet (its times in `from`) into the chunk being made.
    bool write(AVPacket* pkt, AVRational from, std::string* why) {
        if (!oc_->pb) {
            if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return *why = averr(rc), false;
            chunkStart = INT64_MAX, chunkEnd = INT64_MIN;
        }
        av_packet_rescale_ts(pkt, from, st_->time_base);
        pkt->stream_index = 0;
        chunkStart = std::min(chunkStart, pkt->pts);
        chunkEnd = std::max(chunkEnd, pkt->pts + std::max<int64_t>(pkt->duration, 0));
        if (int rc = av_write_frame(oc_, pkt); rc < 0) return *why = "cannot write a sample: " + averr(rc), false;
        return true;
    }
    // The chunk made: appended to segment `k`'s file (`partFile`: written as that part's file too).
    bool chunk(int64_t k, const std::string& partFile, std::string* why) {
        if (!chunkOpen()) return true;
        int64_t head = 0;
        if (delayed_ && init.empty()) {
            av_write_frame(oc_, nullptr);  // (the first flush writes the header alone)
            head = avio_tell(oc_->pb);
        }
        av_write_frame(oc_, nullptr);
        uint8_t* b = nullptr;
        const int n = avio_close_dyn_buf(oc_->pb, &b);
        oc_->pb = nullptr;
        const int64_t total = std::max(0, n);
        bool ok = true;
        if (head > 0) {
            init.assign(b, b + std::min(head, total));
            ok = writeAtomically(dir_ + "/init.mp4", init.data(), init.size());
            if (!ok) *why = "cannot write " + dir_ + "/init.mp4";
        }
        const uint8_t* c = b + std::min(head, total);
        const size_t size = size_t(total - std::min(head, total));
        if (ok && !partFile.empty() && !(ok = writeAtomically(dir_ + "/" + partFile, c, size))) *why = "cannot write " + dir_ + "/" + partFile;
        if (ok && fileFor_ != k) {
            file_ = std::make_unique<QFile>(QString::fromStdString(dir_ + "/" + segName(k, "m4s")));
            fileFor_ = k;
            if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) ok = false, *why = "cannot write " + dir_ + "/" + segName(k, "m4s");
        }
        if (ok && (file_->write(reinterpret_cast<const char*>(c), qint64(size)) != qint64(size) || !file_->flush()))
            ok = false, *why = "cannot write " + dir_ + "/" + segName(k, "m4s");
        av_free(b);
        return ok;
    }
    void endSegment() {
        file_.reset();
        fileFor_ = -1;
    }
    double seconds(int64_t t) const { return double(t) * timescale.num / timescale.den; }

    std::vector<uint8_t> init;
    AVRational timescale{1, 1};
    int64_t chunkStart = 0, chunkEnd = 0;

private:
    std::string dir_;
    bool delayed_ = false;
    AVFormatContext* oc_ = nullptr;
    AVStream* st_ = nullptr;
    std::unique_ptr<QFile> file_;
    int64_t fileFor_ = -1;
};

struct Part {
    double seconds = 0;
    bool independent = false;
};

struct Segment {
    int64_t number = 0;
    double start = 0, seconds = 0;  // presentation, from the first picture
    std::vector<Part> parts;
    bool complete = false;
};

struct Rendition {
    std::string name, codecs;
    bool video = true;
    // Pictures.
    StreamingRung rung;
    int width = 0, height = 0;
    AVCodecContext* enc = nullptr;
    SwsContext* sws = nullptr;
    int swsKey[3] = {0, 0, -1};
    AVFrame* frame = nullptr;
    // Sound.
    std::unique_ptr<Eac3Extended> extended;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    int frameSize = 1024, channels = 2, sampleRate = 48000;
    int64_t nextPts = 0;
    AVChannelLayout layout{};
    // Out.
    AVPacket* pkt = nullptr;
    std::unique_ptr<LiveCmaf> cmaf;
    std::unique_ptr<TsSegmenter> ts;
    std::deque<Segment> segments;  // the newest last (the one being made, while not complete)
    int64_t current = 0;           // the segment being made (from 1), 0 before the first
    int part = -1;                 // its part being made
    double chunkSeconds = 0;       // the samples' in the chunk being made
    bool chunkHasData = false;
    double segmentSoFar = 0;       // the chunks' finished in the segment being made
    double soundStart = -1;        // the first packet's time (sound)
    bool firstChunkOfSegment = true;
    std::string error;
    ~Rendition() {
        avcodec_free_context(&enc);
        sws_freeContext(sws);
        av_frame_free(&frame);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo);
        av_packet_free(&pkt);
        av_channel_layout_uninit(&layout);
    }
};

struct Item {
    AVFrame* frame = nullptr;
    bool video = true;
    int fpsNum = 25, fpsDen = 1;
    bool end = false;
};

}  // namespace

struct LivePackager::State {
    LiveSettings set;
    std::string folder;
    std::thread thread;
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<Item> queue;
    LiveStatus st;
    std::set<std::string> growingFiles;
    bool stopping = false;

    // On the packager's thread.
    std::vector<std::unique_ptr<Rendition>> video, audio;
    bool opened = false, failed = false, mastered = false;
    AVRational fps{25, 1};
    int64_t ast = 0;                  // the wall clock at the presentation's 0, ms (fixed once a playlist or the MPD states it)
    // The earliest the pictures came against their times: each picture's arrival less its time in the presentation,
    // the least of those until the timeline is first stated (so a recorder's first pictures, handed over together
    // as it catches up with the feed, do not put the presentation's 0 later than the pictures could be had). Nothing
    // is stated before the first segment is made, by when the feed has settled.
    int64_t epoch = INT64_MAX, firstPushed = AV_NOPTS_VALUE;
    bool published = false, firstSegment = false;
    int64_t encodedSegment = -1;  // the segment (from 0) of the last picture encoded
    int64_t firstPicture = 0;         // its number in the recording
    int64_t segFrames = 50, partFrames = 50;
    double segSeconds = 2, partSeconds = 2, partTarget = 2;
    bool parts = false, ts = false, cmaf = true;
    std::unique_ptr<LoudnessMeter> loudness;
    std::vector<double> weights;
    int dialnorm = -24;
    std::vector<float> interleaved;

    ~State() {
        for (Item& i : queue) av_frame_free(&i.frame);
    }

    void setGrowing(const std::string& rel, bool on) {
        std::lock_guard lock(m);
        if (on) growingFiles.insert(rel);
        else growingFiles.erase(rel);
    }
    bool fail(const std::string& why) {
        std::lock_guard lock(m);
        if (st.error.empty()) st.error = why;
        failed = true;
        return false;
    }

    // ---- Opening ----

    bool openVideoEncoder(Rendition& r) {
        const AVCodec* codec = avcodec_find_encoder_by_name(r.rung.codec.c_str());
        avcodec_free_context(&r.enc);
        r.enc = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!r.enc) return fail("This build has no " + r.rung.codec + " encoder");
        AVCodecContext* e = r.enc;
        e->width = r.width, e->height = r.height;
        e->time_base = {fps.den, fps.num};
        e->framerate = fps;
        e->sample_aspect_ratio = {1, 1};
        e->pix_fmt = AV_PIX_FMT_YUV420P;
        e->color_primaries = AVCOL_PRI_BT709, e->color_trc = AVCOL_TRC_BT709, e->colorspace = AVCOL_SPC_BT709;
        e->color_range = AVCOL_RANGE_MPEG;
        e->chroma_sample_location = AVCHROMA_LOC_LEFT;
        e->bit_rate = r.rung.bitrate;
        if (r.rung.codec != "libsvtav1") e->rc_max_rate = r.rung.bitrate * 3 / 2;
        e->rc_buffer_size = int(std::min<int64_t>(INT32_MAX, r.rung.bitrate));
        e->gop_size = int(segFrames * 2 + 1);  // (keyframes where forced: each segment's first picture)
        e->keyint_min = 1;
        e->max_b_frames = 0;
        e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        const std::string preset = set.preset.empty() ? "veryfast" : set.preset;
        if (r.rung.codec == "libsvtav1") {
            static const std::map<std::string, int> svt = {{"ultrafast", 12}, {"superfast", 11}, {"veryfast", 10}, {"faster", 9}, {"fast", 8},
                                                           {"medium", 6}, {"slow", 5}, {"slower", 4}, {"veryslow", 3}};
            const auto it = svt.find(preset);
            av_dict_set(&opts, "preset", it != svt.end() ? std::to_string(it->second).c_str() : preset.c_str(), 0);
            // (low delay: each picture coded as it comes, referring only back; a new encoder each segment starts it
            // on a keyframe, as SVT-AV1 takes no keyframe on request)
            av_dict_set(&opts, "svtav1-params", "pred-struct=1:scd=0", 0);
            e->flags |= AV_CODEC_FLAG_CLOSED_GOP;
        } else {
            av_dict_set(&opts, "preset", preset.c_str(), 0);
            av_dict_set(&opts, "tune", "zerolatency", 0);
            av_dict_set(&opts, "forced-idr", "1", 0);
            av_dict_set(&opts, r.rung.codec == "libx264" ? "x264-params" : "x265-params",
                        r.rung.codec == "libx264" ? "scenecut=0:open-gop=0" : "log-level=error:scenecut=0:open-gop=0", 0);
        }
        const int rc = avcodec_open2(e, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) return fail("Cannot open the " + r.rung.codec + " encoder: " + averr(rc));
        return true;
    }

    std::string renditionDir(const std::string& name, std::string* why) {
        QDir d(QString::fromStdString(folder + "/" + name));
        // (what an earlier package left there goes; a folder of that name holding anything else is left alone)
        if (d.exists() && !d.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (!streamingRenditionFolder(d.absolutePath().toStdString())) {
                *why = folder + "/" + name + " holds other files: choose another folder for the package";
                return {};
            }
            d.removeRecursively();
        }
        if (!QDir().mkpath(d.absolutePath())) {
            *why = "Cannot make " + folder + "/" + name;
            return {};
        }
        return d.absolutePath().toStdString();
    }

    bool openOutputs(Rendition& r, const AVCodecParameters* par, AVRational tb) {
        std::string why;
        const std::string dir = renditionDir(r.name, &why);
        if (dir.empty()) return fail(why);
        if (cmaf) {
            r.cmaf = std::make_unique<LiveCmaf>();
            if (!r.cmaf->open(dir, par, tb, &why)) return fail(why);
        }
        if (ts) {
            r.ts = std::make_unique<TsSegmenter>();
            if (!r.ts->open(par, &why)) return fail(why);
        }
        r.pkt = av_packet_alloc();
        return true;
    }

    bool openVideo(const AVFrame* f) {
        std::vector<StreamingRung> rungs = set.rungs;
        if (rungs.empty()) {
            rungs = defaultLadder(f->width, f->height, set.codec);
            if (rungs.size() > 2) rungs.resize(2);
            if (f->height <= 480 && rungs.size() > 1) rungs.resize(1);
        }
        for (const StreamingRung& rung : rungs) {
            if (rung.codec != "libx264" && rung.codec != "libx265" && rung.codec != "libsvtav1")
                return fail("A rung's codec must be libx264, libx265 or libsvtav1");
            if (ts && rung.codec != "libx264")
                return fail(std::string("MPEG-TS segments carry H.264, not ") + (rung.codec == "libx265" ? "HEVC" : "AV1") + ": choose CMAF segments");
            auto r = std::make_unique<Rendition>();
            r->rung = rung;
            r->width = std::min(rung.width > 0 ? rung.width : f->width, f->width) & ~1;
            r->height = rung.height > 0 ? std::min(rung.height, f->height) & ~1 : int(std::lround(double(r->width) * f->height / f->width / 2)) * 2;
            r->name = "v" + std::to_string(video.size() + 1) + "_" + std::to_string(r->height) + "p_" +
                      (rung.codec == "libx265" ? "hevc" : rung.codec == "libsvtav1" ? "av1" : "h264");
            if (!openVideoEncoder(*r)) return false;
            r->frame = av_frame_alloc();
            r->frame->format = AV_PIX_FMT_YUV420P, r->frame->width = r->width, r->frame->height = r->height;
            if (av_frame_get_buffer(r->frame, 0) < 0) return fail("Out of memory");
            std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> par(avcodec_parameters_alloc(), [](AVCodecParameters* q) { avcodec_parameters_free(&q); });
            avcodec_parameters_from_context(par.get(), r->enc);
            if (!openOutputs(*r, par.get(), r->enc->time_base)) return false;
            const char* name = avcodec_get_name(par->codec_id);
            const char* box = par->codec_id == AV_CODEC_ID_H264 ? "avcC" : par->codec_id == AV_CODEC_ID_HEVC ? "hvcC" : "av1C";
            if (r->cmaf) {
                r->codecs = streamingCodecName(name, boxPayload(r->cmaf->init.data(), r->cmaf->init.size(), box), par->profile);
            } else {
                // (no CMAF made: the configuration from an MP4 header written aside)
                LiveCmaf probe;
                std::string why;
                const std::string tmp = folder + "/" + r->name;
                if (probe.open(tmp, par.get(), r->enc->time_base, &why))
                    r->codecs = streamingCodecName(name, boxPayload(probe.init.data(), probe.init.size(), box), par->profile);
                QFile::remove(QString::fromStdString(tmp + "/init.mp4"));
            }
            video.push_back(std::move(r));
        }
        return !video.empty() || fail("The ladder is empty");
    }

    bool openAudio(const AVFrame* f) {
        const std::string codecName = set.audioCodec;
        const AVCodec* codec = avcodec_find_encoder_by_name(codecName == "eac3" ? "eac3" : codecName.c_str());
        if (!codec) return fail("This build has no " + codecName + " encoder");
        if (ts && codecName == "libopus") return fail("MPEG-TS segments carry AAC, AC-3 or E-AC-3 sound, not Opus");
        auto r = std::make_unique<Rendition>();
        r->video = false;
        r->name = "a1_" + pathName(set.audioLanguage.empty() ? "und" : set.audioLanguage);
        const int in = f->ch_layout.nb_channels;
        // The layout: the feed's stereo, 5.1 or 7.1 where the codec carries it (AC-3 up to 5.1), else stereo.
        const int maxChannels = codecName == "ac3" ? 6 : 8;
        const int out = in == 8 && maxChannels >= 8 ? 8 : in >= 6 ? 6 : in == 1 ? 1 : 2;
        av_channel_layout_default(&r->layout, out);
        if (out == 6) av_channel_layout_from_mask(&r->layout, AV_CH_LAYOUT_5POINT1);
        if (out == 8) av_channel_layout_from_mask(&r->layout, AV_CH_LAYOUT_7POINT1);
        r->channels = out;
        AVCodecParameters* par = avcodec_parameters_alloc();
        std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> hold(par, [](AVCodecParameters* q) { avcodec_parameters_free(&q); });
        AVSampleFormat format = AV_SAMPLE_FMT_FLTP;
        AVRational tb{1, 48000};
        Ac3Metadata meta;
        if (codecName == "eac3" && out == 8) {
            r->extended = std::make_unique<Eac3Extended>();
            std::string why;
            if (!r->extended->open(48000, std::clamp<int64_t>(set.audioBitrate * 4, 256000, 2048000), meta, &why)) return fail(why);
            AVCodecContext* d = r->extended->describe();
            avcodec_parameters_from_context(par, d);
            avcodec_free_context(&d);
            r->frameSize = 1536;
        } else {
            r->enc = avcodec_alloc_context3(codec);
            AVCodecContext* e = r->enc;
            av_channel_layout_copy(&e->ch_layout, &r->layout);
            e->sample_rate = 48000;
            e->sample_fmt = codecName == "libopus" ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
            e->bit_rate = codecName == "ac3" || codecName == "eac3" ? std::clamp<int64_t>(set.audioBitrate * std::max(2, out) / 2, 96000, codecName == "ac3" ? 640000 : 1024000)
                                                                    : set.audioBitrate * std::max(1, out / 2);
            e->time_base = tb;
            e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* opts = nullptr;
            if (codecName == "ac3" || codecName == "eac3")
                ac3EncoderOptions(meta, codecName == "eac3", out >= 3 && out != 4, out >= 4, &opts);
            const int rc = avcodec_open2(e, codec, &opts);
            av_dict_free(&opts);
            if (rc < 0) return fail("Cannot open the " + codecName + " encoder: " + averr(rc));
            avcodec_parameters_from_context(par, e);
            format = e->sample_fmt;
            r->frameSize = e->frame_size > 0 ? e->frame_size : 1024;
        }
        if (codecName == "ac3" || codecName == "eac3") {
            loudness = std::make_unique<LoudnessMeter>(48000);
            const std::string name = out == 8 ? "7.1" : out == 6 ? "5.1" : "stereo";
            weights.clear();
            for (const Speaker& sp : layoutSpeakers(name)) weights.push_back(sp.loudnessWeight);
            weights.resize(size_t(out), 1.0);
        }
        r->fifo = av_audio_fifo_alloc(format, out, r->frameSize * 4);
        r->frame = av_frame_alloc();
        if (!r->fifo || !r->frame) return fail("Out of memory");
        if (!openOutputs(*r, par, tb)) return false;
        const char* name = avcodec_get_name(par->codec_id);
        r->codecs = streamingCodecName(name ? name : "", {}, par->profile);
        audio.push_back(std::move(r));
        return true;
    }

    bool open(const Item& first) {
        fps = AVRational{first.fpsNum > 0 ? first.fpsNum : 25, first.fpsDen > 0 ? first.fpsDen : 1};
        const double rate = av_q2d(fps);
        ts = set.segmentFormat == "ts" && set.hls;
        cmaf = !ts || set.dash;
        parts = set.lowLatency && !ts;
        partFrames = std::max<int64_t>(1, std::llround(std::clamp(set.partSeconds, 0.1, 1.0) * rate));
        segFrames = std::max<int64_t>(1, std::llround(std::clamp(set.segmentSeconds, 1.0, 6.0) * rate));
        if (parts) segFrames = std::max<int64_t>(1, std::llround(double(segFrames) / double(partFrames))) * partFrames;
        else partFrames = segFrames;
        segSeconds = double(segFrames) / rate;
        partSeconds = double(partFrames) / rate;
        // (a part of sound runs on to the end of the packet across its end: a coded frame longer at most)
        partTarget = std::ceil((partSeconds + 0.033) * 1000) / 1000;
        ast = wallMs();
        {
            std::lock_guard lock(m);
            if (epoch != INT64_MAX) ast = epoch;
        }
        firstPicture = first.frame->pts == AV_NOPTS_VALUE ? 0 : first.frame->pts;
        if (!openVideo(first.frame)) return false;
        opened = true;
        return true;
    }

    // ---- Playlists and the MPD ----

    std::string mediaPlaylist(const Rendition& r, bool ended) {
        const bool tsList = ts;
        std::vector<const Segment*> done;
        for (const Segment& s : r.segments)
            if (s.complete) done.push_back(&s);
        while (int(done.size()) > set.window) done.erase(done.begin());
        const Segment* making = !r.segments.empty() && !r.segments.back().complete ? &r.segments.back() : nullptr;
        double longest = segSeconds;
        for (const Segment* s : done) longest = std::max(longest, s->seconds);
        std::string m = "#EXTM3U\n#EXT-X-VERSION:" + std::string(tsList ? "3" : parts ? "9" : "7") + "\n#EXT-X-TARGETDURATION:" +
                        std::to_string(std::max<long long>(1, std::llround(longest))) + "\n";
        if (parts) {
            m += "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=" + fmt("%.3f", partTarget * 3) + "\n";
            m += "#EXT-X-PART-INF:PART-TARGET=" + fmt("%.3f", partTarget) + "\n";
        }
        const int64_t first = !done.empty() ? done.front()->number : making ? making->number : 1;
        m += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(first) + "\n";
        if (!tsList) m += "#EXT-X-MAP:URI=\"init.mp4\"\n";
        bool dated = false;
        auto date = [&](const Segment& s) {
            if (dated) return;
            m += "#EXT-X-PROGRAM-DATE-TIME:" + isoDate(ast + int64_t(std::llround(s.start * 1000))) + "\n";
            dated = true;
        };
        auto listParts = [&](const Segment& s) {
            for (size_t j = 0; j < s.parts.size(); ++j)
                m += "#EXT-X-PART:DURATION=" + fmt("%.5f", s.parts[j].seconds) + ",URI=\"" + partName(s.number, int(j)) + "\"" +
                     (s.parts[j].independent ? ",INDEPENDENT=YES" : "") + "\n";
        };
        for (size_t i = 0; i < done.size(); ++i) {
            date(*done[i]);
            // (parts for the newest segments only, as the live edge needs them)
            if (parts && !ended && i + 2 >= done.size()) listParts(*done[i]);
            m += "#EXTINF:" + fmt("%.5f", done[i]->seconds) + ",\n" + segName(done[i]->number, tsList ? "ts" : "m4s") + "\n";
        }
        if (parts && !ended) {
            if (making) {
                date(*making);
                listParts(*making);
            }
            const int64_t k = making ? making->number : (done.empty() ? 1 : done.back()->number + 1);
            const int j = making ? int(making->parts.size()) : 0;
            m += "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"" + partName(k, j) + "\"\n";
        }
        if (ended) m += "#EXT-X-ENDLIST\n";
        return m;
    }

    bool writeMaster() {
        if (!set.hls) return true;
        std::string m = "#EXTM3U\n#EXT-X-VERSION:" + std::string(ts ? "3" : parts ? "9" : "7") + "\n#EXT-X-INDEPENDENT-SEGMENTS\n";
        int64_t audioRate = 0;
        for (const auto& a : audio) {
            audioRate = std::max<int64_t>(audioRate, a->extended ? set.audioBitrate * 4 : a->enc ? a->enc->bit_rate : set.audioBitrate);
            m += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=" + quoted(set.audioName.empty() ? "Main" : set.audioName) + ",LANGUAGE=" +
                 quoted(set.audioLanguage.empty() ? "und" : set.audioLanguage) + ",DEFAULT=YES,AUTOSELECT=YES,CHANNELS=\"" + std::to_string(a->channels) +
                 "\",URI=\"" + a->name + "/playlist.m3u8\"\n";
        }
        const std::string audioCodecs = audio.empty() ? "" : "," + audio.front()->codecs;
        for (const auto& v : video)
            m += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(v->rung.bitrate * 3 / 2 + audioRate) + ",AVERAGE-BANDWIDTH=" +
                 std::to_string(v->rung.bitrate + audioRate) + ",CODECS=\"" + v->codecs + audioCodecs + "\",RESOLUTION=" + std::to_string(v->width) + "x" +
                 std::to_string(v->height) + ",FRAME-RATE=" + fmt("%.3f", av_q2d(fps)) + ",VIDEO-RANGE=SDR" + (audio.empty() ? "" : ",AUDIO=\"audio\"") +
                 ",CLOSED-CAPTIONS=NONE\n" + v->name + "/playlist.m3u8\n";
        return writeAtomically(folder + "/master.m3u8", m) || fail("Cannot write " + folder + "/master.m3u8");
    }

    // The presentation's 0 on the wall clock, fixed as it is first stated.
    void fixEpoch() {
        if (published) return;
        std::lock_guard lock(m);
        if (epoch != INT64_MAX) ast = epoch;
        published = true;
    }

    bool writeMpd(bool ended, double duration) {
        if (!set.dash || (!firstSegment && !ended)) return true;
        fixEpoch();
        const double depth = segSeconds * set.window;
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" "
                        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" type=\"dynamic\" availabilityStartTime=\"" + isoDate(ast) +
                        "\" publishTime=\"" + isoDate(wallMs()) + "\"";
        if (ended) x += " mediaPresentationDuration=\"PT" + fmt("%.3f", duration) + "S\"";
        else x += " minimumUpdatePeriod=\"PT" + fmt("%.3f", segSeconds) + "S\"";
        x += " timeShiftBufferDepth=\"PT" + fmt("%.3f", depth) + "S\" maxSegmentDuration=\"PT" + fmt("%.3f", segSeconds) + "S\" minBufferTime=\"PT" +
             fmt("%.3f", parts ? partSeconds : segSeconds) + "S\" suggestedPresentationDelay=\"PT" + fmt("%.3f", parts ? partTarget * 3 : segSeconds * 3) +
             "S\">\n";
        if (parts) {
            const int target = int(std::lround(partTarget * 3 * 1000));
            x += "<ServiceDescription id=\"0\"><Latency referenceId=\"0\" target=\"" + std::to_string(target) + "\" min=\"" + std::to_string(target / 2) +
                 "\" max=\"" + std::to_string(target * 2) + "\"/><PlaybackRate min=\"0.96\" max=\"1.04\"/></ServiceDescription>\n";
        }
        x += "<Period id=\"0\" start=\"PT0S\">\n";
        auto templ = [&](const Rendition& r) {
            const int64_t scale = r.cmaf ? r.cmaf->timescale.den / std::max(1, r.cmaf->timescale.num) : 90000;
            std::string t = "<SegmentTemplate timescale=\"" + std::to_string(scale) + "\" duration=\"" + std::to_string(std::llround(segSeconds * double(scale))) +
                            "\" startNumber=\"1\" initialization=\"" + r.name + "/init.mp4\" media=\"" + r.name + "/seg$Number%05d$.m4s\"";
            if (parts) t += " availabilityTimeOffset=\"" + fmt("%.3f", segSeconds - partSeconds) + "\" availabilityTimeComplete=\"false\"";
            return t + "/>\n";
        };
        int id = 1;
        if (!video.empty()) {
            x += "<AdaptationSet id=\"" + std::to_string(id++) + "\" contentType=\"video\" mimeType=\"video/mp4\" segmentAlignment=\"true\" startWithSAP=\"1\" "
                 "frameRate=\"" + std::to_string(fps.num) + "/" + std::to_string(fps.den) + "\">\n";
            for (const auto& v : video)
                x += "<Representation id=\"" + v->name + "\" bandwidth=\"" + std::to_string(v->rung.bitrate * 3 / 2) + "\" codecs=\"" + v->codecs +
                     "\" width=\"" + std::to_string(v->width) + "\" height=\"" + std::to_string(v->height) + "\" sar=\"1:1\">\n" + templ(*v) + "</Representation>\n";
            x += "</AdaptationSet>\n";
        }
        for (const auto& a : audio) {
            x += "<AdaptationSet id=\"" + std::to_string(id++) + "\" contentType=\"audio\" mimeType=\"audio/mp4\" lang=\"" +
                 xmlEscape(set.audioLanguage.empty() ? "und" : set.audioLanguage) + "\" segmentAlignment=\"true\">\n<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"main\"/>\n";
            x += "<Representation id=\"" + a->name + "\" bandwidth=\"" + std::to_string(a->enc ? a->enc->bit_rate : set.audioBitrate * 4) + "\" codecs=\"" + a->codecs +
                 "\" audioSamplingRate=\"48000\">\n<AudioChannelConfiguration schemeIdUri=\"urn:mpeg:mpegB:cicp:ChannelConfiguration\" value=\"" +
                 std::to_string(a->channels == 8 ? 12 : a->channels) + "\"/>\n" + templ(*a) + "</Representation>\n</AdaptationSet>\n";
        }
        x += "</Period>\n<UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\" value=\"" + isoDate(wallMs()) + "\"/>\n</MPD>\n";
        return writeAtomically(folder + "/manifest.mpd", x) || fail("Cannot write " + folder + "/manifest.mpd");
    }

    // ---- Segments ----

    bool writePlaylist(Rendition& r, bool ended = false) {
        if (!set.hls || (!firstSegment && !ended)) return true;
        fixEpoch();
        return writeAtomically(folder + "/" + r.name + "/playlist.m3u8", mediaPlaylist(r, ended)) || fail("Cannot write " + r.name + "'s playlist");
    }

    // The chunk being made is done (a part, or with no parts the segment's one).
    bool endChunk(Rendition& r) {
        if (r.current <= 0 || r.segments.empty() || !r.chunkHasData) return true;
        r.chunkHasData = false;
        Segment& s = r.segments.back();
        std::string why;
        double seconds = 0;
        if (r.cmaf && r.cmaf->chunkOpen()) {
            seconds = r.cmaf->seconds(r.cmaf->chunkEnd - r.cmaf->chunkStart);
            const std::string part = parts ? partName(r.current, r.part) : std::string();
            const std::string rel = r.name + "/" + segName(r.current, "m4s");
            setGrowing(rel, true);
            if (!r.cmaf->chunk(r.current, part, &why)) return fail(why);
        } else {
            seconds = r.chunkSeconds;
        }
        r.chunkSeconds = 0;
        r.segmentSoFar += seconds;
        if (parts) {
            s.parts.push_back({seconds, r.video ? r.firstChunkOfSegment : true});
            r.firstChunkOfSegment = false;
            return writePlaylist(r);
        }
        return true;
    }

    bool endSegment(Rendition& r, bool last = false) {
        if (r.current <= 0 || r.segments.empty()) return true;
        if (!endChunk(r)) return false;
        Segment& s = r.segments.back();
        std::string why;
        double seconds = r.segmentSoFar;
        if (r.ts && r.ts->started()) {
            std::vector<uint8_t> bytes;
            if (!r.ts->end(bytes, &why)) return fail(why);
            seconds = double(r.ts->finish - r.ts->start) / 90000;
            if (r.video && !r.ts->keyFirst) return fail(r.name + "'s segment " + std::to_string(r.current) + " does not start on a keyframe");
            if (!writeAtomically(folder + "/" + r.name + "/" + segName(r.current, "ts"), bytes.data(), bytes.size()))
                return fail("Cannot write " + r.name + "/" + segName(r.current, "ts"));
        }
        if (r.cmaf) r.cmaf->endSegment();
        setGrowing(r.name + "/" + segName(r.current, "m4s"), false);
        s.seconds = seconds > 0 ? seconds : segSeconds;
        s.complete = true;
        if (!firstSegment && r.video && &r == video.front().get()) {
            // (the timeline stated from now: every rendition's playlist so far)
            firstSegment = true;
            for (auto& o : video)
                if (o.get() != &r && !writePlaylist(*o)) return false;
            for (auto& o : audio)
                if (!writePlaylist(*o)) return false;
        }
        // The window: the playlists list `window` segments; the files of those a little older go (a player may still
        // be fetching them).
        while (int(r.segments.size()) > set.window + 3) {
            const Segment& old = r.segments.front();
            const QString dir = QString::fromStdString(folder + "/" + r.name + "/");
            QFile::remove(dir + QString::fromStdString(segName(old.number, "m4s")));
            QFile::remove(dir + QString::fromStdString(segName(old.number, "ts")));
            for (size_t j = 0; j < old.parts.size(); ++j) QFile::remove(dir + QString::fromStdString(partName(old.number, int(j))));
            r.segments.pop_front();
        }
        if (r.video && &r == video.front().get()) {
            std::lock_guard lock(m);
            st.segments = r.current;
            int64_t complete = 0;
            for (const Segment& g : r.segments) complete += g.complete;
            st.firstListed = r.current - std::min<int64_t>(complete, set.window) + 1;
            st.dialnorm = dialnorm;
        }
        if (!last && r.video && &r == video.front().get()) {
            // (the dialogue level as measured so far, from the next segment)
            if (loudness) {
                const LoudnessResult lr = loudness->result();
                if (lr.valid) dialnorm = ac3Dialnorm(lr.integrated);
                for (auto& a : audio) {
                    if (a->extended) a->extended->setDialnorm(dialnorm);
                    else if (a->enc) ac3SetDialnorm(a->enc, dialnorm);
                }
            }
            if (mastered && !writeMpd(false, 0)) return false;
        }
        return writePlaylist(r, last);
    }

    // Where a packet of a rendition goes: segment `k` (from 1), part `j`.
    bool place(Rendition& r, int64_t k, int j) {
        if (k != r.current) {
            if (!endSegment(r)) return false;
            r.current = k;
            r.part = j;
            r.firstChunkOfSegment = true;
            r.segmentSoFar = 0;
            Segment s;
            s.number = k;
            s.start = double(k - 1) * segSeconds;
            r.segments.push_back(s);
            if (r.ts) {
                std::string why;
                if (!r.ts->begin(&why)) return fail(why);
            }
        } else if (j != r.part) {
            if (!endChunk(r)) return false;
            r.part = j;
        }
        return true;
    }

    bool writePacket(Rendition& r, AVPacket* pkt, AVRational tb) {
        std::string why;
        if (r.ts) {
            AVPacket* copy = av_packet_clone(pkt);
            const bool ok = copy && r.ts->write(copy, tb.num, tb.den, &why);
            av_packet_free(&copy);
            if (!ok) return fail(why.empty() ? "Out of memory" : why);
        }
        r.chunkSeconds += double(pkt->duration) * tb.num / tb.den;
        r.chunkHasData = true;
        if (r.cmaf && !r.cmaf->write(pkt, tb, &why)) return fail(why);
        return true;
    }

    bool takeVideo(Rendition& r) {
        for (;;) {
            const int rc = avcodec_receive_packet(r.enc, r.pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return fail("Encoding failed: " + averr(rc));
            if (r.pkt->duration <= 0) r.pkt->duration = 1;
            const int64_t i = r.pkt->pts;
            const int64_t k = i / segFrames + 1;
            const int j = int((i % segFrames) / partFrames);
            if (k != r.current && !(r.pkt->flags & AV_PKT_FLAG_KEY))
                return fail("The " + r.rung.codec + " encoder did not start segment " + std::to_string(k) + " on a keyframe");
            if (!place(r, k, j)) return false;
            if (!writePacket(r, r.pkt, r.enc->time_base)) return false;
            av_packet_unref(r.pkt);
            // (a part ends with its last picture: sent then, not when the next comes)
            if ((i + 1) % partFrames == 0 && parts && !endChunk(r)) return false;
            if ((i + 1) % partFrames == 0 && parts) r.part = int(((i + 1) % segFrames) / partFrames);
        }
    }

    bool takeAudio(Rendition& r) {
        for (;;) {
            const int rc = r.extended ? r.extended->receive(r.pkt) : avcodec_receive_packet(r.enc, r.pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return fail("Encoding the sound failed: " + (r.extended && !r.extended->error.empty() ? r.extended->error : averr(rc)));
            // (the encoder's priming, before 0, is left out: the presentation starts with the pictures)
            if (r.pkt->pts == AV_NOPTS_VALUE || r.pkt->pts < 0) {
                av_packet_unref(r.pkt);
                continue;
            }
            if (r.pkt->duration <= 0) r.pkt->duration = r.frameSize;
            const double t = double(r.pkt->pts) / 48000;
            const int64_t k = int64_t(std::floor(t / segSeconds + 1e-9)) + 1;
            const int j = parts ? int(std::floor((t - double(k - 1) * segSeconds) / partSeconds + 1e-9)) : 0;
            if (!place(r, k, std::max(0, j))) return false;
            if (!writePacket(r, r.pkt, {1, 48000})) return false;
            av_packet_unref(r.pkt);
        }
    }

    bool encodePicture(const AVFrame* f) {
        const int64_t index = (f->pts == AV_NOPTS_VALUE ? 0 : f->pts) - firstPicture;
        if (index < 0) return true;
        // (the first picture of a segment, or the first that came of it when the segment's first was dropped: a
        // keyframe)
        const int64_t segment = index / segFrames, before = encodedSegment;
        const bool start = segment != before;
        encodedSegment = segment;
        for (auto& v : video) {
            Rendition& r = *v;
            if (start && before >= 0 && r.rung.codec == "libsvtav1") {
                // (the segment's pictures all out, then a new encoder whose first picture is a keyframe)
                avcodec_send_frame(r.enc, nullptr);
                if (!takeVideo(r)) return false;
                if (!openVideoEncoder(r)) return false;
            }
            const int key[3] = {f->width, f->height, f->format};
            if (!r.sws || std::memcmp(key, r.swsKey, sizeof key) != 0) {
                sws_freeContext(r.sws);
                r.sws = sws_getContext(f->width, f->height, AVPixelFormat(f->format), r.width, r.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr,
                                       nullptr, nullptr);
                if (!r.sws) return fail("Cannot scale the pictures");
                std::memcpy(r.swsKey, key, sizeof key);
            }
            if (av_frame_make_writable(r.frame) < 0) return fail("Out of memory");
            sws_scale(r.sws, f->data, f->linesize, 0, f->height, r.frame->data, r.frame->linesize);
            r.frame->pts = index;
            r.frame->pict_type = start ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            if (int rc = avcodec_send_frame(r.enc, r.frame); rc < 0) return fail("Encoding failed: " + averr(rc));
            if (!takeVideo(r)) return false;
        }
        return true;
    }

    bool encodeSoundFifo(Rendition& r, bool final) {
        while (av_audio_fifo_size(r.fifo) >= r.frameSize || (final && av_audio_fifo_size(r.fifo) > 0)) {
            const int n = std::min(r.frameSize, av_audio_fifo_size(r.fifo));
            av_frame_unref(r.frame);
            r.frame->nb_samples = n;
            r.frame->format = r.extended ? AV_SAMPLE_FMT_FLTP : r.enc->sample_fmt;
            r.frame->sample_rate = 48000;
            av_channel_layout_copy(&r.frame->ch_layout, &r.layout);
            if (av_frame_get_buffer(r.frame, 0) < 0) return fail("Out of memory");
            av_audio_fifo_read(r.fifo, reinterpret_cast<void**>(r.frame->data), n);
            r.frame->pts = r.nextPts;
            r.nextPts += n;
            if (loudness && r.frame->format == AV_SAMPLE_FMT_FLTP) {
                interleaved.resize(size_t(n) * size_t(r.channels));
                for (int c = 0; c < r.channels; ++c) {
                    const float* p = reinterpret_cast<const float*>(r.frame->data[c]);
                    for (int i = 0; i < n; ++i) interleaved[size_t(i) * size_t(r.channels) + size_t(c)] = p[i];
                }
                loudness->addChannels(interleaved.data(), n, r.channels, weights.data());
            }
            const int rc = r.extended ? r.extended->send(r.frame) : avcodec_send_frame(r.enc, r.frame);
            if (rc < 0) return fail("Encoding the sound failed: " + averr(rc));
            if (!takeAudio(r)) return false;
        }
        return true;
    }

    bool encodeSound(const AVFrame* f) {
        if (audio.empty()) {
            if (set.audioCodec.empty() || set.audioCodec == "none" || !opened) return true;
            // (sound starting a segment or more after the pictures is left out: the renditions were announced)
            if (mastered) return true;
            if (!openAudio(f)) return false;
            audio.back()->nextPts = (f->pts == AV_NOPTS_VALUE ? 0 : f->pts) * 48000 / std::max(1, f->sample_rate) -
                                    firstPicture * 48000 * fps.den / std::max(1, fps.num);
            if (audio.back()->nextPts < 0) audio.back()->nextPts = 0;
        }
        Rendition& r = *audio.front();
        if (!r.swr) {
            if (swr_alloc_set_opts2(&r.swr, &r.layout, r.extended ? AV_SAMPLE_FMT_FLTP : r.enc->sample_fmt, 48000, &f->ch_layout, AVSampleFormat(f->format),
                                    f->sample_rate, 0, nullptr) < 0 ||
                swr_init(r.swr) < 0)
                return fail("Cannot convert the sound");
        }
        const int most = swr_get_out_samples(r.swr, f->nb_samples);
        AVFrame* out = av_frame_alloc();
        out->nb_samples = most;
        out->format = r.extended ? AV_SAMPLE_FMT_FLTP : r.enc->sample_fmt;
        av_channel_layout_copy(&out->ch_layout, &r.layout);
        out->sample_rate = 48000;
        if (av_frame_get_buffer(out, 0) < 0) {
            av_frame_free(&out);
            return fail("Out of memory");
        }
        const int got = swr_convert(r.swr, out->data, most, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        if (got > 0) av_audio_fifo_write(r.fifo, reinterpret_cast<void**>(out->data), got);
        av_frame_free(&out);
        return encodeSoundFifo(r, false);
    }

    bool finishAll() {
        if (!opened) return true;
        for (auto& v : video) {
            if (!v->enc) continue;
            avcodec_send_frame(v->enc, nullptr);
            if (!takeVideo(*v) || !endSegment(*v, true)) return false;
        }
        for (auto& a : audio) {
            if (!encodeSoundFifo(*a, true)) return false;
            if (a->extended) a->extended->send(nullptr);
            else avcodec_send_frame(a->enc, nullptr);
            if (!takeAudio(*a) || !endSegment(*a, true)) return false;
        }
        double duration = 0;
        for (auto& v : video)
            if (!v->segments.empty()) duration = std::max(duration, v->segments.back().start + v->segments.back().seconds);
        return writeMpd(true, duration);
    }

    void run() {
        for (;;) {
            Item it;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return !queue.empty(); });
                it = queue.front();
                queue.pop_front();
            }
            if (it.end) break;
            bool ok = !failed;
            if (ok && it.video && !opened) ok = open(it);
            if (ok && it.video) ok = encodePicture(it.frame);
            else if (ok && !it.video) ok = encodeSound(it.frame);
            // The multivariant playlist and the MPD once the sound (if any) has had a segment to start in.
            if (ok && opened && !mastered && (!audio.empty() || set.audioCodec == "none" || set.audioCodec.empty() ||
                                              (!video.empty() && video.front()->current >= 2))) {
                mastered = true;
                std::vector<std::string> names;
                for (auto& v : video) names.push_back(v->name);
                for (auto& a : audio) names.push_back(a->name);
                ok = writeMaster() && writeMpd(false, 0);
                std::lock_guard lock(m);
                st.renditions = names;
                if (set.hls) st.hls = folder + "/master.m3u8";
                if (set.dash) st.dash = folder + "/manifest.mpd";
            }
            av_frame_free(&it.frame);
        }
        if (!failed) finishAll();
        std::lock_guard lock(m);
        for (Item& i : queue) av_frame_free(&i.frame);
        queue.clear();
        st.running = false;
        st.finished = true;
        growingFiles.clear();
    }

    void push(const AVFrame* f, bool video, int num, int den) {
        if (!f) return;
        {
            std::lock_guard lock(m);
            if (!st.running || stopping) return;
            if (video) {
                ++st.frames;
                if (!published && num > 0 && den > 0 && f->pts != AV_NOPTS_VALUE) {
                    if (firstPushed == AV_NOPTS_VALUE) firstPushed = f->pts;
                    epoch = std::min(epoch, wallMs() - (f->pts - firstPushed) * 1000 * den / num);
                }
                // (the encoders more than two seconds behind: this picture is left out)
                int64_t waiting = 0;
                for (const Item& i : queue) waiting += i.video;
                st.behind = num > 0 ? double(waiting) * den / num : 0;
                if (num > 0 && waiting > 2 * num / std::max(1, den)) {
                    ++st.dropped;
                    return;
                }
            }
        }
        AVFrame* copy = av_frame_alloc();
        copy->format = f->format;
        copy->width = f->width, copy->height = f->height;
        copy->nb_samples = f->nb_samples;
        copy->sample_rate = f->sample_rate;
        av_channel_layout_copy(&copy->ch_layout, &f->ch_layout);
        if (av_frame_get_buffer(copy, 0) < 0 || av_frame_copy(copy, f) < 0 || av_frame_copy_props(copy, f) < 0) {
            av_frame_free(&copy);
            return;
        }
        copy->pts = f->pts;
        {
            std::lock_guard lock(m);
            queue.push_back({copy, video, num, den, false});
        }
        cv.notify_one();
    }
};

LivePackager::LivePackager() : s_(std::make_unique<State>()) {}

LivePackager::~LivePackager() { finish(); }

bool LivePackager::start(const LiveSettings& set, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (s_->thread.joinable()) return fail("The live package is already being made");
    if (set.folder.empty()) return fail("Choose a folder for the package");
    if (!set.hls && !set.dash) return fail("Choose HLS, DASH or both");
    if (set.segmentFormat != "cmaf" && set.segmentFormat != "ts") return fail("Segments are cmaf or ts");
    if (set.segmentFormat == "ts" && set.lowLatency) return fail("Low latency needs CMAF segments (its parts are CMAF chunks)");
    if (set.window < 3 || set.window > 100) return fail("The window is 3 to 100 segments");
    if (set.segmentSeconds < 1 || set.segmentSeconds > 6) return fail("Segments are 1 to 6 seconds long");
    static const std::set<std::string> sounds = {"aac", "libopus", "ac3", "eac3", "none", ""};
    if (!sounds.count(set.audioCodec)) return fail("The sound is aac, libopus, ac3, eac3 or none");
    QDir root(QString::fromStdString(set.folder));
    if (!root.mkpath(".")) return fail("Cannot make the folder " + set.folder);
    // What an earlier live package left at the top (its renditions' folders are replaced as each opens).
    for (const char* f : {"master.m3u8", "manifest.mpd"}) root.remove(QString::fromLatin1(f));
    State& s = *s_;
    s.set = set;
    s.folder = root.absolutePath().toStdString();
    s.st = LiveStatus{};
    s.st.running = true;
    s.stopping = false;
    s.thread = std::thread([this] { s_->run(); });
    return true;
}

void LivePackager::picture(const AVFrame* f, int fpsNum, int fpsDen) { s_->push(f, true, fpsNum, fpsDen); }

void LivePackager::sound(const AVFrame* f) { s_->push(f, false, 0, 0); }

void LivePackager::finish() {
    State& s = *s_;
    if (!s.thread.joinable()) return;
    {
        std::lock_guard lock(s.m);
        s.stopping = true;
        s.queue.push_back({nullptr, true, 0, 0, true});
    }
    s.cv.notify_one();
    s.thread.join();
}

LiveStatus LivePackager::status() const {
    std::lock_guard lock(s_->m);
    return s_->st;
}

bool LivePackager::growing(const std::string& rel) const {
    std::lock_guard lock(s_->m);
    return s_->growingFiles.count(rel) > 0;
}

// ---- The origin ----

namespace {

class OriginServer : public QTcpServer {
public:
    std::function<void(qintptr)> incoming;

protected:
    void incomingConnection(qintptr fd) override { incoming(fd); }
};

// Whether an HLS media playlist has segment `msn` (with `part` >= 0: that part of it, or the whole segment).
bool playlistHas(const QByteArray& text, int64_t msn, int part) {
    int64_t sequence = 0, complete = 0;
    int partsAfter = 0;
    for (const QByteArray& raw : text.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith("#EXT-X-MEDIA-SEQUENCE:")) sequence = line.mid(22).toLongLong();
        else if (line == "#EXT-X-ENDLIST") return true;
        else if (line.startsWith("#EXT-X-PART:")) ++partsAfter;
        else if (!line.isEmpty() && !line.startsWith('#')) ++complete, partsAfter = 0;
    }
    const int64_t last = sequence + complete - 1;
    if (msn <= last) return true;
    return part >= 0 && msn == last + 1 && partsAfter > part;
}

double targetDuration(const QByteArray& text) {
    const int at = text.indexOf("#EXT-X-TARGETDURATION:");
    return at < 0 ? 6 : std::max(1.0, text.mid(at + 22, 8).trimmed().split('\n').front().toDouble());
}

QByteArray contentType(const QString& rel) {
    if (rel.endsWith(".m3u8")) return "application/vnd.apple.mpegurl";
    if (rel.endsWith(".mpd")) return "application/dash+xml";
    if (rel.endsWith(".m4s") || rel.endsWith(".mp4")) return "video/mp4";
    if (rel.endsWith(".ts")) return "video/mp2t";
    if (rel.endsWith(".vtt")) return "text/vtt";
    if (rel.endsWith(".html")) return "text/html; charset=utf-8";
    if (rel.endsWith(".js")) return "text/javascript";
    return "application/octet-stream";
}

}  // namespace

struct LiveOrigin::State {
    std::string folder, address;
    std::function<bool(const std::string&)> growing;
    double waitSeconds = 10;
    std::thread thread;
    std::mutex m;
    QEventLoop* loop = nullptr;
    int port = 0;

    // A connection's one request (each answered, then closed). Kept until its socket is gone.
    struct Request {
        QTcpSocket* socket = nullptr;
        QByteArray in;
        bool parsed = false, head = false, streaming = false, done = false;
        QString rel;
        int64_t msn = -1;
        int part = -1;
        Clock::time_point came;
        qint64 sent = 0;
        // (a held playlist request: the file as last read, so it is read again only when it has changed, or every
        // tenth of a second at least)
        qint64 seenSize = -1, seenModified = -1;
        Clock::time_point seenAt;
        double holdFor = 0;
    };
    std::vector<std::unique_ptr<Request>> requests;

    static QByteArray headers(int code, const QByteArray& type, qint64 length, bool chunked) {
        const char* reason = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                             : code == 405 ? "Method Not Allowed" : "Service Unavailable";
        QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
        h += "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\n"
             "Access-Control-Expose-Headers: Date\r\nCache-Control: no-cache\r\nConnection: close\r\n";
        h += "Date: " + QLocale::c().toString(QDateTime::currentDateTimeUtc(), "ddd, dd MMM yyyy HH:mm:ss 'GMT'").toLatin1() + "\r\n";
        if (!type.isEmpty()) h += "Content-Type: " + type + "\r\n";
        if (chunked) h += "Transfer-Encoding: chunked\r\n";
        else if (length >= 0) h += "Content-Length: " + QByteArray::number(length) + "\r\n";
        return h + "\r\n";
    }
    void close(Request& r) {
        r.done = true;
        r.socket->disconnectFromHost();
    }
    void answer(Request& r, int code, const QByteArray& type, const QByteArray& body) {
        r.socket->write(headers(code, type, body.size(), false));
        if (!r.head) r.socket->write(body);
        close(r);
    }
    bool after(const Request& r, double seconds) const { return Clock::now() > r.came + std::chrono::milliseconds(int64_t(seconds * 1000)); }

    // Answers what can be answered now, else leaves it waiting (the timer asks again).
    void attempt(Request& r) {
        if (r.done || !r.socket) return;
        if (!r.parsed) {
            const int end = r.in.indexOf("\r\n\r\n");
            if (end < 0) {
                if (r.in.size() > 16384) answer(r, 400, "text/plain", "Too long");
                return;
            }
            const QList<QByteArray> first = r.in.left(r.in.indexOf("\r\n")).split(' ');
            r.parsed = true;
            r.came = Clock::now();
            if (first.size() < 2) return answer(r, 400, "text/plain", "Bad request");
            const QByteArray method = first[0];
            if (method == "OPTIONS") return answer(r, 204, {}, {});
            if (method != "GET" && method != "HEAD") return answer(r, 405, "text/plain", "GET or HEAD");
            r.head = method == "HEAD";
            const QUrl url(QStringLiteral("http://origin") + QString::fromLatin1(first[1]));
            const QString p = QDir::cleanPath(url.path(QUrl::FullyDecoded));
            if (p == "/time") return answer(r, 200, "text/plain", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toLatin1());
            r.rel = p.startsWith('/') ? p.mid(1) : p;
            // (inside the folder only, and nothing hidden: no staging or temporary files)
            if (r.rel.isEmpty() || r.rel.contains("..") || r.rel.contains('\\') || r.rel.contains(':') || r.rel.endsWith(".tmp"))
                return answer(r, 404, "text/plain", "Not found");
            for (const QString& part : r.rel.split('/'))
                if (part.isEmpty() || part.startsWith('.')) return answer(r, 404, "text/plain", "Not found");
            const QUrlQuery q(url);
            if (q.hasQueryItem("_HLS_msn")) {
                r.msn = q.queryItemValue("_HLS_msn").toLongLong();
                r.part = q.hasQueryItem("_HLS_part") ? q.queryItemValue("_HLS_part").toInt() : -1;
            }
        }
        QFile f(QString::fromStdString(folder + "/" + r.rel.toStdString()));
        const bool grows = growing && growing(r.rel.toStdString());
        if (r.streaming) {
            // A segment as it grows: what is new, then (once finished) the end.
            if (grows && QFileInfo(f).size() <= r.sent) return;  // (nothing new yet)
            if (f.open(QIODevice::ReadOnly) && f.size() > r.sent && f.seek(r.sent)) {
                const QByteArray more = f.read(f.size() - r.sent);
                if (!more.isEmpty()) {
                    r.socket->write(QByteArray::number(qlonglong(more.size()), 16) + "\r\n" + more + "\r\n");
                    r.sent += more.size();
                }
            }
            if (!grows && (!f.isOpen() || f.size() <= r.sent)) {
                r.socket->write("0\r\n\r\n");
                close(r);
            }
            return;
        }
        if (!f.exists()) {
            if (after(r, waitSeconds)) answer(r, 404, "text/plain", "Not found");
            return;
        }
        if (r.msn >= 0 && r.rel.endsWith(".m3u8")) {
            const QFileInfo fi(f);
            const qint64 size = fi.size(), modified = fi.lastModified().toMSecsSinceEpoch();
            if (size == r.seenSize && modified == r.seenModified && Clock::now() - r.seenAt < std::chrono::milliseconds(100)) {
                if (after(r, r.holdFor)) answer(r, 503, "text/plain", "Not yet");
                return;
            }
            if (!f.open(QIODevice::ReadOnly)) return;
            const QByteArray text = f.readAll();
            f.close();
            r.seenSize = size, r.seenModified = modified, r.seenAt = Clock::now();
            // (held for three target durations at most)
            r.holdFor = targetDuration(text) * 3;
            if (!playlistHas(text, r.msn, r.part)) {
                if (after(r, r.holdFor)) answer(r, 503, "text/plain", "Not yet");
                return;
            }
        }
        if (grows) {
            r.socket->write(headers(200, contentType(r.rel), -1, true));
            if (r.head) {
                r.socket->write("0\r\n\r\n");
                return close(r);
            }
            r.streaming = true;
            return attempt(r);
        }
        if (!f.open(QIODevice::ReadOnly)) {
            if (after(r, waitSeconds)) answer(r, 404, "text/plain", "Not found");
            return;
        }
        QByteArray body = f.readAll();
        if (r.rel.endsWith(".mpd")) {
            // (the clock as it is when sent)
            const QByteArray scheme = "urn:mpeg:dash:utc:direct:2014\" value=\"";
            const int at = body.indexOf(scheme);
            if (at >= 0) {
                const int from = at + int(scheme.size()), to = body.indexOf('"', from);
                if (to > from) body.replace(from, to - from, QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzzZ").toLatin1());
            }
        }
        answer(r, 200, contentType(r.rel), body);
    }

    void run(std::promise<std::string>& started) {
        QEventLoop l;
        OriginServer server;
        server.setMaxPendingConnections(64);
        server.incoming = [&](qintptr fd) {
            auto* socket = new QTcpSocket(&server);
            if (!socket->setSocketDescriptor(fd) || requests.size() >= 256) {
                socket->abort();
                socket->deleteLater();
                return;
            }
            auto r = std::make_unique<Request>();
            r->socket = socket;
            Request* raw = r.get();
            requests.push_back(std::move(r));
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [this, raw] {
                if (raw->done) return;
                raw->in += raw->socket->readAll();
                attempt(*raw);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            // (the request goes with its socket)
            QObject::connect(socket, &QObject::destroyed, &server, [this, raw] {
                requests.erase(std::remove_if(requests.begin(), requests.end(), [raw](const std::unique_ptr<Request>& q) { return q.get() == raw; }),
                               requests.end());
            });
        };
        const QHostAddress host(QString::fromStdString(address));
        if (!server.listen(host, quint16(port))) {
            started.set_value("Cannot listen on " + address + ":" + std::to_string(port) + ": " + server.errorString().toStdString());
            return;
        }
        port = server.serverPort();
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, &server, [this] {
            // (a copy: answering can close a socket, but it goes only once control is back in the loop)
            std::vector<Request*> waiting;
            for (auto& r : requests)
                if (!r->done && r->parsed) waiting.push_back(r.get());
            for (Request* r : waiting) attempt(*r);
        });
        tick.start(10);
        {
            std::lock_guard lock(m);
            loop = &l;
        }
        started.set_value({});
        l.exec();
        {
            std::lock_guard lock(m);
            loop = nullptr;
        }
        tick.stop();
        QObject::disconnect(&server, nullptr, nullptr, nullptr);
        for (QTcpSocket* socket : server.findChildren<QTcpSocket*>()) {
            QObject::disconnect(socket, nullptr, nullptr, nullptr);
            socket->abort();
        }
        requests.clear();
    }
};

LiveOrigin::LiveOrigin() : s_(std::make_unique<State>()) {}

LiveOrigin::~LiveOrigin() { stop(); }

bool LiveOrigin::start(const std::string& folder, int port, std::function<bool(const std::string&)> growing, std::string* error,
                       const std::string& address, double waitSeconds) {
    if (s_->thread.joinable()) {
        if (error) *error = "The origin is already serving";
        return false;
    }
    s_->folder = QDir(QString::fromStdString(folder)).absolutePath().toStdString();
    s_->growing = std::move(growing);
    s_->address = address.empty() ? "127.0.0.1" : address;
    s_->port = std::max(0, port);
    s_->waitSeconds = std::clamp(waitSeconds, 0.5, 60.0);
    std::promise<std::string> started;
    std::future<std::string> ready = started.get_future();
    s_->thread = std::thread([this, &started] { s_->run(started); });
    const std::string why = ready.get();
    if (!why.empty()) {
        s_->thread.join();
        if (error) *error = why;
        return false;
    }
    return true;
}

int LiveOrigin::port() const { return s_->port; }

void LiveOrigin::stop() {
    if (!s_->thread.joinable()) return;
    for (;;) {
        {
            std::lock_guard lock(s_->m);
            if (s_->loop) {
                QMetaObject::invokeMethod(s_->loop, "quit", Qt::QueuedConnection);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    s_->thread.join();
}

}  // namespace montage
