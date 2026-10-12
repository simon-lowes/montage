#include "OGraf.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
QString number(double v) { return QString::number(v, 'g', 15); }

QJsonObject parseObject(const std::string& json) {
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    return d.isObject() ? d.object() : QJsonObject{};
}

QString compact(const QJsonValue& v) {
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    // A bare value: wrapped, then unwrapped.
    const QString a = QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
    return a.mid(1, a.size() - 2);
}

// ---- The browser, over the Chrome DevTools Protocol ------------------------------------------------------------

// A headless browser of its own (a fresh profile, no network but what the page's requests are answered with),
// driven over its DevTools WebSocket: one page, its session attached.
class Browser {
public:
    ~Browser() {
        if (sock_.state() == QAbstractSocket::ConnectedState) {
            post("Browser.close", {}, false);
            sock_.flush();
            sock_.waitForBytesWritten(1000);
            sock_.close();
        }
        if (proc_.state() != QProcess::NotRunning && !proc_.waitForFinished(3000)) {
            proc_.kill();
            proc_.waitForFinished(3000);
        }
    }

    std::function<void(const QString& method, const QJsonObject& params)> onEvent;
    const std::atomic<bool>* cancel = nullptr;  // stops waiting for the browser

    bool start(const std::string& exe, int width, int height, bool sandbox, std::string* error) {
        if (!profile_.isValid()) return fail("Cannot make a browser profile folder", error);
        // Offline: every request the page makes is answered by the renderer (Fetch), and anything that escapes that
        // (a WebSocket, WebRTC) goes to a proxy that is not there, local addresses included.
        QStringList args{"--headless",
                         "--user-data-dir=" + profile_.path(),
                         "--remote-debugging-port=0",
                         "--remote-debugging-address=127.0.0.1",
                         "--no-first-run",
                         "--no-default-browser-check",
                         "--disable-background-networking",
                         "--disable-component-update",
                         "--disable-default-apps",
                         "--disable-extensions",
                         "--disable-sync",
                         "--disable-features=Translate,MediaRouter,OptimizationHints",
                         "--proxy-server=http://127.0.0.1:9",
                         "--proxy-bypass-list=<-loopback>",
                         "--force-webrtc-ip-handling-policy=disable_non_proxied_udp",
                         "--hide-scrollbars",
                         "--mute-audio",
                         "--force-color-profile=srgb",
                         "--enable-unsafe-swiftshader",
                         QStringLiteral("--window-size=%1,%2").arg(width).arg(height),
                         "about:blank"};
        if (!sandbox) args.insert(1, "--no-sandbox");
        // What it says goes to a file (a pipe nobody reads while it renders would fill and stop it).
        proc_.setProcessChannelMode(QProcess::MergedChannels);
        proc_.setStandardOutputFile(logFile_.path() + "/browser.log");
        proc_.start(qs(exe), args);
        if (!proc_.waitForStarted(15000)) return fail("Cannot start the browser (" + exe + ")", error);
        // Once it listens it says where ("DevTools listening on ws://127.0.0.1:port/path") and writes the same to
        // DevToolsActivePort in its profile (which a snap's own /tmp hides).
        const QString portFile = profile_.path() + "/DevToolsActivePort";
        static const QRegularExpression listening(QStringLiteral("DevTools listening on ws://[^:/\\s]+:(\\d+)(/\\S+)"));
        QDeadlineTimer wait(30000);
        int port = 0;
        QString path;
        while (!wait.hasExpired()) {
            if (cancel && cancel->load()) return fail("Cancelled", error);
            QFile f(portFile);
            if (f.open(QIODevice::ReadOnly)) {
                const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
                if (lines.size() >= 2 && !lines[1].trimmed().isEmpty()) {
                    port = lines[0].trimmed().toInt();
                    path = lines[1].trimmed();
                    break;
                }
            }
            const QRegularExpressionMatch m = listening.match(QString::fromUtf8(log()));
            if (m.hasMatch()) {
                port = m.captured(1).toInt();
                path = m.captured(2);
                break;
            }
            if (proc_.waitForFinished(30) || proc_.state() == QProcess::NotRunning)
                return fail("The browser stopped at once: " + QString::fromUtf8(log().right(400)).trimmed().toStdString(), error);
        }
        if (port <= 0) return fail("The browser did not open its DevTools port", error);
        sock_.setProxy(QNetworkProxy::NoProxy);
        sock_.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
        if (!sock_.waitForConnected(10000)) return fail("Cannot reach the browser's DevTools port", error);
        QByteArray key(16, 0);
        for (char& c : key) c = char(QRandomGenerator::global()->bounded(256));
        sock_.write("GET " + path.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key.toBase64() +
                    "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        QDeadlineTimer hs(10000);
        int end = -1;
        while ((end = int(in_.indexOf("\r\n\r\n"))) < 0) {
            if (hs.hasExpired() || !sock_.waitForReadyRead(int(std::max<qint64>(1, hs.remainingTime()))))
                return fail("The browser's DevTools did not answer", error);
            in_ += sock_.readAll();
        }
        if (!in_.startsWith("HTTP/1.1 101")) return fail("The browser's DevTools refused the connection", error);
        in_.remove(0, end + 4);
        // A page of the size asked, its session attached.
        QJsonObject r;
        if (!call("Target.createTarget", {{"url", "about:blank"}}, &r, error, false)) return false;
        const QString target = r.value("targetId").toString();
        if (!call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, &r, error, false)) return false;
        session_ = r.value("sessionId").toString();
        return !session_.isEmpty() || fail("The browser gave no page", error);
    }

