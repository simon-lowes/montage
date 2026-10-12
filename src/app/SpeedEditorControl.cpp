#include "SpeedEditorControl.h"

#include <QAction>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "Keymap.h"
#include "PlaybackController.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

using speed::Key;

namespace {
constexpr int kDoublePressMs = 400;
constexpr double kLevelStep = 0.5;  // dB a step of the wheel

struct Binding {
    Key key;
    const char* what;
    const char* second;  // pressed twice
};
const Binding kBindings[] = {
    {Key::SmartInsert, "Clip/Smart Insert", ""},
    {Key::Append, "Clip/Append at End", ""},
    {Key::RippleOverwrite, "Clip/Ripple Overwrite", ""},
    {Key::CloseUp, "Clip/Close Up", ""},
    {Key::PlaceOnTop, "Clip/Place on Top", ""},
    {Key::SourceOverwrite, "Clip/Overwrite from Source", ""},
    {Key::In, "Sequence/Mark In", "Sequence/Clear In"},
    {Key::Out, "Sequence/Mark Out", "Sequence/Clear Out"},
    {Key::TrimIn, "speed:trim_in", ""},
    {Key::TrimOut, "speed:trim_out", ""},
    {Key::Roll, "speed:roll", "speed:slide"},
    {Key::SlipSource, "speed:slip_source", ""},
    {Key::SlipDest, "speed:slip_dest", ""},
    {Key::TransitionDuration, "speed:transition_duration", ""},
    {Key::Cut, "speed:cut", ""},
    {Key::Dissolve, "speed:dissolve", ""},
    {Key::SmoothCut, "speed:smooth_cut", ""},
    {Key::Escape, "speed:escape", "Edit/Undo"},
    {Key::SyncBin, "Clip/Match Frame", ""},
    {Key::AudioLevel, "speed:audio_level", "Sequence/Add Marker"},
    {Key::FullView, "Playback/Full Screen Program", ""},
    {Key::Transition, "Clip/Apply Default Transition", "Clip/New Title"},
    {Key::Split, "Clip/Add Edit", ""},
    {Key::Snap, "Sequence/Snapping", ""},
    {Key::RippleDelete, "Edit/Ripple Delete", ""},
    {Key::Cam1, "speed:cam1", ""},
    {Key::Cam2, "speed:cam2", ""},
    {Key::Cam3, "speed:cam3", ""},
    {Key::Cam4, "speed:cam4", ""},
    {Key::Cam5, "speed:cam5", ""},
    {Key::Cam6, "speed:cam6", ""},
    {Key::Cam7, "speed:cam7", ""},
    {Key::Cam8, "speed:cam8", ""},
    {Key::Cam9, "speed:cam9", ""},
    {Key::LiveOverwrite, "speed:live_overwrite", ""},
    {Key::VideoOnly, "Clip/Edit Video Only", ""},
    {Key::AudioOnly, "Clip/Edit Audio Only", ""},
    {Key::StopPlay, "speed:play", ""},
    {Key::Source, "speed:source", ""},
    {Key::Timeline, "speed:timeline", ""},
    {Key::Shuttle, "speed:shuttle", ""},
    {Key::Jog, "speed:jog", ""},
    {Key::Scroll, "speed:scroll", ""},
    {Key::Freeze, "Clip/Add Frame Hold", ""},
    {Key::Swap, "Clip/Swap with Next Clip", ""},
    {Key::Insert, "Clip/Insert from Source", ""},
    {Key::Overwrite, "Clip/Overwrite from Source", ""},
    {Key::Replace, "Clip/Replace with Source Clip", ""},
    {Key::FitToFill, "Clip/Fit to Fill", ""},
    {Key::TrimEditor, "speed:roll", ""},
};

const Binding* bindingOf(Key k) {
    for (const Binding& b : kBindings)
        if (b.key == k) return &b;
    return nullptr;
}

// Built-in functions and what the settings call them.
const std::pair<const char*, const char*> kBuiltIns[] = {
    {"speed:jog", "Wheel: jog"},
    {"speed:shuttle", "Wheel: shuttle"},
    {"speed:scroll", "Wheel: scroll"},
    {"speed:source", "Source monitor"},
    {"speed:timeline", "Timeline (Program monitor)"},
    {"speed:play", "Stop / play"},
    {"speed:trim_in", "Trim the incoming side (wheel)"},
    {"speed:trim_out", "Trim the outgoing side (wheel)"},
    {"speed:roll", "Roll the edit (wheel)"},
    {"speed:slide", "Slide the clip (wheel)"},
    {"speed:slip_source", "Slip the clip after the edit (wheel)"},
    {"speed:slip_dest", "Slip the clip before the edit (wheel)"},
    {"speed:audio_level", "Audio level (hold, wheel)"},
    {"speed:transition_duration", "Transition length (hold, wheel)"},
    {"speed:cut", "Cut at the edit"},
    {"speed:dissolve", "Dissolve at the edit"},
    {"speed:smooth_cut", "Smooth cut at the edit"},
    {"speed:escape", "End trimming, slipping or sliding"},
    {"speed:cam1", "Multicam angle 1"},
    {"speed:cam2", "Multicam angle 2"},
    {"speed:cam3", "Multicam angle 3"},
    {"speed:cam4", "Multicam angle 4"},
    {"speed:cam5", "Multicam angle 5"},
    {"speed:cam6", "Multicam angle 6"},
    {"speed:cam7", "Multicam angle 7"},
    {"speed:cam8", "Multicam angle 8"},
    {"speed:cam9", "Multicam angle 9"},
    {"speed:live_overwrite", "Live overwrite (angles cut when stopped too)"},
};
}  // namespace

