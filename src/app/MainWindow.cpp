#include "MainWindow.h"

#include "Keymap.h"
#include "LoudnessReadout.h"
#include "Voiceover.h"

#include <QAction>
#include <QCheckBox>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QFutureWatcher>
#include <QMenuBar>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QtConcurrent>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

#include "AudioMeterWidget.h"
#include "audio/PluginEffect.h"
#include "media/Decoder.h"
#include "media/HwAccel.h"
#include "media/MediaPool.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "ExportDialog.h"
#include "InspectorWidget.h"
#include "AutoDuckDialog.h"
#include "AutoMixDialog.h"
#include "ScriptCutDialog.h"
#include "KeyframePanel.h"
#include "MediaBinWidget.h"
#include "RenderQueue.h"
#include "RenderQueuePanel.h"
#include "MixerPanel.h"
#include "MulticamPanel.h"
#include "CaptionsPanel.h"
#include "MaskOverlay.h"
#include "SequenceIndexPanel.h"
#include "ShotSearchPanel.h"
#include "PeoplePanel.h"
#include "SpeechDialog.h"
#include "TranscriptPanel.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "PluginManagerDialog.h"
#include "ScopesWidget.h"
#include "SequenceSettingsDialog.h"
#include "Theme.h"
#include "core/Checkerboard.h"
#include "core/Effects.h"
#include "core/Interchange.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#include "render/AafExport.h"
#include "render/ClipAnalysis.h"
#include "render/RenderCache.h"
#include "render/Compositor.h"
#include "render/MusicEdit.h"
#include "render/VoiceMatch.h"
#include "render/Exporter.h"
#include "render/Processing.h"

namespace montage {

namespace {
QSettings appSettings() { return QSettings("Montage", "Montage"); }
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    state_ = new EditorState(this);
    program_ = new PlaybackController(this);
    source_ = new PlaybackController(this);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    buildPanels();
    buildActions();
    buildMenus();
    keymap::load(this);

    // Crash safety: find sessions that did not exit cleanly before starting ours.
    recovery_ = new RecoveryManager(state_, QString(), this);
    const auto crashed = recovery_->crashedSessions();
    recovery_->beginSession();
    if (!crashed.empty() && !qEnvironmentVariableIsSet("MONTAGE_NO_RECOVERY_PROMPT")) {
        // Ask after the window is up; the plugin scan waits for the safe-mode answer.
        QTimer::singleShot(0, this, [this, crashed] { offerRecovery(crashed); });
    } else {
        scanPluginsInBackground();
    }

    syncTimer_.setSingleShot(true);
    syncTimer_.setInterval(0);
    connect(&syncTimer_, &QTimer::timeout, this, &MainWindow::syncProgram);
    connect(state_, &EditorState::projectChanged, &syncTimer_, qOverload<>(&QTimer::start));
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        syncProgram();
        program_->seek(state_->playhead());
        updateTitle();
    });
    connect(state_, &EditorState::playheadChanged, this, [this](FrameTime t) {
        if (t != program_->position()) program_->seek(t);
    });
    connect(program_, &PlaybackController::positionChanged, this, [this](FrameTime t) {
        state_->setPlayhead(t);
        if (program_->isPlaying()) timeline_->followPlayhead(t);
    });
    connect(state_, &EditorState::sourceChanged, this, &MainWindow::rebuildSourceProject);
    connect(state_, &EditorState::historyChanged, this, &MainWindow::updateActions);
    connect(state_, &EditorState::selectionChanged, this, &MainWindow::updateActions);
    connect(state_, &EditorState::fileStateChanged, this, &MainWindow::updateTitle);
    connect(state_, &EditorState::statusMessage, this, [this](const QString& text, int ms) { statusBar()->showMessage(text, ms); });
    connect(state_, &EditorState::mediaReady, this, [this] { program_->requestFrame(); });
    connect(program_, &PlaybackController::frameRendered, scopes_, &ScopesWidget::setFrame);
    connect(program_, &PlaybackController::audioLevels, this, [this](float l, float r, const QVector<float>& tracks) {
        meter_->setLevels(l, r);
        mixer_->setLevels(l, r, tracks);
    });
    connect(source_, &PlaybackController::audioLevels, this, [this](float l, float r, const QVector<float>&) { meter_->setLevels(l, r); });
    // Route J/K/L, I/O and friends to the monitor the user last touched.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        for (QWidget* w = now; w; w = w->parentWidget()) {
            if (w == sourcePanel_) {
                active_ = Monitor::Source;
                return;
            }
            if (w == programPanel_ || w == timeline_) {
                active_ = Monitor::Program;
                return;
            }
        }
    });
    connect(sourcePanel_, &MonitorPanel::activated, this, [this] { active_ = Monitor::Source; });
    connect(programPanel_, &MonitorPanel::activated, this, [this] { active_ = Monitor::Program; });

    statusInfo_ = new QLabel(this);
    statusInfo_->setStyleSheet(QString("color: %1; padding-right: 8px;").arg(theme::kTextDim.name()));
    statusBar()->addPermanentWidget(statusInfo_);
    auto* cacheTimer = new QTimer(this);
    connect(cacheTimer, &QTimer::timeout, this, [this] {
        const Sequence* s = state_->sequence();
        statusInfo_->setText(tr("%1 · %2×%3 · %4 fps · cache %5 MB")
                                 .arg(QString::fromStdString(s ? s->name : ""))
                                 .arg(s ? s->width : 0)
                                 .arg(s ? s->height : 0)
                                 .arg(s ? s->fpsValue() : 0, 0, 'f', 3)
                                 .arg(MediaPool::instance().frameCacheBytes() / (1024 * 1024)));
    });
    cacheTimer->start(1000);

    resize(1600, 960);
    restoreLayout();
    syncProgram();
    rebuildSourceProject();
    updateTitle();
    updateActions();
    timeline_->zoomToFit();
    statusBar()->showMessage(tr("Welcome to Montage — import media with Ctrl+I or drag files onto the Media bin"), 8000);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// Layout

void MainWindow::buildPanels() {
    timeline_ = new TimelineWidget(state_, this);
    setCentralWidget(timeline_);
    connect(timeline_, &TimelineWidget::clipActivated, this, [this](Id clip) {
        const Sequence* s = state_->sequence();
        const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
        if (!c) return;
        if (const MediaItem* m = state_->project().findMedia(c->mediaId)) {
            if (m->kind == MediaKind::Sequence) state_->setActiveSequence(m->sequenceId);
            else openInSource(m->id);
        }
    });

    auto makeDock = [this](const QString& title, const QString& name, QWidget* w) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(name);
        d->setWidget(w);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        docks_.push_back(d);
        return d;
    };
    sourcePanel_ = new MonitorPanel(MonitorPanel::Mode::Source, state_, source_, this);
    programPanel_ = new MonitorPanel(MonitorPanel::Mode::Program, state_, program_, this);
    new MaskOverlay(state_, programPanel_->viewer());
    connect(programPanel_, &MonitorPanel::exportFrameRequested, this, &MainWindow::exportFrame);
    bin_ = new MediaBinWidget(state_, this);
    connect(bin_, &MediaBinWidget::openInSource, this, &MainWindow::openInSource);
    connect(bin_, &MediaBinWidget::newTitleRequested, this, &MainWindow::addTitle);
    connect(bin_, &MediaBinWidget::newSequenceRequested, this, &MainWindow::newSequence);
    connect(bin_, &MediaBinWidget::findSimilarRequested, this, [this](Id media) {
        const MediaItem* m = state_->project().findMedia(media);
        if (!m) return;
        shotsDock_->show();
        shotsDock_->raise();
        shots_->searchSimilar(media, m->duration / 2);
    });
    connect(bin_, &MediaBinWidget::createMulticamRequested, this, [this](const std::vector<Id>& media) {
        if (MulticamPanel::createMulticamDialog(state_, media, this)) {
            multicamDock_->show();
            multicamDock_->raise();
        }
    });
    effects_ = new EffectsBrowser(this);
    connect(effects_, &EffectsBrowser::applyRequested, this, &MainWindow::applyFromBrowser);
    inspector_ = new InspectorWidget(state_, this);
    scopes_ = new ScopesWidget(this);
    mixer_ = new MixerPanel(state_, this);
    connect(mixer_, &MixerPanel::effectsRequested, this, [this] {
        inspectorDock_->show();
        inspectorDock_->raise();
    });
    captions_ = new CaptionsPanel(state_, this);
    transcript_ = new TranscriptPanel(state_, this);
    connect(transcript_, &TranscriptPanel::sourceSeekRequested, this, [this](FrameTime f) {
        source_->seek(f);
        sourceDock_->raise();
    });
    connect(source_, &PlaybackController::positionChanged, transcript_, &TranscriptPanel::setSourcePosition);
    shots_ = new ShotSearchPanel(state_, this);
    connect(shots_, &ShotSearchPanel::openRequested, this, [this](Id media, FrameTime in, FrameTime out, FrameTime at) {
        // The moment, marked in the Source monitor and ready to edit in.
        openInSource(media);
        state_->setSourceIn(in);
        state_->setSourceOut(out);
        source_->seek(at);
    });
    people_ = new PeoplePanel(state_, this);
    connect(people_, &PeoplePanel::openRequested, this, [this](Id media, FrameTime in, FrameTime out, FrameTime at) {
        openInSource(media);
        if (in >= 0 && out >= in) {
            state_->setSourceIn(in);
            state_->setSourceOut(out);
        }
        source_->seek(at);
    });
    connect(people_, &PeoplePanel::smartBinCreated, this, [this](Id id) {
        bin_->showSmartBin(id);
        binDock_->raise();
    });
    meter_ = new AudioMeterWidget(this);
    multicam_ = new MulticamPanel(state_, program_, this);

    sourceDock_ = makeDock(tr("Source"), "source", sourcePanel_);
    programDock_ = makeDock(tr("Program"), "program", programPanel_);
    inspectorDock_ = makeDock(tr("Inspector"), "inspector", inspector_);
    binDock_ = makeDock(tr("Media"), "media", bin_);
    effectsDock_ = makeDock(tr("Effects"), "effects", effects_);
    scopesDock_ = makeDock(tr("Scopes"), "scopes", scopes_);
    mixerDock_ = makeDock(tr("Audio Mixer"), "mixer", mixer_);
    captionsDock_ = makeDock(tr("Captions"), "captions", captions_);
    transcriptDock_ = makeDock(tr("Transcript"), "transcript", transcript_);
    shotsDock_ = makeDock(tr("Find Shots"), "shots", shots_);
    peopleDock_ = makeDock(tr("People"), "people", people_);
    indexDock_ = makeDock(tr("Sequence Index"), "index", new SequenceIndexPanel(state_, this));
    multicamDock_ = makeDock(tr("Multicam"), "multicam", multicam_);
    keyframesDock_ = makeDock(tr("Keyframes"), "keyframes", new KeyframePanel(state_, this));
    queue_ = new RenderQueue(this);
    queueDock_ = makeDock(tr("Render Queue"), "renderqueue", new RenderQueuePanel(queue_, this));
    connect(queue_, &RenderQueue::jobFinished, this, [this](int id, bool ok) {
        const RenderQueue::Job* j = queue_->job(id);
        if (!j) return;
        const QString file = QDir::toNativeSeparators(QString::fromStdString(j->settings.path));
        if (ok) statusBar()->showMessage(tr("Rendered %1").arg(file), 8000);
        else if (j->status == RenderQueue::Status::Failed) statusBar()->showMessage(tr("Render failed: %1 (%2)").arg(file, j->error), 10000);
    });
    // 1–9 cut to an angle (live while playing); Shift cuts at the playhead when stopped.
    for (int i = 0; i < 9; ++i) {
        auto* sw = new QShortcut(QKeySequence(Qt::Key_1 + i), this);
        connect(sw, &QShortcut::activated, this, [this, i] { multicam_->switchTo(i, false); });
        auto* cut = new QShortcut(QKeySequence(Qt::SHIFT | (Qt::Key_1 + i)), this);
        connect(cut, &QShortcut::activated, this, [this, i] { multicam_->switchTo(i, true); });
    }
    connect(timeline_, &TimelineWidget::captionActivated, this, [this](Id track, int index) {
        captionsDock_->show();
        captionsDock_->raise();
        captions_->editCaption(track, index);
    });
    // The peak meters with the loudness readout beneath them.
    loudness_ = new LoudnessReadout(this);
    loudness_->setObjectName(QStringLiteral("loudness"));
    auto* meters = new QWidget(this);
    auto* ml = new QVBoxLayout(meters);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(2);
    ml->addWidget(meter_, 1);
    ml->addWidget(loudness_);
    for (PlaybackController* pc : {program_, source_}) connect(pc, &PlaybackController::loudness, loudness_, &LoudnessReadout::setReading);
    connect(loudness_, &LoudnessReadout::resetRequested, this, [this] {
        program_->resetLoudness();
        source_->resetLoudness();
    });
    meterDock_ = makeDock(tr("Meters"), "meters", meters);
    // The render bar follows edits (once they settle).
    renderBarTimer_.setSingleShot(true);
    renderBarTimer_.setInterval(400);
    connect(&renderBarTimer_, &QTimer::timeout, this, [this] { refreshRenderBar(); });
    for (auto sig : {&EditorState::projectChanged, &EditorState::sequenceSwitched})
        connect(state_, sig, &renderBarTimer_, qOverload<>(&QTimer::start));
    resetLayout();
}

