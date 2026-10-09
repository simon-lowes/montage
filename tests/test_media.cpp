// Media tests: probing, frame-accurate decoding, audio mixing, export round trips.
#include <QtTest>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <array>
#include <QImage>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <random>
#include <complex>
#include <sstream>
#include <cstdio>

#include "core/Aaf.h"
#include "core/Bleep.h"
#include "core/AutoTag.h"
#include "core/Cfb.h"
#include "core/MaskPath.h"
#include "core/Automation.h"
#include "core/EditOps.h"
#include "core/MediaLog.h"
#include "core/Multicam.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "core/Surround.h"
#include "core/Transcript.h"
#include "core/TranscriptEdit.h"
#include "audio/SpeechCleanup.h"
#include "audio/TimeStretch.h"
#include "core/ClipAnimation.h"
#include "core/MergeClips.h"
#include "media/DualSystem.h"
#include "media/FieldRecorder.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/AutoDuck.h"
#include "media/Decoder.h"
#include "media/HwAccel.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#include "media/Relink.h"
#include "media/SpeakerSwitch.h"
#include "media/Tracking.h"
#include "media/Vector.h"
#include "media/Beats.h"
#include "render/AafExport.h"
#include "render/Retime.h"
#include "render/FaceRefine.h"
#include "render/AudioFx.h"
#include "audio/AudioRepair.h"
#include "render/MusicEdit.h"
#include "render/Highlights.h"
#include "render/AutoBroll.h"
#include "render/AutoMix.h"
#include "render/VideoDenoise.h"
#include "render/VoiceMatch.h"
#include "media/Segmenter.h"
#include "media/Translator.h"
#include "media/SpeechEnhance.h"
#include "media/SuperScale.h"
#include "media/Reframe.h"
#include "media/Diarizer.h"
#include "media/Faces.h"
#include "media/DepthMap.h"
#include "media/Rife.h"
#include "media/Matting.h"
#include "media/TextToSpeech.h"
#include "media/Inpaint.h"
#include "media/VisualSearch.h"
#include "automation/McpServer.h"
#ifdef MONTAGE_WITH_WHISPER
#include "media/Transcriber.h"
#endif
#include "render/ClipAnalysis.h"
#include "render/ColorSpace.h"
#include "render/AudioFx.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/Processing.h"
#include "media/CameraRaw.h"
#include "core/Slate.h"
#include "render/PaperEdit.h"
#include "render/QualityCheck.h"
#include "render/ProjectManager.h"
#include "render/LutExport.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

using namespace montage;

// Unbuffered output, so that if the process dies the log shows how far it got.
static const int kUnbufferedStdout = [] { return std::setvbuf(stdout, nullptr, _IONBF, 0); }();

namespace {

// Writes a 16-bit stereo WAV with constant left / right levels.
void writeWav(const std::string& path, int rate, double seconds, float left, float right) {
    FILE* f = std::fopen(path.c_str(), "wb");
    QVERIFY(f);
    int frames = int(rate * seconds);
    uint32_t dataBytes = uint32_t(frames) * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    int16_t l = int16_t(std::lround(left * 32767)), r = int16_t(std::lround(right * 32767));
    for (int i = 0; i < frames; ++i) {
        std::fwrite(&l, 2, 1, f);
        std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
}

// A field recorder's 16-bit stereo WAV: a bext chunk (time reference and a
// description with sSCENE lines) before the sound, an iXML chunk after it.
void writeBwf(const std::string& path, const AudioBuffer& sound, uint64_t timeReference, const std::string& description,
              const std::string& ixml) {
    std::string bext(602, '\0');
    bext.replace(0, std::min<size_t>(description.size(), 256), description.substr(0, 256));
    for (int i = 0; i < 8; ++i) bext[338 + size_t(i)] = char((timeReference >> (8 * i)) & 0xff);
    std::string x = ixml;
    if (x.size() % 2) x += ' ';
    const int rate = sound.sampleRate;
    std::string data;
    for (float v : sound.samples) {
        const int16_t q = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        data.append(reinterpret_cast<const char*>(&q), 2);
    }
    auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [](uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); };
    const std::string fmt = u16(1) + u16(2) + u32(uint32_t(rate)) + u32(uint32_t(rate) * 4) + u16(4) + u16(16);
    const std::string chunks = "fmt " + u32(16) + fmt + "bext" + u32(uint32_t(bext.size())) + bext + "data" + u32(uint32_t(data.size())) +
                               data + "iXML" + u32(uint32_t(x.size())) + x;
    FILE* f = std::fopen(path.c_str(), "wb");
    QVERIFY(f);
    const std::string head = "RIFF" + u32(uint32_t(4 + chunks.size())) + "WAVE";
    std::fwrite(head.data(), 1, head.size(), f);
    std::fwrite(chunks.data(), 1, chunks.size(), f);
    std::fclose(f);
}

// A mono "microphone" as a 16-bit stereo WAV: a tone at `amplitude` while
// talking(t) says so, `bleed` of another voice otherwise.
void writeVoiceWav(const std::string& path, double seconds, double hz, const std::function<bool(double)>& talking,
                   const std::function<bool(double)>& other) {
    const int rate = 48000;
    FILE* f = std::fopen(path.c_str(), "wb");
    QVERIFY(f);
    const int frames = int(rate * seconds);
    uint32_t dataBytes = uint32_t(frames) * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / rate;
        double v = 0.002 * std::sin(i * 0.37);  // room tone
        if (talking(t)) v += 0.3 * std::sin(2 * M_PI * hz * t);
        if (other(t)) v += 0.02 * std::sin(2 * M_PI * hz * 1.5 * t);  // the other voice, faintly
        const int16_t s = int16_t(std::lround(std::clamp(v, -1.0, 1.0) * 32767));
        std::fwrite(&s, 2, 1, f);
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
}

// A minimal uncompressed DNG: an RGGB Bayer mosaic of `scene` (linear RGB, 0..1) from a
// "camera" whose sensor sees linear sRGB, so it develops back to the scene.
bool writeTestDng(const std::string& path, int w, int h, const std::function<std::array<double, 3>(int, int)>& scene, int orientation = 1) {
    struct Entry {
        uint16_t tag, type;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries;
    auto bytes = [](const void* p, size_t n) { return std::vector<uint8_t>(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    auto shorts = [&](uint16_t tag, std::vector<uint16_t> v) { entries.push_back({tag, 3, uint32_t(v.size()), bytes(v.data(), v.size() * 2)}); };
    auto longs = [&](uint16_t tag, std::vector<uint32_t> v) { entries.push_back({tag, 4, uint32_t(v.size()), bytes(v.data(), v.size() * 4)}); };
    auto byteList = [&](uint16_t tag, std::vector<uint8_t> v) { entries.push_back({tag, 1, uint32_t(v.size()), v}); };
    auto ascii = [&](uint16_t tag, const std::string& t) { entries.push_back({tag, 2, uint32_t(t.size() + 1), bytes(t.c_str(), t.size() + 1)}); };
    auto rationals = [&](uint16_t tag, uint16_t type, const std::vector<double>& v) {
        std::vector<int32_t> r;
        for (double x : v) r.push_back(int32_t(std::lround(x * 10000))), r.push_back(10000);
        entries.push_back({tag, type, uint32_t(v.size()), bytes(r.data(), r.size() * 4)});
    };
    const uint32_t dataBytes = uint32_t(w) * uint32_t(h) * 2;
    longs(254, {0});
    longs(256, {uint32_t(w)});
    longs(257, {uint32_t(h)});
    shorts(258, {16});
    shorts(259, {1});
    shorts(262, {32803});
    ascii(271, "Montage");
    ascii(272, "Test Camera");
    longs(273, {8});  // the pixels follow the header
    shorts(274, {uint16_t(orientation)});
    shorts(277, {1});
    longs(278, {uint32_t(h)});
    longs(279, {dataBytes});
    shorts(284, {1});
    shorts(33421, {2, 2});
    byteList(33422, {0, 1, 1, 2});
    byteList(50706, {1, 4, 0, 0});
    byteList(50707, {1, 1, 0, 0});
    ascii(50708, "Montage Test Camera");
    longs(50714, {0});
    longs(50717, {65535});
    // XYZ to camera: the camera is linear sRGB.
    rationals(50721, 10, {3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570});
    rationals(50728, 5, {1, 1, 1});
    shorts(50778, {21});
    std::vector<uint8_t> file = {'I', 'I', 42, 0};
    const uint32_t ifd = 8 + dataBytes;
    file.insert(file.end(), reinterpret_cast<const uint8_t*>(&ifd), reinterpret_cast<const uint8_t*>(&ifd) + 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto c = scene(x, y);
            const int ch = (y % 2 == 0) ? (x % 2 == 0 ? 0 : 1) : (x % 2 == 0 ? 1 : 2);
            const uint16_t v = uint16_t(std::clamp(c[size_t(ch)] * 0.8, 0.0, 1.0) * 65535 + 0.5);
            file.push_back(uint8_t(v & 0xff)), file.push_back(uint8_t(v >> 8));
        }
    // The IFD, then the values too long to sit in it.
    const uint32_t extra0 = ifd + 2 + uint32_t(entries.size()) * 12 + 4;
    std::vector<uint8_t> extra;
    const uint16_t n = uint16_t(entries.size());
    file.push_back(uint8_t(n & 0xff)), file.push_back(uint8_t(n >> 8));
    for (const Entry& e : entries) {
        uint8_t rec[12] = {};
        std::memcpy(rec, &e.tag, 2), std::memcpy(rec + 2, &e.type, 2), std::memcpy(rec + 4, &e.count, 4);
        if (e.data.size() <= 4) std::memcpy(rec + 8, e.data.data(), e.data.size());
        else {
            const uint32_t at = extra0 + uint32_t(extra.size());
            std::memcpy(rec + 8, &at, 4);
            extra.insert(extra.end(), e.data.begin(), e.data.end());
            if (extra.size() % 2) extra.push_back(0);
        }
        file.insert(file.end(), rec, rec + 12);
    }
    for (int i = 0; i < 4; ++i) file.push_back(0);
    file.insert(file.end(), extra.begin(), extra.end());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    return ok;
}

MediaItem probeOrFail(Project& p, const std::string& path) {
    MediaItem m;
    m.id = p.newId();
    std::string err;
    bool ok = probeMedia(path, m, &err);
    if (!ok) qWarning("probe failed: %s", err.c_str());
    return m;
}

// Every subtitle event in a file, as (start seconds, end seconds, ASS text), read
// back with FFmpeg's demuxer and decoder for the file's first subtitle stream.
struct SubEvent {
    double start, end;
    QString text;
};
std::vector<SubEvent> readSubtitles(const std::string& path, const char* format, std::string* codecName,
                                    std::string* language = nullptr) {
    std::vector<SubEvent> out;
    AVFormatContext* fmt = nullptr;
    const AVInputFormat* in = format ? av_find_input_format(format) : nullptr;
    if (avformat_open_input(&fmt, path.c_str(), in, nullptr) < 0) return out;
    avformat_find_stream_info(fmt, nullptr);
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
    if (idx < 0) {
        avformat_close_input(&fmt);
        return out;
    }
    AVStream* st = fmt->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    *codecName = codec ? codec->name : "";
    if (language)
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) *language = e->value;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return out;
    }
    AVPacket* pkt = av_packet_alloc();
    auto decode = [&](AVPacket* p) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(ctx, &sub, &got, p) >= 0 && got) {
            const double base = sub.pts != AV_NOPTS_VALUE ? double(sub.pts) / AV_TIME_BASE
                                                          : double(p->pts) * av_q2d(st->time_base);
            QStringList texts;
            for (unsigned i = 0; i < sub.num_rects; ++i)
                if (sub.rects[i]->ass) texts << QString::fromUtf8(sub.rects[i]->ass).section(',', 8);
            if (!texts.isEmpty())
                out.push_back({base + sub.start_display_time / 1000.0, base + sub.end_display_time / 1000.0, texts.join(' ')});
            avsubtitle_free(&sub);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx) decode(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return out;
}

float meanAbs(const std::vector<float>& v, int ch, size_t from, size_t to) {
    double acc = 0;
    for (size_t i = from; i < to; ++i) acc += std::fabs(v[i * 2 + size_t(ch)]);
    return float(acc / double(to - from));
}

// A song at 128 BPM starting `lead` seconds in: a kick on each bar's first beat, a
// tick on every beat, and a chord per bar (0 C, 1 Am, 2 F, 3 G), then a second of silence.
constexpr double kSongBeat = 60.0 / 128, kSongBar = 4 * kSongBeat;
std::vector<float> testSong(int rate, double lead, const std::vector<int>& barChord) {
    static const std::vector<std::vector<double>> chordHz = {
        {261.6, 329.6, 392.0}, {220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}, {196.0, 246.9, 293.7}};
    const int bars = int(barChord.size());
    std::vector<float> x(size_t((lead + bars * kSongBar + 1.0) * rate), 0.0f);
    unsigned seed = 7;
    for (int b = 0; b < bars; ++b) {
        for (int k = 0; k < 4; ++k) {
            const size_t i0 = size_t((lead + b * kSongBar + k * kSongBeat) * rate);
            for (size_t i = 0; i < size_t(0.03 * rate); ++i) {
                seed = seed * 1664525u + 1013904223u;
                x[i0 + i] += float((double(seed >> 8) / (1 << 24) - 0.5) * 0.3 * std::exp(-double(i) / (0.008 * rate)));
            }
            if (k == 0)
                for (size_t i = 0; i < size_t(0.2 * rate); ++i)
                    x[i0 + i] += float(0.6 * std::sin(2 * M_PI * 55 * double(i) / rate) * std::exp(-double(i) / (0.06 * rate)));
        }
        for (size_t i = 0; i < size_t(kSongBar * rate); ++i) {
            const double t = double(i) / rate;
            double v = 0;
            for (double hz : chordHz[size_t(barChord[size_t(b)])]) v += std::sin(2 * M_PI * hz * (lead + b * kSongBar + t));
            x[size_t((lead + b * kSongBar) * rate) + i] += float(0.06 * v * std::min(1.0, t / 0.02));
        }
    }
    return x;
}

// 16-bit mono WAV.
bool writeMonoWav(const std::string& path, const std::vector<float>& x, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t bytes = uint32_t(x.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(1), u32(uint32_t(rate)), u32(uint32_t(rate) * 2), u16(2), u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (float v : x) {
        const int16_t s = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        std::fwrite(&s, 2, 1, f);
    }
    return std::fclose(f) == 0;
}

// Amplitude of `hz` in channel `ch` of interleaved stereo, over samples [from, to).
double toneLevel(const std::vector<float>& b, int ch, double hz, size_t from, size_t to = 0, int rate = 48000) {
    if (!to) to = b.size() / 2;
    std::complex<double> acc = 0;
    for (size_t i = from; i < to; ++i) acc += double(b[i * 2 + size_t(ch)]) * std::polar(1.0, -2 * M_PI * hz * double(i) / rate);
    return 2 * std::abs(acc) / double(to - from);
}

// The fundamental of a steady sound (mono, from channel 0), by autocorrelation.
double pitchOf(const std::vector<float>& b, size_t from, size_t to, int rate = 48000) {
    const int lo = rate / 1000, hi = rate / 60;
    std::vector<double> r(size_t(hi) + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag)
        for (size_t i = from; i + size_t(lag) < to; ++i) r[size_t(lag)] += double(b[i * 2]) * b[(i + size_t(lag)) * 2];
    int best = lo;
    for (int lag = lo; lag <= hi; ++lag)
        if (r[size_t(lag)] > r[size_t(best)]) best = lag;
    const double a = r[size_t(best - 1)], c = r[size_t(best)], d = r[size_t(best + 1)];
    return rate / (best + 0.5 * (a - d) / (a - 2 * c + d));
}

}  // namespace

class TestMedia : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    std::string path(const char* name) { return (dir_.path() + "/" + name).toStdString(); }

private slots:
    void probeAndDecodeWav() {
        std::string wav = path("tone.wav");
        writeWav(wav, 48000, 1.0, 0.5f, -0.25f);
        Project p;
        MediaItem m = probeOrFail(p, wav);
        QCOMPARE(int(m.kind), int(MediaKind::Audio));
        QVERIFY(m.hasAudio && !m.hasVideo);
        QVERIFY(std::fabs(m.duration - 1.0) < 0.01);
        auto buf = decodeAudio(wav, 48000);
        QVERIFY(buf);
        QVERIFY(std::abs(buf->frames() - 48000) < 10);
        QVERIFY(std::fabs(buf->samples[2000] - 0.5f) < 0.001f);
        QVERIFY(std::fabs(buf->samples[2001] + 0.25f) < 0.001f);
        // Resampling keeps the level.
        auto b44 = decodeAudio(wav, 44100);
        QVERIFY(std::abs(b44->frames() - 44100) < 50);
        QVERIFY(std::fabs(b44->samples[20000] - 0.5f) < 0.01f);
        auto pk = computePeaks(*buf, 480);
        QCOMPARE(pk->minmax.size(), size_t(200));
    }

    void probeReadsWhenAndWhatRecorded() {
        // A camera file's tags: when it was recorded, and the camera's make and model.
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty()) QSKIP("Needs the ffmpeg program");
        const std::string file = path("camera.mov");
        QProcess run;
        run.start(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=red:s=64x36:d=1", "-metadata", "creation_time=2024-05-06T07:08:09Z",
                           "-metadata", "make=Canon", "-metadata", "model=Canon EOS R5", "-c:v", "mpeg4", QString::fromStdString(file)});
        QVERIFY(run.waitForFinished(60000));
        QVERIFY2(run.exitCode() == 0, run.readAllStandardError().constData());
        Project p;
        const MediaItem m = probeOrFail(p, file);
        QVERIFY2(QString::fromStdString(m.created).startsWith("2024-05-06T07:08:09"), m.created.c_str());
        QCOMPARE(m.metadata.at("device"), std::string("Canon EOS R5"));  // the model already names the make
        // Files without tags have neither.
        const std::string wav = path("untagged.wav");
        writeWav(wav, 48000, 0.1, 0.1f, 0.1f);
        const MediaItem plain = probeOrFail(p, wav);
        QVERIFY(plain.created.empty() && plain.metadata.empty());
    }

    void loudnessMeasurement() {
        // A 997 Hz stereo sine at -23 dBFS reads -23 LUFS (EBU Tech 3341 case 1 style).
        AudioBuffer buf;
        buf.sampleRate = 48000;
        double amp = std::pow(10.0, -23.0 / 20.0);
        for (int i = 0; i < 48000 * 5; ++i) {
            float v = float(amp * std::sin(2 * M_PI * 997.0 * i / 48000.0));
            buf.samples.push_back(v);
            buf.samples.push_back(v);
        }
        LoudnessResult r = measureLoudness(buf);
        QVERIFY(r.valid);
        QVERIFY2(std::fabs(r.integrated + 23.0) < 0.2, qPrintable(QString::number(r.integrated)));
        QVERIFY(std::fabs(r.truePeakDb + 23.0) < 0.1);
        // Same at 44.1 kHz (coefficients are rate-dependent).
        AudioBuffer b44;
        b44.sampleRate = 44100;
        for (int i = 0; i < 44100 * 5; ++i) {
            float v = float(amp * std::sin(2 * M_PI * 997.0 * i / 44100.0));
            b44.samples.push_back(v);
            b44.samples.push_back(v);
        }
        QVERIFY(std::fabs(measureLoudness(b44).integrated + 23.0) < 0.2);
        // Silence is gated out; a sub-range measures only that range.
        AudioBuffer quiet;
        quiet.sampleRate = 48000;
        quiet.samples.assign(48000 * 2 * 2, 0.0f);
        QVERIFY(!measureLoudness(quiet).valid);
        LoudnessResult part = measureLoudness(buf, 48000, 48000 * 2);
        QVERIFY(std::fabs(part.integrated + 23.0) < 0.3);
        // Fed in pieces, the meter gives the same answer.
        LoudnessMeter pieces(48000);
        for (int64_t at = 0; at < buf.frames(); at += 1000) pieces.add(buf.samples.data() + at * 2, std::min<int64_t>(1000, buf.frames() - at));
        QCOMPARE(pieces.result().integrated, r.integrated);
        QCOMPARE(pieces.result().truePeakDb, r.truePeakDb);
        // True peak finds what falls between samples: a 12 kHz sine sampled 45° off its peaks reads 3 dB low as samples.
        AudioBuffer between;
        between.sampleRate = 48000;
        for (int i = 0; i < 48000; ++i) {
            const float v = float(0.5 * std::sin(M_PI / 2 * i + M_PI / 4));
            between.samples.push_back(v);
            between.samples.push_back(v);
        }
        const double tp = measureLoudness(between).truePeakDb;
        QVERIFY2(std::fabs(tp - 20 * std::log10(0.5)) < 0.3, qPrintable(QString::number(tp)));
    }

    void builtInAudioEffects() {
        constexpr int sr = 48000;
        auto tone = [](double hz, double amp, int frames = 48000) {
            std::vector<float> b(size_t(frames) * 2);
            for (int i = 0; i < frames; ++i) b[size_t(i) * 2] = b[size_t(i) * 2 + 1] = float(amp * std::sin(2 * M_PI * hz * i / sr));
            return b;
        };
        // RMS in dB over the second half (past any settling).
        auto rmsDb = [](const std::vector<float>& b) {
            double sum = 0;
            const size_t from = b.size() / 2;
            for (size_t i = from; i < b.size(); ++i) sum += double(b[i]) * b[i];
            return 10 * std::log10(sum / double(b.size() - from) + 1e-20);
        };
        const double ref1k = rmsDb(tone(1000, 0.1)), ref100 = rmsDb(tone(100, 0.1));

        // Parametric EQ: a +12 dB bell at 1 kHz lifts 1 kHz, not 100 Hz; the low shelf cuts 50 Hz.
        fx::ParametricEq eq;
        eq.set(sr, {100, 0, 1}, {250, 0, 1}, {1000, 12, 1}, {4000, 0, 1}, {10000, 0, 1}, 0);
        auto b = tone(1000, 0.1);
        eq.process(b.data(), sr);
        QVERIFY2(std::fabs(rmsDb(b) - ref1k - 12) < 0.3, qPrintable(QString::number(rmsDb(b) - ref1k)));
        fx::ParametricEq eq2;
        eq2.set(sr, {100, 0, 1}, {250, 0, 1}, {1000, 12, 1}, {4000, 0, 1}, {10000, 0, 1}, 0);
        b = tone(100, 0.1);
        eq2.process(b.data(), sr);
        QVERIFY(std::fabs(rmsDb(b) - ref100) < 0.6);
        fx::ParametricEq shelf;
        shelf.set(sr, {200, -12, 1}, {250, 0, 1}, {1000, 0, 1}, {4000, 0, 1}, {10000, 0, 1}, -3);
        b = tone(30, 0.1);
        shelf.process(b.data(), sr);
        QVERIFY2(std::fabs(rmsDb(b) - rmsDb(tone(30, 0.1)) + 15) < 0.7, qPrintable(QString::number(rmsDb(b) - rmsDb(tone(30, 0.1)))));

        // De-esser: a loud 7 kHz "s" comes down by up to the reduction; a 1 kHz voice does not.
        fx::DeEsser ds;
        b = tone(7000, 0.3);
        ds.process(b.data(), sr, sr, 5000, -30, 10);
        const double cut = rmsDb(tone(7000, 0.3)) - rmsDb(b);
        QVERIFY2(cut > 6 && cut < 11, qPrintable(QString::number(cut)));
        fx::DeEsser ds2;
        b = tone(1000, 0.3);
        ds2.process(b.data(), sr, sr, 5000, -30, 10);
        QVERIFY(std::fabs(rmsDb(b) - rmsDb(tone(1000, 0.3))) < 0.5);

        // Noise gate: speech-level sound passes, hiss below the threshold drops by the range.
        fx::NoiseGate gate;
        b = tone(1000, 0.1);
        gate.process(b.data(), sr, sr, -45, -40, 1, 50, 150);
        QVERIFY(std::fabs(rmsDb(b) - ref1k) < 0.2);
        fx::NoiseGate gate2;
        b = tone(1000, 0.002);  // -54 dBFS
        gate2.process(b.data(), sr, sr, -45, -40, 1, 50, 150);
        QVERIFY2(std::fabs(rmsDb(b) - rmsDb(tone(1000, 0.002)) + 40) < 1, qPrintable(QString::number(rmsDb(b) - rmsDb(tone(1000, 0.002)))));

        // Reverb: a click rings on and dies away; with no mix the sound is untouched.
        fx::Reverb rv;
        std::vector<float> click(size_t(sr) * 2 * 2, 0.f);
        click[0] = click[1] = 1.f;
        rv.process(click.data(), 2 * sr, sr, 0.7, 0.3, 1, 1);
        auto energy = [&](int from, int to) {
            double e = 0;
            for (int i = from; i < to; ++i) e += double(click[size_t(i) * 2]) * click[size_t(i) * 2];
            return e;
        };
        QVERIFY(energy(sr / 10, sr / 5) > 1e-4);           // still ringing 100-200 ms later
        QVERIFY(energy(sr, sr + sr / 10) < energy(sr / 10, sr / 5));  // and fading
        fx::Reverb dry;
        b = tone(1000, 0.1);
        const auto before = b;
        dry.process(b.data(), sr, sr, 0.7, 0.3, 1, 0);
        QCOMPARE(b, before);

        // Channel tools: a microphone on the left only fills both sides; swap; mono; polarity.
        std::vector<float> lr{0.5f, 0.f, -0.25f, 0.f};
        auto ch = lr;
        fx::channelTools(ch.data(), 2, 2, false, false);
        QCOMPARE(ch, (std::vector<float>{0.5f, 0.5f, -0.25f, -0.25f}));
        ch = lr;
        fx::channelTools(ch.data(), 2, 4, false, false);
        QCOMPARE(ch, (std::vector<float>{0.f, 0.5f, 0.f, -0.25f}));
        ch = lr;
        fx::channelTools(ch.data(), 2, 1, false, true);
        QCOMPARE(ch, (std::vector<float>{0.25f, -0.25f, -0.125f, 0.125f}));

        // In the mixer, as clip effects: Left to Both on a left-only file.
        const std::string wav = path("left.wav");
        writeWav(wav, 48000, 1.0, 0.5f, 0.f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& c = s.audioTracks[0].clips.front();
        Effect tools = makeEffect(p, "channels");
        tools.params["mode"] = 2.0;
        c.effects.push_back(tools);
        c.effects.push_back(makeEffect(p, "parametric_eq"));
        c.effects.push_back(makeEffect(p, "gate"));
        c.effects.push_back(makeEffect(p, "deesser"));
        AudioMixer mixer;
        std::vector<float> out(4800 * 2);
        mixer.mix(p, s, 12000, 4800, out.data());
        QVERIFY2(std::fabs(out[4000 * 2] - out[4000 * 2 + 1]) < 1e-5f && out[4000 * 2] > 0.4f,
                 qPrintable(QString("%1 %2").arg(out[4000 * 2]).arg(out[4000 * 2 + 1])));
    }

    void folderFaderInTheMix() {
        // A steady signal on A1, which is in a folder: the folder's fader lowers it, as a VCA.
        const std::string wav = path("vca.wav");
        writeWav(wav, 48000, 1.0, 0.5f, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto level = [&]() {
            AudioMixer mixer;
            std::vector<float> out(64 * 2);
            mixer.mix(p, s, 12000, 64, out.data());
            return double(out[0]);
        };
        const double base = level();
        QVERIFY(base > 0.1);
        QVERIFY(edit::setTrackFolder(s, {{TrackKind::Audio, 0}}, "Dialogue").ok);
        edit::setFolderGain(s, TrackKind::Audio, "Dialogue", -6.0);
        QVERIFY2(std::fabs(20 * std::log10(level() / base) + 6) < 0.05, qPrintable(QString::number(level() / base)));
        // With the track's own fader: the two add up.
        s.audioTracks[0].volumeDb = -4;
        QVERIFY(std::fabs(20 * std::log10(level() / base) + 10) < 0.05);
        // Renamed, it keeps its level; out of the folder, the track is back to its own fader.
        QVERIFY(edit::renameFolder(s, TrackKind::Audio, "Dialogue", "Dial").ok);
        QCOMPARE(edit::folderGain(s, TrackKind::Audio, "Dial"), -6.0);
        QVERIFY(edit::setTrackFolder(s, {{TrackKind::Audio, 0}}, "").ok);
        QVERIFY(s.folderGains.empty());
        QVERIFY(std::fabs(20 * std::log10(level() / base) + 4) < 0.05);
    }

    void trackAutomationInTheMix() {
        // A steady signal on A1 under a volume lane rising from -60 dB to 0 over a second, and a pan lane.
        const std::string wav = path("steady.wav");
        writeWav(wav, 48000, 2.0, 0.5f, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = Rational{30, 1};
        s.sampleRate = 48000;
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto levelAt = [&](double seconds, int channel) {
            AudioMixer mixer;
            std::vector<float> out(64 * 2);
            mixer.mix(p, s, int64_t(seconds * 48000), 64, out.data());
            return double(out[size_t(channel)]);
        };
        const double base = levelAt(0.5, 0);
        QVERIFY(base > 0.1);
        Track& a1 = s.audioTracks[0];
        a1.volumeAuto.addKey(0, -60);
        a1.volumeAuto.addKey(30, 0);
        auto db = [&](double v) { return 20 * std::log10(std::max(1e-9, v / base)); };
        QVERIFY2(std::fabs(db(levelAt(0.5, 0)) + 30) < 0.3, qPrintable(QString::number(db(levelAt(0.5, 0)))));
        QVERIFY(std::fabs(db(levelAt(0.25, 0)) + 45) < 0.3);
        QVERIFY(std::fabs(db(levelAt(1.5, 0))) < 0.05);
        // Smooth between frames: no steps of a frame's size within a block.
        {
            AudioMixer mixer;
            std::vector<float> out(4800 * 2);
            mixer.mix(p, s, 12000, 4800, out.data());
            double worst = 0;
            for (size_t i = 1; i < 4800; ++i) worst = std::max(worst, double(std::fabs(out[i * 2] - out[(i - 1) * 2])));
            QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));
        }
        // Off (and Write) play the fader instead.
        a1.automation = int(AutomationMode::Off);
        QVERIFY(std::fabs(levelAt(0.5, 0) - base) < 1e-4);
        a1.automation = int(AutomationMode::Read);
        // Pan hard left from frame 30.
        a1.panAuto.addKey(0, 0);
        a1.panAuto.addKey(30, -1);
        QVERIFY(levelAt(1.5, 1) < 1e-4 && levelAt(1.5, 0) > base);
        QVERIFY(std::fabs(levelAt(0.0, 0) - levelAt(0.0, 1)) < 1e-6);
        // Exports hear it too.
        ExportSettings st;
        st.path = path("automated.wav");
        st.videoCodec = "none";
        st.audioCodec = "pcm_s16le";
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        AudioBufferPtr back = decodeAudio(st.path, 48000, &err);
        QVERIFY2(back, err.c_str());
        QVERIFY(std::fabs(back->samples[size_t(12000) * 2]) < 0.05 * base);  // 0.25 s: -45 dB
        QVERIFY(std::fabs(back->samples[size_t(72000) * 2 + 1]) < 1e-3);     // 1.5 s: hard left
    }

    void lutExportOverMcp() {
        // A matte graded to black and white, with a blur a LUT cannot hold.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        Clip c = makeGeneratorClip(p, "color", 30);
        Effect bw = makeEffect(p, "color_correct");
        bw.params["saturation"] = 0.0;
        c.effects.push_back(bw);
        c.effects.push_back(makeEffect(p, "gaussian_blur"));
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        const Id id = s.videoTracks[0].clips.front().id;
        const QString project = QString::fromStdString(path("lut.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_export_lut"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        const QString cube = QString::fromStdString(path("bw.cube"));
        const QJsonObject r = call({{"project", project}, {"clip", double(id)}, {"path", cube}, {"size", 17}});
        const QString text = r.value("content").toArray().at(0).toObject().value("text").toString();
        QVERIFY2(!r.value("isError").toBool() && text.contains("17-point") && text.contains("left out"), qPrintable(text));
        std::string err;
        const auto lut = loadCubeLut(cube.toStdString(), &err);
        QVERIFY2(lut && lut->size == 17, err.c_str());
        // Pure red comes out grey.
        float rr = 1, gg = 0, bb = 0;
        lut->apply(rr, gg, bb);
        QVERIFY2(std::fabs(rr - gg) < 1e-3f && std::fabs(gg - bb) < 1e-3f && rr > 0.05f, qPrintable(QString("%1 %2 %3").arg(rr).arg(gg).arg(bb)));
        QVERIFY(call({{"project", project}, {"clip", 999999.0}, {"path", cube}}).value("isError").toBool());
    }

    void qualityCheckSound() {
        // JFK's speech, then three seconds of nothing.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const double fps = s.fpsValue();
        const FrameTime speechEnd = s.duration(), end = speechEnd + FrameTime(std::lround(3 * fps));
        QcSettings q;
        q.flashing = false, q.levels = false, q.blackSeconds = 0, q.freezeSeconds = 0;
        auto describe = [](const std::vector<QcIssue>& v) {
            QString t;
            for (const QcIssue& i : v) t += QStringLiteral("%1 %2-%3 %4; ").arg(qcKindName(i.kind)).arg(i.start).arg(i.end).arg(QString::fromStdString(i.text));
            return t;
        };
        std::vector<QcIssue> issues = qualityCheck(p, s, 0, end, q);
        // Only the silence after the speech (its pauses are shorter than two seconds), and nothing clips.
        QVERIFY2(issues.size() == 1 && issues[0].kind == QcKind::Silence, qPrintable(describe(issues)));
        QVERIFY2(std::fabs(double(issues[0].start - speechEnd)) < 0.6 * fps && issues[0].end == end, qPrintable(describe(issues)));
        // Against a streaming target: too quiet.
        q.loudnessTarget = -14;
        issues = qualityCheck(p, s, 0, speechEnd, q);
        QVERIFY2(issues.size() == 1 && issues[0].kind == QcKind::Loudness, qPrintable(describe(issues)));
        QVERIFY(QString::fromStdString(issues[0].text).contains("LUFS"));
        // 24 dB louder: it clips and its peaks go over the ceiling.
        s.audioTracks[0].clips.front().audio.params["gain_db"] = 24.0;
        issues = qualityCheck(p, s, 0, speechEnd, q);
        auto has = [&](QcKind k) { return std::any_of(issues.begin(), issues.end(), [&](const QcIssue& i) { return i.kind == k; }); };
        QVERIFY2(has(QcKind::Clipping) && has(QcKind::TruePeak), qPrintable(describe(issues)));
        QVERIFY(!has(QcKind::Silence));
        q.clipping = false, q.loudnessTarget = 0;
        QVERIFY2(qualityCheck(p, s, 0, speechEnd, q).empty(), qPrintable(describe(qualityCheck(p, s, 0, speechEnd, q))));
    }

    void dereverbSpeech() {
        constexpr int sr = 16000;
        std::string err;
        AudioBufferPtr dry = decodeAudio(MONTAGE_TEST_DATA_DIR "/jfk.wav", sr, &err);
        QVERIFY2(dry, err.c_str());
        // A room: the direct sound, then velvet-noise reflections from 5 ms decaying to -60 dB at rt60, with the
        // reverberant energy `drrDb` below the direct sound's.
        auto room = [&](double rt60, double drrDb, bool lateOnly = false, bool earlyOnly = false) {
            std::mt19937 rng(7);
            std::uniform_real_distribution<double> u(0, 1);
            std::vector<std::pair<int, float>> taps;
            const int len = int(rt60 * sr * 1.2), density = 2000;
            double energy = 0;
            for (int m = 0;; ++m) {
                const int at = int(sr * 0.005) + int((m + u(rng)) * sr / density);
                if (at >= len) break;
                const float g = float((u(rng) < 0.5 ? -1 : 1) * std::pow(10.0, -3.0 * at / (rt60 * sr)));
                taps.push_back({at, g});
                energy += double(g) * g;
            }
            const float scale = float(std::sqrt(std::pow(10.0, -drrDb / 10) / energy));
            AudioBuffer wet;
            wet.sampleRate = sr;
            wet.samples = dry->samples;
            const int64_t n = dry->frames();
            for (const auto& [at, g] : taps) {
                if ((earlyOnly && at >= sr / 20) || (lateOnly && at < sr / 20)) continue;
                for (int64_t i = 0; i + at < n; ++i)
                    for (int c = 0; c < 2; ++c) wet.samples[size_t(i + at) * 2 + size_t(c)] += dry->samples[size_t(i) * 2 + size_t(c)] * g * scale;
            }
            return wet;
        };
        // How far the speech with its early reflections (the first 50 ms, which De-Reverb keeps) stands above
        // everything else in `b` (dB), at the best scale.
        AudioBuffer target;
        auto srr = [&](const AudioBuffer& b) {
            double xy = 0, xx = 0, yy = 0;
            for (size_t i = 0; i < b.samples.size(); ++i) {
                xy += double(b.samples[i]) * target.samples[i];
                xx += double(target.samples[i]) * target.samples[i];
                yy += double(b.samples[i]) * b.samples[i];
            }
            const double k = xy / xx;
            return 10 * std::log10(k * k * xx / std::max(1e-12, yy - 2 * k * xy + k * k * xx));
        };
        target = room(0.8, 0, false, true);
        const AudioBuffer wet = room(0.8, 0);
        const double before = srr(wet);
        AudioBuffer fixedKnown, fixedAuto, gentle;
        dereverb(wet, fixedKnown, 100, 0.8, 30);
        dereverb(wet, fixedAuto, 100, 0, 30);
        dereverb(wet, gentle, 40, 0.8, 30);
        const double known = srr(fixedKnown), autoT = srr(fixedAuto), mild = srr(gentle);
        const double est06 = estimateReverbTime(room(0.6, 0)), est08 = estimateReverbTime(wet), est15 = estimateReverbTime(room(1.5, 0));
        const double estDry = estimateReverbTime(*dry);
        AudioBuffer dryOut;
        dereverb(*dry, dryOut, 100, 0, 30);
        target = *dry;
        const double dryKept = srr(dryOut);
        target = room(1.5, -3, false, true);
        const AudioBuffer big = room(1.5, -3);
        AudioBuffer bigOut;
        dereverb(big, bigOut, 100, 0, 30);
        const QString got = QString::asprintf("wet %.2f, known %.2f, auto %.2f, gentle %.2f; dry %.2f; big room %.2f -> %.2f; RT60 %.2f %.2f %.2f, dry %.2f",
                                              before, known, autoT, mild, dryKept, srr(big), srr(bigOut), est06, est08, est15, estDry);
        // The room's reverberation time is found from the speech within 15 %, and a dry recording reads as dry.
        QVERIFY2(est06 > 0.51 && est06 < 0.69 && est08 > 0.68 && est08 < 0.92 && est15 > 1.27 && est15 < 1.73 && estDry < 0.35, qPrintable(got));
        // The late reverberation comes down (2.6 dB more speech over what is left in a 0.8 s room, 3.2 dB in a
        // 1.5 s one), estimated or told the room; a lower amount does less; dry speech goes through almost untouched.
        QVERIFY2(known - before > 2.2 && std::fabs(autoT - known) < 0.3, qPrintable(got));
        QVERIFY2(mild > before + 0.5 && mild < known, qPrintable(got));
        QVERIFY2(srr(bigOut) - srr(big) > 2.7, qPrintable(got));
        QVERIFY2(dryKept > 23, qPrintable(got));
        // As a clip effect it runs on the source audio, like the other repairs.
        QVERIFY(isSourceAudioEffect("dereverb"));
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "dereverb");
        e.params["reverb_time"] = 0.8;
        auto wetPtr = std::make_shared<AudioBuffer>(wet);
        AudioBufferPtr cleaned = cleanedAudio("wet-room", wetPtr, {&e}, true);
        QVERIFY(cleaned && cleaned != wetPtr);
        target = room(0.8, 0, false, true);
        QVERIFY2(srr(*cleaned) > before + 2, qPrintable(QString::number(srr(*cleaned))));
    }

    void audioRepair() {
        constexpr int sr = 48000;
        // Amplitude of `hz` in channel `ch` of interleaved stereo, from sample `from` on.
        auto level = [](const std::vector<float>& b, int ch, double hz, size_t from) {
            std::complex<double> acc = 0;
            const size_t n = b.size() / 2;
            for (size_t i = from; i < n; ++i) acc += double(b[i * 2 + size_t(ch)]) * std::polar(1.0, -2 * M_PI * hz * double(i) / sr);
            return 2 * std::abs(acc) / double(n - from);
        };
        // De-Hum: 50 Hz mains and its harmonics out of a 440 Hz and 1 kHz tone.
        std::vector<float> b(size_t(sr) * 3 * 2);
        const double hum[4][2] = {{50, 0.05}, {100, 0.03}, {150, 0.02}, {250, 0.015}};
        for (size_t i = 0; i < b.size() / 2; ++i) {
            const double t = double(i) / sr;
            double v = 0.1 * std::sin(2 * M_PI * 440 * t) + 0.1 * std::sin(2 * M_PI * 1000 * t);
            for (const auto& h : hum) v += h[1] * std::sin(2 * M_PI * h[0] * t);
            b[i * 2] = b[i * 2 + 1] = float(v);
        }
        const auto withHum = b;
        fx::DeHum dh;
        dh.process(b.data(), sr * 3, sr, 50, 6, 30, 2);
        for (const auto& h : hum) {
            const double down = 20 * std::log10(level(b, 0, h[0], sr * 3 / 2) / level(withHum, 0, h[0], sr * 3 / 2));
            QVERIFY2(down < -25, qPrintable(QString("%1 Hz: %2 dB").arg(h[0]).arg(down)));
        }
        for (double hz : {440.0, 1000.0}) {
            const double kept = 20 * std::log10(level(b, 1, hz, sr * 3 / 2) / level(withHum, 1, hz, sr * 3 / 2));
            QVERIFY2(std::fabs(kept) < 0.2, qPrintable(QString("%1 Hz: %2 dB").arg(hz).arg(kept)));
        }

        // De-Click: a chord that swells, with a little hiss, and 40 clicks of two kinds.
        AudioBuffer clean;
        clean.sampleRate = sr;
        clean.samples.resize(size_t(sr) * 2 * 2);
        std::mt19937 rng(7);
        std::normal_distribution<double> hiss(0, 0.001);
        for (size_t i = 0; i < clean.samples.size() / 2; ++i) {
            const double t = double(i) / sr;
            const double chord = std::sin(2 * M_PI * 220 * t) + 0.6 * std::sin(2 * M_PI * 330 * t) + 0.4 * std::sin(2 * M_PI * 495 * t) +
                                 0.3 * std::sin(2 * M_PI * 660 * t);
            for (int c = 0; c < 2; ++c) clean.samples[i * 2 + size_t(c)] = float(0.2 * chord * (0.6 + 0.4 * std::sin(2 * M_PI * 0.7 * t + c)) + hiss(rng));
        }
        AudioBuffer clicked = clean;
        std::vector<int64_t> at;
        int expected = 0;
        for (int k = 0; k < 40; ++k) {
            const int64_t t = 1000 + int64_t(k) * 2300 + int64_t(rng() % 500);
            at.push_back(t);
            const bool both = k % 2 == 0, burst = k % 3 == 0;
            const float sign = rng() % 2 ? 1.0f : -1.0f;
            for (int c = 0; c < (both ? 2 : 1); ++c) {
                if (burst)  // a scratch: 0.3 ms of decaying crackle
                    for (int j = 0; j < 15; ++j) clicked.samples[size_t(t + j) * 2 + size_t(c)] += sign * 0.3f * std::exp(-j / 5.0f) * (j % 2 ? -1.0f : 1.0f);
                else
                    clicked.samples[size_t(t) * 2 + size_t(c)] += sign * 0.4f;
                ++expected;
            }
        }
        AudioBuffer fixed;
        int found = 0;
        declick(clicked, fixed, 50, 2, &found);
        QVERIFY2(found >= expected && found <= expected + 2, qPrintable(QString("%1 of %2").arg(found).arg(expected)));
        // Round each click the error is gone (over 35 dB down; scratches' tails too); away from them nothing changed.
        double before = 0, after = 0;
        std::vector<bool> near(size_t(clean.frames()), false);
        for (int64_t t : at)
            for (int64_t j = t - 40; j < t + 60; ++j) {
                near[size_t(j)] = true;
                for (int c = 0; c < 2; ++c) {
                    const size_t i = size_t(j) * 2 + size_t(c);
                    before += std::pow(clicked.samples[i] - clean.samples[i], 2);
                    after += std::pow(fixed.samples[i] - clean.samples[i], 2);
                }
            }
        qInfo("chord: %d of %d clicks found, error %.1f dB", found, expected, 10 * std::log10(after / before));
        QVERIFY2(10 * std::log10(after / before) < -35, qPrintable(QString::number(10 * std::log10(after / before))));
        for (int64_t j = 0; j < clean.frames(); ++j)
            if (!near[size_t(j)])
                for (int c = 0; c < 2; ++c) QCOMPARE(fixed.samples[size_t(j) * 2 + size_t(c)], clicked.samples[size_t(j) * 2 + size_t(c)]);
        // Clean audio is left alone.
        AudioBuffer untouched;
        declick(clean, untouched, 50, 2, &found);
        QCOMPARE(found, 0);
        QVERIFY(untouched.samples == clean.samples);

        // On speech: clicks out of the JFK clip, and the speech itself barely touched.
        std::string err;
        AudioBufferPtr speech = decodeAudio(MONTAGE_TEST_DATA_DIR "/jfk.wav", sr, &err);
        QVERIFY2(speech, err.c_str());
        AudioBuffer jfkClicked = *speech;
        std::vector<int64_t> jat;
        for (int64_t t = sr / 2; t < speech->frames() - sr / 2; t += sr / 4) {
            jat.push_back(t);
            for (int c = 0; c < 2; ++c) jfkClicked.samples[size_t(t) * 2 + size_t(c)] += (t / (sr / 4)) % 2 ? 0.3f : -0.3f;
        }
        AudioBuffer jfkFixed, jfkClean;
        declick(jfkClicked, jfkFixed, 50, 2, &found);
        int falseAlarms = 0;
        declick(*speech, jfkClean, 50, 2, &falseAlarms);
        double cb = 0, ca = 0, sig = 0, changed = 0;
        for (int64_t t : jat)
            for (int64_t j = t - 40; j < t + 60; ++j) {
                const size_t i = size_t(j) * 2;
                cb += std::pow(jfkClicked.samples[i] - speech->samples[i], 2);
                ca += std::pow(jfkFixed.samples[i] - speech->samples[i], 2);
            }
        for (size_t i = 0; i < speech->samples.size(); ++i) {
            sig += std::pow(speech->samples[i], 2);
            changed += std::pow(jfkClean.samples[i] - speech->samples[i], 2);
        }
        qInfo("jfk: %d found for %d clicks, %.1f dB, %d on clean speech, change %.1f dB", found, int(jat.size()) * 2,
              10 * std::log10(ca / cb), falseAlarms, 10 * std::log10(changed / sig + 1e-30));
        QVERIFY(found >= int(jat.size()) * 2 * 9 / 10 && falseAlarms <= 6);
        QVERIFY2(10 * std::log10(ca / cb) < -15, qPrintable(QString::number(10 * std::log10(ca / cb))));
        QVERIFY2(10 * std::log10(changed / sig + 1e-30) < -30, qPrintable(QString::number(10 * std::log10(changed / sig + 1e-30))));
    }

    void creativeAudioEffects() {
        constexpr int sr = 48000;
        auto tone = [](double hz, double amp) {
            std::vector<float> b(size_t(sr) * 2);
            for (int i = 0; i < sr; ++i) b[size_t(i) * 2] = b[size_t(i) * 2 + 1] = float(amp * std::sin(2 * M_PI * hz * i / sr));
            return b;
        };
        auto db = [](double a, double b) { return 20 * std::log10(a / b); };
        // A flanger held still is a comb: 1 ms at half mix cancels 500 Hz and passes 1 kHz;
        // feedback makes 1 kHz ring up (to 1.5 times at 50 %).
        {
            fx::ModDelay fl;
            auto b = tone(500, 0.2);
            fl.process(b.data(), sr, sr, 1.0, 0, 0, 0, 0, 0.5);
            QVERIFY2(db(toneLevel(b, 0, 500, sr / 2), 0.2) < -30, qPrintable(QString::number(db(toneLevel(b, 0, 500, sr / 2), 0.2))));
            fx::ModDelay pass;
            b = tone(1000, 0.2);
            pass.process(b.data(), sr, sr, 1.0, 0, 0, 0, 0, 0.5);
            QVERIFY(std::fabs(db(toneLevel(b, 0, 1000, sr / 2), 0.2)) < 0.3);
            fx::ModDelay ring;
            b = tone(1000, 0.2);
            ring.process(b.data(), sr, sr, 1.0, 0, 0, 0.5, 0, 0.5);
            QVERIFY2(std::fabs(db(toneLevel(b, 0, 1000, sr / 2), 0.3)) < 0.3, qPrintable(QString::number(toneLevel(b, 0, 1000, sr / 2))));
        }
        // Chorus: the copy wanders in time, the two sides differently; with no mix nothing changes.
        {
            fx::ModDelay ch;
            auto b = tone(1000, 0.2);
            ch.process(b.data(), sr, sr, 15, 3, 0.8, 0, 1, 0.5);
            double diff = 0, energy = 0;
            for (size_t i = size_t(sr) / 2; i < size_t(sr); ++i) diff += std::fabs(b[i * 2] - b[i * 2 + 1]), energy += std::fabs(b[i * 2]);
            QVERIFY(diff > energy * 0.1);
            fx::ModDelay dry;
            b = tone(1000, 0.2);
            const auto before = b;
            dry.process(b.data(), sr, sr, 15, 3, 0.8, 0, 1, 0);
            QCOMPARE(b, before);
        }
        // Phaser held at 1 kHz: four all-passes there cancel 414 Hz and 2414 Hz (tan 22.5° and 67.5°) and pass 1 kHz.
        {
            for (double hz : {1000 * std::tan(M_PI / 8), 1000 * std::tan(3 * M_PI / 8)}) {
                fx::Phaser ph;
                auto b = tone(hz, 0.2);
                ph.process(b.data(), sr, sr, 4, 1000, 3000, 0, 0, 0, 0.5);
                QVERIFY2(db(toneLevel(b, 0, hz, sr / 2), 0.2) < -30, qPrintable(QString("%1 Hz %2 dB").arg(hz).arg(db(toneLevel(b, 0, hz, sr / 2), 0.2))));
            }
            fx::Phaser ph;
            auto b = tone(1000, 0.2);
            ph.process(b.data(), sr, sr, 4, 1000, 3000, 0, 0, 0, 0.5);
            QVERIFY(std::fabs(db(toneLevel(b, 0, 1000, sr / 2), 0.2)) < 0.3);
        }
        // Tremolo at 4 Hz, full depth: silent at three quarters of each cycle, full at a quarter. Auto-pan keeps the power.
        {
            fx::Tremolo tr;
            std::vector<float> b(size_t(sr) * 2, 0.5f);
            tr.process(b.data(), sr, sr, 4, 1, 0, false);
            QVERIFY(std::fabs(b[size_t(sr / 16) * 2] - 0.5f) < 1e-3f && std::fabs(b[size_t(3 * sr / 16) * 2]) < 1e-3f);
            fx::Tremolo pan;
            std::vector<float> c(size_t(sr) * 2, 0.5f);
            pan.process(c.data(), sr, sr, 4, 1, 0, true);
            QVERIFY(std::fabs(c[size_t(sr / 16) * 2]) < 1e-3f && c[size_t(sr / 16) * 2 + 1] > 0.7f);
            for (size_t i = 0; i < c.size(); i += 2) QVERIFY(std::fabs(c[i] * c[i] + c[i + 1] * c[i + 1] - 0.5f) < 1e-4f);
        }
        // Saturation: tape adds odd harmonics only, tube even ones too; the level of -12 dBFS holds.
        {
            fx::Saturator tape;
            auto b = tone(1000, 0.5);
            tape.process(b.data(), sr, sr, 0, 18, 0, 1, 0);
            const double f1 = toneLevel(b, 0, 1000, sr / 2);
            QVERIFY2(db(toneLevel(b, 0, 3000, sr / 2), f1) > -30, qPrintable(QString::number(db(toneLevel(b, 0, 3000, sr / 2), f1))));
            QVERIFY2(db(toneLevel(b, 0, 2000, sr / 2), f1) < -80, qPrintable(QString::number(db(toneLevel(b, 0, 2000, sr / 2), f1))));
            fx::Saturator tube;
            b = tone(1000, 0.5);
            tube.process(b.data(), sr, sr, 1, 18, 0, 1, 0);
            QVERIFY2(db(toneLevel(b, 0, 2000, sr / 2), toneLevel(b, 0, 1000, sr / 2)) > -40,
                     qPrintable(QString::number(db(toneLevel(b, 0, 2000, sr / 2), toneLevel(b, 0, 1000, sr / 2)))));
            fx::Saturator level;
            b = tone(1000, 0.25);
            level.process(b.data(), sr, sr, 0, 24, 0, 1, 0);
            float peak = 0;
            for (size_t i = size_t(sr); i < b.size(); i += 2) peak = std::max(peak, std::fabs(b[i]));
            QVERIFY2(std::fabs(peak - 0.25f) < 0.0125f, qPrintable(QString::number(peak)));
            // Anti-aliasing: a hard-clipped 5 kHz tone's 9th harmonic (45 kHz) folds to 3 kHz; less of it than a plain clip.
            fx::Saturator clip;
            b = tone(5000, 0.9);
            auto naive = b;
            clip.process(b.data(), sr, sr, 2, 24, 0, 1, 0);
            for (float& v : naive) v = std::clamp(v * 15.85f, -1.0f, 1.0f);
            const double alias = db(toneLevel(b, 0, 3000, sr / 2), toneLevel(b, 0, 5000, sr / 2));
            const double plain = db(toneLevel(naive, 0, 3000, sr / 2), toneLevel(naive, 0, 5000, sr / 2));
            qInfo("hard clip: 3 kHz alias at %.1f dB, %.1f dB plain", alias, plain);
            QVERIFY2(alias < plain - 15, qPrintable(QString("%1 against %2").arg(alias).arg(plain)));
        }
        // Stereo width: none is mono, double makes a left-only sound 1.5 and -0.5, bass can be made mono.
        {
            std::vector<float> lr{0.4f, 0.0f, -0.2f, 0.1f};
            auto b = lr;
            fx::StereoWidth w0;
            w0.process(b.data(), 2, sr, 0, 0);
            QVERIFY(b[0] == b[1] && b[2] == b[3]);
            b = lr;
            fx::StereoWidth w1;
            w1.process(b.data(), 2, sr, 1, 0);
            for (size_t i = 0; i < b.size(); ++i) QVERIFY(std::fabs(b[i] - lr[i]) < 1e-7f);
            b = {1.0f, 0.0f};
            fx::StereoWidth w2;
            w2.process(b.data(), 1, sr, 2, 0);
            QVERIFY(std::fabs(b[0] - 1.5f) < 1e-6f && std::fabs(b[1] + 0.5f) < 1e-6f);
            for (double hz : {50.0, 2000.0}) {
                std::vector<float> left(size_t(sr) * 2, 0.0f);
                for (int i = 0; i < sr; ++i) left[size_t(i) * 2] = float(0.3 * std::sin(2 * M_PI * hz * i / sr));
                fx::StereoWidth bass;
                bass.process(left.data(), sr, sr, 1, 150);
                const double l = toneLevel(left, 0, hz, sr / 2), r = toneLevel(left, 1, hz, sr / 2);
                if (hz < 100) QVERIFY2(std::fabs(db(l, r)) < 0.5, qPrintable(QString("%1 %2").arg(l).arg(r)));
                else QVERIFY2(r < l * 0.03, qPrintable(QString("%1 %2").arg(l).arg(r)));
            }
        }
        // In the mixer, as clip effects.
        const std::string wav = path("creative.wav");
        std::vector<float> mono(static_cast<size_t>(sr));
        for (int i = 0; i < sr; ++i) mono[size_t(i)] = float(0.3 * std::sin(2 * M_PI * 440 * i / sr));
        QVERIFY(writeMonoWav(wav, mono, sr));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& c = s.audioTracks[0].clips.front();
        for (const char* type : {"dehum", "chorus", "flanger", "phaser", "tremolo", "saturation", "stereo_width", "multiband"}) {
            QVERIFY2(findEffectInfo(type), type);
            c.effects.push_back(makeEffect(p, type));
        }
        AudioMixer mixer;
        std::vector<float> out(4800 * 2);
        mixer.mix(p, s, 12000, 4800, out.data());
        double energy = 0;
        for (float v : out) {
            QVERIFY(std::isfinite(v));
            energy += double(v) * v;
        }
        QVERIFY(energy > 1);
    }

    void multibandCompressor() {
        constexpr int sr = 48000;
        auto tones = [](std::initializer_list<std::pair<double, double>> parts) {
            std::vector<float> b(size_t(sr) * 2 * 2);
            for (size_t i = 0; i < b.size() / 2; ++i) {
                double v = 0;
                for (const auto& [hz, amp] : parts) v += amp * std::sin(2 * M_PI * hz * double(i) / sr);
                b[i * 2] = b[i * 2 + 1] = float(v);
            }
            return b;
        };
        auto db = [](double a, double b) { return 20 * std::log10(a / b); };
        // Doing nothing, the three bands add back up flat, crossovers included.
        const fx::BandSettings idle[3] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
        for (double hz : {60.0, 200.0, 1000.0, 2500.0, 9000.0}) {
            fx::MultibandCompressor mb;
            auto b = tones({{hz, 0.2}});
            mb.process(b.data(), 2 * sr, sr, 200, 2500, idle, 10, 150, 0);
            const double change = db(toneLevel(b, 0, hz, sr), 0.2);
            QVERIFY2(std::fabs(change) < 0.1, qPrintable(QString("%1 Hz: %2 dB").arg(hz).arg(change)));
        }
        // A loud bass is squeezed while a quiet treble beside it is left alone.
        const fx::BandSettings bass[3] = {{-30, 4, 0}, {0, 1, 0}, {0, 1, 0}};
        fx::MultibandCompressor mb;
        auto b = tones({{80, 0.5}, {6000, 0.03}});
        mb.process(b.data(), 2 * sr, sr, 200, 2500, bass, 10, 150, 0);
        const double low = db(toneLevel(b, 0, 80, sr), 0.5), high = db(toneLevel(b, 0, 6000, sr), 0.03);
        qInfo("multiband: bass %.1f dB, treble %.2f dB", low, high);
        QVERIFY(low < -12 && std::fabs(high) < 0.5);
        // A band's gain lifts only that band.
        const fx::BandSettings lift[3] = {{0, 1, 0}, {0, 1, 6}, {0, 1, 0}};
        fx::MultibandCompressor up;
        b = tones({{900, 0.1}, {8000, 0.1}});
        up.process(b.data(), 2 * sr, sr, 200, 2500, lift, 10, 150, 0);
        QVERIFY(std::fabs(db(toneLevel(b, 0, 900, sr), 0.1) - 6) < 0.4 && std::fabs(db(toneLevel(b, 0, 8000, sr), 0.1)) < 0.3);
    }

    void pitchShifting() {
        constexpr int sr = 48000;
        // A 200 Hz tone with eight harmonics: up 4 semitones and down 5, the fundamental moves; length and level stay.
        AudioBuffer in;
        in.sampleRate = sr;
        in.samples.resize(size_t(sr) * 2 * 2);
        for (size_t i = 0; i < in.samples.size() / 2; ++i) {
            double v = 0;
            for (int k = 1; k <= 8; ++k) v += 0.15 / k * std::sin(2 * M_PI * 200 * k * double(i) / sr);
            in.samples[i * 2] = in.samples[i * 2 + 1] = float(v);
        }
        auto rms = [](const AudioBuffer& b, size_t from, size_t to) {
            double s = 0;
            for (size_t i = from; i < to; ++i) s += double(b.samples[i * 2]) * b.samples[i * 2];
            return std::sqrt(s / double(to - from));
        };
        for (double st : {4.0, -5.0}) {
            AudioBuffer out;
            pitchShift(in, out, st);
            QCOMPARE(out.frames(), in.frames());
            const double f = pitchOf(out.samples, sr / 2, sr / 2 + sr / 4), want = 200 * std::pow(2.0, st / 12);
            qInfo("%+.0f semitones: %.2f Hz (wanted %.2f)", st, f, want);
            QVERIFY2(std::fabs(f / want - 1) < 0.005, qPrintable(QString("%1 Hz, wanted %2").arg(f).arg(want)));
            const double change = 20 * std::log10(rms(out, sr / 2, sr * 3 / 2) / rms(in, sr / 2, sr * 3 / 2));
            QVERIFY2(std::fabs(change) < 1, qPrintable(QString::number(change)));
        }
        AudioBuffer same;
        pitchShift(in, same, 0);
        QVERIFY(same.samples == in.samples);
        // Timing holds: a note starting half a second in still starts there.
        AudioBuffer burst;
        burst.sampleRate = sr;
        burst.samples.assign(size_t(sr) * 2 * 2, 0.0f);
        for (size_t i = size_t(sr) / 2; i < size_t(sr) * 3 / 2; ++i) burst.samples[i * 2] = burst.samples[i * 2 + 1] = float(0.3 * std::sin(2 * M_PI * 330 * double(i) / sr));
        for (double st : {7.0, -7.0}) {
            AudioBuffer out;
            pitchShift(burst, out, st);
            // The middle of the rise, on a 5 ms envelope.
            size_t onset = 0;
            double env = 0;
            for (size_t i = 0; i < size_t(out.frames()); ++i) {
                env = env * 0.996 + std::fabs(out.samples[i * 2]) * 0.004;
                if (env > 0.3 * 2 / M_PI / 2) {
                    onset = i;
                    break;
                }
            }
            const double ms = (double(onset) - sr / 2) * 1000 / sr;
            qInfo("%+.0f semitones: onset %.1f ms off", st, ms);
            QVERIFY2(std::fabs(ms) < 10, qPrintable(QString("%1 ms").arg(ms)));
        }
        // Through the mixer: an octave up.
        const std::string wav = path("pitch.wav");
        std::vector<float> mono(size_t(sr) * 2);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = float(0.3 * std::sin(2 * M_PI * 440 * double(i) / sr));
        QVERIFY(writeMonoWav(wav, mono, sr));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Effect up = makeEffect(p, "pitch_shift");
        up.params["semitones"] = 12.0;
        s.audioTracks[0].clips.front().effects.push_back(up);
        AudioMixer mixer;
        std::vector<float> out(9600 * 2);
        mixer.mix(p, s, 24000, 9600, out.data());
        QVERIFY2(toneLevel(out, 0, 880, 0) > 10 * toneLevel(out, 0, 440, 0),
                 qPrintable(QString("%1 %2").arg(toneLevel(out, 0, 880, 0)).arg(toneLevel(out, 0, 440, 0))));
    }

    void maintainPitchOnSpeedChanges() {
        constexpr int sr = 48000;
        // A 200 Hz tone with harmonics, two seconds.
        AudioBuffer in;
        in.sampleRate = sr;
        in.samples.resize(size_t(sr) * 2 * 2);
        for (size_t i = 0; i < in.samples.size() / 2; ++i) {
            double v = 0;
            for (int k = 1; k <= 6; ++k) v += 0.15 / k * std::sin(2 * M_PI * 200 * k * double(i) / sr);
            in.samples[i * 2] = in.samples[i * 2 + 1] = float(v);
        }
        auto rms = [](const AudioBuffer& b, size_t from, size_t to) {
            double sum = 0;
            for (size_t i = from; i < to; ++i) sum += double(b.samples[i * 2]) * b.samples[i * 2];
            return std::sqrt(sum / double(to - from));
        };
        const int hop = stretchHop(sr);
        auto along = [&](double speed, int64_t frames) {
            std::vector<double> pos;
            for (int64_t k = 0; k <= frames / hop + 2; ++k) pos.push_back(double(k * hop) * speed);
            return pos;
        };
        // Twice as fast and half as fast: half and twice the length, the same pitch and level.
        for (double speed : {2.0, 0.5, 1.37}) {
            const int64_t frames = int64_t(std::llround(double(in.frames()) / speed));
            AudioBuffer out;
            wsolaStretch(in, along(speed, frames), hop, frames, out);
            QCOMPARE(out.frames(), frames);
            const size_t mid = size_t(frames / 2);
            const double f = pitchOf(out.samples, mid - 4800, mid + 4800);
            qInfo("speed %.2f: %.2f Hz", speed, f);
            QVERIFY2(std::fabs(f / 200 - 1) < 0.01, qPrintable(QString("%1 Hz at %2x").arg(f).arg(speed)));
            const double change = 20 * std::log10(rms(out, mid - 9600, mid + 9600) / rms(in, size_t(sr) / 2, size_t(sr) * 3 / 2));
            QVERIFY2(std::fabs(change) < 1.5, qPrintable(QString("%1 dB at %2x").arg(change).arg(speed)));
            // No clicks where grains join: the waveform never jumps further between samples than the tone does.
            auto steepest = [](const AudioBuffer& b, size_t from, size_t to) {
                double most = 0;
                for (size_t i = from + 1; i < to; ++i) most = std::max(most, double(std::fabs(b.samples[i * 2] - b.samples[(i - 1) * 2])));
                return most;
            };
            const double jump = steepest(out, 4800, size_t(frames) - 4800), tone = steepest(in, 4800, size_t(in.frames()) - 4800);
            QVERIFY2(jump < 1.25 * tone, qPrintable(QString("%1 against %2 at %3x").arg(jump).arg(tone).arg(speed)));
        }
        // At normal speed it is the sound itself.
        AudioBuffer same;
        wsolaStretch(in, along(1.0, in.frames()), hop, in.frames(), same);
        double worst = 0;
        for (size_t i = 0; i < in.samples.size(); ++i) worst = std::max(worst, double(std::fabs(same.samples[i] - in.samples[i])));
        QVERIFY2(worst < 1e-5, qPrintable(QString::number(worst)));
        // Timing follows the map: a note half a second in starts a quarter of a second in at double speed.
        AudioBuffer burst;
        burst.sampleRate = sr;
        burst.samples.assign(size_t(sr) * 2 * 2, 0.0f);
        for (size_t i = size_t(sr) / 2; i < size_t(sr) * 3 / 2; ++i) burst.samples[i * 2] = burst.samples[i * 2 + 1] = float(0.3 * std::sin(2 * M_PI * 330 * double(i) / sr));
        AudioBuffer fast;
        wsolaStretch(burst, along(2.0, sr), hop, sr, fast);
        size_t onset = 0;
        for (size_t i = 0; i < size_t(fast.frames()); ++i)
            if (std::fabs(fast.samples[i * 2]) > 0.15) {
                onset = i;
                break;
            }
        const double ms = (double(onset) - sr / 4.0) * 1000 / sr;
        QVERIFY2(std::fabs(ms) < 15, qPrintable(QString("%1 ms").arg(ms)));

        // Through the mixer: a 440 Hz clip at double speed rises an octave, unless its pitch is kept.
        const std::string wav = path("keep-pitch.wav");
        std::vector<float> mono(size_t(sr) * 4);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = float(0.3 * std::sin(2 * M_PI * 440 * double(i) / sr));
        QVERIFY(writeMonoWav(wav, mono, sr));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id clip = s.audioTracks[0].clips.front().id;
        QVERIFY(edit::setSpeed(p, s, clip, 2.0, true).ok);
        AudioMixer mixer;
        std::vector<float> out(9600 * 2);
        mixer.mix(p, s, 24000, 9600, out.data());
        QVERIFY(toneLevel(out, 0, 880, 0) > 10 * toneLevel(out, 0, 440, 0));
        QVERIFY(edit::setMaintainPitch(p, s, clip, true));
        QVERIFY(!edit::setMaintainPitch(p, s, clip, true));
        mixer.mix(p, s, 24000, 9600, out.data());
        QVERIFY2(toneLevel(out, 0, 440, 0) > 10 * toneLevel(out, 0, 880, 0),
                 qPrintable(QString("%1 %2").arg(toneLevel(out, 0, 440, 0)).arg(toneLevel(out, 0, 880, 0))));
        // Under a speed ramp too: 440 Hz all the way through.
        QVERIFY(edit::setSpeed(p, s, clip, 1.0, true).ok);
        QVERIFY(edit::applySpeedRamp(p, s, clip, "hero").ok);
        QVERIFY(edit::clipById(s, clip)->ramped());
        const Clip& ramped = *edit::clipById(s, clip);
        const int64_t total = int64_t(std::llround(double(ramped.duration) * sr / s.fpsValue()));
        std::vector<float> whole(size_t(total) * 2);
        mixer.mix(p, s, 0, total, whole.data());
        for (int k = 1; k <= 4; ++k) {
            const size_t at = size_t(total * k / 5);
            const double f = pitchOf(whole, at - 2400, at + 2400);
            QVERIFY2(std::fabs(f / 440 - 1) < 0.02, qPrintable(QString("%1 Hz at %2/5").arg(f).arg(k)));
        }

        // Over MCP: on the clip and its linked sound.
        p.media.clear();
        Project q = makeDefaultProject();
        MediaItem qm = probeOrFail(q, wav);
        q.media.push_back(qm);
        QVERIFY(edit::placeMedia(q, *q.active(), qm.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("keep-pitch.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        const double qclip = double(q.active()->audioTracks[0].clips.front().id);
        QJsonObject r = call("montage_set_speed", {{"project", project}, {"clip", qclip}, {"speed", 1.5}, {"maintain_pitch", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        {
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            const Clip& a = back.active()->audioTracks[0].clips.front();
            QCOMPARE(a.speed, 1.5);
            QVERIFY(a.timing.p("maintain_pitch", 0) > 0.5);
            for (Id other : edit::linkedClips(*back.active(), a.id))
                QVERIFY(edit::clipById(*back.active(), other)->timing.p("maintain_pitch", 0) > 0.5);
        }
        r = call("montage_speed_ramp", {{"project", project}, {"clip", qclip}, {"preset", "bullet"}, {"maintain_pitch", false}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        {
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            QVERIFY(back.active()->audioTracks[0].clips.front().timing.p("maintain_pitch", 1) < 0.5);
        }
    }

    void clipAnimationOverMcp() {
        Project q = makeDefaultProject();
        Clip c = makeGeneratorClip(q, "color", 90);
        edit::overwrite(q, *q.active(), {TrackKind::Video, 0}, c);
        const double id = double(q.active()->videoTracks[0].clips.front().id);
        const QString project = QString::fromStdString(path("animate.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_animate_clip"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"clip", id}, {"in", "pop"}, {"in_seconds", 0.4}, {"out", "slide_down"}, {"combo", "float"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject anim = r.value("structuredContent").toObject().value("animation").toObject();
        QCOMPARE(anim.value("in").toObject().value("type").toString(), QString("pop"));
        QCOMPARE(anim.value("in").toObject().value("seconds").toDouble(), 0.4);
        QCOMPARE(anim.value("combo").toObject().value("seconds").toDouble(), 1.0);
        {
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            const Clip& k = back.active()->videoTracks[0].clips.front();
            QCOMPARE(k.animOut, (ClipAnimation{"slide_down", 0.5}));
        }
        QVERIFY(call({{"project", project}, {"clip", id}, {"in", "teleport"}}).value("isError").toBool());
        QVERIFY(call({{"project", project}, {"clip", id}}).value("isError").toBool());  // nothing asked
        r = call({{"project", project}, {"clip", id}, {"combo", "none"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(!r.value("structuredContent").toObject().value("animation").toObject().contains("combo"));
    }

    void dualSystemSound() {
        // The field recorder: JFK's speech, stamped two seconds before 10:00:00, logged in iXML (after the sound).
        std::string err;
        AudioBufferPtr speech = decodeAudio(MONTAGE_TEST_DATA_DIR "/jfk.wav", 48000, &err);
        QVERIFY2(speech, err.c_str());
        const std::string rec = path("T03.wav");
        const std::string ixml =
            "<?xml version=\"1.0\"?><BWFXML><PROJECT>Inauguration</PROJECT><SCENE>12A</SCENE><TAKE>3</TAKE><TAPE>D001</TAPE>"
            "<CIRCLED>TRUE</CIRCLED><NOTE>wind on take 2</NOTE><SPEED><TIMECODE_RATE>25/1</TIMECODE_RATE><TIMECODE_FLAG>NDF</TIMECODE_FLAG>"
            "</SPEED><TRACK_LIST><TRACK_COUNT>2</TRACK_COUNT><TRACK><CHANNEL_INDEX>1</CHANNEL_INDEX><NAME>Boom</NAME></TRACK>"
            "<TRACK><CHANNEL_INDEX>2</CHANNEL_INDEX><NAME>Lav</NAME></TRACK></TRACK_LIST></BWFXML>";
        writeBwf(rec, *speech, uint64_t(35998) * 48000, "sSCENE=99\r\nsTAKE=1\r\n", ixml);
        FieldRecording f;
        QVERIFY(readFieldRecording(rec, f));
        QCOMPARE(f.startSeconds, 35998.0);
        QCOMPARE(f.scene, std::string("12A"));  // iXML over bext
        QCOMPARE(f.trackNames, (std::vector<std::string>{"Boom", "Lav"}));
        QVERIFY(f.circled);
        QVERIFY(!readFieldRecording(MONTAGE_TEST_DATA_DIR "/jfk.wav", f));  // a plain WAV
        // Read on import.
        Project p = makeDefaultProject();
        MediaItem sound = probeOrFail(p, rec);
        QCOMPARE(sound.timecode, 35998.0);
        QCOMPARE(sound.metadata["scene"], std::string("12A"));
        QCOMPARE(sound.metadata["take"], std::string("3"));
        QCOMPARE(sound.metadata["tape"], std::string("D001"));
        QCOMPARE(sound.metadata["comment"], std::string("wind on take 2"));
        QCOMPARE(sound.metadata["tracks"], std::string("Boom, Lav"));
        QCOMPARE(sound.metadata["circled"], std::string("Yes"));
        QCOMPARE(sound.metadata["timecode_rate"], std::string("25/1"));
        QVERIFY(sound.duration > 10);
        // The camera: six seconds of picture whose own sound is the speech from two seconds in.
        {
            Project cam = makeDefaultProject();
            Sequence& s = *cam.active();
            s.width = 320;
            s.height = 180;
            s.fps = Rational{25, 1};
            Clip picture = makeGeneratorClip(cam, "color", 150);
            edit::overwrite(cam, s, {TrackKind::Video, 0}, picture);
            MediaItem src = probeOrFail(cam, MONTAGE_TEST_DATA_DIR "/jfk.wav");
            cam.media.push_back(src);
            Clip scratch = makeClip(cam, src, TrackKind::Audio, s);
            scratch.sourceIn = 50;  // two seconds in
            scratch.duration = 150;
            edit::overwrite(cam, s, {TrackKind::Audio, 0}, scratch);
            ExportSettings st;
            st.path = path("A001.mp4");
            QVERIFY2(exportSequence(cam, s, st, nullptr, nullptr, &err), err.c_str());
        }
        MediaItem camera = probeOrFail(p, path("A001.mp4"));
        camera.timecode = 36000;  // 10:00:00:00
        p.media.push_back(camera);
        p.media.push_back(sound);
        // By timecode and by the sound: both two seconds early.
        SoundSync byTc = syncSound(p, camera.id, sound.id, SyncBy::Timecode);
        QVERIFY(byTc.found && byTc.byTimecode);
        QVERIFY(std::fabs(byTc.offset + 2) < 1e-6);
        SoundSync byWave = syncSound(p, camera.id, sound.id, SyncBy::Waveform);
        QVERIFY2(byWave.found && !byWave.byTimecode && std::fabs(byWave.offset + 2) < 0.03,
                 qPrintable(QString("%1 (%2)").arg(byWave.offset).arg(byWave.confidence)));
        // Merged by the sound: at a second in, the merged clip plays what the recorder had three seconds in.
        std::string why;
        const Id merged = mergeClips(p, camera.id, {sound.id}, {byWave.offset}, {}, &why);
        QVERIFY2(merged, why.c_str());
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = Rational{25, 1};
        QVERIFY(edit::placeMedia(p, s, merged, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        AudioMixer mixer;
        std::vector<float> out(24000 * 2);
        mixer.mix(p, s, 48000, 24000, out.data());
        double dot = 0, a2 = 0, b2 = 0;
        for (size_t i = 0; i < 24000; ++i) {
            const double x = out[i * 2], y = speech->samples[(size_t(3 * 48000) + i) * 2];
            dot += x * y, a2 += x * x, b2 += y * y;
        }
        const double corr = dot / std::sqrt(a2 * b2 + 1e-12);
        QVERIFY2(corr > 0.95, qPrintable(QString::number(corr)));  // the recorder's sound, lined up (the camera's is muted)
        // Sync Dailies over the bin: paired by timecode.
        std::vector<std::string> report;
        const auto made = syncDailies(p, {camera.id, sound.id}, false, &report);
        QCOMPARE(made.size(), size_t(1));
        QVERIFY2(!report.empty() && report.back().find("timecode") != std::string::npos, report.empty() ? "" : report.back().c_str());
        // Over MCP: merged by waveform and placed; then dailies for the whole project.
        Project q = makeDefaultProject();
        const QString project = QString::fromStdString(path("dailies.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_merge_clips", {{"project", project}, {"video", QString::fromStdString(path("A001.mp4"))},
                                                     {"sounds", QJsonArray{QString::fromStdString(rec)}}, {"sync", "waveform"}, {"place", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject synced = r.value("structuredContent").toObject().value("synced").toArray().at(0).toObject();
        QVERIFY(std::fabs(synced.value("offset").toDouble() + 2) < 0.03 && synced.value("by").toString() == "waveform");
        {
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            QCOMPARE(back.active()->videoTracks[0].clips.size(), size_t(1));
            QVERIFY(isMergedClip(back, back.active()->videoTracks[0].clips[0].mediaId));
        }
        r = call("montage_merge_clips", {{"project", project}, {"video", QString::fromStdString(path("A001.mp4"))},
                                         {"sounds", QJsonArray{QString::fromStdString(rec)}}, {"sync", "timecode"}});
        QVERIFY(r.value("isError").toBool());  // the camera file carries no timecode
        r = call("montage_sync_dailies", {{"project", project}, {"place", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("merged").toArray().size(), 1);
    }

    void liveLoudness() {
        const int rate = 48000;
        auto tone = [&](double amp, double seconds) {
            std::vector<float> v(size_t(rate * seconds) * 2);
            for (size_t i = 0; i < v.size() / 2; ++i) v[i * 2] = v[i * 2 + 1] = float(amp * std::sin(2 * M_PI * 1000.0 * double(i) / rate));
            return v;
        };
        LoudnessMeter m(rate);
        QVERIFY(m.momentary() <= -70 && m.shortTerm() <= -70 && m.loudnessRange() == 0.0);
        // A steady tone: momentary, short-term and integrated agree.
        auto quiet = tone(0.1, 4);
        m.add(quiet.data(), int64_t(quiet.size() / 2));
        const double integrated = m.result().integrated;
        QVERIFY2(std::fabs(m.momentary() - integrated) < 0.1 && std::fabs(m.shortTerm() - integrated) < 0.1,
                 qPrintable(QString("%1 %2 %3").arg(m.momentary()).arg(m.shortTerm()).arg(integrated)));
        QVERIFY(m.loudnessRange() < 0.5);
        QVERIFY(std::fabs(m.seconds() - 4) < 0.01);
        // Three times louder (+9.5 dB) for a second: momentary follows at once, short-term only part way.
        auto loud = tone(0.3, 1);
        m.add(loud.data(), int64_t(loud.size() / 2));
        QVERIFY2(std::fabs(m.momentary() - (integrated + 9.54)) < 0.2, qPrintable(QString::number(m.momentary() - integrated)));
        QVERIFY(m.shortTerm() > integrated + 2 && m.shortTerm() < integrated + 7);
        QVERIFY(m.maxMomentary() >= m.momentary() - 1e-9 && m.maxShortTerm() >= m.shortTerm() - 1e-9);
        // Alternating 10 dB every 6 s for a minute: a range of about 10 LU.
        LoudnessMeter r(rate);
        for (int k = 0; k < 10; ++k) {
            auto part = tone(k % 2 ? 0.316 : 0.1, 6);
            r.add(part.data(), int64_t(part.size() / 2));
        }
        QVERIFY2(std::fabs(r.loudnessRange() - 10) < 1.0, qPrintable(QString::number(r.loudnessRange())));
        r.reset();
        QCOMPARE(r.seconds(), 0.0);
        QVERIFY(r.momentary() <= -70);
    }

    void peakLimiter() {
        // A quiet tone with a loud burst in the middle: the burst is held under the ceiling, the rest passes untouched.
        const int rate = 48000, n = rate;
        std::vector<float> in(size_t(n) * 2), out(in.size());
        for (int i = 0; i < n; ++i) {
            const double a = (i >= 24000 && i < 24480) ? 1.6 : 0.2;
            in[size_t(i) * 2] = in[size_t(i) * 2 + 1] = float(a * std::sin(2 * M_PI * 440.0 * i / rate));
        }
        PeakLimiter lim(rate, -1.0);
        const int delay = lim.latency();
        QCOMPARE(delay, 240);  // 5 ms
        // In two pieces, as an export feeds it.
        lim.process(in.data(), out.data(), 10000);
        lim.process(in.data() + 20000, out.data() + 20000, n - 10000);
        const float ceiling = float(std::pow(10.0, -1.0 / 20));
        float peak = 0;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        QVERIFY2(peak <= ceiling + 1e-6f, qPrintable(QString::number(peak)));
        QVERIFY(peak > ceiling * 0.95f);
        // Delayed by the look-ahead, and untouched before the burst (in both pieces).
        for (int i : {1000, 9000, 21000})
            QVERIFY2(std::fabs(out[size_t(i + delay) * 2] - in[size_t(i) * 2]) < 1e-6f, qPrintable(QString::number(i)));
        // The gain recovers over the release after the burst: well down soon after, nearly back 5 releases later.
        QVERIFY(std::fabs(out[size_t(25000 + delay) * 2]) < 0.9f * std::fabs(in[size_t(25000) * 2]) + 1e-6f || std::fabs(in[size_t(25000) * 2]) < 0.01f);
        for (int i = 45000; i < 45100; ++i) QVERIFY(std::fabs(out[size_t(i + delay) * 2] - in[size_t(i) * 2]) < 0.002f);
    }

    void surroundMixExportAndStems() {
        // Two mono tones: 440 Hz on A1 (narrowed to the centre, with some LFE) and 1 kHz on A2 (a point, back left).
        const int rate = 48000;
        auto tone = [&](double hz, const char* name) {
            std::vector<float> x(size_t(rate) * 3);
            for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.3 * std::sin(2 * M_PI * hz * double(i) / rate));
            const std::string f = path(name);
            writeMonoWav(f, x, rate);
            return f;
        };
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        for (const std::string& f : {tone(440, "a440.wav"), tone(1000, "a1k.wav")}) {
            MediaItem m = probeOrFail(p, f);
            p.media.push_back(m);
        }
        QVERIFY(edit::placeMedia(p, s, p.media[0].id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::placeMedia(p, s, p.media[1].id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 1}, false).ok);
        s.audioLayout = "5.1";
        s.audioTracks[0].surround.width = 0;
        s.audioTracks[0].surround.lfeDb = -6;
        s.audioTracks[1].surround.x = -1;
        s.audioTracks[1].surround.y = -1;
        s.audioTracks[1].surround.width = 0;
        // The mix in 5.1: L R C LFE Ls Rs.
        AudioMixer mixer;
        const int n = rate / 2;
        std::vector<float> six(size_t(n) * 6);
        mixer.mixLayout(p, s, rate / 2, n, six.data());
        auto rms = [&](const std::vector<float>& buf, int ch, int channels) {
            double acc = 0;
            const size_t frames = buf.size() / size_t(channels);
            for (size_t i = 0; i < frames; ++i) acc += double(buf[i * size_t(channels) + size_t(ch)]) * buf[i * size_t(channels) + size_t(ch)];
            return std::sqrt(acc / double(frames));
        };
        const double toneRms = 0.3 / std::sqrt(2.0);
        QVERIFY(rms(six, 0, 6) < 1e-4 && rms(six, 1, 6) < 1e-4);                     // nothing in front left and right
        QVERIFY(std::fabs(rms(six, 2, 6) - toneRms) < 0.01);                         // 440 Hz in the centre
        QVERIFY(std::fabs(rms(six, 3, 6) - toneRms * std::pow(10.0, -6.0 / 20)) < 0.01);  // and the LFE at -6 dB
        QVERIFY(rms(six, 4, 6) > 3 * rms(six, 5, 6));                                // 1 kHz mostly in Ls
        QVERIFY(std::fabs(std::hypot(rms(six, 4, 6), rms(six, 5, 6)) - toneRms) < 0.01);  // constant power
        // Listening in stereo gives the fold-down of exactly that.
        AudioMixer listen;
        std::vector<float> two(size_t(n) * 2), fold(size_t(n) * 2);
        listen.mix(p, s, rate / 2, n, two.data());
        downmixToStereo("5.1", six.data(), n, fold.data());
        for (size_t i = 0; i < two.size(); i += 97) QVERIFY(std::fabs(two[i] - fold[i]) < 1e-6);

        // BS.1770 weights the surrounds by 1.41 (+1.5 dB) and leaves the LFE out.
        const auto& sp = layoutSpeakers("5.1");
        std::vector<double> w;
        for (const Speaker& k : sp) w.push_back(k.loudnessWeight);
        auto oneChannel = [&](int ch) {
            std::vector<float> buf(size_t(rate) * 3 * 6, 0.0f);
            for (size_t i = 0; i < size_t(rate) * 3; ++i) buf[i * 6 + size_t(ch)] = float(0.1 * std::sin(2 * M_PI * 1000 * double(i) / rate));
            return measureLoudness(buf.data(), rate * 3, 6, w.data(), rate);
        };
        QVERIFY(std::fabs(oneChannel(4).integrated - oneChannel(0).integrated - 10 * std::log10(1.41)) < 0.05);
        QVERIFY(!oneChannel(3).valid);

        // Exported: six channels (WAV and AAC), or two when folded down.
        ExportSettings st = findExportPreset("Audio - WAV 24-bit")->settings;
        st.path = path("mix51.wav");
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        MediaItem out;
        QVERIFY(probeMedia(st.path, out));
        QCOMPARE(out.channels, 6);
        ExportSettings aac = st;
        aac.path = path("mix51.m4a");
        aac.audioCodec = "aac";
        QVERIFY2(exportSequence(p, s, aac, nullptr, nullptr, &err), err.c_str());
        QVERIFY(probeMedia(aac.path, out));
        QCOMPARE(out.channels, 6);
        ExportSettings down = st;
        down.path = path("mix51-stereo.wav");
        down.downmixStereo = true;
        QVERIFY2(exportSequence(p, s, down, nullptr, nullptr, &err), err.c_str());
        QVERIFY(probeMedia(down.path, out));
        QCOMPARE(out.channels, 2);
        // Loudness-normalised in 5.1: measured with the surround weights, within 0.3 LU.
        ExportSettings norm = st;
        norm.path = path("mix51-norm.wav");
        norm.loudnessTarget = -24;
        QVERIFY2(exportSequence(p, s, norm, nullptr, nullptr, &err), err.c_str());
        {
            // Read the 24-bit WAV directly (decoding would fold it down).
            QFile f(QString::fromStdString(norm.path));
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray data = f.readAll();
            const int at = data.indexOf("data");
            QVERIFY(at > 0);
            const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData()) + at + 8;
            const size_t samples = size_t(data.size() - at - 8) / 3;
            std::vector<float> pcm(samples);
            for (size_t i = 0; i < samples; ++i) {
                int32_t v = int32_t(bytes[i * 3]) | int32_t(bytes[i * 3 + 1]) << 8 | int32_t(bytes[i * 3 + 2]) << 16;
                if (v & 0x800000) v |= ~0xFFFFFF;
                pcm[i] = float(v) / 8388608.0f;
            }
            const LoudnessResult r = measureLoudness(pcm.data(), int64_t(samples / 6), 6, w.data(), rate);
            QVERIFY2(std::fabs(r.integrated + 24) < 0.3, qPrintable(QString::number(r.integrated)));
        }

        // Stems, from a stereo version: one per track, and they add up to the mix.
        s.audioLayout = "stereo";
        s.audioTracks[0].name = "Dialogue";
        s.audioTracks[1].name = "Music";
        st.path = path("show.wav");
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        std::vector<StemFile> stems;
        QVERIFY2(exportStems(p, s, st, StemsByTrack, &stems, {}, nullptr, &err), err.c_str());
        QCOMPARE(stems.size(), size_t(2));
        QCOMPARE(QString::fromStdString(std::filesystem::path(stems[0].path).filename().string()), QString("show - Dialogue.wav"));
        const AudioBufferPtr whole = decodeAudio(st.path, rate, &err), a = decodeAudio(stems[0].path, rate, &err),
                             b = decodeAudio(stems[1].path, rate, &err);
        QVERIFY(whole && a && b);
        QCOMPARE(a->frames(), whole->frames());
        double worst = 0;
        for (size_t i = 0; i < whole->samples.size(); i += 13) worst = std::max(worst, double(std::fabs(a->samples[i] + b->samples[i] - whole->samples[i])));
        QVERIFY2(worst < 1e-4, qPrintable(QString::number(worst)));
        // By bus: the music routed to a bus; the dialogue goes straight to the master ("Main").
        Bus bus;
        bus.id = p.newId();
        bus.name = "Music Bus";
        s.buses.push_back(bus);
        s.audioTracks[1].output = bus.id;
        stems.clear();
        QVERIFY2(exportStems(p, s, st, StemsByBus, &stems, {}, nullptr, &err), err.c_str());
        QCOMPARE(stems.size(), size_t(2));
        QCOMPARE(stems[0].name, std::string("Main"));
        QCOMPARE(stems[1].name, std::string("Music Bus"));

        // By role: A1 tagged Dialogue, A2 untagged ("No Role"); they add up to the mix too.
        s.audioTracks[1].output = 0;
        s.buses.clear();
        s.audioTracks[0].clips[0].role = "Dialogue";
        stems.clear();
        QVERIFY2(exportStems(p, s, st, StemsByRole, &stems, {}, nullptr, &err), err.c_str());
        QCOMPARE(stems.size(), size_t(2));
        QCOMPARE(stems[0].name, std::string("Dialogue"));
        QCOMPARE(stems[1].name, std::string("No Role"));
        QCOMPARE(QString::fromStdString(std::filesystem::path(stems[0].path).filename().string()), QString("show - Dialogue.wav"));
        {
            const AudioBufferPtr d = decodeAudio(stems[0].path, rate, &err), rest = decodeAudio(stems[1].path, rate, &err);
            QVERIFY(d && rest);
            double sum = 0, alone = 0;
            for (size_t i = 0; i < whole->samples.size(); i += 13) {
                sum = std::max(sum, double(std::fabs(d->samples[i] + rest->samples[i] - whole->samples[i])));
                alone = std::max(alone, double(std::fabs(d->samples[i] - a->samples[i])));  // the dialogue stem is A1's sound
            }
            QVERIFY2(sum < 1e-4 && alone < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(sum).arg(alone)));
        }
        // A muted role is not heard and gets no stem; with every role muted, nothing plays.
        s.audioTracks[1].clips[0].role = "Music";
        edit::setRoleMuted(s, "Music", true);
        stems.clear();
        QVERIFY2(exportStems(p, s, st, StemsByRole, &stems, {}, nullptr, &err), err.c_str());
        QCOMPARE(stems.size(), size_t(1));
        edit::setRoleMuted(s, "Dialogue", true);
        {
            AudioMixer quiet;
            std::vector<float> buf(size_t(rate / 4) * 2);
            quiet.mix(p, s, rate / 2, rate / 4, buf.data());
            QVERIFY(std::all_of(buf.begin(), buf.end(), [](float v) { return v == 0.0f; }));
        }
        QVERIFY(!exportStems(p, s, st, StemsByRole, &stems, {}, nullptr, &err));
        s.mutedRoles.clear();
        s.audioTracks[0].clips[0].role.clear();
        s.audioTracks[1].clips[0].role.clear();

        // Through MCP: 5.1, dialogue in the centre, the music round the back; a stereo fold-down with track stems.
        const QString project = QString::fromStdString(path("surround.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_set_surround",
                             {{"project", project}, {"layout", "5.1"},
                              {"tracks", QJsonArray{QJsonObject{{"track", "A1"}, {"width", 0}},
                                                    QJsonObject{{"track", "A2"}, {"angle", 180}, {"lfe_db", -12}}}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("channels").toInt(), 6);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->audioLayout, std::string("5.1"));
        QCOMPARE(back.active()->audioTracks[0].surround.width, 0.0);
        QVERIFY(back.active()->audioTracks[1].surround.y < -0.99 && std::fabs(back.active()->audioTracks[1].surround.lfeDb + 12) < 1e-9);
        r = call("montage_set_surround", {{"project", project}, {"layout", "quad"}});
        QVERIFY(r.value("isError").toBool());
        const QString wav = QString::fromStdString(path("mcp-mix.wav"));
        r = call("montage_render", {{"project", project}, {"output", wav}, {"preset", "Audio - WAV 24-bit"},
                                    {"downmix_stereo", true}, {"stems", "tracks"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(probeMedia(wav.toStdString(), out));
        QCOMPARE(out.channels, 2);
        const QJsonArray stemList = r.value("structuredContent").toObject().value("stems").toArray();
        QCOMPARE(stemList.size(), 2);
        QVERIFY(probeMedia(stemList[1].toObject().value("path").toString().toStdString(), out));
        QCOMPARE(out.channels, 2);  // the stems follow the fold-down
        // Roles through MCP: A1 tagged, then the untagged A2 heard and tagged; a role muted; stems by role.
        const double a1 = double(back.active()->audioTracks[0].clips[0].id);
        r = call("montage_set_roles", {{"project", project}, {"clips", QJsonArray{a1}}, {"role", "Dialogue"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = call("montage_set_roles", {{"project", project}, {"detect", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->audioTracks[0].clips[0].role, std::string("Dialogue"));  // kept
        const std::string heardAs = back.active()->audioTracks[1].clips[0].role;
        QVERIFY2(heardAs == "Music" || heardAs == "Effects", heardAs.c_str());  // a steady tone is not speech
        r = call("montage_set_roles", {{"project", project}, {"mute", QJsonArray{QString::fromStdString(heardAs)}}});
        QVERIFY(r.value("content").toArray().at(0).toObject().value("text").toString().contains("1 role(s) muted"));
        r = call("montage_render", {{"project", project}, {"output", wav}, {"preset", "Audio - WAV 24-bit"},
                                    {"downmix_stereo", true}, {"stems", "roles"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("stems").toArray().size(), 1);
        r = call("montage_project_info", {{"project", project}});
        QCOMPARE(r.value("structuredContent").toObject().value("muted_roles").toArray().size(), 1);
        r = call("montage_set_roles", {{"project", project}, {"clips", QJsonArray{a1}}});
        QVERIFY(r.value("isError").toBool());
        r = call("montage_render", {{"project", project}, {"output", wav}, {"stems", "sideways"}});
        QVERIFY(r.value("isError").toBool());
    }

    void lottieAndSvgMedia() {
        if (!vectorSupport()) QSKIP("Built without ThorVG");
        // A 200x100 Lottie: a 20 px red square moving from x 20 to x 180 over 2 s at 30 fps, linearly.
        const std::string lottie = path("box.json");
        {
            std::ofstream f(lottie);
            f << R"({"v":"5.7.4","fr":30,"ip":0,"op":60,"w":200,"h":100,"nm":"box","ddd":0,"assets":[],
              "layers":[{"ddd":0,"ind":1,"ty":4,"nm":"box","sr":1,"ao":0,"ip":0,"op":60,"st":0,"bm":0,
                "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},
                  "p":{"a":1,"k":[{"t":0,"s":[20,50,0],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":60,"s":[180,50,0]}]}},
                "shapes":[{"ty":"gr","nm":"g","it":[
                  {"ty":"rc","nm":"r","d":1,"s":{"a":0,"k":[20,20]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0}},
                  {"ty":"fl","nm":"f","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"r":1},
                  {"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}]}]}]})";
        }
        QVERIFY(isVectorPath(lottie));
        const std::string notLottie = path("settings.json");
        {
            std::ofstream f(notLottie);
            f << R"({"name": "not an animation"})";
        }
        QVERIFY(!isVectorPath(notLottie));
        MediaItem m;
        std::string err;
        QVERIFY2(probeMedia(lottie, m, &err), err.c_str());
        QVERIFY(m.kind == MediaKind::Video && m.hasVideo && !m.hasAudio);
        QCOMPARE(m.width, 200);
        QCOMPARE(m.height, 100);
        QCOMPARE(m.fps.num, 30);
        QCOMPARE(m.fps.den, 1);
        QVERIFY(std::fabs(m.duration - 2.0) < 1e-6);
        // Where the red is, and that the rest is transparent.
        auto redCentre = [](const Frame16& f) {
            double sx = 0, n = 0;
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const uint16_t* p = &f.px[(size_t(y) * size_t(f.width) + size_t(x)) * 4];
                    if (p[3] > 32768 && p[0] > 60000 && p[1] < 5000) sx += x, ++n;
                }
            return n > 0 ? sx / n : -1.0;
        };
        VideoDecoder dec;
        QVERIFY2(dec.open(lottie, &err), err.c_str());
        QVERIFY(!dec.isStill());
        QCOMPARE(dec.displayWidth(), 200);
        Frame16Ptr f0 = dec.frameAt(0.0), f1 = dec.frameAt(1.0);
        QVERIFY(f0 && f1);
        QVERIFY2(std::fabs(redCentre(*f0) - 20) < 1.5, qPrintable(QString::number(redCentre(*f0))));
        QVERIFY2(std::fabs(redCentre(*f1) - 100) < 1.5, qPrintable(QString::number(redCentre(*f1))));
        QCOMPARE(int(f1->px[(size_t(5) * 200 + 5) * 4 + 3]), 0);
        // Drawn at the size it is shown: twice as big, the square is still sharp and in the same place.
        Frame16Ptr big = dec.frameAt(1.0, 400, 200);
        QVERIFY(big && big->width == 400);
        QVERIFY2(std::fabs(redCentre(*big) - 200) < 2, qPrintable(QString::number(redCentre(*big))));
        int soft = 0;
        for (int x = 0; x < 400; ++x) {
            const uint16_t a = big->px[(size_t(100) * 400 + size_t(x)) * 4 + 3];
            if (a > 2000 && a < 63000) ++soft;
        }
        QVERIFY2(soft <= 2, qPrintable(QString::number(soft)));  // an edge each side, at most a pixel wide
        // Past the end it holds the last frame.
        Frame16Ptr end = dec.frameAt(5.0);
        QVERIFY(end && redCentre(*end) > 175);

        // An SVG: a still, sharp at ten times its size.
        const std::string svg = path("dot.svg");
        {
            std::ofstream f(svg);
            f << R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32" viewBox="0 0 64 32">)"
                 R"(<circle cx="16" cy="16" r="10" fill="#0000ff"/></svg>)";
        }
        MediaItem sm;
        QVERIFY2(probeMedia(svg, sm, &err), err.c_str());
        QVERIFY(sm.kind == MediaKind::Image);
        QCOMPARE(sm.width, 64);
        QCOMPARE(sm.height, 32);
        VideoDecoder sd;
        QVERIFY(sd.open(svg, &err) && sd.isStill());
        Frame16Ptr dot = sd.frameAt(0, 640, 320);
        QVERIFY(dot);
        const uint16_t* c = &dot->px[(size_t(160) * 640 + 160) * 4];
        QVERIFY(c[2] > 65000 && c[0] < 500 && c[3] > 65000);
        QCOMPARE(int(dot->px[(size_t(300) * 640 + 600) * 4 + 3]), 0);
        int edge = 0;
        for (int x = 0; x < 640; ++x) {
            const uint16_t a = dot->px[(size_t(160) * 640 + size_t(x)) * 4 + 3];
            if (a > 2000 && a < 63000) ++edge;
        }
        QVERIFY2(edge <= 4, qPrintable(QString::number(edge)));

        // On the timeline: two seconds long, over a background, at 30 fps.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 200;
        s.height = 100;
        s.fps = {30, 1};
        Clip bg = makeGeneratorClip(p, "color", 60);
        bg.generator.params["color.r"] = Param(0.0);
        bg.generator.params["color.g"] = Param(0.5);
        bg.generator.params["color.b"] = Param(0.0);
        edit::overwrite(p, s, {TrackKind::Video, 0}, bg);
        MediaItem pm = probeOrFail(p, lottie);
        p.media.push_back(pm);
        QVERIFY(edit::placeMedia(p, s, pm.id, 0, 0, -1, {TrackKind::Video, 1}, {TrackKind::Audio, 0}, false).ok);
        QCOMPARE(s.videoTracks[1].clips.at(0).duration, FrameTime(60));
        const Image frame = renderSequenceFrame(p, s, 30, {});
        const float* red = frame.at(100, 50);
        const float* green = frame.at(30, 20);
        QVERIFY(red[0] > 0.95f && red[1] < 0.05f);
        QVERIFY(green[0] < 0.05f && std::fabs(green[1] - 0.5f) < 0.02f);

        // Through MCP: the Lottie placed like any media, and a star drawn on over a second.
        const QString project = QString::fromStdString(path("graphics.montage"));
        QVERIFY(saveProject(makeDefaultProject(), project.toStdString()));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_place_media", {{"project", project}, {"media", QString::fromStdString(lottie)}, {"at", 0}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = call("montage_add_shape", {{"project", project}, {"shape", "star"}, {"at", 0}, {"duration", 2}, {"width", 300},
                                       {"height", 300}, {"fill", "none"}, {"stroke_width", 6}, {"stroke_color", "#ff8800"},
                                       {"points", 6}, {"draw_on", 1.0}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(call("montage_add_shape", {{"project", project}, {"at", 0}, {"fill", "not a colour"}}).value("isError").toBool());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Sequence& bs = *back.active();
        QVERIFY(bs.videoTracks.size() >= 2);
        const Clip& placed = bs.videoTracks[0].clips.at(0);
        QCOMPARE(placed.duration, FrameTime(std::llround(2.0 * bs.fpsValue())));
        const Clip& shape = bs.videoTracks[1].clips.at(0);
        QCOMPARE(shape.generator.type, std::string("shape"));
        QCOMPARE(shape.generator.p("shape", 0), 3.0);
        QCOMPARE(shape.generator.p("points", 0), 6.0);
        QCOMPARE(shape.generator.p("fill", 0), 0.0);
        QCOMPARE(shape.generator.p("trim_end", 0), 0.0);
        QCOMPARE(shape.generator.p("trim_end", FrameTime(std::llround(bs.fpsValue()))), 100.0);
        QVERIFY(std::fabs(shape.generator.p("stroke_color.g", 0) - 0x88 / 255.0) < 1e-6);
    }

    void bleepInTheMix() {
        // 3 s of a 300 Hz tone; "darn" said 1.0-1.5 s in.
        const int rate = 48000;
        std::vector<float> x(size_t(rate) * 3);
        for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.4 * std::sin(2 * M_PI * 300 * double(i) / rate));
        const std::string wav = path("bleep-source.wav");
        QVERIFY(writeMonoWav(wav, x, rate));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m = probeOrFail(p, wav);
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.start = 0.2;
        seg.end = 2.5;
        seg.text = "oh darn it";
        seg.words = {{0.2, 0.6, "oh"}, {1.0, 1.5, "darn"}, {2.0, 2.5, "it"}};
        t->segments = {seg};
        m.transcript = t;
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const auto words = sequenceTranscriptWords(p, s);
        QCOMPARE(words.size(), size_t(3));
        QVERIFY(bleepWords(p, s, {words[1]}).ok);
        // Energy at a frequency over a stretch of the mix (Goertzel).
        auto energy = [&](const std::vector<float>& mix, double from, double to, double hz) {
            const int a = int(from * rate), b = int(to * rate);
            const double w = 2 * M_PI * hz / rate, k = 2 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (int i = a; i < b; ++i) {
                const double s0 = mix[size_t(i) * 2] + k * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            return std::sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / (b - a) * 2;  // the amplitude
        };
        auto mixdown = [&] {
            AudioMixer mixer;
            std::vector<float> out(size_t(rate) * 3 * 2);
            mixer.mix(p, s, 0, rate * 3, out.data());
            return out;
        };
        std::vector<float> mix = mixdown();
        const float mono = std::sqrt(0.5f);  // a mono file decodes to each channel at -3 dB
        QVERIFY(std::fabs(energy(mix, 0.2, 0.8, 300) - 0.4 * mono) < 0.02);   // the source before the word
        QVERIFY(energy(mix, 1.05, 1.45, 300) < 0.01);                          // gone under the bleep
        QVERIFY2(std::fabs(energy(mix, 1.05, 1.45, 1000) - std::pow(10.0, -12.0 / 20)) < 0.02,  // a -12 dB tone
                 qPrintable(QString::number(energy(mix, 1.05, 1.45, 1000))));
        QVERIFY(std::fabs(energy(mix, 1.6, 2.4, 300) - 0.4 * mono) < 0.02);   // back after it
        // Silence instead of a tone.
        edit::clipById(s, s.audioTracks[0].clips[0].id)->effects[0].params["mode"] = Param(1.0);
        mix = mixdown();
        double peak = 0;
        for (int i = int(1.05 * rate); i < int(1.45 * rate); ++i) peak = std::max(peak, double(std::fabs(mix[size_t(i) * 2])));
        QVERIFY(peak < 1e-4);
        // Trimmed from the front, the bleep stays on the word.
        Clip& c = s.audioTracks[0].clips[0];
        c.sourceIn += 10;
        c.duration -= 10;
        mix = mixdown();
        peak = 0;
        for (int i = int(0.65 * rate); i < int(1.05 * rate); ++i) peak = std::max(peak, double(std::fabs(mix[size_t(i) * 2])));
        QVERIFY(peak < 1e-4);  // the word now plays 0.6-1.1 s

        // Through MCP: the profanity in a transcript.
        t->segments[0].words[1].text = "damn";
        p.media[0].transcript = t;
        s.audioTracks[0].clips[0].effects.clear();
        const QString project = QString::fromStdString(path("bleep.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_bleep"}, {"arguments", QJsonObject{{"project", project}, {"profanity", true}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("words").toArray().size(), 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->audioTracks[0].clips[0].effects.at(0).type, std::string("bleep"));
    }

    void aafExportForAudioPost() {
        // Mono speech (JFK) and a stereo tone, at 25 fps.
        const int rate = 48000;
        const std::string tone = path("tone-stereo.wav");
        {
            QFile f(QString::fromStdString(tone));
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int frames = rate * 6;
            QByteArray data;
            QDataStream out(&data, QIODevice::WriteOnly);
            out.setByteOrder(QDataStream::LittleEndian);
            out.writeRawData("RIFF", 4);
            out << quint32(36 + frames * 4);
            out.writeRawData("WAVEfmt ", 8);
            out << quint32(16) << quint16(1) << quint16(2) << quint32(rate) << quint32(rate * 4) << quint16(4) << quint16(16);
            out.writeRawData("data", 4);
            out << quint32(frames * 4);
            for (int i = 0; i < frames; ++i)
                out << qint16(std::lround(8000 * std::sin(2 * M_PI * 440 * i / rate))) << qint16(std::lround(8000 * std::sin(2 * M_PI * 660 * i / rate)));
            f.write(data);
        }
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        s.name = "Reel 1";
        MediaItem speech = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        p.media.push_back(speech);
        MediaItem stereo = probeOrFail(p, tone);
        p.media.push_back(stereo);
        while (s.audioTracks.size() < 2) edit::addTrack(p, s, TrackKind::Audio);
        auto place = [&](int track, const MediaItem& m, FrameTime at, double in, FrameTime len) {
            Clip c;
            c.id = p.newId();
            c.mediaId = m.id;
            c.name = m.name;
            c.start = at;
            c.duration = len;
            c.sourceIn = in;
            c.audio = makeEffect(p, "volume");
            s.audioTracks[size_t(track)].clips.push_back(c);
            return c.id;
        };
        const Id a = place(0, speech, 0, 25, 50), b = place(0, speech, 50, 150, 60);
        const Id t1 = place(1, stereo, 20, 0, 40), t2 = place(1, stereo, 100, 0, 30);
        s.audioTracks[0].name = "Dialogue";
        s.audioTracks[1].name = "Music";
        // A crossfade between the two speech clips, a fade out after the second, its gain keyframed; the tone at -6 dB;
        // the second tone clip at double speed (rendered).
        Transition x;
        x.id = p.newId();
        x.type = "crossfade";
        x.clipA = a;
        x.clipB = b;
        x.duration = 10;
        s.audioTracks[0].transitions.push_back(x);
        Transition out = x;
        out.id = p.newId();
        out.clipA = b;
        out.clipB = 0;
        out.duration = 12;
        out.type = "crossfade_linear";
        s.audioTracks[0].transitions.push_back(out);
        Param gain;
        gain.addKey(0, 0.0);
        gain.addKey(30, -12.0);
        edit::clipById(s, b)->audio.params["gain_db"] = gain;
        edit::clipById(s, t1)->audio.params["gain_db"] = Param(-6.0);
        edit::clipById(s, t2)->speed = 2.0;

        const std::string aaf = path("Reel 1.aaf");
        AafExportResult r;
        std::string err;
        QVERIFY2(exportAaf(p, s, aaf, &r, {}, nullptr, &err), err.c_str());
        QCOMPARE(r.audioTracks, 3);  // Dialogue (mono), Music L and R
        QCOMPARE(r.clips, 6);
        QCOMPARE(r.transitions, 1);
        QCOMPARE(int(r.mediaFiles.size()), 5);  // speech, tone L/R, the rendered clip L/R
        // The media: mono 24-bit WAVs at 48 kHz, the speech as it decodes.
        MediaItem wav;
        QVERIFY(probeMedia(r.mediaFiles[0], wav));
        QCOMPARE(wav.channels, 1);
        QCOMPARE(wav.sampleRate, rate);
        QVERIFY(QFileInfo(QString::fromStdString(r.mediaFiles[0])).absolutePath().endsWith("Reel 1 Media"));

        // The file, read back: a compound file holding the AAF object model.
        CfbEntry root;
        QVERIFY2(readCompoundFile(aaf, root, &err), err.c_str());
        QVERIFY(root.find("MetaDictionary-1") && root.find("Header-2") && root.find("properties") && root.find("referenced properties"));
        QVERIFY(root.at("MetaDictionary-1")->children.size() > 100);
        const CfbEntry* content = root.at("Header-2/Content-3b03");
        QVERIFY(content);
        auto props = [](const CfbEntry& obj) {
            std::map<uint16_t, std::string> out;
            const CfbEntry* ps = obj.find("properties");
            if (!ps || ps->data.size() < 4) return out;
            const std::string& d = ps->data;
            const int n = uint8_t(d[2]) | uint8_t(d[3]) << 8;
            size_t at = 4 + size_t(n) * 6;
            for (int i = 0; i < n; ++i) {
                const size_t h = 4 + size_t(i) * 6;
                const uint16_t pid = uint16_t(uint8_t(d[h]) | uint8_t(d[h + 1]) << 8);
                const size_t len = uint8_t(d[h + 4]) | size_t(uint8_t(d[h + 5])) << 8;
                out[pid] = d.substr(at, len);
                at += len;
            }
            return out;
        };
        auto i64 = [](const std::string& b) {
            int64_t v = 0;
            for (int k = 7; k >= 0; --k) v = v << 8 | uint8_t(b[size_t(k)]);
            return v;
        };
        int masters = 0, files = 0, comps = 0;
        const CfbEntry* comp = nullptr;
        const std::string compClass = aafAuid("0d010101-0101-3500-060e-2b3402060101");
        for (const CfbEntry& m : content->children) {
            if (!m.storage) continue;
            const std::string cls(reinterpret_cast<const char*>(m.clsid.data()), 16);
            if (cls == compClass) ++comps, comp = &m;
            else if (cls == aafAuid("0d010101-0101-3600-060e-2b3402060101")) ++masters;
            else if (cls == aafAuid("0d010101-0101-3700-060e-2b3402060101")) ++files;
        }
        QCOMPARE(masters, 3);
        QCOMPARE(files, 5);
        QCOMPARE(comps, 1);
        // The dialogue track: the first clip runs 5 frames on into the crossfade, which overlaps 10 frames; the
        // second starts 5 frames early, fades out over its last 12 and has its gain keyframes.
        const CfbEntry* dialogue = comp->at("Slots-4403{1}/Segment-4803");
        QVERIFY(dialogue);
        QCOMPARE(i64(props(*dialogue)[0x0202]), int64_t(110));
        const CfbEntry* c0 = dialogue->find("Components-1001{0}");
        const CfbEntry* c1 = dialogue->find("Components-1001{1}");
        const CfbEntry* c2 = dialogue->find("Components-1001{2}");
        QVERIFY(c0 && c1 && c2 && !dialogue->find("Components-1001{3}"));
        QCOMPARE(i64(props(*c0)[0x0202]), int64_t(55));
        QCOMPARE(i64(props(*c0)[0x1201]), int64_t(25));
        QCOMPARE(i64(props(*c1)[0x0202]), int64_t(10));
        QCOMPARE(i64(props(*c1)[0x1802]), int64_t(5));
        const CfbEntry* inner = c2->find("InputSegments-b02{0}");
        QVERIFY(inner);
        QCOMPARE(i64(props(*inner)[0x1201]), int64_t(145));
        QCOMPARE(i64(props(*inner)[0x0202]), int64_t(65));
        QCOMPARE(i64(props(*inner)[0x1204]), int64_t(12));
        QVERIFY(c2->at("Parameters-b03{0}/PointList-4e02{1}"));

        // Through MCP.
        {
            const QString project = QString::fromStdString(path("reel.montage"));
            QVERIFY(saveProject(p, project.toStdString()));
            McpServer server;
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_export_timeline"},
                                                         {"arguments", QJsonObject{{"project", project}, {"format", "aaf"},
                                                                                   {"output", QString::fromStdString(path("mcp.aaf"))}}},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            const QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
            QVERIFY2(!res.value("isError").toBool(), QJsonDocument(res).toJson().constData());
            QCOMPARE(res.value("structuredContent").toObject().value("crossfades").toInt(), 1);
            QVERIFY(QFileInfo::exists(QString::fromStdString(path("mcp Media/jfk.wav"))));
        }

        // Checkerboarding through MCP: the speech labelled with two speakers, split onto two tracks.
        {
            Project cb = makeDefaultProject();
            Sequence& cs = *cb.active();
            cs.fps = {25, 1};
            cs.audioTracks.resize(1);
            MediaItem sp = probeOrFail(cb, MONTAGE_TEST_DATA_DIR "/jfk.wav");
            auto tr = std::make_shared<Transcript>();
            TranscriptSegment s1, s2;
            s1.start = 0.3, s1.end = 3.0, s1.speaker = 0;
            s2.start = 3.6, s2.end = 7.0, s2.speaker = 1;
            tr->segments = {s1, s2};
            sp.transcript = tr;
            cb.media.push_back(sp);
            QVERIFY(edit::placeMedia(cb, cs, sp.id, 0, 0, 200, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
            const QString project = QString::fromStdString(path("checker.montage"));
            QVERIFY(saveProject(cb, project.toStdString()));
            McpServer server;
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_checkerboard"},
                                                         {"arguments", QJsonObject{{"project", project}, {"track", "A1"}}},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            const QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
            QVERIFY2(!res.value("isError").toBool(), QJsonDocument(res).toJson().constData());
            QCOMPARE(res.value("structuredContent").toObject().value("people").toInt(), 2);
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            QCOMPARE(back.active()->audioTracks.size(), size_t(2));
            QCOMPARE(back.active()->audioTracks[1].name, std::string("Speaker 2"));
        }

        // An independent reader (pyaaf2), when there is one: the same structure, through to the WAV files.
        const QByteArray python = qgetenv("MONTAGE_TEST_PYAAF2");
        if (python.isEmpty()) return;
        QProcess py;
        py.start(QString::fromLocal8Bit(python), {"-I", QStringLiteral(MONTAGE_TEST_TOOLS_DIR "/aaf_check.py"), QString::fromStdString(aaf)});
        QVERIFY(py.waitForFinished(60000));
        QVERIFY2(py.exitCode() == 0, py.readAllStandardError().constData());
        const QJsonObject j = QJsonDocument::fromJson(py.readAllStandardOutput()).object();
        QCOMPARE(j.value("compositions").toInt(), 1);
        QCOMPARE(j.value("mobs").toInt(), 9);
        const QJsonArray tr = j.value("tracks").toArray();
        QCOMPARE(tr.size(), 3);
        QCOMPARE(tr[0].toObject().value("name").toString(), QString("Dialogue"));
        QCOMPARE(tr[1].toObject().value("name").toString(), QString("Music L"));
        const QJsonArray dc = tr[0].toObject().value("components").toArray();
        QCOMPARE(dc[0].toObject().value("start").toInt(), 25);
        QCOMPARE(dc[1].toObject().value("type").toString(), QString("transition"));
        const QJsonObject g = dc[2].toObject();
        QCOMPARE(g.value("type").toString(), QString("gain"));
        const QJsonArray pts = g.value("params").toObject().value("points").toArray();
        QCOMPARE(pts.size(), 2);
        QVERIFY(std::fabs(pts[1].toArray()[1].toDouble() - std::pow(10.0, -12.0 / 20)) < 1e-4);
        QVERIFY(std::fabs(pts[1].toArray()[0].toDouble() - 35.0 / 65) < 1e-6);  // 30 frames in, plus the 5 it starts early
        const QJsonObject clip = g.value("inputs").toArray()[0].toObject();
        QVERIFY(clip.value("file").toString().endsWith("jfk.wav"));
        QCOMPARE(clip.value("fade_out").toInt(), 12);
        const QJsonArray music = tr[1].toObject().value("components").toArray();
        QCOMPARE(music[0].toObject().value("type").toString(), QString("filler"));
        QVERIFY(std::fabs(music[1].toObject().value("params").toObject().value("constant").toDouble() - std::pow(10.0, -6.0 / 20)) < 1e-4);
        QVERIFY(music[1].toObject().value("inputs").toArray()[0].toObject().value("file").toString().endsWith("tone-stereo%20L.wav"));
        QVERIFY(tr[2].toObject().value("components").toArray()[1].toObject().value("inputs").toArray()[0].toObject()
                    .value("file").toString().endsWith("tone-stereo%20R.wav"));
    }

    void superScaleUpscaling() {
        if (!upscalerAvailable()) QSKIP("Built without ONNX Runtime");
        if (!upscaleModel().installed()) QSKIP("Set MONTAGE_UPSCALE_MODEL to the Super Scale model");
        // A picture with lettering, lines, circles and a soft gradient, shrunk to a quarter and enlarged back.
        const int W = 256, H = 192;
        QImage qi(W, H, QImage::Format_RGBA8888_Premultiplied);
        {
            QLinearGradient bg(0, 0, W, H);
            bg.setColorAt(0, QColor(235, 240, 250));
            bg.setColorAt(1, QColor(250, 230, 210));
            QPainter pa(&qi);
            pa.setRenderHint(QPainter::Antialiasing);
            pa.fillRect(qi.rect(), bg);
            QFont font("Sans Serif");
            font.setPixelSize(44);
            font.setBold(true);
            pa.setFont(font);
            pa.setPen(QColor(20, 20, 30));
            pa.drawText(QRect(0, 10, W, 60), Qt::AlignCenter, "Montage");
            pa.setPen(QPen(QColor(200, 40, 40), 5));
            pa.drawEllipse(QPointF(70, 130), 40, 40);
            pa.setPen(QPen(QColor(30, 90, 200), 6));
            for (int i = 0; i < 4; ++i) pa.drawLine(130 + i * 28, 95, 150 + i * 28, 175);
        }
        Image truth(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W * 4; ++x) truth.row(y)[x] = qi.constScanLine(y)[x] / 255.0f;
        const Image small = resizeImage(truth, W / 4, H / 4);
        QCOMPARE(small.width, 64);
        const Image plain = resizeImage(small, W, H);
        Image ai;
        std::string err;
        QVERIFY2(superScale(small, W, H, ai, 1.0, &err), err.c_str());
        QCOMPARE(ai.width, W);
        auto psnr = [&](const Image& a) {
            double acc = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    for (int c = 0; c < 3; ++c) acc += std::pow(double(a.at(x, y)[c]) - truth.at(x, y)[c], 2);
            return 10 * std::log10(1.0 / (acc / (double(W) * H * 3)));
        };
        // Edges: the sum of squared differences between neighbours (blur lowers it).
        auto edges = [&](const Image& a) {
            double acc = 0;
            for (int y = 0; y < H - 1; ++y)
                for (int x = 0; x < W - 1; ++x)
                    for (int c = 0; c < 3; ++c)
                        acc += std::pow(double(a.at(x + 1, y)[c]) - a.at(x, y)[c], 2) + std::pow(double(a.at(x, y + 1)[c]) - a.at(x, y)[c], 2);
            return acc;
        };
        const double pPlain = psnr(plain), pAi = psnr(ai);
        QVERIFY2(pAi > pPlain + 1.0, qPrintable(QString("plain %1 dB, Super Scale %2 dB").arg(pPlain).arg(pAi)));
        QVERIFY2(edges(ai) > 1.3 * edges(plain) && edges(ai) < 1.2 * edges(truth),
                 qPrintable(QString("%1 %2 %3").arg(edges(plain)).arg(edges(ai)).arg(edges(truth))));
        // Half strength is half way between plain scaling and the model.
        Image half;
        QVERIFY(superScale(small, W, H, half, 0.5));
        QVERIFY(std::fabs(half.at(100, 40)[0] - 0.5f * (plain.at(100, 40)[0] + ai.at(100, 40)[0])) < 1e-5f);
        // Not larger than it is: plain resizing, the model not used.
        Image same;
        QVERIFY(superScale(small, 64, 48, same));
        QCOMPARE(same.px, small.px);

        // Tiles join without seams: a picture wider than a tile matches a single pass over a part of it.
        Image wide(400, 60);
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 400; ++x) {
                float* p = wide.at(x, y);
                p[0] = 0.5f + 0.4f * float(std::sin(x * 0.37) * std::cos(y * 0.23));
                p[1] = (x / 7 + y / 5) % 2 ? 0.8f : 0.2f;
                p[2] = float(x) / 400;
                p[3] = 1;
            }
        Image tiled, part;
        QVERIFY(superScale4x(wide, tiled, &err));
        Image crop(150, 60);
        for (int y = 0; y < 60; ++y) std::copy_n(wide.at(250, y), 150 * 4, crop.at(0, y));
        QVERIFY(superScale4x(crop, part, &err));
        double worst = 0;
        for (int y = 0; y < 240; ++y)
            for (int x = (290 - 250) * 4; x < 600; ++x)  // past the crop's own edge effects, across the tile join at 320
                for (int c = 0; c < 3; ++c) worst = std::max(worst, double(std::fabs(part.at(x, y)[c] - tiled.at(1000 + x, y)[c])));
        QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));

        // As a clip effect: a still shown four times its size goes through the model; without it, plain scaling.
        const QString png = QString::fromStdString(path("small.png"));
        {
            QImage q(64, 48, QImage::Format_RGBA8888);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    const float* p = small.at(x, y);
                    q.setPixelColor(x, y, QColor::fromRgbF(p[0], p[1], p[2], 1));
                }
            QVERIFY(q.save(png));
        }
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = W;
        s.height = H;
        MediaItem m = probeOrFail(p, png.toStdString());
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Image without = renderSequenceFrame(p, s, 5, {});
        s.videoTracks[0].clips[0].effects.push_back(makeEffect(p, "super_scale"));
        const Image with = renderSequenceFrame(p, s, 5, {});
        QVERIFY2(psnr(with) > psnr(without) + 1.0, qPrintable(QString("%1 %2").arg(psnr(without)).arg(psnr(with))));
        QVERIFY2(edges(with) > 1.3 * edges(without), qPrintable(QString("%1 %2").arg(edges(without)).arg(edges(with))));
        // A quarter-size preview does not run it (the clip is not shown larger than it is there).
        RenderOptions quarter;
        quarter.scale = 0.25;
        const Image preview = renderSequenceFrame(p, s, 5, quarter);
        QCOMPARE(preview.width, 64);

        // A copy on disk: the still four times larger, as a PNG.
        const std::string bigPng = path("small (Super Scale 4x).png");
        QVERIFY2(createSuperScaled(png.toStdString(), bigPng, 4, 1.0, {}, nullptr, &err), err.c_str());
        MediaItem bm;
        QVERIFY(probeMedia(bigPng, bm));
        QCOMPARE(bm.width, W);
        QCOMPARE(bm.height, H);
        // A short video, twice as large, its sound kept: through MCP.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 96;
        gs.height = 64;
        gs.fps = {25, 1};
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "bars", 10));
        MediaItem tone = probeOrFail(gen, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        gen.media.push_back(tone);
        QVERIFY(edit::placeMedia(gen, gs, tone.id, 0, 0, 10, {TrackKind::Video, 1}, {TrackKind::Audio, 0}, false).ok);
        ExportSettings vs = findExportPreset("H.264 - High Quality")->settings;
        vs.path = path("bars.mp4");
        QVERIFY2(exportSequence(gen, gs, vs, nullptr, nullptr, &err), err.c_str());
        McpServer server;
        const QString out = QString::fromStdString(path("bars-2x.mov"));
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_super_scale"},
                                                     {"arguments", QJsonObject{{"input", QString::fromStdString(vs.path)}, {"output", out}, {"factor", 2}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        MediaItem vm;
        QVERIFY(probeMedia(out.toStdString(), vm));
        QCOMPARE(vm.width, 192);
        QCOMPARE(vm.height, 128);
        QVERIFY(vm.hasAudio);
        QCOMPARE(vm.videoCodec, std::string("prores"));
        QVERIFY(std::fabs(vm.duration - 0.4) < 0.05);
    }

    void interlacedFootage() {
        // A combed picture: a smooth vertical gradient, with a bright box that moved between the fields
        // (at x 10-20 in the top field's lines, x 34-44 in the bottom field's).
        const int W = 64, H = 32;
        auto combed = [&](AVFrame* f, bool interlaced, bool topFirst) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const bool box = (y % 2 == 0) ? (x >= 10 && x < 20) : (x >= 34 && x < 44);
                    f->data[0][y * f->linesize[0] + x] = uint8_t(box && y >= 8 && y < 24 ? 235 : 40 + y * 4);
                }
            for (int p = 1; p < 3; ++p)
                for (int y = 0; y < H / 2; ++y) std::fill_n(f->data[p] + y * f->linesize[p], W / 2, uint8_t(128));
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
            f->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
            if (interlaced) f->flags |= AV_FRAME_FLAG_INTERLACED | (topFirst ? AV_FRAME_FLAG_TOP_FIELD_FIRST : 0);
#else
            f->interlaced_frame = interlaced, f->top_field_first = topFirst;
#endif
        };
        auto frame = [&] {
            AVFrame* f = av_frame_alloc();
            f->format = AV_PIX_FMT_YUV420P, f->width = W, f->height = H;
            av_frame_get_buffer(f, 0);
            return f;
        };
        // How much the box area's lines zig-zag against their neighbours.
        auto zigzag = [&](const uint8_t* y0, int stride) {
            double sum = 0;
            for (int y = 9; y < 23; ++y)
                for (int x = 8; x < 46; ++x)
                    sum += std::abs(int(y0[y * stride + x]) - (int(y0[(y - 1) * stride + x]) + int(y0[(y + 1) * stride + x])) / 2);
            return sum / (14 * 38);
        };
        AVFrame* f = frame();
        combed(f, true, true);
        const double before = zigzag(f->data[0], f->linesize[0]);
        QCOMPARE(fieldDominance(f), 1);
        QVERIFY(deinterlaceFrame(f));
        const double after = zigzag(f->data[0], f->linesize[0]);
        QVERIFY2(after < before * 0.05, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        // The top field is kept: the box stands where its lines showed it; the gradient is untouched.
        QCOMPARE(int(f->data[0][15 * f->linesize[0] + 15]), 235);
        QVERIFY(f->data[0][15 * f->linesize[0] + 38] < 120);
        QCOMPARE(int(f->data[0][3 * f->linesize[0] + 50]), 40 + 3 * 4);
        // Bottom field first keeps the other field; a progressive frame is left alone.
        combed(f, true, false);
        QCOMPARE(fieldDominance(f), 2);
        QVERIFY(deinterlaceFrame(f));
        QCOMPARE(int(f->data[0][15 * f->linesize[0] + 38]), 235);
        QVERIFY(f->data[0][16 * f->linesize[0] + 15] < 120);
        combed(f, false, false);
        QVERIFY(!deinterlaceFrame(f));
        av_frame_free(&f);

        // End to end: an interlaced MPEG-2 file of that picture decodes without the combing.
        const std::string file = path("interlaced.mpg");
        {
            AVFormatContext* oc = nullptr;
            QVERIFY(avformat_alloc_output_context2(&oc, nullptr, "mpeg", file.c_str()) >= 0);
            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
            QVERIFY(codec);
            AVStream* st = avformat_new_stream(oc, nullptr);
            AVCodecContext* c = avcodec_alloc_context3(codec);
            c->width = W * 4, c->height = H * 4;  // larger, so the encoder keeps the detail
            c->time_base = {1, 25}, c->framerate = {25, 1};
            c->pix_fmt = AV_PIX_FMT_YUV420P;
            c->gop_size = 1, c->bit_rate = 8000000;
            c->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
            c->field_order = AV_FIELD_TT;
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            QVERIFY(avcodec_open2(c, codec, nullptr) >= 0);
            avcodec_parameters_from_context(st->codecpar, c);
            st->time_base = c->time_base;
            QVERIFY(avio_open(&oc->pb, file.c_str(), AVIO_FLAG_WRITE) >= 0);
            QVERIFY(avformat_write_header(oc, nullptr) >= 0);
            AVFrame* big = av_frame_alloc();
            big->format = c->pix_fmt, big->width = c->width, big->height = c->height;
            av_frame_get_buffer(big, 0);
            AVPacket* pkt = av_packet_alloc();
            auto drain = [&] {
                while (avcodec_receive_packet(c, pkt) >= 0) {
                    av_packet_rescale_ts(pkt, c->time_base, st->time_base);
                    pkt->stream_index = st->index;
                    av_interleaved_write_frame(oc, pkt);
                }
            };
            for (int i = 0; i < 8; ++i) {
                av_frame_make_writable(big);
                // The combed pattern drawn 4x wider and 4x taller, field lines kept one line apart.
                for (int y = 0; y < big->height; ++y)
                    for (int x = 0; x < big->width; ++x) {
                        const int sy = (y / 8) * 2 + (y % 2);  // pairs of field lines
                        const bool box = (y % 2 == 0) ? (x / 4 >= 10 && x / 4 < 20) : (x / 4 >= 34 && x / 4 < 44);
                        big->data[0][y * big->linesize[0] + x] = uint8_t(box && sy >= 8 && sy < 24 ? 235 : 40 + y);
                    }
                for (int p = 1; p < 3; ++p)
                    for (int y = 0; y < big->height / 2; ++y) std::fill_n(big->data[p] + y * big->linesize[p], big->width / 2, uint8_t(128));
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
                big->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
#else
                big->interlaced_frame = 1, big->top_field_first = 1;
#endif
                big->pts = i;
                QVERIFY(avcodec_send_frame(c, big) >= 0);
                drain();
            }
            avcodec_send_frame(c, nullptr);
            drain();
            av_write_trailer(oc);
            avio_closep(&oc->pb);
            av_packet_free(&pkt);
            av_frame_free(&big);
            avcodec_free_context(&c);
            avformat_free_context(oc);
        }
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(file, &err), err.c_str());
        const Frame16Ptr out = dec.frameAt(0.1);
        QVERIFY(out && out->width == W * 4 && out->height == H * 4);
        // Luma of the decoded picture (the green channel is close enough on grey and white).
        double zz = 0;
        int n = 0;
        for (int y = 40; y < 90; ++y)
            for (int x = 40; x < 176; ++x) {
                auto g = [&](int yy) { return double(out->px[(size_t(yy) * size_t(out->width) + size_t(x)) * 4 + 1]) / 65535.0; };
                zz += std::abs(g(y) - (g(y - 1) + g(y + 1)) / 2), ++n;
            }
        zz /= n;
        QVERIFY2(zz < 0.02, qPrintable(QString("zig-zag left %1").arg(zz)));
    }

    void cameraRawStills() {
        QVERIFY(isRawPath("/a/IMG_0001.CR3") && isRawPath("b.nef") && isRawPath("c.dng") && !isRawPath("d.jpg") && !isRawPath("raw"));
        if (!rawAvailable()) QSKIP("Built without LibRaw");
        // Red on the left, blue on the right, a grey band along the top.
        auto scene = [](int x, int y) -> std::array<double, 3> {
            if (y < 54) return {0.45, 0.45, 0.45};
            return x < 192 ? std::array<double, 3>{0.7, 0.04, 0.04} : std::array<double, 3>{0.04, 0.04, 0.7};
        };
        const std::string dng = path("scene.dng");
        QVERIFY(writeTestDng(dng, 384, 216, scene));
        Project p = makeDefaultProject();
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(dng, m, &err), err.c_str());
        QCOMPARE(int(m.kind), int(MediaKind::Image));
        QCOMPARE(m.width, 384);
        QCOMPARE(m.height, 216);
        QCOMPARE(m.videoCodec, std::string("raw"));
        QVERIFY2(QString::fromStdString(m.metadata["camera"]).contains("Test Camera"), m.metadata["camera"].c_str());
        // Developed: the colours where they were, the grey neutral.
        VideoDecoder dec;
        QVERIFY2(dec.open(dng, &err), err.c_str());
        QVERIFY(dec.isStill());
        const Frame16Ptr f = dec.frameAt(0);
        QVERIFY(f && f->width == 384 && f->height == 216);
        auto px = [&](const Frame16& fr, int x, int y) {
            const uint16_t* q = &fr.px[(size_t(y) * size_t(fr.width) + size_t(x)) * 4];
            return std::array<double, 3>{q[0] / 65535.0, q[1] / 65535.0, q[2] / 65535.0};
        };
        const auto red = px(*f, 96, 140), blue = px(*f, 288, 140), grey = px(*f, 192, 25);
        QVERIFY2(red[0] > 0.5 && red[1] < 0.25 && red[2] < 0.25, qPrintable(QString("%1 %2 %3").arg(red[0]).arg(red[1]).arg(red[2])));
        QVERIFY2(blue[2] > 0.5 && blue[0] < 0.25 && blue[1] < 0.25, qPrintable(QString("%1 %2 %3").arg(blue[0]).arg(blue[1]).arg(blue[2])));
        QVERIFY2(grey[0] > 0.3 && std::abs(grey[0] - grey[1]) < 0.03 && std::abs(grey[2] - grey[1]) < 0.03,
                 qPrintable(QString("%1 %2 %3").arg(grey[0]).arg(grey[1]).arg(grey[2])));
        // Scaled on request, as other stills are.
        const Frame16Ptr small = dec.frameAt(0, 192, 108);
        QVERIFY(small && small->width == 192 && small->height == 108);
        QVERIFY(px(*small, 48, 70)[0] > 0.5);

        // Shot in portrait: turned upright (the stored left half becomes the top).
        const std::string tall = path("portrait.dng");
        QVERIFY(writeTestDng(tall, 384, 216, scene, 6));
        MediaItem pm;
        QVERIFY(probeMedia(tall, pm, &err));
        QCOMPARE(pm.width, 216);
        QCOMPARE(pm.height, 384);
        VideoDecoder up;
        QVERIFY(up.open(tall, &err));
        const Frame16Ptr u = up.frameAt(0);
        QVERIFY(u && u->width == 216 && u->height == 384);
        QVERIFY(px(*u, 80, 96)[0] > 0.5 && px(*u, 80, 288)[2] > 0.5);

        // In a sequence, like any other still.
        p.media.push_back(m);
        Sequence& s = *p.active();
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        RenderOptions o;
        o.scale = 0.25;
        const Image frame = renderProgramFrame(p, s, 10, o);
        const float* l = frame.at(frame.width / 4, frame.height * 2 / 3);
        const float* r = frame.at(frame.width * 3 / 4, frame.height * 2 / 3);
        QVERIFY2(l[0] > 0.5f && l[2] < 0.25f && r[2] > 0.5f && r[0] < 0.25f, qPrintable(QString("%1 %2").arg(l[0]).arg(r[2])));
        // A file that is not raw at all says so.
        const std::string bad = path("broken.cr2");
        {
            FILE* f2 = std::fopen(bad.c_str(), "wb");
            QVERIFY(f2);
            std::fputs("not a raw file", f2);
            std::fclose(f2);
        }
        MediaItem none;
        QVERIFY(!probeMedia(bad, none, &err) && !err.empty());
    }

    void gifAndImageSequences() {
        // Two seconds: colour bars with a white square moving across them.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 640;
        s.height = 360;
        s.fps = {30, 1};
        edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "bars", 60));
        Clip sq = makeGeneratorClip(p, "shape", 60);
        sq.generator.params["width"] = Param(60.0);
        sq.generator.params["height"] = Param(60.0);
        sq.generator.params["fill_color.r"] = sq.generator.params["fill_color.g"] = sq.generator.params["fill_color.b"] = Param(1.0);
        sq.generator.params["pos_x"].addKey(0, -250.0);
        sq.generator.params["pos_x"].addKey(59, 250.0);
        edit::overwrite(p, s, {TrackKind::Video, 1}, sq);
        std::string err;
        // Animated GIF: 480 wide, 15 fps, and close to the picture despite 256 colours.
        ExportSettings gif = findExportPreset("Animated GIF")->settings;
        gif.path = path("export.gif");
        QVERIFY2(exportSequence(p, s, gif, nullptr, nullptr, &err), err.c_str());
        Project probe = makeDefaultProject();
        MediaItem m = probeOrFail(probe, gif.path);
        QCOMPARE(m.width, 480);
        QCOMPARE(m.height, 270);
        QVERIFY2(std::fabs(m.duration - 2.0) < 0.15, qPrintable(QString::number(m.duration)));
        for (double at : {0.5, 1.5}) {
            Frame16Ptr f = MediaPool::instance().videoFrame(gif.path, at + 0.01, 480, 270, true);
            QVERIFY(f);
            const Image got = toImage(*f);
            RenderOptions ro;
            ro.scale = 480.0 / 640;
            const Image want = renderProgramFrame(p, s, FrameTime(std::lround(at * 30)), ro);
            double diff = 0;
            for (int y = 0; y < 270; ++y)
                for (int x = 0; x < 480; ++x)
                    for (int k = 0; k < 3; ++k) diff += std::fabs(got.at(x, y)[k] - want.at(x, std::min(y, want.height - 1))[k]);
            diff /= 480.0 * 270 * 3;
            qInfo("GIF frame at %.1f s: mean difference %.4f", at, diff);
            QVERIFY2(diff < 0.04, qPrintable(QString::number(diff)));
        }
        // A PNG sequence: one numbered file a frame, the first matching the picture exactly (8-bit).
        ExportSettings png = findExportPreset("PNG Sequence")->settings;
        png.path = path("frames/shot.png");
        png.in = 10;
        png.out = 15;
        QDir().mkpath(QString::fromStdString(path("frames")));
        QVERIFY2(exportSequence(p, s, png, nullptr, nullptr, &err), err.c_str());
        for (int i = 0; i < 5; ++i) QVERIFY2(QFileInfo::exists(QString::fromStdString(path("frames")) + QString("/shot_%1.png").arg(i, 6, 10, QChar('0'))), qPrintable(QString::number(i)));
        QVERIFY(!QFileInfo::exists(QString::fromStdString(path("frames/shot_000005.png"))));
        QImage first(QString::fromStdString(path("frames/shot_000000.png")));
        QCOMPARE(first.size(), QSize(640, 360));
        const Image frame10 = renderProgramFrame(p, s, 10, {});
        const QRgb px = first.pixel(320, 180);
        QVERIFY(std::abs(qRed(px) - int(std::lround(frame10.at(320, 180)[0] * 255))) <= 2);
        // TIFF: 16 bits a channel. And image sequences refuse sound.
        ExportSettings tiff = findExportPreset("TIFF Sequence (16-bit)")->settings;
        tiff.path = path("frames/grade.tif");
        tiff.in = 0;
        tiff.out = 2;
        QVERIFY2(exportSequence(p, s, tiff, nullptr, nullptr, &err), err.c_str());
        {
            // BitsPerSample (tag 258) from the first directory of the little-endian TIFF.
            QFile f(QString::fromStdString(path("frames/grade_000001.tif")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray b = f.readAll();
            auto u16 = [&](int at) { return int(uint8_t(b[at])) | int(uint8_t(b[at + 1])) << 8; };
            auto u32 = [&](int at) { return u16(at) | u16(at + 2) << 16; };
            QVERIFY(b.startsWith("II*"));
            const int ifd = u32(4), n = u16(ifd);
            int bits = 0;
            for (int i = 0; i < n; ++i) {
                const int e = ifd + 2 + i * 12;
                if (u16(e) == 258) bits = u32(e + 4) > 2 ? u16(u32(e + 8)) : u16(e + 8);
            }
            QCOMPARE(bits, 16);
        }
        tiff.audioCodec = "aac";
        QVERIFY(!exportSequence(p, s, tiff, nullptr, nullptr, &err));
        // The social presets normalise to -14 LUFS.
        QCOMPARE(findExportPreset("Social - TikTok / Reels / Shorts")->settings.loudnessTarget, -14.0);
    }

    void masteringCodecs() {
        // A short sequence: colour bars, a title on transparency above, the JFK clip's sound.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320, s.height = 180, s.fps = {25, 1};
        Clip bars = makeGeneratorClip(p, "bars", 10);
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, bars).ok);
        MediaItem speech = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        p.media.push_back(speech);
        QVERIFY(edit::placeMedia(p, s, speech.id, 0, 0, 10, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Project overlay = p;
        Sequence& os = *overlay.active();
        os.videoTracks[0].clips.clear();
        Clip title = makeGeneratorClip(overlay, "title", 10);
        QVERIFY(edit::overwrite(overlay, os, {TrackKind::Video, 0}, title).ok);
        // What a file holds: video codec, its profile, pixel format, and the audio codec.
        struct Info {
            std::string codec, profile, pix, audio;
        };
        auto info = [](const std::string& file) {
            Info r;
            AVFormatContext* fmt = nullptr;
            if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) return r;
            avformat_find_stream_info(fmt, nullptr);
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                const AVCodecParameters* cp = fmt->streams[i]->codecpar;
                if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                    r.codec = avcodec_get_name(cp->codec_id);
                    if (const char* pr = avcodec_profile_name(cp->codec_id, cp->profile)) r.profile = pr;
                    if (const char* pf = av_get_pix_fmt_name(AVPixelFormat(cp->format))) r.pix = pf;
                } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                    r.audio = avcodec_get_name(cp->codec_id);
                }
            }
            avformat_close_input(&fmt);
            return r;
        };
        RenderOptions ro;
        const Image ref = renderProgramFrame(p, s, 4, ro);
        auto meanDiff = [&](const std::string& file) {
            VideoDecoder dec;
            if (!dec.open(file)) return 1.0;
            const Frame16Ptr f = dec.frameAt(4.0 / 25 + 0.001);
            if (!f) return 1.0;
            const Image got = toImage(*f);
            double d = 0;
            for (int y = 0; y < ref.height; ++y)
                for (int x = 0; x < ref.width; ++x)
                    for (int c = 0; c < 3; ++c) d += std::abs(got.at(x, y)[c] - ref.at(x, y)[c]);
            return d / (double(ref.width) * ref.height * 3);
        };
        struct Want {
            const char* preset;
            const char* codec;
            const char* profile;  // "" = not checked
            const char* pix;
            const char* audio;
            double tolerance;
        };
        const Want wants[] = {
            {"Apple ProRes 422", "prores", "Standard", "yuv422p10le", "pcm_s24le", 0.012},
            {"Apple ProRes 422 Proxy", "prores", "Proxy", "yuv422p10le", "pcm_s16le", 0.03},
            {"Apple ProRes 4444 XQ (alpha)", "prores", "XQ", "yuva444p12le", "pcm_s24le", 0.012},  // decoded as 12-bit
            {"Avid DNxHR SQ", "dnxhd", "DNXHR SQ", "yuv422p", "pcm_s24le", 0.02},
            {"Avid DNxHR LB", "dnxhd", "DNXHR LB", "yuv422p", "pcm_s16le", 0.04},
            {"Avid DNxHR HQX (10-bit)", "dnxhd", "DNXHR HQX", "yuv422p10le", "pcm_s24le", 0.012},
            {"Avid DNxHR 444 (10-bit)", "dnxhd", "DNXHR 444", "yuv444p10le", "pcm_s24le", 0.012},
            {"GoPro CineForm", "cfhd", "", "yuv422p10le", "pcm_s24le", 0.02},
            {"FFV1 (lossless archive)", "ffv1", "", "yuv422p10le", "flac", 0.006},
            {"Uncompressed 10-bit (v210)", "v210", "", "yuv422p10le", "pcm_s24le", 0.006},
        };
        std::string err;
        for (const Want& w : wants) {
            const ExportPreset* pr = findExportPreset(w.preset);
            QVERIFY2(pr, w.preset);
            ExportSettings st = pr->settings;
            st.path = path((std::string("master-") + std::to_string(&w - wants) + "." + pr->extension).c_str());
            QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), qPrintable(QString("%1: %2").arg(w.preset, err.c_str())));
            const Info in = info(st.path);
            QVERIFY2(in.codec == w.codec && in.audio == w.audio && (in.pix == w.pix || (std::string(w.codec) == "v210")),
                     qPrintable(QString("%1: %2 %3 %4").arg(w.preset, in.codec.c_str(), in.pix.c_str(), in.audio.c_str())));
            if (*w.profile) QVERIFY2(in.profile == w.profile, qPrintable(QString("%1: profile %2").arg(w.preset, in.profile.c_str())));
            const double d = meanDiff(st.path);
            QVERIFY2(d < w.tolerance, qPrintable(QString("%1: mean difference %2").arg(w.preset).arg(d)));
        }
        // Transparency survives CineForm RGBA and ProRes 4444 XQ: clear round the title, solid on it.
        for (const char* name : {"GoPro CineForm (alpha)", "Apple ProRes 4444 XQ (alpha)"}) {
            ExportSettings st = findExportPreset(name)->settings;
            st.path = path((std::string("alpha-") + (name[0] == 'G' ? "cfhd" : "xq") + ".mov").c_str());
            st.audioCodec = "none";
            QVERIFY2(exportSequence(overlay, os, st, nullptr, nullptr, &err), err.c_str());
            if (name[0] == 'G') QCOMPARE(QString::fromStdString(info(st.path).pix), QString("gbrap12le"));
            VideoDecoder dec;
            QVERIFY(dec.open(st.path));
            const Image got = toImage(*dec.frameAt(0.16));
            QVERIFY2(got.at(5, 5)[3] < 0.02f, name);
            int solid = 0;
            for (int y = 0; y < got.height; ++y)
                for (int x = 0; x < got.width; ++x) solid += got.at(x, y)[3] > 0.98f;
            QVERIFY2(solid > 50, name);
        }
    }

    void motionBlurAndDeflicker() {
        std::string err;
        auto footage = [&](const std::string& file, const std::function<void(Project&, Sequence&)>& build) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 320;
            gs.height = 180;
            gs.fps = {25, 1};
            build(gen, gs);
            ExportSettings st = findExportPreset("Apple ProRes 422 HQ")->settings;
            st.path = path(file.c_str());
            st.audioCodec = "none";
            if (!exportSequence(gen, gs, st, nullptr, nullptr, &err)) qFatal("%s", err.c_str());
            return st.path;
        };
        auto openIn = [&](Project& p, const std::string& file) -> Clip& {
            Sequence& s = *p.active();
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            MediaItem m = probeOrFail(p, file);
            p.media.push_back(m);
            edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            return s.videoTracks[0].clips.at(0);
        };
        // Motion Blur: a white square moving 16 px a frame smears along its path, not across it.
        const std::string moving = footage("moving-square.mov", [](Project& gen, Sequence& gs) {
            Clip bg = makeGeneratorClip(gen, "color", 20);
            bg.generator.params["color.r"] = bg.generator.params["color.g"] = bg.generator.params["color.b"] = Param(0.0);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, bg);
            Clip sq = makeGeneratorClip(gen, "shape", 20);
            sq.generator.params["width"] = Param(40.0);
            sq.generator.params["height"] = Param(40.0);
            sq.generator.params["fill_color.r"] = sq.generator.params["fill_color.g"] = sq.generator.params["fill_color.b"] = Param(1.0);
            sq.generator.params["pos_x"].addKey(0, -150.0);
            sq.generator.params["pos_x"].addKey(19, 154.0);
            edit::overwrite(gen, gs, {TrackKind::Video, 1}, sq);
        });
        Project p = makeDefaultProject();
        Clip& clip = openIn(p, moving);
        Sequence& s = *p.active();
        auto soft = [](const Image& img, bool across) {
            // Pixels part way between black and white along the middle row (across) or column (along the square's centre).
            int n = 0, cx = 0;
            double best = 0;
            for (int x = 0; x < img.width; ++x)
                if (img.at(x, 90)[1] > best) best = img.at(x, 90)[1], cx = x;
            const int len = across ? img.width : img.height;
            for (int i = 0; i < len; ++i) {
                const float v = across ? img.at(i, 90)[1] : img.at(cx, i)[1];
                n += v > 0.1f && v < 0.9f;
            }
            return n;
        };
        const Image sharp = renderSequenceFrame(p, s, 8, {});
        Effect mb = makeEffect(p, "motion_blur");
        mb.params["shutter"] = 360.0;
        clip.effects.push_back(mb);
        const Image blurred = renderSequenceFrame(p, s, 8, {});
        qInfo("motion blur: %d soft pixels across (from %d), %d along (from %d)", soft(blurred, true), soft(sharp, true), soft(blurred, false),
              soft(sharp, false));
        QVERIFY(soft(sharp, true) <= 4 && soft(blurred, true) >= 16);
        QVERIFY(soft(blurred, false) <= 6);
        clip.effects.back().params["shutter"] = 0.0;
        QVERIFY(renderSequenceFrame(p, s, 8, {}).px == sharp.px);

        // Deflicker: a gradient that flickers a fifth of a stop up and down each frame while slowly brightening.
        const std::string flicker = footage("flicker.mov", [](Project& gen, Sequence& gs) {
            Clip g = makeGeneratorClip(gen, "gradient", 24);
            Effect cc = makeEffect(gen, "color_correct");
            for (int f = 0; f < 24; ++f) cc.params["exposure"].addKey(f, (f % 2 ? -0.2 : 0.2) + 0.02 * f);
            g.effects.push_back(cc);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, g);
        });
        Project q = makeDefaultProject();
        Clip& fc = openIn(q, flicker);
        Sequence& qs = *q.active();
        auto means = [&] {
            std::vector<double> v;
            for (FrameTime t = 5; t < 17; ++t) {
                const Image img = renderSequenceFrame(q, qs, t, {});
                double acc = 0;
                for (size_t i = 0; i < img.px.size(); i += 4) acc += img.px[i + 1];
                v.push_back(acc / double(img.px.size() / 4));
            }
            return v;
        };
        // How much it jumps about: the second difference (a steady ramp has none).
        auto jitter = [](const std::vector<double>& v) {
            double acc = 0;
            for (size_t i = 1; i + 1 < v.size(); ++i) acc += std::fabs(v[i + 1] - 2 * v[i] + v[i - 1]);
            return acc / double(v.size() - 2);
        };
        const std::vector<double> before = means();
        fc.effects.push_back(makeEffect(q, "deflicker"));
        const std::vector<double> after = means();
        qInfo("deflicker: jitter %.5f from %.5f; %.4f to %.4f over the run (from %.4f to %.4f)", jitter(after), jitter(before), after.front(),
              after.back(), before.front(), before.back());
        QVERIFY2(jitter(after) < jitter(before) / 5, qPrintable(QString("%1 %2").arg(jitter(after)).arg(jitter(before))));
        // The slow brightening is kept.
        QVERIFY(after.back() > after.front() + 0.01);
    }

    void videoNoiseReductionOnFootage() {
        // Grainy footage: a flat colour with fresh film grain on every frame, kept by ProRes 422 HQ.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        Clip c = makeGeneratorClip(gen, "color", 20);
        c.generator.params["color.r"] = Param(0.45);
        c.generator.params["color.g"] = Param(0.5);
        c.generator.params["color.b"] = Param(0.4);
        Effect grain = makeEffect(gen, "film_grain");
        grain.params["amount"] = Param(0.35);
        grain.params["size"] = Param(0.5);
        grain.params["color"] = Param(1.0);
        c.effects.push_back(grain);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
        ExportSettings st = findExportPreset("Apple ProRes 422 HQ")->settings;
        st.path = path("grain.mov");
        st.audioCodec = "none";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());

        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem m = probeOrFail(p, st.path);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[0].clips.at(0);
        auto mean = [](const Image& img, int x0, int x1) {
            double acc = 0;
            for (int y = 0; y < img.height; ++y)
                for (int x = x0; x < x1; ++x) acc += img.at(x, y)[1];
            return acc / (double(img.height) * (x1 - x0));
        };
        auto noiseIn = [](const Image& img, int x0, int x1) {
            Image part(x1 - x0, img.height);
            for (int y = 0; y < img.height; ++y) std::copy_n(img.at(x0, y), size_t(x1 - x0) * 4, part.at(0, y));
            return estimateNoise(part);
        };
        const Image raw = renderSequenceFrame(p, s, 10, {});
        const double before = estimateNoise(raw);
        QVERIFY2(before > 0.02, qPrintable(QString::number(before)));
        // Temporal only (two frames either side): the grain falls towards 1/sqrt(5) of itself; the colour stays.
        Effect nr = makeEffect(p, "video_denoise");
        nr.params["luma"] = Param(0.0);
        nr.params["chroma"] = Param(0.0);
        clip.effects.push_back(nr);
        const Image temporal = renderSequenceFrame(p, s, 10, {});
        const double afterTemporal = estimateNoise(temporal);
        QVERIFY2(afterTemporal < 0.6 * before, qPrintable(QString("%1 -> %2").arg(before).arg(afterTemporal)));
        QVERIFY(std::fabs(mean(temporal, 0, 320) - mean(raw, 0, 320)) < 0.01);
        // At the first frame there are only frames after it, and it still works.
        QVERIFY(estimateNoise(renderSequenceFrame(p, s, 0, {})) < 0.75 * before);
        // No frames either side and no spatial pass: nothing changes.
        clip.effects.back().params["frames"] = Param(0.0);
        QVERIFY(std::fabs(estimateNoise(renderSequenceFrame(p, s, 10, {})) - before) < 0.02 * before);
        // The defaults (temporal and spatial) go further.
        clip.effects.back() = makeEffect(p, "video_denoise");
        const double afterBoth = estimateNoise(renderSequenceFrame(p, s, 10, {}));
        QVERIFY2(afterBoth < afterTemporal, qPrintable(QString("%1 %2").arg(afterBoth).arg(afterTemporal)));
        // Under a mask (the left half), only the left half is cleaned.
        Effect& masked = clip.effects.back();
        masked.params["mask.shape"] = Param(2.0);
        masked.params["mask.x"] = Param(0.25);
        masked.params["mask.w"] = Param(0.5);
        masked.params["mask.h"] = Param(2.0);
        masked.params["mask.feather"] = Param(0.0);
        const Image half = renderSequenceFrame(p, s, 10, {});
        QVERIFY2(noiseIn(half, 8, 150) < 0.6 * noiseIn(half, 170, 312),
                 qPrintable(QString("%1 %2").arg(noiseIn(half, 8, 150)).arg(noiseIn(half, 170, 312))));
        QVERIFY(std::fabs(noiseIn(half, 170, 312) - noiseIn(raw, 170, 312)) < 0.05 * noiseIn(raw, 170, 312));
    }

    void loudnessNormalisedExport() {
        // A 220 Hz tone (about -10.6 LUFS) from 1 s to 4 s, normalised to -14 LUFS, then pushed past a -1 dBTP ceiling.
        const std::string tone = path("norm-tone.wav");
        writeVoiceWav(tone, 4, 220, [](double t) { return t >= 1; }, [](double) { return false; });
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, tone);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        ExportSettings st = findExportPreset("Audio - WAV 24-bit")->settings;
        std::string err;
        auto render = [&](double target, const char* name) {
            st.path = path(name);
            st.loudnessTarget = target;
            if (!exportSequence(p, s, st, nullptr, nullptr, &err)) return AudioBufferPtr();
            return decodeAudio(st.path, 48000, &err);
        };
        const AudioBufferPtr plain = render(0, "plain.wav"), normal = render(-14, "normal.wav");
        QVERIFY2(plain && normal, err.c_str());
        const LoudnessResult before = measureLoudness(*plain), after = measureLoudness(*normal);
        QVERIFY2(std::fabs(after.integrated + 14) < 0.3, qPrintable(QString::number(after.integrated)));
        QVERIFY2(before.integrated > -12, qPrintable(QString::number(before.integrated)));
        // Same length, and the sound starts where it did.
        QCOMPARE(normal->frames(), plain->frames());
        auto onset = [](const AudioBuffer& b) {
            for (int64_t i = 0; i < b.frames(); ++i)
                if (std::fabs(b.samples[size_t(i) * 2]) > 0.05f) return i;
            return int64_t(-1);
        };
        QVERIFY(std::llabs(onset(*normal) - onset(*plain)) <= 48);  // 1 ms
        // Asking for more than the ceiling allows: limited, never over -1 dBTP.
        st.peakCeiling = -1;
        const AudioBufferPtr hot = render(-0.5, "hot.wav");
        QVERIFY2(hot, err.c_str());
        const LoudnessResult limited = measureLoudness(*hot);
        QVERIFY2(limited.truePeakDb <= -1.0 + 0.1, qPrintable(QString::number(limited.truePeakDb)));
        QVERIFY(limited.integrated > -6);
    }

    void audioSyncFindsOffset() {
        // Reference: 12 s of irregular noise bursts. Other: the same scene recorded
        // from 1.5 s in, quieter and with background hiss.
        AudioBuffer ref, other;
        ref.sampleRate = other.sampleRate = 48000;
        uint32_t seed = 12345;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return float((seed >> 8) & 0xffff) / 65535.0f * 2 - 1;
        };
        std::vector<float> mono(48000 * 12, 0.0f);
        int pos = 0;
        while (pos < int(mono.size())) {
            int len = 2000 + int((rnd() + 1) * 6000);
            for (int i = 0; i < len && pos + i < int(mono.size()); ++i) mono[size_t(pos + i)] = 0.6f * rnd() * std::exp(-i / 3000.0f);
            pos += len + 4000 + int((rnd() + 1) * 20000);
        }
        for (float v : mono) {
            ref.samples.push_back(v);
            ref.samples.push_back(v);
        }
        for (size_t i = 72000; i < mono.size(); ++i) {
            float v = mono[i] * 0.3f + 0.01f * rnd();
            other.samples.push_back(v);
            other.samples.push_back(v);
        }
        SyncResult r = findAudioOffset(ref, other);
        QVERIFY(r.found);
        QVERIFY2(std::fabs(r.offset - 1.5) < 0.005, qPrintable(QString::number(r.offset)));
        // Unrelated audio does not produce a confident match.
        AudioBuffer unrelated;
        unrelated.sampleRate = 48000;
        for (int i = 0; i < 48000 * 6; ++i) {
            float v = 0.2f * std::sin(i * 0.05f);
            unrelated.samples.push_back(v);
            unrelated.samples.push_back(v);
        }
        QVERIFY(!findAudioOffset(ref, unrelated).found);
    }

    void trackBusAndMasterEffects() {
        std::string wav = path("bus.wav");
        writeWav(wav, 48000, 1.0, 0.5f, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto limiter = [&](double ceilingDb) {
            Effect e = makeEffect(p, "limiter");
            e.params["ceiling_db"] = ceilingDb;
            return e;
        };
        auto level = [&](int64_t at) {
            AudioMixer mixer;  // fresh state each time
            std::vector<float> out(4800 * 2);
            mixer.mix(p, s, at, 4800, out.data());
            return out[4000 * 2];
        };
        QVERIFY(std::fabs(level(12000) - 0.5f) < 0.005f);
        // A track insert (limiter at -12 dB) works on the track's sum.
        Track& a1 = s.audioTracks[0];
        a1.effects.push_back(limiter(-12));
        QVERIFY(std::fabs(level(12000) - 0.2512f) < 0.005f);
        a1.effects.clear();
        // Routed to a bus with its own limiter; a muted bus is silent; a missing bus means master.
        Bus b;
        b.id = p.newId();
        b.name = "Dialogue";
        b.effects.push_back(limiter(-20));
        s.buses.push_back(b);
        a1.output = b.id;
        QVERIFY(std::fabs(level(12000) - 0.1f) < 0.003f);
        s.buses[0].volumeDb = -6.0206;
        QVERIFY(std::fabs(level(12000) - 0.05f) < 0.003f);
        s.buses[0].muted = true;
        QVERIFY(std::fabs(level(12000)) < 1e-6f);
        a1.output = 999999;
        QVERIFY(std::fabs(level(12000) - 0.5f) < 0.005f);
        a1.output = 0;
        // Master effects and fader.
        s.masterEffects.push_back(limiter(-12));
        QVERIFY(std::fabs(level(12000) - 0.2512f) < 0.005f);
        s.masterVolumeDb = -6.0206;
        QVERIFY(std::fabs(level(12000) - 0.1256f) < 0.004f);
        s.masterEffects.clear();
        s.masterVolumeDb = 0;
        // Track inserts keep running after the last clip: an echo rings past the clip's end (1 s).
        Effect echo = makeEffect(p, "delay");
        echo.params["time_ms"] = 300.0;
        echo.params["feedback"] = 0.0;
        echo.params["mix"] = 100.0;
        a1.effects.push_back(echo);
        {
            AudioMixer mixer;
            std::vector<float> out(48000 * 2);
            mixer.mix(p, s, 24000, 48000, out.data());  // 0.5 s .. 1.5 s
            QVERIFY(std::fabs(out[(48000 * 1.1 - 24000) * 2]) > 0.4f);   // 1.1 s: the echo of 0.8 s
            QVERIFY(std::fabs(out[(48000 * 1.4 - 24000) * 2]) < 1e-6f);  // 1.4 s: the echo is over
        }
        // All of it is saved with the project.
        Project q;
        QVERIFY(projectFromJson(projectToJson(p), q));
        QCOMPARE(q.active()->buses, s.buses);
        QCOMPARE(q.active()->audioTracks[0].effects.size(), size_t(1));
        QCOMPARE(q.active()->audioTracks[0], s.audioTracks[0]);
    }

    void renderClipVideoBakesEffects() {
        // A two-second ramp in ProRes (128 x 72), shown in a larger sequence, inverted and at double speed.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 128, gs.height = 72, gs.fps = Rational{25, 1};
        Clip ramp = makeGeneratorClip(gen, "color", 50);
        ramp.generator.params["color.r"].addKey(0, 0.1);
        ramp.generator.params["color.r"].addKey(49, 0.9);
        ramp.generator.params["color.b"] = 0.3;
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, ramp);
        ExportSettings st;
        st.videoCodec = "prores_ks";
        st.audioCodec = "none";
        st.path = path("rr-source.mov");
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 256, s.height = 144, s.fps = Rational{25, 1};
        MediaItem m = probeOrFail(p, st.path);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id id = s.videoTracks[0].clips[0].id;
        QVERIFY(edit::setSpeed(p, s, id, 2.0, true).ok);
        edit::clipById(s, id)->effects.push_back(makeEffect(p, "invert"));
        RenderOptions ro;
        const Image before = renderProgramFrame(p, s, 10, ro);
        // Rendered at the media's size, with its transparency, then swapped in.
        const std::string out = path("rr-video.mov");
        QVERIFY2(renderClipVideo(p, s, id, out, &err), err.c_str());
        Project probe;
        const MediaItem rendered = probeOrFail(probe, out);
        QCOMPARE(rendered.width, 128);
        QCOMPARE(rendered.height, 72);
        QVERIFY(std::fabs(rendered.duration - 1.0) < 0.05);  // 25 frames: the double-speed clip
        MediaItem rm = probeOrFail(p, out);
        p.media.push_back(rm);
        QVERIFY(edit::replaceWithRender(s, id, rm.id).ok);
        const Clip& c = *edit::clipById(s, id);
        QVERIFY(c.effects.empty() && c.speed == 1.0 && !c.ramped());
        const Image after = renderProgramFrame(p, s, 10, ro);
        float worst = 0;
        for (size_t i = 0; i < after.px.size(); ++i) worst = std::max(worst, std::fabs(after.px[i] - before.px[i]));
        QVERIFY2(worst < 0.02f, qPrintable(QString::number(worst)));
        // Restored as it was.
        QVERIFY(edit::restoreUnrendered(s, id).ok);
        QCOMPARE(edit::clipById(s, id)->speed, 2.0);
        QCOMPARE(edit::clipById(s, id)->effects.size(), size_t(1));
        QCOMPARE(edit::clipById(s, id)->mediaId, m.id);
    }

    void renderClipAudioBakesEffects() {
        std::string wav = path("render-src.wav");
        writeWav(wav, 48000, 4.0, 0.5f, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        auto r = edit::placeMedia(p, s, m.id, 30, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        QVERIFY(r.ok);
        Clip* c = nullptr;
        for (Id id : r.created)
            if (auto loc = edit::locate(s, id); loc && loc->track.kind == TrackKind::Audio) c = edit::clipById(s, id);
        QVERIFY(c);
        Effect lim = makeEffect(p, "limiter");
        lim.params["ceiling_db"] = -12.0;
        c->effects.push_back(lim);
        c->audio.params["gain_db"] = -6.0;  // volume stays live: not baked
        std::string err;
        const std::string out = path("rendered.wav");
        QVERIFY2(renderClipAudio(p, s, c->id, out, &err), err.c_str());
        AudioBufferPtr baked = decodeAudio(out, 48000, &err);
        QVERIFY(baked);
        QVERIFY(std::abs(baked->frames() - 96000) < 100);  // 60 frames at 30 fps = 2 s
        QVERIFY(std::fabs(baked->samples[48000 * 2] - 0.2512f) < 0.005f);
    }

    void mixerGainPanMuteAndFades() {
        std::string wav = path("mix.wav");
        writeWav(wav, 48000, 2.0, 0.5f, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        auto r = edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        QVERIFY(r.ok);
        QCOMPARE(r.created.size(), size_t(1));
        Clip* c = edit::clipById(s, r.created[0]);
        QCOMPARE(c->duration, FrameTime(60));
        c->audio.params["gain_db"] = -6.0206;
        AudioMixer mixer;
        std::vector<float> out(4800 * 2);
        std::vector<MeterLevels> levels;
        mixer.mix(p, s, 24000, 4800, out.data(), &levels);
        QVERIFY(std::fabs(out[100] - 0.25f) < 0.005f);
        QCOMPARE(levels.size(), s.audioTracks.size());
        QVERIFY(std::fabs(levels[0].peakL - 0.25f) < 0.005f);
        // Hard left pan.
        c->audio.params["pan"] = -1.0;
        mixer.mix(p, s, 24000, 4800, out.data());
        QVERIFY(out[101] < 0.001f && out[100] > 0.3f);
        c->audio.params["pan"] = 0.0;
        // Track fader and mute.
        trackAt(s, {TrackKind::Audio, 0})->volumeDb = -6.0206;
        mixer.mix(p, s, 24000, 4800, out.data());
        QVERIFY(std::fabs(out[100] - 0.125f) < 0.005f);
        trackAt(s, {TrackKind::Audio, 0})->muted = true;
        mixer.mix(p, s, 24000, 4800, out.data());
        QCOMPARE(out[100], 0.0f);
        trackAt(s, {TrackKind::Audio, 0})->muted = false;
        trackAt(s, {TrackKind::Audio, 0})->volumeDb = 0;
        // Solo on another (empty) track silences this one.
        trackAt(s, {TrackKind::Audio, 1})->solo = true;
        mixer.mix(p, s, 24000, 4800, out.data());
        QCOMPARE(out[100], 0.0f);
        trackAt(s, {TrackKind::Audio, 1})->solo = false;
        // Fade in over the first 10 frames (16000 samples): rises from silence.
        c->audio.params["gain_db"] = 0.0;
        QVERIFY(edit::addTransition(p, s, c->id, edit::Edge::In, "crossfade", 10).ok);
        std::vector<float> head(16000 * 2);
        mixer.mix(p, s, 0, 16000, head.data());
        QVERIFY(meanAbs(head, 0, 0, 800) < 0.05f);
        QVERIFY(meanAbs(head, 0, 15000, 16000) > 0.45f);
        // Past the clip: silence.
        mixer.mix(p, s, 48000 * 3, 4800, out.data());
        QCOMPARE(out[10], 0.0f);
    }

    void nestedAudioFollowsSpeedAndRate() {
        // A 3 s WAV whose level steps up each second: 0.1, 0.2, 0.3.
        std::string wav = path("steps.wav");
        {
            FILE* f = std::fopen(wav.c_str(), "wb");
            int rate = 48000, frames = rate * 3;
            uint32_t bytes = uint32_t(frames) * 4;
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
            u32(uint32_t(rate)); u32(uint32_t(rate) * 4); u16(4); u16(16); std::fwrite("data", 1, 4, f); u32(bytes);
            for (int i = 0; i < frames; ++i) {
                int16_t v = int16_t(std::lround((0.1 * (i / rate + 1)) * 32767));
                std::fwrite(&v, 2, 1, f);
                std::fwrite(&v, 2, 1, f);
            }
            std::fclose(f);
        }
        Project p = makeDefaultProject();
        Sequence& inner = *p.active();
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, inner, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Id innerId = inner.id;
        auto r = edit::makeCompound(p, *p.findSequence(innerId), edit::expandLinks(*p.findSequence(innerId),
                                    {trackAt(*p.findSequence(innerId), {TrackKind::Audio, 0})->clips[0].id}), "Nest");
        QVERIFY(r.ok);
        Sequence& outer = *p.findSequence(innerId);
        Id nestAudio = trackAt(outer, {TrackKind::Audio, 0})->clips[0].id;
        AudioMixer mixer;
        std::vector<float> out(4800 * 2);
        // Normal speed: at 1.5 s we hear the second step.
        mixer.mix(p, outer, 72000, 4800, out.data());
        QVERIFY2(std::fabs(out[200] - 0.2f) < 0.01f, qPrintable(QString::number(out[200])));
        // 2x speed: the block 0.9-1.1 s of timeline covers source 1.8-2.2 s, so the
        // step from 0.2 to 0.3 happens inside it, at timeline 1.0 s.
        QVERIFY(edit::setSpeed(p, outer, nestAudio, 2.0, true).ok);
        std::vector<float> blk(9600 * 2);
        mixer.mix(p, outer, 43200, 9600, blk.data());
        QVERIFY2(std::fabs(blk[200] - 0.2f) < 0.01f, qPrintable(QString::number(blk[200])));
        QVERIFY2(std::fabs(blk[7200 * 2] - 0.3f) < 0.01f, qPrintable(QString::number(blk[7200 * 2])));
        // Outer sequence at 44.1 kHz: timing is still right (0.5 s -> source 1.0 s -> second step).
        outer.sampleRate = 44100;
        mixer.reset();
        mixer.mix(p, outer, 22050 + 2000, 2000, out.data());
        QVERIFY2(std::fabs(out[200] - 0.2f) < 0.01f, qPrintable(QString::number(out[200])));
    }

    void relinkOfflineMedia() {
        // A: a red second at 64 x 36; B: two seconds at 64 x 36; C: a second at 32 x 18; and a sound.
        QDir().mkpath(QString::fromStdString(path("relink/orig")));
        auto video = [&](const char* name, int w, int h, int frames) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = w, gs.height = h, gs.fps = Rational{25, 1};
            Clip c = makeGeneratorClip(gen, "color", frames);
            c.generator.params["color.r"] = Param(0.0);
            c.generator.params["color.g"] = Param(0.7);
            c.generator.params["color.b"] = Param(0.2);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.audioCodec = "none";
            st.preset = "ultrafast";
            st.path = path(name);
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        };
        video("relink/orig/a.mp4", 64, 36, 25);
        video("relink/orig/b.mp4", 64, 36, 50);
        video("relink/c.mp4", 32, 18, 25);
        writeWav(path("relink/orig/voice.wav"), 48000, 1.0, 0.2f, 0.2f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{25, 1};
        MediaItem a = probeOrFail(p, path("relink/orig/a.mp4")), b = probeOrFail(p, path("relink/orig/b.mp4"));
        MediaItem voice = probeOrFail(p, path("relink/orig/voice.wav"));
        a.rating = 4;
        p.media.push_back(a), p.media.push_back(b), p.media.push_back(voice);
        const auto sub = makeSubclip(p, a.id, 0.2, 0.6);
        QVERIFY(sub);
        MediaItem subItem = *sub;
        subItem.id = p.newId();
        p.media.push_back(subItem);
        QVERIFY(edit::placeMedia(p, s, a.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(offlineMedia(p).empty());

        // The card moves: everything is offline (the subclip with its media), and the clip shows Media Offline.
        QVERIFY(QDir().mkpath(QString::fromStdString(path("relink/moved"))));
        QVERIFY(QDir().rename(QString::fromStdString(path("relink/orig")), QString::fromStdString(path("relink/moved/card"))));
        QCOMPARE(offlineMedia(p), (std::vector<Id>{a.id, b.id, voice.id}));
        QVERIFY(isOffline(*p.findMedia(subItem.id)));
        const Image slate = renderSequenceFrame(p, s, 5, {});
        QCOMPARE(slate.width, 64);
        auto px = [&](const Image& im, int x, int y, int ch) { return im.px[(size_t(y) * size_t(im.width) + size_t(x)) * 4 + size_t(ch)]; };
        QVERIFY2(px(slate, 2, 2, 0) > 0.3f && px(slate, 2, 2, 1) < 0.12f, qPrintable(QString::number(px(slate, 2, 2, 0))));
        bool text = false;
        for (int y = 0; y < slate.height; ++y)
            for (int x = 0; x < slate.width; ++x) text |= px(slate, x, y, 1) > 0.3f;
        QVERIFY(text);  // "Media Offline" in white

        // Only the same footage relinks: not a sound, another size or another length.
        const std::string card = path("relink/moved/card/");
        std::string why;
        QVERIFY(!relinkMedia(p, a.id, card + "voice.wav", RelinkCheck::Strict, &why));
        QVERIFY(!relinkMedia(p, a.id, path("relink/c.mp4"), RelinkCheck::Strict, &why));
        QVERIFY2(QString::fromStdString(why).contains("32 x 18"), why.c_str());
        QVERIFY(!relinkMedia(p, a.id, card + "b.mp4", RelinkCheck::Strict, &why));
        QVERIFY(!relinkMedia(p, a.id, path("relink/nowhere.mp4"), RelinkCheck::Strict, &why));
        QVERIFY(isOffline(*p.findMedia(a.id)));

        // Searching the folder above finds all three a level down; the subclip follows; the logging stays.
        Project found = p;
        QCOMPARE(relinkFromFolder(found, path("relink/moved")), (std::vector<Id>{a.id, b.id, voice.id}));
        QVERIFY(offlineMedia(found).empty());
        QCOMPARE(found.findMedia(a.id)->path, card + "a.mp4");
        QCOMPARE(found.findMedia(subItem.id)->path, card + "a.mp4");
        QCOMPARE(found.findMedia(subItem.id)->subclipIn, 0.2);
        QCOMPARE(found.findMedia(a.id)->rating, 4);
        const Image back = renderSequenceFrame(found, *found.active(), 5, {});
        QVERIFY(px(back, 2, 2, 1) > 0.3f && px(back, 2, 2, 0) < 0.1f);
        // Too deep a search finds nothing; a transcode is found by its name with another extension.
        Project shallow = p;
        QVERIFY(relinkFromFolder(shallow, path("relink"), {}, 0).empty());
        Project transcoded = p;
        for (MediaItem& m : transcoded.media)
            if (m.id == b.id) m.path = "D:\\Shoot\\Card 1\\b.mxf";
        QCOMPARE(relinkFromFolder(transcoded, path("relink"), {b.id}), std::vector<Id>{b.id});
        QCOMPARE(transcoded.findMedia(b.id)->path, card + "b.mp4");

        // Replace Footage takes different footage, its details and name, keeping the clips.
        Project replaced = p;
        QVERIFY(!relinkMedia(replaced, a.id, card + "voice.wav", RelinkCheck::Replace, &why));
        QVERIFY2(relinkMedia(replaced, a.id, path("relink/c.mp4"), RelinkCheck::Replace, &why), why.c_str());
        QCOMPARE(replaced.findMedia(a.id)->width, 32);
        QCOMPARE(replaced.findMedia(a.id)->name, std::string("c.mp4"));
        QCOMPARE(replaced.findMedia(subItem.id)->path, path("relink/c.mp4"));
        QCOMPARE(replaced.active()->videoTracks[0].clips.size(), size_t(1));
    }

    void projectManager() {
        // A: four seconds of a changing ramp (ProRes, 96 x 54 at 25 fps); B: a sound nothing uses; C: a still.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 96, gs.height = 54, gs.fps = Rational{25, 1};
        Clip ramp = makeGeneratorClip(gen, "color", 100);
        ramp.generator.params["color.r"].addKey(0, 0.0);
        ramp.generator.params["color.r"].addKey(99, 1.0);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, ramp);
        ExportSettings st;
        st.videoCodec = "prores_ks";
        st.audioCodec = "none";
        st.path = path("pm-a.mov");
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        writeWav(path("pm-b.wav"), 48000, 1.0, 0.2f, 0.2f);
        QImage still(96, 54, QImage::Format_RGB32);
        still.fill(qRgb(30, 160, 60));
        QVERIFY(still.save(QString::fromStdString(path("pm-c.png"))));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 96, s.height = 54, s.fps = Rational{25, 1};
        MediaItem a = probeOrFail(p, path("pm-a.mov")), b = probeOrFail(p, path("pm-b.wav")), c = probeOrFail(p, path("pm-c.png"));
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.start = 0.5, seg.end = 3.5;
        seg.words = {{0.5, 0.8, "one"}, {1.5, 1.8, "two"}, {3.2, 3.5, "three"}};
        t->segments.push_back(seg);
        a.transcript = t;
        p.media.push_back(a), p.media.push_back(b), p.media.push_back(c);
        // The cut uses A's second second, then the still; another sequence uses A from 3 s.
        QVERIFY(edit::placeMedia(p, s, a.id, 0, 25, 50, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::placeMedia(p, s, c.id, 25, 0, 25, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id mainId = s.id;  // `s` goes stale when the sequence list grows
        Sequence other = makeSequence(p, "Other", 96, 54, Rational{25, 1});
        QVERIFY(edit::placeMedia(p, other, a.id, 0, 75, 90, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        p.sequences.push_back(other);
        auto frame = [](const Project& pr, FrameTime f) {
            RenderOptions ro;
            return renderProgramFrame(pr, *pr.active(), f, ro);
        };
        const Image before = frame(p, 10);
        QCOMPARE(usedMedia(p, {}).size(), size_t(2));

        // Collect: the used files copied whole, B left out, everything pointing into the new folder.
        ConsolidateOptions o;
        o.folder = path("collected");
        o.name = "Collected";
        ConsolidateResult res;
        QVERIFY2(consolidateProject(p, o, &res, {}, nullptr, &err), err.c_str());
        QCOMPARE(res.copied, 2);
        QCOMPARE(res.trimmed, 0);
        QVERIFY(res.bytes > 0 && res.missing.empty());
        Project collected;
        QVERIFY2(loadProject(res.projectPath, collected, &err), err.c_str());
        QCOMPARE(collected.media.size(), size_t(2));
        QCOMPARE(collected.sequences.size(), size_t(2));
        for (const MediaItem& m : collected.media) QVERIFY2(QString::fromStdString(m.path).contains("/collected/Media/"), m.path.c_str());
        QVERIFY(frame(collected, 10).px == before.px);

        // Consolidate the first sequence only, with half a second of handles: A becomes 0.5 to 2.5 s.
        o.folder = path("trimmed");
        o.trim = true;
        o.handles = 0.5;
        o.sequences = {mainId};
        QVERIFY2(consolidateProject(p, o, &res, {}, nullptr, &err), err.c_str());
        QCOMPARE(res.trimmed, 1);
        QCOMPARE(res.copied, 1);
        Project trimmed;
        QVERIFY2(loadProject(res.projectPath, trimmed, &err), err.c_str());
        QCOMPARE(trimmed.sequences.size(), size_t(1));
        const MediaItem* ta = trimmed.findMedia(a.id);
        QVERIFY(ta && QString::fromStdString(ta->path).endsWith("_trim.mov"));
        QVERIFY2(std::fabs(ta->duration - 2.0) < 0.1, qPrintable(QString::number(ta->duration)));
        QCOMPARE(trimmed.active()->videoTracks[0].clips[0].sourceIn, 12.5);
        const Image after = frame(trimmed, 10);
        float worst = 0;
        for (size_t i = 0; i < after.px.size(); ++i) worst = std::max(worst, std::fabs(after.px[i] - before.px[i]));
        QVERIFY2(worst < 0.02f, qPrintable(QString::number(worst)));
        // The transcript moved with it: "one" at 0 s, "two" at 1 s, "three" (3.2 s) outside the span.
        QVERIFY(ta->transcript);
        const auto& words = ta->transcript->segments.at(0).words;
        QCOMPARE(words.size(), size_t(2));
        QVERIFY(std::fabs(words[0].start) < 1e-9 && std::fabs(words[1].start - 1.0) < 1e-9);

        // A missing file is reported and left where it was; no folder is an error.
        p.media.back().path = path("gone.png");
        o.trim = false;
        o.folder = path("partial");
        QVERIFY(consolidateProject(p, o, &res, {}, nullptr, &err));
        QCOMPARE(res.missing.size(), size_t(1));
        o.folder.clear();
        QVERIFY(!consolidateProject(p, o, &res, {}, nullptr, &err));
    }

    void smartRendering() {
        // The source: one second of a changing ramp in ProRes 422 HQ, 128 x 72 at 25 fps.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 128, gs.height = 72, gs.fps = Rational{25, 1};
        Clip ramp = makeGeneratorClip(gen, "color", 25);
        ramp.generator.params["color.r"].addKey(0, 0.1);
        ramp.generator.params["color.r"].addKey(24, 0.9);
        ramp.generator.params["color.g"] = 0.4;
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, ramp);
        ExportSettings st;
        st.videoCodec = "prores_ks";
        st.profile = "hq";
        st.audioCodec = "none";
        st.path = path("smart-source.mov");
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        // A cut of it: the first 15 frames as they are, the rest inverted.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 128, s.height = 72, s.fps = Rational{25, 1};
        MediaItem m = probeOrFail(p, st.path);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::razor(p, s, {TrackKind::Video, 0}, 15).ok);
        s.videoTracks[0].clips[1].effects.push_back(makeEffect(p, "invert"));
        ExportSettings ex = st;
        ex.path = path("smart.mov");
        int copied = -1;
        QVERIFY2(exportSequence(p, s, ex, nullptr, nullptr, &err, nullptr, &copied), err.c_str());
        QCOMPARE(copied, 15);
        // The copied frames are the source's own, byte for byte; the rest decode inverted.
        auto packets = [](const std::string& file) {
            std::vector<QByteArray> out;
            AVFormatContext* fmt = nullptr;
            if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) return out;
            AVPacket* pkt = av_packet_alloc();
            while (av_read_frame(fmt, pkt) >= 0) {
                if (fmt->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                    out.push_back(QByteArray(reinterpret_cast<const char*>(pkt->data), pkt->size));
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            avformat_close_input(&fmt);
            return out;
        };
        const auto source = packets(st.path), smart = packets(ex.path);
        QCOMPARE(smart.size(), size_t(25));
        for (int i = 0; i < 15; ++i) QVERIFY2(smart[size_t(i)] == source[size_t(i)], qPrintable(QString::number(i)));
        QVERIFY(smart[20] != source[20]);
        VideoDecoder a, b;
        QVERIFY(a.open(st.path, &err) && b.open(ex.path, &err));
        const Frame16Ptr fa = a.frameAt(20 / 25.0, 0, 0), fb = b.frameAt(20 / 25.0, 0, 0);
        QVERIFY(fa && fb);
        const size_t mid = (size_t(36) * 128 + 64) * 4;
        QVERIFY2(std::fabs(fa->px[mid] / 65535.0 + fb->px[mid] / 65535.0 - 1.0) < 0.02, qPrintable(QString("%1 %2").arg(fa->px[mid]).arg(fb->px[mid])));
        const Frame16Ptr ca = a.frameAt(5 / 25.0, 0, 0), cb = b.frameAt(5 / 25.0, 0, 0);
        QVERIFY(ca && cb && ca->px == cb->px);
        // Nothing is copied when smart rendering is off, for another flavour, or where the clip is moved.
        ex.smartRender = false;
        QVERIFY(exportSequence(p, s, ex, nullptr, nullptr, &err, nullptr, &copied));
        QCOMPARE(copied, 0);
        ex.smartRender = true;
        ex.profile = "proxy";
        QVERIFY(exportSequence(p, s, ex, nullptr, nullptr, &err, nullptr, &copied));
        QCOMPARE(copied, 0);
        ex.profile = "hq";
        s.videoTracks[0].clips[0].motion.params["scale"] = 90.0;
        QVERIFY(exportSequence(p, s, ex, nullptr, nullptr, &err, nullptr, &copied));
        QCOMPARE(copied, 0);
        // Burn-ins change every frame: nothing copied.
        s.videoTracks[0].clips[0].motion.params["scale"] = 100.0;
        ex.burnIn.timecode = true;
        QVERIFY(exportSequence(p, s, ex, nullptr, nullptr, &err, nullptr, &copied));
        QCOMPARE(copied, 0);
    }

    void exportedChapters() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        const FrameTime sec = FrameTime(std::lround(s.fpsValue()));
        edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 3 * sec));
        edit::addMarker(s, Marker{sec, 0, "Middle", "", 0, true});
        edit::addMarker(s, Marker{2 * sec, 0, "End", "", 0, true});
        edit::addMarker(s, Marker{sec + sec / 2, 0, "Not a chapter", "", 0});
        // The chapters a player sees: start (seconds) and title.
        auto chaptersIn = [](const std::string& file) {
            std::vector<std::pair<double, QString>> out;
            AVFormatContext* fmt = nullptr;
            if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) return out;
            for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
                const AVChapter* c = fmt->chapters[i];
                const AVDictionaryEntry* t = av_dict_get(c->metadata, "title", nullptr, 0);
                out.push_back({double(c->start) * av_q2d(c->time_base), t ? QString::fromUtf8(t->value) : QString()});
            }
            avformat_close_input(&fmt);
            return out;
        };
        ExportSettings st;
        st.videoCodec = "libx264";
        st.audioCodec = "none";
        st.preset = "ultrafast";
        st.crf = 35;
        std::string err;
        for (const char* file : {"chapters.mp4", "chapters.mov", "chapters.mkv"}) {
            st.path = path(file);
            QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
            const auto ch = chaptersIn(st.path);
            QVERIFY2(ch.size() == 3, file);
            QCOMPARE(ch[0].second, QString("Intro"));
            QCOMPARE(ch[1].second, QString("Middle"));
            QCOMPARE(ch[2].second, QString("End"));
            QVERIFY2(std::fabs(ch[0].first) < 0.01 && std::fabs(ch[1].first - 1) < 0.01 && std::fabs(ch[2].first - 2) < 0.01, file);
        }
        // An In to Out range: timed from its start.
        st.path = path("range.mp4");
        st.in = sec;
        st.out = 3 * sec - 1;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        auto ch = chaptersIn(st.path);
        QCOMPARE(ch.size(), size_t(2));
        QCOMPARE(ch[0].second, QString("Middle"));
        QVERIFY(std::fabs(ch[0].first) < 0.01 && std::fabs(ch[1].first - 1) < 0.01);
        // Turned off.
        st.chapters = false;
        st.path = path("nochapters.mp4");
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        QVERIFY(chaptersIn(st.path).empty());
    }

    void exportAndDecodeFrameAccurately() {
        // A colour matte whose red channel ramps 0 -> 1 over 30 frames.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        Clip c = makeGeneratorClip(p, "color", 30);
        c.generator.params["color.r"].addKey(0, 0.0);
        c.generator.params["color.r"].addKey(29, 1.0);
        c.generator.params["color.g"] = 0.2;
        c.generator.params["color.b"] = 0.2;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        ExportSettings st;
        st.path = path("ramp.mp4");
        st.videoCodec = "libx264";
        st.audioCodec = "none";
        st.crf = 1;
        st.preset = "ultrafast";
        std::string err;
        int progressCalls = 0;
        QVERIFY2(exportSequence(p, s, st, [&](double, FrameTime) { ++progressCalls; }, nullptr, &err), err.c_str());
        QVERIFY(progressCalls > 0);
        Project q;
        MediaItem m = probeOrFail(q, st.path);
        QCOMPARE(m.width, 160);
        QCOMPARE(m.height, 90);
        QVERIFY(std::fabs(m.fps.toDouble() - 30) < 0.01);
        QVERIFY(std::fabs(m.duration - 1.0) < 0.05);
        VideoDecoder dec;
        QVERIFY(dec.open(st.path, &err));
        auto redAt = [&](int frame) {
            Frame16Ptr f = dec.frameAt(frame / 30.0, 0, 0);
            if (!f) return -1.0f;
            return f->px[(size_t(45) * 160 + 80) * 4] / 65535.0f;
        };
        // Random access in both directions, then sequential.
        for (int frame : {25, 3, 17, 0, 29, 12, 13, 14, 15}) {
            float red = redAt(frame);
            QVERIFY2(std::fabs(red - frame / 29.0f) < 0.03f,
                     qPrintable(QString("frame %1: red %2 expected %3").arg(frame).arg(red).arg(frame / 29.0f)));
        }
        // The shared pool returns the same frames (and caches them).
        Frame16Ptr a = MediaPool::instance().videoFrame(st.path, 10 / 30.0, 80, 45);
        Frame16Ptr b = MediaPool::instance().videoFrame(st.path, 10 / 30.0, 80, 45);
        QVERIFY(a && a == b);
        QCOMPARE(a->width, 80);
    }

    void hardwareDecodeAndEncode() {
        // A 320x180 ramp, encoded in software.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        Clip c = makeGeneratorClip(p, "color", 30);
        c.generator.params["color.r"].addKey(0, 0.0);
        c.generator.params["color.r"].addKey(29, 1.0);
        c.generator.params["color.g"] = 0.3;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        ExportSettings st;
        st.path = path("hw-ramp.mp4");
        st.videoCodec = "libx264";
        st.audioCodec = "none";
        st.crf = 10;
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());

        // Hardware decoding (when this machine has a device) gives the same
        // picture as software. H.264 decoding is bit exact, but the device's
        // NV12 output goes through a different conversion to RGB than
        // software's planar YUV, so allow rounding-level differences.
        auto frames = [&](HwDecodeMode mode, std::string* hw) {
            setHwDecodeMode(mode);
            VideoDecoder dec;
            std::vector<Frame16Ptr> out;
            if (!dec.open(st.path, &err)) return out;
            for (int f : {0, 7, 15, 29, 3}) out.push_back(dec.frameAt(f / 30.0));
            *hw = dec.hardware();
            return out;
        };
        std::string swName, hwName;
        const int slotsBefore = activeHwDecoders();  // pooled decoders from earlier tests may hold some
        auto sw = frames(HwDecodeMode::Off, &swName);
        auto hw = frames(HwDecodeMode::Auto, &hwName);
        setHwDecodeMode(HwDecodeMode::Auto);
        QVERIFY(swName.empty());
        qInfo("hardware decoder: %s", hwName.empty() ? "none (software)" : hwName.c_str());
        QCOMPARE(sw.size(), size_t(5));
        QCOMPARE(hw.size(), size_t(5));
        for (size_t i = 0; i < sw.size(); ++i) {
            QVERIFY(sw[i] && hw[i]);
            QCOMPARE(hw[i]->width, sw[i]->width);
            QCOMPARE(hw[i]->px.size(), sw[i]->px.size());
            double sum = 0, worst = 0;
            for (size_t k = 0; k < sw[i]->px.size(); ++k) {
                const double d = std::abs(int(hw[i]->px[k]) - int(sw[i]->px[k])) / 65535.0;
                sum += d;
                worst = std::max(worst, d);
            }
            const double mean = sum / double(sw[i]->px.size());
            QVERIFY2(mean < 0.006 && worst < 0.08,
                     qPrintable(QString("frame %1 differs: mean %2, max %3").arg(i).arg(mean).arg(worst)));
        }
        QCOMPARE(activeHwDecoders(), slotsBefore);  // slots are released when decoders close

        // The hardware preset exports with this machine's encoder, or x264.
        const ExportPreset* preset = findExportPreset("H.264 - Hardware");
        QVERIFY(preset);
        QVERIFY(findExportPreset("H.265 - Hardware"));
        ExportSettings hs = preset->settings;
        hs.path = path("hw-export.mp4");
        hs.audioCodec = "none";
        std::string used;
        QVERIFY2(exportSequence(p, s, hs, nullptr, nullptr, &err, &used), err.c_str());
        qInfo("hardware preset encoder: %s", used.c_str());
        QVERIFY(!used.empty());
        Project q;
        MediaItem m = probeOrFail(q, hs.path);
        QCOMPARE(m.width, 320);
        QVERIFY(std::fabs(m.duration - 1.0) < 0.1);
        VideoDecoder dec;
        QVERIFY(dec.open(hs.path, &err));
        Frame16Ptr f = dec.frameAt(15 / 30.0);
        QVERIFY(f);
        const float red = f->px[(size_t(90) * 320 + 160) * 4] / 65535.0f;
        QVERIFY2(std::fabs(red - 15 / 29.0f) < 0.06f, qPrintable(QString::number(red)));
    }

#ifdef MONTAGE_WITH_WHISPER
    void speakerLabels() {
        // Labelling: words go to whoever speaks over them; segments split where that changes.
        Transcript t;
        TranscriptSegment seg;
        seg.start = 0;
        seg.end = 6;
        for (int i = 0; i < 6; ++i) seg.words.push_back({double(i), i + 0.8, "w" + std::to_string(i), 1});
        seg.text = "w0 w1 w2 w3 w4 w5";
        t.segments.push_back(seg);
        TranscriptSegment quiet{7, 8, "hm", {}, -1};
        t.segments.push_back(quiet);
        applySpeakers(t, {{0, 2.9, 0}, {2.9, 3.1, 1}, {3.1, 3.5, 0}, {3.5, 6.5, 1}});
        QCOMPARE(int(t.segments.size()), 3);
        QCOMPARE(t.segments[0].text, std::string("w0 w1 w2 w3"));  // w3 overlaps 0 most
        QCOMPARE(t.segments[0].speaker, 0);
        QCOMPARE(t.segments[1].text, std::string("w4 w5"));
        QCOMPARE(t.segments[1].speaker, 1);
        QCOMPARE(t.segments[2].speaker, 1);  // within a second of speaker 2's turn
        QCOMPARE(speakerCount(t), 2);
        QCOMPARE(speakerName(t, 1), std::string("Speaker 2"));
        t.speakerNames = {"Ann"};
        QCOMPARE(speakerName(t, 0), std::string("Ann"));
        const std::string vtt = transcriptAs(t, "vtt");
        QVERIFY2(vtt.find("<v Ann>w0 w1 w2 w3") != std::string::npos && vtt.find("<v Speaker 2>w4 w5") != std::string::npos, vtt.c_str());
        QVERIFY(transcriptAs(t, "txt").find("Ann:\nw0 w1 w2 w3\n\nSpeaker 2:\nw4 w5\n") != std::string::npos);
        Transcript back;
        QVERIFY(transcriptFromJson(transcriptToJson(t), back) && back == t);

        // Features: a 1 kHz tone puts its energy in the mel bin around 1 kHz, every 10 ms.
        std::vector<float> tone(16000);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * float(std::sin(2 * M_PI * 1000 * double(i) / 16000));
        const std::vector<float> fb = speakerFeatures(tone.data(), tone.size());
        QCOMPARE(int(fb.size()), 100 * 80);
        const float* frame = &fb[50 * 80];
        const int peak = int(std::max_element(frame, frame + 80) - frame);
        // Mel bin centres: 1 kHz is 1000.0 Hz -> mel 999.99; bins step (mel(7600) - mel(20)) / 81.
        const double step = (1127 * std::log(1 + 7600 / 700.0) - 1127 * std::log(1 + 20 / 700.0)) / 81;
        const int expected = int(std::lround((1127 * std::log(1 + 1000 / 700.0) - 1127 * std::log(1 + 20 / 700.0)) / step)) - 1;
        QVERIFY2(std::abs(peak - expected) <= 1, qPrintable(QString("%1 %2").arg(peak).arg(expected)));

        // Clustering: three voices, each heard eight times with some variation.
        std::mt19937 rng(5);
        std::normal_distribution<float> g(0, 1);
        std::vector<std::vector<float>> centres(3, std::vector<float>(64)), points;
        for (auto& c : centres)
            for (float& v : c) v = g(rng);
        std::vector<int> truth;
        for (int i = 0; i < 24; ++i) {
            const int k = (i * 7) % 3;
            std::vector<float> p = centres[size_t(k)];
            for (float& v : p) v += 0.25f * g(rng);
            points.push_back(p);
            truth.push_back(k);
        }
        for (const auto& labels : {clusterSpeakers(points, 0, 0.5), clusterSpeakers(points, 3, 0)}) {
            QCOMPARE(*std::max_element(labels.begin(), labels.end()), 2);
            for (int i = 0; i < 24; ++i)
                for (int j = 0; j < 24; ++j) QCOMPARE(labels[size_t(i)] == labels[size_t(j)], truth[size_t(i)] == truth[size_t(j)]);
            QCOMPARE(labels[0], 0);  // numbered in order of first appearance
        }
        const std::vector<int> two = clusterSpeakers(points, 2, 0);
        QCOMPARE(*std::max_element(two.begin(), two.end()), 1);
        QCOMPARE(int(clusterSpeakers({}, 0, 0.5).size()), 0);

        // The real models, if present: two voices, one of them heard twice.
        if (!diarizerAvailable()) QSKIP("Built without ONNX Runtime");
        if (!speakerModel().installed()) QSKIP("Set MONTAGE_SPEAKER_MODEL to a folder with the speaker models to run the rest");
        std::string err;
        AudioBufferPtr jfk = decodeAudio(MONTAGE_TEST_DATA_DIR "/jfk.wav", 16000, &err);
        QVERIFY2(jfk, err.c_str());
        std::vector<float> a(size_t(jfk->frames()));
        for (size_t i = 0; i < a.size(); ++i) a[i] = jfk->samples[i * 2];
        // A second voice: the same speech 35 % higher and faster (pitch and formants move together).
        std::vector<float> b;
        for (double x = 0; x + 1 < double(a.size()); x += 1.35) {
            const size_t i = size_t(x);
            b.push_back(float(a[i] + (a[i + 1] - a[i]) * (x - double(i))));
        }
        std::vector<float> mix = a;
        mix.insert(mix.end(), 16000, 0.f);
        const double bStart = double(mix.size()) / 16000;
        mix.insert(mix.end(), b.begin(), b.end());
        mix.insert(mix.end(), 16000, 0.f);
        const double aAgain = double(mix.size()) / 16000;
        mix.insert(mix.end(), a.begin(), a.begin() + 16000 * 6);
        std::vector<SpeakerTurn> turns;
        double last = 0;
        QVERIFY2(diarize(mix, {}, turns, [&](double f) { last = f; }, nullptr, &err), err.c_str());
        QCOMPARE(last, 1.0);
        for (const auto& turn : turns) qInfo("turn %.2f-%.2f speaker %d", turn.start, turn.end, turn.speaker);
        // Who speaks most in each part, and for most of it.
        auto main = [&](double from, double to) {
            std::map<int, double> time;
            for (const auto& turn : turns) time[turn.speaker] += std::max(0.0, std::min(to, turn.end) - std::max(from, turn.start));
            auto best = std::max_element(time.begin(), time.end(), [](auto& x, auto& y) { return x.second < y.second; });
            return best == time.end() || best->second < 0.5 * (to - from) ? -1 : best->first;
        };
        QCOMPARE(main(0, 11), 0);
        QCOMPARE(main(bStart, bStart + 8), 1);
        QCOMPARE(main(aAgain, aAgain + 6), 0);
        int people = 0;
        for (const auto& turn : turns) people = std::max(people, turn.speaker + 1);
        QCOMPARE(people, 2);
        // Told there is one speaker, it finds one.
        DiarizeOptions one;
        one.speakers = 1;
        QVERIFY(diarize(mix, one, turns, {}, nullptr, &err));
        for (const auto& turn : turns) QCOMPARE(turn.speaker, 0);
    }

    void findSimilarShotsByFrame() {
        if (!visualSearchAvailable() || !visualModel().installed()) QSKIP("Set MONTAGE_VISUAL_MODEL to the CLIP model");
        // Two videos with the same two scenes (a red kitchen, a blue garden) in the opposite order.
        auto scenes = [&](const std::string& file, bool kitchenFirst) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 320;
            gs.height = 180;
            gs.fps = {25, 1};
            for (int k = 0; k < 2; ++k) {
                const bool kitchen = (k == 0) == kitchenFirst;
                Clip c = makeGeneratorClip(gen, "color", 100);
                c.generator.params["color.r"] = Param(kitchen ? 0.85 : 0.05);
                c.generator.params["color.g"] = Param(kitchen ? 0.08 : 0.35);
                c.generator.params["color.b"] = Param(kitchen ? 0.06 : 0.9);
                c.start = k * 100;
                edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
                Clip t = makeGeneratorClip(gen, "title", 100);
                t.generator.strings["text"] = kitchen ? "KITCHEN" : "GARDEN";
                t.generator.params["size"] = Param(48.0);
                t.start = k * 100;
                edit::overwrite(gen, gs, {TrackKind::Video, 1}, t);
            }
            ExportSettings st;
            st.path = path(file.c_str());
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            return exportSequence(gen, gs, st, nullptr, nullptr, &err) ? st.path : std::string();
        };
        const std::string a = scenes("similar-a.mp4", true), b = scenes("similar-b.mp4", false);
        QVERIFY(!a.empty() && !b.empty());
        Project p = makeDefaultProject();
        std::string err;
        for (const std::string& f : {a, b}) {
            MediaItem m = probeOrFail(p, f);
            VisualIndex index;
            QVERIFY2(indexVideo(f, 0, index, 0, {}, nullptr, &err), err.c_str());
            m.visual = std::make_shared<const VisualIndex>(index);
            p.media.push_back(m);
        }
        // Like the kitchen in A (at 1 s): the kitchen in B (4-8 s) first, A's own kitchen left out.
        std::vector<float> like;
        QVERIFY2(embedFrame(a, 1.0, like, &err), err.c_str());
        QCOMPARE(int(like.size()), 512);
        const auto hits = montage::findSimilarShots(p, like, p.media[0].id, 1.0, 5);
        QVERIFY(!hits.empty());
        QCOMPARE(hits[0].media, p.media[1].id);
        QVERIFY2(hits[0].best >= 4 && hits[0].best <= 8, qPrintable(QString::number(hits[0].best)));
        QVERIFY(hits[0].score > 0.9f);
        for (const ShotMatch& h : hits) QVERIFY(!(h.media == p.media[0].id && h.start <= 1.0 && h.end >= 1.0));
        // The gardens come after the kitchen.
        for (size_t i = 1; i < hits.size(); ++i) QVERIFY(hits[i].score <= hits[0].score);

        // Through MCP: like B's garden at 2 s finds A's garden (4-8 s).
        const QString project = QString::fromStdString(path("similar.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_find_shots"},
                                                     {"arguments", QJsonObject{{"project", project},
                                                                               {"like", QJsonObject{{"media", QString::fromStdString(b)}, {"seconds", 2.0}}}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject first = r.value("structuredContent").toObject().value("moments").toArray()[0].toObject();
        QVERIFY(first.value("media").toString().endsWith("similar-a.mp4"));
        QVERIFY(first.value("best_seconds").toDouble() >= 4);
    }

    void jpegStillsDecode() {
        // A JPEG still decodes, at any size (FFmpeg's image demuxer reads a JPEG as finished after a seek).
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg", &err), err.c_str());
        QVERIFY(dec.isStill());
        Frame16Ptr f = dec.frameAt(0);
        QVERIFY(f);
        QCOMPARE(f->width, 320);
        QCOMPARE(f->height, 415);
        Frame16Ptr half = dec.frameAt(1.5, 160, 208, true);
        QVERIFY(half);
        QCOMPARE(half->width, 160);
        // Not black: the portrait's middle has colour.
        const uint16_t* px = &half->px[(size_t(104) * 160 + 80) * 4];
        QVERIFY(int(px[0]) + px[1] + px[2] > 3000);
        MediaItem m;
        QVERIFY2(probeMedia(MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg", m, &err), err.c_str());
        QCOMPARE(int(m.kind), int(MediaKind::Image));
    }

    void depthMaps() {
        if (!depthAvailable() || !depthModel().installed()) QSKIP("Set MONTAGE_DEPTH_MODEL to Depth Anything V2 Small");
        const std::string still = MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg";
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(still, &err), err.c_str());
        Frame16Ptr f = dec.frameAt(0);
        QVERIFY(f);
        const Image img = toImage(*f);
        DepthMap d;
        QVERIFY2(estimateDepth(img, d, 518, &err), err.c_str());
        // The short side at 518, both multiples of 14, as the model was trained.
        QCOMPARE(d.width, 518);
        QCOMPARE(d.height, 644);
        // Near is 1, far 0, as the Python reference gives (0.02 and 0.05 for the dark backdrop, 0.43 for the
        // face, 0.83 for the helmet in front of him, 1 for the table edge).
        auto at = [&](double u, double v) { return d.at(u, v); };
        QVERIFY2(at(0.08, 0.08) < 0.12f && at(0.92, 0.06) < 0.15f, qPrintable(QString("%1 %2").arg(at(0.08, 0.08)).arg(at(0.92, 0.06))));
        QVERIFY2(std::abs(at(0.62, 0.22) - 0.43f) < 0.1f, qPrintable(QString::number(at(0.62, 0.22))));
        QVERIFY2(std::abs(at(0.25, 0.75) - 0.83f) < 0.1f, qPrintable(QString::number(at(0.25, 0.75))));
        QVERIFY(at(0.05, 0.95) > 0.9f);
        QCOMPARE(*std::max_element(d.values.begin(), d.values.end()), 1.0f);
        QCOMPARE(*std::min_element(d.values.begin(), d.values.end()), 0.0f);
        // Cached by content.
        auto c1 = cachedDepth(img), c2 = cachedDepth(img);
        QVERIFY(c1 && c1 == c2);

        // Through the compositor, on the still in a sequence of its own shape.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 400;
        MediaItem m = probeOrFail(p, still);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[0].clips.at(0);
        const Image plain = renderSequenceFrame(p, s, 0, {});
        auto lum = [](const Image& im, double u, double v) {
            const float* q = im.at(int(u * im.width), int(v * im.height));
            return 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
        };
        // Depth Map: the depth as grey, near white.
        clip.effects.push_back(makeEffect(p, "depth_map"));
        Image shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(lum(shown, 0.25, 0.75) > 0.7f && lum(shown, 0.08, 0.08) < 0.15f);
        clip.effects.back().params["invert"] = Param(1.0);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(lum(shown, 0.25, 0.75) < 0.3f && lum(shown, 0.08, 0.08) > 0.85f);
        clip.effects.clear();
        // Depth Fog: the far backdrop turns to the fog colour; the helmet in front is untouched.
        Effect fog = makeEffect(p, "depth_fog");
        fog.params["amount"] = Param(100.0);
        clip.effects.push_back(fog);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY2(lum(shown, 0.08, 0.08) > 0.6f, qPrintable(QString::number(lum(shown, 0.08, 0.08))));
        QVERIFY(std::abs(lum(shown, 0.25, 0.75) - lum(plain, 0.25, 0.75)) < 0.01f);
        clip.effects.clear();
        // A depth qualifier: an effect on the near half only (here, turning it black).
        Effect dark = makeEffect(p, "color_correct");
        dark.params["exposure"] = Param(-10.0);
        dark.params["mask.depth"] = Param(1.0);
        dark.params["mask.depth_low"] = Param(60.0);
        dark.params["mask.depth_soft"] = Param(5.0);
        clip.effects.push_back(dark);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY2(lum(shown, 0.25, 0.75) < 0.02f, qPrintable(QString::number(lum(shown, 0.25, 0.75))));
        QVERIFY(std::abs(lum(shown, 0.62, 0.22) - lum(plain, 0.62, 0.22)) < 0.01f);
        clip.effects.clear();
        // Lens Blur focused on the helmet: the face behind it loses its detail, the helmet keeps its edge.
        // Fine detail: the mean Laplacian (a blurred edge keeps its total gradient, not its curvature).
        auto detail = [](const Image& im, double u, double v) {
            const int cx = int(u * im.width), cy = int(v * im.height);
            auto l = [&](int x, int y) {
                const float* q = im.at(x, y);
                return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
            };
            double sum = 0;
            int n = 0;
            for (int y = cy - 8; y <= cy + 8; ++y)
                for (int x = cx - 8; x <= cx + 8; ++x, ++n) sum += std::abs(4 * l(x, y) - l(x - 1, y) - l(x + 1, y) - l(x, y - 1) - l(x, y + 1));
            return sum / n;
        };
        Effect lens = makeEffect(p, "depth_blur");
        lens.params["focus"] = Param(83.0);
        lens.params["radius"] = Param(8.0);
        clip.effects.push_back(lens);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY2(detail(shown, 0.62, 0.24) < 0.3 * detail(plain, 0.62, 0.24),
                 qPrintable(QString("%1 vs %2").arg(detail(shown, 0.62, 0.24)).arg(detail(plain, 0.62, 0.24))));
        QVERIFY2(detail(shown, 0.25, 0.72) > 0.9 * detail(plain, 0.25, 0.72),
                 qPrintable(QString("%1 vs %2").arg(detail(shown, 0.25, 0.72)).arg(detail(plain, 0.25, 0.72))));

        // Relight from the left, then the right: the helmet's lit side follows the light.
        clip.effects.clear();
        Effect light = makeEffect(p, "relight");
        light.params["intensity"] = Param(120.0);
        light.params["direction"] = Param(180.0);
        clip.effects.push_back(light);
        const Image fromLeft = renderSequenceFrame(p, s, 0, {});
        clip.effects.back().params["direction"] = Param(0.0);
        const Image fromRight = renderSequenceFrame(p, s, 0, {});
        auto side = [&](const Image& im, double u) { return lum(im, u, 0.73) / std::max(1e-3f, lum(plain, u, 0.73)); };
        QVERIFY2(side(fromLeft, 0.13) > side(fromRight, 0.13) && side(fromRight, 0.4) > side(fromLeft, 0.4),
                 qPrintable(QString("%1 %2 %3 %4").arg(side(fromLeft, 0.13)).arg(side(fromRight, 0.13)).arg(side(fromLeft, 0.4)).arg(side(fromRight, 0.4))));

        // Through MCP: a depth qualifier on an effect.
        clip.effects.clear();
        const QString project = QString::fromStdString(path("depth.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_add_effect"},
                                                     {"arguments", QJsonObject{{"project", project}, {"clip", double(clip.id)}, {"effect", "color_correct"},
                                                                               {"params", QJsonObject{{"exposure", -10}, {"mask.depth", 1}, {"mask.depth_low", 60}}}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back, &err));
        shown = renderSequenceFrame(back, *back.active(), 0, {});
        QVERIFY(lum(shown, 0.25, 0.75) < 0.02f && std::abs(lum(shown, 0.62, 0.22) - lum(plain, 0.62, 0.22)) < 0.01f);
    }

    void rifeSlowMotion() {
        if (!rifeAvailable() || !rifeModel().installed()) QSKIP("Set MONTAGE_RIFE_MODEL to the folder with RIFEv4.26_0921.zip");
        // A photo moving 10 px right and 6 px down between two frames; the truth halfway is the crop moved 5 and 3.
        VideoDecoder dec;
        std::string err;
        QVERIFY(dec.open(MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg", &err));
        const Image photo = toImage(*dec.frameAt(0));
        auto crop = [&](int dx, int dy) {
            Image c(256, 192, Image::Uninitialized{});
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < 256; ++x) std::copy_n(photo.at(x + 40 - dx, y + 120 - dy), 4, c.at(x, y));
            return c;
        };
        const Image a = crop(0, 0), b = crop(10, 6), truth = crop(5, 3);
        auto error = [&](const Image& im) {  // mean absolute error inside a 16 px border
            double sum = 0;
            int n = 0;
            for (int y = 16; y < 176; ++y)
                for (int x = 16; x < 240; ++x, ++n)
                    for (int c = 0; c < 3; ++c) sum += std::abs(im.at(x, y)[c] - truth.at(x, y)[c]);
            return sum / (n * 3);
        };
        Image ai;
        QVERIFY2(rifeInterpolate(a, b, 0.5, ai, &err), err.c_str());
        QCOMPARE(ai.width, 256);
        QCOMPARE(ai.height, 192);
        const double eAi = error(ai), eBlend = error(blendFrames(a, b, 0.5)), eFlow = error(interpolateFrames(a, b, 0.5));
        qInfo("halfway error: RIFE %.4f, optical flow %.4f, blend %.4f", eAi, eFlow, eBlend);
        QVERIFY(eAi < eBlend / 3);
        QVERIFY(eAi <= eFlow);
        // A quarter of the way, and the ends: the frames themselves.
        QVERIFY(rifeInterpolate(a, b, 0.0, ai));
        double atA = 0;
        for (size_t i = 0; i < ai.px.size(); ++i) atA = std::max(atA, double(std::abs(ai.px[i] - a.px[i])));
        QVERIFY2(atA < 0.06, qPrintable(QString::number(atA)));
        // The official weights were unpacked beside the graph once.
        QVERIFY(QFile::exists(QString::fromStdString(rifeModel().directory()) + "/rife-4.26.onnx"));
        QVERIFY(QDir(QString::fromStdString(rifeModel().directory()) + "/data").count() > 150);

        // In a sequence: the moving photo as a video, then played at half speed with AI frames.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 256;
        gs.height = 192;
        gs.fps = {25, 1};
        MediaItem pm = probeOrFail(gen, MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg");
        gen.media.push_back(pm);
        QVERIFY(edit::placeMedia(gen, gs, pm.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& moving = gs.videoTracks[0].clips.at(0);
        moving.duration = 6;
        moving.motion.params["fit"] = Param(3.0);  // 1:1
        // Position in sequence pixels from the centre: 10 px right and 6 px down a frame.
        moving.motion.params["pos_x"] = Param(0.0);
        moving.motion.params["pos_y"] = Param(0.0);
        moving.motion.params["pos_x"].keys = {{0, -40.0}, {5, 10.0}};
        moving.motion.params["pos_y"].keys = {{0, -20.0}, {5, 10.0}};
        ExportSettings st;
        st.path = path("moving.mov");
        st.videoCodec = "prores_ks";
        st.profile = "hq";
        st.audioCodec = "none";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 256;
        s.height = 192;
        s.fps = {25, 1};
        MediaItem vm = probeOrFail(p, st.path);
        p.media.push_back(vm);
        QVERIFY(edit::placeMedia(p, s, vm.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::setSpeed(p, s, s.videoTracks[0].clips.at(0).id, 0.5, true, false).ok);
        Clip& slow = s.videoTracks[0].clips.at(0);
        if (slow.timing.empty()) slow.timing = makeEffect(p, "time");
        // Sequence frame 5 is halfway between source frames 2 and 3; the truth is the photo rendered there.
        auto at = [&](int sampling) {
            slow.timing.params["sampling"] = Param(double(sampling));
            return renderSequenceFrame(p, s, 5, {});
        };
        moving.motion.params["pos_x"].keys = {{0, -40.0}, {10, 10.0}};
        moving.motion.params["pos_y"].keys = {{0, -20.0}, {10, 10.0}};
        const Image real = renderSequenceFrame(gen, gs, 5, {});
        auto diff = [&](const Image& im) {
            double sum = 0;
            int n = 0;
            for (int y = 24; y < 168; ++y)
                for (int x = 24; x < 232; ++x, ++n)
                    for (int c = 0; c < 3; ++c) sum += std::abs(im.at(x, y)[c] - real.at(x, y)[c]);
            return sum / (n * 3);
        };
        const double sAi = diff(at(3)), sBlend = diff(at(1)), sNearest = diff(at(0));
        qInfo("half speed: RIFE %.4f, blend %.4f, nearest %.4f", sAi, sBlend, sNearest);
        QVERIFY(sAi < sBlend / 2 && sAi < sNearest / 2);

        // Through MCP: half speed with AI frames.
        Project fresh = makeDefaultProject();
        Sequence& fs = *fresh.active();
        MediaItem fm = probeOrFail(fresh, st.path);
        fresh.media.push_back(fm);
        QVERIFY(edit::placeMedia(fresh, fs, fm.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("rife.montage"));
        QVERIFY(saveProject(fresh, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_set_speed"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        const double clipId = double(fs.videoTracks[0].clips.at(0).id);
        QJsonObject r = call({{"project", project}, {"clip", clipId}, {"speed", 0.5}, {"frames", "ai"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(call({{"project", project}, {"clip", clipId}, {"speed", 0.5}, {"frames", "wobbly"}}).value("isError").toBool());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back, &err));
        QCOMPARE(back.active()->videoTracks[0].clips.at(0).timing.p("sampling", 0), 3.0);
    }

    void textBehindPeople() {
        if (!mattingAvailable() || !mattingModel().installed()) QSKIP("Set MONTAGE_MATTE_MODEL to the MODNet model");
        // A full-frame red "title" over the portrait, put behind him: red round him, he stays in front.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 400;
        MediaItem mi = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg");
        p.media.push_back(mi);
        QVERIFY(edit::placeMedia(p, s, mi.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Image plain = renderSequenceFrame(p, s, 0, {});
        Clip red = makeGeneratorClip(p, "color", 60);
        red.generator.params["color.r"] = Param(1.0);
        red.generator.params["color.g"] = Param(0.0);
        red.generator.params["color.b"] = Param(0.0);
        red.effects.push_back(makeEffect(p, "behind_people"));
        const Id redId = red.id;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, red).ok);
        auto px = [](const Image& im, double u, double v) { return im.at(int(u * im.width), int(v * im.height)); };
        auto same = [&](const Image& im, double u, double v) {
            const float *a = px(im, u, v), *b = px(plain, u, v);
            return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 0.03f;
        };
        Image shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(px(shown, 0.03, 0.03)[0] > 0.95f && px(shown, 0.03, 0.03)[1] < 0.05f);
        QVERIFY(same(shown, 0.62, 0.24) && same(shown, 0.6, 0.5));
        // At half, half the red is over him.
        edit::clipById(s, redId)->effects.back().params["amount"] = Param(50.0);
        shown = renderSequenceFrame(p, s, 0, {});
        const float r = px(shown, 0.62, 0.24)[0], r0 = px(plain, 0.62, 0.24)[0];
        QVERIFY2(std::fabs(r - (0.5f + 0.5f * r0)) < 0.05f, qPrintable(QString("%1 %2").arg(r).arg(r0)));
    }

    void removeBackground() {
        if (!mattingAvailable() || !mattingModel().installed()) QSKIP("Set MONTAGE_MATTE_MODEL to the MODNet model");
        const std::string still = MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg";
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(still, &err), err.c_str());
        const Image img = toImage(*dec.frameAt(0));
        ValueMap m;
        QVERIFY2(estimatePersonMatte(img, m, 512, &err), err.c_str());
        // The short side at 512, multiples of 32.
        QCOMPARE(m.width, 512);
        QCOMPARE(m.height, 640);
        // On him 1, on the backdrop 0, as the Python reference gives.
        QVERIFY(m.at(0.62, 0.24) > 0.95f && m.at(0.6, 0.5) > 0.95f);
        QVERIFY(m.at(0.03, 0.03) < 0.05f && m.at(0.95, 0.1) < 0.05f);
        QVERIFY(cachedPersonMatte(img) == cachedPersonMatte(img));

        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 400;
        Clip red = makeGeneratorClip(p, "color", 60);
        red.generator.params["color.r"] = Param(1.0);
        red.generator.params["color.g"] = Param(0.0);
        red.generator.params["color.b"] = Param(0.0);
        edit::overwrite(p, s, {TrackKind::Video, 0}, red);
        MediaItem mi = probeOrFail(p, still);
        p.media.push_back(mi);
        QVERIFY(edit::placeMedia(p, s, mi.id, 0, 0, -1, {TrackKind::Video, 1}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[1].clips.at(0);
        const Image plain = renderSequenceFrame(p, s, 0, {});
        auto px = [](const Image& im, double u, double v) { return im.at(int(u * im.width), int(v * im.height)); };
        auto same = [&](const Image& im, double u, double v) {
            const float *a = px(im, u, v), *b = px(plain, u, v);
            return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 0.03f;
        };
        auto isRed = [&](const Image& im, double u, double v) {
            const float* q = px(im, u, v);
            return q[0] > 0.95f && q[1] < 0.05f && q[2] < 0.05f;
        };
        // Remove Background: the red track shows through around him.
        clip.effects.push_back(makeEffect(p, "remove_background"));
        Image shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(isRed(shown, 0.03, 0.03) && isRed(shown, 0.95, 0.1));
        QVERIFY(same(shown, 0.62, 0.24) && same(shown, 0.6, 0.5));
        // Keep the background instead: he is gone.
        clip.effects.back().params["keep"] = Param(1.0);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(isRed(shown, 0.62, 0.24) && same(shown, 0.03, 0.03));
        // A wider edge reaches further into the backdrop.
        clip.effects.back().params["keep"] = Param(0.0);
        auto alphaSum = [&](const Image& im) {
            double sum = 0;
            for (int y = 0; y < im.height; ++y)
                for (int x = 0; x < im.width; ++x) sum += 1 - im.at(x, y)[0] + im.at(x, y)[1];  // not red
            return sum;
        };
        const double base = alphaSum(renderSequenceFrame(p, s, 0, {}));
        clip.effects.back().params["shift"] = Param(8.0);
        QVERIFY(alphaSum(renderSequenceFrame(p, s, 0, {})) > base * 1.02);
        clip.effects.clear();
        // A People mask: an effect on him only, and inverted on everything but him.
        Effect dark = makeEffect(p, "color_correct");
        dark.params["exposure"] = Param(-10.0);
        dark.params["mask.shape"] = Param(4.0);
        dark.params["mask.feather"] = Param(2.0);
        clip.effects.push_back(dark);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(px(shown, 0.62, 0.24)[0] < 0.02f && same(shown, 0.03, 0.03));
        clip.effects.back().params["mask.invert"] = Param(1.0);
        shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(same(shown, 0.62, 0.24));
        clip.effects.clear();

        // Through MCP.
        const QString project = QString::fromStdString(path("cutout.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_add_effect"},
                                                     {"arguments", QJsonObject{{"project", project}, {"clip", double(clip.id)}, {"effect", "remove_background"}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back, &err));
        QVERIFY(isRed(renderSequenceFrame(back, *back.active(), 0, {}), 0.03, 0.03));
    }

    void slateFromSpeech() {
        const QByteArray whisper = qgetenv("MONTAGE_TEST_WHISPER_MODEL");
        if (!ttsAvailable() || !ttsModel().installed() || whisper.isEmpty())
            QSKIP("Set MONTAGE_TTS_MODEL and MONTAGE_TEST_WHISPER_MODEL to test slates from speech");
        // A take that opens with its slate called out, then the line.
        std::vector<float> audio;
        std::string err;
        QVERIFY2(synthesizeSpeech("Scene twelve, take three. Action! The meeting starts at nine.", "am_michael", 1.0, audio, &err), err.c_str());
        const std::string wav = path("slate.wav");
        QVERIFY(writeSpeechWav(wav, audio, &err));
        Project p = makeDefaultProject();
        MediaItem m = probeOrFail(p, wav);
        TranscribeOptions opts;
        opts.model = whisper.toStdString();
        Transcript t;
        QVERIFY2(transcribeMedia(wav, opts, t, {}, nullptr, &err), err.c_str());
        m.transcript = std::make_shared<const Transcript>(t);
        p.media.push_back(m);
        MediaItem plain = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");  // no slate in it
        Transcript jfk;
        QVERIFY(transcribeMedia(plain.path, opts, jfk, {}, nullptr, &err));
        plain.transcript = std::make_shared<const Transcript>(jfk);
        p.media.push_back(plain);
        QCOMPARE(logFromSlates(p), 1);
        QCOMPARE(QString::fromStdString(p.media[0].metadata["scene"]), QString("12"));
        QCOMPARE(QString::fromStdString(p.media[0].metadata["take"]), QString("3"));
        QVERIFY(p.media[1].metadata["scene"].empty());
        // Over MCP, on a saved project.
        p.media[0].metadata.clear();
        const QString project = QString::fromStdString(path("slate.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_log_media"},
                                                     {"arguments", QJsonObject{{"project", project}, {"media", "slate.wav"}, {"from_slate", true}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("from_slates").toInt(), 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(QString::fromStdString(back.media[0].metadata["scene"]), QString("12"));
        QCOMPARE(QString::fromStdString(back.media[0].metadata["take"]), QString("3"));
    }

    void paperEdit() {
        Project p = makeDefaultProject();
        p.active()->fps = {25, 1};
        MediaItem speech = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.words = {{0.3, 0.5, "And", 1}, {0.5, 0.7, "so,", 1}, {0.8, 1.0, "my", 1}, {1.0, 1.3, "fellow", 1}, {1.3, 2.0, "Americans,", 1},
                     {3.0, 3.3, "ask", 1}, {3.3, 3.6, "not", 1}, {3.6, 3.8, "what", 1}, {3.8, 4.0, "your", 1}, {4.0, 4.5, "country", 1},
                     {6.0, 6.3, "ask", 1}, {6.3, 6.5, "what", 1}, {6.5, 6.7, "you", 1}, {6.7, 6.9, "can", 1}, {6.9, 7.1, "do", 1}};
        t->segments = {seg};
        speech.transcript = t;
        p.media.push_back(speech);
        // Quotes found in the transcript, ignoring case and punctuation.
        const auto a = findLine(p, speech.id, "my fellow americans");
        QVERIFY(a && a->in == 0.8 && a->out == 2.0);
        QCOMPARE(QString::fromStdString(a->text), QString("my fellow Americans,"));
        const auto first = findLine(p, speech.id, "ask");
        const auto second = findLine(p, speech.id, "ask what", 0);
        QVERIFY(first && second && first->in == 3.0 && second->in == 6.0);
        QVERIFY(findLine(p, speech.id, "ask", 5)->in == 6.0);
        QVERIFY(!findLine(p, speech.id, "ask what your country"));
        // Laid out in the order given, with 0.1 s of air.
        const size_t sequences = p.sequences.size();
        const Id id = makePaperEdit(p, {*second, *a}, "Quotes");
        QVERIFY(id);
        QCOMPARE(p.sequences.size(), sequences + 1);
        const Sequence* s = p.findSequence(id);
        QVERIFY(s && s->name == "Quotes" && p.active()->id != id);
        const auto& clips = s->audioTracks.at(0).clips;
        QCOMPARE(clips.size(), size_t(2));
        QCOMPARE(FrameTime(clips[0].sourceIn), FrameTime(std::round(5.9 * 25)));
        QCOMPARE(clips[0].duration, FrameTime(std::round(6.6 * 25) - std::round(5.9 * 25)));  // "ask what", with its air
        QCOMPARE(FrameTime(clips[1].sourceIn), FrameTime(std::round(0.7 * 25)));
        QCOMPARE(clips[1].start, clips[0].end());
        QVERIFY(!makePaperEdit(p, {}, "Nothing"));

        // Over MCP: quotes and a range.
        const QString project = QString::fromStdString(path("paper.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_paper_edit"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project},
                              {"lines", QJsonArray{QJsonObject{{"media", "jfk.wav"}, {"text", "Ask not what your country"}},
                                                   QJsonObject{{"media", "jfk.wav"}, {"from", 0.3}, {"to", 0.7}}}},
                              {"handle", 0.0}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("lines").toArray().size(), 2);
        QVERIFY(std::abs(r.value("structuredContent").toObject().value("duration_seconds").toDouble() - 1.9) < 0.05);
        r = call({{"project", project}, {"lines", QJsonArray{QJsonObject{{"media", "jfk.wav"}, {"text", "never said"}}}}});
        QVERIFY(r.value("isError").toBool());
    }

    void mcpSwapsClips() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        Clip a = makeGeneratorClip(p, "color", 30), b = makeGeneratorClip(p, "color", 45);
        b.start = 30;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, a).ok && edit::overwrite(p, s, {TrackKind::Video, 0}, b).ok);
        const Id first = s.videoTracks[0].clips[0].id, second = s.videoTracks[0].clips[1].id;
        const QString project = QString::fromStdString(path("swap.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_swap_clip"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"clip", double(first)}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(edit::clipById(*back.active(), second)->start, FrameTime(0));
        QCOMPARE(edit::clipById(*back.active(), first)->start, FrameTime(45));
        QVERIFY(call({{"project", project}, {"clip", double(first)}, {"with", "next"}}).value("isError").toBool());
        QVERIFY(call({{"project", project}, {"clip", double(first)}, {"with", "sideways"}}).value("isError").toBool());
        QVERIFY(!call({{"project", project}, {"clip", double(first)}, {"with", "previous"}}).value("isError").toBool());
    }

    void blemishRemover() {
        if (!faceSearchAvailable() || !faceModel().installed()) QSKIP("Set MONTAGE_FACE_MODEL to the YuNet and SFace models");
        const std::string still = MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg";
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(still, &err), err.c_str());
        const Image clean = toImage(*dec.frameAt(0));
        const auto faces = cachedFaces(clean);
        QVERIFY(faces && faces->size() == 1);
        const FaceBox f = faces->front();
        const int W = clean.width, H = clean.height;
        const double fw = f.w * W;
        const std::vector<float> skin = faceSkinMask(f, W, H);
        // Four dark spots (5 px across, 4 % of the face) on the cheeks, forehead and chin.
        const double lx = f.landmarks[0] * W, ly = f.landmarks[1] * H, rx = f.landmarks[2] * W, ry = f.landmarks[3] * H;
        const double mx = (f.landmarks[6] + f.landmarks[8]) / 2 * W, my = (f.landmarks[7] + f.landmarks[9]) / 2 * H;
        const std::vector<std::pair<double, double>> spots = {
            {lx * 0.55 + mx * 0.45 - 0.16 * fw, ly * 0.45 + my * 0.55},
            {rx * 0.55 + mx * 0.45 + 0.08 * fw, ry * 0.45 + my * 0.55},
            {(lx + rx) / 2, (ly + ry) / 2 - 0.28 * fw},
            {mx, my + 0.16 * fw}};
        for (auto [x, y] : spots) QVERIFY2(skin[size_t(y) * size_t(W) + size_t(x)] > 0.6f, qPrintable(QString("%1,%2").arg(x).arg(y)));
        Image spotted = clean;
        for (auto [cx, cy] : spots)
            for (int y = int(cy) - 4; y <= int(cy) + 4; ++y)
                for (int x = int(cx) - 4; x <= int(cx) + 4; ++x) {
                    const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    const float k = float(std::clamp(3.0 - d, 0.0, 1.0)) * 0.45f;  // 2.5 px radius, soft edge
                    float* p = spotted.at(x, y);
                    p[0] *= 1 - k * 0.8f, p[1] *= 1 - k, p[2] *= 1 - k;
                }
        auto luma = [](const Image& im, int x, int y) {
            const float* q = im.at(x, y);
            return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
        };
        // How much darker a spot's centre is than the ring round it.
        auto contrast = [&](const Image& im, double cx, double cy) {
            double in = 0, ring = 0;
            int ni = 0, nr = 0;
            for (int y = int(cy) - 7; y <= int(cy) + 7; ++y)
                for (int x = int(cx) - 7; x <= int(cx) + 7; ++x) {
                    const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    if (d < 1.5) in += luma(im, x, y), ++ni;
                    else if (d > 5 && d < 7) ring += luma(im, x, y), ++nr;
                }
            return ring / nr - in / ni;
        };
        Image healed = spotted;
        const int found = removeBlemishes(healed, *faces, BlemishSettings{});
        QVERIFY2(found >= 4, qPrintable(QString::number(found)));
        for (auto [x, y] : spots) {
            const double before = contrast(spotted, x, y), after = contrast(healed, x, y), natural = contrast(clean, x, y);
            QVERIFY2(before > natural + 0.05, qPrintable(QString("%1 %2").arg(before).arg(natural)));
            QVERIFY2(after - natural < 0.25 * (before - natural), qPrintable(QString("spot at %1,%2: %3 -> %4 (clean %5)").arg(x).arg(y).arg(before).arg(after).arg(natural)));
        }
        // Eyes, mouth and the backdrop untouched.
        for (auto [ex, ey] : {std::pair{lx, ly}, std::pair{rx, ry}, std::pair{mx, my}})
            for (int y = int(ey) - 3; y <= int(ey) + 3; ++y)
                for (int x = int(ex) - 3; x <= int(ex) + 3; ++x)
                    for (int c = 0; c < 3; ++c) QCOMPARE(healed.at(x, y)[c], spotted.at(x, y)[c]);
        for (auto [x, y] : {std::pair{10, 10}, std::pair{300, 30}, std::pair{160, 400}})
            for (int c = 0; c < 4; ++c) QCOMPARE(healed.at(x, y)[c], spotted.at(x, y)[c]);
        // The clean face mostly as it was (its own few marks aside).
        Image cleanHealed = clean;
        removeBlemishes(cleanHealed, *faces, BlemishSettings{});
        int changed = 0, inFace = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (skin[size_t(y) * size_t(W) + size_t(x)] < 0.5f) continue;
                ++inFace;
                changed += std::abs(luma(cleanHealed, x, y) - luma(clean, x, y)) > 0.03;
            }
        QVERIFY2(changed < inFace / 25, qPrintable(QString("%1 of %2").arg(changed).arg(inFace)));
        // No amount, no change; Show Spots marks them in red.
        Image none = spotted;
        BlemishSettings off;
        off.amount = 0;
        removeBlemishes(none, *faces, off);
        QCOMPARE(none.px, spotted.px);
        Image shown = spotted;
        BlemishSettings show;
        show.showSpots = true;
        removeBlemishes(shown, *faces, show);
        QVERIFY(shown.at(int(spots[0].first), int(spots[0].second))[0] > 0.9f);

        // As an effect on a clip of the spotted picture.
        QImage q(W, H, QImage::Format_RGBA8888);
        toRgba8(spotted, q.bits(), size_t(q.bytesPerLine()));
        const QString png = QString::fromStdString(path("spotted.png"));
        QVERIFY(q.save(png));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = W, s.height = H;
        MediaItem m = probeOrFail(p, png.toStdString());
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[0].clips.at(0);
        QVERIFY(findEffectInfo("blemish_remover"));
        clip.effects.push_back(makeEffect(p, "blemish_remover"));
        QVERIFY(needsFaces(clip.effects.back()));
        const Image frame = renderSequenceFrame(p, s, 0, {});
        for (auto [x, y] : spots) QVERIFY2(contrast(frame, x, y) < contrast(spotted, x, y) * 0.5, qPrintable(QString("%1,%2").arg(x).arg(y)));
    }

    void faceRefinement() {
        if (!faceSearchAvailable() || !faceModel().installed()) QSKIP("Set MONTAGE_FACE_MODEL to the YuNet and SFace models");
        const std::string still = MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg";
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(still, &err), err.c_str());
        const Image img = toImage(*dec.frameAt(0));
        auto faces = cachedFaces(img);
        QVERIFY(faces);
        QCOMPARE(int(faces->size()), 1);
        const FaceBox f = faces->front();
        // Where OpenCV's YuNet puts it (77, 88, 123 x 156 of 320 x 415).
        QVERIFY(std::abs(f.x * 320 - 77) < 4 && std::abs(f.w * 320 - 123) < 4 && std::abs(f.y * 415 - 88) < 4);
        QVERIFY(cachedFaces(img) == faces);
        // The skin mask: the cheeks in, the eyes, mouth and backdrop out.
        const std::vector<float> mask = faceSkinMask(f, 320, 415);
        auto at = [&](double x, double y) { return mask[size_t(y) * 320 + size_t(x)]; };
        const double cheekX = (f.landmarks[0] * 0.5 + f.landmarks[6] * 0.5) * 320, cheekY = (f.landmarks[1] * 0.4 + f.landmarks[7] * 0.6) * 415;
        QVERIFY2(at(cheekX, cheekY) > 0.9f, qPrintable(QString::number(at(cheekX, cheekY))));
        QCOMPARE(at(f.landmarks[0] * 320, f.landmarks[1] * 415), 0.0f);
        QCOMPARE(at((f.landmarks[6] + f.landmarks[8]) / 2 * 320, (f.landmarks[7] + f.landmarks[9]) / 2 * 415), 0.0f);
        QCOMPARE(at(10, 10), 0.0f);

        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 415;
        MediaItem m = probeOrFail(p, still);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[0].clips.at(0);
        const Image plain = renderSequenceFrame(p, s, 0, {});
        auto texture = [](const Image& im, double cx, double cy) {  // mean Laplacian over 13 x 13
            auto l = [&](int x, int y) {
                const float* q = im.at(x, y);
                return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
            };
            double sum = 0;
            for (int y = int(cy) - 6; y <= int(cy) + 6; ++y)
                for (int x = int(cx) - 6; x <= int(cx) + 6; ++x)
                    sum += std::abs(4 * l(x, y) - l(x - 1, y) - l(x + 1, y) - l(x, y - 1) - l(x, y + 1));
            return sum / 169;
        };
        auto luma = [](const Image& im, double x, double y) {
            const float* q = im.at(int(x), int(y));
            return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
        };
        Effect refine = makeEffect(p, "face_refine");
        refine.params["smooth"] = Param(100.0);
        refine.params["eyes_bright"] = Param(100.0);
        clip.effects.push_back(refine);
        const Image done = renderSequenceFrame(p, s, 0, {});
        // Smoother skin, brighter eyes, the backdrop and suit untouched.
        QVERIFY2(texture(done, cheekX, cheekY) < 0.7 * texture(plain, cheekX, cheekY),
                 qPrintable(QString("%1 vs %2").arg(texture(done, cheekX, cheekY)).arg(texture(plain, cheekX, cheekY))));
        const double eyeX = f.landmarks[0] * 320, eyeY = f.landmarks[1] * 415;
        auto eye = [&](const Image& im) {  // the eye's mean luma over 5 x 5
            double sum = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) sum += luma(im, eyeX + dx, eyeY + dy);
            return sum / 25;
        };
        QVERIFY2(eye(done) > eye(plain) * 1.08, qPrintable(QString("%1 vs %2").arg(eye(done)).arg(eye(plain))));
        for (auto [x, y] : {std::pair{10, 10}, std::pair{300, 30}, std::pair{160, 400}})
            for (int c = 0; c < 4; ++c) QCOMPARE(done.at(x, y)[c], plain.at(x, y)[c]);
        // Show Face Mask.
        clip.effects.back().params["show"] = Param(1.0);
        const Image shown = renderSequenceFrame(p, s, 0, {});
        QVERIFY(luma(shown, cheekX, cheekY) > 0.5);
        QVERIFY(luma(shown, eyeX, eyeY) < 0.05);
        clip.effects.clear();

        // Through MCP.
        const QString project = QString::fromStdString(path("refine.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_add_effect"},
                                                     {"arguments", QJsonObject{{"project", project}, {"clip", double(clip.id)}, {"effect", "face_refine"},
                                                                               {"params", QJsonObject{{"smooth", 80}}}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
    }

    void textToSpeech() {
        if (!ttsAvailable() || !ttsModel().installed()) QSKIP("Set MONTAGE_TTS_MODEL to the Kokoro speech pack");
        // Phonemes as misaki itself gives them (checked against misaki 0.7.4 with spaCy).
        const std::pair<const char*, const char*> us[] = {
            {"Hello, world! This is Montage, a video editor.", "həlˈO, wˈɜɹld! ðˌɪs ɪz mɑntˈɑʒ, ɐ vˈɪdiO ˈɛdəɾəɹ."},
            {"It costs $4.50 or £3, about 75% of 1,200 units.",
             "ˌɪt kˈɔsts fˈɔɹ dˈɑləɹz ænd fˈɪfti sˈɛnts ɔɹ θɹˈi pˈWndz, əbˈWt sˈɛvənti fˈIv pəɹsˈɛnt ʌv wˈʌn θˈWzᵊnd tˈu hˈʌndɹəd jˈunəts."},
            {"Version 2.1.3 shipped in the 1990s; the 21st century began in 2001.",
             "vˈɜɹʒən tˈu wˈʌn θɹˈi ʃˈɪpt ɪn ðə nˌIntˈin nˈIndiz; ðə twˈɛnti fˈɜɹst sˈɛnʧəɹi bəɡˈæn ɪn tˈu θˈWzᵊnd wˈʌn."},
            {"NASA and the U.S. sent 3 men. Dr. Smith said: \"It works!\"",
             "nˈæsə ænd ðə jˌuˈɛs sˈɛnt θɹˈi mˈɛn. dˈɑktəɹ smˈɪθ sˈɛd: “ˌɪt wˈɜɹks!”"},
            {"An AI voice reads it to everyone in 12.5 seconds.", "ɐn ˈAˌI vˈYs ɹˈidz ɪt tʊ ˈɛvɹiwən ɪn twˈɛlv pYnt fˈIv sˈɛkəndz."}};
        std::string ps, err;
        for (const auto& [text, want] : us) {
            QVERIFY2(textToPhonemes(text, false, ps, &err), err.c_str());
            QCOMPARE(QString::fromStdString(ps), QString::fromUtf8(want));
        }
        QVERIFY(textToPhonemes("Good evening from London. The weather is rather grey today.", true, ps, &err));
        QCOMPARE(QString::fromStdString(ps), QString::fromUtf8("ɡˈʊd ˈiːvnɪŋ fɹɒm lˈʌndən. ðə wˈɛðə ɪz ɹˈɑːðə ɡɹˈA tədˈA."));
        // A word the dictionaries lack, split into ones they know.
        QVERIFY(textToPhonemes("voiceover", false, ps, &err));
        QCOMPARE(QString::fromStdString(ps), QString::fromUtf8("vˈYsˌOvəɹ"));

        // Speech: about as long as it takes to say, loud enough, slower when asked.
        std::vector<float> audio, slow;
        QVERIFY2(synthesizeSpeech("Hello and welcome to Montage.", "af_heart", 1.0, audio, &err), err.c_str());
        const double seconds = double(audio.size()) / kTtsSampleRate;
        QVERIFY2(seconds > 1.2 && seconds < 4, qPrintable(QString::number(seconds)));
        float peak = 0;
        for (float v : audio) peak = std::max(peak, std::abs(v));
        QVERIFY(peak > 0.1f && peak <= 1.0f);
        QVERIFY(synthesizeSpeech("Hello and welcome to Montage.", "bm_george", 0.7, slow, &err));
        QVERIFY(slow.size() > audio.size() * 1.2);
        QVERIFY(!synthesizeSpeech("Hello", "nobody", 1, audio, &err));
        const std::string wav = path("tts.wav");
        QVERIFY(synthesizeSpeech("Hello and welcome to Montage.", "af_heart", 1.0, audio, &err));
        QVERIFY(writeSpeechWav(wav, audio, &err));
        // Heard back by Whisper as what was written.
        if (const QByteArray model = qgetenv("MONTAGE_TEST_WHISPER_MODEL"); !model.isEmpty()) {
            TranscribeOptions opts;
            opts.model = model.toStdString();
            Transcript t;
            QVERIFY2(transcribeMedia(wav, opts, t, {}, nullptr, &err), err.c_str());
            const QString heard = QString::fromStdString(t.text()).toLower();
            QVERIFY2(heard.contains("hello") && heard.contains("welcome") && heard.contains("montage"), qPrintable(heard));
        }

        // Through MCP: text at a time, and a caption track cue by cue.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        CaptionTrack track;
        track.id = p.newId();
        track.captions = {{0, 60, "First line of the script.", {}}, {90, 150, "And the second one.", {}}};
        s.captionTracks.push_back(track);
        const QString project = QString::fromStdString(path("speech.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_generate_speech"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"captions", 0}, {"voice", "bf_emma"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("clips").toArray().size(), 2);
        r = call({{"project", project}, {"text", "The end."}, {"at", 10}, {"track", "A2"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(call({{"project", project}, {"text", "x"}, {"voice", "nobody"}}).value("isError").toBool());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back, &err));
        const Sequence& bs = *back.active();
        QCOMPARE(int(bs.audioTracks[0].clips.size()), 2);
        QCOMPARE(bs.audioTracks[0].clips[0].start, FrameTime(0));
        QCOMPARE(bs.audioTracks[0].clips[1].start, FrameTime(90));
        // Each fits before the next cue.
        QVERIFY(bs.audioTracks[0].clips[0].end() <= 92);
        QCOMPARE(int(bs.audioTracks[1].clips.size()), 1);
        QCOMPARE(bs.audioTracks[1].clips[0].start, FrameTime(std::llround(10 * bs.fpsValue())));
        QVERIFY(QFileInfo::exists(QFileInfo(project).absolutePath() + "/Voiceover"));
    }

    void objectRemoval() {
        if (!inpaintAvailable() || !inpaintModel().installed()) QSKIP("Set MONTAGE_INPAINT_MODEL to the LaMa model");
        // A red disc on a soft, striped background: removed, the background comes back.
        const int W = 320, H = 240;
        Image clean(W, H), withDisc(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float g = 0.35f + 0.25f * float(x) / W + 0.05f * std::sin(y * 0.4f);
                float* c = clean.at(x, y);
                c[0] = g, c[1] = g * 0.9f + 0.05f, c[2] = 0.6f - 0.2f * float(y) / H, c[3] = 1;
                std::copy_n(c, 4, withDisc.at(x, y));
            }
        std::vector<float> mask(size_t(W) * H, 0.0f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double r = std::hypot(x - 160.0, y - 120.0);
                if (r < 22) {
                    float* c = withDisc.at(x, y);
                    c[0] = 0.95f, c[1] = 0.05f, c[2] = 0.05f;
                }
                if (r < 26) mask[size_t(y) * W + x] = 1;
            }
        auto error = [&](const Image& im) {  // mean difference from the clean picture inside the disc
            double sum = 0;
            int n = 0;
            for (int y = 98; y < 142; ++y)
                for (int x = 138; x < 182; ++x)
                    if (std::hypot(x - 160.0, y - 120.0) < 22)
                        for (int c = 0; c < 3; ++c) sum += std::abs(im.at(x, y)[c] - clean.at(x, y)[c]), ++n;
            return sum / n;
        };
        Image out;
        std::string err;
        QVERIFY2(inpaint(withDisc, mask, out, &err), err.c_str());
        qInfo("inside the disc: %.4f from the background before, %.4f after", error(withDisc), error(out));
        QVERIFY(error(out) < error(withDisc) / 8);
        QVERIFY(error(out) < 0.05);
        // Outside the mask, nothing changes.
        for (auto [x, y] : {std::pair{10, 10}, std::pair{300, 200}, std::pair{100, 120}})
            for (int c = 0; c < 4; ++c) QCOMPARE(out.at(x, y)[c], withDisc.at(x, y)[c]);
        // Nothing masked, nothing done.
        QVERIFY(inpaint(withDisc, std::vector<float>(size_t(W) * H, 0.0f), out, &err));
        QVERIFY(out.px == withDisc.px);

        // On the photo, through the effect: the flag on his shoulder painted out, white suit in its place.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 400;
        MediaItem m = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/faces/armstrong.jpg");
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = s.videoTracks[0].clips.at(0);
        const Image plain = renderSequenceFrame(p, s, 0, {});
        Effect removal = makeEffect(p, "object_removal");
        removal.params["mask.shape"] = Param(2.0);
        removal.params["mask.x"] = Param(0.84);
        removal.params["mask.y"] = Param(0.465);
        removal.params["mask.w"] = Param(0.2);
        removal.params["mask.h"] = Param(0.11);
        removal.params["mask.feather"] = Param(3.0);
        clip.effects.push_back(removal);
        const Image done = renderSequenceFrame(p, s, 0, {});
        auto stats = [](const Image& im, int x0, int y0, int x1, int y1, double& red, double& sat) {
            red = sat = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x, ++n) {
                    const float* q = im.at(x, y);
                    red += q[0] - (q[1] + q[2]) / 2;
                    sat += std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]});
                }
            red /= n, sat /= n;
        };
        double redBefore, satBefore, redAfter, satAfter;
        stats(plain, 250, 175, 290, 195, redBefore, satBefore);
        stats(done, 250, 175, 290, 195, redAfter, satAfter);
        qInfo("flag area: saturation %.3f before, %.3f after", satBefore, satAfter);
        QVERIFY(satAfter < satBefore / 2 && redAfter < 0.05);
        for (auto [x, y] : {std::pair{60, 60}, std::pair{160, 300}, std::pair{100, 200}})
            for (int c = 0; c < 4; ++c) QCOMPARE(done.at(x, y)[c], plain.at(x, y)[c]);
        // Without a mask it does nothing.
        clip.effects.back().params["mask.shape"] = Param(0.0);
        QVERIFY(renderSequenceFrame(p, s, 0, {}).px == plain.px);
        clip.effects.clear();

        // Through MCP.
        const QString project = QString::fromStdString(path("removal.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_add_effect"},
                                                     {"arguments", QJsonObject{{"project", project}, {"clip", double(clip.id)}, {"effect", "object_removal"},
                                                                               {"params", QJsonObject{{"mask.shape", 1}, {"mask.x", 0.84}, {"mask.y", 0.465},
                                                                                                      {"mask.w", 0.22}, {"mask.h", 0.12}}}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
    }

    void peopleSearch() {
        if (!faceSearchAvailable() || !faceModel().installed()) QSKIP("Set MONTAGE_FACE_MODEL to the YuNet and SFace models");
        std::string err;
        auto model = FaceModel::load(&err);
        QVERIFY2(model, err.c_str());
        auto still = [&](const char* name) {
            VideoDecoder dec;
            const std::string f = std::string(MONTAGE_TEST_DATA_DIR "/faces/") + name;
            if (!dec.open(f, &err)) return Frame16Ptr();
            return dec.frameAt(0, dec.displayWidth(), dec.displayHeight(), true);
        };
        // One face each, where OpenCV's own YuNet finds it (x, y, w, h at 320 px wide).
        struct Ref { const char* file; float box[4]; };
        const Ref refs[] = {{"jfk-color.jpg", {77, 88, 123, 156}},
                            {"jfk-looking-up.jpg", {54, 67, 182, 218}},
                            {"armstrong.jpg", {170, 76, 53, 70}}};
        std::vector<std::vector<float>> ids;
        for (const Ref& r : refs) {
            Frame16Ptr f = still(r.file);
            QVERIFY2(f, err.c_str());
            QCOMPARE(f->width, 320);
            const auto faces = model->detect(*f);
            QCOMPARE(int(faces.size()), 1);
            const DetectedFace& d = faces[0];
            QVERIFY(d.score > 0.85f);
            const float got[4] = {d.x, d.y, d.w, d.h};
            for (int i = 0; i < 4; ++i)
                QVERIFY2(std::abs(got[i] - r.box[i]) <= 4, qPrintable(QString("%1 %2: %3 vs %4").arg(r.file).arg(i).arg(got[i]).arg(r.box[i])));
            // The eyes sit in the top half of the box, the mouth below the nose.
            QVERIFY(d.landmarks[1] < d.y + d.h * 0.6f && d.landmarks[9] > d.landmarks[5]);
            ids.push_back(model->embed(*f, d));
            QCOMPARE(int(ids.back().size()), 128);
        }
        auto cosine = [](const std::vector<float>& a, const std::vector<float>& b) {
            float s = 0;
            for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
            return s;
        };
        QVERIFY(std::abs(cosine(ids[0], ids[0]) - 1) < 1e-3f);
        QVERIFY2(cosine(ids[0], ids[1]) > 0.5f, qPrintable(QString::number(cosine(ids[0], ids[1]))));
        QVERIFY(cosine(ids[0], ids[2]) < 0.3f && cosine(ids[1], ids[2]) < 0.3f);

        // Indexed in a project: Kennedy twice, Armstrong once.
        Project p = makeDefaultProject();
        for (const Ref& r : refs) {
            const std::string f = std::string(MONTAGE_TEST_DATA_DIR "/faces/") + r.file;
            MediaItem m = probeOrFail(p, f);
            FaceIndex index;
            QVERIFY2(indexFaces(f, 0, index, 0, 8, 32, {}, nullptr, &err), err.c_str());
            QCOMPARE(int(index.faces.size()), 1);
            QCOMPARE(index.model, faceModel().id);
            QVERIFY(std::abs(index.faces[0].x - r.box[0] / 320.0f) < 0.02f);
            m.faces = std::make_shared<const FaceIndex>(index);
            p.media.push_back(m);
        }
        QCOMPARE(groupPeople(p), 2);
        const auto people = peopleIn(p);
        QCOMPARE(int(people.size()), 2);
        QCOMPARE(people[0].faces, 2);
        QCOMPARE(people[0].media, 2);
        QCOMPARE(people[1].faces, 1);
        const int jfk = people[0].id, neil = people[1].id;
        QCOMPARE(p.media[0].faces->faces[0].person, jfk);
        QCOMPARE(p.media[1].faces->faces[0].person, jfk);
        QCOMPARE(p.media[2].faces->faces[0].person, neil);
        QCOMPARE(QString::fromStdString(personName(p, neil)), QString("Person %1").arg(neil));

        // Names and ids survive grouping again, and saving.
        for (Person& person : p.people)
            if (person.id == neil) person.name = "Neil Armstrong";
        QCOMPARE(groupPeople(p), 2);
        QCOMPARE(p.media[2].faces->faces[0].person, neil);
        QCOMPARE(personName(p, neil), std::string("Neil Armstrong"));
        const auto moments = findPerson(p, jfk);
        QCOMPARE(int(moments.size()), 2);
        QCOMPARE(moments[0].media, p.media[0].id);
        QCOMPARE(moments[1].media, p.media[1].id);
        QVERIFY(findPerson(p, 999).empty());

        FaceIndex back;
        QVERIFY(faceIndexFromJson(faceIndexToJson(*p.media[0].faces), back));
        QCOMPARE(back.faces.size(), p.media[0].faces->faces.size());
        QVERIFY(back == *p.media[0].faces);
        const std::string file = path("people.montage");
        QVERIFY(saveProject(p, file));
        Project loaded;
        QVERIFY(loadProject(file, loaded, &err));
        QCOMPARE(int(loaded.people.size()), 2);
        QVERIFY(loaded.people == p.people);
        QVERIFY(loaded.media[2].faces && *loaded.media[2].faces == *p.media[2].faces);
        QCOMPARE(personName(loaded, neil), std::string("Neil Armstrong"));

        // Who is in what: the bin's search and a smart bin rule.
        QCOMPARE(peopleSeen(&p, p.media[2]), std::vector<std::string>{"Neil Armstrong"});
        QVERIFY(mediaMatchesSearch(p.media[2], "armstrong", &p));
        QVERIFY(!mediaMatchesSearch(p.media[0], "armstrong", &p));
        SmartBin bin;
        bin.rules.push_back({"people", "includes", "neil armstrong"});
        QCOMPARE(smartBinMedia(p, bin), std::vector<Id>{p.media[2].id});
        bin.rules[0] = {"people", "!empty", ""};
        QCOMPARE(int(smartBinMedia(p, bin).size()), 3);

        // A new face joins the person it looks like; people already found keep theirs.
        auto unsorted = std::make_shared<FaceIndex>(*p.media[1].faces);
        unsorted->faces[0].person = 0;
        p.media[1].faces = unsorted;
        QCOMPARE(groupPeople(p), 2);
        QCOMPARE(p.media[1].faces->faces[0].person, jfk);
        // Merged by hand, they stay merged; starting again splits them.
        QVERIFY(renamePerson(p, jfk, "JFK"));
        QVERIFY(!renamePerson(p, 999, "Nobody"));
        QVERIFY(mergePeople(p, neil, jfk));
        QCOMPARE(int(p.people.size()), 1);
        QCOMPARE(p.media[2].faces->faces[0].person, jfk);
        QCOMPARE(personName(p, jfk), std::string("JFK"));
        QVERIFY(!mergePeople(p, neil, jfk));
        QCOMPARE(groupPeople(p), 1);
        QCOMPARE(groupPeople(p, 0.42f, true), 2);
        QCOMPARE(p.media[0].faces->faces[0].person, jfk);
        QVERIFY(p.media[2].faces->faces[0].person != jfk);
        // A smart bin naming someone follows them to a new name, and into a merge.
        const int armstrong = p.media[2].faces->faces[0].person;
        SmartBin named;
        named.id = p.newId();
        named.rules.push_back({"people", "includes", personName(p, armstrong)});
        p.smartBins.push_back(named);
        QVERIFY(renamePerson(p, armstrong, "Buzz"));
        QCOMPARE(p.smartBins.back().rules[0].value, std::string("Buzz"));
        QCOMPARE(smartBinMedia(p, p.smartBins.back()), std::vector<Id>{p.media[2].id});
        QVERIFY(mergePeople(p, armstrong, jfk));
        QCOMPARE(p.smartBins.back().rules[0].value, std::string("JFK"));

        // Through MCP: found, named and joined.
        const QString project = QString::fromStdString(path("people-mcp.montage"));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonArray stills;
        for (const Ref& r : refs) stills.append(QString(MONTAGE_TEST_DATA_DIR "/faces/") + r.file);
        QJsonObject r = call("montage_create_project", {{"project", project}, {"media", stills}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = call("montage_find_people", {{"project", project}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QJsonArray found = r.value("structuredContent").toObject().value("people").toArray();
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].toObject().value("clips").toInt(), 2);
        const int first = found[0].toObject().value("id").toInt(), second = found[1].toObject().value("id").toInt();
        r = call("montage_name_person", {{"project", project}, {"person", first}, {"name", "John Kennedy"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = call("montage_find_people", {{"project", project}, {"person", "john kennedy"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonArray seen = r.value("structuredContent").toObject().value("moments").toArray();
        QCOMPARE(seen.size(), 2);
        QVERIFY(seen[0].toObject().value("media").toString().contains("jfk"));
        QVERIFY(call("montage_find_people", {{"project", project}, {"person", "Nobody"}}).value("isError").toBool());
        r = call("montage_name_person", {{"project", project}, {"person", second}, {"same_as", "John Kennedy"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project afterMcp;
        QVERIFY(loadProject(project.toStdString(), afterMcp, &err));
        QCOMPARE(int(peopleIn(afterMcp).size()), 1);
        QCOMPARE(peopleIn(afterMcp)[0].name, std::string("John Kennedy"));
    }

    void visualSearch() {
        // The index on its own: 8-bit samples keep similarities, and survive saving.
        VisualIndex vi;
        vi.model = "test";
        vi.step = 1;
        std::vector<float> a(512), b(512);
        for (int i = 0; i < 512; ++i) {
            a[size_t(i)] = float(std::sin(i * 0.37));
            b[size_t(i)] = float(std::cos(i * 0.11));
        }
        auto normalise = [](std::vector<float>& v) {
            double l = 0;
            for (float x : v) l += double(x) * x;
            for (float& x : v) x = float(x / std::sqrt(l));
        };
        normalise(a);
        normalise(b);
        vi.add(0.5, a);
        vi.add(1.5, b);
        QVERIFY(std::fabs(vi.similarity(0, a) - 1.f) < 0.002f);
        double dot = 0;
        for (int i = 0; i < 512; ++i) dot += double(a[size_t(i)]) * b[size_t(i)];
        QVERIFY(std::fabs(vi.similarity(1, a) - float(dot)) < 0.01f);
        VisualIndex back;
        QVERIFY(visualIndexFromJson(visualIndexToJson(vi), back) && back == vi);

        if (!visualSearchAvailable()) QSKIP("Built without ONNX Runtime");
        if (!visualModel().installed()) QSKIP("Set MONTAGE_VISUAL_MODEL to a folder with the CLIP model to run the rest");
        std::string err;
        auto clip = ClipModel::load(&err);
        QVERIFY2(clip, err.c_str());
        // CLIP's tokenizer, against the reference implementation's ids.
        const std::vector<std::pair<std::string, std::vector<int64_t>>> tokens = {
            {"a photo of a dog", {49406, 320, 1125, 539, 320, 1929, 49407}},
            {"A red car, driving FAST!!", {49406, 320, 736, 1615, 267, 4161, 1953, 748, 49407}},
            {"Café au lait — 1990's \"quotes\"", {49406, 15304, 2566, 572, 585, 2005, 272, 280, 280, 271, 568, 257, 5808, 257, 49407}},
            {"  two   spaces\tand tab", {49406, 1237, 9006, 537, 14724, 49407}},
            {"running runners ran", {49406, 2761, 10571, 4031, 49407}},
            {"Ünïcödé naïve résumé 日本語",
             {49406, 6522, 77, 35689, 66, 7255, 67, 4166, 1097, 35689, 563, 29106, 7054, 4166, 39121, 44353, 34002, 508, 49407}},
            {"it's we'll they're I'M", {49406, 585, 568, 649, 1342, 889, 982, 328, 880, 49407}},
            {"a2b3 c4d 12345", {49406, 320, 273, 321, 274, 322, 275, 323, 272, 273, 274, 275, 276, 49407}},
        };
        for (const auto& [text, ids] : tokens) {
            const auto got = clip->tokens(text);
            QString shown;
            for (int64_t id : got) shown += QString::number(id) + " ";
            QVERIFY2(got == ids, qPrintable(QString::fromStdString(text) + ": " + shown));
        }
        QCOMPARE(int(clip->tokens(std::string(400, 'a') + " b c d e f g h i j k l m n o p q r s t u v w x y z").size()) <= 77, true);

        // Footage: 4 s of a red scene, then 4 s of a blue one.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        for (int k = 0; k < 2; ++k) {
            Clip c = makeGeneratorClip(gen, "color", 100);
            c.generator.params["color.r"] = Param(k == 0 ? 0.85 : 0.05);
            c.generator.params["color.g"] = Param(k == 0 ? 0.08 : 0.15);
            c.generator.params["color.b"] = Param(k == 0 ? 0.06 : 0.9);
            c.start = k * 100;
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
        }
        ExportSettings st;
        st.path = path("colours.mp4");
        st.audioCodec = "none";
        st.preset = "ultrafast";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        VisualIndex index;
        double last = 0;
        QVERIFY2(indexVideo(st.path, 0, index, 0, [&](double f) { last = f; }, nullptr, &err), err.c_str());
        QCOMPARE(int(index.samples.size()), 8);  // every second for a short clip
        QCOMPARE(last, 1.0);
        QCOMPARE(index.model, std::string("clip-vit-b32"));
        Project p = makeDefaultProject();
        MediaItem m = probeOrFail(p, st.path);
        m.visual = std::make_shared<const VisualIndex>(index);
        p.media.push_back(m);
        for (const auto& [query, from, to] : {std::tuple{"a red image", 0.0, 4.0}, std::tuple{"a blue image", 4.0, 8.0}}) {
            const auto q = clip->text(query, &err);
            QCOMPARE(int(q.size()), 512);
            const auto hits = findShots(p, q);
            QVERIFY(!hits.empty());
            QVERIFY2(hits[0].best >= from && hits[0].best <= to, qPrintable(QString("%1: %2").arg(query).arg(hits[0].best)));
            QVERIFY2(hits[0].start >= from - 0.6 && hits[0].end <= to + 0.6,
                     qPrintable(QString("%1: %2-%3").arg(query).arg(hits[0].start).arg(hits[0].end)));
            QCOMPARE(hits[0].media, m.id);
        }
        // Cancelling stops without a result.
        std::atomic<bool> stop{true};
        QVERIFY(!indexVideo(st.path, 0, index, 0, {}, &stop, &err));

        // Auto-tag labels: one unit embedding per label, each nearest its own descriptions.
        const LabelEmbeddings labels = clip->labels(&err);
        QVERIFY2(labels.size() == tagCategories().size(), err.c_str());
        for (size_t c = 0; c < labels.size(); ++c) {
            QCOMPARE(labels[c].size(), tagCategories()[c].labels.size());
            for (size_t l = 0; l < labels[c].size(); ++l) {
                double len = 0;
                for (float x : labels[c][l]) len += double(x) * x;
                QVERIFY(std::fabs(len - 1) < 1e-3);
                const auto own = clip->text(tagCategories()[c].labels[l].prompts.front(), &err);
                auto dot = [&](const std::vector<float>& a) {
                    double d = 0;
                    for (size_t i = 0; i < a.size(); ++i) d += double(a[i]) * own[i];
                    return d;
                };
                for (size_t o = 0; o < labels[c].size(); ++o)
                    if (o != l) QVERIFY(dot(labels[c][l]) > dot(labels[c][o]));
            }
        }
        // Footage indexed from those descriptions gets those tags.
        VisualIndex described;
        described.step = 1;
        for (int k = 0; k < 4; ++k) described.add(k, clip->text("an extreme close-up shot", &err));
        QCOMPARE(autoTags(described, labels).keywords.front(), std::string("Close-up"));
    }

    void autoDuckMusic() {
        // Dialogue: speech at 2-4 s, then 6-6.5 and 6.8-7.3 s (one pause too short to come back up), and a 0.1 s knock at 10 s.
        const std::string talk = path("duck-dialogue.wav"), music = path("duck-music.wav");
        writeVoiceWav(talk, 12, 220, [](double t) { return (t >= 2 && t < 4) || (t >= 6 && t < 6.5) || (t >= 6.8 && t < 7.3) || (t >= 10 && t < 10.1); },
                      [](double) { return false; });
        writeWav(music, 48000, 12, 0.3f, 0.3f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem dm = probeOrFail(p, talk), mm = probeOrFail(p, music);
        p.media.push_back(dm);
        p.media.push_back(mm);
        const TrackRef A1{TrackKind::Audio, 0}, A2{TrackKind::Audio, 1};
        QVERIFY(edit::placeMedia(p, s, dm.id, 0, 0, -1, {TrackKind::Video, 0}, A1, false).ok);
        QVERIFY(edit::placeMedia(p, s, mm.id, 0, 0, -1, {TrackKind::Video, 0}, A2, false).ok);
        const Id musicClip = trackAt(s, A2)->clips.front().id;

        DuckOptions o;
        std::string err;
        Spans spans = dialogueSpans(p, s, {0}, o, &err);
        QVERIFY2(err.empty(), err.c_str());
        QString shown;
        for (const auto& [a, b] : spans) shown += QString("[%1, %2] ").arg(a).arg(b);
        QCOMPARE(int(spans.size()), 2);
        QVERIFY2(std::fabs(spans[0].first - 2) < 0.06 && std::fabs(spans[0].second - 4) < 0.06, qPrintable(shown));
        QVERIFY2(std::fabs(spans[1].first - 6) < 0.06 && std::fabs(spans[1].second - 7.3) < 0.06, qPrintable(shown));

        // The music dips from its own level, fading down before and up after.
        Clip& mc = *edit::clipById(s, musicClip);
        mc.audio.params["gain_db"] = Param(-3);
        QVERIFY(duckClip(mc, s, spans, o));
        const Param& g = mc.audio.params.at("gain_db");
        auto at = [&](double sec) { return g.at(FrameTime(std::llround(sec * 25))); };
        QVERIFY(std::fabs(at(1) + 3) < 1e-6);
        QVERIFY(std::fabs(at(3) + 18) < 1e-6);
        QVERIFY(std::fabs(at(5) + 3) < 1e-6);
        QVERIFY(std::fabs(at(6.6) + 18) < 1e-6);  // the short pause stays down
        QVERIFY(std::fabs(at(11) + 3) < 1e-6);   // the knock does not duck
        QVERIFY2(std::fabs(at(4.4) + 10.5) < 1.0, qPrintable(QString::number(at(4.4))));  // halfway up
        QVERIFY2(at(1.85) < -3 && at(1.85) > -18, qPrintable(QString::number(at(1.85))));  // on the way down
        QVERIFY(!duckClip(mc, s, spans, o));  // the same again changes nothing
        // Nothing to duck under: the keys go, the level stays.
        QVERIFY(duckClip(mc, s, {}, o));
        QVERIFY(!mc.audio.params.at("gain_db").animated());
        QCOMPARE(mc.audio.params.at("gain_db").value, -3.0);

        // A transcript's words mark speech instead, following where the clip sits and starts.
        Transcript t;
        t.segments.push_back({2, 4, "well then", {{2.0, 2.5, "well", 1, {}}, {3.5, 4.0, "then", 1, {}}}, -1});
        p.findMedia(dm.id)->transcript = std::make_shared<const Transcript>(t);
        Clip& dc = trackAt(s, A1)->clips.front();
        dc.start = 25;  // one second later on the timeline
        spans = dialogueSpans(p, s, {0}, o, &err);
        QCOMPARE(int(spans.size()), 1);  // the 1 s gap is shorter than the fades need
        QVERIFY(std::fabs(spans[0].first - 3) < 1e-6 && std::fabs(spans[0].second - 5) < 1e-6);
        o.useTranscripts = false;
        QVERIFY(dialogueSpans(p, s, {0}, o, &err).size() >= 2);  // loudness again
        o.useTranscripts = true;
        QVERIFY(dialogueSpans(p, s, {1}, o, &err).size() == 1);  // the music track is loud throughout
        trackAt(s, A1)->muted = true;
        QVERIFY(dialogueSpans(p, s, {0}, o, &err).empty());
        trackAt(s, A1)->muted = false;

        // The MCP tool.
        const QString project = QString::fromStdString(path("duck.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_auto_duck"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call(QJsonObject{{"project", project}, {"music_track", "A2"}, {"dialogue_tracks", QJsonArray{"A1"}}, {"amount_db", -20}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("clips_changed").toInt(), 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Param& bg = edit::clipById(*back.active(), musicClip)->audio.params.at("gain_db");
        QVERIFY(std::fabs(bg.at(4 * 25) + 23) < 1e-6);  // -3 dB, down 20
        r = call(QJsonObject{{"project", project}, {"music_track", "V1"}, {"dialogue_tracks", QJsonArray{"A1"}}});
        QVERIFY(r.value("isError").toBool());
        r = call(QJsonObject{{"project", project}, {"music_track", "A2"}});
        QVERIFY(r.value("isError").toBool());
    }

    void mcpBuildsACutFromAScript() {
        // Two takes of a two-line scene; the second take fluffs line one.
        Project p = makeDefaultProject();
        auto take = [&](const char* name, std::vector<TranscriptWord> words) {
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Video;
            m.name = name;
            m.path = path((std::string(name) + ".mp4").c_str());
            m.hasVideo = m.hasAudio = true;
            m.duration = 30;
            auto t = std::make_shared<Transcript>();
            TranscriptSegment seg;
            seg.words = std::move(words);
            t->segments.push_back(seg);
            m.transcript = t;
            p.media.push_back(m);
            return m.id;
        };
        const Id t1 = take("take1", {{1.0, 1.3, "Where", 1}, {1.4, 1.6, "were", 1}, {1.7, 1.9, "you", 1}, {2.0, 2.4, "last", 1},
                                     {2.5, 2.9, "night?", 1}, {4.0, 4.3, "Out.", 1}, {4.4, 4.7, "Walking.", 1}});
        const Id t2 = take("take2", {{1.0, 1.3, "Where", 1}, {1.4, 1.6, "were", 1}, {1.7, 1.9, "uh", 1}, {2.0, 2.2, "you", 1},
                                     {2.3, 2.7, "last", 1}, {2.8, 3.2, "night?", 1}});
        const QString project = QString::fromStdString(path("scene.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        // The scene as a Final Draft file.
        const QString fdx = QString::fromStdString(path("scene.fdx"));
        {
            QFile f(fdx);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"(<?xml version="1.0" encoding="UTF-8"?><FinalDraft DocumentType="Script" Version="5"><Content>
<Paragraph Type="Scene Heading"><Text>INT. HALL - NIGHT</Text></Paragraph>
<Paragraph Type="Character"><Text>ANNA</Text></Paragraph>
<Paragraph Type="Dialogue"><Text>Where were you </Text><Text>last night?</Text></Paragraph>
<Paragraph Type="Character"><Text>BEN</Text></Paragraph>
<Paragraph Type="Parenthetical"><Text>(shrugs)</Text></Paragraph>
<Paragraph Type="Dialogue"><Text>Out. Walking.</Text></Paragraph>
<Paragraph Type="Action"><Text>She turns away.</Text></Paragraph>
</Content></FinalDraft>)");
        }
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_script_cut"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        // A dry run reports the lines and their best readings, and changes nothing.
        QJsonObject r = call(QJsonObject{{"project", project}, {"script_file", fdx}, {"dry_run", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QJsonArray lines = r.value("structuredContent").toObject().value("lines").toArray();
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0].toObject().value("speaker").toString(), QString("Anna"));
        QCOMPARE(lines[0].toObject().value("takes").toInt(), 2);
        QCOMPARE(lines[0].toObject().value("best").toObject().value("media").toString(), QString("take1"));
        QCOMPARE(lines[1].toObject().value("speaker").toString(), QString("Ben"));
        QCOMPARE(lines[1].toObject().value("best").toObject().value("start").toDouble(), 4.0);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.sequences.size(), size_t(1));
        // The build: a new active sequence with both lines on V1, take 2 above line one.
        r = call(QJsonObject{{"project", project}, {"script_file", fdx}, {"name", "Scene 4"}, {"handle", 0.0}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("placed").toInt(), 2);
        QCOMPARE(r.value("structuredContent").toObject().value("alternates").toInt(), 1);
        QVERIFY(loadProject(project.toStdString(), back));
        const Sequence& s = *back.active();
        QCOMPARE(s.name, std::string("Scene 4"));
        QCOMPARE(s.videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(s.videoTracks[0].clips[0].mediaId, t1);
        QCOMPARE(s.videoTracks[0].clips[0].sourceIn, 1.0 * s.fpsValue());
        QCOMPARE(s.videoTracks[1].clips.size(), size_t(1));
        QCOMPARE(s.videoTracks[1].clips[0].mediaId, t2);
        QVERIFY(!s.videoTracks[1].clips[0].enabled);
        QCOMPARE(s.markers.size(), size_t(2));
        // Nothing to go on: an error, not an empty sequence.
        r = call(QJsonObject{{"project", project}, {"script", "Lines nobody ever said."}});
        QVERIFY(r.value("isError").toBool());
    }

    void autoBroll() {
        if (!visualSearchAvailable() || !visualModel().installed()) QSKIP("Set MONTAGE_VISUAL_MODEL to the CLIP model");
        std::string err;
        // Two cutaway shots made from the portraits, three seconds each, and a plain "talking head".
        auto still = [&](const char* file, const char* image, FrameTime frames) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 320;
            gs.height = 240;
            gs.fps = {25, 1};
            if (image) {
                MediaItem m = probeOrFail(gen, std::string(MONTAGE_TEST_DATA_DIR "/faces/") + image);
                gen.media.push_back(m);
                edit::placeMedia(gen, gs, m.id, 0, 0, double(frames), {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            } else {
                Clip c = makeGeneratorClip(gen, "color", frames);
                c.generator.params["color.r"] = c.generator.params["color.g"] = c.generator.params["color.b"] = Param(0.5);
                edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            }
            ExportSettings st = findExportPreset("H.264 - Fast Draft")->settings;
            st.path = path(file);
            st.audioCodec = "none";
            if (!exportSequence(gen, gs, st, nullptr, nullptr, &err)) qFatal("%s", err.c_str());
            return st.path;
        };
        const std::string moon = still("broll-astronaut.mp4", "armstrong.jpg", 75), office = still("broll-office.mp4", "jfk-color.jpg", 75),
                          head = still("broll-head.mp4", nullptr, 300);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 240;
        s.fps = {25, 1};
        std::vector<Id> cutaways;
        for (const std::string& f : {moon, office}) {
            MediaItem m = probeOrFail(p, f);
            VisualIndex index;
            QVERIFY2(indexVideo(f, 0, index, 0, {}, nullptr, &err), err.c_str());
            m.visual = std::make_shared<const VisualIndex>(index);
            p.media.push_back(m);
            cutaways.push_back(m.id);
        }
        MediaItem talk = probeOrFail(p, head);
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        auto words = [&](const char* text, double at) {
            for (const QString& w : QString(text).split(' ')) {
                seg.words.push_back({at, at + 0.3, w.toStdString(), 1});
                at += 0.35;
            }
        };
        words("An astronaut in a white space suit stood in front of the moon.", 1.0);
        words("Then the president in a dark suit smiled at his desk.", 6.0);
        t->segments.push_back(seg);
        talk.transcript = t;
        p.media.push_back(talk);
        QVERIFY(edit::placeMedia(p, s, talk.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto clip = ClipModel::load(&err);
        QVERIFY2(clip, err.c_str());
        BrollOptions o;
        o.coverage = 1;
        const std::vector<BrollPick> picks = planBroll(p, s, cutaways, [&](const std::string& x) { return clip->text(x); }, o, &err);
        QVERIFY2(picks.size() == 2, err.c_str());
        for (const BrollPick& b : picks) qInfo("\"%s\" -> %llu (%.3f)", b.sentence.c_str(), (unsigned long long)b.media, b.score);
        // Each sentence gets the shot it describes, over its own words.
        QCOMPARE(picks[0].media, cutaways[0]);
        QCOMPARE(picks[1].media, cutaways[1]);
        QCOMPARE(picks[0].at, FrameTime(25));
        QCOMPARE(picks[1].at, FrameTime(150));
        QVERIFY(picks[0].length <= 75 && picks[0].length >= 37);
        // Placed on V2, picture only; the talking head is untouched and no sound is added.
        size_t audioBefore = 0;
        for (const Track& a : s.audioTracks) audioBefore += a.clips.size();
        const edit::Result r = placeBroll(p, s, picks, 1);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(s.videoTracks[1].clips.size(), size_t(2));
        size_t audioAfter = 0;
        for (const Track& a : s.audioTracks) audioAfter += a.clips.size();
        QCOMPARE(audioAfter, audioBefore);
        QCOMPARE(s.videoTracks[0].clips.size(), size_t(1));
        // At half coverage, only the better match.
        o.coverage = 0.5;
        QCOMPARE(planBroll(p, s, cutaways, [&](const std::string& x) { return clip->text(x); }, o, &err).size(), size_t(1));
        // Over MCP, choosing from the footage the sequence does not use.
        Project q = makeDefaultProject();
        Sequence& qs = *q.active();
        qs.fps = {25, 1};
        for (const MediaItem& m : p.media) q.media.push_back(m);
        QVERIFY(edit::placeMedia(q, qs, talk.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("broll.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_auto_broll"}, {"arguments", QJsonObject{{"project", project}, {"coverage", 1.0}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!res.value("isError").toBool(), QJsonDocument(res).toJson().constData());
        QCOMPARE(res.value("structuredContent").toObject().value("cutaways").toArray().size(), 2);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->videoTracks[1].clips.size(), size_t(2));
    }

    void autoHighlights() {
        // Thirty seconds of grey: a square moves only from 8 to 12 s, and the sound bursts from 20 to 23 s.
        std::string err;
        std::vector<float> sound(size_t(48000) * 30);
        std::mt19937 rng(3);
        std::normal_distribution<float> hiss(0, 0.003f);
        for (size_t i = 0; i < sound.size(); ++i) {
            const double t = double(i) / 48000;
            sound[i] = hiss(rng) + (t >= 20 && t < 23 ? float(0.5 * std::sin(2 * M_PI * 440 * t)) : 0.0f);
        }
        const std::string wav = path("highlight-sound.wav");
        QVERIFY(writeMonoWav(wav, sound, 48000));
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        Clip bg = makeGeneratorClip(gen, "color", 750);
        bg.generator.params["color.r"] = bg.generator.params["color.g"] = bg.generator.params["color.b"] = Param(0.4);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, bg);
        Clip sq = makeGeneratorClip(gen, "shape", 750);
        sq.generator.params["width"] = sq.generator.params["height"] = Param(40.0);
        sq.generator.params["pos_x"].addKey(0, -100.0);
        sq.generator.params["pos_x"].addKey(200, -100.0);
        sq.generator.params["pos_x"].addKey(300, 100.0);
        sq.generator.params["pos_x"].addKey(749, 100.0);
        edit::overwrite(gen, gs, {TrackKind::Video, 1}, sq);
        MediaItem audio = probeOrFail(gen, wav);
        gen.media.push_back(audio);
        QVERIFY(edit::placeMedia(gen, gs, audio.id, 0, 0, -1, {TrackKind::Video, 2}, {TrackKind::Audio, 0}, false).ok);
        ExportSettings st = findExportPreset("Apple ProRes 422 HQ")->settings;
        st.path = path("highlight-footage.mov");
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());

        Project p = makeDefaultProject();
        MediaItem m = probeOrFail(p, st.path);
        p.media.push_back(m);
        HighlightOptions o;
        o.seconds = 7;
        o.minLength = 2;
        o.maxLength = 4;
        const std::vector<HighlightMoment> moments = findHighlights(p, {m.id}, o, {}, nullptr, &err);
        QVERIFY2(!moments.empty(), err.c_str());
        double total = 0;
        bool motion = false, loud = false;
        for (const HighlightMoment& h : moments) {
            qInfo("highlight %.1f to %.1f s (score %.2f)", h.in, h.out, h.score);
            total += h.out - h.in;
            motion |= h.in < 12 && h.out > 8;
            loud |= h.in < 23 && h.out > 20;
            // Nothing from the still, quiet stretches.
            QVERIFY(!(h.out <= 7.5) && !(h.in >= 13 && h.out <= 19.5) && !(h.in >= 24));
        }
        QVERIFY(motion && loud);
        QVERIFY2(total >= 6 && total <= 8.5, qPrintable(QString::number(total)));
        for (size_t i = 1; i < moments.size(); ++i) QVERIFY(moments[i].in >= moments[i - 1].out);
        // Laid out in a new sequence, back to back with their sound.
        const Id seq = makeHighlightSequence(p, moments);
        QVERIFY(seq);
        const Sequence* hs = p.findSequence(seq);
        QCOMPARE(hs->videoTracks[0].clips.size(), moments.size());
        QCOMPARE(hs->audioTracks[0].clips.size(), moments.size());
        QVERIFY(std::fabs(double(hs->duration()) / hs->fpsValue() - total) < 0.2);
        // Over MCP.
        Project q = makeDefaultProject();
        q.media.push_back(m);
        const QString project = QString::fromStdString(path("highlights.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_highlights"}, {"arguments", QJsonObject{{"project", project}, {"seconds", 7}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!res.value("isError").toBool(), QJsonDocument(res).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.sequences.size(), size_t(2));
        QCOMPARE(back.sequences.back().name, std::string("Highlights"));
    }

    void cutToTheBeat() {
        // Twenty bars at 128 BPM (a bar every 1.875 s), from half a second in.
        const double lead = 0.5;
        const std::vector<int> chords = {0, 0, 1, 2, 1, 2, 0, 3, 0, 3, 1, 2, 1, 2, 0, 3, 0, 3, 3, 3};
        const std::string wav = path("cut-song.wav");
        QVERIFY(writeMonoWav(wav, testSong(48000, lead, chords), 48000));
        // A four-second video, a one-second one (too short for a bar) and a still.
        std::string err;
        auto video = [&](const char* name, FrameTime frames, float red) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", frames);
            c.generator.params["color.r"] = Param(double(red));
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st = findExportPreset("H.264 - Fast Draft")->settings;
            st.path = path(name);
            st.audioCodec = "none";
            if (!exportSequence(gen, gs, st, nullptr, nullptr, &err)) qFatal("%s", err.c_str());
            return st.path;
        };
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        const double fps = s.fpsValue();
        auto add = [&](const std::string& file) {
            MediaItem m = probeOrFail(p, file);
            p.media.push_back(m);
            return m.id;
        };
        const Id song = add(wav), longer = add(video("beat-4s.mp4", FrameTime(std::lround(4 * fps)), 0.9f)),
                 shorter = add(video("beat-1s.mp4", FrameTime(std::lround(1 * fps)), 0.2f)), still = add(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg");
        QVERIFY(edit::placeMedia(p, s, song, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Clip music = s.audioTracks[0].clips.at(0);
        BeatGrid g;
        QVERIFY2(mediaBeats(p, song, g, nullptr, &err), err.c_str());
        const edit::Result r = cutToBeat(p, s, music, g, {longer, shorter, still}, 1, true, 0);
        QVERIFY2(r.ok, r.error.c_str());
        const auto& clips = s.videoTracks[0].clips;
        QCOMPARE(clips.size(), r.created.size());
        QVERIFY2(clips.size() >= 18, qPrintable(QString::number(clips.size())));
        // From the first bar, a bar each, back to back, to the end of the music.
        QVERIFY2(std::fabs(double(clips.front().start) / fps - lead) < 0.05, qPrintable(QString::number(clips.front().start)));
        for (size_t i = 0; i < clips.size(); ++i) {
            if (i + 1 < clips.size()) {
                QCOMPARE(clips[i].end(), clips[i + 1].start);
                QVERIFY2(std::fabs(double(clips[i].duration) / fps - kSongBar) < 0.06, qPrintable(QString::number(clips[i].duration)));
            }
            // The one-second video never fits a bar; the four-second one and the still take turns.
            QCOMPARE(clips[i].mediaId, i % 2 ? still : longer);
        }
        QVERIFY(std::abs(clips.back().end() - music.end()) <= 1);
        // The video's middle is used, and no sound comes with the pictures.
        QVERIFY(std::fabs(clips[0].sourceIn - std::floor((4 * fps - double(clips[0].duration)) / 2)) <= 1);
        QCOMPARE(s.audioTracks[0].clips.size(), size_t(1));
        // Over MCP: every two bars.
        Project q = makeDefaultProject();
        Sequence& qs = *q.active();
        for (const MediaItem& m : p.media) q.media.push_back(m);
        QVERIFY(edit::placeMedia(q, qs, song, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("cut-to-beat.montage"));
        QVERIFY(saveProject(q, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_cut_to_beat"},
                                                     {"arguments", QJsonObject{{"project", project}, {"clip", double(qs.audioTracks[0].clips[0].id)},
                                                                               {"media", QJsonArray{double(longer), double(still)}}, {"every", 2}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!res.value("isError").toBool(), QJsonDocument(res).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const auto& two = back.active()->videoTracks[0].clips;
        QVERIFY(two.size() >= 9 && two.size() <= 11);
        QVERIFY(std::fabs(double(two[0].duration) / fps - 2 * kSongBar) < 0.06);
    }

    void mcpMarksTheBeatAndFitsMusic() {
        const double lead = 0.5;
        const std::vector<int> chords = {0, 0, 1, 2, 1, 2, 0, 3, 0, 3, 1, 2, 1, 2, 0, 3, 0, 3, 3, 3};
        const std::string wav = path("mcp-song.wav");
        QVERIFY(writeMonoWav(wav, testSong(48000, lead, chords), 48000));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        m.id = p.newId();
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id clip = s.audioTracks[0].clips.at(0).id;
        const QString project = QString::fromStdString(path("song.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const char* tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", tool}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_beat_markers", {{"project", project}, {"clip", double(clip)}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(std::fabs(r.value("structuredContent").toObject().value("tempo").toDouble() - 128) < 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QVERIFY(back.active()->markers.size() >= chords.size());
        r = call("montage_fit_music", {{"project", project}, {"clip", double(clip)}, {"seconds", 30}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const double secs = r.value("structuredContent").toObject().value("seconds").toDouble();
        QVERIFY(std::fabs(secs - 30) <= kSongBar / 2);
        QVERIFY(loadProject(project.toStdString(), back));
        const auto& pieces = back.active()->audioTracks[0].clips;
        QVERIFY(pieces.size() >= 2);
        QCOMPARE(back.active()->audioTracks[0].transitions.size(), pieces.size() - 1);
        // Too short a length is refused.
        r = call("montage_fit_music", {{"project", project}, {"clip", double(pieces[0].id)}, {"seconds", 2}});
        QVERIFY(r.value("isError").toBool());
    }

    void mcpRemovesRetakes() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.name = "piece to camera";
        m.path = path("retakes.mp4");
        m.hasVideo = m.hasAudio = true;
        m.duration = 20;
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        // "Welcome back to the, uh, welcome back to the channel."
        seg.words = {{1.0, 1.3, "Welcome", 1}, {1.4, 1.6, "back", 1}, {1.7, 1.8, "to", 1}, {1.9, 2.0, "the,", 1}, {2.3, 2.5, "uh,", 1},
                     {3.0, 3.3, "welcome", 1}, {3.4, 3.6, "back", 1}, {3.7, 3.8, "to", 1}, {3.9, 4.0, "the", 1}, {4.1, 4.6, "channel.", 1}};
        t->segments.push_back(seg);
        m.transcript = t;
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, 250, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("retakes.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_cut_speech"}, {"arguments", QJsonObject{{"project", project}, {"retakes", true}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject sc = r.value("structuredContent").toObject();
        QCOMPARE(sc.value("retakes").toInt(), 1);
        // From the first "Welcome" (1.0 s) to the second (3.0 s).
        QVERIFY2(std::fabs(sc.value("removed_seconds").toDouble() - 2.0) < 0.05, QJsonDocument(sc).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const auto words = sequenceTranscriptWords(back, *back.active());
        // The kept take only: "welcome back to the channel."
        QCOMPARE(int(words.size()), 5);
        QCOMPARE(words[0].text, std::string("welcome"));
        QCOMPARE(words[4].text, std::string("channel."));
    }

    void mcpCutsBySpeech() {
        // An interview clip with a filler, a long pause and a phrase to lose.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.name = "interview";
        m.path = path("interview.mp4");
        m.hasVideo = m.hasAudio = true;
        m.duration = 20;
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.words = {{1.0, 1.3, "So", 1},     {1.4, 1.7, "um", 1},    {1.8, 2.1, "I", 1},      {2.2, 2.6, "think", 1},
                     {5.0, 5.4, "we", 1},     {5.5, 5.9, "should", 1}, {6.0, 6.3, "go.", 1},   {6.4, 6.7, "You", 1},
                     {6.8, 7.1, "know,", 1},  {7.2, 7.6, "really.", 1}};
        t->segments.push_back(seg);
        m.transcript = t;
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, 250, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const QString project = QString::fromStdString(path("speech.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_cut_speech"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        // A phrase that is never said changes nothing.
        QJsonObject r = call(QJsonObject{{"project", project}, {"phrases", QJsonArray{"never said"}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("removed_seconds").toDouble(), 0.0);
        // "um" (1.4 s to "I" at 1.8 s), the pause after "think" (to 0.3 s), and "you know" (to "really." at 7.2 s).
        r = call(QJsonObject{{"project", project}, {"phrases", QJsonArray{"you know"}}, {"fillers", true},
                             {"pauses_longer_than", 1.0}, {"smooth_cuts", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject sc = r.value("structuredContent").toObject();
        QCOMPARE(sc.value("fillers").toInt(), 1);
        QCOMPARE(sc.value("pauses").toInt(), 1);
        QCOMPARE(sc.value("phrases").toArray().at(0).toObject().value("times").toInt(), 1);
        QCOMPARE(sc.value("smooth_cuts").toInt(), 3);
        QVERIFY2(std::fabs(sc.value("removed_seconds").toDouble() - 82 / 25.0) < 1e-9, qPrintable(QString::number(sc.value("removed_seconds").toDouble())));
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Sequence& bs = *back.active();
        QCOMPARE(bs.duration(), FrameTime(250 - 82));
        QCOMPARE(bs.videoTracks[0].clips.size(), size_t(4));
        QCOMPARE(bs.audioTracks[0].clips.size(), size_t(4));
        QCOMPARE(bs.videoTracks[0].transitions.size(), size_t(3));
        QVERIFY(bs.audioTracks[0].transitions.empty());
        // What is left reads "So I think we should go. really."
        std::string said;
        for (const TranscriptWord& w : sequenceTranscriptWords(back, bs)) said += (said.empty() ? "" : " ") + w.text;
        QCOMPARE(QString::fromStdString(said), QString("So I think we should go. really."));
        // Without a transcript there is nothing to go on.
        Project bare = makeDefaultProject();
        const QString empty = QString::fromStdString(path("bare.montage"));
        QVERIFY(saveProject(bare, empty.toStdString()));
        QVERIFY(call(QJsonObject{{"project", empty}, {"fillers", true}}).value("isError").toBool());
    }

    void mcpMatchesColour() {
        // A warm, lifted hero shot and a flat shot of the same scene.
        QImage flat(320, 180, QImage::Format_RGB32), hero(320, 180, QImage::Format_RGB32);
        std::mt19937 rng(4);
        std::uniform_int_distribution<int> px(0, 300), sz(6, 30), col(10, 245);
        flat.fill(QColor(110, 110, 110));
        {
            QPainter pa(&flat);
            for (int i = 0; i < 200; ++i) pa.fillRect(px(rng), px(rng) * 180 / 300, sz(rng), sz(rng), QColor(col(rng), col(rng), col(rng)));
        }
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                const QRgb c = flat.pixel(x, y);
                hero.setPixel(x, y, qRgb(std::min(255, 20 + qRed(c) * 9 / 10), qGreen(c), qBlue(c) * 3 / 4));
            }
        const std::string flatPng = path("mcp-flat.png"), heroPng = path("mcp-hero.png");
        QVERIFY(flat.save(QString::fromStdString(flatPng)) && hero.save(QString::fromStdString(heroPng)));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem hm = probeOrFail(p, heroPng), fm = probeOrFail(p, flatPng);
        p.media.push_back(hm);
        p.media.push_back(fm);
        QVERIFY(edit::placeMedia(p, s, hm.id, 0, 0, 25, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::placeMedia(p, s, fm.id, 25, 0, 25, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id target = s.videoTracks[0].clips.at(1).id;
        const QString project = QString::fromStdString(path("match.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_match_color"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call(QJsonObject{{"project", project}, {"reference_at", 0.4}, {"clips", QJsonArray{double(target)}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("matched").toInt(), 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Clip* c = edit::clipById(*back.active(), target);
        QVERIFY(c && c->effects.size() == 1 && c->effects[0].strings.count("match"));
        // The flat shot now renders like the hero shot.
        RenderOptions o;
        o.displaySpace = "rec709";
        const Image a = renderProgramFrame(back, *back.active(), 10, o), b = renderProgramFrame(back, *back.active(), 35, o);
        double d = 0;
        for (size_t i = 0; i < a.px.size(); i += 4)
            for (int k = 0; k < 3; ++k) d += std::fabs(a.px[i + k] - b.px[i + k]);
        d /= double(a.px.size() / 4 * 3);
        QVERIFY2(d < 0.01, qPrintable(QString::number(d)));
        QVERIFY(call(QJsonObject{{"project", project}, {"reference_at", 0.4}, {"clips", QJsonArray{}}}).value("isError").toBool());
    }

    void exportBurnIns() {
        // Grey picture, 160 x 90; a review copy with timecode and text top left and a red logo bottom right.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        s.fps = {25, 1};
        Clip c = makeGeneratorClip(p, "color", 10);
        c.generator.params["color.r"] = c.generator.params["color.g"] = c.generator.params["color.b"] = Param(0.5);
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        QImage logo(40, 20, QImage::Format_ARGB32);
        logo.fill(QColor(255, 0, 0));
        const QString logoPath = QString::fromStdString(path("logo.png"));
        QVERIFY(logo.save(logoPath));
        ExportSettings st = findExportPreset("H.264 - High Quality")->settings;
        st.path = path("review.mp4");
        st.audioCodec = "none";
        st.crf = 10;
        st.burnIn.timecode = true;
        st.burnIn.text = "DRAFT";
        st.burnIn.size = 0.12;
        st.burnIn.watermark = logoPath.toStdString();
        st.burnIn.watermarkOpacity = 1.0;
        st.burnIn.watermarkWidth = 0.25;
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        VideoDecoder dec;
        QVERIFY2(dec.open(st.path, &err), err.c_str());
        const Image img = toImage(*dec.frameAt(0.2));
        // Top left: white text on a dark box; bottom right: red; the middle: grey as it was.
        float brightest = 0, darkest = 1;
        for (int y = 3; y < 30; ++y)
            for (int x = 5; x < 70; ++x) {
                brightest = std::max(brightest, img.at(x, y)[1]);
                darkest = std::min(darkest, img.at(x, y)[1]);
            }
        QVERIFY2(brightest > 0.8f && darkest < 0.35f, qPrintable(QString("%1 %2").arg(brightest).arg(darkest)));
        const float* red = img.at(160 - 5 - 10, 90 - 3 - 5);
        QVERIFY2(red[0] > 0.7f && red[1] < 0.3f, qPrintable(QString("%1 %2").arg(red[0]).arg(red[1])));
        const float* mid = img.at(80, 50);
        QVERIFY(std::fabs(mid[1] - 0.5f) < 0.08f);
        // A missing logo is an error, not a silent copy without it.
        st.burnIn.watermark = path("no-such-logo.png");
        QVERIFY(!exportSequence(p, s, st, nullptr, nullptr, &err));
        QVERIFY(QString::fromStdString(err).contains("watermark"));

        // Over MCP.
        const QString project = QString::fromStdString(path("review.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_render"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call(QJsonObject{{"project", project}, {"output", QString::fromStdString(path("review-mcp.mp4"))},
                                         {"burn_in", QJsonObject{{"timecode", true}, {"watermark", logoPath}, {"corner", "bottom_left"}}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(QFileInfo(QString::fromStdString(path("review-mcp.mp4"))).size() > 0);
        r = call(QJsonObject{{"project", project}, {"output", QString::fromStdString(path("x.mp4"))}, {"burn_in", QJsonObject{{"corner", "middle"}}}});
        QVERIFY(r.value("isError").toBool());
    }

    void mediaPoolKeepsOnlyOpenFiles() {
        // Two files decoded; then only the first belongs to the open project, then none (a project closed).
        const std::string a = path("pool-a.mp4"), b = path("pool-b.mp4");
        writeBallVideo(a, 6);
        writeBallVideo(b, 6);
        MediaPool& pool = MediaPool::instance();
        pool.clear();
        pool.setOpenFiles(std::nullopt);
        QVERIFY(pool.videoFrame(a, 0.0, 64, 36));
        QVERIFY(pool.videoFrame(b, 0.0, 64, 36));
        QCOMPARE(pool.openDecoders(), size_t(2));
        // b's idle decoder is closed at once, and its next one as soon as it is released.
        pool.setOpenFiles(std::set<std::string>{a});
        QCOMPARE(pool.openDecoders(), size_t(1));
        QVERIFY(pool.videoFrame(b, 0.1, 64, 36));
        QCOMPARE(pool.openDecoders(), size_t(1));
        pool.setOpenFiles(std::set<std::string>{});
        QCOMPARE(pool.openDecoders(), size_t(0));
        // No limit again: decoders are kept for reuse.
        pool.setOpenFiles(std::nullopt);
        QVERIFY(pool.videoFrame(b, 0.2, 64, 36));
        QCOMPARE(pool.openDecoders(), size_t(1));
        pool.clear();
    }

    void mcpCompareSequences() {
        // Two versions of a cut of the same (not decoded) file: the second trims the first shot and adds a third.
        Project p = makeDefaultProject();
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.name = "shot.mov";
        m.path = path("shot.mov");
        m.duration = 20;
        m.width = 1920, m.height = 1080;
        m.fps = {30, 1};
        m.hasVideo = true;
        p.media.push_back(m);
        Sequence& v1 = *p.active();
        v1.name = "Cut 1";
        QVERIFY(edit::placeMedia(p, v1, m.id, 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::placeMedia(p, v1, m.id, 60, 100, 160, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Sequence v2 = v1;
        v2.id = p.newId();
        v2.name = "Cut 2";
        v2.videoTracks[0].clips[0].duration = 40;  // out 20 earlier
        QVERIFY(edit::placeMedia(p, v2, m.id, 200, 300, 330, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        p.sequences.push_back(v2);
        p.activeSequence = p.sequences.back().id;
        const QString project = QString::fromStdString(path("versions.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_compare_sequences"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"before", "Cut 1"}, {"add_markers", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonArray changes = r.value("structuredContent").toObject().value("changes").toArray();
        QCOMPARE(changes.size(), 2);  // the linked sound changes with the picture, so it is not listed again
        QCOMPARE(changes[0].toObject().value("change").toString(), QString("trimmed"));
        QCOMPARE(changes[0].toObject().value("details").toString(), QString("out -20"));
        QCOMPARE(changes[1].toObject().value("change").toString(), QString("added"));
        QCOMPARE(changes[1].toObject().value("at_seconds").toDouble(), 200.0 / 30);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->markers.size(), size_t(2));
        QCOMPARE(back.active()->markers[0].name, std::string("Trimmed: shot.mov"));
        r = call({{"project", project}, {"before", "Cut 1"}, {"after", "Cut 1"}});
        QVERIFY(r.value("isError").toBool());
        r = call({{"project", project}, {"before", "Cut 9"}});
        QVERIFY(r.value("isError").toBool());
    }

    void mcpServerEditsProjects() {
        writeBallVideo(path("mcp-ball.mp4"), 12);
        const QString project = QString::fromStdString(path("agent.montage"));
        McpServer server;
        int nextId = 1;
        // One request: the response object (progress notifications before it in `notes`).
        std::vector<QJsonObject> notes;
        auto call = [&](const QString& method, QJsonObject params, const QJsonObject& meta = {}) {
            if (!meta.isEmpty()) params["_meta"] = meta;
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", method}, {"params", params}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            notes.clear();
            for (size_t i = 0; i + 1 < lines.size(); ++i) notes.push_back(QJsonDocument::fromJson(QByteArray::fromStdString(lines[i])).object());
            return lines.empty() ? QJsonObject{} : QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object();
        };
        auto tool = [&](const QString& name, const QJsonObject& args, const QJsonObject& meta = {}) {
            return call("tools/call", QJsonObject{{"name", name}, {"arguments", args}}, meta).value("result").toObject();
        };
        auto text = [](const QJsonObject& r) {
            for (const QJsonValue& c : r.value("content").toArray())
                if (c.toObject().value("type").toString() == "text") return c.toObject().value("text").toString();
            return QString();
        };

        // The handshake older clients use.
        QJsonObject init = call("initialize", QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}},
                                                           {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}});
        QCOMPARE(init.value("result").toObject().value("protocolVersion").toString(), QString("2025-06-18"));
        QCOMPARE(init.value("result").toObject().value("serverInfo").toObject().value("name").toString(), QString("montage"));
        QVERIFY(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
        const QJsonArray tools = call("tools/list", {}).value("result").toObject().value("tools").toArray();
        QVERIFY(tools.size() >= 20);
        for (const QJsonValue& t : tools) {
            QVERIFY(t.toObject().value("name").toString().startsWith("montage_"));
            QCOMPARE(t.toObject().value("inputSchema").toObject().value("type").toString(), QString("object"));
        }

        // Build and edit a project.
        QJsonObject r = tool("montage_create_project", QJsonObject{{"project", project}, {"media", QJsonArray{QString::fromStdString(path("mcp-ball.mp4"))}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("width").toInt(), 640);
        r = tool("montage_split", QJsonObject{{"project", project}, {"at", 0.2}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_project_info", QJsonObject{{"project", project}});
        const QJsonArray v1 = r.value("structuredContent").toObject().value("tracks").toArray().at(0).toObject().value("clips").toArray();
        QCOMPARE(v1.size(), 2);
        QCOMPARE(v1.at(1).toObject().value("start").toString(), QString("00:00:00:05"));  // 0.2 s at 25 fps
        const double second = v1.at(1).toObject().value("id").toDouble();
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "invert"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "gaussian_blur"}, {"params", QJsonObject{{"no_such", 1}}}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("no_such"));
        // A drawn Bezier mask from frame fractions (a curved point among corners); too few points refused.
        r = tool("montage_add_effect",
                 QJsonObject{{"project", project}, {"clip", second}, {"effect", "gaussian_blur"},
                             {"mask_path", QJsonArray{QJsonArray{0.2, 0.2}, QJsonArray{0.8, 0.2},
                                                      QJsonObject{{"x", 0.5}, {"y", 0.8}, {"in", QJsonArray{0.1, 0}}, {"out", QJsonArray{-0.1, 0}}}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            const Id fxId = Id(r.value("structuredContent").toObject().value("effect_id").toDouble());
            Project masked;
            QVERIFY(loadProject(project.toStdString(), masked));
            const Clip* c = edit::clipById(*masked.active(), Id(second));
            QVERIFY(c);
            const Effect* e = nullptr;
            for (const Effect& ef : c->effects)
                if (ef.id == fxId) e = &ef;
            QVERIFY(e);
            QCOMPARE(e->p("mask.shape", 0), 5.0);
            const auto pts = maskPath(*e, 0);
            QCOMPARE(pts.size(), size_t(3));
            QVERIFY(pts[2].smooth() && !pts[0].smooth());
            double u = 0, v = 0;
            boxToFrame(maskBox(*e, 0), 640, 360, pts[2].x, pts[2].y, u, v);
            QVERIFY2(std::fabs(u - 0.5) < 1e-9 && std::fabs(v - 0.8) < 1e-9, qPrintable(QString("%1 %2").arg(u).arg(v)));
        }
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "invert"},
                                                   {"mask_path", QJsonArray{QJsonArray{0.2, 0.2}, QJsonArray{0.8, 0.2}}}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("three"));
        // A speed ramp keeps a clip's footage and length (this 7-frame clip is too short for one).
        r = tool("montage_speed_ramp", QJsonObject{{"project", project}, {"clip", second}, {"preset", "bullet"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("too short"));
        {
            const QString rampProject = QString::fromStdString(path("ramp.montage"));
            r = tool("montage_create_project", QJsonObject{{"project", rampProject}, {"media", QJsonArray{QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")}}});
            QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
            r = tool("montage_project_info", QJsonObject{{"project", rampProject}});
            double speech = 0;
            for (const QJsonValue& t : r.value("structuredContent").toObject().value("tracks").toArray())
                if (const QJsonArray cl = t.toObject().value("clips").toArray(); !cl.isEmpty() && !speech) speech = cl.at(0).toObject().value("id").toDouble();
            QVERIFY(speech);
            r = tool("montage_speed_ramp", QJsonObject{{"project", rampProject}, {"clip", speech}, {"preset", "bullet"}});
            QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
            const QJsonArray speeds = r.value("structuredContent").toObject().value("speeds").toArray();
            QCOMPARE(speeds.size(), 5);
            QVERIFY(speeds[2].toDouble() < speeds[0].toDouble() * 0.2);
            QVERIFY(tool("montage_speed_ramp", QJsonObject{{"project", rampProject}, {"clip", speech}, {"preset", "warp"}}).value("isError").toBool());
            r = tool("montage_speed_ramp", QJsonObject{{"project", rampProject}, {"clip", speech}, {"preset", "none"}});
            QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        }
        // Text settings: the Colour Warper's mesh, checked; unknown settings refused.
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "color_warper"},
                                                   {"strings", QJsonObject{{"mesh", "0,4,30,0,0;6,4,-20,0.1,0"}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QVERIFY(tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "color_warper"},
                                                       {"strings", QJsonObject{{"mesh", "0,9,30,0,0"}}}}).value("isError").toBool());
        QVERIFY(tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "color_warper"},
                                                       {"strings", QJsonObject{{"nope", "x"}}}}).value("isError").toBool());
        {
            Project warped;
            QVERIFY(loadProject(project.toStdString(), warped));
            const Clip* c = edit::clipById(*warped.active(), Id(second));
            QVERIFY(c && c->effects.back().type == "color_warper");
            QCOMPARE(c->effects.back().s("mesh"), std::string("0,4,30,0,0;6,4,-20,0.1,0"));
        }
        // Rolling Shutter Repair measures the camera's movement as it is added, and goes first.
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", second}, {"effect", "rolling_shutter"},
                                                   {"params", QJsonObject{{"readout", 60}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            Project withRs;
            QVERIFY(loadProject(project.toStdString(), withRs));
            const Clip* c = edit::clipById(*withRs.active(), Id(second));
            QVERIFY(c && !c->effects.empty());
            QCOMPARE(c->effects.front().type, std::string("rolling_shutter"));
            QCOMPARE(c->effects.front().p("readout", 0), 60.0);
            CameraMotion cm;
            QVERIFY(cameraMotionFromString(c->effects.front().s("motion"), cm) && !cm.steps.empty());
        }
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "Hello"}, {"at", "00:00:00:00"}, {"duration", 0.3}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("text").toString(), QString("Hello"));
        // A ready-made lower third, and an unknown template refused.
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "Ada Lovelace\nMathematician"}, {"at", 0.1},
                                                  {"duration", 0.2}, {"template", "lower_third"}, {"track", "V3"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "x"}, {"at", 0}, {"template", "nope"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("nope"));
        // Rolling credits, and a title made to crawl.
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "Cast\nCrew"}, {"at", 0}, {"duration", 0.3},
                                                  {"template", "credits"}, {"track", "V4"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "News"}, {"at", 0.3}, {"duration", 0.2},
                                                  {"motion", "crawl_left"}, {"track", "V4"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QVERIFY(tool("montage_add_title", QJsonObject{{"project", project}, {"text", "x"}, {"at", 0}, {"motion", "sideways"}}).value("isError").toBool());
        {
            Project withCredits;
            QVERIFY(loadProject(project.toStdString(), withCredits));
            const auto& v4 = withCredits.active()->videoTracks.at(3).clips;
            QCOMPARE(v4.size(), size_t(2));
            QCOMPARE(v4[0].generator.p("motion", 0), 1.0);
            QCOMPARE(v4[1].generator.p("motion", 0), 2.0);
        }
        // Chapters: none yet, then YouTube's list (with a warning: fewer than three).
        QVERIFY(tool("montage_chapters", QJsonObject{{"project", project}}).value("isError").toBool());
        r = tool("montage_add_marker", QJsonObject{{"project", project}, {"at", 0.2}, {"name", "Part two"}, {"chapter", true}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_chapters", QJsonObject{{"project", project}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QVERIFY2(text(r).startsWith("0:00 Intro\n0:00 Part two\n") && text(r).contains("three"), qPrintable(text(r)));
        QVERIFY(!tool("montage_undo", QJsonObject{{"project", project}}).value("isError").toBool());
        // Quality check: the problems listed, and marked when asked (one undo step).
        r = tool("montage_quality_check", QJsonObject{{"project", project}, {"silence_seconds", 0.1}, {"markers", true}});
        QVERIFY2(!r.value("isError").toBool() && text(r).contains("Silence"), qPrintable(text(r)));
        {
            Project checked;
            QVERIFY(loadProject(project.toStdString(), checked));
            const auto& mk = checked.active()->markers;
            QVERIFY(std::any_of(mk.begin(), mk.end(), [](const Marker& m) { return m.name == "QC: Silence" && m.color == 11; }));
        }
        QVERIFY(!tool("montage_undo", QJsonObject{{"project", project}}).value("isError").toBool());
        // Track automation: a mode and volume points.
        r = tool("montage_automate_track", QJsonObject{{"project", project}, {"track", "A1"}, {"mode", "latch"},
                                                       {"volume", QJsonArray{QJsonArray{0, -20}, QJsonArray{0.2, 0}}}});
        QVERIFY2(!r.value("isError").toBool() && text(r).contains("Latch automation") && text(r).contains("-20.00 dB"), qPrintable(text(r)));
        {
            Project automated;
            QVERIFY(loadProject(project.toStdString(), automated));
            const Track& t = automated.active()->audioTracks.at(0);
            QCOMPARE(t.automation, int(AutomationMode::Latch));
            QCOMPARE(t.volumeAuto.keys.size(), size_t(2));
            QCOMPARE(t.volumeAuto.keys[0].v, -20.0);
        }
        QVERIFY(tool("montage_automate_track", QJsonObject{{"project", project}, {"track", "V1"}, {"mode", "read"}}).value("isError").toBool());
        QVERIFY(tool("montage_automate_track", QJsonObject{{"project", project}, {"mode", "loud"}}).value("isError").toBool());
        QVERIFY(!tool("montage_undo", QJsonObject{{"project", project}}).value("isError").toBool());
        // A clip marker, on the clip rather than the sequence.
        {
            Project info;
            QVERIFY(loadProject(project.toStdString(), info));
            const Clip& first = info.active()->videoTracks.at(0).clips.at(0);
            r = tool("montage_add_marker", QJsonObject{{"project", project}, {"at", double(first.start) / info.active()->fpsValue()}, {"name", "On clip"},
                                                       {"clip", double(first.id)}});
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("Clip marker"), qPrintable(text(r)));
            Project marked;
            QVERIFY(loadProject(project.toStdString(), marked));
            QCOMPARE(marked.active()->videoTracks.at(0).clips.at(0).markers.size(), size_t(1));
            QCOMPARE(marked.active()->markers.size(), info.active()->markers.size());
            QVERIFY(tool("montage_add_marker", QJsonObject{{"project", project}, {"at", 9999}, {"clip", double(first.id)}}).value("isError").toBool());
            QVERIFY(!tool("montage_undo", QJsonObject{{"project", project}}).value("isError").toBool());
        }
        {
            // An audition on the first clip: a take added, picked, listed and kept (the project put back after).
            const QString saved = project + ".before-audition";
            QVERIFY(QFile::copy(project, saved));
            Project info;
            QVERIFY(loadProject(project.toStdString(), info));
            const Clip* first = nullptr;
            for (const Track& t : info.active()->videoTracks)
                for (const Clip& c : t.clips)
                    if (!first && c.mediaId) first = &c;
            QVERIFY(first);
            const MediaItem* fm = info.findMedia(first->mediaId);
            const QJsonObject clipArgs{{"project", project}, {"clip", double(first->id)}};
            QJsonObject args = clipArgs;
            args["add"] = QJsonArray{QString::fromStdString(fm->path)};
            args["in"] = 0.1;
            r = tool("montage_audition", args);
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("Take 1 of 2"), qPrintable(text(r)));
            args = clipArgs;
            args["pick"] = "next";
            r = tool("montage_audition", args);
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("Take 2 of 2"), qPrintable(text(r)));
            args["pick"] = 5;
            QVERIFY(tool("montage_audition", args).value("isError").toBool());
            args = clipArgs;
            args["finalize"] = true;
            r = tool("montage_audition", args);
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("no other takes"), qPrintable(text(r)));
            QVERIFY(text(tool("montage_audition", clipArgs)).contains("no takes"));
            QVERIFY(QFile::remove(project) && QFile::rename(saved, project));
        }
        // A copy of the project with its media in a new folder.
        r = tool("montage_consolidate", QJsonObject{{"project", project}, {"folder", QString::fromStdString(path("mcp-copy"))}, {"name", "Copy"}});
        QVERIFY2(!r.value("isError").toBool() && text(r).contains("Wrote"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(QString::fromStdString(path("mcp-copy/Copy.montage"))));
        {
            // Its media moved away: listed as offline, then found again by searching a folder.
            const QString copy = QString::fromStdString(path("mcp-copy/Copy.montage"));
            QVERIFY(QDir().rename(QString::fromStdString(path("mcp-copy/Media")), QString::fromStdString(path("mcp-copy/Moved"))));
            r = tool("montage_relink", QJsonObject{{"project", copy}});
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("offline:"), qPrintable(text(r)));
            r = tool("montage_relink", QJsonObject{{"project", copy}, {"folder", QString::fromStdString(path("mcp-copy"))}});
            QVERIFY2(!r.value("isError").toBool() && text(r).contains("No media is offline"), qPrintable(text(r)));
            Project relinked;
            QVERIFY(loadProject(copy.toStdString(), relinked));
            QVERIFY(offlineMedia(relinked).empty());
            QVERIFY(tool("montage_relink", QJsonObject{{"project", copy}, {"folder", "/no/such/folder"}}).value("isError").toBool());
        }
        // Marker lists in and out.
        r = tool("montage_import_markers", QJsonObject{{"project", project}, {"text", "Timecode,Comment\n00:00:00:05,Check the title\n"}});
        QVERIFY2(!r.value("isError").toBool() && text(r).contains("Added 1"), qPrintable(text(r)));
        r = tool("montage_export_markers", QJsonObject{{"project", project}, {"format", "avid"}});
        QVERIFY2(!r.value("isError").toBool() && text(r).contains("\t00:00:00:05\tV1\t"), qPrintable(text(r)));
        QVERIFY(tool("montage_export_markers", QJsonObject{{"project", project}, {"format", "pdf"}}).value("isError").toBool());
        QVERIFY(tool("montage_import_markers", QJsonObject{{"project", project}, {"text", "nothing here"}}).value("isError").toBool());
        QVERIFY(!tool("montage_undo", QJsonObject{{"project", project}}).value("isError").toBool());
        r = tool("montage_add_marker", QJsonObject{{"project", project}, {"at", 0.1}, {"name", "Look"}});
        QVERIFY(!r.value("isError").toBool());
        Project saved;
        QVERIFY(loadProject(project.toStdString(), saved));
        QCOMPARE(saved.active()->markers.size(), size_t(1));
        QVERIFY(!saved.active()->markers[0].chapter);
        QCOMPARE(saved.active()->videoTracks.at(1).clips.size(), size_t(1));  // the title, above the picture
        // Undo puts back the version before the marker.
        r = tool("montage_undo", QJsonObject{{"project", project}});
        QVERIFY(!r.value("isError").toBool());
        QVERIFY(loadProject(project.toStdString(), saved));
        QVERIFY(saved.active()->markers.empty());

        // Logging media, and finding it by text and by rules (saved as a smart bin).
        const QString ball = QString::fromStdString(path("mcp-ball.mp4"));
        r = tool("montage_log_media", QJsonObject{{"project", project}, {"media", ball}, {"rating", 4}, {"label", "rose"},
                                                  {"add_keywords", QJsonArray{"ball", "test shot"}},
                                                  {"fields", QJsonObject{{"scene", "3"}, {"take", "2"}}}, {"bin", "Selects/ Day 1"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().at(0).toObject().value("label").toString(), QString("Rose"));
        r = tool("montage_log_media", QJsonObject{{"project", project}, {"media", "mcp-ball.mp4"}, {"remove_keywords", "TEST SHOT"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));  // by name too
        r = tool("montage_log_media", QJsonObject{{"project", project}, {"media", ball}, {"fields", QJsonObject{{"duration", "4"}}}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("not a field"));
        r = tool("montage_log_media", QJsonObject{{"project", project}, {"media", "nope.mov"}, {"rating", 1}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("No media"));
        r = tool("montage_log_media", QJsonObject{{"project", project}, {"media", ball}, {"rating", 9}});
        QVERIFY(r.value("isError").toBool());
        {
            Project logged;
            QVERIFY(loadProject(project.toStdString(), logged));
            const MediaItem& m = logged.media.at(0);
            QCOMPARE(m.rating, 4);
            QCOMPARE(m.label, labelFromName("Rose"));
            QCOMPARE(m.keywords, std::vector<std::string>{"ball"});
            QCOMPARE(m.metadata, (std::map<std::string, std::string>{{"scene", "3"}, {"take", "2"}}));
            QCOMPARE(m.bin, std::string("Selects/Day 1"));
        }
        const QJsonArray rules{QJsonObject{{"field", "rating"}, {"op", ">="}, {"value", 3}},
                               QJsonObject{{"field", "keywords"}, {"op", "includes"}, {"value", "BALL"}}};
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"rules", rules}, {"save_as", "Selects"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().size(), 1);
        QVERIFY(text(r).contains("mcp-ball.mp4"));
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"text", "ball 3"}});  // keyword and scene
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().size(), 1);
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"rules", QJsonArray{QJsonObject{{"field", "usage"}, {"op", "is"}, {"value", 0}}}}});
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().size(), 0);  // it is in the cut
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"rules", QJsonArray{QJsonObject{{"field", "rating"}, {"op", "contains"}, {"value", "x"}}}}});
        QVERIFY(r.value("isError").toBool() && text(r).contains(">="));
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"rules", QJsonArray{QJsonObject{{"field", "mood"}, {"op", "is"}, {"value", "x"}}}}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("scene"));
        // A subclip, placed by its name.
        r = tool("montage_make_subclip", QJsonObject{{"project", project}, {"media", ball}, {"start_seconds", 0.12}, {"end_seconds", 0.36}, {"name", "Bounce"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("subclip_start_seconds").toDouble(), 0.12);
        r = tool("montage_make_subclip", QJsonObject{{"project", project}, {"media", ball}, {"start_seconds", 0.3}, {"end_seconds", 0.3}});
        QVERIFY(r.value("isError").toBool());
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", "Bounce"}, {"at", 10}, {"track", "V1"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            Project withSub;
            QVERIFY(loadProject(project.toStdString(), withSub));
            const Clip* placedSub = nullptr;
            for (const Clip& c : withSub.active()->videoTracks.at(0).clips)
                if (c.name == "Bounce") placedSub = &c;
            QVERIFY(placedSub);
            QCOMPARE(placedSub->start, FrameTime(250));
            QVERIFY(std::fabs(placedSub->sourceIn - 3) < 1e-6);  // 0.12 s at 25 fps
            QCOMPARE(placedSub->duration, FrameTime(6));
        }
        r = tool("montage_find_media", QJsonObject{{"project", project}, {"rules", QJsonArray{QJsonObject{{"field", "kind"}, {"op", "is"}, {"value", "subclip"}}}}});
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().size(), 1);
        // Used three times: the placed subclip, and both halves of the split clip play part of its range.
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().at(0).toObject().value("usage").toInt(), 3);
        // Source in and out are seconds of the media.
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", ball}, {"at", 20}, {"in", 0.04}, {"out", 0.2}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            Project ranged;
            QVERIFY(loadProject(project.toStdString(), ranged));
            const Clip& last = ranged.active()->videoTracks.at(0).clips.back();
            QCOMPARE(last.start, FrameTime(500));
            QVERIFY(std::fabs(last.sourceIn - 1) < 1e-6);
            QCOMPARE(last.duration, FrameTime(4));
        }
        // Edit modes: place on top of the clip at 20 s, then replace that clip with a longer range, rippling.
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", ball}, {"at", 20}, {"in", 0}, {"out", 0.2}, {"mode", "place_on_top"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", ball}, {"at", 20.08}, {"in", 0}, {"out", 0.4}, {"mode", "ripple_overwrite"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            Project edited;
            QVERIFY(loadProject(project.toStdString(), edited));
            const Sequence& es = *edited.active();
            const Clip& last = es.videoTracks.at(0).clips.back();
            QCOMPARE(last.start, FrameTime(500));
            QCOMPARE(last.duration, FrameTime(10));
            QVERIFY(std::any_of(es.videoTracks.begin() + 1, es.videoTracks.end(), [](const Track& t) {
                return std::any_of(t.clips.begin(), t.clips.end(), [](const Clip& c) { return c.start == 500 && c.duration == 5; });
            }));
        }
        // The two clips at 20.08 s side by side, then the top one in the top left corner.
        r = tool("montage_layout", QJsonObject{{"project", project}, {"layout", "side_by_side"}, {"at", 20.08}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        {
            const QJsonArray placed = r.value("structuredContent").toObject().value("clips").toArray();
            QCOMPARE(placed.size(), 2);
            const double x0 = placed[0].toObject().value("position").toArray()[0].toDouble(),
                         x1 = placed[1].toObject().value("position").toArray()[0].toDouble();
            QVERIFY2(x0 < 0 && std::fabs(x0 + x1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(x0).arg(x1)));
        }
        r = tool("montage_layout", QJsonObject{{"project", project}, {"layout", "picture_in_picture"}, {"at", 20.08}, {"corner", "top_left"}, {"size", 0.25}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QVERIFY(std::fabs(r.value("structuredContent").toObject().value("clips").toArray()[1].toObject().value("scale").toDouble() - 25) < 1e-6);
        r = tool("montage_layout", QJsonObject{{"project", project}, {"layout", "mosaic"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("layout"));
        r = tool("montage_layout", QJsonObject{{"project", project}, {"layout", "grid"}, {"at", 900}});
        QVERIFY(r.value("isError").toBool());
        r = tool("montage_layout", QJsonObject{{"project", project}, {"layout", "picture_in_picture"}, {"at", 20.08}, {"corner", "middle"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("corner"));
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", ball}, {"at", 900}, {"mode", "ripple_overwrite"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("No clip"));
        r = tool("montage_place_media", QJsonObject{{"project", project}, {"media", ball}, {"mode", "sideways"}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("mode"));
        r = tool("montage_project_info", QJsonObject{{"project", project}});
        QCOMPARE(r.value("structuredContent").toObject().value("media").toArray().at(0).toObject().value("rating").toInt(), 4);
        QVERIFY(loadProject(project.toStdString(), saved));
        QCOMPARE(saved.smartBins.size(), size_t(1));
        QCOMPARE(saved.smartBins.at(0).name, std::string("Selects"));

        // Looking at a frame returns an image.
        r = tool("montage_render_frame", QJsonObject{{"project", project}, {"at", 0.4}, {"width", 320}});
        const QJsonObject image = r.value("content").toArray().at(0).toObject();
        QCOMPARE(image.value("type").toString(), QString("image"));
        QImage shown;
        QVERIFY(shown.loadFromData(QByteArray::fromBase64(image.value("data").toString().toLatin1()), "PNG"));
        QCOMPARE(shown.width(), 320);
        QVERIFY(shown.pixelColor(260, 30).red() > 100 || shown.pixelColor(260, 30).blue() > 100);  // the (inverted) ground
        // Exchange and render, with progress.
        r = tool("montage_export_timeline", QJsonObject{{"project", project}, {"format", "otio"}, {"output", QString::fromStdString(path("agent.otio"))}});
        QVERIFY(!r.value("isError").toBool() && QFileInfo::exists(QString::fromStdString(path("agent.otio"))));
        r = tool("montage_render", QJsonObject{{"project", project}, {"output", QString::fromStdString(path("agent.mp4"))}, {"out", 0.3}},
                 QJsonObject{{"progressToken", "render-1"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QVERIFY(!notes.empty());
        QCOMPARE(notes.front().value("method").toString(), QString("notifications/progress"));
        QCOMPARE(notes.front().value("params").toObject().value("progressToken").toString(), QString("render-1"));

        // Finding shots by description (indexes the video first, once).
        if (visualSearchAvailable() && visualModel().installed()) {
            r = tool("montage_find_shots", QJsonObject{{"project", project}, {"query", "a red ball"}, {"max", 3}});
            QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
            QVERIFY(!r.value("structuredContent").toObject().value("moments").toArray().isEmpty());
            QVERIFY(loadProject(project.toStdString(), saved) && saved.media.at(0).visual);
        }

        // An adjustment layer on a new top track, then an effect on it.
        r = tool("montage_add_adjustment_layer", QJsonObject{{"project", project}, {"at", 0}, {"duration", 0.2}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        const double layer = r.value("structuredContent").toObject().value("id").toDouble();
        QCOMPARE(r.value("structuredContent").toObject().value("generator").toString(), QString("adjustment"));
        r = tool("montage_add_effect", QJsonObject{{"project", project}, {"clip", layer}, {"effect", "invert"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        r = tool("montage_add_adjustment_layer", QJsonObject{{"project", project}, {"at", 0}, {"track", "A1"}});
        QVERIFY(r.value("isError").toBool());

        // Tagging footage by what it shows (from the same index).
        if (visualSearchAvailable() && visualModel().installed()) {
            r = tool("montage_auto_tag", QJsonObject{{"project", project}});
            QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
            const QJsonArray tagged = r.value("structuredContent").toObject().value("media").toArray();
            QVERIFY(!tagged.isEmpty());
            QSet<QString> known;
            for (const TagCategory& c : tagCategories())
                for (const TagLabel& l : c.labels) known.insert(QString::fromStdString(l.keyword));
            for (const QJsonValue& m : tagged)
                for (const QJsonValue& k : m.toObject().value("keywords").toArray()) QVERIFY2(known.contains(k.toString()), qPrintable(k.toString()));
        }

        // Mistakes are reported, not fatal.
        r = tool("montage_move_clip", QJsonObject{{"project", project}, {"clip", 999999}, {"start", 1}});
        QVERIFY(r.value("isError").toBool() && text(r).contains("No clip"));
        r = tool("montage_split", QJsonObject{{"project", project}, {"at", "half past"}});
        QVERIFY(r.value("isError").toBool());
        QCOMPARE(call("tools/call", QJsonObject{{"name", "no_such_tool"}}).value("error").toObject().value("code").toInt(), -32602);
        QCOMPARE(call("no/such", {}).value("error").toObject().value("code").toInt(), -32601);
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(server.handle("{oops").at(0))).object().value("error").toObject().value("code").toInt(), -32700);

        // The stateless revision: version and capabilities on every request.
        const QJsonObject modern{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                 {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
        QJsonObject d = call("server/discover", {}, modern).value("result").toObject();
        QCOMPARE(d.value("resultType").toString(), QString("complete"));
        QVERIFY(d.value("supportedVersions").toArray().contains(QJsonValue("2026-07-28")));
        QCOMPARE(d.value("_meta").toObject().value("io.modelcontextprotocol/serverInfo").toObject().value("name").toString(), QString("montage"));
        r = tool("montage_project_info", QJsonObject{{"project", project}}, modern);
        QCOMPARE(r.value("resultType").toString(), QString("complete"));
        QVERIFY(!r.value("isError").toBool());
        QJsonObject old = modern;
        old["io.modelcontextprotocol/protocolVersion"] = "1999-01-01";
        const QJsonObject e = call("tools/list", {}, old).value("error").toObject();
        QCOMPARE(e.value("code").toInt(), -32022);
        QVERIFY(e.value("data").toObject().value("supported").toArray().contains(QJsonValue("2025-11-25")));
        QCOMPARE(call("tools/list", {}, QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}).value("error").toObject().value("code").toInt(), -32602);

        // On a stream: one line per message.
        std::istringstream in("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
        std::ostringstream out;
        QCOMPARE(server.run(in, out), 0);
        QCOMPARE(QString::fromStdString(out.str()), QString("{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{}}\n"));
    }

    void transcribesSpeech() {
        // Needs a whisper model: $MONTAGE_TEST_WHISPER_MODEL (CI downloads tiny.en).
        const QByteArray model = qgetenv("MONTAGE_TEST_WHISPER_MODEL");
        if (model.isEmpty() || !QFileInfo::exists(QString::fromLocal8Bit(model)))
            QSKIP("Set MONTAGE_TEST_WHISPER_MODEL to a ggml whisper model to run this test");
        const std::string jfk = MONTAGE_TEST_DATA_DIR "/jfk.wav";
        TranscribeOptions opts;
        opts.model = model.toStdString();
        opts.language = "en";
        Transcript t;
        std::string err;
        double lastProgress = -1;
        QVERIFY2(transcribeMedia(jfk, opts, t, [&](double f) { lastProgress = f; }, nullptr, &err), err.c_str());
        const QString text = QString::fromStdString(t.text()).toLower();
        qInfo("transcript: %s", qPrintable(text));
        QVERIFY2(text.contains("ask not what your country can do for you"), qPrintable(text));
        QCOMPARE(QString::fromStdString(t.language), QString("en"));
        QVERIFY(lastProgress > 0.5);
        // Word timings are ordered and inside the 11 s clip.
        double prev = 0;
        for (const auto& s : t.segments)
            for (const auto& w : s.words) {
                QVERIFY(w.start >= prev - 0.05 && w.end >= w.start && w.end <= 11.5);
                prev = w.start;
            }
        auto hits = findPhrase(t, "your country");
        QVERIFY(!hits.empty());
        QVERIFY(hits[0].first > 3 && hits[0].second < 11);
        QVERIFY(!transcriptCues(t).empty());

        // With speaker labels: one voice throughout.
        if (diarizerAvailable() && speakerModel().installed()) {
            TranscribeOptions withSpeakers = opts;
            withSpeakers.speakers = true;
            Transcript labelled;
            QVERIFY2(transcribeMedia(jfk, withSpeakers, labelled, {}, nullptr, &err), err.c_str());
            QCOMPARE(labelled.text(), t.text());
            for (const auto& s : labelled.segments) QCOMPARE(s.speaker, 0);
        }

        // Cancelling stops it.
        std::atomic<bool> cancel{true};
        Transcript none;
        QVERIFY(!transcribeMedia(jfk, opts, none, {}, &cancel, &err));
        // A missing model is reported.
        opts.model = "no-such-model";
        QVERIFY(!transcribeMedia(jfk, opts, none, {}, nullptr, &err));
        QVERIFY(QString::fromStdString(err).contains("not found"));
    }
#endif

    // A textured still moved by known jitter every frame: a shaky "camera".
    // Returns the jitter (sequence pixels) per frame.
    std::vector<Point2> writeShakyVideo(const std::string& file, int frames) {
        QImage tex(480, 270, QImage::Format_RGB32);
        tex.fill(QColor(90, 90, 90));
        {
            QPainter pa(&tex);
            std::mt19937 rng(7);
            std::uniform_int_distribution<int> x(0, 470), y(0, 260), sz(6, 40), c(0, 255);
            for (int i = 0; i < 220; ++i)
                pa.fillRect(x(rng), y(rng), sz(rng), sz(rng), QColor(c(rng), c(rng), c(rng)));
        }
        const QString png = QString::fromStdString(path("texture.png"));
        tex.save(png);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem m = probeOrFail(p, png.toStdString());
        p.media.push_back(m);
        Clip c = makeClip(p, m, TrackKind::Video, s);
        c.duration = frames;
        std::vector<Point2> jitter;
        c.motion.params["scale"] = Param(130.0);
        for (int i = 0; i < frames; ++i) {
            const Point2 j{6 * std::sin(i * 1.7), 4 * std::cos(i * 1.1)};
            jitter.push_back(j);
            c.motion.params["pos_x"].addKey(i, j.x, Interp::Hold);
            c.motion.params["pos_y"].addKey(i, j.y, Interp::Hold);
        }
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        ExportSettings st;
        st.path = file;
        st.audioCodec = "none";
        st.crf = 12;
        st.preset = "ultrafast";
        std::string err;
        if (!exportSequence(p, s, st, nullptr, nullptr, &err)) qWarning("export failed: %s", err.c_str());
        return jitter;
    }

    // A textured card turning in perspective (a Corner Pin keyed from one quad
    // to another) over a still, duller background; 320 x 180 at 25 fps.
    // Returns the card's corners in each frame, as fractions of the frame.
    std::vector<TrackQuad> writeCardVideo(const std::string& file, int frames) {
        auto texture = [&](const char* name, QColor base, int seed, int count, int lo, int hi, int cmin, int cmax) {
            QImage img(480, 270, QImage::Format_RGB32);
            img.fill(base);
            QPainter pa(&img);
            std::mt19937 rng(seed);
            std::uniform_int_distribution<int> x(0, 470), y(0, 260), sz(lo, hi), c(cmin, cmax);
            for (int i = 0; i < count; ++i) pa.fillRect(x(rng), y(rng), sz(rng), sz(rng), QColor(c(rng), c(rng), c(rng)));
            pa.end();
            const QString png = QString::fromStdString(path(name));
            img.save(png);
            return png.toStdString();
        };
        const std::string card = texture("card.png", QColor(120, 120, 120), 11, 140, 12, 60, 0, 255);
        const std::string backdrop = texture("backdrop.png", QColor(40, 40, 40), 5, 120, 8, 40, 20, 90);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem bm = probeOrFail(p, backdrop), cm = probeOrFail(p, card);
        p.media.push_back(bm);
        p.media.push_back(cm);
        Clip bc = makeClip(p, bm, TrackKind::Video, s);
        bc.duration = frames;
        edit::overwrite(p, s, {TrackKind::Video, 0}, bc);
        Clip cc = makeClip(p, cm, TrackKind::Video, s);
        cc.duration = frames;
        const TrackQuad a{{{0.25, 0.2}, {0.7, 0.25}, {0.72, 0.8}, {0.22, 0.75}}};
        const TrackQuad b{{{0.3, 0.26}, {0.78, 0.2}, {0.75, 0.86}, {0.28, 0.78}}};
        Effect pin = makeEffect(p, "corner_pin");
        const char* names[4][2] = {{"tl_x", "tl_y"}, {"tr_x", "tr_y"}, {"br_x", "br_y"}, {"bl_x", "bl_y"}};
        for (int k = 0; k < 4; ++k) {
            pin.params[names[k][0]] = Param();
            pin.params[names[k][1]] = Param();
            pin.params[names[k][0]].addKey(0, a.p[k].x, Interp::Linear);
            pin.params[names[k][0]].addKey(frames - 1, b.p[k].x, Interp::Linear);
            pin.params[names[k][1]].addKey(0, a.p[k].y, Interp::Linear);
            pin.params[names[k][1]].addKey(frames - 1, b.p[k].y, Interp::Linear);
        }
        cc.effects.push_back(pin);
        edit::overwrite(p, s, {TrackKind::Video, 1}, cc);
        ExportSettings st;
        st.path = file;
        st.audioCodec = "none";
        st.crf = 12;
        st.preset = "ultrafast";
        std::string err;
        if (!exportSequence(p, s, st, nullptr, nullptr, &err)) qWarning("export failed: %s", err.c_str());
        std::vector<TrackQuad> out;
        for (int i = 0; i < frames; ++i) {
            const double f = double(i) / (frames - 1);
            TrackQuad q;
            for (int k = 0; k < 4; ++k) q.p[k] = {a.p[k].x + (b.p[k].x - a.p[k].x) * f, a.p[k].y + (b.p[k].y - a.p[k].y) * f};
            out.push_back(q);
        }
        return out;
    }

    // The largest corner error between two quads, in pixels of a 320 x 180 frame.
    static double quadError(const TrackQuad& a, const TrackQuad& b) {
        double worst = 0;
        for (int k = 0; k < 4; ++k)
            worst = std::max({worst, std::fabs(a.p[k].x - b.p[k].x) * 320, std::fabs(a.p[k].y - b.p[k].y) * 180});
        return worst;
    }

    // A shaded red ball (70 x 50 px radii) crossing textured ground, 640 x 360 at
    // 25 fps; returns its centre in each frame.
    std::vector<Point2> writeBallVideo(const std::string& file, int frames) {
        QImage bg(640, 360, QImage::Format_RGB32);
        std::mt19937 rng(3);
        std::normal_distribution<double> noise(0, 10);
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                const double r = 76 + 51 * std::sin(x / 37.0) + 25 * std::cos(y / 23.0) + noise(rng);
                const double g = 115 + 38 * std::sin((x + y) / 41.0) + noise(rng);
                const double b = 128 + 51 * std::cos(x / 53.0) + noise(rng);
                bg.setPixel(x, y, qRgb(std::clamp(int(r), 0, 255), std::clamp(int(g), 0, 255), std::clamp(int(b), 0, 255)));
            }
        QImage ball(640, 360, QImage::Format_ARGB32);
        ball.fill(Qt::transparent);
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                const double u = (x + 0.5 - 320) / 70, v = (y + 0.5 - 180) / 50;
                if (u * u + v * v > 1) continue;
                const double sh = 1.0 - 0.35 * ((u + 0.28) * (u + 0.28) + (v + 0.3) * (v + 0.3));
                ball.setPixel(x, y, qRgba(int(230 * sh), int(51 * sh), int(38 * sh), 255));
            }
        const QString bgPng = QString::fromStdString(path("ground.png")), ballPng = QString::fromStdString(path("ball.png"));
        bg.save(bgPng);
        ball.save(ballPng);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 640;
        s.height = 360;
        s.fps = {25, 1};
        MediaItem mb = probeOrFail(p, bgPng.toStdString()), mo = probeOrFail(p, ballPng.toStdString());
        p.media.push_back(mb);
        p.media.push_back(mo);
        Clip cb = makeClip(p, mb, TrackKind::Video, s), co = makeClip(p, mo, TrackKind::Video, s);
        cb.duration = co.duration = frames;
        std::vector<Point2> centres;
        for (int i = 0; i < frames; ++i) {
            const Point2 c{180.0 + 14 * i, 170 + 40 * std::sin(i / 3.0)};
            centres.push_back(c);
            co.motion.params["pos_x"].addKey(i, c.x - 320, Interp::Hold);
            co.motion.params["pos_y"].addKey(i, c.y - 180, Interp::Hold);
        }
        edit::overwrite(p, s, {TrackKind::Video, 0}, cb);
        while (s.videoTracks.size() < 2) s.videoTracks.push_back(makeTrack(p, TrackKind::Video, "V2"));
        edit::overwrite(p, s, {TrackKind::Video, 1}, co);
        ExportSettings st;
        st.path = file;
        st.audioCodec = "none";
        st.crf = 10;
        st.preset = "ultrafast";
        std::string err;
        if (!exportSequence(p, s, st, nullptr, nullptr, &err)) qWarning("export failed: %s", err.c_str());
        return centres;
    }

    void objectMasksFollowAnObject() {
        if (!segmenterAvailable()) QSKIP("Built without ONNX Runtime");
        if (!objectModel().installed())
            QSKIP("Set MONTAGE_OBJECT_MODEL to a folder with the EdgeTAM model files to run this test");
        const int frames = 16;
        const std::string video = path("ball.mp4");
        const auto centres = writeBallVideo(video, frames);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 640;
        s.height = 360;
        s.fps = {25, 1};
        MediaItem m = probeOrFail(p, video);
        p.media.push_back(m);
        Clip c = makeClip(p, m, TrackKind::Video, s);
        c.duration = frames;
        Effect e = makeEffect(p, "invert");
        e.params["mask.shape"] = Param(3.0);
        // How well a frame's segmentation covers the ball (intersection over union).
        auto iou = [&](const ObjectMask& o, int64_t n) {
            std::vector<float> logits;
            if (!o.logits(n, logits)) return -1.0;
            const std::vector<float> matte = objectMatte(logits, 640, 360, 1, 0);
            double both = 0, either = 0;
            for (int y = 0; y < 360; ++y)
                for (int x = 0; x < 640; ++x) {
                    const double u = (x + 0.5 - centres[size_t(n)].x) / 70, v = (y + 0.5 - centres[size_t(n)].y) / 50;
                    const bool in = u * u + v * v <= 1, picked = matte[size_t(y) * 640 + size_t(x)] > 0.5f;
                    both += in && picked;
                    either += in || picked;
                }
            return both / std::max(1.0, either);
        };

        // One click on the ball picks it out of the first frame.
        auto clicked = withObjectPoint(p, s, c, e, 0, {centres[0].x / 640, centres[0].y / 360, 1});
        QVERIFY(clicked);
        QCOMPARE(clicked->fps, 25.0);
        std::string err;
        auto first = segmentClipObjectFrame(p, s, c, *clicked, 0, &err);
        QVERIFY2(first, err.c_str());
        QCOMPARE(int(first->frames.size()), 1);
        QVERIFY2(iou(*first, 0) > 0.95, qPrintable(QString::number(iou(*first, 0))));

        // Tracked through the clip from that click.
        std::vector<double> fractions;
        auto tracked = trackClipObject(p, s, c, *first, 0, true, [&](double f) { fractions.push_back(f); }, nullptr, &err);
        QVERIFY2(tracked, err.c_str());
        QCOMPARE(int(tracked->frames.size()), frames);
        QCOMPARE(int(fractions.size()), frames);
        QCOMPARE(fractions.back(), 1.0);
        for (int n = 0; n < frames; ++n) QVERIFY2(iou(*tracked, n) > 0.95, qPrintable(QString("frame %1: %2").arg(n).arg(iou(*tracked, n))));

        // Rendered as the effect's matte: the inverted area is the ball at that frame.
        e.object = tracked;
        Image frame(640, 360);
        frame.fill(1, 1, 1, 1);
        const std::vector<float> matte = effectMatte(e, 8, frame, 1.0, (8 + 0.5) / 25);
        double inside = 0, outside = 0;
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                const double u = (x + 0.5 - centres[8].x) / 70, v = (y + 0.5 - centres[8].y) / 50;
                (u * u + v * v <= 0.8 ? inside : outside) += u * u + v * v > 0.8 && u * u + v * v < 1.25 ? 0 : matte[size_t(y) * 640 + size_t(x)];
            }
        QVERIFY2(inside > 0.97 * M_PI * 70 * 50 * 0.8 && outside < 50, qPrintable(QString("%1 %2").arg(inside).arg(outside)));
        // Not segmented: no matte (a frame of the media the clip does not show).
        const std::vector<float> none = effectMatte(e, 8, frame, 1.0, 40.0);
        QVERIFY(!none.empty() && *std::max_element(none.begin(), none.end()) == 0.f);

        // Backwards, from a click on the last frame only.
        Effect e2 = makeEffect(p, "invert");
        e2.params["mask.shape"] = Param(3.0);
        auto lastClick = withObjectPoint(p, s, c, e2, frames - 1, {centres.back().x / 640, centres.back().y / 360, 1});
        QVERIFY(!trackClipObject(p, s, c, *lastClick, 4, true, {}, nullptr, &err));
        QVERIFY(!err.empty());  // nothing clicked at or before frame 4
        auto back = trackClipObject(p, s, c, *lastClick, frames - 1, false, {}, nullptr, &err);
        QVERIFY2(back, err.c_str());
        QCOMPARE(int(back->frames.size()), frames);
        for (int n = 0; n < frames; ++n) QVERIFY2(iou(*back, n) > 0.95, qPrintable(QString("frame %1: %2").arg(n).arg(iou(*back, n))));

        // Cancelling stops without a result.
        std::atomic<bool> cancel{true};
        err = "x";
        QVERIFY(!trackClipObject(p, s, c, *first, 0, true, {}, &cancel, &err));
        QVERIFY(err.empty());
    }

    void slowMotionFrameSampling() {
        // A white square moving 12 px right every frame.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        Clip sq = makeGeneratorClip(gen, "color", 20);
        sq.generator.params["color.r"] = sq.generator.params["color.g"] = sq.generator.params["color.b"] = Param(1.0);
        sq.motion.params["scale"] = Param(10.0);  // 32 x 18 px
        for (int i = 0; i < 20; ++i) sq.motion.params["pos_x"].addKey(i, -100 + 12 * i, Interp::Hold);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, sq);
        ExportSettings st;
        st.path = path("moving.mp4");
        st.audioCodec = "none";
        st.crf = 0;
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());

        // At 50 % speed, timeline frame 3 shows source frame 1.5.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem m = probeOrFail(p, st.path);
        p.media.push_back(m);
        Clip c = makeClip(p, m, TrackKind::Video, s);
        c.duration = 36;
        c.timing.params["speed"] = Param(50.0);
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        QVERIFY(trackAt(s, {TrackKind::Video, 0})->clips[0].ramped());
        auto row = [&](int sampling) {
            trackAt(s, {TrackKind::Video, 0})->clips[0].timing.params["sampling"] = Param(double(sampling));
            RenderOptions ro;
            Image img = renderProgramFrame(p, s, 3, ro);
            std::vector<float> v(320);
            for (int x = 0; x < 320; ++x) v[size_t(x)] = img.at(x, 90)[1];
            return v;
        };
        auto centroid = [](const std::vector<float>& v) {
            double sum = 0, w = 0;
            for (size_t x = 0; x < v.size(); ++x) {
                sum += x * v[x];
                w += v[x];
            }
            return sum / std::max(1e-9, w);
        };
        auto soft = [](const std::vector<float>& v) {  // pixels caught between black and white
            int n = 0;
            for (float x : v) n += x > 0.25f && x < 0.75f;
            return n;
        };
        // Square centres: frame 1 at x = 72, frame 2 at 84, so 1.5 is at 78.
        const auto nearest = row(0), blend = row(1), flow = row(2);
        QVERIFY2(std::fabs(centroid(nearest) - 72) < 1.5, qPrintable(QString::number(centroid(nearest))));
        QVERIFY2(std::fabs(centroid(blend) - 78) < 1.5, qPrintable(QString::number(centroid(blend))));
        QVERIFY2(std::fabs(centroid(flow) - 78) < 1.5, qPrintable(QString::number(centroid(flow))));
        // Blending shows two half-bright copies; optical flow moves one square into place.
        QVERIFY2(soft(blend) >= 18, qPrintable(QString::number(soft(blend))));
        QVERIFY2(soft(flow) <= 6, qPrintable(QString::number(soft(flow))));
        QVERIFY(soft(nearest) <= 4);
    }

    void rollingShutterFromFootage() {
        // A textured card panning right at 6 px a frame, as a global shutter sees it and as a rolling shutter
        // reading out over a whole frame does (sheared: the bottom row 6 px further on than the top).
        QImage tex(480, 270, QImage::Format_RGB32);
        tex.fill(QColor(90, 90, 90));
        {
            QPainter pa(&tex);
            std::mt19937 rng(11);
            std::uniform_int_distribution<int> x(0, 470), y(0, 260), sz(6, 40), c(0, 255);
            for (int i = 0; i < 260; ++i) pa.fillRect(x(rng), y(rng), sz(rng), sz(rng), QColor(c(rng), c(rng), c(rng)));
        }
        // At 160 % the card is 512 x 288 in the 320 x 180 frame: 6 px across the 180 rows on screen.
        const double shear = 6.0 / 180;
        QImage sheared(480, 270, QImage::Format_RGB32);
        sheared.fill(QColor(90, 90, 90));
        {
            QPainter pa(&sheared);
            pa.setRenderHint(QPainter::SmoothPixmapTransform);
            pa.setTransform(QTransform(1, 0, shear, 1, -shear * 135, 0));
            pa.drawImage(0, 0, tex);
        }
        const int frames = 30;
        auto write = [&](const QImage& card, const std::string& file) {
            const QString png = QString::fromStdString(file + ".png");
            card.save(png);
            Project p = makeDefaultProject();
            Sequence& s = *p.active();
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            MediaItem m = probeOrFail(p, png.toStdString());
            p.media.push_back(m);
            Clip c = makeClip(p, m, TrackKind::Video, s);
            c.duration = frames;
            c.motion.params["scale"] = Param(160.0);
            c.motion.params["pos_x"].addKey(0, -87);
            c.motion.params["pos_x"].addKey(frames - 1, -87 + 6 * (frames - 1));
            edit::overwrite(p, s, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = file;
            st.audioCodec = "none";
            st.crf = 10;
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        };
        const std::string clean = path("pan-global.mp4"), rolling = path("pan-rolling.mp4");
        write(tex, clean);
        write(sheared, rolling);
        auto project = [&](const std::string& file, Project& p) {
            p = makeDefaultProject();
            Sequence& s = *p.active();
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            MediaItem mi = probeOrFail(p, file);
            p.media.push_back(mi);
            QVERIFY(edit::placeMedia(p, s, mi.id, 0, 0, frames, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        };
        Project pc, pr;
        project(clean, pc);
        project(rolling, pr);
        Clip& clip = trackAt(*pr.active(), {TrackKind::Video, 0})->clips.at(0);
        std::string motion, err;
        QVERIFY2(analyzeClipStabilization(pr, *pr.active(), clip, motion, {}, nullptr, &err), err.c_str());
        auto difference = [&](int frame) {
            const Image a = renderProgramFrame(pc, *pc.active(), frame, {}), b = renderProgramFrame(pr, *pr.active(), frame, {});
            double d = 0;
            for (int y = 20; y < 160; ++y)
                for (int x = 60; x < 260; ++x) d += std::fabs(a.at(x, y)[1] - b.at(x, y)[1]);
            return d / (140 * 200);
        };
        const double skewed = difference(15);
        Effect rs = makeEffect(pr, "rolling_shutter");
        rs.strings["motion"] = motion;
        rs.params["readout"] = Param(100.0);
        rs.params["framing"] = Param(1.0);
        clip.effects.insert(clip.effects.begin(), rs);
        const double repaired = difference(15);
        QVERIFY2(repaired < skewed * 0.4, qPrintable(QString("%1 -> %2").arg(skewed).arg(repaired)));
        // Half the readout repairs about half of it.
        clip.effects.front().params["readout"] = Param(50.0);
        const double half = difference(15);
        QVERIFY2(half > repaired && half < skewed, qPrintable(QString("%1 %2 %3").arg(skewed).arg(half).arg(repaired)));
    }

    void trackingAndStabilization() {
        const std::string video = path("shaky.mp4");
        const int frames = 40;
        const auto jitter = writeShakyVideo(video, frames);
        // Feature tracking between two frames recovers the shift.
        VideoDecoder dec;
        std::string err;
        QVERIFY2(dec.open(video, &err), err.c_str());
        const GrayImage a = toGray(*dec.frameAt(0)), b = toGray(*dec.frameAt(1 / 25.0));
        const auto pts = goodFeatures(a, 200, 8);
        QVERIFY2(pts.size() > 40, qPrintable(QString::number(pts.size())));
        std::vector<Point2> moved;
        std::vector<bool> ok;
        trackPoints(a, b, pts, moved, ok);
        std::vector<Point2> fa, fb;
        for (size_t i = 0; i < pts.size(); ++i)
            if (ok[i]) {
                fa.push_back(pts[i]);
                fb.push_back(moved[i]);
            }
        QVERIFY(fa.size() > pts.size() / 2);
        Similarity m;
        QVERIFY(fitMotion(fa, fb, MotionModel::Similarity, m));
        QVERIFY2(std::fabs(m.tx - (jitter[1].x - jitter[0].x)) < 0.3 && std::fabs(m.ty - (jitter[1].y - jitter[0].y)) < 0.3,
                 qPrintable(QString("%1 %2").arg(m.tx).arg(m.ty)));
        QVERIFY(std::fabs(m.angle) < 0.003 && std::fabs(m.scale - 1) < 0.003);

        // Camera motion over the clip follows the jitter (fractions of the width).
        CameraMotion cam = analyzeCameraMotion(video, 0, frames / 25.0, {}, nullptr, &err);
        QCOMPARE(int(cam.steps.size()), frames);
        double x = 0, y = 0, worst = 0;
        for (int i = 1; i < frames; ++i) {
            x += cam.steps[size_t(i)].tx;
            y += cam.steps[size_t(i)].ty;
            worst = std::max({worst, std::fabs(x * 320 - (jitter[size_t(i)].x - jitter[0].x)),
                              std::fabs(y * 320 - (jitter[size_t(i)].y - jitter[0].y))});
        }
        QVERIFY2(worst < 0.6, qPrintable(QString::number(worst)));
        // Locked: each frame is moved back onto the first.
        auto lock = stabilizationCorrections(cam, 0, MotionModel::Similarity);
        for (int i = 0; i < frames; ++i)
            QVERIFY(std::fabs(lock[size_t(i)].tx * 320 + (jitter[size_t(i)].x - jitter[0].x)) < 0.8);
        // Smoothed corrections are smaller, and the zoom covers the largest.
        auto smooth = stabilizationCorrections(cam, 1.0, MotionModel::Translation);
        const double zoom = stabilizationZoom(lock, 180.0 / 320);
        QVERIFY2(zoom > 1.02 && zoom < 1.2, qPrintable(QString::number(zoom)));
        QVERIFY(stabilizationZoom(smooth, 180.0 / 320) <= zoom + 1e-9);

        // A region followed through the clip.
        TrackRegion start{0.4, 0.45, 0.25, 0.3, 0};
        auto track = trackRegion(video, 0, (frames - 1) / 25.0, start, MotionModel::Translation, {}, nullptr, &err);
        QCOMPARE(int(track.size()), frames);
        worst = 0;
        for (int i = 0; i < frames; ++i)
            worst = std::max({worst, std::fabs((track[size_t(i)].x - start.x) * 320 - (jitter[size_t(i)].x - jitter[0].x)),
                              std::fabs((track[size_t(i)].y - start.y) * 180 - (jitter[size_t(i)].y - jitter[0].y))});
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
        // Backwards from the end follows the same motion in reverse (measured from where it
        // starts, so the forward pass's own drift does not count twice).
        auto back = trackRegion(video, (frames - 1) / 25.0, 0, track.back(), MotionModel::Translation, {}, nullptr, &err);
        QCOMPARE(int(back.size()), frames);
        worst = 0;
        for (int k = 0; k < frames; ++k) {
            const int f = frames - 1 - k;
            worst = std::max({worst, std::fabs((back[size_t(k)].x - back[0].x) * 320 - (jitter[size_t(f)].x - jitter[size_t(frames - 1)].x)),
                              std::fabs((back[size_t(k)].y - back[0].y) * 180 - (jitter[size_t(f)].y - jitter[size_t(frames - 1)].y))});
        }
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));

        // In a sequence: the clip shows footage frames 10 to 34.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem mi = probeOrFail(p, video);
        p.media.push_back(mi);
        QVERIFY(edit::placeMedia(p, s, mi.id, 0, 10, 35, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip& clip = trackAt(s, {TrackKind::Video, 0})->clips.at(0);
        // Stabilize: frame-to-frame differences shrink to almost nothing when locked.
        std::string motion;
        QVERIFY2(analyzeClipStabilization(p, s, clip, motion, {}, nullptr, &err), err.c_str());
        Effect stab = makeEffect(p, "stabilize");
        stab.strings["motion"] = motion;
        stab.params["smoothness"] = Param(0.0);
        stab.params["method"] = Param(0.0);
        auto shake = [&](int from, int to) {
            RenderOptions ro;
            double total = 0;
            Image prev = renderProgramFrame(p, s, from, ro);
            for (int f = from + 1; f <= to; ++f) {
                Image img = renderProgramFrame(p, s, f, ro);
                double d = 0;
                // The middle of the frame (edges move with the zoom).
                for (int y = 45; y < 135; ++y)
                    for (int x = 80; x < 240; ++x) d += std::fabs(img.at(x, y)[1] - prev.at(x, y)[1]);
                total += d / (90 * 160);
                prev = std::move(img);
            }
            return total / (to - from);
        };
        const double before = shake(0, 12);
        clip.effects.push_back(stab);
        const double after = shake(0, 12);
        QVERIFY2(after < before * 0.35, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        // The motion data travels with the project.
        Project back2;
        QVERIFY(projectFromJson(projectToJson(p), back2));
        QCOMPARE(back2.active()->videoTracks[0].clips[0].effects.back().s("motion"), motion);

        // Mask tracking writes keyframes that follow the footage.
        Effect blur = makeEffect(p, "gaussian_blur");
        blur.params["mask.shape"] = Param(1.0);
        blur.params["mask.x"] = Param(0.4);
        blur.params["mask.y"] = Param(0.45);
        std::vector<std::pair<FrameTime, TrackRegion>> keys;
        QVERIFY2(trackClipMask(p, s, clip, blur, 0, true, MotionModel::Translation, keys, {}, nullptr, &err), err.c_str());
        QCOMPARE(clip.duration, FrameTime(25));
        QCOMPARE(int(keys.size()), 25);
        applyMaskTrack(blur, keys);
        QCOMPARE(blur.params["mask.x"].keys.size(), size_t(25));
        // Local frame 20 is footage frame 30.
        const double dx = (blur.p("mask.x", 20) - 0.4) * 320, expect = jitter[30].x - jitter[10].x;
        QVERIFY2(std::fabs(dx - expect) < 1.0, qPrintable(QString("%1 vs %2").arg(dx).arg(expect)));
        // Not from the last frame forwards.
        QVERIFY(!trackClipMask(p, s, clip, blur, 24, true, MotionModel::Translation, keys, {}, nullptr, &err));
        // Backwards from the end: keys down to frame 0, following the footage (frame 34 to 10).
        QVERIFY(trackClipMask(p, s, clip, blur, 24, false, MotionModel::Translation, keys, {}, nullptr, &err));
        QCOMPARE(keys.front().first, FrameTime(24));
        QCOMPARE(keys.back().first, FrameTime(0));
        const double shift = (keys.back().second.x - keys.front().second.x) * 320, truth = jitter[10].x - jitter[34].x;
        QVERIFY2(std::fabs(shift - truth) < 1.0, qPrintable(QString("%1 vs %2").arg(shift).arg(truth)));
    }

    void followTheFootage() {
        const std::string video = path("follow.mp4");
        const auto jitter = writeShakyVideo(video, 40);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem mi = probeOrFail(p, video);
        p.media.push_back(mi);
        // The footage shows its frames 10 to 34; a title above it, positioned on a detailed spot.
        QVERIFY(edit::placeMedia(p, s, mi.id, 0, 10, 35, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip title = makeGeneratorClip(p, "title", 25);
        title.generator.strings["text"] = "Look";
        title.motion.params["pos_x"] = -30.0;
        title.motion.params["pos_y"] = 10.0;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, title).ok);
        Clip& t = trackAt(s, {TrackKind::Video, 1})->clips.at(0);
        const Clip& footage = trackAt(s, {TrackKind::Video, 0})->clips.at(0);
        QVERIFY(footageBeneath(p, s, t, 0) == &footage);
        QVERIFY(!footageBeneath(p, s, footage, 0));
        std::vector<FollowKey> keys;
        std::string err;
        QVERIFY2(trackClipFollow(p, s, t, 0, true, MotionModel::Similarity, 0.3, keys, {}, nullptr, &err), err.c_str());
        QCOMPARE(int(keys.size()), 25);
        QCOMPARE(keys.front().t, FrameTime(0));
        double worst = 0;
        for (const FollowKey& k : keys) {
            // Local frame k.t is footage frame 10 + k.t.
            worst = std::max({worst, std::fabs(k.x + 30 - (jitter[size_t(10 + k.t)].x - jitter[10].x)),
                              std::fabs(k.y - 10 - (jitter[size_t(10 + k.t)].y - jitter[10].y))});
            QVERIFY(std::fabs(k.scale - 100) < 2 && std::fabs(k.rotation) < 1);  // the camera only shifts
        }
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
        // Position only: scale and rotation stay as they were.
        applyFollow(t, keys, MotionModel::Translation);
        QCOMPARE(t.motion.params["pos_x"].keys.size(), size_t(25));
        QVERIFY(!t.motion.params["scale"].animated());
        QVERIFY(std::fabs(t.motion.p("pos_x", 20) + 30 - (jitter[30].x - jitter[10].x)) < 1.0);
        // Backwards from the end: keys down to the first frame.
        QVERIFY2(trackClipFollow(p, s, t, 24, false, MotionModel::Translation, 0.3, keys, {}, nullptr, &err), err.c_str());
        QCOMPARE(keys.front().t, FrameTime(24));
        QCOMPARE(keys.back().t, FrameTime(0));
        // Nothing beneath: an error.
        QVERIFY(!trackClipFollow(p, s, footage, 0, true, MotionModel::Translation, 0.3, keys, {}, nullptr, &err));
        QVERIFY(!err.empty());
    }

    void autoReframe() {
        // A red ball crossing textured ground left to right (x 180 -> 516 of 640), bobbing up and down.
        const std::string video = path("reframe-ball.mp4");
        const int frames = 25;
        const auto centres = writeBallVideo(video, frames);
        std::string err;
        // The subject of each frame is the ball (the first has no motion to go on, only contrast).
        const auto pts = findSubject(video, 0, (frames - 1) / 25.0, 1 / 25.0, {}, nullptr, &err);
        QCOMPARE(int(pts.size()), frames);
        double worst = 0;
        for (int i = 1; i < frames; ++i)
            worst = std::max({worst, std::fabs(pts[size_t(i)].x * 640 - centres[size_t(i)].x), std::fabs(pts[size_t(i)].y * 360 - centres[size_t(i)].y)});
        QVERIFY2(worst < 40, qPrintable(QString::number(worst)));
        // Smoothing keeps the path but not the jitter; a still subject gives a still path.
        const auto smooth = smoothSubjectPath(pts, 0.2);
        QVERIFY(smooth.front().x < smooth.back().x);
        std::vector<SubjectPoint> still(10);
        for (int i = 0; i < 10; ++i) still[size_t(i)] = {i * 0.2, 0.5 + 0.01 * (i % 2), 0.4, 1};
        const auto held = smoothSubjectPath(still, 0.2);
        for (const SubjectPoint& q : held) QVERIFY(q.x == held[0].x && std::fabs(q.x - 0.505) < 0.003);

        // The 16:9 cut made 9:16, following the ball.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 640;
        s.height = 360;
        s.fps = {25, 1};
        MediaItem mi = probeOrFail(p, video);
        p.media.push_back(mi);
        QVERIFY(edit::placeMedia(p, s, mi.id, 0, 0, frames, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const Id original = s.id;
        int w = 0, h = 0;
        reframeSize(s, 9, 16, w, h);
        QCOMPARE(w, 360);
        QCOMPARE(h, 640);
        std::map<Id, std::vector<ReframeKey>> paths;
        QVERIFY2(analyzeSequenceReframe(p, s, 2, paths, {}, nullptr, &err), err.c_str());
        QCOMPARE(paths.size(), size_t(1));
        const Id made = makeReframedSequence(p, original, w, h, paths);
        const Sequence* vs = p.findSequence(made);
        QVERIFY(vs && vs->id != original);
        QCOMPARE(vs->name, std::string("Sequence 1 9:16"));
        QCOMPARE(vs->width, 360);
        QCOMPARE(vs->height, 640);
        const Clip& c = vs->videoTracks[0].clips.at(0);
        QVERIFY(c.id != p.findSequence(original)->videoTracks[0].clips.at(0).id);
        QVERIFY(c.motion.params.at("pos_x").animated());
        QVERIFY(!p.findSequence(original)->videoTracks[0].clips.at(0).motion.params["pos_x"].animated());  // the original is untouched
        // Where the ball lands across the 360 px frame: the picture fills 640 px of height, so it is 1137.8 px wide.
        const double dw = 640.0 * 640.0 / 360.0;
        int inFrame = 0, centred = 0;
        for (int i = 0; i < frames; ++i) {
            const double x = 180 + c.motion.p("pos_x", i) + (centres[size_t(i)].x / 640 - 0.5) * dw;
            if (x > 0 && x < 360) ++inFrame;
            if (std::fabs(x - 180) < 90) ++centred;
        }
        // A centre crop would lose the ball for most of the shot.
        QVERIFY2(inFrame == frames && centred >= frames - 2, qPrintable(QString("%1 in frame, %2 centred").arg(inFrame).arg(centred)));
        int naive = 0;
        for (int i = 0; i < frames; ++i) naive += std::fabs((centres[size_t(i)].x / 640 - 0.5) * dw) < 180 ? 1 : 0;
        QVERIFY2(naive <= frames - 8, qPrintable(QString::number(naive)));  // 15 of 25

        // Over MCP: a square version becomes the active sequence.
        const QString project = QString::fromStdString(path("reframe.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_auto_reframe"},
                                                     {"arguments", QJsonObject{{"project", project}, {"aspect", "1:1"}, {"motion", "faster"}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("width").toInt(), 360);
        QCOMPARE(r.value("structuredContent").toObject().value("clips_reframed").toInt(), 1);
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->name, std::string("Sequence 1 1:1"));
        QCOMPARE(back.active()->height, 360);
        QVERIFY(back.active()->videoTracks[0].clips.at(0).motion.params.at("pos_x").animated());
    }

    void planarTracking() {
        // A homography from point pairs, a fifth of them wrong.
        Homography truth;
        const double th[9] = {1.1, 0.08, 12, -0.05, 0.95, 7, 0.0004, -0.0002, 1};
        std::copy(th, th + 9, truth.h);
        std::mt19937 rng(1);
        std::uniform_real_distribution<double> u(0, 300);
        std::vector<Point2> from, to;
        for (int i = 0; i < 50; ++i) {
            from.push_back({u(rng), u(rng)});
            to.push_back(i % 5 == 0 ? Point2{u(rng), u(rng)} : truth.apply(from.back()));
        }
        Homography h;
        int inliers = 0;
        QVERIFY(fitHomography(from, to, h, &inliers));
        QCOMPARE(inliers, 40);
        for (int i = 0; i < 50; ++i)
            if (i % 5) {
                const Point2 q = h.apply(from[size_t(i)]);
                QVERIFY(std::hypot(q.x - to[size_t(i)].x, q.y - to[size_t(i)].y) < 1e-6);
            }
        QVERIFY(!fitHomography({from[1], from[2], from[3]}, {to[1], to[2], to[3]}, h));

        // A card turning in perspective, followed through the clip and back.
        const std::string video = path("card.mp4");
        const int frames = 30;
        const auto corners = writeCardVideo(video, frames);
        std::string err;
        auto track = trackQuad(video, 0, (frames - 1) / 25.0, corners[0], {}, nullptr, &err);
        QCOMPARE(int(track.size()), frames);
        double worst = 0;
        for (int i = 0; i < frames; ++i) worst = std::max(worst, quadError(track[size_t(i)], corners[size_t(i)]));
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
        auto back = trackQuad(video, (frames - 1) / 25.0, 0, corners.back(), {}, nullptr, &err);
        QCOMPARE(int(back.size()), frames);
        worst = 0;
        for (int k = 0; k < frames; ++k) worst = std::max(worst, quadError(back[size_t(k)], corners[size_t(frames - 1 - k)]));
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
        // Corners on nothing (the black corner of a blank frame) cannot be tracked.
        const TrackQuad blank{{{0.0, 0.0}, {0.02, 0.0}, {0.02, 0.02}, {0.0, 0.02}}};
        QVERIFY(trackQuad(video, 0, 1, blank, {}, nullptr, &err).size() < 2);

        // In a sequence: a picture pinned over the card's footage follows it.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        MediaItem mv = probeOrFail(p, video), mi = probeOrFail(p, path("backdrop.png"));
        p.media.push_back(mv);
        p.media.push_back(mi);
        QVERIFY(edit::placeMedia(p, s, mv.id, 0, 0, frames, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip pic = makeClip(p, mi, TrackKind::Video, s);
        pic.duration = frames;
        Effect pin = makeEffect(p, "corner_pin");
        const char* names[4][2] = {{"tl_x", "tl_y"}, {"tr_x", "tr_y"}, {"br_x", "br_y"}, {"bl_x", "bl_y"}};
        for (int k = 0; k < 4; ++k) {
            pin.params[names[k][0]] = Param(corners[5].p[k].x);
            pin.params[names[k][1]] = Param(corners[5].p[k].y);
        }
        pic.effects.push_back(pin);
        edit::overwrite(p, s, {TrackKind::Video, 1}, pic);
        const Clip& under = trackAt(s, {TrackKind::Video, 0})->clips.at(0);
        const Clip& over = trackAt(s, {TrackKind::Video, 1})->clips.at(0);
        QVERIFY(cornerTrackSource(p, s, over, 5) == &under);
        QVERIFY(cornerTrackSource(p, s, under, 5) == &under);  // nothing beneath: its own footage
        QVERIFY(quadError(cornerPinQuad(pin, 5), corners[5]) < 1e-9);
        std::vector<std::pair<FrameTime, TrackQuad>> keys;
        QVERIFY2(trackClipCorners(p, s, over, pin, 5, true, keys, {}, nullptr, &err), err.c_str());
        QCOMPARE(int(keys.size()), frames - 5);
        QCOMPARE(keys.front().first, FrameTime(5));
        QCOMPARE(keys.back().first, FrameTime(frames - 1));
        applyCornerTrack(pin, keys);
        QCOMPARE(pin.params["br_x"].keys.size(), size_t(frames - 5));
        worst = 0;
        for (int f = 5; f < frames; ++f) worst = std::max(worst, quadError(cornerPinQuad(pin, f), corners[size_t(f)]));
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
        // Backwards to the start adds the earlier keys and keeps the later ones.
        QVERIFY2(trackClipCorners(p, s, over, pin, 5, false, keys, {}, nullptr, &err), err.c_str());
        QCOMPARE(keys.back().first, FrameTime(0));
        applyCornerTrack(pin, keys);
        QCOMPARE(pin.params["br_x"].keys.size(), size_t(frames));
        QVERIFY2(quadError(cornerPinQuad(pin, 0), corners[0]) < 1.0, qPrintable(QString::number(quadError(cornerPinQuad(pin, 0), corners[0]))));
        // Not past the end.
        QVERIFY(!trackClipCorners(p, s, over, pin, frames - 1, true, keys, {}, nullptr, &err));
    }

    void multicamSpeakerSwitchAndAudioAngles() {
        // Two people with a microphone each: A talks for 3 s, then B for 3 s,
        // then both at once for 2 s, then silence.
        auto aTalks = [](double t) { return t < 3 || (t >= 6 && t < 8); };
        auto bTalks = [](double t) { return (t >= 3 && t < 8); };
        writeVoiceWav(path("lavA.wav"), 10, 220, aTalks, bTalks);
        writeVoiceWav(path("lavB.wav"), 10, 330, bTalks, aTalks);
        Project p = makeDefaultProject();
        Sequence& s0 = *p.active();
        s0.fps = {30, 1};
        auto camera = [&](const char* name) {
            MediaItem m;
            m.id = p.newId();
            m.name = name;
            m.path = std::string("/nonexistent/") + name;
            m.hasVideo = true;
            m.duration = 10;
            m.width = 320;
            m.height = 180;
            m.fps = {30, 1};
            p.media.push_back(m);
            return m.id;
        };
        const Id camA = camera("CamA.mov"), camB = camera("CamB.mov"), wide = camera("Wide.mov");
        p.media.push_back(probeOrFail(p, path("lavA.wav")));
        const Id lavA = p.media.back().id;
        p.media.push_back(probeOrFail(p, path("lavB.wav")));
        const Id lavB = p.media.back().id;
        std::string err;
        const Id mcId = makeMulticam(p, {camA, camB, wide, lavA, lavB}, {0, 0, 0, 0, 0}, "Talk", &err);
        QVERIFY2(mcId, err.c_str());
        const Sequence& mc = *p.findSequence(p.findMedia(mcId)->sequenceId);
        QCOMPARE(mc.audioTracks.size(), size_t(2));

        AutoSwitchOptions o;
        o.listen = {0, 1, -1};  // A's close-up hears lav A, B's hears lav B
        o.wideAngle = 2;
        auto changes = speakerAngleChanges(p, mc, o, &err);
        QVERIFY2(!changes.empty(), err.c_str());
        QString got;
        for (auto [f, a] : changes) got += QString("(%1,%2) ").arg(f).arg(a);
        QCOMPARE(changes.size(), size_t(3));
        QCOMPARE(changes[0], (std::pair<FrameTime, int>{0, 0}));
        QVERIFY2(changes[1].second == 1 && changes[1].first >= 88 && changes[1].first <= 105, qPrintable(got));
        QVERIFY2(changes[2].second == 2 && changes[2].first >= 178 && changes[2].first <= 195, qPrintable(got));
        // A longer minimum shot holds A until 4 s, and the wide until B has had 4 s.
        o.minShotSeconds = 4;
        changes = speakerAngleChanges(p, mc, o, &err);
        got.clear();
        for (auto [f, a] : changes) got += QString("(%1,%2) ").arg(f).arg(a);
        QCOMPARE(changes.size(), size_t(3));
        QVERIFY2(changes[1].first == 120 && changes[1].second == 1, qPrintable(got));
        QVERIFY2(changes[2].first == 240 && changes[2].second == 2, qPrintable(got));
        // Without a wide angle the last speaker keeps the shot through cross-talk and silence.
        o.minShotSeconds = 2;
        o.wideAngle = -1;
        changes = speakerAngleChanges(p, mc, o, &err);
        QCOMPARE(changes.size(), size_t(2));
        // Nobody to listen to.
        o.listen = {-1, -1, -1};
        QVERIFY(speakerAngleChanges(p, mc, o, &err).empty());
        QVERIFY(!err.empty());

        // The same scene from one recording's speaker labels instead of two microphones:
        // A's words for 3 s, B's for 3 s, then both for 2 s.
        {
            auto t = std::make_shared<Transcript>();
            auto say = [&](double from, double to, int speaker) {
                TranscriptSegment seg;
                seg.speaker = speaker;
                for (double w = from; w + 0.45 <= to; w += 0.5) seg.words.push_back({w, w + 0.45, "word", 1});
                seg.start = from;
                seg.end = to;
                t->segments.push_back(seg);
            };
            say(0, 3, 0);
            say(3, 6, 1);
            say(6, 8, 0);
            say(6, 8, 1);
            p.findMedia(lavA)->transcript = t;
            int speakers = 0;
            const auto turns = transcriptTurns(p, mc, 0, &speakers);
            QCOMPARE(speakers, 2);
            QVERIFY(transcriptTurns(p, mc, 1).empty());  // lav B has no transcript
            AutoSwitchOptions lo;
            lo.wideAngle = 2;
            auto byLabels = turnAngleChanges(mc, turns, {0, 1}, lo, &err);
            got.clear();
            for (auto [f, a] : byLabels) got += QString("(%1,%2) ").arg(f).arg(a);
            QCOMPARE(byLabels.size(), size_t(3));
            QCOMPARE(byLabels[0], (std::pair<FrameTime, int>{0, 0}));
            QVERIFY2(byLabels[1].second == 1 && byLabels[1].first >= 88 && byLabels[1].first <= 105, qPrintable(got));
            QVERIFY2(byLabels[2].second == 2 && byLabels[2].first >= 178 && byLabels[2].first <= 195, qPrintable(got));
            // B without a close-up: A keeps the floor alone, the wide shot covers B.
            byLabels = turnAngleChanges(mc, turns, {0, -1}, lo, &err);
            got.clear();
            for (auto [f, a] : byLabels) got += QString("(%1,%2) ").arg(f).arg(a);
            QVERIFY2(byLabels.size() >= 2 && byLabels[1].second == 2, qPrintable(got));
            QVERIFY(turnAngleChanges(mc, turns, {-1, -1}, lo, &err).empty() && !err.empty());
            p.findMedia(lavA)->transcript.reset();
        }

        // Audio angles: the mix, or one microphone.
        Sequence& s = *p.active();
        QVERIFY(edit::placeMedia(p, s, mcId, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto rms = [&](int audioAngle, double at) {
            trackAt(s, {TrackKind::Audio, 0})->clips[0].audioAngle = audioAngle;
            AudioMixer mixer;
            std::vector<float> out(4800 * 2);
            mixer.mix(p, s, int64_t(at * 48000), 4800, out.data());
            double sum = 0;
            for (float v : out) sum += double(v) * v;
            return std::sqrt(sum / double(out.size()));
        };
        QVERIFY(rms(-1, 1.0) > 0.15);  // A talking, whole mix
        QVERIFY(rms(0, 1.0) > 0.15);   // lav A
        QVERIFY(rms(1, 1.0) < 0.03);   // lav B hears A faintly
        QVERIFY(rms(1, 4.0) > 0.15);
    }

    void hdrExportRoundTrip() {
        if (!avcodec_find_encoder_by_name("libx265")) QSKIP("This FFmpeg has no libx265");
        // Graphics white in an HDR10 sequence.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        s.colorSpace = "rec2100pq";
        s.hdrPeakNits = 1000;
        Clip c = makeGeneratorClip(p, "color", 10);
        c.generator.params["color.r"] = 1.0;
        c.generator.params["color.g"] = 1.0;
        c.generator.params["color.b"] = 1.0;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        ExportSettings st;
        st.path = path("hdr10.mp4");
        st.videoCodec = "libx265";
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());

        // 10-bit, tagged BT.2020 / PQ, with HDR10 mastering display and light levels.
        AVFormatContext* fmt = nullptr;
        QCOMPARE(avformat_open_input(&fmt, st.path.c_str(), nullptr, nullptr), 0);
        QVERIFY(avformat_find_stream_info(fmt, nullptr) >= 0);
        const AVCodecParameters* cp = fmt->streams[0]->codecpar;
        const int format = cp->format;
        const auto trc = cp->color_trc;
        const auto primaries = cp->color_primaries;
        const auto matrix = cp->color_space;
        double maxLum = 0;
        unsigned maxCll = 0;
        if (const AVPacketSideData* sd = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data,
                                                                  AV_PKT_DATA_MASTERING_DISPLAY_METADATA))
            maxLum = av_q2d(reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data)->max_luminance);
        if (const AVPacketSideData* sd =
                av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL))
            maxCll = reinterpret_cast<const AVContentLightMetadata*>(sd->data)->MaxCLL;
        avformat_close_input(&fmt);
        QCOMPARE(format, int(AV_PIX_FMT_YUV420P10LE));
        QCOMPARE(trc, AVCOL_TRC_SMPTE2084);
        QCOMPARE(primaries, AVCOL_PRI_BT2020);
        QCOMPARE(matrix, AVCOL_SPC_BT2020_NCL);
        QCOMPARE(maxLum, 1000.0);
        QCOMPARE(maxCll, 1000u);

        // Montage reads it back as HDR10, at reference white (203 nits).
        MediaItem m;
        QVERIFY2(probeMedia(st.path, m, &err), err.c_str());
        QCOMPARE(m.colorSpace, std::string("rec2100pq"));
        VideoDecoder dec;
        QVERIFY(dec.open(st.path, &err));
        Frame16Ptr f = dec.frameAt(0.1);
        QVERIFY(f);
        const double code = f->px[(size_t(90) * 320 + 160) * 4] / 65535.0;
        QVERIFY2(std::fabs(code - 0.5807) < 0.01, qPrintable(QString::number(code)));
        // In a Rec.709 sequence it is tone mapped: white comes back near white.
        Project q = makeDefaultProject();
        Sequence& qs = *q.active();
        qs.width = 320;
        qs.height = 180;
        qs.fps = {25, 1};
        m.id = q.newId();
        q.media.push_back(m);
        QVERIFY(edit::placeMedia(q, qs, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        RenderOptions ro;
        Image img = renderProgramFrame(q, qs, 2, ro);
        QVERIFY2(img.at(160, 90)[0] > 0.93f && img.at(160, 90)[0] <= 1.0f, qPrintable(QString::number(img.at(160, 90)[0])));
        // Interpreted as Rec.709 instead, the PQ code values show as they are (the flat look).
        q.media.back().colorOverride = "rec709";
        img = renderProgramFrame(q, qs, 2, ro);
        QVERIFY(std::fabs(img.at(160, 90)[0] - 0.5807f) < 0.01f);

        // The HDR sequence delivered in SDR: Rec.709, 8-bit, tone mapped.
        st.path = path("sdr-version.mp4");
        st.colorSpace = "rec709";
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        MediaItem sdr;
        QVERIFY2(probeMedia(st.path, sdr, &err), err.c_str());
        QVERIFY(sdr.colorSpace.empty());  // Rec.709
        VideoDecoder dec2;
        QVERIFY(dec2.open(st.path, &err));
        f = dec2.frameAt(0.1);
        QVERIFY(f);
        const double white = f->px[(size_t(90) * 320 + 160) * 4] / 65535.0;
        QVERIFY2(white > 0.93 && white <= 1.0, qPrintable(QString::number(white)));
    }

    void captionsBurnInAndEmbed() {
        // A mid-grey clip with two captions.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        Clip c = makeGeneratorClip(p, "color", 50);
        c.generator.params["color.r"] = 0.3;
        c.generator.params["color.g"] = 0.3;
        c.generator.params["color.b"] = 0.3;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        CaptionTrack ct;
        ct.id = p.newId();
        ct.name = "English";
        ct.style.size = 0.1;
        ct.captions = {{5, 20, "Hello world"}, {30, 45, "Second {line}\nof text"}};
        s.captionTracks.push_back(ct);

        // The viewer draws the caption only when asked, and only while it is on screen.
        RenderOptions ro;
        Image plain = renderProgramFrame(p, s, 10, ro);
        ro.captions = true;
        Image withCaption = renderProgramFrame(p, s, 10, ro);
        Image between = renderProgramFrame(p, s, 25, ro);
        auto bright = [](const Image& img) {
            int n = 0;
            for (int y = img.height * 3 / 4; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) n += img.at(x, y)[0] > 0.9f ? 1 : 0;
            return n;
        };
        QCOMPARE(bright(plain), 0);
        QVERIFY2(bright(withCaption) > 30, qPrintable(QString::number(bright(withCaption))));  // white text, bottom quarter
        QCOMPARE(bright(between), 0);

        // MP4: burned in and embedded as mov_text with the language.
        ExportSettings st;
        st.path = path("captions.mp4");
        st.audioCodec = "none";
        st.preset = "ultrafast";
        st.burnInCaptions = true;
        st.embedCaptions = true;
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        std::string codec, lang;
        auto events = readSubtitles(st.path, nullptr, &codec, &lang);
        QCOMPARE(QString::fromStdString(codec), QString("mov_text"));
        QCOMPARE(QString::fromStdString(lang), QString("eng"));
        QCOMPARE(events.size(), size_t(2));
        QCOMPARE(events[0].text, QString("Hello world"));
        QVERIFY(std::fabs(events[0].start - 0.2) < 0.02 && std::fabs(events[0].end - 0.8) < 0.02);
        // Braces stay literal text (the decoder escapes them again for ASS).
        QCOMPARE(events[1].text, QString("Second \\{line\\}\\Nof text"));
        VideoDecoder dec;
        QVERIFY(dec.open(st.path, &err));
        Frame16Ptr f = dec.frameAt(10 / 25.0);
        QVERIFY(f);
        int burned = 0;
        for (int y = 135; y < 180; ++y)
            for (int x = 0; x < 320; ++x) burned += f->px[(size_t(y) * 320 + x) * 4] > 0.85 * 65535 ? 1 : 0;
        QVERIFY2(burned > 20, qPrintable(QString::number(burned)));

        // An export range shifts the captions; MKV gets SubRip.
        st.path = path("captions.mkv");
        st.burnInCaptions = false;
        st.in = 25;
        st.out = 50;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        events = readSubtitles(st.path, nullptr, &codec);
        QVERIFY2(codec == "subrip" || codec == "srt", codec.c_str());  // the decoder's name varies by FFmpeg version
        QCOMPARE(events.size(), size_t(1));
        QVERIFY(std::fabs(events[0].start - 0.2) < 0.02);

        // A WAV cannot carry captions: say so.
        st.path = path("captions.wav");
        st.videoCodec = "none";
        st.audioCodec = "pcm_s16le";
        QVERIFY(!exportSequence(p, s, st, nullptr, nullptr, &err));
        QVERIFY(QString::fromStdString(err).contains("captions"));
    }

    void sccDecodesWithFfmpeg() {
        // FFmpeg's own SCC demuxer and CEA-608 decoder read back what we write.
        const Rational fps{30000, 1001};
        std::vector<Caption> caps = {{60, 150, "HELLO WORLD"}, {300, 390, "Two lines\nof text"}, {420, 480, "Caf\xC3\xA9 \xE2\x99\xAA"}};
        const std::string file = path("captions.scc");
        {
            QFile f(QString::fromStdString(file));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(captionsToScc(caps, fps)));
        }
        std::string codec;
        auto events = readSubtitles(file, "scc", &codec);
        QCOMPARE(QString::fromStdString(codec), QString("cc_dec"));
        QStringList texts;
        for (const auto& e : events) texts << e.text;
        const QString all = texts.join(" | ");
        qInfo("608: %s", qPrintable(all));
        QVERIFY2(all.contains("HELLO WORLD"), qPrintable(all));
        QVERIFY2(all.contains("Two lines") && all.contains("of text"), qPrintable(all));
        QVERIFY2(all.contains(QString::fromUtf8("Caf\xC3\xA9")), qPrintable(all));
        // Loading starts early so that a decoder taking one byte pair per frame shows the
        // caption at 2 s; FFmpeg's decoder shows it as soon as the line is read.
        for (const auto& e : events)
            if (e.text.contains("HELLO")) QVERIFY2(e.start > 1.4 && e.start < 2.05, qPrintable(QString::number(e.start)));
    }

    void captionFilesReadBack() {
        // FFmpeg's ASS demuxer and decoder read what we write, on the same times.
        const Rational fps{25, 1};
        const std::vector<Caption> caps = {{25, 75, "Hello, world\nsecond line"}, {100, 150, "Caf\xC3\xA9"}};
        const std::string file = path("captions.ass");
        {
            QFile f(QString::fromStdString(file));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(captionsToAss(caps, fps, CaptionStyle{}, 1920, 1080)));
        }
        std::string codec;
        const auto events = readSubtitles(file, "ass", &codec);
        QVERIFY2(codec == "ass" || codec == "ssa", codec.c_str());
        QCOMPARE(events.size(), size_t(2));
        QVERIFY(std::fabs(events[0].start - 1.0) < 0.011 && std::fabs(events[0].end - 3.0) < 0.011);
        QVERIFY(std::fabs(events[1].start - 4.0) < 0.011 && std::fabs(events[1].end - 6.0) < 0.011);
        QVERIFY2(events[0].text.contains("Hello, world\\Nsecond line"), qPrintable(events[0].text));
        QVERIFY2(events[1].text.contains(QString::fromUtf8("Caf\xC3\xA9")), qPrintable(events[1].text));

        // Through MCP: a track written as TTML and EBU STL, and each read back as a new track.
        Project p = makeDefaultProject();
        CaptionTrack ct;
        ct.id = p.newId();
        ct.name = "English";
        ct.captions = {{30, 90, "First caption"}, {120, 180, "Second\ncaption"}};
        p.active()->captionTracks.push_back(ct);
        const QString project = QString::fromStdString(path("caption-files.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_captions"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        const QString ttml = QString::fromStdString(path("caption-files.ttml")), stl = QString::fromStdString(path("caption-files.stl"));
        QJsonObject r = call({{"project", project}, {"export", ttml}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("captions").toInt(), 2);
        r = call({{"project", project}, {"export", stl}, {"track", 0}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        {
            QFile f(stl);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.size(), qint64(1024 + 2 * 128));
            QCOMPARE(f.read(6).mid(3), QByteArray("STL"));
        }
        r = call({{"project", project}, {"import", stl}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = call({{"project", project}, {"import", ttml}, {"language", "en"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const auto& tracks = back.active()->captionTracks;
        QCOMPARE(tracks.size(), size_t(3));
        QCOMPARE(tracks[1].name, std::string("caption-files"));
        QCOMPARE(tracks[2].language, std::string("en"));
        for (size_t i = 1; i < 3; ++i) {
            QCOMPARE(tracks[i].captions.size(), size_t(2));
            for (size_t k = 0; k < 2; ++k) {
                QCOMPARE(tracks[i].captions[k].text, ct.captions[k].text);
                QCOMPARE(tracks[i].captions[k].start, ct.captions[k].start);
                QCOMPARE(tracks[i].captions[k].end, ct.captions[k].end);
            }
        }
        // Checking and fixing through montage_edit_captions.
        auto edit = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_edit_captions"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        r = edit({{"project", project}, {"action", "check"}, {"max_cps", 5}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonArray flagged = r.value("structuredContent").toObject().value("issues").toArray();
        QCOMPARE(flagged.size(), 2);  // "First caption" and "Second\ncaption" in two seconds each: 6.5 and 7 a second
        QVERIFY(flagged[0].toObject().value("issues").toString().contains("characters a second"));
        r = edit({{"project", project}, {"action", "shift"}, {"by", -0.5}, {"captions", QJsonArray{1}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = edit({{"project", project}, {"action", "replace"}, {"find", "caption"}, {"replace", "line"}, {"whole_words", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        r = edit({{"project", project}, {"action", "fix_timing"}, {"max_cps", 3}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        {
            Project fixed;
            QVERIFY(loadProject(project.toStdString(), fixed));
            const auto& c = fixed.active()->captionTracks.front().captions;
            const FrameTime half = FrameTime(std::llround(fixed.active()->fpsValue() / 2));
            QCOMPARE(c[1].start, FrameTime(120) - half);
            QCOMPARE(c[0].text, std::string("First line"));
            QCOMPARE(c[1].text, std::string("Second\nline"));
            QCOMPARE(c[0].end, c[1].start - 2);  // stretched for a slow reader up to the gap
        }
        // Placing captions: the second at the top left, kept when exported.
        r = edit({{"project", project}, {"action", "place"}, {"place", "top left"}, {"captions", QJsonArray{1}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(edit({{"project", project}, {"action", "place"}, {"place", "upside down"}}).value("isError").toBool());
        r = edit({{"project", project}, {"action", "raise_over_titles"}});
        QVERIFY2(!r.value("isError").toBool() && r.value("structuredContent").toObject().value("changed").toInt(-1) == 0,
                 QJsonDocument(r).toJson().constData());
        {
            Project placed;
            QVERIFY(loadProject(project.toStdString(), placed));
            const auto& c = placed.active()->captionTracks.front().captions;
            QCOMPARE(captionKeypad(c[0]), 2);
            QCOMPARE(captionKeypad(c[1]), 7);
        }
        const QString placedSrt = QString::fromStdString(path("placed.srt"));
        r = call({{"project", project}, {"export", placedSrt}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        {
            QFile f(placedSrt);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray text = f.readAll();
            QVERIFY2(text.contains("{\\an7}Second") && !text.contains("{\\an2}"), text.constData());
        }
        QVERIFY(edit({{"project", project}, {"action", "spin"}}).value("isError").toBool());
        QVERIFY(edit({{"project", project}, {"action", "replace"}, {"find", "zebra"}, {"replace", "x"}}).value("isError").toBool());

        // An unknown format and a missing track are refused.
        r = call({{"project", project}, {"export", QString::fromStdString(path("x.docx"))}});
        QVERIFY(r.value("isError").toBool());
        r = call({{"project", project}, {"export", ttml}, {"track", 9}});
        QVERIFY(r.value("isError").toBool());
    }

    void translation() {
        // Languages and routes: direct models, English as the bridge between two others.
        const auto& langs = translationLanguages();
        QVERIFY(langs.size() >= 20);
        QVERIFY(std::any_of(langs.begin(), langs.end(), [](const auto& l) { return l.code == "de" && l.name == "German"; }));
        QCOMPARE(translationRoute("en", "de").size(), size_t(1));
        QCOMPARE(translationRoute("de", "fr").size(), size_t(2));
        QVERIFY(translationRoute("en", "en").empty());
        QVERIFY(translationRoute("en", "xx").empty());
        const ModelPack* ende = translationModel("en", "de");
        QVERIFY(ende && ende->files.size() == 5 && ende->bytes() > 200'000'000);
        if (!translatorAvailable() || !ende->installed())
            QSKIP("Set MONTAGE_TRANSLATION_MODELS to a folder with translate-en-de to run the rest");
        // SentencePiece splits as the reference implementation does.
        SentencePiece sp;
        std::string err;
        QVERIFY2(sp.load(ende->path(ende->files[2]), &err), err.c_str());
        QVERIFY(sp.size() > 30000);  // the source side (the vocabulary is shared with the target)
        QCOMPARE(sp.encode("The meeting starts at nine o'clock tomorrow morning."),
                 (std::vector<std::string>{"▁The", "▁meeting", "▁starts", "▁at", "▁nine", "▁o", "'", "clock",
                                           "▁tomorrow", "▁morning", "."}));
        QCOMPARE(sp.encode("  Hello   world "), (std::vector<std::string>{"▁Hello", "▁world"}));
        // Translations as the reference gives them.
        std::vector<std::string> out;
        QVERIFY2(translateTexts({"And so, my fellow Americans, ask not what your country can do for you.",
                                 "The meeting starts at nine o'clock tomorrow morning.", "Hello world", ""},
                                "en", "de", out, {}, nullptr, &err),
                 err.c_str());
        QCOMPARE(out.size(), size_t(4));
        QCOMPARE(QString::fromStdString(out[0]), QString("Und so, meine amerikanischen Kollegen, fragen Sie nicht, was Ihr Land für Sie tun kann."));
        QCOMPARE(QString::fromStdString(out[1]), QString("Das Treffen beginnt morgen früh um neun Uhr."));
        QCOMPARE(QString::fromStdString(out[2]), QString("Hallo Welt"));
        QCOMPARE(out[3], std::string());
        // A caption track, translated through MCP: same timings, German text, a new hidden track.
        Project p = makeDefaultProject();
        CaptionTrack ct;
        ct.id = p.newId();
        ct.captions = {{0, 60, "Hello world", {0.0, 0.5}}, {60, 150, "The meeting starts at nine\no'clock tomorrow morning.", {}}};
        p.active()->captionTracks.push_back(ct);
        const QString project = QString::fromStdString(path("captions.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_translate_captions"},
                                                     {"arguments", QJsonObject{{"project", project}, {"to", "de"}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        QCOMPARE(back.active()->captionTracks.size(), size_t(2));
        const CaptionTrack& de = back.active()->captionTracks[1];
        QCOMPARE(de.language, std::string("de"));
        QCOMPARE(de.name, std::string("Subtitles (German)"));
        QVERIFY(!de.visible);
        QCOMPARE(de.captions.size(), size_t(2));
        QCOMPARE(de.captions[1].start, FrameTime(60));
        QCOMPARE(de.captions[1].end, FrameTime(150));
        QCOMPARE(QString::fromStdString(de.captions[0].text), QString("Hallo Welt"));
        QVERIFY(de.captions[0].wordTimes.empty());
        QVERIFY(QString::fromStdString(de.captions[1].text).simplified().startsWith("Das Treffen beginnt"));

        // A pair whose model is not here says so.
        QVERIFY(!translateTexts({"Hallo"}, "de", "en", out, {}, nullptr, &err) || translationModel("de", "en")->installed());
    }

    void dubIntoEnglish() {
        const ModelPack* deen = translationModel("de", "en");
        QVERIFY(deen);
        if (!translatorAvailable() || !deen->installed() || !ttsAvailable() || !ttsModel().installed())
            QSKIP("Set MONTAGE_TRANSLATION_MODELS (with translate-de-en) and MONTAGE_TTS_MODEL to test dubbing");
        // German captions over the original speech on A1.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem speech = probeOrFail(p, MONTAGE_TEST_DATA_DIR "/jfk.wav");
        p.media.push_back(speech);
        QVERIFY(edit::placeMedia(p, s, speech.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        CaptionTrack de;
        de.id = p.newId();
        de.language = "de";
        de.captions = {{0, 50, "Hallo Welt", {}}, {150, 250, "Das Treffen beginnt morgen früh um neun Uhr.", {}}};
        s.captionTracks.push_back(de);
        const size_t tracks = s.audioTracks.size();
        const QString project = QString::fromStdString(path("dub.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_dub"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"voice", "am_michael"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("audio_track").toString(), QStringLiteral("A%1").arg(tracks + 1));
        Project back;
        std::string err;
        QVERIFY(loadProject(project.toStdString(), back, &err));
        const Sequence& bs = *back.active();
        // English captions, same timings, hidden.
        QCOMPARE(bs.captionTracks.size(), size_t(2));
        const CaptionTrack& en = bs.captionTracks[1];
        QCOMPARE(en.language, std::string("en"));
        QVERIFY(!en.visible);
        QVERIFY2(QString::fromStdString(en.captions[0].text).toLower().contains("hello"), en.captions[0].text.c_str());
        QVERIFY2(QString::fromStdString(en.captions[1].text).toLower().contains("meeting"), en.captions[1].text.c_str());
        QCOMPARE(en.captions[1].start, FrameTime(150));
        // Spoken at each cue on a new track, each before the next cue.
        QCOMPARE(bs.audioTracks.size(), tracks + 1);
        const Track& dub = bs.audioTracks[tracks];
        QCOMPARE(dub.name, std::string("Dub (English)"));
        QCOMPARE(dub.clips.size(), size_t(2));
        QCOMPARE(dub.clips[0].start, FrameTime(0));
        QCOMPARE(dub.clips[1].start, FrameTime(150));
        QVERIFY(dub.clips[0].end() <= 152);
        // The original dips 18 dB while the dub speaks and is back up between the lines.
        const Clip& original = bs.audioTracks[0].clips[0];
        const Param& gain = original.audio.params.at("gain_db");
        QVERIFY(!gain.keys.empty());
        QVERIFY(std::abs(gain.at(10) + 18) < 0.01);
        QVERIFY(std::abs(gain.at(200 - original.start) + 18) < 0.01);
        const FrameTime between = (dub.clips[0].end() + 150) / 2;
        QVERIFY2(dub.clips[0].end() + 30 < 150 - 10, "the lines are far enough apart to come back up between them");
        QVERIFY2(std::abs(gain.at(between)) < 0.01, qPrintable(QString::number(gain.at(between))));

        // English already: spoken as it is, and nothing ducked when asked.
        r = call({{"project", project}, {"track", 1}, {"duck_db", 0}, {"voice", "bf_emma"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("ducked").toInt(), 0);
        QVERIFY(loadProject(project.toStdString(), back, &err));
        QCOMPARE(back.active()->captionTracks.size(), size_t(2));
        QCOMPARE(back.active()->audioTracks.size(), tracks + 2);
        QCOMPARE(back.active()->audioTracks[tracks + 1].clips.size(), size_t(2));
        QVERIFY(call({{"project", project}, {"track", 5}}).value("isError").toBool());
    }

    void audioRolesAndAutoMix() {
        // Speech (the JFK clip), music (the test song), and effects (door slams over room tone, pink-ish noise).
        std::vector<float> speech;
        std::string err;
        QVERIFY2(decodeMono(MONTAGE_TEST_DATA_DIR "/jfk.wav", 48000, speech, nullptr, &err), err.c_str());
        const std::vector<int> chords = {0, 0, 1, 2, 1, 2, 0, 3, 0, 3, 1, 2};
        const std::vector<float> music = testSong(48000, 0.2, chords);
        std::vector<float> effects(size_t(48000 * 12), 0.0f);
        unsigned seed = 11;
        float lp = 0;
        for (size_t i = 0; i < effects.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            lp = 0.98f * lp + 0.02f * (float(seed >> 8) / float(1 << 24) - 0.5f);
            effects[i] = 0.3f * lp;
        }
        for (double t : {1.3, 4.1, 8.7})  // slams
            for (size_t i = 0; i < size_t(0.4 * 48000); ++i)
                effects[size_t(t * 48000) + i] += float(0.7 * std::sin(2 * M_PI * 70 * double(i) / 48000) * std::exp(-double(i) / 2400.0));
        const RoleGuess gs = classifyAudio(speech, 48000), gm = classifyAudio(music, 48000), ge = classifyAudio(effects, 48000);
        for (const RoleGuess* g : {&gs, &gm, &ge})
            qInfo("%s: speech %.2f music %.2f (pauses %.2f, syllabic %.2f, beat %.2f)", audioRoleName(g->role), g->speech, g->music,
                  g->pauses, g->syllabic, g->beat);
        QCOMPARE(int(gs.role), int(AudioRole::Dialogue));
        QCOMPARE(int(gm.role), int(AudioRole::Music));
        QCOMPARE(int(ge.role), int(AudioRole::Effects));
        QCOMPARE(int(classifyAudio(std::vector<float>(48000 * 3, 0.0f), 48000).role), int(AudioRole::Silence));
        // A transcript's words settle it.
        QCOMPARE(int(classifyAudio(effects, 48000, 2.5).role), int(AudioRole::Dialogue));

        // A sequence: JFK loud on A1, then JFK 12 dB quieter whose second half drops another 9 dB,
        // and music under both on A2.
        std::vector<float> quiet(speech.size());
        for (size_t i = 0; i < speech.size(); ++i) quiet[i] = speech[i] * float(std::pow(10.0, (i < speech.size() / 2 ? -12 : -21) / 20.0));
        const std::string loudWav = path("jfk-loud.wav"), quietWav = path("jfk-quiet.wav"), songWav = path("bed.wav");
        QVERIFY(writeMonoWav(loudWav, speech, 48000) && writeMonoWav(quietWav, quiet, 48000));
        std::vector<float> bed = testSong(48000, 0.0, std::vector<int>(14, 0));
        QVERIFY(writeMonoWav(songWav, bed, 48000));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        auto add = [&](const std::string& file) -> Id {
            MediaItem m;
            if (!probeMedia(file, m)) return 0;
            m.id = p.newId();
            m.hasVideo = false;
            p.media.push_back(m);
            return m.id;
        };
        const Id loudId = add(loudWav), quietId = add(quietWav), bedId = add(songWav);
        QVERIFY(loudId && quietId && bedId);
        QVERIFY(edit::placeMedia(p, s, loudId, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        const FrameTime second = s.audioTracks[0].clips[0].end() + FrameTime(2 * s.fpsValue());
        QVERIFY(edit::placeMedia(p, s, quietId, second, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QVERIFY(edit::placeMedia(p, s, bedId, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 1}, false).ok);
        MixOptions o;
        std::string planErr;
        const auto plan = planMix(p, s, o, {}, nullptr, &planErr);
        QCOMPARE(plan.size(), size_t(3));
        const ClipMix* loud = nullptr; const ClipMix* soft = nullptr; const ClipMix* mus = nullptr;
        for (const ClipMix& m : plan) {
            const Clip* c = edit::clipById(s, m.clip);
            if (c->mediaId == loudId) loud = &m;
            if (c->mediaId == quietId) soft = &m;
            if (c->mediaId == bedId) mus = &m;
        }
        QVERIFY(loud && soft && mus);
        QCOMPARE(int(loud->role), int(AudioRole::Dialogue));
        QCOMPARE(int(soft->role), int(AudioRole::Dialogue));
        QCOMPARE(int(mus->role), int(AudioRole::Music));
        // Each clip is brought to its level: the quiet one gets about 12 dB more (a bit more, for its drop).
        QVERIFY(std::fabs(loud->guess.loudness + loud->gainDb - o.dialogueLufs) < 0.01);
        QVERIFY2(soft->gainDb - loud->gainDb > 12 && soft->gainDb - loud->gainDb < 18,
                 qPrintable(QString::number(soft->gainDb - loud->gainDb)));
        QVERIFY(std::fabs(mus->guess.loudness + mus->gainDb - o.musicLufs) < 0.01);
        // The ride lifts the quiet half of the second clip, within the range.
        QVERIFY(!soft->ride.empty());
        double early = 0, late = 0;
        const FrameTime half = edit::clipById(s, soft->clip)->duration / 2;
        for (const auto& [f, db] : soft->ride) {
            QVERIFY(std::fabs(db) <= o.rideRangeDb + 1e-9);
            if (f < half - 60) early = db;
            if (f > half + 90) late = std::max(late, db);
        }
        QVERIFY2(late - early > 4, qPrintable(QString("%1 -> %2").arg(early).arg(late)));
        // Applied: levels and ride on the clips, and the music dips under the speech.
        QCOMPARE(applyMix(p, s, plan, o), 3);
        const Clip* bedClip = edit::clipById(s, mus->clip);
        const Param& g = bedClip->audio.params.at("gain_db");
        QVERIFY(g.animated());
        const double underSpeech = g.at(FrameTime(2.0 * s.fpsValue())), base = mus->gainDb;
        QVERIFY2(std::fabs(underSpeech - (base + o.duckDb)) < 0.5, qPrintable(QString("%1 vs %2").arg(underSpeech).arg(base)));
        QVERIFY(edit::clipById(s, soft->clip)->audio.params.at("gain_db").animated());
        // The clips remember what they were mixed as (their audio role), and a role given by hand wins over the ear.
        QCOMPARE(bedClip->role, std::string("Music"));
        QCOMPARE(edit::clipById(s, loud->clip)->role, std::string("Dialogue"));
        {
            Sequence tagged = s;
            edit::clipById(tagged, loud->clip)->role = "Effects";
            for (const ClipMix& m : planMix(p, tagged, o))
                if (m.clip == loud->clip) {
                    QCOMPARE(int(m.role), int(AudioRole::Effects));
                    QCOMPARE(int(m.guess.role), int(AudioRole::Dialogue));
                }
        }

        // Through MCP: a dry run lists the roles; calling the music "effects" puts it at the effects level.
        const QString project = QString::fromStdString(path("mix.montage"));
        Project fresh = p;
        for (Track& t : fresh.active()->audioTracks)
            for (Clip& c : t.clips) {
                c.audio.params.erase("gain_db");
                c.role.clear();
            }
        QVERIFY(saveProject(fresh, project.toStdString()));
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_auto_mix"}, {"arguments", args},
                                                         {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                               {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call({{"project", project}, {"dry_run", true}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("clips").toArray().size(), 3);
        r = call({{"project", project}, {"roles", QJsonObject{{QString::number(mus->clip), "effects"}}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Clip* bedBack = edit::clipById(*back.active(), mus->clip);
        QVERIFY(std::fabs(bedBack->audio.params.at("gain_db").at(0) - (o.effectsLufs - mus->guess.loudness)) < 0.01);
        QVERIFY(!bedBack->audio.params.at("gain_db").animated());  // effects are not ducked
    }

    void matchVoiceEq() {
        // JFK as recorded, and as if on a thin, bright microphone: lows down 8 dB, presence up 6 dB.
        std::vector<float> ref;
        std::string err;
        QVERIFY2(decodeMono(MONTAGE_TEST_DATA_DIR "/jfk.wav", 48000, ref, nullptr, &err), err.c_str());
        fx::ParametricEq mic;
        mic.set(48000, {120, -8, 1}, {300, 0, 0.9}, {1200, 0, 0.9}, {4000, 6, 0.9}, {8000, 0, 1}, 0);
        std::vector<float> stereo(ref.size() * 2);
        for (size_t i = 0; i < ref.size(); ++i) stereo[i * 2] = stereo[i * 2 + 1] = ref[i];
        mic.process(stereo.data(), int(ref.size()));
        std::vector<float> other(ref.size());
        for (size_t i = 0; i < ref.size(); ++i) other[i] = stereo[i * 2];
        const auto a = speechSpectrum(ref, 48000), b = speechSpectrum(other, 48000);
        const VoiceEq eq = fitVoiceEq(b, a);
        qInfo("match voice: low %+.1f, 300 Hz %+.1f, 1.2 kHz %+.1f, 4 kHz %+.1f, high %+.1f dB; %.1f -> %.1f dB apart", eq.lowDb,
              eq.b1Db, eq.b2Db, eq.b3Db, eq.highDb, eq.beforeDb, eq.afterDb);
        QVERIFY(eq.beforeDb > 2);
        QVERIFY(eq.afterDb < 0.5);
        QVERIFY(std::fabs(eq.lowDb - 8) < 2);
        QVERIFY(std::fabs(eq.b3Db + 6) < 2);
        for (double v : {eq.b1Db, eq.b2Db, eq.highDb}) QVERIFY(std::fabs(v) < 2.5);
        // The same voice needs nothing.
        const VoiceEq same = fitVoiceEq(a, a);
        QVERIFY(same.afterDb < 0.05 && std::fabs(same.lowDb) < 0.1 && std::fabs(same.b3Db) < 0.1);
        // As an effect, tagged so matching again replaces it.
        Project p = makeDefaultProject();
        const Effect e = voiceEqEffect(p, eq);
        QCOMPARE(e.type, std::string("parametric_eq"));
        QCOMPARE(e.s("match"), std::string("voice"));
        QCOMPARE(e.p("b3_db", 0), eq.b3Db);

        // Through MCP: the thin clip matched to the reference clip.
        const std::string refWav = path("voice-ref.wav"), thinWav = path("voice-thin.wav");
        QVERIFY(writeMonoWav(refWav, ref, 48000) && writeMonoWav(thinWav, other, 48000));
        Sequence& s = *p.active();
        Id ids[2];
        for (int i = 0; i < 2; ++i) {
            MediaItem m;
            QVERIFY(probeMedia(i == 0 ? refWav : thinWav, m));
            m.id = ids[i] = p.newId();
            p.media.push_back(m);
            QVERIFY(edit::placeMedia(p, s, m.id, i * 400, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        }
        const Id refClip = s.audioTracks[0].clips[0].id, thinClip = s.audioTracks[0].clips[1].id;
        const QString project = QString::fromStdString(path("voices.montage"));
        QVERIFY(saveProject(p, project.toStdString()));
        McpServer server;
        const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                              {"params", QJsonObject{{"name", "montage_match_voice"},
                                                     {"arguments", QJsonObject{{"project", project}, {"reference", double(refClip)},
                                                                               {"clips", QJsonArray{double(thinClip)}}}},
                                                     {"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}}}}}};
        const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        Project back;
        QVERIFY(loadProject(project.toStdString(), back));
        const Clip* matched = edit::clipById(*back.active(), thinClip);
        QVERIFY(matched && !matched->effects.empty() && matched->effects[0].s("match") == "voice");
        QVERIFY(std::fabs(matched->effects[0].p("b3_db", 0) + 6) < 2);
    }

    void beatsAndFittingMusic() {
        // A song: intro (C), verse (Am F), chorus (C G) twice, outro (G).
        const int rate = 44100;
        const double beat = kSongBeat, bar = kSongBar, lead = 0.5;
        struct Section { std::vector<int> chords; int bars; };  // chord ids, cycled bar by bar
        const std::vector<Section> song = {{{0}, 4}, {{1, 2}, 8}, {{0, 3}, 8}, {{1, 2}, 8}, {{0, 3}, 8}, {{3}, 4}};
        std::vector<int> barChord;
        for (const auto& sec : song)
            for (int b = 0; b < sec.bars; ++b) barChord.push_back(sec.chords[size_t(b) % sec.chords.size()]);
        const int bars = int(barChord.size());
        const std::vector<float> x = testSong(rate, lead, barChord);
        const double total = double(x.size()) / rate;
        const BeatGrid g = detectBeats(x, rate);
        qInfo("tempo %.2f BPM, %zu beats, %zu bars; beats %.3f %.3f ... %.3f %.3f; bars %.3f ... %.3f", g.tempo, g.beats.size(),
              g.downbeats.size(), g.beats[0], g.beats[1], g.beats[g.beats.size() - 2], g.beats.back(), g.downbeats.front(), g.downbeats.back());
        QVERIFY(std::fabs(g.tempo - 128) < 1);
        QVERIFY(g.beats.size() >= size_t(4 * bars - 4) && g.beats.size() <= size_t(4 * bars + 1));
        double worst = 0;
        double meanOff = 0;
        for (double b : g.beats) {
            meanOff += std::remainder(b - lead, beat) / double(g.beats.size());
            worst = std::max(worst, std::fabs(std::remainder(b - lead, beat)));
        }
        qInfo("beats are %.1f ms off on average, %.1f ms at worst", meanOff * 1000, worst * 1000);
        QVERIFY(std::fabs(meanOff) < 0.006);
        QVERIFY2(worst < 0.015, qPrintable(QString::number(worst)));
        int onBar = 0;
        for (double d : g.downbeats) onBar += std::fabs(std::remainder(d - lead, bar)) < 0.03;
        QVERIFY2(onBar >= int(g.downbeats.size()) - 1, qPrintable(QString("%1 of %2").arg(onBar).arg(g.downbeats.size())));

        auto barOf = [&](double t) { return int(std::lround((t - lead) / bar)); };
        auto check = [&](const MusicFit& f, double target) {
            QVERIFY(f.ok());
            qInfo("fit to %.1f s: %.2f s in %zu pieces, worst join %.2f", target, f.duration, f.segments.size(), f.similarity);
            QVERIFY(std::fabs(f.duration - target) <= bar / 2 + 1e-6);
            QCOMPARE(f.segments.front().in, 0.0);
            QVERIFY(std::fabs(f.segments.back().out - total) < 1e-6);
            // Every join is from a bar start to a bar start in the same place in the chords.
            for (size_t i = 0; i + 1 < f.segments.size(); ++i) {
                const int from = barOf(f.segments[i].out), to = barOf(f.segments[i + 1].in);
                QVERIFY(std::fabs(std::remainder(f.segments[i].out - lead, bar)) < 0.03);
                QVERIFY(std::fabs(std::remainder(f.segments[i + 1].in - lead, bar)) < 0.03);
                QVERIFY2(barChord[size_t(from)] == barChord[size_t(to)] && barChord[size_t(from - 1)] == barChord[size_t(to - 1)],
                         qPrintable(QString("bar %1 -> %2").arg(from).arg(to)));
                QVERIFY(f.segments[i].out >= 8 - 1e-6 && f.segments[i + 1].in <= total - 8 + 1e-6);
            }
        };
        const MusicFit shorter = fitMusic(x, rate, g, 50);
        check(shorter, 50);
        QVERIFY(shorter.segments.size() >= 2);
        const MusicFit longer = fitMusic(x, rate, g, 110);
        check(longer, 110);
        // Already the right length: left whole.
        const MusicFit same = fitMusic(x, rate, g, total);
        QCOMPARE(same.segments.size(), size_t(1));
        // No beat, no fit.
        QVERIFY(detectBeats(std::vector<float>(size_t(rate) * 5, 0.0f), rate).empty());
    }

    void noiseReductionAndVoiceIsolation() {
        // Speech (the public-domain JFK clip) plus steady white noise at about -32 dBFS.
        std::string err;
        AudioBufferPtr clean = decodeAudio(MONTAGE_TEST_DATA_DIR "/jfk.wav", 48000, &err);
        QVERIFY2(clean && clean->frames() > 48000 * 10, err.c_str());
        AudioBuffer noisy = *clean;
        uint32_t seed = 12345;
        for (float& v : noisy.samples) {
            seed = seed * 1664525u + 1013904223u;
            v += (float(seed >> 8) / float(1 << 24) - 0.5f) * 0.08f;
        }
        auto rms = [](const AudioBuffer& b, double t0, double t1) {
            double acc = 0;
            const int64_t a = int64_t(t0 * b.sampleRate), z = int64_t(t1 * b.sampleRate);
            for (int64_t i = a; i < z; ++i) acc += double(b.samples[size_t(i) * 2]) * b.samples[size_t(i) * 2];
            return std::sqrt(acc / double(z - a));
        };
        // How well the result matches the clean speech: SNR in dB over the whole clip,
        // at the best alignment within +/- 1000 samples (also reports that lag).
        auto snr = [&](const AudioBuffer& b, int* lagOut) {
            double best = -1e9;
            int bestLag = 0;
            const int64_t n = std::min(b.frames(), clean->frames());
            for (int lag = -1000; lag <= 1000; lag += 4) {
                double sig = 0, err2 = 0;
                for (int64_t i = 2000; i < n - 2000; i += 3) {
                    const double c = clean->samples[size_t(i) * 2], o = b.samples[size_t(i + lag) * 2];
                    sig += c * c;
                    err2 += (o - c) * (o - c);
                }
                const double v = 10 * std::log10(sig / std::max(err2, 1e-12));
                if (v > best) best = v, bestLag = lag;
            }
            if (lagOut) *lagOut = bestLag;
            return best;
        };
        int lag = 0;
        const double snrIn = snr(noisy, &lag);
        // The pause between "Americans" and "ask not" (about 2.2 s to 3.2 s) is noise only.
        const double pauseIn = rms(noisy, 2.3, 3.1);

        AudioBuffer denoised;
        reduceNoise(noisy, denoised, 20, 50);
        QCOMPARE(denoised.frames(), noisy.frames());
        const double snrDenoise = snr(denoised, &lag);
        const double pauseDenoise = rms(denoised, 2.3, 3.1);
        qInfo("noise reduction: SNR %.1f -> %.1f dB, pause %.1f dB lower, lag %d", snrIn, snrDenoise,
              20 * std::log10(pauseIn / pauseDenoise), lag);
        QCOMPARE(lag, 0);
        QVERIFY(pauseDenoise < pauseIn * std::pow(10.0, -12 / 20.0));  // noise down by 12 dB or more
        QVERIFY(snrDenoise > snrIn + 4);                                // and the speech is still there

        if (hasVoiceIsolation()) {
            AudioBuffer isolated;
            QVERIFY(isolateVoice(noisy, isolated, 100));
            const double snrVoice = snr(isolated, &lag);
            const double pauseVoice = rms(isolated, 2.3, 3.1);
            qInfo("voice isolation: SNR %.1f -> %.1f dB, pause %.1f dB lower, lag %d", snrIn, snrVoice,
                  20 * std::log10(pauseIn / pauseVoice), lag);
            QVERIFY2(std::abs(lag) <= 8, "RNNoise delay is not compensated");
            QVERIFY(pauseVoice < pauseIn * std::pow(10.0, -15 / 20.0));
            // RNNoise also removes the crowd noise in the 1961 recording itself, so compare
            // speech levels rather than waveforms: "And so my fellow Americans" keeps its level.
            QVERIFY(snrVoice > snrIn);
            const double speechClean = rms(*clean, 0.4, 2.0), speechVoice = rms(isolated, 0.4, 2.0);
            QVERIFY2(std::fabs(20 * std::log10(speechVoice / speechClean)) < 4,
                     qPrintable(QString::number(20 * std::log10(speechVoice / speechClean))));
            // Amount 0 leaves the audio as it was.
            QVERIFY(isolateVoice(noisy, isolated, 0));
            QCOMPARE(isolated.samples.size(), noisy.samples.size());
            QVERIFY(std::fabs(isolated.samples[48000] - noisy.samples[48000]) < 1e-5f);
        }

        // Enhance Speech (DeepFilterNet3), when its model is here.
        if (speechEnhancerAvailable() && speechModel().installed()) {
            AudioBuffer enhanced;
            std::string err;
            QVERIFY2(enhanceSpeech(noisy, enhanced, 100, 100, false, nullptr, &err), err.c_str());
            QCOMPARE(enhanced.frames(), noisy.frames());
            const double snrEnhanced = snr(enhanced, &lag);
            const double pauseEnhanced = rms(enhanced, 2.3, 3.1);
            qInfo("enhance speech: SNR %.1f -> %.1f dB, pause %.1f dB lower, lag %d", snrIn, snrEnhanced,
                  20 * std::log10(pauseIn / pauseEnhanced), lag);
            QVERIFY2(std::abs(lag) <= 4, "the model's delay is not compensated");
            // Far quieter pauses than spectral noise reduction. Like RNNoise it also takes out the
            // crowd in the 1961 recording, so the waveform match is checked only loosely.
            QVERIFY(pauseEnhanced < pauseIn * std::pow(10.0, -30 / 20.0));
            QVERIFY(pauseEnhanced < pauseDenoise * std::pow(10.0, -15 / 20.0));
            QVERIFY(snrEnhanced > snrIn);
            const double speechEnhanced = rms(enhanced, 0.4, 2.0), speechClean = rms(*clean, 0.4, 2.0);
            QVERIFY2(std::fabs(20 * std::log10(speechEnhanced / speechClean)) < 4,
                     qPrintable(QString::number(20 * std::log10(speechEnhanced / speechClean))));
            // A 10 dB cap: the pause comes down by about 10 dB, not more.
            AudioBuffer capped;
            QVERIFY(enhanceSpeech(noisy, capped, 100, 10));
            const double pauseCapped = 20 * std::log10(pauseIn / rms(capped, 2.3, 3.1));
            QVERIFY2(pauseCapped > 8 && pauseCapped < 10.5, qPrintable(QString::number(pauseCapped)));
            // Amount 0 leaves the audio as it was.
            QVERIFY(enhanceSpeech(noisy, capped, 0, 100));
            QVERIFY(std::fabs(capped.samples[48000] - noisy.samples[48000]) < 1e-6f);
            // Everything but the speech: a hum under the voice comes through, the voice does not.
            AudioBuffer hummed = *clean;
            for (int64_t i = 0; i < hummed.frames(); ++i)
                for (int c = 0; c < 2; ++c)
                    hummed.samples[size_t(i) * 2 + size_t(c)] += float(0.05 * std::sin(2 * M_PI * 120 * double(i) / hummed.sampleRate));
            AudioBuffer rest;
            QVERIFY(enhanceSpeech(hummed, rest, 100, 100, true));
            QCOMPARE(rest.frames(), hummed.frames());
            // The hum is all there (its share of the result), and the speech is mostly gone:
            // what is left besides the hum while he speaks, against the speech itself.
            double humIn = 0, hum2 = 0, left = 0, speech = 0;
            const int64_t a = int64_t(0.4 * hummed.sampleRate), z = int64_t(2.0 * hummed.sampleRate);
            for (int64_t i = 2000; i < hummed.frames() - 2000; ++i) {
                const double h = 0.05 * std::sin(2 * M_PI * 120 * double(i) / hummed.sampleRate);
                hum2 += h * h;
                humIn += rest.samples[size_t(i) * 2] * h;
                if (i >= a && i < z) {
                    left += std::pow(rest.samples[size_t(i) * 2] - h, 2);
                    speech += std::pow(clean->samples[size_t(i) * 2], 2);
                }
            }
            const double humGain = 20 * std::log10(humIn / hum2), speechDown = 10 * std::log10(speech / left);
            qInfo("everything but speech: hum %.1f dB, speech %.1f dB lower", humGain, speechDown);
            // (What is left also holds the 1961 crowd, which belongs in the background.)
            QVERIFY(std::fabs(humGain) < 1.5);
            QVERIFY(speechDown > 7);
        } else {
            qInfo("Set MONTAGE_SPEECH_MODEL to a folder with deepfilternet3.onnx to test Enhance Speech");
        }

        // Through the mixer: an exported mix of a clip with Noise Reduction is cleaner.
        const std::string noisyWav = path("noisy-speech.wav");
        {
            std::vector<int16_t> pcm(noisy.samples.size());
            for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(std::lround(std::clamp(noisy.samples[i], -1.0f, 1.0f) * 32767));
            FILE* f = std::fopen(noisyWav.c_str(), "wb");
            QVERIFY(f);
            const uint32_t bytes = uint32_t(pcm.size() * 2);
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f);
            u32(36 + bytes);
            std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16), u16(1), u16(2), u32(48000), u32(48000 * 4), u16(4), u16(16);
            std::fwrite("data", 1, 4, f);
            u32(bytes);
            std::fwrite(pcm.data(), 2, pcm.size(), f);
            std::fclose(f);
        }
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m = probeOrFail(p, noisyWav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Effect fx;
        fx.id = p.newId();
        fx.type = "denoise";
        fx.params["reduction_db"] = 20.0;
        fx.params["sensitivity"] = 50.0;
        s.audioTracks[0].clips[0].effects.push_back(fx);
        ExportSettings st;
        st.path = path("denoised-mix.wav");
        st.videoCodec = "none";
        st.audioCodec = "pcm_s16le";
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        AudioBufferPtr mixed = decodeAudio(st.path, 48000, &err);
        QVERIFY(mixed);
        QVERIFY2(rms(*mixed, 2.3, 3.1) < pauseIn * std::pow(10.0, -10 / 20.0), "the mixer did not apply Noise Reduction");
    }

    void importedVideoComposites() {
        // Use the exported ramp as media inside a new project, at half speed.
        std::string src = path("ramp.mp4");
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        MediaItem m = probeOrFail(p, src);
        p.media.push_back(m);
        auto r = edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        QVERIFY(r.ok);
        QVERIFY(edit::setSpeed(p, s, r.created[0], 0.5, true).ok);
        QCOMPARE(edit::clipById(s, r.created[0])->duration, FrameTime(60));
        RenderOptions o;
        Image img = renderProgramFrame(p, s, 40, o);  // source frame 20
        float red = img.at(80, 45)[0];
        QVERIFY2(std::fabs(red - 20 / 29.0f) < 0.04f, qPrintable(QString::number(red)));
    }

    void sceneDetectionAndProxies() {
        // Three shots: 1 s red, 1 s blue (with a moving title), 1 s bars.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        Clip a = makeGeneratorClip(p, "color", 30);
        a.generator.params["color.r"] = 0.9;
        Clip b = makeGeneratorClip(p, "color", 30);
        b.start = 30;
        b.generator.params["color.b"] = 0.9;
        Clip c = makeGeneratorClip(p, "bars", 30);
        c.start = 60;
        edit::overwrite(p, s, {TrackKind::Video, 0}, a);
        edit::overwrite(p, s, {TrackKind::Video, 0}, b);
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        Clip t = makeGeneratorClip(p, "title", 30);
        t.start = 30;
        t.generator.params["size"] = 24.0;
        t.generator.params["pos_x"].addKey(0, -100);
        t.generator.params["pos_x"].addKey(29, 100);
        edit::overwrite(p, s, {TrackKind::Video, 1}, t);
        ExportSettings st;
        st.path = path("shots.mp4");
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        auto cuts = detectSceneCuts(st.path, 0.5, nullptr, nullptr, &err);
        QCOMPARE(cuts.size(), size_t(2));
        QVERIFY(std::fabs(cuts[0] - 1.0) < 0.05);
        QVERIFY(std::fabs(cuts[1] - 2.0) < 0.05);
        // Proxy: smaller, intra-coded, same length.
        std::string proxy = path("shots_proxy.mp4");
        QVERIFY2(createProxy(st.path, proxy, 160, nullptr, nullptr, &err), err.c_str());
        Project q;
        MediaItem pm = probeOrFail(q, proxy);
        QCOMPARE(pm.width, 160);
        QCOMPARE(pm.height, 90);
        QVERIFY(!pm.hasAudio);
        QVERIFY(std::fabs(pm.duration - 3.0) < 0.1);
        // Rendering with proxies uses the proxy file but keeps the clip geometry.
        MediaItem src = probeOrFail(p, st.path);
        src.proxyPath = proxy;
        p.media.push_back(src);
        Sequence& s2 = p.sequences.emplace_back(makeSequence(p, "Proxy test", 320, 180, {30, 1}));
        edit::placeMedia(p, s2, src.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        RenderOptions ro;
        ro.useProxies = true;
        Image img = renderProgramFrame(p, s2, 45, ro);
        QCOMPARE(img.width, 320);
        QVERIFY(img.at(10, 170)[2] > 0.6f);  // blue shot
        // Matching an empty sequence to a clip adopts its size and rate.
        Sequence empty = makeSequence(p, "Empty", 1920, 1080, {25, 1});
        QVERIFY(edit::matchSequenceToMedia(empty, src));
        QCOMPARE(empty.width, 320);
        QCOMPARE(empty.fps.toDouble(), 30.0);
        QVERIFY(!edit::matchSequenceToMedia(s2, src));  // not empty
    }

    void exportAudioAndIntermediates() {
        std::string wav = path("src.wav");
        writeWav(wav, 48000, 1.0, 0.4f, 0.4f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 256;  // DNxHR's minimum frame size
        s.height = 144;
        MediaItem m = probeOrFail(p, wav);
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "bars", 30));
        std::string err;
        // WAV mixdown keeps the level.
        ExportSettings w = findExportPreset("Audio - WAV 24-bit")->settings;
        w.path = path("mix.wav");
        QVERIFY2(exportSequence(p, s, w, nullptr, nullptr, &err), err.c_str());
        auto back = decodeAudio(w.path, 48000);
        QVERIFY(back && std::abs(back->frames() - 48000) < 100);
        QVERIFY(std::fabs(back->samples[10000] - 0.4f) < 0.002f);
        // ProRes and DNxHR intermediates with PCM audio.
        for (const char* name : {"Apple ProRes 422 HQ", "Avid DNxHR HQ"}) {
            ExportSettings st = findExportPreset(name)->settings;
            st.path = path(std::string(name).find("ProRes") != std::string::npos ? "out_prores.mov" : "out_dnx.mov");
            QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
            Project q;
            MediaItem o = probeOrFail(q, st.path);
            QVERIFY(o.hasVideo && o.hasAudio);
            QVERIFY(o.videoCodec == "prores" || o.videoCodec == "dnxhd");
        }
        // Cancellation stops the export.
        std::atomic<bool> cancel{true};
        ExportSettings st = findExportPreset("H.264 - Fast Draft")->settings;
        st.path = path("cancelled.mp4");
        QVERIFY(!exportSequence(p, s, st, nullptr, &cancel, &err));
        QVERIFY(!QFileInfo::exists(QString::fromStdString(st.path)));  // no partial file left behind
        // Still frame.
        QVERIFY2(exportStill(p, s, 5, path("still.png"), &err), err.c_str());
        QVERIFY(QFileInfo(QString::fromStdString(path("still.png"))).size() > 100);
    }
};

QTEST_MAIN(TestMedia)
#include "test_media.moc"
