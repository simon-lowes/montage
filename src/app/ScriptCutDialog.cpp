#include "ScriptCutDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "EditorState.h"
#include "Theme.h"

namespace montage {

namespace {
QString clock(double seconds) {
    const int m = int(seconds) / 60;
    return QStringLiteral("%1:%2").arg(m).arg(seconds - m * 60, 4, 'f', 1, QLatin1Char('0'));
}
}  // namespace

ScriptCutDialog::ScriptCutDialog(EditorState* state, const std::vector<Id>& selected, QWidget* parent)
    : QDialog(parent), state_(state), selected_(selected) {
    setWindowTitle(tr("Build Cut from Script"));
    resize(900, 600);
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Paste the script or open it. Each line is found in the transcribed takes; the best reading "
                                "of each goes on V1/A1 in script order, and other readings sit disabled on the tracks above. "
                                "A name in capitals above the words, or \"Name:\" before them, says who speaks."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto* split = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(split);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    text_ = new QPlainTextEdit(left);
    text_->setObjectName(QStringLiteral("scriptText"));
    text_->setPlaceholderText(tr("JOHN\nI never thought we'd make it this far.\n\nMARY\nNeither did I."));
    auto* open = new QPushButton(tr("Open Script…"), left);
    open->setObjectName(QStringLiteral("openScript"));
    lv->addWidget(text_, 1);
    lv->addWidget(open, 0, Qt::AlignLeft);
    preview_ = new QTreeWidget(split);
    preview_->setObjectName(QStringLiteral("scriptPreview"));
    preview_->setHeaderLabels({tr("Line"), tr("Takes"), tr("Best reading")});
    preview_->setRootIsDecorated(false);
    preview_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    split->addWidget(left);
    split->addWidget(preview_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("scriptSummary"));
    summary_->setWordWrap(true);
    lay->addWidget(summary_);

    auto* form = new QFormLayout;
    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("scriptScope"));
    scope_->addItem(tr("Every transcribed clip"));
    if (!selected_.empty()) {
        scope_->addItem(tr("The %n clip(s) selected in the bin", "", int(selected_.size())));
        scope_->setCurrentIndex(1);
    }
    coverage_ = new QDoubleSpinBox(this);
    coverage_->setObjectName(QStringLiteral("scriptCoverage"));
    coverage_->setRange(20, 100);
    coverage_->setValue(60);
    coverage_->setSuffix(tr(" % of the line"));
    coverage_->setDecimals(0);
    coverage_->setToolTip(tr("How much of a line a reading must say to count"));
    alternates_ = new QSpinBox(this);
    alternates_->setObjectName(QStringLiteral("scriptAlternates"));
    alternates_->setRange(0, 8);
    alternates_->setValue(3);
    handle_ = new QDoubleSpinBox(this);
    handle_->setObjectName(QStringLiteral("scriptHandle"));
    handle_->setRange(0, 2);
    handle_->setSingleStep(0.05);
    handle_->setValue(0.15);
    handle_->setSuffix(tr(" s"));
    markers_ = new QCheckBox(tr("A marker on each line"), this);
    markers_->setObjectName(QStringLiteral("scriptMarkers"));
    markers_->setChecked(true);
    name_ = new QLineEdit(tr("Script Cut"), this);
    name_->setObjectName(QStringLiteral("scriptSequenceName"));
    form->addRow(tr("Search:"), scope_);
    form->addRow(tr("A reading says at least:"), coverage_);
    form->addRow(tr("Alternates per line:"), alternates_);
    form->addRow(tr("Handles:"), handle_);
    form->addRow(QString(), markers_);
    form->addRow(tr("New sequence:"), name_);
    lay->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Build Cut"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(300);
    connect(debounce_, &QTimer::timeout, this, &ScriptCutDialog::refresh);
    auto changed = [this] {
        stale_ = true;
        debounce_->start();
    };
    connect(text_, &QPlainTextEdit::textChanged, this, changed);
    connect(scope_, &QComboBox::currentIndexChanged, this, changed);
    connect(coverage_, &QDoubleSpinBox::valueChanged, this, changed);
    connect(open, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Script"), QString(),
                                                          tr("Scripts (*.txt *.fountain *.md *.fdx);;All files (*)"));
        QString err;
        if (!path.isEmpty() && !loadScript(path, &err)) summary_->setText(err);
    });
    refresh();
}

