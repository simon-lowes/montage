// Montage — a headless Chrome, Chromium or Edge driven over the Chrome DevTools Protocol: OGraf graphics render in one
// (render/OGraf.h), and tests open Montage's live review page in one (render/WebRtcReview.h).
#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>

namespace montage {


// A headless browser of its own (a fresh profile), driven over its DevTools WebSocket: one page, its session attached.
// Offline by default: no network but what the page's requests are answered with (render/OGraf.h).
class HeadlessBrowser {
public:
    ~HeadlessBrowser() {
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

    struct Options {
        bool offline = true;    // else it reaches the network (and this computer's own addresses) as a browser does
        QStringList extraArgs;  // more of Chrome's switches
    };

    bool start(const std::string& exe, int width, int height, bool sandbox, std::string* error) { return start(exe, width, height, sandbox, error, Options{}); }
    bool start(const std::string& exe, int width, int height, bool sandbox, std::string* error, const Options& options) {
        if (!profile_.isValid()) return fail("Cannot make a browser profile folder", error);
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
                         "--hide-scrollbars",
                         "--mute-audio",
                         "--force-color-profile=srgb",
                         "--enable-unsafe-swiftshader",
                         QStringLiteral("--window-size=%1,%2").arg(width).arg(height),
                         "about:blank"};
        QStringList more;
        if (!sandbox) more << "--no-sandbox";
        // Offline: every request the page makes is answered by the renderer (Fetch), and anything that escapes that
        // (a WebSocket, WebRTC) goes to a proxy that is not there, local addresses included.
        if (options.offline)
            more << "--proxy-server=http://127.0.0.1:9" << "--proxy-bypass-list=<-loopback>"
                 << "--force-webrtc-ip-handling-policy=disable_non_proxied_udp";
        more << options.extraArgs;
        args = QStringList{args.first()} + more + args.mid(1);
        // What it says goes to a file (a pipe nobody reads while it renders would fill and stop it).
        proc_.setProcessChannelMode(QProcess::MergedChannels);
        proc_.setStandardOutputFile(logFile_.path() + "/browser.log");
        proc_.start(QString::fromStdString(exe), args);
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
        // A busy machine can be slow to answer the upgrade (or drop the first one), so a silent attempt is retried
        // on a fresh connection until the minute is up.
        sock_.setProxy(QNetworkProxy::NoProxy);
        QDeadlineTimer hs(60000);
        int end = -1;
        while (end < 0) {
            if (cancel && cancel->load()) return fail("Cancelled", error);
            if (hs.hasExpired()) return fail("The browser's DevTools did not answer", error);
            sock_.abort();
            in_.clear();
            sock_.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
            if (!sock_.waitForConnected(10000)) return fail("Cannot reach the browser's DevTools port", error);
            QByteArray key(16, 0);
            for (char& c : key) c = char(QRandomGenerator::global()->bounded(256));
            sock_.write("GET " + path.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key.toBase64() +
                        "\r\nSec-WebSocket-Version: 13\r\n\r\n");
            QDeadlineTimer attempt(std::min<qint64>(15000, std::max<qint64>(1, hs.remainingTime())));
            while ((end = int(in_.indexOf("\r\n\r\n"))) < 0 && !attempt.hasExpired() &&
                   sock_.state() == QAbstractSocket::ConnectedState) {
                if (sock_.waitForReadyRead(int(std::min<qint64>(250, std::max<qint64>(1, attempt.remainingTime())))))
                    in_ += sock_.readAll();
                else if (cancel && cancel->load()) return fail("Cancelled", error);
            }
            if (end < 0) end = int((in_ += sock_.readAll()).indexOf("\r\n\r\n"));
            if (end < 0 && proc_.state() == QProcess::NotRunning)
                return fail("The browser stopped: " + QString::fromUtf8(log().right(400)).trimmed().toStdString(), error);
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

}  // namespace montage
