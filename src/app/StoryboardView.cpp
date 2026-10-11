#include "StoryboardView.h"

#include <QDropEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "MediaBinModel.h"
#include "core/Storyboard.h"

namespace montage {

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

std::vector<Id> StoryboardView::shown() const {
    std::vector<Id> ids;
    if (!model()) return ids;
    for (int r = 0; r < model()->rowCount(); ++r) ids.push_back(idOf_(model()->index(r, 0)));
    return ids;
}

std::vector<Id> StoryboardView::shownInOrder() const { return storyboardOrder(state_->project(), shown()); }

std::vector<Id> StoryboardView::selectedInOrder() const {
    std::vector<Id> picked;
    for (const QModelIndex& i : selectionModel()->selectedIndexes())
        if (i.column() == 0) picked.push_back(idOf_(i));
    return storyboardOrder(state_->project(), picked);
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

bool StoryboardView::saveBoard(const QString& label) {
    const QSize cell = cellSize();
    std::map<Id, BoardSpot> now;
    for (int r = 0; model() && r < model()->rowCount(); ++r) {
        const QPoint at = rectForIndex(model()->index(r, 0)).topLeft();
        // Cells, to a hundredth (nothing placed above or left of the board's corner).
        auto cells = [](int px, int size) { return std::max(0.0, std::round(double(px) / size * 100.0) / 100.0); };
        now[idOf_(model()->index(r, 0))] = {cells(at.x(), cell.width()), cells(at.y(), cell.height()), true};
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

void StoryboardView::dropEvent(QDropEvent* e) {
    const bool moving = e->source() == this;
    QListView::dropEvent(e);
    if (moving) saveBoard(tr("Arrange Storyboard"));
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
