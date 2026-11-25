// A TURN relay of the tests' own (RFC 8656), whole enough for a browser and Montage's relay bridge to meet through
// it: it listens over UDP, TCP or TLS on 127.0.0.1 or on ::1, takes a login (a fixed one, or those a shared secret
// makes, refused once their time is up), gives each client a relayed UDP port on the same loopback address, and
// carries Send and Data indications and channels' data between each client and the addresses it let in. It signs its
// answers as RFC 8489 has them. The tests can have it change a byte of its answers, send them a byte at a time, put
// a channel's data or bytes that are not TURN into a stream, or close a connection, and it counts what it carried.
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QMessageAuthenticationCode>
#include <QList>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QStringList>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace montage::reviewtest {

class FakeRelay {
public:
    struct Config {
        bool v6 = false;                      // on ::1, giving addresses there, rather than on 127.0.0.1
        // On 127.0.0.1, but speaking as a relay whose addresses are IPv6: its address for a client is given as
        // 2001:db8::7e1a, and a peer's as 2001:db8::1, each standing for this computer's loopback (for computers
        // with no IPv6 of their own: everything about IPv6 but the sockets).
        bool pretendV6 = false;
        bool udp = false, tcp = false, tls = false;  // what it listens on (TCP and TLS are one listener: one or the other)
        std::string user, password;           // a fixed login, or
        std::string secret;                   // the secret logins are made from ("<expiry>:<name>", refused after their time)
        std::string certificatePem, keyPem;   // for TLS
        int lifetime = 600;                   // seconds an address is given for
        bool strictFamily = false;            // an Allocate that names no family is for IPv4, as RFC 8656 has it (440 on ::1)
    };
    // What the tests may turn on while it runs.
    std::atomic<bool> corruptAnswers{false};  // a byte of each signed answer changed after it was signed
    std::atomic<bool> trickle{false};         // a stream's answers a byte at a time
    std::atomic<bool> channelNoise{false};    // a channel's data (to nobody) put before each answer in a stream
    std::atomic<bool> garbage{false};         // bytes that are not TURN in place of the next answer in a stream
    std::atomic<bool> hangUp{false};          // the connection closed in place of the next answer
    std::atomic<bool> silent{false};          // nothing answered, and nothing carried either way
    std::atomic<bool> refuseRefresh{false};   // a Refresh that keeps the address is answered 508 (it keeps it all the same)
    std::atomic<bool> forget{false};          // the next Refresh finds the address gone: 437, as after a restart
    std::atomic<bool> refusePermission{false};  // a CreatePermission answered 403 (the peer is not let in)
    std::atomic<bool> unsignedRefusals{false};  // a refusal sent unsigned, as one made up on the way would be
    // Each answer's attributes changed before it is signed (and each packet passed on from a peer), a different way
    // every time, from this seed: values with bytes changed, cut short or longer, attributes left out, said twice
    // or under another's name. What arrives is well formed and rightly signed, and says something else.
    std::atomic<quint32> mangle{0};
    // What it has done.
    std::atomic<int> connections{0}, allocations{0}, refreshes{0}, released{0}, permissions{0}, channelBinds{0};
    std::atomic<int> loginsRefused{0}, loginsExpired{0}, familiesRefused{0};
    std::atomic<quint64> sendIndications{0}, channelData{0}, toPeers{0}, fromPeers{0};
    std::atomic<quint64> bytesOverUdp{0}, bytesOverStream{0};

    // The Send indications passed on from the client whose relayed port is this one (a client's checks on pairs
    // that were not chosen are its own, not the chosen one's).
    quint64 sendIndicationsFrom(quint16 relayedPort) const {
        std::lock_guard lock(logMutex_);
        return sendsByPort_.value(relayedPort);
    }

    // What it did, in order (the first few hundred things), for a failing test's message.
    QByteArray told() const {
        std::lock_guard lock(logMutex_);
        return log_.join("; ").toUtf8();
    }

    explicit FakeRelay(Config c) : config_(std::move(c)) {}
    ~FakeRelay() { stop(); }
    FakeRelay(const FakeRelay&) = delete;
    FakeRelay& operator=(const FakeRelay&) = delete;