    // Sends a command and waits for its answer, handling events meanwhile.
    bool call(const QString& method, const QJsonObject& params, QJsonObject* result, std::string* error, bool onPage = true,
              int ms = 120000) {
        const int id = post(method, params, onPage);
        QDeadlineTimer deadline(ms);
        for (;;) {
            QJsonObject m;
            if (!read(m, deadline, error)) {
                if (error && cancel && cancel->load()) *error = "Cancelled";
                else if (error && error->empty()) *error = "The browser did not answer " + method.toStdString();
                return false;
            }
            if (m.contains("id")) {
                if (m.value("id").toInt() != id) continue;
                if (m.contains("error")) {
                    if (error) *error = m.value("error").toObject().value("message").toString().toStdString();
                    return false;
                }
                if (result) *result = m.value("result").toObject();
                return true;
            }
            dispatch(m);
        }
    }

    // Reads and handles events until `done` says so.
    bool until(const std::function<bool()>& done, int ms, std::string* error) {
        QDeadlineTimer deadline(ms);
        while (!done()) {
            QJsonObject m;
            if (!read(m, deadline, error)) return false;
            if (!m.contains("id")) dispatch(m);
        }
        return true;
    }

    int post(const QString& method, const QJsonObject& params, bool onPage = true) {
        QJsonObject m{{"id", ++next_}, {"method", method}, {"params", params}};
        if (onPage && !session_.isEmpty()) m["sessionId"] = session_;
        sendFrame(0x1, QJsonDocument(m).toJson(QJsonDocument::Compact));
        return next_;
    }

    // What it has said (the end of it).
    QByteArray log() const {
        QFile f(logFile_.path() + "/browser.log");
        if (!f.open(QIODevice::ReadOnly)) return {};
        if (f.size() > 16000) f.seek(f.size() - 16000);
        return f.readAll();
    }

private:
    static bool fail(const std::string& message, std::string* error) {
        if (error) *error = message;
        return false;
    }

    void dispatch(const QJsonObject& m) {
        if (onEvent && m.contains("method")) onEvent(m.value("method").toString(), m.value("params").toObject());
    }

    void sendFrame(int opcode, const QByteArray& payload) {
        QByteArray f;
        f.append(char(0x80 | opcode));
        const qint64 n = payload.size();
        if (n < 126) {
            f.append(char(0x80 | n));
        } else if (n < 65536) {
            f.append(char(0x80 | 126));
            f.append(char((n >> 8) & 0xff));
            f.append(char(n & 0xff));
        } else {
            f.append(char(0x80 | 127));
            for (int i = 7; i >= 0; --i) f.append(char((n >> (8 * i)) & 0xff));
        }
        char mask[4];
        for (char& c : mask) c = char(QRandomGenerator::global()->bounded(256));
        f.append(mask, 4);
        const qsizetype at = f.size();
        f.append(payload);
        for (qint64 i = 0; i < n; ++i) f[at + i] = char(f[at + i] ^ mask[i % 4]);
        sock_.write(f);
        sock_.flush();
    }

    bool fill(qint64 n, const QDeadlineTimer& deadline) {
        while (in_.size() < n) {
            if (sock_.bytesAvailable() > 0) {
                in_ += sock_.readAll();
                continue;
            }
            if (deadline.hasExpired() || sock_.state() != QAbstractSocket::ConnectedState || (cancel && cancel->load())) return false;
            sock_.waitForReadyRead(int(std::clamp<qint64>(deadline.remainingTime(), 1, 1000)));
        }
        return true;
    }

    // The next whole message (a text frame and its continuations; pings answered on the way).
    bool read(QJsonObject& out, const QDeadlineTimer& deadline, std::string* error) {
        QByteArray message;
        for (;;) {
            if (!fill(2, deadline)) return false;
            const int b0 = uchar(in_[0]), b1 = uchar(in_[1]);
            const int opcode = b0 & 0x0f;
            const bool fin = b0 & 0x80, masked = b1 & 0x80;
            qint64 len = b1 & 0x7f, at = 2;
            if (len == 126) {
                if (!fill(4, deadline)) return false;
                len = (qint64(uchar(in_[2])) << 8) | uchar(in_[3]);
                at = 4;
            } else if (len == 127) {
                if (!fill(10, deadline)) return false;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | uchar(in_[2 + i]);
                at = 10;
            }
            const qint64 maskAt = at;
            if (masked) at += 4;
            if (!fill(at + len, deadline)) return false;
            QByteArray payload = in_.mid(at, len);
            if (masked)
                for (qint64 i = 0; i < len; ++i) payload[i] = char(payload[i] ^ in_[maskAt + i % 4]);
            in_.remove(0, at + len);
            if (opcode == 0x8) {
                if (error) *error = "The browser closed the connection";
                return false;
            }
            if (opcode == 0x9) {
                sendFrame(0xA, payload);
                continue;
            }
            if (opcode == 0xA) continue;
            message += payload;
            if (!fin) continue;
            out = QJsonDocument::fromJson(message).object();
            return true;
        }
    }

