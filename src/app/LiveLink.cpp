#include "LiveLink.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtConcurrent>
#include <set>

#include "EditorState.h"
#include "LiveBridge.h"
#include "automation/McpServer.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "core/ProjectIO.h"

namespace montage {

namespace {

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

QByteArray lastLine(const std::vector<std::string>& lines) { return lines.empty() ? QByteArray() : QByteArray::fromStdString(lines.back()); }

QJsonObject toolResult(const QString& text, const QJsonObject& structured, bool isError) {
    QJsonObject r{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}, {"isError", isError}};
    if (!structured.isEmpty()) r["structuredContent"] = structured;
    return r;
}

QString canonical(const QString& path) {
    const QFileInfo fi(path);
    return fi.exists() ? fi.canonicalFilePath() : QDir::cleanPath(fi.absoluteFilePath());
}

// The project as the link compares it: the playhead and marks set aside, since playback moves them.
Project withoutMarks(Project p) {
    for (Sequence& s : p.sequences) s.playhead = 0, s.inPoint = s.outPoint = -1;
    return p;
}

QString trackName(const Sequence& s, Id clip) {
    for (size_t i = 0; i < s.videoTracks.size(); ++i)
        for (const Clip& c : s.videoTracks[i].clips)
            if (c.id == clip) return QStringLiteral("V%1").arg(i + 1);
    for (size_t i = 0; i < s.audioTracks.size(); ++i)
        for (const Clip& c : s.audioTracks[i].clips)
            if (c.id == clip) return QStringLiteral("A%1").arg(i + 1);
    return {};
}

const char* kContextTool = R"json({"name":"montage_live_context","title":"What is open in Montage",
"description":"The project open in the Montage app (on the agent link, tools without a \"project\" argument work on it, each call one undo step the editor can take back): its file, the active sequence (size, rate, length), the playhead, In and Out, the selected clips (ids for the \"clips\" arguments of other tools) and the last undo step. Can move the playhead (timecode or seconds) or select clips, to show the editor a result.",
"inputSchema":{"type":"object","properties":{"playhead":{"type":["string","number"],"description":"Move the playhead here (timecode or seconds)"},
"select":{"type":"array","items":{"type":"number"},"description":"Select these clips (ids)"}}},
"annotations":{"readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}})json";

// Whether a tool works on an existing project (so it can work on the open one): not one whose "project" is the new
// file it writes (Create a project, Import a timeline).
bool readsProject(const QJsonObject& tool) {
    const QJsonObject props = tool.value("inputSchema").toObject().value("properties").toObject();
    return props.contains("project") && !props.value("project").toObject().value("description").toString().contains(QStringLiteral("file to write"));
}

constexpr qint64 kMaxRequest = 16 * 1024 * 1024;  // bytes: far beyond any tool call

}  // namespace

LiveLink::LiveLink(EditorState* state, QObject* parent)
    : QObject(parent), state_(state), protocol_(std::make_unique<McpServer>()), worker_(std::make_shared<McpServer>()) {
    const QJsonObject list = QJsonDocument::fromJson(lastLine(protocol_->handle(
                                                         compact(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}).toStdString())))
                                 .object();
    for (const QJsonValue& v : list.value("result").toObject().value("tools").toArray()) {
        const QJsonObject t = v.toObject();
        tools_[t.value("name").toString()] = {t.value("title").toString(), readsProject(t)};
    }
    // The connection file names the open project.
    connect(state_, &EditorState::fileStateChanged, this, [this] {
        if (!running()) return;
        LiveConnection c;
        if (!readLiveConnection(c) || c.token != token_ || c.project == state_->filePath()) return;
        c.project = state_->filePath();
        writeLiveConnection(c);
    });
}

LiveLink::~LiveLink() {
    if (cancel_) *cancel_ = true;  // a tool still running finishes on its own and discards its copy
    stop();
}

