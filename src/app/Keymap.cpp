#include "Keymap.h"
#include "Settings.h"

#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace montage::keymap {

namespace {

const char* const kId = "keymapId";
const char* const kDefault = "keymapDefault";
const char* const kGroup = "keymap/";

QString clean(QString s) {
    s.remove('&');
    s.remove(QStringLiteral("…"));
    s.remove(QStringLiteral("..."));
    return s.trimmed();
}

QString keyText(const QKeySequence& k) { return k.toString(QKeySequence::PortableText); }

// Every key Montage gives the action (some have two, as Redo has Ctrl+Shift+Z and Ctrl+Y).
QList<QKeySequence> defaultKeys(const QAction* a) {
    QList<QKeySequence> out;
    for (const QString& k : a->property(kDefault).toString().split(QStringLiteral("; "), Qt::SkipEmptyParts))
        out << QKeySequence::fromString(k, QKeySequence::PortableText);
    return out;
}

// Saves the action's key if it differs from its own, else forgets it.
void persist(const QAction* a) {
    QSettings s = appSettings();
    const QString id = idOf(a);
    if (a->shortcuts() == defaultKeys(a)) s.remove(kGroup + id);
    else s.setValue(kGroup + id, a->shortcut().isEmpty() ? QStringLiteral("none") : keyText(a->shortcut()));
}

void setKeys(QAction* a, const QList<QKeySequence>& keys) {
    a->setShortcuts(keys);
    const QKeySequence k = keys.isEmpty() ? QKeySequence() : keys.front();
    // Tool buttons show their key in the tooltip.
    QString tip = a->toolTip();
    const int open = tip.lastIndexOf(QStringLiteral(" ("));
    if (open > 0 && tip.endsWith(')') && a->isCheckable()) tip = tip.left(open);
    if (a->isCheckable() && !tip.isEmpty() && tip != clean(a->text()))
        a->setToolTip(k.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, k.toString(QKeySequence::NativeText)));
}

// Other editors' layouts, as changes from Montage's own (which follows Premiere Pro).
// Keys are Qt's portable text; "Ctrl" is Cmd on macOS. "" removes a key.
struct Preset {
    const char* name;
    std::vector<std::pair<const char*, const char*>> keys;
};

const std::vector<Preset>& presetTable() {
    static const std::vector<Preset> table = {
        {"Montage (Premiere Pro)", {}},
        {"Final Cut Pro",
         {
             {"Tools/Select", "A"},
             {"Tools/Razor", "B"},
             {"Tools/Ripple", "T"},
             {"Tools/Roll", ""},
             {"Tools/Slip", ""},
             {"Tools/Slide", ""},
             {"Tools/Hand", "H"},
             {"Clip/Insert from Source", "W"},
             {"Clip/Overwrite from Source", "D"},
             {"Clip/Add Edit", "Ctrl+B"},
             {"Sequence/Snapping", "N"},
             {"Clip/Add Frame Hold", "Alt+F"},
             {"Clip/Match Frame", "Shift+F"},
             {"Sequence/Select Clips After Playhead", ""},
             {"Sequence/Select Clips After Playhead on Target Track", ""},
             {"Sequence/Ripple Trim Previous Edit to Playhead", ""},
             {"Sequence/Ripple Trim Next Edit to Playhead", ""},
             {"Sequence/Zoom In", "Ctrl+="},
             {"Sequence/Zoom Out", "Ctrl+-"},
             {"Sequence/Zoom to Fit", "Shift+Z"},
             {"File/Export Media", "Ctrl+E"},
             {"Edit/Paste Attributes", "Ctrl+Shift+V"},
             {"Clip/Nudge Left", ","},
             {"Clip/Nudge Right", "."},
             {"Clip/Auto Colour", "Ctrl+Alt+B"},
         }},
        {"DaVinci Resolve",
         {
             {"Tools/Select", "A"},
             {"Tools/Razor", "B"},
             {"Tools/Ripple", "T"},
             {"Tools/Roll", ""},
             {"Tools/Slip", ""},
             {"Tools/Slide", ""},
             {"Tools/Hand", ""},
             {"Clip/Add Edit", "Ctrl+B"},
             {"Clip/Insert from Source", "F9"},
             {"Clip/Overwrite from Source", "F10"},
             {"Clip/Replace with Source Clip", "F11"},
             {"Clip/Fit to Fill", "Shift+F11"},
             {"Clip/Add Frame Hold", "Shift+R"},
             {"Clip/Reverse Match Frame", ""},
             {"Sequence/Snapping", "N"},
             {"Sequence/Select Clips After Playhead", "Alt+Y"},
             {"Sequence/Select Clips After Playhead on Target Track", "Y"},
             {"Sequence/Ripple Trim Previous Edit to Playhead", "Ctrl+Shift+["},
             {"Sequence/Ripple Trim Next Edit to Playhead", "Ctrl+Shift+]"},
             {"Sequence/Zoom In", "Ctrl+="},
             {"Sequence/Zoom Out", "Ctrl+-"},
             {"Sequence/Zoom to Fit", "Shift+Z"},
         }},
        {"Avid Media Composer",
         {
             {"Tools/Select", ""},
             {"Tools/Razor", ""},
             {"Tools/Ripple", ""},
             {"Tools/Roll", ""},
             {"Tools/Slip", ""},
             {"Tools/Slide", ""},
             {"Tools/Hand", ""},
             {"Clip/Insert from Source", "V"},
             {"Clip/Overwrite from Source", "B"},
             {"Sequence/Lift", "Z"},
             {"Sequence/Extract", "X"},
             {"Sequence/Mark In", "E"},
             {"Sequence/Mark Out", "R"},
             {"Sequence/Mark Clip", "T"},
             {"Sequence/Clear In and Out", "G"},
             {"Sequence/Go to In", "Q"},
             {"Sequence/Go to Out", "W"},
             {"Clip/Add Edit", "H"},
             {"Sequence/Ripple Trim Previous Edit to Playhead", ""},
             {"Sequence/Ripple Trim Next Edit to Playhead", ""},
             {"Sequence/Select Clips After Playhead", ""},
             {"Sequence/Select Clips After Playhead on Target Track", ""},
             {"Sequence/Snapping", ""},
             {"Clip/Match Frame", ""},
             {"Sequence/Add Marker", "F5"},
         }},
    };
    return table;
}

}  // namespace

