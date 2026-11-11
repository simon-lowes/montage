#include "Nmos.h"

#include "media/JpegXs.h"
#include "render/St2110Receive.h"

#include <QDnsLookup>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace montage::nmos {

namespace {

// (the namespace Montage's NMOS ids are made in, with the host name and label)
const QUuid kNamespace(QStringLiteral("{8a3c5e0e-2b8d-5f0a-9d61-6f2c4d1b7e35}"));
const char* const kStreams[3] = {"video", "audio", "data"};

QString text(const char* s) { return QString::fromUtf8(s); }

QString idText(const QUuid& u) { return u.toString(QUuid::WithoutBraces); }

QByteArray toJson(const QJsonValue& v) {
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    return QByteArrayLiteral("null");
}

Node::Response jsonResponse(int code, const QJsonValue& v) {
    Node::Response r;
    r.code = code;
    r.body = toJson(v);
    return r;
}

Node::Response errorResponse(int code, const QString& error) {
    return jsonResponse(code, QJsonObject{{"code", code}, {"error", error}, {"debug", QJsonValue::Null}});
}

QJsonArray listing(std::initializer_list<const char*> names) {
    QJsonArray a;
    for (const char* n : names) a.append(QString::fromLatin1(n));
    return a;
}

const char* reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 423: return "Locked";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
    }
    return "Unknown";
}

bool isIpv4(const QString& s) {
    QHostAddress a;
    return a.setAddress(s) && a.protocol() == QAbstractSocket::IPv4Protocol && s.count('.') == 3;
}

QJsonObject rational(Rational r) { return QJsonObject{{"numerator", r.num}, {"denominator", r.den}}; }

// The search domain the system's resolver uses (resolv.conf's "search", else "domain").
QString systemDomain() {
    // (the operating system's search domain: Windows' from its settings, elsewhere resolv.conf's)
    const QString known = QHostInfo::localDomainName();
    if (!known.isEmpty()) return known;
    QFile f(QStringLiteral("/etc/resolv.conf"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString domain;
    for (const QByteArray& raw : f.readAll().split('\n')) {
        const QStringList w = QString::fromUtf8(raw).simplified().split(' ', Qt::SkipEmptyParts);
        if (w.size() < 2) continue;
        if (w[0] == "search") return w[1];
        if (w[0] == "domain") domain = w[1];
    }
    return domain;
}

// The network parts of an SDP transport file: each path's destination (address and port) and the source it filters
// for (two paths in an ST 2022-7 "DUP" group, else the first media description's one).
struct SdpPath {
    QString address, source;
    int port = 0;
};
std::vector<SdpPath> sdpPaths(const QString& sdp) {
    struct Media {
        SdpPath path;
        QString mid;
    };
    std::vector<Media> media;
    QStringList dup;
    QString sessionAddress;
    for (QString line : sdp.split('\n')) {
        line = line.trimmed();
        if (line.size() < 2 || line[1] != '=') continue;
        const QString v = line.mid(2);
        const QStringList w = v.split(' ', Qt::SkipEmptyParts);
        // (the source only from a source filter: the origin's address may be another computer's)
        if (line[0] == 'm' && w.size() >= 2) {
            Media m;
            m.path.port = w[1].section('/', 0, 0).toInt();
            m.path.address = sessionAddress;
            media.push_back(m);
        } else if (line[0] == 'c' && w.size() >= 3 && !media.empty()) {
            media.back().path.address = w[2].section('/', 0, 0);
        } else if (line[0] == 'c' && w.size() >= 3) {
            // (a session-level connection: every description's that gives none)
            sessionAddress = w[2].section('/', 0, 0);
        } else if (line[0] == 'a') {
            if (v.startsWith(QLatin1String("group:DUP"))) dup = v.mid(9).split(' ', Qt::SkipEmptyParts);
            else if (v.startsWith(QLatin1String("mid:")) && !media.empty()) media.back().mid = v.mid(4).trimmed();
            else if (v.startsWith(QLatin1String("source-filter:")) && !media.empty() && w.size() >= 6 && w[1] == QLatin1String("incl"))
                media.back().path.source = w[5];
        }
    }
    std::vector<SdpPath> out;
    if (dup.size() >= 2) {
        for (int k = 0; k < 2; ++k)
            for (const Media& m : media)
                if (m.mid == dup[k]) out.push_back(m.path);
        if (out.size() == 2) return out;
        out.clear();
    }
    if (!media.empty()) out.push_back(media.front().path);
    return out;
}

}  // namespace

QString taiText(int64_t ns) {
    if (ns < 0) ns = 0;
    return QStringLiteral("%1:%2").arg(ns / 1000000000).arg(ns % 1000000000);
}

bool parseTai(const QString& text, int64_t& ns) {
    static const QRegularExpression re(QStringLiteral("^([0-9]+):([0-9]+)$"));
    const auto m = re.match(text);
    if (!m.hasMatch()) return false;
    bool ok1 = false, ok2 = false;
    const qint64 s = m.captured(1).toLongLong(&ok1), n = m.captured(2).toLongLong(&ok2);
    if (!ok1 || !ok2 || n >= 1000000000 || s > 9000000000LL) return false;
    ns = s * 1000000000 + n;
    return true;
}

// ---------------------------------------------------------------------------

Node::Node(QObject* parent) : QObject(parent) {}

Node::~Node() { stop(); }

bool Node::running() const { return server_ && server_->isListening(); }

int Node::port() const { return running() ? int(server_->serverPort()) : 0; }

QString Node::apiUrl() const { return running() ? QStringLiteral("http://%1:%2").arg(address_).arg(port()) : QString(); }

QUuid Node::receiverId(int stream) const {
    for (const RConn& r : receivers_)
        if (r.stream == stream) return r.id;
    return {};
}

QUuid Node::senderId(int stream) const { return stream >= 0 && stream < int(connections_.size()) ? connections_[size_t(stream)].id : QUuid(); }

QString Node::registry() const {
    return registryIndex_ >= 0 && registryIndex_ < int(registries_.size()) ? registries_[size_t(registryIndex_)].url : QString();
}

QString Node::newVersion() {
    lastVersion_ = std::max(st2110::ptpNowNs(), lastVersion_ + 1);
    return taiText(lastVersion_);
}

bool Node::start(const NodeOptions& o, QString* error) {
    stop();
    o_ = o;
    stopping_ = false;
    mdnsRetryMs_ = 2000;
    run_ = new QObject(this);
    const QString host = o.hostname.isEmpty() ? QHostInfo::localHostName() : o.hostname;
    o_.hostname = host;
    auto id = [&](const QString& what) { return QUuid::createUuidV5(kNamespace, QStringLiteral("%1/%2/%3").arg(host, o.label, what)); };
    nodeId_ = id(QStringLiteral("node"));
    deviceId_ = id(QStringLiteral("device"));
    for (int i = 0; i < 3; ++i) {
        sourceIds_[i] = id(QStringLiteral("source/") + kStreams[i]);
        flowIds_[i] = id(QStringLiteral("flow/") + kStreams[i]);
    }
    // The address the APIs are found at: the one given, else the sending interface's, else this computer's first.
    address_ = o.address;
    if (address_.isEmpty() && !options_.interfaceAddress.empty()) address_ = QString::fromStdString(options_.interfaceAddress);
    if (address_.isEmpty())
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
                address_ = a.toString();
                break;
            }
    if (address_.isEmpty()) address_ = QStringLiteral("127.0.0.1");
    server_ = new QTcpServer(this);
    if (!server_->listen(QHostAddress::Any, quint16(o.port))) {
        if (error) *error = text("The NMOS APIs cannot listen on port %1: %2").arg(o.port).arg(server_->errorString());
        delete server_;
        server_ = nullptr;
        return false;
    }
    // The APIs listen on every interface, so what anyone on the network can hold open is bounded: so many connections
    // in all and from one address (an IPv6 /64 counted as one, as one computer can have many addresses in it), a
    // request's headers and body, how long a connection may sit without finishing one (a request trickled in a byte at
    // a time is cut off), and how long it is kept alive at all. When every place is taken, a newcomer takes the place of
    // a connection of the address holding the most, so a few hosts cannot keep the others out: its quietest between
    // requests, else (all of them mid-request) its quietest.
    // An address at its own limit has its quietest connection between requests let go for a newcomer (a client's pool
    // opens another when it needs one); only when all of them are mid-request is the newcomer refused.
    constexpr int kMaxClients = 64, kMaxClientsFrom = 4, kIdleMs = 15000, kLifeMs = 60000;
    constexpr qsizetype kMaxHeader = 64 << 10, kMaxBody = 1 << 20;
    struct Client {
        QString from;
        qint64 lastRequest = 0;
        std::shared_ptr<QByteArray> buffer;  // (what has come of the request it is on: empty between requests)
        uint64_t order = 0;                  // (when it came, among the others: the earlier is the quieter of two alike)
    };
    auto arrivals = std::make_shared<uint64_t>(0);
    auto clients = std::make_shared<std::map<QTcpSocket*, Client>>();
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    clientCount_ = [clients] { return int(clients->size()); };
    connect(server_, &QTcpServer::newConnection, this, [this, clients, clock, arrivals] {
        while (QTcpSocket* s = server_->nextPendingConnection()) {
            const QHostAddress address = s->peerAddress();
            QString peer = address.toString();
            bool v4 = false;
            address.toIPv4Address(&v4);
            if (!v4 && address.protocol() == QAbstractSocket::IPv6Protocol) {
                Q_IPV6ADDR ip = address.toIPv6Address();
                for (int i = 8; i < 16; ++i) ip[i] = 0;
                peer = QHostAddress(ip).toString() + "/64";
            }
            std::map<QString, int> count;
            for (const auto& [socket, c] : *clients) ++count[c.from];
            if (count[peer] >= kMaxClientsFrom) {
                QTcpSocket* quietest = nullptr;
                std::pair<qint64, uint64_t> quiet;
                for (const auto& [socket, c] : *clients)
                    if (c.from == peer && c.buffer && c.buffer->isEmpty() && socket->bytesToWrite() == 0 && socket->bytesAvailable() == 0 &&
                        (!quietest || std::pair(c.lastRequest, c.order) < quiet))
                        quietest = socket, quiet = {c.lastRequest, c.order};
                if (!quietest) {
                    s->abort();
                    s->deleteLater();
                    continue;
                }
                clients->erase(quietest);
                quietest->disconnectFromHost();  // (closed in turn, so the client sees it gone before it reuses it)
                --count[peer];
            }
            if (int(clients->size()) >= kMaxClients) {
                // (of the address holding the most, one between requests first, closed in turn: a request on its way,
                // or its answer, is cut off only when every one of that address's connections has one)
                QTcpSocket* quietest = nullptr;
                std::tuple<int, bool, qint64, uint64_t> quiet;
                for (const auto& [socket, c] : *clients) {
                    const bool busy = !(c.buffer && c.buffer->isEmpty() && socket->bytesToWrite() == 0 && socket->bytesAvailable() == 0);
                    const std::tuple<int, bool, qint64, uint64_t> key{-count[c.from], busy, c.lastRequest, c.order};
                    if (!quietest || key < quiet) quietest = socket, quiet = key;
                }
                if (quietest) {
                    clients->erase(quietest);
                    if (std::get<1>(quiet)) quietest->abort();
                    else quietest->disconnectFromHost();
                }
            }
            auto buffer = std::make_shared<QByteArray>();
            (*clients)[s] = {peer, clock->elapsed(), buffer, ++*arrivals};
            auto retire = std::make_shared<bool>(false);
            auto idle = new QTimer(s);
            idle->setSingleShot(true);
            idle->start(kIdleMs);
            connect(idle, &QTimer::timeout, s, &QTcpSocket::abort);
            // (kept alive a minute: then closed after the request it is on, or at once between requests)
            auto life = new QTimer(s);
            life->setSingleShot(true);
            life->start(kLifeMs);
            connect(life, &QTimer::timeout, s, [s, buffer, retire] {
                *retire = true;
                if (buffer->isEmpty()) s->disconnectFromHost();
            });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QObject::destroyed, this, [clients, s] { clients->erase(s); });
            connect(s, &QTcpSocket::readyRead, s, [this, s, buffer, idle, retire, clients, clock] {
                if (stopping_) return;  // (nothing more taken while it unregisters)
                *buffer += s->readAll();
                if (buffer->size() > kMaxHeader + kMaxBody) {
                    s->abort();
                    return;
                }
                // Each whole request in turn (HTTP/1.1, kept alive).
                for (;;) {
                    const int end = buffer->indexOf("\r\n\r\n");
                    if ((end < 0 && buffer->size() > kMaxHeader) || end > kMaxHeader) {
                        s->write("HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        s->disconnectFromHost();
                        buffer->clear();
                        return;
                    }
                    if (end < 0) return;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
                    qsizetype length = 0;
                    bool close = request.value(2) == "HTTP/1.0", lengthOk = true;
                    for (qsizetype i = 1; i < lines.size(); ++i) {
                        const int colon = lines[i].indexOf(':');
                        if (colon < 0) continue;
                        const QByteArray name = lines[i].left(colon).trimmed().toLower(), value = lines[i].mid(colon + 1).trimmed();
                        if (name == "content-length") {
                            const qlonglong n = value.toLongLong(&lengthOk);
                            lengthOk = lengthOk && n >= 0 && n <= kMaxBody;
                            length = lengthOk ? qsizetype(n) : 0;
                        }
                        if (name == "connection") close = value.toLower() == "close";
                    }
                    if (!lengthOk) {  // (a length that cannot be one: refused, and the connection closed)
                        s->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        s->disconnectFromHost();
                        buffer->clear();
                        return;
                    }
                    if (buffer->size() < end + 4 + length) return;
                    const QByteArray body = buffer->mid(end + 4, length);
                    buffer->remove(0, end + 4 + length);
                    idle->start(kIdleMs);
                    if (auto c = clients->find(s); c != clients->end()) c->second.lastRequest = clock->elapsed();
                    close = close || *retire;
                    const QByteArray method = request.value(0);
                    Response r = request.size() < 3 ? errorResponse(400, QStringLiteral("Not an HTTP request"))
                                                    : handle(method, QUrl::fromPercentEncoding(request.value(1)), body);
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.code) + " " + reasonPhrase(r.code) + "\r\n";
                    if (!r.body.isEmpty() || r.code != 204) out += "Content-Type: " + r.contentType + "\r\n";
                    out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
                    out += "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, PUT, POST, PATCH, HEAD, OPTIONS, DELETE\r\n"
                           "Access-Control-Allow-Headers: Content-Type, Accept\r\n"
                           "Access-Control-Max-Age: 3600\r\n";
                    for (const auto& [name, value] : r.headers) out += name + ": " + value + "\r\n";
                    if (close) out += "Connection: close\r\n";
                    out += "\r\n";
                    if (method != "HEAD") out += r.body;
                    s->write(out);
                    if (close) {
                        s->disconnectFromHost();
                        return;
                    }
                }
            });
        }
    });
    // The senders' connections: as the output sends now.
    connections_.clear();
    for (int i = 0; i < (o.senders ? 3 : 0); ++i) {
        Connection c;
        c.id = id(QStringLiteral("sender/") + kStreams[i]);
        c.stream = i;
        c.receiverId = c.stagedReceiverId = QJsonValue::Null;
        c.mode = c.requestedTime = QJsonValue::Null;
        connections_.push_back(c);
    }
    // The receivers: not connected, until a controller connects them.
    receivers_.clear();
    for (int i = 0; i < (o.receivers ? 3 : 0); ++i) {
        RConn r;
        r.id = id(QStringLiteral("receiver/") + kStreams[i]);
        r.stream = i;
        r.version = newVersion();
        receivers_.push_back(r);
    }
    nodeVersion_ = newVersion();
    deviceVersion_ = newVersion();
    for (int i = 0; i < 3; ++i) {
        sourceVersion_[i] = newVersion();
        flowVersion_[i] = newVersion();
    }
    setStreams(video_, audio_, options_, active_);
    std::fill(std::begin(mdnsVersions_), std::end(mdnsVersions_), 0);
    mdnsError_.clear();
    if (o.announce || o.discover) startMdns();
    net_ = new QNetworkAccessManager(this);
    heartbeat_ = new QTimer(this);
    heartbeat_->setInterval(o.heartbeatMs);
    connect(heartbeat_, &QTimer::timeout, this, &Node::heartbeat);
    rediscover_ = new QTimer(this);
    rediscover_->setSingleShot(true);
    connect(rediscover_, &QTimer::timeout, this, &Node::discover);
    if (o.discover) QTimer::singleShot(0, this, &Node::discover);
    return true;
}