SpeedEditorControl::SpeedEditorControl(EditorState* state, QWidget* window, Hooks hooks, QObject* parent)
    : QObject(parent), state_(state), window_(window), hooks_(std::move(hooks)) {
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(3000);
    connect(pollTimer_, &QTimer::timeout, this, &SpeedEditorControl::poll);
    authTimer_ = new QTimer(this);
    authTimer_->setSingleShot(true);
    connect(authTimer_, &QTimer::timeout, this, [this] {
        if (device_ && !renew()) authTimer_->start(15000);  // tried again soon (it keeps working a little longer)
    });
    // The lights follow the editor: the angle shown, the edit's transition, switches.
    ledTimer_ = new QTimer(this);
    ledTimer_->setInterval(250);
    connect(ledTimer_, &QTimer::timeout, this, &SpeedEditorControl::refreshLeds);
    deferTimer_ = new QTimer(this);
    deferTimer_->setSingleShot(true);
    deferTimer_->setInterval(kDoublePressMs);
    connect(deferTimer_, &QTimer::timeout, this, [this] {
        const Key k = std::exchange(deferred_, Key::None);
        if (k != Key::None) run(binding(k), k);
    });
}

SpeedEditorControl::~SpeedEditorControl() {
    // (the device's thread stopped before the object it reports to goes)
    device_.reset();
}

void SpeedEditorControl::setEnabled(bool on) {
    enabled_ = on;
    if (on) {
        pollTimer_->start();
        QTimer::singleShot(0, this, &SpeedEditorControl::poll);
    } else {
        pollTimer_->stop();
        detach();
    }
}

void SpeedEditorControl::poll() {
    if (!enabled_ || device_) return;
    std::vector<HidDeviceInfo> found;
    for (uint16_t product : {speed::kSpeedEditor, speed::kEditorKeyboard})
        for (const HidDeviceInfo& d : hidDevices(speed::kVendor, product)) found.push_back(d);
    // Paths no longer there may be another device next time.
    for (auto it = refused_.begin(); it != refused_.end();)
        it = std::any_of(found.begin(), found.end(), [&](const HidDeviceInfo& d) { return d.path == *it; }) ? std::next(it) : refused_.erase(it);
    for (const HidDeviceInfo& d : found) {
        if (refused_.count(d.path)) continue;
        std::string err;
        auto dev = openHid(
            d.path,
            [this](const uint8_t* data, size_t n) {
                std::vector<uint8_t> report(data, data + n);
                QMetaObject::invokeMethod(this, [this, report] { feed(report); }, Qt::QueuedConnection);
            },
            [this] { QMetaObject::invokeMethod(this, [this] { lost(); }, Qt::QueuedConnection); }, &err);
        if (!dev) {
            error_ = QString::fromStdString(err);
            refused_.insert(d.path);
            emit connectionChanged();
            continue;
        }
        QString why;
        if (attach(std::move(dev), d, &why)) return;
        refused_.insert(d.path);  // (the Editor Keyboard's ordinary keyboard: no challenge there)
    }
}

