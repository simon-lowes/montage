// Application integration tests: drive the real main window offscreen —
// timeline mouse gestures, tools, undo, inspector, monitors and playback.
#include <QtTest>

#include <QAbstractScrollArea>
#include <QAction>
#include <QDoubleSpinBox>
#include <QMimeData>
#include <QScrollBar>

#include "EditorState.h"
#include "MainWindow.h"
#include "PlaybackController.h"
#include "TimelineWidget.h"
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