void Node::stop() {
    if (!server_ || stopping_) return;  // (also when called again while it waits for the registry)
    stopping_ = true;
    // Nothing more taken or changed: the APIs closed, scheduled activations dropped, the heartbeat and DNS-SD stopped.
    server_->close();
    for (Connection& c : connections_) {
        delete c.timer;
        c.timer = nullptr;
    }
    for (RConn& r : receivers_) {
        delete r.timer;
        r.timer = nullptr;
    }
    delete heartbeat_;
    heartbeat_ = nullptr;
    delete rediscover_;
    rediscover_ = nullptr;
    if (o_.announce) announceNode();  // (a goodbye: its records' times to live 0)
    delete run_;  // (the multicast DNS socket and timers with it)
    run_ = nullptr;
    mdns_.reset();
    lookups_ = 0;
    // Then unregistered (a short wait: the node goes either way).
    if (registered_ && net_ && !registry().isEmpty()) {
        QNetworkRequest req(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/resource/nodes/") + idText(nodeId_)));
        req.setTransferTimeout(1000);
        QNetworkReply* reply = net_->deleteResource(req);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        reply->deleteLater();
    }
    connections_.clear();
    receivers_.clear();
    delete net_;
    net_ = nullptr;
    delete server_;
    server_ = nullptr;
    const bool was = registered_;
    registered_ = registering_ = false;
    registryIndex_ = -1;
    queue_.clear();
    if (was) if (onRegistered) onRegistered(false);
}

// ---- What the senders send.

st2110::Endpoint Node::endpointOf(int stream, int leg) const {
    if (leg == 1) return stream == 0 ? options_.video2 : stream == 1 ? options_.audio2 : options_.anc2;
    return stream == 0 ? options_.video : stream == 1 ? options_.audio : options_.anc;
}

QString Node::sourceAddress(int leg) const {
    if (leg == 1 && !options_.interfaceAddress2.empty()) return QString::fromStdString(options_.interfaceAddress2);
    if (!options_.interfaceAddress.empty()) return QString::fromStdString(options_.interfaceAddress);
    return address_;
}

QString Node::interfaceName(int leg) const {
    const QString at = sourceAddress(leg);
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces())
        for (const QNetworkAddressEntry& e : i.addressEntries())
            if (e.ip().toString() == at) return i.name();
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces())
        if ((i.flags() & QNetworkInterface::IsUp) && !(i.flags() & QNetworkInterface::IsLoopBack)) return i.name();
    return QStringLiteral("lo");
}

void Node::setStreams(const st2110::VideoFormat& video, const st2110::AudioFormat& audio, const st2110::SenderOptions& o, bool active) {
    const bool formatChanged = video.width != video_.width || video.height != video_.height || !(video.fps == video_.fps) ||
                               video.interlaced != video_.interlaced || video.depth != video_.depth || video.tcs != video_.tcs ||
                               video.colorimetry != video_.colorimetry || audio.channels != audio_.channels || video.codec != video_.codec ||
                               video.bpp != video_.bpp || video.packetMode != video_.packetMode || video.sampling != video_.sampling;
    video_ = video;
    audio_ = audio;
    options_ = o;
    active_ = active;
    // The active legs: where the output sends now.
    const bool enabled[3] = {o.sendVideo, o.sendAudio, o.sendAnc};
    for (Connection& c : connections_) {
        for (int l = 0; l < 2; ++l) {
            const st2110::Endpoint e = endpointOf(c.stream, l);
            Leg& leg = c.leg[l];
            leg.sourceIp = sourceAddress(l);
            leg.destinationIp = QString::fromStdString(e.address);
            leg.destinationPort = e.port;
            leg.sourcePort = o.sourcePort > 0 ? o.sourcePort : 5004;
            leg.rtpEnabled = enabled[c.stream] && o.pathOn[c.stream][l];
            // (a leg added: staged as it is)
            if (l >= senderLegs()) c.stagedLeg[l] = Leg{};
        }
        c.masterEnable = active && enabled[c.stream];
        c.version = newVersion();
    }
    if (formatChanged)
        for (int i = 0; i < 3; ++i) {
            sourceVersion_[i] = newVersion();
            flowVersion_[i] = newVersion();
        }
    if (!running()) return;
    if (formatChanged) {
        const QJsonArray sources = sourceResources(), flows = flowResources();
        for (const QJsonValue& s : sources) resourceChanged(QStringLiteral("source"), s.toObject());
        for (const QJsonValue& f : flows) resourceChanged(QStringLiteral("flow"), f.toObject());
    }
    for (const QJsonValue& s : senderResources()) resourceChanged(QStringLiteral("sender"), s.toObject());
}

// ---- IS-04 resources.

QJsonObject Node::nodeResource() const {
    QJsonArray interfaces;
    // (the senders' legs' interfaces: the second path's may be another)
    QStringList names{interfaceName(0)};
    if (senderLegs() > 1 && !names.contains(interfaceName(1))) names << interfaceName(1);
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (!names.contains(i.name())) continue;
        QString mac = i.hardwareAddress().toLower().replace(':', '-');
        if (!QRegularExpression(QStringLiteral("^([0-9a-f]{2}-){5}[0-9a-f]{2}$")).match(mac).hasMatch()) mac = QStringLiteral("00-00-00-00-00-00");
        interfaces.append(QJsonObject{{"name", i.name()}, {"chassis_id", mac}, {"port_id", mac}});
    }
    if (interfaces.isEmpty())
        interfaces.append(QJsonObject{{"name", interfaceName()}, {"chassis_id", QJsonValue::Null}, {"port_id", "00-00-00-00-00-00"}});
    // The clock: PTP when the reference names a grandmaster ("ptp=IEEE1588-2008:<gmid>:<domain>") or is traceable to
    // TAI ("ptp=IEEE1588-2008:traceable", the system clock: no grandmaster id to give, so zeros), else the internal one.
    QJsonObject clock{{"name", "clk0"}, {"ref_type", "internal"}};
    const QStringList ref = QString::fromStdString(options_.refclk).split(':');
    static const QRegularExpression gmid(QStringLiteral("^([0-9A-Fa-f]{2}-){7}[0-9A-Fa-f]{2}$"));
    if (ref.size() >= 2 && ref[0] == "ptp=IEEE1588-2008" && gmid.match(ref[1]).hasMatch()) {
        std::string source;
        st2110::ptpNowNs(&source);
        clock = QJsonObject{{"name", "clk0"},        {"ref_type", "ptp"},          {"traceable", false},
                            {"version", "IEEE1588-2008"}, {"gmid", ref[1].toLower()}, {"locked", source == "TAI clock"}};
    } else if (ref.size() == 2 && ref[0] == "ptp=IEEE1588-2008" && ref[1] == "traceable") {
        std::string source;
        st2110::ptpNowNs(&source);
        clock = QJsonObject{{"name", "clk0"},        {"ref_type", "ptp"},                       {"traceable", true},
                            {"version", "IEEE1588-2008"}, {"gmid", "00-00-00-00-00-00-00-00"}, {"locked", source == "TAI clock"}};
    }
    const QString href = QStringLiteral("http://%1:%2/").arg(address_).arg(port());
    return QJsonObject{{"id", idText(nodeId_)},
                       {"version", nodeVersion_},
                       {"label", o_.label},
                       {"description", text("Montage's reference output")},
                       {"tags", QJsonObject{}},
                       {"href", href},
                       {"hostname", o_.hostname},
                       {"api", QJsonObject{{"versions", QJsonArray{"v1.3"}},
                                           {"endpoints", QJsonArray{QJsonObject{{"host", address_}, {"port", port()}, {"protocol", "http"},
                                                                                {"authorization", false}}}}}},
                       {"caps", QJsonObject{}},
                       {"services", QJsonArray{}},
                       {"clocks", QJsonArray{clock}},
                       {"interfaces", interfaces}};
}

QJsonObject Node::deviceResource() const {
    QJsonArray senders, receivers;
    for (const Connection& c : connections_) senders.append(idText(c.id));
    for (const RConn& r : receivers_) receivers.append(idText(r.id));
    return QJsonObject{{"id", idText(deviceId_)},
                       {"version", deviceVersion_},
                       {"label", o_.label},
                       {"description", o_.senders ? text("ST 2110 reference output") : text("ST 2110 capture")},
                       {"tags", QJsonObject{}},
                       {"type", "urn:x-nmos:device:generic"},
                       {"node_id", idText(nodeId_)},
                       {"senders", senders},
                       {"receivers", receivers},
                       {"controls", QJsonArray{QJsonObject{{"href", QStringLiteral("http://%1:%2/x-nmos/connection/v1.1/").arg(address_).arg(port())},
                                                           {"type", "urn:x-nmos:control:sr-ctrl/v1.1"},
                                                           {"authorization", false}}}}};
}

