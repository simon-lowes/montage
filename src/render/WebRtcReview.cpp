#include "render/WebRtcReview.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <algorithm>
#include <cstring>

#ifdef MONTAGE_WITH_WEBRTC
#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <rtc/rtc.hpp>

#include "core/ReviewPage.h"
#include "media/ThreadScheduling.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

namespace montage::output {

std::vector<std::string> reviewCertificateHosts() {
    std::vector<std::string> out{"localhost", "127.0.0.1", "::1"};
    const QString name = QHostInfo::localHostName();
    if (!name.isEmpty()) {
        out.push_back(name.toStdString());
        if (!name.contains('.')) out.push_back(name.toStdString() + ".local");
    }
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (!a.isLoopback() && !a.isLinkLocal() && !a.isNull()) out.push_back(a.toString().section('%', 0, 0).toStdString());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

#ifndef MONTAGE_WITH_WEBRTC

bool makeReviewCertificate(const std::vector<std::string>&, std::string&, std::string&, std::string* error) {
    if (error) *error = "This build of Montage has no WebRTC (libdatachannel and OpenSSL)";
    return false;
}

struct BrowserReviewBackend::Impl {
    StreamSettings settings;
};

BrowserReviewBackend::BrowserReviewBackend() : d_(std::make_unique<Impl>()) {}
BrowserReviewBackend::~BrowserReviewBackend() = default;
bool BrowserReviewBackend::available(std::string* why) {
    if (why) *why = "This build of Montage has no WebRTC (libdatachannel and OpenSSL)";
    return false;
}
std::vector<Device> BrowserReviewBackend::devices() { return {}; }
void BrowserReviewBackend::setSettings(const StreamSettings& s) { d_->settings = s; }
StreamSettings BrowserReviewBackend::settings() const { return d_->settings; }
void BrowserReviewBackend::setSequence(Id) {}
bool BrowserReviewBackend::open(const Device&, const Mode&, const Signal&, int, int, std::string* error) { return available(error); }
bool BrowserReviewBackend::send(const VideoFrame&, const Audio&) { return false; }
void BrowserReviewBackend::close() {}
void BrowserReviewBackend::wake() {}
int BrowserReviewBackend::receivers() { return 0; }
ReviewStats BrowserReviewBackend::stats() const { return {}; }
std::vector<RemoteMarker> BrowserReviewBackend::takeMarkers() { return {}; }
std::vector<ReviewPointer> BrowserReviewBackend::pointers() const { return {}; }

#else

namespace {

using Clock = std::chrono::steady_clock;

constexpr qsizetype kMaxHeader = 16 * 1024, kMaxBody = 256 * 1024;  // (an offer is a few kilobytes)
constexpr int kMaxConnections = 64, kMaxConnectionsFrom = 8;  // (all, and from one address)
constexpr double kGuessRate = 5, kGuessBurst = 20;            // passphrases tried a second, from anywhere
constexpr int kMarkerRate = 10, kMarkerBurst = 50;  // as the marker channel's (render/RemoteReview.cpp)
constexpr qsizetype kDrawingRate = 256 * 1024, kDrawingBurst = 4 * 1024 * 1024;
constexpr int kPointerRate = 30;
constexpr std::chrono::minutes kFailureMemory{15};
constexpr std::chrono::seconds kPointerLife{4};
const char* const kPalette[] = {"#ff3b30", "#34c759", "#0a84ff", "#ff9f0a", "#bf5af2", "#64d2ff", "#ffd60a", "#ff375f"};

std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

QByteArray randomHex(int bytes) {
    QByteArray b(bytes, 0);
    for (char& c : b) c = char(QRandomGenerator::system()->bounded(256));
    return b.toHex();
}

// (compared as digests, in the same time whatever they hold or however long they are)
bool sameBytes(const QByteArray& a, const QByteArray& b) {
    const QByteArray x = QCryptographicHash::hash(a, QCryptographicHash::Sha256), y = QCryptographicHash::hash(b, QCryptographicHash::Sha256);
    int diff = 0;
    for (qsizetype i = 0; i < x.size(); ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}

// Where a connection comes from, as wrong passphrases are counted: its address, or its /64 for IPv6 (which one
// computer can have many addresses in).
QString guesser(const QHostAddress& a) {
    bool v4 = false;
    a.toIPv4Address(&v4);
    if (v4 || a.protocol() != QAbstractSocket::IPv6Protocol) return a.toString();
    Q_IPV6ADDR ip = a.toIPv6Address();
    for (int i = 8; i < 16; ++i) ip[i] = 0;
    return QHostAddress(ip).toString() + "/64";
}

QString lower(QString s) { return s.toLower(); }

// An ICE server as the settings name it ("turn:user:password@host:3478") and as a browser takes it.
QJsonObject browserIceServer(const std::string& url) {
    const QString u = QString::fromStdString(url);
    const int colon = u.indexOf(':');
    const QString scheme = u.left(colon), rest = u.mid(colon + 1);
    const int at = rest.lastIndexOf('@');
    QJsonObject o{{"urls", at < 0 ? u : scheme + ':' + rest.mid(at + 1)}};
    if (at >= 0) {
        const QString who = rest.left(at);
        const int c = who.indexOf(':');
        o["username"] = QUrl::fromPercentEncoding(who.left(c).toUtf8());
        if (c >= 0) o["credential"] = QUrl::fromPercentEncoding(who.mid(c + 1).toUtf8());
    }
    return o;
}

struct Request {
    QByteArray method, path;
    std::map<QByteArray, QByteArray> headers;  // names in lower case
    QByteArray body;
    QByteArray header(const char* name) const {
        auto it = headers.find(name);
        return it == headers.end() ? QByteArray() : it->second;
    }
};

// Parses a whole request from the start of `in` (removing it); 0 when more is needed, -1 when it is not one Montage
// takes, -2 when it is too large.
int takeRequest(QByteArray& in, Request& r) {
    const qsizetype end = in.indexOf("\r\n\r\n");
    if (end < 0) return in.size() > kMaxHeader ? -2 : 0;
    if (end > kMaxHeader) return -2;
    const QList<QByteArray> lines = in.left(end).split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    if (first.size() != 3 || !first[2].startsWith("HTTP/1.")) return -1;
    r.method = first[0];
    r.path = first[1];
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) continue;
        r.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
    }
    bool ok = true;
    const QByteArray cl = r.header("content-length");
    const qint64 length = cl.isEmpty() ? 0 : cl.toLongLong(&ok);
    if (!ok || length < 0) return -1;
    if (length > kMaxBody) return -2;
    if (!r.header("transfer-encoding").isEmpty()) return -1;
    if (in.size() < end + 4 + length) return 0;
    r.body = in.mid(end + 4, length);
    in.remove(0, end + 4 + length);
    return 1;
}

// A web server of the review's own: accepts connections and hands them on.
class HttpServer : public QTcpServer {
public:
    std::function<void(qintptr)> incoming;

protected:
    void incomingConnection(qintptr fd) override { incoming(fd); }
};

// The offer's choice for a medium: the payload type of the first codec in `order` it takes, and its format line.
struct Choice {
    int pt = -1;
    std::string codec;  // "h264", "av1", "hevc", "opus"
    std::string fmtp;
};

Choice choose(const rtc::Description::Media& m, const std::vector<std::string>& order) {
    for (const std::string& want : order) {
        Choice best;
        int bestScore = 0;
        for (int pt : m.payloadTypes()) {
            const rtc::Description::Media::RtpMap* map = m.rtpMap(pt);
            if (!map) continue;
            const QString format = lower(QString::fromStdString(map->format));
            std::string fmtp;
            for (const std::string& f : map->fmtps) fmtp += (fmtp.empty() ? "" : ";") + f;
            const QString f = lower(QString::fromStdString(fmtp));
            int score = 0;
            if (want == "h264" && format == "h264") {
                // Packetised in fragments (mode 1); constrained baseline best, as x264 makes it, then baseline.
                if (!f.contains("packetization-mode=1")) continue;
                score = f.contains("profile-level-id=42e0") ? 3 : f.contains("profile-level-id=4200") ? 2 : 1;
            } else if (want == "av1" && format == "av1") {
                if (f.contains("profile=") && !f.contains("profile=0")) continue;  // (8-bit 4:2:0 is profile 0)
                score = 1;
            } else if (want == "hevc" && format == "h265") {
                if (f.contains("profile-id=") && !f.contains("profile-id=1")) continue;  // (Main)
                score = 1;
            } else if (want == "opus" && format == "opus" && map->clockRate == 48000) {
                score = 1;
            }
            if (score > bestScore) {
                bestScore = score;
                best = {pt, want, fmtp};
            }
        }
        if (best.pt >= 0) return best;
    }
    return {};
}

struct Failures {
    int count = 0;
    Clock::time_point last;
};

}  // namespace

bool makeReviewCertificate(const std::vector<std::string>& hosts, std::string& certificatePem, std::string& keyPem, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = std::string("Cannot make a certificate: ") + what;
        return false;
    };
    // (Qt's TLS through the system's own (Secure Transport, Schannel) serves RSA keys only)
    const bool ec = QSslSocket::activeBackend() == QLatin1String("openssl");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(ec ? EVP_EC_gen("P-256") : EVP_RSA_gen(2048), EVP_PKEY_free);
    if (!key) return fail("no key");
    std::unique_ptr<X509, decltype(&X509_free)> x(X509_new(), X509_free);
    if (!x || !X509_set_version(x.get(), 2)) return fail("no certificate");
    unsigned char serial[16];
    if (RAND_bytes(serial, sizeof serial) != 1) return fail("no random numbers");
    serial[0] &= 0x7f;  // (positive)
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(BN_bin2bn(serial, sizeof serial, nullptr), BN_free);
    if (!bn || !BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(x.get()))) return fail("no serial number");
    X509_gmtime_adj(X509_getm_notBefore(x.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x.get()), 2L * 365 * 24 * 3600);
    X509_set_pubkey(x.get(), key.get());
    X509_NAME* name = X509_get_subject_name(x.get());
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("Montage"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("Montage live review"), -1, -1, 0);
    X509_set_issuer_name(x.get(), name);
    std::string san;
    for (const std::string& h : hosts) {
        if (h.empty() || h.find_first_of(",\r\n") != std::string::npos) continue;
        san += (san.empty() ? "" : ",") + std::string(QHostAddress(QString::fromStdString(h)).isNull() ? "DNS:" : "IP:") + h;
    }
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x.get(), x.get(), nullptr, nullptr, 0);
    const std::pair<int, std::string> exts[] = {{NID_basic_constraints, "critical,CA:FALSE"},
                                                {NID_key_usage, "critical,digitalSignature"},
                                                {NID_ext_key_usage, "serverAuth"},
                                                {NID_subject_alt_name, san}};
    for (const auto& [nid, value] : exts) {
        if (value.empty()) continue;
        X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
        if (!e) return fail("a bad host name");
        X509_add_ext(x.get(), e, -1);
        X509_EXTENSION_free(e);
    }
    if (!X509_sign(x.get(), key.get(), EVP_sha256())) return fail("cannot sign it");
    auto pem = [](const std::function<int(BIO*)>& write) {
        std::unique_ptr<BIO, decltype(&BIO_free)> b(BIO_new(BIO_s_mem()), BIO_free);
        if (!b || write(b.get()) != 1) return std::string();
        char* data = nullptr;
        const long n = BIO_get_mem_data(b.get(), &data);
        return std::string(data, size_t(std::max(0L, n)));
    };
    certificatePem = pem([&](BIO* b) { return PEM_write_bio_X509(b, x.get()); });
    keyPem = pem([&](BIO* b) { return PEM_write_bio_PrivateKey(b, key.get(), nullptr, nullptr, 0, nullptr, nullptr); });
    return (!certificatePem.empty() && !keyPem.empty()) || fail("cannot write it");
}

