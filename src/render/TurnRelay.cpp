#include "render/TurnRelay.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkProxy>
#include <QObject>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslError>
#include <QSslSocket>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace montage::output {

TurnLogin turnRestLogin(const std::string& secret, int64_t expires, const std::string& name) {
    TurnLogin login;
    login.user = std::to_string(expires) + ":" + name;
    login.password = QMessageAuthenticationCode::hash(QByteArray::fromStdString(login.user), QByteArray::fromStdString(secret), QCryptographicHash::Sha1)
                         .toBase64()
                         .toStdString();
    return login;
}

namespace {

// STUN and TURN as the bridge speaks them (RFC 8489, RFC 8656).
const quint32 kCookie = 0x2112A442;
enum : quint16 {
    kAllocate = 0x0003,
    kRefresh = 0x0004,
    kSend = 0x0006,
    kData = 0x0007,
    kCreatePermission = 0x0008,
    kRequest = 0x0000,
    kIndication = 0x0010,
    kSuccess = 0x0100,
    kError = 0x0110,
};
enum : quint16 {
    kUsername = 0x0006,
    kMessageIntegrity = 0x0008,
    kErrorCode = 0x0009,
    kLifetime = 0x000D,
    kXorPeerAddress = 0x0012,
    kDataAttribute = 0x0013,
    kRealm = 0x0014,
    kNonce = 0x0015,
    kXorRelayedAddress = 0x0016,
    kRequestedAddressFamily = 0x0017,
    kRequestedTransport = 0x0019,
    kMessageIntegritySha256 = 0x001C,
    kPasswordAlgorithm = 0x001D,
    kPasswordAlgorithms = 0x8002,
};
const int kMaxPeers = 32;
const int kLifetimeAsked = 600;          // seconds
const int kPermissionEveryMs = 240000;   // (a permission lasts five minutes)
const int kPermissionAgainMs = 1000;     // a CreatePermission unanswered is asked again this soon after
const int kRefusedAgainMs = 5000;        // and a Refresh or CreatePermission refused this soon after
const int kResendMs[] = {300, 600, 1200, 2400, 1500};  // the waits after each sending of a request: six seconds in all
const int kLookupMs = 5000;   // how long the relay's name is looked up for
const int kReleaseMs = 250;   // how long the address given back waits for the relay's answer, each time
const qint64 kStreamBacklog = 1 << 20;  // bytes waiting for a relay reached over TCP or TLS, past which packets are dropped

void put16(QByteArray& b, quint16 v) {
    const quint16 be = qToBigEndian(v);
    b.append(reinterpret_cast<const char*>(&be), 2);
}
void put32(QByteArray& b, quint32 v) {
    const quint32 be = qToBigEndian(v);
    b.append(reinterpret_cast<const char*>(&be), 4);
}
quint16 get16(const QByteArray& b, qsizetype at) { return qFromBigEndian<quint16>(b.constData() + at); }
quint32 get32(const QByteArray& b, qsizetype at) { return qFromBigEndian<quint32>(b.constData() + at); }

void putAttribute(QByteArray& b, quint16 type, const QByteArray& value) {
    put16(b, type);
    put16(b, quint16(value.size()));
    b.append(value);
    while (b.size() % 4) b.append('\0');
}

// An IPv4 or IPv6 address, as a viewer's or the relay's.
struct Address {
    bool v6 = false;
    quint8 b[16] = {};  // (IPv4: the first four)
    QByteArray bytes() const { return QByteArray(reinterpret_cast<const char*>(b), v6 ? 16 : 4); }
    QByteArray key() const { return QByteArray(1, v6 ? '6' : '4') + bytes(); }
    QHostAddress host() const { return v6 ? QHostAddress(b) : QHostAddress(qFromBigEndian<quint32>(b)); }
    std::string text() const { return host().toString().toStdString(); }
    bool operator==(const Address& o) const { return v6 == o.v6 && memcmp(b, o.b, v6 ? 16 : 4) == 0; }
};
Address loopback4() {
    Address a;
    a.b[0] = 127, a.b[3] = 1;
    return a;
}

bool isIpv4(const QString& text, quint32& ip) {
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    ip = 0;
    for (const QString& p : parts) {
        bool ok = false;
        const uint v = p.toUInt(&ok);
        if (!ok || v > 255 || p.size() > 3) return false;
        ip = (ip << 8) | v;
    }
    return true;
}
// An address written out: dotted IPv4, or IPv6 without a zone ("fe80::1%eth0" names a link of this computer's, which
// no relay carries to).
bool readAddress(const QString& text, Address& a) {
    quint32 ip = 0;
    if (isIpv4(text, ip)) {
        a = {};
        qToBigEndian(ip, a.b);
        return true;
    }
    if (!text.contains(QLatin1Char(':')) || text.contains(QLatin1Char('%')) || text.contains(QLatin1Char('.'))) return false;
    const QHostAddress h(text);
    if (h.protocol() != QAbstractSocket::IPv6Protocol) return false;
    a.v6 = true;
    const Q_IPV6ADDR v = h.toIPv6Address();
    memcpy(a.b, v.c, 16);
    return true;
}
// XOR-…-ADDRESS (RFC 8489 §14.2): the port with the cookie's top half, the address with the cookie and, for IPv6, the
// message's transaction id after it.
QByteArray xorAddress(const Address& a, quint16 port, const QByteArray& id) {
    QByteArray v;
    put16(v, a.v6 ? 0x0002 : 0x0001);
    put16(v, port ^ quint16(kCookie >> 16));
    QByteArray pad;
    put32(pad, kCookie);
    pad += id.left(12).leftJustified(12, '\0');
    for (int k = 0; k < (a.v6 ? 16 : 4); ++k) v.append(char(a.b[k] ^ quint8(pad[k])));
    return v;
}
bool readXorAddress(const QByteArray& v, const QByteArray& id, Address& a, quint16& port) {
    if (v.size() < 8) return false;
    const quint8 family = quint8(v[1]);
    if (family != 0x01 && family != 0x02) return false;
    a = {};
    a.v6 = family == 0x02;
    if (v.size() < (a.v6 ? 20 : 8)) return false;
    port = get16(v, 2) ^ quint16(kCookie >> 16);
    QByteArray pad;
    put32(pad, kCookie);
    pad += id.left(12).leftJustified(12, '\0');
    for (int k = 0; k < (a.v6 ? 16 : 4); ++k) a.b[k] = quint8(v[4 + k]) ^ quint8(pad[k]);
    return true;
}

struct Message {
    quint16 type = 0;
    QByteArray id;
    std::vector<std::pair<quint16, QByteArray>> attributes;
    const QByteArray* find(quint16 t) const {
        for (const auto& a : attributes)
            if (a.first == t) return &a.second;
        return nullptr;
    }
};
// A STUN message read (false for anything else: a channel's data, or cut short).
bool parse(const QByteArray& b, Message& m) {
    if (b.size() < 20 || (quint8(b[0]) & 0xC0) || get32(b, 4) != kCookie) return false;
    const qsizetype length = get16(b, 2);
    if (length % 4 || 20 + length > b.size()) return false;
    m.type = get16(b, 0);
    m.id = b.mid(8, 12);
    for (qsizetype at = 20; at + 4 <= 20 + length;) {
        const quint16 type = get16(b, at);
        const qsizetype size = get16(b, at + 2);
        if (at + 4 + size > 20 + length) return false;
        m.attributes.emplace_back(type, b.mid(at + 4, size));
        at += 4 + ((size + 3) & ~qsizetype(3));
    }
    return true;
}

// The CRC-32 of a STUN message's FINGERPRINT (the common one, as in zlib).
quint32 crc32(const QByteArray& b) {
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : b) {
        c ^= quint8(ch);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// An answer to a connectivity check (a STUN Binding success response) that names `from` as the address the check came
// from: `to` is put in its place (an IPv6 address takes more room than the loopback one it replaces), and the answer
// signed again with the ICE password and its fingerprint worked out again. Anything else is left as it is.
void renameMappedAddress(QByteArray& d, const Address& from, quint16 fromPort, const Address& to, quint16 toPort, const QByteArray& password) {
    if (d.size() < 20 || get16(d, 0) != 0x0101 || get32(d, 4) != kCookie || 20 + qsizetype(get16(d, 2)) != d.size()) return;
    const QByteArray id = d.mid(8, 12);
    struct Part {
        quint16 type = 0;
        QByteArray value;
    };
    std::vector<Part> parts;
    int mapped = -1, integrity = -1;
    bool fingerprint = false;
    for (qsizetype at = 20; at + 4 <= d.size();) {
        const quint16 type = get16(d, at);
        const qsizetype size = get16(d, at + 2);
        if (at + 4 + size > d.size()) return;
        if (type == 0x0020 && integrity < 0 && mapped < 0) mapped = int(parts.size());
        if (type == kMessageIntegrity && size == 20 && integrity < 0) integrity = int(parts.size());
        if (type == 0x8028 && size == 4 && integrity >= 0) {
            // (the fingerprint is the last attribute: anything after it is not a message Montage's end made)
            if (at + 8 != d.size()) return;
            fingerprint = true;
            break;
        }
        parts.push_back({type, d.mid(at + 4, size)});
        at += 4 + ((size + 3) & ~qsizetype(3));
    }
    Address named;
    quint16 port = 0;
    if (mapped < 0 || integrity < 0 || !readXorAddress(parts[size_t(mapped)].value, id, named, port) || !(named == from) || port != fromPort) return;
    parts[size_t(mapped)].value = xorAddress(to, toPort, id);
    QByteArray out = d.left(20);
    auto setLength = [&](qsizetype length) {
        const quint16 be = qToBigEndian(quint16(length));
        memcpy(out.data() + 2, &be, 2);
    };
    for (int k = 0; k < int(parts.size()); ++k) {
        if (k == integrity) {
            setLength(out.size() - 20 + 24);
            putAttribute(out, kMessageIntegrity, QMessageAuthenticationCode::hash(out, password, QCryptographicHash::Sha1));
        } else {
            putAttribute(out, parts[size_t(k)].type, parts[size_t(k)].value);
        }
    }
    if (out.size() - 20 + (fingerprint ? 8 : 0) > 0xFFFF) return;
    if (fingerprint) {
        setLength(out.size() - 20 + 8);
        QByteArray crc;
        put32(crc, crc32(out) ^ 0x5354554Eu);
        putAttribute(out, 0x8028, crc);
    }
    setLength(out.size() - 20);
    d = out;
}

// A viewer's connectivity check (a STUN Binding request) that says it is from the controlling end with a tiebreaker
// of 0 is given a tiebreaker of 1, and signed again with Montage's end's ICE password (which the check is signed with)
// and its fingerprint worked out again. Chrome sends 0 from a relay it reaches over TCP or TLS, and Montage's ICE takes
// 0 for no ICE-CONTROLLING at all and refuses the check (400) as naming neither role. Anything else is left as it is.
void giveTiebreaker(QByteArray& d, const QByteArray& password) {
    if (d.size() < 20 || get16(d, 0) != 0x0001 || get32(d, 4) != kCookie || 20 + qsizetype(get16(d, 2)) != d.size()) return;
    qsizetype controlling = -1, integrity = -1, fingerprint = -1;
    for (qsizetype at = 20; at + 4 <= d.size();) {
        const quint16 type = get16(d, at);
        const qsizetype size = get16(d, at + 2);
        if (at + 4 + size > d.size()) return;
        if (integrity < 0) {
            if (type == 0x802A && size == 8) controlling = at + 4;
            if (type == kMessageIntegrity && size == 20) integrity = at;
        } else if (type == 0x8028 && size == 4 && at + 8 == d.size()) {
            fingerprint = at;
        } else {
            return;  // (anything else after the signature: not a check as browsers make them)
        }
        at += 4 + ((size + 3) & ~qsizetype(3));
    }
    if (controlling < 0 || integrity < 0) return;
    for (int k = 0; k < 8; ++k)
        if (d[controlling + k] != '\0') return;
    d[controlling + 7] = '\1';
    QByteArray covered = d.left(integrity);
    const quint16 upTo = qToBigEndian(quint16(integrity - 20 + 24));
    memcpy(covered.data() + 2, &upTo, 2);
    const QByteArray mac = QMessageAuthenticationCode::hash(covered, password, QCryptographicHash::Sha1);
    memcpy(d.data() + integrity + 4, mac.constData(), 20);
    if (fingerprint >= 0) {
        const quint32 crc = qToBigEndian(crc32(d.left(fingerprint)) ^ 0x5354554Eu);
        memcpy(d.data() + fingerprint + 4, &crc, 4);
    }
}

// One line's candidate, when it is one: "a=candidate:<foundation> <component> <transport> <priority> <address> <port> typ ...".
struct Candidate {
    QString foundation, component, transport, priority, address;
    quint16 port = 0;
};
bool readCandidate(const QString& line, Candidate& c) {
    if (!line.startsWith(QLatin1String("a=candidate:"))) return false;
    const QStringList t = line.mid(12).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (t.size() < 8 || t[6] != QLatin1String("typ")) return false;
    bool ok = false;
    const uint port = t[5].toUInt(&ok);
    if (!ok || port == 0 || port > 65535) return false;
    c = {t[0], t[1], t[2], t[3], t[4], quint16(port)};
    return true;
}

// The lines of an SDP, each through `line` (which gives the line to keep, or a null string to leave it out).
std::string mapLines(const std::string& sdp, const std::function<QString(const QString&)>& line) {
    std::string out;
    size_t at = 0;
    while (at < sdp.size()) {
        size_t end = sdp.find('\n', at);
        if (end == std::string::npos) end = sdp.size();
        std::string one = sdp.substr(at, end - at);
        const bool cr = !one.empty() && one.back() == '\r';
        if (cr) one.pop_back();
        const QString kept = line(QString::fromStdString(one));
        if (!kept.isNull()) out += kept.toStdString() + (cr ? "\r\n" : "\n");
        at = end + 1;
    }
    return out;
}

const char* transportName(RelayAddress::Transport t) { return t == RelayAddress::Transport::Tls ? "TLS" : t == RelayAddress::Transport::Tcp ? "TCP" : "UDP"; }

// The one thread every bridge's sockets and timers live on: begun with the first bridge and ended with the last.
struct BridgeThread {
    std::mutex life;  // held while it is begun or ended
    std::thread thread;
    int users = 0;
    std::mutex m;
    std::condition_variable cv;
    QObject* home = nullptr;  // (under m)
    QEventLoop* loop = nullptr;
    std::atomic<int> running{0};
    std::atomic<quint64> started{0};
};
BridgeThread& bridgeThread() {
    static BridgeThread* t = new BridgeThread;  // (never destroyed: a bridge may outlive the statics)
    return *t;
}
QObject* joinBridgeThread() {
    BridgeThread& t = bridgeThread();
    std::lock_guard life(t.life);
    if (t.users++ == 0) {
        t.thread = std::thread([&t] {
            QEventLoop l;
            QObject h;
            {
                std::lock_guard lock(t.m);
                t.home = &h;
                t.loop = &l;
            }
            ++t.running;
            ++t.started;
            t.cv.notify_all();
            l.exec();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            {
                std::lock_guard lock(t.m);
                t.home = nullptr;
                t.loop = nullptr;
            }
            --t.running;
        });
        std::unique_lock lock(t.m);
        t.cv.wait(lock, [&] { return t.home != nullptr; });
    }
    std::lock_guard lock(t.m);
    return t.home;
}
void leaveBridgeThread() {
    BridgeThread& t = bridgeThread();
    std::lock_guard life(t.life);
    if (--t.users > 0) return;
    {
        std::lock_guard lock(t.m);
        if (t.loop) QMetaObject::invokeMethod(t.loop, &QEventLoop::quit, Qt::QueuedConnection);
    }
    if (t.thread.joinable()) t.thread.join();
}

}  // namespace

bool parseRelayAddress(const std::string& url, RelayAddress& relay) {
    const size_t colon = url.find(':');
    if (colon == std::string::npos) return false;
    const std::string scheme = url.substr(0, colon);
    if (scheme != "turn" && scheme != "turns") return false;
    relay.transport = scheme == "turns" ? RelayAddress::Transport::Tls : RelayAddress::Transport::Udp;
    std::string rest = url.substr(colon + 1);
    if (const size_t q = rest.find('?'); q != std::string::npos) {
        // (RFC 7065's ?transport=: turn: over TCP; turns: is TLS over TCP whatever it says)
        const QString query = QString::fromStdString(rest.substr(q + 1)).toLower();
        if (query.contains(QLatin1String("transport=tcp")) && relay.transport == RelayAddress::Transport::Udp) relay.transport = RelayAddress::Transport::Tcp;
        rest = rest.substr(0, q);
    }
    uint16_t port = scheme == "turns" ? 5349 : 3478;
    std::string host;
    if (!rest.empty() && rest[0] == '[') {
        const size_t end = rest.find(']');
        if (end == std::string::npos) return false;
        host = rest.substr(1, end - 1);
        rest = rest.substr(end + 1);
        if (!rest.empty() && rest[0] != ':') return false;
        if (!rest.empty()) rest = rest.substr(1);
    } else {
        const size_t c = rest.rfind(':');
        host = rest.substr(0, c);
        rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
    }
    if (!rest.empty()) {
        bool ok = false;
        const int p = QString::fromStdString(rest).toInt(&ok);
        if (!ok || p < 1 || p > 65535) return false;
        port = uint16_t(p);
    }
    if (host.empty()) return false;
    relay.host = host;
    relay.port = port;
    return true;
}

std::string relayPin(const std::string& text) {
    QString hex = QString::fromStdString(text).trimmed().toLower();
    // (as OpenSSL prints it: "sha256 Fingerprint=AB:CD:…")
    if (const qsizetype eq = hex.indexOf(QLatin1Char('=')); eq >= 0) hex = hex.mid(eq + 1);
    hex.remove(QLatin1Char(':')).remove(QLatin1Char(' '));
    if (hex.size() != 64) return {};
    for (const QChar c : hex)
        if (!(c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return {};
    return hex.toStdString();
}

std::string offerThroughRelay(const std::string& sdp, const std::function<uint16_t(const std::string& ip, uint16_t port)>& alias) {
    return mapLines(sdp, [&](const QString& line) {
        if (!line.startsWith(QLatin1String("a=candidate:"))) return line;
        Candidate c;
        Address a;
        if (!readCandidate(line, c) || c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) != 0 || !readAddress(c.address, a)) return QString();
        const uint16_t port = alias(c.address.toStdString(), c.port);
        if (!port) return QString();
        return QStringLiteral("a=candidate:%1 %2 UDP %3 127.0.0.1 %4 typ host").arg(c.foundation, c.component, c.priority).arg(port);
    });
}

int offerFamily(const std::string& sdp) {
    int v4 = 0, v6 = 0;
    mapLines(sdp, [&](const QString& line) {
        Candidate c;
        Address a;
        if (readCandidate(line, c) && c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) == 0 && readAddress(c.address, a)) ++(a.v6 ? v6 : v4);
        return QString();
    });
    return v6 > 0 && v4 == 0 ? 6 : 4;
}

int offerAddresses(const std::string& sdp) {
    int n = 0;
    mapLines(sdp, [&](const QString& line) {
        Candidate c;
        Address a;
        if (readCandidate(line, c) && c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) == 0 && readAddress(c.address, a)) ++n;
        return QString();
    });
    return n;
}

