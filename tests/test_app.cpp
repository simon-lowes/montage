// Application integration tests: drive the real main window offscreen —
// timeline mouse gestures, tools, undo, inspector, monitors and playback.
#include <QtTest>
#include <QSignalSpy>
#include <QPlainTextEdit>

#include <QAbstractScrollArea>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QStatusBar>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMimeData>
#include <QScrollBar>
#include <QSlider>
#include <QPushButton>
#include <QMenu>
#include <QPainter>
#include <QTabBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "AutoDuckDialog.h"
#include "AutoMixDialog.h"
#include "CaptionsPanel.h"
#include "ColorWheel.h"
#include "CurveEditor.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "ExportDialog.h"
#include "ExposureView.h"
#include "QualityCheckDialog.h"
#include "ProjectManagerDialog.h"
#include "MediaBinModel.h"
#include "ScopesWidget.h"
#include "LinkMediaDialog.h"
#include "EffectPresetStore.h"
#include "EffectsBrowser.h"
#include "MediaBinWidget.h"
#include "SmartBinDialog.h"
#include "ScriptCutDialog.h"
#include "core/KeyframeEdit.h"
#include "core/MediaLog.h"
#include "InspectorWidget.h"
#include "core/Bleep.h"
#include "core/Transcript.h"
#include "SequenceSettingsDialog.h"
#include "media/Vector.h"
#include "SurroundPanner.h"
#include "audio/Plugins.h"
#include "MainWindow.h"
#include "media/Beats.h"
#include "render/AudioFx.h"
#include "audio/SpeechCleanup.h"
#include "media/SpeechEnhance.h"
#include "media/Translator.h"
#include "ModelPacks.h"
#include "MixerPanel.h"
#include "MulticamPanel.h"
#include "PluginEditorWindow.h"
#include "audio/PluginEffect.h"
#include "KeyframePanel.h"
#include "Keymap.h"
#include "LoudnessReadout.h"
#include "Voiceover.h"
#include "MaskOverlay.h"
#include "media/Diarizer.h"
#include "media/VisualSearch.h"
#include "media/Faces.h"
#include "media/DepthMap.h"
#include "media/Rife.h"
#include "media/Matting.h"
#include "media/TextToSpeech.h"
#include "media/Inpaint.h"
#include "SpeechDialog.h"
#include "ShotSearchPanel.h"
#include "PeoplePanel.h"
#include "SequenceIndexPanel.h"
#include "core/Automation.h"
#include "core/History.h"
#include "render/Processing.h"
#include "media/Segmenter.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "Recovery.h"
#include "RenderQueue.h"
#include "RenderQueuePanel.h"
#include "TimelineWidget.h"
#include "TranscribeDialog.h"
#include "TranscriptPanel.h"
#include "core/AutoTag.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/Multicam.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/RenderCache.h"
#include "render/Exporter.h"
#include "render/Ocio.h"

using namespace montage;

namespace {

const TrackRef V1{TrackKind::Video, 0};
const TrackRef A1{TrackKind::Audio, 0};

// Builds a project with two coloured generator clips on V1 and a title on V2.
Project demoProject() {
    Project p = makeDefaultProject();
    Sequence& s = *p.active();
    s.width = 320;
    s.height = 180;
    Clip a = makeGeneratorClip(p, "color", 60);
    a.generator.params["color.r"] = 0.8;
    a.name = "Red";
    Clip b = makeGeneratorClip(p, "color", 60);
    b.generator.params["color.b"] = 0.8;
    b.start = 60;
    b.name = "Blue";
    edit::overwrite(p, s, V1, a);
    edit::overwrite(p, s, V1, b);
    Clip t = makeGeneratorClip(p, "title", 45);
    t.start = 15;
    edit::overwrite(p, s, {TrackKind::Video, 1}, t);
    return p;
}

const Clip* clipNamed(const Sequence& s, const char* name) {
    for (TrackRef r : allTracks(s))
        for (const auto& c : trackAt(s, r)->clips)
            if (c.name == name) return &c;
    return nullptr;
}

}  // namespace

