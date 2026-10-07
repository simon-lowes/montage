#include "PluginManagerDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "audio/Plugins.h"

namespace montage {

using namespace plugins;

PluginManagerDialog::PluginManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Audio Plugins"));
    resize(820, 480);
    auto* lay = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Vendor"), tr("Format"), tr("Category"), tr("Status")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSortingEnabled(true);
    lay->addWidget(table_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    rescan_ = buttons->addButton(tr("Rescan"), QDialogButtonBox::ActionRole);
    rescan_->setToolTip(tr("Look for new or changed plugins"));
    retry_ = buttons->addButton(tr("Retry Blocked"), QDialogButtonBox::ActionRole);
    retry_->setToolTip(tr("Load blocked plugins again, in case they have been fixed"));
    connect(rescan_, &QPushButton::clicked, this, [this] { startScan(false); });
    connect(retry_, &QPushButton::clicked, this, [this] { startScan(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    refresh();
}

void PluginManagerDialog::refresh() {
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    auto addRow = [this](const QStringList& cells, const QString& tip, bool usable) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            item->setToolTip(tip);
            if (!usable) item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
            table_->setItem(row, c, item);
        }
    };
    const auto all = Registry::instance().plugins();
    int ready = 0;
    for (const Descriptor& d : all) {
        const bool usable = canHost(d.format) && !d.instrument;
        ready += usable ? 1 : 0;
        QString status = usable ? tr("Ready")
                         : d.instrument ? tr("Instrument (not used by the editor)")
                                        : tr("Found; %1 plugins are not supported in this version yet")
                                              .arg(QString::fromLatin1(formatName(d.format)));
        addRow({QString::fromStdString(d.name), QString::fromStdString(d.vendor), QString::fromLatin1(formatName(d.format)),
                QString::fromStdString(d.category), status},
               QString::fromStdString(d.path), usable);
    }
    const auto blocked = Registry::instance().blocklist();
    for (const Blocked& b : blocked)
        addRow({QFileInfo(QString::fromStdString(b.path)).completeBaseName(), QString(), QString::fromLatin1(formatName(b.format)),
                QString(), tr("Blocked: %1").arg(QString::fromStdString(b.reason))},
               QString::fromStdString(b.path), false);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    if (!scanning_)
        summary_->setText(tr("%n plugin(s) found, %1 ready to use, %2 blocked. Plugins are loaded in a separate "
                             "process while scanning, so one that crashes is blocked instead of closing Montage.",
                             "", int(all.size()))
                              .arg(ready)
                              .arg(blocked.size()));
    retry_->setEnabled(!scanning_ && !blocked.empty());
}

void PluginManagerDialog::setScanning(bool on) {
    scanning_ = on;
    rescan_->setEnabled(!on);
    retry_->setEnabled(!on);
}

void PluginManagerDialog::startScan(bool retryBlocked) {
    if (scanning_) return;
    setScanning(true);
    summary_->setText(tr("Scanning…"));
    QPointer<QLabel> label = summary_;
    auto* watcher = new QFutureWatcher<ScanReport>(this);
    connect(watcher, &QFutureWatcher<ScanReport>::finished, this, [this, watcher] {
        watcher->deleteLater();
        setScanning(false);
        refresh();
        emit pluginsChanged();
    });
    watcher->setFuture(QtConcurrent::run([retryBlocked, label] {
        return Registry::instance().scan(retryBlocked, [label](int done, int total, const std::string& path) {
            const QString text = path.empty() ? QString()
                                              : PluginManagerDialog::tr("Scanning %1 of %2: %3")
                                                    .arg(done + 1)
                                                    .arg(total)
                                                    .arg(QFileInfo(QString::fromStdString(path)).fileName());
            QMetaObject::invokeMethod(
                qApp, [label, text] { if (label && !text.isEmpty()) label->setText(text); }, Qt::QueuedConnection);
        });
    }));
}

}  // namespace montage
