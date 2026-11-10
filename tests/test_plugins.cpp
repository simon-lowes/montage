// Audio plugin tests: discovery, out-of-process scanning with cache and
// blocklist, CLAP hosting, plugin effects in the mixer, and plug-ins in host
// processes of their own (bit for bit the same, bypassed when they fail).
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>
#include <QStandardPaths>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <mutex>

#include "audio/AraHost.h"
#include "audio/AudioDevice.h"
#include "audio/PluginEffect.h"
#include "audio/PluginHost.h"
#include "audio/PluginIpc.h"
#include "audio/Plugins.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "media/MediaPool.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Ofx.h"
#include "render/OfxRemote.h"
#include "render/Exporter.h"
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

// A 16-bit stereo WAV of `frames` frames, each sample as `sample(frame, channel)` gives it.
void writeWavOf(const std::string& path, int rate, int frames, const std::function<float(int, int)>& sample) {
    FILE* f = std::fopen(path.c_str(), "wb");
    QVERIFY(f);
    const uint32_t dataBytes = uint32_t(frames) * 4;
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
    for (int i = 0; i < frames; ++i)
        for (int ch = 0; ch < 2; ++ch) {
            const int16_t v = int16_t(std::lround(std::clamp(sample(i, ch), -1.0f, 1.0f) * 32767));
            std::fwrite(&v, 2, 1, f);
        }
    std::fclose(f);
}

// ---- Plug-in hosts (audio/PluginHost.h) ----

// The plug-in hosting settings for one test, put back (with the failure list cleared) when it ends.
struct Hosting {
    HostingSettings before = hosting();
    explicit Hosting(Isolation i, const std::function<void(HostingSettings&)>& tweak = {}) {
        HostingSettings s = before;
        s.isolation = i;
        if (tweak) tweak(s);
        setHosting(s);
        clearHostFailures();
    }
    ~Hosting();
    Hosting(const Hosting&) = delete;
    Hosting& operator=(const Hosting&) = delete;
};

// A fault (tests/plugins/test_fault.h) for the plug-in hosts that start while it is set. It reaches them through
// their environment; nothing in the test's own process runs a plug-in meanwhile.
struct Fault {
    explicit Fault(const char* spec) { qputenv("MONTAGE_TEST_PLUGIN_FAULT", spec); }
    ~Fault() { qunsetenv("MONTAGE_TEST_PLUGIN_FAULT"); }
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;
};

// Waits for `done`, for as long as a busy machine could need (the condition is what is tested, not the time).
bool eventually(const std::function<bool()>& done, int ms = 60000) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms) return false;
        QThread::msleep(5);
    }
    return true;
}

// (the hosts the test started end with it, so the next test's start afresh)
Hosting::~Hosting() {
    ofx::remote::releaseIdle();
    eventually([] { return runningHosts().empty(); }, 20000);
    setHosting(before);
    clearHostFailures();
}

std::optional<Descriptor> clapPlugin(const char* sub, const std::string& id) {
    for (const std::string& file : findPluginFiles(Format::Clap, {clapDir(sub)}))
        for (const Descriptor& d : probeInProcess(Format::Clap, file))
            if (d.pluginId == id) return d;
    return std::nullopt;
}

// The test plug-ins by short name, as the tests use them: "gain", "invert", "delay", "convolver", "vst3", "lv2".
std::optional<Descriptor> testPlugin(const QString& name) {
    if (name == "gain") return clapPlugin("good", "org.montage.test.gain");
    if (name == "invert") return clapPlugin("good", "org.montage.test.invert");
    if (name == "delay") return clapPlugin("latency", "org.montage.test.delay64");
    if (name == "convolver") return clapPlugin("convolver", "org.montage.test.convolver");
#ifdef MONTAGE_WITH_VST3
    if (name == "vst3") {
        const auto files = findPluginFiles(Format::Vst3, {MONTAGE_TEST_VST3_DIR});
        if (files.size() != 1) return std::nullopt;
        const auto ds = probeInProcess(Format::Vst3, files[0]);
        if (ds.size() == 1) return ds[0];
    }
#endif
#ifdef MONTAGE_TEST_LV2_BUNDLE
    if (name == "lv2") {
        Registry& reg = Registry::instance();
        reg.setSearchPaths(Format::Lv2, {QFileInfo(MONTAGE_TEST_LV2_BUNDLE).absolutePath().toStdString()});
        reg.scan();
        auto d = reg.find("lv2:urn:montage:test:lv2-gain");
        reg.setSearchPaths(Format::Lv2, {"/nonexistent-montage-test-dir"});
        return d;
    }
#endif
    return std::nullopt;
}

// A setting of the gain-like test plug-ins and the gain it gives: CLAP 0..2, VST3 normalised (gain twice it), LV2 dB.
struct GainSetting {
    uint32_t id;
    double value, gain;
};
GainSetting halfGain(const QString& name, Instance& inst) {
    const auto params = inst.parameters();
    const uint32_t id = params.empty() ? 0 : params[0].id;
    if (name == "vst3") return {id, 0.25, 0.5};
    if (name == "lv2") return {id, -6.0206, 0.5};
    return {id, 0.5, 0.5};
}

std::optional<HostFailure> failureOf(const std::string& id) {
    for (const HostFailure& f : hostFailures())
        if (f.pluginId == id) return f;
    return std::nullopt;
}

// Noise the same every run.
std::vector<float> testNoise(size_t n, uint32_t seed) {
    std::vector<float> v(n);
    for (float& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = float(int32_t(seed >> 8) % 20000) / 20000.0f * 0.8f;
    }
    return v;
}

// A device test's playback: a click at timeline sample `clickAt` on its first channel, its blocks numbered from 0.
struct Clicker {
    std::atomic<int64_t> pos{0};
    int64_t clickAt = 4800;
    int64_t render(float* out, int channels, int frames) {
        const int64_t p = pos.fetch_add(frames);
        std::fill(out, out + size_t(frames) * size_t(channels), 0.0f);
        if (clickAt >= p && clickAt < p + frames) out[size_t(clickAt - p) * size_t(channels)] = 0.8f;
        return p;
    }
};

// A device test's recording of the first input, placed as a take starting at timeline sample 0.
struct Take {
    std::mutex m;
    std::vector<float> got;
    audiodev::TakeAligner aligner{0, true};
    void add(const float* in, int channels, int frames, int64_t sample) {
        const audiodev::TakeAligner::Step st = aligner.next(sample, frames);
        std::lock_guard<std::mutex> lock(m);
        got.resize(got.size() + size_t(st.silence), 0.0f);
        for (int i = st.skip; i < frames; ++i) got.push_back(in[size_t(i) * size_t(channels)]);
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(m);
        return got.size();
    }
    int64_t click() {
        std::lock_guard<std::mutex> lock(m);
        return audiodev::findClick(got.data(), int64_t(got.size()));
    }
};

// Records a click through `e` (open): where it lands in the take.
int64_t recordClick(audiodev::Engine& e, int rate = 48000) {
    Clicker clicker;
    Take take;
    const int out = e.addOutput(&clicker, rate, 2, [&](float* o, int ch, int n) { return clicker.render(o, ch, n); });
    const int in = e.addInput(&clicker, rate, [&](const float* i, int ch, int n, int64_t at) { take.add(i, ch, n, at); });
    if (out < 0 || in < 0) return -2;
    QElapsedTimer t;
    t.start();
    while (take.size() < size_t(clicker.clickAt + rate / 10) && t.elapsed() < 10000) QThread::msleep(10);
    e.removeInput(in);
    e.removeOutput(out);
    return take.click();
}

// Each test runs twice: in Montage's own process and in plug-in hosts.
void hostingRows() {
    QTest::addColumn<bool>("isolated");
    QTest::newRow("in process") << false;
    QTest::newRow("in a plug-in host") << true;
}

}  // namespace

class TestPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString path(const char* name) const { return dir_.filePath(name); }

