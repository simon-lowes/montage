// Montage — every setting Montage keeps, declared once (Edit › Preferences… shows them, and the code reads them here).
//
// Each setting has a key in Montage's settings store (QSettings, organisation and application "Montage"), a page of
// the Preferences window, a name and a sentence saying what it does, a kind and a default. What the code is given,
// in order: the environment variable that overrides it, when it has one and it is set; a value a facility's file
// locks; the value set in the window (or wherever else Montage lets it be changed); the facility file's default; and
// the setting's own default.
//
// A facility's file (MONTAGE_FACILITY_SETTINGS names it) is JSON: {"montage-settings": 1, "settings": {key: value},
// "locked": [key, ...]}. Its values are the defaults on every computer that names it, and a locked one cannot be
// changed there. A user's settings are saved to and read from a file of the same form (no "locked").
//
// Remembered choices (a dialog's last folder, the window's layout, an export's last options) are not settings: they
// are declared here too, by key or by group, so that a key the code reads is always one of the two.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <vector>

namespace montage::prefs {

enum class Kind { Toggle, Whole, Number, Text, Folder, File, Program, Choice };

struct Choice {
    QString value;
    const char* label = nullptr;  // (translated under "montage::Preferences"; empty: the value itself)
};

struct Setting {
    QString key;
    const char* page = nullptr;         // one of pages()
    const char* label = nullptr;        // its name in the window (translated under "montage::Preferences")
    const char* description = nullptr;  // a sentence on what it does
    Kind kind = Kind::Toggle;
    QVariant fallback;                  // its default ("" for a folder, file or program: the one Montage finds itself)
    QString env;                        // the environment variable that overrides it
    double min = 0, max = 0;            // a number's range
    const char* unit = nullptr;         // shown after a number ("MB", "s")
    std::vector<Choice> choices;        // a choice's values (none: given by the window, as languages are)
    bool restart = false;               // read when Montage starts: a change is used from then
};

// Every setting, in the order the window shows them, and the pages, in theirs.
const std::vector<Setting>& all();
const QStringList& pages();
const Setting* find(const QString& key);

// What the code uses (see above), as the setting's kind: a bool, int, double or string. A key that is not a
// setting gives an invalid QVariant.
QVariant get(const QString& key);
bool toggle(const QString& key);
int whole(const QString& key);
double number(const QString& key);
QString text(const QString& key);

// The default here: the facility file's, else the setting's own.
QVariant defaultFor(const QString& key);
// Whether a value of the user's own is kept for it (else its default is used).
bool isSet(const QString& key);
// The environment variable and value that override it now ("MONTAGE_GPU_MEMORY=512"; "" when none does).
QString overriddenBy(const QString& key);
// The facility file that locks it ("" when none does).
QString lockedBy(const QString& key);

// Sets the user's own value (made the setting's kind, a number kept in its range). False, with why, when the key is
// no setting or a facility file locks it.
bool set(const QString& key, const QVariant& value, QString* why = nullptr);
// The user's own value let go: its default is used again (a locked one is unchanged).
void restore(const QString& key);

// The facility's file (MONTAGE_FACILITY_SETTINGS; "" when none is named), read again (for testing, or after the
// variable or the file changed; it is otherwise read once).
QString facilityFile();
void reloadFacility();
// A problem reading it ("" when there is none).
QString facilityProblem();

// The user's settings (every one with a value of its own) written to a file; and a file read back, each setting in
// it set (one locked, one whose value it cannot take, or a key that is no setting passed over and listed in `passed`).
bool saveTo(const QString& path, QString* error = nullptr);
bool loadFrom(const QString& path, QStringList* passed = nullptr, QString* error = nullptr);

// Keys the code reads that are remembered choices, not settings: whole keys, and groups ending in "/*".
const QStringList& remembered();
bool isRemembered(const QString& key);

}  // namespace montage::prefs
