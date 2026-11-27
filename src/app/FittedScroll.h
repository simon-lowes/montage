// Montage — a dialog's rows in a scroll area as tall as they are, up to what the screen has room for, so that on a
// screen too short for them (1280 by 800) they scroll instead of running off it.
#pragma once

#include <QEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <algorithm>

namespace montage {

class FittedScroll : public QScrollArea {
public:
    // `rows` is what it holds (taken over); it scrolls up and down only, without a frame.
    FittedScroll(QWidget* rows, QWidget* parent) : QScrollArea(parent) {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWidget(rows);
    }
    QSize sizeHint() const override {
        if (!widget()) return QScrollArea::sizeHint();
        const QSize inner = widget()->sizeHint();
        const QScreen* on = screen();
        const int room = on ? std::max(480, on->availableGeometry().height() - 160) : inner.height();
        const int bar = inner.height() > room ? verticalScrollBar()->sizeHint().width() : 0;
        return {inner.width() + 2 * frameWidth() + bar, std::min(inner.height(), room) + 2 * frameWidth()};
    }
    // (never narrower than its rows: they are not cut off at the side)
    QSize minimumSizeHint() const override {
        const QSize least = QScrollArea::minimumSizeHint();
        if (!widget()) return least;
        return {widget()->minimumSizeHint().width() + 2 * frameWidth() + verticalScrollBar()->sizeHint().width(), least.height()};
    }

protected:
    // (rows shown or hidden change what it holds: the dialog's layout asks again)
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == widget() && e->type() == QEvent::LayoutRequest) updateGeometry();
        return QScrollArea::eventFilter(o, e);
    }
};

}  // namespace montage