void SpeedEditorControl::lost() {
    if (!device_) return;
    detach();
    state_->message(tr("The Blackmagic keyboard went away"), 4000);
}

bool SpeedEditorControl::attach(std::unique_ptr<HidDevice> device, const HidDeviceInfo& info, QString* error) {
    detach();
    device_ = std::move(device);
    product_ = info.product;
    std::string why;
    const int seconds = speed::authenticate([this](const std::vector<uint8_t>& r) { return device_->sendFeature(r); },
                                            [this](uint8_t id, std::vector<uint8_t>& r) { return device_->getFeature(id, r); }, &why);
    if (seconds <= 0) {
        device_.reset();
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    authTimer_->start(std::max(1, seconds - 30) * 1000);  // renewed half a minute early
    name_ = !info.name.empty() ? QString::fromStdString(info.name)
            : info.product == speed::kEditorKeyboard ? tr("DaVinci Resolve Editor Keyboard")
                                                     : tr("DaVinci Resolve Speed Editor");
    error_.clear();
    decoder_.reset();
    battery_ = -1;
    charging_ = false;
    accumulated_ = 0;
    shuttleSent_ = 0;
    target_ = Target::None;
    holding_ = Key::None;
    ledsKnown_ = false;
    setJogMode(jogMode_);
    refreshLeds();
    ledTimer_->start();
    emit connectionChanged();
    return true;
}

void SpeedEditorControl::detach() {
    if (!device_) return;
    ledTimer_->stop();
    authTimer_->stop();
    deferTimer_->stop();
    deferred_ = Key::None;
    if (jogMode_ == Key::Shuttle && shuttleSent_ != 0)
        if (PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr) c->setSpeed(0);
    shuttleSent_ = 0;
    device_.reset();
    name_.clear();
    battery_ = -1;
    emit connectionChanged();
}

bool SpeedEditorControl::renew() {
    if (!device_) return false;
    std::string why;
    const int seconds = speed::authenticate([this](const std::vector<uint8_t>& r) { return device_->sendFeature(r); },
                                            [this](uint8_t id, std::vector<uint8_t>& r) { return device_->getFeature(id, r); }, &why);
    if (seconds <= 0) {
        error_ = QString::fromStdString(why);
        return false;
    }
    authTimer_->start(std::max(1, seconds - 30) * 1000);
    return true;
}

void SpeedEditorControl::send(const std::vector<uint8_t>& report) {
    if (device_) device_->write(report);
}

void SpeedEditorControl::feed(const std::vector<uint8_t>& report) {
    if (!device_) return;
    for (const speed::Event& e : decoder_.feed(report.data(), report.size())) {
        switch (e.kind) {
            case speed::Event::KeyDown: keyDown(e.key); break;
            case speed::Event::KeyUp: keyUp(e.key); break;
            case speed::Event::Jog: jog(e.mode, e.value); break;
            case speed::Event::Battery:
                if (battery_ != e.level || charging_ != e.charging) {
                    battery_ = e.level;
                    charging_ = e.charging;
                    emit batteryChanged();
                }
                break;
            case speed::Event::None: break;
        }
    }
}

// ---- Keys

QString SpeedEditorControl::defaultBinding(Key k) {
    const Binding* b = bindingOf(k);
    return b ? QString::fromLatin1(b->what) : QString();
}

QString SpeedEditorControl::binding(Key k) const {
    const auto it = changes_.find(k);
    return it != changes_.end() ? it->second : defaultBinding(k);
}

QString SpeedEditorControl::secondBinding(Key k) const {
    const Binding* b = bindingOf(k);
    return b && binding(k) == defaultBinding(k) ? QString::fromLatin1(b->second) : QString();
}

void SpeedEditorControl::setBinding(Key k, const QString& what) {
    if (what == defaultBinding(k)) changes_.erase(k);
    else changes_[k] = what;
}

QJsonObject SpeedEditorControl::bindingChanges() const {
    QJsonObject out;
    for (const auto& [k, what] : changes_) out.insert(QString::fromStdString(speed::keyName(k)), what);
    return out;
}

void SpeedEditorControl::setBindingChanges(const QJsonObject& changes) {
    changes_.clear();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        Key k;
        if (speed::keyFromName(it.key().toStdString(), k) && it.value().isString()) setBinding(k, it.value().toString());
    }
}

