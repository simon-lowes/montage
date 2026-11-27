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
#include <optional>
#include <set>
#include <thread>
#include <tuple>

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
#include <juice/juice.h>
#include <rtc/rtc.hpp>

#include "core/ReviewPage.h"
#include "media/Image.h"
#include "media/ThreadScheduling.h"
#include "render/ColorSpace.h"
#include "render/ReviewCongestion.h"
#include "render/ReviewGate.h"
#include "render/TurnRelay.h"

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

// Down to the rung that fits what got through (at least one down), or staying when none below works. A try or a
// probe under way has failed, and the next waits twice as long.
int RungSteering::down(int rung, int last, Clock::time_point now, const std::function<bool(int)>& tooBig,
                       const std::function<bool(int)>& usable) {
    if (trying || probing) hold = std::min(hold * 2, std::chrono::milliseconds(32000));
    trying = probing = false;
    int to = std::min(rung + 1, last);
    while (to < last && tooBig(to)) ++to;
    while (to > rung && !usable(to)) --to;
    if (to > rung) {
        movedAt = now;
        nextTry = now + hold;
        postponed = false;
        return to;
    }
    nextTry = std::max(nextTry, now + hold);  // (the smallest already, or none below works)
    return rung;
}

int RungSteering::report(int rung, int last, double loss, Clock::time_point at, Clock::time_point now,
                         const std::function<bool(int)>& tooBig, const std::function<bool(int)>& usable) {
    using std::chrono::milliseconds;
    // A try that has lasted 20 s without heavy loss fits (a link that buffers a long way brings its loss late).
    if (trying && now - triedAt > std::chrono::seconds(20)) {
        trying = false;
        hold = milliseconds(8000);
    }
    if (at - movedAt < milliseconds(700)) return rung;  // (it counts the rung before's packets)
    if (loss > 0.10) return down(rung, last, now, tooBig, usable);
    if (loss >= 0.02 && !postponed) {
        nextTry = std::max(nextTry, now + hold);
        postponed = true;
    }
    if (rung > 0 && now >= nextTry && !probing && usable(rung - 1)) {
        if (probes) {
            probing = true;
            return rung;
        }
        trying = true;
        triedAt = now;
        movedAt = now;
        nextTry = now + hold;
        postponed = false;
        return rung - 1;
    }
    return rung;
}

int RungSteering::delayed(int rung, int last, Clock::time_point now, const std::function<bool(int)>& tooBig,
                          const std::function<bool(int)>& usable) {
    if (trying && now - triedAt > std::chrono::seconds(20)) {
        trying = false;
        hold = std::chrono::milliseconds(8000);
    }
    if (now - movedAt < std::chrono::milliseconds(700)) return rung;
    return down(rung, last, now, tooBig, usable);
}

int RungSteering::probed(int rung, bool arrived, Clock::time_point now, const std::function<bool(int)>& usable) {
    if (!probing) return rung;
    probing = false;
    if (arrived && rung > 0 && usable(rung - 1)) {
        trying = true;
        triedAt = now;
        movedAt = now;
        nextTry = now + hold;
        postponed = false;
        return rung - 1;
    }
    hold = std::min(hold * 2, std::chrono::milliseconds(32000));
    nextTry = now + hold;
    return rung;
}

