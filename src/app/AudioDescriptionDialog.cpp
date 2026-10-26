#include "AudioDescriptionDialog.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>

#include "EditorState.h"
#include "ModelPacks.h"
#include "Settings.h"
#include "WorkProgress.h"
#include "SpeechDialog.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "render/FootageWords.h"
#include "media/AutoDuck.h"
#include "media/LanguageModel.h"
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
    auto* draftButton = new QPushButton(tr("Draft From the Pictures"), this);
    draftButton->setObjectName(QStringLiteral("adDraft"));
    draftButton->setToolTip(tr("Write a first draft in each empty gap from what the picture shows, on this computer"));
    useModel_ = new QCheckBox(tr("Write with the language model"), this);
    useModel_->setObjectName(QStringLiteral("adUseModel"));
    useModel_->setToolTip(tr("Have the language model word each draft as a describer would; otherwise a brief caption of the picture"));
    useModel_->setChecked(languageModelAvailable() && saved.value(QStringLiteral("ad/useModel"), true).toBool());
    useModel_->setEnabled(languageModelAvailable());
    draftButton->setEnabled(describeAvailable());
    find->addWidget(new QLabel(tr("Gaps of at least"), this));
    find->addWidget(minGap_);
    find->addSpacing(12);
    find->addWidget(new QLabel(tr("Pace:"), this));
    find->addWidget(pace_);
    find->addStretch(1);
    find->addWidget(findButton);
    find->addWidget(addButton);
    root->addLayout(find);
    auto* drafting = new QHBoxLayout;
    drafting->addWidget(draftButton);
    drafting->addWidget(useModel_);
    drafting->addStretch(1);
    root->addLayout(drafting);

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
    connect(draftButton, &QPushButton::clicked, this, [this] {
        QString err;
        if (draft(&err) < 0 && !err.isEmpty()) status_->setText(err);
    });
    connect(useModel_, &QCheckBox::toggled, this, [](bool on) { appSettings().setValue(QStringLiteral("ad/useModel"), on); });
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
    // When the dialogue has been cut since the gaps were found (or those added at the playhead checked), they are
    // found (checked) again once the edits settle and no description is being typed, and when the dialog is shown
    // after edits made while it was hidden, so the table never offers a gap that is now under a line.
    refind_ = new QTimer(this);
    refind_->setSingleShot(true);
    refind_->setInterval(400);
    // (The dialogue is listened to off the UI thread, which a long sequence would hold up; the answer is used if the
    // dialogue is still as it was, else it is listened to again.)
    connect(refind_, &QTimer::timeout, this, [this] {
        if (!isVisible() || !stale()) return;
        if (QWidget* w = QApplication::focusWidget(); w && w != table_ && table_->isAncestorOf(w)) return refind_->start();  // (typing)
        const Sequence* s = state_->sequence();
        if (!s) return;
        if (listening_) {
            listenAgain_ = true;
            return;
        }
        listening_ = true;
        const Project project = state_->project();
        const Sequence sequence = *s;
        const int skip = adTrack();
        const std::string layout = dialogueLayout();
        const bool found = !foundFor_.empty();
        const Id sequenceId = s->id;
        const auto cancel = cancel_;
        auto* watcher = new QFutureWatcher<std::pair<Spans, std::string>>(this);
        connect(watcher, &QFutureWatcher<std::pair<Spans, std::string>>::finished, this, [this, watcher, layout, found, sequenceId, cancel] {
            const auto [speech, err] = watcher->result();
            watcher->deleteLater();
            listening_ = false;
            QString why;
            if (const Sequence* now = state_->sequence();
                now && now->id == sequenceId && !cancel->load() && dialogueLayout() == layout && found == !foundFor_.empty()) {
                if (found ? useFound(speech, err, layout, &why) < 0 : !useAdded(speech, err, layout, &why)) status_->setText(why);
            } else {
                listenAgain_ = true;
            }
            if (std::exchange(listenAgain_, false) && isVisible() && stale()) refind_->start();
        });
        watcher->setFuture(QtConcurrent::run([project, sequence, skip, cancel] {
            std::string err;
            Spans speech = descriptionSpeech(project, sequence, skip, &err, cancel.get());
            return std::make_pair(std::move(speech), err);
        }));
    });
    connect(state_, &EditorState::projectChanged, this, [this] {
        followAdded();
        if (isVisible() && stale()) refind_->start();
        refresh();
    });
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        gaps_.clear();
        added_.clear();
        anchors_.clear();
        foundFor_.clear();
        addedFor_.clear();
        refresh();
    });
    refresh();
}

void AudioDescriptionDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    if (cancel_->load()) cancel_ = std::make_shared<std::atomic<bool>>(false);
    if (stale()) refind_->start();
}

void AudioDescriptionDialog::hideEvent(QHideEvent* e) {
    cancel_->store(true);  // (a listening still going stops; its answer is not used)
    QDialog::hideEvent(e);
}

AudioDescriptionDialog::~AudioDescriptionDialog() { cancel_->store(true); }

bool AudioDescriptionDialog::stale() const {
    if (!foundFor_.empty()) return dialogueLayout() != foundFor_;
    return !added_.empty() && dialogueLayout() != addedFor_;
}

