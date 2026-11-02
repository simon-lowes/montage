#include "MidiLearn.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QAbstractSlider>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDial>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

#include "ColorWheel.h"
#include "EditorState.h"
#include "Keymap.h"
#include "MixerPanel.h"
#include "Settings.h"

namespace montage {

using midimap::Change;
using midimap::Encoding;
using midimap::Mapping;

namespace {

const QString kMergeKey = QStringLiteral("midi-learn");

double unit(double v, double lo, double hi) { return hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0, 1.0) : 0.0; }

struct Strip {
    int number = 0;   // from 1, counted from the bank
    QString control;  // volume, pan, mute, solo
};

std::optional<Strip> parseStrip(const QString& target) {
    const QStringList parts = target.split(QLatin1Char(':'));
    if (parts.size() != 3 || parts[0] != QLatin1String("strip")) return std::nullopt;
    bool ok = false;
    const int n = parts[1].toInt(&ok);
    if (!ok || n < 1) return std::nullopt;
    return Strip{n, parts[2]};
}

}  // namespace

MidiLearn::MidiLearn(EditorState* state, MixerPanel* mixer, QWidget* window, QObject* parent)
    : QObject(parent), state_(state), mixer_(mixer), window_(window) {
    folder_ = midimaps::folder();
    learnTimer_ = new QTimer(this);
    learnTimer_->setSingleShot(true);
    learnTimer_->setInterval(600);
    connect(learnTimer_, &QTimer::timeout, this, [this] {
        if (auto l = learner_.finish()) learnControl(*l);
    });
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &MidiLearn::refresh);
    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(2000);
    connect(reconnectTimer_, &QTimer::timeout, this, &MidiLearn::reconnect);
    watcher_ = new QFileSystemWatcher(this);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, &MidiLearn::mapFileChanged);
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &MidiLearn::mapFileChanged);
    // Layered mappings follow the panel with focus.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now && inApp(now)) setPanel(panelOf(now));
    });
}

MidiLearn::~MidiLearn() {
    // The connection goes first, so nothing more arrives from its thread.
    connection_.reset();
    if (learning_) qApp->removeEventFilter(this);
}

// ---- The controller.

bool MidiLearn::connectDevice(const QString& input, const QString& output, QString* error) {
    std::string inId, outId;
    for (const MidiPortInfo& p : midiInputs())
        if (QString::fromStdString(p.name) == input) inId = p.id;
    for (const MidiPortInfo& p : midiOutputs())
        if (!output.isEmpty() && QString::fromStdString(p.name) == output) outId = p.id;
    if (inId.empty()) {
        if (error) *error = tr("%1 is not plugged in").arg(input);
        return false;
    }
    std::string why;
    auto c = openMidi(
        inId, outId, [this](const MidiMessage& m) { receive(m); }, &why, [this] { connectionLost(); });
    if (!c) {
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    output_ = output;
    appSettings().setValue(QStringLiteral("midiLearn/device"), input);
    appSettings().setValue(QStringLiteral("midiLearn/output"), output);
    setConnection(std::move(c), input);
    return true;
}

void MidiLearn::setConnection(std::unique_ptr<MidiConnection> connection, const QString& device) {
    connection_.reset();
    ++generation_;
    connection_ = std::move(connection);
    reconnectTimer_->stop();
    if (!connection_) return;
    const bool another = device != device_;
    device_ = device;
    decoder_.reset();
    if (another || mapper_.mappings().empty()) loadMap();
    mapper_.resetTakeover();
    mapper_.reshow();
    refresh();
    refreshTimer_->start();
    emit connectionChanged(true);
}

void MidiLearn::disconnectDevice() {
    if (!connection_) return;
    refreshTimer_->stop();
    reconnectTimer_->stop();
    releaseHolds();
    connection_.reset();
    ++generation_;
    appSettings().remove(QStringLiteral("midiLearn/device"));
    emit connectionChanged(false);
}

void MidiLearn::restore() {
    const QString device = appSettings().value(QStringLiteral("midiLearn/device")).toString();
    if (device.isEmpty()) return;
    output_ = appSettings().value(QStringLiteral("midiLearn/output")).toString();
    device_ = device;
    loadMap();
    if (!connectDevice(device, output_)) reconnectTimer_->start();
}

void MidiLearn::receive(const MidiMessage& m) {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(m);
    if (!drainPending_) {
        drainPending_ = true;
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }
}

void MidiLearn::connectionLost() {
    const quint64 generation = generation_.load();
    QMetaObject::invokeMethod(this, [this, generation] { if (generation == generation_.load()) lost(); }, Qt::QueuedConnection);
}

void MidiLearn::lost() {
    if (!connection_) return;
    refreshTimer_->stop();
    releaseHolds();
    connection_.reset();
    ++generation_;
    emit connectionChanged(false);
    state_->message(tr("%1 unplugged: it is used again when it comes back").arg(device_), 5000);
    reconnectTimer_->start();  // matched by name, on whatever port it comes back on
}

void MidiLearn::reconnect() {
    if (connection_ || device_.isEmpty()) {
        reconnectTimer_->stop();
        return;
    }
    for (const MidiPortInfo& p : midiInputs())
        if (QString::fromStdString(p.name) == device_) {
            QString output = output_;
            bool outThere = false;
            for (const MidiPortInfo& o : midiOutputs()) outThere = outThere || QString::fromStdString(o.name) == output;
            if (connectDevice(device_, outThere ? output : QString())) state_->message(tr("%1 connected").arg(device_), 4000);
            return;
        }
}

void MidiLearn::drain() {
    std::vector<MidiMessage> batch;
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        batch.swap(inbox_);
        drainPending_ = false;
    }
    if (!connection_) return;
    for (const MidiMessage& m : batch) handle(m);
}