QJsonArray Node::sourceResources() const {
    QJsonArray out;
    if (!o_.senders) return out;
    static const char* const labels[3] = {"Picture", "Sound", "Ancillary data"};
    for (int i = 0; i < 3; ++i) {
        QJsonObject s{{"id", idText(sourceIds_[i])},
                      {"version", sourceVersion_[i]},
                      {"label", o_.label + " " + labels[i]},
                      {"description", QString::fromLatin1(labels[i])},
                      {"tags", QJsonObject{}},
                      {"caps", QJsonObject{}},
                      {"device_id", idText(deviceId_)},
                      {"parents", QJsonArray{}},
                      {"clock_name", "clk0"}};
        if (i == 0) {
            s["format"] = "urn:x-nmos:format:video";
            s["grain_rate"] = rational(video_.fps);
        } else if (i == 1) {
            s["format"] = "urn:x-nmos:format:audio";
            QJsonArray channels;
            for (int c = 0; c < audio_.channels; ++c) {
                if (audio_.channels == 2) channels.append(QJsonObject{{"label", c ? "Right" : "Left"}, {"symbol", c ? "R" : "L"}});
                else channels.append(QJsonObject{{"label", QStringLiteral("Channel %1").arg(c + 1)}, {"symbol", QStringLiteral("U%1").arg(c + 1, 2, 10, QChar('0'))}});
            }
            s["channels"] = channels;
        } else {
            s["format"] = "urn:x-nmos:format:data";
            s["grain_rate"] = rational(video_.fps);
        }
        out.append(s);
    }
    return out;
}

QJsonArray Node::flowResources() const {
    QJsonArray out;
    if (!o_.senders) return out;
    static const char* const labels[3] = {"Picture", "Sound", "Ancillary data"};
    for (int i = 0; i < 3; ++i) {
        QJsonObject f{{"id", idText(flowIds_[i])},
                      {"version", flowVersion_[i]},
                      {"label", o_.label + " " + labels[i]},
                      {"description", QString::fromLatin1(labels[i])},
                      {"tags", QJsonObject{}},
                      {"source_id", idText(sourceIds_[i])},
                      {"device_id", idText(deviceId_)},
                      {"parents", QJsonArray{}}};
        if (i == 0) {
            const int w = video_.width, h = video_.height, d = video_.depth;
            f["format"] = "urn:x-nmos:format:video";
            f["grain_rate"] = rational(video_.fps);
            f["frame_width"] = w;
            f["frame_height"] = h;
            f["interlace_mode"] = video_.interlaced ? "interlaced_tff" : "progressive";
            f["colorspace"] = QString::fromStdString(video_.colorimetry);
            f["transfer_characteristic"] = QString::fromStdString(video_.tcs);
            f["media_type"] = video_.jpegXs() ? "video/jxsv" : "video/raw";
            if (video_.jpegXs()) {
                // (AMWA BCP-006-01: the codestream's profile, level and sublevel as its SDP names them, and its
                // kilobits a second)
                if (!video_.profile.empty()) f["profile"] = QString::fromStdString(video_.profile);
                if (!video_.level.empty()) f["level"] = QString::fromStdString(video_.level);
                if (!video_.sublevel.empty()) f["sublevel"] = QString::fromStdString(video_.sublevel);
                f["bit_rate"] = qint64(std::ceil(double(w) * h * video_.bpp * video_.fps.toDouble() / 1000));
            }
            const int cw = video_.sampling == st2110::Sampling::Ycc422 ? w / 2 : w;
            const bool rgb = video_.sampling == st2110::Sampling::Rgb;
            f["components"] = QJsonArray{QJsonObject{{"name", rgb ? "R" : "Y"}, {"width", w}, {"height", h}, {"bit_depth", d}},
                                         QJsonObject{{"name", rgb ? "G" : "Cb"}, {"width", cw}, {"height", h}, {"bit_depth", d}},
                                         QJsonObject{{"name", rgb ? "B" : "Cr"}, {"width", cw}, {"height", h}, {"bit_depth", d}}};
        } else if (i == 1) {
            f["format"] = "urn:x-nmos:format:audio";
            f["sample_rate"] = QJsonObject{{"numerator", audio_.rate}, {"denominator", 1}};
            // (ST 2110-31's AES3 subframes: no linear PCM flow, so by its RTP payload's name and no bit depth)
            if (audio_.aes3) {
                f["media_type"] = "audio/AM824";
            } else {
                f["media_type"] = audio_.bits == 16 ? "audio/L16" : "audio/L24";
                f["bit_depth"] = audio_.bits;
            }
        } else {
            f["format"] = "urn:x-nmos:format:data";
            f["grain_rate"] = rational(video_.fps);
            f["media_type"] = "video/smpte291";
            f["DID_SDID"] = QJsonArray{QJsonObject{{"DID", "0x60"}, {"SDID", "0x60"}}, QJsonObject{{"DID", "0x61"}, {"SDID", "0x01"}}};
        }
        out.append(f);
    }
    return out;
}

QJsonObject Node::senderResource(int stream) const {
    const Connection& c = connections_[size_t(stream)];
    static const char* const labels[3] = {"Picture", "Sound", "Ancillary data"};
    const bool multicast = st2110::isMulticast(c.leg[0].destinationIp.toStdString());
    QJsonArray bindings{interfaceName(0)};
    if (senderLegs() > 1) bindings.append(interfaceName(1));
    const bool jxs = stream == 0 && video_.jpegXs();
    QJsonObject out{
        {"id", idText(c.id)},
        {"version", c.version},
        {"label", o_.label + " " + labels[stream]},
        {"description", text("ST 2110-%1").arg(stream == 0 ? (jxs ? "22" : "20") : stream == 1 ? "30" : "40")},
        {"tags", QJsonObject{}},
        {"caps", QJsonObject{}},
        {"flow_id", idText(flowIds_[stream])},
        {"transport", multicast ? "urn:x-nmos:transport:rtp.mcast" : "urn:x-nmos:transport:rtp.ucast"},
        {"device_id", idText(deviceId_)},
        {"manifest_href", c.masterEnable ? QJsonValue(QStringLiteral("http://%1:%2/x-nmos/connection/v1.1/single/senders/%3/transportfile")
                                                          .arg(address_)
                                                          .arg(port())
                                                          .arg(idText(c.id)))
                                         : QJsonValue(QJsonValue::Null)},
        {"interface_bindings", bindings},
        // (parked: no receiver)
        {"subscription", QJsonObject{{"receiver_id", c.masterEnable ? c.receiverId : QJsonValue(QJsonValue::Null)}, {"active", c.masterEnable}}}};
    if (jxs) {
        // (BCP-006-01: the bandwidth its SDP's b=AS gives, its ST 2110-21 type and RFC 9134's packetization mode)
        out["bit_rate"] = qint64(video_.bandwidthKbps);
        out["st2110_21_sender_type"] = "2110TPW";
        out["packet_transmission_mode"] = video_.packetMode == 1 ? "slice_sequential" : "codestream";
    }
    return out;
}

QJsonArray Node::senderResources() const {
    QJsonArray out;
    for (size_t i = 0; i < connections_.size(); ++i) out.append(senderResource(int(i)));
    return out;
}

// ---- IS-05.

QJsonArray Node::legsJson(const Leg* legs, int stream, bool resolve) const {
    QJsonArray out;
    for (int i = 0; i < senderLegs(); ++i) out.append(legJson(legs[i], stream, i, resolve));
    return out;
}

QJsonObject Node::legJson(const Leg& l, int stream, int index, bool resolve) const {
    QJsonObject o{{"source_ip", l.sourceIp}, {"destination_ip", l.destinationIp}, {"source_port", l.sourcePort},
                  {"destination_port", l.destinationPort}, {"rtp_enabled", l.rtpEnabled}};
    if (resolve) {
        const st2110::Endpoint e = endpointOf(stream, index);
        if (l.sourceIp == "auto") o["source_ip"] = sourceAddress(index);
        if (l.destinationIp == "auto") o["destination_ip"] = QString::fromStdString(e.address);
        if (l.sourcePort.isString()) o["source_port"] = 5004;
        if (l.destinationPort.isString()) o["destination_port"] = e.port > 0 ? e.port : 5004;
    }
    return o;
}

QJsonObject Node::connectionJson(const Connection& c, bool staged) const {
    QJsonObject activation;
    if (staged) {
        activation = QJsonObject{{"mode", c.mode}, {"requested_time", c.requestedTime},
                                 {"activation_time", c.mode.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(taiText(c.activationNs))}};
        return QJsonObject{{"receiver_id", c.stagedReceiverId}, {"master_enable", c.stagedMasterEnable}, {"activation", activation},
                           {"transport_params", legsJson(c.stagedLeg, c.stream, false)}};
    }
    activation = QJsonObject{{"mode", c.lastMode.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastMode)},
                             {"requested_time", c.lastRequested.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastRequested)},
                             {"activation_time", c.lastActivation.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastActivation)}};
    return QJsonObject{{"receiver_id", c.receiverId}, {"master_enable", c.masterEnable}, {"activation", activation},
                       {"transport_params", legsJson(c.leg, c.stream, true)}};
}

QJsonArray Node::constraintsJson(int) const {
    QJsonArray out;
    for (int leg = 0; leg < senderLegs(); ++leg) {
        // (each leg from its own interface, when the output names one)
        const std::string& given = leg == 1 && !options_.interfaceAddress2.empty() ? options_.interfaceAddress2 : options_.interfaceAddress;
        QJsonArray sources;
        if (!given.empty()) {
            sources.append(QString::fromStdString(given));
        } else {
            for (const QHostAddress& a : QNetworkInterface::allAddresses())
                if (a.protocol() == QAbstractSocket::IPv4Protocol) sources.append(a.toString());
        }
        out.append(QJsonObject{{"source_ip", QJsonObject{{"enum", sources}}},
                               {"destination_ip", QJsonObject{}},
                               {"source_port", QJsonObject{}},
                               {"destination_port", QJsonObject{}},
                               {"rtp_enabled", QJsonObject{}}});
    }
    return out;
}

QString Node::transportFile(int stream) const {
    const Connection& c = connections_[size_t(stream)];
    // The legs sending: both (ST 2022-7's two descriptions, grouped as duplicates), or the one on.
    std::vector<int> on;
    for (int l = 0; l < senderLegs(); ++l)
        if (c.leg[l].rtpEnabled) on.push_back(l);
    if (on.empty()) on.push_back(0);
    const QJsonObject leg = legJson(c.leg[on[0]], stream, on[0], true);
    st2110::Endpoint to{leg["destination_ip"].toString().toStdString(), leg["destination_port"].toInt()};
    st2110::SdpOptions so;
    so.source = leg["source_ip"].toString().toStdString();
    so.name = o_.label.toStdString();
    so.refclk = options_.refclk;
    so.ttl = options_.ttl;
    so.session = uint64_t(lastVersion_ / 1000000000);
    st2110::Endpoint to2;
    const st2110::Endpoint* second = nullptr;
    if (on.size() > 1) {
        const QJsonObject leg2 = legJson(c.leg[1], stream, 1, true);
        to2 = {leg2["destination_ip"].toString().toStdString(), leg2["destination_port"].toInt()};
        so.source2 = leg2["source_ip"].toString().toStdString();
        second = &to2;
    }
    if (stream == 0) return QString::fromStdString(st2110::videoSdp(video_, to, options_.videoPayload, so, second));
    if (stream == 1) return QString::fromStdString(st2110::audioSdp(audio_, to, options_.audioPayload, so, second));
    return QString::fromStdString(st2110::ancSdp(video_, to, options_.ancPayload, so, {{0x60, 0x60}, {0x61, 0x01}}, second));
}

