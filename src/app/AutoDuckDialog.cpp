#include "AutoDuckDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

#include "DialogJobs.h"
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
    keep_ = new QCheckBox(tr("Keep up to date"), this);
    keep_->setObjectName(QStringLiteral("duckKeep"));
    keep_->setToolTip(tr("The duck remembers its dialogue tracks and these settings. When dialogue under it is added, trimmed,\n"
                         "moved or muted, its keyframes are made again as part of that edit. A keyframe you move by hand is kept."));
    // (as the selected clips are now: on when any of them is kept up to date already)
    keep_->setChecked(s && std::any_of(music.begin(), music.end(), [&](Id id) {
        const Clip* c = edit::clipById(*s, id);
        return c && c->duck;
    }));
    form->addRow(QString(), keep_);
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

bool AutoDuckDialog::keepUpToDate() const { return keep_->isChecked(); }

std::vector<int> AutoDuckDialog::dialogueTracks() const {
    std::vector<int> out;
    for (int i = 0; i < tracks_->count(); ++i)
        if (tracks_->item(i)->checkState() == Qt::Checked) out.push_back(tracks_->item(i)->data(Qt::UserRole).toInt());
    return out;
}

int AutoDuckDialog::apply(EditorState* state, const std::vector<Id>& music, const std::vector<int>& tracks, const DuckOptions& o,
                          QWidget* parent, bool keep) {
    const Sequence* seq = state->sequence();
    if (!seq || music.empty()) return -1;
    if (tracks.empty()) {
        state->message(tr("Choose the tracks the dialogue is on"), 5000);
        return -1;
    }
    // Reading the dialogue's sound can take a while: a background job (DialogJobs.h), watched behind its progress
    // dialog or left to run. Its result, the stretches of speech, is applied as one undo step when it ends.
    QJsonArray clips, trackIds;
    for (Id id : music) clips.append(QString::number(id));
    for (int i : tracks)
        if (i >= 0 && i < int(seq->audioTracks.size())) trackIds.append(QString::number(seq->audioTracks[size_t(i)].id));
    const QJsonObject options{{"amount", o.amountDb},       {"fadeDown", o.fadeDown},   {"fadeUp", o.fadeUp},
                              {"minPause", o.minPause},     {"minSpeech", o.minSpeech}, {"threshold", o.thresholdDb},
                              {"transcripts", o.useTranscripts}};
    const QJsonObject input{{"sequence", QString::number(seq->id)}, {"clips", clips}, {"tracks", trackIds}, {"options", options}, {"keep", keep}};
    const TaskOutcome r = DialogJobs::of(parent)->run(state, QStringLiteral("auto-duck"), tr("Listening for dialogue…"), input, parent);
    if (r.how == TaskOutcome::Failed && !r.error.isEmpty()) state->message(r.error, 6000);
    if (!r.done()) return -1;
    state->message(r.applied.value("message").toString(), 5000);
    return r.applied.value("changed").toInt();
}

}  // namespace montage
