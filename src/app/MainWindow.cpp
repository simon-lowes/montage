#include "MainWindow.h"
#include "LiveBridge.h"
#include "LiveLink.h"
#include "render/Ofx.h"
#include "AssistantPanel.h"
#include "CleanFeed.h"
#include "Settings.h"

#include "CompareDialog.h"

#include "Keymap.h"
#include "LoudnessReadout.h"
#include "Voiceover.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
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
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QFutureWatcher>
#include <QMenuBar>
#include <QPointer>
#include <QProgressDialog>
#include <QPageSize>
#include <QDateTime>
#include <QTextDocument>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QtConcurrent>
#include <QMessageBox>
#include <QScreen>
#include <QDir>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTableWidget>
#include <QChildEvent>
#include <QTabBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

#include "AudioMeterWidget.h"
#include "audio/PluginEffect.h"
#include "core/Surround.h"
#include "core/ColorGroups.h"
#include "core/GradeVersions.h"
#include "core/AudioChannels.h"
#include "render/ClipPlacement.h"
#include "core/Chapters.h"
#include "core/ChapterSuggest.h"
#include "core/MarkerList.h"
#include "render/AudioReactive.h"
#include "render/VfxPull.h"
#include "render/LutExport.h"
#include "media/TextReader.h"
#include "media/Decoder.h"
#include "media/HwAccel.h"
#include "media/MediaPool.h"
#include "media/Faces.h"
#include "render/AutoMix.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "ExportDialog.h"
#include "InspectorWidget.h"
#include "AutoDuckDialog.h"
#include "AutoMixDialog.h"
#include "ScriptCutDialog.h"
#include "QualityCheckDialog.h"
#include "OffloadDialog.h"
#include "ProductionPanel.h"
#include "core/AafImport.h"
#include "core/Production.h"
#include "core/ProjectLock.h"
#include "core/ProjectIO.h"
#include "ProjectManagerDialog.h"
#include "LinkMediaDialog.h"
#include "EffectPresetStore.h"
#include "media/Relink.h"
#include "KeyframePanel.h"
#include "MediaBinWidget.h"
#include "RenderQueue.h"
#include "RenderQueuePanel.h"
#include "MixerPanel.h"
#include "MulticamPanel.h"
#include "AdrPanel.h"
#include "AudioDescriptionDialog.h"
#include "CaptionsPanel.h"
#include "MaskOverlay.h"
#include "TransformOverlay.h"
#include "SequenceIndexPanel.h"
#include "PanFollowDialog.h"
#include "RedactFacesDialog.h"
#include "SpectralRepairDialog.h"
#include "ShotSearchPanel.h"
#include "ModelPacks.h"
#include "media/VisualSearch.h"
#include "PeoplePanel.h"
#include "SpeechDialog.h"
#include "TranscriptPanel.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "PluginManagerDialog.h"
#include "ScopesWidget.h"
#include "SpellUi.h"
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
#include "render/AutoBroll.h"
#include "render/Highlights.h"
#include "render/Shorts.h"
#include "render/Letterbox.h"
#include "render/LightLevel.h"
#include "render/Hdr10Plus.h"
#include "render/Versions.h"
#include "media/MicBleed.h"
#include "render/MusicEdit.h"
#include "render/VoiceMatch.h"
#include "render/Exporter.h"
#include "render/Processing.h"

namespace montage {


MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    state_ = new EditorState(this);
    liveLink_ = new LiveLink(state_, this);
    program_ = new PlaybackController(this);
    program_->setObjectName(QStringLiteral("programPlayback"));
    source_ = new PlaybackController(this);
    source_->setObjectName(QStringLiteral("sourcePlayback"));
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

    // The agent link comes back on if it was on when Montage last closed.
    connect(liveLink_, &LiveLink::applied, this, [this](const QString& label) { statusBar()->showMessage(label, 5000); });
    if (appSettings().value(QStringLiteral("agentLink/enabled"), false).toBool()) setAgentLink(true);

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
        if (program_->isPlaying()) {
            timeline_->followPlayhead(t);
            if (mixer_) mixer_->playbackPosition(t);
        }
    });
    // Fader automation is written while the program plays.
    connect(program_, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (!mixer_) return;
        if (playing) mixer_->playbackStarted(program_->position());
        else mixer_->playbackStopped(program_->position());
    });
    connect(state_, &EditorState::sourceChanged, this, &MainWindow::rebuildSourceProject);
    connect(state_, &EditorState::historyChanged, this, &MainWindow::updateActions);
    connect(state_, &EditorState::selectionChanged, this, &MainWindow::updateActions);
    connect(state_, &EditorState::fileStateChanged, this, &MainWindow::updateTitle);
    connect(state_, &EditorState::statusMessage, this, [this](const QString& text, int ms) { statusBar()->showMessage(text, ms); });
    connect(state_, &EditorState::mediaReady, this, [this] { program_->requestFrame(); });
    connect(program_, &PlaybackController::scopeFrameRendered, scopes_, &ScopesWidget::setSignal);
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

    // The selection's length (Resolve 21's duration readout): how long the selected clips run, first start to last end.
    selectionInfo_ = new QLabel(this);
    selectionInfo_->setObjectName(QStringLiteral("selectionInfo"));
    selectionInfo_->setStyleSheet(QString("color: %1; padding-right: 8px;").arg(theme::kText.name()));
    statusBar()->addPermanentWidget(selectionInfo_);
    auto showSelection = [this] { selectionInfo_->setText(selectionSummary()); };
    connect(state_, &EditorState::selectionChanged, this, showSelection);
    connect(state_, &EditorState::projectChanged, this, showSelection);
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
    {
        auto* centre = new QWidget(this);
        auto* v = new QVBoxLayout(centre);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        sequenceTabs_ = new QTabBar(centre);
        sequenceTabs_->setObjectName(QStringLiteral("sequenceTabs"));
        sequenceTabs_->setTabsClosable(true);
        sequenceTabs_->setMovable(true);
        sequenceTabs_->setExpanding(false);
        sequenceTabs_->setDocumentMode(true);
        sequenceTabs_->setToolTip(tr("Open sequences: click to switch, drag to reorder, close to put away (double-click a sequence in the bin to open it)"));
        v->addWidget(sequenceTabs_);
        v->addWidget(timeline_, 1);
        setCentralWidget(centre);
        connect(sequenceTabs_, &QTabBar::currentChanged, this, [this](int i) {
            if (i >= 0 && i < int(openSequences_.size())) state_->setActiveSequence(openSequences_[size_t(i)]);
        });
        connect(sequenceTabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
            if (from < 0 || to < 0 || from >= int(openSequences_.size()) || to >= int(openSequences_.size())) return;
            const Id id = openSequences_[size_t(from)];
            openSequences_.erase(openSequences_.begin() + from);
            openSequences_.insert(openSequences_.begin() + to, id);
        });
        connect(sequenceTabs_, &QTabBar::tabCloseRequested, this, [this](int i) {
            // The last open sequence stays: the timeline always shows one.
            if (openSequences_.size() <= 1 || i < 0 || i >= int(openSequences_.size())) return;
            const Id closing = openSequences_[size_t(i)];
            openSequences_.erase(openSequences_.begin() + i);
            if (state_->project().activeSequence == closing)
                state_->setActiveSequence(openSequences_[size_t(std::min<int>(i, int(openSequences_.size()) - 1))]);
            syncSequenceTabs();
        });
        for (auto sig : {&EditorState::projectChanged, &EditorState::sequenceSwitched}) connect(state_, sig, this, &MainWindow::syncSequenceTabs);
    }
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
    // The transform box first: mask and Corner Pin handles, installed after it, get the pointer before it.
    new TransformOverlay(state_, programPanel_->viewer());
    new MaskOverlay(state_, programPanel_->viewer());
    connect(programPanel_, &MonitorPanel::exportFrameRequested, this, &MainWindow::exportFrame);
    connect(timeline_, &TimelineWidget::trimViewChanged, this,
            [this](FrameTime left, FrameTime right, const QString& leftLabel, const QString& rightLabel) {
                if (trimView_ && !trimView_->isChecked()) return;
                // The program's copy of the project is brought up to date a moment later; the two-up needs this update.
                if (const Sequence* s = state_->sequence()) program_->setProject(state_->project(), s->id);
                programPanel_->showTrimView(left, right, leftLabel, rightLabel);
            });
    connect(timeline_, &TimelineWidget::trimViewEnded, programPanel_, &MonitorPanel::endTrimView);
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
    connect(bin_, &MediaBinWidget::linkMediaRequested, this, [this] { showLinkMedia(); });
    connect(state_, &EditorState::mediaFileChanged, this, [this] {  // a file changed on disk: show it as it is now
        program_->requestFrame();
        source_->requestFrame();
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
    assistant_ = new AssistantPanel(liveLink_, this);
    assistantDock_ = makeDock(tr("Assistant"), "assistant", assistant_);
    indexDock_ = makeDock(tr("Sequence Index"), "index", new SequenceIndexPanel(state_, this));
    multicamDock_ = makeDock(tr("Multicam"), "multicam", multicam_);
    adr_ = new AdrPanel(state_, program_, programPanel_->viewer(), this);
    adrDock_ = makeDock(tr("ADR"), "adr", adr_);
    keyframesDock_ = makeDock(tr("Keyframes"), "keyframes", new KeyframePanel(state_, this));
    queue_ = new RenderQueue(this);
    queueDock_ = makeDock(tr("Render Queue"), "renderqueue", new RenderQueuePanel(queue_, this));
    production_ = new ProductionPanel(state_, this);
    productionDock_ = makeDock(tr("Production"), "production", production_);
    connect(production_, &ProductionPanel::openRequested, this, [this](const QString& path, bool readOnly) {
        const bool here = !state_->filePath().isEmpty() && QFileInfo(path).absoluteFilePath() == QFileInfo(state_->filePath()).absoluteFilePath();
        if (here && !readOnly && state_->readOnly()) {
            takeEditInteractive();
            return;
        }
        if (here && readOnly == state_->readOnly()) return;
        if (!maybeSave()) return;
        openProjectAs(path, readOnly ? OpenMode::ReadOnly : OpenMode::Ask);
    });
    connect(production_, &ProductionPanel::newProjectRequested, this, &MainWindow::newProjectInProduction);
    connect(production_, &ProductionPanel::importRequested, this, [this](const QString& path) { importFromProjectDialog(path); });
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
    // Background render waits for a quiet spell after edits and playback.
    backgroundTimer_.setSingleShot(true);
    backgroundTimer_.setInterval(4000);
    connect(&backgroundTimer_, &QTimer::timeout, this, &MainWindow::startBackgroundRender);
    for (auto sig : {&EditorState::projectChanged, &EditorState::sequenceSwitched})
        connect(state_, sig, this, &MainWindow::stopBackgroundRender);
    connect(program_, &PlaybackController::playingChanged, this, &MainWindow::stopBackgroundRender);
    resetLayout();
}

void MainWindow::arrangeDocks(const DockGroups& top, const DockGroups& left, const DockGroups& right) {
    for (QDockWidget* d : docks_) {
        removeDockWidget(d);
        d->setFloating(false);
    }
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    std::vector<QDockWidget*> placed;
    auto place = [&](Qt::DockWidgetArea area, const DockGroups& groups, Qt::Orientation split) {
        // Split before tabbing: a split beside a tabbed panel only adds another tab.
        QDockWidget* prev = nullptr;
        for (const auto& g : groups) {
            if (g.empty()) continue;
            if (prev) splitDockWidget(prev, g[0], split);
            else addDockWidget(area, g[0]);
            prev = g[0];
        }
        for (const auto& g : groups) {
            for (size_t i = 1; i < g.size(); ++i) tabifyDockWidget(g[0], g[i]);
            for (QDockWidget* d : g) {
                d->show();
                placed.push_back(d);
            }
        }
    };
    place(Qt::TopDockWidgetArea, top, Qt::Horizontal);
    place(Qt::LeftDockWidgetArea, left, Qt::Vertical);
    place(Qt::RightDockWidgetArea, right, Qt::Vertical);
    QDockWidget* home = top.front().front();
    for (QDockWidget* d : docks_)
        if (std::find(placed.begin(), placed.end(), d) == placed.end()) {
            tabifyDockWidget(home, d);
            d->hide();
        }
    for (const DockGroups* area : {&top, &left, &right})
        for (const auto& g : *area)
            if (!g.empty()) g[0]->raise();
}

const QStringList& MainWindow::builtInWorkspaces() {
    static const QStringList names = {QStringLiteral("Editing"), QStringLiteral("Colour"),   QStringLiteral("Audio"),
                                      QStringLiteral("Effects"), QStringLiteral("Captions"), QStringLiteral("Logging")};
    return names;
}

void MainWindow::layOutBuiltIn(const QString& name) {
    const int h = height();
    if (name == "Colour") {
        // A large Program monitor between the scopes and the Inspector (Premiere's Color workspace).
        arrangeDocks({{scopesDock_}, {programDock_}}, {{binDock_, effectsDock_, indexDock_}}, {{inspectorDock_, keyframesDock_}});
        setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);  // the Inspector the full height
        resizeDocks({binDock_}, {260}, Qt::Horizontal);
        resizeDocks({inspectorDock_}, {400}, Qt::Horizontal);
        resizeDocks({scopesDock_, programDock_}, {520, 900}, Qt::Horizontal);
        resizeDocks({scopesDock_}, {h * 3 / 5}, Qt::Vertical);
    } else if (name == "Audio") {
        // The mixer where the Source monitor was, wide meters (Premiere's Audio workspace, Resolve's Fairlight page).
        arrangeDocks({{mixerDock_, adrDock_, sourceDock_}, {programDock_}}, {{binDock_, effectsDock_}}, {{meterDock_}});
        resizeDocks({mixerDock_, programDock_}, {900, 640}, Qt::Horizontal);
        resizeDocks({mixerDock_}, {h / 2 + 40}, Qt::Vertical);
        resizeDocks({meterDock_}, {110}, Qt::Horizontal);
        resizeDocks({binDock_}, {320}, Qt::Horizontal);
    } else if (name == "Effects") {
        arrangeDocks({{inspectorDock_, keyframesDock_, sourceDock_}, {programDock_}}, {{effectsDock_, binDock_, indexDock_}}, {{meterDock_}});
        resizeDocks({effectsDock_}, {360}, Qt::Horizontal);
        resizeDocks({meterDock_}, {70}, Qt::Horizontal);
        resizeDocks({inspectorDock_, programDock_}, {640, 900}, Qt::Horizontal);
        resizeDocks({inspectorDock_}, {h / 2 + 40}, Qt::Vertical);
    } else if (name == "Captions") {
        arrangeDocks({{captionsDock_, sourceDock_}, {programDock_}}, {{transcriptDock_, binDock_, indexDock_}}, {{meterDock_}});
        resizeDocks({transcriptDock_}, {400}, Qt::Horizontal);
        resizeDocks({meterDock_}, {70}, Qt::Horizontal);
        resizeDocks({captionsDock_, programDock_}, {640, 900}, Qt::Horizontal);
        resizeDocks({captionsDock_}, {h / 2 + 40}, Qt::Vertical);
    } else if (name == "Logging") {
        // A wide bin to log and search in, the Source monitor to mark in (Premiere's Assembly, Resolve's Media page).
        arrangeDocks({{sourceDock_, inspectorDock_}, {programDock_}}, {{binDock_, shotsDock_, peopleDock_, transcriptDock_, indexDock_, assistantDock_}}, {});
        resizeDocks({binDock_}, {760}, Qt::Horizontal);
        resizeDocks({sourceDock_, programDock_}, {560, 280}, Qt::Horizontal);
        resizeDocks({sourceDock_}, {h / 2 + 40}, Qt::Vertical);
    } else {  // Editing
        arrangeDocks({{sourceDock_, inspectorDock_, scopesDock_, mixerDock_, captionsDock_, multicamDock_, keyframesDock_}, {programDock_}},
                     {{binDock_, effectsDock_, transcriptDock_, shotsDock_, peopleDock_, indexDock_, queueDock_, assistantDock_}}, {{meterDock_}});
        resizeDocks({binDock_}, {380}, Qt::Horizontal);
        resizeDocks({meterDock_}, {70}, Qt::Horizontal);
        resizeDocks({sourceDock_, programDock_}, {720, 860}, Qt::Horizontal);
        resizeDocks({sourceDock_}, {h / 2 + 40}, Qt::Vertical);
    }
}

void MainWindow::resetLayout() {
    if (!builtInWorkspaces().contains(workspace_) && applyWorkspace(workspace_)) return;
    layOutBuiltIn(builtInWorkspaces().contains(workspace_) ? workspace_ : QStringLiteral("Editing"));
}

QStringList MainWindow::workspaces() const {
    QStringList names = builtInWorkspaces();
    QSettings s = appSettings();
    s.beginGroup(QStringLiteral("workspaces"));
    QStringList saved = s.childKeys();
    saved.sort(Qt::CaseInsensitive);
    for (const QString& n : saved)
        if (!names.contains(n)) names << n;
    return names;
}

bool MainWindow::applyWorkspace(const QString& name) {
    if (builtInWorkspaces().contains(name)) {
        layOutBuiltIn(name);
    } else {
        QSettings s = appSettings();
        const QByteArray layout = s.value(QStringLiteral("workspaces/") + name).toByteArray();
        if (name.isEmpty() || layout.isEmpty() || !restoreState(layout, 1)) return false;
    }
    workspace_ = name;
    appSettings().setValue(QStringLiteral("window/workspace"), name);
    syncWorkspaceUi();
    return true;
}

bool MainWindow::saveWorkspace(const QString& name) {
    const QString n = name.trimmed();
    if (n.isEmpty() || builtInWorkspaces().contains(n, Qt::CaseInsensitive) || n.contains('/') || n.contains('\\')) return false;
    {
        QSettings s = appSettings();
        s.setValue(QStringLiteral("workspaces/") + n, saveState(1));
    }
    workspace_ = n;
    appSettings().setValue(QStringLiteral("window/workspace"), n);
    syncWorkspaceUi();
    return true;
}

bool MainWindow::deleteWorkspace(const QString& name) {
    if (builtInWorkspaces().contains(name) || !workspaces().contains(name)) return false;
    {
        QSettings s = appSettings();
        s.remove(QStringLiteral("workspaces/") + name);
    }
    if (workspace_ == name) workspace_ = QStringLiteral("Editing");  // the panels stay where they are
    syncWorkspaceUi();
    return true;
}

// The workspace bar, and the saved workspaces in Window › Workspaces, follow the list and the current one.
void MainWindow::syncWorkspaceUi() {
    if (!workspaceBar_ || !workspaceMenu_) return;
    syncingWorkspace_ = true;
    const QStringList names = workspaces();
    while (workspaceBar_->count() > 0) workspaceBar_->removeTab(0);
    for (const QString& n : names) workspaceBar_->addTab(n);
    workspaceBar_->setCurrentIndex(int(names.indexOf(workspace_)));
    std::erase_if(workspaceActions_, [this](QAction* a) {
        if (builtInWorkspaces().contains(a->data().toString())) return false;
        workspaceGroup_->removeAction(a);
        delete a;
        return true;
    });
    deleteWorkspaceMenu_->clear();
    for (const QString& n : names) {
        if (builtInWorkspaces().contains(n)) continue;
        auto* a = new QAction(n, workspaceMenu_);
        a->setData(n);
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [this, n] { applyWorkspace(n); });
        workspaceMenu_->insertAction(workspaceCustomEnd_, a);
        workspaceGroup_->addAction(a);
        workspaceActions_.push_back(a);
        deleteWorkspaceMenu_->addAction(n, this, [this, n] { deleteWorkspace(n); });
    }
    deleteWorkspaceMenu_->setEnabled(!deleteWorkspaceMenu_->isEmpty());
    for (QAction* a : workspaceActions_) a->setChecked(a->data().toString() == workspace_);
    syncingWorkspace_ = false;
}