void MainWindow::resetLayout() {
    for (QDockWidget* d : docks_) {
        removeDockWidget(d);
        d->setFloating(false);
    }
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    addDockWidget(Qt::TopDockWidgetArea, sourceDock_);
    splitDockWidget(sourceDock_, programDock_, Qt::Horizontal);
    tabifyDockWidget(sourceDock_, inspectorDock_);
    tabifyDockWidget(sourceDock_, scopesDock_);
    tabifyDockWidget(sourceDock_, mixerDock_);
    tabifyDockWidget(sourceDock_, captionsDock_);
    tabifyDockWidget(sourceDock_, multicamDock_);
    tabifyDockWidget(sourceDock_, keyframesDock_);
    sourceDock_->raise();
    addDockWidget(Qt::LeftDockWidgetArea, binDock_);
    tabifyDockWidget(binDock_, effectsDock_);
    tabifyDockWidget(binDock_, transcriptDock_);
    tabifyDockWidget(binDock_, shotsDock_);
    tabifyDockWidget(binDock_, peopleDock_);
    tabifyDockWidget(binDock_, indexDock_);
    tabifyDockWidget(binDock_, queueDock_);
    binDock_->raise();
    addDockWidget(Qt::RightDockWidgetArea, meterDock_);
    for (QDockWidget* d : docks_) d->show();
    resizeDocks({binDock_}, {380}, Qt::Horizontal);
    resizeDocks({meterDock_}, {70}, Qt::Horizontal);
    resizeDocks({sourceDock_, programDock_}, {720, 860}, Qt::Horizontal);
    resizeDocks({sourceDock_}, {height() / 2 + 40}, Qt::Vertical);
}

void MainWindow::restoreLayout() {
    QSettings s = appSettings();
    if (s.contains("window/geometry")) restoreGeometry(s.value("window/geometry").toByteArray());
    if (s.contains("window/state")) restoreState(s.value("window/state").toByteArray(), 1);
}

// ---------------------------------------------------------------------------
// Actions & menus

void MainWindow::buildActions() {}

