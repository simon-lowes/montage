#include "render/RemoteReview.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkProxy>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace montage::output {

namespace {

using Clock = std::chrono::steady_clock;

// Montage's SEI user-data UUID (2f175443-8ef0-4acf-a748-a09c2a1209b9).
constexpr uint8_t kTagUuid[16] = {0x2f, 0x17, 0x54, 0x43, 0x8e, 0xf0, 0x4a, 0xcf, 0xa7, 0x48, 0xa0, 0x9c, 0x2a, 0x12, 0x09, 0xb9};

std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

std::string timecodeText(const Timecode& tc) {
    char s[24];
    std::snprintf(s, sizeof s, "%02d:%02d:%02d%c%02d", tc.hours, tc.minutes, tc.seconds, tc.dropFrame ? ';' : ':', tc.frames);
    return s;
}

// What the SRT protocol is given (its options), the rest by the address itself.
void srtOptions(AVDictionary** d, bool listen, const std::string& passphrase, int latencyMs) {
    av_dict_set(d, "mode", listen ? "listener" : "caller", 0);
    av_dict_set(d, "transtype", "live", 0);
    av_dict_set_int(d, "latency", int64_t(std::clamp(latencyMs, 20, 8000)) * 1000, 0);  // (µs)
    if (!passphrase.empty()) {
        av_dict_set(d, "passphrase", passphrase.c_str(), 0);
        av_dict_set_int(d, "pbkeylen", 32, 0);  // AES-256
    }
}

// A font for timecodes: 5 x 7, each row's pixels in the low five bits (the leftmost first).
const uint8_t* glyph(char c) {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
    static const uint8_t colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t semicolon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08};
    static const uint8_t blank[7] = {};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return c == ':' ? colon : c == ';' ? semicolon : blank;
}

int burnScale(int height) { return std::max(1, height / 216); }

}  // namespace

// ---------------------------------------------------------------------------

bool parseStreamAddress(const std::string& address, std::string& scheme, std::string& host, int& port) {
    const size_t sep = address.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    scheme = address.substr(0, sep);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string rest = address.substr(sep + 3);
    rest = rest.substr(0, rest.find_first_of("?/"));
    size_t colon = rest.rfind(':');
    if (!rest.empty() && rest.front() == '[') {  // [IPv6]:port
        const size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        host = rest.substr(1, close - 1);
        colon = rest.find(':', close);
    } else {
        host = colon == std::string::npos ? rest : rest.substr(0, colon);
    }
    if (colon == std::string::npos) return false;
    const std::string p = rest.substr(colon + 1);
    if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
    port = std::stoi(p);
    return !host.empty() && port > 0 && port < 65536;
}

int markersPortFor(const StreamSettings& s) {
    if (s.markersPort < 0) return -1;
    if (s.markersPort > 0) return s.markersPort;
    std::string scheme, host;
    int port = 0;
    if (!parseStreamAddress(s.address, scheme, host, port) || port >= 65535) return -1;
    return port + 1;
}

std::string checkStreamSettings(const StreamSettings& s) {
    std::string scheme, host;
    int port = 0;
    if (!parseStreamAddress(s.address, scheme, host, port)) return "The address needs a host and a port, such as srt://0.0.0.0:9000";
    if (scheme != "srt" && scheme != "udp") return "Streams go out over SRT (srt://) or UDP (udp://)";
    if (scheme == "srt" && (s.passphrase.size() < 10 || s.passphrase.size() > 79))
        return "SRT needs a passphrase of 10 to 79 characters";
    if (markersPortFor(s) >= 0 && s.passphrase.size() < 10) return "Markers from reviewers need a passphrase of 10 characters or more";
    if (s.codec != "hevc" && s.codec != "h264") return "The codec is \"hevc\" or \"h264\"";
    if (s.bitrateKbps < 250 || s.bitrateKbps > 200000) return "The bit rate is 250 to 200000 kb/s";
    if (s.markersPort > 65535) return "The marker port is 1 to 65535";
    return {};
}

std::string generatePassphrase() {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string out;
    for (int i = 0; i < 20; ++i) out += alphabet[QRandomGenerator::system()->bounded(int(sizeof alphabet - 1))];
    return out;
}

std::vector<uint8_t> streamTagPayload(const StreamTag& t) {
    QJsonObject o;
    o["f"] = qint64(t.frame);
    o["tc"] = QString::fromStdString(t.timecode);
    o["fps"] = QStringLiteral("%1/%2").arg(t.fps.num).arg(t.fps.den);
    if (t.sequence) o["seq"] = qint64(t.sequence);
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    std::vector<uint8_t> out(kTagUuid, kTagUuid + 16);
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

bool parseStreamTag(const uint8_t* data, size_t size, StreamTag& out) {
    for (size_t at : {size_t(0), size_t(16)}) {
        if (size < at + 16 + 2 || std::memcmp(data + at, kTagUuid, 16) != 0) continue;
        QByteArray json(reinterpret_cast<const char*>(data + at + 16), qsizetype(size - at - 16));
        while (!json.isEmpty() && json.back() == '\0') json.chop(1);
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        if (!o.contains("f")) return false;
        out.frame = FrameTime(o.value("f").toInteger(-1));
        out.timecode = o.value("tc").toString().toStdString();
        const QStringList fps = o.value("fps").toString().split('/');
        out.fps = fps.size() == 2 ? Rational{fps[0].toInt(), std::max(1, fps[1].toInt())} : Rational{0, 1};
        out.sequence = Id(o.value("seq").toInteger(0));
        return out.frame >= 0;
    }
    return false;
}

Planes420::Planes420(int w, int h)
    : width(w), height(h), y(size_t(w) * size_t(h), 64), u(size_t((w + 1) / 2) * size_t((h + 1) / 2), 512), v(u) {}

Planes420 planesFrom(const VideoFrame& f) {
    Planes420 p(f.width, f.height);
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    auto at16 = [](const uint8_t* b) { return int(b[0]) | (int(b[1]) << 8); };
    if (f.format == PixelFormat::P216) {
        const uint8_t* uv = f.data + f.stride * size_t(f.height);
        parallelRows(f.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = f.data + size_t(y) * f.stride;
                for (int x = 0; x < f.width; ++x) p.y[size_t(y) * size_t(f.width) + size_t(x)] = uint16_t(std::min(1023, (at16(row + 2 * x) + 32) >> 6));
            }
        });
        for (int y = 0; y < ch; ++y) {
            const uint8_t* a = uv + size_t(2 * y) * f.stride;
            const uint8_t* b = uv + size_t(std::min(2 * y + 1, f.height - 1)) * f.stride;
            for (int x = 0; x < cw; ++x) {
                p.u[size_t(y) * size_t(cw) + size_t(x)] = uint16_t(std::min(1023, (at16(a + 4 * x) + at16(b + 4 * x) + 64) >> 7));
                p.v[size_t(y) * size_t(cw) + size_t(x)] = uint16_t(std::min(1023, (at16(a + 4 * x + 2) + at16(b + 4 * x + 2) + 64) >> 7));
            }
        }
    } else if (f.format == PixelFormat::Uyvy8) {
        for (int y = 0; y < f.height; ++y) {
            const uint8_t* row = f.data + size_t(y) * f.stride;
            for (int x = 0; x < f.width; ++x) p.y[size_t(y) * size_t(f.width) + size_t(x)] = uint16_t(row[(x / 2) * 4 + 1 + 2 * (x & 1)] << 2);
        }
        for (int y = 0; y < ch; ++y) {
            const uint8_t* a = f.data + size_t(2 * y) * f.stride;
            const uint8_t* b = f.data + size_t(std::min(2 * y + 1, f.height - 1)) * f.stride;
            for (int x = 0; x < cw; ++x) {
                p.u[size_t(y) * size_t(cw) + size_t(x)] = uint16_t((a[4 * x] + b[4 * x]) << 1);
                p.v[size_t(y) * size_t(cw) + size_t(x)] = uint16_t((a[4 * x + 2] + b[4 * x + 2]) << 1);
            }
        }
    }
    return p;
}

