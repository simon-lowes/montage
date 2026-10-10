#include "AdrPanel.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "Settings.h"
#include "Voiceover.h"
#include "core/History.h"

namespace montage {

namespace {

enum Column { CueName, CueCharacter, CueStart, CueEnd, CueLine, CueNote, CueStatus, CueTakes, CueColumns };

QString qs(const std::string& s) { return QString::fromStdString(s); }

// A table that says when a cell is being edited (the list is not rebuilt under an open editor).
class CueTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    bool editing() const { return state() == QAbstractItemView::EditingState; }
};

}  // namespace

AdrPanel::AdrPanel(EditorState* state, PlaybackController* program, ViewerWidget* viewer, QWidget* parent)
    : QWidget(parent), state_(state), program_(program), recorder_(new VoiceoverRecorder(state, this)) {
    setObjectName(QStringLiteral("adrPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    // The cue list and where cues come from.
    auto* tools = new QHBoxLayout;
    auto button = [&](const QString& text, const char* name, const QString& tip, auto fn) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        tools->addWidget(b);
        return b;
    };
    button(tr("Add Cue"), "adrAddCue", tr("A cue from In to Out, or two seconds from the playhead"), [this] { addCue(); });
    button(tr("From Captions"), "adrFromCaptions", tr("A cue for each caption (between In and Out when marked); a speaker label is the character"),
           [this] { addCuesFromCaptions(); });
    button(tr("From Markers"), "adrFromMarkers", tr("A cue for each range marker (between In and Out when marked)"), [this] { addCuesFromMarkers(); });
    button(tr("Import…"), "adrImport", tr("Cues from a cue sheet (CSV or tab separated)"), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Cue Sheet"), QString(), tr("Cue sheets (*.csv *.tsv *.txt);;All files (*)"));
        QString err;
        if (!path.isEmpty() && !importCueSheet(path, &err)) state_->message(err, 6000);
    });
    button(tr("Export…"), "adrExport", tr("The cue sheet, for the stage and the actor"), [this] {
        const Sequence* s = state_->sequence();
        const QString name = (s ? qs(s->name) : tr("Sequence")) + tr(" ADR.csv");
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Cue Sheet"), name, tr("Cue sheet (*.csv)"));
        QString err;
        if (!path.isEmpty() && !exportCueSheet(path, &err)) state_->message(err, 6000);
    });
    button(tr("Remove"), "adrRemove", tr("Remove the selected cue (its takes stay on the timeline)"), [this] { removeCue(); });
    tools->addStretch(1);
    root->addLayout(tools);

    table_ = new CueTable(0, CueColumns, this);
    table_->setObjectName(QStringLiteral("adrCues"));
    table_->setHorizontalHeaderLabels({tr("Cue"), tr("Character"), tr("Start"), tr("End"), tr("Line"), tr("Note"), tr("Status"), tr("Takes")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(CueLine, QHeaderView::Stretch);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    root->addWidget(table_, 1);
    connect(table_, &QTableWidget::cellChanged, this, &AdrPanel::cellEdited);
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row, int, int previous, int) {
        if (refreshing_ || row == previous) return;
        refreshTakes();
        updateButtons();
        // The picture goes to the line (not while a cycle runs).
        const Sequence* s = state_->sequence();
        if (const AdrCue* q = s && !cycle_ ? findAdrCue(*s, currentCue()) : nullptr) program_->seek(q->start);
    });
    connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (!currentCue()) return;
        QMenu menu(this);
        menu.addAction(tr("Rehearse"), this, &AdrPanel::rehearse);
        menu.addAction(tr("Record"), this, &AdrPanel::record);
        QMenu* status = menu.addMenu(tr("Status"));
        for (int k = kAdrToRecord; k <= kAdrOmitted; ++k) status->addAction(qs(adrStatusName(k)), this, [this, k] { setCueStatus(k); });
        menu.addSeparator();
        menu.addAction(tr("Remove Cue"), this, &AdrPanel::removeCue);
        menu.exec(table_->viewport()->mapToGlobal(at));
    });

    // The selected cue: its status and its takes.
    auto* cueRow = new QHBoxLayout;
    status_ = new QComboBox(this);
    status_->setObjectName(QStringLiteral("adrStatus"));
    for (int k = kAdrToRecord; k <= kAdrOmitted; ++k) status_->addItem(qs(adrStatusName(k)));
    takes_ = new QComboBox(this);
    takes_->setObjectName(QStringLiteral("adrTakes"));
    takes_->setToolTip(tr("The cue's takes: the one chosen plays (an audition, like any other)"));
    takes_->setMinimumContentsLength(16);
    cueRow->addWidget(new QLabel(tr("Status:"), this));
    cueRow->addWidget(status_);
    cueRow->addSpacing(12);
    cueRow->addWidget(new QLabel(tr("Take:"), this));
    cueRow->addWidget(takes_, 1);
    root->addLayout(cueRow);
    connect(status_, &QComboBox::activated, this, [this](int k) { setCueStatus(k); });
    connect(takes_, &QComboBox::activated, this, [this](int k) { pickTake(k); });

    // How a cycle goes.
    QSettings saved = appSettings();
    auto* grid = new QGridLayout;
    auto seconds = [&](const char* name, const char* key, double value, double lo, double hi, const QString& tip) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(QLatin1String(name));
        box->setRange(lo, hi);
        box->setSingleStep(0.5);
        box->setDecimals(1);
        box->setSuffix(tr(" s"));
        box->setValue(saved.value(QStringLiteral("adr/") + QLatin1String(key), value).toDouble());
        box->setToolTip(tip);
        connect(box, &QDoubleSpinBox::valueChanged, this, [key](double v) { appSettings().setValue(QStringLiteral("adr/") + QLatin1String(key), v); });
        return box;
    };
    preRoll_ = seconds("adrPreRoll", "preRoll", 4, 0, 30, tr("Picture before the line (never less than the beeps need)"));
    postRoll_ = seconds("adrPostRoll", "postRoll", 1, 0, 10, tr("Picture after the line"));
    streamer_ = seconds("adrStreamer", "streamer", 2, 0, 5, tr("How long the streamer takes to cross the picture, reaching the edge on the line (0 = none)"));
    beeps_ = new QSpinBox(this);
    beeps_->setObjectName(QStringLiteral("adrBeeps"));
    beeps_->setRange(0, 3);
    beeps_->setValue(saved.value(QStringLiteral("adr/beeps"), 3).toInt());
    beeps_->setToolTip(tr("Beeps a second apart before the line, which starts where the next would be"));
    connect(beeps_, &QSpinBox::valueChanged, this, [](int v) { appSettings().setValue(QStringLiteral("adr/beeps"), v); });
    input_ = new QComboBox(this);
    input_->setObjectName(QStringLiteral("adrInput"));
    input_->addItems(VoiceoverRecorder::inputs());
    guide_ = new QComboBox(this);
    guide_->setObjectName(QStringLiteral("adrGuide"));
    guide_->setToolTip(tr("The production sound the actor follows"));
    muteGuide_ = new QCheckBox(tr("Mute while recording"), this);
    muteGuide_->setObjectName(QStringLiteral("adrMuteGuide"));
    muteGuide_->setChecked(saved.value(QStringLiteral("adr/muteGuide"), true).toBool());
    connect(muteGuide_, &QCheckBox::toggled, this, [](bool on) { appSettings().setValue(QStringLiteral("adr/muteGuide"), on); });
    track_ = new QComboBox(this);
    track_->setObjectName(QStringLiteral("adrTrack"));
    track_->setToolTip(tr("Where a cue's first take goes; later takes join it"));
    loop_ = new QCheckBox(tr("Loop: record take after take"), this);
    loop_->setObjectName(QStringLiteral("adrLoop"));
    grid->addWidget(new QLabel(tr("Pre-roll:"), this), 0, 0);
    grid->addWidget(preRoll_, 0, 1);
    grid->addWidget(new QLabel(tr("Beeps:"), this), 0, 2);
    grid->addWidget(beeps_, 0, 3);
    grid->addWidget(new QLabel(tr("Post-roll:"), this), 1, 0);
    grid->addWidget(postRoll_, 1, 1);
    grid->addWidget(new QLabel(tr("Streamer:"), this), 1, 2);
    grid->addWidget(streamer_, 1, 3);
    grid->addWidget(new QLabel(tr("Input:"), this), 2, 0);
    grid->addWidget(input_, 2, 1, 1, 3);
    grid->addWidget(new QLabel(tr("Guide:"), this), 3, 0);
    grid->addWidget(guide_, 3, 1);
    grid->addWidget(muteGuide_, 3, 2, 1, 2);
    grid->addWidget(new QLabel(tr("Takes to:"), this), 4, 0);
    grid->addWidget(track_, 4, 1);
    grid->addWidget(loop_, 4, 2, 1, 2);
    root->addLayout(grid);

    // The transport.
    auto* transport = new QHBoxLayout;
    rehearse_ = new QPushButton(tr("Rehearse"), this);
    rehearse_->setObjectName(QStringLiteral("adrRehearse"));
    rehearse_->setToolTip(tr("Play the cycle with its beeps and streamer, without recording"));
    record_ = new QPushButton(tr("Record"), this);
    record_->setObjectName(QStringLiteral("adrRecord"));
    record_->setToolTip(tr("Play the cycle and record a take"));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("adrStop"));
    stop_->setToolTip(tr("Stop (a take being recorded is kept)"));
    meter_ = new QProgressBar(this);
    meter_->setRange(0, 1000);
    meter_->setTextVisible(false);
    meter_->setMaximumHeight(10);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("adrState"));
    transport->addWidget(rehearse_);
    transport->addWidget(record_);
    transport->addWidget(stop_);
    transport->addWidget(meter_, 1);
    root->addLayout(transport);
    root->addWidget(statusLabel_);
    connect(rehearse_, &QPushButton::clicked, this, &AdrPanel::rehearse);
    connect(record_, &QPushButton::clicked, this, &AdrPanel::record);
    connect(stop_, &QPushButton::clicked, this, &AdrPanel::stop);

    connect(recorder_, &VoiceoverRecorder::level, this, [this](float peak) {
        const double db = peak > 0 ? 20 * std::log10(peak) : -60;
        meter_->setValue(int(std::clamp((db + 60) / 60, 0.0, 1.0) * 1000));
    });
    connect(recorder_, &VoiceoverRecorder::taken, this, &AdrPanel::onTaken);
    connect(program_, &PlaybackController::positionChanged, this, &AdrPanel::onPosition);

    // Edits arrive in bursts; the list follows a moment later (never under an open cell editor).
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(0);
    connect(refreshTimer_, &QTimer::timeout, this, &AdrPanel::refresh);
    connect(state_, &EditorState::projectChanged, refreshTimer_, qOverload<>(&QTimer::start));
    // Another sequence or project: a take being recorded belongs to neither, so it is dropped.
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        if (recorder_->isRecording()) {
            recorder_->cancel();
            state_->message(tr("ADR take dropped: the sequence changed while recording"), 6000);
        }
        if (cycle_) endCycle();
        refresh();
    });
    // Playback stopping (Space, the sequence's end, a shuttle) ends a cycle as reaching its end does.
    connect(program_, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (!playing) playbackEnded();
    });

    if (viewer) {
        QPointer<AdrPanel> self(this);
        viewer->addOverlay([self](QPainter& p, const QRectF& r) {
            if (self) self->paintOverlay(p, r);
        });
    }
    refresh();
}