void MainWindow::restoreLayout() {
    QSettings s = appSettings();
    if (s.contains("window/geometry")) restoreGeometry(s.value("window/geometry").toByteArray());
    if (s.contains("window/state")) restoreState(s.value("window/state").toByteArray(), 1);
    const QString ws = s.value("window/workspace").toString();
    if (workspaces().contains(ws)) workspace_ = ws;  // laid out as it was left, by the state above
    syncWorkspaceUi();
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
    add(file, tr("Import Image Sequence…"), QKeySequence(), [this] { bin_->importImageSequenceDialog(); })->setObjectName(QStringLiteral("importImageSequence"));
    add(file, tr("Watch Folders…"), QKeySequence(), [this] { watchFoldersDialog(); })->setObjectName(QStringLiteral("watchFolders"));
    add(file, tr("&Export Media…"), QKeySequence("Ctrl+M"), [this] { exportMedia(); });
    add(file, tr("Export &Versions…"), QKeySequence(), [this] { exportVersionsDialog(); })->setObjectName(QStringLiteral("exportVersions"));
    add(file, tr("Export for Re&view…"), QKeySequence(), [this] { exportForReviewDialog(); })->setObjectName(QStringLiteral("exportForReview"));
    add(file, tr("Export &DCP (Digital Cinema)…"), QKeySequence(), [this] { exportDcpDialog(); })->setObjectName(QStringLiteral("exportDcp"));
    add(file, tr("Export &IMF Master…"), QKeySequence(), [this] { exportImfDialog(); })->setObjectName(QStringLiteral("exportImf"));
    add(file, tr("Export Immersive Master (&ADM BWF)…"), QKeySequence(), [this] { exportAdmDialog(); })
        ->setObjectName(QStringLiteral("exportAdm"));
    add(file, tr("Import Review Notes…"), QKeySequence(), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Review Notes"), appSettings().value(QStringLiteral("export/lastDirectory")).toString(),
                                                          tr("Review notes (*.json);;Marker lists (*.csv *.txt *.tsv)"));
        if (!path.isEmpty()) importMarkers(path);
    })->setObjectName(QStringLiteral("importReviewNotes"));
    add(file, tr("Project &Manager…"), QKeySequence(), [this] {
        ProjectManagerDialog dlg(state_, this);
        if (dlg.exec() == QDialog::Accepted) runProjectManager(dlg.options());
    })->setObjectName(QStringLiteral("projectManager"));
    {
        QMenu* prod = file->addMenu(tr("Productio&n"));
        add(prod, tr("&New Production…"), QKeySequence(), [this] {
            const QString folder = QFileDialog::getExistingDirectory(this, tr("New Production: Choose or Make Its Folder (on the Shared Drive)"),
                                                                     appSettings().value("lastProjectDir").toString());
            if (folder.isEmpty()) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("New Production"), tr("Name:"), QLineEdit::Normal, QDir(folder).dirName(), &ok);
            if (ok) openProduction(folder, true, name);
        })->setObjectName(QStringLiteral("newProduction"));
        add(prod, tr("&Open Production…"), QKeySequence(), [this] {
            const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Production"), appSettings().value("lastProjectDir").toString());
            if (folder.isEmpty()) return;
            if (!isProduction(folder.toStdString()) &&
                QMessageBox::question(this, tr("Open Production"), tr("%1 is not a production yet. Make it one?").arg(QDir::toNativeSeparators(folder))) !=
                    QMessageBox::Yes)
                return;
            openProduction(folder, true);
        })->setObjectName(QStringLiteral("openProduction"));
        add(prod, tr("&Import from Project…"), QKeySequence(), [this] { importFromProjectDialog(); })->setObjectName(QStringLiteral("importFromProject"));
        takeEdit_ = add(file, tr("&Edit Project (Take the Lock)"), QKeySequence(), [this] { takeEditInteractive(); });
        takeEdit_->setObjectName(QStringLiteral("takeEdit"));
        takeEdit_->setEnabled(false);
        connect(state_, &EditorState::lockStateChanged, this, [this] {
            updateTitle();
            takeEdit_->setEnabled(state_->canTakeEdit());
        });
    }
    add(file, tr("&Offload Card…"), QKeySequence(), [this] {
        OffloadDialog dlg(state_, this);
        dlg.exec();
    })->setObjectName(QStringLiteral("offloadCard"));
    add(file, tr("Verify Media &Hash List…"), QKeySequence(), [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Verify a Folder Against Its ASC MHL Hash List"));
        if (folder.isEmpty()) return;
        QString report;
        const MhlVerifyResult v = verifyMhlWithProgress(this, folder, false, &report);
        QMessageBox box(v.ok ? QMessageBox::Information : QMessageBox::Warning, tr("Verify Media Hash List"), report, QMessageBox::Close, this);
        QPushButton* record = v.error.isEmpty() ? box.addButton(tr("Record a Generation"), QMessageBox::ActionRole) : nullptr;
        box.exec();
        if (record && box.clickedButton() == record) {
            verifyMhlWithProgress(this, folder, true, &report);
            QMessageBox::information(this, tr("Verify Media Hash List"), report);
        }
    })->setObjectName(QStringLiteral("verifyMhl"));
    add(file, tr("&Link Media…"), QKeySequence(), [this] { showLinkMedia(); })->setObjectName(QStringLiteral("linkMediaAction"));
    add(file, tr("Export &Frame…"), QKeySequence("Ctrl+Shift+E"), [this] { exportFrame(); });
    add(file, tr("Export &VFX Pulls…"), QKeySequence(), [this] { vfxPullDialog(); })->setObjectName(QStringLiteral("vfxPulls"));
    add(file, tr("&Import Timeline (FCP XML, FCPXML, OTIO, EDL, AAF)…"), QKeySequence(), [this] { importTimeline(); });
    add(file, tr("Export Final Cut Pro &7 XML (Premiere, Resolve)…"), QKeySequence(), [this] { exportInterchange(Interchange::Fcp7Xml); });
    add(file, tr("Export &FCPXML (Final Cut Pro)…"), QKeySequence(), [this] { exportInterchange(Interchange::FcpXml); });
    add(file, tr("Export E&DL (CMX 3600)…"), QKeySequence(), [this] { exportInterchange(Interchange::Edl); });
    add(file, tr("Export &OpenTimelineIO…"), QKeySequence(), [this] { exportInterchange(Interchange::Otio); });
    add(file, tr("Export &AAF (Pro Tools, Fairlight, Media Composer)…"), QKeySequence(), [this] { exportAafDialog(); })
        ->setObjectName(QStringLiteral("exportAaf"));
    add(file, tr("Export AAF, Sound Only (Pro Tools, Fairlight)…"), QKeySequence(), [this] { exportAafDialog(false); })
        ->setObjectName(QStringLiteral("exportAafSound"));
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
    add(editM, tr("Delete &Gaps"), QKeySequence(), [this] { deleteGaps(); })->setObjectName(QStringLiteral("deleteGaps"));
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
    // Spelling: as you type in captions and titles, and the dictionary for titles.
    editM->addSeparator();
    QMenu* spellM = editM->addMenu(tr("Spe&lling"));
    spellM->setObjectName(QStringLiteral("spellingMenu"));
    QAction* asYouType = spellM->addAction(tr("Check Spelling as You Type"));
    asYouType->setObjectName(QStringLiteral("checkSpelling"));
    asYouType->setCheckable(true);
    asYouType->setChecked(spellCheckingOn());
    connect(asYouType, &QAction::toggled, this, [this](bool on) {
        setSpellCheckingOn(on);
        emit state_->projectChanged();  // marks redrawn
    });
    spellM->addSeparator();
    auto* dictionaries = new QActionGroup(spellM);
    for (const auto& [id, name] : {std::pair{"en-US", tr("English (US)")}, std::pair{"en-GB", tr("English (UK)")}}) {
        QAction* a = spellM->addAction(name);
        a->setObjectName(QStringLiteral("spelling-%1").arg(QLatin1String(id)));
        a->setCheckable(true);
        a->setChecked(titleSpellingLanguage() == QLatin1String(id));
        a->setToolTip(tr("The dictionary for titles; captions use their track's language"));
        dictionaries->addAction(a);
        const QString lang = QLatin1String(id);
        connect(a, &QAction::triggered, this, [this, lang] {
            setTitleSpellingLanguage(lang);
            emit state_->projectChanged();
        });
    }

    // ---- Clip
    QMenu* clipM = menuBar()->addMenu(tr("&Clip"));
    QAction* speed = add(clipM, tr("&Speed / Duration…"), QKeySequence("Ctrl+R"), [this] { speedDialog(); });
    add(clipM, tr("Add &Edit"), QKeySequence("Ctrl+K"), [this] { addEdit(false); });
    add(clipM, tr("Add Edit to &All Tracks"), QKeySequence("Ctrl+Shift+K"), [this] { addEdit(true); });
    QAction* trans = add(clipM, tr("Apply Default &Transition"), QKeySequence("Ctrl+D"), [this] { addDefaultTransition(false); });
    add(clipM, tr("Apply Default Audio &Crossfade"), QKeySequence("Ctrl+Shift+D"), [this] { addDefaultTransition(true); });
    add(clipM, tr("Apply Default Transitions to Selection"), QKeySequence("Shift+D"), [this] { addTransitionsToSelection(); })
        ->setObjectName(QStringLiteral("transitionsToSelection"));
    QAction* nest = add(clipM, tr("&Nest (Compound Clip)…"), QKeySequence(), [this] {
        auto sel = state_->selectedClips();
        if (sel.empty()) return;
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Nest"), tr("Compound clip name:"), QLineEdit::Normal, tr("Nested Sequence"), &ok);
        if (!ok || name.isEmpty()) return;
        state_->apply(tr("Nest"), [sel, name](Project& p, Sequence& s) { return edit::makeCompound(p, s, sel, name.toStdString()); });
    });
    add(clipM, tr("Detect &Scene Cuts"), QKeySequence(), [this] { detectScenes(); });
    add(clipM, tr("Export &LUT from Grade…"), QKeySequence(), [this] {
        if (!clipForCommand()) {
            statusBar()->showMessage(tr("Select a graded clip under the playhead to export its LUT"), 5000);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("Export LUT"), QString(), tr("LUT files (*.cube)"));
        if (!path.isEmpty()) exportClipLut(path.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive) ? path : path + QStringLiteral(".cube"));
    })->setObjectName(QStringLiteral("exportLut"));
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
    add(clipM, tr("Remove Mic &Bleed…"), QKeySequence(), withSeq([this] { micBleedDialog(); }))->setObjectName(QStringLiteral("removeMicBleed"));
    add(clipM, tr("S&pectral Repair…"), QKeySequence(), withSeq([this] { spectralRepairDialog(); }))->setObjectName(QStringLiteral("spectralRepair"));
    add(clipM, tr("Pan to &Follow…"), QKeySequence(), withSeq([this] { panFollowDialog(); }))->setObjectName(QStringLiteral("panFollow"));
    add(clipM, tr("Redact &Faces…"), QKeySequence(), withSeq([this] { redactFacesDialog(); }))->setObjectName(QStringLiteral("redactFaces"));
    add(clipM, tr("Remove Letterbo&x"), QKeySequence(), withSeq([this] { removeLetterbox(); }))->setObjectName(QStringLiteral("removeLetterbox"));
    // Colour groups: grade shots together, before and after each clip's own grade (core/ColorGroups.h).
    QMenu* groupM = clipM->addMenu(tr("Colour &Group"));
    groupM->setObjectName(QStringLiteral("colorGroupMenu"));
    connect(groupM, &QMenu::aboutToShow, this, [this, groupM] {
        groupM->clear();
        const Sequence* s = state_->sequence();
        if (!s) return;
        groupM->addAction(tr("New Group from Selection…"), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("New Colour Group"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
            if (ok) newColorGroup(name.trimmed());
        })->setObjectName(QStringLiteral("newColorGroup"));
        QMenu* join = groupM->addMenu(tr("Add Selection to Group"));
        join->setEnabled(!s->colorGroups.empty());
        for (const ColorGroup& g : s->colorGroups) {
            const Id id = g.id;
            join->addAction(QString::fromStdString(g.name), this, [this, id] { addToColorGroup(id); });
        }
        groupM->addAction(tr("Remove Selection from Group"), this, [this] { removeFromColorGroup(); })->setObjectName(QStringLiteral("removeFromColorGroup"));
        if (s->colorGroups.empty()) return;
        groupM->addSeparator();
        QMenu* rename = groupM->addMenu(tr("Rename Group"));
        QMenu* remove = groupM->addMenu(tr("Delete Group"));
        QMenu* select = groupM->addMenu(tr("Select Group's Clips"));
        for (const ColorGroup& g : s->colorGroups) {
            const Id id = g.id;
            const QString name = QString::fromStdString(g.name);
            rename->addAction(name, this, [this, id, name] {
                bool ok = false;
                const QString to = QInputDialog::getText(this, tr("Rename Colour Group"), tr("Name:"), QLineEdit::Normal, name, &ok).trimmed();
                if (ok && !to.isEmpty())
                    state_->edit(tr("Rename Colour Group"), [id, to](Project&, Sequence& sq) { return edit::renameColorGroup(sq, id, to.toStdString()).ok; });
            });
            remove->addAction(name, this, [this, id] {
                state_->edit(tr("Delete Colour Group"), [id](Project&, Sequence& sq) { return edit::deleteColorGroup(sq, id).ok; });
            });
            select->addAction(name, this, [this, id] {
                if (const Sequence* sq = state_->sequence()) state_->setSelection(colorGroupMembers(*sq, id), false);
            });
        }
    });
    add(clipM, tr("Auto &Colour"), QKeySequence("Ctrl+Alt+C"), [this] { autoColor(); });
    add(clipM, tr("Set Colour &Reference"), QKeySequence(), [this] { setColourReference(); })
        ->setObjectName(QStringLiteral("setColourReference"));
    add(clipM, tr("&Key Out Green / Blue Screen"), QKeySequence(), [this] { keyOutScreen(); })->setObjectName(QStringLiteral("keyScreen"));
    add(clipM, tr("Match Colour to Reference"), QKeySequence("Ctrl+Alt+Shift+C"), [this] { matchColour(); })
        ->setObjectName(QStringLiteral("matchColour"));
    compareRef_ = add(clipM, tr("Compare with Reference"), QKeySequence(), [this] { setCompareWithReference(compareRef_->isChecked()); });
    compareRef_->setCheckable(true);
    compareRef_->setObjectName(QStringLiteral("compareReference"));
    add(clipM, tr("Auto Reframe"), QKeySequence(), [this] { autoReframeClips(); })->setObjectName(QStringLiteral("autoReframeClips"));
    add(clipM, tr("Add Frame &Hold"), QKeySequence("Shift+F"), [this] { addFrameHold(); })->setObjectName(QStringLiteral("addFrameHold"));
    {
        QMenu* extendM = clipM->addMenu(tr("E&xtend Clip"));
        extendM->setObjectName(QStringLiteral("extendClipMenu"));
        auto extendBy = [this](double seconds) {
            const Clip* c = state_->primaryClip();
            const Sequence* s = state_->sequence();
            if (!c || !s) return state_->message(tr("Select a video clip to extend"));
            extendClip(c->id, FrameTime(std::llround(seconds * s->fpsValue())));
        };
        add(extendM, tr("By 1 Second"), QKeySequence(), [extendBy] { extendBy(1); })->setObjectName(QStringLiteral("extendClip1s"));
        add(extendM, tr("By 2 Seconds"), QKeySequence(), [extendBy] { extendBy(2); })->setObjectName(QStringLiteral("extendClip2s"));
        add(extendM, tr("To the Playhead"), QKeySequence(), [this] {
            const Clip* c = state_->primaryClip();
            if (!c) return state_->message(tr("Select a video clip to extend"));
            if (state_->playhead() <= c->end()) return state_->message(tr("Put the playhead after the clip's end"));
            extendClip(c->id, state_->playhead() - c->end() + 1);
        })->setObjectName(QStringLiteral("extendClipToPlayhead"));
    }
    {
        // Speed ramp presets on the selected clips: the same footage in the same length, paced differently.
        QMenu* ramps = clipM->addMenu(tr("Speed &Ramp"));
        for (const edit::SpeedRampPreset& r : edit::speedRampPresets()) {
            QAction* a = add(ramps, tr(r.name.c_str()), QKeySequence(), [this, id = r.id, name = r.name] { speedRamp(id, tr(name.c_str())); });
            a->setObjectName(QString::fromStdString("ramp_" + r.id));
            a->setStatusTip(tr(r.description.c_str()));
        }
        ramps->addSeparator();
        add(ramps, tr("Remove Ramp"), QKeySequence(), [this] { speedRamp("none", tr("Remove Ramp")); })
            ->setObjectName(QStringLiteral("ramp_none"));
    }
    {
        // Grade versions on the selected picture clips: a new one each, or all to the next or previous one.
        QMenu* grades = clipM->addMenu(tr("&Grade Versions"));
        auto onSelected = [this](const QString& label, std::function<edit::Result(Project&, Sequence&, Id)> fn) {
            const auto sel = state_->selectedClips();
            state_->apply(label, [sel, fn](Project& p, Sequence& s) {
                bool any = false;
                for (Id id : sel) {
                    bool video = false;
                    for (const Track& t : s.videoTracks)
                        for (const Clip& c : t.clips) video = video || c.id == id;
                    if (video && fn(p, s, id).ok) any = true;
                }
                return any ? edit::Result{} : edit::Result::fail({});
            });
        };
        add(grades, tr("New Version"), QKeySequence(), [onSelected] {
            onSelected(tr("New Grade Version"), [](Project& p, Sequence& s, Id id) { return edit::addGradeVersion(p, s, id); });
        })->setObjectName(QStringLiteral("gradeNewVersion"));
        for (const int step : {1, -1})
            add(grades, step > 0 ? tr("Next Version") : tr("Previous Version"), QKeySequence(), [onSelected, step] {
                onSelected(tr("Switch Grade Version"), [step](Project&, Sequence& s, Id id) {
                    const Clip* c = edit::clipById(s, id);
                    if (!c || c->gradeVersions.size() < 2) return edit::Result::fail({});
                    const int n = int(c->gradeVersions.size());
                    return edit::switchGradeVersion(s, id, ((c->gradeVersion + step) % n + n) % n);
                });
            })->setObjectName(step > 0 ? QStringLiteral("gradeNextVersion") : QStringLiteral("gradePreviousVersion"));
    }
    add(clipM, tr("Animate to Audio…"), QKeySequence(), [this] { animateToAudioDialog(); })->setObjectName(QStringLiteral("animateToAudio"));
    {
        // Align in Frame: the selected pictures lined up with the frame, inside the action-safe margin.
        QMenu* align = clipM->addMenu(tr("Align in Frame"));
        align->setObjectName(QStringLiteral("alignMenu"));
        const std::pair<QString, const char*> places[] = {{tr("Centre"), "center"},         {tr("Top"), "top"},
                                                         {tr("Bottom"), "bottom"},         {tr("Left"), "left"},
                                                         {tr("Right"), "right"},           {tr("Top Left"), "top_left"},
                                                         {tr("Top Right"), "top_right"},   {tr("Bottom Left"), "bottom_left"},
                                                         {tr("Bottom Right"), "bottom_right"}};
        for (const auto& [label, name] : places)
            add(align, label, QKeySequence(), [this, n = std::string(name)] { alignSelection(n); })
                ->setObjectName(QStringLiteral("align_") + QString::fromLatin1(name));
    }
    {
        QMenu* channels = clipM->addMenu(tr("Audio Channels"));
        channels->setObjectName(QStringLiteral("audioChannelsMenu"));
        add(channels, tr("Choose Channels…"), QKeySequence(), [this] { audioChannelsDialog(); })->setObjectName(QStringLiteral("audioChannels"));
        add(channels, tr("Split into Mono Clips"), QKeySequence(), [this] { splitSelectionChannels(false); })->setObjectName(QStringLiteral("splitChannels"));
        add(channels, tr("Split into Stereo Pairs"), QKeySequence(), [this] { splitSelectionChannels(true); })->setObjectName(QStringLiteral("splitChannelPairs"));
        add(channels, tr("Play Stereo Mix"), QKeySequence(), [this] { setSelectionChannels({}); })->setObjectName(QStringLiteral("channelsMix"));
    }
    add(clipM, tr("Swap with Previous Clip"), QKeySequence("Ctrl+Shift+,"), [this] { swapClip(false); })->setObjectName(QStringLiteral("swapPrevious"));
    add(clipM, tr("Swap with Next Clip"), QKeySequence("Ctrl+Shift+."), [this] { swapClip(true); })->setObjectName(QStringLiteral("swapNext"));
    add(clipM, tr("Join Through Edits"), QKeySequence(), [this] { joinThroughEdits(); })->setObjectName(QStringLiteral("joinThroughEdits"));
    add(clipM, tr("Close Up"), QKeySequence(), [this] { closeUp(); })->setObjectName(QStringLiteral("closeUp"));
    // Video layouts: the selected video clips (or those under the playhead) sharing the frame.
    QMenu* layoutM = clipM->addMenu(tr("Layout"));
    layoutM->setObjectName(QStringLiteral("layoutMenu"));
    const std::tuple<edit::Layout, QString, const char*> layouts[] = {
        {edit::Layout::PictureInPicture, tr("Picture in Picture"), "layoutPip"},
        {edit::Layout::SideBySide, tr("Side by Side"), "layoutSideBySide"},
        {edit::Layout::TopAndBottom, tr("Top and Bottom"), "layoutTopBottom"},
        {edit::Layout::ThreeAcross, tr("Three Across"), "layoutThreeAcross"},
        {edit::Layout::Grid, tr("2 × 2 Grid"), "layoutGrid"},
        {edit::Layout::FullFrame, tr("Full Frame (Reset)"), "layoutFullFrame"}};
    for (const auto& [layout, name, object] : layouts)
        add(layoutM, name, QKeySequence(), [this, layout = layout] { arrangeLayout(layout); })->setObjectName(QLatin1String(object));
    // Audio roles (Final Cut's roles): what a clip is, for muting a role and stems by role.
    roleMenu_ = clipM->addMenu(tr("Audio Role"));
    roleMenu_->setObjectName(QStringLiteral("audioRoleMenu"));
    add(roleMenu_, tr("Dialogue"), QKeySequence("Ctrl+Alt+1"), [this] { setSelectedRole(QStringLiteral("Dialogue")); })
        ->setObjectName(QStringLiteral("roleDialogue"));
    add(roleMenu_, tr("Music"), QKeySequence("Ctrl+Alt+2"), [this] { setSelectedRole(QStringLiteral("Music")); })
        ->setObjectName(QStringLiteral("roleMusic"));
    add(roleMenu_, tr("Effects"), QKeySequence("Ctrl+Alt+3"), [this] { setSelectedRole(QStringLiteral("Effects")); })
        ->setObjectName(QStringLiteral("roleEffects"));
    roleMenu_->addSeparator()->setObjectName(QStringLiteral("customRolesStart"));
    roleMenu_->addSeparator();
    add(roleMenu_, tr("New Role…"), QKeySequence(), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Role"), tr("Role name:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) setSelectedRole(name);
    })->setObjectName(QStringLiteral("roleNew"));
    add(roleMenu_, tr("No Role"), QKeySequence(), [this] { setSelectedRole(QString()); })->setObjectName(QStringLiteral("roleNone"));
    add(roleMenu_, tr("Detect Roles by Listening"), QKeySequence(), [this] { detectRoles(); })->setObjectName(QStringLiteral("detectRoles"));
    connect(roleMenu_, &QMenu::aboutToShow, this, [this] {
        // The sequence's own roles between the standard ones and the rest, and a tick on the selection's role.
        const Sequence* s = state_->sequence();
        for (QAction* a : roleMenu_->actions())
            if (a->property("customRole").isValid()) delete a;
        std::string current;
        bool mixed = false;
        if (s)
            for (Id id : edit::expandLinks(*s, state_->selectedClips()))
                if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio) {
                    const std::string& r = edit::clipById(*s, id)->role;
                    if (!current.empty() && r != current) mixed = true;
                    current = r;
                }
        QAction* before = nullptr;
        bool afterStart = false;
        for (QAction* a : roleMenu_->actions()) {
            if (afterStart) {
                before = a;
                break;
            }
            afterStart = a->objectName() == QLatin1String("customRolesStart");
        }
        if (s)
            for (const std::string& r : edit::sequenceRoles(*s)) {
                if (std::find(edit::kStandardRoles.begin(), edit::kStandardRoles.end(), r) != edit::kStandardRoles.end()) continue;
                const QString name = QString::fromStdString(r);
                auto* a = new QAction(name, roleMenu_);
                a->setProperty("customRole", name);
                connect(a, &QAction::triggered, this, [this, name] { setSelectedRole(name); });
                roleMenu_->insertAction(before, a);
            }
        for (QAction* a : roleMenu_->actions()) {
            const QString role = a->property("customRole").isValid() ? a->property("customRole").toString()
                                 : a->objectName() == QLatin1String("roleDialogue") ? QStringLiteral("Dialogue")
                                 : a->objectName() == QLatin1String("roleMusic")    ? QStringLiteral("Music")
                                 : a->objectName() == QLatin1String("roleEffects")  ? QStringLiteral("Effects")
                                                                                    : QString();
            if (role.isEmpty()) continue;
            a->setCheckable(true);
            a->setChecked(!mixed && role.toStdString() == current);
        }
    });
    add(clipM, tr("Save Effects as Preset…"), QKeySequence(), [this] {
        const Clip* c = state_->primaryClip();
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Effects as Preset"), tr("Preset name:"), QLineEdit::Normal,
                                                   c ? QString::fromStdString(c->name) : QString(), &ok);
        if (ok) saveEffectsAsPreset(name);
    })->setObjectName(QStringLiteral("saveEffectPreset"));
    QMenu* auditionM = clipM->addMenu(tr("Audition"));
    add(auditionM, tr("Add Selected Media as Takes"), QKeySequence("Ctrl+Alt+Y"), [this] { addTakesFromBin(); })
        ->setObjectName(QStringLiteral("addTakes"));
    add(auditionM, tr("Next Take"), QKeySequence("Ctrl+Alt+Right"), [this] { cycleTake(1); })->setObjectName(QStringLiteral("nextTake"));
    add(auditionM, tr("Previous Take"), QKeySequence("Ctrl+Alt+Left"), [this] { cycleTake(-1); })->setObjectName(QStringLiteral("previousTake"));
    add(auditionM, tr("Finalize Audition"), QKeySequence(), [this] { finalizeAudition(); })->setObjectName(QStringLiteral("finalizeAudition"));
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
    add(clipM, tr("Make Highlights…"), QKeySequence(), [this] { highlightsDialog(); })->setObjectName(QStringLiteral("makeHighlights"));
    add(clipM, tr("Import Embedded Captions"), QKeySequence(), [this] { importEmbeddedCaptions(); })
        ->setObjectName(QStringLiteral("importEmbeddedCaptions"));
    add(clipM, tr("Read Burned-In Subtitles…"), QKeySequence(), [this] { readBurnedInSubtitles(); })
        ->setObjectName(QStringLiteral("readBurnedInSubtitles"));
    add(clipM, tr("Add B-Roll by What Is Said"), QKeySequence(), [this] { addBroll(); })->setObjectName(QStringLiteral("autoBroll"));
    add(clipM, tr("S&ynchronize by Audio"), QKeySequence(), [this] { syncByAudio(); });
    clipM->addSeparator();
    add(clipM, tr("&Insert from Source"), QKeySequence(Qt::Key_Comma), [this] { state_->insertFromSource(false); });
    add(clipM, tr("&Overwrite from Source"), QKeySequence(Qt::Key_Period), [this] { state_->insertFromSource(true); });
    add(clipM, tr("Place on Top"), QKeySequence(Qt::Key_F12), [this] { state_->sourceEdit(EditorState::SourceEdit::PlaceOnTop); })
        ->setObjectName(QStringLiteral("placeOnTop"));
    add(clipM, tr("Append at End"), QKeySequence("Shift+F12"), [this] { state_->sourceEdit(EditorState::SourceEdit::Append); })
        ->setObjectName(QStringLiteral("appendAtEnd"));
    add(clipM, tr("Ripple Overwrite"), QKeySequence("Shift+F10"), [this] { state_->sourceEdit(EditorState::SourceEdit::RippleOverwrite); })
        ->setObjectName(QStringLiteral("rippleOverwrite"));
    add(clipM, tr("Smart Insert"), QKeySequence(), [this] { state_->sourceEdit(EditorState::SourceEdit::SmartInsert); })
        ->setObjectName(QStringLiteral("smartInsert"));
    add(clipM, tr("&Match Frame"), QKeySequence(Qt::Key_F), [this] { matchFrame(); })->setObjectName(QStringLiteral("matchFrame"));
    add(clipM, tr("Reverse Match Frame"), QKeySequence("Shift+R"), [this] { reverseMatchFrame(); })
        ->setObjectName(QStringLiteral("reverseMatchFrame"));
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
    add(seqM, tr("Make &Shorts…"), QKeySequence(), [this] { shortsDialog(); })->setObjectName(QStringLiteral("makeShorts"));
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
    add(seqM, tr("A&DR (Dialogue Replacement)…"), QKeySequence(), [this] {
        // The cue list and recording to picture, in its panel beside the Program monitor.
        adrDock_->show();
        adrDock_->raise();
    })->setObjectName(QStringLiteral("showAdr"));
    add(seqM, tr("Audio &Description…"), QKeySequence(), [this] {
        if (!state_->sequence()) return;
        // Kept open beside the work while descriptions are written.
        if (!audioDescription_) audioDescription_ = new AudioDescriptionDialog(state_, this);
        audioDescription_->show();
        audioDescription_->raise();
    })->setObjectName(QStringLiteral("audioDescription"));
    add(seqM, tr("&Generate Voiceover…"), QKeySequence("Ctrl+Alt+G"), [this] {
        if (!state_->sequence()) return;
        SpeechDialog dlg(state_, this);
        dlg.exec();
    })->setObjectName(QStringLiteral("generateVoiceover"));
    {
        QAction* mute = add(seqM, tr("Global &Mute"), QKeySequence(), [] {});
        mute->setObjectName(QStringLiteral("globalMute"));
        mute->setCheckable(true);
        connect(mute, &QAction::toggled, this, [this](bool on) {
            program_->setGlobalMute(on);
            state_->message(on ? tr("Global Mute: playback is silent (clips, tracks and exports are unchanged)") : tr("Global Mute off"), 4000);
        });
    }
    add(seqM, tr("Clear Solo"), QKeySequence(), [this] { clearOrRestoreSolo(); })->setObjectName(QStringLiteral("clearSolo"));
    add(seqM, tr("Render In to Out"), QKeySequence(Qt::Key_Return), [this] { renderInToOut(); })
        ->setObjectName(QStringLiteral("renderInToOut"));
    add(seqM, tr("Delete Render Files"), QKeySequence(), [this] { deleteRenderFiles(); })
        ->setObjectName(QStringLiteral("deleteRenderFiles"));
    {
        QAction* bg = add(seqM, tr("&Background Render"), QKeySequence(), [this](bool on) { setBackgroundRender(on); });
        bg->setCheckable(true);
        bg->setObjectName(QStringLiteral("backgroundRender"));
        bg->setToolTip(tr("Render stretches with effects, titles or transitions while you are not editing or playing"));
        backgroundRender_ = appSettings().value("render/background", false).toBool();
        bg->setChecked(backgroundRender_);
    }
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
    add(seqM, tr("Select Nearest Edit (Trim Mode)"), QKeySequence("Shift+T"), [this] { selectNearestEdit(); })->setObjectName(QStringLiteral("selectEdit"));
    add(seqM, tr("Cycle Trim Side"), QKeySequence("Alt+T"), [this] { cycleTrimSide(); })->setObjectName(QStringLiteral("cycleTrimSide"));
    add(seqM, tr("Extend Edit"), QKeySequence("E"), [this] { extendEdit(); })->setObjectName(QStringLiteral("extendEdit"));
    add(seqM, tr("Trim Backward"), QKeySequence("Ctrl+Left"), [this] { trimSelectedEdit(-1); })->setObjectName(QStringLiteral("trimBackward"));
    add(seqM, tr("Trim Forward"), QKeySequence("Ctrl+Right"), [this] { trimSelectedEdit(1); })->setObjectName(QStringLiteral("trimForward"));
    add(seqM, tr("Trim Backward Five Frames"), QKeySequence("Ctrl+Shift+Left"), [this] { trimSelectedEdit(-5); })
        ->setObjectName(QStringLiteral("trimBackward5"));
    add(seqM, tr("Trim Forward Five Frames"), QKeySequence("Ctrl+Shift+Right"), [this] { trimSelectedEdit(5); })
        ->setObjectName(QStringLiteral("trimForward5"));
    add(seqM, tr("End Trim Mode"), QKeySequence(Qt::Key_Escape), [this] {
        if (!stopTrimShuttle(false)) endTrimMode();  // Esc while trimming dynamically puts the edit back first
    })->setObjectName(QStringLiteral("endTrim"));
    add(seqM, tr("Ripple Trim Previous Edit to Playhead"), QKeySequence(Qt::Key_Q), [this] { rippleTrimToPlayhead(true); })
        ->setObjectName(QStringLiteral("rippleTrimPrevious"));
    add(seqM, tr("Ripple Trim Next Edit to Playhead"), QKeySequence(Qt::Key_W), [this] { rippleTrimToPlayhead(false); })
        ->setObjectName(QStringLiteral("rippleTrimNext"));
    add(seqM, tr("Select Clips After Playhead"), QKeySequence(Qt::Key_A), [this] { selectForward(true); })
        ->setObjectName(QStringLiteral("selectForward"));
    add(seqM, tr("Select Clips After Playhead on Target Track"), QKeySequence("Shift+A"), [this] { selectForward(false); })
        ->setObjectName(QStringLiteral("selectForwardTrack"));
    add(seqM, tr("Add &Marker"), QKeySequence(Qt::Key_M), [this] { addMarker(); });
    add(seqM, tr("Add C&hapter Marker"), QKeySequence("Alt+M"), [this] { addChapterMarker(); })->setObjectName(QStringLiteral("addChapter"));
    add(seqM, tr("Add C&lip Marker"), QKeySequence("Shift+Alt+M"), [this] { addClipMarker(); })->setObjectName(QStringLiteral("addClipMarker"));
    add(seqM, tr("Copy Chapters for YouTube"), QKeySequence(), [this] { copyYoutubeChapters(); })->setObjectName(QStringLiteral("copyChapters"));
    add(seqM, tr("Suggest Chapters…"), QKeySequence(), [this] { suggestChapterMarkers(); })->setObjectName(QStringLiteral("suggestChapters"));
    add(seqM, tr("Compare with Sequence…"), QKeySequence(), [this] {
        // Another version of the cut, compared with this one (the older one picked from the project's sequences).
        const Sequence* now = state_->sequence();
        if (!now) return;
        QStringList names;
        std::vector<Id> ids;
        for (const Sequence& sq : state_->project().sequences)
            if (sq.id != now->id && !sq.multicam) {
                names << QString::fromStdString(sq.name);
                ids.push_back(sq.id);
            }
        if (ids.empty()) {
            state_->message(tr("Duplicate the sequence before changing it, then compare the two versions"), 6000);
            return;
        }
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Compare with Sequence"), tr("Compare %1 with the earlier version:").arg(QString::fromStdString(now->name)),
                                                   names, 0, false, &ok);
        if (ok) compareWith(ids[size_t(names.indexOf(pick))]);
    })->setObjectName(QStringLiteral("compareSequences"));
    add(seqM, tr("Export Markers…"), QKeySequence(), [this] {
        QString filter;
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Markers"), QString(),
                                                          tr("Marker list (*.csv);;Avid locators (*.txt);;Resolve marker EDL (*.edl);;PDF with pictures (*.pdf)"), &filter);
        if (path.isEmpty()) return;
        QString file = path;
        if (QFileInfo(file).suffix().isEmpty())
            file += filter.contains("*.txt") ? ".txt" : filter.contains("*.edl") ? ".edl" : filter.contains("*.pdf") ? ".pdf" : ".csv";
        exportMarkers(file);
    })->setObjectName(QStringLiteral("exportMarkers"));
    add(seqM, tr("Import Markers…"), QKeySequence(), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Markers"), QString(), tr("Marker lists and review notes (*.csv *.txt *.tsv *.json)"));
        if (!path.isEmpty()) importMarkers(path);
    })->setObjectName(QStringLiteral("importMarkers"));
    add(seqM, tr("&Quality Check…"), QKeySequence(), [this] {
        if (!state_->sequence()) return;
        QualityCheckDialog dlg(state_, this);
        dlg.exec();
    })->setObjectName(QStringLiteral("qualityCheck"));
    add(seqM, tr("Analyse HDR &Light Levels…"), QKeySequence(), [this] { analyseHdrLightLevels(); })->setObjectName(QStringLiteral("analyseHdrLightLevels"));
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
        const bool trackAuto = appSettings().value("timeline/trackAutomation", false).toBool();
        timeline_->setShowTrackAutomation(trackAuto);
        QAction* a = add(seqM, tr("Show &Track Automation"), QKeySequence(), [this](bool on) {
            timeline_->setShowTrackAutomation(on);
            appSettings().setValue("timeline/trackAutomation", on);
        });
        a->setCheckable(true);
        a->setChecked(trackAuto);
        a->setObjectName(QStringLiteral("showTrackAutomation"));
        a->setToolTip(tr("Draw each audio track's volume automation across its row: drag the line or a point, Ctrl/Cmd-click to add a "
                         "point, Alt-click a point to delete it"));
    }
    {
        const bool durations = appSettings().value("timeline/clipDurations", false).toBool();
        timeline_->setShowClipDurations(durations);
        QAction* a = add(seqM, tr("Show Clip &Durations"), QKeySequence(), [this](bool on) {
            timeline_->setShowClipDurations(on);
            appSettings().setValue("timeline/clipDurations", on);
        });
        a->setCheckable(true);
        a->setChecked(durations);
        a->setObjectName(QStringLiteral("showClipDurations"));
    }
    {
        const bool normalize = appSettings().value("timeline/normalizeWaveforms", false).toBool();
        timeline_->setNormalizeWaveforms(normalize);
        QAction* a = add(seqM, tr("Normalise Waveforms"), QKeySequence(), [this](bool on) {
            timeline_->setNormalizeWaveforms(on);
            appSettings().setValue("timeline/normalizeWaveforms", on);
        });
        a->setCheckable(true);
        a->setChecked(normalize);
        a->setObjectName(QStringLiteral("normalizeWaveforms"));
        a->setToolTip(tr("Draw each audio clip's waveform to the full height from its own loudest point (quiet sound becomes "
                         "readable; the clip's level no longer scales it)"));
    }
    {
        const bool dups = appSettings().value("timeline/duplicateFrames", false).toBool();
        timeline_->setShowDuplicateFrames(dups);
        QAction* a = add(seqM, tr("Show &Duplicate Frame Markers"), QKeySequence(), [this](bool on) {
            timeline_->setShowDuplicateFrames(on);
            appSettings().setValue("timeline/duplicateFrames", on);
        });
        a->setCheckable(true);
        a->setChecked(dups);
        a->setObjectName(QStringLiteral("showDuplicateFrames"));
        a->setToolTip(tr("Mark video frames used more than once in the sequence with a stripe, one colour per file"));
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
    trimView_ = add(play, tr("Two-Up Trim View"), QKeySequence(), [](bool on) { appSettings().setValue("playback/twoUpTrim", on); });
    trimView_->setObjectName(QStringLiteral("twoUpTrim"));
    trimView_->setCheckable(true);
    trimView_->setChecked(appSettings().value("playback/twoUpTrim", true).toBool());
    trimView_->setToolTip(tr("While trimming, rolling, slipping or sliding, show the frames either side of the edit side by side"));
    // Video output: the Program picture alone, full screen here or on another display.
    QAction* fullScreen = add(play, tr("Full Screen Program"), QKeySequence("Ctrl+Shift+F"), [this] {
        if (cleanFeed_ && cleanFeed_->isVisible()) hideCleanFeed();
        else showCleanFeed(-1);
    });
    fullScreen->setObjectName(QStringLiteral("fullScreenProgram"));
    fullScreen->setToolTip(tr("The Program picture alone, full screen (Esc to leave)"));
    QMenu* output = play->addMenu(tr("Video Output"));
    output->setObjectName(QStringLiteral("videoOutputMenu"));
    connect(output, &QMenu::aboutToShow, this, [this, output] {
        output->clear();
        QAction* off = output->addAction(tr("Off"), this, [this] { hideCleanFeed(); });
        off->setCheckable(true);
        off->setChecked(!cleanFeed_ || !cleanFeed_->isVisible());
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const QRect g = screens[i]->geometry();
            QAction* a = output->addAction(tr("Screen %1: %2 (%3 × %4)").arg(i + 1).arg(screens[i]->name()).arg(g.width()).arg(g.height()),
                                           this, [this, i] { showCleanFeed(i); });
            a->setCheckable(true);
            a->setChecked(cleanFeed_ && cleanFeed_->isVisible() && cleanFeed_->screen() == screens[i]);
        }
    });
    // How ambisonic sound (core/Ambisonics.h) is heard: binaurally on headphones, or as stereo on speakers.
    {
        QMenu* ambiM = play->addMenu(tr("Ambisonic Monitoring"));
        ambiM->setObjectName(QStringLiteral("ambisonicMonitorMenu"));
        auto* group = new QActionGroup(ambiM);
        const bool binaural = appSettings().value("playback/ambisonicBinaural", true).toBool();
        program_->setAmbisonicBinaural(binaural);
        for (const bool b : {true, false}) {
            QAction* a = ambiM->addAction(b ? tr("Binaural (Headphones)") : tr("Stereo (Speakers)"), this, [this, b] {
                program_->setAmbisonicBinaural(b);
                appSettings().setValue("playback/ambisonicBinaural", b);
            });
            a->setObjectName(b ? QStringLiteral("ambisonicBinaural") : QStringLiteral("ambisonicStereo"));
            a->setCheckable(true);
            a->setChecked(b == binaural);
            group->addAction(a);
        }
    }
    // How the Program monitor shows a stereo 3D sequence's two eyes (render/Stereo.h).
    QMenu* stereoM = play->addMenu(tr("Stereo 3D View"));
    stereoM->setObjectName(QStringLiteral("stereoViewMenu"));
    stereoM->setToolTip(tr("For stereoscopic 3D sequences: one eye, red-cyan anaglyph, both eyes, or where they differ"));
    {
        auto* group = new QActionGroup(stereoM);
        const QString saved = appSettings().value("playback/stereoView", "left").toString();
        StereoView initial = StereoView::Left;
        stereoViewFromName(saved.toStdString(), initial);
        program_->setStereoView(initial);
        const std::pair<StereoView, QString> views[] = {
            {StereoView::Left, tr("Left Eye")},
            {StereoView::Right, tr("Right Eye")},
            {StereoView::Anaglyph, tr("Anaglyph (Red-Cyan)")},
            {StereoView::SideBySideHalf, tr("Side by Side")},
            {StereoView::TopBottomHalf, tr("Top and Bottom")},
            {StereoView::Difference, tr("Difference")},
        };
        for (const auto& [view, label] : views) {
            QAction* a = stereoM->addAction(label, this, [this, view = view] {
                program_->setStereoView(view);
                appSettings().setValue("playback/stereoView", QString::fromStdString(stereoViewName(view)));
            });
            a->setObjectName(QStringLiteral("stereoView_") + QString::fromStdString(stereoViewName(view)));
            a->setCheckable(true);
            a->setChecked(view == initial);
            group->addAction(a);
        }
    }
    play->addSeparator();
    add(play, tr("&Play / Pause"), QKeySequence(Qt::Key_Space), [this] { activeController()->togglePlay(); });
    // In Trim mode J, K and L trim the selected edit as it plays (dynamic trimming).
    add(play, tr("Shuttle &Reverse"), QKeySequence(Qt::Key_J), [this] {
        if (!shuttleTrim(-1)) activeController()->shuttle(-1);
    })->setObjectName(QStringLiteral("shuttleReverse"));
    add(play, tr("&Stop"), QKeySequence(Qt::Key_K), [this] {
        if (!stopTrimShuttle()) activeController()->shuttle(0);
    })->setObjectName(QStringLiteral("shuttleStop"));
    add(play, tr("Shuttle &Forward"), QKeySequence(Qt::Key_L), [this] {
        if (!shuttleTrim(1)) activeController()->shuttle(1);
    })->setObjectName(QStringLiteral("shuttleForward"));
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
    // AI agents on the open project (MCP over the agent link).
    agentLinkAction_ = add(toolsM, tr("Let AI Agents Edit This Project"), QKeySequence(), [this] { setAgentLink(agentLinkAction_->isChecked()); });
    agentLinkAction_->setObjectName(QStringLiteral("agentLink"));
    agentLinkAction_->setCheckable(true);
    agentLinkAction_->setToolTip(tr("Claude and other MCP clients edit the project open here, each change one undo step"));
    add(toolsM, tr("Agent Link…"), QKeySequence(), [this] { agentLinkDialog(); })->setObjectName(QStringLiteral("agentLinkDialog"));
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
    workspaceMenu_ = windowMenu_->addMenu(tr("&Workspaces"));
    workspaceGroup_ = new QActionGroup(this);
    for (int i = 0; i < builtInWorkspaces().size(); ++i) {
        const QString n = builtInWorkspaces()[i];
        QAction* a = add(workspaceMenu_, n, QKeySequence(QStringLiteral("Alt+Shift+%1").arg(i + 1)), [this, n] { applyWorkspace(n); });
        a->setObjectName(QStringLiteral("workspace") + n);
        a->setData(n);
        a->setCheckable(true);
        workspaceGroup_->addAction(a);
        workspaceActions_.push_back(a);
    }
    workspaceCustomEnd_ = workspaceMenu_->addSeparator();
    add(workspaceMenu_, tr("&Save as New Workspace…"), QKeySequence(), [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Save Workspace"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !saveWorkspace(n)) state_->message(tr("Choose a name other than a built-in workspace's"), 4000);
    })->setObjectName(QStringLiteral("saveWorkspace"));
    deleteWorkspaceMenu_ = workspaceMenu_->addMenu(tr("&Delete Workspace"));
    add(windowMenu_, tr("&Reset Workspace to Saved Layout"), QKeySequence(), [this] { resetLayout(); })->setObjectName(QStringLiteral("resetWorkspace"));
    workspaceBar_ = new QTabBar(this);
    workspaceBar_->setObjectName(QStringLiteral("workspaceBar"));
    workspaceBar_->setDrawBase(false);
    workspaceBar_->setExpanding(false);
    workspaceBar_->setToolTip(tr("Workspaces: the panels laid out for a task (Alt+Shift+1 to 6)"));
    statusBar()->insertPermanentWidget(0, workspaceBar_);
    connect(workspaceBar_, &QTabBar::currentChanged, this, [this](int i) {
        if (!syncingWorkspace_ && i >= 0) applyWorkspace(workspaceBar_->tabText(i));
    });
    syncWorkspaceUi();
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
    timeline_->setClipContextActions({cut, copy, pasteA, dup, sep1, del, rdel, sep2, enable, link, unlink, speed, trans, nest, roleMenu_->menuAction(), sep3});
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
    auto* roomTone = new QAction(tr("Fill Gap with Room Tone"), this);
    roomTone->setObjectName(QStringLiteral("fillRoomTone"));
    connect(roomTone, &QAction::triggered, this, [this] {
        auto t = timeline_->contextTrack();
        if (!t || t->kind != TrackKind::Audio) {
            state_->message(tr("Room tone fills a gap on an audio track"));
            return;
        }
        fillRoomTone(*t, timeline_->contextFrame());
    });
    timeline_->setEmptyContextActions({pasteHere, closeGap, roomTone});
}