bool LiveLink::start(quint16 port, QString* error) {
    if (running()) return true;
    server_ = new QTcpServer(this);
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        if (error) *error = server_->errorString();
        delete server_;
        server_ = nullptr;
        return false;
    }
    connect(server_, &QTcpServer::newConnection, this, &LiveLink::onConnection);
    QByteArray key(24, Qt::Uninitialized);
    for (char& c : key) c = char(QRandomGenerator::system()->bounded(256));
    token_ = QString::fromLatin1(key.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    LiveConnection c;
    c.port = server_->serverPort();
    c.token = token_;
    c.pid = QCoreApplication::applicationPid();
    c.project = state_->filePath();
    if (!writeLiveConnection(c, error)) {
        stop();
        return false;
    }
    emit runningChanged(true);
    return true;
}

void LiveLink::stop() {
    if (!server_) return;
    // Agents' calls stop with the link: those waiting are dropped and a running one's result is not applied.
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->external) it = queue_.erase(it);
        else ++it;
    }
    if (cancel_ && runningExternal_) *cancel_ = true;
    server_->close();
    delete server_;
    server_ = nullptr;
    // Only our own file: another Montage may have taken the link since.
    LiveConnection c;
    if (readLiveConnection(c) && c.token == token_) QFile::remove(liveConnectionFile());
    token_.clear();
    emit runningChanged(false);
}

bool LiveLink::running() const { return server_ && server_->isListening(); }
quint16 LiveLink::port() const { return server_ ? server_->serverPort() : 0; }
QString LiveLink::url() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(port()); }

// ---- HTTP -------------------------------------------------------------------------------------------------------

void LiveLink::onConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        buffers_[s];
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadable(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            buffers_.erase(s);
            s->deleteLater();
        });
    }
}

void LiveLink::respond(QTcpSocket* socket, int status, const QByteArray& body) {
    static const std::map<int, const char*> reasons = {{200, "OK"}, {202, "Accepted"}, {400, "Bad Request"}, {401, "Unauthorized"},
                                                      {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"},
                                                      {413, "Payload Too Large"}};
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasons.at(status) + "\r\n";
    if (!body.isEmpty()) head += "Content-Type: application/json\r\n";
    if (status == 401) head += "WWW-Authenticate: Bearer\r\n";
    if (status == 405) head += "Allow: POST\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
    socket->write(head + body);
    socket->disconnectFromHost();
}

void LiveLink::onReadable(QTcpSocket* socket) {
    QByteArray& buf = buffers_[socket];
    buf += socket->readAll();
    const int headerEnd = int(buf.indexOf("\r\n\r\n"));
    if (headerEnd < 0) {
        if (buf.size() > 65536) respond(socket, 400, {});
        return;
    }
    const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    std::map<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = int(lines[i].indexOf(':'));
        if (colon > 0) headers[lines[i].left(colon).trimmed().toLower()] = lines[i].mid(colon + 1).trimmed();
    }
    const QByteArray method = request.value(0), path = request.value(1);
    // Checked as soon as the headers are in, before any body is kept. Only pages on this computer may call it (a web
    // page elsewhere cannot, even through the browser).
    auto refuse = [&](int status) {
        buf.clear();
        respond(socket, status, {});
    };
    if (headers.count("origin")) {
        const QByteArray origin = headers["origin"];
        if (!origin.startsWith("http://127.0.0.1") && !origin.startsWith("http://localhost")) return refuse(403);
    }
    if (path != "/mcp" && path != "/") return refuse(404);
    if (headers["authorization"] != "Bearer " + token_.toUtf8()) return refuse(401);
    if (method != "POST") return refuse(405);
    const qint64 length = headers.count("content-length") ? headers["content-length"].toLongLong() : 0;
    if (length < 0 || length > kMaxRequest) return refuse(413);
    if (buf.size() - headerEnd - 4 < length) return;  // the rest of the body is on its way
    const QByteArray body = buf.mid(headerEnd + 4, length);
    buf.clear();
    QPointer<QTcpSocket> alive(socket);
    handle(
        body,
        [this, alive](QByteArray answer) {
            if (!alive) return;
            respond(alive, answer.isEmpty() ? 202 : 200, answer);
        },
        true);
}

// ---- MCP --------------------------------------------------------------------------------------------------------

