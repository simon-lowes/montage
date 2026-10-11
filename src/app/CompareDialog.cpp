#include "CompareDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <map>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/MediaLog.h"

namespace montage {

int CompareDialog::labelFor(ChangeKind k) {
    switch (k) {
        case ChangeKind::Added: return labelFromName("Forest");
        case ChangeKind::Removed: return labelFromName("Red");
        case ChangeKind::Trimmed: return labelFromName("Yellow");
        case ChangeKind::Moved: return labelFromName("Cerulean");
        case ChangeKind::Changed: return labelFromName("Violet");
    }
    return 0;
}

int CompareDialog::labelFor(CutEventKind k) {
    switch (k) {
        case CutEventKind::Same: return 0;
        case CutEventKind::Moved: return labelFromName("Cerulean");
        case CutEventKind::Inserted: return labelFromName("Forest");
        case CutEventKind::Extended: return labelFromName("Mango");
        case CutEventKind::Deleted: return labelFromName("Red");
        case CutEventKind::Trimmed: return labelFromName("Yellow");
    }
    return 0;
}

namespace {

QTableWidget* makeTable(QWidget* parent, const char* name, const QStringList& headers) {
    auto* t = new QTableWidget(0, int(headers.size()), parent);
    t->setObjectName(QString::fromLatin1(name));
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}

}  // namespace

CompareDialog::CompareDialog(EditorState* state, Id before, QWidget* parent) : QDialog(parent), state_(state), before_(before) {
    setObjectName(QStringLiteral("compareDialog"));
    const Sequence* after = state_->sequence();
    after_ = after ? after->id : 0;
    resize(880, 520);
    auto* lay = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("compareSummary"));
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("compareTabs"));
    lay->addWidget(tabs_, 1);
    table_ = makeTable(tabs_, "compareTable", {tr("Change"), tr("Clip"), tr("Track"), tr("Timecode"), tr("Length"), tr("Details")});
    tabs_->addTab(table_, tr("Clips"));
    connect(table_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) { activate(item->row()); });
    auto* cutPage = new QWidget(tabs_);
    auto* cutLay = new QVBoxLayout(cutPage);
    cutSummary_ = new QLabel(cutPage);
    cutSummary_->setObjectName(QStringLiteral("cutSummary"));
    cutSummary_->setWordWrap(true);
    cutLay->addWidget(cutSummary_);
    cutTable_ = makeTable(cutPage, "cutTable",
                          {tr("#"), tr("Change"), tr("Shot"), tr("Old In"), tr("Old Out"), tr("New In"), tr("New Out"), tr("Length"), tr("Shift")});
    cutLay->addWidget(cutTable_, 1);
    tabs_->addTab(cutPage, tr("Change List"));
    connect(cutTable_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) { activateCut(item->row()); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    markersButton_ = buttons->addButton(tr("Add as Markers"), QDialogButtonBox::ActionRole);
    markersButton_->setObjectName(QStringLiteral("compareAddMarkers"));
    connect(markersButton_, &QPushButton::clicked, this, [this] { addMarkers(); });
    exportButton_ = buttons->addButton(tr("Export Change List…"), QDialogButtonBox::ActionRole);
    exportButton_->setObjectName(QStringLiteral("compareExportList"));
    connect(exportButton_, &QPushButton::clicked, this, [this] { exportChangeListDialog(); });
    conformButton_ = buttons->addButton(tr("Re-conform…"), QDialogButtonBox::ActionRole);
    conformButton_->setObjectName(QStringLiteral("compareReconform"));
    connect(conformButton_, &QPushButton::clicked, this, [this] { reconformDialog(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    refresh();
    // Edits to either version while the dialog is open: the lists follow (once the edits settle).
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(250);
    connect(refreshTimer_, &QTimer::timeout, this, &CompareDialog::refresh);
    connect(state_, &EditorState::projectChanged, refreshTimer_, qOverload<>(&QTimer::start));
}

void CompareDialog::refresh() {
    if (refreshTimer_) refreshTimer_->stop();
    const Project& p = state_->project();
    const Sequence* after = p.findSequence(after_);
    const Sequence* old = p.findSequence(before_);
    const QString oldName = QString::fromStdString(old ? old->name : ""), newName = QString::fromStdString(after ? after->name : "");
    setWindowTitle(tr("Compare %1 with %2").arg(oldName, newName));
    changes_.clear();
    cuts_ = CutChanges{};
    std::string cutError;
    if (old && after) {
        changes_ = compareSequences(p, *old, *after);
        cuts_ = cutChanges(p, *old, *after, &cutError);
    } else {
        cutError = tr("One of the two versions is no longer in the project.").toStdString();
    }
    std::map<ChangeKind, int> counts;
    for (const TimelineChange& c : changes_) ++counts[c.kind];
    QStringList parts;
    for (ChangeKind k : {ChangeKind::Added, ChangeKind::Removed, ChangeKind::Trimmed, ChangeKind::Moved, ChangeKind::Changed})
        if (counts[k]) parts << QStringLiteral("%1 %2").arg(counts[k]).arg(QString::fromLatin1(changeKindName(k)).toLower());
    summary_->setText(changes_.empty() ? tr("No differences: the two versions play the same clips the same way.")
                                       : tr("From %1 to %2: %3.").arg(oldName, newName, parts.join(QStringLiteral(", "))));
    const Rational fps = after ? after->fps : Rational{30, 1};
    auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, fps)); };
    table_->setRowCount(0);
    for (const TimelineChange& c : changes_) {
        const int r = table_->rowCount();
        table_->insertRow(r);
        const QString track = QStringLiteral("%1%2").arg(c.track.kind == TrackKind::Video ? "V" : "A").arg(c.track.index + 1);
        const QStringList cells{QString::fromLatin1(changeKindName(c.kind)), QString::fromStdString(c.name), track, tc(c.at), tc(c.length),
                                QString::fromStdString(c.details)};
        for (int col = 0; col < Columns; ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == Change) item->setData(Qt::DecorationRole, theme::labelColor(labelFor(c.kind)));
            table_->setItem(r, col, item);
        }
    }
    table_->resizeColumnsToContents();

    if (!cutError.empty()) {
        cutSummary_->setText(QString::fromStdString(cutError));
    } else {
        std::map<CutEventKind, int> n;
        for (const montage::CutEvent& e : cuts_.events) ++n[e.kind];
        QStringList what;
        for (CutEventKind k : {CutEventKind::Moved, CutEventKind::Inserted, CutEventKind::Extended, CutEventKind::Deleted, CutEventKind::Trimmed})
            if (n[k]) what << QStringLiteral("%1 %2").arg(n[k]).arg(QString::fromLatin1(cutEventName(k)).toLower());
        cutSummary_->setText(what.isEmpty() ? tr("The picture is the same: nothing to re-conform.")
                                            : tr("The picture, frame by frame: %1 (%2 to %3).")
                                                  .arg(what.join(QStringLiteral(", ")), tc(cuts_.oldLength), tc(cuts_.newLength)));
    }
    cutTable_->setRowCount(0);
    int number = 0;
    for (const montage::CutEvent& e : cuts_.events) {
        const int r = cutTable_->rowCount();
        cutTable_->insertRow(r);
        const bool hasOld = e.kind != CutEventKind::Inserted && e.kind != CutEventKind::Extended;
        const bool kept = e.kind == CutEventKind::Same || e.kind == CutEventKind::Moved;
        QString shot = QString::fromStdString(e.shot);
        if (e.shots > 1) shot += tr(" (+%1 more)").arg(e.shots - 1);
        const QStringList cells{QString::number(++number),
                                QString::fromLatin1(cutEventName(e.kind)),
                                shot,
                                hasOld ? tc(e.oldIn) : QString(),
                                hasOld ? tc(e.oldOut) : QString(),
                                tc(e.newIn),
                                tc(e.newOut),
                                tc(e.length()),
                                kept && e.shift() ? QStringLiteral("%1%2").arg(e.shift() > 0 ? "+" : "").arg(e.shift()) : QString()};
        for (int col = 0; col < CutColumns; ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == CutKind && labelFor(e.kind)) item->setData(Qt::DecorationRole, theme::labelColor(labelFor(e.kind)));
            cutTable_->setItem(r, col, item);
        }
    }
    cutTable_->resizeColumnsToContents();
    markersButton_->setEnabled(!changes_.empty());
    exportButton_->setEnabled(!cuts_.events.empty());
    conformButton_->setEnabled(cuts_.changed() > 0);
    conformButton_->setToolTip(tr("Rebuild a sequence cut to %1 (a mix, a grade, effects and titles) to play against %2").arg(oldName, newName));
}