void MidiLearn::handle(const MidiMessage& m) {
    std::vector<midimap::Event> events;
    decoder_.feed(m, [&](const midimap::Event& e) { events.push_back(e); });
    if (events.empty()) return;
    if (learning_) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const midimap::Event& e : events) {
            // The rest of the move that was just learnt (or the halves of its 14-bit pair) is not learnt again.
            if (justLearnt_ && now - justLearntAt_ < 600) {
                const midimap::Key& k = *justLearnt_;
                const bool same = e.key == k || (k.kind == midimap::Kind::Cc14 && e.key.kind == midimap::Kind::Cc &&
                                                 e.key.channel == k.channel && (e.key.number == k.number || e.key.number == k.number + 32));
                if (same) {
                    justLearntAt_ = now;
                    continue;
                }
            }
            learner_.add(e);
        }
        if (learner_.ready()) {
            learnTimer_->stop();
            if (auto l = learner_.finish()) learnControl(*l);
        } else if (!learner_.empty()) {
            learnTimer_->start();
        }
        return;
    }
    midimap::Targets targets;
    targets.get = [this](const Mapping& map) { return targetValue(QString::fromStdString(map.target)); };
    targets.set = [this](const Mapping& map, const Change& c) { applyChange(QString::fromStdString(map.target), c); };
    // Every move from the controller is one undo step together, however many controls move at once.
    mixer_->setEditMergeKey(kMergeKey);
    for (const midimap::Event& e : events) mapper_.handle(e, targets);
    mixer_->setEditMergeKey(QString());
    refresh();
}

void MidiLearn::refresh() {
    if (!connection_) return;
    for (const MidiMessage& m : mapper_.refresh([this](const Mapping& map) { return targetValue(QString::fromStdString(map.target)); }))
        connection_->send(m);
}

// ---- Learning.

void MidiLearn::setLearning(bool on) {
    if (on == learning_) return;
    learning_ = on;
    learner_.clear();
    learnTimer_->stop();
    pendingControl_.reset();
    pendingTarget_.clear();
    if (on) {
        qApp->installEventFilter(this);
        state_->message(tr("MIDI Learn: move a control on %1, then click a parameter (or the other way round)")
                            .arg(device_.isEmpty() ? tr("the controller") : device_),
                        8000);
    } else {
        qApp->removeEventFilter(this);
        mapper_.resetTakeover();
    }
    emit learningChanged(on);
}

void MidiLearn::learnControl(const midimap::Learnt& l) {
    pendingControl_ = l;
    if (!pendingTarget_.isEmpty()) {
        complete();
        return;
    }
    state_->message(tr("MIDI Learn: %1 — now click the parameter it controls").arg(QString::fromStdString(midimap::keyLabel(l.key))), 8000);
}

void MidiLearn::learnTarget(const QString& target) {
    if (!learning_ || target.isEmpty()) return;
    pendingTarget_ = target;
    if (pendingControl_) {
        complete();
        return;
    }
    state_->message(tr("MIDI Learn: %1 — now move the control for it").arg(targetLabel(target)), 8000);
}