Node::Response Node::patchStaged(Connection& c, const QJsonObject& patch, bool bulk) {
    (void)bulk;
    for (const QString& key : patch.keys())
        if (key != "receiver_id" && key != "master_enable" && key != "activation" && key != "transport_params")
            return errorResponse(400, text("Unknown property \"%1\"").arg(key));
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    if (patch.contains("receiver_id")) {
        const QJsonValue v = patch["receiver_id"];
        if (!v.isNull() && !(v.isString() && uuid.match(v.toString()).hasMatch())) return errorResponse(400, text("receiver_id is a UUID or null"));
    }
    if (patch.contains("master_enable") && !patch["master_enable"].isBool()) return errorResponse(400, text("master_enable is true or false"));
    // The legs: as many as there are, only the parameters the constraints name, each of its type and allowed.
    Leg legs[2] = {c.stagedLeg[0], c.stagedLeg[1]};
    if (patch.contains("transport_params")) {
        if (!patch["transport_params"].isArray()) return errorResponse(400, text("transport_params is an array"));
        const QJsonArray given = patch["transport_params"].toArray();
        if (given.size() != senderLegs()) return errorResponse(400, senderLegs() == 1 ? text("This sender has one leg") : text("This sender has two legs"));
        for (int i = 0; i < senderLegs(); ++i) {
        if (!given[i].isObject()) return errorResponse(400, text("A leg is an object"));
        Leg& leg = legs[i];
        const QJsonObject p = given[i].toObject();
        const QJsonArray allowedSources = constraintsJson(c.stream)[i].toObject()["source_ip"].toObject()["enum"].toArray();
        for (const QString& key : p.keys()) {
            const QJsonValue v = p[key];
            if (key == "source_ip" || key == "destination_ip") {
                if (!v.isString() || (v.toString() != "auto" && !isIpv4(v.toString())))
                    return errorResponse(400, text("%1 is an IPv4 address or \"auto\"").arg(key));
                if (key == "source_ip" && v.toString() != "auto" && !allowedSources.contains(v)) return errorResponse(400, text("No interface has %1").arg(v.toString()));
                (key == "source_ip" ? leg.sourceIp : leg.destinationIp) = v.toString();
            } else if (key == "source_port" || key == "destination_port") {
                const bool ok = (v.isString() && v.toString() == "auto") ||
                                (v.isDouble() && v.toDouble() == std::floor(v.toDouble()) && v.toInt() >= (key == "source_port" ? 0 : 1) && v.toInt() <= 65535);
                if (!ok) return errorResponse(400, text("%1 is a port or \"auto\"").arg(key));
                (key == "source_port" ? leg.sourcePort : leg.destinationPort) = v.isString() ? v : QJsonValue(v.toInt());
            } else if (key == "rtp_enabled") {
                if (!v.isBool()) return errorResponse(400, text("rtp_enabled is true or false"));
                leg.rtpEnabled = v.toBool();
            } else {
                return errorResponse(400, text("This sender has no parameter \"%1\"").arg(key));
            }
        }
        }
    }
    // Activation.
    QJsonValue mode = QJsonValue::Undefined, requested = QJsonValue::Null;
    if (patch.contains("activation")) {
        if (!patch["activation"].isObject()) return errorResponse(400, text("activation is an object"));
        const QJsonObject a = patch["activation"].toObject();
        for (const QString& key : a.keys())
            if (key != "mode" && key != "requested_time") return errorResponse(400, text("Unknown activation property \"%1\"").arg(key));
        if (!a.contains("mode")) return errorResponse(400, text("activation needs a mode"));
        mode = a["mode"];
        const QString m = mode.toString();
        if (!mode.isNull() && m != "activate_immediate" && m != "activate_scheduled_absolute" && m != "activate_scheduled_relative")
            return errorResponse(400, text("Not an activation mode"));
        requested = a.value("requested_time");
        if (requested.isUndefined()) requested = QJsonValue::Null;
        int64_t ns = 0;
        if (!requested.isNull() && !(requested.isString() && parseTai(requested.toString(), ns)))
            return errorResponse(400, text("requested_time is \"<seconds>:<nanoseconds>\""));
        if ((m == "activate_scheduled_absolute" || m == "activate_scheduled_relative") && requested.isNull())
            return errorResponse(400, text("A scheduled activation needs requested_time"));
    }
    // A scheduled activation pending: nothing changes but cancelling it.
    if (!c.mode.isNull() && !(mode.isNull() && !mode.isUndefined())) return errorResponse(423, text("An activation is scheduled: cancel it first"));
    if (patch.contains("receiver_id")) c.stagedReceiverId = patch["receiver_id"];
    if (patch.contains("master_enable")) c.stagedMasterEnable = patch["master_enable"].toBool();
    c.stagedLeg[0] = legs[0], c.stagedLeg[1] = legs[1];
    if (mode.isNull() && !mode.isUndefined()) {
        // Cancelled.
        delete c.timer;
        c.timer = nullptr;
        c.mode = c.requestedTime = QJsonValue::Null;
        return jsonResponse(200, connectionJson(c, true));
    }
    if (mode.isUndefined()) return jsonResponse(200, connectionJson(c, true));
    const QString m = mode.toString();
    if (m == "activate_immediate") {
        c.requestedTime = QJsonValue::Null;
        activate(c, m);
        QJsonObject r = connectionJson(c, true);
        r["activation"] = QJsonObject{{"mode", m}, {"requested_time", QJsonValue::Null}, {"activation_time", c.lastActivation}};
        return jsonResponse(200, r);
    }
    int64_t at = 0;
    parseTai(requested.toString(), at);
    const int64_t now = st2110::ptpNowNs();
    if (m == "activate_scheduled_relative") at = at > std::numeric_limits<int64_t>::max() - now ? std::numeric_limits<int64_t>::max() : at + now;
    c.mode = m;
    c.requestedTime = requested;
    c.activationNs = at;
    delete c.timer;
    c.timer = new QTimer(this);
    c.timer->setSingleShot(true);
    c.timer->setTimerType(Qt::PreciseTimer);
    const QUuid id = c.id;
    connect(c.timer, &QTimer::timeout, this, [this, id] {
        for (Connection& x : connections_)
            if (x.id == id) {
                // (a time further off than the timer reaches, a day: waited for again)
                const int64_t left = x.activationNs - st2110::ptpNowNs();
                if (left > 0) {
                    x.timer->start(int(std::min<int64_t>((left + 999999) / 1000000, 24 * 3600 * 1000)));
                    return;
                }
                const QString mm = x.mode.toString();
                x.timer->deleteLater();
                x.timer = nullptr;
                activate(x, mm);
                return;
            }
    });
    c.timer->start(int(std::clamp<int64_t>((at - now + 999999) / 1000000, 0, 24 * 3600 * 1000)));
    return jsonResponse(202, connectionJson(c, true));
}

void Node::activate(Connection& c, const QString& mode) {
    c.lastMode = mode;
    c.lastRequested = c.requestedTime.isString() ? c.requestedTime.toString() : QString();
    // (a scheduled one never reads as earlier than asked, should the timer be a little early)
    const bool scheduled = mode == QLatin1String("activate_scheduled_relative") || mode == QLatin1String("activate_scheduled_absolute");
    c.lastActivation = taiText(scheduled ? std::max(st2110::ptpNowNs(), c.activationNs) : st2110::ptpNowNs());
    c.receiverId = c.stagedReceiverId;
    c.masterEnable = c.stagedMasterEnable;
    c.leg[0] = c.stagedLeg[0], c.leg[1] = c.stagedLeg[1];
    c.mode = c.requestedTime = QJsonValue::Null;
    c.version = newVersion();
    applyActive();
    resourceChanged(QStringLiteral("sender"), senderResource(c.stream));
}

void Node::applyActive() {
    // The output's options from the active legs ("auto": as it was).
    st2110::SenderOptions o = options_;
    for (const Connection& c : connections_) {
        bool any = false;
        for (int l = 0; l < senderLegs(); ++l) {
            const QJsonObject leg = legJson(c.leg[l], c.stream, l, true);
            st2110::Endpoint& e = l == 0 ? (c.stream == 0 ? o.video : c.stream == 1 ? o.audio : o.anc)
                                         : (c.stream == 0 ? o.video2 : c.stream == 1 ? o.audio2 : o.anc2);
            e.address = leg["destination_ip"].toString().toStdString();
            e.port = leg["destination_port"].toInt();
            o.pathOn[c.stream][l] = c.leg[l].rtpEnabled;
            any = any || c.leg[l].rtpEnabled;
            if (c.stream == 0) {
                if (c.leg[l].sourceIp != "auto") (l == 0 ? o.interfaceAddress : o.interfaceAddress2) = c.leg[l].sourceIp.toStdString();
                if (l == 0 && c.leg[l].sourcePort.isDouble()) o.sourcePort = c.leg[l].sourcePort.toInt();
            }
        }
        (c.stream == 0 ? o.sendVideo : c.stream == 1 ? o.sendAudio : o.sendAnc) = c.masterEnable && any;
    }
    options_ = o;
    if (onActivated) onActivated(o);
}

// ---- Receivers.

QString Node::legInterface(int index) const {
    if (index == 1 && !o_.interface2.isEmpty()) return o_.interface2;
    return sourceAddress();
}

QJsonObject Node::receiverResource(const RConn& r) const {
    static const char* const labels[3] = {"Picture", "Sound", "Ancillary data"};
    static const char* const formats[3] = {"urn:x-nmos:format:video", "urn:x-nmos:format:audio", "urn:x-nmos:format:data"};
    const QJsonArray types = r.stream == 0   ? (jpegXsAvailable() ? QJsonArray{"video/raw", "video/jxsv"} : QJsonArray{"video/raw"})
                             : r.stream == 1 ? QJsonArray{"audio/L24", "audio/L16"}
                                             : QJsonArray{"video/smpte291"};
    // (each leg bound to its interface)
    auto nameOf = [&](const QString& address) {
        for (const QNetworkInterface& i : QNetworkInterface::allInterfaces())
            for (const QNetworkAddressEntry& e : i.addressEntries())
                if (e.ip().toString() == address) return i.name();
        return interfaceName();
    };
    return QJsonObject{{"id", idText(r.id)},
                       {"version", r.version},
                       {"label", o_.label + " " + labels[r.stream] + " in"},
                       {"description", text("ST 2110-%1 receiver (ST 2022-7)").arg(r.stream == 0 ? (jpegXsAvailable() ? "20 and -22" : "20") : r.stream == 1 ? "30" : "40")},
                       {"tags", QJsonObject{}},
                       {"device_id", idText(deviceId_)},
                       {"transport", "urn:x-nmos:transport:rtp"},
                       {"interface_bindings", QJsonArray{nameOf(legInterface(0)), nameOf(legInterface(1))}},
                       {"subscription", QJsonObject{{"sender_id", r.masterEnable ? r.senderId : QJsonValue(QJsonValue::Null)}, {"active", r.masterEnable}}},
                       {"format", formats[r.stream]},
                       {"caps", QJsonObject{{"media_types", types}}}};
}

QJsonArray Node::receiverResources() const {
    QJsonArray out;
    for (const RConn& r : receivers_) out.append(receiverResource(r));
    return out;
}

QJsonObject Node::rlegJson(const RLeg& l, int index, bool resolve) const {
    QJsonObject o{{"source_ip", l.sourceIp}, {"multicast_ip", l.multicastIp}, {"interface_ip", l.interfaceIp},
                  {"destination_port", l.destinationPort}, {"rtp_enabled", l.rtpEnabled}};
    if (resolve) {
        if (l.interfaceIp == "auto") o["interface_ip"] = legInterface(index);
        if (l.destinationPort.isString()) o["destination_port"] = 5004;
    }
    return o;
}

QJsonObject Node::receiverJson(const RConn& r, bool staged) const {
    if (staged) {
        const QJsonObject activation{{"mode", r.mode}, {"requested_time", r.requestedTime},
                                     {"activation_time", r.mode.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(taiText(r.activationNs))}};
        return QJsonObject{{"sender_id", r.stagedSenderId},
                           {"master_enable", r.stagedMasterEnable},
                           {"activation", activation},
                           {"transport_file", QJsonObject{{"data", r.stagedFileData}, {"type", r.stagedFileType}}},
                           {"transport_params", QJsonArray{rlegJson(r.stagedLeg[0], 0, false), rlegJson(r.stagedLeg[1], 1, false)}}};
    }
    const QJsonObject activation{{"mode", r.lastMode.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.lastMode)},
                                 {"requested_time", r.lastRequested.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.lastRequested)},
                                 {"activation_time", r.lastActivation.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.lastActivation)}};
    return QJsonObject{{"sender_id", r.senderId},
                       {"master_enable", r.masterEnable},
                       {"activation", activation},
                       {"transport_file", QJsonObject{{"data", r.fileData}, {"type", r.fileType}}},
                       {"transport_params", QJsonArray{rlegJson(r.leg[0], 0, true), rlegJson(r.leg[1], 1, true)}}};
}

