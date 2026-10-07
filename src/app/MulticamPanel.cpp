#include "MulticamPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "PlaybackController.h"
#include "Theme.h"
#include "core/Multicam.h"
#include "media/AudioSync.h"
#include "media/MediaPool.h"
#include "media/SpeakerSwitch.h"
#include "render/Compositor.h"
#include "render/Processing.h"

namespace montage {

namespace {
QSettings settings() { return QSettings(QStringLiteral("Montage"), QStringLiteral("Montage")); }
}  // namespace

// One angle: its picture, number and name; red while it is on the program.
class AngleView : public QWidget {
public:
    AngleView(int index, MulticamPanel* panel) : QWidget(panel), index_(index), panel_(panel) {
        setMinimumSize(96, 54);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::PointingHandCursor);
        setObjectName(QStringLiteral("angle%1").arg(index + 1));
    }
    void setImage(const QImage& img) {
        image_ = img;
        update();
    }
    const QImage& image() const { return image_; }
    void setName(const QString& name) {
        name_ = name;
        setToolTip(tr("Angle %1: %2 (key %1; Shift-click cuts at the playhead)").arg(index_ + 1).arg(name));
        update();
    }
    void setLive(bool live, bool playing) {
        live_ = live;
        playing_ = playing;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!image_.isNull()) {
            QSize s = image_.size().scaled(size() - QSize(4, 4), Qt::KeepAspectRatio);
            QRect r(QPoint(0, 0), s);
            r.moveCenter(rect().center());
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(r, image_);
        }
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        const QString label = QStringLiteral("%1  %2").arg(index_ + 1).arg(name_);
        QRect lr = rect().adjusted(4, 4, -4, -4);
        lr.setHeight(p.fontMetrics().height() + 2);
        p.fillRect(lr.adjusted(0, 0, 0, 0).intersected(QRect(lr.topLeft(), QSize(p.fontMetrics().horizontalAdvance(label) + 8, lr.height()))),
                   QColor(0, 0, 0, 150));
        p.setPen(Qt::white);
        p.drawText(lr.adjusted(4, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
        if (live_) {
            // Red while cutting live, yellow when stopped (as editors expect).
            p.setPen(QPen(playing_ ? QColor(230, 40, 40) : QColor(240, 200, 40), 4));
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect().adjusted(2, 2, -2, -2));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) panel_->switchTo(index_, e->modifiers() & Qt::ShiftModifier);
    }

private:
    int index_;
    MulticamPanel* panel_;
    QImage image_;
    QString name_;
    bool live_ = false, playing_ = false;
};

MulticamPanel::MulticamPanel(EditorState* state, PlaybackController* program, QWidget* parent)
    : QWidget(parent), state_(state), program_(program) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    gridHost_ = new QWidget(this);
    grid_ = new QGridLayout(gridHost_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(3);
    lay->addWidget(gridHost_, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    QPalette dim = status_->palette();
    dim.setColor(QPalette::WindowText, theme::kTextDim);
    status_->setPalette(dim);
    lay->addWidget(status_);
    auto* row = new QHBoxLayout;
    audioFollows_ = new QCheckBox(tr("Audio follows video"), this);
    audioFollows_->setObjectName(QStringLiteral("audioFollows"));
    audioFollows_->setToolTip(tr("Switching an angle also switches the clip's sound to that camera's own audio"));
    audioFollows_->setChecked(settings().value(QStringLiteral("multicam/audioFollows"), false).toBool());
    connect(audioFollows_, &QCheckBox::toggled, this, [](bool on) { settings().setValue(QStringLiteral("multicam/audioFollows"), on); });
    autoButton_ = new QPushButton(tr("Auto Switch..."), this);
    autoButton_->setObjectName(QStringLiteral("autoSwitch"));
    autoButton_->setToolTip(tr("Cut to whoever is speaking, from each person's microphone"));
    connect(autoButton_, &QPushButton::clicked, this, &MulticamPanel::autoSwitch);
    row->addWidget(audioFollows_);
    row->addStretch(1);
    row->addWidget(autoButton_);
    lay->addLayout(row);

    throttle_ = new QTimer(this);
    throttle_->setSingleShot(true);
    throttle_->setInterval(60);
    connect(throttle_, &QTimer::timeout, this, &MulticamPanel::requestRender);
    connect(&watcher_, &QFutureWatcher<std::vector<QImage>>::finished, this, [this] {
        const auto images = watcher_.result();
        for (size_t i = 0; i < images.size() && i < views_.size(); ++i) views_[i]->setImage(images[i]);
        if (renderPending_) {
            renderPending_ = false;
            throttle_->start();
        }
    });
    connect(state_, &EditorState::projectChanged, this, &MulticamPanel::refresh);
    connect(state_, &EditorState::playheadChanged, this, &MulticamPanel::refresh);
    connect(state_, &EditorState::selectionChanged, this, &MulticamPanel::refresh);
    connect(program_, &PlaybackController::playingChanged, this, &MulticamPanel::refresh);
    refresh();
}

MulticamPanel::~MulticamPanel() { watcher_.waitForFinished(); }

void MulticamPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    refresh();  // nothing renders while the panel is hidden
}

