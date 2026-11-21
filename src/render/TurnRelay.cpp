#include "render/TurnRelay.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkProxy>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>
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
    kRequestedTransport = 0x0019,
    kMessageIntegritySha256 = 0x001C,
    kPasswordAlgorithm = 0x001D,
    kPasswordAlgorithms = 0x8002,
};
const int kMaxPeers = 32;
const int kLifetimeAsked = 600;          // seconds
const int kPermissionEveryMs = 240000;   // (a permission lasts five minutes)
const int kPermissionAgainMs = 1000;     // a CreatePermission unanswered is asked again this soon after
const int kResendMs[] = {300, 600, 1200, 2400, 1500};  // the waits after each sending of a request: six seconds in all
const int kLookupMs = 5000;   // how long the relay's name is looked up for
const int kReleaseMs = 250;   // how long the address given back waits for the relay's answer, each time

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
QByteArray xorAddress(quint32 ip, quint16 port) {
    QByteArray v;
    put16(v, 0x0001);  // IPv4
    put16(v, port ^ quint16(kCookie >> 16));
    put32(v, ip ^ kCookie);
    return v;
}
bool readXorAddress(const QByteArray& v, quint32& ip, quint16& port) {
    if (v.size() < 8 || quint8(v[1]) != 0x01) return false;
    port = get16(v, 2) ^ quint16(kCookie >> 16);
    ip = get32(v, 4) ^ kCookie;
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
// from: `to` is put in its place, and the answer signed again with the ICE password and its fingerprint worked out
// again. Anything else is left as it is.
void renameMappedAddress(QByteArray& d, quint32 fromIp, quint16 fromPort, quint32 toIp, quint16 toPort, const QByteArray& password) {
    if (d.size() < 20 || get16(d, 0) != 0x0101 || get32(d, 4) != kCookie || 20 + qsizetype(get16(d, 2)) != d.size()) return;
    qsizetype mapped = -1, integrity = -1, fingerprint = -1;
    for (qsizetype at = 20; at + 4 <= d.size();) {
        const quint16 type = get16(d, at);
        const qsizetype size = get16(d, at + 2);
        if (at + 4 + size > d.size()) return;
        if (type == 0x0020 && size == 8 && integrity < 0) mapped = at + 4;
        if (type == kMessageIntegrity && size == 20 && integrity < 0) integrity = at;
        if (type == 0x8028 && size == 4) fingerprint = at;
        at += 4 + ((size + 3) & ~qsizetype(3));
    }
    quint32 ip = 0;
    quint16 port = 0;
    if (mapped < 0 || integrity < 0 || !readXorAddress(d.mid(mapped, 8), ip, port) || ip != fromIp || port != fromPort) return;
    d.replace(mapped, 8, xorAddress(toIp, toPort));
    auto setLength = [&](qsizetype end) {
        const quint16 length = qToBigEndian(quint16(end - 20));
        memcpy(d.data() + 2, &length, 2);
    };
    setLength(integrity + 24);
    d.replace(integrity + 4, 20, QMessageAuthenticationCode::hash(d.left(integrity), password, QCryptographicHash::Sha1));
    if (fingerprint > integrity) {
        setLength(fingerprint + 8);
        const quint32 crc = qToBigEndian(crc32(d.left(fingerprint)) ^ 0x5354554Eu);
        memcpy(d.data() + fingerprint + 4, &crc, 4);
    }
    setLength(d.size());
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

}  // namespace

std::string offerThroughRelay(const std::string& sdp, const std::function<uint16_t(const std::string& ip, uint16_t port)>& alias) {
    return mapLines(sdp, [&](const QString& line) {
        if (!line.startsWith(QLatin1String("a=candidate:"))) return line;
        Candidate c;
        quint32 ip = 0;
        if (!readCandidate(line, c) || c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) != 0 || !isIpv4(c.address, ip)) return QString();
        const uint16_t port = alias(c.address.toStdString(), c.port);
        if (!port) return QString();
        return QStringLiteral("a=candidate:%1 %2 UDP %3 127.0.0.1 %4 typ host").arg(c.foundation, c.component, c.priority).arg(port);
    });
}

