#include "PluginManagerDialog.h"
#include "Settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QSpinBox>
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

#include "audio/PluginHost.h"

namespace montage {

using namespace plugins;

namespace {
constexpr int kIdRole = Qt::UserRole;        // plugin id (enable switch)
constexpr int kPathRole = Qt::UserRole + 1;  // plugin file (rescan selected)

QString settingsKey(Format f) { return QStringLiteral("plugins/extraFolders/%1").arg(QString::fromLatin1(formatName(f))); }
}  // namespace

void PluginManagerDialog::applySavedFolders() {
    QSettings s = appSettings();
    for (Format f : kAllFormats) {
        std::vector<std::string> dirs;
        for (const QString& d : s.value(settingsKey(f)).toStringList()) dirs.push_back(d.toStdString());
        Registry::instance().setExtraSearchPaths(f, std::move(dirs));
    }
}

void PluginManagerDialog::applySavedHosting() {
    QSettings s = appSettings();
    HostingSettings h;
    if (const auto i = isolationFromName(s.value(QStringLiteral("plugins/hosting/isolation")).toString().toStdString())) h.isolation = *i;
    h.missedBlocks = std::clamp(s.value(QStringLiteral("plugins/hosting/missedBlocks"), h.missedBlocks).toInt(), 1, 1000);
    h.bypassWithSilence = s.value(QStringLiteral("plugins/hosting/bypassWithSilence"), h.bypassWithSilence).toBool();
    h.exportTimeoutMs = std::max(1000, s.value(QStringLiteral("plugins/hosting/exportTimeoutMs"), h.exportTimeoutMs).toInt());
    h.memoryLimitMb = std::max(0, s.value(QStringLiteral("plugins/hosting/memoryLimitMb"), h.memoryLimitMb).toInt());
    h.autoRestarts = std::clamp(s.value(QStringLiteral("plugins/hosting/autoRestarts"), h.autoRestarts).toInt(), 0, 100);
    setHosting(h);
    onHostingChanged([](const HostingSettings& n) {
        QSettings out = appSettings();
        out.setValue(QStringLiteral("plugins/hosting/isolation"), QString::fromLatin1(isolationName(n.isolation)));
        out.setValue(QStringLiteral("plugins/hosting/missedBlocks"), n.missedBlocks);
        out.setValue(QStringLiteral("plugins/hosting/bypassWithSilence"), n.bypassWithSilence);
        out.setValue(QStringLiteral("plugins/hosting/exportTimeoutMs"), n.exportTimeoutMs);
        out.setValue(QStringLiteral("plugins/hosting/memoryLimitMb"), n.memoryLimitMb);
        out.setValue(QStringLiteral("plugins/hosting/autoRestarts"), n.autoRestarts);
    });
}

void PluginManagerDialog::editHosting(QWidget* parent) {
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("pluginHostingDialog"));
    dlg.setWindowTitle(tr("Plug-in Hosting"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* help = new QLabel(tr("Plug-ins run in plug-in host processes of their own, so one that crashes, stops responding "
                               "or uses too much memory is bypassed and started again instead of closing Montage. "
                               "ARA plug-ins always run in Montage's own process."),
                            &dlg);
    help->setWordWrap(true);
    lay->addWidget(help);
    const HostingSettings h = hosting();
    auto* form = new QFormLayout;
    auto* isolation = new QComboBox(&dlg);
    isolation->setObjectName(QStringLiteral("hostingIsolation"));
    isolation->addItem(tr("Each plug-in in a host of its own (recommended)"), int(Isolation::PerPlugin));
    isolation->addItem(tr("Each instance in a host of its own"), int(Isolation::PerInstance));
    isolation->addItem(tr("One host for each maker"), int(Isolation::PerVendor));
    isolation->addItem(tr("All plug-ins in one host"), int(Isolation::Shared));
    isolation->addItem(tr("In Montage's own process (off)"), int(Isolation::InProcess));
    isolation->setCurrentIndex(std::max(0, isolation->findData(int(h.isolation))));
    isolation->setToolTip(tr("With a host for each plug-in, its instances share it; with a host for each instance, a fault "
                             "in one instance reaches no other, at the cost of more memory. Fewer hosts use less memory; a "
                             "fault then reaches every plug-in in the same host, but one that crashes while loading is "
                             "blocked alone."));
    form->addRow(tr("Run plug-ins:"), isolation);
    auto* missed = new QSpinBox(&dlg);
    missed->setObjectName(QStringLiteral("hostingMissedBlocks"));
    missed->setRange(1, 1000);
    missed->setValue(h.missedBlocks);
    missed->setSuffix(tr(" audio blocks"));
    missed->setToolTip(tr("During playback, a plug-in that is late with this many blocks in a row is stopped and bypassed "
                          "(each late block passes meanwhile)."));
    form->addRow(tr("Stopped responding after:"), missed);
    auto* bypass = new QComboBox(&dlg);
    bypass->setObjectName(QStringLiteral("hostingBypass"));
    bypass->addItem(tr("Its input (delayed by its latency)"), false);
    bypass->addItem(tr("Silence"), true);
    bypass->setCurrentIndex(h.bypassWithSilence ? 1 : 0);
    form->addRow(tr("A bypassed plug-in gives:"), bypass);
    auto* memory = new QSpinBox(&dlg);
    memory->setObjectName(QStringLiteral("hostingMemoryLimit"));
    memory->setRange(0, 1 << 20);
    memory->setSingleStep(512);
    memory->setValue(h.memoryLimitMb);
    memory->setSuffix(tr(" MB"));
    memory->setSpecialValueText(tr("No limit"));
    form->addRow(tr("Stop a host using more than:"), memory);
    auto* wait = new QSpinBox(&dlg);
    wait->setObjectName(QStringLiteral("hostingExportTimeout"));
    wait->setRange(1, 3600);
    wait->setValue(std::max(1, h.exportTimeoutMs / 1000));
    wait->setSuffix(tr(" s"));
    wait->setToolTip(tr("An export waits this long for a slow plug-in, then stops and names it."));
    form->addRow(tr("An export waits up to:"), wait);
    auto* restarts = new QSpinBox(&dlg);
    restarts->setObjectName(QStringLiteral("hostingAutoRestarts"));
    restarts->setRange(0, 100);
    restarts->setValue(h.autoRestarts);
    restarts->setToolTip(tr("Times a session each plug-in is started again by itself after failing; after that it stays "
                            "bypassed until you choose Restart."));
    form->addRow(tr("Start a failed plug-in again:"), restarts);
    lay->addLayout(form);
    if (!hostAvailable()) {
        auto* missing = new QLabel(tr("The plug-in host program is missing from this installation: plug-ins run in "
                                      "Montage's own process."),
                                   &dlg);
        missing->setWordWrap(true);
        lay->addWidget(missing);
    }
    auto* note = new QLabel(tr("Changes apply to plug-ins loaded from now on."), &dlg);
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
    lay->addWidget(note);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    HostingSettings n = h;
    n.isolation = Isolation(isolation->currentData().toInt());
    n.missedBlocks = missed->value();
    n.bypassWithSilence = bypass->currentData().toBool();
    n.memoryLimitMb = memory->value();
    n.exportTimeoutMs = wait->value() * 1000;
    n.autoRestarts = restarts->value();
    setHostingAndSave(n);
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
    hosting_ = buttons->addButton(tr("Hosting…"), QDialogButtonBox::ActionRole);
    hosting_->setObjectName(QStringLiteral("pluginHosting"));
    hosting_->setToolTip(tr("How plug-ins run in host processes of their own, and what happens when one fails"));
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
    connect(hosting_, &QPushButton::clicked, this, [this] { editHosting(this); });
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
                             "process while scanning, so one that crashes is blocked instead of closing Montage, and "
                             "run in plug-in hosts of their own while you work (Hosting…).",
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

    QSettings settings = appSettings();
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
