#include "TangentControl.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "Keymap.h"
#include "PlaybackController.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/GradeGraph.h"
#include "render/Compositor.h"

namespace montage {

namespace {

const EffectInfo* colorCorrectInfo() { return findEffectInfo("color_correct"); }

const ParamInfo* paramInfo(const std::string& name) {
    if (const EffectInfo* info = colorCorrectInfo())
        for (const ParamInfo& p : info->params)
            if (p.name == name) return &p;
    return nullptr;
}

// A stable id from a name (FNV-1a), in a range of its own.
uint32_t hashed(uint32_t base, const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return base | (h & 0x00FFFFFF);
}

// The Color Correct the clip's node grades with (its first, on or off).
Effect* colorCorrect(Clip& c, int node) {
    for (Effect& e : c.effects)
        if (e.type == "color_correct" && e.node == node) return &e;
    return nullptr;
}

const char* kBuiltIns[][2] = {{"tangent:play", QT_TRANSLATE_NOOP("TangentControl", "Play / Stop")},
                              {"tangent:next_node", QT_TRANSLATE_NOOP("TangentControl", "Next Node")},
                              {"tangent:previous_node", QT_TRANSLATE_NOOP("TangentControl", "Previous Node")}};

}  // namespace

TangentControl::TangentControl(EditorState* state, QWidget* window, Hooks hooks, QObject* parent)
    : QObject(parent), state_(state), window_(window), hooks_(std::move(hooks)) {
    retry_ = new QTimer(this);
    retry_->setInterval(5000);
    connect(retry_, &QTimer::timeout, this, &TangentControl::tryConnect);
    watch_ = new QTimer(this);
    watch_->setInterval(200);
    connect(watch_, &QTimer::timeout, this, &TangentControl::watch);
    if (const EffectInfo* info = colorCorrectInfo())
        for (const ParamInfo& p : info->params)
            if (p.kind == ParamKind::Number) parameters_[parameterId(p.name)] = p.name;
}

TangentControl::~TangentControl() = default;

uint32_t TangentControl::parameterId(const std::string& name) { return hashed(0x03000000, name); }

uint32_t TangentControl::actionId(const QString& command) const {
    auto it = actionIds_.find(command);
    return it == actionIds_.end() ? 0 : it->second;
}

QString TangentControl::folder() const {
    if (!folder_.isEmpty()) return folder_;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tangent");
}

bool TangentControl::isConnected() const { return socket_ && socket_->state() == QAbstractSocket::ConnectedState; }

void TangentControl::setEnabled(bool on) {
    enabled_ = on;
    if (on) {
        tryConnect();
        retry_->start();
    } else {
        retry_->stop();
        watch_->stop();
        if (socket_) {
            socket_->abort();
            socket_->deleteLater();
            socket_ = nullptr;
            introduced_ = false;
            emit connectionChanged();
        }
    }
}

void TangentControl::tryConnect() {
    if (!enabled_ || socket_) return;
    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::readyRead, this, &TangentControl::readHub);
    connect(socket_, &QTcpSocket::connected, this, [this] {
        error_.clear();
        emit connectionChanged();
    });
    connect(socket_, &QTcpSocket::disconnected, this, &TangentControl::lost);
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!socket_) return;
        error_ = tr("Tangent Hub is not running (%1)").arg(socket_->errorString());
        lost();
    });
    decoder_ = {};
    socket_->connectToHost(QStringLiteral("127.0.0.1"), port_);
}

void TangentControl::lost() {
    if (!socket_) return;
    const bool was = introduced_;
    socket_->deleteLater();
    socket_ = nullptr;
    introduced_ = false;
    watch_->stop();
    if (was) emit connectionChanged();
}

void TangentControl::send(const std::string& bytes) {
    if (socket_ && socket_->state() == QAbstractSocket::ConnectedState) socket_->write(bytes.data(), qint64(bytes.size()));
}

void TangentControl::readHub() {
    if (!socket_) return;
    const QByteArray bytes = socket_->readAll();
    std::vector<tangent::HubMessage> messages;
    std::string err;
    const bool ok = decoder_.feed(std::string(bytes.constData(), size_t(bytes.size())), messages, &err);
    for (const tangent::HubMessage& m : messages) handle(m);
    if (!ok) {
        error_ = QString::fromStdString(err);
        if (socket_) socket_->abort();
    }
}

