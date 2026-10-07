// Media tests: probing, frame-accurate decoding, audio mixing, export round trips.
#include <QtTest>

#include <cmath>
#include <cstdio>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Decoder.h"
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
        // Still frame.
        QVERIFY2(exportStill(p, s, 5, path("still.png"), &err), err.c_str());
        QVERIFY(QFileInfo(QString::fromStdString(path("still.png"))).size() > 100);
    }
};

QTEST_MAIN(TestMedia)
#include "test_media.moc"