void MainWindow::buildMenus() {
    auto add = [this](QMenu* menu, const QString& text, const QKeySequence& key, auto fn) {
        QAction* a = menu->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);  // keep shortcuts alive even when menus are hidden
        keymap::registerAction(a, menu->title(), text);  // its key can be changed (Keyboard Shortcuts)
        return a;
    };
    auto withSeq = [this](auto fn) {
        return [this, fn] {
            if (state_->sequence()) fn();
        };
    };

    // ---- File
    QMenu* file = menuBar()->addMenu(tr("&File"));
    add(file, tr("&New Project"), QKeySequence::New, [this] { newProject(); });
    add(file, tr("&Open Project…"), QKeySequence::Open, [this] { openDialog(); });
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    rebuildRecentMenu();
    add(file, tr("&Save"), QKeySequence::Save, [this] { save(); });
    add(file, tr("Save &As…"), QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    add(file, tr("Open Auto-Save S&napshot…"), QKeySequence(), [this] { openSnapshot(); });
    file->addSeparator();
    add(file, tr("&Import Media…"), QKeySequence("Ctrl+I"), [this] { bin_->importDialog(); });
    add(file, tr("&Export Media…"), QKeySequence("Ctrl+M"), [this] { exportMedia(); });
    add(file, tr("Export &Frame…"), QKeySequence("Ctrl+Shift+E"), [this] { exportFrame(); });
    add(file, tr("&Import Timeline (FCP XML, FCPXML, OTIO, EDL)…"), QKeySequence(), [this] { importTimeline(); });
    add(file, tr("Export Final Cut Pro &7 XML (Premiere, Resolve)…"), QKeySequence(), [this] { exportInterchange(Interchange::Fcp7Xml); });
    add(file, tr("Export &FCPXML (Final Cut Pro)…"), QKeySequence(), [this] { exportInterchange(Interchange::FcpXml); });
    add(file, tr("Export E&DL (CMX 3600)…"), QKeySequence(), [this] { exportInterchange(Interchange::Edl); });
    add(file, tr("Export &OpenTimelineIO…"), QKeySequence(), [this] { exportInterchange(Interchange::Otio); });
    add(file, tr("Export &AAF for Audio Post (Pro Tools, Fairlight)…"), QKeySequence(), [this] { exportAafDialog(); })
        ->setObjectName(QStringLiteral("exportAaf"));
    file->addSeparator();
    add(file, tr("&Quit"), QKeySequence::Quit, [this] { close(); });

    // ---- Edit
    QMenu* editM = menuBar()->addMenu(tr("&Edit"));
    undo_ = add(editM, tr("&Undo"), QKeySequence::Undo, [this] { state_->undo(); });
    redo_ = add(editM, tr("&Redo"), QKeySequence("Ctrl+Shift+Z"), [this] { state_->redo(); });
    redo_->setShortcuts({QKeySequence("Ctrl+Shift+Z"), QKeySequence("Ctrl+Y")});
    editM->addSeparator();
    QAction* cut = add(editM, tr("Cu&t"), QKeySequence::Cut, [this] { copySelection(true); });
    QAction* copy = add(editM, tr("&Copy"), QKeySequence::Copy, [this] { copySelection(false); });
    QAction* pasteA = add(editM, tr("&Paste"), QKeySequence::Paste, [this] { paste(false); });
    add(editM, tr("Paste &Insert"), QKeySequence("Ctrl+Shift+V"), [this] { paste(true); });
    add(editM, tr("Paste A&ttributes…"), QKeySequence("Ctrl+Alt+V"), [this] {
        if (const unsigned what = askAttributes(tr("Paste Attributes"), false)) pasteAttributes(what);
    })->setObjectName(QStringLiteral("pasteAttributes"));
    add(editM, tr("Remove Attributes…"), QKeySequence(), [this] {
        if (const unsigned what = askAttributes(tr("Remove Attributes"), true)) removeAttributes(what);
    })->setObjectName(QStringLiteral("removeAttributes"));
    QAction* dup = add(editM, tr("D&uplicate"), QKeySequence("Ctrl+Alt+D"), [this] {
        auto sel = state_->selectedClips();
        FrameTime at = state_->playhead();
        state_->apply(tr("Duplicate"), [sel, at](Project& p, Sequence& s) { return edit::duplicateClips(p, s, sel, at); });
    });
    editM->addSeparator();
    QAction* del = add(editM, tr("&Delete (Lift)"), QKeySequence::Delete, [this] { deleteSelection(false); });
    del->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    QAction* rdel = add(editM, tr("&Ripple Delete"), QKeySequence("Shift+Delete"), [this] { deleteSelection(true); });
    rdel->setShortcuts({QKeySequence("Shift+Delete"), QKeySequence("Shift+Backspace")});
    editM->addSeparator();
    add(editM, tr("Select &All"), QKeySequence::SelectAll, withSeq([this] {
            std::vector<Id> all;
            for (TrackRef r : allTracks(*state_->sequence()))
                for (const auto& c : trackAt(*state_->sequence(), r)->clips) all.push_back(c.id);
            state_->setSelection(all, false);
        }));
    add(editM, tr("Dese&lect All"), QKeySequence("Ctrl+Shift+A"), [this] { state_->clearSelection(); });
    QAction* link = add(editM, tr("&Link"), QKeySequence("Ctrl+L"), [this] {
        auto sel = state_->selectedClips();
        state_->apply(tr("Link"), [sel](Project& p, Sequence& s) { return edit::linkClips(p, s, sel); });
    });
    QAction* unlink = add(editM, tr("U&nlink"), QKeySequence("Ctrl+Shift+L"), [this] {
        auto sel = state_->selectedClips();
        state_->apply(tr("Unlink"), [sel](Project&, Sequence& s) { return edit::unlinkClips(s, sel); });
    });
    QAction* enable = add(editM, tr("&Enable / Disable"), QKeySequence("Shift+E"), [this] {
        auto sel = state_->selectedClips();
        state_->edit(tr("Enable / Disable"), [sel](Project&, Sequence& s) {
            bool any = false;
            for (Id id : sel)
                if (Clip* c = edit::clipById(s, id)) {
                    c->enabled = !c->enabled;
                    any = true;
                }
            return any;
        });
    });

    // ---- Clip
    QMenu* clipM = menuBar()->addMenu(tr("&Clip"));
    QAction* speed = add(clipM, tr("&Speed / Duration…"), QKeySequence("Ctrl+R"), [this] { speedDialog(); });
    add(clipM, tr("Add &Edit"), QKeySequence("Ctrl+K"), [this] { addEdit(false); });
    add(clipM, tr("Add Edit to &All Tracks"), QKeySequence("Ctrl+Shift+K"), [this] { addEdit(true); });
    QAction* trans = add(clipM, tr("Apply Default &Transition"), QKeySequence("Ctrl+D"), [this] { addDefaultTransition(false); });
    add(clipM, tr("Apply Default Audio &Crossfade"), QKeySequence("Ctrl+Shift+D"), [this] { addDefaultTransition(true); });
    QAction* nest = add(clipM, tr("&Nest (Compound Clip)…"), QKeySequence(), [this] {
        auto sel = state_->selectedClips();
        if (sel.empty()) return;
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Nest"), tr("Compound clip name:"), QLineEdit::Normal, tr("Nested Sequence"), &ok);
        if (!ok || name.isEmpty()) return;
        state_->apply(tr("Nest"), [sel, name](Project& p, Sequence& s) { return edit::makeCompound(p, s, sel, name.toStdString()); });
    });
    add(clipM, tr("Detect &Scene Cuts"), QKeySequence(), [this] { detectScenes(); });
    add(clipM, tr("Find Similar S&hots"), QKeySequence(), [this] { findSimilarShots(); })->setObjectName(QStringLiteral("findSimilarShots"));
    add(clipM, tr("Checkerboard Dialogue by Speaker"), QKeySequence(), withSeq([this] {
            // The selected audio clips, each split where the speaker changes, a track per person.
            std::vector<Id> clips;
            for (Id id : state_->selectedClips())
                if (auto loc = edit::locate(*state_->sequence(), id); loc && loc->track.kind == TrackKind::Audio) clips.push_back(id);
            if (clips.empty()) {
                statusBar()->showMessage(tr("Select the dialogue clips to split by speaker"), 5000);
                return;
            }
            int people = 0;
            QString why;
            const bool ok = state_->apply(tr("Checkerboard by Speaker"), [&](Project& p, Sequence& s) {
                edit::Result all;
                for (Id id : clips) {
                    int n = 0;
                    const edit::Result r = checkerboardBySpeaker(p, s, id, &n);
                    if (!r.ok) {
                        why = QString::fromStdString(r.error);
                        continue;
                    }
                    people = std::max(people, n);
                    all.created.insert(all.created.end(), r.created.begin(), r.created.end());
                }
                if (all.created.empty()) return edit::Result::fail(why.toStdString());
                return all;
            });
            statusBar()->showMessage(ok ? tr("Split by speaker: %n people, each on their own track", "", people) : why, 6000);
        }))->setObjectName(QStringLiteral("checkerboardBySpeaker"));
    add(clipM, tr("Normalize &Loudness…"), QKeySequence(), [this] { normalizeLoudness(); });
    add(clipM, tr("Auto &Duck Music…"), QKeySequence(), withSeq([this] {
            // The selected audio clips are the music.
            std::vector<Id> music;
            for (Id id : state_->selectedClips())
                if (auto loc = edit::locate(*state_->sequence(), id); loc && loc->track.kind == TrackKind::Audio) music.push_back(id);
            if (music.empty()) {
                statusBar()->showMessage(tr("Select the music clips to duck"), 5000);
                return;
            }
            AutoDuckDialog dlg(state_, music, this);
            if (dlg.exec() == QDialog::Accepted) AutoDuckDialog::apply(state_, music, dlg.dialogueTracks(), dlg.options(), this);
        }))->setObjectName(QStringLiteral("autoDuck"));
    add(clipM, tr("Auto &Colour"), QKeySequence("Ctrl+Alt+C"), [this] { autoColor(); });
    add(clipM, tr("Set Colour &Reference"), QKeySequence(), [this] { setColourReference(); })
        ->setObjectName(QStringLiteral("setColourReference"));
    add(clipM, tr("Match Colour to Reference"), QKeySequence("Ctrl+Alt+Shift+C"), [this] { matchColour(); })
        ->setObjectName(QStringLiteral("matchColour"));
    compareRef_ = add(clipM, tr("Compare with Reference"), QKeySequence(), [this] { setCompareWithReference(compareRef_->isChecked()); });
    compareRef_->setCheckable(true);
    compareRef_->setObjectName(QStringLiteral("compareReference"));
    add(clipM, tr("Auto Reframe"), QKeySequence(), [this] { autoReframeClips(); })->setObjectName(QStringLiteral("autoReframeClips"));
    add(clipM, tr("Add Frame &Hold"), QKeySequence("Shift+F"), [this] { addFrameHold(); })->setObjectName(QStringLiteral("addFrameHold"));
    add(clipM, tr("Replace with Source Clip"), QKeySequence(), [this] { replaceWithSource(); })
        ->setObjectName(QStringLiteral("replaceWithSource"));
    add(clipM, tr("Fit to Fill"), QKeySequence(), [this] { fitToFill(); })->setObjectName(QStringLiteral("fitToFill"));
    add(clipM, tr("Set Voice Reference"), QKeySequence(), [this] { setVoiceReference(); })->setObjectName(QStringLiteral("setVoiceReference"));
    add(clipM, tr("Match Voice to Reference"), QKeySequence(), [this] { matchVoice(); })->setObjectName(QStringLiteral("matchVoice"));
    add(clipM, tr("Add Bar Markers"), QKeySequence(), [this] { addBeatMarkers(false); })->setObjectName(QStringLiteral("addBarMarkers"));
    add(clipM, tr("Add Beat Markers"), QKeySequence(), [this] { addBeatMarkers(true); })->setObjectName(QStringLiteral("addBeatMarkers"));
    add(clipM, tr("Fit Music to Length…"), QKeySequence(), [this] { fitMusicDialog(); })->setObjectName(QStringLiteral("fitMusic"));
    add(clipM, tr("Cut Selected Media to the Beat"), QKeySequence(), [this] { cutMediaToBeat(1, true); })
        ->setObjectName(QStringLiteral("cutToBeat"));
    add(clipM, tr("S&ynchronize by Audio"), QKeySequence(), [this] { syncByAudio(); });
    clipM->addSeparator();
    add(clipM, tr("&Insert from Source"), QKeySequence(Qt::Key_Comma), [this] { state_->insertFromSource(false); });
    add(clipM, tr("&Overwrite from Source"), QKeySequence(Qt::Key_Period), [this] { state_->insertFromSource(true); });
    add(clipM, tr("&Match Frame"), QKeySequence(Qt::Key_F), [this] { matchFrame(); });
    {
        QAction* sub = add(clipM, tr("Make S&ubclip"), QKeySequence("Ctrl+U"), [this] { makeSubclip(); });
        sub->setToolTip(tr("Save the Source monitor's In to Out as a subclip in the media bin"));
        sub->setObjectName(QStringLiteral("makeSubclip"));
    }
    add(clipM, tr("Nudge &Left"), QKeySequence("Alt+Left"), [this] { nudge(-1); });
    add(clipM, tr("Nudge &Right"), QKeySequence("Alt+Right"), [this] { nudge(1); });
    clipM->addSeparator();
    add(clipM, tr("New &Title"), QKeySequence("Ctrl+T"), [this] { addTitle(); });
    add(clipM, tr("New &Colour Matte"), QKeySequence(), withSeq([this] {
            const Sequence* s = state_->sequence();
            FrameTime at = s->playhead, len = FrameTime(std::llround(5 * s->fpsValue()));
            int vt = state_->targetVideoTrack();
            state_->apply(tr("New Colour Matte"), [=](Project& p, Sequence& sq) {
                Clip c = makeGeneratorClip(p, "color", len);
                c.start = at;
                return edit::overwrite(p, sq, {TrackKind::Video, std::min(vt, int(sq.videoTracks.size()) - 1)}, c);
            });
        }));

    add(clipM, tr("New &Adjustment Layer"), QKeySequence(), withSeq([this] {
            // Above the targeted track (a new track if it is the top one), so it adjusts what is below.
            const Sequence* s = state_->sequence();
            FrameTime at = s->playhead, len = FrameTime(std::llround(5 * s->fpsValue()));
            int vt = state_->targetVideoTrack() + 1;
            state_->apply(tr("New Adjustment Layer"), [=](Project& p, Sequence& sq) {
                TrackRef track{TrackKind::Video, std::min(vt, int(sq.videoTracks.size()))};
                if (track.index == int(sq.videoTracks.size())) track = edit::addTrack(p, sq, TrackKind::Video);
                Clip c = makeGeneratorClip(p, "adjustment", len);
                c.start = at;
                return edit::overwrite(p, sq, track, c);
            });
        }))->setObjectName(QStringLiteral("newAdjustmentLayer"));

    // ---- Sequence
    QMenu* seqM = menuBar()->addMenu(tr("&Sequence"));
    add(seqM, tr("Sequence &Settings…"), QKeySequence(), [this] { SequenceSettingsDialog::editActive(state_, this); });
    add(seqM, tr("&New Sequence…"), QKeySequence("Ctrl+Alt+N"), [this] { newSequence(); });
    add(seqM, tr("&Duplicate Sequence"), QKeySequence(), [this] {
        const Sequence* s = state_->sequence();
        if (!s) return;
        Id made = 0;
        const Id from = s->id;
        state_->edit(tr("Duplicate Sequence"), [&made, from](Project& p, Sequence&) {
            made = edit::duplicateSequence(p, from);
            return made != 0;
        });
        if (made) state_->setActiveSequence(made);
    })->setObjectName(QStringLiteral("duplicateSequence"));
    add(seqM, tr("Auto &Reframe Sequence…"), QKeySequence(), [this] { autoReframeDialog(); })
        ->setObjectName(QStringLiteral("autoReframeSequence"));
    add(seqM, tr("Build Cut from &Script…"), QKeySequence(), [this] { scriptCutDialog(); })
        ->setObjectName(QStringLiteral("buildScriptCut"));
    add(seqM, tr("Auto &Mix…"), QKeySequence(), [this] { AutoMixDialog::run(state_, this); })->setObjectName(QStringLiteral("autoMix"));
    add(seqM, tr("Record &Voiceover…"), QKeySequence("Ctrl+Alt+R"), [this] {
        if (!state_->sequence()) return;
        // One dialog, kept open beside the work while takes are recorded.
        if (!voiceover_) voiceover_ = new VoiceoverDialog(state_, program_, this);
        voiceover_->show();
        voiceover_->raise();
    })->setObjectName(QStringLiteral("recordVoiceover"));
    add(seqM, tr("&Generate Voiceover…"), QKeySequence("Ctrl+Alt+G"), [this] {
        if (!state_->sequence()) return;
        SpeechDialog dlg(state_, this);
        dlg.exec();
    })->setObjectName(QStringLiteral("generateVoiceover"));
    add(seqM, tr("Render In to Out"), QKeySequence(Qt::Key_Return), [this] { renderInToOut(); })
        ->setObjectName(QStringLiteral("renderInToOut"));
    add(seqM, tr("Delete Render Files"), QKeySequence(), [this] { deleteRenderFiles(); })
        ->setObjectName(QStringLiteral("deleteRenderFiles"));
    add(seqM, tr("Add &Video Track"), QKeySequence(), [this] {
        state_->edit(tr("Add Video Track"), [](Project& p, Sequence& s) {
            edit::addTrack(p, s, TrackKind::Video);
            return true;
        });
    });
    add(seqM, tr("Add &Audio Track"), QKeySequence(), [this] {
        state_->edit(tr("Add Audio Track"), [](Project& p, Sequence& s) {
            edit::addTrack(p, s, TrackKind::Audio);
            return true;
        });
    });
    seqM->addSeparator();
    add(seqM, tr("Mark &In"), QKeySequence(Qt::Key_I), [this] {
        if (active_ == Monitor::Source) state_->setSourceIn(source_->position());
        else state_->setInPoint(program_->position());
    });
    add(seqM, tr("Mark &Out"), QKeySequence(Qt::Key_O), [this] {
        if (active_ == Monitor::Source) state_->setSourceOut(source_->position());
        else state_->setOutPoint(program_->position());
    });
    add(seqM, tr("Mark Clip"), QKeySequence(Qt::Key_X), [this] { markClip(); });
    add(seqM, tr("&Clear In and Out"), QKeySequence("Ctrl+Shift+X"), [this] {
        if (active_ == Monitor::Source) {
            state_->setSourceIn(-1);
            state_->setSourceOut(-1);
        } else {
            state_->setInPoint(-1);
            state_->setOutPoint(-1);
        }
    });
    add(seqM, tr("Go to In"), QKeySequence("Shift+I"), [this] {
        FrameTime t = active_ == Monitor::Source ? state_->sourceIn() : (state_->sequence() ? state_->sequence()->inPoint : -1);
        if (t >= 0) activeController()->seek(t);
    });
    add(seqM, tr("Go to Out"), QKeySequence("Shift+O"), [this] {
        FrameTime t = active_ == Monitor::Source ? state_->sourceOut() : (state_->sequence() ? state_->sequence()->outPoint : -1);
        if (t >= 0) activeController()->seek(t);
    });
    add(seqM, tr("&Lift"), QKeySequence(Qt::Key_Semicolon), withSeq([this] {
            FrameTime a = state_->sequence()->inPoint, b = state_->sequence()->outPoint + 1;
            state_->apply(tr("Lift"), [a, b](Project& p, Sequence& s) { return edit::liftRange(p, s, a, b, allTracks(s)); });
        }));
    add(seqM, tr("E&xtract"), QKeySequence(Qt::Key_Apostrophe), withSeq([this] {
            FrameTime a = state_->sequence()->inPoint, b = state_->sequence()->outPoint + 1;
            state_->apply(tr("Extract"), [a, b](Project& p, Sequence& s) { return edit::extractRange(p, s, a, b, allTracks(s)); });
        }));
    seqM->addSeparator();
    add(seqM, tr("Ripple Trim Previous Edit to Playhead"), QKeySequence(Qt::Key_Q), [this] { rippleTrimToPlayhead(true); })
        ->setObjectName(QStringLiteral("rippleTrimPrevious"));
    add(seqM, tr("Ripple Trim Next Edit to Playhead"), QKeySequence(Qt::Key_W), [this] { rippleTrimToPlayhead(false); })
        ->setObjectName(QStringLiteral("rippleTrimNext"));
    add(seqM, tr("Select Clips After Playhead"), QKeySequence(Qt::Key_A), [this] { selectForward(true); })
        ->setObjectName(QStringLiteral("selectForward"));
    add(seqM, tr("Select Clips After Playhead on Target Track"), QKeySequence("Shift+A"), [this] { selectForward(false); })
        ->setObjectName(QStringLiteral("selectForwardTrack"));
    add(seqM, tr("Add &Marker"), QKeySequence(Qt::Key_M), [this] { addMarker(); });
    add(seqM, tr("Next Marker"), QKeySequence("Shift+M"), [this] { jumpMarker(true); });
    add(seqM, tr("Previous Marker"), QKeySequence("Ctrl+Shift+M"), [this] { jumpMarker(false); });
    add(seqM, tr("Clear Marker at Playhead"), QKeySequence("Ctrl+Alt+M"), [this] {
        FrameTime t = state_->playhead();
        state_->edit(tr("Clear Marker"), [t](Project&, Sequence& s) { return edit::removeMarkerAt(s, t); });
    });
    seqM->addSeparator();
    snapping_ = add(seqM, tr("S&napping"), QKeySequence(Qt::Key_S), [this] { state_->setSnapping(snapping_->isChecked()); });
    snapping_->setCheckable(true);
    snapping_->setChecked(true);
    {
        // Lines over clips for volume and opacity, with their keyframes.
        const bool volume = appSettings().value("timeline/volumeLines", true).toBool();
        const bool opacity = appSettings().value("timeline/opacityLines", false).toBool();
        timeline_->setShowVolumeLines(volume);
        timeline_->setShowOpacityLines(opacity);
        QAction* v = add(seqM, tr("Show Clip &Volume"), QKeySequence(), [this](bool on) {
            timeline_->setShowVolumeLines(on);
            appSettings().setValue("timeline/volumeLines", on);
        });
        v->setCheckable(true);
        v->setChecked(volume);
        v->setObjectName(QStringLiteral("showVolumeLines"));
        v->setToolTip(tr("Draw each audio clip's volume as a line: drag it, Ctrl/Cmd-click to add a keyframe, Alt-click a keyframe to delete it"));
        QAction* o = add(seqM, tr("Show Clip &Opacity"), QKeySequence(), [this](bool on) {
            timeline_->setShowOpacityLines(on);
            appSettings().setValue("timeline/opacityLines", on);
        });
        o->setCheckable(true);
        o->setChecked(opacity);
        o->setObjectName(QStringLiteral("showOpacityLines"));
    }
    add(seqM, tr("Zoom &In"), QKeySequence(Qt::Key_Equal), [this] { timeline_->zoomIn(); });
    add(seqM, tr("Zoom &Out"), QKeySequence(Qt::Key_Minus), [this] { timeline_->zoomOut(); });
    add(seqM, tr("Zoom to &Fit"), QKeySequence(Qt::Key_Backslash), [this] { timeline_->zoomToFit(); });

    // ---- Playback
    QMenu* play = menuBar()->addMenu(tr("&Playback"));
    {
        // Hardware decoding (on by default; MONTAGE_HWACCEL=off overrides the saved choice).
        const bool hw = qEnvironmentVariable("MONTAGE_HWACCEL") != "off" &&
                        appSettings().value("playback/hardwareDecoding", true).toBool();
        setHwDecodeMode(hw ? HwDecodeMode::Auto : HwDecodeMode::Off);
        QAction* a = add(play, tr("&Hardware Decoding"), QKeySequence(), [this](bool on) {
            setHwDecodeMode(on ? HwDecodeMode::Auto : HwDecodeMode::Off);
            appSettings().setValue("playback/hardwareDecoding", on);
            MediaPool::instance().clear();  // reopen decoders with the new setting
            syncProgram();
            program_->seek(state_->playhead());
            statusBar()->showMessage(on ? tr("Hardware decoding on: video decodes on the GPU or media engine where supported")
                                        : tr("Hardware decoding off: all video decodes on the CPU"),
                                     5000);
        });
        a->setCheckable(true);
        a->setChecked(hw);
        a->setToolTip(tr("Decode H.264, HEVC, VP9, AV1 and ProRes on the GPU or media engine "
                         "(VideoToolbox, D3D11, NVDEC, VAAPI) where supported"));
        play->addSeparator();
    }
    add(play, tr("&Play / Pause"), QKeySequence(Qt::Key_Space), [this] { activeController()->togglePlay(); });
    add(play, tr("Shuttle &Reverse"), QKeySequence(Qt::Key_J), [this] { activeController()->shuttle(-1); });
    add(play, tr("&Stop"), QKeySequence(Qt::Key_K), [this] { activeController()->shuttle(0); });
    add(play, tr("Shuttle &Forward"), QKeySequence(Qt::Key_L), [this] { activeController()->shuttle(1); });
    play->addSeparator();
    add(play, tr("Step Back"), QKeySequence(Qt::Key_Left), [this] { activeController()->step(-1); });
    add(play, tr("Step Forward"), QKeySequence(Qt::Key_Right), [this] { activeController()->step(1); });
    add(play, tr("Back 5 Frames"), QKeySequence("Shift+Left"), [this] { activeController()->step(-5); });
    add(play, tr("Forward 5 Frames"), QKeySequence("Shift+Right"), [this] { activeController()->step(5); });
    add(play, tr("Previous Edit"), QKeySequence(Qt::Key_Up), withSeq([this] {
            program_->seek(edit::prevEdit(*state_->sequence(), state_->playhead()));
        }));
    add(play, tr("Next Edit"), QKeySequence(Qt::Key_Down), withSeq([this] {
            program_->seek(edit::nextEdit(*state_->sequence(), state_->playhead()));
        }));
    add(play, tr("Go to Start"), QKeySequence(Qt::Key_Home), [this] { activeController()->seek(0); });
    add(play, tr("Go to End"), QKeySequence(Qt::Key_End), [this] {
        const Sequence* s = activeController()->sequence();
        if (s) activeController()->seek(s->duration());
    });

    // ---- Tools (menu + toolbar)
    QMenu* toolsM = menuBar()->addMenu(tr("&Tools"));
    QToolBar* tb = addToolBar(tr("Tools"));
    tb->setObjectName("tools");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* group = new QActionGroup(this);
    const std::tuple<TimelineWidget::Tool, QString, QString, Qt::Key> tools[] = {
        {TimelineWidget::Tool::Select, tr("Select"), tr("Selection tool: move, trim, select"), Qt::Key_V},
        {TimelineWidget::Tool::Razor, tr("Razor"), tr("Razor: cut clips (Shift = all tracks)"), Qt::Key_C},
        {TimelineWidget::Tool::Ripple, tr("Ripple"), tr("Ripple edit: trim and shift following clips"), Qt::Key_B},
        {TimelineWidget::Tool::Roll, tr("Roll"), tr("Rolling edit: move a cut between two clips"), Qt::Key_N},
        {TimelineWidget::Tool::Slip, tr("Slip"), tr("Slip: change a clip's content, not its position"), Qt::Key_Y},
        {TimelineWidget::Tool::Slide, tr("Slide"), tr("Slide: move a clip between its neighbours"), Qt::Key_U},
        {TimelineWidget::Tool::Hand, tr("Hand"), tr("Hand: pan the timeline"), Qt::Key_H},
    };
    for (const auto& [tool, name, tip, key] : tools) {
        TimelineWidget::Tool t = tool;
        QAction* a = add(toolsM, name, QKeySequence(key), [this, t] { timeline_->setTool(t); });
        a->setCheckable(true);
        a->setToolTip(QString("%1 (%2)").arg(tip, QKeySequence(key).toString()));
        group->addAction(a);
        tb->addAction(a);
        toolActions_.push_back({tool, a});
    }
    toolActions_.front().second->setChecked(true);
    connect(timeline_, &TimelineWidget::toolChanged, this, [this](TimelineWidget::Tool t) {
        for (auto& [tool, a] : toolActions_) a->setChecked(tool == t);
    });
    toolsM->addSeparator();
    add(toolsM, tr("Audio &Plugins…"), QKeySequence(), [this] {
        auto* dlg = new PluginManagerDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &PluginManagerDialog::pluginsChanged, effects_, &EffectsBrowser::reload);
        dlg->show();
    });
    tb->addSeparator();
    tb->addAction(snapping_);
    tb->addSeparator();
    tb->addAction(undo_);
    tb->addAction(redo_);

    // ---- Window & Help
    windowMenu_ = menuBar()->addMenu(tr("&Window"));
    for (QDockWidget* d : docks_) windowMenu_->addAction(d->toggleViewAction());
    windowMenu_->addSeparator();
    add(windowMenu_, tr("&Reset Layout"), QKeySequence(), [this] { resetLayout(); });
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    add(help, tr("&Keyboard Shortcuts"), QKeySequence(Qt::Key_F1), [this] { showShortcuts(); });
    add(help, tr("&About Montage"), QKeySequence(), [this] { about(); });

    // Timeline context menus reuse the actions.
    auto* sep1 = new QAction(this);
    sep1->setSeparator(true);
    auto* sep2 = new QAction(this);
    sep2->setSeparator(true);
    auto* sep3 = new QAction(this);
    sep3->setSeparator(true);
    timeline_->setClipContextActions({cut, copy, pasteA, dup, sep1, del, rdel, sep2, enable, link, unlink, speed, trans, nest, sep3});
    auto* closeGap = new QAction(tr("Close Gap"), this);
    connect(closeGap, &QAction::triggered, this, [this] {
        auto t = timeline_->contextTrack();
        if (!t) return;
        TrackRef ref = *t;
        FrameTime f = timeline_->contextFrame();
        state_->apply(tr("Close Gap"), [ref, f](Project& p, Sequence& s) { return edit::closeGap(p, s, ref, f); });
    });
    auto* pasteHere = new QAction(tr("Paste Here"), this);
    connect(pasteHere, &QAction::triggered, this, [this] {
        state_->setPlayhead(timeline_->contextFrame());
        paste(false);
    });
    timeline_->setEmptyContextActions({pasteHere, closeGap});
}