void TangentControl::buildActions() {
    actions_.clear();
    actionIds_.clear();
    auto add = [&](const QString& command) {
        uint32_t id = hashed(0x05000000, command.toStdString());
        while (actions_.count(id)) id = 0x05000000 | ((id + 1) & 0x00FFFFFF);  // (two names hashed alike)
        actions_[id] = command;
        actionIds_[command] = id;
    };
    for (const auto& b : kBuiltIns) add(QString::fromLatin1(b[0]));
    if (window_)
        for (QAction* a : keymap::actions(window_)) add(keymap::idOf(a));
}

tangent::ControlSystem TangentControl::controls() const {
    tangent::ControlSystem cs;
    cs.modes = {{kMode, "Colour"}};
    tangent::Group primaries{"Primaries", {}, {}, {}};
    std::map<std::string, std::vector<uint32_t>> wheels;  // lift, gamma, gain: r, g, b, then the master
    if (const EffectInfo* info = colorCorrectInfo())
        for (const ParamInfo& p : info->params) {
            if (p.kind != ParamKind::Number) continue;
            tangent::Control c;
            c.id = parameterId(p.name);
            c.name = p.label;
            c.shortName = p.label.substr(0, 9);
            c.min = p.min, c.max = p.max, c.step = p.step > 0 ? p.step : 0.01;
            primaries.controls.push_back(c);
            const std::string wheel = p.name.substr(0, p.name.find('_'));
            if (wheel == "lift" || wheel == "gamma" || wheel == "gain") wheels[wheel].push_back(c.id);
        }
    for (const char* w : {"lift", "gamma", "gain"}) {
        std::vector<uint32_t> m = wheels[w];
        if (m.size() == 4) std::rotate(m.begin(), m.begin() + 1, m.end());  // (the master after red, green, blue)
        std::string name = w;
        name[0] = char(std::toupper(name[0]));
        primaries.bindings.push_back({name, m});
    }
    cs.groups.push_back(primaries);
    tangent::Group nodes{"Nodes", {}, {}, {}};
    tangent::Control menu;
    menu.kind = tangent::Control::Menu;
    menu.id = kNodeMenu;
    menu.name = "Node";
    nodes.controls.push_back(menu);
    cs.groups.push_back(nodes);
    // Commands, by menu.
    tangent::Group commands{"Commands", {}, {}, {}};
    std::map<QString, tangent::Group> menus;
    for (const auto& [id, command] : actions_) {
        tangent::Control a;
        a.kind = tangent::Control::Action;
        a.id = id;
        QString group, label;
        bool builtIn = false;
        for (const auto& b : kBuiltIns)
            if (command == QLatin1String(b[0])) builtIn = true, group = tr("Montage"), label = tr(b[1]);
        if (!builtIn) {
            QAction* act = window_ ? keymap::find(window_, command) : nullptr;
            group = command.section('/', 0, 0);
            label = act ? act->text().remove('&') : command.section('/', 1);
        }
        a.name = label.toStdString();
        menus[group].name = group.toStdString();
        menus[group].controls.push_back(a);
    }
    for (auto& [name, g] : menus) {
        std::sort(g.controls.begin(), g.controls.end(), [](const tangent::Control& a, const tangent::Control& b) { return a.name < b.name; });
        commands.groups.push_back(std::move(g));
    }
    cs.groups.push_back(commands);
    return cs;
}

void TangentControl::introduce() {
    buildActions();
    const QString dir = folder();
    QDir().mkpath(dir);
    auto write = [&](const QString& name, const std::string& text) {
        QFile f(dir + QLatin1Char('/') + name);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.data(), qint64(text.size()));
    };
    write(QStringLiteral("controls.xml"), tangent::controlsXml(controls()));
    const auto [mapName, map] = tangent::emptyMap();
    if (!QFile::exists(dir + QLatin1Char('/') + QString::fromStdString(mapName))) write(QString::fromStdString(mapName), map);
    send(tangent::applicationDefinition("Montage", QDir::toNativeSeparators(dir).toStdString(), ""));
    send(tangent::modeValue(kMode));
    introduced_ = true;
    displayed_.clear();
    watched_.clear();
    watch();
    watch_->start();
    emit connectionChanged();
}