    QProcess proc_;
    QTemporaryDir profile_, logFile_;
    QTcpSocket sock_;
    QByteArray in_;
    int next_ = 0;
    QString session_;
};

// Installed before the page's own scripts. Until __ografStart() time is the real one; after, it stands still but for
// __ografAdvance(ms), which runs the timers due in order (each followed by its promise jobs), then the animation
// frames, and puts every CSS transition, CSS animation and Web Animation (in shadow trees too) at its time.
const char* const kClock = R"js((() => {
  const real = { now: performance.now.bind(performance), dateNow: Date.now, raf: window.requestAnimationFrame.bind(window),
    caf: window.cancelAnimationFrame.bind(window), st: window.setTimeout.bind(window), si: window.setInterval.bind(window),
    ct: window.clearTimeout.bind(window), ci: window.clearInterval.bind(window) };
  let on = false, now = 0, base = 0, seq = 1e9;
  const timers = new Map(), seen = new WeakMap();
  let frames = new Map();
  performance.now = () => on ? now : real.now();
  Date.now = () => on ? base + now : real.dateNow();
  window.requestAnimationFrame = cb => { if (!on) return real.raf(cb); const id = ++seq; frames.set(id, cb); return id; };
  window.cancelAnimationFrame = id => { frames.delete(id); real.caf(id); };
  window.setTimeout = (cb, ms, ...a) => { if (!on) return real.st(cb, ms, ...a); const id = ++seq;
    timers.set(id, { at: now + Math.max(0, +ms || 0), cb, a, every: 0 }); return id; };
  window.setInterval = (cb, ms, ...a) => { if (!on) return real.si(cb, ms, ...a); const id = ++seq; const every = Math.max(1, +ms || 0);
    timers.set(id, { at: now + every, cb, a, every }); return id; };
  window.clearTimeout = id => { timers.delete(id); real.ct(id); };
  window.clearInterval = id => { timers.delete(id); real.ci(id); };
  const call = (cb, a) => { try { typeof cb === 'function' ? cb(...a) : (0, eval)(String(cb)); } catch (e) { console.error(e); } };
  const animations = () => {
    const all = document.getAnimations();
    const walk = root => { for (const el of root.querySelectorAll('*')) if (el.shadowRoot) { all.push(...el.shadowRoot.getAnimations()); walk(el.shadowRoot); } };
    walk(document);
    return [...new Set(all)];
  };
  window.__ografStart = () => { base = real.dateNow(); now = 0; on = true;
    for (const a of animations()) { seen.set(a, -(a.currentTime || 0)); a.pause(); } };
  window.__ografAdvance = async t => {
    for (;;) {
      let next = null;
      for (const [id, x] of timers) if (x.at <= t && (!next || x.at < next[1].at || (x.at === next[1].at && id < next[0]))) next = [id, x];
      if (!next) break;
      const [id, x] = next;
      now = x.at;
      if (x.every) x.at += x.every; else timers.delete(id);
      call(x.cb, x.a);
      await null;
    }
    now = t;
    const due = frames;
    frames = new Map();
    for (const cb of due.values()) call(cb, [now]);
    await null;
    for (const a of animations()) { if (!seen.has(a)) seen.set(a, now); a.pause(); a.currentTime = Math.max(0, now - seen.get(a)); }
  };
})();)js";

QString mimeType(const QString& file) {
    const QString e = QFileInfo(file).suffix().toLower();
    if (e == "js" || e == "mjs") return "text/javascript";
    if (e == "json") return "application/json";
    if (e == "html" || e == "htm") return "text/html";
    if (e == "css") return "text/css";
    if (e == "svg") return "image/svg+xml";
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "gif") return "image/gif";
    if (e == "webp") return "image/webp";
    if (e == "woff2") return "font/woff2";
    if (e == "woff") return "font/woff";
    if (e == "ttf") return "font/ttf";
    if (e == "otf") return "font/otf";
    if (e == "mp4") return "video/mp4";
    if (e == "webm") return "video/webm";
    if (e == "wasm") return "application/wasm";
    return "application/octet-stream";
}

const char* const kHost = "ograf.localhost";

}  // namespace

bool isOGrafManifest(const std::string& path) { return qs(path).toLower().endsWith(".ograf.json"); }