void MainWindow::updateActions() {
    undo_->setEnabled(state_->canUndo());
    redo_->setEnabled(state_->canRedo());
    undo_->setText(state_->canUndo() ? tr("&Undo %1").arg(state_->undoText()) : tr("&Undo"));
    redo_->setText(state_->canRedo() ? tr("&Redo %1").arg(state_->redoText()) : tr("&Redo"));
}

void MainWindow::updateTitle() {
    QString name = state_->filePath().isEmpty() ? tr("Untitled") : QFileInfo(state_->filePath()).completeBaseName();
    setWindowTitle(QString("%1[*] — Montage").arg(name));
    setWindowModified(state_->isModified());
}

PlaybackController* MainWindow::activeController() const { return active_ == Monitor::Source ? source_ : program_; }

// ---------------------------------------------------------------------------
// Monitors

void MainWindow::syncProgram() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    program_->setProject(state_->project(), s->id);
    program_->requestFrame();
}

void MainWindow::rebuildSourceProject() {
    const MediaItem* m = state_->project().findMedia(state_->sourceMedia());
    const Sequence* main = state_->sequence();
    if (!m || !main) {
        sourcePanel_->viewer()->setImage(QImage());
        source_->setProject(makeDefaultProject(), 0);
        sourcePanel_->refresh();
        return;
    }
    Project tmp = state_->project();
    int w = m->hasVideo && m->width > 0 ? m->width : main->width;
    int h = m->hasVideo && m->height > 0 ? m->height : main->height;
    Sequence ss = makeSequence(tmp, "Source", w, h, main->fps, 1, 1);
    ss.sampleRate = main->sampleRate;
    edit::placeMedia(tmp, ss, m->id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
    sourceSequence_ = ss.id;
    tmp.sequences.push_back(ss);
    bool same = source_->sequence() && source_->sequence()->duration() == ss.duration();
    FrameTime pos = same ? source_->position() : 0;
    source_->pause();
    source_->setProject(tmp, sourceSequence_);
    source_->seek(pos);
    source_->requestFrame();
    sourceDock_->setWindowTitle(tr("Source: %1").arg(QString::fromStdString(m->name)));
    sourcePanel_->refresh();
}

void MainWindow::openInSource(Id media) {
    state_->setSourceMedia(media);
    if (state_->sourceIn() >= 0) source_->seek(state_->sourceIn());  // a subclip: at its start
    sourceDock_->raise();
    active_ = Monitor::Source;
}

// Saves the Source monitor's In–Out as a subclip in the bin.
Id MainWindow::makeSubclip() {
    const Id media = state_->sourceMedia();
    const MediaItem* m = state_->project().findMedia(media);
    if (!m) {
        statusBar()->showMessage(tr("Open a clip in the Source monitor and mark In and Out first"), 5000);
        return 0;
    }
    const double fps = state_->sequence() ? state_->sequence()->fpsValue() : 30.0;
    const FrameTime in = std::max<FrameTime>(0, state_->sourceIn());
    const FrameTime out = state_->sourceOut() >= 0 ? state_->sourceOut() : FrameTime(std::ceil(m->duration * fps)) - 1;
    const Id id = state_->makeSubclip(media, in, out);
    if (!id) {
        statusBar()->showMessage(tr("Only video and audio files can have subclips"), 5000);
        return 0;
    }
    statusBar()->showMessage(tr("Subclip \"%1\" added to the media bin").arg(QString::fromStdString(state_->project().findMedia(id)->name)), 5000);
    return id;
}

// ---------------------------------------------------------------------------
// File commands

bool MainWindow::maybeSave() {
    if (!state_->isModified()) return true;
    auto r = QMessageBox::warning(this, tr("Montage"), tr("The project has unsaved changes. Save them?"),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::newProject() {
    if (!maybeSave()) return;
    program_->pause();
    state_->newProject();
}

void MainWindow::openDialog() {
    if (!maybeSave()) return;
    QSettings s = appSettings();
    QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), s.value("lastProjectDir").toString(),
                                                tr("Montage projects (*.montage);;All files (*)"));
    if (!path.isEmpty()) openProject(path);
}

bool MainWindow::openProject(const QString& path) {
    QString err;
    program_->pause();
    if (!state_->open(path, &err)) {
        QMessageBox::warning(this, tr("Open Project"), err);
        return false;
    }
    addRecent(path);
    appSettings().setValue("lastProjectDir", QFileInfo(path).absolutePath());
    timeline_->zoomToFit();
    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    return true;
}

bool MainWindow::save() {
    if (state_->filePath().isEmpty()) return saveAs();
    QString err;
    if (!state_->save(state_->filePath(), &err)) {
        QMessageBox::warning(this, tr("Save"), err);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(state_->filePath()), 3000);
    return true;
}

bool MainWindow::saveAs() {
    QSettings s = appSettings();
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project"), s.value("lastProjectDir").toString() + "/Untitled.montage",
                                                tr("Montage projects (*.montage)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".montage")) path += ".montage";
    QString err;
    if (!state_->save(path, &err)) {
        QMessageBox::warning(this, tr("Save"), err);
        return false;
    }
    addRecent(path);
    s.setValue("lastProjectDir", QFileInfo(path).absolutePath());
    return true;
}

void MainWindow::addRecent(const QString& path) {
    QSettings s = appSettings();
    QStringList recent = s.value("recentProjects").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 10) recent.removeLast();
    s.setValue("recentProjects", recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    QStringList recent = appSettings().value("recentProjects").toStringList();
    for (const QString& p : recent)
        recentMenu_->addAction(QFileInfo(p).fileName(), this, [this, p] {
            if (maybeSave()) openProject(p);
        })->setToolTip(p);
    recentMenu_->setEnabled(!recent.isEmpty());
}

void MainWindow::exportMedia() {
    program_->pause();
    if (!state_->sequence() || state_->sequence()->duration() == 0) {
        QMessageBox::information(this, tr("Export"), tr("The sequence is empty."));
        return;
    }
    ExportDialog dlg(state_, this);
    dlg.setQueue(queue_);
    dlg.exec();
    if (!queue_->jobs().empty() && queue_->jobs().back().status == RenderQueue::Status::Waiting && !queue_->running()) {
        queueDock_->show();
        queueDock_->raise();
    }
}

void MainWindow::exportFrame() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    QSettings st = appSettings();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Frame"), st.value("lastExportDir").toString() + "/frame.png",
                                                tr("Images (*.png *.jpg *.tif)"));
    if (path.isEmpty()) return;
    std::string err;
    if (exportStill(state_->project(), *s, state_->playhead(), path.toStdString(), &err))
        statusBar()->showMessage(tr("Saved frame to %1").arg(path), 4000);
    else QMessageBox::warning(this, tr("Export Frame"), QString::fromStdString(err));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (queue_->running() &&
        QMessageBox::question(this, tr("Render Queue"), tr("Renders are in progress. Stop them and quit?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    queue_->stop();
    queue_->waitForIdle();
    program_->pause();
    source_->pause();
    recovery_->endSession();  // a clean exit: nothing to recover next time
    QSettings s = appSettings();
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState(1));
    e->accept();
}

// ---------------------------------------------------------------------------
// Editing commands

void MainWindow::copySelection(bool cut) {
    const Sequence* s = state_->sequence();
    if (!s || state_->selectedClips().empty()) return;
    clipboard_ = edit::copyClips(*s, state_->selectedClips());
    if (cut) deleteSelection(false);
    statusBar()->showMessage(tr("%n clip(s) copied", "", int(clipboard_.size())), 2000);
}

void MainWindow::paste(bool insertMode) {
    if (clipboard_.empty()) return;
    auto items = clipboard_;
    FrameTime at = state_->playhead();
    std::vector<Id> created;
    bool ok = state_->apply(insertMode ? tr("Paste Insert") : tr("Paste"), [&](Project& p, Sequence& s) {
        auto r = edit::pasteClips(p, s, items, at, insertMode);
        created = r.created;
        return r;
    });
    if (ok) state_->setSelection(created, false);
}

void MainWindow::deleteSelection(bool ripple) {
    if (Id t = state_->selectedTransition()) {
        state_->apply(tr("Delete Transition"), [t](Project&, Sequence& s) { return edit::removeTransition(s, t); });
        return;
    }
    auto sel = state_->selectedClips();
    if (sel.empty()) return;
    state_->apply(ripple ? tr("Ripple Delete") : tr("Delete"), [sel, ripple](Project& p, Sequence& s) {
        return edit::removeClips(p, s, sel, ripple);
    });
}

void MainWindow::addEdit(bool allTracks) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime t = state_->playhead();
    if (allTracks) {
        state_->apply(tr("Add Edit to All Tracks"), [t](Project& p, Sequence& sq) { return edit::razorAll(p, sq, t); });
        return;
    }
    // Cut the selected clips under the playhead, or the targeted tracks if nothing is selected.
    std::vector<TrackRef> tracks;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id)) {
            const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
            if (c.contains(t)) tracks.push_back(loc->track);
        }
    if (tracks.empty()) tracks = {{TrackKind::Video, state_->targetVideoTrack()}, {TrackKind::Audio, state_->targetAudioTrack()}};
    state_->apply(tr("Add Edit"), [t, tracks](Project& p, Sequence& sq) {
        edit::Result last = edit::Result::fail("No clip under the playhead");
        for (TrackRef r : tracks) {
            auto res = edit::razor(p, sq, r, t);
            if (res.ok) last = res;
        }
        return last;
    });
}