QStringList SpeedEditorControl::builtIns() {
    QStringList out;
    for (const auto& [id, label] : kBuiltIns) out << QString::fromLatin1(id);
    return out;
}

QString SpeedEditorControl::builtInLabel(const QString& id) {
    for (const auto& [i, label] : kBuiltIns)
        if (id == QLatin1String(i)) return tr(label);
    return id;
}

void SpeedEditorControl::keyDown(Key k) {
    const QString second = secondBinding(k);
    if (k == lastKey_ && lastPress_.isValid() && lastPress_.elapsed() < kDoublePressMs && !second.isEmpty()) {
        // Pressed twice: what the brackets say (a primary still waiting is dropped).
        lastKey_ = Key::None;
        if (deferred_ == k) {
            deferTimer_->stop();
            deferred_ = Key::None;
        }
        if (holding_ == k) {
            holding_ = Key::None;
            target_ = Target::None;
        }
        run(second, k);
        return;
    }
    lastKey_ = k;
    lastPress_.restart();
    // TRANS waits to see whether it is pressed again (else a title would come with a transition).
    if (k == Key::Transition && !second.isEmpty()) {
        deferred_ = k;
        deferTimer_->start();
        return;
    }
    run(binding(k), k);
}

void SpeedEditorControl::keyUp(Key k) {
    if (holding_ != k) return;
    holding_ = Key::None;
    if (target_ == Target::Level || target_ == Target::TransitionDuration) target_ = Target::None;
}

void SpeedEditorControl::run(const QString& what, Key k) {
    if (what.isEmpty()) return;
    if (what.startsWith(QLatin1String("speed:"))) {
        runBuiltIn(what, k);
    } else if (QAction* a = keymap::find(window_, what); !a) {
        state_->message(tr("No command %1").arg(what), 4000);
    } else if (!a->isEnabled()) {
        state_->message(tr("%1 is not available now").arg(a->text().remove(QLatin1Char('&'))), 4000);
    } else {
        a->trigger();
    }
    refreshLeds();  // what it changed shows at once
}