bool readOGraf(const std::string& manifest, OGrafGraphic& out, std::string* error) {
    QFile f(qs(manifest));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + manifest;
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!d.isObject()) {
        if (error) *error = "Not an OGraf manifest (" + pe.errorString().toStdString() + ")";
        return false;
    }
    const QJsonObject o = d.object();
    OGrafGraphic g;
    g.manifest = QFileInfo(qs(manifest)).absoluteFilePath().toStdString();
    g.main = o.value("main").toString().toStdString();
    if (g.main.empty()) {
        if (error) *error = "The OGraf manifest names no main module";
        return false;
    }
    g.id = o.value("id").toString().toStdString();
    g.name = o.value("name").toString(QFileInfo(qs(manifest)).fileName().section('.', 0, 0)).toStdString();
    g.description = o.value("description").toString().toStdString();
    g.version = o.value("version").toVariant().toString().toStdString();
    g.realTime = o.value("supportsRealTime").toBool(true);
    g.nonRealTime = o.value("supportsNonRealTime").toBool(false);
    g.stepCount = o.value("stepCount").toInt(1);
    const QJsonObject schema = o.value("schema").toObject();
    g.schema = compact(schema).toStdString();
    const QJsonObject props = schema.value("properties").toObject();
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QJsonObject p = it.value().toObject();
        OGrafField fd;
        fd.key = it.key().toStdString();
        fd.title = p.value("title").toString(it.key()).toStdString();
        fd.description = p.value("description").toString().toStdString();
        QString type = p.value("type").toString();
        if (p.value("type").isArray())
            for (const QJsonValue& v : p.value("type").toArray())
                if (v.toString() != "null") {
                    type = v.toString();
                    break;
                }
        fd.type = type == "string" || type == "number" || type == "integer" || type == "boolean" ? type.toStdString() : "json";
        for (const QJsonValue& v : p.value("enum").toArray())
            if (v.isString()) fd.choices.push_back(v.toString().toStdString());
        fd.multiline = p.value("gddType").toString() == "multi-line";
        g.fields.push_back(fd);
    }
    for (const QJsonValue& v : o.value("customActions").toArray()) {
        const QJsonObject a = v.toObject();
        if (!a.value("id").toString().isEmpty())
            g.customActions.push_back({a.value("id").toString().toStdString(), a.value("name").toString(a.value("id").toString()).toStdString()});
    }
    out = std::move(g);
    return true;
}

std::string ografDefaultData(const OGrafGraphic& g) {
    const QJsonObject props = parseObject(g.schema).value("properties").toObject();
    QJsonObject data;
    for (auto it = props.begin(); it != props.end(); ++it)
        if (it.value().toObject().contains("default")) data[it.key()] = it.value().toObject().value("default");
    return compact(data).toStdString();
}

std::string ografBrowser() {
    const QString env = qEnvironmentVariable("MONTAGE_CHROMIUM");
    if (!env.isEmpty()) return QFileInfo(env).isExecutable() ? env.toStdString() : std::string();
    QStringList candidates;
#if defined(Q_OS_MACOS)
    for (const char* app : {"Google Chrome.app/Contents/MacOS/Google Chrome", "Chromium.app/Contents/MacOS/Chromium",
                            "Microsoft Edge.app/Contents/MacOS/Microsoft Edge", "Brave Browser.app/Contents/MacOS/Brave Browser"}) {
        candidates << QStringLiteral("/Applications/") + app << QDir::homePath() + "/Applications/" + app;
    }
#elif defined(Q_OS_WIN)
    for (const char* root : {"PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"}) {
        const QString r = qEnvironmentVariable(root);
        if (r.isEmpty()) continue;
        candidates << r + "/Google/Chrome/Application/chrome.exe" << r + "/Microsoft/Edge/Application/msedge.exe"
                   << r + "/Chromium/Application/chrome.exe";
    }
#endif
    for (const QString& c : candidates)
        if (QFileInfo(c).isExecutable()) return c.toStdString();
    for (const char* name : {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "microsoft-edge",
                             "microsoft-edge-stable", "chrome", "msedge"}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) return found.toStdString();
    }
    return {};
}