AdrPanel::~AdrPanel() {
    if (recorder_->isRecording()) recorder_->cancel();
}

Id AdrPanel::currentCue() const {
    const int row = table_->currentRow();
    const QTableWidgetItem* it = row >= 0 ? table_->item(row, CueName) : nullptr;
    return it ? Id(it->data(Qt::UserRole).toULongLong()) : 0;
}

void AdrPanel::selectCue(Id id) {
    for (int r = 0; r < table_->rowCount(); ++r)
        if (const QTableWidgetItem* it = table_->item(r, CueName); it && Id(it->data(Qt::UserRole).toULongLong()) == id) {
            table_->setCurrentCell(r, CueName);
            return;
        }
}

AdrSettings AdrPanel::settings() const {
    AdrSettings st;
    st.preRoll = preRoll_->value();
    st.postRoll = postRoll_->value();
    st.beeps = beeps_->value();
    st.streamer = streamer_->value();
    return st;
}

bool AdrPanel::isRecording() const { return recorder_->isRecording(); }

void AdrPanel::refresh() {
    if (static_cast<CueTable*>(table_)->editing()) {
        refreshTimer_->start(200);
        return;
    }
    const Sequence* s = state_->sequence();
    const Id keep = currentCue();
    refreshing_ = true;
    table_->setRowCount(s ? int(s->adrCues.size()) : 0);
    if (s) {
        for (int r = 0; r < int(s->adrCues.size()); ++r) {
            const AdrCue& q = s->adrCues[size_t(r)];
            const QColor ink = q.status == kAdrOmitted ? palette().color(QPalette::Disabled, QPalette::Text)
                                                       : palette().color(QPalette::Text);
            auto put = [&](int col, const QString& text, bool editable) {
                auto* it = new QTableWidgetItem(text);
                it->setData(Qt::UserRole, qulonglong(q.id));
                if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
                it->setForeground(q.status == kAdrApproved && col == CueStatus ? QColor(70, 170, 90) : ink);
                table_->setItem(r, col, it);
            };
            put(CueName, qs(q.name), true);
            put(CueCharacter, qs(q.character), true);
            put(CueStart, qs(formatTimecode(q.start, s->fps)), true);
            put(CueEnd, qs(formatTimecode(q.end, s->fps)), true);
            put(CueLine, qs(q.line), true);
            put(CueNote, qs(q.note), true);
            put(CueStatus, qs(adrStatusName(q.status)), false);
            put(CueTakes, QString::number(adrTakeCount(*s, q)), false);
        }
        // The tracks a guide can be and takes can go to.
        QStringList names;
        for (size_t i = 0; i < s->audioTracks.size(); ++i)
            names << (s->audioTracks[i].name.empty() ? QStringLiteral("A%1").arg(i + 1) : qs(s->audioTracks[i].name));
        QStringList guideItems = QStringList{tr("None")} + names, trackItems = QStringList{tr("ADR track")} + names;
        auto sync = [](QComboBox* box, const QStringList& items, int fallback) {
            QStringList now;
            for (int i = 0; i < box->count(); ++i) now << box->itemText(i);
            if (now == items) return;
            const int was = box->currentIndex();
            box->clear();
            box->addItems(items);
            box->setCurrentIndex(was >= 0 && was < items.size() ? was : std::min(fallback, int(items.size()) - 1));
        };
        int firstNonAdr = 1;  // the guide: the first track that is not the ADR track
        for (int i = 0; i < int(s->audioTracks.size()); ++i)
            if (s->audioTracks[size_t(i)].name != "ADR") {
                firstNonAdr = i + 1;
                break;
            }
        sync(guide_, guideItems, firstNonAdr);
        sync(track_, trackItems, 0);
    }
    table_->resizeColumnToContents(CueName);
    refreshing_ = false;
    if (keep) selectCue(keep);
    if (!currentCue() && table_->rowCount() > 0) {
        refreshing_ = true;
        table_->setCurrentCell(0, CueName);
        refreshing_ = false;
    }
    refreshTakes();
    updateButtons();
}