void MidiLearn::complete() {
    if (!pendingControl_ || pendingTarget_.isEmpty()) return;
    QString why;
    if (!midimaps::validTarget(pendingTarget_, &why)) {
        state_->message(tr("MIDI Learn: %1 cannot be mapped").arg(targetLabel(pendingTarget_)), 6000);
        pendingTarget_.clear();
        return;
    }
    justLearnt_ = pendingControl_->key;
    justLearntAt_ = QDateTime::currentMSecsSinceEpoch();
    learner_.clear();
    learnTimer_->stop();
    Mapping m;
    m.key = pendingControl_->key;
    m.encoding = pendingControl_->encoding;
    m.target = pendingTarget_.toStdString();
    if (learnInPanel_) m.layer = mapper_.layer();
    // A button on something that is on or off (mute, a check box, a checkable command) flips it at each press.
    if (m.encoding == Encoding::Button) {
        QWidget* w = widgetFor(pendingTarget_);
        const auto* b = qobject_cast<const QAbstractButton*>(w);
        QAction* a = pendingTarget_.startsWith(QLatin1String("command:")) ? keymap::find(window_, pendingTarget_.mid(8)) : nullptr;
        m.toggle = (b && b->isCheckable()) || (a && a->isCheckable());
    }
    const QString control = QString::fromStdString(midimap::keyLabel(m.key)), target = pendingTarget_;
    pendingControl_.reset();
    pendingTarget_.clear();
    addMapping(m);
    state_->message(tr("MIDI Learn: %1 controls %2").arg(control, targetLabel(target)), 6000);
    emit learnt(QString::fromStdString(midimap::keyName(m.key)), target);
}

bool MidiLearn::eventFilter(QObject* watched, QEvent* event) {
    if (!learning_) return false;
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease &&
        event->type() != QEvent::MouseButtonDblClick)
        return false;
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() != Qt::LeftButton) return false;
    // A menu command: chosen, not run.
    if (auto* menu = qobject_cast<QMenu*>(watched)) {
        if (event->type() != QEvent::MouseButtonRelease) return false;
        QAction* a = menu->actionAt(me->position().toPoint());
        const QString id = keymap::idOf(a);
        // (MIDI Learn's own commands still work: Learn is turned off from the menu that turned it on)
        const bool own = a && (a->objectName() == QLatin1String("midiLearn") || a->objectName() == QLatin1String("midiMap"));
        if (a && !a->menu() && !id.isEmpty() && !own) {
            learnTarget(QStringLiteral("command:") + id);
            for (QWidget* w = menu; w; w = qobject_cast<QMenu*>(w->parentWidget())) w->close();
            return true;
        }
        return false;
    }
    auto* w = qobject_cast<QWidget*>(watched);
    if (!w || !inApp(w)) return false;
    const QString target = targetAt(w);
    if (target.isEmpty()) return false;
    if (event->type() == QEvent::MouseButtonPress) learnTarget(target);
    return true;  // the click picks the parameter without changing it
}

// ---- Targets.

bool MidiLearn::inApp(const QWidget* w) const {
    const QWidget* top = w ? w->window() : nullptr;
    return top && (top == window_ || (qobject_cast<const QDockWidget*>(top) && top->parentWidget() == window_));
}

QString MidiLearn::panelOf(QWidget* w) const {
    for (QWidget* p = w; p && p != window_; p = p->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(p)) return d->objectName();
    return QStringLiteral("main");
}

void MidiLearn::setPanel(const QString& layer) {
    if (layer.toStdString() == mapper_.layer()) return;
    releaseHolds();
    mapper_.setLayer(layer.toStdString());
    refresh();
}