void SpeedEditorControl::runBuiltIn(const QString& id, Key k) {
    PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr;
    const QString name = id.mid(6);
    if (name == QLatin1String("jog")) return setJogMode(Key::Jog);
    if (name == QLatin1String("shuttle")) return setJogMode(Key::Shuttle);
    if (name == QLatin1String("scroll")) return setJogMode(Key::Scroll);
    if (name == QLatin1String("source") || name == QLatin1String("timeline")) {
        if (hooks_.showMonitor) hooks_.showMonitor(name == QLatin1String("source"));
        return;
    }
    if (name == QLatin1String("play")) {
        if (c) c->togglePlay();
        shuttleSent_ = 0;
        return;
    }
    if (name.startsWith(QLatin1String("cam"))) {
        const int angle = name.mid(3).toInt() - 1;
        if (hooks_.switchAngle && !hooks_.switchAngle(angle, liveOverwrite_)) state_->message(tr("No multicam clip at the playhead"), 3000);
        refreshLeds();
        return;
    }
    if (name == QLatin1String("live_overwrite")) {
        liveOverwrite_ = !liveOverwrite_;
        state_->message(liveOverwrite_ ? tr("Live overwrite: angles cut at the playhead") : tr("Live overwrite off"), 3000);
        refreshLeds();
        return;
    }
    if (name == QLatin1String("cut")) return void(setEditTransition(""));
    if (name == QLatin1String("dissolve")) return void(setEditTransition("cross_dissolve"));
    if (name == QLatin1String("smooth_cut")) return void(setEditTransition("smooth_cut"));
    if (name == QLatin1String("audio_level") || name == QLatin1String("transition_duration")) {
        target_ = name == QLatin1String("audio_level") ? Target::Level : Target::TransitionDuration;
        holding_ = k;
        ++gesture_;
        accumulated_ = 0;
        return;
    }
    if (name == QLatin1String("escape")) {
        target_ = Target::None;
        targetClip_ = 0;
        if (QAction* a = keymap::find(window_, QStringLiteral("Sequence/End Trim Mode"))) a->trigger();
        return;
    }
    const Sequence* s = state_->sequence();
    if (!s) return;
    if (name == QLatin1String("trim_in") || name == QLatin1String("trim_out") || name == QLatin1String("roll")) {
        target_ = Target::None;
        accumulated_ = 0;
        if (hooks_.selectEdit) hooks_.selectEdit(name == QLatin1String("roll") ? 0 : name == QLatin1String("trim_out") ? 1 : 2);
        return;
    }
    if (name == QLatin1String("slip_source") || name == QLatin1String("slip_dest") || name == QLatin1String("slide")) {
        Id outgoing = 0, incoming = 0;
        FrameTime at = 0;
        Id clip = 0;
        if (name == QLatin1String("slide")) {
            if (const Clip* under = edit::clipAt(*s, {TrackKind::Video, state_->targetVideoTrack()}, state_->playhead())) clip = under->id;
        } else if (targetEdit(outgoing, incoming, at)) {
            clip = name == QLatin1String("slip_source") ? incoming : outgoing;
        }
        if (!clip) return state_->message(tr("No clip there on the target video track"), 3000);
        if (QAction* a = keymap::find(window_, QStringLiteral("Sequence/End Trim Mode")); a && hooks_.trimming && hooks_.trimming()) a->trigger();
        target_ = name == QLatin1String("slide") ? Target::Slide : Target::Slip;
        targetClip_ = clip;
        ++gesture_;
        accumulated_ = 0;
        const Clip* cl = edit::clipById(*s, clip);
        state_->message((target_ == Target::Slide ? tr("Sliding %1: turn the wheel (ESC ends)") : tr("Slipping %1: turn the wheel (ESC ends)"))
                            .arg(cl ? QString::fromStdString(cl->name) : QString()),
                        4000);
        return;
    }
}

// ---- The wheel

void SpeedEditorControl::setJogMode(Key mode) {
    if (mode != Key::Jog && mode != Key::Shuttle && mode != Key::Scroll) return;
    if (jogMode_ == Key::Shuttle && mode != Key::Shuttle && shuttleSent_ != 0) {
        if (PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr) c->setSpeed(0);  // leaving the shuttle stops it
    }
    jogMode_ = mode;
    accumulated_ = 0;
    shuttleSent_ = 0;
    send(speed::jogModeReport(mode == Key::Shuttle ? speed::JogMode::AbsoluteDeadZero : speed::JogMode::Relative2));
    refreshLeds();
}

