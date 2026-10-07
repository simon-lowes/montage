#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>
#include <algorithm>
#include <iterator>

#include "core/MediaLog.h"

namespace montage::theme {

namespace {
// Colours for core/MediaLog.h's label names, in the same order.
const QColor kLabelColors[] = {
    QColor(),                  QColor(0x8a, 0x5c, 0xc9), QColor(0x5b, 0x6e, 0xd8), QColor(0x2a, 0x9d, 0x8f),
    QColor(0xb2, 0x8d, 0xd8), QColor(0x2f, 0x8f, 0xc9), QColor(0x3d, 0x8a, 0x48), QColor(0xd1, 0x5f, 0x8c),
    QColor(0xe0, 0x93, 0x3c), QColor(0xd8, 0xc2, 0x3a), QColor(0xa8, 0x8a, 0x64), QColor(0xc8, 0x46, 0x46),
};
static_assert(std::size(kLabelColors) == 12);
}  // namespace

QColor labelColor(int index) {
    if (index <= 0 || index >= labelCount()) return QColor();
    return kLabelColors[index];
}

int labelCount() { return std::min(montage::labelCount(), int(std::size(kLabelColors))); }

const char* labelName(int index) { return index <= 0 || index >= labelCount() ? "Default" : montage::labelName(index); }

QFont monoFont(int pointSize) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(pointSize);
    return f;
}

QIcon icon(const char* nameC) {
    const QString name = QString::fromLatin1(nameC);
    QIcon result;
    for (int size : {16, 32, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 16.0, size / 16.0);
        p.setPen(Qt::NoPen);
        p.setBrush(kText);
        auto tri = [&](double x0, double x1, double y0, double y1, bool right) {
            QPainterPath path;
            if (right) {
                path.moveTo(x0, y0);
                path.lineTo(x1, (y0 + y1) / 2);
                path.lineTo(x0, y1);
            } else {
                path.moveTo(x1, y0);
                path.lineTo(x0, (y0 + y1) / 2);
                path.lineTo(x1, y1);
            }
            path.closeSubpath();
            p.drawPath(path);
        };
        if (name == "play") tri(4, 13, 2.5, 13.5, true);
        else if (name == "pause") {
            p.drawRect(QRectF(4, 3, 3, 10));
            p.drawRect(QRectF(9, 3, 3, 10));
        } else if (name == "step-back") {
            p.drawRect(QRectF(3, 3.5, 2, 9));
            tri(6, 13, 3.5, 12.5, false);
        } else if (name == "step-forward") {
            tri(3, 10, 3.5, 12.5, true);
            p.drawRect(QRectF(11, 3.5, 2, 9));
        } else if (name == "to-in" || name == "to-out") {
            bool in = name == "to-in";
            p.setPen(QPen(kText, 1.6));
            p.setBrush(Qt::NoBrush);
            double bx = in ? 4 : 12;
            p.drawLine(QPointF(bx, 3), QPointF(bx, 13));
            p.drawLine(QPointF(bx, 3), QPointF(bx + (in ? 2.5 : -2.5), 3));
            p.drawLine(QPointF(bx, 13), QPointF(bx + (in ? 2.5 : -2.5), 13));
            p.setPen(Qt::NoPen);
            p.setBrush(kText);
            if (in) tri(7, 13, 4.5, 11.5, false);
            else tri(3, 9, 4.5, 11.5, true);
        } else if (name == "mark-in" || name == "mark-out") {
            bool in = name == "mark-in";
            p.setPen(QPen(kText, 1.8));
            double bx = in ? 6 : 10;
            p.drawLine(QPointF(bx, 2.5), QPointF(bx, 13.5));
            p.drawLine(QPointF(bx, 2.5), QPointF(bx + (in ? 4 : -4), 2.5));
            p.drawLine(QPointF(bx, 13.5), QPointF(bx + (in ? 4 : -4), 13.5));
        } else if (name == "save") {
            p.setPen(QPen(kText, 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 1.5, 1.5);
            p.drawRect(QRectF(5, 2.5, 6, 3.5));
            p.drawRect(QRectF(4.5, 8.5, 7, 5));
        }
        result.addPixmap(pm);
    }
    return result;
}

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, kWindow);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, QColor(0x18, 0x19, 0x1c));
    p.setColor(QPalette::AlternateBase, kPanel);
    p.setColor(QPalette::ToolTipBase, kPanelAlt);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kPanelAlt);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::PlaceholderText, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::Text, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, kTextDim);
    app.setPalette(p);
    app.setStyleSheet(R"(
        QMainWindow::separator { background: #16171a; width: 4px; height: 4px; }
        QDockWidget { titlebar-close-icon: none; color: #c9ccd2; }
        QDockWidget::title { background: #24262b; padding: 4px 8px; border-bottom: 1px solid #16171a; }
        QTabBar::tab { background: #24262b; color: #8d929c; padding: 5px 12px; border: none; }
        QTabBar::tab:selected { background: #2c2f35; color: #dcdee3; border-bottom: 2px solid #3d8bff; }
        QToolBar { background: #24262b; border: none; spacing: 2px; padding: 2px; }
        QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
        QToolButton:hover { background: #33363d; }
        QToolButton:checked { background: #2a4a7a; border-color: #3d8bff; }
        QStatusBar { background: #24262b; color: #8d929c; }
        QGroupBox { border: 1px solid #3a3d44; border-radius: 4px; margin-top: 10px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #c9ccd2; }
        QScrollArea { border: none; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
            background: #18191c; border: 1px solid #3a3d44; border-radius: 3px; padding: 2px 4px; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus { border-color: #3d8bff; }
        QSlider::groove:horizontal { height: 4px; background: #3a3d44; border-radius: 2px; }
        QSlider::handle:horizontal { width: 10px; margin: -5px 0; background: #c9ccd2; border-radius: 5px; }
        QMenu { background: #24262b; border: 1px solid #3a3d44; }
        QMenu::item:selected { background: #2a4a7a; }
    )");
}

}  // namespace montage::theme