QByteArray LiveLink::patchedToolList(const QByteArray& answer) const {
    QJsonObject reply = QJsonDocument::fromJson(answer).object();
    QJsonObject result = reply.value("result").toObject();
    QJsonArray tools;
    for (const QJsonValue& v : result.value("tools").toArray()) {
        QJsonObject t = v.toObject();
        QJsonObject schema = t.value("inputSchema").toObject();
        QJsonObject props = schema.value("properties").toObject();
        if (readsProject(t)) {
            // The open project unless another file is named.
            props["project"] = QJsonObject{{"type", "string"}, {"description", "Leave out for the project open in Montage, or a .montage file to work on instead"}};
            schema["properties"] = props;
            QJsonArray required;
            for (const QJsonValue& r : schema.value("required").toArray())
                if (r.toString() != "project") required.append(r);
            schema["required"] = required;
            t["inputSchema"] = schema;
        }
        if (t.value("name").toString() == "montage_undo")
            t["description"] = "Undo the last change an agent made to the project open in Montage (one step; the editor's own changes are left alone). With \"project\", put that file back as it was before the last edit made through these tools.";
        tools.append(t);
    }
    tools.append(QJsonDocument::fromJson(kContextTool).object());
    result["tools"] = tools;
    reply["result"] = result;
    return compact(reply);
}

bool LiveLink::targetsOpenProject(const QJsonObject& args) const {
    const QString named = args.value("project").toString();
    return named.isEmpty() || (!state_->filePath().isEmpty() && canonical(named) == canonical(state_->filePath()));
}

QString LiveLink::snapshotPath() const {
    // Beside the project, so what a tool writes "beside the project" (room tone, speech, review copies) lands there.
    const QString path = state_->filePath();
    if (!path.isEmpty()) {
        const QFileInfo fi(path);
        return fi.absolutePath() + "/." + fi.completeBaseName() + ".agent.montage";
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Agent Link");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/Untitled.montage");
}

QJsonObject LiveLink::contextResult(const QJsonObject& args, bool& isError) {
    isError = false;
    const Sequence* s = state_->sequence();
    if (!s) {
        isError = true;
        return toolResult("No sequence is open", {}, true);
    }
    QStringList moved;
    if (args.contains("playhead")) {
        const QJsonValue v = args.value("playhead");
        FrameTime t = -1;
        if (v.isDouble()) t = FrameTime(std::llround(v.toDouble() * s->fpsValue()));
        else if (!parseTimecode(v.toString().toStdString(), s->fps, t)) t = -1;
        if (t < 0) {
            isError = true;
            return toolResult(QStringLiteral("\"%1\" is not a timecode or a number of seconds").arg(v.toVariant().toString()), {}, true);
        }
        state_->setPlayhead(t);
        moved << QStringLiteral("playhead moved");
    }
    if (args.contains("select")) {
        std::vector<Id> ids;
        for (const QJsonValue& v : args.value("select").toArray())
            if (edit::clipById(*s, Id(v.toDouble()))) ids.push_back(Id(v.toDouble()));
        state_->setSelection(ids, false);
        moved << QStringLiteral("%1 clip(s) selected").arg(ids.size());
    }
    s = state_->sequence();
    auto tc = [&](FrameTime f) { return f < 0 ? QJsonValue() : QJsonValue(QString::fromStdString(formatTimecode(f, s->fps))); };
    QJsonArray selected;
    for (Id id : state_->selectedClips())
        if (const Clip* c = edit::clipById(*s, id)) {
            const MediaItem* m = c->mediaId ? state_->project().findMedia(c->mediaId) : nullptr;
            QJsonObject o{{"id", double(id)}, {"name", QString::fromStdString(c->name)}, {"track", trackName(*s, id)},
                          {"start", tc(c->start)}, {"end", tc(c->end())}};
            if (m) o["media"] = QString::fromStdString(m->name);
            selected.append(o);
        }
    const QJsonObject info{{"project", state_->filePath().isEmpty() ? QJsonValue() : QJsonValue(state_->filePath())},
                           {"modified", state_->isModified()},
                           {"sequence", QString::fromStdString(s->name)},
                           {"width", s->width},
                           {"height", s->height},
                           {"fps", s->fpsValue()},
                           {"duration", tc(s->duration())},
                           {"playhead", tc(state_->playhead())},
                           {"playhead_seconds", double(state_->playhead()) / s->fpsValue()},
                           {"in", tc(s->inPoint)},
                           {"out", tc(s->outPoint)},
                           {"selected_clips", selected},
                           {"last_change", state_->canUndo() ? QJsonValue(state_->undoText()) : QJsonValue()}};
    QString text = QStringLiteral("%1, sequence \"%2\" (%3x%4, %5 fps), playhead %6, %7 clip(s) selected")
                       .arg(state_->filePath().isEmpty() ? QStringLiteral("An untitled project") : QFileInfo(state_->filePath()).fileName(),
                            QString::fromStdString(s->name))
                       .arg(s->width)
                       .arg(s->height)
                       .arg(s->fpsValue(), 0, 'g', 6)
                       .arg(info.value("playhead").toString())
                       .arg(selected.size());
    if (!moved.isEmpty()) text += " (" + moved.join(", ") + ")";
    return toolResult(text, info, false);
}