std::string answerThroughRelay(const std::string& sdp, const std::string& relayedIp, uint16_t relayedPort, uint16_t* localPort) {
    if (localPort) *localPort = 0;
    bool given = false;
    const bool v6 = relayedIp.find(':') != std::string::npos;
    return mapLines(sdp, [&](const QString& line) {
        if (!line.startsWith(QLatin1String("a=candidate:"))) return line;
        Candidate c;
        if (given || !readCandidate(line, c) || c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) != 0 || c.address != QLatin1String("127.0.0.1"))
            return QString();
        given = true;
        if (localPort) *localPort = c.port;
        return QStringLiteral("a=candidate:%1 %2 UDP %3 %4 %5 typ relay raddr %6 rport 0")
            .arg(c.foundation, c.component, c.priority, QString::fromStdString(relayedIp))
            .arg(relayedPort)
            .arg(v6 ? QStringLiteral("::") : QStringLiteral("0.0.0.0"));
    });
}

// A bridge's sockets and timer live on the bridges' thread, and everything below `// (on the thread)` runs there.
struct RelayBridge::Impl {
    RelayAddress relay;
    QByteArray pin;  // (the certificate's SHA-256, when one is pinned)
    TurnLogin login;

    std::mutex m;
    std::condition_variable cv;
    std::string relayedIp;  // (under m)
    std::string error;      // (under m)
    std::string stateText = "Not begun";  // (under m)
    bool released = false;  // (under m) the address has been given back, or there was none
    std::atomic<quint16> relayedPort{0}, localPort{0};
    std::atomic<bool> localSaid{false};
    std::atomic<quint64> toRelay{0}, fromRelay{0}, refused{0};