void MainWindow::addDefaultTransition(bool audio) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime t = state_->playhead();
    TrackRef ref{audio ? TrackKind::Audio : TrackKind::Video, audio ? state_->targetAudioTrack() : state_->targetVideoTrack()};
    // Prefer a selected clip's nearest edge; otherwise the edit point nearest the playhead on the targeted track.
    Id clipId = 0;
    edit::Edge edge = edit::Edge::In;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == ref.kind) {
            const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
            clipId = c.id;
            edge = std::llabs(t - c.start) <= std::llabs(c.end() - t) ? edit::Edge::In : edit::Edge::Out;
            break;
        }
    if (!clipId) {
        const Track* tr = trackAt(*s, ref);
        if (!tr) return;
        FrameTime best = std::numeric_limits<FrameTime>::max();
        for (const Clip& c : tr->clips) {
            if (std::llabs(c.start - t) < best) {
                best = std::llabs(c.start - t);
                clipId = c.id;
                edge = edit::Edge::In;
            }
            if (std::llabs(c.end() - t) < best) {
                best = std::llabs(c.end() - t);
                clipId = c.id;
                edge = edit::Edge::Out;
            }
        }
    }
    if (!clipId) {
        state_->message(tr("No clip on the targeted track"));
        return;
    }
    FrameTime dur = FrameTime(std::llround(s->fpsValue()));
    std::string type = audio ? "crossfade" : "cross_dissolve";
    state_->apply(audio ? tr("Add Audio Crossfade") : tr("Add Transition"),
                  [=](Project& p, Sequence& sq) { return edit::addTransition(p, sq, clipId, edge, type, dur); });
}

void MainWindow::speedDialog() {
    const Clip* c = state_->primaryClip();
    if (!c) return;
    bool ok = false;
    double pct = QInputDialog::getDouble(this, tr("Speed / Duration"), tr("Speed (%):"), c->speed * 100, 1, 10000, 1, &ok);
    if (!ok) return;
    auto sel = state_->selectedClips();
    double sp = pct / 100.0;
    state_->apply(tr("Speed / Duration"), [sel, sp](Project& p, Sequence& s) {
        // One call per link group: setSpeed changes linked partners itself and ripples once.
        edit::Result last;
        std::set<Id> done;
        for (Id id : sel) {
            const Clip* cc = edit::clipById(s, id);
            if (!cc || done.count(id)) continue;
            for (Id l : edit::linkedClips(s, id)) done.insert(l);
            last = edit::setSpeed(p, s, id, sp, true, cc->reverse);
            if (!last.ok) return last;
        }
        return last;
    });
}

void MainWindow::nudge(int frames) {
    auto sel = state_->selectedClips();
    if (sel.empty()) return;
    state_->apply(tr("Nudge"), [sel, frames](Project& p, Sequence& s) { return edit::moveClips(p, s, sel, frames, 0, 0); });
}

void MainWindow::matchFrame() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime t = state_->playhead();
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i) {
        const Clip* c = edit::clipAt(*s, {TrackKind::Video, i}, t);
        if (!c || c->isGenerator() || s->videoTracks[size_t(i)].muted) continue;
        FrameTime srcFrame = FrameTime(std::floor(c->sourceFrameAt(t)));
        openInSource(c->mediaId);
        source_->seek(srcFrame);
        return;
    }
    state_->message(tr("No video clip under the playhead"));
}

void MainWindow::addMarker() {
    if (active_ == Monitor::Source) return;
    FrameTime t = state_->playhead();
    int n = state_->sequence() ? int(state_->sequence()->markers.size()) + 1 : 1;
    state_->edit(tr("Add Marker"), [t, n](Project&, Sequence& s) {
        edit::addMarker(s, Marker{t, 0, "Marker " + std::to_string(n), "", 0});
        return true;
    });
}

void MainWindow::jumpMarker(bool forward) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime t = state_->playhead(), best = -1;
    for (const auto& m : s->markers) {
        if (forward && m.t > t && (best < 0 || m.t < best)) best = m.t;
        if (!forward && m.t < t && m.t > best) best = m.t;
    }
    if (best >= 0) program_->seek(best);
}

void MainWindow::markClip() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime t = state_->playhead();
    const Clip* c = edit::clipAt(*s, {TrackKind::Video, state_->targetVideoTrack()}, t);
    if (!c) c = edit::clipAt(*s, {TrackKind::Audio, state_->targetAudioTrack()}, t);
    if (!c) return;
    state_->setInPoint(c->start);
    state_->setOutPoint(c->end() - 1);
}

void MainWindow::detectScenes() {
    const Clip* c = state_->primaryClip();
    const Sequence* s = state_->sequence();
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video || !s) {
        state_->message(tr("Select a video clip to detect scene cuts in"));
        return;
    }
    const Id clipId = c->id;
    const std::string path = m->path;
    auto* dlg = new QProgressDialog(tr("Detecting scene cuts in %1...").arg(QString::fromStdString(m->name)), tr("Cancel"), 0, 1000, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(300);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    QPointer<QProgressDialog> guard(dlg);
    auto* watcher = new QFutureWatcher<std::vector<double>>(this);
    connect(watcher, &QFutureWatcher<std::vector<double>>::finished, this, [this, watcher, dlg, clipId, cancel] {
        std::vector<double> cuts = watcher->result();
        dlg->close();
        dlg->deleteLater();
        watcher->deleteLater();
        if (*cancel) return;
        int made = 0;
        state_->apply(tr("Detect Scene Cuts"), [&](Project& p, Sequence& sq) {
            const Clip* clip = edit::clipById(sq, clipId);
            if (!clip) return edit::Result::fail("The clip is gone");
            // Map source times to timeline frames inside the clip.
            std::vector<FrameTime> frames;
            for (double sec : cuts) {
                double src = sec * sq.fpsValue();
                double local = clip->ramped() ? clip->localForSource(src) : (src - clip->sourceIn) / clip->speed;
                if (clip->reverse) local = double(clip->duration) - local;
                FrameTime f = clip->start + FrameTime(std::llround(local));
                if (f > clip->start && f < clip->end()) frames.push_back(f);
            }
            std::vector<TrackRef> tracks;
            for (Id l : edit::linkedClips(sq, clipId))
                if (auto loc = edit::locate(sq, l)) tracks.push_back(loc->track);
            for (FrameTime f : frames) {
                std::map<Id, Id> regroup;
                for (TrackRef t : tracks) {
                    const Clip* under = edit::clipAt(sq, t, f);
                    Id group = under ? under->linkGroup : 0;
                    auto r = edit::razor(p, sq, t, f);
                    if (r.ok && group && !r.created.empty()) {
                        auto it = regroup.find(group);
                        if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
                        if (Clip* rc = edit::clipById(sq, r.created[0])) rc->linkGroup = it->second;
                    }
                }
                ++made;
            }
            if (!made) return edit::Result::fail("No scene cuts found in this clip");
            return edit::Result{};
        });
        if (made) state_->message(tr("Cut the clip at %n scene change(s)", "", made), 5000);
    });
    watcher->setFuture(QtConcurrent::run([path, cancel, guard] {
        return detectSceneCuts(path, 0.5, [guard](double f) {
            QMetaObject::invokeMethod(qApp, [guard, f] {
                if (guard) guard->setValue(int(f * 1000));
            }, Qt::QueuedConnection);
        }, cancel.get());
    }));
}

void MainWindow::normalizeLoudness() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    std::vector<Id> audio;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) audio.push_back(id);
    if (audio.empty()) {
        state_->message(tr("Select audio clips to normalize"));
        return;
    }
    const QStringList targets = {tr("-14 LUFS (streaming: YouTube, Spotify)"), tr("-16 LUFS (podcasts, Apple)"),
                                 tr("-23 LUFS (broadcast, EBU R128)"), tr("-24 LUFS (broadcast, ATSC A/85)")};
    const double values[] = {-14, -16, -23, -24};
    bool ok = false;
    QString choice = QInputDialog::getItem(this, tr("Normalize Loudness"), tr("Target loudness:"), targets, 0, false, &ok);
    if (!ok) return;
    double target = values[std::max<qsizetype>(0, targets.indexOf(choice))];
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<std::pair<Id, double>> gains;  // clip -> dB change
    for (Id id : audio) {
        const Clip* c = edit::clipById(*s, id);
        const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
        if (!m || !m->hasAudio || m->path.empty()) continue;
        AudioBufferPtr buf = MediaPool::instance().audio(m->path, s->sampleRate);
        if (!buf) continue;
        double perFrame = double(s->sampleRate) / s->fpsValue();
        int64_t first = int64_t(std::llround(c->sourceIn * perFrame));
        int64_t count = int64_t(std::llround(c->sourceExtent() * perFrame));
        LoudnessResult r = measureLoudness(*buf, first, count);
        if (r.valid) gains.push_back({id, target - r.integrated});
    }
    QApplication::restoreOverrideCursor();
    if (gains.empty()) {
        state_->message(tr("Nothing to normalize: the selected clips are silent"));
        return;
    }
    state_->edit(tr("Normalize Loudness"), [gains](Project&, Sequence& sq) {
        for (const auto& [id, delta] : gains) {
            Clip* c = edit::clipById(sq, id);
            if (!c) continue;
            Param& g = c->audio.params["gain_db"];
            g.value = std::clamp(g.value + delta, -60.0, 24.0);
            for (auto& k : g.keys) k.v = std::clamp(k.v + delta, -60.0, 24.0);
        }
        return true;
    });
    state_->message(tr("Normalized %n clip(s) to %1 LUFS", "", int(gains.size())).arg(target), 5000);
}

void MainWindow::autoColor() {
    const Sequence* s = state_->sequence();
    const Clip* c = state_->primaryClip();
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!s || !m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) {
        state_->message(tr("Select a video clip to colour-balance"));
        return;
    }
    // Analyse the clip's own frame under the playhead (or its first frame).
    FrameTime t = c->contains(state_->playhead()) ? state_->playhead() : c->start;
    double sec = m->kind == MediaKind::Video ? std::max(0.0, c->sourceFrameAt(t) / s->fpsValue()) : 0.0;
    Image frame = renderMediaFrame(state_->project(), *m, sec, 320, 180);
    Id clipId = c->id;
    state_->edit(tr("Auto Colour"), [clipId, &frame](Project& p, Sequence& sq) {
        Clip* cc = edit::clipById(sq, clipId);
        if (!cc) return false;
        Effect e = autoColorCorrection(frame, p.newId());
        // Replace an earlier auto correction instead of stacking another.
        for (auto& existing : cc->effects)
            if (existing.type == "color_correct" && existing.strings.count("auto")) {
                e.id = existing.id;
                e.strings["auto"] = "1";
                existing = e;
                return true;
            }
        e.strings["auto"] = "1";
        cc->effects.insert(cc->effects.begin(), e);
        return true;
    });
    inspectorDock_->raise();
}

void MainWindow::scriptCutDialog() {
    ScriptCutDialog dlg(state_, bin_ ? bin_->selectedMedia() : std::vector<Id>{}, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ScriptCutResult r = ScriptCutDialog::build(state_, dlg.script(), dlg.options(), dlg.sequenceName());
    if (!r.sequence) {
        state_->message(tr("None of the script's lines were found in the transcribed takes"), 6000);
        return;
    }
    state_->message(tr("Built %1: %2 line(s) placed, %3 alternate(s), %4 not found")
                        .arg(dlg.sequenceName())
                        .arg(r.placed)
                        .arg(r.alternates)
                        .arg(r.missing),
                    8000);
}

void MainWindow::setColourReference() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to take a colour reference from"));
        return;
    }
    const FrameTime t = std::clamp<FrameTime>(state_->playhead(), 0, s->duration() - 1);
    colourRef_ = colourReferenceFrame(state_->project(), *s, t);
    {
        RenderOptions o;
        o.scale = std::min(1.0, 1280.0 / std::max(1, s->width));
        o.displaySpace = "rec709";
        const Image view = renderProgramFrame(state_->project(), *s, t, o);
        colourRefView_ = QImage(view.width, view.height, QImage::Format_RGBA8888);
        toRgba8(view, colourRefView_.bits(), size_t(colourRefView_.bytesPerLine()));
    }
    // Named after the top clip with a picture at the playhead.
    colourRefName_ = QString::fromStdString(s->name);
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i)
        if (const Clip* c = edit::clipAt(*s, TrackRef{TrackKind::Video, i}, t); c && !s->videoTracks[size_t(i)].muted) {
            colourRefName_ = QString::fromStdString(c->name);
            break;
        }
    state_->message(tr("Colour reference: %1 at %2. Select clips and choose Match Colour to Reference.")
                        .arg(colourRefName_, QString::fromStdString(formatTimecode(t, s->fps))),
                    6000);
    if (compareRef_ && compareRef_->isChecked()) setCompareWithReference(true);
}

void MainWindow::setCompareWithReference(bool on) {
    if (on && colourRefView_.isNull()) {
        state_->message(tr("Park on the look to compare with and choose Clip › Set Colour Reference first"), 5000);
        on = false;
    }
    if (compareRef_) {
        QSignalBlocker b(compareRef_);
        compareRef_->setChecked(on);
    }
    if (!programPanel_) return;
    if (on) programPanel_->viewer()->setCompare(colourRefView_, tr("Reference: %1").arg(colourRefName_));
    else programPanel_->viewer()->clearCompare();
}

int MainWindow::matchColour() {
    if (!state_->sequence()) return 0;
    if (colourRef_.empty()) {
        state_->message(tr("Park on the look to match and choose Clip › Set Colour Reference first"), 5000);
        return 0;
    }
    const std::vector<Id> clips(state_->selectedClips().begin(), state_->selectedClips().end());
    const Image ref = colourRef_;
    const FrameTime at = state_->playhead();
    int n = 0;
    state_->edit(tr("Match Colour"), [&](Project& p, Sequence& sq) {
        n = matchClipColour(p, sq, clips, ref, at);
        return n > 0;
    });
    if (n == 0) {
        state_->message(tr("Select the video clips to match"));
        return 0;
    }
    state_->message(tr("Matched %n clip(s) to %1", "", n).arg(colourRefName_), 5000);
    inspectorDock_->raise();
    return n;
}