    bool start(std::string* why = nullptr) {
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return begun_; });
        if (why) *why = error_;
        return error_.empty();
    }
    void stop() {
        {
            std::lock_guard lock(m_);
            if (loop_) QMetaObject::invokeMethod(loop_, &QEventLoop::quit, Qt::QueuedConnection);
        }
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    QHostAddress address() const { return QHostAddress(config_.v6 ? QHostAddress::LocalHostIPv6 : QHostAddress::LocalHost); }
    // The address it gives clients as theirs at the relay, and the one it names peers by.
    QHostAddress relayedAddress() const { return config_.pretendV6 ? QHostAddress(QStringLiteral("2001:db8::7e1a")) : address(); }
    QHostAddress peerAddress() const { return config_.pretendV6 ? QHostAddress(QStringLiteral("2001:db8::1")) : address(); }
    // Whether this computer has an IPv6 loopback address at all.
    static bool hasIpv6() {
        QUdpSocket s;
        return s.bind(QHostAddress(QHostAddress::LocalHostIPv6), 0);
    }
    // An answer to a connectivity check as an ICE end makes it (a STUN Binding success response naming `mapped`,
    // signed with the ICE password and fingerprinted).
    static QByteArray bindingAnswer(const QHostAddress& mapped, quint16 port, const QByteArray& id, const QByteArray& password) {
        QByteArray a;
        attribute(a, 0x0020, xorAddress(mapped, port, id));
        return signedMessage(0x0101, id, a, password);
    }
    static QByteArray signedMessage(quint16 type, const QByteArray& id, const QByteArray& a, const QByteArray& password) {
        QByteArray m = message(type, id, a);
        m[2] = char((a.size() + 24) >> 8), m[3] = char((a.size() + 24) & 0xff);
        attribute(m, 0x0008, QMessageAuthenticationCode::hash(m, password, QCryptographicHash::Sha1));
        m[2] = char((m.size() - 20 + 8) >> 8), m[3] = char((m.size() - 20 + 8) & 0xff);
        quint32 c = 0xFFFFFFFFu;
        for (const char ch : m) {
            c ^= quint8(ch);
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        const quint32 f = ~c ^ 0x5354554Eu;
        QByteArray print;
        for (int k = 3; k >= 0; --k) print.append(char(f >> (8 * k)));
        attribute(m, 0x8028, print);
        return m;
    }
    // A connectivity check as a browser's controlling end makes it (a STUN Binding request with USE-CANDIDATE and the
    // tiebreaker given), signed with the ICE password of the end it goes to and fingerprinted.
    static QByteArray bindingRequest(const QByteArray& id, quint64 tiebreaker, const QByteArray& password) {
        QByteArray a, tie;
        attribute(a, 0x0006, "abcd:efgh");
        for (int k = 7; k >= 0; --k) tie.append(char(tiebreaker >> (8 * k)));
        attribute(a, 0x802A, tie);
        attribute(a, 0x0025, QByteArray());
        attribute(a, 0x0024, QByteArray::fromHex("6e001fff"));
        return signedMessage(0x0001, id, a, password);
    }
    // "turn:127.0.0.1:port", "turn:[::1]:port?transport=tcp", "turns:[::1]:port".
    std::string url() const {
        const std::string host = config_.v6 ? "[::1]" : "127.0.0.1";
        return (config_.tls ? "turns:" : "turn:") + host + ":" + std::to_string(port_) + (config_.tcp && !config_.tls ? "?transport=tcp" : "");
    }

private:
    void note(const QString& what) {
        std::lock_guard lock(logMutex_);
        if (log_.size() < 300) log_.append(what);
    }
    struct Client {
        int noted = 0;  // how many of its packets were written down
        QAbstractSocket* stream = nullptr;  // TCP or TLS; null for a UDP client
        QHostAddress from;
        quint16 fromPort = 0;
        QByteArray inbox;
        QUdpSocket* back = nullptr;  // its relayed port, once it has one
        QList<QHostAddress> allowed;
        QHash<quint16, QPair<QHostAddress, quint16>> channels;
        QByteArray key;
    };
    // A listener whose connections are TLS.
    class Listener : public QTcpServer {
    public:
        std::function<void(qintptr)> incoming;

    protected:
        void incomingConnection(qintptr handle) override { incoming(handle); }
    };

    static void put16(QByteArray& b, quint16 v) { b.append(char(v >> 8)).append(char(v & 0xff)); }
    static quint16 get16(const QByteArray& b, qsizetype at) { return quint16(quint8(b[at]) << 8 | quint8(b[at + 1])); }
    static void attribute(QByteArray& b, quint16 type, const QByteArray& value) {
        put16(b, type);
        put16(b, quint16(value.size()));
        b.append(value);
        while (b.size() % 4) b.append('\0');
    }
    static QByteArray xorAddress(const QHostAddress& a, quint16 port, const QByteArray& id) {
        QByteArray v;
        const bool v6 = a.protocol() == QAbstractSocket::IPv6Protocol;
        put16(v, v6 ? 2 : 1);
        put16(v, port ^ 0x2112);
        const QByteArray pad = QByteArray::fromHex("2112a442") + id;
        if (v6) {
            const Q_IPV6ADDR raw = a.toIPv6Address();
            for (int k = 0; k < 16; ++k) v.append(char(raw.c[k] ^ quint8(pad[k])));
        } else {
            const quint32 ip = a.toIPv4Address();
            for (int k = 0; k < 4; ++k) v.append(char(quint8(ip >> (24 - 8 * k)) ^ quint8(pad[k])));
        }
        return v;
    }
    static bool readXorAddress(const QByteArray& v, const QByteArray& id, QHostAddress& a, quint16& port) {
        if (v.size() < 8) return false;
        const bool v6 = quint8(v[1]) == 2;
        if (v.size() < (v6 ? 20 : 8)) return false;
        port = get16(v, 2) ^ 0x2112;
        const QByteArray pad = QByteArray::fromHex("2112a442") + id;
        if (v6) {
            Q_IPV6ADDR raw;
            for (int k = 0; k < 16; ++k) raw.c[k] = quint8(v[4 + k]) ^ quint8(pad[k]);
            a = QHostAddress(raw);
        } else {
            quint32 ip = 0;
            for (int k = 0; k < 4; ++k) ip = (ip << 8) | (quint8(v[4 + k]) ^ quint8(pad[k]));
            a = QHostAddress(ip);
        }
        return true;
    }
    static QByteArray message(quint16 type, const QByteArray& id, const QByteArray& attributes) {
        QByteArray b;
        put16(b, type);
        put16(b, quint16(attributes.size()));
        return b + QByteArray::fromHex("2112a442") + id + attributes;
    }

    void run() {
        QEventLoop loop;
        QObject home;
        QUdpSocket udp;
        Listener listener;
        udp.setProxy(QNetworkProxy::NoProxy);
        listener.setProxy(QNetworkProxy::NoProxy);
        std::string error;
        // (one port number for whatever it listens on)
        bool bound = false;
        for (int tries = 0; tries < 20 && !bound; ++tries) {
            udp.close();
            listener.close();
            if (config_.udp) {
                if (!udp.bind(address(), 0)) continue;
                bound = !(config_.tcp || config_.tls) || listener.listen(address(), udp.localPort());
                port_ = udp.localPort();
            } else {
                bound = listener.listen(address(), 0);
                port_ = listener.serverPort();
            }
        }
        if (!bound) error = "The fake relay found no port on " + address().toString().toStdString();
        home_ = &home;
        QObject::connect(&udp, &QUdpSocket::readyRead, &home, [&] {
            while (udp.hasPendingDatagrams()) {
                QByteArray d(int(udp.pendingDatagramSize()), '\0');
                QHostAddress from;
                quint16 fromPort = 0;
                udp.readDatagram(d.data(), d.size(), &from, &fromPort);
                bytesOverUdp += quint64(d.size());
                const QString k = from.toString() + "/" + QString::number(fromPort);
                auto it = udpClients_.find(k);
                if (it == udpClients_.end()) {
                    auto c = std::make_shared<Client>();
                    c->from = from;
                    c->fromPort = fromPort;
                    it = udpClients_.insert(k, c);
                    ++connections;
                }
                heard(*it.value(), d, &udp);
            }
        });
        listener.incoming = [&](qintptr handle) {
            QAbstractSocket* s = nullptr;
            if (config_.tls) {
                auto* tls = new QSslSocket(&home);
                tls->setSocketDescriptor(handle);
                tls->setLocalCertificate(QSslCertificate(QByteArray::fromStdString(config_.certificatePem), QSsl::Pem));
                QSslKey key(QByteArray::fromStdString(config_.keyPem), QSsl::Ec, QSsl::Pem);
                if (key.isNull()) key = QSslKey(QByteArray::fromStdString(config_.keyPem), QSsl::Rsa, QSsl::Pem);
                tls->setPrivateKey(key);
                tls->setPeerVerifyMode(QSslSocket::VerifyNone);
                tls->startServerEncryption();
                s = tls;
            } else {
                s = new QTcpSocket(&home);
                s->setSocketDescriptor(handle);
            }
            s->setProxy(QNetworkProxy::NoProxy);
            auto c = std::make_shared<Client>();
            c->stream = s;
            c->from = s->peerAddress();
            c->fromPort = s->peerPort();
            streamClients_.insert(s, c);
            ++connections;
            QObject::connect(s, &QAbstractSocket::readyRead, &home, [this, s] {
                auto it = streamClients_.find(s);
                if (it == streamClients_.end()) return;
                std::shared_ptr<Client> c = it.value();
                const QByteArray got = s->readAll();
                bytesOverStream += quint64(got.size());
                c->inbox += got;
                while (c->inbox.size() >= 4) {
                    const bool channel = (quint8(c->inbox[0]) & 0xC0) == 0x40;
                    const qsizetype size = channel ? 4 + ((get16(c->inbox, 2) + 3) & ~3) : 20 + get16(c->inbox, 2);
                    if (c->inbox.size() < size) break;
                    const QByteArray one = c->inbox.left(size);
                    c->inbox.remove(0, size);
                    heard(*c, one, nullptr);
                    if (!streamClients_.contains(s)) return;
                }
            });
            QObject::connect(s, &QAbstractSocket::disconnected, &home, [this, s] {
                auto it = streamClients_.find(s);
                if (it == streamClients_.end()) return;
                // (a connection that ends takes its address with it)
                if (it.value()->back) it.value()->back->deleteLater();
                it.value()->back = nullptr;
                streamClients_.erase(it);
                s->deleteLater();
            });
        };
        {
            std::lock_guard lock(m_);
            error_ = error;
            begun_ = true;
            loop_ = &loop;
        }
        cv_.notify_all();
        if (error.empty()) loop.exec();
        {
            std::lock_guard lock(m_);
            loop_ = nullptr;
        }
        for (auto& c : udpClients_) delete c->back;
        for (auto& c : streamClients_) delete c->back;
        udpClients_.clear();
        streamClients_.clear();
        home_ = nullptr;
    }

    void write(Client& c, QUdpSocket* udp, const QByteArray& b) {
        if (!c.stream) {
            if (udp) udp->writeDatagram(b, c.from, c.fromPort);
            return;
        }
        if (trickle) {
            for (const char ch : b) {
                c.stream->write(&ch, 1);
                c.stream->flush();
            }
        } else {
            c.stream->write(b);
        }
    }

    // The password a login's user name has here: the fixed one, or what the secret makes of the name.
    QByteArray passwordOf(const QByteArray& user) const {
        if (config_.secret.empty()) return user == QByteArray::fromStdString(config_.user) ? QByteArray::fromStdString(config_.password) : QByteArray();
        return QMessageAuthenticationCode::hash(user, QByteArray::fromStdString(config_.secret), QCryptographicHash::Sha1).toBase64();
    }

    // An answer's attributes changed (see `mangle`).
    void mangled(QByteArray& a) {
        const quint32 seed = mangle;
        if (!seed) return;
        QRandomGenerator rng(seed + ++mangles_);
        QList<QPair<quint16, QByteArray>> list;
        for (qsizetype at = 0; at + 4 <= a.size();) {
            const qsizetype n = get16(a, at + 2);
            if (at + 4 + n > a.size()) break;
            list.append({get16(a, at), a.mid(at + 4, n)});
            at += 4 + ((n + 3) & ~qsizetype(3));
        }
        if (list.isEmpty()) return;
        const int which = int(rng.bounded(int(list.size())));
        static const quint16 names[] = {0x0016, 0x000D, 0x0020, 0x0009, 0x0012, 0x0013, 0x0014, 0x0015, 0x0017};
        switch (rng.bounded(7)) {
        case 0:
            if (!list[which].second.isEmpty()) list[which].second[int(rng.bounded(int(list[which].second.size())))] = char(rng.bounded(256));
            break;
        case 1:
            list.removeAt(which);
            break;
        case 2:
            list.insert(which, list[which]);
            break;
        case 3:
            list[which].second.truncate(int(rng.bounded(int(list[which].second.size()) + 1)));
            break;
        case 4:
            list[which].first = names[rng.bounded(int(std::size(names)))];
            break;
        case 5:
            for (int k = int(rng.bounded(1, 64)); k > 0; --k) list[which].second.append(char(rng.bounded(256)));
            break;
        default:  // (as it is)
            break;
        }
        a.clear();
        for (const auto& [type, value] : list) attribute(a, type, value);
    }

    // One message from a client: a Send indication or a channel's data carried on, or a request answered.
    void heard(Client& c, const QByteArray& d, QUdpSocket* udp) {
        if (silent) return;
        if (d.size() >= 4 && (quint8(d[0]) & 0xC0) == 0x40) {
            const quint16 number = get16(d, 0);
            const qsizetype size = get16(d, 2);
            if (c.back && c.channels.contains(number) && 4 + size <= d.size()) {
                ++channelData;
                ++toPeers;
                c.back->writeDatagram(d.mid(4, size), real(c.channels[number].first), c.channels[number].second);
            }
            return;
        }
        if (d.size() < 20 || d.mid(4, 4) != QByteArray::fromHex("2112a442")) return;
        const quint16 type = get16(d, 0);
        const QByteArray id = d.mid(8, 12);
        const qsizetype end = std::min<qsizetype>(d.size(), 20 + get16(d, 2));
        QByteArray user, data, peerRaw;
        qsizetype integrityAt = -1;
        quint32 lifetime = 0;
        bool lifetimeGiven = false;
        int family = 0;
        quint16 channel = 0;
        for (qsizetype at = 20; at + 4 <= end;) {
            const quint16 t = get16(d, at);
            const qsizetype n = get16(d, at + 2);
            if (at + 4 + n > end) return;
            const QByteArray value = d.mid(at + 4, n);
            if (t == 0x0006) user = value;
            if (t == 0x0013) data = value;
            if (t == 0x0012) peerRaw = value;
            if (t == 0x0008 && integrityAt < 0) integrityAt = at;
            if (t == 0x000D && n == 4) lifetime = qFromBigEndian<quint32>(value.constData()), lifetimeGiven = true;
            if (t == 0x0017 && n >= 1) family = quint8(value[0]) == 2 ? 6 : 4;
            if (t == 0x000C && n >= 2) channel = get16(value, 0);
            at += 4 + ((n + 3) & ~qsizetype(3));
        }
        QHostAddress peer;
        quint16 peerPort = 0;
        const bool peerGiven = !peerRaw.isEmpty() && readXorAddress(peerRaw, id, peer, peerPort);
        if (type == 0x0016) {  // a Send indication
            const bool may = c.back && peerGiven && c.allowed.contains(peer);
            if (c.noted++ < 40) {
                QString kinds;
                if (data.size() >= 20 && data.mid(4, 4) == QByteArray::fromHex("2112a442"))
                    for (qsizetype at = 20; at + 4 <= data.size(); at += 4 + ((get16(data, at + 2) + 3) & ~3)) kinds += QString(" %1/%2").arg(get16(data, at), 4, 16, QChar('0')).arg(get16(data, at + 2));
                note(QStringLiteral("%1 sends %2 bytes to %3:%4%5%6").arg(c.fromPort).arg(data.size()).arg(peer.toString()).arg(peerPort).arg(may ? "" : " (not allowed)", kinds));
            }
            if (may) {
                ++sendIndications;
                {
                    std::lock_guard lock(logMutex_);
                    ++sendsByPort_[c.back->localPort()];
                }
                ++toPeers;
                c.back->writeDatagram(data, real(peer), peerPort);
            }
            return;
        }
        if (type & 0x0110) return;  // (not a request)
        const QByteArray realm = "fake-relay";
        auto challenge = [&](const QByteArray& code) {
            QByteArray a;
            attribute(a, 0x0009, QByteArray::fromHex("0000") + code + "Unauthorized");
            attribute(a, 0x0014, realm);
            attribute(a, 0x0015, "a nonce of the fake relay's");
            write(c, udp, message(type | 0x0110, id, a));
        };
        if (integrityAt < 0) return challenge(QByteArray::fromHex("0401"));
        // The login: its key is the MD5 of user:realm:password, and the request's HMAC-SHA1 under it must be right.
        const QByteArray key = QCryptographicHash::hash(user + ":" + realm + ":" + passwordOf(user), QCryptographicHash::Md5);
        QByteArray covered = d.left(integrityAt);
        const quint16 length = quint16(integrityAt - 20 + 24);
        covered[2] = char(length >> 8), covered[3] = char(length & 0xff);
        bool right = !passwordOf(user).isEmpty() && QMessageAuthenticationCode::hash(covered, key, QCryptographicHash::Sha1) == d.mid(integrityAt + 4, 20);
        if (right && !config_.secret.empty() && user.left(user.indexOf(':')).toLongLong() <= QDateTime::currentSecsSinceEpoch()) {
            right = false;
            ++loginsExpired;
        }
        if (!right) {
            ++loginsRefused;
            return challenge(QByteArray::fromHex("0401"));
        }
        c.key = key;
        QByteArray a;
        bool refused = false;
        auto refuse = [&](const char* code, const char* reason) {
            refused = true;
            attribute(a, 0x0009, QByteArray::fromHex("0000") + QByteArray::fromHex(code) + reason);
        };
        const QByteArray life = QByteArray::fromHex("0000") + char(config_.lifetime >> 8) + char(config_.lifetime & 0xff);
        if (type == 0x0003) {
            const int mine = config_.v6 || config_.pretendV6 ? 6 : 4;
            const int asked = family ? family : config_.strictFamily ? 4 : mine;
            if (asked != mine) {
                ++familiesRefused;
                refuse("0428", "Address Family not Supported");  // 440
            } else if (c.back) {
                refuse("0425", "Allocation Mismatch");  // 437
            } else {
                c.back = new QUdpSocket(home_);
                c.back->setProxy(QNetworkProxy::NoProxy);
                c.back->bind(address(), 0);
                Client* client = &c;
                QObject::connect(c.back, &QUdpSocket::readyRead, home_, [this, client, udp] { fromPeer(*client, udp); });
                ++allocations;
                attribute(a, 0x0016, xorAddress(relayedAddress(), c.back->localPort(), id));
                attribute(a, 0x000D, life);
                attribute(a, 0x0020, xorAddress(c.from, c.fromPort, id));
            }
        } else if (type == 0x0004) {
            if (!c.back) {
                refuse("0425", "Allocation Mismatch");
            } else if (lifetimeGiven && lifetime == 0) {
                ++released;
                c.back->deleteLater();
                c.back = nullptr;
                c.allowed.clear();
                c.channels.clear();
                attribute(a, 0x000D, QByteArray(4, '\0'));
            } else if (forget.exchange(false)) {
                c.back->deleteLater();
                c.back = nullptr;
                c.allowed.clear();
                c.channels.clear();
                refuse("0425", "Allocation Mismatch");
            } else if (refuseRefresh) {
                ++refreshes;
                refuse("0508", "Insufficient Capacity");
            } else {
                ++refreshes;
                attribute(a, 0x000D, life);
            }
        } else if (type == 0x0008 || type == 0x0009) {
            if (!c.back || !peerGiven) {
                refuse("0400", "Bad Request");
            } else if ((peer.protocol() == QAbstractSocket::IPv6Protocol) != (config_.v6 || config_.pretendV6)) {
                refuse("042b", "Peer Address Family Mismatch");  // 443
            } else if (type == 0x0008 && refusePermission) {
                refuse("0403", "Forbidden");
            } else {
                if (!c.allowed.contains(peer)) c.allowed.append(peer);
                if (type == 0x0009) {
                    c.channels[channel] = {peer, peerPort};
                    ++channelBinds;
                } else {
                    ++permissions;
                }
            }
        } else {
            refuse("0400", "Bad Request");
        }
        mangled(a);
        note(QStringLiteral("%1 asks %2%3%4").arg(c.fromPort).arg(type, 4, 16, QChar('0')).arg(peerGiven ? " for " + peer.toString() + ":" + QString::number(peerPort) : QString()).arg(refused ? " (refused)" : ""));
        QByteArray answer = message(type | (refused ? 0x0110 : 0x0100), id, a);
        if (!refused || !unsignedRefusals) {
            answer[2] = char((a.size() + 24) >> 8), answer[3] = char((a.size() + 24) & 0xff);
            attribute(answer, 0x0008, QMessageAuthenticationCode::hash(answer, key, QCryptographicHash::Sha1));
        }
        // (a byte of the signed answer changed on its way, as a box in the middle might: the relayed port's, the
        // lifetime's or the error's, whichever attribute comes first)
        if (corruptAnswers && answer.size() > 28) answer[27] = char(answer[27] ^ 0x01);
        if (c.stream && hangUp.exchange(false)) return c.stream->disconnectFromHost();
        if (c.stream && garbage.exchange(false)) return write(c, udp, QByteArray::fromHex("c0ffee00c0ffee00c0ffee00"));
        if (c.stream && channelNoise) write(c, udp, QByteArray::fromHex("4abc0005") + "noise" + QByteArray(3, '\0'));
        write(c, udp, answer);
    }

    // What a peer sent a client's relayed port: on to the client, as a channel's data where one is bound to the peer.
    void fromPeer(Client& c, QUdpSocket* udp) {
        while (c.back && c.back->hasPendingDatagrams()) {
            QByteArray d(int(c.back->pendingDatagramSize()), '\0');
            QHostAddress from;
            quint16 fromPort = 0;
            c.back->readDatagram(d.data(), d.size(), &from, &fromPort);
            if (silent) continue;
            if (from.protocol() == QAbstractSocket::IPv6Protocol && !config_.v6) from = QHostAddress(from.toIPv4Address());
            if (config_.pretendV6) from = peerAddress();
            if (c.noted++ < 40)
                note(QStringLiteral("%1's port %2 hears %3 bytes from %4:%5%6").arg(c.fromPort).arg(c.back->localPort()).arg(d.size()).arg(from.toString()).arg(fromPort).arg(c.allowed.contains(from) ? "" : " (not allowed)"));
            if (!c.allowed.contains(from)) continue;
            ++fromPeers;
            quint16 number = 0;
            for (auto it = c.channels.begin(); it != c.channels.end(); ++it)
                if (it.value().first == from && it.value().second == fromPort) number = it.key();
            if (number) {
                QByteArray b;
                put16(b, number);
                put16(b, quint16(d.size()));
                b += d;
                while (c.stream && b.size() % 4) b.append('\0');
                write(c, udp, b);
            } else {
                // (a transaction id of its own for each, as RFC 8489 has it for indications too)
                QByteArray id(12, '\0');
                for (char& ch : id) ch = char(QRandomGenerator::global()->bounded(256));
                QByteArray a;
                attribute(a, 0x0012, xorAddress(from, fromPort, id));
                attribute(a, 0x0013, d);
                mangled(a);
                write(c, udp, message(0x0017, id, a));
            }
        }
    }

    // Where a peer named in a request really is.
    QHostAddress real(const QHostAddress& peer) const { return config_.pretendV6 ? QHostAddress(QHostAddress::LocalHost) : peer; }

    Config config_;
    quint32 mangles_ = 0;
    mutable std::mutex logMutex_;
    QStringList log_;
    QHash<quint16, quint64> sendsByPort_;  // Send indications passed on, by the sender's relayed port (under logMutex_)
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool begun_ = false;
    std::string error_;
    QEventLoop* loop_ = nullptr;
    QObject* home_ = nullptr;
    std::atomic<int> port_{0};
    QHash<QString, std::shared_ptr<Client>> udpClients_;
    QHash<QAbstractSocket*, std::shared_ptr<Client>> streamClients_;
};

}  // namespace montage::reviewtest