std::string answerThroughRelay(const std::string& sdp, const std::string& relayedIp, uint16_t relayedPort, uint16_t* localPort) {
    if (localPort) *localPort = 0;
    bool given = false;
    return mapLines(sdp, [&](const QString& line) {
        if (!line.startsWith(QLatin1String("a=candidate:"))) return line;
        Candidate c;
        if (given || !readCandidate(line, c) || c.transport.compare(QLatin1String("udp"), Qt::CaseInsensitive) != 0 || c.address != QLatin1String("127.0.0.1"))
            return QString();
        given = true;
        if (localPort) *localPort = c.port;
        return QStringLiteral("a=candidate:%1 %2 UDP %3 %4 %5 typ relay raddr 0.0.0.0 rport 0")
            .arg(c.foundation, c.component, c.priority, QString::fromStdString(relayedIp))
            .arg(relayedPort);
    });
}

// The bridge's thread: its sockets and timer live there, and everything below `// (on the thread)` runs there.
struct RelayBridge::Impl {
    std::string host;
    uint16_t port = 0;
    TurnLogin login;

    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    QObject* home = nullptr;  // (under m) what queued calls run on, while the thread's loop runs
    QEventLoop* loop = nullptr;
    std::string relayedIp;    // (under m)
    std::string error;        // (under m)
    std::atomic<quint16> relayedPort{0}, localPort{0};
    std::atomic<bool> localSaid{false};
    std::atomic<quint64> toRelay{0}, fromRelay{0};

    // (on the thread)
    struct Request {
        quint16 method = 0;
        std::vector<std::pair<quint16, QByteArray>> attributes;
        int sends = 0, logins = 0;
        qint64 resendAt = 0;
        std::function<void(const Message* answer)> done;  // (null: no answer in time)
    };
    struct Alias {
        QUdpSocket* socket = nullptr;
        quint32 ip = 0;
        quint16 port = 0;
    };
    QUdpSocket* outer = nullptr;
    QTimer* timer = nullptr;
    std::map<QByteArray, Request> requests;
    std::map<quint64, Alias> aliases;  // by the viewer's address and port
    std::set<quint32> permitted;
    QByteArray realm, nonce, key, algorithms;
    QByteArray md5Key;  // (the key as MD5 makes it, whatever was chosen: libjuice's relay signs its answers with it)
    quint16 algorithm = 0;  // (0: none named, the key is MD5's)
    QByteArray icePassword;  // Montage's end's, once said
    bool allocated = false, asked = false, refreshing = false, lookingUp = false;
    int lifetime = kLifetimeAsked;  // seconds, as the relay last gave it
    // When to ask for the address again, and when the relay lets it go unless it is (now()'s clock, ms).
    qint64 refreshAt = 0, expiresAt = 0, permitAt = 0;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    std::function<void(const std::string&)> allocateDone;

    qint64 now() const { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(); }
    // The relay's LIFETIME in an answer, kept, and when to ask again: half way through it.
    void lifetimeFrom(const Message& answer) {
        if (const QByteArray* l = answer.find(kLifetime); l && l->size() == 4) lifetime = int(std::clamp<quint32>(get32(*l, 0), 20, 3600));
        refreshAt = now() + qint64(lifetime) * 500;
        expiresAt = now() + qint64(lifetime) * 1000;
    }
    // The relay has let the address go (its time ran out unrefreshed, or it no longer knows it): nothing more is
    // carried, and the bridge says why.
    void lapsed(const std::string& why) {
        if (!allocated) return;
        allocated = false;
        setError(why);
    }

