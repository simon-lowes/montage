// Montage — Help › Find Command (see CommandFinder.h).
#include "CommandFinder.h"

#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QVBoxLayout>
#include <algorithm>

#include "EffectPresetStore.h"
#include "Keymap.h"
#include "Language.h"
#include "Settings.h"

namespace montage {

namespace {

const char* const kRecentKey = "commands/recent";
constexpr int kRecentMost = 30;
constexpr int kIdRole = Qt::UserRole;

QString plain(QString s) {
    s.remove('&');
    s.remove(QStringLiteral("…"));
    s.remove(QStringLiteral("..."));
    return s.trimmed();
}

// The menus an action is in, outermost first, as shown ("Clip", or "Sequence › Markers").
QString menuPath(const QAction* action) {
    QStringList path;
    const QObject* at = action;
    for (int depth = 0; depth < 6; ++depth) {
        const QMenu* menu = nullptr;
        for (const QObject* w : at == action ? action->associatedObjects() : QList<QObject*>{}) {
            if ((menu = qobject_cast<const QMenu*>(w))) break;
        }
        if (at != action) {
            // (a submenu's own action sits in its parent menu)
            const QMenu* sub = qobject_cast<const QMenu*>(at);
            if (!sub) break;
            for (const QObject* w : sub->menuAction()->associatedObjects())
                if ((menu = qobject_cast<const QMenu*>(w))) break;
        }
        if (!menu) break;
        path.prepend(plain(menu->title()));
        at = menu;
    }
    return path.join(QStringLiteral(" › "));
}

}  // namespace

CommandFinder::CommandFinder(QWidget* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("commandFinder"));
    setWindowTitle(tr("Find Command"));
    resize(560, 420);
    auto* lay = new QVBoxLayout(this);
    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("commandFinderText"));
    edit_->setPlaceholderText(tr("Type the name of a command, an effect or a preset"));
    edit_->setClearButtonEnabled(true);
    edit_->installEventFilter(this);
    lay->addWidget(edit_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("commandFinderList"));
    list_->setUniformItemSizes(true);
    lay->addWidget(list_, 1);
    auto* hint = new QLabel(tr("Return runs the one chosen; the last used are listed first"), this);
    hint->setEnabled(false);
    lay->addWidget(hint);
    connect(edit_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(edit_, &QLineEdit::returnPressed, this, [this] {
        const QString id = chosen();
        if (id.isEmpty()) return;
        // (closed first: a command that opens a dialog of its own does so over the main window)
        accept();
        run(id);
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const QString id = item->data(kIdRole).toString();
        accept();
        run(id);
    });
    gather();
    refresh();
}

void CommandFinder::gather() {
    entries_.clear();
    // The menus' commands, in menu order.
    for (QAction* a : keymap::actions(window_)) {
        if (!a || a->isSeparator()) continue;
        Entry e;
        e.id = keymap::idOf(a);
        e.name = plain(a->text());
        e.where = menuPath(a);
        if (e.where.isEmpty()) e.where = e.id.section('/', 0, 0);
        e.search = e.name + ' ' + e.where + ' ' + e.id;
        e.action = a;
        entries_.push_back(std::move(e));
    }
    // The effects, transitions and generators of the catalogue.
    for (const EffectInfo& info : effectCatalog()) {
        if (info.hidden || info.category == EffectCategory::Fixed) continue;
        Entry e;
        e.id = QStringLiteral("effect:") + QString::fromStdString(info.type);
        e.name = language::engine(info.displayName);
        const QString kind = info.category == EffectCategory::Generator ? tr("Generator")
                             : info.category == EffectCategory::VideoTransition || info.category == EffectCategory::AudioTransition ? tr("Transition")
                             : info.category == EffectCategory::AudioFilter ? tr("Audio effect") : tr("Effect");
        e.where = info.group.empty() ? kind : kind + QStringLiteral(" › ") + language::engine(info.group);
        e.search = e.name + ' ' + e.where + ' ' + QString::fromStdString(info.displayName) + ' ' + QString::fromStdString(info.group) + ' ' +
                   QString::fromStdString(info.type);
        e.type = QString::fromStdString(info.type);
        e.category = info.category;
        entries_.push_back(std::move(e));
    }
    // The effect presets saved on this computer.
    for (const auto& [file, preset] : presets::all()) {
        Entry e;
        e.id = QStringLiteral("preset:") + file;
        e.name = QString::fromStdString(preset.name);
        e.where = tr("Preset");
        e.search = e.name + ' ' + e.where + QStringLiteral(" preset");
        e.type = e.id;
        e.category = preset.video ? EffectCategory::VideoFilter : EffectCategory::AudioFilter;
        entries_.push_back(std::move(e));
    }
}

