#include "VfxShotsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Settings.h"
#include "render/VfxShots.h"

namespace montage {

namespace {
QString statusText(int status) {
    switch (status) {
    case kVfxForReview: return VfxShotsDialog::tr("For Review");
    case kVfxApproved: return VfxShotsDialog::tr("Approved");
    case kVfxFinal: return VfxShotsDialog::tr("Final");
    default: return VfxShotsDialog::tr("In Progress");
    }
}
QColor statusColour(int status) {
    switch (status) {
    case kVfxForReview: return QColor(214, 160, 40);
    case kVfxApproved: return QColor(70, 170, 90);
    case kVfxFinal: return QColor(70, 130, 210);
    default: return {};
    }
}
}  // namespace

VfxShotsDialog::VfxShotsDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("VFX Shots"));
    setObjectName(QStringLiteral("vfxShotsDialog"));
    resize(820, 440);
    auto* root = new QVBoxLayout(this);
    // Where the versions come back, and how they are named.
    auto* where = new QHBoxLayout;
    returns_ = new QLabel(this);
    returns_->setObjectName(QStringLiteral("vfxReturnsFolder"));
    returns_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* change = new QPushButton(tr("Returns Folder…"), this);
    change->setObjectName(QStringLiteral("vfxChooseReturns"));
    pattern_ = new QLineEdit(this);
    pattern_->setObjectName(QStringLiteral("vfxReturnsPattern"));
    pattern_->setAccessibleName(tr("How versions are named"));
    pattern_->setPlaceholderText(QStringLiteral("{shot}_comp_v###"));
    pattern_->setToolTip(tr("How the vendor names a version: {shot} for the shot's name, #'s for the version's number"));
    where->addWidget(returns_, 1);
    where->addWidget(change);
    where->addWidget(new QLabel(tr("Named:"), this));
    where->addWidget(pattern_);
    root->addLayout(where);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("vfxShotsList"));
    list_->setAccessibleName(tr("VFX shots"));
    list_->setRootIsDecorated(false);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setHeaderLabels({tr("Shot"), tr("Status"), tr("Shows"), tr("Versions"), tr("Note"), tr("Problems")});
    list_->header()->setStretchLastSection(true);
    root->addWidget(list_, 1);
    // Versions.
    auto* versions = new QHBoxLayout;
    auto button = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    auto* scan = button(tr("Look for New Versions"), "vfxScan");
    auto* plate = button(tr("Plate"), "vfxPlate");
    auto* down = button(tr("Previous Version"), "vfxVersionDown");
    auto* up = button(tr("Next Version"), "vfxVersionUp");
    auto* latest = button(tr("Latest"), "vfxLatest");
    for (QPushButton* b : {scan, plate, down, up, latest}) versions->addWidget(b);
    versions->addStretch(1);
    root->addLayout(versions);
    // Status and note.
    auto* review = new QHBoxLayout;
    status_ = new QComboBox(this);
    status_->setObjectName(QStringLiteral("vfxStatus"));
    status_->setAccessibleName(tr("Status"));
    for (int k = kVfxInProgress; k <= kVfxFinal; ++k) status_->addItem(statusText(k), k);
    note_ = new QLineEdit(this);
    note_->setObjectName(QStringLiteral("vfxNote"));
    note_->setAccessibleName(tr("Note"));
    note_->setPlaceholderText(tr("Note"));
    auto* apply = button(tr("Set"), "vfxSetStatus");
    review->addWidget(new QLabel(tr("Status:"), this));
    review->addWidget(status_);
    review->addWidget(note_, 1);
    review->addWidget(apply);
    root->addLayout(review);
    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("vfxMessage"));
    message_->setWordWrap(true);
    root->addWidget(message_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* exportButton = buttons->addButton(tr("Export Shot List…"), QDialogButtonBox::ActionRole);
    exportButton->setObjectName(QStringLiteral("vfxExportList"));
    auto* scripts = buttons->addButton(tr("Write Compositing Scripts"), QDialogButtonBox::ActionRole);
    scripts->setObjectName(QStringLiteral("vfxWriteScripts"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(change, &QPushButton::clicked, this, [this] {
        const Sequence* s = state_->sequence();
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Returns Folder"), s ? QString::fromStdString(s->vfxReturns) : QString());
        if (!folder.isEmpty()) setReturnsFolder(folder);
    });
    connect(pattern_, &QLineEdit::editingFinished, this, [this] {
        const std::string pattern = pattern_->text().trimmed().toStdString();
        const Sequence* s = state_->sequence();
        if (!s || s->vfxPattern == pattern) return;
        state_->edit(tr("VFX Version Names"), [&](Project&, Sequence& seq) {
            seq.vfxPattern = pattern;
            return true;
        });
    });
    connect(scan, &QPushButton::clicked, this, [this] {
        const Sequence* s = state_->sequence();
        if (!s || s->vfxReturns.empty()) {
            message_->setText(tr("Choose the folder the versions come back to first"));
            return;
        }
        std::vector<std::string> problems;
        const std::vector<std::string> added = state_->scanVfxReturnsNow(&problems);
        QStringList names;
        for (const std::string& n : added) names << QString::fromStdString(n);
        message_->setText(added.empty() ? tr("No new versions") : tr("New: %1").arg(names.join(QStringLiteral(", "))));
    });
    connect(plate, &QPushButton::clicked, this, [this] { showVersion(0); });
    connect(down, &QPushButton::clicked, this, [this] { showVersion(0, -1); });
    connect(up, &QPushButton::clicked, this, [this] { showVersion(0, 1); });
    connect(latest, &QPushButton::clicked, this, [this] { showVersion(-1); });
    connect(apply, &QPushButton::clicked, this, [this] {
        const QString note = note_->text();
        setStatus(status_->currentData().toInt(), &note);
    });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, [this] {
        // The picked shot's status and note to change, and its clip picked on the timeline.
        const QList<QTreeWidgetItem*> items = list_->selectedItems();
        const Sequence* s = state_->sequence();
        if (items.isEmpty() || !s) return;
        std::vector<Id> clips;
        for (QTreeWidgetItem* it : items)
            if (const VfxShotRecord* r = findVfxShot(*s, it->text(0).toStdString()); r && edit::clipById(*s, r->clip))
                clips.push_back(r->clip);
        if (const VfxShotRecord* r = findVfxShot(*s, items.front()->text(0).toStdString())) {
            status_->setCurrentIndex(std::max(0, status_->findData(r->status)));
            note_->setText(QString::fromStdString(r->note));
        }
        if (!clips.empty()) state_->setSelection(clips);
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        const Sequence* s = state_->sequence();
        if (!s) return;
        if (const VfxShotRecord* r = findVfxShot(*s, it->text(0).toStdString()))
            if (const Clip* c = edit::clipById(*s, r->clip)) state_->setPlayhead(c->start);
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        QString filter;
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Shot List"), appSettings().value("vfx/listFolder").toString(),
                                                          tr("CSV (*.csv);;OpenTimelineIO (*.otio)"), &filter);
        if (path.isEmpty()) return;
        appSettings().setValue("vfx/listFolder", QFileInfo(path).absolutePath());
        QString err;
        message_->setText(exportList(path, &err) ? tr("Wrote %1").arg(QDir::toNativeSeparators(path)) : err);
    });
    connect(scripts, &QPushButton::clicked, this, [this] {
        QString err;
        if (!writeScripts(&err)) message_->setText(err);
    });
    connect(state_, &EditorState::projectChanged, this, &VfxShotsDialog::refresh);
    refresh();
}

