#include "EncodeFoldersDialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "DialogJobs.h"
#include "EditorState.h"
#include "Settings.h"
#include "render/Exporter.h"

namespace montage {

// The job each encode runs as (registered with the dialogs' other tasks: DialogTasks.cpp).
void registerEncodeFolderTaskKind() {
    TaskKind k;
    k.run = [](TaskRun& r) {
        const QString output = r.input.value("output").toString();
        if (!encodeFileWithPreset(r.input.value("file").toString(), r.input.value("preset").toString(), output, &r.error, r.progress, r.cancel))
            return false;
        r.result = QJsonObject{{"output", output}};
        return true;
    };
    DialogJobs::registerKind(QStringLiteral("encode-folder"), k);
}

EncodeFolderJobs* EncodeFolderJobs::of(QWidget* window, EditorState* state) {
    if (auto* found = window->findChild<EncodeFolderJobs*>(QString(), Qt::FindDirectChildrenOnly)) return found;
    return new EncodeFolderJobs(window, state);
}

EncodeFolderJobs::EncodeFolderJobs(QWidget* window, EditorState* state) : QObject(window), window_(window), state_(state) {
    DialogJobs* jobs = DialogJobs::of(window);
    connect(jobs, &DialogJobs::taskDone, this, [this](const QString& id, const QString& kind, const QJsonObject& input, const QJsonObject& result) {
        if (kind != QLatin1String("encode-folder")) return;
        pending_.erase(id);
        // (A job taken up again after a restart is known by what it was given.)
        const QString file = input.value("file").toString();
        if (EncodeFolderScan* scan = scanFor(input.value("folder").toString())) scan->markDone(QFileInfo(file).fileName(), result.value("output").toString());
        emit encoded(file, result.value("output").toString());
    });
    connect(jobs, &DialogJobs::finished, this, [this](const QString& id, bool ok, const QString& text) {
        const auto it = pending_.find(id);
        if (ok || it == pending_.end()) return;
        if (EncodeFolderScan* scan = scanFor(it->second.folder)) scan->letGo(it->second.file);
        emit failed(QDir(it->second.folder).filePath(it->second.file), text);
        pending_.erase(it);
    });
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] { scan(); });
    reloadFolders();
}

EncodeFolderJobs::~EncodeFolderJobs() = default;

void EncodeFolderJobs::setSettleMs(qint64 ms) {
    settleMs_ = ms;
    reloadFolders();
}

void EncodeFolderJobs::reloadFolders() {
    folders_ = savedEncodeFolders();
    scans_.clear();
    for (const EncodeFolder& f : folders_) scans_.push_back(std::make_unique<EncodeFolderScan>(f, settleMs_));
    if (folders_.empty()) timer_->stop();
    else timer_->start();
}

EncodeFolderScan* EncodeFolderJobs::scanFor(const QString& folder) const {
    for (const auto& s : scans_)
        if (QDir(s->folder().input) == QDir(folder)) return s.get();
    return nullptr;
}

QString EncodeFolderJobs::titleFor(const QString& path) const { return tr("Encoding %1").arg(QDir::toNativeSeparators(path)); }

int EncodeFolderJobs::scan() {
    if (!window_) return 0;
    DialogJobs* jobs = DialogJobs::of(window_);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int began = 0;
    for (const auto& s : scans_)
        for (const QString& name : s->ready(now)) {
            const QString path = QDir(s->folder().input).filePath(name);
            // One already running for it (taken up again after a restart): left to finish, and known by its id.
            const QString title = titleFor(path);
            const QString output = s->outputFor(name);
            QString running;
            for (const DialogJobs::TaskInfo& t : jobs->tasks())
                if (t.kind == QLatin1String("encode-folder") && t.title == title) running = t.id;
            if (!running.isEmpty()) {
                pending_[running] = {s->folder().input, name, output};
                continue;
            }
            const QJsonObject input{{"file", path}, {"folder", s->folder().input}, {"preset", s->folder().preset}, {"output", output}};
            const QString id = jobs->startOn(Project(), state_, QStringLiteral("encode-folder"), title, input);
            if (id.isEmpty()) {
                s->letGo(name);
                continue;
            }
            pending_[id] = {s->folder().input, name, output};
            ++started_;
            ++began;
        }
    return began;
}

void showEncodeFoldersDialog(QWidget* window, EditorState* state) {
    QDialog dlg(window);
    dlg.setObjectName(QStringLiteral("encodeFoldersDialog"));
    dlg.setWindowTitle(EncodeFolderJobs::tr("Encode Folders"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* note = new QLabel(EncodeFolderJobs::tr("Each file that lands in one of these folders is encoded with its preset into its output folder once "
                                        "it has stopped growing, as a background job. Each file is encoded once, also after a restart."),
                            &dlg);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto* table = new QTableWidget(0, 3, &dlg);
    table->setObjectName(QStringLiteral("encodeFolderTable"));
    table->setHorizontalHeaderLabels({EncodeFolderJobs::tr("Folder"), EncodeFolderJobs::tr("Preset"), EncodeFolderJobs::tr("Output folder")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    lay->addWidget(table, 1);
    auto addRow = [&](const EncodeFolder& f) {
        const int r = table->rowCount();
        table->insertRow(r);
        auto* in = new QTableWidgetItem(QDir::toNativeSeparators(f.input));
        in->setData(Qt::UserRole, f.input);
        in->setFlags(in->flags() & ~Qt::ItemIsEditable);
        table->setItem(r, 0, in);
        auto* presets = new QComboBox(table);
        for (const ExportPreset& p : exportPresets()) presets->addItem(QString::fromStdString(p.name));
        presets->setCurrentText(f.preset);
        table->setCellWidget(r, 1, presets);
        auto* out = new QTableWidgetItem(QDir::toNativeSeparators(f.output));
        out->setData(Qt::UserRole, f.output);
        out->setFlags(out->flags() & ~Qt::ItemIsEditable);
        table->setItem(r, 2, out);
    };
    for (const EncodeFolder& f : savedEncodeFolders()) addRow(f);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(EncodeFolderJobs::tr("Add Folder…"), &dlg);
    add->setObjectName(QStringLiteral("addEncodeFolder"));
    auto* remove = new QPushButton(EncodeFolderJobs::tr("Remove"), &dlg);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    lay->addLayout(row);
    QObject::connect(add, &QPushButton::clicked, &dlg, [&] {
        const QString in = QFileDialog::getExistingDirectory(&dlg, EncodeFolderJobs::tr("Folder to Encode From"), appSettings().value(QStringLiteral("lastImportDir")).toString());
        if (in.isEmpty()) return;
        const QString out = QFileDialog::getExistingDirectory(&dlg, EncodeFolderJobs::tr("Folder to Encode Into"), in);
        if (out.isEmpty()) return;
        addRow({in, exportPresets().empty() ? QString() : QString::fromStdString(exportPresets().front().name), out});
    });
    QObject::connect(remove, &QPushButton::clicked, &dlg, [&] {
        if (const int r = table->currentRow(); r >= 0) table->removeRow(r);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.resize(760, 320);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<EncodeFolder> folders;
    for (int r = 0; r < table->rowCount(); ++r) {
        const auto* presets = qobject_cast<QComboBox*>(table->cellWidget(r, 1));
        folders.push_back({table->item(r, 0)->data(Qt::UserRole).toString(), presets ? presets->currentText() : QString(),
                           table->item(r, 2)->data(Qt::UserRole).toString()});
    }
    saveEncodeFolders(folders);
    EncodeFolderJobs::of(window, state)->reloadFolders();
}

}  // namespace montage