    // (on the thread)
    struct Request {
        quint16 method = 0;
        std::vector<std::pair<quint16, QByteArray>> attributes;
        int sends = 0, logins = 0, unproven = 0;
        qint64 resendAt = 0;
        // (a viewer's address the request names: written against the id of each sending, as an IPv6 one must be)
        bool hasPeer = false;
        Address peer;
        quint16 peerPort = 0;
        std::function<void(const Message* answer)> done;  // (null: no answer in time)
    };
    struct Alias {
        QUdpSocket* socket = nullptr;
        Address address;
        quint16 port = 0;
    };
    QObject* ctx = nullptr;  // what this bridge's connections and queued calls hang on
    QAbstractSocket* outer = nullptr;
    bool stream = false;     // TCP or TLS: STUN messages one after another in a stream of bytes
    QByteArray inbox;        // (a stream's bytes not yet a whole message)
    QTimer* timer = nullptr;
    std::map<QByteArray, Request> requests;
    std::map<QByteArray, Alias> aliases;  // by the viewer's address and port
    std::set<QByteArray> permitted;
    QByteArray realm, nonce, key, algorithms;
    QByteArray md5Key;  // (the key as MD5 makes it, whatever was chosen: libjuice's relay signs its answers with it)
    quint16 algorithm = 0;  // (0: none named, the key is MD5's)
    QByteArray icePassword;  // Montage's end's, once said
    bool allocated = false, asked = false, refreshing = false, lookingUp = false, connected = false, ended = false;
    bool relayedV6 = false;
    int families[2] = {4, 6}, familyAt = 0;
    std::string refreshRefused;  // the relay's refusal of the last Refresh, until one is taken
    std::string permitRefused;   // and of the last CreatePermission, until one is taken
    std::string certificateRefused;  // why the relay's certificate was not taken
    int lifetime = kLifetimeAsked;  // seconds, as the relay last gave it
    // When to ask for the address again, and when the relay lets it go unless it is (now()'s clock, ms).
    qint64 refreshAt = 0, expiresAt = 0, permitAt = 0, connectBy = 0;
    // The address being given back: the Refresh for no time, its answer waited for, twice at most.
    QByteArray releaseId;
    int releaseRound = 0;
    qint64 releaseBy = 0;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    std::function<void(const std::string&)> allocateDone;