void MainWindow::updateActions() {
    undo_->setEnabled(state_->canUndo());
    redo_->setEnabled(state_->canRedo());
    undo_->setText(state_->canUndo() ? tr("&Undo %1").arg(state_->undoText()) : tr("&Undo"));
    redo_->setText(state_->canRedo() ? tr("&Redo %1").arg(state_->redoText()) : tr("&Redo"));
}

void MainWindow::updateTitle() {
    QString name = state_->filePath().isEmpty() ? tr("Untitled") : QFileInfo(state_->filePath()).completeBaseName();
    if (state_->readOnly())
        name += state_->lockHolder().isEmpty() ? tr(" (Read-Only)") : tr(" (Read-Only: %1 is editing)").arg(state_->lockHolder());
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
    if (state_->readOnly()) {
        // Changes made before someone else took the project over: only a copy can keep them.
        const auto r = QMessageBox::warning(
            this, tr("Montage"),
            tr("Your changes to this project are not saved, and %1 is editing it now. Save them as a copy?")
                .arg(state_->lockHolder().isEmpty() ? tr("someone else") : state_->lockHolder()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel) return false;
        if (r == QMessageBox::Save) return saveAs();
        return true;
    }
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

bool MainWindow::openProject(const QString& path) { return openProjectAs(path, OpenMode::Ask); }

bool MainWindow::openProjectAs(const QString& path, OpenMode mode) {
    QString err;
    program_->pause();
    EditorState::Access access = mode == OpenMode::ReadOnly ? EditorState::Access::ReadOnly
                                 : mode == OpenMode::Edit   ? EditorState::Access::Edit
                                                            : EditorState::Access::Auto;
    if (mode == OpenMode::Ask) {
        const LockStatus st = projectLockStatus(path.toStdString());
        const QString who = QString::fromStdString(st.owner.describe()), file = QFileInfo(path).completeBaseName();
        const QString since = st.owner.since.isValid() ? QLocale().toString(st.owner.since.toLocalTime(), QLocale::ShortFormat) : tr("a while");
        if (st.state == LockState::Theirs) {
            QMessageBox box(QMessageBox::Information, tr("Project in Use"),
                            tr("%1 is editing “%2” (since %3).\n\nOpen it read-only? You can watch, play and export it; it follows their saves, "
                               "and once they close it File › Edit Project lets you edit it.")
                                .arg(who, file, since),
                            QMessageBox::Cancel, this);
            QPushButton* ro = box.addButton(tr("Open Read-Only"), QMessageBox::AcceptRole);
            box.exec();
            if (box.clickedButton() != ro) return false;
            access = EditorState::Access::ReadOnly;
        } else if (st.state == LockState::Stale) {
            QMessageBox box(QMessageBox::Question, tr("Project Left Open"),
                            tr("“%2” was left open by %1, who has not been heard from since %3 (Montage closed unexpectedly, or the "
                               "computer went to sleep).\n\nTake it over and edit it?")
                                .arg(who, file, st.owner.heartbeat.isValid() ? QLocale().toString(st.owner.heartbeat.toLocalTime(), QLocale::ShortFormat) : since),
                            QMessageBox::Cancel, this);
            QPushButton* take = box.addButton(tr("Take Over"), QMessageBox::AcceptRole);
            QPushButton* ro = box.addButton(tr("Open Read-Only"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == take) access = EditorState::Access::Edit;
            else if (box.clickedButton() == ro) access = EditorState::Access::ReadOnly;
            else return false;
        }
    }
    if (!state_->open(path, &err, access)) {
        QMessageBox::warning(this, tr("Open Project"), err);
        return false;
    }
    addRecent(path);
    appSettings().setValue("lastProjectDir", QFileInfo(path).absolutePath());
    timeline_->zoomToFit();
    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    if (!offlineMedia(state_->project()).empty()) showLinkMedia();
    return true;
}

LinkMediaDialog* MainWindow::showLinkMedia() {
    if (offlineMedia(state_->project()).empty()) {
        state_->message(tr("No media is offline"), 3000);
        return nullptr;
    }
    if (!linkMedia_) {
        linkMedia_ = new LinkMediaDialog(state_, this);
        linkMedia_->setAttribute(Qt::WA_DeleteOnClose);
    } else {
        linkMedia_->refresh();
    }
    linkMedia_->show();
    linkMedia_->raise();
    return linkMedia_;
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

bool MainWindow::takeEditInteractive() {
    bool discard = false;
    if (state_->isModified()) {
        if (QMessageBox::question(this, tr("Edit Project"),
                                  tr("Your changes here are not saved. Discard them and edit the project as it was last saved? (Save As keeps "
                                     "them as a copy instead.)")) != QMessageBox::Yes)
            return false;
        discard = true;
    }
    QString err;
    if (!state_->takeEdit(&err, discard)) {
        if (!err.isEmpty()) QMessageBox::information(this, tr("Edit Project"), err);
        return false;
    }
    return true;
}

bool MainWindow::openProduction(const QString& folder, bool create, const QString& name) {
    if (!isProduction(folder.toStdString())) {
        std::string err;
        if (!create || !createProduction(folder.toStdString(), name.toStdString(), &err)) {
            if (!err.empty()) QMessageBox::warning(this, tr("Production"), QString::fromStdString(err));
            return false;
        }
    }
    appSettings().setValue("lastProductionDir", QDir(folder).absolutePath());
    production_->setFolder(folder);
    productionDock_->show();
    productionDock_->raise();
    return true;
}

void MainWindow::newProjectInProduction(const QString& folder) {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Project in Production"), tr("Name:"), QLineEdit::Normal, tr("Reel 1"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const QString path = QDir(folder).filePath(name + ".montage");
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("New Project"), tr("%1 already exists").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!maybeSave()) return;
    program_->pause();
    state_->newProject();
    QString err;
    if (!state_->save(path, &err)) {
        QMessageBox::warning(this, tr("New Project"), err);
        return;
    }
    addRecent(path);
    production_->refresh();
}

std::vector<Id> MainWindow::importSequencesFrom(const QString& project, const QStringList& names, QString* error) {
    Project from;
    std::string err;
    if (!loadProject(project.toStdString(), from, &err)) {
        if (error) *error = QString::fromStdString(err);
        return {};
    }
    checkStereoMedia(from);
    std::vector<Id> wanted;
    for (const Sequence& s : from.sequences)
        if (names.isEmpty() || names.contains(QString::fromStdString(s.name))) wanted.push_back(s.id);
    if (wanted.empty()) {
        if (error) *error = tr("No such sequence in %1").arg(QFileInfo(project).fileName());
        return {};
    }
    std::vector<Id> made;
    state_->edit(tr("Import from Project"), [&](Project& p, Sequence&) {
        made = importFromProject(p, from, wanted);
        return !made.empty();
    });
    if (made.empty()) {
        if (error && error->isEmpty()) *error = state_->readOnly() ? tr("This project is read-only") : tr("Nothing was imported");
        return {};
    }
    state_->setActiveSequence(made.front());
    statusBar()->showMessage(tr("Imported %n sequence(s) from %1", "", int(made.size())).arg(QFileInfo(project).completeBaseName()), 5000);
    return made;
}

void MainWindow::importFromProjectDialog(QString project) {
    if (project.isEmpty())
        project = QFileDialog::getOpenFileName(this, tr("Import from Project"), appSettings().value("lastProjectDir").toString(),
                                               tr("Montage projects (*.montage)"));
    if (project.isEmpty()) return;
    Project from;
    std::string err;
    if (!loadProject(project.toStdString(), from, &err)) {
        QMessageBox::warning(this, tr("Import from Project"), QString::fromStdString(err));
        return;
    }
    checkStereoMedia(from);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Import from %1").arg(QFileInfo(project).completeBaseName()));
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(tr("Sequences to bring in (with the sequences they nest and their media, linked where it is):"), &dlg));
    auto* list = new QListWidget(&dlg);
    list->setObjectName(QStringLiteral("importSequences"));
    for (const Sequence& s : from.sequences) {
        auto* item = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(QString::fromStdString(s.name),
                                                                         QString::fromStdString(formatTimecode(s.duration(), s.fps))),
                                         list);
        item->setData(Qt::UserRole, QString::fromStdString(s.name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    v->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList names;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) names << list->item(i)->data(Qt::UserRole).toString();
    if (names.isEmpty()) return;
    QString error;
    if (importSequencesFrom(project, names, &error).empty()) QMessageBox::warning(this, tr("Import from Project"), error);
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
    backgroundRender_ = false;
    if (backgroundCancel_) *backgroundCancel_ = true;  // a background render stops at its next frame
    program_->pause();
    source_->pause();
    recovery_->endSession();  // a clean exit: nothing to recover next time
    QSettings s = appSettings();
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState(1));
    s.setValue("window/workspace", workspace_);
    e->accept();
}

// ---------------------------------------------------------------------------
// Editing commands

void MainWindow::copySelection(bool cut) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    if (state_->selectedClips().empty()) {
        // A selected gap (Resolve 21.1): its length is kept to paste as empty space; cutting closes it.
        if (const auto& gap = timeline_->selectedGap(); gap && gap->to > gap->from) {
            gapClipboard_ = gap->to - gap->from;
            gapTrack_ = gap->track;
            gapCopiedLast_ = true;
            const QString length = QString::fromStdString(formatTimecode(gapClipboard_, s->fps));
            if (cut) deleteSelection(false);
            statusBar()->showMessage(cut ? tr("Gap cut (%1)").arg(length) : tr("Gap copied (%1)").arg(length), 2000);
        }
        return;
    }
    gapCopiedLast_ = false;
    clipboard_ = edit::copyClips(*s, state_->selectedClips());
    if (cut) deleteSelection(false);
    statusBar()->showMessage(tr("%n clip(s) copied", "", int(clipboard_.size())), 2000);
}

void MainWindow::paste(bool insertMode) {
    if (gapCopiedLast_ && gapClipboard_ > 0) {
        // A gap pastes as empty space at the playhead, on the selected gap's track or the one it came from.
        const auto& gap = timeline_->selectedGap();
        const TrackRef track = gap ? gap->track : gapTrack_;
        const FrameTime at = state_->playhead(), length = gapClipboard_;
        if (state_->apply(tr("Paste Gap"), [track, at, length](Project& p, Sequence& s) { return edit::insertGap(p, s, track, at, length); }))
            timeline_->clearGap();
        return;
    }
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
    if (sel.empty()) {
        // A selected gap closes, rippling what follows.
        if (const auto& gap = timeline_->selectedGap()) {
            const TrackRef track = gap->track;
            const FrameTime at = gap->from;
            state_->apply(tr("Ripple Delete Gap"), [track, at](Project& p, Sequence& s) { return edit::closeGap(p, s, track, at); });
            timeline_->clearGap();
        }
        return;
    }
    state_->apply(ripple ? tr("Ripple Delete") : tr("Delete"), [sel, ripple](Project& p, Sequence& s) {
        return edit::removeClips(p, s, sel, ripple);
    });
}

void MainWindow::childEvent(QChildEvent* e) {
    QMainWindow::childEvent(e);
    if (e->type() == QEvent::ChildPolished)
        if (auto* bar = qobject_cast<QTabBar*>(e->child())) {
            bar->setElideMode(Qt::ElideNone);
            bar->setUsesScrollButtons(true);
        }
}

void MainWindow::clearOrRestoreSolo() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    const Id seq = s->id;
    if (const std::vector<Id> soloed = edit::soloedTracks(*s); !soloed.empty()) {
        if (state_->apply(tr("Clear Solo"), [](Project&, Sequence& sq) { return edit::setSoloedTracks(sq, {}); })) {
            soloMemory_[seq] = soloed;
            state_->message(tr("Solo cleared: Clear Solo again brings it back"), 4000);
        }
        return;
    }
    auto it = soloMemory_.find(seq);
    if (it == soloMemory_.end()) {
        state_->message(tr("No track is soloed"), 3000);
        return;
    }
    const std::vector<Id> tracks = it->second;
    if (state_->apply(tr("Restore Solo"), [tracks](Project&, Sequence& sq) { return edit::setSoloedTracks(sq, tracks); }))
        soloMemory_.erase(it);
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

void MainWindow::addTransitionsToSelection(std::optional<TrackKind> only) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    std::vector<Id> clips;
    for (Id id : state_->selectedClips())
        if (const auto loc = edit::locate(*s, id); loc && (!only || loc->track.kind == *only)) clips.push_back(id);
    if (clips.empty()) {
        state_->message(tr("Select the clips to give transitions"));
        return;
    }
    const FrameTime dur = FrameTime(std::llround(s->fpsValue()));
    state_->apply(tr("Apply Default Transitions"), [clips, dur](Project& p, Sequence& sq) {
        return edit::addTransitionsToClips(p, sq, clips, "cross_dissolve", "crossfade", dur);
    });
}