void RungSteering::switched(Clock::time_point now) {
    movedAt = now;
    nextTry = std::max(nextTry, now + hold);
    if (trying) triedAt = now;
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

const std::string kColourSpaceExtension = "http://www.webrtc.org/experiments/rtp-hdrext/color-space";

using Clock = std::chrono::steady_clock;

constexpr qsizetype kMaxHeader = 16 * 1024, kMaxBody = 256 * 1024;  // (an offer is a few kilobytes)
constexpr int kMaxConnections = 64, kMaxConnectionsFrom = 8;  // (all, and from one address or IPv6 /64)
constexpr int kHandshakeMs = 5000;                            // a TLS handshake not done by then is dropped
constexpr int kMarkerRate = 10, kMarkerBurst = 50;  // as the marker channel's (render/RemoteReview.cpp)
constexpr qsizetype kDrawingRate = 256 * 1024, kDrawingBurst = 4 * 1024 * 1024;
constexpr int kPointerRate = 30;
constexpr size_t kMaxChannelBacklog = 256 * 1024;  // bytes queued on a viewer's data channel before more are dropped
constexpr std::chrono::seconds kPointerLife{4};
constexpr std::chrono::milliseconds kProbeLength{700};  // how long padding is sent at the rung above's rate
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

// The network around it: an IPv4 address's /24, an IPv6 address's /48 (a site's, which holds many /64s).
QString network(const QHostAddress& a) {
    bool v4 = false;
    const quint32 ip4 = a.toIPv4Address(&v4);
    if (v4) return QHostAddress(ip4 & 0xffffff00u).toString() + "/24";
    if (a.protocol() != QAbstractSocket::IPv6Protocol) return a.toString();
    Q_IPV6ADDR ip = a.toIPv6Address();
    for (int i = 6; i < 16; ++i) ip[i] = 0;
    return QHostAddress(ip).toString() + "/48";
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
    std::string codec;  // "h264", "av1", "vp9", "hevc", "opus"
    std::string fmtp;
};

// (`tenBit`: one that carries 10-bit pictures: VP9's profile 2, HEVC's Main 10)
Choice choose(const rtc::Description::Media& m, const std::vector<std::string>& order, bool tenBit = false) {
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
            if (tenBit && want != "vp9" && want != "hevc") continue;
            if (want == "h264" && format == "h264") {
                // Packetised in fragments (mode 1); constrained baseline best, as x264 makes it, then baseline.
                if (!f.contains("packetization-mode=1")) continue;
                score = f.contains("profile-level-id=42e0") ? 3 : f.contains("profile-level-id=4200") ? 2 : 1;
            } else if (want == "av1" && format == "av1") {
                if (f.contains("profile=") && !f.contains("profile=0")) continue;  // (8-bit 4:2:0 is profile 0)
                score = 1;
            } else if (want == "vp9" && format == "vp9") {
                // (profile 0: 8-bit 4:2:0; profile 2: 10-bit)
                if (tenBit ? !f.contains("profile-id=2") : f.contains("profile-id=") && !f.contains("profile-id=0")) continue;
                score = 1;
            } else if (want == "hevc" && format == "h265") {
                // (Main, or for 10-bit Main 10)
                if (tenBit ? !f.contains("profile-id=2") : f.contains("profile-id=") && !f.contains("profile-id=1")) continue;
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
    if (!X509_gmtime_adj(X509_getm_notBefore(x.get()), -3600) || !X509_gmtime_adj(X509_getm_notAfter(x.get()), 2L * 365 * 24 * 3600))
        return fail("no validity period");
    if (!X509_set_pubkey(x.get(), key.get())) return fail("cannot put the key in the certificate");
    // (The name is made apart and then set: OpenSSL 4 gives a certificate's own name back read-only.)
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(X509_NAME_new(), X509_NAME_free);
    if (!name) return fail("no certificate name");
    if (!X509_NAME_add_entry_by_txt(name.get(), "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("Montage"), -1, -1, 0)
        || !X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("Montage live review"), -1, -1, 0))
        return fail("no certificate name");
    if (!X509_set_subject_name(x.get(), name.get()) || !X509_set_issuer_name(x.get(), name.get())) return fail("cannot name the certificate");
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
        const int added = X509_add_ext(x.get(), e, -1);
        X509_EXTENSION_free(e);
        if (!added) return fail("cannot add its extensions");
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
    bool hdr = false;   // sent 10-bit HDR (else SDR)
    std::shared_ptr<class Vp9Packetizer> vp9;  // VP9's, told each picture's size
    std::atomic<bool> needKey{true};  // nothing but a key frame goes to them next
    std::atomic<int64_t> lastKeyAsk{0};  // when they last asked for one (ms on the steady clock)
    std::atomic<bool> connected{false}, gone{false};
    // The rung they are sent, and the one they are moving to (sent from its next key frame on).
    std::atomic<int> rung{0}, wantRung{0};
    Clock::time_point created = Clock::now();
    std::mutex m;  // the members below
    std::shared_ptr<rtc::DataChannel> channel;
    std::string author = "Reviewer", color = kPalette[0];
    double markerBudget = kMarkerBurst, drawingBudget = double(kDrawingBurst), pointerBudget = kPointerRate;
    Clock::time_point budgetAt = Clock::now();
    // Their receiver reports on the picture: how many, and the share of packets lost in the last.
    uint64_t reports = 0;
    double loss = 0;
    Clock::time_point reportAt;
    // (send's thread) How the ladder steers them: reports read, when they last moved, when they may next try the rung
    // above, how long they wait between tries (doubled each time one fails), and what was sent them lately.
    uint64_t seenReports = 0;
    RungSteering steering;
    int64_t lastPts = -1;
    std::deque<std::pair<Clock::time_point, size_t>> sent;
    // The queue on the way to them, where their browser reports when packets arrive (a lock of its own: written by
    // the connection's threads, read by send's), and the padding a probe adds after each picture.
    std::mutex linkM;
    LinkDelay link;
    std::shared_ptr<class Padder> padder;
    // Only through the relay: this viewer's way there (render/TurnRelay.h), which outlives the connection. With a
    // relay reached over TCP or TLS that the review is not only through (`beside`), a second way to Montage's end
    // beside its own addresses, for a viewer on that relay.
    std::shared_ptr<RelayBridge> bridge;
    bool beside = false;
    bool padding = false;  // (send's thread) a probe's padding goes after their pictures
};

// Padding after a picture's packets, for a probe: as many bytes as were asked for the next picture, in packets of the
// same stream with nothing but padding, which a receiver counts and drops.
class Padder final : public rtc::MediaHandler {
public:
    explicit Padder(std::shared_ptr<rtc::RtpPacketizationConfig> config) : config_(std::move(config)) {}
    std::atomic<int> bytes{0};
    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        const int wanted = bytes.exchange(0);
        if (wanted <= 0 || messages.empty() || !messages.back() || messages.back()->type != rtc::Message::Binary) return;
        const rtc::message_ptr last = messages.back();
        uint16_t sequence = config_->sequenceNumber;
        for (const std::vector<uint8_t>& p : paddingAfter(reinterpret_cast<const uint8_t*>(last->data()), last->size(), sequence, size_t(wanted))) {
            auto m = rtc::make_message(reinterpret_cast<const std::byte*>(p.data()), reinterpret_cast<const std::byte*>(p.data()) + p.size());
            m->dscp = last->dscp;
            messages.push_back(std::move(m));
        }
        config_->sequenceNumber = sequence;
    }

private:
    std::shared_ptr<rtc::RtpPacketizationConfig> config_;
};

// VP9 over RTP (RFC 9628) in flexible mode: one spatial and temporal layer, a 15-bit picture ID, each inter picture
// referring to the one before, and the scalability structure (the picture's size) on key frames.
class Vp9Packetizer final : public rtc::RtpPacketizer {
public:
    explicit Vp9Packetizer(std::shared_ptr<rtc::RtpPacketizationConfig> config)
        : rtc::RtpPacketizer(std::move(config)), picture_(uint16_t(QRandomGenerator::system()->bounded(0x8000))) {}
    std::atomic<int> width{0}, height{0};

protected:
    std::vector<rtc::binary> fragment(rtc::binary data) override {
        if (data.empty()) return {};
        const bool key = keyFrame(data);
        picture_ = uint16_t((picture_ + 1) & 0x7fff);
        const size_t room = DefaultMaxFragmentSize - 12;  // (its RTP header and the descriptor's own bytes below)
        std::vector<rtc::binary> out;
        for (size_t at = 0; at < data.size();) {
            const bool first = at == 0;
            rtc::binary p;
            // I P L F B E V Z: a picture ID; inter-predicted unless a key frame; no layer indices; flexible mode;
            // beginning and end of the picture; the scalability structure on a key frame's first packet.
            p.push_back(std::byte((1 << 7) | (key ? 0 : 1 << 6) | (1 << 4) | (first ? 1 << 3 : 0) | (key && first ? 1 << 1 : 0)));
            p.push_back(std::byte(0x80 | (picture_ >> 8)));  // (M: the 15-bit form)
            p.push_back(std::byte(picture_ & 0xff));
            if (!key) p.push_back(std::byte(1 << 1));  // P_DIFF 1 (the picture before), N 0 (no more)
            if (key && first) {
                p.push_back(std::byte(1 << 4));  // N_S 0 (one spatial layer), Y (its size), G 0 (no group description)
                for (int v : {width.load(), height.load()}) {
                    p.push_back(std::byte((v >> 8) & 0xff));
                    p.push_back(std::byte(v & 0xff));
                }
            }
            const size_t n = std::min(room, data.size() - at);
            p.insert(p.end(), data.begin() + ptrdiff_t(at), data.begin() + ptrdiff_t(at + n));
            at += n;
            if (at == data.size()) p[0] |= std::byte(1 << 2);  // E
            out.push_back(std::move(p));
        }
        return out;
    }

private:
    // A key frame's uncompressed header: frame marker, profile (a reserved bit for profile 3), not an existing frame
    // shown, frame type 0.
    static bool keyFrame(const rtc::binary& d) {
        const unsigned b = unsigned(d[0]);
        if ((b >> 6) != 2) return false;
        const int profile = int(((b >> 5) & 1) | (((b >> 4) & 1) << 1));
        int bit = profile == 3 ? 2 : 3;
        if ((b >> bit) & 1) return false;  // show_existing_frame
        return ((b >> (bit - 1)) & 1) == 0;
    }
    uint16_t picture_;
};

// A viewer's reports on their picture: the share of packets lost (in RTCP sender and receiver reports) and, with the
// transport-wide sequence number extension agreed (`wideId`, 0 without), when each packet arrived. Every picture packet
// sent is numbered here on its way out, last in the chain. One sent again on a viewer's asking goes without a number
// (what is kept for sending again is kept before this, and sent again past it): it is not judged, and the one the link
// lost stays lost. That is as wanted: a probe judges what share of its first sending the link carried, and the share
// of the first sending that arrives is the same whether what is sent again is counted with it or not.
class FeedbackReader final : public rtc::MediaHandler {
public:
    FeedbackReader(std::weak_ptr<Viewer> viewer, uint32_t ssrc, int wideId) : viewer_(std::move(viewer)), ssrc_(ssrc), wideId_(wideId) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (const auto& message : messages) {
            if (!message || message->type != rtc::Message::Control) continue;  // (RTCP only)
            const auto* d = reinterpret_cast<const uint8_t*>(message->data());
            const size_t size = message->size();
            for (size_t at = 0; at + 8 <= size;) {
                if ((d[at] >> 6) != 2) break;
                const int count = d[at] & 31, type = d[at + 1];
                const size_t length = (size_t((d[at + 2] << 8) | d[at + 3]) + 1) * 4;
                if (at + length > size) break;
                if (type == 200 || type == 201) {
                    for (size_t b = at + (type == 200 ? 28 : 8), i = 0; i < size_t(count) && b + 24 <= at + length; ++i, b += 24) {
                        const uint32_t ssrc = (uint32_t(d[b]) << 24) | (uint32_t(d[b + 1]) << 16) | (uint32_t(d[b + 2]) << 8) | d[b + 3];
                        if (ssrc != ssrc_) continue;
                        if (auto v = viewer_.lock()) {
                            std::lock_guard lock(v->m);
                            v->loss = d[b + 4] / 256.0;
                            v->reportAt = Clock::now();
                            ++v->reports;
                        }
                    }
                } else if (type == 205 && count == 15 && wideId_) {
                    // (read only as far as what was sent them allows: a report of a few bytes saying 65,535 packets
                    // were lost is not walked through, under the lock the sending shares)
                    if (auto v = viewer_.lock()) {
                        size_t room = 0;
                        {
                            std::lock_guard lock(v->linkM);
                            room = v->link.reportRoom();
                        }
                        ArrivalReport report;
                        if (parseTransportFeedback(d + at, length, report, room)) {
                            std::lock_guard lock(v->linkM);
                            v->link.feedback(report, Clock::now());
                        }
                    }
                }
                at += length;
            }
        }
    }
    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        if (!wideId_) return;
        auto v = viewer_.lock();
        if (!v) return;
        const auto now = Clock::now();  // (one moment for a picture's packets: they are judged together)
        std::lock_guard lock(v->linkM);
        for (rtc::message_ptr& message : messages) {
            if (!message || message->type != rtc::Message::Binary) continue;
            const auto* d = reinterpret_cast<const uint8_t*>(message->data());
            const std::vector<uint8_t> numbered = withTransportSequence(d, message->size(), wideId_, v->link.nextSequence());
            if (numbered.empty()) continue;  // (not a packet that can carry the number: sent as it is, uncounted)
            v->link.sent(numbered.size(), now);
            auto m = rtc::make_message(reinterpret_cast<const std::byte*>(numbered.data()), reinterpret_cast<const std::byte*>(numbered.data()) + numbered.size());
            m->dscp = message->dscp;
            message = std::move(m);
        }
    }

private:
    std::weak_ptr<Viewer> viewer_;
    uint32_t ssrc_;
    int wideId_;
};

// An encoder for one codec, rung and depth.
struct EncoderKey {
    std::string codec;
    int rung = 0;
    bool hdr = false;
    bool operator<(const EncoderKey& o) const { return std::tie(codec, rung, hdr) < std::tie(o.codec, o.rung, o.hdr); }
};

struct VideoEncoder {
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* picture = nullptr;
    std::string name;
    std::map<int64_t, StreamTag> tags;  // by picture, until its packet comes out
    ~VideoEncoder() {
        // (ended properly, which SVT-AV1 asks for; what is left is dropped)
        if (ctx && pkt && avcodec_send_frame(ctx, nullptr) >= 0)
            while (avcodec_receive_packet(ctx, pkt) >= 0) av_packet_unref(pkt);
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
        av_frame_free(&picture);
    }
};

// One rung of the ladder: its size and bit rate.
struct Rung {
    int width = 0, height = 0;
    int64_t bitrate = 0;
};

// A picture coded from one encoder this tick.
struct Coded {
    EncoderKey key;
    std::vector<std::byte> data;
    int64_t pts = 0;
    bool keyFrame = false;
    StreamTag tag;
    int width = 0, height = 0;
};

