// Montage — colours and styling shared by the custom-painted widgets.
#pragma once

#include <QColor>
#include <QFont>

class QApplication;

namespace montage::theme {

inline const QColor kWindow{0x1c, 0x1d, 0x21};
inline const QColor kPanel{0x24, 0x26, 0x2b};
inline const QColor kPanelAlt{0x2c, 0x2f, 0x35};
inline const QColor kBorder{0x3a, 0x3d, 0x44};
inline const QColor kText{0xdc, 0xde, 0xe3};
inline const QColor kTextDim{0x8d, 0x92, 0x9c};
inline const QColor kAccent{0x3d, 0x8b, 0xff};
inline const QColor kPlayhead{0xff, 0x4d, 0x4d};
inline const QColor kInOut{0x3d, 0x8b, 0xff, 50};
inline const QColor kSnap{0xff, 0xd0, 0x40};
inline const QColor kVideoClip{0x3f, 0x6f, 0xb5};
inline const QColor kAudioClip{0x3c, 0x9a, 0x6e};
inline const QColor kGeneratorClip{0x8a, 0x5c, 0xc9};
inline const QColor kCompoundClip{0xc9, 0x7c, 0x3a};
inline const QColor kDisabledClip{0x55, 0x58, 0x5e};
inline const QColor kSelection{0xff, 0xff, 0xff};
inline const QColor kTransition{0xff, 0xff, 0xff, 70};
inline const QColor kMeterGreen{0x3c, 0xc8, 0x6e};
inline const QColor kMeterYellow{0xf0, 0xc8, 0x3c};
inline const QColor kMeterRed{0xf0, 0x46, 0x46};

// Clip label colours (index = Clip::colorLabel, 0 = default by kind).
QColor labelColor(int index);
int labelCount();
const char* labelName(int index);

// Applies the dark Fusion palette and stylesheet to the application.
void apply(QApplication& app);

QFont monoFont(int pointSize = 9);

}  // namespace montage::theme