void MainWindow::addDefaultTransition(bool audio) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // Several clips of the kind selected: every edit point of each (Premiere and Final Cut do the same).
    int selected = 0;
    for (Id id : state_->selectedClips())
        if (const auto loc = edit::locate(*s, id); loc && loc->track.kind == (audio ? TrackKind::Audio : TrackKind::Video)) ++selected;
    if (selected > 1) {
        addTransitionsToSelection(audio ? TrackKind::Audio : TrackKind::Video);
        return;
    }
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
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Speed / Duration"));
    auto* form = new QFormLayout(&dlg);
    auto* speed = new QDoubleSpinBox(&dlg);
    speed->setRange(1, 10000);
    speed->setDecimals(1);
    speed->setSuffix(QStringLiteral(" %"));
    speed->setValue(c->speed * 100);
    form->addRow(tr("Speed:"), speed);
    auto* pitch = new QCheckBox(tr("Maintain Audio Pitch"), &dlg);
    pitch->setToolTip(tr("Sound played faster or slower keeps its pitch instead of rising or falling"));
    pitch->setChecked(c->timing.p("maintain_pitch", 0) > 0.5);
    form->addRow(QString(), pitch);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() == QDialog::Accepted) setSelectionSpeed(speed->value() / 100.0, pitch->isChecked());
}

bool MainWindow::animateSelectionToAudio(Id effect, const std::string& param, int track, int band, double low, double high) {
    const auto sel = state_->selectedClips();
    if (sel.empty()) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = state_->apply(tr("Animate to Audio"), [sel, effect, param, track, band, low, high](Project& p, Sequence& s) {
        edit::Result last = edit::Result::fail("Select a picture clip");
        bool any = false;
        for (Id id : sel) {
            bool video = false;
            for (const Track& t : s.videoTracks)
                for (const Clip& c : t.clips) video = video || c.id == id;
            if (!video) continue;
            last = edit::animateToAudio(p, s, id, effect, param, track, AudioBand(std::clamp(band, 0, 3)), low, high);
            any = any || last.ok;
        }
        return any ? edit::Result{} : last;
    });
    QApplication::restoreOverrideCursor();
    return ok;
}

bool MainWindow::alignSelection(const std::string& where) {
    Align a;
    if (!parseAlign(where, a)) return false;
    const auto sel = state_->selectedClips();
    if (sel.empty()) return false;
    const FrameTime playhead = state_->playhead();
    return state_->apply(tr("Align in Frame"), [sel, a, playhead](Project& p, Sequence& s) {
        edit::Result last = edit::Result::fail(tr("Select picture clips").toStdString());
        bool any = false;
        for (Id id : sel) {
            const Clip* c = edit::clipById(s, id);
            if (!c) continue;
            // At the playhead when it is over the clip, else at its start.
            const FrameTime t = c->contains(playhead) ? playhead : c->start;
            const edit::Result r = edit::alignClip(p, s, id, t, a);
            if (r.ok) any = true;
            else last = r;
        }
        return any ? edit::Result{} : last;
    });
}

bool MainWindow::setSelectionChannels(const std::vector<int>& channels) {
    const auto sel = state_->selectedClips();
    if (sel.empty()) return false;
    return state_->apply(tr("Audio Channels"), [sel, channels](Project& p, Sequence& s) {
        edit::Result last = edit::Result::fail(tr("Select clips with sound").toStdString());
        bool any = false;
        std::set<Id> done;  // each sound clip once (a picture and its sound are both selected)
        for (Id id : sel) {
            if (done.count(id)) continue;
            for (Id l : edit::linkedClips(s, id)) done.insert(l);
            const edit::Result r = edit::setClipChannels(p, s, id, channels);
            if (r.ok) any = true;
            else if (!r.error.empty() || !last.ok) last = r;
        }
        return any ? edit::Result{} : last;
    });
}

bool MainWindow::splitSelectionChannels(bool pairs) {
    const auto sel = state_->selectedClips();
    if (sel.empty()) return false;
    return state_->apply(pairs ? tr("Split into Stereo Pairs") : tr("Split into Mono Clips"), [sel, pairs](Project& p, Sequence& s) {
        edit::Result last = edit::Result::fail(tr("Select clips with sound").toStdString()), all;
        bool any = false;
        std::set<Id> done;
        for (Id id : sel) {
            if (done.count(id) || !edit::clipById(s, id)) continue;
            for (Id l : edit::linkedClips(s, id)) done.insert(l);
            const edit::Result r = edit::splitAudioChannels(p, s, id, pairs);
            if (r.ok) {
                any = true;
                all.created.insert(all.created.end(), r.created.begin(), r.created.end());
            } else {
                last = r;
            }
        }
        return any ? all : last;
    });
}

void MainWindow::audioChannelsDialog() {
    const Clip* c = state_->primaryClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) return;
    // The sound clip: this one, or the sound linked to a picture.
    const Clip* sound = nullptr;
    for (Id id : edit::linkedClips(*s, c->id))
        if (const auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Audio && !sound) sound = edit::clipById(*s, id);
    const MediaItem* m = sound ? state_->project().findMedia(sound->mediaId) : nullptr;
    if (!m || sourceChannelCount(*m) == 0) {
        state_->message(tr("Select a clip with sound"));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("audioChannelsDialog"));
    dlg.setWindowTitle(tr("Audio Channels"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("Channels of %1 the clip plays (none ticked: the stereo mix).").arg(QString::fromStdString(m->name)), &dlg));
    const std::vector<std::string> names = sourceChannelNames(*m);
    std::vector<QCheckBox*> boxes;
    for (size_t i = 0; i < names.size(); ++i) {
        auto* box = new QCheckBox(tr("%1: %2").arg(i + 1).arg(QString::fromStdString(names[i])), &dlg);
        box->setObjectName(QStringLiteral("channel%1").arg(i + 1));
        box->setChecked(std::find(sound->channels.begin(), sound->channels.end(), int(i)) != sound->channels.end());
        layout->addWidget(box);
        boxes.push_back(box);
    }
    layout->addWidget(new QLabel(tr("One channel plays in the centre, two as left and right."), &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<int> chosen;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i]->isChecked()) chosen.push_back(int(i));
    setSelectionChannels(chosen);
}

void MainWindow::animateToAudioDialog() {
    const Clip* c = state_->primaryClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Animate to Audio"));
    auto* form = new QFormLayout(&dlg);
    // What moves: the Transform's settings, then the clip's other effects' number settings.
    auto* what = new QComboBox(&dlg);
    auto addParams = [&](const Effect& e, const QString& prefix, Id id) {
        if (const EffectInfo* info = findEffectInfo(e.type))
            for (const ParamInfo& pi : info->params)
                if (pi.kind == ParamKind::Number || pi.kind == ParamKind::Percent || pi.kind == ParamKind::Angle)
                    what->addItem(prefix + QString::fromStdString(pi.label), QVariantList{QVariant::fromValue<qulonglong>(id), QString::fromStdString(pi.name)});
    };
    addParams(c->motion.empty() ? makeEffect("transform", 0) : c->motion, tr("Transform › "), 0);
    if (c->isGenerator()) addParams(c->generator, QString::fromStdString(findEffectInfo(c->generator.type) ? findEffectInfo(c->generator.type)->displayName : c->generator.type) + " › ", c->generator.id);
    for (const Effect& e : c->effects)
        addParams(e, QString::fromStdString(findEffectInfo(e.type) ? findEffectInfo(e.type)->displayName : e.type) + " › ", e.id);
    what->setCurrentIndex(std::max(0, what->findText(tr("Transform › Scale"), Qt::MatchStartsWith)));
    form->addRow(tr("Setting:"), what);
    auto* track = new QSpinBox(&dlg);
    track->setRange(0, std::max(1, int(s->audioTracks.size())));
    track->setValue(1);
    track->setSpecialValueText(tr("All tracks"));
    track->setPrefix(tr("A"));
    form->addRow(tr("Audio track:"), track);
    auto* band = new QComboBox(&dlg);
    band->addItems({tr("All frequencies"), tr("Lows (beats, bass)"), tr("Mids (voices)"), tr("Highs (hi-hats, sibilance)")});
    form->addRow(tr("Listen to:"), band);
    auto* low = new QDoubleSpinBox(&dlg);
    auto* high = new QDoubleSpinBox(&dlg);
    for (auto* b : {low, high}) {
        b->setRange(-100000, 100000);
        b->setDecimals(2);
    }
    low->setValue(100);
    high->setValue(115);
    form->addRow(tr("When quiet:"), low);
    form->addRow(tr("At the loudest:"), high);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted || what->currentIndex() < 0) return;
    const QVariantList target = what->currentData().toList();
    animateSelectionToAudio(Id(target.at(0).toULongLong()), target.at(1).toString().toStdString(), track->value(), band->currentIndex(),
                            low->value(), high->value());
}

void MainWindow::vfxPullDialog() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    std::vector<Id> clips = state_->selectedClips();
    if (clips.empty()) {
        state_->message(tr("Select the shots to pull on the timeline"), 5000);
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Export VFX Pulls"));
    auto* form = new QFormLayout(&dlg);
    auto* format = new QComboBox(&dlg);
    format->addItem(tr("OpenEXR (half float, linear)"), QStringLiteral("exr"));
    format->addItem(tr("DPX (10-bit, the footage's own colour)"), QStringLiteral("dpx"));
    format->addItem(tr("TIFF (16-bit)"), QStringLiteral("tiff"));
    form->addRow(tr("Format:"), format);
    auto* handles = new QSpinBox(&dlg);
    handles->setRange(0, 100);
    handles->setValue(appSettings().value("vfx/handles", 8).toInt());
    handles->setSuffix(tr(" frames"));
    form->addRow(tr("Handles:"), handles);
    auto* cutIn = new QSpinBox(&dlg);
    cutIn->setRange(0, 100000);
    cutIn->setValue(1001);
    form->addRow(tr("First frame of the cut:"), cutIn);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder for the Pulls"), appSettings().value("vfx/folder").toString());
    if (folder.isEmpty()) return;
    appSettings().setValue("vfx/folder", folder);
    appSettings().setValue("vfx/handles", handles->value());
    VfxPullOptions o;
    o.folder = folder.toStdString();
    o.format = format->currentData().toString().toStdString();
    o.handles = handles->value();
    o.cutIn = cutIn->value();
    QProgressDialog progress(tr("Pulling shots..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    std::atomic<bool> cancel{false};
    connect(&progress, &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    std::vector<VfxShot> shots;
    std::string err;
    const bool ok = exportVfxPulls(state_->project(), *s, clips, o, &shots, [&](double f, FrameTime) {
        progress.setValue(int(f * 1000));
        QApplication::processEvents();
    }, &cancel, &err);
    progress.close();
    if (!ok) QMessageBox::warning(this, tr("Export VFX Pulls"), QString::fromStdString(err));
    else statusBar()->showMessage(tr("Pulled %n shot(s) with a pull list into %1", "", int(shots.size())).arg(QDir::toNativeSeparators(folder)), 8000);
}

CleanFeedWindow* MainWindow::showCleanFeed(int screenIndex) {
    if (!cleanFeed_) {
        cleanFeed_ = new CleanFeedWindow(this);
        connect(program_, &PlaybackController::frameRendered, cleanFeed_, [this](const QImage& img, FrameTime) {
            if (cleanFeed_->isVisible()) cleanFeed_->setFrame(img);
        });
        connect(cleanFeed_, &CleanFeedWindow::closed, this, [this] { statusBar()->showMessage(tr("Video output off"), 2000); });
    }
    const QList<QScreen*> screens = QGuiApplication::screens();
    QScreen* target = screenIndex >= 0 && screenIndex < screens.size() ? screens[screenIndex] : screen();
    if (target) {
        cleanFeed_->setScreen(target);
        cleanFeed_->setGeometry(target->geometry());
    }
    cleanFeed_->showFullScreen();
    cleanFeed_->raise();
    cleanFeed_->activateWindow();
    program_->requestFrame();
    return cleanFeed_;
}

void MainWindow::hideCleanFeed() {
    if (cleanFeed_ && cleanFeed_->isVisible()) cleanFeed_->close();
}

bool MainWindow::setSelectionSpeed(double sp, bool maintainPitch) {
    auto sel = state_->selectedClips();
    if (sel.empty()) return false;
    return state_->apply(tr("Speed / Duration"), [sel, sp, maintainPitch](Project& p, Sequence& s) {
        // One call per link group: setSpeed changes linked partners itself and ripples once.
        bool changed = false;
        std::set<Id> done;
        for (Id id : sel) {
            const Clip* cc = edit::clipById(s, id);
            if (!cc || done.count(id)) continue;
            for (Id l : edit::linkedClips(s, id)) done.insert(l);
            changed = edit::setMaintainPitch(p, s, id, maintainPitch) || changed;
            if (std::fabs(cc->speed - sp) < 1e-9) continue;
            const edit::Result r = edit::setSpeed(p, s, id, sp, true, cc->reverse);
            if (!r.ok) return r;
            changed = true;
        }
        return changed ? edit::Result{} : edit::Result::fail({});  // the same again is no edit
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
    // Through nested sequences and multicam clips to the file underneath (Resolve 21's Match Frame).
    if (const auto match = edit::matchSource(state_->project(), *s, state_->playhead())) {
        openInSource(match->media);
        source_->seek(FrameTime(std::floor(match->frame + 1e-6)));
        return;
    }
    state_->message(tr("No video clip under the playhead"));
}

CompareDialog* MainWindow::compareWith(Id before) {
    if (!state_->sequence() || !state_->project().findSequence(before)) return nullptr;
    auto* dlg = new CompareDialog(state_, before, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    return dlg;
}

QString MainWindow::selectionSummary() const {
    const Sequence* s = state_->sequence();
    if (!s) return {};
    FrameTime first = std::numeric_limits<FrameTime>::max(), last = std::numeric_limits<FrameTime>::min();
    int n = 0;
    for (Id id : state_->selectedClips())
        if (const Clip* c = edit::clipById(*s, id)) {
            first = std::min(first, c->start);
            last = std::max(last, c->end());
            ++n;
        }
    if (!n) return {};
    const QString length = QString::fromStdString(formatTimecode(last - first, s->fps));
    return n == 1 ? tr("1 clip selected · %1").arg(length) : tr("%1 clips selected · %2").arg(n).arg(length);
}

bool MainWindow::reverseMatchFrame() {
    // The Source monitor's frame, found in the sequence: the first use after the playhead (round to the first), so
    // pressing it again steps through every use.
    const Sequence* s = state_->sequence();
    const Id media = state_->sourceMedia();
    if (!s || !media) {
        state_->message(tr("Open a clip in the Source monitor first"));
        return false;
    }
    const auto uses = edit::sourceFrameUses(*s, media, double(source_->position()));
    if (uses.empty()) {
        state_->message(tr("This frame is not used in %1").arg(QString::fromStdString(s->name)));
        return false;
    }
    const FrameTime now = state_->playhead();
    auto it = std::find_if(uses.begin(), uses.end(), [now](const edit::FrameUse& u) { return u.at > now; });
    if (it == uses.end()) it = uses.begin();
    state_->setPlayhead(it->at);
    state_->setSelection({it->clip});
    active_ = Monitor::Program;
    programDock_->raise();
    state_->message(uses.size() > 1 ? tr("Use %1 of %2 of this frame; Reverse Match Frame again for the next")
                                          .arg(int(it - uses.begin()) + 1)
                                          .arg(uses.size())
                                    : tr("Found the frame at %1").arg(QString::fromStdString(formatTimecode(it->at, s->fps))));
    return true;
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

void MainWindow::addChapterMarker() {
    if (active_ == Monitor::Source || !state_->sequence()) return;
    const FrameTime t = state_->playhead();
    int n = 1;
    for (const Marker& m : state_->sequence()->markers) n += m.chapter;
    // A marker already at the playhead becomes a chapter; otherwise a new one is added.
    state_->edit(tr("Add Chapter Marker"), [t, n](Project&, Sequence& s) {
        for (Marker& m : s.markers)
            if (m.t == t) {
                if (m.chapter) return false;
                m.chapter = true;
                return true;
            }
        edit::addMarker(s, Marker{t, 0, "Chapter " + std::to_string(n), "", 0, true});
        return true;
    });
}

bool MainWindow::selectNearestEdit() {
    const Sequence* s = state_->sequence();
    const int vt = state_->targetVideoTrack();
    if (!s || vt < 0 || vt >= int(s->videoTracks.size())) return false;
    const Track& t = s->videoTracks[size_t(vt)];
    const FrameTime at = state_->playhead();
    // Every clip start and end on the target track is an edit point; the nearest wins.
    FrameTime best = -1;
    for (const Clip& c : t.clips)
        for (FrameTime f : {c.start, c.end()})
            if (best < 0 || std::llabs(f - at) < std::llabs(best - at)) best = f;
    if (best < 0) {
        statusBar()->showMessage(tr("No edit on the target video track"), 4000);
        return false;
    }
    TrimEdit e;
    e.track = {TrackKind::Video, vt};
    for (const Clip& c : t.clips) {
        if (c.end() == best) e.outgoing = c.id;
        if (c.start == best) e.incoming = c.id;
    }
    e.side = e.outgoing && e.incoming ? 0 : e.outgoing ? 1 : 2;
    trimEdit_ = e;
    state_->setPlayhead(best);
    showTrimEdit();
    return true;
}

void MainWindow::cycleTrimSide() {
    if (!trimEdit_) return;
    // Roll, then the outgoing side, then the incoming side (sides only where there is a clip).
    for (int i = 0; i < 3; ++i) {
        trimEdit_->side = (trimEdit_->side + 1) % 3;
        const int side = trimEdit_->side;
        if ((side == 0 && trimEdit_->outgoing && trimEdit_->incoming) || (side == 1 && trimEdit_->outgoing) || (side == 2 && trimEdit_->incoming)) break;
    }
    showTrimEdit();
}

bool MainWindow::trimSelectedEdit(FrameTime delta) {
    if (!trimEdit_) {
        statusBar()->showMessage(tr("Select an edit to trim first (Shift+T)"), 4000);
        return false;
    }
    const TrimEdit e = *trimEdit_;
    const bool ok = state_->apply(tr("Trim"), [e, delta](Project& p, Sequence& s) {
        if (e.side == 0) return edit::roll(p, s, e.outgoing, e.incoming, delta);
        if (e.side == 1) return edit::trim(p, s, e.outgoing, edit::Edge::Out, delta, edit::TrimMode::Ripple);
        return edit::trim(p, s, e.incoming, edit::Edge::In, delta, edit::TrimMode::Ripple);
    });
    showTrimEdit();
    return ok;
}

bool MainWindow::extendEdit() {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    const FrameTime target = state_->playhead();
    // In Trim mode: the selected edit, on its side, to the playhead.
    if (trimEdit_) {
        const Clip* out = trimEdit_->outgoing ? edit::clipById(*s, trimEdit_->outgoing) : nullptr;
        const Clip* in = trimEdit_->incoming ? edit::clipById(*s, trimEdit_->incoming) : nullptr;
        if (!out && !in) return false;
        const FrameTime cut = trimEdit_->side == 2 && in ? in->start : out ? out->end() : in->start;
        return target != cut && trimSelectedEdit(target - cut);
    }
    // Else each selected clip's nearest edge, or the clip edge nearest the playhead on the target video track.
    std::vector<Id> clips = state_->selectedClips();
    if (clips.empty()) {
        if (const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()})) {
            FrameTime best = std::numeric_limits<FrameTime>::max();
            Id nearest = 0;
            for (const Clip& c : t->clips)
                for (FrameTime e : {c.start, c.end()})
                    if (std::llabs(e - target) < best) best = std::llabs(e - target), nearest = c.id;
            if (nearest) clips.push_back(nearest);
        }
    }
    if (clips.empty()) {
        state_->message(tr("Select a clip or an edit to extend to the playhead"));
        return false;
    }
    return state_->apply(tr("Extend Edit"), [clips, target](Project& p, Sequence& sq) {
        edit::Result last = edit::Result::fail(""), all;
        bool any = false;
        for (Id id : clips) {
            const edit::Result r = edit::extendEdit(p, sq, id, target);
            if (r.ok) any = true;
            else if (!r.error.empty()) last = r;
        }
        return any ? all : last;
    });
}

bool MainWindow::shuttleTrim(int direction) {
    if (!trimEdit_ || !state_->sequence() || direction == 0) return false;
    if (!trimShuttle_) {
        if (state_->inGesture()) return false;
        activeController()->shuttle(0);
        trimShuttle_.emplace();
        state_->beginGesture(tr("Dynamic Trim"));
        if (!trimShuttleTimer_) {
            trimShuttleTimer_ = new QTimer(this);
            trimShuttleTimer_->setInterval(20);
            connect(trimShuttleTimer_, &QTimer::timeout, this, [this] {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                advanceTrimShuttle(double(now - trimShuttleLast_) / 1000.0);
                trimShuttleLast_ = now;
            });
        }
    }
    // Again the same way: faster (up to 8x); the other way: 1x that way.
    int& v = trimShuttle_->speed;
    v = (v > 0) == (direction > 0) && v != 0 ? std::clamp(v * 2, -8, 8) : direction;
    trimShuttleLast_ = QDateTime::currentMSecsSinceEpoch();
    if (!trimShuttleManual_) trimShuttleTimer_->start();
    statusBar()->showMessage(tr("Dynamic trim at %1x: K keeps it, Esc puts it back").arg(v), 4000);
    return true;
}

void MainWindow::advanceTrimShuttle(double seconds) {
    const Sequence* s = state_->sequence();
    if (!trimShuttle_ || !trimEdit_ || !s || trimShuttle_->speed == 0) return;
    trimShuttle_->played += trimShuttle_->speed * seconds * s->fpsValue();
    const FrameTime want = FrameTime(std::llround(trimShuttle_->played));
    if (want == trimShuttle_->applied) return;
    const TrimEdit e = *trimEdit_;
    auto trim = [e](Project& p, Sequence& sq, FrameTime delta) {
        if (delta == 0) return true;
        if (e.side == 0) return edit::roll(p, sq, e.outgoing, e.incoming, delta).ok;
        if (e.side == 1) return edit::trim(p, sq, e.outgoing, edit::Edge::Out, delta, edit::TrimMode::Ripple).ok;
        return edit::trim(p, sq, e.incoming, edit::Edge::In, delta, edit::TrimMode::Ripple).ok;
    };
    // As far as the media (or the clip) allows: a frame at a time back towards the last trim that worked.
    const FrameTime from = trimShuttle_->applied;
    FrameTime reached = from;
    // How far a trim really moved the edit (edits stop short at a clip's or its media's end).
    auto moved = [e](const Sequence& before, const Sequence& after) -> FrameTime {
        const Id id = e.side == 2 ? e.incoming : e.outgoing;
        const Clip* a = edit::clipById(before, id);
        const Clip* b = edit::clipById(after, id);
        if (!a || !b) return 0;
        return e.side == 2 ? a->duration - b->duration : b->end() - a->end();
    };
    state_->updateGesture([&](Project& p, Sequence& sq) {
        for (FrameTime d = want; d != from; d += want > from ? -1 : 1) {
            Project trial = p;
            Sequence& ts = *trial.findSequence(sq.id);
            if (trim(trial, ts, d)) {
                reached = moved(sq, ts);
                break;
            }
        }
        trim(p, sq, reached);
    });
    trimShuttle_->applied = reached;
    if (reached != want) {
        trimShuttle_->played = double(reached);
        trimShuttle_->speed = 0;
        if (trimShuttleTimer_) trimShuttleTimer_->stop();
        statusBar()->showMessage(tr("The trim can go no further: K keeps it, Esc puts it back"), 5000);
    }
    showTrimEdit();
}

bool MainWindow::stopTrimShuttle(bool keep) {
    if (!trimShuttle_) return false;
    if (trimShuttleTimer_) trimShuttleTimer_->stop();
    const FrameTime applied = trimShuttle_->applied;
    trimShuttle_.reset();
    state_->endGesture(keep);
    showTrimEdit();
    if (keep && applied) statusBar()->showMessage(tr("Trimmed %1 frame(s)").arg(applied), 4000);
    return true;
}

void MainWindow::endTrimMode() {
    stopTrimShuttle(false);
    if (!trimEdit_) return;
    trimEdit_.reset();
    timeline_->clearTrimEdit();
    programPanel_->endTrimView();
}

void MainWindow::showTrimEdit() {
    const Sequence* s = state_->sequence();
    const Clip* out = s && trimEdit_ && trimEdit_->outgoing ? edit::clipById(*s, trimEdit_->outgoing) : nullptr;
    const Clip* in = s && trimEdit_ && trimEdit_->incoming ? edit::clipById(*s, trimEdit_->incoming) : nullptr;
    if (!trimEdit_ || (!out && !in)) {
        endTrimMode();
        return;
    }
    timeline_->setTrimEdit(trimEdit_->outgoing, trimEdit_->incoming, trimEdit_->side);
    // The playhead and the two-up follow the edit: the outgoing side's last frame and the incoming side's first.
    const FrameTime cut = trimEdit_->side == 2 && in ? in->start : out ? out->end() : in->start;
    const FrameTime left = trimEdit_->side == 2 && in ? in->start - 1 : cut - 1, right = trimEdit_->side == 1 && out ? out->end() : cut;
    state_->setPlayhead(cut);
    auto label = [&](const Clip* c, FrameTime f) {
        return (c ? QString::fromStdString(c->name) + QStringLiteral("  ") : QString()) + QString::fromStdString(formatTimecode(f, s->fps));
    };
    emit timeline_->trimViewChanged(left, right, label(out, left), label(in, right));
    const char* sides[] = {QT_TR_NOOP("both sides (roll)"), QT_TR_NOOP("the outgoing side"), QT_TR_NOOP("the incoming side")};
    statusBar()->showMessage(tr("Trimming %1: Ctrl+Left/Right a frame, with Shift five; Alt+T changes side; Esc ends").arg(tr(sides[trimEdit_->side])), 6000);
}

void MainWindow::addClipMarker() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    const FrameTime t = state_->playhead();
    // The selected clip under the playhead (picture or sound), else the picture there.
    const Clip* target = nullptr;
    for (Id id : state_->selectedClips())
        if (const Clip* c = edit::clipById(*s, id); c && c->contains(t)) {
            target = c;
            break;
        }
    if (!target) target = clipForCommand();
    if (!target) {
        statusBar()->showMessage(tr("Select a clip under the playhead to mark it"), 5000);
        return;
    }
    const Id id = target->id;
    const std::string name = "Marker " + std::to_string(target->markers.size() + 1);
    state_->edit(tr("Add Clip Marker"), [id, t, name](Project&, Sequence& sq) { return edit::addClipMarker(sq, id, t, Marker{0, 0, name, "", 0}); });
}

bool MainWindow::exportMarkers(const QString& path) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("pdf")) return exportMarkersPdf(path);
    const std::string text = ext == QLatin1String("txt") ? markersToAvidLocators(*s) : ext == QLatin1String("edl") ? markersToResolveEdl(*s) : markersToCsv(*s);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        statusBar()->showMessage(tr("Cannot write %1").arg(path), 6000);
        return false;
    }
    statusBar()->showMessage(tr("Exported %n marker(s) to %1", nullptr, int(s->markers.size())).arg(QFileInfo(path).fileName()), 6000);
    return true;
}

bool MainWindow::exportMarkersPdf(const QString& path) {
    // Resolve 21.1's marker list as a PDF: a page header, then each marker with a picture of its frame, timecode,
    // length, colour and notes, for a review or a client.
    const Sequence* s = state_->sequence();
    if (!s) return false;
    QTextDocument doc;
    const double fps = s->fpsValue();
    auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, s->fps)); };
    QString html = QStringLiteral("<h2>%1</h2><p style='color:#555'>%2 · %3 × %4 · %5 fps · %6</p>")
                       .arg(QString(s->name.c_str()).toHtmlEscaped(),
                            tr("%n marker(s)", nullptr, int(s->markers.size())))
                       .arg(s->width)
                       .arg(s->height)
                       .arg(fps, 0, 'f', 3)
                       .arg(QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' border='1' style='border-collapse:collapse' width='100%'>"
                           "<tr style='background:#eee'><th></th><th>#</th><th>%1</th><th>%2</th><th>%3</th><th>%4</th><th>%5</th></tr>")
                .arg(tr("Marker"), tr("Timecode"), tr("Duration"), tr("Colour"), tr("Notes"));
    RenderOptions o;
    o.scale = std::min(1.0, 192.0 / std::max(1, s->width));
    o.displaySpace = "rec709";
    for (size_t i = 0; i < s->markers.size(); ++i) {
        const Marker& mk = s->markers[i];
        QString picture;
        if (s->duration() > 0) {
            const Image view = renderProgramFrame(state_->project(), *s, std::clamp<FrameTime>(mk.t, 0, s->duration() - 1), o);
            if (!view.empty()) {
                QImage img(view.width, view.height, QImage::Format_RGBA8888);
                toRgba8(view, img.bits(), size_t(img.bytesPerLine()));
                const QUrl url(QStringLiteral("marker:%1").arg(i));
                doc.addResource(QTextDocument::ImageResource, url, img);
                picture = QStringLiteral("<img src='%1' width='%2'>").arg(url.toString()).arg(img.width());
            }
        }
        const QColor colour = theme::labelColor(mk.color);
        const QString swatch = colour.isValid() ? QStringLiteral("<span style='background:%1'>&nbsp;&nbsp;&nbsp;&nbsp;</span> %2")
                                                      .arg(colour.name(), QString::fromUtf8(theme::labelName(mk.color)))
                                                : QString();
        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td><b>%3</b>%4</td><td>%5</td><td>%6</td><td>%7</td><td>%8</td></tr>")
                    .arg(picture)
                    .arg(i + 1)
                    .arg(QString::fromStdString(mk.name).toHtmlEscaped(), mk.chapter ? tr(" (chapter)") : QString(), tc(mk.t),
                         mk.duration > 0 ? tc(mk.duration) : QString(), swatch, QString::fromStdString(mk.comment).toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    doc.setHtml(html);
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageOrientation(QPageLayout::Landscape);
    writer.setTitle(tr("Markers: %1").arg(QString::fromStdString(s->name)));
    writer.setCreator(QStringLiteral("Montage"));
    doc.print(&writer);
    if (!QFileInfo(path).exists() || QFileInfo(path).size() == 0) {
        statusBar()->showMessage(tr("Cannot write %1").arg(path), 6000);
        return false;
    }
    statusBar()->showMessage(tr("Exported %n marker(s) to %1", nullptr, int(s->markers.size())).arg(QFileInfo(path).fileName()), 6000);
    return true;
}

int MainWindow::importMarkers(const QString& path) {
    const Sequence* s = state_->sequence();
    QFile f(path);
    if (!s || !f.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(tr("Cannot read %1").arg(path), 6000);
        return 0;
    }
    std::vector<Marker> markers;
    std::string err;
    if (!parseMarkerList(f.readAll().toStdString(), *s, markers, &err)) {
        statusBar()->showMessage(QString::fromStdString(err), 6000);
        return 0;
    }
    state_->edit(tr("Import Markers"), [&](Project&, Sequence& sq) {
        for (const Marker& m : markers) edit::addMarker(sq, m);
        return true;
    });
    statusBar()->showMessage(tr("Imported %n marker(s)", nullptr, int(markers.size())), 6000);
    return int(markers.size());
}

int MainWindow::suggestChapterMarkers(double minSeconds, bool ask) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    std::string err;
    auto find = [&](double secs) {
        ChapterOptions o;
        o.minSeconds = secs;
        return montage::suggestChapters(state_->project(), *s, o, &err);
    };
    std::vector<SuggestedChapter> chapters = find(minSeconds);
    if (chapters.empty()) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    bool replace = true;
    if (ask) {
        QDialog dlg(this);
        dlg.setObjectName(QStringLiteral("suggestChaptersDialog"));
        dlg.setWindowTitle(tr("Suggest Chapters"));
        auto* lay = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout;
        auto* shortest = new QSpinBox(&dlg);
        shortest->setObjectName(QStringLiteral("chapterMinSeconds"));
        shortest->setRange(10, 3600);
        shortest->setSuffix(tr(" s"));
        shortest->setValue(int(minSeconds));
        form->addRow(tr("Shortest chapter:"), shortest);
        lay->addLayout(form);
        auto* table = new QTableWidget(0, 2, &dlg);
        table->setObjectName(QStringLiteral("chapterTable"));
        table->setHorizontalHeaderLabels({tr("Starts"), tr("Title (double-click to rename)")});
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->hide();
        table->setMinimumSize(420, 220);
        lay->addWidget(table, 1);
        auto* keep = new QCheckBox(tr("Keep the chapter markers already there"), &dlg);
        keep->setObjectName(QStringLiteral("keepChapters"));
        lay->addWidget(keep);
        auto fill = [&] {
            table->setRowCount(int(chapters.size()));
            for (int r = 0; r < int(chapters.size()); ++r) {
                auto* when = new QTableWidgetItem(QString::fromStdString(formatTimecode(chapters[size_t(r)].start, s->fps)));
                when->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                table->setItem(r, 0, when);
                table->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(chapters[size_t(r)].title)));
            }
            table->resizeColumnToContents(0);
        };
        fill();
        connect(shortest, &QSpinBox::valueChanged, &dlg, [&](int v) {
            if (auto again = find(v); !again.empty()) {
                chapters = std::move(again);
                fill();
            }
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Chapters"));
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        lay->addWidget(buttons);
        if (dlg.exec() != QDialog::Accepted) return 0;
        for (int r = 0; r < table->rowCount() && r < int(chapters.size()); ++r)
            if (const QString t = table->item(r, 1)->text().trimmed(); !t.isEmpty()) chapters[size_t(r)].title = t.toStdString();
        replace = !keep->isChecked();
    }
    if (!state_->edit(tr("Suggest Chapters"), [&](Project&, Sequence& sq) { return edit::addSuggestedChapters(sq, chapters, replace).ok; }))
        return 0;
    state_->message(tr("%n chapter marker(s) added; Copy Chapters for YouTube lists them", "", int(chapters.size())), 6000);
    return int(chapters.size());
}

