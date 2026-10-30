// Montage — an AMWA NMOS Node for the ST 2110 reference output (render/St2110.h), so a facility's broadcast controller
// finds and routes it as it does other IP gear:
//  - IS-04 v1.3: the Node API over HTTP (the node, its device, the video, sound and data sources, flows and senders),
//    registered with the registry found by DNS-SD (unicast DNS: _nmos-register._tcp in the search domain, from a
//    given server or the system's; the best priority first, failing over to the next) or named outright, and kept
//    there by a heartbeat.
//  - IS-05 v1.1: the Connection API for the three senders: their constraints, staged and active transport parameters
//    (source and destination addresses and ports, on or off), activation at once or scheduled (absolute or relative
//    TAI times), bulk changes, and the SDP transport file each sends.
// Activating changes the sender (`onActivated`); the resources' versions change with it, and the registry is told.
#pragma once

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QUuid>
#include <functional>
#include <memory>
#include <vector>

#include "render/St2110.h"

class QTcpServer;
class QTcpSocket;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

namespace montage::nmos {

// A TAI time as NMOS writes it: "<seconds>:<nanoseconds>".
QString taiText(int64_t ns);
bool parseTai(const QString& text, int64_t& ns);

struct NodeOptions {
    QString label = QStringLiteral("Montage");
    QString hostname;              // "" = this computer's name
    QString address;               // the address the APIs are reached at ("" = the interface's, else the first found)
    int port = 0;                  // the APIs' HTTP port (0: any free)
    // Finding the registry: a given Registration API ("http://host:port"), else DNS-SD in `domain` ("" = the system's
    // search domain) through `dnsServer` ("" = the system's).
    QString registry;
    QString domain;
    QString dnsServer;
    bool discover = true;          // look for a registry at all (else the Node API alone, peer to peer)
    int heartbeatMs = 5000;
};

// A registry DNS-SD offered.
struct Registry {
    QString url;  // "http://host:port"
    int priority = 100;
    QString name;
};

// (the engine has no moc: callbacks rather than signals)
class Node : public QObject {
public:
    explicit Node(QObject* parent = nullptr);
    ~Node() override;

    // Starts the APIs (and registration); false and why when the port cannot be had.
    bool start(const NodeOptions& o, QString* error = nullptr);
    void stop();  // (unregistering first, when registered)
    bool running() const;
    int port() const;
    QString apiUrl() const;  // "http://address:port"

    // What the senders send, and where: the reference output's format and the options it sends with; `active`
    // whether it is sending. Changing them changes the resources (and tells the registry).
    void setStreams(const st2110::VideoFormat& video, const st2110::AudioFormat& audio, const st2110::SenderOptions& o, bool active);
    st2110::SenderOptions activeOptions() const { return options_; }

    // The registry in use ("" when none), and those DNS-SD offered at the last look.
    QString registry() const;
    std::vector<Registry> registries() const { return registries_; }
    bool registered() const { return registered_; }

    // Ids, kept from one run to the next (made from the host name and label).
    QUuid nodeId() const { return nodeId_; }
    QUuid deviceId() const { return deviceId_; }
    QUuid senderId(int stream) const;  // 0 video, 1 audio, 2 data

    // An HTTP request answered as the APIs answer it (the server's own handling; tests call it directly too).
    struct Response {
        int code = 200;
        QByteArray body;
        QByteArray contentType = "application/json";
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    Response handle(const QByteArray& method, const QString& path, const QByteArray& body);

    // A sender's activation changed where (or whether) the streams go: the options to send with now.
    std::function<void(const st2110::SenderOptions&)> onActivated;
    std::function<void(bool registered)> onRegistered;

private:
    struct Leg {
        QString sourceIp = QStringLiteral("auto"), destinationIp = QStringLiteral("auto");
        QJsonValue sourcePort = QStringLiteral("auto"), destinationPort = QStringLiteral("auto");
        bool rtpEnabled = true;
    };
    struct Connection {
        QUuid id;
        int stream = 0;
        QJsonValue receiverId;  // (null)
        bool masterEnable = true;
        Leg leg;
        // Staged, and when it is to happen.
        QJsonValue stagedReceiverId;
        bool stagedMasterEnable = true;
        Leg stagedLeg;
        QJsonValue mode, requestedTime;  // (null when none scheduled)
        int64_t activationNs = 0;
        QTimer* timer = nullptr;
        QString lastActivation;  // the activation_time last made (active's)
        QString lastMode, lastRequested;
        QString version;  // the IS-04 sender's
    };

    QJsonObject nodeResource() const;
    QJsonObject deviceResource() const;
    QJsonArray sourceResources() const;
    QJsonArray flowResources() const;
    QJsonArray senderResources() const;
    QJsonObject senderResource(int stream) const;
    QJsonObject connectionJson(const Connection& c, bool staged) const;
    QJsonObject legJson(const Leg& l, int stream, bool resolve) const;
    QJsonArray constraintsJson(int stream) const;
    QString transportFile(int stream) const;
    Response patchStaged(Connection& c, const QJsonObject& patch, bool bulk);
    void activate(Connection& c, const QString& mode);
    void applyActive();
    QString newVersion();
    st2110::Endpoint endpointOf(int stream) const;
    QString interfaceName() const;
    QString sourceAddress() const;

    // Registration.
    void discover();
    void lookupDone();
    void registerAll();
    void postNext();
    void heartbeat();
    void registryFailed();
    void resourceChanged(const QString& type, const QJsonObject& data);

    NodeOptions o_;
    QTcpServer* server_ = nullptr;
    QNetworkAccessManager* net_ = nullptr;
    QTimer* heartbeat_ = nullptr;
    QTimer* rediscover_ = nullptr;
    QObject* run_ = nullptr;  // this run's DNS-SD lookups (gone with it)
    QString address_;
    QUuid nodeId_, deviceId_;
    QUuid sourceIds_[3], flowIds_[3];
    std::vector<Connection> connections_;
    QString nodeVersion_, deviceVersion_, sourceVersion_[3], flowVersion_[3];
    int64_t lastVersion_ = 0;
    st2110::VideoFormat video_;
    st2110::AudioFormat audio_;
    st2110::SenderOptions options_;
    bool active_ = false;
    std::vector<Registry> registries_;
    int registryIndex_ = -1;
    bool registered_ = false, registering_ = false;
    std::vector<std::pair<QString, QJsonObject>> queue_;  // resources to POST, in order
    bool deleteFirst_ = false;
    int lookups_ = 0;
    std::vector<Registry> found_;
    bool stopping_ = false;
};

}  // namespace montage::nmos