// ---------------------------------------------------------------------------

namespace {

// One viewer: their peer connection, its tracks, and the data channel the page opened.
struct Viewer {
    std::string id;     // the WHEP session's, in its Location (a secret: it ends the session)
    QString address;    // where they connect from
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::RtpPacketizationConfig> vcfg, acfg;
    std::string codec;  // the picture's ("" none)
    std::atomic<bool> needKey{true};  // nothing but a key frame goes to them next
    std::atomic<bool> connected{false}, gone{false};
    Clock::time_point created = Clock::now();
    std::mutex m;  // the members below
    std::shared_ptr<rtc::DataChannel> channel;
    std::string author = "Reviewer", color = kPalette[0];
    double markerBudget = kMarkerBurst, drawingBudget = double(kDrawingBurst), pointerBudget = kPointerRate;
    Clock::time_point budgetAt = Clock::now();
};

struct VideoEncoder {
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    std::string name;
    std::map<int64_t, StreamTag> tags;  // by picture, until its packet comes out
    ~VideoEncoder() {
        // (ended properly, which SVT-AV1 asks for; what is left is dropped)
        if (ctx && pkt && avcodec_send_frame(ctx, nullptr) >= 0)
            while (avcodec_receive_packet(ctx, pkt) >= 0) av_packet_unref(pkt);
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
    }
};

}  // namespace

struct BrowserReviewBackend::Impl {
    mutable std::mutex m;
    std::condition_variable cv;
    StreamSettings settings;
    Id sequence = 0;
    bool open = false, woken = false;
    Mode mode;
    int audioChannels = 0, audioRate = 48000;
    bool https = true;
    QHostAddress host;
    int port = 0;
    QSslCertificate certificate;
    QSslKey key;
    std::vector<rtc::IceServer> iceServers;

    // The web server's thread, its loop and the object queued work runs on.
    std::thread http;
    QObject* home = nullptr;
    QEventLoop* loop = nullptr;
    std::map<QString, Failures> failures;  // (that thread's)
    Clock::time_point nextRefusal;
    double guesses = kGuessBurst;  // passphrases that may be tried now
    Clock::time_point guessesAt = Clock::now();

    std::map<std::string, std::shared_ptr<Viewer>> viewers;
    // Viewers let go, kept a few seconds more: a callback of theirs still running is never the last to hold one (a
    // connection is not destroyed from its own thread).
    std::deque<std::pair<Clock::time_point, std::shared_ptr<Viewer>>> retired;
    std::vector<RemoteMarker> received;
    int markerCount = 0;
    struct Pointer {
        ReviewPointer p;
        Clock::time_point at;
    };
    std::map<std::string, Pointer> pointing;  // by viewer
    int joined = 0;  // viewers so far (their colours)
    std::string lastError;

    // Encoding (send's thread; the encoders map is changed under m, as stats() reads it).
    std::map<std::string, std::unique_ptr<VideoEncoder>> encoders;
    std::map<std::string, std::string> encoderErrors;
    std::map<std::string, Clock::time_point> forcedKey;  // when each codec's encoder was last made to start a key frame
    AVFrame* picture = nullptr;
    AVCodecContext* aenc = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* apkt = nullptr;
    SwrContext* swr = nullptr;
    Clock::time_point t0, next;
    int64_t vpts = -1, apts = 0;
    int64_t encoded = 0, skipped = 0;
    std::deque<std::pair<Clock::time_point, size_t>> bytes;

    // ---- The web server ----