QString MainWindow::copyYoutubeChapters() {
    const Sequence* s = state_->sequence();
    if (!s) return {};
    // Within In to Out when both are set, as the export would be.
    const bool range = s->inPoint >= 0 && s->outPoint >= 0;
    std::string warning;
    const QString text = QString::fromStdString(youtubeChapters(*s, range ? s->inPoint : 0, range ? s->outPoint + 1 : -1, &warning));
    if (!text.isEmpty()) QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(warning.empty() ? tr("Copied %n chapter(s) for YouTube", nullptr, int(text.count(QLatin1Char('\n'))))
                                             : (text.isEmpty() ? QString() : tr("Copied chapters for YouTube. ")) + QString::fromStdString(warning),
                             8000);
    return text;
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
        AudioBufferPtr buf = MediaPool::instance().audio(audioKey(m->path, c->channels), s->sampleRate);
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

bool MainWindow::setAgentLink(bool on) {
    QString err;
    const bool ok = on ? liveLink_->start(0, &err) : (liveLink_->stop(), true);
    if (!ok) statusBar()->showMessage(tr("The agent link could not start: %1").arg(err), 8000);
    const bool running = liveLink_->running();
    appSettings().setValue(QStringLiteral("agentLink/enabled"), running);
    if (agentLinkAction_) agentLinkAction_->setChecked(running);
    if (ok && on) statusBar()->showMessage(tr("AI agents can now edit this project (montage-cli mcp --live, or %1)").arg(liveLink_->url()), 8000);
    return ok;
}

void MainWindow::agentLinkDialog() {
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("agentLinkWindow"));
    dlg.setWindowTitle(tr("Agent Link"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(tr("With the agent link on, Claude and other MCP clients use Montage's tools on the project open here. "
                                "Each change they make is one undo step named \"Assistant: …\", and they can read and move the playhead "
                                "and selection. Only programs on this computer that have the key can connect."),
                             &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* enabled = new QCheckBox(tr("Let AI agents edit the project open in Montage"), &dlg);
    enabled->setObjectName(QStringLiteral("agentLinkEnabled"));
    enabled->setChecked(liveLink_->running());
    lay->addWidget(enabled);
    auto* form = new QFormLayout;
    QString cli = QCoreApplication::applicationDirPath() + QStringLiteral("/montage-cli");
#ifdef Q_OS_WIN
    cli += QStringLiteral(".exe");
#endif
    auto* stdioCmd = new QLineEdit(&dlg);
    stdioCmd->setObjectName(QStringLiteral("agentLinkStdio"));
    stdioCmd->setReadOnly(true);
    auto* httpCmd = new QLineEdit(&dlg);
    httpCmd->setObjectName(QStringLiteral("agentLinkHttp"));
    httpCmd->setReadOnly(true);
    form->addRow(tr("Claude Code / Desktop:"), stdioCmd);
    form->addRow(tr("HTTP clients:"), httpCmd);
    lay->addLayout(form);
    auto refresh = [&] {
        stdioCmd->setText(QStringLiteral("claude mcp add montage-live -- \"%1\" mcp --live").arg(QDir::toNativeSeparators(cli)));
        httpCmd->setText(liveLink_->running() ? QStringLiteral("claude mcp add --transport http montage-live %1 --header \"Authorization: Bearer %2\"")
                                                    .arg(liveLink_->url(), liveLink_->token())
                                              : tr("(off)"));
    };
    refresh();
    connect(enabled, &QCheckBox::toggled, &dlg, [&](bool on) {
        setAgentLink(on);
        enabled->setChecked(liveLink_->running());
        refresh();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton* copy = buttons->addButton(tr("Copy Command"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [&] { QGuiApplication::clipboard()->setText(stdioCmd->text()); });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.resize(640, dlg.sizeHint().height());
    dlg.exec();
}

int MainWindow::keyOutScreen() {
    if (!state_->sequence()) return 0;
    std::vector<Id> clips(state_->selectedClips().begin(), state_->selectedClips().end());
    if (clips.empty())
        if (const Clip* c = clipForCommand()) clips.push_back(c->id);
    const FrameTime at = state_->playhead();
    int n = 0;
    std::string err;
    state_->edit(tr("Key Out Screen"), [&](Project& p, Sequence& sq) {
        for (Id id : clips)
            if (const auto loc = edit::locate(sq, id); loc && loc->track.kind == TrackKind::Video && keyScreen(p, sq, id, at, &err)) ++n;
        return n > 0;
    });
    state_->message(n ? tr("Keyed %n clip(s): fine-tune the Keyer in the Inspector (View: Matte or Status shows the matte)", "", n)
                      : (err.empty() ? tr("Select a green or blue screen clip") : QString::fromStdString(err)),
                    6000);
    return n;
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

int MainWindow::arrangeLayout(edit::Layout layout, double gap) {
    // The selected video clips; with fewer than two, every video clip under the playhead.
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    std::vector<Id> ids;
    for (Id id : state_->selectedClips())
        if (auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Video) ids.push_back(id);
    if (ids.size() < 2) {
        ids.clear();
        for (int i = 0; i < int(s->videoTracks.size()); ++i)
            if (const Clip* c = edit::clipAt(*s, {TrackKind::Video, i}, state_->playhead()); c && !s->videoTracks[size_t(i)].muted)
                ids.push_back(c->id);
    }
    if (ids.empty()) {
        state_->message(tr("Select the video clips to arrange, or put the playhead over them"));
        return 0;
    }
    edit::LayoutOptions o;
    o.gap = gap;
    const bool ok = state_->apply(tr("Layout"), [&](Project& p, Sequence& sq) { return edit::arrangeLayout(p, sq, ids, layout, o); });
    if (ok) state_->setSelection(ids);
    return ok ? int(ids.size()) : 0;
}

int MainWindow::setSelectedRole(const QString& role) {
    const std::vector<Id> sel = state_->selectedClips();
    if (sel.empty()) {
        state_->message(tr("Select the clips to give a role"));
        return 0;
    }
    int changed = 0;
    state_->edit(role.isEmpty() ? tr("Clear Audio Role") : tr("Audio Role: %1").arg(role), [&](Project&, Sequence& s) {
        changed = edit::setClipRole(s, sel, role.toStdString());
        return changed > 0;
    });
    if (changed) state_->message(role.isEmpty() ? tr("Cleared the role of %n audio clip(s)", nullptr, changed)
                                                : tr("%1 on %n audio clip(s)", nullptr, changed).arg(role));
    else state_->message(tr("No audio clips selected to change"));
    return changed;
}

int MainWindow::detectRoles() {
    // Listens to the audio (Auto Mix's classifier): the selected clips get what they are heard as; with nothing
    // selected, every audio clip without a role does.
    const Sequence* seq = state_->sequence();
    if (!seq) return 0;
    const std::vector<Id> sel = state_->selectedClips().empty() ? std::vector<Id>{} : edit::expandLinks(*seq, state_->selectedClips());
    const Project snap = state_->project();
    const Sequence s = *seq;
    std::vector<ClipMix> plan;
    const bool ok = runWithProgress(this, state_, tr("Listening to the audio…"), [&](const auto& progress, const std::atomic<bool>* cancel, std::string* err) {
        plan = planMix(snap, s, MixOptions{}, progress, cancel, err);
        if (plan.empty() && err->empty()) *err = "There is no audio to listen to";
        return !plan.empty();
    });
    if (!ok) return 0;
    int changed = 0;
    state_->edit(tr("Detect Roles"), [&](Project&, Sequence& sq) {
        for (const ClipMix& m : plan) {
            if (m.guess.role == AudioRole::Silence) continue;
            Clip* c = edit::clipById(sq, m.clip);
            if (!c) continue;
            const bool chosen = sel.empty() ? c->role.empty() : std::find(sel.begin(), sel.end(), c->id) != sel.end();
            const std::string role = audioRoleName(m.guess.role);
            if (!chosen || c->role == role) continue;
            c->role = role;
            ++changed;
        }
        return changed > 0;
    });
    state_->message(changed ? tr("Gave %n clip(s) a role", nullptr, changed) : tr("No clips needed a role"), 6000);
    return changed;
}

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
    QSettings settings = appSettings();
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

bool MainWindow::exportClipLut(const QString& path, int size) {
    const Clip* c = clipForCommand();
    if (!c) return false;
    const FrameTime t = std::clamp<FrameTime>(state_->playhead() - c->start, 0, std::max<FrameTime>(0, c->duration - 1));
    std::vector<std::string> skipped;
    const WorkingSpaceScope working(state_->sequence() ? &sequenceColorSpace(*state_->sequence()) : nullptr);
    const Lut3D lut = bakeLut(c->effects, t, size, &skipped);
    std::string err;
    if (!writeCubeLut(lut, path.toStdString(), c->name, &err)) {
        statusBar()->showMessage(QString::fromStdString(err), 6000);
        return false;
    }
    QStringList left;
    for (const std::string& n : skipped) left << QString::fromStdString(n);
    statusBar()->showMessage(left.isEmpty() ? tr("Exported the grade of \"%1\" as %2").arg(QString::fromStdString(c->name), QFileInfo(path).fileName())
                                            : tr("Exported the grade as %1, without %2 (a LUT holds only colour changes)")
                                                  .arg(QFileInfo(path).fileName(), left.join(QStringLiteral(", "))),
                             8000);
    return true;
}

void MainWindow::syncSequenceTabs() {
    if (!sequenceTabs_) return;
    const Project& p = state_->project();
    std::erase_if(openSequences_, [&](Id id) { return !p.findSequence(id); });
    if (p.activeSequence && std::find(openSequences_.begin(), openSequences_.end(), p.activeSequence) == openSequences_.end())
        openSequences_.push_back(p.activeSequence);
    const QSignalBlocker block(sequenceTabs_);
    while (sequenceTabs_->count() > int(openSequences_.size())) sequenceTabs_->removeTab(sequenceTabs_->count() - 1);
    while (sequenceTabs_->count() < int(openSequences_.size())) sequenceTabs_->addTab(QString());
    for (int i = 0; i < int(openSequences_.size()); ++i) {
        const Sequence* s = p.findSequence(openSequences_[size_t(i)]);
        sequenceTabs_->setTabText(i, s ? QString::fromStdString(s->name) : QString());
        if (openSequences_[size_t(i)] == p.activeSequence) sequenceTabs_->setCurrentIndex(i);
    }
    sequenceTabs_->setTabsClosable(openSequences_.size() > 1);
}

bool MainWindow::runProjectManager(const ConsolidateOptions& o) {
    if (o.folder.empty()) {
        statusBar()->showMessage(tr("Choose a folder to copy the project to"), 5000);
        return false;
    }
    const Project project = state_->project();
    ConsolidateResult res;
    const bool ok = runWithProgress(this, state_, o.trim ? tr("Consolidating the project…") : tr("Collecting the project's media…"),
                                    [&](const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
                                        return consolidateProject(project, o, &res, progress, cancel, error);
                                    });
    if (!ok) return false;
    QString text = tr("Copied the project to %1: %n file(s) copied", nullptr, res.copied).arg(QString::fromStdString(res.projectPath));
    if (res.trimmed) text += tr(", %n consolidated", nullptr, res.trimmed);
    if (!res.missing.empty()) text += tr(", %n missing", nullptr, int(res.missing.size()));
    statusBar()->showMessage(text, 10000);
    return true;
}

namespace {
// The clip an audition command acts on: the selected one, else the video clip under the playhead.
const Clip* auditionClip(const EditorState* state, const Clip* underPlayhead) {
    const Sequence* s = state->sequence();
    if (!s) return nullptr;
    for (Id id : state->selectedClips())
        if (const Clip* c = edit::clipById(*s, id); c && !c->isGenerator()) return c;
    return underPlayhead;
}
}  // namespace

QString MainWindow::saveEffectsAsPreset(const QString& name) {
    const Sequence* s = state_->sequence();
    const Clip* c = state_->primaryClip();
    const auto loc = c && s ? edit::locate(*s, c->id) : std::nullopt;
    if (!c || !loc || c->effects.empty()) {
        state_->message(tr("Select a clip with effects to save them as a preset"));
        return {};
    }
    EffectPreset preset;
    preset.name = name.trimmed().isEmpty() ? c->name : name.trimmed().toStdString();
    preset.video = loc->track.kind == TrackKind::Video;
    preset.effects = c->effects;
    QString error;
    const QString file = presets::save(preset, &error);
    if (file.isEmpty()) {
        state_->message(tr("Could not save the preset: %1").arg(error));
        return {};
    }
    effects_->reload();
    state_->message(tr("Saved preset %1 (in the Effects panel's Presets)").arg(QString::fromStdString(preset.name)), 4000);
    return file;
}

int MainWindow::applyEffectPreset(const QString& file) {
    EffectPreset preset;
    QString error;
    if (!presets::load(file, preset, &error)) {
        state_->message(tr("Could not read the preset: %1").arg(error));
        return 0;
    }
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const TrackKind kind = preset.video ? TrackKind::Video : TrackKind::Audio;
    std::vector<Id> targets;
    for (Id id : state_->selectedClips())
        if (const auto loc = edit::locate(*s, id); loc && loc->track.kind == kind) targets.push_back(id);
    if (targets.empty()) {
        state_->message(preset.video ? tr("Select the video clips to put the preset on") : tr("Select the audio clips to put the preset on"));
        return 0;
    }
    state_->edit(tr("Apply Preset %1").arg(QString::fromStdString(preset.name)), [&](Project& p, Sequence& sq) {
        for (Id id : targets)
            if (Clip* c = edit::clipById(sq, id)) applyPreset(p, *c, preset);
        return true;
    });
    return int(targets.size());
}

Id MainWindow::closeUp(double zoom) {
    const Sequence* s = state_->sequence();
    const Clip* c = clipForCommand();
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!s || !c || !m || m->kind == MediaKind::Sequence || m->kind == MediaKind::Audio) {
        state_->message(tr("Put the playhead over a video clip for a close-up"));
        return 0;
    }
    FrameTime from = c->start, to = c->end();
    if (s->inPoint >= 0 && s->outPoint >= s->inPoint && s->inPoint < c->end() && s->outPoint >= c->start) {
        from = std::max(s->inPoint, c->start);
        to = std::min(s->outPoint + 1, c->end());
    }
    // The face in the middle frame of the stretch, if there is a face model.
    double u = 0.5, v = 0.45;
    bool face = false;
    if (faceSearchAvailable() && faceModel().installed()) {
        if (auto model = FaceModel::load()) {
            const double sec = m->kind == MediaKind::Video ? c->sourceFrameAt((from + to) / 2) / s->fpsValue() : 0.0;
            if (Frame16Ptr f = MediaPool::instance().videoFrame(m->path, sec, 0, 0)) {
                const auto faces = model->detect(*f);
                if (!faces.empty() && f->width > 0 && f->height > 0) {
                    u = (faces.front().x + faces.front().w / 2) / f->width;
                    v = (faces.front().y + faces.front().h / 2) / f->height;
                    face = true;
                }
            }
        }
    }
    const Id clip = c->id;
    Id created = 0;
    state_->apply(tr("Close Up"), [&](Project& p, Sequence& sq) {
        edit::Result r = edit::closeUp(p, sq, clip, from, to, zoom, u, v);
        if (r.ok && !r.created.empty()) created = r.created.front();
        return r;
    });
    if (created) state_->message(face ? tr("Close-up framed on the face") : tr("No face found: the close-up is on the middle of the picture"), 4000);
    return created;
}

int MainWindow::joinThroughEdits() {
    if (!state_->sequence()) return 0;
    const std::vector<Id> ids = state_->selectedClips();
    int joined = 0;
    state_->edit(tr("Join Through Edits"), [&](Project& p, Sequence& s) {
        joined = edit::joinThroughEdits(p, s, ids);
        return joined > 0;
    });
    state_->message(joined ? tr("Joined %n through edit(s)", "", joined) : tr("No through edits to join"), 3000);
    return joined;
}

int MainWindow::addTakesFromBin() {
    const Clip* c = auditionClip(state_, clipForCommand());
    const Sequence* s = state_->sequence();
    if (!c || !s) {
        state_->message(tr("Select the clip to add takes to"));
        return 0;
    }
    std::vector<Id> ids = bin_->selectedMedia();
    if (ids.empty() && state_->sourceMedia()) ids.push_back(state_->sourceMedia());
    std::vector<std::pair<Id, double>> media;
    const double fps = s->fpsValue();
    for (Id id : ids) {
        const MediaItem* m = state_->project().findMedia(id);
        if (!m) continue;
        if (m->subclipOf) media.push_back({m->subclipOf, m->subclipIn * fps});
        else if (id == state_->sourceMedia() && state_->sourceIn() >= 0) media.push_back({id, double(state_->sourceIn())});
        else media.push_back({id, 0.0});
    }
    if (media.empty()) {
        state_->message(tr("Select the takes in the Media bin, or open one in the Source monitor"));
        return 0;
    }
    const Id clip = c->id;
    if (!state_->apply(tr("Add Takes"), [clip, media](Project& p, Sequence& sq) { return edit::addTakes(p, sq, clip, media); }))
        return 0;
    const Clip* after = edit::clipById(*state_->sequence(), clip);
    state_->message(tr("Audition: %n take(s) — Ctrl+Alt+Right and Left try them in place", "", after ? int(after->takes.size()) : 0), 5000);
    return int(media.size());
}

bool MainWindow::cycleTake(int step) {
    const Clip* c = auditionClip(state_, clipForCommand());
    if (!c || c->takes.empty()) {
        state_->message(tr("Select an audition (a clip with takes) to try its takes"));
        return false;
    }
    const Id clip = c->id;
    return state_->apply(step > 0 ? tr("Next Take") : tr("Previous Take"),
                         [clip, step](Project& p, Sequence& s) { return edit::cycleTake(p, s, clip, step); });
}

bool MainWindow::finalizeAudition() {
    const Clip* c = auditionClip(state_, clipForCommand());
    if (!c || c->takes.empty()) {
        state_->message(tr("Select an audition (a clip with takes) to finalize"));
        return false;
    }
    const Id clip = c->id;
    return state_->apply(tr("Finalize Audition"), [clip](Project& p, Sequence& s) { return edit::finalizeAudition(p, s, clip); });
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

Id MainWindow::makeHighlights(double seconds, const QString& lookFor) {
    std::vector<Id> media;
    for (Id id : bin_ ? bin_->selectedMedia() : std::vector<Id>{})
        if (const MediaItem* m = state_->project().findMedia(id); m && m->kind == MediaKind::Video) media.push_back(id);
    if (media.empty())
        for (const MediaItem& m : state_->project().media)
            if (m.kind == MediaKind::Video) media.push_back(m.id);
    if (media.empty()) {
        state_->message(tr("Import some video to make highlights from"));
        return 0;
    }
    HighlightOptions o;
    o.seconds = seconds;
    std::string err;
    if (!lookFor.trimmed().isEmpty() && visualSearchAvailable()) {
        if (!ensureModelPack(this, visualModel(), tr("Make Highlights"),
                             tr("Looking for something in the footage uses CLIP, which runs on this computer.")))
            return 0;
        if (!indexVideos(state_, media, this)) return 0;
        if (auto clip = ClipModel::load(&err)) o.lookFor = clip->text(lookFor.trimmed().toStdString(), &err);
    }
    auto project = std::make_shared<const Project>(state_->project());
    std::vector<HighlightMoment> moments;
    if (!runWithProgress(this, state_, tr("Finding the highlights..."), [&, project](const auto& progress, const auto* cancel, std::string* e) {
            moments = findHighlights(*project, media, o, progress, cancel, e);
            return !moments.empty();
        }))
        return 0;
    Id seq = 0;
    state_->edit(tr("Make Highlights"), [&](Project& p, Sequence&) {
        seq = makeHighlightSequence(p, moments);
        return seq != 0;
    });
    if (seq) {
        state_->setActiveSequence(seq);
        state_->message(tr("%n moment(s) in a new Highlights sequence", "", int(moments.size())), 6000);
    }
    return seq;
}

int MainWindow::importEmbeddedCaptions() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    // The selected video clip, else the one under the playhead on the target video track.
    const Clip* clip = nullptr;
    for (Id id : state_->selectedClips())
        for (const Track& t : s->videoTracks)
            for (const Clip& c : t.clips)
                if (c.id == id && c.mediaId) clip = &c;
    if (!clip)
        if (const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()}))
            for (const Clip& c : t->clips)
                if (c.mediaId && c.start <= state_->playhead() && state_->playhead() < c.end()) clip = &c;
    const MediaItem* m = clip ? state_->project().findMedia(clip->mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video) {
        state_->message(tr("Select a video clip whose file carries closed captions"));
        return 0;
    }
    const std::string path = m->path;
    const Rational fps = s->fps;
    const Clip target = *clip;
    std::vector<Caption> found;
    if (!runWithProgress(this, state_, tr("Reading closed captions..."), [&](const auto& progress, const auto* cancel, std::string* e) {
            return readEmbeddedCaptions(path, fps, found, progress, cancel, e);
        }))
        return 0;
    const std::vector<Caption> placed = captionsThroughClip(target, found);
    if (placed.empty()) {
        state_->message(tr("None of the clip's closed captions fall within it"));
        return 0;
    }
    state_->edit(tr("Import Embedded Captions"), [&](Project& p, Sequence& sq) {
        CaptionTrack t;
        t.id = p.newId();
        t.name = "CC1 - " + target.name;
        t.captions = placed;
        sq.captionTracks.push_back(std::move(t));
        return true;
    });
    state_->message(tr("%n closed caption(s) imported as a caption track", "", int(placed.size())), 5000);
    return int(placed.size());
}

int MainWindow::readBurnedInSubtitles(int where, const QString& language, bool ask) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    // The selected video clip, else the one under the playhead on the target video track.
    const Clip* clip = nullptr;
    for (Id id : state_->selectedClips())
        for (const Track& t : s->videoTracks)
            for (const Clip& c : t.clips)
                if (c.id == id && c.mediaId) clip = &c;
    if (!clip)
        if (const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()}))
            for (const Clip& c : t->clips)
                if (c.mediaId && c.start <= state_->playhead() && state_->playhead() < c.end()) clip = &c;
    const MediaItem* m = clip ? state_->project().findMedia(clip->mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video) {
        state_->message(tr("Select a video clip with subtitles in its picture"));
        return 0;
    }
    QString lang = language;
    if (ask) {
        QDialog dlg(this);
        dlg.setObjectName(QStringLiteral("burnedInDialog"));
        dlg.setWindowTitle(tr("Read Burned-In Subtitles"));
        auto* form = new QFormLayout(&dlg);
        auto* place = new QComboBox(&dlg);
        place->addItems({tr("Bottom of the picture"), tr("Top of the picture"), tr("Whole picture")});
        place->setCurrentIndex(std::clamp(where, 0, 2));
        auto* script = new QComboBox(&dlg);
        script->addItem(tr("English"), QStringLiteral("en"));
        script->addItem(tr("Other languages in Latin script (French, German, Spanish...)"), QStringLiteral("und"));
        form->addRow(new QLabel(tr("The subtitles in the picture of %1 are read off it and become a caption track.").arg(QString::fromStdString(clip->name)), &dlg));
        form->addRow(tr("Where they are:"), place);
        form->addRow(tr("Language:"), script);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Read"));
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(buttons);
        if (dlg.exec() != QDialog::Accepted) return 0;
        where = place->currentIndex();
        lang = script->currentData().toString();
    }
    if (!ensureModelPack(this, ocrModel(), tr("Read Burned-In Subtitles"),
                         tr("Reading text in the picture uses PP-OCR (PaddlePaddle, Apache-2.0), which runs on this computer.")))
        return 0;
    const TextRegion region = where == 1 ? TextRegion{0, 0, 1, 0.4} : where == 2 ? TextRegion{} : TextRegion{0, 0.6, 1, 1};
    const std::string path = m->path;
    const Rational fps = s->fps;
    const Clip target = *clip;
    // Only the part of the media the clip plays.
    const double a = std::min(target.sourceAt(0), target.sourceAt(double(target.duration))) / fps.toDouble();
    const double b = std::max(target.sourceAt(0), target.sourceAt(double(target.duration))) / fps.toDouble();
    std::vector<Caption> found;
    if (!runWithProgress(this, state_, tr("Reading the subtitles..."), [&](const auto& progress, const auto* cancel, std::string* e) {
            return montage::readBurnedInSubtitles(path, a, b + 1.0 / fps.toDouble(), region, fps, found, lang.toStdString(), 4,
                                                  [&](double f) { progress(f); }, cancel, e);
        }))
        return 0;
    const std::vector<Caption> placed = captionsThroughClip(target, found);
    if (placed.empty()) {
        state_->message(tr("No subtitles were read in the clip's picture"), 5000);
        return 0;
    }
    state_->edit(tr("Read Burned-In Subtitles"), [&](Project& p, Sequence& sq) {
        CaptionTrack t;
        t.id = p.newId();
        t.name = "Burned-In - " + target.name;
        t.language = lang == "und" ? "und" : "en";
        t.captions = placed;
        sq.captionTracks.push_back(std::move(t));
        return true;
    });
    state_->message(tr("%n subtitle(s) read into a caption track", "", int(placed.size())), 5000);
    return int(placed.size());
}

int MainWindow::removeMicBleed(std::vector<int> tracks, double reductionDb) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    if (tracks.empty())
        for (size_t i = 0; i < s->audioTracks.size(); ++i)
            if (!s->audioTracks[i].muted && !s->audioTracks[i].clips.empty()) tracks.push_back(int(i));
    if (tracks.size() < 2) {
        state_->message(tr("Remove Mic Bleed works across two audio tracks or more, a mic on each speaker"), 6000);
        return 0;
    }
    BleedOptions o;
    o.reductionDb = std::clamp(reductionDb, -60.0, -3.0);
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::vector<Spans> dips;
    if (!runWithProgress(this, state_, tr("Listening to each mic..."), [&, project](const auto&, const auto* cancel, std::string* e) {
            const Sequence* sq = project->findSequence(seqId);
            dips = sq ? bleedSpans(*project, *sq, tracks, o, e, cancel) : std::vector<Spans>{};
            return !dips.empty();
        }))
        return 0;
    int changed = 0;
    state_->edit(tr("Remove Mic Bleed"), [&](Project&, Sequence& sq) {
        changed = montage::removeMicBleed(sq, tracks, dips, o);
        return changed > 0;
    });
    size_t stretches = 0;
    for (const Spans& d : dips) stretches += d.size();
    state_->message(changed ? tr("Each mic dips while it is not its speaker's turn: %n clip(s), %1 stretch(es)", "", changed).arg(stretches)
                            : tr("No mic bleed to remove: every mic is its speaker's throughout"),
                    6000);
    return changed;
}

