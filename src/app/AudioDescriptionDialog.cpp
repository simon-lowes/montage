#include "AudioDescriptionDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>

#include "EditorState.h"
#include "Settings.h"
#include "SpeechDialog.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "media/AutoDuck.h"
#include "media/TextToSpeech.h"

namespace montage {

namespace {

enum Column { ColStart, ColEnd, ColRoom, ColText, ColFit, ColCount };

QString qs(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

AudioDescriptionDialog::AudioDescriptionDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Audio Description"));
    setObjectName(QStringLiteral("audioDescriptionDialog"));
    resize(760, 520);
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Descriptions of what is seen, spoken in the gaps between the dialogue. Find the gaps, write a "
                                "description in each, then voice them onto the AD track and duck the programme under them."),
                             this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QSettings saved = appSettings();
    auto* find = new QHBoxLayout;
    minGap_ = new QDoubleSpinBox(this);
    minGap_->setObjectName(QStringLiteral("adMinGap"));
    minGap_->setRange(0.5, 30);
    minGap_->setSingleStep(0.5);
    minGap_->setSuffix(tr(" s"));
    minGap_->setValue(saved.value(QStringLiteral("ad/minGap"), 2.0).toDouble());
    pace_ = new QSpinBox(this);
    pace_->setObjectName(QStringLiteral("adPace"));
    pace_->setRange(80, 260);
    pace_->setSuffix(tr(" words/min"));
    pace_->setValue(saved.value(QStringLiteral("ad/pace"), 160).toInt());
    pace_->setToolTip(tr("How fast the descriptions are read, to tell whether one fits its gap"));
    auto* findButton = new QPushButton(tr("Find Gaps"), this);
    findButton->setObjectName(QStringLiteral("adFindGaps"));
    auto* addButton = new QPushButton(tr("Add at Playhead"), this);
    addButton->setObjectName(QStringLiteral("adAdd"));
    find->addWidget(new QLabel(tr("Gaps of at least"), this));
    find->addWidget(minGap_);
    find->addSpacing(12);
    find->addWidget(new QLabel(tr("Pace:"), this));
    find->addWidget(pace_);
    find->addStretch(1);
    find->addWidget(findButton);
    find->addWidget(addButton);
    root->addLayout(find);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setObjectName(QStringLiteral("adTable"));
    table_->setHorizontalHeaderLabels({tr("Start"), tr("End"), tr("Room"), tr("Description"), tr("Fit")});
    table_->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    root->addWidget(table_, 1);

    auto* make = new QHBoxLayout;
    voice_ = new QComboBox(this);
    voice_->setObjectName(QStringLiteral("adVoice"));
    for (const TtsVoice& v : ttsVoices()) voice_->addItem(qs(v.name), qs(v.id));
    voice_->setCurrentIndex(std::max(0, voice_->findData(saved.value(QStringLiteral("ad/voice"), QStringLiteral("bf_emma")).toString())));
    auto* voiceButton = new QPushButton(tr("Voice Descriptions"), this);
    voiceButton->setObjectName(QStringLiteral("adVoiceButton"));
    duckDb_ = new QDoubleSpinBox(this);
    duckDb_->setObjectName(QStringLiteral("adDuckDb"));
    duckDb_->setRange(-30, -1);
    duckDb_->setSuffix(tr(" dB"));
    duckDb_->setValue(saved.value(QStringLiteral("ad/duck"), -9.0).toDouble());
    auto* duckButton = new QPushButton(tr("Duck Programme"), this);
    duckButton->setObjectName(QStringLiteral("adDuck"));
    hear_ = new QCheckBox(tr("Hear descriptions"), this);
    hear_->setObjectName(QStringLiteral("adHear"));
    hear_->setToolTip(tr("Play the descriptions while working; exports put them in the described stream either way"));
    make->addWidget(new QLabel(tr("Voice:"), this));
    make->addWidget(voice_);
    make->addWidget(voiceButton);
    make->addSpacing(12);
    make->addWidget(duckDb_);
    make->addWidget(duckButton);
    make->addStretch(1);
    make->addWidget(hear_);
    root->addLayout(make);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("adStatus"));
    root->addWidget(status_);

