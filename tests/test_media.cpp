// Media tests: probing, frame-accurate decoding, audio mixing, export round trips.
#include <QtTest>

#include <cmath>
#include <cstdio>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/Decoder.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#include "render/Compositor.h"
#include "render/Exporter.h"

using namespace montage;

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