    void run(std::promise<std::string>& started) {
        QEventLoop l;
        HttpServer server;
        server.setMaxPendingConnections(16);
        server.incoming = [this, &server](qintptr fd) { accept(server, fd); };
        if (!server.listen(host, quint16(port))) {
            started.set_value("Cannot listen on port " + std::to_string(port) + ": " + server.errorString().toStdString());
            return;
        }
        QTimer prune;
        QObject::connect(&prune, &QTimer::timeout, &server, [this] { pruneViewers(); });
        prune.start(1000);
        {
            std::lock_guard lock(m);
            home = &server;
            loop = &l;
        }
        started.set_value({});
        l.exec();
        {
            std::lock_guard lock(m);
            home = nullptr;
            loop = nullptr;
        }
    }

    void accept(HttpServer& server, qintptr fd) {
        auto* c = new QSslSocket(&server);
        if (!c->setSocketDescriptor(fd)) {
            delete c;
#ifdef Q_OS_WIN
            ::closesocket(SOCKET(fd));
#else
            ::close(int(fd));
#endif
            return;
        }
        const QList<QSslSocket*> open = server.findChildren<QSslSocket*>();
        const QHostAddress from = c->peerAddress();
        const auto fromThere = std::count_if(open.begin(), open.end(), [&](QSslSocket* o) { return o->peerAddress() == from; });
        if (open.size() > kMaxConnections || fromThere > kMaxConnectionsFrom) {
            c->abort();
            c->deleteLater();
            return;
        }
        QObject::connect(c, &QAbstractSocket::disconnected, c, &QObject::deleteLater);
        QObject::connect(c, &QAbstractSocket::readyRead, c, [this, c] { read(c); });
        // (TLS errors and the like: the connection is simply dropped)
        QObject::connect(c, &QAbstractSocket::errorOccurred, c, [c] { c->abort(); });
        auto* idle = new QTimer(c);
        idle->setSingleShot(true);
        QObject::connect(idle, &QTimer::timeout, c, [c] { c->abort(); });
        idle->start(20000);
        if (https) {
            QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
            conf.setLocalCertificate(certificate);
            conf.setPrivateKey(key);
            conf.setProtocol(QSsl::TlsV1_2OrLater);
            conf.setPeerVerifyMode(QSslSocket::VerifyNone);
            c->setSslConfiguration(conf);
            c->startServerEncryption();
        }
    }

    void read(QSslSocket* c) {
        if (c->property("answered").toBool()) {
            c->readAll();
            return;
        }
        QByteArray in = c->property("in").toByteArray() + c->readAll();
        Request r;
        const int got = takeRequest(in, r);
        c->setProperty("in", in);
        if (got == 0) return;
        c->setProperty("answered", true);
        if (got == -2) return respond(c, "413 Content Too Large", {}, "Too large");
        if (got < 0) return respond(c, "400 Bad Request", {}, "Not understood");
        handle(c, r);
    }

    void respond(QSslSocket* c, const QByteArray& status, std::vector<std::pair<QByteArray, QByteArray>> headers, const QByteArray& body,
                 const QByteArray& type = "text/plain; charset=utf-8") {
        QByteArray out = "HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n";
        if (!body.isEmpty()) out += "Content-Type: " + type + "\r\n";
        out += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nCache-Control: no-store\r\n";
        // (other WHEP players, such as one on a web page of its own, call from other origins; the passphrase guards it)
        out += "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: Location, Link\r\n";
        for (const auto& [k, v] : headers) out += k + ": " + v + "\r\n";
        out += "\r\n" + body;
        c->write(out);
        c->disconnectFromHost();
    }