class TestApp : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    std::unique_ptr<MainWindow> win_;

    EditorState* state() { return win_->state(); }
    TimelineWidget* timeline() { return win_->timeline(); }
    QWidget* viewport() { return timeline()->viewport(); }

    // Viewport position of a timeline frame on a track row (row geometry
    // is found by probing, so the test does not depend on layout constants).
    QPoint pointFor(FrameTime f, TrackRef track) {
        const Sequence* s = state()->sequence();
        int y = -1;
        // Rows: video tracks top-down from the highest index, after a 30 px ruler.
        int top = 30 - timeline()->verticalScrollBar()->value();
        int vh = 62, ah = 52;
        if (track.kind == TrackKind::Video) {
            y = top + (int(s->videoTracks.size()) - 1 - track.index) * vh + vh / 2 + 6;
        } else {
            y = top + int(s->videoTracks.size()) * vh + 8 + track.index * ah + ah / 2 + 6;
        }
        double ppf = ppf_;
        int x = 176 + int(f * ppf) - timeline()->horizontalScrollBar()->value();
        return {x, y};
    }
    double ppf_ = 4.0;  // pixels per frame after zoom-to-fit

    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QTest::mousePress(viewport(), Qt::LeftButton, mods, from);
        QPoint mid = (from + to) / 2;
        QMouseEvent m1(QEvent::MouseMove, mid, viewport()->mapToGlobal(mid), Qt::NoButton, Qt::LeftButton, mods);
        QApplication::sendEvent(viewport(), &m1);
        QMouseEvent m2(QEvent::MouseMove, to, viewport()->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, mods);
        QApplication::sendEvent(viewport(), &m2);
        QTest::mouseRelease(viewport(), Qt::LeftButton, mods, to);
    }

    void loadDemo() {
        std::string path = (dir_.path() + "/demo.montage").toStdString();
        QVERIFY(saveProject(demoProject(), path));
        QVERIFY(win_->openProject(QString::fromStdString(path)));
        ppf_ = measurePpf();
    }

    // Zooms to fit and returns the resulting pixels per frame.
    double measurePpf() {
        timeline()->zoomToFit();
        // zoomToFit maps max(duration, 300) frames to (viewport width - 176 - 30).
        const Sequence* s = state()->sequence();
        FrameTime dur = std::max<FrameTime>(s->duration(), FrameTime(s->fpsValue() * 10));
        int vw = std::max(100, viewport()->width() - 176 - 30);
        return double(vw) / double(dur);
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        win_ = std::make_unique<MainWindow>();
        win_->resize(1600, 1000);
        win_->show();
        QVERIFY(QTest::qWaitForWindowExposed(win_.get()));
    }

    void cleanupTestCase() {
        // Avoid the "save changes?" prompt.
        state()->newProject();
        win_.reset();
    }

    void openAndRenderProgram() {
        loadDemo();
        QCOMPARE(state()->sequence()->duration(), FrameTime(120));
        // The program monitor renders a frame for the playhead.
        QSignalSpy spy(win_->findChild<PlaybackController*>(), &PlaybackController::frameRendered);
        state()->setPlayhead(30);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0 || !win_->findChildren<PlaybackController*>().isEmpty(), 3000);
    }

    void moveClipWithMouse() {
        loadDemo();
        const Clip* blue = clipNamed(*state()->sequence(), "Blue");
        QVERIFY(blue);
        Id blueId = blue->id;
        // Drag the blue clip 20 frames later (no snapping targets nearby).
        state()->setSnapping(false);
        QPoint from = pointFor(80, V1);
        QPoint to = from + QPoint(int(20 * ppf_), 0);
        drag(from, to);
        const Clip* moved = edit::clipById(*state()->sequence(), blueId);
        QVERIFY(moved);
        QVERIFY2(std::llabs(moved->start - 80) <= 1, qPrintable(QString("start %1").arg(moved->start)));
        QVERIFY(state()->isSelected(blueId));
        // One undo step restores it.
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), blueId)->start, FrameTime(60));
        state()->redo();
        QVERIFY(std::llabs(edit::clipById(*state()->sequence(), blueId)->start - 80) <= 1);
        state()->setSnapping(true);
    }

    void gradualDragAcrossTracks() {
        loadDemo();
        state()->setSnapping(false);
        Id blue = clipNamed(*state()->sequence(), "Blue")->id;
        QPoint from = pointFor(80, V1);
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, from);
        // Move up one track at a time (V1 -> V2 -> V3) and a few frames right.
        for (int step = 1; step <= 12; ++step) {
            QPoint p = from + QPoint(step * 2, -step * 62 * 2 / 12);
            QMouseEvent mv(QEvent::MouseMove, p, viewport()->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport(), &mv);
        }
        QPoint end = from + QPoint(24, -124);
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, end);
        auto loc = edit::locate(*state()->sequence(), blue);
        QVERIFY(loc);
        QCOMPARE(loc->track.index, 2);  // reached V3
        state()->setSnapping(true);
    }

    void trimEdgeWithMouse() {
        loadDemo();
        state()->setSnapping(false);
        Id red = clipNamed(*state()->sequence(), "Red")->id;
        // Pull the red clip's out-point 15 frames to the left.
        QPoint edge = pointFor(60, V1) - QPoint(2, 0);
        drag(edge, edge - QPoint(int(15 * ppf_), 0));
        const Clip* c = edit::clipById(*state()->sequence(), red);
        QVERIFY2(std::llabs(c->end() - 45) <= 1, qPrintable(QString("end %1").arg(c->end())));
        // Ripple tool closes the gap instead.
        state()->undo();
        timeline()->setTool(TimelineWidget::Tool::Ripple);
        drag(edge, edge - QPoint(int(15 * ppf_), 0));
        c = edit::clipById(*state()->sequence(), red);
        const Clip* blue = clipNamed(*state()->sequence(), "Blue");
        QVERIFY(std::llabs(c->end() - 45) <= 1);
        QCOMPARE(blue->start, c->end());
        timeline()->setTool(TimelineWidget::Tool::Select);
        state()->setSnapping(true);
    }

    void twoUpTrimView() {
        loadDemo();
        state()->setSnapping(false);
        QAction* option = win_->findChild<QAction*>("twoUpTrim");
        QVERIFY(option && option->isChecked());
        MonitorPanel* program = nullptr;
        for (MonitorPanel* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        // What a half shows, a little in from its corner (clear of the title in the middle).
        auto colourIn = [&](bool right) {
            const QRectF r = viewer->twoUpRect(right);
            const QImage shot = viewer->grab().toImage();
            const qreal dpr = shot.devicePixelRatio();
            return shot.pixelColor(int((r.left() + r.width() * 0.1) * dpr), int((r.top() + r.height() * 0.15) * dpr));
        };
        auto move = [&](QPoint to) {
            QMouseEvent m(QEvent::MouseMove, to, viewport()->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport(), &m);
        };
        // Ripple-trimming red's end 10 frames earlier: red's new last frame beside blue's first.
        timeline()->setTool(TimelineWidget::Tool::Ripple);
        const QPoint edge = pointFor(60, V1) - QPoint(2, 0);
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, edge);
        move(edge - QPoint(int(5 * ppf_), 0));
        move(edge - QPoint(int(10 * ppf_), 0));
        QTRY_VERIFY(program->trimViewShown() && viewer->twoUp());
        QTRY_VERIFY_WITH_TIMEOUT(colourIn(false).red() > 150 && colourIn(false).blue() < 60, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(colourIn(true).blue() > 150 && colourIn(true).red() < 60, 5000);
        QVERIFY2(viewer->twoUpLabel(false).startsWith("Red") && viewer->twoUpLabel(true).startsWith("Blue"),
                 qPrintable(viewer->twoUpLabel(false) + " | " + viewer->twoUpLabel(true)));
        const FrameTime cut = clipNamed(*state()->sequence(), "Blue")->start;
        QVERIFY(viewer->twoUpLabel(true).endsWith(timecodeString(state()->sequence(), cut)));
        // Letting go brings the picture back.
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, edge - QPoint(int(10 * ppf_), 0));
        QVERIFY(!program->trimViewShown() && !viewer->twoUp());
        // A slip shows the clip's own first and last frames: blue on both sides.
        state()->undo();
        timeline()->setTool(TimelineWidget::Tool::Slip);
        const QPoint body = pointFor(90, V1);
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, body);
        move(body + QPoint(int(4 * ppf_), 0));
        move(body + QPoint(int(8 * ppf_), 0));
        QTRY_VERIFY(viewer->twoUp());
        QTRY_VERIFY_WITH_TIMEOUT(colourIn(false).blue() > 150 && colourIn(true).blue() > 150, 5000);
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, body + QPoint(int(8 * ppf_), 0));
        // Turned off, trimming leaves the picture alone.
        option->trigger();
        QVERIFY(!option->isChecked());
        timeline()->setTool(TimelineWidget::Tool::Ripple);
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, edge);
        move(edge - QPoint(int(5 * ppf_), 0));
        move(edge - QPoint(int(10 * ppf_), 0));
        QVERIFY(!program->trimViewShown() && !viewer->twoUp());
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, edge - QPoint(int(10 * ppf_), 0));
        option->trigger();
        timeline()->setTool(TimelineWidget::Tool::Select);
        state()->setSnapping(true);
    }

    void swapClipsFromTheMenu() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setSelection({red});
        win_->findChild<QAction*>("swapNext")->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), blue)->start, FrameTime(0));
        QCOMPARE(edit::clipById(*state()->sequence(), red)->start, FrameTime(60));
        QVERIFY(state()->isSelected(red));
        // Swapping the same clip back, then one undo step at a time.
        win_->findChild<QAction*>("swapPrevious")->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), red)->start, FrameTime(0));
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), red)->start, FrameTime(60));
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), blue)->start, FrameTime(60));
        // Nothing after the last clip: a message, nothing changed.
        state()->setSelection({blue});
        QVERIFY(!win_->swapClip(true));
        QCOMPARE(edit::clipById(*state()->sequence(), blue)->start, FrameTime(60));
    }

    void chapterMarkersFromTheMenu() {
        loadDemo();
        auto* add = win_->findChild<QAction*>("addChapter");
        QVERIFY(add);
        QCOMPARE(add->shortcut(), QKeySequence("Alt+M"));
        // The export dialog offers chapters only when there are chapter markers, and only for files that hold them.
        auto presetIndex = [](const char* ext) {
            for (size_t i = 0; i < exportPresets().size(); ++i)
                if (exportPresets()[i].extension == ext) return int(i);
            return -1;
        };
        {
            ExportDialog ed(state(), win_.get());
            ed.findChild<QComboBox*>("exportPreset")->setCurrentIndex(presetIndex("mp4"));
            QVERIFY(!ed.findChild<QCheckBox*>("exportChapters")->isEnabled());
        }
        // A new chapter marker, then an ordinary marker made a chapter; the same place again changes nothing.
        state()->setPlayhead(60);
        add->trigger();
        state()->setPlayhead(90);
        QTest::keyClick(win_.get(), Qt::Key_M);
        add->trigger();
        add->trigger();
        const auto& markers = state()->sequence()->markers;
        QCOMPARE(markers.size(), size_t(2));
        QVERIFY(std::all_of(markers.begin(), markers.end(), [](const Marker& m) { return m.chapter; }));
        QCOMPARE(QString::fromStdString(std::find_if(markers.begin(), markers.end(), [](const Marker& m) { return m.t == 60; })->name),
                 QString("Chapter 1"));
        // YouTube's list on the clipboard, timed from In when In and Out are set, with a warning when YouTube would not show it.
        const double fps = state()->sequence()->fpsValue();
        win_->findChild<QAction*>("copyChapters")->trigger();
        QString expected = QString("0:00 Intro\n0:%1 Chapter 1\n0:%2 Marker 2\n").arg(int(60 / fps), 2, 10, QChar('0')).arg(int(90 / fps), 2, 10, QChar('0'));
        QCOMPARE(QGuiApplication::clipboard()->text(), expected);
        QVERIFY(win_->statusBar()->currentMessage().contains("ten seconds"));
        state()->edit("Range", [](Project&, Sequence& s) {
            s.inPoint = 60;
            s.outPoint = 119;
            return true;
        });
        QCOMPARE(win_->copyYoutubeChapters(), QString("0:00 Chapter 1\n0:%1 Marker 2\n").arg(int(30 / fps), 2, 10, QChar('0')));
        // The Sequence Index and the export dialog know them.
        auto* panel = win_->findChild<SequenceIndexPanel*>();
        panel->setFilter("chapter");
        QTRY_COMPARE(panel->rowCount(), 2);
        panel->setFilter("");
        {
            ExportDialog ed(state(), win_.get());
            auto* preset = ed.findChild<QComboBox*>("exportPreset");
            auto* chapters = ed.findChild<QCheckBox*>("exportChapters");
            preset->setCurrentIndex(presetIndex("mp4"));
            QVERIFY(chapters->isEnabled());
            preset->setCurrentIndex(presetIndex("gif"));
            QVERIFY(!chapters->isEnabled());
            // Smart rendering is offered for ProRes and DNxHR only.
            auto* smart = ed.findChild<QCheckBox*>("exportSmartRender");
            QVERIFY(smart && !smart->isEnabled());
            for (size_t i = 0; i < exportPresets().size(); ++i)
                if (exportPresets()[i].settings.videoCodec == "prores_ks") {
                    preset->setCurrentIndex(int(i));
                    break;
                }
            QVERIFY(smart->isEnabled());
        }
        // One undo step each.
        state()->undo();
        state()->undo();
        const auto& restored = state()->sequence()->markers;
        QCOMPARE(std::count_if(restored.begin(), restored.end(), [](const Marker& m) { return m.chapter; }), 1);
    }

    void qualityCheckDialog() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("qualityCheck"));
        QualityCheckDialog dlg(state(), win_.get());
        QCOMPARE(dlg.findChild<QComboBox*>("qcRange")->currentIndex(), 0);  // no In and Out
        auto* markers = dlg.findChild<QPushButton*>("qcMarkers");
        QVERIFY(!markers->isEnabled());
        // The demo has no sound: four seconds of silence, and nothing else at the default limits.
        QVERIFY(dlg.runCheck());
        QCOMPARE(dlg.issues().size(), size_t(1));
        QCOMPARE(dlg.issues()[0].kind, QcKind::Silence);
        auto* table = dlg.findChild<QTableWidget*>("qcIssues");
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(table->item(0, 2)->text().contains("Silence"));
        QCOMPARE(table->item(0, 0)->text(), QString::fromStdString(formatTimecode(0, state()->sequence()->fps)));
        QVERIFY(markers->isEnabled());
        dlg.findChild<QCheckBox*>("qcSilence")->setChecked(false);
        QVERIFY(dlg.runCheck());
        QVERIFY(dlg.issues().empty());
        QVERIFY(dlg.findChild<QLabel*>("qcSummary")->text().contains("No problems"));
        QVERIFY(!markers->isEnabled());
        // A one-second freeze limit finds the stills; a problem can be gone to and marked as one undo step.
        dlg.findChild<QDoubleSpinBox*>("qcFreezeSeconds")->setValue(1.0);
        QVERIFY(dlg.runCheck());
        const auto& found = dlg.issues();
        const auto blue = std::find_if(found.begin(), found.end(), [](const QcIssue& i) { return i.kind == QcKind::Freeze && i.start == 60; });
        QVERIFY(blue != found.end());
        QCOMPARE(blue->end, FrameTime(120));
        state()->setPlayhead(0);
        dlg.activate(int(blue - found.begin()));
        QCOMPARE(state()->playhead(), FrameTime(60));
        const size_t before = state()->sequence()->markers.size();
        QCOMPARE(dlg.addMarkers(), int(found.size()));
        QCOMPARE(state()->sequence()->markers.size(), before + found.size());
        state()->undo();
        QCOMPARE(state()->sequence()->markers.size(), before);
        // In to Out when both are set.
        state()->edit("Range", [](Project&, Sequence& s) {
            s.inPoint = 60;
            s.outPoint = 119;
            return true;
        });
        QualityCheckDialog ranged(state(), win_.get());
        QCOMPARE(ranged.findChild<QComboBox*>("qcRange")->currentIndex(), 1);
        ranged.findChild<QCheckBox*>("qcSilence")->setChecked(false);
        ranged.findChild<QDoubleSpinBox*>("qcFreezeSeconds")->setValue(1.0);
        QVERIFY(ranged.runCheck());
        QCOMPARE(ranged.issues().size(), size_t(1));
        QVERIFY(ranged.issues()[0].start == 60 && ranged.issues()[0].end == 120);
    }

    void mixerAutomation() {
        loadDemo();
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        QTRY_VERIFY(mixer->trackFader(0));
        QSlider* fader = mixer->trackFader(0);
        QComboBox* mode = mixer->trackAutomationMode(0);
        auto a1 = [&]() -> const Track& { return state()->sequence()->audioTracks.at(0); };
        QCOMPARE(mode->currentIndex(), int(AutomationMode::Read));
        mode->setCurrentIndex(int(AutomationMode::Write));
        QCOMPARE(a1().automation, int(AutomationMode::Write));
        // A Write pass: 0 dB, then -12 dB from frame 20. The mix hears the fader while it writes.
        mixer->playbackStarted(0);
        QVERIFY(mixer->recordingAutomation());
        QVERIFY(!mode->isEnabled());
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 20) fader->setValue(-120);
            mixer->playbackPosition(f);
            if (f == 30) QCOMPARE(a1().volumeDb, -12.0);
        }
        mixer->playbackStopped(60);
        QVERIFY(!mixer->recordingAutomation());
        QVERIFY(a1().volumeAuto.animated());
        QCOMPARE(a1().volumeAuto.at(10), 0.0);
        QCOMPARE(a1().volumeAuto.at(40), -12.0);
        QCOMPARE(a1().volumeAuto.at(61), 0.0);
        QCOMPARE(a1().volumeDb, 0.0);          // the fader's own level is left alone
        QVERIFY(!a1().panAuto.animated());     // the pan was not moved
        QCOMPARE(a1().automation, int(AutomationMode::Touch));  // Write hands over to Touch
        QCOMPARE(mode->currentIndex(), int(AutomationMode::Touch));
        QVERIFY(mode->isEnabled());
        // The fader follows the automation at the playhead.
        state()->setPlayhead(40);
        QCOMPARE(fader->value(), -120);
        state()->setPlayhead(10);
        QCOMPARE(fader->value(), 0);
        // One undo step.
        state()->undo();
        QVERIFY(!a1().volumeAuto.animated());
        QCOMPARE(a1().automation, int(AutomationMode::Write));
        state()->redo();
        QCOMPARE(a1().volumeAuto.at(40), -12.0);
        // A Touch pass: held at -3 dB over frames 5-14, then gliding back to the -12 dB written before.
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 5) {
                fader->setSliderDown(true);
                fader->setValue(-30);
            }
            if (f == 15) fader->setSliderDown(false);
            mixer->playbackPosition(f);
            if (f == 50) QCOMPARE(fader->value(), -120);  // let go: back to what is there
        }
        mixer->playbackStopped(60);
        QCOMPARE(a1().volumeAuto.at(10), -3.0);
        QVERIFY(a1().volumeAuto.at(30) < -3.0 && a1().volumeAuto.at(30) > -12.0);
        QCOMPARE(a1().volumeAuto.at(50), -12.0);
        QCOMPARE(a1().volumeAuto.at(2), 0.0);
        // Stopped, on a track reading its lane, the fader sets the lane at the playhead.
        mode->setCurrentIndex(int(AutomationMode::Read));
        state()->setPlayhead(50);
        fader->setValue(-60);
        QVERIFY(a1().volumeAuto.keyAt(50));
        QCOMPARE(a1().volumeAuto.keyAt(50)->v, -6.0);
        QCOMPARE(a1().volumeDb, 0.0);
        // Nothing armed: playing writes nothing.
        const size_t keys = a1().volumeAuto.keys.size();
        mixer->playbackStarted(0);
        QVERIFY(!mixer->recordingAutomation());
        mixer->playbackStopped(30);
        QCOMPARE(a1().volumeAuto.keys.size(), keys);
    }

    void trackAutomationOnTheTimeline() {
        loadDemo();
        auto* toggle = win_->findChild<QAction*>("showTrackAutomation");
        QVERIFY(toggle);
        if (!toggle->isChecked()) toggle->trigger();
        QVERIFY(timeline()->showTrackAutomation());
        ppf_ = measurePpf();
        auto a1 = [&]() -> const Track& { return state()->sequence()->audioTracks.at(0); };
        auto lane = [&](FrameTime f) { return timeline()->trackLanePoint(0, f); };
        QVERIFY(lane(30).x() > 0);
        // Without points the line is the fader's level: dragging it moves the fader, as one undo step.
        drag(lane(30), lane(30) + QPoint(0, 10));
        const double lowered = a1().volumeDb;
        QVERIFY2(lowered < -1, qPrintable(QString::number(lowered)));
        QVERIFY(!a1().volumeAuto.animated());
        state()->undo();
        QCOMPARE(a1().volumeDb, 0.0);
        // Ctrl/Cmd-click adds points at the line's level; a point drags in time and value.
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::ControlModifier, lane(30));
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::ControlModifier, lane(90));
        QCOMPARE(a1().volumeAuto.keys.size(), size_t(2));
        QVERIFY(std::abs(a1().volumeAuto.keys[0].t - 30) <= 1 && std::abs(a1().volumeAuto.keys[1].t - 90) <= 1);
        QCOMPARE(a1().volumeAuto.keys[1].v, 0.0);
        const FrameTime k1 = a1().volumeAuto.keys[1].t;
        drag(lane(k1), lane(k1) + QPoint(0, 14));
        QVERIFY2(a1().volumeAuto.keys[1].v < -1, qPrintable(QString::number(a1().volumeAuto.keys[1].v)));
        QVERIFY(std::abs(a1().volumeAuto.keys[1].t - k1) <= 1);
        // The mixer's fader follows the line at the playhead.
        auto* mixer = win_->findChild<MixerPanel*>();
        state()->setPlayhead(a1().volumeAuto.keys[1].t);
        QCOMPARE(mixer->trackFader(0)->value(), int(std::lround(a1().volumeAuto.keys[1].v * 10)));
        // Dragging the line between the points moves both.
        const double k0v = a1().volumeAuto.keys[0].v;
        drag(lane(60), lane(60) + QPoint(0, -12));
        QVERIFY(a1().volumeAuto.keys[0].v > k0v);
        // Alt-click deletes a point.
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::AltModifier, lane(a1().volumeAuto.keys[0].t));
        QCOMPARE(a1().volumeAuto.keys.size(), size_t(1));
        // Off: the clip lines come back and the track line is not drawn.
        toggle->trigger();
        QVERIFY(!timeline()->showTrackAutomation());
        QCOMPARE(lane(30), QPoint(-1, -1));
    }

    void exportLutFromGrade() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("exportLut"));
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->edit("Grade", [red](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            Effect e = makeEffect(p, "color_correct");
            e.params["saturation"] = 0.0;
            c->effects.push_back(e);
            return true;
        });
        state()->setSelection({red});
        state()->setPlayhead(30);
        const QString cube = dir_.path() + "/red.cube";
        QVERIFY(win_->exportClipLut(cube, 9));
        QVERIFY(win_->statusBar()->currentMessage().contains("red.cube"));
        std::string err;
        const auto lut = loadCubeLut(cube.toStdString(), &err);
        QVERIFY2(lut && lut->size == 9, err.c_str());
        float r = 0.8f, g = 0, b = 0;
        lut->apply(r, g, b);
        QVERIFY(std::fabs(r - g) < 1e-3f && std::fabs(g - b) < 1e-3f);
        // A spatial effect is named as left out.
        state()->edit("Blur", [red](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects.push_back(makeEffect(p, "gaussian_blur"));
            return true;
        });
        QVERIFY(win_->exportClipLut(cube, 9));
        QVERIFY2(win_->statusBar()->currentMessage().contains(QString::fromStdString(findEffectInfo("gaussian_blur")->displayName)),
                 qPrintable(win_->statusBar()->currentMessage()));
        // Nothing under the playhead: nothing written.
        state()->clearSelection();
        state()->setPlayhead(5000);
        QVERIFY(!win_->exportClipLut(dir_.path() + "/none.cube"));
        QVERIFY(!QFileInfo::exists(dir_.path() + "/none.cube"));
    }

    void clipMarkersFromTheMenu() {
        loadDemo();
        auto* add = win_->findChild<QAction*>("addClipMarker");
        QVERIFY(add);
        QCOMPARE(add->shortcut(), QKeySequence("Shift+Alt+M"));
        const Id blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setSelection({blue});
        state()->setPlayhead(75);
        add->trigger();
        auto markers = [&]() -> const std::vector<Marker>& { return edit::clipById(*state()->sequence(), blue)->markers; };
        QCOMPARE(markers().size(), size_t(1));
        QCOMPARE(markers()[0].t, FrameTime(15));  // 15 frames into Blue's source
        QCOMPARE(QString::fromStdString(markers()[0].name), QString("Marker 1"));
        QVERIFY(state()->sequence()->markers.empty());  // not a sequence marker
        // The Sequence Index lists it where it shows, and renames it.
        auto* panel = win_->findChild<SequenceIndexPanel*>();
        panel->setFilter("clip marker");
        QTRY_COMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Start), QString::fromStdString(formatTimecode(75, state()->sequence()->fps)));
        QVERIFY(panel->rename(0, "Hit"));
        QCOMPARE(QString::fromStdString(markers()[0].name), QString("Hit"));
        panel->setFilter("");
        // Undone one step at a time.
        state()->undo();
        QCOMPARE(QString::fromStdString(markers()[0].name), QString("Marker 1"));
        state()->undo();
        QVERIFY(markers().empty());
        // Nothing under the playhead: a message and no marker.
        state()->clearSelection();
        state()->setPlayhead(5000);
        add->trigger();
        QVERIFY(win_->statusBar()->currentMessage().contains("Select a clip"));
    }

    void exposureChecks() {
        // False colour by brightness (Rec.709 luma of what is shown).
        QCOMPARE(falseColorFor(0.01), QColor(130, 40, 170));
        QCOMPARE(falseColorFor(0.03), QColor(40, 90, 230));
        QCOMPARE(falseColorFor(0.41), QColor(70, 190, 70));
        QCOMPARE(falseColorFor(0.54), QColor(240, 130, 190));
        QCOMPARE(falseColorFor(0.98), QColor(250, 230, 40));
        QCOMPARE(falseColorFor(1.0), QColor(230, 30, 30));
        QCOMPARE(falseColorFor(0.7), QColor(179, 179, 179));
        // Zebras: only where it is that bright, striped dark and light.
        QImage img(64, 32, QImage::Format_RGB32);
        img.fill(qRgb(128, 128, 128));
        for (int y = 0; y < 32; ++y)
            for (int x = 32; x < 64; ++x) img.setPixel(x, y, qRgb(255, 255, 255));
        const QImage z = exposureView(img, ExposureView::Zebras100);
        int dark = 0, untouched = 0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) {
                if (x < 32) untouched += z.pixel(x, y) == img.pixel(x, y);
                else dark += qRed(z.pixel(x, y)) < 50;
            }
        QCOMPARE(untouched, 32 * 32);
        QVERIFY(dark > 32 * 32 / 3 && dark < 2 * 32 * 32 / 3);
        // Skin zebras catch 65-75 %, not white.
        img.fill(qRgb(178, 178, 178));  // 70 %
        QVERIFY(exposureView(img, ExposureView::Zebras70) != img);
        img.fill(qRgb(255, 255, 255));
        QCOMPARE(exposureView(img, ExposureView::Zebras70).pixel(5, 5), img.pixel(5, 5));
        // On the Program monitor: the shown picture changes, the rendered frame does not.
        loadDemo();
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        auto* combo = program->findChild<QComboBox*>("exposureView");
        QVERIFY(combo && combo->count() == 4);
        state()->setPlayhead(10);
        QTRY_VERIFY(!program->viewer()->image().isNull());
        const QImage frame = program->viewer()->image();
        combo->setCurrentIndex(3);
        QCOMPARE(program->viewer()->exposureView(), int(ExposureView::FalseColor));
        const QImage shown = program->viewer()->shownImage();
        QCOMPARE(shown, exposureView(frame, ExposureView::FalseColor));
        QVERIFY(shown != frame.convertToFormat(QImage::Format_RGB32));
        QCOMPARE(program->viewer()->image(), frame);
        combo->setCurrentIndex(0);
        QCOMPARE(program->viewer()->shownImage(), program->viewer()->image());
    }

    void sequenceTabs() {
        loadDemo();
        auto* tabs = win_->findChild<QTabBar*>("sequenceTabs");
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->count(), 1);
        const Id first = state()->project().activeSequence;
        QCOMPARE(tabs->tabText(0), QString::fromStdString(state()->sequence()->name));
        QVERIFY(!tabs->tabsClosable());  // the only one stays
        // A new sequence opens in its own tab; clicking a tab switches to it.
        const Id second = state()->newSequence("Second", 320, 180, Rational{30, 1});
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(tabs->tabText(1), QString("Second"));
        tabs->setCurrentIndex(0);
        QCOMPARE(state()->project().activeSequence, first);
        state()->edit("Rename", [](Project&, Sequence& s) {
            s.name = "Main cut";
            return true;
        });
        QCOMPARE(tabs->tabText(0), QString("Main cut"));
        // Closing the active tab moves to its neighbour; the last one cannot be closed.
        emit tabs->tabCloseRequested(0);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(state()->project().activeSequence, second);
        emit tabs->tabCloseRequested(0);
        QCOMPARE(tabs->count(), 1);
        // Opening the first again (as from the bin) brings its tab back.
        state()->setActiveSequence(first);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(tabs->tabText(1), QString("Main cut"));
    }

    void markerListsFromTheMenu() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("exportMarkers") && win_->findChild<QAction*>("importMarkers"));
        state()->edit("Markers", [](Project&, Sequence& s) {
            edit::addMarker(s, Marker{10, 0, "One", "first", 0});
            edit::addMarker(s, Marker{40, 5, "Two", "", 6});
            return true;
        });
        const QString csv = dir_.path() + "/markers.csv";
        QVERIFY(win_->exportMarkers(csv));
        QVERIFY(win_->exportMarkers(dir_.path() + "/markers.txt"));
        QVERIFY(win_->exportMarkers(dir_.path() + "/markers.edl"));
        QFile edl(dir_.path() + "/markers.edl");
        QVERIFY(edl.open(QIODevice::ReadOnly) && edl.readAll().contains("|M:Two"));
        const auto saved = state()->sequence()->markers;
        state()->edit("Clear", [](Project&, Sequence& s) {
            s.markers.clear();
            return true;
        });
        QCOMPARE(win_->importMarkers(csv), 2);
        QCOMPARE(state()->sequence()->markers, saved);
        state()->undo();
        QVERIFY(state()->sequence()->markers.empty());
        QCOMPARE(win_->importMarkers(dir_.path() + "/missing.csv"), 0);
    }

    void projectManagerDialog() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("projectManager"));
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        }));
        // The dialog's choices become the options.
        ProjectManagerDialog dlg(state(), win_.get());
        dlg.findChild<QLineEdit*>("pmFolder")->setText(dir_.path() + "/handoff");
        dlg.findChild<QLineEdit*>("pmName")->setText("Handoff");
        dlg.findChild<QComboBox*>("pmSequences")->setCurrentIndex(1);
        auto* codec = dlg.findChild<QComboBox*>("pmCodec");
        QVERIFY(!codec->isEnabled());  // only when consolidating
        dlg.findChild<QComboBox*>("pmMode")->setCurrentIndex(1);
        QVERIFY(codec->isEnabled());
        dlg.findChild<QComboBox*>("pmMode")->setCurrentIndex(0);
        const ConsolidateOptions o = dlg.options();
        QCOMPARE(QString::fromStdString(o.name), QString("Handoff"));
        QCOMPARE(o.sequences, std::vector<Id>{state()->sequence()->id});
        QVERIFY(!o.trim);
        // Collect: the sound is copied beside the new project, which opens.
        QVERIFY(win_->runProjectManager(o));
        QVERIFY(QFileInfo::exists(dir_.path() + "/handoff/Handoff.montage"));
        QVERIFY(QFileInfo::exists(dir_.path() + "/handoff/Media/jfk.wav"));
        QVERIFY(win_->statusBar()->currentMessage().contains("1 file"));
        Project copy;
        QVERIFY(loadProject((dir_.path() + "/handoff/Handoff.montage").toStdString(), copy));
        QCOMPARE(copy.media.size(), size_t(1));
        QVERIFY(QString::fromStdString(copy.media[0].path).endsWith("handoff/Media/jfk.wav"));
        QVERIFY(!win_->runProjectManager(ConsolidateOptions{}));  // no folder
    }

    void trimMode() {
        loadDemo();
        auto act = [&](const char* name) {
            auto* a = win_->findChild<QAction*>(name);
            QVERIFY2(a, name);
            a->trigger();
        };
        auto clip = [&](const char* name) { return clipNamed(*state()->sequence(), name); };
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        auto* twoUp = win_->findChild<QAction*>("twoUpTrim");
        QVERIFY(program && twoUp);
        if (!twoUp->isChecked()) twoUp->trigger();
        // Without an edit selected, trimming says how to start.
        act("trimForward");
        QVERIFY(win_->statusBar()->currentMessage().contains("Shift+T"));
        // The edit nearest the playhead on V1: Red into Blue at 60, both sides (a roll).
        state()->setPlayhead(55);
        act("selectEdit");
        QVERIFY(win_->trimEdit());
        QCOMPARE(win_->trimEdit()->side, 0);
        QCOMPARE(state()->playhead(), FrameTime(60));
        QVERIFY(timeline()->trimEditShown());
        QVERIFY(program->trimViewShown());
        act("trimForward");
        QCOMPARE(clip("Red")->end(), FrameTime(61));
        QCOMPARE(clip("Blue")->start, FrameTime(61));
        QCOMPARE(state()->playhead(), FrameTime(61));
        // The outgoing side, five frames: a ripple that pushes Blue along.
        act("cycleTrimSide");
        QCOMPARE(win_->trimEdit()->side, 1);
        act("trimForward5");
        QCOMPARE(clip("Red")->end(), FrameTime(66));
        QCOMPARE(clip("Blue")->start, FrameTime(66));
        QCOMPARE(clip("Blue")->duration, FrameTime(59));
        // The incoming side, a frame back: Blue gets a frame longer at its head.
        act("cycleTrimSide");
        QCOMPARE(win_->trimEdit()->side, 2);
        act("trimBackward");
        QCOMPARE(clip("Blue")->duration, FrameTime(60));
        QCOMPARE(clip("Red")->end(), clip("Blue")->start);
        // One undo step each.
        state()->undo();
        QCOMPARE(clip("Blue")->duration, FrameTime(59));
        // Esc ends trim mode.
        act("endTrim");
        QVERIFY(!win_->trimEdit());
        QVERIFY(!timeline()->trimEditShown());
        QVERIFY(!program->trimViewShown());
        // At the end of the track there is only an outgoing side.
        state()->setPlayhead(200);
        act("selectEdit");
        QVERIFY(win_->trimEdit());
        QCOMPARE(win_->trimEdit()->incoming, Id(0));
        QCOMPARE(win_->trimEdit()->side, 1);
        act("cycleTrimSide");
        QCOMPARE(win_->trimEdit()->side, 1);
        act("endTrim");
    }

    void razorTool() {
        loadDemo();
        timeline()->setTool(TimelineWidget::Tool::Razor);
        state()->setSnapping(false);
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::NoModifier, pointFor(30, V1));
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.size(), size_t(3));
        timeline()->setTool(TimelineWidget::Tool::Select);
        state()->setSnapping(true);
    }

    void rubberBandSelection() {
        loadDemo();
        state()->clearSelection();
        // Drag from empty space on V3 down across V2 and V1.
        QPoint a = pointFor(110, {TrackKind::Video, 2});
        QPoint b = pointFor(10, V1);
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, a);
        QMouseEvent mv(QEvent::MouseMove, b, viewport()->mapToGlobal(b), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &mv);
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, b);
        QVERIFY(state()->selectedClips().size() >= 2);
    }

    void keyboardEditing() {
        loadDemo();
        state()->setPlayhead(30);
        // Ctrl+K cuts on the targeted tracks; Ctrl+Z undoes it.
        timeline()->setFocus();
        QTest::keyClick(win_.get(), Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.size(), size_t(3));
        QTest::keyClick(win_.get(), Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.size(), size_t(2));
        // Select the red clip, ripple delete with Shift+Delete.
        state()->setSelection({clipNamed(*state()->sequence(), "Red")->id});
        QTest::keyClick(win_.get(), Qt::Key_Delete, Qt::ShiftModifier);
        QCOMPARE(clipNamed(*state()->sequence(), "Blue")->start, FrameTime(0));
        // I / O marks and markers.
        state()->setPlayhead(10);
        QTest::keyClick(win_.get(), Qt::Key_I);
        state()->setPlayhead(20);
        QTest::keyClick(win_.get(), Qt::Key_O);
        QCOMPARE(state()->sequence()->inPoint, FrameTime(10));
        QCOMPARE(state()->sequence()->outPoint, FrameTime(20));
        QTest::keyClick(win_.get(), Qt::Key_M);
        QCOMPARE(state()->sequence()->markers.size(), size_t(1));
        // Arrow keys step the playhead; Down jumps to the next edit.
        QTest::keyClick(win_.get(), Qt::Key_Right);
        QCOMPARE(state()->playhead(), FrameTime(21));
        QTest::keyClick(win_.get(), Qt::Key_Home);
        QCOMPARE(state()->playhead(), FrameTime(0));
        QTest::keyClick(win_.get(), Qt::Key_Down);
        QVERIFY(state()->playhead() > 0);
        // Tool shortcuts.
        QTest::keyClick(win_.get(), Qt::Key_C);
        QCOMPARE(timeline()->tool(), TimelineWidget::Tool::Razor);
        QTest::keyClick(win_.get(), Qt::Key_V);
        QCOMPARE(timeline()->tool(), TimelineWidget::Tool::Select);
    }

    void copyPasteViaShortcuts() {
        loadDemo();
        state()->setSelection({clipNamed(*state()->sequence(), "Red")->id});
        QTest::keyClick(win_.get(), Qt::Key_C, Qt::ControlModifier);
        state()->setPlayhead(200);
        QTest::keyClick(win_.get(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.size(), size_t(3));
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.back().start, FrameTime(200));
    }

    void dropEffectAndTransition() {
        loadDemo();
        Id red = clipNamed(*state()->sequence(), "Red")->id;
        auto dropMime = [&](const QString& type, QPoint pos) {
            QMimeData mime;
            mime.setData("application/x-montage-effect", type.toUtf8());
            QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport(), &enter);
            QDragMoveEvent move(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport(), &move);
            QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport(), &drop);
        };
        dropMime("gaussian_blur", pointFor(20, V1));
        QCOMPARE(edit::clipById(*state()->sequence(), red)->effects.size(), size_t(1));
        dropMime("cross_dissolve", pointFor(55, V1));  // near the cut between red and blue
        QCOMPARE(trackAt(*state()->sequence(), V1)->transitions.size(), size_t(1));
        // Dropping a generator creates a clip.
        dropMime("bars", pointFor(130, V1));
        QVERIFY(trackAt(*state()->sequence(), V1)->clips.size() >= 3);
        // Effects are undoable.
        state()->undo();
        state()->undo();
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), red)->effects.size(), size_t(0));
    }

    void audioPluginFromBrowserToInspector() {
        // Register the test CLAP plugins (built with the tests).
        plugins::Registry& reg = plugins::Registry::instance();
        reg.setCachePath((dir_.path() + "/plugin-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        for (plugins::Format f : plugins::kAllFormats) reg.setSearchPaths(f, {"/nonexistent-montage-test-dir"});
        reg.setSearchPaths(plugins::Format::Clap, {QStringLiteral(MONTAGE_TEST_CLAP_DIR "/good").toStdString()});
        reg.scan();
        auto* browser = win_->findChild<EffectsBrowser*>();
        QVERIFY(browser);
        browser->reload();
        bool listed = false;
        for (QTreeWidgetItem* item : browser->findChild<QTreeWidget*>()->findItems("Montage Test Gain", Qt::MatchRecursive))
            listed |= item->data(0, Qt::UserRole).toString() == "plugin:clap:org.montage.test.gain";
        QVERIFY(listed);

        // An audio clip on A1.
        const QString wav = dir_.path() + "/tone.wav";
        {
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int frames = 48000 * 2;
            QByteArray data;
            QDataStream out(&data, QIODevice::WriteOnly);
            out.setByteOrder(QDataStream::LittleEndian);
            out.writeRawData("RIFF", 4);
            out << quint32(36 + frames * 4);
            out.writeRawData("WAVEfmt ", 8);
            out << quint32(16) << quint16(1) << quint16(2) << quint32(48000) << quint32(48000 * 4) << quint16(4) << quint16(16);
            out.writeRawData("data", 4);
            out << quint32(frames * 4);
            for (int i = 0; i < frames * 2; ++i) out << qint16(8000);
            f.write(data);
        }
        state()->newProject();
        auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false);
        }));
        ppf_ = measurePpf();
        Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;

        // Drag the plugin from the browser onto the clip.
        QMimeData mime;
        mime.setData("application/x-montage-effect", "plugin:clap:org.montage.test.gain");
        const QPoint pos = pointFor(10, A1);
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &enter);
        QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &drop);
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->effects.size(), size_t(1));
        QCOMPARE(c->effects[0].type, std::string("plugin"));

        // The inspector shows the plugin's Gain parameter (0..2); editing it is undoable.
        state()->setSelection({clip}, false);
        QApplication::processEvents();
        QDoubleSpinBox* gain = nullptr;
        for (auto* sp : win_->findChildren<QDoubleSpinBox*>())
            if (sp->maximum() == 2 && sp->minimum() == 0 && sp->isVisibleTo(win_.get())) gain = sp;
        QVERIFY(gain);
        gain->setValue(0.5);
        c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->effects[0].p("param.7", 0), 0.5);
        state()->undo();
        c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->effects[0].p("param.7", 0), 1.0);
        state()->newProject();
    }

    void pluginEditorWindow() {
        // Runs after audioPluginFromBrowserToInspector, which registered the test CLAP plugins.
        auto d = plugins::Registry::instance().find("clap:org.montage.test.gain");
        QVERIFY(d.has_value());
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false);
        }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        Id eid = 0;
        QVERIFY(state()->edit("Add Plugin", [&](Project& p, Sequence& s) {
            auto e = plugins::makePluginEffect(p, *d);
            if (!e) return false;
            eid = e->id;
            edit::clipById(s, clip)->effects.push_back(*e);
            return true;
        }));
        state()->setSelection({clip}, false);
        state()->setPlayhead(10);
        QApplication::processEvents();

        // The Inspector's Editor button opens the plugin's editor in a window.
        QToolButton* button = nullptr;
        for (auto* b : win_->findChildren<QToolButton*>("pluginEditor"))
            if (b->isVisibleTo(win_.get())) button = b;
        QVERIFY(button);
        button->click();
        PluginEditorWindow* editor = PluginEditorWindow::find(clip, eid);
        QVERIFY(editor);
        QVERIFY(editor->isVisible());
        QCOMPARE(PluginEditorWindow::open(state(), clip, eid, win_.get()), editor);  // raised, not opened twice

        // The test plugin's editor turns its gain knob to 0.25 when shown: that becomes an edit of the effect.
        auto gain = [&] { return edit::clipById(*state()->sequence(), clip)->effects.at(0).p("param.7", 10, -1); };
        QTRY_COMPARE_WITH_TIMEOUT(gain(), 0.25, 3000);
        // ...and the editor asked to be 400 x 250.
        QTRY_VERIFY_WITH_TIMEOUT(editor->width() >= 400 && editor->height() >= 250, 2000);

        // Undo puts the knob back in the editor too.
        state()->undo();
        QCOMPARE(gain(), 1.0);
        QCOMPARE(editor->instance()->parameter(7), 1.0);

        // Closing stores the plugin's settings in the effect.
        editor->close();
        QApplication::processEvents();
        QVERIFY(!PluginEditorWindow::find(clip, eid));
        const std::string bytes = plugins::decodeState(edit::clipById(*state()->sequence(), clip)->effects.at(0).s("state"));
        QCOMPARE(bytes.size(), sizeof(double));
        double saved = 0;
        std::memcpy(&saved, bytes.data(), sizeof saved);
        QCOMPARE(saved, 1.0);

        // Deleting the clip closes an open editor.
        button = nullptr;
        for (auto* b : win_->findChildren<QToolButton*>("pluginEditor"))
            if (b->isVisibleTo(win_.get())) button = b;
        QVERIFY(button);
        button->click();
        QVERIFY(PluginEditorWindow::find(clip, eid));
        QVERIFY(state()->apply("Delete", [clip](Project& p, Sequence& s) { return edit::removeClips(p, s, {clip}, true); }));
        QApplication::processEvents();
        QVERIFY(!PluginEditorWindow::find(clip, eid));
        state()->newProject();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void mixerEffectsAndBuses() {
        loadDemo();
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        const Id a1 = state()->sequence()->audioTracks.at(0).id;
        // FX on the first strip puts its inserts in the Inspector.
        QToolButton* fx = nullptr;
        for (auto* b : mixer->findChildren<QToolButton*>("fxButton"))
            if (!fx && b->isVisibleTo(mixer)) fx = b;
        QVERIFY(fx);
        fx->click();
        QCOMPARE(state()->inspectedChain(), a1);
        QApplication::processEvents();
        // Add a limiter from the Inspector's Add menu.
        QPushButton* add = nullptr;
        for (auto* b : win_->findChildren<QPushButton*>())
            if (b->text().startsWith("Add Audio Effect") && b->isVisibleTo(win_.get())) add = b;
        QVERIFY(add);
        QAction* limiter = nullptr;
        std::function<void(QMenu*)> findIn = [&](QMenu* m) {
            for (QAction* a : m->actions()) {
                if (a->menu()) findIn(a->menu());
                else if (a->text() == "Limiter") limiter = a;
            }
        };
        findIn(add->menu());
        QVERIFY(limiter);
        limiter->trigger();
        QCOMPARE(state()->sequence()->audioTracks.at(0).effects.size(), size_t(1));
        QApplication::processEvents();
        QCOMPARE(fx->text(), QString("FX 1"));
        // Selecting a clip shows the clip again.
        state()->setSelection({clipNamed(*state()->sequence(), "Red")->id}, false);
        QCOMPARE(state()->inspectedChain(), Id(0));

        // A bus, and routing the track to it.
        auto* addBus = mixer->findChild<QToolButton*>("addBus");
        QVERIFY(addBus);
        addBus->click();
        QCOMPARE(state()->sequence()->buses.size(), size_t(1));
        const Id bus = state()->sequence()->buses[0].id;
        QApplication::processEvents();
        QComboBox* out = nullptr;
        for (auto* c : mixer->findChildren<QComboBox*>("outputCombo"))
            if (!out && c->isVisibleTo(mixer)) out = c;  // rebuilt strips replace the old ones
        QVERIFY(out);
        QCOMPARE(out->count(), 2);
        out->setCurrentIndex(1);
        emit out->activated(1);
        QCOMPARE(state()->sequence()->audioTracks.at(0).output, bus);
        // Master fader.
        auto* master = mixer->findChild<QSlider*>("masterFader");
        QVERIFY(master);
        master->setValue(-60);
        QCOMPARE(state()->sequence()->masterVolumeDb, -6.0);
        // Undo walks it back.
        state()->undo();
        state()->undo();
        QCOMPARE(state()->sequence()->audioTracks.at(0).output, Id(0));
        state()->undo();
        QVERIFY(state()->sequence()->buses.empty());
    }

    void surroundMixerAndExport() {
        loadDemo();
        // Speech on A1 and A2.
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->sequence()->audioTracks.size() >= 2);
        for (int t = 0; t < 2; ++t)
            QVERIFY(state()->apply("Place", [media, t](Project& p, Sequence& s) {
                return edit::placeMedia(p, s, media, 0, 0, 60, V1, {TrackKind::Audio, t}, false);
            }));
        // Sequence settings carry the layout.
        {
            SequenceSettingsDialog dlg(win_.get());
            NewSequenceSpec spec;
            spec.audioLayout = "5.1";
            dlg.setSpec(spec);
            auto* layout = dlg.findChild<QComboBox*>("audioLayout");
            QVERIFY(layout);
            QCOMPARE(layout->currentData().toString(), QString("5.1"));
            layout->setCurrentIndex(layout->findData(QString("7.1")));
            QCOMPARE(dlg.spec().audioLayout, std::string("7.1"));
        }
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        auto visiblePanners = [&] {
            std::vector<SurroundPanner*> v;
            for (auto* p : mixer->findChildren<SurroundPanner*>("surroundPanner"))
                if (p->isVisibleTo(mixer)) v.push_back(p);
            return v;
        };
        QVERIFY(visiblePanners().empty());  // stereo: pan dials
        QVERIFY(state()->edit("5.1", [](Project&, Sequence& s) {
            s.audioLayout = "5.1";
            return true;
        }));
        QApplication::processEvents();
        const size_t tracks = state()->sequence()->audioTracks.size();
        QCOMPARE(visiblePanners().size(), tracks);
        // Drag the first track's sound round to the back left: one undo step.
        SurroundPanner* panner = visiblePanners().front();
        const QPointF front = panner->toWidget(0, 1), back = panner->toWidget(-0.7, -0.7);
        QTest::mousePress(panner, Qt::LeftButton, {}, front.toPoint());
        QTest::mouseMove(panner, ((front + back) / 2).toPoint());
        QTest::mouseMove(panner, back.toPoint());
        QTest::mouseRelease(panner, Qt::LeftButton, {}, back.toPoint());
        const SurroundPan placed = state()->sequence()->audioTracks.at(0).surround;
        QVERIFY2(placed.x < -0.5 && placed.y < -0.5, qPrintable(QString("%1 %2").arg(placed.x).arg(placed.y)));
        // The wheel narrows it.
        QWheelEvent wheel(panner->rect().center(), panner->mapToGlobal(panner->rect().center()), {}, {0, -120}, Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(panner, &wheel);
        QVERIFY(std::fabs(state()->sequence()->audioTracks.at(0).surround.width - 0.9) < 1e-9);
        state()->undo();  // straight after, the drag and the wheel are one step
        QCOMPARE(state()->sequence()->audioTracks.at(0).surround, SurroundPan{});
        QCOMPARE(state()->sequence()->audioLayout, std::string("5.1"));
        // Routed to a bus, a track is placed by the bus's panner instead.
        mixer->findChild<QToolButton*>("addBus")->click();
        const Id bus = state()->sequence()->buses.at(0).id;
        QVERIFY(state()->edit("Route", [bus](Project&, Sequence& s) {
            s.audioTracks[0].output = bus;
            return true;
        }));
        QApplication::processEvents();
        QCOMPARE(visiblePanners().size(), tracks);  // one track fewer, one bus more

        // Export: every channel or the stereo fold-down, and stems.
        {
            ExportDialog ed(state(), win_.get());
            auto* channels = ed.findChild<QComboBox*>("exportAudioChannels");
            auto* stems = ed.findChild<QComboBox*>("exportStems");
            QVERIFY(channels && stems);
            QVERIFY(!channels->isHidden());
            QVERIFY(channels->itemText(0).contains("5.1") && channels->itemText(0).contains("6"));
            QCOMPARE(stems->count(), 3);
            auto* preset = ed.findChild<QComboBox*>("exportPreset");
            auto* path = ed.findChild<QLineEdit*>("exportPath");
            QVERIFY(preset && path);
            preset->setCurrentIndex(preset->findText("Audio - WAV 24-bit"));
            path->setText(dir_.path() + "/surround-mix.wav");
            channels->setCurrentIndex(1);
            stems->setCurrentIndex(2);
            auto* go = ed.findChild<QPushButton*>("exportButton");
            QVERIFY(go);
            go->click();
            QTRY_COMPARE_WITH_TIMEOUT(ed.result(), int(QDialog::Accepted), 60000);
        }
        MediaItem m;
        QVERIFY(probeMedia((dir_.path() + "/surround-mix.wav").toStdString(), m));
        QCOMPARE(m.channels, 2);
        const QString busName = QString::fromStdString(state()->sequence()->buses.at(0).name);
        QVERIFY(QFileInfo::exists(dir_.path() + "/surround-mix - " + busName + ".wav"));
        QVERIFY(QFileInfo::exists(dir_.path() + "/surround-mix - Main.wav"));
        state()->undo();  // the route
        state()->undo();  // the bus
        state()->undo();  // 5.1
        QCOMPARE(state()->sequence()->audioLayout, std::string("stereo"));
        QApplication::processEvents();
        QVERIFY(visiblePanners().empty());
        {
            ExportDialog ed(state(), win_.get());
            QVERIFY(ed.findChild<QComboBox*>("exportAudioChannels")->isHidden());
        }
    }

    void shapeLayersAndLottieInTheApp() {
        state()->newProject();
        // A Shape from the Effects browser lands at the playhead, selected, with its settings in the Inspector.
        auto* browser = win_->findChild<EffectsBrowser*>();
        QVERIFY(browser);
        bool listed = false;
        for (QTreeWidgetItem* item : browser->findChild<QTreeWidget*>()->findItems("Shape", Qt::MatchRecursive))
            listed |= item->data(0, Qt::UserRole).toString() == "shape";
        QVERIFY(listed);
        emit browser->applyRequested("shape", EffectCategory::Generator);
        const Clip* c = state()->primaryClip();
        QVERIFY(c && c->generator.type == "shape");
        QApplication::processEvents();
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector);
        bool trim = false;
        for (auto* l : inspector->findChildren<QLabel*>()) trim |= l->text().startsWith("Trim End") && l->isVisibleTo(inspector);
        QVERIFY(trim);
        // It renders in the viewer's frame: the default 400 x 300 amber rectangle in the middle.
        const Image frame = renderSequenceFrame(state()->project(), *state()->sequence(), c->start, {});
        const float* mid = frame.at(frame.width / 2, frame.height / 2);
        QVERIFY(mid[0] > 0.9f && mid[1] > 0.7f && mid[2] < 0.15f);
        // A Lottie file imports as footage with its length and frame rate.
        const QString json = dir_.path() + "/pulse.json";
        {
            QFile f(json);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({"v":"5.7.4","fr":25,"ip":0,"op":50,"w":100,"h":100,"nm":"pulse","ddd":0,"assets":[],
                "layers":[{"ddd":0,"ind":1,"ty":4,"nm":"dot","sr":1,"ao":0,"ip":0,"op":50,"st":0,"bm":0,
                "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[50,50,0]},
                  "s":{"a":1,"k":[{"t":0,"s":[50,50,100],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":50,"s":[100,100,100]}]}},
                "shapes":[{"ty":"el","nm":"e","d":1,"s":{"a":0,"k":[60,60]},"p":{"a":0,"k":[0,0]}},
                          {"ty":"fl","nm":"f","c":{"a":0,"k":[0,0.6,1,1]},"o":{"a":0,"k":100},"r":1}]}]})");
        }
        if (vectorSupport()) {
            const auto ids = state()->importFiles({json});
            QCOMPARE(ids.size(), size_t(1));
            const MediaItem* m = state()->project().findMedia(ids[0]);
            QVERIFY(m && m->kind == MediaKind::Video && m->hasVideo && !m->hasAudio);
            QVERIFY(std::fabs(m->duration - 2.0) < 1e-6);
            QCOMPARE(m->fps.num, 25);
        }
        // Stills and footage offer a Super Scale copy (2x, 3x, 4x); graphics drawn at any size do not.
        const QString png = dir_.path() + "/still.png";
        {
            QImage q(32, 24, QImage::Format_RGB32);
            q.fill(Qt::darkCyan);
            QVERIFY(q.save(png));
        }
        const auto still = state()->importFiles({png});
        QCOMPARE(still.size(), size_t(1));
        auto* bin = win_->findChild<MediaBinWidget*>();
        auto* icons = bin->findChild<QAbstractItemView*>("mediaIcons");
        QVERIFY(bin && icons);
        bin->setView(MediaBinWidget::View::Icons);
        auto superScaleChoices = [&](Id id) {
            bin->selectMedia({id});
            int found = -1;
            QTimer::singleShot(0, this, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                auto* sub = menu->findChild<QMenu*>("superScaleMenu");
                found = sub && sub->menuAction()->isVisible() ? int(sub->actions().size()) : 0;
                menu->close();
            });
            emit icons->customContextMenuRequested(icons->visualRect(icons->currentIndex()).center());
            return found;
        };
        QCOMPARE(superScaleChoices(still[0]), 3);
        state()->newProject();
        win_->activateWindow();  // the context menu took the focus
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void checkerboardFromTheClipMenu() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        // Two speakers, as diarization would label them.
        QVERIFY(state()->edit("Label", [media](Project& p, Sequence&) {
            auto t = std::make_shared<Transcript>();
            TranscriptSegment a, b;
            a.start = 0.3, a.end = 3.0, a.speaker = 0, a.text = "And so";
            b.start = 3.6, b.end = 7.0, b.speaker = 1, b.text = "my fellow";
            t->segments = {a, b};
            p.findMedia(media)->transcript = t;
            return true;
        }));
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        state()->setSelection({clip}, false);
        const size_t before = state()->sequence()->audioTracks.at(0).clips.size();
        const FrameTime origEnd = edit::clipById(*state()->sequence(), clip)->end();
        win_->findChild<QAction*>("checkerboardBySpeaker")->trigger();
        QCOMPARE(state()->sequence()->audioTracks.at(0).clips.size(), before);  // the second speaker moved off A1
        QCOMPARE(state()->sequence()->audioTracks.at(1).clips.size(), size_t(1));
        const FrameTime cut = FrameTime(std::llround(3.3 * state()->sequence()->fpsValue()));
        QCOMPARE(state()->sequence()->audioTracks.at(0).clips.at(0).end(), cut);
        QCOMPARE(state()->sequence()->audioTracks.at(1).clips.at(0).start, cut);
        state()->undo();
        QCOMPARE(state()->sequence()->audioTracks.at(1).clips.size(), size_t(0));
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->end(), origEnd);  // one undo step puts it back
        state()->newProject();
    }

    void exportAafFromTheFileMenu() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 10, 25, 50, V1, A1, false); }));
        QVERIFY(win_->findChild<QAction*>("exportAaf"));
        QString summary;
        QVERIFY(win_->exportAafTo(dir_.path() + "/turnover.aaf", &summary));
        QVERIFY2(summary.contains("1 audio tracks") && summary.contains("1 clips"), qPrintable(summary));
        QVERIFY(QFileInfo::exists(dir_.path() + "/turnover.aaf"));
        QVERIFY(QFileInfo::exists(dir_.path() + "/turnover Media/jfk.wav"));
        state()->newProject();
    }

    void renderAndReplaceVideo() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->edit("Effect", [red](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects.push_back(makeEffect(p, "invert"));
            return true;
        });
        RenderOptions ro;
        const Image before = renderProgramFrame(state()->project(), *state()->sequence(), 5, ro);
        QString err;
        QVERIFY2(timeline()->renderAndReplace(red, &err), qPrintable(err));
        const Clip* c = edit::clipById(*state()->sequence(), red);
        QVERIFY(c && !c->isGenerator() && c->effects.empty() && !c->unrendered.empty());
        const MediaItem* m = state()->project().findMedia(c->mediaId);
        QVERIFY(m && QString::fromStdString(m->path).endsWith(".mov") && m->bin == "Rendered Video");
        const Image after = renderProgramFrame(state()->project(), *state()->sequence(), 5, ro);
        float worst = 0;
        for (size_t i = 0; i < after.px.size(); ++i) worst = std::max(worst, std::fabs(after.px[i] - before.px[i]));
        QVERIFY2(worst < 0.02f, qPrintable(QString::number(worst)));
        // Restore Unrendered brings back the matte and its effect.
        state()->apply("Restore", [red](Project&, Sequence& s) { return edit::restoreUnrendered(s, red); });
        c = edit::clipById(*state()->sequence(), red);
        QVERIFY(c->isGenerator() && c->effects.size() == 1);
    }

    void renderAndReplaceInTheTimeline() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, 90, V1, A1, false);
        }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        QVERIFY(state()->edit("Limiter", [clip](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "limiter");
            e.params["ceiling_db"] = -20.0;
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        }));
        QString err;
        QVERIFY2(timeline()->renderAndReplace(clip, &err), qPrintable(err));
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c->mediaId != media);
        QVERIFY(c->effects.empty());
        const MediaItem* rendered = state()->project().findMedia(c->mediaId);
        QVERIFY(rendered);
        QCOMPARE(QString::fromStdString(rendered->bin), QString("Rendered Audio"));
        // The rendered sound is limited to -20 dB.
        AudioMixer mixer;
        std::vector<float> out(48000 * 2);
        mixer.mix(state()->project(), *state()->sequence(), 0, 48000, out.data());
        float peak = 0;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        QVERIFY2(peak > 0.05f && peak < 0.105f, qPrintable(QString::number(peak)));
        // Undo, and Restore Unrendered.
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->mediaId, media);
        state()->redo();
        QVERIFY(state()->apply("Restore", [clip](Project&, Sequence& s) { return edit::restoreUnrendered(s, clip); }));
        c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->mediaId, media);
        QCOMPARE(c->effects.size(), size_t(1));
        state()->newProject();
    }

    void crashRecoveryAndSnapshots() {
        QTemporaryDir rdir;
        QVERIFY(rdir.isValid());
        loadDemo();
        const QString projectPath = state()->filePath();
        Id red = clipNamed(*state()->sequence(), "Red")->id;
        {
            RecoveryManager rm(state(), rdir.path());
            QVERIFY(rm.crashedSessions().empty());
            rm.setSnapshotInterval(0);
            rm.beginSession();
            state()->edit("Rename", [red](Project&, Sequence& s) {
                edit::clipById(s, red)->name = "Recovered";
                return true;
            });
            rm.saveNow();
            QCOMPARE(rm.snapshots("demo").size(), 1);
            rm.abandonSession();  // the process "crashes"
        }
        state()->newProject();  // the edit is gone from the editor

        RecoveryManager rm2(state(), rdir.path());
        auto crashed = rm2.crashedSessions();
        QCOMPARE(crashed.size(), size_t(1));
        QCOMPARE(crashed[0].projectPath, projectPath);
        QCOMPARE(crashed[0].projectName, QString("demo"));
        QVERIFY(!crashed[0].recoveryFile.isEmpty());
        QVERIFY(rm2.recover(crashed[0]));
        QCOMPARE(state()->filePath(), projectPath);  // saving writes the real project
        QVERIFY(state()->isModified());
        QVERIFY(clipNamed(*state()->sequence(), "Recovered"));
        QVERIFY(rm2.crashedSessions().empty());

        // A running session is never reported as crashed; a clean exit leaves nothing.
        rm2.beginSession();
        {
            RecoveryManager other(state(), rdir.path());
            QVERIFY(other.crashedSessions().empty());
        }
        rm2.endSession();
        QVERIFY(QDir(rdir.path() + "/sessions").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        // Snapshots keep the newest N.
        const QString snaps = rm2.snapshotDir("demo");
        for (int i = 1; i <= 5; ++i) {
            QFile f(snaps + QString("/demo 2020-01-0%1 10-00-00.montage").arg(i));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        rm2.setMaxSnapshots(3);
        rm2.setSnapshotInterval(0);
        rm2.beginSession();
        state()->edit("Rename", [red](Project&, Sequence& s) {
            edit::clipById(s, red)->name = "Again";
            return true;
        });
        rm2.saveNow();
        const QStringList kept = rm2.snapshots("demo");
        QCOMPARE(kept.size(), 3);
        QVERIFY(!kept[0].contains("2020"));  // today's snapshot is the newest
        rm2.endSession();

        // Safe mode hides plugins.
        plugins::Registry::instance().setEnabled(false);
        QVERIFY(!plugins::Registry::instance().find("clap:org.montage.test.gain").has_value());
        QVERIFY(plugins::Registry::instance().plugins().empty());
        plugins::Registry::instance().setEnabled(true);
        state()->newProject();
    }

    void speechModelDownload() {
        // A local "mirror" with a model, a file that is not a model, and nothing else.
        QTemporaryDir mirror, models;
        auto writeFile = [&](const QString& name, const QByteArray& data) {
            QFile f(mirror.path() + "/" + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        writeFile("ggml-tiny.en.bin", QByteArray("lmgg", 4) + QByteArray(256 * 1024, '\0'));
        writeFile("ggml-base.en.bin", "<html>Not found</html>");
        qputenv("MONTAGE_WHISPER_MODELS", models.path().toLocal8Bit());
        qputenv("MONTAGE_WHISPER_MODEL_URL", QUrl::fromLocalFile(mirror.path()).toString().toLocal8Bit());
        auto fetch = [](const char* name, QString* error) {
            ModelDownload dl;
            QSignalSpy done(&dl, &ModelDownload::finished);
            dl.start(name);
            if (done.isEmpty() && !done.wait(20000)) return false;
            *error = done.at(0).at(1).toString();
            return done.at(0).at(0).toBool();
        };
        QString err;
        QVERIFY2(fetch("tiny.en", &err), qPrintable(err));
        QCOMPARE(QString::fromStdString(whisperModelPath("tiny.en")), models.path() + "/ggml-tiny.en.bin");
        QCOMPARE(QFileInfo(models.path() + "/ggml-tiny.en.bin").size(), qint64(4 + 256 * 1024));
        QVERIFY(!fetch("base.en", &err));
        QVERIFY2(err.contains("not a speech model"), qPrintable(err));
        QVERIFY(whisperModelPath("base.en").empty());
        QVERIFY(!fetch("small.en", &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(QDir(models.path()).entryList(QDir::Files), QStringList{"ggml-tiny.en.bin"});  // no .part left

        // The dialog says which models are already here.
        {
            TranscribeDialog dlg(1, win_.get());
            auto* model = dlg.findChildren<QComboBox*>().value(0);
            QVERIFY(model);
            QVERIFY(model->itemText(0).contains("downloaded"));
            QVERIFY(!model->itemText(1).contains("downloaded"));
            model->setCurrentIndex(0);
            QCOMPARE(QString::fromStdString(dlg.options().model), QString("tiny.en"));
            QCOMPARE(QString::fromStdString(dlg.options().language), QString("en"));  // English-only model
        }
        qunsetenv("MONTAGE_WHISPER_MODELS");
        qunsetenv("MONTAGE_WHISPER_MODEL_URL");
    }

    void transcribeFromMediaBin() {
        const QString model = qEnvironmentVariable("MONTAGE_TEST_WHISPER_MODEL");
        if (model.isEmpty() || !QFileInfo::exists(model))
            QSKIP("Set MONTAGE_TEST_WHISPER_MODEL to a ggml whisper model to run this test");
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id id = ids[0];
        TranscribeOptions opts;
        opts.model = model.toStdString();
        startTranscription(state(), ids, opts, win_.get());
        QTRY_VERIFY_WITH_TIMEOUT(state()->project().findMedia(id)->transcript != nullptr, 120000);
        const auto transcript = state()->project().findMedia(id)->transcript;
        QVERIFY(QString::fromStdString(transcript->text()).contains("your country", Qt::CaseInsensitive));
        QVERIFY(transcript->wordCount() > 15);

        // The media bin search finds the clip by what is said in it.
        QLineEdit* search = nullptr;
        for (auto* e : win_->findChildren<QLineEdit*>())
            if (e->placeholderText() == "Search media") search = e;
        QVERIFY(search);
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin);
        search->setText("\"fellow americans\"");
        QCOMPARE(bin->shownMedia().size(), size_t(1));
        search->setText("\"words nobody said\"");
        QCOMPARE(bin->shownMedia().size(), size_t(0));
        search->clear();

        // It is one undo step, and it is saved with the project.
        state()->undo();
        QVERIFY(!state()->project().findMedia(id)->transcript);
        state()->redo();
        QVERIFY(state()->project().findMedia(id)->transcript);
        const std::string path = (dir_.path() + "/transcribed.montage").toStdString();
        QVERIFY(saveProject(state()->project(), path));
        Project back;
        QVERIFY(loadProject(path, back));
        QVERIFY(back.findMedia(id)->transcript);
        QCOMPARE(*back.findMedia(id)->transcript, *transcript);
        state()->newProject();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));  // the window has the focus back
    }

    void hoverScrubInTheBin() {
        // A video red for its first second and blue for its second.
        const QString path = dir_.path() + "/skim.mp4";
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            for (int k = 0; k < 2; ++k) {
                Clip c = makeGeneratorClip(gen, "color", 30);
                c.generator.params["color.r"] = Param(k ? 0.0 : 0.9);
                c.generator.params["color.g"] = Param(0.0);
                c.generator.params["color.b"] = Param(k ? 0.9 : 0.0);
                c.start = 30 * k;
                edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            }
            ExportSettings st;
            st.path = path.toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({path});
        QCOMPARE(ids.size(), size_t(1));
        auto* bin = win_->findChild<MediaBinWidget*>();
        win_->raisePanel("media");
        bin->setView(MediaBinWidget::View::Icons);
        bin->setHoverScrub(true);
        auto* icons = bin->findChild<QListView*>("mediaIcons");
        QVERIFY(icons && icons->isVisible());
        QTRY_COMPARE(bin->shownMedia(), ids);
        const QModelIndex vi = icons->model()->index(0, 0);
        const QRect cell = icons->visualRect(vi);
        auto move = [&](double u) {
            const QPoint pos(int(cell.center().x() - MediaBinModel::kThumbW / 2.0 + u * MediaBinModel::kThumbW), cell.top() + 20);
            QMouseEvent ev(QEvent::MouseMove, QPointF(pos), QPointF(icons->viewport()->mapToGlobal(pos)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(icons->viewport(), &ev);
        };
        // The thumbnail's colour away from the playhead line.
        auto shown = [&](double u) {
            const QPixmap pm = bin->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>();
            const int x = u < 0.5 ? MediaBinModel::kThumbW - 20 : 20;
            return pm.toImage().pixelColor(x, MediaBinModel::kThumbH / 2);
        };
        MediaBinModel* model = bin->model();

        // Near the left: the red second, with the playhead line drawn; near the right: the blue one.
        move(0.1);
        QCOMPARE(model->skimmed(), ids[0]);
        QVERIFY2(model->skimSeconds() > 0.1 && model->skimSeconds() < 0.3, qPrintable(QString::number(model->skimSeconds())));
        QTRY_VERIFY(shown(0.1).red() > 150 && model->skimFrameShown());
        QVERIFY(shown(0.1).blue() < 80);
        move(0.9);
        QVERIFY(model->skimSeconds() > 1.7 && model->skimSeconds() < 2.0);
        QTRY_VERIFY(shown(0.9).blue() > 150 && model->skimFrameShown());
        QVERIFY(shown(0.9).red() < 80);
        // Leaving puts the poster frame back.
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(icons->viewport(), &leave);
        QCOMPARE(model->skimmed(), Id(0));
        // Off, moving over it does nothing.
        bin->setHoverScrub(false);
        move(0.9);
        QCOMPARE(model->skimmed(), Id(0));
        bin->setHoverScrub(true);
        state()->newProject();
    }

    void mediaBinLogging() {
        // Two short videos and a sound file.
        QStringList files;
        for (int k = 0; k < 2; ++k) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", 25);
            c.generator.params["color.r"] = Param(k ? 0.2 : 0.8);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = (dir_.path() + QString("/take%1.mp4").arg(k + 1)).toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
            files << QString::fromStdString(st.path);
        }
        files << QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav");
        state()->newProject();
        const std::vector<Id> ids = state()->importFiles(files);
        QCOMPARE(ids.size(), size_t(3));
        const Id take1 = ids[0], take2 = ids[1], jfk = ids[2];
        auto* bin = win_->findChild<MediaBinWidget*>();
        auto* tree = bin->findChild<BinTree*>("binTree");
        auto* icons = bin->findChild<QAbstractItemView*>("mediaIcons");
        auto* list = bin->findChild<QTreeView*>("mediaList");
        QVERIFY(bin && tree && icons && list);
        auto media = [&](Id id) { return state()->project().findMedia(id); };
        QCOMPARE(bin->shownMedia(), ids);
        QVERIFY(tree->isHidden());  // no bins yet

        // Bins: a new one appears in the tree; renaming and moving media are undoable.
        const QString first = bin->newBin({});
        QCOMPARE(first, QString("Bin"));
        QVERIFY(!tree->isHidden());
        QVERIFY(bin->renameBin(first, "Inter/views"));  // a slash would nest it
        QCOMPARE(projectBins(state()->project()), std::vector<std::string>{"Inter-views"});
        state()->undo();
        QCOMPARE(projectBins(state()->project()), std::vector<std::string>{"Bin"});
        state()->redo();
        QVERIFY(bin->renameBin("Inter-views", "Interviews"));
        QVERIFY(bin->moveToBin({take1, take2}, "Interviews"));
        QCOMPARE(bin->shownMedia(), std::vector<Id>{jfk});
        bin->showBin("Interviews");
        QCOMPARE(bin->shownMedia(), (std::vector<Id>{take1, take2}));
        QCOMPARE(tree->currentItem()->text(0), QString("Interviews"));
        // A nested bin, then media dropped on it in the tree.
        const QString day = bin->newBin("Interviews");
        QCOMPARE(day, QString("Interviews/Bin"));
        QTreeWidgetItem* dayItem = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, BinTree::PathRole).toString() == day) dayItem = *it;
        QVERIFY(dayItem);
        tree->scrollToItem(dayItem);
        {
            QMimeData mime;
            mime.setData("application/x-montage-media", QByteArray::number(qulonglong(take2)));
            const QPoint at = tree->visualItemRect(dayItem).center();
            QDragEnterEvent enter(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &enter);
            QDragMoveEvent move(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &move);
            QVERIFY(move.isAccepted());
            QDropEvent drop(QPointF(at), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &drop);
        }
        QCOMPARE(media(take2)->bin, std::string("Interviews/Bin"));
        QCOMPARE(bin->shownMedia(), std::vector<Id>{take1});
        // A search looks inside the bin's bins.
        bin->showBin({});
        auto* search = bin->findChild<QLineEdit*>();
        search->setText("take");
        QCOMPARE(bin->shownMedia(), (std::vector<Id>{take1, take2}));
        search->clear();

        // Ratings from the keyboard: 0–5, and X to reject (or un-reject), over the window's multicam keys.
        bin->showBin("Interviews");
        bin->setView(MediaBinWidget::View::Icons);
        bin->selectMedia({take1});
        icons->setFocus();
        QTest::keyClick(icons, Qt::Key_4);
        QCOMPARE(media(take1)->rating, 4);
        state()->undo();
        QCOMPARE(media(take1)->rating, 0);
        state()->redo();
        QTest::keyClick(icons, Qt::Key_X);
        QCOMPARE(media(take1)->rating, -1);
        QTest::keyClick(icons, Qt::Key_X);
        QCOMPARE(media(take1)->rating, 0);
        QTest::keyClick(icons, Qt::Key_5);
        QCOMPARE(media(take1)->rating, 5);

        // A colour label from the context menu.
        bool triggered = false;
        QTimer::singleShot(0, this, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            if (auto* sub = menu->findChild<QMenu*>("labelMenu"))
                for (QAction* a : sub->actions())
                    if (a->data().toInt() == labelFromName("Forest")) {
                        a->trigger();
                        triggered = true;
                    }
            menu->close();
        });
        emit icons->customContextMenuRequested(QPoint(10, 10));
        QVERIFY(triggered);
        QCOMPARE(media(take1)->label, labelFromName("Forest"));
        QVERIFY(bin->addKeywords({take1, jfk}, {"interview", "Anna"}));
        QVERIFY(bin->removeKeyword({jfk}, "ANNA"));
        QCOMPARE(media(jfk)->keywords, std::vector<std::string>{"interview"});

        // The list view: columns of fields, edits to every selected row as one step, sorting.
        bin->showBin({});
        bin->setView(MediaBinWidget::View::List);
        QCOMPARE(bin->currentView(), static_cast<QAbstractItemView*>(list));
        search->setText("interview");  // take1 (in a bin) and jfk
        QCOMPARE(bin->shownMedia().size(), size_t(2));
        const int scene = MediaBinModel::columnOf("scene"), rating = MediaBinModel::columnOf("rating");
        list->setColumnHidden(scene, false);
        bin->selectMedia({take1, jfk});
        const QModelIndex cell = list->model()->index(0, scene);
        {
            QLineEdit editor;
            editor.setText("12A");
            list->itemDelegate()->setModelData(&editor, list->model(), cell);
        }
        QCOMPARE(media(take1)->metadata.at("scene"), std::string("12A"));
        QCOMPARE(media(jfk)->metadata.at("scene"), std::string("12A"));
        QVERIFY(list->model()->index(0, scene).data().toString() == "12A");
        state()->undo();
        QVERIFY(!media(take1)->metadata.count("scene") && !media(jfk)->metadata.count("scene"));
        state()->redo();
        // One cell outside the selection edits just its row.
        bin->selectMedia({take1});
        const int jfkRow = bin->shownMedia()[0] == jfk ? 0 : 1;
        QVERIFY(list->model()->setData(list->model()->index(jfkRow, scene), "14"));
        QCOMPARE(media(jfk)->metadata.at("scene"), std::string("14"));
        QCOMPARE(media(take1)->metadata.at("scene"), std::string("12A"));
        list->sortByColumn(rating, Qt::DescendingOrder);
        QCOMPARE(bin->shownMedia().front(), take1);
        list->sortByColumn(rating, Qt::AscendingOrder);
        QCOMPARE(bin->shownMedia().front(), jfk);
        QCOMPARE(list->model()->index(1, rating).data().toString(), QString(5, QChar(0x2605)));
        search->clear();

        // Smart bins: rules edited in the dialog, contents kept up to date.
        {
            SmartBinDialog dlg(state()->project(), SmartBin{}, win_.get());
            auto* count = dlg.findChild<QLabel*>("smartCount");
            QVERIFY(count);
            QCOMPARE(dlg.ruleCount(), 1);  // rating at least three stars, to start with
            QVERIFY2(count->text().startsWith("1 "), qPrintable(count->text()));
            dlg.findChild<QLineEdit*>("smartName")->setText("Interviews to use");
            dlg.addRule();
            QCOMPARE(dlg.ruleCount(), 2);
            const auto fields = dlg.findChildren<QComboBox*>("ruleField");
            QCOMPARE(fields.size(), 2);
            fields[1]->setCurrentIndex(fields[1]->findData("keywords"));
            auto choices = dlg.findChildren<QComboBox*>("ruleChoice");
            choices[1]->setCurrentText("interview");
            QVERIFY2(count->text().startsWith("1 "), qPrintable(count->text()));
            dlg.findChild<QComboBox*>("smartMatch")->setCurrentIndex(1);  // any
            QVERIFY2(count->text().startsWith("2 "), qPrintable(count->text()));
            const SmartBin b = dlg.bin();
            QCOMPARE(b.name, std::string("Interviews to use"));
            QVERIFY(!b.matchAll);
            QCOMPARE(b.rules.size(), size_t(2));
            QCOMPARE(b.rules[0], (SmartRule{"rating", ">=", "3"}));
            QCOMPARE(b.rules[1], (SmartRule{"keywords", "includes", "interview"}));
            const Id smart = bin->addSmartBin(b);
            QVERIFY(smart);
            QCOMPARE(bin->currentSmartBin(), smart);
            std::vector<Id> shown = bin->shownMedia();
            std::sort(shown.begin(), shown.end());
            QCOMPARE(shown, (std::vector<Id>{take1, jfk}));
            // Logging updates it: rejecting take 1 still leaves its keyword.
            SmartBin strict = *findSmartBin(state()->project(), smart);
            strict.matchAll = true;
            QVERIFY(bin->updateSmartBin(strict));
            QCOMPARE(bin->shownMedia(), std::vector<Id>{take1});
            QVERIFY(bin->setRating({take1}, -1));
            QVERIFY(bin->shownMedia().empty());
            state()->undo();
            QCOMPARE(bin->shownMedia(), std::vector<Id>{take1});
            // Editing an existing smart bin starts from its rules.
            SmartBinDialog again(state()->project(), strict, win_.get());
            QCOMPARE(again.ruleCount(), 2);
            QCOMPARE(again.bin(), strict);
        }

        // Deleting a bin keeps its media; the project saves all of it.
        QVERIFY(bin->deleteBin("Interviews"));
        QCOMPARE(media(take1)->bin, std::string());
        QCOMPARE(media(take2)->bin, std::string("Bin"));
        const std::string path = (dir_.path() + "/logged.montage").toStdString();
        QVERIFY(saveProject(state()->project(), path));
        Project back;
        QVERIFY(loadProject(path, back));
        QCOMPARE(back.smartBins, state()->project().smartBins);
        QCOMPARE(back.bins, state()->project().bins);
        QCOMPARE(back.findMedia(take1)->rating, 5);
        QCOMPARE(back.findMedia(take1)->label, labelFromName("Forest"));
        QCOMPARE(back.findMedia(jfk)->metadata, (std::map<std::string, std::string>{{"scene", "14"}}));
        bin->setView(MediaBinWidget::View::Icons);
        state()->newProject();
        QVERIFY(bin->shownMedia().empty());
        QCOMPARE(bin->currentSmartBin(), Id(0));
        QVERIFY(tree->isHidden());
    }

    void clipLinesOnTheTimeline() {
        // A sound clip on A1 and a colour clip on V1, 11 s each.
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        const Clip* placed = trackAt(*state()->sequence(), A1)->clips.empty() ? nullptr : &trackAt(*state()->sequence(), A1)->clips.front();
        QVERIFY(placed);
        const Id audio = placed->id;
        const FrameTime len = placed->duration;
        QVERIFY(state()->edit("Colour", [len](Project& p, Sequence& s) {
            Clip c = makeGeneratorClip(p, "color", len);
            return edit::overwrite(p, s, V1, c).ok;
        }));
        const Id video = trackAt(*state()->sequence(), V1)->clips.front().id;
        ppf_ = measurePpf();
        timeline()->setShowVolumeLines(true);
        timeline()->setShowOpacityLines(false);
        state()->clearSelection();
        auto clip = [&](Id id) { return edit::clipById(*state()->sequence(), id); };
        auto gain = [&]() -> const Param& { return clip(audio)->audio.params.at("gain_db"); };
        const QRect band = timeline()->lineBand(audio);
        QVERIFY(!band.isNull());
        QVERIFY(timeline()->lineBand(video).isNull());  // opacity lines are off
        auto at = [&](FrameTime f, Id id = 0) {
            const int x = band.left() - 2 + int(std::lround(double(f) * ppf_));
            return QPoint(x, timeline()->lineY(id ? id : audio, f));
        };
        // 0 dB sits at 71 % of the height.
        QVERIFY(std::abs(at(60).y() - (band.top() + int(std::lround((1 - std::sqrt(0.5)) * (band.height() - 1))))) <= 1);

        // Dragging the line down lowers the volume, as one undo step.
        drag(at(60), at(60) + QPoint(0, band.height() / 4));
        const double lowered = gain().value;
        QVERIFY2(lowered < -3 && lowered > -20, qPrintable(QString::number(lowered)));
        QVERIFY(!gain().animated());
        QVERIFY(state()->isSelected(audio));
        QCOMPARE(trackAt(*state()->sequence(), A1)->clips.front().start, FrameTime(0));  // the clip did not move
        state()->undo();
        QCOMPARE(gain().value, 0.0);
        state()->redo();
        QCOMPARE(gain().value, lowered);
        // It stops at +6 dB.
        drag(at(60), QPoint(at(60).x(), band.top() - 30));
        QCOMPARE(gain().value, kGainLineMaxDb);
        state()->undo();

        // Ctrl/Cmd-click adds keyframes on the line.
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::ControlModifier, at(90));
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::ControlModifier, at(200));
        QCOMPARE(gain().keys.size(), size_t(2));
        QVERIFY(std::abs(gain().keys[0].t - 90) <= 1 && std::abs(gain().keys[1].t - 200) <= 1);
        QCOMPARE(gain().keys[0].v, lowered);
        const FrameTime k0 = gain().keys[0].t, k1 = gain().keys[1].t;
        // Dragging the line between them moves both.
        drag(at(150), at(150) + QPoint(0, 12));
        QVERIFY(gain().keys[0].v < lowered);
        QCOMPARE(gain().keys[0].v, gain().keys[1].v);
        // Dragging a key moves it in time and value; with Shift only in value.
        const double before = gain().keys[0].v;
        drag(QPoint(at(k0).x(), timeline()->lineY(audio, k0)), QPoint(at(k0 + 20).x(), timeline()->lineY(audio, k0) - 10));
        QVERIFY2(std::abs(gain().keys[0].t - (k0 + 20)) <= 1, qPrintable(QString::number(gain().keys[0].t)));
        QVERIFY(gain().keys[0].v > before);
        const FrameTime moved = gain().keys[0].t;
        drag(QPoint(at(moved).x(), timeline()->lineY(audio, moved)), QPoint(at(moved + 30).x(), timeline()->lineY(audio, moved) + 8),
             Qt::ShiftModifier);
        QCOMPARE(gain().keys[0].t, moved);
        state()->undo();
        QCOMPARE(gain().keys[0].t, moved);
        // A key cannot pass its neighbour.
        drag(QPoint(at(moved).x(), timeline()->lineY(audio, moved)), QPoint(at(k1 + 40).x(), timeline()->lineY(audio, moved)));
        QCOMPARE(gain().keys[0].t, k1 - 1);
        state()->undo();

        // Right-click a key to change how it eases; Alt-click deletes it.
        bool triggered = false;
        QTimer::singleShot(0, this, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            for (QAction* a : menu->actions())
                if (a->data().toInt() == int(Interp::Hold) && a->isCheckable()) {
                    a->trigger();
                    triggered = true;
                }
            menu->close();
        });
        const QPoint key0(at(moved).x(), timeline()->lineY(audio, moved));
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, key0, viewport()->mapToGlobal(key0));
        QApplication::sendEvent(viewport(), &menuEvent);
        QVERIFY(triggered);
        QCOMPARE(gain().keys[0].interp, Interp::Hold);
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::AltModifier, QPoint(at(k1).x(), timeline()->lineY(audio, k1)));
        QCOMPARE(gain().keys.size(), size_t(1));
        state()->undo();
        QCOMPARE(gain().keys.size(), size_t(2));

        // Opacity lines on video clips, linear from 0 to 100 %.
        timeline()->setShowOpacityLines(true);
        const QRect vband = timeline()->lineBand(video);
        QVERIFY(!vband.isNull());
        const QPoint top = at(100, video);
        QCOMPARE(top.y(), vband.top());  // 100 %
        drag(top, QPoint(top.x(), vband.top() + (vband.height() - 1) / 2));
        const double opacity = clip(video)->motion.params.at("opacity").value;
        QVERIFY2(std::fabs(opacity - 50) < 3, qPrintable(QString::number(opacity)));
        // With the line hidden, the same drag moves the clip instead.
        timeline()->setShowOpacityLines(false);
        state()->setSnapping(false);
        const QPoint mid(top.x(), vband.top() + (vband.height() - 1) / 2);
        drag(mid, mid + QPoint(int(30 * ppf_), 0));
        QVERIFY(clip(video)->start > 0);
        state()->setSnapping(true);
        state()->newProject();
    }

    void sourceMonitorShowsTheWaveform() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        MonitorPanel *source = nullptr, *program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            (m->mode() == MonitorPanel::Mode::Source ? source : program) = m;
        QVERIFY(source && program);
        QVERIFY(!source->scrubBar()->hasWaveform());

        // Opened in the Source monitor, its audio shows on the scrub bar once decoded, which grows to fit it.
        state()->setSourceMedia(ids[0]);
        QTRY_VERIFY_WITH_TIMEOUT(source->scrubBar()->hasWaveform(), 20000);
        QVERIFY(!program->scrubBar()->hasWaveform());
        QCOMPARE(source->scrubBar()->height(), 40);
        const QImage img = source->scrubBar()->grab().toImage();
        int wave = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                QColor c = img.pixelColor(x, y);
                if (c.green() > 110 && c.green() > c.red() + 30 && c.green() > c.blue() + 10) ++wave;
            }
        QVERIFY2(wave > img.width(), qPrintable(QString::number(wave)));

        state()->setSourceMedia(0);
        QVERIFY(!source->scrubBar()->hasWaveform());
        QCOMPARE(source->scrubBar()->height(), 18);
        state()->newProject();
    }

    void rippleDeleteAGap() {
        loadDemo();
        const Clip* red = clipNamed(*state()->sequence(), "Red");
        const Id redId = red->id, blueId = clipNamed(*state()->sequence(), "Blue")->id;
        const FrameTime redEnd = red->end();
        // Blue moved half a second later leaves a gap after Red.
        QVERIFY(state()->apply("Move", [blueId](Project& p, Sequence& s) { return edit::moveClips(p, s, {blueId}, 15, 0, 0); }));
        TimelineWidget* tl = win_->timeline();
        const QRect r = tl->clipBounds(redId), b = tl->clipBounds(blueId);
        QVERIFY(b.left() > r.right() + 4);
        // A click in it selects it (and only that); Delete closes it, rippling Blue back.
        QTest::mouseClick(tl->viewport(), Qt::LeftButton, {}, QPoint((r.right() + b.left()) / 2, r.center().y()));
        QVERIFY(tl->selectedGap());
        QCOMPARE(tl->selectedGap()->from, redEnd);
        QCOMPARE(tl->selectedGap()->to, redEnd + 15);
        QVERIFY(state()->selectedClips().empty());
        QAction* lift = nullptr;  // Edit › Delete (Delete / Backspace)
        for (QAction* a : win_->findChildren<QAction*>())
            if (a->text() == "&Delete (Lift)") lift = a;
        QVERIFY(lift);
        lift->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), blueId)->start, redEnd);
        QVERIFY(!tl->selectedGap());
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), blueId)->start, redEnd + 15);
        // On a clip there is no gap; selecting a clip drops a selected gap.
        QVERIFY(!tl->selectGapAt({TrackKind::Video, 0}, 5));
        QVERIFY(tl->selectGapAt({TrackKind::Video, 0}, redEnd + 3));
        state()->setSelection({redId});
        QVERIFY(!tl->selectedGap());
    }

    void auditionsFromTheBin() {
        // Two takes of a shot: the first in the cut, the second added from the bin as a take.
        QStringList files;
        for (int k = 0; k < 2; ++k) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", 40);
            c.generator.params["color.r"] = Param(k ? 0.0 : 0.9);
            c.generator.params["color.b"] = Param(k ? 0.9 : 0.0);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = (dir_.path() + QString("/aud%1.mp4").arg(k + 1)).toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
            files << QString::fromStdString(st.path);
        }
        state()->newProject();
        const auto ids = state()->importFiles(files);
        QCOMPARE(ids.size(), size_t(2));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id clip = state()->sequence()->videoTracks[0].clips[0].id;
        auto* bin = win_->findChild<MediaBinWidget*>();
        auto* add = win_->findChild<QAction*>("addTakes");
        auto* next = win_->findChild<QAction*>("nextTake");
        QVERIFY(add && next && win_->findChild<QAction*>("previousTake") && win_->findChild<QAction*>("finalizeAudition"));
        QCOMPARE(next->shortcut(), QKeySequence("Ctrl+Alt+Right"));
        QVERIFY(!win_->cycleTake(1));  // not an audition yet
        state()->setSelection({clip});
        bin->selectMedia({ids[1]});
        add->trigger();
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->takes.size(), size_t(2));
        // The next take plays in its place, red giving way to blue, as one undo step.
        auto centre = [&]() {
            const Image img = renderSequenceFrame(state()->project(), *state()->sequence(), 10, {});
            const size_t i = (size_t(img.height / 2) * size_t(img.width) + size_t(img.width / 2)) * 4;
            return std::make_pair(img.px[i], img.px[i + 2]);
        };
        QVERIFY(centre().first > 0.5f);
        next->trigger();
        c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->mediaId, ids[1]);
        QCOMPARE(c->take, 1);
        QVERIFY(centre().second > 0.5f && centre().first < 0.2f);
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->mediaId, ids[0]);
        state()->redo();
        QVERIFY(win_->finalizeAudition());
        c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c->takes.empty());
        QCOMPARE(c->mediaId, ids[1]);

        // Duplicate frame markers: the same take again later is striped under its repeated frames, when shown.
        QVERIFY(state()->edit("Again", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[1], 60, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        auto* dups = win_->findChild<QAction*>("showDuplicateFrames");
        QVERIFY(dups && !win_->timeline()->showDuplicateFrames());
        dups->trigger();
        QVERIFY(win_->timeline()->showDuplicateFrames());
        QCOMPARE(win_->timeline()->duplicateSpans(clip).size(), size_t(1));
        QCOMPARE(win_->timeline()->duplicateSpans(clip).front().from, FrameTime(0));
        dups->trigger();
        QVERIFY(!win_->timeline()->showDuplicateFrames());

        // Clip durations in the name strips, as a view option.
        auto* durations = win_->findChild<QAction*>("showClipDurations");
        QVERIFY(durations);
        if (durations->isChecked()) durations->trigger();  // off to start with, whatever an earlier run left
        QVERIFY(!win_->timeline()->showClipDurations());
        for (int k = 0; k < 4; ++k) win_->timeline()->zoomIn();  // clips wide enough for the badge
        const QImage plain = win_->timeline()->viewport()->grab().toImage();
        durations->trigger();
        QVERIFY(win_->timeline()->showClipDurations());
        QVERIFY(win_->timeline()->viewport()->grab().toImage() != plain);
        durations->trigger();

        // A razor cut is marked as a through edit; Join Through Edits puts the clip back together.
        const Id again = state()->sequence()->videoTracks[0].clips[1].id;
        state()->setSelection({});
        QVERIFY(state()->apply("Cut", [](Project& p, Sequence& s) { return edit::razorAll(p, s, 75); }));
        QVERIFY(win_->timeline()->isThroughEdit(again));
        QVERIFY(!win_->timeline()->isThroughEdit(clip));
        win_->findChild<QAction*>("joinThroughEdits")->trigger();
        QVERIFY(!win_->timeline()->isThroughEdit(again));
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(2));
        state()->undo();
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(3));
        state()->newProject();
    }

    void changedMediaReloads() {
        // A graphic in the cut, re-saved in another application.
        const QString png = dir_.path() + "/graphic.png";
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(qRgb(220, 20, 20));
        QVERIFY(img.save(png));
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        auto centre = [&]() {
            const Image f = renderSequenceFrame(state()->project(), *state()->sequence(), 10, {});
            const size_t i = (size_t(f.height / 2) * size_t(f.width) + size_t(f.width / 2)) * 4;
            return QColor::fromRgbF(std::clamp(f.px[i], 0.f, 1.f), std::clamp(f.px[i + 1], 0.f, 1.f), std::clamp(f.px[i + 2], 0.f, 1.f));
        };
        QVERIFY(centre().red() > 150);
        QSignalSpy spy(state(), &EditorState::mediaFileChanged);
        img.fill(qRgb(20, 20, 220));
        QVERIFY(img.save(png));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 5000);
        QCOMPARE(Id(spy.first().first().toULongLong()), ids[0]);
        QVERIFY2(centre().blue() > 150 && centre().red() < 80, qPrintable(centre().name()));
        // Saved larger: its new size is read.
        QImage big(128, 72, QImage::Format_RGB32);
        big.fill(qRgb(20, 200, 20));
        QVERIFY(big.save(png));
        QTRY_COMPARE_WITH_TIMEOUT(state()->project().findMedia(ids[0])->width, 128, 5000);
        QVERIFY(centre().green() > 150);
        state()->newProject();
    }

    void linkMediaOnOpen() {
        // A project whose card has moved since it was saved.
        const QString root = dir_.path() + "/link";
        QVERIFY(QDir().mkpath(root + "/card"));
        auto video = [&](const QString& file, double g, int frames) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", frames);
            c.generator.params["color.g"] = Param(g);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = file.toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        };
        video(root + "/card/take.mp4", 0.8, 30);
        video(root + "/other.mp4", 0.1, 90);
        QVERIFY(QFile::copy(QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav"), root + "/card/jfk.wav"));
        state()->newProject();
        const auto ids = state()->importFiles({root + "/card/take.mp4", root + "/card/jfk.wav"});
        QCOMPARE(ids.size(), size_t(2));
        const Id take = ids[0], jfk = ids[1];
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, take, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        QVERIFY(state()->save(root + "/cut.montage"));
        state()->newProject();
        QVERIFY(QDir().rename(root + "/card", root + "/Card 2"));

        // Opening it lists both files as offline, in the bin and on the timeline.
        QVERIFY(win_->openProject(root + "/cut.montage"));
        auto* dlg = win_->findChild<LinkMediaDialog*>("linkMedia");
        QVERIFY(dlg && dlg->isVisible());
        QCOMPARE(dlg->offline(), (std::vector<Id>{take, jfk}));
        QCOMPARE(dlg->findChild<QTableWidget*>("linkMediaList")->rowCount(), 2);
        QVERIFY(state()->isMediaOffline(take) && state()->isMediaOffline(jfk));
        auto* bin = win_->findChild<MediaBinWidget*>();
        const QModelIndex row = bin->model()->index(bin->model()->rowOf(take), 0);
        QVERIFY(row.data(Qt::ToolTipRole).toString().contains("Media offline"));

        // A file that is not the same footage is refused; locating the take finds the sound beside it, in one undo step.
        QCOMPARE(dlg->locate(take, root + "/other.mp4"), 0);
        QVERIFY(dlg->findChild<QLabel*>("linkStatus")->text().contains("does not match"));
        QCOMPARE(dlg->locate(take, root + "/Card 2/take.mp4"), 2);
        QTRY_VERIFY(!win_->findChild<LinkMediaDialog*>("linkMedia"));  // closed once everything was found
        QVERIFY(!state()->isMediaOffline(take) && !state()->isMediaOffline(jfk));
        QCOMPARE(state()->project().findMedia(jfk)->path, (root + "/Card 2/jfk.wav").toStdString());
        state()->undo();
        QVERIFY(state()->isMediaOffline(take) && state()->isMediaOffline(jfk));
        // Or search a folder for all of them.
        dlg = win_->showLinkMedia();
        QVERIFY(dlg);
        QCOMPARE(dlg->searchFolder(root), 2);
        QTRY_VERIFY(!win_->findChild<LinkMediaDialog*>("linkMedia"));
        QVERIFY(!win_->showLinkMedia());  // nothing offline
        // Replace Footage swaps in other footage under the same clips.
        QString why;
        QVERIFY2(bin->replaceFootage(take, root + "/other.mp4", &why), qPrintable(why));
        QCOMPARE(state()->project().findMedia(take)->name, std::string("other.mp4"));
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(state()->sequence()->videoTracks[0].clips[0].mediaId, take);
        QVERIFY(!bin->replaceFootage(take, root + "/Card 2/jfk.wav", &why));
        state()->newProject();
    }

    void subclipsFromTheSourceMonitor() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id jfk = ids[0];
        auto* bin = win_->findChild<MediaBinWidget*>();
        auto* make = win_->findChild<QAction*>("makeSubclip");
        QVERIFY(bin && make);
        const double fps = state()->sequence()->fpsValue();

        // In and Out in the Source monitor, then Make Subclip.
        state()->setSourceMedia(jfk);
        state()->setSourceIn(FrameTime(2 * fps));
        state()->setSourceOut(FrameTime(5 * fps) - 1);
        make->trigger();
        QCOMPARE(state()->project().media.size(), size_t(2));
        const MediaItem sub = state()->project().media.back();
        QCOMPARE(sub.subclipOf, jfk);
        QVERIFY(std::fabs(sub.subclipIn - 2) < 1e-9 && std::fabs(sub.subclipOut - 5) < 1e-9);
        QCOMPARE(sub.name, std::string("jfk.wav Subclip 1"));
        QCOMPARE(bin->shownMedia(), (std::vector<Id>{jfk, sub.id}));
        const QModelIndex row = bin->model()->index(1, MediaBinModel::columnOf("kind"));
        QCOMPARE(row.data().toString(), QString("Audio Subclip"));
        QVERIFY(bin->model()->index(1, 0).data(Qt::ToolTipRole).toString().contains("Subclip of jfk.wav"));
        state()->undo();
        QCOMPARE(state()->project().media.size(), size_t(1));
        state()->redo();

        // Opening it opens its media with In and Out around it.
        state()->setSourceMedia(0);
        state()->setSourceMedia(sub.id);
        QCOMPARE(state()->sourceMedia(), jfk);
        QCOMPARE(state()->sourceIn(), FrameTime(2 * fps));
        QCOMPARE(state()->sourceOut(), FrameTime(5 * fps) - 1);

        // Dragged to the timeline, it places that range of its media, under its name.
        bin->selectMedia({sub.id});
        std::unique_ptr<QMimeData> mime(bin->model()->mimeData({bin->model()->index(1, 0)}));
        ppf_ = measurePpf();
        const QPoint at = pointFor(0, A1);
        QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &enter);
        QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &move);
        QDropEvent drop(QPointF(at), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &drop);
        const Track& a1 = *trackAt(*state()->sequence(), A1);
        QCOMPARE(a1.clips.size(), size_t(1));
        const Clip& placed = a1.clips.front();
        QCOMPARE(placed.mediaId, jfk);
        QCOMPARE(placed.name, sub.name);
        QCOMPARE(placed.sourceIn, 2 * fps);
        QCOMPARE(placed.duration, FrameTime(3 * fps));
        bin->setView(MediaBinWidget::View::List);
        QCOMPARE(bin->model()->index(1, MediaBinModel::columnOf("usage")).data().toString(), QString("1"));
        bin->setView(MediaBinWidget::View::Icons);

        // Removing the media takes its subclips with it, and undo brings both back.
        QVERIFY(state()->removeMedia(jfk));
        QVERIFY(state()->project().media.empty());
        state()->undo();
        QCOMPARE(state()->project().media.size(), size_t(2));
        state()->newProject();
    }

    void autoDuckFromTheClipMenu() {
        // JFK's speech on A1 and, as "music", the same file on A2.
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Dialogue", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        QVERIFY(state()->apply("Music", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, -1, V1, {TrackKind::Audio, 1}, false);
        }));
        const Id music = trackAt(*state()->sequence(), {TrackKind::Audio, 1})->clips.front().id;
        // The dialog takes the other tracks with sound as dialogue.
        AutoDuckDialog dlg(state(), {music}, win_.get());
        QCOMPARE(dlg.dialogueTracks(), std::vector<int>{0});
        QCOMPARE(dlg.options().amountDb, -15.0);
        dlg.findChild<QDoubleSpinBox*>("duckAmount")->setValue(-12);
        QCOMPARE(dlg.options().amountDb, -12.0);
        // Applying writes the music's volume keyframes as one undo step.
        QCOMPARE(AutoDuckDialog::apply(state(), {music}, dlg.dialogueTracks(), dlg.options(), win_.get()), 1);
        const Param& g = edit::clipById(*state()->sequence(), music)->audio.params.at("gain_db");
        QVERIFY(g.animated());
        double lowest = 0;
        for (const Keyframe& k : g.keys) lowest = std::min(lowest, k.v);
        QCOMPARE(lowest, -12.0);
        QCOMPARE(state()->undoText(), QString("Auto Duck"));
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), music)->audio.params.count("gain_db") ||
                !edit::clipById(*state()->sequence(), music)->audio.params.at("gain_db").animated());
        QCOMPARE(AutoDuckDialog::apply(state(), {music}, {}, dlg.options(), win_.get()), -1);  // no dialogue tracks
        auto* action = win_->findChild<QAction*>("autoDuck");
        QVERIFY(action);
        state()->clearSelection();
        action->trigger();  // nothing selected: a message, no dialog
        state()->newProject();
    }

    void adjustmentLayerFromTheClipMenu() {
        loadDemo();
        state()->setPlayhead(20);
        const size_t tracks = state()->sequence()->videoTracks.size();
        auto* action = win_->findChild<QAction*>("newAdjustmentLayer");
        QVERIFY(action);
        // Above the targeted track (V1): on V2, overwriting what is there as a matte does.
        state()->setTargetVideoTrack(0);
        action->trigger();
        const Track& v2 = state()->sequence()->videoTracks.at(1);
        const auto it = std::find_if(v2.clips.begin(), v2.clips.end(), [](const Clip& c) { return c.generator.type == "adjustment"; });
        QVERIFY(it != v2.clips.end());
        QCOMPARE(it->start, FrameTime(20));
        QCOMPARE(state()->sequence()->videoTracks.size(), tracks);
        state()->undo();
        // From the top track, a new track is made for it.
        state()->setTargetVideoTrack(int(tracks) - 1);
        action->trigger();
        QCOMPARE(state()->sequence()->videoTracks.size(), tracks + 1);
        QCOMPARE(state()->sequence()->videoTracks.back().clips.at(0).generator.type, std::string("adjustment"));
        state()->undo();
        QCOMPARE(state()->sequence()->videoTracks.size(), tracks);
    }

    void gradingCurvesWheelsAndCompare() {
        auto mouse = [](QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButtons held) {
            QMouseEvent ev(type, pos, w->mapToGlobal(pos), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
            QApplication::sendEvent(w, &ev);
        };
        // A hue curve: click the line to add a point, drag it, double-click to remove it.
        {
            CurveEditor ed(CurveEditor::Mode::Hue);
            ed.resize(372, 112);
            QSignalSpy spy(&ed, &CurveEditor::edited);
            QCOMPARE(ed.count(), 0);
            const QPointF at = ed.toWidget({1.0 / 3, 0.5});
            mouse(&ed, QEvent::MouseButtonPress, at, Qt::LeftButton);
            QCOMPARE(ed.count(), 1);
            QVERIFY(ed.dragging());
            mouse(&ed, QEvent::MouseMove, ed.toWidget({1.0 / 3, 0.9}), Qt::LeftButton);
            mouse(&ed, QEvent::MouseButtonRelease, ed.toWidget({1.0 / 3, 0.9}), Qt::NoButton);
            QVERIFY(!ed.dragging());
            QVERIFY(spy.size() >= 3);
            QCOMPARE(spy.last().at(1).toBool(), true);
            QVERIFY(std::fabs(ed.valueAt(1.0 / 3) - 0.9) < 0.02);
            // One point is a level everywhere (and round the wrap).
            QVERIFY(std::fabs(ed.valueAt(0.9) - ed.valueAt(1.0 / 3)) < 1e-3);
            mouse(&ed, QEvent::MouseButtonDblClick, ed.toWidget({1.0 / 3, ed.valueAt(1.0 / 3)}), Qt::LeftButton);
            QCOMPARE(ed.count(), 0);
            QCOMPARE(spy.last().at(0).toString(), QString());
        }
        // A tone curve keeps its ends: they move up and down but not across, and cannot be removed.
        {
            CurveEditor ed(CurveEditor::Mode::Tone);
            ed.resize(212, 140);
            QCOMPARE(ed.points(), QString("0,0 1,1"));
            mouse(&ed, QEvent::MouseButtonPress, ed.toWidget({0, 0}), Qt::LeftButton);
            mouse(&ed, QEvent::MouseMove, ed.toWidget({0.2, 0.1}), Qt::LeftButton);
            mouse(&ed, QEvent::MouseButtonRelease, ed.toWidget({0.2, 0.1}), Qt::NoButton);
            QVERIFY(ed.points().startsWith("0,0.1"));
            mouse(&ed, QEvent::MouseButtonDblClick, ed.toWidget({1, 1}), Qt::LeftButton);
            QCOMPARE(ed.count(), 2);
        }
        // Wheel geometry: a puck maps to balanced offsets and back.
        {
            double r, g, b;
            ColorWheel::puckToRgb({0.3, 0.4}, 0.25, r, g, b);
            QVERIFY(std::fabs(r + g + b) < 1e-9);
            const QPointF back = ColorWheel::rgbToPuck(r + 0.1, g + 0.1, b + 0.1, 0.25);  // the shared part is ignored
            QVERIFY(std::fabs(back.x() - 0.3) < 1e-9 && std::fabs(back.y() - 0.4) < 1e-9);
            ColorWheel::puckToRgb({1, 0}, 0.5, r, g, b);
            QVERIFY(std::fabs(r - 0.5) < 1e-9 && std::fabs(g + 0.25) < 1e-9 && std::fabs(b + 0.25) < 1e-9);
        }

        // In the Inspector: Hue Curves and Color Correct on a clip.
        QImage frame(320, 180, QImage::Format_RGB32);
        frame.fill(QColor(200, 60, 40));
        const QString png = dir_.path() + "/grade.png";
        QVERIFY(frame.save(png));
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->edit("Grade", [clip](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, clip);
            c->effects.push_back(makeEffect(p, "hue_curves"));
            Effect cc = makeEffect(p, "color_correct");
            for (const char* n : {"gain_r", "gain_g", "gain_b"}) cc.params[n] = Param(1.2);
            c->effects.push_back(cc);
            return true;
        });
        state()->setSelection({clip}, false);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        auto effectOf = [&](const char* type) -> const Effect* {
            for (const Effect& e : edit::clipById(*state()->sequence(), clip)->effects)
                if (e.type == type) return &e;
            return nullptr;
        };
        auto visible = [&]<typename W>(const char* name) -> W* {
            for (auto* w : win_->findChildren<W*>(name))
                if (w->isVisibleTo(win_.get())) return w;
            return nullptr;
        };
        auto* hueSat = visible.template operator()<CurveEditor>("curve_hue_sat");
        QVERIFY(hueSat);
        QVERIFY(hueSat->width() > 40 && hueSat->height() > 40);
        // Drag the reds' saturation down: one undoable change.
        mouse(hueSat, QEvent::MouseButtonPress, hueSat->toWidget({0.02, 0.5}), Qt::LeftButton);
        for (double y : {0.4, 0.3, 0.2})
            mouse(hueSat, QEvent::MouseMove, hueSat->toWidget({0.02, y}), Qt::LeftButton);
        mouse(hueSat, QEvent::MouseButtonRelease, hueSat->toWidget({0.02, 0.2}), Qt::NoButton);
        QApplication::processEvents();
        QVERIFY(!effectOf("hue_curves")->s("hue_sat").empty());
        RenderOptions o;
        o.displaySpace = "rec709";
        auto centre = [&] {
            const Image img = renderProgramFrame(state()->project(), *state()->sequence(), 5, o);
            const float* p = img.at(img.width / 2, img.height / 2);
            return std::array<float, 3>{p[0], p[1], p[2]};
        };
        const auto graded = centre();
        QVERIFY2(graded[0] - graded[2] < 0.5f, qPrintable(QString("%1 %2 %3").arg(graded[0]).arg(graded[1]).arg(graded[2])));
        state()->undo();
        QVERIFY(effectOf("hue_curves")->s("hue_sat").empty());
        QVERIFY(centre()[0] - centre()[2] > graded[0] - graded[2] + 0.1f);
        state()->redo();
        QApplication::processEvents();

        // The Gain wheel pushed to red at the rim: the channels split about where they were.
        auto* gain = visible.template operator()<ColorWheel>("wheel_gain");
        QVERIFY(gain && visible.template operator()<ColorWheel>("wheel_lift") && visible.template operator()<ColorWheel>("wheel_gamma"));
        gain->setPuck({1, 0}, true);
        const Effect* cc = effectOf("color_correct");
        QVERIFY(std::fabs(cc->p("gain_r", 0) - 1.7) < 1e-6);
        QVERIFY(std::fabs(cc->p("gain_g", 0) - 0.95) < 1e-6);
        QVERIFY(std::fabs(cc->p("gain_b", 0) - 0.95) < 1e-6);
        QApplication::processEvents();
        gain = visible.template operator()<ColorWheel>("wheel_gain");
        QVERIFY(std::fabs(gain->puck().x() - 1) < 1e-6 && std::fabs(gain->puck().y()) < 1e-6);
        state()->undo();
        QVERIFY(std::fabs(effectOf("color_correct")->p("gain_r", 0) - 1.2) < 1e-6);
        state()->redo();
        QApplication::processEvents();
        // Back to the centre (double-click): only the balance goes, the shared 1.2 stays.
        gain = visible.template operator()<ColorWheel>("wheel_gain");
        mouse(gain, QEvent::MouseButtonDblClick, QPointF(gain->width() / 2.0, 10), Qt::LeftButton);
        cc = effectOf("color_correct");
        for (const char* n : {"gain_r", "gain_g", "gain_b"}) QVERIFY(std::fabs(cc->p(n, 0) - 1.2) < 1e-6);

        // Compare with Reference: off until there is a reference.
        auto* compare = win_->findChild<QAction*>("compareReference");
        QVERIFY(compare && compare->isCheckable());
        ViewerWidget* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m->viewer();
        QVERIFY(program);
        compare->trigger();
        QVERIFY(!compare->isChecked() && !program->comparing());
        state()->setPlayhead(5);
        win_->findChild<QAction*>("setColourReference")->trigger();
        compare->trigger();
        QVERIFY(compare->isChecked() && program->comparing());
        // The reference left of the divider, the picture right of it.
        QImage blue(320, 180, QImage::Format_RGB32);
        blue.fill(QColor(20, 40, 220));
        program->setImage(blue);
        program->resize(400, 225);
        QCOMPARE(program->split(), 0.5);
        auto shot = [&] { return program->grab().toImage(); };
        const QRectF r = program->imageRect();
        QImage img = shot();
        const QColor left = img.pixelColor(int(r.left() + r.width() * 0.25), int(r.center().y()));
        const QColor right = img.pixelColor(int(r.left() + r.width() * 0.75), int(r.center().y()));
        QVERIFY2(left.red() > left.blue() && right.blue() > right.red(), qPrintable(left.name() + " " + right.name()));
        // Dragging the divider wipes further across.
        const QPointF div(program->dividerX(), r.center().y());
        mouse(program, QEvent::MouseButtonPress, div, Qt::LeftButton);
        mouse(program, QEvent::MouseMove, QPointF(r.left() + r.width() * 0.8, r.center().y()), Qt::LeftButton);
        mouse(program, QEvent::MouseButtonRelease, QPointF(r.left() + r.width() * 0.8, r.center().y()), Qt::NoButton);
        QVERIFY(std::fabs(program->split() - 0.8) < 0.02);
        img = shot();
        const QColor nowLeft = img.pixelColor(int(r.left() + r.width() * 0.75), int(r.center().y()));
        QVERIFY(nowLeft.red() > nowLeft.blue());
        compare->trigger();
        QVERIFY(!compare->isChecked() && !program->comparing());
        program->setSplit(0.5);
        state()->newProject();
    }

    void depthEffectsInTheApp() {
        // Listed under Depth in the effects browser.
        auto* browser = win_->findChild<EffectsBrowser*>();
        QVERIFY(browser);
        browser->reload();
        QSet<QString> listed;
        for (QTreeWidgetItem* item : browser->findChild<QTreeWidget*>()->findItems("Depth", Qt::MatchRecursive))
            for (int i = 0; i < item->childCount(); ++i) listed.insert(item->child(i)->data(0, Qt::UserRole).toString());
        QVERIFY2(listed.contains("depth_blur") && listed.contains("depth_fog") && listed.contains("depth_map"), qPrintable(QStringList(listed.values()).join(",")));
        if (!depthAvailable()) QSKIP("Built without ONNX Runtime");
        // A depth qualifier offers the model while it is missing.
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Depth", [red](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "color_correct");
            e.params["mask.depth"] = Param(1.0);
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        const QByteArray saved = qgetenv("MONTAGE_DEPTH_MODEL");
        qputenv("MONTAGE_DEPTH_MODEL", (dir_.path() + "/no-depth-model").toUtf8());
        state()->setSelection({});
        state()->setSelection({red});
        QTRY_VERIFY(win_->findChild<QPushButton*>("getDepthModel"));
        if (saved.isEmpty()) qunsetenv("MONTAGE_DEPTH_MODEL");
        else qputenv("MONTAGE_DEPTH_MODEL", saved);
        if (!depthModel().installed()) QSKIP("Set MONTAGE_DEPTH_MODEL to test with the model");
        QVERIFY(ensureEffectModel(win_.get(), "depth_blur"));
        QVERIFY(ensureEffectModel(win_.get(), "mask.depth"));
        state()->setSelection({});
        state()->setSelection({red});
        QTRY_VERIFY(!win_->findChild<QPushButton*>("getDepthModel"));
    }

    void faceRefinementInTheBrowser() {
        auto* browser = win_->findChild<EffectsBrowser*>();
        browser->reload();
        bool listed = false;
        for (QTreeWidgetItem* item : browser->findChild<QTreeWidget*>()->findItems("Refine", Qt::MatchRecursive))
            for (int i = 0; i < item->childCount(); ++i) listed |= item->child(i)->data(0, Qt::UserRole).toString() == "face_refine";
        QVERIFY(listed);
        bool removal = false;
        for (QTreeWidgetItem* item : browser->findChild<QTreeWidget*>()->findItems("Refine", Qt::MatchRecursive))
            for (int i = 0; i < item->childCount(); ++i) removal |= item->child(i)->data(0, Qt::UserRole).toString() == "object_removal";
        QVERIFY(removal);
        if (inpaintAvailable() && inpaintModel().installed()) QVERIFY(ensureEffectModel(win_.get(), "object_removal"));
        if (!faceSearchAvailable() || !faceModel().installed()) QSKIP("Set MONTAGE_FACE_MODEL to test with the model");
        QVERIFY(ensureEffectModel(win_.get(), "face_refine"));
    }

    void generateVoiceover() {
        loadDemo();
        QAction* act = win_->findChild<QAction*>("generateVoiceover");
        QVERIFY(act);
        if (!ttsAvailable() || !ttsModel().installed()) QSKIP("Set MONTAGE_TTS_MODEL to the Kokoro speech pack");
        state()->setPlayhead(15);
        SpeechDialog dlg(state(), win_.get());
        dlg.findChild<QPlainTextEdit*>("speechText")->setPlainText("Testing the voiceover.");
        auto* voice = dlg.findChild<QComboBox*>("speechVoice");
        voice->setCurrentIndex(voice->findData("am_michael"));
        const int track = dlg.findChild<QComboBox*>("speechTrack")->currentData().toInt();
        const auto clips = dlg.generate();
        QCOMPARE(int(clips.size()), 1);
        const Clip* c = edit::clipById(*state()->sequence(), clips[0]);
        QVERIFY(c);
        QCOMPARE(c->start, FrameTime(15));
        QCOMPARE(edit::locate(*state()->sequence(), clips[0])->track.index, track);
        const MediaItem* m = state()->project().findMedia(c->mediaId);
        QVERIFY(m && m->bin == "Voiceover" && QFileInfo::exists(QString::fromStdString(m->path)));
        // One undo step takes it away.
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), clips[0]));
    }

    void peopleMaskOffersItsModel() {
        if (!mattingAvailable()) QSKIP("Built without ONNX Runtime");
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("People", [red](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "color_correct");
            e.params["mask.shape"] = Param(4.0);
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        const QByteArray saved = qgetenv("MONTAGE_MATTE_MODEL");
        qputenv("MONTAGE_MATTE_MODEL", (dir_.path() + "/no-matte").toUtf8());
        state()->setSelection({});
        state()->setSelection({red});
        QTRY_VERIFY(win_->findChild<QPushButton*>("getMatteModel"));
        if (saved.isEmpty()) qunsetenv("MONTAGE_MATTE_MODEL");
        else qputenv("MONTAGE_MATTE_MODEL", saved);
        if (!mattingModel().installed()) QSKIP("Set MONTAGE_MATTE_MODEL to test with the model");
        QVERIFY(ensureEffectModel(win_.get(), "remove_background"));
        state()->setSelection({});
        state()->setSelection({red});
        QTRY_VERIFY(!win_->findChild<QPushButton*>("getMatteModel"));
    }

    void aiFramesOfferTheirModel() {
        if (!rifeAvailable()) QSKIP("Built without ONNX Runtime");
        // A video clip at half speed with AI frames chosen.
        const QString video = dir_.path() + "/ai-frames.mp4";
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", 10);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = video.toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        QVERIFY(state()->edit("Slow", [clip](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, clip);
            if (c->timing.empty()) c->timing = makeEffect(p, "time");
            c->timing.params["sampling"] = Param(3.0);
            return true;
        }));
        const QByteArray saved = qgetenv("MONTAGE_RIFE_MODEL");
        qputenv("MONTAGE_RIFE_MODEL", (dir_.path() + "/no-rife").toUtf8());
        state()->setSelection({});
        state()->setSelection({clip});
        QTRY_VERIFY(win_->findChild<QPushButton*>("getRifeModel"));
        if (saved.isEmpty()) qunsetenv("MONTAGE_RIFE_MODEL");
        else qputenv("MONTAGE_RIFE_MODEL", saved);
        if (!rifeModel().installed()) QSKIP("Set MONTAGE_RIFE_MODEL to test with the model");
        QVERIFY(ensureEffectModel(win_.get(), "rife"));
        state()->setSelection({});
        state()->setSelection({clip});
        QTRY_VERIFY(!win_->findChild<QPushButton*>("getRifeModel"));
        state()->newProject();
    }

    void enhanceSpeechAsksForItsModel() {
        // Effects without a model need nothing; Enhance Speech is ready once its model is here.
        QVERIFY(ensureEffectModel(win_.get(), "denoise"));
        if (!speechEnhancerAvailable() || !speechModel().installed()) QSKIP("Set MONTAGE_SPEECH_MODEL to test with the model");
        QVERIFY(ensureEffectModel(win_.get(), "enhance_speech"));
        const EffectInfo* info = findEffectInfo("enhance_speech");
        QVERIFY(info && !info->hidden);
        QVERIFY(isSourceAudioEffect("enhance_speech"));
    }

    void matchVoiceToAReference() {
        // JFK, and JFK as if on a thin, bright microphone.
        std::vector<float> ref;
        std::string err;
        QVERIFY2(decodeMono(MONTAGE_TEST_DATA_DIR "/jfk.wav", 48000, ref, nullptr, &err), err.c_str());
        fx::ParametricEq mic;
        mic.set(48000, {120, -8, 1}, {300, 0, 0.9}, {1200, 0, 0.9}, {4000, 6, 0.9}, {8000, 0, 1}, 0);
        std::vector<float> st(ref.size() * 2);
        for (size_t i = 0; i < ref.size(); ++i) st[i * 2] = st[i * 2 + 1] = ref[i];
        mic.process(st.data(), int(ref.size()));
        std::vector<float> thin(ref.size());
        for (size_t i = 0; i < ref.size(); ++i) thin[i] = st[i * 2];
        const QString wav = dir_.path() + "/thin.wav";
        {
            WavWriter w;
            QVERIFY(w.open(wav, 48000, 1));
            w.write(thin.data(), qint64(thin.size()));
            QVERIFY(w.close());
        }
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav"), wav});
        QCOMPARE(ids.size(), size_t(2));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            return edit::placeMedia(p, s, ids[1], 400, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Sequence& s = *state()->sequence();
        QCOMPARE(s.audioTracks[0].clips.size(), size_t(2));
        const Id good = s.audioTracks[0].clips[0].id, other = s.audioTracks[0].clips[1].id;
        QVERIFY(win_->findChild<QAction*>("setVoiceReference") && win_->findChild<QAction*>("matchVoice"));
        // No reference yet: nothing happens.
        state()->setSelection({other}, false);
        QCOMPARE(win_->matchVoice(), 0);
        state()->setSelection({good}, false);
        QVERIFY(win_->setVoiceReference());
        state()->setSelection({other}, false);
        QCOMPARE(win_->matchVoice(), 1);
        auto eqOf = [&] {
            const Clip* c = edit::clipById(*state()->sequence(), other);
            return c && !c->effects.empty() && c->effects[0].type == "parametric_eq" ? &c->effects[0] : nullptr;
        };
        QVERIFY(eqOf());
        QCOMPARE(eqOf()->s("match"), std::string("voice"));
        QVERIFY2(std::fabs(eqOf()->p("low_db", 0) - 8) < 2, qPrintable(QString::number(eqOf()->p("low_db", 0))));
        QVERIFY(std::fabs(eqOf()->p("b3_db", 0) + 6) < 2);
        // Matching again replaces the EQ; one undo takes it off.
        QCOMPARE(win_->matchVoice(), 1);
        QCOMPARE(edit::clipById(*state()->sequence(), other)->effects.size(), size_t(1));
        state()->undo();
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), other)->effects.empty());
        state()->newProject();
    }

    void translateCaptionTrack() {
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        QVERIFY(panel->findChild<QAction*>("translateCaptions"));
        const ModelPack* ende = translationModel("en", "de");
        if (!translatorAvailable() || !ende || !ende->installed()) QSKIP("Set MONTAGE_TRANSLATION_MODELS to test with the en-de model");
        state()->newProject();
        Id track = 0;
        state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            t.captions = {{0, 45, "Hello world", {}}, {45, 120, "The meeting starts at nine o'clock tomorrow morning.", {}}};
            s.captionTracks.push_back(t);
            return true;
        });
        panel->setCurrentTrack(track);
        QString error;
        const Id made = panel->translateTrack("de", &error);
        QVERIFY2(made, qPrintable(error));
        const Sequence& s = *state()->sequence();
        QCOMPARE(s.captionTracks.size(), size_t(2));
        QCOMPARE(panel->currentTrack(), made);
        QCOMPARE(s.captionTracks[1].language, std::string("de"));
        QCOMPARE(QString::fromStdString(s.captionTracks[1].captions[0].text), QString("Hallo Welt"));
        QCOMPARE(s.captionTracks[1].captions[1].start, FrameTime(45));
        // One undo removes it.
        state()->undo();
        QCOMPARE(state()->sequence()->captionTracks.size(), size_t(1));
        state()->newProject();
    }

    void dubCaptionTrack() {
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        QVERIFY(panel->findChild<QAction*>("dubCaptions"));
        const ModelPack* deen = translationModel("de", "en");
        if (!translatorAvailable() || !deen || !deen->installed() || !ttsAvailable() || !ttsModel().installed())
            QSKIP("Set MONTAGE_TRANSLATION_MODELS (with translate-de-en) and MONTAGE_TTS_MODEL to test dubbing");
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        Id track = 0;
        state()->apply("Place", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            t.language = "de";
            t.captions = {{0, 45, "Hallo Welt", {}}};
            s.captionTracks.push_back(t);
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        panel->setCurrentTrack(track);
        const int tracks = int(state()->sequence()->audioTracks.size());
        QString error;
        const auto r = panel->dub("af_heart", -12, &error);
        QVERIFY2(!r.clips.empty(), qPrintable(error));
        const Sequence& s = *state()->sequence();
        QCOMPARE(s.captionTracks.size(), size_t(2));
        QCOMPARE(s.captionTracks[1].id, r.captions);
        QCOMPARE(r.audioTrack, tracks);
        QCOMPARE(s.audioTracks[size_t(tracks)].name, std::string("Dub (English)"));
        QCOMPARE(r.ducked, 1);
        QVERIFY(std::abs(s.audioTracks[0].clips[0].audio.params.at("gain_db").at(5) + 12) < 0.01);
        // One undo takes the speech and the ducking away together; then the import of its sound files, then the translation.
        state()->undo();
        QCOMPARE(int(state()->sequence()->audioTracks.size()), tracks);
        const auto& params = state()->sequence()->audioTracks[0].clips[0].audio.params;
        QVERIFY(!params.count("gain_db") || params.at("gain_db").keys.empty());
        QCOMPARE(state()->sequence()->captionTracks.size(), size_t(2));
        state()->undo();
        QCOMPARE(state()->sequence()->captionTracks.size(), size_t(2));
        state()->undo();
        QCOMPARE(state()->sequence()->captionTracks.size(), size_t(1));
        state()->newProject();
    }

    void autoMixDialog() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            return edit::placeMedia(p, s, ids[0], 60, 0, -1, {TrackKind::Video, 1}, {TrackKind::Audio, 1}, false);
        });
        QVERIFY(win_->findChild<QAction*>("autoMix"));
        auto plan = planMix(state()->project(), *state()->sequence(), MixOptions{});
        QCOMPARE(plan.size(), size_t(2));
        AutoMixDialog dlg(state(), plan, win_.get());
        auto* table = dlg.findChild<QTableWidget*>("autoMixClips");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 2);
        auto* role0 = dlg.findChild<QComboBox*>("autoMixRole0");
        QVERIFY(role0);
        QCOMPARE(role0->currentText(), QString("Dialogue"));
        const double web = dlg.plan()[0].gainDb;
        QVERIFY(std::fabs(dlg.plan()[0].guess.loudness + web - (-18)) < 0.01);
        // Broadcast levels: 7 dB lower all round.
        dlg.findChild<QComboBox*>("autoMixPreset")->setCurrentIndex(2);
        QVERIFY(std::fabs(dlg.plan()[0].gainDb - (web - 7)) < 0.01);
        // Calling the second clip music sets it to the music level.
        dlg.setRole(1, AudioRole::Music);
        QCOMPARE(int(dlg.plan()[1].role), int(AudioRole::Music));
        QVERIFY(std::fabs(dlg.plan()[1].guess.loudness + dlg.plan()[1].gainDb - dlg.options().musicLufs) < 0.01);
        QCOMPARE(dlg.findChild<QComboBox*>("autoMixRole1")->currentText(), QString("Music"));
        // Applied as one step: the music dips where the dialogue speaks.
        QCOMPARE(AutoMixDialog::apply(state(), dlg.plan(), dlg.options()), 2);
        const Sequence& s = *state()->sequence();
        const Clip& music = s.audioTracks[1].clips[0];
        QVERIFY(music.audio.params.at("gain_db").animated());
        QVERIFY(std::fabs(s.audioTracks[0].clips[0].audio.params.at("gain_db").at(0) - dlg.plan()[0].gainDb) < 7);
        state()->undo();
        QVERIFY(!state()->sequence()->audioTracks[1].clips[0].audio.params.count("gain_db") ||
                !state()->sequence()->audioTracks[1].clips[0].audio.params.at("gain_db").animated());
        state()->newProject();
    }

    void beatMarkersAndFittingMusic() {
        // A 128 BPM song: kick on each bar, a tick on every beat, chords changing by bar.
        const int rate = 48000;
        const double beat = 60.0 / 128, bar = 4 * beat, lead = 0.5;
        const std::vector<int> barChord = {0, 0, 1, 2, 1, 2, 0, 3, 0, 3, 1, 2, 1, 2, 0, 3, 0, 3, 3, 3};
        const std::vector<std::vector<double>> hz = {{261.6, 329.6, 392.0}, {220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}, {196.0, 246.9, 293.7}};
        const int bars = int(barChord.size());
        const double total = lead + bars * bar + 1.0;
        std::vector<float> x(size_t(total * rate), 0.0f);
        unsigned seed = 3;
        for (int b = 0; b < bars; ++b) {
            for (int k = 0; k < 4; ++k) {
                const size_t i0 = size_t((lead + b * bar + k * beat) * rate);
                for (size_t i = 0; i < size_t(0.03 * rate); ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    x[i0 + i] += float((double(seed >> 8) / (1 << 24) - 0.5) * 0.3 * std::exp(-double(i) / (0.008 * rate)));
                }
                if (k == 0)
                    for (size_t i = 0; i < size_t(0.2 * rate); ++i)
                        x[i0 + i] += float(0.6 * std::sin(2 * M_PI * 55 * double(i) / rate) * std::exp(-double(i) / (0.06 * rate)));
            }
            for (size_t i = 0; i < size_t(bar * rate); ++i) {
                const double t = lead + b * bar + double(i) / rate;
                double v = 0;
                for (double f : hz[size_t(barChord[size_t(b)])]) v += std::sin(2 * M_PI * f * t);
                x[size_t((lead + b * bar) * rate) + i] += float(0.06 * v);
            }
        }
        const QString wav = dir_.path() + "/song.wav";
        {
            WavWriter w;
            QVERIFY(w.open(wav, rate, 1));
            w.write(x.data(), qint64(x.size()));
            QVERIFY(w.close());
        }
        state()->newProject();
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Sequence* s = state()->sequence();
        const double fps = s->fpsValue();
        QCOMPARE(s->audioTracks[0].clips.size(), size_t(1));
        const Id clip = s->audioTracks[0].clips[0].id;
        state()->setSelection({clip}, false);

        // Bar markers on every bar, each on a bar of the music; beat markers four times as many.
        QVERIFY(win_->findChild<QAction*>("addBarMarkers") && win_->findChild<QAction*>("fitMusic"));
        // (The chords stopping dead at the end make one more hit: a final bar, as songs often end.)
        const int barMarks = win_->addBeatMarkers(false);
        QVERIFY2(barMarks == bars || barMarks == bars + 1, qPrintable(QString::number(barMarks)));
        for (const Marker& m : state()->sequence()->markers) {
            QVERIFY(QString::fromStdString(m.name).startsWith("Bar "));
            QVERIFY2(std::fabs(std::remainder(m.t / fps - lead, bar)) <= 0.5 / fps + 0.01, qPrintable(QString::number(m.t)));
        }
        QCOMPARE(state()->sequence()->markers.front().name, std::string("Bar 1"));
        QVERIFY(win_->addBeatMarkers(true) >= 4 * bars - 1);
        QVERIFY(state()->sequence()->markers.size() >= size_t(4 * bars - 1));  // the bar markers were replaced
        QCOMPARE(state()->sequence()->markers[1].name, std::string("1.2"));

        // Fit to 30 s: pieces back to back, crossfaded, each join a whole number of bars apart in the music.
        QVERIFY(win_->fitMusicToLength(FrameTime(30 * fps)));
        s = state()->sequence();
        const auto& pieces = s->audioTracks[0].clips;
        QVERIFY(pieces.size() >= 2);
        const FrameTime length = pieces.back().end() - pieces.front().start;
        QVERIFY2(std::fabs(length / fps - 30) <= bar / 2 + 1.0 / fps, qPrintable(QString::number(length / fps)));
        QCOMPARE(s->audioTracks[0].transitions.size(), pieces.size() - 1);
        for (size_t i = 0; i + 1 < pieces.size(); ++i) {
            QCOMPARE(pieces[i + 1].start, pieces[i].end());
            const double cut = (pieces[i].sourceIn + double(pieces[i].duration)) / fps, resume = pieces[i + 1].sourceIn / fps;
            QVERIFY2(std::fabs(std::remainder(resume - cut, bar)) < 0.02, qPrintable(QString("%1 -> %2").arg(cut).arg(resume)));
            QVERIFY(std::fabs(std::remainder(cut - lead, bar)) < 0.03 + 0.5 / fps);
        }
        QCOMPARE(pieces.front().sourceIn, 0.0);
        // One undo puts the clip back whole.
        state()->undo();
        QCOMPARE(state()->sequence()->audioTracks[0].clips.size(), size_t(1));
        QCOMPARE(state()->sequence()->audioTracks[0].clips[0].id, clip);
        state()->newProject();
    }

    void buildACutFromAScript() {
        state()->newProject();
        const Id before = state()->sequence()->id;
        // Two transcribed takes (the files need not exist: the cut is built from transcripts).
        auto take = [&](const char* name, std::vector<TranscriptWord> words) {
            Id id = 0;
            state()->edit("Take", [&](Project& p, Sequence&) {
                MediaItem m;
                m.id = id = p.newId();
                m.kind = MediaKind::Video;
                m.name = name;
                m.path = (dir_.path() + "/" + name + ".mp4").toStdString();
                m.hasVideo = m.hasAudio = true;
                m.duration = 20;
                m.width = 1280;
                m.height = 720;
                m.fps = {25, 1};
                auto t = std::make_shared<Transcript>();
                TranscriptSegment seg;
                seg.words = std::move(words);
                t->segments.push_back(seg);
                m.transcript = t;
                p.media.push_back(m);
                return true;
            });
            return id;
        };
        const Id a = take("A", {{1.0, 1.3, "Hello", 1}, {1.4, 1.8, "there", 1}, {5.0, 5.3, "General", 1}, {5.4, 5.8, "Kenobi", 1}});
        const Id b = take("B", {{2.0, 2.3, "Hello", 1}, {2.4, 2.8, "there", 1}});
        QVERIFY(win_->findChild<QAction*>("buildScriptCut"));
        ScriptCutDialog dlg(state(), {b}, win_.get());
        dlg.setScript("OBI-WAN\nHello there.\n\nGRIEVOUS\nGeneral Kenobi.\n\nA line nobody said.");
        // Searching only the selected take: one line found.
        auto* preview = dlg.findChild<QTreeWidget*>("scriptPreview");
        QVERIFY(preview);
        QCOMPARE(preview->topLevelItemCount(), 3);
        QCOMPARE(dlg.matches()[0].takes.size(), size_t(1));
        QVERIFY(dlg.findChild<QLabel*>("scriptSummary")->text().contains("1 of 3"));
        // Every take: two lines found, the first with two readings.
        dlg.findChild<QComboBox*>("scriptScope")->setCurrentIndex(0);
        QCOMPARE(dlg.matches()[0].takes.size(), size_t(2));
        QCOMPARE(dlg.matches()[1].takes.front().mediaId, a);
        QCOMPARE(preview->topLevelItem(2)->text(2), QString("Not found"));
        QVERIFY(dlg.findChild<QLabel*>("scriptSummary")->text().contains("2 of 3"));
        // Building makes a new sequence, sized like the one that was open, and opens it.
        const ScriptCutResult r = ScriptCutDialog::build(state(), dlg.script(), dlg.options(), dlg.sequenceName());
        QVERIFY(r.sequence);
        QCOMPARE(r.placed, 2);
        QCOMPARE(r.missing, 1);
        QCOMPARE(r.alternates, 1);
        QCOMPARE(state()->sequence()->id, r.sequence);
        QCOMPARE(state()->sequence()->name, std::string("Script Cut"));
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(state()->sequence()->markers.size(), size_t(3));
        // One undo removes it.
        state()->undo();
        QVERIFY(!state()->project().findSequence(r.sequence));
        QCOMPARE(state()->sequence()->id, before);
        state()->newProject();
    }

    void matchColourToAReference() {
        // The same scene twice: graded warm and lifted (the hero shot), and flat.
        QImage flat(320, 180, QImage::Format_RGB32), hero(320, 180, QImage::Format_RGB32);
        std::mt19937 rng(9);
        std::uniform_int_distribution<int> px(0, 300), sz(6, 30), col(10, 245);
        flat.fill(QColor(110, 110, 110));
        {
            QPainter pa(&flat);
            for (int i = 0; i < 200; ++i) pa.fillRect(px(rng), px(rng) * 180 / 300, sz(rng), sz(rng), QColor(col(rng), col(rng), col(rng)));
        }
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                const QRgb c = flat.pixel(x, y);
                auto f = [](int v) { return v / 255.0; };
                const double r = 0.08 + 0.9 * f(qRed(c)), g = std::pow(f(qGreen(c)), 1.15), b = 0.78 * f(qBlue(c));
                hero.setPixel(x, y, qRgb(int(std::lround(255 * std::min(1.0, r))), int(std::lround(255 * g)), int(std::lround(255 * b))));
            }
        const QString flatPng = dir_.path() + "/flat.png", heroPng = dir_.path() + "/hero.png";
        QVERIFY(flat.save(flatPng) && hero.save(heroPng));
        state()->newProject();
        const auto ids = state()->importFiles({heroPng, flatPng});
        QCOMPARE(ids.size(), size_t(2));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            return edit::placeMedia(p, s, ids[1], 30, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Sequence& s = *state()->sequence();
        QCOMPARE(s.videoTracks[0].clips.size(), size_t(2));
        const Id target = s.videoTracks[0].clips[1].id;
        auto* setRef = win_->findChild<QAction*>("setColourReference");
        auto* match = win_->findChild<QAction*>("matchColour");
        QVERIFY(setRef && match);
        RenderOptions o;
        o.displaySpace = "rec709";
        auto diff = [&](FrameTime a, FrameTime b) {
            const Image ia = renderProgramFrame(state()->project(), *state()->sequence(), a, o);
            const Image ib = renderProgramFrame(state()->project(), *state()->sequence(), b, o);
            double d = 0;
            for (size_t i = 0; i < ia.px.size(); i += 4)
                for (int k = 0; k < 3; ++k) d += std::fabs(ia.px[i + k] - ib.px[i + k]);
            return d / double(ia.px.size() / 4 * 3);
        };
        const double before = diff(10, 40);
        QVERIFY(before > 0.04);
        // Park on the hero shot and take it as the reference; then match the flat one.
        state()->setPlayhead(10);
        setRef->trigger();
        QVERIFY(win_->hasColourReference());
        state()->setSelection({target}, false);
        match->trigger();
        const Clip* c = edit::clipById(*state()->sequence(), target);
        QCOMPARE(c->effects.size(), size_t(1));
        QCOMPARE(c->effects[0].type, std::string("color_correct"));
        QVERIFY(c->effects[0].strings.count("match"));
        const double after = diff(10, 40);
        QVERIFY2(after < 0.01 && after < before * 0.2, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        // Matching again replaces the match rather than stacking another; one undo removes it.
        match->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), target)->effects.size(), size_t(1));
        state()->undo();
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), target)->effects.empty());
        state()->newProject();
    }

    void autoReframeASequence() {
        // A still with its subject (a red disc) well right of centre.
        QImage img(640, 360, QImage::Format_RGB32);
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) img.setPixel(x, y, qRgb(90 + x / 16, 100, 110 - y / 12));
        {
            QPainter pa(&img);
            pa.setRenderHint(QPainter::Antialiasing);
            pa.setBrush(QColor(220, 40, 30));
            pa.setPen(Qt::NoPen);
            pa.drawEllipse(QPointF(520, 180), 45, 45);
        }
        const QString png = dir_.path() + "/subject.png";
        QVERIFY(img.save(png));
        state()->newProject();
        state()->edit("Size", [](Project&, Sequence& s) {
            s.width = 640;
            s.height = 360;
            return true;
        });
        const auto ids = state()->importFiles({png});
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 50, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id original = state()->sequence()->id;
        const size_t sequences = state()->project().sequences.size();
        QVERIFY(win_->findChild<QAction*>("autoReframeSequence"));
        const Id made = win_->autoReframeSequence(9, 16, 1);
        QVERIFY(made);
        QCOMPARE(state()->sequence()->id, made);
        QCOMPARE(state()->project().sequences.size(), sequences + 1);
        QCOMPARE(state()->sequence()->width, 360);
        QCOMPARE(state()->sequence()->height, 640);
        // The disc is brought to the middle: the picture (1138 px wide) moves left by about 355 px.
        const Clip& c = state()->sequence()->videoTracks[0].clips.at(0);
        QVERIFY2(c.motion.p("pos_x", 0) < -250, qPrintable(QString::number(c.motion.p("pos_x", 0))));
        QCOMPARE(c.motion.p("fit", 0), 1.0);
        // It shows in the media bin, and undo takes it away.
        const auto bin = std::find_if(state()->project().media.begin(), state()->project().media.end(),
                                      [made](const MediaItem& m) { return m.kind == MediaKind::Sequence && m.sequenceId == made; });
        QVERIFY(bin != state()->project().media.end());
        QCOMPARE(bin->width, 360);
        // Within a sequence: Clip › Auto Reframe on the selection.
        state()->setSelection({c.id}, false);
        state()->edit("Centre", [id = c.id](Project&, Sequence& s) {
            edit::clipById(s, id)->motion.params["pos_x"] = Param(0.0);
            return true;
        });
        QCOMPARE(win_->autoReframeClips(), 1);
        QVERIFY(state()->sequence()->videoTracks[0].clips.at(0).motion.p("pos_x", 0) < -250);
        // Duplicate Sequence.
        state()->setActiveSequence(original);
        win_->findChild<QAction*>("duplicateSequence")->trigger();
        QCOMPARE(state()->sequence()->name, std::string("Sequence 1 Copy"));
        QVERIFY(state()->sequence()->id != original);
        state()->newProject();
    }

    void editingStaplesFromTheMenus() {
        // Two seconds of colour bars as a video file.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 160;
        gs.height = 90;
        gs.fps = {30, 1};
        Clip bars = makeGeneratorClip(gen, "bars", 60);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, bars);
        ExportSettings st;
        st.path = (dir_.path() + "/bars.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        const auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        // Undo replaces the project, so the sequence is looked up each time.
const auto seq = [this] { return state()->sequence(); };
        auto action = [&](const char* name) {
            auto* a = win_->findChild<QAction*>(name);
            if (!a) qWarning("no action %s", name);
            return a;
        };
        // Q at frame 10 takes frames 0-9 and puts the playhead on the join; W at 20 takes the rest.
        state()->setPlayhead(10);
        action("rippleTrimPrevious")->trigger();
        QCOMPARE(seq()->duration(), FrameTime(50));
        QCOMPARE(state()->playhead(), FrameTime(0));
        QCOMPARE(seq()->videoTracks[0].clips[0].sourceIn, 10.0);
        state()->setPlayhead(20);
        action("rippleTrimNext")->trigger();
        QCOMPARE(seq()->duration(), FrameTime(20));
        state()->undo();
        state()->undo();
        QCOMPARE(seq()->duration(), FrameTime(60));
        // Frame Hold at 15 (Shift+F).
        state()->setSelection({}, false);
        state()->setPlayhead(15);
        action("addFrameHold")->trigger();
        QCOMPARE(seq()->videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(seq()->videoTracks[0].clips[1].sourceFrameAt(40), 15.0);
        state()->undo();
        // Paste Attributes: split at 30, scale the first half, copy it and paste its motion onto the second.
        state()->edit("Split and scale", [](Project& p, Sequence& sq) {
            if (!edit::razorAll(p, sq, 30).ok) return false;
            sq.videoTracks[0].clips[0].motion.params["scale"] = Param(150.0);
            return true;
        });
        QCOMPARE(seq()->videoTracks[0].clips.size(), size_t(2));
        const Id first = seq()->videoTracks[0].clips[0].id, second = seq()->videoTracks[0].clips[1].id;
        QAction* copy = nullptr;
        for (QAction* a : win_->findChildren<QAction*>())
            if (a->shortcut() == QKeySequence(QKeySequence::Copy)) copy = a;
        QVERIFY(copy && action("pasteAttributes") && action("removeAttributes"));
        state()->setSelection({first}, false);
        copy->trigger();
        state()->setSelection({second}, false);
        QVERIFY(win_->pasteAttributes(edit::AttrMotion));
        QCOMPARE(edit::clipById(*seq(), second)->motion.p("scale", 0), 150.0);
        QVERIFY(win_->removeAttributes(edit::AttrMotion));
        QCOMPARE(edit::clipById(*seq(), second)->motion.p("scale", 0, 100), 100.0);
        // Select Clips After Playhead (A).
        state()->setPlayhead(30);
        action("selectForward")->trigger();
        QCOMPARE(state()->selectedClips(), std::vector<Id>{second});
        // Replace with Source Clip: the source In (frame 5) goes to the clip's start.
        state()->setSourceMedia(ids[0]);
        state()->setSourceIn(5);
        state()->setSourceOut(-1);
        action("replaceWithSource")->trigger();
        QCOMPARE(edit::clipById(*seq(), second)->sourceIn, 5.0);
        // Fit to Fill: source 0-59 into timeline 0-29 at double speed.
        state()->setSourceIn(0);
        state()->setSourceOut(59);
        state()->edit("Marks", [](Project&, Sequence& sq) {
            sq.inPoint = 0;
            sq.outPoint = 29;
            return true;
        });
        action("fitToFill")->trigger();
        const Clip* fitted = edit::clipAt(*seq(), {TrackKind::Video, 0}, 10);
        QVERIFY(fitted && fitted->start == 0 && fitted->duration == 30);
        QCOMPARE(fitted->speed, 2.0);
        state()->newProject();
    }

    void keyboardShortcuts() {
        QWidget* w = win_.get();
        keymap::resetAll(w);
        const auto all = keymap::actions(w);
        QVERIFY(all.size() > 100);
        // Every command has its own id, and no key does two things.
        QSet<QString> ids;
        QMap<QString, QString> used;
        auto checkUnique = [&](const QString& when) {
            used.clear();
            for (QAction* a : keymap::actions(w))
                for (const QKeySequence& k : a->shortcuts()) {
                    const QString key = k.toString(QKeySequence::PortableText);
                    QVERIFY2(!used.contains(key), qPrintable(QString("%1: %2 is on %3 and %4").arg(when, key, used.value(key), keymap::idOf(a))));
                    used[key] = keymap::idOf(a);
                }
        };
        for (QAction* a : all) {
            QVERIFY2(!ids.contains(keymap::idOf(a)), qPrintable(keymap::idOf(a)));
            ids.insert(keymap::idOf(a));
        }
        checkUnique("Montage");
        // Every preset names real commands and leaves no key on two of them.
        for (const QString& preset : keymap::presets()) {
            const QJsonObject keys = keymap::presetKeys(preset);
            for (auto it = keys.begin(); it != keys.end(); ++it)
                QVERIFY2(keymap::find(w, it.key()), qPrintable(preset + ": " + it.key()));
            QVERIFY(keymap::applyPreset(w, preset));
            checkUnique(preset);
            for (auto it = keys.begin(); it != keys.end(); ++it)
                QCOMPARE(keymap::find(w, it.key())->shortcut().toString(QKeySequence::PortableText), it.value().toString());
        }
        keymap::resetAll(w);
        // Resetting brings back every key, the second ones too.
        QAction* redo = keymap::find(w, "Edit/Redo");
        QVERIFY(redo && redo->shortcuts().size() == 2);
        // A key in use moves only when asked.
        QAction* trim = keymap::find(w, "Sequence/Ripple Trim Previous Edit to Playhead");
        QAction* marker = keymap::find(w, "Sequence/Add Marker");
        QVERIFY(trim && marker);
        QCOMPARE(trim->shortcut(), QKeySequence(Qt::Key_Q));
        QCOMPARE(keymap::conflict(w, QKeySequence(Qt::Key_Q), marker), trim);
        QVERIFY(!keymap::assign(w, "Sequence/Add Marker", QKeySequence(Qt::Key_Q), false));
        QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_M));
        QVERIFY(keymap::assign(w, "Sequence/Add Marker", QKeySequence(Qt::Key_Q), true));
        QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_Q));
        QVERIFY(trim->shortcut().isEmpty());
        // Kept between sessions: only the changes are stored, and load() puts them back.
        QSettings settings;
        QCOMPARE(settings.value("keymap/Sequence/Add Marker").toString(), QString("Q"));
        QCOMPARE(settings.value("keymap/Sequence/Ripple Trim Previous Edit to Playhead").toString(), QString("none"));
        QVERIFY(!settings.contains("keymap/Sequence/Lift"));
        marker->setShortcut(QKeySequence(Qt::Key_M));
        keymap::load(w);
        QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_Q));
        // Saved to a file and read back.
        const QJsonObject layout = keymap::save(w);
        keymap::resetAll(w);
        QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_M));
        QVERIFY(keymap::restore(w, layout));
        QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_Q));
        QVERIFY(!keymap::restore(w, QJsonObject{{"something", 1}}));
        // The dialog: search, pick, set (taking the key over).
        keymap::resetAll(w);
        {
            keymap::Dialog dlg(w);
            dlg.setFilter("marker");
            QVERIFY(dlg.select("Sequence/Add Marker"));
            QVERIFY(!dlg.select("Sequence/Lift"));  // filtered out
            QVERIFY(dlg.setSelectedKey(QKeySequence(Qt::Key_F7)));
            QCOMPARE(marker->shortcut(), QKeySequence(Qt::Key_F7));
            QVERIFY(dlg.setSelectedKey(QKeySequence(Qt::Key_Q)));
            QVERIFY(trim->shortcut().isEmpty());
        }
        // The shortcut works: Q now adds a marker (with the Program monitor the one in use).
        state()->newProject();
        state()->setPlayhead(12);
        win_->activateWindow();
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) m->viewer()->setFocus();
        QApplication::processEvents();
        const size_t markers = state()->sequence()->markers.size();
        QTest::keyClick(win_.get(), Qt::Key_Q);
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);
        keymap::resetAll(w);
    }

    void loudnessReadout() {
        auto* readout = win_->findChild<LoudnessReadout*>("loudness");
        QVERIFY(readout);
        readout->setTargetIndex(0);  // EBU R128, -23
        QCOMPARE(readout->target(), -23.0);
        readout->setReading(-22.8, -23.4, -23.2, 4.1, -2.0);
        QCOMPARE(readout->text("M"), QString("-22.8"));
        QCOMPARE(readout->text("I"), QString("-23.2"));
        QCOMPARE(readout->text("LRA"), QString("4.1"));
        QCOMPARE(readout->text("TP"), QString("-2.0"));
        QCOMPARE(readout->integratedStatus(), 0);  // on target
        readout->setTargetIndex(2);  // streaming, -14: well off
        QCOMPARE(readout->integratedStatus(), 2);
        readout->setTargetIndex(1);  // -24: within 1 LU too
        QCOMPARE(readout->integratedStatus(), 0);
        readout->setTargetIndex(0);
        readout->setReading(-21.6, -21.6, -21.6, 4.1, -2.0);  // 1.4 LU over -23: near
        QCOMPARE(readout->integratedStatus(), 1);
        readout->setReading(-200, -200, -200, 0, -200);  // nothing measured yet
        QCOMPARE(readout->text("I"), QString("—"));
        QCOMPARE(readout->text("LRA"), QString("0.0"));
        // Reset clears it and asks for a new measurement.
        readout->setReading(-20, -20, -20, 1, -0.5);
        QSignalSpy reset(readout, &LoudnessReadout::resetRequested);
        readout->findChild<QToolButton*>("loudnessReset")->click();
        QCOMPARE(reset.count(), 1);
        QCOMPARE(readout->text("M"), QString("—"));
        readout->setTargetIndex(0);
    }

    void renderInToOutAndTheRenderBar() {
        loadDemo();
        RenderCache::instance().clear();
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program && program->controller());
        state()->edit("Marks", [](Project&, Sequence& s) {
            s.inPoint = 0;
            s.outPoint = 9;
            return true;
        });
        auto* render = win_->findChild<QAction*>("renderInToOut");
        QVERIFY(render && render->shortcut() == QKeySequence(Qt::Key_Return));
        render->trigger();
        QCOMPARE(RenderCache::instance().count(), 10);
        QCOMPARE(timeline()->renderedRanges(), (std::vector<std::pair<FrameTime, FrameTime>>{{0, 10}}));
        QCOMPARE(win_->renderInToOut(), 0);  // nothing left to render there
        // The Program monitor shows the rendered frame: marked so it can be told apart, it is what appears.
        const RenderOptions o = program->controller()->renderOptions();
        const QByteArray key = frameKey(state()->project(), *state()->sequence(), 4, o);
        QImage marked = RenderCache::instance().load(key);
        QVERIFY(!marked.isNull());
        marked.fill(QColor(255, 0, 255));
        QVERIFY(RenderCache::instance().store(key, marked));
        state()->setPlayhead(3);
        program->controller()->seek(4);
        QTRY_VERIFY_WITH_TIMEOUT(!program->viewer()->image().isNull() && program->viewer()->image().pixelColor(5, 5).red() > 240 &&
                                     program->viewer()->image().pixelColor(5, 5).green() < 15,
                                 5000);
        // An edit to what is on screen there takes those frames off the bar (once it settles).
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->edit("Invert", [red](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects.push_back(makeEffect(p, "invert"));
            return true;
        });
        win_->refreshRenderBar(true);
        const auto after = timeline()->renderedRanges();
        FrameTime covered = 0;
        for (const auto& [a, b] : after) covered += b - a;
        QVERIFY2(covered < 10, qPrintable(QString::number(covered)));
        // Undo brings them back.
        state()->undo();
        win_->refreshRenderBar(true);
        QCOMPARE(timeline()->renderedRanges(), (std::vector<std::pair<FrameTime, FrameTime>>{{0, 10}}));
        win_->findChild<QAction*>("deleteRenderFiles")->trigger();
        QCOMPARE(RenderCache::instance().count(), 0);
        QVERIFY(timeline()->renderedRanges().empty());
    }

    void voiceoverRecording() {
        // The WAV writer: a second of tone, read back as a one-second sound.
        const QString wav = dir_.path() + "/tone.wav";
        {
            WavWriter w;
            QVERIFY(w.open(wav, 48000, 1));
            std::vector<float> tone(48000);
            for (size_t i = 0; i < tone.size(); ++i) tone[i] = float(0.5 * std::sin(2 * M_PI * 440 * double(i) / 48000));
            w.write(tone.data(), 24000);
            w.write(tone.data() + 24000, 24000);
            QCOMPARE(w.frames(), qint64(48000));
            QVERIFY(w.close());
        }
        MediaItem probed;
        std::string err;
        QVERIFY2(probeMedia(wav.toStdString(), probed, &err), err.c_str());
        QVERIFY(probed.hasAudio && std::fabs(probed.duration - 1.0) < 0.01);

        // A take fed with two seconds of sound lands on A2 where it began, saved beside the project.
        state()->newProject();
        QVERIFY(state()->save(dir_.path() + "/vo.montage"));
        VoiceoverRecorder rec(state());
        QCOMPARE(rec.takeFolder(), dir_.path() + "/Voiceover");
        QVERIFY(rec.start(30, 1, -1, {}, false, 48000, 1));
        QVERIFY(rec.isRecording());
        std::vector<float> block(4800, 0.25f);
        for (int i = 0; i < 20; ++i) rec.feed(block.data(), 4800);
        QVERIFY(std::fabs(rec.seconds() - 2.0) < 1e-9);
        const Id clip = rec.stop();
        QVERIFY(clip);
        QVERIFY(!rec.isRecording());
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        auto loc = edit::locate(*state()->sequence(), clip);
        QVERIFY(c && loc && loc->track == (TrackRef{TrackKind::Audio, 1}));
        QCOMPARE(c->start, FrameTime(30));
        QVERIFY(std::abs(c->duration - FrameTime(std::llround(2 * state()->sequence()->fpsValue()))) <= 1);
        QCOMPARE(rec.lastTake(), dir_.path() + "/Voiceover/Sequence 1 VO 1.wav");
        QVERIFY(QFileInfo::exists(rec.lastTake()));
        QCOMPARE(state()->project().findMedia(c->mediaId)->bin, std::string("Voiceover"));
        // Undo takes the clip off the track.
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), clip));
        // Punch-in: from In (100) to Out (129) it stops by itself at Out, whatever keeps coming.
        QVERIFY(rec.start(100, 0, 130, {}, false, 48000, 1));
        for (int i = 0; i < 40; ++i) rec.feed(block.data(), 4800);
        QTRY_VERIFY(!rec.isRecording());
        QCOMPARE(rec.lastTake(), dir_.path() + "/Voiceover/Sequence 1 VO 2.wav");
        const Clip* punched = edit::clipAt(*state()->sequence(), {TrackKind::Audio, 0}, 110);
        QVERIFY(punched && punched->start == 100);
        QVERIFY(std::abs(punched->duration - 30) <= 1);
        // Nothing recorded: no clip, no file.
        QVERIFY(rec.start(200, 0, -1, {}, false));
        QCOMPARE(rec.stop(), Id(0));
        QVERIFY(!QFileInfo::exists(dir_.path() + "/Voiceover/Sequence 1 VO 3.wav"));
        // The dialog from the Sequence menu.
        win_->findChild<QAction*>("recordVoiceover")->trigger();
        auto* dlg = win_->findChild<VoiceoverDialog*>("voiceoverDialog");
        QVERIFY(dlg && dlg->isVisible());
        QVERIFY(dlg->findChild<QPushButton*>("voiceoverRecord"));
        dlg->close();
        state()->newProject();
    }

    void renderQueueInTheBackground() {
        loadDemo();
        RenderQueue* queue = win_->renderQueue();
        QVERIFY(queue && queue->jobs().empty());
        // From the Export dialog: Add to Queue hands the export over and closes.
        {
            ExportDialog ed(state(), win_.get());
            ed.setQueue(queue);
            auto* add = ed.findChild<QPushButton*>("addToQueue");
            auto* path = ed.findChild<QLineEdit*>("exportPath");
            QVERIFY(add && path && !add->isHidden());
            path->setText(dir_.path() + "/queued-a.mp4");
            add->click();
            QCOMPARE(ed.result(), int(QDialog::Accepted));
        }
        QCOMPARE(queue->jobs().size(), size_t(1));
        QCOMPARE(queue->jobs()[0].status, RenderQueue::Status::Waiting);
        QVERIFY(!queue->running());
        // Two more: one that cannot be written, one fast preset. Each renders the project as it was when added.
        ExportSettings st = findExportPreset("H.264 - High Quality")->settings;
        st.preset = "ultrafast";
        st.path = (dir_.path() + "/no-such-folder/b.mp4").toStdString();
        const Id seq = state()->sequence()->id;
        const int bad = queue->add("Bad", "H.264", state()->project(), seq, st);
        st.path = (dir_.path() + "/queued-c.mp4").toStdString();
        const int good = queue->add("Good", "H.264", state()->project(), seq, st);
        QVERIFY(state()->apply("Clear", [](Project& p, Sequence& s) {
            std::vector<Id> all;
            for (TrackRef r : allTracks(s))
                for (const Clip& c : trackAt(s, r)->clips) all.push_back(c.id);
            return edit::removeClips(p, s, all, false);
        }));
        auto* panel = win_->findChild<RenderQueuePanel*>();
        QVERIFY(panel);
        auto* startButton = panel->findChild<QPushButton*>("queueStart");
        QVERIFY(startButton->isEnabled());
        startButton->click();
        QVERIFY(queue->running());
        QTRY_VERIFY_WITH_TIMEOUT(!queue->running(), 60000);
        QCOMPARE(queue->jobs()[0].status, RenderQueue::Status::Done);
        QCOMPARE(queue->job(bad)->status, RenderQueue::Status::Failed);
        QVERIFY(!queue->job(bad)->error.isEmpty());
        QCOMPARE(queue->job(good)->status, RenderQueue::Status::Done);
        MediaItem m;
        QVERIFY(probeMedia((dir_.path() + "/queued-c.mp4").toStdString(), m));
        QVERIFY2(std::fabs(m.duration - 4.0) < 0.1, qPrintable(QString::number(m.duration)));  // the 120 frames as they were
        QVERIFY(QFileInfo::exists(dir_.path() + "/queued-a.mp4"));
        // Retry puts a job back; Remove and Clear Finished tidy up.
        QVERIFY(queue->retry(bad));
        QCOMPARE(queue->job(bad)->status, RenderQueue::Status::Waiting);
        QVERIFY(queue->remove(bad));
        queue->clearFinished();
        QVERIFY(queue->jobs().empty());
        // Stopping cancels the job rendering and leaves no partial file.
        state()->undo();
        QVERIFY(state()->edit("Long", [](Project&, Sequence& s) {
            s.videoTracks[0].clips.back().duration = 3000;
            return true;
        }));
        st.path = (dir_.path() + "/long.mp4").toStdString();
        const int longJob = queue->add("Long", "H.264", state()->project(), seq, st);
        queue->start();
        QTRY_VERIFY(queue->job(longJob)->progress > 0);
        queue->stop();
        QTRY_VERIFY_WITH_TIMEOUT(queue->job(longJob)->status == RenderQueue::Status::Cancelled, 30000);
        QVERIFY(!queue->running());
        QVERIFY(!QFileInfo::exists(dir_.path() + "/long.mp4"));
        QVERIFY(queue->remove(longJob));
    }

    void keyframeGraphEditor() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Keys", [red](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            c->motion.params["opacity"].addKey(0, 100);
            c->motion.params["opacity"].addKey(30, 50);
            c->motion.params["opacity"].addKey(50, 80);
            return true;
        }));
        state()->setSelection({red}, false);
        win_->raisePanel("keyframes");
        auto* panel = win_->findChild<KeyframePanel*>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible() && panel->width() > 300);
        panel->resize(panel->width(), std::max(panel->height(), 220));
        auto* toggle = panel->findChild<QToolButton*>("keyframeGraph");
        QVERIFY(toggle && toggle->isCheckable());
        toggle->click();
        QVERIFY(panel->graph());
        QCOMPARE(int(panel->rows().size()), 1);
        auto opacity = [&]() -> const Param& { return edit::clipById(*state()->sequence(), red)->motion.params.at("opacity"); };
        auto drag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QTest::mousePress(panel, Qt::LeftButton, mods, from);
            for (int i = 1; i <= 4; ++i) {
                const QPoint at = from + (to - from) * i / 4;
                QMouseEvent move(QEvent::MouseMove, at, panel->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, mods);
                QApplication::sendEvent(panel, &move);
            }
            QTest::mouseRelease(panel, Qt::LeftButton, mods, to);
        };
        // The keys sit on the curve at their values: 100 above 80 above 50.
        const QPointF k0 = panel->graphPoint(0, 0), k30 = panel->graphPoint(0, 30), k50 = panel->graphPoint(0, 50);
        QVERIFY(k0.y() < k50.y() && k50.y() < k30.y());
        // Drag the middle key up to the last key's height and later: one undo step moves it in time and value.
        const QPoint target(int(panel->graphPoint(0, 36).x()), int(k50.y()));
        drag(k30.toPoint(), target);
        const Param& moved = opacity();
        QCOMPARE(moved.keys.size(), size_t(3));
        QVERIFY2(std::llabs(moved.keys[1].t - 36) <= 1, qPrintable(QString::number(moved.keys[1].t)));
        QVERIFY2(std::fabs(moved.keys[1].v - 80) < 3, qPrintable(QString::number(moved.keys[1].v)));
        state()->undo();
        QCOMPARE(opacity().keys[1].t, FrameTime(30));
        QCOMPARE(opacity().keys[1].v, 50.0);

        // Easy Ease from the menu on the middle key; its handles then show and can be dragged.
        panel->select({{panel->rows()[0].address, 30}});
        QVERIFY(panel->easeSelected(true, true));
        QCOMPARE(opacity().keys[0].interp, Interp::Bezier);
        QCOMPARE(opacity().keys[1].interp, Interp::Bezier);
        QCOMPARE(opacity().keys[1].inDv, 0.0);
        QCOMPARE(opacity().keys[1].outDv, 0.0);
        panel->select({{panel->rows()[0].address, 30}});
        const QPointF h = panel->handlePoint(0, 30, true);
        QVERIFY(h.x() > panel->graphPoint(0, 30).x());
        QVERIFY(std::fabs(h.y() - panel->graphPoint(0, 30).y()) < 1.5);  // flat
        // Pull the out handle upwards: the curve rises sooner after the key, and the in handle follows (linked).
        const double before = opacity().at(34);
        drag(h.toPoint(), h.toPoint() + QPoint(0, -40));
        QVERIFY(opacity().keys[1].outDv > 0);
        QVERIFY(opacity().keys[1].inDv < 0);  // the same slope on the other side
        QVERIFY(opacity().at(34) > before);
        // Alt breaks the handles: dragging the in handle leaves the out one where it was.
        const double out = opacity().keys[1].outDv;
        panel->select({{panel->rows()[0].address, 30}});
        const QPointF hin = panel->handlePoint(0, 30, false);
        drag(hin.toPoint(), hin.toPoint() + QPoint(0, 30), Qt::AltModifier);
        QCOMPARE(opacity().keys[1].outDv, out);
        // All of it undoes step by step.
        state()->undo();
        state()->undo();
        state()->undo();
        QCOMPARE(opacity().keys[1].interp, Interp::Linear);
        // Clicking the row's label shows its curve alone.
        QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, QPoint(40, panel->keyPoint(0, 0).y()));
        QCOMPARE(panel->graphRow(), 0);
        toggle->click();
        QVERIFY(!panel->graph());
        state()->newProject();
    }

    void effectPresetsSaveAndApply() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        QVERIFY(state()->edit("Fx", [red](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            Effect e = makeEffect("gaussian_blur", p.newId());
            e.params["radius"] = Param(9.0);
            c->effects.push_back(e);
            return true;
        }));
        // Nothing selected: refused. Red selected: saved and listed in the Effects panel.
        state()->setSelection({});
        QVERIFY(win_->saveEffectsAsPreset("Soft Focus Test").isEmpty());
        QVERIFY(win_->findChild<QAction*>("saveEffectPreset"));
        state()->setSelection({red});
        const QString file = win_->saveEffectsAsPreset("Soft Focus Test");
        QVERIFY(QFileInfo::exists(file));
        auto* browser = win_->findChild<EffectsBrowser*>();
        auto* tree = browser->findChild<QTreeWidget*>();
        bool listed = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == "Presets")
                for (int k = 0; k < tree->topLevelItem(i)->childCount(); ++k) listed |= tree->topLevelItem(i)->child(k)->text(0) == "Soft Focus Test";
        QVERIFY(listed);
        // On Blue: the blur added with its setting, as one undo step.
        state()->setSelection({blue});
        QCOMPARE(win_->applyEffectPreset(file), 1);
        const Clip* b = edit::clipById(*state()->sequence(), blue);
        QCOMPARE(b->effects.size(), size_t(1));
        QCOMPARE(b->effects[0].p("radius", 0), 9.0);
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), blue)->effects.empty());
        QVERIFY(presets::remove(file));
        browser->reload();
        state()->newProject();
    }

    void keyframeRepeatFromThePanel() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Keys", [red](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            c->motion.params["rotation"].addKey(0, 0);
            c->motion.params["rotation"].addKey(10, 90);
            return true;
        }));
        state()->setSelection({red}, false);
        win_->raisePanel("keyframes");
        auto* panel = win_->findChild<KeyframePanel*>();
        QTRY_COMPARE(panel->clip(), red);
        QVERIFY(!panel->setRepeat(Repeat::Loop));  // nothing selected
        panel->select({{panel->rows().front().address, 10}});
        QVERIFY(panel->setRepeat(Repeat::Offset));
        const Param& rot = edit::clipById(*state()->sequence(), red)->motion.params.at("rotation");
        QCOMPARE(rot.repeat, Repeat::Offset);
        QCOMPARE(rot.at(25), 225.0);  // spinning on: 180 + half of 90
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), red)->motion.params.at("rotation").repeat, Repeat::Hold);
    }

    void keyframePanelEditsKeys() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Keys", [red](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            c->motion.params["opacity"].addKey(0, 100);
            c->motion.params["opacity"].addKey(30, 50);
            c->motion.params["scale"].addKey(10, 100);
            c->motion.params["scale"].addKey(40, 120);
            return true;
        }));
        state()->setSelection({red}, false);
        win_->raisePanel("keyframes");
        auto* panel = win_->findChild<KeyframePanel*>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible() && panel->width() > 300);
        QCOMPARE(panel->clip(), red);
        QCOMPARE(int(panel->rows().size()), 2);
        int opacityRow = -1, scaleRow = -1;
        for (int r = 0; r < 2; ++r) (panel->rows()[size_t(r)].address.param == "opacity" ? opacityRow : scaleRow) = r;
        QVERIFY(opacityRow >= 0 && scaleRow >= 0);
        QVERIFY(panel->rows()[size_t(opacityRow)].label.contains("Opacity"));
        auto keysOf = [&](const char* name) {
            std::vector<FrameTime> out;
            for (const Keyframe& k : edit::clipById(*state()->sequence(), red)->motion.params.at(name).keys) out.push_back(k.t);
            return out;
        };
        using Times = std::vector<FrameTime>;

        // Click a key to select it, then drag it later.
        const QPoint k30 = panel->keyPoint(opacityRow, 30);
        QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, k30);
        QCOMPARE(panel->selection().size(), size_t(1));
        const QPoint later = panel->keyPoint(opacityRow, 45);
        QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, k30);
        QMouseEvent move(QEvent::MouseMove, later, panel->mapToGlobal(later), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &move);
        QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier, later);
        QVERIFY2(std::llabs(keysOf("opacity")[1] - 45) <= 1, qPrintable(QString::number(keysOf("opacity")[1])));
        state()->undo();
        QCOMPARE(keysOf("opacity"), (Times{0, 30}));

        // A box round everything selects all four; arrow keys nudge them together; Delete removes them.
        panel->select({});
        const QPoint from(panel->keyPoint(0, 0).x() - 10, panel->keyPoint(0, 0).y() - 10);
        const QPoint to(panel->keyPoint(1, 59).x() + 5, panel->keyPoint(1, 59).y() + 10);
        QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent boxMove(QEvent::MouseMove, to, panel->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &boxMove);
        QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(panel->selection().size(), size_t(4));
        panel->setFocus();
        QTest::keyClick(panel, Qt::Key_Left);  // the key at 0 cannot go earlier
        QCOMPARE(keysOf("opacity"), (Times{0, 30}));
        QTest::keyClick(panel, Qt::Key_Right);
        QCOMPARE(keysOf("opacity"), (Times{1, 31}));
        QCOMPARE(keysOf("scale"), (Times{11, 41}));
        QVERIFY(panel->setInterpolation(Interp::Hold));
        QCOMPARE(edit::clipById(*state()->sequence(), red)->motion.params.at("scale").keys[0].interp, Interp::Hold);
        QTest::keyClick(panel, Qt::Key_Delete);
        QVERIFY(!edit::clipById(*state()->sequence(), red)->motion.params.at("opacity").animated());
        QVERIFY(panel->rows().empty());
        state()->undo();
        QCOMPARE(int(panel->rows().size()), 2);

        // Double-click adds a key with the value there; a click on the ruler moves the playhead.
        QTest::mouseDClick(panel, Qt::LeftButton, Qt::NoModifier, panel->keyPoint(opacityRow, 20));
        const auto& op = edit::clipById(*state()->sequence(), red)->motion.params.at("opacity");
        QCOMPARE(op.keys.size(), size_t(3));
        QVERIFY(std::fabs(op.keys[1].v - op.at(op.keys[1].t)) < 1e-9);
        QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, QPoint(panel->keyPoint(0, 25).x(), 5));
        QVERIFY(std::llabs(state()->playhead() - 25) <= 1);
    }

    void colourManagementUi() {
        // A short grey video to interpret.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 160;
        gs.height = 90;
        Clip grey = makeGeneratorClip(gen, "color", 10);
        for (const char* k : {"color.r", "color.g", "color.b"}) grey.generator.params[k] = 0.4;
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, grey);
        ExportSettings st;
        st.path = (dir_.path() + "/grey.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));

        // Interpret Colour from the media bin's context menu.
        auto* binWidget = win_->findChild<MediaBinWidget*>();
        QVERIFY(binWidget);
        QCOMPARE(binWidget->shownMedia(), ids);
        binWidget->selectMedia(ids);
        QAbstractItemView* bin = binWidget->currentView();
        bool triggered = false;
        QTimer::singleShot(0, this, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            if (auto* sub = menu->findChild<QMenu*>("interpretColour"))
                for (QAction* a : sub->actions())
                    if (a->data().toString() == "slog3-sgamut3cine") {
                        a->trigger();
                        triggered = true;
                    }
            menu->close();
        });
        emit bin->customContextMenuRequested(QPoint(10, 10));
        QVERIFY(triggered);
        QCOMPARE(state()->project().findMedia(ids[0])->colorOverride, std::string("slog3-sgamut3cine"));
        QVERIFY(bin->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains("S-Log3"));
        state()->undo();
        QVERIFY(state()->project().findMedia(ids[0])->colorOverride.empty());

        // Sequence settings: colour space, with the HDR peak for PQ only.
        SequenceSettingsDialog dlg(win_.get());
        NewSequenceSpec spec;
        spec.colorSpace = "rec2100pq";
        spec.hdrPeakNits = 4000;
        dlg.setSpec(spec);
        auto* space = dlg.findChild<QComboBox*>("colorSpace");
        auto* peak = dlg.findChild<QSpinBox*>("hdrPeak");
        QVERIFY(space && peak);
        QCOMPARE(space->currentData().toString(), QString("rec2100pq"));
        QVERIFY(peak->isEnabled());
        QCOMPARE(peak->value(), 4000);
        QCOMPARE(dlg.spec().hdrPeakNits, 4000.0);
        space->setCurrentIndex(space->findData(QString("rec709")));
        QVERIFY(!peak->isEnabled());
        QCOMPARE(dlg.spec().colorSpace, std::string("rec709"));

        // Export: deliver the HLG sequence as it is or in any display space.
        state()->edit("HLG", [](Project&, Sequence& s) {
            s.colorSpace = "rec2100hlg";
            return true;
        });
        ExportDialog ed(state(), win_.get());
        auto* color = ed.findChild<QComboBox*>("exportColor");
        QVERIFY(color);
        QCOMPARE(color->count(), int(displayColorSpaces().size()) + 1);
        QVERIFY(color->itemText(0).contains("HLG"));
        // Loudness targets for delivery, as (LUFS, dBTP).
        auto* loudness = ed.findChild<QComboBox*>("exportLoudness");
        QVERIFY(loudness && loudness->count() == 5);
        QCOMPARE(loudness->itemData(0).toPointF(), QPointF(0, 0));
        QCOMPARE(loudness->itemData(1).toPointF(), QPointF(-14, -1));
        QCOMPARE(loudness->itemData(3).toPointF(), QPointF(-23, -1));
        state()->newProject();
        win_->activateWindow();  // the context menu took the focus
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void multicamPanelSwitching() {
        // Two cameras: a red one and a blue one, three seconds each.
        auto camera = [&](const char* file, double r, double g, double b) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip c = makeGeneratorClip(gen, "color", 90);
            c.generator.params["color.r"] = r;
            c.generator.params["color.g"] = g;
            c.generator.params["color.b"] = b;
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
            ExportSettings st;
            st.path = (dir_.path() + "/" + file).toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        };
        camera("red.mp4", 0.9, 0.1, 0.1);
        camera("blue.mp4", 0.1, 0.1, 0.9);
        state()->newProject();
        auto ids = state()->importFiles({dir_.path() + "/red.mp4", dir_.path() + "/blue.mp4"});
        QCOMPARE(ids.size(), size_t(2));
        const Id mc = MulticamPanel::createMulticam(state(), ids, MulticamPanel::Sync::InPoints, "Show", win_.get());
        QVERIFY(mc);
        state()->apply("Place", [mc](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, mc, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        state()->setPlayhead(30);
        auto* panel = win_->findChild<MulticamPanel*>();
        QVERIFY(panel);
        win_->findChild<QDockWidget*>("multicam")->show();
        win_->findChild<QDockWidget*>("multicam")->raise();
        QTRY_COMPARE(panel->angleCount(), 2);
        const Id clip = panel->currentClip();
        QVERIFY(clip);
        // Both angles render side by side.
        QTRY_VERIFY_WITH_TIMEOUT(panel->angleImages().size() == 2 && !panel->angleImages()[1].isNull(), 5000);
        const QImage red = panel->angleImages()[0], blue = panel->angleImages()[1];
        const QColor rc = red.pixelColor(red.width() / 2, red.height() / 2), bc = blue.pixelColor(blue.width() / 2, blue.height() / 2);
        QVERIFY2(rc.red() > 180 && rc.blue() < 80, qPrintable(rc.name()));
        QVERIFY2(bc.blue() > 180 && bc.red() < 80, qPrintable(bc.name()));

        // Stopped: clicking an angle switches the shot without cutting.
        QVERIFY(panel->switchTo(1));
        const Sequence* s = state()->sequence();
        QCOMPARE(s->videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(s->videoTracks[0].clips[0].angle, 1);
        // Shift cuts at the playhead; the key cuts to angle 1 from there.
        QTest::keyClick(win_.get(), Qt::Key_1, Qt::ShiftModifier);
        s = state()->sequence();
        QCOMPARE(s->videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(s->videoTracks[0].clips[0].angle, 1);
        QCOMPARE(s->videoTracks[0].clips[1].start, FrameTime(30));
        QCOMPARE(s->videoTracks[0].clips[1].angle, 0);
        // The cut shows in the program: red from the cut on.
        RenderOptions o;
        auto centre = [&](FrameTime t) {
            Image img = renderProgramFrame(state()->project(), *state()->sequence(), t, o);
            return QColor::fromRgbF(img.at(img.width / 2, img.height / 2)[0], 0, img.at(img.width / 2, img.height / 2)[2]);
        };
        QVERIFY(centre(10).blueF() > 0.7f);
        QVERIFY(centre(40).redF() > 0.7f);
        // The key without Shift switches the shot under the playhead.
        QTest::keyClick(win_.get(), Qt::Key_2);
        QCOMPARE(state()->sequence()->videoTracks[0].clips[1].angle, 1);
        state()->undo();
        QCOMPARE(state()->sequence()->videoTracks[0].clips[1].angle, 0);
        state()->undo();
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(1));
        state()->newProject();
    }

    void stabilizeAndTrackFromInspector() {
        // A textured still shaken by a few pixels every frame.
        QImage tex(320, 180, QImage::Format_RGB32);
        tex.fill(QColor(80, 80, 80));
        {
            QPainter pa(&tex);
            for (int i = 0; i < 120; ++i)
                pa.fillRect((i * 97) % 300, (i * 53) % 170, 6 + (i * 7) % 20, 6 + (i * 11) % 20, QColor((i * 37) % 255, (i * 71) % 255, (i * 13) % 255));
        }
        const QString png = dir_.path() + "/texture.png";
        QVERIFY(tex.save(png));
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        MediaItem m;
        m.id = gen.newId();
        std::string err;
        QVERIFY(probeMedia(png.toStdString(), m, &err));
        gen.media.push_back(m);
        Clip c = makeClip(gen, m, TrackKind::Video, gs);
        c.duration = 30;
        c.motion.params["scale"] = Param(130.0);
        for (int i = 0; i < 30; ++i) {
            c.motion.params["pos_x"].addKey(i, 5 * std::sin(i * 1.7), Interp::Hold);
            c.motion.params["pos_y"].addKey(i, 4 * std::cos(i * 1.1), Interp::Hold);
        }
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
        ExportSettings st;
        st.path = (dir_.path() + "/shaky.mp4").toStdString();
        st.audioCodec = "none";
        st.crf = 12;
        st.preset = "ultrafast";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());

        state()->newProject();
        auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->setSelection({clip}, false);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();

        // Adding Stabilize analyses the clip straight away.
        QPushButton* add = nullptr;
        for (auto* b : win_->findChildren<QPushButton*>())
            if (b->text().startsWith("Add Video Effect") && b->isVisibleTo(win_.get())) add = b;
        QVERIFY(add);
        QAction* stab = nullptr;
        std::function<void(QMenu*)> findIn = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                if (a->menu()) findIn(a->menu());
                else if (a->text() == "Stabilize") stab = a;
            }
        };
        findIn(add->menu());
        QVERIFY(stab);
        stab->trigger();
        auto effectOf = [&](const char* type) -> const Effect* {
            const Clip* k = edit::clipById(*state()->sequence(), clip);
            for (const Effect& e : k->effects)
                if (e.type == type) return &e;
            return nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(effectOf("stabilize") && !effectOf("stabilize")->s("motion").empty(), 30000);
        QApplication::processEvents();
        QPushButton* again = nullptr;
        for (auto* b : win_->findChildren<QPushButton*>("analyzeStabilize"))
            if (b->isVisibleTo(win_.get())) again = b;
        QVERIFY(again);

        // A masked blur, tracked forwards from the playhead.
        state()->edit("Blur", [clip](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "gaussian_blur");
            e.params["mask.shape"] = Param(1.0);
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        state()->setPlayhead(5);
        QApplication::processEvents();
        QToolButton* fwd = nullptr;
        for (auto* b : win_->findChildren<QToolButton*>("trackMaskForward"))
            if (b->isVisibleTo(win_.get())) fwd = b;
        QVERIFY(fwd);
        fwd->click();
        auto maskX = [&]() -> const Param* {
            const Effect* b = effectOf("gaussian_blur");
            auto it = b ? b->params.find("mask.x") : decltype(b->params.end()){};
            return b && it != b->params.end() ? &it->second : nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(maskX() && maskX()->animated(), 30000);
        const Effect* blur = effectOf("gaussian_blur");
        QCOMPARE(blur->params.at("mask.x").keys.front().t, FrameTime(5));
        // To the clip's last frame (25 fps footage in a 30 fps sequence).
        QCOMPARE(blur->params.at("mask.x").keys.back().t, edit::clipById(*state()->sequence(), clip)->duration - 1);
        state()->undo();
        QVERIFY(!maskX() || !maskX()->animated());

        // A Corner Pin's corners follow the surface they sit on (here the clip's own footage).
        state()->edit("Pin", [clip](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "corner_pin");
            e.params["tl_x"] = Param(0.3);
            e.params["tl_y"] = Param(0.3);
            e.params["tr_x"] = Param(0.7);
            e.params["tr_y"] = Param(0.3);
            e.params["br_x"] = Param(0.7);
            e.params["br_y"] = Param(0.7);
            e.params["bl_x"] = Param(0.3);
            e.params["bl_y"] = Param(0.7);
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        QApplication::processEvents();
        QToolButton* pinFwd = nullptr;
        for (auto* b : win_->findChildren<QToolButton*>("trackCornersForward"))
            if (b->isVisibleTo(win_.get())) pinFwd = b;
        QVERIFY(pinFwd);
        pinFwd->click();
        auto corner = [&](const char* name) -> const Param* {
            const Effect* e = effectOf("corner_pin");
            auto it = e ? e->params.find(name) : decltype(e->params.end()){};
            return e && it != e->params.end() ? &it->second : nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(corner("br_y") && corner("br_y")->animated(), 30000);
        QCOMPARE(corner("tl_x")->keys.front().t, FrameTime(5));
        QCOMPARE(corner("tl_x")->keys.back().t, edit::clipById(*state()->sequence(), clip)->duration - 1);
        // The camera only shakes: the corners stay within a few pixels of where they were put.
        for (const Keyframe& k : corner("tl_x")->keys) QVERIFY(std::fabs(k.v - 0.3) < 0.06);
        state()->undo();
        QVERIFY(!corner("tl_x")->animated());

        // A title above the footage follows it (Transform › Follow).
        Id titleId = 0;
        state()->edit("Title", [&](Project& p, Sequence& s) {
            Clip t = makeGeneratorClip(p, "title", edit::clipById(s, clip)->duration);
            t.motion.params["pos_x"] = -20.0;
            titleId = t.id;
            return edit::overwrite(p, s, {TrackKind::Video, 1}, t).ok;
        });
        state()->setSelection({titleId}, false);
        QApplication::processEvents();
        QToolButton* follow = nullptr;
        for (auto* b : win_->findChildren<QToolButton*>("followForward"))
            if (b->isVisibleTo(win_.get())) follow = b;
        QVERIFY(follow);
        follow->click();
        auto posX = [&]() -> const Param* {
            const Clip* t = edit::clipById(*state()->sequence(), titleId);
            auto it = t ? t->motion.params.find("pos_x") : decltype(t->motion.params.end()){};
            return t && it != t->motion.params.end() ? &it->second : nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(posX() && posX()->animated(), 30000);
        QCOMPARE(posX()->keys.front().t, FrameTime(5));
        // The 320 px footage shakes up to 5 px either way, scaled up to the sequence; the title moves as much.
        const double px = state()->sequence()->width / 320.0;
        double most = 0;
        for (const Keyframe& k : posX()->keys) most = std::max(most, std::fabs(k.v + 20));
        QVERIFY2(most > 1 * px && most < 11 * px, qPrintable(QString::number(most / px)));
        state()->undo();
        QVERIFY(!posX()->animated());
        state()->newProject();
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void ocioEffectInInspector() {
        if (!ocioAvailable()) QSKIP("Built without OpenColorIO");
        const QString cfg = dir_.path() + "/inspector.ocio";
        QFile f(cfg);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("ocio_profile_version: 2\nroles:\n  default: linear\n  scene_linear: linear\n"
                "file_rules:\n  - !<Rule> {name: Default, colorspace: default}\n"
                "displays:\n  Monitor:\n    - !<View> {name: Raw, colorspace: linear}\n"
                "colorspaces:\n  - !<ColorSpace>\n    name: linear\n  - !<ColorSpace>\n    name: half\n"
                "    from_scene_reference: !<MatrixTransform> {matrix: [0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1]}\n");
        f.close();
        loadDemo();
        const Id clip = clipNamed(*state()->sequence(), "Red")->id;
        Id fx = 0;
        state()->edit("OCIO", [&](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "ocio");
            e.strings["config"] = cfg.toStdString();
            fx = e.id;
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        state()->setSelection({clip}, false);
        QApplication::processEvents();
        // The input and output lists come from the config.
        QComboBox* src = nullptr;
        for (auto* c : win_->findChildren<QComboBox*>("dynamic_dst"))
            if (c->isVisibleTo(win_.get())) src = c;
        QVERIFY(src);
        QCOMPARE(src->count(), 2);
        QCOMPARE(src->itemText(1), QString("half"));
        emit src->textActivated("half");
        const Effect* e = edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), clip, fx);
        QVERIFY(e);
        QCOMPARE(e->s("dst"), std::string("half"));
        state()->newProject();
    }

    void captionsPanelAndTimelineLane() {
        // A clip whose media has a (made-up) transcript.
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        const char* words[] = {"Ask", "not", "what", "your", "country", "can", "do", "for", "you."};
        for (int i = 0; i < 9; ++i) seg.words.push_back({0.5 + i * 0.4, 0.85 + i * 0.4, words[i], 1});
        t->segments.push_back(seg);
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false);
        }));

        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        QVERIFY(panel->generateFromTranscripts() > 0);
        const Sequence* s = state()->sequence();
        QCOMPARE(s->captionTracks.size(), size_t(1));
        const Id track = s->captionTracks[0].id;
        QCOMPARE(panel->currentTrack(), track);
        QCOMPARE(QString::fromStdString(s->captionTracks[0].captions[0].text), QString("Ask not what your country can do for you."));
        auto* table = panel->findChild<QTableWidget*>();
        QCOMPARE(table->rowCount(), int(s->captionTracks[0].captions.size()));

        // Editing the text in the table is undoable.
        table->item(0, 2)->setText("Ask not.");
        QCOMPARE(state()->sequence()->captionTracks[0].captions[0].text, std::string("Ask not."));
        state()->undo();
        QCOMPARE(QString::fromStdString(state()->sequence()->captionTracks[0].captions[0].text).left(8), QString("Ask not "));

        // The timeline shows the track as a lane above V1: drag the caption later.
        ppf_ = measurePpf();
        const Caption before = state()->sequence()->captionTracks[0].captions[0];
        const int laneY = 30 + 12;
        const int x = 176 + int((before.start + before.end) / 2 * ppf_) - timeline()->horizontalScrollBar()->value();
        drag({x, laneY}, {x + int(10 * ppf_), laneY});
        const Caption moved = state()->sequence()->captionTracks[0].captions[0];
        QVERIFY2(moved.start > before.start, qPrintable(QString("%1 -> %2").arg(before.start).arg(moved.start)));
        QCOMPARE(moved.end - moved.start, before.end - before.start);
        state()->undo();
        QCOMPARE(state()->sequence()->captionTracks[0].captions[0], before);
        // Double-clicking a caption opens it for editing in the panel.
        QSignalSpy activated(timeline(), &TimelineWidget::captionActivated);
        QTest::mouseDClick(viewport(), Qt::LeftButton, Qt::NoModifier, {x, laneY});
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.at(0).at(1).toInt(), 0);

        // The program monitor shows captions when CC is on.
        auto* cc = win_->findChild<QToolButton*>("showCaptions");
        QVERIFY(cc);
        cc->setChecked(false);
        cc->setChecked(true);
        QVERIFY(cc->isChecked());

        // Export and import round trip through WebVTT.
        const QString vtt = dir_.path() + "/captions.vtt";
        QVERIFY(panel->exportFile(vtt));
        QVERIFY(panel->importFile(vtt));
        QCOMPARE(state()->sequence()->captionTracks.size(), size_t(2));
        // What WebVTT carries comes back: times and text (not the words' timings).
        const auto& made = state()->sequence()->captionTracks[0].captions;
        const auto& read = state()->sequence()->captionTracks[1].captions;
        QCOMPARE(read.size(), made.size());
        for (size_t i = 0; i < made.size(); ++i) {
            QCOMPARE(read[i].start, made[i].start);
            QCOMPARE(read[i].end, made[i].end);
            QCOMPARE(read[i].text, made[i].text);
        }
        state()->newProject();
        QApplication::processEvents();
    }

    void transcriptPanelEditsTheCut() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.words = {{0.5, 0.9, "And", 1},  {1.0, 1.3, "so,", 1},       {1.4, 1.7, "um,", 1},   {1.8, 2.2, "my", 1},
                     {2.3, 2.8, "fellow", 1}, {2.9, 3.6, "Americans.", 1}, {6.0, 6.4, "Ask", 1}, {6.5, 6.9, "not.", 1}};
        t->segments.push_back(seg);
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false);
        }));
        const FrameTime full = state()->sequence()->duration();
        const double fps = state()->sequence()->fpsValue();

        auto* panel = win_->findChild<TranscriptPanel*>();
        QVERIFY(panel);
        panel->setMode(TranscriptPanel::Mode::Sequence);
        QCOMPARE(panel->words().size(), size_t(8));
        // Search, including a partly typed last word.
        QCOMPARE(panel->find("fellow amer"), 1);
        QCOMPARE(panel->selectedWords(), std::make_pair(4, 5));
        QCOMPARE(panel->find("nothing like this"), 0);

        // Bleeping "fellow" (one undo step): a Bleep effect on the audio clip over that stretch of source.
        {
            auto* bleep = panel->findChild<QToolButton*>("bleepButton");
            QVERIFY(bleep && bleep->isVisibleTo(panel) && bleep->menu());
            panel->selectWords(4, 4);
            panel->bleepSelection();
            const Clip& a1 = trackAt(*state()->sequence(), A1)->clips.at(0);
            QVERIFY(!a1.effects.empty() && a1.effects.back().type == "bleep");
            const auto ranges = bleepRanges(a1.effects.back());
            QVERIFY(ranges.size() == 1 && std::fabs(ranges[0].first - 2.3) < 0.05 && std::fabs(ranges[0].second - 2.8) < 0.05);
            state()->undo();
            QVERIFY(trackAt(*state()->sequence(), A1)->clips.at(0).effects.empty());
        }
        // Deleting "my fellow" cuts 1.8 s .. 2.9 s out of every track.
        panel->selectWords(3, 4);
        panel->deleteSelection();
        const FrameTime cut = FrameTime(std::llround(2.9 * fps)) - FrameTime(std::llround(1.8 * fps));
        QCOMPARE(state()->sequence()->duration(), full - cut);
        QCOMPARE(panel->words().size(), size_t(6));
        state()->undo();
        QCOMPARE(state()->sequence()->duration(), full);
        QCOMPARE(panel->words().size(), size_t(8));

        // Smooth Cuts is an option that stays set (sound-only here, so no transitions appear).
        auto* smooth = panel->findChild<QToolButton*>("smoothCuts");
        QVERIFY(smooth && smooth->isCheckable());
        smooth->setChecked(true);
        QVERIFY(QSettings().value("transcript/smoothCuts").toBool());
        panel->selectWords(3, 4);
        panel->deleteSelection();
        QCOMPARE(state()->sequence()->duration(), full - cut);
        for (const Track& t : state()->sequence()->audioTracks) QVERIFY(t.transitions.empty());
        state()->undo();
        smooth->setChecked(false);

        // Filler words and long pauses.
        panel->removeFillerWords();
        QCOMPARE(panel->words().size(), size_t(7));
        QVERIFY(state()->sequence()->duration() < full);
        const FrameTime noFillers = state()->sequence()->duration();
        panel->removePauses(1.0, 0.3);
        QVERIFY(state()->sequence()->duration() < noFillers - FrameTime(fps * 1.5));

        // Clicking a word moves the playhead there.
        state()->setPlayhead(0);
        panel->selectWords(0, 0);

        // Source mode: select words to edit them into the timeline.
        state()->newProject();
        ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        const Id src = ids[0];
        QVERIFY(state()->edit("Transcript", [src, t](Project& p, Sequence&) {
            p.findMedia(src)->transcript = t;
            return true;
        }));
        state()->setSourceMedia(src);
        panel->setMode(TranscriptPanel::Mode::Source);
        QCOMPARE(panel->words().size(), size_t(8));
        panel->selectWords(6, 7);  // "Ask not."
        panel->insertSelection(false);
        const Sequence* s = state()->sequence();
        QCOMPARE(trackAt(*s, A1)->clips.size(), size_t(1));
        const Clip& c = trackAt(*s, A1)->clips[0];
        QCOMPARE(FrameTime(c.sourceIn), FrameTime(std::floor(6.0 * fps)));
        QCOMPARE(c.duration, FrameTime(std::ceil(6.9 * fps)) - FrameTime(std::floor(6.0 * fps)));
        panel->setMode(TranscriptPanel::Mode::Sequence);
        state()->newProject();
        QApplication::processEvents();
    }

    void paperEditInTranscriptPanel() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.words = {{1.0, 1.4, "My", 1}, {1.5, 1.9, "fellow", 1}, {2.0, 2.8, "Americans,", 1}, {6.0, 6.4, "ask", 1}, {6.5, 6.9, "not.", 1}};
        t->segments = {seg};
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        auto* panel = win_->findChild<TranscriptPanel*>();
        QVERIFY(panel);
        state()->setSourceMedia(media);
        panel->setMode(TranscriptPanel::Mode::Source);
        QTRY_COMPARE(panel->words().size(), size_t(5));
        auto* list = panel->findChild<QListWidget*>("paperEdit");
        QVERIFY(list && panel->findChild<QToolButton*>("paperAdd") && panel->findChild<QToolButton*>("paperAssemble"));
        panel->selectWords(0, 2);  // "My fellow Americans,"
        QCOMPARE(panel->addToPaperEdit(), 1);
        panel->selectWords(3, 4);  // "ask not."
        QCOMPARE(panel->addToPaperEdit(), 2);
        QVERIFY(list->isVisible());
        // Reordered: "ask not." first.
        list->insertItem(0, list->takeItem(1));
        QCOMPARE(QString::fromStdString(panel->paperEdit()[0].text), QString("ask not."));
        const Id before = state()->sequence()->id;
        const Id made = panel->assemblePaperEdit("Quotes");
        QVERIFY(made && made != before);
        QCOMPARE(state()->sequence()->id, made);
        const Sequence& s = *state()->sequence();
        QCOMPARE(s.name, std::string("Quotes"));
        QCOMPARE(trackAt(s, A1)->clips.size(), size_t(2));
        const double fps = s.fpsValue();
        QCOMPARE(FrameTime(trackAt(s, A1)->clips[0].sourceIn), FrameTime(std::round(5.9 * fps)));  // 0.1 s before "ask"
        QCOMPARE(trackAt(s, A1)->clips[1].start, trackAt(s, A1)->clips[0].end());
        // One undo step takes the sequence away.
        state()->undo();
        QVERIFY(!state()->project().findSequence(made));
        panel->clearPaperEdit();
        QCOMPARE(list->count(), 0);
        panel->setMode(TranscriptPanel::Mode::Sequence);
        state()->newProject();
        QApplication::processEvents();
    }

    void speakerLabelsInTranscriptPanel() {
        // Two people: the panel names them at each change, in both modes.
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        TranscriptSegment a, b, c;
        a.words = {{0.5, 0.9, "Hello", 1}, {1.0, 1.4, "there.", 1}};
        a.speaker = 0;
        b.words = {{1.6, 2.0, "Hi", 1}, {2.1, 2.5, "back.", 1}};
        b.speaker = 1;
        c.words = {{2.7, 3.0, "Good.", 1}};
        c.speaker = 0;
        t->segments = {a, b, c};
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        auto* panel = win_->findChild<TranscriptPanel*>();
        QVERIFY(panel);
        auto* text = panel->findChild<QTextEdit*>();
        QVERIFY(text);
        state()->setSourceMedia(media);
        panel->setMode(TranscriptPanel::Mode::Source);
        QTRY_COMPARE(panel->words().size(), size_t(5));
        QCOMPARE(panel->words()[2].speaker, std::string("Speaker 2"));
        QString shown = text->toPlainText();
        QVERIFY2(shown.indexOf("Speaker 1") == 0 && shown.indexOf("Speaker 2") > shown.indexOf("there."), qPrintable(shown));
        QCOMPARE(shown.count("Speaker 1"), 2);  // again when they speak again
        // Renaming is one undo step, and every label follows.
        QVERIFY(panel->renameSpeaker("Speaker 1", "Ann"));
        QCOMPARE(speakerName(*state()->project().findMedia(media)->transcript, 0), std::string("Ann"));
        QTRY_VERIFY(text->toPlainText().count("Ann") == 2);
        QVERIFY(!panel->renameSpeaker("Nobody", "Bob"));
        state()->undo();
        QTRY_VERIFY(text->toPlainText().count("Speaker 1") == 2);
        state()->redo();
        // In the cut, the names come from each clip's transcript.
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        panel->setMode(TranscriptPanel::Mode::Sequence);
        QTRY_COMPARE(panel->words().size(), size_t(5));
        QCOMPARE(panel->words()[0].speaker, std::string("Ann"));
        QVERIFY(text->toPlainText().contains("Speaker 2"));
        // The Transcribe dialog offers speaker labels (when this build can run them).
        TranscribeDialog dlg(1, win_.get());
        auto* label = dlg.findChild<QCheckBox*>("labelSpeakers");
        auto* count = dlg.findChild<QComboBox*>("speakerCount");
        QVERIFY(label && count);
        QCOMPARE(label->isEnabled(), diarizerAvailable());
        if (label->isEnabled()) {
            label->setChecked(true);
            count->setCurrentIndex(count->findData(2));
            QVERIFY(dlg.options().speakers);
            QCOMPARE(dlg.options().speakerCount, 2);
            label->setChecked(false);
            QVERIFY(!dlg.options().speakers);
            QVERIFY(!count->isEnabled());
        }
        state()->newProject();
        QApplication::processEvents();
    }

    void findShotsByDescription() {
        // 4 s of a red scene, then 4 s of a blue one.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        for (int k = 0; k < 2; ++k) {
            Clip c = makeGeneratorClip(gen, "color", 100);
            c.generator.params["color.r"] = Param(k == 0 ? 0.85 : 0.05);
            c.generator.params["color.g"] = Param(k == 0 ? 0.08 : 0.15);
            c.generator.params["color.b"] = Param(k == 0 ? 0.06 : 0.9);
            c.start = k * 100;
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, c);
        }
        ExportSettings st;
        st.path = (dir_.path() + "/scenes.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        const auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        auto* panel = win_->findChild<ShotSearchPanel*>();
        QVERIFY(panel);
        auto* status = panel->findChild<QLabel*>("shotStatus");
        QVERIFY(status);
        QCOMPARE(panel->search("   "), 0);
        if (!visualSearchAvailable() || !visualModel().installed()) QSKIP("Needs ONNX Runtime and the CLIP model (MONTAGE_VISUAL_MODEL)");
        QTRY_VERIFY(status->text().contains("0 of 1"));
        QVERIFY(panel->indexMissing());
        QTRY_VERIFY(status->text().contains("1 of 1"));
        QVERIFY(state()->project().findMedia(ids[0])->visual);
        // The index is not an edit: nothing to undo, but it is saved.
        QVERIFY(panel->search("a blue image") > 0);
        const ShotMatch top = panel->results().front();
        QVERIFY2(top.best >= 4 && top.best <= 8, qPrintable(QString::number(top.best)));
        QCOMPARE(panel->findChild<QListWidget*>("shotResults")->count(), int(panel->results().size()));
        // Opening it marks the moment in the Source monitor.
        panel->open(0);
        QCOMPARE(state()->sourceMedia(), ids[0]);
        const double fps = state()->sequence()->fpsValue();
        QVERIFY(state()->sourceIn() >= FrameTime(std::floor((top.start - 0.01) * fps)) && state()->sourceIn() < state()->sourceOut());
        QVERIFY(panel->search("a red image") > 0);
        QVERIFY2(panel->results().front().best < 4, qPrintable(QString::number(panel->results().front().best)));
        // Find Similar Shots from the timeline: like the red scene at the playhead, leaving that moment out.
        {
            const Id media = ids[0];
            QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
            state()->setPlayhead(25);
            state()->setSelection({}, false);
            win_->findChild<QAction*>("findSimilarShots")->trigger();
            QVERIFY(!panel->results().empty());
            for (const ShotMatch& h : panel->results()) QVERIFY(!(h.start <= 1.0 && h.end >= 1.0));
            QVERIFY(panel->findChild<QLineEdit*>()->placeholderText().startsWith("Like"));
            state()->undo();
            QVERIFY(panel->search("a red image") > 0);
        }
        // A result saved as a subclip is named after the search.
        const Id sub = panel->makeSubclip(0);
        QVERIFY(sub);
        QCOMPARE(state()->project().findMedia(sub)->name, std::string("A red image"));
        QVERIFY(state()->project().findMedia(sub)->subclipIn < 4);
        // Auto-Tag: keywords from the known labels, as one undo step (the subclip from its media's index).
        auto* bin = win_->findChild<MediaBinWidget*>();
        const int tagged = bin->autoTag({ids[0], sub});
        QSet<QString> known;
        for (const TagCategory& c : tagCategories())
            for (const TagLabel& l : c.labels) known.insert(QString::fromStdString(l.keyword));
        for (Id id : {ids[0], sub})
            for (const std::string& k : state()->project().findMedia(id)->keywords) QVERIFY2(known.contains(QString::fromStdString(k)), k.c_str());
        if (tagged > 0) {
            state()->undo();
            QVERIFY(state()->project().findMedia(ids[0])->keywords.empty() && state()->project().findMedia(sub)->keywords.empty());
            state()->redo();
        }
        const QString saved = dir_.path() + "/shots.montage";
        QString err2;
        QVERIFY(state()->save(saved, &err2));
        state()->newProject();
        QVERIFY(win_->openProject(saved));
        QVERIFY(state()->project().media.at(0).visual && !state()->project().media.at(0).visual->samples.empty());
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void maskOverlayInProgramMonitor() {
        loadDemo();
        // A blur limited to an ellipse on the red clip.
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Mask", [red](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "gaussian_blur");
            e.params["mask.shape"] = 1.0;
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        state()->setSelection({red}, false);
        state()->setPlayhead(10);
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        auto* overlay = viewer->findChild<MaskOverlay*>();
        QVERIFY(overlay);
        auto shapes = overlay->shapes();
        QCOMPARE(shapes.size(), size_t(1));
        QPointF center, wh, hh;
        QVERIFY(overlay->handles(shapes[0], center, wh, hh));
        QVERIFY(wh.x() > center.x() && hh.y() > center.y());

        // Drag inside the mask to move it right by a tenth of the picture.
        const QRectF r = viewer->imageRect();
        const QPoint from = center.toPoint(), to = (center + QPointF(r.width() * 0.1, 0)).toPoint();
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), viewer->mapToGlobal(QPointF(to)), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(viewer, &move);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, to);
        const Effect& fx = edit::clipById(*state()->sequence(), red)->effects.back();
        QVERIFY2(std::fabs(fx.p("mask.x", 10) - 0.6) < 0.02, qPrintable(QString::number(fx.p("mask.x", 10))));
        QVERIFY(std::fabs(fx.p("mask.y", 10) - 0.5) < 0.02);

        // The width handle resizes it.
        shapes = overlay->shapes();
        QVERIFY(overlay->handles(shapes[0], center, wh, hh));
        const QPoint w0 = wh.toPoint(), w1 = (wh + QPointF(r.width() * 0.1, 0)).toPoint();
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, w0);
        QMouseEvent move2(QEvent::MouseMove, QPointF(w1), viewer->mapToGlobal(QPointF(w1)), Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(viewer, &move2);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, w1);
        const Effect& fx2 = edit::clipById(*state()->sequence(), red)->effects.back();
        QVERIFY2(std::fabs(fx2.p("mask.w", 10) - 0.6) < 0.03, qPrintable(QString::number(fx2.p("mask.w", 10))));
        // Each drag is one undo step.
        state()->undo();
        const Effect& undone = edit::clipById(*state()->sequence(), red)->effects.back();
        QVERIFY(std::fabs(undone.p("mask.w", 10, 0.4) - 0.4) < 1e-6);
        QVERIFY(std::fabs(undone.p("mask.x", 10) - 0.6) < 0.02);  // the move stays

        // A Corner Pin shows its corners; dragging one moves it.
        QVERIFY(state()->edit("Pin", [red](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects.push_back(makeEffect(p, "corner_pin"));
            return true;
        }));
        const auto pins = overlay->pins();
        QCOMPARE(pins.size(), size_t(1));
        QPointF tl, br;
        QVERIFY(overlay->cornerHandle(pins[0], 0, tl) && overlay->cornerHandle(pins[0], 2, br));
        QVERIFY(std::fabs(tl.x() - r.left()) < 1.5 && std::fabs(br.y() - r.bottom()) < 1.5);
        const QPoint c0 = tl.toPoint() + QPoint(1, 1), c1 = (tl + QPointF(r.width() * 0.1, r.height() * 0.2)).toPoint();
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, c0);
        QMouseEvent move3(QEvent::MouseMove, QPointF(c1), viewer->mapToGlobal(QPointF(c1)), Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(viewer, &move3);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, c1);
        const Effect& pin = edit::clipById(*state()->sequence(), red)->effects.back();
        QVERIFY2(std::fabs(pin.p("tl_x", 10) - 0.1) < 0.02 && std::fabs(pin.p("tl_y", 10) - 0.2) < 0.02,
                 qPrintable(QString("%1 %2").arg(pin.p("tl_x", 10)).arg(pin.p("tl_y", 10))));
        QVERIFY(std::fabs(pin.p("br_x", 10) - 1) < 1e-9);  // the others stay
        state()->undo();
        QVERIFY(std::fabs(edit::clipById(*state()->sequence(), red)->effects.back().p("tl_x", 10)) < 1e-9);
        state()->setSelection({}, false);
    }

    void globalMute() {
        // A tone on A1: heard, then silent under Global Mute, with the clip and track untouched.
        state()->newProject();
        const QString wav = dir_.path() + "/mute-tone.wav";
        {
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int rate = 48000, n = rate;
            auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
            f.write("RIFF", 4), u32(36 + n * 2), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
            f.write("data", 4), u32(n * 2);
            for (int i = 0; i < n; ++i) u16(uint16_t(int16_t(std::lround(8000 * std::sin(2 * M_PI * 440 * i / rate)))));
        }
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        QVERIFY(program);
        auto peak = [](const std::vector<float>& v) {
            float m = 0;
            for (float x : v) m = std::max(m, std::fabs(x));
            return m;
        };
        QTRY_VERIFY(peak(program->heard(4800, 4800)) > 0.1f);
        QAction* mute = win_->findChild<QAction*>("globalMute");
        QVERIFY(mute && mute->isCheckable());
        mute->trigger();
        QVERIFY(program->globalMute());
        QCOMPARE(peak(program->heard(4800, 4800)), 0.0f);
        // Nothing in the project changed.
        const Sequence* s = state()->sequence();
        QVERIFY(!s->audioTracks[0].muted && s->audioTracks[0].clips.front().enabled);
        mute->trigger();
        QVERIFY(!program->globalMute());
        QVERIFY(peak(program->heard(4800, 4800)) > 0.1f);
    }

    void trackFoldersOnTheTimeline() {
        loadDemo();
        // Four audio tracks, A2 and A3 in a "Dialogue" folder, each with a clip.
        state()->edit("Tracks", [](Project& p, Sequence& s) {
            while (s.audioTracks.size() < 4) edit::addTrack(p, s, TrackKind::Audio);
            return true;
        });
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        Id onA2 = 0;
        QVERIFY(state()->apply("Place", [&](Project& p, Sequence& s) {
            auto r = edit::placeMedia(p, s, ids[0], 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 1}, false);
            if (r.ok && !r.created.empty()) onA2 = r.created.back();
            return r;
        }));
        QVERIFY(onA2);
        QVERIFY(state()->apply("Folder", [](Project&, Sequence& s) {
            return edit::setTrackFolder(s, {{TrackKind::Audio, 1}, {TrackKind::Audio, 2}}, "Dialogue");
        }));
        TimelineWidget* tl = win_->timeline();
        const QRect header = tl->folderHeaderRect(TrackKind::Audio, "Dialogue");
        QVERIFY(header.isValid());
        QVERIFY(tl->trackShown({TrackKind::Audio, 1}) && tl->trackShown({TrackKind::Audio, 3}));
        QVERIFY(tl->clipBounds(onA2).top() > header.bottom());  // under its header

        // Clicking the arrow collapses it: its tracks are hidden, the rest move up; one undo step.
        QTest::mouseClick(tl->viewport(), Qt::LeftButton, {}, QPoint(12, header.center().y()));
        QVERIFY(edit::folderCollapsed(*state()->sequence(), TrackKind::Audio, "Dialogue"));
        QVERIFY(!tl->trackShown({TrackKind::Audio, 1}) && !tl->trackShown({TrackKind::Audio, 2}));
        QVERIFY(tl->trackShown({TrackKind::Audio, 3}));
        QVERIFY(tl->clipBounds(onA2).isNull());
        state()->undo();
        QVERIFY(tl->trackShown({TrackKind::Audio, 1}));
        state()->redo();

        // The folder's M button mutes both of its tracks, and again unmutes them.
        const QRect h2 = tl->folderHeaderRect(TrackKind::Audio, "Dialogue");
        const QPoint mute(h2.right() - 10 - 3 * 20 + 10, h2.top() + 10);
        QTest::mouseClick(tl->viewport(), Qt::LeftButton, {}, mute);
        QVERIFY(state()->sequence()->audioTracks[1].muted && state()->sequence()->audioTracks[2].muted);
        QVERIFY(!state()->sequence()->audioTracks[3].muted);
        QTest::mouseClick(tl->viewport(), Qt::LeftButton, {}, mute);
        QVERIFY(!state()->sequence()->audioTracks[1].muted && !state()->sequence()->audioTracks[2].muted);

        // The mixer shows the folder's fader (a VCA); moving it is one undoable level for the folder.
        auto* mixer = win_->findChild<MixerPanel*>();
        QTRY_VERIFY(mixer->folderFader("Dialogue"));
        mixer->folderFader("Dialogue")->setValue(-60);  // -6.0 dB
        QCOMPARE(edit::folderGain(*state()->sequence(), TrackKind::Audio, "Dialogue"), -6.0);
        state()->undo();
        QCOMPARE(edit::folderGain(*state()->sequence(), TrackKind::Audio, "Dialogue"), 0.0);
        QTRY_COMPARE(mixer->folderFader("Dialogue")->value(), 0);
        state()->newProject();
        QTRY_VERIFY(!mixer->folderFader("Dialogue"));
    }

    void quadScopes() {
        // A frame with a ramp and colour bars, in all four scopes at once.
        QImage frame(320, 180, QImage::Format_RGB32);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
                frame.setPixel(x, y, y < 90 ? qRgb(x * 255 / 319, x * 255 / 319, x * 255 / 319)
                                            : QColor::fromHsv((x / 40) * 45 % 360, 200, 220).rgb());
        ScopesWidget scopes;
        scopes.resize(640, 420);
        scopes.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scopes));
        scopes.setMode(ScopesWidget::Mode::Quad);
        QCOMPARE(scopes.findChild<QComboBox*>()->currentText(), QString("All Four"));
        scopes.setFrame(frame, 0);
        QTRY_VERIFY(scopes.hasSignal());
        const QImage img = scopes.grab().toImage();
        // Every quarter holds a trace.
        const QRect quarters[4] = {QRect(0, 30, 320, 195), QRect(320, 30, 320, 195), QRect(0, 225, 320, 195), QRect(320, 225, 320, 195)};
        for (const QRect& q : quarters) {
            int lit = 0;
            for (int y = q.top(); y <= q.bottom(); ++y)
                for (int x = q.left(); x <= q.right(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    lit += std::max({c.red(), c.green(), c.blue()}) > 160;
                }
            QVERIFY2(lit > 40, qPrintable(QStringLiteral("%1,%2: %3").arg(q.x()).arg(q.y()).arg(lit)));
        }
        scopes.setMode(ScopesWidget::Mode::Waveform);
        QVERIFY(scopes.hasSignal());
    }

    void workspaces() {
        auto dock = [this](const char* name) { return win_->findChild<QDockWidget*>(name); };
        // On screen: shown and in front (Qt moves the panels behind a tab out of sight rather than hiding them).
        auto shown = [&](const char* name) { return dock(name)->isVisible() && !dock(name)->visibleRegion().isEmpty(); };
        auto* bar = win_->findChild<QTabBar*>("workspaceBar");
        QVERIFY(bar && dock("scopes") && dock("mixer") && dock("source") && dock("meters"));
        QCOMPARE(win_->workspaces().mid(0, 6), MainWindow::builtInWorkspaces());
        QCOMPARE(win_->findChild<QAction*>("workspaceColour")->shortcut(), QKeySequence("Alt+Shift+2"));

        // Colour: the scopes and the Program monitor across the top, the Inspector beside them; no Source monitor.
        win_->findChild<QAction*>("workspaceColour")->trigger();
        QCOMPARE(win_->currentWorkspace(), QString("Colour"));
        QCOMPARE(bar->tabText(bar->currentIndex()), QString("Colour"));
        QVERIFY(shown("scopes") && shown("program") && shown("inspector"));
        QVERIFY(!shown("source") && !shown("mixer"));
        QVERIFY(dock("scopes")->geometry().right() < dock("program")->geometry().left());
        QVERIFY(dock("inspector")->geometry().left() > dock("program")->geometry().left());

        // Audio from the bar: the mixer in front where the Source monitor was.
        bar->setCurrentIndex(int(win_->workspaces().indexOf("Audio")));
        QCOMPARE(win_->currentWorkspace(), QString("Audio"));
        QVERIFY(shown("mixer") && shown("meters"));
        QVERIFY(!shown("scopes"));
        QVERIFY(!shown("source"));  // tabbed behind the mixer
        QVERIFY(dock("mixer")->geometry().right() < dock("program")->geometry().left());
        QVERIFY(win_->findChild<QAction*>("workspaceAudio")->isChecked());

        // A layout of one's own, saved by name, comes back as it was.
        QVERIFY(win_->applyWorkspace("Editing"));
        QVERIFY(shown("source") && shown("meters"));
        dock("meters")->hide();
        QVERIFY(!win_->saveWorkspace("Colour"));  // a built-in's name
        QVERIFY(!win_->saveWorkspace("  "));
        QVERIFY(win_->saveWorkspace("No Meters"));
        QCOMPARE(win_->currentWorkspace(), QString("No Meters"));
        QVERIFY(win_->workspaces().contains("No Meters"));
        QCOMPARE(bar->count(), 7);
        QVERIFY(win_->applyWorkspace("Editing"));
        QVERIFY(shown("meters"));
        QVERIFY(win_->applyWorkspace("No Meters"));
        QVERIFY(!shown("meters") && shown("source"));
        // Reset puts the current workspace back as saved.
        dock("source")->hide();
        win_->findChild<QAction*>("resetWorkspace")->trigger();
        QVERIFY(shown("source") && !shown("meters"));
        QVERIFY(!win_->deleteWorkspace("Editing"));
        QVERIFY(win_->deleteWorkspace("No Meters"));
        QVERIFY(!win_->workspaces().contains("No Meters"));
        QCOMPARE(bar->count(), 6);
        QVERIFY(!win_->applyWorkspace("No Meters"));
        QVERIFY(win_->applyWorkspace("Editing"));
        QVERIFY(shown("source") && shown("program") && shown("meters"));
    }

    void sequenceIndexPanel() {
        loadDemo();
        state()->edit("Marker", [](Project&, Sequence& s) {
            Marker m;
            m.t = 30;
            m.name = "Chorus";
            m.color = 7;  // Rose
            s.markers.push_back(m);
            return true;
        });
        auto* panel = win_->findChild<SequenceIndexPanel*>();
        QVERIFY(panel);
        // Two colour clips, a title and the marker.
        QTRY_COMPARE(panel->rowCount(), 4);
        QCOMPARE(win_->findChild<QLabel*>("indexCount")->text(), QString("4 of 4"));
        // The filter matches any column: a name, a kind, a track.
        panel->setFilter("blue");
        QCOMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Track), QString("V1"));
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Start), QString::fromStdString(formatTimecode(60, state()->sequence()->fps)));
        panel->setFilter("marker");
        QCOMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Name), QString("Chorus"));
        panel->setFilter("V2");
        QCOMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Kind), QString("Title"));
        // A colour's name finds what carries it.
        panel->setFilter("rose");
        QCOMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Name), QString("Chorus"));
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Color), QString("Rose"));
        // Activating a row selects the clip and moves the playhead to it.
        panel->setFilter("blue");
        panel->activate(0);
        const Clip* blue = clipNamed(*state()->sequence(), "Blue");
        QCOMPARE(state()->playhead(), FrameTime(60));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{blue->id});
        // Renamed in place, as one undo step; the index follows.
        QVERIFY(panel->rename(0, "Navy"));
        QVERIFY(clipNamed(*state()->sequence(), "Navy"));
        panel->setFilter("navy");
        QCOMPARE(panel->rowCount(), 1);
        state()->undo();
        QVERIFY(clipNamed(*state()->sequence(), "Blue"));
        QCOMPARE(panel->rowCount(), 0);
        panel->setFilter("");
        QCOMPARE(panel->rowCount(), 4);
    }

    void peoplePanel() {
        state()->newProject();
        const QString faces = QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/");
        const auto ids = state()->importFiles({faces + "jfk-color.jpg", faces + "jfk-looking-up.jpg", faces + "armstrong.jpg"});
        QCOMPARE(ids.size(), size_t(3));
        auto* panel = win_->findChild<PeoplePanel*>();
        QVERIFY(panel);
        auto* status = panel->findChild<QLabel*>("peopleStatus");
        if (!faceSearchAvailable() || !faceModel().installed()) QSKIP("Needs ONNX Runtime and the face models (MONTAGE_FACE_MODEL)");
        QTRY_VERIFY(status->text().contains("0 of 3"));
        QVERIFY(panel->findChild<QPushButton*>("findPeople")->isEnabled());
        QVERIFY(panel->findPeople());
        QTRY_VERIFY(status->text().contains("3 of 3"));
        QVERIFY(!panel->findChild<QPushButton*>("findPeople")->isEnabled());
        QCOMPARE(int(panel->people().size()), 2);
        auto* list = panel->findChild<QListWidget*>("peopleList");
        QCOMPARE(list->count(), 2);
        // Their faces load as icons.
        QTRY_VERIFY(!list->item(0)->icon().isNull());
        // Kennedy (in two stills) first; choosing him lists both.
        QCOMPARE(panel->selectPerson(0), 2);
        QCOMPARE(panel->findChild<QListWidget*>("personMoments")->count(), 2);
        const int jfk = panel->currentPerson();
        // Named in place, as one undo step.
        list->item(0)->setText("Jack");
        QTRY_COMPARE(QString::fromStdString(personName(state()->project(), jfk)), QString("Jack"));
        QCOMPARE(panel->currentPerson(), jfk);
        // A smart bin of the stills he is in.
        const Id bin = panel->makeSmartBin(jfk);
        QVERIFY(bin);
        auto* binWidget = win_->findChild<MediaBinWidget*>();
        QCOMPARE(binWidget->currentSmartBin(), bin);
        QCOMPARE(int(smartBinMedia(state()->project(), *findSmartBin(state()->project(), bin)).size()), 2);
        // Opening a still loads it in the Source monitor.
        panel->open(0);
        QVERIFY(state()->sourceMedia() == ids[0] || state()->sourceMedia() == ids[1]);
        QCOMPARE(panel->makeSubclip(0), Id(0));  // no subclips of stills
        // Same Person As: joined, the smart bin follows, and undo splits them again.
        const int neil = panel->people()[1].id;
        QVERIFY(panel->merge(neil, jfk));
        QCOMPARE(int(panel->people().size()), 1);
        QCOMPARE(int(smartBinMedia(state()->project(), *findSmartBin(state()->project(), bin)).size()), 3);
        state()->undo();
        QCOMPARE(int(panel->people().size()), 2);
        state()->undo();  // the smart bin
        state()->undo();  // the name
        QCOMPARE(QString::fromStdString(personName(state()->project(), jfk)), QString("Person %1").arg(jfk));
        // The bin's search finds people by name.
        QVERIFY(panel->rename(neil, "Neil Armstrong"));
        QVERIFY(mediaMatchesSearch(*state()->project().findMedia(ids[2]), "neil", &state()->project()));
    }

    void objectMaskFromViewer() {
        // A red ball crossing textured ground, 320 x 180 at 25 fps.
        QImage bg(320, 180, QImage::Format_RGB32), ball(320, 180, QImage::Format_ARGB32);
        ball.fill(Qt::transparent);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                bg.setPixel(x, y, qRgb(int(76 + 51 * std::sin(x / 18.0)), int(115 + 38 * std::sin((x + y) / 20.0)), int(128 + 51 * std::cos(y / 26.0))));
                const double u = (x + 0.5 - 160) / 36, v = (y + 0.5 - 90) / 26;
                const double sh = 1.0 - 0.35 * ((u + 0.28) * (u + 0.28) + (v + 0.3) * (v + 0.3));
                if (u * u + v * v <= 1) ball.setPixel(x, y, qRgba(int(230 * sh), int(51 * sh), int(38 * sh), 255));
            }
        const QString bgPng = dir_.path() + "/ground.png", ballPng = dir_.path() + "/ball.png";
        QVERIFY(bg.save(bgPng) && ball.save(ballPng));
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320;
        gs.height = 180;
        gs.fps = {25, 1};
        std::string err;
        MediaItem mb, mo;
        mb.id = gen.newId();
        mo.id = gen.newId();
        QVERIFY(probeMedia(bgPng.toStdString(), mb, &err) && probeMedia(ballPng.toStdString(), mo, &err));
        gen.media.push_back(mb);
        gen.media.push_back(mo);
        const int frames = 6;
        Clip cb = makeClip(gen, mb, TrackKind::Video, gs), co = makeClip(gen, mo, TrackKind::Video, gs);
        cb.duration = co.duration = frames;
        auto centre = [](int i) { return QPointF(90.0 + 8 * i, 90 + 10 * std::sin(i / 2.0)); };
        for (int i = 0; i < frames; ++i) {
            co.motion.params["pos_x"].addKey(i, centre(i).x() - 160, Interp::Hold);
            co.motion.params["pos_y"].addKey(i, centre(i).y() - 90, Interp::Hold);
        }
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, cb);
        edit::overwrite(gen, gs, {TrackKind::Video, 1}, co);
        ExportSettings st;
        st.path = (dir_.path() + "/ball.mp4").toStdString();
        st.audioCodec = "none";
        st.crf = 10;
        st.preset = "ultrafast";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());

        state()->newProject();
        auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->edit("Invert", [clip](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "invert");
            e.params["mask.shape"] = Param(3.0);
            e.params["mask.feather"] = Param(1.0);
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        const Id fxId = edit::clipById(*state()->sequence(), clip)->effects.back().id;
        state()->setSelection({clip}, false);
        state()->setPlayhead(0);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        auto visible = [&](const char* name) -> QWidget* {
            for (auto* w : win_->findChildren<QWidget*>(name))
                if (w->isVisibleTo(win_.get())) return w;
            return nullptr;
        };
        // The Object shape has its own controls in place of the shape tracker.
        QTRY_VERIFY(visible("objectStatus"));
        QVERIFY(static_cast<QLabel*>(visible("objectStatus"))->text().contains("Click the object"));
        QVERIFY(visible("trackObjectForward") && visible("trackObjectBack") && visible("clearObject"));
        QVERIFY(!visible("trackMaskForward"));
        // Tracking needs a click first.
        static_cast<QToolButton*>(visible("trackObjectForward"))->click();
        QTest::qWait(50);
        QVERIFY(!edit::clipById(*state()->sequence(), clip)->effects.back().object);

        if (!segmenterAvailable() || !objectModel().installed())
            QSKIP("Picking objects needs ONNX Runtime and the model (MONTAGE_OBJECT_MODEL)");
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        auto* overlay = viewer->findChild<MaskOverlay*>();
        QVERIFY(overlay);
        QCOMPARE(overlay->objectTargets().size(), size_t(1));
        const QRectF r = viewer->imageRect();
        auto at = [&](QPointF seqPx) { return QPointF(r.left() + seqPx.x() * r.width() / 320, r.top() + seqPx.y() * r.height() / 180).toPoint(); };
        auto object = [&]() { return edit::clipById(*state()->sequence(), clip)->effects.back().object; };

        // A click on the ball: the click is kept at once, the segmentation follows.
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(centre(0)));
        QVERIFY(object() && object()->prompts.size() == 1);
        QCOMPARE(object()->prompts.begin()->second.at(0).label, 1);
        QTRY_VERIFY_WITH_TIMEOUT(object()->frames.count(0) == 1, 60000);
        QTRY_COMPARE(overlay->pending(), 0);
        std::vector<float> logits;
        QVERIFY(object()->logits(0, logits));
        QVERIFY2(objectCoverage(logits) > 0.04 && objectCoverage(logits) < 0.08, qPrintable(QString::number(objectCoverage(logits))));
        QTRY_VERIFY(static_cast<QLabel*>(visible("objectStatus"))->text().contains("1 segmented"));
        // One undo step takes the click and its segmentation away; redo brings both back.
        state()->undo();
        QVERIFY(!object() || object()->prompts.empty());
        state()->redo();
        QVERIFY(object() && object()->frames.count(0) == 1);

        // Alt-click: not the object. Ctrl-click on it: removed again.
        const QPoint off = at(QPointF(280, 30));
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::AltModifier, off);
        QCOMPARE(int(object()->prompts.at(0).size()), 2);
        QCOMPARE(object()->prompts.at(0).back().label, 0);
        QTRY_COMPARE_WITH_TIMEOUT(overlay->pending(), 0, 60000);
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::ControlModifier, off);
        QCOMPARE(int(object()->prompts.at(0).size()), 1);
        QTRY_COMPARE_WITH_TIMEOUT(overlay->pending(), 0, 60000);
        // A dragged box replaces nothing else on the frame and adds two corners.
        const QPoint b0 = at(centre(0) - QPointF(45, 32)), b1 = at(centre(0) + QPointF(45, 32));
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, b0);
        QMouseEvent move(QEvent::MouseMove, QPointF(b1), viewer->mapToGlobal(QPointF(b1)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer, &move);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, b1);
        QCOMPARE(int(object()->prompts.at(0).size()), 3);
        QCOMPARE(object()->prompts.at(0)[0].label, 2);
        QCOMPARE(object()->prompts.at(0)[1].label, 3);
        QTRY_COMPARE_WITH_TIMEOUT(overlay->pending(), 0, 60000);

        // Track ▶ follows it to the end of the clip, as one undo step.
        static_cast<QToolButton*>(visible("trackObjectForward"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(object()->frames.size() == size_t(frames), 120000);
        for (int n = 0; n < frames; ++n) {
            QVERIFY(object()->logits(n, logits));
            QVERIFY2(objectCoverage(logits) > 0.04 && objectCoverage(logits) < 0.08, qPrintable(QString("%1: %2").arg(n).arg(objectCoverage(logits))));
        }
        // The program monitor inverts the ball only.
        state()->setPlayhead(3);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        RenderOptions ro;
        const Image frame3 = renderProgramFrame(state()->project(), *state()->sequence(), 3, ro);
        const QPointF c3 = centre(3);
        QVERIFY2(frame3.at(int(c3.x()), int(c3.y()))[0] < 0.4f, "the ball is inverted (red becomes dark)");
        QVERIFY(std::fabs(frame3.at(300, 170)[0] - float(bg.pixelColor(300, 170).redF())) < 0.08f);

        // Saved with the project.
        QString err2;
        const QString saved = dir_.path() + "/object.montage";
        QVERIFY2(state()->save(saved, &err2), qPrintable(err2));
        state()->newProject();
        QVERIFY(win_->openProject(saved));
        const Clip* reopened = edit::clipById(*state()->sequence(), clip);
        QVERIFY(reopened && reopened->effects.back().object && reopened->effects.back().object->frames.size() == size_t(frames));
        QCOMPARE(reopened->effects.back().id, fxId);
        state()->setSelection({}, false);
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
    }

    void timeRemappingKeepsSoundWithPicture() {
        // A file with picture and sound, placed as linked video and audio clips.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 160;
        gs.height = 90;
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "color", 60));
        MediaItem wav;
        wav.id = gen.newId();
        std::string err;
        QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", wav, &err));
        gen.media.push_back(wav);
        QVERIFY(edit::placeMedia(gen, gs, wav.id, 0, 0, 60, {TrackKind::Video, 1}, {TrackKind::Audio, 0}, false).ok);
        ExportSettings st;
        st.path = (dir_.path() + "/av.mp4").toStdString();
        st.preset = "ultrafast";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id v = state()->sequence()->videoTracks[0].clips.at(0).id;
        const Id a = state()->sequence()->audioTracks[0].clips.at(0).id;
        state()->setSelection({v}, false);
        QApplication::processEvents();
        // The Time Remapping speed (0..1000 %; 0 holds the frame).
        QDoubleSpinBox* speed = nullptr;
        for (auto* sp : win_->findChildren<QDoubleSpinBox*>())
            if (sp->suffix() == " %" && sp->minimum() == 0 && sp->maximum() == 1000) speed = sp;
        QVERIFY(speed);
        speed->setValue(200);
        auto clipSpeed = [&](Id id) { return edit::clipById(*state()->sequence(), id)->speedAt(0); };
        QVERIFY(std::fabs(clipSpeed(v) - 2) < 1e-6);
        QVERIFY(std::fabs(clipSpeed(a) - 2) < 1e-6);  // the sound follows
        QVERIFY(edit::clipById(*state()->sequence(), a)->ramped());
        state()->undo();
        QVERIFY(std::fabs(clipSpeed(v) - 1) < 1e-6 && std::fabs(clipSpeed(a) - 1) < 1e-6);
        state()->newProject();
    }

    void backgroundRenderWhenIdle() {
        loadDemo();
        RenderCache::instance().clear();
        win_->refreshRenderBar(true);
        // A blur on Blue: the only stretch worth rendering ahead besides the title.
        const Clip* blue = clipNamed(*state()->sequence(), "Blue");
        const Id blueId = blue->id;
        const FrameTime blueStart = blue->start, blueEnd = blue->end();
        QVERIFY(state()->edit("Blur", [blueId](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, blueId);
            Effect e = makeEffect("gaussian_blur", p.newId());
            c->effects.push_back(e);
            return true;
        }));
        const auto ranges = rangesToRender(*state()->sequence());
        QVERIFY(std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) { return r.first <= blueStart && r.second >= blueEnd; }));
        auto* action = win_->findChild<QAction*>("backgroundRender");
        QVERIFY(action);
        win_->setBackgroundRenderDelay(50);
        if (!action->isChecked()) action->trigger();
        QVERIFY(win_->backgroundRender());
        // After the quiet spell the blurred clip is rendered, and the render bar shows it.
        QTRY_VERIFY_WITH_TIMEOUT(!win_->backgroundRendering() && RenderCache::instance().count() >= int(blueEnd - blueStart), 30000);
        // An edit stops it and it starts again later; turning it off leaves the rendered frames.
        // Turned off again (and the setting with it).
        action->trigger();
        QVERIFY(!win_->backgroundRender());
        win_->setBackgroundRenderDelay(4000);
        RenderCache::instance().clear();
        state()->newProject();
    }

    void inspectorEditsAllSelected() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setSelection({red, blue});
        QApplication::processEvents();
        const Id primary = state()->primaryClip()->id, other = primary == red ? blue : red;
        auto* note = win_->findChild<QLabel*>("inspectorMultiClip");
        QVERIFY(note && note->text().contains("2 selected"));
        QDoubleSpinBox* opacity = nullptr;
        for (auto* sp : win_->findChildren<QDoubleSpinBox*>())
            if (sp->suffix() == " %" && sp->maximum() == 100 && sp->value() == 100 && sp->isVisibleTo(win_.get())) opacity = sp;
        QVERIFY(opacity);
        opacity->setValue(40);
        // The same parameter changed on both clips, in one undo step.
        const Clip* a = edit::clipById(*state()->sequence(), primary);
        const char* name = std::fabs(a->motion.p("opacity", 0) - 40) < 0.01 ? "opacity" : "crop_bottom";
        QVERIFY(std::fabs(a->motion.p(name, 0) - 40) < 0.01);
        QVERIFY(std::fabs(edit::clipById(*state()->sequence(), other)->motion.p(name, 0) - 40) < 0.01);
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), other)->motion.p("opacity", 0), 100.0);
        QCOMPARE(edit::clipById(*state()->sequence(), primary)->motion.p("opacity", 0), 100.0);
        // One clip selected: no note, and only that clip changes.
        state()->setSelection({red});
        QApplication::processEvents();
        QVERIFY(!win_->findChild<QLabel*>("inspectorMultiClip"));
    }

    void inspectorEditsAreUndoable() {
        loadDemo();
        Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->setSelection({red});
        QApplication::processEvents();
        // Find the Opacity spin box inside the inspector and change it.
        QDoubleSpinBox* opacity = nullptr;
        for (auto* sp : win_->findChildren<QDoubleSpinBox*>())
            if (sp->suffix() == " %" && sp->maximum() == 100 && sp->value() == 100 && sp->isVisibleTo(win_.get())) {
                // Opacity is the only 0..100 % parameter defaulting to 100 in Transform.
                opacity = sp;
            }
        QVERIFY(opacity);
        opacity->setValue(40);
        const Clip* c = edit::clipById(*state()->sequence(), red);
        QVERIFY(std::fabs(c->motion.p("opacity", 0) - 40) < 0.01 || std::fabs(c->motion.p("crop_bottom", 0) - 40) < 0.01);
        QVERIFY(state()->canUndo());
        state()->undo();
        c = edit::clipById(*state()->sequence(), red);
        QCOMPARE(c->motion.p("opacity", 0), 100.0);
    }

    void playbackAdvances() {
        loadDemo();
        auto controllers = win_->findChildren<PlaybackController*>();
        QVERIFY(!controllers.isEmpty());
        PlaybackController* program = nullptr;
        for (auto* pc : controllers)
            if (pc->sequence() && pc->sequence()->id == state()->sequence()->id) program = pc;
        QVERIFY(program);
        state()->setPlayhead(0);
        program->play();
        QTRY_VERIFY_WITH_TIMEOUT(state()->playhead() > 5, 3000);
        program->pause();
        QVERIFY(!program->isPlaying());
        // J / L shuttle changes speed; K stops.
        program->shuttle(1);
        program->shuttle(1);
        QCOMPARE(program->speed(), 2.0);
        program->shuttle(0);
        QCOMPARE(program->speed(), 0.0);
    }

    void saveAndReopen() {
        loadDemo();
        state()->setPlayhead(50);
        QTest::keyClick(win_.get(), Qt::Key_K, Qt::ControlModifier);
        QString path = dir_.path() + "/saved.montage";
        QString err;
        QVERIFY2(state()->save(path, &err), qPrintable(err));
        QVERIFY(!state()->isModified());
        state()->newProject();
        QVERIFY(win_->openProject(path));
        QCOMPARE(trackAt(*state()->sequence(), V1)->clips.size(), size_t(3));
    }
};

QTEST_MAIN(TestApp)
#include "test_app.moc"