int CompareDialog::rowCount() const { return table_->rowCount(); }

QString CompareDialog::cell(int row, int column) const {
    const QTableWidgetItem* item = table_->item(row, column);
    return item ? item->text() : QString();
}

int CompareDialog::cutRowCount() const { return cutTable_->rowCount(); }

QString CompareDialog::cutCell(int row, int column) const {
    const QTableWidgetItem* item = cutTable_->item(row, column);
    return item ? item->text() : QString();
}

void CompareDialog::showTab(int index) { tabs_->setCurrentIndex(index); }

void CompareDialog::activate(int row) {
    if (row < 0 || row >= int(changes_.size())) return;
    if (state_->sequence() && state_->sequence()->id != after_) state_->setActiveSequence(after_);
    const TimelineChange& c = changes_[size_t(row)];
    if (c.after) state_->setSelection({c.after});
    state_->setPlayhead(c.at);
}

void CompareDialog::activateCut(int row) {
    if (row < 0 || row >= int(cuts_.events.size())) return;
    if (state_->sequence() && state_->sequence()->id != after_) state_->setActiveSequence(after_);
    state_->setPlayhead(cuts_.events[size_t(row)].newIn);
}

int CompareDialog::addMarkers() {
    if (refreshTimer_ && refreshTimer_->isActive()) refresh();
    if (state_->sequence() && state_->sequence()->id != after_) state_->setActiveSequence(after_);
    const auto changes = changes_;
    int n = 0;
    state_->edit(tr("Add Change Markers"), [&](Project&, Sequence& s) {
        for (const TimelineChange& c : changes) {
            std::string name = changeKindName(c.kind);
            if (!c.name.empty()) name += ": " + c.name;
            edit::addMarker(s, Marker{c.at, c.kind == ChangeKind::Removed ? 0 : c.length, name, c.details, labelFor(c.kind)});
            ++n;
        }
        return n > 0;
    });
    return n;
}