void AudioDescriptionDialog::keepAdded(const std::vector<std::pair<double, double>>& speech, double fps) {
    // A gap added at the playhead stays while it starts clear of the dialogue (ending where the next line now begins)
    // and not inside a gap found anyway (the found one is drafted and voiced instead).
    auto drop = [&](DescriptionGap& a) {
        for (const DescriptionGap& g : gaps_)
            if (a.start >= g.start && a.start < g.end) return true;
        for (const auto& [t0, t1] : speech) {
            if (t0 <= double(a.start) / fps && t1 > double(a.start) / fps) return true;
            if (t0 > double(a.start) / fps) a.end = std::min(a.end, FrameTime(std::floor(t0 * fps)));
        }
        return a.end <= a.start;
    };
    size_t kept = 0;
    for (size_t i = 0; i < added_.size(); ++i)
        if (!drop(added_[i])) {
            added_[kept] = added_[i];
            anchors_[kept++] = anchors_[i];
        }
    added_.resize(kept);
    anchors_.resize(kept);
}

namespace {
// Where a clip's sound would start on the timeline, untrimmed: it moves with the clip and its slips, not its trims.
FrameTime soundOrigin(const Clip& c) { return c.reverse ? c.start : c.start - FrameTime(std::llround(c.sourceIn / c.speed)); }
}  // namespace

AudioDescriptionDialog::Anchor AudioDescriptionDialog::anchorAt(FrameTime at) const {
    Anchor a;
    const Sequence* s = state_->sequence();
    if (!s) return a;
    const int skip = adTrack();
    FrameTime best = -1;
    for (size_t i = 0; i < s->audioTracks.size(); ++i)
        if (int(i) != skip)
            for (const Clip& c : s->audioTracks[i].clips)
                if (c.start <= at && c.start > best) best = c.start, a.clip = c.id;
    if (const Clip* c = a.clip ? edit::clipById(*s, a.clip) : nullptr) a.offset = at - soundOrigin(*c);
    return a;
}

void AudioDescriptionDialog::followAdded() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    bool moved = false;
    for (size_t i = 0; i < added_.size(); ++i) {
        const Clip* c = anchors_[i].clip ? edit::clipById(*s, anchors_[i].clip) : nullptr;
        if (!c) continue;
        const FrameTime shift = soundOrigin(*c) + anchors_[i].offset - added_[i].start;
        if (!shift) continue;
        for (DescriptionGap& g : gaps_)
            if (g == added_[i]) g.start += shift, g.end += shift;
        added_[i].start += shift, added_[i].end += shift;
        moved = true;
    }
    if (moved) std::sort(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start < b.start; });
}

bool AudioDescriptionDialog::checkAdded(QString* error) {
    const Sequence* s = state_->sequence();
    if (!s || added_.empty()) return true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const Spans speech = descriptionSpeech(state_->project(), *s, adTrack(), &err);
    QApplication::restoreOverrideCursor();
    return useAdded(speech, err, dialogueLayout(), error);
}

bool AudioDescriptionDialog::useAdded(const Spans& speech, const std::string& err, const std::string& layout, QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    addedFor_ = layout;
    if (!err.empty()) {
        if (error) *error = qs(err);
        return false;
    }
    gaps_.clear();  // (none were found: those added are all there are)
    keepAdded(speech, s->fpsValue());
    gaps_ = added_;
    std::sort(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start < b.start; });
    refresh();
    return true;
}

int AudioDescriptionDialog::adTrack() const {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    for (size_t i = 0; i < s->audioTracks.size(); ++i)
        if (s->audioTracks[i].name == "AD") return int(i);
    return -1;
}

int AudioDescriptionDialog::rows() const { return table_->rowCount(); }

std::string AudioDescriptionDialog::dialogueLayout() const {
    const Sequence* s = state_->sequence();
    if (!s) return {};
    std::string out;
    const int ad = adTrack();
    for (size_t i = 0; i < s->audioTracks.size(); ++i) {
        if (int(i) == ad) continue;
        const Track& t = s->audioTracks[i];
        out += t.muted ? "M|" : "|";
        for (const Clip& c : t.clips)
            out += std::to_string(c.mediaId) + ":" + std::to_string(c.start) + ":" + std::to_string(c.duration) + ":" +
                   std::to_string(c.sourceIn) + ":" + std::to_string(c.speed) + (c.reverse ? "r" : "") + (c.enabled ? "," : "x,");
    }
    return out;
}

int AudioDescriptionDialog::findGaps(QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const Spans speech = descriptionSpeech(state_->project(), *s, adTrack(), &err);
    QApplication::restoreOverrideCursor();
    return useFound(speech, err, dialogueLayout(), error);
}