    // The passphrase as a bearer token. Only so many are tried a second, from anywhere (the rest refused untried);
    // refused ones are told a second after the one before, whoever sent them, and an address that has guessed wrong
    // too often lately is refused at once.
    bool authorised(QSslSocket* c, const Request& r) {
        const QString peer = guesser(c->peerAddress());
        const auto now = Clock::now();
        guesses = std::min(kGuessBurst, guesses + std::chrono::duration<double>(now - guessesAt).count() * kGuessRate);
        guessesAt = now;
        if (guesses < 1 || nextRefusal - now > std::chrono::seconds(10)) {
            respond(c, "429 Too Many Requests", {{"Retry-After", "5"}}, "Too many passphrases tried: try again shortly");
            return false;
        }
        guesses -= 1;
        for (auto it = failures.begin(); it != failures.end();) it = now - it->second.last > kFailureMemory ? failures.erase(it) : std::next(it);
        while (failures.size() > 4096)
            failures.erase(std::min_element(failures.begin(), failures.end(), [](const auto& a, const auto& b) { return a.second.last < b.second.last; }));
        auto blocked = failures.find(peer);
        if (blocked != failures.end() && blocked->second.count >= 10) {
            respond(c, "429 Too Many Requests", {}, "Too many wrong passphrases from this address: try again later");
            return false;
        }
        QByteArray given = r.header("authorization");
        if (given.toLower().startsWith("bearer ")) given = given.mid(7).trimmed();
        else given.clear();
        if (sameBytes(given, QByteArray::fromStdString(settings.passphrase))) return true;
        Failures& f = failures[peer];
        ++f.count;
        f.last = now;
        nextRefusal = std::max(now, nextRefusal) + std::chrono::seconds(1);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(nextRefusal - now).count());
        QTimer::singleShot(ms, c, [this, c] {
            respond(c, "401 Unauthorized", {{"WWW-Authenticate", "Bearer realm=\"Montage review\""}}, "Wrong passphrase");
        });
        return false;
    }

    void handle(QSslSocket* c, const Request& r) {
        const QByteArray path = r.path.left(r.path.indexOf('?') < 0 ? r.path.size() : r.path.indexOf('?'));
        if (r.method == "OPTIONS")
            return respond(c, "204 No Content",
                           {{"Access-Control-Allow-Methods", "GET, POST, DELETE, PATCH, OPTIONS"},
                            {"Access-Control-Allow-Headers", "Authorization, Content-Type"},
                            {"Accept-Post", "application/sdp"}},
                           {});
        if (r.method == "GET" && (path == "/" || path == "/index.html"))
            return respond(c, "200 OK",
                           {{"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; "
                                                        "img-src data:; media-src 'self' blob:; frame-ancestors 'none'"},
                            {"X-Frame-Options", "DENY"}},
                           browserReviewPage(), "text/html; charset=utf-8");
        if (r.method == "GET" && path == "/review.js") return respond(c, "200 OK", {}, browserReviewScript(), "text/javascript; charset=utf-8");
        if (path == "/config" && r.method == "GET") {
            if (!authorised(c, r)) return;
            QJsonArray ice;
            for (const std::string& s : settings.iceServers) ice.append(browserIceServer(s));
            std::lock_guard lock(m);
            const QJsonObject o{{"iceServers", ice}, {"fps", QJsonArray{mode.fps.num, mode.fps.den}}, {"width", mode.width}, {"height", mode.height},
                                {"sequence", double(sequence)}};
            return respond(c, "200 OK", {}, QJsonDocument(o).toJson(QJsonDocument::Compact), "application/json");
        }
        if (path == "/whep") {
            if (r.method != "POST") return respond(c, "405 Method Not Allowed", {{"Allow", "POST, OPTIONS"}}, {});
            if (!authorised(c, r)) return;
            if (!r.header("content-type").toLower().startsWith("application/sdp")) return respond(c, "415 Unsupported Media Type", {}, "An SDP offer, please");
            return offer(c, r.body);
        }
        if (path.startsWith("/whep/")) {
            if (r.method == "PATCH") return respond(c, "405 Method Not Allowed", {{"Allow", "DELETE"}}, {});  // (no trickle ICE)
            if (r.method != "DELETE") return respond(c, "405 Method Not Allowed", {{"Allow", "DELETE, PATCH"}}, {});
            if (!authorised(c, r)) return;
            std::shared_ptr<Viewer> v;
            {
                std::lock_guard lock(m);
                auto it = viewers.find(path.mid(6).toStdString());
                if (it != viewers.end()) {
                    v = it->second;
                    viewers.erase(it);
                    pointing.erase(v->id);
                }
            }
            if (!v) return respond(c, "404 Not Found", {}, "No such session");
            closeViewer(v);
            return respond(c, "200 OK", {}, {});
        }
        respond(c, "404 Not Found", {}, "Not found");
    }

    // A viewer's offer: their connection made, answered once its candidates are gathered.
    void offer(QSslSocket* c, const QByteArray& sdp) {
        {
            std::lock_guard lock(m);
            if (int(viewers.size()) >= settings.maxViewers) return respond(c, "503 Service Unavailable", {}, "As many viewers as allowed are watching");
        }
        std::shared_ptr<Viewer> v;
        std::string why;
        try {
            v = makeViewer(sdp.toStdString(), c->peerAddress().toString(), why);
        } catch (const std::exception& e) {
            why = std::string("The offer is not one Montage can answer: ") + e.what();
        }
        if (!v) return respond(c, "400 Bad Request", {}, QByteArray::fromStdString(why));
        auto done = std::make_shared<std::atomic<bool>>(false);
        QPointer<QSslSocket> sock(c);
        std::weak_ptr<Viewer> wv = v;
        auto finish = [this, sock, wv] {
            std::shared_ptr<Viewer> v = wv.lock();
            if (!sock) return;
            std::optional<rtc::Description> local = v ? v->pc->localDescription() : std::nullopt;
            if (!local) return respond(sock, "500 Internal Server Error", {}, "No answer");
            std::vector<std::pair<QByteArray, QByteArray>> headers{{"Location", "/whep/" + QByteArray::fromStdString(v->id)}};
            for (const std::string& s : settings.iceServers) {
                const QJsonObject o = browserIceServer(s);
                QByteArray link = "<" + o.value("urls").toString().toUtf8() + ">; rel=\"ice-server\"";
                if (o.contains("username"))
                    link += "; username=\"" + o.value("username").toString().toUtf8() + "\"; credential=\"" + o.value("credential").toString().toUtf8() +
                            "\"; credential-type=\"password\"";
                headers.emplace_back("Link", link);
            }
            respond(sock, "201 Created", headers, QByteArray::fromStdString(std::string(*local)), "application/sdp");
        };
        auto later = [this, done, finish] {
            if (done->exchange(true)) return;
            std::lock_guard lock(m);
            if (home) QMetaObject::invokeMethod(home, finish, Qt::QueuedConnection);
        };
        v->pc->onGatheringStateChange([later](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) later();
        });
        {
            std::lock_guard lock(m);
            viewers[v->id] = v;
        }
        try {
            v->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), rtc::Description::Type::Offer));
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(m);
                viewers.erase(v->id);
            }
            closeViewer(v);
            return respond(c, "400 Bad Request", {}, QByteArray("The offer is not one Montage can answer: ") + e.what());
        }
        if (v->pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) later();
        QTimer::singleShot(3000, c, [later] { later(); });  // (what was gathered by then)
    }

    std::shared_ptr<Viewer> makeViewer(const std::string& sdp, const QString& address, std::string& why) {
        const rtc::Description offered(sdp, rtc::Description::Type::Offer);
        auto v = std::make_shared<Viewer>();
        v->id = randomHex(16).toStdString();
        v->address = address;
        rtc::Configuration conf;
        conf.iceServers = iceServers;
        // Media on the same port number as the page (UDP), all viewers together: one port to open in a firewall.
        conf.enableIceUdpMux = true;
        conf.portRangeBegin = conf.portRangeEnd = uint16_t(port);
        if (host != QHostAddress::Any && host != QHostAddress::AnyIPv6 && host != QHostAddress::AnyIPv4) conf.bindAddress = host.toString().toStdString();
        v->pc = std::make_shared<rtc::PeerConnection>(conf);
        std::vector<std::string> order{settings.codec};
        for (const char* c : {"h264", "av1", "hevc"})
            if (settings.codec != c) order.push_back(c);
        const rtc::Description& o = offered;
        for (int i = 0; i < o.mediaCount(); ++i) {
            const auto entry = o.media(i);
            const rtc::Description::Media* const* mp = std::get_if<const rtc::Description::Media*>(&entry);
            if (!mp || !*mp) continue;
            const rtc::Description::Media& media = **mp;
            const auto dir = media.direction();
            if (dir != rtc::Description::Direction::RecvOnly && dir != rtc::Description::Direction::SendRecv) continue;
            const bool isVideo = media.type() == "video", isAudio = media.type() == "audio";
            if ((isVideo && v->video) || (isAudio && (v->audio || audioChannels <= 0)) || (!isVideo && !isAudio)) continue;
            const Choice pick = choose(media, isVideo ? order : std::vector<std::string>{"opus"});
            if (pick.pt < 0) continue;
            const uint32_t ssrc = QRandomGenerator::system()->generate() | 1u;
            auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "montage", uint8_t(pick.pt), isVideo ? 90000 : 48000);
            std::unique_ptr<rtc::Description::Media> answer;
            if (isVideo) {
                auto d = std::make_unique<rtc::Description::Video>(media.mid(), rtc::Description::Direction::SendOnly);
                d->addVideoCodec(pick.pt, pick.codec == "h264" ? "H264" : pick.codec == "av1" ? "AV1" : "H265",
                                 pick.fmtp.empty() ? std::nullopt : std::optional<std::string>(pick.fmtp));
                d->addSSRC(ssrc, "montage", "montage", "video");
                answer = std::move(d);
            } else {
                auto d = std::make_unique<rtc::Description::Audio>(media.mid(), rtc::Description::Direction::SendOnly);
                d->addOpusCodec(pick.pt, pick.fmtp.empty() ? std::nullopt : std::optional<std::string>(pick.fmtp));
                d->addSSRC(ssrc, "montage", "montage", "audio");
                answer = std::move(d);
            }
            // The extensions that say which medium a packet is (before SSRCs are known) and ask for no playout delay.
            rtc::Description::Media copy = media;
            for (int id : copy.extIds()) {
                const rtc::Description::Media::ExtMap* e = copy.extMap(id);
                if (!e) continue;
                if (e->uri == "urn:ietf:params:rtp-hdrext:sdes:mid") {
                    answer->addExtMap(rtc::Description::Media::ExtMap(id, e->uri));
                    cfg->midId = uint8_t(id);
                    cfg->mid = media.mid();
                } else if (isVideo && e->uri == "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay") {
                    answer->addExtMap(rtc::Description::Media::ExtMap(id, e->uri));
                    cfg->playoutDelayId = uint8_t(id);
                    cfg->playoutDelayMin = cfg->playoutDelayMax = 0;
                }
            }
            std::shared_ptr<rtc::Track> track = v->pc->addTrack(*answer);
            std::weak_ptr<Viewer> wv = v;
            const std::string codec = pick.codec;
            std::shared_ptr<rtc::RtpPacketizer> packetizer;
            if (codec == "h264") packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg);
            else if (codec == "hevc") packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg);
            else if (codec == "av1") packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, cfg);
            else packetizer = std::make_shared<rtc::OpusRtpPacketizer>(cfg);
            packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(cfg));
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            if (isVideo) {
                // (the viewer's own flag only: the next picture sent starts a key frame for them, whoever else
                // watches on the same encoder)
                packetizer->addToChain(std::make_shared<rtc::PliHandler>([wv] {
                    if (auto v = wv.lock()) v->needKey = true;
                }));
                track->onOpen([wv] {
                    if (auto v = wv.lock()) v->needKey = true;
                });
            }
            track->setMediaHandler(packetizer);
            if (isVideo) {
                v->video = track;
                v->vcfg = cfg;
                v->codec = codec;
            } else {
                v->audio = track;
                v->acfg = cfg;
            }
        }
        if (!v->video && !v->audio) {
            why = "The offer asks for no picture Montage can send (H.264 in packetisation mode 1, AV1 or HEVC) and no Opus sound";
            v->pc->close();
            return nullptr;
        }
        std::weak_ptr<Viewer> wv = v;
        v->pc->onStateChange([wv](rtc::PeerConnection::State s) {
            auto v = wv.lock();
            if (!v) return;
            if (s == rtc::PeerConnection::State::Connected) v->connected = true;
            if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed) v->gone = true;
        });
        v->pc->onDataChannel([this, wv](std::shared_ptr<rtc::DataChannel> dc) {
            auto v = wv.lock();
            if (!v) return;
            {
                std::lock_guard lock(v->m);
                v->channel = dc;
            }
            dc->onOpen([this, wv] {
                if (auto v = wv.lock()) hello(*v);
            });
            dc->onMessage([this, wv](rtc::message_variant data) {
                auto v = wv.lock();
                if (const std::string* s = std::get_if<std::string>(&data); v && s) message(v, *s);
            });
            if (dc->isOpen()) hello(*v);
        });
        {
            std::lock_guard lock(m);
            v->color = kPalette[joined++ % (sizeof kPalette / sizeof *kPalette)];
        }
        return v;
    }

    static void sendText(Viewer& v, const QJsonObject& o) {
        std::shared_ptr<rtc::DataChannel> dc;
        {
            std::lock_guard lock(v.m);
            dc = v.channel;
        }
        if (!dc || !dc->isOpen()) return;
        try {
            dc->send(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
        } catch (const std::exception&) {
        }
    }

    void hello(Viewer& v) {
        QJsonObject o{{"t", "hello"}};
        {
            std::lock_guard lock(m);
            o["fps"] = QJsonArray{mode.fps.num, mode.fps.den};
            o["sequence"] = double(sequence);
        }
        {
            std::lock_guard lock(v.m);
            o["color"] = QString::fromStdString(v.color);
        }
        o["codec"] = QString::fromStdString(v.codec);
        sendText(v, o);
    }

    // Spends from a viewer's budgets (refilled at their rates); false when there is not enough.
    static bool spend(Viewer& v, double markers, double drawing, double pointers) {
        std::lock_guard lock(v.m);
        const auto now = Clock::now();
        const double s = std::chrono::duration<double>(now - v.budgetAt).count();
        v.budgetAt = now;
        v.markerBudget = std::min<double>(kMarkerBurst, v.markerBudget + s * kMarkerRate);
        v.drawingBudget = std::min<double>(double(kDrawingBurst), v.drawingBudget + s * double(kDrawingRate));
        v.pointerBudget = std::min<double>(kPointerRate, v.pointerBudget + s * kPointerRate);
        if (v.markerBudget < markers || v.drawingBudget < drawing || v.pointerBudget < pointers) return false;
        v.markerBudget -= markers;
        v.drawingBudget -= drawing;
        v.pointerBudget -= pointers;
        return true;
    }

    // What a viewer's page sent: who they are, a note, or where they point.
    void message(const std::shared_ptr<Viewer>& v, const std::string& text) {
        if (text.size() > size_t(kMaxBody)) return;
        const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
        const QString t = o.value("t").toString();
        if (t == "hi") {
            QString name = o.value("author").toString().simplified().left(100);
            std::lock_guard lock(v->m);
            if (!name.isEmpty()) v->author = name.toStdString();
            return;
        }
        if (t == "note") {
            const QString id = o.value("id").toString().left(100);
            const QJsonArray strokes = o.value("drawing").toArray();
            const std::string drawing = strokes.isEmpty() ? std::string() : reviewDrawingJson(parseReviewDrawing(QJsonDocument(strokes).toJson(QJsonDocument::Compact).toStdString()));
            if (!spend(*v, 1, double(drawing.size()), 0)) {
                sendText(*v, {{"t", "err"}, {"id", id}, {"why", "Too many notes at once: send them more slowly"}});
                return;
            }
            RemoteMarker r;
            r.frame = FrameTime(o.value("f").toInteger(-1));
            r.duration = std::max<FrameTime>(0, FrameTime(o.value("d").toInteger(0)));
            r.sequence = Id(o.value("seq").toInteger(0));
            r.timecode = o.value("tc").toString().left(32).toStdString();
            r.text = o.value("text").toString().left(10000).toStdString();
            r.drawing = drawing;
            r.id = id.toStdString();
            {
                std::lock_guard lock(v->m);
                r.author = v->author;
            }
            if (r.frame < 0 || (r.text.empty() && r.drawing.empty())) {
                sendText(*v, {{"t", "err"}, {"id", id}, {"why", "A note needs a frame, and words or a drawing"}});
                return;
            }
            {
                std::lock_guard lock(m);
                received.push_back(std::move(r));
                ++markerCount;
            }
            sendText(*v, {{"t", "ack"}, {"id", id}});
            return;
        }
        if (t == "p") {
            if (!spend(*v, 0, 0, 1)) return;
            const bool off = o.value("off").toBool();
            const double x = std::clamp(o.value("x").toDouble(), 0.0, 1.0), y = std::clamp(o.value("y").toDouble(), 0.0, 1.0);
            QJsonObject relay{{"t", "p"}, {"id", QString::fromStdString(v->id.substr(0, 8))}};
            std::vector<std::shared_ptr<Viewer>> others;
            {
                std::lock_guard lock(m);
                if (off) {
                    pointing.erase(v->id);
                } else {
                    std::lock_guard vl(v->m);
                    pointing[v->id] = {{v->author, v->color, x, y}, Clock::now()};
                    relay["who"] = QString::fromStdString(v->author);
                    relay["c"] = QString::fromStdString(v->color);
                }
                for (const auto& [id, w] : viewers)
                    if (w != v) others.push_back(w);
            }
            if (off) relay["off"] = true;
            else relay["x"] = x, relay["y"] = y;
            for (const auto& w : others) sendText(*w, relay);
        }
    }

    // (Callbacks reset first, each waiting for one already running, so none reaches this backend once it is gone:
    // the connection's, then its channel's, which only the connection's could still have set.)
    void closeViewer(const std::shared_ptr<Viewer>& v) {
        v->gone = true;
        try {
            v->pc->resetCallbacks();
            std::shared_ptr<rtc::DataChannel> channel;
            {
                std::lock_guard lock(v->m);
                channel = v->channel;
            }
            if (channel) channel->resetCallbacks();
            if (v->video) v->video->resetCallbacks();
            if (v->audio) v->audio->resetCallbacks();
            v->pc->close();
        } catch (const std::exception&) {
        }
        std::lock_guard lock(m);
        retired.emplace_back(Clock::now(), v);
    }

    // Viewers whose connection failed or closed, or never came up, are let go.
    void pruneViewers() {
        std::vector<std::shared_ptr<Viewer>> drop, gone;
        {
            std::lock_guard lock(m);
            const auto now = Clock::now();
            while (!retired.empty() && now - retired.front().first > std::chrono::seconds(5)) {
                gone.push_back(std::move(retired.front().second));
                retired.pop_front();
            }
            for (auto it = viewers.begin(); it != viewers.end();) {
                const auto& v = it->second;
                if (v->gone || (!v->connected && now - v->created > std::chrono::seconds(30))) {
                    drop.push_back(v);
                    pointing.erase(v->id);
                    it = viewers.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = pointing.begin(); it != pointing.end();) it = now - it->second.at > kPointerLife ? pointing.erase(it) : std::next(it);
        }
        for (const auto& v : drop) closeViewer(v);
    }

    // ---- Encoding ----

    VideoEncoder* encoder(const std::string& codec) {
        auto it = encoders.find(codec);
        if (it != encoders.end()) return it->second.get();
        if (encoderErrors.count(codec)) return nullptr;
        std::vector<std::string> names;
        if (codec == "h264") {
            if (settings.hardware) names.push_back("h264_nvenc");
            names.push_back("libx264");
        } else if (codec == "av1") {
            names = {"libsvtav1", "libaom-av1"};
        } else {
            if (settings.hardware) names.push_back("hevc_nvenc");
            names.push_back("libx265");
        }
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        std::string why = "This FFmpeg has no encoder for " + codec;
        for (const std::string& name : names) {
            const AVCodec* c = avcodec_find_encoder_by_name(name.c_str());
            if (!c) continue;
            AVCodecContext* e = avcodec_alloc_context3(c);
            e->width = mode.width;
            e->height = mode.height;
            e->time_base = AVRational{mode.fps.den, mode.fps.num};
            e->framerate = AVRational{mode.fps.num, mode.fps.den};
            e->pix_fmt = AV_PIX_FMT_YUV420P;
            e->gop_size = std::max(1, int(std::lround(fps * 4)));  // (and a key frame whenever a viewer joins or asks)
            e->max_b_frames = 0;
            e->bit_rate = int64_t(settings.bitrateKbps) * 1000;
            e->rc_max_rate = e->bit_rate;
            e->rc_buffer_size = int(e->bit_rate / 2);
            e->color_range = AVCOL_RANGE_MPEG;
            e->colorspace = AVCOL_SPC_BT709;
            e->color_primaries = AVCOL_PRI_BT709;
            e->color_trc = AVCOL_TRC_BT709;
            e->chroma_sample_location = AVCHROMA_LOC_LEFT;
            e->flags |= AV_CODEC_FLAG_LOW_DELAY;
            if (name.find("nvenc") != std::string::npos) {
                av_opt_set(e->priv_data, "preset", "p1", 0);
                av_opt_set(e->priv_data, "tune", "ll", 0);
                av_opt_set(e->priv_data, "rc", "cbr", 0);
                av_opt_set_int(e->priv_data, "zerolatency", 1, 0);
                av_opt_set_int(e->priv_data, "delay", 0, 0);
                av_opt_set_int(e->priv_data, "forced-idr", 1, 0);
                if (codec == "h264") av_opt_set(e->priv_data, "profile", "baseline", 0);
            } else if (name == "libx264") {
                av_opt_set(e->priv_data, "preset", "veryfast", 0);
                av_opt_set(e->priv_data, "tune", "zerolatency", 0);
                av_opt_set(e->priv_data, "profile", "baseline", 0);
                av_opt_set_int(e->priv_data, "forced-idr", 1, 0);
            } else if (name == "libx265") {
                av_opt_set(e->priv_data, "preset", "ultrafast", 0);
                av_opt_set(e->priv_data, "tune", "zerolatency", 0);
                av_opt_set(e->priv_data, "x265-params", "log-level=error:repeat-headers=1", 0);
                av_opt_set_int(e->priv_data, "forced-idr", 1, 0);
            } else if (name == "libsvtav1") {
                av_opt_set(e->priv_data, "preset", "11", 0);
                // (constant bit rate, which SVT-AV1 takes with its low-delay structure; it wants no maximum rate)
                av_opt_set(e->priv_data, "svtav1-params", "pred-struct=1:lookahead=0:scd=0:rc=2", 0);
                e->rc_max_rate = 0;
            } else if (name == "libaom-av1") {
                av_opt_set(e->priv_data, "usage", "realtime", 0);
                av_opt_set_int(e->priv_data, "cpu-used", 8, 0);  // (the most FFmpeg 6 takes)
                av_opt_set_int(e->priv_data, "lag-in-frames", 0, 0);
                av_opt_set_int(e->priv_data, "row-mt", 1, 0);
            }
            int r;
            {
                const KeepThreadScheduling keep;  // (SVT-AV1 would leave this thread real-time)
                r = avcodec_open2(e, c, nullptr);
            }
            if (r < 0) {
                why = "Cannot open " + name + ": " + averr(r);
                avcodec_free_context(&e);
                continue;
            }
            auto enc = std::make_unique<VideoEncoder>();
            enc->ctx = e;
            enc->pkt = av_packet_alloc();
            enc->name = name;
            VideoEncoder* out = enc.get();
            std::lock_guard lock(m);
            encoders[codec] = std::move(enc);
            return out;
        }
        encoderErrors[codec] = why;
        std::lock_guard lock(m);
        lastError = why;
        return nullptr;
    }

    bool openSound(std::string* error) {
        const AVCodec* c = avcodec_find_encoder_by_name("libopus");
        if (!c) {
            if (error) *error = "This FFmpeg has no Opus encoder (libopus)";
            return false;
        }
        aenc = avcodec_alloc_context3(c);
        aenc->sample_rate = 48000;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLT;
        av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->bit_rate = int64_t(std::clamp(settings.audioBitrateKbps, 32, 510)) * 1000;
        aenc->time_base = AVRational{1, 48000};
        av_opt_set(aenc->priv_data, "application", "lowdelay", 0);
        av_opt_set_double(aenc->priv_data, "frame_duration", 20, 0);
        if (const int r = avcodec_open2(aenc, c, nullptr); r < 0) {
            if (error) *error = "Cannot open the Opus encoder: " + averr(r);
            return false;
        }
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 48000);
        aframe = av_frame_alloc();
        aframe->format = AV_SAMPLE_FMT_FLT;
        aframe->sample_rate = 48000;
        av_channel_layout_default(&aframe->ch_layout, 2);
        aframe->nb_samples = aenc->frame_size > 0 ? aenc->frame_size : 960;
        av_frame_get_buffer(aframe, 0);
        apkt = av_packet_alloc();
        if (audioRate != 48000) {
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, 2);
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &stereo, AV_SAMPLE_FMT_FLT, audioRate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                if (error) *error = "Cannot resample the sound to 48 kHz";
                return false;
            }
        }
        return true;
    }

    void freeEncoders() {
        std::map<std::string, std::unique_ptr<VideoEncoder>> old;
        {
            std::lock_guard lock(m);
            old.swap(encoders);
        }
        old.clear();
        encoderErrors.clear();
        forcedKey.clear();
        av_frame_free(&picture);
        avcodec_free_context(&aenc);
        if (fifo) av_audio_fifo_free(fifo);
        fifo = nullptr;
        av_frame_free(&aframe);
        av_packet_free(&apkt);
        swr_free(&swr);
    }

    void counted(size_t size) {
        const auto now = Clock::now();
        std::lock_guard lock(m);
        bytes.emplace_back(now, size);
        while (!bytes.empty() && now - bytes.front().first > std::chrono::seconds(3)) bytes.pop_front();
    }

    // A picture's packet to the viewers on its codec: each one's tag first (its RTP timestamp, frame and timecode), then
    // the picture; a viewer waiting for a key frame gets nothing until one.
    void deliver(const std::string& codec, const AVPacket* pkt, const StreamTag& tag, const std::vector<std::shared_ptr<Viewer>>& list) {
        const bool key = pkt->flags & AV_PKT_FLAG_KEY;
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        const uint32_t ticks = uint32_t(int64_t(std::llround(double(pkt->pts) * 90000.0 / fps)) & 0xffffffff);
        bool sent = false;
        for (const auto& v : list) {
            if (v->codec != codec || !v->video || !v->video->isOpen()) continue;
            if (v->needKey) {
                if (!key) continue;
                v->needKey = false;
            }
            const uint32_t rtp = v->vcfg->startTimestamp + ticks;
            if (tag.frame >= 0)
                sendText(*v, {{"t", "f"}, {"r", double(rtp)}, {"f", double(tag.frame)}, {"tc", QString::fromStdString(tag.timecode)}, {"seq", double(tag.sequence)}});
            try {
                v->video->sendFrame(reinterpret_cast<const std::byte*>(pkt->data), size_t(pkt->size), rtc::FrameInfo(rtp));
                sent = true;
            } catch (const std::exception&) {
            }
        }
        if (sent) counted(size_t(pkt->size));
    }

    bool encodePicture(const VideoFrame& f, int64_t pts, const std::vector<std::shared_ptr<Viewer>>& list) {
        std::set<std::string> codecs;
        for (const auto& v : list)
            if (v->video && !v->codec.empty()) codecs.insert(v->codec);
        if (codecs.empty()) return true;
        Planes420 p = planesFrom(f);
        if (settings.burnTimecode) burnTimecode(p, streamTimecode(f.timecode));
        if (settings.frameCode) burnFrameCode(p, f.frame);
        if (!picture) {
            picture = av_frame_alloc();
            picture->format = AV_PIX_FMT_YUV420P;
            picture->width = mode.width;
            picture->height = mode.height;
            if (av_frame_get_buffer(picture, 0) < 0) return false;
        }
        if (av_frame_make_writable(picture) < 0) return false;
        const int w = mode.width, h = mode.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        auto b8 = [](uint16_t v) { return uint8_t(std::min(255, (v + 2) >> 2)); };
        for (int y = 0; y < h; ++y) {
            uint8_t* d = picture->data[0] + size_t(y) * size_t(picture->linesize[0]);
            for (int x = 0; x < w; ++x) d[x] = b8(p.y[size_t(y) * size_t(w) + size_t(x)]);
        }
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const size_t i = size_t(y) * size_t(cw) + size_t(x);
                picture->data[1][size_t(y) * size_t(picture->linesize[1]) + size_t(x)] = b8(p.u[i]);
                picture->data[2][size_t(y) * size_t(picture->linesize[2]) + size_t(x)] = b8(p.v[i]);
            }
        picture->pts = pts;
        StreamTag tag;
        tag.frame = f.frame;
        tag.timecode = streamTimecode(f.timecode);
        tag.fps = f.fps;
        {
            std::lock_guard lock(m);
            tag.sequence = sequence;
        }
        bool any = false;
        for (const std::string& codec : codecs) {
            VideoEncoder* enc = encoder(codec);
            if (!enc) continue;
            // A key frame for whoever waits for one (just joined, or lost pictures), at most twice a second whatever
            // they ask.
            bool key = false;
            for (const auto& v : list) key = key || (v->codec == codec && v->needKey);
            const auto now = Clock::now();
            auto last = forcedKey.find(codec);
            key = key && (last == forcedKey.end() || now - last->second >= std::chrono::milliseconds(500));
            if (key) forcedKey[codec] = now;
            picture->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            enc->tags[pts] = tag;
            while (enc->tags.size() > 64) enc->tags.erase(enc->tags.begin());
            if (avcodec_send_frame(enc->ctx, picture) < 0) continue;
            for (;;) {
                const int r = avcodec_receive_packet(enc->ctx, enc->pkt);
                if (r < 0) break;
                auto t = enc->tags.find(enc->pkt->pts);
                deliver(codec, enc->pkt, t == enc->tags.end() ? StreamTag{} : t->second, list);
                av_packet_unref(enc->pkt);
                any = true;
            }
        }
        picture->pict_type = AV_PICTURE_TYPE_NONE;
        return any;
    }

    void encodeSound(const Audio& a, int64_t pictureAt, const std::vector<std::shared_ptr<Viewer>>& list) {
        if (!aenc || a.frames <= 0) return;
        bool listening = false;
        for (const auto& v : list) listening |= v->audio && v->audio->isOpen();
        // Sound follows on from what went before, unless pictures were skipped (or it drifted) by more than two frames'
        // worth: then from where the picture is.
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        const int64_t want = std::llround(double(pictureAt) * 48000 / fps);
        const int64_t queued = apts + av_audio_fifo_size(fifo);
        if (std::llabs(queued - want) > int64_t(2 * 48000 / fps)) {
            av_audio_fifo_reset(fifo);
            apts = want;
        }
        std::vector<float> stereo(size_t(a.frames) * 2, 0.0f);
        if (a.samples && a.channels > 0)
            for (int i = 0; i < a.frames; ++i) {
                stereo[size_t(i) * 2] = a.samples[size_t(i) * size_t(a.channels)];
                stereo[size_t(i) * 2 + 1] = a.samples[size_t(i) * size_t(a.channels) + size_t(std::min(1, a.channels - 1))];
            }
        if (swr) {
            std::vector<float> out(size_t(swr_get_out_samples(swr, a.frames)) * 2 + 64);
            uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
            const uint8_t* in = reinterpret_cast<const uint8_t*>(stereo.data());
            const int n = swr_convert(swr, &o, int(out.size() / 2), &in, a.frames);
            out.resize(size_t(std::max(0, n)) * 2);
            stereo.swap(out);
        }
        void* planes[1] = {stereo.data()};
        av_audio_fifo_write(fifo, planes, int(stereo.size() / 2));
        while (av_audio_fifo_size(fifo) >= aframe->nb_samples) {
            if (av_frame_make_writable(aframe) < 0) return;
            av_audio_fifo_read(fifo, reinterpret_cast<void**>(aframe->data), aframe->nb_samples);
            aframe->pts = apts;
            apts += aframe->nb_samples;
            if (!listening) continue;  // (nobody to hear it: kept in time, not encoded)
            if (avcodec_send_frame(aenc, aframe) < 0) return;
            while (avcodec_receive_packet(aenc, apkt) >= 0) {
                const uint32_t ticks = uint32_t(apkt->pts & 0xffffffff);
                bool sent = false;
                for (const auto& v : list) {
                    if (!v->audio || !v->audio->isOpen()) continue;
                    try {
                        v->audio->sendFrame(reinterpret_cast<const std::byte*>(apkt->data), size_t(apkt->size), rtc::FrameInfo(v->acfg->startTimestamp + ticks));
                        sent = true;
                    } catch (const std::exception&) {
                    }
                }
                if (sent) counted(size_t(apkt->size));
                av_packet_unref(apkt);
            }
        }
    }
};

