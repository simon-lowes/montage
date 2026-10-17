// Application integration tests: drive the real main window offscreen —
// timeline mouse gestures, tools, undo, inspector, monitors and playback.
#include <QtTest>
#include <limits>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QPlainTextEdit>

#include <QAbstractScrollArea>
#include <QAction>
#include <QComboBox>
#include <QStandardItemModel>
#include <QCheckBox>
#include <QFormLayout>
#include <QClipboard>
#include <QStatusBar>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QTreeWidgetItemIterator>
#include <atomic>
#include <thread>
#include <sstream>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMimeData>
#include <QScrollBar>
#include <QSlider>
#include <QPushButton>
#include <QTextBrowser>
#include <QColorDialog>
#include <QMenu>
#include <QPainter>
#include <QTabBar>
#include <QTableWidget>
#include <QSpinBox>
#include <QTimer>
#include <QTextEdit>
#include <QToolButton>
#include <QUdpSocket>
#include <QTreeView>
#include <QTreeWidget>
#include <QJsonObject>
#include <QJsonDocument>
#include "core/ProjectLock.h"
#include "ProductionPanel.h"
#include "ReferenceOutputPanel.h"
#include "RemoteReviewPanel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "AutoDuckDialog.h"
#include "Settings.h"
#include "media/MediaPool.h"
#include "media/Exr.h"
#include "media/ImageSequence.h"
#include "core/Ambisonics.h"
#include "media/Sofa.h"
#include "AutoMixDialog.h"
#include "LicencesDialog.h"
#include "CaptionsPanel.h"
#include "CleanFeed.h"
#include "DisplayCalibration.h"
#include "MonitorCalibrationDialog.h"
#include "PatchGenerator.h"
#include "ColorWarperEditor.h"
#include "ColorWheel.h"
#include "CurveEditor.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "ExportDialog.h"
#include <QMessageBox>
#include "media/ContentCredentials.h"
#include "ExposureView.h"
#include "QualityCheckDialog.h"
#include "ConformCheckDialog.h"
#include "OffloadDialog.h"
#include "CaptureDialog.h"
#include "media/Capture.h"
#include "SpellUi.h"
#include "core/ColorGroups.h"
#include "core/Interpretation.h"
#include "LiveBridge.h"
#include "LiveLink.h"
#include "render/Ofx.h"
#include "Assistant.h"
#include "AssistantPanel.h"
#include "PanFollowDialog.h"
#include "RedactFacesDialog.h"
#include "SpectralRepairDialog.h"
#include "automation/McpServer.h"
#include "render/OGraf.h"
#include "GalleryPanel.h"
#include "LinedScriptPanel.h"
#include "TrimPanel.h"
#include "render/Gallery.h"
#include "ClipStripPanel.h"
#include "automation/PythonHost.h"
#include "automation/ScriptHost.h"
#include "ScriptConsole.h"
#include "ScriptPanelWidget.h"
#include "media/SpeechSearch.h"
#include "media/TextReader.h"
#include "core/OnScreenText.h"
#include "ProjectManagerDialog.h"
#include "MediaBinModel.h"
#include "ScopesWidget.h"
#include "render/Gamut.h"
#include "LinkMediaDialog.h"
#include "EffectPresetStore.h"
#include "EffectsBrowser.h"
#include "MediaBinWidget.h"
#include "ReviewOverlay.h"
#include "ChartMatchDialog.h"
#include "LensCorrectionDialog.h"
#include "media/Proxy.h"
#include "StoryboardView.h"
#include "StackedTimelineWidget.h"
#include "SmartBinDialog.h"
#include "ScriptCutDialog.h"
#include "core/KeyframeEdit.h"
#include "core/CaptionTools.h"
#include "core/ColorWarp.h"
#include "core/MaskPath.h"
#include "core/MediaLog.h"
#include "InspectorWidget.h"
#include "core/Bleep.h"
#include "core/Transcript.h"
#include "PsdWriter.h"
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
#include "ControlSurface.h"
#include "OscControl.h"
#include "SpeedEditorControl.h"
#include "TangentControl.h"
#include "control/Osc.h"
#include "control/SpeedEditor.h"
#include "control/Tangent.h"
#include "core/Cdl.h"
#include "render/Amf.h"
#include "control/Hui.h"
#include "control/MackieControl.h"
#include "MulticamPanel.h"
#include "PluginEditorWindow.h"
#include "audio/PluginEffect.h"
#include "KeyframePanel.h"
#include "GradeNodePanel.h"
#include "CompareDialog.h"
#include "Keymap.h"
#include "LoudnessReadout.h"
#include "Voiceover.h"
#include "AdrPanel.h"
#include "AudioDescriptionDialog.h"
#include "core/AudioDescription.h"
#include "core/Adr.h"
#include "MaskOverlay.h"
#include "PaintOverlay.h"
#include "render/Restoration.h"
#include "TransformOverlay.h"
#include "render/ClipPlacement.h"
#include "media/ImageSequence.h"
#include "media/Diarizer.h"
#include "media/VisualSearch.h"
#include "media/Faces.h"
#include "media/DepthMap.h"
#include "media/Rife.h"
#include "media/Matting.h"
#include "media/TextToSpeech.h"
#include "media/VoiceClone.h"
#include "media/SoundSearch.h"
#include "media/StemSeparation.h"
#include "media/Inpaint.h"
#include "SpeechDialog.h"
#include "VoiceConvert.h"
#include "StemsDialog.h"
#include "GraphicsDialogs.h"
#include "ShotSearchPanel.h"
#include "PeoplePanel.h"
#include "SequenceIndexPanel.h"
#include "core/Automation.h"
#include "core/History.h"
#include "render/Processing.h"
#include "media/Segmenter.h"
#include "HdrSurface.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "Recovery.h"
#include "RenderQueue.h"
#include "RenderQueuePanel.h"
#include "render/RenderFarm.h"
#include "TimelineWidget.h"
#include "TranscribeDialog.h"
#include "TranscriptPanel.h"
#include "core/AutoTag.h"
#include "core/ClipAnimation.h"
#include "core/EditOps.h"
#include "core/GradeVersions.h"
#include "core/MergeClips.h"
#include "core/AudioChannels.h"
#include "core/Effects.h"
#include "core/Expression.h"
#include "core/Multicam.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "media/LtcTimecode.h"
#include "audio/Ltc.h"
#include "render/RoomTone.h"
#include "render/ColorSpace.h"
#include "render/ClipAnalysis.h"
#include "render/Compositor.h"
#include "render/RenderCache.h"
#include "render/BroadcastMxf.h"
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

// Three talks (cooking, football, astronomy) of eight sentences, a word every 0.4 s and 0.6 s between sentences.
static std::shared_ptr<Transcript> threeTalks(double& end) {
    const std::vector<std::vector<const char*>> talks = {
        {"Boil the pasta in plenty of salted water.", "Chop the garlic and warm the olive oil.", "Add the tomatoes and let the sauce simmer.",
         "A tomato sauce needs fresh garlic.", "Stir the sauce so the tomatoes break down.", "Drain the pasta and keep some water.",
         "Toss the pasta through the sauce.", "Finish the pasta with basil and cheese."},
        {"Now to the football match on Saturday.", "The team started slowly and the coach worried.", "Their striker missed two chances early.",
         "Our defence held until the goalkeeper slipped.", "The goal came from a corner kick.", "The coach changed the formation at half time.",
         "The new striker scored a brilliant goal.", "The coach praised the team after the match."},
        {"Finally the night sky this month.", "The planets line up after sunset.", "A small telescope shows the moons of Jupiter.",
         "Saturn and its rings shine in any telescope.", "The galaxy stretches across a dark sky.", "With no moon the stars stand out.",
         "Point the telescope at the Orion nebula.", "Enjoy the stars and planets this month."}};
    auto t = std::make_shared<Transcript>();
    t->language = "en";
    double at = 0.5;
    for (const auto& talk : talks)
        for (const char* sentence : talk) {
            TranscriptSegment seg;
            for (const QString& w : QString::fromLatin1(sentence).split(' ')) {
                seg.words.push_back({at, at + 0.3, w.toStdString(), 1});
                at += 0.4;
            }
            at += 0.6;
            t->segments.push_back(seg);
        }
    end = at;
    return t;
}

// A ColorChecker photographed under a warm cast, on a grey wall (640 x 360, the patch area at 120,80 300 x 200).
static QImage castChartImage() {
    const ChartColors& ref = colorChecker24();
    const double cast[3][3] = {{1.15, 0.08, 0.0}, {0.02, 0.95, 0.03}, {0.0, 0.05, 0.7}};
    QImage img(640, 360, QImage::Format_RGB32);
    img.fill(QColor(84, 84, 84));
    QPainter p(&img);
    p.fillRect(QRect(112, 72, 316, 216), QColor(13, 13, 13));
    for (int i = 0; i < 24; ++i) {
        double rgb[3];
        for (int c = 0; c < 3; ++c) {
            double l = 0;
            for (int k = 0; k < 3; ++k) l += cast[c][k] * std::pow(ref[size_t(i)][size_t(k)], 2.4);
            rgb[c] = std::pow(std::max(0.0, l * 0.9), 1 / 2.4);
        }
        const double cw = 50, ch = 50;
        p.fillRect(QRectF(120 + (i % 6) * cw + 4, 80 + (i / 6) * ch + 4, cw - 8, ch - 8),
                   QColor::fromRgbF(float(std::min(1.0, rgb[0])), float(std::min(1.0, rgb[1])), float(std::min(1.0, rgb[2]))));
    }
    return img;
}

// A Speed Editor as the protocol has it: it answers the challenge on feature report 6 (checking the answer) and keeps
// what it is sent.
class FakeSpeedEditor : public montage::HidDevice {
public:
    explicit FakeSpeedEditor(std::shared_ptr<std::vector<std::vector<uint8_t>>> written) : written_(std::move(written)) {}
    int authenticated = 0;
    bool write(const std::vector<uint8_t>& report) override {
        written_->push_back(report);
        return true;
    }
    bool sendFeature(const std::vector<uint8_t>& r) override {
        if (r.size() != 10 || r[0] != 6) return false;
        if (r[1] == 0) state_ = 1;
        else if (r[1] == 1 && state_ == 1) state_ = 2;
        else if (r[1] == 3 && state_ == 2) {
            uint64_t got = 0;
            for (int i = 7; i >= 0; --i) got = got << 8 | r[size_t(2 + i)];
            state_ = got == montage::speed::authResponse(kChallenge) ? 3 : 4;
            if (state_ == 3) ++authenticated;
        }
        return true;
    }
    bool getFeature(uint8_t id, std::vector<uint8_t>& out) override {
        if (id != 6) return false;
        out.assign(10, 0);
        out[0] = 6;
        if (state_ == 1) {
            for (int i = 0; i < 8; ++i) out[size_t(2 + i)] = uint8_t(kChallenge >> (8 * i));
        } else {
            out[1] = state_ == 2 ? 2 : 4;
            if (state_ == 3) out[2] = 88, out[3] = 2;  // 600 seconds
        }
        return true;
    }

private:
    static constexpr uint64_t kChallenge = 0x0123456789abcdef;
    std::shared_ptr<std::vector<std::vector<uint8_t>>> written_;
    int state_ = 0;
};

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
        // The window active again with the timeline focused, for its shortcuts: on the offscreen platform a dialog an
        // earlier test closed leaves no window active.
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
        if (!QApplication::focusWidget()) timeline()->setFocus();
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
        // Settings of their own: nothing the app saved (window layout, workspace, presets) leaks into the tests.
        for (auto format : {QSettings::NativeFormat, QSettings::IniFormat})
            QSettings::setPath(format, QSettings::UserScope, dir_.filePath(QStringLiteral("settings")));
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
            // More audio streams in a master (a MOV with sound), not in a GIF; every caption language needs two tracks.
            auto* streams = ed.findChild<QComboBox*>("exportAudioStreams");
            QVERIFY(streams && streams->isEnabled() && streams->count() == 4);  // the mix only, roles, tracks, dialogue and M&E
            QVERIFY(streams->itemText(3).contains("M&E"));
            preset->setCurrentIndex(presetIndex("gif"));
            QVERIFY(!streams->isEnabled());
            QVERIFY(!ed.findChild<QCheckBox*>("exportAllCaptions")->isEnabled());
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

    void conformCheckDialog() {
        // A shot and the offline cut's reference movie of it, reframed to 110 %: the dialog finds the reframe and
        // puts it right as one undo step.
        QVERIFY(win_->findChild<QAction*>("conformCheck"));
        const QString shot = dir_.path() + "/conform-shot.mp4", reference = dir_.path() + "/conform-reference.mp4";
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 320, gs.height = 180, gs.fps = {25, 1};
            QVERIFY(edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "bars", 25)).ok);
            Clip t = makeGeneratorClip(gen, "title", 25);
            t.generator.strings["text"] = "CONFORM 42";
            t.generator.params["size"] = 40.0;
            QVERIFY(edit::overwrite(gen, gs, {TrackKind::Video, 1}, t).ok);
            ExportSettings st;
            st.path = shot.toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        {
            Project ref = makeDefaultProject();
            Sequence& rs = *ref.active();
            rs.width = 320, rs.height = 180, rs.fps = {25, 1};
            MediaItem m;
            m.id = ref.newId();
            QVERIFY(probeMedia(shot.toStdString(), m));
            ref.media.push_back(m);
            Clip c = makeClip(ref, m, TrackKind::Video, rs);
            c.motion.params["scale"] = Param(110.0);
            QVERIFY(edit::overwrite(ref, rs, {TrackKind::Video, 0}, c).ok);
            ExportSettings st;
            st.path = reference.toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            st.burnIn.timecode = true;
            std::string err;
            QVERIFY2(exportSequence(ref, rs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({shot, reference});
        QCOMPARE(ids.size(), size_t(2));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            s.width = 320, s.height = 180, s.fps = {25, 1};
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        ConformCheckDialog dlg(state(), win_.get());
        auto* refBox = dlg.findChild<QComboBox*>("conformReference");
        QCOMPARE(refBox->count(), 2);
        QCOMPARE(refBox->currentData().toLongLong(), qint64(ids[1]));  // the one not cut in
        auto* applyAll = dlg.findChild<QPushButton*>("conformApplyAll");
        QVERIFY(!applyAll->isEnabled());
        dlg.findChild<QLineEdit*>("conformStart")->setText("not a time");
        QVERIFY(!dlg.runCheck());
        dlg.setReferenceStart(0);
        QVERIFY(dlg.runCheck());
        QCOMPARE(dlg.shots().size(), size_t(1));
        QCOMPARE(dlg.shots()[0].verdict, ConformVerdict::Reframed);
        auto* table = dlg.findChild<QTableWidget*>("conformShots");
        QCOMPARE(table->item(0, 2)->text(), QString("Reframed"));
        QVERIFY(applyAll->isEnabled());
        QVERIFY(dlg.findChild<QPushButton*>("conformMarkers")->isEnabled());
        QCOMPARE(dlg.applyFixes(true), 1);
        const Clip& c = state()->sequence()->videoTracks[0].clips[0];
        QVERIFY2(std::fabs(c.motion.p("scale", 0, 100) - 110) < 0.5, std::to_string(c.motion.p("scale", 0, 100)).c_str());
        QVERIFY(table->item(0, 2)->text().startsWith("Fixed"));
        QVERIFY(!applyAll->isEnabled());
        QVERIFY(!dlg.findChild<QPushButton*>("conformMarkers")->isEnabled());
        QVERIFY(dlg.runCheck());
        QCOMPARE(dlg.shots()[0].verdict, ConformVerdict::Matches);
        QVERIFY(dlg.findChild<QLabel*>("conformSummary")->text().contains("match"));
        state()->undo();
        QCOMPARE(state()->sequence()->videoTracks[0].clips[0].motion.p("scale", 0, 100), 100.0);
        // The undo leaves the results stale: dropped, to be checked again. Markers go on once for a check.
        QVERIFY(dlg.shots().empty());
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(dlg.runCheck());
        QCOMPARE(dlg.shots()[0].verdict, ConformVerdict::Reframed);
        QCOMPARE(dlg.addMarkers(), 1);
        QCOMPARE(dlg.addMarkers(), 0);
        QVERIFY(!dlg.findChild<QPushButton*>("conformMarkers")->isEnabled());
        QCOMPARE(dlg.shots().size(), size_t(1));  // (its own markers do not make the results stale)
    }

    void aleAndCdlFromTheMenus() {
        state()->newProject();
        Id media = 0;
        QVERIFY(state()->edit("Media", [&](Project& p, Sequence& s) {
            MediaItem m;
            m.id = media = p.newId();
            m.kind = MediaKind::Video;
            m.name = "A001C005.mov";
            m.path = "/cards/A001C005.mov";
            m.duration = 8;
            m.width = 1920;
            m.height = 1080;
            m.fps = s.fps;
            m.hasVideo = true;
            m.timecode = 3600;
            p.media.push_back(m);
            return edit::placeMedia(p, s, m.id, 0, 0, 60, {TrackKind::Video, 0}, {}, false).ok;
        }));
        for (const char* name : {"importAle", "exportAle", "importCdl", "exportCdl"}) QVERIFY(win_->findChild<QAction*>(name));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        auto clipGrade = [&](Cdl& out) { return clipCdl(*edit::clipById(*state()->sequence(), clip), 0, out); };
        // An ALE: its log on the media and its CDL on the clip, as one undo step.
        const QString ale = dir_.path() + "/dailies.ale";
        {
            QFile f(ale);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("Heading\nFIELD_DELIM\tTABS\nFPS\t25\n\nColumn\nName\tScene\tTake\tASC_SOP\tASC_SAT\n\nData\n"
                    "A001C005\t7B\t1\t(0.9 1.0 1.1)(0.02 0.0 -0.02)(1.0 1.0 1.0)\t1.1\n");
        }
        QCOMPARE(win_->importAle(ale), 1);
        QCOMPARE(state()->project().findMedia(media)->metadata.at("scene"), std::string("7B"));
        Cdl g;
        QVERIFY(clipGrade(g) && g.slope[0] == 0.9 && g.saturation == 1.1);
        state()->undo();
        QVERIFY(!state()->project().findMedia(media)->metadata.count("scene"));
        QVERIFY(!clipGrade(g));
        QCOMPARE(win_->importAle(dir_.path() + "/missing.ale"), -1);
        // A .cc onto the selected clip; out again as a collection; the bin's media as an ALE.
        const QString cc = dir_.path() + "/look.cc";
        {
            Cdl look;
            look.id = "look";
            look.slope[1] = 1.3;
            QFile f(cc);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(writeCdlXml({look}, CdlFormat::Cc)));
        }
        QCOMPARE(win_->importCdl(cc), 0);  // nothing selected
        state()->setSelection({clip});
        QCOMPARE(win_->importCdl(cc), 1);
        QVERIFY(clipGrade(g) && g.slope[1] == 1.3);
        const QString ccc = dir_.path() + "/out.ccc";
        QVERIFY(win_->exportCdl(ccc));
        QFile out(ccc);
        QVERIFY(out.open(QIODevice::ReadOnly));
        const std::vector<Cdl> again = parseCdlXml(out.readAll().toStdString());
        QVERIFY(again.size() == 1 && again[0].sameGrade(g));
        state()->setSelection({});
        const QString aleOut = dir_.path() + "/out.ale";
        QVERIFY(win_->exportAle(aleOut));
        QFile af(aleOut);
        QVERIFY(af.open(QIODevice::ReadOnly));
        const QString aleText = QString::fromUtf8(af.readAll());
        QVERIFY2(aleText.contains("A001C005.mov") && aleText.contains("ASC_SOP") && aleText.contains("01:00:00:00"), qPrintable(aleText));
    }

    void amfFromTheMenus() {
        state()->newProject();
        QVERIFY(state()->edit("Media", [&](Project& p, Sequence& s) {
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Video;
            m.name = "A001C012.mov";
            m.path = "/cards/A001C012.mov";
            m.duration = 8;
            m.width = 1920;
            m.height = 1080;
            m.fps = s.fps;
            m.hasVideo = true;
            p.media.push_back(m);
            return edit::placeMedia(p, s, m.id, 0, 0, 60, {TrackKind::Video, 0}, {}, false).ok;
        }));
        for (const char* name : {"importAmf", "exportAmf"}) QVERIFY(win_->findChild<QAction*>(name));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        auto theClip = [&] { return edit::clipById(*state()->sequence(), clip); };
        const QString data = QStringLiteral(MONTAGE_TEST_DATA_DIR "/amf");
        // Nothing selected: the clip its clip name matches, as one undo step; the sequence's space kept when asked.
        state()->setSelection({});
        QCOMPARE(win_->importAmf(data + "/A001C012.amf", 0), 1);
        QCOMPARE(theClip()->effects.size(), size_t(3));
        QCOMPARE(state()->project().findMedia(theClip()->mediaId)->colorOverride, std::string("logc3-awg3"));
        QVERIFY(win_->statusBar()->currentMessage().contains("Bleach"));  // the look it could not carry
        state()->undo();
        QVERIFY(theClip()->effects.empty());
        // Another onto the selected clip, taking its output transform's space.
        state()->setSelection({clip});
        QCOMPARE(win_->importAmf(data + "/B002C003.amf", 1), 1);
        QCOMPARE(state()->sequence()->colorSpace, std::string("rec2100pq"));
        QCOMPARE(win_->importAmf(data + "/missing.amf", 0), 0);
        // Out again for the selected clip; without a selection, a hint.
        const QString out = dir_.path() + "/clip.amf";
        QCOMPARE(win_->exportAmf(out), 1);
        AmfDocument back;
        QVERIFY(readAmf(out.toStdString(), back));
        QCOMPARE(back.outputSpace, std::string("rec2100pq"));
        QCOMPARE(back.looks.size(), size_t(1));
        state()->setSelection({});
        win_->findChild<QAction*>("exportAmf")->trigger();
        QTRY_VERIFY(win_->statusBar()->currentMessage().contains("Select the clips"));
    }

    void controlSurfaceMackie() {
        // A stand-in for the surface's MIDI connection: what Montage sends it is kept.
        struct Recorder : MidiConnection {
            std::shared_ptr<std::vector<MidiMessage>> sent = std::make_shared<std::vector<MidiMessage>>();
            bool send(const MidiMessage& m) override {
                sent->push_back(m);
                return true;
            }
            std::string inputName() const override { return "X-Touch"; }
            std::string outputName() const override { return "X-Touch"; }
        };
        state()->newProject();
        QVERIFY(state()->edit("Tracks", [](Project& p, Sequence& s) {
            while (s.audioTracks.size() < 10) edit::addTrack(p, s, TrackKind::Audio);
            for (size_t i = 0; i < s.audioTracks.size(); ++i) s.audioTracks[i].name = "Trk " + std::to_string(i + 1);
            s.audioTracks[0].name = "Dialogue A";
            return true;
        }));
        auto* mixer = win_->findChild<MixerPanel*>();
        QTRY_COMPARE(mixer->trackStrips(), 10);
        ControlSurface* surface = win_->controlSurface();
        QVERIFY(surface && !surface->isConnected());
        QVERIFY(win_->findChild<QAction*>("controlSurface"));
        auto rec = std::make_unique<Recorder>();
        auto sent = rec->sent;
        QSignalSpy connected(surface, &ControlSurface::connectionChanged);
        surface->setConnection(std::move(rec));
        QVERIFY(surface->isConnected() && connected.size() == 1);
        QCOMPARE(surface->surfaceName(), QString("X-Touch"));
        auto has = [&](const MidiMessage& m) { return std::find(sent->begin(), sent->end(), m) != sent->end(); };
        // On connecting: asked if it is there, then shown everything: names, levels, faders, the timecode.
        QCOMPARE(sent->front(), mcu::deviceQuery());
        QVERIFY(has(mcu::lcd(0, "Dialog ")));
        QVERIFY(has(mcu::lcd(7, "Trk 2  ")));
        QVERIFY(has(mcu::lcd(mcu::kLcdWidth, " 0.0   ")));
        QVERIFY(has(mcu::fader(0, mcu::faderValue(0))));
        QVERIFY(has(mcu::led(mcu::Stop, 2)) && has(mcu::meterMode(0, 3)));
        QVERIFY(has(mcu::timecode("00:00:00:00")[0]));
        QVERIFY(has(mcu::assignment("1")[1]));
        auto track = [&](int i) -> const Track& { return state()->sequence()->audioTracks.at(size_t(i)); };

        // A touched fader moving: the track's volume, the position echoed back so the servo holds it, one undo step.
        sent->clear();
        surface->handle({0x90, 0x68, 0x7F});
        QVERIFY(mixer->trackFader(0)->isSliderDown());
        const int minus6 = mcu::faderValue(-6);
        surface->handle(mcu::fader(0, mcu::faderValue(-3)));
        surface->handle(mcu::fader(0, minus6));
        QCOMPARE(track(0).volumeDb, -6.0);
        QVERIFY(has(mcu::fader(0, minus6)));
        surface->handle({0x90, 0x68, 0x00});
        QVERIFY(!mixer->trackFader(0)->isSliderDown());
        state()->undo();
        QCOMPARE(track(0).volumeDb, 0.0);
        // The motor follows the mixer: the fader goes back up.
        sent->clear();
        surface->refresh();
        QVERIFY(has(mcu::fader(0, mcu::faderValue(0))));
        // A surface without touch sensing: a move holds the fader until it rests.
        surface->handle(mcu::fader(1, minus6));
        QCOMPARE(track(1).volumeDb, -6.0);
        QVERIFY(mixer->trackFader(1)->isSliderDown());
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(1)->isSliderDown(), 3000);
        // The master fader.
        surface->handle(mcu::fader(mcu::kMasterChannel, mcu::faderValue(-10)));
        QCOMPARE(state()->sequence()->masterVolumeDb, -10.0);

        // Mute and solo light up; the V-Pot pans (shown on the strip for a moment) and its push centres it.
        sent->clear();
        surface->handle({0x90, uint8_t(mcu::Mute + 2), 0x7F});
        surface->handle({0x90, uint8_t(mcu::Mute + 2), 0x00});
        QVERIFY(track(2).muted);
        QVERIFY(has(mcu::led(mcu::Mute + 2, 2)));
        surface->handle({0x90, uint8_t(mcu::Solo + 3), 0x7F});
        QVERIFY(track(3).solo && has(mcu::led(mcu::Solo + 3, 2)));
        surface->handle({0xB0, 0x10, 0x05});
        QCOMPARE(track(0).pan, 0.1);
        QVERIFY(has(mcu::ring(0, mcu::panRing(0.1))));
        QVERIFY(has(mcu::lcd(mcu::kLcdWidth, " R10   ")));
        surface->handle({0xB0, 0x10, 0x43});
        QVERIFY(std::fabs(track(0).pan - 0.04) < 1e-9);
        surface->handle({0x90, uint8_t(mcu::VPotPush), 0x7F});
        QCOMPARE(track(0).pan, 0.0);

        // Banking: the strips move to the last eight tracks, the faders and names with them.
        sent->clear();
        surface->handle({0x90, uint8_t(mcu::BankRight), 0x7F});
        QCOMPARE(surface->bank(), 2);
        QVERIFY(has(mcu::lcd(0, "Trk 3  ")) && has(mcu::assignment("3")[1]));
        surface->handle(mcu::fader(0, minus6));
        QCOMPARE(track(2).volumeDb, -6.0);
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(2)->isSliderDown(), 3000);
        surface->handle({0x90, uint8_t(mcu::ChannelLeft), 0x7F});
        QCOMPARE(surface->bank(), 1);
        surface->handle({0x90, uint8_t(mcu::BankLeft), 0x7F});
        QCOMPARE(surface->bank(), 0);
        // A level changed in Montage moves the motor.
        QVERIFY(state()->edit("Level", [](Project&, Sequence& s) {
            s.audioTracks[4].volumeDb = -12;
            return true;
        }));
        sent->clear();
        surface->refresh();
        QVERIFY(has(mcu::fader(4, mcu::faderValue(-12))));

        // Automation: select a strip, Touch on it, and a pass written while the fader is held.
        surface->handle({0x90, uint8_t(mcu::Select + 5), 0x7F});
        QCOMPARE(surface->selectedTrack(), 5);
        surface->handle({0x90, uint8_t(mcu::Touch), 0x7F});
        QCOMPARE(track(5).automation, int(AutomationMode::Touch));
        QVERIFY(has(mcu::led(mcu::Touch, 2)));
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 40; ++f) {
            if (f == 10) surface->handle({0x90, 0x6D, 0x7F});
            if (f == 10) surface->handle(mcu::fader(5, mcu::faderValue(-20)));
            if (f == 30) surface->handle({0x90, 0x6D, 0x00});
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(40);
        QVERIFY(track(5).volumeAuto.animated());
        QCOMPARE(track(5).volumeAuto.at(20), -20.0);
        QCOMPARE(track(5).volumeAuto.at(5), 0.0);

        // Transport, jog and markers.
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        state()->setPlayhead(10);
        surface->handle({0xB0, 0x3C, 0x03});
        QTRY_COMPARE(state()->playhead(), FrameTime(13));
        surface->handle({0xB0, 0x3C, 0x41});
        QTRY_COMPARE(state()->playhead(), FrameTime(12));
        const size_t markers = state()->sequence()->markers.size();
        surface->handle({0x90, uint8_t(mcu::Marker), 0x7F});
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);
        surface->handle({0x90, uint8_t(mcu::Undo), 0x7F});
        QCOMPARE(state()->sequence()->markers.size(), markers);
        sent->clear();
        surface->handle({0x90, uint8_t(mcu::Play), 0x7F});
        QVERIFY(program->isPlaying());
        QVERIFY(has(mcu::led(mcu::Play, 2)) && has(mcu::led(mcu::Stop, 0)));
        surface->handle({0x90, uint8_t(mcu::Stop), 0x7F});
        QVERIFY(!program->isPlaying());
        // Play while shuttling plays at normal speed.
        program->shuttle(1);
        program->shuttle(1);
        QVERIFY(program->speed() > 1);
        surface->handle({0x90, uint8_t(mcu::Play), 0x7F});
        QCOMPARE(program->speed(), 1.0);
        surface->handle({0x90, uint8_t(mcu::Stop), 0x7F});
        QVERIFY(!program->isPlaying());
        // Meters follow playback levels.
        sent->clear();
        surface->setLevels(0, 0, QVector<float>{0.5f, 0.25f, 1.0f, 1.0f});
        QVERIFY(has(mcu::meter(0, 8)) && has(mcu::meter(1, 12)));

        // Moves from the surface are one undo step together, two faders moving at once included.
        QVERIFY(state()->edit("Level", [](Project&, Sequence& s) {
            s.audioTracks[4].volumeDb = 0;
            return true;
        }));
        surface->handle(mcu::fader(6, minus6));
        surface->handle(mcu::fader(7, minus6));
        surface->handle(mcu::fader(6, mcu::faderValue(-3)));
        QCOMPARE(track(6).volumeDb, -3.0);
        QCOMPARE(track(7).volumeDb, -6.0);
        state()->undo();
        QCOMPARE(track(6).volumeDb, 0.0);
        QCOMPARE(track(7).volumeDb, 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(6)->isSliderDown() && !mixer->trackFader(7)->isSliderDown(), 3000);
        // A fader's moves that queued up together are applied once, at the last position.
        sent->clear();
        surface->receive(mcu::fader(6, mcu::faderValue(-20)));
        surface->receive(mcu::fader(6, mcu::faderValue(-10)));
        QTRY_COMPARE(track(6).volumeDb, -10.0);
        QVERIFY(has(mcu::fader(6, mcu::faderValue(-10))) && !has(mcu::fader(6, mcu::faderValue(-20))));
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(6)->isSliderDown(), 3000);
        // Banking while a fader is touched: the fader holds its new track, and its motor stays still.
        surface->handle({0x90, 0x68, 0x7F});
        sent->clear();
        surface->handle({0x90, uint8_t(mcu::BankRight), 0x7F});
        QCOMPARE(surface->bank(), 2);
        QVERIFY(!mixer->trackFader(0)->isSliderDown() && mixer->trackFader(2)->isSliderDown());
        QVERIFY(std::none_of(sent->begin(), sent->end(), [](const MidiMessage& m) { return m[0] == 0xE0; }));
        surface->handle(mcu::fader(0, mcu::faderValue(-12)));
        QCOMPARE(track(2).volumeDb, -12.0);
        surface->handle({0x90, 0x68, 0x00});
        QVERIFY(!mixer->trackFader(2)->isSliderDown());
        surface->handle({0x90, uint8_t(mcu::BankLeft), 0x7F});
        QCOMPARE(surface->bank(), 0);

        // A surface switched on later says so: everything is sent again.
        sent->clear();
        surface->handle({0xF0, 0x00, 0x00, 0x66, 0x14, 0x01, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0xF7});
        QVERIFY(has(mcu::lcd(0, "Dialog ")));
        // Disconnecting leaves it clean.
        sent->clear();
        surface->disconnectSurface();
        QVERIFY(!surface->isConnected() && connected.size() == 2);
        QVERIFY(has(mcu::fader(0, 0)) && has(mcu::led(mcu::Mute + 2, 0)) && has(mcu::lcd(0, std::string(2 * mcu::kLcdWidth, ' '))));

        // The settings dialog lists the ports and opens once.
        QDialog* dlg = win_->controlSurfaceDialog();
        QVERIFY(dlg && dlg->findChild<QComboBox*>("surfaceInput") && dlg->findChild<QComboBox*>("surfaceOutput"));
        QVERIFY(dlg->findChild<QLabel*>("surfaceStatus")->text().contains("Not connected"));
        QCOMPARE(win_->controlSurfaceDialog(), dlg);
        dlg->close();

        // The device going away (unplugged mid-move): what it held lets go and it is closed.
        surface->setConnection(std::make_unique<Recorder>());
        QVERIFY(surface->isConnected() && connected.size() == 3);
        surface->handle({0x90, 0x69, 0x7F});
        QVERIFY(mixer->trackFader(1)->isSliderDown());
        surface->connectionLost();
        QTRY_VERIFY(!surface->isConnected());
        QCOMPARE(connected.size(), 4);
        QVERIFY(!mixer->trackFader(1)->isSliderDown());
        // A loss reported by a connection since replaced is ignored.
        surface->setConnection(std::make_unique<Recorder>());
        surface->connectionLost();
        surface->setConnection(std::make_unique<Recorder>());
        QTest::qWait(50);
        QVERIFY(surface->isConnected());
        surface->disconnectSurface();
    }

    void oscControl() {
        state()->newProject();
        QVERIFY(state()->edit("Tracks", [](Project& p, Sequence& s) {
            while (s.audioTracks.size() < 2) edit::addTrack(p, s, TrackKind::Audio);
            edit::overwrite(p, s, V1, makeGeneratorClip(p, "color", 250));
            return true;
        }));
        auto* mixer = win_->findChild<MixerPanel*>();
        QTRY_VERIFY(mixer->trackFader(1));
        OscControl* osc = win_->oscControl();
        QVERIFY(osc);
        QString err;
        QVERIFY2(osc->start(0, false, &err), qPrintable(err));
        QVERIFY(osc->isRunning() && osc->port() > 0);
        // A sender (as Companion's generic OSC connection) that also listens for feedback.
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        osc->setFeedbackPort(sender.localPort());
        auto sendOsc = [&](const osc::Message& m) {
            const std::string bytes = osc::encode(m);
            sender.writeDatagram(bytes.data(), qint64(bytes.size()), QHostAddress::LocalHost, osc->port());
        };
        std::vector<osc::Message> heard;
        auto received = [&](const std::string& address) {
            while (sender.hasPendingDatagrams()) {
                QByteArray d(int(sender.pendingDatagramSize()), '\0');
                sender.readDatagram(d.data(), d.size());
                osc::parse(std::string(d.constData(), size_t(d.size())), heard);
            }
            for (auto it = heard.rbegin(); it != heard.rend(); ++it)  // (the latest)
                if (it->address == address) return static_cast<const osc::Message*>(&*it);
            return static_cast<const osc::Message*>(nullptr);
        };
        // Everything waiting read and forgotten (so a check sees only what comes after).
        auto drain = [&] {
            QTest::qWait(30);
            received("");
            heard.clear();
        };
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        const Rational fps = state()->sequence()->fps;
        // Over the network socket: go to a timecode, play (a button's release, 0, does nothing), stop; feedback back.
        sendOsc({"/montage/goto", {osc::string("00:00:02:00")}});
        QTRY_COMPARE(state()->playhead(), FrameTime(2 * fps.num / fps.den));
        QTRY_VERIFY(received("/montage/state/timecode"));
        sendOsc({"/montage/play", {osc::int32(0)}});
        QTest::qWait(100);
        QVERIFY(!program->isPlaying());
        sendOsc({"/montage/play", {osc::int32(1)}});
        QTRY_VERIFY(program->isPlaying());
        QTRY_VERIFY(received("/montage/state/playing") && received("/montage/state/playing")->args.at(0).i == 1);
        sendOsc({"/montage/stop", {}});
        QTRY_VERIFY(!program->isPlaying());
        // TouchOSC's buttons send 1.0 then 0.0: one toggle, not two.
        QVERIFY(osc->handle({"/montage/playpause", {osc::float32(1.0f)}}) && program->isPlaying());
        QVERIFY(osc->handle({"/montage/playpause", {osc::float32(0.0f)}}) && program->isPlaying());
        QVERIFY(osc->handle({"/montage/stop", {}}) && !program->isPlaying());
        drain();
        sendOsc({"/montage/nowhere", {}});
        QTRY_VERIFY(received("/montage/error"));
        // Its own feedback addresses are never answered (another Montage's, or its own sent back).
        drain();
        QVERIFY(osc->handle({"/montage/state/playing", {osc::int32(1)}}) && osc->handle({"/montage/error", {osc::string("x")}}));
        QTest::qWait(50);
        QVERIFY(!received("/montage/error"));
        // A new feedback port gets the whole state at once, without waiting for it to change.
        QUdpSocket other;
        QVERIFY(other.bind(QHostAddress::LocalHost, 0));
        osc->setFeedbackPort(other.localPort());
        QTRY_VERIFY(other.hasPendingDatagrams());
        std::vector<osc::Message> got;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            while (other.hasPendingDatagrams()) {
                QByteArray d(int(other.pendingDatagramSize()), '\0');
                other.readDatagram(d.data(), d.size());
                osc::parse(std::string(d.constData(), size_t(d.size())), got);
            }
            int states = 0;
            for (const osc::Message& m : got) states += m.address.rfind("/montage/state/", 0) == 0;
            return states >= 3;
        }(), 2000);
        osc->setFeedbackPort(sender.localPort());
        // A feedback port pointed back at the port it listens on: nothing answers itself (the window stays free).
        QSignalSpy handled(osc, &OscControl::handled);
        osc->setFeedbackPort(osc->port());
        sendOsc({"/montage/nowhere", {}});
        QTest::qWait(300);
        QVERIFY(handled.size() < 5);
        osc->setFeedbackPort(sender.localPort());
        // Seconds, steps, start and end, In and Out, loop on and off.
        QVERIFY(osc->handle({"/montage/goto", {osc::float32(1.0f)}}));
        QCOMPARE(state()->playhead(), FrameTime(fps.num / fps.den));
        QVERIFY(osc->handle({"/montage/step", {osc::int32(3)}}));
        QTRY_COMPARE(state()->playhead(), FrameTime(fps.num / fps.den + 3));
        QVERIFY(osc->handle({"/montage/in", {}}));
        QCOMPARE(state()->sequence()->inPoint, FrameTime(fps.num / fps.den + 3));
        QVERIFY(osc->handle({"/montage/end", {}}));
        QTRY_COMPARE(state()->playhead(), state()->sequence()->duration());
        QVERIFY(osc->handle({"/montage/start", {}}));
        QTRY_COMPARE(state()->playhead(), FrameTime(0));
        QVERIFY(osc->handle({"/montage/loop", {osc::int32(1)}}) && program->loop());
        QVERIFY(osc->handle({"/montage/loop", {osc::int32(0)}}) && !program->loop());
        QVERIFY(!osc->handle({"/montage/goto", {osc::string("soon")}}, QHostAddress::LocalHost, &err) && err.contains("timecode"));
        // Markers: named, and the menu command by its address; undo.
        const size_t markers = state()->sequence()->markers.size();
        QVERIFY(osc->handle({"/montage/marker", {osc::string("Cue")}}));
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);
        QCOMPARE(QString::fromStdString(state()->sequence()->markers.back().name), QString("Cue"));
        QString addMarker;
        for (const auto& [address, id] : osc->commandAddresses())
            if (id == "Sequence/Add Marker") addMarker = address;
        QCOMPARE(addMarker, QString("/montage/command/sequence/add_marker"));
        state()->setPlayhead(30);
        QVERIFY(osc->handle({addMarker.toStdString(), {}}));
        QCOMPARE(state()->sequence()->markers.size(), markers + 2);
        QVERIFY(osc->handle({addMarker.toStdString(), {osc::int32(0)}}));  // (a release)
        QCOMPARE(state()->sequence()->markers.size(), markers + 2);
        QVERIFY(osc->handle({"/montage/undo", {}}));
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);
        QVERIFY(!osc->handle({"/montage/command/sequence/no_such_thing", {}}, QHostAddress::LocalHost, &err) && err.contains("No menu command"));
        // The mixer: a track's volume (one undo step), mute set and toggled, solo, the master.
        auto track = [&](int i) -> const Track& { return state()->sequence()->audioTracks.at(size_t(i)); };
        const int steps = int(state()->history().undoCount());
        QVERIFY(osc->handle({"/montage/track/2/volume", {osc::float32(-6)}}));
        QCOMPARE(track(1).volumeDb, -6.0);
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        // Held a moment as a hand holds it (so Touch and Latch passes write the move), then let go.
        QVERIFY(mixer->trackFader(1)->isSliderDown());
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(1)->isSliderDown(), 3000);
        QVERIFY(osc->handle({"/montage/track/1/mute", {osc::int32(1)}}) && track(0).muted);
        QVERIFY(osc->handle({"/montage/track/1/mute", {osc::int32(1)}}) && track(0).muted);  // (set, not toggled)
        QVERIFY(osc->handle({"/montage/track/1/mute", {}}) && !track(0).muted);            // (no value: toggled)
        QVERIFY(osc->handle({"/montage/track/2/solo", {osc::boolean(true)}}) && track(1).solo);
        QVERIFY(osc->handle({"/montage/master/volume", {osc::int32(-3)}}));
        QCOMPARE(state()->sequence()->masterVolumeDb, -3.0);
        QVERIFY(!osc->handle({"/montage/track/9/volume", {osc::int32(0)}}, QHostAddress::LocalHost, &err) && err.contains("track"));
        QVERIFY(!osc->handle({"/montage/track/1/volume", {osc::string("loud")}}, QHostAddress::LocalHost, &err));
        // Agent tools: off until allowed; then on the open project only, one undo step named after each, the answer sent back.
        QVERIFY(!osc->handle({"/montage/tool/add_marker", {osc::string(R"({"at": 4})")}}, QHostAddress::LocalHost, &err) && err.contains("off"));
        osc->setToolsAllowed(true);
        QVERIFY(!osc->handle({"/montage/tool/add_marker", {osc::string(R"({"at": 4, "project": "/elsewhere.montage"})")}}, QHostAddress::LocalHost, &err) &&
                err.contains("open project"));
        QVERIFY(!osc->handle({"/montage/tool/undo", {}}, QHostAddress::LocalHost, &err) && err.contains("/montage/undo"));
        drain();
        sendOsc({"/montage/tool/add_marker", {osc::string(R"({"at": 4, "name": "From OSC"})")}});
        QTRY_VERIFY(std::any_of(state()->sequence()->markers.begin(), state()->sequence()->markers.end(),
                                [](const Marker& m) { return m.name == "From OSC"; }));
        QCOMPARE(QString::fromStdString(state()->history().undoLabel()), QString("OSC: add_marker"));
        QTRY_VERIFY(received("/montage/reply/add_marker"));
        QVERIFY(!osc->handle({"/montage/tool/add_marker", {osc::string("not json")}}, QHostAddress::LocalHost, &err) && err.contains("JSON"));
        drain();
        sendOsc({"/montage/tool/add_marker", {osc::string(R"({"at": "never"})")}});
        QTRY_VERIFY(received("/montage/error"));
        QVERIFY2(QString::fromStdString(received("/montage/error")->args.at(0).s).contains("at"), received("/montage/error")->args.at(0).s.c_str());
        osc->setToolsAllowed(false);
        // A malformed packet is answered with an error and nothing done.
        drain();
        sender.writeDatagram("bad!", 4, QHostAddress::LocalHost, osc->port());
        QTRY_VERIFY(received("/montage/error"));
        QVERIFY(QString::fromStdString(received("/montage/error")->args.at(0).s).contains("address"));  // ("bad!" is a word, but no address)
        // The settings: in the Control Surface dialog, with every address listed.
        QDialog* dlg = win_->controlSurfaceDialog();
        QVERIFY(dlg->findChild<QCheckBox*>("oscEnabled")->isChecked());
        QVERIFY(dlg->findChild<QCheckBox*>("oscTools") && !dlg->findChild<QCheckBox*>("oscTools")->isChecked());
        QVERIFY(dlg->findChild<QLabel*>("oscStatus")->text().contains(QString::number(osc->port())));
        dlg->findChild<QPushButton*>("oscAddresses")->click();
        auto* list = dlg->findChild<QDialog*>("oscAddressList");
        QVERIFY(list && list->findChild<QPlainTextEdit*>()->toPlainText().contains("/montage/command/sequence/add_marker"));
        dlg->close();
        osc->stop();
        osc->setFeedbackPort(0);
        QVERIFY(!osc->isRunning());
    }

    void speedEditor() {
        using speed::Key;
        state()->newProject();
        // Three 50-frame shots on V1 (edits at 50 and 100) and sound on A1 over the first 60 frames.
        QVERIFY(state()->edit("Shots", [](Project& p, Sequence& s) {
            const char* names[] = {"A", "B", "C"};
            for (int i = 0; i < 3; ++i) {
                Clip c = makeGeneratorClip(p, "color", 50);
                c.name = names[i];
                c.start = 50 * i;
                c.sourceIn = 20;
                edit::overwrite(p, s, V1, c);
            }
            return true;
        }));
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id jfk = ids[0];
        QVERIFY(state()->apply("Sound", [jfk](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, jfk, 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false, false, true);
        }));
        auto clip = [&](const char* name) { return clipNamed(*state()->sequence(), name); };
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        SpeedEditorControl* se = win_->speedEditor();
        QVERIFY(se);
        se->setEnabled(false);  // (no looking for real devices)

        // Every key's command is one the menus have.
        for (Key k : speed::allKeys())
            for (const QString& what : {SpeedEditorControl::defaultBinding(k), se->secondBinding(k)})
                if (!what.isEmpty() && !what.startsWith("speed:")) QVERIFY2(keymap::find(win_.get(), what), qPrintable(what));

        // Plugged in: authenticated, the wheel set to jog and its light on.
        auto written = std::make_shared<std::vector<std::vector<uint8_t>>>();
        auto* fake = new FakeSpeedEditor(written);
        HidDeviceInfo info;
        info.vendor = speed::kVendor;
        info.product = speed::kSpeedEditor;
        QString err;
        QVERIFY2(se->attach(std::unique_ptr<HidDevice>(fake), info, &err), qPrintable(err));
        QVERIFY(se->isConnected());
        QCOMPARE(fake->authenticated, 1);
        QVERIFY(se->deviceName().contains("Speed Editor"));
        auto sent = [&](const std::vector<uint8_t>& r) { return std::find(written->begin(), written->end(), r) != written->end(); };
        QVERIFY(sent(speed::jogModeReport(speed::JogMode::Relative2)));
        QCOMPARE(se->jogLeds(), speed::jogLedBit(Key::Jog));
        QVERIFY(sent(speed::jogLedReport(speed::jogLedBit(Key::Jog))));
        // Snapping is on, so SNAP is lit; the edit nearest the playhead (0) has no transition, so CUT is.
        QVERIFY(se->leds() & speed::ledBit(Key::Snap));
        QVERIFY(se->leds() & speed::ledBit(Key::Cut));
        QVERIFY(se->renew());
        QCOMPARE(fake->authenticated, 2);

        auto keys = [](std::initializer_list<Key> held) {
            std::vector<uint8_t> r(13, 0);
            r[0] = 4;
            size_t i = 0;
            for (Key k : held) {
                r[1 + i * 2] = uint8_t(k);
                r[2 + i * 2] = uint8_t(uint16_t(k) >> 8);
                ++i;
            }
            return r;
        };
        auto press = [&](Key k) {
            se->feed(keys({k}));
            se->feed(keys({}));
        };
        auto wheel = [&](speed::JogMode mode, int32_t v) {
            se->feed({3, uint8_t(mode), uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24), 0});
        };
        auto apart = [] { QTest::qWait(450); };  // (pressed again later: not a double press)

        // IN at the playhead; IN twice clears it.
        state()->setPlayhead(10);
        program->seek(10);
        press(Key::In);
        QCOMPARE(state()->sequence()->inPoint, FrameTime(10));
        apart();
        press(Key::In);
        press(Key::In);
        QCOMPARE(state()->sequence()->inPoint, FrameTime(-1));

        // The wheel: a frame a step (360), backwards too; SCRL a quarter second a step.
        wheel(speed::JogMode::Relative2, 720);
        QTRY_COMPARE(state()->playhead(), FrameTime(12));
        wheel(speed::JogMode::Relative2, -200);
        wheel(speed::JogMode::Relative2, -200);  // (steps add up)
        QTRY_COMPARE(state()->playhead(), FrameTime(11));
        wheel(speed::JogMode::Absolute, 3000);  // (absolute values are the shuttle's: ignored)
        QCOMPARE(state()->playhead(), FrameTime(11));
        press(Key::Scroll);
        QCOMPARE(se->jogLeds(), speed::jogLedBit(Key::Scroll));
        const double fps = state()->sequence()->fpsValue();
        wheel(speed::JogMode::Relative2, 360);
        QTRY_COMPARE(state()->playhead(), FrameTime(11 + std::lround(fps / 4)));
        // SHTL: by how far it is turned, either way; back to the middle stops.
        press(Key::Shuttle);
        QVERIFY(sent(speed::jogModeReport(speed::JogMode::AbsoluteDeadZero)));
        QCOMPARE(SpeedEditorControl::shuttleSpeed(30), 0.0);
        QCOMPARE(SpeedEditorControl::shuttleSpeed(1100), 1.0);
        QCOMPARE(SpeedEditorControl::shuttleSpeed(-4096), -32.0);
        wheel(speed::JogMode::AbsoluteDeadZero, 1100);
        QVERIFY(program->isPlaying());
        QCOMPARE(program->speed(), 1.0);
        wheel(speed::JogMode::AbsoluteDeadZero, -3000);
        QCOMPARE(program->speed(), -8.0);
        wheel(speed::JogMode::AbsoluteDeadZero, 0);
        QVERIFY(!program->isPlaying());
        wheel(speed::JogMode::AbsoluteDeadZero, 1100);
        QVERIFY(program->isPlaying());
        // Another monitor picked: the one shuttling stops (and the wheel shuttles the one picked).
        press(Key::Source);
        QVERIFY(!program->isPlaying());
        press(Key::Timeline);
        wheel(speed::JogMode::AbsoluteDeadZero, 1100);
        QVERIFY(program->isPlaying());
        // Stopped some other way, STOP/PLAY plays at once.
        program->pause();
        apart();
        press(Key::StopPlay);
        QVERIFY(program->isPlaying());
        apart();
        press(Key::StopPlay);
        QVERIFY(!program->isPlaying());
        wheel(speed::JogMode::AbsoluteDeadZero, 2000);
        QVERIFY(program->isPlaying());
        press(Key::Jog);  // leaving the shuttle stops it
        QVERIFY(!program->isPlaying());
        QCOMPARE(se->jogLeds(), speed::jogLedBit(Key::Jog));
        // STOP/PLAY.
        press(Key::StopPlay);
        QVERIFY(program->isPlaying());
        apart();
        press(Key::StopPlay);
        QVERIFY(!program->isPlaying());

        // TRIM IN: the edit at 50, its incoming side, trimmed by the wheel; ESC ends it.
        state()->setPlayhead(48);
        program->seek(48);
        press(Key::TrimIn);
        QVERIFY(win_->trimEdit());
        QCOMPARE(win_->trimEdit()->side, 2);
        wheel(speed::JogMode::Relative2, 360);
        QCOMPARE(clip("B")->duration, FrameTime(49));
        press(Key::Escape);
        QVERIFY(!win_->trimEdit());
        state()->undo();
        QCOMPARE(clip("B")->duration, FrameTime(50));
        // ROLL: both sides.
        press(Key::Roll);
        QCOMPARE(win_->trimEdit()->side, 0);
        wheel(speed::JogMode::Relative2, -360);
        QCOMPARE(clip("A")->end(), FrameTime(49));
        QCOMPARE(clip("B")->start, FrameTime(49));
        apart();
        press(Key::Escape);
        QVERIFY(!win_->trimEdit());
        // ESC twice: Undo (the roll back).
        press(Key::Escape);
        QCOMPARE(clip("A")->end(), FrameTime(50));

        // SLIP SRC: the clip after the nearest edit slips (one undo step however far); ESC lets it go.
        state()->setPlayhead(48);
        program->seek(48);
        apart();
        press(Key::SlipSource);
        wheel(speed::JogMode::Relative2, -360 * 3);
        wheel(speed::JogMode::Relative2, -360 * 2);
        QCOMPARE(clip("B")->sourceIn, 15.0);
        QCOMPARE(clip("A")->sourceIn, 20.0);
        state()->undo();
        QCOMPARE(clip("B")->sourceIn, 20.0);
        press(Key::SlipDest);
        wheel(speed::JogMode::Relative2, 360);
        QCOMPARE(clip("A")->sourceIn, 21.0);
        apart();
        press(Key::Escape);
        const FrameTime at = state()->playhead();
        wheel(speed::JogMode::Relative2, 360);
        QTRY_COMPARE(state()->playhead(), at + 1);  // the wheel moves the playhead again
        QCOMPARE(clip("A")->sourceIn, 21.0);
        state()->undo();
        QCOMPARE(clip("A")->sourceIn, 20.0);
        // In SHTL a slip takes the wheel's turns too (the device turned relative until ESC), and a slow slip is still
        // one undo step.
        press(Key::Shuttle);
        apart();
        written->clear();
        press(Key::SlipSource);
        QVERIFY(sent(speed::jogModeReport(speed::JogMode::Relative2)));
        wheel(speed::JogMode::AbsoluteDeadZero, 2000);  // (sent before the device changed mode: ignored)
        QVERIFY(!program->isPlaying());
        wheel(speed::JogMode::Relative2, 720);
        QCOMPARE(clip("B")->sourceIn, 22.0);
        QTest::qWait(1600);
        wheel(speed::JogMode::Relative2, 360);
        QCOMPARE(clip("B")->sourceIn, 23.0);
        state()->undo();
        QCOMPARE(clip("B")->sourceIn, 20.0);
        apart();
        written->clear();
        press(Key::Escape);
        QVERIFY(sent(speed::jogModeReport(speed::JogMode::AbsoluteDeadZero)));
        QVERIFY(!program->isPlaying());
        press(Key::Jog);
        // ROLL twice slides the clip the playhead was in (B), not the one it moved to at the edit (C).
        state()->setPlayhead(98);
        program->seek(98);
        apart();
        press(Key::Roll);
        press(Key::Roll);
        QVERIFY(!win_->trimEdit());
        wheel(speed::JogMode::Relative2, 360);
        QCOMPARE(clip("B")->start, FrameTime(51));
        QCOMPARE(clip("A")->end(), FrameTime(51));
        state()->undo();
        QCOMPARE(clip("B")->start, FrameTime(50));
        apart();
        press(Key::Escape);

        // DIS, SMTH CUT and CUT at the nearest edit (50), lit to match; TRANS DUR held lengthens it.
        state()->setPlayhead(48);
        program->seek(48);
        auto transitions = [&] { return trackAt(*state()->sequence(), V1)->transitions; };
        press(Key::Dissolve);
        QCOMPARE(transitions().size(), size_t(1));
        QCOMPARE(transitions()[0].type, std::string("cross_dissolve"));
        QCOMPARE(transitions()[0].clipA, clip("A")->id);
        QCOMPARE(transitions()[0].clipB, clip("B")->id);
        QVERIFY(se->leds() & speed::ledBit(Key::Dissolve));
        QVERIFY(!(se->leds() & speed::ledBit(Key::Cut)));
        const FrameTime length = transitions()[0].duration;
        se->feed(keys({Key::TransitionDuration}));
        wheel(speed::JogMode::Relative2, 720);
        se->feed(keys({}));
        QCOMPARE(transitions()[0].duration, length + 2);
        QCOMPARE(state()->playhead(), FrameTime(48));  // (held, the wheel did not move it)
        press(Key::SmoothCut);
        QCOMPARE(transitions()[0].type, std::string("smooth_cut"));
        QCOMPARE(transitions()[0].duration, length + 2);  // the length kept
        QVERIFY(se->leds() & speed::ledBit(Key::SmoothCut));
        press(Key::Cut);
        QVERIFY(transitions().empty());
        QVERIFY(se->leds() & speed::ledBit(Key::Cut));
        // TRANS waits for a second press, but another key coming first runs it at once: here CUT takes it off again.
        state()->clearSelection();
        apart();
        press(Key::Transition);
        press(Key::Cut);
        QTest::qWait(450);
        QVERIFY(transitions().empty());
        // A dialog open: the keys wait.
        {
            QDialog dialog(win_.get());
            dialog.setModal(true);
            dialog.show();
            QTRY_VERIFY(QApplication::activeModalWidget());
            apart();
            press(Key::Dissolve);
            QVERIFY(transitions().empty());
        }

        // AUDIO LEVEL held: the sound at the playhead, half a dB a step, one undo step.
        state()->setPlayhead(20);
        const Clip* sound = edit::clipAt(*state()->sequence(), A1, 20);
        QVERIFY(sound);
        const Id soundId = sound->id;
        auto gain = [&] { return edit::clipById(*state()->sequence(), soundId)->audio.p("gain_db", 0, 0); };
        se->feed(keys({Key::AudioLevel}));
        wheel(speed::JogMode::Relative2, 360);
        wheel(speed::JogMode::Relative2, 360);
        se->feed(keys({}));
        QCOMPARE(gain(), 1.0);
        state()->undo();
        QCOMPARE(gain(), 0.0);
        // AUDIO LEVEL twice: a marker.
        const size_t markers = state()->sequence()->markers.size();
        apart();
        press(Key::AudioLevel);
        press(Key::AudioLevel);
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);

        // Switches lit while on: SNAP, VIDEO ONLY and AUDIO ONLY (one turns the other off).
        press(Key::Snap);
        QVERIFY(!state()->snapping());
        QVERIFY(!(se->leds() & speed::ledBit(Key::Snap)));
        apart();
        press(Key::Snap);
        QVERIFY(state()->snapping());
        press(Key::VideoOnly);
        QVERIFY(state()->sourceVideo() && !state()->sourceAudio());
        QVERIFY(se->leds() & speed::ledBit(Key::VideoOnly));
        press(Key::AudioOnly);
        QVERIFY(!state()->sourceVideo() && state()->sourceAudio());
        QVERIFY(se->leds() & speed::ledBit(Key::AudioOnly));
        QVERIFY(!(se->leds() & speed::ledBit(Key::VideoOnly)));
        apart();
        press(Key::AudioOnly);
        QVERIFY(state()->sourceVideo() && state()->sourceAudio());
        QVERIFY(!(se->leds() & (speed::ledBit(Key::VideoOnly) | speed::ledBit(Key::AudioOnly))));
        // LIVE O/WR latches; no multicam clip here, so no angle is lit.
        press(Key::LiveOverwrite);
        QVERIFY(se->liveOverwrite() && (se->leds() & speed::ledBit(Key::LiveOverwrite)));
        press(Key::Cam2);
        QVERIFY(!(se->leds() & speed::ledBit(Key::Cam2)));
        apart();
        press(Key::LiveOverwrite);
        QVERIFY(!se->liveOverwrite());
        // The battery, as reported.
        se->feed({7, 1, 55});
        QCOMPARE(se->batteryLevel(), 55);
        QVERIFY(se->charging());

        // Any key can do any menu command, and back.
        se->setBinding(Key::Cam1, "Sequence/Add Marker");
        QCOMPARE(se->bindingChanges().value("cam1").toString(), QString("Sequence/Add Marker"));
        state()->setPlayhead(30);
        press(Key::Cam1);
        QCOMPARE(state()->sequence()->markers.size(), markers + 2);
        se->setBinding(Key::In, "");  // nothing
        press(Key::In);
        QCOMPARE(state()->sequence()->inPoint, FrameTime(-1));
        QCOMPARE(se->secondBinding(Key::In), QString());  // (not its own thing: nothing twice either)
        se->setBinding(Key::Cam1, SpeedEditorControl::defaultBinding(Key::Cam1));
        se->setBinding(Key::In, SpeedEditorControl::defaultBinding(Key::In));
        QVERIFY(se->bindingChanges().isEmpty());
        se->setBinding(Key::Split, "Nowhere/Nothing");
        press(Key::Split);  // (said, not done)
        QVERIFY(win_->statusBar()->currentMessage().contains("Nowhere"));
        se->setBindingChanges({});

        // The keys dialog: a key given another command, then all put back.
        QDialog* keysDlg = win_->speedEditorKeysDialog();
        QVERIFY(keysDlg);
        auto* cam3 = keysDlg->findChild<QComboBox*>("speedKey_cam3");
        QVERIFY(cam3);
        QCOMPARE(cam3->currentData().toString(), QString("speed:cam3"));
        cam3->setCurrentIndex(cam3->findData("Clip/Add Edit"));
        QCOMPARE(se->binding(Key::Cam3), QString("Clip/Add Edit"));
        keysDlg->findChild<QPushButton*>("speedEditorReset")->click();
        QCOMPARE(se->binding(Key::Cam3), QString("speed:cam3"));
        QVERIFY(se->bindingChanges().isEmpty());
        keysDlg->close();
        // Edit > Control Surface shows it.
        QDialog* dlg = win_->controlSurfaceDialog();
        QVERIFY(dlg->findChild<QLabel*>("speedEditorStatus")->text().contains("Speed Editor"));
        QVERIFY(dlg->findChild<QLabel*>("speedEditorStatus")->text().contains("55"));
        // Unplugged.
        se->detach();
        QVERIFY(!se->isConnected());
        QVERIFY(!dlg->findChild<QLabel*>("speedEditorStatus")->text().contains("Connected"));
        dlg->close();
        appSettings().remove("speedEditor/bindings");
        state()->newProject();
    }

    void captureWhileEditing() {
        // File › Capture: a live feed (lavfi's test pattern and tone, in real time) recorded to ProRes; a second in, the
        // file is in the Captures bin and grows there; once stopped it holds the whole recording.
        const std::vector<std::string> kinds = captureDeviceKinds();
        if (std::find(kinds.begin(), kinds.end(), "lavfi") == kinds.end()) QSKIP("This build has no capture devices");
        state()->newProject();
        CaptureDialog* dlg = win_->captureDialog();
        QVERIFY(dlg && dlg == win_->captureDialog());
        QTemporaryDir dir;
        dlg->setSource("lavfi", "testsrc2=size=320x180:rate=25,realtime[out0];sine=sample_rate=48000,arealtime[out1]");
        dlg->setFolder(dir.path());
        dlg->setClipName("Take 01");
        dlg->setCodec("prores_lt");
        dlg->setWrapper("mov");
        dlg->setTimecode("1:00");
        QVERIFY(!dlg->record());
        QVERIFY(dlg->statusText().contains("hh:mm:ss:ff"));
        dlg->setTimecode("01:00:00:00");
        dlg->setSeconds(6);
        QVERIFY(dlg->record());
        QVERIFY(dlg->isRecording());
        QCOMPARE(dlg->filePath(), dir.filePath("Take 01.mov"));
        QTRY_VERIFY_WITH_TIMEOUT(dlg->importedMedia() != 0, 10000);
        const Id id = dlg->importedMedia();
        const MediaItem* m = state()->project().findMedia(id);
        QVERIFY(m && m->hasVideo && m->hasAudio);
        QCOMPARE(QString::fromStdString(m->bin), QString("Captures"));
        QCOMPARE(m->timecode, 3600.0);
        QVERIFY(m->duration < 5.9);
        QVERIFY(dlg->statusText().startsWith("Recording 00:00:0"));
        // Cut in while it records.
        QVERIFY(state()->edit("Place", [id](Project& p, Sequence& s) { return edit::placeMedia(p, s, id, 0, 0, 25, {TrackKind::Video, 0}, {}, false).ok; }));
        bool grew = false;
        QElapsedTimer t;
        t.start();
        while (dlg->isRecording() && t.elapsed() < 20000) {
            QTest::qWait(50);
            grew = grew || state()->isMediaGrowing(id);
        }
        QVERIFY(!dlg->isRecording());
        QVERIFY(grew);
        QTRY_VERIFY_WITH_TIMEOUT(std::fabs(state()->project().findMedia(id)->duration - 6.0) < 0.05 && !state()->isMediaGrowing(id), 20000);
        QVERIFY(dlg->statusText().startsWith("Recorded 00:00:06"));
        QVERIFY(dlg->statusText().contains("320×180") && dlg->statusText().contains("TC 01:00:00:00"));
        QCOMPARE(win_->findChild<QLineEdit*>("captureName")->text(), QString("Take 02"));
        // A feed that does not open: said so, nothing imported. Options that are not name=value refused.
        dlg->setSource(QString(), "udp://127.0.0.1:9?timeout=500000");
        dlg->setOptions("fifo_size");
        QVERIFY(!dlg->record());
        QVERIFY(dlg->statusText().contains("name=value"));
        dlg->setOptions("fifo_size=1000");
        dlg->setSeconds(0);
        QVERIFY(dlg->record());
        QTRY_VERIFY_WITH_TIMEOUT(!dlg->isRecording(), 20000);
        QTRY_VERIFY(!dlg->statusText().contains("Waiting"));
        QCOMPARE(dlg->importedMedia(), Id(0));
        dlg->hide();
    }

    // Tangent panels through a stand-in Tangent Hub: introduced, the controls file written, turns, resets and value
    // requests answered on the graded clip's Color Correct (a run of turns one undo step), the node menu, the jog
    // wheel and a menu command.
    void tangentPanels() {
        state()->newProject();
        QVERIFY(state()->edit("Shot", [](Project& p, Sequence& s) {
            Clip c = makeGeneratorClip(p, "color", 50);
            c.name = "Shot A";
            return edit::overwrite(p, s, V1, c).ok;
        }));
        const Id shot = clipNamed(*state()->sequence(), "Shot A")->id;
        state()->setPlayhead(10);
        TangentControl* tc = win_->tangent();
        QVERIFY(tc);
        tc->setEnabled(false);
        QTemporaryDir dir;
        tc->setFolder(dir.path());
        QTcpServer hub;
        QVERIFY(hub.listen(QHostAddress::LocalHost));
        tc->setPort(hub.serverPort());
        tc->setEnabled(true);
        QTRY_VERIFY(hub.hasPendingConnections());
        QTcpSocket* peer = hub.nextPendingConnection();
        std::string pending;
        std::vector<tangent::AppMessage> heard;
        size_t seen = 0;
        auto toHub = [&](const std::string& b) {
            peer->write(b.data(), qint64(b.size()));
            peer->flush();
        };
        // The next message of `command` (and id, unless ~0) after those already looked at.
        auto hear = [&](uint32_t command, uint32_t id = ~0u) -> std::optional<tangent::AppMessage> {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const QByteArray b = peer->readAll();
                pending.append(b.constData(), size_t(b.size()));
                size_t used = 0;
                std::string err;
                if (!tangent::decodeApp(pending, heard, &used, &err)) return std::nullopt;
                pending.erase(0, used);
                for (; seen < heard.size(); ++seen)
                    if (heard[seen].command == command && (id == ~0u || heard[seen].id == id)) return heard[seen++];
            }
            return std::nullopt;
        };
        auto colorCorrect = [&]() -> const Effect* {
            const Clip* c = edit::clipById(*state()->sequence(), shot);
            for (const Effect& e : c->effects)
                if (e.type == "color_correct") return &e;
            return nullptr;
        };

        // Introduced: Montage named with its folder, its controls written there.
        toHub(tangent::initiateComms(5, {{0x0C, 1}}));
        auto def = hear(tangent::ApplicationDefinition);
        QVERIFY(def);
        QCOMPARE(def->strings.size(), size_t(3));
        QCOMPARE(def->strings[0], std::string("Montage"));
        QCOMPARE(QDir::fromNativeSeparators(QString::fromStdString(def->strings[1])), dir.path());
        QVERIFY(hear(tangent::ModeValue, TangentControl::kMode));
        QFile xml(dir.path() + "/controls.xml");
        QVERIFY(xml.open(QIODevice::ReadOnly));
        const QString controls = QString::fromUtf8(xml.readAll());
        const uint32_t liftR = TangentControl::parameterId("lift_r"), gain = TangentControl::parameterId("gain");
        QVERIFY(controls.contains("<Controls>") && controls.indexOf("<Group name=\"Primaries\">") > controls.indexOf("<Controls>"));
        QVERIFY(controls.contains(QString::asprintf("<Parameter id=\"%#010x\">", liftR)));
        QVERIFY(controls.contains("<Name9>Lift R</Name9>"));
        QVERIFY(controls.contains("<Binding name=\"Lift\">"));
        QVERIFY(controls.contains(QString::asprintf("<Menu id=\"%#010x\">", TangentControl::kNodeMenu)));
        QVERIFY(controls.contains("<Name>Add Marker</Name>"));
        QVERIFY(QFile::exists(dir.path() + "/element-tk-map.xml"));
        QTRY_VERIFY(tc->isConnected());

        // Values asked for: the defaults, at their default (the clip has no Color Correct yet).
        toHub(tangent::idCommand(tangent::ParameterValueRequest, gain));
        auto v = hear(tangent::ParameterValue, gain);
        QVERIFY(v && v->value == 1.0f && v->atDefault);
        // Two turns of Lift R with one of Lift G between (a trackball rolling): a Color Correct added at the playhead's
        // clip, 0.2, one undo step.
        const uint32_t liftG = TangentControl::parameterId("lift_g");
        toHub(tangent::parameterChange(liftR, 0.1f));
        QVERIFY((v = hear(tangent::ParameterValue, liftR)) && std::fabs(v->value - 0.1f) < 1e-6f && !v->atDefault);
        toHub(tangent::parameterChange(liftG, 0.05f));
        QVERIFY((v = hear(tangent::ParameterValue, liftG)) && std::fabs(v->value - 0.05f) < 1e-6f);
        toHub(tangent::parameterChange(liftR, 0.1f));
        QVERIFY((v = hear(tangent::ParameterValue, liftR)) && std::fabs(v->value - 0.2f) < 1e-6f);
        QVERIFY(colorCorrect());
        QVERIFY(std::fabs(colorCorrect()->p("lift_r", 10) - 0.2) < 1e-6);
        // Too far: held at its range.
        toHub(tangent::parameterChange(liftR, 5.0f));
        QVERIFY((v = hear(tangent::ParameterValue, liftR)) && v->value == 1.0f);
        // One undo takes the whole run (the Color Correct with it); the Hub is told to ask again.
        state()->undo();
        QVERIFY(!colorCorrect());
        QVERIFY(hear(tangent::AllChange));
        // Reset: back to its default.
        toHub(tangent::parameterChange(liftR, 0.3f));
        QVERIFY((v = hear(tangent::ParameterValue, liftR)) && std::fabs(v->value - 0.3f) < 1e-6f);
        toHub(tangent::idCommand(tangent::ParameterReset, liftR));
        QVERIFY((v = hear(tangent::ParameterValue, liftR)) && v->value == 0.0f && v->atDefault);

        // The node menu: the clip shown in the Inspector, as nodes; the menu moves to the next corrector, and turns go
        // to its Color Correct.
        int added = 0;
        QVERIFY(state()->edit("Nodes", [shot, &added](Project& p, Sequence& s) {
            edit::clipById(s, shot)->effects.push_back(makeEffect(p, "invert"));
            if (!edit::convertToNodes(s, shot).ok) return false;
            return edit::addGradeNode(p, s, shot, edit::NodeHow::Serial, 1, &added).ok;
        }));
        state()->setSelection({shot});
        state()->selectGradeNode(1);
        QVERIFY(hear(tangent::AllChange));
        toHub(tangent::menuChange(TangentControl::kNodeMenu, 1));
        auto menu = hear(tangent::MenuString, TangentControl::kNodeMenu);
        QVERIFY(menu && menu->strings.size() == 1);
        QCOMPARE(menu->strings[0], "Node " + std::to_string(added));
        QCOMPARE(state()->selectedGradeNode(), added);
        auto shown = hear(tangent::DisplayText);
        QVERIFY(shown && shown->strings.size() == 2);
        QCOMPARE(shown->strings[0], std::string("Shot A"));
        QCOMPARE(shown->strings[1], "Node " + std::to_string(added));
        toHub(tangent::parameterChange(gain, 0.5f));
        QVERIFY((v = hear(tangent::ParameterValue, gain)) && std::fabs(v->value - 1.5f) < 1e-6f);
        const Clip* graded = edit::clipById(*state()->sequence(), shot);
        bool onAdded = false;
        for (const Effect& e : graded->effects) onAdded = onAdded || (e.type == "color_correct" && e.node == added);
        QVERIFY(onAdded);
        // A shared node: its own Color Correct changed (every clip showing it follows), none added to the clip.
        Id sharedId = 0;
        QVERIFY(state()->edit("Share", [shot, added, &sharedId](Project& p, Sequence& s) {
            return edit::shareGradeNode(p, s, shot, added, "Look", &sharedId).ok;
        }));
        toHub(tangent::parameterChange(gain, 0.25f));
        QVERIFY((v = hear(tangent::ParameterValue, gain)) && std::fabs(v->value - 1.75f) < 1e-6f);
        // A turn that is not a number changes nothing.
        toHub(tangent::parameterChange(gain, std::numeric_limits<float>::quiet_NaN()));
        QVERIFY((v = hear(tangent::ParameterValue, gain)) && std::fabs(v->value - 1.75f) < 1e-6f);
        const SharedNode* look = findSharedNode(*state()->sequence(), sharedId);
        QVERIFY(look && look->effects.size() == 1);
        QVERIFY(std::fabs(look->effects[0].p("gain", 10) - 1.75) < 1e-6);
        int onClip = 0;
        for (const Effect& e : edit::clipById(*state()->sequence(), shot)->effects) onClip += e.type == "color_correct" && e.node == added;
        QCOMPARE(onClip, 0);

        // The jog wheel steps the playhead; a menu command runs.
        state()->setSelection({});
        const FrameTime at = state()->playhead();
        toHub(tangent::transport(3, 0));
        QTRY_COMPARE(state()->playhead(), at + 3);
        // The shuttle ring's turns add up (1x, then 2x), and back in the middle it stops.
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        toHub(tangent::transport(0, 1));
        QTRY_COMPARE(program->speed(), 1.0);
        toHub(tangent::transport(0, 1));
        QTRY_COMPARE(program->speed(), 2.0);
        toHub(tangent::transport(0, -2));
        QTRY_COMPARE(program->speed(), 0.0);
        // However far the ring is said to turn (from a turn already made, so the two add up), it stops at 32 times.
        // (from the middle of ten hours, so playing at 32 times reaches neither end before the next turn)
        const FrameTime far = FrameTime(std::llround(state()->sequence()->fpsValue() * 36000));
        QVERIFY(state()->edit("Far", [shot, far](Project& p, Sequence& s) {
            auto loc = edit::locate(s, shot);
            if (!loc) return false;
            Clip copy = *edit::clipById(s, shot);
            copy.id = p.newId();
            copy.start = far;
            trackAt(s, loc->track)->clips.push_back(copy);
            return true;
        }));
        state()->setPlayhead(far / 2);
        program->seek(far / 2);
        toHub(tangent::transport(0, 1));
        QTRY_COMPARE(program->speed(), 1.0);
        toHub(tangent::transport(0, std::numeric_limits<int32_t>::max()));
        QTRY_COMPARE(program->speed(), 32.0);
        toHub(tangent::transport(0, std::numeric_limits<int32_t>::min()));
        QTRY_COMPARE(program->speed(), -32.0);
        toHub(tangent::transport(0, 6));
        QTRY_COMPARE(program->speed(), 0.0);
        state()->undo();
        const QString addMarker = "Sequence/Add Marker";
        QVERIFY(keymap::find(win_.get(), addMarker) && tc->actionId(addMarker));
        const size_t markers = state()->sequence()->markers.size();
        toHub(tangent::idCommand(tangent::ActionOn, tc->actionId(addMarker)));
        QTRY_COMPARE(state()->sequence()->markers.size(), markers + 1);
        tc->setEnabled(false);
        QVERIFY(!tc->isConnected());
    }

    void controlSurfaceHui() {
        // A surface in HUI mode (Pro Tools' protocol): the same strips, transport and displays, in zones and ports.
        struct Recorder : MidiConnection {
            std::shared_ptr<std::vector<MidiMessage>> sent = std::make_shared<std::vector<MidiMessage>>();
            bool send(const MidiMessage& m) override {
                sent->push_back(m);
                return true;
            }
            std::string inputName() const override { return "HUI"; }
            std::string outputName() const override { return "HUI"; }
        };
        state()->newProject();
        QVERIFY(state()->edit("Tracks", [](Project& p, Sequence& s) {
            while (s.audioTracks.size() < 10) edit::addTrack(p, s, TrackKind::Audio);
            for (size_t i = 0; i < s.audioTracks.size(); ++i) s.audioTracks[i].name = "Trk " + std::to_string(i + 1);
            s.audioTracks[0].name = "Dialogue A";
            edit::overwrite(p, s, V1, makeGeneratorClip(p, "color", 100));
            return true;
        }));
        auto* mixer = win_->findChild<MixerPanel*>();
        QTRY_COMPARE(mixer->trackStrips(), 10);
        ControlSurface* surface = win_->controlSurface();
        QVERIFY(surface && !surface->isConnected());
        surface->setProtocol(SurfaceProtocol::Hui);
        auto rec = std::make_unique<Recorder>();
        auto sent = rec->sent;
        surface->setConnection(std::move(rec));
        QVERIFY(surface->isConnected());
        auto has = [&](const MidiMessage& m) { return std::find(sent->begin(), sent->end(), m) != sent->end(); };
        // A light or fader is two messages in a row.
        auto hasPair = [&](const std::vector<MidiMessage>& ms) {
            return std::search(sent->begin(), sent->end(), ms.begin(), ms.end()) != sent->end();
        };
        auto press = [&](int zone, int port, bool down = true) {
            surface->handle({0xB0, 0x0F, uint8_t(zone)});
            surface->handle({0xB0, 0x2F, uint8_t((down ? 0x40 : 0) | port)});
        };
        auto track = [&](int i) -> const Track& { return state()->sequence()->audioTracks.at(size_t(i)); };
        // On connecting: pinged, then shown everything: the strips' names, the main display, faders, lights, timecode.
        QCOMPARE(sent->front(), hui::ping());
        QVERIFY(has(hui::stripText(0, "Dial")) && has(hui::stripText(1, "Trk ")));
        QVERIFY(has(hui::display(0, "Dial Trk  ")));
        QVERIFY(has(hui::display(4, "0.0  0.0  ")));
        QVERIFY(hasPair(hui::fader(0, mcu::faderValue(0))));
        QVERIFY(hasPair(hui::led(0x0E, 3, true)));  // Stop
        QVERIFY(hasPair(hui::led(0x16, 0, true)));  // the timecode light
        QVERIFY(has(hui::timecode("00:00:00:00")[0]));
        QVERIFY(has(hui::stripText(8, "   1")));  // Select-Assign: the first track on the strips
        // Nothing of Mackie Control's: no pitch bend, channel pressure, notes but the ping, or its system exclusive.
        QVERIFY(std::none_of(sent->begin(), sent->end(), [](const MidiMessage& m) {
            return (m[0] & 0xF0) == 0xE0 || (m[0] & 0xF0) == 0xD0 || (m[0] == 0x90 && m != hui::ping()) || (m[0] == 0xF0 && (m.size() < 5 || m[4] != 0x05));
        }));

        // A touched fader moving: the track's volume, the position echoed back.
        sent->clear();
        press(0, 0);
        QVERIFY(mixer->trackFader(0)->isSliderDown());
        const int minus6 = mcu::faderValue(-6);
        for (const MidiMessage& m : hui::fader(0, mcu::faderValue(-3))) surface->handle(m);
        for (const MidiMessage& m : hui::fader(0, minus6)) surface->handle(m);
        QCOMPARE(track(0).volumeDb, -6.0);
        QVERIFY(hasPair(hui::fader(0, minus6)));
        press(0, 0, false);
        QVERIFY(!mixer->trackFader(0)->isSliderDown());
        state()->undo();
        QCOMPARE(track(0).volumeDb, 0.0);
        // A fader's moves queued up together (four messages) are applied once, at the last position.
        sent->clear();
        for (const MidiMessage& m : hui::fader(6, mcu::faderValue(-20))) surface->receive(m);
        for (const MidiMessage& m : hui::fader(6, mcu::faderValue(-10))) surface->receive(m);
        QTRY_COMPARE(track(6).volumeDb, -10.0);
        QVERIFY(hasPair(hui::fader(6, mcu::faderValue(-10))) && !hasPair(hui::fader(6, mcu::faderValue(-20))));
        QTRY_VERIFY_WITH_TIMEOUT(!mixer->trackFader(6)->isSliderDown(), 3000);

        // Mute lights up; the V-Pot pans (the main display shows it for a moment) and V-Select centres it.
        sent->clear();
        press(2, 2);
        press(2, 2, false);
        QVERIFY(track(2).muted);
        QVERIFY(hasPair(hui::led(2, 2, true)));
        press(3, 3);
        QVERIFY(track(3).solo && hasPair(hui::led(3, 3, true)));
        surface->handle({0xB0, 0x40, 0x45});
        QCOMPARE(track(0).pan, 0.1);
        QVERIFY(has(hui::ring(0, mcu::panRing(0.1))));
        QTRY_VERIFY_WITH_TIMEOUT(has(hui::display(4, "R10  0.0  ")), 900);
        surface->handle({0xB0, 0x40, 0x02});
        QVERIFY(std::fabs(track(0).pan - 0.06) < 1e-9);
        press(0, 5);
        QCOMPARE(track(0).pan, 0.0);

        // Banking.
        sent->clear();
        press(0x0A, 3);
        QCOMPARE(surface->bank(), 2);
        QVERIFY(has(hui::stripText(0, "Trk ")) && has(hui::stripText(8, "   3")));
        press(0x0A, 0);
        QCOMPARE(surface->bank(), 1);
        press(0x0A, 1);
        QCOMPARE(surface->bank(), 0);

        // Automation: select a strip, Touch on it, then Off.
        sent->clear();
        press(5, 1);
        QCOMPARE(surface->selectedTrack(), 5);
        QVERIFY(hasPair(hui::led(5, 1, true)));
        press(0x18, 5);
        QCOMPARE(track(5).automation, int(AutomationMode::Touch));
        QVERIFY(hasPair(hui::led(0x18, 5, true)));
        press(0x18, 3);
        QCOMPARE(track(5).automation, int(AutomationMode::Off));
        QVERIFY(hasPair(hui::led(0x18, 3, true)) && hasPair(hui::led(0x18, 5, false)));

        // Transport: RTZ and End, the jog wheel, Loop, markers from the keypad's Enter, Play and Stop.
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        state()->setPlayhead(50);
        press(0x0F, 0);
        QTRY_COMPARE(state()->playhead(), FrameTime(0));
        press(0x0F, 1);
        QTRY_COMPARE(state()->playhead(), state()->sequence()->duration());
        state()->setPlayhead(10);
        surface->handle({0xB0, 0x0D, 0x43});
        QTRY_COMPARE(state()->playhead(), FrameTime(13));
        surface->handle({0xB0, 0x0D, 0x01});
        QTRY_COMPARE(state()->playhead(), FrameTime(12));
        sent->clear();
        press(0x0F, 3);
        auto* loop = win_->findChild<QToolButton*>("programLoop");
        QVERIFY(loop && loop->isChecked() && program->loop());
        QVERIFY(hasPair(hui::led(0x0F, 3, true)));
        press(0x0F, 3);
        QVERIFY(!loop->isChecked() && !program->loop());
        // Looping set by playback itself (ADR's takes) with the button left as it was: Loop follows playback.
        loop->setChecked(true);
        program->setLoop(false);
        press(0x0F, 3);
        QVERIFY(loop->isChecked() && program->loop());
        press(0x0F, 3);
        QVERIFY(!loop->isChecked() && !program->loop());
        // The timecode display is sent from its rightmost digit up to the last that changed (a frame: one digit).
        state()->setPlayhead(0);
        surface->refresh();
        sent->clear();
        state()->setPlayhead(1);
        surface->refresh();
        QVERIFY(has(hui::timecode(hui::timecodeDigits("00:00:00:01"), 1)));
        const size_t markers = state()->sequence()->markers.size();
        press(0x14, 0);
        QCOMPARE(state()->sequence()->markers.size(), markers + 1);
        press(0x08, 3);  // Undo
        QCOMPARE(state()->sequence()->markers.size(), markers);
        sent->clear();
        press(0x0E, 4);
        QVERIFY(program->isPlaying());
        QVERIFY(hasPair(hui::led(0x0E, 4, true)) && hasPair(hui::led(0x0E, 3, false)));
        press(0x0E, 3);
        QVERIFY(!program->isPlaying());

        // Meters: both sides, and let fall once levels stop coming.
        sent->clear();
        surface->setLevels(0, 0, QVector<float>{0.5f, 0.25f, 1.0f, 1.0f});
        QVERIFY(has(hui::meter(0, 0, 8)) && has(hui::meter(0, 1, 8)) && has(hui::meter(1, 0, 12)));
        QTRY_VERIFY_WITH_TIMEOUT(has(hui::meter(0, 0, 0)) && has(hui::meter(1, 1, 0)), 2000);
        // Pinged once a second; a ping answered a second after the last is not news, an answer after a silence long
        // enough for the surface to have gone off line (two seconds) is, and so is a surface switched on (its several
        // resets shown everything once).
        sent->clear();
        QTRY_VERIFY_WITH_TIMEOUT(has(hui::ping()), 2500);
        surface->handle({0x90, 0x00, 0x7F});
        sent->clear();
        QTest::qWait(1000);
        surface->handle({0x90, 0x00, 0x7F});
        QVERIFY(!has(hui::stripText(0, "Dial")));
        QTest::qWait(1900);
        sent->clear();
        surface->handle({0x90, 0x00, 0x7F});
        QVERIFY(has(hui::stripText(0, "Dial")));
        QTest::qWait(600);
        sent->clear();
        surface->handle({0xFF});
        surface->handle({0xFF});
        surface->handle({0x90, 0x00, 0x7F});
        QCOMPARE(std::count(sent->begin(), sent->end(), hui::stripText(0, "Dial")), 1);
        QVERIFY(has(hui::display(0, "Dial Trk  ")));
        // The same project opened again keeps the bank and selection; another project starts afresh.
        const QString file = dir_.filePath("hui.montage");
        QVERIFY(state()->save(file));
        QVERIFY(state()->open(file));
        press(0x0A, 3);
        press(4, 1);
        QCOMPARE(surface->bank(), 2);
        QCOMPARE(surface->selectedTrack(), 6);
        QVERIFY(state()->open(file));
        surface->refresh();
        QCOMPARE(surface->bank(), 2);
        QCOMPARE(surface->selectedTrack(), 6);
        state()->newProject();
        surface->refresh();
        QCOMPARE(surface->bank(), 0);
        QCOMPARE(surface->selectedTrack(), -1);
        QVERIFY(state()->open(file));
        surface->refresh();

        // Disconnecting leaves it clean.
        sent->clear();
        surface->disconnectSurface();
        QVERIFY(!surface->isConnected());
        QVERIFY(hasPair(hui::fader(0, 0)) && has(hui::stripText(0, "")) && has(hui::display(4, "")));
        QVERIFY(hasPair(hui::zoneOff(2)) && hasPair(hui::zoneOff(0x18)));

        // Switched to Mackie Control while connected: taken over in it on the same ports.
        auto rec2 = std::make_unique<Recorder>();
        auto sent2 = rec2->sent;
        surface->setConnection(std::move(rec2));
        sent2->clear();
        surface->setProtocol(SurfaceProtocol::MackieControl);
        QVERIFY(surface->isConnected() && surface->protocol() == SurfaceProtocol::MackieControl);
        QCOMPARE(sent2->front(), mcu::deviceQuery());
        QVERIFY(std::find(sent2->begin(), sent2->end(), mcu::lcd(0, "Dialog ")) != sent2->end());
        // The same connection still: unplugged just before the switch, the loss still closes it.
        surface->connectionLost();
        surface->setProtocol(SurfaceProtocol::Hui);
        QTRY_VERIFY(!surface->isConnected());
        surface->setProtocol(SurfaceProtocol::MackieControl);

        // The dialog offers both protocols and keeps the choice.
        QDialog* dlg = win_->controlSurfaceDialog();
        auto* protocol = dlg->findChild<QComboBox*>("surfaceProtocol");
        QVERIFY(protocol && protocol->count() == 2 && protocol->currentIndex() == 0);
        protocol->setCurrentIndex(1);
        QCOMPARE(surface->protocol(), SurfaceProtocol::Hui);
        QCOMPARE(appSettings().value("controlSurface/protocol").toString(), QString("hui"));
        protocol->setCurrentIndex(0);
        QCOMPARE(surface->protocol(), SurfaceProtocol::MackieControl);
        dlg->close();
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
            if (f == 30) QCOMPARE(mixer->heard().audioTracks.at(0).volumeDb, -12.0);  // heard, the project unchanged
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

    void mixerSendsAndBusAutomation() {
        loadDemo();
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        QTRY_VERIFY(mixer->trackFader(0));
        auto seq = [&]() { return state()->sequence(); };
        auto a1 = [&]() -> const Track& { return seq()->audioTracks.at(0); };
        const size_t buses = seq()->buses.size();
        QVERIFY(mixer->addSendButton(0));
        // "+ Send" › New Effects Return: a return bus, and a send row on the strip.
        const Id ret = mixer->newReturn(0);
        QVERIFY(ret);
        QCOMPARE(seq()->buses.size(), buses + 1);
        QTRY_VERIFY(mixer->sendLevel(0, 0));
        QCOMPARE(a1().sends.size(), size_t(1));
        QCOMPARE(a1().sends[0].bus, ret);
        // Its slider sets the level, a drag one undo step; the same bus again adds nothing.
        QSlider* level = mixer->sendLevel(0, 0);
        level->setValue(-100);
        level->setValue(-120);
        QCOMPARE(a1().sends[0].levelDb, -12.0);
        state()->undo();
        QCOMPARE(a1().sends[0].levelDb, 0.0);
        mixer->addSend(0, ret);
        QCOMPARE(a1().sends.size(), size_t(1));
        // Touch on A1: a mute pressed at frame 20 is written from there to the stop (a switch latches); the send's level
        // held at -6 dB over frames 30-39. One undo step.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Touch));
        mixer->playbackStarted(0);
        QVERIFY(mixer->recordingAutomation());
        level = mixer->sendLevel(0, 0);
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 20) mixer->trackMute(0)->setChecked(true);
            if (f == 30) {
                level->setSliderDown(true);
                level->setValue(-60);
            }
            if (f == 40) level->setSliderDown(false);
            mixer->playbackPosition(f);
            if (f == 25) QVERIFY(mixer->heard().audioTracks.at(0).muted && !a1().muted);  // heard while it is written
        }
        mixer->playbackStopped(60);
        QVERIFY(!mixer->recordingAutomation());
        QVERIFY(!a1().muted && a1().muteAuto.animated());
        QVERIFY(!trackMutedAt(a1(), 10) && trackMutedAt(a1(), 30) && trackMutedAt(a1(), 60));
        QCOMPARE(a1().sends[0].levelAuto.at(35), -6.0);
        QCOMPARE(a1().sends[0].levelAuto.at(10), 0.0);
        QCOMPARE(a1().sends[0].levelDb, 0.0);
        state()->undo();
        QVERIFY(!a1().muteAuto.animated() && !a1().sends[0].levelAuto.animated());
        state()->redo();
        QVERIFY(a1().muteAuto.animated());
        // The mute button follows its lane at the playhead, and clicked there switches the lane to its next point.
        state()->setPlayhead(10);
        QVERIFY(!mixer->trackMute(0)->isChecked());
        state()->setPlayhead(30);
        QVERIFY(mixer->trackMute(0)->isChecked());
        mixer->trackMute(0)->click();
        QVERIFY(!mixer->trackMute(0)->isChecked());
        QVERIFY(!a1().muted && !trackMutedAt(a1(), 30) && !trackMutedAt(a1(), 45) && trackMutedAt(a1(), 25));
        state()->undo();
        QVERIFY(trackMutedAt(a1(), 30));
        state()->setPlayhead(10);
        mixer->trackMute(0)->click();  // before the lane mutes: muted from here, the start as it was
        QVERIFY(trackMutedAt(a1(), 10) && trackMutedAt(a1(), 15) && !trackMutedAt(a1(), 5) && !a1().muted);
        state()->undo();
        // No send to the bus the track goes to; routing it to a bus it sends to drops that send.
        QVERIFY(!state()->apply("Send", [ret](Project&, Sequence& s) {
            s.audioTracks[0].output = ret;
            return edit::setSend(s, 0, ret, 0, false);
        }));
        QCOMPARE(a1().output, Id(0));
        {
            QComboBox* out = mixer->trackFader(0)->parentWidget()->findChild<QComboBox*>("outputCombo");  // A1's strip
            QVERIFY(out);
            const int to = out->findData(QVariant::fromValue(qulonglong(ret)));
            QVERIFY(to >= 0);
            emit out->activated(to);
            QCOMPARE(a1().output, ret);
            QVERIFY(a1().sends.empty());
            state()->undo();
            QCOMPARE(a1().sends.size(), size_t(1));
            QTRY_VERIFY(mixer->sendLevel(0, 0));
        }
        // The return bus in Write: its fader -10 dB from frame 15; Write hands over to Touch.
        const int busIndex = int(seq()->buses.size()) - 1;
        QSlider* busFader = mixer->busFader(busIndex);
        QComboBox* busMode = mixer->busAutomationMode(busIndex);
        QVERIFY(busFader && busMode && mixer->busPan(busIndex) && mixer->busMute(busIndex));
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Read));
        busMode->setCurrentIndex(int(AutomationMode::Write));
        QCOMPARE(seq()->buses.back().automation, int(AutomationMode::Write));
        mixer->playbackStarted(0);
        QVERIFY(mixer->recordingAutomation() && !busMode->isEnabled());
        for (FrameTime f = 0; f <= 40; ++f) {
            if (f == 15) busFader->setValue(-100);
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(40);
        {
            const Bus& b = seq()->buses.back();
            QVERIFY(b.volumeAuto.animated());
            QCOMPARE(b.volumeAuto.at(5), 0.0);
            QCOMPARE(b.volumeAuto.at(25), -10.0);
            QCOMPARE(b.volumeDb, 0.0);
            QCOMPARE(b.automation, int(AutomationMode::Touch));
        }
        QCOMPARE(busMode->currentIndex(), int(AutomationMode::Touch));
        // Stopped, reading: the bus fader follows its lane, and moving it keys the lane at the playhead.
        busMode->setCurrentIndex(int(AutomationMode::Read));
        state()->setPlayhead(25);
        QCOMPARE(busFader->value(), -100);
        busFader->setValue(-30);
        QVERIFY(seq()->buses.back().volumeAuto.keyAt(25) && seq()->buses.back().volumeAuto.keyAt(25)->v == -3.0);
        QCOMPARE(seq()->buses.back().volumeDb, 0.0);
        // A send set below the slider's -60 dB stays there through a Write pass that leaves it alone.
        QVERIFY(state()->apply("Quiet", [ret](Project&, Sequence& s) { return edit::setSend(s, 0, ret, -90, false); }));
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Write));
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 20; ++f) mixer->playbackPosition(f);
        mixer->playbackStopped(20);
        QCOMPARE(a1().sends[0].levelDb, -90.0);
        QVERIFY(!a1().sends[0].levelAuto.animated() || std::fabs(a1().sends[0].levelAuto.at(10) - -90.0) < 1e-9);
        // Deleting the bus takes A1's send with it.
        QVERIFY(state()->apply("Delete Bus", [ret](Project&, Sequence& s) { return edit::removeBus(s, ret); }));
        QVERIFY(a1().sends.empty());
        QTRY_VERIFY(!mixer->sendLevel(0, 0));
    }

    void insertAutomationAndInterruptedPasses() {
        loadDemo();
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        QTRY_VERIFY(mixer->trackFader(0));
        auto a1 = [&]() -> const Track& { return state()->sequence()->audioTracks.at(0); };
        const Id track = a1().id;
        const std::string name = a1().name;
        Id eq = 0, ch = 0;
        QVERIFY(state()->edit("Inserts", [&](Project& p, Sequence& s) {
            Effect e;
            e.id = eq = p.newId();
            e.type = "eq3";
            s.audioTracks[0].effects.push_back(e);
            Effect c;
            c.id = ch = p.newId();
            c.type = "channels";
            s.audioTracks[0].effects.push_back(c);
            return true;
        }));
        auto value = [&](Id fx, const char* param, FrameTime t) {  // as heard (the pass plays over the project)
            const Sequence heard = mixer->heard();
            for (const Effect& e : heard.audioTracks.at(0).effects)
                if (e.id == fx) return e.p(param, t, 0);
            return -100.0;
        };
        auto lane = [&](Id fx, const char* param) {
            for (const Effect& e : a1().effects)
                if (e.id == fx && e.params.count(param)) return e.params.at(param);
            return Param();
        };
        // Nothing is taken outside a pass.
        QVERIFY(!state()->captureParameter({track, eq, "mid_db", 6, 0, 0.048, false, {}}));
        // Touch on A1: the EQ's mid held at +6 dB over frames 10-19, then let go (back to what was there); Channel Tools'
        // mode (a choice) changed at frame 30, latching to the stop; an edit at frame 40 is kept, the pass carrying on.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Touch));
        bool held = false;
        mixer->playbackStarted(0);
        QVERIFY(mixer->recordingAutomation());
        for (FrameTime f = 0; f <= 80; ++f) {
            if (f == 10) {
                held = true;
                QVERIFY(state()->captureParameter({track, eq, "mid_db", 6, 0, 0.048, false, [&held] { return held; }}));
                QCOMPARE(value(eq, "mid_db", f), 6.0);  // heard at once
                QVERIFY(!lane(eq, "mid_db").animated() && !lane(eq, "mid_db").value);  // the project untouched
            }
            if (f == 20) held = false;
            if (f == 30) QVERIFY(state()->captureParameter({track, ch, "mode", 1, 0, 0.004, true, {}}));
            if (f == 40) {
                QVERIFY(state()->edit("Rename", [](Project&, Sequence& s) {
                    s.audioTracks[0].name = "Dialogue";
                    return true;
                }));
                QCOMPARE(a1().name, std::string("Dialogue"));
                QVERIFY(mixer->recordingAutomation());
            }
            mixer->playbackPosition(f);
            if (f == 15) QCOMPARE(value(eq, "mid_db", f), 6.0);
            if (f == 25) QCOMPARE(value(eq, "mid_db", f), 0.0);
            if (f == 50) QCOMPARE(value(ch, "mode", f), 1.0);  // still latched after the edit
        }
        mixer->playbackStopped(80);
        QVERIFY(!mixer->recordingAutomation());
        QCOMPARE(a1().name, std::string("Dialogue"));
        const Param mid = lane(eq, "mid_db");
        QVERIFY(mid.animated());
        QCOMPARE(mid.at(5), 0.0);
        QCOMPARE(mid.at(15), 6.0);
        QVERIFY(std::fabs(mid.at(70)) < 1e-9);  // a second's glide back from frame 19
        const Param mode = lane(ch, "mode");
        QVERIFY(mode.animated());
        QCOMPARE(mode.at(20), 0.0);
        QCOMPARE(mode.at(35), 1.0);
        QCOMPARE(mode.at(80), 1.0);
        for (const Keyframe& k : mode.keys) QCOMPARE(int(k.interp), int(Interp::Hold));
        // The pass is one undo step, after the rename's.
        state()->undo();
        QVERIFY(!lane(eq, "mid_db").animated() && !lane(ch, "mode").animated());
        QCOMPARE(a1().name, std::string("Dialogue"));
        state()->undo();
        QCOMPARE(a1().name, name);
        state()->redo();
        state()->redo();
        QVERIFY(lane(eq, "mid_db").animated() && lane(ch, "mode").animated());
        // Undo during a pass undoes the step before it; the pass carries on. Here the EQ's low band is typed into the
        // Inspector at frame 15 (counted as held for half a second).
        state()->inspectChain(track);
        QApplication::processEvents();
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector);
        auto lowRow = [&]() -> QDoubleSpinBox* {  // the EQ's first row, Low (dB) (looked up each time: rows are rebuilt)
            for (QDoubleSpinBox* sp : inspector->findChildren<QDoubleSpinBox*>())
                if (sp->minimum() == -24 && sp->maximum() == 24) return sp;
            return nullptr;
        };
        QVERIFY(lowRow());
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 80; ++f) {
            if (f == 5) {
                held = true;
                QVERIFY(state()->captureParameter({track, eq, "high_db", -6, 0, 0.048, false, [&held] { return held; }}));
            }
            if (f == 10) {
                state()->undo();
                QVERIFY(mixer->recordingAutomation());
                QVERIFY(!lane(eq, "mid_db").animated());  // the last pass undone
            }
            // A stepped parameter (a plugin's often runs 0 to 1): its keys hold at the values moved to, not rounded.
            if (f == 25) QVERIFY(state()->captureParameter({track, eq, "mid_q", 0.333, 0.9, 1e-6, true, {}, false}));
            if (f == 15) {
                QApplication::processEvents();
                QDoubleSpinBox* low = lowRow();
                QVERIFY(low);
                low->setValue(-3);
            }
            if (f == 20) held = false;
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(80);
        QVERIFY(!lane(eq, "mid_db").animated());  // the undone pass stays undone
        QCOMPARE(lane(eq, "high_db").at(12), -6.0);  // moved before the undo, written
        {
            const Param q = lane(eq, "mid_q");
            QVERIFY(q.animated() && std::fabs(q.at(40) - 0.333) < 1e-9 && std::fabs(q.at(10) - 0.9) < 1e-9);
            for (const Keyframe& k : q.keys) QCOMPARE(int(k.interp), int(Interp::Hold));
        }
        const Param lowLane = lane(eq, "low_db");
        QVERIFY(lowLane.animated());
        QCOMPARE(lowLane.at(10), 0.0);
        QCOMPARE(lowLane.at(17), -3.0);
        QVERIFY(std::fabs(lowLane.at(80)) < 1e-9);
        // An edit during a pass to a lane the pass is not writing (a point added to A1's volume lane) is kept, and a
        // control the pass writes keeps its value through the refresh that edit causes.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Latch));
        QSlider* fader = mixer->trackFader(0);
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 40; ++f) {
            if (f == 5) {
                fader->setSliderDown(true);
                fader->setValue(-100);
            }
            if (f == 8) fader->setSliderDown(false);  // latched at -10 dB
            if (f == 20) {
                QVERIFY(state()->edit("Pan point", [](Project&, Sequence& s) {
                    s.audioTracks[0].panAuto.addKey(30, -1);
                    return true;
                }));
                QCOMPARE(fader->value(), -100);  // still latched
            }
            mixer->playbackPosition(f);
            if (f == 30) QCOMPARE(mixer->heard().audioTracks.at(0).volumeDb, -10.0);
        }
        mixer->playbackStopped(40);
        QVERIFY(a1().panAuto.keyAt(30));  // the edit made under the pass kept
        QCOMPARE(a1().volumeAuto.at(30), -10.0);
        QCOMPARE(a1().volumeAuto.at(40), -10.0);
        // A sequence made active mid-pass: what was written stays on the sequence it was recorded on.
        const Id first = state()->sequence()->id;
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 20; ++f) {
            if (f == 5) {
                fader->setSliderDown(true);
                fader->setValue(-200);
            }
            if (f == 8) fader->setSliderDown(false);
            if (f == 15) {
                const Id other = edit::duplicateSequence(state()->mutableProject(), first, "Other");
                state()->setActiveSequence(other);
            }
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(20);
        QCOMPARE(state()->project().findSequence(first)->audioTracks.at(0).volumeAuto.at(10), -20.0);
        state()->setActiveSequence(first);
        // Through edits during a pass: a pressed mute stays pressed (and written) when the strips refresh; a level set
        // under the pass (A1's fader level, from elsewhere) is what the fader shows and glides back to; an insert the
        // pass writes shows its played value in the Inspector.
        state()->setActiveSequence(first);
        state()->inspectChain(track);
        QApplication::processEvents();
        fader = mixer->trackFader(0);
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Touch));
        QVERIFY(state()->edit("Clear lanes", [](Project&, Sequence& s) {
            s.audioTracks[0].volumeAuto = Param(), s.audioTracks[0].muteAuto = Param(), s.audioTracks[0].volumeDb = 0;
            return true;
        }));
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 5) mixer->trackMute(0)->click();
            if (f == 10) {
                QVERIFY(state()->edit("Level", [](Project&, Sequence& s) {
                    s.audioTracks[0].volumeDb = -10;
                    return true;
                }));
                QVERIFY(mixer->trackMute(0)->isChecked());  // still pressed after the refresh
            }
            if (f == 20) {
                QVERIFY(state()->captureParameter({track, eq, "low_db", -9, 0, 0.048, false, {}}));
                QVERIFY(state()->edit("Rename", [](Project&, Sequence& s) {  // (any edit: the Inspector refreshes)
                    s.audioTracks[0].name = "Dialogue 2";
                    return true;
                }));
                QApplication::processEvents();
                QVERIFY(lowRow());
                QCOMPARE(lowRow()->value(), -9.0);  // the Inspector shows what plays
                QCOMPARE(*state()->liveParameter(track, eq, "low_db"), -9.0);
            }
            if (f == 30) {
                fader->setSliderDown(true);
                fader->setValue(-30);
            }
            if (f == 35) fader->setSliderDown(false);
            mixer->playbackPosition(f);
            if (f == 12) QCOMPARE(fader->value(), -100);  // the level set under the pass
            if (f == 25) QVERIFY(mixer->heard().audioTracks.at(0).muted);
        }
        mixer->playbackStopped(60);
        QVERIFY(trackMutedAt(a1(), 30) && trackMutedAt(a1(), 60) && !trackMutedAt(a1(), 2));
        QCOMPARE(a1().volumeDb, -10.0);
        QCOMPARE(a1().volumeAuto.at(20), -10.0);  // either side of the touch: the level set under the pass
        QCOMPARE(a1().volumeAuto.at(32), -3.0);
        QVERIFY(std::fabs(a1().volumeAuto.at(90) - -10.0) < 1e-9);  // glided back after the touch
        // Stopping while another gesture (a drag) is held: the pass waits for it, and is written after it.
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 20; ++f) {
            if (f == 5) {
                fader->setSliderDown(true);
                fader->setValue(-200);
            }
            if (f == 8) fader->setSliderDown(false);
            mixer->playbackPosition(f);
        }
        state()->beginGesture("Drag");
        state()->updateGesture([](Project&, Sequence& s) { s.markers.push_back(Marker{3, 0, "dragged", "", 0, false}); });
        mixer->playbackStopped(20);
        QVERIFY(state()->inGesture() && mixer->recordingAutomation());
        state()->endGesture(true);
        QTRY_VERIFY(!mixer->recordingAutomation());
        QCOMPARE(a1().volumeAuto.at(6), -20.0);
        QVERIFY(std::any_of(state()->sequence()->markers.begin(), state()->sequence()->markers.end(),
                            [](const Marker& m) { return m.name == "dragged"; }));
        // Played again and stopped again while the pass waits for a drag: written at the first stop (Latch writes to
        // the stop), nothing more.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Latch));
        const Param before = a1().volumeAuto;
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 20; ++f) {
            if (f == 5) {
                fader->setSliderDown(true);
                fader->setValue(-150);
            }
            if (f == 8) fader->setSliderDown(false);
            mixer->playbackPosition(f);
        }
        state()->beginGesture("Drag 3");
        mixer->playbackStopped(20);
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 40; ++f) mixer->playbackPosition(f);
        mixer->playbackStopped(40);
        state()->endGesture(false);
        QTRY_VERIFY(!mixer->recordingAutomation());
        QCOMPARE(a1().volumeAuto.at(15), -15.0);
        QCOMPARE(a1().volumeAuto.at(30), before.at(30));  // not written on to the second stop
        // An insert parameter the effect does not store yet keeps its default (not 0) when the pass follows an edit.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Touch));
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 40; ++f) {
            if (f == 5) QVERIFY(state()->captureParameter({track, eq, "high_hz", 6000, 5000, 1, false, {}}));
            if (f == 8) QVERIFY(state()->edit("Rename 3", [](Project&, Sequence& s) {
                s.audioTracks[0].name = "Dialogue 3";
                return true;
            }));
            mixer->playbackPosition(f);
            if (f == 6) QCOMPARE(*state()->liveParameter(track, eq, "high_hz"), 6000.0);
            if (f == 39) QCOMPARE(*state()->liveParameter(track, eq, "high_hz"), 5000.0);  // let go: back to its default
        }
        mixer->playbackStopped(40);
        // One moved less than its thinning tolerance writes no keys and is left at its default, not 0.
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 10; ++f) {
            if (f == 3) QVERIFY(state()->captureParameter({track, eq, "mid_hz", 1000.5, 1000, 1, false, {}}));
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(10);
        {
            Sequence now = *state()->sequence();
            const Effect* fx = edit::ownedEffect(now, track, eq);
            QVERIFY(fx);
            QVERIFY(!fx->params.count("mid_hz") || !fx->params.at("mid_hz").animated());
            QCOMPARE(fx->p("mid_hz", 5, 1000), 1000.0);
        }
        // A pass that wrote nothing leaves a gesture alone at the stop.
        mixer->playbackStarted(0);
        state()->beginGesture("Drag 2");
        mixer->playbackStopped(5);
        QVERIFY(state()->inGesture() && !mixer->recordingAutomation());
        state()->endGesture(false);
        // A track in Read takes nothing.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Read));
        mixer->playbackStarted(0);
        QVERIFY(!state()->captureParameter({track, eq, "mid_db", 3, 0, 0.048, false, {}}));
        mixer->playbackStopped(5);
    }

    void surroundPositionAutomation() {
        loadDemo();
        state()->edit("5.1", [](Project&, Sequence& s) {
            s.audioLayout = "5.1";
            s.audioTracks[0].output = 0;
            return true;
        });
        auto* mixer = win_->findChild<MixerPanel*>();
        QVERIFY(mixer);
        QTRY_VERIFY(mixer->trackSurround(0) && !mixer->trackSurround(0)->isHidden());
        SurroundPanner* panner = mixer->trackSurround(0);
        auto a1 = [&]() -> const Track& { return state()->sequence()->audioTracks.at(0); };
        const SurroundPan still = a1().surround;
        // Animate Position keys it where it is, at the playhead.
        state()->setPlayhead(10);
        emit panner->animateRequested(true);
        QVERIFY(surroundAnimated(a1()));
        QVERIFY(a1().surroundXAuto.keyAt(10) && a1().surroundYAuto.keyAt(10) && a1().surroundZAuto.keyAt(10));
        QVERIFY(panner->isAnimated());
        // Moved, it is keyed at the playhead; its own position stays.
        state()->setPlayhead(50);
        SurroundPan right = panner->pan();
        right.x = 0.5, right.y = std::sqrt(0.75);
        panner->setPan(right);
        emit panner->changed(right, true);
        QVERIFY(a1().surroundXAuto.keyAt(50));
        QCOMPARE(a1().surroundXAuto.keyAt(50)->v, 0.5);
        QCOMPARE(a1().surround.x, still.x);
        // The panner follows the playhead.
        state()->setPlayhead(30);
        QVERIFY(std::fabs(panner->pan().x - (still.x + 0.5) / 2) < 1e-6);
        state()->setPlayhead(0);
        QCOMPARE(panner->pan().x, still.x);
        // A Write pass records the panner: where it was, then hard right from frame 20; Write hands over to Touch.
        mixer->trackAutomationMode(0)->setCurrentIndex(int(AutomationMode::Write));
        mixer->playbackStarted(0);
        QVERIFY(mixer->recordingAutomation());
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 20) {
                SurroundPan p = panner->pan();
                p.x = 1, p.y = 0;
                panner->setPan(p);
            }
            mixer->playbackPosition(f);
            if (f == 30) QCOMPARE(mixer->heard().audioTracks.at(0).surround.x, 1.0);  // heard as it is written
        }
        mixer->playbackStopped(60);
        QVERIFY(surroundAnimated(a1()));
        QVERIFY(std::fabs(a1().surroundXAuto.at(10) - still.x) < 1e-6);
        QCOMPARE(a1().surroundXAuto.at(40), 1.0);
        QCOMPARE(a1().surroundYAuto.at(40), 0.0);
        QCOMPARE(a1().automation, int(AutomationMode::Touch));
        QCOMPARE(a1().surround.x, still.x);
        // One undo step back to the keys made by hand.
        state()->undo();
        QVERIFY(a1().surroundXAuto.keyAt(50) && a1().surroundXAuto.keyAt(50)->v == 0.5);
        state()->redo();
        // A Touch pass writes only while the panner is held.
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 60; ++f) {
            if (f == 5) QTest::mousePress(panner, Qt::LeftButton, {}, panner->toWidget(-1, 0).toPoint());
            if (f == 15) QTest::mouseRelease(panner, Qt::LeftButton, {}, panner->toWidget(-1, 0).toPoint());
            mixer->playbackPosition(f);
        }
        mixer->playbackStopped(60);
        QVERIFY2(a1().surroundXAuto.at(10) < -0.9, qPrintable(QString::number(a1().surroundXAuto.at(10))));
        QVERIFY(std::fabs(a1().surroundXAuto.at(2) - still.x) < 1e-6);                 // before it was held
        QVERIFY(a1().surroundXAuto.at(50) > a1().surroundXAuto.at(20) + 0.1);           // let go: gliding back
        // During a pass another track's panner and Animate Position wait for the stop, while a width
        // change on the track being recorded (the wheel: not a drag) is kept, all as one undo step.
        state()->edit("A2 out", [](Project&, Sequence& s) {
            s.audioTracks.at(1).output = 0;
            return true;
        });
        QTRY_VERIFY(mixer->trackSurround(1) && !mixer->trackSurround(1)->isHidden());
        const SurroundPan a2Was = state()->sequence()->audioTracks.at(1).surround;
        const double widthWas = a1().surround.width;
        const int notch = widthWas > 0.5 ? -120 : 120;
        mixer->playbackStarted(0);
        for (FrameTime f = 0; f <= 30; ++f) {
            if (f == 10) {
                SurroundPan moved = a2Was;
                moved.x = -0.7;
                emit mixer->trackSurround(1)->changed(moved, true);
                emit mixer->trackSurround(1)->animateRequested(true);
            }
            if (f == 12) {
                const QPointF c(panner->width() / 2.0, panner->height() / 2.0);
                QWheelEvent wheel(c, panner->mapToGlobal(c), QPoint(), QPoint(0, notch), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(panner, &wheel);
                QVERIFY(panner->isDragging());  // held a moment, so Touch writes it
            }
            mixer->playbackPosition(f);
            if (f == 11) QVERIFY(mixer->recordingAutomation());
        }
        mixer->playbackStopped(30);
        QVERIFY(state()->sequence()->audioTracks.at(1).surround == a2Was);
        QVERIFY(!surroundAnimated(state()->sequence()->audioTracks.at(1)));
        const double widened = std::clamp(widthWas + (notch > 0 ? 0.1 : -0.1), 0.0, 1.0);
        QVERIFY2(std::fabs(a1().surround.width - widened) < 1e-9, qPrintable(QString::number(a1().surround.width)));
        QVERIFY2(a1().surroundXAuto.at(10) < -0.9, qPrintable(QString::number(a1().surroundXAuto.at(10))));  // the last pass's lane kept
        state()->undo();
        QVERIFY(std::fabs(a1().surround.width - widthWas) < 1e-9);
        state()->redo();
        // Stopping the animation leaves it where it is at the playhead.
        state()->setPlayhead(40);
        const double here = a1().surroundXAuto.at(40);
        emit panner->animateRequested(false);
        QVERIFY(!surroundAnimated(a1()));
        QCOMPARE(a1().surround.x, here);
        QVERIFY(!panner->isAnimated());
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
        QVERIFY(combo && combo->count() == 6);
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
        // Out of Gamut on a Rec.709 sequence: nothing it delivers is outside Rec.709.
        combo->setCurrentIndex(int(ExposureView::OutOfGamut709));
        win_->findChild<PlaybackController*>("programPlayback")->requestFrame();
        QTest::qWait(300);
        QCOMPARE(program->viewer()->shownImage(), program->viewer()->image().convertToFormat(QImage::Format_RGB32));
        combo->setCurrentIndex(0);
    }

    void gamutScopeAndChecks() {
        // A Rec.2020 frame: the left half its pure green (outside Rec.709 and P3), the right half grey.
        QImage frame(160, 90, QImage::Format_RGBA64);
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x)
                frame.setPixelColor(x, y, x < 80 ? QColor::fromRgba64(0, 65535, 0, 65535) : QColor::fromRgba64(30000, 30000, 30000, 65535));
        ScopesWidget scopes;
        scopes.resize(640, 420);
        scopes.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scopes));
        scopes.setMode(ScopesWidget::Mode::Chromaticity);
        auto* cie = scopes.findChild<QComboBox*>("scopeCie");
        QVERIFY(cie && cie->isVisible());
        scopes.setSignal(frame, 0, "rec2020", 1000);
        QTRY_VERIFY(scopes.outsideRec709() >= 0);
        QVERIFY2(std::fabs(scopes.outsideRec709() - 0.5) < 0.01 && std::fabs(scopes.outsideP3() - 0.5) < 0.01,
                 qPrintable(QString("%1 %2").arg(scopes.outsideRec709()).arg(scopes.outsideP3())));
        // Plotted where the colours are: Rec.2020's green at (0.170, 0.797), grey at D65; in u'v' too.
        const int half = 160 * 90 / 2;
        QVERIFY(scopes.cieSamplesNear(0.170, 0.797, 0.01) > half * 9 / 10);
        QVERIFY(scopes.cieSamplesNear(0.3127, 0.3290, 0.01) > half * 9 / 10);
        cie->setCurrentIndex(1);
        QVERIFY(scopes.cieUv());
        const Xy greenUv = xyToUv({0.170, 0.797});
        QVERIFY(scopes.cieSamplesNear(greenUv.x, greenUv.y, 0.01) > half * 9 / 10);
        QVERIFY(scopes.cieSamplesNear(0.170, 0.797, 0.01) < half / 10);
        QVERIFY(!scopes.grab().toImage().isNull());
        // Rec.709 frames are all inside.
        QImage sdr(160, 90, QImage::Format_RGB32);
        sdr.fill(qRgb(255, 0, 0));
        scopes.setFrame(sdr, 1);
        QTRY_COMPARE(scopes.outsideRec709(), 0.0);
        QCOMPARE(scopes.outsideP3(), 0.0);
        scopes.setMode(ScopesWidget::Mode::Waveform);
        QVERIFY(!cie->isVisible());

        // Out of gamut on a monitor: the green striped, the grey left; only with the frame's own signal.
        QImage picture(160, 90, QImage::Format_RGB32);
        picture.fill(qRgb(90, 90, 90));
        const QImage marked = gamutView(picture, frame, "rec2020", Primaries::Bt709);
        int left = 0, right = 0;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) (x < 80 ? left : right) += marked.pixel(x, y) != picture.pixel(x, y);
        QCOMPARE(left, half);
        QCOMPARE(right, 0);
        QCOMPARE(gamutView(picture, frame, "rec2020", Primaries::Bt2020), picture);
        ViewerWidget viewer;
        viewer.setImage(picture, 5);
        viewer.setSignal(frame, 5, "rec2020");
        viewer.setExposureView(int(ExposureView::OutOfGamut709));
        QCOMPARE(viewer.shownImage(), marked);
        viewer.setSignal(frame, 6, "rec2020");  // (another frame's)
        QCOMPARE(viewer.shownImage(), picture);
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
        // A PDF with a picture of each marker's frame (Red at 10, Blue at 40... both colour mattes, so at least one image).
        const QString pdf = dir_.path() + "/markers.pdf";
        QVERIFY(win_->exportMarkers(pdf));
        {
            QFile f(pdf);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray data = f.readAll();
            QVERIFY(data.startsWith("%PDF-"));
            QVERIFY(data.contains("/Subtype /Image") || data.contains("/Subtype/Image"));
            QVERIFY(data.contains("/Type /Page") || data.contains("/Type/Page"));
        }
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
        win_->activateWindow();  // window shortcuts reach only the active window (a busy machine can be slow to give it)
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
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
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
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
        for (auto* sp : win_->findChild<InspectorWidget*>()->findChildren<QDoubleSpinBox*>())
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
            QCOMPARE(stems->count(), 4);  // none, tracks, buses, roles
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

    void immersiveMixerAndAdmExport() {
        loadDemo();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        for (int t = 0; t < 2; ++t)
            QVERIFY(state()->apply("Place", [media, t](Project& p, Sequence& s) {
                return edit::placeMedia(p, s, media, 0, 0, 60, V1, {TrackKind::Audio, t}, false);
            }));
        // Sequence settings offer the immersive layouts.
        {
            SequenceSettingsDialog dlg(win_.get());
            auto* layout = dlg.findChild<QComboBox*>("audioLayout");
            for (const char* l : {"5.1.2", "5.1.4", "7.1.2", "7.1.4"}) QVERIFY2(layout->findData(QString(l)) >= 0, l);
            layout->setCurrentIndex(layout->findData(QString("7.1.4")));
            QCOMPARE(dlg.spec().audioLayout, std::string("7.1.4"));
        }
        QVERIFY(state()->edit("7.1.4", [](Project&, Sequence& s) {
            s.audioLayout = "7.1.4";
            return true;
        }));
        QApplication::processEvents();
        auto* mixer = win_->findChild<MixerPanel*>();
        std::vector<SurroundPanner*> panners;
        for (auto* p : mixer->findChildren<SurroundPanner*>("surroundPanner"))
            if (p->isVisibleTo(mixer)) panners.push_back(p);
        QVERIFY(panners.size() >= 2);
        // Alt+wheel raises the second track; the plain wheel still narrows it.
        SurroundPanner* panner = panners[1];
        QWheelEvent up(panner->rect().center(), panner->mapToGlobal(panner->rect().center()), {}, {0, 120}, Qt::NoButton, Qt::AltModifier,
                       Qt::NoScrollPhase, false);
        QApplication::sendEvent(panner, &up);
        QApplication::sendEvent(panner, &up);
        QVERIFY(std::fabs(state()->sequence()->audioTracks.at(1).surround.z - 0.2) < 1e-9);
        QWheelEvent narrow(panner->rect().center(), panner->mapToGlobal(panner->rect().center()), {}, {0, -120}, Qt::NoButton, {},
                           Qt::NoScrollPhase, false);
        QApplication::sendEvent(panner, &narrow);
        QVERIFY(std::fabs(state()->sequence()->audioTracks.at(1).surround.width - 0.9) < 1e-9);
        // Made an object, it is written as one: a 7.1.4 bed (12 channels) and A2.
        QVERIFY(state()->edit("Object", [](Project&, Sequence& s) {
            s.audioTracks[1].surround.object = true;
            return true;
        }));
        QVERIFY(win_->findChild<QAction*>("exportAdm"));
        const QString path = dir_.filePath("immersive.wav");
        AdmSettings st;
        st.title = "App master";
        AdmResult r;
        QVERIFY(win_->exportAdmTo(path, st, &r));
        QVERIFY(r.bedChannels == 12 && r.objects == 1 && r.bedPack == "AP_00010017");
        BwfInfo info;
        QVERIFY(readBwfInfo(path.toStdString(), info) && info.channels == 13 && info.chna.size() == 13);
        QVERIFY(info.axml.find("audioObjectName=\"A2\"") != std::string::npos);
        state()->newProject();
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
        // Sound only, from its own menu item.
        QVERIFY(win_->findChild<QAction*>("exportAafSound"));
        QVERIFY(win_->exportAafTo(dir_.path() + "/sound.aaf", &summary, false));
        QVERIFY2(summary.contains("1 audio tracks") && !summary.contains("video"), qPrintable(summary));
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

    void proxyMediaFromTheBin() {
        // A camera clip with sound.
        const QString root = dir_.path() + "/proxies";
        QDir(root).removeRecursively();
        QVERIFY(QDir().mkpath(root + "/card"));
        const QString video = root + "/card/B001.mp4";
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 320, gs.height = 180;
            MediaItem src;
            src.id = gen.newId();
            std::string err;
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", src, &err));
            gen.media.push_back(src);
            Clip sound = makeClip(gen, src, TrackKind::Audio, gs);
            sound.duration = 30;
            edit::overwrite(gen, gs, {TrackKind::Audio, 0}, sound);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "bars", 30));
            ExportSettings st;
            st.path = video.toStdString();
            st.preset = "ultrafast";
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        const Id id = ids[0];
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin);

        // The options: ProRes Proxy, half size, with sound, in a Proxy folder beside the original; remembered.
        bin->proxyDialogHook_ = [](QDialog* d) {
            auto* format = d->findChild<QComboBox*>("proxyFormat");
            format->setCurrentIndex(format->findData("prores_proxy"));
            auto* size = d->findChild<QComboBox*>("proxySize");
            size->setCurrentIndex(size->findData(0));
            d->findChild<QCheckBox*>("proxySound")->setChecked(true);
            auto* where = d->findChild<QComboBox*>("proxyWhere");
            where->setCurrentIndex(where->findData("beside"));
            QMetaObject::invokeMethod(d, "accept", Qt::QueuedConnection);
        };
        const auto options = bin->proxyOptionsDialog();
        QVERIFY(options);
        QVERIFY(options->format == ProxyFormat::ProResProxy && options->maxWidth == 0 && options->sound);
        QString shown;
        bin->proxyDialogHook_ = [&shown](QDialog* d) {
            shown = d->findChild<QComboBox*>("proxyFormat")->currentData().toString();
            QMetaObject::invokeMethod(d, "reject", Qt::QueuedConnection);
        };
        QVERIFY(!bin->proxyOptionsDialog());
        QCOMPARE(shown, QString("prores_proxy"));
        bin->proxyDialogHook_ = nullptr;

        // Made in the background and attached in one undo step, nothing half written left behind.
        bin->createProxies({id}, *options);
        const QString proxy = root + "/card/Proxy/B001_Proxy.mov";
        QTRY_COMPARE_WITH_TIMEOUT(state()->project().findMedia(id)->proxyPath, proxy.toStdString(), 60000);
        QVERIFY(QFileInfo::exists(proxy));
        QCOMPARE(QDir(root + "/card/Proxy").entryList(QDir::Files), QStringList{"B001_Proxy.mov"});
        MediaItem made;
        QVERIFY(probeMedia(proxy.toStdString(), made));
        QVERIFY(made.width == 160 && made.hasAudio && made.videoCodec == "prores");
        state()->undo();
        QVERIFY(state()->project().findMedia(id)->proxyPath.empty());

        // Or the camera's own proxies, found in a folder by name and length.
        QCOMPARE(bin->attachProxiesFrom({id}, root + "/card/Proxy"), 1);
        QCOMPARE(state()->project().findMedia(id)->proxyPath, QFileInfo(proxy).absoluteFilePath().toStdString());
        QCOMPARE(bin->attachProxiesFrom({id}, root + "/nowhere"), 0);
        QVERIFY(state()->save(root + "/cut.montage"));
        state()->newProject();

        // The originals offline: the project opens on its proxies, without asking where the files went.
        bool renamed = false;
        QTRY_VERIFY_WITH_TIMEOUT(renamed || (renamed = QDir().rename(root + "/card/B001.mp4", root + "/B001-away.mp4")), 10000);
        QVERIFY(win_->openProject(root + "/cut.montage"));
        QVERIFY(!win_->findChild<LinkMediaDialog*>("linkMedia"));
        QVERIFY(win_->statusBar()->currentMessage().contains("playing from their proxies"));
        QVERIFY(state()->isMediaOffline(id));
        // A proxy without sound for a file with sound, or no proxy at all, is still asked about.
        state()->newProject();
        bool removed = false;
        QTRY_VERIFY_WITH_TIMEOUT(removed || (removed = QFile::remove(proxy)), 10000);
        {
            ProxyOptions silent;
            silent.format = ProxyFormat::ProResProxy;
            silent.maxWidth = 0;
            std::string err;
            QVERIFY2(createProxy((root + "/B001-away.mp4").toStdString(), proxy.toStdString(), silent, {}, nullptr, &err), err.c_str());
        }
        QVERIFY(win_->openProject(root + "/cut.montage"));
        auto* dlg = win_->findChild<LinkMediaDialog*>("linkMedia");
        QVERIFY(dlg && dlg->isVisible());
        dlg->close();
        state()->newProject();
        removed = false;
        QTRY_VERIFY_WITH_TIMEOUT(removed || (removed = QFile::remove(proxy)), 10000);
        QVERIFY(win_->openProject(root + "/cut.montage"));
        dlg = win_->findChild<LinkMediaDialog*>("linkMedia");
        QVERIFY(dlg && dlg->isVisible());
        dlg->close();

        // Two files of the same name (A.mp4 and A.mov) each get their own proxy beside them, made in one go.
        state()->newProject();
        QVERIFY(QDir().mkpath(root + "/card2"));
        QVERIFY(QFile::copy(root + "/B001-away.mp4", root + "/card2/A.mp4"));
        QVERIFY(QFile::copy(root + "/B001-away.mp4", root + "/card2/A.mov"));
        const auto pair = state()->importFiles({root + "/card2/A.mp4", root + "/card2/A.mov"});
        QCOMPARE(pair.size(), size_t(2));
        bin->createProxies(pair, *options);
        QTRY_VERIFY_WITH_TIMEOUT(!state()->project().findMedia(pair[0])->proxyPath.empty() && !state()->project().findMedia(pair[1])->proxyPath.empty(), 60000);
        const std::string pa = state()->project().findMedia(pair[0])->proxyPath, pb = state()->project().findMedia(pair[1])->proxyPath;
        QVERIFY2(pa != pb, pa.c_str());
        QVERIFY(QFileInfo::exists(QString::fromStdString(pa)) && QFileInfo::exists(QString::fromStdString(pb)));
        QCOMPARE(QDir(root + "/card2/Proxy").entryList(QDir::Files).size(), 2);
        appSettings().setValue(QStringLiteral("proxy/where"), "cache");
        appSettings().setValue(QStringLiteral("proxy/format"), "h264");
        appSettings().setValue(QStringLiteral("proxy/sound"), false);
        state()->newProject();
    }

    void mergeClipsAndSyncDailies() {
        // A camera clip whose own sound is JFK's speech from two seconds in, and the speech as the recorder's file.
        const QString video = dir_.path() + "/A002.mp4";
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            Clip picture = makeGeneratorClip(gen, "color", 120);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, picture);
            MediaItem src;
            src.id = gen.newId();
            std::string err;
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", src, &err));
            gen.media.push_back(src);
            Clip scratch = makeClip(gen, src, TrackKind::Audio, gs);
            scratch.sourceIn = 60;  // two seconds in at 30 fps
            scratch.duration = 120;
            edit::overwrite(gen, gs, {TrackKind::Audio, 0}, scratch);
            ExportSettings st;
            st.path = video.toStdString();
            st.preset = "ultrafast";
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video, QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(2));
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin);
        const size_t before = state()->project().media.size();
        // Sync Dailies: no timecode, so by the sound; one undo step.
        QStringList report;
        const auto made = bin->syncDailies(ids, true, &report);
        QCOMPARE(made.size(), size_t(1));
        QVERIFY2(report.join(" ").contains("waveform"), qPrintable(report.join(" ")));
        const MediaItem* merged = state()->project().findMedia(made[0]);
        QVERIFY(merged && isMergedClip(state()->project(), made[0]));
        const Sequence* inside = state()->project().findSequence(merged->sequenceId);
        QVERIFY(inside && inside->audioTracks.size() == 2 && inside->audioTracks[1].muted);
        const Clip& sound = inside->audioTracks[0].clips.at(0);
        QVERIFY2(std::abs(FrameTime(sound.sourceIn) - 60) <= 1, qPrintable(QString::number(sound.sourceIn)));  // two seconds in
        QCOMPARE(state()->project().media.size(), before + 1);
        state()->undo();
        QCOMPARE(state()->project().media.size(), before);
        // Merge Clips by their starts, without the camera's sound; by timecode refused with why.
        QString why;
        const Id starts = bin->mergeClips(ids[0], {ids[1]}, 3, false, "Starts", &why);
        QVERIFY2(starts, qPrintable(why));
        QCOMPARE(state()->project().findMedia(starts)->name, std::string("Starts"));
        const Sequence* s2 = state()->project().findSequence(state()->project().findMedia(starts)->sequenceId);
        QCOMPARE(s2->audioTracks.size(), size_t(1));
        QCOMPARE(FrameTime(s2->audioTracks[0].clips.at(0).sourceIn), FrameTime(0));
        QVERIFY(!bin->mergeClips(ids[0], {ids[1]}, 1, true, {}, &why));
        QVERIFY2(why.contains("timecode"), qPrintable(why));
        state()->newProject();
    }

    void timecodeFromLtcInTheBin() {
        // A recorder's file with a timecode generator's LTC (from 10:00:00:00, 25 fps) on its second channel, and one without.
        const int sr = 48000;
        const std::vector<float> ltc = encodeLtc(ltcFrameNumber({10, 0, 0, 0}, 25), 100, {25, 1}, false, sr);
        std::vector<float> withLtc(ltc.size() * 2), without(ltc.size() * 2);
        for (size_t i = 0; i < ltc.size(); ++i) {
            withLtc[i * 2] = without[i * 2] = without[i * 2 + 1] = float(0.3 * std::sin(2 * M_PI * 440 * double(i) / sr));
            withLtc[i * 2 + 1] = ltc[i];
        }
        const QString a = dir_.path() + "/ltc-a.wav", b = dir_.path() + "/ltc-b.wav";
        std::string err;
        QVERIFY(writeStereoWav(a.toStdString(), withLtc, sr, &err) && writeStereoWav(b.toStdString(), without, sr, &err));
        state()->newProject();
        const auto ids = state()->importFiles({a, b});
        QCOMPARE(ids.size(), size_t(2));
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin);
        state()->edit("place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        });
        // Read in one undo step: the file with LTC takes it, its clip stops playing it; the other is left as it was.
        QCOMPARE(bin->timecodeFromLtc(ids), 1);
        const MediaItem* m = state()->project().findMedia(ids[0]);
        QVERIFY(usesLtcTimecode(*m) && std::fabs(m->timecode - 36000) < 1e-5 && m->ltcChannel == 1);
        QVERIFY(state()->project().findMedia(ids[1])->timecode < 0);
        QCOMPARE(state()->sequence()->audioTracks[0].clips[0].channels, std::vector<int>{0});
        state()->undo();
        QVERIFY(!usesLtcTimecode(*state()->project().findMedia(ids[0])));
        QVERIFY(state()->sequence()->audioTracks[0].clips[0].channels.empty());
        // Sequence > Add LTC Track: the sequence's timecode on a new muted track, in a Timecode folder beside the project.
        QVERIFY(state()->save(dir_.path() + "/ltc.montage"));
        const size_t tracks = state()->sequence()->audioTracks.size();
        auto* add = win_->findChild<QAction*>("addLtcTrack");
        QVERIFY(add);
        add->trigger();
        const Sequence* s = state()->sequence();
        QCOMPARE(s->audioTracks.size(), tracks + 1);
        QVERIFY(s->audioTracks.back().name == "LTC" && s->audioTracks.back().muted && s->audioTracks.back().clips.size() == 1);
        LtcReading r;
        const std::string written = state()->project().findMedia(s->audioTracks.back().clips[0].mediaId)->path;
        QVERIFY2(QString::fromStdString(written).startsWith(dir_.path() + "/Timecode/"), written.c_str());
        QVERIFY2(readLtc(written, r, &err), err.c_str());
        QVERIFY(std::fabs(r.start) < 1e-5);
        state()->newProject();
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

    void speedRampMenu() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        }));
        const Clip& placed = state()->sequence()->audioTracks[0].clips.at(0);
        const Id id = placed.id;
        const double extent = placed.sourceExtent();
        state()->setSelection({id}, false);
        QAction* bullet = win_->findChild<QAction*>("ramp_bullet");
        QVERIFY(bullet && win_->findChild<QAction*>("ramp_none"));
        bullet->trigger();
        const Clip* c = edit::clipById(*state()->sequence(), id);
        QVERIFY(c->ramped());
        QVERIFY(std::fabs(c->sourceExtent() - extent) < 1e-6);
        QVERIFY(c->speedAt(c->duration / 2.0) < 0.2 * c->speedAt(0));
        QCOMPARE(state()->undoText(), tr("Speed Ramp: Bullet"));
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), id)->ramped());
        // Nothing selected: nothing changes.
        state()->setSelection({}, false);
        bullet->trigger();
        QVERIFY(!edit::clipById(*state()->sequence(), id)->ramped());
        state()->newProject();
    }

    void vfxPullsNeedASelection() {
        loadDemo();
        state()->setSelection({}, false);
        QAction* pulls = win_->findChild<QAction*>("vfxPulls");
        QVERIFY(pulls);
        pulls->trigger();  // nothing selected: no dialog, a hint instead
        QTRY_VERIFY(win_->statusBar()->currentMessage().contains("Select the shots"));
    }

    void fullScreenVideoOutput() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("fullScreenProgram") && win_->findChild<QMenu*>("videoOutputMenu"));
        // On the first screen: full screen, the Program picture fitted in black, following the playhead.
        CleanFeedWindow* feed = win_->showCleanFeed(0);
        QVERIFY(feed && feed->isVisible() && feed->isFullScreen());
        QCOMPARE(feed->geometry().size(), QGuiApplication::screens().at(0)->geometry().size());
        QTRY_VERIFY(!feed->frame().isNull());
        const QRect pic = feed->pictureRect();
        QVERIFY(std::abs(double(pic.width()) / pic.height() - 16.0 / 9) < 0.02);
        QVERIFY(feed->rect().contains(pic));
        state()->setPlayhead(10);  // the red clip
        QTRY_VERIFY(qRed(feed->frame().pixel(feed->frame().width() / 2, feed->frame().height() / 2)) > 200);
        state()->setPlayhead(70);  // the blue one
        QTRY_VERIFY(qBlue(feed->frame().pixel(feed->frame().width() / 2, feed->frame().height() / 2)) > 200);
        // What it draws: black around the picture, the picture in the middle.
        const QImage shot = feed->grab().toImage();
        const QPoint mid = pic.center();
        QVERIFY(qBlue(shot.pixel(mid)) > 200);
        if (pic.top() > 2) QCOMPARE(shot.pixel(mid.x(), 0) & 0xffffff, 0u);
        // Esc closes it; the editor's keys go to the main window.
        QTest::keyClick(feed, Qt::Key_Escape);
        QVERIFY(!feed->isVisible());
        // The menu action toggles it.
        win_->findChild<QAction*>("fullScreenProgram")->trigger();
        QVERIFY(win_->cleanFeed()->isVisible());
        win_->findChild<QAction*>("fullScreenProgram")->trigger();
        QVERIFY(!win_->cleanFeed()->isVisible());
        state()->setPlayhead(0);
    }

    void monitorCalibration() {
        loadDemo();
        MonitorPanel* program = nullptr;
        for (MonitorPanel* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTemporaryDir dir;
        const QString lutPath = dir.filePath(QStringLiteral("half.cube"));
        {
            QFile f(lutPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("LUT_3D_SIZE 2\n0 0 0\n0.5 0 0\n0 0.5 0\n0.5 0.5 0\n0 0 0.5\n0.5 0 0.5\n0 0.5 0.5\n0.5 0.5 0.5\n");
        }
        DisplayCalibration& cal = DisplayCalibration::instance();
        cal.setBypassed(false);
        const QString screen = DisplayCalibration::screenTarget(viewer->screen());
        QString why;
        QVERIFY(!cal.setLutPath(screen, dir.filePath(QStringLiteral("missing.cube")), &why) && !why.isEmpty());
        QVERIFY2(cal.setLutPath(screen, lutPath, &why), qPrintable(why));
        // The viewer draws its picture through its screen's LUT; the picture itself (the scopes', the renders') is as it was.
        state()->setPlayhead(10);  // the red clip
        QTRY_VERIFY(!viewer->image().isNull() && qRed(viewer->image().pixel(viewer->image().width() / 2, viewer->image().height() / 2)) > 200);
        const QPoint mid(viewer->image().width() / 2, viewer->image().height() / 2);
        const int red = qRed(viewer->image().pixel(mid));
        QVERIFY(std::abs(qRed(viewer->drawnImage().pixel(mid)) - red / 2) <= 1);
        // The full-screen output on that screen too.
        CleanFeedWindow* feed = win_->showCleanFeed(0);
        QTRY_VERIFY(!feed->frame().isNull());
        const QPoint fmid(feed->frame().width() / 2, feed->frame().height() / 2);
        if (QGuiApplication::screens().at(0) == viewer->screen())
            QVERIFY(std::abs(qRed(feed->drawnFrame().pixel(fmid)) - qRed(feed->frame().pixel(fmid)) / 2) <= 1);
        feed->close();
        // Bypassed: as it was.
        cal.setBypassed(true);
        QCOMPARE(qRed(viewer->drawnImage().pixel(mid)), red);
        cal.setBypassed(false);
        QVERIFY(cal.screenLut(viewer->screen()) && !cal.screenLut(viewer->screen(), true));

        // The dialog: each screen's LUTs, cleared there.
        win_->findChild<QAction*>("monitorCalibrationAction")->trigger();
        auto* dlg = win_->findChild<MonitorCalibrationDialog*>("monitorCalibration");
        QVERIFY(dlg);
        auto* file = dlg->findChild<QLabel*>(QStringLiteral("lutFile:") + screen);
        QVERIFY(file);
        QCOMPARE(file->text(), QStringLiteral("half.cube"));
        dlg->findChild<QPushButton*>(QStringLiteral("lutClear:") + screen)->click();
        QVERIFY(cal.lutPath(screen).isEmpty());
        QCOMPARE(qRed(viewer->drawnImage().pixel(mid)), red);
        QTRY_COMPARE(dlg->findChild<QLabel*>(QStringLiteral("lutFile:") + screen)->text(), QStringLiteral("None"));

        // The patch generator, measured through by calibration software (a stand-in listening here).
        QTcpServer software;
        QVERIFY(software.listen(QHostAddress::LocalHost));
        auto frame = [](const QByteArray& xml) {
            QByteArray out(4, '\0');
            const quint32 n = quint32(xml.size());
            out[0] = char(n >> 24), out[1] = char(n >> 16 & 255), out[2] = char(n >> 8 & 255), out[3] = char(n & 255);
            return out + xml;
        };
        dlg->findChild<QLineEdit*>("patchHost")->setText(QStringLiteral("127.0.0.1"));
        dlg->findChild<QSpinBox*>("patchPort")->setValue(software.serverPort());
        auto* target = dlg->findChild<QComboBox*>("patchTarget");
        target->setCurrentIndex(0);
        auto* startStop = dlg->findChild<QPushButton*>("patchStartStop");
        auto* status = dlg->findChild<QLabel*>("patchStatus");
        auto* gen = win_->findChild<PatchGenerator*>();
        QVERIFY(gen);
        startStop->click();
        QVERIFY(gen->active());
        QVERIFY(gen->window() && gen->window()->isVisible() && gen->window()->isFullScreen());
        QTRY_VERIFY(software.hasPendingConnections() || software.waitForNewConnection(100));
        QTcpSocket* client = software.nextPendingConnection();
        QVERIFY(client);
        QTRY_VERIFY(status->text().contains("waiting for the first patch"));
        client->write(frame("<?xml version=\"1.0\" encoding=\"UTF-8\" ?><calibration><color red=\"940\" green=\"64\" blue=\"64\" bits=\"10\"/>"
                            "<background red=\"0\" green=\"0\" blue=\"0\" bits=\"10\"/><geometry x=\"0.4000\" y=\"0.4000\" cx=\"0.2000\" cy=\"0.2000\"/></calibration>"));
        client->flush();
        QTRY_COMPARE(gen->patches(), 1);
        QVERIFY(status->text().contains("R 940 G 64 B 64 (10-bit)"));
        const QImage shown = gen->window()->picture();
        QCOMPARE(qRed(shown.pixel(shown.width() / 2, shown.height() / 2)), int(std::lround(940.0 / 1023 * 255)));
        QCOMPARE(qRed(shown.pixel(2, 2)), 0);
        // The software going away: waited for again, then stopped from the dialog.
        client->disconnectFromHost();
        QTRY_VERIFY(status->text().contains("Waiting for the calibration software"));
        QTRY_VERIFY(software.hasPendingConnections() || software.waitForNewConnection(100));  // (tried again)
        startStop->click();
        QVERIFY(!gen->active() && !gen->window()->isVisible());
        QCOMPARE(status->text(), QStringLiteral("Off"));
        while (QTcpSocket* stale = software.nextPendingConnection()) stale->deleteLater();  // (its tries)

        // On the reference output (which must be running): its own LUT, and patches in place of the program.
        gen->setReferenceOutput(true);
        QVERIFY(!gen->start(QStringLiteral("127.0.0.1"), software.serverPort(), &why) && why.contains("reference output"));
        QVERIFY(!gen->active());
        target->setCurrentIndex(target->findData(-1));
        qputenv("MONTAGE_VIRTUAL_OUTPUT", "1");
        auto* controller = win_->findChild<ReferenceOutputController*>();
        const output::Config saved = ReferenceOutputController::loadConfig();
        output::Config cfg = saved;
        cfg.backend = "Virtual";
        cfg.device = "";
        cfg.mode = "";  // (the sequence's)
        cfg.depth = 10;
        QVERIFY2(controller->start(cfg, &why), qPrintable(why));
        QVERIFY2(cal.setLutPath(controller->calibrationTarget(), lutPath, &why), qPrintable(why));
        QVERIFY(controller->scheduler()->calibration());
        QVERIFY(controller->status().contains("calibrated"));
        QTRY_VERIFY(dlg->findChild<QLabel*>(QStringLiteral("lutFile:") + controller->calibrationTarget()));
        auto dev = std::dynamic_pointer_cast<output::VirtualBackend>(controller->backend());
        startStop->click();
        QVERIFY(gen->active() && gen->referenceOutput());
        QTRY_VERIFY(software.hasPendingConnections() || software.waitForNewConnection(100));
        client = software.nextPendingConnection();
        client->write(frame("<calibration><color red=\"1023\" green=\"1023\" blue=\"1023\" bits=\"10\"/></calibration>"));
        client->flush();
        QTRY_COMPARE(gen->patches(), 1);
        // White, not through the LUT (which halves): 10-bit video levels' white.
        auto luma = [](const output::VirtualBackend::Sent& x) {
            const uint32_t w0 = x.data[0] | x.data[1] << 8 | x.data[2] << 16 | uint32_t(x.data[3]) << 24;
            return int(w0 >> 10 & 1023);
        };
        QTRY_VERIFY_WITH_TIMEOUT(!dev->sent().empty() && luma(dev->sent().back()) == 940, 5000);
        startStop->click();
        QTRY_VERIFY_WITH_TIMEOUT(luma(dev->sent().back()) != 940, 5000);  // (the program again)
        controller->stop();
        ReferenceOutputController::saveConfig(saved);
        cal.setLutPath(DisplayCalibration::outputTarget(QStringLiteral("Virtual"), QStringLiteral("virtual")), QString());
        qunsetenv("MONTAGE_VIRTUAL_OUTPUT");
        dlg->close();
        state()->setPlayhead(0);
    }

    void gradeVersionsInInspectorAndMenu() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Grade", [&](Project& p, Sequence& s) {
            Effect cc = makeEffect(p, "color_correct");
            cc.params["saturation"] = Param(0.0);
            edit::clipById(s, red)->effects.push_back(cc);
            return true;
        }));
        state()->setSelection({red}, false);
        QApplication::processEvents();
        auto* versions = win_->findChild<QComboBox*>("gradeVersions");
        QVERIFY(versions && versions->count() == 1 && !versions->isEnabled());
        // A new version from the Inspector's menu: a copy, shown, in one undo step.
        win_->findChild<QAction*>("newGradeVersion")->trigger();
        const Clip* c = clipNamed(*state()->sequence(), "Red");
        QCOMPARE(c->gradeVersions.size(), size_t(2));
        QCOMPARE(c->gradeVersion, 1);
        QApplication::processEvents();
        versions = win_->findChild<QComboBox*>("gradeVersions");
        QVERIFY(versions->isEnabled() && versions->count() == 2 && versions->currentIndex() == 1);
        // Back to version 1 from the combo; Clip > Grade Versions > Next Version on the selection.
        emit versions->activated(0);
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->gradeVersion, 0);
        win_->findChild<QAction*>("gradeNextVersion")->trigger();
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->gradeVersion, 1);
        win_->findChild<QAction*>("gradeNextVersion")->trigger();  // round to the first
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->gradeVersion, 0);
        state()->undo();
        state()->undo();
        state()->undo();
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->gradeVersion, 1);
        state()->undo();
        QVERIFY(clipNamed(*state()->sequence(), "Red")->gradeVersions.empty());
        state()->setSelection({}, false);
    }

    void nodesPanelEditsTheGraph() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Grade", [&](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects = {makeEffect(p, "color_correct"), makeEffect(p, "curves")};
            return true;
        }));
        auto* dock = win_->findChild<QDockWidget*>("nodes");
        QVERIFY(dock);
        dock->setFloating(true);
        dock->resize(700, 320);
        dock->show();
        auto* panel = win_->findChild<GradeNodePanel*>();
        QVERIFY(panel);
        state()->setSelection({red}, false);
        QApplication::processEvents();
        QCOMPARE(panel->clip(), red);
        auto clip = [&] { return edit::clipById(*state()->sequence(), red); };
        // Use Nodes: one node each, in series, the first picked; the Inspector shows that node alone.
        QVERIFY(panel->convert());
        QCOMPARE(clip()->grade.nodes.size(), size_t(2));
        QCOMPARE(panel->selectedNode(), 1);
        QApplication::processEvents();
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector->findChild<QPushButton*>("gradeNodeSettings"));
        QVERIFY(!inspector->findChild<QLabel*>("gradeNodesNote"));
        // A click on node 2 picks it; on nothing, no node (the Inspector then shows them all).
        QTest::mouseClick(panel->canvas(), Qt::LeftButton, {}, panel->nodeCentre(2).toPoint());
        QCOMPARE(panel->selectedNode(), 2);
        QTest::mouseClick(panel->canvas(), Qt::LeftButton, {}, QPoint(4, panel->canvas()->height() - 4));
        QCOMPARE(panel->selectedNode(), 0);
        QApplication::processEvents();
        QVERIFY(inspector->findChild<QLabel*>("gradeNodesNote"));
        // Dragged: moved in one undo step.
        const double x0 = findGradeNode(clip()->grade, 2)->x;
        const QPoint from = panel->nodeCentre(2).toPoint(), to = from + QPoint(30, 20);
        QTest::mousePress(panel->canvas(), Qt::LeftButton, {}, from);
        for (QPoint at : {from + QPoint(10, 5), to}) {
            QMouseEvent m(QEvent::MouseMove, at, panel->canvas()->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(panel->canvas(), &m);
        }
        QTest::mouseRelease(panel->canvas(), Qt::LeftButton, {}, to);
        QVERIFY(findGradeNode(clip()->grade, 2)->x > x0 + 10);
        state()->undo();
        QCOMPARE(findGradeNode(clip()->grade, 2)->x, x0);
        // A parallel node beside node 2 (Alt+P on the canvas, not the menus'), then node 1's key dragged into the new
        // node's key input.
        panel->selectNode(2);
        panel->canvas()->setFocus();
        QTest::keyClick(panel->canvas(), Qt::Key_P, Qt::AltModifier);
        const int branch = panel->selectedNode();
        QCOMPARE(findGradeNode(clip()->grade, branch)->kind, std::string("corrector"));
        QCOMPARE(clip()->grade.nodes.size(), size_t(4));  // and the mixer
        QApplication::processEvents();
        const QPoint keyOut = panel->portPoint(1, true, 0, true).toPoint(), keyIn = panel->portPoint(branch, false, 1, true).toPoint();
        QTest::mousePress(panel->canvas(), Qt::LeftButton, {}, keyOut);
        QMouseEvent m(QEvent::MouseMove, keyIn, panel->canvas()->mapToGlobal(keyIn), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel->canvas(), &m);
        QTest::mouseRelease(panel->canvas(), Qt::LeftButton, {}, keyIn);
        const GradeLink* k = gradeLinkInto(clip()->grade, branch, 1);
        QVERIFY(k && k->from == 1 && k->key);
        QVERIFY(!panel->canvas()->grab().isNull());  // (drawn: nodes, mixer, links, ports)
        // A link that would make a loop is refused, the graph unchanged.
        const GradeGraph before = clip()->grade;
        QVERIFY(!panel->connectNodes({branch + 1, 1, 0, false}));
        QCOMPARE(clip()->grade, before);
        // Switched off and renamed; shared, then used by the Blue clip.
        panel->selectNode(branch);
        QTest::keyClick(panel->canvas(), Qt::Key_D, Qt::ControlModifier);  // (not the timeline's Default Transition)
        QVERIFY(!findGradeNode(clip()->grade, branch)->enabled);
        QVERIFY(panel->renameSelected("Sky"));
        QCOMPARE(gradeNodeName(*findGradeNode(clip()->grade, branch)), std::string("Sky"));
        panel->selectNode(1);
        QVERIFY(panel->shareSelected("Look"));
        QCOMPARE(state()->sequence()->sharedNodes.size(), size_t(1));
        const Id look = state()->sequence()->sharedNodes[0].id;
        const Id blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setSelection({blue}, false);
        QVERIFY(panel->useShared(look));
        QCOMPARE(findGradeNode(edit::clipById(*state()->sequence(), blue)->grade, panel->selectedNode())->shared, look);
        // Delete on the canvas removes the picked node; Flatten turns the graph back into a stack.
        state()->setSelection({red}, false);
        panel->selectNode(branch);
        panel->canvas()->setFocus();
        QTest::keyClick(panel->canvas(), Qt::Key_Delete);
        QVERIFY(!findGradeNode(clip()->grade, branch));
        QVERIFY(panel->flatten());
        QVERIFY(clip()->grade.empty());
        QCOMPARE(clip()->effects.size(), size_t(2));
        dock->setFloating(false);
        dock->hide();
        state()->setSelection({}, false);
    }

    void animateToAudioOnTheSelection() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("animateToAudio"));
        QVERIFY(findEffectInfo("audio_viz"));
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->setSelection({red}, false);
        // No sound under the clip yet: refused, nothing changes.
        QVERIFY(!win_->animateSelectionToAudio(0, "scale", 1, 0, 100, 120));
        auto scaleAnimated = [&] {
            const Clip* c = clipNamed(*state()->sequence(), "Red");
            return c->motion.params.count("scale") && c->motion.params.at("scale").animated();
        };
        QVERIFY(!scaleAnimated());
        // Speech on A1 under it: the scale follows it, in one undo step.
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        state()->setSelection({red}, false);
        QVERIFY(win_->animateSelectionToAudio(0, "scale", 1, 0, 100, 120));
        QVERIFY(scaleAnimated());
        const Clip* c = clipNamed(*state()->sequence(), "Red");
        double lo = 1e9, hi = -1e9;
        for (FrameTime f = 0; f < c->duration; ++f) lo = std::min(lo, c->motion.p("scale", f)), hi = std::max(hi, c->motion.p("scale", f));
        QVERIFY2(lo >= 99.99 && lo < 105 && std::fabs(hi - 120) < 0.01, qPrintable(QString("%1 %2").arg(lo).arg(hi)));
        state()->undo();
        QVERIFY(!scaleAnimated());
        state()->setSelection({}, false);
    }

    void audioChannelsOnTheSelection() {
        // A camera file's four channels (a boom, a lav and two spare), 200-500 Hz.
        const QString wav = dir_.path() + "/four-channels.wav";
        {
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int frames = 48000;
            QByteArray data;
            QDataStream out(&data, QIODevice::WriteOnly);
            out.setByteOrder(QDataStream::LittleEndian);
            out.writeRawData("RIFF", 4);
            out << quint32(36 + frames * 8);
            out.writeRawData("WAVEfmt ", 8);
            out << quint32(16) << quint16(1) << quint16(4) << quint32(48000) << quint32(48000 * 8) << quint16(8) << quint16(16);
            out.writeRawData("data", 4);
            out << quint32(frames * 8);
            for (int i = 0; i < frames; ++i)
                for (int ch = 0; ch < 4; ++ch) out << qint16(std::lround(8000 * std::sin(2 * M_PI * (200 + 100 * ch) * i / 48000.0)));
            f.write(data);
        }
        state()->newProject();
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QCOMPARE(sourceChannelCount(*state()->project().findMedia(media)), 4);
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        state()->setSelection({clip}, false);
        for (const char* name : {"audioChannels", "splitChannels", "splitChannelPairs", "channelsMix"}) QVERIFY2(win_->findChild<QAction*>(name), name);
        // Channel 2 alone: one undo step, and decoded in the background for playback and the waveform.
        QVERIFY(win_->setSelectionChannels({1}));
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->channels, std::vector<int>{1});
        const std::string path = state()->project().findMedia(media)->path;
        QTRY_VERIFY(MediaPool::instance().audioIfReady(audioKey(path, {1}), state()->sequence()->sampleRate));
        QTRY_VERIFY(MediaPool::instance().peaksIfReady(audioKey(path, {1})));
        QVERIFY(!win_->setSelectionChannels({7}));  // refused: the file has four
        win_->findChild<QAction*>("channelsMix")->trigger();
        QVERIFY(edit::clipById(*state()->sequence(), clip)->channels.empty());
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->channels, std::vector<int>{1});
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), clip)->channels.empty());
        // Split into mono clips from the menu: four clips on A1-A4, one undo step.
        win_->findChild<QAction*>("splitChannels")->trigger();
        const Sequence* s = state()->sequence();
        QCOMPARE(s->audioTracks.size(), size_t(4));
        for (int t = 0; t < 4; ++t) QCOMPARE(s->audioTracks[size_t(t)].clips.at(0).channels, std::vector<int>{t});
        state()->undo();
        QCOMPARE(state()->sequence()->audioTracks[1].clips.size(), size_t(0));
        // The media bin's setting: new clips come a channel each.
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin->setAudioChannelMode({media}, kChannelsMono));
        QVERIFY(!bin->setAudioChannelMode({media}, kChannelsMono));
        QCOMPARE(state()->project().findMedia(media)->audioChannelMode, std::string("mono"));
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 100, 0, -1, V1, A1, false); }));
        for (int t = 0; t < 4; ++t) {
            const Clip* c = edit::clipAt(*state()->sequence(), {TrackKind::Audio, t}, 110);
            QVERIFY(c);
            QCOMPARE(c->channels, std::vector<int>{t});
        }
        state()->setSelection({}, false);
    }

    void footage360() {
        // A 2:1 still marked 360° from the media bin comes into a flat sequence as a view out of the sphere.
        const QString png = dir_.path() + "/sphere.png";
        {
            QImage q(400, 200, QImage::Format_RGB32);
            for (int y = 0; y < 200; ++y)
                for (int x = 0; x < 400; ++x) q.setPixel(x, y, qRgb(x * 255 / 399, y * 255 / 199, 0));
            QVERIFY(q.save(png));
        }
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin->setMediaProjection(ids, "equirect"));
        QVERIFY(!bin->setMediaProjection(ids, "equirect"));
        QCOMPARE(state()->project().findMedia(ids[0])->projection, std::string("equirect"));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, 30, V1, A1, false); }));
        const Clip& c = trackAt(*state()->sequence(), V1)->clips.at(0);
        QCOMPARE(c.effects.at(0).type, std::string("reframe_360"));
        QVERIFY(findEffectInfo("reframe_360"));
        // Look around: selected, its picture in the Program monitor drags the view, one undo step.
        const Id clip = c.id;
        state()->setPlayhead(10);
        state()->setSelection({clip}, false);
        QApplication::processEvents();
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        QTRY_COMPARE(program->lookClip(), clip);
        ViewerWidget* viewer = program->viewer();
        QVERIFY(viewer->lookAround());
        const QRectF pic = viewer->imageRect();
        QVERIFY(pic.width() > 50);
        const QPoint from = pic.center().toPoint(), to = from + QPoint(int(pic.width() / 4), 0);
        QTest::mousePress(viewer, Qt::LeftButton, {}, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), viewer->mapToGlobal(QPointF(to)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer, &move);
        QTest::mouseRelease(viewer, Qt::LeftButton, {}, to);
        auto yaw = [&] {
            for (const Effect& e : edit::clipById(*state()->sequence(), clip)->effects)
                if (e.type == "reframe_360") return e.p("yaw", 0);
            return 999.0;
        };
        QVERIFY2(std::fabs(yaw() + 25) < 2, qPrintable(QString::number(yaw())));  // a quarter of the width at 100°, turned left
        state()->undo();
        QCOMPARE(yaw(), 0.0);
        state()->setSelection({}, false);
        QApplication::processEvents();
        QVERIFY(!viewer->lookAround());
        state()->undo();
        state()->undo();
        QVERIFY(state()->project().findMedia(media)->projection.empty());
        // Sequence settings: a 360° sequence.
        SequenceSettingsDialog dlg(win_.get());
        NewSequenceSpec spec;
        spec.width = 3840;
        spec.height = 1920;
        spec.spherical = true;
        dlg.setSpec(spec);
        auto* sphere = dlg.findChild<QCheckBox*>("sphericalSequence");
        QVERIFY(sphere && sphere->isChecked());
        QVERIFY(dlg.spec().spherical);
        sphere->setChecked(false);
        QVERIFY(!dlg.spec().spherical);
        state()->setSelection({}, false);
    }

    void extendEditAction() {
        loadDemo();
        QAction* extend = win_->findChild<QAction*>("extendEdit");
        QVERIFY(extend);
        QCOMPARE(extend->shortcut(), QKeySequence("E"));
        auto red = [&] { return clipNamed(*state()->sequence(), "Red"); };
        auto blue = [&] { return clipNamed(*state()->sequence(), "Blue"); };
        // A selected clip: Blue's head, nearest the playhead, rolls on to it (the mattes start at their first frame,
        // so they have nothing before it to roll back into).
        state()->setSelection({blue()->id}, false);
        state()->setPlayhead(70);
        extend->trigger();
        QCOMPARE(red()->end(), FrameTime(70));
        QCOMPARE(blue()->start, FrameTime(70));
        state()->undo();
        QCOMPARE(blue()->start, FrameTime(60));
        state()->setPlayhead(50);
        extend->trigger();  // back is refused
        QCOMPARE(blue()->start, FrameTime(60));
        // Nothing selected: the edit nearest the playhead on the target track.
        state()->setSelection({}, false);
        state()->setPlayhead(68);
        extend->trigger();
        QCOMPARE(red()->end(), FrameTime(68));
        state()->undo();
        // Trim mode: the selected edit to the playhead.
        state()->setPlayhead(55);
        QVERIFY(win_->selectNearestEdit());
        state()->setPlayhead(70);
        extend->trigger();
        QCOMPARE(red()->end(), FrameTime(70));
        QCOMPARE(blue()->start, FrameTime(70));
        win_->endTrimMode();
        state()->undo();
    }

    void dynamicTrimWithJKL() {
        loadDemo();
        win_->setManualTrimShuttle(true);
        const double fps = state()->sequence()->fpsValue();
        auto red = [&] { return clipNamed(*state()->sequence(), "Red"); };
        auto blue = [&] { return clipNamed(*state()->sequence(), "Blue"); };
        QAction* J = win_->findChild<QAction*>("shuttleReverse");
        QAction* K = win_->findChild<QAction*>("shuttleStop");
        QAction* L = win_->findChild<QAction*>("shuttleForward");
        QVERIFY(J && K && L);
        QVERIFY(!win_->shuttleTrim(1));  // not in Trim mode: the keys are the transport's
        state()->setPlayhead(55);
        QVERIFY(win_->selectNearestEdit());  // the cut at 60
        const size_t undos = state()->history().undoCount();
        // Both sides (a roll): L plays on and the cut follows; K keeps it as one undo step.
        L->trigger();
        QVERIFY(win_->trimShuttling());
        win_->advanceTrimShuttle(5 / fps);
        QCOMPARE(win_->trimShuttleFrames(), FrameTime(5));
        QCOMPARE(red()->end(), FrameTime(65));
        QCOMPARE(blue()->start, FrameTime(65));
        QCOMPARE(state()->playhead(), FrameTime(65));  // the two-up and playhead follow the edit
        L->trigger();  // again: twice as fast
        QCOMPARE(win_->trimShuttleSpeed(), 2);
        win_->advanceTrimShuttle(2 / fps);
        QCOMPARE(red()->end(), FrameTime(69));
        J->trigger();  // the other way, at 1x
        QCOMPARE(win_->trimShuttleSpeed(), -1);
        win_->advanceTrimShuttle(1 / fps);
        QCOMPARE(red()->end(), FrameTime(68));
        K->trigger();
        QVERIFY(!win_->trimShuttling());
        QCOMPARE(state()->history().undoCount(), undos + 1);
        QCOMPARE(red()->end(), FrameTime(68));
        state()->undo();
        QCOMPARE(red()->end(), FrameTime(60));
        // The outgoing side: J plays back and shortens it, the incoming clip following (a ripple).
        state()->setPlayhead(60);
        QVERIFY(win_->selectNearestEdit());
        while (win_->trimEdit()->side != 1) win_->cycleTrimSide();
        J->trigger();
        win_->advanceTrimShuttle(10 / fps);
        K->trigger();
        QCOMPARE(red()->end(), FrameTime(50));
        QCOMPARE(blue()->start, FrameTime(50));
        state()->undo();
        // Esc puts a dynamic trim back and stays in Trim mode.
        QVERIFY(win_->selectNearestEdit());
        L->trigger();
        win_->advanceTrimShuttle(3 / fps);
        QCOMPARE(red()->end(), FrameTime(63));
        win_->findChild<QAction*>("endTrim")->trigger();
        QVERIFY(!win_->trimShuttling() && win_->trimEdit());
        QCOMPARE(red()->end(), FrameTime(60));
        QCOMPARE(state()->history().undoCount(), undos);
        // The trim stops where it can go no further: the outgoing clip down to its last frame.
        while (win_->trimEdit()->side != 1) win_->cycleTrimSide();
        J->trigger();
        win_->advanceTrimShuttle(100 / fps);
        QCOMPARE(win_->trimShuttleSpeed(), 0);  // stopped
        QCOMPARE(win_->trimShuttleFrames(), FrameTime(-59));
        QCOMPARE(red()->duration, FrameTime(1));
        win_->findChild<QAction*>("endTrim")->trigger();
        QCOMPARE(red()->duration, FrameTime(60));
        QCOMPARE(state()->history().undoCount(), undos);
        win_->endTrimMode();
        win_->setManualTrimShuttle(false);
    }

    void importImageSequences() {
        const QString dir = dir_.path() + "/plate";
        QDir().mkpath(dir);
        for (int n = 1; n <= 10; ++n) {
            QImage q(32, 18, QImage::Format_RGB32);
            q.fill(qRgb(n * 20, 0, 0));
            QVERIFY(q.save(dir + QStringLiteral("/plate_%1.png").arg(n, 3, 10, QLatin1Char('0'))));
        }
        state()->newProject();
        // The folder: one clip of ten frames at the sequence's rate, not ten stills.
        auto ids = state()->importFiles({dir});
        QCOMPARE(ids.size(), size_t(1));
        const MediaItem* m = state()->project().findMedia(ids[0]);
        QVERIFY(isImageSequencePath(m->path));
        QCOMPARE(m->name, std::string("plate_[001-010].png"));
        QVERIFY(std::fabs(m->duration - 10 / state()->sequence()->fpsValue()) < 1e-9);
        // One frame on its own is a still; Import Image Sequence makes the run from it at a chosen rate.
        ids = state()->importFiles({dir + "/plate_004.png"});
        QCOMPARE(state()->project().findMedia(ids.at(0))->kind, MediaKind::Image);
        QVERIFY(win_->findChild<QAction*>("importImageSequence"));
        const Id seq = state()->importImageSequence(dir + "/plate_004.png", Rational{25, 1});
        QVERIFY(seq);
        QCOMPARE(state()->project().findMedia(seq)->duration, 0.4);
        QString why;
        QVERIFY(!state()->importImageSequence(dir_.path() + "/nothing.png", Rational{25, 1}, &why));
        QVERIFY(!why.isEmpty());
        // Interpret Frame Rate from the bin: ten frames at 5 fps are 2 s, one undo step.
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin->setImageSequenceRate(seq, 5));
        QCOMPARE(state()->project().findMedia(seq)->duration, 2.0);
        state()->undo();
        QCOMPARE(state()->project().findMedia(seq)->duration, 0.4);
        // With grouping turned off, frames come in as stills.
        appSettings().setValue("import/imageSequences", false);
        ids = state()->importFiles({dir});
        appSettings().remove("import/imageSequences");
        QCOMPARE(ids.size(), size_t(10));
    }

    void correctTranscriptsInThePanel() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        seg.words = {{0.5, 0.9, "Welcome", 1}, {1.0, 1.3, "to", 1}, {1.4, 1.9, "Montaj,", 1}, {2.0, 2.3, "says", 1},
                     {2.4, 2.8, "Jon", 1},     {2.9, 3.4, "Smyth.", 1}};
        t->segments.push_back(seg);
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        auto* panel = win_->findChild<TranscriptPanel*>();
        QVERIFY(panel && panel->findChild<QToolButton*>("correctButton"));
        state()->setSourceMedia(media);
        const int timelineWidth = viewport()->width();
        panel->setMode(TranscriptPanel::Mode::Source);
        auto text = [&] { return state()->project().findMedia(media)->transcript->segments[0].text; };
        // Correct the selected words, one undo step; revert them.
        panel->selectWords(4, 5);
        QVERIFY(panel->correctSelection("John Smith."));
        QCOMPARE(text(), std::string("Welcome to Montaj, says John Smith."));
        QCOMPARE(panel->words().size(), size_t(6));
        panel->selectWords(4, 4);
        QVERIFY(panel->revertSelection());
        QCOMPARE(text(), std::string("Welcome to Montaj, says Jon Smyth."));
        state()->undo();
        QCOMPARE(text(), std::string("Welcome to Montaj, says John Smith."));
        // Replace across transcripts, and fix the vocabulary's near misses.
        QCOMPARE(panel->replaceInAllTranscripts("says", "said"), 1);
        QVERIFY(state()->edit("Vocabulary", [](Project& p, Sequence&) {
            p.vocabulary = {"Montage"};
            return true;
        }));
        QCOMPARE(panel->applyVocabulary(), 1);
        QCOMPARE(text(), std::string("Welcome to Montage, said John Smith."));  // its comma kept
        QCOMPARE(panel->applyVocabulary(), 0);
        state()->undo();
        QCOMPARE(text(), std::string("Welcome to Montaj, said John Smith."));
        panel->setMode(TranscriptPanel::Mode::Sequence);
        QCOMPARE(viewport()->width(), timelineWidth);  // switching modes never widens the panel over the timeline
        QVERIFY2(panel->minimumSizeHint().width() < 320, qPrintable(QString::number(panel->minimumSizeHint().width())));  // buttons wrap
    }

    void suggestChaptersFromTheMenu() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("suggestChapters"));
        QCOMPARE(win_->suggestChapterMarkers(20, false), 0);  // nothing transcribed
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        const Id media = ids[0];
        double end = 0;
        auto t = threeTalks(end);
        QVERIFY(state()->edit("Transcript", [media, t, end](Project& p, Sequence& s) {
            MediaItem* m = p.findMedia(media);
            m->transcript = t;
            m->duration = end + 1;  // as long as the talks (the test recording itself is short)
            return edit::placeMedia(p, s, media, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        QCOMPARE(win_->suggestChapterMarkers(20, false), 3);
        const auto& markers = state()->sequence()->markers;
        QCOMPARE(markers.size(), size_t(3));
        QCOMPARE(markers[0].t, FrameTime(0));
        QVERIFY(std::all_of(markers.begin(), markers.end(), [](const Marker& mk) { return mk.chapter && !mk.name.empty(); }));
        state()->undo();
        QVERIFY(state()->sequence()->markers.empty());
        // The dialog: rename the first chapter and keep a chapter marker already there.
        QVERIFY(state()->edit("Marker", [](Project&, Sequence& s) {
            edit::addMarker(s, Marker{10, 0, "Mine", {}, 0, true});
            return true;
        }));
        QTimer::singleShot(0, this, [&] {
            auto* dlg = win_->findChild<QDialog*>("suggestChaptersDialog");
            QVERIFY(dlg);
            dlg->findChild<QSpinBox*>("chapterMinSeconds")->setValue(20);
            auto* table = dlg->findChild<QTableWidget*>("chapterTable");
            QCOMPARE(table->rowCount(), 3);
            table->item(0, 1)->setText("Cooking");
            dlg->findChild<QCheckBox*>("keepChapters")->setChecked(true);
            dlg->accept();
        });
        win_->findChild<QAction*>("suggestChapters")->trigger();
        QCOMPARE(state()->sequence()->markers.size(), size_t(4));
        QCOMPARE(state()->sequence()->markers[0].name, std::string("Cooking"));
        QVERIFY(std::any_of(state()->sequence()->markers.begin(), state()->sequence()->markers.end(),
                            [](const Marker& mk) { return mk.name == "Mine"; }));
    }

    void makeShortsFromTheMenu() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("makeShorts"));
        ShortsOptions o;
        o.count = 2;
        o.minSeconds = 3;
        o.maxSeconds = 7;  // each sentence on its own
        o.liveliness = false;
        ShortBuild b;
        b.reframe = false;
        QVERIFY(win_->makeShorts(o, b).empty());  // nothing transcribed yet
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        t->language = "en";
        TranscriptSegment seg;
        double at = 0.3;
        for (const char* w : {"And", "so,", "my", "fellow", "Americans,", "ask", "not", "what", "your", "country", "can", "do",
                              "for", "you.", "Ask", "what", "you", "can", "do", "for", "your", "country."}) {
            seg.words.push_back({at, at + 0.3, w, 0.9f});
            at += 0.45;
        }
        t->segments.push_back(seg);
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        const size_t sequences = state()->project().sequences.size(), jobs = win_->renderQueue()->jobs().size();
        const std::vector<Id> made = win_->makeShorts(o, b, true);
        QCOMPARE(made.size(), size_t(2));
        QCOMPARE(state()->project().sequences.size(), sequences + 2);
        QCOMPARE(state()->sequence()->id, made.front());
        for (size_t i = 0; i < made.size(); ++i) {
            const Sequence* s = state()->project().findSequence(made[i]);
            QVERIFY(s && s->height > s->width);
            QCOMPARE(QString::fromStdString(s->name), QStringLiteral("jfk - Short %1").arg(i + 1));
            QCOMPARE(s->captionTracks.size(), size_t(1));
            QCOMPARE(s->videoTracks[0].clips.at(0).generator.type, std::string("audio_viz"));  // sound alone gets an audiogram
        }
        // A render job each: H.264 at -14 LUFS with the captions burned in.
        QCOMPARE(win_->renderQueue()->jobs().size(), jobs + 2);
        const RenderQueue::Job& job = win_->renderQueue()->jobs().back();
        QCOMPARE(job.sequence, made.back());
        QVERIFY(job.settings.burnInCaptions);
        QCOMPARE(job.settings.loudnessTarget, -14.0);
        QVERIFY(QString::fromStdString(job.settings.path).endsWith("jfk - Short 2.mp4"));
        // One undo step takes them all away.
        state()->undo();
        QCOMPARE(state()->project().sequences.size(), sequences);
        QVERIFY(!state()->project().findSequence(made.front()));
        const int last = win_->renderQueue()->jobs().back().id, before = win_->renderQueue()->jobs()[jobs].id;
        for (int id : {before, last}) QVERIFY(win_->renderQueue()->remove(id));
        // The dialog: find the moments, leave one out, create square shorts without captions.
        int rows = 0;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = win_->findChild<QDialog*>("shortsDialog");
            QVERIFY(dlg);
            auto* create = dlg->findChild<QPushButton*>("shortsCreate");
            QVERIFY(!create->isEnabled());  // nothing found yet
            dlg->findChild<QSpinBox*>("shortsMin")->setValue(3);
            dlg->findChild<QSpinBox*>("shortsMax")->setValue(7);
            dlg->findChild<QComboBox*>("shortsShape")->setCurrentIndex(1);
            dlg->findChild<QComboBox*>("shortsLook")->setCurrentIndex(0);
            dlg->findChild<QPushButton*>("shortsFind")->click();
            auto* table = dlg->findChild<QTableWidget*>("shortsTable");
            rows = table->rowCount();
            if (rows == 2) table->item(1, 0)->setCheckState(Qt::Unchecked);
            QVERIFY(create->isEnabled());
            create->click();
        });
        win_->findChild<QAction*>("makeShorts")->trigger();
        QCOMPARE(rows, 2);
        QCOMPARE(state()->project().sequences.size(), sequences + 1);
        const Sequence* square = state()->sequence();
        QCOMPARE(square->width, square->height);
        QVERIFY(square->captionTracks.empty());
        QCOMPARE(win_->renderQueue()->jobs().size(), jobs);  // not queued
    }

    void exportVersionsQueued() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("exportVersions"));
        const Id source = state()->sequence()->id;
        QVERIFY(state()->edit("Captions", [](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = p.newId();
            t.name = "Subtitles";
            t.captions = {{0, 24, "Hello"}};
            t.style.position = 0.9;
            s.captionTracks.push_back(t);
            return true;
        }));
        const QString name = QString::fromStdString(state()->sequence()->name);
        const size_t sequences = state()->project().sequences.size(), jobs = win_->renderQueue()->jobs().size();
        const QString folder = dir_.filePath("versions");
        const std::vector<Id> ids = win_->exportVersions({{16, 9, "16x9"}, {9, 16, "9x16"}, {1, 1, "1x1"}}, folder, true, -14);
        QCOMPARE(ids.size(), size_t(3));
        QCOMPARE(ids[0], source);  // the cut itself is the landscape version
        QCOMPARE(state()->sequence()->id, source);
        QCOMPARE(state()->project().sequences.size(), sequences + 2);
        const Sequence* tall = state()->project().findSequence(ids[1]);
        QVERIFY(tall && tall->height > tall->width);
        QCOMPARE(QString::fromStdString(tall->name), name + " 9x16");
        QCOMPARE(tall->captionTracks.at(0).style.position, 0.75);
        const Sequence* square = state()->project().findSequence(ids[2]);
        QVERIFY(square && square->height == square->width);
        QVERIFY(QDir(folder).exists());
        // A job each, named after its version, at its own size, captions burned in at -14 LUFS.
        QCOMPARE(win_->renderQueue()->jobs().size(), jobs + 3);
        for (size_t i = 0; i < 3; ++i) {
            const RenderQueue::Job& job = win_->renderQueue()->jobs()[jobs + i];
            QCOMPARE(job.sequence, ids[i]);
            const Sequence* v = state()->project().findSequence(ids[i]);
            QCOMPARE(QString::fromStdString(job.settings.path), QDir(folder).filePath(QString::fromStdString(v->name) + ".mp4"));
            QCOMPARE(job.settings.width, 0);
            QVERIFY(job.settings.burnInCaptions);
            QCOMPARE(job.settings.loudnessTarget, -14.0);
        }
        for (size_t i = 0; i < 3; ++i) win_->renderQueue()->remove(win_->renderQueue()->jobs().back().id);
        // One undo step takes the copies away; doing it again does not pile up versions.
        state()->undo();
        QCOMPARE(state()->project().sequences.size(), sequences);
        win_->exportVersions({{9, 16, "9x16"}}, folder, false, 0);
        win_->exportVersions({{9, 16, "9x16"}}, folder, false, 0);
        QCOMPARE(state()->project().sequences.size(), sequences + 1);
        QVERIFY(!win_->renderQueue()->jobs().back().settings.burnInCaptions);
        QCOMPARE(win_->renderQueue()->jobs().back().settings.loudnessTarget, 0.0);
        for (int i = 0; i < 2; ++i) win_->renderQueue()->remove(win_->renderQueue()->jobs().back().id);
        // The dialog: 4:5 alone, as mixed, into a folder.
        const QString other = dir_.filePath("feeds");
        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = win_->findChild<QDialog*>("exportVersionsDialog");
            QVERIFY(dlg);
            shown = true;
            QVERIFY(dlg->findChild<QCheckBox*>("version16x9")->isChecked());
            dlg->findChild<QCheckBox*>("version16x9")->setChecked(false);
            dlg->findChild<QCheckBox*>("version9x16")->setChecked(false);
            dlg->findChild<QCheckBox*>("version4x5")->setChecked(true);
            dlg->findChild<QLineEdit*>("versionsFolder")->setText(other);
            dlg->accept();
        });
        win_->findChild<QAction*>("exportVersions")->trigger();
        QVERIFY(shown);
        const RenderQueue::Job& job = win_->renderQueue()->jobs().back();
        const Sequence* feed = state()->project().findSequence(job.sequence);
        QVERIFY(feed && std::abs(feed->width * 5 - feed->height * 4) <= 10);  // sizes are even (180 x 226)
        QVERIFY(QString::fromStdString(job.settings.path).startsWith(other));
        win_->renderQueue()->remove(job.id);
    }

    void exportForReviewAndNotes() {
        loadDemo();
        QVERIFY(win_->findChild<QAction*>("exportForReview"));
        QVERIFY(win_->findChild<QAction*>("importReviewNotes"));
        const QString name = QString::fromStdString(state()->sequence()->name);
        const size_t jobs = win_->renderQueue()->jobs().size();
        ReviewExportOptions o;
        o.watermark = "Review copy";
        o.note = "Notes by Friday";
        const QString folder = dir_.filePath("review");
        const QString page = win_->exportForReview(folder, o);
        QCOMPARE(page, QDir(folder).filePath(name + " - Review.html"));
        QFile f(page);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Notes by Friday"));
        // The copy is queued: H.264 with timecode and the watermark burned in.
        QCOMPARE(win_->renderQueue()->jobs().size(), jobs + 1);
        const RenderQueue::Job& job = win_->renderQueue()->jobs().back();
        QCOMPARE(job.sequence, state()->sequence()->id);
        QCOMPARE(QString::fromStdString(job.settings.path), QDir(folder).filePath(name + " - Review.mp4"));
        QCOMPARE(job.settings.videoCodec, std::string("libx264"));
        QVERIFY(job.settings.burnIn.timecode);
        QCOMPARE(job.settings.burnIn.text, std::string("Review copy"));
        win_->renderQueue()->remove(job.id);

        // Notes saved from the page come back as markers, one undo step.
        const size_t markers = state()->sequence()->markers.size();
        const QString notes = dir_.filePath("notes.json");
        QFile nf(notes);
        QVERIFY(nf.open(QIODevice::WriteOnly));
        nf.write(R"({"montageReview":1,"fps":[30,1],"notes":[{"frame":15,"author":"Sam","text":"Tighter"},{"frame":45,"author":"Ana","text":"Louder","done":true}]})");
        nf.close();
        QCOMPARE(win_->importMarkers(notes), 2);
        const auto& ms = state()->sequence()->markers;
        QCOMPARE(ms.size(), markers + 2);
        QVERIFY(std::any_of(ms.begin(), ms.end(), [](const Marker& m) { return m.name == "Sam" && m.comment == "Tighter"; }));
        QVERIFY(std::any_of(ms.begin(), ms.end(), [](const Marker& m) { return m.name == "\xE2\x9C\x93 Ana"; }));
        state()->undo();
        QCOMPARE(state()->sequence()->markers.size(), markers);

        // The dialog: 720p, no timecode, In to Out when marked.
        state()->setInPoint(5);
        state()->setOutPoint(20);
        const QString other = dir_.filePath("review2");
        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = win_->findChild<QDialog*>("exportForReviewDialog");
            QVERIFY(dlg);
            shown = true;
            dlg->findChild<QLineEdit*>("reviewFolder")->setText(other);
            dlg->findChild<QComboBox*>("reviewSize")->setCurrentIndex(1);
            dlg->findChild<QCheckBox*>("reviewTimecode")->setChecked(false);
            QVERIFY(dlg->findChild<QCheckBox*>("reviewRange")->isEnabled());
            dlg->findChild<QCheckBox*>("reviewRange")->setChecked(true);
            dlg->accept();
        });
        win_->findChild<QAction*>("exportForReview")->trigger();
        QVERIFY(shown);
        const RenderQueue::Job& j2 = win_->renderQueue()->jobs().back();
        QVERIFY(QString::fromStdString(j2.settings.path).startsWith(other));
        QVERIFY(!j2.settings.burnIn.timecode);
        QCOMPARE(j2.settings.in, FrameTime(5));
        QCOMPARE(j2.settings.out, FrameTime(21));
        QVERIFY(QFileInfo::exists(QDir(other).filePath(name + " - Review.html")));
        win_->renderQueue()->remove(j2.id);
    }

    void matchColourChart() {
        // A still of a chart under a warm cast, on the timeline twice.
        const QString file = dir_.filePath("chart.png");
        QVERIFY(castChartImage().save(file));
        state()->newProject();
        const auto ids = state()->importFiles({file});
        QCOMPARE(ids.size(), size_t(1));
        Id a = 0, b = 0;
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            s.width = 640, s.height = 360;
            if (!edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
            if (!edit::placeMedia(p, s, ids[0], 60, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
            a = s.videoTracks[0].clips[0].id, b = s.videoTracks[0].clips[1].id;
            return true;
        }));
        QVERIFY(win_->findChild<QAction*>("matchColourChart"));
        state()->setSelection({a}, false);
        // Found when the dialog opens; the outline dragged off it, the match falls apart; Find Chart puts it back.
        QString first, after;
        bool shown = false;
        win_->chartDialogHook_ = [&](ChartMatchDialog* dlg) {
            shown = true;
            // Looked for in the background: nothing to apply until it is found.
            QVERIFY(!dlg->findChild<QPushButton*>("chartApply")->isEnabled());
            QTRY_VERIFY(!dlg->searching() && dlg->ready());
            QVERIFY(dlg->findChild<QPushButton*>("chartApply")->isEnabled());
            const TrackQuad found = dlg->quad();
            QVERIFY(std::fabs(found.p[0].x - 120.0 / 640) < 0.02 && std::fabs(found.p[2].y - 280.0 / 360) < 0.03);
            QVERIFY(dlg->fit().deltaEBefore > 5 && dlg->fit().deltaEAfter < 2);
            first = dlg->findChild<QLabel*>("chartStatus")->text();
            TrackQuad off = found;
            for (auto& pt : off.p) pt.x += 0.25;
            dlg->setQuad(off);
            QVERIFY(dlg->fit().deltaEAfter > 3 || !dlg->fit().ok);
            dlg->findChild<QPushButton*>("chartFind")->click();
            QTRY_VERIFY(!dlg->searching() && dlg->fit().ok);
            QVERIFY(dlg->fit().deltaEAfter < 2);
            after = dlg->findChild<QLabel*>("chartStatus")->text();
            // The corners move with the keys: 3 picks the bottom right, Shift+Right moves it ten pixels of the frame.
            auto* view = dlg->findChild<QWidget*>("chartView");
            const double x = dlg->quad().p[2].x;
            QTest::keyClick(view, Qt::Key_3);
            QTest::keyClick(view, Qt::Key_Right, Qt::ShiftModifier);
            QVERIFY(std::fabs(dlg->quad().p[2].x - (x + 10.0 / 640)) < 1e-6);
            QTest::keyClick(view, Qt::Key_Left, Qt::ShiftModifier);
            QVERIFY(std::fabs(dlg->quad().p[2].x - x) < 1e-6);
            QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
        };
        QCOMPARE(win_->matchColourChart(), 1);
        QVERIFY(shown);
        QVERIFY2(first.contains("Delta E") && after.contains("Delta E"), qPrintable(first));
        // One Chart Match, first in the clip's effects, one undo step; the other clip of the media untouched.
        auto chartOn = [&](Id id) {
            const Clip* c = edit::clipById(*state()->sequence(), id);
            return c && !c->effects.empty() && c->effects.front().type == "chart_match";
        };
        QVERIFY(chartOn(a));
        QVERIFY(!chartOn(b));
        // The picture played: the patches as the chart has them.
        {
            const Sequence& s = *state()->sequence();
            const Image img = renderProgramFrame(state()->project(), s, 10, RenderOptions{});
            TrackQuad q;
            q.p[0] = {120.0 / 640, 80.0 / 360}, q.p[1] = {420.0 / 640, 80.0 / 360}, q.p[2] = {420.0 / 640, 280.0 / 360}, q.p[3] = {120.0 / 640, 280.0 / 360};
            const ChartColors got = sampleChart(img, q);
            double worst = 0;
            for (int i = 0; i < 24; ++i) worst = std::max(worst, deltaE76(got[size_t(i)], colorChecker24()[size_t(i)]));
            QVERIFY2(worst < 4, qPrintable(QString::number(worst)));
        }
        state()->undo();
        QVERIFY(!chartOn(a));
        // Every clip of the media, when asked; cancelling changes nothing.
        win_->chartDialogHook_ = [](ChartMatchDialog* dlg) {
            QTRY_VERIFY(!dlg->searching());
            dlg->findChild<QCheckBox*>("chartSameMedia")->setChecked(true);
            QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
        };
        QCOMPARE(win_->matchColourChart(), 2);
        QVERIFY(chartOn(a) && chartOn(b));
        win_->chartDialogHook_ = [](ChartMatchDialog* dlg) { QMetaObject::invokeMethod(dlg, "reject", Qt::QueuedConnection); };
        QCOMPARE(win_->matchColourChart(), 0);
        // A picture with no chart: Apply stays off, and accepting anyway changes nothing.
        {
            const QString plain = dir_.filePath("plain.png");
            QImage grey(640, 360, QImage::Format_RGB32);
            grey.fill(QColor(90, 90, 90));
            QVERIFY(grey.save(plain));
            const auto plainIds = state()->importFiles({plain});
            QCOMPARE(plainIds.size(), size_t(1));
            Id c = 0;
            QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
                if (!edit::placeMedia(p, s, plainIds[0], 200, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
                c = s.videoTracks[0].clips.back().id;
                return true;
            }));
            state()->setSelection({c}, false);
            bool apply = true;
            win_->chartDialogHook_ = [&](ChartMatchDialog* dlg) {
                QTRY_VERIFY(!dlg->searching());
                apply = dlg->findChild<QPushButton*>("chartApply")->isEnabled() || dlg->ready();
                QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
            };
            QCOMPARE(win_->matchColourChart(), 0);
            QVERIFY(!apply);
            QVERIFY(!chartOn(c));
        }
        win_->chartDialogHook_ = nullptr;
        state()->newProject();
    }

    void aiRunsOn() {
        // Tools > AI Models Run On: one choice checked at a time, remembered in the settings; fixed while
        // MONTAGE_INFERENCE says where models run (as it does in these tests).
        auto* menu = win_->findChild<QMenu*>("aiRunOn");
        QVERIFY(menu);
        QVERIFY(win_->findChild<QAction*>("aiDevices"));
        auto* efficient = win_->findChild<QAction*>("aiRunOn_efficient");
        auto* cpu = win_->findChild<QAction*>("aiRunOn_cpu");
        auto* automatic = win_->findChild<QAction*>("aiRunOn_auto");
        QVERIFY(efficient && cpu && automatic);
        const QByteArray was = qgetenv("MONTAGE_INFERENCE");
        qputenv("MONTAGE_INFERENCE", "cpu");
        emit menu->aboutToShow();
        QVERIFY(!efficient->isEnabled() && efficient->toolTip().contains("MONTAGE_INFERENCE"));
        qunsetenv("MONTAGE_INFERENCE");
        auto restore = qScopeGuard([&] {
            qputenv("MONTAGE_INFERENCE", was);
            emit menu->aboutToShow();
        });
        emit menu->aboutToShow();
        QVERIFY(efficient->isEnabled());
        efficient->trigger();
        QVERIFY(efficient->isChecked() && !automatic->isChecked() && !cpu->isChecked());
        QCOMPARE(appSettings().value("ai/inference").toString(), QString("efficient"));
        cpu->trigger();
        QVERIFY(cpu->isChecked() && !efficient->isChecked());
        QCOMPARE(appSettings().value("ai/inference").toString(), QString("cpu"));
        automatic->trigger();
        QCOMPARE(appSettings().value("ai/inference").toString(), QString("auto"));
    }

    void reviewDrawingsAndVersions() {
        loadDemo();
        MonitorPanel* program = nullptr;
        for (MonitorPanel* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        auto* overlay = win_->findChild<ReviewOverlay*>();
        QVERIFY(overlay);
        QAction* show = win_->findChild<QAction*>("showReviewDrawings");
        QVERIFY(show && show->isChecked() && show->isCheckable());

        // A reviewer's green line across the middle of frame 10, drawn over the Program monitor there and nowhere else.
        state()->setPlayhead(10);
        QTest::qWait(300);  // (the frame rendered for the playhead)
        ReviewStroke line;
        line.color = "#00ff00";
        line.width = 0.05;
        line.points = {{0.1, 0.5}, {0.9, 0.5}};
        Marker m{10, 0, "Sam", "Here", 7, false};
        m.drawing = reviewDrawingJson({line});
        m.reviewId = "x1";
        QVERIFY(state()->edit("Mark", [&](Project&, Sequence& s) {
            edit::addMarker(s, m);
            return true;
        }));
        QCOMPARE(overlay->strokesNow().size(), size_t(1));
        QImage black(320, 180, QImage::Format_RGB32);
        black.fill(Qt::black);
        viewer->setImage(black);
        auto centre = [&] {
            const QImage shot = viewer->grab().toImage();
            const qreal dpr = shot.devicePixelRatio();
            const QRectF r = viewer->imageRect();
            return shot.pixelColor(int(r.center().x() * dpr), int(r.center().y() * dpr));
        };
        QColor c = centre();
        QVERIFY2(c.green() > 200 && c.red() < 80 && c.blue() < 80, qPrintable(c.name()));
        show->trigger();  // Playback › Show Review Drawings off
        QVERIFY(!overlay->shown());
        c = centre();
        QVERIFY2(c.green() < 60, qPrintable(c.name()));
        show->trigger();
        QVERIFY(overlay->shown());
        state()->setPlayhead(11);
        QVERIFY(overlay->strokesNow().empty());
        // A marker with a length shows its drawing all through it.
        QVERIFY(state()->edit("Lengthen", [&](Project&, Sequence& s) {
            for (Marker& x : s.markers)
                if (x.reviewId == "x1") x.duration = 5;
            return true;
        }));
        QCOMPARE(overlay->strokesNow().size(), size_t(1));
        state()->setPlayhead(15);
        QVERIFY(overlay->strokesNow().empty());
        // A note that had to sit beside its frame shows its drawing on the frame it was drawn on.
        QVERIFY(state()->edit("Move", [&](Project&, Sequence& s) {
            for (Marker& x : s.markers)
                if (x.reviewId == "x1") x.duration = 0, x.t = 12, x.drawnAt = 10;
            return true;
        }));
        state()->setPlayhead(10);
        QCOMPARE(overlay->strokesNow().size(), size_t(1));
        state()->setPlayhead(12);
        QVERIFY(overlay->strokesNow().empty());

        // A copy sent again to the same folder is version 2, and its page compares with version 1.
        const QString name = QString::fromStdString(state()->sequence()->name);
        const QString folder = dir_.filePath("versions");
        QDir(folder).removeRecursively();
        const QString first = win_->exportForReview(folder, ReviewExportOptions{});
        QCOMPARE(first, QDir(folder).filePath(name + " - Review.html"));
        win_->renderQueue()->remove(win_->renderQueue()->jobs().back().id);
        {
            QFile copy(QDir(folder).filePath(name + " - Review.mp4"));  // (as if rendered)
            QVERIFY(copy.open(QIODevice::WriteOnly));
        }
        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = win_->findChild<QDialog*>("exportForReviewDialog");
            QVERIFY(dlg);
            shown = true;
            dlg->findChild<QLineEdit*>("reviewFolder")->setText(folder);
            auto* version = dlg->findChild<QSpinBox*>("reviewVersion");
            auto* compare = dlg->findChild<QCheckBox*>("reviewCompare");
            QCOMPARE(version->value(), 2);
            QVERIFY(compare->isEnabled() && compare->isChecked());
            version->setValue(1);
            QVERIFY(!compare->isEnabled());  // nothing before version 1
            version->setValue(2);
            QVERIFY(compare->isEnabled());
            dlg->accept();
        });
        win_->findChild<QAction*>("exportForReview")->trigger();
        QVERIFY(shown);
        const RenderQueue::Job& job = win_->renderQueue()->jobs().back();
        QCOMPARE(QString::fromStdString(job.settings.path), QDir(folder).filePath(name + " - Review v2.mp4"));
        win_->renderQueue()->remove(job.id);
        QFile page(QDir(folder).filePath(name + " - Review v2.html"));
        QVERIFY(page.open(QIODevice::ReadOnly));
        const auto info = readReviewPage(page.readAll().toStdString());
        QVERIFY(info && info->version == 2);

        // Notes imported twice: the second time brings the same markers up to date instead of adding more.
        const QString notes = dir_.filePath("drawn-notes.json");
        auto writeNotes = [&](const char* json) {
            QFile f(notes);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(json);
        };
        writeNotes(R"({"montageReview":2,"fps":[30,1],"version":2,"notes":[{"id":"q1","frame":40,"author":"Ana","text":"Brighter",
            "drawing":[{"tool":"arrow","points":[[0.2,0.2],[0.5,0.5]]}]}]})");
        const size_t before = state()->sequence()->markers.size();
        QCOMPARE(win_->importMarkers(notes), 1);
        QCOMPARE(state()->sequence()->markers.size(), before + 1);
        writeNotes(R"({"montageReview":2,"fps":[30,1],"version":2,"notes":[{"id":"q1","frame":40,"author":"Ana","text":"Brighter","status":"done",
            "drawing":[{"tool":"arrow","points":[[0.2,0.2],[0.5,0.5]]}]}]})");
        QCOMPARE(win_->importMarkers(notes), 1);
        QCOMPARE(state()->sequence()->markers.size(), before + 1);
        QVERIFY(win_->statusBar()->currentMessage().contains("up to date"));
        const auto& ms = state()->sequence()->markers;
        QVERIFY(std::any_of(ms.begin(), ms.end(), [](const Marker& x) { return x.reviewId == "q1" && x.name.rfind("\xE2\x9C\x93 ", 0) == 0; }));
        const auto q1 = std::find_if(ms.begin(), ms.end(), [](const Marker& x) { return x.reviewId == "q1"; });
        state()->setPlayhead(q1->t);
        QCOMPARE(overlay->strokesNow().size(), size_t(1));
        QCOMPARE(overlay->strokesNow()[0].tool, std::string("arrow"));
    }

    void transitionsToSelection() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setSelection({red, blue}, false);
        QAction* all = win_->findChild<QAction*>("transitionsToSelection");
        QVERIFY(all);
        QCOMPARE(all->shortcut(), QKeySequence("Shift+D"));
        all->trigger();
        // Red's head, the cut and Blue's tail, in one undo step.
        QCOMPARE(trackAt(*state()->sequence(), V1)->transitions.size(), size_t(3));
        state()->undo();
        QCOMPARE(trackAt(*state()->sequence(), V1)->transitions.size(), size_t(0));
        // Ctrl+D with both selected does the same for pictures.
        win_->addTransitionsToSelection(TrackKind::Video);
        QCOMPARE(trackAt(*state()->sequence(), V1)->transitions.size(), size_t(3));
        state()->undo();
        state()->setSelection({}, false);
        all->trigger();  // nothing selected: a hint, no change
        QCOMPARE(trackAt(*state()->sequence(), V1)->transitions.size(), size_t(0));
    }

    void motionPathInTheProgramMonitor() {
        loadDemo();
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        auto* overlay = viewer->findChild<TransformOverlay*>();
        QVERIFY(overlay);
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        const FrameTime last = clipNamed(*state()->sequence(), "Red")->duration - 1;
        state()->setPlayhead(5);
        state()->setSelection({red}, false);
        QApplication::processEvents();
        std::vector<QPointF> path;
        std::vector<std::pair<FrameTime, QPointF>> keys;
        QVERIFY(!overlay->motionPath(red, path, keys));  // a still position has no path
        // Left to right across the clip.
        QVERIFY(state()->edit("Keys", [red, last](Project& p, Sequence& s) {
            Clip* clip = edit::clipById(s, red);
            if (clip->motion.empty()) clip->motion = makeEffect(p, "transform");
            clip->motion.params["pos_x"].addKey(0, -80);
            clip->motion.params["pos_x"].addKey(last, 80);
            return true;
        }));
        QApplication::processEvents();
        QVERIFY(overlay->motionPath(red, path, keys));
        QCOMPARE(path.size(), size_t(last + 1));  // a dot a frame
        QCOMPARE(keys.size(), size_t(2));
        const QRectF pic = viewer->imageRect();
        const double perPixel = 320.0 / pic.width();
        QVERIFY(std::fabs(keys[0].second.x() - (pic.center().x() - 80 / perPixel)) < 1.5);
        QVERIFY(std::fabs(keys[1].second.x() - (pic.center().x() + 80 / perPixel)) < 1.5);
        QCOMPARE(keys[1].first, last);
        // Even steps for a linear move.
        const double first = QLineF(path[0], path[1]).length(), mid = QLineF(path[size_t(last / 2)], path[size_t(last / 2 + 1)]).length();
        QVERIFY(std::fabs(first - mid) < 0.05);
        // Dragging the last keyframe 30 pixels up moves that keyframe only: one undo step.
        const QPointF from = keys[1].second, to = from + QPointF(0, -30);
        QTest::mousePress(viewer, Qt::LeftButton, {}, from.toPoint());
        QMouseEvent move(QEvent::MouseMove, to, viewer->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(viewer, &move);
        QTest::mouseRelease(viewer, Qt::LeftButton, {}, to.toPoint());
        const Clip* c = clipNamed(*state()->sequence(), "Red");
        QVERIFY2(std::fabs(c->motion.p("pos_y", last) + 30 * perPixel) < 1, qPrintable(QString::number(c->motion.p("pos_y", last))));
        QVERIFY(std::fabs(c->motion.p("pos_x", last) - 80) < 1);
        QCOMPARE(c->motion.p("pos_x", 0), -80.0);
        QCOMPARE(c->motion.params.at("pos_x").keys.size(), size_t(2));  // no key added at the playhead
        state()->undo();
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->motion.p("pos_y", last), 0.0);
        state()->undo();
    }

    void transformInTheProgramMonitor() {
        loadDemo();
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        auto* overlay = viewer->findChild<TransformOverlay*>();
        QVERIFY(overlay);
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->setPlayhead(5);
        state()->setSelection({red}, false);
        QApplication::processEvents();
        QCOMPARE(overlay->target(), red);
        const QRectF pic = viewer->imageRect();
        QVERIFY(pic.width() > 100);
        std::array<QPointF, 4> c;
        QPointF anchor;
        QVERIFY(overlay->box(red, c, anchor));
        QVERIFY(QLineF(c[0], pic.topLeft()).length() < 1.5 && QLineF(c[2], pic.bottomRight()).length() < 1.5);  // a full-frame matte
        QVERIFY(QLineF(anchor, pic.center()).length() < 1.5);
        const double perPixel = 320.0 / pic.width();  // sequence pixels per widget pixel
        auto dragFromTo = [&](QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
            QTest::mousePress(viewer, Qt::LeftButton, mods, from.toPoint());
            QMouseEvent move(QEvent::MouseMove, to, viewer->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, mods);
            QApplication::sendEvent(viewer, &move);
            QTest::mouseRelease(viewer, Qt::LeftButton, mods, to.toPoint());
        };
        auto motion = [&](const char* name, double def = 0) { return clipNamed(*state()->sequence(), "Red")->motion.p(name, 5, def); };
        // Move: a quarter of the picture to the right, one undo step.
        dragFromTo(pic.center(), pic.center() + QPointF(pic.width() / 4, 0));
        QVERIFY2(std::fabs(motion("pos_x") - 80) < 2, qPrintable(QString::number(motion("pos_x"))));
        QVERIFY(std::fabs(motion("pos_y")) < 1);
        state()->undo();
        QCOMPARE(motion("pos_x"), 0.0);
        // A small move snaps back to the centre; Ctrl leaves it where it is.
        dragFromTo(pic.center(), pic.center() + QPointF(3, 0));
        QCOMPARE(motion("pos_x"), 0.0);
        dragFromTo(pic.center(), pic.center() + QPointF(3, 0), Qt::ControlModifier);
        QVERIFY2(std::fabs(motion("pos_x") - 3 * perPixel) < 1, qPrintable(QString::number(motion("pos_x"))));
        state()->undo();
        // Scale from a corner: half as far again from the anchor is 150 %.
        QVERIFY(overlay->box(red, c, anchor));
        dragFromTo(c[1], anchor + (c[1] - anchor) * 1.5);
        QVERIFY2(std::fabs(motion("scale", 100) - 150) < 2, qPrintable(QString::number(motion("scale", 100))));
        state()->undo();
        // Stretch from the right edge: 120 % wide, height untouched.
        const QPointF right = (c[1] + c[2]) / 2;
        dragFromTo(right, anchor + (right - anchor) * 1.2);
        QVERIFY2(std::fabs(motion("scale_x", 100) - 120) < 2, qPrintable(QString::number(motion("scale_x", 100))));
        QCOMPARE(motion("scale_y", 100), 100.0);
        state()->undo();
        // Turn from just outside a corner: a quarter turn about the anchor; Shift rounds to 15°.
        const QPointF out = c[1] + QPointF(10, -10);
        const QPointF a = out - anchor;
        dragFromTo(out, anchor + QPointF(-a.y(), a.x()));
        QVERIFY2(std::fabs(motion("rotation") - 90) < 2, qPrintable(QString::number(motion("rotation"))));
        state()->undo();
        dragFromTo(out, anchor + QPointF(a.x() * std::cos(0.3) - a.y() * std::sin(0.3), a.x() * std::sin(0.3) + a.y() * std::cos(0.3)),
                   Qt::ShiftModifier);
        QCOMPARE(motion("rotation"), 15.0);  // 17° rounded
        state()->undo();
        // An animated setting is keyed at the playhead.
        QVERIFY(state()->edit("Keys", [red](Project& p, Sequence& s) {
            Clip* clip = edit::clipById(s, red);
            if (clip->motion.empty()) clip->motion = makeEffect(p, "transform");
            clip->motion.params["pos_x"].addKey(0, 0);
            clip->motion.params["pos_x"].addKey(50, 0);
            return true;
        }));
        dragFromTo(pic.center(), pic.center() + QPointF(pic.width() / 4, 0));
        const Param& px = clipNamed(*state()->sequence(), "Red")->motion.params.at("pos_x");
        QCOMPARE(px.keys.size(), size_t(3));
        QVERIFY(std::fabs(px.at(5) - 80) < 2 && px.at(50) == 0);
        state()->undo();
        state()->undo();
        // Clip > Align in Frame: half size, then to the top left corner inside the action-safe margin.
        QVERIFY(state()->edit("Half", [red](Project& p, Sequence& s) {
            Clip* clip = edit::clipById(s, red);
            if (clip->motion.empty()) clip->motion = makeEffect(p, "transform");
            clip->motion.params["scale"] = Param(50.0);
            return true;
        }));
        QVERIFY(win_->findChild<QAction*>("align_top_left"));
        win_->findChild<QAction*>("align_top_left")->trigger();
        std::array<double, 4> xs, ys;
        QVERIFY(clipFrameQuad(state()->project(), *state()->sequence(), *clipNamed(*state()->sequence(), "Red"), 5, xs, ys));
        QVERIFY2(std::fabs(xs[0] - 9) < 0.01 && std::fabs(ys[0] - 9) < 0.01, qPrintable(QString("%1 %2").arg(xs[0]).arg(ys[0])));
        state()->undo();
        state()->undo();
        // With nothing selected, a click on the picture selects the clip on top (the title over Red) and moves it.
        state()->setSelection({}, false);
        state()->setPlayhead(20);
        QApplication::processEvents();
        QCOMPARE(overlay->target(), Id(0));
        const Clip* title = edit::clipAt(*state()->sequence(), {TrackKind::Video, 1}, 20);
        QVERIFY(title);
        const Id titleId = title->id;
        dragFromTo(pic.center(), pic.center() + QPointF(0, pic.height() / 4));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{titleId});
        QVERIFY(std::fabs(edit::clipById(*state()->sequence(), titleId)->motion.p("pos_y", 5) - 45) < 2);
        state()->undo();
        state()->setSelection({}, false);
        state()->setPlayhead(0);
    }

    void clipAnimationInInspector() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        state()->setSelection({red}, false);
        QApplication::processEvents();
        auto* in = win_->findChild<QComboBox*>("animIn");
        auto* inSecs = win_->findChild<QDoubleSpinBox*>("animInSeconds");
        auto* combo = win_->findChild<QComboBox*>("animCombo");
        QVERIFY(in && inSecs && combo && win_->findChild<QComboBox*>("animOut"));
        QCOMPARE(in->currentIndex(), 0);  // None
        // Slide Left over 0.8 s: one undo step.
        inSecs->setValue(0.8);
        in->setCurrentIndex(in->findData(QStringLiteral("slide_left")));
        emit in->activated(in->currentIndex());
        const Clip* c = clipNamed(*state()->sequence(), "Red");
        QCOMPARE(c->animIn, (ClipAnimation{"slide_left", 0.8}));
        QCOMPARE(state()->undoText(), tr("Clip Animation"));
        combo->setCurrentIndex(combo->findData(QStringLiteral("pulse")));
        emit combo->activated(combo->currentIndex());
        QCOMPARE(clipNamed(*state()->sequence(), "Red")->animLoop.type, std::string("pulse"));
        state()->undo();
        QVERIFY(clipNamed(*state()->sequence(), "Red")->animLoop.type.empty());
        state()->undo();
        QVERIFY(!hasClipAnimation(*clipNamed(*state()->sequence(), "Red")));
        // The Inspector follows: back to None.
        QApplication::processEvents();
        QCOMPARE(win_->findChild<QComboBox*>("animIn")->currentIndex(), 0);
        state()->setSelection({}, false);
    }

    void speedWithMaintainedPitch() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        }));
        const Id id = state()->sequence()->audioTracks[0].clips.at(0).id;
        const FrameTime length = state()->sequence()->audioTracks[0].clips.at(0).duration;
        state()->setSelection({id}, false);
        // Speed / Duration at 150 % keeping the pitch: one undo step.
        QVERIFY(win_->setSelectionSpeed(1.5, true));
        const Clip* c = edit::clipById(*state()->sequence(), id);
        QCOMPARE(c->speed, 1.5);
        QVERIFY(c->duration < length);
        QVERIFY(c->timing.p("maintain_pitch", 0) > 0.5);
        // The Inspector's Time Remapping shows it.
        auto* inspector = win_->findChild<InspectorWidget*>();
        QApplication::processEvents();
        bool shown = false;
        for (QCheckBox* b : inspector->widget()->findChildren<QCheckBox*>()) shown = shown || b->text().contains("Maintain Audio Pitch");
        for (QLabel* l : inspector->widget()->findChildren<QLabel*>()) shown = shown || l->text().contains("Maintain Audio Pitch");
        QVERIFY(shown);
        // Only the pitch changing is still an edit; the same again is not.
        QVERIFY(win_->setSelectionSpeed(1.5, false));
        QVERIFY(edit::clipById(*state()->sequence(), id)->timing.p("maintain_pitch", 0) < 0.5);
        QVERIFY(!win_->setSelectionSpeed(1.5, false));
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), id)->timing.p("maintain_pitch", 0) > 0.5);
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), id)->speed, 1.0);
        QVERIFY(edit::clipById(*state()->sequence(), id)->timing.p("maintain_pitch", 0) < 0.5);
        // It plays: the mix at 150 % kept pitch is made without waiting in exports and is sound, not silence.
        QVERIFY(win_->setSelectionSpeed(1.5, true));
        AudioMixer mixer;
        std::vector<float> out(48000 * 2);
        mixer.mix(state()->project(), *state()->sequence(), 48000, 48000, out.data());
        double energy = 0;
        for (float v : out) energy += double(v) * v;
        QVERIFY(energy > 1);
        state()->newProject();
    }

    void gapsCutCopyPasteAndClearSolo() {
        loadDemo();
        const Clip* red = clipNamed(*state()->sequence(), "Red");
        const Id redId = red->id, blueId = clipNamed(*state()->sequence(), "Blue")->id;
        const FrameTime redEnd = red->end();
        QVERIFY(state()->apply("Move", [blueId](Project& p, Sequence& s) { return edit::moveClips(p, s, {blueId}, 15, 0, 0); }));
        TimelineWidget* tl = win_->timeline();
        auto action = [this](const QString& text) {
            for (QAction* a : win_->findChildren<QAction*>())
                if (a->text() == text) return a;
            return static_cast<QAction*>(nullptr);
        };
        QAction *copy = action("&Copy"), *cut = action("Cu&t"), *paste = action("&Paste");
        QVERIFY(copy && cut && paste);
        auto blue = [&] { return edit::clipById(*state()->sequence(), blueId)->start; };
        // Copy the 15-frame gap after Red, and paste it inside Red: Red splits and Blue moves on by 15.
        QVERIFY(tl->selectGapAt({TrackKind::Video, 0}, redEnd + 3));
        copy->trigger();
        state()->setPlayhead(5);
        paste->trigger();
        QCOMPARE(blue(), redEnd + 30);
        QCOMPARE(edit::clipById(*state()->sequence(), redId)->end(), FrameTime(5));
        QCOMPARE(state()->undoText(), tr("Paste Gap"));
        state()->undo();
        QCOMPARE(blue(), redEnd + 15);
        // Cut closes it; pasting at Red's end brings it back.
        QVERIFY(tl->selectGapAt({TrackKind::Video, 0}, redEnd + 3));
        cut->trigger();
        QCOMPARE(blue(), redEnd);
        state()->setPlayhead(redEnd);
        paste->trigger();
        QCOMPARE(blue(), redEnd + 15);
        // Copying clips makes Paste paste clips again.
        state()->setSelection({redId});
        copy->trigger();
        const size_t clips = state()->sequence()->videoTracks[0].clips.size();
        state()->setPlayhead(state()->sequence()->duration() + 10);
        paste->trigger();
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), clips + 1);

        // Clear Solo clears every solo and, used again, brings them back.
        QAction* clearSolo = win_->findChild<QAction*>("clearSolo");
        QVERIFY(clearSolo);
        QVERIFY(state()->edit("Solo", [](Project&, Sequence& s) {
            s.videoTracks[0].solo = true;
            s.audioTracks[0].solo = true;
            return true;
        }));
        const auto soloed = edit::soloedTracks(*state()->sequence());
        QCOMPARE(soloed.size(), size_t(2));
        clearSolo->trigger();
        QVERIFY(edit::soloedTracks(*state()->sequence()).empty());
        clearSolo->trigger();
        QCOMPARE(edit::soloedTracks(*state()->sequence()), soloed);
        // One undo step each.
        state()->undo();
        QVERIFY(edit::soloedTracks(*state()->sequence()).empty());
        state()->undo();
        QCOMPARE(edit::soloedTracks(*state()->sequence()), soloed);
        state()->setSelection({}, false);
    }

    void sidechainInInspector() {
        // A compressor on a music clip, keyed from the inspector by another audio track.
        state()->newProject();
        const QString wav = dir_.path() + "/sc-bed.wav";
        {
            WavWriter w;
            QVERIFY(w.open(wav, 48000, 1));
            std::vector<float> tone(48000, 0.1f);
            w.write(tone.data(), int(tone.size()));
            QVERIFY(w.close());
        }
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        Id clip = 0;
        QVERIFY(state()->edit("Bed", [&](Project& p, Sequence& s) {
            while (s.audioTracks.size() < 2) edit::addTrack(p, s, TrackKind::Audio);
            s.audioTracks[1].name = "Dialogue";
            const auto r = edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, -1}, {TrackKind::Audio, 0}, false);
            if (!r.ok) return false;
            clip = r.created.front();
            edit::clipById(s, clip)->effects.push_back(makeEffect(p, "compressor"));
            return true;
        }));
        state()->setSelection({clip}, false);
        QApplication::processEvents();
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector && inspector->widget());
        auto* key = inspector->widget()->findChild<QComboBox*>("track_sidechain");
        QVERIFY(key);
        // None, then every audio track but the clip's own (a key cannot be its own signal).
        QCOMPARE(key->count(), int(state()->sequence()->audioTracks.size()));
        for (int i = 0; i < key->count(); ++i) QVERIFY(!key->itemText(i).startsWith("A1 "));
        QCOMPARE(key->currentIndex(), 0);  // None
        const int dialogue = key->findText("A2  Dialogue");
        QVERIFY(dialogue > 0);
        key->setCurrentIndex(dialogue);
        emit key->activated(dialogue);
        const auto keyOf = [&] { return edit::clipById(*state()->sequence(), clip)->effects.at(0).s("sidechain"); };
        QCOMPARE(keyOf(), std::to_string(state()->sequence()->audioTracks[1].id));
        state()->undo();
        QVERIFY(keyOf().empty());
        QApplication::processEvents();
        key = inspector->widget()->findChild<QComboBox*>("track_sidechain");
        QVERIFY(key && key->currentIndex() == 0);

        // A plugin with a key input offers the same choice (one without does not).
        plugins::Registry& reg = plugins::Registry::instance();
        reg.setCachePath((dir_.path() + "/plugin-cache-sc.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        for (plugins::Format f : plugins::kAllFormats) reg.setSearchPaths(f, {"/nonexistent-montage-test-dir"});
        reg.setSearchPaths(plugins::Format::Clap, {QStringLiteral(MONTAGE_TEST_CLAP_DIR "/sidechain").toStdString(),
                                                   QStringLiteral(MONTAGE_TEST_CLAP_DIR "/good").toStdString()});
        reg.scan();
        auto ducker = reg.find("clap:org.montage.test.ducker"), gain = reg.find("clap:org.montage.test.gain");
        QVERIFY(ducker && gain);
        QVERIFY(state()->edit("Plugins", [&](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, clip);
            c->effects.clear();
            auto e = plugins::makePluginEffect(p, *gain);
            auto d = plugins::makePluginEffect(p, *ducker);
            if (!e || !d) return false;
            c->effects.push_back(*e);
            c->effects.push_back(*d);
            return true;
        }));
        state()->setSelection({}, false);
        state()->setSelection({clip}, false);
        QApplication::processEvents();
        const auto keys = inspector->widget()->findChildren<QComboBox*>("track_sidechain");
        QCOMPARE(keys.size(), 1);  // the ducker's, not the gain's
        key = keys.front();
        key->setCurrentIndex(key->findText("A2  Dialogue"));
        emit key->activated(key->currentIndex());
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->effects.at(1).s("sidechain"), std::to_string(state()->sequence()->audioTracks[1].id));
        QVERIFY(edit::clipById(*state()->sequence(), clip)->effects.at(0).s("sidechain").empty());
    }

    void sharedProjectLocking() {
        state()->newProject();
        QVERIFY(QDir().mkpath(dir_.path() + "/shared"));
        const QString path = dir_.path() + "/shared/Reel 1.montage";
        // Someone else on another machine is editing it.
        auto foreignLock = [](const QString& project) {
            const QString t = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            QFile f(QString::fromStdString(lockPathFor(project.toStdString())));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(QJsonObject{{"user", "sam"}, {"host", "edit-bay-2"}, {"app", "Montage"}, {"pid", 4242.0}, {"since", t}, {"heartbeat", t}}).toJson());
        };
        auto theirSave = [](const QString& project, const std::string& name, int ahead) {
            Project p;
            QVERIFY(loadProject(project.toStdString(), p) || (p = makeDefaultProject(), true));
            p.active()->name = name;
            QVERIFY(saveProject(p, project.toStdString()));
            QFile f(project);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.setFileTime(QDateTime::currentDateTime().addSecs(ahead), QFileDevice::FileModificationTime);  // a later save
        };
        {
            Project p = makeDefaultProject();
            p.active()->name = "Cut 1";
            QVERIFY(saveProject(p, path.toStdString()));
        }
        foreignLock(path);
        QVERIFY(win_->openProjectAs(path, MainWindow::OpenMode::ReadOnly));
        QVERIFY(state()->readOnly() && !state()->holdsLock());
        QCOMPARE(state()->lockHolder(), QString("sam on edit-bay-2"));
        QVERIFY2(win_->windowTitle().contains("Read-Only: sam on edit-bay-2"), qPrintable(win_->windowTitle()));
        // Nothing changes it, and it cannot be saved over theirs.
        QVERIFY(!state()->edit("Rename", [](Project&, Sequence& s) {
            s.name = "Mine";
            return true;
        }));
        QVERIFY(state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")}).empty());
        QString err;
        QVERIFY(!state()->save(path, &err) && err.contains("sam"));
        QVERIFY(!win_->findChild<QAction*>("takeEdit")->isEnabled());
        QVERIFY(!state()->open(path, &err, EditorState::Access::Edit) && err.contains("sam"));
        // Their save is shown here.
        theirSave(path, "Cut 2", 2);
        state()->checkSharedState();
        QCOMPARE(state()->sequence()->name, std::string("Cut 2"));
        // They close it after one more save: it can be edited here, from what they saved last.
        theirSave(path, "Cut 3", 4);
        QFile::remove(QString::fromStdString(lockPathFor(path.toStdString())));
        QVERIFY(state()->canTakeEdit());
        state()->checkSharedState();
        QVERIFY(win_->findChild<QAction*>("takeEdit")->isEnabled());
        QVERIFY2(state()->takeEdit(&err), qPrintable(err));
        QCOMPARE(state()->sequence()->name, std::string("Cut 3"));
        QVERIFY(!state()->readOnly() && state()->holdsLock());
        QCOMPARE(projectLockStatus(path.toStdString()).state, LockState::Mine);
        QVERIFY(!win_->windowTitle().contains("Read-Only"));
        QVERIFY(state()->edit("Rename", [](Project&, Sequence& s) {
            s.name = "Mine";
            return true;
        }));
        QVERIFY2(state()->save(path, &err), qPrintable(err));
        // Marks on a read-only project are not changes to keep (tested below, once read-only again).
        // A network blip: the lock file gone for a moment is written again, and the project stays this editor's.
        QFile::remove(QString::fromStdString(lockPathFor(path.toStdString())));
        state()->checkSharedState(true);
        QVERIFY(state()->holdsLock() && !state()->readOnly());
        QCOMPARE(projectLockStatus(path.toStdString()).state, LockState::Mine);
        // Taken over while this machine slept, with changes not yet saved: read-only, and their saves never replace
        // those changes; Edit Project will not discard them unless asked.
        QVERIFY(state()->edit("Rename", [](Project&, Sequence& s) {
            s.name = "Unsaved";
            return true;
        }));
        foreignLock(path);
        state()->checkSharedState(true);
        QVERIFY(state()->readOnly() && !state()->holdsLock() && state()->isModified());
        theirSave(path, "Their Cut", 8);
        state()->checkSharedState();
        QCOMPARE(state()->sequence()->name, std::string("Unsaved"));
        QFile::remove(QString::fromStdString(lockPathFor(path.toStdString())));
        QVERIFY(!state()->takeEdit(&err) && err.contains("not saved"));
        QCOMPARE(state()->sequence()->name, std::string("Unsaved"));
        QVERIFY2(state()->takeEdit(&err, true), qPrintable(err));
        QCOMPARE(state()->sequence()->name, std::string("Their Cut"));
        QVERIFY(!state()->isModified() && state()->holdsLock());
        // Closing it lets it go.
        state()->newProject();
        QCOMPARE(projectLockStatus(path.toStdString()).state, LockState::Free);
        // Saving a read-only project as a copy makes the copy this editor's.
        foreignLock(path);
        QVERIFY(win_->openProjectAs(path, MainWindow::OpenMode::ReadOnly));
        state()->setInPoint(10);
        QVERIFY(!state()->isModified());  // marks on a read-only project are not changes
        const QString copy = dir_.path() + "/shared/Reel 1 (mine).montage";
        QVERIFY2(state()->save(copy, &err), qPrintable(err));
        QVERIFY(!state()->readOnly() && state()->holdsLock());
        QCOMPARE(projectLockStatus(copy.toStdString()).state, LockState::Mine);
        QCOMPARE(projectLockStatus(path.toStdString()).state, LockState::Theirs);
        state()->newProject();
        QCOMPARE(projectLockStatus(copy.toStdString()).state, LockState::Free);

        // A production: its projects in the Production panel with who is editing each.
        const QString prod = dir_.path() + "/Feature";
        QVERIFY(win_->openProduction(prod, true, "The Feature"));
        const QString reel1 = prod + "/Reel 1.montage", reel2 = prod + "/Reel 2.montage";
        {
            Project p = makeDefaultProject();
            p.active()->name = "Reel 1 Cut";
            QVERIFY(saveProject(p, reel1.toStdString()));
            p.active()->name = "Reel 2 Cut";
            QVERIFY(saveProject(p, reel2.toStdString()));
        }
        foreignLock(reel2);
        ProductionPanel* panel = win_->productionPanel();
        QVERIFY(panel && panel->folder() == QDir(prod).absolutePath());
        panel->refresh();
        auto* tree = panel->findChild<QTreeWidget*>("productionProjects");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        QVERIFY(panel->findChild<QLabel*>("productionTitle")->text().contains("The Feature"));
        QCOMPARE(tree->topLevelItem(0)->text(1), QString("Free"));
        QVERIFY(tree->topLevelItem(1)->text(1).contains("sam on edit-bay-2"));
        // Opened from the panel: editing it.
        emit panel->openRequested(reel1, false);
        QVERIFY(state()->holdsLock() && !state()->readOnly());
        QCOMPARE(state()->filePath(), reel1);
        panel->refresh();
        QCOMPARE(tree->topLevelItem(0)->text(1), QString("Editing (you)"));
        // A sequence brought in from the reel someone else is editing (read, not changed), one undo step.
        const std::vector<Id> made = win_->importSequencesFrom(reel2, {"Reel 2 Cut"}, &err);
        QVERIFY2(made.size() == 1, qPrintable(err));
        QCOMPARE(state()->sequence()->name, std::string("Reel 2 Cut"));
        QVERIFY(state()->project().findSequence(made[0]));
        state()->undo();
        QVERIFY(!state()->project().findSequence(made[0]));
        QVERIFY(win_->importSequencesFrom(reel2, {"No Such Cut"}, &err).empty() && !err.isEmpty());
        QVERIFY(win_->findChild<QAction*>("newProduction") && win_->findChild<QAction*>("openProduction") &&
                win_->findChild<QAction*>("importFromProject"));
        state()->newProject();
        QFile::remove(QString::fromStdString(lockPathFor(reel2.toStdString())));
        QFile::remove(QString::fromStdString(lockPathFor(path.toStdString())));
    }

    void offloadCardDialog() {
        state()->newProject();
        // A card: a recording and its sidecar.
        const QString card = dir_.path() + "/cards/A007";
        QVERIFY(QDir().mkpath(card + "/CLIPS"));
        {
            WavWriter w;
            QVERIFY(w.open(card + "/CLIPS/A007C001.wav", 48000, 1));
            std::vector<float> tone(4800, 0.1f);
            w.write(tone.data(), int(tone.size()));
            QVERIFY(w.close());
            QFile f(card + "/A007.xml");
            QVERIFY(f.open(QIODevice::WriteOnly) && f.write("<clip/>") > 0);
        }
        QVERIFY(win_->findChild<QAction*>("offloadCard") && win_->findChild<QAction*>("verifyMhl"));
        OffloadDialog dlg(state(), win_.get());
        dlg.findChild<QListWidget*>("offloadDestinations")->clear();  // whatever an earlier run remembered
        dlg.setSource(card);
        dlg.addDestination(dir_.path() + "/drive1");
        dlg.addDestination(dir_.path() + "/drive2");
        dlg.findChild<QCheckBox*>("offloadImport")->setChecked(true);
        dlg.findChild<QCheckBox*>("offloadVerify")->setChecked(true);
        dlg.findChild<QCheckBox*>("offloadMhl")->setChecked(true);
        const OffloadResult r = dlg.run();
        QVERIFY2(r.ok, qPrintable(dlg.report()));
        QVERIFY2(dlg.report().contains("every copy matches"), qPrintable(dlg.report()));
        QVERIFY(QFileInfo::exists(dir_.path() + "/drive2/A007/CLIPS/A007C001.wav"));
        QCOMPARE(QDir(dir_.path() + "/drive1/A007/ascmhl").entryList({"*.mhl"}).size(), qsizetype(1));
        // The copied recording, in a bin named after the card (the sidecar is not media).
        const auto& media = state()->project().media;
        QCOMPARE(media.size(), size_t(1));
        QCOMPARE(media[0].bin, std::string("A007"));
        QVERIFY(QString::fromStdString(media[0].path).startsWith(dir_.path() + "/drive1/A007/"));
        // Verified later, with a generation recorded.
        QString report;
        const MhlVerifyResult v = verifyMhlWithProgress(win_.get(), dir_.path() + "/drive2/A007", true, &report);
        QVERIFY2(v.ok && v.verified == 2 && !v.generation.isEmpty(), qPrintable(report));
        QVERIFY2(report.contains("match"), qPrintable(report));
    }

    void syncIndicators() {
        state()->newProject();
        Id v = 0, a = 0;
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Video;
            m.name = "sync.mov";
            m.path = (dir_.path() + "/missing-sync.mov").toStdString();
            m.duration = 10;
            m.width = 1920, m.height = 1080;
            m.fps = {30, 1};
            m.hasVideo = m.hasAudio = true;
            p.media.push_back(m);
            const auto r = edit::placeMedia(p, s, m.id, 30, 30, 90, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            if (!r.ok) return false;
            v = r.created[0], a = r.created[1];
            return true;
        }));
        TimelineWidget* tl = win_->timeline();
        for (int k = 0; k < 4; ++k) tl->zoomIn();  // clips wide enough for the badge
        QCOMPARE(tl->syncOffsetOf(a), 0.0);
        const QImage inSync = tl->viewport()->grab().toImage();
        // The sound alone nudged 6 frames later: a red +6 on it, none on the picture.
        QVERIFY(state()->apply("Nudge", [a](Project& p, Sequence& s) { return edit::moveClips(p, s, {a}, 6, 0, 0, false); }));
        QCOMPARE(tl->syncOffsetOf(a), 6.0);
        QCOMPARE(tl->syncOffsetOf(v), 0.0);
        QVERIFY(tl->viewport()->grab().toImage() != inSync);
        // Right-click it: Slip into Sync, then (undone) Move into Sync.
        auto viaMenu = [&](const char* name) {
            bool triggered = false;
            QTimer::singleShot(0, this, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                if (auto* act = menu->findChild<QAction*>(name)) {
                    act->trigger();
                    triggered = true;
                }
                menu->close();
            });
            const QPoint at = tl->clipBounds(a).center();
            QContextMenuEvent ev(QContextMenuEvent::Mouse, at, tl->viewport()->mapToGlobal(at));
            QApplication::sendEvent(tl->viewport(), &ev);
            return triggered;
        };
        QVERIFY(viaMenu("slipIntoSync"));
        QCOMPARE(tl->syncOffsetOf(a), 0.0);
        QCOMPARE(edit::clipById(*state()->sequence(), a)->sourceIn, 36.0);
        QCOMPARE(edit::clipById(*state()->sequence(), a)->start, FrameTime(36));
        state()->undo();
        QCOMPARE(tl->syncOffsetOf(a), 6.0);
        QVERIFY(viaMenu("moveIntoSync"));
        QCOMPARE(edit::clipById(*state()->sequence(), a)->start, FrameTime(30));
        QCOMPARE(tl->syncOffsetOf(a), 0.0);
        QVERIFY(!viaMenu("moveIntoSync"));  // offered only while out of sync
        // Quality Check reports it.
        state()->undo();
        QcSettings q;
        q.flashing = q.levels = q.clipping = q.spelling = false;
        q.blackSeconds = q.freezeSeconds = q.silenceSeconds = 0;
        const auto issues = qualityCheck(state()->project(), *state()->sequence(), 0, -1, q);
        QCOMPARE(issues.size(), size_t(1));
        QCOMPARE(issues[0].kind, QcKind::OutOfSync);
        QCOMPARE(issues[0].start, FrameTime(36));
        QVERIFY(issues[0].text.find("6 frames late") != std::string::npos);
        q.sync = false;
        QVERIFY(qualityCheck(state()->project(), *state()->sequence(), 0, -1, q).empty());
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

    void watchFolderImports() {
        const QString folder = dir_.path() + "/incoming";
        QDir().mkpath(folder + "/day2");
        auto png = [&](const QString& file, QRgb colour) {
            QImage img(32, 18, QImage::Format_RGB32);
            img.fill(colour);
            return img.save(file);
        };
        QVERIFY(png(folder + "/first.png", qRgb(200, 0, 0)));
        QFile junk(folder + "/notes.txt");
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not media");
        junk.close();
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("watchFolders"));
        const size_t before = state()->project().media.size();
        // What is there comes in once it has settled; a text file never does.
        QVERIFY(state()->addWatchFolder(folder));
        QVERIFY(!state()->addWatchFolder(folder));  // already watched
        QTRY_COMPARE_WITH_TIMEOUT(state()->project().media.size(), before + 1, 8000);
        QCOMPARE(state()->project().media.back().name, std::string("first.png"));
        QCOMPARE(state()->project().media.back().bin, std::string("Watch Folder - incoming"));
        // A file arriving later, in a subfolder too.
        QVERIFY(png(folder + "/day2/second.png", qRgb(0, 200, 0)));
        QTRY_COMPARE_WITH_TIMEOUT(state()->project().media.size(), before + 2, 8000);
        QCOMPARE(state()->project().media.back().name, std::string("second.png"));
        // Kept with the project.
        QVERIFY(state()->save(dir_.path() + "/watch.montage"));
        state()->newProject();
        QVERIFY(state()->open(dir_.path() + "/watch.montage"));
        QCOMPARE(state()->project().watchFolders.size(), size_t(1));
        QVERIFY(state()->scanWatchFolders().empty());  // nothing new
        // No longer watched: nothing more comes in.
        QVERIFY(state()->removeWatchFolder(folder));
        const size_t now = state()->project().media.size();
        QVERIFY(png(folder + "/third.png", qRgb(0, 0, 200)));
        QTest::qWait(2500);
        QCOMPARE(state()->project().media.size(), now);
        state()->newProject();
    }

    void removeLetterboxFromTheMenu() {
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 320, gs.height = 180, gs.fps = {25, 1};
        Clip box = makeGeneratorClip(gen, "shape", 25);
        box.generator.params["width"] = Param(320.0);
        box.generator.params["height"] = Param(120.0);
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, box);
        ExportSettings st;
        st.path = (dir_.path() + "/bars.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("removeLetterbox"));
        QCOMPARE(win_->removeLetterbox(), 0);  // nothing selected
        const auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids.at(0), 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->setSelection({clip}, false);
        QCOMPARE(win_->removeLetterbox(), 1);
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c->motion.p("crop_top", 0) > 15 && c->motion.p("crop_bottom", 0) > 15);
        QVERIFY(c->motion.p("scale", 0) > 140);
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->motion.p("crop_top", 0), 0.0);
        state()->newProject();
    }

    void exportDcpFromTheFileMenu() {
        // A second of a colour matte in a 2.39:1 sequence: the dialog would pick Scope.
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("exportDcp"));
        QVERIFY(state()->edit("Matte", [&](Project& p, Sequence& s) {
            s.width = 96, s.height = 40, s.fps = Rational{24, 1};
            Clip c = makeGeneratorClip(p, "color", 24);
            c.generator.params["color.r"] = 0.8;
            return edit::overwrite(p, s, V1, c).ok;
        }));
        QCOMPARE(defaultDcpContainer(*state()->sequence()), std::string("scope"));
        DcpSettings st;
        st.title = "Menu Test";
        st.container = "scope";
        st.threads = 2;
        QStringList problems;
        const QString folder = win_->exportDcpTo(dir_.path(), st, &problems);
        QVERIFY2(!folder.isEmpty(), qPrintable(win_->statusBar()->currentMessage()));
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        QVERIFY(QFileInfo(folder).fileName().startsWith("MenuTest_FTR_S_EN-XX_XX_51_2K_"));
        QVERIFY2(win_->statusBar()->currentMessage().contains("checked"), qPrintable(win_->statusBar()->currentMessage()));
        // Over MCP: made and checked; a damaged package reported.
        QVERIFY(state()->save(dir_.filePath("dcp.montage")));
        McpServer server;
        auto call = [&](const QString& tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_export_dcp", {{"project", dir_.filePath("dcp.montage")}, {"folder", dir_.filePath("mcp")}, {"title", "Over MCP"},
                                                   {"kind", "trailer"}, {"studio", "XY"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject made = r.value("structuredContent").toObject();
        QVERIFY2(made.value("name").toString().startsWith("OverMCP_TLR_S_EN-XX_XX_51_2K_XY_"), qPrintable(made.value("name").toString()));
        QVERIFY(made.value("problems").toArray().isEmpty() && made.value("frames").toInt() == 24 && made.value("width").toInt() == 2048);
        r = call("montage_verify_dcp", {{"folder", made.value("folder").toString()}});
        QVERIFY(r.value("structuredContent").toObject().value("ok").toBool());
        QFile::remove(QDir(made.value("folder").toString()).filePath("VOLINDEX.xml"));
        r = call("montage_verify_dcp", {{"folder", made.value("folder").toString()}});
        QVERIFY(!r.value("structuredContent").toObject().value("ok").toBool());
        QVERIFY(call("montage_export_dcp", {{"project", dir_.filePath("dcp.montage")}, {"folder", dir_.filePath("mcp")}, {"container", "imax"}})
                    .value("isError").toBool());
        state()->newProject();
    }

    void exportImfFromTheFileMenu() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("exportImf"));
        if (!openJpegAvailable()) QSKIP("Built without OpenJPEG 2.5");
        QVERIFY(state()->edit("Matte", [&](Project& p, Sequence& s) {
            s.width = 128, s.height = 72, s.fps = Rational{25, 1};
            Clip c = makeGeneratorClip(p, "color", 25);
            c.generator.params["color.g"] = 0.6;
            return edit::overwrite(p, s, V1, c).ok;
        }));
        ImfSettings st;
        st.title = "Menu Master";
        st.threads = 2;
        QStringList problems;
        const QString folder = win_->exportImfTo(dir_.path(), st, &problems);
        QVERIFY2(!folder.isEmpty(), qPrintable(win_->statusBar()->currentMessage()));
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        QVERIFY(QFileInfo(folder).fileName().startsWith("Menu_Master_IMF_"));
        QVERIFY2(win_->statusBar()->currentMessage().contains("checked"), qPrintable(win_->statusBar()->currentMessage()));
        // Over MCP: made (HDR, 12-bit, lossy) and checked; a package missing its asset map reported.
        QVERIFY(state()->save(dir_.filePath("imf.montage")));
        McpServer server;
        auto call = [&](const QString& tool, const QJsonObject& args) {
            const QJsonObject req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
            const auto lines = server.handle(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString());
            return QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object().value("result").toObject();
        };
        QJsonObject r = call("montage_export_imf", {{"project", dir_.filePath("imf.montage")}, {"folder", dir_.filePath("mcp-imf")}, {"title", "Over MCP"},
                                                   {"kind", "episode"}, {"colour", "rec2020-pq"}, {"lossless", false}, {"megabits_per_second", 100}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        const QJsonObject made = r.value("structuredContent").toObject();
        QVERIFY(made.value("problems").toArray().isEmpty() && made.value("frames").toInt() == 25 && made.value("bits").toInt() == 12);
        QVERIFY(made.value("colour").toString() == "rec2020-pq" && made.value("edit_rate").toString() == "25/1" && made.value("rsiz").toInt() == 0x0411);
        r = call("montage_verify_imf", {{"folder", made.value("folder").toString()}});
        QVERIFY(r.value("structuredContent").toObject().value("ok").toBool());
        QFile::remove(QDir(made.value("folder").toString()).filePath("ASSETMAP.xml"));
        r = call("montage_verify_imf", {{"folder", made.value("folder").toString()}});
        QVERIFY(!r.value("structuredContent").toObject().value("ok").toBool());
        QVERIFY(call("montage_export_imf", {{"project", dir_.filePath("imf.montage")}, {"folder", dir_.filePath("mcp-imf")}, {"colour", "sepia"}})
                    .value("isError").toBool());
        state()->newProject();
    }

    void spectralRepairFromTheClipMenu() {
        // Three seconds of a 440 Hz voice with a 3 kHz whistle from 1.0 to 1.5 s.
        const int rate = 48000;
        std::vector<float> x(size_t(rate) * 3);
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = double(i) / rate;
            x[i] = float(0.2 * std::sin(2 * M_PI * 440 * t) + (t >= 1.0 && t < 1.5 ? 0.3 * std::sin(2 * M_PI * 3000 * t) : 0));
        }
        WavWriter w;
        QVERIFY(w.open(dir_.path() + "/whistle.wav", rate, 1));
        w.write(x.data(), int64_t(x.size()));
        QVERIFY(w.close());
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("spectralRepair"));
        QVERIFY(!win_->spectralRepairDialog());  // nothing to repair
        const auto ids = state()->importFiles({dir_.path() + "/whistle.wav"});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false).ok; }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        state()->setSelection({clip}, false);
        SpectralRepairDialog* dlg = win_->spectralRepairDialog();
        QVERIFY(dlg && dlg->isVisible());
        // The spectrogram: the whistle bright at 3 kHz in its half second, dark there before it.
        auto level = [&](double t, double hz) {
            const QImage& im = dlg->view()->picture();
            const double minHz = dlg->view()->minHz();
            const int px = std::clamp(int(t / 3.0 * im.width()), 0, im.width() - 1);
            const int py = std::clamp(int(std::lround((im.height() - 1) * (1 - std::log(hz / minHz) / std::log(24000 / minHz)))), 0, im.height() - 1);
            return qGray(im.pixel(px, py));
        };
        const int whistle = level(1.25, 3000), before = level(0.5, 3000);
        QVERIFY2(whistle > before + 80, qPrintable(QString("%1 %2").arg(whistle).arg(before)));
        // A box across its moment, fitted to its frequencies, then healed: one undo step on the clip.
        QVERIFY(!dlg->heal());  // nothing chosen yet
        dlg->setSelection(1.0, 1.5, 0, 0);
        QVERIFY(dlg->status().contains("all frequencies"));
        QVERIFY(dlg->findFrequencies());
        QVERIFY2(dlg->view()->selectionLow() < 3000 && dlg->view()->selectionHigh() > 3000 && dlg->view()->selectionHigh() - dlg->view()->selectionLow() < 400,
                 qPrintable(dlg->status()));
        QVERIFY(dlg->heal());
        QCOMPARE(state()->undoText(), QString("Spectral Heal"));
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c && c->effects.front().type == "spectral_repair");
        QCOMPARE(dlg->regions().size(), size_t(1));
        QVERIFY2(level(1.25, 3000) < whistle - 80, qPrintable(QString::number(level(1.25, 3000))));  // the picture shows it gone
        QVERIFY(std::abs(level(1.25, 440) - level(0.5, 440)) < 30);  // the voice still there
        // Undo and redo show in the editor.
        state()->undo();
        QVERIFY(dlg->regions().empty() && level(1.25, 3000) > whistle - 20);
        state()->redo();
        QCOMPARE(dlg->regions().size(), size_t(1));
        // Attenuate the whole of a stretch, then take it back; Clear All removes the effect.
        dlg->setSelection(2.0, 2.2, 0, 0);
        dlg->setAllFrequencies(true);
        QVERIFY(dlg->attenuate(-12));
        QCOMPARE(dlg->regions().size(), size_t(2));
        QVERIFY(dlg->regions()[1].mode == "attenuate" && dlg->regions()[1].gainDb == -12 && dlg->regions()[1].high == 0);
        QVERIFY(dlg->removeLast());
        QCOMPARE(dlg->regions().size(), size_t(1));
        QVERIFY(dlg->clearAll());
        c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(std::none_of(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "spectral_repair"; }));
        dlg->close();
        state()->newProject();
    }

    void removeMicBleedFromTheMenu() {
        // Two mics: the first speaker loud on one and 18 dB down on the other, then the second speaker the other way.
        const int rate = 48000;
        std::vector<float> a(size_t(rate) * 6), b(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            const double t = double(i) / rate;
            const double one = t >= 0.5 && t < 2.5 ? 0.3 * std::sin(2 * M_PI * 300 * t) : 0.0;
            const double two = t >= 3 && t < 5 ? 0.3 * std::sin(2 * M_PI * 500 * t) : 0.0;
            a[i] = float(one + two / 8);
            b[i] = float(two + one / 8);
        }
        auto writeWav = [](const QString& file, const std::vector<float>& x) {
            WavWriter w;
            if (!w.open(file, 48000, 1)) return false;
            w.write(x.data(), int64_t(x.size()));
            return w.close();
        };
        QVERIFY(writeWav(dir_.path() + "/host.wav", a));
        QVERIFY(writeWav(dir_.path() + "/guest.wav", b));
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("removeMicBleed"));
        QCOMPARE(win_->removeMicBleed(), 0);  // nothing on the tracks
        const auto ids = state()->importFiles({dir_.path() + "/host.wav", dir_.path() + "/guest.wav"});
        QCOMPARE(ids.size(), size_t(2));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok &&
                   edit::placeMedia(p, s, ids[1], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 1}, false).ok;
        }));
        QCOMPARE(win_->removeMicBleed(), 2);
        const Sequence* s = state()->sequence();
        const double fps = s->fpsValue();
        auto gain = [&](int track, double t) { return s->audioTracks[size_t(track)].clips.at(0).audio.p("gain_db", FrameTime(std::llround(t * fps))); };
        QVERIFY(std::fabs(gain(0, 4.0) + 24) < 0.5);  // the host's mic down while the guest speaks
        QVERIFY(std::fabs(gain(1, 1.5) + 24) < 0.5);
        QVERIFY(std::fabs(gain(0, 1.5)) < 0.5 && std::fabs(gain(1, 4.0)) < 0.5);
        state()->undo();
        QVERIFY(state()->sequence()->audioTracks[0].clips.at(0).audio.params.at("gain_db").keys.empty());
        state()->newProject();
    }

    void importEmbeddedClosedCaptions() {
        // A clip whose file carries CEA-608 captions.
        Project gen = makeDefaultProject();
        Sequence& gs = *gen.active();
        gs.width = 160, gs.height = 90, gs.fps = {30, 1};
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "color", 150));
        CaptionTrack ct;
        ct.id = gen.newId();
        ct.captions = {{30, 75, "Closed captions", {}}, {90, 140, "Second one", {}}};
        gs.captionTracks.push_back(ct);
        ExportSettings st;
        st.path = (dir_.path() + "/cc.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        st.cea608 = true;
        std::string err;
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        QCOMPARE(win_->importEmbeddedCaptions(), 0);  // no clip
        const auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids.at(0), 60, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        state()->setPlayhead(100);
        QVERIFY(win_->findChild<QAction*>("importEmbeddedCaptions"));
        QCOMPARE(win_->importEmbeddedCaptions(), 2);
        const Sequence* s = state()->sequence();
        QCOMPARE(s->captionTracks.size(), size_t(1));
        const double scale = s->fpsValue() / 30.0;
        QVERIFY(std::llabs(s->captionTracks[0].captions[0].start - (60 + FrameTime(std::llround(30 * scale)))) <= 2);
        QCOMPARE(s->captionTracks[0].captions[1].text, std::string("Second one"));
        state()->undo();
        QVERIFY(state()->sequence()->captionTracks.empty());
        state()->newProject();
    }

    void layeredPsdImport() {
        using Px = std::array<uint16_t, 4>;
        auto write = [&](const QString& file, uint16_t red) {
            std::vector<TestPsdLayer> layers(2);
            layers[0].name = "Back", layers[0].right = 64, layers[0].bottom = 36;
            layers[0].pixel = [](int, int) { return Px{0, 0, 0, 65535}; };
            layers[1].name = "Badge", layers[1].left = 16, layers[1].top = 8, layers[1].right = 48, layers[1].bottom = 28, layers[1].compression = 1;
            layers[1].pixel = [red](int, int) { return Px{red, 0, uint16_t(65535 - red), 65535}; };
            return testpsd::write(file, 64, 36, 8, false, layers, [](int, int) { return Px{0, 0, 0, 65535}; });
        };
        const QString file = dir_.path() + "/badge.psd";
        QVERIFY(write(file, 65535));
        state()->newProject();
        // As a sequence, one undo step.
        const auto ids = state()->importPsd(file, PsdImport::Sequence);
        QCOMPARE(ids.size(), size_t(3));
        const MediaItem* seqItem = state()->project().findMedia(ids.back());
        QCOMPARE(seqItem->kind, MediaKind::Sequence);
        const Sequence* s = state()->project().findSequence(seqItem->sequenceId);
        QCOMPARE(s->videoTracks.size(), size_t(2));
        QCOMPARE(s->videoTracks[1].name, std::string("Badge"));
        state()->undo();
        QVERIFY(!state()->project().findMedia(ids.back()));
        // importFiles follows the preference: merged by default, else layers or a sequence.
        QCOMPARE(state()->importFiles({file}).size(), size_t(1));
        appSettings().setValue("import/psd", "layers");
        QCOMPARE(state()->importFiles({file}).size(), size_t(2));
        appSettings().setValue("import/psd", "sequence");
        const auto again = state()->importFiles({file});
        appSettings().remove("import/psd");
        QCOMPARE(again.size(), size_t(3));
        QString why;
        QVERIFY(state()->importPsd(dir_.path() + "/missing.psd", PsdImport::Layers, &why).empty());
        QVERIFY(!why.isEmpty());
        // A layer in the cut follows the file when it is saved again.
        const Id badge = again[1];
        QCOMPARE(state()->project().findMedia(badge)->name, std::string("badge - Badge"));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& sq) {
            return edit::placeMedia(p, sq, badge, 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        auto centre = [&]() {
            const Image f = renderSequenceFrame(state()->project(), *state()->sequence(), 5, {});
            const size_t i = (size_t(f.height / 2) * size_t(f.width) + size_t(f.width / 2)) * 4;
            return QColor::fromRgbF(std::clamp(f.px[i], 0.f, 1.f), std::clamp(f.px[i + 1], 0.f, 1.f), std::clamp(f.px[i + 2], 0.f, 1.f));
        };
        QVERIFY(centre().red() > 200);
        QSignalSpy spy(state(), &EditorState::mediaFileChanged);
        QVERIFY(write(file, 0));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(centre().blue() > 200 && centre().red() < 50, 5000);
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
        // Closing the project lets its files go (on Windows a folder with an open file cannot be renamed); background
        // thumbnail and audio jobs may hold one a moment longer.
        // (QTRY_ evaluates its condition again once it holds, so the rename is latched.)
        bool renamed = false;
        QTRY_VERIFY_WITH_TIMEOUT(renamed || (renamed = QDir().rename(root + "/card", root + "/Card 2")), 10000);

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

    void hdrPaletteInInspector() {
        // A dark grey still with the HDR Palette on it.
        QImage frame(320, 180, QImage::Format_RGB32);
        frame.fill(QColor(50, 50, 50));
        const QString png = dir_.path() + "/palette.png";
        QVERIFY(frame.save(png));
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->edit("Palette", [clip](Project& p, Sequence& s) {
            edit::clipById(s, clip)->effects.push_back(makeEffect(p, "hdr_palette"));
            return true;
        });
        state()->setSelection({clip}, false);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        auto visible = [&]<typename W>(const char* name) -> W* {
            for (auto* w : win_->findChildren<W*>(name))
                if (w->isVisibleTo(win_.get())) return w;
            return nullptr;
        };
        auto shown = [&](const QString& text) {
            for (QLabel* l : win_->findChildren<QLabel*>())
                if (l->isVisibleTo(win_.get()) && l->text() == text) return true;
            return false;
        };
        // Global first: its wheel and sliders, none of the zones'.
        auto* zone = visible.template operator()<QComboBox>("hdrZone");
        QVERIFY(zone);
        QCOMPARE(zone->count(), 7);
        QVERIFY(visible.template operator()<ColorWheel>("wheel_global"));
        QVERIFY(shown("Global Exposure (stops)"));
        QVERIFY(!shown("Shadow Exposure (stops)"));
        // The Shadow zone: its own wheel and sliders.
        zone->setCurrentIndex(3);
        QTRY_VERIFY(shown("Shadow Exposure (stops)"));
        QVERIFY(!shown("Global Exposure (stops)"));
        QVERIFY(shown("Shadow Up To (stops)"));
        QCOMPARE(visible.template operator()<QComboBox>("hdrZone")->currentText(), QString("Shadow"));
        auto* wheel = visible.template operator()<ColorWheel>("wheel_shadow");
        QVERIFY(wheel);
        // Pushing the shadows towards red: balanced channel shifts, one undo step, a redder picture.
        RenderOptions o;
        o.displaySpace = "rec709";
        auto centre = [&] {
            const Image img = renderProgramFrame(state()->project(), *state()->sequence(), 5, o);
            const float* p = img.at(img.width / 2, img.height / 2);
            return std::array<float, 3>{p[0], p[1], p[2]};
        };
        const auto before = centre();
        wheel->setPuck({0.8, 0});
        const Effect* e = &edit::clipById(*state()->sequence(), clip)->effects.back();
        QVERIFY(e->p("shadow_r", 0) > 0.2 && e->p("shadow_g", 0) < 0);
        QVERIFY(std::fabs(e->p("shadow_r", 0) + e->p("shadow_g", 0) + e->p("shadow_b", 0)) < 1e-9);
        const auto after = centre();
        QVERIFY2(after[0] > before[0] + 0.02f && after[2] < before[2], qPrintable(QString("%1 %2").arg(after[0]).arg(after[2])));
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->effects.back().p("shadow_r", 0), 0.0);
        state()->newProject();
    }

    void colorSliceInInspector() {
        // Colour Slice shows one vector at a time: Global's settings, then a hue's own, then Skin's with its centre.
        QImage frame(64, 36, QImage::Format_RGB32);
        frame.fill(QColor(160, 60, 40));
        const QString png = dir_.path() + "/slice.png";
        QVERIFY(frame.save(png));
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->edit("Slice", [clip](Project& p, Sequence& s) {
            edit::clipById(s, clip)->effects.push_back(makeEffect(p, "color_slice"));
            return true;
        });
        state()->setSelection({clip}, false);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        auto vector = [&]() -> QComboBox* {
            for (auto* w : win_->findChildren<QComboBox*>("sliceVector"))
                if (w->isVisibleTo(win_.get())) return w;
            return nullptr;
        };
        auto shown = [&](const QString& text) {
            for (QLabel* l : win_->findChildren<QLabel*>())
                if (l->isVisibleTo(win_.get()) && l->text() == text) return true;
            return false;
        };
        QVERIFY(vector());
        QCOMPARE(vector()->count(), 8);
        QVERIFY(shown("Density") && shown("Depth"));
        QVERIFY(!shown("Red Density") && !shown("Skin Centre (°)"));
        vector()->setCurrentIndex(1);
        QTRY_VERIFY(shown("Red Density"));
        QVERIFY(shown("Red Hue (°)") && !shown("Depth") && !shown("Skin Centre (°)"));
        vector()->setCurrentIndex(7);
        QTRY_VERIFY(shown("Skin Centre (°)"));
        QVERIFY(shown("Skin Density") && shown("Skin Width (°)") && !shown("Red Density"));
        QCOMPARE(vector()->currentText(), QString("Skin"));
        vector()->setCurrentIndex(0);
        QTRY_VERIFY(shown("Depth"));
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

    void correctLensInTheApp() {
        // The test database stands in for lensfun's.
        QTemporaryDir lenses;
        QVERIFY(QFile::copy(QStringLiteral(MONTAGE_TEST_DATA_DIR "/lensfun/test-lenses.xml"), lenses.filePath("test-lenses.xml")));
        qputenv("MONTAGE_LENS_DB", lenses.path().toUtf8());
        reloadLensDatabase();
        struct Restore {
            ~Restore() {
                qunsetenv("MONTAGE_LENS_DB");
                reloadLensDatabase();
                lensDialogHook() = nullptr;
            }
        } restore;
        // A GoPro still, on the timeline twice.
        const QString file = dir_.filePath("gopro.png");
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(80, 110, 140));
        QVERIFY(img.save(file));
        state()->newProject();
        const auto ids = state()->importFiles({file});
        QCOMPARE(ids.size(), size_t(1));
        Id a = 0, b = 0;
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            p.findMedia(ids[0])->metadata["device"] = "GoPro HERO11 Black";
            s.width = 320, s.height = 180;
            if (!edit::placeMedia(p, s, ids[0], 0, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
            if (!edit::placeMedia(p, s, ids[0], 60, 0, 30, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
            a = s.videoTracks[0].clips[0].id, b = s.videoTracks[0].clips[1].id;
            return true;
        }));
        auto lensOf = [&](Id clip) -> const Effect* {
            for (const Effect& e : edit::clipById(*state()->sequence(), clip)->effects)
                if (e.type == "lens_correction") return &e;
            return nullptr;
        };
        state()->setSelection({a}, false);
        // The camera from the clip's Camera Model, its fixed lens with it; applied to this clip alone.
        bool shown = false;
        lensDialogHook() = [&](LensCorrectionDialog* dlg) {
            shown = true;
            QCOMPARE(dlg->cameraText(), QStringLiteral("GoPro Hero11 Black"));
            QCOMPARE(dlg->lensText(), QStringLiteral("GoPro HERO11 Black & compatibles"));
            QVERIFY(dlg->findChild<QPushButton*>("lensApply")->isEnabled());
            QVERIFY(dlg->findChild<QLabel*>("lensAbout")->text().contains("rectilinear"));
            dlg->findChild<QCheckBox*>()->setChecked(false);
            QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
        };
        QAction* action = win_->findChild<QAction*>("correctLens");
        QVERIFY(action);
        action->trigger();
        QVERIFY(shown);
        QVERIFY(lensOf(a) && !lensOf(b));
        QCOMPARE(edit::clipById(*state()->sequence(), a)->effects[0].type, std::string("lens_correction"));
        QCOMPARE(lensOf(a)->s("lens"), std::string("GoPro HERO11 Black & compatibles"));
        QCOMPARE(lensOf(a)->p("focal", 0), 2.7);
        // In the Inspector: the lens it holds, and Choose Lens… opens the dialog on it. Another camera: its lenses (a
        // lens left unchosen cannot be applied), then the fisheye from the adapter, at f/4 on both clips.
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        QLabel* name = nullptr;
        for (auto* l : win_->findChildren<QLabel*>("lensName"))
            if (l->isVisibleTo(win_.get())) name = l;
        QVERIFY(name);
        QVERIFY2(name->text().contains("HERO11") && name->text().contains("GoPro Hero11 Black"), qPrintable(name->text()));
        QPushButton* choose = nullptr;
        for (auto* c : win_->findChildren<QPushButton*>("chooseLens"))
            if (c->isVisibleTo(win_.get())) choose = c;
        QVERIFY(choose);
        shown = false;
        lensDialogHook() = [&](LensCorrectionDialog* dlg) {
            shown = true;
            QCOMPARE(dlg->lensText(), QStringLiteral("GoPro HERO11 Black & compatibles"));  // what it holds
            QVERIFY(dlg->setCamera("Sony ILCE-6400"));
            QVERIFY(dlg->lensText().isEmpty());
            QVERIFY(!dlg->findChild<QPushButton*>("lensApply")->isEnabled());
            QCOMPARE(dlg->findChild<QComboBox*>("lensLens")->count(), 2);
            // Typed names count: a lens part-typed is none, one typed whole (any case) is it; a camera typed brings
            // its lenses.
            auto* lensBox = dlg->findChild<QComboBox*>("lensLens");
            lensBox->setEditText("Samyang 8mm");
            QVERIFY(!dlg->findChild<QPushButton*>("lensApply")->isEnabled());
            lensBox->setEditText("samyang 8mm f/2.8 umc fish-eye");
            QVERIFY(dlg->findChild<QPushButton*>("lensApply")->isEnabled());
            QVERIFY(dlg->choice().lens);
            dlg->findChild<QComboBox*>("lensCamera")->setEditText("Sony ILCE-7M4");
            QCOMPARE(lensBox->count(), 1);
            QVERIFY(dlg->choice().camera && dlg->choice().camera->model == "ILCE-7M4");
            QVERIFY(dlg->setCamera("Sony ILCE-6400"));
            QVERIFY(dlg->setLens("Samyang 8mm f/2.8 UMC Fish-eye"));
            dlg->findChild<QDoubleSpinBox*>("lensAperture")->setValue(4);
            QVERIFY(dlg->sameMedia());
            QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
        };
        choose->click();
        QTRY_VERIFY(shown);
        QTRY_VERIFY(lensOf(b));
        for (Id clip : {a, b}) {
            QCOMPARE(lensOf(clip)->s("lens"), std::string("Samyang 8mm f/2.8 UMC Fish-eye"));
            QCOMPARE(lensOf(clip)->s("camera"), std::string("Sony ILCE-6400"));
            QCOMPARE(lensOf(clip)->p("aperture", 0), 4.0);
        }
        QCOMPARE(int(std::count_if(edit::clipById(*state()->sequence(), a)->effects.begin(), edit::clipById(*state()->sequence(), a)->effects.end(),
                                   [](const Effect& e) { return e.type == "lens_correction"; })),
                 1);
        // The aperture not known: no vignetting correction, and so when opened again.
        shown = false;
        lensDialogHook() = [&](LensCorrectionDialog* dlg) {
            shown = true;
            dlg->findChild<QDoubleSpinBox*>("lensAperture")->setValue(0);
            QMetaObject::invokeMethod(dlg, "accept", Qt::QueuedConnection);
        };
        QVERIFY(win_->correctLens());
        QVERIFY(shown);
        QCOMPARE(lensOf(a)->p("aperture", -1), 0.0);
        // Cancelled, nothing changes.
        const std::string profile = lensOf(a)->s("profile");
        shown = false;
        lensDialogHook() = [&](LensCorrectionDialog* dlg) {
            shown = true;
            QCOMPARE(dlg->findChild<QDoubleSpinBox*>("lensAperture")->value(), 0.0);
            QMetaObject::invokeMethod(dlg, "reject", Qt::QueuedConnection);
        };
        QVERIFY(!win_->correctLens());
        QVERIFY(shown);
        QCOMPARE(lensOf(a)->s("profile"), profile);
        // Played: filled to the frame's corners.
        const Image frame = renderProgramFrame(state()->project(), *state()->sequence(), 5, RenderOptions{});
        QVERIFY(frame.at(0, 0)[3] > 0.99f);
    }

    void contentCredentialsInTheApp() {
        // The Export dialog offers signing for files that can hold credentials, and not for MKV.
        state()->newProject();
        const QString certs = MONTAGE_TEST_DATA_DIR "/c2pa/chain.pem", key = MONTAGE_TEST_DATA_DIR "/c2pa/signer.key";
        appSettings().remove("credentials/certificate");
        appSettings().remove("credentials/key");
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->apply("Place", [&](Project& p, Sequence& s) { return edit::placeMedia(p, s, ids[0], 0, 0, 60, V1, A1, false); }));
        const QString out = dir_.path() + "/signed-in-app.wav";
        {
            ExportDialog ed(state(), win_.get());
            auto* sign = ed.findChild<QCheckBox*>("exportCredentials");
            auto* identity = ed.findChild<QPushButton*>("exportCredentialIdentity");
            auto* training = ed.findChild<QCheckBox*>("exportAllowAiTraining");
            auto* preset = ed.findChild<QComboBox*>("exportPreset");
            auto* path = ed.findChild<QLineEdit*>("exportPath");
            QVERIFY(sign && identity && training && preset && path);
            QVERIFY(identity->text().contains("Choose"));
            preset->setCurrentIndex(preset->findText("Audio - WAV 24-bit"));
            QCOMPARE(sign->isEnabled(), contentCredentialsAvailable());
            for (int i = 0; i < preset->count(); ++i)
                if (preset->itemText(i).contains("MKV")) {
                    preset->setCurrentIndex(i);
                    QVERIFY(!sign->isEnabled());
                    sign->setChecked(true);  // (remembered from another export): said to go out unsigned
                    QVERIFY(sign->text().contains("unsigned"));
                    sign->setChecked(false);
                    break;
                }
            if (!contentCredentialsAvailable()) QSKIP("Built without the C2PA library (scripts/build-c2pa.sh) for the rest");
            preset->setCurrentIndex(preset->findText("Audio - WAV 24-bit"));
            path->setText(out);
            // Asked to sign without a certificate: nothing is exported.
            sign->setChecked(true);
            QVERIFY(training->isEnabled() && !training->isChecked());
            QTimer::singleShot(0, [] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->accept();
            });
            ed.findChild<QPushButton*>("exportButton")->click();
            QVERIFY(!ed.isExporting() && !QFileInfo::exists(out));
        }
        appSettings().setValue("credentials/certificate", certs);
        appSettings().setValue("credentials/key", key);
        {
            ExportDialog ed(state(), win_.get());
            QCOMPARE(ed.findChild<QPushButton*>("exportCredentialIdentity")->text(), QStringLiteral("chain.pem"));
            ed.findChild<QComboBox*>("exportPreset")->setCurrentIndex(ed.findChild<QComboBox*>("exportPreset")->findText("Audio - WAV 24-bit"));
            ed.findChild<QLineEdit*>("exportPath")->setText(out);
            ed.findChild<QCheckBox*>("exportCredentials")->setChecked(true);
            ed.findChild<QPushButton*>("exportButton")->click();
            QTRY_COMPARE_WITH_TIMEOUT(ed.result(), int(QDialog::Accepted), 60000);
        }
        appSettings().setValue("export/credentials", false);
        CredentialSummary c;
        QVERIFY(readContentCredentials(out.toStdString(), c) && c.present && c.state == "Valid");

        // Imported, the bin shows who signed it (read in the background), in its column and tooltip; not an undo step.
        const int undoBefore = int(state()->history().undoCount());
        const auto signedIds = state()->importFiles({out});
        QCOMPARE(signedIds.size(), size_t(1));
        auto label = [&] {
            const MediaItem* m = state()->project().findMedia(signedIds[0]);
            const auto it = m ? m->metadata.find("c2pa") : decltype(m->metadata.end()){};
            return m && it != m->metadata.end() ? QString::fromStdString(it->second) : QString();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!label().isEmpty(), 20000);
        QVERIFY2(label().startsWith("Signed by Montage tests") && label().contains("(valid)"), qPrintable(label()));
        QCOMPARE(int(state()->history().undoCount()), undoBefore + 1);  // the import alone
        state()->undo();
        QVERIFY(!state()->project().findMedia(signedIds[0]));
        state()->redo();
        QVERIFY2(label().startsWith("Signed by"), "redo brings back what was read");
        // A file without credentials gets none.
        const MediaItem* plain = state()->project().findMedia(ids[0]);
        QVERIFY(plain && !plain->metadata.count("c2pa"));
        appSettings().remove("credentials/certificate");
        appSettings().remove("credentials/key");
    }

    void convertVoice() {
        // JFK's line (two to five seconds of the file) on A1, and the whole of it again on A2 as the voice to convert to.
        state()->newProject();
        QVERIFY(state()->save(dir_.path() + "/convert.montage"));
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        const double fps = state()->sequence()->fpsValue();
        QVERIFY(state()->apply("Place", [&](Project& p, Sequence& s) {
            edit::addTrack(p, s, TrackKind::Audio);
            const edit::Result r = edit::placeMedia(p, s, media, 0, FrameTime(2 * fps), FrameTime(5 * fps), V1, A1, false);
            return r.ok ? edit::placeMedia(p, s, media, 600, 0, -1, V1, {TrackKind::Audio, 1}, false) : r;
        }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        const Id voiceClip = state()->sequence()->audioTracks.at(1).clips.at(0).id;
        // The clip a selection converts.
        QCOMPARE(voiceConversionClip(state()->project(), *state()->sequence(), {clip}), clip);
        QCOMPARE(voiceConversionClip(state()->project(), *state()->sequence(), {}), Id(0));
        QVERIFY(win_->findChild<QAction*>("convertVoice"));

        // The dialog offers every other clip with sound, then a recording, and needs the speaker's consent.
        VoiceConvertDialog dlg(state(), clip, voiceClip, win_.get());
        auto* voice = dlg.findChild<QComboBox*>("convertVoice");
        auto* consent = dlg.findChild<QCheckBox*>("convertConsent");
        auto* go = dlg.findChild<QPushButton*>("convertGo");
        QVERIFY(voice && consent && go);
        QCOMPARE(voice->count(), 2);
        QCOMPARE(voice->currentData().toLongLong(), qint64(voiceClip));
        QCOMPARE(voice->itemData(1).toLongLong(), qint64(0));
        QVERIFY(!go->isEnabled());
        QVERIFY(!dlg.convert());  // no consent
        consent->setChecked(true);
        QCOMPARE(go->isEnabled(), voiceCloneAvailable());
        dlg.setRecording(QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav"));
        QCOMPARE(voice->currentData().toLongLong(), qint64(0));
        QVERIFY(dlg.findChild<QLabel*>("convertStatus")->text().contains("jfk.wav") || !voiceCloneAvailable());
        voice->setCurrentIndex(0);

        if (!voiceCloneAvailable() || !voiceCloneModel().installed()) QSKIP("Set MONTAGE_VOICE_CLONE_MODEL to the Chatterbox Multilingual pack");
        // Converted: a take of the clip, picked, with a second either side, its file beside the project saying what it is.
        QVERIFY(dlg.convert());
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->takes.size(), size_t(2));
        QCOMPARE(c->take, 1);
        QVERIFY(std::abs(c->sourceIn - fps) < 1e-6);
        const MediaItem* m = state()->project().findMedia(c->mediaId);
        QVERIFY(m && m->bin == "Voiceover" && QString::fromStdString(m->name).contains("Converted Voice"));
        QVERIFY(QFileInfo(QString::fromStdString(m->path)).absolutePath() == QDir(dir_.path()).absoluteFilePath("Voiceover"));
        QVERIFY(std::abs(m->duration - 5.0) < 0.05);
        QVERIFY(QString::fromStdString(m->metadata.at("comment")).contains("converted to a voice cloned from jfk"));
        QVERIFY(std::find(m->keywords.begin(), m->keywords.end(), "AI voice") != m->keywords.end());
        // One undo step gives back the original.
        state()->undo();
        c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c->takes.empty());
        QCOMPARE(c->mediaId, media);
    }

    void clonedVoiceover() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        const Id clip = trackAt(*state()->sequence(), A1)->clips.at(0).id;
        // The voice in a clip: up to ten seconds of what it plays; the voice heard at a frame; none where nothing plays.
        ClonedVoice v;
        QString err;
        QVERIFY2(clipVoice(state()->project(), *state()->sequence(), clip, v, &err), qPrintable(err));
        QVERIFY2(v.sample.size() > size_t(kCloneSampleRate) * 5 && v.sample.size() <= size_t(kCloneSampleRate) * 10, qPrintable(QString::number(v.sample.size())));
        QVERIFY(QString::fromStdString(v.from).contains("jfk.wav"));
        ClonedVoice at;
        QVERIFY(voiceAt(state()->project(), *state()->sequence(), 50, at, &err));
        QCOMPARE(at.sample.size(), v.sample.size());
        QVERIFY(!voiceAt(state()->project(), *state()->sequence(), 100000, at, &err) && !err.isEmpty());
        QVERIFY(QString::fromStdString(clonedVoiceNote(v)).contains("cloned from jfk"));
        // The dialog: the clone choices ask for a selected clip with sound and the speaker's consent before generating.
        state()->setSelection({});
        SpeechDialog dlg(state(), win_.get());
        auto* voice = dlg.findChild<QComboBox*>("speechVoice");
        auto* consent = dlg.findChild<QCheckBox*>("speechConsent");
        auto* go = dlg.findChild<QPushButton*>("speechGenerate");
        QVERIFY(voice && consent && go && dlg.findChild<QComboBox*>("speechLanguage"));
        QVERIFY(consent->isHidden());
        voice->setCurrentIndex(voice->findData("clone:clip"));
        emit voice->activated(voice->currentIndex());
        QVERIFY(!consent->isHidden() && !go->isEnabled());
        state()->setSelection({clip}, false);
        consent->setChecked(true);
        QCOMPARE(go->isEnabled(), voiceCloneAvailable());
        QVERIFY(dlg.findChild<QLabel*>("speechSample")->text().contains("jfk"));
        consent->setChecked(false);
        QVERIFY(!go->isEnabled());
        dlg.findChild<QPlainTextEdit*>("speechText")->setPlainText("Testing a cloned voice.");
        QVERIFY(dlg.generate().empty());  // no consent
        if (!voiceCloneAvailable() || !voiceCloneModel().installed()) QSKIP("Set MONTAGE_VOICE_CLONE_MODEL to the Chatterbox Multilingual pack");
        consent->setChecked(true);
        state()->setPlayhead(300);
        const auto clips = dlg.generate();
        QCOMPARE(int(clips.size()), 1);
        const Clip* c = edit::clipById(*state()->sequence(), clips[0]);
        QVERIFY(c && c->start == 300);
        const MediaItem* m = state()->project().findMedia(c->mediaId);
        QVERIFY(m && m->bin == "Voiceover" && QString::fromStdString(m->name).contains("Cloned Speech"));
        QVERIFY(QString::fromStdString(m->metadata.at("comment")).contains("cloned from jfk"));
        QVERIFY(std::find(m->keywords.begin(), m->keywords.end(), "AI voice") != m->keywords.end());
        QFile wav(QString::fromStdString(m->path));
        QVERIFY(wav.open(QIODevice::ReadOnly));
        QVERIFY(wav.readAll().contains("Generated speech"));  // the note in the file itself
        const Id generated = m->id;
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), clips[0]));
        // The bin's copy stays labelled: the note came with its import.
        const MediaItem* left = state()->project().findMedia(generated);
        QVERIFY(left && left->metadata.count("comment"));
        QVERIFY(std::find(left->keywords.begin(), left->keywords.end(), "AI voice") != left->keywords.end());
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
        // Dialogue Match to the same reference, with room tone: the EQ (now Dialogue Match's), the volume brought to
        // the reference's as heard, and the reference's room tone under the clip, for its length.
        QVERIFY(win_->findChild<QAction*>("matchDialogue"));
        state()->edit("Quieter", [&](Project&, Sequence& sq) {
            edit::clipById(sq, good)->audio.params["gain_db"] = Param(-4.0);
            return true;
        });
        QCOMPARE(win_->matchVoice(), 1);
        state()->setSelection({other}, false);
        const size_t tracks = state()->sequence()->audioTracks.size();
        QCOMPARE(win_->matchDialogue(DialogueMatchOptions{}, true), 1);
        const Sequence& after = *state()->sequence();
        const Clip* matched = edit::clipById(after, other);
        QVERIFY(matched && eqOf() && eqOf()->s("match") == "dialogue");
        int eqs = 0;
        for (const Effect& e : matched->effects) eqs += e.type == "parametric_eq";
        QCOMPARE(eqs, 1);
        std::vector<float> heardRef, heardOther;
        QVERIFY(clipSpeechAudio(state()->project(), after, *edit::clipById(after, good), heardRef) &&
                clipSpeechAudio(state()->project(), after, *matched, heardOther));
        const double gain = matched->audio.p("gain_db", 0), planned = planDialogueMatch(heardRef, heardOther, 48000).gainDb - 4;
        QVERIFY2(std::fabs(gain - planned) < 0.05, qPrintable(QString("%1 %2").arg(gain).arg(planned)));
        // (on the next track down, free there; a track is added only when none below is)
        QCOMPARE(after.audioTracks.size(), std::max<size_t>(tracks, 2));
        QCOMPARE(after.audioTracks[1].clips.size(), size_t(1));
        QCOMPARE(after.audioTracks[1].clips[0].start, matched->start);
        QCOMPARE(after.audioTracks[1].clips[0].end(), matched->end());
        // One undo takes the match and its room tone off together.
        state()->undo();
        QVERIFY(state()->sequence()->audioTracks[1].clips.empty());
        QVERIFY(eqOf() && eqOf()->s("match") == "voice");
        state()->redo();
        QCOMPARE(state()->sequence()->audioTracks[1].clips.size(), size_t(1));
        // The reference itself is not matched to itself.
        state()->setSelection({good}, false);
        QCOMPARE(win_->matchDialogue(DialogueMatchOptions{}), 0);
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

    void captionsPanelFileFormats() {
        // Each format the Captions panel writes, read back as a new track with the same captions.
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        state()->newProject();
        Id track = 0;
        QVERIFY(state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            t.captions = {{30, 90, "Bonjour \xC3\xA0 tous", {}}, {120, 180, "Second\nline", {}}};
            s.captionTracks.push_back(t);
            return true;
        }));
        panel->setCurrentTrack(track);
        const auto source = state()->sequence()->captionTracks.front().captions;
        for (const char* ext : {"srt", "vtt", "scc", "ttml", "stl", "ass"}) {
            const QString file = dir_.path() + "/panel-captions." + ext;
            QString error;
            QVERIFY2(panel->exportFile(file, &error), qPrintable(error));
            const size_t before = state()->sequence()->captionTracks.size();
            QVERIFY2(panel->importFile(file, &error), qPrintable(QString("%1: %2").arg(ext, error)));
            const auto& tracks = state()->sequence()->captionTracks;
            QCOMPARE(tracks.size(), before + 1);
            const auto& got = tracks.back().captions;
            QCOMPARE(got.size(), source.size());
            for (size_t i = 0; i < got.size(); ++i) {
                QVERIFY2(got[i].text == source[i].text, qPrintable(QString("%1: %2").arg(ext, QString::fromStdString(got[i].text))));
                // SCC runs at 29.97, so its times may land a frame off at other rates.
                QVERIFY2(std::abs(got[i].start - source[i].start) <= (std::string(ext) == "scc" ? 1 : 0), ext);
                QVERIFY2(std::abs(got[i].end - source[i].end) <= (std::string(ext) == "scc" ? 1 : 0), ext);
            }
            panel->setCurrentTrack(track);
        }
        state()->newProject();
    }

    void captionsPanelChecksAndTools() {
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        state()->newProject();
        Id track = 0;
        QVERIFY(state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            const FrameTime sec = FrameTime(std::llround(s.fpsValue()));
            t.captions = {{0, 2 * sec, "Fine", {}},
                          {2 * sec + 1, 2 * sec + 6, "Far too many words for this little time on the screen", {}},
                          {5 * sec, 6 * sec, "Teh end", {}}};
            s.captionTracks.push_back(t);
            return true;
        }));
        panel->setCurrentTrack(track);
        const CaptionLimits saved = panel->limits();
        panel->setLimits(CaptionLimits{});
        auto caps = [&] { return state()->sequence()->captionTracks.front().captions; };
        // The second caption is flagged, with why in its tooltip, and the first runs into it.
        auto* table = panel->findChild<QTableWidget*>();
        QVERIFY(table && table->columnCount() == 5);
        const std::vector<unsigned> issues = panel->issues();
        QVERIFY(issues[0] == kCaptionGapTooSmall && (issues[1] & kCaptionTooFast) && (issues[1] & kCaptionLineTooLong) && issues[2] == 0);
        QVERIFY(table->item(1, 3)->text() == QString(QChar(0x26A0)));
        QVERIFY2(table->item(1, 3)->toolTip().contains("characters a second"), qPrintable(table->item(1, 3)->toolTip()));
        // The third keeps to the limits; only its spelling is flagged.
        QCOMPARE(table->item(2, 3)->toolTip(), QString("Spelling: Teh"));
        auto* summary = panel->findChild<QLabel*>("captionCheckSummary");
        QVERIFY2(summary && summary->text() == "2 of 3 captions break the reading limits; 1 with spelling mistakes", qPrintable(summary->text()));
        // Fix Timing from the menu: one undo step.
        QAction* fix = panel->findChild<QAction*>("fixCaptionTiming");
        QVERIFY(fix);
        fix->trigger();
        QCOMPARE(caps()[0].end, caps()[1].start - 2);
        QVERIFY(caps()[1].end > caps()[1].start + 5);
        QVERIFY(!(panel->issues()[1] & kCaptionTooFast));
        state()->undo();
        QCOMPARE(caps()[0].end, caps()[1].start - 1);
        // Shift only the selected caption; then all of them.
        table->clearSelection();
        table->selectRow(2);
        QCOMPARE(panel->selectedCaptions(), std::vector<size_t>{2});
        const FrameTime third = caps()[2].start;
        QVERIFY(panel->shiftCaptions(12));
        QCOMPARE(caps()[2].start, third + 12);
        QCOMPARE(caps()[0].start, FrameTime(0));
        table->clearSelection();
        QVERIFY(panel->shiftCaptions(10));
        QCOMPARE(caps()[0].start, FrameTime(10));
        // Find and replace a typo across the track.
        QCOMPARE(panel->findReplace("teh", "The", false, true), 1);
        QCOMPARE(caps()[2].text, std::string("The end"));
        // Sync: the first caption to 0 and the last to twice its time.
        const FrameTime lastStart = caps()[2].start;
        QVERIFY(panel->syncToTwoPoints(0, 2 * (lastStart - 10)));
        QCOMPARE(caps()[0].start, FrameTime(0));
        QCOMPARE(caps()[2].start, 2 * (lastStart - 10));
        state()->undo();
        QCOMPARE(caps()[0].start, FrameTime(10));
        panel->setLimits(saved);
        state()->newProject();
    }

    void captionLooksInPanel() {
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        state()->newProject();
        Id track = 0;
        QVERIFY(state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            t.captions = {{0, 30, "One", {}}};
            s.captionTracks.push_back(t);
            return true;
        }));
        panel->setCurrentTrack(track);
        auto style = [&] { return state()->sequence()->captionTracks.front().style; };
        // A built-in look in one undo step; the same again is no edit.
        QVERIFY(panel->applyLook("creator_pop"));
        QVERIFY(style() == findCaptionLook("creator_pop")->style);
        QVERIFY(!panel->applyLook("creator_pop"));
        QVERIFY(!panel->applyLook("no_such_look"));
        state()->undo();
        QVERIFY(style() == CaptionStyle{});
        // A look of one's own: saved from the track, offered by name, applied elsewhere.
        QVERIFY(state()->edit("Tweak", [](Project&, Sequence& s) {
            s.captionTracks.front().style.shadow = 0.1;
            s.captionTracks.front().style.allCaps = true;
            return true;
        }));
        QVERIFY(panel->saveLook("House Style"));
        QVERIFY(!panel->saveLook("classic"));  // a built-in's name
        QVERIFY(!panel->saveLook("a/b"));
        QVERIFY(panel->savedLooks().contains("House Style"));
        const CaptionStyle house = style();
        QVERIFY(panel->applyLook("neon"));
        QVERIFY(panel->applyLook("House Style"));
        QVERIFY(style() == house);
        state()->newProject();
    }

    void captionsPanelPlacement() {
        auto* panel = win_->findChild<CaptionsPanel*>();
        QVERIFY(panel);
        state()->newProject();
        Id track = 0;
        QVERIFY(state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = track = p.newId();
            t.captions = {{0, 30, "One", {}}, {40, 70, "Two", {}}, {80, 110, "Three", {}}};
            s.captionTracks.push_back(t);
            // A lower third over the third caption.
            Clip lower = makeGeneratorClip(p, "title_lower_third", 40);
            lower.start = 75;
            edit::overwrite(p, s, {TrackKind::Video, 0}, lower);
            return true;
        }));
        panel->setCurrentTrack(track);
        auto caps = [&] { return state()->sequence()->captionTracks.front().captions; };
        auto* table = panel->findChild<QTableWidget*>();
        QVERIFY(table && panel->findChild<QToolButton*>("placeCaptions"));
        // With nothing selected, the caption under the playhead goes to the top.
        table->clearSelection();
        state()->setPlayhead(50);
        panel->findChild<QAction*>("placeTop")->trigger();
        QCOMPARE(captionKeypad(caps()[1]), 8);
        QCOMPARE(captionKeypad(caps()[0]), 2);
        QCOMPARE(table->item(1, 4)->text(), QString(QChar(0x2191)));
        QCOMPARE(table->item(1, 4)->toolTip(), QString("top"));
        QVERIFY(table->item(0, 4)->text().isEmpty());
        // Selected captions lined up left, keeping where they are up or down; one undo step.
        table->selectRow(0);
        table->selectionModel()->select(table->model()->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(panel->selectedCaptions(), (std::vector<size_t>{0, 1}));
        panel->findChild<QAction*>("placeLeft")->trigger();
        QCOMPARE(captionKeypad(caps()[0]), 1);
        QCOMPARE(captionKeypad(caps()[1]), 7);
        state()->undo();
        QCOMPARE(captionKeypad(caps()[0]), 2);
        QCOMPARE(captionKeypad(caps()[1]), 8);
        // Splitting keeps the place for both halves.
        table->clearSelection();
        state()->setPlayhead(55);
        QVERIFY(panel->findChild<QToolButton*>() != nullptr);
        for (QToolButton* b : panel->findChildren<QToolButton*>())
            if (b->text() == "Split") b->click();
        QCOMPARE(caps().size(), size_t(4));
        QCOMPARE(captionKeypad(caps()[1]), 8);
        QCOMPARE(captionKeypad(caps()[2]), 8);
        // Move Above Titles lifts the caption under the lower third.
        panel->findChild<QAction*>("raiseCaptions")->trigger();
        QCOMPARE(captionKeypad(caps()[3]), 8);
        QCOMPARE(captionKeypad(caps()[0]), 2);
        QCOMPARE(panel->raiseOverTitles(), 0);
        // The Program monitor draws it at the top: ink in the top half of the frame at frame 90.
        Image img(320, 180);
        img.fill(0, 0, 0, 1);
        drawCaption(img, state()->sequence()->captionTracks.front(), 90);
        int top = 0, bottom = 0;
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
                if (img.at(x, y)[0] > 0.5f) (y < 90 ? top : bottom)++;
        QVERIFY2(top > 20 && bottom == 0, qPrintable(QString("%1 %2").arg(top).arg(bottom)));
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

    void regenerateWordsAndDubInTheSpeakersVoice() {
        state()->newProject();
        auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        auto t = std::make_shared<Transcript>();
        t->language = "en";
        TranscriptSegment seg;
        seg.words = {{0.5, 0.9, "And", 1}, {1.0, 1.3, "so", 1}, {1.8, 2.2, "my", 1}, {2.3, 2.8, "fellow", 1}, {2.9, 3.6, "Americans.", 1}};
        t->segments.push_back(seg);
        QVERIFY(state()->edit("Transcript", [media, t](Project& p, Sequence&) {
            p.findMedia(media)->transcript = t;
            return true;
        }));
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, -1, V1, A1, false); }));
        auto* panel = win_->findChild<TranscriptPanel*>();
        QVERIFY(panel && panel->findChild<QAction*>("regenerateWords"));
        panel->setMode(TranscriptPanel::Mode::Sequence);
        QCOMPARE(panel->words().size(), size_t(5));
        auto* captions = win_->findChild<CaptionsPanel*>();
        QVERIFY(captions);
        if (!voiceCloneAvailable() || !voiceCloneModel().installed()) QSKIP("Set MONTAGE_VOICE_CLONE_MODEL to the Chatterbox Multilingual pack");
        // "my fellow" (1.8 to 2.8 s) said again as "our dear", in his voice, over the words on A1.
        const double fps = state()->sequence()->fpsValue();
        panel->selectWords(2, 3);
        QString err;
        const Id made = panel->regenerateSelection("our dear", "en", &err);
        QVERIFY2(made, qPrintable(err));
        const auto& a1 = trackAt(*state()->sequence(), A1)->clips;
        QCOMPARE(int(a1.size()), 3);
        QCOMPARE(a1[1].id, made);
        QCOMPARE(a1[1].start, FrameTime(std::floor(1.8 * fps + 1e-6)));
        QVERIFY(a1[1].end() <= FrameTime(std::ceil(2.8 * fps)));
        QCOMPARE(a1[2].start, FrameTime(std::ceil(2.8 * fps - 1e-6)));
        const MediaItem* m = state()->project().findMedia(a1[1].mediaId);
        QVERIFY(m && m->transcript && std::find(m->keywords.begin(), m->keywords.end(), "AI voice") != m->keywords.end());
        QApplication::processEvents();
        QStringList heard;
        for (const TranscriptWord& w : panel->words()) heard << QString::fromStdString(w.text);
        QVERIFY2(heard.join(' ').contains("our dear"), qPrintable(heard.join(' ')));
        // One undo puts the words back.
        state()->undo();
        QCOMPARE(int(trackAt(*state()->sequence(), A1)->clips.size()), 1);
        // Music on the track above and a second copy of the speech below (a camera's other channel): the voice and the
        // track are the speech's, the music is left alone, and both copies of the words are replaced.
        {
            std::vector<float> tone(size_t(24000) * 12);
            for (size_t i = 0; i < tone.size(); ++i) tone[i] = float(0.2 * std::sin(2 * M_PI * 220.0 * double(i) / 24000));
            const QString music = dir_.path() + "/music.wav";
            QVERIFY(writeSpeechWav(music.toStdString(), tone));
            const auto imported = state()->importFiles({music});
            QCOMPARE(imported.size(), size_t(1));
            const Id song = imported[0];
            const std::vector<Track> before = state()->sequence()->audioTracks;
            QVERIFY(state()->apply("Stack", [&](Project& p, Sequence& sq) {
                sq.audioTracks[0].clips.clear();
                while (sq.audioTracks.size() < 3) edit::addTrack(p, sq, TrackKind::Audio);
                if (!edit::placeMedia(p, sq, song, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return edit::Result::fail("music");
                if (!edit::placeMedia(p, sq, media, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 1}, false).ok) return edit::Result::fail("A2");
                return edit::placeMedia(p, sq, media, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 2}, false);
            }));
            QVERIFY(state()->sequence()->audioTracks.size() >= 3);
            QApplication::processEvents();
            QCOMPARE(panel->words().size(), size_t(5));  // the two copies' words count once
            panel->selectWords(2, 3);
            const Id again = panel->regenerateSelection("our dear", "en", &err);
            QVERIFY2(again, qPrintable(err));
            const auto at = edit::locate(*state()->sequence(), again);
            QVERIFY(at && at->track.index == 1);  // the speech's track, not the music's
            QCOMPARE(int(state()->sequence()->audioTracks[0].clips.size()), 1);  // the music untouched
            QCOMPARE(int(state()->sequence()->audioTracks[2].clips.size()), 2);  // the other copy cleared there too
            const Clip* newClip = edit::clipById(*state()->sequence(), again);
            QVERIFY(newClip->end() <= FrameTime(std::ceil(2.8 * fps)));
            const MediaItem* said = state()->project().findMedia(newClip->mediaId);
            QVERIFY(QString::fromStdString(said->metadata.at("comment")).contains("jfk"));  // his voice, not the tone's
            state()->undo();
            QCOMPARE(int(state()->sequence()->audioTracks[2].clips.size()), 1);
            QVERIFY(state()->edit("Unstack", [&](Project&, Sequence& sq2) {
                sq2.audioTracks = before;
                return true;
            }));
        }
        // A caption track dubbed in the speaker's own voice (heard under its first caption).
        Id track = 0;
        QVERIFY(state()->edit("Captions", [&](Project& p, Sequence& s) {
            CaptionTrack c;
            c.id = track = p.newId();
            c.language = "en";
            c.captions = {{0, 45, "Hello and welcome.", {}}, {60, 120, "This is the dub.", {}}};
            s.captionTracks.push_back(c);
            return true;
        }));
        captions->setCurrentTrack(track);
        ClonedVoice voice;
        QVERIFY(voiceAt(state()->project(), *state()->sequence(), 0, voice, &err));
        voice.language = "en";
        const int tracks = int(state()->sequence()->audioTracks.size());
        const auto r = captions->dub({}, -12, &err, &voice);
        QVERIFY2(r.clips.size() == 2, qPrintable(err));
        QCOMPARE(r.audioTrack, tracks);
        QCOMPARE(state()->sequence()->audioTracks[size_t(tracks)].name, std::string("Dub (English)"));
        QCOMPARE(r.captions, track);  // already in English
        const MediaItem* dubbed = state()->project().findMedia(edit::clipById(*state()->sequence(), r.clips[0])->mediaId);
        QVERIFY(dubbed && QString::fromStdString(dubbed->metadata.at("comment")).contains("cloned from"));
        QCOMPARE(r.ducked, 1);
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
        // A preset's old name still finds it, and the new name is the one remembered.
        QVERIFY(keymap::presets().contains(QStringLiteral("Montage")) && !keymap::presets().contains(QStringLiteral("Montage (Premiere Pro)")));
        QVERIFY(!keymap::presetKeys("Final Cut Pro").isEmpty());
        QVERIFY(keymap::presetKeys("Final Cut Pro") == keymap::presetKeys("Final Cut Pro-style"));
        QVERIFY(keymap::applyPreset(w, "Avid Media Composer"));
        QCOMPARE(appSettings().value("keymapPreset").toString(), QString("Avid Media Composer-style"));
        QVERIFY(keymap::applyPreset(w, "Montage (Premiere Pro)"));
        QCOMPARE(appSettings().value("keymapPreset").toString(), QString("Montage"));
        QVERIFY(!keymap::applyPreset(w, "Nothing"));
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
        QSettings settings = appSettings();
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
        // Netflix: the dialogue's loudness counts, within 2 LU, while speech is 15 % of the sound or more.
        readout->setTargetIndex(4);
        QVERIFY(readout->dialogueTarget());
        QCOMPARE(readout->target(), -27.0);
        readout->setReading(-20, -20, -20, 6, -3);
        readout->setDialogue(-28.5, 0.4);
        QCOMPARE(readout->text("D"), QString("-28.5"));
        QCOMPARE(readout->text("SP"), QString("40 %"));
        QCOMPARE(readout->integratedStatus(), 0);
        readout->setDialogue(-28.5, 0.1);  // too little speech: the programme's -20 counts
        QCOMPARE(readout->integratedStatus(), 2);
        readout->setDialogue(-200, -1);  // nothing marked as speech
        QCOMPARE(readout->text("D"), QString("—"));
        QCOMPARE(readout->text("SP"), QString("—"));
        readout->setTargetIndex(0);
        QVERIFY(!readout->dialogueTarget());
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

    void adrPanelRecordsTakes() {
        state()->newProject();
        QVERIFY(state()->save(dir_.path() + "/adr.montage"));
        state()->apply("Caption", [](Project& p, Sequence& s) {
            CaptionTrack ct;
            ct.id = p.newId();
            Caption c;
            c.start = 150, c.end = 210, c.text = "MAYA: We should go.";
            ct.captions.push_back(c);
            s.captionTracks.push_back(ct);
            return edit::Result{};
        });
        win_->findChild<QAction*>("showAdr")->trigger();
        auto* panel = win_->findChild<AdrPanel*>("adrPanel");
        QVERIFY(panel && panel->isVisible());
        panel->setUseInputDevice(false);
        panel->setLoopGap(20);
        // A cue from the caption, its speaker the character.
        QCOMPARE(panel->addCuesFromCaptions(), 1);
        QCOMPARE(state()->sequence()->adrCues.size(), size_t(1));
        const AdrCue cue = state()->sequence()->adrCues[0];
        QCOMPARE(cue.name, std::string("M101"));
        auto* table = panel->findChild<QTableWidget*>("adrCues");
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString("MAYA"));
        QCOMPARE(table->item(0, 4)->text(), QString("We should go."));
        QCOMPARE(panel->currentCue(), cue.id);
        // A cell edits its cue; a start that is no time is refused and put back.
        table->item(0, 5)->setText("Traffic noise");
        QCOMPARE(state()->sequence()->adrCues[0].note, std::string("Traffic noise"));
        QSignalSpy said(state(), &EditorState::statusMessage);
        table->item(0, 2)->setText("not a time");
        QCOMPARE(state()->sequence()->adrCues[0].start, FrameTime(150));
        QTRY_COMPARE(table->item(0, 2)->text(), QString::fromStdString(formatTimecode(150, state()->sequence()->fps)));
        // The status bar says why, in the panel's own (translatable) words.
        QVERIFY(!said.isEmpty());
        QCOMPARE(said.last().at(0).toString(), QString("The start must be a timecode before the end"));

        QVERIFY(!panel->findChild<QPushButton*>("adrConvertVoice")->isEnabled());  // no take to convert yet
        // Rehearse: the cycle (beeps, streamer) goes to the Program monitor and nothing is recorded.
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        panel->rehearse();
        QVERIFY(panel->isRunning() && !panel->isRecording());
        QVERIFY(program->adrCycle().has_value());
        QCOMPARE(program->adrCycle()->lineFrom, FrameTime(150));
        QCOMPARE(program->adrCycle()->beeps.size(), size_t(3));
        QVERIFY(program->mutedAudioTracks().empty());
        // Stopping playback (Space, a shuttle, the sequence's end) ends the pass.
        const AdrCycle rehearsed = *program->adrCycle();
        program->pause();
        QVERIFY(!panel->isRunning() && !program->adrCycle());
        QCOMPARE(panel->findChild<QPushButton*>("adrRehearse")->isEnabled(), true);
        // Half way across the picture, the streamer: a white bar down the middle of the frame.
        program->setAdrCycle(rehearsed);
        program->seek(120);
        MonitorPanel* monitor = nullptr;
        for (MonitorPanel* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) monitor = m;
        QVERIFY(monitor);
        ViewerWidget* viewer = monitor->viewer();
        QTRY_VERIFY(!viewer->image().isNull());
        QTRY_VERIFY2(qGray(viewer->grab().toImage().pixel(viewer->width() / 2 - 1, viewer->height() / 2)) > 200, "no streamer");
        QVERIFY(qGray(viewer->grab().toImage().pixel(viewer->width() / 4, viewer->height() / 2)) < 60);
        program->setAdrCycle(std::nullopt);

        // Record: the guide (A1) is muted, and the take (fed as the input would feed it) lands over the line on a new
        // ADR track, in sync.
        QCOMPARE(panel->findChild<QComboBox*>("adrGuide")->currentIndex(), 1);
        QVERIFY(panel->findChild<QCheckBox*>("adrMuteGuide")->isChecked());
        panel->record();
        QVERIFY(panel->isRecording());
        QCOMPARE(program->mutedAudioTracks(), std::vector<int>{0});
        const AdrCycle cyc = *program->adrCycle();
        const qint64 samples = qint64(std::llround(double(cyc.playTo - cyc.playFrom) / state()->sequence()->fpsValue() * 48000));
        std::vector<float> block(4800, 0.2f);
        auto feedCycle = [&] {
            for (qint64 done = 0; done < samples + 4800; done += 4800) panel->recorder()->feed(block.data(), 4800);
        };
        feedCycle();
        QTRY_VERIFY(!panel->isRunning());
        QVERIFY(program->mutedAudioTracks().empty() && !program->adrCycle());
        const Id clip = state()->sequence()->adrCues[0].clip;
        QVERIFY(clip);
        QCOMPARE(state()->sequence()->adrCues[0].status, int(kAdrRecorded));
        // A take can be said again in the guide's voice (none on the guide track here).
        QTRY_VERIFY(panel->findChild<QPushButton*>("adrConvertVoice")->isEnabled());
        QCOMPARE(panel->guideClip(), Id(0));
        // Aligned to the guide only once there is one; a take with no speech in it is refused, saying why.
        auto* align = panel->findChild<QPushButton*>("adrAlign");
        QVERIFY(align && !align->isEnabled());
        const auto speech = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(speech.size(), size_t(1));
        QVERIFY(state()->apply("Guide", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, speech.front(), 150, 0, -1, {TrackKind::Video, -1}, {TrackKind::Audio, 0}, false);
        }));
        QTRY_VERIFY(panel->guideClip() != 0);
        QTRY_VERIFY(align->isEnabled());
        QVERIFY(!panel->alignToGuide());
        QVERIFY(said.last().at(0).toString().contains("no speech"));
        QVERIFY(edit::clipById(*state()->sequence(), clip)->effects.empty());
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c && c->start == 150 && c->duration == 60);
        QCOMPARE(c->sourceIn, double(150 - cyc.playFrom));
        const auto loc = edit::locate(*state()->sequence(), clip);
        QCOMPARE(state()->sequence()->audioTracks[size_t(loc->track.index)].name, std::string("ADR"));
        QVERIFY(QFileInfo::exists(dir_.path() + "/ADR/M101 take 1.wav"));
        QCOMPARE(state()->project().findMedia(c->mediaId)->bin, std::string("ADR"));
        QTRY_COMPARE(table->item(0, 7)->text(), QString("1"));

        // The picture stopping before the input had all of the pass arms a delayed stop; it belongs to that pass only and
        // never cuts the next take short.
        panel->record();
        program->pause();
        feedCycle();
        QTRY_VERIFY(!panel->isRunning());
        QTRY_COMPARE(adrTakeCount(*state()->sequence(), state()->sequence()->adrCues[0]), 2);
        panel->record();
        QTest::qWait(1700);
        QVERIFY(panel->isRecording());
        feedCycle();
        QTRY_VERIFY(!panel->isRunning());
        QTRY_COMPARE(adrTakeCount(*state()->sequence(), state()->sequence()->adrCues[0]), 3);
        // Loop: take after take until stopped, the ADR track muted too while recording; Stop keeps what was recorded.
        panel->findChild<QCheckBox*>("adrLoop")->setChecked(true);
        panel->record();
        QCOMPARE(program->mutedAudioTracks(), (std::vector<int>{0, loc->track.index}));
        feedCycle();
        QTRY_COMPARE(adrTakeCount(*state()->sequence(), state()->sequence()->adrCues[0]), 4);
        QTRY_VERIFY(panel->isRecording());  // the next take began by itself
        for (int i = 0; i < 45; ++i) panel->recorder()->feed(block.data(), 4800);  // 4.5 s: into the line
        panel->stop();
        QVERIFY(!panel->isRunning() && !panel->isRecording());
        c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->takes.size(), size_t(5));
        QCOMPARE(c->take, 4);
        QTest::qWait(100);
        QVERIFY(!panel->isRunning());
        // Stopped in the pre-roll, before the line: not a take.
        panel->findChild<QCheckBox*>("adrLoop")->setChecked(false);
        panel->record();
        panel->recorder()->feed(block.data(), 4800);
        panel->stop();
        QVERIFY(!panel->isRunning());
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->takes.size(), size_t(5));
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->take, 4);
        // Another take picked from the list.
        auto* takes = panel->findChild<QComboBox*>("adrTakes");
        QTRY_COMPARE(takes->count(), 5);
        panel->pickTake(0);
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->take, 0);

        // The cue sheet, out and back in.
        const QString sheet = dir_.path() + "/cues.csv";
        QVERIFY(panel->exportCueSheet(sheet));
        QFile f(sheet);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("M101,MAYA,"));
        f.close();
        panel->removeCue();
        QVERIFY(state()->sequence()->adrCues.empty());
        QVERIFY(panel->importCueSheet(sheet));
        QCOMPARE(state()->sequence()->adrCues.size(), size_t(1));
        QCOMPARE(state()->sequence()->adrCues[0].status, int(kAdrRecorded));
        QCOMPARE(state()->sequence()->adrCues[0].note, std::string("Traffic noise"));
        QVERIFY(!panel->importCueSheet(dir_.path() + "/missing.csv"));
        // Another project opened mid-take: the take is dropped, not imported into it.
        panel->selectCue(state()->sequence()->adrCues[0].id);
        panel->findChild<QCheckBox*>("adrLoop")->setChecked(false);
        panel->record();
        QVERIFY(panel->isRecording());
        panel->recorder()->feed(block.data(), 4800);
        const QString partial = panel->recorder()->takeFolder();
        state()->newProject();
        QVERIFY(!panel->isRecording() && !panel->isRunning() && !program->adrCycle());
        QVERIFY(state()->project().media.empty());
        QVERIFY(!QFileInfo::exists(partial + "/M101 take 7.wav"));
        state()->newProject();
    }

    void audioDescriptionDialog() {
        state()->newProject();
        // Dialogue: a second of tone at 0 s and again at 3 s.
        const QString wav = dir_.path() + "/ad-line.wav";
        {
            WavWriter w;
            QVERIFY(w.open(wav, 48000, 1));
            std::vector<float> tone(48000);
            for (size_t i = 0; i < tone.size(); ++i) tone[i] = float(0.3 * std::sin(2 * M_PI * 440 * double(i) / 48000));
            w.write(tone.data(), 48000);
            QVERIFY(w.close());
        }
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids.front();
        QVERIFY(state()->apply("Lines", [&](Project& p, Sequence& s) {
            edit::Result r = edit::placeMedia(p, s, media, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            if (!r.ok) return r;
            r = edit::placeMedia(p, s, media, 90, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            if (!r.ok) return r;
            // Music between the lines (not dialogue: the gap is still found), to be ducked under the description.
            const TrackRef music = edit::addTrack(p, s, TrackKind::Audio);
            r = edit::placeMedia(p, s, media, 36, 0, -1, {TrackKind::Video, -1}, music, false);
            if (r.ok) edit::clipById(s, r.created.front())->role = "Music";
            return r;
        }));
        win_->findChild<QAction*>("audioDescription")->trigger();
        auto* dlg = win_->findChild<AudioDescriptionDialog*>("audioDescriptionDialog");
        QVERIFY(dlg && dlg->isVisible());
        dlg->findChild<QDoubleSpinBox*>("adMinGap")->setValue(1.0);
        QString err;
        QVERIFY2(dlg->findGaps(&err) >= 1, qPrintable(err));
        auto* table = dlg->findChild<QTableWidget*>("adTable");
        QVERIFY(table->rowCount() >= 1);
        // A description typed into the gap goes on the hidden description track; its fit is shown.
        table->item(0, 3)->setText("Rain falls.");
        QTRY_VERIFY(findDescriptionTrack(*state()->sequence()) >= 0);
        const CaptionTrack& t = state()->sequence()->captionTracks[size_t(findDescriptionTrack(*state()->sequence()))];
        QCOMPARE(t.captions.size(), size_t(1));
        QCOMPARE(t.captions[0].text, std::string("Rain falls."));
        QVERIFY(!t.visible);
        QVERIFY(t.captions[0].start >= 39 && t.captions[0].end <= 81);
        QTRY_COMPARE(table->item(0, 4)->text(), QString("Fits"));
        // One too long for its gap says how many words to cut.
        table->item(0, 3)->setText("Rain streams down the tall window while thunder rolls far across the dark valley below.");
        QTRY_VERIFY(table->item(0, 4)->text().startsWith("Too long"));
        // Undo puts the first back.
        state()->undo();
        QTRY_COMPARE(table->item(0, 3)->text(), QString("Rain falls."));
        // Muting them while working; nothing to duck before they are voiced.
        dlg->setHear(false);
        QVERIFY(edit::roleMuted(*state()->sequence(), kDescriptionRole));
        QVERIFY(!dlg->findChild<QCheckBox*>("adHear")->isChecked());
        dlg->setHear(true);
        QCOMPARE(dlg->duck(), 0);
        // Voiced (when the speech model is here) onto the AD track as Description clips, and the programme ducked.
        if (ttsAvailable() && ttsModel().installed()) {
            QCOMPARE(dlg->voice(&err), 1);
            const Sequence& s = *state()->sequence();
            QVERIFY(hasDescriptionClips(s));
            int ad = -1;
            for (int i = 0; i < int(s.audioTracks.size()); ++i)
                if (s.audioTracks[size_t(i)].name == "AD") ad = i;
            QVERIFY(ad >= 0 && s.audioTracks[size_t(ad)].clips.size() == 1);
            QCOMPARE(dlg->duck(), 1);  // the music; the lines are clear of it
            QCOMPARE(dlg->voice(&err), 1);  // voiced again: replaced, not added
            QCOMPARE(state()->sequence()->audioTracks[size_t(ad)].clips.size(), size_t(1));
        } else {
            QVERIFY(state()->apply("Description clip", [&](Project& p, Sequence& s) {
                const TrackRef r = edit::addTrack(p, s, TrackKind::Audio);
                s.audioTracks[size_t(r.index)].name = "AD";
                edit::Result res = edit::placeMedia(p, s, media, 40, 0, -1, {TrackKind::Video, -1}, r, false);
                if (res.ok) edit::clipById(s, res.created.front())->role = kDescriptionRole;
                return res;
            }));
            QCOMPARE(dlg->duck(), 1);  // the music; the lines are clear of it
        }
        // Export offers the described stream once there are descriptions, for containers with several streams.
        {
            ExportDialog ed(state(), win_.get());
            auto* described = ed.findChild<QCheckBox*>("exportDescribed");
            auto* preset = ed.findChild<QComboBox*>("exportPreset");
            QVERIFY(described && preset);
            preset->setCurrentIndex(preset->findText("Audio - AAC (M4A)"));
            QVERIFY(!described->isEnabled());  // M4A here: one stream
            for (int i = 0; i < preset->count(); ++i)
                if (preset->itemText(i).startsWith("H.264")) {
                    preset->setCurrentIndex(i);
                    break;
                }
            QVERIFY(described->isEnabled());
        }
        dlg->close();
        state()->newProject();
    }

    void keyOutGreenScreen() {
        QAction* action = win_->findChild<QAction*>("keyScreen");
        QVERIFY(action);
        // A green screen still with a person-coloured block in the middle.
        QImage shot(320, 180, QImage::Format_RGB32);
        shot.fill(QColor::fromRgbF(0.12f, 0.72f, 0.2f));
        for (int y = 50; y < 130; ++y)
            for (int x = 120; x < 200; ++x) shot.setPixelColor(x, y, QColor::fromRgbF(0.8f, 0.58f, 0.47f));
        const QString png = dir_.filePath("greenscreen.png");
        QVERIFY(shot.save(png));
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 50, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id clip = state()->sequence()->videoTracks[0].clips.front().id;
        state()->setSelection({clip}, false);
        state()->setPlayhead(10);
        action->trigger();
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->effects.size(), size_t(1));
        QCOMPARE(c->effects[0].type, std::string("screen_key"));
        QVERIFY(std::fabs(c->effects[0].p("key.g", 0) - 0.72) < 0.03 && std::fabs(c->effects[0].p("key.r", 0) - 0.12) < 0.03);
        // Run again: the same Keyer, re-read, not a second one (and no change, so no new undo step).
        action->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->effects.size(), size_t(1));
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), clip)->effects.empty());
        // Nothing green to key: refused.
        QImage grey(64, 64, QImage::Format_RGB32);
        grey.fill(QColor(128, 120, 110));
        const QString gp = dir_.filePath("grey.png");
        QVERIFY(grey.save(gp));
        const auto gid = state()->importFiles({gp});
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, gid[0], 100, 0, 20, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id greyClip = state()->sequence()->videoTracks[0].clips.back().id;
        state()->setSelection({greyClip}, false);
        QCOMPARE(win_->keyOutScreen(), 0);
    }

    void interpretFootageFromTheBin() {
        // A second at 50 fps, 64 x 36 ProRes.
        const QString video = dir_.filePath("fifty.mov");
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 64, gs.height = 36, gs.fps = Rational{50, 1};
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "bars", 50));
            ExportSettings st;
            st.path = video.toStdString();
            st.videoCodec = "prores_ks";
            st.audioCodec = "none";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const FrameTime placedLength = state()->sequence()->videoTracks[0].clips.front().duration;
        auto* bin = win_->findChild<MediaBinWidget*>();
        auto* icons = bin->findChild<QAbstractItemView*>("mediaIcons");
        QVERIFY(bin && icons);
        bin->setView(MediaBinWidget::View::Icons);
        // The bin's context menu offers it.
        bin->selectMedia({ids[0]});
        bool offered = false;
        QTimer::singleShot(0, this, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            for (QAction* a : menu->actions()) offered |= a->objectName() == "interpretFootage";
            menu->close();
        });
        emit icons->customContextMenuRequested(icons->visualRect(icons->currentIndex()).center());
        QVERIFY(offered);
        win_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(win_.get()));
        // The dialog: assume 25 fps and a 2x anamorphic squeeze.
        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg || dlg->objectName() != "interpretFootageDialog") return;
            shown = true;
            auto* fileRate = dlg->findChild<QRadioButton*>("interpretFileRate");
            if (!fileRate || !fileRate->isChecked() || !fileRate->text().contains("50")) return dlg->reject();
            dlg->findChild<QRadioButton*>("interpretAssumeRate")->setChecked(true);
            dlg->findChild<QComboBox*>("interpretRate")->setCurrentText("25");
            dlg->findChild<QRadioButton*>("interpretConformPar")->setChecked(true);
            auto* preset = dlg->findChild<QComboBox*>("interpretParPreset");
            preset->setCurrentIndex(preset->findText("Anamorphic 2x"));
            dlg->findChild<QComboBox*>("interpretAlpha")->setCurrentIndex(2);  // ignore
            dlg->accept();
        });
        bin->interpretFootageDialog({ids[0]});
        QVERIFY(shown);
        const MediaItem* m = state()->project().findMedia(ids[0]);
        QCOMPARE(m->fps, (Rational{25, 1}));
        QVERIFY2(std::fabs(m->duration - 2.0) < 0.05, qPrintable(QString::number(m->duration)));
        QCOMPARE(m->width, 128);
        const Interpretation in = interpretationOf(*m);
        QVERIFY(in.conformed() && in.par == 2.0 && in.alpha == "ignore");
        QCOMPARE(state()->sequence()->videoTracks[0].clips.front().duration, placedLength);  // the clip keeps its length
        // One undo step back to the file's own reading.
        state()->undo();
        m = state()->project().findMedia(ids[0]);
        QCOMPARE(m->fps, (Rational{50, 1}));
        QCOMPARE(m->width, 64);
        QVERIFY(interpretationOf(*m).empty());
        // Opened again on the conformed footage, it starts from how it is read; Cancel changes nothing.
        QVERIFY(bin->interpretFootage({ids[0]}, in));
        QTimer::singleShot(0, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg) return;
            shown = dlg->findChild<QRadioButton*>("interpretAssumeRate")->isChecked() &&
                    std::fabs(dlg->findChild<QDoubleSpinBox*>("interpretPar")->value() - 2.0) < 1e-9;
            dlg->reject();
        });
        shown = false;
        bin->interpretFootageDialog({ids[0]});
        QVERIFY(shown);
        QCOMPARE(state()->project().findMedia(ids[0])->fps, (Rational{25, 1}));
        // A bad rate is refused with nothing changed.
        Interpretation bad;
        bad.fps = Rational{5000, 1};
        QVERIFY(!bin->interpretFootage({ids[0]}, bad));
        QCOMPARE(state()->project().findMedia(ids[0])->fps, (Rational{25, 1}));
        state()->newProject();
    }

    void ambisonicUi() {
        // A four-channel ambisonic WAV (speech placed to the left of a field), which a WAV does not mark as one.
        const QString field = dir_.filePath("field.wav");
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.fps = Rational{25, 1};
            gs.audioLayout = "ambix";
            MediaItem speech;
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", speech));
            speech.id = gen.newId();
            gen.media.push_back(speech);
            QVERIFY(edit::placeMedia(gen, gs, speech.id, 0, 0, 50, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
            gs.audioTracks[0].surround = SurroundPan{-1, 0, 0, -100, 0, false};
            ExportSettings st = findExportPreset("Audio - WAV 24-bit")->settings;
            st.path = field.toStdString();
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({field});
        QCOMPARE(ids.size(), size_t(1));
        QCOMPARE(state()->project().findMedia(ids[0])->channels, 4);
        QCOMPARE(state()->project().findMedia(ids[0])->ambisonic, 0);
        // The media bin marks it ambisonic, in one undo step.
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin->setMediaAmbisonic(ids, 1));
        QVERIFY(!bin->setMediaAmbisonic(ids, 1));
        QCOMPARE(state()->project().findMedia(ids[0])->ambisonic, 1);
        state()->undo();
        QCOMPARE(state()->project().findMedia(ids[0])->ambisonic, 0);
        state()->redo();
        // The Ambisonics effect is offered among the audio effects.
        QVERIFY(findEffectInfo("ambisonics"));
        // Sequence Settings offers an ambisonic mix.
        {
            SequenceSettingsDialog dlg(win_.get());
            NewSequenceSpec spec;
            spec.audioLayout = "ambix";
            dlg.setSpec(spec);
            auto* layouts = dlg.findChild<QComboBox*>("audioLayout");
            QVERIFY(layouts && layouts->currentData().toString() == "ambix");
            QCOMPARE(dlg.spec().audioLayout, std::string("ambix"));
        }
        // Playback › Ambisonic Monitoring: binaural or stereo, remembered.
        MonitorPanel* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp;
        QVERIFY(program);
        auto* binaural = win_->findChild<QAction*>("ambisonicBinaural");
        auto* stereo = win_->findChild<QAction*>("ambisonicStereo");
        QVERIFY(binaural && stereo);
        stereo->trigger();
        QVERIFY(!program->controller()->ambisonicBinaural() && stereo->isChecked() && !binaural->isChecked());
        QCOMPARE(appSettings().value("playback/ambisonicBinaural").toBool(), false);
        binaural->trigger();
        QVERIFY(program->controller()->ambisonicBinaural());
        appSettings().remove("playback/ambisonicBinaural");
        state()->newProject();
    }

    void higherOrderAmbisonicsUi() {
        // A third-order field (16 channels) as a WAV, which does not mark it as one.
        const QString field = dir_.filePath("field-hoa3.wav");
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.fps = Rational{25, 1};
            gs.audioLayout = "ambix3";
            MediaItem speech;
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", speech));
            speech.id = gen.newId();
            gen.media.push_back(speech);
            QVERIFY(edit::placeMedia(gen, gs, speech.id, 0, 0, 25, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
            ExportSettings st = findExportPreset("Audio - WAV 24-bit")->settings;
            st.path = field.toStdString();
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({field});
        QCOMPARE(ids.size(), size_t(1));
        QCOMPARE(state()->project().findMedia(ids[0])->channels, 16);
        // Marked ambisonic from the media bin: to the highest order its channels hold.
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin->setMediaAmbisonic(ids, kMaxAmbisonicOrder));
        QCOMPARE(state()->project().findMedia(ids[0])->ambisonic, 3);
        QVERIFY(bin->setMediaAmbisonic(ids, 1));
        QCOMPARE(state()->project().findMedia(ids[0])->ambisonic, 1);
        // Sequence Settings offers second- and third-order mixes.
        {
            SequenceSettingsDialog dlg(win_.get());
            auto* layouts = dlg.findChild<QComboBox*>("audioLayout");
            QVERIFY(layouts && layouts->findData("ambix2") >= 0 && layouts->findData("ambix3") >= 0);
            NewSequenceSpec spec;
            spec.audioLayout = "ambix3";
            dlg.setSpec(spec);
            QCOMPARE(dlg.spec().audioLayout, std::string("ambix3"));
        }
        // Playback › Ambisonic Monitoring › the head heard through: the model, SADIE II's KU100, or a SOFA file.
        auto* head = win_->findChild<QAction*>("hrtfHead");
        auto* ku100 = win_->findChild<QAction*>("hrtfKu100");
        auto* file = win_->findChild<QAction*>("hrtfFile");
        auto* menu = win_->findChild<QMenu*>("ambisonicMonitorMenu");
        QVERIFY(head && ku100 && file && menu);
        QCOMPARE(file->isEnabled(), sofaSupported());
        QVERIFY(head->isChecked() && binauralHrtf().empty());
        const QString sofa = QStringLiteral(MONTAGE_TEST_DATA_DIR "/sofa/small.sofa");
        if (!sofaSupported()) {
            QVERIFY(!win_->setBinauralHrtfChoice(sofa, true));
            QSKIP("Built without libmysofa");
        }
        QVERIFY(win_->setBinauralHrtfChoice(sofa, true));
        QCOMPARE(binauralHrtf(), sofa.toStdString());
        QCOMPARE(appSettings().value("playback/binauralHrtf").toString(), sofa);
        QMetaObject::invokeMethod(menu, "aboutToShow");
        QVERIFY(file->isChecked() && !head->isChecked() && file->text().contains("small.sofa"));
        // A file that is not one is refused, the choice kept.
        QVERIFY(!win_->setBinauralHrtfChoice(field, true));
        QCOMPARE(binauralHrtf(), sofa.toStdString());
        // The KU100 when downloaded (not asked for here).
        QCOMPARE(win_->setBinauralHrtfChoice("ku100", true), sadieKu100Pack().installed());
        head->trigger();
        QVERIFY(binauralHrtf().empty());
        QMetaObject::invokeMethod(menu, "aboutToShow");
        QVERIFY(head->isChecked() && !file->isChecked() && !ku100->isChecked());
        appSettings().remove("playback/binauralHrtf");
        state()->newProject();
    }

    void stereoscopic3dUi() {
        // A side-by-side file (left half red, right half blue), 128 x 36.
        const QString video = dir_.filePath("sbs.mov");
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 128, gs.height = 36, gs.fps = Rational{25, 1};
            Clip blue = makeGeneratorClip(gen, "color", 10), red = makeGeneratorClip(gen, "color", 10);
            blue.generator.params["color.r"] = 0.0, blue.generator.params["color.g"] = 0.0, blue.generator.params["color.b"] = 1.0;
            red.generator.params["color.r"] = 1.0, red.generator.params["color.g"] = 0.0, red.generator.params["color.b"] = 0.0;
            red.motion.params["crop_right"] = 50.0;
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, blue);
            edit::overwrite(gen, gs, {TrackKind::Video, 1}, red);
            ExportSettings st;
            st.path = video.toStdString();
            st.videoCodec = "prores_ks";
            st.audioCodec = "none";
            std::string err;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        auto* bin = win_->findChild<MediaBinWidget*>();
        // Interpret Footage: side by side, eyes swapped.
        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg || dlg->objectName() != "interpretFootageDialog") return;
            auto* stereo = dlg->findChild<QComboBox*>("interpretStereo");
            auto* swap = dlg->findChild<QCheckBox*>("interpretSwapEyes");
            if (!stereo || !swap || !stereo->isEnabled()) return dlg->reject();
            shown = true;
            stereo->setCurrentIndex(stereo->findData(QStringLiteral("sbs")));
            swap->setChecked(true);
            dlg->accept();
        });
        bin->interpretFootageDialog({ids[0]});
        QVERIFY(shown);
        const MediaItem* m = state()->project().findMedia(ids[0]);
        QCOMPARE(m->stereo, std::string("sbs"));
        QCOMPARE(m->width, 64);
        QVERIFY(interpretationOf(*m).swapEyes);
        // Opened again it shows how the file is read.
        shown = false;
        QTimer::singleShot(0, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg) return;
            shown = dlg->findChild<QComboBox*>("interpretStereo")->currentData().toString() == "sbs" &&
                    dlg->findChild<QCheckBox*>("interpretSwapEyes")->isChecked();
            dlg->reject();
        });
        bin->interpretFootageDialog({ids[0]});
        QVERIFY(shown);
        // Sequence Settings: a stereoscopic sequence; VR180 only with 360°.
        {
            SequenceSettingsDialog dlg(win_.get());
            NewSequenceSpec spec;
            spec.width = 64;
            spec.height = 36;
            spec.stereo3d = true;
            dlg.setSpec(spec);
            auto* stereo = dlg.findChild<QCheckBox*>("stereoSequence");
            auto* vr180 = dlg.findChild<QCheckBox*>("vr180Sequence");
            auto* sphere = dlg.findChild<QCheckBox*>("sphericalSequence");
            QVERIFY(stereo && vr180 && sphere);
            QVERIFY(stereo->isChecked() && dlg.spec().stereo3d);
            QVERIFY(!vr180->isEnabled());
            sphere->setChecked(true);
            QVERIFY(vr180->isEnabled());
            vr180->setChecked(true);
            QVERIFY(dlg.spec().vr180 && dlg.spec().spherical);
            sphere->setChecked(false);
            QVERIFY(!vr180->isChecked() && !dlg.spec().vr180);
        }
        QVERIFY(state()->edit("3D", [&](Project& p, Sequence& s) {
            s.width = 64, s.height = 36, s.fps = Rational{25, 1};
            s.stereo3d = true;
            return edit::placeMedia(p, s, ids[0], 0, 0, 10, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        // Playback › Stereo 3D View: the Program monitor shows the anaglyph, then both eyes side by side.
        MonitorPanel* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp;
        QVERIFY(program);
        auto* left = win_->findChild<QAction*>("stereoView_left");
        auto* anaglyph = win_->findChild<QAction*>("stereoView_anaglyph");
        auto* sbs = win_->findChild<QAction*>("stereoView_sbs_half");
        QVERIFY(left && anaglyph && sbs && win_->findChild<QAction*>("stereoView_difference"));
        anaglyph->trigger();
        QCOMPARE(program->controller()->stereoView(), StereoView::Anaglyph);
        QVERIFY(anaglyph->isChecked() && !left->isChecked());
        QCOMPARE(appSettings().value("playback/stereoView").toString(), QStringLiteral("anaglyph"));
        QCOMPARE(program->controller()->renderOptions().stereoView, StereoView::Anaglyph);
        state()->setPlayhead(5);
        program->controller()->seek(5);
        // The eyes are swapped (Interpret Footage): the left eye is blue, the right red; the anaglyph's red channel
        // carries the left eye's brightness, its blue the right eye's.
        QTRY_VERIFY(!program->viewer()->image().isNull() && program->viewer()->image().width() == 64);
        QTRY_VERIFY2(qBlue(program->viewer()->image().pixel(32, 18)) < 60 && qRed(program->viewer()->image().pixel(32, 18)) > 5,
                     qPrintable(QString::number(program->viewer()->image().pixel(32, 18), 16)));
        sbs->trigger();
        QTRY_VERIFY(program->viewer()->image().width() == 64 && qBlue(program->viewer()->image().pixel(16, 18)) > 180 &&
                    qRed(program->viewer()->image().pixel(48, 18)) > 180);
        left->trigger();
        QCOMPARE(program->controller()->stereoView(), StereoView::Left);
        // Export: the 3D choice is offered for a stereo sequence and sets the export's packing.
        {
            ExportDialog ed(state(), win_.get());
            auto* stereo = ed.findChild<QComboBox*>("exportStereo");
            QVERIFY(stereo && !stereo->isHidden());
            stereo->setCurrentIndex(stereo->findData(QStringLiteral("tb")));
            QVERIFY(stereo->currentText().contains("Top and bottom"));
        }
        QVERIFY(state()->edit("2D", [](Project&, Sequence& s) {
            s.stereo3d = false;
            return true;
        }));
        {
            ExportDialog ed(state(), win_.get());
            QVERIFY(ed.findChild<QComboBox*>("exportStereo")->isHidden());
        }
        appSettings().remove("playback/stereoView");
        appSettings().remove("export/stereo");
        state()->newProject();
    }

    // One request to the agent link over HTTP, as a client sends it; the event loop runs meanwhile (the link lives on
    // this thread). The body; `status` gets the HTTP status.
    QByteArray agentPost(quint16 port, const QByteArray& body, const QByteArray& token, int* status, const QByteArray& extra = {}) {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, port);
        QElapsedTimer t;
        t.start();
        while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < 10000) QTest::qWait(5);
        QByteArray req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n" + extra;
        if (!token.isEmpty()) req += "Authorization: Bearer " + token + "\r\n";
        req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        s.write(req);
        QByteArray got;
        while (t.elapsed() < 60000 && s.state() != QAbstractSocket::UnconnectedState) {
            QTest::qWait(5);
            got += s.readAll();
        }
        got += s.readAll();
        const int end = int(got.indexOf("\r\n\r\n"));
        if (status) *status = got.split(' ').value(1).toInt();
        return end < 0 ? QByteArray() : got.mid(end + 4);
    }

    // A raw request to the agent link; the status it answers with (0: none).
    int rawAgentStatus(quint16 port, const QByteArray& request) {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, port);
        QElapsedTimer t;
        t.start();
        while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < 10000) QTest::qWait(5);
        s.write(request);
        QByteArray got;
        while (t.elapsed() < 20000 && s.state() != QAbstractSocket::UnconnectedState && !got.contains("\r\n")) {
            QTest::qWait(5);
            got += s.readAll();
        }
        return got.split(' ').value(1).toInt();
    }

    void agentLinkKeepsWhatFilesSay() {
        // A tool's result on the open project, applied while a growing file's length was read again meanwhile.
        Project before = makeDefaultProject();
        MediaItem rec;
        rec.id = before.newId();
        rec.path = "/rec/take1.wav";
        rec.name = "take1.wav";
        rec.hasAudio = true;
        rec.duration = 2.0;
        before.media.push_back(rec);
        MediaItem sub = rec;  // a subclip of it: its range is its own
        sub.id = before.newId();
        sub.subclipOf = rec.id;
        sub.duration = 1.0;
        before.media.push_back(sub);
        Project now = before;
        now.media[0].duration = 5.0;  // read again in Montage while the tool ran
        now.media[1].sampleRate = 48000;
        now.sequences[0].playhead = 40;
        Project loaded = before;
        loaded.sequences[0].markers.push_back(Marker{});  // the tool's edit
        std::vector<std::string> reread;
        std::optional<Project> merged = mergeToolResult(before, now, loaded, &reread);
        QVERIFY(merged);
        QCOMPARE(merged->media[0].duration, 5.0);
        QCOMPARE(merged->media[1].duration, 1.0);
        QCOMPARE(merged->media[1].sampleRate, 48000);
        QCOMPARE(merged->sequences[0].markers.size(), size_t(1));
        QCOMPARE(merged->sequences[0].playhead, FrameTime(40));
        QVERIFY(reread.empty());
        // The tool read the file again itself (montage_refresh_media): its reading stands, and the file is named.
        loaded.media[0].duration = 6.0;
        merged = mergeToolResult(before, now, loaded, &reread);
        QVERIFY(merged);
        QCOMPARE(merged->media[0].duration, 6.0);
        QCOMPARE(reread, std::vector<std::string>{"/rec/take1.wav"});
        // An edit in Montage meanwhile (a subclip's range included) still refuses the result.
        Project edited = now;
        edited.media[1].duration = 0.5;
        QVERIFY(!mergeToolResult(before, edited, loaded));
        edited = now;
        edited.media[0].name = "Take 1";
        QVERIFY(!mergeToolResult(before, edited, loaded));
    }

    void agentLinkEditsTheOpenProject() {
        qputenv("MONTAGE_MCP_LIVE_FILE", dir_.filePath("mcp-live.json").toUtf8());
        QVERIFY(win_->findChild<QAction*>("agentLink") && win_->findChild<QAction*>("agentLinkDialog"));
        state()->newProject();
        QVERIFY(state()->save(dir_.filePath("linked.montage")));
        LiveLink* link = win_->liveLink();
        QVERIFY(win_->setAgentLink(true));
        QVERIFY(link->running() && win_->findChild<QAction*>("agentLink")->isChecked());
        LiveConnection c;
        QVERIFY(readLiveConnection(c));
        QVERIFY(c.port == link->port() && c.token == link->token() && c.project == dir_.filePath("linked.montage"));
        const QByteArray token = link->token().toUtf8();
        auto rpc = [&](const QString& method, const QJsonObject& params = {}) {
            QJsonObject m{{"jsonrpc", "2.0"}, {"id", 7}, {"method", method}};
            if (!params.isEmpty()) m["params"] = params;
            int status = 0;
            const QByteArray body = agentPost(link->port(), QJsonDocument(m).toJson(QJsonDocument::Compact), token, &status);
            return status == 200 ? QJsonDocument::fromJson(body).object().value("result").toObject() : QJsonObject{{"status", status}};
        };
        auto call = [&](const QString& tool, const QJsonObject& args = {}) { return rpc("tools/call", {{"name", tool}, {"arguments", args}}); };
        // Without the key, or from a web page elsewhere: refused.
        int status = 0;
        agentPost(link->port(), R"({"jsonrpc":"2.0","id":1,"method":"ping"})", {}, &status);
        QCOMPARE(status, 401);
        agentPost(link->port(), R"({"jsonrpc":"2.0","id":1,"method":"ping"})", token, &status, "Origin: https://example.com\r\n");
        QCOMPARE(status, 403);
        // Judged on the headers, before any body is kept: a huge body without the key, or too big with it.
        QCOMPARE(rawAgentStatus(link->port(), "POST /mcp HTTP/1.1\r\nContent-Length: 4000000000\r\n\r\n"), 401);
        QCOMPARE(rawAgentStatus(link->port(), "POST /mcp HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\nContent-Length: 4000000000\r\n\r\n"), 413);
        // The tools: "project" may be left out, and the link's own context tool is there.
        const QJsonArray tools = rpc("tools/list").value("tools").toArray();
        QJsonObject title, context;
        for (const QJsonValue& v : tools) {
            if (v.toObject().value("name") == "montage_add_title") title = v.toObject();
            if (v.toObject().value("name") == "montage_live_context") context = v.toObject();
        }
        QVERIFY(!title.isEmpty() && !context.isEmpty());
        QVERIFY(!title.value("inputSchema").toObject().value("required").toArray().contains("project"));
        // Except where "project" is the new file a tool writes: never the open project.
        for (const QJsonValue& v : tools)
            if (v.toObject().value("name") == "montage_create_project" || v.toObject().value("name") == "montage_import_timeline")
                QVERIFY(v.toObject().value("inputSchema").toObject().value("required").toArray().contains("project"));
        // A title added by the agent: one undo step, named for it.
        const int steps = int(state()->history().undoCount());
        QJsonObject r = call("montage_add_title", {{"text", "From the agent"}, {"at", 1}, {"duration", 2}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(!QJsonDocument(r).toJson().contains(".agent.montage"));  // the answer names the open project
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        QVERIFY2(state()->undoText().startsWith("Assistant: "), qPrintable(state()->undoText()));
        Id titleClip = 0;
        for (const Track& t : state()->sequence()->videoTracks)
            for (const Clip& clip : t.clips)
                if (clip.isGenerator()) titleClip = clip.id;
        QVERIFY(titleClip);
        QVERIFY(!QFileInfo::exists(dir_.filePath(".linked.agent.montage")));  // the copy it worked on is gone
        // Reading changes nothing.
        r = call("montage_project_info");
        QVERIFY(!r.value("isError").toBool() && r.value("structuredContent").toObject().contains("tracks"));
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        // The context: moving the playhead and selecting the new title for the editor to see.
        r = call("montage_live_context", {{"playhead", 2.0}, {"select", QJsonArray{double(titleClip)}}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(state()->playhead(), FrameTime(std::llround(2.0 * state()->sequence()->fpsValue())));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{titleClip});
        const QJsonObject info = r.value("structuredContent").toObject();
        QCOMPARE(info.value("selected_clips").toArray().at(0).toObject().value("id").toDouble(), double(titleClip));
        QCOMPARE(info.value("project").toString(), dir_.filePath("linked.montage"));
        // Undo takes back the agent's change; an editor's change it leaves alone.
        r = call("montage_undo");
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(!edit::clipById(*state()->sequence(), titleClip));
        QVERIFY(state()->edit("Editor's marker", [](Project&, Sequence& s) {
            s.markers.push_back(Marker{5, 0, "mine", "", 0, false});
            return true;
        }));
        r = call("montage_undo");
        QVERIFY(r.value("isError").toBool());
        QCOMPARE(state()->undoText(), QString("Editor's marker"));
        // The editor changes the project while a tool runs: the tool's result is not applied.
        QByteArray answer;
        link->handle(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"},
                                               {"params", QJsonObject{{"name", "montage_add_marker"}, {"arguments", QJsonObject{{"at", 3}, {"name", "agent"}}}}}})
                         .toJson(QJsonDocument::Compact),
                     [&](QByteArray a) { answer = a; });
        QVERIFY(state()->edit("Meanwhile", [](Project&, Sequence& s) {
            s.markers.push_back(Marker{8, 0, "meanwhile", "", 0, false});
            return true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty(), 30000);
        QVERIFY2(answer.contains("not applied"), answer.constData());
        QCOMPARE(state()->undoText(), QString("Meanwhile"));
        for (const Marker& m : state()->sequence()->markers) QVERIFY(m.name != "agent");
        // The stdio bridge (montage-cli mcp --live) relays to the app.
        std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})" "\n"
                              R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"montage_live_context","arguments":{}}})" "\n");
        std::ostringstream out;
        std::atomic<bool> bridged{false};
        std::thread bridge([&] {
            runLiveBridge(in, out);
            bridged = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(bridged.load(), 30000);
        bridge.join();
        const QList<QByteArray> lines = QByteArray::fromStdString(out.str()).trimmed().split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].contains("montage_live_context"));
        QVERIFY2(lines[1].contains("\"sequence\""), lines[1].constData());
        // Off while an agent's tool runs: its result is not applied, and its calls still waiting are dropped.
        auto markerCall = [](int id, const QString& name) {
            return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                                             {"params", QJsonObject{{"name", "montage_add_marker"}, {"arguments", QJsonObject{{"at", 4}, {"name", name}}}}}})
                .toJson(QJsonDocument::Compact);
        };
        auto hasMarker = [&](const QString& name) {
            for (const Marker& m : state()->sequence()->markers)
                if (QString::fromStdString(m.name) == name) return true;
            return false;
        };
        QByteArray running, waiting;
        link->handle(markerCall(10, "while turning off"), [&](QByteArray a) { running = a; }, true);
        link->handle(markerCall(11, "queued"), [&](QByteArray a) { waiting = a; }, true);
        QVERIFY(link->busy());
        // Off: the connection file goes, and the bridge says Montage is not reachable.
        QVERIFY(win_->setAgentLink(false));
        QVERIFY(!link->running() && !QFileInfo::exists(liveConnectionFile()));
        QTRY_VERIFY_WITH_TIMEOUT(!running.isEmpty(), 30000);
        QVERIFY2(running.contains("turned off"), running.constData());
        QTRY_VERIFY(!link->busy());
        QTest::qWait(100);
        QVERIFY(waiting.isEmpty() && !hasMarker("while turning off") && !hasMarker("queued"));
        QVERIFY(!QFileInfo::exists(dir_.filePath(".linked.agent.montage")));
        // A link destroyed while its tool runs (the app quitting): the tool finishes by itself, nothing is applied and
        // its copy of the project is removed.
        {
            auto* doomed = new LiveLink(state());
            QByteArray never;
            doomed->handle(markerCall(12, "after quitting"), [&](QByteArray a) { never = a; });
            QVERIFY(doomed->busy());
            delete doomed;
            QVERIFY(QThreadPool::globalInstance()->waitForDone(30000));
            QTest::qWait(50);
            QVERIFY(never.isEmpty() && !hasMarker("after quitting"));
            QVERIFY(!QFileInfo::exists(dir_.filePath(".linked.agent.montage")));
        }
        std::istringstream in2(R"({"jsonrpc":"2.0","id":3,"method":"ping"})" "\n");
        std::ostringstream out2;
        runLiveBridge(in2, out2);
        QVERIFY2(QByteArray::fromStdString(out2.str()).contains("-32000"), out2.str().c_str());
        qunsetenv("MONTAGE_MCP_LIVE_FILE");
        state()->newProject();
    }

    void assistantPanelEditsWithTools() {
        // A stand-in for the model's service: answers each request with the next scripted reply, keeping what was asked.
        struct FakeModel {
            QTcpServer server;
            QList<QByteArray> replies;
            QList<QByteArray> heads, bodies;
            bool hold = false;  // leave the next request unanswered
        } fake;
        QVERIFY(fake.server.listen(QHostAddress::LocalHost));
        QObject::connect(&fake.server, &QTcpServer::newConnection, &fake.server, [&fake] {
            QTcpSocket* s = fake.server.nextPendingConnection();
            auto* buf = new QByteArray;
            QObject::connect(s, &QTcpSocket::disconnected, s, [s, buf] {
                delete buf;
                s->deleteLater();
            });
            QObject::connect(s, &QTcpSocket::readyRead, s, [&fake, s, buf] {
                *buf += s->readAll();
                const int end = int(buf->indexOf("\r\n\r\n"));
                if (end < 0) return;
                const QByteArray head = buf->left(end).toLower();
                const int at = int(head.indexOf("content-length:"));
                const int length = at < 0 ? 0 : head.mid(at + 15, head.indexOf('\n', at) - at - 15).trimmed().toInt();
                if (buf->size() - end - 4 < length) return;
                fake.heads << buf->left(end);
                fake.bodies << buf->mid(end + 4, length);
                buf->clear();
                if (fake.hold || fake.replies.isEmpty()) return;
                const QByteArray body = fake.replies.takeFirst();
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                         "\r\nConnection: close\r\n\r\n" + body);
                s->disconnectFromHost();
            });
        });
        const QString endpoint = QStringLiteral("http://127.0.0.1:%1").arg(fake.server.serverPort());
        state()->newProject();
        AssistantPanel* panel = win_->assistant();
        QVERIFY(panel && win_->findChild<QDockWidget*>("assistant"));
        AssistantSession* session = panel->session();
        QVERIFY(session->toolCount() > 90);
        // Claude: a title asked for, added with the title tool, then a word on what was done.
        AssistantConfig c;
        c.endpoint = endpoint;
        c.apiKey = "test-key";
        session->setConfig(c);
        fake.replies << R"({"content":[{"type":"text","text":"Adding it."},{"type":"tool_use","id":"tu1","name":"montage_add_title","input":{"text":"Hello there","at":1}}],"stop_reason":"tool_use"})"
                     << R"({"content":[{"type":"text","text":"Done: a title at 1 s."}],"stop_reason":"end_turn"})";
        bool finished = false;
        auto connection = connect(session, &AssistantSession::finished, this, [&] { finished = true; });
        panel->ask("Put a title saying Hello there at one second");
        QTRY_VERIFY_WITH_TIMEOUT(finished, 30000);
        QCOMPARE(fake.bodies.size(), 2);
        QVERIFY(fake.heads[0].startsWith("POST /v1/messages"));
        QVERIFY(fake.heads[0].toLower().contains("x-api-key: test-key") && fake.heads[0].toLower().contains("anthropic-version: 2023-06-01"));
        const QJsonObject first = QJsonDocument::fromJson(fake.bodies[0]).object();
        QCOMPARE(first.value("model").toString(), QString("claude-opus-5-5"));
        QVERIFY(!first.value("system").toString().isEmpty());
        const QJsonArray tools = first.value("tools").toArray();
        QVERIFY(tools.size() > 90 && tools.last().toObject().contains("cache_control"));
        const QJsonArray messages = QJsonDocument::fromJson(fake.bodies[1]).object().value("messages").toArray();
        QCOMPARE(messages.size(), 3);  // the ask, the model's turn, the tool's answer
        const QJsonObject result = messages[2].toObject().value("content").toArray().at(0).toObject();
        QVERIFY(result.value("type") == "tool_result" && result.value("tool_use_id") == "tu1" && !result.value("is_error").toBool());
        bool titled = false;
        for (const Track& t : state()->sequence()->videoTracks)
            for (const Clip& clip : t.clips) titled |= clip.isGenerator();
        QVERIFY(titled);
        QVERIFY2(state()->undoText().startsWith("Assistant: "), qPrintable(state()->undoText()));
        const QString log = panel->transcriptText();
        QVERIFY2(log.contains("Adding it.") && log.contains("Done: a title at 1 s.") && log.contains("✓"), qPrintable(log));
        // Stop: a request left unanswered is dropped and the conversation stays usable.
        fake.hold = true;
        finished = false;
        panel->ask("And another");
        QTRY_VERIFY_WITH_TIMEOUT(fake.bodies.size() == 3, 10000);
        QVERIFY(session->busy());
        session->stop();
        QVERIFY(!session->busy() && finished);
        QVERIFY(panel->transcriptText().contains("Stopped"));
        fake.hold = false;
        // No key: said so, nothing sent.
        qunsetenv("ANTHROPIC_API_KEY");
        c.apiKey.clear();
        session->setConfig(c);
        session->clear();
        QString failure;
        auto failConnection = connect(session, &AssistantSession::failed, this, [&](const QString& why) { failure = why; });
        session->send("Anything");
        QVERIFY2(failure.contains("API key"), qPrintable(failure));
        QCOMPARE(fake.bodies.size(), 3);
        disconnect(failConnection);
        // An OpenAI-compatible endpoint (a local model): the context tool moves the playhead, then the answer.
        // (A different service starts a new conversation: the earlier one is in Claude's form.)
        c.provider = "openai";
        c.model = "local-model";
        c.apiKey = "k";
        session->setConfig(c);
        const QJsonObject call{{"id", "c1"}, {"type", "function"},
                               {"function", QJsonObject{{"name", "montage_live_context"}, {"arguments", QStringLiteral("{\"playhead\":3}")}}}};
        const QJsonObject toolTurn{{"role", "assistant"}, {"content", QJsonValue()}, {"tool_calls", QJsonArray{call}}};
        const QJsonObject answerTurn{{"role", "assistant"}, {"content", "The playhead is at 3 seconds."}};
        fake.replies << QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", toolTurn}, {"finish_reason", "tool_calls"}}}}}).toJson(QJsonDocument::Compact)
                     << QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", answerTurn}, {"finish_reason", "stop"}}}}}).toJson(QJsonDocument::Compact);
        finished = false;
        panel->ask("Go to three seconds");
        QTRY_VERIFY_WITH_TIMEOUT(finished, 30000);
        QCOMPARE(fake.bodies.size(), 5);
        QVERIFY(fake.heads[3].startsWith("POST /v1/chat/completions"));
        QVERIFY(fake.heads[3].toLower().contains("authorization: bearer k"));
        const QJsonObject openFirst = QJsonDocument::fromJson(fake.bodies[3]).object();
        QCOMPARE(openFirst.value("model").toString(), QString("local-model"));
        QCOMPARE(openFirst.value("messages").toArray().at(0).toObject().value("role").toString(), QString("system"));
        QCOMPARE(openFirst.value("messages").toArray().size(), 2);  // the system prompt and this request only
        QCOMPARE(openFirst.value("tools").toArray().at(0).toObject().value("type").toString(), QString("function"));
        const QJsonArray openMessages = QJsonDocument::fromJson(fake.bodies[4]).object().value("messages").toArray();
        const QJsonObject toolMessage = openMessages.last().toObject();
        QVERIFY(toolMessage.value("role") == "tool" && toolMessage.value("tool_call_id") == "c1");
        QCOMPARE(state()->playhead(), FrameTime(std::llround(3 * state()->sequence()->fpsValue())));
        QVERIFY(panel->transcriptText().contains("The playhead is at 3 seconds."));
        disconnect(connection);
        session->setConfig(AssistantConfig{});
        state()->newProject();
    }

    void openFxPluginsInTheBrowserAndInspector() {
        ofx::Registry& reg = ofx::Registry::instance();
        reg.setCachePath(dir_.filePath("ofx-cache.json").toStdString());
        reg.setProbeExecutable(MONTAGE_PLUGIN_PROBE);
        reg.setSearchPaths({MONTAGE_TEST_OFX_DIR "/good"});
        QCOMPARE(reg.scan(), 3);
        auto* browser = win_->findChild<EffectsBrowser*>();
        QVERIFY(browser);
        browser->reload();
        auto* tree = browser->findChild<QTreeWidget*>();
        QTreeWidgetItem* leaf = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == "ofx:org.montage.test.invert/1") leaf = *it;
        QVERIFY(leaf);
        QCOMPARE(leaf->text(0), QString("Test Invert"));
        QVERIFY(leaf->parent() && leaf->parent()->text(0) == "Montage Test" && leaf->parent()->parent()->text(0) == "Video Plugins");
        // Applied to the selected clip from the browser: an "ofx" effect, shown in the Inspector with its controls.
        state()->newProject();
        QVERIFY(state()->edit("Matte", [](Project& p, Sequence& s) {
            return edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 50)).ok;
        }));
        const Id clip = state()->sequence()->videoTracks[0].clips.front().id;
        state()->setSelection({clip}, false);
        emit browser->applyRequested("ofx:org.montage.test.invert/1", EffectCategory::VideoFilter);
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QCOMPARE(c->effects.size(), size_t(1));
        QCOMPARE(c->effects[0].type, std::string("ofx"));
        QCOMPARE(state()->undoText(), QString("Add Test Invert"));
        auto titled = [&] {
            for (QLabel* l : win_->findChild<InspectorWidget*>()->findChildren<QLabel*>())
                if (l->text().contains("Test Invert")) return true;
            return false;
        };
        QTRY_VERIFY(titled());
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), clip)->effects.empty());
        state()->newProject();
    }

    void extendClipCommand() {
        QVERIFY(win_->findChild<QAction*>("extendClip1s"));
        QVERIFY(win_->findChild<QAction*>("extendClip2s"));
        QVERIFY(win_->findChild<QAction*>("extendClipToPlayhead"));
        // A two-second shot with sound.
        const QString video = dir_.filePath("shot.mp4");
        {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, makeGeneratorClip(gen, "bars", 60));
            MediaItem src;
            src.id = gen.newId();
            std::string err;
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", src, &err));
            gen.media.push_back(src);
            Clip sound = makeClip(gen, src, TrackKind::Audio, gs);
            sound.duration = 60;
            edit::overwrite(gen, gs, {TrackKind::Audio, 0}, sound);
            ExportSettings st;
            st.path = video.toStdString();
            st.preset = "ultrafast";
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        const FrameTime len = 40;
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            if (!edit::placeMedia(p, s, ids[0], 0, 0, len, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok) return false;
            Clip next = makeGeneratorClip(p, "color", 10);
            next.start = len;
            return edit::overwrite(p, s, {TrackKind::Video, 0}, next).ok;
        }));
        const Sequence* s = state()->sequence();
        const Id shot = s->videoTracks[0].clips.front().id, next = s->videoTracks[0].clips.back().id;
        const FrameTime second = FrameTime(std::llround(s->fpsValue()));
        // Extended by a second: the picture held and the room under its sound, the next clip pushed along.
        const std::vector<Id> made = win_->extendClip(shot, second);
        QCOMPARE(made.size(), size_t(2));
        s = state()->sequence();
        const Clip* e = edit::clipById(*s, made[0]);
        QVERIFY(e && e->start == len && e->duration == second);
        QCOMPARE(e->timing.p("speed", 0, 100), 0.0);
        const Clip* tone = edit::clipById(*s, made[1]);
        QVERIFY(tone && tone->start == len && tone->duration == second);
        QVERIFY(QString::fromStdString(state()->project().findMedia(tone->mediaId)->path).contains("Room Tone"));
        QCOMPARE(edit::clipById(*s, next)->start, len + second);
        // One undo step takes it all back.
        state()->undo();
        s = state()->sequence();
        QVERIFY(!edit::clipById(*s, made[0]) && !edit::clipById(*s, made[1]));
        QCOMPARE(edit::clipById(*s, next)->start, len);
        // Without ripple there is no room; a generated clip cannot be extended this way.
        QVERIFY(win_->extendClip(shot, second, false).empty());
        QVERIFY(win_->extendClip(next, second).empty());
        // To the playhead, from the menu.
        state()->setSelection({shot}, false);
        state()->setPlayhead(len + 4);
        win_->findChild<QAction*>("extendClipToPlayhead")->trigger();
        QCOMPARE(state()->sequence()->videoTracks[0].clips.at(1).duration, FrameTime(5));
    }

    void roomToneGapFill() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("fillRoomTone"));
        // A take with a steady room under a tone that comes and goes.
        constexpr int sr = 48000;
        const QString wav = dir_.filePath("take.wav");
        {
            std::vector<int16_t> pcm(size_t(sr) * 3);
            uint32_t x = 1;
            double lp = 0;
            for (size_t i = 0; i < pcm.size(); ++i) {
                x = x * 1664525u + 1013904223u;
                lp += 0.1 * ((double(x >> 8) / double(1 << 24)) * 2 - 1 - lp);
                const double v = 0.03 * lp + ((i / (sr / 4)) % 2 == 0 ? 0.2 * std::sin(2 * M_PI * 300 * double(i) / sr) : 0);
                pcm[i] = int16_t(std::lround(v * 32767));
            }
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
            const uint32_t bytes = uint32_t(pcm.size() * 2);
            f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(sr), u32(sr * 2), u16(2), u16(16), f.write("data", 4), u32(bytes);
            f.write(reinterpret_cast<const char*>(pcm.data()), qint64(bytes));
        }
        const auto ids = state()->importFiles({wav});
        QCOMPARE(ids.size(), size_t(1));
        const double fps = state()->sequence()->fpsValue();
        const FrameTime second = FrameTime(std::llround(fps));
        QVERIFY(state()->edit("Cut", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, second, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok &&
                   edit::placeMedia(p, s, ids[0], 2 * second, 2 * second, 3 * second, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const TrackRef a1{TrackKind::Audio, 0};
        QCOMPARE(win_->fillRoomTone(a1, second / 2), Id(0));  // a clip is there
        const Id made = win_->fillRoomTone(a1, second + 3);
        QVERIFY(made);
        const Clip* c = edit::clipById(*state()->sequence(), made);
        QVERIFY(c);
        QCOMPARE(c->start, second);
        QCOMPARE(c->duration, second);
        const MediaItem* m = state()->project().findMedia(c->mediaId);
        QVERIFY(m && QFileInfo::exists(QString::fromStdString(m->path)) && QString::fromStdString(m->path).contains("Room Tone"));
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), made));
        // Part of the gap, from a given clip.
        const Id first = trackAt(*state()->sequence(), a1)->clips.front().id;
        const Id part = win_->fillRoomTone(a1, second + 5, second + 10, first);
        QVERIFY(part);
        QCOMPARE(edit::clipById(*state()->sequence(), part)->duration, FrameTime(5));
    }

    void deleteGapsCommand() {
        loadDemo();
        QAction* action = win_->findChild<QAction*>("deleteGaps");
        QVERIFY(action);
        const FrameTime end = state()->sequence()->duration();
        QVERIFY(state()->edit("Late clip", [&](Project& p, Sequence& s) {
            Clip c = makeGeneratorClip(p, "color", 10);
            c.start = end + 25;
            return edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok;
        }));
        const Id late = state()->sequence()->videoTracks[0].clips.back().id;
        action->trigger();
        QCOMPARE(edit::clipById(*state()->sequence(), late)->start, end);
        QCOMPARE(state()->sequence()->duration(), end + 10);
        state()->undo();
        QCOMPARE(edit::clipById(*state()->sequence(), late)->start, end + 25);
        QCOMPARE(win_->deleteGaps(), 1);
        QCOMPARE(win_->deleteGaps(), 0);  // none left
    }

    void broadcastMxfExport() {
        loadDemo();
        RenderQueue* queue = win_->renderQueue();
        const size_t jobs = queue->jobs().size();
        int xdcam = -1, mp4 = -1;
        for (size_t i = 0; i < exportPresets().size(); ++i) {
            if (exportPresets()[i].name == "XDCAM HD422 (MXF)") xdcam = int(i);
            if (mp4 < 0 && exportPresets()[i].extension == "mp4") mp4 = int(i);
        }
        QVERIFY(xdcam >= 0 && mp4 >= 0);
        ExportDialog ed(state(), win_.get());
        ed.setQueue(queue);
        auto* preset = ed.findChild<QComboBox*>("exportPreset");
        auto* tc = ed.findChild<QLineEdit*>("exportStartTimecode");
        QVERIFY(tc);
        preset->setCurrentIndex(mp4);
        QVERIFY(!tc->isEnabled());  // MP4 keeps no timecode
        auto* delivery = ed.findChild<QComboBox*>("exportMxfDelivery");
        auto* interlaced = ed.findChild<QCheckBox*>("exportInterlaced");
        QVERIFY(delivery && interlaced);
        QVERIFY(!delivery->isEnabled());
        preset->setCurrentIndex(xdcam);
        QVERIFY(tc->isEnabled());
        tc->setText("10:00:00:00");
        // XDCAM HD422's deliveries: as encoded, RDD 9, AS-11 X8; AS-11 X8 with its metadata, coded interlaced.
        QCOMPARE(delivery->isEnabled(), broadcastMxfAvailable());
        QCOMPARE(delivery->count(), 3);
        QCOMPARE(delivery->itemData(2).toString(), QStringLiteral("as11-x8"));
        delivery->setCurrentIndex(2);
        QVERIFY(ed.findChild<QPushButton*>("exportAs11Metadata")->isEnabled() == broadcastMxfAvailable());
        QVERIFY(interlaced->isEnabled());
        interlaced->setChecked(true);
        ed.findChild<QLineEdit*>("exportPath")->setText(dir_.path() + "/delivery.mxf");
        ed.findChild<QPushButton*>("addToQueue")->click();
        QCOMPARE(ed.result(), int(QDialog::Accepted));
        QCOMPARE(queue->jobs().size(), jobs + 1);
        const RenderQueue::Job& job = queue->jobs().back();
        QCOMPARE(job.settings.videoCodec, std::string("mpeg2video"));
        QCOMPARE(job.settings.monoAudioTracks, 8);
        QCOMPARE(job.settings.startTimecode, formatTimecode(FrameTime(std::llround(36000 * state()->sequence()->fpsValue())), state()->sequence()->fps));
        QCOMPARE(job.settings.width, 1920);
        QVERIFY(job.settings.interlaced);
        QCOMPARE(job.settings.mxfDelivery, broadcastMxfAvailable() ? std::string("as11-x8") : std::string());
        queue->remove(job.id);
        appSettings().remove("export/mxfDelivery");
        appSettings().remove("export/interlaced");
        // A preset remembered under its old name, with its maker's name in front, comes back under its new one.
        const QVariant remembered = appSettings().value("export/preset");
        appSettings().setValue("export/preset", "Avid DNxHR HQ (MXF)");
        const QString restored = ExportDialog(state(), win_.get()).findChild<QComboBox*>("exportPreset")->currentText();
        appSettings().setValue("export/preset", remembered);  // later tests export with the preset they had
        QCOMPARE(restored, QStringLiteral("DNxHR HQ (MXF)"));
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

    void redactFacesFromTheClipMenu() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("redactFaces"));
        QVERIFY(!win_->redactFacesDialog());  // no clip
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 90, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        state()->setSelection({clip}, false);
        state()->setPlayhead(30);
        RedactFacesDialog* dlg = win_->redactFacesDialog();
        QVERIFY(dlg && dlg->isVisible());
        QVERIFY(dlg->status().contains("Find the faces"));
        auto effect = [&]() -> const Effect* {
            for (const Effect& e : edit::clipById(*state()->sequence(), clip)->effects)
                if (e.type == "redact_faces") return &e;
            return nullptr;
        };
        if (!faceSearchAvailable() || !faceModel().installed()) {
            QVERIFY(!dlg->findFaces(true));
            // Still: every face found as it plays.
            QVERIFY(dlg->apply());
            QVERIFY(effect() && effect()->s("tracks").empty());
            dlg->close();
            state()->newProject();
            QSKIP("Set MONTAGE_FACE_MODEL to the YuNet and SFace models for the rest");
        }
        // Kennedy's face, where the still is fitted in the frame.
        const Sequence& seq = *state()->sequence();
        const double k = seq.height / 415.0, left = (seq.width - 320 * k) / 2;
        const int fx = int(left + 138 * k), fy = int(166 * k);
        auto detail = [&](const Image& im) {
            double sum = 0;
            for (int y = fy - 20; y < fy + 20; ++y)
                for (int x = fx - 20; x < fx + 20; ++x) sum += std::fabs(im.at(x + 1, y)[1] - im.at(x, y)[1]) + std::fabs(im.at(x, y + 1)[1] - im.at(x, y)[1]);
            return sum;
        };
        const Image plain = renderSequenceFrame(state()->project(), seq, 30, {});
        QVERIFY(dlg->findFaces(true));
        QVERIFY(!dlg->busy());
        QCOMPARE(dlg->groupCount(), 1);
        QCOMPARE(dlg->list()->count(), 1);
        QVERIFY(!dlg->list()->item(0)->icon().isNull());
        QVERIFY(dlg->redacted(0));
        QVERIFY2(dlg->status().contains("1 face") && dlg->status().contains("1 covered"), qPrintable(dlg->status()));
        // Covered: one undoable step that puts the effect first on the clip, with the analysis.
        QVERIFY(dlg->apply());
        QVERIFY(effect() && !effect()->s("tracks").empty() && effect()->s("keep").empty());
        QCOMPARE(&edit::clipById(*state()->sequence(), clip)->effects.front(), effect());
        Image covered = renderSequenceFrame(state()->project(), *state()->sequence(), 30, {});
        qInfo("face detail %.1f, covered %.1f", detail(plain), detail(covered));
        QVERIFY(detail(covered) < detail(plain) * 0.3);
        state()->undo();
        QVERIFY(!effect());
        state()->redo();
        QVERIFY(effect());
        // Left showing, pixelated next time: the clip as it was; reopened, the choice is remembered.
        dlg->setRedacted(0, false);
        dlg->setStyle(1);
        QVERIFY(dlg->apply());
        QVERIFY(!effect()->s("keep").empty());
        QCOMPARE(effect()->p("style", 0), 1.0);
        const Image shown = renderSequenceFrame(state()->project(), *state()->sequence(), 30, {});
        QCOMPARE(detail(shown), detail(plain));
        dlg->close();
        QTest::qWait(10);
        dlg = win_->redactFacesDialog();
        QVERIFY(dlg);
        QCOMPARE(dlg->groupCount(), 1);
        QVERIFY(!dlg->redacted(0));
        QVERIFY(dlg->status().contains("0 covered"));
        // Another sequence closes it.
        QPointer<RedactFacesDialog> guard(dlg);
        state()->newProject();
        QTest::qWait(10);
        QVERIFY(!guard || !guard->isVisible());
    }

    void panToFollowFromTheClipMenu() {
        // A textured card crossing a flat frame left to right (x = 80 + pos(t) of 160), with sound.
        const QString video = dir_.path() + "/crossing.mp4";
        const int frames = 30;
        auto pos = [&](double t) { return -55 + 110 * t / (frames - 1); };
        {
            QImage card(32, 32, QImage::Format_RGB32);
            card.fill(QColor(30, 30, 30));
            {
                QPainter pa(&card);
                std::mt19937 rng(5);
                std::uniform_int_distribution<int> xy(0, 28), sz(2, 7), c(0, 255);
                for (int i = 0; i < 40; ++i) pa.fillRect(xy(rng), xy(rng), sz(rng), sz(rng), QColor(c(rng), c(rng), c(rng)));
            }
            const QString cardPng = dir_.path() + "/crossing-card.png";
            QVERIFY(card.save(cardPng));
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 160;
            gs.height = 90;
            gs.fps = {25, 1};
            Clip bg = makeGeneratorClip(gen, "color", frames);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, bg);
            edit::addTrack(gen, gs, TrackKind::Video);
            MediaItem cm;
            cm.id = gen.newId();
            std::string err;
            QVERIFY(probeMedia(cardPng.toStdString(), cm, &err));
            gen.media.push_back(cm);
            Clip fg = makeClip(gen, cm, TrackKind::Video, gs);
            fg.duration = frames;
            fg.motion.params["scale"] = Param(100.0);
            fg.motion.params["pos_x"].addKey(0, pos(0), Interp::Linear);
            fg.motion.params["pos_x"].addKey(frames - 1, pos(frames - 1), Interp::Linear);
            edit::overwrite(gen, gs, {TrackKind::Video, 1}, fg);
            MediaItem src;
            src.id = gen.newId();
            QVERIFY(probeMedia(MONTAGE_TEST_DATA_DIR "/jfk.wav", src, &err));
            gen.media.push_back(src);
            Clip snd = makeClip(gen, src, TrackKind::Audio, gs);
            snd.duration = frames;
            edit::overwrite(gen, gs, {TrackKind::Audio, 0}, snd);
            ExportSettings st;
            st.path = video.toStdString();
            st.preset = "ultrafast";
            st.crf = 12;
            QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        }
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("panFollow"));
        QVERIFY(!win_->panFollowDialog());  // no clip
        const auto ids = state()->importFiles({video});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            s.width = 160;
            s.height = 90;
            s.fps = {25, 1};
            return edit::placeMedia(p, s, ids[0], 0, 0, frames, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const Id sound = state()->sequence()->audioTracks[0].clips.at(0).id;
        state()->setSelection({sound}, false);
        state()->setPlayhead(15);
        PanFollowDialog* dlg = win_->panFollowDialog();
        QVERIFY(dlg && dlg->isVisible());
        QCOMPARE(dlg->frame(), FrameTime(15));
        QVERIFY(dlg->findChild<QWidget*>("panFollowPicker"));
        QVERIFY2(dlg->status().contains("pan"), qPrintable(dlg->status()));
        // It starts on the subject: the card.
        QVERIFY2(std::fabs(dlg->pointX() * 160 - (80 + pos(15))) < 16, qPrintable(QString::number(dlg->pointX())));
        // A click on the picture moves the point.
        auto* picker = dlg->findChild<QWidget*>("panFollowPicker");
        QTest::mouseClick(picker, Qt::LeftButton, {}, QPoint(picker->width() / 4, picker->height() / 2));
        QVERIFY(dlg->pointX() < 0.4);
        dlg->setPoint((80 + pos(15)) / 160, 0.5);
        dlg->setSize(2);
        QVERIFY(dlg->track(true));
        QVERIFY(!dlg->busy());
        QCOMPARE(dlg->keyCount(), frames);
        QVERIFY2(dlg->status().contains("Followed"), qPrintable(dlg->status()));
        // One undoable step on the track's pan lane, read back as the card crosses.
        const Track& tr = state()->sequence()->audioTracks[0];
        QVERIFY(tr.panAuto.animated());
        QCOMPARE(tr.automation, int(AutomationMode::Read));
        QVERIFY2(trackPanAt(tr, 2) < -0.4 && trackPanAt(tr, 27) > 0.4, qPrintable(QString("%1 %2").arg(trackPanAt(tr, 2)).arg(trackPanAt(tr, 27))));
        state()->undo();
        QVERIFY(!state()->sequence()->audioTracks[0].panAuto.animated());
        state()->redo();
        QVERIFY(state()->sequence()->audioTracks[0].panAuto.animated());
        // Half the stage: half the pan, writing over the span again.
        dlg->setWidth(0.5);
        QVERIFY(dlg->track(true));
        const double half = trackPanAt(state()->sequence()->audioTracks[0], 27);
        QVERIFY2(half > 0.2 && half < 0.45, qPrintable(QString::number(half)));
        // The sound moved while it was being followed: the frames tracked no longer fit, so nothing is written.
        const Param lane = state()->sequence()->audioTracks[0].panAuto;
        dlg->setWidth(1.0);
        QVERIFY(dlg->track(false));
        QVERIFY(state()->edit("Nudge", [](Project&, Sequence& s) {
            s.audioTracks[0].clips[0].start += 3;
            return true;
        }));
        QTRY_VERIFY(!dlg->busy());
        QTest::qWait(400);  // the picture refreshing after the edit keeps what the attempt said
        QVERIFY2(dlg->status().contains("moved"), qPrintable(dlg->status()));
        QVERIFY(state()->sequence()->audioTracks[0].panAuto == lane);
        state()->undo();
        // In the middle of a drag (or a fader pass), the result waits for it to end rather than breaking it.
        dlg->setWidth(0.75);
        state()->beginGesture("Drag");
        QVERIFY(dlg->track(true));
        QVERIFY(state()->inGesture());
        QVERIFY(state()->sequence()->audioTracks[0].panAuto == lane);
        state()->endGesture(false);
        QTRY_VERIFY(!(state()->sequence()->audioTracks[0].panAuto == lane));
        const double wider = trackPanAt(state()->sequence()->audioTracks[0], 27);
        QVERIFY2(wider > half + 0.1, qPrintable(QString::number(wider)));
        // Another sequence closes it, and what was being followed is never written into the new one.
        QPointer<PanFollowDialog> guard(dlg);
        QVERIFY(dlg->track(false));
        state()->newProject();
        QTest::qWait(300);
        QVERIFY(!guard || !guard->isVisible());
        QVERIFY(!state()->canUndo());
    }

    void closeUpOnAFace() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 90, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        state()->setPlayhead(30);
        const size_t tracks = state()->sequence()->videoTracks.size();
        QVERIFY(win_->findChild<QAction*>("closeUp"));
        const Id id = win_->closeUp(1.5);
        QVERIFY(id);
        const Clip* c = edit::clipById(*state()->sequence(), id);
        const auto loc = edit::locate(*state()->sequence(), id);
        QVERIFY(c && loc && loc->track.index == 1);
        QVERIFY(state()->sequence()->videoTracks.size() >= tracks);
        QCOMPARE(c->motion.p("scale", 0), 150.0);
        if (faceSearchAvailable() && faceModel().installed())
            QVERIFY(std::fabs(c->motion.p("pos_x", 0)) + std::fabs(c->motion.p("pos_y", 0)) > 1);  // moved to the face
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), id));
        state()->newProject();
    }

    void sourceEditModes() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg")});
        QCOMPARE(ids.size(), size_t(1));
        // Two 60-frame stills on V1 (an edit at 60), and a 20-frame source range.
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok &&
                   edit::placeMedia(p, s, ids[0], 60, 0, 60, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        const auto seq = [this] { return state()->sequence(); };
        const auto v1 = [&] { return seq()->videoTracks[0].clips.size(); };
        state()->setSourceMedia(ids[0]);
        state()->setSourceIn(0);
        state()->setSourceOut(19);
        for (const char* name : {"appendAtEnd", "placeOnTop", "rippleOverwrite", "smartInsert"}) QVERIFY2(win_->findChild<QAction*>(name), name);
        // Append at End: after the last clip, whatever the playhead.
        state()->setPlayhead(10);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::Append));
        QCOMPARE(seq()->duration(), FrameTime(140));
        QCOMPARE(seq()->videoTracks[0].clips.back().start, FrameTime(120));
        QCOMPARE(state()->playhead(), FrameTime(140));
        state()->undo();
        // Place on Top: the first free track above V1, then the next one above that; nothing below moves.
        state()->setPlayhead(30);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::PlaceOnTop));
        QCOMPARE(state()->playhead(), FrameTime(50));
        state()->setPlayhead(30);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::PlaceOnTop));
        QVERIFY(seq()->videoTracks.size() >= 3);
        for (int i : {1, 2}) {
            QCOMPARE(seq()->videoTracks[size_t(i)].clips.size(), size_t(1));
            QCOMPARE(seq()->videoTracks[size_t(i)].clips[0].start, FrameTime(30));
            QCOMPARE(seq()->videoTracks[size_t(i)].clips[0].duration, FrameTime(20));
        }
        QCOMPARE(v1(), size_t(2));
        QCOMPARE(seq()->duration(), FrameTime(120));
        state()->undo();
        state()->undo();
        // Ripple Overwrite: the 60-frame clip under the playhead becomes the 20-frame range and the end pulls in.
        state()->setPlayhead(70);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::RippleOverwrite));
        QCOMPARE(v1(), size_t(2));
        QCOMPARE(seq()->videoTracks[0].clips[1].start, FrameTime(60));
        QCOMPARE(seq()->videoTracks[0].clips[1].duration, FrameTime(20));
        QCOMPARE(seq()->duration(), FrameTime(80));
        state()->undo();
        QCOMPARE(seq()->duration(), FrameTime(120));
        // With the playhead over nothing it is refused.
        state()->setPlayhead(500);
        QVERIFY(!state()->sourceEdit(EditorState::SourceEdit::RippleOverwrite));
        // Smart Insert: at 55 the nearest edit is 60, so the range goes in there rather than splitting the first clip.
        state()->setPlayhead(55);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::SmartInsert));
        QCOMPARE(v1(), size_t(3));
        QCOMPARE(seq()->videoTracks[0].clips[0].duration, FrameTime(60));
        QCOMPARE(seq()->videoTracks[0].clips[1].start, FrameTime(60));
        QCOMPARE(seq()->videoTracks[0].clips[2].start, FrameTime(80));
        QCOMPARE(state()->playhead(), FrameTime(80));
        state()->setSourceMedia(0);
        state()->newProject();
    }

    void audioRolesMenuAndIndex() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, V1, {TrackKind::Audio, 0}, false).ok &&
                   edit::placeMedia(p, s, ids[0], 0, 0, 90, V1, {TrackKind::Audio, 1}, false).ok;
        }));
        const Id speech = state()->sequence()->audioTracks[0].clips.at(0).id, other = state()->sequence()->audioTracks[1].clips.at(0).id;
        auto role = [&](Id id) { return edit::clipById(*state()->sequence(), id)->role; };
        state()->setSelection({});
        QCOMPARE(win_->setSelectedRole(QStringLiteral("Music")), 0);  // nothing selected
        state()->setSelection({other});
        QVERIFY(win_->findChild<QAction*>("roleMusic"));
        win_->findChild<QAction*>("roleMusic")->trigger();
        QCOMPARE(role(other), std::string("Music"));
        // The Sequence Index shows the role, and its Music box mutes the role (one undo step).
        auto* panel = win_->findChild<SequenceIndexPanel*>();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel->setFilter(QStringLiteral("Music"));
        QCOMPARE(panel->rowCount(), 1);
        QCOMPARE(panel->cell(0, SequenceIndexPanel::Role), QString("Music"));
        panel->setFilter(QString());
        auto* box = panel->findChild<QCheckBox*>(QStringLiteral("role_Music"));
        QVERIFY(box && box->isChecked());
        box->click();
        QVERIFY(edit::roleMuted(*state()->sequence(), "Music"));
        QVERIFY(!panel->roleHeard(QStringLiteral("Music")));
        state()->undo();
        QVERIFY(!edit::roleMuted(*state()->sequence(), "Music"));
        // A role of the editor's own: listed in the index and the menu, ticked for the selection.
        QCOMPARE(win_->setSelectedRole(QStringLiteral("Room Tone")), 1);
        QVERIFY(panel->roles().contains(QStringLiteral("Room Tone")));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(panel->findChild<QCheckBox*>(QStringLiteral("role_Room Tone")));
        auto* menu = win_->findChild<QMenu*>(QStringLiteral("audioRoleMenu"));
        QVERIFY(menu);
        emit menu->aboutToShow();
        QAction* custom = nullptr;
        for (QAction* a : menu->actions())
            if (a->property("customRole").toString() == QLatin1String("Room Tone")) custom = a;
        QVERIFY(custom && custom->isChecked());
        QVERIFY(!win_->findChild<QAction*>("roleMusic")->isChecked());
        // Detect Roles with nothing selected: the untagged speech is heard as dialogue; the tagged clip keeps its role.
        state()->setSelection({});
        QCOMPARE(win_->detectRoles(), 1);
        QCOMPARE(role(speech), std::string("Dialogue"));
        QCOMPARE(role(other), std::string("Room Tone"));
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

    void keyframeCopyPaste() {
        // Red's opacity fade and a blur's radius, copied and pasted onto Blue at the playhead.
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        QVERIFY(state()->edit("Keys", [&](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            c->motion.params["opacity"].addKey(10, 100);
            c->motion.params["opacity"].addKey(30, 0, Interp::Hold);
            Effect blur = makeEffect("gaussian_blur", p.newId());
            blur.params["radius"].addKey(20, 8);
            c->effects.push_back(blur);
            Clip* b = edit::clipById(s, blue);
            b->effects.push_back(makeEffect("gaussian_blur", p.newId()));
            return true;
        }));
        state()->setSelection({red}, false);
        win_->raisePanel("keyframes");
        auto* panel = win_->findChild<KeyframePanel*>();
        QTRY_COMPARE(panel->clip(), red);
        QVERIFY(!panel->copySelected());  // nothing selected
        std::set<KeyframePanel::Key> all;
        for (const auto& r : panel->rows())
            if (const Param* p = findParam(*edit::clipById(*state()->sequence(), red), r.address))
                for (const Keyframe& k : p->keys) all.insert({r.address, k.t});
        QCOMPARE(all.size(), size_t(3));
        panel->select(all);
        // Ctrl+C in the panel copies the keys, not the clip.
        panel->setFocus();
        QTest::keyClick(panel, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(KeyframePanel::hasCopiedKeys());
        // Onto Blue (starts at 60) at frame 70: the earliest copied key (10) lands at Blue's frame 10.
        state()->setSelection({blue}, false);
        QTRY_COMPARE(panel->clip(), blue);
        state()->setPlayhead(70);
        panel->setFocus();
        QTest::keyClick(panel, Qt::Key_V, Qt::ControlModifier);
        const Clip* b = edit::clipById(*state()->sequence(), blue);
        const Param& op = b->motion.params.at("opacity");
        QCOMPARE(op.keys.size(), size_t(2));
        QCOMPARE(op.keys[0].t, FrameTime(10));
        QCOMPARE(op.keys[1].t, FrameTime(30));
        QCOMPARE(op.keys[1].interp, Interp::Hold);
        QCOMPARE(b->effects.at(0).params.at("radius").keys.at(0).t, FrameTime(20));
        QCOMPARE(b->effects.at(0).params.at("radius").keys.at(0).v, 8.0);
        QCOMPARE(panel->selection().size(), size_t(3));  // the pasted keys
        // One undo step; Blue's own clips were not pasted over by the window's Paste.
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(2));
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), blue)->motion.params.count("opacity") ||
                !edit::clipById(*state()->sequence(), blue)->motion.params.at("opacity").animated());
        // From the menu-less API: a clip with no blur takes only the opacity.
        const Id title = state()->sequence()->videoTracks[1].clips.at(0).id;
        state()->setSelection({title}, false);
        QTRY_COMPARE(panel->clip(), title);
        state()->setPlayhead(15);
        QCOMPARE(panel->pasteAtPlayhead(), 2);
    }

    void reverseMatchFrameFromSource() {
        // The same still twice in the sequence (at 0 and 100): Shift+R finds the Source monitor's frame in each in turn.
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, 60, V1, {TrackKind::Audio, 0}, false).ok &&
                   edit::placeMedia(p, s, ids[0], 100, 10, 70, V1, {TrackKind::Audio, 0}, false).ok;
        }));
        auto* act = win_->findChild<QAction*>("reverseMatchFrame");
        QVERIFY(act);
        state()->setSourceMedia(0);
        QVERIFY(!win_->reverseMatchFrame());  // nothing in the Source monitor
        state()->setSourceMedia(ids[0]);
        auto* source = win_->findChild<PlaybackController*>(QStringLiteral("sourcePlayback"));
        QVERIFY(source);
        source->seek(30);
        QTRY_COMPARE(source->position(), FrameTime(30));
        state()->setPlayhead(0);
        act->trigger();
        QCOMPARE(state()->playhead(), FrameTime(30));
        const Id first = state()->sequence()->videoTracks[0].clips[0].id, second = state()->sequence()->videoTracks[0].clips[1].id;
        QCOMPARE(state()->selectedClips(), std::vector<Id>{first});
        act->trigger();  // again: the second use (source 30 is the second clip's frame 20)
        QCOMPARE(state()->playhead(), FrameTime(120));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{second});
        act->trigger();  // and round to the first
        QCOMPARE(state()->playhead(), FrameTime(30));
        source->seek(5);  // only in the first clip
        QTRY_COMPARE(source->position(), FrameTime(5));
        act->trigger();
        act->trigger();
        QCOMPARE(state()->playhead(), FrameTime(5));
        // Match Frame goes back: the second clip's frame 125 is the still's frame 35.
        state()->setPlayhead(125);
        win_->findChild<QAction*>("matchFrame")->trigger();
        QCOMPARE(state()->sourceMedia(), ids[0]);
        QTRY_COMPARE(source->position(), FrameTime(35));
        state()->setSourceMedia(0);
        state()->newProject();
    }

    void videoLayoutsFromTheMenu() {
        // The demo: Red then Blue on V1, a title on V2 over 15-60. At 30, Side by Side takes the two clips there.
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id, title = state()->sequence()->videoTracks[1].clips.at(0).id;
        state()->setSelection({}, false);
        state()->setPlayhead(30);
        QVERIFY(win_->findChild<QAction*>("layoutSideBySide"));
        win_->findChild<QAction*>("layoutSideBySide")->trigger();
        auto m = [&](Id id, const char* k) { return edit::clipById(*state()->sequence(), id)->motion.p(k, 0); };
        QCOMPARE(m(red, "pos_x"), -80.0);  // a 320-wide sequence
        QCOMPARE(m(title, "pos_x"), 80.0);
        QCOMPARE(state()->selectedClips().size(), size_t(2));
        state()->undo();
        QCOMPARE(m(red, "pos_x"), 0.0);
        // Nothing under the playhead: refused.
        state()->setSelection({}, false);
        state()->setPlayhead(500);
        QCOMPARE(win_->arrangeLayout(edit::Layout::Grid), 0);
        // Picture in picture from a selection of both.
        state()->setSelection({red, title}, false);
        // The status bar reads the selection's length: Red (0-60) and the title (15-60) run 60 frames.
        auto* readout = win_->findChild<QLabel*>(QStringLiteral("selectionInfo"));
        QVERIFY(readout);
        QCOMPARE(readout->text(), QString("2 clips selected · %1").arg(QString::fromStdString(formatTimecode(60, state()->sequence()->fps))));
        QCOMPARE(win_->arrangeLayout(edit::Layout::PictureInPicture), 2);
        QCOMPARE(m(red, "scale"), 100.0);
        QVERIFY(std::fabs(m(title, "scale") - 30) < 1e-9);
        state()->setSelection({}, false);
        QVERIFY(readout->text().isEmpty());
    }

    void compareSequencesDialog() {
        // The demo, then a second version with Blue 15 frames shorter and Red turned off.
        loadDemo();
        const Id first = state()->sequence()->id;
        Sequence copy = *state()->sequence();
        QVERIFY(state()->edit("Version 2", [&](Project& p, Sequence&) {
            copy.id = p.newId();
            copy.name = "Version 2";
            copy.videoTracks[0].clips[1].duration -= 15;
            copy.videoTracks[0].clips[0].enabled = false;
            p.sequences.push_back(copy);
            return true;
        }));
        state()->setActiveSequence(copy.id);
        QVERIFY(win_->findChild<QAction*>("compareSequences"));
        CompareDialog* dlg = win_->compareWith(first);
        QVERIFY(dlg);
        QCOMPARE(dlg->rowCount(), 2);
        QCOMPARE(dlg->cell(0, CompareDialog::Change), QString("Changed"));
        QCOMPARE(dlg->cell(0, CompareDialog::Details), QString("turned off"));
        QCOMPARE(dlg->cell(1, CompareDialog::Change), QString("Trimmed"));
        QCOMPARE(dlg->cell(1, CompareDialog::Track), QString("V1"));
        QVERIFY(dlg->findChild<QLabel*>("compareSummary")->text().contains("1 trimmed"));
        dlg->activate(1);
        QCOMPARE(state()->playhead(), FrameTime(60));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{copy.videoTracks[0].clips[1].id});
        QCOMPARE(dlg->addMarkers(), 2);
        QCOMPARE(state()->sequence()->markers.size(), size_t(2));
        QCOMPARE(state()->sequence()->markers[1].color, CompareDialog::labelFor(ChangeKind::Trimmed));
        state()->undo();
        QVERIFY(state()->sequence()->markers.empty());
        dlg->close();
        QVERIFY(!win_->compareWith(999999));
        state()->newProject();
    }

    void reconformFromCompareDialog() {
        // Version 2 of the demo: Blue first, then Red, and no title.
        loadDemo();
        const Id first = state()->sequence()->id;
        const size_t sequences = state()->project().sequences.size();
        Sequence v2 = *state()->sequence();
        QVERIFY(state()->edit("Version 2", [&](Project& p, Sequence&) {
            v2.id = p.newId();
            v2.name = "Version 2";
            std::swap(v2.videoTracks[0].clips[0].start, v2.videoTracks[0].clips[1].start);
            std::swap(v2.videoTracks[0].clips[0], v2.videoTracks[0].clips[1]);
            v2.videoTracks[1].clips.clear();
            p.sequences.push_back(v2);
            return true;
        }));
        state()->setActiveSequence(v2.id);
        CompareDialog* dlg = win_->compareWith(first);
        QVERIFY(dlg);
        dlg->showTab(1);
        // Blue kept its place in the order; Red's first 15 frames (the rest were under the title) moved after it and
        // run on; the title is gone.
        QCOMPARE(dlg->cutRowCount(), 4);
        QCOMPARE(dlg->cutCell(0, CompareDialog::CutKind), QString("Same"));
        QCOMPARE(dlg->cutCell(0, CompareDialog::CutShot), QString("Blue"));
        QCOMPARE(dlg->cutCell(0, CompareDialog::CutShift), QString("-60"));
        QCOMPARE(dlg->cutCell(1, CompareDialog::CutKind), QString("Moved"));
        QCOMPARE(dlg->cutCell(1, CompareDialog::CutOldIn), QString::fromStdString(formatTimecode(0, Rational{30, 1})));
        QCOMPARE(dlg->cutCell(2, CompareDialog::CutKind), QString("Deleted"));
        QCOMPARE(dlg->cutCell(3, CompareDialog::CutKind), QString("Extended"));
        QVERIFY(dlg->cutCell(3, CompareDialog::CutOldIn).isEmpty());
        QVERIFY(dlg->findChild<QLabel*>("cutSummary")->text().contains("1 moved"));
        dlg->activateCut(3);
        QCOMPARE(state()->playhead(), FrameTime(75));
        // Both lists.
        const QString edl = dir_.filePath("changes.edl"), csv = dir_.filePath("changes.csv");
        QVERIFY(dlg->exportChangeList(edl) && dlg->exportChangeList(csv));
        QFile e(edl);
        QVERIFY(e.open(QIODevice::ReadOnly));
        const QByteArray edlText = e.readAll();
        QVERIFY(edlText.contains("001  OLDCUT") && edlText.contains("* CHANGE: MOVED") && edlText.contains("NEWCUT"));
        QFile c(csv);
        QVERIFY(c.open(QIODevice::ReadOnly));
        QVERIFY(c.readAll().startsWith("Event,Change,Shot"));
        // The old version re-conformed: Blue then Red's first 15 frames from it, the rest of Red from Version 2
        // (labelled), a marker where the title was taken out and the new material begins; one undo step.
        QVERIFY(dlg->findChild<QPushButton*>("compareReconform")->isEnabled());
        const Id made = dlg->reconform(first);
        QVERIFY(made);
        QCOMPARE(state()->sequence()->id, made);
        const Sequence& out = *state()->sequence();
        QCOMPARE(out.name, std::string("Sequence 1 (Conformed)"));
        const auto& v = out.videoTracks[0].clips;
        QCOMPARE(v.size(), size_t(3));
        QCOMPARE(v[0].name, std::string("Blue"));
        QCOMPARE(v[1].name, std::string("Red"));
        QCOMPARE(v[1].duration, FrameTime(15));
        QCOMPARE(v[2].start, FrameTime(75));
        QVERIFY(v[2].colorLabel != 0 && v[1].colorLabel == 0);
        QVERIFY(out.videoTracks[1].clips.empty());
        QCOMPARE(out.markers.size(), size_t(1));  // the title's removal and the new material share a frame
        QVERIFY(QString::fromStdString(out.markers[0].name).startsWith("Deleted: ") &&
                QString::fromStdString(out.markers[0].name).contains(" / Extended: Red"));
        QCOMPARE(state()->project().sequences.size(), sequences + 2);
        state()->undo();
        QCOMPARE(state()->project().sequences.size(), sequences + 1);
        // Going to a change brings Version 2 back.
        state()->setActiveSequence(first);
        dlg->activateCut(0);
        QCOMPARE(state()->sequence()->id, v2.id);
        // An edit to Version 2 with the dialog open: the lists follow it (Red gone: Red and the title deleted, Blue the same).
        QVERIFY(state()->edit("Drop Red", [&](Project&, Sequence& s) {
            s.videoTracks[0].clips.pop_back();
            return true;
        }));
        QTRY_COMPARE(dlg->cutRowCount(), 3);
        QCOMPARE(dlg->cutCell(2, CompareDialog::CutKind), QString("Same"));
        dlg->close();
        state()->newProject();
    }

    void normalizeWaveformsOption() {
        // The JFK clip turned down 20 dB: drawn at a tenth normally, at its own peak's full height when normalised.
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            if (!edit::placeMedia(p, s, ids[0], 0, 0, -1, V1, {TrackKind::Audio, 0}, false).ok) return false;
            s.audioTracks[0].clips[0].audio.params["gain_db"] = Param(-20.0);
            return true;
        }));
        const Clip clip = state()->sequence()->audioTracks[0].clips.at(0);
        const std::string path = state()->project().findMedia(ids[0])->path;
        QTRY_VERIFY_WITH_TIMEOUT(MediaPool::instance().peaksIfReady(path) != nullptr, 20000);
        auto* act = win_->findChild<QAction*>("normalizeWaveforms");
        QVERIFY(act && act->isCheckable());
        if (act->isChecked()) act->trigger();
        QVERIFY(std::fabs(timeline()->waveformScale(clip) - 0.1) < 1e-9);
        act->trigger();
        QVERIFY(timeline()->normalizeWaveforms());
        QVERIFY(appSettings().value("timeline/normalizeWaveforms").toBool());
        const double scale = timeline()->waveformScale(clip);
        float peak = 0;
        for (float v : MediaPool::instance().peaksIfReady(path)->minmax) peak = std::max(peak, std::fabs(v));
        QVERIFY2(peak > 0.05f && std::fabs(scale - 1.0 / peak) < 1e-3 * scale, qPrintable(QString("%1 %2").arg(scale).arg(peak)));
        act->trigger();
        QVERIFY(!timeline()->normalizeWaveforms());
        state()->newProject();
    }

    void transitionEdgesDragTheirLength() {
        // Red (0-60) and Blue (60-120) with a 20-frame dissolve over 50-70: dragging its end out to 80 makes it 40.
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->apply("Dissolve", [red](Project& p, Sequence& s) {
            return edit::addTransition(p, s, red, edit::Edge::Out, "cross_dissolve", 20);
        }));
        const Id id = state()->sequence()->videoTracks[0].transitions.at(0).id;
        auto length = [&] { return state()->sequence()->videoTracks[0].transitions.at(0).duration; };
        drag(pointFor(70, V1), pointFor(80, V1));
        QVERIFY2(std::llabs(length() - 40) <= 2, qPrintable(QString::number(length())));
        QCOMPARE(state()->selectedTransition(), id);
        state()->undo();
        QCOMPARE(length(), FrameTime(20));
        // The start edge works the same way, from the other side (as far, so the drag clears the drag distance at any zoom).
        drag(pointFor(50, V1), pointFor(40, V1));
        QVERIFY2(std::llabs(length() - 40) <= 2, qPrintable(QString::number(length())));
        state()->undo();
    }

    void inspectorExpressions() {
        if (!expressionsAvailable()) QSKIP("Built without Lua");
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        const Id motion = clipNamed(*state()->sequence(), "Red")->motion.id;
        state()->setSelection({red}, false);
        QApplication::processEvents();
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector);
        auto button = [&] { return inspector->findChild<QToolButton*>(QStringLiteral("expression_%1_opacity").arg(motion)); };
        QTRY_VERIFY(button());
        auto opacity = [&]() -> const Param& { return edit::clipById(*state()->sequence(), red)->motion.params.at("opacity"); };
        // Typed into the dialog: checked as it is typed, then applied in one undo step.
        auto useDialog = [&](const QString& text, const char* press, QString* status = nullptr, bool* okEnabled = nullptr) {
            bool seen = false;
            QTimer::singleShot(0, this, [&, text, press] {
                auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dlg || dlg->objectName() != "expressionDialog") {  // (left unseen: the check after it fails)
                    if (dlg) dlg->reject();
                    return;
                }
                auto* edit = dlg->findChild<QPlainTextEdit*>("expressionText");
                edit->setPlainText(text);
                if (status) *status = dlg->findChild<QLabel*>("expressionStatus")->text();
                auto* box = dlg->findChild<QDialogButtonBox*>();
                if (okEnabled) *okEnabled = box->button(QDialogButtonBox::Ok)->isEnabled();
                seen = true;
                if (!std::strcmp(press, "ok")) box->button(QDialogButtonBox::Ok)->click();
                else if (!std::strcmp(press, "cancel")) box->button(QDialogButtonBox::Cancel)->click();
                else dlg->findChild<QPushButton*>(press)->click();
            });
            button()->click();
            QVERIFY(seen);
        };
        QString status;
        bool okEnabled = true;
        useDialog("value +", "cancel", &status, &okEnabled);
        QVERIFY2(status.startsWith("Does not work") && !okEnabled, qPrintable(status));
        useDialog("linear(frame, 0, 10, 0, 100)", "ok", &status, &okEnabled);
        QVERIFY2(status.startsWith("At the playhead") && okEnabled, qPrintable(status));
        QCOMPARE(opacity().expr, std::string("linear(frame, 0, 10, 0, 100)"));
        QCOMPARE(opacity().at(5), 50.0);
        QTRY_VERIFY(button()->isChecked() && !button()->property("failing").toBool());
        // Its box shows what it gives, and is not edited while it drives the setting.
        auto* box = inspector->findChild<QDoubleSpinBox*>(QStringLiteral("param_%1_opacity").arg(motion));
        QTRY_VERIFY(box && !box->isEnabled());
        // The Keyframes panel shows it as a curve.
        win_->raisePanel("keyframes");
        auto* panel = win_->findChild<KeyframePanel*>();
        QTRY_VERIFY(panel && panel->clip() == red);
        QTRY_VERIFY(std::any_of(panel->rows().begin(), panel->rows().end(), [](const auto& r) { return r.address.param == "opacity" && r.label.endsWith("="); }));
        state()->undo();
        QVERIFY(opacity().expr.empty());
        state()->redo();
        // A failing one shows on its button; the keyed value is used.
        useDialog("nil + frame", "ok");
        state()->setPlayhead(clipNamed(*state()->sequence(), "Red")->start + 3);
        QApplication::processEvents();
        QTRY_VERIFY(button()->property("failing").toBool());
        QCOMPARE(opacity().at(3), opacity().keyedAt(3));
        // Baked: keys in its place.
        useDialog("linear(frame, 0, 10, 0, 100)", "expressionBake");
        QVERIFY(opacity().expr.empty() && opacity().keys.size() == 3);
        QCOMPARE(opacity().at(5), 50.0);
        QTRY_VERIFY(!button()->isChecked());
        state()->undo();
        QCOMPARE(opacity().expr, std::string("nil + frame"));
        useDialog("", "expressionRemove");
        QVERIFY(opacity().expr.empty());
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
        QVERIFY(loudness && loudness->count() == 6);
        QCOMPARE(loudness->itemData(0).toPointF(), QPointF(0, 0));
        QCOMPARE(loudness->itemData(1).toPointF(), QPointF(-14, -1));
        QCOMPARE(loudness->itemData(3).toPointF(), QPointF(-23, -1));
        // Netflix's is measured on the dialogue.
        QCOMPARE(loudness->itemData(5).toPointF(), QPointF(-27, -2));
        QVERIFY(loudness->itemData(5, Qt::UserRole + 1).toBool() && !loudness->itemData(4, Qt::UserRole + 1).toBool());
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

    void trimPassesPanel() {
        // The demo as an HDR master: an SDR target added in the Trims panel, which the Program monitor then shows.
        const QString path = dir_.path() + "/trims-demo.montage";
        Project demo = demoProject();
        demo.active()->colorSpace = "rec2100pq";
        QVERIFY(saveProject(demo, path.toStdString()));
        QVERIFY(win_->openProject(path));
        auto* trims = win_->findChild<TrimPanel*>();
        QVERIFY(trims);
        PlaybackController* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp->controller();
        QVERIFY(program);
        QCOMPARE(trims->shown(), QString());
        QVERIFY(!win_->findChild<QDoubleSpinBox*>("trim_gain")->isEnabled());
        QVERIFY(trims->addTarget("sdr"));
        QCOMPARE(trims->shown(), QString("sdr"));
        QCOMPARE(program->trimTarget(), std::string("sdr"));

        // The shot at the playhead (Red; the title over it is not one) trimmed: one undo step for a run of changes.
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->setPlayhead(20);
        QCOMPARE(trims->shot(), red);
        auto* gain = win_->findChild<QDoubleSpinBox*>("trim_gain");
        QVERIFY(gain->isEnabled());
        gain->setValue(0.8);
        gain->setValue(0.6);
        QCOMPARE(edit::clipById(*state()->sequence(), red)->trims.at("sdr").p("gain", 0), 0.6);
        state()->undo();
        QVERIFY(edit::clipById(*state()->sequence(), red)->trims.empty());
        state()->redo();
        QCOMPARE(gain->value(), 0.6);
        // Keyframe on: the change is a key at the playhead.
        auto* key = win_->findChild<QToolButton*>("trimKeyframe");
        QVERIFY(key && !gain->keyboardTracking());
        key->setChecked(true);
        win_->findChild<QDoubleSpinBox*>("trim_lift")->setValue(0.1);
        QCOMPARE(edit::clipById(*state()->sequence(), red)->trims.at("sdr").params.at("lift").keys.size(), size_t(2));
        key->setChecked(false);
        state()->setSelection({blue}, false);
        QVERIFY(trims->copyToSelection());
        QCOMPARE(edit::clipById(*state()->sequence(), blue)->trims.at("sdr").p("gain", 0), 0.6);
        state()->setPlayhead(70);
        QCOMPARE(trims->shot(), blue);
        QVERIFY(trims->resetShot());
        QVERIFY(edit::clipById(*state()->sequence(), blue)->trims.empty());

        // Queue Passes: a render of each target, delivering it.
        RenderQueue* queue = win_->renderQueue();
        const size_t before = queue->jobs().size();
        QCOMPARE(win_->queueTrimPasses(dir_.path() + "/passes"), 1);
        QCOMPARE(queue->jobs().size(), before + 1);
        QCOMPARE(queue->jobs().back().settings.trimTarget, std::string("sdr"));
        QVERIFY(QString::fromStdString(queue->jobs().back().settings.path).endsWith(".mp4"));
        queue->remove(queue->jobs().back().id);

        // Removing the target shows the master again.
        QVERIFY(trims->removeShown());
        QCOMPARE(trims->shown(), QString());
        QCOMPARE(program->trimTarget(), std::string());
        QVERIFY(state()->sequence()->trimTargets.empty());
    }

    void linedScriptPanel() {
        // Two takes of a speech (the second a copy of the first file), each read by its transcript.
        state()->newProject();
        const QString jfk = QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav"), copy = dir_.path() + "/take2.wav";
        QFile::remove(copy);
        QVERIFY(QFile::copy(jfk, copy));
        const auto ids = state()->importFiles({jfk, copy});
        QCOMPARE(ids.size(), size_t(2));
        auto said = [](double at, const QString& text) {
            auto t = std::make_shared<Transcript>();
            TranscriptSegment seg;
            seg.start = at;
            seg.text = text.toStdString();
            for (const QString& w : text.split(' ')) {
                seg.words.push_back({at, at + 0.3, w.toStdString(), -1, {}});
                at += 0.4;
            }
            seg.end = at;
            t->segments.push_back(seg);
            return t;
        };
        QVERIFY(state()->edit("Transcribe", [&](Project& p, Sequence&) {
            p.findMedia(ids[0])->transcript = said(1.0, "ask not what your country can do for you");
            p.findMedia(ids[1])->transcript = said(2.0, "ask not what your country can do for you ask what you can do for your country");
            return true;
        }));
        win_->findChild<QAction*>("showLinedScript")->trigger();
        auto* panel = win_->findChild<LinedScriptPanel*>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(panel->setScript("ask.fountain", "Title: Ask\n\nJFK\nAsk not what your country can do for you.\n\n"
                                                 "JFK\nAsk what you can do for your country.\n\nJFK\nA line nobody says.\n"));
        const ProjectScript& script = state()->project().script;
        QCOMPARE(script.format, std::string("fountain"));
        QCOMPARE(script.readings.size(), size_t(3));
        QCOMPARE(script.readings[0].size(), size_t(2));
        QVERIFY(script.readings[1].size() >= 2);  // (line one says most of its words too, in another order)
        QCOMPARE(script.readings[1][0].media, ids[1]);
        QVERIFY(script.readings[2].empty());
        QCOMPARE(panel->layout().pages, 1);
        QVERIFY(win_->findChild<QLabel*>("linedScriptSummary")->text().contains("2 of 3"));
        // A click on a take's line opens that take in the Source monitor, its reading of the line marked.
        const LinedScriptLayout::Segment* seg = nullptr;
        for (const auto& sg : panel->layout().segments)
            if (sg.line == 1 && sg.reading == 0) seg = &sg;
        QVERIFY(seg);
        QWidget* pages = panel->pagesWidget();
        const double scale = (pages->width() - 20.0) / LinedScriptLayout::kPageWidth;
        const QPoint at(int(10 + panel->layout().columns[size_t(seg->column)].x * scale), int(10 + (seg->y0 + seg->y1) / 2 * scale));
        QTest::mouseClick(pages, Qt::LeftButton, {}, at);
        const ScriptReading& r = script.readings[1][0];
        QCOMPARE(r.media, ids[1]);
        QCOMPARE(state()->sourceMedia(), ids[1]);
        const double fps = state()->sequence()->fpsValue();
        QCOMPARE(state()->sourceIn(), FrameTime(std::floor(r.start * fps)));
        QCOMPARE(state()->sourceOut(), FrameTime(std::ceil(r.end * fps)) - 1);  // (Out is the last frame played)
        QCOMPARE(panel->selectedLine(), 1);
        // A line with no take: nothing opens. Line one's other take, by its reading.
        QVERIFY(!panel->cueLine(2));
        QVERIFY(panel->cueLine(0, 1));
        QCOMPARE(state()->sourceMedia(), script.readings[0][1].media);
        // Exported as a PDF; Set Script was one undo step.
        QVERIFY(panel->exportPdf(dir_.path() + "/ask.pdf"));
        QVERIFY(QFileInfo(dir_.path() + "/ask.pdf").size() > 1000);
        state()->undo();
        QVERIFY(state()->project().script.text.empty());
        QTRY_COMPARE(panel->layout().pages, 0);
        state()->newProject();
    }

    void galleryAndClipStrip() {
        // The demo (Red, then Blue on V1; a title over Red on V2), saved: its gallery beside it.
        const QString path = dir_.path() + "/gallery-demo.montage";
        QVERIFY(saveProject(demoProject(), path.toStdString()));
        QVERIFY(win_->openProject(path));
        auto* gallery = win_->findChild<GalleryPanel*>();
        QVERIFY(gallery);
        QCOMPARE(gallery->folder(), dir_.path() + "/Gallery/gallery-demo");
        const Id red = clipNamed(*state()->sequence(), "Red")->id, blue = clipNamed(*state()->sequence(), "Blue")->id;
        state()->edit("Grade", [&](Project& p, Sequence& s) {
            edit::clipById(s, red)->effects.push_back(makeEffect(p, "invert"));
            return true;
        });
        QVERIFY(win_->findChild<QAction*>("grabStillAction"));

        // Grab Still at frame 5 (Red alone), then Wipe: the Program monitor compares with it until Wipe is off.
        state()->setPlayhead(5);
        QVERIFY(gallery->grab());
        auto* stills = win_->findChild<QListWidget*>("galleryStills");
        QCOMPARE(stills->count(), 1);
        QVERIFY(!gallery->selectedStill().isEmpty());
        MonitorPanel* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp;
        QVERIFY(program);
        QVERIFY(gallery->wipe(true));
        QVERIFY(program->viewer()->comparing());
        QVERIFY(win_->findChild<QPushButton*>("wipeStill")->isChecked());
        gallery->wipe(false);
        QVERIFY(!program->viewer()->comparing());

        // Apply Grade to the selected clip: Blue gets the invert, one undo step.
        state()->setSelection({blue}, false);
        QVERIFY(gallery->applyToSelection());
        auto inverted = [&] {
            const Clip* c = edit::clipById(*state()->sequence(), blue);
            return std::any_of(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "invert"; });
        };
        QVERIFY(inverted());
        state()->undo();
        QVERIFY(!inverted());
        QVERIFY(gallery->addAlbum("Looks"));
        QCOMPARE(gallery->album(), QString("Looks"));
        QCOMPARE(stills->count(), 0);
        gallery->setAlbum("Stills");
        QCOMPARE(stills->count(), 1);
        gallery->selectStill(stills->item(0)->data(Qt::UserRole).toString());
        QVERIFY(gallery->removeSelected());
        QCOMPARE(stills->count(), 0);

        // The clip strip: every picture clip in timeline order, graded thumbnails, the one under the playhead marked.
        auto* strip = win_->findChild<ClipStripPanel*>();
        QVERIFY(strip);
        strip->rebuild();
        const std::vector<Id> listed = strip->clips();
        QCOMPARE(listed.size(), size_t(3));
        QCOMPARE(listed.front(), red);
        QCOMPARE(listed.back(), blue);
        auto* list = win_->findChild<QListWidget*>("clipStrip");
        QTRY_COMPARE_WITH_TIMEOUT(strip->pending(), 0, 20000);
        QVERIFY(!list->item(0)->icon().isNull());
        state()->setPlayhead(20);  // the title, over Red
        QCOMPARE(list->currentRow(), 1);
        emit list->itemClicked(list->item(2));
        QCOMPARE(state()->selectedClips(), std::vector<Id>{blue});
        QCOMPARE(state()->playhead(), FrameTime(60));
        QCOMPARE(list->currentRow(), 2);
        strip->setLightbox(true);
        QVERIFY(strip->lightbox() && list->isWrapping() && list->iconSize().width() > 200);
        strip->setLightbox(false);
        QVERIFY(!list->isWrapping());

        // A still grabbed before the project's first save goes with it when it is saved.
        QStandardPaths::setTestModeEnabled(true);
        state()->newProject();
        QCOMPARE(gallery->folder(), QString::fromStdString(galleryFolder("")));
        state()->edit("Clip", [](Project& p, Sequence& s) { return edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 30)).ok; });
        state()->setPlayhead(5);
        QVERIFY(gallery->grab());
        const QString saved = dir_.path() + "/gallery-later.montage";
        QVERIFY(state()->save(saved));
        QCOMPARE(gallery->folder(), dir_.path() + "/Gallery/gallery-later");
        QCOMPARE(stills->count(), 1);
        QVERIFY(galleryStills(galleryFolder(""), kDefaultAlbum).empty());
        QDir(QString::fromStdString(galleryFolder(""))).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }

    void ografGraphicInInspector() {
        // Add OGraf Graphic: the graphic at the playhead with its schema's defaults, its fields in the Inspector.
        state()->newProject();
        state()->edit("Format", [](Project&, Sequence& s) {
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            return true;
        });
        qputenv("MONTAGE_OGRAF_CACHE", (dir_.path() + "/ograf-cache").toUtf8());
        struct Unset {
            ~Unset() { qunsetenv("MONTAGE_OGRAF_CACHE"); }
        } unset;
        QCOMPARE(win_->addOGrafGraphic(dir_.path() + "/none.ograf.json"), Id(0));
        const Id clip = win_->addOGrafGraphic(QStringLiteral(MONTAGE_TEST_DATA_DIR "/ograf/lower-third/lower.ograf.json"));
        QVERIFY(clip);
        // (A second long, not played out: each value tried renders it.)
        state()->edit("Shorter", [&](Project&, Sequence& s) {
            edit::clipById(s, clip)->duration = 25;
            edit::clipById(s, clip)->generator.params["out"] = Param(0.0);
            return true;
        });
        auto data = [&] {
            const Clip* c = edit::clipById(*state()->sequence(), clip);
            return QJsonDocument::fromJson(QByteArray::fromStdString(c->generator.s("data"))).object();
        };
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->name, std::string("Lower Third"));
        QCOMPARE(data().value("name").toString(), QString("Ada Lovelace"));
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();
        auto visible = [&]<typename W>(const char* name, W*) {
            W* found = nullptr;
            for (auto* w : win_->findChildren<W*>(name))
                if (w->isVisibleTo(win_.get())) found = w;
            return found;
        };
        auto* name = visible("ograf_name", static_cast<QLineEdit*>(nullptr));
        auto* colour = visible("ograf_color", static_cast<QComboBox*>(nullptr));
        auto* size = visible("ograf_size", static_cast<QDoubleSpinBox*>(nullptr));
        auto* count = visible("ograf_count", static_cast<QSpinBox*>(nullptr));
        auto* show = visible("ograf_show", static_cast<QCheckBox*>(nullptr));
        auto* extra = visible("ograf_extra", static_cast<QLineEdit*>(nullptr));
        QVERIFY(name && colour && size && count && show && extra && visible("ograf_role", static_cast<QPlainTextEdit*>(nullptr)));
        QCOMPARE(name->text(), QString("Ada Lovelace"));
        QCOMPARE(size->value(), 40.0);
        QVERIFY(show->isChecked());
        QCOMPARE(extra->text(), QString(R"~({"a":1})~"));
        // Edited: each field written into the data (one undo step a field).
        name->setText("Grace Hopper");
        emit name->editingFinished();
        colour->setCurrentIndex(1);
        show->setChecked(false);
        extra->setText("[1, 2]");
        emit extra->editingFinished();
        QCOMPARE(data().value("name").toString(), QString("Grace Hopper"));
        QCOMPARE(data().value("color").toString(), QString("rgb(0, 255, 0)"));
        QCOMPARE(data().value("show").toBool(true), false);
        QCOMPARE(data().value("extra").toArray().size(), 2);
        state()->undo();
        QCOMPARE(data().value("extra").toObject().value("a").toInt(), 1);
        QApplication::processEvents();
        QCOMPARE(extra->text(), QString(R"~({"a":1})~"));

        // Render: its frames now; the viewer draws them (the bar green, in all the way 0.8 s in).
        if (ografBrowser().empty()) QSKIP("No Chrome, Chromium or Edge to render OGraf graphics in (set MONTAGE_CHROMIUM)");
        const Sequence& sq = *state()->sequence();
        const Clip* c = edit::clipById(sq, clip);
        const OGrafJob job = ografClipJob(c->generator, sq.width, sq.height, sq.fpsValue(), c->duration);
        auto* render = visible("renderOGraf", static_cast<QPushButton*>(nullptr));
        QVERIFY(render);
        render->click();
        QTRY_VERIFY_WITH_TIMEOUT(ografCached(job), 120000);
        state()->setPlayhead(20);
        MonitorPanel* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp;
        QVERIFY(program);
        auto greenAt = [&](double x, double y) {
            const QImage img = program->viewer()->image();
            if (img.isNull()) return false;
            const QColor px = img.pixelColor(int(x * img.width()), int(y * img.height()));
            return px.green() > 200 && px.red() < 60;
        };
        QTRY_VERIFY_WITH_TIMEOUT(greenAt(150.0 / 320, 120.0 / 180), 30000);
    }

    void cameraTrackerInInspectorAndViewer() {
        // A clip with a solve (made here: a camera 1.6 above a ground of points, moving right while it looks along -z).
        QImage tex(320, 180, QImage::Format_RGB32);
        tex.fill(QColor(60, 90, 120));
        const QString png = dir_.path() + "/flat.png";
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
        Clip gc = makeClip(gen, m, TrackKind::Video, gs);
        gc.duration = 20;
        edit::overwrite(gen, gs, {TrackKind::Video, 0}, gc);
        ExportSettings st;
        st.path = (dir_.path() + "/flat.mp4").toStdString();
        st.audioCodec = "none";
        st.preset = "ultrafast";
        QVERIFY2(exportSequence(gen, gs, st, nullptr, nullptr, &err), err.c_str());
        state()->newProject();
        state()->edit("Format", [](Project&, Sequence& s) {
            s.width = 320;
            s.height = 180;
            s.fps = {25, 1};
            return true;
        });
        auto ids = state()->importFiles({QString::fromStdString(st.path)});
        QCOMPARE(ids.size(), size_t(1));
        state()->apply("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids[0], 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        });
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
        CameraSolve cs;
        cs.width = 320;
        cs.height = 180;
        cs.focal = 300;
        cs.fps = 25;
        cs.oriented = true;
        cs.keyframes = 3;
        for (int i = 0; i < 20; ++i) {
            SolvedCamera c;
            c.valid = true;
            c.R = {1, 0, 0, 0, -1, 0, 0, 0, -1};
            c.C = {0.02 * i, 1.6, 0};
            cs.cameras.push_back(c);
        }
        for (int i = 0; i < 40; ++i) {
            SolvedPoint sp;
            sp.X = {-2 + 0.1 * i, 0, -3 - 0.15 * i};
            sp.error = 0.4;
            sp.seen = 20;
            sp.last = 19;
            cs.points.push_back(sp);
        }
        Id eid = 0;
        state()->edit("Tracker", [&](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "camera_track");
            e.strings["solve"] = cameraSolveToString(cs);
            eid = e.id;
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        state()->setSelection({clip}, false);
        state()->setPlayhead(10);
        win_->findChild<QDockWidget*>("inspector")->show();
        win_->findChild<QDockWidget*>("inspector")->raise();
        QApplication::processEvents();

        // The Inspector says what was solved and offers Solve, Place Title and Export.
        QLabel* status = nullptr;
        for (auto* l : win_->findChildren<QLabel*>("cameraTrackStatus"))
            if (l->isVisibleTo(win_.get())) status = l;
        QVERIFY(status);
        QVERIFY2(status->text().contains("Solved") && status->text().contains("40 point"), qPrintable(status->text()));
        for (const char* name : {"solveCamera", "placeSceneTitle", "exportCamera"}) {
            QPushButton* b = nullptr;
            for (auto* x : win_->findChildren<QPushButton*>(name))
                if (x->isVisibleTo(win_.get())) b = x;
            QVERIFY2(b && b->isEnabled(), name);
        }

        // The viewer shows the points tracked in the frame on screen (green: they fit), until Show Tracked Points is off.
        MonitorPanel* program = nullptr;
        for (auto* mp : win_->findChildren<MonitorPanel*>())
            if (mp->mode() == MonitorPanel::Mode::Program) program = mp;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        auto* overlay = viewer->findChild<MaskOverlay*>();
        QVERIFY(overlay);
        auto greens = [&] {
            QApplication::processEvents();
            const QImage img = viewer->grab().toImage();
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.green() > 180 && c.red() < 140 && c.blue() < 160) ++n;
                }
            return n;
        };
        QVERIFY(greens() > 20);
        state()->edit("Hide", [&](Project&, Sequence& s) {
            edit::ownedEffect(s, clip, eid)->params["show_points"] = Param(0.0);
            return true;
        });
        QCOMPARE(greens(), 0);

        // Place Title: a title over the clip for its length, on the ground where the picture's (50 %, 70 %) is at the
        // playhead, its Corner Pin keyed through the clip.
        auto* inspector = win_->findChild<InspectorWidget*>();
        const Id title = inspector->placeTitleInScene(clip, eid, "Hello");
        QVERIFY(title);
        const Sequence& sq = *state()->sequence();
        QCOMPARE(edit::locate(sq, title)->track.index, 1);
        const Clip* t = edit::clipById(sq, title);
        QCOMPARE(t->duration, FrameTime(20));
        const Effect* pin = nullptr;
        for (const Effect& e : t->effects)
            if (e.type == "corner_pin") pin = &e;
        QVERIFY(pin && pin->params.at("tl_x").keys.size() == 20);
        // The ground point under (50 %, 70 %) at frame 10 is in the middle of the title's bottom half there.
        const TrackQuad q = cornerPinQuad(*pin, 10);
        const double midX = (q.p[0].x + q.p[1].x + q.p[2].x + q.p[3].x) / 4, midY = (q.p[0].y + q.p[1].y + q.p[2].y + q.p[3].y) / 4;
        QVERIFY2(std::fabs(midX - 0.5) < 0.02 && std::fabs(midY - 0.7) < 0.03, qPrintable(QString("%1 %2").arg(midX).arg(midY)));
        QVERIFY(q.p[0].y < q.p[3].y && q.p[0].x < q.p[1].x);  // its top edge farther away, upright on screen
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), title));
        // Pointing at the sky: nothing placed.
        state()->edit("Sky", [&](Project&, Sequence& s) {
            edit::ownedEffect(s, clip, eid)->params["place_y"] = Param(10.0);
            return true;
        });
        QCOMPARE(inspector->placeTitleInScene(clip, eid, "Up"), Id(0));

        // Solving a flat picture finds nothing to follow: the solve kept is the one it had.
        inspector->solveCameraTrack(clip, eid);
        CameraSolve kept;
        QVERIFY(clipCameraSolve(*edit::clipById(*state()->sequence(), clip), kept));
        QCOMPARE(kept.points.size(), size_t(40));
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
        // This footage has no camera motion data: Camera Gyro is not offered, and the method stays as it was.
        QComboBox* method = nullptr;
        for (auto* b : win_->findChildren<QComboBox*>("choice_method"))
            if (b->isVisibleTo(win_.get())) method = b;
        QVERIFY(method && method->count() == 4);
        auto* methods = qobject_cast<QStandardItemModel*>(method->model());
        QVERIFY(methods && !methods->item(3)->isEnabled() && methods->item(2)->isEnabled());
        QCOMPARE(method->currentIndex(), 2);
        QVERIFY(effectOf("stabilize")->s("gyro").empty());
        QPushButton* again = nullptr;
        for (auto* b : win_->findChildren<QPushButton*>("analyzeStabilize"))
            if (b->isVisibleTo(win_.get())) again = b;
        QVERIFY(again);

        // Sky Replacement on footage measures the camera's movement when added too, for its sky to follow.
        add = nullptr;
        for (auto* b : win_->findChildren<QPushButton*>())
            if (b->text().startsWith("Add Video Effect") && b->isVisibleTo(win_.get())) add = b;
        QVERIFY(add);
        QAction* skyAction = nullptr;
        std::function<void(QMenu*)> findSky = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                if (a->menu()) findSky(a->menu());
                else if (a->text() == "Sky Replacement") skyAction = a;
            }
        };
        findSky(add->menu());
        QVERIFY(skyAction);
        skyAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(effectOf("sky_replace") && !effectOf("sky_replace")->s("motion").empty(), 30000);
        QApplication::processEvents();
        int analyzers = 0;
        for (auto* b : win_->findChildren<QPushButton*>("analyzeStabilize")) analyzers += b->isVisibleTo(win_.get());
        QCOMPARE(analyzers, 2);
        state()->edit("No Sky", [clip](Project&, Sequence& s) {
            auto& fx = edit::clipById(s, clip)->effects;
            fx.erase(std::remove_if(fx.begin(), fx.end(), [](const Effect& e) { return e.type == "sky_replace"; }), fx.end());
            return true;
        });

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
        // What counts as a filler: the transcript's language, the discourse option, the project's own words.
        QVERIFY(panel->fillerOptions().language.empty());
        QVERIFY(state()->edit("Language", [media](Project& p, Sequence&) {
            auto lt = std::make_shared<Transcript>(*p.findMedia(media)->transcript);
            lt->language = "en";
            p.findMedia(media)->transcript = lt;
            return true;
        }));
        QApplication::processEvents();
        QCOMPARE(panel->fillerOptions().language, std::string("en"));
        QVERIFY(panel->findChild<QAction*>("discourseFillers") && panel->findChild<QAction*>("customFillers"));
        panel->setDiscourseFillers(true);
        QVERIFY(appSettings().value("transcript/discourseFillers").toBool());
        QVERIFY(panel->fillerOptions().discourse && panel->findChild<QAction*>("discourseFillers")->isChecked());
        panel->setDiscourseFillers(false);
        QVERIFY(state()->edit("Fillers", [](Project& p, Sequence&) {
            p.fillerWords = {"ask not"};
            return true;
        }));
        QApplication::processEvents();
        QCOMPARE(panel->fillerOptions().custom, std::vector<std::string>{"ask not"});
        const std::vector<bool> mask = fillerWordMask(panel->words(), panel->fillerOptions());
        QVERIFY(mask[2] && mask[6] && mask[7] && !mask[0] && !mask[1]);
        state()->undo();
        state()->undo();
        QApplication::processEvents();
        QVERIFY(panel->fillerOptions().custom.empty());
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
        QVERIFY(appSettings().value("transcript/smoothCuts").toBool());
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

    void inspectorFitsASideDock() {
        // The Colour workspace's Inspector is a side dock: a clip's rows must fit 380 px without sideways scrolling.
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        // With effects whose sections are busiest: colour wheels, a mask with tracking, Stabilize, the Colour Warper.
        QVERIFY(state()->edit("Effects", [red](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, red);
            c->effects.push_back(makeEffect(p, "color_correct"));
            Effect blur = makeEffect(p, "gaussian_blur");
            blur.params["mask.shape"] = Param(1.0);
            c->effects.push_back(blur);
            c->effects.push_back(makeEffect(p, "stabilize"));
            c->effects.push_back(makeEffect(p, "color_warper"));
            return true;
        }));
        state()->setSelection({red}, false);
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector && inspector->widget());
        QApplication::processEvents();
        QString widest;
        int most = 0;
        for (QFormLayout* form : inspector->widget()->findChildren<QFormLayout*>())
            for (int r = 0; r < form->rowCount(); ++r) {
                QLayoutItem* label = form->itemAt(r, QFormLayout::LabelRole);
                QLayoutItem* field = form->itemAt(r, QFormLayout::FieldRole);
                if (!field) field = form->itemAt(r, QFormLayout::SpanningRole);
                if (!field) continue;
                const int w = (label ? label->minimumSize().width() + form->horizontalSpacing() : 0) + field->minimumSize().width();
                if (w > most) {
                    most = w;
                    auto* l = label ? qobject_cast<QLabel*>(label->widget()) : nullptr;
                    widest = l ? l->text() : QStringLiteral("row %1").arg(r);
                }
            }
        QVERIFY2(most <= 340, qPrintable(QString("%1: %2 px").arg(widest).arg(most)));
        state()->setSelection({}, false);
    }

    void colourWarperInInspector() {
        loadDemo();
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        Id fx = 0;
        QVERIFY(state()->edit("Warper", [&](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "color_warper");
            fx = e.id;
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        state()->setSelection({red}, false);
        auto* editor = win_->findChild<ColorWarperEditor*>("warp_mesh");
        QTRY_VERIFY(editor && editor->isVisible());
        editor->resize(240, 240);
        auto mesh = [&] { return edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), red, fx)->s("mesh"); };
        // Drag red's full-saturation point to where orange (30 degrees) is: one undo step.
        const QPoint from = editor->pointPos(0, 4).toPoint(), to = editor->colourPos(30.0 / 360, 1).toPoint();
        QTest::mousePress(editor, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i) {
            const QPoint at = from + (to - from) * i / 4;
            QMouseEvent move(QEvent::MouseMove, QPointF(at), editor->mapToGlobal(QPointF(at)), Qt::NoButton, Qt::LeftButton, {});
            QApplication::sendEvent(editor, &move);
        }
        QTest::mouseRelease(editor, Qt::LeftButton, {}, to);
        ColorWarp w;
        QVERIFY(parseColorWarp(mesh(), w));
        QVERIFY2(std::fabs(w.at(0, 4).dh - 30) < 2.5 && std::fabs(w.at(0, 4).ds) < 0.03, mesh().c_str());
        QCOMPARE(editor->selectedSpoke(), 0);
        // The selected point's brightness from the spin box.
        auto* luma = win_->findChild<QDoubleSpinBox*>("warpLuma");
        QVERIFY(luma && luma->isEnabled());
        luma->setValue(-0.5);
        QVERIFY(parseColorWarp(mesh(), w) && std::fabs(w.at(0, 4).dl + 0.5) < 1e-9);
        // The picture changes: Red's red turns towards orange.
        const Image frame = renderSequenceFrame(state()->project(), *state()->sequence(), 10, {});
        QVERIFY(frame.at(frame.width / 2, frame.height / 2)[1] > 0.05f);
        // Undone (with the drag when it came straight after it, as curve edits run together).
        state()->undo();
        QVERIFY(parseColorWarp(mesh(), w) && w.at(0, 4).dl == 0);
        QVERIFY(state()->edit("Mesh", [&](Project&, Sequence& s) {
            edit::ownedEffect(s, red, fx)->strings["mesh"] = "0,4,30,0,0";
            return true;
        }));
        // Double-clicking puts a point back; Reset All clears the mesh.
        QTRY_VERIFY(std::fabs(editor->warp().at(0, 4).dh - 30) < 1e-9);
        QTest::mouseDClick(editor, Qt::LeftButton, {}, editor->pointPos(0, 4).toPoint());
        QVERIFY(mesh().empty());  // the moved point, not spoke 1's that it now covers
        state()->undo();
        QVERIFY(!mesh().empty());
        auto* resetAll = win_->findChild<QToolButton*>("warpResetAll");
        QVERIFY(resetAll);
        resetAll->click();
        QVERIFY(mesh().empty());
        state()->setSelection({}, false);
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

    void pickFocusInProgramMonitor() {
        loadDemo();
        // Lens Blur (Depth) on the red clip: Pick Focus, then a click in the Program monitor focuses on that point.
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        Id lensId = 0;
        QVERIFY(state()->edit("Lens", [red, &lensId](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "depth_blur");
            lensId = e.id;
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        state()->setSelection({red}, false);
        state()->setPlayhead(10);
        const int64_t lt = 10 - clipNamed(*state()->sequence(), "Red")->start;
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        QPushButton* pick = nullptr;
        QTRY_VERIFY((pick = win_->findChild<QPushButton*>(QStringLiteral("pickFocus"))) != nullptr);
        pick->click();
        QCOMPARE(state()->paintEffect(), lensId);
        const QRectF r = viewer->imageRect();
        auto at = [&r](double u, double v) { return QPointF(r.left() + u * r.width(), r.top() + v * r.height()).toPoint(); };
        auto fx = [&]() -> const Effect& { return *edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), red, lensId); };
        const double px = 2.0 / r.width(), py = 2.0 / r.height();
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.3, 0.4));
        QCOMPARE(std::lround(fx().p("follow", lt)), 1L);
        QVERIFY(std::fabs(fx().p("point_x", lt) - 0.3) < px && std::fabs(fx().p("point_y", lt) - 0.4) < py);
        QVERIFY(!fx().params.at("point_x").animated());
        // Keyed: each click keys both, eased, so two frames make a rack.
        QVERIFY(state()->edit("Key", [red, lensId](Project&, Sequence& s) {
            Effect* e = edit::ownedEffect(s, red, lensId);
            e->params["point_x"].addKey(0, 0.7, Interp::Smooth);
            return true;
        }));
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.2, 0.6));
        const Keyframe* kx = fx().params.at("point_x").keyAt(lt);
        const Keyframe* ky = fx().params.at("point_y").keyAt(lt);
        QVERIFY(kx && ky);
        QVERIFY(std::fabs(kx->v - 0.2) < px && std::fabs(ky->v - 0.6) < py && kx->interp == Interp::Smooth);
        QVERIFY(std::fabs(fx().p("point_x", 0) - 0.7) < 1e-9);  // the earlier key stays
        state()->undo();
        QVERIFY(!fx().params.at("point_x").keyAt(lt));
        // Esc stops picking; a click then leaves it alone.
        QTest::keyClick(viewer, Qt::Key_Escape);
        QCOMPARE(state()->paintEffect(), Id(0));
        QTRY_VERIFY((pick = win_->findChild<QPushButton*>(QStringLiteral("pickFocus"))) && !pick->isChecked());
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.6, 0.6));
        QVERIFY(!fx().params.at("point_x").keyAt(lt));
        state()->setSelection({}, false);
    }

    void restorationPaintInProgramMonitor() {
        loadDemo();
        // A Paint effect on the red clip, armed from the Inspector, painted on the Program monitor.
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        Id paintId = 0;
        QVERIFY(state()->edit("Paint", [red, &paintId](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "paint");
            paintId = e.id;
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        state()->setSelection({red}, false);
        state()->setPlayhead(10);
        const int64_t lt = 10 - clipNamed(*state()->sequence(), "Red")->start;
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        QVERIFY(viewer->findChild<PaintOverlay*>());
        QPushButton* arm = nullptr;
        QTRY_VERIFY((arm = win_->findChild<QPushButton*>(QStringLiteral("paintOnMonitor"))) != nullptr);
        arm->click();
        QCOMPARE(state()->paintEffect(), paintId);
        const QRectF r = viewer->imageRect();
        auto at = [&r](double u, double v) { return QPointF(r.left() + u * r.width(), r.top() + v * r.height()).toPoint(); };
        auto fx = [&]() -> const Effect& { return *edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), red, paintId); };

        // Alt-click picks where to copy from; a drag paints one stroke, one undo step.
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::AltModifier, at(0.2, 0.5));
        QVERIFY(fx().s("strokes").empty());
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, at(0.5, 0.5));
        for (double u = 0.52; u <= 0.6001; u += 0.02) {
            const QPointF to(at(u, 0.5));
            QMouseEvent move(QEvent::MouseMove, to, viewer->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer, &move);
        }
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, at(0.6, 0.5));
        std::vector<PaintStroke> strokes = paintStrokesFromString(fx().s("strokes"));
        QCOMPARE(strokes.size(), size_t(1));
        const PaintStroke& st = strokes[0];
        QCOMPARE(st.frame, lt);
        QVERIFY(!st.heal && !st.hold);
        QVERIFY2(st.points.size() >= 3, qPrintable(QString::number(st.points.size())));
        const double px = 1.0 / r.width();
        QVERIFY(std::fabs(st.points.front()[0] - 0.5) < 2 * px && std::fabs(st.points.back()[0] - 0.6) < 2 * px);
        QVERIFY2(std::fabs(st.dx + 0.3) < 2 * px && std::fabs(st.dy) < 2 * px, qPrintable(QString("%1 %2").arg(st.dx).arg(st.dy)));
        QVERIFY(std::fabs(fx().p("source_x", lt) - st.dx) < 1e-5);  // later strokes keep the offset
        QVERIFY(st.radius > 0.005 && st.radius < 0.1);
        state()->undo();
        QVERIFY(paintStrokesFromString(fx().s("strokes")).empty());
        state()->redo();
        QCOMPARE(paintStrokesFromString(fx().s("strokes")).size(), size_t(1));
        // A second stroke without Alt keeps the offset; Clear This Frame takes both away.
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, at(0.5, 0.7));
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, at(0.55, 0.7));
        strokes = paintStrokesFromString(fx().s("strokes"));
        QCOMPARE(strokes.size(), size_t(2));
        QVERIFY(std::fabs(strokes[1].dx - strokes[0].dx) < 1e-5);
        QPushButton* clearFrame = nullptr;
        QTRY_VERIFY((clearFrame = win_->findChild<QPushButton*>(QStringLiteral("paintClearFrame"))) != nullptr);
        clearFrame->click();
        QVERIFY(paintStrokesFromString(fx().s("strokes")).empty());
        // Esc stops painting; the Inspector's button follows.
        QTest::keyClick(viewer, Qt::Key_Escape);
        QCOMPARE(state()->paintEffect(), Id(0));
        QTRY_VERIFY((arm = win_->findChild<QPushButton*>(QStringLiteral("paintOnMonitor"))) && !arm->isChecked());
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.5, 0.5));
        QVERIFY(paintStrokesFromString(fx().s("strokes")).empty());

        // Dust Spot Removal: not on a generator (spots are taken out where a file is read)...
        Id dustId = 0;
        QVERIFY(state()->edit("Dust", [red, &dustId](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "dust_spots");
            dustId = e.id;
            edit::clipById(s, red)->effects.push_back(e);
            return true;
        }));
        QTRY_VERIFY(win_->findChildren<QPushButton*>(QStringLiteral("paintOnMonitor")).size() == 2);
        QVERIFY(!win_->findChildren<QPushButton*>(QStringLiteral("paintOnMonitor")).back()->isEnabled());
        state()->setPaintEffect(dustId);
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.4, 0.3));
        QVERIFY(edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), red, dustId)->s("spots").empty());
        state()->setPaintEffect(0);

        // ...but on a picture: a click marks a spot on this frame, Ctrl-click removes it.
        const QString png = dir_.filePath("dusty.png");
        {
            QImage q(320, 180, QImage::Format_RGB32);
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x) q.setPixel(x, y, qRgb(x * 255 / 319, y * 255 / 179, 90));
            QVERIFY(q.save(png));
        }
        state()->newProject();
        const auto ids = state()->importFiles({png});
        QCOMPARE(ids.size(), size_t(1));
        const Id media = ids[0];
        QVERIFY(state()->apply("Place", [media](Project& p, Sequence& s) { return edit::placeMedia(p, s, media, 0, 0, 30, V1, A1, false); }));
        const Id still = trackAt(*state()->sequence(), V1)->clips.at(0).id;
        QVERIFY(state()->edit("Dust", [still, &dustId](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "dust_spots");
            dustId = e.id;
            edit::clipById(s, still)->effects.push_back(e);
            return true;
        }));
        state()->setSelection({still}, false);
        state()->setPlayhead(10);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        QTRY_VERIFY((arm = win_->findChild<QPushButton*>(QStringLiteral("paintOnMonitor"))) && arm->isEnabled());
        arm->click();
        QCOMPARE(state()->paintEffect(), dustId);
        const QRectF rs = viewer->imageRect();
        auto spotAt = [&rs](double u, double v) { return QPointF(rs.left() + u * rs.width(), rs.top() + v * rs.height()).toPoint(); };
        const double spx = 1.0 / rs.width();
        auto dust = [&] { return dustSpotsFromString(edit::ownedEffect(const_cast<Sequence&>(*state()->sequence()), still, dustId)->s("spots")); };
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, spotAt(0.4, 0.3));
        QCOMPARE(dust().size(), size_t(1));
        QCOMPARE(dust()[0].frame, int64_t(10));
        QVERIFY(std::fabs(dust()[0].x - 0.4) < 2 * spx && std::fabs(dust()[0].y - 0.3) < 2.0 / rs.height());
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, spotAt(0.7, 0.6));
        QCOMPARE(dust().size(), size_t(2));
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::ControlModifier, spotAt(0.7, 0.6));
        QCOMPARE(dust().size(), size_t(1));
        QVERIFY(std::fabs(dust()[0].x - 0.4) < 2 * spx);
        // A spot only on another frame is left alone by a Ctrl-click there.
        state()->setPlayhead(11);
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::ControlModifier, spotAt(0.4, 0.3));
        QCOMPARE(dust().size(), size_t(1));
        state()->setPaintEffect(0);
        state()->setSelection({}, false);
    }

    void drawnMaskInProgramMonitor() {
        loadDemo();
        // A blur limited to a Bezier mask on the red clip, drawn in the viewer.
        const Id red = clipNamed(*state()->sequence(), "Red")->id;
        QVERIFY(state()->edit("Mask", [red](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "gaussian_blur");
            e.params["mask.shape"] = 5.0;
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
        QCOMPARE(overlay->shapes().size(), size_t(1));
        QVERIFY(overlay->shapes()[0].drawing());
        const QRectF r = viewer->imageRect();
        auto at = [&r](double u, double v) { return QPointF(r.left() + u * r.width(), r.top() + v * r.height()).toPoint(); };
        auto fx = [&]() -> const Effect& { return edit::clipById(*state()->sequence(), red)->effects.back(); };
        const double W = state()->sequence()->width, H = state()->sequence()->height;
        // Where point i is, as a fraction of the frame.
        auto where = [&](int i) {
            const auto pts = maskPath(fx(), 10);
            double u = -1, v = -1;
            if (i < int(pts.size())) boxToFrame(maskBox(fx(), 10), W, H, pts[size_t(i)].x, pts[size_t(i)].y, u, v);
            return QPointF(u, v);
        };
        auto close = [&r](QPointF a, double u, double v) { return std::fabs(a.x() - u) * r.width() < 2 && std::fabs(a.y() - v) * r.height() < 2; };
        auto drag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QTest::mousePress(viewer, Qt::LeftButton, mods, from);
            QMouseEvent move(QEvent::MouseMove, QPointF(to), viewer->mapToGlobal(QPointF(to)), Qt::NoButton, Qt::LeftButton, mods);
            QApplication::sendEvent(viewer, &move);
            QTest::mouseRelease(viewer, Qt::LeftButton, mods, to);
        };

        // Three clicks place corners; clicking the first point closes the path.
        for (QPoint p : {at(0.3, 0.3), at(0.7, 0.3), at(0.5, 0.7)}) QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, p);
        QCOMPARE(maskPathCount(fx()), 3);
        QVERIFY(fx().p("mask.open", 10) > 0.5);
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.3, 0.3));
        QVERIFY(!fx().params.count("mask.open"));
        QVERIFY(!overlay->shapes()[0].drawing());
        // The box now frames the path, and the points are where they were clicked.
        const MaskBox b = maskBox(fx(), 10);
        QVERIFY2(std::fabs(b.x - 0.5) < 0.01 && std::fabs(b.y - 0.5) < 0.01 && std::fabs(b.w - 0.4) < 0.01 && std::fabs(b.h - 0.4) < 0.01,
                 qPrintable(QString("%1 %2 %3 %4").arg(b.x).arg(b.y).arg(b.w).arg(b.h)));
        QVERIFY(close(where(0), 0.3, 0.3) && close(where(1), 0.7, 0.3) && close(where(2), 0.5, 0.7));
        // The mask limits the blur: rendered, the effect has a matte inside the triangle only.
        const std::vector<float> m = effectMatte(fx(), 10, Image(64, 36), 0.05);
        QVERIFY(m[size_t(14 * 64 + 32)] > 0.9f && m[size_t(2 * 64 + 2)] < 0.01f);

        // Dragging a point moves it (one undo step).
        drag(at(0.5, 0.7), at(0.5, 0.8));
        QVERIFY(close(where(2), 0.5, 0.8));
        state()->undo();
        QVERIFY(close(where(2), 0.5, 0.7));
        // Clicking the outline adds a point there; Ctrl-click removes it.
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(0.5, 0.3));
        QCOMPARE(maskPathCount(fx()), 4);
        QVERIFY(close(where(1), 0.5, 0.3));
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::ControlModifier, at(0.5, 0.3));
        QCOMPARE(maskPathCount(fx()), 3);
        // Alt-click makes a point smooth, and back.
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::AltModifier, at(0.5, 0.7));
        QVERIFY(maskPath(fx(), 10)[2].smooth());
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::AltModifier, at(0.5, 0.7));
        QVERIFY(!maskPath(fx(), 10)[2].smooth());
        // Dragging inside moves the whole path.
        drag(at(0.5, 0.42), at(0.6, 0.42));
        QVERIFY2(close(where(0), 0.4, 0.3) && close(where(2), 0.6, 0.7), qPrintable(QString("%1 %2").arg(where(0).x()).arg(where(0).y())));
        state()->undo();
        // The handle above the box turns it: dragged level with the centre to its right, a quarter turn.
        QPointF rot, ctr;
        QVERIFY(overlay->rotateHandle(overlay->shapes()[0], rot) && overlay->boxToWidget(overlay->shapes()[0], 0, 0, ctr));
        QVERIFY(rot.y() < ctr.y());
        drag(rot.toPoint(), (ctr + QPointF(r.width() * 0.3, 0)).toPoint());
        QVERIFY2(std::fabs(fx().p("mask.rotation", 10) - 90) < 2, qPrintable(QString::number(fx().p("mask.rotation", 10))));
        state()->undo();

        // Animate Path in the Inspector keys the whole path; the keyframe panel shows it as one row.
        QCheckBox* animate = nullptr;
        QTRY_VERIFY((animate = win_->findChild<QCheckBox*>("animateMaskPath")) != nullptr);
        QVERIFY(!animate->isChecked());
        animate->click();
        QVERIFY(maskPathAnimated(fx()));
        QCOMPARE(maskPathKeyTimes(fx()), (std::vector<FrameTime>{10}));
        state()->setPlayhead(30);
        drag(at(0.5, 0.7), at(0.5, 0.8));
        QCOMPARE(maskPathKeyTimes(fx()), (std::vector<FrameTime>{10, 30}));
        QVERIFY(std::fabs(maskPath(fx(), 20)[2].y - (maskPath(fx(), 10)[2].y + maskPath(fx(), 30)[2].y) / 2) < 1e-9);
        auto* panel = win_->findChild<KeyframePanel*>();
        QVERIFY(panel);
        int pathRows = 0;
        for (const auto& row : panel->rows()) {
            QVERIFY(!isMaskPathParam(row.address.param) || row.address.param == kMaskPathParam);
            if (row.address.param == kMaskPathParam) {
                ++pathRows;
                QCOMPARE(row.label.section(QStringLiteral(" · "), 1), QStringLiteral("Mask Path"));
                // Moving its key moves every coordinate's.
                panel->select({{row.address, 30}});
                QVERIFY(panel->shiftSelected(5));
            }
        }
        QCOMPARE(pathRows, 1);
        QCOMPARE(maskPathKeyTimes(fx()), (std::vector<FrameTime>{10, 35}));
        for (const std::string& n : maskPathParams(fx())) QVERIFY(fx().params.at(n).keyAt(35));
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

    void readTextInPictures() {
        if (!ocrAvailable() || !ocrModel().installed()) QSKIP("Text reading model not installed (set MONTAGE_OCR_MODEL)");
        // A video with two subtitles burned in, and a slate card.
        auto video = [&](const std::vector<std::pair<FrameTime, std::string>>& titles, int anchor, const QString& file) {
            Project gen = makeDefaultProject();
            Sequence& gs = *gen.active();
            gs.width = 640, gs.height = 360, gs.fps = {25, 1};
            Clip bg = makeGeneratorClip(gen, "color", 100);
            edit::overwrite(gen, gs, {TrackKind::Video, 0}, bg);
            for (const auto& [at, text] : titles) {
                Clip t = makeGeneratorClip(gen, "title", 40);
                t.generator.strings["text"] = text;
                t.generator.params["size"] = 32.0;
                t.generator.params["anchor"] = double(anchor);
                t.start = at;
                edit::overwrite(gen, gs, {TrackKind::Video, 1}, t);
            }
            ExportSettings st;
            st.path = (dir_.path() + "/" + file).toStdString();
            st.audioCodec = "none";
            st.preset = "ultrafast";
            std::string err;
            return exportSequence(gen, gs, st, nullptr, nullptr, &err) ? QString::fromStdString(st.path) : QString();
        };
        const QString subs = video({{0, "Where were you last night"}, {50, "I was at home, honestly"}}, 2, "subs.mp4");
        const QString slate = video({{0, "SCENE 4\nTAKE 6"}}, 0, "slate4.mp4");
        QVERIFY(!subs.isEmpty() && !slate.isEmpty());
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("readBurnedInSubtitles"));
        QCOMPARE(win_->readBurnedInSubtitles(0, "en", false), 0);  // nothing selected or under the playhead
        const auto ids = state()->importFiles({subs, slate});
        QCOMPARE(ids.size(), size_t(2));
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            return edit::placeMedia(p, s, ids.at(0), 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok;
        }));
        state()->setSelection({state()->sequence()->videoTracks[0].clips.at(0).id}, false);
        QCOMPARE(win_->readBurnedInSubtitles(0, "en", false), 2);
        const CaptionTrack& t = state()->sequence()->captionTracks.back();
        QVERIFY(QString::fromStdString(t.name).startsWith("Burned-In"));
        QVERIFY2(readingSimilarity(t.captions[0].text, "Where were you last night") >= 0.9, t.captions[0].text.c_str());
        QVERIFY(readingSimilarity(t.captions[1].text, "I was at home, honestly") >= 0.9);
        state()->undo();
        QVERIFY(state()->sequence()->captionTracks.empty());
        // The slate logged from the picture, in the media bin.
        auto* bin = win_->findChild<MediaBinWidget*>();
        QCOMPARE(bin->logSlatesFromPicture({ids.at(1), ids.at(0)}), 1);
        QCOMPARE(mediaFieldText(*state()->project().findMedia(ids.at(1)), "scene"), std::string("4"));
        QCOMPARE(mediaFieldText(*state()->project().findMedia(ids.at(1)), "take"), std::string("6"));
        state()->undo();
        QCOMPARE(mediaFieldText(*state()->project().findMedia(ids.at(1)), "scene"), std::string());
        state()->newProject();
    }

    void findWhatIsSaid() {
        if (!speechSearchAvailable() || !sentenceModel().installed()) QSKIP("Speech search model not installed (set MONTAGE_SENTENCE_MODEL)");
        state()->newProject();
        QVERIFY(state()->edit("Recordings", [](Project& p, Sequence&) {
            MediaItem m;
            m.id = p.newId();
            m.name = "interview.wav";
            m.kind = MediaKind::Audio;
            m.hasAudio = true;
            auto t = std::make_shared<Transcript>();
            t->language = "en";
            double at = 0;
            for (const char* sentence : {"We ran out of money halfway through the shoot.", "The cat sat on the windowsill all afternoon."}) {
                TranscriptSegment seg;
                for (const QString& w : QString(sentence).split(' ')) {
                    seg.words.push_back({at, at + 0.25, w.toStdString(), 1});
                    at += 0.3;
                }
                t->segments.push_back(seg);
                at += 1;
            }
            m.transcript = t;
            p.media.push_back(m);
            return true;
        }));
        auto* panel = win_->findChild<ShotSearchPanel*>();
        QVERIFY(panel);
        auto* mode = panel->findChild<QComboBox*>("findMode");
        QVERIFY(mode);
        mode->setCurrentIndex(1);
        QCOMPARE(panel->mode(), ShotSearchPanel::Said);
        QVERIFY(panel->findChild<QLabel*>("shotStatus")->text().contains("1 of 1"));
        QVERIFY(panel->findChild<QPushButton*>("indexShots")->isHidden());
        QVERIFY(panel->search("financial trouble") >= 1);
        QVERIFY(panel->resultText(0).contains("money"));
        auto* list = panel->findChild<QListWidget*>("shotResults");
        QVERIFY(list->item(0)->text().contains("interview.wav") && list->item(0)->text().contains("money"));
        // Opening it cues the Source monitor on the moment; a subclip can be made of it.
        QSignalSpy opened(panel, &ShotSearchPanel::openRequested);
        panel->open(0);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(1).value<FrameTime>(), FrameTime(0));
        // Back to what's shown: the results go.
        mode->setCurrentIndex(0);
        QCOMPARE(list->count(), 0);
        QVERIFY(panel->resultText(0).isEmpty());
        state()->newProject();
    }

    void graphicsTemplatesInTheApp() {
        state()->newProject();
        qputenv("MONTAGE_TEMPLATES_DIR", (dir_.path() + "/templates").toUtf8());
        for (const char* name : {"insertGraphicsTemplate", "publishPicture", "fillGraphics", "saveGraphicsTemplate", "graphicFields"})
            QVERIFY2(win_->findChild<QAction*>(name), name);
        // A lower third in a sequence of its own: a title.
        const Id main = state()->sequence()->id;
        Id gid = 0, titleId = 0;
        QVERIFY(state()->edit("Make", [&](Project& p, Sequence& outer) {
            Sequence g;
            g.id = gid = p.newId();
            g.name = "Lower Third";
            g.width = outer.width, g.height = outer.height, g.fps = outer.fps;
            g.videoTracks.resize(1);
            g.videoTracks[0].id = p.newId();
            Clip t;
            t.id = titleId = p.newId();
            t.duration = 60;
            t.generator = makeEffect(p, "title");
            g.videoTracks[0].clips.push_back(t);
            p.sequences.push_back(g);
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Sequence;
            m.sequenceId = gid;
            m.name = "Lower Third";
            m.hasVideo = true;
            p.media.push_back(m);
            return true;
        }));
        state()->setActiveSequence(gid);
        // Its text and colour published from the Inspector.
        auto* inspector = win_->findChild<InspectorWidget*>();
        QVERIFY(inspector);
        QCOMPARE(inspector->publishField(titleId, 0, "text", "Name"), QString());
        QCOMPARE(inspector->publishField(titleId, 0, "color", "Colour"), QString());
        QVERIFY(!inspector->publishField(titleId, 0, "text", "Again").isEmpty());
        QCOMPARE(state()->sequence()->fields.size(), size_t(2));
        state()->undo();
        QCOMPARE(state()->sequence()->fields.size(), size_t(1));
        state()->redo();
        // The fields dialog: renamed, moved, kept.
        {
            GraphicFieldsDialog dlg(state(), win_.get());
            QCOMPARE(dlg.findChild<QListWidget*>("fields")->count(), 2);
            QVERIFY(dlg.rename(0, "Person"));
            QVERIFY(dlg.move(1, -1));
            QCOMPARE(state()->sequence()->fields[0].label, std::string("Colour"));
            QCOMPARE(state()->sequence()->fields[1].label, std::string("Person"));
            QVERIFY(!dlg.move(0, -1));
        }
        // Saved to the library, then inserted into the main sequence from it.
        const QString saved = saveSequenceAsGraphicsTemplate(state(), win_.get(), "Name Strap");
        QVERIFY(!saved.isEmpty() && QFile::exists(saved));
        state()->setActiveSequence(main);
        Id clip = 0;
        {
            GraphicsTemplatesDialog dlg(state(), win_.get());
            QCOMPARE(dlg.templates().size(), size_t(1));
            QCOMPARE(dlg.findChild<QListWidget*>("templates")->count(), 1);
            clip = dlg.insert(0);
        }
        QVERIFY(clip);
        const Clip* c = edit::clipById(*state()->sequence(), clip);
        QVERIFY(c && graphicOf(state()->project(), *c));
        // Selected, the Inspector shows its fields: typing in one sets the graphic's own value.
        state()->setSelection({clip});
        QApplication::processEvents();
        auto* name = inspector->findChild<QPlainTextEdit*>("field_name");  // (by id: renaming changes its label only)
        QVERIFY(name && inspector->findChild<QToolButton*>("field_colour"));
        QCOMPARE(name->toPlainText(), QStringLiteral("Title"));
        name->setPlainText(QStringLiteral("Jane Smith"));
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->fields.at("name"), std::string("Jane Smith"));
        // Filled from a table: two more after it, one undo step.
        QFile csv(dir_.filePath("names.csv"));
        QVERIFY(csv.open(QIODevice::WriteOnly));
        csv.write("Person,Colour\nAda,#00ff00\nGrace,#0000ff\nLinus,#ff00ff\n");
        csv.close();
        const int undoBefore = int(state()->history().undoCount());
        const std::vector<Id> made = fillGraphicsFromFile(state(), clip, dir_.filePath("names.csv"));
        QCOMPARE(made.size(), size_t(3));
        QCOMPARE(int(state()->history().undoCount()), undoBefore + 1);
        QCOMPARE(edit::clipById(*state()->sequence(), made[2])->fields.at("name"), std::string("Linus"));
        state()->undo();
        QVERIFY(!edit::clipById(*state()->sequence(), made[2]));
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->fields.at("name"), std::string("Jane Smith"));
        // The colour picker: Cancel leaves the graphic as it was (no value of its own, no undo step); OK is one step.
        state()->setSelection({clip});
        QApplication::processEvents();
        auto pickColour = [&](bool accept) {
            QTimer::singleShot(0, [accept] {
                auto* dlg = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                dlg->setCurrentColor(Qt::green);
                accept ? dlg->accept() : dlg->reject();
            });
            inspector->findChild<QToolButton*>("field_colour")->click();
        };
        int steps = int(state()->history().undoCount());
        pickColour(false);
        QVERIFY(!edit::clipById(*state()->sequence(), clip)->fields.count("colour"));
        QCOMPARE(int(state()->history().undoCount()), steps);
        pickColour(true);
        QCOMPARE(edit::clipById(*state()->sequence(), clip)->fields.at("colour"), std::string("#00ff00"));
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        // A field published while an instance is selected shows up in its Graphic section.
        const Id inserted = graphicOf(state()->project(), *edit::clipById(*state()->sequence(), clip))->id;  // (the library's copy)
        QVERIFY(state()->edit("Publish", [&](Project& p, Sequence&) {
            Sequence* g = p.findSequence(inserted);
            const Id title = g->videoTracks[0].clips[0].id;
            return publishField(*g, *fieldFor(*g, title, 0, "size", "Size")).ok;
        }));
        QApplication::processEvents();
        QVERIFY(inspector->findChild<QWidget*>("field_size"));
        // In the graphic itself, a clip's own effect can be published from its settings' menus.
        Id gradeId = 0;
        QVERIFY(state()->edit("Grade", [&](Project& p, Sequence&) {
            Clip* t = edit::clipById(*p.findSequence(gid), titleId);
            t->effects.push_back(makeEffect(p, "color_correct"));
            gradeId = t->effects.back().id;
            return true;
        }));
        state()->setActiveSequence(gid);
        state()->setSelection({titleId});
        QApplication::processEvents();
        auto* sat = inspector->findChild<QDoubleSpinBox*>(QStringLiteral("param_%1_saturation").arg(gradeId));
        QVERIFY(sat);
        QStringList offered;
        QTimer::singleShot(0, [&offered] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : m->actions()) offered << a->text();
                m->close();
            }
        });
        emit sat->customContextMenuRequested(QPoint(2, 2));
        QVERIFY2(offered.contains(QStringLiteral("Publish to Graphic…")), qPrintable(offered.join(", ")));
        state()->setActiveSequence(main);
        qunsetenv("MONTAGE_TEMPLATES_DIR");
        state()->newProject();
    }

    void growingMedia() {
        state()->newProject();
        // A recording still being written (a WAV whose header does not yet give its length, as recorders write them).
        const QString wav = dir_.filePath("growing.wav");
        auto append = [&](double seconds) {
            QFile f(wav);
            QVERIFY(f.open(QIODevice::Append));
            f.write(QByteArray(int(48000 * seconds) * 2, '\0'));
        };
        {
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QDataStream d(&f);
            d.setByteOrder(QDataStream::LittleEndian);
            f.write("RIFF");
            d << quint32(0xFFFFFFFF);
            f.write("WAVEfmt ");
            d << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
            f.write("data");
            d << quint32(0xFFFFFFFF);
        }
        append(1.0);
        MediaItem m;
        QVERIFY(probeMedia(wav.toStdString(), m));
        Id id = 0;
        QVERIFY(state()->edit("Import", [&](Project& p, Sequence&) {
            m.id = id = p.newId();
            p.media.push_back(m);
            return true;
        }));
        auto length = [&] { return state()->project().findMedia(id)->duration; };
        QVERIFY(std::fabs(length() - 1.0) < 0.01);
        // Its sound, as playback has it.
        const std::string file = wav.toStdString();
        QVERIFY(MediaPool::instance().audio(file, 48000));
        auto heard = [&] {
            AudioBufferPtr a = MediaPool::instance().audioIfReady(file, 48000);
            return a ? double(a->frames()) / 48000 : -1.0;
        };
        // Time is the test's (put back however the test ends): written every 0.8 s for over two seconds, it counts as
        // growing and its length is read (on a worker).
        qint64 now = 1000000;
        state()->setClock([&now] { return now; });
        struct Restore {
            EditorState* s;
            ~Restore() {
                s->setClock({});
                s->setGrowingRefreshMs(15000);
            }
        } restore{state()};
        state()->setGrowingRefreshMs(5000);
        QSignalSpy refreshed(state(), &EditorState::growingRefreshed);
        const int steps = int(state()->history().undoCount());
        const uint64_t revision = state()->history().revision();
        for (int i = 0; i < 4; ++i) {
            append(0.5);
            state()->noteMediaFileWritten(wav);
            now += 800;
            state()->checkGrowingMedia();
            QCOMPARE(state()->isMediaGrowing(id), i == 3);  // (written for 2.4 s by the fourth write)
        }
        QTRY_COMPARE(refreshed.size(), 1);
        QVERIFY2(std::fabs(length() - 3.0) < 0.01, qPrintable(QString::number(length())));
        // The sound stays playable while it grows, then holds the new part too.
        QVERIFY(heard() > 0.99);
        QTRY_VERIFY2(std::fabs(heard() - 3.0) < 0.01, qPrintable(QString::number(heard())));
        // A fact about the file, not an edit: no undo step, but the project is changed (saved, the length is kept).
        QCOMPARE(int(state()->history().undoCount()), steps);
        QVERIFY(state()->history().revision() > revision && state()->isModified());
        auto* bin = win_->findChild<MediaBinWidget*>();
        bin->showBin(QString());
        QApplication::processEvents();
        const QModelIndex row = bin->model()->index(bin->model()->rowOf(id), 0);
        QVERIFY(row.data(Qt::ToolTipRole).toString().contains("Growing"));
        QCOMPARE(state()->growingFiles(), QStringList{wav});
        // Still growing: read again every five seconds, not only once it is quiet.
        for (int i = 0; i < 7; ++i) {
            append(0.25);
            state()->noteMediaFileWritten(wav);
            now += 800;
            state()->checkGrowingMedia();
        }
        QVERIFY(state()->isMediaGrowing(id));
        QTRY_COMPARE(refreshed.size(), 2);
        QVERIFY2(std::fabs(length() - 4.75) < 0.01, qPrintable(QString::number(length())));
        QCOMPARE(int(state()->history().undoCount()), steps);
        // Undo after the length was read keeps it (every state undo can bring back has it).
        QVERIFY(state()->edit("Marker", [](Project&, Sequence& s) {
            s.markers.push_back(Marker{});
            return true;
        }));
        state()->undo();
        QVERIFY2(std::fabs(length() - 4.75) < 0.01, qPrintable(QString::number(length())));
        state()->redo();
        QVERIFY(std::fabs(length() - 4.75) < 0.01);
        state()->undo();
        // Stopped: reloaded in full, no longer growing.
        now += 3500;
        state()->checkGrowingMedia();
        QVERIFY(!state()->isMediaGrowing(id));
        QVERIFY(state()->growingFiles().isEmpty());
        QVERIFY2(std::fabs(length() - 4.75) < 0.01, qPrintable(QString::number(length())));
        // A save in a few writes over a second is not growing (it is reloaded once quiet, as before).
        state()->noteMediaFileWritten(wav);
        now += 500;
        state()->noteMediaFileWritten(wav);
        now += 500;
        state()->noteMediaFileWritten(wav);
        now += 1000;
        state()->checkGrowingMedia();
        QVERIFY(!state()->isMediaGrowing(id));
        now += 600;
        state()->checkGrowingMedia();
        QVERIFY(!state()->isMediaGrowing(id));
        // "Only when they stop": nothing read while it grows.
        state()->setGrowingRefreshMs(0);
        refreshed.clear();
        for (int i = 0; i < 4; ++i) {
            append(0.25);
            state()->noteMediaFileWritten(wav);
            now += 800;
            state()->checkGrowingMedia();
        }
        QVERIFY(state()->isMediaGrowing(id));
        QTest::qWait(100);
        QCOMPARE(refreshed.size(), 0);
        QVERIFY(std::fabs(length() - 4.75) < 0.01);
        // Another project open while it grows: no longer followed.
        state()->newProject();
        QVERIFY(state()->growingFiles().isEmpty());
        now += 3500;
        state()->checkGrowingMedia();
        QVERIFY(state()->growingFiles().isEmpty());
    }

    void storyboardBin() {
        state()->newProject();
        auto* bin = win_->findChild<MediaBinWidget*>();
        QVERIFY(bin && win_->findChild<QToolButton*>("boardViewButton"));
        std::vector<Id> ids;
        QVERIFY(state()->edit("Media", [&](Project& p, Sequence& s) {
            for (const char* name : {"one", "two", "three"}) {
                MediaItem m;
                m.id = p.newId();
                m.name = name;
                m.kind = MediaKind::Video;
                m.hasVideo = m.hasAudio = true;
                m.path = (dir_.path() + "/missing-" + name + ".mov").toStdString();
                m.duration = 1;
                m.fps = s.fps;
                m.width = s.width, m.height = s.height;
                p.media.push_back(m);
                ids.push_back(m.id);
            }
            return true;
        }));
        bin->showBin(QString());
        bin->setView(MediaBinWidget::View::Board);
        win_->show();
        QApplication::processEvents();
        StoryboardView* board = bin->board();
        QCOMPARE(board->shownInOrder(), ids);  // nothing placed yet: as listed
        auto indexOf = [&](Id id) {
            for (int r = 0; r < board->model()->rowCount(); ++r)
                if (Id(board->model()->index(r, 0).data(MediaBinModel::IdRole).toULongLong()) == id) return board->model()->index(r, 0);
            return QModelIndex();
        };
        // Moved about (three first, one last): saved as one undo step, in cells; the one not moved pinned where it was.
        const QSize cell = board->cellSize();
        board->moveThumbnail(indexOf(ids[0]), QPoint(2 * cell.width(), 0));
        board->moveThumbnail(indexOf(ids[2]), QPoint(0, 0));
        const int steps = int(state()->history().undoCount());
        QVERIFY(board->saveBoard("Arrange Storyboard", {ids[0], ids[2]}));
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        QVERIFY(!board->saveBoard("Arrange Storyboard", {ids[0], ids[2]}));  // nothing moved since
        QCOMPARE(state()->project().findMedia(ids[0])->board, (MediaItem::BoardSpot{2, 0, true}));
        QCOMPARE(state()->project().findMedia(ids[1])->board, (MediaItem::BoardSpot{1, 0, true}));  // (where it showed)
        QCOMPARE(board->shownInOrder(), (std::vector<Id>{ids[2], ids[1], ids[0]}));
        // A real drag on the board: pressed on "two", dropped a cell and a quarter lower; only it moves, one undo step.
        board->selectionModel()->clearSelection();
        const QPoint press = board->visualRect(indexOf(ids[1])).center();
        QTest::mousePress(board->viewport(), Qt::LeftButton, {}, press);
        QCOMPARE(bin->selectedMedia(), std::vector<Id>{ids[1]});
        std::unique_ptr<QMimeData> drag(board->dragData());
        QVERIFY(drag);
        const QPoint drop = press + QPoint(0, cell.height() + cell.height() / 4);
        QDragEnterEvent enter(press, Qt::CopyAction | Qt::MoveAction, drag.get(), Qt::LeftButton, {});
        QApplication::sendEvent(board->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), Qt::MoveAction);
        QDropEvent dropped(drop, Qt::CopyAction | Qt::MoveAction, drag.get(), Qt::LeftButton, {});
        QApplication::sendEvent(board->viewport(), &dropped);
        QTest::mouseRelease(board->viewport(), Qt::LeftButton, {}, drop);
        QVERIFY(dropped.isAccepted());
        QCOMPARE(int(state()->history().undoCount()), steps + 2);
        QCOMPARE(state()->project().findMedia(ids[1])->board, (MediaItem::BoardSpot{1, 1.25, true}));
        QCOMPARE(state()->project().findMedia(ids[0])->board, (MediaItem::BoardSpot{2, 0, true}));
        // Undone: the thumbnail goes back (the bin shows the same media, so only the board's refresh moves it).
        state()->undo();
        QApplication::processEvents();
        QCOMPARE(board->thumbnailAt(indexOf(ids[1])), QPoint(cell.width(), 0));
        // Kept where they are when the bin is rebuilt, and at twice the size, twice as far apart.
        bin->rebuild();
        QApplication::processEvents();
        QCOMPARE(board->thumbnailAt(indexOf(ids[0])), QPoint(2 * cell.width(), 0));
        const int width = board->thumbnailWidth();
        board->setThumbnailWidth(width * 2);
        QCOMPARE(board->thumbnailAt(indexOf(ids[0])).x(), 2 * board->cellSize().width());
        board->setThumbnailWidth(width);
        // Dragged out in storyboard order, whatever order they were picked in.
        bin->selectMedia({ids[0], ids[2]});
        std::unique_ptr<QMimeData> mime(board->dragData());
        QCOMPARE(QString::fromUtf8(mime->data("application/x-montage-media")), QStringLiteral("%1,%2").arg(ids[2]).arg(ids[0]));
        // Assembled at the playhead on the targeted tracks, in that order, one undo step.
        bin->selectMedia({});
        state()->setPlayhead(12);
        const std::vector<Id> made = bin->assembleStoryboard(false);
        QCOMPARE(made.size(), size_t(6));
        const Track& v1 = state()->sequence()->videoTracks[size_t(state()->targetVideoTrack())];
        QCOMPARE(v1.clips.size(), size_t(3));
        QCOMPARE(v1.clips[0].start, FrameTime(12));
        QCOMPARE(v1.clips[0].mediaId, ids[2]);
        QCOMPARE(v1.clips[1].mediaId, ids[1]);
        QCOMPARE(v1.clips[2].mediaId, ids[0]);
        state()->undo();
        QVERIFY(state()->sequence()->videoTracks[0].clips.empty());
        // Only the selection, in board order.
        bin->selectMedia({ids[0], ids[1]});
        QCOMPARE(bin->assembleStoryboard(false).size(), size_t(4));
        QCOMPARE(state()->sequence()->videoTracks[0].clips[0].mediaId, ids[1]);
        state()->undo();
        // Arrange in Grid: rows as wide as the view, in storyboard order, one undo step, shown at once.
        board->moveThumbnail(indexOf(ids[0]), QPoint(5 * cell.width(), 3 * cell.height()));
        QVERIFY(board->saveBoard("Arrange Storyboard", {ids[0]}));
        QVERIFY(bin->arrangeBoard());
        QCOMPARE(state()->project().findMedia(ids[0])->board, (MediaItem::BoardSpot{2, 0, true}));
        QCOMPARE(state()->project().findMedia(ids[2])->board, (MediaItem::BoardSpot{0, 0, true}));
        QCOMPARE(board->thumbnailAt(indexOf(ids[1])), QPoint(cell.width(), 0));
        QCOMPARE(board->shownInOrder(), (std::vector<Id>{ids[2], ids[1], ids[0]}));
        // A search shows media from other bins too: nothing is arranged or saved then.
        auto* search = bin->findChild<QLineEdit*>();
        search->setText("t");  // two, three
        QApplication::processEvents();
        QVERIFY(!board->arrangeable() && !bin->arrangeBoard());
        QVERIFY(!board->saveBoard("Arrange Storyboard", {ids[1]}));
        search->clear();
        QApplication::processEvents();
        QVERIFY(board->arrangeable());
        // A new sequence (its item in this bin) is not assembled into itself.
        state()->newSequence("Second", 1920, 1080, state()->sequence()->fps);
        bin->rebuild();
        QApplication::processEvents();
        bin->selectMedia({});
        QCOMPARE(bin->assembleStoryboard(false).size(), size_t(6));
        for (const Track& t : state()->sequence()->videoTracks)
            for (const Clip& c : t.clips) QVERIFY(state()->project().findMedia(c.mediaId)->kind != MediaKind::Sequence);
        bin->setView(MediaBinWidget::View::Icons);
        state()->newProject();
    }

    void scriptPanelsInTheApp() {
        if (!scriptingAvailable()) QSKIP("built without Lua");
        state()->newProject();
        appSettings().remove("scripts/panels");
        // A script with its own panel: run once, it opens the panel; its button runs it again with what the panel holds.
        const QString path = QFileInfo(dir_.filePath("marker-maker.lua")).absoluteFilePath();
        auto write = [&](const QByteArray& code) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(code);
        };
        const QByteArray code = R"lua(
local ui = montage.panel{title = "Marker Maker", controls = {
  {type = "text", id = "name", label = "Name", value = "Cue"},
  {type = "number", id = "at", label = "At (s)", value = 1, min = 0, max = 600},
  {type = "check", id = "chapter", label = "Chapter"},
  {type = "label", id = "status", text = ""},
  {type = "button", id = "add", label = "Add Marker"},
  {type = "button", id = "fail", label = "Fail"},
}}
if ui.pressed == "fail" then error("on purpose") end
montage.add_marker{at = ui.at, name = ui.name, chapter = ui.chapter}
montage.add_marker{at = ui.at + 1, name = ui.name .. " end"}
montage.set_panel{status = "Added " .. ui.name, at = ui.at + 2.5}
print("added " .. ui.name)
)lua";
        write(code);
        ScriptConsole* console = win_->scriptConsole();
        const int steps = int(state()->history().undoCount());
        QVERIFY(console->runFile(path));
        ScriptPanelWidget* panel = win_->scriptPanel(path);
        QVERIFY(panel);
        auto* dock = qobject_cast<QDockWidget*>(panel->parentWidget());
        QVERIFY(dock && dock->windowTitle() == "Marker Maker" && dock->isVisible());
        QCOMPARE(int(state()->history().undoCount()), steps);  // (nothing ran past montage.panel)
        QVERIFY(state()->sequence()->markers.empty());
        QVERIFY(appSettings().value("scripts/panels").toStringList().contains(path));
        QVERIFY(console->output().contains("Panel \"Marker Maker\" open"));
        // Filled in and pressed: two markers as one undo step, the panel showing what the script set and printed.
        qobject_cast<QLineEdit*>(panel->control("name"))->setText("Intro");
        qobject_cast<QDoubleSpinBox*>(panel->control("at"))->setValue(2);
        qobject_cast<QCheckBox*>(panel->control("chapter"))->setChecked(true);
        qobject_cast<QPushButton*>(panel->control("add"))->click();
        const Sequence* s = state()->sequence();
        QCOMPARE(s->markers.size(), size_t(2));
        QCOMPARE(QString::fromStdString(s->markers[0].name), QString("Intro"));
        QCOMPARE(s->markers[0].t, FrameTime(2 * s->fps.num / s->fps.den));
        QVERIFY(s->markers[0].chapter);
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        QCOMPARE(QString::fromStdString(state()->history().undoLabel()), QString("Script: marker-maker.lua"));
        QCOMPARE(qobject_cast<QLabel*>(panel->control("status"))->text(), QString("Added Intro"));
        QCOMPARE(qobject_cast<QDoubleSpinBox*>(panel->control("at"))->value(), 4.5);  // (more decimals than its step: kept)
        QCOMPARE(panel->findChild<QLabel*>("panelMessage")->text(), QString("added Intro"));
        QVERIFY(panel->control("add")->isEnabled());
        state()->undo();
        QVERIFY(state()->sequence()->markers.empty());
        // An error shows on the panel, and nothing is changed.
        qobject_cast<QPushButton*>(panel->control("fail"))->click();
        QVERIFY(panel->findChild<QLabel*>("panelMessage")->text().contains("on purpose"));
        QVERIFY(state()->sequence()->markers.empty());
        // The script edited on disk runs as edited (its panel described again when it is run again).
        write(QByteArray(code).replace("Marker Maker", "Marker Maker 2"));
        QVERIFY(console->runFile(path));
        QCOMPARE(win_->scriptPanel(path), qobject_cast<ScriptPanelWidget*>(dock->widget()));
        QCOMPARE(dock->windowTitle(), QString("Marker Maker 2"));
        // Edited to have another control, then pressed on the old panel: the new control's default arrives (not
        // nothing), what the old panel held carries over, and the panel is rebuilt to match.
        write(QByteArray(code).replace("Marker Maker", "Marker Maker 3")
                  .replace("{type = \"label\", id = \"status\", text = \"\"},", "{type = \"label\", id = \"status\", text = \"\"},\n  {type = \"check\", id = \"dry\", label = \"Dry run\", value = true},")
                  .replace("if ui.pressed == \"fail\"", "print('dry ' .. tostring(ui.dry))\nif ui.dry then return end\nif ui.pressed == \"fail\""));
        qobject_cast<QLineEdit*>(win_->scriptPanel(path)->control("name"))->setText("Kept");
        qobject_cast<QPushButton*>(win_->scriptPanel(path)->control("add"))->click();
        QVERIFY(console->output().contains("dry true"));
        QVERIFY(state()->sequence()->markers.empty());  // (a dry run)
        QTRY_COMPARE(dock->windowTitle(), QString("Marker Maker 3"));
        QVERIFY(win_->scriptPanel(path)->control("dry"));
        QCOMPARE(qobject_cast<QLineEdit*>(win_->scriptPanel(path)->control("name"))->text(), QString("Kept"));
        write(QByteArray(code).replace("Marker Maker", "Marker Maker 2"));
        QVERIFY(console->runFile(path));
        // Closed by its button: gone and not opened again; opened again at start-up while listed, running no tools
        // before its panel (a script that calls one first is refused).
        dock->close();
        QTRY_VERIFY(!win_->scriptPanel(path));
        QVERIFY(!appSettings().value("scripts/panels").toStringList().contains(path));
        appSettings().setValue("scripts/panels", QStringList{path});
        win_->restoreScriptPanels();
        QVERIFY(win_->scriptPanel(path));
        QVERIFY(state()->sequence()->markers.empty());
        write("montage.add_marker{at = 1, name = 'too soon'}\n" + code);
        QPointer<QDockWidget> old = qobject_cast<QDockWidget*>(win_->scriptPanel(path)->parentWidget());
        old->close();
        QTRY_VERIFY(old.isNull());
        appSettings().setValue("scripts/panels", QStringList{path});
        win_->restoreScriptPanels();
        QVERIFY(!win_->scriptPanel(path));
        QVERIFY(state()->sequence()->markers.empty());
        QVERIFY(console->output().contains("runs no tools"));
        // ...and, having described no panel, it is forgotten rather than run at every start.
        QVERIFY(!appSettings().value("scripts/panels").toStringList().contains(path));
        // Opening by itself, a script runs no programs, writes no files and is ended after five seconds.
        const QString trap = dir_.filePath("written-at-start.txt");
        write(QByteArray("local f = io.open([[") + trap.toUtf8() + "]], 'w')\n" + code);
        appSettings().setValue("scripts/panels", QStringList{path});
        win_->restoreScriptPanels();
        QVERIFY(!QFileInfo::exists(trap) && !win_->scriptPanel(path));
        QVERIFY(console->output().contains("only reading while a panel opens"));
        write("while true do end\n" + code);
        appSettings().setValue("scripts/panels", QStringList{path});
        QElapsedTimer spent;
        spent.start();
        win_->restoreScriptPanels();
        QVERIFY(spent.elapsed() < 15000 && !win_->scriptPanel(path) && !console->running());
        QVERIFY(!appSettings().value("scripts/panels").toStringList().contains(path));
        // Its dock closed while its button's run is going: the run ends cleanly (an error that is not text named after
        // the script, which the gone panel no longer holds).
        write(QByteArray(code).replace("if ui.pressed == \"fail\" then error(\"on purpose\") end",
                                       "if ui.pressed == \"fail\" then montage.add_marker{at = 1, name = 'x'}; error({code = 1}) end"));
        QVERIFY(console->runFile(path));
        QPointer<QDockWidget> closing = qobject_cast<QDockWidget*>(win_->scriptPanel(path)->parentWidget());
        QTimer::singleShot(0, closing.data(), [closing] { closing->close(); });
        qobject_cast<QPushButton*>(win_->scriptPanel(path)->control("fail"))->click();
        QTRY_VERIFY(closing.isNull());
        QVERIFY(!win_->scriptPanel(path) && !console->running());
        QVERIFY(console->output().contains("marker-maker.lua: the script raised a table"));
        state()->undo();
        // The console's own script can have a panel too.
        QVERIFY(console->run("local ui = montage.panel{title = 'Here', controls = {{type = 'button', id = 'b'}}}\nprint('pressed ' .. ui.pressed)", "Console"));
        QVERIFY(win_->scriptPanel("console"));
        appSettings().remove("scripts/panels");
        qobject_cast<QDockWidget*>(win_->scriptPanel("console")->parentWidget())->close();
    }

    void scriptsInTheApp() {
        if (!scriptingAvailable()) QSKIP("built without Lua");
        state()->newProject();
        auto* console = win_->scriptConsole();
        QVERIFY(console && win_->findChild<QMenu*>("scriptsMenu"));
        auto titles = [&] {
            int n = 0;
            for (const Track& t : state()->sequence()->videoTracks)
                for (const Clip& c : t.clips) n += c.generator.type == "title";
            return n;
        };
        // Two titles from one run: one undo step, named for the script; what it prints in the console.
        const int steps = int(state()->history().undoCount());
        console->clearOutput();
        QVERIFY(console->run("local here = montage.live_context{}\n"
                             "print('at ' .. here.playhead)\n"
                             "montage.add_title{text = 'One', at = 0}\n"
                             "montage.add_title{text = 'Two', at = 1}\n",
                             "titles.lua"));
        QCOMPARE(titles(), 2);
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        QCOMPARE(state()->undoText(), QStringLiteral("Script: titles.lua"));
        QVERIFY2(console->output().contains("at 00:00:00:00"), qPrintable(console->output()));
        state()->undo();
        QCOMPARE(titles(), 0);
        // montage.undo{} takes back only the script's own run, never the editor's last change.
        QVERIFY(state()->edit("Mine", [](Project&, Sequence& s) {
            Marker m;
            m.name = "editor's";
            s.markers.push_back(m);
            return true;
        }));
        QVERIFY(!console->run("montage.undo{}", "undo.lua"));
        QCOMPARE(state()->sequence()->markers.size(), size_t(1));
        QVERIFY(console->run("montage.add_title{text = 'Gone', at = 0}\nmontage.undo{}", "undo2.lua"));
        QCOMPARE(titles(), 0);
        QCOMPARE(state()->undoText(), QStringLiteral("Mine"));
        // A tool's error: the run fails with it shown, and what it did before stays one undo step.
        QVERIFY(!console->run("montage.add_title{text = 'Kept', at = 0}\nmontage.add_title{at = 'never'}", "fails.lua"));
        QCOMPARE(titles(), 1);
        QCOMPARE(state()->undoText(), QStringLiteral("Script: fails.lua"));
        QVERIFY(console->output().contains("fails.lua:2:"));
        state()->undo();
        // Stop: a script that would never end stops, and the window kept working meanwhile.
        bool ticked = false;
        QTimer::singleShot(100, console, [&] {
            ticked = true;
            QVERIFY(console->running());
            console->stop();
        });
        QVERIFY(!console->run("while true do end", "forever.lua"));
        QVERIFY(ticked && !console->running());
        QVERIFY(console->output().endsWith("Stopped"));
        // ...also when it catches errors around the tools it calls.
        QTimer::singleShot(100, console, [console] { console->stop(); });
        QVERIFY(!console->run("while true do pcall(montage.live_context, {}) end", "catching.lua"));
        QVERIFY(console->output().endsWith("Stopped"));
        // Stop during a tool call: a call waiting is answered at once, one running is not applied.
        {
            LiveLink* link = win_->liveLink();
            auto addTitle = [](int id) {
                return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                                                 {"params", QJsonObject{{"name", "montage_add_title"}, {"arguments", QJsonObject{{"text", "x"}, {"at", 0}}}}}})
                    .toJson(QJsonDocument::Compact);
            };
            QByteArray running, waiting;
            link->handleScript(addTitle(1), [&running](QByteArray r) { running = r; }, "Script: t.lua", "test-run");
            link->handleScript(addTitle(2), [&waiting](QByteArray r) { waiting = r; }, "Script: t.lua", "test-run");
            link->cancelScript("test-run");
            QVERIFY2(waiting.contains("stopped before"), waiting.constData());
            QTRY_VERIFY(!running.isEmpty());
            QVERIFY2(running.contains("was stopped while"), running.constData());
            QCOMPARE(titles(), 0);
        }
        // montage.undo{} takes back this run's step only, not an earlier run of the same script.
        QVERIFY(console->run("montage.add_title{text = 'First', at = 0}", "same.lua"));
        QCOMPARE(titles(), 1);
        QVERIFY(!console->run("montage.undo{}", "same.lua"));
        QCOMPARE(titles(), 1);
        state()->undo();
        // Another project opened while a script runs: it stops rather than editing that one.
        QTimer::singleShot(100, this, [this] { state()->newProject(); });
        QVERIFY(!console->run("while true do montage.live_context{} end", "switch.lua"));
        QVERIFY2(console->output().contains("closed"), qPrintable(console->output()));
        QCOMPARE(int(state()->history().undoCount()), 0);
        // The scripts folder: what the menu lists, run from a file.
        qputenv("MONTAGE_SCRIPTS_DIR", (dir_.path() + "/scripts").toUtf8());
        QDir().mkpath(dir_.path() + "/scripts");
        QFile f(dir_.path() + "/scripts/Add Marker.lua");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("montage.add_marker{at = 2, name = montage.args[1] or 'none'}\n");
        f.close();
        QCOMPARE(listScripts(), QStringList{dir_.path() + "/scripts/Add Marker.lua"});
        QVERIFY(console->runFile(listScripts()[0], {"From a file"}));
        QCOMPARE(state()->sequence()->markers.back().name, std::string("From a file"));
        QVERIFY(console->open(listScripts()[0]) && console->editor()->toPlainText().contains("add_marker"));
        qunsetenv("MONTAGE_SCRIPTS_DIR");
        state()->newProject();
    }

    void pythonScriptsInTheApp() {
        if (!pythonAvailable()) QSKIP("no Python 3.8 or later here");
        state()->newProject();
        appSettings().remove("scripts/panels");
        auto* console = win_->scriptConsole();
        auto titles = [&] {
            int n = 0;
            for (const Track& t : state()->sequence()->videoTracks)
                for (const Clip& c : t.clips) n += c.generator.type == "title";
            return n;
        };
        // Two titles from one Python run: one undo step, named for the script; what it prints in the console.
        const int steps = int(state()->history().undoCount());
        console->clearOutput();
        QVERIFY2(console->run("here = montage.live_context()\n"
                              "print('at', here['playhead'])\n"
                              "montage.add_title(text='One', at=0)\n"
                              "montage.add_title({'text': 'Two', 'at': 1})\n",
                              "titles.py"),
                 qPrintable(console->output()));
        QCOMPARE(titles(), 2);
        QCOMPARE(int(state()->history().undoCount()), steps + 1);
        QCOMPARE(state()->undoText(), QStringLiteral("Script: titles.py"));
        QVERIFY2(console->output().contains("at 00:00:00:00"), qPrintable(console->output()));
        state()->undo();
        // A tool's error fails the run on its line, what it did before kept as one undo step.
        QVERIFY(!console->run("montage.add_title(text='Kept', at=0)\nmontage.add_title(at='never')", "fails.py"));
        QCOMPARE(titles(), 1);
        QVERIFY(console->output().contains("fails.py:2: ToolError"));
        state()->undo();
        // Stop: a script busy or asleep stops, the window working meanwhile.
        for (const char* endless : {"while True:\n    pass", "import time\ntime.sleep(600)", "while True:\n    try:\n        montage.live_context()\n    except Exception:\n        pass"}) {
            bool ticked = false;
            QTimer::singleShot(300, console, [&] {
                ticked = true;
                QVERIFY(console->running());
                console->stop();
            });
            QVERIFY(!console->run(endless, "forever.py"));
            QVERIFY(ticked && !console->running());
            QVERIFY(console->output().endsWith("Stopped"));
        }
        // The language: switching swaps the untouched example, a file opened or saved sets it, and an unsaved Python
        // script runs as Python.
        {
            ScriptConsole fresh(state(), win_->liveLink());
            QVERIFY(fresh.findChild<QComboBox*>("scriptLanguage") && !fresh.python());
            fresh.setPython(true);
            QVERIFY(fresh.editor()->toPlainText().startsWith("# A Python script"));
            fresh.setPython(false);
            QVERIFY(fresh.editor()->toPlainText().startsWith("-- A Lua script"));
        }
        console->setPython(true);
        console->editor()->setPlainText("import sys\nprint('version', sys.version_info[0])");
        console->clearOutput();
        console->findChild<QPushButton*>("runScript")->click();
        QTRY_VERIFY(!console->running());
        QVERIFY2(console->output().contains("version 3"), qPrintable(console->output()));
        qputenv("MONTAGE_SCRIPTS_DIR", (dir_.path() + "/pyscripts").toUtf8());
        QDir().mkpath(dir_.path() + "/pyscripts");
        const QString path = QFileInfo(dir_.path() + "/pyscripts/Marker Maker.py").absoluteFilePath();
        auto write = [&](const QByteArray& code) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(code);
        };
        const QByteArray code = R"py(
ui = montage.panel(title="Marker Maker", controls=[
    {"type": "text", "id": "name", "label": "Name", "value": "Cue"},
    {"type": "number", "id": "at", "label": "At (s)", "value": 1, "min": 0, "max": 600},
    {"type": "label", "id": "status", "text": ""},
    {"type": "button", "id": "add", "label": "Add Marker"},
])
montage.add_marker(at=ui["at"], name=ui["name"])
montage.set_panel(status="Added " + ui["name"])
print("added " + ui["name"])
)py";
        write(code);
        QCOMPARE(listScripts(), QStringList{path});
        QVERIFY(console->open(path) && console->python());
        QVERIFY(console->save(dir_.path() + "/pyscripts/copy.lua") && !console->python());
        // Saving the Lua example as Python keeps what the editor holds (no example swapped in).
        {
            ScriptConsole fresh(state(), win_->liveLink());
            const QString example = fresh.editor()->toPlainText();
            QVERIFY(example.startsWith("-- A Lua script"));
            QVERIFY(fresh.save(dir_.path() + "/pyscripts/copy.py") && fresh.python());
            QCOMPARE(fresh.editor()->toPlainText(), example);
        }
        QFile::remove(dir_.path() + "/pyscripts/copy.py");
        QFile::remove(dir_.path() + "/pyscripts/copy.lua");
        // A Python script's panel: opened by its run, its button runs it again with what the panel holds.
        QVERIFY(console->runFile(path));
        ScriptPanelWidget* panel = win_->scriptPanel(path);
        QVERIFY(panel);
        QVERIFY(state()->sequence()->markers.empty());
        qobject_cast<QLineEdit*>(panel->control("name"))->setText("Intro");
        qobject_cast<QPushButton*>(panel->control("add"))->click();
        QCOMPARE(state()->sequence()->markers.size(), size_t(1));
        QCOMPARE(QString::fromStdString(state()->sequence()->markers[0].name), QString("Intro"));
        QCOMPARE(qobject_cast<QLabel*>(panel->control("status"))->text(), QString("Added Intro"));
        QCOMPARE(panel->findChild<QLabel*>("panelMessage")->text(), QString("added Intro"));
        state()->undo();
        // Opened again at start-up: quietly (writing no files), and ended if it takes more than five seconds.
        auto* dock = qobject_cast<QDockWidget*>(panel->parentWidget());
        dock->close();
        QTRY_VERIFY(!win_->scriptPanel(path));
        appSettings().setValue("scripts/panels", QStringList{path});
        win_->restoreScriptPanels();
        QVERIFY(win_->scriptPanel(path));
        qobject_cast<QDockWidget*>(win_->scriptPanel(path)->parentWidget())->close();
        QTRY_VERIFY(!win_->scriptPanel(path));
        const QString trap = dir_.filePath("py-written-at-start.txt");
        write("open(r'" + trap.toUtf8() + "', 'w')\n" + code);
        appSettings().setValue("scripts/panels", QStringList{path});
        win_->restoreScriptPanels();
        QVERIFY(!QFileInfo::exists(trap) && !win_->scriptPanel(path));
        QVERIFY(console->output().contains("only reading while a panel opens"));
        write("import time\ntime.sleep(600)\n" + code);
        appSettings().setValue("scripts/panels", QStringList{path});
        QElapsedTimer spent;
        spent.start();
        win_->restoreScriptPanels();
        QVERIFY(spent.elapsed() < 15000 && !win_->scriptPanel(path) && !console->running());
        QVERIFY(!appSettings().value("scripts/panels").toStringList().contains(path));
        appSettings().remove("scripts/panels");
        qunsetenv("MONTAGE_SCRIPTS_DIR");
        console->setPython(false);
        state()->newProject();
    }

    void renderFarmInTheQueue() {
        state()->newProject();
        auto* panel = win_->findChild<RenderQueuePanel*>();
        QVERIFY(panel);
        const QString was = panel->farmFolder();
        auto* sendButton = panel->findChild<QPushButton*>("queueSendToFarm");
        QVERIFY(sendButton);
        // A job on the local queue, sent to the farm: off the queue, waiting there for a node.
        Project p = makeDefaultProject();
        p.active()->width = 320;
        p.active()->height = 180;
        edit::overwrite(p, *p.active(), {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 25));
        ExportSettings st;
        st.path = dir_.filePath("farm-out/queued.mp4").toStdString();
        st.audioCodec = "none";
        RenderQueue* queue = win_->renderQueue();
        const int id = queue->add("Farm job", "H.264", p, p.active()->id, st);
        panel->setFarmFolder({});
        QVERIFY(!panel->sendToFarm(id));  // (no farm yet)
        const QString farm = dir_.filePath("farm");
        panel->setFarmFolder(farm);
        QCOMPARE(appSettings().value("renderFarm/folder").toString(), farm);
        QVERIFY(panel->sendToFarm(id));
        QVERIFY(!queue->job(id));
        auto* list = panel->findChild<QTreeWidget*>("farmJobs");
        QCOMPARE(list->topLevelItemCount(), 1);
        QCOMPARE(list->topLevelItem(0)->text(0), QString("Farm job"));
        QVERIFY(list->topLevelItem(0)->text(3).startsWith("Waiting"));
        QCOMPARE(panel->findChild<QLabel*>("farmNodes")->text(), QString("No nodes running"));
        // Cancelled from the panel, then retried.
        list->setCurrentItem(list->topLevelItem(0));
        list->topLevelItem(0)->setSelected(true);
        auto* cancel = panel->findChild<QPushButton*>("farmCancel");
        auto* retry = panel->findChild<QPushButton*>("farmRetry");
        auto* remove = panel->findChild<QPushButton*>("farmRemove");
        QVERIFY(cancel->isEnabled() && !retry->isEnabled());
        cancel->click();
        QCOMPARE(list->topLevelItem(0)->text(3), QString("Cancelled"));
        QVERIFY(retry->isEnabled() && remove->isEnabled());
        retry->click();
        QVERIFY(list->topLevelItem(0)->text(3).startsWith("Waiting"));
        // A node renders it; the panel follows on its own (its timer).
        FarmNodeOptions o;
        o.farm = farm.toStdString();
        o.name = "Test node";
        o.once = true;
        int rendered = 0;
        std::thread node([&] { runFarmNode(o, &rendered); });
        node.join();
        QCOMPARE(rendered, 1);
        auto* dock = qobject_cast<QDockWidget*>(panel->parentWidget());
        const bool shown = dock && dock->isVisible();
        if (dock) dock->show();
        QTRY_VERIFY_WITH_TIMEOUT(list->topLevelItem(0)->text(3).startsWith("Done"), 5000);  // (read on a worker by its timer)
        if (dock && !shown) dock->hide();
        QCOMPARE(list->topLevelItem(0)->text(1), QString("Test node"));
        QVERIFY(QFileInfo::exists(QString::fromStdString(st.path)));
        QVERIFY(!cancel->isEnabled());
        remove->click();
        QCOMPARE(list->topLevelItemCount(), 0);
        panel->setFarmFolder(was);
    }

    void separateStemsFromTheMenu() {
        state()->newProject();
        QVERIFY(win_->findChild<QAction*>("separateStems"));
        {
            StemsDialog dlg(win_.get());
            auto* stems = dlg.findChild<QComboBox*>("stems");
            QVERIFY(stems && stems->count() == 4);
            QCOMPARE(dlg.options().stems, 4);
            QVERIFY(dlg.options().muteOriginal);
            // A film's mix into dialogue, music and effects: the note says which model and how big.
            auto* note = dlg.findChild<QLabel*>("stemsNote");
            QVERIFY(note && note->text().contains("Spleeter"));
            stems->setCurrentIndex(stems->findData(3));
            QCOMPARE(dlg.options().stems, 3);
            QVERIFY(note->text().contains("Bandit") && note->text().contains("447 MB"));
            stems->setCurrentIndex(stems->findData(2));
            dlg.findChild<QCheckBox*>("muteOriginal")->setChecked(false);
            QCOMPARE(dlg.options().stems, 2);
            QVERIFY(!dlg.options().muteOriginal);
        }
        QString why;
        QVERIFY(separateStems(state(), {}, StemOptions{}, win_.get(), &why).empty());
        QVERIFY(why.contains("Select"));
        if (!stemSeparationAvailable() || !stemModel(4).installed()) QSKIP("Set MONTAGE_STEM_MODELS to a folder holding spleeter-4stems");
        // A clip of JFK from 1 s to 6 s, at frame 10: its stems on four tracks named for them, linked with it, in sync,
        // from WAVs in the Stems folder beside the project; the clip muted. One undo takes the placing back.
        QVERIFY(state()->save(dir_.filePath("stems.montage")));
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        const double fps = state()->sequence()->fpsValue();
        Id clip = 0;
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            const edit::Result r = edit::placeMedia(p, s, ids[0], 10, 1 * fps, 6 * fps, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
            if (r.ok) clip = r.created.at(0);
            return r.ok;
        }));
        const size_t tracks = state()->sequence()->audioTracks.size();
        const int undoBefore = int(state()->history().undoCount());
        StemOptions o;
        o.handles = 0.5;
        const std::vector<Id> made = separateStems(state(), {clip}, o, win_.get(), &why);
        QVERIFY2(made.size() == 4, qPrintable(why));
        QCOMPARE(int(state()->history().undoCount()), undoBefore + 1);  // brought in and placed in one step
        const Sequence* s = state()->sequence();
        QCOMPARE(s->audioTracks.size(), tracks + 4);
        const Clip* original = edit::clipById(const_cast<Sequence&>(*s), clip);
        QVERIFY(original && !original->enabled);
        const QStringList names{"Vocals", "Drums", "Bass", "Other"};
        for (size_t k = 0; k < made.size(); ++k) {
            const Clip* c = edit::clipById(const_cast<Sequence&>(*s), made[k]);
            QVERIFY(c && c->start == 10 && c->duration == original->duration && c->linkGroup == original->linkGroup);
            QCOMPARE(QString::fromStdString(s->audioTracks[size_t(edit::locate(*s, made[k])->track.index)].name), names[int(k)]);
            const MediaItem* m = state()->project().findMedia(c->mediaId);
            QVERIFY(m && QString::fromStdString(m->path).startsWith(dir_.filePath("Stems/")));
            QVERIFY(QString::fromStdString(m->path).endsWith(names[int(k)] + ".wav"));
            QVERIFY(m->metadata.at("comment").find("separated from") != std::string::npos);
            // The source from half a second (and a frame) before the clip's In, so the stem's In is that far in.
            QVERIFY2(std::abs(c->sourceIn - (0.5 + 1.0 / fps) * fps) < 1e-6, qPrintable(QString::number(c->sourceIn)));
        }
        const size_t mediaWith = state()->project().media.size();
        state()->undo();
        QCOMPARE(state()->sequence()->audioTracks.size(), tracks);
        QVERIFY(edit::clipById(const_cast<Sequence&>(*state()->sequence()), clip)->enabled);
        QCOMPARE(state()->project().media.size(), mediaWith - 4);  // the stems leave the bin with that one undo
        state()->newProject();
    }

    void findWhatIsHeard() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/jfk.wav")});
        QCOMPARE(ids.size(), size_t(1));
        auto* panel = win_->findChild<ShotSearchPanel*>();
        QVERIFY(panel);
        auto* mode = panel->findChild<QComboBox*>("findMode");
        QVERIFY(mode && mode->count() == 3);
        mode->setCurrentIndex(2);
        QCOMPARE(panel->mode(), ShotSearchPanel::Heard);
        QVERIFY(panel->findChild<QLineEdit*>("shotQuery")->placeholderText().contains("applause"));
        auto* status = panel->findChild<QLabel*>("shotStatus");
        if (!soundSearchAvailable()) {
            QVERIFY(status->text().contains("cannot"));
            return;
        }
        QVERIFY2(status->text().contains("0 of 1"), qPrintable(status->text()));
        QVERIFY(!panel->findChild<QPushButton*>("indexShots")->isHidden());
        if (!soundModel().installed()) QSKIP("Set MONTAGE_SOUND_MODEL to the CLAP model");
        // Searching indexes the recording first (saved with the project, not an undo step), then finds the speech.
        const int undoBefore = int(state()->history().undoCount());
        QVERIFY(panel->search("a man giving a speech") >= 1);
        const MediaItem* m = state()->project().findMedia(ids[0]);
        QVERIFY(m && m->sounds && m->sounds->samples.size() == 1);
        QCOMPARE(int(state()->history().undoCount()), undoBefore);
        QVERIFY(status->text().contains("1 of 1"));
        QCOMPARE(panel->results()[0].media, ids[0]);
        auto* list = panel->findChild<QListWidget*>("shotResults");
        QVERIFY(list->count() >= 1 && list->item(0)->text().contains("jfk.wav"));
        QSignalSpy opened(panel, &ShotSearchPanel::openRequested);
        panel->open(0);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(1).value<FrameTime>(), FrameTime(0));  // the window it was heard in, from its start
        // Find Similar (shots like a frame) switches back to what's shown, so it indexes videos, not sound.
        panel->searchSimilar(ids[0], 1.0);
        QCOMPARE(panel->mode(), ShotSearchPanel::Shown);
        state()->newProject();
    }

    void colorGroupsFromTheMenu() {
        state()->newProject();
        QVERIFY(state()->edit("Shots", [](Project& p, Sequence& s) {
            for (int i = 0; i < 3; ++i) {
                Clip c = makeGeneratorClip(p, "color", 10);
                c.start = i * 10;
                if (!edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok) return false;
            }
            return true;
        }));
        const auto& clips = state()->sequence()->videoTracks[0].clips;
        const Id a = clips[0].id, b = clips[1].id, c = clips[2].id;
        QVERIFY(win_->findChild<QMenu*>("colorGroupMenu"));
        state()->setSelection({}, false);
        QCOMPARE(win_->newColorGroup("Nothing"), Id(0));  // nothing selected
        state()->setSelection({a, b}, false);
        const Id group = win_->newColorGroup("Interview");
        QVERIFY(group);
        QCOMPARE(colorGroupMembers(*state()->sequence(), group), (std::vector<Id>{a, b}));
        // The Inspector shows the group's grades for a member, and they run on every member.
        state()->setSelection({a}, false);
        auto* inspector = win_->findChild<InspectorWidget*>();
        QTRY_VERIFY(inspector->findChild<QLabel*>("colorGroupPre") && inspector->findChild<QLabel*>("colorGroupPost"));
        QVERIFY2(inspector->findChild<QLabel*>("colorGroupPre")->text().contains("2 clip"), qPrintable(inspector->findChild<QLabel*>("colorGroupPre")->text()));
        const Id post = findColorGroup(*state()->sequence(), group)->postId;
        QVERIFY(state()->edit("Add", [post](Project& p, Sequence& s) {
            edit::effectChain(s, post)->push_back(makeEffect(p, "invert"));
            return true;
        }));
        QCOMPARE(gradeChain(*state()->sequence(), *edit::clipById(*state()->sequence(), b)).size(), size_t(1));
        // The menu: a third clip joins, then leaves.
        QMenu* menu = win_->findChild<QMenu*>("colorGroupMenu");
        emit menu->aboutToShow();
        QVERIFY(menu->findChild<QAction*>("newColorGroup"));
        state()->setSelection({c}, false);
        QVERIFY(win_->addToColorGroup(group));
        QCOMPARE(colorGroupMembers(*state()->sequence(), group).size(), size_t(3));
        QVERIFY(win_->removeFromColorGroup());
        QVERIFY(!win_->removeFromColorGroup());
        QCOMPARE(colorGroupMembers(*state()->sequence(), group).size(), size_t(2));
        // A clip outside the group shows no group sections.
        QTRY_VERIFY(!inspector->findChild<QLabel*>("colorGroupPre"));
        // Undo takes it all back, a step at a time.
        state()->undo();  // remove
        state()->undo();  // add
        state()->undo();  // the effect
        QVERIFY(findColorGroup(*state()->sequence(), group)->post.empty());
        state()->undo();  // the group
        QVERIFY(state()->sequence()->colorGroups.empty());
        QCOMPARE(edit::clipById(*state()->sequence(), a)->colorGroup, Id(0));
        state()->newProject();
    }

    void spellCheckInCaptionsAndTitles() {
        setSpellCheckingOn(true);
        setTitleSpellingLanguage("en-US");
        state()->newProject();
        QVERIFY(state()->edit("Captions", [](Project& p, Sequence& s) {
            CaptionTrack t;
            t.id = p.newId();
            t.captions = {{0, 50, "Teh show begins."}, {60, 110, "All good here."}};
            s.captionTracks = {t};
            return true;
        }));
        auto* panel = win_->findChild<CaptionsPanel*>();
        auto* table = panel->findChild<QTableWidget*>();
        QTRY_COMPARE(table->rowCount(), 2);
        // The caption with a mistake is flagged, with the word in its tooltip.
        QCOMPARE(table->item(0, 3)->text(), QString("\u26A0"));
        QVERIFY2(table->item(0, 3)->toolTip().contains("Spelling: Teh"), qPrintable(table->item(0, 3)->toolTip()));
        QVERIFY(table->item(1, 3)->text().isEmpty());
        QVERIFY(panel->findChild<QLabel*>("captionCheckSummary")->text().contains("spelling"));
        QVERIFY(!panel->spellingMenu(1));  // nothing wrong there
        // Corrected from the suggestions, one undo step.
        std::unique_ptr<QMenu> menu(panel->spellingMenu(0));
        QVERIFY(menu);
        QMenu* teh = menu->actions().at(0)->menu();
        QVERIFY(teh && teh->title() == "Teh");
        QAction* the = nullptr;
        for (QAction* a : teh->actions())
            if (a->text() == "The") the = a;
        QVERIFY(the);
        the->trigger();
        QCOMPARE(state()->sequence()->captionTracks[0].captions[0].text, std::string("The show begins."));
        QVERIFY(!panel->spellingMenu(0));
        state()->undo();
        QCOMPARE(state()->sequence()->captionTracks[0].captions[0].text, std::string("Teh show begins."));
        // Added to the dictionary: the project's vocabulary, then right everywhere.
        menu.reset(panel->spellingMenu(0));
        QAction* learn = menu->actions().at(0)->menu()->findChild<QAction*>("learnSpelling");
        QVERIFY(learn);
        learn->trigger();
        QCOMPARE(state()->project().vocabulary, std::vector<std::string>{"Teh"});
        QVERIFY(!panel->spellingMenu(0));
        QTRY_VERIFY2(table->item(0, 3)->text().isEmpty(), qPrintable(table->item(0, 3)->toolTip()));
        state()->undo();
        QVERIFY(state()->project().vocabulary.empty());
        // Turned off: nothing flagged.
        auto* asYouType = win_->findChild<QAction*>("checkSpelling");
        QVERIFY(asYouType && asYouType->isChecked());
        asYouType->setChecked(false);
        QVERIFY(!panel->spellingMenu(0));
        QTRY_VERIFY(table->item(0, 3)->text().isEmpty());
        asYouType->setChecked(true);
        QVERIFY(panel->spellingMenu(0) != nullptr);
        delete panel->spellingMenu(0);

        // A title's text: misspelt words underlined, and corrected from the right-click menu.
        QPlainTextEdit edit;
        enableSpellCheck(&edit, state(), [] { return titleSpellingLanguage(); });
        edit.setPlainText("Welcom home");
        bool marked = false;
        for (const QTextLayout::FormatRange& r : edit.document()->firstBlock().layout()->formats())
            marked = marked || (r.start == 0 && r.length == 6 && r.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline);
        QVERIFY(marked);
        QMenu titleMenu;
        titleMenu.addAction("Copy");
        QTextCursor in(edit.document());
        in.setPosition(8);  // in "home"
        QVERIFY(!addSpellingActions(&titleMenu, &edit, in, state(), "en-US"));
        in.setPosition(2);
        QVERIFY(addSpellingActions(&titleMenu, &edit, in, state(), "en-US"));
        QCOMPARE(titleMenu.actions().at(0)->text(), QString("Welcome"));
        titleMenu.actions().at(0)->trigger();
        QCOMPARE(edit.toPlainText(), QString("Welcome home"));
        // The dictionary for titles, from Edit > Spelling.
        win_->findChild<QAction*>("spelling-en-GB")->trigger();
        QCOMPARE(titleSpellingLanguage(), QString("en-GB"));
        edit.setPlainText("Colour");
        QVERIFY(edit.document()->firstBlock().layout()->formats().isEmpty());
        win_->findChild<QAction*>("spelling-en-US")->trigger();
        QCOMPARE(titleSpellingLanguage(), QString("en-US"));
        // Quality Check offers it.
        QualityCheckDialog dlg(state(), win_.get());
        QVERIFY(dlg.findChild<QCheckBox*>("qcSpelling")->isChecked());
        QCOMPARE(dlg.settings().titleLanguage, std::string("en-US"));
        state()->newProject();
    }

    void hdrScopesAndLightLevels() {
        // A PQ frame: a quarter at 1000 nits, the rest at 100.
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        auto frameAt = [](const ColorSpace& cs, double hiNits, double loNits) {
            QImage f(160, 90, QImage::Format_RGBA64);
            const quint16 hi = quint16(std::lround(nitsToCode(cs, hiNits) * 65535)), lo = quint16(std::lround(nitsToCode(cs, loNits) * 65535));
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) {
                    const quint16 v = x < 80 && y < 45 ? hi : lo;
                    f.setPixelColor(x, y, QColor::fromRgba64(v, v, v, 65535));
                }
            return f;
        };
        ScopesWidget scopes;
        scopes.resize(640, 420);
        scopes.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scopes));
        auto* range = scopes.findChild<QComboBox*>("scopeRange");
        auto* nits = scopes.findChild<QLabel*>("scopeNits");
        QVERIFY(range && nits);
        scopes.setSignal(frameAt(pq, 1000, 100), 0, "rec2100pq", 1000);
        QTRY_VERIFY(scopes.hdr() && scopes.peakNits() > 0);
        QVERIFY2(std::fabs(scopes.peakNits() - 1000) < 3 && std::fabs(scopes.averageNits() - 325) < 3,
                 qPrintable(QString("%1 %2").arg(scopes.peakNits()).arg(scopes.averageNits())));
        QVERIFY(range->isVisible() && nits->isVisible());
        QVERIFY2(nits->text().contains("325"), qPrintable(nits->text()));
        QVERIFY(nits->styleSheet().isEmpty());  // within the mastering peak
        // The waveform's top brought down to 1000 nits shows the levels up to it.
        QCOMPARE(scopes.traceRows(), 256);
        range->setCurrentIndex(range->findData(1000));
        QCOMPARE(scopes.nitsRange(), 1000.0);
        QCOMPARE(scopes.traceRows(), int(std::lround(nitsToCode(pq, 1000) * 255)) + 1);
        for (auto m : {ScopesWidget::Mode::Quad, ScopesWidget::Mode::Histogram, ScopesWidget::Mode::Parade}) {
            scopes.setMode(m);
            QVERIFY(!scopes.grab().toImage().isNull());
        }
        // Above a 400-nit mastering peak: warned.
        scopes.setSignal(frameAt(pq, 1000, 100), 1, "rec2100pq", 400);
        QTRY_VERIFY(!nits->styleSheet().isEmpty());
        // HLG: nits on a 1000-nit display, no range to choose.
        const ColorSpace& hlg = *findColorSpace("rec2100hlg");
        scopes.setSignal(frameAt(hlg, 1000, 100), 2, "rec2100hlg", 1000);
        QTRY_VERIFY(scopes.signalSpace() == "rec2100hlg" && !range->isVisible());
        QVERIFY2(std::fabs(scopes.peakNits() - 1000) < 3 && std::fabs(scopes.averageNits() - 325) < 4, qPrintable(QString::number(scopes.averageNits())));
        // Back to SDR: code values, no nits.
        QImage sdr(160, 90, QImage::Format_RGB32);
        sdr.fill(qRgb(128, 128, 128));
        scopes.setMode(ScopesWidget::Mode::Waveform);
        scopes.setFrame(sdr, 3);
        QTRY_VERIFY(!scopes.hdr());
        QCOMPARE(scopes.peakNits(), -1.0);
        QVERIFY(!range->isVisible() && !nits->isVisible());
        QCOMPARE(scopes.traceRows(), 256);

        // The Program monitor gives the scopes an HDR sequence as delivered, not its SDR preview.
        state()->newProject();
        QVERIFY(state()->edit("HDR", [](Project& p, Sequence& s) {
            s.colorSpace = "rec2100pq";
            s.hdrPeakNits = 1000;
            Clip c = makeGeneratorClip(p, "color", 10);
            for (const char* k : {"color.r", "color.g", "color.b"}) c.generator.params[k] = 1.0;
            return edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok;
        }));
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        QSignalSpy spy(program, &PlaybackController::scopeFrameRendered);
        program->requestFrame();
        auto delivered = [&] {  // the latest frame for the scopes, once one comes as delivered
            for (int i = int(spy.size()) - 1; i >= 0; --i)
                if (spy.at(i).at(2).toString() == "rec2100pq") return spy.at(i);
            return QList<QVariant>{};
        };
        QTRY_VERIFY(!delivered().isEmpty());
        const QList<QVariant> last = delivered();
        const QImage signal = last.at(0).value<QImage>();
        QCOMPARE(signal.format(), QImage::Format_RGBA64);
        const double code = signal.pixelColor(signal.width() / 2, signal.height() / 2).redF();
        QVERIFY2(std::fabs(code - 0.5807) < 0.002, qPrintable(QString::number(code)));  // graphics white: 203 nits
        // Analyse HDR Light Levels: measured and kept with the sequence, one undo step.
        QVERIFY(win_->findChild<QAction*>("analyseHdrLightLevels"));
        QVERIFY(win_->analyseHdrLightLevels(false));
        QCOMPARE(state()->sequence()->hdrMaxCll, 203.0);
        QCOMPARE(state()->sequence()->hdrMaxFall, 203.0);
        // PQ: HDR10+'s scenes measured in the same pass and kept too (one shot, one scene at reference white).
        QCOMPARE(state()->sequence()->hdr10Plus.size(), size_t(1));
        QVERIFY(std::fabs(state()->sequence()->hdr10Plus[0].maxScl[1] - 203) < 1 && state()->sequence()->hdr10Plus[0].end == 10);
        {
            // The export dialog offers HDR10+ for PQ output only.
            ExportDialog ed(state(), win_.get());
            auto* hdr10 = ed.findChild<QCheckBox*>("exportHdr10Plus");
            auto* color = ed.findChild<QComboBox*>("exportColor");
            QVERIFY(hdr10 && color && hdr10->isEnabled());
            color->setCurrentIndex(color->findData(QStringLiteral("rec709")));
            QVERIFY(!hdr10->isEnabled());
            color->setCurrentIndex(color->findData(QStringLiteral("rec2100pq")));
            QVERIFY(hdr10->isEnabled());
        }
        state()->undo();
        QCOMPARE(state()->sequence()->hdrMaxCll, 0.0);
        QVERIFY(state()->sequence()->hdr10Plus.empty());
        // Not for SDR.
        state()->newProject();
        QVERIFY(!win_->analyseHdrLightLevels(false));
    }

    void editingFromAnotherSequence() {
        state()->newProject();
        const auto ids = state()->importFiles({QStringLiteral(MONTAGE_TEST_DATA_DIR "/faces/jfk-color.jpg")});
        QCOMPARE(ids.size(), size_t(1));
        const Id cut = state()->project().activeSequence;
        const Sequence* first = state()->sequence();
        // A selects reel at the same rate: three 30-frame stills on V1 (0, 30, 60).
        const Id selects = state()->newSequence(QStringLiteral("Selects"), first->width, first->height, first->fps);
        QVERIFY(selects && state()->project().activeSequence == selects);
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) {
            bool ok = true;
            for (int i = 0; i < 3; ++i) ok = ok && edit::placeMedia(p, s, ids[0], i * 30, 0, 30, V1, A1, false).ok;
            return ok;
        }));
        const std::vector<Id> pulls = [&] {
            std::vector<Id> out;
            for (const Clip& c : state()->sequence()->videoTracks[0].clips) out.push_back(c.id);
            return out;
        }();
        Id reel = 0;
        for (const MediaItem& m : state()->project().media)
            if (m.kind == MediaKind::Sequence && m.sequenceId == selects) reel = m.id;
        QVERIFY(reel);
        state()->setActiveSequence(cut);
        const auto v1 = [this]() -> const std::vector<Clip>& { return state()->sequence()->videoTracks[0].clips; };

        // Insert Sequences as Clips off: a source edit from the reel nests it.
        auto* asClips = win_->findChild<QAction*>(QStringLiteral("sequencesAsClips"));
        QVERIFY(asClips && asClips->isCheckable());
        if (asClips->isChecked()) asClips->trigger();
        QVERIFY(!state()->sequencesAsClips());
        state()->setSourceMedia(reel);
        state()->setSourceIn(15);
        state()->setSourceOut(44);
        state()->setPlayhead(0);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::Overwrite));
        QCOMPARE(v1().size(), size_t(1));
        QCOMPARE(v1()[0].mediaId, reel);
        QCOMPARE(v1()[0].duration, FrameTime(30));
        state()->undo();
        // On: the same edit brings the reel's own clips, cut at the marks (15 frames of each of the first two).
        asClips->trigger();
        QVERIFY(state()->sequencesAsClips());
        state()->setPlayhead(0);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::Overwrite));
        QCOMPARE(v1().size(), size_t(2));
        for (size_t i = 0; i < 2; ++i) {
            QCOMPARE(v1()[i].mediaId, ids[0]);
            QCOMPARE(v1()[i].start, FrameTime(15 * i));
            QCOMPARE(v1()[i].duration, FrameTime(15));
        }
        QCOMPARE(v1()[0].sourceIn, 15.0);
        QCOMPARE(state()->playhead(), FrameTime(30));  // after all of it, not after its first piece
        QCOMPARE(state()->project().findSequence(selects)->videoTracks[0].clips.size(), size_t(3));  // the reel is untouched
        state()->undo();
        QVERIFY(v1().empty());

        // The stacked timeline shows every sequence but the open one.
        auto* stacked = win_->findChild<StackedTimelineWidget*>();
        QVERIFY(stacked);
        auto* bin = win_->findChild<MediaBinWidget*>();
        emit bin->openInStackedTimeline(selects);  // the bin's "Open in Stacked Timeline"
        QCOMPARE(stacked->sequenceId(), selects);
        QCOMPARE(stacked->picker()->count(), 1);
        QVERIFY(win_->findChild<QDockWidget*>(QStringLiteral("stacked"))->isVisibleTo(win_.get()));
        // Drag its last two clips onto the timeline at frame 100: they keep their spacing.
        stacked->setSelection({pulls[1], pulls[2]});
        ppf_ = measurePpf();
        const auto dropAt = [&](FrameTime f, Qt::KeyboardModifiers mods) {
            std::unique_ptr<QMimeData> mime(stacked->dragData());
            QVERIFY(mime);
            const QPoint pos = pointFor(f, V1);
            QDragEnterEvent enter(pos, Qt::CopyAction, mime.get(), Qt::LeftButton, mods);
            QApplication::sendEvent(viewport(), &enter);
            QVERIFY(enter.isAccepted());
            QDropEvent drop(pos, Qt::CopyAction, mime.get(), Qt::LeftButton, mods);
            QApplication::sendEvent(viewport(), &drop);
        };
        dropAt(100, Qt::NoModifier);
        QCOMPARE(v1().size(), size_t(2));
        QCOMPARE(v1()[0].start, FrameTime(100));
        QCOMPARE(v1()[1].start, FrameTime(130));
        QCOMPARE(state()->selectedClips().size(), size_t(2));
        state()->undo();
        // A range marked with I and O, dropped with Ctrl, inserts into a 200-frame clip.
        QVERIFY(state()->edit("Place", [&](Project& p, Sequence& s) { return edit::placeMedia(p, s, ids[0], 0, 0, 200, V1, A1, false).ok; }));
        ppf_ = measurePpf();
        stacked->setSelection({});
        stacked->setPlayhead(15);
        QTest::keyClick(stacked->canvas(), Qt::Key_I);
        stacked->setPlayhead(44);
        QTest::keyClick(stacked->canvas(), Qt::Key_O);
        QCOMPARE(stacked->markIn(), FrameTime(15));
        QCOMPARE(stacked->markOut(), FrameTime(44));
        dropAt(100, Qt::ControlModifier);
        QCOMPARE(state()->sequence()->duration(), FrameTime(230));
        QCOMPARE(v1().size(), size_t(4));
        QCOMPARE(v1()[1].start, FrameTime(100));
        QCOMPARE(v1()[2].start, FrameTime(115));
        QCOMPARE(v1()[3].start, FrameTime(130));
        state()->undo();
        state()->undo();

        // Swap: edit the reel and stack the cut. A cut that nests the reel cannot go back into it.
        asClips->trigger();
        state()->setPlayhead(0);
        QVERIFY(state()->sourceEdit(EditorState::SourceEdit::Overwrite));
        QCOMPARE(v1()[0].mediaId, reel);
        QVERIFY(stacked->swap());
        QCOMPARE(state()->project().activeSequence, selects);
        QCOMPARE(stacked->sequenceId(), cut);
        stacked->setMarks(-1, -1);
        stacked->setSelection({state()->project().findSequence(cut)->videoTracks[0].clips[0].id});
        ppf_ = measurePpf();
        dropAt(200, Qt::NoModifier);
        QCOMPARE(v1().size(), size_t(3));
        state()->setActiveSequence(cut);
        QCOMPARE(stacked->sequenceId(), selects);  // the open sequence never shows in the stack
        state()->undo();
        if (asClips->isChecked()) asClips->trigger();
    }

    void hdrViewerOnHdrSequences() {
        auto* action = win_->findChild<QAction*>("hdrViewer");
        QVERIFY(action);
        QCOMPARE(action->isEnabled(), hdrViewerBuilt());
        auto* program = win_->findChild<PlaybackController*>("programPlayback");
        MonitorPanel* panel = nullptr;
        for (MonitorPanel* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) panel = m;
        QVERIFY(program && panel);
        ViewerWidget* viewer = panel->viewer();
        auto* badge = panel->findChild<QLabel*>("hdrBadge");
        QVERIFY(badge);
        auto makeHdr = [&](const char* space) {
            state()->newProject();
            return state()->edit("HDR", [space](Project& p, Sequence& s) {
                s.colorSpace = space;
                s.hdrPeakNits = 1000;
                Clip c = makeGeneratorClip(p, "color", 10);
                for (const char* k : {"color.r", "color.g", "color.b"}) c.generator.params[k] = 1.0;
                return edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok;
            });
        };
        auto centre = [](const HdrPicture& pic) {
            float px[4];
            qFloatFromFloat16(px, reinterpret_cast<const qfloat16*>(&pic.rgba[(size_t(pic.height / 2) * size_t(pic.width) + size_t(pic.width / 2)) * 4]), 4);
            return std::array<float, 4>{px[0], px[1], px[2], px[3]};
        };
        // The controller renders an HDR sequence's light when asked: graphics white (203 nits) is SDR white, 1.0. (The
        // viewer, off, leaves the controller alone; on, it renders HDR light only while it can show it.)
        viewer->setHdrViewer(false);
        QVERIFY(makeHdr("rec2100pq"));
        const bool wasOn = program->hdrOutput();
        program->setHdrOutput(true);
        QSignalSpy hdrSpy(program, &PlaybackController::hdrFrameRendered);
        auto latest = [&] { return hdrSpy.isEmpty() ? HdrPicturePtr() : hdrSpy.last().at(0).value<HdrPicturePtr>(); };
        program->requestFrame();
        QTRY_VERIFY(latest() != nullptr);
        const HdrPicturePtr pic = latest();
        QVERIFY(pic->width > 0 && pic->rgba.size() == size_t(pic->width) * size_t(pic->height) * 4);
        const auto c = centre(*pic);
        QVERIFY2(std::fabs(c[0] - 1) < 0.01 && std::fabs(c[1] - 1) < 0.01 && std::fabs(c[2] - 1) < 0.01 && c[3] == 1,
                 qPrintable(QString("%1 %2 %3 %4").arg(c[0]).arg(c[1]).arg(c[2]).arg(c[3])));
        QVERIFY(std::fabs(pic->contentPeak - 1000.0 / 203) < 1e-6);
        // Playing too, frame after frame.
        hdrSpy.clear();
        program->seek(0);
        program->play();
        QTRY_VERIFY(hdrSpy.size() >= 3);
        program->pause();
        for (const QList<QVariant>& args : hdrSpy) QVERIFY(args.at(0).value<HdrPicturePtr>() != nullptr);
        // An SDR sequence has none (the viewer shows its SDR picture).
        QVERIFY(makeHdr("rec709"));
        hdrSpy.clear();
        program->requestFrame();
        QTRY_VERIFY(!hdrSpy.isEmpty());
        QTRY_VERIFY(latest() == nullptr);
        program->setHdrOutput(wasOn);

        // Without a display that shows HDR the viewer stays SDR, with the reason.
        viewer->setHdrViewer(false);
        viewer->setHdrViewer(true);
        viewer->setHdrPicture(pic);
        QVERIFY(!viewer->hdrShowing());
        QVERIFY(!badge->isVisible());
        QCOMPARE(viewer->hdrHeadroom(), 1.0);
#ifdef MONTAGE_HDR_VIEWER
        QVERIFY(!viewer->hdrStatus().isEmpty());  // (this machine's display is not HDR, or there is no GPU device)
        // On a stand-in HDR display (Qt's Null RHI, four times SDR white): the picture on the HDR surface, the viewer's
        // own drawing over it with the picture's area clear, the badge up, input passed on to the viewer.
        HdrSurface::setTestDisplay(4);
        viewer->setHdrViewer(false);
        viewer->setHdrViewer(true);
        QVERIFY(makeHdr("rec2100pq"));
        program->requestFrame();
        QTRY_VERIFY(viewer->hdrShowing());
        QCOMPARE(viewer->hdrHeadroom(), 4.0);
        QTRY_VERIFY(badge->isVisible());
        QVERIFY(program->hdrOutput());
        HdrSurface* surface = viewer->hdrSurface();
        QVERIFY(surface && surface->hdrActive());
        QTRY_VERIFY(!surface->overlay().isNull());
        const QImage overlay = surface->overlay();
        const QRectF r = viewer->imageRect();
        const qreal dpr = overlay.devicePixelRatio();
        QVERIFY(!r.isEmpty());
        QCOMPARE(qAlpha(overlay.pixel(int(r.center().x() * dpr), int(r.center().y() * dpr))), 0);
        if (r.left() > 4) QCOMPARE(qAlpha(overlay.pixel(int(2 * dpr), int(r.center().y() * dpr))), 255);  // the background
        // Guides drawn by the viewer reach the surface.
        viewer->setSafeMargins(true);
        QTRY_VERIFY(surface->overlay() != overlay);
        viewer->setSafeMargins(false);
        struct Presses : QObject {
            int n = 0;
            bool eventFilter(QObject*, QEvent* e) override {
                if (e->type() == QEvent::MouseButtonPress) ++n;
                return false;
            }
        } presses;
        viewer->installEventFilter(&presses);
        QMouseEvent press(QEvent::MouseButtonPress, r.center(), r.center(), viewer->mapToGlobal(r.center().toPoint()), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, r.center(), r.center(), viewer->mapToGlobal(r.center().toPoint()), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &release);
        viewer->removeEventFilter(&presses);
        QCOMPARE(presses.n, 1);
        // Turned off and on: the last HDR picture is not shown again (it may be another frame by then); the next brings it back.
        viewer->setHdrViewer(false);
        viewer->setHdrViewer(true);
        QVERIFY(!viewer->hdrShowing());
        program->requestFrame();
        QTRY_VERIFY(viewer->hdrShowing());
        // The display stops showing HDR (asked every second, paused too): the SDR picture again, and why.
        HdrSurface::setTestDisplay(0);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->hdrShowing(), 5000);
        QCOMPARE(viewer->hdrStatus(), QStringLiteral("This display does not show HDR"));
        HdrSurface::setTestDisplay(4);
        viewer->setHdrViewer(false);
        viewer->setHdrViewer(true);
        program->requestFrame();
        QTRY_VERIFY(viewer->hdrShowing());
        // Two-up trimming draws over it in SDR; an SDR sequence and the viewer switched off take it away.
        viewer->setTwoUp(QImage(), QImage(), "A", "B");
        QVERIFY(!viewer->hdrShowing());
        viewer->clearTwoUp();
        QTRY_VERIFY(viewer->hdrShowing());
        QVERIFY(makeHdr("rec709"));
        program->requestFrame();
        QTRY_VERIFY(!viewer->hdrShowing());
        QTRY_VERIFY(!badge->isVisible());
        HdrSurface::setTestDisplay(0);
#endif
        // The menu's setting decides again.
        viewer->setHdrViewer(false);
        panel->setHdrViewer(action->isChecked());
        QVERIFY(!viewer->hdrShowing());
        state()->newProject();
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

    void cryptomattePicking() {
        if (!exrAvailable()) QSKIP("Built without OpenEXR");
        // A render with a diffuse layer and a Cryptomatte: "ball" on the left half, "floor" on the right.
        const int W = 320, H = 180;
        const float ball = cryptomatteId("ball"), floor = cryptomatteId("floor");
        for (int n = 1; n <= 6; ++n) {
            std::vector<ExrOutLayer> layers(3);
            layers[0].channels = {"R", "G", "B", "A"};
            layers[1].name = "diffuse", layers[1].channels = {"R", "G", "B"};
            layers[2].name = "CryptoObject00", layers[2].channels = {"R", "G", "B", "A"}, layers[2].fullFloat = true;
            for (auto& l : layers) l.data.assign(size_t(W) * H * l.channels.size(), 0.0f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = size_t(y) * W + x;
                    const bool left = x < W / 2;
                    std::fill_n(&layers[0].data[i * 4], 3, 0.5f);
                    layers[0].data[i * 4 + 3] = 1;
                    layers[1].data[i * 3 + (left ? 0 : 1)] = 1;
                    layers[2].data[i * 4] = left ? ball : floor;
                    layers[2].data[i * 4 + 1] = 1;
                }
            const std::map<std::string, std::string> attrs{
                {"cryptomatte/0a1b2c3/name", "CryptoObject"}, {"cryptomatte/0a1b2c3/hash", "MurmurHash3_32"},
                {"cryptomatte/0a1b2c3/conversion", "uint32_to_float32"},
                {"cryptomatte/0a1b2c3/manifest", "{\"ball\":\"" + cryptomatteHex(ball) + "\",\"floor\":\"" + cryptomatteHex(floor) + "\"}"}};
            std::string err;
            QVERIFY2(writeExr(QString(dir_.path() + "/pick.%1.exr").arg(n, 4, 10, QChar('0')).toStdString(), W, H, layers, "zip", attrs, &err),
                     err.c_str());
        }
        ImageSequence seq;
        QVERIFY(detectImageSequence((dir_.path() + "/pick.0001.exr").toStdString(), seq));
        seq.fps = {25, 1};
        state()->newProject();
        state()->apply("Place", [&](Project& p, Sequence& s) {
            s.width = W, s.height = H, s.fps = {25, 1};
            MediaItem m;
            m.id = p.newId();
            if (!probeMedia(imageSequencePath(seq), m)) return edit::Result{};
            p.media.push_back(m);
            return edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {}, false);
        });
        QCOMPARE(state()->sequence()->videoTracks[0].clips.size(), size_t(1));
        const Id clip = state()->sequence()->videoTracks[0].clips.at(0).id;
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
        // The clip's layer, chosen in the Inspector as one undo step.
        QTRY_VERIFY(visible("exrLayer"));
        auto* layer = static_cast<QComboBox*>(visible("exrLayer"));
        QCOMPARE(layer->count(), 2);
        const int diffuse = layer->findData(QStringLiteral("diffuse"));
        QVERIFY(diffuse > 0);
        layer->setCurrentIndex(diffuse);
        emit layer->activated(diffuse);
        QTRY_COMPARE(edit::clipById(*state()->sequence(), clip)->layer, std::string("diffuse"));
        state()->undo();
        QTRY_COMPARE(edit::clipById(*state()->sequence(), clip)->layer, std::string());
        state()->redo();
        // A Cryptomatte mask: its controls, then objects picked in the Program monitor.
        state()->edit("Invert", [clip](Project& p, Sequence& s) {
            Effect e = makeEffect(p, "invert");
            e.params["mask.shape"] = Param(7.0);
            edit::clipById(s, clip)->effects.push_back(e);
            return true;
        });
        QTRY_VERIFY(visible("cryptomatteStatus"));
        QVERIFY(static_cast<QLabel*>(visible("cryptomatteStatus"))->text().contains("Click objects"));
        QCOMPARE(static_cast<QComboBox*>(visible("cryptomatteLayer"))->currentText(), QString("CryptoObject"));
        QVERIFY(!visible("cryptomatteClear")->isEnabled());
        MonitorPanel* program = nullptr;
        for (auto* m : win_->findChildren<MonitorPanel*>())
            if (m->mode() == MonitorPanel::Mode::Program) program = m;
        QVERIFY(program);
        ViewerWidget* viewer = program->viewer();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->image().isNull(), 5000);
        const QRectF r = viewer->imageRect();
        auto at = [&](double x, double y) { return QPointF(r.left() + x * r.width() / W, r.top() + y * r.height() / H).toPoint(); };
        auto ids = [&]() { return edit::clipById(*state()->sequence(), clip)->effects.back().s("mask.crypto_ids"); };
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(60, 90));
        QTRY_COMPARE(ids(), cryptomatteHex(ball));
        QTRY_VERIFY(static_cast<QLabel*>(visible("cryptomatteStatus"))->text() == "ball");
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, at(260, 90));
        QTRY_VERIFY(static_cast<QLabel*>(visible("cryptomatteStatus"))->text() == "ball, floor");
        // Choosing the Cryptomatte already shown (the file's first, none named) keeps the picks.
        emit static_cast<QComboBox*>(visible("cryptomatteLayer"))->activated(0);
        QCOMPARE(parseCryptomatteIds(ids()).size(), size_t(2));
        // The matte: the inverted ball (diffuse red becomes cyan) and floor.
        const Image both = renderProgramFrame(state()->project(), *state()->sequence(), 0, RenderOptions{});
        QVERIFY(both.at(60, 90)[0] < 0.05f && both.at(260, 90)[1] < 0.05f);
        // Ctrl-click takes one away (one undo step each); Clear takes them all.
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::ControlModifier, at(60, 90));
        QTRY_COMPARE(ids(), cryptomatteHex(floor));
        state()->undo();
        QTRY_COMPARE(parseCryptomatteIds(ids()).size(), size_t(2));
        QTRY_VERIFY(visible("cryptomatteClear") && visible("cryptomatteClear")->isEnabled());
        static_cast<QToolButton*>(visible("cryptomatteClear"))->click();
        QTRY_COMPARE(ids(), std::string());
    }

    void gpuStartupGuard() {
        // Turning GPU rendering on marks the settings until the GPU has drawn a frame, so a driver crash while starting
        // it leaves it off the next launch; a GPU that can't start leaves no mark.
        auto* gpuAction = win_->findChild<QAction*>(QStringLiteral("gpuRendering"));
        QVERIFY(gpuAction);
        const bool was = gpuAction->isChecked();
        if (was) gpuAction->trigger();  // (off first)
        QVERIFY(!gpuAction->isChecked());
        gpuAction->trigger();
        if (gpuAction->isChecked()) {
            loadDemo();
            state()->setPlayhead(5);
            QTRY_VERIFY_WITH_TIMEOUT(!appSettings().contains("playback/gpuStarting"), 10000);
        } else {
            QVERIFY(!appSettings().contains("playback/gpuStarting"));
        }
        if (gpuAction->isChecked() != was) gpuAction->trigger();
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
        // Several objects: + adds one, − one to cut out (chosen at once), Cut Out flips it, Remove drops the chosen one;
        // each one undo step. Choosing an object is not an edit: it makes no undo step.
        {
            auto fx = [&]() -> const Effect& { return edit::clipById(*state()->sequence(), clip)->effects.back(); };
            auto* list = static_cast<QComboBox*>(visible("objectList"));
            QVERIFY(list && visible("objectAdd") && visible("objectSubtract") && visible("objectRemove") && visible("objectCutOut"));
            QCOMPARE(list->count(), 1);
            // Until the first object is picked there is nothing to add to or cut out of.
            QVERIFY(!visible("objectAdd")->isEnabled() && !visible("objectSubtract")->isEnabled());
            state()->edit("Pick Object", [&](Project& p, Sequence& s) {
                Clip* c = edit::clipById(s, clip);
                Effect& ef = c->effects.back();
                ef.object = withNewObject(p, s, *c, ef, false);
                return ef.object != nullptr;
            });
            QTRY_VERIFY(visible("objectAdd")->isEnabled() && visible("objectSubtract")->isEnabled());
            static_cast<QToolButton*>(visible("objectSubtract"))->click();
            QTRY_VERIFY(fx().object->count() == 2);
            QTRY_COMPARE(static_cast<QComboBox*>(visible("objectList"))->count(), 2);
            list = static_cast<QComboBox*>(visible("objectList"));
            QCOMPARE(list->currentIndex(), 1);
            QVERIFY(list->itemText(1).contains("cut out"));
            QCOMPARE(state()->maskObject(fx()), 1);
            QVERIFY(fx().object->object(1).subtract);
            auto* cut = static_cast<QCheckBox*>(visible("objectCutOut"));
            QVERIFY(cut->isEnabled() && cut->isChecked());
            cut->click();
            QTRY_VERIFY(!fx().object->object(1).subtract);
            // Choosing the first: Cut Out is off for it, and undo still undoes the Cut Out change.
            const QString lastStep = state()->undoText();
            list = static_cast<QComboBox*>(visible("objectList"));
            list->setCurrentIndex(0);
            emit list->activated(0);
            QTRY_COMPARE(state()->maskObject(fx()), 0);
            QTRY_VERIFY(!static_cast<QCheckBox*>(visible("objectCutOut"))->isEnabled());
            QCOMPARE(state()->undoText(), lastStep);
            list = static_cast<QComboBox*>(visible("objectList"));
            list->setCurrentIndex(1);
            emit list->activated(1);
            QTRY_COMPARE(state()->maskObject(fx()), 1);
            static_cast<QToolButton*>(visible("objectRemove"))->click();
            QTRY_VERIFY(fx().object && fx().object->count() == 1);
            QCOMPARE(state()->maskObject(fx()), 0);
            static_cast<QToolButton*>(visible("objectRemove"))->click();
            QTRY_VERIFY(!fx().object);
        }

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

    void licencesShown() {
        // Help offers the licences, About Qt and About Montage.
        QAction* show = win_->findChild<QAction*>("showLicences");
        QVERIFY(show);
        // The dialog shows Montage's licence, the GPL itself and the third-party notices, all built in.
        LicencesDialog dlg(win_.get());
        auto* montagePage = dlg.findChild<QTextBrowser*>("licenceMontage");
        QVERIFY(montagePage && montagePage->toPlainText().contains("either version 3 of the License"));
        QVERIFY(montagePage->toPlainText().contains("additional permission under section 7"));
        auto* gpl = dlg.findChild<QTextBrowser*>("licenceGpl");
        QVERIFY(gpl && gpl->toPlainText().contains("GNU GENERAL PUBLIC LICENSE"));
        auto* notices = dlg.findChild<QTextBrowser*>("licenceNotices");
        QVERIFY(notices && notices->toPlainText().contains("lensfun") && notices->toPlainText().contains("Sylvain Munaut"));
        // FFmpeg reports its own licence, which the dialog and About Montage repeat.
        QVERIFY(!ffmpegLicence().isEmpty() && montagePage->toPlainText().contains(ffmpegLicence()));
        // A build run from its build folder has no packaged licences folder to open.
        QVERIFY(packagedLicencesFolder().isEmpty());
        QVERIFY(!dlg.findChild<QPushButton*>("openLicencesFolder")->isEnabled());
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

    void remoteReviewRoundTrip() {
        loadDemo();
        std::string why;
        if (!output::StreamBackend().available(&why)) QSKIP(why.c_str());
        QUdpSocket udp;
        udp.bind(QHostAddress::LocalHost, 0);
        const int port = udp.localPort();
        udp.close();
        QTcpServer tcp;
        tcp.listen(QHostAddress::Any, 0);
        const int markersPort = tcp.serverPort();
        tcp.close();
        // Started from Playback › Video Output › Remote Review…, a passphrase made for it.
        auto* action = win_->findChild<QAction*>("remoteReview");
        QVERIFY(action);
        action->trigger();
        auto* dlg = win_->findChild<QDialog*>("remoteReviewDialog");
        QVERIFY(dlg);
        dlg->findChild<QLineEdit*>("reviewStreamAddress")->setText(QStringLiteral("srt://127.0.0.1:%1").arg(port));
        const QString pass = dlg->findChild<QLineEdit*>("reviewStreamPassphrase")->text();
        QCOMPARE(pass.size(), 20);
        auto* size = dlg->findChild<QComboBox*>("reviewStreamSize");
        size->setCurrentIndex(size->findData(540));
        dlg->findChild<QCheckBox*>("reviewStreamHardware")->setChecked(false);
        dlg->findChild<QSpinBox*>("reviewStreamMarkersPort")->setValue(markersPort);
        state()->setPlayhead(10);
        dlg->findChild<QPushButton*>("reviewStreamStart")->click();
        auto* controller = win_->findChild<RemoteReviewController*>();
        QVERIFY(controller);
        QVERIFY2(controller->running(), qPrintable(controller->status()));
        QVERIFY(action->isChecked());
        QVERIFY(controller->scheduler()->mode().height <= 540);
        QVERIFY(dlg->findChild<QLabel*>("reviewStreamHowTo")->text().contains(pass));

        // A reviewer watches in File › Watch Remote Review…: the frame at the playhead, its timecode.
        auto* watch = win_->findChild<QAction*>("watchRemoteReview");
        QVERIFY(watch);
        watch->trigger();
        auto* viewer = win_->findChild<ReviewViewerWindow*>("reviewViewer");
        QVERIFY(viewer);
        viewer->findChild<QLineEdit*>("reviewAddress")->setText(QStringLiteral("srt://127.0.0.1:%1").arg(port));
        viewer->findChild<QLineEdit*>("reviewPassphrase")->setText(pass);
        viewer->findChild<QLineEdit*>("reviewAuthor")->setText(QStringLiteral("Ana"));
        viewer->findChild<QLineEdit*>("reviewMarkersTo")->setText(QStringLiteral("127.0.0.1:%1").arg(markersPort));
        viewer->findChild<QPushButton*>("reviewConnect")->click();
        QTRY_VERIFY_WITH_TIMEOUT(viewer->shownTag().frame == 10, 20000);
        const Rational fps = state()->sequence()->fps;
        QCOMPARE(viewer->findChild<QLabel*>("reviewTimecode")->text(), QString::fromStdString(formatTimecode(10, fps)));
        QVERIFY(!viewer->findChild<ReviewPicture*>("reviewPicture")->image().isNull());
        QTRY_VERIFY_WITH_TIMEOUT(dlg->findChild<QLabel*>("reviewStreamStatus")->text().contains(tr("Streaming to a viewer")), 5000);
        // The editor seeks; the reviewer sees it.
        state()->setPlayhead(40);
        QTRY_VERIFY_WITH_TIMEOUT(viewer->shownTag().frame == 40, 10000);

        // A note sent from there lands as a marker at that frame, named after the reviewer, in one undo step.
        const size_t before = state()->sequence()->markers.size();
        viewer->findChild<QLineEdit*>("reviewNote")->setText(QStringLiteral("Warmer here"));
        viewer->findChild<QPushButton*>("reviewAddMarker")->click();
        QTRY_COMPARE_WITH_TIMEOUT(state()->sequence()->markers.size(), before + 1, 10000);
        auto it = std::find_if(state()->sequence()->markers.begin(), state()->sequence()->markers.end(),
                               [](const Marker& m) { return m.comment == "Warmer here"; });
        QVERIFY(it != state()->sequence()->markers.end());
        QCOMPARE(it->t, FrameTime(40));
        QVERIFY(QString::fromStdString(it->name).contains("Ana"));
        QVERIFY(!it->reviewId.empty());
        QTRY_VERIFY_WITH_TIMEOUT(viewer->findChild<QLabel*>("reviewStatus")->text().contains(tr("Marker sent")), 5000);
        state()->undo();
        QCOMPARE(state()->sequence()->markers.size(), before);
        // A wrong passphrase gets nothing through.
        QString err;
        output::RemoteMarker m{40, 0, 0, "", "Eve", "Let me in", "", ""};
        std::string swhy;
        QVERIFY(!output::sendRemoteMarkers("127.0.0.1", markersPort, "the wrong passphrase", {m}, &swhy));
        QTest::qWait(400);
        QCOMPARE(state()->sequence()->markers.size(), before);

        // An agent sees it running with the viewer connected, and stops it.
        auto call = [&](const QJsonObject& args) {
            QJsonObject answer;
            const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                                  {"params", QJsonObject{{"name", "montage_remote_review"}, {"arguments", args}}}};
            win_->liveLink()->handle(QJsonDocument(msg).toJson(QJsonDocument::Compact),
                                     [&](QByteArray a) { answer = QJsonDocument::fromJson(a).object().value("result").toObject(); });
            return answer;
        };
        QJsonObject r = call({{"action", "status"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(r.value("structuredContent").toObject().value("viewer_connected").toBool());
        viewer->close();
        r = call({{"action", "stop"}});
        QVERIFY(!r.value("isError").toBool() && !controller->running());
        QVERIFY(!action->isChecked());
        // Started by an agent with no passphrase: one is made and given back.
        r = call({{"action", "start"}, {"address", QStringLiteral("srt://127.0.0.1:%1").arg(port)}, {"passphrase", ""}, {"hardware", false}, {"markers_port", -1}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QCOMPARE(r.value("structuredContent").toObject().value("passphrase").toString().size(), 20);
        QVERIFY(controller->running());
        r = call({{"action", "start"}, {"address", "rtmp://example.com:1935"}});
        QVERIFY(r.value("isError").toBool());
        r = call({{"action", "stop"}});
        QVERIFY(!controller->running());
        dlg->close();
    }

    void referenceOutputFollowsProgram() {
        loadDemo();
        // The test device stands in for a DeckLink card or NDI.
        qputenv("MONTAGE_VIRTUAL_OUTPUT", "1");
        auto* action = win_->findChild<QAction*>("referenceOutput");
        QVERIFY(action);
        action->trigger();
        auto* dlg = win_->findChild<QDialog*>("referenceOutputDialog");
        QVERIFY(dlg);
        auto* backend = dlg->findChild<QComboBox*>("outputBackend");
        QVERIFY(backend);
        const int virt = backend->findData(QStringLiteral("Virtual"));
        QVERIFY(virt >= 0);
        backend->setCurrentIndex(virt);
        QCOMPARE(dlg->findChild<QComboBox*>("outputDevice")->currentText(), QStringLiteral("Virtual Output"));
        // Started at the sequence's own mode: the Program's frames go out and the menu shows it on.
        state()->setPlayhead(10);
        auto* start = dlg->findChild<QPushButton*>("outputStart");
        start->click();
        auto* controller = win_->findChild<ReferenceOutputController*>();
        QVERIFY(controller);
        QVERIFY2(controller->running(), qPrintable(controller->status()));
        QVERIFY(action->isChecked());
        auto dev = std::dynamic_pointer_cast<output::VirtualBackend>(controller->backend());
        QVERIFY(dev);
        // (a small sequence is fitted into the device's mode at its rate)
        QCOMPARE(dev->openMode().fps.num * state()->sequence()->fps.den, state()->sequence()->fps.num * dev->openMode().fps.den);
        QTRY_VERIFY_WITH_TIMEOUT(!dev->sent().empty() && dev->sent().back().frame == 10, 5000);
        // A seek sends the new frame.
        state()->setPlayhead(40);
        QTRY_VERIFY_WITH_TIMEOUT(dev->sent().back().frame == 40, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(dlg->findChild<QLabel*>("outputStatus")->text().contains("Virtual Output"), 2000);
        // Stopped from the panel.
        start->click();
        QVERIFY(!controller->running());
        QVERIFY(!action->isChecked());
        dlg->close();
        // The same from an agent: listed, started on the Virtual device, its status, stopped.
        auto call = [&](const QJsonObject& args) {
            QJsonObject answer;
            const QJsonObject m{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                                {"params", QJsonObject{{"name", "montage_reference_output"}, {"arguments", args}}}};
            win_->liveLink()->handle(QJsonDocument(m).toJson(QJsonDocument::Compact),
                                     [&](QByteArray a) { answer = QJsonDocument::fromJson(a).object().value("result").toObject(); });
            return answer;
        };
        QJsonObject r = call({{"action", "list"}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(QJsonDocument(r).toJson().contains("Virtual Output"));
        r = call({{"action", "start"}, {"interface", "Virtual"}, {"device", "Virtual Output"}, {"mode", "1080p25"}, {"depth", 8}});
        QVERIFY2(!r.value("isError").toBool(), QJsonDocument(r).toJson().constData());
        QVERIFY(controller->running());
        dev = std::dynamic_pointer_cast<output::VirtualBackend>(controller->backend());
        QCOMPARE(dev->openMode().name, std::string("1080p25"));
        QCOMPARE(controller->scheduler()->signal().format, output::PixelFormat::Uyvy8);
        r = call({{"action", "status"}});
        QCOMPARE(r.value("structuredContent").toObject().value("mode").toString(), QStringLiteral("1080p25"));
        r = call({{"action", "start"}, {"interface", "Virtual"}, {"mode", "4320p25"}});
        QVERIFY(r.value("isError").toBool());  // no such mode
        r = call({{"action", "start"}, {"device", "Virtual Ouptut"}});
        QVERIFY(r.value("isError").toBool() && !controller->running());  // a device misnamed is not swapped for another
        QVERIFY(!action->isChecked());
        r = call({{"action", "stop"}});
        QVERIFY(!r.value("isError").toBool() && !controller->running());
        qunsetenv("MONTAGE_VIRTUAL_OUTPUT");
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

// (AI models run on the processor in tests, so results do not depend on the machine's GPU: media/Inference.h.)
[[maybe_unused]] static const bool kCpuInference = (qputenv("MONTAGE_INFERENCE", "cpu"), true);