void setKey(QAction* a, const QKeySequence& k) { setKeys(a, k.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{k}); }

void registerAction(QAction* action, const QString& menu, const QString& text) {
    action->setProperty(kId, clean(menu) + '/' + clean(text));
}

QString idOf(const QAction* action) { return action ? action->property(kId).toString() : QString(); }

QKeySequence defaultKey(const QAction* action) {
    const auto keys = defaultKeys(action);
    return keys.isEmpty() ? QKeySequence() : keys.front();
}

QList<QAction*> actions(const QWidget* window) {
    QList<QAction*> out;
    for (QAction* a : window->actions())
        if (!idOf(a).isEmpty()) out.push_back(a);
    return out;
}

QAction* find(const QWidget* window, const QString& id) {
    for (QAction* a : actions(window))
        if (idOf(a) == id) return a;
    return nullptr;
}

void load(QWidget* window) {
    QSettings s = appSettings();
    for (QAction* a : actions(window)) {
        // Montage's own keys, as the menus left them.
        if (!a->property(kDefault).isValid()) {
            QStringList keys;
            for (const QKeySequence& k : a->shortcuts()) keys << keyText(k);
            a->setProperty(kDefault, keys.join(QStringLiteral("; ")));
        }
        const QVariant v = s.value(kGroup + idOf(a));
        if (!v.isValid()) continue;
        const QString text = v.toString();
        setKey(a, text == QStringLiteral("none") ? QKeySequence() : QKeySequence::fromString(text, QKeySequence::PortableText));
    }
}

QAction* conflict(const QWidget* window, const QKeySequence& key, const QAction* except) {
    if (key.isEmpty()) return nullptr;
    for (QAction* a : actions(window))
        if (a != except && a->shortcuts().contains(key)) return a;
    return nullptr;
}

bool assign(QWidget* window, const QString& id, const QKeySequence& key, bool takeOver) {
    QAction* a = find(window, id);
    if (!a) return false;
    if (QAction* other = conflict(window, key, a)) {
        if (!takeOver) return false;
        QList<QKeySequence> rest = other->shortcuts();
        rest.removeAll(key);
        setKeys(other, rest);
        persist(other);
    }
    setKey(a, key);
    persist(a);
    return true;
}

void resetAll(QWidget* window) {
    appSettings().remove(QStringLiteral("keymap"));
    for (QAction* a : actions(window)) setKeys(a, defaultKeys(a));
}

QStringList presets() {
    QStringList out;
    for (const Preset& p : presetTable()) out << QString::fromLatin1(p.name);
    return out;
}