Rect burnInBox(int width, int height, size_t chars) {
    const int s = burnScale(height);
    Rect r;
    r.width = ((int(chars) * 6 - 1 + 4) * s + 1) & ~1;
    r.height = ((7 + 4) * s + 1) & ~1;
    r.x = std::max(0, (width - r.width) / 2) & ~1;
    r.y = std::max(0, height - r.height - height / 20) & ~1;
    r.width = std::min(r.width, width - r.x);
    r.height = std::min(r.height, height - r.y);
    return r;
}

void burnTimecode(Planes420& p, const std::string& text) {
    if (p.width <= 0 || p.height <= 0 || text.empty()) return;
    const Rect box = burnInBox(p.width, p.height, text.size());
    const int s = burnScale(p.height), cw = (p.width + 1) / 2;
    for (int y = box.y; y < box.y + box.height; ++y)
        std::fill_n(p.y.begin() + ptrdiff_t(y) * p.width + box.x, box.width, uint16_t(64));
    for (int y = box.y / 2; y < (box.y + box.height + 1) / 2; ++y)
        for (int x = box.x / 2; x < (box.x + box.width + 1) / 2 && x < cw; ++x) {
            p.u[size_t(y) * size_t(cw) + size_t(x)] = 512;
            p.v[size_t(y) * size_t(cw) + size_t(x)] = 512;
        }
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t* g = glyph(text[i]);
        const int gx = box.x + (2 + int(i) * 6) * s, gy = box.y + 2 * s;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col) {
                if (!(g[row] & (0x10 >> col))) continue;
                for (int dy = 0; dy < s; ++dy)
                    for (int dx = 0; dx < s; ++dx) {
                        const int x = gx + col * s + dx, y = gy + row * s + dy;
                        if (x < box.x + box.width && y < box.y + box.height) p.y[size_t(y) * size_t(p.width) + size_t(x)] = 940;
                    }
            }
    }
}

// ---------------------------------------------------------------------------
// The marker channel: JSON lines over TCP, the passphrase never sent. Montage greets each client with a salt, an
// iteration count and a nonce; both sides derive a key from the passphrase (PBKDF2-HMAC-SHA256), the client proves it
// has it over both nonces, Montage proves it back, and every marker after that carries a sequence number and an
// HMAC under a key for that connection alone. The notes themselves are not encrypted.

namespace {

QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }

QByteArray mac(const QByteArray& key, const QByteArray& text) {
    return QMessageAuthenticationCode::hash(text, key, QCryptographicHash::Sha256).toHex();
}

QByteArray markerKey(const std::string& passphrase, const QByteArray& salt, int iterations) {
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, QByteArray::fromStdString(passphrase), salt, iterations, 32);
}

QByteArray randomHex(int bytes) {
    QByteArray b(bytes, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(b.data()), bytes / 4);
    return b.toHex();
}

