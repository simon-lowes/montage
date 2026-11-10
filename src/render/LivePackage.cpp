#include "LivePackage.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLocale>
#include <QRegularExpression>
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
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/Captions.h"
#include "core/Cea708.h"
#include "core/Surround.h"
#include "media/Loudness.h"
#include "render/Ac3.h"
#include "render/ImageSubtitles.h"
#include "render/Scte35.h"
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
#ifdef _WIN32
    // (replaced in one step; refused while another program has the old one open without sharing its deletion, as the
    // origin's own reads and a virus scanner's do for a moment, so tried again for up to two seconds)
    const std::wstring from = QString::fromStdString(tmp).toStdWString(), to = QString::fromStdString(path).toStdWString();
    for (int i = 0; i < 400; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(5);
    }
    return false;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
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

// ISO BMFF boxes, written by hand where FFmpeg's muxer has no say: DASH events (emsg) and IMSC 1 in fragmented MP4.
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    put32(b, uint32_t(v >> 32));
    put32(b, uint32_t(v));
}
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void putText(std::vector<uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload, int version = -1, uint32_t flags = 0) {
    std::vector<uint8_t> b;
    put32(b, uint32_t(8 + (version >= 0 ? 4 : 0) + payload.size()));
    b.insert(b.end(), type, type + 4);
    if (version >= 0) put32(b, uint32_t(version) << 24 | (flags & 0xffffff));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> o;
    for (const auto& p : parts) o.insert(o.end(), p.begin(), p.end());
    return o;
}

// A DASH event message box (ISO/IEC 23009-1 5.10.3.3, version 1: its time on the track's timeline), as a CMAF
// chunk carries it before its moof.
std::vector<uint8_t> emsgBox(uint32_t timescale, uint64_t time, uint32_t duration, uint32_t id, const std::string& scheme, const std::string& value,
                             const std::vector<uint8_t>& message) {
    std::vector<uint8_t> p;
    put32(p, timescale);
    put64(p, time);
    put32(p, duration);
    put32(p, id);
    putText(p, scheme);
    putText(p, value);
    p.insert(p.end(), message.begin(), message.end());
    return box("emsg", p, 1);
}

const char* const kScte35Scheme = "urn:scte:scte35:2013:bin";