QJsonArray Node::receiverConstraints() const {
    QJsonArray out;
    for (int i = 0; i < 2; ++i) {
        QJsonArray interfaces;
        const QString own = legInterface(i);
        interfaces.append(own);
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && a.toString() != own) interfaces.append(a.toString());
        out.append(QJsonObject{{"source_ip", QJsonObject{}},
                               {"multicast_ip", QJsonObject{}},
                               {"interface_ip", QJsonObject{{"enum", interfaces}}},
                               {"destination_port", QJsonObject{}},
                               {"rtp_enabled", QJsonObject{}}});
    }
    return out;
}

Node::Response Node::patchReceiver(RConn& r, const QJsonObject& patch) {
    for (const QString& key : patch.keys())
        if (key != "sender_id" && key != "master_enable" && key != "activation" && key != "transport_params" && key != "transport_file")
            return errorResponse(400, text("Unknown property \"%1\"").arg(key));
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    if (patch.contains("sender_id")) {
        const QJsonValue v = patch["sender_id"];
        if (!v.isNull() && !(v.isString() && uuid.match(v.toString()).hasMatch())) return errorResponse(400, text("sender_id is a UUID or null"));
    }
    if (patch.contains("master_enable") && !patch["master_enable"].isBool()) return errorResponse(400, text("master_enable is true or false"));
    RLeg legs[2] = {r.stagedLeg[0], r.stagedLeg[1]};
    QJsonValue fileData = r.stagedFileData, fileType = r.stagedFileType;
    // A transport file: its paths become the legs' parameters (those patched beside it applied after).
    if (patch.contains("transport_file")) {
        if (!patch["transport_file"].isObject()) return errorResponse(400, text("transport_file is an object"));
        const QJsonObject f = patch["transport_file"].toObject();
        for (const QString& key : f.keys())
            if (key != "data" && key != "type") return errorResponse(400, text("Unknown transport_file property \"%1\"").arg(key));
        const QJsonValue data = f.value("data"), type = f.value("type");
        if (!(data.isNull() || data.isString() || data.isUndefined()) || !(type.isNull() || type.isString() || type.isUndefined()))
            return errorResponse(400, text("transport_file's data and type are strings or null"));
        if (data.isString() && !data.toString().isEmpty()) {
            if (!type.isString() || type.toString() != "application/sdp") return errorResponse(400, text("The transport file is application/sdp"));
            const std::vector<SdpPath> paths = sdpPaths(data.toString());
            if (paths.empty() || paths[0].port <= 0 || paths[0].port > 65535 || !isIpv4(paths[0].address))
                return errorResponse(400, text("The SDP gives no IPv4 address and port to receive at"));
            // (one Montage can receive, of this receiver's kind: refused now rather than taken and then not received)
            std::string why;
            const std::vector<st2110::SdpStream> found = st2110::parseSdp(data.toString().toStdString(), &why);
            if (found.empty()) return errorResponse(400, QString::fromStdString(why));
            const std::string& e = found.front().encoding;
            if ((r.stream == 0) != (e == "raw" || e == "jxsv") || (r.stream == 2) != (e == "smpte291"))
                return errorResponse(400, text("The SDP describes another kind of stream than this receiver takes"));
            for (int i = 0; i < 2; ++i) {
                RLeg& l = legs[i];
                if (i >= int(paths.size())) {
                    l.rtpEnabled = false;
                    continue;
                }
                const SdpPath& p = paths[size_t(i)];
                const bool multicast = st2110::isMulticast(p.address.toStdString());
                l.multicastIp = multicast ? QJsonValue(p.address) : QJsonValue(QJsonValue::Null);
                l.sourceIp = isIpv4(p.source) ? QJsonValue(p.source) : QJsonValue(QJsonValue::Null);
                // (unicast: to the interface it names)
                if (!multicast) l.interfaceIp = p.address;
                l.destinationPort = p.port;
                l.rtpEnabled = true;
            }
        }
        fileData = data.isUndefined() ? QJsonValue(QJsonValue::Null) : data;
        fileType = type.isUndefined() ? QJsonValue(QJsonValue::Null) : type;
    }
    if (patch.contains("transport_params")) {
        if (!patch["transport_params"].isArray()) return errorResponse(400, text("transport_params is an array"));
        const QJsonArray given = patch["transport_params"].toArray();
        if (given.size() != 2) return errorResponse(400, text("This receiver has two legs"));
        const QJsonArray constraints = receiverConstraints();
        for (int i = 0; i < 2; ++i) {
            if (!given[i].isObject()) return errorResponse(400, text("A leg is an object"));
            const QJsonObject p = given[i].toObject();
            RLeg& l = legs[i];
            for (const QString& key : p.keys()) {
                const QJsonValue v = p[key];
                if (key == "source_ip" || key == "multicast_ip") {
                    if (!v.isNull() && !(v.isString() && isIpv4(v.toString()))) return errorResponse(400, text("%1 is an IPv4 address or null").arg(key));
                    if (key == "multicast_ip" && v.isString() && !st2110::isMulticast(v.toString().toStdString()))
                        return errorResponse(400, text("%1 is not a multicast address").arg(v.toString()));
                    (key == "source_ip" ? l.sourceIp : l.multicastIp) = v;
                } else if (key == "interface_ip") {
                    if (!v.isString() || (v.toString() != "auto" && !isIpv4(v.toString()))) return errorResponse(400, text("interface_ip is an IPv4 address or \"auto\""));
                    if (v.toString() != "auto" && !constraints[i].toObject()["interface_ip"].toObject()["enum"].toArray().contains(v))
                        return errorResponse(400, text("No interface has %1").arg(v.toString()));
                    l.interfaceIp = v.toString();
                } else if (key == "destination_port") {
                    const bool ok = (v.isString() && v.toString() == "auto") ||
                                    (v.isDouble() && v.toDouble() == std::floor(v.toDouble()) && v.toInt() >= 1 && v.toInt() <= 65535);
                    if (!ok) return errorResponse(400, text("destination_port is a port or \"auto\""));
                    l.destinationPort = v.isString() ? v : QJsonValue(v.toInt());
                } else if (key == "rtp_enabled") {
                    if (!v.isBool()) return errorResponse(400, text("rtp_enabled is true or false"));
                    l.rtpEnabled = v.toBool();
                } else {
                    return errorResponse(400, text("This receiver has no parameter \"%1\"").arg(key));
                }
            }
        }
    }
    QJsonValue mode = QJsonValue::Undefined, requested = QJsonValue::Null;
    if (patch.contains("activation")) {
        if (!patch["activation"].isObject()) return errorResponse(400, text("activation is an object"));
        const QJsonObject a = patch["activation"].toObject();
        for (const QString& key : a.keys())
            if (key != "mode" && key != "requested_time") return errorResponse(400, text("Unknown activation property \"%1\"").arg(key));
        if (!a.contains("mode")) return errorResponse(400, text("activation needs a mode"));
        mode = a["mode"];
        const QString m = mode.toString();
        if (!mode.isNull() && m != "activate_immediate" && m != "activate_scheduled_absolute" && m != "activate_scheduled_relative")
            return errorResponse(400, text("Not an activation mode"));
        requested = a.value("requested_time");
        if (requested.isUndefined()) requested = QJsonValue::Null;
        int64_t ns = 0;
        if (!requested.isNull() && !(requested.isString() && parseTai(requested.toString(), ns)))
            return errorResponse(400, text("requested_time is \"<seconds>:<nanoseconds>\""));
        if ((m == "activate_scheduled_absolute" || m == "activate_scheduled_relative") && requested.isNull())
            return errorResponse(400, text("A scheduled activation needs requested_time"));
    }
    if (!r.mode.isNull() && !(mode.isNull() && !mode.isUndefined())) return errorResponse(423, text("An activation is scheduled: cancel it first"));
    if (patch.contains("sender_id")) r.stagedSenderId = patch["sender_id"];
    if (patch.contains("master_enable")) r.stagedMasterEnable = patch["master_enable"].toBool();
    r.stagedLeg[0] = legs[0], r.stagedLeg[1] = legs[1];
    r.stagedFileData = fileData, r.stagedFileType = fileType;
    if (mode.isNull() && !mode.isUndefined()) {
        delete r.timer;
        r.timer = nullptr;
        r.mode = r.requestedTime = QJsonValue::Null;
        return jsonResponse(200, receiverJson(r, true));
    }
    if (mode.isUndefined()) return jsonResponse(200, receiverJson(r, true));
    const QString m = mode.toString();
    if (m == "activate_immediate") {
        r.requestedTime = QJsonValue::Null;
        activateReceiver(r, m);
        QJsonObject out = receiverJson(r, true);
        out["activation"] = QJsonObject{{"mode", m}, {"requested_time", QJsonValue::Null}, {"activation_time", r.lastActivation}};
        return jsonResponse(200, out);
    }
    int64_t at = 0;
    parseTai(requested.toString(), at);
    const int64_t now = st2110::ptpNowNs();
    if (m == "activate_scheduled_relative") at = at > std::numeric_limits<int64_t>::max() - now ? std::numeric_limits<int64_t>::max() : at + now;
    r.mode = m;
    r.requestedTime = requested;
    r.activationNs = at;
    delete r.timer;
    r.timer = new QTimer(this);
    r.timer->setSingleShot(true);
    r.timer->setTimerType(Qt::PreciseTimer);
    const QUuid id = r.id;
    connect(r.timer, &QTimer::timeout, this, [this, id] {
        for (RConn& x : receivers_)
            if (x.id == id) {
                const int64_t left = x.activationNs - st2110::ptpNowNs();
                if (left > 0) {
                    x.timer->start(int(std::min<int64_t>((left + 999999) / 1000000, 24 * 3600 * 1000)));
                    return;
                }
                const QString mm = x.mode.toString();
                x.timer->deleteLater();
                x.timer = nullptr;
                activateReceiver(x, mm);
                return;
            }
    });
    r.timer->start(int(std::clamp<int64_t>((at - now + 999999) / 1000000, 0, 24 * 3600 * 1000)));
    return jsonResponse(202, receiverJson(r, true));
}

void Node::activateReceiver(RConn& r, const QString& mode) {
    r.lastMode = mode;
    r.lastRequested = r.requestedTime.isString() ? r.requestedTime.toString() : QString();
    const bool scheduled = mode == QLatin1String("activate_scheduled_relative") || mode == QLatin1String("activate_scheduled_absolute");
    r.lastActivation = taiText(scheduled ? std::max(st2110::ptpNowNs(), r.activationNs) : st2110::ptpNowNs());
    r.senderId = r.stagedSenderId;
    r.masterEnable = r.stagedMasterEnable;
    r.leg[0] = r.stagedLeg[0], r.leg[1] = r.stagedLeg[1];
    r.fileData = r.stagedFileData, r.fileType = r.stagedFileType;
    r.mode = r.requestedTime = QJsonValue::Null;
    r.version = newVersion();
    resourceChanged(QStringLiteral("receiver"), receiverResource(r));
    if (onReceiverActivated) onReceiverActivated(receiverConnection(r.stream));
}

ReceiverConnection Node::receiverConnection(int stream) const {
    ReceiverConnection c;
    c.stream = stream;
    for (const RConn& r : receivers_) {
        if (r.stream != stream) continue;
        c.active = r.masterEnable;
        c.senderId = r.senderId.toString();
        c.transportFile = r.fileData.toString();
        for (int i = 0; i < 2; ++i) {
            const QJsonObject l = rlegJson(r.leg[i], i, true);
            c.leg[i].enabled = r.masterEnable && r.leg[i].rtpEnabled;
            c.leg[i].sourceIp = l["source_ip"].toString();
            c.leg[i].multicastIp = l["multicast_ip"].toString();
            c.leg[i].interfaceIp = l["interface_ip"].toString();
            c.leg[i].port = l["destination_port"].toInt();
        }
    }
    return c;
}

QStringList Node::receiverInterfaces(int stream) const {
    // (as receiverStreams places the paths: the enabled legs in turn, so the second leg alone is the first path)
    QStringList out;
    const ReceiverConnection c = receiverConnection(stream);
    for (int i = 0; i < 2; ++i)
        if (c.leg[i].enabled) out << c.leg[i].interfaceIp;
    return out;
}