bool renderOGraf(const OGrafJob& job, const std::string& folder, const OGrafProgress& progress, const std::atomic<bool>* cancel,
                 std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    OGrafGraphic g;
    if (!readOGraf(job.manifest, g, error)) return false;
    if (job.frames <= 0 || job.width < 1 || job.height < 1 || !(job.fps > 0)) return fail("Nothing to render");
    const std::string exe = ografBrowser();
    if (exe.empty()) return fail("OGraf graphics need Chrome, Chromium or Edge installed (or MONTAGE_CHROMIUM set to one)");
    if (!QDir().mkpath(qs(folder))) return fail("Cannot write " + folder);
    const QDir root = QFileInfo(qs(g.manifest)).absoluteDir();
    const QString rootPath = QFileInfo(root.absolutePath()).canonicalFilePath();

    // Sandboxed, as browsing is, unless it cannot be: Chrome refuses root (containers), and some systems forbid the
    // namespaces its sandbox needs. The page is offline either way.
    bool sandbox = true;
#ifndef _WIN32
    sandbox = geteuid() != 0;
#endif
    auto browser = std::make_unique<Browser>();
    browser->cancel = cancel;
    if (!browser->start(exe, job.width, job.height, sandbox, error)) {
        if (!sandbox || !browser->log().toLower().contains("sandbox")) return false;
        browser = std::make_unique<Browser>();
        browser->cancel = cancel;
        if (!browser->start(exe, job.width, job.height, false, error)) return false;
    }
    Browser& b = *browser;
    std::string pageError;  // the graphic's own: an uncaught exception
    const QByteArray harness =
        QStringLiteral(
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>html,body{margin:0;padding:0;width:100%;height:100%;"
            "overflow:hidden;background:transparent}</style></head><body><script type=\"module\">\n"
            "window.__ograf = (async () => {\n"
            "  const mod = await import(%1);\n"
            "  const G = mod.default;\n"
            "  if (typeof G !== 'function') throw new Error('The graphic\\'s module has no default export (its element class)');\n"
            "  customElements.define('montage-ograf-graphic', G);\n"
            "  const el = document.createElement('montage-ograf-graphic');\n"
            "  el.style.cssText = 'position:absolute;left:0;top:0;width:100%;height:100%;display:block';\n"
            "  document.body.appendChild(el);\n"
            "  window.__graphic = el;\n"
            "  return true;\n"
            "})();\n"
            "</script></body></html>")
            .arg(compact(QJsonValue("/" + QString::fromStdString(g.main).remove(QRegularExpression("^(\\./|/)+")))))
            .toUtf8();
    b.onEvent = [&](const QString& method, const QJsonObject& p) {
        if (method == "Fetch.requestPaused") {
            const QUrl url(p.value("request").toObject().value("url").toString());
            const QString urlPath = url.path(QUrl::FullyDecoded);
            const QString id = p.value("requestId").toString();
            if (url.host() != kHost) {
                b.post("Fetch.failRequest", {{"requestId", id}, {"errorReason", "BlockedByClient"}});
                return;
            }
            QByteArray body;
            QString type = "text/plain";
            int code = 404;
            const QString path = QDir::cleanPath(urlPath);
            if (path == "/__montage/harness.html") {
                body = harness;
                type = "text/html";
                code = 200;
            } else {
                // Only files really inside its folder (a link out of it is not followed).
                const QString file = QFileInfo(QDir::cleanPath(root.absoluteFilePath(path.mid(1)))).canonicalFilePath();
                QFile f(file);
                if (!file.isEmpty() && file.startsWith(rootPath + "/") && QFileInfo(file).isFile() && f.open(QIODevice::ReadOnly)) {
                    body = f.readAll();
                    type = mimeType(file);
                    code = 200;
                }
            }
            b.post("Fetch.fulfillRequest", {{"requestId", id},
                                            {"responseCode", code},
                                            {"responseHeaders", QJsonArray{QJsonObject{{"name", "Content-Type"}, {"value", type}},
                                                                           QJsonObject{{"name", "Cache-Control"}, {"value", "no-store"}}}},
                                            {"body", QString::fromLatin1(body.toBase64())}});
        } else if (method == "Runtime.exceptionThrown") {
            const QJsonObject d = p.value("exceptionDetails").toObject();
            const QString text = d.value("exception").toObject().value("description").toString(d.value("text").toString());
            if (pageError.empty()) pageError = text.section('\n', 0, 0).toStdString();
        }
    };
    auto call = [&](const char* method, const QJsonObject& params, QJsonObject* r = nullptr) { return b.call(method, params, r, error); };
    // Runs `js` in the page (awaiting what it returns); false with the graphic's error when it throws.
    auto eval = [&](const QString& js, QJsonValue* value = nullptr) {
        QJsonObject r;
        if (!call("Runtime.evaluate", {{"expression", js}, {"awaitPromise", true}, {"returnByValue", true}}, &r)) return false;
        if (r.contains("exceptionDetails")) {
            const QJsonObject d = r.value("exceptionDetails").toObject();
            const QString text = d.value("exception").toObject().value("description").toString(d.value("text").toString());
            if (error) *error = "The graphic failed: " + text.section('\n', 0, 0).toStdString();
            return false;
        }
        if (value) *value = r.value("result").toObject().value("value");
        return true;
    };
    if (!call("Page.enable", {}) || !call("Runtime.enable", {}) ||
        !call("Page.addScriptToEvaluateOnNewDocument", {{"source", QString::fromUtf8(kClock)}}) ||
        !call("Fetch.enable", {{"patterns", QJsonArray{QJsonObject{{"urlPattern", "*"}}}}}) ||
        !call("Emulation.setDeviceMetricsOverride", {{"width", job.width}, {"height", job.height}, {"deviceScaleFactor", 1}, {"mobile", false}}) ||
        !call("Emulation.setDefaultBackgroundColorOverride", {{"color", QJsonObject{{"r", 0}, {"g", 0}, {"b", 0}, {"a", 0}}}}))
        return false;
    bool loaded = false;
    auto previous = b.onEvent;
    b.onEvent = [&](const QString& method, const QJsonObject& p) {
        if (method == "Page.loadEventFired") loaded = true;
        previous(method, p);
    };
    if (!call("Page.navigate", {{"url", QStringLiteral("http://%1/__montage/harness.html").arg(kHost)}}) ||
        !b.until([&] { return loaded; }, 60000, error))
        return false;
    if (!eval("window.__ograf")) return false;

    QJsonValue canSeek;
    if (!eval("typeof __graphic.goToTime === 'function' && typeof __graphic.setActionsSchedule === 'function'", &canSeek)) return false;
    const bool seek = g.nonRealTime && canSeek.toBool();
    const QString data = compact(QJsonDocument::fromJson(QByteArray::fromStdString(job.data)).isObject()
                                     ? QJsonValue(parseObject(job.data))
                                     : QJsonValue(QJsonObject{}));
    // (In one go: the data may hold "%1".)
    if (!eval(QStringLiteral("__graphic.load({data: %1, renderType: '%2', renderCharacteristics: {resolution: {width: %3, height: %4}, "
                             "frameRate: %5}}).then(() => document.fonts.ready).then(() => true)")
                  .arg(data, seek ? QStringLiteral("non-realtime") : QStringLiteral("realtime"), QString::number(job.width),
                       QString::number(job.height), number(job.fps))))
        return false;
    std::vector<OGrafAction> actions = job.actions;
    std::stable_sort(actions.begin(), actions.end(), [](const OGrafAction& a, const OGrafAction& c) { return a.at < c.at; });
    auto paramsOf = [](const OGrafAction& a) {
        const QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(a.params));
        return d.isObject() ? compact(d.object()) : QStringLiteral("{}");
    };
    if (seek) {
        QStringList schedule;
        for (const OGrafAction& a : actions)
            schedule << QStringLiteral("{timestamp: %1, action: {type: %2, params: %3}}")
                            .arg(number(a.at * 1000), compact(QJsonValue(qs(a.type))), paramsOf(a));
        if (!eval("__graphic.setActionsSchedule({schedule: [" + schedule.join(',') + "]}).then(() => true)")) return false;
    } else if (!eval("__ografStart(), true")) {
        return false;
    }
    size_t nextAction = 0;
    for (int i = 0; i < job.frames; ++i) {
        if (cancel && cancel->load()) return fail("Cancelled");
        const double ms = i * 1000.0 / job.fps;
        if (seek) {
            if (!eval(QStringLiteral("__graphic.goToTime({timestamp: %1}).then(() => true)").arg(number(ms)))) return false;
        } else {
            // The actions due by now, each at its own time (not awaited: they finish as the clock runs on).
            for (; nextAction < actions.size() && actions[nextAction].at * 1000 <= ms + 1e-6; ++nextAction) {
                const OGrafAction& a = actions[nextAction];
                const QString type = qs(a.type);
                if (type != "playAction" && type != "stopAction" && type != "updateAction" && type != "customAction") continue;
                // (One that fails says so when the frame is taken.)
                if (!eval(QStringLiteral("__ografAdvance(%1).then(() => { Promise.resolve(__graphic.%2(%3)).catch(e => { "
                                         "window.__ografFailed = window.__ografFailed || '%2: ' + ((e && e.message) || e); }); "
                                         "return true; })")
                              .arg(number(a.at * 1000), type, paramsOf(a))))
                    return false;
            }
            QJsonValue failed;
            if (!eval(QStringLiteral("__ografAdvance(%1).then(() => window.__ografFailed || true)").arg(number(ms)), &failed)) return false;
            if (failed.isString()) return fail("The graphic failed: " + failed.toString().toStdString());
        }
        if (!pageError.empty()) return fail("The graphic failed: " + pageError);
        QJsonObject shot;
        if (!call("Page.captureScreenshot", {{"format", "png"}, {"captureBeyondViewport", false}}, &shot)) return false;
        QFile out(qs(folder) + QStringLiteral("/frame_%1.png").arg(i, 5, 10, QLatin1Char('0')));
        if (!out.open(QIODevice::WriteOnly) || out.write(QByteArray::fromBase64(shot.value("data").toString().toLatin1())) <= 0)
            return fail("Cannot write the graphic's frames to " + folder);
        if (progress) progress(double(i + 1) / job.frames);
    }
    return true;
}

