// Montage — colours and styling shared by the custom-painted widgets: the dark theme, or a high-contrast one (text at
// 7:1 or more against what it is drawn on), and the text scaled for reading at a distance.
#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>

class QApplication;

namespace montage::theme {

inline QColor kWindow{0x1c, 0x1d, 0x21};
inline QColor kPanel{0x24, 0x26, 0x2b};
inline QColor kPanelAlt{0x2c, 0x2f, 0x35};
inline QColor kBorder{0x3a, 0x3d, 0x44};
inline QColor kText{0xdc, 0xde, 0xe3};
inline QColor kTextDim{0x8d, 0x92, 0x9c};
inline QColor kAccent{0x3d, 0x8b, 0xff};
inline QColor kPlayhead{0xff, 0x4d, 0x4d};
inline QColor kInOut{0x3d, 0x8b, 0xff, 50};
inline QColor kSnap{0xff, 0xd0, 0x40};
inline QColor kVideoClip{0x3f, 0x6f, 0xb5};
inline QColor kAudioClip{0x3c, 0x9a, 0x6e};
inline QColor kGeneratorClip{0x8a, 0x5c, 0xc9};
inline QColor kCompoundClip{0xc9, 0x7c, 0x3a};
inline QColor kDisabledClip{0x55, 0x58, 0x5e};
inline QColor kSelection{0xff, 0xff, 0xff};
inline QColor kTransition{0xff, 0xff, 0xff, 70};
inline QColor kMeterGreen{0x3c, 0xc8, 0x6e};
inline QColor kMeterYellow{0xf0, 0xc8, 0x3c};
inline QColor kMeterRed{0xf0, 0x46, 0x46};

// Clip label colours (index = Clip::colorLabel, 0 = default by kind).
QColor labelColor(int index);
int labelCount();
const char* labelName(int index);

// Applies the dark Fusion palette and stylesheet to the application, with the theme's colours and text scale; again
// after either changes (the widgets then repaint in the new colours).
void apply(QApplication& app);

// The high-contrast colours (true) or the usual ones; apply() puts them on the application.
void setHighContrast(bool on);
bool highContrast();
// Text size, 1 (as designed) to 2 (twice as large).
void setTextScale(double scale);
double textScale();
// The colours and scale kept in the settings ("ui/highContrast", "ui/textScale"): read (at startup) and saved.
void loadSettings();
void saveSettings();

// WCAG 2's contrast ratio of two colours, 1 to 21 (alpha ignored).
double contrastRatio(const QColor& a, const QColor& b);

// The window's monospaced font at a point size (scaled with the text).
QFont monoFont(int pointSize = 9);

// Vector transport / UI icons drawn in the theme colour:
// "play", "pause", "step-back", "step-forward", "to-in", "to-out", "mark-in", "mark-out", "save".
QIcon icon(const char* name);

}  // namespace montage::theme
