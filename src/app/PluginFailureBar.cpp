#include "PluginFailureBar.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include "audio/PluginHost.h"

namespace montage {

PluginFailureBar::PluginFailureBar(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("pluginFailureBar"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, pal.color(QPalette::ToolTipBase));
    pal.setColor(QPalette::WindowText, pal.color(QPalette::ToolTipText));
    setPalette(pal);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 3, 4, 3);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("pluginFailureText"));
    text_->setWordWrap(true);
    text_->setTextFormat(Qt::PlainText);
    row->addWidget(text_, 1);
    restart_ = new QPushButton(tr("Restart"), this);
    restart_->setObjectName(QStringLiteral("pluginFailureRestart"));
    restart_->setToolTip(tr("Start the plug-in again in a new host, with its last settings"));
    disable_ = new QPushButton(tr("Disable"), this);
    disable_->setObjectName(QStringLiteral("pluginFailureDisable"));
    disable_->setToolTip(tr("Keep the plug-in bypassed and turn it off in Tools › Audio Plugins (projects keep it)"));
    close_ = new QPushButton(tr("Close"), this);
    close_->setObjectName(QStringLiteral("pluginFailureClose"));
    close_->setFlat(true);
    for (QPushButton* b : {restart_, disable_, close_}) row->addWidget(b);
    connect(restart_, &QPushButton::clicked, this, [this] {
        if (!pluginId_.empty()) plugins::restartPlugin(pluginId_);
        serial_ = ~uint64_t(0);
        refresh();
    });
    connect(disable_, &QPushButton::clicked, this, [this] {
        if (pluginId_.empty()) return;
        plugins::disablePluginAfterFailure(pluginId_);
        emit pluginsChanged();
        for (const plugins::HostFailure& f : plugins::hostFailures()) dismissedAt_ = std::max(dismissedAt_, f.lastMs);
        hide();
    });
    connect(close_, &QPushButton::clicked, this, [this] {
        for (const plugins::HostFailure& f : plugins::hostFailures()) dismissedAt_ = std::max(dismissedAt_, f.lastMs);
        hide();
    });
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &PluginFailureBar::refresh);
    timer_->start();
    hide();
}

void PluginFailureBar::refresh() {
    const uint64_t serial = plugins::hostFailureSerial();
    if (serial == serial_) return;
    serial_ = serial;
    // The latest failure not yet closed (the list comes newest first).
    const std::vector<plugins::HostFailure> all = plugins::hostFailures();
    const plugins::HostFailure* f = nullptr;
    for (const plugins::HostFailure& x : all)
        if (x.lastMs > dismissedAt_ && !x.disabled) {
            f = &x;
            break;
        }
    if (!f) {
        hide();
        return;
    }
    pluginId_ = f->pluginId;
    const QString name = QString::fromStdString(f->name);
    QString what = f->kind == "hang"     ? tr("%1 stopped responding").arg(name)
                   : f->kind == "memory" ? tr("%1 used more memory than the limit").arg(name)
                   : f->kind == "load"   ? tr("%1 crashed while loading and is blocked").arg(name)
                                         : tr("%1 crashed").arg(name);
    const QString when = QDateTime::fromMSecsSinceEpoch(f->lastMs).toString(QStringLiteral("HH:mm:ss"));
    const QString state = f->kind == "load"    ? tr("Rescan it in Tools › Audio Plugins to try it again.")
                          : f->bypassedNow > 0 ? tr("It is bypassed; playback carries on.")
                          : f->restarts > 0    ? tr("It was started again.")
                                               : QString();
    QString text = tr("%1 in its plug-in host at %2.").arg(what, when);
    if (!state.isEmpty()) text += QLatin1Char(' ') + state;
    if (f->count > 1) text += QLatin1Char(' ') + tr("(%n time(s) this session)", "", f->count);
    const int others = int(all.size()) - 1;
    if (others > 0) text += QLatin1Char(' ') + tr("%n other plug-in(s) also failed (montage_plugins lists them).", "", others);
    text_->setText(text);
    text_->setToolTip(QString::fromStdString(f->reason));
    restart_->setVisible(f->kind != "load" && f->bypassedNow > 0);
    disable_->setVisible(f->kind != "load");
    show();
}

}  // namespace montage
