#include "OscControl.h"

#include <QAction>
#include <QDial>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "Keymap.h"
#include "LiveLink.h"
#include "MixerPanel.h"
#include "PlaybackController.h"
#include "core/History.h"

namespace montage {

namespace {

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

}  // namespace

OscControl::OscControl(EditorState* state, MixerPanel* mixer, PlaybackController* program, LiveLink* link, QWidget* window,
                       QObject* parent)
    : QObject(parent), state_(state), mixer_(mixer), program_(program), link_(link), window_(window) {
    feedbackTimer_ = new QTimer(this);
    feedbackTimer_->setInterval(100);
    connect(feedbackTimer_, &QTimer::timeout, this, &OscControl::sendFeedback);
}

OscControl::~OscControl() = default;

bool OscControl::start(quint16 port, bool network, QString* error) {
    stop();
    auto* socket = new QUdpSocket(this);
    if (!socket->bind(network ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::LocalHost), port)) {
        if (error) *error = tr("Cannot listen for OSC on port %1: %2").arg(port).arg(socket->errorString());
        delete socket;
        return false;
    }
    socket_ = socket;
    connect(socket_, &QUdpSocket::readyRead, this, &OscControl::readPending);
    playingSent_ = loopSent_ = -1;
    timecodeSent_.clear();
    feedbackTimer_->start();
    emit runningChanged(true);
    return true;
}

void OscControl::stop() {
    if (!socket_) return;
    feedbackTimer_->stop();
    socket_->close();
    socket_->deleteLater();
    socket_ = nullptr;
    emit runningChanged(false);
}

bool OscControl::isRunning() const { return socket_ != nullptr; }
quint16 OscControl::port() const { return socket_ ? socket_->localPort() : 0; }

void OscControl::readPending() {
    while (socket_ && socket_->hasPendingDatagrams()) {
        const QNetworkDatagram d = socket_->receiveDatagram();
        std::vector<osc::Message> messages;
        std::string err;
        const QByteArray data = d.data();
        if (!osc::parse(std::string(data.constData(), size_t(data.size())), messages, &err)) {
            peer_ = d.senderAddress();
            send({"/montage/error", {osc::string(err)}});
            continue;
        }
        for (const osc::Message& m : messages) handle(m, d.senderAddress());
    }
}

void OscControl::send(const osc::Message& m) {
    if (!socket_ || !feedbackPort_ || peer_.isNull()) return;
    const std::string bytes = osc::encode(m);
    socket_->writeDatagram(bytes.data(), qint64(bytes.size()), peer_, feedbackPort_);
}

void OscControl::sendFeedback() {
    if (!feedbackPort_ || peer_.isNull()) return;
    const int playing = program_->isPlaying() ? 1 : 0, loop = program_->loop() ? 1 : 0;
    if (playing != playingSent_) send({"/montage/state/playing", {osc::int32(playingSent_ = playing)}});
    if (loop != loopSent_) send({"/montage/state/loop", {osc::int32(loopSent_ = loop)}});
    const Sequence* s = state_->sequence();
    const QString tc = s ? QString::fromStdString(formatTimecode(state_->playhead(), s->fps)) : QString();
    if (tc != timecodeSent_) send({"/montage/state/timecode", {osc::string((timecodeSent_ = tc).toStdString())}});
}

QString OscControl::slug(const QString& commandId) {
    QString out;
    for (const QString& part : commandId.split(QLatin1Char('/'))) {
        QString p = part.toLower();
        p.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
        p.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
        if (!out.isEmpty()) out += QLatin1Char('/');
        out += p;
    }
    return out;
}

QList<std::pair<QString, QString>> OscControl::commandAddresses() const {
    QList<std::pair<QString, QString>> out;
    for (QAction* a : keymap::actions(window_)) {
        const QString id = keymap::idOf(a);
        if (!id.isEmpty()) out.push_back({QStringLiteral("/montage/command/") + slug(id), id});
    }
    return out;
}

bool OscControl::command(const QString& name, QString* error) {
    const auto it = commands_.find(name);
    if (it == commands_.end() || !it->second) {
        if (error) *error = tr("%1 is not available").arg(name);
        return false;
    }
    it->second();
    return true;
}

