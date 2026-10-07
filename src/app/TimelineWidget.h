// Montage — the timeline: tracks, clips, transitions and the editing tools.
#pragma once

#include <QAbstractScrollArea>
#include <QList>
#include <optional>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

class QAction;
class QMimeData;

namespace montage {

class EditorState;

class TimelineWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class Tool { Select, Razor, Ripple, Roll, Slip, Slide, Hand };
    Q_ENUM(Tool)

    explicit TimelineWidget(EditorState* state, QWidget* parent = nullptr);

    Tool tool() const { return tool_; }
    void setTool(Tool t);

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    // Keeps the playhead in view during playback (page-style scrolling).
    void followPlayhead(FrameTime t);

    // Actions shown in the clip context menu (owned by the main window).
    void setClipContextActions(const QList<QAction*>& actions) { clipActions_ = actions; }
    void setEmptyContextActions(const QList<QAction*>& actions) { emptyActions_ = actions; }
    // Where the last context menu was opened (for "Close Gap", "Paste" ...).
    FrameTime contextFrame() const { return contextFrame_; }
    std::optional<TrackRef> contextTrack() const { return contextTrack_; }

    QSize sizeHint() const override { return {900, 320}; }

signals:
    void toolChanged(montage::TimelineWidget::Tool tool);
    void clipActivated(montage::Id clip);  // double-click

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    struct Row {
        TrackRef ref;
        int y = 0;
        int h = 0;
    };
    enum class HitKind { None, Ruler, Header, Body, ClipBody, ClipIn, ClipOut, Transition };
    enum class HeaderButton { None, Target, Visible, Lock, Mute, Solo, Name };
    struct Hit {
        HitKind kind = HitKind::None;
        std::optional<TrackRef> track;
        Id clip = 0;
        Id transition = 0;
        HeaderButton button = HeaderButton::None;
        FrameTime frame = 0;
    };
    enum class DragKind { None, Scrub, Move, Trim, Roll, Slip, Slide, Rubber, Pan };
    struct DragState {
        DragKind kind = DragKind::None;
        QPoint pressPos;
        FrameTime pressFrame = 0;
        FrameTime origEdge = 0;  // trimmed edge position at press time
        std::optional<TrackRef> pressTrack;
        Id clip = 0;
        Id neighbor = 0;
        std::vector<Id> ids;
        edit::Edge edge = edit::Edge::In;
        bool started = false;
        bool insertMode = false;
        bool unlinked = false;
        int hOffsetAtPress = 0;
        int vOffsetAtPress = 0;
        QRect band;
        QString label;  // live readout (e.g. "+00:00:00:12")
    };
    struct Ghost {
        TrackRef track;
        FrameTime start = 0;
        FrameTime duration = 0;
    };

    std::vector<Row> rows() const;
    int contentHeight() const;
    int dividerY() const;
    std::optional<Row> rowAt(int y) const;
    int xForFrame(FrameTime f) const;
    double frameAtX(int x) const;  // fractional
    FrameTime frameRound(int x) const;
    Hit hitTest(const QPoint& pos) const;
    HeaderButton headerButtonAt(const Row& r, const QPoint& pos) const;
    QRect headerButtonRect(const Row& r, HeaderButton b) const;
    void updateScrollBars();
    void zoomAround(int x, double factor);
    FrameTime snapFrame(FrameTime f, const std::vector<Id>& exclude, bool* snapped = nullptr);
    FrameTime snapDelta(const std::vector<Id>& ids, FrameTime delta);

    void paintRuler(QPainter& p);
    void paintHeaders(QPainter& p, const std::vector<Row>& rs);
    void paintClip(QPainter& p, const Row& row, const Clip& c, const QRect& r);
    void paintWaveform(QPainter& p, const Clip& c, const QRect& r, const QColor& col);
    void paintThumbnails(QPainter& p, const Clip& c, const QRect& r);

    void beginDrag(QMouseEvent* e, const Hit& hit);
    void updateDrag(QMouseEvent* e);
    void handleHeaderClick(const Hit& hit);
    std::vector<Ghost> ghostsFor(const QMimeData* mime, const QPoint& pos) const;
    void dropMedia(const QMimeData* mime, const QPoint& pos, bool insertMode);
    void dropEffect(const QString& type, const QPoint& pos);

    EditorState* state_;
    Tool tool_ = Tool::Select;
    double ppf_ = 3.0;  // pixels per frame
    DragState drag_;
    FrameTime snapIndicator_ = -1;
    std::vector<Ghost> ghosts_;
    QList<QAction*> clipActions_;
    QList<QAction*> emptyActions_;
    FrameTime contextFrame_ = 0;
    std::optional<TrackRef> contextTrack_;
    QPoint hoverPos_;
};

}  // namespace montage