QString MidiLearn::targetAt(QWidget* w) const {
    if (!w) return {};
    // The mixer's strips, counted from the bank.
    if (mixer_ && mixer_->isAncestorOf(w)) {
        for (QWidget* p = w; p && p != mixer_; p = p->parentWidget()) {
            if (p == mixer_->masterFader()) return QStringLiteral("master:volume");
            for (int t = 0; mixer_->trackFader(t); ++t) {
                const char* what = p == mixer_->trackFader(t) ? "volume"
                                   : p == mixer_->trackPan(t) ? "pan"
                                   : p == mixer_->trackMute(t) ? "mute"
                                   : p == mixer_->trackSolo(t) ? "solo"
                                                               : nullptr;
                if (!what) continue;
                // A track outside the bank's reach: the bank moves to it (strips count from the bank).
                const int strip = t - bank_ + 1;
                if (strip < 1 || strip > 99) const_cast<MidiLearn*>(this)->setBank(t);
                return QStringLiteral("strip:%1:%2").arg(t - bank_ + 1).arg(QLatin1String(what));
            }
        }
        return {};
    }
    // A colour wheel: the axis nearer to where it was clicked.
    for (QWidget* p = w; p && p != window_; p = p->parentWidget())
        if (auto* wheel = qobject_cast<ColorWheel*>(p)) {
            if (wheel->objectName().isEmpty()) return {};
            const QPoint at = wheel->mapFromGlobal(QCursor::pos());
            const QPointF c = QRectF(wheel->rect()).center();
            const bool vertical = std::fabs(at.y() - c.y()) > std::fabs(at.x() - c.x());
            return QStringLiteral("widget:%1/%2#%3").arg(panelOf(wheel), wheel->objectName(), vertical ? "y" : "x");
        }
    // A slider, spin box, check box, button or list: the named control, or the named spin box its slider drives
    // (the Inspector's rows).
    QWidget* control = nullptr;
    for (QWidget* p = w; p && p != window_ && !control; p = p->parentWidget())
        if (qobject_cast<QAbstractSlider*>(p) || qobject_cast<QAbstractSpinBox*>(p) || qobject_cast<QAbstractButton*>(p) ||
            qobject_cast<QComboBox*>(p))
            control = p;
    if (!control) return {};
    if (control->objectName().isEmpty() && qobject_cast<QAbstractSlider*>(control) && control->parentWidget()) {
        for (auto* spin : control->parentWidget()->findChildren<QAbstractSpinBox*>(QString(), Qt::FindDirectChildrenOnly))
            if (!spin->objectName().isEmpty()) {
                control = spin;
                break;
            }
    }
    if (control->objectName().isEmpty() || control->objectName().startsWith(QLatin1String("qt_")) ||
        control->objectName().contains(QLatin1Char('#')) || control->objectName().contains(QLatin1Char('/')))
        return {};
    return QStringLiteral("widget:%1/%2").arg(panelOf(control), control->objectName());
}

QWidget* MidiLearn::widgetFor(const QString& target) const {
    if (auto s = parseStrip(target)) {
        const int t = bank_ + s->number - 1;
        if (s->control == QLatin1String("volume")) return mixer_->trackFader(t);
        if (s->control == QLatin1String("pan")) return mixer_->trackPan(t);
        if (s->control == QLatin1String("mute")) return mixer_->trackMute(t);
        if (s->control == QLatin1String("solo")) return mixer_->trackSolo(t);
        return nullptr;
    }
    if (target == QLatin1String("master:volume")) return mixer_->masterFader();
    if (!target.startsWith(QLatin1String("widget:"))) return nullptr;
    // The widget found last time, while it is still the one shown (the Inspector rebuilds its rows).
    if (const auto it = found_.find(target); it != found_.end() && it->second && it->second->isVisibleTo(window_))
        return it->second;
    QString path = target.mid(7);
    path = path.section(QLatin1Char('#'), 0, 0);
    const QString panel = path.section(QLatin1Char('/'), 0, 0), name = path.section(QLatin1Char('/'), 1);
    if (name.isEmpty()) return nullptr;
    QWidget* in = window_;
    if (panel != QLatin1String("main"))
        if (auto* d = window_->findChild<QDockWidget*>(panel)) in = d;
    QWidget* found = nullptr;
    for (QWidget* w : in->findChildren<QWidget*>(name)) {
        if (panel == QLatin1String("main") && panelOf(w) != panel) continue;
        if (w->isVisibleTo(window_)) {
            found_[target] = w;
            return w;
        }
        if (!found) found = w;
    }
    return found;
}

double MidiLearn::targetValue(const QString& target) const {
    if (target.startsWith(QLatin1String("command:"))) {
        QAction* a = keymap::find(window_, target.mid(8));
        if (!a) return std::nan("");
        return a->isCheckable() && a->isChecked() ? 1.0 : 0.0;
    }
    if (target.startsWith(QLatin1String("bank:"))) return 0;
    QWidget* w = widgetFor(target);
    if (!w || !w->isEnabled()) return std::nan("");
    if (auto* wheel = qobject_cast<ColorWheel*>(w)) {
        const QPointF p = wheel->puck();
        return std::clamp(((target.endsWith(QLatin1String("#y")) ? p.y() : p.x()) + 1) / 2, 0.0, 1.0);
    }
    if (auto* s = qobject_cast<QAbstractSlider*>(w)) return unit(s->value(), s->minimum(), s->maximum());
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) return unit(d->value(), d->minimum(), d->maximum());
    if (auto* i = qobject_cast<QSpinBox*>(w)) return unit(i->value(), i->minimum(), i->maximum());
    if (auto* c = qobject_cast<QComboBox*>(w)) return c->count() > 1 ? unit(c->currentIndex(), 0, c->count() - 1) : 0.0;
    if (auto* b = qobject_cast<QAbstractButton*>(w)) return b->isCheckable() && b->isChecked() ? 1.0 : 0.0;
    return std::nan("");
}