namespace {

// Runs `work` off the UI thread behind a progress dialog with Cancel; false if
// it fails (the error shown in the status bar) or is cancelled.
bool runWithProgress(QWidget* parent, EditorState* state, const QString& title,
                     const std::function<bool(const std::function<void(double)>&, const std::atomic<bool>*, std::string*)>& work) {
    QProgressDialog progress(title, QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(&progress, &QProgressDialog::canceled, parent, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<bool, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([work, done, cancel] {
        std::string err;
        const bool ok = work([done](double f) { *done = f; }, cancel.get(), &err);
        return Out{ok, err};
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    progress.disconnect(parent);
    progress.close();
    const Out r = watcher.result();
    if (!r.first && !*cancel && !r.second.empty()) state->message(QString::fromStdString(r.second), 6000);
    return r.first && !*cancel;
}

}  // namespace

bool MainWindow::rippleTrimToPlayhead(bool previous) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    const FrameTime t = state_->playhead();
    const FrameTime edit = previous ? edit::previousClipEdge(*s, t) : edit::nextClipEdge(*s, t);
    const bool ok = state_->apply(previous ? tr("Ripple Trim Previous Edit") : tr("Ripple Trim Next Edit"),
                                  [t, previous](Project& p, Sequence& sq) { return edit::rippleTrimToPlayhead(p, sq, t, previous); });
    // The playhead stays on the join (it moves back to where the cut now is).
    if (ok && previous) state_->setPlayhead(edit);
    return ok;
}

unsigned MainWindow::askAttributes(const QString& title, bool removing) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    auto* lay = new QVBoxLayout(&dlg);
    const std::pair<const char*, QString> items[] = {{"attrMotion", tr("Motion (position, scale, rotation, crop)")},
                                                     {"attrOpacity", tr("Opacity and blend mode")},
                                                     {"attrTimeRemap", tr("Time Remapping")},
                                                     {"attrVolume", tr("Volume and pan")},
                                                     {"attrEffects", removing ? tr("Effects") : tr("Effects (added after the clip's own)")}};
    std::vector<QCheckBox*> boxes;
    QSettings settings;
    for (const auto& [name, label] : items) {
        auto* b = new QCheckBox(label, &dlg);
        b->setObjectName(QString::fromLatin1(name));
        b->setChecked(settings.value(QStringLiteral("attributes/") + QString::fromLatin1(name), true).toBool());
        lay->addWidget(b);
        boxes.push_back(b);
    }
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(box);
    if (dlg.exec() != QDialog::Accepted) return 0;
    unsigned what = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        settings.setValue(QStringLiteral("attributes/") + boxes[i]->objectName(), boxes[i]->isChecked());
        if (boxes[i]->isChecked()) what |= 1u << i;
    }
    return what;
}

bool MainWindow::pasteAttributes(unsigned what) {
    if (!state_->sequence() || !what) return false;
    if (clipboard_.empty()) {
        state_->message(tr("Copy a clip first, then select clips and paste its attributes"));
        return false;
    }
    std::vector<Id> to(state_->selectedClips().begin(), state_->selectedClips().end());
    if (to.empty()) {
        state_->message(tr("Select the clips to paste onto"));
        return false;
    }
    // The first copied picture clip gives the picture attributes, the first sound clip the volume.
    const edit::ClipboardItem* video = nullptr;
    const edit::ClipboardItem* audio = nullptr;
    for (const auto& item : clipboard_) {
        if (!video && item.track.kind == TrackKind::Video) video = &item;
        if (!audio && item.track.kind == TrackKind::Audio) audio = &item;
    }
    return state_->apply(tr("Paste Attributes"), [&](Project& p, Sequence& s) {
        edit::Result r = edit::Result::fail("Nothing to paste onto");
        if (video) {
            edit::Result rv = edit::pasteAttributes(p, s, video->clip, TrackKind::Video, to, what & ~unsigned(edit::AttrVolume));
            if (rv.ok) r = rv;
        }
        if (audio) {
            const unsigned audioWhat = what & unsigned(edit::AttrVolume | edit::AttrEffects | edit::AttrTimeRemap);
            edit::Result ra = edit::pasteAttributes(p, s, audio->clip, TrackKind::Audio, to, video ? audioWhat & ~unsigned(edit::AttrTimeRemap) : audioWhat);
            if (ra.ok) r = ra;
        }
        return r;
    });
}

bool MainWindow::removeAttributes(unsigned what) {
    if (!state_->sequence() || !what) return false;
    std::vector<Id> ids(state_->selectedClips().begin(), state_->selectedClips().end());
    if (ids.empty()) {
        state_->message(tr("Select the clips to change"));
        return false;
    }
    return state_->apply(tr("Remove Attributes"), [&](Project& p, Sequence& s) { return edit::removeAttributes(p, s, ids, what); });
}

const Clip* MainWindow::clipForCommand() const {
    const Sequence* s = state_->sequence();
    if (!s) return nullptr;
    const FrameTime t = state_->playhead();
    for (Id id : state_->selectedClips()) {
        auto loc = edit::locate(*s, id);
        const Clip* c = edit::clipById(*s, id);
        if (loc && loc->track.kind == TrackKind::Video && c && c->contains(t)) return c;
    }
    const int target = state_->targetVideoTrack();
    if (target >= 0 && target < int(s->videoTracks.size()))
        if (const Clip* c = edit::clipAt(*s, {TrackKind::Video, target}, t)) return c;
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i)
        if (const Clip* c = edit::clipAt(*s, {TrackKind::Video, i}, t)) return c;
    return nullptr;
}

const Clip* MainWindow::musicClip() const {
    const Sequence* s = state_->sequence();
    if (!s) return nullptr;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) return edit::clipById(*s, id);
    const FrameTime t = state_->playhead();
    for (int i = int(s->audioTracks.size()) - 1; i >= 0; --i)
        if (const Clip* c = edit::clipAt(*s, {TrackKind::Audio, i}, t)) return c;
    return nullptr;
}

bool MainWindow::setVoiceReference() {
    const Clip* c = musicClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) {
        state_->message(tr("Select the audio clip whose voice is right"));
        return false;
    }
    const Id clip = c->id, seqId = s->id;
    const QString name = QString::fromStdString(c->name);
    auto project = std::make_shared<const Project>(state_->project());
    std::vector<double> spectrum;
    if (!runWithProgress(this, state_, tr("Listening to the voice..."), [&, project](const auto&, const auto*, std::string* err) {
            const Sequence* sq = project->findSequence(seqId);
            const Clip* k = sq ? edit::clipById(*sq, clip) : nullptr;
            return k && clipSpeechSpectrum(*project, *sq, *k, spectrum, err);
        }))
        return false;
    voiceRef_ = spectrum;
    voiceRefName_ = name;
    state_->message(tr("Voice reference: %1. Select dialogue clips and choose Match Voice to Reference.").arg(name), 6000);
    return true;
}

int MainWindow::matchVoice() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    if (voiceRef_.empty()) {
        state_->message(tr("Select the clip whose voice is right and choose Clip › Set Voice Reference first"), 5000);
        return 0;
    }
    std::vector<Id> clips;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) clips.push_back(id);
    if (clips.empty()) {
        state_->message(tr("Select the audio clips to match"));
        return 0;
    }
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const std::vector<double> reference = voiceRef_;
    std::map<Id, VoiceEq> fits;
    if (!runWithProgress(this, state_, tr("Matching voices..."), [&, project](const auto& progress, const auto* cancel, std::string* err) {
            const Sequence* sq = project->findSequence(seqId);
            if (!sq) return false;
            for (size_t i = 0; i < clips.size(); ++i) {
                if (cancel && cancel->load()) return false;
                progress(double(i) / double(clips.size()));
                std::vector<double> spectrum;
                if (const Clip* k = edit::clipById(*sq, clips[i]); k && clipSpeechSpectrum(*project, *sq, *k, spectrum, err))
                    fits[clips[i]] = fitVoiceEq(spectrum, reference);
            }
            return !fits.empty();
        }))
        return 0;
    state_->edit(tr("Match Voice"), [&](Project& p, Sequence& sq) {
        for (const auto& [id, eq] : fits)
            if (Clip* k = edit::clipById(sq, id)) applyVoiceEq(p, *k, eq);
        return true;
    });
    state_->message(tr("Matched %n clip(s) to %1", "", int(fits.size())).arg(voiceRefName_), 5000);
    inspectorDock_->raise();
    return int(fits.size());
}

int MainWindow::addBeatMarkers(bool everyBeat) {
    const Clip* c = musicClip();
    if (!c) {
        state_->message(tr("Select a music clip"));
        return 0;
    }
    const Id clip = c->id, media = c->mediaId;
    auto project = std::make_shared<const Project>(state_->project());
    BeatGrid grid;
    if (!runWithProgress(this, state_, tr("Finding the beat..."), [&, project](const auto&, const auto* cancel, std::string* err) {
            return mediaBeats(*project, media, grid, cancel, err);
        }))
        return 0;
    int n = 0;
    state_->edit(everyBeat ? tr("Add Beat Markers") : tr("Add Bar Markers"), [&](Project&, Sequence& s) {
        const Clip* k = edit::clipById(s, clip);
        if (!k) return false;
        n = montage::addBeatMarkers(s, *k, grid, everyBeat);
        return n > 0;
    });
    state_->message(tr("%n marker(s) at %1 BPM", "", n).arg(grid.tempo, 0, 'f', 1), 5000);
    return n;
}

int MainWindow::cutMediaToBeat(int every, bool bars) {
    const Clip* c = musicClip();
    const std::vector<Id> media = bin_ ? bin_->selectedMedia() : std::vector<Id>{};
    if (!c || media.empty()) {
        state_->message(tr("Select a music clip on the timeline and the clips to cut to it in the bin"));
        return 0;
    }
    const Id clip = c->id, music = c->mediaId;
    auto project = std::make_shared<const Project>(state_->project());
    BeatGrid grid;
    if (!runWithProgress(this, state_, tr("Finding the beat..."), [&, project](const auto&, const auto* cancel, std::string* err) {
            return mediaBeats(*project, music, grid, cancel, err);
        }))
        return 0;
    int n = 0;
    std::string error;
    state_->apply(tr("Cut to the Beat"), [&](Project& p, Sequence& s) {
        const Clip* k = edit::clipById(s, clip);
        if (!k) return edit::Result::fail("The music clip is gone");
        const Clip copy = *k;
        edit::Result r = cutToBeat(p, s, copy, grid, media, every, bars, std::max(0, state_->targetVideoTrack()));
        n = int(r.created.size());
        error = r.error;
        return r;
    });
    if (n) state_->message(tr("%n clip(s) cut to the music at %1 BPM", "", n).arg(grid.tempo, 0, 'f', 1), 5000);
    else if (!error.empty()) state_->message(QString::fromStdString(error), 6000);
    return n;
}

bool MainWindow::fitMusicToLength(FrameTime target) {
    const Clip* c = musicClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) {
        state_->message(tr("Select a music clip"));
        return false;
    }
    const Id clip = c->id, seqId = s->id;
    auto project = std::make_shared<const Project>(state_->project());
    MusicFit fit;
    if (!runWithProgress(this, state_, tr("Listening to the music..."), [&, project](const auto&, const auto* cancel, std::string* err) {
            const Sequence* sq = project->findSequence(seqId);
            const Clip* k = sq ? edit::clipById(*sq, clip) : nullptr;
            return k && analyzeMusicFit(*project, *sq, *k, target, fit, cancel, err);
        }))
        return false;
    QString error;
    const bool ok = state_->apply(tr("Fit Music to Length"), [&](Project& p, Sequence& sq) {
        edit::Result r = applyMusicFit(p, sq, clip, fit);
        if (!r.ok) error = QString::fromStdString(r.error);
        return r;
    });
    if (!ok) {
        if (!error.isEmpty()) state_->message(error, 5000);
        return false;
    }
    state_->message(tr("The music now lasts %1 s (%n join(s))", "", int(fit.segments.size()) - 1).arg(fit.duration, 0, 'f', 1), 6000);
    return true;
}

void MainWindow::fitMusicDialog() {
    const Sequence* s = state_->sequence();
    const Clip* c = musicClip();
    if (!s || !c) {
        state_->message(tr("Select a music clip"));
        return;
    }
    const double fps = s->fpsValue();
    // Suggested lengths: In to Out, the picture, and the clip's own.
    FrameTime picture = 0;
    for (const Track& t : s->videoTracks)
        for (const Clip& k : t.clips) picture = std::max(picture, k.end());
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Fit Music to Length"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(tr("Shortens or lengthens \"%1\" by whole bars where the music repeats itself, keeping its "
                                "start and its ending. Joins are crossfaded on the beat.").arg(QString::fromStdString(c->name)), &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* form = new QFormLayout;
    auto* length = new QDoubleSpinBox(&dlg);
    length->setObjectName(QStringLiteral("fitMusicLength"));
    length->setRange(5, 3600);
    length->setDecimals(2);
    length->setSuffix(tr(" s"));
    auto* preset = new QComboBox(&dlg);
    if (s->inPoint >= 0 && s->outPoint > s->inPoint) preset->addItem(tr("In to Out"), double(s->outPoint - s->inPoint + 1) / fps);
    if (picture > 0) preset->addItem(tr("The picture"), double(picture - c->start) / fps);
    for (int secs : {15, 30, 60}) preset->addItem(tr("%1 s").arg(secs), double(secs));
    connect(preset, &QComboBox::currentIndexChanged, &dlg, [preset, length](int i) { length->setValue(preset->itemData(i).toDouble()); });
    length->setValue(preset->count() ? preset->itemData(0).toDouble() : double(c->duration) / fps);
    form->addRow(tr("Length:"), length);
    form->addRow(tr("Use:"), preset);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    fitMusicToLength(FrameTime(std::llround(length->value() * fps)));
}

