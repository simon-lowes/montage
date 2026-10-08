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

    // Bakes an audio clip's effects into a new audio file and points the clip at it (undoable).
    bool renderAndReplace(montage::Id clip, QString* error = nullptr);

    // Lines over clips for their volume (audio clips) and opacity (video clips),
    // with their keyframes: drag the line to change it, Ctrl/Cmd-click it to add
    // a keyframe, drag keyframes, Alt-click one to delete it.
    void setShowVolumeLines(bool on);
    bool showVolumeLines() const { return showVolume_; }
    void setShowOpacityLines(bool on);
    bool showOpacityLines() const { return showOpacity_; }
    // The render bar under the ruler: the frame ranges ([first, end)) whose
    // rendered previews are cached show green.
    void setRenderedRanges(std::vector<std::pair<FrameTime, FrameTime>> ranges);
    const std::vector<std::pair<FrameTime, FrameTime>>& renderedRanges() const { return rendered_; }
    // Where a clip's line is drawn (widget coordinates), for tests; empty if it is not shown.
    QRect lineBand(montage::Id clip) const;
    int lineY(montage::Id clip, montage::FrameTime local) const;

signals:
    void toolChanged(montage::TimelineWidget::Tool tool);
    void clipActivated(montage::Id clip);  // double-click
    void captionActivated(montage::Id track, int index);  // double-click on a caption
    // While an edit is trimmed, rolled, slipped or slid: the frames either side of it
    // (timeline frames, -1 for none) and their labels, for a two-up view; then the end.
    void trimViewChanged(montage::FrameTime left, montage::FrameTime right, const QString& leftLabel, const QString& rightLabel);
    void trimViewEnded();

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
    void emitTrimView();
    struct Row {
        TrackRef ref;
        int y = 0;
        int h = 0;
    };
    enum class HitKind { None, Ruler, Header, Body, ClipBody, ClipIn, ClipOut, Transition, CaptionLane, Caption, CaptionIn, CaptionOut };
    enum class HeaderButton { None, Target, Visible, Lock, Mute, Solo, Name };
    struct Hit {
        HitKind kind = HitKind::None;
        std::optional<TrackRef> track;
        Id clip = 0;
        Id transition = 0;
        HeaderButton button = HeaderButton::None;
        FrameTime frame = 0;
        Id captionTrack = 0;
        int caption = -1;
    };
    enum class DragKind { None, Scrub, Move, Trim, Roll, Slip, Slide, Rubber, Pan, CaptionMove, CaptionIn, CaptionOut, Line, LineKey };
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
        Id captionTrack = 0;
        int caption = -1;
        FrameTime key = -1;     // the keyframe dragged (clip-local frame)
        double lineAtPress = 0;  // the line's value under the press
    };
    // A clip's line: which fixed parameter it shows and its range.
    struct Lane {
        Effect Clip::*fixed;
        const char* param;
        double def, lo, hi;
        bool gain;  // drawn on the volume scale (core/KeyframeEdit.h)
    };
    struct LaneHit {
        Id clip = 0;
        Lane lane{};
        FrameTime local = 0;  // clip-local frame under the pointer
        FrameTime key = -1;   // a keyframe under the pointer
        bool onLine = false;
    };
    struct Ghost {
        TrackRef track;
        FrameTime start = 0;
        FrameTime duration = 0;
    };

    int captionLanesHeight() const;
    void paintCaptionLanes(QPainter& p);
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
    std::optional<Lane> laneFor(const Clip& c, TrackKind kind) const;
    static QRect laneBand(const QRect& clipRect);
    static int laneY(const Lane& lane, const QRect& band, double v);
    static double laneValue(const Lane& lane, const QRect& band, int y);
    std::optional<LaneHit> laneHit(const QPoint& pos) const;
    bool clipRect(Id clip, QRect& r, TrackKind* kind = nullptr) const;
    bool laneOf(Id clip, Lane& lane, QRect& band) const;
    void paintLane(QPainter& p, const Clip& c, TrackKind kind, const QRect& r);
    bool beginLaneDrag(QMouseEvent* e, const LaneHit& h);
    void laneMenu(QMenu& menu, const LaneHit& h);

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
    Id selectedCaptionTrack_ = 0;  // the caption last clicked
    int selectedCaption_ = -1;
    FrameTime snapIndicator_ = -1;
    std::vector<Ghost> ghosts_;
    QList<QAction*> clipActions_;
    QList<QAction*> emptyActions_;
    FrameTime contextFrame_ = 0;
    std::optional<TrackRef> contextTrack_;
    QPoint hoverPos_;
    bool showVolume_ = true;
    bool showOpacity_ = false;
    std::vector<std::pair<FrameTime, FrameTime>> rendered_;
};

}  // namespace montage