bool sameBytes(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (qsizetype i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

constexpr qsizetype kMaxLine = 1 << 20;  // (a marker's drawing can be long)
constexpr qsizetype kMaxGreeting = 1024;  // before the client has proved it has the passphrase
constexpr int kIterations = 100000;

class MarkerServer {
public:
    MarkerServer(std::string passphrase, std::function<void(RemoteMarker)> take) : pass_(std::move(passphrase)), take_(std::move(take)) {}
    ~MarkerServer() { stop(); }

    bool start(const QHostAddress& host, int port, std::string* error) {
        salt_ = QByteArray::fromHex(randomHex(16));
        key_ = markerKey(pass_, salt_, kIterations);
        std::promise<std::string> started;
        std::future<std::string> result = started.get_future();
        thread_ = std::thread([this, host, port, &started] { run(host, port, started); });
        const std::string why = result.get();
        if (!why.empty()) {
            thread_.join();
            if (error) *error = why;
            return false;
        }
        return true;
    }

    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard lock(m_);
            if (loop_) QMetaObject::invokeMethod(loop_, &QEventLoop::quit, Qt::QueuedConnection);
        }
        thread_.join();
    }

private:
    void run(const QHostAddress& host, int port, std::promise<std::string>& started) {
        QEventLoop loop;
        QTcpServer server;
        server.setMaxPendingConnections(16);
        if (!server.listen(host, quint16(port))) {
            started.set_value("The marker channel cannot listen on port " + std::to_string(port) + ": " + server.errorString().toStdString());
            return;
        }
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this, &server] {
            while (QTcpSocket* c = server.nextPendingConnection()) {
                QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
                const QString peer = c->peerAddress().toString();
                // Strangers are kept few: a handful waiting to log in (from any one address, fewer), and none from an
                // address that has guessed wrong too often.
                int waiting = 0, fromPeer = 0;
                for (QTcpSocket* o : server.findChildren<QTcpSocket*>())
                    if (o != c && !o->property("authed").toBool()) {
                        ++waiting;
                        fromPeer += o->peerAddress().toString() == peer;
                    }
                if (waiting >= 16 || fromPeer >= 4 || failures_[peer] >= 10 || server.findChildren<QTcpSocket*>().size() > 48) {
                    c->abort();
                    c->deleteLater();
                    continue;
                }
                c->setReadBufferSize(kMaxLine + 4096);
                const QByteArray nonce = randomHex(16);
                c->setProperty("nonce", nonce);
                c->setProperty("authed", false);
                c->setProperty("seq", 0);
                c->write(line({{"montage", "review"}, {"version", 2}, {"salt", QString::fromLatin1(salt_.toHex())}, {"iterations", kIterations},
                               {"nonce", QString::fromLatin1(nonce)}}));
                QObject::connect(c, &QTcpSocket::readyRead, c, [this, c] { read(c); });
                // Ten seconds to log in; then ten minutes' quiet at most.
                auto* idle = new QTimer(c);
                idle->setObjectName("idle");
                idle->setSingleShot(true);
                idle->start(10000);
                QObject::connect(idle, &QTimer::timeout, c, [c] { c->abort(); });
            }
        });
        {
            std::lock_guard lock(m_);
            loop_ = &loop;
        }
        started.set_value({});
        loop.exec();
        std::lock_guard lock(m_);
        loop_ = nullptr;
    }

    void refuse(QTcpSocket* c) {
        // Each wrong answer is told a second after the one before it, whoever sent them: guesses come one a second.
        c->setProperty("failed", true);
        ++failures_[c->peerAddress().toString()];
        const auto now = Clock::now();
        nextRefusal_ = std::max(now, nextRefusal_) + std::chrono::seconds(1);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(nextRefusal_ - now).count());
        QTimer::singleShot(ms, c, [c] {
            c->write(line({{"error", "wrong passphrase"}}));
            c->disconnectFromHost();
        });
    }

    void read(QTcpSocket* c) {
        if (c->property("failed").toBool()) {
            c->readAll();
            return;
        }
        while (c->canReadLine()) {
            const bool authed = c->property("authed").toBool();
            const QByteArray text = c->readLine(authed ? kMaxLine : kMaxGreeting).trimmed();
            if (text.isEmpty()) continue;
            const QJsonObject o = QJsonDocument::fromJson(text).object();
            if (!authed) {
                const QByteArray sn = c->property("nonce").toByteArray(), cn = o.value("nonce").toString().toLatin1();
                if (cn.size() != 32 || !sameBytes(o.value("auth").toString().toLatin1(), mac(key_, "client|" + sn + "|" + cn))) {
                    refuse(c);
                    return;
                }
                c->setProperty("authed", true);
                c->setProperty("session", mac(key_, "session|" + sn + "|" + cn));
                c->write(line({{"ok", true}, {"proof", QString::fromLatin1(mac(key_, "server|" + sn + "|" + cn))}}));
                if (QTimer* idle = c->findChild<QTimer*>("idle")) idle->start(600000);
                continue;
            }
            if (QTimer* idle = c->findChild<QTimer*>("idle")) idle->start(600000);
            const QByteArray session = c->property("session").toByteArray();
            const int seq = c->property("seq").toInt() + 1;
            const QByteArray body = o.value("marker").toString().toUtf8();
            if (o.value("seq").toInt() != seq || !sameBytes(o.value("mac").toString().toLatin1(), mac(session, QByteArray::number(seq) + "|" + body))) {
                c->write(line({{"error", "the marker's signature is wrong"}}));
                c->disconnectFromHost();
                return;
            }
            c->setProperty("seq", seq);
            const QJsonObject m = QJsonDocument::fromJson(body).object();
            RemoteMarker r;
            r.frame = FrameTime(m.value("frame").toInteger(-1));
            r.duration = std::max<FrameTime>(0, FrameTime(m.value("duration").toInteger(0)));
            r.sequence = Id(m.value("sequence").toInteger(0));
            r.timecode = m.value("timecode").toString().left(32).toStdString();
            r.author = m.value("author").toString().left(100).toStdString();
            r.text = m.value("text").toString().left(10000).toStdString();
            r.drawing = m.value("drawing").toString().toStdString();
            r.id = m.value("id").toString().left(100).toStdString();
            if (r.frame < 0) {
                c->write(line({{"error", "the frame is not one of the stream's"}, {"seq", seq}}));
                continue;
            }
            take_(std::move(r));
            c->write(line({{"ok", true}, {"seq", seq}, {"mac", QString::fromLatin1(mac(session, "ack|" + QByteArray::number(seq)))}}));
        }
        // (a line longer than allowed is not waited for)
        if (c->bytesAvailable() > (c->property("authed").toBool() ? kMaxLine : kMaxGreeting)) c->abort();
    }

    std::string pass_;
    QByteArray salt_, key_;
    std::function<void(RemoteMarker)> take_;
    std::thread thread_;
    std::mutex m_;
    QEventLoop* loop_ = nullptr;
    std::map<QString, int> failures_;  // wrong answers by address (the server's thread only)
    Clock::time_point nextRefusal_;
};

}  // namespace

bool sendRemoteMarkers(const std::string& host, int port, const std::string& passphrase, const std::vector<RemoteMarker>& markers,
                       std::string* error, int timeoutMs) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    QTcpSocket sock;
    sock.setProxy(QNetworkProxy::NoProxy);
    sock.connectToHost(QString::fromStdString(host), quint16(port));
    if (!sock.waitForConnected(timeoutMs)) return fail("Cannot reach the marker channel at " + host + ":" + std::to_string(port) + ": " + sock.errorString().toStdString());
    auto readObject = [&](QJsonObject& o) {
        while (!sock.canReadLine())
            if (!sock.waitForReadyRead(timeoutMs)) return false;
        o = QJsonDocument::fromJson(sock.readLine(kMaxGreeting)).object();
        return true;
    };
    QJsonObject hello;
    if (!readObject(hello) || hello.value("montage").toString() != "review" || hello.value("version").toInt() != 2)
        return fail("That is not a Montage marker channel (or not this version's)");
    const QByteArray salt = QByteArray::fromHex(hello.value("salt").toString().toLatin1());
    const int iterations = hello.value("iterations").toInt();
    if (salt.size() < 16 || iterations < 10000 || iterations > 10000000) return fail("The marker channel's greeting is not sound");
    const QByteArray key = markerKey(passphrase, salt, iterations);
    const QByteArray sn = hello.value("nonce").toString().toLatin1(), cn = randomHex(16);
    sock.write(line({{"nonce", QString::fromLatin1(cn)}, {"auth", QString::fromLatin1(mac(key, "client|" + sn + "|" + cn))}}));
    QJsonObject reply;
    // (twice the time: wrong answers are told late)
    if (!readObject(reply) && !readObject(reply)) return fail("The marker channel did not answer");
    if (!reply.value("ok").toBool())
        return fail(reply.value("error").toString() == "wrong passphrase" ? "The passphrase is wrong" : "The marker channel refused the connection");
    if (!sameBytes(reply.value("proof").toString().toLatin1(), mac(key, "server|" + sn + "|" + cn)))
        return fail("The marker channel could not show it has the passphrase: not sending to it");
    const QByteArray session = mac(key, "session|" + sn + "|" + cn);
    int seq = 0;
    for (const RemoteMarker& m : markers) {
        QJsonObject o{{"frame", qint64(m.frame)}, {"duration", qint64(m.duration)}, {"timecode", QString::fromStdString(m.timecode)},
                      {"author", QString::fromStdString(m.author)}, {"text", QString::fromStdString(m.text)}};
        if (m.sequence) o["sequence"] = qint64(m.sequence);
        if (!m.drawing.empty()) o["drawing"] = QString::fromStdString(m.drawing);
        if (!m.id.empty()) o["id"] = QString::fromStdString(m.id);
        const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        ++seq;
        sock.write(line({{"seq", seq}, {"marker", QString::fromUtf8(body)}, {"mac", QString::fromLatin1(mac(session, QByteArray::number(seq) + "|" + body))}}));
        if (!readObject(reply)) return fail("The marker channel did not answer");
        if (!reply.value("ok").toBool())
            return fail(reply.contains("error") ? "Montage refused the marker: " + reply.value("error").toString().toStdString() : "The marker was not taken");
        if (reply.value("seq").toInt() != seq || !sameBytes(reply.value("mac").toString().toLatin1(), mac(session, "ack|" + QByteArray::number(seq))))
            return fail("The marker channel's answer is not signed");
    }
    sock.disconnectFromHost();
    if (sock.state() != QAbstractSocket::UnconnectedState) sock.waitForDisconnected(1000);
    return true;
}