void MidiLearn::applyChange(const QString& target, const Change& c) {
    const bool press = c.edge == Change::Press;
    if (target.startsWith(QLatin1String("command:"))) {
        QAction* a = keymap::find(window_, target.mid(8));
        if (!a || !a->isEnabled()) return;
        if (a->isCheckable() ? (c.value >= 0.5) != a->isChecked() && c.edge != Change::None : press) a->trigger();
        return;
    }
    if (target.startsWith(QLatin1String("bank:"))) {
        if (press) setBank(bank_ + (target.endsWith(QLatin1String("previous")) ? -bankSize_ : bankSize_));
        return;
    }
    QWidget* w = widgetFor(target);
    if (!w || !w->isEnabled()) return;
    if (auto* wheel = qobject_cast<ColorWheel*>(w)) {
        QPointF p = wheel->puck();
        const double v = std::clamp(c.value * 2 - 1, -1.0, 1.0);
        (target.endsWith(QLatin1String("#y")) ? p.ry() : p.rx()) = v;
        if (std::hypot(p.x(), p.y()) > 1) p /= std::hypot(p.x(), p.y());  // (the puck stays in the disc)
        wheel->setPuck(p, false);
        hold(target, wheel);  // its last move is final once the control rests
        return;
    }
    if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
        // Held while it moves, as a mouse holds it: Touch and Latch write a track's automation while held.
        if (!s->isSliderDown()) s->setSliderDown(true);
        hold(target, s);
        s->setValue(int(std::lround(s->minimum() + c.value * (s->maximum() - s->minimum()))));
        return;
    }
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
        d->setValue(d->minimum() + c.value * (d->maximum() - d->minimum()));
        return;
    }
    if (auto* i = qobject_cast<QSpinBox*>(w)) {
        i->setValue(int(std::lround(i->minimum() + c.value * (i->maximum() - i->minimum()))));
        return;
    }
    if (auto* box = qobject_cast<QComboBox*>(w)) {
        const int index = box->count() > 1 ? int(std::lround(c.value * (box->count() - 1))) : 0;
        if (index != box->currentIndex() && index < box->count()) {
            box->setCurrentIndex(index);
            emit box->activated(index);
        }
        return;
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (b->isCheckable() ? (c.value >= 0.5) != b->isChecked() && c.edge != Change::None : press) b->click();
    }
}

void MidiLearn::hold(const QString& target, QObject* what) {
    QTimer*& t = holds_[target];
    if (!t) {
        t = new QTimer(this);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, target] { release(target); });
    }
    t->setInterval(holdMs_);
    t->start();
    Q_UNUSED(what);
}

void MidiLearn::release(const QString& target) {
    auto it = holds_.find(target);
    if (it != holds_.end()) it->second->stop();
    QWidget* w = widgetFor(target);
    if (auto* wheel = qobject_cast<ColorWheel*>(w)) {
        wheel->setPuck(wheel->puck(), true);
    } else if (auto* s = qobject_cast<QAbstractSlider*>(w); s && s->isSliderDown()) {
        mixer_->setEditMergeKey(kMergeKey);
        s->setSliderDown(false);
        mixer_->setEditMergeKey(QString());
    }
}

void MidiLearn::releaseHolds() {
    std::vector<QString> held;
    for (const auto& [target, timer] : holds_)
        if (timer->isActive()) held.push_back(target);
    for (const QString& t : held) release(t);
}

QString MidiLearn::targetLabel(const QString& target) const {
    if (target.startsWith(QLatin1String("command:"))) {
        QAction* a = keymap::find(window_, target.mid(8));
        return a ? a->text().remove(QLatin1Char('&')).remove(QStringLiteral("…")) : target.mid(8);
    }
    if (auto s = parseStrip(target)) {
        const QString what = s->control == QLatin1String("volume") ? tr("volume")
                             : s->control == QLatin1String("pan")  ? tr("pan")
                             : s->control == QLatin1String("mute") ? tr("mute")
                                                                    : tr("solo");
        return tr("strip %1 %2").arg(s->number).arg(what);
    }
    if (target == QLatin1String("master:volume")) return tr("master volume");
    if (target == QLatin1String("bank:previous")) return tr("previous bank");
    if (target == QLatin1String("bank:next")) return tr("next bank");
    QWidget* w = widgetFor(target);
    if (!w) return target.mid(target.indexOf(QLatin1Char(':')) + 1);
    QString name = w->accessibleName();
    if (name.isEmpty()) {
        // The Inspector's rows: the label beside the row.
        for (QWidget* p = w; p && name.isEmpty(); p = p->parentWidget())
            if (QWidget* parent = p->parentWidget())
                if (auto* form = qobject_cast<QFormLayout*>(parent->layout()))
                    if (auto* label = qobject_cast<QLabel*>(form->labelForField(p))) name = label->text().remove(QLatin1Char(':'));
    }
    if (name.isEmpty())
        if (auto* b = qobject_cast<QAbstractButton*>(w)) name = b->text().remove(QLatin1Char('&'));
    if (name.isEmpty()) name = w->toolTip();
    if (name.isEmpty()) name = w->objectName();
    if (target.endsWith(QLatin1String("#x"))) name += tr(" (across)");
    if (target.endsWith(QLatin1String("#y"))) name += tr(" (up and down)");
    return name;
}