std::vector<st2110::SdpStream> Node::receiverStreams(QString* why) const {
    std::vector<st2110::SdpStream> out;
    for (const RConn& r : receivers_) {
        const ReceiverConnection c = receiverConnection(r.stream);
        if (!c.active || c.transportFile.isEmpty() || !(c.leg[0].enabled || c.leg[1].enabled)) continue;
        std::string error;
        const std::vector<st2110::SdpStream> found = st2110::parseSdp(c.transportFile.toStdString(), &error);
        if (found.empty()) {
            if (why) *why = QString::fromStdString(error);
            continue;
        }
        // The format from the file, where to listen from the parameters (which a controller may have changed since).
        st2110::SdpStream s = found.front();
        int n = 0;
        for (int i = 0; i < 2; ++i) {
            if (!c.leg[i].enabled) continue;
            s.path[n] = {(c.leg[i].multicastIp.isEmpty() ? c.leg[i].interfaceIp : c.leg[i].multicastIp).toStdString(), c.leg[i].port};
            s.source[n] = c.leg[i].sourceIp.toStdString();
            s.interfaceAddress[n] = c.leg[i].interfaceIp.toStdString();  // (each receiver's own legs')
            ++n;
        }
        if (n < 2) s.path[1] = {}, s.source[1].clear(), s.interfaceAddress[1].clear();
        out.push_back(s);
    }
    return out;
}

Node::Response Node::putTarget(RConn& r, const QJsonObject& sender) {
    // IS-04's basic connection management: a sender connects the receiver (its manifest fetched for the transport
    // file), an empty object disconnects it.
    if (r.timer) {
        delete r.timer;
        r.timer = nullptr;
        r.mode = r.requestedTime = QJsonValue::Null;
    }
    if (sender.isEmpty()) {
        r.stagedSenderId = QJsonValue::Null;
        r.stagedMasterEnable = false;
        activateReceiver(r, QStringLiteral("activate_immediate"));
        return jsonResponse(202, QJsonObject{});
    }
    if (!sender["id"].isString()) return errorResponse(400, text("The target is a sender, or an empty object"));
    // (another sender's transport file is not this one's: none until its manifest comes)
    if (sender["id"] != r.stagedSenderId) r.stagedFileData = r.stagedFileType = QJsonValue::Null;
    r.stagedSenderId = sender["id"];
    r.stagedMasterEnable = true;
    activateReceiver(r, QStringLiteral("activate_immediate"));
    const QString href = sender["manifest_href"].toString();
    if (!href.isEmpty() && net_) {
        QNetworkRequest req{QUrl(href)};
        req.setTransferTimeout(5000);
        QNetworkReply* reply = net_->get(req);
        // (anyone may name the manifest: a megabyte of it at most)
        reply->setReadBufferSize(1 << 20);
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() >= (1 << 20)) reply->abort();
        });
        const QUuid id = r.id;
        const QJsonValue senderId = sender["id"];
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, senderId] {
            reply->deleteLater();
            if (stopping_ || reply->error() != QNetworkReply::NoError) return;
            const QString sdp = QString::fromUtf8(reply->read(1 << 20));
            for (RConn& x : receivers_)
                if (x.id == id && x.senderId == senderId && x.masterEnable && x.mode.isNull()) {
                    // (staged and made active as if a controller had patched it so)
                    const Response p = patchReceiver(x, QJsonObject{{"transport_file", QJsonObject{{"data", sdp}, {"type", "application/sdp"}}},
                                                                    {"activation", QJsonObject{{"mode", "activate_immediate"}}}});
                    (void)p;
                }
        });
    }
    return jsonResponse(202, sender);
}

// ---- The APIs.

Node::Response Node::handle(const QByteArray& method, const QString& rawPath, const QByteArray& body) {
    QString path = rawPath.section('?', 0, 0);
    const QStringList seg = path.split('/', Qt::SkipEmptyParts);
    if (method == "OPTIONS") {
        Response r;
        r.code = 200;
        r.body.clear();
        return r;
    }
    const bool get = method == "GET" || method == "HEAD";
    auto notAllowed = [&] { return errorResponse(405, text("%1 is not allowed here").arg(QString::fromLatin1(method))); };
    auto notFound = [&] { return errorResponse(404, text("Nothing at %1").arg(path)); };
    if (seg.isEmpty()) return get ? jsonResponse(200, listing({"x-nmos/"})) : notAllowed();
    if (seg[0] != "x-nmos") return notFound();
    if (seg.size() == 1) return get ? jsonResponse(200, listing({"connection/", "node/"})) : notAllowed();

    if (seg[1] == "node") {
        if (seg.size() == 2) return get ? jsonResponse(200, listing({"v1.3/"})) : notAllowed();
        if (seg[2] != "v1.3") return notFound();
        if (seg.size() == 3) return get ? jsonResponse(200, listing({"self/", "sources/", "flows/", "devices/", "senders/", "receivers/"})) : notAllowed();
        const QString what = seg[3];
        if (what == "receivers") {
            if (seg.size() == 4) return get ? jsonResponse(200, receiverResources()) : notAllowed();
            RConn* r = nullptr;
            for (RConn& x : receivers_)
                if (idText(x.id) == seg[4]) r = &x;
            if (!r) return notFound();
            if (seg.size() == 5) return get ? jsonResponse(200, receiverResource(*r)) : notAllowed();
            if (seg.size() == 6 && seg[5] == "target") {
                if (method != "PUT") return notAllowed();
                QJsonParseError pe;
                const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
                if (pe.error != QJsonParseError::NoError || !doc.isObject()) return errorResponse(400, text("The body is a JSON object"));
                return putTarget(*r, doc.object());
            }
            return notFound();
        }
        QJsonArray all;
        if (what == "self") {
            if (seg.size() != 4) return notFound();
            return get ? jsonResponse(200, nodeResource()) : notAllowed();
        }
        if (what == "devices") all = QJsonArray{deviceResource()};
        else if (what == "sources") all = sourceResources();
        else if (what == "flows") all = flowResources();
        else if (what == "senders") all = senderResources();
        else return notFound();
        if (seg.size() == 4) return get ? jsonResponse(200, all) : notAllowed();
        if (seg.size() != 5) return notFound();
        for (const QJsonValue& v : all)
            if (v.toObject()["id"].toString() == seg[4]) return get ? jsonResponse(200, v) : notAllowed();
        return notFound();
    }

    if (seg[1] != "connection") return notFound();
    if (seg.size() == 2) return get ? jsonResponse(200, listing({"v1.1/"})) : notAllowed();
    if (seg[2] != "v1.1") return notFound();
    if (seg.size() == 3) return get ? jsonResponse(200, listing({"bulk/", "single/"})) : notAllowed();
    if (seg[3] == "bulk") {
        if (seg.size() == 4) return get ? jsonResponse(200, listing({"senders/", "receivers/"})) : notAllowed();
        if (seg.size() != 5 || (seg[4] != "senders" && seg[4] != "receivers")) return notFound();
        if (method != "POST") return notAllowed();
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isArray()) return errorResponse(400, text("The body is an array of {id, params}"));
        const QJsonArray items = doc.array();
        for (const QJsonValue& item : items) {
            const QJsonObject o = item.toObject();
            if (!item.isObject() || !o["id"].isString() || !o["params"].isObject() || o.size() != 2)
                return errorResponse(400, text("Each item is {id, params}"));
        }
        QJsonArray results;
        for (const QJsonValue& item : items) {
            const QJsonObject o = item.toObject();
            const QString id = o["id"].toString();
            Connection* c = nullptr;
            RConn* rc = nullptr;
            if (seg[4] == "senders")
                for (Connection& x : connections_)
                    if (idText(x.id) == id) c = &x;
            if (seg[4] == "receivers")
                for (RConn& x : receivers_)
                    if (idText(x.id) == id) rc = &x;
            if (!c && !rc) {
                results.append(QJsonObject{{"id", id}, {"code", 404}, {"error", text("No such %1").arg(seg[4].chopped(1))}});
                continue;
            }
            const Response r = c ? patchStaged(*c, o["params"].toObject(), true) : patchReceiver(*rc, o["params"].toObject());
            QJsonObject result{{"id", id}, {"code", r.code}};
            if (r.code >= 400) result["error"] = QJsonDocument::fromJson(r.body).object()["error"];
            results.append(result);
        }
        return jsonResponse(200, results);
    }
    if (seg[3] != "single") return notFound();
    if (seg.size() == 4) return get ? jsonResponse(200, listing({"senders/", "receivers/"})) : notAllowed();
    if (seg[4] == "receivers") {
        if (seg.size() == 5) {
            if (!get) return notAllowed();
            QJsonArray ids;
            for (const RConn& r : receivers_) ids.append(idText(r.id) + "/");
            return jsonResponse(200, ids);
        }
        RConn* r = nullptr;
        for (RConn& x : receivers_)
            if (idText(x.id) == seg[5]) r = &x;
        if (!r) return notFound();
        if (seg.size() == 6) return get ? jsonResponse(200, listing({"constraints/", "staged/", "active/", "transporttype/"})) : notAllowed();
        if (seg.size() != 7) return notFound();
        const QString what = seg[6];
        if (what == "constraints") return get ? jsonResponse(200, receiverConstraints()) : notAllowed();
        if (what == "active") return get ? jsonResponse(200, receiverJson(*r, false)) : notAllowed();
        if (what == "transporttype") {
            if (!get) return notAllowed();
            Response t;
            t.body = "\"urn:x-nmos:transport:rtp\"";
            return t;
        }
        if (what == "staged") {
            if (get) return jsonResponse(200, receiverJson(*r, true));
            if (method != "PATCH") return notAllowed();
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) return errorResponse(400, text("The body is a JSON object"));
            return patchReceiver(*r, doc.object());
        }
        return notFound();
    }
    if (seg[4] != "senders") return notFound();
    if (seg.size() == 5) {
        if (!get) return notAllowed();
        QJsonArray ids;
        for (const Connection& c : connections_) ids.append(idText(c.id) + "/");
        return jsonResponse(200, ids);
    }
    Connection* c = nullptr;
    for (Connection& x : connections_)
        if (idText(x.id) == seg[5]) c = &x;
    if (!c) return notFound();
    if (seg.size() == 6) return get ? jsonResponse(200, listing({"constraints/", "staged/", "active/", "transportfile/", "transporttype/"})) : notAllowed();
    if (seg.size() != 7) return notFound();
    const QString what = seg[6];
    if (what == "constraints") return get ? jsonResponse(200, constraintsJson(c->stream)) : notAllowed();
    if (what == "active") return get ? jsonResponse(200, connectionJson(*c, false)) : notAllowed();
    if (what == "transporttype") {
        if (!get) return notAllowed();
        Response r;
        r.body = "\"urn:x-nmos:transport:rtp\"";
        return r;
    }
    if (what == "transportfile") {
        if (!get) return notAllowed();
        if (!c->masterEnable) return errorResponse(404, text("The sender is not sending"));
        Response r;
        r.body = transportFile(c->stream).toUtf8();
        r.contentType = "application/sdp";
        return r;
    }
    if (what == "staged") {
        if (get) return jsonResponse(200, connectionJson(*c, true));
        if (method != "PATCH") return notAllowed();
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) return errorResponse(400, text("The body is a JSON object"));
        return patchStaged(*c, doc.object(), false);
    }
    return notFound();
}

// ---- Registration.

// ---- Multicast DNS (RFC 6762) and DNS-SD (RFC 6763): the Node announced as _nmos-node._tcp.local and answering
// queries for it; registries browsed for as _nmos-register._tcp.local.

namespace {

constexpr quint16 kMdnsPort = 5353;
constexpr quint16 kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33, kTypeAny = 255;

void putName(QByteArray& b, const QString& name) {
    for (const QString& label : name.split('.', Qt::SkipEmptyParts)) {
        const QByteArray l = label.toUtf8().left(63);
        b.append(char(l.size()));
        b.append(l);
    }
    b.append('\0');
}
void put16(QByteArray& b, quint16 v) { b.append(char(v >> 8)).append(char(v & 0xFF)); }
void put32(QByteArray& b, quint32 v) { put16(b, quint16(v >> 16)), put16(b, quint16(v)); }
quint16 get16(const QByteArray& m, int at) { return quint16(quint8(m[at]) << 8 | quint8(m[at + 1])); }

// A name at `at` (with compression pointers, a few dozen at most; 255 octets at most in all, as RFC 1035 holds names
// to, so a chain of pointers cannot make one long), `at` moved past it.
}  // namespace

