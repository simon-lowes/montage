// Montage — keyboard shortcuts: every menu command can be given any key,
// with conflicts caught, changes kept between sessions, layouts saved to and
// loaded from files, and presets that follow other editors (Premiere Pro,
// Final Cut Pro, DaVinci Resolve, Avid Media Composer).
//
// Commands are known by a stable id, "<menu>/<command>" (as the menus show
// them, without mnemonics or ellipses), set when the menus are built; only
// the keys that differ from Montage's own are stored.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

class QAction;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace montage::keymap {

// Gives an action its id and remembers its own key (call as it is created).
void registerAction(QAction* action, const QString& menu, const QString& text);
QString idOf(const QAction* action);
QKeySequence defaultKey(const QAction* action);
// The window's commands, in menu order.
QList<QAction*> actions(const QWidget* window);
QAction* find(const QWidget* window, const QString& id);

// Applies the saved keys (at startup).
void load(QWidget* window);
// The command already using `key`, other than `except`; null if none.
QAction* conflict(const QWidget* window, const QKeySequence& key, const QAction* except = nullptr);
// Gives command `id` the key (empty: none) and saves it. A command already
// using that key loses it when `takeOver`, else nothing changes and false.
bool assign(QWidget* window, const QString& id, const QKeySequence& key, bool takeOver);
void resetAll(QWidget* window);

// Presets: Montage's own keys, then other editors' layouts on top of them.
QStringList presets();
// The keys a preset changes from Montage's own (id -> key, "" for none).
QJsonObject presetKeys(const QString& preset);
bool applyPreset(QWidget* window, const QString& preset);

// The whole layout as JSON ({"montage-keymap": 1, "keys": {id: key}}), and back.
QJsonObject save(const QWidget* window);
bool restore(QWidget* window, const QJsonObject& json, QString* error = nullptr);

// Edit › Keyboard Shortcuts: search, set, reset, presets, import and export.
class Dialog : public QDialog {
    Q_OBJECT
public:
    explicit Dialog(QWidget* window);

    void setFilter(const QString& text);
    // Selects command `id` for editing; false if it is not shown.
    bool select(const QString& id);
    // What the key field would do: the command it would take the key from, or "".
    QString pendingConflict() const;
    // Sets the selected command's key as the key field does.
    bool setSelectedKey(const QKeySequence& key);

private:
    void rebuild();
    void updateRow(QTreeWidgetItem* item);
    QAction* selected() const;

    QWidget* window_;
    QLineEdit* filter_;
    QComboBox* preset_;
    QTreeWidget* tree_;
    QKeySequenceEdit* edit_;
    QLabel* status_;
};

}  // namespace montage::keymap