bool MainWindow::addFrameHold() {
    const Clip* c = clipForCommand();
    if (!c) {
        state_->message(tr("Put the playhead over a video clip"));
        return false;
    }
    const Id id = c->id;
    const FrameTime t = state_->playhead();
    return state_->apply(tr("Add Frame Hold"), [id, t](Project& p, Sequence& s) { return edit::addFrameHold(p, s, id, t); });
}

bool MainWindow::replaceWithSource() {
    const Sequence* s = state_->sequence();
    const MediaItem* m = state_->project().findMedia(state_->sourceMedia());
    if (!s || !m) {
        state_->message(tr("Load a clip in the Source monitor first"));
        return false;
    }
    const Clip* target = nullptr;
    if (const Clip* sel = state_->primaryClip()) target = sel;
    if (!target) target = clipForCommand();
    if (!target) {
        state_->message(tr("Select the clip to replace"));
        return false;
    }
    // The source In goes to the clip's start; without one, the Source monitor's frame goes to the playhead.
    const bool byIn = state_->sourceIn() >= 0;
    const double align = byIn ? double(state_->sourceIn()) : double(source_->position());
    const FrameTime at = byIn || !target->contains(state_->playhead()) ? target->start : state_->playhead();
    const Id id = target->id, media = m->id;
    return state_->apply(tr("Replace with Source Clip"),
                         [=](Project& p, Sequence& sq) { return edit::replaceClip(p, sq, id, media, align, at); });
}

bool MainWindow::fitToFill() {
    const Sequence* s = state_->sequence();
    const MediaItem* m = state_->project().findMedia(state_->sourceMedia());
    if (!s || !m) {
        state_->message(tr("Load a clip in the Source monitor first"));
        return false;
    }
    if (state_->sourceIn() < 0 || state_->sourceOut() < 0 || s->inPoint < 0 || s->outPoint < 0) {
        state_->message(tr("Fit to Fill needs In and Out marked in both the Source monitor and the timeline"));
        return false;
    }
    const TrackRef vt{TrackKind::Video, std::min(state_->targetVideoTrack(), int(s->videoTracks.size()) - 1)};
    const TrackRef at{TrackKind::Audio, std::min(state_->targetAudioTrack(), int(s->audioTracks.size()) - 1)};
    const double in = double(state_->sourceIn()), out = double(state_->sourceOut());
    const FrameTime tin = s->inPoint, tout = s->outPoint;
    const Id media = m->id;
    return state_->apply(tr("Fit to Fill"), [=](Project& p, Sequence& sq) { return edit::fitToFill(p, sq, media, in, out, tin, tout, vt, at); });
}

int MainWindow::renderInToOut() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) return 0;
    const FrameTime from = s->inPoint >= 0 ? s->inPoint : 0;
    const FrameTime to = s->outPoint >= 0 ? std::min(s->outPoint, s->duration() - 1) : s->duration() - 1;
    if (to < from) return 0;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const RenderOptions o = program_->renderOptions();
    int rendered = 0;
    const bool ok = runWithProgress(this, state_, tr("Rendering previews..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string*) {
        const Sequence* sq = project->findSequence(seqId);
        if (!sq) return false;
        rendered = renderToCache(*project, *sq, from, to, o, RenderCache::instance(), progress, cancel);
        return rendered >= 0;
    });
    refreshRenderBar(true);
    if (!ok) return -1;
    state_->message(rendered ? tr("Rendered %n frame(s)", "", rendered) : tr("Everything there is already rendered"), 4000);
    return rendered;
}

void MainWindow::deleteRenderFiles() {
    RenderCache::instance().clear();
    timeline_->setRenderedRanges({});
    state_->message(tr("Render files deleted"), 3000);
}

void MainWindow::refreshRenderBar(bool wait) {
    const Sequence* s = state_->sequence();
    const int generation = ++renderBarGeneration_;
    if (!s || RenderCache::instance().count() == 0) {
        timeline_->setRenderedRanges({});
        return;
    }
    // Keys for every frame take a moment on long timelines: worked out off the UI thread.
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const RenderOptions o = program_->renderOptions();
    auto work = [project, seqId, o] {
        const Sequence* sq = project->findSequence(seqId);
        return sq ? cachedRanges(*project, *sq, 0, sq->duration(), o, RenderCache::instance())
                  : std::vector<std::pair<FrameTime, FrameTime>>{};
    };
    if (wait) {
        timeline_->setRenderedRanges(work());
        return;
    }
    auto* watcher = new QFutureWatcher<std::vector<std::pair<FrameTime, FrameTime>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        if (generation == renderBarGeneration_) timeline_->setRenderedRanges(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(work));
}

int MainWindow::selectForward(bool allTracks) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    std::optional<TrackRef> track;
    if (!allTracks) track = TrackRef{TrackKind::Video, std::min(state_->targetVideoTrack(), int(s->videoTracks.size()) - 1)};
    const auto ids = edit::clipsFrom(*s, state_->playhead(), track);
    state_->setSelection(ids);
    return int(ids.size());
}

Id MainWindow::autoReframeSequence(int aspectW, int aspectH, int speed) {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to reframe"));
        return 0;
    }
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::map<Id, std::vector<ReframeKey>> paths;
    const bool ok = runWithProgress(this, state_, tr("Finding the subject of each shot..."),
                                    [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
                                        const Sequence* sq = project->findSequence(seqId);
                                        return sq && analyzeSequenceReframe(*project, *sq, speed, paths, progress, cancel, err);
                                    });
    if (!ok) return 0;
    int w = 0, h = 0;
    reframeSize(*s, aspectW, aspectH, w, h);
    Id made = 0;
    state_->edit(tr("Auto Reframe Sequence"), [&](Project& p, Sequence&) {
        made = makeReframedSequence(p, seqId, w, h, paths);
        return made != 0;
    });
    if (!made) return 0;
    state_->setActiveSequence(made);
    state_->message(tr("Reframed %n clip(s) into %1 x %2", "", int(paths.size())).arg(w).arg(h), 5000);
    return made;
}

void MainWindow::autoReframeDialog() {
    if (!state_->sequence()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Auto Reframe Sequence"));
    auto* form = new QFormLayout(&dlg);
    auto* aspect = new QComboBox(&dlg);
    aspect->setObjectName(QStringLiteral("reframeAspect"));
    aspect->addItem(tr("Vertical 9:16"), QSize(9, 16));
    aspect->addItem(tr("Square 1:1"), QSize(1, 1));
    aspect->addItem(tr("Vertical 4:5"), QSize(4, 5));
    aspect->addItem(tr("Horizontal 16:9"), QSize(16, 9));
    auto* motion = new QComboBox(&dlg);
    motion->setObjectName(QStringLiteral("reframeMotion"));
    motion->addItems({tr("Slower Motion"), tr("Default"), tr("Faster Motion")});
    motion->setCurrentIndex(1);
    motion->setToolTip(tr("How closely the frame follows the subject: slower for interviews, faster for sport"));
    form->addRow(tr("Aspect ratio:"), aspect);
    form->addRow(tr("Motion:"), motion);
    auto* note = new QLabel(tr("A copy of the sequence is made at the new shape; each clip follows its subject."), &dlg);
    note->setWordWrap(true);
    form->addRow(note);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(box);
    if (dlg.exec() != QDialog::Accepted) return;
    const QSize a = aspect->currentData().toSize();
    autoReframeSequence(a.width(), a.height(), motion->currentIndex());
}

int MainWindow::autoReframeClips(int speed) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    std::vector<Id> ids(state_->selectedClips().begin(), state_->selectedClips().end());
    if (ids.empty()) {
        state_->message(tr("Select the clips to reframe"));
        return 0;
    }
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::map<Id, std::vector<ReframeKey>> paths;
    const bool ok = runWithProgress(this, state_, tr("Finding the subject..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        if (!sq) return false;
        for (size_t i = 0; i < ids.size(); ++i) {
            const Clip* c = edit::clipById(*sq, ids[i]);
            if (!c || (cancel && cancel->load())) continue;
            std::vector<ReframeKey> path;
            const auto part = [&](double f) { progress((double(i) + f) / double(ids.size())); };
            if (analyzeClipReframe(*project, *sq, *c, speed, path, part, cancel, err)) paths[ids[i]] = std::move(path);
        }
        return !paths.empty();
    });
    if (!ok) return 0;
    state_->edit(tr("Auto Reframe"), [&](Project& p, Sequence& sq) {
        for (const auto& [id, path] : paths)
            if (Clip* c = edit::clipById(sq, id)) applyReframe(p, sq, *c, path);
        return true;
    });
    state_->message(tr("Reframed %n clip(s)", "", int(paths.size())), 4000);
    return int(paths.size());
}

void MainWindow::syncByAudio() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // Audio clips in the selection, earliest (then lowest track) first: that one is the reference.
    struct Item {
        Id id;
        FrameTime start;
        int track;
    };
    std::vector<Item> items;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) {
            const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
            const MediaItem* m = state_->project().findMedia(c.mediaId);
            if (m && m->hasAudio && !m->path.empty() && c.speed == 1.0 && !c.reverse) items.push_back({id, c.start, loc->track.index});
        }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return std::tie(a.start, a.track) < std::tie(b.start, b.track); });
    if (items.size() < 2) {
        state_->message(tr("Select two or more clips with audio (at 100% speed) to synchronize"));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const Clip* ref = edit::clipById(*s, items[0].id);
    AudioBufferPtr refBuf = MediaPool::instance().audio(state_->project().findMedia(ref->mediaId)->path, s->sampleRate);
    std::vector<std::pair<std::vector<Id>, FrameTime>> moves;
    QStringList failed;
    Id refGroup = ref->linkGroup;
    for (size_t i = 1; i < items.size(); ++i) {
        const Clip* o = edit::clipById(*s, items[i].id);
        if (refGroup && o->linkGroup == refGroup) continue;
        const MediaItem* om = state_->project().findMedia(o->mediaId);
        AudioBufferPtr ob = MediaPool::instance().audio(om->path, s->sampleRate);
        SyncResult r = refBuf && ob ? findAudioOffset(*refBuf, *ob) : SyncResult{};
        if (!r.found) {
            failed << QString::fromStdString(o->name);
            continue;
        }
        FrameTime newStart = FrameTime(std::llround(o->sourceIn - ref->sourceIn + double(ref->start) + r.offset * s->fpsValue()));
        moves.push_back({edit::linkedClips(*s, o->id), newStart - o->start});
    }
    QApplication::restoreOverrideCursor();
    if (!moves.empty())
        state_->edit(tr("Synchronize by Audio"), [moves](Project& p, Sequence& sq) {
            for (const auto& [ids, delta] : moves) edit::moveClips(p, sq, ids, delta, 0, 0);
            return true;
        });
    if (!failed.isEmpty()) QMessageBox::information(this, tr("Synchronize"), tr("No reliable audio match for: %1").arg(failed.join(", ")));
    else state_->message(tr("Synchronized %n clip(s) to %1", "", int(moves.size())).arg(QString::fromStdString(ref->name)), 5000);
}

void MainWindow::exportInterchange(Interchange format) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    QSettings st = appSettings();
    struct Kind {
        const char* ext;
        QString title, filter;
    };
    const Kind kinds[] = {{"edl", tr("Export EDL"), tr("CMX 3600 EDL (*.edl)")},
                          {"otio", tr("Export OpenTimelineIO"), tr("OpenTimelineIO (*.otio)")},
                          {"xml", tr("Export Final Cut Pro 7 XML"), tr("Final Cut Pro 7 XML (*.xml)")},
                          {"fcpxml", tr("Export FCPXML"), tr("FCPXML (*.fcpxml)")}};
    const Kind& k = kinds[int(format)];
    QString path = QFileDialog::getSaveFileName(this, k.title,
                                                st.value("lastExportDir").toString() + "/" + QString::fromStdString(s->name) + "." + k.ext,
                                                k.filter);
    if (path.isEmpty()) return;
    const Project& pr = state_->project();
    const std::string text = format == Interchange::Edl ? exportEdl(pr, *s)
                             : format == Interchange::Otio ? exportOtio(pr, *s)
                             : format == Interchange::Fcp7Xml ? exportFcp7Xml(pr, *s)
                                                              : exportFcpXml(pr, *s);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        QMessageBox::warning(this, tr("Export"), tr("Cannot write %1").arg(path));
        return;
    }
    st.setValue("lastExportDir", QFileInfo(path).absolutePath());
    statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
}

void MainWindow::findSimilarShots() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // The selected video clip, else the top one under the playhead; its frame at the playhead (or its middle).
    const Clip* c = state_->primaryClip();
    if (c && !(edit::locate(*s, c->id) && edit::locate(*s, c->id)->track.kind == TrackKind::Video)) c = nullptr;
    for (int i = int(s->videoTracks.size()) - 1; !c && i >= 0; --i)
        if (const Clip* under = edit::clipAt(*s, TrackRef{TrackKind::Video, i}, s->playhead); under && under->mediaId) c = under;
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video) {
        statusBar()->showMessage(tr("Select a video clip, or put the playhead over one"), 5000);
        return;
    }
    const FrameTime at = c->contains(s->playhead) ? s->playhead : c->start + c->duration / 2;
    const double seconds = c->sourceFrameAt(at) / s->fpsValue();
    shotsDock_->show();
    shotsDock_->raise();
    const int n = shots_->searchSimilar(m->id, seconds);
    statusBar()->showMessage(tr("%n moment(s) like this one", "", n), 5000);
}

