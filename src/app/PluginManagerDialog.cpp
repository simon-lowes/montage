#include "PluginManagerDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <set>

namespace montage {

using namespace plugins;

namespace {
constexpr int kIdRole = Qt::UserRole;        // plugin id (enable switch)
constexpr int kPathRole = Qt::UserRole + 1;  // plugin file (rescan selected)

QString settingsKey(Format f) { return QStringLiteral("plugins/extraFolders/%1").arg(QString::fromLatin1(formatName(f))); }
}  // namespace

void PluginManagerDialog::applySavedFolders() {
    QSettings s;
    for (Format f : kAllFormats) {
        std::vector<std::string> dirs;
        for (const QString& d : s.value(settingsKey(f)).toStringList()) dirs.push_back(d.toStdString());
        Registry::instance().setExtraSearchPaths(f, std::move(dirs));
    }
}

PluginManagerDialog::PluginManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Audio Plugins"));
    resize(900, 560);
    auto* lay = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({tr("On"), tr("Name"), tr("Vendor"), tr("Format"), tr("Category"), tr("Status")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSortingEnabled(true);
    connect(table_, &QTableWidget::itemChanged, this, &PluginManagerDialog::itemChanged);
    lay->addWidget(table_, 3);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setPlaceholderText(tr("The scan log appears here after a scan."));
    log_->hide();
    lay->addWidget(log_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    rescan_ = buttons->addButton(tr("Rescan"), QDialogButtonBox::ActionRole);
    rescan_->setToolTip(tr("Look for new or changed plugins"));
    rescanSelected_ = buttons->addButton(tr("Rescan Selected"), QDialogButtonBox::ActionRole);
    rescanSelected_->setToolTip(tr("Load the selected plugins again, even if unchanged or blocked"));
    retry_ = buttons->addButton(tr("Retry Blocked"), QDialogButtonBox::ActionRole);
    retry_->setToolTip(tr("Load blocked plugins again, in case they have been fixed"));
    folders_ = buttons->addButton(tr("Folders…"), QDialogButtonBox::ActionRole);
    folders_->setToolTip(tr("Add folders to search for plugins"));
    auto* showLog = buttons->addButton(tr("Show Log"), QDialogButtonBox::ActionRole);
    showLog->setCheckable(true);
    connect(showLog, &QPushButton::toggled, log_, &QWidget::setVisible);
    connect(rescan_, &QPushButton::clicked, this, [this] { startScan([] { return Registry::instance().scan(false); }); });
    connect(retry_, &QPushButton::clicked, this, [this] { startScan([] { return Registry::instance().scan(true); }); });
    connect(rescanSelected_, &QPushButton::clicked, this, [this] {
        std::set<std::string> paths;
        for (QTableWidgetItem* item : table_->selectedItems())
            if (QTableWidgetItem* name = table_->item(item->row(), 1)) paths.insert(name->data(kPathRole).toString().toStdString());
        paths.erase(std::string());
        if (paths.empty()) return;
        std::vector<std::string> list(paths.begin(), paths.end());
        startScan([list] { return Registry::instance().rescan(list); });
    });
    connect(folders_, &QPushButton::clicked, this, &PluginManagerDialog::editFolders);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    refresh();
}

void PluginManagerDialog::refresh() {
    refreshing_ = true;
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    auto addRow = [this](bool switchable, bool on, const QString& id, const QStringList& cells, const QString& path, bool usable) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* sw = new QTableWidgetItem;
        if (switchable) {
            sw->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            sw->setCheckState(on ? Qt::Checked : Qt::Unchecked);
            sw->setData(kIdRole, id);
            sw->setToolTip(tr("Offer this plugin in the Effects browser and menus"));
        } else {
            sw->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        table_->setItem(row, 0, sw);
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            item->setToolTip(path);
            if (!usable) item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
            if (c == 0) item->setData(kPathRole, path);
            table_->setItem(row, c + 1, item);
        }
    };
    const auto all = Registry::instance().plugins();
    int ready = 0;
    for (const Descriptor& d : all) {
        const bool hostable = canHost(d.format) && !d.instrument;
        const bool on = !Registry::instance().isPluginDisabled(d.id);
        ready += hostable && on ? 1 : 0;
        QString status = !hostable ? (d.instrument ? tr("Instrument (not used by the editor)")
                                                   : tr("Found; %1 plugins are not supported in this version yet")
                                                         .arg(QString::fromLatin1(formatName(d.format))))
                         : on ? tr("Ready")
                              : tr("Turned off");
        addRow(hostable, on, QString::fromStdString(d.id),
               {QString::fromStdString(d.name), QString::fromStdString(d.vendor), QString::fromLatin1(formatName(d.format)),
                QString::fromStdString(d.category), status},
               QString::fromStdString(d.path), hostable && on);
    }
    const auto blocked = Registry::instance().blocklist();
    for (const Blocked& b : blocked)
        addRow(false, false, QString(),
               {QFileInfo(QString::fromStdString(b.path)).completeBaseName(), QString(), QString::fromLatin1(formatName(b.format)),
                QString(), tr("Blocked: %1").arg(QString::fromStdString(b.reason))},
               QString::fromStdString(b.path), false);
    table_->setSortingEnabled(true);
    table_->sortByColumn(1, Qt::AscendingOrder);
    if (!scanning_)
        summary_->setText(tr("%n plugin(s) found, %1 ready to use, %2 blocked. Plugins are loaded in a separate "
                             "process while scanning, so one that crashes is blocked instead of closing Montage.",
                             "", int(all.size()))
                              .arg(ready)
                              .arg(blocked.size()));
    retry_->setEnabled(!scanning_ && !blocked.empty());
    refreshing_ = false;
}

void PluginManagerDialog::itemChanged(QTableWidgetItem* item) {
    if (refreshing_ || item->column() != 0) return;
    const QString id = item->data(kIdRole).toString();
    if (id.isEmpty()) return;
    Registry::instance().setPluginDisabled(id.toStdString(), item->checkState() != Qt::Checked);
    QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    emit pluginsChanged();
}

void PluginManagerDialog::setScanning(bool on) {
    scanning_ = on;
    for (QPushButton* b : {rescan_, rescanSelected_, retry_, folders_}) b->setEnabled(!on);
}

void PluginManagerDialog::startScan(const std::function<ScanReport()>& job) {
    if (scanning_) return;
    setScanning(true);
    summary_->setText(tr("Scanning…"));
    auto* watcher = new QFutureWatcher<ScanReport>(this);
    connect(watcher, &QFutureWatcher<ScanReport>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const ScanReport rep = watcher->result();
        QStringList lines;
        for (const std::string& l : rep.log) lines << QString::fromStdString(l);
        log_->setPlainText(tr("%1 files: %2 from the cache, %3 loaded in the probe, %4 newly blocked")
                               .arg(rep.files)
                               .arg(rep.fromCache)
                               .arg(rep.probed)
                               .arg(rep.newlyBlocked.size()) +
                           "\n\n" + lines.join('\n'));
        setScanning(false);
        refresh();
        emit pluginsChanged();
    });
    watcher->setFuture(QtConcurrent::run(job));
}

void PluginManagerDialog::editFolders() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Plugin Folders"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* help = new QLabel(tr("Montage always searches the standard folders for each format. Add other folders here."), &dlg);
    help->setWordWrap(true);
    lay->addWidget(help);
    auto* format = new QComboBox(&dlg);
    for (Format f : kAllFormats)
        if (f != Format::AudioUnit) format->addItem(QString::fromLatin1(formatName(f)), int(f));
    lay->addWidget(format);
    auto* list = new QListWidget(&dlg);
    lay->addWidget(list);
    auto* defaults = new QLabel(&dlg);
    defaults->setWordWrap(true);
    defaults->setStyleSheet(QStringLiteral("color: palette(mid);"));
    lay->addWidget(defaults);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Folder…"), &dlg);
    auto* remove = new QPushButton(tr("Remove"), &dlg);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    lay->addLayout(row);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(box);