// ---- On the timeline ------------------------------------------------------------------------------------------

OGrafJob ografClipJob(const Effect& g, int width, int height, double fps, FrameTime duration) {
    OGrafJob job;
    const auto m = g.strings.find("manifest");
    if (m == g.strings.end() || m->second.empty()) return job;
    job.manifest = m->second;
    const auto d = g.strings.find("data");
    job.data = d != g.strings.end() && !d->second.empty() ? d->second : "{}";
    job.width = width;
    job.height = height;
    job.fps = fps;
    job.frames = int(std::max<FrameTime>(0, duration));
    const double length = fps > 0 ? double(job.frames) / fps : 0;
    const auto a = g.strings.find("actions");
    const QJsonDocument given = QJsonDocument::fromJson(QByteArray::fromStdString(a != g.strings.end() ? a->second : std::string()));
    if (given.isArray()) {
        for (const QJsonValue& v : given.array()) {
            const QJsonObject o = v.toObject();
            OGrafAction act;
            act.at = o.value("at").toDouble();
            act.type = o.value("type").toString().toStdString();
            act.params = compact(o.value("params").toObject()).toStdString();
            job.actions.push_back(act);
        }
        return job;
    }
    // Played in at the start, on a step every `advance` seconds (when it has steps), out `out` seconds before the end.
    job.actions.push_back({0, "playAction", "{\"delta\":1}"});
    OGrafGraphic graphic;
    const double advance = g.p("advance", 0, 0), out = g.p("out", 0, 1);
    const double stopAt = std::max(0.0, length - out);
    if (advance > 0 && readOGraf(job.manifest, graphic) && graphic.stepCount != 1) {
        // (Its steps but the first; -1: as many as it likes; 0: none.)
        int steps = graphic.stepCount > 1 ? graphic.stepCount - 1 : graphic.stepCount < 0 ? 1000 : 0;
        for (double t = advance; t < stopAt && steps-- > 0; t += advance) job.actions.push_back({t, "playAction", "{\"delta\":1}"});
    }
    if (out > 0) job.actions.push_back({stopAt, "stopAction", "{}"});
    return job;
}

