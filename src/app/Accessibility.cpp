#include "Accessibility.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAccessibleWidget>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <algorithm>
#include <map>
#include <memory>

#include "EditorState.h"
#include "KeyframePanel.h"
#include "TimelineWidget.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage::access {

QString trackName(const Sequence& s, TrackRef ref) {
    const Track* t = trackAt(s, ref);
    if (t && !t->name.empty()) return QString::fromStdString(t->name);
    return (ref.kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(ref.index + 1);
}

namespace {

QString gLastAnnouncement;

QHash<const QObject*, LiveText>& liveTexts() {
    static QHash<const QObject*, LiveText> texts;
    return texts;
}

// Bumped when a timeline's clips change, so its table reads them again.
QHash<const QObject*, uint64_t>& revisions() {
    static QHash<const QObject*, uint64_t> r;
    return r;
}

// The interfaces made for a widget's parts (a timeline's cells, a panel's keys), let go of when the parts change. They
// are deleted once control is back in the event loop, never while one of them may be answering a call (which can be
// what noticed the change); until then they say they are no longer valid. Whatever is left goes with the widget's
// interface.
class Parts {
public:
    ~Parts() { release(); }
    uint64_t generation() const { return generation_; }
    // A part's interface, made the first time it is asked for.
    template <class Make>
    QAccessibleInterface* get(int64_t key, Make&& make) {
        auto& id = ids_[key];
        if (!id) id = QAccessible::registerAccessibleInterface(make());
        return QAccessible::accessibleInterface(id);
    }
    // The parts changed: the interfaces made so far are stale, deleted later from the event loop (with `context`).
    void stale(QObject* context) {
        ++generation_;
        if (ids_.empty()) return;
        for (const auto& [key, id] : ids_) pending_->push_back(id);
        ids_.clear();
        QTimer::singleShot(0, context, [pending = pending_] {
            for (const QAccessible::Id id : *pending) QAccessible::deleteAccessibleInterface(id);
            pending->clear();
        });
    }

private:
    void release() {
        for (const auto& [key, id] : ids_) QAccessible::deleteAccessibleInterface(id);
        for (const QAccessible::Id id : *pending_) QAccessible::deleteAccessibleInterface(id);
        ids_.clear();
        pending_->clear();
    }
    uint64_t generation_ = 0;
    std::map<int64_t, QAccessible::Id> ids_;
    std::shared_ptr<std::vector<QAccessible::Id>> pending_ = std::make_shared<std::vector<QAccessible::Id>>();
};

QString transitionName(const Transition& t) {
    const EffectInfo* info = findEffectInfo(t.type);
    return QString::fromStdString(info ? info->displayName : t.type);
}

QString clipText(const Sequence& s, const Track& track, TrackRef ref, const Clip& c) {
    QStringList parts;
    parts << (c.name.empty() ? QApplication::translate("Accessibility", "Untitled clip") : QString::fromStdString(c.name));
    parts << trackName(s, ref);
    parts << QApplication::translate("Accessibility", "%1 to %2").arg(timecodeString(&s, c.start), timecodeString(&s, c.end()));
    for (const Transition& t : track.transitions) {
        if (t.clipB == c.id) parts << QApplication::translate("Accessibility", "%1 in").arg(transitionName(t));
        if (t.clipA == c.id) parts << QApplication::translate("Accessibility", "%1 out").arg(transitionName(t));
    }
    if (!c.effects.empty()) parts << QApplication::translate("Accessibility", "%n effect(s)", nullptr, int(c.effects.size()));
    if (!c.enabled) parts << QApplication::translate("Accessibility", "disabled");
    return parts.join(QStringLiteral(", "));
}

// --- widgets whose text is worked out when asked ---

class LiveAccessible : public QAccessibleWidget {
public:
    LiveAccessible(QWidget* w, QAccessible::Role role) : QAccessibleWidget(w, role) {}
    QString text(QAccessible::Text t) const override {
        const LiveText live = liveTexts().value(object());
        const std::function<QString()>* f = t == QAccessible::Name          ? &live.name
                                            : t == QAccessible::Value       ? &live.value
                                            : t == QAccessible::Description ? &live.description
                                                                            : nullptr;
        if (f && *f) return (*f)();
        // (A value is the description too: AT-SPI reads only numbers as values.)
        if (t == QAccessible::Description && live.value) return live.value();
        return QAccessibleWidget::text(t);
    }
};

// --- the timeline: a table of tracks, its cells the clips and gaps ---

class TimelineAccessible;

class TimelineCellAccessible : public QAccessibleInterface, public QAccessibleTableCellInterface, public QAccessibleActionInterface {
public:
    TimelineCellAccessible(TimelineAccessible* table, int row, int column, uint64_t generation)
        : table_(table), row_(row), column_(column), generation_(generation) {}
    bool isValid() const override;
    QObject* object() const override { return nullptr; }
    QWindow* window() const override;
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override;
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override;
    QAccessible::Role role() const override { return QAccessible::Cell; }
    QAccessible::State state() const override;
    void* interface_cast(QAccessible::InterfaceType t) override {
        if (t == QAccessible::TableCellInterface) return static_cast<QAccessibleTableCellInterface*>(this);
        if (t == QAccessible::ActionInterface) return static_cast<QAccessibleActionInterface*>(this);
        return nullptr;
    }
    // Table cell.
    bool isSelected() const override;
    QList<QAccessibleInterface*> columnHeaderCells() const override { return {}; }
    QList<QAccessibleInterface*> rowHeaderCells() const override { return {}; }
    int columnIndex() const override { return column_; }
    int rowIndex() const override { return row_; }
    int columnExtent() const override { return 1; }
    int rowExtent() const override { return 1; }
    QAccessibleInterface* table() const override;
    // Actions: pressing a cell selects its clip (or gap).
    QStringList actionNames() const override { return {pressAction(), setFocusAction()}; }
    void doAction(const QString& action) override;
    QStringList keyBindingsForAction(const QString&) const override { return {}; }

private:
    const TimelineCell* cell() const;  // null once the timeline has changed
    TimelineAccessible* table_;
    int row_, column_;
    uint64_t generation_;
};

class TimelineAccessible : public QAccessibleWidget, public QAccessibleTableInterface {
public:
    explicit TimelineAccessible(QWidget* w) : QAccessibleWidget(w, QAccessible::Table) {}

    TimelineWidget* timeline() const { return static_cast<TimelineWidget*>(widget()); }
    const Sequence* sequence() const { return timeline()->state()->sequence(); }

    const std::vector<TimelineRow>& rows() const {
        const uint64_t revision = revisions().value(object());
        if (!read_ || revision != revision_) {
            cells_.stale(object());
            rows_ = sequence() ? timelineRows(*sequence(), timeline()->shownTracks()) : std::vector<TimelineRow>();
            revision_ = revision;
            read_ = true;
        }
        return rows_;
    }
    const TimelineCell* cellData(int row, int column) const {
        const auto& r = rows();
        if (row < 0 || row >= int(r.size()) || column < 0 || column >= int(r[size_t(row)].cells.size())) return nullptr;
        return &r[size_t(row)].cells[size_t(column)];
    }
    QRect cellRect(int row, int column) const {
        const TimelineCell* c = cellData(row, column);
        if (!c) return {};
        // (Clipped to the viewport: a clip scrolled under the track headers or out of view is not there.)
        const QRect r = timeline()->spanBounds(rows_[size_t(row)].track, c->start, c->end) & timeline()->viewport()->rect();
        return r.isEmpty() ? QRect() : QRect(timeline()->viewport()->mapToGlobal(r.topLeft()), r.size());
    }
    bool selected(const TimelineCell& c) const {
        if (c.clip) return timeline()->state()->isSelected(c.clip);
        return false;
    }

    void* interface_cast(QAccessible::InterfaceType t) override {
        if (t == QAccessible::TableInterface) return static_cast<QAccessibleTableInterface*>(this);
        return QAccessibleWidget::interface_cast(t);
    }
    QString text(QAccessible::Text t) const override {
        if (t == QAccessible::Value && sequence()) return timelineValue(*sequence(), timeline()->state()->selectedClips(), timeline()->state()->focusClip());
        if (t == QAccessible::Description && sequence()) return QString::fromStdString(sequence()->name);
        return QAccessibleWidget::text(t);
    }
    int childCount() const override {
        int n = 0;
        for (const TimelineRow& r : rows()) n += int(r.cells.size());
        return n;
    }
    QAccessibleInterface* child(int index) const override {
        if (index < 0) return nullptr;
        const auto& r = rows();
        for (int row = 0; row < int(r.size()); ++row) {
            const int n = int(r[size_t(row)].cells.size());
            if (index < n) return cellAt(row, index);
            index -= n;
        }
        return nullptr;
    }
    int indexOfChild(const QAccessibleInterface* child) const override {
        const QAccessibleTableCellInterface* cell =
            child && child->role() == QAccessible::Cell ? const_cast<QAccessibleInterface*>(child)->tableCellInterface() : nullptr;
        if (!cell || cell->table() != this || !child->isValid()) return -1;
        int index = cell->columnIndex();
        const auto& r = rows();
        for (int row = 0; row < cell->rowIndex() && row < int(r.size()); ++row) index += int(r[size_t(row)].cells.size());
        return index;
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        const auto& r = rows();
        for (int row = 0; row < int(r.size()); ++row)
            for (int column = 0; column < int(r[size_t(row)].cells.size()); ++column)
                if (cellRect(row, column).contains(x, y)) return cellAt(row, column);
        return nullptr;
    }
    QAccessibleInterface* focusChild() const override {
        if (!timeline()->hasFocus()) return nullptr;
        const Id focus = timeline()->state()->focusClip();
        const auto& r = rows();
        for (int row = 0; row < int(r.size()); ++row)
            for (int column = 0; column < int(r[size_t(row)].cells.size()); ++column)
                if (const Id clip = r[size_t(row)].cells[size_t(column)].clip; clip && clip == focus)
                    return cellAt(row, column);
        return nullptr;
    }

    // Table.
    QAccessibleInterface* caption() const override { return nullptr; }
    QAccessibleInterface* summary() const override { return nullptr; }
    QAccessibleInterface* cellAt(int row, int column) const override {
        if (!cellData(row, column)) return nullptr;
        return cells_.get((int64_t(row) << 32) | column, [&] {
            return new TimelineCellAccessible(const_cast<TimelineAccessible*>(this), row, column, cells_.generation());
        });
    }
    QList<QAccessibleInterface*> selectedCells() const override {
        QList<QAccessibleInterface*> out;
        const auto& r = rows();
        for (int row = 0; row < int(r.size()); ++row)
            for (int column = 0; column < int(r[size_t(row)].cells.size()); ++column)
                if (selected(r[size_t(row)].cells[size_t(column)])) out << cellAt(row, column);
        return out;
    }
    int selectedCellCount() const override { return int(selectedCells().size()); }
    QString columnDescription(int) const override { return QString(); }
    QString rowDescription(int row) const override {
        const auto& r = rows();
        return row >= 0 && row < int(r.size()) ? r[size_t(row)].name : QString();
    }
    int selectedColumnCount() const override { return 0; }
    int selectedRowCount() const override { return 0; }
    int columnCount() const override {
        size_t n = 0;
        for (const TimelineRow& r : rows()) n = std::max(n, r.cells.size());
        return int(n);
    }
    int rowCount() const override { return int(rows().size()); }
    QList<int> selectedColumns() const override { return {}; }
    QList<int> selectedRows() const override { return {}; }
    bool isColumnSelected(int) const override { return false; }
    bool isRowSelected(int) const override { return false; }
    bool selectRow(int) override { return false; }
    bool selectColumn(int) override { return false; }
    bool unselectRow(int) override { return false; }
    bool unselectColumn(int) override { return false; }
    void modelChange(QAccessibleTableModelChangeEvent*) override { read_ = false; }

    uint64_t generation() const {
        rows();
        return cells_.generation();
    }

private:
    mutable std::vector<TimelineRow> rows_;
    mutable uint64_t revision_ = 0;
    mutable bool read_ = false;
    mutable Parts cells_;
};

const TimelineCell* TimelineCellAccessible::cell() const {
    return table_->generation() == generation_ ? table_->cellData(row_, column_) : nullptr;
}
bool TimelineCellAccessible::isValid() const { return table_->isValid() && cell(); }
QWindow* TimelineCellAccessible::window() const { return table_->window(); }
QAccessibleInterface* TimelineCellAccessible::parent() const { return table_; }
QAccessibleInterface* TimelineCellAccessible::table() const { return table_; }
QRect TimelineCellAccessible::rect() const { return cell() ? table_->cellRect(row_, column_) : QRect(); }
bool TimelineCellAccessible::isSelected() const { return cell() && table_->selected(*cell()); }

QString TimelineCellAccessible::text(QAccessible::Text t) const {
    const TimelineCell* c = cell();
    if (!c) return QString();
    if (t == QAccessible::Name) return c->text;
    return QString();
}

QAccessible::State TimelineCellAccessible::state() const {
    QAccessible::State s;
    s.selectable = s.focusable = true;
    s.selected = isSelected();
    s.focused = s.selected && table_->focusChild() == this;
    const QRect r = rect();
    s.invisible = r.isEmpty();
    s.offscreen = r.isEmpty() || !QRect(table_->timeline()->viewport()->mapToGlobal(QPoint()), table_->timeline()->viewport()->size()).intersects(r);
    return s;
}

void TimelineCellAccessible::doAction(const QString& action) {
    const TimelineCell* c = cell();
    if (!c) return;
    TimelineWidget* t = table_->timeline();
    const TrackRef track = table_->rows()[size_t(row_)].track;
    const TimelineCell copy = *c;  // (selecting may change the timeline)
    if (copy.clip) t->state()->setSelection({copy.clip});
    else {
        t->state()->clearSelection();
        t->selectGapAt(track, copy.start);
    }
    if (action == setFocusAction()) t->setFocus();
}

// --- the keyframes: a list ---

class KeyframesAccessible;

class KeyframeItemAccessible : public QAccessibleInterface, public QAccessibleActionInterface {
public:
    KeyframeItemAccessible(KeyframesAccessible* list, KeyframePanel::Key key, uint64_t generation)
        : list_(list), key_(std::move(key)), generation_(generation) {}
    bool isValid() const override;
    QObject* object() const override { return nullptr; }
    QWindow* window() const override;
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override;
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override;
    QAccessible::Role role() const override { return QAccessible::ListItem; }
    QAccessible::State state() const override;
    void* interface_cast(QAccessible::InterfaceType t) override {
        return t == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this) : nullptr;
    }
    QStringList actionNames() const override { return {pressAction()}; }
    void doAction(const QString&) override;
    QStringList keyBindingsForAction(const QString&) const override { return {}; }
    const KeyframePanel::Key& key() const { return key_; }

private:
    KeyframesAccessible* list_;
    KeyframePanel::Key key_;
    uint64_t generation_;  // its keys' when made
};

class KeyframesAccessible : public QAccessibleWidget {
public:
    explicit KeyframesAccessible(QWidget* w) : QAccessibleWidget(w, QAccessible::List) {}
    KeyframePanel* panel() const { return static_cast<KeyframePanel*>(widget()); }

    const std::vector<KeyframePanel::Key>& keys() const {
        auto now = panel()->keys();
        if (now != keys_) {
            items_.stale(object());
            keys_ = std::move(now);
        }
        return keys_;
    }
    int childCount() const override { return int(keys().size()); }
    QAccessibleInterface* child(int index) const override {
        const auto& k = keys();
        if (index < 0 || index >= int(k.size())) return nullptr;
        return items_.get(index, [&] {
            return new KeyframeItemAccessible(const_cast<KeyframesAccessible*>(this), k[size_t(index)], items_.generation());
        });
    }
    int indexOfChild(const QAccessibleInterface* child) const override {
        if (!child || child->role() != QAccessible::ListItem || child->parent() != this || !child->isValid()) return -1;
        const auto& k = keys();
        const auto it = std::find(k.begin(), k.end(), static_cast<const KeyframeItemAccessible*>(child)->key());
        return it == k.end() ? -1 : int(it - k.begin());
    }
    QAccessibleInterface* focusChild() const override {
        if (!panel()->hasFocus() || panel()->selection().empty()) return nullptr;
        const auto& k = keys();
        const auto it = std::find(k.begin(), k.end(), *panel()->selection().begin());
        return it == k.end() ? nullptr : child(int(it - k.begin()));
    }
    QString text(QAccessible::Text t) const override {
        if (t == QAccessible::Name && panel()->accessibleName().isEmpty()) return QApplication::translate("Accessibility", "Keyframes");
        if (t == QAccessible::Description)
            return QApplication::translate("Accessibility", "%n keyframe(s) on %1 parameters", nullptr, int(keys().size()))
                .arg(panel()->rows().size());
        return QAccessibleWidget::text(t);
    }

    uint64_t generation() const {
        keys();
        return items_.generation();
    }

private:
    mutable std::vector<KeyframePanel::Key> keys_;
    mutable Parts items_;
};

bool KeyframeItemAccessible::isValid() const { return list_->isValid() && list_->generation() == generation_; }
QWindow* KeyframeItemAccessible::window() const { return list_->window(); }
QAccessibleInterface* KeyframeItemAccessible::parent() const { return list_; }
QString KeyframeItemAccessible::text(QAccessible::Text t) const {
    return t == QAccessible::Name && isValid() ? list_->panel()->keyText(key_) : QString();
}
QRect KeyframeItemAccessible::rect() const {
    if (!isValid()) return {};
    KeyframePanel* p = list_->panel();
    for (int row = 0; row < int(p->rows().size()); ++row)
        if (p->rows()[size_t(row)].address == key_.address) {
            const QPoint c = p->mapToGlobal(p->keyPoint(row, key_.t));
            return QRect(c - QPoint(5, 5), QSize(11, 11));
        }
    return {};
}
QAccessible::State KeyframeItemAccessible::state() const {
    QAccessible::State s;
    if (!isValid()) return s;
    s.selectable = s.focusable = true;
    s.selected = list_->panel()->selection().count(key_) > 0;
    s.focused = s.selected && list_->focusChild() == this;
    return s;
}
void KeyframeItemAccessible::doAction(const QString&) { list_->panel()->select({key_}); }

QAccessibleInterface* factory(const QString& className, QObject* object) {
    if (!object || !object->isWidgetType()) return nullptr;
    auto* w = static_cast<QWidget*>(object);
    if (className == QLatin1String("montage::TimelineWidget")) return new TimelineAccessible(w);
    if (className == QLatin1String("montage::KeyframePanel")) return new KeyframesAccessible(w);
    if (const auto it = liveTexts().constFind(object); it != liveTexts().constEnd()) return new LiveAccessible(w, it->role);
    // A custom widget that takes the focus and says nothing more of itself is a graphic drawn by Montage, not a client.
    if (className == QLatin1String("QWidget") && w->focusPolicy() != Qt::NoFocus)
        return new QAccessibleWidget(w, QAccessible::Graphic);
    return nullptr;
}

// --- names for the controls that would have none ---

QString labelText(const QLabel* label) {
    QString t = label->text();
    t.remove(QRegularExpression(QStringLiteral("<[^>]*>")));  // rich text
    t.remove(QLatin1Char('&'));
    t = t.trimmed();
    if (t.endsWith(QLatin1Char(':'))) t.chop(1);
    return t;
}

// Whether a widget has a name of its own, as Qt's interfaces give it (the text on a button, a label's or group's).
bool namedAlready(const QWidget* w) {
    if (!w->accessibleName().isEmpty()) return true;
    if (const auto* b = qobject_cast<const QAbstractButton*>(w)) return !b->text().remove(QLatin1Char('&')).trimmed().isEmpty();
    if (const auto* g = qobject_cast<const QGroupBox*>(w)) return !g->title().isEmpty();
    return qobject_cast<const QLabel*>(w) || qobject_cast<const QMenu*>(w) || qobject_cast<const QMenuBar*>(w) ||
           qobject_cast<const QDockWidget*>(w) || w->isWindow();
}

bool wantsName(const QWidget* w) {
    if (w->focusPolicy() == Qt::NoFocus || namedAlready(w)) return false;
    // A spin box's or combo box's own line edit, and a scroll area's viewport, take their owner's name.
    const QWidget* parent = w->parentWidget();
    if (parent && (qobject_cast<const QAbstractSpinBox*>(parent) || qobject_cast<const QComboBox*>(parent))) return false;
    if (qobject_cast<const QScrollBar*>(w)) return false;
    return true;
}

class Namer : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override {
        // A list that drops down keeps Alt+Down (to open it) from the clip command on that key. (Open, it has the keyboard.)
        if (e->type() == QEvent::ShortcutOverride && qobject_cast<QComboBox*>(o)) {
            auto* key = static_cast<QKeyEvent*>(e);
            if (key->key() == Qt::Key_Down && key->modifiers() == Qt::AltModifier) {
                e->accept();
                return true;
            }
        }
        // (Again when shown: a widget polished before it was put in its panel may find its name there.)
        if ((e->type() == QEvent::Polish || e->type() == QEvent::Show) && o->isWidgetType()) {
            auto* w = static_cast<QWidget*>(o);
            // A scroll area that takes the focus (to scroll with the keys) is a pane to a screen reader, not a bare client.
            // (Before naming it: a name sent to a screen reader makes its interface.)
            if (auto* area = qobject_cast<QAbstractScrollArea*>(w); area && w->focusPolicy() != Qt::NoFocus &&
                !qobject_cast<QAbstractItemView*>(w) && !qobject_cast<QTextEdit*>(w) && !qobject_cast<QPlainTextEdit*>(w) &&
                !qobject_cast<TimelineWidget*>(w) && !liveTexts().contains(w))
                describe(w, {QAccessible::Pane, {}, {}, {}});
            if (wantsName(w))
                if (const QString name = inferredName(w); !name.isEmpty()) w->setAccessibleName(name);
        }
        return false;
    }
};

}  // namespace