bool readDnsName(const QByteArray& m, int& at, QString& out) {
    QStringList labels;
    int pos = at, jumps = 0, octets = 1;
    bool jumped = false;
    while (pos < m.size()) {
        const quint8 len = quint8(m[pos]);
        if (len == 0) {
            if (!jumped) at = pos + 1;
            out = labels.join('.');
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= m.size() || ++jumps > 32) return false;
            const int to = (len & 0x3F) << 8 | quint8(m[pos + 1]);
            if (!jumped) at = pos + 2;
            jumped = true;
            pos = to;
            continue;
        }
        if (len > 63 || pos + 1 + len > m.size()) return false;
        if ((octets += 1 + len) > 255) return false;
        labels << QString::fromUtf8(m.mid(pos + 1, len));
        pos += 1 + len;
    }
    return false;
}

namespace {

bool readName(const QByteArray& m, int& at, QString& out) { return readDnsName(m, at, out); }

QByteArray record(const QString& name, quint16 type, bool flush, quint32 ttl, const QByteArray& data) {
    QByteArray b;
    putName(b, name);
    put16(b, type);
    put16(b, quint16(1 | (flush ? 0x8000 : 0)));
    put32(b, ttl);
    put16(b, quint16(data.size()));
    return b + data;
}

QByteArray message(quint16 id, bool response, const QList<QByteArray>& questions, const QList<QByteArray>& answers) {
    QByteArray b;
    put16(b, id);
    put16(b, response ? 0x8400 : 0);  // (a response: authoritative)
    put16(b, quint16(questions.size()));
    put16(b, quint16(answers.size()));
    put16(b, 0), put16(b, 0);
    for (const QByteArray& q : questions) b += q;
    for (const QByteArray& a : answers) b += a;
    return b;
}

QByteArray question(const QString& name, quint16 type) {
    QByteArray b;
    putName(b, name);
    put16(b, type);
    put16(b, 1);
    return b;
}

// A DNS label of letters, digits and hyphens.
QString dnsLabel(QString s) {
    for (QChar& c : s)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != '-') c = '-';
    return s.left(40);
}

}  // namespace

struct Node::Mdns {
    QUdpSocket* socket = nullptr;  // (the Node's child)
    QString instance, host;        // "<label>-<port>._nmos-node._tcp.local", "montage-<id>.local"
    QHostAddress address;
    int port = 0;
    QTimer* announce = nullptr;
    int announcing = 0;  // announcements still to send (two, a second apart)
    // Browsing for registries: what the answers have said so far, by instance name.
    struct Found {
        QString target;
        int port = 0, priority = 100;
        bool txt = false, usable = true;
    };
    std::map<QString, Found> found;
    std::map<QString, QHostAddress> addresses;  // (by host name, lower case)
    QTimer* browse = nullptr;
    int queries = 0;
    std::function<void(std::vector<Registry>)> browsed;
    // The local links (RFC 6762 11: only a sender on one of them is heard), and when the Node last answered to the
    // group (once a second at most, as 6.2 has it; a query sooner has its answer due when the second is up).
    std::vector<std::pair<QHostAddress, int>> links;
    QElapsedTimer answered;
    bool answerDue = false;
    bool onLink(const QHostAddress& from) const {
        QHostAddress a = from;
        bool v4 = false;
        if (const quint32 four = a.toIPv4Address(&v4); v4) a = QHostAddress(four);
        if (a.isLoopback() || a.isLinkLocal()) return true;
        return std::any_of(links.begin(), links.end(), [&](const auto& l) { return a.isInSubnet(l.first, l.second); });
    }
};

QByteArray Node::mdnsTxt() const {
    // IS-04's TXT record: the API's protocol, versions and authorisation; peer to peer (no registry), the resources'
    // versions too (each counted round from 0 to 255 as resources of its kind change).
    QByteArray b;
    auto entry = [&](const QByteArray& kv) { b.append(char(kv.size())).append(kv); };
    entry("api_proto=http");
    entry("api_ver=v1.0,v1.1,v1.2,v1.3");
    entry("api_auth=false");
    if (!registered_) {
        static const char* const kinds[6] = {"slf", "src", "flw", "dvc", "snd", "rcv"};
        for (int i = 0; i < 6; ++i) entry(QByteArray("ver_") + kinds[i] + "=" + QByteArray::number(mdnsVersions_[i]));
    }
    return b;
}

void Node::announceNode() {
    if (!mdns_ || !mdns_->socket) return;
    Mdns& m = *mdns_;
    const QString service = QStringLiteral("_nmos-node._tcp.local");
    QByteArray srv;
    put16(srv, 0), put16(srv, 0), put16(srv, quint16(m.port));
    putName(srv, m.host);
    QByteArray ptr;
    putName(ptr, m.instance);
    QByteArray enumerate;
    putName(enumerate, service);
    QByteArray a;
    put32(a, m.address.toIPv4Address());
    const quint32 ttl = stopping_ ? 0 : 120;
    const QByteArray msg = message(0, true, {},
                                   {record(service, kTypePtr, false, stopping_ ? 0 : 4500, ptr),
                                    record(QStringLiteral("_services._dns-sd._udp.local"), kTypePtr, false, stopping_ ? 0 : 4500, enumerate),
                                    record(m.instance, kTypeSrv, true, ttl, srv), record(m.instance, kTypeTxt, true, stopping_ ? 0 : 4500, mdnsTxt()),
                                    record(m.host, kTypeA, true, ttl, a)});
    m.socket->writeDatagram(msg, QHostAddress(QStringLiteral("224.0.0.251")), kMdnsPort);
}

void Node::startMdns() {
    mdns_ = std::make_unique<Mdns>();
    Mdns& m = *mdns_;
    m.socket = new QUdpSocket(run_);
    // (shared with the system's own responder, where there is one)
    if (!m.socket->bind(QHostAddress::AnyIPv4, kMdnsPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        mdnsError_ = m.socket->errorString();
        m.socket = nullptr;
        return;
    }
    m.socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    m.socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    const QHostAddress group(QStringLiteral("224.0.0.251"));
    // Listening on every interface that takes multicast (a query may come in on any); sent from the one the APIs are
    // offered on, where it takes multicast, else as routing chooses.
    m.socket->joinMulticastGroup(group);
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        const auto f = i.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::CanMulticast)) continue;
        m.socket->joinMulticastGroup(group, i);
        for (const QNetworkAddressEntry& e : i.addressEntries()) {
            if (e.ip().toString() == address_) m.socket->setMulticastInterface(i);
            if (e.prefixLength() >= 0) m.links.emplace_back(e.ip(), e.prefixLength());
        }
    }
    m.address = QHostAddress(address_);
    m.port = port();
    m.instance = dnsLabel(o_.label) + QStringLiteral("-%1._nmos-node._tcp.local").arg(m.port);
    m.host = QStringLiteral("montage-%1.local").arg(idText(nodeId_).left(8));
    connect(m.socket, &QUdpSocket::readyRead, this, [this] {
        if (!mdns_ || !mdns_->socket) return;
        Mdns& md = *mdns_;
        while (md.socket->hasPendingDatagrams()) {
            const QNetworkDatagram d = md.socket->receiveDatagram(9000);
            const QByteArray msg = d.data();
            if (msg.size() < 12 || !md.onLink(d.senderAddress())) continue;  // (from off the local links: not heard)
            const quint16 id = get16(msg, 0), flags = get16(msg, 2);
            const int qd = get16(msg, 4), records = get16(msg, 6) + get16(msg, 8) + get16(msg, 10);
            int at = 12;
            const QString service = QStringLiteral("_nmos-node._tcp.local");
            bool wanted = false;
            QList<QByteArray> asked;
            for (int i = 0; i < qd && at < msg.size(); ++i) {
                QString name;
                if (!readName(msg, at, name) || at + 4 > msg.size()) break;
                const quint16 type = get16(msg, at);
                at += 4;
                if (flags & 0x8000) continue;
                const QString n = name.toLower();
                const bool ours = (n == service && (type == kTypePtr || type == kTypeAny)) ||
                                  (n == md.instance.toLower() && (type == kTypeSrv || type == kTypeTxt || type == kTypeAny)) ||
                                  (n == md.host.toLower() && (type == kTypeA || type == kTypeAny)) ||
                                  (n == QStringLiteral("_services._dns-sd._udp.local") && type == kTypePtr);
                if (ours) wanted = true, asked << question(name, type);
            }
            if (!(flags & 0x8000)) {
                if (!wanted || stopping_) continue;
                // A query for the Node: answered to the group, and straight back to a querier not on port 5353 (a
                // one-shot query, RFC 6762's legacy unicast: its id and questions with the answer).
                if (d.senderPort() == kMdnsPort) {
                    // (once a second at most: a query sooner is answered when the second is up, one answer for all
                    // asked meanwhile)
                    const qint64 wait = md.answered.isValid() ? 1000 - md.answered.elapsed() : 0;
                    if (wait <= 0) {
                        md.answered.start();
                        announceNode();
                    } else if (!md.answerDue) {
                        md.answerDue = true;
                        QTimer::singleShot(std::chrono::milliseconds(wait), md.socket, [this] {
                            if (!mdns_ || !mdns_->socket || stopping_) return;
                            mdns_->answerDue = false;
                            mdns_->answered.start();
                            announceNode();
                        });
                    }
                } else {
                    QByteArray srv;
                    put16(srv, 0), put16(srv, 0), put16(srv, quint16(md.port));
                    putName(srv, md.host);
                    QByteArray ptr, a;
                    putName(ptr, md.instance);
                    put32(a, md.address.toIPv4Address());
                    const QByteArray answer = message(id, true, asked,
                                                      {record(service, kTypePtr, false, 10, ptr), record(md.instance, kTypeSrv, false, 10, srv),
                                                       record(md.instance, kTypeTxt, false, 10, mdnsTxt()), record(md.host, kTypeA, false, 10, a)});
                    md.socket->writeDatagram(answer, d.senderAddress(), quint16(d.senderPort()));
                }
                continue;
            }
            // A response: what it says of registries, while browsing for them.
            if (!md.browse) continue;
            for (int i = 0; i < records && at < msg.size(); ++i) {
                QString name;
                if (!readName(msg, at, name) || at + 10 > msg.size()) break;
                const quint16 type = get16(msg, at), length = get16(msg, at + 8);
                const quint32 ttl = quint32(get16(msg, at + 4)) << 16 | get16(msg, at + 6);
                int data = at + 10;
                at = data + length;
                if (at > msg.size()) break;
                const QString n = name.toLower();
                if (type == kTypePtr && (n == QStringLiteral("_nmos-register._tcp.local") || n == QStringLiteral("_nmos-registration._tcp.local"))) {
                    QString instance;
                    if (!readName(msg, data, instance)) continue;
                    if (ttl == 0) md.found.erase(instance.toLower());  // (a goodbye)
                    else if (md.found.size() < 256) md.found.emplace(instance.toLower(), Mdns::Found{});
                } else if (!n.endsWith(QStringLiteral("._nmos-register._tcp.local")) && !n.endsWith(QStringLiteral("._nmos-registration._tcp.local"))) {
                    if (type == kTypeA && length == 4) {
                        if (ttl == 0) md.addresses.erase(n);  // (a goodbye)
                        else if (md.addresses.size() < 1024 || md.addresses.count(n))
                            md.addresses[n] = QHostAddress(quint32(get16(msg, data)) << 16 | get16(msg, data + 2));
                    }
                    continue;  // (another service's records: a Node's own, say)
                } else if (ttl == 0 || (!md.found.count(n) && md.found.size() >= 256)) {
                    if (ttl == 0) md.found.erase(n);
                    continue;
                } else if (type == kTypeSrv && length >= 7) {
                    QString target;
                    int t = data + 6;
                    if (!readName(msg, t, target)) continue;
                    Mdns::Found& f = md.found[n];
                    f.target = target.toLower(), f.port = get16(msg, data + 4);
                } else if (type == kTypeTxt) {
                    Mdns::Found& f = md.found[n];
                    QString ver, proto, auth = QStringLiteral("false");
                    for (int p = data; p < data + length;) {
                        const int len = quint8(msg[p]);
                        if (p + 1 + len > data + length) break;  // (a string past its record's end)
                        const QByteArray kv = msg.mid(p + 1, len);
                        p += 1 + len;
                        const int eq = kv.indexOf('=');
                        const QByteArray k = kv.left(eq), value = kv.mid(eq + 1);
                        if (k == "api_ver") ver = QString::fromUtf8(value);
                        else if (k == "api_proto") proto = QString::fromUtf8(value);
                        else if (k == "api_auth") auth = QString::fromUtf8(value);
                        else if (k == "pri") f.priority = value.toInt();
                    }
                    f.txt = true;
                    // (as by unicast DNS: a registry speaking v1.3 over HTTP without authorisation)
                    f.usable = ver.split(',').contains(QStringLiteral("v1.3")) && proto == "http" && auth == "false";
                }
            }
        }
    });
    if (o_.announce) {
        m.announce = new QTimer(run_);
        m.announce->setInterval(1000);
        m.announcing = 2;
        connect(m.announce, &QTimer::timeout, this, [this] {
            announceNode();
            if (mdns_ && --mdns_->announcing <= 0) mdns_->announce->stop();
        });
        QTimer::singleShot(0, run_, [this] { announceNode(); });
        m.announce->start();
    }
}

