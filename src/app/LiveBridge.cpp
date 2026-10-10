#include "LiveBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <iostream>
#include <string>

namespace montage {

QString liveConnectionFile() {
    const QByteArray env = qgetenv("MONTAGE_MCP_LIVE_FILE");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/Montage/mcp-live.json");
}

bool writeLiveConnection(const LiveConnection& c, QString* error) {
    const QString file = liveConnectionFile();
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = out.errorString();
        return false;
    }
    const QJsonObject o{{"url", c.url()}, {"host", c.host}, {"port", int(c.port)}, {"token", c.token}, {"pid", double(c.pid)}, {"project", c.project}};
    out.write(QJsonDocument(o).toJson());
    if (!out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);  // the key is the user's alone
    return true;
}

bool readLiveConnection(LiveConnection& out, QString* error) {
    QFile f(liveConnectionFile());
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Montage is not running with Tools > Agent Link turned on");
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    out.host = o.value("host").toString(QStringLiteral("127.0.0.1"));
    out.port = quint16(o.value("port").toInt());
    out.token = o.value("token").toString();
    out.pid = qint64(o.value("pid").toDouble());
    out.project = o.value("project").toString();
    if (!out.port || out.token.isEmpty()) {
        if (error) *error = QStringLiteral("The agent link file %1 is not readable").arg(f.fileName());
        return false;
    }
    return true;
}

bool postLiveMessage(const LiveConnection& c, const QByteArray& message, QByteArray& response, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    response.clear();
    QTcpSocket socket;
    socket.connectToHost(c.host, c.port);
    if (!socket.waitForConnected(10000)) return fail(QStringLiteral("Cannot reach Montage at %1: %2").arg(c.url(), socket.errorString()));
    QByteArray request = "POST /mcp HTTP/1.1\r\nHost: " + c.host.toUtf8() + ':' + QByteArray::number(c.port) +
                         "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nAuthorization: Bearer " +
                         c.token.toUtf8() + "\r\nContent-Length: " + QByteArray::number(message.size()) + "\r\nConnection: close\r\n\r\n" + message;
    socket.write(request);
    if (!socket.waitForBytesWritten(10000)) return fail(socket.errorString());
    // The answer comes when the tool has finished, which can take minutes (transcription, renders).
    QByteArray got;
    int headerEnd = -1;
    qint64 length = -1;
    int status = 0;
    for (;;) {
        if (headerEnd < 0) {
            headerEnd = int(got.indexOf("\r\n\r\n"));
            if (headerEnd >= 0) {
                const QList<QByteArray> lines = got.left(headerEnd).split('\n');
                const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                status = first.value(1).toInt();
                for (const QByteArray& l : lines) {
                    const int colon = int(l.indexOf(':'));
                    if (colon > 0 && l.left(colon).trimmed().toLower() == "content-length") length = l.mid(colon + 1).trimmed().toLongLong();
                }
            }
        }
        if (headerEnd >= 0 && length >= 0 && got.size() - headerEnd - 4 >= length) break;
        if (socket.state() != QAbstractSocket::ConnectedState && !socket.bytesAvailable()) break;
        if (!socket.waitForReadyRead(-1) && !socket.bytesAvailable() && socket.state() != QAbstractSocket::ConnectedState) {
            got += socket.readAll();
            break;
        }
        got += socket.readAll();
    }
    if (headerEnd < 0) return fail(QStringLiteral("Montage closed the connection without answering"));
    response = got.mid(headerEnd + 4, length >= 0 ? length : -1);
    if (status == 202) {
        response.clear();
        return true;
    }
    if (status == 401) return fail(QStringLiteral("Montage refused the agent link key (turn Agent Link off and on again)"));
    if (status != 200) return fail(QStringLiteral("Montage answered HTTP %1").arg(status));
    return true;
}

int runLiveBridge(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const QByteArray message = QByteArray::fromStdString(line);
        LiveConnection c;
        QString err;
        QByteArray response;
        const bool sent = readLiveConnection(c, &err) && postLiveMessage(c, message, response, &err);
        if (sent) {
            if (!response.trimmed().isEmpty()) out << QJsonDocument::fromJson(response).toJson(QJsonDocument::Compact).toStdString() << '\n';
        } else {
            const QJsonObject request = QJsonDocument::fromJson(message).object();
            if (!request.contains("id")) continue;  // a notification: nothing to answer
            const QJsonObject reply{{"jsonrpc", "2.0"}, {"id", request.value("id")}, {"error", QJsonObject{{"code", -32000}, {"message", err}}}};
            out << QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString() << '\n';
        }
        out.flush();
    }
    return 0;
}

}  // namespace montage