bool OscControl::runTool(const QString& name, const osc::Message& m, QString* error) {
    QJsonObject args;
    if (!m.args.empty()) {
        const QByteArray json = QByteArray::fromStdString(m.args.front().text());
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
        if (!doc.isObject()) {
            if (error) *error = tr("a tool's argument is a JSON object, like {\"text\": \"Hi\"}");
            return false;
        }
        args = doc.object();
    }
    const QString tool = name.startsWith(QLatin1String("montage_")) ? name : QStringLiteral("montage_") + name;
    const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", ++calls_}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}};
    // One undo step for the call, its answer sent back when it comes (the window stays free meanwhile).
    const QString reply = QStringLiteral("/montage/reply/") + name;
    link_->handleScript(compact(msg), [this, reply](QByteArray answer) {
        const QJsonObject r = QJsonDocument::fromJson(answer).object();
        QString text;
        if (r.contains("error")) {
            text = r.value("error").toObject().value("message").toString();
            send({"/montage/error", {osc::string(text.toStdString())}});
            return;
        }
        const QJsonObject result = r.value("result").toObject();
        for (const QJsonValue& c : result.value("content").toArray()) text += c.toObject().value("text").toString();
        send({result.value("isError").toBool() ? std::string("/montage/error") : reply.toStdString(), {osc::string(text.toStdString())}});
    }, tr("OSC: %1").arg(name), QStringLiteral("osc-%1").arg(calls_));
    return true;
}