void VfxShotsDialog::refresh() {
    const Sequence* s = state_->sequence();
    returns_->setText(s && !s->vfxReturns.empty() ? tr("Returns: %1").arg(QDir::toNativeSeparators(QString::fromStdString(s->vfxReturns)))
                                                  : tr("No returns folder"));
    if (!pattern_->hasFocus()) pattern_->setText(s ? QString::fromStdString(s->vfxPattern) : QString());
    QStringList picked;
    for (QTreeWidgetItem* it : list_->selectedItems()) picked << it->text(0);
    const QSignalBlocker block(list_);
    list_->clear();
    if (!s) return;
    for (const VfxShotRecord& r : s->vfxShots) {
        QString versions, problems;
        const int shown = vfxShownVersion(*s, r);
        for (const VfxVersion& v : r.versions) {
            versions += (versions.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("v%1").arg(v.number);
            if (v.number == shown)
                for (const std::string& pr : v.problems) problems += (problems.isEmpty() ? QString() : QStringLiteral("; ")) + QString::fromStdString(pr);
        }
        if (!edit::clipById(*s, r.clip)) problems = tr("Its clip is no longer in the cut");
        auto* it = new QTreeWidgetItem(list_, {QString::fromStdString(r.name), statusText(r.status),
                                               shown ? QStringLiteral("v%1").arg(shown) : tr("Plate"), versions,
                                               QString::fromStdString(r.note), problems});
        if (const QColor c = statusColour(r.status); c.isValid()) it->setForeground(1, c);
        if (!problems.isEmpty()) it->setForeground(5, QColor(210, 80, 70));
        it->setToolTip(5, problems);
        it->setSelected(picked.contains(it->text(0)));
    }
    for (int k = 0; k < 4; ++k) list_->resizeColumnToContents(k);
}

std::vector<std::string> VfxShotsDialog::chosenShots() const {
    std::vector<std::string> out;
    for (QTreeWidgetItem* it : list_->selectedItems()) out.push_back(it->text(0).toStdString());
    if (out.empty())
        if (const Sequence* s = state_->sequence())
            for (const VfxShotRecord& r : s->vfxShots) out.push_back(r.name);
    return out;
}

bool VfxShotsDialog::showVersion(int version, int step) {
    const std::vector<std::string> shots = chosenShots();
    QStringList failed;
    const bool done = state_->edit(step ? (step > 0 ? tr("Next VFX Version") : tr("Previous VFX Version")) : tr("Show VFX Version"),
                                   [&](Project& p, Sequence& s) {
                                       bool any = false;
                                       for (const std::string& name : shots) {
                                           const VfxShotRecord* r = findVfxShot(s, name);
                                           if (!r) continue;
                                           const edit::Result res = step ? edit::stepVfxVersion(p, s, r->clip, step) : edit::setVfxVersion(p, s, r->clip, version);
                                           if (res.ok) any = true;
                                           else failed << QString::fromStdString(res.error);
                                       }
                                       return any;
                                   });
    message_->setText(failed.join(QStringLiteral("\n")));
    return done;
}

bool VfxShotsDialog::setStatus(int status, const QString* note) {
    const std::vector<std::string> shots = chosenShots();
    if (list_->selectedItems().isEmpty()) {
        message_->setText(tr("Pick the shots in the list"));
        return false;
    }
    const std::string text = note ? note->toStdString() : std::string();
    return state_->edit(tr("VFX Status"), [&](Project&, Sequence& s) {
        for (const std::string& name : shots)
            if (!edit::setVfxStatus(s, name, status, note ? &text : nullptr).ok) return false;
        return true;
    });
}

bool VfxShotsDialog::exportList(const QString& path, QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    const std::string text = path.endsWith(QStringLiteral(".otio"), Qt::CaseInsensitive) ? vfxShotListOtio(*s) : vfxShotListCsv(*s);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size()) || !f.commit()) {
        if (error) *error = tr("Could not write %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool VfxShotsDialog::writeScripts(QString* error) {
    const std::vector<std::string> shots = chosenShots();
    std::vector<std::pair<std::string, std::string>> written;  // shot, file
    const Sequence* seq = state_->sequence();
    if (!seq) return false;
    for (const std::string& name : shots) {
        const VfxShotRecord* r = findVfxShot(*seq, name);
        if (!r) continue;
        const QString dir = QFileInfo(QString::fromStdString(r->plate)).absolutePath();
        const QString file = QString::fromStdString(vfxScriptFile(*r, dir.toStdString()));
        const int next = r->versions.empty() ? 1 : r->versions.back().number + 1;
        const QString ext = QFileInfo(QString::fromStdString(r->plate)).suffix().toLower();
        const std::string returns = seq->vfxReturns.empty() ? QDir(dir).filePath(QStringLiteral("returns")).toStdString() : seq->vfxReturns;
        const std::string nk = nukeScript(*r, vfxVersionPath(returns, seq->vfxPattern, *r, next, ext == QLatin1String("tif") ? "tiff" : ext.toStdString()));
        QSaveFile f(file);
        if (!f.open(QIODevice::WriteOnly) || f.write(nk.data(), qint64(nk.size())) != qint64(nk.size()) || !f.commit()) {
            if (error) *error = tr("Could not write %1").arg(QDir::toNativeSeparators(file));
            return false;
        }
        written.emplace_back(name, file.toStdString());
    }
    state_->edit(tr("VFX Scripts"), [&](Project&, Sequence& s) {
        bool changed = false;
        for (const auto& [name, file] : written)
            if (VfxShotRecord* r = findVfxShot(s, name); r && r->script != file) r->script = file, changed = true;
        return changed;
    });
    message_->setText(tr("Wrote %n script(s), each writing the version after the shot's latest", "", int(written.size())));
    return true;
}

void VfxShotsDialog::setReturnsFolder(const QString& folder) {
    const std::string path = QDir::cleanPath(QFileInfo(folder).absoluteFilePath()).toStdString();
    state_->edit(tr("VFX Returns Folder"), [&](Project&, Sequence& s) {
        if (s.vfxReturns == path) return false;
        s.vfxReturns = path;
        return true;
    });
}

}  // namespace montage