Id MulticamPanel::currentClip() const {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const Project& p = state_->project();
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Video)
            if (multicamSequence(p, trackAt(*s, loc->track)->clips[loc->index])) return id;
    const FrameTime t = state_->playhead();
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i) {
        const Track& tr = s->videoTracks[size_t(i)];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips)
            if (c.enabled && c.contains(t) && multicamSequence(p, c)) return c.id;
    }
    return 0;
}

bool MulticamPanel::audioFollowsVideo() const { return audioFollows_->isChecked(); }

std::vector<QImage> MulticamPanel::angleImages() const {
    std::vector<QImage> out;
    for (const AngleView* v : views_) out.push_back(v->image());
    return out;
}

void MulticamPanel::rebuildGrid(const std::vector<std::string>& names) {
    for (AngleView* v : views_) delete v;
    views_.clear();
    const int n = int(names.size());
    const int cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4;
    for (int i = 0; i < n; ++i) {
        auto* v = new AngleView(i, this);
        v->setName(QString::fromStdString(names[size_t(i)]));
        grid_->addWidget(v, i / cols, i % cols);
        views_.push_back(v);
    }
    names_ = names;
}

void MulticamPanel::refresh() {
    clip_ = currentClip();
    const Sequence* s = state_->sequence();
    const Clip* c = clip_ && s ? edit::clipById(*s, clip_) : nullptr;
    const Sequence* mc = c ? multicamSequence(state_->project(), *c) : nullptr;
    const std::vector<std::string> names = mc ? angleNames(*mc) : std::vector<std::string>{};
    if (names != names_) rebuildGrid(names);
    const bool playing = program_->isPlaying();
    for (size_t i = 0; i < views_.size(); ++i) views_[i]->setLive(c && c->angle == int(i), playing);
    autoButton_->setEnabled(mc != nullptr);
    if (!mc)
        status_->setText(tr("Put a multicam clip under the playhead (Media › Create Multicam Clip) to switch angles here."));
    else
        status_->setText(playing ? tr("Live: click an angle or press 1–%1 to cut to it.").arg(std::min<size_t>(9, names.size()))
                                 : tr("Click an angle or press 1–%1 to switch this shot; Shift cuts at the playhead.")
                                       .arg(std::min<size_t>(9, names.size())));
    if (mc && isVisible() && !throttle_->isActive()) throttle_->start();
}