QJsonObject presetKeys(const QString& preset) {
    QJsonObject out;
    for (const Preset& p : presetTable())
        if (preset == QLatin1String(p.name))
            for (const auto& [id, key] : p.keys) out[QString::fromUtf8(id)] = QString::fromLatin1(key);
    return out;
}

bool applyPreset(QWidget* window, const QString& preset) {
    if (!presets().contains(preset)) return false;
    resetAll(window);
    const QJsonObject keys = presetKeys(preset);
    // Keys first taken away, then given, so a preset can move a key from one command to another.
    for (auto it = keys.begin(); it != keys.end(); ++it)
        if (QAction* a = find(window, it.key())) {
            setKey(a, QKeySequence());
            persist(a);
        }
    for (auto it = keys.begin(); it != keys.end(); ++it)
        if (!it.value().toString().isEmpty())
            assign(window, it.key(), QKeySequence::fromString(it.value().toString(), QKeySequence::PortableText), true);
    appSettings().setValue(QStringLiteral("keymapPreset"), preset);
    return true;
}

QJsonObject save(const QWidget* window) {
    QJsonObject keys;
    for (QAction* a : actions(window)) keys[idOf(a)] = keyText(a->shortcut());
    return QJsonObject{{"montage-keymap", 1}, {"keys", keys}};
}

bool restore(QWidget* window, const QJsonObject& json, QString* error) {
    if (json.value("montage-keymap").toInt() != 1 || !json.value("keys").isObject()) {
        if (error) *error = QObject::tr("This is not a Montage keyboard layout");
        return false;
    }
    resetAll(window);
    const QJsonObject keys = json.value("keys").toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it)
        if (QAction* a = find(window, it.key())) {
            setKey(a, QKeySequence());
            persist(a);
        }
    for (auto it = keys.begin(); it != keys.end(); ++it)
        if (!it.value().toString().isEmpty())
            assign(window, it.key(), QKeySequence::fromString(it.value().toString(), QKeySequence::PortableText), true);
    return true;
}

// ---------------------------------------------------------------------------
// Dialog

Dialog::Dialog(QWidget* window) : QDialog(window), window_(window) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("keymapFilter"));
    filter_->setPlaceholderText(tr("Search commands or keys"));
    filter_->setClearButtonEnabled(true);
    preset_ = new QComboBox(this);
    preset_->setObjectName(QStringLiteral("keymapPreset"));
    preset_->addItem(tr("Layout…"));
    preset_->addItems(presets());
    preset_->setToolTip(tr("Start from another editor's keys"));
    top->addWidget(filter_, 1);
    top->addWidget(preset_);
    lay->addLayout(top);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("keymapTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Default")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    lay->addWidget(tree_, 1);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Shortcut:"), this));
    edit_ = new QKeySequenceEdit(this);
    edit_->setObjectName(QStringLiteral("keymapEdit"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    edit_->setMaximumSequenceLength(1);
#endif
    auto* clear = new QPushButton(tr("None"), this);
    clear->setObjectName(QStringLiteral("keymapNone"));
    auto* reset = new QPushButton(tr("Default"), this);
    reset->setObjectName(QStringLiteral("keymapDefault"));
    row->addWidget(edit_, 1);
    row->addWidget(clear);
    row->addWidget(reset);
    lay->addLayout(row);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("keymapStatus"));
    status_->setWordWrap(true);
    lay->addWidget(status_);
    auto* bottom = new QHBoxLayout;
    auto* resetAllBtn = new QPushButton(tr("Reset All"), this);
    auto* importBtn = new QPushButton(tr("Import…"), this);
    auto* exportBtn = new QPushButton(tr("Export…"), this);
    bottom->addWidget(resetAllBtn);
    bottom->addWidget(importBtn);
    bottom->addWidget(exportBtn);
    bottom->addStretch();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bottom->addWidget(close);
    lay->addLayout(bottom);
    resize(640, 680);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(filter_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] {
        QAction* a = selected();
        edit_->setEnabled(a);
        edit_->setKeySequence(a ? a->shortcut() : QKeySequence());
        status_->clear();
    });
    connect(edit_, &QKeySequenceEdit::editingFinished, this, [this] {
        if (selected() && edit_->keySequence() != selected()->shortcut()) setSelectedKey(edit_->keySequence());
    });
    connect(clear, &QPushButton::clicked, this, [this] { setSelectedKey(QKeySequence()); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QAction* a = selected()) setSelectedKey(defaultKey(a));
    });
    connect(resetAllBtn, &QPushButton::clicked, this, [this] {
        resetAll(window_);
        rebuild();
        status_->setText(tr("Every command has Montage's own key again."));
    });
    connect(preset_, &QComboBox::activated, this, [this](int i) {
        if (i <= 0) return;
        applyPreset(window_, preset_->itemText(i));
        rebuild();
        status_->setText(tr("Keys now follow %1.").arg(preset_->itemText(i)));
        preset_->setCurrentIndex(0);
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Keyboard Layout"), QStringLiteral("montage-keys.json"),
                                                          tr("Keyboard layouts (*.json)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(save(window_)).toJson()) < 0)
            QMessageBox::warning(this, tr("Export Keyboard Layout"), tr("Could not write %1").arg(path));
    });
    connect(importBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Keyboard Layout"), QString(), tr("Keyboard layouts (*.json)"));
        if (path.isEmpty()) return;
        QFile f(path);
        QString err;
        if (!f.open(QIODevice::ReadOnly) || !restore(window_, QJsonDocument::fromJson(f.readAll()).object(), &err)) {
            QMessageBox::warning(this, tr("Import Keyboard Layout"), err.isEmpty() ? tr("Could not read %1").arg(path) : err);
            return;
        }
        rebuild();
    });
    rebuild();
}

