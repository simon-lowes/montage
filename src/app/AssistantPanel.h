// Montage — the Assistant panel (Window › Assistant): a conversation with a language model that edits the open
// project with Montage's tools (app/Assistant.h). Each tool it runs is listed as it goes, every change is an undo
// step, and Stop drops a request mid-way. Settings choose Claude (Anthropic's API) or an OpenAI-compatible endpoint
// (OpenAI, or a local Ollama, LM Studio or llama.cpp server), the model and the key.
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace montage {

class AssistantSession;
class LiveLink;

class AssistantPanel : public QWidget {
    Q_OBJECT
public:
    AssistantPanel(LiveLink* link, QWidget* parent = nullptr);
    AssistantSession* session() const { return session_; }
    void ask(const QString& text);  // as if typed and sent
    QString transcriptText() const;
    bool settingsDialog();          // true when changed

private:
    void append(const QString& who, const QString& html, const QString& colour);
    void refreshHeader();

    AssistantSession* session_;
    QLabel* header_ = nullptr;
    QTextBrowser* log_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* stop_ = nullptr;
};

}  // namespace montage
