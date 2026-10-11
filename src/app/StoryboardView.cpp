#include "StoryboardView.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <set>

#include "EditorState.h"
#include "MediaBinModel.h"
#include "core/Storyboard.h"

namespace montage {

namespace {

const char* kBoardMime = "application/x-montage-board";  // marks a drag as the board's own (moving thumbnails)

// The bin's thumbnails are made at one size: drawn at the board's.
class BoardDelegate : public QStyledItemDelegate {
public:
    explicit BoardDelegate(QListView* view) : QStyledItemDelegate(view), view_(view) {}

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QVariant d = index.data(Qt::DecorationRole);
        if (d.canConvert<QPixmap>() && !d.value<QPixmap>().isNull()) {
            option->icon = QIcon(d.value<QPixmap>().scaled(view_->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            option->decorationSize = view_->iconSize();
        }
    }

private:
    QListView* view_;
};

}  // namespace

StoryboardView::StoryboardView(EditorState* state, std::function<Id(const QModelIndex&)> idOf, QWidget* parent)
    : QListView(parent), state_(state), idOf_(std::move(idOf)) {
    setViewMode(QListView::IconMode);
    setMovement(QListView::Free);
    setResizeMode(QListView::Fixed);  // (Adjust would lay them out again on every resize)
    setLayoutMode(QListView::SinglePass);
    setUniformItemSizes(true);
    setWordWrap(true);
    setTextElideMode(Qt::ElideMiddle);
    setSelectionRectVisible(true);
    setItemDelegate(new BoardDelegate(this));
    setThumbnailWidth(MediaBinModel::kThumbW);
}

void StoryboardView::setThumbnailWidth(int width) {
    thumbWidth_ = std::clamp(width, 64, 480);
    setIconSize(QSize(thumbWidth_, thumbWidth_ * MediaBinModel::kThumbH / MediaBinModel::kThumbW));
    setGridSize(cellSize());
    applyBoard();
}

QSize StoryboardView::cellSize() const {
    return {thumbWidth_ + 16, thumbWidth_ * MediaBinModel::kThumbH / MediaBinModel::kThumbW + 40};
}

int StoryboardView::columns() const { return std::max(1, viewport()->width() / std::max(1, cellSize().width())); }

QPoint StoryboardView::contentsPos(const QPoint& viewportPos) const {
    return viewportPos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

std::vector<Id> StoryboardView::shown() const {
    std::vector<Id> ids;
    if (!model()) return ids;
    for (int r = 0; r < model()->rowCount(); ++r) ids.push_back(idOf_(model()->index(r, 0)));
    return ids;
}

std::vector<Id> StoryboardView::shownInOrder() const { return storyboardOrder(state_->project(), shown()); }

QModelIndexList StoryboardView::selectedThumbnails() const {
    QModelIndexList out;
    if (!selectionModel()) return out;
    std::set<int> rows;
    for (const QModelIndex& i : selectionModel()->selectedIndexes()) rows.insert(i.row());  // (whole rows are selected)
    for (int r : rows) out << model()->index(r, 0);
    return out;
}

std::vector<Id> StoryboardView::selectedInOrder() const {
    std::vector<Id> picked;  // in the board's row order, which media not yet placed keep
    for (const QModelIndex& i : selectedThumbnails()) picked.push_back(idOf_(i));
    return storyboardOrder(state_->project(), picked);
}

QMimeData* StoryboardView::dragData() const {
    const QModelIndexList picked = selectedThumbnails();
    if (picked.isEmpty()) return nullptr;
    std::map<Id, QModelIndex> byId;
    for (const QModelIndex& i : picked) byId[idOf_(i)] = i;
    QModelIndexList ordered;
    for (Id id : selectedInOrder()) ordered << byId[id];
    QMimeData* data = model()->mimeData(ordered);
    if (data) data->setData(kBoardMime, QByteArray::number(quintptr(this)));
    return data;
}

bool StoryboardView::ownDrag(const QMimeData* data) const {
    return data && data->data(kBoardMime) == QByteArray::number(quintptr(this));
}

void StoryboardView::applyBoard() {
    if (applying_ || !model()) return;
    applying_ = true;
    const QSize cell = cellSize();
    const std::vector<Id> ids = shown();
    const std::map<Id, BoardSpot> spots = boardLayout(state_->project(), ids, columns());
    for (int r = 0; r < int(ids.size()); ++r)
        if (const auto it = spots.find(ids[size_t(r)]); it != spots.end())
            setPositionForIndex(QPoint(int(std::lround(it->second.x * cell.width())), int(std::lround(it->second.y * cell.height()))),
                                model()->index(r, 0));
    applying_ = false;
}

bool StoryboardView::saveBoard(const QString& label, const std::vector<Id>& moved) {
    if (!model() || !arrangeable_) return false;
    const QSize cell = cellSize();
    const std::set<Id> movedSet(moved.begin(), moved.end());
    std::map<Id, BoardSpot> now;
    for (int r = 0; r < model()->rowCount(); ++r) {
        const QModelIndex i = model()->index(r, 0);
        const Id id = idOf_(i);
        const MediaItem* m = state_->project().findMedia(id);
        if (!m || (!movedSet.count(id) && m->board.placed)) continue;  // (the moved, and the not yet placed where they show)
        const QPoint at = thumbnailAt(i);
        // Cells, to a hundredth (nothing above or left of the board's corner).
        auto cells = [](int px, int size) { return std::max(0.0, std::round(double(px) / size * 100.0) / 100.0); };
        now[id] = {cells(at.x(), cell.width()), cells(at.y(), cell.height()), true};
    }
    return state_->edit(label, [&now](Project& p, Sequence&) {
        bool changed = false;
        for (const auto& [id, spot] : now)
            if (MediaItem* m = p.findMedia(id); m && !(m->board == spot)) m->board = spot, changed = true;
        return changed;
    });
}

void StoryboardView::doItemsLayout() {
    QListView::doItemsLayout();
    applyBoard();  // (a layout puts every thumbnail back in a grid)
}

void StoryboardView::mousePressEvent(QMouseEvent* e) {
    pressPos_ = contentsPos(e->position().toPoint());
    QListView::mousePressEvent(e);
}

void StoryboardView::startDrag(Qt::DropActions actions) {
    // Its own drag (QListView's would move every selected column, and its order is the clicks'): the media in
    // storyboard order, marked so a drop here moves them.
    QMimeData* data = dragData();
    if (!data) return;
    auto* drag = new QDrag(this);
    drag->setMimeData(data);
    const QModelIndexList picked = selectedThumbnails();
    const QPixmap thumb = picked.value(0).data(Qt::DecorationRole).value<QPixmap>();
    if (!thumb.isNull()) {
        drag->setPixmap(thumb.scaled(iconSize() / 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(drag->pixmap().width() / 2, drag->pixmap().height() / 2));
    }
    drag->exec(actions | Qt::CopyAction, Qt::CopyAction);  // (nothing is removed, whatever the target answers)
}

void StoryboardView::dragEnterEvent(QDragEnterEvent* e) {
    if (ownDrag(e->mimeData())) {
        if (!arrangeable_) return e->ignore();
        e->setDropAction(Qt::MoveAction);
        return e->accept();
    }
    QListView::dragEnterEvent(e);
}

void StoryboardView::dragMoveEvent(QDragMoveEvent* e) {
    if (ownDrag(e->mimeData())) {
        if (!arrangeable_) return e->ignore();
        e->setDropAction(Qt::MoveAction);
        return e->accept();
    }
    QListView::dragMoveEvent(e);
}

void StoryboardView::dropEvent(QDropEvent* e) {
    if (!ownDrag(e->mimeData())) return QListView::dropEvent(e);
    if (!arrangeable_) return e->ignore();
    // The selection moved by how far the pointer went from where it was pressed.
    const QPoint delta = contentsPos(e->position().toPoint()) - pressPos_;
    std::vector<Id> moved;
    for (const QModelIndex& i : selectedThumbnails()) {
        const QPoint to = thumbnailAt(i) + delta;
        setPositionForIndex(QPoint(std::max(0, to.x()), std::max(0, to.y())), i);
        moved.push_back(idOf_(i));
    }
    e->setDropAction(Qt::MoveAction);
    e->accept();
    saveBoard(tr("Arrange Storyboard"), moved);
}

void StoryboardView::resizeEvent(QResizeEvent* e) {
    QListView::resizeEvent(e);
    applyBoard();  // media not placed yet fill rows as wide as the view
}

void StoryboardView::wheelEvent(QWheelEvent* e) {
    // Ctrl+wheel resizes the thumbnails (the arrangement scales with them).
    if (e->modifiers() & Qt::ControlModifier) {
        setThumbnailWidth(thumbWidth_ + (e->angleDelta().y() > 0 ? 16 : -16));
        emit thumbnailWidthChanged(thumbWidth_);
        e->accept();
        return;
    }
    QListView::wheelEvent(e);
}

}  // namespace montage