void AdrPanel::refreshTakes() {
    const Sequence* s = state_->sequence();
    const AdrCue* q = s ? findAdrCue(*s, currentCue()) : nullptr;
    const Clip* c = q && q->clip ? edit::clipById(*s, q->clip) : nullptr;
    takes_->blockSignals(true);
    takes_->clear();
    if (c && c->takes.empty()) {
        takes_->addItem(c->name.empty() ? tr("Take 1") : qs(c->name));
    } else if (c) {
        for (size_t i = 0; i < c->takes.size(); ++i) {
            const std::string& name = int(i) == c->take ? c->name : c->takes[i].name;
            takes_->addItem(QStringLiteral("%1. %2").arg(i + 1).arg(qs(name)));
        }
        takes_->setCurrentIndex(c->take);
    }
    takes_->setEnabled(c && c->takes.size() > 1 && !cycle_);
    takes_->blockSignals(false);
    status_->blockSignals(true);
    status_->setCurrentIndex(q ? q->status : 0);
    status_->setEnabled(q != nullptr);
    status_->blockSignals(false);
}

void AdrPanel::updateButtons() {
    const bool cue = currentCue() != 0;
    rehearse_->setEnabled(cue && !cycle_);
    record_->setEnabled(cue && !cycle_ && (input_->count() > 0 || !useDevice_));
    stop_->setEnabled(cycle_.has_value() || recorder_->isRecording());
    if (!cycle_) statusLabel_->setText(cue ? tr("Ready") : tr("Add cues: from captions, range markers, In/Out, or a cue sheet"));
}

