#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QSettings>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <iterator>

#include "Settings.h"
#include "core/MediaLog.h"

namespace montage::theme {

namespace {
struct Colours {
    QColor window, panel, panelAlt, border, text, textDim, accent, playhead, inOut, snap, videoClip, audioClip,
        generatorClip, compoundClip, disabledClip, selection, transition, meterGreen, meterYellow, meterRed, base, hover,
        checked, separator;
};
// The dark theme, and the high-contrast one: black grounds, white text, yellow for focus and selection, clips dark
// enough for their white names (7:1 or more).
const Colours kDark{{0x1c, 0x1d, 0x21}, {0x24, 0x26, 0x2b}, {0x2c, 0x2f, 0x35}, {0x3a, 0x3d, 0x44}, {0xdc, 0xde, 0xe3},
                    {0x9a, 0xa0, 0xaa}, {0x3d, 0x8b, 0xff}, {0xff, 0x4d, 0x4d}, {0x3d, 0x8b, 0xff, 50}, {0xff, 0xd0, 0x40},
                    {0x3f, 0x6f, 0xb5}, {0x3c, 0x9a, 0x6e}, {0x8a, 0x5c, 0xc9}, {0xc9, 0x7c, 0x3a}, {0x55, 0x58, 0x5e},
                    {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff, 70}, {0x3c, 0xc8, 0x6e}, {0xf0, 0xc8, 0x3c}, {0xf0, 0x46, 0x46},
                    {0x18, 0x19, 0x1c}, {0x33, 0x36, 0x3d}, {0x2a, 0x4a, 0x7a}, {0x16, 0x17, 0x1a}};
const Colours kHighContrast{{0x00, 0x00, 0x00}, {0x0a, 0x0a, 0x0a}, {0x1a, 0x1a, 0x1a}, {0xc8, 0xc8, 0xc8},
                            {0xff, 0xff, 0xff}, {0xe6, 0xe6, 0xe6}, {0xff, 0xd4, 0x00}, {0xff, 0x5a, 0x5a},
                            {0xff, 0xd4, 0x00, 70}, {0x00, 0xe5, 0xff}, {0x00, 0x40, 0x99}, {0x00, 0x66, 0x3a},
                            {0x5a, 0x2a, 0x99}, {0x7a, 0x3f, 0x00}, {0x33, 0x33, 0x33}, {0xff, 0xff, 0x00},
                            {0xff, 0xff, 0xff, 110}, {0x30, 0xff, 0x70}, {0xff, 0xe0, 0x30}, {0xff, 0x50, 0x50},
                            {0x00, 0x00, 0x00}, {0x2a, 0x2a, 0x2a}, {0x4d, 0x40, 0x00}, {0xc8, 0xc8, 0xc8}};
bool gHighContrast = false;
double gTextScale = 1;
double gBasePointSize = 0;  // the application font's own size, before scaling

const Colours& current() { return gHighContrast ? kHighContrast : kDark; }


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

void setHighContrast(bool on) {
    gHighContrast = on;
    const Colours& c = current();
    kWindow = c.window, kPanel = c.panel, kPanelAlt = c.panelAlt, kBorder = c.border, kText = c.text, kTextDim = c.textDim;
    kAccent = c.accent, kPlayhead = c.playhead, kInOut = c.inOut, kSnap = c.snap, kVideoClip = c.videoClip;
    kAudioClip = c.audioClip, kGeneratorClip = c.generatorClip, kCompoundClip = c.compoundClip, kDisabledClip = c.disabledClip;
    kSelection = c.selection, kTransition = c.transition, kMeterGreen = c.meterGreen, kMeterYellow = c.meterYellow;
    kMeterRed = c.meterRed;
}

bool highContrast() { return gHighContrast; }

void setTextScale(double scale) { gTextScale = std::clamp(scale, 1.0, 2.0); }

double textScale() { return gTextScale; }

void loadSettings() {
    QSettings settings = appSettings();
    setHighContrast(settings.value(QStringLiteral("ui/highContrast"), false).toBool());
    setTextScale(settings.value(QStringLiteral("ui/textScale"), 1.0).toDouble());
}

void saveSettings() {
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("ui/highContrast"), gHighContrast);
    settings.setValue(QStringLiteral("ui/textScale"), gTextScale);
}

double contrastRatio(const QColor& a, const QColor& b) {
    auto luminance = [](const QColor& c) {
        auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
    };
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QFont monoFont(int pointSize) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(pointSize * gTextScale);
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

namespace {
// Fusion, with tab names never shortened to "Tra…": a crowded dock shows scroll arrows instead.
class Style : public QProxyStyle {
public:
    Style() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* ret) const override {
        if (hint == SH_TabBar_ElideMode) return Qt::ElideNone;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }
};
}  // namespace

void apply(QApplication& app) {
    // (Once: with a style sheet set the application's style is a wrapper round it, so it cannot be asked.)
    static bool styled = false;
    if (!styled) {
        app.setStyle(new Style);
        styled = true;
    }
    const Colours& c = current();
    QPalette p;
    p.setColor(QPalette::Window, kWindow);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, kPanel);
    p.setColor(QPalette::ToolTipBase, kPanelAlt);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kPanelAlt);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, gHighContrast ? QColor(Qt::black) : QColor(Qt::white));
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::PlaceholderText, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::Text, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, kTextDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, kTextDim);
    app.setPalette(p);
    QFont font = app.font();
    if (gBasePointSize <= 0) gBasePointSize = font.pointSizeF() > 0 ? font.pointSizeF() : 9;
    font.setPointSizeF(gBasePointSize * gTextScale);
    app.setFont(font);
    // (The focused field's border is the accent, and in high contrast twice as thick: where the keyboard is shows.)
    const QString focusWidth = gHighContrast ? QStringLiteral("2px") : QStringLiteral("1px");
    QString sheet = QStringLiteral(R"(
        QMainWindow::separator { background: @sep; width: 4px; height: 4px; }
        QDockWidget { titlebar-close-icon: none; color: @text; }
        QDockWidget::title { background: @panel; padding: 4px 8px; border-bottom: 1px solid @sep; }
        QTabBar::tab { background: @panel; color: @dim; padding: 5px 12px; border: none; }
        QTabBar::tab:selected { background: @alt; color: @text; border-bottom: 2px solid @accent; }
        QToolBar { background: @panel; border: none; spacing: 2px; padding: 2px; }
        QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
        QToolButton:hover { background: @hover; }
        QToolButton:checked { background: @checked; border-color: @accent; }
        QToolButton:focus { border: @focus solid @accent; }
        QStatusBar { background: @panel; color: @dim; }
        QGroupBox { border: 1px solid @border; border-radius: 4px; margin-top: 10px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: @text; }
        QScrollArea { border: none; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
            background: @base; border: 1px solid @border; border-radius: 3px; padding: 2px 4px; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border: @focus solid @accent; }
        QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
        QSlider::handle:horizontal { width: 10px; margin: -5px 0; background: @text; border-radius: 5px; }
        QMenu { background: @panel; border: 1px solid @border; }
        QMenu::item:selected { background: @checked; }
    )");
    const std::pair<const char*, QColor> names[] = {{"@sep", c.separator}, {"@text", kText},   {"@dim", kTextDim},
                                                    {"@panel", kPanel},    {"@alt", kPanelAlt}, {"@accent", kAccent},
                                                    {"@hover", c.hover},   {"@checked", c.checked}, {"@border", kBorder},
                                                    {"@base", c.base}};
    for (const auto& [name, colour] : names) sheet.replace(QLatin1String(name), colour.name());
    sheet.replace(QLatin1String("@focus"), focusWidth);
    app.setStyleSheet(sheet);
    for (QWidget* w : QApplication::allWidgets()) w->update();
}

}  // namespace montage::theme