// ---------------------------------------------------------------------------

struct StreamBackend::Impl {
    mutable std::mutex m;
    std::condition_variable cv;
    StreamSettings settings;
    // Opened
    bool open = false, woken = false;
    Mode mode;
    Signal signal;
    int audioChannels = 0, audioRate = 48000;
    std::string url;
    bool srt = true;
    Id sequence = 0;
    std::unique_ptr<MarkerServer> markers;
    std::vector<RemoteMarker> received;
    int markerCount = 0, markerPort = -1;

    // The encoders (kept open while the output is), and how pictures go to the video one.
    const AVCodec* vcodec = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVPixelFormat vfmt = AV_PIX_FMT_NONE;
    bool tags = false;
    AVAudioFifo* fifo = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;

    // The connection: opened on a thread of its own (a listener waits there for a viewer), used here.
    std::thread connector;
    bool needConnect = false, closing = false;
    AVIOContext* pendingIo = nullptr;
    std::string connectError;
    AVFormatContext* mux = nullptr;
    bool connected = false, keyed = false, fresh = false;
    Clock::time_point t0;  // when the output opened: timestamps count from it, across viewers
    int64_t vpts = -1, apts = 0;
    int64_t encoded = 0, skipped = 0;
    std::deque<std::pair<Clock::time_point, size_t>> bytes;
    Clock::time_point next;

    static int interrupted(void* opaque) {
        auto* d = static_cast<Impl*>(opaque);
        std::lock_guard lock(d->m);
        return d->closing || d->woken ? 1 : 0;  // (a write held up by the network gives way when stopping)
    }

