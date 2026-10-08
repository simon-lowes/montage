#include "QualityCheckDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "core/History.h"

namespace montage {

namespace {

QDoubleSpinBox* seconds(double value, QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(0.1, 600);
    s->setDecimals(1);
    s->setSingleStep(0.5);
    s->setSuffix(QObject::tr(" s"));
    s->setValue(value);
    return s;
}

}  // namespace

QualityCheckDialog::QualityCheckDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Quality Check"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    range_ = new QComboBox(this);
    range_->setObjectName(QStringLiteral("qcRange"));
    range_->addItem(tr("Whole sequence"));
    range_->addItem(tr("In to Out"));
    const Sequence* s = state_->sequence();
    if (s && s->inPoint >= 0 && s->outPoint >= 0) range_->setCurrentIndex(1);
    else range_->setItemData(1, 0, Qt::UserRole - 1);  // disabled without In and Out
    form->addRow(tr("Range:"), range_);

    flashing_ = new QCheckBox(tr("Flashing that can trigger seizures (ITU-R BT.1702, Ofcom, WCAG)"), this);
    flashing_->setObjectName(QStringLiteral("qcFlashing"));
    flashing_->setChecked(true);
    levels_ = new QCheckBox(tr("Levels outside EBU R103"), this);
    levels_->setObjectName(QStringLiteral("qcLevels"));
    levels_->setChecked(true);
    form->addRow(tr("Picture:"), flashing_);
    form->addRow(QString(), levels_);
    auto row = [&](QCheckBox*& box, QDoubleSpinBox*& spin, const QString& text, const char* name, double value, bool on) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        box = new QCheckBox(text, w);
        box->setObjectName(QString::fromLatin1(name));
        box->setChecked(on);
        spin = seconds(value, w);
        spin->setObjectName(QString::fromLatin1(name) + QStringLiteral("Seconds"));
        connect(box, &QCheckBox::toggled, spin, &QWidget::setEnabled);
        h->addWidget(box);
        h->addWidget(spin);
        h->addStretch();
        return w;
    };
    form->addRow(QString(), row(black_, blackSeconds_, tr("Black for at least"), "qcBlack", 1.0, true));
    form->addRow(QString(), row(freeze_, freezeSeconds_, tr("Frozen for at least"), "qcFreeze", 5.0, true));
    form->addRow(tr("Sound:"), row(silence_, silenceSeconds_, tr("Silent for at least"), "qcSilence", 2.0, true));
    clipping_ = new QCheckBox(tr("Clipping (sound at full scale)"), this);
    clipping_->setObjectName(QStringLiteral("qcClipping"));
    clipping_->setChecked(true);
    form->addRow(QString(), clipping_);
    loudness_ = new QComboBox(this);
    loudness_->setObjectName(QStringLiteral("qcLoudness"));
    loudness_->addItem(tr("Not checked"), QPointF(0, 0));
    loudness_->addItem(tr("Streaming: -14 LUFS, -1 dBTP"), QPointF(-14, -1));
    loudness_->addItem(tr("Podcast: -16 LUFS, -1 dBTP"), QPointF(-16, -1));
    loudness_->addItem(tr("EBU R128: -23 LUFS, -1 dBTP"), QPointF(-23, -1));
    loudness_->addItem(tr("ATSC A/85: -24 LKFS, -2 dBTP"), QPointF(-24, -2));
    form->addRow(tr("Loudness:"), loudness_);
    layout->addLayout(form);

    auto* check = new QPushButton(tr("Check"), this);
    check->setObjectName(QStringLiteral("qcRun"));
    check->setDefault(true);
    connect(check, &QPushButton::clicked, this, [this] { runCheck(); });
    layout->addWidget(check, 0, Qt::AlignLeft);

    table_ = new QTableWidget(0, 3, this);
    table_->setObjectName(QStringLiteral("qcIssues"));
    table_->setHorizontalHeaderLabels({tr("Start"), tr("End"), tr("Problem")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setMinimumSize(560, 180);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { activate(r); });
    layout->addWidget(table_, 1);
    summary_ = new QLabel(tr("Choose the checks and press Check."), this);
    summary_->setObjectName(QStringLiteral("qcSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    markers_ = buttons->addButton(tr("Add Markers"), QDialogButtonBox::ActionRole);
    markers_->setObjectName(QStringLiteral("qcMarkers"));
    markers_->setEnabled(false);
    connect(markers_, &QPushButton::clicked, this, [this] { addMarkers(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QcSettings QualityCheckDialog::settings() const {
    QcSettings q;
    q.flashing = flashing_->isChecked();
    q.levels = levels_->isChecked();
    q.blackSeconds = black_->isChecked() ? blackSeconds_->value() : 0;
    q.freezeSeconds = freeze_->isChecked() ? freezeSeconds_->value() : 0;
    q.silenceSeconds = silence_->isChecked() ? silenceSeconds_->value() : 0;
    q.clipping = clipping_->isChecked();
    const QPointF target = loudness_->currentData().toPointF();
    q.loudnessTarget = target.x();
    q.peakCeiling = target.y();
    return q;
}

bool QualityCheckDialog::runCheck() {
    const Sequence* seq = state_->sequence();
    if (!seq) return false;
    const bool range = range_->currentIndex() == 1 && seq->inPoint >= 0 && seq->outPoint >= 0;
    const FrameTime in = range ? seq->inPoint : 0, out = range ? seq->outPoint + 1 : -1;
    const QcSettings q = settings();
    const Project project = state_->project();
    const Sequence sequence = *seq;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<int>>(0);
    QProgressDialog progress(tr("Checking the sequence…"), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(done->load()); });
    tick.start(100);
    QFutureWatcher<std::vector<QcIssue>> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<std::vector<QcIssue>>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([project, sequence, in, out, q, cancel, done] {
        return qualityCheck(project, sequence, in, out, q, [done](double f) { *done = int(f * 999); }, cancel.get());
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    if (*cancel) return false;
    issues_ = watcher.result();

    const Rational fps = sequence.fps;
    auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, fps)); };
    table_->setRowCount(int(issues_.size()));
    for (int r = 0; r < int(issues_.size()); ++r) {
        const QcIssue& i = issues_[size_t(r)];
        table_->setItem(r, 0, new QTableWidgetItem(tc(i.start)));
        table_->setItem(r, 1, new QTableWidgetItem(tc(i.end)));
        table_->setItem(r, 2, new QTableWidgetItem(QString::fromStdString(i.text)));
    }
    summary_->setText(issues_.empty() ? tr("No problems found.") : tr("%n problem(s) found. Double-click one to go to it.", nullptr, int(issues_.size())));
    markers_->setEnabled(!issues_.empty());
    return true;
}

int QualityCheckDialog::addMarkers() {
    if (issues_.empty()) return 0;
    const std::vector<QcIssue> issues = issues_;
    int added = 0;
    state_->edit(tr("Quality Check Markers"), [&](Project&, Sequence& s) {
        added = addQcMarkers(s, issues);
        return true;
    });
    state_->message(tr("Marked %n problem(s) on the timeline", nullptr, added), 5000);
    return added;
}

void QualityCheckDialog::activate(int row) {
    if (row < 0 || row >= int(issues_.size())) return;
    state_->setPlayhead(issues_[size_t(row)].start);
}

}  // namespace montage