// ---- The map.

void MidiLearn::setBank(int bank) {
    int tracks = 0;
    while (mixer_->trackFader(tracks)) ++tracks;
    bank = std::clamp(bank, 0, std::max(0, tracks - 1));
    if (bank == bank_) return;
    releaseHolds();
    bank_ = bank;
    mapper_.resetTakeover();
    mapper_.reshow();
    refresh();
    state_->message(tr("MIDI strips: tracks %1 to %2").arg(bank_ + 1).arg(bank_ + bankSize_), 2500);
}

void MidiLearn::setBankSize(int n) {
    bankSize_ = std::clamp(n, 1, 64);
    saveMap();
}

void MidiLearn::setFolder(const QString& folder) {
    folder_ = folder;
    loadMap();
}

QString MidiLearn::mapFile() const { return device_.isEmpty() ? QString() : midimaps::fileFor(folder_, device_); }

void MidiLearn::addMapping(const Mapping& m) {
    mapper_.map(m);
    saveMap();
    emit mapChanged();
}

void MidiLearn::setMapping(size_t index, const Mapping& m) {
    if (index >= mapper_.mappings().size()) return;
    const Mapping old = mapper_.mappings()[index];
    if (old.key == m.key && old.layer == m.layer) {
        mapper_.setMappingOptions(index, m);
    } else {
        mapper_.unmap(old.key, old.layer);
        mapper_.map(m);
    }
    saveMap();
    emit mapChanged();
}

void MidiLearn::removeMapping(size_t index) {
    if (index >= mapper_.mappings().size()) return;
    const Mapping m = mapper_.mappings()[index];
    mapper_.unmap(m.key, m.layer);
    saveMap();
    emit mapChanged();
}

void MidiLearn::loadMap() {
    if (!watcher_->files().isEmpty()) watcher_->removePaths(watcher_->files());
    if (!watcher_->directories().isEmpty()) watcher_->removePaths(watcher_->directories());
    const QString file = mapFile();
    loadFailed_ = false;
    found_.clear();
    if (file.isEmpty()) {
        mapper_.clear();
        bankSize_ = 8;
        emit mapChanged();
        return;
    }
    midimaps::Map map;
    QString why;
    QStringList skipped;
    if (!QFile::exists(file)) {
        mapper_.clear();
        bankSize_ = 8;
    } else {
        if (midimaps::load(file, map, &why, &skipped)) {
            mapper_.clear();
            for (const Mapping& m : map.mappings) mapper_.map(m);
            bankSize_ = map.bankSize;
            if (!skipped.isEmpty()) {
                loadFailed_ = true;  // (what was left out is kept in a copy before the file is next written)
                state_->message(tr("MIDI map: %1 left out (%2)").arg(skipped.size()).arg(skipped.join(QStringLiteral("; "))), 8000);
            }
        } else {
            // Not readable (half written, or edited wrongly): what was in use is kept, and the file kept aside
            // before it is written over.
            loadFailed_ = true;
            state_->message(tr("MIDI map not read: %1").arg(why), 6000);
        }
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) saved_ = f.readAll();
        watcher_->addPath(file);
    }
    QDir().mkpath(folder_);
    watcher_->addPath(folder_);  // (a map written for the first time, e.g. by the MCP tool)
    emit mapChanged();
}

void MidiLearn::saveMap() {
    const QString file = mapFile();
    if (file.isEmpty()) return;
    midimaps::Map map;
    map.device = device_;
    map.bankSize = bankSize_;
    map.mappings = mapper_.mappings();
    if (loadFailed_ && QFile::exists(file)) {
        const QString aside = file + QStringLiteral(".unread");
        QFile::remove(aside);
        QFile::copy(file, aside);
        loadFailed_ = false;
    }
    saved_ = QJsonDocument(midimaps::toJson(map)).toJson(QJsonDocument::Indented);
    QString why;
    if (!midimaps::save(file, map, &why)) state_->message(tr("MIDI map not saved: %1").arg(why), 6000);
    if (!watcher_->files().contains(file)) watcher_->addPath(file);
}

