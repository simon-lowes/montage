#include "LiveLink.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
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
#include <QTimer>
#include <QtConcurrent>
#include <set>

#include "EditorState.h"
#include "LiveBridge.h"
#include "automation/McpServer.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "media/ImageSequence.h"

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

// The project as the link compares it: the playhead and marks set aside, since playback moves them, and what the
// files say (a growing recording's length is read again while a tool runs, outside any edit).
Project withoutMarks(Project p) {
    for (Sequence& s : p.sequences) s.playhead = 0, s.inPoint = s.outPoint = -1;
    const MediaItem blank;
    for (MediaItem& m : p.media) copyProbedFacts(m, blank, !m.subclipOf);
    return p;
}

}  // namespace

std::optional<Project> mergeToolResult(const Project& before, const Project& now, Project loaded, std::vector<std::string>* reread) {
    if (!(withoutMarks(now) == withoutMarks(before))) return std::nullopt;
    // The editor's playhead and marks stay where they are.
    for (Sequence& s : loaded.sequences)
        if (const Sequence* cur = now.findSequence(s.id)) s.playhead = cur->playhead, s.inPoint = cur->inPoint, s.outPoint = cur->outPoint;
    for (MediaItem& m : loaded.media) {
        const MediaItem* was = before.findMedia(m.id);
        const MediaItem* cur = now.findMedia(m.id);
        if (!was || !cur || m.path != cur->path) continue;
        if (sameProbedFacts(m, *was, !m.subclipOf)) {
            copyProbedFacts(m, *cur, !m.subclipOf);  // (read again in Montage, or unchanged)
        } else if (reread && !m.path.empty() && !sameProbedFacts(m, *cur, !m.subclipOf)) {
            const std::string file = mediaFileOnDisk(m.path);
            if (std::find(reread->begin(), reread->end(), file) == reread->end()) reread->push_back(file);
        }
    }
    return loaded;
}

namespace {

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
    // Background calls' progress, as the worker's tools report it.
    worker_->setNotificationSink([board = progress_](const std::string& line) {
        const QJsonObject n = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (n.value("method").toString() != QLatin1String("notifications/progress")) return;
        const QJsonObject p = n.value("params").toObject();
        std::lock_guard l(board->m);
        board->of[p.value("progressToken").toString()] = {p.value("progress").toDouble(), p.value("message").toString()};
    });
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
    if (cancel_) {
        // A tool still running stops where it can (so quitting doesn't wait for it) and discards its copy.
        *cancel_ = true;
        worker_->cancel(runningId_);
    }
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
        if (it->external) {
            if (!it->async.isEmpty()) asyncJobs_[it->async].status = QStringLiteral("cancelled");
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    if (cancel_ && runningExternal_) {
        *cancel_ = true;
        if (!runningAsync_.isEmpty()) cancelJob(runningAsync_);
        else worker_->cancel(runningId_);
    }
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
    for (const auto& [tool, run] : appTools_) tools.append(tool);
    result["tools"] = tools;
    reply["result"] = result;
    return compact(reply);
}

void LiveLink::addAppTool(const QByteArray& toolJson, AppTool run) {
    appTools_.emplace_back(QJsonDocument::fromJson(toolJson).object(), std::move(run));
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
    dispatch(message, std::move(done), external, {}, {});
}

void LiveLink::handleScript(const QByteArray& message, std::function<void(QByteArray)> done, const QString& label, const QString& group) {
    dispatch(message, std::move(done), false, label, group);
}

void LiveLink::cancelScript(const QString& group) {
    if (group.isEmpty()) return;
    std::vector<Job> dropped;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->group == group) dropped.push_back(std::move(*it)), it = queue_.erase(it);
        else ++it;
    }
    if (cancel_ && runningGroup_ == group) *cancel_ = true;
    if (cancel_ && runningGroup_ == group && !runningAsync_.isEmpty()) cancelJob(runningAsync_);
    for (Job& j : dropped) {
        if (!j.async.isEmpty()) asyncJobs_[j.async].cancel = true;
        const QJsonObject r = toolResult(QStringLiteral("The script was stopped before %1 ran").arg(j.message.value("params").toObject().value("name").toString()), {}, true);
        j.done(compact(QJsonObject{{"jsonrpc", "2.0"}, {"id", j.message.value("id")}, {"result", r}}));
    }
}