// Y′CbCr codes (10-bit) of one colour space to those of BT.709 SDR, as the Program monitor shows them: a 33³ table,
// looked up trilinearly.
struct ToSdr {
    static constexpr int N = 33;
    std::vector<float> table;  // N³ × 3, Y′ Cb Cr codes
    void build(const ColorSpace& from, bool bt2020, double peakNits) {
        table.resize(size_t(N) * N * N * 3);
        const double kr = bt2020 ? 0.2627 : 0.2126, kb = bt2020 ? 0.0593 : 0.0722;
        const double kr7 = 0.2126, kb7 = 0.0722;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j)
                for (int k = 0; k < N; ++k) {
                    const double y = (i * 1023.0 / (N - 1) - 64) / 876, cb = (j * 1023.0 / (N - 1) - 512) / 896, cr = (k * 1023.0 / (N - 1) - 512) / 896;
                    const double r = y + 2 * (1 - kr) * cr, b = y + 2 * (1 - kb) * cb, g = (y - kr * r - kb * b) / (1 - kr - kb);
                    float rgb[3] = {float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(b, 0.0, 1.0))};
                    convertPixel(rgb, from, rec709Space(), peakNits);
                    const double R = std::clamp<double>(rgb[0], 0, 1), G = std::clamp<double>(rgb[1], 0, 1), B = std::clamp<double>(rgb[2], 0, 1);
                    const double Y = kr7 * R + (1 - kr7 - kb7) * G + kb7 * B;
                    float* o = &table[((size_t(i) * N + size_t(j)) * N + size_t(k)) * 3];
                    o[0] = float(64 + 876 * Y);
                    o[1] = float(512 + 896 * (B - Y) / (2 * (1 - kb7)));
                    o[2] = float(512 + 896 * (R - Y) / (2 * (1 - kr7)));
                }
    }
    void at(int y, int u, int v, float out[3]) const {
        const float s = float(N - 1) / 1023.0f;
        const float fy = std::clamp(float(y) * s, 0.0f, float(N - 1)), fu = std::clamp(float(u) * s, 0.0f, float(N - 1)),
                    fv = std::clamp(float(v) * s, 0.0f, float(N - 1));
        const int i = std::min(N - 2, int(fy)), j = std::min(N - 2, int(fu)), k = std::min(N - 2, int(fv));
        const float a = fy - float(i), b = fu - float(j), c = fv - float(k);
        for (int ch = 0; ch < 3; ++ch) {
            auto t = [&](int di, int dj, int dk) { return table[((size_t(i + di) * N + size_t(j + dj)) * N + size_t(k + dk)) * 3 + size_t(ch)]; };
            const float c00 = t(0, 0, 0) + (t(0, 0, 1) - t(0, 0, 0)) * c, c01 = t(0, 1, 0) + (t(0, 1, 1) - t(0, 1, 0)) * c;
            const float c10 = t(1, 0, 0) + (t(1, 0, 1) - t(1, 0, 0)) * c, c11 = t(1, 1, 0) + (t(1, 1, 1) - t(1, 1, 0)) * c;
            const float c0 = c00 + (c01 - c00) * b, c1 = c10 + (c11 - c10) * b;
            out[ch] = c0 + (c1 - c0) * a;
        }
    }
    // A whole picture (each chroma sample the mean of the four pixels it serves).
    void apply(Planes420& p) const {
        const int w = p.width, h = p.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        parallelRows(ch, [&](int y0, int y1) {
            for (int cy = y0; cy < y1; ++cy)
                for (int cx = 0; cx < cw; ++cx) {
                    const size_t ci = size_t(cy) * size_t(cw) + size_t(cx);
                    const int u = p.u[ci], v = p.v[ci];
                    float su = 0, sv = 0;
                    int n = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int x = 2 * cx + dx, y = 2 * cy + dy;
                            if (x >= w || y >= h) continue;
                            uint16_t& Y = p.y[size_t(y) * size_t(w) + size_t(x)];
                            float o[3];
                            at(Y, u, v, o);
                            Y = uint16_t(std::clamp(std::lround(o[0]), 0L, 1023L));
                            su += o[1], sv += o[2], ++n;
                        }
                    p.u[ci] = uint16_t(std::clamp(std::lround(su / float(n)), 0L, 1023L));
                    p.v[ci] = uint16_t(std::clamp(std::lround(sv / float(n)), 0L, 1023L));
                }
        });
    }
};

// A picture at a smaller size, each sample the mean of the box it covers.
Planes420 shrink(const Planes420& p, int w, int h) {
    if (w == p.width && h == p.height) return p;
    Planes420 out(w, h);
    auto plane = [](const std::vector<uint16_t>& in, int iw, int ih, std::vector<uint16_t>& o, int ow, int oh) {
        parallelRows(oh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int sy0 = int(int64_t(y) * ih / oh), sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * ih / oh));
                for (int x = 0; x < ow; ++x) {
                    const int sx0 = int(int64_t(x) * iw / ow), sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * iw / ow));
                    uint32_t sum = 0;
                    for (int sy = sy0; sy < sy1; ++sy)
                        for (int sx = sx0; sx < sx1; ++sx) sum += in[size_t(sy) * size_t(iw) + size_t(sx)];
                    const uint32_t n = uint32_t((sy1 - sy0) * (sx1 - sx0));
                    o[size_t(y) * size_t(ow) + size_t(x)] = uint16_t((sum + n / 2) / n);
                }
            }
        });
    };
    plane(p.y, p.width, p.height, out.y, w, h);
    plane(p.u, (p.width + 1) / 2, (p.height + 1) / 2, out.u, (w + 1) / 2, (h + 1) / 2);
    plane(p.v, (p.width + 1) / 2, (p.height + 1) / 2, out.v, (w + 1) / 2, (h + 1) / 2);
    return out;
}

// A UDP port held for viewers to share: libjuice's socket on it, which the viewers' connections then share, made
// before any connection of libdatachannel's, so with Windows' sockets started for it here (a connection starts them
// otherwise). Asked of libjuice itself, not through libdatachannel's listener, whose stop can be passed over (its flag
// of having stopped is never set to false first, so it holds whatever was in its memory): let go, the socket is closed
// before the destructor returns, unless a viewer's connection still shares it. Throws when the port cannot be had.
class HeldUdpPort {
public:
    HeldUdpPort(uint16_t port, const std::optional<std::string>& at) : port_(port) {
#ifdef Q_OS_WIN
        WSADATA data;
        winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
        if (juice_mux_listen(at ? at->c_str() : nullptr, port, &HeldUdpPort::unknown, nullptr) < 0) {
            release();
            throw std::runtime_error("Cannot listen on UDP port " + std::to_string(port));
        }
    }
    ~HeldUdpPort() {
        juice_mux_listen(nullptr, port_, nullptr, nullptr);
        release();
    }
    HeldUdpPort(const HeldUdpPort&) = delete;
    HeldUdpPort& operator=(const HeldUdpPort&) = delete;

private:
    // (a check from no viewer's connection: left unanswered)
    static void unknown(const juice_mux_binding_request_t*, void*) {}
    void release() {
#ifdef Q_OS_WIN
        if (winsock_) WSACleanup();
        winsock_ = false;
#endif
    }
    uint16_t port_;
#ifdef Q_OS_WIN
    bool winsock_ = false;
#endif
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
    bool relayInUse = false;  // a TURN relay among them, which Montage reaches too
    // Only through the relay: where it is, for the bridge each viewer is given (Montage's ICE is then given no server
    // at all), and the bridges of offers still waiting for the relay's answer. Not only through it, a relay reached
    // over TCP or TLS (`relayBeside`, which Montage's ICE cannot reach) is where a viewer on it is given a bridge too.
    RelayAddress relay;
    bool relayBeside = false;
    std::set<std::shared_ptr<RelayBridge>> bridging;
    // The relay's login for this review: the settings' own, or one made from the shared secret that runs out at
    // `expires` (seconds since 1970; 0: none), when the link ends too.
    TurnLogin relayLogin;
    std::atomic<int64_t> expires{0};
    // (the web server's thread) Once the link has ended: when its viewers were told so (and let go a second later).
    std::optional<Clock::time_point> endedAt;

    // The viewers' shared UDP port (the page's own number), held from when the review opens until it closes, so that
    // no other socket is given that number before the first viewer comes (a browser's own WebRTC, say, where the system
    // hands ports out one after another), which would leave every viewer unable to join. (None when each viewer has a
    // port of its own: through a relay Montage reaches, or only through the relay.)
    std::unique_ptr<HeldUdpPort> sharedPort;

    // The web server's thread, its loop and the object queued work runs on.
    std::thread http;
    QObject* home = nullptr;
    QEventLoop* loop = nullptr;
    PassphraseGate gate;  // (that thread's) who may have a passphrase tried now, by guesser() and network()

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
    std::optional<Clock::time_point> relayErrorAt;  // when lastError was last a viewer's way through the relay failing

