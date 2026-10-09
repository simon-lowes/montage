#include "Assistant.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "LiveLink.h"
#include "Settings.h"

namespace montage {

namespace {

constexpr int kMaxResultChars = 24000;  // of one tool's answer, as the model sees it

const char* kSystem =
    "You are the editing assistant inside Montage, a professional video editor. You edit the project the editor has open, "
    "using Montage's tools. Leave out the \"project\" argument: the tools then work on the open project, and each change "
    "you make is one undo step the editor can take back. Start by calling montage_live_context (what is open, the "
    "playhead, the selected clips) and montage_project_info (tracks, clips with their ids, media) when you need them. "
    "Times are seconds or timecode (HH:MM:SS:FF). Look at a frame with montage_render_frame when the picture matters. "
    "Do what is asked, then say briefly what you did; ask when a request is ambiguous. Do not render or export files "
    "unless asked.";

QString firstLine(const QString& text) {
    const QString line = text.section('\n', 0, 0).trimmed();
    return line.size() > 200 ? line.left(197) + QStringLiteral("...") : line;
}

}  // namespace

QString AssistantConfig::effectiveEndpoint() const {
    QString e = endpoint.trimmed();
    if (e.isEmpty()) e = provider == "openai" ? QStringLiteral("http://localhost:11434") : QStringLiteral("https://api.anthropic.com");
    while (e.endsWith('/')) e.chop(1);
    return e;
}

QString AssistantConfig::effectiveModel() const {
    if (!model.trimmed().isEmpty()) return model.trimmed();
    return provider == "openai" ? QStringLiteral("llama3.1") : QStringLiteral("claude-opus-5-5");
}

QString AssistantConfig::effectiveKey() const {
    if (!apiKey.trimmed().isEmpty()) return apiKey.trimmed();
    return QString::fromLocal8Bit(qgetenv(provider == "openai" ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY"));
}

AssistantConfig AssistantSession::savedConfig() {
    QSettings s = appSettings();
    AssistantConfig c;
    c.provider = s.value(QStringLiteral("assistant/provider"), c.provider).toString();
    c.endpoint = s.value(QStringLiteral("assistant/endpoint")).toString();
    c.model = s.value(QStringLiteral("assistant/model")).toString();
    c.apiKey = s.value(QStringLiteral("assistant/apiKey")).toString();
    return c;
}

void AssistantSession::saveConfig(const AssistantConfig& c) {
    QSettings s = appSettings();
    s.setValue(QStringLiteral("assistant/provider"), c.provider);
    s.setValue(QStringLiteral("assistant/endpoint"), c.endpoint);
    s.setValue(QStringLiteral("assistant/model"), c.model);
    s.setValue(QStringLiteral("assistant/apiKey"), c.apiKey);
}

AssistantSession::AssistantSession(LiveLink* link, QObject* parent) : QObject(parent), link_(link), config_(savedConfig()) {
    net_ = new QNetworkAccessManager(this);
    // The tools as the agent link offers them: every MCP tool, on the open project.
    link_->handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", [this](QByteArray answer) {
        tools_ = QJsonDocument::fromJson(answer).object().value("result").toObject().value("tools").toArray();
    });
}

AssistantSession::~AssistantSession() {
    ++generation_;
    if (reply_) reply_->abort();
}

QString AssistantSession::title(const QString& tool) const {
    for (const QJsonValue& v : tools_)
        if (v.toObject().value("name").toString() == tool) return v.toObject().value("title").toString(tool);
    return tool;
}

void AssistantSession::send(const QString& text) {
    if (busy_ || text.trimmed().isEmpty()) return;
    messages_.append(QJsonObject{{"role", "user"}, {"content", text}});
    steps_ = 0;
    busy_ = true;
    emit busyChanged(true);
    request();
}

void AssistantSession::stop() {
    if (!busy_) return;
    ++generation_;
    if (reply_) reply_->abort();
    // Drop the half-finished exchange so the conversation stays well formed.
    while (!messages_.isEmpty()) {
        const QJsonObject last = messages_.last().toObject();
        if (last.value("role") == "user" && last.value("content").isString()) break;
        messages_.removeLast();
    }
    if (!messages_.isEmpty()) messages_.removeLast();
    done(tr("Stopped"));
}

void AssistantSession::clear() {
    stop();
    messages_ = QJsonArray();
    steps_ = 0;
}

void AssistantSession::done(const QString& error) {
    if (!busy_) return;
    busy_ = false;
    emit busyChanged(false);
    if (!error.isEmpty()) emit failed(error);
    emit finished();
}

void AssistantSession::request() {
    const bool openai = config_.provider == "openai";
    const QString key = config_.effectiveKey();
    if (!openai && key.isEmpty()) return done(tr("Add an Anthropic API key in the Assistant's settings, or set ANTHROPIC_API_KEY"));
    QJsonObject body{{"model", config_.effectiveModel()}, {"max_tokens", config_.maxTokens}};
    QJsonArray tools;
    for (const QJsonValue& v : tools_) {
        const QJsonObject t = v.toObject();
        if (openai)
            tools.append(QJsonObject{{"type", "function"},
                                     {"function", QJsonObject{{"name", t.value("name")}, {"description", t.value("description")}, {"parameters", t.value("inputSchema")}}}});
        else
            tools.append(QJsonObject{{"name", t.value("name")}, {"description", t.value("description")}, {"input_schema", t.value("inputSchema")}});
    }
    QString url = config_.effectiveEndpoint();
    if (openai) {
        QJsonArray messages{QJsonObject{{"role", "system"}, {"content", kSystem}}};
        for (const QJsonValue& m : messages_) messages.append(m);
        body["messages"] = messages;
        if (!tools.isEmpty()) body["tools"] = tools;
        url += url.endsWith("/v1") ? "/chat/completions" : "/v1/chat/completions";
    } else {
        // The tools and instructions are the same every turn: cached.
        if (!tools.isEmpty()) {
            QJsonObject last = tools.last().toObject();
            last["cache_control"] = QJsonObject{{"type", "ephemeral"}};
            tools[tools.size() - 1] = last;
            body["tools"] = tools;
        }
        body["system"] = kSystem;
        body["messages"] = messages_;
        url += "/v1/messages";
    }
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(300000);
    if (openai) {
        if (!key.isEmpty()) req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    } else {
        req.setRawHeader("x-api-key", key.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
    }
    reply_ = net_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const int generation = generation_;
    QNetworkReply* r = reply_;
    connect(r, &QNetworkReply::finished, this, [this, r, generation] {
        r->deleteLater();
        if (generation == generation_) onReply(r);
    });
}

void AssistantSession::onReply(QNetworkReply* reply) {
    const QByteArray data = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (reply->error() != QNetworkReply::NoError || status >= 400 || o.isEmpty()) {
        QString why = o.value("error").toObject().value("message").toString();
        if (why.isEmpty()) why = o.value("error").toString();
        if (why.isEmpty()) why = reply->errorString();
        return done(status ? tr("The model's service answered %1: %2").arg(status).arg(why) : why);
    }
    QList<Call> calls;
    if (config_.provider == "openai") {
        QJsonObject msg = o.value("choices").toArray().at(0).toObject().value("message").toObject();
        msg["role"] = "assistant";
        if (!msg.contains("content") || msg.value("content").isNull()) msg["content"] = QString();
        messages_.append(msg);
        if (const QString text = msg.value("content").toString().trimmed(); !text.isEmpty()) emit said(text);
        for (const QJsonValue& v : msg.value("tool_calls").toArray()) {
            const QJsonObject f = v.toObject().value("function").toObject();
            QJsonValue args = f.value("arguments");
            calls.push_back({v.toObject().value("id").toString(), f.value("name").toString(),
                             args.isString() ? QJsonDocument::fromJson(args.toString().toUtf8()).object() : args.toObject()});
        }
    } else {
        const QJsonArray content = o.value("content").toArray();
        messages_.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        for (const QJsonValue& v : content) {
            const QJsonObject b = v.toObject();
            if (b.value("type") == "text" && !b.value("text").toString().trimmed().isEmpty()) emit said(b.value("text").toString().trimmed());
            if (b.value("type") == "tool_use") calls.push_back({b.value("id").toString(), b.value("name").toString(), b.value("input").toObject()});
        }
    }
    if (calls.isEmpty()) return done();
    runCalls(calls, {});
}

void AssistantSession::runCalls(QList<Call> calls, QJsonArray results) {
    const bool openai = config_.provider == "openai";
    if (calls.isEmpty()) {
        if (openai)
            for (const QJsonValue& r : results) messages_.append(r);
        else
            messages_.append(QJsonObject{{"role", "user"}, {"content", results}});
        if (++steps_ >= config_.maxSteps) return done(tr("Stopped after %1 steps; ask again to carry on").arg(steps_));
        return request();
    }
    if (!link_) return done(tr("The agent link is gone"));
    const Call call = calls.takeFirst();
    const QString name = title(call.name);
    emit toolStarted(name, call.input);
    static int nextId = 0;
    const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", ++nextId}, {"method", "tools/call"},
                          {"params", QJsonObject{{"name", call.name}, {"arguments", call.input}}}};
    const int generation = generation_;
    link_->handle(QJsonDocument(msg).toJson(QJsonDocument::Compact), [this, call, name, calls, results, generation, openai](QByteArray answer) mutable {
        if (generation != generation_) return;
        const QJsonObject reply = QJsonDocument::fromJson(answer).object();
        const QJsonObject result = reply.value("result").toObject();
        bool isError = result.value("isError").toBool() || reply.contains("error");
        QString text;
        QJsonArray blocks;  // for Claude: the text and any pictures (a rendered frame)
        if (reply.contains("error")) text = reply.value("error").toObject().value("message").toString();
        for (const QJsonValue& v : result.value("content").toArray()) {
            const QJsonObject c = v.toObject();
            if (c.value("type") == "text") text += (text.isEmpty() ? "" : "\n") + c.value("text").toString();
            else if (c.value("type") == "image" && !openai)
                blocks.append(QJsonObject{{"type", "image"},
                                          {"source", QJsonObject{{"type", "base64"}, {"media_type", c.value("mimeType")}, {"data", c.value("data")}}}});
            else if (c.value("type") == "image") text += "\n[a picture, not shown to this model]";
        }
        const QString summary = firstLine(text);
        if (result.contains("structuredContent"))
            text += "\n" + QString::fromUtf8(QJsonDocument(result.value("structuredContent").toObject()).toJson(QJsonDocument::Compact));
        if (text.size() > kMaxResultChars) text = text.left(kMaxResultChars) + "\n[cut short]";
        if (text.isEmpty()) text = isError ? QStringLiteral("Failed") : QStringLiteral("Done");
        emit toolFinished(name, summary, isError);
        if (openai) {
            results.append(QJsonObject{{"role", "tool"}, {"tool_call_id", call.id}, {"content", text}});
        } else {
            blocks.prepend(QJsonObject{{"type", "text"}, {"text", text}});
            QJsonObject r{{"type", "tool_result"}, {"tool_use_id", call.id}, {"content", blocks}};
            if (isError) r["is_error"] = true;
            results.append(r);
        }
        runCalls(calls, results);
    });
}

}  // namespace montage
