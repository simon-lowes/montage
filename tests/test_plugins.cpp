// Audio plugin tests: discovery, out-of-process scanning with cache and
// blocklist, CLAP hosting, and plugin effects in the mixer.
#include <QtTest>

#include <cmath>
#include <cstdio>

#include "audio/PluginEffect.h"
#include "audio/Plugins.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "render/Compositor.h"

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
        QCOMPARE(files.size(), size_t(3));  // good, hang, crash
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

#ifdef MONTAGE_WITH_VST3
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