Id AdrPanel::addCue() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    AdrCue q;
    if (s->inPoint >= 0 && s->outPoint > s->inPoint) {
        q.start = s->inPoint;
        q.end = s->outPoint + 1;
    } else {
        q.start = state_->playhead();
        q.end = q.start + std::max<FrameTime>(1, FrameTime(std::llround(2 * s->fpsValue())));
    }
    std::vector<Id> ids;
    state_->apply(tr("Add ADR Cue"), [&](Project& p, Sequence& sq) {
        ids = addAdrCues(p, sq, {q});
        return edit::Result{};
    });
    refresh();
    if (!ids.empty()) selectCue(ids.front());
    return ids.empty() ? 0 : ids.front();
}

int AdrPanel::addCuesFromCaptions() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const bool marked = s->inPoint >= 0 && s->outPoint > s->inPoint;
    std::vector<AdrCue> cues = adrCuesFromCaptions(*s, -1, marked ? s->inPoint : 0, marked ? s->outPoint : -1);
    if (cues.empty()) {
        state_->message(marked ? tr("No captions between In and Out") : tr("The sequence has no captions"));
        return 0;
    }
    std::vector<Id> ids;
    state_->apply(tr("ADR Cues from Captions"), [&](Project& p, Sequence& sq) {
        ids = addAdrCues(p, sq, cues);
        return edit::Result{};
    });
    refresh();
    if (!ids.empty()) selectCue(ids.front());
    state_->message(tr("Added %n ADR cue(s)", "", int(ids.size())));
    return int(ids.size());
}