namespace {

// Every file of a graphic's folder by size and time (an edited graphic renders again), looked at once every two
// seconds at most (frames ask for it one after another).
QString cacheBase() {
    const QString base = qEnvironmentVariable("MONTAGE_OGRAF_CACHE");
    return base.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/montage/ograf" : base;
}

QByteArray folderStamp(const QString& manifest) {
    static std::mutex m;
    static std::map<QString, std::pair<qint64, QByteArray>> memo;
    static QElapsedTimer clock;
    std::lock_guard lock(m);
    if (!clock.isValid()) clock.start();
    auto& [when, stamp] = memo[manifest];
    if (!stamp.isEmpty() && clock.elapsed() - when < 2000) return stamp;
    QStringList files;
    QDirIterator it(QFileInfo(manifest).absolutePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && files.size() < 2000) {
        const QFileInfo fi(it.next());
        files << QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    }
    files.sort();
    stamp = QCryptographicHash::hash(files.join('\n').toUtf8(), QCryptographicHash::Sha1);
    when = clock.elapsed();
    return stamp;
}

}  // namespace

std::string ografCacheFolder(const OGrafJob& job) {
    if (job.manifest.empty()) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    const QString manifest = QFileInfo(qs(job.manifest)).absoluteFilePath();
    // (Montage's way of rendering them, and the browser, render differently when they change.)
    const QString browser = qs(ografBrowser());
    h.addData(QByteArrayLiteral("ograf-render-1|"));
    h.addData((browser + "|" + QString::number(QFileInfo(browser).lastModified().toMSecsSinceEpoch()) + "|").toUtf8());
    h.addData(manifest.toUtf8());
    h.addData(folderStamp(manifest));
    h.addData(("|" + qs(job.data) + "|" + QString::number(job.width) + "x" + QString::number(job.height) + "|" + number(job.fps) + "|" +
               QString::number(job.frames)).toUtf8());
    for (const OGrafAction& a : job.actions) h.addData(("|" + number(a.at) + "|" + qs(a.type) + "|" + qs(a.params)).toUtf8());
    return (cacheBase() + "/" + QString::fromLatin1(h.result().toHex())).toStdString();
}

bool ografCached(const OGrafJob& job) {
    const std::string folder = ografCacheFolder(job);
    return !folder.empty() && QFileInfo::exists(qs(folder) + "/done");
}

namespace {

struct Pending {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    std::string error;
    std::atomic<double> fraction{0};
};
std::mutex gMutex;
std::map<std::string, std::shared_ptr<Pending>> gRunning;
// Renders that failed, and when: not tried again for every frame, but again after a while (the browser was busy, or
// has been installed since).
struct Failure {
    std::string error;
    qint64 at = 0;
};
std::map<std::string, Failure> gFailed;
constexpr qint64 kRetryAfterMs = 120000;

// Keeps the cache to MONTAGE_OGRAF_CACHE_MB (default 2048): the renders finished longest ago go first. Renders left
// half done by a crash go after a day.
void pruneCache(const QString& keep) {
    const QDir base(cacheBase());
    const qint64 limit = qEnvironmentVariableIntValue("MONTAGE_OGRAF_CACHE_MB") > 0 ? qEnvironmentVariableIntValue("MONTAGE_OGRAF_CACHE_MB") : 2048;
    struct Entry {
        QString path;
        qint64 bytes = 0, when = 0;
    };
    std::vector<Entry> entries;
    qint64 total = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QFileInfo& d : base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (d.fileName().contains(".part-")) {
            if (now - d.lastModified().toMSecsSinceEpoch() > 24 * 3600 * 1000) QDir(d.absoluteFilePath()).removeRecursively();
            continue;
        }
        const QFileInfo done(d.absoluteFilePath() + "/done");
        if (!done.exists()) continue;
        Entry e{d.absoluteFilePath(), 0, done.lastModified().toMSecsSinceEpoch()};
        for (const QFileInfo& f : QDir(e.path).entryInfoList(QDir::Files)) e.bytes += f.size();
        total += e.bytes;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.when < b.when; });
    for (const Entry& e : entries) {
        if (total <= limit * 1024 * 1024) break;
        if (e.path == keep) continue;
        QDir(e.path).removeRecursively();
        total -= e.bytes;
    }
}