void MidiLearn::mapFileChanged() {
    // Changed by someone else (the MCP tool, a person editing it): read again.
    const QString file = mapFile();
    if (file.isEmpty() || !QFile::exists(file)) return;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray now = f.readAll();
    if (now == saved_) {
        if (!watcher_->files().contains(file)) watcher_->addPath(file);
        return;
    }
    loadMap();
    mapper_.reshow();
    refresh();
}

}  // namespace montage

// ---- The dialog.

namespace montage {

QDialog* MidiLearn::openDialog(QWidget* parent) {
    if (auto* open = parent->findChild<QDialog*>(QStringLiteral("midiLearnDialog"))) {
        open->raise();
        open->activateWindow();
        return open;
    }
    auto* dlg = new QDialog(parent);
    dlg->setObjectName(QStringLiteral("midiLearnDialog"));
    dlg->setWindowTitle(tr("MIDI Learn"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto* v = new QVBoxLayout(dlg);
    auto* intro = new QLabel(tr("Any MIDI controller: turn Learn on, move a knob, fader, encoder or button, then click what it "
                                "should control in the Inspector, the mixer, a colour wheel or a menu. Relative encoders and "
                                "14-bit faders are recognised; a fader takes over a parameter once it reaches its value."),
                             dlg);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* form = new QFormLayout;
    v->addLayout(form);
    auto* input = new QComboBox(dlg);
    input->setObjectName(QStringLiteral("midiLearnInput"));
    input->setAccessibleName(tr("MIDI input"));
    auto* output = new QComboBox(dlg);
    output->setObjectName(QStringLiteral("midiLearnOutput"));
    output->setAccessibleName(tr("MIDI output"));
    output->addItem(tr("None"), QString());
    for (const MidiPortInfo& p : midiInputs()) input->addItem(QString::fromStdString(p.name));
    for (const MidiPortInfo& p : midiOutputs()) output->addItem(QString::fromStdString(p.name), QString::fromStdString(p.name));
    if (const int at = input->findText(device_); at >= 0) input->setCurrentIndex(at);
    const int outAt = output->findData(output_.isEmpty() ? input->currentText() : output_);
    output->setCurrentIndex(outAt >= 0 ? outAt : 0);
    form->addRow(tr("MIDI input:"), input);
    form->addRow(tr("Lights (MIDI output):"), output);
    auto* status = new QLabel(dlg);
    status->setObjectName(QStringLiteral("midiLearnStatus"));
    status->setWordWrap(true);
    form->addRow(status);
    auto showStatus = [this, status] {
        status->setText(isConnected() ? tr("Connected to %1; its map: %2").arg(device_, QDir::toNativeSeparators(mapFile()))
                                      : device_.isEmpty() ? tr("Not connected.")
                                                          : tr("%1 is not connected (it is used when it is plugged in).").arg(device_));
    };
    showStatus();
    connect(this, &MidiLearn::connectionChanged, status, showStatus);
    auto* portButtons = new QHBoxLayout;
    auto* connectButton = new QPushButton(tr("Connect"), dlg);
    connectButton->setObjectName(QStringLiteral("midiLearnConnect"));
    auto* disconnectButton = new QPushButton(tr("Disconnect"), dlg);
    disconnectButton->setObjectName(QStringLiteral("midiLearnDisconnect"));
    portButtons->addWidget(connectButton);
    portButtons->addWidget(disconnectButton);
    portButtons->addStretch(1);
    form->addRow(portButtons);
    connect(connectButton, &QPushButton::clicked, dlg, [this, input, output, status] {
        if (input->currentText().isEmpty()) {
            status->setText(tr("No MIDI input: plug the controller in and open this again."));
            return;
        }
        QString why;
        if (!connectDevice(input->currentText(), output->currentData().toString(), &why)) status->setText(why);
    });
    connect(disconnectButton, &QPushButton::clicked, this, &MidiLearn::disconnectDevice);

    auto* learnRow = new QHBoxLayout;
    auto* learn = new QPushButton(tr("Learn"), dlg);
    learn->setObjectName(QStringLiteral("midiLearnToggle"));
    learn->setCheckable(true);
    learn->setChecked(learning_);
    auto* inPanel = new QCheckBox(tr("Only while the panel clicked in has focus"), dlg);
    inPanel->setObjectName(QStringLiteral("midiLearnInPanel"));
    inPanel->setChecked(learnInPanel_);
    learnRow->addWidget(learn);
    learnRow->addWidget(inPanel, 1);
    v->addLayout(learnRow);
    connect(learn, &QPushButton::toggled, this, &MidiLearn::setLearning);
    connect(this, &MidiLearn::learningChanged, learn, [learn](bool on) {
        const QSignalBlocker block(learn);
        learn->setChecked(on);
    });
    connect(inPanel, &QCheckBox::toggled, this, &MidiLearn::setLearnInPanel);

    auto* bankRow = new QHBoxLayout;
    auto* bankSize = new QSpinBox(dlg);
    bankSize->setObjectName(QStringLiteral("midiLearnBankSize"));
    bankSize->setAccessibleName(tr("Strips in a bank"));
    bankSize->setRange(1, 64);
    bankSize->setValue(bankSize_);
    bankRow->addWidget(new QLabel(tr("Mixer strips in a bank:"), dlg));
    bankRow->addWidget(bankSize);
    bankRow->addStretch(1);
    v->addLayout(bankRow);
    connect(bankSize, &QSpinBox::valueChanged, this, &MidiLearn::setBankSize);

    auto* table = new QTableWidget(dlg);
    table->setObjectName(QStringLiteral("midiLearnMap"));
    table->setAccessibleName(tr("MIDI map"));
    table->setColumnCount(9);
    table->setHorizontalHeaderLabels({tr("Control"), tr("Controls"), tr("Panel"), tr("From"), tr("To"), tr("Curve"),
                                      tr("Invert"), tr("Toggle"), tr("Takeover")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    v->addWidget(table, 1);
    auto fillTable = [this, table] {
        const QSignalBlocker block(table);
        const auto& maps = mapper_.mappings();
        table->setRowCount(int(maps.size()));
        for (int r = 0; r < int(maps.size()); ++r) {
            const Mapping& m = maps[size_t(r)];
            auto text = [&](int c, const QString& s, bool editable) {
                auto* item = new QTableWidgetItem(s);
                if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                table->setItem(r, c, item);
            };
            auto check = [&](int c, bool on, bool applies) {
                auto* item = new QTableWidgetItem;
                item->setFlags(applies ? (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::ItemIsSelectable);
                if (applies) item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
                table->setItem(r, c, item);
            };
            QString control = QString::fromStdString(midimap::keyLabel(m.key));
            if (m.encoding != Encoding::Absolute && m.encoding != Encoding::Button)
                control += QStringLiteral(" · ") + tr("relative");
            text(0, control, false);
            text(1, targetLabel(QString::fromStdString(m.target)), false);
            text(2, m.layer.empty() ? tr("Every panel") : QString::fromStdString(m.layer), false);
            text(3, QString::number(m.low * 100, 'g', 4), true);
            text(4, QString::number(m.high * 100, 'g', 4), true);
            text(5, QString::number(m.curve, 'g', 3), m.encoding == Encoding::Absolute);
            check(6, m.invert, true);
            check(7, m.toggle, m.encoding == Encoding::Button);
            check(8, m.pickup, m.encoding == Encoding::Absolute);
        }
    };
    fillTable();
    connect(this, &MidiLearn::mapChanged, table, fillTable);
    connect(table, &QTableWidget::itemChanged, this, [this, table, fillTable](QTableWidgetItem* item) {
        const size_t r = size_t(item->row());
        if (r >= mapper_.mappings().size()) return;
        Mapping m = mapper_.mappings()[r];
        bool ok = true;
        const double number = item->text().toDouble(&ok);
        switch (item->column()) {
            case 3: if (ok) m.low = std::clamp(number / 100, 0.0, 1.0); break;
            case 4: if (ok) m.high = std::clamp(number / 100, 0.0, 1.0); break;
            case 5: if (ok) m.curve = std::clamp(number, 0.05, 20.0); break;
            case 6: m.invert = item->checkState() == Qt::Checked; break;
            case 7: m.toggle = item->checkState() == Qt::Checked; break;
            case 8: m.pickup = item->checkState() == Qt::Checked; break;
            default: break;
        }
        setMapping(r, m);
        QTimer::singleShot(0, table, fillTable);  // (shows what was kept)
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QPushButton* remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    remove->setObjectName(QStringLiteral("midiLearnRemove"));
    connect(remove, &QPushButton::clicked, this, [this, table] {
        std::vector<int> rows;
        for (const QModelIndex& i : table->selectionModel()->selectedRows()) rows.push_back(i.row());
        std::sort(rows.rbegin(), rows.rend());
        for (int r : rows) removeMapping(size_t(r));
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    v->addWidget(buttons);
    dlg->resize(760, 520);
    dlg->show();
    return dlg;
}

}  // namespace montage
