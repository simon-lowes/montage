// Montage — reaching the agent link of a running Montage (app/LiveLink.h) from outside it: where the app writes its
// address and key, one request over HTTP, and the stdio bridge `montage-cli mcp --live` that MCP clients which only
// start programs (Claude Desktop, Claude Code's stdio servers) use to reach the open project.
#pragma once

#include <QByteArray>
#include <QString>
#include <iosfwd>

namespace montage {

struct LiveConnection {
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 0;
    QString token;    // sent as "Authorization: Bearer <token>"
    qint64 pid = 0;   // the app's process
    QString project;  // the project open when it was written
    QString url() const { return QStringLiteral("http://%1:%2/mcp").arg(host).arg(port); }
};

// $MONTAGE_MCP_LIVE_FILE, else mcp-live.json in the user's Montage configuration folder.
QString liveConnectionFile();
bool writeLiveConnection(const LiveConnection& c, QString* error = nullptr);  // readable by the user only
bool readLiveConnection(LiveConnection& out, QString* error = nullptr);
// POSTs one JSON-RPC message to the app and waits for its answer ("" for a notification). False with `error`.
bool postLiveMessage(const LiveConnection& c, const QByteArray& message, QByteArray& response, QString* error = nullptr);
// Relays newline-delimited JSON-RPC between `in` / `out` and the running app until `in` closes. While the app is
// not reachable, each request is answered with an error saying so.
int runLiveBridge(std::istream& in, std::ostream& out);

}  // namespace montage