RedactFacesDialog* MainWindow::redactFacesDialog() {
    const Clip* c = state_->primaryClip();
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) {
        state_->message(tr("Select the video clip whose faces to cover"));
        return nullptr;
    }
    auto* dlg = new RedactFacesDialog(state_, c->id, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    return dlg;
}

PanFollowDialog* MainWindow::panFollowDialog() {
    const Clip* c = musicClip();
    if (!c || !c->mediaId) {
        state_->message(tr("Select the audio clip whose panning should follow the picture"));
        return nullptr;
    }
    auto* dlg = new PanFollowDialog(state_, c->id, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    return dlg;
}

SpectralRepairDialog* MainWindow::spectralRepairDialog() {
    const Clip* c = musicClip();
    if (!c || !c->mediaId) {
        state_->message(tr("Select the audio clip to repair"));
        return nullptr;
    }
    auto* dlg = new SpectralRepairDialog(state_, c->id, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    return dlg;
}

void MainWindow::micBleedDialog() {
    const Sequence* s = state_->sequence();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remove Mic Bleed"));
    auto* form = new QFormLayout(&dlg);
    form->addRow(new QLabel(tr("For talk recorded with a mic on each speaker: each track dips while it is not its speaker's turn, "
                               "so the others' voices it picked up go quiet. Two people talking at once both stay up."),
                            &dlg));
    std::vector<QCheckBox*> boxes;
    for (size_t i = 0; i < s->audioTracks.size(); ++i) {
        const Track& t = s->audioTracks[i];
        auto* box = new QCheckBox(QStringLiteral("A%1 %2").arg(i + 1).arg(QString::fromStdString(t.name)), &dlg);
        box->setChecked(!t.muted && !t.clips.empty());
        box->setEnabled(!t.clips.empty());
        form->addRow(boxes.empty() ? tr("Mics:") : QString(), box);
        boxes.push_back(box);
    }
    auto* amount = new QDoubleSpinBox(&dlg);
    amount->setRange(-60, -3);
    amount->setValue(-24);
    amount->setSuffix(tr(" dB"));
    form->addRow(tr("Dip by:"), amount);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<int> tracks;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i]->isChecked()) tracks.push_back(int(i));
    if (tracks.size() < 2) {
        state_->message(tr("Choose two mics or more"), 5000);
        return;
    }
    removeMicBleed(tracks, amount->value());
}

int MainWindow::removeLetterbox() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    struct Job {
        Id clip;
        std::string path;
        double from, to;
    };
    std::vector<Job> jobs;
    const double fps = s->fpsValue();
    for (Id id : state_->selectedClips())
        for (const Track& t : s->videoTracks)
            for (const Clip& c : t.clips)
                if (c.id == id && c.mediaId)
                    if (const MediaItem* m = state_->project().findMedia(c.mediaId); m && m->kind == MediaKind::Video)
                        jobs.push_back({c.id, m->path, std::min(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps,
                                        std::max(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps});
    if (jobs.empty()) {
        state_->message(tr("Select the video clips with black bars to remove"));
        return 0;
    }
    std::vector<Bars> bars(jobs.size());
    if (!runWithProgress(this, state_, tr("Looking for black bars..."), [&](const auto& progress, const auto* cancel, std::string* e) {
            for (size_t i = 0; i < jobs.size(); ++i) {
                if (!detectBars(jobs[i].path, jobs[i].from, jobs[i].to, bars[i], e, 9, cancel)) return false;
                progress(double(i + 1) / double(jobs.size()));
            }
            return true;
        }))
        return 0;
    int changed = 0;
    state_->apply(tr("Remove Letterbox"), [&](Project& p, Sequence& sq) {
        edit::Result all = edit::Result::fail("");
        for (size_t i = 0; i < jobs.size(); ++i)
            if (bars[i].any() && edit::removeLetterbox(p, sq, jobs[i].clip, bars[i]).ok) ++changed, all = {};
        return all;
    });
    state_->message(changed ? tr("Cropped the black bars off %n clip(s)", "", changed) : tr("No black bars found round the pictures"), 5000);
    return changed;
}

Id MainWindow::newColorGroup(const QString& name) {
    const std::vector<Id> sel = state_->selectedClips();
    Id created = 0;
    std::string why;
    state_->apply(tr("New Colour Group"), [&](Project& p, Sequence& s) {
        const edit::Result r = edit::makeColorGroup(p, s, sel, name.toStdString(), &created);
        why = r.error;
        return r;
    });
    if (!created) state_->message(QString::fromStdString(why.empty() ? std::string("Select the video clips to group") : why), 4000);
    else if (const ColorGroup* g = state_->sequence() ? findColorGroup(*state_->sequence(), created) : nullptr)
        state_->message(tr("Made the colour group %1: its grades are in the Inspector").arg(QString::fromStdString(g->name)), 5000);
    return created;
}

bool MainWindow::addToColorGroup(Id group) {
    const std::vector<Id> sel = state_->selectedClips();
    return state_->edit(tr("Add to Colour Group"), [&](Project&, Sequence& s) { return edit::addToColorGroup(s, sel, group).ok; });
}

bool MainWindow::removeFromColorGroup() {
    const std::vector<Id> sel = state_->selectedClips();
    return state_->edit(tr("Remove from Colour Group"), [&](Project&, Sequence& s) { return edit::removeFromColorGroup(s, sel).ok; });
}

std::vector<Id> MainWindow::exportVersions(const std::vector<VersionShape>& shapes, const QString& folder, bool captions, double lufs,
                                           const QString& presetName) {
    const Sequence* s = state_->sequence();
    if (!s || shapes.empty()) return {};
    const ExportPreset* preset = findExportPreset(presetName.toStdString());
    if (!preset) preset = findExportPreset("H.264 - High Quality");
    // Reframing reads every shot: done on a copy, then the versions added in one undo step.
    auto work = std::make_shared<Project>(state_->project());
    const Id source = s->id;
    std::vector<Id> ids;
    if (!runWithProgress(this, state_, tr("Finding the subject of each shot..."), [&, work](const auto& progress, const auto* cancel, std::string* e) {
            return makeVersionSequences(*work, source, shapes, ids, 1, [&](double f) { progress(f); }, cancel, e);
        }))
        return {};
    if (std::any_of(ids.begin(), ids.end(), [&](Id id) { return id != source; }))
        state_->edit(tr("Export Versions"), [&](Project& p, Sequence&) {
            p.sequences = work->sequences;
            p.media = work->media;
            p.nextId = std::max(p.nextId, work->nextId);
            return true;
        });
    QDir().mkpath(folder);
    for (Id id : ids) {
        const Sequence* v = state_->project().findSequence(id);
        if (!v) continue;
        const ExportSettings st = versionSettings(*v, preset ? preset->settings : ExportSettings{}, folder.toStdString(), captions, lufs);
        queue_->add(QString::fromStdString(v->name), preset ? QString::fromStdString(preset->name) : QString(), state_->project(), id, st);
    }
    if (queueDock_) queueDock_->show();
    state_->setActiveSequence(source);
    state_->message(tr("%n version(s) queued to render into %1", "", int(ids.size())).arg(QDir::toNativeSeparators(folder)), 6000);
    return ids;
}

void MainWindow::exportVersionsDialog() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("exportVersionsDialog"));
    dlg.setWindowTitle(tr("Export Versions"));
    auto* form = new QFormLayout(&dlg);
    form->addRow(new QLabel(tr("%1 in several shapes at once: each a copy reframed to follow its shots' subjects, then rendered.")
                                .arg(QString::fromStdString(s->name)),
                            &dlg));
    std::vector<std::pair<QCheckBox*, VersionShape>> boxes;
    const QStringList names = {tr("16:9 Landscape (YouTube)"), tr("9:16 Vertical (Shorts, Reels, TikTok)"), tr("4:5 Portrait (feeds)"), tr("1:1 Square")};
    const std::vector<VersionShape> shapes = standardVersionShapes();
    for (size_t i = 0; i < shapes.size(); ++i) {
        auto* box = new QCheckBox(names[int(i)], &dlg);
        box->setObjectName(QStringLiteral("version%1").arg(QString::fromStdString(shapes[i].label)));
        box->setChecked(i < 2);
        form->addRow(i == 0 ? tr("Shapes:") : QString(), box);
        boxes.push_back({box, shapes[i]});
    }
    auto* folder = new QLineEdit(&dlg);
    folder->setObjectName(QStringLiteral("versionsFolder"));
    QString start = appSettings().value(QStringLiteral("export/lastDirectory")).toString();
    if (start.isEmpty()) start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    folder->setText(start);
    auto* browse = new QPushButton(tr("Choose…"), &dlg);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Export Versions To"), folder->text());
        if (!d.isEmpty()) folder->setText(d);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    form->addRow(tr("Folder:"), row);
    auto* captions = new QCheckBox(tr("Burn in captions"), &dlg);
    captions->setChecked(!s->captionTracks.empty());
    captions->setEnabled(!s->captionTracks.empty());
    form->addRow(QString(), captions);
    auto* loud = new QComboBox(&dlg);
    loud->addItem(tr("Streaming: -14 LUFS"), -14.0);
    loud->addItem(tr("Podcast: -16 LUFS"), -16.0);
    loud->addItem(tr("As mixed"), 0.0);
    form->addRow(tr("Loudness:"), loud);
    auto* format = new QComboBox(&dlg);
    for (const char* name : {"H.264 - High Quality", "H.265 / HEVC", "Apple ProRes 422 HQ"})
        if (findExportPreset(name)) format->addItem(QString::fromLatin1(name));
    form->addRow(tr("Format:"), format);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Queue Versions"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<VersionShape> chosen;
    for (const auto& [box, shape] : boxes)
        if (box->isChecked()) chosen.push_back(shape);
    if (chosen.empty()) return;
    appSettings().setValue(QStringLiteral("export/lastDirectory"), folder->text());
    exportVersions(chosen, folder->text(), captions->isChecked(), loud->currentData().toDouble(), format->currentText());
}

Id MainWindow::fillRoomTone(TrackRef track, FrameTime at, FrameTime to, Id source) {
    const Sequence* s = state_->sequence();
    const Track* t = s ? trackAt(*s, track) : nullptr;
    if (!t || track.kind != TrackKind::Audio) return 0;
    // The gap around `at`, and the clips either side of it.
    TrackGap gap;
    if (!gapAt(*s, track, at, gap)) {
        state_->message(tr("Room tone fills a gap: there is a clip here"));
        return 0;
    }
    const FrameTime from = to > 0 ? at : gap.start;  // a range as given, else the whole gap
    const FrameTime end = to > 0 ? std::min(to, gap.end) : gap.end;
    if (end <= from) {
        state_->message(tr("There is no gap to fill here"));
        return 0;
    }
    const Clip* before = gap.before ? edit::clipById(*s, gap.before) : nullptr;
    const Clip* after = gap.after ? edit::clipById(*s, gap.after) : nullptr;
    const Clip* learnFrom = source ? edit::clipById(*s, source) : (before ? before : after);
    if (!learnFrom) {
        state_->message(tr("Room tone is learned from a clip on the track: there is none"));
        return 0;
    }
    const Project p = state_->project();
    const Sequence seq = *s;
    const Clip clip = *learnFrom;
    RoomToneProfile prof;
    if (!runWithProgress(this, state_, tr("Learning the room tone..."), [&](const auto&, const auto*, std::string* e) {
            return clipRoomTone(p, seq, clip, prof, e);
        }))
        return 0;
    const int64_t samples = int64_t(std::llround(double(end - from) / seq.fpsValue() * prof.sampleRate));
    const std::vector<float> tone = synthesizeRoomTone(prof, samples, uint32_t(from + 1));
    const QString project = state_->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Room Tone");
    QDir().mkpath(folder);
    QString path;
    int n = 1;
    do path = folder + '/' + QString::fromStdString(seq.name) + QStringLiteral(" Room Tone %1.wav").arg(n++);
    while (QFileInfo::exists(path));
    std::string err;
    if (!writeStereoWav(path.toStdString(), tone, prof.sampleRate, &err)) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    const std::vector<Id> media = state_->importFiles({path}, nullptr, QStringLiteral("Room Tone"));
    if (media.empty()) return 0;
    Id made = 0;
    state_->edit(tr("Fill with Room Tone"), [&](Project& pr, Sequence& sq) {
        if (!edit::placeMedia(pr, sq, media[0], from, 0, -1, {TrackKind::Video, 0}, track, false).ok) return false;
        if (const Track* tr = trackAt(sq, track))
            for (const Clip& c : tr->clips)
                if (c.start == from && c.mediaId == media[0]) made = c.id;
        return made != 0;
    });
    if (made) state_->message(tr("Filled %1 with room tone learned from %2").arg(QString::fromStdString(formatTimecode(end - from, seq.fps)),
                                                                                  QString::fromStdString(clip.name)),
                              5000);
    return made;
}

int MainWindow::deleteGaps(bool leading) {
    int closed = 0;
    FrameTime frames = 0;
    if (!state_->apply(tr("Delete Gaps"), [&](Project& p, Sequence& s) { return edit::deleteGaps(p, s, leading, &closed, &frames); }))
        return 0;
    const Sequence* s = state_->sequence();
    state_->message(tr("Closed %n gap(s), %1 in all", "", closed).arg(QString::fromStdString(formatTimecode(frames, s->fps))), 5000);
    return closed;
}

QString MainWindow::exportForReview(const QString& folder, const ReviewExportOptions& options) {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return {};
    }
    QDir().mkpath(folder);
    const ReviewPackage pkg = reviewPackage(*s, folder.toStdString(), options);
    std::string err;
    if (!writeReviewPage(pkg, &err)) {
        state_->message(QString::fromStdString(err), 6000);
        return {};
    }
    queue_->add(QString::fromStdString(s->name) + tr(" - Review"), tr("Review copy"), state_->project(), s->id, pkg.settings);
    if (queueDock_) queueDock_->show();
    const QString page = QString::fromStdString(pkg.pagePath);
    state_->message(tr("Review copy queued; send it with %1. The notes saved from the page come back with File › Import Review Notes.")
                        .arg(QFileInfo(page).fileName()),
                    8000);
    return page;
}