int AudioDescriptionDialog::useFound(const Spans& speech, const std::string& err, const std::string& layout, QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    foundFor_ = layout;  // (also when it fails: not tried again until the dialogue changes)
    if (!err.empty()) {
        if (error) *error = qs(err);
        return -1;
    }
    const FrameTime end = std::max<FrameTime>(s->duration(), 1);
    const double fps = s->fpsValue();
    gaps_ = descriptionGaps(speech, fps, 0, end, minGap_->value());
    keepAdded(speech, fps);
    gaps_.insert(gaps_.end(), added_.begin(), added_.end());
    std::sort(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start < b.start || (a.start == b.start && a.end < b.end); });
    gaps_.erase(std::unique(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start == b.start && a.end == b.end; }),
                gaps_.end());
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
    // A row already covering the playhead is the one to write in (two descriptions in one gap would talk over each
    // other); else a new one from the playhead, within the gap it is in.
    auto rowAt = [&] {
        for (int r = 0; r < table_->rowCount(); ++r)
            if (const QTableWidgetItem* it = table_->item(r, ColText);
                it && FrameTime(it->data(Qt::UserRole).toLongLong()) <= at && at < FrameTime(it->data(Qt::UserRole + 1).toLongLong()))
                return r;
        return -1;
    };
    if (rowAt() < 0) {
        FrameTime end = at + FrameTime(std::llround(3 * s->fpsValue()));
        for (const DescriptionGap& g : gaps_)
            if (at >= g.start && at < g.end) end = std::min(end, g.end);
        gaps_.push_back({at, end});
        added_.push_back({at, end});
        anchors_.push_back(anchorAt(at));
        std::sort(gaps_.begin(), gaps_.end(), [](const DescriptionGap& a, const DescriptionGap& b) { return a.start < b.start; });
        refresh();
        // (With no gaps found, checked against the dialogue now: kept clear of it, as a found gap would be.)
        QString err;
        if (foundFor_.empty() && !checkAdded(&err)) status_->setText(err);
        else if (rowAt() < 0) status_->setText(tr("The playhead is in the dialogue"));
    }
    if (const int r = rowAt(); r >= 0) {
        table_->setCurrentCell(r, ColText);
        table_->editItem(table_->item(r, ColText));
    }
}

int AudioDescriptionDialog::draft(QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return -1;
    // (Found again: the dialogue may have been cut since they were last found. Those found before, when the dialogue
    // cannot be heard now.)
    QString why;
    if (findGaps(&why) < 0 && gaps_.empty()) {
        if (error) *error = why;
        return -1;
    }
    // The gaps with no description yet.
    std::vector<DescriptionGap> empty;
    const int t = findDescriptionTrack(*s);
    for (const DescriptionGap& g : gaps_)
        if (t < 0 || std::none_of(s->captionTracks[size_t(t)].captions.begin(), s->captionTracks[size_t(t)].captions.end(),
                                  [&](const Caption& c) { return c.start < g.end && c.end > g.start; }))
            empty.push_back(g);
    if (empty.empty()) {
        if (error) *error = gaps_.empty() ? tr("No gaps between the dialogue to describe") : tr("Every gap has a description");
        return -1;
    }
    if (!describeAvailable()) {
        if (error) *error = tr("This build of Montage cannot describe pictures: it was built without ONNX Runtime.");
        return -1;
    }
    const bool withModel = useModel_->isChecked() && languageModelAvailable();
    if (!ensureModelPack(this, describeModel(), tr("Draft Audio Description"),
                         tr("Drafting descriptions puts each picture into words on this computer.")))
        return -1;
    if (withModel && !ensureModelPack(this, languageModel(), tr("Draft Audio Description"),
                                      tr("The language model words each draft as a describer would, on this computer.")))
        return -1;
    struct Outcome {
        std::vector<DescriptionDraft> drafts;
        std::string error;
    };
    const Project project = state_->project();
    const Sequence seq = *s;
    const double pace = pace_->value();
    const Outcome o = withProgress<Outcome>(this, tr("Drafting descriptions…"), [&](const auto& progress) {
        Outcome out;
        std::atomic<bool> stop{false};
        auto describer = PictureDescriber::load(&out.error);
        if (!describer) return out;
        std::shared_ptr<LanguageModel> lm;
        if (withModel && !(lm = LanguageModel::load({}, &out.error))) return out;
        DraftOptions opts;
        opts.wordsPerMinute = pace;
        out.drafts = draftDescriptions(project, seq, empty, *describer, lm.get(), opts, [&](double f) { stop = !progress(f, QString()); },
                                       &stop, &out.error);
        return out;
    });
    if (o.drafts.empty()) {
        if (error) *error = o.error == "Cancelled" ? QString() : qs(o.error);
        return -1;
    }
    state_->edit(tr("Draft Descriptions"), [&](Project& p, Sequence& sq) {
        bool any = false;
        for (const DescriptionDraft& d : o.drafts) any = setDescription(p, sq, d.start, d.end, d.text) || any;
        return any;
    });
    refresh();
    status_->setText(tr("Drafted %n description(s) from the pictures: read each one through and correct it", "", int(o.drafts.size())));
    return int(o.drafts.size());
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
    // On a lane of its own, heard only with the descriptions: the mix without them is not dipped.
    int changed = 0;
    state_->edit(tr("Duck Under Descriptions"), [&](Project&, Sequence& sq) {
        changed = duckUnderDescriptions(sq, duckDb_->value(), 0.25);
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
