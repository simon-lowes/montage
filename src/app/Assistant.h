// Montage — the Assistant (Premiere 26's AI Assistant panel, Avid's Gemini panel, Descript's Underlord): the editor
// asks in words ("cut the ums from the interview", "add a lower third for Jane at 10 seconds", "make a 30-second
// version") and a language model does it with Montage's MCP tools on the open project, through the agent link
// (app/LiveLink.h), so each change is an undo step "Assistant: …". Montage brings no model: it talks to Anthropic's
// Messages API (Claude) or to any OpenAI-compatible chat endpoint (OpenAI, or a local Ollama, LM Studio or llama.cpp
// server), with the key the editor gives or ANTHROPIC_API_KEY / OPENAI_API_KEY.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace montage {

class LiveLink;

struct AssistantConfig {
    QString provider = QStringLiteral("anthropic");  // "anthropic", or "openai" for OpenAI-compatible endpoints
    QString endpoint;  // "" = the provider's own (api.anthropic.com; a local Ollama for "openai")
    QString model;     // "" = the provider's default
    QString apiKey;    // "" = from the environment
    int maxTokens = 4096;
    int maxSteps = 24;  // model turns in one request before it is stopped
    QString effectiveEndpoint() const;
    QString effectiveModel() const;
    QString effectiveKey() const;
};

class AssistantSession : public QObject {
    Q_OBJECT
public:
    explicit AssistantSession(LiveLink* link, QObject* parent = nullptr);
    ~AssistantSession() override;

    static AssistantConfig savedConfig();
    static void saveConfig(const AssistantConfig& c);
    // New settings: a request in flight is stopped, and a different service starts a new conversation (each keeps it in
    // its own form).
    void setConfig(const AssistantConfig& c);
    const AssistantConfig& config() const { return config_; }

    // A request from the editor: the model works on it (calling tools as it needs) until it answers.
    void send(const QString& text);
    void stop();   // the request in flight is dropped; a tool already running finishes
    void clear();  // a new conversation
    bool busy() const { return busy_; }
    int toolCount() const { return int(tools_.size()); }

signals:
    void said(const QString& text);  // the model's words
    void toolStarted(const QString& title, const QJsonObject& input);
    void toolFinished(const QString& title, const QString& summary, bool isError);
    void failed(const QString& message);
    void finished();
    void busyChanged(bool busy);

private:
    struct Call {
        QString id, name;
        QJsonObject input;
    };
    void request();
    void onReply(QNetworkReply* reply);
    void runCalls(QList<Call> calls, QJsonArray results);
    void done(const QString& error = QString());
    QString title(const QString& tool) const;

    QPointer<LiveLink> link_;
    AssistantConfig config_;
    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QJsonArray tools_;     // as the agent link lists them
    QJsonArray messages_;  // the conversation, in the provider's own form
    bool busy_ = false;
    int steps_ = 0;
    int generation_ = 0;  // bumped by stop() and clear(), so late answers are dropped
};

}  // namespace montage