void MainWindow::exportForReviewDialog() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("exportForReviewDialog"));
    dlg.setWindowTitle(tr("Export for Review"));
    auto* form = new QFormLayout(&dlg);
    auto* intro = new QLabel(tr("A review copy of %1 and a page that plays it in any browser, offline. Reviewers pause on a frame or "
                                "mark a range, type notes and save them as a file; import that file and each note becomes a marker.")
                                 .arg(QString::fromStdString(s->name)),
                             &dlg);
    intro->setWordWrap(true);
    form->addRow(intro);
    auto* folder = new QLineEdit(&dlg);
    folder->setObjectName(QStringLiteral("reviewFolder"));
    QString start = appSettings().value(QStringLiteral("export/lastDirectory")).toString();
    if (start.isEmpty()) start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    folder->setText(start);
    auto* browse = new QPushButton(tr("Choose…"), &dlg);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Export for Review To"), folder->text());
        if (!d.isEmpty()) folder->setText(d);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    form->addRow(tr("Folder:"), row);
    auto* size = new QComboBox(&dlg);
    size->setObjectName(QStringLiteral("reviewSize"));
    size->addItem(tr("1080p"), 1080);
    size->addItem(tr("720p (smaller file)"), 720);
    size->addItem(tr("Sequence size (%1 x %2)").arg(s->width).arg(s->height), 0);
    form->addRow(tr("Size:"), size);
    auto* timecode = new QCheckBox(tr("Burn in timecode"), &dlg);
    timecode->setObjectName(QStringLiteral("reviewTimecode"));
    timecode->setChecked(true);
    form->addRow(QString(), timecode);
    auto* watermark = new QLineEdit(&dlg);
    watermark->setObjectName(QStringLiteral("reviewWatermark"));
    watermark->setPlaceholderText(tr("e.g. Review copy - not for broadcast"));
    form->addRow(tr("Watermark:"), watermark);
    auto* markers = new QCheckBox(tr("Show my markers on the page"), &dlg);
    markers->setObjectName(QStringLiteral("reviewMarkers"));
    markers->setChecked(!s->markers.empty());
    markers->setEnabled(!s->markers.empty());
    form->addRow(QString(), markers);
    const bool marked = s->inPoint >= 0 && s->outPoint > s->inPoint;
    auto* range = new QCheckBox(tr("Only In to Out"), &dlg);
    range->setObjectName(QStringLiteral("reviewRange"));
    range->setEnabled(marked);
    form->addRow(QString(), range);
    auto* note = new QPlainTextEdit(&dlg);
    note->setObjectName(QStringLiteral("reviewNote"));
    note->setPlaceholderText(tr("A message for the reviewers (optional)"));
    note->setFixedHeight(note->fontMetrics().lineSpacing() * 4 + 12);
    form->addRow(tr("Message:"), note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    ReviewExportOptions o;
    o.maxHeight = size->currentData().toInt();
    o.timecode = timecode->isChecked();
    o.watermark = watermark->text().trimmed().toStdString();
    o.markers = markers->isChecked();
    o.note = note->toPlainText().trimmed().toStdString();
    if (range->isChecked() && marked) {
        o.in = s->inPoint;
        o.out = s->outPoint + 1;
    }
    appSettings().setValue(QStringLiteral("export/lastDirectory"), folder->text());
    exportForReview(folder->text(), o);
}

QString MainWindow::exportDcpTo(const QString& parent, const DcpSettings& settings, QStringList* problems) {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return {};
    }
    // Made from a copy, off the UI thread (a feature takes a while: every frame is compressed to JPEG 2000).
    const Project project = state_->project();
    const Sequence seq = *s;
    QProgressDialog progress(tr("Making the DCP of %1...").arg(QString::fromStdString(seq.name)), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false};
    connect(&progress, &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    DcpResult result;
    std::string err;
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    QTimer tick;
    connect(&tick, &QTimer::timeout, this, [&] { progress.setValue(done.load()); });
    tick.start(100);
    // Then checked as a server would, reading every file back (also off the UI thread: a feature is many gigabytes).
    std::vector<std::string> issues;
    std::atomic<bool> checking{false};
    connect(&tick, &QTimer::timeout, this, [&] {
        if (checking.load()) progress.setLabelText(tr("Checking the DCP of %1...").arg(QString::fromStdString(seq.name)));
    });
    watcher.setFuture(QtConcurrent::run([&] {
        if (!exportDcp(project, seq, settings, parent.toStdString(), &result, [&](double f) {
                done = int(f * 850);
                return !cancel.load();
            }, &err))
            return false;
        checking = true;
        issues = verifyDcp(result.folder, [&](double f) {
            done = 850 + int(f * 150);
            return !cancel.load();
        });
        return true;
    }));
    if (!watcher.isFinished()) loop.exec();
    tick.stop();
    progress.close();
    if (!watcher.result()) {
        state_->message(cancel ? tr("DCP cancelled") : tr("No DCP: %1").arg(QString::fromStdString(err)), 8000);
        return {};
    }
    if (cancel.load() && !issues.empty() && issues.back() == "Stopped") {
        state_->message(tr("DCP %1 made; its check was stopped").arg(QString::fromStdString(result.name)), 8000);
        return QString::fromStdString(result.folder);
    }
    QStringList found;
    for (const std::string& i : issues) found << QString::fromStdString(i);
    if (problems) *problems = found;
    const QString folder = QString::fromStdString(result.folder);
    if (!found.isEmpty())
        state_->message(tr("DCP %1 made, but the check found: %2").arg(QString::fromStdString(result.name), found.join("; ")), 12000);
    else if (!result.cinemaProfile)
        state_->message(tr("DCP %1 made, but this FFmpeg has no OpenJPEG, so the pictures are not in the DCI profile most cinema servers need")
                            .arg(QString::fromStdString(result.name)),
                        12000);
    else
        state_->message(tr("DCP %1 made and checked (%2 fps, %3 x %4, %5 channels): copy the folder to a drive for the cinema")
                            .arg(QString::fromStdString(result.name))
                            .arg(result.fps)
                            .arg(result.width)
                            .arg(result.height)
                            .arg(result.channels),
                        12000);
    return folder;
}

QString MainWindow::exportImfTo(const QString& parent, const ImfSettings& settings, QStringList* problems) {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return {};
    }
    // Made from a copy, off the UI thread, then checked there too (every frame is compressed, every file read back).
    const Project project = state_->project();
    const Sequence seq = *s;
    QProgressDialog progress(tr("Making the IMF master of %1...").arg(QString::fromStdString(seq.name)), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false}, checking{false};
    connect(&progress, &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    ImfResult result;
    std::string err;
    std::vector<std::string> issues;
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    QTimer tick;
    connect(&tick, &QTimer::timeout, this, [&] {
        progress.setValue(done.load());
        if (checking.load()) progress.setLabelText(tr("Checking the IMF master of %1...").arg(QString::fromStdString(seq.name)));
    });
    tick.start(100);
    watcher.setFuture(QtConcurrent::run([&] {
        if (!exportImf(project, seq, settings, parent.toStdString(), &result, [&](double f) {
                done = int(f * 850);
                return !cancel.load();
            }, &err))
            return false;
        checking = true;
        issues = verifyImf(result.folder, [&](double f) {
            done = 850 + int(f * 150);
            return !cancel.load();
        });
        return true;
    }));
    if (!watcher.isFinished()) loop.exec();
    tick.stop();
    progress.close();
    if (!watcher.result()) {
        state_->message(cancel ? tr("IMF master cancelled") : tr("No IMF master: %1").arg(QString::fromStdString(err)), 8000);
        return {};
    }
    const QString folder = QString::fromStdString(result.folder);
    const QString name = QFileInfo(folder).fileName();
    if (cancel.load() && !issues.empty() && issues.back() == "Stopped") {
        state_->message(tr("IMF master %1 made; its check was stopped").arg(name), 8000);
        return folder;
    }
    QStringList found;
    for (const std::string& i : issues) found << QString::fromStdString(i);
    if (problems) *problems = found;
    if (!found.isEmpty())
        state_->message(tr("IMF master %1 made, but the check found: %2").arg(name, found.join("; ")), 12000);
    else
        state_->message(tr("IMF master %1 made and checked (%2 x %3, %4-bit %5, %6 channels)")
                            .arg(name)
                            .arg(result.width)
                            .arg(result.height)
                            .arg(result.bits)
                            .arg(QString::fromStdString(result.colour))
                            .arg(result.channels),
                        12000);
    return folder;
}

bool MainWindow::exportAdmTo(const QString& path, const AdmSettings& settings, AdmResult* resultOut) {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return false;
    }
    // Mixed from a copy, off the UI thread.
    const Project project = state_->project();
    const Sequence seq = *s;
    QProgressDialog progress(tr("Making the immersive master of %1...").arg(QString::fromStdString(seq.name)), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false};
    connect(&progress, &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    AdmResult result;
    std::string err;
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    QTimer tick;
    connect(&tick, &QTimer::timeout, this, [&] { progress.setValue(done.load()); });
    tick.start(100);
    watcher.setFuture(QtConcurrent::run([&] {
        return exportAdmBwf(project, seq, settings, path.toStdString(), &result, [&](double f) {
            done = int(f * 1000);
            return !cancel.load();
        }, &err);
    }));
    if (!watcher.isFinished()) loop.exec();
    tick.stop();
    progress.close();
    if (!watcher.result()) {
        state_->message(cancel ? tr("Immersive master cancelled") : tr("No immersive master: %1").arg(QString::fromStdString(err)), 8000);
        return false;
    }
    if (resultOut) *resultOut = result;
    state_->message(tr("Immersive master %1 written: a %2 bed and %n object(s)", "", result.objects)
                        .arg(QFileInfo(path).fileName(), QString::fromStdString(seq.audioLayout)),
                    10000);
    return true;
}

void MainWindow::exportAdmDialog() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return;
    }
    if (!immersiveLayout(s->audioLayout) && layoutChannels(s->audioLayout) <= 2 && admObjectTracks(*s).empty() &&
        QMessageBox::question(this, tr("Export Immersive Master"),
                              tr("%1 is mixed in stereo. Set an immersive layout (Sequence Settings) and mark tracks as audio "
                                 "objects (right-click their panners) to make an immersive master.\n\nExport the stereo bed anyway?")
                                  .arg(QString::fromStdString(s->name))) != QMessageBox::Yes)
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Immersive Master (ADM BWF)"),
                                                      QString::fromStdString(s->name) + QStringLiteral(".wav"), tr("ADM BWF (*.wav)"));
    if (path.isEmpty()) return;
    AdmSettings st;
    st.title = s->name;
    st.inOut = s->inPoint >= 0 && s->outPoint >= s->inPoint;
    exportAdmTo(QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".wav") : path, st);
}

void MainWindow::exportImfDialog() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return;
    }
    if (!openJpegAvailable()) {
        state_->message(tr("This build of Montage cannot make IMF masters: it was built without OpenJPEG 2.5"), 8000);
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("exportImfDialog"));
    dlg.setWindowTitle(tr("Export IMF Master"));
    auto* form = new QFormLayout(&dlg);
    auto* intro = new QLabel(tr("An IMF (Interoperable Master Format) package of %1, Application #2E, as streaming services and broadcasters "
                                "ask for: JPEG 2000 pictures (lossless by default), 24-bit sound with channel labels, checked when it is done.")
                                 .arg(QString::fromStdString(s->name)),
                             &dlg);
    intro->setWordWrap(true);
    form->addRow(intro);
    auto* title = new QLineEdit(&dlg);
    title->setObjectName(QStringLiteral("imfTitle"));
    const QString file = state_->filePath();
    title->setText(file.isEmpty() ? QString::fromStdString(s->name) : QFileInfo(file).completeBaseName());
    form->addRow(tr("Title:"), title);
    auto* kind = new QComboBox(&dlg);
    kind->setObjectName(QStringLiteral("imfKind"));
    for (const auto& [label, value] : std::vector<std::pair<QString, QString>>{{tr("Feature"), "feature"}, {tr("Episode"), "episode"},
                                                                             {tr("Short"), "short"}, {tr("Trailer"), "trailer"},
                                                                             {tr("Teaser"), "teaser"}, {tr("Advertisement"), "advertisement"},
                                                                             {tr("Promotion"), "promotion"}, {tr("Test"), "test"}})
        kind->addItem(label, value);
    form->addRow(tr("Kind:"), kind);
    auto* size = new QComboBox(&dlg);
    size->setObjectName(QStringLiteral("imfSize"));
    size->addItem(tr("As the sequence (%1 x %2)").arg(s->width).arg(s->height), "sequence");
    size->addItem(tr("HD (1920 x 1080)"), "hd");
    size->addItem(tr("UHD (3840 x 2160)"), "uhd");
    size->addItem(tr("4K (4096 x 2160)"), "4k");
    form->addRow(tr("Picture:"), size);
    auto* colour = new QComboBox(&dlg);
    colour->setObjectName(QStringLiteral("imfColour"));
    colour->addItem(tr("Rec.709 (SDR)"), "rec709");
    colour->addItem(tr("P3-D65 PQ (HDR)"), "p3d65-pq");
    colour->addItem(tr("Rec.2020 PQ (HDR10)"), "rec2020-pq");
    colour->addItem(tr("Rec.2020 HLG"), "rec2020-hlg");
    colour->setCurrentIndex(colour->findData(QString::fromStdString(defaultImfColour(*s))));
    form->addRow(tr("Colour:"), colour);
    auto* bits = new QComboBox(&dlg);
    bits->setObjectName(QStringLiteral("imfBits"));
    bits->addItem(tr("Automatic (10-bit SDR, 12-bit HDR)"), 0);
    bits->addItem(tr("10-bit"), 10);
    bits->addItem(tr("12-bit"), 12);
    form->addRow(tr("Depth:"), bits);
    auto* coding = new QComboBox(&dlg);
    coding->setObjectName(QStringLiteral("imfCoding"));
    coding->addItem(tr("Lossless"), 0);
    for (int mbps : {800, 400, 200}) coding->addItem(tr("Lossy, %1 Mbit/s").arg(mbps), mbps);
    form->addRow(tr("JPEG 2000:"), coding);
    auto* language = new QLineEdit(QStringLiteral("en"), &dlg);
    language->setObjectName(QStringLiteral("imfLanguage"));
    form->addRow(tr("Sound language:"), language);
    auto* issuer = new QLineEdit(appSettings().value(QStringLiteral("imf/issuer"), QStringLiteral("Montage")).toString(), &dlg);
    issuer->setObjectName(QStringLiteral("imfIssuer"));
    form->addRow(tr("Issuer:"), issuer);
    const bool marked = s->inPoint >= 0 && s->outPoint >= s->inPoint;
    auto* range = new QCheckBox(tr("Only In to Out"), &dlg);
    range->setObjectName(QStringLiteral("imfRange"));
    range->setEnabled(marked);
    form->addRow(QString(), range);
    auto* folder = new QLineEdit(&dlg);
    folder->setObjectName(QStringLiteral("imfFolder"));
    QString start = appSettings().value(QStringLiteral("export/lastDirectory")).toString();
    if (start.isEmpty()) start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    folder->setText(start);
    auto* browse = new QPushButton(tr("Choose…"), &dlg);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Make the IMF Master In"), folder->text());
        if (!d.isEmpty()) folder->setText(d);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    form->addRow(tr("Make it in:"), row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Make IMF Master"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    ImfSettings st;
    st.title = title->text().trimmed().toStdString();
    st.kind = kind->currentData().toString().toStdString();
    st.size = size->currentData().toString().toStdString();
    st.colour = colour->currentData().toString().toStdString();
    st.bits = bits->currentData().toInt();
    st.lossless = coding->currentData().toInt() == 0;
    if (!st.lossless) st.megabitsPerSecond = coding->currentData().toInt();
    st.language = language->text().trimmed().toStdString();
    st.issuer = issuer->text().trimmed().toStdString();
    st.inOut = range->isChecked() && marked;
    appSettings().setValue(QStringLiteral("export/lastDirectory"), folder->text());
    appSettings().setValue(QStringLiteral("imf/issuer"), issuer->text().trimmed());
    exportImfTo(folder->text(), st);
}

