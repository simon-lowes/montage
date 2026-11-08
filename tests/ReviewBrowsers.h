// Montage tests — what the live review tests in a browser need around Montage (test_render's browserReview*): a
// Firefox driven through geckodriver (W3C WebDriver), a UDP relay that holds what comes back to a bit rate, HTTPS
// requests from a chosen address of this computer, and the review itself playing a picture in real time.
#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "core/Model.h"
#include "media/Image.h"
#include "render/HeadlessBrowser.h"
#include "render/OGraf.h"
#include "render/ReferenceOutput.h"
#include "render/RemoteReview.h"
#include "render/WebRtcReview.h"

namespace montage::reviewtest {

inline int freeTcpPort() {
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return int(s.serverPort());
}

// A script for the review page that reads the frame code burnt into each picture shown (from a canvas) and keeps it
// beside the frame the page's tag names: __montage.checks, [{ code, tag }].
inline QString frameCheckScript() {
    return QStringLiteral(
        "(() => { const s = window.__montage; s.checks = []; const c = document.createElement('canvas'); let g = null;"
        "s.onFrame.push((now, meta, v, t) => { const w = v.videoWidth, h = v.videoHeight; if (!w || !h) return;"
        "if (c.width !== w || c.height !== h || !g) { c.width = w; c.height = h; g = c.getContext('2d', { willReadFrequently: true }); }"
        "g.drawImage(v, 0, 0); const b = Math.max(4, Math.floor(h / 30) & ~1), rw = Math.min(w, 26 * b);"
        "const px = g.getImageData(0, 0, rw, 2 * b).data; let code = 0;"
        "for (let bit = 0; bit < 24; ++bit) { const cx = (bit + 1) * b, cy = b; let sum = 0;"
        "for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) { const i = ((cy + dy) * rw + cx + dx) * 4;"
        "sum += (0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]) / 255; }"
        "sum /= 9; if (sum > 0.3 && sum < 0.7) { code = -1; break; } if (sum >= 0.7) code += 2 ** bit; }"
        "s.checks.push({ code, tag: t ? t.f : -1, timing: s.timing }); if (s.checks.length > 400) s.checks.shift(); }); return true; })()");
}

// A WHEP player of the test's own in the page (no data channel), kept as window[name]: `path` the endpoint ("/whep" or
// "/whep?hdr=1"); with `relayPort`, the offer goes without this browser's candidates and the answer's are moved to that
// port, so the media go through a relay there.
inline QString whepPlayerScript(const QString& name, const QString& passphrase, const QString& path, int montagePort, int relayPort) {
    return QStringLiteral(
               "(async () => { const pc = new RTCPeerConnection(); window.%1 = pc;"
               "pc.addTransceiver('video', { direction: 'recvonly' }); pc.addTransceiver('audio', { direction: 'recvonly' });"
               "pc.ontrack = (e) => { if (e.track.kind === 'video') window.%1Track = e.track; };"
               "await pc.setLocalDescription(await pc.createOffer());"
               "await new Promise(r => { if (pc.iceGatheringState === 'complete') r(); pc.onicegatheringstatechange = () => pc.iceGatheringState === 'complete' && r(); setTimeout(r, 2000); });"
               "let offer = pc.localDescription.sdp; const relay = %5;"
               "if (relay) offer = offer.split('\\r\\n').filter(l => !l.startsWith('a=candidate') && !l.startsWith('a=end-of-candidates')).join('\\r\\n');"
               "const w = await fetch('%3', { method: 'POST', headers: { Authorization: 'Bearer %2', 'Content-Type': 'application/sdp' }, body: offer });"
               "if (w.status !== 201) return 'refused ' + w.status; window.%1At = w.headers.get('Location');"
               "let answer = await w.text();"
               "if (relay) answer = answer.split(' %4 typ host').join(' ' + relay + ' typ host');"
               "await pc.setRemoteDescription({ type: 'answer', sdp: answer }); return 'ok'; })()")
        .arg(name, passphrase, path)
        .arg(montagePort)
        .arg(relayPort);
}

// The size of the last picture a peer connection (window[name]) decoded: "640x360", or "" before one.
inline QString decodedSizeScript(const QString& pc) {
    return QStringLiteral(
               "%1.getStats().then(r => { let s = ''; r.forEach(x => { if (x.type === 'inbound-rtp' && x.kind === 'video' && x.frameWidth) s = x.frameWidth + 'x' + x.frameHeight; }); return s; })")
        .arg(pc);
}

// A UDP relay on this computer for one client: what it sends goes on to `to`, and what comes back is held to `kbps`
// (0: passed at once) through a queue of a tenth of a second, the rest dropped, as a slow link's router does.
class ThrottledRelay {
public:
    std::atomic<int> kbps{0};
    std::atomic<int64_t> dropped{0};