BrowserReviewBackend::BrowserReviewBackend() : d_(std::make_unique<Impl>()) {}

BrowserReviewBackend::~BrowserReviewBackend() { close(); }

bool BrowserReviewBackend::available(std::string* why) {
    if (!avcodec_find_encoder_by_name("libx264") && !avcodec_find_encoder_by_name("libsvtav1") && !avcodec_find_encoder_by_name("libaom-av1")) {
        if (why) *why = "This FFmpeg has no H.264 or AV1 encoder (libx264, libsvtav1 or libaom)";
        return false;
    }
    return true;
}

std::vector<Device> BrowserReviewBackend::devices() {
    Device d;
    d.id = "browser";
    d.name = "Browser review";
    d.formats = {PixelFormat::P216, PixelFormat::Uyvy8};
    d.hdr = false;
    d.audioChannels = {2};
    d.audioRates.clear();
    return {d};
}

void BrowserReviewBackend::setSettings(const StreamSettings& s) {
    std::lock_guard lock(d_->m);
    d_->settings = s;
}

StreamSettings BrowserReviewBackend::settings() const {
    std::lock_guard lock(d_->m);
    return d_->settings;
}

void BrowserReviewBackend::setSequence(Id id) {
    std::lock_guard lock(d_->m);
    d_->sequence = id;
}

