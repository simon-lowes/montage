// Montage — the bin's Board view (core/Storyboard.h; Premiere's Freeform view, Avid's Frame view): the shown media as
// thumbnails placed anywhere, where each was left (MediaItem::board, saved with the project). Selections drag out, and
// assemble, in storyboard order: rows top to bottom, left to right.
#pragma once

#include <QListView>
#include <functional>
#include <vector>

#include "core/Model.h"

class QMimeData;

namespace montage {

class EditorState;

class StoryboardView : public QListView {
    Q_OBJECT
public:
    // `idOf` names the media an index of the view's model shows.
    StoryboardView(EditorState* state, std::function<Id(const QModelIndex&)> idOf, QWidget* parent = nullptr);

    // Thumbnail size (the board scales with it: spots are in cells).
    void setThumbnailWidth(int width);
    int thumbnailWidth() const { return thumbWidth_; }
    QSize cellSize() const;

    // Whether thumbnails can be moved (not while a search or smart bin shows a selection of media from other bins).
    void setArrangeable(bool on) { arrangeable_ = on; }
    bool arrangeable() const { return arrangeable_; }

    // Puts every thumbnail where its media sits on the board (media not placed after the rest).
    void applyBoard();
    // Saves where the thumbnails of `moved` are now as their media's spots, pinning the shown media not yet placed
    // where they show (so they do not move); one undo step, false if nothing changed.
    bool saveBoard(const QString& label, const std::vector<Id>& moved);
    // Where a thumbnail is, in the board's pixels, and moving one there (saved by saveBoard).
    QPoint thumbnailAt(const QModelIndex& index) const { return rectForIndex(index).topLeft(); }
    void moveThumbnail(const QModelIndex& index, const QPoint& to) { setPositionForIndex(to, index); }
    std::vector<Id> shownInOrder() const;     // storyboard order
    std::vector<Id> selectedInOrder() const;  // the selection, in storyboard order
    int columns() const;                      // how many cells fit across
    void doItemsLayout() override;            // (then the board's spots again)
    // What a drag of the selection carries: its media in storyboard order, marked as this board's.
    QMimeData* dragData() const;

signals:
    void thumbnailWidthChanged(int width);  // Ctrl+wheel

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    std::vector<Id> shown() const;
    QModelIndexList selectedThumbnails() const;  // one index per selected row, in the board's row order
    bool ownDrag(const QMimeData* data) const;
    QPoint contentsPos(const QPoint& viewportPos) const;
    EditorState* state_;
    std::function<Id(const QModelIndex&)> idOf_;
    int thumbWidth_ = 0;
    bool applying_ = false;
    bool arrangeable_ = true;
    QPoint pressPos_;  // where the last press was, in the board's pixels (a drag moves by the distance from it)
};

}  // namespace montage
