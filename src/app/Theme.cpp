#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace montage::theme {

namespace {
struct Label {
    const char* name;
    QColor color;
};
const Label kLabels[] = {
    {"Default", QColor()},           {"Violet", QColor(0x8a, 0x5c, 0xc9)}, {"Iris", QColor(0x5b, 0x6e, 0xd8)},
    {"Caribbean", QColor(0x2a, 0x9d, 0x8f)}, {"Lavender", QColor(0xb2, 0x8d, 0xd8)}, {"Cerulean", QColor(0x2f, 0x8f, 0xc9)},
    {"Forest", QColor(0x3d, 0x8a, 0x48)}, {"Rose", QColor(0xd1, 0x5f, 0x8c)},     {"Mango", QColor(0xe0, 0x93, 0x3c)},
    {"Yellow", QColor(0xd8, 0xc2, 0x3a)}, {"Tan", QColor(0xa8, 0x8a, 0x64)},      {"Red", QColor(0xc8, 0x46, 0x46)},
};
}  // namespace

QColor labelColor(int index) {
    if (index <= 0 || index >= labelCount()) return QColor();
    return kLabels[index].color;
}

int labelCount() { return int(sizeof(kLabels) / sizeof(kLabels[0])); }

const char* labelName(int index) {
    if (index < 0 || index >= labelCount()) return kLabels[0].name;
    return kLabels[index].name;
}

QFont monoFont(int pointSize) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(pointSize);
    return f;
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