    qint64 now() const { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(); }
    void setState(const std::string& text) {
        std::lock_guard lock(m);
        stateText = text;
    }
    std::string over() const { return std::string(" over ") + transportName(relay.transport); }
    // The relay's LIFETIME in an answer, kept, and when to ask again: half way through it.
    void lifetimeFrom(const Message& answer) {
        if (const QByteArray* l = answer.find(kLifetime); l && l->size() == 4) lifetime = int(std::clamp<quint32>(get32(*l, 0), 20, 3600));
        refreshAt = now() + qint64(lifetime) * 500;
        expiresAt = now() + qint64(lifetime) * 1000;
    }
    // The relay has let the address go (its time ran out unrefreshed, it no longer knows it, or the connection to it
    // ended): nothing more is carried, and the bridge says why.
    void lapsed(const std::string& why) {
        if (!allocated) return;
        allocated = false;
        ended = true;
        setError(why);
        setState("Ended");
    }

    // (on the thread) Begun: what the bridge's connections hang on, and its clock.
    void begin() {
        ctx = new QObject;
        timer = new QTimer(ctx);
        QObject::connect(timer, &QTimer::timeout, ctx, [this] { tick(); });
        timer->start(100);
    }

    static QByteArray randomId() {
        quint32 words[3];
        QRandomGenerator::system()->fillRange(words);
        return QByteArray(reinterpret_cast<const char*>(words), 12);
    }

    // A request's bytes: its attributes, then the login once the relay has named its realm, signed.
    QByteArray build(const Request& r, const QByteArray& id) const {
        QByteArray b;
        put16(b, quint16(r.method | kRequest));
        put16(b, 0);
        put32(b, kCookie);
        b.append(id);
        for (const auto& a : r.attributes) putAttribute(b, a.first, a.second);
        if (!realm.isEmpty()) {
            putAttribute(b, kUsername, QByteArray::fromStdString(login.user));
            putAttribute(b, kRealm, realm);
            putAttribute(b, kNonce, nonce);
            if (algorithm) {
                // (the relay's list first, then the one chosen from it: libjuice's relay reads them in that order)
                putAttribute(b, kPasswordAlgorithms, algorithms);
                QByteArray named;
                put16(named, algorithm);
                put16(named, 0);
                putAttribute(b, kPasswordAlgorithm, named);
            }
            // (the length counts each integrity attribute while it is worked out; a relay that named its
            // algorithms is given the SHA-256 one as well, as RFC 8489 asks, after the one every relay reads)
            auto sign = [&](quint16 type, QCryptographicHash::Algorithm hash, int size) {
                const quint16 withIntegrity = qToBigEndian(quint16(b.size() - 20 + 4 + size));
                memcpy(b.data() + 2, &withIntegrity, 2);
                putAttribute(b, type, QMessageAuthenticationCode::hash(b, key, hash));
            };
            sign(kMessageIntegrity, QCryptographicHash::Sha1, 20);
            if (algorithm) sign(kMessageIntegritySha256, QCryptographicHash::Sha256, 32);
        }
        const quint16 length = qToBigEndian(quint16(b.size() - 20));
        memcpy(b.data() + 2, &length, 2);
        return b;
    }

    // A request on its way. Over UDP it is sent again until answered, six seconds in all; a stream loses nothing, so
    // there it is sent once and waited for as long (RFC 8489 §6.2.2).
    void send(Request r) {
        if (!outer) return;
        const QByteArray id = randomId();
        if (r.hasPeer) {
            r.attributes.erase(std::remove_if(r.attributes.begin(), r.attributes.end(), [](const auto& a) { return a.first == kXorPeerAddress; }), r.attributes.end());
            r.attributes.emplace_back(kXorPeerAddress, xorAddress(r.peer, r.peerPort, id));
        }
        r.sends = stream ? int(std::size(kResendMs)) : 1;
        r.resendAt = now() + (stream ? std::accumulate(std::begin(kResendMs), std::end(kResendMs), 0) : kResendMs[0]);
        outer->write(build(r, id));
        requests[id] = std::move(r);
    }

