// Media tests: probing, frame-accurate decoding, audio mixing, export round trips.
#include <QtTest>

#include <cmath>
#include <cstdio>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "core/Transcript.h"
#include "audio/SpeechCleanup.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/Decoder.h"
#include "media/HwAccel.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#ifdef MONTAGE_WITH_WHISPER
#include "media/Transcriber.h"
#endif
#include "render/Compositor.h"
#include "render/Exporter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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