    bool openEncoders(std::string* error) {
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        const bool hevc = settings.codec == "hevc";
        std::vector<std::string> names;
        if (settings.hardware) names.push_back(hevc ? "hevc_nvenc" : "h264_nvenc");
        names.push_back(hevc ? "libx265" : "libx264");
        std::string why;
        for (const std::string& name : names) {
            const AVCodec* c = avcodec_find_encoder_by_name(name.c_str());
            if (!c) {
                why = "This FFmpeg has no " + name;
                continue;
            }
            AVCodecContext* e = avcodec_alloc_context3(c);
            const bool nvenc = name.find("nvenc") != std::string::npos;
            e->width = mode.width;
            e->height = mode.height;
            e->time_base = AVRational{mode.fps.den, mode.fps.num};
            e->framerate = AVRational{mode.fps.num, mode.fps.den};
            e->pix_fmt = hevc ? (nvenc ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P10LE) : (nvenc ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P);
            e->gop_size = std::max(1, int(std::lround(fps)));  // a key frame each second, for viewers joining
            e->max_b_frames = 0;
            e->bit_rate = int64_t(settings.bitrateKbps) * 1000;
            e->rc_max_rate = e->bit_rate;
            e->rc_buffer_size = int(e->bit_rate / 2);
            e->color_range = AVCOL_RANGE_MPEG;
            e->colorspace = signal.bt2020 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
            e->color_primaries = signal.bt2020 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
            e->color_trc = signal.eotf == Eotf::Pq ? AVCOL_TRC_SMPTE2084 : signal.eotf == Eotf::Hlg ? AVCOL_TRC_ARIB_STD_B67 : AVCOL_TRC_BT709;
            e->chroma_sample_location = AVCHROMA_LOC_LEFT;
            e->flags |= AV_CODEC_FLAG_LOW_DELAY;
            if (nvenc) {
                av_opt_set(e->priv_data, "preset", "p1", 0);
                av_opt_set(e->priv_data, "tune", "ll", 0);
                av_opt_set(e->priv_data, "rc", "cbr", 0);
                av_opt_set_int(e->priv_data, "zerolatency", 1, 0);
                av_opt_set_int(e->priv_data, "delay", 0, 0);
            } else {
                av_opt_set(e->priv_data, "preset", hevc ? "ultrafast" : "veryfast", 0);
                av_opt_set(e->priv_data, "tune", "zerolatency", 0);
                if (hevc) av_opt_set(e->priv_data, "x265-params", "log-level=error:repeat-headers=1", 0);
            }
            av_opt_set_int(e->priv_data, "forced-idr", 1, 0);
            tags = av_opt_set_int(e->priv_data, "udu_sei", 1, 0) >= 0;
            const int r = avcodec_open2(e, c, nullptr);
            if (r < 0) {
                why = "Cannot open " + name + ": " + averr(r);
                avcodec_free_context(&e);
                continue;
            }
            venc = e;
            vcodec = c;
            vfmt = e->pix_fmt;
            break;
        }
        if (!venc) {
            if (error) *error = why;
            return false;
        }
        vframe = av_frame_alloc();
        vframe->format = vfmt;
        vframe->width = mode.width;
        vframe->height = mode.height;
        if (av_frame_get_buffer(vframe, 0) < 0) {
            if (error) *error = "Out of memory";
            return false;
        }
        pkt = av_packet_alloc();
        if (audioChannels > 0) {
            const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
            AVCodecContext* a = ac ? avcodec_alloc_context3(ac) : nullptr;
            if (a) {
                a->sample_rate = audioRate;
                a->sample_fmt = AV_SAMPLE_FMT_FLTP;
                av_channel_layout_default(&a->ch_layout, 2);
                a->bit_rate = int64_t(settings.audioBitrateKbps) * 1000;
                a->time_base = AVRational{1, audioRate};
                if (avcodec_open2(a, ac, nullptr) < 0) avcodec_free_context(&a);
            }
            if (!a) {
                if (error) *error = "This FFmpeg has no AAC encoder";
                return false;
            }
            aenc = a;
            fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, audioRate);
            aframe = av_frame_alloc();
            aframe->format = AV_SAMPLE_FMT_FLTP;
            aframe->sample_rate = audioRate;
            av_channel_layout_default(&aframe->ch_layout, 2);
            aframe->nb_samples = aenc->frame_size > 0 ? aenc->frame_size : 1024;
            av_frame_get_buffer(aframe, 0);
        }
        return true;
    }

    void freeEncoders() {
        avcodec_free_context(&venc);
        avcodec_free_context(&aenc);
        if (fifo) av_audio_fifo_free(fifo);
        fifo = nullptr;
        av_frame_free(&vframe);
        av_frame_free(&aframe);
        av_packet_free(&pkt);
        vcodec = nullptr;
    }

    void connectLoop() {
        for (;;) {
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return closing || needConnect; });
                if (closing) return;
            }
            AVDictionary* opts = nullptr;
            if (srt) srtOptions(&opts, settings.listen, settings.passphrase, settings.latencyMs);
            av_dict_set_int(&opts, "pkt_size", 1316, 0);  // (seven transport packets a datagram)
            AVIOInterruptCB cb{&Impl::interrupted, this};
            AVIOContext* io = nullptr;
            const int r = avio_open2(&io, url.c_str(), AVIO_FLAG_WRITE, &cb, &opts);
            av_dict_free(&opts);
            std::unique_lock lock(m);
            if (closing) {
                if (io) avio_closep(&io);
                return;
            }
            if (r >= 0) {
                pendingIo = io;
                needConnect = false;
                connectError.clear();
                continue;
            }
            connectError = "Cannot open " + url + ": " + averr(r);
            cv.wait_for(lock, std::chrono::seconds(1), [&] { return closing; });  // (then tried again)
        }
    }

    // A viewer came: the transport stream starts, from a key frame.
    bool startMux(AVIOContext* io) {
        AVFormatContext* oc = nullptr;
        if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) {
            avio_closep(&io);
            return false;
        }
        oc->pb = io;
        oc->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
        oc->max_delay = 0;
        AVStream* vs = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(vs->codecpar, venc);
        vs->time_base = venc->time_base;
        if (aenc) {
            AVStream* as = avformat_new_stream(oc, nullptr);
            avcodec_parameters_from_context(as->codecpar, aenc);
            as->time_base = aenc->time_base;
        }
        av_dict_set(&oc->metadata, "service_name", "Montage review", 0);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);
        const int r = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            avformat_free_context(oc);
            avio_closep(&io);
            std::lock_guard lock(m);
            connectError = "Cannot start the stream: " + averr(r);
            return false;
        }
        mux = oc;
        keyed = false;
        fresh = true;
        std::lock_guard lock(m);
        connected = true;
        encoded = skipped = 0;
        bytes.clear();
        return true;
    }

    void stopMux(bool reconnect) {
        if (mux) {
            AVIOContext* io = mux->pb;
            avformat_free_context(mux);
            mux = nullptr;
            avio_closep(&io);
        }
        std::lock_guard lock(m);
        connected = false;
        if (reconnect) {
            needConnect = true;
            cv.notify_all();
        }
    }

    // Packets the encoder has ready go out; false when the connection failed.
    bool drain(AVCodecContext* enc, int stream) {
        for (;;) {
            const int r = avcodec_receive_packet(enc, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            if (stream == 0 && !keyed) {
                if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_unref(pkt);
                    continue;
                }
                keyed = true;
            }
            if (stream == 1 && !keyed) {  // (sound starts with the first picture)
                av_packet_unref(pkt);
                continue;
            }
            pkt->stream_index = stream;
            av_packet_rescale_ts(pkt, enc->time_base, mux->streams[stream]->time_base);
            const size_t size = size_t(pkt->size);
            const int w = av_write_frame(mux, pkt);
            av_packet_unref(pkt);
            if (w < 0) return false;
            const auto now = Clock::now();
            std::lock_guard lock(m);
            bytes.emplace_back(now, size);
            while (!bytes.empty() && now - bytes.front().first > std::chrono::seconds(3)) bytes.pop_front();
        }
    }

    bool encodePicture(const VideoFrame& f, int64_t pts, bool key) {
        Planes420 p = planesFrom(f);
        if (settings.burnTimecode) burnTimecode(p, timecodeText(f.timecode));
        if (av_frame_make_writable(vframe) < 0) return false;
        const int w = mode.width, h = mode.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        switch (vfmt) {
            case AV_PIX_FMT_YUV420P10LE:
                for (int y = 0; y < h; ++y) std::memcpy(vframe->data[0] + size_t(y) * size_t(vframe->linesize[0]), p.y.data() + size_t(y) * size_t(w), size_t(w) * 2);
                for (int y = 0; y < ch; ++y) {
                    std::memcpy(vframe->data[1] + size_t(y) * size_t(vframe->linesize[1]), p.u.data() + size_t(y) * size_t(cw), size_t(cw) * 2);
                    std::memcpy(vframe->data[2] + size_t(y) * size_t(vframe->linesize[2]), p.v.data() + size_t(y) * size_t(cw), size_t(cw) * 2);
                }
                break;
            case AV_PIX_FMT_P010LE:
                for (int y = 0; y < h; ++y) {
                    auto* d = reinterpret_cast<uint16_t*>(vframe->data[0] + size_t(y) * size_t(vframe->linesize[0]));
                    for (int x = 0; x < w; ++x) d[x] = uint16_t(p.y[size_t(y) * size_t(w) + size_t(x)] << 6);
                }
                for (int y = 0; y < ch; ++y) {
                    auto* d = reinterpret_cast<uint16_t*>(vframe->data[1] + size_t(y) * size_t(vframe->linesize[1]));
                    for (int x = 0; x < cw; ++x) {
                        d[2 * x] = uint16_t(p.u[size_t(y) * size_t(cw) + size_t(x)] << 6);
                        d[2 * x + 1] = uint16_t(p.v[size_t(y) * size_t(cw) + size_t(x)] << 6);
                    }
                }
                break;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_NV12: {
                auto b8 = [](uint16_t v) { return uint8_t(std::min(255, (v + 2) >> 2)); };
                for (int y = 0; y < h; ++y) {
                    uint8_t* d = vframe->data[0] + size_t(y) * size_t(vframe->linesize[0]);
                    for (int x = 0; x < w; ++x) d[x] = b8(p.y[size_t(y) * size_t(w) + size_t(x)]);
                }
                for (int y = 0; y < ch; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        const size_t i = size_t(y) * size_t(cw) + size_t(x);
                        if (vfmt == AV_PIX_FMT_NV12) {
                            uint8_t* d = vframe->data[1] + size_t(y) * size_t(vframe->linesize[1]);
                            d[2 * x] = b8(p.u[i]);
                            d[2 * x + 1] = b8(p.v[i]);
                        } else {
                            vframe->data[1][size_t(y) * size_t(vframe->linesize[1]) + size_t(x)] = b8(p.u[i]);
                            vframe->data[2][size_t(y) * size_t(vframe->linesize[2]) + size_t(x)] = b8(p.v[i]);
                        }
                    }
                }
                break;
            }
            default: return false;
        }
        vframe->pts = pts;
        vframe->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        av_frame_remove_side_data(vframe, AV_FRAME_DATA_SEI_UNREGISTERED);
        if (tags) {
            StreamTag t;
            t.frame = f.frame;
            t.timecode = timecodeText(f.timecode);
            t.fps = f.fps;
            t.sequence = sequence;
            const std::vector<uint8_t> payload = streamTagPayload(t);
            if (AVFrameSideData* sd = av_frame_new_side_data(vframe, AV_FRAME_DATA_SEI_UNREGISTERED, payload.size()))
                std::memcpy(sd->data, payload.data(), payload.size());
        }
        if (avcodec_send_frame(venc, vframe) < 0) return false;
        return drain(venc, 0);
    }

    bool encodeSound(const Audio& a, int64_t pictureAt) {
        if (!aenc || a.frames <= 0) return true;
        // Sound follows on from what went before, unless pictures were skipped (or it drifted) by more than two
        // frames' worth: then from where the picture is.
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        const int64_t want = std::llround(double(pictureAt) * audioRate / fps);
        const int64_t queued = apts + av_audio_fifo_size(fifo);
        if (std::llabs(queued - want) > int64_t(2 * audioRate / fps)) {
            av_audio_fifo_reset(fifo);
            apts = want;
        }
        std::vector<float> l(size_t(a.frames), 0.0f), r(size_t(a.frames), 0.0f);
        if (a.samples && a.channels > 0)
            for (int i = 0; i < a.frames; ++i) {
                l[size_t(i)] = a.samples[size_t(i) * size_t(a.channels)];
                r[size_t(i)] = a.samples[size_t(i) * size_t(a.channels) + size_t(std::min(1, a.channels - 1))];
            }
        void* planes[2] = {l.data(), r.data()};
        av_audio_fifo_write(fifo, planes, a.frames);
        while (av_audio_fifo_size(fifo) >= aframe->nb_samples) {
            if (av_frame_make_writable(aframe) < 0) return false;
            av_audio_fifo_read(fifo, reinterpret_cast<void**>(aframe->data), aframe->nb_samples);
            aframe->pts = apts;
            apts += aframe->nb_samples;
            if (avcodec_send_frame(aenc, aframe) < 0) return false;
            if (!drain(aenc, 1)) return false;
        }
        return true;
    }
};