    void tick() {
        const qint64 at = now();
        if (releaseRound) {
            if (at >= releaseBy) releaseDone();
            return;
        }
        if (!connected && connectBy && at >= connectBy) {
            linkLost(QStringLiteral("it did not answer in time"));
            if (outer) outer->abort();
            return;
        }
        std::vector<QByteArray> lost;
        for (auto& [id, r] : requests) {
            if (at < r.resendAt) continue;
            if (r.sends >= int(std::size(kResendMs))) {
                lost.push_back(id);
                continue;
            }
            r.resendAt = at + kResendMs[r.sends++];
            if (outer) outer->write(build(r, id));
        }
        for (const QByteArray& id : lost) {
            auto it = requests.find(id);
            if (it == requests.end()) continue;
            Request r = std::move(it->second);
            requests.erase(it);
            if (r.unproven) {
                // (answers came, but none that the login's key had signed: said as it is, where the request's end is told)
                setError("The TURN relay did not answer in a way Montage could check: " + std::to_string(r.unproven) +
                         (r.unproven == 1 ? " answer was" : " answers were") + " not signed with this review's login");
                unprovenLost = true;
            }
            if (r.done) r.done(nullptr);
            unprovenLost = false;
        }
        if (allocated && at >= expiresAt)
            lapsed("The TURN relay's address for this viewer ran out: " +
                   (refreshRefused.empty() ? std::string("the relay did not answer in time to keep it") : "the relay refused to keep it. " + refreshRefused));
        // The address is asked for again half way through its time and, when an asking goes unanswered (six seconds
        // of sending it), at once again for as long as the address lasts: a lost answer costs no viewer their picture.
        if (allocated && !refreshing && at >= refreshAt) {
            refreshing = true;
            Request r;
            r.method = kRefresh;
            QByteArray life;
            put32(life, kLifetimeAsked);
            r.attributes.emplace_back(kLifetime, life);
            r.done = [this](const Message* answer) {
                refreshing = false;
                if (!answer) {
                    refreshAt = now();
                    return;
                }
                if ((answer->type & 0x0110) == kError) {
                    // (437: the relay knows the address no more; another refusal is asked again a few seconds on)
                    if (errorCode(*answer) == 437) return lapsed(refusal(answer));
                    refreshAt = now() + kRefusedAgainMs;
                    refreshRefused = refusal(answer);
                    return standing();
                }
                // (kept again: a refusal before it is no longer the bridge's state)
                if (!refreshRefused.empty()) {
                    refreshRefused.clear();
                    standing();
                }
                lifetimeFrom(*answer);
            };
            send(std::move(r));
        }
        if (allocated && at >= permitAt) {
            permitAt = at + kPermissionEveryMs;
            for (const auto& [k, a] : aliases) permit(a.address, a.port, true);
        }
    }
    bool unprovenLost = false;  // (while a request that only had unsigned answers is told it had none)
    bool releasing = false;     // release() was begun (it is begun once)

    void setError(const std::string& why) {
        std::lock_guard lock(m);
        error = why;
    }
    static int errorCode(const Message& answer) {
        const QByteArray* e = answer.find(kErrorCode);
        return e && e->size() >= 4 ? (quint8((*e)[2]) & 7) * 100 + quint8((*e)[3]) : 0;
    }
    static std::string refusal(const Message* answer) {
        const QByteArray* e = answer ? answer->find(kErrorCode) : nullptr;
        if (!e || e->size() < 4) return "The TURN relay did not answer";
        const int code = errorCode(*answer);
        if (code == 401 || code == 431) return "The TURN relay did not take Montage's login (" + std::to_string(code) + "): its time may be up";
        // (the relay's reason as text: printable characters only, at most 80 of them, never cut inside one)
        QString reason;
        for (const QChar ch : QString::fromUtf8(e->mid(4)).simplified())
            if (ch.isPrint() || ch.isSurrogate()) reason += ch;
        if (reason.size() > 80) {
            const qsizetype cut = reason[79].isHighSurrogate() ? 79 : 80;
            reason = reason.left(cut) + QChar(0x2026);
        }
        return "The TURN relay refused (" + std::to_string(code) + (reason.isEmpty() ? "" : " " + reason.toStdString()) + ")";
    }
    // The bridge's error while it carries: the relay's refusal that still stands (of keeping the address, else of
    // letting a viewer in), or none once what was refused has been taken since. An ended bridge keeps why it ended.
    void standing() {
        if (ended) return;
        setError(!refreshRefused.empty() ? refreshRefused : permitRefused);
    }

    // Whether an answer is signed with the login's key (RFC 8489 §9.2.5): it has MESSAGE-INTEGRITY or
    // MESSAGE-INTEGRITY-SHA256, and each one it has is right for the message up to it, under the key chosen or the
    // one MD5 makes (either is proof the relay knows the password).
    bool signedRight(const QByteArray& d) const {
        const qsizetype end = std::min<qsizetype>(d.size(), 20 + get16(d, 2));
        bool any = false;
        for (qsizetype at = 20; at + 4 <= end;) {
            const quint16 type = get16(d, at);
            const qsizetype size = get16(d, at + 2);
            if (at + 4 + size > end) return false;
            if (type == kMessageIntegrity || type == kMessageIntegritySha256) {
                const bool sha256 = type == kMessageIntegritySha256;
                if (size != (sha256 ? 32 : 20)) return false;
                QByteArray covered = d.left(at);
                const quint16 length = qToBigEndian(quint16(at - 20 + 4 + size));
                memcpy(covered.data() + 2, &length, 2);
                const QCryptographicHash::Algorithm hash = sha256 ? QCryptographicHash::Sha256 : QCryptographicHash::Sha1;
                const QByteArray given = d.mid(at + 4, size);
                if (QMessageAuthenticationCode::hash(covered, key, hash) != given && QMessageAuthenticationCode::hash(covered, md5Key, hash) != given) return false;
                any = true;
            }
            at += 4 + ((size + 3) & ~qsizetype(3));
        }
        return any;
    }

    // An answer to a request: the relay's realm and nonce are taken from a 401 or 438 and the request sent again
    // with the login (three times at most); anything else is the request's answer. Once Montage signs its
    // requests, an answer that is not signed with the same key (any but a 401 or 438, which carry the relay's
    // challenge) is not used and is counted: the request goes on waiting (over UDP, sent again as before).
    void answered(const Message& m, const QByteArray& raw) {
        auto it = requests.find(m.id);
        if (it == requests.end() || (m.type & ~0x0110) != it->second.method) return;
        const int answerCode = (m.type & 0x0110) == kError ? errorCode(m) : 0;
        const bool challenge = answerCode == 401 || answerCode == 438;
        const bool signedAtAll = m.find(kMessageIntegrity) || m.find(kMessageIntegritySha256);
        if (!key.isEmpty() && !challenge && ((m.type & 0x0110) == kSuccess || signedAtAll) && !signedRight(raw)) {
            ++refused;
            ++it->second.unproven;
            return;
        }
        Request r = std::move(it->second);
        requests.erase(it);
        if ((m.type & 0x0110) == kError) {
            const int code = errorCode(m);
            const QByteArray* n = m.find(kNonce);
            const QByteArray* rl = m.find(kRealm);
            // (401 once the login has been sent with this realm is the relay's refusal of it)
            const bool retry = (code == 438 || (code == 401 && (realm.isEmpty() || r.logins == 0))) && n && (rl || !realm.isEmpty()) && r.logins < 3;
            if (retry) {
                nonce = *n;
                if (rl) realm = *rl;
                algorithm = 0;
                QCryptographicHash::Algorithm hash = QCryptographicHash::Md5;
                if (const QByteArray* offered = m.find(kPasswordAlgorithms)) {
                    // (RFC 8489: the relay's list is sent back with the one chosen; SHA-256 where offered, else MD5)
                    algorithms = *offered;
                    bool md5 = false, sha256 = false;
                    for (qsizetype at = 0; at + 4 <= offered->size(); at += 4 + ((get16(*offered, at + 2) + 3) & ~3)) {
                        md5 = md5 || get16(*offered, at) == 1;
                        sha256 = sha256 || get16(*offered, at) == 2;
                    }
                    algorithm = sha256 ? 2 : md5 ? 1 : 0;
                    if (algorithm == 2) hash = QCryptographicHash::Sha256;
                }
                const QByteArray said = QByteArray::fromStdString(login.user) + ":" + realm + ":" + QByteArray::fromStdString(login.password);
                key = QCryptographicHash::hash(said, hash);
                md5Key = QCryptographicHash::hash(said, QCryptographicHash::Md5);
                ++r.logins;
                return send(std::move(r));
            }
        }
        if (r.done) r.done(&m);
    }

