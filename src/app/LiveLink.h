// Montage — the agent link (Premiere 26's AI Assistant, Avid's agentic editing and Descript's Underlord work on the
// project in front of the editor; Montage's MCP tools otherwise edit project files on disk): AI agents edit the
// project open in the app through every MCP tool, each call one undo step named "Assistant: <tool>", and can read
// and move the playhead and selection. Served as MCP over HTTP on this computer only (127.0.0.1), with a key: the
// address and key are written to liveConnectionFile() for `montage-cli mcp --live`, the stdio bridge.
//
// A tool runs on a copy of the open project saved beside it (so files it writes land beside the project), off the
// UI thread; when it changed the copy, the copy becomes the open project in one undo step, unless the editor
// changed the project meanwhile (then nothing is applied and the agent is told). Calls run one at a time.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "core/Model.h"

class QTcpServer;
class QTcpSocket;

namespace montage {

class EditorState;
class McpServer;

class LiveLink : public QObject {
    Q_OBJECT
public:
    explicit LiveLink(EditorState* state, QObject* parent = nullptr);
    ~LiveLink() override;

    // Listens on 127.0.0.1 (`port` 0: any free one) with a new key, and writes the connection file.
    bool start(quint16 port = 0, QString* error = nullptr);
    void stop();  // and removes the connection file
    bool running() const;
    quint16 port() const;
    QString token() const { return token_; }
    QString url() const;
    bool busy() const { return busy_; }

    // One JSON-RPC message, as the HTTP endpoint takes it; `done` gets the answer ("" for a notification), now or
    // once the tool has run.
    void handle(const QByteArray& message, std::function<void(QByteArray)> done);

signals:
    void runningChanged(bool on);
    // A tool changed the open project: the undo step's name.
    void applied(const QString& label);

private:
    struct Job {
        QJsonObject message;
        std::function<void(QByteArray)> done;
    };
    struct ToolInfo {
        QString title;
        bool takesProject = false;
    };
    void onConnection();
    void onReadable(QTcpSocket* socket);
    void respond(QTcpSocket* socket, int status, const QByteArray& body);
    QByteArray patchedToolList(const QByteArray& list) const;
    QJsonObject contextResult(const QJsonObject& args, bool& isError);
    bool targetsOpenProject(const QJsonObject& args) const;
    QString snapshotPath() const;
    void pump();
    void finish(const Job& job, const QString& tool, const Project& before, const QByteArray& snapshot, const QString& path,
                const std::string& answer);

    EditorState* state_;
    QTcpServer* server_ = nullptr;
    QString token_;
    std::unique_ptr<McpServer> protocol_;  // answers protocol messages on the UI thread
    std::unique_ptr<McpServer> worker_;    // runs tools, one at a time, off it
    std::map<QString, ToolInfo> tools_;
    std::deque<Job> queue_;
    bool busy_ = false;
    std::map<QTcpSocket*, QByteArray> buffers_;
};

}  // namespace montage