bool CompareDialog::exportChangeList(const QString& path, int format) {
    refresh();  // the versions as they are now
    const bool edl = format >= 0 ? format == 1 : QFileInfo(path).suffix().compare(QStringLiteral("edl"), Qt::CaseInsensitive) == 0;
    const std::string text = edl ? changeEdl(cuts_) : changeListCsv(cuts_);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        state_->message(tr("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    state_->message(tr("Change list written to %1").arg(QFileInfo(path).fileName()));
    return true;
}

void CompareDialog::exportChangeListDialog() {
    QString filter;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Change List"), QString(), tr("Change EDL (*.edl);;Change list (*.csv)"), &filter);
    if (path.isEmpty()) return;
    // The format the filter names, its extension added unless the name already ends with it.
    const bool edl = !filter.contains("*.csv");
    QString file = path;
    if (QFileInfo(file).suffix().compare(edl ? "edl" : "csv", Qt::CaseInsensitive) != 0) file += edl ? ".edl" : ".csv";
    exportChangeList(file, edl ? 1 : 0);
}

Id CompareDialog::reconform(Id source, const ReconformOptions& options) {
    refresh();  // the versions as they are now
    Id made = 0;
    std::string error;
    const CutChanges cuts = cuts_;
    const Id newCut = after_;
    state_->edit(tr("Re-conform Sequence"), [&](Project& p, Sequence&) {
        made = reconformSequence(p, source, cuts, newCut, options, &error).sequence;
        return made != 0;
    });
    if (!made) {
        state_->message(QString::fromStdString(error));
        return 0;
    }
    state_->setActiveSequence(made);
    return made;
}

void CompareDialog::reconformDialog() {
    const Project& p = state_->project();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Re-conform"));
    auto* form = new QFormLayout(&dlg);
    const Sequence* old = p.findSequence(before_);
    const Sequence* now = p.findSequence(after_);
    auto* intro = new QLabel(tr("Make a copy of a sequence cut to %1 that plays against %2.")
                                 .arg(QString::fromStdString(old ? old->name : ""), QString::fromStdString(now ? now->name : "")),
                             &dlg);
    intro->setWordWrap(true);
    form->addRow(intro);
    auto* source = new QComboBox(&dlg);
    for (const Sequence& s : p.sequences) {
        if (s.id == after_ || int64_t(s.fps.num) * cuts_.fps.den != int64_t(cuts_.fps.num) * s.fps.den) continue;
        source->addItem(QString::fromStdString(s.name), QVariant::fromValue<qulonglong>(s.id));
        if (s.id == before_) source->setCurrentIndex(source->count() - 1);
    }
    form->addRow(tr("Sequence:"), source);
    auto* name = new QLineEdit(&dlg);
    auto setName = [=] { name->setText(tr("%1 (Conformed)").arg(source->currentText())); };
    setName();
    connect(source, &QComboBox::currentIndexChanged, &dlg, setName);
    form->addRow(tr("Name:"), name);
    auto* fill = new QCheckBox(tr("Fill new material from %1").arg(QString::fromStdString(now ? now->name : "")), &dlg);
    fill->setChecked(true);
    form->addRow(fill);
    auto* marks = new QCheckBox(tr("Mark what was added and taken out"), &dlg);
    marks->setChecked(true);
    form->addRow(marks);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (source->count() == 0 || dlg.exec() != QDialog::Accepted) return;
    ReconformOptions o;
    o.name = name->text().trimmed().toStdString();
    o.fillFromNewCut = fill->isChecked();
    o.markers = marks->isChecked();
    reconform(Id(source->currentData().toULongLong()), o);
}

}  // namespace montage