    QSettings settings;
    std::map<int, QStringList> folders;
    for (Format f : kAllFormats) folders[int(f)] = settings.value(settingsKey(f)).toStringList();
    auto show = [&] {
        const Format f = Format(format->currentData().toInt());
        list->clear();
        list->addItems(folders[int(f)]);
        QStringList std;
        for (const std::string& d : defaultSearchPaths(f)) std << QString::fromStdString(d);
        defaults->setText(tr("Standard folders: %1").arg(std.join(QStringLiteral(", "))));
    };
    connect(format, &QComboBox::currentIndexChanged, &dlg, show);
    connect(add, &QPushButton::clicked, &dlg, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&dlg, tr("Add Plugin Folder"));
        if (dir.isEmpty()) return;
        QStringList& l = folders[format->currentData().toInt()];
        if (!l.contains(dir)) l << dir;
        show();
    });
    connect(remove, &QPushButton::clicked, &dlg, [&] {
        if (QListWidgetItem* item = list->currentItem()) folders[format->currentData().toInt()].removeAll(item->text());
        show();
    });
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    show();
    if (dlg.exec() != QDialog::Accepted) return;
    for (const auto& [f, l] : folders) settings.setValue(settingsKey(Format(f)), l);
    applySavedFolders();
    startScan([] { return Registry::instance().scan(false); });
}

}  // namespace montage