StreamBackend::StreamBackend() : d_(std::make_unique<Impl>()) {}

StreamBackend::~StreamBackend() { close(); }

bool StreamBackend::available(std::string* why) {
    void* it = nullptr;
    bool srt = false;
    while (const char* name = avio_enum_protocols(&it, 1)) srt |= std::strcmp(name, "srt") == 0;
    if (!srt) {
        if (why) *why = "This FFmpeg was built without SRT (libsrt)";
        return false;
    }
    if (!avcodec_find_encoder_by_name("libx265") && !avcodec_find_encoder_by_name("libx264") && !avcodec_find_encoder_by_name("hevc_nvenc")) {
        if (why) *why = "This FFmpeg has no HEVC or H.264 encoder";
        return false;
    }
    return true;
}

std::vector<Device> StreamBackend::devices() {
    Device d;
    d.id = "stream";
    d.name = "Remote review stream";
    d.formats = {PixelFormat::P216, PixelFormat::Uyvy8};
    d.hdr = true;
    d.audioChannels = {2};
    d.audioRates.clear();
    return {d};
}

void StreamBackend::setSequence(Id id) {
    std::lock_guard lock(d_->m);
    d_->sequence = id;
}

void StreamBackend::setSettings(const StreamSettings& s) {
    std::lock_guard lock(d_->m);
    d_->settings = s;
}

StreamSettings StreamBackend::settings() const {
    std::lock_guard lock(d_->m);
    return d_->settings;
}

bool StreamBackend::open(const Device&, const Mode& mode, const Signal& signal, int audioChannels, int audioRate, std::string* error) {
    close();
    Impl& d = *d_;
    const std::string why = checkStreamSettings(d.settings);
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    if (mode.width % 2 || mode.height % 2) {
        if (error) *error = "Streams need an even width and height";
        return false;
    }
    if (d.settings.codec == "h264" && signal.eotf != Eotf::Sdr) {
        if (error) *error = "H.264 goes out at 8 bits: HDR needs HEVC (or the picture as the viewer shows it)";
        return false;
    }
    std::string scheme, host;
    int port = 0;
    parseStreamAddress(d.settings.address, scheme, host, port);
    d.srt = scheme == "srt";
    d.url = d.settings.address;
    d.mode = mode;
    d.signal = signal;
    d.audioChannels = audioChannels;
    d.audioRate = audioRate > 0 ? audioRate : 48000;
    if (!d.openEncoders(error)) {
        d.freeEncoders();
        return false;
    }
    d.markerPort = markersPortFor(d.settings);
    d.markerCount = 0;
    d.received.clear();
    if (d.markerPort >= 0) {
        d.markers = std::make_unique<MarkerServer>(d.settings.passphrase, [this](RemoteMarker r) {
            std::lock_guard lock(d_->m);
            d_->received.push_back(std::move(r));
            ++d_->markerCount;
        });
        // On the address the stream listens on when it is one of this computer's, else on all of them.
        const QHostAddress at = d.srt && d.settings.listen && host != "0.0.0.0" && host != "::" ? QHostAddress(QString::fromStdString(host)) : QHostAddress::Any;
        if (!d.markers->start(at.isNull() ? QHostAddress(QHostAddress::Any) : at, d.markerPort, error)) {
            d.markers.reset();
            d.freeEncoders();
            return false;
        }
    }
    {
        std::lock_guard lock(d.m);
        d.open = true;
        d.woken = false;
        d.closing = false;
        d.needConnect = true;
        d.pendingIo = nullptr;
        d.connectError.clear();
        d.next = d.t0 = Clock::now();
        d.vpts = -1;
        d.apts = 0;
    }
    d.connector = std::thread([this] { d_->connectLoop(); });
    return true;
}