    void run() {
        QEventLoop l;
        QObject h;
        QUdpSocket o;
        QTimer t;
        o.setProxy(QNetworkProxy::NoProxy);
        outer = &o;
        timer = &t;
        QObject::connect(&o, &QUdpSocket::readyRead, &h, [this] { readOuter(); });
        QObject::connect(&t, &QTimer::timeout, &h, [this] { tick(); });
        t.start(100);
        {
            std::lock_guard lock(m);
            home = &h;
            loop = &l;
        }
        cv.notify_all();
        l.exec();
        {
            std::lock_guard lock(m);
            home = nullptr;
            loop = nullptr;
        }
        // The address is given back (the relay lets it go by itself otherwise). Its answer is waited for a moment,
        // and a nonce the relay calls stale (438) is taken and the address given back once more with it.
        if (allocated) {
            QObject::disconnect(&o, nullptr, &h, nullptr);
            Request r;
            r.method = kRefresh;
            QByteArray zero;
            put32(zero, 0);
            r.attributes.emplace_back(kLifetime, zero);
            for (int round = 0; round < 2; ++round) {
                const QByteArray id = randomId();
                o.write(build(r, id));
                o.waitForBytesWritten(50);
                bool stale = false, answered = false;
                QElapsedTimer waited;
                waited.start();
                while (!answered && waited.elapsed() < kReleaseMs && o.waitForReadyRead(int(std::max<qint64>(1, kReleaseMs - waited.elapsed())))) {
                    while (!answered && o.hasPendingDatagrams()) {
                        QByteArray d(int(o.pendingDatagramSize()), '\0');
                        const qint64 n = o.readDatagram(d.data(), d.size());
                        Message msg;
                        if (n <= 0 || !parse(d.left(n), msg) || msg.id != id || (msg.type & ~0x0110) != kRefresh) continue;
                        answered = true;
                        if (const QByteArray* nonceGiven = msg.find(kNonce); (msg.type & 0x0110) == kError && errorCode(msg) == 438 && nonceGiven) {
                            nonce = *nonceGiven;
                            stale = true;
                        }
                    }
                }
                if (!stale) break;
            }
        }
        requests.clear();
        allocateDone = nullptr;
        for (auto& [k, a] : aliases) delete a.socket;
        aliases.clear();
        outer = nullptr;
        timer = nullptr;
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

    void send(Request r) {
        const QByteArray id = randomId();
        r.sends = 1;
        r.resendAt = now() + kResendMs[0];
        outer->write(build(r, id));
        requests[id] = std::move(r);
    }

    void tick() {
        const qint64 at = now();
        std::vector<QByteArray> lost;
        for (auto& [id, r] : requests) {
            if (at < r.resendAt) continue;
            if (r.sends >= int(std::size(kResendMs))) {
                lost.push_back(id);
                continue;
            }
            r.resendAt = at + kResendMs[r.sends++];
            outer->write(build(r, id));
        }
        for (const QByteArray& id : lost) {
            auto it = requests.find(id);
            if (it == requests.end()) continue;
            Request r = std::move(it->second);
            requests.erase(it);
            if (r.done) r.done(nullptr);
        }
        if (allocated && at >= expiresAt) lapsed("The TURN relay's address for this viewer ran out: the relay did not answer in time to keep it");
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
                    refreshAt = now() + 5000;
                    return fail(answer);
                }
                lifetimeFrom(*answer);
            };
            send(std::move(r));
        }
        if (allocated && at >= permitAt) {
            permitAt = at + kPermissionEveryMs;
            for (const auto& [k, a] : aliases) permit(a.ip, a.port, true);
        }
    }

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
    void fail(const Message* answer) { setError(refusal(answer)); }

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
    // challenge) is as if it had not come: the request is sent again as before.
    void answered(const Message& m, const QByteArray& raw) {
        auto it = requests.find(m.id);
        if (it == requests.end() || (m.type & ~0x0110) != it->second.method) return;
        const int answerCode = (m.type & 0x0110) == kError ? errorCode(m) : 0;
        const bool challenge = answerCode == 401 || answerCode == 438;
        const bool signedAtAll = m.find(kMessageIntegrity) || m.find(kMessageIntegritySha256);
        if (!key.isEmpty() && !challenge && ((m.type & 0x0110) == kSuccess || signedAtAll) && !signedRight(raw)) return;
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
        // The relay's address (its first IPv4 one). A name is looked up without holding this thread, so a bridge
        // let go of meanwhile ends at once.
        quint32 literal = 0;
        if (isIpv4(QString::fromStdString(host), literal)) return allocateAt(QHostAddress(literal));
        // (a lookup gets a few seconds: a name server that never answers is said so)
        lookingUp = true;
        const int lookup = QHostInfo::lookupHost(QString::fromStdString(host), home, [this](const QHostInfo& found) {
            if (!std::exchange(lookingUp, false)) return;
            for (const QHostAddress& a : found.addresses())
                if (a.protocol() == QAbstractSocket::IPv4Protocol) return allocateAt(a);
            allocateAt({});
        });
        QTimer::singleShot(kLookupMs, home, [this, lookup] {
            if (!std::exchange(lookingUp, false)) return;
            QHostInfo::abortHostLookup(lookup);
            allocated_("The TURN relay's address (" + host + ") was not found in time");
        });
    }
    // The one socket to the outside, connected to the relay, and the relay asked for an address.
    void allocateAt(const QHostAddress& address) {
        if (address.isNull()) return allocated_("The TURN relay's address (" + host + ") was not found, or is not IPv4");
        // (bound first: a UDP socket only connected has no port of its own on every system until it sends)
        if (!outer->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) return allocated_("No UDP port to reach the TURN relay from");
        outer->connectToHost(address, port);
        if (!outer->waitForConnected(2000)) return allocated_("The TURN relay cannot be reached: " + outer->errorString().toStdString());
        Request r;
        r.method = kAllocate;
        QByteArray udp;
        put32(udp, 17u << 24);
        r.attributes.emplace_back(kRequestedTransport, udp);
        QByteArray life;
        put32(life, kLifetimeAsked);
        r.attributes.emplace_back(kLifetime, life);
        r.done = [this](const Message* answer) {
            if (!answer || (answer->type & 0x0110) == kError) return allocated_(refusal(answer));
            quint32 ip = 0;
            quint16 p = 0;
            const QByteArray* relayed = answer->find(kXorRelayedAddress);
            if (!relayed || !readXorAddress(*relayed, ip, p)) return allocated_("The TURN relay gave no IPv4 address");
            {
                std::lock_guard lock(m);
                relayedIp = QHostAddress(ip).toString().toStdString();
            }
            relayedPort = p;
            allocated = true;
            lifetimeFrom(*answer);
            permitAt = now() + kPermissionEveryMs;
            for (const auto& [k, a] : aliases) permit(a.ip, a.port, true);
            allocated_({});
        };
        send(std::move(r));
    }
    void allocated_(const std::string& why) {
        if (!why.empty()) setError(why);
        if (auto done = std::exchange(allocateDone, nullptr)) done(why);
    }

    // The relay is asked to let a viewer's address in (again every four minutes, and a second after an asking that
    // went unanswered, so a lost answer leaves no gap of minutes in the picture).
    void permit(quint32 ip, quint16 peerPort, bool again) {
        if (!again && !permitted.insert(ip).second) return;
        if (!allocated) return;
        Request r;
        r.method = kCreatePermission;
        r.attributes.emplace_back(kXorPeerAddress, xorAddress(ip, peerPort));
        r.done = [this](const Message* answer) {
            if (!answer) permitAt = std::min(permitAt, now() + kPermissionAgainMs);
            else if ((answer->type & 0x0110) == kError) fail(answer);
        };
        send(std::move(r));
    }

    Alias* aliasFor(quint32 ip, quint16 peerPort) {
        const quint64 k = (quint64(ip) << 16) | peerPort;
        auto it = aliases.find(k);
        if (it != aliases.end()) return &it->second;
        if (int(aliases.size()) >= kMaxPeers || !home) return nullptr;
        auto* s = new QUdpSocket;
        s->setProxy(QNetworkProxy::NoProxy);
        if (!s->bind(QHostAddress(QHostAddress::LocalHost), 0)) {
            delete s;
            return nullptr;
        }
        QObject::connect(s, &QUdpSocket::readyRead, home, [this, s, ip, peerPort] { readAlias(s, ip, peerPort); });
        Alias& a = aliases[k];
        a = {s, ip, peerPort};
        permit(ip, peerPort, false);
        return &a;
    }

    // What Montage's end sent a viewer's address: wrapped for the relay (only what this computer's loopback sent,
    // from Montage's end's port).
    void readAlias(QUdpSocket* s, quint32 ip, quint16 peerPort) {
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
            if (!icePassword.isEmpty()) renameMappedAddress(d, 0x7F000001u, s->localPort(), ip, peerPort, icePassword);
            QByteArray b;
            b.reserve(d.size() + 40);
            put16(b, quint16(kSend | kIndication));
            put16(b, 0);
            put32(b, kCookie);
            b.append(randomId());
            putAttribute(b, kXorPeerAddress, xorAddress(ip, peerPort));
            putAttribute(b, kDataAttribute, d);
            const quint16 length = qToBigEndian(quint16(b.size() - 20));
            memcpy(b.data() + 2, &length, 2);
            // (counted before it goes, so whoever sees a packet arrive finds it counted)
            if (allocated) {
                ++toRelay;
                outer->write(b);
            }
        }
    }

    // What the relay sent: an answer to a request, or a viewer's packet, passed to Montage's end from that viewer's
    // loopback port.
    void readOuter() {
        while (outer->hasPendingDatagrams()) {
            QByteArray d(int(outer->pendingDatagramSize()), '\0');
            const qint64 n = outer->readDatagram(d.data(), d.size());
            if (n <= 0) continue;
            d.truncate(n);
            Message msg;
            if (!parse(d, msg)) continue;
            if (msg.type == quint16(kData | kIndication)) {
                quint32 ip = 0;
                quint16 peerPort = 0;
                const QByteArray* peerAddress = msg.find(kXorPeerAddress);
                const QByteArray* data = msg.find(kDataAttribute);
                const quint16 to = localPort;
                if (!peerAddress || !data || !readXorAddress(*peerAddress, ip, peerPort) || !to) continue;
                if (Alias* a = aliasFor(ip, peerPort)) {
                    ++fromRelay;
                    a->socket->writeDatagram(*data, QHostAddress(QHostAddress::LocalHost), to);
                }
            } else if (msg.type & 0x0100) {
                answered(msg, d);
            }
        }
    }
};