int AdrPanel::addCuesFromMarkers() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const bool marked = s->inPoint >= 0 && s->outPoint > s->inPoint;
    std::vector<AdrCue> cues = adrCuesFromMarkers(*s, marked ? s->inPoint : 0, marked ? s->outPoint : -1);
    if (cues.empty()) {
        state_->message(tr("No range markers (markers with a duration) to make cues from"));
        return 0;
    }
    std::vector<Id> ids;
    state_->apply(tr("ADR Cues from Markers"), [&](Project& p, Sequence& sq) {
        ids = addAdrCues(p, sq, cues);
        return edit::Result{};
    });
    refresh();
    if (!ids.empty()) selectCue(ids.front());
    state_->message(tr("Added %n ADR cue(s)", "", int(ids.size())));
    return int(ids.size());
}

bool AdrPanel::importCueSheet(const QString& path, QString* error) {
    const Sequence* s = state_->sequence();
    QFile f(path);
    if (!s || !f.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot read %1").arg(path);
        return false;
    }
    std::vector<AdrCue> cues;
    std::string err;
    if (!parseAdrCueSheet(f.readAll().toStdString(), *s, cues, &err)) {
        if (error) *error = qs(err);
        return false;
    }
    std::vector<Id> ids;
    state_->apply(tr("Import ADR Cue Sheet"), [&](Project& p, Sequence& sq) {
        ids = addAdrCues(p, sq, cues);
        return edit::Result{};
    });
    refresh();
    state_->message(tr("Read %n ADR cue(s) from %1", "", int(ids.size())).arg(QFileInfo(path).fileName()));
    return true;
}