    connect(findButton, &QPushButton::clicked, this, [this] {
        QString err;
        if (findGaps(&err) < 0) status_->setText(err);
    });
    connect(addButton, &QPushButton::clicked, this, &AudioDescriptionDialog::addAtPlayhead);
    connect(voiceButton, &QPushButton::clicked, this, [this] {
        QString err;
        if (voice(&err) <= 0 && !err.isEmpty()) status_->setText(err);
    });
    connect(duckButton, &QPushButton::clicked, this, &AudioDescriptionDialog::duck);
    connect(hear_, &QCheckBox::toggled, this, &AudioDescriptionDialog::setHear);
    connect(table_, &QTableWidget::cellChanged, this, &AudioDescriptionDialog::cellEdited);
    connect(pace_, &QSpinBox::valueChanged, this, [this](int v) {
        appSettings().setValue(QStringLiteral("ad/pace"), v);
        refresh();
    });
    connect(minGap_, &QDoubleSpinBox::valueChanged, this, [](double v) { appSettings().setValue(QStringLiteral("ad/minGap"), v); });
    connect(duckDb_, &QDoubleSpinBox::valueChanged, this, [](double v) { appSettings().setValue(QStringLiteral("ad/duck"), v); });
    connect(voice_, &QComboBox::currentIndexChanged, this,
            [this] { appSettings().setValue(QStringLiteral("ad/voice"), voice_->currentData().toString()); });
    connect(state_, &EditorState::projectChanged, this, &AudioDescriptionDialog::refresh);
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        gaps_.clear();
        refresh();
    });
    refresh();
}

int AudioDescriptionDialog::adTrack() const {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    for (size_t i = 0; i < s->audioTracks.size(); ++i)
        if (s->audioTracks[i].name == "AD") return int(i);
    return -1;
}

std::vector<int> AudioDescriptionDialog::dialogueTracks() const {
    std::vector<int> out;
    const Sequence* s = state_->sequence();
    const int ad = adTrack();
    if (s)
        for (int i = 0; i < int(s->audioTracks.size()); ++i)
            if (i != ad) out.push_back(i);
    return out;
}

int AudioDescriptionDialog::rows() const { return table_->rowCount(); }

int AudioDescriptionDialog::findGaps(QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    DuckOptions o;
    o.minPause = 0.5;
    std::string err;
    const Spans speech = dialogueSpans(state_->project(), *s, dialogueTracks(), o, &err);
    QApplication::restoreOverrideCursor();
    if (!err.empty()) {
        if (error) *error = qs(err);
        return -1;
    }
    const FrameTime end = std::max<FrameTime>(s->duration(), 1);
    gaps_ = descriptionGaps(speech, s->fpsValue(), 0, end, minGap_->value());
    refresh();
    status_->setText(tr("%n gap(s) of %1 s or more between the dialogue", "", int(gaps_.size())).arg(minGap_->value()));
    return int(gaps_.size());
}

void AudioDescriptionDialog::refresh() {
    if (refreshing_) return;
    const Sequence* s = state_->sequence();
    refreshing_ = true;
    // The descriptions written, then the gaps found that have none.
    struct Row {
        FrameTime start, end;
        std::string text;
    };
    std::vector<Row> rowsOut;
    const int t = s ? findDescriptionTrack(*s) : -1;
    if (t >= 0)
        for (const Caption& c : s->captionTracks[size_t(t)].captions) rowsOut.push_back({c.start, c.end, c.text});
    for (const DescriptionGap& g : gaps_)
        if (std::none_of(rowsOut.begin(), rowsOut.end(), [&](const Row& r) { return r.start < g.end && r.end > g.start; }))
            rowsOut.push_back({g.start, g.end, {}});
    std::sort(rowsOut.begin(), rowsOut.end(), [](const Row& a, const Row& b) { return a.start < b.start; });
    table_->setRowCount(int(rowsOut.size()));
    int tooLong = 0, written = 0;
    for (int r = 0; r < int(rowsOut.size()) && s; ++r) {
        const Row& row = rowsOut[size_t(r)];
        const double room = double(row.end - row.start) / s->fpsValue();
        auto put = [&](int col, const QString& text, bool editable) {
            auto* it = new QTableWidgetItem(text);
            it->setData(Qt::UserRole, qlonglong(row.start));
            it->setData(Qt::UserRole + 1, qlonglong(row.end));
            if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            table_->setItem(r, col, it);
            return it;
        };
        put(ColStart, qs(formatTimecode(row.start, s->fps)), false);
        put(ColEnd, qs(formatTimecode(row.end, s->fps)), false);
        put(ColRoom, QStringLiteral("%1 s").arg(room, 0, 'f', 1), false);
        put(ColText, qs(row.text), true);
        QString fit;
        if (!row.text.empty()) {
            ++written;
            const DescriptionFit f = descriptionFit(row.text, room, pace_->value());
            if (f.speed <= 1.0) fit = tr("Fits");
            else if (f.fits) fit = tr("Fits a little faster (%1×)").arg(f.speed, 0, 'f', 2);
            else {
                fit = tr("Too long: cut %n word(s)", "", f.overWords);
                ++tooLong;
            }
            QTableWidgetItem* it = put(ColFit, fit, false);
            if (!f.fits) it->setForeground(QColor(230, 90, 70));
        } else {
            put(ColFit, QString(), false);
        }
    }
    refreshing_ = false;
    if (s) {
        const bool heard = !edit::roleMuted(*s, kDescriptionRole);
        const QSignalBlocker block(hear_);
        hear_->setChecked(heard);
    }
    if (written) status_->setText(tooLong ? tr("%1 descriptions, %2 too long for their gaps").arg(written).arg(tooLong)
                                          : tr("%n description(s), all fitting their gaps", "", written));
}