const CommandFinder::Entry* CommandFinder::entry(const QString& id) const {
    for (const Entry& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

QStringList CommandFinder::find(const QString& text) const {
    const QStringList words = text.split(' ', Qt::SkipEmptyParts);
    auto matches = [&](const Entry& e) {
        for (const QString& w : words)
            if (!e.search.contains(w, Qt::CaseInsensitive)) return false;
        return true;
    };
    // Those run before first, the latest first; then the rest: names that start with what was typed, then names that
    // hold it, then what is found by its menu or group alone, each in menu order.
    QStringList out;
    const QStringList last = recent();
    for (const QString& id : last)
        if (const Entry* e = entry(id); e && matches(*e)) out << id;
    const QString whole = text.trimmed();
    for (int rank = 0; rank < 3; ++rank)
        for (const Entry& e : entries_) {
            if (!matches(e) || last.contains(e.id)) continue;
            const int is = whole.isEmpty() || e.name.startsWith(whole, Qt::CaseInsensitive) ? 0 : e.name.contains(whole, Qt::CaseInsensitive) ? 1 : 2;
            if (is == rank) out << e.id;
        }
    return out;
}

void CommandFinder::setText(const QString& text) {
    edit_->setText(text);
    refresh();
}

void CommandFinder::refresh() {
    list_->clear();
    for (const QString& id : find(edit_->text())) {
        const Entry* e = entry(id);
        auto* item = new QListWidgetItem(e->where.isEmpty() ? e->name : QStringLiteral("%1    —  %2").arg(e->name, e->where), list_);
        item->setData(kIdRole, id);
        if (e->action) {
            if (!e->action->shortcut().isEmpty()) item->setToolTip(e->action->shortcut().toString(QKeySequence::NativeText));
            // (a command that cannot be used now is listed as its menu shows it)
            if (!e->action->isEnabled()) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
    }
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->flags() & Qt::ItemIsEnabled) {
            list_->setCurrentRow(i);
            break;
        }
}

QStringList CommandFinder::listed() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->data(kIdRole).toString();
    return out;
}

QString CommandFinder::chosen() const { return list_->currentItem() ? list_->currentItem()->data(kIdRole).toString() : QString(); }

bool CommandFinder::run(const QString& id) {
    const Entry* e = entry(id);
    if (!e) return false;
    if (e->type.isEmpty() && (!e->action || !e->action->isEnabled())) return false;
    QStringList last = recent();
    last.removeAll(id);
    last.prepend(id);
    while (last.size() > kRecentMost) last.removeLast();
    appSettings().setValue(kRecentKey, last);
    // (copies: running a command may rebuild the menus, or delete this dialog)
    const QPointer<QAction> action = e->action;
    const QString type = e->type;
    const EffectCategory category = e->category;
    if (!type.isEmpty()) emit applyRequested(type, category);
    else if (action) action->trigger();
    return true;
}

QStringList CommandFinder::recent() { return appSettings().value(kRecentKey).toStringList(); }

void CommandFinder::forgetRecent() { appSettings().remove(kRecentKey); }

bool CommandFinder::eventFilter(QObject* watched, QEvent* event) {
    // The arrow keys move through the list while the text keeps the keyboard.
    if (watched == edit_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
            const int step = key == Qt::Key_Down ? 1 : key == Qt::Key_Up ? -1 : key == Qt::Key_PageDown ? 10 : -10;
            int row = list_->currentRow();
            for (int next = row + (step > 0 ? 1 : -1), moved = 0; next >= 0 && next < list_->count(); next += step > 0 ? 1 : -1) {
                if (!(list_->item(next)->flags() & Qt::ItemIsEnabled)) continue;
                row = next;
                if (++moved >= std::abs(step)) break;
            }
            if (row >= 0) list_->setCurrentRow(row);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

}  // namespace montage