TangentControl::Target TangentControl::target() const {
    Target t;
    const Sequence* s = state_->sequence();
    if (!s) return t;
    auto picture = [&](Id id) -> const Clip* {
        const auto loc = edit::locate(*s, id);
        return loc && loc->track.kind == TrackKind::Video ? edit::clipById(*s, id) : nullptr;
    };
    const Clip* c = state_->primaryClip() ? picture(state_->primaryClip()->id) : nullptr;
    if (!c) {
        const std::vector<bool> mattes = hiddenMatteTracks(*s, state_->playhead());
        for (int i = int(s->videoTracks.size()) - 1; i >= 0 && !c; --i)
            if (!s->videoTracks[size_t(i)].muted && !mattes[size_t(i)])
                if (const Clip* under = edit::clipAt(*s, TrackRef{TrackKind::Video, i}, state_->playhead()); under && under->enabled) c = under;
    }
    if (!c) return t;
    t.clip = c->id;
    t.at = std::max<FrameTime>(0, state_->playhead() - c->start);
    if (!c->grade.empty()) {
        const int picked = state_->primaryClip() && state_->primaryClip()->id == c->id ? state_->selectedGradeNode() : 0;
        const GradeNode* n = picked ? findGradeNode(c->grade, picked) : nullptr;
        if (n && n->kind == "corrector") {
            t.node = picked;
        } else {
            for (int key : gradeOrder(c->grade))
                if (const GradeNode* k = findGradeNode(c->grade, key); k && k->kind == "corrector") {
                    t.node = key;
                    break;
                }
        }
    }
    return t;
}

double TangentControl::value(const std::string& name) const {
    const ParamInfo* info = paramInfo(name);
    const double def = info ? info->def : 0;
    const Target t = target();
    const Sequence* s = state_->sequence();
    if (!t.clip || !s) return def;
    Clip* c = edit::clipById(const_cast<Sequence&>(*s), t.clip);
    const Effect* e = c ? colorCorrect(*c, t.node) : nullptr;
    return e ? e->p(name, t.at, def) : def;
}

bool TangentControl::change(const std::string& name, double to, bool reset) {
    const ParamInfo* info = paramInfo(name);
    if (!info) return false;
    const Target t = target();
    if (!t.clip) {
        state_->message(tr("Select a clip to grade"), 3000);
        return false;
    }
    const double v = reset ? info->def : std::clamp(to, info->min, info->max);
    const QString label = tr("%1 (Tangent)").arg(QString::fromStdString(info->label));
    const QString key = QStringLiteral("tangent:%1:%2:%3").arg(t.clip).arg(t.node).arg(QString::fromStdString(name));
    return state_->edit(label, [t, name, v](Project& p, Sequence& s) {
        Clip* c = edit::clipById(s, t.clip);
        if (!c) return false;
        Effect* e = colorCorrect(*c, t.node);
        if (!e) {
            Effect made = makeEffect(p, "color_correct");
            made.node = t.node;
            c->effects.push_back(made);
            e = &c->effects.back();
        }
        e->params[name].set(t.at, v);
        return true;
    }, key);
}

void TangentControl::sendValue(uint32_t id) {
    auto it = parameters_.find(id);
    if (it == parameters_.end()) return;
    const ParamInfo* info = paramInfo(it->second);
    const double v = value(it->second);
    send(tangent::parameterValue(id, float(v), info && std::fabs(v - info->def) < 1e-9));
}

void TangentControl::sendMenu() {
    const Target t = target();
    const Sequence* s = state_->sequence();
    const Clip* c = s && t.clip ? edit::clipById(const_cast<Sequence&>(*s), t.clip) : nullptr;
    std::string text = "Clip";
    if (c && t.node)
        if (const GradeNode* n = findGradeNode(c->grade, t.node)) text = gradeNodeName(*n);
    if (!c) text = "No clip";
    send(tangent::menuString(kNodeMenu, text, false));
}

