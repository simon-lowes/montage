// Audio plugin tests: discovery, out-of-process scanning with cache and
// blocklist, CLAP hosting, and plugin effects in the mixer.
#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "audio/PluginEffect.h"
#include "audio/Plugins.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "render/Compositor.h"
#include "render/Ofx.h"
#include "media/ImageSequence.h"
#include "automation/McpServer.h"

using namespace montage;
using namespace montage::plugins;

namespace {

const QString kClapDir = QStringLiteral(MONTAGE_TEST_CLAP_DIR);

std::string clapDir(const char* sub) { return (kClapDir + "/" + sub).toStdString(); }

void isolate(Registry& r, const QString& cache, std::vector<std::string> clapDirs) {
    r.setCachePath(cache.toStdString());
    r.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
    // Never look at plugins installed on the machine running the tests.
    for (Format f : kAllFormats) r.setSearchPaths(f, {"/nonexistent-montage-test-dir"});
    r.setSearchPaths(Format::Clap, std::move(clapDirs));
}

std::optional<Descriptor> gainDescriptor() {
    auto files = findPluginFiles(Format::Clap, {clapDir("good")});
    if (files.size() != 1) return std::nullopt;
    for (const Descriptor& d : probeInProcess(Format::Clap, files[0]))
        if (d.pluginId == "org.montage.test.gain") return d;
    return std::nullopt;
}

void writeWav(const std::string& path, int rate, double seconds, float level) {
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
    int16_t v = int16_t(std::lround(level * 32767));
    for (int i = 0; i < frames * 2; ++i) std::fwrite(&v, 2, 1, f);
    std::fclose(f);
}

}  // namespace

class TestPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString path(const char* name) const { return dir_.filePath(name); }