bool prepare(const OGrafJob& job, bool retry, const OGrafProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    const std::string folder = ografCacheFolder(job);
    if (folder.empty()) {
        if (error) *error = "No graphic chosen";
        return false;
    }
    if (QFileInfo::exists(qs(folder) + "/done")) return true;
    std::shared_ptr<Pending> pending;
    bool mine = false;
    {
        std::lock_guard lock(gMutex);
        if (!retry) {
            const auto f = gFailed.find(folder);
            if (f != gFailed.end() && QDateTime::currentMSecsSinceEpoch() - f->second.at < kRetryAfterMs) {
                if (error) *error = f->second.error;
                return false;
            }
        }
        auto& slot = gRunning[folder];
        if (!slot) {
            slot = std::make_shared<Pending>();
            mine = true;
        }
        pending = slot;
    }
    if (!mine) {
        // Waiting for the render already running (its progress shown; Cancel stops waiting, not it).
        std::unique_lock lock(pending->m);
        while (!pending->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return pending->done; })) {
            if (progress) progress(pending->fraction.load());
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return false;
            }
        }
        if (!pending->ok && error) *error = pending->error;
        return pending->ok;
    }
    // Rendered beside the cache's place and moved there whole.
    const QString part = qs(folder) + QStringLiteral(".part-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    std::string err;
    bool ok = renderOGraf(job, part.toStdString(), [&](double f) {
        pending->fraction = f;
        if (progress) progress(f);
    }, cancel, &err);
    if (ok) {
        QFile done(part + "/done");
        ok = done.open(QIODevice::WriteOnly) && done.write(QByteArray::number(job.frames)) > 0;
        done.close();
        ok = ok && (QDir().rename(part, qs(folder)) || QFileInfo::exists(qs(folder) + "/done"));
        if (!ok) err = "Cannot store the graphic's frames in " + folder;
    }
    if (QFileInfo::exists(part)) QDir(part).removeRecursively();
    if (ok) pruneCache(qs(folder));
    {
        std::lock_guard lock(gMutex);
        gRunning.erase(folder);
        if (ok) gFailed.erase(folder);
        else if (!(cancel && cancel->load())) gFailed[folder] = {err, QDateTime::currentMSecsSinceEpoch()};
    }
    {
        std::lock_guard lock(pending->m);
        pending->done = true;
        pending->ok = ok;
        pending->error = err;
    }
    pending->cv.notify_all();
    if (!ok && error) *error = err;
    return ok;
}

}  // namespace

bool prepareOGraf(const OGrafJob& job, const OGrafProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    return prepare(job, true, progress, cancel, error);
}

std::string ografFailure(const OGrafJob& job) {
    const std::string folder = ografCacheFolder(job);
    std::lock_guard lock(gMutex);
    const auto f = gFailed.find(folder);
    return f != gFailed.end() ? f->second.error : std::string();
}

Image ografFrame(const Effect& g, FrameTime t, int w, int h, int sequenceWidth, int sequenceHeight, FrameTime duration, double fps) {
    Image img(w, h);
    if (w <= 0 || h <= 0 || sequenceWidth <= 0 || sequenceHeight <= 0) return img;
    // Always rendered at the sequence's size (one render for every preview size and transform), then scaled.
    const OGrafJob job = ografClipJob(g, sequenceWidth, sequenceHeight, fps, duration);
    if (job.manifest.empty() || job.frames <= 0 || !prepare(job, false, {}, nullptr, nullptr)) return img;
    const int i = int(std::clamp<FrameTime>(t, 0, job.frames - 1));
    QImage q(qs(ografCacheFolder(job)) + QStringLiteral("/frame_%1.png").arg(i, 5, 10, QLatin1Char('0')));
    if (q.isNull()) return img;
    q = q.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (q.width() != w || q.height() != h) q = q.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < h; ++y) {
        const uchar* s = q.constScanLine(y);
        float* d = img.row(y);
        for (int x = 0; x < w * 4; ++x) d[x] = s[x] / 255.0f;
    }
    return img;
}

}  // namespace montage