    void allocate() {
        // The relay's address: as written, or its name looked up (its first IPv4 address, or with none its first
        // IPv6 one). A name is looked up without holding the thread, so a bridge let go of meanwhile ends at once.
        setState("Finding the relay");
        Address literal;
        if (readAddress(QString::fromStdString(relay.host), literal)) return connectTo(literal.host());
        // (a lookup gets a few seconds: a name server that never answers is said so)
        lookingUp = true;
        const int lookup = QHostInfo::lookupHost(QString::fromStdString(relay.host), ctx, [this](const QHostInfo& found) {
            if (!std::exchange(lookingUp, false)) return;
            QHostAddress v6;
            for (const QHostAddress& a : found.addresses()) {
                if (a.protocol() == QAbstractSocket::IPv4Protocol) return connectTo(a);
                if (a.protocol() == QAbstractSocket::IPv6Protocol && v6.isNull()) v6 = a;
            }
            connectTo(v6);
        });
        QTimer::singleShot(kLookupMs, ctx, [this, lookup] {
            if (!std::exchange(lookingUp, false)) return;
            QHostInfo::abortHostLookup(lookup);
            allocated_("The TURN relay's address (" + relay.host + ") was not found in time");
        });
    }
    // The one socket to the outside, connected to the relay over the transport said, and then the relay asked for
    // an address.
    void connectTo(const QHostAddress& address) {
        if (address.isNull()) return allocated_("The TURN relay's address (" + relay.host + ") was not found");
        setState("Connecting to the relay" + over());
        connectBy = now() + std::accumulate(std::begin(kResendMs), std::end(kResendMs), 0);
        if (relay.transport == RelayAddress::Transport::Udp) {
            auto* udp = new QUdpSocket(ctx);
            outer = udp;
            udp->setProxy(QNetworkProxy::NoProxy);
            // (bound first: a UDP socket only connected has no port of its own on every system until it sends)
            if (!udp->bind(QHostAddress(address.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4), 0))
                return allocated_("No UDP port to reach the TURN relay from");
        } else if (relay.transport == RelayAddress::Transport::Tcp) {
            outer = new QTcpSocket(ctx);
            outer->setProxy(QNetworkProxy::NoProxy);
            stream = true;
        } else {
            auto* tls = new QSslSocket(ctx);
            outer = tls;
            tls->setProxy(QNetworkProxy::NoProxy);
            stream = true;
            tls->setPeerVerifyMode(QSslSocket::VerifyPeer);
            // A pinned certificate is the relay's whoever signed it and whatever name it bears: the errors of a
            // certificate that is the pinned one are passed over, and no other certificate is taken.
            QObject::connect(tls, &QSslSocket::sslErrors, ctx, [this, tls](const QList<QSslError>& errors) {
                if (!pin.isEmpty() && tls->peerCertificate().digest(QCryptographicHash::Sha256) == pin) return tls->ignoreSslErrors();
                certificateRefused = "The TURN relay's certificate was not accepted: " +
                                     (pin.isEmpty() ? (errors.isEmpty() ? std::string("it could not be checked") : errors.first().errorString().toStdString())
                                                    : std::string("it is not the one pinned"));
            });
            QObject::connect(tls, &QSslSocket::encrypted, ctx, [this, tls] {
                if (!pin.isEmpty() && tls->peerCertificate().digest(QCryptographicHash::Sha256) != pin) {
                    certificateRefused = "The TURN relay's certificate was not accepted: it is not the one pinned";
                    linkLost(QString());
                    return tls->abort();
                }
                linked();
            });
        }
        QObject::connect(outer, &QAbstractSocket::readyRead, ctx, [this] { readOuter(); });
        if (stream) {
            // (a stream that ends takes the address with it: the relay lets it go when the connection does)
            QObject::connect(outer, &QAbstractSocket::errorOccurred, ctx, [this](QAbstractSocket::SocketError) { linkLost(outer ? outer->errorString() : QString()); });
            QObject::connect(outer, &QAbstractSocket::disconnected, ctx, [this] { linkLost(QStringLiteral("the relay closed the connection")); });
        }
        if (relay.transport == RelayAddress::Transport::Tls) {
            static_cast<QSslSocket*>(outer)->connectToHostEncrypted(address.toString(), relay.port, QString::fromStdString(relay.host));
        } else {
            if (relay.transport == RelayAddress::Transport::Tcp) QObject::connect(outer, &QAbstractSocket::connected, ctx, [this] { linked(); });
            outer->connectToHost(address, relay.port);
            // (UDP only names where its packets go: there is nothing to wait for)
            if (relay.transport == RelayAddress::Transport::Udp) {
                if (outer->state() != QAbstractSocket::ConnectedState && !outer->waitForConnected(2000))
                    return allocated_("The TURN relay cannot be reached: " + outer->errorString().toStdString());
                linked();
            }
        }
    }
    // The connection to the relay failed or ended (TCP and TLS).
    void linkLost(const QString& why) {
        if (ended || releaseRound) return;
        const std::string said = !certificateRefused.empty() ? certificateRefused
                                 : (connected ? "The connection to the TURN relay" + over() + " ended" : "The TURN relay cannot be reached" + over()) +
                                       (why.isEmpty() ? std::string() : ": " + why.toStdString());
        connectBy = 0;
        if (allocated) return lapsed(said);
        ended = true;
        setState("Ended");
        // (requests waiting are told they have no answer; an Allocate among them says why through allocated_)
        setError(said);
        auto waiting = std::move(requests);
        requests.clear();
        if (allocateDone) return allocated_(said);
        for (auto& [id, r] : waiting)
            if (r.done) r.done(nullptr);
    }
    void linked() {
        if (connected || ended) return;
        connected = true;
        connectBy = 0;
        askForAddress();
    }
    void askForAddress() {
        setState("Asking the relay for an address");
        Request r;
        r.method = kAllocate;
        QByteArray udp;
        put32(udp, 17u << 24);
        r.attributes.emplace_back(kRequestedTransport, udp);
        // (an IPv6 address is asked for by name, RFC 8656's REQUESTED-ADDRESS-FAMILY; IPv4 is what a relay gives
        // when nothing is said, and relays older than that attribute know no other)
        if (families[familyAt] == 6) r.attributes.emplace_back(kRequestedAddressFamily, QByteArray::fromHex("02000000"));
        QByteArray life;
        put32(life, kLifetimeAsked);
        r.attributes.emplace_back(kLifetime, life);
        r.done = [this](const Message* answer) {
            if (!answer) {
                std::string why;
                {
                    std::lock_guard lock(m);
                    if (unprovenLost) why = error;
                }
                return allocated_(why.empty() ? refusal(nullptr) : why);
            }
            if ((answer->type & 0x0110) == kError) {
                // (440: the relay has no address of that family; the other is asked for, once)
                if (errorCode(*answer) == 440 && familyAt == 0) {
                    familyAt = 1;
                    return askForAddress();
                }
                return allocated_(refusal(answer));
            }
            Address a;
            quint16 p = 0;
            const QByteArray* relayed = answer->find(kXorRelayedAddress);
            if (!relayed || !readXorAddress(*relayed, answer->id, a, p)) return allocated_("The TURN relay gave no address");
            {
                std::lock_guard lock(m);
                relayedIp = a.text();
            }
            relayedV6 = a.v6;
            relayedPort = p;
            allocated = true;
            lifetimeFrom(*answer);
            permitAt = now() + kPermissionEveryMs;
            for (const auto& [k, al] : aliases) permit(al.address, al.port, true);
            setState("Carrying" + over());
            allocated_({});
        };
        send(std::move(r));
    }
    void allocated_(const std::string& why) {
        if (!why.empty()) {
            setError(why);
            if (!allocated) setState("Ended");
        }
        if (auto done = std::exchange(allocateDone, nullptr)) done(why);
    }