    ~ThrottledRelay() { stop(); }
    bool start(quint16 to) {
        std::atomic<int> ready{0};
        thread_ = std::thread([this, to, &ready] { run(to, ready); });
        while (ready == 0) QThread::msleep(5);
        return ready > 0;
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    quint16 port() const { return port_; }

private:
    void run(quint16 to, std::atomic<int>& ready) {
        QUdpSocket front, back;
        front.setProxy(QNetworkProxy::NoProxy);
        back.setProxy(QNetworkProxy::NoProxy);
        if (!front.bind(QHostAddress::LocalHost, 0) || !back.bind(QHostAddress::LocalHost, 0)) {
            ready = -1;
            return;
        }
        port_ = front.localPort();
        ready = 1;
        QHostAddress client;
        quint16 clientPort = 0;
        std::deque<QByteArray> queue;
        qint64 queued = 0;
        double tokens = 0;
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0;
        while (!stop_) {
            front.waitForReadyRead(1);
            while (front.hasPendingDatagrams()) {
                QByteArray d(int(front.pendingDatagramSize()), 0);
                QHostAddress from;
                quint16 fromPort = 0;
                front.readDatagram(d.data(), d.size(), &from, &fromPort);
                client = from, clientPort = fromPort;
                back.writeDatagram(d, QHostAddress::LocalHost, to);
            }
            back.waitForReadyRead(1);
            const int rate = kbps;
            while (back.hasPendingDatagrams()) {
                QByteArray d(int(back.pendingDatagramSize()), 0);
                back.readDatagram(d.data(), d.size());
                if (!clientPort) continue;
                if (rate <= 0) {
                    front.writeDatagram(d, client, clientPort);
                } else if (queued + d.size() <= qint64(rate) * 125 / 10) {
                    queued += d.size();
                    queue.push_back(std::move(d));
                } else {
                    ++dropped;
                }
            }
            const qint64 now = clock.nsecsElapsed();
            if (rate > 0) tokens = std::min(6000.0, tokens + double(now - last) * 1e-9 * rate * 125);
            last = now;
            while (!queue.empty() && (rate <= 0 || tokens >= queue.front().size())) {
                if (rate > 0) tokens -= queue.front().size();
                queued -= queue.front().size();
                front.writeDatagram(queue.front(), client, clientPort);
                queue.pop_front();
            }
        }
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<quint16> port_{0};
};

// An HTTPS GET to Montage on this computer from one of its addresses (`from`: "127.0.0.2"; on Linux every 127.x
// address is this computer's), the passphrase as the bearer token: the response's status, or 0 for none in time.
inline int httpsStatus(const QString& from, int port, const QByteArray& path, const QByteArray& bearer, int ms = 10000) {
    QSslSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.setPeerVerifyMode(QSslSocket::VerifyNone);
    if (!from.isEmpty() && !s.bind(QHostAddress(from))) return -1;
    s.connectToHostEncrypted(QStringLiteral("127.0.0.1"), quint16(port));
    if (!s.waitForEncrypted(ms)) return 0;
    s.write("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + bearer + "\r\nConnection: close\r\n\r\n");
    QByteArray in;
    QDeadlineTimer deadline(ms);
    while (!in.contains("\r\n") && !deadline.hasExpired() && s.state() == QAbstractSocket::ConnectedState)
        if (s.waitForReadyRead(int(std::max<qint64>(1, deadline.remainingTime())))) in += s.readAll();
    in += s.readAll();
    const QList<QByteArray> first = in.left(in.indexOf("\r\n")).split(' ');
    return first.size() >= 2 ? first[1].toInt() : 0;
}

// Firefox (MONTAGE_FIREFOX, else firefox on the PATH) headless, through geckodriver (MONTAGE_GECKODRIVER, the runner
// images' $GECKOWEBDRIVER, else geckodriver on the PATH): a session of its own that takes this computer's own
// certificate, plays sound without a gesture, and makes WebRTC candidates on loopback.
class GeckoBrowser {
public:
    ~GeckoBrowser() {
        if (!session_.isEmpty()) request("DELETE", "/session/" + session_.toUtf8(), {}, nullptr, nullptr, 15000);
        if (driver_.state() != QProcess::NotRunning) {
            driver_.terminate();
            if (!driver_.waitForFinished(5000)) driver_.kill();
            driver_.waitForFinished(3000);
        }
    }

    static std::string firefox() {
        const QByteArray env = qgetenv("MONTAGE_FIREFOX");
        if (!env.isEmpty()) return env.toStdString();
        return QStandardPaths::findExecutable(QStringLiteral("firefox")).toStdString();
    }
    static std::string geckodriver() {
        QByteArray env = qgetenv("MONTAGE_GECKODRIVER");
        if (!env.isEmpty()) return env.toStdString();
        env = qgetenv("GECKOWEBDRIVER");
        if (!env.isEmpty()) {
            for (const char* name : {"/geckodriver", "/geckodriver.exe"})
                if (QFile::exists(QString::fromLocal8Bit(env) + name)) return (QString::fromLocal8Bit(env) + name).toStdString();
        }
        return QStandardPaths::findExecutable(QStringLiteral("geckodriver")).toStdString();
    }

    bool start(std::string* error) {
        const std::string exe = geckodriver(), fx = firefox();
        if (exe.empty() || fx.empty()) return fail("No Firefox and geckodriver", error);
        port_ = freeTcpPort();
        driver_.setProcessChannelMode(QProcess::MergedChannels);
        driver_.setStandardOutputFile(logDir_.path() + "/geckodriver.log");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("MOZ_HEADLESS"), QStringLiteral("1"));
        driver_.setProcessEnvironment(env);
        driver_.start(QString::fromStdString(exe), {QStringLiteral("--port"), QString::number(port_), QStringLiteral("--host"), QStringLiteral("127.0.0.1")});
        if (!driver_.waitForStarted(15000)) return fail("Cannot start geckodriver (" + exe + ")", error);
        QElapsedTimer waited;
        waited.start();
        QJsonObject status;
        while (!request("GET", "/status", {}, &status, nullptr, 2000)) {
            if (waited.elapsed() > 30000 || driver_.state() == QProcess::NotRunning)
                return fail("geckodriver did not answer: " + log().right(800).toStdString(), error);
            QThread::msleep(100);
        }
        const QJsonObject prefs{{"media.peerconnection.ice.loopback", true},
                                {"media.peerconnection.ice.obfuscate_host_addresses", false},
                                {"media.peerconnection.ice.default_address_only", false},
                                {"media.autoplay.default", 0},
                                {"media.autoplay.blocking_policy", 0},
                                {"media.navigator.permission.disabled", true},
                                {"network.proxy.type", 0}};
        const QJsonObject options{{"binary", QString::fromStdString(fx)},
                                  {"args", QJsonArray{"-headless", "-width", "800", "-height", "600"}},
                                  {"prefs", prefs}};
        const QJsonObject caps{{"capabilities", QJsonObject{{"alwaysMatch", QJsonObject{{"browserName", "firefox"},
                                                                                       {"acceptInsecureCerts", true},
                                                                                       {"moz:firefoxOptions", options}}}}}};
        QJsonObject r;
        if (!request("POST", "/session", caps, &r, error, 120000)) return false;
        session_ = r.value("value").toObject().value("sessionId").toString();
        if (session_.isEmpty()) return fail("No session: " + QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString(), error);
        version_ = r.value("value").toObject().value("capabilities").toObject().value("browserVersion").toString();
        request("POST", path("/timeouts"), {{"script", 90000}, {"pageLoad", 60000}}, nullptr, nullptr, 15000);
        return true;
    }

    QString version() const { return version_; }

    bool navigate(const QString& url, std::string* error) { return request("POST", path("/url"), {{"url", url}}, nullptr, error, 90000); }

    // An expression's value (a promise's, once settled); null with a warning when it throws.
    QJsonValue js(const QString& expression, int ms = 30000) {
        QJsonObject r;
        std::string why;
        if (!request("POST", path("/execute/sync"), {{"script", "return (" + expression + ");"}, {"args", QJsonArray{}}}, &r, &why, ms + 5000)) {
            qWarning("%s", why.c_str());
            return {};
        }
        return r.value("value");
    }

    QByteArray log() const {
        QFile f(logDir_.path() + "/geckodriver.log");
        return f.open(QIODevice::ReadOnly) ? f.readAll().right(16000) : QByteArray();
    }

private:
    static bool fail(const std::string& why, std::string* error) {
        if (error) *error = why;
        return false;
    }
    QByteArray path(const char* rest) const { return "/session/" + session_.toUtf8() + rest; }

    // One request on a connection of its own; false and why for no answer or a WebDriver error.
    bool request(const QByteArray& method, const QByteArray& where, const QJsonObject& body, QJsonObject* out, std::string* error, int ms) {
        QTcpSocket s;
        s.setProxy(QNetworkProxy::NoProxy);
        s.connectToHost(QStringLiteral("127.0.0.1"), quint16(port_));
        if (!s.waitForConnected(std::min(ms, 5000))) return fail("Cannot reach geckodriver", error);
        const QByteArray payload = method == "GET" || method == "DELETE" ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
        s.write(method + " " + where + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port_) +
                "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + QByteArray::number(payload.size()) +
                "\r\nConnection: close\r\n\r\n" + payload);
        QByteArray in;
        QDeadlineTimer deadline(ms);
        qsizetype end = -1, length = -1;
        while (!deadline.hasExpired()) {
            if (end < 0 && (end = in.indexOf("\r\n\r\n")) >= 0) {
                for (const QByteArray& line : in.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
            }
            if (end >= 0 && length >= 0 && in.size() >= end + 4 + length) break;
            if (s.state() != QAbstractSocket::ConnectedState && !s.bytesAvailable()) break;
            if (s.waitForReadyRead(int(std::clamp<qint64>(deadline.remainingTime(), 1, 1000)))) in += s.readAll();
        }
        in += s.readAll();
        if (end < 0) return fail("geckodriver did not answer " + where.toStdString(), error);
        const QJsonObject r = QJsonDocument::fromJson(in.mid(end + 4, length >= 0 ? length : -1)).object();
        const QJsonObject value = r.value("value").toObject();
        if (value.contains("error")) return fail(value.value("error").toString().toStdString() + ": " + value.value("message").toString().toStdString(), error);
        if (out) *out = r;
        return in.startsWith("HTTP/1.1 2") || fail("geckodriver: " + in.left(in.indexOf("\r\n")).toStdString(), error);
    }

    QProcess driver_;
    QTemporaryDir logDir_;
    int port_ = 0;
    QString session_, version_;
};

// The review playing a sequence of the size and colour space asked, each frame `picture` makes, in real time by a
// player of its own (parked on `playhead`, or playing from it), with a 1 kHz tone.
struct ReviewRig {
    Project project = makeDefaultProject();
    Sequence* sequence = nullptr;
    std::shared_ptr<output::BrowserReviewBackend> review = std::make_shared<output::BrowserReviewBackend>();
    output::StreamSettings settings;
    std::unique_ptr<output::Scheduler> scheduler;
    std::function<std::shared_ptr<const Image>(FrameTime)> picture;
    std::atomic<bool> playing{false};
    std::atomic<int64_t> playhead{100};

    ReviewRig() {
        settings.address = "https://127.0.0.1:" + std::to_string(freeTcpPort());
        settings.passphrase = output::generatePassphrase();
        settings.hardware = false;
        settings.bitrateKbps = 3000;
        settings.frameCode = true;
    }
    ~ReviewRig() { stop(); }

    int port() const { return std::stoi(settings.address.substr(settings.address.rfind(':') + 1)); }
    QString page() const { return QStringLiteral("https://127.0.0.1:%1/").arg(port()); }
    QString passphrase() const { return QString::fromStdString(settings.passphrase); }

    bool start(int width, int height, const std::string& colorSpace, std::string* why) {
        Sequence& s = *project.active();
        sequence = &s;
        s.width = width, s.height = height, s.fps = {25, 1}, s.sampleRate = 48000, s.colorSpace = colorSpace;
        review->setSettings(settings);
        review->setSequence(s.id);
        const output::Mode mode{"browser", width, height, {25, 1}, false, 0};
        output::Config cfg;
        cfg.depth = 10;
        cfg.audioChannels = 2;
        // (the HDR picture itself for HDR sequences, as the panel sends it with HDR to browsers on)
        cfg.rec709 = !(settings.hdr && (colorSpace == "rec2100pq" || colorSpace == "rec2100hlg"));
        scheduler = std::make_unique<output::Scheduler>(review);
        if (!scheduler->start(review->devices()[0], mode, s, cfg, why)) return false;
        scheduler->setAudioSource([](int64_t start, int frames, int channels, float* out) {
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c) out[size_t(i) * size_t(channels) + size_t(c)] = 0.25f * float(std::sin(2 * M_PI * 1000 * double(start + i) / 48000));
        });
        player_ = std::thread([this] {
            QElapsedTimer clock;
            FrameTime from = 0;
            bool was = false;
            while (!stop_) {
                if (playing && !was) {
                    clock.restart();
                    from = playhead;
                }
                was = playing;
                const FrameTime t = playing ? from + clock.elapsed() * 25 / 1000 : FrameTime(playhead);
                playhead = t;
                scheduler->setPosition(t, playing ? 1 : 0);
                for (FrameTime f = t; f < t + 4; ++f)
                    if (scheduler->wants(f)) scheduler->submit(f, picture(f));
                QThread::msleep(5);
            }
        });
        return true;
    }
    void stop() {
        stop_ = true;
        if (player_.joinable()) player_.join();
        if (scheduler) scheduler->stop();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread player_;
};

// A headless Chrome, Chromium or Edge (MONTAGE_CHROMIUM, or one installed) on a page: as a reviewer's, the certificate
// taken as it is, sound allowed.
class ChromeTab {
public:
    static std::string exe() { return ografBrowser(); }
    bool start(std::string* why, const QStringList& more = {}) {
        bool sandbox = true;
#ifndef _WIN32
        sandbox = geteuid() != 0;
#endif
        HeadlessBrowser::Options options;
        options.offline = false;
        options.extraArgs = QStringList{"--ignore-certificate-errors", "--autoplay-policy=no-user-gesture-required",
                                        "--disable-features=WebRtcHideLocalIpsWithMdns"} + more;
        browser_ = std::make_unique<HeadlessBrowser>();
        if (browser_->start(exe(), 800, 600, sandbox, why, options)) return true;
        if (!sandbox) return false;
        browser_ = std::make_unique<HeadlessBrowser>();
        return browser_->start(exe(), 800, 600, false, why, options);
    }
    bool open(const QString& url, std::string* why) {
        QJsonObject r;
        if (!browser_->call("Page.navigate", {{"url", url}}, &r, why)) return false;
        if (until("document.readyState === 'complete' && !!window.__montage", 20)) return true;
        if (why) *why = "The review page did not load: " + browser_->log().right(1500).toStdString();
        return false;
    }
    QJsonValue js(const QString& expression, int ms = 30000) {
        QJsonObject r;
        std::string err;
        if (!browser_->call("Runtime.evaluate", {{"expression", expression}, {"awaitPromise", true}, {"returnByValue", true}}, &r, &err, true, ms)) {
            qWarning("%s", err.c_str());
            return QJsonValue();
        }
        if (r.contains("exceptionDetails")) qWarning("%s", QJsonDocument(r.value("exceptionDetails").toObject()).toJson().constData());
        return r.value("result").toObject().value("value");
    }
    bool until(const QString& expression, int seconds) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < seconds * 1000) {
            if (js(expression).toBool()) return true;
            QThread::msleep(50);
        }
        return false;
    }
    QByteArray log() const { return browser_ ? browser_->log() : QByteArray(); }

private:
    std::unique_ptr<HeadlessBrowser> browser_;
};

}  // namespace montage::reviewtest