void LiveLink::handle(const QByteArray& message, std::function<void(QByteArray)> done, bool external) {
    const QJsonObject msg = QJsonDocument::fromJson(message).object();
    const QString method = msg.value("method").toString();
    if (method == "tools/list") {
        const QByteArray answer = lastLine(protocol_->handle(message.toStdString()));
        return done(QJsonDocument::fromJson(answer).object().contains("result") ? patchedToolList(answer) : answer);
    }
    if (method != "tools/call" || !msg.contains("id")) return done(lastLine(protocol_->handle(message.toStdString())));
    const QJsonObject params = msg.value("params").toObject();
    const QString name = params.value("name").toString();
    const QJsonObject args = params.value("arguments").toObject();
    // Answers made here take the shape the server gives this request's protocol era (a ping with the same _meta).
    auto answer = [&](const QJsonObject& result) {
        QJsonObject ping{{"jsonrpc", "2.0"}, {"id", msg.value("id")}, {"method", "ping"}, {"params", QJsonObject{{"_meta", params.value("_meta")}}}};
        if (!params.contains("_meta")) ping.remove("params");
        QJsonObject reply = QJsonDocument::fromJson(lastLine(protocol_->handle(compact(ping).toStdString()))).object();
        if (reply.contains("error")) return done(compact(reply));
        QJsonObject r = reply.value("result").toObject();
        for (auto it = result.begin(); it != result.end(); ++it) r[it.key()] = it.value();
        reply["result"] = r;
        done(compact(reply));
    };
    if (name == "montage_live_context") {
        bool isError = false;
        return answer(contextResult(args, isError));
    }
    if (name == "montage_undo" && targetsOpenProject(args)) {
        // Only an agent's own change: the editor's are theirs to undo.
        const QString last = state_->undoText();
        if (!state_->canUndo() || !last.startsWith(QStringLiteral("Assistant: ")))
            return answer(toolResult(state_->canUndo() ? QStringLiteral("The last change (%1) was the editor's; it is left alone").arg(last)
                                                       : QStringLiteral("Nothing to undo"),
                                     {}, true));
        state_->undo();
        return answer(toolResult(QStringLiteral("Undid %1").arg(last), {}, false));
    }
    queue_.push_back({msg, std::move(done), external});
    pump();
}