// IMSC 1 in fragmented MP4 (ISO/IEC 14496-30: an XMLSubtitleSampleEntry, stpp), one TTML document a segment, its
// times on the track's timeline (milliseconds from the presentation's 0), as DASH players take live captions.
std::vector<uint8_t> stppInit(const std::string& language) {
    std::vector<uint8_t> matrix;
    for (uint32_t v : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u}) put32(matrix, v);
    std::vector<uint8_t> ftyp = {'i', 's', 'o', '6'};
    put32(ftyp, 0);
    for (const char* t : {"iso6", "cmfc", "dash"}) ftyp.insert(ftyp.end(), t, t + 4);
    std::vector<uint8_t> mvhd;
    put32(mvhd, 0), put32(mvhd, 0), put32(mvhd, 1000), put32(mvhd, 0), put32(mvhd, 0x10000), put16(mvhd, 0x100), put16(mvhd, 0);
    put32(mvhd, 0), put32(mvhd, 0);
    mvhd.insert(mvhd.end(), matrix.begin(), matrix.end());
    for (int i = 0; i < 6; ++i) put32(mvhd, 0);
    put32(mvhd, 2);
    std::vector<uint8_t> tkhd;
    put32(tkhd, 0), put32(tkhd, 0), put32(tkhd, 1), put32(tkhd, 0), put32(tkhd, 0), put32(tkhd, 0), put32(tkhd, 0);
    put16(tkhd, 0), put16(tkhd, 0), put16(tkhd, 0), put16(tkhd, 0);
    tkhd.insert(tkhd.end(), matrix.begin(), matrix.end());
    put32(tkhd, 0), put32(tkhd, 0);
    // (the language packed as ISO 639-2/T, three letters of five bits)
    std::string lang = "und";
    const std::string primary = language.substr(0, language.find('-'));
    if (const char* three = iso639_2(primary)) lang = three;
    if (lang.size() != 3 || !std::all_of(lang.begin(), lang.end(), [](char c) { return c >= 'a' && c <= 'z'; })) lang = "und";
    std::vector<uint8_t> mdhd;
    put32(mdhd, 0), put32(mdhd, 0), put32(mdhd, 1000), put32(mdhd, 0);
    put16(mdhd, uint16_t(((lang[0] - 0x60) & 31) << 10 | ((lang[1] - 0x60) & 31) << 5 | ((lang[2] - 0x60) & 31)));
    put16(mdhd, 0);
    std::vector<uint8_t> hdlr;
    put32(hdlr, 0);
    hdlr.insert(hdlr.end(), {'s', 'u', 'b', 't'});
    put32(hdlr, 0), put32(hdlr, 0), put32(hdlr, 0);
    putText(hdlr, "Captions");
    std::vector<uint8_t> dref;
    put32(dref, 1);
    const std::vector<uint8_t> url = box("url ", {}, 0, 1);
    dref.insert(dref.end(), url.begin(), url.end());
    std::vector<uint8_t> stpp(6, 0);
    put16(stpp, 1);
    putText(stpp, "http://www.w3.org/ns/ttml");
    putText(stpp, "");
    putText(stpp, "");
    std::vector<uint8_t> stsd;
    put32(stsd, 1);
    const std::vector<uint8_t> entry = box("stpp", stpp);
    stsd.insert(stsd.end(), entry.begin(), entry.end());
    std::vector<uint8_t> none, stsz;
    put32(none, 0);
    put32(stsz, 0), put32(stsz, 0);
    const std::vector<uint8_t> stbl = box("stbl", join({box("stsd", stsd, 0), box("stts", none, 0), box("stsc", none, 0), box("stsz", stsz, 0), box("stco", none, 0)}));
    const std::vector<uint8_t> minf = box("minf", join({box("sthd", {}, 0), box("dinf", box("dref", dref, 0)), stbl}));
    const std::vector<uint8_t> mdia = box("mdia", join({box("mdhd", mdhd, 0), box("hdlr", hdlr, 0), minf}));
    std::vector<uint8_t> trex;
    put32(trex, 1), put32(trex, 1), put32(trex, 0), put32(trex, 0), put32(trex, 0);
    const std::vector<uint8_t> moov = box("moov", join({box("mvhd", mvhd, 0), box("trak", join({box("tkhd", tkhd, 0, 3), mdia})), box("mvex", box("trex", trex, 0))}));
    return join({box("ftyp", ftyp), moov});
}
std::vector<uint8_t> stppSegment(uint32_t sequence, uint64_t startMs, uint32_t durationMs, const std::string& document) {
    std::vector<uint8_t> mfhd, tfhd, tfdt, trun;
    put32(mfhd, sequence);
    put32(tfhd, 1);
    put64(tfdt, startMs);
    put32(trun, 1);
    const size_t trunAt = trun.size();
    put32(trun, 0);  // (the data offset, set below)
    put32(trun, durationMs);
    put32(trun, uint32_t(document.size()));
    auto moofOf = [&] { return box("moof", join({box("mfhd", mfhd, 0), box("traf", join({box("tfhd", tfhd, 0, 0x020000), box("tfdt", tfdt, 1), box("trun", trun, 0, 0x000301)}))})); };
    const uint32_t offset = uint32_t(moofOf().size() + 8);
    for (int i = 0; i < 4; ++i) trun[trunAt + size_t(i)] = uint8_t(offset >> (8 * (3 - i)));
    std::vector<uint8_t> mdat(document.begin(), document.end());
    return join({moofOf(), box("mdat", mdat)});
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
    // The chunk made: appended to segment `k`'s file (`partFile`: written as that part's file too), after `prefix`
    // (event messages, before its moof).
    bool chunk(int64_t k, const std::string& partFile, std::string* why, const std::vector<uint8_t>& prefix = {}) {
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
        size_t size = size_t(total - std::min(head, total));
        std::vector<uint8_t> withPrefix;
        if (!prefix.empty()) {
            withPrefix = prefix;
            withPrefix.insert(withPrefix.end(), c, c + size);
            c = withPrefix.data(), size = withPrefix.size();
        }
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
    bool text = false;              // captions (WebVTT segments for HLS, IMSC 1 in fragmented MP4 for DASH)
    int track = 0;                  // sound: the feed's stream
    std::string language, label;    // sound and captions
    std::vector<uint8_t> emsg;      // event messages for its next chunk (pictures)
    uint32_t textSequence = 0;      // captions: the IMSC fragments' numbers
    bool ended = false;             // its playlist ended (kept so when the others' are written after)
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
    std::unique_ptr<LoudnessMeter> loudness;  // (AC-3 and E-AC-3: the dialogue level, measured as it goes)
    std::vector<double> weights;
    int dialnorm = -24;
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
    int track = 0;
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
    std::vector<float> interleaved;
    std::vector<LiveSoundTrack> tracks;  // (soundTracks: the feed's; empty, one; under m)
    bool tracksStated = false;
    std::vector<LiveSoundTrack> trackList;  // (the packager thread's copy, taken as it opens)

    // Captions: the feed's cc_data entries and when each came (seconds of the presentation), the last half minute
    // and more, decoded again at each segment's end; the caption rendition once listed.
    std::deque<std::pair<double, CcTriplet>> cc;
    bool ccSeen = false;
    std::unique_ptr<Rendition> captions;

    // Cues: asked for (any thread, under m; a cue for the next picture gets its picture as it is pushed), and placed.
    struct Asked {
        LiveCue cue;
        int64_t picture = -1;  // (the recording's picture it is for, once known)
        bool fromFeed = false;
    };
    std::vector<Asked> asked;
    struct Placed {
        int id = 0;
        LiveCue cue;
        int64_t frame = 0, back = -1;  // the splice's picture and the return's (-1 not known), from the presentation's first
        std::vector<uint8_t> out, in;  // the SCTE 35 messages (the return's once known)
        bool fromFeed = false, late = false, keyframe = false, returned = false, signalled = false, inSignalled = false;
        bool returnSeen = false;  // the feed's return message came (its repeats passed over)
        int64_t placedAt = -1;    // the picture it was placed at
    };
    std::vector<Placed> placed;
    int nextCue = 1;
    int64_t encodedIndex = -1;  // the last picture encoded

    // Chapters: asked for (under m) and placed (seconds of the presentation).
    struct ChapterAsked {
        std::string title;
        double at = -1;
        int64_t picture = -1;
    };
    std::vector<ChapterAsked> chaptersAsked;
    std::vector<std::pair<double, std::string>> chapterList;
    double encodedEnd = 0;  // (the pictures encoded so far, seconds)

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

    // A sound track's language: the one asked for, else (the main) the settings', else the feed's, else und.
    std::string trackLanguage(int track) const {
        if (track >= 0 && size_t(track) < set.languages.size() && !set.languages[size_t(track)].empty()) return set.languages[size_t(track)];
        if (track == 0 && !set.audioLanguage.empty() && set.audioLanguage != "und") return set.audioLanguage;
        if (track >= 0 && size_t(track) < trackList.size() && !trackList[size_t(track)].language.empty()) {
            const std::string two = languageFromTag(trackList[size_t(track)].language);
            return two.empty() ? trackList[size_t(track)].language : two;
        }
        return "und";
    }
    // Its name in the players' menus (unique: HLS's NAME is): the main sound's, the feed's title, else its language.
    std::string trackLabel(int track, const std::string& language) const {
        std::string label = track == 0 && !set.audioName.empty() && trackList.size() <= 1 ? set.audioName
                            : size_t(track) < trackList.size() && !trackList[size_t(track)].title.empty() ? trackList[size_t(track)].title
                            : language != "und" ? language
                                                : "Sound " + std::to_string(track + 1);
        const std::string base = label;
        for (int n = 2; std::any_of(audio.begin(), audio.end(), [&](const std::unique_ptr<Rendition>& a) { return a->label == label; }); ++n)
            label = base + " " + std::to_string(n);
        return label;
    }
    int expectedTracks() const { return trackList.empty() ? 1 : int(trackList.size()); }
    Rendition* audioTrack(int track) {
        for (auto& a : audio)
            if (a->track == track) return a.get();
        return nullptr;
    }

    bool openAudio(const AVFrame* f, int track) {
        const std::string codecName = set.audioCodec;
        const AVCodec* codec = avcodec_find_encoder_by_name(codecName == "eac3" ? "eac3" : codecName.c_str());
        if (!codec) return fail("This build has no " + codecName + " encoder");
        if (ts && codecName == "libopus") return fail("MPEG-TS segments carry AAC, AC-3 or E-AC-3 sound, not Opus");
        auto r = std::make_unique<Rendition>();
        r->video = false;
        r->track = track;
        r->language = trackLanguage(track);
        r->label = trackLabel(track, r->language);
        r->name = "a" + std::to_string(track + 1) + "_" + pathName(r->language);
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
            r->loudness = std::make_unique<LoudnessMeter>(48000);
            const std::string name = out == 8 ? "7.1" : out == 6 ? "5.1" : "stereo";
            for (const Speaker& sp : layoutSpeakers(name)) r->weights.push_back(sp.loudnessWeight);
            r->weights.resize(size_t(out), 1.0);
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
        {
            std::lock_guard lock(m);
            trackList = tracks;
        }
        if (!openVideo(first.frame)) return false;
        opened = true;
        return writeChapters();  // (none yet: named in the multivariant playlist from the start)
    }

    // ---- Playlists and the MPD ----

    double seconds(int64_t frame) const { return double(frame) * fps.den / std::max(1, fps.num); }
    int64_t ticks(int64_t frame) const { return frame * 90000 * fps.den / std::max(1, fps.num); }
    std::string dateOf(double t) const { return isoDate(ast + int64_t(std::llround(t * 1000))); }

    // The last part a rendition has made: its segment's number and its index there (an LL-HLS rendition report's
    // LAST-MSN and LAST-PART). False before its first.
    static bool lastPart(const Rendition& r, int64_t& msn, int& part) {
        for (auto it = r.segments.rbegin(); it != r.segments.rend(); ++it)
            if (!it->parts.empty()) {
                msn = it->number, part = int(it->parts.size()) - 1;
                return true;
            }
        return false;
    }

    // The cues' date range tags (each at its time), those still inside the window from `from` (seconds) on.
    std::vector<std::pair<double, std::string>> dateRanges(double from) const {
        std::vector<std::pair<double, std::string>> tags;
        for (const Placed& p : placed) {
            const double at = seconds(p.frame), end = p.back >= 0 ? seconds(p.back) : at;
            if (std::max(at, end) < from) continue;
            const std::string id = "#EXT-X-DATERANGE:ID=\"cue-" + std::to_string(p.id) + "\",START-DATE=\"" + dateOf(at) + "\"";
            if (p.cue.other) {
                tags.push_back({at, id + ",SCTE35-CMD=" + scte35::toHex(p.out)});
            } else if (p.cue.returning) {  // (a return whose out was not seen)
                tags.push_back({at, id + ",SCTE35-IN=" + scte35::toHex(p.out)});
            } else {
                tags.push_back({at, id + (p.cue.duration > 0 ? ",PLANNED-DURATION=" + fmt("%.3f", p.cue.duration) : std::string()) + ",SCTE35-OUT=" +
                                        scte35::toHex(p.out)});
                if (p.returned && p.back >= 0)
                    tags.push_back({end, id + ",DURATION=" + fmt("%.3f", end - at) + (p.in.empty() ? std::string() : ",SCTE35-IN=" + scte35::toHex(p.in))});
            }
        }
        std::stable_sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return tags;
    }

    std::string mediaPlaylist(const Rendition& r, bool ended) {
        const bool tsList = ts && !r.text;
        const bool withParts = parts && !r.text;
        const char* ext = r.text ? "vtt" : tsList ? "ts" : "m4s";
        // The finished segments, and a gap for each number none was made for (all its pictures dropped): HLS numbers
        // the segments by their place in the list, so a missing one is listed as EXT-X-GAP, never left out.
        std::vector<const Segment*> done;
        for (const Segment& s : r.segments)
            if (s.complete) done.push_back(&s);
        std::deque<Segment> gaps;  // (a deque: what is listed points into it)
        const Segment* making = !r.segments.empty() && !r.segments.back().complete ? &r.segments.back() : nullptr;
        {
            std::vector<const Segment*> listed;
            auto fill = [&](int64_t upTo) {  // (the gaps before segment `upTo`)
                if (listed.empty()) return;
                for (int64_t k = std::max(listed.back()->number + 1, upTo - set.window); k < upTo; ++k) {
                    Segment g;
                    g.number = k;
                    g.start = double(k - 1) * segSeconds;
                    g.seconds = segSeconds;
                    gaps.push_back(g);
                    listed.push_back(&gaps.back());
                }
            };
            for (const Segment* s : done) {
                fill(s->number);
                listed.push_back(s);
            }
            if (making && !ended) fill(making->number);  // (and before the one being made, whose parts come next)
            done = std::move(listed);
        }
        const bool gap = !gaps.empty();
        while (int(done.size()) > set.window) done.erase(done.begin());
        double longest = segSeconds;
        for (const Segment* s : done) longest = std::max(longest, s->seconds);
        const long long target = std::max<long long>(1, std::llround(longest));
        std::string m = "#EXTM3U\n#EXT-X-VERSION:" + std::string(parts ? "9" : gap ? "8" : tsList ? "3" : "7") + "\n#EXT-X-TARGETDURATION:" +
                        std::to_string(target) + "\n";
        if (parts) {
            // (blocking reloads, and delta updates skipping what is more than six target durations old: the origin
            // makes both)
            m += "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,CAN-SKIP-UNTIL=" + fmt("%.1f", double(target) * 6) +
                 (withParts ? ",PART-HOLD-BACK=" + fmt("%.3f", partTarget * 3) : std::string()) + "\n";
            if (withParts) m += "#EXT-X-PART-INF:PART-TARGET=" + fmt("%.3f", partTarget) + "\n";
        }
        const int64_t first = !done.empty() ? done.front()->number : making ? making->number : 1;
        m += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(first) + "\n";
        if (!tsList && !r.text) m += "#EXT-X-MAP:URI=\"init.mp4\"\n";
        // (the cues in the window, each before the segment its time falls in, those ahead after the last)
        std::vector<std::pair<double, std::string>> tags;
        if (!r.text) tags = dateRanges(!done.empty() ? done.front()->start : making ? making->start : 0);
        size_t tag = 0;
        auto tagsBefore = [&](double end) {
            for (; tag < tags.size() && tags[tag].first < end; ++tag) m += tags[tag].second + "\n";
        };
        auto listParts = [&](const Segment& s) {
            for (size_t j = 0; j < s.parts.size(); ++j)
                m += "#EXT-X-PART:DURATION=" + fmt("%.5f", s.parts[j].seconds) + ",URI=\"" + partName(s.number, int(j)) + "\"" +
                     (s.parts[j].independent ? ",INDEPENDENT=YES" : "") + "\n";
        };
        for (size_t i = 0; i < done.size(); ++i) {
            const bool missing = !done[i]->complete;
            tagsBefore(done[i]->start + done[i]->seconds);
            // (each segment dated: date ranges are placed by it, and a delta update keeps the dates of what it lists)
            if (!missing) m += "#EXT-X-PROGRAM-DATE-TIME:" + dateOf(done[i]->start) + "\n";
            // (parts for the newest segments only, as the live edge needs them)
            if (withParts && !ended && !missing && i + 2 >= done.size()) listParts(*done[i]);
            m += std::string(missing ? "#EXT-X-GAP\n" : "") + "#EXTINF:" + fmt("%.5f", done[i]->seconds) + ",\n" + segName(done[i]->number, ext) + "\n";
        }
        tagsBefore(1e300);
        if (withParts && !ended) {
            if (making) {
                m += "#EXT-X-PROGRAM-DATE-TIME:" + dateOf(making->start) + "\n";
                listParts(*making);
            }
            const int64_t k = making ? making->number : (done.empty() ? 1 : done.back()->number + 1);
            const int j = making ? int(making->parts.size()) : 0;
            m += "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"" + partName(k, j) + "\"\n";
            // Where the other renditions have got to: a player switching to one asks for its next part at once.
            auto report = [&](const Rendition& o) {
                int64_t msn = 0;
                int part = 0;
                if (&o == &r || !lastPart(o, msn, part)) return;
                m += "#EXT-X-RENDITION-REPORT:URI=\"../" + o.name + "/playlist.m3u8\",LAST-MSN=" + std::to_string(msn) + ",LAST-PART=" + std::to_string(part) + "\n";
            };
            for (const auto& v : video) report(*v);
            for (const auto& a : audio) report(*a);
        }
        if (ended) m += "#EXT-X-ENDLIST\n";
        return m;
    }

    bool writeMaster() {
        if (!set.hls) return true;
        std::string m = "#EXTM3U\n#EXT-X-VERSION:" + std::string(ts ? "3" : parts ? "9" : "7") + "\n#EXT-X-INDEPENDENT-SEGMENTS\n";
        m += "#EXT-X-SESSION-DATA:DATA-ID=\"org.montage.chapters\",URI=\"chapters.vtt\",FORMAT=RAW\n";
        int64_t audioRate = 0;
        for (const auto& a : audio) {
            audioRate = std::max<int64_t>(audioRate, a->extended ? set.audioBitrate * 4 : a->enc ? a->enc->bit_rate : set.audioBitrate);
            m += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=" + montage::quoted(a->label) + ",LANGUAGE=" + montage::quoted(a->language) + ",DEFAULT=" +
                 (a->track == 0 ? "YES" : "NO") + ",AUTOSELECT=YES,CHANNELS=\"" + std::to_string(a->channels) + "\",URI=\"" + a->name + "/playlist.m3u8\"\n";
        }
        if (captions)
            m += "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=" + montage::quoted(captions->label) + ",LANGUAGE=" + montage::quoted(captions->language) +
                 ",DEFAULT=NO,AUTOSELECT=YES,FORCED=NO,URI=\"" + captions->name + "/playlist.m3u8\"\n";
        const std::string audioCodecs = audio.empty() ? "" : "," + audio.front()->codecs;
        for (const auto& v : video)
            m += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(v->rung.bitrate * 3 / 2 + audioRate) + ",AVERAGE-BANDWIDTH=" +
                 std::to_string(v->rung.bitrate + audioRate) + ",CODECS=\"" + v->codecs + audioCodecs + "\",RESOLUTION=" + std::to_string(v->width) + "x" +
                 std::to_string(v->height) + ",FRAME-RATE=" + fmt("%.3f", av_q2d(fps)) + ",VIDEO-RANGE=SDR" + (audio.empty() ? "" : ",AUDIO=\"audio\"") +
                 (captions ? ",SUBTITLES=\"subs\"" : "") + ",CLOSED-CAPTIONS=NONE\n" + v->name + "/playlist.m3u8\n";
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
            // (ad cues: SCTE 35 messages in emsg boxes, before the chunk they come in)
            x += std::string("<InbandEventStream schemeIdUri=\"") + kScte35Scheme + "\" value=\"1\"/>\n";
            for (const auto& v : video)
                x += "<Representation id=\"" + v->name + "\" bandwidth=\"" + std::to_string(v->rung.bitrate * 3 / 2) + "\" codecs=\"" + v->codecs +
                     "\" width=\"" + std::to_string(v->width) + "\" height=\"" + std::to_string(v->height) + "\" sar=\"1:1\">\n" + templ(*v) + "</Representation>\n";
            x += "</AdaptationSet>\n";
        }
        for (const auto& a : audio) {
            x += "<AdaptationSet id=\"" + std::to_string(id++) + "\" contentType=\"audio\" mimeType=\"audio/mp4\" lang=\"" + xmlEscape(a->language) +
                 "\" segmentAlignment=\"true\">\n<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"" + (a->track == 0 ? "main" : "alternate") +
                 "\"/>\n<Label>" + xmlEscape(a->label) + "</Label>\n";
            x += "<Representation id=\"" + a->name + "\" bandwidth=\"" + std::to_string(a->enc ? a->enc->bit_rate : set.audioBitrate * 4) + "\" codecs=\"" + a->codecs +
                 "\" audioSamplingRate=\"48000\">\n" +
                 (set.audioCodec == "ac3" || set.audioCodec == "eac3"
                      ? ac3DashChannelConfiguration(a->channels)
                      : "<AudioChannelConfiguration schemeIdUri=\"urn:mpeg:mpegB:cicp:ChannelConfiguration\" value=\"" +
                            std::to_string(a->channels == 8 ? 12 : a->channels) + "\"/>") +
                 "\n" + templ(*a) + "</Representation>\n</AdaptationSet>\n";
        }
        if (captions) {
            // (IMSC 1 text in fragmented MP4, a TTML document a segment, made at each segment's end)
            x += "<AdaptationSet id=\"" + std::to_string(id++) + "\" contentType=\"text\" mimeType=\"application/mp4\" lang=\"" + xmlEscape(captions->language) +
                 "\" segmentAlignment=\"true\">\n<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"caption\"/>\n<Label>" + xmlEscape(captions->label) +
                 "</Label>\n<Representation id=\"" + captions->name + "\" bandwidth=\"2000\" codecs=\"stpp.ttml.im1t\">\n<SegmentTemplate timescale=\"1000\" duration=\"" +
                 std::to_string(std::llround(segSeconds * 1000)) + "\" startNumber=\"1\" initialization=\"" + captions->name + "/init.mp4\" media=\"" +
                 captions->name + "/seg$Number%05d$.m4s\"/>\n</Representation>\n</AdaptationSet>\n";
        }
        x += "</Period>\n<UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\" value=\"" + isoDate(wallMs()) + "\"/>\n</MPD>\n";
        return writeAtomically(folder + "/manifest.mpd", x) || fail("Cannot write " + folder + "/manifest.mpd");
    }

    // ---- Segments ----

    bool writePlaylist(Rendition& r, bool ended = false) {
        r.ended = r.ended || ended;
        if (!set.hls || (!firstSegment && !r.ended)) return true;
        fixEpoch();
        return writeAtomically(folder + "/" + r.name + "/playlist.m3u8", mediaPlaylist(r, r.ended)) || fail("Cannot write " + r.name + "'s playlist");
    }
    // Every rendition's playlist (with parts, each states where the others have got to).
    bool writePlaylists() {
        for (auto& o : video)
            if (!writePlaylist(*o)) return false;
        for (auto& o : audio)
            if (!writePlaylist(*o)) return false;
        return true;
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
            // (the cues' event messages, before the chunk's moof)
            if (!r.cmaf->chunk(r.current, part, &why, r.emsg)) return fail(why);
            r.emsg.clear();
        } else {
            seconds = r.chunkSeconds;
        }
        r.chunkSeconds = 0;
        r.segmentSoFar += seconds;
        if (parts) {
            s.parts.push_back({seconds, r.video ? r.firstChunkOfSegment : true});
            r.firstChunkOfSegment = false;
            return writePlaylists();
        }
        return true;
    }

    // ---- Captions ----

    bool openCaptions() {
        auto r = std::make_unique<Rendition>();
        r->video = false, r->text = true;
        r->language = set.captionLanguage.empty() ? "und" : set.captionLanguage;
        r->label = !set.captionName.empty() ? set.captionName : "Captions";
        r->name = "s1_" + pathName(r->language);
        std::string why;
        const std::string dir = renditionDir(r->name, &why);
        if (dir.empty()) return fail(why);
        if (set.dash) {
            const std::vector<uint8_t> init = stppInit(r->language);
            if (!writeAtomically(dir + "/init.mp4", init.data(), init.size())) return fail("Cannot write " + dir + "/init.mp4");
        }
        captions = std::move(r);
        std::lock_guard lock(m);
        st.captioned = true;
        return true;
    }

    // The captions of a segment of the pictures (its number, first frame and length): what the feed's caption data
    // shows over it, decoded again with what came since (a caption still up runs to the segment's end and goes on in
    // the next), as a WebVTT segment for HLS and an IMSC 1 fragment for DASH.
    bool writeCaptions(const Segment& vs, bool last) {
        if (!captions) return true;
        Rendition& r = *captions;
        const Rational rate{fps.num, fps.den};
        const int64_t from = (vs.number - 1) * segFrames, to = from + std::max<int64_t>(1, std::llround(vs.seconds * av_q2d(fps)));
        std::vector<Caption> all, part;
        if (!cc.empty()) {
            const std::vector<std::pair<double, CcTriplet>> list(cc.begin(), cc.end());
            if (!captionsFromCc(list, rate, all)) all.clear();
        }
        for (Caption c : all) {
            if (c.end <= from || c.start >= to) continue;
            c.start = std::max<FrameTime>(c.start, from), c.end = std::min<FrameTime>(c.end, to);
            part.push_back(std::move(c));
        }
        const std::string dir = folder + "/" + r.name + "/";
        if (set.hls) {
            // (cue times are the presentation's, as the pictures' are: from 0 in CMAF, from kTsOffset in MPEG-TS)
            std::string vtt = captionsToVtt(part, rate);
            const size_t nl = vtt.find('\n');
            vtt.insert(nl == std::string::npos ? vtt.size() : nl + 1, "X-TIMESTAMP-MAP=MPEGTS:" + std::to_string(ts ? kTsOffset : 0) + ",LOCAL:00:00:00.000\n");
            if (!writeAtomically(dir + segName(vs.number, "vtt"), vtt)) return fail("Cannot write " + dir + segName(vs.number, "vtt"));
        }
        if (set.dash) {
            const int64_t startMs = std::llround(seconds(from) * 1000), endMs = std::llround(seconds(to) * 1000);
            const std::vector<uint8_t> frag = stppSegment(++r.textSequence, uint64_t(startMs), uint32_t(std::max<int64_t>(1, endMs - startMs)),
                                                          captionsToTtml(part, rate, r.language));
            if (!writeAtomically(dir + segName(vs.number, "m4s"), frag.data(), frag.size())) return fail("Cannot write " + dir + segName(vs.number, "m4s"));
        }
        Segment s;
        s.number = vs.number, s.start = vs.start, s.seconds = vs.seconds, s.complete = true;
        r.segments.push_back(s);
        while (int(r.segments.size()) > 2 * set.window + 2) {
            QFile::remove(QString::fromStdString(dir + segName(r.segments.front().number, "vtt")));
            QFile::remove(QString::fromStdString(dir + segName(r.segments.front().number, "m4s")));
            r.segments.pop_front();
        }
        // (what came before the next segment's last half minute is no longer needed)
        const double keep = seconds(to) - 30;
        while (!cc.empty() && cc.front().first < keep) cc.pop_front();
        {
            std::lock_guard lock(m);
            ++st.captionSegments;
            st.captionCues += int64_t(part.size());
        }
        return writePlaylist(r, last);
    }

    // ---- Chapters ----

    bool writeChapters() {
        std::vector<Caption> cues;
        const int64_t edge = std::llround(encodedEnd * av_q2d(fps));
        for (size_t i = 0; i < chapterList.size(); ++i) {
            Caption c;
            c.start = std::llround(chapterList[i].first * av_q2d(fps));
            c.end = i + 1 < chapterList.size() ? std::llround(chapterList[i + 1].first * av_q2d(fps)) : std::max(edge, c.start + 1);
            c.text = chapterList[i].second;
            if (c.end > c.start) cues.push_back(std::move(c));
        }
        return writeAtomically(folder + "/chapters.vtt", captionsToVtt(cues, Rational{fps.num, fps.den})) || fail("Cannot write " + folder + "/chapters.vtt");
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
        const bool front = r.video && &r == video.front().get();
        if (!firstSegment && front) {
            // (the timeline stated from now: every rendition's playlist so far)
            firstSegment = true;
            for (auto& o : video)
                if (o.get() != &r && !writePlaylist(*o)) return false;
            for (auto& o : audio)
                if (!writePlaylist(*o)) return false;
        }
        // The window: the playlists list `window` segments; a segment's files stay for its own length and the
        // playlist's after it leaves (RFC 8216 6.2.2: a player paused, behind a CDN or at the window's start may still
        // fetch it), so `window` + 1 segments more are kept, then the oldest go.
        while (int(r.segments.size()) > 2 * set.window + 2) {
            const Segment& old = r.segments.front();
            const QString dir = QString::fromStdString(folder + "/" + r.name + "/");
            QFile::remove(dir + QString::fromStdString(segName(old.number, "m4s")));
            QFile::remove(dir + QString::fromStdString(segName(old.number, "ts")));
            for (size_t j = 0; j < old.parts.size(); ++j) QFile::remove(dir + QString::fromStdString(partName(old.number, int(j))));
            r.segments.pop_front();
        }
        if (front) {
            if (!writeCaptions(s, last)) return false;
            if (!chapterList.empty() && !writeChapters()) return false;
            std::lock_guard lock(m);
            st.segments = r.current;
            int64_t complete = 0;
            for (const Segment& g : r.segments) complete += g.complete;
            st.firstListed = r.current - std::min<int64_t>(complete, set.window) + 1;
            for (const auto& a : audio)
                if (a->track == 0) st.dialnorm = a->dialnorm;
        }
        if (!last && front) {
            // (the dialogue level as measured so far, from the next segment: each sound track's own)
            for (auto& a : audio) {
                if (!a->loudness) continue;
                const LoudnessResult lr = a->loudness->result();
                if (lr.valid) a->dialnorm = ac3Dialnorm(lr.integrated);
                if (a->extended) a->extended->setDialnorm(a->dialnorm);
                else if (a->enc) ac3SetDialnorm(a->enc, a->dialnorm);
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

    // ---- Cues and chapters ----

    // A cue made here: splice_insert (or time_signal with a placement opportunity's segmentation) at the
    // presentation's 90 kHz time of `frame`, the break's out point or (`in`) its return.
    std::vector<uint8_t> makeCue(int id, int64_t frame, double duration, bool in) const {
        scte35::Cue c;
        c.sapType = 0;  // (a keyframe there, closed)
        c.ptsTime = ticks(frame);
        const int64_t dur = std::llround(duration * 90000);
        if (set.cueCommand == "time_signal") {
            c.command = scte35::Cue::TimeSignal;
            scte35::Segmentation g;
            g.eventId = uint32_t(id);
            g.typeId = in ? 0x35 : 0x34;  // provider placement opportunity start and end
            g.segmentNum = g.segmentsExpected = 1;
            g.duration = !in && duration > 0 ? dur : -1;
            c.segmentations = {g};
        } else {
            c.eventId = uint32_t(id);
            c.outOfNetwork = !in;
            c.breakDuration = !in && duration > 0 ? dur : -1;
            c.uniqueProgramId = 1;
        }
        return scte35::encode(c);
    }

    // A cue's event messages (its out point, and its return once known), in each rung's next chunk: those not sent
    // yet, or (`again`) those still ahead of picture `index` (each segment's start repeats them for players that
    // join between the message and its splice; the same id, so a player raises each once).
    void signal(Placed& p, bool again = false, int64_t index = 0) {
        const bool out = again ? p.signalled && p.frame > index : !p.signalled;
        const bool in = p.back >= 0 && !p.in.empty() && (again ? p.inSignalled && p.back > index : !p.inSignalled);
        if (!out && !in) return;
        for (auto& v : video) {
            if (!v->cmaf) continue;
            const uint32_t scale = uint32_t(v->cmaf->timescale.den / std::max(1, v->cmaf->timescale.num));
            const auto at = [&](int64_t frame) { return uint64_t(std::llround(seconds(frame) * scale)); };
            if (out) {
                const uint32_t dur = p.back >= 0 ? uint32_t(std::llround(seconds(p.back - p.frame) * scale)) : 0xffffffffu;
                const std::vector<uint8_t> e = emsgBox(scale, at(p.frame), dur, uint32_t(p.id * 2), kScte35Scheme, "1", p.out);
                v->emsg.insert(v->emsg.end(), e.begin(), e.end());
            }
            if (in) {
                const std::vector<uint8_t> e = emsgBox(scale, at(p.back), 0, uint32_t(p.id * 2 + 1), kScte35Scheme, "1", p.in);
                v->emsg.insert(v->emsg.end(), e.begin(), e.end());
            }
        }
        p.signalled = true;
        if (p.back >= 0 && !p.in.empty()) p.inSignalled = true;
    }

    void publishCues() {
        std::vector<LiveCueStatus> list;
        for (const Placed& p : placed) {
            LiveCueStatus c;
            c.id = p.id, c.frame = p.frame, c.seconds = seconds(p.frame);
            c.duration = p.back >= 0 ? seconds(p.back - p.frame) : p.cue.duration;
            c.fromFeed = p.fromFeed, c.late = p.late, c.keyframe = p.keyframe, c.returned = p.returned;
            list.push_back(c);
        }
        std::lock_guard lock(m);
        st.cues = std::move(list);
    }

    // The cues and chapters asked for, placed from picture `index` on (one for a picture already encoded on this one).
    void takeAsked(int64_t index) {
        std::vector<Asked> got;
        std::vector<ChapterAsked> chapters;
        {
            std::lock_guard lock(m);
            for (auto it = asked.begin(); it != asked.end();)
                if (it->cue.at >= 0 || it->picture >= 0) got.push_back(*it), it = asked.erase(it);
                else ++it;
            for (auto it = chaptersAsked.begin(); it != chaptersAsked.end();)
                if (it->at >= 0 || it->picture >= 0) chapters.push_back(*it), it = chaptersAsked.erase(it);
                else ++it;
        }
        const double rate = av_q2d(fps);
        auto frameOf = [&](double at, int64_t picture, bool* late) {
            int64_t f = at >= 0 ? std::llround(at * rate) - firstPicture : picture - firstPicture;
            *late = f < index;
            return std::max(f, index);
        };
        bool changed = false;
        std::vector<Asked> later;
        for (Asked& a : got) {
            bool late = false;
            const int64_t frame = frameOf(a.cue.at, a.picture, &late);
            if (frame > index + int64_t(3600 * rate)) {
                // (the feed's an hour ahead: not a time it means; one placed here waits for its time to come near)
                if (!a.fromFeed) later.push_back(a);
                continue;
            }
            if (a.fromFeed && a.cue.cancel) {
                // A cancel: the feed's break of that event not yet begun taken off (its message, if sent, stays sent).
                const size_t was = placed.size();
                placed.erase(std::remove_if(placed.begin(), placed.end(),
                                            [&](const Placed& p) { return p.fromFeed && p.cue.eventId == a.cue.eventId && p.frame > index && !p.keyframe; }),
                             placed.end());
                changed = changed || placed.size() != was;
                continue;
            }
            // (a message the feed repeats ahead of its splice: the cue placed already)
            if (a.fromFeed && !a.cue.other &&
                std::any_of(placed.begin(), placed.end(), [&](const Placed& p) {
                    return p.fromFeed && !p.cue.other && !p.cue.returning && p.cue.eventId == a.cue.eventId &&
                           (a.cue.returning ? p.returnSeen : !p.returned);
                }))
                continue;
            if (a.cue.returning) {
                // The return of a break: the feed's by its event id, one placed here the latest still out.
                Placed* out = nullptr;
                for (auto it = placed.rbegin(); it != placed.rend() && !out; ++it)
                    if (!it->cue.other && !it->cue.returning && !it->returned && it->fromFeed == a.fromFeed &&
                        (a.fromFeed ? it->cue.eventId == a.cue.eventId : it->back < 0))
                        out = &*it;
                if (out) {
                    out->back = std::max(frame, out->frame + 1);
                    out->in = a.fromFeed && !a.cue.scte35.empty() ? a.cue.scte35 : makeCue(out->id, out->back, 0, true);
                    out->inSignalled = false;
                    out->returnSeen = a.fromFeed;
                    signal(*out);
                    changed = true;
                    continue;
                }
            }
            Placed p;
            p.id = nextCue++;
            p.cue = a.cue;
            p.fromFeed = a.fromFeed;
            p.frame = frame;
            p.placedAt = index;
            p.late = late;
            if (!a.cue.other && !a.cue.returning && a.cue.duration > 0 && a.cue.autoReturn)
                p.back = frame + std::max<int64_t>(1, std::llround(a.cue.duration * rate));
            p.out = a.fromFeed && !a.cue.scte35.empty() ? a.cue.scte35 : makeCue(p.id, frame, a.cue.duration, a.cue.returning);
            // (a break of the feed's that returns by itself: its end listed with its length, no message of its own)
            if (p.back >= 0 && !a.fromFeed) p.in = makeCue(p.id, p.back, 0, true);
            placed.push_back(std::move(p));
            signal(placed.back());
            changed = true;
        }
        if (!later.empty()) {
            std::lock_guard lock(m);
            asked.insert(asked.end(), later.begin(), later.end());
        }
        // (what has left the window and its files is forgotten)
        const double gone = seconds(index) - segSeconds * (2 * set.window + 4);
        const size_t before = placed.size();
        placed.erase(std::remove_if(placed.begin(), placed.end(), [&](const Placed& p) { return seconds(std::max(p.frame, p.back)) < gone; }), placed.end());
        if (changed || placed.size() != before) publishCues();
        if (!chapters.empty()) {
            for (const ChapterAsked& c : chapters) {
                bool late = false;
                chapterList.push_back({seconds(frameOf(c.at, c.picture, &late)), c.title});
            }
            std::stable_sort(chapterList.begin(), chapterList.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (chapterList.size() > 1000) chapterList.erase(chapterList.begin(), chapterList.begin() + ptrdiff_t(chapterList.size() - 1000));
            writeChapters();
            std::lock_guard lock(m);
            st.chapters = int(chapterList.size());
        }
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
            if (&r == video.front().get()) {
                bool marked = false;
                for (Placed& p : placed)
                    if (p.frame == i && !p.keyframe && (r.pkt->flags & AV_PKT_FLAG_KEY)) p.keyframe = marked = true;
                if (marked) publishCues();
            }
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
        takeAsked(index);
        // (its caption data, kept to be decoded at the segment's end)
        if (set.captions != "none")
            if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_A53_CC)) {
                for (size_t i = 0; i + 3 <= sd->size; i += 3) {
                    const CcTriplet t{sd->data[i], sd->data[i + 1], sd->data[i + 2]};
                    cc.push_back({seconds(index), t});
                    if ((t[0] & 0x04) && ((t[1] & 0x7f) || (t[2] & 0x7f))) ccSeen = true;
                }
                while (cc.size() > 200000) cc.pop_front();
            }
        // (the first picture of a segment, or the first that came of it when the segment's first was dropped, and
        // a cue's splice or return: a keyframe)
        const int64_t segment = index / segFrames, before = encodedSegment;
        // (a cue on a picture that was dropped goes on the first after it)
        bool moved = false;
        for (Placed& p : placed) {
            if (p.frame > encodedIndex && p.frame < index) p.frame = index, moved = true;
            if (p.back > encodedIndex && p.back < index) p.back = std::max(index, p.frame + 1), moved = true;
        }
        if (moved) publishCues();
        encodedIndex = index;
        // (each segment's start: the event messages of cues still ahead, again)
        if (segment != before)
            for (Placed& p : placed)
                if (p.placedAt != index) signal(p, true, index);
        const bool cueHere = std::any_of(placed.begin(), placed.end(), [&](const Placed& p) { return p.frame == index || p.back == index; });
        const bool start = segment != before || cueHere;
        encodedSegment = segment;
        encodedEnd = seconds(index + 1);
        bool returned = false;
        for (Placed& p : placed)
            if (p.back >= 0 && p.back <= index && !p.returned) p.returned = returned = true;
        if (returned) publishCues();
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
            if (r.loudness && r.frame->format == AV_SAMPLE_FMT_FLTP) {
                interleaved.resize(size_t(n) * size_t(r.channels));
                for (int c = 0; c < r.channels; ++c) {
                    const float* p = reinterpret_cast<const float*>(r.frame->data[c]);
                    for (int i = 0; i < n; ++i) interleaved[size_t(i) * size_t(r.channels) + size_t(c)] = p[i];
                }
                r.loudness->addChannels(interleaved.data(), n, r.channels, r.weights.data());
            }
            const int rc = r.extended ? r.extended->send(r.frame) : avcodec_send_frame(r.enc, r.frame);
            if (rc < 0) return fail("Encoding the sound failed: " + averr(rc));
            if (!takeAudio(r)) return false;
        }
        return true;
    }

    bool encodeSound(const AVFrame* f, int track) {
        Rendition* found = audioTrack(track);
        if (!found) {
            if (set.audioCodec.empty() || set.audioCodec == "none" || !opened) return true;
            // (sound starting a segment or more after the pictures is left out: the renditions were announced; and a
            // track the feed did not name)
            if (mastered || track < 0 || track >= expectedTracks() || audio.size() >= 16) return true;
            if (!openAudio(f, track)) return false;
            found = audio.back().get();
            found->nextPts = (f->pts == AV_NOPTS_VALUE ? 0 : f->pts) * 48000 / std::max(1, f->sample_rate) -
                             firstPicture * 48000 * fps.den / std::max(1, fps.num);
            if (found->nextPts < 0) found->nextPts = 0;
        }
        Rendition& r = *found;
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

    // The renditions stated: the multivariant playlist and the MPD written, the status naming them.
    bool announce() {
        std::vector<std::string> names;
        for (auto& v : video) names.push_back(v->name);
        for (auto& a : audio) names.push_back(a->name);
        if (captions) names.push_back(captions->name);
        const bool ok = writeMaster() && writeMpd(false, 0);
        std::lock_guard lock(m);
        st.renditions = names;
        st.soundRenditions = int(audio.size());
        if (set.hls) st.hls = folder + "/master.m3u8";
        if (set.dash) st.dash = folder + "/manifest.mpd";
        return ok;
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
        if (!chapterList.empty() && !writeChapters()) return false;
        double duration = 0;
        for (auto& v : video)
            if (!v->segments.empty()) duration = std::max(duration, v->segments.back().start + v->segments.back().seconds);
        return writeMpd(true, duration);
    }

    // After an error: the playlists end and the MPD turns static at what was finished, and the segment being made is
    // no longer streamed as it grows (players stop waiting for more, not only when the recording stops).
    bool closedAfterError = false;
    void closeAfterError() {
        closedAfterError = true;
        {
            std::lock_guard lock(m);
            growingFiles.clear();
        }
        if (!opened) return;
        double duration = 0;
        for (auto& v : video) {
            for (const Segment& s : v->segments)
                if (s.complete) duration = std::max(duration, s.start + s.seconds);
            writePlaylist(*v, true);
        }
        for (auto& a : audio) writePlaylist(*a, true);
        if (captions) writePlaylist(*captions, true);
        if (mastered) writeMpd(true, duration);
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
            else if (ok && !it.video) ok = encodeSound(it.frame, it.track);
            // The multivariant playlist and the MPD once each sound track has had a segment to start in; again when
            // captions first turn up in the feed (at the end of the segment they came in).
            const bool noSound = set.audioCodec == "none" || set.audioCodec.empty();
            if (ok && opened && !mastered && (int(audio.size()) >= expectedTracks() || noSound || (!video.empty() && video.front()->current >= 2))) {
                mastered = true;
                std::stable_sort(audio.begin(), audio.end(), [](const auto& a, const auto& b) { return a->track < b->track; });
                if (set.captions == "always" || (set.captions == "auto" && ccSeen)) ok = openCaptions();
                ok = ok && announce();
            } else if (ok && mastered && !captions && set.captions == "auto" && ccSeen && !video.empty() && video.front()->segments.size() >= 2 &&
                       video.front()->segments.front().complete) {
                ok = openCaptions();
                // (the segments made so far: their captions now)
                for (const Segment& g : video.front()->segments)
                    if (ok && g.complete) ok = writeCaptions(g, false);
                ok = ok && announce();
            }
            av_frame_free(&it.frame);
            if (failed && !closedAfterError) closeAfterError();
        }
        if (!failed) finishAll();
        std::lock_guard lock(m);
        for (Item& i : queue) av_frame_free(&i.frame);
        queue.clear();
        st.running = false;
        st.finished = true;
        growingFiles.clear();
    }

    void push(const AVFrame* f, bool video, int num, int den, int track = 0) {
        if (!f) return;
        {
            std::lock_guard lock(m);
            if (!st.running || stopping) return;
            if (video) {
                ++st.frames;
                // (cues and chapters for the next picture recorded: this one)
                if (f->pts != AV_NOPTS_VALUE) {
                    for (Asked& a : asked)
                        if (a.cue.at < 0 && a.picture < 0) a.picture = f->pts;
                    for (ChapterAsked& c : chaptersAsked)
                        if (c.at < 0 && c.picture < 0) c.picture = f->pts;
                }
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
            queue.push_back({copy, video, track, num, den, false});
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
    if (set.captions != "auto" && set.captions != "always" && set.captions != "none") return fail("Captions are auto, always or none");
    if (set.cueCommand != "splice_insert" && set.cueCommand != "time_signal") return fail("Cues are splice_insert or time_signal");
    QDir root(QString::fromStdString(set.folder));
    if (!root.mkpath(".")) return fail("Cannot make the folder " + set.folder);
    // What an earlier live package left at the top (its renditions' folders are replaced as each opens).
    for (const char* f : {"master.m3u8", "manifest.mpd", "chapters.vtt"}) root.remove(QString::fromLatin1(f));
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

void LivePackager::soundTracks(const std::vector<LiveSoundTrack>& tracks) {
    std::lock_guard lock(s_->m);
    // (named before the sound: the packager's thread reads them from its first sound on)
    if (s_->tracksStated) return;
    s_->tracks = tracks;
    if (s_->tracks.size() > 16) s_->tracks.resize(16);
    s_->tracksStated = true;
}

void LivePackager::sound(const AVFrame* f, int track) { s_->push(f, false, 0, 0, track); }

void LivePackager::cue(const LiveCue& cue) {
    std::lock_guard lock(s_->m);
    if (!s_->st.running || s_->stopping || s_->asked.size() >= 256) return;
    s_->asked.push_back({cue, -1, false});
}

void LivePackager::cueFromFeed(const std::vector<uint8_t>& section, double feedZero, double at) {
    scte35::Cue c;
    if (!scte35::decode(section, c)) return;
    LiveCue lc;
    lc.scte35 = section;
    // (the feed's 90 kHz clock, 33 bits, from the recording's first picture)
    auto when = [&](int64_t pts) {
        if (pts < 0) return std::isnan(at) ? -1.0 : std::max(0.0, at);
        const int64_t wrap = int64_t(1) << 33;
        int64_t d = ((pts + c.ptsAdjustment) % wrap - int64_t(std::llround(feedZero * 90000)) % wrap) % wrap;
        if (d < 0) d += wrap;
        if (d >= wrap / 2) d -= wrap;
        return std::max(0.0, double(d) / 90000);
    };
    if (c.command == scte35::Cue::Insert) {
        lc.eventId = c.eventId;
        lc.cancel = c.eventCancel;
        lc.at = when(c.immediate || c.eventCancel ? -1 : c.ptsTime);
        lc.returning = !c.outOfNetwork;
        lc.autoReturn = c.autoReturn;
        lc.duration = !lc.returning && c.breakDuration > 0 ? double(c.breakDuration) / 90000 : 0;
    } else if (c.command == scte35::Cue::TimeSignal) {
        lc.at = when(c.ptsTime);
        lc.other = true;
        for (const scte35::Segmentation& g : c.segmentations)
            if (g.cancel) {
                lc.other = false, lc.cancel = true, lc.eventId = g.eventId, lc.at = when(-1);
                break;
            } else if (g.typeId >= 0x30 && g.typeId <= 0x3f) {
                // (an advertisement's or placement opportunity's start, even; its end, odd)
                lc.other = false;
                lc.eventId = g.eventId;
                lc.returning = (g.typeId & 1) != 0;
                lc.duration = !lc.returning && g.duration > 0 ? double(g.duration) / 90000 : 0;
                break;
            }
    } else {
        return;  // (splice_null: a heartbeat)
    }
    std::lock_guard lock(s_->m);
    if (!s_->st.running || s_->stopping || s_->asked.size() >= 256) return;
    s_->asked.push_back({lc, -1, true});
}

void LivePackager::chapter(const std::string& title, double at) {
    std::lock_guard lock(s_->m);
    if (!s_->st.running || s_->stopping || s_->chaptersAsked.size() >= 256) return;
    s_->chaptersAsked.push_back({title, at, -1});
}

void LivePackager::finish() {
    State& s = *s_;
    if (!s.thread.joinable()) return;
    {
        std::lock_guard lock(s.m);
        s.stopping = true;
        s.queue.push_back({nullptr, true, 0, 0, 0, true});
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

std::string playlistDeltaUpdate(const std::string& playlist, int* skipped) {
    if (skipped) *skipped = 0;
    const QString text = QString::fromStdString(playlist);
    static const QRegularExpression canSkip("CAN-SKIP-UNTIL=([0-9.]+)"), partDuration("DURATION=([0-9.]+)");
    const QRegularExpressionMatch until = canSkip.match(text);
    if (!until.hasMatch() || text.contains("#EXT-X-ENDLIST")) return playlist;
    const double boundary = until.captured(1).toDouble();
    QStringList lines = text.split('\n');
    // The header (up to the first segment's tags), then each segment's lines up to its URI, then the rest (the
    // parts being made, the preload hint, the reports).
    auto segmentTag = [](const QString& l) {
        return l.startsWith("#EXTINF:") || l.startsWith("#EXT-X-PROGRAM-DATE-TIME:") || l.startsWith("#EXT-X-GAP") || l.startsWith("#EXT-X-PART:") ||
               l.startsWith("#EXT-X-DATERANGE:") || l.startsWith("#EXT-X-DISCONTINUITY") || l.startsWith("#EXT-X-BITRATE:");
    };
    int at = 0;
    while (at < lines.size() && !segmentTag(lines[at])) ++at;
    struct Seg {
        int from = 0, to = 0;  // its lines [from, to]
        double end = 0;        // its end, from the first's start
    };
    std::vector<Seg> segs;
    double t = 0, trailing = 0;
    int from = at;
    for (int i = at; i < lines.size(); ++i) {
        const QString& l = lines[i];
        if (l.startsWith("#EXTINF:")) t += l.mid(8).section(',', 0, 0).toDouble();
        if (!l.isEmpty() && !l.startsWith('#')) {
            segs.push_back({from, i, t});
            from = i + 1;
            trailing = 0;
        } else if (l.startsWith("#EXT-X-PART:")) {
            const QRegularExpressionMatch d = partDuration.match(l);
            if (d.hasMatch()) trailing += d.captured(1).toDouble();
        }
    }
    // (the parts after the last whole segment are part of the playlist's length: they are what was made last)
    const double end = t + trailing;
    size_t n = 0;
    while (n < segs.size() && end - segs[n].end >= boundary) ++n;
    if (n == 0) return playlist;
    QStringList out = lines.mid(0, at);
    out << QStringLiteral("#EXT-X-SKIP:SKIPPED-SEGMENTS=%1").arg(qulonglong(n));
    // (the skipped segments' date ranges stay: they belong to the playlist, not to a segment)
    for (int i = segs.front().from; i <= segs[n - 1].to; ++i)
        if (lines[i].startsWith("#EXT-X-DATERANGE:")) out << lines[i];
    out << lines.mid(segs[n - 1].to + 1);
    if (skipped) *skipped = int(n);
    return out.join('\n').toStdString();
}

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
        bool parsed = false, head = false, streaming = false, done = false, skip = false;
        QString rel;
        int64_t msn = -1;
        int part = -1;
        Clock::time_point came;
        Clock::time_point accepted = Clock::now();  // (a request not whole 10 s after this is let go)
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

    // A blocking reload (_HLS_msn, _HLS_part): whether the playlist has what was asked for (else held, answered 503
    // after three target durations).
    bool playlistCanAnswer(Request& r, QFile& f) {
        const QFileInfo fi(f);
        const qint64 size = fi.size(), modified = fi.lastModified().toMSecsSinceEpoch();
        if (size == r.seenSize && modified == r.seenModified && Clock::now() - r.seenAt < std::chrono::milliseconds(100)) {
            if (after(r, r.holdFor)) answer(r, 503, "text/plain", "Not yet");
            return false;
        }
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QByteArray text = f.readAll();
        f.close();
        r.seenSize = size, r.seenModified = modified, r.seenAt = Clock::now();
        // (held for three target durations at most)
        r.holdFor = targetDuration(text) * 3;
        if (!playlistHas(text, r.msn, r.part)) {
            if (after(r, r.holdFor)) answer(r, 503, "text/plain", "Not yet");
            return false;
        }
        return true;
    }

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
            r.skip = q.queryItemValue("_HLS_skip") == "YES" || q.queryItemValue("_HLS_skip") == "v2";
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
            // (waited for only when it is a name the packager is about to write, in a folder it writes)
            const bool coming = (r.rel.endsWith(".m4s") || r.rel.endsWith(".ts") || r.rel.endsWith(".m3u8") || r.rel.endsWith(".mp4") ||
                                 r.rel.endsWith(".mpd")) &&
                                QFileInfo(f).dir().exists();
            if (!coming || after(r, waitSeconds)) answer(r, 404, "text/plain", "Not found");
            return;
        }
        if (r.skip && r.rel.endsWith(".m3u8")) {
            // (a delta update: the playlist as it is when it can be answered, its old segments skipped)
            if (r.msn >= 0 && !playlistCanAnswer(r, f)) return;
            if (!f.open(QIODevice::ReadOnly)) {
                if (after(r, waitSeconds)) answer(r, 404, "text/plain", "Not found");
                return;
            }
            const QByteArray text = f.readAll();
            return answer(r, 200, contentType(r.rel), QByteArray::fromStdString(playlistDeltaUpdate(text.toStdString())));
        }
        if (r.msn >= 0 && r.rel.endsWith(".m3u8") && !playlistCanAnswer(r, f)) return;
        if (grows) {
            r.socket->write(headers(200, contentType(r.rel), -1, true));
            if (r.head) return close(r);  // (the headers alone)
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
            // (full: the oldest still sending its request makes room, so connections that never finish one cannot
            // keep players out)
            auto open = std::count_if(requests.begin(), requests.end(), [](const std::unique_ptr<Request>& q) { return !q->done; });
            if (open >= 256) {
                Request* oldest = nullptr;
                for (auto& q : requests)
                    if (!q->parsed && !q->done && (!oldest || q->accepted < oldest->accepted)) oldest = q.get();
                if (oldest) {
                    oldest->done = true;
                    oldest->socket->abort();  // (its request goes with the socket, once control is back in the loop)
                    oldest->socket->deleteLater();
                    --open;
                }
            }
            if (!socket->setSocketDescriptor(fd) || open >= 256 || requests.size() >= 512) {
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
            std::vector<Request*> waiting, slow;
            const auto now = Clock::now();
            for (auto& r : requests)
                if (!r->done && r->parsed) waiting.push_back(r.get());
                else if (!r->done && now - r->accepted > std::chrono::seconds(10)) slow.push_back(r.get());
            for (Request* r : waiting) attempt(*r);
            for (Request* r : slow) {
                r->done = true;
                r->socket->abort();
                r->socket->deleteLater();
            }
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