void MulticamPanel::requestRender() {
    if (watcher_.isRunning()) {
        renderPending_ = true;
        return;
    }
    const Sequence* s = state_->sequence();
    const Clip* c = clip_ && s ? edit::clipById(*s, clip_) : nullptr;
    if (!c) return;
    std::shared_ptr<const Project> snap = program_->snapshot();
    const Sequence* mc = multicamSequence(*snap, *c);
    if (!mc) return;
    const Id mcId = mc->id;
    const FrameTime frame = FrameTime(std::floor(c->sourceFrameAt(state_->playhead()) * mc->fpsValue() / s->fpsValue() + 1e-6));
    const int n = int(mc->videoTracks.size());
    const int cellW = views_.empty() ? 320 : std::max(96, views_.front()->width());
    const double scale = std::clamp(double(cellW) / std::max(1, mc->width), 0.05, 0.5);
    watcher_.setFuture(QtConcurrent::run([snap, mcId, frame, n, scale] {
        std::vector<QImage> out;
        const Sequence* m = snap->findSequence(mcId);
        if (!m) return out;
        for (int i = 0; i < n; ++i) {
            RenderOptions o;
            o.scale = scale;
            o.soloVideoTrack = i;
            o.displaySpace = "rec709";
            Image img = renderProgramFrame(*snap, *m, frame, o);
            QImage q(img.width, img.height, QImage::Format_RGBA8888);
            toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
            out.push_back(q);
        }
        return out;
    }));
}

bool MulticamPanel::switchTo(int angle, bool cut) {
    const Id clip = currentClip();
    if (!clip || angle < 0 || angle >= angleCount()) return false;
    const bool live = cut || program_->isPlaying();
    const FrameTime at = state_->playhead();
    const bool follows = audioFollows_->isChecked();
    const bool ok = state_->apply(live ? tr("Cut to Angle %1").arg(angle + 1) : tr("Switch to Angle %1").arg(angle + 1),
                                  [=](Project& p, Sequence& s) { return edit::switchAngle(p, s, clip, angle, at, live, follows); });
    refresh();
    return ok;
}

