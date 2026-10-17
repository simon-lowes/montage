#include "ConformCheckDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <map>
#include <memory>
#include <set>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "core/MediaLog.h"

namespace montage {

int ConformCheckDialog::labelFor(ConformVerdict v) {
    switch (v) {
        case ConformVerdict::Matches: return 0;
        case ConformVerdict::Reframed: return labelFromName("Cerulean");
        case ConformVerdict::Slipped: return labelFromName("Yellow");
        case ConformVerdict::OtherTake: return labelFromName("Violet");
        case ConformVerdict::NoMatch: return labelFromName("Red");
        case ConformVerdict::Missing: return labelFromName("Mango");
    }
    return 0;
}

ConformCheckDialog::ConformCheckDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setObjectName(QStringLiteral("conformCheckDialog"));
    setWindowTitle(tr("Conform Check"));
    resize(860, 500);
    const Sequence* seq = state_->sequence();
    sequence_ = seq ? seq->id : 0;
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Each shot of this sequence against the offline cut's reference movie: what matches, what is "
                                "framed differently, slipped or another take. Grades and burnt-in timecode are not counted."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* form = new QFormLayout;
    reference_ = new QComboBox(this);
    reference_->setObjectName(QStringLiteral("conformReference"));
    // The video media of the project; first the ones this sequence does not use (a reference movie is not cut in),
    // the one nearest its length first among those.
    const Project& p = state_->project();
    std::set<Id> used;
    if (seq)
        for (const Track& tr : seq->videoTracks)
            for (const Clip& c : tr.clips) used.insert(c.mediaId);
    const double length = seq && seq->fpsValue() > 0 ? double(seq->duration()) / seq->fpsValue() : 0;
    std::vector<const MediaItem*> items;
    for (const MediaItem& m : p.media)
        if (m.hasVideo && m.kind == MediaKind::Video) items.push_back(&m);
    std::stable_sort(items.begin(), items.end(), [&](const MediaItem* a, const MediaItem* b) {
        const bool ua = used.count(a->id), ub = used.count(b->id);
        if (ua != ub) return !ua;
        return std::fabs(a->duration - length) < std::fabs(b->duration - length);
    });
    for (const MediaItem* m : items) reference_->addItem(QString::fromStdString(m->name), QVariant::fromValue<qint64>(m->id));
    form->addRow(tr("Reference movie:"), reference_);
    start_ = new QLineEdit(this);
    start_->setObjectName(QStringLiteral("conformStart"));
    start_->setText(QString::fromStdString(formatTimecode(0, seq ? seq->fps : Rational{25, 1})));
    start_->setToolTip(tr("Where in this sequence the reference movie's first frame belongs"));
    form->addRow(tr("Reference starts at:"), start_);
    lay->addLayout(form);
    auto* run = new QPushButton(tr("Check"), this);
    run->setObjectName(QStringLiteral("conformRun"));
    run->setEnabled(reference_->count() > 0 && seq);
    connect(run, &QPushButton::clicked, this, [this] { runCheck(); });
    lay->addWidget(run, 0, Qt::AlignLeft);
    summary_ = new QLabel(reference_->count() ? tr("Pick the reference movie and check.") : tr("Import the offline cut's reference movie first."), this);
    summary_->setObjectName(QStringLiteral("conformSummary"));
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    table_ = new QTableWidget(0, 5, this);
    table_->setObjectName(QStringLiteral("conformShots"));
    table_->setHorizontalHeaderLabels({tr("Shot"), tr("Timecode"), tr("Verdict"), tr("Likeness"), tr("Details")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    connect(table_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) { activate(item->row()); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &ConformCheckDialog::updateButtons);
    lay->addWidget(table_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    applyButton_ = buttons->addButton(tr("Apply Fix"), QDialogButtonBox::ActionRole);
    applyButton_->setObjectName(QStringLiteral("conformApply"));
    connect(applyButton_, &QPushButton::clicked, this, [this] { applyFixes(false); });
    applyAllButton_ = buttons->addButton(tr("Apply All"), QDialogButtonBox::ActionRole);
    applyAllButton_->setObjectName(QStringLiteral("conformApplyAll"));
    connect(applyAllButton_, &QPushButton::clicked, this, [this] { applyFixes(true); });
    markersButton_ = buttons->addButton(tr("Add as Markers"), QDialogButtonBox::ActionRole);
    markersButton_->setObjectName(QStringLiteral("conformMarkers"));
    connect(markersButton_, &QPushButton::clicked, this, [this] { addMarkers(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    updateButtons();
    connect(state_, &EditorState::projectChanged, this, [this] {
        if (!editing_ && !shots_.empty()) forget();
    });
}

void ConformCheckDialog::forget() {
    shots_.clear();
    fixed_.clear();
    table_->setRowCount(0);
    summary_->setText(tr("The project has changed since the check: check again."));
    updateButtons();
}

void ConformCheckDialog::setReference(Id media) {
    const int i = reference_->findData(QVariant::fromValue<qint64>(media));
    if (i >= 0) reference_->setCurrentIndex(i);
}

void ConformCheckDialog::setReferenceStart(FrameTime t) {
    const Sequence* seq = state_->project().findSequence(sequence_);
    start_->setText(QString::fromStdString(formatTimecode(t, seq ? seq->fps : Rational{25, 1})));
}

bool ConformCheckDialog::runCheck() {
    const Sequence* seq = state_->project().findSequence(sequence_);
    if (!seq || reference_->currentIndex() < 0) return false;
    ConformOptions o;
    if (!parseTimecode(start_->text().toStdString(), seq->fps, o.referenceStart)) {
        summary_->setText(tr("“%1” is not a timecode.").arg(start_->text()));
        return false;
    }
    const Id ref = Id(reference_->currentData().toLongLong());
    const Project project = state_->project();
    const Sequence sequence = *seq;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<int>>(0);
    QProgressDialog progress(tr("Comparing the shots with the reference…"), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(done->load()); });
    tick.start(100);
    struct Outcome {
        std::vector<ConformShot> shots;
        std::string error;
    };
    QFutureWatcher<Outcome> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<Outcome>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([project, sequence, ref, o, cancel, done] {
        Outcome out;
        out.shots = conformCheck(project, sequence, ref, o, [done](double f) { *done = int(f * 999); }, cancel.get(), &out.error);
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    if (*cancel) return false;
    const Outcome out = watcher.result();
    if (!state_->project().findSequence(sequence_)) return false;  // (deleted while it ran)
    shots_ = out.shots;
    fixed_.assign(shots_.size(), false);
    marked_ = false;
    if (shots_.empty() && !out.error.empty()) {
        summary_->setText(QString::fromStdString(out.error));
        table_->setRowCount(0);
        updateButtons();
        return false;
    }
    auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, sequence.fps)); };
    std::map<ConformVerdict, int> n;
    table_->setRowCount(int(shots_.size()));
    for (int r = 0; r < int(shots_.size()); ++r) {
        const ConformShot& s = shots_[size_t(r)];
        ++n[s.verdict];
        const Clip* c = edit::clipById(sequence, s.clip);
        const QStringList cells{c ? QString::fromStdString(c->name) : tr("(none)"), tc(s.start), QString::fromLatin1(conformVerdictName(s.verdict)),
                                QStringLiteral("%1 %").arg(int(std::lround(s.score * 100))), QString::fromStdString(s.details)};
        for (int col = 0; col < 5; ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == 2 && labelFor(s.verdict)) item->setData(Qt::DecorationRole, theme::labelColor(labelFor(s.verdict)));
            table_->setItem(r, col, item);
        }
    }
    table_->resizeColumnsToContents();
    QStringList parts;
    for (ConformVerdict v : {ConformVerdict::Reframed, ConformVerdict::Slipped, ConformVerdict::OtherTake, ConformVerdict::NoMatch, ConformVerdict::Missing})
        if (n[v]) parts << QStringLiteral("%1 %2").arg(n[v]).arg(QString::fromLatin1(conformVerdictName(v)).toLower());
    summary_->setText(shots_.empty()   ? tr("This sequence has no shots from media to check.")
                      : parts.isEmpty() ? tr("All %n shot(s) match the reference.", nullptr, int(shots_.size()))
                                        : tr("%1 of %2 shots match; %3. Double-click a shot to go to it.")
                                              .arg(n[ConformVerdict::Matches])
                                              .arg(shots_.size())
                                              .arg(parts.join(QStringLiteral(", "))));
    updateButtons();
    return true;
}

void ConformCheckDialog::activate(int row) {
    if (row < 0 || row >= int(shots_.size()) || !state_->project().findSequence(sequence_)) return;
    if (state_->sequence() && state_->sequence()->id != sequence_) state_->setActiveSequence(sequence_);
    if (shots_[size_t(row)].clip) state_->setSelection({shots_[size_t(row)].clip});
    state_->setPlayhead(shots_[size_t(row)].start);
}

int ConformCheckDialog::applyFixes(bool all) {
    std::set<int> rows;
    if (all) {
        for (int r = 0; r < int(shots_.size()); ++r) rows.insert(r);
    } else {
        for (const QModelIndex& i : table_->selectionModel()->selectedRows()) rows.insert(i.row());
    }
    std::vector<int> todo;
    for (int r : rows)
        if (r < int(shots_.size()) && !fixed_[size_t(r)] && shots_[size_t(r)].fixable()) todo.push_back(r);
    if (todo.empty() || !state_->project().findSequence(sequence_)) return 0;
    if (state_->sequence() && state_->sequence()->id != sequence_) state_->setActiveSequence(sequence_);
    if (!state_->sequence() || state_->sequence()->id != sequence_) return 0;
    std::vector<int> done;
    editing_ = true;
    state_->edit(todo.size() == 1 ? tr("Apply Conform Fix") : tr("Apply Conform Fixes"), [&](Project& p, Sequence& s) {
        for (int r : todo)
            if (applyConformFix(p, s, shots_[size_t(r)])) done.push_back(r);
        return !done.empty();
    });
    editing_ = false;
    for (int r : done) {
        fixed_[size_t(r)] = true;
        table_->item(r, 2)->setText(tr("Fixed (%1)").arg(QString::fromLatin1(conformVerdictName(shots_[size_t(r)].verdict)).toLower()));
        table_->item(r, 2)->setData(Qt::DecorationRole, QVariant());
    }
    updateButtons();
    return int(done.size());
}

int ConformCheckDialog::addMarkers() {
    std::vector<ConformShot> differ;
    for (size_t i = 0; i < shots_.size(); ++i)
        if (shots_[i].verdict != ConformVerdict::Matches && !fixed_[i]) differ.push_back(shots_[i]);
    if (differ.empty() || marked_ || !state_->project().findSequence(sequence_)) return 0;
    if (state_->sequence() && state_->sequence()->id != sequence_) state_->setActiveSequence(sequence_);
    if (!state_->sequence() || state_->sequence()->id != sequence_) return 0;
    int n = 0;
    editing_ = true;
    state_->edit(tr("Add Conform Markers"), [&](Project&, Sequence& s) {
        for (const ConformShot& c : differ) {
            edit::addMarker(s, Marker{c.start, c.length, std::string("Conform: ") + conformVerdictName(c.verdict), c.details, labelFor(c.verdict)});
            ++n;
        }
        return n > 0;
    });
    editing_ = false;
    marked_ = n > 0;
    updateButtons();
    return n;
}

void ConformCheckDialog::updateButtons() {
    bool any = false, selected = false, differs = false;
    for (size_t i = 0; i < shots_.size(); ++i) {
        if (fixed_[i]) continue;
        any = any || shots_[i].fixable();
        differs = differs || shots_[i].verdict != ConformVerdict::Matches;
    }
    if (table_->selectionModel())
        for (const QModelIndex& i : table_->selectionModel()->selectedRows())
            if (i.row() < int(shots_.size()) && !fixed_[size_t(i.row())] && shots_[size_t(i.row())].fixable()) selected = true;
    applyButton_->setEnabled(selected);
    applyAllButton_->setEnabled(any);
    markersButton_->setEnabled(differs && !marked_);
}

}  // namespace montage
