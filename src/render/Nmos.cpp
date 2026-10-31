#include "Nmos.h"

#include <QDnsLookup>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <map>

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
    // in all and from one address, a request's headers and body, and how long a connection may sit without finishing
    // one (a request trickled in a byte at a time is cut off).
    constexpr int kMaxClients = 64, kMaxClientsFrom = 8, kIdleMs = 15000;
    constexpr qsizetype kMaxHeader = 64 << 10, kMaxBody = 1 << 20;
    auto from = std::make_shared<std::map<QString, int>>();
    auto clients = std::make_shared<int>(0);
    connect(server_, &QTcpServer::newConnection, this, [this, from, clients] {
        while (QTcpSocket* s = server_->nextPendingConnection()) {
            const QString peer = s->peerAddress().toString();
            if (*clients >= kMaxClients || (*from)[peer] >= kMaxClientsFrom) {
                if ((*from)[peer] == 0) from->erase(peer);
                s->abort();
                s->deleteLater();
                continue;
            }
            ++*clients;
            ++(*from)[peer];
            auto buffer = std::make_shared<QByteArray>();
            auto idle = new QTimer(s);
            idle->setSingleShot(true);
            idle->start(kIdleMs);
            connect(idle, &QTimer::timeout, s, &QTcpSocket::abort);
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QObject::destroyed, this, [from, clients, peer] {
                --*clients;
                if (--(*from)[peer] <= 0) from->erase(peer);
            });
            connect(s, &QTcpSocket::readyRead, s, [this, s, buffer, idle] {
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
    for (int i = 0; i < 3; ++i) {
        Connection c;
        c.id = id(QStringLiteral("sender/") + kStreams[i]);
        c.stream = i;
        c.receiverId = c.stagedReceiverId = QJsonValue::Null;
        c.mode = c.requestedTime = QJsonValue::Null;
        connections_.push_back(c);
    }
    nodeVersion_ = newVersion();
    deviceVersion_ = newVersion();
    for (int i = 0; i < 3; ++i) {
        sourceVersion_[i] = newVersion();
        flowVersion_[i] = newVersion();
    }
    setStreams(video_, audio_, options_, active_);
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
    delete heartbeat_;
    heartbeat_ = nullptr;
    delete rediscover_;
    rediscover_ = nullptr;
    delete run_;
    run_ = nullptr;
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

st2110::Endpoint Node::endpointOf(int stream) const { return stream == 0 ? options_.video : stream == 1 ? options_.audio : options_.anc; }

QString Node::sourceAddress() const {
    if (!options_.interfaceAddress.empty()) return QString::fromStdString(options_.interfaceAddress);
    return address_;
}

QString Node::interfaceName() const {
    const QString at = sourceAddress();
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
                               video.colorimetry != video_.colorimetry || audio.channels != audio_.channels;
    video_ = video;
    audio_ = audio;
    options_ = o;
    active_ = active;
    // The active legs: where the output sends now.
    const bool enabled[3] = {o.sendVideo, o.sendAudio, o.sendAnc};
    for (Connection& c : connections_) {
        const st2110::Endpoint e = endpointOf(c.stream);
        c.leg.sourceIp = sourceAddress();
        c.leg.destinationIp = QString::fromStdString(e.address);
        c.leg.destinationPort = e.port;
        c.leg.sourcePort = o.sourcePort > 0 ? o.sourcePort : 5004;
        c.leg.rtpEnabled = enabled[c.stream];
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
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (i.name() != interfaceName()) continue;
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
    QJsonArray senders;
    for (const Connection& c : connections_) senders.append(idText(c.id));
    return QJsonObject{{"id", idText(deviceId_)},
                       {"version", deviceVersion_},
                       {"label", o_.label},
                       {"description", text("ST 2110 reference output")},
                       {"tags", QJsonObject{}},
                       {"type", "urn:x-nmos:device:generic"},
                       {"node_id", idText(nodeId_)},
                       {"senders", senders},
                       {"receivers", QJsonArray{}},
                       {"controls", QJsonArray{QJsonObject{{"href", QStringLiteral("http://%1:%2/x-nmos/connection/v1.1/").arg(address_).arg(port())},
                                                           {"type", "urn:x-nmos:control:sr-ctrl/v1.1"},
                                                           {"authorization", false}}}}};
}

QJsonArray Node::sourceResources() const {
    QJsonArray out;
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
            f["media_type"] = "video/raw";
            f["components"] = QJsonArray{QJsonObject{{"name", "Y"}, {"width", w}, {"height", h}, {"bit_depth", d}},
                                         QJsonObject{{"name", "Cb"}, {"width", w / 2}, {"height", h}, {"bit_depth", d}},
                                         QJsonObject{{"name", "Cr"}, {"width", w / 2}, {"height", h}, {"bit_depth", d}}};
        } else if (i == 1) {
            f["format"] = "urn:x-nmos:format:audio";
            f["sample_rate"] = QJsonObject{{"numerator", audio_.rate}, {"denominator", 1}};
            f["media_type"] = "audio/L24";
            f["bit_depth"] = 24;
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
    const bool multicast = st2110::isMulticast(c.leg.destinationIp.toStdString());
    return QJsonObject{
        {"id", idText(c.id)},
        {"version", c.version},
        {"label", o_.label + " " + labels[stream]},
        {"description", text("ST 2110-%1").arg(stream == 0 ? "20" : stream == 1 ? "30" : "40")},
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
        {"interface_bindings", QJsonArray{interfaceName()}},
        // (parked: no receiver)
        {"subscription", QJsonObject{{"receiver_id", c.masterEnable ? c.receiverId : QJsonValue(QJsonValue::Null)}, {"active", c.masterEnable}}}};
}

QJsonArray Node::senderResources() const {
    QJsonArray out;
    for (size_t i = 0; i < connections_.size(); ++i) out.append(senderResource(int(i)));
    return out;
}

// ---- IS-05.

QJsonObject Node::legJson(const Leg& l, int stream, bool resolve) const {
    QJsonObject o{{"source_ip", l.sourceIp}, {"destination_ip", l.destinationIp}, {"source_port", l.sourcePort},
                  {"destination_port", l.destinationPort}, {"rtp_enabled", l.rtpEnabled}};
    if (resolve) {
        const st2110::Endpoint e = endpointOf(stream);
        if (l.sourceIp == "auto") o["source_ip"] = sourceAddress();
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
                           {"transport_params", QJsonArray{legJson(c.stagedLeg, c.stream, false)}}};
    }
    activation = QJsonObject{{"mode", c.lastMode.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastMode)},
                             {"requested_time", c.lastRequested.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastRequested)},
                             {"activation_time", c.lastActivation.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c.lastActivation)}};
    return QJsonObject{{"receiver_id", c.receiverId}, {"master_enable", c.masterEnable}, {"activation", activation},
                       {"transport_params", QJsonArray{legJson(c.leg, c.stream, true)}}};
}

QJsonArray Node::constraintsJson(int) const {
    QJsonArray sources;
    if (!options_.interfaceAddress.empty()) {
        sources.append(QString::fromStdString(options_.interfaceAddress));
    } else {
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol) sources.append(a.toString());
    }
    return QJsonArray{QJsonObject{{"source_ip", QJsonObject{{"enum", sources}}},
                                  {"destination_ip", QJsonObject{}},
                                  {"source_port", QJsonObject{}},
                                  {"destination_port", QJsonObject{}},
                                  {"rtp_enabled", QJsonObject{}}}};
}

