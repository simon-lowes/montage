#include "AutoDuckDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QProgressDialog>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <memory>

#include "EditorState.h"
#include "core/EditOps.h"

namespace montage {

namespace {
QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, double value, double step, const QString& suffix) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(QString::fromLatin1(name));
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(step < 1 ? 1 : 0);
    s->setValue(value);
    s->setSuffix(suffix);
    return s;
}
}  // namespace

AutoDuckDialog::AutoDuckDialog(EditorState* state, const std::vector<Id>& music, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Auto Duck Music"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("The %n selected clip(s) will dip wherever someone speaks on the tracks ticked below. "
                                "The result is volume keyframes you can adjust on the timeline.", "", int(music.size())),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    tracks_ = new QListWidget(this);
    tracks_->setObjectName(QStringLiteral("duckTracks"));
    tracks_->setMaximumHeight(120);
    const Sequence* s = state->sequence();
    for (int i = 0; s && i < int(s->audioTracks.size()); ++i) {
        const Track& t = s->audioTracks[size_t(i)];
        const bool holdsMusic = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
            return std::find(music.begin(), music.end(), c.id) != music.end();
        });
        auto* it = new QListWidgetItem(QString::fromStdString(t.name), tracks_);
        it->setData(Qt::UserRole, i);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        // Dialogue: the other tracks with sound on them.
        it->setCheckState(!holdsMusic && !t.clips.empty() ? Qt::Checked : Qt::Unchecked);
    }
    lay->addWidget(new QLabel(tr("Dialogue is on:"), this));
    lay->addWidget(tracks_);
    auto* form = new QFormLayout;
    amount_ = spin(this, "duckAmount", -40, -1, -15, 1, tr(" dB"));
    fadeDown_ = spin(this, "duckFadeDown", 0, 5, 0.3, 0.1, tr(" s"));
    fadeUp_ = spin(this, "duckFadeUp", 0, 5, 0.8, 0.1, tr(" s"));
    threshold_ = spin(this, "duckThreshold", -70, -10, -40, 1, tr(" dBFS"));
    threshold_->setToolTip(tr("Sound louder than this on the dialogue tracks counts as speech"));
    transcripts_ = new QCheckBox(tr("Use transcripts' words where clips are transcribed"), this);
    transcripts_->setObjectName(QStringLiteral("duckTranscripts"));
    transcripts_->setChecked(true);
    form->addRow(tr("Duck by:"), amount_);
    form->addRow(tr("Fade down:"), fadeDown_);
    form->addRow(tr("Fade up:"), fadeUp_);
    form->addRow(tr("Speech louder than:"), threshold_);
    form->addRow(QString(), transcripts_);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

DuckOptions AutoDuckDialog::options() const {
    DuckOptions o;
    o.amountDb = amount_->value();
    o.fadeDown = fadeDown_->value();
    o.fadeUp = fadeUp_->value();
    o.thresholdDb = threshold_->value();
    o.useTranscripts = transcripts_->isChecked();
    return o;
}

std::vector<int> AutoDuckDialog::dialogueTracks() const {
    std::vector<int> out;
    for (int i = 0; i < tracks_->count(); ++i)
        if (tracks_->item(i)->checkState() == Qt::Checked) out.push_back(tracks_->item(i)->data(Qt::UserRole).toInt());
    return out;
}

int AutoDuckDialog::apply(EditorState* state, const std::vector<Id>& music, const std::vector<int>& tracks, const DuckOptions& o,
                          QWidget* parent) {
    const Sequence* seq = state->sequence();
    if (!seq || music.empty()) return -1;
    if (tracks.empty()) {
        state->message(tr("Choose the tracks the dialogue is on"), 5000);
        return -1;
    }
    // Reading the dialogue's sound can take a while: in the background, with progress.
    const Project project = state->project();
    const Sequence sequence = *seq;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QProgressDialog progress(tr("Listening for dialogue…"), tr("Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    using Out = std::pair<Spans, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([project, sequence, tracks, o, cancel] {
        Out out;
        out.first = dialogueSpans(project, sequence, tracks, o, &out.second, cancel.get());
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    const Out r = watcher.result();
    if (*cancel) return -1;
    if (!r.second.empty()) {
        state->message(QString::fromStdString(r.second), 6000);
        return -1;
    }
    int changed = 0;
    const Spans spans = r.first;
    state->edit(tr("Auto Duck"), [&](Project&, Sequence& s) {
        for (Id id : music)
            if (Clip* c = edit::clipById(s, id)) changed += duckClip(*c, s, spans, o) ? 1 : 0;
        return changed > 0;
    });
    state->message(spans.empty() ? tr("No dialogue found on those tracks")
                                 : tr("Ducked %n clip(s) under %1 stretch(es) of dialogue", "", changed).arg(spans.size()),
                   5000);
    return changed;
}

}  // namespace montage
