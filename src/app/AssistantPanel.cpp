#include "AssistantPanel.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "Assistant.h"
#include "LiveLink.h"

namespace montage {

AssistantPanel::AssistantPanel(LiveLink* link, QWidget* parent) : QWidget(parent), session_(new AssistantSession(link, this)) {
    setObjectName(QStringLiteral("assistantPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    header_ = new QLabel(this);
    header_->setObjectName(QStringLiteral("assistantHeader"));
    auto* settings = new QPushButton(tr("Settings…"), this);
    settings->setObjectName(QStringLiteral("assistantSettings"));
    auto* fresh = new QPushButton(tr("New Chat"), this);
    fresh->setObjectName(QStringLiteral("assistantNewChat"));
    top->addWidget(header_, 1);
    top->addWidget(fresh);
    top->addWidget(settings);
    lay->addLayout(top);
    log_ = new QTextBrowser(this);
    log_->setObjectName(QStringLiteral("assistantLog"));
    log_->setOpenExternalLinks(false);
    log_->setPlaceholderText(tr("Ask for an edit in your own words: \"remove the ums from the interview\", \"put a lower third for "
                                "Jane Doe, Director, at 10 seconds\", \"make a 30-second cut for social\". Every change is one "
                                "undo step."));
    lay->addWidget(log_, 1);
    auto* bottom = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("assistantInput"));
    input_->setPlaceholderText(tr("Ask the Assistant…"));
    send_ = new QPushButton(tr("Send"), this);
    send_->setObjectName(QStringLiteral("assistantSend"));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("assistantStop"));
    stop_->setEnabled(false);
    bottom->addWidget(input_, 1);
    bottom->addWidget(send_);
    bottom->addWidget(stop_);
    lay->addLayout(bottom);

    auto submit = [this] {
        const QString text = input_->text().trimmed();
        if (text.isEmpty() || session_->busy()) return;
        input_->clear();
        ask(text);
    };
    connect(send_, &QPushButton::clicked, this, submit);
    connect(input_, &QLineEdit::returnPressed, this, submit);
    connect(stop_, &QPushButton::clicked, session_, &AssistantSession::stop);
    connect(fresh, &QPushButton::clicked, this, [this] {
        session_->clear();
        log_->clear();
    });
    connect(settings, &QPushButton::clicked, this, [this] { settingsDialog(); });
    connect(session_, &AssistantSession::said, this, [this](const QString& text) { append(tr("Assistant"), text.toHtmlEscaped().replace('\n', "<br>"), {}); });
    connect(session_, &AssistantSession::toolStarted, this, [this](const QString& title, const QJsonObject&) {
        append({}, QStringLiteral("⚙ %1…").arg(title.toHtmlEscaped()), QStringLiteral("#8a8f98"));
    });
    connect(session_, &AssistantSession::toolFinished, this, [this](const QString& title, const QString& summary, bool isError) {
        append({}, QStringLiteral("%1 %2: %3").arg(isError ? "✗" : "✓", title.toHtmlEscaped(), summary.toHtmlEscaped()),
               isError ? QStringLiteral("#d26a6a") : QStringLiteral("#8a8f98"));
    });
    connect(session_, &AssistantSession::failed, this, [this](const QString& why) { append({}, why.toHtmlEscaped(), QStringLiteral("#d26a6a")); });
    connect(session_, &AssistantSession::busyChanged, this, [this](bool busy) {
        send_->setEnabled(!busy);
        stop_->setEnabled(busy);
    });
    refreshHeader();
}

void AssistantPanel::ask(const QString& text) {
    append(tr("You"), text.toHtmlEscaped(), {});
    session_->send(text);
}

QString AssistantPanel::transcriptText() const { return log_->toPlainText(); }

void AssistantPanel::append(const QString& who, const QString& html, const QString& colour) {
    QString line = who.isEmpty() ? html : QStringLiteral("<b>%1:</b> %2").arg(who.toHtmlEscaped(), html);
    if (!colour.isEmpty()) line = QStringLiteral("<span style=\"color:%1\">%2</span>").arg(colour, line);
    log_->append(line);
}

void AssistantPanel::refreshHeader() {
    const AssistantConfig& c = session_->config();
    QString text = c.provider == "openai" ? tr("%1 at %2").arg(c.effectiveModel(), c.effectiveEndpoint()) : c.effectiveModel();
    if (c.provider != "openai" && c.effectiveKey().isEmpty()) text += tr(" (no API key: open Settings)");
    header_->setText(text);
}

bool AssistantPanel::settingsDialog() {
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("assistantSettingsDialog"));
    dlg.setWindowTitle(tr("Assistant Settings"));
    auto* form = new QFormLayout(&dlg);
    AssistantConfig c = session_->config();
    auto* provider = new QComboBox(&dlg);
    provider->setObjectName(QStringLiteral("assistantProvider"));
    provider->addItem(tr("Claude (Anthropic API)"), QStringLiteral("anthropic"));
    provider->addItem(tr("OpenAI-compatible (OpenAI, Ollama, LM Studio, llama.cpp)"), QStringLiteral("openai"));
    provider->setCurrentIndex(c.provider == "openai" ? 1 : 0);
    auto* endpoint = new QLineEdit(c.endpoint, &dlg);
    endpoint->setObjectName(QStringLiteral("assistantEndpoint"));
    auto* model = new QLineEdit(c.model, &dlg);
    model->setObjectName(QStringLiteral("assistantModel"));
    auto* key = new QLineEdit(c.apiKey, &dlg);
    key->setObjectName(QStringLiteral("assistantKey"));
    key->setEchoMode(QLineEdit::Password);
    auto hints = [&] {
        AssistantConfig probe;
        probe.provider = provider->currentData().toString();
        endpoint->setPlaceholderText(probe.effectiveEndpoint());
        model->setPlaceholderText(probe.effectiveModel());
        key->setPlaceholderText(probe.provider == "openai" ? tr("OPENAI_API_KEY, or none for a local server") : tr("ANTHROPIC_API_KEY"));
    };
    hints();
    connect(provider, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, hints);
    form->addRow(tr("Service:"), provider);
    form->addRow(tr("Endpoint:"), endpoint);
    form->addRow(tr("Model:"), model);
    form->addRow(tr("API key:"), key);
    auto* note = new QLabel(tr("The key is kept in Montage's settings on this computer. Requests go only to the endpoint above."), &dlg);
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return false;
    const bool newService = c.provider != provider->currentData().toString();
    c.provider = provider->currentData().toString();
    c.endpoint = endpoint->text().trimmed();
    c.model = model->text().trimmed();
    c.apiKey = key->text().trimmed();
    session_->setConfig(c);
    if (newService) {
        log_->clear();
        append({}, tr("New conversation with %1").arg(provider->currentText().toHtmlEscaped()), QStringLiteral("#8a8f98"));
    }
    AssistantSession::saveConfig(c);
    refreshHeader();
    return true;
}

}  // namespace montage