bool BrowserReviewBackend::open(const Device&, const Mode& mode, const Signal& signal, int audioChannels, int audioRate, std::string* error) {
    close();
    Impl& d = *d_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        d.freeEncoders();
        return false;
    };
    std::string why = checkStreamSettings(d.settings);
    if (!why.empty()) return fail(why);
    if (!isBrowserAddress(d.settings.address)) return fail("A browser review's address is https://host:port");
    if (mode.width % 2 || mode.height % 2) return fail("Streams need an even width and height");
    if (signal.eotf != Eotf::Sdr) return fail("Browsers are sent the picture as the viewer shows it (Rec. 709), not HDR");
    std::string scheme, host;
    int port = 0;
    parseStreamAddress(d.settings.address, scheme, host, port);
    d.https = scheme == "https";
    d.host = host == "0.0.0.0" ? QHostAddress(QHostAddress::Any) : host == "::" ? QHostAddress(QHostAddress::Any) : QHostAddress(QString::fromStdString(host));
    if (d.host.isNull()) {
        const QHostInfo info = QHostInfo::fromName(QString::fromStdString(host));
        if (info.addresses().isEmpty()) return fail("Cannot find " + host);
        d.host = info.addresses().first();
    }
    d.port = port;
    if (d.https) {
        if (!QSslSocket::supportsSsl()) return fail("This Qt has no TLS (OpenSSL) for https");
        std::string certPem, keyPem;
        if (d.settings.certificateFile.empty()) {
            if (!makeReviewCertificate(reviewCertificateHosts(), certPem, keyPem, &why)) return fail(why);
        } else {
            QFile cf(QString::fromStdString(d.settings.certificateFile)), kf(QString::fromStdString(d.settings.keyFile));
            if (!cf.open(QIODevice::ReadOnly)) return fail("Cannot read " + d.settings.certificateFile);
            if (!kf.open(QIODevice::ReadOnly)) return fail("Cannot read " + d.settings.keyFile);
            certPem = cf.readAll().toStdString();
            keyPem = kf.readAll().toStdString();
        }
        // (the certificate first, then any chain after it, which browsers find themselves)
        const QList<QSslCertificate> chain = QSslCertificate::fromData(QByteArray::fromStdString(certPem), QSsl::Pem);
        if (chain.isEmpty()) return fail("The certificate is not one in PEM");
        d.certificate = chain.first();
        d.key = QSslKey(QByteArray::fromStdString(keyPem), QSsl::Ec, QSsl::Pem);
        if (d.key.isNull()) d.key = QSslKey(QByteArray::fromStdString(keyPem), QSsl::Rsa, QSsl::Pem);
        if (d.key.isNull()) return fail("The private key is not an EC or RSA key in PEM (without a password)");
    }
    d.iceServers.clear();
    for (const std::string& s : d.settings.iceServers) {
        try {
            d.iceServers.emplace_back(s);
        } catch (const std::exception& e) {
            return fail(std::string("ICE server ") + s + ": " + e.what());
        }
    }
    d.mode = mode;
    d.audioChannels = audioChannels;
    d.audioRate = audioRate > 0 ? audioRate : 48000;
    if (audioChannels > 0 && !d.openSound(&why)) return fail(why);
    {
        std::lock_guard lock(d.m);
        d.open = true;
        d.woken = false;
        d.lastError.clear();
        d.received.clear();
        d.markerCount = 0;
        d.pointing.clear();
        d.joined = 0;
        d.next = d.t0 = Clock::now();
        d.vpts = -1;
        d.apts = 0;
        d.encoded = d.skipped = 0;
        d.bytes.clear();
    }
    std::promise<std::string> started;
    std::future<std::string> result = started.get_future();
    d.http = std::thread([this, &started] { d_->run(started); });
    why = result.get();
    if (!why.empty()) {
        d.http.join();
        std::lock_guard lock(d.m);
        d.open = false;
        return fail(why);
    }
    return true;
}