bool OscControl::handle(const osc::Message& m, const QHostAddress& from, QString* error) {
    peer_ = from;
    const QString address = QString::fromStdString(m.address);
    QString err;
    auto fail = [&](const QString& e) {
        err = e;
        return false;
    };
    auto arg = [&](size_t i) -> const osc::Value* { return i < m.args.size() ? &m.args[i] : nullptr; };
    // A button press sends 1 and its release 0 (Companion, TouchOSC): a release does nothing.
    const bool released = !m.args.empty() && (m.args.front().type == 'F' || (m.args.front().type == 'i' && m.args.front().i == 0)) &&
                          !address.contains(QLatin1String("/track/")) && !address.startsWith(QLatin1String("/montage/master/")) &&
                          !address.startsWith(QLatin1String("/montage/command/")) &&
                          address != QLatin1String("/montage/loop") && address != QLatin1String("/montage/shuttle") &&
                          address != QLatin1String("/montage/step") && address != QLatin1String("/montage/goto");
    bool ok = true;
    const Sequence* seq = state_->sequence();
    if (!address.startsWith(QLatin1String("/montage/"))) {
        ok = fail(tr("%1 is not a Montage address (they start /montage/)").arg(address));
    } else if (released) {
        ok = true;
    } else if (address == QLatin1String("/montage/play")) {
        if (program_->isPlaying() && program_->speed() != 1.0) program_->shuttle(0);
        if (!program_->isPlaying()) program_->togglePlay();
    } else if (address == QLatin1String("/montage/stop")) {
        if (program_->isPlaying()) program_->shuttle(0);
    } else if (address == QLatin1String("/montage/playpause")) {
        program_->togglePlay();
    } else if (address == QLatin1String("/montage/rewind")) {
        program_->shuttle(-1);
    } else if (address == QLatin1String("/montage/forward")) {
        program_->shuttle(1);
    } else if (address == QLatin1String("/montage/shuttle")) {
        bool n = false;
        const double d = arg(0) ? arg(0)->number(&n) : 0;
        if (!n) ok = fail(tr("/montage/shuttle takes -1, 0 or 1"));
        else program_->shuttle(d > 0 ? 1 : d < 0 ? -1 : 0);
    } else if (address == QLatin1String("/montage/step")) {
        bool n = false;
        const double d = arg(0) ? arg(0)->number(&n) : 1;
        if (arg(0) && !n) ok = fail(tr("/montage/step takes a number of frames"));
        else if (!program_->isPlaying()) program_->step(int(std::lround(d)));
    } else if (address == QLatin1String("/montage/goto")) {
        if (!seq || !arg(0)) {
            ok = fail(tr("/montage/goto takes a timecode or seconds"));
        } else {
            FrameTime t = 0;
            bool n = false;
            const double secs = arg(0)->number(&n);
            if (n) t = FrameTime(std::llround(secs * seq->fps.num / seq->fps.den));
            else if (!parseTimecode(arg(0)->text(), seq->fps, t)) ok = fail(tr("/montage/goto: \"%1\" is not a timecode").arg(QString::fromStdString(arg(0)->text())));
            if (ok) program_->seek(std::max<FrameTime>(0, t));
        }
    } else if (address == QLatin1String("/montage/start")) {
        program_->seek(0);
    } else if (address == QLatin1String("/montage/end")) {
        if (seq) program_->seek(seq->duration());
    } else if (address == QLatin1String("/montage/in")) {
        state_->setInPoint(state_->playhead());
    } else if (address == QLatin1String("/montage/out")) {
        state_->setOutPoint(state_->playhead());
    } else if (address == QLatin1String("/montage/marker")) {
        if (arg(0) && (arg(0)->type == 's' || arg(0)->type == 'S') && seq) {
            const FrameTime at = state_->playhead();
            const std::string name = arg(0)->s;
            state_->edit(tr("Add Marker"), [at, name](Project&, Sequence& s) {
                edit::addMarker(s, Marker{at, 0, name, "", 0});
                return true;
            });
        } else {
            ok = command(QStringLiteral("marker"), &err);
        }
    } else if (address == QLatin1String("/montage/loop")) {
        const bool want = arg(0) ? arg(0)->truthy() : !program_->loop();
        if (want != program_->loop()) ok = command(QStringLiteral("loop"), &err);
    } else if (address == QLatin1String("/montage/undo")) {
        state_->undo();
    } else if (address == QLatin1String("/montage/redo")) {
        state_->redo();
    } else if (address == QLatin1String("/montage/save")) {
        ok = command(QStringLiteral("save"), &err);
    } else if (address == QLatin1String("/montage/record")) {
        ok = command(QStringLiteral("record"), &err);
    } else if (address == QLatin1String("/montage/previous_edit")) {
        ok = command(QStringLiteral("previousEdit"), &err);
    } else if (address == QLatin1String("/montage/next_edit")) {
        ok = command(QStringLiteral("nextEdit"), &err);
    } else if (address.startsWith(QLatin1String("/montage/command/"))) {
        // A menu command by its Keyboard Shortcuts id, written as an address (or the id itself).
        const QString want = address.mid(17);
        QAction* found = nullptr;
        for (QAction* a : keymap::actions(window_)) {
            const QString id = keymap::idOf(a);
            if (!id.isEmpty() && (slug(id) == want || id == want)) {
                found = a;
                if (a->isEnabled()) break;
            }
        }
        const bool zero = arg(0) && (arg(0)->type == 'F' || ((arg(0)->type == 'i' || arg(0)->type == 'f') && arg(0)->number() == 0));
        if (!found) ok = fail(tr("No menu command at %1").arg(address));
        else if (!found->isEnabled()) ok = fail(tr("%1 is not available now").arg(keymap::idOf(found)));
        else if (found->isCheckable() && arg(0) && arg(0)->type != 'N') {
            // A switch: the value sets it (on or off), as the menu's own tick would.
            if (found->isChecked() != arg(0)->truthy()) found->trigger();
        } else if (!zero) {
            found->trigger();  // (a button's release, 0, does nothing)
        }
    } else if (address.startsWith(QLatin1String("/montage/tool/"))) {
        const QString name = address.mid(14);
        if (name.isEmpty() || name.contains(QLatin1Char('/'))) ok = fail(tr("/montage/tool/<name> names one tool"));
        else ok = runTool(name, m, &err);
    } else if (address.startsWith(QLatin1String("/montage/track/")) || address.startsWith(QLatin1String("/montage/master/"))) {
        // The mixer, as its own controls do it (automation passes record it); a move is one undo step.
        const bool master = address.startsWith(QLatin1String("/montage/master/"));
        const QStringList parts = address.mid(master ? 16 : 15).split(QLatin1Char('/'));
        bool numbered = master;
        const int track = master ? 0 : parts.value(0).toInt(&numbered) - 1;
        const QString what = master ? parts.value(0) : parts.value(1);
        QSlider* fader = master ? mixer_->masterFader() : mixer_->trackFader(track);
        if (!numbered || (!master && parts.size() != 2) || (master && parts.size() != 1) || !fader) {
            ok = fail(tr("No audio track at %1").arg(address));
        } else {
            mixer_->setEditMergeKey(QStringLiteral("osc-mixer"));
            bool n = false;
            const double v = arg(0) ? arg(0)->number(&n) : 0;
            if (what == QLatin1String("volume")) {
                if (!n) ok = fail(tr("%1 takes decibels").arg(address));
                else fader->setValue(int(std::lround(std::clamp(v, -60.0, 12.0) * 10)));
            } else if (!master && (what == QLatin1String("mute") || what == QLatin1String("solo"))) {
                QToolButton* b = what == QLatin1String("mute") ? mixer_->trackMute(track) : mixer_->trackSolo(track);
                if (!b) {
                    ok = fail(tr("No audio track at %1").arg(address));
                } else {
                    const bool want = arg(0) ? arg(0)->truthy() : !b->isChecked();
                    if (b->isChecked() != want) b->click();
                }
            } else if (!master && what == QLatin1String("pan")) {
                QDial* pan = mixer_->trackPan(track);
                if (!n) ok = fail(tr("%1 takes -1 (left) to 1 (right)").arg(address));
                else if (!pan || !pan->isVisibleTo(mixer_)) ok = fail(tr("Track %1 pans in two dimensions").arg(track + 1));
                else pan->setValue(int(std::lround(std::clamp(v, -1.0, 1.0) * 100)));
            } else {
                ok = fail(tr("No mixer control at %1").arg(address));
            }
            mixer_->setEditMergeKey(QString());
        }
    } else {
        ok = fail(tr("No Montage address %1").arg(address));
    }
    if (!ok) send({"/montage/error", {osc::string(err.toStdString())}});
    if (error) *error = err;
    emit handled(address, ok);
    return ok;
}

}  // namespace montage
