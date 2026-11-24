// Montage — the application's settings store. Always organisation "Montage", application "Montage", so the app, its
// command-line tools and its tests read and write the same place whatever the process's own names are (a
// default-constructed QSettings relies on those, and on Windows cannot read or write at all without them). Kept in
// QSettings' default format, the system's own unless a program chooses another: the tests choose a file of their own,
// since the system's store on Windows (the registry) and macOS outlives a run and is not moved by QSettings::setPath.
#pragma once

#include <QSettings>

namespace montage {

inline QSettings appSettings() { return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Montage"), QStringLiteral("Montage")); }

}  // namespace montage