bool BrowserReviewBackend::send(const VideoFrame& frame, const Audio& audio) {
    Impl& d = *d_;
    std::vector<std::shared_ptr<Viewer>> list;
    {
        std::lock_guard lock(d.m);
        if (!d.open || d.woken) return false;
        for (const auto& [id, v] : d.viewers)
            if (v->connected && !v->gone) list.push_back(v);
    }
    const double fps = d.mode.fps.toDouble() > 0 ? d.mode.fps.toDouble() : 25;
    // Pictures are stamped with the time they go out, so viewers see them in step with the clock whether or not
    // encoding keeps up (the frames it could not send are skipped).
    const int64_t due = std::llround(std::chrono::duration<double>(Clock::now() - d.t0).count() * fps);
    const int64_t pts = d.vpts < 0 || due > d.vpts + 2 ? std::max(due, d.vpts + 1) : d.vpts + 1;
    const int64_t skipped = d.vpts < 0 || list.empty() ? 0 : pts - d.vpts - 1;
    d.encodePicture(frame, pts, list);
    d.encodeSound(audio, pts, list);
    d.vpts = pts;
    {
        std::lock_guard lock(d.m);
        d.skipped += skipped;
        if (!list.empty()) ++d.encoded;
    }
    std::unique_lock lock(d.m);
    d.next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / std::max(1.0, frame.fps.toDouble())));
    const auto now = Clock::now();
    if (d.next < now - std::chrono::milliseconds(200)) d.next = now;  // (fallen far behind: not caught up in a burst)
    d.cv.wait_until(lock, d.next, [&] { return d.woken || !d.open; });
    return !d.woken && d.open;
}

