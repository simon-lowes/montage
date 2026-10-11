// Montage — the bin's Board view (core/Storyboard.h; Premiere's Freeform view, Avid's Frame view): the shown media as
// thumbnails placed anywhere, where each was left (MediaItem::board, saved with the project). Selections drag out, and
// assemble, in storyboard order: rows top to bottom, left to right.
#pragma once

#include <QListView>
#include <functional>
#include <vector>

#include "core/Model.h"

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

    // Puts every thumbnail where its media sits on the board (media not placed after the rest).
    void applyBoard();
    // Saves where the thumbnails are now as the media's board spots; one undo step (false if nothing moved).
    bool saveBoard(const QString& label);
    // Where a thumbnail is, in the board's pixels, and moving one there (saved by saveBoard).
    QPoint thumbnailAt(const QModelIndex& index) const { return rectForIndex(index).topLeft(); }
    void moveThumbnail(const QModelIndex& index, const QPoint& to) { setPositionForIndex(to, index); }
    std::vector<Id> shownInOrder() const;     // storyboard order
    std::vector<Id> selectedInOrder() const;  // the selection, in storyboard order
    int columns() const;                      // how many cells fit across
    void doItemsLayout() override;            // (then the board's spots again)

signals:
    void thumbnailWidthChanged(int width);  // Ctrl+wheel

protected:
    void dropEvent(QDropEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    std::vector<Id> shown() const;
    EditorState* state_;
    std::function<Id(const QModelIndex&)> idOf_;
    int thumbWidth_ = 0;
    bool applying_ = false;
};

}  // namespace montage