bool MainWindow::exportAafTo(const QString& path, QString* summary) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    // In the background on a copy of the project: decoding and writing the WAVs takes a while.
    auto snap = std::make_shared<Project>(state_->project());
    const Id seqId = s->id;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    QProgressDialog dlg(tr("Exporting AAF and its audio files..."), tr("Cancel"), 0, 1000, this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(300);
    connect(&dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    struct Outcome {
        bool ok = false;
        AafExportResult result;
        std::string error;
    };
    QFutureWatcher<Outcome> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<Outcome>::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    connect(&poll, &QTimer::timeout, this, [&] { dlg.setValue(int(progress->load() * 1000)); });
    poll.start(100);
    watcher.setFuture(QtConcurrent::run([snap, seqId, path, cancel, progress]() {
        Outcome o;
        const Sequence* sq = snap->findSequence(seqId);
        if (!sq) return o;
        o.ok = exportAaf(*snap, *sq, path.toStdString(), &o.result, [progress](double f, FrameTime) { *progress = f; }, cancel.get(),
                         &o.error);
        return o;
    }));
    loop.exec();
    poll.stop();
    dlg.close();
    const Outcome o = watcher.result();
    if (!o.ok) {
        if (!*cancel) QMessageBox::warning(this, tr("Export AAF"), QString::fromStdString(o.error));
        return false;
    }
    QString text = tr("%1: %2 audio tracks, %3 clips, %4 crossfades; %5 WAV files in \"%6\"")
                       .arg(QFileInfo(path).fileName())
                       .arg(o.result.audioTracks)
                       .arg(o.result.clips)
                       .arg(o.result.transitions)
                       .arg(o.result.mediaFiles.size())
                       .arg(QFileInfo(path).completeBaseName() + tr(" Media"));
    for (const std::string& w : o.result.warnings) text += "\n" + QString::fromStdString(w);
    if (summary) *summary = text;
    return true;
}

void MainWindow::exportAafDialog() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    QSettings st = appSettings();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export AAF for Audio Post"), st.value("lastExportDir").toString() + "/" + QString::fromStdString(s->name) + ".aaf",
        tr("AAF (*.aaf)"));
    if (path.isEmpty()) return;
    st.setValue("lastExportDir", QFileInfo(path).absolutePath());
    QString summary;
    if (exportAafTo(path, &summary)) statusBar()->showMessage(tr("Exported %1").arg(summary), 10000);
}

void MainWindow::importTimeline() {
    QSettings st = appSettings();
    QString path = QFileDialog::getOpenFileName(this, tr("Import Timeline"), st.value("lastImportTimelineDir").toString(),
                                                tr("Timelines (*.xml *.fcpxml *.otio *.edl);;Final Cut Pro 7 XML (*.xml);;"
                                                   "FCPXML (*.fcpxml);;OpenTimelineIO (*.otio);;CMX 3600 EDL (*.edl)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).isDir()) path += "/Info.fcpxml";  // an .fcpxmld bundle
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Timeline"), tr("Cannot read %1").arg(path));
        return;
    }
    st.setValue("lastImportTimelineDir", QFileInfo(path).absolutePath());
    const std::string text = f.readAll().toStdString();
    const MediaProber prober = [](const std::string& file, MediaItem& m) { return probeMedia(file, m, nullptr); };
    const QString ext = QFileInfo(path).suffix().toLower();
    // EDLs do not say their rate: take the open sequence's.
    const Rational fps = state_->sequence() ? state_->sequence()->fps : Rational{30, 1};
    const std::string dir = QFileInfo(path).absolutePath().toStdString();
    ImportResult r;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    state_->edit(tr("Import Timeline"), [&](Project& p, Sequence&) {
        r = ext == "edl"                      ? importEdl(p, text, fps, prober, dir)
            : ext == "xml" || ext == "fcpxml" ? importXmlTimeline(p, text, prober)
                                              : importOtio(p, text, prober);
        return r.ok;
    });
    QApplication::restoreOverrideCursor();
    if (!r.ok) {
        QMessageBox::warning(this, tr("Import Timeline"), QString::fromStdString(r.error));
        return;
    }
    state_->setActiveSequence(r.sequence);
    QStringList notes;
    if (!r.offline.empty()) notes << tr("%n file(s) not found; their clips are offline:", "", int(r.offline.size()));
    for (size_t i = 0; i < r.offline.size() && i < 10; ++i) notes << QStringLiteral("  ") + QString::fromStdString(r.offline[i]);
    for (const auto& w : r.warnings) notes << QString::fromStdString(w);
    if (!notes.isEmpty()) QMessageBox::information(this, tr("Import Timeline"), notes.join('\n'));
    statusBar()->showMessage(tr("Imported %n clip(s) from %1", "", r.clips).arg(QFileInfo(path).fileName()), 5000);
}

void MainWindow::addTitle() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    FrameTime at = s->playhead, len = FrameTime(std::llround(5 * s->fpsValue()));
    // Put titles on the first track above the target that is free at the playhead.
    int vt = std::min(state_->targetVideoTrack() + 1, int(s->videoTracks.size()) - 1);
    for (int i = vt; i < int(s->videoTracks.size()); ++i)
        if (edit::trackEmpty(s->videoTracks[size_t(i)], at, at + len)) {
            vt = i;
            break;
        }
    std::vector<Id> created;
    state_->apply(tr("New Title"), [&](Project& p, Sequence& sq) {
        Clip c = makeGeneratorClip(p, "title", len);
        c.start = at;
        auto r = edit::overwrite(p, sq, {TrackKind::Video, vt}, c);
        created = r.created;
        return r;
    });
    if (!created.empty()) {
        state_->setSelection(created, false);
        inspectorDock_->raise();
    }
}

void MainWindow::newSequence() {
    auto spec = SequenceSettingsDialog::askNew(this, tr("Sequence %1").arg(state_->project().sequences.size() + 1));
    if (!spec) return;
    Id id = state_->newSequence(spec->name, spec->width, spec->height, spec->fps);
    if (id)
        state_->edit(tr("Sequence Settings"), [spec = *spec](Project&, Sequence& s) {
            s.sampleRate = spec.sampleRate;
            s.colorSpace = spec.colorSpace;
            s.hdrPeakNits = spec.hdrPeakNits;
            s.audioLayout = spec.audioLayout;
            return true;
        });
}

void MainWindow::offerRecovery(const std::vector<RecoveryManager::Session>& crashed) {
    const bool pluginsUsed = std::any_of(crashed.begin(), crashed.end(), [](const auto& s) { return s.pluginsUsed; });
    auto unsaved = std::find_if(crashed.begin(), crashed.end(), [](const auto& s) { return !s.recoveryFile.isEmpty(); });
    bool safeMode = false;
    if (unsaved != crashed.end()) {
        QMessageBox box(QMessageBox::Warning, tr("Recover Unsaved Work"),
                        tr("Montage closed unexpectedly. Unsaved changes to “%1” from %2 can be recovered.")
                            .arg(unsaved->projectName.isEmpty() ? tr("Untitled") : unsaved->projectName,
                                 QLocale().toString(unsaved->lastSave.toLocalTime(), QLocale::ShortFormat)),
                        QMessageBox::NoButton, this);
        QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
        box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.setDefaultButton(recover);
        auto* safe = new QCheckBox(tr("Start with audio plugins disabled (safe mode)"), &box);
        safe->setChecked(pluginsUsed);
        safe->setVisible(pluginsUsed);
        box.setCheckBox(safe);
        box.exec();
        safeMode = pluginsUsed && safe->isChecked();
        if (box.clickedButton() == recover) {
            QString err;
            if (recovery_->recover(*unsaved, &err)) {
                timeline_->zoomToFit();
                statusBar()->showMessage(tr("Recovered unsaved changes; save to keep them"), 8000);
            } else {
                QMessageBox::warning(this, tr("Recover Unsaved Work"), err);
            }
        } else {
            recovery_->discard(*unsaved);
        }
    } else if (pluginsUsed) {
        safeMode = QMessageBox::question(this, tr("Safe Mode"),
                                         tr("Montage closed unexpectedly while audio plugins were in use. Start with "
                                            "audio plugins disabled for this session (safe mode)?")) == QMessageBox::Yes;
    }
    // Sessions without unsaved work have been reported; forget them.
    for (const auto& s : crashed)
        if (s.recoveryFile.isEmpty()) recovery_->discard(s);
    if (safeMode) {
        plugins::Registry::instance().setEnabled(false);
        effects_->reload();
        statusBar()->showMessage(tr("Safe mode: audio plugins are disabled until Montage is restarted"), 10000);
    } else {
        scanPluginsInBackground();
    }
}

void MainWindow::openSnapshot() {
    if (!maybeSave()) return;
    const QString name = state_->filePath().isEmpty() ? QString::fromStdString(state_->project().name)
                                                       : QFileInfo(state_->filePath()).completeBaseName();
    QString dir = recovery_->snapshotDir(name.isEmpty() ? tr("Untitled") : name);
    if (!QFileInfo::exists(dir)) dir = recovery_->baseDir() + "/snapshots";
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Auto-Save Snapshot"), dir, tr("Montage projects (*.montage)"));
    if (path.isEmpty()) return;
    QString err;
    // Opened as an untitled copy so saving never overwrites the snapshot.
    if (!state_->recover(path, QString(), &err)) {
        QMessageBox::warning(this, tr("Open Auto-Save Snapshot"), err);
        return;
    }
    timeline_->zoomToFit();
    statusBar()->showMessage(tr("Opened snapshot %1 as an untitled project").arg(QFileInfo(path).fileName()), 6000);
}

void MainWindow::scanPluginsInBackground() {
    // Like other hosts, look for new or changed plugins at every launch; unchanged
    // ones come from the cache, so this is quick after the first run.
    if (qEnvironmentVariableIsSet("MONTAGE_NO_PLUGIN_SCAN")) return;
    PluginManagerDialog::applySavedFolders();
    auto* watcher = new QFutureWatcher<plugins::ScanReport>(this);
    connect(watcher, &QFutureWatcher<plugins::ScanReport>::finished, this, [this, watcher] {
        watcher->deleteLater();
        effects_->reload();
        const int blocked = int(watcher->result().newlyBlocked.size());
        if (blocked > 0)
            state_->message(tr("%n audio plugin(s) failed to load and were blocked (Tools › Audio Plugins)", "", blocked), 8000);
    });
    watcher->setFuture(QtConcurrent::run([] { return plugins::Registry::instance().scan(); }));
}

void MainWindow::applyFromBrowser(const QString& typeQ, EffectCategory category) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    std::string type = typeQ.toStdString();
    const EffectInfo* info = findEffectInfo(type);
    if (!info && !plugins::isPluginType(type)) return;
    const QString name = QString::fromStdString(plugins::effectTypeName(type));
    if (category == EffectCategory::Generator) {
        FrameTime at = s->playhead, len = FrameTime(std::llround(5 * s->fpsValue()));
        int vt = state_->targetVideoTrack();
        std::vector<Id> created;
        state_->apply(tr("Add %1").arg(QString::fromStdString(info->displayName)), [&](Project& p, Sequence& sq) {
            Clip c = makeGeneratorClip(p, type, len);
            c.start = at;
            auto r = edit::overwrite(p, sq, {TrackKind::Video, std::min(vt, int(sq.videoTracks.size()) - 1)}, c);
            created = r.created;
            return r;
        });
        if (!created.empty()) state_->setSelection(created, false);
        return;
    }
    if (category == EffectCategory::VideoTransition || category == EffectCategory::AudioTransition) {
        bool audio = category == EffectCategory::AudioTransition;
        const Clip* c = state_->primaryClip();
        if (!c) {
            state_->message(tr("Select a clip to add a transition to"));
            return;
        }
        Id clipId = c->id;
        FrameTime t = state_->playhead();
        edit::Edge edge = std::llabs(t - c->start) <= std::llabs(c->end() - t) ? edit::Edge::In : edit::Edge::Out;
        // For audio transitions use the linked audio clip if a video clip is primary.
        if (audio)
            for (Id id : state_->selectedClips())
                if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) clipId = id;
        FrameTime dur = FrameTime(std::llround(s->fpsValue()));
        state_->apply(tr("Add Transition"), [=](Project& p, Sequence& sq) { return edit::addTransition(p, sq, clipId, edge, type, dur); });
        return;
    }
    bool video = category == EffectCategory::VideoFilter;
    std::vector<Id> targets;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && (loc->track.kind == TrackKind::Video) == video) targets.push_back(id);
    if (targets.empty()) {
        state_->message(video ? tr("Select a video clip to apply the effect to") : tr("Select an audio clip to apply the effect to"));
        return;
    }
    QString error;
    state_->edit(tr("Add %1").arg(name), [targets, type, &error](Project& p, Sequence& sq) {
        bool added = false;
        for (Id id : targets)
            if (Clip* c = edit::clipById(sq, id)) {
                std::string err;
                if (auto e = plugins::makeEffectOfType(p, type, &err)) {
                    c->effects.push_back(*e);
                    added = true;
                } else {
                    error = QString::fromStdString(err);
                }
            }
        return added;
    });
    if (!error.isEmpty()) state_->message(tr("Could not load %1: %2").arg(name, error));
    inspectorDock_->raise();
}

// ---------------------------------------------------------------------------
// Help

void MainWindow::showShortcuts() {
    keymap::Dialog dlg(this);
    dlg.exec();
}

void MainWindow::about() {
    QMessageBox::about(this, tr("About Montage"),
                       tr("<h3>Montage %1</h3><p>A professional non-linear video editor written in C++ with Qt and FFmpeg.</p>"
                          "<p>Multi-track editing with ripple, roll, slip and slide tools; keyframed effects and colour correction; "
                          "titles, transitions, scopes, an audio mixer and export to H.264, HEVC, ProRes, DNxHR, VP9 and AV1.</p>")
                           .arg(MONTAGE_VERSION));
}

void MainWindow::raisePanel(const QString& name) {
    for (QDockWidget* d : docks_)
        if (d->objectName() == name) {
            d->show();
            d->raise();
        }
}

void MainWindow::scheduleScreenshot(const QString& path, int delayMs) {
    QTimer::singleShot(delayMs, this, [this, path] {
        QApplication::processEvents();
        grab().save(path);
        QApplication::quit();
    });
}

}  // namespace montage