QString inferredName(const QWidget* w) {
    // A label made its buddy.
    if (const QWidget* window = w->window())
        for (const QLabel* l : window->findChildren<QLabel*>())
            if (l->buddy() == w && !labelText(l).isEmpty()) return labelText(l);
    // The label of its row: a form's, a grid's cell to the left, or the label before it in a row of widgets.
    if (const QWidget* parent = w->parentWidget(); parent && parent->layout()) {
        std::function<QString(QLayout*)> search = [&](QLayout* layout) -> QString {
            if (auto* form = qobject_cast<QFormLayout*>(layout))
                if (auto* l = qobject_cast<QLabel*>(form->labelForField(const_cast<QWidget*>(w))); l && !labelText(l).isEmpty())
                    return labelText(l);
            if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
                const int i = grid->indexOf(w);
                if (i >= 0) {
                    int row, column, rows, columns;
                    grid->getItemPosition(i, &row, &column, &rows, &columns);
                    for (int c = column - 1; c >= 0; --c)
                        if (QLayoutItem* item = grid->itemAtPosition(row, c))
                            if (auto* l = qobject_cast<QLabel*>(item->widget()); l && !labelText(l).isEmpty()) return labelText(l);
                }
            }
            if (qobject_cast<QBoxLayout*>(layout)) {
                const int i = layout->indexOf(w);
                for (int j = i - 1; i >= 0 && j >= 0; --j) {
                    QWidget* before = layout->itemAt(j)->widget();
                    if (auto* l = qobject_cast<QLabel*>(before); l && !labelText(l).isEmpty()) return labelText(l);
                    if (before && !qobject_cast<QLabel*>(before)) break;  // (another control's label)
                }
            }
            for (int i = 0; i < layout->count(); ++i)
                if (QLayout* inner = layout->itemAt(i)->layout())
                    if (QString found = search(inner); !found.isEmpty()) return found;
            return QString();
        };
        if (QString found = search(parent->layout()); !found.isEmpty()) return found;
    }
    // Its tool tip (without the shortcut in brackets), placeholder, or object name ("sequenceTabs": "Sequence tabs").
    QString tip = w->toolTip();
    tip.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    if (!tip.trimmed().isEmpty()) {
        const qsizetype end = tip.indexOf(QRegularExpression(QStringLiteral("[\\n:—]| \\(")));
        return (end > 0 ? tip.left(end) : tip).trimmed();
    }
    if (const auto* e = qobject_cast<const QLineEdit*>(w); e && !e->placeholderText().isEmpty()) return e->placeholderText();
    if (const auto* e = qobject_cast<const QPlainTextEdit*>(w); e && !e->placeholderText().isEmpty()) return e->placeholderText();
    if (const auto* e = qobject_cast<const QTextEdit*>(w); e && !e->placeholderText().isEmpty()) return e->placeholderText();
    if (const auto* c = qobject_cast<const QComboBox*>(w); c && c->lineEdit() && !c->lineEdit()->placeholderText().isEmpty())
        return c->lineEdit()->placeholderText();
    QString name = w->objectName();
    if (name.isEmpty() || name.startsWith(QLatin1String("qt_"))) {
        // Last, the panel it is in ("Captions" for the captions' table).
        for (const QWidget* p = w->parentWidget(); p; p = p->parentWidget())
            if (const auto* dock = qobject_cast<const QDockWidget*>(p)) return dock->windowTitle();
        return QString();
    }
    name.replace(QRegularExpression(QStringLiteral("([a-z0-9])([A-Z])")), QStringLiteral("\\1 \\2"));
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    name = name.toLower();
    name[0] = name[0].toUpper();
    return name;
}