void Node::mdnsChanged(int kind) {
    if (kind >= 0 && kind < 6) mdnsVersions_[kind] = (mdnsVersions_[kind] + 1) % 256;
    // (said again shortly, the changes of a moment together)
    if (!mdns_ || !mdns_->announce) return;
    mdns_->announcing = std::max(mdns_->announcing, 1);
    if (!mdns_->announce->isActive()) mdns_->announce->start(100);
}

void Node::browseRegistries(std::function<void(std::vector<Registry>)> done) {
    if (!mdns_ || !mdns_->socket) {
        done({});
        return;
    }
    Mdns& m = *mdns_;
    m.found.clear();
    m.browsed = std::move(done);
    delete m.browse;
    m.browse = new QTimer(run_);
    m.browse->setInterval(600);
    m.queries = 0;
    auto ask = [this] {
        Mdns& md = *mdns_;
        // (the services; then, for those whose records have not all come, their SRV, TXT and the hosts' addresses)
        QList<QByteArray> qs{question(QStringLiteral("_nmos-register._tcp.local"), kTypePtr),
                             question(QStringLiteral("_nmos-registration._tcp.local"), kTypePtr)};
        for (const auto& [name, f] : md.found) {
            if (f.target.isEmpty()) qs << question(name, kTypeSrv);
            if (!f.txt) qs << question(name, kTypeTxt);
            if (!f.target.isEmpty() && !md.addresses.count(f.target)) qs << question(f.target, kTypeA);
        }
        md.socket->writeDatagram(message(0, false, qs, {}), QHostAddress(QStringLiteral("224.0.0.251")), kMdnsPort);
    };
    connect(m.browse, &QTimer::timeout, this, [this, ask] {
        Mdns& md = *mdns_;
        bool whole = !md.found.empty();
        for (const auto& [name, f] : md.found) whole = whole && f.txt && !f.target.isEmpty() && md.addresses.count(f.target);
        if (++md.queries < 4 && !whole) {
            ask();
            return;
        }
        md.browse->deleteLater();
        md.browse = nullptr;
        std::vector<Registry> out;
        for (const auto& [name, f] : md.found) {
            const auto a = md.addresses.find(f.target);
            if (!f.usable || !f.txt || a == md.addresses.end() || f.port <= 0) continue;
            out.push_back(Registry{QStringLiteral("http://%1:%2").arg(a->second.toString()).arg(f.port), f.priority, name});
        }
        auto done = std::move(md.browsed);
        done(out);
    });
    ask();
    m.browse->start();
}

void Node::discover() {
    if (stopping_ || !running()) return;
    if (!o_.registry.isEmpty()) {
        registries_ = {Registry{o_.registry.endsWith('/') ? o_.registry.chopped(1) : o_.registry, 0, QStringLiteral("given")}};
        registryIndex_ = 0;
        registerAll();
        return;
    }
    const QString domain = o_.domain.isEmpty() ? systemDomain() : o_.domain;
    found_.clear();
    if (domain.isEmpty() || domain.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0 ||
        domain.compare(QStringLiteral("local."), Qt::CaseInsensitive) == 0) {
        // Multicast DNS on the local network (no search domain to look in by unicast DNS).
        browsedMdns_ = true;
        lookups_ = 1;
        browseRegistries([this](std::vector<Registry> found) {
            if (stopping_) return;
            found_ = std::move(found);
            lookupDone();
        });
        return;
    }
    browsedMdns_ = false;
    auto* ptr = new QDnsLookup(QDnsLookup::PTR, QStringLiteral("_nmos-register._tcp.") + domain, run_);
    if (!o_.dnsServer.isEmpty()) ptr->setNameserver(QHostAddress(o_.dnsServer));
    lookups_ = 1;
    connect(ptr, &QDnsLookup::finished, this, [this, ptr] {
        ptr->deleteLater();
        for (const QDnsDomainNameRecord& r : ptr->pointerRecords()) {
            // Each instance: its SRV (host and port) and TXT (version, protocol, priority), then its host's address.
            const QString instance = r.value();
            auto* srv = new QDnsLookup(QDnsLookup::SRV, instance, run_);
            auto* txt = new QDnsLookup(QDnsLookup::TXT, instance, run_);
            if (!o_.dnsServer.isEmpty()) {
                srv->setNameserver(QHostAddress(o_.dnsServer));
                txt->setNameserver(QHostAddress(o_.dnsServer));
            }
            auto entry = std::make_shared<Registry>();
            entry->name = instance;
            auto ok = std::make_shared<bool>(true);
            auto parts = std::make_shared<int>(2);
            auto host = std::make_shared<QString>();
            auto port = std::make_shared<int>(0);
            auto partDone = [this, entry, ok, parts, host, port] {
                if (--*parts > 0) return;
                if (!*ok || host->isEmpty()) {
                    lookupDone();
                    return;
                }
                if (!run_) return;
                auto* a = new QDnsLookup(QDnsLookup::A, *host, run_);
                if (!o_.dnsServer.isEmpty()) a->setNameserver(QHostAddress(o_.dnsServer));
                connect(a, &QDnsLookup::finished, this, [this, a, entry, port] {
                    a->deleteLater();
                    const auto addresses = a->hostAddressRecords();
                    if (!addresses.isEmpty()) {
                        entry->url = QStringLiteral("http://%1:%2").arg(addresses.first().value().toString()).arg(*port);
                        found_.push_back(*entry);
                    }
                    lookupDone();
                });
                a->lookup();
            };
            connect(srv, &QDnsLookup::finished, this, [srv, host, port, ok, partDone] {
                srv->deleteLater();
                const auto records = srv->serviceRecords();
                if (records.isEmpty()) *ok = false;
                else {
                    *host = records.first().target();
                    *port = records.first().port();
                }
                partDone();
            });
            connect(txt, &QDnsLookup::finished, this, [txt, entry, ok, partDone] {
                txt->deleteLater();
                QString ver, proto, auth = QStringLiteral("false");
                for (const QDnsTextRecord& t : txt->textRecords())
                    for (const QByteArray& v : t.values()) {
                        const int eq = v.indexOf('=');
                        const QByteArray k = v.left(eq), value = v.mid(eq + 1);
                        if (k == "api_ver") ver = QString::fromUtf8(value);
                        else if (k == "api_proto") proto = QString::fromUtf8(value);
                        else if (k == "api_auth") auth = QString::fromUtf8(value);
                        else if (k == "pri") entry->priority = value.toInt();
                    }
                // Only a registry speaking v1.3 over HTTP without authorisation.
                if (!ver.split(',').contains(QStringLiteral("v1.3")) || proto != "http" || auth != "false") *ok = false;
                partDone();
            });
            ++lookups_;
            srv->lookup();
            txt->lookup();
        }
        lookupDone();
    });
    ptr->lookup();
}

void Node::lookupDone() {
    if (--lookups_ > 0 || stopping_) return;
    // Best priority first (a lower number), those of the same priority in a random order.
    std::vector<Registry> list = found_;
    std::shuffle(list.begin(), list.end(), *QRandomGenerator::global());
    std::stable_sort(list.begin(), list.end(), [](const Registry& a, const Registry& b) { return a.priority < b.priority; });
    registries_ = list;
    if (registries_.empty()) {
        registryIndex_ = -1;
        // (multicast queries further and further apart while no registry answers, to an hour at most)
        if (browsedMdns_) {
            rediscover_->start(mdnsRetryMs_);
            mdnsRetryMs_ = std::min(mdnsRetryMs_ * 2, 3600 * 1000);
        } else {
            rediscover_->start(2000);
        }
        return;
    }
    mdnsRetryMs_ = 2000;
    registryIndex_ = 0;
    registerAll();
}

void Node::registerAll() {
    if (registry().isEmpty() || stopping_) return;
    heartbeat_->stop();
    queue_.clear();
    queue_.emplace_back(QStringLiteral("node"), nodeResource());
    queue_.emplace_back(QStringLiteral("device"), deviceResource());
    for (const QJsonValue& v : sourceResources()) queue_.emplace_back(QStringLiteral("source"), v.toObject());
    for (const QJsonValue& v : flowResources()) queue_.emplace_back(QStringLiteral("flow"), v.toObject());
    for (const QJsonValue& v : senderResources()) queue_.emplace_back(QStringLiteral("sender"), v.toObject());
    for (const QJsonValue& v : receiverResources()) queue_.emplace_back(QStringLiteral("receiver"), v.toObject());
    registering_ = true;
    deleteFirst_ = false;
    postNext();
}

void Node::postNext() {
    if (queue_.empty()) {
        registering_ = false;
        if (!registered_) {
            registered_ = true;
            mdnsChanged(-1);  // (no longer peer to peer: the TXT record without the versions)
            if (onRegistered) onRegistered(true);
        }
        heartbeat_->start();
        return;
    }
    const auto [type, data] = queue_.front();
    QNetworkRequest req(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/resource")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setTransferTimeout(std::max(1000, o_.heartbeatMs));
    QNetworkReply* reply = net_->post(req, toJson(QJsonObject{{"type", type}, {"data", data}}));
    connect(reply, &QNetworkReply::finished, this, [this, reply, type] {
        reply->deleteLater();
        if (stopping_ || !registering_) return;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0 || code >= 500) {
            registryFailed();
            return;
        }
        if (type == "node" && code == 200 && !deleteFirst_) {
            // The registry had this node already (from a run before): taken away, and registered afresh.
            deleteFirst_ = true;
            QNetworkRequest del(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/resource/nodes/") + idText(nodeId_)));
            del.setTransferTimeout(std::max(1000, o_.heartbeatMs));
            QNetworkReply* d = net_->deleteResource(del);
            connect(d, &QNetworkReply::finished, this, [this, d] {
                d->deleteLater();
                if (stopping_ || !registering_) return;
                postNext();
            });
            return;
        }
        queue_.erase(queue_.begin());
        postNext();
    });
}

void Node::heartbeat() {
    if (stopping_ || registry().isEmpty() || registering_) return;
    QNetworkRequest req(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/health/nodes/") + idText(nodeId_)));
    req.setTransferTimeout(std::max(1000, o_.heartbeatMs));
    // (an empty body: Content-Length 0 and no Content-Type, as IS-04 asks)
    QNetworkReply* reply = net_->post(req, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (stopping_ || registering_) return;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 404) registerAll();  // (the registry forgot it: everything again)
        else if (code == 0 || code >= 500) registryFailed();
    });
}

void Node::registryFailed() {
    // The next registry by priority: a heartbeat there first (it may know this node), else registering; none left, a
    // fresh look.
    registering_ = false;
    queue_.clear();
    ++registryIndex_;
    if (registryIndex_ >= 0 && registryIndex_ < int(registries_.size())) {
        if (registered_) {
            heartbeat();
            heartbeat_->start();
        } else {
            registerAll();
        }
        return;
    }
    heartbeat_->stop();
    registryIndex_ = -1;
    if (registered_) {
        registered_ = false;
        mdnsChanged(-1);
        if (onRegistered) onRegistered(false);
    }
    rediscover_->start(1000);
}

void Node::resourceChanged(const QString& type, const QJsonObject& data) {
    static const QStringList kinds{QStringLiteral("node"), QStringLiteral("source"), QStringLiteral("flow"), QStringLiteral("device"),
                                   QStringLiteral("sender"), QStringLiteral("receiver")};
    mdnsChanged(int(kinds.indexOf(type)));
    if (!registered_ || registering_ || stopping_ || registry().isEmpty()) return;
    QNetworkRequest req(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/resource")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setTransferTimeout(std::max(1000, o_.heartbeatMs));
    QNetworkReply* reply = net_->post(req, toJson(QJsonObject{{"type", type}, {"data", data}}));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

}  // namespace montage::nmos