RelayBridge::RelayBridge(std::string host, uint16_t port, TurnLogin login) : d_(std::make_unique<Impl>()) {
    d_->host = std::move(host);
    d_->port = port;
    d_->login = std::move(login);
    d_->thread = std::thread([d = d_.get()] { d->run(); });
    std::unique_lock lock(d_->m);
    d_->cv.wait(lock, [&] { return d_->home != nullptr; });
}

RelayBridge::~RelayBridge() {
    {
        std::lock_guard lock(d_->m);
        if (d_->loop) QMetaObject::invokeMethod(d_->loop, &QEventLoop::quit, Qt::QueuedConnection);
    }
    if (d_->thread.joinable()) d_->thread.join();
}

void RelayBridge::allocate(std::function<void(const std::string& error)> done) {
    std::lock_guard lock(d_->m);
    if (!d_->home) return;
    QMetaObject::invokeMethod(
        d_->home,
        [d = d_.get(), done = std::move(done)]() mutable {
            if (d->asked) return;
            d->asked = true;
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
    quint32 address = 0;
    if (!port || !isIpv4(QString::fromStdString(ip), address)) return 0;
    QObject* home = nullptr;
    {
        std::lock_guard lock(d_->m);
        home = d_->home;
    }
    if (!home) return 0;
    uint16_t alias = 0;
    QMetaObject::invokeMethod(
        home,
        [&] {
            if (Impl::Alias* a = d_->aliasFor(address, port)) alias = a->socket->localPort();
        },
        Qt::BlockingQueuedConnection);
    return alias;
}

void RelayBridge::setLocal(uint16_t port, const std::string& icePassword) {
    d_->localPort = port;
    d_->localSaid = true;
    std::lock_guard lock(d_->m);
    if (d_->home)
        QMetaObject::invokeMethod(
            d_->home, [d = d_.get(), password = QByteArray::fromStdString(icePassword)] { d->icePassword = password; }, Qt::QueuedConnection);
}

uint64_t RelayBridge::toRelay() const { return d_->toRelay; }
uint64_t RelayBridge::fromRelay() const { return d_->fromRelay; }
std::string RelayBridge::lastError() const {
    std::lock_guard lock(d_->m);
    return d_->error;
}

}  // namespace montage::output