double SpeedEditorControl::shuttleSpeed(int32_t position) {
    // Eight zones each way over the half turn, a sixteenth of a turn each: a quarter speed up to 32 times.
    const int v = std::min(std::abs(position), speed::kJogHalfTurn);
    if (v < 64) return 0;
    static const double speeds[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
    const int zone = std::min(7, v * 8 / (speed::kJogHalfTurn + 1));
    return position < 0 ? -speeds[zone] : speeds[zone];
}

void SpeedEditorControl::jog(speed::JogMode mode, int32_t value) {
    const bool absolute = mode == speed::JogMode::Absolute || mode == speed::JogMode::AbsoluteDeadZero;
    if (jogMode_ == Key::Shuttle) {
        if (!absolute) return;  // (the mode is still changing)
        const double v = shuttleSpeed(value);
        if (v == shuttleSent_) return;
        shuttleSent_ = v;
        if (PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr) c->setSpeed(v);
        return;
    }
    if (absolute) return;
    accumulated_ += value;
    const int steps = accumulated_ / speed::kJogStep;
    accumulated_ -= steps * speed::kJogStep;
    if (steps) turn(steps);
}

void SpeedEditorControl::turn(int steps) {
    const Sequence* s = state_->sequence();
    PlaybackController* c = hooks_.controller ? hooks_.controller() : nullptr;
    const double fps = s && s->fpsValue() > 0 ? s->fpsValue() : 25.0;
    const FrameTime frames = jogMode_ == Key::Scroll ? FrameTime(steps) * std::max<FrameTime>(1, FrameTime(std::lround(fps / 4))) : steps;
    const QString key = QStringLiteral("speed-editor:%1").arg(gesture_);
    switch (target_) {
        case Target::Slip:
        case Target::Slide: {
            const Id clip = targetClip_;
            const bool slide = target_ == Target::Slide;
            QString why;
            const bool ok = state_->edit(
                slide ? tr("Slide") : tr("Slip"),
                [&](Project& p, Sequence& sq) {
                    const edit::Result r = slide ? edit::slide(p, sq, clip, frames) : edit::slip(p, sq, clip, frames);
                    why = QString::fromStdString(r.error);
                    return r.ok;
                },
                key);
            if (!ok && !why.isEmpty()) state_->message(why, 3000);
            return;
        }
        case Target::Level: {
            if (!s) return;
            // The selected sound clips, else the sound at the playhead on the target audio track (and its links).
            std::vector<Id> ids;
            for (Id id : state_->selectedClips())
                for (Id l : edit::linkedClips(*s, id))
                    if (auto loc = edit::locate(*s, l); loc && loc->track.kind == TrackKind::Audio && std::find(ids.begin(), ids.end(), l) == ids.end())
                        ids.push_back(l);
            if (ids.empty())
                if (const Clip* under = edit::clipAt(*s, {TrackKind::Audio, state_->targetAudioTrack()}, state_->playhead()))
                    for (Id l : edit::linkedClips(*s, under->id))
                        if (auto loc = edit::locate(*s, l); loc && loc->track.kind == TrackKind::Audio) ids.push_back(l);
            if (ids.empty()) return state_->message(tr("No sound at the playhead on the target audio track"), 3000);
            const FrameTime at = state_->playhead();
            double level = 0;
            state_->edit(
                tr("Audio Level"),
                [&](Project& p, Sequence& sq) {
                    for (Id id : ids) {
                        Clip* cl = edit::clipById(sq, id);
                        if (!cl) continue;
                        if (cl->audio.empty()) cl->audio = makeEffect(p, "volume");
                        Param& g = cl->audio.params["gain_db"];
                        const FrameTime t = std::clamp<FrameTime>(at - cl->start, 0, std::max<FrameTime>(0, cl->duration - 1));
                        level = std::clamp(g.at(t) + kLevelStep * steps, -60.0, 24.0);
                        g.set(t, level);
                    }
                    return true;
                },
                key);
            state_->message(tr("Audio level %1 dB").arg(level, 0, 'f', 1), 2000);
            return;
        }
        case Target::TransitionDuration: {
            Id outgoing = 0, incoming = 0;
            FrameTime at = 0;
            if (!s || !targetEdit(outgoing, incoming, at)) return;
            const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()});
            const Transition* tr0 = nullptr;
            for (const Transition& tr : t->transitions)
                if (tr.clipA == outgoing && tr.clipB == incoming) tr0 = &tr;
            if (!tr0) return state_->message(tr("No transition at the edit"), 3000);
            const Id id = tr0->id;
            const FrameTime want = std::max<FrameTime>(2, tr0->duration + frames);
            FrameTime set = want;
            state_->edit(
                tr("Transition Duration"),
                [&](Project&, Sequence& sq) {
                    const edit::Result r = edit::setTransitionDuration(sq, id, want);
                    if (r.ok && r.applied > 0) set = r.applied;
                    return r.ok;
                },
                key);
            state_->message(tr("Transition %1 frames").arg(set), 2000);
            return;
        }
        case Target::None: break;
    }
    if (hooks_.trimming && hooks_.trimming()) {
        if (hooks_.trimBy) hooks_.trimBy(frames);
        return;
    }
    if (c) c->step(int(frames));
}

// ---- The edit nearest the playhead

bool SpeedEditorControl::targetEdit(Id& outgoing, Id& incoming, FrameTime& at) const {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()});
    if (!t) return false;
    const FrameTime here = state_->playhead();
    FrameTime best = -1;
    for (const Clip& c : t->clips)
        for (FrameTime f : {c.start, c.end()})
            if (best < 0 || std::llabs(f - here) < std::llabs(best - here)) best = f;
    if (best < 0) return false;
    outgoing = incoming = 0;
    for (const Clip& c : t->clips) {
        if (c.end() == best) outgoing = c.id;
        if (c.start == best) incoming = c.id;
    }
    at = best;
    return true;
}