void ScriptCutDialog::setScript(const QString& text) {
    text_->setPlainText(text);
    refresh();
}

QString ScriptCutDialog::script() const { return text_->toPlainText(); }

bool ScriptCutDialog::loadScript(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Could not open %1").arg(QFileInfo(path).fileName());
        return false;
    }
    const QByteArray data = f.readAll();
    if (path.endsWith(QStringLiteral(".fdx"), Qt::CaseInsensitive)) setScript(QString::fromStdString(fdxToScript(data.toStdString())));
    else setScript(QString::fromUtf8(data));
    return true;
}

ScriptCutOptions ScriptCutDialog::options() const {
    ScriptCutOptions o;
    o.minCoverage = coverage_->value() / 100.0;
    o.maxAlternates = alternates_->value();
    o.handle = handle_->value();
    o.markers = markers_->isChecked();
    if (scope_->currentIndex() == 1) o.media = selected_;
    return o;
}

QString ScriptCutDialog::sequenceName() const {
    const QString n = name_->text().trimmed();
    return n.isEmpty() ? tr("Script Cut") : n;
}

const std::vector<ScriptMatch>& ScriptCutDialog::matches() {
    if (stale_) refresh();
    return matches_;
}

void ScriptCutDialog::refresh() {
    debounce_->stop();
    stale_ = false;
    matches_ = matchScript(state_->project(), parseScript(script().toStdString()), options());
    preview_->clear();
    int found = 0;
    for (const ScriptMatch& m : matches_) {
        QString line = QString::fromStdString(m.line.text);
        if (!m.line.speaker.empty()) line = QString::fromStdString(m.line.speaker) + QStringLiteral(": ") + line;
        auto* it = new QTreeWidgetItem(preview_);
        it->setText(0, line);
        it->setToolTip(0, line);
        it->setText(1, QString::number(m.takes.size()));
        if (m.takes.empty()) {
            it->setText(2, tr("Not found"));
            for (int c = 0; c < 3; ++c) it->setForeground(c, QColor(230, 110, 100));
            continue;
        }
        ++found;
        const ScriptTake& t = m.takes.front();
        const MediaItem* media = state_->project().findMedia(t.mediaId);
        QString best = QStringLiteral("%1  %2–%3").arg(media ? QString::fromStdString(media->name) : QString(), clock(t.start), clock(t.end));
        if (t.extraWords > 0) best += tr("  (%n extra word(s))", "", t.extraWords);
        it->setText(2, best);
    }
    bool anyTranscript = false;
    for (const MediaItem& m : state_->project().media) anyTranscript = anyTranscript || (m.transcript && !m.transcript->empty());
    if (!anyTranscript) summary_->setText(tr("Nothing is transcribed yet: transcribe the takes first (Clip › Transcribe)."));
    else if (matches_.empty()) summary_->setText(tr("No lines yet."));
    else summary_->setText(tr("Found %1 of %n line(s).", "", int(matches_.size())).arg(found));
}

ScriptCutResult ScriptCutDialog::build(EditorState* state, const QString& script, const ScriptCutOptions& o, const QString& name) {
    ScriptCutResult res;
    const auto lines = parseScript(script.toStdString());
    if (lines.empty()) return res;
    state->edit(tr("Build Cut from Script"), [&](Project& p, Sequence&) {
        res = buildScriptCut(p, matchScript(p, lines, o), name.toStdString(), o);
        return res.sequence != 0;
    });
    if (res.sequence) state->setActiveSequence(res.sequence);
    return res;
}

}  // namespace montage