    // Encoding (send's thread; the encoders map is changed under m, as stats() reads it).
    std::map<EncoderKey, std::unique_ptr<VideoEncoder>> encoders;
    std::map<EncoderKey, std::string> encoderErrors;
    std::map<EncoderKey, Clock::time_point> forcedKey;  // when each encoder was last made to start a key frame
    std::vector<Rung> ladder;  // full size first
    bool hdrSignal = false;    // the frames are PQ or HLG (BT.2020)
    Eotf eotf = Eotf::Sdr;
    std::unique_ptr<ToSdr> toSdr;  // for frames that are not BT.709 SDR
    uint16_t hdrWhite = 940;       // burnt-in text in HDR pictures: at HDR reference white
    AVCodecContext* aenc = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* apkt = nullptr;
    SwrContext* swr = nullptr;
    Clock::time_point t0, next;
    int64_t vpts = -1, apts = 0;
    int64_t encoded = 0, skipped = 0;
    std::atomic<int> probes{0}, queueMoves{0};  // probes begun, and moves down for a standing queue, since opened
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
        const QList<QSslSocket*> all = server.findChildren<QSslSocket*>();
        // (counted by IPv6 /64, as one computer can have many addresses in one)
        const QString from = guesser(c->peerAddress());
        const auto fromThere = std::count_if(all.begin(), all.end(), [&](QSslSocket* o) { return guesser(o->peerAddress()) == from; });
        if (fromThere > kMaxConnectionsFrom) {
            c->abort();
            c->deleteLater();
            return;
        }
        // (the places are for connections with something to wait for. One only waiting to be told its passphrase was
        // wrong holds none: guessers from many addresses, each waiting, keep nobody out. authorised() bounds those.)
        QList<QSslSocket*> open;
        for (QSslSocket* o : all)
            if (!o->property("refusing").toBool()) open.append(o);
        if (open.size() > kMaxConnections) {
            // Full. A reviewer who gave the passphrase lately still gets in, in the place of the oldest connection
            // still waiting to be answered from anyone else. Anyone else gets in by their share: in the place of the
            // oldest connection waiting from the address (IPv6 /64) holding the most, when that holds more than
            // theirs, so that a few addresses holding every place cannot keep a newcomer out. A newcomer from an
            // address holding none gets in even when every address holds one (many addresses of one network, as an
            // IPv6 /56 has 256 /64s, or of many): in the place of the oldest connection waiting from the network
            // holding the most, so that those of one network taking every place make room only for each other, and
            // anyone else's connection is the oldest only once all the places have been taken again after it.
            QSslSocket* oldest = nullptr;
            gate.tidy(Clock::now());  // (trust that has run out counts for nothing here either)
            if (gate.trusted(from)) {
                for (QSslSocket* o : open)
                    if (o != c && !o->property("answered").toBool() && !gate.trusted(guesser(o->peerAddress()))) {
                        oldest = o;
                        break;  // (children are listed oldest first)
                    }
            } else {
                // (of the addresses with a connection that could go: one waiting to be answered)
                std::map<QString, int> held, heldAround;
                std::set<QString> waiting;
                for (QSslSocket* o : open)
                    if (o != c) {
                        ++held[guesser(o->peerAddress())];
                        ++heldAround[network(o->peerAddress())];
                        if (!o->property("answered").toBool()) waiting.insert(guesser(o->peerAddress()));
                    }
                QString most;
                int mostHeld = 0;
                for (const auto& [who, n] : held)
                    if (n > mostHeld && waiting.count(who) && !gate.trusted(who)) most = who, mostHeld = n;
                const int mine = held[from];
                if (mostHeld > mine + 1)
                    for (QSslSocket* o : open)
                        if (o != c && !o->property("answered").toBool() && guesser(o->peerAddress()) == most) {
                            oldest = o;
                            break;
                        }
                if (!oldest && mine == 0) {
                    // (the oldest of those waiting from the networks holding the most: children are listed oldest first)
                    int mostAround = 0;
                    for (QSslSocket* o : open)
                        if (o != c && !o->property("answered").toBool() && !gate.trusted(guesser(o->peerAddress())))
                            if (const int n = heldAround[network(o->peerAddress())]; n > mostAround) oldest = o, mostAround = n;
                }
            }
            if (!oldest) {
                c->abort();
                c->deleteLater();
                return;
            }
            oldest->abort();
        }
        QObject::connect(c, &QAbstractSocket::disconnected, c, &QObject::deleteLater);
        QObject::connect(c, &QAbstractSocket::readyRead, c, [this, c] { read(c); });
        // (TLS errors and the like: the connection is simply dropped)
        QObject::connect(c, &QAbstractSocket::errorOccurred, c, [c] { c->abort(); });
        auto* idle = new QTimer(c);
        idle->setSingleShot(true);
        QObject::connect(idle, &QTimer::timeout, c, [c] { c->abort(); });
        idle->start(https ? kHandshakeMs : 20000);
        if (https) {
            // A handshake gets a few seconds; once it is done, the request gets the rest.
            // (the request follows at once: a connection that sends none is not kept holding its place)
            QObject::connect(c, &QSslSocket::encrypted, idle, [idle] { idle->start(kHandshakeMs); });
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
        if (auto* idle = c->findChild<QTimer*>()) idle->start(20000);  // (the answer may take a moment: a wrong passphrase's)
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

    // The passphrase as a bearer token, compared only when the gate lets it be tried (render/ReviewGate.h: an address
    // that guessed wrong too often is refused at once, each address and each guessing network may try only so often,
    // and a right passphrase uses up nobody's tries). Wrong ones are told a second after the one before from the same
    // address.
    bool authorised(QSslSocket* c, const Request& r) {
        const QString peer = guesser(c->peerAddress());
        const QString around = network(c->peerAddress());
        const auto now = Clock::now();
        const PassphraseGate::Verdict may = gate.ask(peer, around, now);
        if (may.answer == PassphraseGate::Answer::Blocked) {
            respond(c, "429 Too Many Requests", {}, "Too many wrong passphrases from this address: try again later");
            return false;
        }
        if (may.answer == PassphraseGate::Answer::AddressBusy) {
            respond(c, "429 Too Many Requests", {{"Retry-After", QByteArray::number(may.retryAfter)}}, "Too many requests from this address: try again shortly");
            return false;
        }
        if (may.answer == PassphraseGate::Answer::NetworkBusy) {
            respond(c, "429 Too Many Requests", {{"Retry-After", QByteArray::number(may.retryAfter)}},
                    "Too many wrong passphrases from this network: try again shortly");
            return false;
        }
        QByteArray given = r.header("authorization");
        if (given.toLower().startsWith("bearer ")) given = given.mid(7).trimmed();
        else given.clear();
        if (sameBytes(given, QByteArray::fromStdString(settings.passphrase))) {
            gate.right(peer, around, now, may);
            return true;
        }
        const int ms = gate.wrong(peer, around, now);
        // (only waiting to be told so, and holding no place meanwhile. No more than a set number wait at once: past
        // that, the one that has waited longest is told now.)
        c->setProperty("refusing", true);
        auto tell = [this](QSslSocket* o) {
            if (!o->property("refusing").toBool()) return;
            o->setProperty("refusing", false);
            respond(o, "401 Unauthorized", {{"WWW-Authenticate", "Bearer realm=\"Montage review\""}}, "Wrong passphrase");
        };
        QTimer::singleShot(ms, c, [tell, c] { tell(c); });
        if (c->parent()) {
            QList<QSslSocket*> waiting;
            for (QSslSocket* o : c->parent()->findChildren<QSslSocket*>())
                if (o->property("refusing").toBool()) waiting.append(o);
            if (waiting.size() > kMaxConnections) tell(waiting.front());  // (children are listed oldest first)
        }
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
            if (expired()) return respond(c, "410 Gone", {}, "This review link has expired");
            QJsonArray ice;
            for (const QJsonObject& s : browserIceServers()) ice.append(s);
            QJsonArray rungs;
            for (const Rung& g : ladder) rungs.append(QJsonArray{g.width, g.height});
            std::lock_guard lock(m);
            const QJsonObject o{{"iceServers", ice}, {"iceTransportPolicy", settings.relayOnly ? "relay" : "all"},
                                {"fps", QJsonArray{mode.fps.num, mode.fps.den}}, {"width", mode.width}, {"height", mode.height},
                                {"sequence", double(sequence)}, {"rungs", rungs},
                                {"hdr", !settings.hdr || !hdrSignal ? "" : eotf == Eotf::Pq ? "pq" : "hlg"}};
            return respond(c, "200 OK", {}, QJsonDocument(o).toJson(QJsonDocument::Compact), "application/json");
        }
        if (path == "/whep") {
            if (r.method != "POST") return respond(c, "405 Method Not Allowed", {{"Allow", "POST, OPTIONS"}}, {});
            if (!authorised(c, r)) return;
            if (expired()) return respond(c, "410 Gone", {}, "This review link has expired");
            if (!r.header("content-type").toLower().startsWith("application/sdp")) return respond(c, "415 Unsupported Media Type", {}, "An SDP offer, please");
            // (?hdr=1: the player decodes 10-bit HDR and wants it)
            const QByteArray query = r.path.mid(path.size() + 1);
            bool hdr = false;
            for (const QByteArray& kv : query.split('&')) hdr = hdr || kv == "hdr=1" || kv == "hdr=true";
            return offer(c, r.body, hdr);
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

    // The review link's time is up (only one with a relay's shared secret has a time): nobody new is let in, as the
    // relay lets no login from it in, and every viewer is told and let go (pruneViewers), as the relay would cut
    // those it carries at their next refresh with that login.
    bool expired() const {
        const int64_t at = expires;
        return at > 0 && std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() >= at;
    }

    // The STUN and TURN servers as browsers are given them: the settings' own, then the TURN relay with this review's
    // login (never the shared secret).
    std::vector<QJsonObject> browserIceServers() const {
        std::vector<QJsonObject> out;
        for (const std::string& s : settings.iceServers) out.push_back(browserIceServer(s));
        if (!settings.turnServer.empty())
            out.push_back({{"urls", QString::fromStdString(settings.turnServer)},
                           {"username", QString::fromStdString(relayLogin.user)},
                           {"credential", QString::fromStdString(relayLogin.password)}});
        return out;
    }

    // A viewer's offer: their connection made, answered once its candidates are gathered. Only through the relay,
    // the relay is first asked for an address for this viewer, and the offer answered from behind that bridge.
    void offer(QSslSocket* c, const QByteArray& sdp, bool hdr) {
        {
            // (offers still waiting for the relay count too: each holds a bridge and an address at the relay)
            std::lock_guard lock(m);
            if (int(viewers.size() + bridging.size()) >= settings.maxViewers)
                return respond(c, "503 Service Unavailable", {}, "As many viewers as allowed are watching");
        }
        // Not only through the relay, a viewer with an address at a relay reached over TCP or TLS is given a bridge
        // beside Montage's own addresses: Chrome's checks from such a relay give the controlling end's tiebreaker as
        // 0, which Montage's ICE refuses, and the bridge repairs them. (Only where Montage's end sends from this
        // computer's loopback to the bridge: bound to no one address, or to the loopback.)
        const bool beside = !settings.relayOnly && relayBeside && relayCandidates(sdp.toStdString()) > 0 &&
                            (host == QHostAddress::Any || host == QHostAddress::AnyIPv6 || host == QHostAddress::AnyIPv4 || (host.isLoopback() && !relayInUse));
        if (!settings.relayOnly && !beside) return answerOffer(c, sdp, hdr, nullptr);
        // (only through the relay, a viewer is reached at its address there: a browser that got none, as Firefox
        // gets none from a relay at ::1, is told so, not left to fail its checks)
        if (!beside && offerAddresses(sdp.toStdString()) == 0)
            return respond(c, "400 Bad Request", {}, "This browser got no address at the review's relay, so it cannot watch through it");
        auto bridge = std::make_shared<RelayBridge>(relay, relayLogin);
        {
            std::lock_guard lock(m);
            bridging.insert(bridge);
        }
        QPointer<QSslSocket> sock(c);
        std::weak_ptr<RelayBridge> wb = bridge;
        // (the bridge calls back on its own thread, where it must not be let go of: the rest is done here, at home)
        // (an address of the family the viewer's own addresses have: a relay carries IPv4 to IPv4 and IPv6 to IPv6)
        bridge->allocate(offerFamily(sdp.toStdString()), [this, sock, wb, sdp, hdr, beside](const std::string& error) {
            std::lock_guard lock(m);
            if (!home) return;
            QMetaObject::invokeMethod(
                home,
                [this, sock, wb, sdp, hdr, error, beside] {
                    std::shared_ptr<RelayBridge> bridge = wb.lock();
                    bool full = false;
                    {
                        std::lock_guard lock(m);
                        bridging.erase(bridge);
                        full = int(viewers.size()) >= settings.maxViewers;
                    }
                    if (!bridge || !sock) return;
                    if (full) return respond(sock, "503 Service Unavailable", {}, "As many viewers as allowed are watching");
                    // (beside Montage's own addresses, a relay that gives no address leaves the viewer those)
                    if (beside) return answerOffer(sock, sdp, hdr, error.empty() ? bridge : nullptr, true);
                    if (!error.empty()) return respond(sock, "502 Bad Gateway", {}, QByteArray::fromStdString(error));
                    answerOffer(sock, sdp, hdr, bridge);
                },
                Qt::QueuedConnection);
        });
    }

    void answerOffer(QSslSocket* c, const QByteArray& offered, bool hdr, const std::shared_ptr<RelayBridge>& bridge, bool beside = false) {
        // (behind a bridge, each address the viewer offers is a loopback port of the bridge's; beside one, each of
        // its addresses at a relay has a loopback port of the bridge's too)
        const auto alias = [&](const std::string& ip, uint16_t p) { return bridge->peer(ip, p); };
        const QByteArray sdp = !bridge ? offered
                                       : QByteArray::fromStdString(beside ? offerAlsoThroughRelay(offered.toStdString(), alias)
                                                                          : offerThroughRelay(offered.toStdString(), alias));
        // (only through the relay, none of the viewer's addresses may be of the family the relay gave Montage: a relay
        // with no IPv6 address for Montage carries nothing to a browser that has only IPv6 ones there; said at once,
        // not left to fail the checks half a minute on)
        if (bridge && !beside && offerAddresses(sdp.toStdString()) == 0)
            return respond(c, "502 Bad Gateway", {},
                           offerFamily(offered.toStdString()) == 6
                               ? "The review's relay gave Montage no IPv6 address, and this browser's addresses there are all IPv6, so it cannot watch through it"
                               : "None of this browser's addresses at the review's relay can be reached through Montage's address there");
        std::shared_ptr<Viewer> v;
        std::string why;
        try {
            v = makeViewer(sdp.toStdString(), c->peerAddress().toString(), hdr, why, bridge, beside);
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
            for (const QJsonObject& o : browserIceServers()) {
                QByteArray link = "<" + o.value("urls").toString().toUtf8() + ">; rel=\"ice-server\"";
                if (o.contains("username"))
                    link += "; username=\"" + o.value("username").toString().toUtf8() + "\"; credential=\"" + o.value("credential").toString().toUtf8() +
                            "\"; credential-type=\"password\"";
                headers.emplace_back("Link", link);
            }
            std::string answer = std::string(*local);
            if (v->bridge && v->beside) {
                // (Montage's end is given at its address at the relay as well as its own, and the bridge told where
                // that end is)
                uint16_t localPort = 0;
                const std::string both = answerAlsoThroughRelay(answer, v->bridge->relayedAddress(), v->bridge->relayedPort(), &localPort);
                if (localPort) {
                    answer = both;
                    v->bridge->setLocal(localPort, local->icePwd().value_or(std::string()));
                }
            } else if (v->bridge) {
                // (Montage's end is given as its address at the relay, and the bridge told where that end is)
                uint16_t localPort = 0;
                answer = answerThroughRelay(answer, v->bridge->relayedAddress(), v->bridge->relayedPort(), &localPort);
                if (!localPort) return respond(sock, "500 Internal Server Error", {}, "No answer");
                v->bridge->setLocal(localPort, local->icePwd().value_or(std::string()));
            }
            respond(sock, "201 Created", headers, QByteArray::fromStdString(answer), "application/sdp");
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

    std::shared_ptr<Viewer> makeViewer(const std::string& sdp, const QString& address, bool wantsHdr, std::string& why,
                                       const std::shared_ptr<RelayBridge>& bridge, bool beside = false) {
        const rtc::Description offered(sdp, rtc::Description::Type::Offer);
        auto v = std::make_shared<Viewer>();
        v->id = randomHex(16).toStdString();
        v->address = address;
        v->bridge = bridge;
        v->beside = bridge && beside;
        rtc::Configuration conf;
        conf.iceServers = iceServers;
        // Media on the same port number as the page (UDP), all viewers together: one port to open in a firewall. With
        // a TURN relay, each viewer's connection has a port of its own instead (libjuice's shared socket cannot reach
        // a relay), which only goes out to the relay and to the viewer.
        // Only through the relay, this end is on this computer's loopback alone, a port of its own, where the bridge
        // carries what it sends to the relay and it can reach nothing else. (Beside a bridge, a port of its own too,
        // where the bridge reaches it from the loopback.)
        if (bridge && !v->beside) {
            conf.bindAddress = "127.0.0.1";
        } else if (!relayInUse && !bridge) {
            conf.enableIceUdpMux = true;
            conf.portRangeBegin = conf.portRangeEnd = uint16_t(port);
        }
        // (bound to the review's own address; when that is this computer's loopback and a relay elsewhere must be
        // reached, unbound but through the relay only, so no other interface's address is offered)
        if ((!bridge || v->beside) && host != QHostAddress::Any && host != QHostAddress::AnyIPv6 && host != QHostAddress::AnyIPv4) {
            if (!relayInUse || !host.isLoopback()) conf.bindAddress = host.toString().toStdString();
            else conf.iceTransportPolicy = rtc::TransportPolicy::Relay;
        }
        v->pc = std::make_shared<rtc::PeerConnection>(conf);
        // Only codecs this FFmpeg can encode are offered back: a browser picked one it cannot would see no picture.
        std::vector<std::string> order;
        for (const std::string& c : {settings.codec, std::string("h264"), std::string("av1"), std::string("vp9"), std::string("hevc")})
            if (std::find(order.begin(), order.end(), c) == order.end() && canEncode(c)) order.push_back(c);
        // 10-bit HDR to a player that asks for it, of an HDR sequence, in VP9 profile 2 or HEVC Main 10 (the preferred
        // first), its transfer given by the colour space extension. (Not AV1: Chrome's WebRTC decodes 8-bit AV1 only,
        // whatever its media capabilities say.)
        std::vector<std::string> tenBit;
        if (wantsHdr && settings.hdr && hdrSignal)
            for (const std::string& c : {settings.codec, std::string("vp9"), std::string("hevc")})
                if ((c == "vp9" || c == "hevc") && std::find(tenBit.begin(), tenBit.end(), c) == tenBit.end() && canEncode(c, true)) tenBit.push_back(c);
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
            Choice pick;
            bool hdr = false;
            bool colourExtension = false;
            int wideId = 0;  // the transport-wide sequence number extension's ID, when its feedback is offered too
            rtc::Description::Media offeredMedia = media;  // (extIds is not const)
            // (an ID of 1-14 only: libdatachannel writes this extension's element in the one-byte form whatever its
            // ID, so one above 14 would arrive malformed and the picture's transfer lost; such a viewer gets SDR)
            for (int id : offeredMedia.extIds())
                if (const auto* e = offeredMedia.extMap(id); e && e->uri == kColourSpaceExtension && id >= 1 && id <= 14) colourExtension = true;
            if (isVideo && !tenBit.empty() && colourExtension) {
                pick = choose(media, tenBit, true);
                hdr = pick.pt >= 0;
            }
            if (pick.pt < 0) pick = choose(media, isVideo ? order : std::vector<std::string>{"opus"});
            if (pick.pt < 0) continue;
            const uint32_t ssrc = QRandomGenerator::system()->generate() | 1u;
            auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "montage", uint8_t(pick.pt), isVideo ? 90000 : 48000);
            std::unique_ptr<rtc::Description::Media> answer;
            if (isVideo) {
                auto d = std::make_unique<rtc::Description::Video>(media.mid(), rtc::Description::Direction::SendOnly);
                const char* name = pick.codec == "h264" ? "H264" : pick.codec == "av1" ? "AV1" : pick.codec == "vp9" ? "VP9" : "H265";
                d->addVideoCodec(pick.pt, name, pick.fmtp.empty() ? std::nullopt : std::optional<std::string>(pick.fmtp));
                d->addSSRC(ssrc, "montage", "montage", "video");
                answer = std::move(d);
            } else {
                auto d = std::make_unique<rtc::Description::Audio>(media.mid(), rtc::Description::Direction::SendOnly);
                d->addOpusCodec(pick.pt, pick.fmtp.empty() ? std::nullopt : std::optional<std::string>(pick.fmtp));
                d->addSSRC(ssrc, "montage", "montage", "audio");
                answer = std::move(d);
            }
            // The extensions that say which medium a packet is (before SSRCs are known), ask for no playout delay, and
            // give the picture's colour (its primaries, transfer, matrix and range, as the codec's own headers do).
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
                } else if (isVideo && e->uri == kColourSpaceExtension && colourExtension) {
                    answer->addExtMap(rtc::Description::Media::ExtMap(id, e->uri));
                    cfg->colorSpaceId = uint8_t(id);
                    cfg->colorRange = 1;  // (limited)
                    // (H.273's codes: BT.2020 and PQ or HLG and BT.2020's non-constant luminance matrix, or BT.709's)
                    cfg->colorPrimaries = hdr ? 9 : 1;
                    cfg->colorTransfer = hdr ? (eotf == Eotf::Pq ? 16 : 18) : 1;
                    cfg->colorMatrix = hdr ? 9 : 1;
                    cfg->colorChromaSitingHorz = 1;  // (co-sited on the left)
                    cfg->colorChromaSitingVert = 2;  // (half way)
                } else if (isVideo && e->uri == kTransportWideExtension && id >= 1 && id <= 14 && !wideId) {
                    // The picture's packets numbered over the whole connection, and the viewer's reports of when each
                    // arrived: how a link that is slow without losing anything is noticed. (An ID of 1-14, as above.)
                    const rtc::Description::Media::RtpMap* offeredMap = media.rtpMap(pick.pt);
                    if (!offeredMap || std::find(offeredMap->rtcpFbs.begin(), offeredMap->rtcpFbs.end(), kTransportWideFeedback) == offeredMap->rtcpFbs.end())
                        continue;
                    answer->addExtMap(rtc::Description::Media::ExtMap(id, e->uri));
                    if (rtc::Description::Media::RtpMap* map = answer->rtpMap(pick.pt)) map->addFeedback(kTransportWideFeedback);
                    wideId = id;
                }
            }
            std::shared_ptr<rtc::Track> track = v->pc->addTrack(*answer);
            std::weak_ptr<Viewer> wv = v;
            const std::string codec = pick.codec;
            std::shared_ptr<rtc::RtpPacketizer> packetizer;
            if (codec == "h264") packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg);
            else if (codec == "hevc") packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg);
            else if (codec == "av1") packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, cfg);
            else if (codec == "vp9") packetizer = v->vp9 = std::make_shared<Vp9Packetizer>(cfg);
            else packetizer = std::make_shared<rtc::OpusRtpPacketizer>(cfg);
            packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(cfg));
            if (isVideo && wideId) {
                // (after the sender reports, which count pictures only; before what is kept for sending again, so
                // padding a viewer misses can be asked for like any packet and leaves no gap in their numbering)
                v->padder = std::make_shared<Padder>(cfg);
                packetizer->addToChain(v->padder);
            }
            // (a probe's padding is many small packets: room for them beside the pictures')
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(isVideo ? 2048 : rtc::RtcpNackResponder::DefaultMaxSize));
            if (isVideo) {
                // (the viewer's own flag only: the next picture sent starts a key frame for them, whoever else
                // watches on the same encoder)
                // At most one key frame a second on a viewer's asking: each costs every viewer on that encoder.
                packetizer->addToChain(std::make_shared<rtc::PliHandler>([wv] {
                    auto v = wv.lock();
                    if (!v) return;
                    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                    int64_t last = v->lastKeyAsk.load();
                    if (now - last < 1000 || !v->lastKeyAsk.compare_exchange_strong(last, now)) return;
                    v->needKey = true;
                }));
                packetizer->addToChain(std::make_shared<FeedbackReader>(wv, ssrc, wideId));
                track->onOpen([wv] {
                    if (auto v = wv.lock()) v->needKey = true;
                });
            }
            track->setMediaHandler(packetizer);
            if (isVideo) {
                v->video = track;
                v->vcfg = cfg;
                v->codec = codec;
                v->hdr = hdr;
            } else {
                v->audio = track;
                v->acfg = cfg;
            }
        }
        if (!v->video && !v->audio) {
            why = "The offer asks for no picture Montage can send (H.264 in packetisation mode 1, AV1, VP9 or HEVC) and no Opus sound";
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
        // A viewer that has stopped taking what is sent (SCTP not acknowledged) is sent nothing more until it catches up.
        if (!dc || !dc->isOpen() || dc->bufferedAmount() > kMaxChannelBacklog) return;
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
        o["hdr"] = v.hdr;
        sendText(v, o);
        rungMessage(v);
    }

    // Which rung a viewer is on now (its size), for their page.
    void rungMessage(Viewer& v) {
        const int r = std::clamp(v.rung.load(), 0, int(ladder.size()) - 1);
        if (ladder.empty()) return;
        sendText(v, {{"t", "rung"}, {"i", r}, {"of", int(ladder.size())}, {"w", ladder[size_t(r)].width}, {"h", ladder[size_t(r)].height}, {"hdr", v.hdr}});
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
        // (the address at the relay is given back now, unwaited: by the time the viewer is let go for good the
        // relay has answered or the moment for it has passed, so a relay gone quiet holds nobody up)
        if (v->bridge) v->bridge->letGo();
        std::lock_guard lock(m);
        retired.emplace_back(Clock::now(), v);
    }

    // Viewers whose connection failed or closed, or never came up, are let go; once the link's time is up, every
    // viewer is told so and let go a second later. A viewer whose way through the relay failed leaves why.
    void pruneViewers() {
        std::vector<std::shared_ptr<Viewer>> drop, gone, told;
        const bool ended = expired();
        {
            std::lock_guard lock(m);
            const auto now = Clock::now();
            while (!retired.empty() && now - retired.front().first > std::chrono::seconds(5)) {
                gone.push_back(std::move(retired.front().second));
                retired.pop_front();
            }
            if (ended && !endedAt) {
                endedAt = now;
                for (const auto& [id, v] : viewers) told.push_back(v);
            }
            const bool letGo = endedAt && now - *endedAt >= std::chrono::seconds(1);
            for (auto it = viewers.begin(); it != viewers.end();) {
                const auto& v = it->second;
                // (a viewer whose address at the relay is gone is let go at once: joining again gives them a new one)
                if (letGo || v->gone || (!v->connected && now - v->created > std::chrono::seconds(30)) || (v->bridge && !v->beside && v->bridge->ended())) {
                    if (v->bridge)
                        if (std::string why = v->bridge->lastError(); !why.empty()) lastError = why, relayErrorAt = now;
                    drop.push_back(v);
                    pointing.erase(v->id);
                    it = viewers.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = pointing.begin(); it != pointing.end();) it = now - it->second.at > kPointerLife ? pointing.erase(it) : std::next(it);
            // (a relay's refusal is the review's error until a viewer who came after it is carried through the relay)
            if (relayErrorAt)
                for (const auto& [id, v] : viewers)
                    if (v->connected && !v->gone && v->bridge && !v->beside && v->created > *relayErrorAt && v->bridge->lastError().empty()) {
                        lastError.clear();
                        relayErrorAt.reset();
                        break;
                    }
        }
        for (const auto& v : told) sendText(*v, {{"t", "end"}, {"why", "This review link has expired"}});
        for (const auto& v : drop) closeViewer(v);
    }

    // ---- Encoding ----

    // The encoders for a codec ("h264", "av1", "vp9", "hevc"), as encoder() tries them; `tenBit` those that may take
    // 10-bit pictures (no hardware encoders there).
    std::vector<std::string> encoderNames(const std::string& codec, bool tenBit) const {
        if (codec == "h264") return tenBit ? std::vector<std::string>{} : std::vector<std::string>{settings.hardware ? "h264_nvenc" : "", "libx264"};
        if (codec == "av1") return {"libsvtav1", "libaom-av1"};
        if (codec == "vp9") return {"libvpx-vp9"};
        if (codec == "hevc") return {settings.hardware && !tenBit ? "hevc_nvenc" : "", "libx265"};
        return {};
    }

    // Whether there is an encoder for the codec, as encoder() would look for one.
    // A hardware encoder counts only when it opens: one built in with no such GPU here would give viewers black.
    bool canEncode(const std::string& codec, bool tenBit = false) const {
        for (const std::string& n : encoderNames(codec, tenBit)) {
            const AVCodec* c = n.empty() ? nullptr : avcodec_find_encoder_by_name(n.c_str());
            if (!c || (n.find("_nvenc") != std::string::npos && !hardwareOpens(n.c_str()))) continue;
            if (!tenBit) return true;
            const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
            // FFmpeg 8 drops AVCodec::pix_fmts; without a context this asks the encoder itself.
            const void* cfg = nullptr;
            if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, nullptr) >= 0)
                formats = static_cast<const AVPixelFormat*>(cfg);
#else
            formats = c->pix_fmts;
#endif
            for (const AVPixelFormat* f = formats; f && *f != AV_PIX_FMT_NONE; ++f)
                if (*f == AV_PIX_FMT_YUV420P10) return true;
        }
        return false;
    }
    bool hardwareOpens(const char* name) const {
        std::lock_guard lock(hardwareMutex);
        if (const auto it = hardwareWorks.find(name); it != hardwareWorks.end()) return it->second;
        bool works = false;
        if (const AVCodec* codec = avcodec_find_encoder_by_name(name))
            if (AVCodecContext* ctx = avcodec_alloc_context3(codec)) {
                ctx->width = 256, ctx->height = 144;
                ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                ctx->time_base = AVRational{1, 30};
                ctx->framerate = AVRational{30, 1};
                works = avcodec_open2(ctx, codec, nullptr) >= 0;
                avcodec_free_context(&ctx);
            }
        // (only a success is kept: one refused while the card's encoders were all busy, an export say, is tried again)
        if (works) hardwareWorks[name] = works;
        return works;
    }
    mutable std::mutex hardwareMutex;
    mutable std::map<std::string, bool> hardwareWorks;

    VideoEncoder* encoder(const EncoderKey& key) {
        auto it = encoders.find(key);
        if (it != encoders.end()) return it->second.get();
        if (encoderErrors.count(key) || key.rung < 0 || key.rung >= int(ladder.size())) return nullptr;
        const Rung& rung = ladder[size_t(key.rung)];
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        std::string why = "This FFmpeg has no encoder for " + key.codec + (key.hdr ? " at 10 bits" : "");
        for (const std::string& name : encoderNames(key.codec, key.hdr)) {
            const AVCodec* c = name.empty() ? nullptr : avcodec_find_encoder_by_name(name.c_str());
            if (!c) continue;
            AVCodecContext* e = avcodec_alloc_context3(c);
            e->width = rung.width;
            e->height = rung.height;
            e->time_base = AVRational{mode.fps.den, mode.fps.num};
            e->framerate = AVRational{mode.fps.num, mode.fps.den};
            e->pix_fmt = key.hdr ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
            e->gop_size = std::max(1, int(std::lround(fps * 4)));  // (and a key frame whenever a viewer joins or asks)
            e->max_b_frames = 0;
            e->bit_rate = rung.bitrate;
            e->rc_max_rate = e->bit_rate;
            e->rc_buffer_size = int(e->bit_rate / 2);
            e->color_range = AVCOL_RANGE_MPEG;
            e->colorspace = key.hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
            e->color_primaries = key.hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
            e->color_trc = !key.hdr ? AVCOL_TRC_BT709 : eotf == Eotf::Pq ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_ARIB_STD_B67;
            e->chroma_sample_location = AVCHROMA_LOC_LEFT;
            e->flags |= AV_CODEC_FLAG_LOW_DELAY;
            if (name.find("nvenc") != std::string::npos) {
                av_opt_set(e->priv_data, "preset", "p1", 0);
                av_opt_set(e->priv_data, "tune", "ll", 0);
                av_opt_set(e->priv_data, "rc", "cbr", 0);
                av_opt_set_int(e->priv_data, "zerolatency", 1, 0);
                av_opt_set_int(e->priv_data, "delay", 0, 0);
                av_opt_set_int(e->priv_data, "forced-idr", 1, 0);
                if (key.codec == "h264") av_opt_set(e->priv_data, "profile", "baseline", 0);
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
                if (key.hdr) av_opt_set(e->priv_data, "profile", "main10", 0);
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
            } else if (name == "libvpx-vp9") {
                // (real time, no hidden reference frames, constant bit rate: libvpx takes it from equal rates)
                av_opt_set(e->priv_data, "deadline", "realtime", 0);
                av_opt_set_int(e->priv_data, "cpu-used", 8, 0);
                av_opt_set_int(e->priv_data, "lag-in-frames", 0, 0);
                av_opt_set_int(e->priv_data, "auto-alt-ref", 0, 0);
                av_opt_set_int(e->priv_data, "row-mt", 1, 0);
                e->rc_min_rate = e->bit_rate;
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
            enc->picture = av_frame_alloc();
            enc->picture->format = e->pix_fmt;
            enc->picture->width = rung.width;
            enc->picture->height = rung.height;
            if (av_frame_get_buffer(enc->picture, 0) < 0) {
                why = "Out of memory";
                continue;
            }
            VideoEncoder* out = enc.get();
            std::lock_guard lock(m);
            encoders[key] = std::move(enc);
            return out;
        }
        encoderErrors[key] = why;
        std::lock_guard lock(m);
        lastError = why;
        relayErrorAt.reset();  // (not the relay's: nothing a viewer's arrival clears)
        return nullptr;
    }

    // What was sent a viewer over the last two seconds, in kb/s.
    static double sentKbps(const Viewer& v, Clock::time_point now) {
        size_t total = 0;
        for (const auto& [at, n] : v.sent)
            if (now - at <= std::chrono::seconds(2)) total += n;
        return double(total) * 8 / 1000 / 2;
    }
    static void sentTo(Viewer& v, size_t n, Clock::time_point now) {
        v.sent.emplace_back(now, n);
        while (!v.sent.empty() && now - v.sent.front().first > std::chrono::seconds(3)) v.sent.pop_front();
    }

    // Moves a viewer on the ladder by their receiver reports and, where their browser reports arrivals, by the queue on
    // the way to them (RungSteering): down when it stands, and up only after a probe at the higher rate arrived
    // without queueing.
    void steer(Viewer& v, Clock::time_point now) {
        const int last = int(ladder.size()) - 1;
        if (last <= 0) return;
        const auto usable = [&](int i) { return !encoderErrors.count({v.codec, i, v.hdr}); };
        bool active = false, standing = false, sending = false;
        double arrived = 0;
        LinkDelay::Probe verdict = LinkDelay::Probe::None;
        if (v.padder) {
            std::lock_guard lock(v.linkM);
            active = v.link.active(now);
            standing = v.link.standing();
            arrived = v.link.receivedKbps();
            verdict = v.link.probe(now);
            sending = v.link.probeSending(now);
        }
        v.steering.probes = active;
        v.padding = v.steering.probing && sending;
        if (v.wantRung != v.rung) return;
        if (v.steering.probing) {
            if (!active || verdict == LinkDelay::Probe::Arrived || verdict == LinkDelay::Probe::Delayed) {
                v.padding = false;
                v.wantRung = v.steering.probed(v.rung, active && verdict == LinkDelay::Probe::Arrived, now, usable);
                std::lock_guard lock(v.linkM);
                v.link.restart(now);  // (the probe's own queue is not the pictures')
                return;
            }
        } else if (active && standing) {
            // (what arrived is what the link carries: the rung under 85% of it)
            const int to = v.steering.delayed(
                v.rung, last, now, [&](int i) { return arrived > 0 && double(ladder[size_t(i)].bitrate) / 1000 > 0.85 * arrived; }, usable);
            if (to != v.rung) {
                v.wantRung = to;
                ++queueMoves;
                return;
            }
        }
        uint64_t reports;
        double loss;
        Clock::time_point at;
        {
            std::lock_guard lock(v.m);
            reports = v.reports;
            loss = v.loss;
            at = v.reportAt;
        }
        if (reports == v.seenReports) return;
        v.seenReports = reports;
        const double through = sentKbps(v, now) * (1 - loss);
        const bool wasProbing = v.steering.probing;
        v.wantRung = v.steering.report(
            v.rung, last, loss, at, now,
            [&](int i) { return double(ladder[size_t(i)].bitrate) / 1000 + settings.audioBitrateKbps > 0.85 * through; }, usable);
        if (v.steering.probing && !wasProbing) {
            std::lock_guard lock(v.linkM);
            v.link.beginProbe(now, kProbeLength);
            v.padding = true;
            ++probes;
        }
    }

    // A picture to each viewer: from the encoder for their codec, rung and depth (each one's tag first: its RTP
    // timestamp, frame and timecode). A viewer waiting for a key frame gets nothing until one; a viewer moving to
    // another rung keeps the one they are on until the new one's key frame.
    void deliver(const std::vector<Coded>& coded, const std::vector<std::shared_ptr<Viewer>>& list) {
        const double fps = mode.fps.toDouble() > 0 ? mode.fps.toDouble() : 25;
        const auto now = Clock::now();
        size_t total = 0;
        for (const auto& v : list) {
            if (!v->video || !v->video->isOpen()) continue;
            const int cur = v->rung, want = v->wantRung;
            auto send = [&](const Coded& c) {
                const uint32_t ticks = uint32_t(int64_t(std::llround(double(c.pts) * 90000.0 / fps)) & 0xffffffff);
                const uint32_t rtp = v->vcfg->startTimestamp + ticks;
                if (c.tag.frame >= 0)
                    sendText(*v, {{"t", "f"}, {"r", double(rtp)}, {"f", double(c.tag.frame)}, {"tc", QString::fromStdString(c.tag.timecode)}, {"seq", double(c.tag.sequence)}});
                if (v->vp9) {
                    v->vp9->width = c.width;
                    v->vp9->height = c.height;
                }
                // (a probe: padding after the picture up to the rung above's rate)
                if (v->padder)
                    v->padder->bytes = v->padding && cur > 0 && c.key.rung == cur
                                           ? int(std::clamp(double(ladder[size_t(cur - 1)].bitrate) / 8 / fps - double(c.data.size()), 0.0, 1e6))
                                           : 0;
                try {
                    v->video->sendFrame(c.data.data(), c.data.size(), rtc::FrameInfo(rtp));
                    v->lastPts = c.pts;
                    sentTo(*v, c.data.size(), now);
                    total = std::max(total, c.data.size());  // (the rate is the most any one viewer is sent)
                } catch (const std::exception&) {
                }
            };
            bool moved = false;
            if (want != cur)
                for (const Coded& c : coded) {
                    if (c.key.codec != v->codec || c.key.hdr != v->hdr || c.key.rung != want) continue;
                    if (!moved && (!c.keyFrame || c.pts <= v->lastPts)) continue;
                    moved = true;
                    send(c);
                }
            if (moved) {
                v->rung = want;
                v->needKey = false;
                // (reports and tries count from the switch, not from when it was asked for)
                v->steering.switched(now);
                {
                    // (from just after the key frame that begins the new size: its burst is no sign of the link)
                    std::lock_guard lock(v->linkM);
                    v->link.restart(now + std::chrono::milliseconds(100));
                }
                rungMessage(*v);
                continue;
            }
            for (const Coded& c : coded) {
                if (c.key.codec != v->codec || c.key.hdr != v->hdr || c.key.rung != cur) continue;
                if (v->needKey) {
                    if (!c.keyFrame) continue;
                    v->needKey = false;
                }
                send(c);
            }
        }
        if (total) counted(total);
    }

    bool encodePicture(const VideoFrame& f, int64_t pts, const std::vector<std::shared_ptr<Viewer>>& list) {
        const auto now = Clock::now();
        std::set<EncoderKey> keys;
        for (const auto& v : list) {
            if (!v->video || v->codec.empty()) continue;
            steer(*v, now);
            keys.insert({v->codec, v->rung, v->hdr});
            if (v->wantRung != v->rung) keys.insert({v->codec, v->wantRung, v->hdr});
        }
        if (keys.empty()) return true;
        StreamTag tag;
        tag.frame = f.frame;
        tag.timecode = streamTimecode(f.timecode);
        tag.fps = f.fps;
        {
            std::lock_guard lock(m);
            tag.sequence = sequence;
        }
        const Planes420 full = planesFrom(f);
        // Each rung's picture once (and the SDR one of an HDR picture), the timecode burnt in at its own size.
        std::map<std::pair<int, bool>, Planes420> pictures;
        auto pictureFor = [&](int rung, bool hdr) -> const Planes420& {
            if (auto it = pictures.find({rung, hdr}); it != pictures.end()) return it->second;
            Planes420 p = shrink(full, ladder[size_t(rung)].width, ladder[size_t(rung)].height);
            if (!hdr && toSdr) toSdr->apply(p);
            if (settings.burnTimecode) burnTimecode(p, tag.timecode, hdr ? hdrWhite : uint16_t(940));
            if (settings.frameCode) burnFrameCode(p, f.frame);
            return pictures.emplace(std::make_pair(rung, hdr), std::move(p)).first->second;
        };
        std::vector<Coded> coded;
        for (const EncoderKey& key : keys) {
            VideoEncoder* enc = encoder(key);
            if (!enc) {
                // (a rung that cannot be made: whoever was moving to it stays where they are)
                for (const auto& v : list)
                    if (v->codec == key.codec && v->hdr == key.hdr && v->wantRung == key.rung && v->rung != key.rung) v->wantRung = v->rung.load();
                continue;
            }
            const Planes420& p = pictureFor(key.rung, key.hdr);
            AVFrame* picture = enc->picture;
            if (av_frame_make_writable(picture) < 0) continue;
            const int w = p.width, h = p.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
            if (key.hdr) {
                for (int y = 0; y < h; ++y)
                    std::memcpy(picture->data[0] + size_t(y) * size_t(picture->linesize[0]), &p.y[size_t(y) * size_t(w)], size_t(w) * 2);
                for (int y = 0; y < ch; ++y) {
                    std::memcpy(picture->data[1] + size_t(y) * size_t(picture->linesize[1]), &p.u[size_t(y) * size_t(cw)], size_t(cw) * 2);
                    std::memcpy(picture->data[2] + size_t(y) * size_t(picture->linesize[2]), &p.v[size_t(y) * size_t(cw)], size_t(cw) * 2);
                }
            } else {
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
            }
            picture->pts = pts;
            // A key frame for whoever waits for one on this encoder (just joined, lost pictures, or moving to its rung),
            // at most twice a second whatever they ask.
            bool keyFrame = false;
            for (const auto& v : list)
                keyFrame = keyFrame || (v->codec == key.codec && v->hdr == key.hdr && v->wantRung == key.rung && (v->needKey || v->wantRung != v->rung));
            auto last = forcedKey.find(key);
            keyFrame = keyFrame && (last == forcedKey.end() || now - last->second >= std::chrono::milliseconds(500));
            if (keyFrame) forcedKey[key] = now;
            picture->pict_type = keyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            enc->tags[pts] = tag;
            while (enc->tags.size() > 64) enc->tags.erase(enc->tags.begin());
            if (avcodec_send_frame(enc->ctx, picture) < 0) continue;
            for (;;) {
                const int r = avcodec_receive_packet(enc->ctx, enc->pkt);
                if (r < 0) break;
                Coded c;
                c.key = key;
                c.data.assign(reinterpret_cast<const std::byte*>(enc->pkt->data), reinterpret_cast<const std::byte*>(enc->pkt->data) + enc->pkt->size);
                c.pts = enc->pkt->pts;
                c.keyFrame = enc->pkt->flags & AV_PKT_FLAG_KEY;
                auto t = enc->tags.find(enc->pkt->pts);
                c.tag = t == enc->tags.end() ? StreamTag{} : t->second;
                c.width = w;
                c.height = h;
                coded.push_back(std::move(c));
                av_packet_unref(enc->pkt);
            }
            picture->pict_type = AV_PICTURE_TYPE_NONE;
        }
        deliver(coded, list);
        return !coded.empty();
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
        std::map<EncoderKey, std::unique_ptr<VideoEncoder>> old;
        {
            std::lock_guard lock(m);
            old.swap(encoders);
        }
        old.clear();
        encoderErrors.clear();
        forcedKey.clear();
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
                        sentTo(*v, size_t(apkt->size), Clock::now());
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
    d.hdr = true;  // (to browsers that decode it, and tone mapped for the rest)
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
        d.sharedPort.reset();
        return false;
    };
    std::string why = checkStreamSettings(d.settings);
    if (!why.empty()) return fail(why);
    if (!isBrowserAddress(d.settings.address)) return fail("A browser review's address is https://host:port");
    if (mode.width % 2 || mode.height % 2) return fail("Streams need an even width and height");
    if (signal.eotf != Eotf::Sdr && signal.format == PixelFormat::Uyvy8) return fail("HDR needs a 10-bit signal (or the BT.709 picture)");
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
    d.relayInUse = false;
    d.relayBeside = false;
    for (const std::string& s : d.settings.iceServers) {
        try {
            // (a relay over TLS or TCP is the browsers' alone: Montage's ICE reaches relays over UDP only, and one it
            // cannot use must not cost the viewers the shared port)
            rtc::IceServer server(s);
            if (server.type == rtc::IceServer::Type::Turn && server.relayType != rtc::IceServer::RelayType::TurnUdp) continue;
            d.relayInUse = d.relayInUse || server.type == rtc::IceServer::Type::Turn;
            d.iceServers.push_back(std::move(server));
        } catch (const std::exception& e) {
            return fail(std::string("ICE server ") + s + ": " + e.what());
        }
    }
    d.expires = 0;
    d.relayLogin = {d.settings.turnUser, d.settings.turnPassword};
    if (!d.settings.turnServer.empty()) {
        // With the relay's shared secret, a login for this review that the relay takes until the link's time is up.
        if (!d.settings.turnSecret.empty()) {
            d.expires = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + d.settings.turnExpirySeconds;
            d.relayLogin = turnRestLogin(d.settings.turnSecret, d.expires);
        }
        RelayAddress relay;
        if (!parseRelayAddress(d.settings.turnServer, relay)) return fail("The TURN relay's address is turn:host:port");
        relay.pin = d.settings.turnPin;
        if (d.settings.relayOnly) {
            // Only through the relay: Montage's end of each connection stays on this computer's loopback behind a
            // bridge to the relay, and its ICE is told of no server, so no media leaves for anywhere but the relay
            // (the page and its signalling are HTTPS, between the viewer and this review's own address). The bridge
            // reaches the relay as its address says: over UDP, TCP or TLS, at an IPv4 or an IPv6 address.
            d.relay = relay;
            d.iceServers.clear();
            d.relayInUse = false;
        } else if (relay.transport != RelayAddress::Transport::Udp) {
            // (not only through the relay, one reached over TCP or TLS is the browsers' own, and a viewer on it is
            // given a bridge to it beside Montage's own addresses)
            d.relay = relay;
            d.relayBeside = true;
        } else {
            // (not only through the relay, Montage's own ICE uses it, which reaches a relay over UDP alone: one over
            // TCP or TLS is then the browsers' own)
            d.iceServers.emplace_back(relay.host, relay.port, d.relayLogin.user, d.relayLogin.password, rtc::IceServer::RelayType::TurnUdp);
            d.relayInUse = true;
        }
    }
    d.mode = mode;
    // The ladder: full size, then half and quarter while they are 64 pixels or more each way, at three eighths and an
    // eighth of the bit rate.
    d.ladder.clear();
    for (int i = 0; i < d.settings.rungs; ++i) {
        Rung r;
        r.width = (mode.width >> i) & ~1;
        r.height = (mode.height >> i) & ~1;
        if (i > 0 && (r.width < 64 || r.height < 64)) break;
        r.bitrate = std::max<int64_t>(100000, int64_t(d.settings.bitrateKbps) * 1000 * (i == 0 ? 8 : i == 1 ? 3 : 1) / 8);
        d.ladder.push_back(r);
    }
    // The frames' colour: HDR goes as it is to those who decode it, and every other picture is made BT.709 SDR.
    d.eotf = signal.eotf;
    d.hdrSignal = signal.eotf == Eotf::Pq || signal.eotf == Eotf::Hlg;
    d.toSdr.reset();
    if (signal.colorSpace != "rec709") {
        const ColorSpace* from = findColorSpace(signal.colorSpace);
        if (!from) return fail("Cannot show " + signal.colorSpace + " in a browser");
        d.toSdr = std::make_unique<ToSdr>();
        d.toSdr->build(*from, signal.bt2020, signal.masteringPeak);
    }
    // (burnt-in text at HDR reference white, 203 nits: PQ's 58 %, HLG's 75 %)
    d.hdrWhite = uint16_t(std::lround(64 + 876 * (d.eotf == Eotf::Hlg ? 0.75 : 0.5806)));
    if (!d.settings.relayOnly && !d.relayInUse && port > 0) {
        const bool any = d.host == QHostAddress::Any || d.host == QHostAddress::AnyIPv6 || d.host == QHostAddress::AnyIPv4;
        try {
            d.sharedPort = std::make_unique<HeldUdpPort>(uint16_t(port), any ? std::nullopt : std::optional<std::string>(d.host.toString().toStdString()));
        } catch (const std::exception&) {
            return fail("Cannot listen on UDP port " + std::to_string(port) + ", where the viewers' pictures and sound go: another program has it");
        }
    }
    d.audioChannels = audioChannels;
    d.audioRate = audioRate > 0 ? audioRate : 48000;
    if (audioChannels > 0 && !d.openSound(&why)) return fail(why);
    {
        std::lock_guard lock(d.m);
        d.open = true;
        d.woken = false;
        d.endedAt.reset();
        d.lastError.clear();
        d.relayErrorAt.reset();
        d.received.clear();
        d.markerCount = 0;
        d.pointing.clear();
        d.joined = 0;
        d.next = d.t0 = Clock::now();
        d.vpts = -1;
        d.apts = 0;
        d.encoded = d.skipped = 0;
        d.probes = d.queueMoves = 0;
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
    std::set<std::shared_ptr<RelayBridge>> bridging;
    {
        std::lock_guard lock(d.m);
        retired.swap(d.retired);
        bridging.swap(d.bridging);
    }
    retired.clear();
    bridging.clear();
    d.sharedPort.reset();
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
    std::set<std::string> names;
    for (const auto& [key, e] : d_->encoders)
        if (names.insert(e->name).second) s.encoder += (s.encoder.empty() ? "" : ", ") + e->name;
    s.rungViewers.assign(d_->ladder.size(), 0);
    for (const auto& [id, v] : d_->viewers)
        if (v->connected && !v->gone && v->video && d_->open && !d_->ladder.empty()) {
            ++s.rungViewers[size_t(std::clamp(v->rung.load(), 0, int(d_->ladder.size()) - 1))];
            s.hdrViewers += v->hdr;
        }
    s.probes = d_->probes;
    s.queueMoves = d_->queueMoves;
    s.linkExpires = d_->open ? d_->expires.load() : 0;
    s.error = d_->lastError;
    // (a viewer's way through the relay that failed: the relay's refusal, or its address for them running out)
    for (const auto& [id, v] : d_->viewers) {
        if (!v->bridge) continue;
        ReviewStats::Bridge b;
        b.viewer = v->address.toStdString();
        b.state = v->bridge->state();
        b.lastRefusal = v->bridge->lastError();
        b.toRelay = v->bridge->toRelay();
        b.fromRelay = v->bridge->fromRelay();
        b.answersRefused = v->bridge->answersRefused();
        b.checksRepaired = v->bridge->checksRepaired();
        b.beside = v->beside;
        b.ports = v->bridge->ports();
        if (s.error.empty()) s.error = b.lastRefusal;
        s.bridges.push_back(std::move(b));
    }
    s.bridgeThreads = RelayBridge::threads();
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