void LiveLink::dispatch(const QByteArray& message, std::function<void(QByteArray)> done, bool external, const QString& label,
                        const QString& group) {
    const QJsonObject msg = QJsonDocument::fromJson(message).object();
    const QString method = msg.value("method").toString();
    if (method == "tools/list") {
        const QByteArray answer = lastLine(protocol_->handle(message.toStdString()));
        return done(QJsonDocument::fromJson(answer).object().contains("result") ? patchedToolList(answer) : answer);
    }
    if (method == "notifications/cancelled") {
        // The running call's work stops (its server knows it by the same id); one still waiting is cancelled as it starts.
        worker_->cancel(msg.value("params").toObject().value("requestId"));
        return done({});
    }
    if (method != "tools/call" || !msg.contains("id")) return done(lastLine(protocol_->handle(message.toStdString())));
    const QJsonObject params = msg.value("params").toObject();
    const QString name = params.value("name").toString();
    const QJsonObject args = params.value("arguments").toObject();
    // Answers made here take the shape the server gives this request's protocol era (a ping with the same _meta).
    // (by value: montage_job may answer later)
    auto answer = [this, msg, params, done](const QJsonObject& result) {
        QJsonObject ping{{"jsonrpc", "2.0"}, {"id", msg.value("id")}, {"method", "ping"}, {"params", QJsonObject{{"_meta", params.value("_meta")}}}};
        if (!params.contains("_meta")) ping.remove("params");
        QJsonObject reply = QJsonDocument::fromJson(lastLine(protocol_->handle(compact(ping).toStdString()))).object();
        if (reply.contains("error")) return done(compact(reply));
        QJsonObject r = reply.value("result").toObject();
        for (auto it = result.begin(); it != result.end(); ++it) r[it.key()] = it.value();
        reply["result"] = r;
        done(compact(reply));
    };
    // Tools answered here at once: asked for in the background, a job that has already finished.
    const bool async = name != "montage_job" && args.value("async").toBool();
    auto give = [&](const QJsonObject& result) {
        if (!async) return answer(result);
        const QString id = QStringLiteral("job-%1").arg(++asyncMade_);
        AsyncJob& j = asyncJobs_[id];
        j.tool = name;
        j.result = result;
        j.status = result.value("isError").toBool() ? QStringLiteral("failed") : QStringLiteral("done");
        answer(toolResult(QStringLiteral("%1 ran as %2: montage_job gives its result").arg(name, id),
                          QJsonObject{{"job", id}, {"tool", name}, {"status", j.status}}, false));
    };
    if (name == "montage_live_context") {
        bool isError = false;
        return give(contextResult(args, isError));
    }
    for (const auto& [tool, run] : appTools_)
        if (tool.value("name").toString() == name) {
            QJsonObject structured;
            bool isError = false;
            const QString text = run(args, structured, isError);
            return give(toolResult(text, structured, isError));
        }
    if (name == "montage_job") return answerJob(args, answer);
    if (name == "montage_undo" && targetsOpenProject(args) && state_->readOnly())
        return give(toolResult(QStringLiteral("The project is open read-only (%1 is editing it): nothing to undo here")
                                   .arg(state_->lockHolder().isEmpty() ? QStringLiteral("someone else") : state_->lockHolder()),
                               {}, true));
    if (name == "montage_undo" && targetsOpenProject(args)) {
        // Only an agent's own change (a script's own run): the editor's are theirs to undo.
        const QString last = state_->undoText();
        // (A script's: this run's own step, not an earlier run of the same script.)
        if (!state_->canUndo() || !(label.isEmpty() ? last.startsWith(QStringLiteral("Assistant: ")) : last == label && state_->lastMergeKey() == group))
            return give(toolResult(state_->canUndo() ? QStringLiteral("The last change (%1) was the editor's; it is left alone").arg(last)
                                                     : QStringLiteral("Nothing to undo"),
                                   {}, true));
        state_->undo();
        return give(toolResult(QStringLiteral("Undid %1").arg(last), {}, false));
    }
    if (async) {
        // In the background: queued as any call (under the job's id, its progress reported under it), answered now.
        const QString id = QStringLiteral("job-%1").arg(++asyncMade_);
        QJsonObject a = args, p = params, m = msg;
        a.remove("async");
        p["arguments"] = a;
        QJsonObject meta = p.value("_meta").toObject();
        meta["progressToken"] = id;
        p["_meta"] = meta;
        m["params"] = p;
        m["id"] = id;
        asyncJobs_[id].tool = name;
        queue_.push_back({m, [this, id](QByteArray reply) { settleJob(id, reply); }, external, label, group, id, state_->projectGeneration()});
        answer(toolResult(QStringLiteral("%1 runs in the background as %2: montage_job reads its progress and result").arg(name, id),
                          QJsonObject{{"job", id}, {"tool", name}, {"status", "queued"}}, false));
        return pump();
    }
    queue_.push_back({msg, std::move(done), external, label, group, {}, state_->projectGeneration()});
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
    if (live && job.generation != state_->projectGeneration()) {
        // Asked for on a project that has since been closed: not run on the one open now.
        busy_ = false;
        job.done(compact(QJsonObject{{"jsonrpc", "2.0"}, {"id", msg.value("id")},
                                     {"result", toolResult(QStringLiteral("The project %1 was asked of was closed before it ran").arg(tool), {}, true)}}));
        return pump();
    }
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
    runningGroup_ = job.group;
    runningAsync_ = job.async;
    runningId_ = msg.value("id");
    if (!job.async.isEmpty()) {
        AsyncJob& a = asyncJobs_[job.async];
        a.status = QStringLiteral("running");
        if (a.cancel) *cancel = true;  // (cancelled as it was about to start)
    }
    const std::string line = compact(msg).toStdString();
    connect(watcher, &QFutureWatcher<std::string>::finished, this, [this, watcher, job, tool, live, before, snapshot, path, cancel] {
        const std::string answer = watcher->result();
        watcher->deleteLater();
        cancel_.reset();
        runningAsync_.clear();
        if (*cancel) {
            if (live) QFile::remove(path), QFile::remove(path + ".bak");
            const QJsonObject r = toolResult(job.group.isEmpty() ? QStringLiteral("The agent link was turned off while %1 ran, so its result was not applied.").arg(tool)
                                                                 : QStringLiteral("The script was stopped while %1 ran, so its result was not applied.").arg(tool),
                                             {}, true);
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

QJsonObject LiveLink::jobReport(const QString& id) {
    const auto it = asyncJobs_.find(id);
    if (it == asyncJobs_.end()) return toolResult(QStringLiteral("No job %1 (montage_job with action list gives them)").arg(id), {}, true);
    const AsyncJob& j = it->second;
    double fraction = j.status == QLatin1String("done") ? 1.0 : 0.0;
    QString step;
    {
        std::lock_guard l(progress_->m);
        if (const auto p = progress_->of.find(id); p != progress_->of.end()) {
            fraction = j.status == QLatin1String("done") ? 1.0 : std::clamp(p->second.first, 0.0, 1.0);
            step = p->second.second;
        }
    }
    QJsonObject o{{"job", id}, {"tool", j.tool}, {"status", j.status}, {"progress", fraction}};
    if (!step.isEmpty()) o["step"] = step;
    QString toolText;
    for (const QJsonValue& c : j.result.value("content").toArray())
        if (c.toObject().value("type").toString() == QLatin1String("text")) toolText += c.toObject().value("text").toString();
    if (j.status == QLatin1String("done")) {
        QJsonObject result = j.result.value("structuredContent").toObject();
        if (result.isEmpty()) result["text"] = toolText;
        else result["message"] = toolText;
        o["result"] = result;
    } else if (!toolText.isEmpty()) {
        o["error"] = toolText;
    }
    const QString text = j.status == QLatin1String("done")    ? QStringLiteral("%1 (%2) is done: %3").arg(id, j.tool, toolText)
                         : j.status == QLatin1String("failed") ? QStringLiteral("%1 (%2) failed: %3").arg(id, j.tool, toolText)
                         : j.status == QLatin1String("running")
                             ? QStringLiteral("%1 (%2) is running: %3 %%4").arg(id, j.tool).arg(std::lround(fraction * 100)).arg(step.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(step))
                             : QStringLiteral("%1 (%2) is %3").arg(id, j.tool, j.status);
    return toolResult(text, o, false);
}

void LiveLink::answerJob(const QJsonObject& args, std::function<void(const QJsonObject&)> done) {
    const QString action = args.value("action").toString(QStringLiteral("status"));
    if (action == QLatin1String("list")) {
        QJsonArray list;
        QStringList lines;
        std::vector<QString> ids;
        for (const auto& [id, j] : asyncJobs_) ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) { return a.mid(4).toInt() < b.mid(4).toInt(); });
        for (const QString& id : ids) {
            const QJsonObject r = jobReport(id);
            QJsonObject o = r.value("structuredContent").toObject();
            o.remove("result");
            list.append(o);
            lines << r.value("content").toArray().first().toObject().value("text").toString().section(QLatin1Char('\n'), 0, 0);
        }
        return done(toolResult(lines.isEmpty() ? QStringLiteral("No background calls") : lines.join(QLatin1Char('\n')), QJsonObject{{"jobs", list}}, false));
    }
    const QString id = args.value("job").toString();
    if (id.isEmpty()) return done(toolResult(QStringLiteral("\"job\" is required"), {}, true));
    if (action != QLatin1String("status") && action != QLatin1String("cancel"))
        return done(toolResult(QStringLiteral("\"action\" is status, cancel or list"), {}, true));
    if (!asyncJobs_.count(id)) return done(jobReport(id));
    if (action == QLatin1String("cancel")) cancelJob(id);
    // Asked to wait: answered when it has finished, or when the time is up (the UI goes on meanwhile).
    const double wait = std::clamp(args.value("wait").toDouble(0), 0.0, 60.0);
    auto finished = [this, id] {
        const auto it = asyncJobs_.find(id);
        return it == asyncJobs_.end() || (it->second.status != QLatin1String("queued") && it->second.status != QLatin1String("running"));
    };
    if (wait <= 0 || finished()) return done(jobReport(id));
    auto* timer = new QTimer(this);
    timer->setInterval(20);
    QElapsedTimer since;
    since.start();
    connect(timer, &QTimer::timeout, this, [this, timer, since, wait, id, finished, done] {
        if (!finished() && since.elapsed() < qint64(wait * 1000)) return;
        timer->deleteLater();
        done(jobReport(id));
    });
    timer->start();
}

void LiveLink::cancelJob(const QString& id) {
    const auto it = asyncJobs_.find(id);
    if (it == asyncJobs_.end()) return;
    AsyncJob& j = it->second;
    j.cancel = true;
    if (j.status == QLatin1String("queued")) {
        // Not started: taken out of the queue.
        for (auto q = queue_.begin(); q != queue_.end(); ++q)
            if (q->async == id) {
                queue_.erase(q);
                break;
            }
        j.status = QStringLiteral("cancelled");
    } else if (j.status == QLatin1String("running") && runningAsync_ == id) {
        // Running: the work stops where it can, and what it did is not applied.
        worker_->cancel(QJsonValue(id));
        if (cancel_) *cancel_ = true;
    }
}

void LiveLink::settleJob(const QString& id, const QByteArray& reply) {
    AsyncJob& j = asyncJobs_[id];
    const QJsonObject r = QJsonDocument::fromJson(reply).object();
    if (r.contains("error")) {
        j.result = toolResult(r.value("error").toObject().value("message").toString(), {}, true);
    } else {
        j.result = r.value("result").toObject();
        j.result.remove("_meta");
        j.result.remove("resultType");
    }
    j.status = j.cancel ? QStringLiteral("cancelled") : j.result.value("isError").toBool() ? QStringLiteral("failed") : QStringLiteral("done");
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
        if (state_->readOnly()) {
            // Someone else is editing it: the change is not made here, and the agent is told so.
            QJsonObject r = toolResult(QStringLiteral("The project is open read-only (%1 is editing it), so %2's change was not applied")
                                           .arg(state_->lockHolder().isEmpty() ? QStringLiteral("someone else") : state_->lockHolder(), tool),
                                       {}, true);
            reply = QJsonObject{{"jsonrpc", "2.0"}, {"id", job.message.value("id")}, {"result", r}};
        } else if (!(withoutMarks(state_->project()) == withoutMarks(before))) {
            // The editor changed the project while the tool ran: theirs stands.
            QJsonObject r = toolResult(QStringLiteral("The project was changed in Montage while %1 ran, so its result was not applied. Call it again.").arg(tool), {}, true);
            reply = QJsonObject{{"jsonrpc", "2.0"}, {"id", job.message.value("id")}, {"result", r}};
        } else {
            Project read;
            std::string err;
            std::vector<std::string> files;
            std::optional<Project> merged;
            if (loadProject(path.toStdString(), read, &err)) {
                checkStereoMedia(read);
                merged = mergeToolResult(before, state_->project(), std::move(read), &files);
            }
            if (merged) {
                Project& loaded = *merged;
                // Files the tool read again itself (montage_refresh_media) are reloaded below, so what plays is what
                // they now hold.
                QStringList reread;
                for (const std::string& f : files) reread << QString::fromStdString(f);
                std::set<Id> had;
                for (const MediaItem& m : state_->project().media) had.insert(m.id);
                const auto info = tools_.find(tool);
                const QString label = !job.label.isEmpty() ? job.label
                                                           : QStringLiteral("Assistant: %1").arg(info != tools_.end() && !info->second.title.isEmpty() ? info->second.title : tool);
                state_->edit(label, [&loaded](Project& p, Sequence&) {
                    p = std::move(loaded);
                    return true;
                }, job.group);
                for (const MediaItem& m : state_->project().media)
                    if (!had.count(m.id) && m.hasAudio) state_->startAudioDecode(m);
                if (!reread.isEmpty()) state_->reloadChangedMedia(reread);
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