QString Node::transportFile(int stream) const {
    const Connection& c = connections_[size_t(stream)];
    const QJsonObject leg = legJson(c.leg, stream, true);
    st2110::Endpoint to{leg["destination_ip"].toString().toStdString(), leg["destination_port"].toInt()};
    st2110::SdpOptions so;
    so.source = leg["source_ip"].toString().toStdString();
    so.name = o_.label.toStdString();
    so.refclk = options_.refclk;
    so.ttl = options_.ttl;
    so.session = uint64_t(lastVersion_ / 1000000000);
    if (stream == 0) return QString::fromStdString(st2110::videoSdp(video_, to, options_.videoPayload, so));
    if (stream == 1) return QString::fromStdString(st2110::audioSdp(audio_, to, options_.audioPayload, so));
    return QString::fromStdString(st2110::ancSdp(video_, to, options_.ancPayload, so));
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
    Leg leg = c.stagedLeg;
    if (patch.contains("transport_params")) {
        if (!patch["transport_params"].isArray()) return errorResponse(400, text("transport_params is an array"));
        const QJsonArray legs = patch["transport_params"].toArray();
        if (legs.size() != 1) return errorResponse(400, text("This sender has one leg"));
        if (!legs[0].isObject()) return errorResponse(400, text("A leg is an object"));
        const QJsonObject p = legs[0].toObject();
        const QJsonArray allowedSources = constraintsJson(c.stream)[0].toObject()["source_ip"].toObject()["enum"].toArray();
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
    c.stagedLeg = leg;
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
    c.leg = c.stagedLeg;
    c.mode = c.requestedTime = QJsonValue::Null;
    c.version = newVersion();
    applyActive();
    resourceChanged(QStringLiteral("sender"), senderResource(c.stream));
}

void Node::applyActive() {
    // The output's options from the active legs ("auto": as it was).
    st2110::SenderOptions o = options_;
    for (const Connection& c : connections_) {
        const QJsonObject leg = legJson(c.leg, c.stream, true);
        st2110::Endpoint& e = c.stream == 0 ? o.video : c.stream == 1 ? o.audio : o.anc;
        e.address = leg["destination_ip"].toString().toStdString();
        e.port = leg["destination_port"].toInt();
        const bool on = c.masterEnable && c.leg.rtpEnabled;
        (c.stream == 0 ? o.sendVideo : c.stream == 1 ? o.sendAudio : o.sendAnc) = on;
        if (c.stream == 0) {
            if (c.leg.sourceIp != "auto") o.interfaceAddress = c.leg.sourceIp.toStdString();
            if (c.leg.sourcePort.isDouble()) o.sourcePort = c.leg.sourcePort.toInt();
        }
    }
    options_ = o;
    if (onActivated) onActivated(o);
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
            if (seg.size() == 4) return get ? jsonResponse(200, QJsonArray{}) : notAllowed();
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
            if (seg[4] == "senders")
                for (Connection& x : connections_)
                    if (idText(x.id) == id) c = &x;
            if (!c) {
                results.append(QJsonObject{{"id", id}, {"code", 404}, {"error", text("No such %1").arg(seg[4].chopped(1))}});
                continue;
            }
            const Response r = patchStaged(*c, o["params"].toObject(), true);
            QJsonObject result{{"id", id}, {"code", r.code}};
            if (r.code >= 400) result["error"] = QJsonDocument::fromJson(r.body).object()["error"];
            results.append(result);
        }
        return jsonResponse(200, results);
    }
    if (seg[3] != "single") return notFound();
    if (seg.size() == 4) return get ? jsonResponse(200, listing({"senders/", "receivers/"})) : notAllowed();
    if (seg[4] == "receivers") {
        if (seg.size() == 5) return get ? jsonResponse(200, QJsonArray{}) : notAllowed();
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

void Node::discover() {
    if (stopping_ || !running()) return;
    if (!o_.registry.isEmpty()) {
        registries_ = {Registry{o_.registry.endsWith('/') ? o_.registry.chopped(1) : o_.registry, 0, QStringLiteral("given")}};
        registryIndex_ = 0;
        registerAll();
        return;
    }
    const QString domain = o_.domain.isEmpty() ? systemDomain() : o_.domain;
    if (domain.isEmpty()) {
        rediscover_->start(10000);
        return;
    }
    found_.clear();
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
        rediscover_->start(2000);
        return;
    }
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
    registering_ = true;
    deleteFirst_ = false;
    postNext();
}

void Node::postNext() {
    if (queue_.empty()) {
        registering_ = false;
        if (!registered_) {
            registered_ = true;
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
        if (onRegistered) onRegistered(false);
    }
    rediscover_->start(1000);
}

void Node::resourceChanged(const QString& type, const QJsonObject& data) {
    if (!registered_ || registering_ || stopping_ || registry().isEmpty()) return;
    QNetworkRequest req(QUrl(registry() + QStringLiteral("/x-nmos/registration/v1.3/resource")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setTransferTimeout(std::max(1000, o_.heartbeatMs));
    QNetworkReply* reply = net_->post(req, toJson(QJsonObject{{"type", type}, {"data", data}}));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

}  // namespace montage::nmos