void install() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    QAccessible::installFactory(factory);
    qApp->installEventFilter(new Namer(qApp));
}

void announce(QWidget* source, const QString& text) {
    gLastAnnouncement = text;
    if (!source || text.isEmpty()) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QAccessibleAnnouncementEvent event(source, text);
    QAccessible::updateAccessibility(&event);
#else
    // (Before Qt 6.8 there is no announcement: the window's status line takes the text as its name, and alerts.)
    QWidget* target = source;
    if (auto* window = qobject_cast<QMainWindow*>(source->window())) target = window->statusBar();
    target->setAccessibleName(text);
    QAccessibleEvent event(target, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
#endif
}

QString lastAnnouncement() { return gLastAnnouncement; }

void describe(QWidget* widget, LiveText text) {
    if (!widget) return;
    const bool known = liveTexts().contains(widget);
    liveTexts().insert(widget, std::move(text));
    if (!known) QObject::connect(widget, &QObject::destroyed, [widget] { liveTexts().remove(widget); });
}

std::vector<TimelineRow> timelineRows(const Sequence& s, const std::vector<TrackRef>& shown) {
    std::vector<TimelineRow> rows;
    for (const TrackRef ref : shown) {
        const Track* t = trackAt(s, ref);
        if (!t) continue;
        TimelineRow row{ref, trackName(s, ref), {}};
        FrameTime at = 0;
        for (const Clip& c : t->clips) {
            if (c.start > at)
                row.cells.push_back({0, at, c.start,
                                     QApplication::translate("Accessibility", "Gap, %1, %2 to %3")
                                         .arg(row.name, timecodeString(&s, at), timecodeString(&s, c.start))});
            row.cells.push_back({c.id, c.start, c.end(), clipText(s, *t, ref, c)});
            at = std::max(at, c.end());
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

QString timelineValue(const Sequence& s, const std::vector<Id>& selection, Id focus) {
    QString value = QApplication::translate("Accessibility", "Playhead %1").arg(timecodeString(&s, s.playhead));
    if (selection.empty()) return value;
    if (!focus || std::find(selection.begin(), selection.end(), focus) == selection.end()) focus = selection.front();
    if (const Clip* c = edit::clipById(s, focus)) {
        value += QApplication::translate("Accessibility", ", %1 selected").arg(QString::fromStdString(c->name));
        if (selection.size() > 1)
            value += QApplication::translate("Accessibility", " with %n other(s)", nullptr, int(selection.size() - 1));
    } else {
        value += QApplication::translate("Accessibility", ", %n clip(s) selected", nullptr, int(selection.size()));
    }
    return value;
}

QString clipDescription(const Sequence& s, Id clip) {
    const auto loc = edit::locate(s, clip);
    if (!loc) return QString();
    const Track* t = trackAt(s, loc->track);
    const Clip* c = edit::clipById(s, clip);
    return t && c ? clipText(s, *t, loc->track, *c) : QString();
}

void notifyFocus(QWidget* widget) {
    if (!widget || !QAccessible::isActive() || !widget->hasFocus()) return;
    if (QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(widget))
        if (QAccessibleInterface* child = iface->focusChild()) {
            QAccessibleEvent event(child, QAccessible::Focus);
            QAccessible::updateAccessibility(&event);
        }
}

void timelineChanged(TimelineWidget* timeline) {
    if (!revisions().contains(timeline)) QObject::connect(timeline, &QObject::destroyed, [timeline] { revisions().remove(timeline); });
    ++revisions()[timeline];
    if (!QAccessible::isActive()) return;
    QAccessibleTableModelChangeEvent event(timeline, QAccessibleTableModelChangeEvent::ModelReset);
    QAccessible::updateAccessibility(&event);
}

void timelineSelectionChanged(TimelineWidget* timeline) {
    if (!QAccessible::isActive() || !timeline->state()->sequence()) return;
    QAccessibleValueChangeEvent value(timeline, timelineValue(*timeline->state()->sequence(), timeline->state()->selectedClips(),
                                                          timeline->state()->focusClip()));
    QAccessible::updateAccessibility(&value);
    QAccessibleEvent selection(timeline, QAccessible::SelectionWithin);
    QAccessible::updateAccessibility(&selection);
    notifyFocus(timeline);
}

}  // namespace montage::access
