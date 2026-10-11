#include "FlowLayout.h"

#include <QStyle>
#include <QWidget>
#include <algorithm>

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), spacing_(spacing) { setContentsMargins(0, 0, 0, 0); }

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

QLayoutItem* FlowLayout::takeAt(int index) { return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr; }

QSize FlowLayout::minimumSize() const {
    // As narrow as the widest item: the rest wrap.
    QSize size;
    for (const QLayoutItem* item : items_)
        if (!item->isEmpty()) size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const {
    // Everything on one row.
    int w = 0, h = 0, n = 0;
    for (const QLayoutItem* item : items_) {
        if (item->isEmpty()) continue;
        w += item->sizeHint().width() + (n++ ? gap(QStyle::PM_LayoutHorizontalSpacing) : 0);
        h = std::max(h, item->sizeHint().height());
    }
    const QMargins m = contentsMargins();
    return QSize(w + m.left() + m.right(), h + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    arrange(rect, false);
}

int FlowLayout::gap(QStyle::PixelMetric metric) const {
    if (spacing_ >= 0) return spacing_;
    const QWidget* w = parentWidget();
    if (!w) return 6;
    const int s = w->style()->pixelMetric(metric, nullptr, w);
    return s >= 0 ? s : 6;
}

int FlowLayout::arrange(const QRect& rect, bool measureOnly) const {
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    const int dx = gap(QStyle::PM_LayoutHorizontalSpacing), dy = gap(QStyle::PM_LayoutVerticalSpacing);
    int x = area.x(), y = area.y(), row = 0;
    for (QLayoutItem* item : items_) {
        if (item->isEmpty()) continue;
        const QSize s = item->sizeHint();
        if (x > area.x() && x + s.width() > area.right() + 1) {  // no room left on this row
            x = area.x();
            y += row + dy;
            row = 0;
        }
        if (!measureOnly) item->setGeometry(QRect(QPoint(x, y), s));
        x += s.width() + dx;
        row = std::max(row, s.height());
    }
    return y + row - rect.y() + m.bottom();
}