void BrowserReviewBackend::close() {
    Impl& d = *d_;
    std::map<std::string, std::shared_ptr<Viewer>> viewers;
    {
        std::lock_guard lock(d.m);
        d.open = false;
        d.cv.notify_all();
        viewers.swap(d.viewers);
        d.pointing.clear();
        if (d.loop) QMetaObject::invokeMethod(d.loop, &QEventLoop::quit, Qt::QueuedConnection);
    }
    for (const auto& [id, v] : viewers) d.closeViewer(v);
    if (d.http.joinable()) d.http.join();
    {
        // (any an offer still being answered added meanwhile)
        std::lock_guard lock(d.m);
        viewers = std::exchange(d.viewers, {});
    }
    for (const auto& [id, v] : viewers) d.closeViewer(v);
    std::deque<std::pair<Clock::time_point, std::shared_ptr<Viewer>>> retired;
    {
        std::lock_guard lock(d.m);
        retired.swap(d.retired);
    }
    retired.clear();
    d.freeEncoders();
}

void BrowserReviewBackend::wake() {
    std::lock_guard lock(d_->m);
    d_->woken = true;
    d_->cv.notify_all();
}

int BrowserReviewBackend::receivers() {
    std::lock_guard lock(d_->m);
    int n = 0;
    for (const auto& [id, v] : d_->viewers) n += v->connected && !v->gone;
    return d_->open ? n : 0;
}

ReviewStats BrowserReviewBackend::stats() const {
    std::lock_guard lock(d_->m);
    ReviewStats s;
    for (const auto& [id, v] : d_->viewers) s.viewers += v->connected && !v->gone;
    if (!d_->open) s.viewers = 0;
    s.connected = s.viewers > 0;
    s.encoded = d_->encoded;
    s.skipped = d_->skipped;
    size_t total = 0;
    for (const auto& b : d_->bytes) total += b.second;
    if (d_->bytes.size() > 1) {
        const double span = std::chrono::duration<double>(d_->bytes.back().first - d_->bytes.front().first).count();
        if (span > 0.2) s.kbps = double(total) * 8 / 1000 / span;
    }
    for (const auto& [codec, e] : d_->encoders) s.encoder += (s.encoder.empty() ? "" : ", ") + e->name;
    s.error = d_->lastError;
    s.markers = d_->markerCount;
    return s;
}

std::vector<RemoteMarker> BrowserReviewBackend::takeMarkers() {
    std::lock_guard lock(d_->m);
    return std::exchange(d_->received, {});
}

std::vector<ReviewPointer> BrowserReviewBackend::pointers() const {
    std::lock_guard lock(d_->m);
    std::vector<ReviewPointer> out;
    const auto now = Clock::now();
    for (const auto& [id, p] : d_->pointing)
        if (now - p.at <= kPointerLife) out.push_back(p.p);
    return out;
}

#endif

}  // namespace montage::output
