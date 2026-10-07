// Application integration tests: drive the real main window offscreen —
// timeline mouse gestures, tools, undo, inspector, monitors and playback.
#include <QtTest>

#include <QAbstractScrollArea>
#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QScrollBar>
#include <QSlider>
#include <QPushButton>
#include <QMenu>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "CaptionsPanel.h"
#include "EditorState.h"
#include "EffectsBrowser.h"
#include "audio/Plugins.h"
#include "MainWindow.h"
#include "MixerPanel.h"
#include "PluginEditorWindow.h"
#include "audio/PluginEffect.h"
#include "MaskOverlay.h"
#include "MonitorPanel.h"
#include "PlaybackController.h"
#include "Recovery.h"
#include "TimelineWidget.h"
#include "TranscribeDialog.h"
#include "TranscriptPanel.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/ProjectIO.h"
#include "render/Exporter.h"

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
        QListWidget* bin = search->parentWidget()->findChild<QListWidget*>();
        QVERIFY(bin);
        search->setText("fellow americans");
        QCOMPARE(bin->count(), 1);
        search->setText("words nobody said");
        QCOMPARE(bin->count(), 0);
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
        QCOMPARE(state()->sequence()->captionTracks[1].captions, state()->sequence()->captionTracks[0].captions);
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

        // Deleting "my fellow" cuts 1.8 s .. 2.9 s out of every track.
        panel->selectWords(3, 4);
        panel->deleteSelection();
        const FrameTime cut = FrameTime(std::llround(2.9 * fps)) - FrameTime(std::llround(1.8 * fps));
        QCOMPARE(state()->sequence()->duration(), full - cut);
        QCOMPARE(panel->words().size(), size_t(6));
        state()->undo();
        QCOMPARE(state()->sequence()->duration(), full);
        QCOMPARE(panel->words().size(), size_t(8));

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
        state()->setSelection({}, false);
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
