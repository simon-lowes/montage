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
#include <functional>
#include <random>
#include <sstream>
#include <cstdio>

#include "core/AutoTag.h"
#include "core/EditOps.h"
#include "core/MediaLog.h"
#include "core/Multicam.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "core/Transcript.h"
#include "core/TranscriptEdit.h"
#include "audio/SpeechCleanup.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/AutoDuck.h"
#include "media/Decoder.h"
#include "media/HwAccel.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#include "media/SpeakerSwitch.h"
#include "media/Tracking.h"
#include "media/Segmenter.h"
#include "media/Diarizer.h"
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
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
        r = tool("montage_add_title", QJsonObject{{"project", project}, {"text", "Hello"}, {"at", "00:00:00:00"}, {"duration", 0.3}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(text(r)));
        QCOMPARE(r.value("structuredContent").toObject().value("text").toString(), QString("Hello"));
        r = tool("montage_add_marker", QJsonObject{{"project", project}, {"at", 0.1}, {"name", "Look"}});
        QVERIFY(!r.value("isError").toBool());
        Project saved;
        QVERIFY(loadProject(project.toStdString(), saved));
        QCOMPARE(saved.active()->markers.size(), size_t(1));
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