void MainWindow::exportDcpDialog() {
    const Sequence* s = state_->sequence();
    if (!s || s->duration() == 0) {
        state_->message(tr("Nothing to export"));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("exportDcpDialog"));
    dlg.setWindowTitle(tr("Export DCP"));
    auto* form = new QFormLayout(&dlg);
    auto* intro = new QLabel(tr("A Digital Cinema Package of %1 for cinema servers and festivals (SMPTE, 2K JPEG 2000, 24-bit sound), "
                                "checked when it is done. Copy the folder it makes to a drive (ext2/3 or NTFS) for the cinema.")
                                 .arg(QString::fromStdString(s->name)),
                             &dlg);
    intro->setWordWrap(true);
    form->addRow(intro);
    auto* title = new QLineEdit(&dlg);
    title->setObjectName(QStringLiteral("dcpTitle"));
    const QString file = state_->filePath();
    title->setText(file.isEmpty() ? QString::fromStdString(s->name) : QFileInfo(file).completeBaseName());
    form->addRow(tr("Title:"), title);
    auto* kind = new QComboBox(&dlg);
    kind->setObjectName(QStringLiteral("dcpKind"));
    for (const auto& [label, value] : std::vector<std::pair<QString, QString>>{{tr("Feature"), "feature"}, {tr("Short"), "short"},
                                                                             {tr("Trailer"), "trailer"}, {tr("Teaser"), "teaser"},
                                                                             {tr("Advertisement"), "advertisement"}, {tr("Test"), "test"},
                                                                             {tr("Rating card"), "rating"}, {tr("Public service announcement"), "psa"}})
        kind->addItem(label, value);
    form->addRow(tr("Kind:"), kind);
    auto* container = new QComboBox(&dlg);
    container->setObjectName(QStringLiteral("dcpContainer"));
    container->addItem(tr("Flat (1.85:1, 1998 x 1080)"), "flat");
    container->addItem(tr("Scope (2.39:1, 2048 x 858)"), "scope");
    container->addItem(tr("Full container (1.90:1, 2048 x 1080)"), "full");
    container->setCurrentIndex(container->findData(QString::fromStdString(defaultDcpContainer(*s))));
    form->addRow(tr("Picture:"), container);
    auto* rate = new QComboBox(&dlg);
    rate->setObjectName(QStringLiteral("dcpRate"));
    rate->addItem(tr("As the sequence (%1 fps)").arg(dcpFrameRate(*s, 0)), 0);
    for (int r : {24, 25, 30, 48}) rate->addItem(tr("%1 fps").arg(r), r);
    form->addRow(tr("Frame rate:"), rate);
    auto* language = new QLineEdit(QStringLiteral("en"), &dlg);
    language->setObjectName(QStringLiteral("dcpLanguage"));
    form->addRow(tr("Sound language:"), language);
    auto* territory = new QLineEdit(QStringLiteral("XX"), &dlg);
    territory->setObjectName(QStringLiteral("dcpTerritory"));
    territory->setToolTip(tr("Where it will be shown (a two-letter country code; XX for anywhere)"));
    form->addRow(tr("Territory:"), territory);
    auto* issuer = new QLineEdit(appSettings().value(QStringLiteral("dcp/issuer"), QStringLiteral("Montage")).toString(), &dlg);
    issuer->setObjectName(QStringLiteral("dcpIssuer"));
    form->addRow(tr("Issuer:"), issuer);
    auto* studio = new QLineEdit(appSettings().value(QStringLiteral("dcp/studio")).toString(), &dlg);
    studio->setObjectName(QStringLiteral("dcpStudio"));
    studio->setPlaceholderText(tr("optional short code, for the name"));
    form->addRow(tr("Studio:"), studio);
    auto* facility = new QLineEdit(appSettings().value(QStringLiteral("dcp/facility")).toString(), &dlg);
    facility->setObjectName(QStringLiteral("dcpFacility"));
    facility->setPlaceholderText(tr("optional short code, for the name"));
    form->addRow(tr("Facility:"), facility);
    const bool marked = s->inPoint >= 0 && s->outPoint > s->inPoint;
    auto* range = new QCheckBox(tr("Only In to Out"), &dlg);
    range->setObjectName(QStringLiteral("dcpRange"));
    range->setEnabled(marked);
    form->addRow(QString(), range);
    auto* folder = new QLineEdit(&dlg);
    folder->setObjectName(QStringLiteral("dcpFolder"));
    QString start = appSettings().value(QStringLiteral("export/lastDirectory")).toString();
    if (start.isEmpty()) start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    folder->setText(start);
    auto* browse = new QPushButton(tr("Choose…"), &dlg);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Make the DCP In"), folder->text());
        if (!d.isEmpty()) folder->setText(d);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    form->addRow(tr("Make it in:"), row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Make DCP"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    DcpSettings st;
    st.title = title->text().trimmed().toStdString();
    st.kind = kind->currentData().toString().toStdString();
    st.container = container->currentData().toString().toStdString();
    st.fps = rate->currentData().toInt();
    st.language = language->text().trimmed().toStdString();
    st.territory = territory->text().trimmed().toStdString();
    st.issuer = issuer->text().trimmed().toStdString();
    st.studio = studio->text().trimmed().toStdString();
    st.facility = facility->text().trimmed().toStdString();
    st.inOut = range->isChecked() && marked;
    appSettings().setValue(QStringLiteral("export/lastDirectory"), folder->text());
    appSettings().setValue(QStringLiteral("dcp/issuer"), issuer->text().trimmed());
    appSettings().setValue(QStringLiteral("dcp/studio"), studio->text().trimmed());
    appSettings().setValue(QStringLiteral("dcp/facility"), facility->text().trimmed());
    exportDcpTo(folder->text(), st);
}

bool MainWindow::analyseHdrLightLevels(bool ask) {
    const Sequence* cur = state_->sequence();
    if (!cur) return false;
    if (!sequenceColorSpace(*cur).hdr()) {
        state_->message(tr("Light levels are measured on HDR sequences (PQ or HLG); see Sequence Settings"), 6000);
        return false;
    }
    // A copy to measure while the dialog runs.
    const Project p = state_->project();
    const Sequence s = *cur;
    const bool marked = s.inPoint >= 0 && s.outPoint >= s.inPoint;
    LightLevels l;
    // PQ: HDR10+'s scenes measured in the same pass.
    const bool pq = sequenceColorSpace(s).transfer == Transfer::Pq;
    std::vector<Hdr10PlusScene> scenes;
    const FrameTime from = marked ? s.inPoint : 0, to = marked ? s.outPoint + 1 : 0;
    if (!runWithProgress(this, state_, tr("Measuring light levels..."), [&](const auto& progress, const auto* cancel, std::string* e) {
            if (pq)
                return analyseHdr10Plus(p, s, from, to, sequenceColorSpace(s), std::clamp(s.hdrPeakNits, 100.0, 10000.0), scenes, e,
                                        [&](double f) { progress(f); }, cancel, &l);
            return measureLightLevels(p, s, from, to, l, e, 1.0, [&](double f) { progress(f); }, cancel);
        }))
        return false;
    unsigned cll = 0, fall = 0;
    hdr10LightLevels(l, cll, fall);
    state_->apply(tr("Analyse HDR Light Levels"), [&](Project&, Sequence& sq) {
        sq.hdrMaxCll = cll, sq.hdrMaxFall = fall;
        if (pq && !scenes.empty()) {
            // The new scenes replace those they overlap; others (another stretch analysed before) stay.
            std::erase_if(sq.hdr10Plus, [&](const Hdr10PlusScene& o) { return o.end > scenes.front().start && o.start < scenes.back().end; });
            sq.hdr10Plus.insert(sq.hdr10Plus.end(), scenes.begin(), scenes.end());
            std::sort(sq.hdr10Plus.begin(), sq.hdr10Plus.end(), [](const Hdr10PlusScene& x, const Hdr10PlusScene& y) { return x.start < y.start; });
        }
        return edit::Result{};
    });
    const QLocale loc;
    state_->message(pq ? tr("MaxCLL %1 nits, MaxFALL %2 nits, %3 HDR10+ scenes").arg(loc.toString(cll), loc.toString(fall)).arg(scenes.size())
                       : tr("MaxCLL %1 nits, MaxFALL %2 nits").arg(loc.toString(cll), loc.toString(fall)),
                    8000);
    if (!ask) return true;
    QMessageBox box(QMessageBox::Information, tr("HDR Light Levels"),
                    tr("MaxCLL %1 nits, the brightest pixel (at %2)\nMaxFALL %3 nits, the brightest frame on average (at %4)")
                        .arg(loc.toString(cll), QString::fromStdString(formatTimecode(l.maxCllFrame, s.fps)), loc.toString(fall),
                             QString::fromStdString(formatTimecode(l.maxFallFrame, s.fps))),
                    QMessageBox::Close, this);
    box.setObjectName(QStringLiteral("hdrLightLevelsDialog"));
    QString more = tr("Saved with the sequence: HDR10 exports state them (MP4 and MOV also measure what they render).");
    if (pq)
        more += tr("\n\nHDR10+: %n scene(s) measured, used by exports with HDR10+ metadata while the cut still matches them.", "", int(scenes.size()));
    if (sequenceColorSpace(s).transfer == Transfer::Pq && cll > s.hdrPeakNits + 0.5)
        more = tr("Brighter than the %1-nit mastering peak: highlights above it will be clipped or tone mapped by displays.\n\n")
                   .arg(loc.toString(qRound(s.hdrPeakNits))) + more;
    box.setInformativeText(more);
    QPushButton* go = box.addButton(tr("Go to Brightest Frame"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == go) state_->setPlayhead(l.maxCllFrame);
    return true;
}

void MainWindow::watchFoldersDialog() {
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("watchFoldersDialog"));
    dlg.setWindowTitle(tr("Watch Folders"));
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(tr("Media files that arrive in these folders (a card being offloaded, renders landing) come into the "
                                 "project by themselves, in a bin named for the folder. What is there now comes in too."),
                              &dlg));
    auto* list = new QListWidget(&dlg);
    list->setObjectName(QStringLiteral("watchFolderList"));
    auto refresh = [&] {
        list->clear();
        for (const std::string& f : state_->project().watchFolders) list->addItem(QString::fromStdString(f));
    };
    refresh();
    lay->addWidget(list, 1);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Folder…"), &dlg);
    auto* remove = new QPushButton(tr("Stop Watching"), &dlg);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    lay->addLayout(row);
    connect(add, &QPushButton::clicked, &dlg, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&dlg, tr("Watch Folder"), appSettings().value(QStringLiteral("lastImportDir")).toString());
        if (!dir.isEmpty() && state_->addWatchFolder(dir)) refresh();
    });
    connect(remove, &QPushButton::clicked, &dlg, [&] {
        if (QListWidgetItem* it = list->currentItem(); it && state_->removeWatchFolder(it->text())) refresh();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

std::vector<Id> MainWindow::shortsSource() const {
    std::vector<Id> media;
    auto usable = [](const MediaItem* m) { return m && !m->subclipOf && m->transcript && !m->transcript->empty(); };
    for (Id id : bin_ ? bin_->selectedMedia() : std::vector<Id>{})
        if (usable(state_->project().findMedia(id))) media.push_back(id);
    if (media.empty())
        for (const MediaItem& m : state_->project().media)
            if (usable(&m)) media.push_back(m.id);
    return media;
}

std::vector<Id> MainWindow::makeShorts(const ShortsOptions& o, const ShortBuild& b, bool queue) {
    const std::vector<Id> media = shortsSource();
    if (media.empty()) {
        state_->message(tr("Transcribe a video or recording first: shorts are found from what is said"), 6000);
        return {};
    }
    auto project = std::make_shared<const Project>(state_->project());
    std::vector<ShortMoment> moments;
    if (!runWithProgress(this, state_, tr("Finding shorts..."), [&, project](const auto& progress, const auto* cancel, std::string* e) {
            moments = findShorts(*project, media, o, progress, cancel, e);
            return !moments.empty();
        }))
        return {};
    return buildShorts(moments, b, queue);
}

std::vector<Id> MainWindow::buildShorts(const std::vector<ShortMoment>& moments, const ShortBuild& b, bool queue) {
    if (moments.empty()) return {};
    // Built on a copy while the footage is read (reframing looks at every shot), then added in one undo step.
    auto work = std::make_shared<Project>(state_->project());
    std::vector<Id> made;
    std::map<Id, int> counts;
    if (!runWithProgress(this, state_, tr("Building shorts..."), [&, work](const auto& progress, const auto* cancel, std::string* e) {
            for (size_t i = 0; i < moments.size(); ++i) {
                const MediaItem* m = work->findMedia(moments[i].media);
                const std::string base = m ? QFileInfo(QString::fromStdString(m->name)).completeBaseName().toStdString() : "Short";
                const std::string name = base + " - Short " + std::to_string(++counts[moments[i].media]);
                const Id id = makeShortSequence(*work, moments[i], b, name, cancel, e);
                if (!id) return false;
                made.push_back(id);
                progress(double(i + 1) / double(moments.size()));
            }
            return true;
        }))
        return {};
    const bool ok = state_->edit(tr("Make Shorts"), [&](Project& p, Sequence&) {
        for (Id id : made) {
            p.sequences.push_back(*work->findSequence(id));
            for (const MediaItem& m : work->media)
                if (m.kind == MediaKind::Sequence && m.sequenceId == id) p.media.push_back(m);
        }
        p.nextId = std::max(p.nextId, work->nextId);
        return true;
    });
    if (!ok) return {};
    if (queue) {
        const ExportPreset* preset = findExportPreset("Social - TikTok / Reels / Shorts");
        QString folder = appSettings().value(QStringLiteral("export/lastDirectory")).toString();
        if (folder.isEmpty() || !QDir(folder).exists()) folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (folder.isEmpty()) folder = QDir::homePath();
        for (Id id : made) {
            const Sequence* s = state_->project().findSequence(id);
            ExportSettings st = preset ? preset->settings : ExportSettings{};
            st.burnInCaptions = !s->captionTracks.empty();
            st.path = QDir(folder).filePath(QString::fromStdString(s->name) + QStringLiteral(".mp4")).toStdString();
            queue_->add(QString::fromStdString(s->name), preset ? QString::fromStdString(preset->name) : QString(), state_->project(), id, st);
        }
        if (queueDock_) queueDock_->show();
    }
    state_->setActiveSequence(made.front());
    state_->message(queue ? tr("%n short(s) made and queued to render", "", int(made.size())) : tr("%n short(s) made", "", int(made.size())), 6000);
    return made;
}

void MainWindow::shortsDialog() {
    if (shortsSource().empty()) {
        state_->message(tr("Transcribe a video or recording first: shorts are found from what is said"), 6000);
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("shortsDialog"));
    dlg.setWindowTitle(tr("Make Shorts"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    lay->addLayout(form);
    QSettings st = appSettings();
    auto* count = new QSpinBox(&dlg);
    count->setObjectName(QStringLiteral("shortsCount"));
    count->setRange(1, 30);
    count->setValue(st.value(QStringLiteral("shorts/count"), 5).toInt());
    form->addRow(tr("How many:"), count);
    auto* shortest = new QSpinBox(&dlg), *longest = new QSpinBox(&dlg);
    shortest->setObjectName(QStringLiteral("shortsMin"));
    longest->setObjectName(QStringLiteral("shortsMax"));
    shortest->setRange(1, 600);
    longest->setRange(1, 600);
    shortest->setSuffix(tr(" s"));
    longest->setSuffix(tr(" s"));
    shortest->setValue(st.value(QStringLiteral("shorts/min"), 15).toInt());
    longest->setValue(st.value(QStringLiteral("shorts/max"), 60).toInt());
    auto* range = new QHBoxLayout;
    range->addWidget(shortest);
    range->addWidget(new QLabel(tr("to"), &dlg));
    range->addWidget(longest);
    form->addRow(tr("Length:"), range);
    auto* shape = new QComboBox(&dlg);
    shape->setObjectName(QStringLiteral("shortsShape"));
    shape->addItem(tr("Vertical 9:16 (TikTok, Reels, Shorts)"), QSize(9, 16));
    shape->addItem(tr("Square 1:1"), QSize(1, 1));
    shape->addItem(tr("Portrait 4:5"), QSize(4, 5));
    shape->addItem(tr("Widescreen 16:9"), QSize(16, 9));
    shape->setCurrentIndex(std::clamp(st.value(QStringLiteral("shorts/shape"), 0).toInt(), 0, 3));
    form->addRow(tr("Shape:"), shape);
    auto* looks = new QComboBox(&dlg);
    looks->setObjectName(QStringLiteral("shortsLook"));
    looks->addItem(tr("No captions"), QString());
    for (const CaptionLook& l : captionLooks()) looks->addItem(QString::fromStdString(l.name), QString::fromStdString(l.id));
    const int look = looks->findData(st.value(QStringLiteral("shorts/look"), QStringLiteral("creator_pop")).toString());
    looks->setCurrentIndex(look < 0 ? 0 : look);
    form->addRow(tr("Captions:"), looks);
    auto* topic = new QLineEdit(&dlg);
    topic->setPlaceholderText(tr("Optional: what they should be about, e.g. \"pricing, customers\""));
    form->addRow(tr("Topic:"), topic);
    auto* fillers = new QCheckBox(tr("Cut filler words"), &dlg), *pauses = new QCheckBox(tr("Shorten pauses"), &dlg);
    auto* hook = new QCheckBox(tr("Show the opening line as a title"), &dlg), *follow = new QCheckBox(tr("Keep the speaker in frame"), &dlg);
    fillers->setChecked(st.value(QStringLiteral("shorts/fillers"), true).toBool());
    pauses->setChecked(st.value(QStringLiteral("shorts/pauses"), true).toBool());
    hook->setChecked(st.value(QStringLiteral("shorts/hook"), false).toBool());
    follow->setChecked(st.value(QStringLiteral("shorts/follow"), true).toBool());
    for (QCheckBox* c : {fillers, pauses, hook, follow}) form->addRow(QString(), c);
    auto* find = new QPushButton(tr("Find Moments"), &dlg);
    find->setObjectName(QStringLiteral("shortsFind"));
    lay->addWidget(find);
    auto* table = new QTableWidget(0, 4, &dlg);
    table->setObjectName(QStringLiteral("shortsTable"));
    table->setHorizontalHeaderLabels({tr("Use"), tr("Length"), tr("Score"), tr("Opening line")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumSize(560, 200);
    lay->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    QPushButton* create = buttons->addButton(tr("Create Sequences"), QDialogButtonBox::AcceptRole);
    QPushButton* createQueue = buttons->addButton(tr("Create and Queue"), QDialogButtonBox::AcceptRole);
    create->setObjectName(QStringLiteral("shortsCreate"));
    createQueue->setObjectName(QStringLiteral("shortsCreateQueue"));
    create->setEnabled(false);
    createQueue->setEnabled(false);
    lay->addWidget(buttons);
    std::vector<ShortMoment> moments;
    bool queue = false;
    connect(find, &QPushButton::clicked, &dlg, [&] {
        ShortsOptions o;
        o.count = count->value();
        o.minSeconds = std::min(shortest->value(), longest->value());
        o.maxSeconds = std::max(shortest->value(), longest->value());
        o.topic = topic->text().toStdString();
        o.fillers.custom = state_->project().fillerWords;
        auto project = std::make_shared<const Project>(state_->project());
        const std::vector<Id> media = shortsSource();
        moments.clear();
        runWithProgress(&dlg, state_, tr("Finding shorts..."), [&, project](const auto& progress, const auto* cancel, std::string* e) {
            moments = findShorts(*project, media, o, progress, cancel, e);
            return !moments.empty();
        });
        table->setRowCount(int(moments.size()));
        for (int r = 0; r < int(moments.size()); ++r) {
            const ShortMoment& m = moments[size_t(r)];
            auto* use = new QTableWidgetItem;
            use->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            use->setCheckState(Qt::Checked);
            table->setItem(r, 0, use);
            table->setItem(r, 1, new QTableWidgetItem(tr("%1 s").arg(m.out - m.in, 0, 'f', 0)));
            table->setItem(r, 2, new QTableWidgetItem(QString::number(m.score, 'f', 2)));
            auto* line = new QTableWidgetItem(QString::fromStdString(m.hookLine));
            line->setToolTip(QString::fromStdString(m.text));
            table->setItem(r, 3, line);
        }
        table->resizeColumnsToContents();
        create->setEnabled(!moments.empty());
        createQueue->setEnabled(!moments.empty());
    });
    connect(create, &QPushButton::clicked, &dlg, [&] { queue = false; });
    connect(createQueue, &QPushButton::clicked, &dlg, [&] { queue = true; });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    st.setValue(QStringLiteral("shorts/count"), count->value());
    st.setValue(QStringLiteral("shorts/min"), shortest->value());
    st.setValue(QStringLiteral("shorts/max"), longest->value());
    st.setValue(QStringLiteral("shorts/shape"), shape->currentIndex());
    st.setValue(QStringLiteral("shorts/look"), looks->currentData().toString());
    st.setValue(QStringLiteral("shorts/fillers"), fillers->isChecked());
    st.setValue(QStringLiteral("shorts/pauses"), pauses->isChecked());
    st.setValue(QStringLiteral("shorts/hook"), hook->isChecked());
    st.setValue(QStringLiteral("shorts/follow"), follow->isChecked());
    std::vector<ShortMoment> chosen;
    for (int r = 0; r < table->rowCount(); ++r)
        if (table->item(r, 0)->checkState() == Qt::Checked) chosen.push_back(moments[size_t(r)]);
    ShortBuild b;
    const QSize aspect = shape->currentData().toSize();
    b.aspectW = aspect.width();
    b.aspectH = aspect.height();
    b.captionLook = looks->currentData().toString().toStdString();
    b.removeFillers = fillers->isChecked();
    b.removePauses = pauses->isChecked();
    b.hookTitle = hook->isChecked();
    b.reframe = follow->isChecked();
    b.fillers.custom = state_->project().fillerWords;
    buildShorts(chosen, b, queue);
}

int MainWindow::addBroll(double coverage) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    if (!visualSearchAvailable()) {
        state_->message(tr("This build of Montage cannot search footage (no ONNX Runtime)"));
        return 0;
    }
    // The footage to choose from: the videos selected in the bin, or every video the sequence does not use.
    std::vector<Id> media;
    for (Id id : bin_ ? bin_->selectedMedia() : std::vector<Id>{})
        if (const MediaItem* m = state_->project().findMedia(id); m && m->kind == MediaKind::Video) media.push_back(id);
    if (media.empty()) {
        std::vector<Id> used;
        for (TrackRef r : allTracks(*s))
            for (const Clip& c : trackAt(*s, r)->clips) used.push_back(c.mediaId);
        for (const MediaItem& m : state_->project().media)
            if (m.kind == MediaKind::Video && std::find(used.begin(), used.end(), m.id) == used.end()) media.push_back(m.id);
    }
    if (media.empty()) {
        state_->message(tr("Import some footage to cut away to"));
        return 0;
    }
    if (!ensureModelPack(this, visualModel(), tr("Add B-Roll"), tr("Matching footage to what is said uses CLIP, which runs on this computer.")))
        return 0;
    if (!indexVideos(state_, media, this)) return 0;
    std::string err;
    auto clip = ClipModel::load(&err);
    if (!clip) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    BrollOptions o;
    o.coverage = coverage;
    const std::vector<BrollPick> picks = planBroll(state_->project(), *s, media, [&](const std::string& t) { return clip->text(t); }, o, &err);
    if (picks.empty()) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    int n = 0;
    state_->apply(tr("Add B-Roll"), [&](Project& p, Sequence& sq) {
        edit::Result r = placeBroll(p, sq, picks, 1);
        n = int(r.created.size());
        return r;
    });
    if (n) state_->message(tr("%n cutaway(s) added on V2", "", n), 5000);
    return n;
}

void MainWindow::highlightsDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Make Highlights"));
    auto* form = new QFormLayout(&dlg);
    auto* secs = new QDoubleSpinBox(&dlg);
    secs->setObjectName(QStringLiteral("highlightSeconds"));
    secs->setRange(5, 600);
    secs->setValue(30);
    secs->setSuffix(tr(" s"));
    form->addRow(tr("Length:"), secs);
    auto* look = new QLineEdit(&dlg);
    look->setObjectName(QStringLiteral("highlightLookFor"));
    look->setPlaceholderText(tr("Optional: what to look for, e.g. \"people dancing\""));
    form->addRow(tr("Look for:"), look);
    form->addRow(new QLabel(tr("Uses the videos selected in the Media bin, or every video. The liveliest moments (loud, moving, "
                               "or like your description) go into a new sequence in the order they happened."),
                            &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() == QDialog::Accepted) makeHighlights(secs->value(), look->text());
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

bool MainWindow::swapClip(bool withNext) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    // The selected clip (a video one first), else the one under the playhead.
    const Clip* c = nullptr;
    for (Id id : state_->selectedClips()) {
        const auto loc = edit::locate(*s, id);
        if (loc && (!c || loc->track.kind == TrackKind::Video)) c = edit::clipById(*s, id);
        if (c && loc && loc->track.kind == TrackKind::Video) break;
    }
    if (!c) c = clipForCommand();
    if (!c) {
        state_->message(tr("Select a clip to swap"));
        return false;
    }
    const Id id = c->id;
    const std::vector<Id> keep(state_->selectedClips().begin(), state_->selectedClips().end());
    const bool ok = state_->apply(withNext ? tr("Swap with Next Clip") : tr("Swap with Previous Clip"),
                                  [id, withNext](Project& p, Sequence& sq) { return edit::swapClip(p, sq, id, withNext); });
    if (ok) state_->setSelection(keep.empty() ? std::vector<Id>{id} : keep);
    return ok;
}

void MainWindow::speedRamp(const std::string& preset, const QString& name) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // Each selected clip once (its linked sound follows it).
    std::vector<Id> clips;
    for (Id id : state_->selectedClips()) {
        const Clip* c = edit::clipById(*s, id);
        if (!c || c->isGenerator()) continue;
        const std::vector<Id> group = edit::linkedClips(*s, id);
        if (std::none_of(clips.begin(), clips.end(), [&](Id done) { return std::find(group.begin(), group.end(), done) != group.end(); }))
            clips.push_back(id);
    }
    if (clips.empty()) {
        state_->message(tr("Select clips to ramp"));
        return;
    }
    state_->apply(preset == "none" ? name : tr("Speed Ramp: %1").arg(name), [clips, preset](Project& p, Sequence& sq) {
        edit::Result last;
        bool any = false;
        for (Id id : clips) {
            last = edit::applySpeedRamp(p, sq, id, preset);
            any |= last.ok;
        }
        return any ? edit::Result{} : last;
    });
}

std::vector<Id> MainWindow::extendClip(Id clipId, FrameTime frames, bool ripple) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clipId) : nullptr;
    const auto loc = s ? edit::locate(*s, clipId) : std::nullopt;
    if (!c || !loc || loc->track.kind != TrackKind::Video || frames <= 0) {
        state_->message(tr("Select a video clip to extend"));
        return {};
    }
    // How the picture moves at the end, and the room under the linked sound, measured off the UI thread.
    const Project p = state_->project();
    const Sequence seq = *s;
    const Clip clip = *c;
    std::vector<Clip> sounds;
    for (Id id : edit::linkedClips(seq, clipId))
        if (const auto l = edit::locate(seq, id); l && l->track.kind == TrackKind::Audio) sounds.push_back(*edit::clipById(seq, id));
    EndMotion motion;
    std::vector<RoomToneProfile> rooms(sounds.size());
    if (!runWithProgress(this, state_, tr("Measuring the end of the shot..."), [&](const auto& progress, const auto* cancel, std::string* e) {
            if (!measureEndMotion(p, seq, clip, motion, e, [&](double f) { progress(f * 0.7); }, cancel)) return false;
            for (size_t i = 0; i < sounds.size(); ++i) clipRoomTone(p, seq, sounds[i], rooms[i], nullptr);  // silence: no fill
            return true;
        }))
        return {};
    // The room tone files, beside the project.
    const QString project = state_->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Room Tone");
    QStringList files;
    std::vector<size_t> filled;
    for (size_t i = 0; i < sounds.size(); ++i) {
        if (!rooms[i].valid()) continue;
        QDir().mkpath(folder);
        QString path;
        int n = 1;
        do path = folder + '/' + QString::fromStdString(seq.name) + QStringLiteral(" Room Tone %1.wav").arg(n++);
        while (QFileInfo::exists(path));
        const int64_t samples = int64_t(std::llround(double(frames) / seq.fpsValue() * rooms[i].sampleRate));
        if (!writeStereoWav(path.toStdString(), synthesizeRoomTone(rooms[i], samples, uint32_t(clipId + i)), rooms[i].sampleRate)) continue;
        files << path;
        filled.push_back(i);
    }
    const std::vector<Id> media = files.isEmpty() ? std::vector<Id>{} : state_->importFiles(files, nullptr, QStringLiteral("Room Tone"));
    std::vector<Id> made;
    std::string err;
    const bool ok = state_->edit(tr("Extend Clip"), [&](Project& pr, Sequence& sq) {
        Id video = 0;
        if (!montage::extendClip(pr, sq, clipId, frames, motion, ripple, &video, &err)) return false;
        made.push_back(video);
        const Id group = pr.newId();
        edit::clipById(sq, video)->linkGroup = group;
        for (size_t k = 0; k < filled.size() && k < media.size(); ++k) {
            const auto l = edit::locate(sq, sounds[filled[k]].id);
            const Clip* a = edit::clipById(sq, sounds[filled[k]].id);
            if (!l || !a) continue;
            const FrameTime at = a->end();
            const Track* tr = trackAt(sq, l->track);
            const bool free = std::none_of(tr->clips.begin(), tr->clips.end(), [&](const Clip& o) { return o.start < at + frames && o.end() > at; });
            if (!free || !edit::placeMedia(pr, sq, media[k], at, 0, frames, {TrackKind::Video, 0}, l->track, false).ok) continue;
            for (Clip& o : trackAt(sq, l->track)->clips)
                if (o.start == at && o.mediaId == media[k]) {
                    o.linkGroup = group;
                    made.push_back(o.id);
                }
        }
        return true;
    });
    if (!ok) {
        state_->message(QString::fromStdString(err), 6000);
        return {};
    }
    state_->message(motion.samples ? tr("Extended %1 by %2, carrying on its motion").arg(QString::fromStdString(clip.name),
                                                                                         QString::fromStdString(formatTimecode(frames, seq.fps)))
                                   : tr("Extended %1 by %2 (held)").arg(QString::fromStdString(clip.name),
                                                                         QString::fromStdString(formatTimecode(frames, seq.fps))),
                    5000);
    return made;
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

void MainWindow::setBackgroundRender(bool on) {
    backgroundRender_ = on;
    appSettings().setValue("render/background", on);
    stopBackgroundRender();
}

void MainWindow::setBackgroundRenderDelay(int ms) {
    backgroundTimer_.setInterval(std::max(0, ms));
    stopBackgroundRender();
}

void MainWindow::stopBackgroundRender() {
    if (backgroundCancel_) *backgroundCancel_ = true;
    backgroundTimer_.stop();
    if (backgroundRender_) backgroundTimer_.start();
}

void MainWindow::startBackgroundRender() {
    const Sequence* s = state_->sequence();
    if (!backgroundRender_ || !s || backgroundBusy_) return;
    if (program_->isPlaying()) {
        backgroundTimer_.start();
        return;
    }
    auto ranges = rangesToRender(*s);
    if (ranges.empty()) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const RenderOptions o = program_->renderOptions();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    backgroundCancel_ = cancel;
    backgroundBusy_ = true;
    auto* watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, cancel] {
        const int rendered = watcher->result();
        watcher->deleteLater();
        backgroundBusy_ = false;
        if (backgroundCancel_ == cancel) backgroundCancel_.reset();
        if (rendered > 0) refreshRenderBar();
        if (rendered < 0 && backgroundRender_ && !backgroundTimer_.isActive()) backgroundTimer_.start();  // stopped: try again later
    });
    watcher->setFuture(QtConcurrent::run([project, seqId, o, ranges, cancel] {
        const Sequence* sq = project->findSequence(seqId);
        if (!sq) return 0;
        int total = 0;
        for (const auto& [a, b] : ranges) {
            const int n = renderToCache(*project, *sq, a, std::min(b, sq->duration()) - 1, o, RenderCache::instance(), {}, cancel.get());
            if (n < 0) return -1;
            total += n;
        }
        return total;
    }));
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
    AudioBufferPtr refBuf = MediaPool::instance().audio(audioKey(state_->project().findMedia(ref->mediaId)->path, ref->channels), s->sampleRate);
    std::vector<std::pair<std::vector<Id>, FrameTime>> moves;
    QStringList failed;
    Id refGroup = ref->linkGroup;
    for (size_t i = 1; i < items.size(); ++i) {
        const Clip* o = edit::clipById(*s, items[i].id);
        if (refGroup && o->linkGroup == refGroup) continue;
        const MediaItem* om = state_->project().findMedia(o->mediaId);
        AudioBufferPtr ob = MediaPool::instance().audio(audioKey(om->path, o->channels), s->sampleRate);
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

bool MainWindow::exportAafTo(const QString& path, QString* summary, bool picture) {
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
    watcher.setFuture(QtConcurrent::run([snap, seqId, path, cancel, progress, picture]() {
        Outcome o;
        const Sequence* sq = snap->findSequence(seqId);
        if (!sq) return o;
        AafExportOptions options;
        options.picture = picture;
        o.ok = exportAaf(*snap, *sq, path.toStdString(), &o.result, [progress](double f, FrameTime) { *progress = f; }, cancel.get(),
                         &o.error, options);
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
    QString text = o.result.mediaFiles.empty() ? QFileInfo(path).fileName() + tr(": no sound")
                                               : tr("%1: %2 audio tracks, %3 clips, %4 crossfades; %5 WAV files in \"%6\"")
                                                     .arg(QFileInfo(path).fileName())
                                                     .arg(o.result.audioTracks)
                                                     .arg(o.result.clips)
                                                     .arg(o.result.transitions)
                                                     .arg(o.result.mediaFiles.size())
                                                     .arg(QFileInfo(path).completeBaseName() + tr(" Media"));
    if (o.result.videoTracks)
        text += tr("; %1 video tracks, %2 clips, %3 dissolves linked to the original files")
                    .arg(o.result.videoTracks)
                    .arg(o.result.videoClips)
                    .arg(o.result.videoTransitions);
    for (const std::string& w : o.result.warnings) text += "\n" + QString::fromStdString(w);
    if (summary) *summary = text;
    return true;
}

void MainWindow::exportAafDialog(bool picture) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    QSettings st = appSettings();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export AAF"), st.value("lastExportDir").toString() + "/" + QString::fromStdString(s->name) + ".aaf",
        tr("AAF (*.aaf)"));
    if (path.isEmpty()) return;
    st.setValue("lastExportDir", QFileInfo(path).absolutePath());
    QString summary;
    if (exportAafTo(path, &summary, picture)) statusBar()->showMessage(tr("Exported %1").arg(summary), 10000);
}

void MainWindow::importTimeline() {
    QSettings st = appSettings();
    QString path = QFileDialog::getOpenFileName(this, tr("Import Timeline"), st.value("lastImportTimelineDir").toString(),
                                                tr("Timelines (*.xml *.fcpxml *.otio *.edl *.aaf);;Final Cut Pro 7 XML (*.xml);;"
                                                   "FCPXML (*.fcpxml);;OpenTimelineIO (*.otio);;CMX 3600 EDL (*.edl);;"
                                                   "AAF (Media Composer, Pro Tools) (*.aaf)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).isDir()) path += "/Info.fcpxml";  // an .fcpxmld bundle
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Timeline"), tr("Cannot read %1").arg(path));
        return;
    }
    st.setValue("lastImportTimelineDir", QFileInfo(path).absolutePath());
    const QString suffix = QFileInfo(path).suffix().toLower();
    const std::string text = suffix == QLatin1String("aaf") ? std::string() : f.readAll().toStdString();
    const MediaProber prober = [](const std::string& file, MediaItem& m) { return probeMedia(file, m, nullptr); };
    const QString ext = QFileInfo(path).suffix().toLower();
    // EDLs do not say their rate: take the open sequence's.
    const Rational fps = state_->sequence() ? state_->sequence()->fps : Rational{30, 1};
    const std::string dir = QFileInfo(path).absolutePath().toStdString();
    ImportResult r;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    state_->edit(tr("Import Timeline"), [&](Project& p, Sequence&) {
        r = ext == "aaf"                      ? importAaf(p, path.toStdString(), prober)
            : ext == "edl"                    ? importEdl(p, text, fps, prober, dir)
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
            s.spherical = spec.spherical;
            s.vr180 = spec.vr180;
            s.stereo3d = spec.stereo3d;
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
    watcher->setFuture(QtConcurrent::run([] {
        plugins::ScanReport report = plugins::Registry::instance().scan();
        ofx::Registry::instance().scan();  // and the OpenFX video plugins
        return report;
    }));
}

void MainWindow::applyFromBrowser(const QString& typeQ, EffectCategory category) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    if (typeQ.startsWith(QStringLiteral("preset:"))) {
        applyEffectPreset(typeQ.mid(7));
        return;
    }
    std::string type = typeQ.toStdString();
    const EffectInfo* info = findEffectInfo(type);
    if (!info && !plugins::isPluginType(type) && !ofx::isOfxType(type)) return;
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