bool AdrPanel::exportCueSheet(const QString& path, QString* error) {
    const Sequence* s = state_->sequence();
    QFile f(path);
    if (!s || !f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = tr("Cannot write %1").arg(path);
        return false;
    }
    const std::string csv = adrCueSheetCsv(*s);
    return f.write(csv.data(), qint64(csv.size())) == qint64(csv.size());
}

void AdrPanel::removeCue() {
    const Id id = currentCue();
    if (!id || cycle_) return;
    state_->apply(tr("Remove ADR Cue"), [&](Project&, Sequence& sq) {
        return removeAdrCue(sq, id) ? edit::Result{} : edit::Result::fail(tr("No such cue").toStdString());
    });
    refresh();
}

void AdrPanel::setCueStatus(int status) {
    const Id id = currentCue();
    if (!id) return;
    state_->apply(tr("Set ADR Cue Status"), [&](Project&, Sequence& sq) {
        AdrCue* q = findAdrCue(sq, id);
        if (!q) return edit::Result::fail(tr("No such cue").toStdString());
        q->status = std::clamp(status, 0, int(kAdrOmitted));
        return edit::Result{};
    });
    refresh();
}

void AdrPanel::cellEdited(int row, int column) {
    if (refreshing_) return;
    const QTableWidgetItem* it = table_->item(row, column);
    if (!it) return;
    const Id id = Id(it->data(Qt::UserRole).toULongLong());
    const std::string text = it->text().trimmed().toStdString();
    const bool ok = state_->apply(tr("Edit ADR Cue"), [&](Project&, Sequence& sq) {
        AdrCue* q = findAdrCue(sq, id);
        if (!q) return edit::Result::fail(tr("No such cue").toStdString());
        FrameTime t = 0;
        switch (column) {
            case CueName:
                if (text.empty()) return edit::Result::fail(tr("A cue needs a number").toStdString());
                for (const AdrCue& o : sq.adrCues)
                    if (o.id != id && o.name == text) return edit::Result::fail(tr("Another cue has that number").toStdString());
                q->name = text;
                break;
            case CueCharacter: q->character = text; break;
            case CueLine: q->line = text; break;
            case CueNote: q->note = text; break;
            case CueStart:
                if (!parseTimecode(text, sq.fps, t) || t < 0 || t >= q->end) return edit::Result::fail(tr("The start must be a timecode before the end").toStdString());
                q->start = t;
                std::stable_sort(sq.adrCues.begin(), sq.adrCues.end(), [](const AdrCue& a, const AdrCue& b) { return a.start < b.start; });
                break;
            case CueEnd:
                if (!parseTimecode(text, sq.fps, t) || t <= q->start) return edit::Result::fail(tr("The end must be a timecode after the start").toStdString());
                q->end = t;
                break;
            default: return edit::Result::fail(tr("That column cannot be edited").toStdString());
        }
        return edit::Result{};
    });
    if (!ok) refreshTimer_->start();  // put the cell back
}