void AudioDescriptionDialog::cellEdited(int row, int column) {
    if (refreshing_ || column != ColText) return;
    const QTableWidgetItem* it = table_->item(row, column);
    if (!it) return;
    const FrameTime start = FrameTime(it->data(Qt::UserRole).toLongLong()), end = FrameTime(it->data(Qt::UserRole + 1).toLongLong());
    const std::string text = it->text().trimmed().toStdString();
    state_->edit(text.empty() ? tr("Remove Description") : tr("Write Description"), [&](Project& p, Sequence& s) {
        return setDescription(p, s, start, end, text);
    });
    QMetaObject::invokeMethod(this, &AudioDescriptionDialog::refresh, Qt::QueuedConnection);
}

void AudioDescriptionDialog::addAtPlayhead() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    const FrameTime at = state_->playhead();
    FrameTime end = at + FrameTime(std::llround(3 * s->fpsValue()));
    for (const DescriptionGap& g : gaps_)
        if (at >= g.start && at < g.end) end = std::min(end, g.end);  // within the gap it is in
    gaps_.push_back({at, end});
    std::sort(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start < b.start; });
    refresh();
    for (int r = 0; r < table_->rowCount(); ++r)
        if (table_->item(r, ColText) && FrameTime(table_->item(r, ColText)->data(Qt::UserRole).toLongLong()) == at) {
            table_->setCurrentCell(r, ColText);
            table_->editItem(table_->item(r, ColText));
        }
}

int AudioDescriptionDialog::voice(QString* error) {
    const Sequence* s = state_->sequence();
    const int t = s ? findDescriptionTrack(*s) : -1;
    std::vector<SpeechLine> lines;
    if (t >= 0)
        for (const Caption& c : s->captionTracks[size_t(t)].captions) {
            SpeechLine l;
            l.text = c.text;
            std::replace(l.text.begin(), l.text.end(), '\n', ' ');
            l.at = c.start;
            l.fit = c.end - c.start;  // its own gap, not up to the next description
            if (!l.text.empty()) lines.push_back(l);
        }
    if (lines.empty()) {
        if (error) *error = tr("Write some descriptions first");
        return 0;
    }
    // What was voiced before goes; the new takes go on the AD track.
    state_->edit(tr("Clear Voiced Descriptions"), [](Project& p, Sequence& sq) {
        std::vector<Id> old;
        for (const Track& tr : sq.audioTracks)
            for (const Clip& c : tr.clips)
                if (c.role == kDescriptionRole) old.push_back(c.id);
        return !old.empty() && edit::removeClips(p, sq, old, false).ok;
    });
    const std::vector<Id> clips = generateSpeech(state_, lines, voice_->currentData().toString().toStdString(), 1.0, adTrack(), this, error);
    if (clips.empty()) return 0;
    state_->edit(tr("Mark Descriptions"), [&](Project&, Sequence& sq) {
        for (Id id : clips)
            if (Clip* c = edit::clipById(sq, id)) {
                c->role = kDescriptionRole;
                if (const auto loc = edit::locate(sq, id)) sq.audioTracks[size_t(loc->track.index)].name = "AD";
            }
        return true;
    });
    status_->setText(tr("Voiced %n description(s) on the AD track", "", int(clips.size())));
    return int(clips.size());
}

int AudioDescriptionDialog::duck() {
    const Sequence* s = state_->sequence();
    const int ad = adTrack();
    if (!s || ad < 0) {
        status_->setText(tr("Voice the descriptions first"));
        return 0;
    }
    DuckOptions o;
    o.amountDb = duckDb_->value();
    o.fadeDown = 0.4;
    o.fadeUp = 0.6;
    int changed = 0;
    state_->edit(tr("Duck Under Descriptions"), [&](Project&, Sequence& sq) {
        const Spans spans = clipSpans(sq, ad, 1.0);
        for (int i = 0; i < int(sq.audioTracks.size()); ++i) {
            if (i == ad) continue;
            for (Clip& c : sq.audioTracks[size_t(i)].clips)
                if (c.role != kDescriptionRole && duckClip(c, sq, spans, o)) ++changed;
        }
        return changed > 0;
    });
    status_->setText(tr("Ducked %n clip(s) under the descriptions", "", changed));
    return changed;
}

void AudioDescriptionDialog::setHear(bool on) {
    state_->edit(on ? tr("Hear Descriptions") : tr("Mute Descriptions"), [on](Project&, Sequence& sq) {
        if (edit::roleMuted(sq, kDescriptionRole) == !on) return false;
        edit::setRoleMuted(sq, kDescriptionRole, !on);
        return true;
    });
}

}  // namespace montage