std::string SpeedEditorControl::editTransition() const {
    Id outgoing = 0, incoming = 0;
    FrameTime at = 0;
    if (!targetEdit(outgoing, incoming, at)) return "?";
    const Track* t = trackAt(*state_->sequence(), {TrackKind::Video, state_->targetVideoTrack()});
    for (const Transition& tr : t->transitions)
        if (tr.clipA == outgoing && tr.clipB == incoming) return tr.type;
    return "";
}

bool SpeedEditorControl::setEditTransition(const std::string& type) {
    Id outgoing = 0, incoming = 0;
    FrameTime at = 0;
    const Sequence* s = state_->sequence();
    if (!s || !targetEdit(outgoing, incoming, at)) {
        state_->message(tr("No edit on the target video track"), 3000);
        return false;
    }
    const Track* t = trackAt(*s, {TrackKind::Video, state_->targetVideoTrack()});
    const Transition* existing = nullptr;
    for (const Transition& tr : t->transitions)
        if (tr.clipA == outgoing && tr.clipB == incoming) existing = &tr;
    if (type.empty()) {
        if (!existing) return true;  // a cut already
        const Id id = existing->id;
        return state_->apply(tr("Cut"), [id](Project&, Sequence& sq) { return edit::removeTransition(sq, id); });
    }
    if (existing && existing->type == type) return true;
    const double fps = s->fpsValue() > 0 ? s->fpsValue() : 25.0;
    // The length it had, else a second for a dissolve and a sixth of one for a smooth cut (it bridges a jump cut).
    const FrameTime duration = existing ? existing->duration
                               : type == "smooth_cut" ? std::max<FrameTime>(2, FrameTime(std::lround(fps / 6)))
                                                      : FrameTime(std::lround(fps));
    const Id clip = incoming ? incoming : outgoing;
    const edit::Edge edge = incoming ? edit::Edge::In : edit::Edge::Out;
    const bool ok = state_->apply(type == "smooth_cut" ? tr("Smooth Cut") : tr("Dissolve"), [=](Project& p, Sequence& sq) {
        return edit::addTransition(p, sq, clip, edge, type, duration);
    });
    refreshLeds();
    return ok;
}

// ---- Lights

void SpeedEditorControl::refreshLeds() {
    if (!device_) return;
    uint32_t leds = 0;
    for (Key k : speed::allKeys()) {
        const uint32_t bit = speed::ledBit(k);
        if (!bit) continue;
        const QString what = binding(k);
        bool on = false;
        if (what.startsWith(QLatin1String("speed:cam"))) {
            on = hooks_.angle && hooks_.angle() == what.mid(9).toInt() - 1;
        } else if (what == QLatin1String("speed:live_overwrite")) {
            on = liveOverwrite_;
        } else if (what == QLatin1String("speed:cut") || what == QLatin1String("speed:dissolve") || what == QLatin1String("speed:smooth_cut")) {
            const std::string type = editTransition();
            on = what == QLatin1String("speed:cut")        ? type.empty()
                 : what == QLatin1String("speed:dissolve") ? type == "cross_dissolve"
                                                           : type == "smooth_cut";
        } else if (!what.startsWith(QLatin1String("speed:")) && !what.isEmpty()) {
            const QAction* a = keymap::find(window_, what);
            on = a && a->isCheckable() && a->isChecked();
        }
        if (on) leds |= bit;
    }
    const uint8_t jogLeds = speed::jogLedBit(jogMode_);
    if (product_ == speed::kEditorKeyboard) leds = 0;  // (its keys have no lights)
    else if (!ledsKnown_ || leds != ledsSent_) send(speed::ledReport(leds));
    if (!ledsKnown_ || jogLeds != jogLedsSent_) send(speed::jogLedReport(jogLeds));
    ledsSent_ = leds;
    jogLedsSent_ = jogLeds;
    ledsKnown_ = true;
}

}  // namespace montage