    // The relay is asked to let a viewer's address in (again every four minutes, and a second after an asking that
    // went unanswered, so a lost answer leaves no gap of minutes in the picture).
    void permit(const Address& address, quint16 peerPort, bool again) {
        if (!again && !permitted.insert(address.key()).second) return;
        // (a relay carries between addresses of one family: an address of the other is never let in)
        if (!allocated || address.v6 != relayedV6) return;
        Request r;
        r.method = kCreatePermission;
        r.hasPeer = true;
        r.peer = address;
        r.peerPort = peerPort;
        r.done = [this](const Message* answer) {
            if (!answer) {
                permitAt = std::min(permitAt, now() + kPermissionAgainMs);
            } else if ((answer->type & 0x0110) == kError) {
                // (asked again a few seconds on, not in four minutes: the viewer has no picture meanwhile)
                permitRefused = refusal(answer);
                permitAt = std::min(permitAt, now() + kRefusedAgainMs);
                standing();
            } else if (!permitRefused.empty()) {
                permitRefused.clear();
                standing();
            }
        };
        send(std::move(r));
    }

    Alias* aliasFor(const Address& address, quint16 peerPort) {
        QByteArray k = address.key();
        put16(k, peerPort);
        auto it = aliases.find(k);
        if (it != aliases.end()) return &it->second;
        if (int(aliases.size()) >= kMaxPeers || !ctx) return nullptr;
        auto* s = new QUdpSocket(ctx);
        s->setProxy(QNetworkProxy::NoProxy);
        if (!s->bind(QHostAddress(QHostAddress::LocalHost), 0)) {
            delete s;
            return nullptr;
        }
        QObject::connect(s, &QUdpSocket::readyRead, ctx, [this, s, address, peerPort] { readAlias(s, address, peerPort); });
        Alias& a = aliases[k];
        a = {s, address, peerPort};
        permit(address, peerPort, false);
        return &a;
    }

    // What Montage's end sent a viewer's address: wrapped for the relay (only what this computer's loopback sent,
    // from Montage's end's port).
    void readAlias(QUdpSocket* s, const Address& address, quint16 peerPort) {
        while (s->hasPendingDatagrams()) {
            QByteArray d(int(s->pendingDatagramSize()), '\0');
            QHostAddress from;
            quint16 fromPort = 0;
            const qint64 n = s->readDatagram(d.data(), d.size(), &from, &fromPort);
            if (n <= 0 || !from.isLoopback()) continue;
            d.truncate(n);
            if (!localSaid && !localPort) localPort = fromPort;
            if (fromPort != localPort) continue;
            // (an answer to the viewer's check names the viewer's own address, not the port here that stands for it)
            if (!icePassword.isEmpty()) renameMappedAddress(d, loopback4(), s->localPort(), address, peerPort, icePassword);
            const QByteArray id = randomId();
            QByteArray b;
            b.reserve(d.size() + 56);
            put16(b, quint16(kSend | kIndication));
            put16(b, 0);
            put32(b, kCookie);
            b.append(id);
            putAttribute(b, kXorPeerAddress, xorAddress(address, peerPort, id));
            putAttribute(b, kDataAttribute, d);
            if (b.size() - 20 > 0xFFFF) continue;
            const quint16 length = qToBigEndian(quint16(b.size() - 20));
            memcpy(b.data() + 2, &length, 2);
            // (a stream the relay is slow to take is not let grow without end: pictures are dropped, as a full UDP
            // buffer drops them)
            if (!allocated || !outer || (stream && outer->bytesToWrite() > kStreamBacklog)) continue;
            // (counted before it goes, so whoever sees a packet arrive finds it counted)
            ++toRelay;
            outer->write(b);
        }
    }

    // What the relay sent: over UDP a message a datagram; over TCP and TLS messages one after another, each as long
    // as its header says (a STUN message, or a channel's data, which Montage asks for none of and passes over).
    void readOuter() {
        if (!outer) return;
        if (!stream) {
            auto* udp = static_cast<QUdpSocket*>(outer);
            while (udp->hasPendingDatagrams()) {
                QByteArray d(int(udp->pendingDatagramSize()), '\0');
                const qint64 n = udp->readDatagram(d.data(), d.size());
                if (n <= 0) continue;
                d.truncate(n);
                heard(d);
                if (!outer) return;
            }
            return;
        }
        inbox += outer->readAll();
        qsizetype at = 0;
        while (inbox.size() - at >= 4) {
            const quint8 first = quint8(inbox[at]);
            qsizetype size = 0;
            if ((first & 0xC0) == 0x00) size = 20 + get16(inbox, at + 2);
            else if ((first & 0xC0) == 0x40) size = 4 + ((get16(inbox, at + 2) + 3) & ~qsizetype(3));
            else size = -1;
            // (bytes that are neither: the stream has lost its place, and nothing after can be trusted)
            if (size < 0 || ((first & 0xC0) == 0x00 && inbox.size() - at >= 8 && get32(inbox, at + 4) != kCookie)) {
                inbox.clear();
                linkLost(QStringLiteral("what the relay sent is not TURN"));
                if (outer) outer->abort();
                return;
            }
            if (inbox.size() - at < size) break;
            const QByteArray one = inbox.mid(at, size);
            at += size;
            if ((first & 0xC0) == 0x00) heard(one);
            if (!outer) return;
        }
        inbox.remove(0, at);
    }
    // One message from the relay: an answer to a request, or a viewer's packet, passed to Montage's end from that
    // viewer's loopback port.
    void heard(const QByteArray& d) {
        Message msg;
        if (!parse(d, msg)) return;
        if (msg.type == quint16(kData | kIndication)) {
            Address address;
            quint16 peerPort = 0;
            const QByteArray* peerAddress = msg.find(kXorPeerAddress);
            const QByteArray* data = msg.find(kDataAttribute);
            const quint16 to = localPort;
            if (releaseRound || !peerAddress || !data || !readXorAddress(*peerAddress, msg.id, address, peerPort) || !to || address.v6 != relayedV6) return;
            if (Alias* a = aliasFor(address, peerPort)) {
                ++fromRelay;
                if (icePassword.isEmpty()) {
                    a->socket->writeDatagram(*data, QHostAddress(QHostAddress::LocalHost), to);
                } else {
                    QByteArray check = *data;
                    giveTiebreaker(check, icePassword);
                    a->socket->writeDatagram(check, QHostAddress(QHostAddress::LocalHost), to);
                }
            }
        } else if (msg.type & 0x0100) {
            if (releaseRound) releaseAnswered(msg);
            else answered(msg, d);
        }
    }