void LiveLink::pump() {
    if (busy_ || queue_.empty()) return;
    busy_ = true;
    Job job = std::move(queue_.front());
    queue_.pop_front();
    QJsonObject msg = job.message;
    QJsonObject params = msg.value("params").toObject();
    const QString tool = params.value("name").toString();
    QJsonObject args = params.value("arguments").toObject();
    const auto info = tools_.find(tool);
    const bool live = info != tools_.end() && info->second.takesProject && targetsOpenProject(args);
    Project before;
    QByteArray snapshot;
    QString path;
    if (live) {
        // The open project, saved where the tool will read it.
        before = state_->project();
        path = snapshotPath();
        std::string err;
        if (!saveProject(before, path.toStdString(), &err)) {
            busy_ = false;
            const QJsonObject reply{{"jsonrpc", "2.0"}, {"id", msg.value("id")},
                                    {"error", QJsonObject{{"code", -32000}, {"message", QStringLiteral("Cannot save a copy of the open project: %1").arg(QString::fromStdString(err))}}}};
            job.done(compact(reply));
            return pump();
        }
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) snapshot = f.readAll();
        args["project"] = path;
        params["arguments"] = args;
        msg["params"] = params;
    }
    auto* watcher = new QFutureWatcher<std::string>(this);
    // The tool holds its server, so it can finish safely if the app quits or the link stops meanwhile; then its copy
    // of the project is removed and nothing is applied.
    std::shared_ptr<McpServer> worker = worker_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    runningExternal_ = job.external;
    const std::string line = compact(msg).toStdString();
    connect(watcher, &QFutureWatcher<std::string>::finished, this, [this, watcher, job, tool, live, before, snapshot, path, cancel] {
        const std::string answer = watcher->result();
        watcher->deleteLater();
        cancel_.reset();
        if (*cancel) {
            if (live) QFile::remove(path), QFile::remove(path + ".bak");
            const QJsonObject r = toolResult(QStringLiteral("The agent link was turned off while %1 ran, so its result was not applied.").arg(tool), {}, true);
            job.done(compact(QJsonObject{{"jsonrpc", "2.0"}, {"id", job.message.value("id")}, {"result", r}}));
        } else if (live) {
            finish(job, tool, before, snapshot, path, answer);
        } else {
            job.done(QByteArray::fromStdString(answer));
        }
        busy_ = false;
        pump();
    });
    const QString copy = live ? path : QString();
    watcher->setFuture(QtConcurrent::run([worker, line, cancel, copy] {
        const std::vector<std::string> lines = worker->handle(line);
        if (*cancel && !copy.isEmpty()) QFile::remove(copy), QFile::remove(copy + ".bak");
        return lines.empty() ? std::string() : lines.back();
    }));
}

void LiveLink::finish(const Job& job, const QString& tool, const Project& before, const QByteArray& snapshot, const QString& path,
                      const std::string& answer) {
    QByteArray after;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) after = f.readAll();
    }
    QJsonObject reply = QJsonDocument::fromJson(QByteArray::fromStdString(answer)).object();
    const QString shown = state_->filePath().isEmpty() ? QStringLiteral("the open project") : state_->filePath();
    if (!after.isEmpty() && after != snapshot) {
        if (!(withoutMarks(state_->project()) == withoutMarks(before))) {
            // The editor changed the project while the tool ran: theirs stands.
            QJsonObject r = toolResult(QStringLiteral("The project was changed in Montage while %1 ran, so its result was not applied. Call it again.").arg(tool), {}, true);
            reply = QJsonObject{{"jsonrpc", "2.0"}, {"id", job.message.value("id")}, {"result", r}};
        } else {
            Project loaded;
            std::string err;
            if (loadProject(path.toStdString(), loaded, &err)) {
                // The editor's playhead and marks stay where they are.
                for (Sequence& s : loaded.sequences)
                    if (const Sequence* now = state_->project().findSequence(s.id)) s.playhead = now->playhead, s.inPoint = now->inPoint, s.outPoint = now->outPoint;
                std::set<Id> had;
                for (const MediaItem& m : state_->project().media) had.insert(m.id);
                const auto info = tools_.find(tool);
                const QString label = QStringLiteral("Assistant: %1").arg(info != tools_.end() && !info->second.title.isEmpty() ? info->second.title : tool);
                state_->edit(label, [&loaded](Project& p, Sequence&) {
                    p = std::move(loaded);
                    return true;
                });
                for (const MediaItem& m : state_->project().media)
                    if (!had.count(m.id) && m.hasAudio) state_->startAudioDecode(m);
                emit applied(label);
            }
        }
    }
    QFile::remove(path);
    QFile::remove(path + ".bak");
    // The answer names the open project, not the copy the tool worked on.
    QByteArray text = compact(reply);
    text.replace(QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact).mid(2).chopped(2), QJsonDocument(QJsonArray{shown}).toJson(QJsonDocument::Compact).mid(2).chopped(2));
    job.done(text);
}

}  // namespace montage