void Dialog::rebuild() {
    const QString want = filter_->text().trimmed();
    const QString current = idOf(selected());
    tree_->clear();
    QMap<QString, QTreeWidgetItem*> menus;
    QTreeWidgetItem* pick = nullptr;
    for (QAction* a : keymap::actions(window_)) {
        const QString id = idOf(a);
        const QString menu = id.section('/', 0, 0), name = id.section('/', 1);
        const QString keys = a->shortcut().toString(QKeySequence::NativeText);
        if (!want.isEmpty() && !id.contains(want, Qt::CaseInsensitive) && !keys.contains(want, Qt::CaseInsensitive)) continue;
        QTreeWidgetItem*& parent = menus[menu];
        if (!parent) {
            parent = new QTreeWidgetItem(tree_, {menu});
            parent->setFirstColumnSpanned(true);
            parent->setFlags(Qt::ItemIsEnabled);
        }
        auto* item = new QTreeWidgetItem(parent, {name});
        item->setData(0, Qt::UserRole, id);
        updateRow(item);
        if (id == current) pick = item;
    }
    tree_->expandAll();
    tree_->resizeColumnToContents(1);
    tree_->resizeColumnToContents(2);
    if (pick) tree_->setCurrentItem(pick);
}

void Dialog::updateRow(QTreeWidgetItem* item) {
    QAction* a = keymap::find(window_, item->data(0, Qt::UserRole).toString());
    if (!a) return;
    item->setText(1, a->shortcut().toString(QKeySequence::NativeText));
    item->setText(2, defaultKey(a).toString(QKeySequence::NativeText));
    QFont f = item->font(1);
    f.setBold(a->shortcut() != defaultKey(a));
    item->setFont(1, f);
}

QAction* Dialog::selected() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    return item ? keymap::find(window_, item->data(0, Qt::UserRole).toString()) : nullptr;
}

void Dialog::setFilter(const QString& text) { filter_->setText(text); }

bool Dialog::select(const QString& id) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        for (int j = 0; j < tree_->topLevelItem(i)->childCount(); ++j) {
            QTreeWidgetItem* item = tree_->topLevelItem(i)->child(j);
            if (item->data(0, Qt::UserRole).toString() == id) {
                tree_->setCurrentItem(item);
                return true;
            }
        }
    return false;
}

QString Dialog::pendingConflict() const {
    QAction* other = conflict(window_, edit_->keySequence(), selected());
    return other ? idOf(other) : QString();
}

bool Dialog::setSelectedKey(const QKeySequence& key) {
    QAction* a = selected();
    if (!a) return false;
    QAction* other = conflict(window_, key, a);
    if (other) {
        const QString otherName = idOf(other).section('/', 1);
        if (isVisible()) {
            const auto answer = QMessageBox::question(
                this, tr("Shortcut in Use"),
                tr("%1 is used by “%2”. Give it to “%3” instead?").arg(key.toString(QKeySequence::NativeText), otherName, idOf(a).section('/', 1)));
            if (answer != QMessageBox::Yes) {
                edit_->setKeySequence(a->shortcut());
                return false;
            }
        }
        status_->setText(tr("“%1” no longer has a shortcut.").arg(otherName));
    }
    assign(window_, idOf(a), key, true);
    edit_->setKeySequence(a->shortcut());
    const QString id = idOf(a);
    rebuild();
    select(id);
    return true;
}

}  // namespace montage::keymap