    // The address is given back (the relay lets it go by itself otherwise): a Refresh for no time, its answer waited
    // for a moment, and a nonce the relay calls stale (438) taken and the address given back once more with it.
    // Nothing is carried meanwhile, and whoever lets the bridge go waits for releaseDone().
    void release() {
        if (releaseRound || releasing) return;  // (already being given back, or done)
        releasing = true;
        timerOnly();
        if (!allocated || !outer || outer->state() != QAbstractSocket::ConnectedState) return releaseDone();
        releaseRound = 1;
        sendRelease();
    }
    void timerOnly() {
        // (no more requests, callbacks or packets: only the relay's answer to the giving back is listened for)
        requests.clear();
        allocateDone = nullptr;
        lookingUp = false;
        for (auto& [k, a] : aliases) QObject::disconnect(a.socket, nullptr, ctx, nullptr);
    }
    void sendRelease() {
        Request r;
        r.method = kRefresh;
        QByteArray zero;
        put32(zero, 0);
        r.attributes.emplace_back(kLifetime, zero);
        releaseId = randomId();
        releaseBy = now() + kReleaseMs;
        outer->write(build(r, releaseId));
        outer->flush();
    }
    void releaseAnswered(const Message& msg) {
        if (msg.id != releaseId || (msg.type & ~0x0110) != kRefresh) return;
        if (const QByteArray* nonceGiven = msg.find(kNonce); (msg.type & 0x0110) == kError && errorCode(msg) == 438 && nonceGiven && releaseRound == 1) {
            nonce = *nonceGiven;
            releaseRound = 2;
            return sendRelease();
        }
        releaseDone();
    }
    void releaseDone() {
        releaseRound = 0;
        allocated = false;
        ended = true;
        if (timer) timer->stop();
        if (outer) QObject::disconnect(outer, nullptr, ctx, nullptr);
        {
            std::lock_guard lock(m);
            released = true;
        }
        cv.notify_all();
    }
};

RelayBridge::RelayBridge(std::string host, uint16_t port, TurnLogin login) : RelayBridge(RelayAddress{std::move(host), port, RelayAddress::Transport::Udp, {}}, std::move(login)) {}

RelayBridge::RelayBridge(RelayAddress relay, TurnLogin login) : d_(std::make_unique<Impl>()) {
    d_->pin = QByteArray::fromHex(QByteArray::fromStdString(relayPin(relay.pin)));
    d_->relay = std::move(relay);
    d_->login = std::move(login);
    QObject* home = joinBridgeThread();
    QMetaObject::invokeMethod(home, [d = d_.get()] { d->begin(); }, Qt::BlockingQueuedConnection);
}

RelayBridge::~RelayBridge() {
    Impl* d = d_.get();
    QObject* home = nullptr;
    {
        std::lock_guard lock(bridgeThread().m);
        home = bridgeThread().home;
    }
    if (home && QThread::currentThread() == home->thread()) {
        // (let go of on the bridges' own thread, which a callback should not do: nothing is waited for, and the
        // thread is left from another)
        d->timerOnly();
        d->releaseDone();
        d_.release();
        QMetaObject::invokeMethod(
            home,
            [d] {
                delete d->ctx;
                delete d;
                std::thread([] { leaveBridgeThread(); }).detach();
            },
            Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(d->ctx, [d] { d->release(); }, Qt::QueuedConnection);
    {
        std::unique_lock lock(d->m);
        d->cv.wait_for(lock, std::chrono::milliseconds(4 * kReleaseMs + 2000), [&] { return d->released; });
    }
    // (its sockets and timer end on their thread, outside any of their own calls; calls still queued for it go too)
    QMetaObject::invokeMethod(
        home,
        [d] {
            delete d->ctx;
            d->ctx = nullptr;
            d->outer = nullptr;
            d->timer = nullptr;
        },
        Qt::BlockingQueuedConnection);
    leaveBridgeThread();
}

void RelayBridge::letGo() {
    Impl* d = d_.get();
    QMetaObject::invokeMethod(d->ctx, [d] { d->release(); }, Qt::QueuedConnection);
}

bool RelayBridge::ended() const {
    std::lock_guard lock(d_->m);
    return d_->stateText == "Ended";
}

void RelayBridge::allocate(int prefer, std::function<void(const std::string& error)> done) {
    QMetaObject::invokeMethod(
        d_->ctx,
        [d = d_.get(), prefer, done = std::move(done)]() mutable {
            if (d->asked) return;
            d->asked = true;
            d->families[0] = prefer == 6 ? 6 : 4;
            d->families[1] = prefer == 6 ? 4 : 6;
            d->allocateDone = std::move(done);
            d->allocate();
        },
        Qt::QueuedConnection);
}

std::string RelayBridge::relayedAddress() const {
    std::lock_guard lock(d_->m);
    return d_->relayedIp;
}
uint16_t RelayBridge::relayedPort() const { return d_->relayedPort; }

uint16_t RelayBridge::peer(const std::string& ip, uint16_t port) {
    Address address;
    if (!port || !readAddress(QString::fromStdString(ip), address)) return 0;
    uint16_t alias = 0;
    QMetaObject::invokeMethod(
        d_->ctx,
        [&] {
            // (a relay carries between addresses of one family: the one its address for Montage has)
            if (d_->relayedPort && address.v6 != d_->relayedV6) return;
            if (Impl::Alias* a = d_->aliasFor(address, port)) alias = a->socket->localPort();
        },
        Qt::BlockingQueuedConnection);
    return alias;
}

void RelayBridge::setLocal(uint16_t port, const std::string& icePassword) {
    d_->localPort = port;
    d_->localSaid = true;
    QMetaObject::invokeMethod(
        d_->ctx, [d = d_.get(), password = QByteArray::fromStdString(icePassword)] { d->icePassword = password; }, Qt::QueuedConnection);
}

uint64_t RelayBridge::toRelay() const { return d_->toRelay; }
uint64_t RelayBridge::fromRelay() const { return d_->fromRelay; }
uint64_t RelayBridge::answersRefused() const { return d_->refused; }
std::string RelayBridge::lastError() const {
    std::lock_guard lock(d_->m);
    return d_->error;
}
std::string RelayBridge::state() const {
    std::lock_guard lock(d_->m);
    return d_->stateText;
}

int RelayBridge::threads() { return bridgeThread().running; }
uint64_t RelayBridge::threadsStarted() { return bridgeThread().started; }

}  // namespace montage::output