private slots:
    void initTestCase() {
        // (MCP's background calls kept in a job store of the tests' own)
        qputenv("MONTAGE_JOBS_DIR", dir_.filePath(QStringLiteral("jobs")).toUtf8());
        // Plug-ins run in Montage's own process unless a test says otherwise (it then names the grouping).
        setHostExecutable(MONTAGE_PLUGIN_HOST);
        HostingSettings h;
        h.isolation = Isolation::InProcess;
        setHosting(h);
    }

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
        QCOMPARE(files.size(), size_t(6));  // good, hang, crash, latency, sidechain, convolver
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
        // Scanned in the probe: the good bundle's seven plug-ins described, the one that crashes blocklisted.
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({good, crash});
        std::vector<std::string> log;
        QCOMPARE(reg.scan(&log), 7);
        QCOMPARE(reg.blocked().size(), size_t(1));
        // (A crash on Linux and macOS; on Windows the test plugin ends its process without the crash dialog.)
        QVERIFY2(reg.blocked()[0].first.find("MontageTestOfxCrash") != std::string::npos &&
                     (reg.blocked()[0].second.find("Crashed") != std::string::npos || reg.blocked()[0].second.find("Could not be loaded") != std::string::npos),
                 reg.blocked()[0].second.c_str());
        QCOMPARE(log.size(), size_t(2));
        // Unchanged files come from the cache, without the probe.
        log.clear();
        QCOMPARE(reg.scan(&log), 7);
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
            const Image img = renderSequenceFrame(back, *back.active(), 3, o);  // kept: the pixel pointer is into it
            const float* px = img.at(32, 18);
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
        // OpenFX 1.5 colour management: a plugin asking for ACEScg is given its picture there (the working space's
        // curve undone and its primaries changed, nothing clipped: a grey of 4.0 in Rec.709 is 4^2.4 in linear light),
        // told so, and its output, in its source's colourspace, comes back into the working space as it was.
        {
            ofx::PluginDesc colour;
            QVERIFY(reg.find("org.montage.test.colour/1", colour));
            QCOMPARE(colour.colour, std::string("OfxImageEffectColourManagementCore"));
            QVERIFY(invert.colour.empty());
            Effect fx = ofx::makeEffect(p, colour);
            Image bright(4, 4);
            bright.fill(4.0f, 4.0f, 4.0f, 1.0f);
            QVERIFY2(ofx::applyEffect(fx, 0, bright, 1.0, {}, &err, findColorSpace("rec709")), err.c_str());
            QVERIFY2(std::fabs(bright.at(1, 1)[0] - std::pow(4.0f, 2.4f)) < 0.02f && bright.at(1, 1)[1] == 1.0f && bright.at(1, 1)[2] == 1.0f,
                     qPrintable(QString("%1 %2 %3").arg(bright.at(1, 1)[0]).arg(bright.at(1, 1)[1]).arg(bright.at(1, 1)[2])));
            fx.params["param.mode"] = Param(1.0);
            bright.fill(4.0f, 0.5f, 0.25f, 1.0f);
            QVERIFY2(ofx::applyEffect(fx, 0, bright, 1.0, {}, &err, findColorSpace("rec709")), err.c_str());
            QVERIFY2(std::fabs(bright.at(1, 1)[0] - 4.0f) < 1e-3f && std::fabs(bright.at(1, 1)[1] - 0.5f) < 1e-4f &&
                         std::fabs(bright.at(1, 1)[2] - 0.25f) < 1e-4f,
                     qPrintable(QString("%1 %2 %3").arg(bright.at(1, 1)[0]).arg(bright.at(1, 1)[1]).arg(bright.at(1, 1)[2])));
            // A sequence graded in ACEScct tells the plugin so, and gives it its picture in ACEScg as it asked.
            fx.params["param.mode"] = Param(2.0);
            bright.fill(0.5f, 0.5f, 0.5f, 1.0f);
            QVERIFY2(ofx::applyEffect(fx, 0, bright, 1.0, {}, &err, findColorSpace("acescct")), err.c_str());
            QVERIFY2(bright.at(1, 1)[1] == 1.0f && bright.at(1, 1)[2] == 1.0f &&
                         std::fabs(bright.at(1, 1)[0] - toLinear(Transfer::AcesCct, 0.5)) < 1e-4,
                     qPrintable(QString("%1 %2 %3").arg(bright.at(1, 1)[0]).arg(bright.at(1, 1)[1]).arg(bright.at(1, 1)[2])));
            bright.fill(0.5f, 0.5f, 0.5f, 1.0f);
            QVERIFY2(ofx::applyEffect(fx, 0, bright, 1.0, {}, &err, findColorSpace("rec709")), err.c_str());
            QCOMPARE(bright.at(1, 1)[1], 0.0f);
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

    void hostsOpenFxTransitionsGeneratorsAndInputs() {
        const std::string good = MONTAGE_TEST_OFX_DIR "/good", crash = MONTAGE_TEST_OFX_DIR "/crash";
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({good, crash});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc dissolve, generator, matte, probe, invert;
        QVERIFY(reg.find("org.montage.test.dissolve/1", dissolve) && reg.find("org.montage.test.generator/1", generator) &&
                reg.find("org.montage.test.matte/1", matte) && reg.find("org.montage.test.probe/1", probe) &&
                reg.find("org.montage.test.invert/1", invert));
        QVERIFY(dissolve.transition() && !dissolve.generator() && generator.generator() && !invert.transition());
        QCOMPARE(matte.context, std::string("OfxImageEffectContextGeneral"));
        QCOMPARE(matte.inputs.size(), size_t(1));
        QVERIFY(matte.inputs[0].name == "Matte" && matte.inputs[0].mask && matte.inputs[0].optional);
        QCOMPARE(probe.overlay, 2);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{25, 1};
        std::string err;
        const float tolerance = 1.0f / 1023;

        // A transition plug-in: the plug-in's Transition parameter is the progress, not a setting.
        Effect fx = ofx::makeEffect(p, dissolve);
        QVERIFY(!fx.params.count("param.Transition"));
        Image plain(4, 4);
        QVERIFY(!ofx::applyEffect(fx, 0, plain, 1.0, {}, &err));
        Image from(8, 4), to(8, 4), mixed;
        from.fill(0.9f, 0.2f, 0.1f, 1.0f);
        to.fill(0.1f, 0.6f, 0.8f, 1.0f);
        for (const double k : {0.25, 0.5, 0.75}) {
            QVERIFY2(ofx::applyTransition(fx, 0, k, from, to, mixed, 1.0, &err), err.c_str());
            QCOMPARE(mixed.width, 8);
            for (int c = 0; c < 4; ++c) {
                const float want = float(from.at(3, 2)[c] * (1 - k) + to.at(3, 2)[c] * k);
                QVERIFY2(std::fabs(mixed.at(3, 2)[c] - want) <= tolerance, qPrintable(QString("%1 %2 %3").arg(k).arg(mixed.at(3, 2)[c]).arg(want)));
            }
        }
        // Either side missing is transparent.
        QVERIFY(ofx::applyTransition(fx, 0, 0.5, Image(), to, mixed, 1.0, &err));
        QVERIFY(std::fabs(mixed.at(1, 1)[3] - 0.5f) <= tolerance && std::fabs(mixed.at(1, 1)[1] - 0.3f) <= tolerance);
        // Between two clips on the timeline: the same pictures as the built-in Cross Dissolve.
        auto colour = [&](float r, float g, float b, FrameTime len) {
            Clip c = makeGeneratorClip(p, "color", len);
            c.generator.params["color.r"] = Param(r);
            c.generator.params["color.g"] = Param(g);
            c.generator.params["color.b"] = Param(b);
            return c;
        };
        RenderOptions o;
        {
            Sequence& t = s;
            const auto first = edit::overwrite(p, t, {TrackKind::Video, 0}, colour(0.9f, 0.2f, 0.1f, 30));
            Clip second = colour(0.1f, 0.6f, 0.8f, 30);
            second.start = 30;
            edit::overwrite(p, t, {TrackKind::Video, 0}, second);
            Project q = p;
            const edit::Result added = edit::addTransition(p, t, first.created[0], edit::Edge::Out, "ofx:org.montage.test.dissolve/1", 8);
            QVERIFY2(added.ok, added.error.c_str());
            QVERIFY(edit::addTransition(q, *q.active(), first.created[0], edit::Edge::Out, "cross_dissolve", 8).ok);
            const Transition* tr = nullptr;
            for (const Transition& x : t.videoTracks[0].transitions) tr = &x;
            QVERIFY(tr && tr->type == "ofx" && tr->params.s("ofx_id") == "org.montage.test.dissolve/1");
            for (FrameTime f = 26; f <= 32; f += 2) {
                const Image a = renderSequenceFrame(p, t, f, o), b = renderSequenceFrame(q, *q.active(), f, o);
                for (int c = 0; c < 3; ++c)
                    QVERIFY2(std::fabs(a.at(32, 18)[c] - b.at(32, 18)[c]) <= tolerance,
                             qPrintable(QString("frame %1: %2 %3").arg(f).arg(a.at(32, 18)[c]).arg(b.at(32, 18)[c])));
            }
            QVERIFY(saveProject(p, path("ofx-transition.montage").toStdString()));
            Project back;
            QVERIFY(loadProject(path("ofx-transition.montage").toStdString(), back));
            const Image a = renderSequenceFrame(p, t, 28, o), b = renderSequenceFrame(back, *back.active(), 28, o);
            QVERIFY(std::fabs(a.at(32, 18)[0] - b.at(32, 18)[0]) <= tolerance);
            t.videoTracks[0].clips.clear();
            t.videoTracks[0].transitions.clear();
        }

        // A generator plug-in makes a generator clip: its colour left of Split, nothing right of it.
        Clip gen = makeGeneratorClip(p, "ofx:org.montage.test.generator/1", 25);
        QCOMPARE(gen.generator.type, std::string("ofx"));
        QCOMPARE(gen.name, std::string("Test Generator"));
        edit::overwrite(p, s, {TrackKind::Video, 0}, gen);
        Image frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY2(std::fabs(frame.at(10, 18)[0] - 0.25f) <= tolerance && std::fabs(frame.at(10, 18)[1] - 0.5f) <= tolerance &&
                     std::fabs(frame.at(10, 18)[2] - 0.75f) <= tolerance,
                 qPrintable(QString("%1 %2 %3").arg(frame.at(10, 18)[0]).arg(frame.at(10, 18)[1]).arg(frame.at(10, 18)[2])));
        QVERIFY(frame.at(50, 18)[0] <= tolerance && frame.at(50, 18)[2] <= tolerance);
        s.videoTracks[0].clips[0].generator.params["param.split"] = Param(0.25);
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY(std::fabs(frame.at(10, 18)[2] - 0.75f) <= tolerance && frame.at(20, 18)[2] <= tolerance);
        // Placed where the clip is moved to (as any generator clip).
        s.videoTracks[0].clips[0].motion.params["pos_x"] = Param(32.0);
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY(frame.at(10, 18)[2] <= tolerance && std::fabs(frame.at(40, 18)[2] - 0.75f) <= tolerance);
        s.videoTracks[0].clips.clear();

        // A General plug-in's matte input reads the track it is connected to (which then is not seen itself), a bin
        // item, or nothing.
        const auto base = edit::overwrite(p, s, {TrackKind::Video, 0}, colour(0.2f, 0.4f, 0.6f, 25));
        if (s.videoTracks.size() < 2) edit::addTrack(p, s, TrackKind::Video);
        Clip half = makeGeneratorClip(p, "ofx:org.montage.test.generator/1", 25);
        half.generator.params["param.colour.r"] = Param(1.0);
        edit::overwrite(p, s, {TrackKind::Video, 1}, half);
        Effect m = ofx::makeEffect(p, matte);
        QVERIFY(m.strings.count("input.Matte") && m.s("input.Matte").empty());
        m.strings["input.Matte"] = "track:" + std::to_string(s.videoTracks[1].id);
        Clip* under = edit::clipById(s, base.created[0]);
        under->effects.push_back(m);
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY2(std::fabs(frame.at(10, 18)[0] - 0.2f) <= tolerance && std::fabs(frame.at(10, 18)[2] - 0.6f) <= tolerance,
                 qPrintable(QString("%1 %2 %3").arg(frame.at(10, 18)[0]).arg(frame.at(10, 18)[1]).arg(frame.at(10, 18)[2])));
        QVERIFY2(frame.at(50, 18)[0] <= tolerance && frame.at(50, 18)[2] <= tolerance, qPrintable(QString::number(frame.at(50, 18)[0])));
        // The matte lines up with the canvas where the clip is moved (seen back through the clip's transform, not
        // stretched over its frame): moved 16 pixels right, it is cut at canvas x 32 still.
        under->motion.params["pos_x"] = Param(16.0);
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY2(std::fabs(frame.at(24, 18)[2] - 0.6f) <= tolerance && frame.at(40, 18)[2] <= tolerance,
                 qPrintable(QString("%1 %2").arg(frame.at(24, 18)[2]).arg(frame.at(40, 18)[2])));
        under->motion.params["pos_x"] = Param(0.0);
        // Not connected: the source as it is.
        under->effects[0].strings["input.Matte"] = "";
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY(std::fabs(frame.at(50, 18)[2] - 0.6f) <= tolerance);
        // The effect's own mask, with none drawn: everything.
        under->effects[0].strings["input.Matte"] = "mask";
        frame = renderSequenceFrame(p, s, 3, o);
        QVERIFY(std::fabs(frame.at(50, 18)[2] - 0.6f) <= tolerance);
        // A picture in the bin: opaque in its top half only.
        {
            QImage q(64, 36, QImage::Format_ARGB32);
            q.fill(Qt::transparent);
            for (int y = 0; y < 18; ++y)
                for (int x = 0; x < 64; ++x) q.setPixel(x, y, qRgba(255, 255, 255, 255));
            QVERIFY(q.save(path("ofx-matte.png")));
            MediaItem item;
            item.id = p.newId();
            QVERIFY(probeMedia(path("ofx-matte.png").toStdString(), item));
            p.media.push_back(item);
            under->effects[0].strings["input.Matte"] = "media:" + std::to_string(item.id);
            frame = renderSequenceFrame(p, s, 3, o);
            QVERIFY2(std::fabs(frame.at(50, 5)[2] - 0.6f) <= tolerance && frame.at(50, 30)[2] <= tolerance,
                     qPrintable(QString("%1 %2").arg(frame.at(50, 5)[2]).arg(frame.at(50, 30)[2])));
        }
        // Saved and loaded, and over MCP (a transition and a generator by their types; inputs by track).
        QVERIFY(saveProject(p, path("ofx-inputs.montage").toStdString()));
        {
            McpServer server;
            auto call = [&](const QString& tool, const QJsonObject& args) {
                const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
                const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
                return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
            };
            const QString project = path("ofx-inputs.montage");
            QJsonObject r = call("montage_list_effects", {{"kind", "transition"}});
            QVERIFY(QJsonDocument(r).toJson().contains("ofx:org.montage.test.dissolve/1"));
            r = call("montage_list_effects", {{"kind", "generator"}});
            QVERIFY(QJsonDocument(r).toJson().contains("ofx:org.montage.test.generator/1"));
            r = call("montage_add_effect", {{"project", project}, {"clip", double(base.created[0])}, {"effect", "ofx:org.montage.test.dissolve/1"}});
            QVERIFY(r.value("isError").toBool());
            r = call("montage_add_generator", {{"project", project}, {"generator", "ofx:org.montage.test.generator/1"}, {"at", 30},
                                               {"duration", 10}, {"track", "V1"}, {"settings", QJsonObject{{"split", 0.75}}}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            Project added;
            QVERIFY(loadProject(project.toStdString(), added));
            const Clip* made = nullptr;
            for (const Clip& c : added.active()->videoTracks[0].clips)
                if (c.generator.type == "ofx") made = &c;
            QVERIFY(made && std::fabs(made->generator.params.at("param.split").value - 0.75) < 1e-9);
            r = call("montage_add_transition", {{"project", project}, {"clip", double(base.created[0])}, {"edge", "out"},
                                                {"type", "ofx:org.montage.test.dissolve/1"}, {"duration", 4}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            r = call("montage_add_effect", {{"project", project}, {"clip", double(base.created[0])}, {"effect", "ofx:org.montage.test.matte/1"},
                                            {"inputs", QJsonObject{{"Matte", "V2"}}}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            QVERIFY(loadProject(project.toStdString(), added));
            const Sequence& as = *added.active();
            QVERIFY(!as.videoTracks[0].transitions.empty() && as.videoTracks[0].transitions.back().type == "ofx");
            const Clip* c = edit::clipById(as, base.created[0]);
            QVERIFY(c && c->effects.size() == 2);
            QCOMPARE(c->effects[1].s("input.Matte"), "track:" + std::to_string(as.videoTracks[1].id));
        }

        // On-screen controls (drawn with the Draw Suite): a 400 x 200 frame shown at its own size, y down in the view.
        Effect pr = ofx::makeEffect(p, probe);
        std::unique_ptr<ofx::Interact> in = ofx::Interact::open(pr, &err);
        QVERIFY2(in, err.c_str());
        QVERIFY(!ofx::Interact::open(ofx::makeEffect(p, dissolve)));  // (no controls)
        ofx::View v;
        v.width = 400, v.height = 200, v.frameW = 400, v.frameH = 200;
        v.m[0] = 1, v.m[1] = 0, v.m[2] = 0, v.m[3] = 0, v.m[4] = -1, v.m[5] = 200;
        QCOMPARE(v.pixelScale(), 1.0);
        ofx::Overlay drawn;
        QVERIFY(in->draw(pr, 0, v, drawn));
        auto guardMark = [&] {  // (the guard file says '1' only while a control runs)
            QFile g(QString::fromStdString(reg.guardPath()));
            return g.open(QIODevice::ReadOnly) ? g.read(1) : QByteArray();
        };
        QCOMPARE(guardMark(), QByteArray("0"));
        QCOMPARE(drawn.ops.size(), size_t(2));
        QCOMPARE(int(drawn.ops[0].kind), int(ofx::DrawOp::Lines));
        QCOMPARE(drawn.ops[0].points.size(), size_t(4));
        QCOMPARE(drawn.ops[0].colour[2], 0.0f);  // yellow
        QCOMPARE(int(drawn.ops[1].kind), int(ofx::DrawOp::Rectangle));
        // Centre (0.5, 0.25) of the frame is canonical (200, 50): the square round it, 10 screen pixels each way.
        QCOMPARE(drawn.ops[1].points[0][0], 190.0);
        QCOMPARE(drawn.ops[1].points[0][1], 40.0);
        QCOMPARE(drawn.ops[1].points[1][0], 210.0);
        QCOMPARE(drawn.ops[1].points[1][1], 60.0);
        // Seen at half size: still 10 screen pixels, 20 canonical.
        ofx::View small = v;
        small.width = 200, small.height = 100, small.m[0] = 0.5, small.m[4] = -0.5, small.m[5] = 100;
        QVERIFY(in->draw(pr, 0, small, drawn) && drawn.ops.size() == 2);
        QCOMPARE(drawn.ops[1].points[0][0], 180.0);
        // A press beside the square is not taken; one on it is, and a drag keys Centre where it is animated.
        Effect edited;
        bool changed = true;
        QVERIFY(!in->pen(ofx::Interact::Pen::Down, pr, 10, v, 20, 20, 1, edited, changed));
        QVERIFY(!changed);
        pr.params["param.centre.x"].addKey(0, 0.5);
        pr.params["param.centre.y"].addKey(0, 0.25);
        QVERIFY(in->pen(ofx::Interact::Pen::Down, pr, 10, v, 205, 148, 1, edited, changed));
        QVERIFY(!changed);
        QVERIFY(in->pen(ofx::Interact::Pen::Move, pr, 10, v, 300, 100, 1, edited, changed));
        QVERIFY(changed);
        QCOMPARE(edited.params.at("param.centre.x").keys.size(), size_t(2));
        QCOMPARE(edited.params.at("param.centre.x").at(10), 0.75);  // 300 of 400
        QCOMPARE(edited.params.at("param.centre.y").at(10), 0.5);   // 100 up from the bottom of 200
        QCOMPARE(edited.params.at("param.centre.x").at(0), 0.5);    // (the key before kept)
        QCOMPARE(pr.params.at("param.centre.x").keys.size(), size_t(1));  // (the effect itself untouched)
        QVERIFY(in->pen(ofx::Interact::Pen::Up, edited, 10, v, 300, 100, 1, edited, changed));
        QVERIFY(!in->pen(ofx::Interact::Pen::Move, edited, 10, v, 320, 100, 1, edited, changed));  // let go
        // Not animated: the value itself.
        Effect still = ofx::makeEffect(p, probe);
        QVERIFY(in->pen(ofx::Interact::Pen::Down, still, 3, v, 200, 150, 1, edited, changed));
        QVERIFY(in->pen(ofx::Interact::Pen::Up, still, 3, v, 100, 150, 1, edited, changed) && changed);
        QVERIFY(edited.params.at("param.centre.x").keys.empty());
        QCOMPARE(edited.params.at("param.centre.x").value, 0.25);
        in.reset();

        // A plug-in Montage stopped in while its controls were showing is blocked at the next scan, until it changes.
        {
            const QString copy = path("ofx-interact-crash");
            const QString from = QString::fromStdString(good);
            QVERIFY(QDir().mkpath(copy));
            std::string binary;
            for (const std::string& b : ofx::findBinaries({good})) binary = b;
            const QString rel = QDir(from).relativeFilePath(QString::fromStdString(binary));
            QVERIFY(QDir().mkpath(QFileInfo(copy + "/" + rel).absolutePath()));
            QVERIFY(QFile::copy(QString::fromStdString(binary), copy + "/" + rel));
            reg.setSearchPaths({copy.toStdString()});
            QCOMPARE(reg.scan(), 7);
            const std::string copied = ofx::findBinaries({copy.toStdString()}).at(0);
            // (as a process that has ended would leave it: this process's own guard is not a crash)
            const QString ended = QFileInfo(QString::fromStdString(reg.guardPath())).absolutePath() + "/ofx-interact-2147483646.guard";
            QFile own(QString::fromStdString(reg.guardPath()));
            QVERIFY(own.open(QIODevice::ReadWrite));  // (written in place: this process keeps it mapped)
            own.write("1\n" + QByteArray::fromStdString(copied) + QByteArray(1, '\0'));
            own.close();
            QCOMPARE(reg.scan(), 7);
            QVERIFY(own.open(QIODevice::ReadWrite));
            own.write("0");
            own.close();
            QFile idle(ended);  // (an ended process that was not in a control at the time)
            QVERIFY(idle.open(QIODevice::WriteOnly));
            idle.write("0\n" + QByteArray::fromStdString(copied) + QByteArray(1, '\0'));
            idle.close();
            QCOMPARE(reg.scan(), 7);
            QVERIFY(!QFileInfo::exists(ended));
            QFile guard(ended);  // (one from before guards were marked: the binary alone)
            QVERIFY(guard.open(QIODevice::WriteOnly));
            guard.write(QByteArray::fromStdString(copied));
            guard.close();
            QCOMPARE(reg.scan(), 0);
            QCOMPARE(reg.blocked().size(), size_t(1));
            QCOMPARE(reg.blocked()[0].second, std::string("Crashed while showing its on-screen controls"));
            QCOMPARE(reg.scan(), 0);  // (still, from the cache)
            reg.setSearchPaths({good, crash});
            QCOMPARE(reg.scan(), 7);
        }
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

    // Phase 15 #2: Apple's own Audio Units in a plug-in host give what they give in Montage's process, bit for bit.
    void appleAudioUnitsInAHost() {
        auto au = readStaticMetadata(Format::AudioUnit, "AudioUnit");
        QVERIFY(au.has_value());
        for (const char* id : {"aufx:dely:appl", "aufx:lpas:appl"}) {
            std::optional<Descriptor> d;
            for (const Descriptor& x : *au)
                if (x.pluginId == id) d = x;
            QVERIFY2(d.has_value(), id);
            std::string err;
            auto here = instantiateHere(*d, &err);
            QVERIFY2(here, err.c_str());
            auto there = instantiateIsolated(*d, Isolation::PerPlugin, &err);
            QVERIFY2(there, err.c_str());
            QVERIFY(hostProcessOf(there.get()) > 0 && hostProcessOf(here.get()) == 0);
            QVERIFY(here->activate(48000, 512) && there->activate(48000, 512));
            QCOMPARE(there->latencySamples(), here->latencySamples());
            // A short delay (AUDelay's time, in seconds), so its echoes fall inside the test.
            for (const ParamInfo& p : here->parameters())
                if (QString::fromStdString(p.name).contains("time", Qt::CaseInsensitive)) {
                    here->setParameter(p.id, 0.02);
                    there->setParameter(p.id, 0.02);
                }
            uint32_t x = 7;
            for (int b = 0; b < 40; ++b) {
                std::vector<float> l(512), r(512);
                for (int i = 0; i < 512; ++i) {
                    x = x * 1664525u + 1013904223u;
                    l[size_t(i)] = r[size_t(i)] = (float(x >> 8) / float(1 << 24) - 0.5f) * (b < 4 ? 1.0f : 0.0f);
                }
                std::vector<float> l2 = l, r2 = r;
                float* a[2] = {l.data(), r.data()};
                float* c[2] = {l2.data(), r2.data()};
                here->process(a, 2, 512);
                there->process(c, 2, 512);
                QVERIFY2(std::memcmp(l.data(), l2.data(), l.size() * 4) == 0 && std::memcmp(r.data(), r2.data(), r.size() * 4) == 0,
                         qPrintable(QString("%1, block %2").arg(id).arg(b)));
            }
            QVERIFY(!bypassed(there.get()));
        }
    }
#endif

    void hostsAClapPlugin_data() { hostingRows(); }
    void hostsAClapPlugin() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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

    void clapEditorProtocol_data() { hostingRows(); }
    void clapEditorProtocol() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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
    void vst3EditorProtocol_data() { hostingRows(); }
    void vst3EditorProtocol() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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

    void vst3Sidechain_data() { hostingRows(); }
    void vst3Sidechain() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
        // The auxiliary input bus is the key: turned on, it hears what it is given, else silence.
        auto files = findPluginFiles(Format::Vst3, {MONTAGE_TEST_VST3_DIR});
        QCOMPARE(files.size(), size_t(1));
        auto ds = probeInProcess(Format::Vst3, files[0]);
        QCOMPARE(ds.size(), size_t(1));
        auto inst = instantiate(ds[0]);
        QVERIFY(inst && inst->activate(48000, 256));
        QVERIFY(inst->hasSidechain());
        std::vector<float> l(600, 0.8f), r(600, 0.8f), k(600, 0.5f);
        float* ch[2] = {l.data(), r.data()};
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(l[300] - 0.8f) < 1e-6f);  // unity gain, no key
        // Until a key is chosen the bus stays off (as DAWs leave it), so a key given anyway is not heard.
        const float* key[2] = {k.data(), k.data()};
        inst->setSidechain(key, 2);
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(l[300] - 0.8f) < 1e-6f);
        inst->enableSidechain(true);
        inst->setSidechain(key, 2);
        inst->process(ch, 2, 600);
        QVERIFY2(std::fabs(l[300] - 0.4f) < 1e-6f && std::fabs(r[599] - 0.4f) < 1e-6f, qPrintable(QString::number(l[300])));
        inst->setSidechain(nullptr, 0);
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(l[300] - 0.4f) < 1e-6f);  // silence on the key: unchanged
        inst->enableSidechain(false);  // and off again
        inst->setSidechain(key, 2);
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(l[300] - 0.4f) < 1e-6f);
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
    void hostsLv2Plugins_data() { hostingRows(); }
    void hostsLv2Plugins() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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
        // Its side chain inputs are the key, not more signal: 0.5 there halves what comes out.
        QVERIFY(inst->hasSidechain());
        inst->setParameter(params[0].id, 0.0);
        std::vector<float> a(600, 0.8f), b(600, 0.8f), k(600, 0.5f);
        float* ab[2] = {a.data(), b.data()};
        const float* key[2] = {k.data(), k.data()};
        inst->setSidechain(key, 2);
        inst->process(ab, 2, 600);
        QVERIFY2(std::fabs(a[300] - 0.4f) < 1e-4f && std::fabs(b[599] - 0.4f) < 1e-4f, qPrintable(QString::number(a[300])));
        inst->setSidechain(nullptr, 0);
        inst->process(ab, 2, 600);
        QVERIFY(std::fabs(a[300] - 0.4f) < 1e-4f);  // 0.4 in, unity, no key

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

    void pluginSidechain_data() { hostingRows(); }
    void pluginSidechain() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
        // A plugin's key input hears what it is given, or silence; plugins without one say so.
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-sidechain.json"), {clapDir("sidechain"), clapDir("good")});
        reg.scan();
        auto d = reg.find("clap:org.montage.test.ducker");
        QVERIFY(d.has_value());
        auto inst = instantiate(*d);
        QVERIFY(inst && inst->activate(48000, 256));
        QVERIFY(inst->hasSidechain());
        std::vector<float> l(600, 0.8f), r(600, 0.8f), kl(600, 0.5f), kr(600, -0.25f);
        float* ch[2] = {l.data(), r.data()};
        const float* key[2] = {kl.data(), kr.data()};
        inst->setSidechain(key, 2);
        inst->process(ch, 2, 600);  // more than one block
        QVERIFY(std::fabs(l[0] - 0.4f) < 1e-6f && std::fabs(l[599] - 0.4f) < 1e-6f && std::fabs(r[599] - 0.6f) < 1e-6f);
        // A mono key goes to both sides.
        std::fill(l.begin(), l.end(), 0.8f);
        std::fill(r.begin(), r.end(), 0.8f);
        const float* mono[1] = {kl.data()};
        inst->setSidechain(mono, 1);
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(r[300] - 0.4f) < 1e-6f);
        // No key: silence there, so nothing is turned down.
        std::fill(l.begin(), l.end(), 0.8f);
        inst->setSidechain(nullptr, 0);
        inst->process(ch, 2, 600);
        QVERIFY(std::fabs(l[300] - 0.8f) < 1e-6f);
        auto gain = reg.find("clap:org.montage.test.gain");
        QVERIFY(gain.has_value());
        auto plain = instantiate(*gain);
        QVERIFY(plain && !plain->hasSidechain());

        // As an effect: marked as having a key input (the gain is not), and in the mixer hearing its sidechain
        // track, muted or not, while that track stays out of the mix.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        std::string err;
        auto e = makePluginEffect(p, *d, &err);
        QVERIFY2(e, err.c_str());
        QCOMPARE(e->s("key_input"), std::string("1"));
        auto g = makePluginEffect(p, *gain, &err);
        QVERIFY(g && g->s("key_input").empty());
        const std::string music = path("sc-music.wav").toStdString(), voice = path("sc-voice.wav").toStdString();
        writeWav(music, 48000, 2.0, 0.5f);
        writeWav(voice, 48000, 2.0, 0.25f);
        for (const std::string& f : {music, voice}) {
            MediaItem m;
            m.id = p.newId();
            QVERIFY2(probeMedia(f, m, &err), err.c_str());
            p.media.push_back(m);
        }
        while (s.audioTracks.size() < 2) edit::addTrack(p, s, TrackKind::Audio);
        for (int t = 0; t < 2; ++t) {
            Clip c = makeClip(p, p.media[p.media.size() - 2 + size_t(t)], TrackKind::Audio, s);
            c.duration = 25;
            QVERIFY(edit::overwrite(p, s, {TrackKind::Audio, t}, c).ok);
        }
        s.audioTracks[1].muted = true;
        auto level = [&] {
            AudioMixer mixer;
            std::vector<float> out(512 * 2);
            mixer.mix(p, s, 4800, 512, out.data());
            return out[600];
        };
        const float alone = level();
        QVERIFY(alone > 0.1f);
        s.audioTracks[0].effects.push_back(*e);
        QVERIFY(std::fabs(level() - alone) < 1e-5f);  // no key chosen
        s.audioTracks[0].effects[0].strings["sidechain"] = std::to_string(s.audioTracks[1].id);
        QVERIFY2(std::fabs(level() - alone * 0.75f) < 1e-3f, qPrintable(QString("%1 vs %2").arg(level()).arg(alone)));
        // Its own track as the key is ignored (a key cannot be its own signal).
        s.audioTracks[0].effects[0].strings["sidechain"] = std::to_string(s.audioTracks[0].id);
        QVERIFY(std::fabs(level() - alone) < 1e-5f);
        // On a clip too.
        s.audioTracks[0].effects.clear();
        Effect onClip = *e;
        onClip.strings["sidechain"] = std::to_string(s.audioTracks[1].id);
        s.audioTracks[0].clips[0].effects.push_back(onClip);
        QVERIFY(std::fabs(level() - alone * 0.75f) < 1e-3f);
        // Known to have a key input once loaded, even for an effect saved without the mark.
        QVERIFY(knownSidechain(d->id) && !knownSidechain(gain->id));
        // A duplicate of the sequence keys from the copy's own track.
        {
            Project dp = p;
            const Id copy = edit::duplicateSequence(dp, dp.activeSequence);
            const Sequence* c = dp.findSequence(copy);
            QVERIFY(c);
            QCOMPARE(c->audioTracks[0].clips[0].effects.at(0).s("sidechain"), std::to_string(c->audioTracks[1].id));
            QVERIFY(c->audioTracks[1].id != s.audioTracks[1].id);
        }

        // In step with the signal through plugin latency: the key's onset (a second in) ducks the sound at that same
        // sample, whether the keyed plugin comes before a latent one on a clip or after one on a track.
        isolate(reg, path("cache-sidechain-latency.json"), {clapDir("sidechain"), clapDir("latency")});
        reg.scan();
        auto delay = reg.find("clap:org.montage.test.delay64");
        auto duck = reg.find("clap:org.montage.test.ducker");
        QVERIFY(delay && duck);
        Project lp = makeDefaultProject();
        Sequence& ls = *lp.active();
        const std::string steady = path("sc-steady.wav").toStdString(), onset = path("sc-onset.wav").toStdString();
        writeWav(steady, 48000, 3.0, 0.5f);
        writeWav(onset, 48000, 1.0, 0.5f);
        for (const std::string& f : {steady, onset}) {
            MediaItem m;
            m.id = lp.newId();
            QVERIFY2(probeMedia(f, m, &err), err.c_str());
            lp.media.push_back(m);
        }
        while (ls.audioTracks.size() < 2) edit::addTrack(lp, ls, TrackKind::Audio);
        Clip main = makeClip(lp, lp.media[0], TrackKind::Audio, ls);
        main.duration = FrameTime(std::llround(3 * ls.fpsValue()));
        QVERIFY(edit::overwrite(lp, ls, {TrackKind::Audio, 0}, main).ok);
        Clip keyClip = makeClip(lp, lp.media[1], TrackKind::Audio, ls);
        keyClip.start = FrameTime(std::llround(ls.fpsValue()));  // from 1 s (sample 48000)
        keyClip.duration = FrameTime(std::llround(ls.fpsValue()));
        QVERIFY(edit::overwrite(lp, ls, {TrackKind::Audio, 1}, keyClip).ok);
        ls.audioTracks[1].muted = true;
        auto keyed = [&](Project& pr) {
            auto fx = makePluginEffect(pr, *duck, &err);
            fx->strings["sidechain"] = std::to_string(ls.audioTracks[1].id);
            return *fx;
        };
        auto duckedFrom = [&] {
            AudioMixer mixer;
            std::vector<float> out(600 * 2);
            mixer.mix(lp, ls, 47700, 600, out.data());
            const float before = out[0];
            for (int i = 0; i < 600; ++i)
                if (out[size_t(i) * 2] < before * 0.8f) return int64_t(47700 + i);
            return int64_t(-1);
        };
        ls.audioTracks[0].clips[0].effects = {keyed(lp), *makePluginEffect(lp, *delay, &err)};
        QCOMPARE(duckedFrom(), int64_t(48000));
        ls.audioTracks[0].clips[0].effects.clear();
        ls.audioTracks[0].effects = {*makePluginEffect(lp, *delay, &err), keyed(lp)};
        QCOMPARE(duckedFrom(), int64_t(48000));
    }

    void pluginDelayCompensation_data() { hostingRows(); }
    void pluginDelayCompensation() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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

    void pluginEffectRunsInTheMixer_data() { hostingRows(); }
    void pluginEffectRunsInTheMixer() {
        QFETCH(bool, isolated);
        Hosting hostingMode(isolated ? Isolation::PerPlugin : Isolation::InProcess);
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

    // ---- Plug-ins in host processes of their own ----

    void isolatedPluginsSoundTheSame_data() {
        QTest::addColumn<QString>("plugin");
        QTest::newRow("CLAP gain") << QString("gain");
        QTest::newRow("CLAP delay") << QString("delay");
        QTest::newRow("CLAP convolver") << QString("convolver");
#ifdef MONTAGE_WITH_VST3
        QTest::newRow("VST3 gain") << QString("vst3");
#endif
#ifdef MONTAGE_TEST_LV2_BUNDLE
        QTest::newRow("LV2 gain") << QString("lv2");
#endif
    }
    void isolatedPluginsSoundTheSame() {
        // The same sound, bit for bit, in Montage's process and in a plug-in host: blocks of every size, parameters
        // changed between blocks, a reset, the transport, and the same latency and state.
        QFETCH(QString, plugin);
        const auto d = testPlugin(plugin);
        QVERIFY(d.has_value());
        const int frames = 48000;
        const std::vector<float> inL = testNoise(size_t(frames), 1), inR = testNoise(size_t(frames), 2);
        struct Run {
            std::vector<float> l, r;
            int latency = -1;
            std::string state;
            std::vector<plugins::ParamInfo> params;
        };
        auto run = [&](Instance& inst) {
            Run out;
            out.l = inL, out.r = inR;
            if (!inst.activate(48000, 512)) return out;
            out.latency = inst.latencySamples();
            out.params = inst.parameters();
            const int sizes[] = {1, 64, 128, 333, 512, 7, 500, 200, 512, 31};
            int at = 0, block = 0;
            while (at < frames) {
                const int n = std::min(sizes[block % 10], frames - at);
                if (!out.params.empty() && (block == 5 || block == 40 || block == 41)) {
                    const plugins::ParamInfo& p0 = out.params[0];
                    inst.setParameter(p0.id, p0.min + (p0.max - p0.min) * (block == 5 ? 0.3 : block == 40 ? 0.9 : 0.6));
                }
                if (block == 60) inst.reset();
                inst.setTransport(at, true, 120, 4);
                float* ch[2] = {out.l.data() + at, out.r.data() + at};
                inst.process(ch, 2, n);
                at += n;
                ++block;
            }
            out.state = inst.saveState();
            return out;
        };
        std::string err;
        auto here = instantiateHere(*d, &err);
        QVERIFY2(here, err.c_str());
        Hosting hostingMode(Isolation::PerPlugin);
        auto there = instantiateIsolated(*d, Isolation::PerPlugin, &err);
        QVERIFY2(there, err.c_str());
        QVERIFY(hostProcessOf(there.get()) > 0 && hostProcessOf(there.get()) != QCoreApplication::applicationPid());
        QCOMPARE(hostProcessOf(here.get()), int64_t(0));
        const Run a = run(*here), b = run(*there);
        QVERIFY(a.latency >= 0);
        QCOMPARE(b.latency, a.latency);
        QCOMPARE(b.params.size(), a.params.size());
        for (size_t i = 0; i < a.params.size(); ++i) {
            QCOMPARE(b.params[i].id, a.params[i].id);
            QCOMPARE(QString::fromStdString(b.params[i].name), QString::fromStdString(a.params[i].name));
            QCOMPARE(b.params[i].max, a.params[i].max);
        }
        QVERIFY(b.state == a.state);
        QVERIFY2(std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
                     std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0,
                 "the plug-in host's output differs from Montage's own");
        QVERIFY(a.l != inL);  // (and it did something)
        QVERIFY(!bypassed(there.get()));
        QVERIFY(hostFailures().empty());
    }

    void isolatedDelayCompensationIsUnchanged() {
        // In the mixer: plug-ins with latency on clips, tracks, a bus and the master give the same mix, sample for
        // sample, in Montage's process and in plug-in hosts (and the step stays where the clip starts).
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-isolated-latency.json"), {clapDir("latency"), clapDir("convolver")});
        reg.scan();
        const auto delay = reg.find("clap:org.montage.test.delay64"), conv = reg.find("clap:org.montage.test.convolver");
        QVERIFY(delay.has_value() && conv.has_value());
        const std::string wav = path("isolated-step.wav").toStdString();
        writeWav(wav, 48000, 2.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, FrameTime(std::llround(s.fpsValue())), 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        auto fx = [&](const Descriptor& d) { return *makePluginEffect(p, d, &err); };
        s.audioTracks[0].clips[0].effects.push_back(fx(*conv));
        s.audioTracks[0].clips[0].effects.push_back(fx(*delay));
        s.audioTracks[0].effects.push_back(fx(*delay));
        Bus b;
        b.id = p.newId();
        b.effects.push_back(fx(*conv));
        s.buses.push_back(b);
        s.audioTracks[0].output = b.id;
        s.masterEffects.push_back(fx(*delay));
        size_t hostsSeen = 0;
        auto mixFrom = [&](int64_t from, int blocks) {
            AudioMixer mixer;
            std::vector<float> all, out(1000 * 2);
            for (int i = 0; i < blocks; ++i) {
                mixer.mix(p, s, from + int64_t(i) * 1000, 1000, out.data());
                all.insert(all.end(), out.begin(), out.end());
            }
            hostsSeen = runningHosts().size();
            return all;
        };
        std::vector<float> here, there;
        {
            Hosting h(Isolation::InProcess);
            here = mixFrom(47000, 6);
            QCOMPARE(hostsSeen, size_t(0));
        }
        {
            Hosting h(Isolation::PerPlugin);
            there = mixFrom(47000, 6);
            QCOMPARE(hostsSeen, size_t(2));  // (the delay's and the convolver's)
        }
        QCOMPARE(there.size(), here.size());
        QVERIFY2(std::memcmp(here.data(), there.data(), here.size() * sizeof(float)) == 0, "the mixes differ");
        // (the step at the clip's start, sample 48000: 1000 samples in)
        QVERIFY(std::fabs(here[999 * 2]) < 1e-6f && std::fabs(here[1000 * 2]) > 0.05f);
    }

    void isolationModesGroupPlugInsIntoHosts() {
        const auto gain = testPlugin("gain"), invert = testPlugin("invert"), delay = testPlugin("delay");
        QVERIFY(gain && invert && delay);
        QCOMPARE(gain->vendor, delay->vendor);
        auto make = [](const Descriptor& d) {
            std::string err;
            auto inst = instantiate(d, &err);
            if (inst) inst->activate(48000, 256);
            return inst;
        };
        auto keys = [] {
            QStringList k;
            for (const HostInfo& h : runningHosts()) k << QString::fromStdString(h.key);
            return k;
        };
        auto pid = [](const std::unique_ptr<Instance>& i) { return i ? hostProcessOf(i.get()) : int64_t(-1); };
        {
            // Each plug-in in a host of its own; the instances of one plug-in share it.
            Hosting h(Isolation::PerPlugin);
            auto a = make(*gain), b = make(*gain), c = make(*invert), e = make(*delay);
            QVERIFY(a && b && c && e);
            QVERIFY(pid(a) > 0 && pid(c) > 0 && pid(e) > 0);
            QCOMPARE(pid(a), pid(b));
            QVERIFY(pid(a) != pid(c) && pid(c) != pid(e) && pid(a) != pid(e));
            QVERIFY2(keys().contains(QString::fromStdString("plugin:" + gain->id)), qPrintable(keys().join(", ")));
        }
        QVERIFY(eventually([&] { return runningHosts().empty(); }));  // (a host with nothing left in it ends)
        {
            // One host for each maker.
            Hosting h(Isolation::PerVendor);
            auto a = make(*gain), c = make(*invert), e = make(*delay);
            QVERIFY(a && c && e && pid(a) > 0);
            QVERIFY(pid(a) == pid(c) && pid(c) == pid(e));
            QVERIFY2(keys().contains(QString::fromStdString("vendor:" + gain->vendor)), qPrintable(keys().join(", ")));
        }
        QVERIFY(eventually([&] { return runningHosts().empty(); }));
        {
            // All plug-ins in one host.
            Hosting h(Isolation::Shared);
            auto a = make(*gain), e = make(*delay);
#ifdef MONTAGE_WITH_VST3
            const auto vst3 = testPlugin("vst3");
            QVERIFY(vst3.has_value());
            auto v = make(*vst3);
            QVERIFY(v && pid(v) == pid(a));
#endif
            QVERIFY(a && e && pid(a) > 0 && pid(a) == pid(e));
            QCOMPARE(keys(), QStringList{"all"});
            QCOMPARE(runningHosts()[0].pid, pid(a));
        }
        {
            // Off: in Montage's own process.
            Hosting h(Isolation::InProcess);
            auto a = make(*gain);
            QVERIFY(a);
            QCOMPARE(pid(a), int64_t(0));
        }
        QVERIFY(eventually([&] { return runningHosts().empty(); }));
        for (Isolation i : {Isolation::PerPlugin, Isolation::PerVendor, Isolation::Shared, Isolation::InProcess})
            QVERIFY(isolationFromName(isolationName(i)) == std::optional<Isolation>(i));
        QVERIFY(!isolationFromName("nonsense"));
        // ARA plug-ins share the document: never in a host, whatever the setting.
        Descriptor ara = *gain;
        ara.ara = true;
        Hosting h(Isolation::PerPlugin);
        auto inProcess = instantiate(ara);
        QVERIFY(inProcess);
        QCOMPARE(hostProcessOf(inProcess.get()), int64_t(0));
    }

    void failingPluginsAreBypassed_data() {
        QTest::addColumn<QString>("plugin");
        QTest::addColumn<QString>("fault");
        QTest::addColumn<bool>("realtime");
        QStringList formats{"gain"};
#ifdef MONTAGE_WITH_VST3
        formats << "vst3";
#endif
#ifdef MONTAGE_TEST_LV2_BUNDLE
        formats << "lv2";
#endif
        for (const QString& f : formats) {
            for (const char* fault : {"crash", "hang", "overrun"})
                QTest::addRow("%s %s, playing", qPrintable(f), fault) << f << QString(fault) << true;
            // (a leak in an export: playback would also count the slow blocks it causes as late)
            QTest::addRow("%s leak, exporting", qPrintable(f)) << f << QString("leak") << false;
        }
        QTest::addRow("gain crash, exporting") << QString("gain") << QString("crash") << false;
        QTest::addRow("gain hang, exporting") << QString("gain") << QString("hang") << false;
    }
    void failingPluginsAreBypassed() {
        // A plug-in that crashes, hangs, writes past its buffers or leaks memory takes only its host down: it is
        // bypassed (its input passes), its failure is recorded by name, and Restart brings it back with its settings.
        QFETCH(QString, plugin);
        QFETCH(QString, fault);
        QFETCH(bool, realtime);
        const auto d = testPlugin(plugin);
        QVERIFY(d.has_value());
        const int missed = 8;
        Hosting hostingMode(Isolation::PerPlugin, [&](HostingSettings& s) {
            s.missedBlocks = missed;
            s.memoryLimitMb = 1024;
            s.exportTimeoutMs = 3000;
            s.autoRestarts = 0;  // (it stays bypassed until Restart, below)
        });
        const int goodBlocks = 4;
        const std::string spec = (fault + ":process:" + QString::number(goodBlocks)).toStdString();
        std::string err;
        std::unique_ptr<Instance> inst;
        {
            Fault f(spec.c_str());
            inst = instantiateIsolated(*d, Isolation::PerPlugin, &err);
        }
        QVERIFY2(inst, err.c_str());
        QVERIFY(inst->activate(48000, 128));
        const GainSetting half = halfGain(plugin, *inst);
        inst->setParameter(half.id, half.value);
        const int64_t firstHost = hostProcessOf(inst.get());
        QVERIFY(firstHost > 0);
        const uint64_t offlineBefore = offlineFailureSerial();
        RenderModeScope mode(realtime);
        std::vector<float> l(128), r(128);
        float* ch[2] = {l.data(), r.data()};
        auto block = [&] {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            inst->process(ch, 2, 128);
        };
        int blocks = 0;
        qint64 slowest = 0;
        QElapsedTimer clock;
        while (blocks < 200 && !bypassed(inst.get())) {
            clock.start();
            block();
            slowest = std::max(slowest, clock.elapsed());
            ++blocks;
        }
        QVERIFY2(bypassed(inst.get()), qPrintable(QString("still running after %1 blocks").arg(blocks)));
        // Bypassed at the block it failed in (a hang during playback: once it has been late the set number of
        // blocks in a row; a leak: once its host passes the memory limit).
        const int bound = fault == "leak" ? 64 : fault == "hang" && realtime ? goodBlocks + missed + 2 : goodBlocks + 2;
        QVERIFY2(blocks <= bound, qPrintable(QString("bypassed after %1 blocks").arg(blocks)));
        // Playback never waits long for it (an export waits up to its time limit).
        QVERIFY2(slowest < (realtime ? 5000 : 30000), qPrintable(QString("a block took %1 ms").arg(slowest)));
        // Its input passes (the LV2 gain's, delayed by its latency: the same, as it is constant).
        block();
        QVERIFY2(l[127] == 0.5f && r[0] == 0.5f, qPrintable(QString::number(l[127])));
        // Named in the failure list, with what happened.
        const QString kind = fault == "hang" ? "hang" : fault == "leak" ? "memory" : "crash";
        QVERIFY(eventually([&] { return failureOf(d->id).has_value(); }));
        const HostFailure f = *failureOf(d->id);
        QCOMPARE(QString::fromStdString(f.name), QString::fromStdString(d->name));
        QVERIFY2(QString::fromStdString(f.kind) == kind, (f.kind + ": " + f.reason).c_str());
        QCOMPARE(f.count, 1);
        QVERIFY(!f.reason.empty() && f.bypassedNow >= 1 && f.firstMs > 0);
        // (a host over its memory limit may end between blocks: it is then found ended, not in a render)
        if (fault != "leak") QVERIFY(f.offline == !realtime);
        if (!realtime) {
            QVERIFY(offlineFailureSerial() != offlineBefore);
            QVERIFY2(QString::fromStdString(lastOfflineFailure()).startsWith(QString::fromStdString(d->name)), lastOfflineFailure().c_str());
        }
        // Restart: a new host, with the parameter as it was set.
        QVERIFY(restartPlugin(d->id));
        QVERIFY(eventually([&] {
            block();
            return !bypassed(inst.get());
        }));
        QVERIFY(hostProcessOf(inst.get()) != firstHost);
        QVERIFY(eventually([&] {
            block();
            return std::fabs(l[127] - float(0.5 * half.gain)) < 1e-3f;
        }, 10000));
        QVERIFY(std::fabs(inst->parameter(half.id) - half.value) < 1e-4);
        QVERIFY(eventually([&] { return failureOf(d->id)->restarts == 1; }));
    }

    // Nightly 10 Nov: a hang in a shared host is recorded once. The other instance in that host (idle, or playing on
    // into the host Montage stopped) is started again without a "crashed (signal 9)" of its own, and without using
    // an automatic restart.
    void hangInASharedHostRecordedOnce() {
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting h(Isolation::Shared, [](HostingSettings& s) {
            s.missedBlocks = 4;
            s.autoRestarts = 1;
        });
        std::string err;
        std::unique_ptr<Instance> hangs, idle;
        {
            Fault f("hang:process:3");
            hangs = instantiate(*d, &err);
            QVERIFY2(hangs, err.c_str());
            idle = instantiate(*d, &err);
            QVERIFY2(idle, err.c_str());
        }
        QVERIFY(hangs->activate(48000, 128) && idle->activate(48000, 128));
        QVERIFY(hostProcessOf(hangs.get()) > 0 && hostProcessOf(hangs.get()) == hostProcessOf(idle.get()));
        RenderModeScope mode(true);
        std::vector<float> l(128), r(128);
        float* ch[2] = {l.data(), r.data()};
        auto block = [&](Instance& inst) {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            inst.process(ch, 2, 128);
        };
        for (int i = 0; i < 200 && !bypassed(hangs.get()); ++i) block(*hangs);
        QVERIFY(bypassed(hangs.get()));
        QVERIFY(eventually([&] { return failureOf(d->id).has_value(); }));
        // Both come back (the hung one with the one automatic restart, the other freely), the failure once.
        QVERIFY(eventually([&] {
            block(*idle);
            block(*hangs);
            return !bypassed(idle.get()) && !bypassed(hangs.get());
        }));
        QThread::msleep(500);  // (the supervisor's passes after it)
        const HostFailure f = *failureOf(d->id);
        QCOMPARE(f.count, 1);
        QVERIFY2(f.kind == "hang", (f.kind + ": " + f.reason).c_str());
    }

    // Nightly 10 Nov: shared memory is reserved when it is made, so a block /dev/shm cannot hold is refused then
    // (with why), rather than Montage ending with SIGBUS when the plug-in host's picture first reaches it.
    void sharedMemoryTooLargeIsRefused() {
#ifdef __linux__
        std::string err;
        auto huge = ipc::SharedMemory::create(ipc::uniqueName("t"), size_t(1) << 44, &err);  // (16 TB)
        QVERIFY(!huge);
        QVERIFY2(err.find("not enough shared memory") != std::string::npos || err.find("cannot") != std::string::npos, err.c_str());
        auto fine = ipc::SharedMemory::create(ipc::uniqueName("t"), size_t(1) << 20, &err);
        QVERIFY2(fine && fine->size() == size_t(1) << 20, err.c_str());
        std::memset(fine->data(), 1, fine->size());
#else
        QSKIP("/dev/shm is Linux's");
#endif
    }

    void bypassCanBeSilence() {
        // The setting: a bypassed plug-in gives silence instead of its input.
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) {
            s.bypassWithSilence = true;
            s.autoRestarts = 0;
        });
        std::unique_ptr<Instance> inst;
        {
            Fault f("crash:process:0");
            inst = instantiate(*d);
        }
        QVERIFY(inst && inst->activate(48000, 128));
        std::vector<float> l(128, 0.5f), r(128, 0.5f);
        float* ch[2] = {l.data(), r.data()};
        inst->process(ch, 2, 128);
        QVERIFY(bypassed(inst.get()));
        QCOMPARE(l[0], 0.0f);
        std::fill(l.begin(), l.end(), 0.5f);
        inst->process(ch, 2, 128);
        QCOMPARE(l[64], 0.0f);
    }

    void playbackCarriesOnWhenAPluginHangs() {
        // A plug-in on a clip stops answering during playback: the mix goes on (the clip's sound passes) and the
        // plug-in comes back by itself in a new host.
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-playback.json"), {clapDir("good")});
        reg.scan();
        const auto d = reg.find("clap:org.montage.test.gain");
        QVERIFY(d.has_value());
        const std::string wav = path("playback.wav").toStdString();
        writeWav(wav, 48000, 20.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        std::vector<float> out(128 * 2);
        {
            AudioMixer warm;  // (the clip's sound decoded, as playback has it)
            warm.mix(p, s, 0, 128, out.data());
        }
        auto e = makePluginEffect(p, *d, &err);
        QVERIFY2(e.has_value(), err.c_str());
        e->params["param.7"] = Param(0.25);
        s.audioTracks[0].clips[0].effects.push_back(*e);
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& h) {
            h.missedBlocks = 8;
            h.autoRestarts = 1;
        });
        AudioMixer mixer;
        mixer.setNonBlocking(true);
        {
            Fault f("hang:process:40");
            mixer.mix(p, s, 0, 128, out.data());
        }
        QVERIFY2(std::fabs(out[0] - 0.125f) < 0.002f, qPrintable(QString::number(out[0])));
        int64_t pos = 128;
        bool passed = false, back = false;
        qint64 slowest = 0;
        QElapsedTimer total, clock;
        total.start();
        while (!back && total.elapsed() < 60000) {
            clock.start();
            mixer.mix(p, s, pos, 128, out.data());
            slowest = std::max(slowest, clock.elapsed());
            pos += 128;
            if (pos > 18 * 48000) pos = 128;
            if (std::fabs(out[0] - 0.5f) < 0.002f) passed = true;
            else if (passed && std::fabs(out[0] - 0.125f) < 0.002f) back = true;
            QThread::usleep(2000);  // (about as fast as playback asks)
        }
        QVERIFY2(passed && back, qPrintable(QString("passed %1, back %2").arg(passed).arg(back)));
        QVERIFY2(slowest < 5000, qPrintable(QString("a block took %1 ms").arg(slowest)));
        const auto f = failureOf(d->id);
        QVERIFY(f.has_value());
        QCOMPARE(QString::fromStdString(f->kind), QString("hang"));
        QCOMPARE(QString::fromStdString(f->name), QString("Montage Test Gain"));
        QVERIFY(!f->offline && f->restarts == 1);
    }

    void exportStopsNamingAFailedPlugin() {
        // An export with a plug-in that crashes stops and says which; the next export runs (in a new host).
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-export.json"), {clapDir("good")});
        reg.scan();
        const auto d = reg.find("clap:org.montage.test.gain");
        QVERIFY(d.has_value());
        const std::string wav = path("export.wav").toStdString();
        writeWav(wav, 48000, 2.0, 0.5f);
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{25, 1};
        MediaItem m;
        m.id = p.newId();
        std::string err;
        QVERIFY2(probeMedia(wav, m, &err), err.c_str());
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        s.audioTracks[0].clips[0].effects.push_back(*makePluginEffect(p, *d, &err));
        Hosting hostingMode(Isolation::PerPlugin);
        ExportSettings st;
        st.path = path("plugin-export.mkv").toStdString();
        st.videoCodec = "ffv1";
        st.audioCodec = "pcm_s16le";
        bool ok = true;
        {
            Fault f("crash:process:3");
            ok = exportSequence(p, s, st, nullptr, nullptr, &err);
        }
        QVERIFY(!ok);
        QVERIFY2(QString::fromStdString(err).startsWith("Export stopped because the plug-in Montage Test Gain"), err.c_str());
        QVERIFY(eventually([&] { return failureOf(d->id).has_value(); }));
        QVERIFY(failureOf(d->id)->offline);
        // (a host started again while the fault was set has it too: the next export is to get a host without it)
        QVERIFY(eventually([] { return runningHosts().empty(); }, 20000));
        err.clear();
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
    }

    void offlineRenderWaitsForARestart() {
        // A render that may wait (an export) and finds its plug-in being started again after a failure waits for it,
        // within the export's limit, instead of stopping at it: it is heard, and no export is told of a failure.
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) { s.autoRestarts = 2; });
        auto inst = instantiate(*d);
        QVERIFY(inst && inst->activate(48000, 256));
        inst->setParameter(7, 0.7);
        std::vector<float> l(256), r(256);
        float* ch[2] = {l.data(), r.data()};
        RenderModeScope offline(false);
        auto level = [&] {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            inst->process(ch, 2, 256);
            return l[255];
        };
        QCOMPARE(level(), 0.35f);
        // Ended between blocks, and found so by the supervisor: bypassed, a new host on its way (being started: the
        // instance still names the old one).
        const int64_t pid = hostProcessOf(inst.get());
        QVERIFY(ipc::killProcess(pid));
        QVERIFY(eventually([&] { return bypassed(inst.get()) && hostProcessOf(inst.get()) == pid; }, 20000));
        const uint64_t offlineBefore = offlineFailureSerial();
        QCOMPARE(level(), 0.35f);  // (waited for, not bypassed)
        QVERIFY(!bypassed(inst.get()) && hostProcessOf(inst.get()) != pid);
        QCOMPARE(offlineFailureSerial(), offlineBefore);
    }

    void stateSurvivesAHostRestart() {
        // A host that ends (here: killed) is started again with the instance's last state and parameters.
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) { s.autoRestarts = 2; });
        auto here = instantiateHere(*d);
        QVERIFY(here && here->activate(48000, 256));
        here->setParameter(7, 0.7);
        std::vector<float> l(256, 0.5f), r(256, 0.5f);
        float* ch[2] = {l.data(), r.data()};
        here->process(ch, 2, 256);
        const std::string state = here->saveState();
        auto inst = instantiate(*d);
        QVERIFY(inst && inst->activate(48000, 256));
        QVERIFY(inst->loadState(state));
        auto level = [&] {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            inst->process(ch, 2, 256);
            return l[255];
        };
        QCOMPARE(level(), 0.35f);
        auto killAndWait = [&] {
            const int64_t pid = hostProcessOf(inst.get());
            if (!ipc::killProcess(pid)) return false;
            return eventually([&] {
                level();
                return !bypassed(inst.get()) && hostProcessOf(inst.get()) != pid && hostProcessOf(inst.get()) > 0;
            });
        };
        QVERIFY(killAndWait());
        QCOMPARE(level(), 0.35f);  // the state it was given
        QCOMPARE(inst->parameter(7), 0.7);
        inst->setParameter(7, 1.5);
        QCOMPARE(level(), 0.75f);
        QVERIFY(killAndWait());
        QCOMPARE(level(), 0.75f);  // and the parameter set after it
        QCOMPARE(inst->saveState().size(), sizeof(double));
        const auto f = failureOf(d->id);
        QVERIFY(f.has_value());
        QCOMPARE(f->count, 2);
        QCOMPARE(f->restarts, 2);
        QCOMPARE(QString::fromStdString(f->kind), QString("crash"));
    }

    void pluginThatCrashesWhileLoadingIsBlocked() {
        // As one that crashes the scan: on the blocklist until the file changes or it is tried again.
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-load-crash.json"), {clapDir("good")});
        reg.scan();
        const auto d = reg.find("clap:org.montage.test.gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin);
        std::string err;
        {
            Fault f("crash:init");
            QVERIFY(!instantiate(*d, &err));
        }
        QVERIFY(!err.empty());
        bool blocked = false;
        for (const Blocked& b : reg.blocklist()) blocked |= b.path == d->path && b.reason.find("crashed while loading") != std::string::npos;
        QVERIFY(blocked);
        QVERIFY(!reg.find("clap:org.montage.test.gain").has_value());
        const auto f = failureOf(d->id);
        QVERIFY(f.has_value());
        QCOMPARE(QString::fromStdString(f->kind), QString("load"));
        reg.unblock(d->path);
        reg.scan();
        QVERIFY(reg.find("clap:org.montage.test.gain").has_value());
        QVERIFY(instantiate(*d, &err));
    }

    void editorEventsComeBackFromTheHost() {
        // The editor runs in the host; its knob changes reach Montage (for undo and automation), and when the host
        // fails the editor is closed.
        struct Listener : EditorListener {
            std::vector<std::pair<uint32_t, double>> params;
            int closed = 0;
            void editorParameter(uint32_t id, double v) override { params.emplace_back(id, v); }
            void editorGesture(uint32_t, bool) override {}
            void editorResize(int, int) override {}
            void editorClosed() override { ++closed; }
        } listener;
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) { s.autoRestarts = 0; });
        auto inst = instantiate(*d);
        QVERIFY(inst && inst->hasEditor());
        int w = 0, h = 0, window = 0;
        QVERIFY(inst->openEditor(&window, &listener, w, h));
        QVERIFY(w > 0 && h > 0);
        inst->idle();
        QCOMPARE(listener.params.size(), size_t(1));
        QCOMPARE(listener.params[0], std::make_pair(7u, 0.25));
        QVERIFY(ipc::killProcess(hostProcessOf(inst.get())));
        QVERIFY(eventually([&] {
            inst->idle();
            return listener.closed == 1;
        }));
    }

    void roundTripAt128Samples() {
        // What one block's trip to a plug-in host and back costs, at 128 samples and 48 kHz (2.67 ms of sound).
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        Hosting hostingMode(Isolation::PerPlugin);
        auto inst = instantiate(*d);
        QVERIFY(inst && inst->activate(48000, 128));
        auto here = instantiateHere(*d);
        QVERIFY(here && here->activate(48000, 128));
        RenderModeScope playing(true);
        std::vector<float> l(128, 0.5f), r(128, 0.5f);
        float* ch[2] = {l.data(), r.data()};
        auto measure = [&](Instance& i) {
            for (int n = 0; n < 200; ++n) i.process(ch, 2, 128);
            std::vector<double> us;
            for (int n = 0; n < 2000; ++n) {
                const auto t0 = std::chrono::steady_clock::now();
                i.process(ch, 2, 128);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            }
            std::sort(us.begin(), us.end());
            return std::array<double, 3>{us[us.size() / 2], us[us.size() * 99 / 100], us.back()};
        };
        const auto remote = measure(*inst), local = measure(*here);
        qInfo("Plug-in host round trip, 128 samples at 48 kHz: median %.1f us, 99th percentile %.1f us, slowest %.1f us "
              "(in Montage's process: median %.1f us)", remote[0], remote[1], remote[2], local[0]);
        QVERIFY(!bypassed(inst.get()));
        QVERIFY2(remote[0] < 10000, "the median round trip takes longer than a few blocks");
    }

    void isolatedOpenFxRendersTheSame() {
        // OpenFX effects, a temporal effect, a General plug-in reading another track, a transition and a generator:
        // the same pictures, bit for bit, rendered in a plug-in host as in Montage's process, which loads none.
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-isolated-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc invert, temporal, matte, generator;
        QVERIFY(reg.find("org.montage.test.invert/1", invert) && reg.find("org.montage.test.temporal/1", temporal) &&
                reg.find("org.montage.test.matte/1", matte) && reg.find("org.montage.test.generator/1", generator));
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{25, 1};
        auto colour = [&](float r, float g, float b, FrameTime len) {
            Clip c = makeGeneratorClip(p, "color", len);
            c.generator.params["color.r"] = Param(r);
            c.generator.params["color.g"] = Param(g);
            c.generator.params["color.b"] = Param(b);
            return c;
        };
        const auto first = edit::overwrite(p, s, {TrackKind::Video, 0}, colour(0.9f, 0.2f, 0.1f, 30));
        Clip second = colour(0.1f, 0.6f, 0.8f, 30);
        second.start = 30;
        edit::overwrite(p, s, {TrackKind::Video, 0}, second);
        QVERIFY(edit::addTransition(p, s, first.created[0], edit::Edge::Out, "ofx:org.montage.test.dissolve/1", 8).ok);
        if (s.videoTracks.size() < 2) edit::addTrack(p, s, TrackKind::Video);
        Clip half = makeGeneratorClip(p, "ofx:org.montage.test.generator/1", 60);
        half.generator.params["param.colour.r"] = Param(1.0);
        edit::overwrite(p, s, {TrackKind::Video, 1}, half);
        Clip* a = edit::clipById(s, first.created[0]);
        Effect inv = ofx::makeEffect(p, invert);
        inv.params["param.amount"].addKey(0, 0.0);
        inv.params["param.amount"].addKey(10, 1.0);
        inv.params["param.tint.g"] = Param(0.5);
        a->effects.push_back(inv);
        a->effects.push_back(ofx::makeEffect(p, temporal));
        Effect m = ofx::makeEffect(p, matte);
        m.strings["input.Matte"] = "track:" + std::to_string(s.videoTracks[1].id);
        a->effects.push_back(m);
        RenderOptions o;
        const FrameTime frames[] = {0, 3, 7, 12, 25, 27, 29, 33, 45};
        std::vector<Image> here, there;
        {
            Hosting h(Isolation::InProcess);
            for (FrameTime f : frames) here.push_back(renderSequenceFrame(p, s, f, o));
        }
        const int loadedHere = ofx::instancesCreated();
        {
            Hosting h(Isolation::PerPlugin);
            for (FrameTime f : frames) there.push_back(renderSequenceFrame(p, s, f, o));
            QVERIFY(!runningHosts().empty());
        }
        QCOMPARE(ofx::instancesCreated(), loadedHere);  // (none made in this process)
        for (size_t i = 0; i < here.size(); ++i) {
            QCOMPARE(there[i].width, here[i].width);
            QCOMPARE(there[i].height, here[i].height);
            QVERIFY2(std::memcmp(here[i].px.data(), there[i].px.data(), here[i].px.size() * sizeof(float)) == 0,
                     qPrintable(QString("frame %1 differs: %2 / %3").arg(frames[i]).arg(here[i].at(10, 18)[0]).arg(there[i].at(10, 18)[0])));
        }
        QVERIFY(std::fabs(here[1].at(10, 18)[0] - here[2].at(10, 18)[0]) > 0.01f);  // (the keyframes did something)
    }

    void failingOpenFxPluginsAreBypassed() {
        // An OpenFX plug-in that crashes or hangs is bypassed for that frame (the picture as it came), named, and
        // started again for the next; past its restarts it stays bypassed until Restart.
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-fault-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc invert;
        QVERIFY(reg.find("org.montage.test.invert/1", invert));
        Project p = makeDefaultProject();
        const Effect e = ofx::makeEffect(p, invert);
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) {
            s.autoRestarts = 1;
            s.exportTimeoutMs = 2000;
        });
        auto render = [&](std::string* err) {
            Image img(8, 8);
            img.fill(0.2f, 0.4f, 0.6f, 1.0f);
            const bool ok = ofx::applyEffect(e, 0, img, 1.0, {}, err);
            return std::make_pair(ok, img.at(3, 3)[0]);
        };
        std::string err;
        {
            Fault f("crash:render:1");
            QVERIFY2(render(&err).first, err.c_str());  // (the first render, in a host started with the fault)
        }
        auto r = render(&err);
        QVERIFY(!r.first);
        QCOMPARE(r.second, 0.2f);  // as it came
        QVERIFY2(err.find("Test Invert") != std::string::npos, err.c_str());
        auto f = failureOf(invert.id);
        QVERIFY(f.has_value());
        QCOMPARE(QString::fromStdString(f->kind), QString("crash"));
        r = render(&err);
        QVERIFY2(r.first && std::fabs(r.second - 0.8f) < 1e-5f, err.c_str());  // started again
        ofx::remote::releaseIdle();
        {
            Fault fault("hang:render:0");
            QElapsedTimer clock;
            clock.start();
            r = render(&err);
            QVERIFY(!r.first);
            QVERIFY2(clock.elapsed() < 30000, "waited past the time limit");
        }
        QVERIFY2(err.find("did not answer") != std::string::npos, err.c_str());
        f = failureOf(invert.id);
        QCOMPARE(QString::fromStdString(f->kind), QString("hang"));
        QCOMPARE(f->count, 2);
        // No restarts left: bypassed until Restart.
        r = render(&err);
        QVERIFY(!r.first);
        QVERIFY2(err.find("bypassed") != std::string::npos, err.c_str());
        QVERIFY(failureOf(invert.id)->bypassedNow >= 1);
        QVERIFY(restartPlugin(invert.id));
        r = render(&err);
        QVERIFY2(r.first && std::fabs(r.second - 0.8f) < 1e-5f, err.c_str());
    }

    // Phase 15 #2: OpenFX on-screen controls drawn by the plug-in's host. The host draws them (the Draw Suite and
    // OpenGL alike) into one premultiplied picture at the monitor's size and pixel ratio; it is the drawing Montage
    // makes in its own process, within 1/255, and a drag sets what it sets there. No instance is made in Montage.
    void isolatedOpenFxControlsDrawTheSame() {
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-controls-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc probe, invert;
        QVERIFY(reg.find("org.montage.test.probe/1", probe) && reg.find("org.montage.test.invert/1", invert));
        Project p = makeDefaultProject();
        Effect e = ofx::makeEffect(p, probe);
        e.params["param.centre.x"] = Param(0.3);
        e.params["param.centre.y"] = Param(0.6);
        // A 400 x 200 frame shown at its own size, y down: Centre (0.3, 0.6) is at view (120, 80).
        ofx::View v;
        v.width = 400, v.height = 200, v.frameW = 400, v.frameH = 200;
        v.m[0] = 1, v.m[1] = 0, v.m[2] = 0, v.m[3] = 0, v.m[4] = -1, v.m[5] = 200;
        auto painted = [](const ofx::Overlay& o, const ofx::View& view) {
            QImage img(view.width, view.height, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter painter(&img);
            ofx::paintOverlay(painter, o, view);
            painter.end();
            return img;
        };
        auto worst = [](const QImage& a, const QImage& b) {
            int w = a.size() == b.size() ? 0 : 255;
            for (int y = 0; w < 255 && y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) {
                    const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                    w = std::max({w, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q)),
                                  std::abs(qAlpha(p) - qAlpha(q))});
                }
            return w;
        };
        // A press on the square, a drag and the release: Centre where the pen let go.
        auto drag = [&](ofx::Interact& c, Effect& now) {
            Effect edited;
            bool changed = false;
            if (!c.pen(ofx::Interact::Pen::Down, now, 0, v, 120, 80, 1, edited, changed)) return false;
            if (!c.pen(ofx::Interact::Pen::Move, now, 0, v, 150, 60, 1, edited, changed) || !changed) return false;
            now = edited;
            if (!c.pen(ofx::Interact::Pen::Up, now, 0, v, 160, 50, 1, edited, changed) || !changed) return false;
            now = edited;
            return true;
        };
        std::string err;
        QImage here, there;
        Effect draggedHere = e, draggedThere = e;
        {
            Hosting h(Isolation::InProcess);
            auto c = ofx::Interact::open(e, &err);
            QVERIFY2(c && !c->remote(), err.c_str());
            ofx::Overlay o;
            QVERIFY(c->draw(e, 0, v, o));
            here = painted(o, v);
            QVERIFY(drag(*c, draggedHere));
        }
        const int made = ofx::instancesCreated();
        {
            Hosting h(Isolation::PerPlugin);
            auto c = ofx::Interact::open(e, &err);
            QVERIFY2(c && c->remote(), err.c_str());
            ofx::Overlay o;
            QVERIFY(c->draw(e, 0, v, o));
            QVERIFY(o.ops.empty() && !o.gl.isNull());  // (one picture, drawn in the host)
            there = painted(o, v);
            QVERIFY(!c->unresponsive());
            QVERIFY(drag(*c, draggedThere));
            // At twice the pixels (a high-density monitor): drawn that finely, shown at the view's size.
            ofx::View fine = v;
            fine.pixelRatio = 2;
            QVERIFY(c->draw(e, 0, fine, o));
            QCOMPARE(o.gl.size(), QSize(800, 400));
            QCOMPARE(o.gl.devicePixelRatio(), 2.0);
            const QColor centre = o.gl.pixelColor(240, 160);
            QVERIFY2(centre.alpha() == 255 && centre.red() == 255 && centre.green() == 0, qPrintable(centre.name(QColor::HexArgb)));
        }
        QCOMPARE(ofx::instancesCreated(), made);  // (none made in this process)
        const QColor square = there.pixelColor(120, 80);
        QVERIFY2(square.alpha() == 255 && square.red() == 255 && square.green() == 0, qPrintable(square.name(QColor::HexArgb)));
        QVERIFY2(worst(here, there) <= 1, qPrintable(QString::number(worst(here, there))));
        for (const char* k : {"param.centre.x", "param.centre.y"}) {
            QVERIFY(draggedHere.params.at(k) == draggedThere.params.at(k));
            QVERIFY(std::fabs(draggedThere.params.at(k).at(0) - e.params.at(k).at(0)) > 0.01);  // (it moved)
        }
        QVERIFY(std::fabs(draggedThere.params.at("param.centre.x").at(0) - 0.4) < 1e-9);  // view x 160 of 400
        // OpenGL controls (Test Invert's green square, canonical (100, 100) to (200, 200)), drawn in the host's own
        // offscreen OpenGL. (This test is no GUI application, so Montage's process has no OpenGL to compare with: the
        // drawing it makes there, which render's ofxControlsAndOpenGl checks, is the square on the view's pixels.)
        if (invert.overlay != 1) QSKIP("The test plug-in was built without OpenGL");
        const Effect inv = ofx::makeEffect(p, invert);
        here = QImage(v.width, v.height, QImage::Format_ARGB32_Premultiplied);
        here.fill(Qt::transparent);
        for (int y = 0; y < 100; ++y)
            for (int x = 100; x < 200; ++x) here.setPixel(x, y, qRgba(0, 255, 0, 255));
        {
            Hosting h(Isolation::PerPlugin);
            auto c = ofx::Interact::open(inv, &err);
            QVERIFY2(c && c->remote(), err.c_str());
            ofx::Overlay o;
            QVERIFY(c->draw(inv, 0, v, o));
            if (o.gl.isNull()) QSKIP("No OpenGL in the plug-in host here (it drew nothing, as in Montage then)");
            there = painted(o, v);
        }
        QCOMPARE(qGreen(there.pixel(150, 50)), 255);
        QCOMPARE(qAlpha(there.pixel(150, 150)), 0);
        QVERIFY2(worst(here, there) <= 1, qPrintable(QString::number(worst(here, there))));
    }

    // Controls that crash their host leave Montage running: closed, the plug-in named and bypassed as for a render.
    // Ones that hang keep their last picture up, marked as not responding within the deadline, and their host is
    // stopped as hung once they have not answered for longer.
    void failingOpenFxControlsAreBypassed() {
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-controls-fault-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc probe;
        QVERIFY(reg.find("org.montage.test.probe/1", probe));
        Project p = makeDefaultProject();
        // (a host for each instance: each effect below starts one of its own, with the fault set then)
        Hosting hostingMode(Isolation::PerInstance, [](HostingSettings& s) { s.autoRestarts = 0; });
        ofx::View v;
        v.width = 200, v.height = 100, v.frameW = 200, v.frameH = 100;
        v.m[4] = -1, v.m[5] = 100;
        std::string err;
        const Effect crashing = ofx::makeEffect(p, probe);
        std::unique_ptr<ofx::Interact> c;
        {
            Fault f("crash:draw:1");
            c = ofx::Interact::open(crashing, &err);
        }
        QVERIFY2(c && c->remote(), err.c_str());
        ofx::Overlay o;
        QVERIFY(c->draw(crashing, 0, v, o) && !o.gl.isNull());
        QVERIFY(!c->draw(crashing, 0, v, o));
        QVERIFY(c->failed() && o.gl.isNull());
        QVERIFY2(c->failure().find("crashed") != std::string::npos, c->failure().c_str());
        auto f = failureOf(probe.id);
        QVERIFY(f.has_value());
        QCOMPARE(QString::fromStdString(f->kind), QString("crash"));
        QCOMPARE(QString::fromStdString(f->name), QString("Test Probe"));
        // Bypassed past its restarts (none here): its renders pass the picture as it came, until Restart.
        Image img(8, 8);
        img.fill(0.2f, 0.4f, 0.6f, 1.0f);
        QVERIFY(!ofx::applyEffect(crashing, 0, img, 1.0, {}, &err));
        QVERIFY2(err.find("bypassed") != std::string::npos, err.c_str());
        QVERIFY(failureOf(probe.id)->bypassedNow >= 1);
        c.reset();
        QVERIFY(restartPlugin(probe.id));
        QVERIFY2(ofx::applyEffect(crashing, 0, img, 1.0, {}, &err), err.c_str());
        // A hang: the picture it drew last, at once, marked.
        const Effect hanging = ofx::makeEffect(p, probe);
        {
            Fault fault("hang:draw:1");
            c = ofx::Interact::open(hanging, &err);
        }
        QVERIFY2(c && c->remote(), err.c_str());
        QVERIFY(c->draw(hanging, 0, v, o) && !o.gl.isNull());
        const QImage last = o.gl;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(c->draw(hanging, 0, v, o));
        QVERIFY2(clock.elapsed() < ofx::remote::Controls::kDeadlineMs + 400, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(c->unresponsive());
        QVERIFY(o.gl == last);
        clock.start();
        QVERIFY(c->draw(hanging, 0, v, o) && o.gl == last);  // (not waited for again)
        QVERIFY(clock.elapsed() < 100);
        Effect edited;
        bool changed = false;
        QVERIFY(!c->pen(ofx::Interact::Pen::Down, hanging, 0, v, 100, 50, 1, edited, changed));  // (not sent meanwhile)
        QVERIFY(eventually([&] {
            c->draw(hanging, 0, v, o);
            return c->failed();
        }, 30000));
        f = failureOf(probe.id);
        QCOMPARE(QString::fromStdString(f->kind), QString("hang"));
        QVERIFY2(f->reason.find("did not answer") != std::string::npos, f->reason.c_str());
        QCOMPARE(f->count, 2);
    }

    // Controls that answer after the deadline but do answer: a slow draw's picture is taken at a later call (draws are
    // then not waited for, each showing the picture the one before asked for), what a late pen reply set is handed on
    // rather than lost, and controls closed with a reply still out let their channel go once it comes.
    void lateOpenFxControlRepliesAreKept() {
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(path("ofx-controls-late-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 7);
        ofx::PluginDesc probe;
        QVERIFY(reg.find("org.montage.test.probe/1", probe));
        Project p = makeDefaultProject();
        Hosting hostingMode(Isolation::PerInstance);
        // (as isolatedOpenFxControlsDrawTheSame: Centre (0.3, 0.6) is at view (120, 80))
        ofx::View v;
        v.width = 400, v.height = 200, v.frameW = 400, v.frameH = 200;
        v.m[0] = 1, v.m[1] = 0, v.m[2] = 0, v.m[3] = 0, v.m[4] = -1, v.m[5] = 200;
        std::string err;
        Effect slow = ofx::makeEffect(p, probe);
        slow.params["param.centre.x"] = Param(0.3);
        slow.params["param.centre.y"] = Param(0.6);
        std::unique_ptr<ofx::Interact> c;
        {
            Fault f("slow:draw");
            c = ofx::Interact::open(slow, &err);
        }
        QVERIFY2(c && c->remote(), err.c_str());
        ofx::Overlay o;
        QVERIFY(c->draw(slow, 0, v, o));
        QVERIFY(o.gl.isNull() && c->waiting() && c->unresponsive());  // (nothing to show yet)
        QVERIFY(eventually([&] {
            c->draw(slow, 0, v, o);
            return !o.gl.isNull();
        }, 10000));
        QVERIFY(qAlpha(o.gl.pixel(120, 80)) > 0);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(c->draw(slow, 0, v, o) && !o.gl.isNull());
        QVERIFY2(clock.elapsed() < 100, qPrintable(QString::number(clock.elapsed())));  // (not waited for)
        QVERIFY(!c->failed() && c->waiting());
        c.reset();
        // A release answered late: the Centre it set comes as a late edit.
        Effect late = slow;
        late.id = p.newId();
        {
            Fault f("slow:pen:1");
            c = ofx::Interact::open(late, &err);
        }
        QVERIFY2(c && c->remote(), err.c_str());
        Effect edited;
        bool changed = false;
        QVERIFY(c->pen(ofx::Interact::Pen::Down, late, 0, v, 120, 80, 1, edited, changed));
        QVERIFY(c->pen(ofx::Interact::Pen::Move, late, 0, v, 150, 60, 1, edited, changed) && changed);
        late = edited;
        QVERIFY(!c->pen(ofx::Interact::Pen::Up, late, 0, v, 160, 50, 1, edited, changed));  // (0.4 s: past the deadline)
        QVERIFY(!changed && c->waiting());
        Effect set;
        QVERIFY(eventually([&] { return c->lateEdits(set); }, 10000));
        QVERIFY(set.params.count("param.centre.x") && std::fabs(set.params.at("param.centre.x").at(0) - 0.4) < 1e-9);
        QVERIFY(!c->lateEdits(set));  // (handed on once)
        QVERIFY(!c->failed() && !failureOf(probe.id).has_value());
        // Closed with the release still out: its host ends once the reply has come and the channel is let go.
        changed = false;
        QVERIFY(c->pen(ofx::Interact::Pen::Down, late, 0, v, 150, 60, 1, edited, changed));  // (where the move left it)
        QVERIFY(!c->pen(ofx::Interact::Pen::Up, late, 0, v, 170, 40, 1, edited, changed));
        QVERIFY(c->waiting());
        c.reset();
        QVERIFY(eventually([] {
            ofx::remote::releaseIdle();
            return runningHosts().empty();
        }, 20000));
        QVERIFY(!failureOf(probe.id).has_value());
    }

    // In a host shared by a maker's plug-ins, the second of two crashes it while loading: that one alone is blocked
    // (the host said which plug-in it was loading), and the first is started again as if nothing of its own failed.
    void sharedHostBlamesThePlugInLoading() {
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-blame.json"), {clapDir("good"), clapDir("latency")});
        reg.scan();
        const auto gain = reg.find("clap:org.montage.test.gain"), delay = reg.find("clap:org.montage.test.delay64");
        QVERIFY(gain && delay);
        QCOMPARE(gain->vendor, delay->vendor);
        QVERIFY(gain->path != delay->path);
        Hosting hostingMode(Isolation::PerVendor, [](HostingSettings& s) { s.autoRestarts = 0; });
        std::string err;
        std::unique_ptr<Instance> first;
        {
            Fault f("crash:latency-init");
            first = instantiate(*gain, &err);
        }
        QVERIFY2(first && first->activate(48000, 128), err.c_str());
        const GainSetting half = halfGain("gain", *first);
        first->setParameter(half.id, half.value);
        const int64_t shared = hostProcessOf(first.get());
        QVERIFY(shared > 0);
        RenderModeScope playing(true);
        std::vector<float> l(128), r(128);
        float* ch[2] = {l.data(), r.data()};
        auto block = [&] {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            first->process(ch, 2, 128);
            return l[127];
        };
        QCOMPARE(block(), float(0.5 * half.gain));
        QVERIFY(!instantiate(*delay, &err));  // (in the same host, which crashes as it loads it)
        QVERIFY2(err.find("while loading") != std::string::npos, err.c_str());
        bool delayBlocked = false, gainBlocked = false;
        for (const Blocked& b : reg.blocklist()) {
            delayBlocked |= b.path == delay->path;
            gainBlocked |= b.path == gain->path;
        }
        QVERIFY(delayBlocked && !gainBlocked);
        QVERIFY(!reg.find("clap:org.montage.test.delay64") && reg.find("clap:org.montage.test.gain"));
        QCOMPARE(QString::fromStdString(failureOf(delay->id)->kind), QString("load"));
        // The first plays on in a new host (with no automatic restarts left: its restart is not counted as one).
        QVERIFY(eventually([&] {
            block();
            return !bypassed(first.get()) && hostProcessOf(first.get()) > 0 && hostProcessOf(first.get()) != shared;
        }));
        QVERIFY(eventually([&] { return std::fabs(block() - float(0.5 * half.gain)) < 1e-6f; }, 10000));
        QVERIFY(!failureOf(gain->id).has_value());
        reg.unblock(delay->path);
        reg.scan();
        QVERIFY(reg.find("clap:org.montage.test.delay64").has_value());
    }

    // A host for each instance: a crash in one leaves its sibling (the same plug-in) untouched, for audio and OpenFX.
    void hostPerInstanceKeepsSiblingsApart() {
        const auto gain = testPlugin("gain");
        QVERIFY(gain.has_value());
        Hosting hostingMode(Isolation::PerInstance, [](HostingSettings& s) { s.autoRestarts = 0; });
        std::string err;
        std::unique_ptr<Instance> crashing, sibling;
        {
            Fault f("crash:process:4");
            crashing = instantiate(*gain, &err);
        }
        sibling = instantiate(*gain, &err);
        QVERIFY2(crashing && sibling, err.c_str());
        QVERIFY(crashing->activate(48000, 128) && sibling->activate(48000, 128));
        QVERIFY(hostProcessOf(crashing.get()) > 0 && hostProcessOf(sibling.get()) > 0);
        QVERIFY(hostProcessOf(crashing.get()) != hostProcessOf(sibling.get()));
        int perInstance = 0;
        for (const HostInfo& h : runningHosts()) perInstance += h.key.rfind("instance:" + gain->id + "#", 0) == 0;
        QCOMPARE(perInstance, 2);
        const GainSetting half = halfGain("gain", *sibling);
        sibling->setParameter(half.id, half.value);
        RenderModeScope playing(true);
        std::vector<float> l(128), r(128);
        float* ch[2] = {l.data(), r.data()};
        bool siblingBypassed = false;
        for (int i = 0; i < 40; ++i) {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            crashing->process(ch, 2, 128);
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
            sibling->process(ch, 2, 128);
            QCOMPARE(l[127], float(0.5 * half.gain));
            siblingBypassed |= bypassed(sibling.get());
        }
        QVERIFY(bypassed(crashing.get()));
        QVERIFY(!siblingBypassed);
        QVERIFY(eventually([&] { return failureOf(gain->id).has_value(); }));
        QCOMPARE(failureOf(gain->id)->count, 1);
        // OpenFX: two Test Inverts, each in a host of its own; the one that crashes is bypassed alone.
        ofx::Registry& oreg = ofx::Registry::instance();
        oreg.setCachePath(path("ofx-instance-cache.json").toStdString());
        oreg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        oreg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(oreg.scan(), 7);
        ofx::PluginDesc invert;
        QVERIFY(oreg.find("org.montage.test.invert/1", invert));
        Project p = makeDefaultProject();
        const Effect a = ofx::makeEffect(p, invert), b = ofx::makeEffect(p, invert);
        auto render = [&](const Effect& e) {
            Image img(8, 8);
            img.fill(0.2f, 0.4f, 0.6f, 1.0f);
            const bool ok = ofx::applyEffect(e, 0, img, 1.0, {}, &err);
            return ok ? img.at(3, 3)[0] : -1.0f;
        };
        {
            Fault f("crash:render:0");
            QCOMPARE(render(a), -1.0f);
        }
        QVERIFY(std::fabs(render(b) - 0.8f) < 1e-5f);
        QCOMPARE(render(a), -1.0f);  // (bypassed, past its restarts)
        QVERIFY2(err.find("bypassed") != std::string::npos, err.c_str());
        QVERIFY(std::fabs(render(b) - 0.8f) < 1e-5f);
        bool own = false;
        for (const HostInfo& h : runningHosts()) own |= h.key == "ofx:instance:" + invert.id + "#" + std::to_string(b.id);
        QVERIFY(own);
        QVERIFY(restartPlugin(invert.id));
        QVERIFY(std::fabs(render(a) - 0.8f) < 1e-5f);
    }

    void mcpReportsAndSetsPluginHosting() {
        // montage_plugins: the grouping read and set, the hosts running, and each failure (which plug-in, when, how
        // often), with Restart.
        McpServer server;
        auto call = [&](const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "montage_plugins"}, {"arguments", args}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        Hosting hostingMode(Isolation::PerPlugin, [](HostingSettings& s) { s.autoRestarts = 0; });
        QJsonObject r = call({});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QJsonObject o = r.value("structuredContent").toObject();
        QCOMPARE(o.value("isolation").toString(), QString("plugin"));
        QVERIFY(o.value("host_available").toBool());
        QCOMPARE(o.value("failures").toArray().size(), 0);
        r = call({{"isolation", "vendor"}, {"missed_blocks", 32}, {"bypass", "silence"}});
        o = r.value("structuredContent").toObject();
        QCOMPARE(o.value("isolation").toString(), QString("vendor"));
        QVERIFY(hosting().isolation == Isolation::PerVendor);
        QCOMPARE(hosting().missedBlocks, 32);
        QVERIFY(hosting().bypassWithSilence);
        QVERIFY(call({{"isolation", "nonsense"}}).value("isError").toBool());
        call({{"isolation", "plugin"}, {"bypass", "input"}});
        // A crash, as the tool reports it.
        const auto d = testPlugin("gain");
        QVERIFY(d.has_value());
        std::unique_ptr<Instance> inst;
        {
            Fault f("crash:process:0");
            inst = instantiate(*d);
        }
        QVERIFY(inst && inst->activate(48000, 128));
        std::vector<float> l(128, 0.5f), rr(128, 0.5f);
        float* ch[2] = {l.data(), rr.data()};
        inst->process(ch, 2, 128);
        QVERIFY(eventually([&] { return failureOf(d->id).has_value(); }));
        o = call({}).value("structuredContent").toObject();
        QCOMPARE(o.value("failures").toArray().size(), 1);
        const QJsonObject fo = o.value("failures").toArray()[0].toObject();
        QCOMPARE(fo.value("plugin").toString(), QString::fromStdString(d->id));
        QCOMPARE(fo.value("name").toString(), QString("Montage Test Gain"));
        QCOMPARE(fo.value("kind").toString(), QString("crash"));
        QCOMPARE(fo.value("count").toInt(), 1);
        QVERIFY(fo.value("bypassed").toInt() >= 1 && !fo.value("first").toString().isEmpty());
        r = call({{"restart", QString::fromStdString(d->id)}});
        QVERIFY(r.value("content").toArray()[0].toObject().value("text").toString().contains("Starting"));
        QVERIFY(eventually([&] {
            inst->process(ch, 2, 128);
            return !bypassed(inst.get());
        }));
        o = call({{"clear_failures", true}}).value("structuredContent").toObject();
        QCOMPARE(o.value("failures").toArray().size(), 0);
        QVERIFY(!o.value("hosts").toArray().isEmpty());
    }

    void audioDeviceSettings() {
        // The device's settings round-trip, buffers are powers of two from 32 to 2048, and a measured loopback is used
        // only at the rate and buffer it was measured at.
        audiodev::DeviceSettings s;
        s.host = audiodev::Host::Jack;
        s.output = "system";
        s.sampleRate = 96000;
        s.bufferFrames = 128;
        s.clock = "Word clock";
        s.inputs = 4;
        s.inputNames = {"Dialogue", "", "Room"};
        s.loopbackFrames = 700, s.loopbackRate = 96000, s.loopbackBuffer = 128;
        QCOMPARE(audiodev::DeviceSettings::fromJson(s.toJson()), s);
        QCOMPARE(audiodev::clampBuffer(1), 32);
        QCOMPARE(audiodev::clampBuffer(100), 128);
        QCOMPARE(audiodev::clampBuffer(256), 256);
        QCOMPARE(audiodev::clampBuffer(9000), 2048);
        QCOMPARE(audiodev::inputName(s, 0), std::string("Dialogue"));
        QCOMPARE(audiodev::inputName(s, 1), std::string("In 2"));
        QCOMPARE(audiodev::inputName(s, 3), std::string("In 4"));
        bool measured = false;
        QCOMPARE(audiodev::roundTrip(s, 96000, 128, {200, 300}, &measured), 700);
        QVERIFY(measured);
        QCOMPARE(audiodev::roundTrip(s, 48000, 128, {200, 300}, &measured), 500);
        QVERIFY(!measured);
        QCOMPARE(audiodev::hostFromId("wasapi_exclusive").value(), audiodev::Host::WasapiExclusive);
        QVERIFY(!audiodev::hostFromId("nothing"));
        QVERIFY(!audiodev::hostBuilt(audiodev::Host::Asio));  // (its SDK is not built)
        QCOMPARE(audiodev::availableHosts().front(), audiodev::Host::Qt);

        // A click is found at its first sample of half its peak; quiet recordings have none.
        std::vector<float> r(1000, 0.001f);
        r[400] = 0.3f, r[401] = 0.9f, r[402] = 0.5f;
        QCOMPARE(audiodev::findClick(r.data(), 1000), int64_t(401));
        QCOMPARE(audiodev::findClick(r.data(), 400), int64_t(-1));

        // Takes: clocked, blocks wait for the playback; a later block starts after silence, an earlier one is cut.
        audiodev::TakeAligner wait(1000, true);
        QCOMPARE(wait.next(-1, 256).skip, 256);
        QVERIFY(!wait.started());
        audiodev::TakeAligner::Step st = wait.next(1100, 256);
        QCOMPARE(st.silence, int64_t(100));
        QCOMPARE(st.skip, 0);
        audiodev::TakeAligner early(1000, true);
        st = early.next(700, 256);
        QCOMPARE(st.skip, 256);
        st = early.next(956, 256);
        QCOMPARE(st.skip, 44);
        QCOMPARE(st.silence, int64_t(0));
        QCOMPARE(early.next(1212, 256).skip, 0);
        // After an input overflow (a kilosample lost), the rest lands where its blocks say, not early; a few samples'
        // jitter is kept as it comes.
        QCOMPARE(early.next(1468 + 1024, 256).silence, int64_t(1024));
        st = early.next(2748 + 10, 256);
        QCOMPARE(st.silence, int64_t(0));
        QCOMPARE(st.skip, 0);
        // A jump back (the playback looped, or frames came twice) carries the take on from there: nothing passed over.
        st = early.next(3014 - 600, 256);
        QCOMPARE(st.skip, 0);
        QCOMPARE(st.silence, int64_t(0));
        QCOMPARE(early.next(2670, 256).skip, 0);
        QCOMPARE(early.next(2926, 256).silence, int64_t(0));
        // A hundred frames lost a block add up: kept twice (within a block), made up on the third.
        QCOMPARE(early.next(3182 + 100, 256).silence, int64_t(0));
        QCOMPARE(early.next(3538 + 100, 256).silence, int64_t(0));
        QCOMPARE(early.next(3894 + 100, 256).silence, int64_t(300));
        QCOMPARE(early.next(4250, 256).silence, int64_t(0));
        // Moved on by more than two seconds (a seek): no minute of silence, the take goes on from there.
        st = early.next(4506 + 480000, 256);
        QCOMPARE(st.silence, int64_t(0));
        QCOMPARE(st.skip, 0);
        QCOMPARE(early.next(4762 + 480000, 256).silence, int64_t(0));
        // Unclocked, the input's latency is taken off its start.
        audiodev::TakeAligner loose(0, false, 300);
        QCOMPARE(loose.next(-1, 256).skip, 256);
        QCOMPARE(loose.next(-1, 256).skip, 44);
        QCOMPARE(loose.next(-1, 256).skip, 0);
        // Two seconds are the take's own: at 192 kHz a gap of 1.5 s is still made up, and one of 2.5 s is a seek.
        audiodev::TakeAligner fast(0, true, 0, 192000);
        QCOMPARE(fast.next(0, 256).silence, int64_t(0));
        QCOMPARE(fast.next(256 + 288000, 256).silence, int64_t(288000));
        QCOMPARE(fast.next(288512 + 480000, 256).silence, int64_t(0));
    }

    void loopbackRecordingLandsAtZero_data() {
        QTest::addColumn<int>("buffer");
        QTest::newRow("64") << 64;
        QTest::newRow("256") << 256;
        QTest::newRow("1024") << 1024;
    }
    void loopbackRecordingLandsAtZero() {
        // A click played out through a loopback (the outputs back on the inputs, as by a cable) is recorded and, the
        // device's reported latencies taken off, lands where it was played.
        QFETCH(int, buffer);
        audiodev::setLoopbackOptions({buffer, 2 * buffer, 0});
        audiodev::Engine e;
        audiodev::DeviceSettings s;
        s.host = audiodev::Host::Loopback;
        s.bufferFrames = buffer;
        s.sampleRate = 48000;
        e.setSettings(s);
        QVERIFY(e.isOpen());
        const audiodev::EngineStatus st = e.status();
        QCOMPARE(st.buffer, buffer);
        QCOMPARE(st.latency.input, buffer);
        QCOMPARE(st.latency.output, 2 * buffer);
        QCOMPARE(st.roundTrip, 3 * buffer);
        QVERIFY(!st.measured);
        QCOMPARE(recordClick(e), int64_t(4800));

        // A round trip longer than the device says (converters it does not report): measured, and taken off instead.
        audiodev::setLoopbackOptions({buffer, 2 * buffer, 300});
        s.host = audiodev::Host::Qt;
        e.setSettings(s);
        QVERIFY(!e.isOpen());
        s.host = audiodev::Host::Loopback;
        e.setSettings(s);
        QCOMPARE(recordClick(e), int64_t(4800 + 300));
        std::string why;
        QCOMPARE(e.measureLoopback(0, 0, 0.5, &why), 3 * buffer + 300);
        QVERIFY(e.status().measured);
        QCOMPARE(e.settings().loopbackFrames, 3 * buffer + 300);
        QCOMPARE(recordClick(e), int64_t(4800));
        // Nothing joined from that output to that input: nothing measured, and the last measurement kept.
        QCOMPARE(e.measureLoopback(1, 0, 0.2, &why), -1);
        QVERIFY(!why.empty());
        QCOMPARE(e.settings().loopbackFrames, 3 * buffer + 300);
        audiodev::setLoopbackOptions({});
    }

    void loopbackFollowsRateChanges() {
        // The device's rate changed mid-session (as a server's is): Montage follows it, resampling its 48 kHz both ways,
        // and recordings still land where they were played.
        audiodev::setLoopbackOptions({256, 256, 0});
        audiodev::Engine e;
        audiodev::DeviceSettings s;
        s.host = audiodev::Host::Loopback;
        s.bufferFrames = 256;
        s.sampleRate = 48000;
        e.setSettings(s);
        QVERIFY(e.isOpen());
        QCOMPARE(recordClick(e), int64_t(4800));
        audiodev::loopbackChangeRate(96000);
        QTRY_COMPARE(e.status().rate, 96000);
        QCOMPARE(e.status().rateChanges, 1);
        QCOMPARE(e.roundTripAt(48000), 256);  // (512 frames at 96 kHz)
        const int64_t at = recordClick(e);
        QVERIFY2(std::llabs(at - 4800) <= 16, qPrintable(QString::number(at)));
        // The playback still runs in real time at its own rate: about 48000 of its samples a second (of the time waited).
        Clicker clicker;
        const int id = e.addOutput(&clicker, 48000, 2, [&](float* o, int ch, int n) { return clicker.render(o, ch, n); });
        QVERIFY(id >= 0);
        QElapsedTimer t;
        t.start();
        QTest::qWait(1000);
        const int64_t played = std::llround(double(clicker.pos.load()) * 1e9 / double(std::max<qint64>(1, t.nsecsElapsed())));
        e.removeOutput(id);
        QVERIFY2(played > 40000 && played < 58000, qPrintable(QString::number(played)));
        audiodev::setLoopbackOptions({});
    }

    void overloadedCallbackIsAnXrun() {
        // A callback held past its period (an overloaded computer) is counted as an xrun, and the count can be reset.
        audiodev::Engine e;
        audiodev::DeviceSettings s;
        s.host = audiodev::Host::Loopback;
        s.bufferFrames = 128;
        s.sampleRate = 48000;
        e.setSettings(s);
        QVERIFY(e.isOpen());
        QTest::qWait(200);
        e.resetXruns();
        const int64_t before = e.status().xruns;
        QVERIFY(before < 3);
        audiodev::loopbackStall(60);
        QTRY_VERIFY(e.status().xruns > before);
        e.resetXruns();
        QVERIFY(e.status().xruns < 3);
    }

    void jackPortsAndLoopback() {
        // Under JACK's dummy driver (a test tool), or a PipeWire server with its JACK library: Montage's ports appear with
        // the monitoring section's names, a click looped from an output port back into an input port is recorded and,
        // the loop measured, lands where it was played at buffers of 64, 256 and 1024, and an overloaded callback is an
        // xrun.
        QProcess jackd;
        const QString server = QStringLiteral("montage-test-%1").arg(QCoreApplication::applicationPid());
        const QString exe = QStandardPaths::findExecutable(QStringLiteral("jackd"));
        if (!exe.isEmpty()) {
            qputenv("JACK_DEFAULT_SERVER", server.toUtf8());
            qputenv("JACK_NO_AUDIO_RESERVATION", "1");
            jackd.start(exe, {"--no-realtime", "-n", server, "-d", "dummy", "-r", "48000", "-p", "256"});
            QVERIFY(jackd.waitForStarted(5000));
        }
        if (audiodev::jackLibrary().empty()) QSKIP("No JACK library (PipeWire's or a system libjack) loads here");
        audiodev::Engine e;
        audiodev::DeviceSettings s;
        s.host = audiodev::Host::Jack;
        s.bufferFrames = 256;
        s.inputs = 2;
        s.inputNames = {"Dialogue", "Room"};
        s.autoConnect = false;
        e.setOutputNames({"L", "R", "C"});
        e.setOutputChannels(3);
        QElapsedTimer t;
        t.start();
        do {
            e.setSettings(audiodev::DeviceSettings{});
            e.setSettings(s);
            if (!e.isOpen()) QThread::msleep(200);
        } while (!e.isOpen() && t.elapsed() < 10000);
        if (!e.isOpen()) {
            if (exe.isEmpty()) QSKIP("No jackd here, and no JACK or PipeWire server answers");
            QFAIL(e.status().error.c_str());
        }
        const audiodev::EngineStatus st = e.status();
        for (const char* port : {":L", ":R", ":C", ":Dialogue", ":Room"})
            QVERIFY2(std::any_of(st.ports.begin(), st.ports.end(), [&](const std::string& p) { return p.size() > strlen(port) && p.compare(p.size() - strlen(port), strlen(port), port) == 0; }),
                     port);
        const std::string client = st.ports.front().substr(0, st.ports.front().find(':'));
        // The speakers renamed (another set): the ports follow.
        e.setOutputNames({"Lw", "Rw", "Cw"});
        const auto renamed = e.status().ports;
        QVERIFY(std::find(renamed.begin(), renamed.end(), client + ":Lw") != renamed.end());
        for (int buffer : {64, 256, 1024}) {
            s.bufferFrames = buffer;
            s.loopbackFrames = 0;
            e.setSettings(audiodev::DeviceSettings{});
            e.setSettings(s);
            QVERIFY2(e.isOpen(), e.status().error.c_str());
            QVERIFY(e.connect(client + ":Lw", client + ":Dialogue"));
            std::string why;
            const int trip = e.measureLoopback(0, 0, 0.5, &why);
            QVERIFY2(trip > 0, why.c_str());
            QCOMPARE(recordClick(e, e.status().rate), int64_t(4800));
        }
        // A callback that overruns its period.
        e.resetXruns();
        const int64_t before = e.status().xruns;
        std::atomic<int> slow{1};
        const int id = e.addOutput(&slow, 48000, 2, [&](float* o, int ch, int n) {
            std::fill(o, o + size_t(n) * size_t(ch), 0.0f);
            if (slow.exchange(0)) QThread::msleep(100);
            return int64_t(-1);
        });
        QVERIFY(id >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(e.status().xruns > before, 5000);
        e.removeOutput(id);
        e.setSettings(audiodev::DeviceSettings{});
        if (jackd.state() != QProcess::NotRunning) {
            jackd.terminate();
            jackd.waitForFinished(5000);
        }
    }

    void araPlugIns() {
        // ARA plug-ins (audio/AraHost.h), with the ARA SDK's test plug-in built as VST3 and CLAP plug-ins
        // (scripts/build-ara-test-plugin.sh): it reads the clip's sound itself, finds a "note" in each stretch of
        // samples that are not zero, plays back the source as it read it, and keeps what it found in its archive.
        const QByteArray dir = qgetenv("MONTAGE_TEST_ARA_PLUGINS");
        if (dir.isEmpty()) QSKIP("MONTAGE_TEST_ARA_PLUGINS is not set (scripts/build-ara-test-plugin.sh builds the test plug-in)");
        Registry& reg = Registry::instance();
        isolate(reg, path("cache-ara.json"), {dir.toStdString()});
        reg.setSearchPaths(Format::Vst3, {dir.toStdString()});
        reg.scan();
        std::vector<Descriptor> found;
        for (const Descriptor& d : reg.plugins())
            if (d.ara && canHost(d.format)) found.push_back(d);
        QCOMPARE(int(found.size()), canHost(Format::Vst3) ? 2 : 1);

        // Three recordings: bursts of sound (never exactly zero within) with silence between, each its own.
        const int rate = 48000;
        struct Burst {
            int start, length;
        };
        const std::vector<std::vector<Burst>> bursts = {
            {{4800, 9600}, {24000, 4800}}, {{0, 12000}, {36000, 7200}, {60000, 2400}}, {{12000, 30000}}};
        const int lengths[] = {48000, 72000, 60000};
        std::vector<std::string> wavs;
        for (int k = 0; k < 3; ++k) {
            wavs.push_back(path(qPrintable(QStringLiteral("ara-%1.wav").arg(k))).toStdString());
            writeWavOf(wavs.back(), rate, lengths[k], [&](int i, int ch) {
                for (const Burst& b : bursts[size_t(k)])
                    if (i >= b.start && i < b.start + b.length)
                        return float((0.3 + 0.1 * k) * (ch ? -1 : 1) * (0.6 + 0.4 * std::sin(i * (0.01 + 0.003 * k))));
                return 0.0f;
            });
        }
        const int regionsBefore = ara::liveRegions();

        for (const Descriptor& d : found) {
            const std::string fmt = formatName(d.format);
            std::string err;
            Project p = makeDefaultProject();
            Sequence& s = *p.active();
            s.sampleRate = rate;
            FrameTime at = 0;
            for (const std::string& wav : wavs) {
                MediaItem m;
                m.id = p.newId();
                QVERIFY2(probeMedia(wav, m, &err), err.c_str());
                p.media.push_back(m);
                QVERIFY(edit::placeMedia(p, s, m.id, at, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
                at = s.audioTracks[0].clips.back().start + s.audioTracks[0].clips.back().duration + 25;
            }
            QCOMPARE(s.audioTracks[0].clips.size(), size_t(3));
            for (Clip& c : s.audioTracks[0].clips) {
                auto e = makePluginEffect(p, d, &err);
                QVERIFY2(e && e->type == "ara", (fmt + ": " + err).c_str());
                c.effects.push_back(*e);
            }
            // The sequence mixed with the plug-in on each clip, and without it.
            auto mixAll = [&](bool plugIn) {
                Sequence copy = s;
                for (Clip& c : copy.audioTracks[0].clips)
                    for (Effect& e : c.effects) e.enabled = plugIn;
                const int64_t n = int64_t(std::llround(double(at) / s.fpsValue() * rate));
                std::vector<float> out(size_t(n) * 2);
                AudioMixer mixer;
                mixer.mix(p, copy, 0, int(n), out.data());
                return out;
            };
            auto differ = [](const std::vector<float>& a, const std::vector<float>& b) {
                double worst = a.size() == b.size() ? 0 : 1e9;
                for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) worst = std::max(worst, double(std::fabs(a[i] - b[i])));
                return worst;
            };
            const int rendersBefore = ara::rendersDone();
            ara::logReads(true);
            const std::vector<float> through = mixAll(true);
            ara::logReads(false);
            QCOMPARE(ara::rendersDone(), rendersBefore + 3);  // each clip's source through the plug-in
            const std::vector<float> plain = mixAll(false);
            double peak = 0;
            for (float v : plain) peak = std::max(peak, double(std::fabs(v)));
            QVERIFY2(peak > 0.2, (fmt + ": the clips are heard").c_str());
            // Its playback is the clip, sample for sample.
            QVERIFY2(differ(through, plain) < 1e-6, (fmt + ": " + std::to_string(differ(through, plain))).c_str());
            // It read every sample of the three sources, each as the decoder gives it.
            const auto reads = ara::readsLogged();
            QCOMPARE(reads.size(), size_t(3));
            for (size_t k = 0; k < wavs.size(); ++k) {
                const std::string& wav = wavs[k];
                const AudioBufferPtr decoded = MediaPool::instance().audio(audioKey(wav, {}), rate);
                QVERIFY(decoded);
                const std::string id = ara::sourceIdFor(s.audioTracks[0].clips[k].effects[0], *decoded);
                const auto log = std::find_if(reads.begin(), reads.end(), [&](const auto& r) { return r.first == id; });
                QVERIFY2(log != reads.end(), (fmt + ": no reads of " + wav).c_str());
                QVERIFY(log->second.reads > 0 && log->second.channels == 2);
                QCOMPARE(log->second.seen.size(), decoded->samples.size());
                size_t wrong = 0;
                for (size_t i = 0; i < decoded->samples.size(); ++i) wrong += !(log->second.seen[i] == decoded->samples[i]);  // (NaN: unread)
                QVERIFY2(wrong == 0, (fmt + ": " + std::to_string(wrong) + " samples of " + wav + " unread or not as decoded").c_str());
            }
            // Slipped and trimmed, the clip still plays what it plays without the plug-in.
            Clip& middle = s.audioTracks[0].clips[1];
            middle.sourceIn += 7;
            middle.duration -= 12;
            QVERIFY2(differ(mixAll(true), mixAll(false)) < 1e-6, (fmt + ": after a slip and a trim").c_str());

            // Over MCP: listed with the audio effects, added by its type, and its analysis read back.
            const QString project = path(qPrintable(QStringLiteral("ara-%1.montage").arg(QString::fromStdString(fmt))));
            QVERIFY(saveProject(p, project.toStdString()));
            McpServer server;
            auto call = [&](const QString& tool, const QJsonObject& args) {
                const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
                const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
                return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
            };
            QJsonObject r = call("montage_list_effects", {{"kind", "audio"}});
            bool listed = false;
            for (const QJsonValue& v : r.value("structuredContent").toObject().value("effects").toArray())
                listed |= v.toObject().value("type").toString().toStdString() == pluginType(d) && v.toObject().value("ara").toBool();
            QVERIFY2(listed, fmt.c_str());
            const Id first = s.audioTracks[0].clips[0].id;
            r = call("montage_add_effect", {{"project", project}, {"clip", double(s.audioTracks[0].clips[2].id)},
                                            {"effect", QString::fromStdString(pluginType(d))}, {"params", QJsonObject{{"tempo_bpm", 96}}}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            r = call("montage_ara_content", {{"project", project}, {"clip", double(first)}});
            QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
            auto checkNotes = [&](const QJsonObject& result) {
                const QJsonObject o = result.value("structuredContent").toObject();
                QVERIFY(o.value("analysed").toBool());
                const QJsonArray notes = o.value("notes").toArray();
                QCOMPARE(notes.size(), qsizetype(bursts[0].size()));
                for (qsizetype i = 0; i < notes.size(); ++i) {
                    const QJsonObject n = notes[i].toObject();
                    const Burst& b = bursts[0][size_t(i)];
                    QVERIFY2(std::fabs(n.value("start").toDouble() - double(b.start) / rate) < 1e-9, qPrintable(QJsonDocument(n).toJson()));
                    QVERIFY(std::fabs(n.value("duration").toDouble() - double(b.length) / rate) < 1e-9);
                    QVERIFY(std::fabs(n.value("volume").toDouble() - 0.3) < 0.01);
                }
            };
            checkNotes(r);
            // Its archive was kept in the effect: reopened, the notes are there without analysing again.
            Project back;
            QVERIFY(loadProject(project.toStdString(), back));
            const Effect& kept = back.active()->audioTracks[0].clips[0].effects.at(0);
            QVERIFY(!kept.s("archive").empty() && kept.s("modification_id") == s.audioTracks[0].clips[0].effects[0].s("modification_id"));
            QCOMPARE(back.active()->audioTracks[0].clips[2].effects.at(1).type, std::string("ara"));
            QCOMPARE(back.active()->audioTracks[0].clips[2].effects.at(1).p("tempo_bpm", 0), 96.0);
            checkNotes(call("montage_ara_content", {{"project", project}, {"clip", double(first)}, {"analyse", false}}));
            // A document restored from the archive renders as before.
            s.audioTracks[0].clips[0].effects[0].strings["archive"] = kept.s("archive");
            const int rendersThen = ara::rendersDone();
            QVERIFY2(differ(mixAll(true), mixAll(false)) < 1e-6, (fmt + ": restored from its archive").c_str());
            QCOMPARE(ara::rendersDone(), rendersThen + 1);

            // An editor's document for a clip closes, ending its playback region, once the clip is deleted.
            QCOMPARE(ara::liveRegions(), regionsBefore);
            std::string key;
            AudioBufferPtr sound;
            const Clip& last = s.audioTracks[0].clips[2];
            QVERIFY(ara::clipSource(p, s, last, key, sound, &err));
            QVERIFY2(ara::liveSession(last.effects[0], key, sound, &err), err.c_str());
            QCOMPARE(ara::liveRegions(), regionsBefore + 1);
            ara::dropSessionsNotIn(p);
            QCOMPARE(ara::liveRegions(), regionsBefore + 1);  // (still in the project)
            // An undo that puts another archive in the effect closes the document it no longer matches.
            s.audioTracks[0].clips[2].effects[0].strings["archive"] = kept.s("archive");
            ara::dropSessionsNotIn(p);
            QCOMPARE(ara::liveRegions(), regionsBefore);
            s.audioTracks[0].clips[2].effects[0].strings.erase("archive");
            QVERIFY2(ara::liveSession(last.effects[0], key, sound, &err), err.c_str());
            QCOMPARE(ara::liveRegions(), regionsBefore + 1);
            QVERIFY(edit::removeClips(p, s, {last.id}, false).ok);
            ara::dropSessionsNotIn(p);
            QCOMPARE(ara::liveRegions(), regionsBefore);
        }
    }
};

QTEST_GUILESS_MAIN(TestPlugins)
#include "test_plugins.moc"