void TangentControl::sendDisplay() {
    const Target t = target();
    const Sequence* s = state_->sequence();
    const Clip* c = s && t.clip ? edit::clipById(const_cast<Sequence&>(*s), t.clip) : nullptr;
    std::vector<std::string> lines;
    if (!c) {
        lines = {"Montage", "No clip"};
    } else {
        lines.push_back(c->name.empty() ? "Clip" : c->name);
        const GradeNode* n = t.node ? findGradeNode(c->grade, t.node) : nullptr;
        lines.push_back(n ? gradeNodeName(*n) : std::string("Color Correct"));
    }
    QString shown;
    for (const std::string& l : lines) shown += QString::fromStdString(l) + QLatin1Char('\n');
    if (shown == displayed_) return;
    displayed_ = shown;
    send(tangent::displayText(lines));
}

void TangentControl::watch() {
    if (!introduced_) return;
    // What the panels show: the clip, its node, and its values there.
    const Target t = target();
    QString now = QStringLiteral("%1/%2/").arg(t.clip).arg(t.node);
    for (const auto& [id, name] : parameters_) now += QString::number(value(name), 'g', 9) + QLatin1Char(',');
    if (now != watched_) {
        const bool first = watched_.isEmpty();
        watched_ = now;
        if (!first) send(tangent::allChange());
    }
    sendDisplay();
}

void TangentControl::command(const QString& id, bool on) {
    if (!on) return;
    if (id == QLatin1String("tangent:play")) {
        if (PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr) c->togglePlay();
        return;
    }
    if (id == QLatin1String("tangent:next_node") || id == QLatin1String("tangent:previous_node")) {
        tangent::HubMessage m;
        m.command = tangent::MenuChange;
        m.id = kNodeMenu;
        m.steps = id == QLatin1String("tangent:next_node") ? 1 : -1;
        handle(m);
        return;
    }
    if (QAction* a = window_ ? keymap::find(window_, id) : nullptr; a && a->isEnabled()) a->trigger();
}

void TangentControl::handle(const tangent::HubMessage& m) {
    switch (m.command) {
        case tangent::InitiateComms:
            introduce();
            break;
        case tangent::ParameterChange:
            if (auto it = parameters_.find(m.id); it != parameters_.end()) {
                change(it->second, value(it->second) + double(m.increment), false);
                sendValue(m.id);
                watched_.clear();  // (its own change: the Hub knows)
                watch();
            }
            break;
        case tangent::ParameterReset:
            if (auto it = parameters_.find(m.id); it != parameters_.end()) {
                change(it->second, 0, true);
                sendValue(m.id);
                watched_.clear();
                watch();
            }
            break;
        case tangent::ParameterValueRequest:
            sendValue(m.id);
            break;
        case tangent::MenuChange:
        case tangent::MenuReset:
            if (m.id == kNodeMenu) {
                const Target t = target();
                const Sequence* s = state_->sequence();
                const Clip* c = s && t.clip ? edit::clipById(const_cast<Sequence&>(*s), t.clip) : nullptr;
                if (c && !c->grade.empty() && state_->primaryClip() && state_->primaryClip()->id == c->id) {
                    std::vector<int> correctors;
                    for (int key : gradeOrder(c->grade))
                        if (const GradeNode* n = findGradeNode(c->grade, key); n && n->kind == "corrector") correctors.push_back(key);
                    if (!correctors.empty()) {
                        int i = int(std::find(correctors.begin(), correctors.end(), t.node) - correctors.begin());
                        i = m.command == tangent::MenuReset ? 0 : std::clamp(i + (m.steps < 0 ? -1 : 1), 0, int(correctors.size()) - 1);
                        state_->selectGradeNode(correctors[size_t(i)]);
                    }
                }
                sendMenu();
                watched_.clear();
                watch();
            }
            break;
        case tangent::MenuStringRequest:
            if (m.id == kNodeMenu) sendMenu();
            break;
        case tangent::ActionOn:
        case tangent::ActionOff:
            if (auto it = actions_.find(m.id); it != actions_.end()) command(it->second, m.command == tangent::ActionOn);
            break;
        case tangent::ModeChange:
            send(tangent::modeValue(kMode));
            break;
        case tangent::Transport: {
            PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr;
            if (!c) break;
            if (!m.jog && !m.shuttle) c->pause();
            if (m.jog) {
                if (c->isPlaying()) c->pause();
                c->step(m.jog);
            }
            if (m.shuttle) {
                const int n = std::clamp(std::abs(m.shuttle), 1, 6);
                c->setSpeed((m.shuttle < 0 ? -1.0 : 1.0) * double(1 << (n - 1)));
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace montage