private slots:
    void environmentVariableAddsSearchPath() {
#ifdef _WIN32
        const char* extra = "C:/montage-test-clap";
#else
        const char* extra = "/montage-test-clap";
#endif
        qputenv("CLAP_PATH", extra);
        auto dirs = defaultSearchPaths(Format::Clap);
        qunsetenv("CLAP_PATH");
        QVERIFY(!dirs.empty());
        QCOMPARE(QString::fromStdString(dirs.front()), QString(extra));
    }

    void findsPluginFiles() {
        auto files = findPluginFiles(Format::Clap, {kClapDir.toStdString()});
        QCOMPARE(files.size(), size_t(4));  // good, hang, crash, latency
        auto good = findPluginFiles(Format::Clap, {clapDir("good")});
        QCOMPARE(good.size(), size_t(1));
        QVERIFY(QString::fromStdString(good[0]).endsWith("MontageTestPlugins.clap"));
    }

    void probeListsEveryPluginInAFile() {
        auto files = findPluginFiles(Format::Clap, {clapDir("good")});
        QCOMPARE(files.size(), size_t(1));
        std::string err;
        auto ds = probeInProcess(Format::Clap, files[0], &err);
        QVERIFY2(err.empty(), err.c_str());
        QCOMPARE(ds.size(), size_t(2));
        QCOMPARE(QString::fromStdString(ds[0].id), QString("clap:org.montage.test.gain"));
        QCOMPARE(QString::fromStdString(ds[0].name), QString("Montage Test Gain"));
        QCOMPARE(QString::fromStdString(ds[0].vendor), QString("Montage"));
        QCOMPARE(QString::fromStdString(ds[0].category), QString("Utility"));
        QCOMPARE(QString::fromStdString(ds[1].category), QString("Filter"));
        QVERIFY(!ds[0].instrument);
        // JSON round trip (the probe's output format).
        QCOMPARE(descriptorsFromJson(descriptorsToJson(ds)), ds);
    }

    void readsMetadataWithoutLoadingCode() {
        // VST3 bundle with moduleinfo.json (JSON5 style: comments, trailing commas).
        const QString vst3 = path("Fake EQ.vst3");
        QVERIFY(QDir().mkpath(vst3 + "/Contents/Resources"));
        {
            QFile f(vst3 + "/Contents/Resources/moduleinfo.json");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({
  "Name": "Fake EQ", // module name
  "Factory Info": { "Vendor": "Fake Audio", },
  "Classes": [
    { "CID": "0123456789ABCDEF0123456789ABCDEF", "Category": "Audio Module Class", "Name": "Fake EQ",
      "Version": "2.1.0", "Sub Categories": ["Fx", "EQ",], },
    { "CID": "FEDCBA9876543210FEDCBA9876543210", "Category": "Component Controller Class", "Name": "Fake EQ Controller" },
  ],
})");
        }
        auto vd = readStaticMetadata(Format::Vst3, vst3.toStdString());
        QVERIFY(vd.has_value());
        QCOMPARE(vd->size(), size_t(1));
        QCOMPARE(QString::fromStdString((*vd)[0].name), QString("Fake EQ"));
        QCOMPARE(QString::fromStdString((*vd)[0].vendor), QString("Fake Audio"));
        QCOMPARE(QString::fromStdString((*vd)[0].category), QString("EQ"));
        QCOMPARE(QString::fromStdString((*vd)[0].id), QString("vst3:0123456789ABCDEF0123456789ABCDEF"));
        QCOMPARE(findPluginFiles(Format::Vst3, {dir_.path().toStdString()}).size(), size_t(1));

        // LV2 bundle: manifest.ttl names the plugin, its .ttl file the details.
        const QString lv2 = path("fake-comp.lv2");
        QVERIFY(QDir().mkpath(lv2));
        {
            QFile m(lv2 + "/manifest.ttl");
            QVERIFY(m.open(QIODevice::WriteOnly));
            m.write("@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                    "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                    "<urn:fake:comp> a lv2:Plugin ;\n    lv2:binary <comp.so> ;\n    rdfs:seeAlso <comp.ttl> .\n");
            QFile t(lv2 + "/comp.ttl");
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("<urn:fake:comp> a lv2:Plugin, lv2:CompressorPlugin ;\n    doap:name \"Fake Compressor\" .\n");
        }
        auto ld = readStaticMetadata(Format::Lv2, lv2.toStdString());
        QVERIFY(ld.has_value());
        QCOMPARE(ld->size(), size_t(1));
        QCOMPARE(QString::fromStdString((*ld)[0].name), QString("Fake Compressor"));
        QCOMPARE(QString::fromStdString((*ld)[0].pluginId), QString("urn:fake:comp"));
        QCOMPARE(QString::fromStdString((*ld)[0].category), QString("Dynamics"));
        QCOMPARE(findPluginFiles(Format::Lv2, {dir_.path().toStdString()}).size(), size_t(1));

#ifdef __APPLE__
        // Audio Units are listed from the system registry; macOS ships Apple's effects.
        auto au = readStaticMetadata(Format::AudioUnit, "AudioUnit");
        QVERIFY(au.has_value() && !au->empty());
        bool apple = false;
        for (const Descriptor& d : *au) apple |= d.vendor == "Apple" && !d.name.empty();
        QVERIFY(apple);
        QCOMPARE(findPluginFiles(Format::AudioUnit, defaultSearchPaths(Format::AudioUnit)).size(), size_t(1));
        QVERIFY(findPluginFiles(Format::AudioUnit, {"/nonexistent"}).empty());
#endif
    }

    void scanProbesNewFilesAndCachesThem() {
        const QString cache = path("cache-good.json");
        {
            Registry r;
            isolate(r, cache, {clapDir("good")});
            ScanReport rep = r.scan();
            QCOMPARE(rep.files, 1);
            QCOMPARE(rep.probed, 1);
            QCOMPARE(rep.fromCache, 0);
            QVERIFY(rep.newlyBlocked.empty());
            QCOMPARE(r.plugins().size(), size_t(2));
            QVERIFY(r.find("clap:org.montage.test.invert").has_value());
        }
        // A fresh registry (next launch) answers unchanged files from the cache.
        Registry again;
        isolate(again, cache, {clapDir("good")});
        QCOMPARE(again.plugins().size(), size_t(2));
        ScanReport rep = again.scan();
        QCOMPARE(rep.fromCache, 1);
        QCOMPARE(rep.probed, 0);
    }

    void scanBlocksPluginsThatHangOrCrash() {
        Registry r;
        isolate(r, path("cache-bad.json"), {clapDir("hang"), clapDir("crash")});
        r.setProbeTimeoutMs(2000);
        ScanReport rep = r.scan();
        QCOMPARE(rep.probed, 2);
        QCOMPARE(rep.newlyBlocked.size(), size_t(2));
        QVERIFY(r.plugins().empty());
        auto blocked = r.blocklist();
        QCOMPARE(blocked.size(), size_t(2));
        QString reasons;
        for (const Blocked& b : blocked) reasons += QString::fromStdString(b.reason) + ";";
        QVERIFY2(reasons.contains("timed out"), qPrintable(reasons));
        QVERIFY2(reasons.contains("crashed") || reasons.contains("exit code"), qPrintable(reasons));

        // Blocked files are not loaded again until they change or are unblocked.
        rep = r.scan();
        QCOMPARE(rep.probed, 0);
        QCOMPARE(rep.fromCache, 2);
        std::string crashPath;
        for (const Blocked& b : blocked)
            if (b.reason.find("timed out") == std::string::npos) crashPath = b.path;
        r.unblock(crashPath);
        QCOMPARE(r.blocklist().size(), size_t(1));
        rep = r.scan();
        QCOMPARE(rep.probed, 1);
        QCOMPARE(r.blocklist().size(), size_t(2));
    }

    void parallelScanRescanAndManagement() {
        // Four copies of the hanging plugin are probed in parallel, so the scan
        // takes about two timeouts instead of four.
        std::vector<std::string> hangDirs;
        const QString hangFile = QString::fromStdString(findPluginFiles(Format::Clap, {clapDir("hang")}).at(0));
        for (int i = 0; i < 4; ++i) {
            const QString d = path("hang-copies") + "/" + QString::number(i);
            QVERIFY(QDir().mkpath(d));
            QVERIFY(QFile::copy(hangFile, d + "/MontageTestHang.clap"));
            hangDirs.push_back(d.toStdString());
        }
        Registry r;
        isolate(r, path("cache-parallel.json"), hangDirs);
        r.setProbeTimeoutMs(1500);
        QElapsedTimer timer;
        timer.start();
        ScanReport rep = r.scan();
        QCOMPARE(rep.probed, 4);
        QCOMPARE(rep.newlyBlocked.size(), size_t(4));
        QVERIFY2(timer.elapsed() < 5200, qPrintable(QString("%1 ms").arg(timer.elapsed())));  // serial: 6 s+
        QCOMPARE(rep.log.size(), size_t(4));
        for (const std::string& line : rep.log) QVERIFY2(line.rfind("blocked: ", 0) == 0, line.c_str());

        // Rescan Selected loads a file again even though it is unchanged.
        Registry g;
        isolate(g, path("cache-manage.json"), {clapDir("good")});
        g.scan();
        const std::string goodFile = findPluginFiles(Format::Clap, {clapDir("good")}).at(0);
        rep = g.rescan({goodFile});
        QCOMPARE(rep.probed, 1);
        QCOMPARE(rep.fromCache, 0);
        QVERIFY(rep.log.at(0).rfind("probed: ", 0) == 0);
        QCOMPARE(g.scan().fromCache, 1);

        // Disabled plugins are remembered, still listed and still found (projects keep working).
        g.setPluginDisabled("clap:org.montage.test.gain", true);
        QVERIFY(g.isPluginDisabled("clap:org.montage.test.gain"));
        QVERIFY(!g.isPluginDisabled("clap:org.montage.test.invert"));
        {
            Registry again;
            isolate(again, path("cache-manage.json"), {clapDir("good")});
            QVERIFY(again.isPluginDisabled("clap:org.montage.test.gain"));
            QVERIFY(again.find("clap:org.montage.test.gain").has_value());
            QCOMPARE(again.plugins().size(), size_t(2));
        }
        g.setPluginDisabled("clap:org.montage.test.gain", false);
        QVERIFY(!g.isPluginDisabled("clap:org.montage.test.gain"));

        // Folders the user adds are scanned after the defaults.
        Registry e;
        e.setCachePath(path("cache-extra.json").toStdString());
        e.setExtraSearchPaths(Format::Clap, {"/extra/clap/folder"});
        auto dirs = e.searchPaths(Format::Clap);
        QVERIFY(dirs.size() > 1);
        QCOMPARE(QString::fromStdString(dirs.back()), QString("/extra/clap/folder"));
    }

    void hostsOpenFxVideoPlugins() {
        const std::string good = MONTAGE_TEST_OFX_DIR "/good", crash = MONTAGE_TEST_OFX_DIR "/crash";
        QCOMPARE(ofx::findBinaries({good, crash}).size(), size_t(2));
        // Scanned in the probe: the good bundle's three filters described, the one that crashes blocklisted.
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({good, crash});
        std::vector<std::string> log;
        QCOMPARE(reg.scan(&log), 3);
        QCOMPARE(reg.blocked().size(), size_t(1));
        // (A crash on Linux and macOS; on Windows the test plugin ends its process without the crash dialog.)
        QVERIFY2(reg.blocked()[0].first.find("MontageTestOfxCrash") != std::string::npos &&
                     (reg.blocked()[0].second.find("Crashed") != std::string::npos || reg.blocked()[0].second.find("Could not be loaded") != std::string::npos),
                 reg.blocked()[0].second.c_str());
        QCOMPARE(log.size(), size_t(2));
        // Unchanged files come from the cache, without the probe.
        log.clear();
        QCOMPARE(reg.scan(&log), 3);
        QVERIFY(std::all_of(log.begin(), log.end(), [](const std::string& l) { return l.rfind("cached", 0) == 0; }));
        QCOMPARE(ofx::instancesCreated(), 0);  // nothing loaded in this process yet
        ofx::PluginDesc invert, temporal;
        QVERIFY(reg.find("org.montage.test.invert/1", invert) && reg.find("org.montage.test.temporal/1", temporal));
        QCOMPARE(invert.label, std::string("Test Invert"));
        QCOMPARE(invert.group, std::string("Montage Test"));
        QVERIFY(!invert.temporal && temporal.temporal && invert.floatImages);
        QCOMPARE(invert.params.size(), size_t(4));
        QCOMPARE(invert.params[0].name, std::string("amount"));
        QCOMPARE(invert.params[0].def.at(0), 1.0);
        QCOMPARE(invert.params[1].choices, (std::vector<std::string>{"Invert", "Pass"}));
        QCOMPARE(invert.params[2].dimensions(), 3);
        QCOMPARE(invert.params[3].stringDefault, std::string("hello"));
        // As a clip effect: a slider, a choice and a colour in the Inspector; the text kept as a string.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{25, 1};
        Effect e = ofx::makeEffect(p, invert);
        QCOMPARE(e.type, std::string("ofx"));
        QCOMPARE(ofx::effectName(e), std::string("Test Invert"));
        QCOMPARE(e.s("str.note"), std::string("hello"));
        const std::vector<montage::ParamInfo> shown = effectParams(e);
        QCOMPARE(shown.size(), size_t(3));
        auto row = [&](const std::string& name) {
            for (const montage::ParamInfo& pi : shown)
                if (pi.name == name) return pi;
            return montage::ParamInfo{};
        };
        QCOMPARE(row("param.amount").label, std::string("Amount"));
        QCOMPARE(int(row("param.mode").kind), int(ParamKind::Choice));
        QCOMPARE(row("param.mode").choices.size(), size_t(2));
        QCOMPARE(int(row("param.tint").kind), int(ParamKind::Color));
        QCOMPARE(row("param.tint").defG, 1.0);
        // On a colour matte: inverted, by an amount that is keyframed, through a tint; Pass leaves it.
        Clip matte = makeGeneratorClip(p, "color", 25);
        matte.generator.params["color.r"] = Param(0.2);
        matte.generator.params["color.g"] = Param(0.4);
        matte.generator.params["color.b"] = Param(0.6);
        matte.effects.push_back(e);
        edit::overwrite(p, s, {TrackKind::Video, 0}, matte);
        RenderOptions o;
        auto at = [&](FrameTime t) {
            const Image img = renderSequenceFrame(p, s, t, o);
            const float* px = img.at(32, 18);
            return std::array<float, 3>{px[0], px[1], px[2]};
        };
        auto near = [](std::array<float, 3> a, std::array<float, 3> b) {
            return std::fabs(a[0] - b[0]) < 0.01 && std::fabs(a[1] - b[1]) < 0.01 && std::fabs(a[2] - b[2]) < 0.01;
        };
        QVERIFY2(near(at(3), {0.8f, 0.6f, 0.4f}), qPrintable(QString("%1 %2 %3").arg(at(3)[0]).arg(at(3)[1]).arg(at(3)[2])));
        QVERIFY(ofx::instancesCreated() >= 1);
        Effect& placed = s.videoTracks[0].clips[0].effects[0];
        placed.params["param.amount"].addKey(0, 0.0);
        placed.params["param.amount"].addKey(10, 1.0);
        QVERIFY2(near(at(5), {0.5f, 0.5f, 0.5f}), qPrintable(QString("%1 %2 %3").arg(at(5)[0]).arg(at(5)[1]).arg(at(5)[2])));
        QVERIFY(near(at(0), {0.2f, 0.4f, 0.6f}));
        placed.params["param.amount"] = Param(1.0);
        placed.params["param.tint.g"] = Param(0.5);
        QVERIFY(near(at(3), {0.8f, 0.3f, 0.4f}));
        placed.params["param.mode"] = Param(1.0);
        QVERIFY(near(at(3), {0.2f, 0.4f, 0.6f}));
        placed.params["param.mode"] = Param(0.0);
        // Saved and loaded: the plugin effect with its settings.
        QVERIFY(saveProject(p, path("ofx.montage").toStdString()));
        Project back;
        QVERIFY(loadProject(path("ofx.montage").toStdString(), back));
        QVERIFY(near([&] {
            const float* px = renderSequenceFrame(back, *back.active(), 3, o).at(32, 18);
            return std::array<float, 3>{px[0], px[1], px[2]};
        }(), {0.8f, 0.3f, 0.4f}));
        // The temporal plugin reads the frames either side through the host.
        Effect avg = ofx::makeEffect(p, temporal);
        Image frame(8, 8);
        frame.fill(0.9f, 0.9f, 0.9f, 1.0f);
        int fetched = 0;
        const ofx::FrameFetch fetch = [&](double t, Image& out) {
            ++fetched;
            out = Image(8, 8);
            out.fill(t < 5 ? 0.3f : 0.6f, 0.3f, 0.3f, 1.0f);
            return true;
        };
        std::string err;
        QVERIFY2(ofx::applyEffect(avg, 5, frame, 1.0, fetch, &err), err.c_str());
        QCOMPARE(fetched, 2);
        QVERIFY2(std::fabs(frame.at(4, 4)[0] - (0.3f + 0.9f + 0.6f) / 3) < 1e-4, qPrintable(QString::number(frame.at(4, 4)[0])));
        QVERIFY(std::fabs(frame.at(4, 4)[1] - 0.5f) < 1e-4);
        // Over MCP: listed with the video effects and added to a clip by its type, parameters by their own names.
        {
            Project mp = makeDefaultProject();
            Sequence& ms = *mp.active();
            ms.width = 64, ms.height = 36;
            edit::overwrite(mp, ms, {TrackKind::Video, 0}, makeGeneratorClip(mp, "color", 25));
            const Id clip = ms.videoTracks[0].clips[0].id;
            const QString project = path("ofx-mcp.montage");
            QVERIFY(saveProject(mp, project.toStdString()));
            McpServer server;
            auto call = [&](const QString& tool, const QJsonObject& args) {
                const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
                const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
                return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
            };
            QJsonObject r = call("montage_list_effects", {{"kind", "video"}});
            bool listed = false;
            for (const QJsonValue& v : r.value("structuredContent").toObject().value("effects").toArray())
                listed |= v.toObject().value("type").toString() == "ofx:org.montage.test.invert/1";
            QVERIFY(listed);
            r = call("montage_add_effect", {{"project", project}, {"clip", double(clip)}, {"effect", "ofx:org.montage.test.invert/1"},
                                            {"params", QJsonObject{{"amount", 0.25}, {"tint.g", 0.5}}}, {"strings", QJsonObject{{"note", "set over MCP"}}}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            Project added;
            QVERIFY(loadProject(project.toStdString(), added));
            const Effect& fx = added.active()->videoTracks[0].clips[0].effects.at(0);
            QVERIFY(fx.type == "ofx" && fx.s("ofx_id") == "org.montage.test.invert/1" && fx.s("str.note") == "set over MCP");
            QCOMPARE(fx.params.at("param.amount").value, 0.25);
            QCOMPARE(fx.params.at("param.tint.g").value, 0.5);
            r = call("montage_add_effect", {{"project", project}, {"clip", double(clip)}, {"effect", "ofx:org.montage.test.invert/1"},
                                            {"params", QJsonObject{{"nonsense", 1}}}});
            QVERIFY(r.value("isError").toBool());
        }
        // In a sequence, the temporal plugin gets the clip's own frames either side: three stills played as a sequence.
        {
            const QString frames = path("ofx-frames");
            QDir().mkpath(frames);
            const int reds[] = {25, 128, 153};
            for (int n = 0; n < 3; ++n) {
                QImage q(16, 16, QImage::Format_RGB32);
                q.fill(qRgb(reds[n], 64, 64));
                QVERIFY(q.save(frames + QStringLiteral("/f%1.png").arg(n + 1)));
            }
            ImageSequence run;
            QVERIFY(detectImageSequence((frames + "/f1.png").toStdString(), run));
            run.fps = Rational{25, 1};
            Project tp = makeDefaultProject();
            Sequence& ts = *tp.active();
            ts.width = 16, ts.height = 16, ts.fps = Rational{25, 1};
            MediaItem m;
            m.id = tp.newId();
            QVERIFY(probeMedia(imageSequencePath(run), m));
            tp.media.push_back(m);
            QVERIFY(edit::placeMedia(tp, ts, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
            ts.videoTracks[0].clips[0].effects.push_back(ofx::makeEffect(tp, temporal));
            const float middle = renderSequenceFrame(tp, ts, 1, o).at(8, 8)[0];
            QVERIFY2(std::fabs(middle - (25 + 128 + 153) / 3.0f / 255.0f) < 0.01f, qPrintable(QString::number(middle)));
            // Log footage: the frames either side are brought into the working space like the frame itself.
            tp.media[0].colorOverride = "slog3-sgamut3cine";
            const Effect kept = ts.videoTracks[0].clips[0].effects[0];
            ts.videoTracks[0].clips[0].effects.clear();
            float plain = 0;
            for (FrameTime f = 0; f < 3; ++f) plain += renderSequenceFrame(tp, ts, f, o).at(8, 8)[0] / 3;
            ts.videoTracks[0].clips[0].effects.push_back(kept);
            const float logMiddle = renderSequenceFrame(tp, ts, 1, o).at(8, 8)[0];
            QVERIFY2(std::fabs(logMiddle - plain) < 0.01f, qPrintable(QString("%1 %2").arg(logMiddle).arg(plain)));
        }
        // Each clip effect has its own instance, told when its parameters change; a position the plugin gives as a
        // fraction of the frame is kept that way and reaches the plugin in pixels.
        {
            ofx::PluginDesc probe;
            QVERIFY(reg.find("org.montage.test.probe/1", probe));
            QVERIFY(probe.params.at(0).normalised && !probe.params.at(1).normalised);
            Effect a = ofx::makeEffect(p, probe), b = ofx::makeEffect(p, probe);
            QVERIFY(a.id != b.id);
            QCOMPARE(a.params.at("param.centre.x").value, 0.5);
            Image half(200, 100);  // a 400 x 200 frame at half size
            QVERIFY2(ofx::applyEffect(a, 0, half, 0.5, {}, &err), err.c_str());
            QVERIFY2(std::fabs(half.at(5, 5)[0] - 0.2f) < 1e-4 && std::fabs(half.at(5, 5)[1] - 0.05f) < 1e-4,
                     qPrintable(QString("%1 %2").arg(half.at(5, 5)[0]).arg(half.at(5, 5)[1])));
            a.params["param.centre.x"] = Param(0.25);
            QVERIFY(ofx::applyEffect(a, 0, half, 0.5, {}, &err) && std::fabs(half.at(5, 5)[0] - 0.1f) < 1e-4);
            auto changes = [&](const Effect& fx) {
                Image i(8, 8);
                if (!ofx::applyEffect(fx, 0, i, 1.0, {}, &err)) return -1.0f;
                return i.at(1, 1)[0] * 10;
            };
            a.params["param.mode"] = Param(1.0);
            b.params["param.mode"] = Param(1.0);
            QCOMPARE(std::lround(changes(a)), 0L);  // the mode changed, not the amount
            a.params["param.amount"] = Param(0.9);
            QCOMPARE(std::lround(changes(a)), 1L);
            QCOMPARE(std::lround(changes(a)), 1L);  // nothing new
            a.params["param.amount"].addKey(0, 0.1);
            QCOMPARE(std::lround(changes(a)), 2L);
            QCOMPARE(std::lround(changes(b)), 0L);  // its own instance: nothing of a's
            // Free instances are destroyed past a limit, least recently used first.
            const int made = ofx::instancesCreated();
            for (int k = 0; k < 20; ++k) {
                Effect fx = ofx::makeEffect(p, probe);
                QVERIFY(changes(fx) >= 0);
            }
            QVERIFY(ofx::instancesCreated() >= made + 20);
            QVERIFY2(ofx::instancesAlive() <= 16, qPrintable(QString::number(ofx::instancesAlive())));
        }
        // A missing plugin leaves the picture as it was and says why.
        Effect gone = e;
        gone.strings["ofx_id"] = "org.example.missing/1";
        gone.strings["ofx_description"] = "[]";
        Image keep(4, 4);
        keep.fill(0.1f, 0.2f, 0.3f, 1.0f);
        QVERIFY(!ofx::applyEffect(gone, 0, keep, 1.0, {}, &err));
        QVERIFY(err.find("not installed") != std::string::npos && std::fabs(keep.at(1, 1)[0] - 0.1f) < 1e-6);
        // A binary named in the project file is never loaded: only what the scan found runs, and a blocked plugin
        // says so rather than crashing the editor.
        const int before = ofx::instancesCreated();
        ofx::PluginDesc elsewhere = invert;
        elsewhere.id = "org.example.elsewhere/1";
        gone.strings["ofx_id"] = elsewhere.id;
        gone.strings["ofx_description"] = ofx::descriptionsToJson({elsewhere});
        QVERIFY(!ofx::applyEffect(gone, 0, keep, 1.0, {}, &err));
        QVERIFY2(err.find("not installed") != std::string::npos, err.c_str());
        elsewhere.binary = reg.blocked()[0].first;
        gone.strings["ofx_description"] = ofx::descriptionsToJson({elsewhere});
        QVERIFY(!ofx::applyEffect(gone, 0, keep, 1.0, {}, &err));
        QVERIFY2(err.find("blocked") != std::string::npos, err.c_str());
        QCOMPARE(ofx::instancesCreated(), before);
        QVERIFY(std::fabs(keep.at(1, 1)[0] - 0.1f) < 1e-6);
    }

#ifdef __APPLE__
    void hostsAppleAudioUnits() {
        // macOS ships Apple's effects; AULowpass is "aufx:lpas:appl".
        auto au = readStaticMetadata(Format::AudioUnit, "AudioUnit");
        QVERIFY(au.has_value());
        std::optional<Descriptor> lowpass;
        for (const Descriptor& d : *au)
            if (d.pluginId == "aufx:lpas:appl") lowpass = d;
        QVERIFY(lowpass.has_value());
        QVERIFY(canHost(Format::AudioUnit));
        std::string err;
        auto inst = instantiate(*lowpass, &err);
        QVERIFY2(inst, err.c_str());
        QVERIFY(inst->activate(48000, 512));
        auto params = inst->parameters();
        QVERIFY(!params.empty());
        qInfo("AULowpass parameter 0: %s (%g..%g)", params[0].name.c_str(), params[0].min, params[0].max);
        inst->setParameter(0, 300);  // cutoff, Hz
        QVERIFY(std::fabs(inst->parameter(0) - 300) < 1);
        auto rmsAfter = [&](double hz) {
            inst->reset();
            const int n = 9600;
            std::vector<float> l(n), r(n);
            for (int i = 0; i < n; ++i) l[i] = r[i] = 0.5f * float(std::sin(2 * M_PI * hz * i / 48000.0));
            float* ch[2] = {l.data(), r.data()};
            inst->process(ch, 2, n);  // more than one 512-frame block
            double acc = 0;
            for (int i = n / 2; i < n; ++i) acc += double(l[i]) * l[i];
            return std::sqrt(acc / (n / 2));
        };
        const double low = rmsAfter(100), high = rmsAfter(6000);
        qInfo("AULowpass at 300 Hz: 100 Hz -> %.3f, 6 kHz -> %.4f", low, high);
        QVERIFY(low > 0.3);
        QVERIFY(high < 0.02);
        QVERIFY(inst->latencySamples() >= 0);
        // Settings round trip into a second instance.
        const std::string state = inst->saveState();
        QVERIFY(!state.empty());
        auto other = instantiate(*lowpass);
        QVERIFY(other && other->activate(48000, 512));
        QVERIFY(other->loadState(state));
        QVERIFY(std::fabs(other->parameter(0) - 300) < 1);
    }
#endif

    void hostsAClapPlugin() {
        auto d = gainDescriptor();
        QVERIFY(d.has_value());
        std::string err;
        auto inst = instantiate(*d, &err);
        QVERIFY2(inst, err.c_str());
        QVERIFY(inst->activate(48000, 256));
        auto params = inst->parameters();
        QCOMPARE(params.size(), size_t(1));
        QCOMPARE(params[0].id, 7u);
        QCOMPARE(QString::fromStdString(params[0].name), QString("Gain"));
        QCOMPARE(params[0].max, 2.0);
        QCOMPARE(params[0].def, 1.0);

        inst->setParameter(7, 0.5);
        std::vector<float> l(1000, 0.8f), r(1000, -0.4f);  // more than one 256-frame block
        float* ch[2] = {l.data(), r.data()};
        inst->process(ch, 2, 1000);
        QCOMPARE(l[0], 0.4f);
        QCOMPARE(l[999], 0.4f);
        QCOMPARE(r[500], -0.2f);
        QCOMPARE(inst->parameter(7), 0.5);

        // State round trip into a second instance.
        const std::string state = inst->saveState();
        QCOMPARE(state.size(), sizeof(double));
        auto other = instantiate(*d);
        QVERIFY(other && other->activate(48000, 256));
        QVERIFY(other->loadState(state));
        QCOMPARE(other->parameter(7), 0.5);
    }

    void clapEditorProtocol() {
        // The test gain's "editor" asks for a size and turns its knob when shown.
        struct Listener : EditorListener {
            std::vector<std::pair<uint32_t, double>> params;
            std::vector<std::pair<uint32_t, bool>> gestures;
            int w = 0, h = 0;
            void editorParameter(uint32_t id, double v) override { params.emplace_back(id, v); }
            void editorGesture(uint32_t id, bool begin) override { gestures.emplace_back(id, begin); }
            void editorResize(int width, int height) override { w = width, h = height; }
        } listener;
        auto d = gainDescriptor();
        QVERIFY(d.has_value());
        auto inst = instantiate(*d);
        QVERIFY(inst);
        QVERIFY(inst->hasEditor());
        int w = 0, h = 0;
        QVERIFY(!inst->openEditor(nullptr, &listener, w, h));  // a parent window is required
        int dummyWindow = 0;
        QVERIFY(inst->openEditor(&dummyWindow, &listener, w, h));
        QCOMPARE(w, 320);
        QCOMPARE(h, 200);
        QCOMPARE(listener.w, 400);  // asked for a resize when shown
        QVERIFY(inst->editorResizable());
        inst->idle();               // the host flushes: the knob change arrives
        QCOMPARE(listener.params.size(), size_t(1));
        QCOMPARE(listener.params[0], std::make_pair(7u, 0.25));
        QCOMPARE(listener.gestures.size(), size_t(2));
        QVERIFY(listener.gestures[0].second && !listener.gestures[1].second);
        QCOMPARE(inst->parameter(7), 0.25);
        inst->closeEditor();
        inst->idle();  // nothing more once closed
        QCOMPARE(listener.params.size(), size_t(1));
    }

#ifdef MONTAGE_WITH_VST3
    void vst3EditorProtocol() {
        struct Listener : EditorListener {
            std::vector<std::pair<uint32_t, double>> params;
            std::vector<std::pair<uint32_t, bool>> gestures;
            int w = 0, h = 0;
            void editorParameter(uint32_t id, double v) override { params.emplace_back(id, v); }
            void editorGesture(uint32_t id, bool begin) override { gestures.emplace_back(id, begin); }
            void editorResize(int width, int height) override { w = width, h = height; }
        } listener;
        auto files = findPluginFiles(Format::Vst3, {MONTAGE_TEST_VST3_DIR});
        QCOMPARE(files.size(), size_t(1));
        auto ds = probeInProcess(Format::Vst3, files[0]);
        QCOMPARE(ds.size(), size_t(1));
        auto inst = instantiate(ds[0]);
        QVERIFY(inst);
        QVERIFY(inst->hasEditor());
        int w = 0, h = 0, dummyWindow = 0;
        QVERIFY(!inst->openEditor(nullptr, &listener, w, h));
        QVERIFY(inst->openEditor(&dummyWindow, &listener, w, h));
        // The view asked the frame for 360x240 while attaching, which also resized it.
        QCOMPARE(listener.w, 360);
        QCOMPARE(listener.h, 240);
        QCOMPARE(w, 360);
        QCOMPARE(h, 240);
        QVERIFY(!inst->editorResizable());
        QCOMPARE(listener.params.size(), size_t(1));
        QCOMPARE(listener.params[0], std::make_pair(3u, 0.125));  // normalised, as parameters() reports
        QCOMPARE(listener.gestures.size(), size_t(2));
        QVERIFY(listener.gestures[0].second && !listener.gestures[1].second);
        inst->closeEditor();
        QVERIFY(inst->hasEditor());
    }

    void probesAndHostsAVst3Plugin() {
        const std::string dir = MONTAGE_TEST_VST3_DIR;
        auto files = findPluginFiles(Format::Vst3, {dir});
        QCOMPARE(files.size(), size_t(1));
        std::string err;
        auto ds = probeInProcess(Format::Vst3, files[0], &err);
        QVERIFY2(err.empty(), err.c_str());
        QCOMPARE(ds.size(), size_t(1));  // the controller class is not listed
        const Descriptor& d = ds[0];
        QCOMPARE(QString::fromStdString(d.name), QString("Montage Test VST3 Gain"));
        QCOMPARE(QString::fromStdString(d.vendor), QString("Montage"));
        QCOMPARE(QString::fromStdString(d.category), QString("Utility"));
        QCOMPARE(d.pluginId.size(), size_t(32));
        QVERIFY(canHost(Format::Vst3));

        // Scanned through the probe process like CLAP files without metadata.
        Registry r;
        isolate(r, path("cache-vst3.json"), {});
        r.setSearchPaths(Format::Vst3, {dir});
        ScanReport rep = r.scan();
        QCOMPARE(rep.probed, 1);
        QVERIFY(r.find(d.id).has_value());

        auto inst = instantiate(d, &err);
        QVERIFY2(inst, err.c_str());
        QVERIFY(inst->activate(48000, 256));
        auto params = inst->parameters();
        QCOMPARE(params.size(), size_t(1));
        QCOMPARE(params[0].id, 3u);
        QCOMPARE(QString::fromStdString(params[0].name), QString("Gain"));
        QCOMPARE(params[0].def, 0.5);
        inst->setParameter(3, 0.25);  // gain 0.5
        std::vector<float> l(700, 0.8f), rr(700, -0.4f);
        float* ch[2] = {l.data(), rr.data()};
        inst->process(ch, 2, 700);
        QCOMPARE(l[0], 0.4f);
        QCOMPARE(l[699], 0.4f);
        QCOMPARE(rr[300], -0.2f);
        QCOMPARE(inst->parameter(3), 0.25);

        // Component state reaches a new instance's processor and controller.
        const std::string state = inst->saveState();
        auto other = instantiate(d, &err);
        QVERIFY2(other, err.c_str());
        QVERIFY(other->activate(48000, 256));
        QVERIFY(other->loadState(state));
        QCOMPARE(other->parameter(3), 0.25);
        std::vector<float> l2(64, 1.0f), r2(64, 1.0f);
        float* ch2[2] = {l2.data(), r2.data()};
        other->process(ch2, 2, 64);
        QCOMPARE(l2[10], 0.5f);
        inst->reset();
        inst->process(ch2, 2, 64);  // still runs after a reset
    }
#endif

#ifdef MONTAGE_TEST_LV2_BUNDLE
    void hostsLv2Plugins() {
        QVERIFY(canHost(Format::Lv2));
        // Found by a scan of its folder (from the manifest, no code loaded).
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-lv2.json"), {});
        reg.setSearchPaths(Format::Lv2, {QFileInfo(MONTAGE_TEST_LV2_BUNDLE).absolutePath().toStdString()});
        reg.scan();
        auto d = reg.find("lv2:urn:montage:test:lv2-gain");
        QVERIFY(d.has_value());
        QCOMPARE(QString::fromStdString(d->name), QString("Montage Test Gain (LV2)"));

        std::string err;
        auto inst = instantiate(*d, &err);
        QVERIFY2(inst, err.c_str());
        QVERIFY(inst->activate(48000, 256));
        // Control inputs are the parameters; the latency output is not one.
        const auto params = inst->parameters();
        QCOMPARE(params.size(), size_t(1));
        QCOMPARE(QString::fromStdString(params[0].name), QString("Gain"));
        QCOMPARE(params[0].min, -60.0);
        QCOMPARE(params[0].max, 24.0);
        QCOMPARE(params[0].def, 0.0);
        QCOMPARE(inst->latencySamples(), 16);
        // -6.0206 dB halves the level; the plugin delays by its reported 16 samples.
        inst->setParameter(params[0].id, -6.0206);
        std::vector<float> l(600), r(600);
        for (int i = 0; i < 600; ++i) l[size_t(i)] = r[size_t(i)] = i >= 100 ? 0.8f : 0.0f;
        float* ch[2] = {l.data(), r.data()};
        inst->process(ch, 2, 600);  // more than one block of 256
        QCOMPARE(l[115], 0.0f);
        QVERIFY(std::fabs(l[116] - 0.4f) < 1e-4f && std::fabs(r[599] - 0.4f) < 1e-4f);
        // Settings survive as LV2 state.
        inst->setParameter(params[0].id, -12.0);
        const std::string state = inst->saveState();
        QVERIFY2(state.find("gain") != std::string::npos, state.c_str());
        auto other = instantiate(*d, &err);
        QVERIFY(other && other->activate(44100, 128));
        QCOMPARE(other->parameter(params[0].id), 0.0);
        QVERIFY(other->loadState(state));
        QVERIFY(std::fabs(other->parameter(params[0].id) + 12.0) < 1e-6);
        // Reset clears the delay line.
        inst->reset();
        std::vector<float> z(64, 0.0f), z2(64, 0.0f);
        float* zc[2] = {z.data(), z2.data()};
        inst->process(zc, 2, 64);
        QCOMPARE(z[0], 0.0f);

        // As a clip effect in the mixer, with its latency compensated.
        const std::string wav = path("lv2-step.wav").toStdString();
        writeWav(wav, 48000, 2.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        m.id = p.newId();
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        const FrameTime at = FrameTime(std::llround(s.fpsValue()));
        QVERIFY(edit::placeMedia(p, s, m.id, at, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto e = makePluginEffect(p, *d, &err);
        QVERIFY2(e, err.c_str());
        s.audioTracks[0].clips[0].effects.push_back(*e);
        AudioMixer mixer;
        std::vector<float> out(400 * 2);
        mixer.mix(p, s, 47900, 400, out.data());
        int64_t step = -1;
        for (int i = 0; i < 400 && step < 0; ++i)
            if (out[size_t(i) * 2] > 0.25f) step = 47900 + i;
        QCOMPARE(step, int64_t(48000));
    }
#endif

    void pluginDelayCompensation() {
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-latency.json"), {clapDir("latency")});
        reg.scan();
        auto d = reg.find("clap:org.montage.test.delay64");
        QVERIFY(d.has_value());
        {
            auto inst = instantiate(*d);
            QVERIFY(inst && inst->activate(48000, 512));
            QCOMPARE(inst->latencySamples(), 64);
        }

        // A clip of constant 0.5 starting at 1 s (sample 48000).
        const std::string wav = path("step.wav").toStdString();
        writeWav(wav, 48000, 2.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        const FrameTime at = FrameTime(std::llround(s.fpsValue()));
        QVERIFY(edit::placeMedia(p, s, m.id, at, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        Clip* clip = &s.audioTracks[0].clips[0];
        auto delay = [&] {
            auto e = makePluginEffect(p, *d, &err);
            return *e;
        };
        // Where the step appears in a fresh mix from 47900, and the level after a seek into the clip.
        auto stepAt = [&](int64_t from) {
            AudioMixer mixer;
            std::vector<float> out(400 * 2);
            mixer.mix(p, s, from, 400, out.data());
            for (int i = 0; i < 400; ++i)
                if (out[size_t(i) * 2] > 0.25f) return from + i;
            return int64_t(-1);
        };
        auto levelAfterSeek = [&] {
            AudioMixer mixer;
            std::vector<float> out(64 * 2);
            mixer.mix(p, s, 60000, 64, out.data());
            return out[0];
        };
        QCOMPARE(stepAt(47900), int64_t(48000));  // reference: no plugins

        // On the clip, a track, a bus and the master, alone and all at once: the
        // step stays at the clip start, and audio after a seek is right at once.
        clip->effects.push_back(delay());
        QCOMPARE(stepAt(47900), int64_t(48000));
        QCOMPARE(levelAfterSeek(), 0.5f);
        clip->effects.clear();
        s.audioTracks[0].effects.push_back(delay());
        QCOMPARE(stepAt(47900), int64_t(48000));
        QCOMPARE(levelAfterSeek(), 0.5f);
        Bus b;
        b.id = p.newId();
        b.effects.push_back(delay());
        s.buses.push_back(b);
        s.audioTracks[0].output = b.id;
        QCOMPARE(stepAt(47900), int64_t(48000));
        s.masterEffects.push_back(delay());
        clip = &s.audioTracks[0].clips[0];
        clip->effects.push_back(delay());
        clip->effects.push_back(delay());  // 5 plugins, 320 samples in all
        QCOMPARE(stepAt(47900), int64_t(48000));
        QCOMPARE(levelAfterSeek(), 0.5f);

        // A track without plugins stays in line with one that has them.
        Track a2 = s.audioTracks[0];
        a2.id = p.newId();
        a2.effects.clear();
        a2.output = 0;
        for (Clip& c : a2.clips) {
            c.id = p.newId();
            c.effects.clear();
            c.linkGroup = 0;
        }
        s.audioTracks.push_back(a2);
        {
            AudioMixer mixer;
            std::vector<float> out(400 * 2);
            mixer.mix(p, s, 47900, 400, out.data());
            QVERIFY(std::fabs(out[99 * 2]) < 1e-6f);               // silence before the clip on both
            QVERIFY(std::fabs(out[100 * 2] - 1.0f) < 0.01f);       // both tracks together, exactly at the start
        }
        // Contiguous blocks after the first need no priming and stay aligned.
        {
            AudioMixer mixer;
            std::vector<float> a(100 * 2), bb(300 * 2);
            mixer.mix(p, s, 47800, 100, a.data());
            mixer.mix(p, s, 47900, 300, bb.data());
            QVERIFY(std::fabs(bb[99 * 2]) < 1e-6f && std::fabs(bb[100 * 2] - 1.0f) < 0.01f);
        }
    }

    void pluginEffectRunsInTheMixer() {
        // The mixer finds plugins through the shared registry.
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-mixer.json"), {clapDir("good")});
        reg.scan();
        auto d = reg.find("clap:org.montage.test.gain");
        QVERIFY(d.has_value());

        const std::string wav = path("tone.wav").toStdString();
        writeWav(wav, 48000, 2.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        auto r = edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        QVERIFY(r.ok);
        Clip* c = edit::clipById(s, r.created[0]);

        auto e = makePluginEffect(p, *d, &err);
        QVERIFY2(e.has_value(), err.c_str());
        QCOMPARE(QString::fromStdString(e->s("plugin_name")), QString("Montage Test Gain"));
        QVERIFY(e->params.count("param.7"));
        auto shown = effectParams(*e);
        QCOMPARE(shown.size(), size_t(1));
        QCOMPARE(QString::fromStdString(shown[0].label), QString("Gain"));
        QCOMPARE(shown[0].max, 2.0);

        e->params["param.7"] = Param(0.25);
        c->effects.push_back(*e);
        AudioMixer mixer;
        std::vector<float> out(4800 * 2);
        mixer.mix(p, s, 24000, 4800, out.data());
        QVERIFY2(std::fabs(out[200] - 0.125f) < 0.002f, qPrintable(QString::number(out[200])));

        // Keyframed parameter changes reach the plugin; reset() keeps it loaded.
        c->effects.back().params["param.7"] = Param(2.0);
        mixer.reset();
        mixer.mix(p, s, 24000, 4800, out.data());
        QVERIFY2(std::fabs(out[200] - 1.0f) < 0.004f, qPrintable(QString::number(out[200])));

        // The effect (identity, parameters, state) survives a project save / load.
        const std::string file = path("plugin.montage").toStdString();
        QVERIFY(saveProject(p, file, &err));
        Project loaded;
        QVERIFY(loadProject(file, loaded, &err));
        const Effect& le = edit::clipById(*loaded.active(), c->id)->effects.back();
        QCOMPARE(le.type, std::string("plugin"));
        QCOMPARE(le.s("plugin_id"), std::string("clap:org.montage.test.gain"));
        QCOMPARE(le.p("param.7", 0), 2.0);
        QCOMPARE(effectParams(le).size(), size_t(1));

        // A plugin that is not installed leaves the audio untouched.
        c->effects.back().strings["plugin_id"] = "clap:not.installed";
        AudioMixer fresh;
        fresh.mix(p, s, 24000, 4800, out.data());
        QVERIFY(std::fabs(out[200] - 0.5f) < 0.002f);
    }
};

QTEST_GUILESS_MAIN(TestPlugins)
#include "test_plugins.moc"