bool StreamBackend::send(const VideoFrame& frame, const Audio& audio) {
    Impl& d = *d_;
    AVIOContext* io = nullptr;
    {
        std::lock_guard lock(d.m);
        if (!d.open || d.woken) return false;
        std::swap(io, d.pendingIo);
    }
    if (io && !d.startMux(io)) d.stopMux(true);
    if (d.connected) {
        const double fps = d.mode.fps.toDouble() > 0 ? d.mode.fps.toDouble() : 25;
        // Pictures are stamped with the time they go out, so a viewer sees them in step with the clock whether or not
        // encoding keeps up (the frames it could not send are skipped).
        // (They run on from one viewer to the next, the encoders being the same.)
        const int64_t due = std::llround(std::chrono::duration<double>(Clock::now() - d.t0).count() * fps);
        const int64_t pts = d.vpts < 0 || due > d.vpts + 2 ? std::max(due, d.vpts + 1) : d.vpts + 1;
        const int64_t skipped = d.fresh ? 0 : pts - d.vpts - 1;
        const bool ok = d.encodePicture(frame, pts, d.fresh) && d.encodeSound(audio, pts);
        d.vpts = pts;
        d.fresh = false;
        {
            // (what the interface's thread reads, changed under the lock)
            std::lock_guard lock(d.m);
            d.skipped += skipped;
            if (ok) ++d.encoded;
            else d.connectError = d.srt ? "The viewer went away" : "The stream could not be sent";
        }
        if (!ok) d.stopMux(true);
    }
    // Then the frame's time.
    std::unique_lock lock(d.m);
    d.next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / std::max(1.0, frame.fps.toDouble())));
    const auto now = Clock::now();
    if (d.next < now - std::chrono::milliseconds(200)) d.next = now;  // (fallen far behind: not caught up in a burst)
    d.cv.wait_until(lock, d.next, [&] { return d.woken || !d.open; });
    return !d.woken && d.open;
}

void StreamBackend::close() {
    Impl& d = *d_;
    {
        std::lock_guard lock(d.m);
        d.closing = true;
        d.open = false;
        d.cv.notify_all();
    }
    if (d.connector.joinable()) d.connector.join();
    if (d.markers) d.markers->stop();
    d.markers.reset();
    d.stopMux(false);
    {
        std::lock_guard lock(d.m);
        if (d.pendingIo) avio_closep(&d.pendingIo);
    }
    d.freeEncoders();
}

void StreamBackend::wake() {
    std::lock_guard lock(d_->m);
    d_->woken = true;
    d_->cv.notify_all();
}

int StreamBackend::receivers() {
    std::lock_guard lock(d_->m);
    return d_->open && d_->connected ? 1 : 0;
}

StreamBackend::Stats StreamBackend::stats() const {
    std::lock_guard lock(d_->m);
    Stats s;
    s.connected = d_->open && d_->connected;
    s.encoded = d_->encoded;
    s.skipped = d_->skipped;
    size_t total = 0;
    for (const auto& b : d_->bytes) total += b.second;
    if (d_->bytes.size() > 1) {
        const double span = std::chrono::duration<double>(d_->bytes.back().first - d_->bytes.front().first).count();
        if (span > 0.2) s.kbps = double(total) * 8 / 1000 / span;
    }
    s.encoder = d_->vcodec ? d_->vcodec->name : "";
    s.error = d_->connectError;
    s.markersPort = d_->markers ? d_->markerPort : -1;
    s.markers = d_->markerCount;
    return s;
}

std::vector<RemoteMarker> StreamBackend::takeMarkers() {
    std::lock_guard lock(d_->m);
    return std::exchange(d_->received, {});
}

// ---------------------------------------------------------------------------

struct ReviewReceiver::Impl {
    mutable std::mutex m;
    std::condition_variable cv;
    std::thread thread;
    bool stopping = false;
    std::string address, passphrase;
    int latencyMs = 120;
    bool listen = false;
    Picture picture_;
    int64_t serial = 0;
    std::deque<StreamTag> tags;
    std::vector<float> audio;
    Status status;

    static int interrupted(void* opaque) {
        auto* d = static_cast<Impl*>(opaque);
        std::lock_guard lock(d->m);
        return d->stopping ? 1 : 0;
    }

    void fail(const std::string& why) {
        std::lock_guard lock(m);
        status.connected = false;
        status.error = why;
    }