void MulticamPanel::autoSwitch() {
    const Id clip = currentClip();
    const Sequence* s = state_->sequence();
    const Clip* c = clip && s ? edit::clipById(*s, clip) : nullptr;
    const Sequence* mc = c ? multicamSequence(state_->project(), *c) : nullptr;
    if (!mc) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Auto Switch by Speaker"));
    const Project& project = state_->project();
    auto angleName = [&](int a) { return QStringLiteral("%1 %2").arg(a + 1).arg(QString::fromStdString(mc->videoTracks[size_t(a)].name)); };
    // Who speaks can come from each person's microphone, or from a transcript
    // whose speakers are labelled (one recording of everyone is enough).
    auto* by = new QComboBox(&dlg);
    by->setObjectName(QStringLiteral("switchBy"));
    by->addItem(tr("Microphones (one per speaker)"), 0);
    by->addItem(tr("Speaker labels in a transcript"), 1);
    auto* pages = new QStackedWidget(&dlg);

    auto* micPage = new QWidget(pages);
    auto* form = new QFormLayout(micPage);
    form->setContentsMargins(0, 0, 0, 0);
    auto* intro = new QLabel(tr("Choose the microphone that hears each angle's speaker. The clip is cut to whoever is "
                                "clearly speaking; the wide angle covers silence and people talking over each other."),
                             micPage);
    intro->setWordWrap(true);
    form->addRow(intro);
    std::vector<QComboBox*> listen;
    for (int a = 0; a < int(mc->videoTracks.size()); ++a) {
        auto* combo = new QComboBox(micPage);
        combo->setObjectName(QStringLiteral("listen%1").arg(a + 1));
        combo->addItem(tr("No one (not a close-up)"), -1);
        for (int t = 0; t < int(mc->audioTracks.size()); ++t)
            combo->addItem(QString::fromStdString(mc->audioTracks[size_t(t)].name), t);
        const int own = angleAudioTrack(*mc, a);
        combo->setCurrentIndex(std::max(0, combo->findData(own)));
        form->addRow(tr("%1 listens to:").arg(angleName(a)), combo);
        listen.push_back(combo);
    }
    auto* margin = new QDoubleSpinBox(micPage);
    margin->setRange(1, 20);
    margin->setValue(4);
    margin->setSuffix(tr(" dB"));
    margin->setToolTip(tr("How much louder than everyone else a speaker must be to get the shot"));
    form->addRow(tr("Speaker margin:"), margin);
    pages->addWidget(micPage);

    auto* labelPage = new QWidget(pages);
    auto* labelForm = new QFormLayout(labelPage);
    labelForm->setContentsMargins(0, 0, 0, 0);
    auto* labelIntro = new QLabel(labelPage);
    labelIntro->setWordWrap(true);
    labelForm->addRow(labelIntro);
    auto* fromTrack = new QComboBox(labelPage);
    fromTrack->setObjectName(QStringLiteral("labelTrack"));
    for (int t = 0; t < int(mc->audioTracks.size()); ++t) {
        int speakers = 0;
        transcriptTurns(project, *mc, t, &speakers);
        if (speakers > 0)
            fromTrack->addItem(tr("%1 (%n speaker(s))", "", speakers).arg(QString::fromStdString(mc->audioTracks[size_t(t)].name)), t);
    }
    labelForm->addRow(tr("Transcript of:"), fromTrack);
    auto* speakerRows = new QWidget(labelPage);
    auto* speakerForm = new QFormLayout(speakerRows);
    speakerForm->setContentsMargins(0, 0, 0, 0);
    labelForm->addRow(speakerRows);
    std::vector<QComboBox*> showSpeaker;
    // One row per speaker of the chosen recording: the angle that shows them (the n-th angle by default).
    auto rebuildSpeakers = [&, speakerForm, speakerRows] {
        while (speakerForm->rowCount() > 0) speakerForm->removeRow(0);
        showSpeaker.clear();
        const int track = fromTrack->currentData().toInt();
        int speakers = 0;
        transcriptTurns(project, *mc, track, &speakers);
        const MediaItem* named = nullptr;
        if (track >= 0 && track < int(mc->audioTracks.size()))
            for (const Clip& c : mc->audioTracks[size_t(track)].clips)
                if (const MediaItem* m = project.findMedia(c.mediaId); m && m->transcript && !named) named = m;
        for (int sp = 0; sp < speakers; ++sp) {
            auto* combo = new QComboBox(speakerRows);
            combo->setObjectName(QStringLiteral("speakerAngle%1").arg(sp + 1));
            combo->addItem(tr("No close-up"), -1);
            for (int a = 0; a < int(mc->videoTracks.size()); ++a) combo->addItem(angleName(a), a);
            combo->setCurrentIndex(sp + 1 < combo->count() ? sp + 1 : 0);
            const QString who = named ? QString::fromStdString(speakerName(*named->transcript, sp)) : tr("Speaker %1").arg(sp + 1);
            speakerForm->addRow(tr("%1 is on:").arg(who), combo);
            showSpeaker.push_back(combo);
        }
    };
    labelIntro->setText(fromTrack->count() == 0
                            ? tr("No recording in this multicam clip has speaker labels yet. In the Media panel, choose "
                                 "Transcribe… with Label speakers on one recording of everyone, then come back here.")
                            : tr("Choose the angle that shows each person. The clip is cut to whoever is speaking, by the "
                                 "transcript's speaker labels; the wide angle covers pauses and people talking over each other."));
    rebuildSpeakers();
    connect(fromTrack, &QComboBox::currentIndexChanged, &dlg, [&] { rebuildSpeakers(); });
    pages->addWidget(labelPage);

    auto* shared = new QFormLayout;
    auto* wide = new QComboBox(&dlg);
    wide->setObjectName(QStringLiteral("wideAngle"));
    wide->addItem(tr("None (stay on the last speaker)"), -1);
    for (int a = 0; a < int(mc->videoTracks.size()); ++a) wide->addItem(angleName(a), a);
    shared->addRow(tr("Wide angle:"), wide);
    auto* minShot = new QDoubleSpinBox(&dlg);
    minShot->setObjectName(QStringLiteral("minShot"));
    minShot->setRange(0.5, 30);
    minShot->setSingleStep(0.5);
    minShot->setValue(2.0);
    minShot->setSuffix(tr(" s"));
    shared->addRow(tr("Shortest shot:"), minShot);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Switch"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto update = [&, pages, buttons] {
        pages->setCurrentIndex(by->currentIndex());
        buttons->button(QDialogButtonBox::Ok)->setEnabled(by->currentIndex() == 0 || fromTrack->count() > 0);
    };
    connect(by, &QComboBox::currentIndexChanged, &dlg, update);
    if (fromTrack->count() > 0) by->setCurrentIndex(1);  // labels are there: the simpler way
    update();
    auto* lay = new QVBoxLayout(&dlg);
    auto* top = new QFormLayout;
    top->addRow(tr("Switch by:"), by);
    lay->addLayout(top);
    lay->addWidget(pages);
    lay->addLayout(shared);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    if (by->currentIndex() == 1) {
        // From the transcript: nothing to decode.
        AutoSwitchOptions o;
        o.wideAngle = wide->currentData().toInt();
        o.minShotSeconds = minShot->value();
        std::vector<int> angleOf;
        for (QComboBox* c : showSpeaker) angleOf.push_back(c->currentData().toInt());
        std::string err;
        const auto changes = turnAngleChanges(*mc, transcriptTurns(project, *mc, fromTrack->currentData().toInt()), angleOf, o, &err);
        if (changes.empty()) {
            state_->message(QString::fromStdString(err));
            return;
        }
        const bool follows = audioFollows_->isChecked();
        if (state_->apply(tr("Auto Switch"), [=](Project& p, Sequence& sq) { return edit::applyAngleChanges(p, sq, clip, changes, follows); }))
            state_->message(tr("Auto Switch made %n cut(s)", "", int(changes.size()) - 1));
        return;
    }
    AutoSwitchOptions o;
    for (QComboBox* l : listen) o.listen.push_back(l->currentData().toInt());
    o.wideAngle = wide->currentData().toInt();
    o.minShotSeconds = minShot->value();
    o.marginDb = margin->value();
    // Listening decodes every microphone: off the UI thread, with a way out.
    QProgressDialog progress(tr("Listening to the microphones..."), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(&progress, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    auto snap = program_->snapshot();
    const Id mcId = mc->id;
    using Changes = std::vector<std::pair<FrameTime, int>>;
    QFutureWatcher<std::pair<Changes, std::string>> work;
    QEventLoop wait;
    connect(&work, &QFutureWatcher<std::pair<Changes, std::string>>::finished, &wait, &QEventLoop::quit);
    work.setFuture(QtConcurrent::run([snap, mcId, o, cancel] {
        std::string err;
        const Sequence* m = snap->findSequence(mcId);
        Changes changes = m ? speakerAngleChanges(*snap, *m, o, &err, cancel.get()) : Changes{};
        return std::make_pair(changes, err);
    }));
    if (!work.isFinished()) wait.exec();
    progress.disconnect(this);  // closing a progress dialog emits canceled()
    progress.close();
    const auto [changes, err] = work.result();
    if (*cancel) return;
    if (changes.empty()) {
        state_->message(QString::fromStdString(err));
        return;
    }
    const bool follows = audioFollows_->isChecked();
    if (state_->apply(tr("Auto Switch"), [=](Project& p, Sequence& sq) { return edit::applyAngleChanges(p, sq, clip, changes, follows); }))
        state_->message(tr("Auto Switch made %n cut(s)", "", int(changes.size()) - 1));
}

Id MulticamPanel::createMulticam(EditorState* state, const std::vector<Id>& media, Sync sync, const QString& name,
                                QWidget* parent) {
    const Project& p = state->project();
    std::vector<double> offsets(media.size(), 0.0);
    if (sync == Sync::Timecode) {
        if (!timecodeOffsets(p, media, offsets)) {
            state->message(tr("Not every clip has a start timecode: sync by audio or by in points instead"), 6000);
            return 0;
        }
    } else if (sync == Sync::Audio) {
        // Each source against the first one with sound.
        std::vector<std::string> paths;
        for (Id id : media) {
            const MediaItem* m = p.findMedia(id);
            paths.push_back(m && m->hasAudio ? m->path : std::string());
        }
        const auto ref = std::find_if(paths.begin(), paths.end(), [](const std::string& s) { return !s.empty(); });
        if (ref == paths.end()) {
            state->message(tr("None of the clips has sound to sync by"), 6000);
            return 0;
        }
        const size_t refIndex = size_t(ref - paths.begin());
        QProgressDialog progress(tr("Syncing the cameras by their sound..."), QString(), 0, 0, parent);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        using Offsets = std::vector<std::pair<double, bool>>;
        QFutureWatcher<Offsets> work;
        QEventLoop wait;
        QObject::connect(&work, &QFutureWatcher<Offsets>::finished, &wait, &QEventLoop::quit);
        const int rate = state->sequence() ? state->sequence()->sampleRate : 48000;
        work.setFuture(QtConcurrent::run([paths, refIndex, rate] {
            Offsets out(paths.size(), {0.0, false});
            AudioBufferPtr refBuf = MediaPool::instance().audio(paths[refIndex], rate);
            out[refIndex] = {0.0, true};
            for (size_t i = 0; i < paths.size(); ++i) {
                if (i == refIndex || paths[i].empty() || !refBuf) continue;
                AudioBufferPtr b = MediaPool::instance().audio(paths[i], rate);
                SyncResult r = b ? findAudioOffset(*refBuf, *b) : SyncResult{};
                out[i] = {r.offset, r.found};
            }
            return out;
        }));
        if (!work.isFinished()) wait.exec();
        progress.close();
        QStringList failed;
        const Offsets found = work.result();
        for (size_t i = 0; i < media.size(); ++i) {
            offsets[i] = found[i].first;
            if (!found[i].second && i != refIndex)
                if (const MediaItem* m = p.findMedia(media[i])) failed << QString::fromStdString(m->name);
        }
        if (!failed.isEmpty())
            QMessageBox::information(parent, tr("Create Multicam Clip"),
                                     tr("No reliable audio match for %1; they start with the first camera.").arg(failed.join(", ")));
    }
    Id created = 0;
    std::string error;
    const std::string n = name.toStdString();
    state->edit(tr("Create Multicam Clip"), [&](Project& pr, Sequence&) {
        created = makeMulticam(pr, media, offsets, n, &error);
        return created != 0;
    });
    if (!created) state->message(QString::fromStdString(error), 6000);
    return created;
}

Id MulticamPanel::createMulticamDialog(EditorState* state, const std::vector<Id>& media, QWidget* parent) {
    const Project& p = state->project();
    bool allTimecode = true, anySound = false;
    int cameras = 0;
    for (Id id : media)
        if (const MediaItem* m = p.findMedia(id)) {
            allTimecode &= m->timecode >= 0;
            anySound |= m->hasAudio;
            cameras += m->hasVideo ? 1 : 0;
        }
    QDialog dlg(parent);
    dlg.setWindowTitle(tr("Create Multicam Clip"));
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(tr("Multicam %1").arg(std::count_if(p.sequences.begin(), p.sequences.end(),
                                                                  [](const Sequence& s) { return s.multicam; }) + 1),
                               &dlg);
    name->setObjectName(QStringLiteral("multicamName"));
    form->addRow(tr("Name:"), name);
    auto* sync = new QComboBox(&dlg);
    sync->setObjectName(QStringLiteral("multicamSync"));
    sync->addItem(tr("Audio (match their sound)"), int(Sync::Audio));
    sync->addItem(tr("Timecode"), int(Sync::Timecode));
    sync->addItem(tr("In points (start together)"), int(Sync::InPoints));
    if (!anySound) sync->removeItem(0);
    if (allTimecode) sync->setCurrentIndex(sync->findData(int(Sync::Timecode)));
    else if (int i = sync->findData(int(Sync::Timecode)); i >= 0) sync->removeItem(i);
    form->addRow(tr("Sync by:"), sync);
    auto* note = new QLabel(tr("%n camera angle(s), in the media bin's order; every clip with sound gets an audio track.", "", cameras), &dlg);
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return 0;
    const QString n = name->text().trimmed().isEmpty() ? tr("Multicam") : name->text().trimmed();
    return createMulticam(state, media, Sync(sync->currentData().toInt()), n, parent);
}

}  // namespace montage
