#include "MergeDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Settings.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"

namespace montage {

MergeDialog::MergeDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Merge Project"));
    setObjectName(QStringLiteral("mergeDialog"));
    resize(900, 520);
    auto* layout = new QVBoxLayout(this);
    auto row = [&](QLabel*& label, const QString& button, const QString& name, auto pick) {
        auto* line = new QHBoxLayout;
        label = new QLabel(this);
        label->setWordWrap(true);
        auto* b = new QPushButton(button, this);
        b->setObjectName(name);
        connect(b, &QPushButton::clicked, this, pick);
        line->addWidget(label, 1);
        line->addWidget(b);
        layout->addLayout(line);
    };
    row(theirsLabel_, tr("Choose Their Copy…"), QStringLiteral("mergeChooseTheirs"), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Their Copy of the Project"), appSettings().value("lastProjectDir").toString(),
                                                          tr("Montage projects (*.montage)"));
        if (!path.isEmpty()) setTheirs(path);
    });
    row(baseLabel_, tr("Choose Base…"), QStringLiteral("mergeChooseBase"), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("The Version Both Started From"), appSettings().value("lastProjectDir").toString(),
                                                          tr("Montage projects (*.montage)"));
        if (!path.isEmpty()) setBase(path);
    });
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("mergeSummary"));
    layout->addWidget(summary_);
    table_ = new QTableWidget(0, Columns, this);
    table_->setObjectName(QStringLiteral("mergeTable"));
    table_->setHorizontalHeaderLabels({tr("Side"), tr("Change"), tr("What"), tr("Field"), tr("Take")});
    table_->horizontalHeader()->setSectionResizeMode(What, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { activate(r); });
    layout->addWidget(table_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    mergeButton_ = buttons->addButton(tr("Merge"), QDialogButtonBox::AcceptRole);
    mergeButton_->setObjectName(QStringLiteral("mergeButton"));
    connect(mergeButton_, &QPushButton::clicked, this, [this] {
        if (merge()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    fill();
}

bool MergeDialog::setTheirs(const QString& path) {
    Project p;
    std::string err;
    if (!loadProject(path.toStdString(), p, &err)) {
        haveTheirs_ = false;
        result_ = {};
        result_.error = tr("Could not read %1: %2").arg(QFileInfo(path).fileName(), QString::fromStdString(err)).toStdString();
        fill();
        return false;
    }
    theirs_ = std::move(p);
    theirsPath_ = path;
    haveTheirs_ = true;
    choices_.clear();
    if (basePath_.isEmpty()) haveBase_ = commonMergeBase(state_->project(), theirs_, base_);
    remerge();
    return result_.ok;
}

bool MergeDialog::setBase(const QString& path) {
    Project p;
    std::string err;
    if (!loadProject(path.toStdString(), p, &err)) {
        result_ = {};
        result_.error = tr("Could not read %1: %2").arg(QFileInfo(path).fileName(), QString::fromStdString(err)).toStdString();
        fill();
        return false;
    }
    base_ = MergeBase{std::move(p), {}, false};
    basePath_ = path;
    haveBase_ = true;
    choices_.clear();
    remerge();
    return result_.ok;
}

void MergeDialog::remerge() {
    result_ = {};
    if (haveTheirs_ && !haveBase_)
        result_.error = tr("Neither copy records the version both started from: choose it with Choose Base.").toStdString();
    else if (haveTheirs_)
        result_ = mergeProjects(base_, state_->project(), theirs_, choices_);
    previewed_ = state_->project();
    fill();
}

int MergeDialog::rowCount() const { return int(result_.conflicts.size() + result_.changes.size()); }

bool MergeDialog::isConflict(int row) const { return row >= 0 && row < int(result_.conflicts.size()); }

QString MergeDialog::cell(int row, int column) const {
    if (row < 0 || row >= rowCount()) return {};
    if (isConflict(row)) {
        const MergeConflict& c = result_.conflicts[size_t(row)];
        switch (column) {
        case Side: return tr("Conflict");
        case Kind:
            return c.kind == MergeConflict::Overlap ? tr("Clips overlap")
                   : c.kind == MergeConflict::Moved ? tr("Moved to two tracks")
                   : c.kind == MergeConflict::Removed ? tr("Removed and changed")
                                                      : tr("Changed on both");
        case What: return QString::fromStdString(c.what);
        case Field: return QString::fromStdString(c.field) + QStringLiteral(": ") + tr("mine %1, theirs %2").arg(QString::fromStdString(c.mine), QString::fromStdString(c.theirs));
        case Choice: return QString::fromLatin1(mergeSideName(c.choice));
        default: return {};
        }
    }
    const MergeChange& c = result_.changes[size_t(row) - result_.conflicts.size()];
    switch (column) {
    case Side: return QString::fromLatin1(mergeSideName(c.side));
    case Kind: return QString::fromLatin1(mergeChangeKindName(c.kind));
    case What: return QString::fromStdString(c.what);
    case Field: return QString::fromStdString(c.field);
    default: return {};
    }
}

void MergeDialog::choose(int row, MergeSide side) {
    if (!isConflict(row)) return;
    const MergeConflict& c = result_.conflicts[size_t(row)];
    if (side == MergeSide::Both && !c.canBoth) return;
    choices_[c.key] = side;
    remerge();
}

void MergeDialog::activate(int row) {
    if (row < 0 || row >= rowCount()) return;
    Id seq = 0, clip = 0;
    if (isConflict(row)) seq = result_.conflicts[size_t(row)].sequence, clip = result_.conflicts[size_t(row)].clip;
    else seq = result_.changes[size_t(row) - result_.conflicts.size()].sequence, clip = result_.changes[size_t(row) - result_.conflicts.size()].clip;
    const Project& p = state_->project();
    if (seq && p.findSequence(seq) && state_->sequence() && state_->sequence()->id != seq) state_->setActiveSequence(seq);
    if (const Sequence* s = state_->sequence(); clip && s && s->id == seq) {
        if (const Clip* c = edit::clipById(*s, clip)) {
            state_->setSelection({clip});
            state_->setPlayhead(c->start);
        } else {
            state_->message(tr("Not in this copy yet: it comes with the merge"));
        }
    }
}

bool MergeDialog::merge() {
    if (!haveTheirs_ || !haveBase_ || !result_.ok) return false;
    if (!(state_->project() == previewed_)) {  // edited since the list was made: shown again to be checked
        remerge();
        state_->message(tr("The project has changed since: check the merge again"));
        return false;
    }
    const Project merged = result_.project;
    const bool done = state_->edit(tr("Merge Project"), [&](Project& p, Sequence&) {
        p = merged;
        return true;
    });
    if (done)
        state_->message(tr("Merged %1: %n change(s)", nullptr, int(result_.changes.size())).arg(QFileInfo(theirsPath_).fileName()) +
                        (result_.conflicts.empty() ? QString() : tr(", %n conflict(s) settled", nullptr, int(result_.conflicts.size()))));
    return done;
}

QString MergeDialog::summary() const { return summary_->text(); }

void MergeDialog::fill() {
    theirsLabel_->setText(haveTheirs_ ? tr("Their copy: %1").arg(QFileInfo(theirsPath_).fileName()) : tr("Their copy: choose the other copy of the project."));
    baseLabel_->setText(!haveBase_            ? tr("Version both started from: not known yet.")
                        : !basePath_.isEmpty() ? tr("Version both started from: %1").arg(QFileInfo(basePath_).fileName())
                                               : tr("Version both started from: as recorded when the copy was made."));
    if (!result_.error.empty()) summary_->setText(QString::fromStdString(result_.error));
    else if (!result_.ok) summary_->setText(tr("Choose their copy of the project to see what merging it brings in."));
    else
        summary_->setText(tr("%n change(s)", nullptr, int(result_.changes.size())) + QStringLiteral(", ") +
                          tr("%n conflict(s)", nullptr, int(result_.conflicts.size())) +
                          (result_.renumbered.empty() ? QString() : QStringLiteral(", ") + tr("%n of their id(s) renumbered", nullptr, int(result_.renumbered.size()))) +
                          (result_.newTracks ? QStringLiteral(", ") + tr("%n new track(s) for their overlapping clips", nullptr, result_.newTracks) : QString()));
    mergeButton_->setEnabled(result_.ok);
    table_->setRowCount(rowCount());
    for (int r = 0; r < rowCount(); ++r) {
        for (int c = 0; c < Columns; ++c) {
            table_->removeCellWidget(r, c);
            table_->setItem(r, c, new QTableWidgetItem(c == Choice && isConflict(r) ? QString() : cell(r, c)));
        }
        if (!isConflict(r)) continue;
        const MergeConflict& k = result_.conflicts[size_t(r)];
        auto* box = new QComboBox(table_);
        box->setObjectName(QStringLiteral("mergeChoice"));
        box->addItem(tr("Mine"), int(MergeSide::Mine));
        box->addItem(tr("Theirs"), int(MergeSide::Theirs));
        if (k.canBoth) box->addItem(tr("Both (theirs on a new track)"), int(MergeSide::Both));
        box->setCurrentIndex(box->findData(int(k.choice)));
        connect(box, &QComboBox::activated, this, [this, r, box](int) {
            const auto side = MergeSide(box->currentData().toInt());
            QMetaObject::invokeMethod(this, [this, r, side] { choose(r, side); }, Qt::QueuedConnection);  // (the list is rebuilt)
        });
        table_->setCellWidget(r, Choice, box);
    }
}

}  // namespace montage
