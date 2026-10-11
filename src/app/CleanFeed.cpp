#include "CleanFeed.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPainter>

namespace montage {

CleanFeedWindow::CleanFeedWindow(QWidget* parent) : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setWindowTitle(tr("Montage — Video Output"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::BlankCursor);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(QString());
}

void CleanFeedWindow::setFrame(const QImage& image) {
    if (image.isNull()) return;
    frame_ = image;
    update();
}

QRect CleanFeedWindow::pictureRect() const {
    if (frame_.isNull()) return {};
    QSize s = frame_.size();
    s.scale(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void CleanFeedWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(pictureRect(), frame_);
}

void CleanFeedWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    // Everything else (space, J/K/L, arrows) is for the editor: pass it to the main window.
    if (parentWidget()) QCoreApplication::sendEvent(parentWidget()->window(), e);
    else QWidget::keyPressEvent(e);
}

void CleanFeedWindow::mouseDoubleClickEvent(QMouseEvent*) { close(); }

void CleanFeedWindow::closeEvent(QCloseEvent* e) {
    emit closed();
    QWidget::closeEvent(e);
}

}  // namespace montage
