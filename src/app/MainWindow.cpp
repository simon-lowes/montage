#include "MainWindow.h"

#include <QAction>
#include <QCheckBox>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
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
#include "media/HwAccel.h"
#include "media/MediaPool.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "ExportDialog.h"
#include "InspectorWidget.h"
#include "MediaBinWidget.h"
#include "MixerPanel.h"
#include "CaptionsPanel.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "PluginManagerDialog.h"
#include "ScopesWidget.h"
#include "SequenceSettingsDialog.h"
#include "Theme.h"
#include "core/Effects.h"
#include "core/Interchange.h"
#include "media/Analysis.h"
#include "media/AudioSync.h"
#include "media/Loudness.h"
#include "media/MediaPool.h"
#include "render/Compositor.h"
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
    connect(programPanel_, &MonitorPanel::exportFrameRequested, this, &MainWindow::exportFrame);
    bin_ = new MediaBinWidget(state_, this);
    connect(bin_, &MediaBinWidget::openInSource, this, &MainWindow::openInSource);
    connect(bin_, &MediaBinWidget::newTitleRequested, this, &MainWindow::addTitle);
    connect(bin_, &MediaBinWidget::newSequenceRequested, this, &MainWindow::newSequence);
    effects_ = new EffectsBrowser(this);
    connect(effects_, &EffectsBrowser::applyRequested, this, &MainWindow::applyFromBrowser);
    inspector_ = new InspectorWidget(state_, this);
    scopes_ = new ScopesWidget(this);
    mixer_ = new MixerPanel(state_, this);
    captions_ = new CaptionsPanel(state_, this);
    meter_ = new AudioMeterWidget(this);

    sourceDock_ = makeDock(tr("Source"), "source", sourcePanel_);
    programDock_ = makeDock(tr("Program"), "program", programPanel_);
    inspectorDock_ = makeDock(tr("Inspector"), "inspector", inspector_);
    binDock_ = makeDock(tr("Media"), "media", bin_);
    effectsDock_ = makeDock(tr("Effects"), "effects", effects_);
    scopesDock_ = makeDock(tr("Scopes"), "scopes", scopes_);
    mixerDock_ = makeDock(tr("Audio Mixer"), "mixer", mixer_);
    captionsDock_ = makeDock(tr("Captions"), "captions", captions_);
    connect(timeline_, &TimelineWidget::captionActivated, this, [this](Id track, int index) {
        captionsDock_->show();
        captionsDock_->raise();
        captions_->editCaption(track, index);
    });
    meterDock_ = makeDock(tr("Meters"), "meters", meter_);
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
    sourceDock_->raise();
    addDockWidget(Qt::LeftDockWidgetArea, binDock_);
    tabifyDockWidget(binDock_, effectsDock_);
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
    add(file, tr("Export E&DL (CMX 3600)…"), QKeySequence(), [this] { exportInterchange(false); });
    add(file, tr("Export &OpenTimelineIO…"), QKeySequence(), [this] { exportInterchange(true); });
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
    add(clipM, tr("Normalize &Loudness…"), QKeySequence(), [this] { normalizeLoudness(); });
    add(clipM, tr("Auto &Colour"), QKeySequence("Ctrl+Alt+C"), [this] { autoColor(); });
    add(clipM, tr("S&ynchronize by Audio"), QKeySequence(), [this] { syncByAudio(); });
    clipM->addSeparator();
    add(clipM, tr("&Insert from Source"), QKeySequence(Qt::Key_Comma), [this] { state_->insertFromSource(false); });
    add(clipM, tr("&Overwrite from Source"), QKeySequence(Qt::Key_Period), [this] { state_->insertFromSource(true); });
    add(clipM, tr("&Match Frame"), QKeySequence(Qt::Key_F), [this] { matchFrame(); });
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

    // ---- Sequence
    QMenu* seqM = menuBar()->addMenu(tr("&Sequence"));
    add(seqM, tr("Sequence &Settings…"), QKeySequence(), [this] { SequenceSettingsDialog::editActive(state_, this); });
    add(seqM, tr("&New Sequence…"), QKeySequence("Ctrl+Alt+N"), [this] { newSequence(); });
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
    sourceDock_->raise();
    active_ = Monitor::Source;
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
    dlg.exec();
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
    if (!maybeSave()) {
        e->ignore();
        return;
    }
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
                double local = (src - clip->sourceIn) / clip->speed;
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

void MainWindow::exportInterchange(bool otio) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    QSettings st = appSettings();
    QString ext = otio ? "otio" : "edl";
    QString path = QFileDialog::getSaveFileName(this, otio ? tr("Export OpenTimelineIO") : tr("Export EDL"),
                                                st.value("lastExportDir").toString() + "/" + QString::fromStdString(s->name) + "." + ext,
                                                otio ? tr("OpenTimelineIO (*.otio)") : tr("CMX 3600 EDL (*.edl)"));
    if (path.isEmpty()) return;
    std::string text = otio ? exportOtio(state_->project(), *s) : exportEdl(state_->project(), *s);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        QMessageBox::warning(this, tr("Export"), tr("Cannot write %1").arg(path));
        return;
    }
    st.setValue("lastExportDir", QFileInfo(path).absolutePath());
    statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
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
        state_->edit(tr("Sequence Settings"), [rate = spec->sampleRate](Project&, Sequence& s) {
            s.sampleRate = rate;
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
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Keyboard Shortcuts"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QList<QAction*> acts = actions();
    for (QAction* a : acts) {
        if (a->shortcut().isEmpty()) continue;
        int row = table->rowCount();
        table->insertRow(row);
        QString text = a->text();
        text.remove('&');
        table->setItem(row, 0, new QTableWidgetItem(text));
        QStringList keys;
        for (const auto& k : a->shortcuts()) keys << k.toString(QKeySequence::NativeText);
        table->setItem(row, 1, new QTableWidgetItem(keys.join(", ")));
    }
    table->resizeColumnToContents(0);
    lay->addWidget(table);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.resize(520, 640);
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