std::vector<int> AdrPanel::mutedWhileRecording() const {
    std::vector<int> out;
    const Sequence* s = state_->sequence();
    if (!s) return out;
    if (muteGuide_->isChecked() && guide_->currentIndex() > 0) out.push_back(guide_->currentIndex() - 1);
    // The takes already made are not heard over the new one.
    if (const AdrCue* q = findAdrCue(*s, cycleCue_); q && q->clip)
        if (const auto loc = edit::locate(*s, q->clip)) out.push_back(loc->track.index);
    return out;
}

void AdrPanel::rehearse() { startCycle(false); }
void AdrPanel::record() { startCycle(true); }

void AdrPanel::startCycle(bool record) {
    if (cycle_ || recorder_->isRecording()) return;
    const Sequence* s = state_->sequence();
    const AdrCue* q = s ? findAdrCue(*s, currentCue()) : nullptr;
    if (!q) {
        state_->message(tr("Pick a cue first"));
        return;
    }
    const AdrCycle c = adrCycle(*s, *q, settings());
    if (!c.valid()) return;
    cycleCue_ = q->id;
    recording_ = record;
    stopRequested_ = false;
    graceArmed_ = false;
    ++cycleSerial_;
    cycleText_ = q->character.empty() ? qs(q->line) : qs(q->character) + QStringLiteral(": ") + qs(q->line);
    const QString cueName = qs(q->name);
    if (record) {
        recorder_->setPlacing(false);
        QString stem = cueName;  // a file name: no path separators or characters Windows refuses
        for (QChar& ch : stem)
            if (QStringLiteral("/\\:*?\"<>|").contains(ch)) ch = '-';
        recorder_->setTakeNaming(QStringLiteral("ADR"), stem + QStringLiteral(" take "));
        if (!recorder_->start(c.playFrom, 0, c.playTo, input_->count() ? input_->currentText() : QString(), useDevice_)) {
            statusLabel_->setText(tr("Could not start recording"));
            return;
        }
        program_->setMutedAudioTracks(mutedWhileRecording());
    }
    // Forward at normal speed (not a shuttle), once through (not looping round In to Out).
    if (program_->isPlaying()) program_->pause();
    loopWas_ = program_->loop();
    program_->setLoop(false);
    cycle_ = c;
    lastPosition_ = c.playFrom;
    program_->setAdrCycle(c);
    program_->seek(c.playFrom);
    program_->play();
    statusLabel_->setText(record ? tr("Recording %1, take %2").arg(cueName).arg(adrTakeCount(*s, *q) + 1) : tr("Rehearsing %1").arg(cueName));
    refreshTakes();
    updateButtons();
}

void AdrPanel::endCycle() {
    cycle_.reset();  // first: pausing below reports playback stopping, which must find no cycle
    ++cycleSerial_;
    recording_ = false;
    program_->setAdrCycle(std::nullopt);
    program_->setMutedAudioTracks({});
    if (program_->isPlaying()) program_->pause();
    program_->setLoop(loopWas_);
    meter_->setValue(0);
    refresh();
}

void AdrPanel::onPosition(FrameTime t) {
    if (!cycle_) return;
    // Past the post-roll, or jumped back (playback looping round): the pass is over.
    const bool back = t + 1 < lastPosition_;
    lastPosition_ = std::max(lastPosition_, t);
    if (t >= cycle_->playTo || back) playbackEnded();
}

void AdrPanel::playbackEnded() {
    if (!cycle_) return;
    if (!recording_) {
        endCycle();
        return;
    }
    if (program_->isPlaying()) program_->pause();  // (comes back here, and finds the stop already armed)
    // The recording stops itself at the end of the cycle; should its input lag behind the picture, a moment later.
    if (!graceArmed_) {
        graceArmed_ = true;
        QTimer::singleShot(1500, this, [this, serial = cycleSerial_] {
            graceArmed_ = false;
            if (cycle_ && cycleSerial_ == serial && recorder_->isRecording()) recorder_->stop();
        });
    }
}