    void run() {
        while (true) {
            {
                std::lock_guard lock(m);
                if (stopping) return;
            }
            watch();
            std::unique_lock lock(m);
            status.connected = false;
            cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping; });
        }
    }

    // One connection, until it drops.
    void watch() {
        std::string scheme, host;
        int port = 0;
        parseStreamAddress(address, scheme, host, port);
        AVFormatContext* ic = avformat_alloc_context();
        ic->interrupt_callback = {&Impl::interrupted, this};
        AVDictionary* opts = nullptr;
        if (scheme == "srt") {
            srtOptions(&opts, listen, passphrase, latencyMs);
            av_dict_set_int(&opts, "connect_timeout", 3000, 0);
        }
        av_dict_set_int(&opts, "probesize", 2000000, 0);
        av_dict_set_int(&opts, "analyzeduration", 2000000, 0);
        int r = avformat_open_input(&ic, address.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            fail(passphrase.empty() || scheme != "srt" ? "Cannot connect: " + averr(r)
                                                      : "Cannot connect (is Montage streaming there, with this passphrase?): " + averr(r));
            return;
        }
        r = avformat_find_stream_info(ic, nullptr);
        const int vi = r < 0 ? -1 : av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) {
            fail("The stream has no picture");
            avformat_close_input(&ic);
            return;
        }
        const int ai = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        auto openDecoder = [&](int index) -> AVCodecContext* {
            const AVCodec* c = avcodec_find_decoder(ic->streams[index]->codecpar->codec_id);
            if (!c) return nullptr;
            AVCodecContext* ctx = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(ctx, ic->streams[index]->codecpar);
            ctx->pkt_timebase = ic->streams[index]->time_base;
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            ctx->thread_type = FF_THREAD_SLICE;  // (frame threads would hold pictures back)
            if (avcodec_open2(ctx, c, nullptr) < 0) avcodec_free_context(&ctx);
            return ctx;
        };
        AVCodecContext* vdec = openDecoder(vi);
        AVCodecContext* adec = ai >= 0 ? openDecoder(ai) : nullptr;
        if (!vdec) {
            fail("Cannot decode the stream's picture");
            avformat_close_input(&ic);
            return;
        }
        SwrContext* swr = nullptr;
        if (adec) {
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, 2);
            swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, nullptr);
            if (swr && swr_init(swr) < 0) swr_free(&swr);
        }
        {
            std::lock_guard lock(m);
            status.connected = true;
            status.error.clear();
            status.audio = adec && swr;
        }
        SwsContext* sws = nullptr;
        AVPacket* p = av_packet_alloc();
        AVFrame* f = av_frame_alloc();
        std::string why = "The stream ended";
        while ((r = av_read_frame(ic, p)) >= 0) {
            if (p->stream_index == vi && avcodec_send_packet(vdec, p) >= 0) {
                while (avcodec_receive_frame(vdec, f) >= 0) {
                    picture(f, sws, avcodec_get_name(vdec->codec_id));
                    av_frame_unref(f);
                }
            } else if (p->stream_index == ai && adec && swr && avcodec_send_packet(adec, p) >= 0) {
                while (avcodec_receive_frame(adec, f) >= 0) {
                    const int out = swr_get_out_samples(swr, f->nb_samples);
                    std::vector<float> buf(size_t(std::max(0, out)) * 2);
                    uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(buf.data())};
                    const int n = swr_convert(swr, dst, out, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
                    if (n > 0) {
                        std::lock_guard lock(m);
                        audio.insert(audio.end(), buf.begin(), buf.begin() + ptrdiff_t(n) * 2);
                        if (audio.size() > 48000 * 2 * 2) audio.erase(audio.begin(), audio.end() - 48000 * 2 * 2);  // (2 s at most)
                    }
                    av_frame_unref(f);
                }
            }
            av_packet_unref(p);
        }
        if (r != AVERROR_EOF && r != AVERROR_EXIT) why = "The stream dropped: " + averr(r);
        av_packet_free(&p);
        av_frame_free(&f);
        sws_freeContext(sws);
        swr_free(&swr);
        avcodec_free_context(&vdec);
        avcodec_free_context(&adec);
        avformat_close_input(&ic);
        fail(why);
    }

    void picture(const AVFrame* f, SwsContext*& sws, const char* codec) {
        Picture pic;
        pic.width = f->width;
        pic.height = f->height;
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        pic.depth = desc ? desc->comp[0].depth : 8;
        pic.y.resize(size_t(f->width) * size_t(f->height));
        for (int y = 0; y < f->height; ++y) {
            const uint8_t* row = f->data[0] + size_t(y) * size_t(f->linesize[0]);
            for (int x = 0; x < f->width; ++x)
                pic.y[size_t(y) * size_t(f->width) + size_t(x)] = pic.depth > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
        }
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height, AV_PIX_FMT_RGBA, SWS_POINT,
                                   nullptr, nullptr, nullptr);
        if (sws) {
            const int cs = f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU709;
            sws_setColorspaceDetails(sws, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                     1 << 16, 1 << 16);
            pic.rgba.resize(size_t(f->width) * size_t(f->height) * 4);
            uint8_t* dst[1] = {pic.rgba.data()};
            const int stride[1] = {f->width * 4};
            sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
        }
        for (int i = 0; i < f->nb_side_data; ++i)
            if (f->side_data[i]->type == AV_FRAME_DATA_SEI_UNREGISTERED &&
                parseStreamTag(f->side_data[i]->data, f->side_data[i]->size, pic.tag))
                pic.tagged = true;
        std::lock_guard lock(m);
        pic.serial = ++serial;
        if (pic.tagged) {
            tags.push_back(pic.tag);
            if (tags.size() > 2000) tags.pop_front();
        }
        status.pictures = serial;
        char text[96];
        std::snprintf(text, sizeof text, "%s %dx%d %d-bit", codec, f->width, f->height, pic.depth);
        status.video = text;
        picture_ = std::move(pic);
    }
};

ReviewReceiver::ReviewReceiver() : d_(std::make_unique<Impl>()) {}

ReviewReceiver::~ReviewReceiver() { stop(); }

bool ReviewReceiver::start(const std::string& address, const std::string& passphrase, int latencyMs, bool listen, std::string* error) {
    stop();
    std::string scheme, host;
    int port = 0;
    if (!parseStreamAddress(address, scheme, host, port) || (scheme != "srt" && scheme != "udp")) {
        if (error) *error = "The address is srt://host:port (or udp://host:port)";
        return false;
    }
    if (scheme == "srt" && !passphrase.empty() && (passphrase.size() < 10 || passphrase.size() > 79)) {
        if (error) *error = "SRT passphrases are 10 to 79 characters";
        return false;
    }
    Impl& d = *d_;
    {
        std::lock_guard lock(d.m);
        d.stopping = false;
        d.address = address;
        d.passphrase = passphrase;
        d.latencyMs = latencyMs;
        d.listen = listen;
        d.status = Status{};
        d.serial = 0;
        d.picture_ = Picture{};
        d.tags.clear();
        d.audio.clear();
    }
    d.thread = std::thread([this] { d_->run(); });
    return true;
}

void ReviewReceiver::stop() {
    if (!d_->thread.joinable()) return;
    {
        std::lock_guard lock(d_->m);
        d_->stopping = true;
        d_->cv.notify_all();
    }
    d_->thread.join();
    std::lock_guard lock(d_->m);
    d_->status.connected = false;
}

bool ReviewReceiver::latest(Picture& out, int64_t after) const {
    std::lock_guard lock(d_->m);
    if (d_->picture_.serial <= after) return false;
    out = d_->picture_;
    return true;
}

std::vector<StreamTag> ReviewReceiver::tags() const {
    std::lock_guard lock(d_->m);
    return {d_->tags.begin(), d_->tags.end()};
}

std::vector<float> ReviewReceiver::takeAudio() {
    std::lock_guard lock(d_->m);
    return std::exchange(d_->audio, {});
}

ReviewReceiver::Status ReviewReceiver::status() const {
    std::lock_guard lock(d_->m);
    return d_->status;
}

}  // namespace montage::output
