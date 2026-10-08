// Montage — the application's settings store. Always organisation "Montage", application "Montage", so the app, its
// command-line tools and its tests read and write the same place whatever the process's own names are (a
// default-constructed QSettings relies on those, and on Windows cannot read or write at all without them).
#pragma once

#include <QSettings>

namespace montage {

inline QSettings appSettings() { return QSettings(QStringLiteral("Montage"), QStringLiteral("Montage")); }

}  // namespace montage