void AdrPanel::onTaken(Id media, FrameTime at) {
    if (!cycle_ || !recording_) return;  // not one of ours
    const Id cue = cycleCue_;
    const int track = track_->currentIndex() - 1;  // -1: the ADR track
    Id clip = 0;
    std::string why;
    const bool kept = state_->apply(tr("Record ADR Take"), [&](Project& p, Sequence& sq) {
        edit::Result r = edit::addAdrTake(p, sq, cue, media, at, track);
        if (const AdrCue* q = r.ok ? findAdrCue(sq, cue) : nullptr) clip = q->clip;
        why = r.error;
        return r;
    });
    if (!kept) state_->message(tr("Take not kept: %1").arg(QString::fromStdString(why)), 6000);
    const bool again = loop_->isChecked() && !stopRequested_;
    endCycle();
    emit takeRecorded(cue, clip);
    if (again)
        QTimer::singleShot(loopGap_, this, [this, cue] {
            if (!cycle_ && !stopRequested_ && loop_->isChecked() && currentCue() == cue) startCycle(true);
        });
}

void AdrPanel::stop() {
    stopRequested_ = true;
    if (recorder_->isRecording()) recorder_->stop();  // what was recorded is kept (onTaken ends the cycle)
    if (cycle_) endCycle();
    updateButtons();
}

void AdrPanel::pickTake(int index) {
    const Sequence* s = state_->sequence();
    const AdrCue* q = s ? findAdrCue(*s, currentCue()) : nullptr;
    if (!q || !q->clip || cycle_) return;
    const Id clip = q->clip;
    state_->apply(tr("Pick Take"), [&](Project& p, Sequence& sq) { return edit::pickTake(p, sq, clip, index); });
    refresh();
}

void AdrPanel::paintOverlay(QPainter& p, const QRectF& r) {
    // The cycle the Program monitor is playing.
    const std::optional<AdrCycle>& cyc = program_->adrCycle();
    if (!cyc) return;
    const FrameTime t = program_->position();
    if (t < cyc->playFrom || t > cyc->playTo) return;
    p.save();
    // The streamer: a white bar crossing the picture with a fading trail, at the right edge on the line.
    const double pos = adrStreamerPosition(*cyc, t);
    if (pos >= 0) {
        const double w = std::max(3.0, r.width() * 0.012), x = r.left() + pos * r.width();
        const double trail = std::min(6 * w, x - r.left());
        if (trail > 0) {
            QLinearGradient g(x - trail, 0, x, 0);
            g.setColorAt(0, QColor(255, 255, 255, 0));
            g.setColorAt(1, QColor(255, 255, 255, 90));
            p.fillRect(QRectF(x - trail, r.top(), trail, r.height()), g);
        }
        p.fillRect(QRectF(std::max(r.left(), x - w), r.top(), w, r.height()), QColor(255, 255, 255, 230));
    }
    // The punch on the line's first frames.
    if (adrPunch(*cyc, t)) {
        const double rad = r.height() * 0.12;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 210));
        p.drawEllipse(r.center(), rad, rad);
    }
    // The line, white before it is due and yellow while it is said.
    if (!cycleText_.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(std::max(10, int(r.height() * 0.045)));
        f.setBold(true);
        p.setFont(f);
        const QRectF box(r.left() + r.width() * 0.05, r.bottom() - r.height() * 0.16, r.width() * 0.9, r.height() * 0.11);
        p.fillRect(box, QColor(0, 0, 0, 150));
        p.setPen(t >= cyc->lineFrom && t < cyc->lineTo ? QColor(255, 220, 60) : QColor(240, 240, 240));
        p.drawText(box.adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap, cycleText_);
    }
    // Recording: a red dot.
    if (recording_) {
        const double rad = std::max(4.0, r.height() * 0.015);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 40, 40));
        p.drawEllipse(QPointF(r.left() + rad * 3, r.top() + rad * 3), rad, rad);
    }
    p.restore();
}

}  // namespace montage
