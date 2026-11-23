// Montage — Help › Find Command: every menu command, effect and saved effect preset found by its name and run from
// the keyboard. Typing narrows the list (each word typed must be in the name, the menu it is in, or for an effect its
// group; as translated or as written in English); Return runs the one chosen. What was run last comes first the next
// time, among what the text typed finds.
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <vector>

#include "core/Effects.h"

class QAction;
class QLineEdit;
class QListWidget;

namespace montage {

class CommandFinder : public QDialog {
    Q_OBJECT
public:
    // `window`: the main window, whose commands (app/Keymap.h) are listed.
    explicit CommandFinder(QWidget* window);

    // What `text` finds, in the order listed, by id: a menu command's "<menu>/<command>" (as app/Keymap.h names it),
    // "effect:<type>" or "preset:<file>".
    QStringList find(const QString& text) const;
    void setText(const QString& text);
    // The ids listed now, and the one chosen.
    QStringList listed() const;
    QString chosen() const;
    // Runs one by its id and notes it as the last used. False when there is none of that id, or it is a command that
    // cannot be used now (greyed in its menu).
    bool run(const QString& id);
    // The ids last run, latest first (kept between sessions), and forgotten.
    static QStringList recent();
    static void forgetRecent();

signals:
    // An effect or a preset chosen: applied as from the Effects panel ("preset:<file>" for a preset).
    void applyRequested(const QString& type, montage::EffectCategory category);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QString id, name, where, search;
        QPointer<QAction> action;
        QString type;
        EffectCategory category = EffectCategory::VideoFilter;
    };
    void gather();
    void refresh();
    const Entry* entry(const QString& id) const;

    QWidget* window_;
    std::vector<Entry> entries_;
    QLineEdit* edit_;
    QListWidget* list_;
};

}  // namespace montage
