#include "ControlSurface.h"

#include <QComboBox>
#include <QDateTime>
#include <QDial>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "EditorState.h"
#include "MixerPanel.h"
#include "PlaybackController.h"
#include "control/MackieControl.h"
#include "core/Automation.h"
#include "core/History.h"

namespace montage {

namespace {

constexpr int kMaster = mcu::kMasterChannel;

// Text centred in a display cell of `cell` characters, the last one a space between cells.
std::string centred(const std::string& s, int cell) {
    std::string t = s.substr(0, size_t(cell - 1));
    const size_t left = (size_t(cell - 1) - t.size()) / 2;
    t.insert(0, left, ' ');
    t.resize(size_t(cell), ' ');
    return t;
}

std::string dbShort(double db, int cell) {
    if (db <= -59.95) return centred("-inf", cell);
    char buf[16];
    // (Four characters have no room for a tenth from -10 dB down.)
    std::snprintf(buf, sizeof buf, cell <= 5 && std::fabs(db) >= 9.95 ? "%+.0f" : "%+.1f", db);
    return centred(std::fabs(db) < 0.05 ? std::string("0.0") : std::string(buf), cell);
}

std::string panShort(double pan, int cell) {
    const int p = int(std::lround(pan * 100));
    if (p == 0) return centred("<C>", cell);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%d", p < 0 ? 'L' : 'R', std::abs(p));
    return centred(buf, cell);
}

constexpr int kMackieCell = 7, kHuiCell = 5;  // a strip's share of a display row

}  // namespace

ControlSurface::ControlSurface(EditorState* state, MixerPanel* mixer, PlaybackController* program, QObject* parent)
    : QObject(parent), state_(state), mixer_(mixer), program_(program) {
    timer_ = new QTimer(this);
    timer_->setInterval(33);
    connect(timer_, &QTimer::timeout, this, &ControlSurface::refresh);
    for (int i = 0; i <= kMaster; ++i) {
        holds_[size_t(i)] = new QTimer(this);
        holds_[size_t(i)]->setSingleShot(true);
        holds_[size_t(i)]->setInterval(1000);  // (a fader without touch sensing counts as held until it rests a second)
        connect(holds_[size_t(i)], &QTimer::timeout, this, [this, i] { release(i); });
    }
    for (int i = 0; i < mcu::kStrips; ++i) {
        potHolds_[size_t(i)] = new QTimer(this);
        potHolds_[size_t(i)]->setSingleShot(true);
        potHolds_[size_t(i)]->setInterval(1000);
        connect(potHolds_[size_t(i)], &QTimer::timeout, this, [this, i] { releasePot(i); });
    }
}

ControlSurface::~ControlSurface() {
    // The connection goes first, so nothing more arrives from its thread.
    connection_.reset();
}

bool ControlSurface::connectPorts(const std::string& input, const std::string& output, std::string* error) {
    disconnectSurface();
    auto c = openMidi(
        input, output,
        [this](const MidiMessage& m) { receive(m); }, error, [this] { connectionLost(); });
    if (!c) return false;
    setConnection(std::move(c));
    return true;
}

void ControlSurface::setConnection(std::unique_ptr<MidiConnection> connection) {
    if (connection_) disconnectSurface();
    connection_ = std::move(connection);
    ++generation_;
    if (!connection_) return;
    // Asked to say it is there (Mackie Control surfaces that wait for a host answer it, the rest are taken as there;
    // a HUI is pinged), meters with their signal and peak lights, then everything it shows.
    decoder_.reset();
    pingReplyAt_ = QDateTime::currentMSecsSinceEpoch();
    hello();
    reset();
    refresh();
    timer_->start();
    emit connectionChanged(true);
}

void ControlSurface::disconnectSurface() {
    if (!connection_) return;
    timer_->stop();
    releaseAll();
    // Left clean: faders down, every light out, strips and display blank.
    if (isHui()) {
        // (Its timecode display goes back to OFF-LINE by itself once the pings stop.)
        for (int i = 0; i < hui::kStrips; ++i) {
            send(hui::fader(i, 0));
            send(hui::ring(i, 0));
            send(hui::meter(i, 0, 0));
            send(hui::meter(i, 1, 0));
        }
        for (int zone = 0; zone < 0x1D; ++zone) send(hui::zoneOff(zone));  // (0x1D: relays and the beeper, left alone)
        for (int i = 0; i <= hui::kStrips; ++i) send(hui::stripText(i, ""));
        for (int zone = 0; zone < 8; ++zone) send(hui::display(zone, ""));
    } else {
        for (int i = 0; i <= kMaster; ++i) send(mcu::fader(i, 0));
        for (int note = 0; note < 0x76; ++note) send(mcu::led(note, 0));
        for (int i = 0; i < mcu::kStrips; ++i) {
            send(mcu::ring(i, 0));
            send(mcu::meter(i, 0));
        }
        send(mcu::lcd(0, std::string(2 * mcu::kLcdWidth, ' '), device_));
        send(mcu::timecode(""));
        send(mcu::assignment(""));
    }
    connection_.reset();
    emit connectionChanged(false);
}

void ControlSurface::setProtocol(SurfaceProtocol protocol) {
    if (protocol == protocol_) return;
    if (!connection_) {
        protocol_ = protocol;
        return;
    }
    // The surface was switched to its other mode: the same ports, taken over in the new protocol.
    timer_->stop();
    releaseAll();
    std::unique_ptr<MidiConnection> c = std::move(connection_);
    protocol_ = protocol;
    setConnection(std::move(c));
}

void ControlSurface::releaseAll() {
    for (int i = 0; i <= kMaster; ++i)
        if (touched_[size_t(i)] || holds_[size_t(i)]->isActive()) {
            touched_[size_t(i)] = false;
            release(i);
        }
    for (int i = 0; i < mcu::kStrips; ++i) releasePot(i);
}

void ControlSurface::receive(const MidiMessage& m) {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(m);
    if (!drainPending_) {
        drainPending_ = true;
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }
}

void ControlSurface::connectionLost() {
    const quint64 generation = generation_.load();
    QMetaObject::invokeMethod(this, [this, generation] { if (generation == generation_.load()) lost(); }, Qt::QueuedConnection);
}

void ControlSurface::lost() {
    if (!connection_) return;
    timer_->stop();
    releaseAll();
    connection_.reset();
    emit connectionChanged(false);
}

void ControlSurface::drain() {
    std::vector<MidiMessage> batch;
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        batch.swap(inbox_);
        drainPending_ = false;
    }
    if (!connection_) return;
    // Decoded in order (a HUI's switches and faders take two messages each), then a fader's moves that queued up
    // behind each other: only the last needs applying (each is an edit), as long as no touch of that fader comes
    // between them.
    std::vector<mcu::Event> events;
    for (const MidiMessage& m : batch) {
        const mcu::Event e = decode(m);
        if (!connection_) return;
        if (e.kind != mcu::Event::None) events.push_back(e);
    }
    for (size_t i = 0; i < events.size() && connection_; ++i) {
        const mcu::Event& e = events[i];
        if (e.kind == mcu::Event::Fader) {
            bool later = false;
            for (size_t j = i + 1; j < events.size() && !later; ++j) {
                const mcu::Event& n = events[j];
                if (n.kind == mcu::Event::Fader && n.index == e.index) later = true;
                else if (n.kind == mcu::Event::Touch && n.index == e.index) break;
            }
            if (later) continue;
        }
        apply(e);
    }
}

int ControlSurface::selectedTrack() const {
    const Sequence* s = state_->sequence();
    if (!s || !selected_ || state_->projectGeneration() != project_) return -1;
    for (size_t i = 0; i < s->audioTracks.size(); ++i)
        if (s->audioTracks[i].id == selected_) return int(i);
    return -1;
}

QString ControlSurface::surfaceName() const {
    if (!connection_) return {};
    const std::string in = connection_->inputName(), out = connection_->outputName();
    return QString::fromStdString(in.empty() ? out : in);
}

void ControlSurface::send(const MidiMessage& m) {
    if (connection_) connection_->send(m);
}

void ControlSurface::send(const std::vector<MidiMessage>& ms) {
    for (const MidiMessage& m : ms) send(m);
}

void ControlSurface::hello() {
    if (isHui()) {
        send(hui::ping());
        pingAt_ = QDateTime::currentMSecsSinceEpoch();
    } else {
        send(mcu::deviceQuery(device_));
    }
}

void ControlSurface::reset() {
    shown_ = {};
    leds_.clear();
    digits_.clear();
    assignment_.clear();
    bottomAt_ = {};
    levels_ = {};
    meterAt_ = {};
    zones_ = {};
    zoneAt_ = {};
    if (!isHui())
        for (int i = 0; i < mcu::kStrips; ++i) send(mcu::meterMode(i, 0x03, device_));
}

void ControlSurface::sendFader(int strip, int value) {
    if (!isHui()) send(mcu::fader(strip, value));
    else if (strip < hui::kStrips) send(hui::fader(strip, value));  // (a HUI has no master fader)
}

void ControlSurface::sendLed(int note, int state) {
    if (!isHui()) {
        if (note >= 0 && note < 0x80) send(mcu::led(note, state));
    } else if (const hui::Port p = hui::portOf(note); p.zone >= 0) {
        send(hui::led(p.zone, p.port, state != 0));
    }
}

void ControlSurface::sendRing(int strip, int value) { send(isHui() ? hui::ring(strip, value) : mcu::ring(strip, value)); }

void ControlSurface::sendMeter(int strip, int level) {
    if (!isHui()) {
        send(mcu::meter(strip, level));
        return;
    }
    // Both sides the same (sources disagree on which bit is the left).
    send(hui::meter(strip, 0, std::min(level, 12)));
    send(hui::meter(strip, 1, std::min(level, 12)));
}

int ControlSurface::tracks() const {
    const Sequence* s = state_->sequence();
    return s ? int(s->audioTracks.size()) : 0;
}

void ControlSurface::setBank(int first) {
    const int n = tracks();
    first = std::clamp(first, 0, std::max(0, n - mcu::kStrips));
    if (first == bank_) return;
    // The old tracks let go; a fader still under a hand holds its new track instead (its motor stays still).
    for (int i = 0; i < mcu::kStrips; ++i) {
        const bool held = touched_[size_t(i)];
        touched_[size_t(i)] = false;
        release(i);
        releasePot(i);
        touched_[size_t(i)] = held;
    }
    bank_ = first;
    for (int i = 0; i < mcu::kStrips; ++i)
        if (touched_[size_t(i)])
            if (QSlider* f = mixer_->trackFader(bank_ + i)) f->setSliderDown(true);
    refresh();
}

// ---- From the surface ------------------------------------------------------------------------------------------------

void ControlSurface::handle(const MidiMessage& m) { apply(decode(m)); }

mcu::Event ControlSurface::decode(const MidiMessage& m) {
    if (isHui()) {
        if (hui::isPingReply(m) || hui::isSystemReset(m)) {
            // A surface switched on, or answering again after a silence (it was off line): shown everything again.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool back = hui::isSystemReset(m) || now - pingReplyAt_ > 3000;
            if (hui::isPingReply(m)) pingReplyAt_ = now;
            if (back) {
                decoder_.reset();
                reset();
                refresh();
            }
            return {};
        }
        return decoder_.decode(m);
    }
    int device = 0, cmd = 0;
    if (mcu::surfaceSysex(m, device, cmd)) {
        // A surface switched on or answering the query: shown everything again, addressed as it calls itself.
        if (cmd == 0x01 && (device == 0x14 || device == 0x10)) {
            device_ = device;
            reset();
            refresh();
        }
        return {};
    }
    return mcu::decode(m);
}

void ControlSurface::followProject() {
    if (state_->projectGeneration() == project_) return;
    project_ = state_->projectGeneration();
    selected_ = 0;  // (ids start again in another project, so the old one could name another track)
    bank_ = 0;
}

void ControlSurface::apply(const mcu::Event& e) {
    if (e.kind == mcu::Event::None || !connection_) return;
    followProject();
    // Every move from the surface is one undo step together, however many controls move at once.
    mixer_->setEditMergeKey(QStringLiteral("control-surface"));
    switch (e.kind) {
        case mcu::Event::Fader: moveFader(e.index, e.value); break;
        case mcu::Event::Touch: touch(e.index, e.down); break;
        case mcu::Event::VPot: turn(e.index, e.value); break;
        case mcu::Event::Jog:
            if (!program_->isPlaying()) program_->step(e.value);
            break;
        case mcu::Event::Button: button(e.index, e.down); break;
        case mcu::Event::None: break;
    }
    mixer_->setEditMergeKey(QString());
    refresh();  // what it changed shows at once
}

void ControlSurface::moveFader(int strip, int value) {
    QSlider* slider = strip == kMaster ? mixer_->masterFader() : mixer_->trackFader(bank_ + strip);
    if (!slider) return;
    if (!touched_[size_t(strip)]) hold(strip);  // surfaces without touch sensing
    // Faders hold their place by the host echoing it (else the servo pulls them back to the last position sent).
    sendFader(strip, value);
    shown_[size_t(strip)].fader = value;
    slider->setValue(int(std::lround(std::clamp(mcu::faderDb(value), -60.0, 12.0) * 10)));
}

void ControlSurface::touch(int strip, bool down) {
    QSlider* slider = strip == kMaster ? mixer_->masterFader() : mixer_->trackFader(bank_ + strip);
    touched_[size_t(strip)] = down;
    if (!slider) return;
    if (down) {
        holds_[size_t(strip)]->stop();
        slider->setSliderDown(true);  // held, as a mouse holds the mixer's fader (Touch and Latch write while held)
    } else {
        release(strip);
    }
}

void ControlSurface::hold(int strip) {
    if (strip != kMaster) {
        if (QSlider* f = mixer_->trackFader(bank_ + strip)) f->setSliderDown(true);
    } else if (QSlider* f = mixer_->masterFader()) {
        f->setSliderDown(true);
    }
    holds_[size_t(strip)]->start();
}

void ControlSurface::release(int strip) {
    holds_[size_t(strip)]->stop();
    if (touched_[size_t(strip)]) return;
    QSlider* slider = strip == kMaster ? mixer_->masterFader() : mixer_->trackFader(bank_ + strip);
    if (slider && slider->isSliderDown()) slider->setSliderDown(false);
    shown_[size_t(strip)].fader = -1;  // sent again where it now is
}

void ControlSurface::releasePot(int strip) {
    potHolds_[size_t(strip)]->stop();
    if (QDial* pan = mixer_->trackPan(bank_ + strip); pan && pan->isSliderDown()) pan->setSliderDown(false);
}

void ControlSurface::turn(int strip, int steps) {
    QDial* pan = mixer_->trackPan(bank_ + strip);
    if (!pan || !pan->isVisibleTo(mixer_) || steps == 0) return;  // (surround tracks pan in two dimensions on screen)
    pan->setSliderDown(true);  // held while it turns, so a Touch pass records it
    potHolds_[size_t(strip)]->start();
    pan->setValue(std::clamp(pan->value() + 2 * steps, -100, 100));
    turnedAt_[size_t(strip)] = QDateTime::currentMSecsSinceEpoch();
}

void ControlSurface::command(const QString& name) {
    const auto it = commands_.find(name);
    if (it != commands_.end() && it->second) it->second();
}

void ControlSurface::button(int note, bool down) {
    if (!down) return;
    const int n = tracks();
    auto strip = [&](int base) { return note >= base && note < base + mcu::kStrips ? bank_ + note - base : -1; };
    if (const int t = strip(mcu::Mute); t >= 0) {
        if (QToolButton* b = mixer_->trackMute(t)) b->click();
    } else if (const int t = strip(mcu::Solo); t >= 0) {
        if (QToolButton* b = mixer_->trackSolo(t)) b->click();
    } else if (const int t = strip(mcu::Select); t >= 0) {
        if (const Sequence* seq = state_->sequence(); seq && t < n) {
            const Id id = seq->audioTracks[size_t(t)].id;
            selected_ = selected_ == id ? 0 : id;
        }
    } else if (const int t = strip(mcu::VPotPush); t >= 0) {
        if (QDial* pan = mixer_->trackPan(t); pan && pan->isVisibleTo(mixer_)) {
            pan->setSliderDown(true);
            potHolds_[size_t(t - bank_)]->start();
            pan->setValue(0);
            turnedAt_[size_t(t - bank_)] = QDateTime::currentMSecsSinceEpoch();
        }
    } else {
        switch (note) {
            case mcu::BankLeft: setBank(bank_ - mcu::kStrips); break;
            case mcu::BankRight: setBank(bank_ + mcu::kStrips); break;
            case mcu::ChannelLeft: setBank(bank_ - 1); break;
            case mcu::ChannelRight: setBank(bank_ + 1); break;
            case mcu::Play:
                // Plays at normal speed, from shuttling too.
                if (program_->isPlaying() && program_->speed() != 1.0) program_->shuttle(0);
                if (!program_->isPlaying()) program_->togglePlay();
                break;
            case mcu::Stop:
                if (program_->isPlaying()) program_->shuttle(0);
                break;
            case mcu::Rewind: program_->shuttle(-1); break;
            case mcu::FastForward: program_->shuttle(1); break;
            case mcu::Record: command(QStringLiteral("record")); break;
            case mcu::Cycle: command(QStringLiteral("loop")); break;
            case hui::ReturnToZero: program_->seek(0); break;
            case hui::GoToEnd:
                if (const Sequence* s = state_->sequence()) program_->seek(s->duration());
                break;
            case mcu::Left: program_->step(-1); break;
            case mcu::Right: program_->step(1); break;
            case mcu::Up: command(QStringLiteral("previousEdit")); break;
            case mcu::Down: command(QStringLiteral("nextEdit")); break;
            case mcu::Marker: command(QStringLiteral("marker")); break;
            case mcu::Save: command(QStringLiteral("save")); break;
            case mcu::Undo: state_->undo(); break;
            case mcu::Read:
            case mcu::Write:
            case mcu::Touch:
            case mcu::Latch:
            case hui::AutomationOff: {
                // The selected track's fader automation; its own mode again turns it off.
                QComboBox* mode = mixer_->trackAutomationMode(selectedTrack());
                if (!mode || !mode->isEnabled()) break;
                const AutomationMode want = note == mcu::Read    ? AutomationMode::Read
                                            : note == mcu::Write ? AutomationMode::Write
                                            : note == mcu::Touch ? AutomationMode::Touch
                                            : note == mcu::Latch ? AutomationMode::Latch
                                                                 : AutomationMode::Off;
                mode->setCurrentIndex(mode->currentIndex() == int(want) ? int(AutomationMode::Off) : int(want));
                break;
            }
            default: break;
        }
    }
}

// ---- To the surface --------------------------------------------------------------------------------------------------

void ControlSurface::setLevels(float, float, const QVector<float>& trackPeaks) {
    if (!connection_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    levelsAt_ = now;
    for (int i = 0; i < mcu::kStrips; ++i) {
        const int t = bank_ + i;
        const float l = 2 * t < trackPeaks.size() ? trackPeaks[2 * t] : 0.f, r = 2 * t + 1 < trackPeaks.size() ? trackPeaks[2 * t + 1] : 0.f;
        const int level = mcu::meterLevel(std::max(l, r));
        int& was = levels_[size_t(i)];
        qint64& at = meterAt_[size_t(i)];
        if (!isHui()) {
            // Meters fall by themselves on a Mackie Control surface: a level is sent when it rises, and again every
            // 300 ms it holds.
            if (level > 0 && (level > was || now - at >= 300)) {
                sendMeter(i, level);
                at = now;
            }
        } else if (level > was || (level < was && now - at >= 100) || (level > 0 && now - at >= 300)) {
            // A HUI's are sent falling too (at most ten times a second, for a 5-pin MIDI cable).
            sendMeter(i, level);
            at = now;
        } else {
            continue;  // (what it shows stays what was sent)
        }
        was = level;
    }
}

void ControlSurface::refresh() {
    if (!connection_) return;
    followProject();
    const Sequence* seq = state_->sequence();
    const int n = tracks();
    if (bank_ > std::max(0, n - mcu::kStrips)) bank_ = std::max(0, n - mcu::kStrips);
    const int selected = selectedTrack();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (isHui()) {
        // Pinged once a second (without, its faders stop), and its meters let fall once playback levels stop coming.
        if (now - pingAt_ >= 1000) hello();
        if (now - levelsAt_ >= 300)
            for (int i = 0; i < hui::kStrips; ++i)
                if (levels_[size_t(i)] > 0) {
                    sendMeter(i, 0);
                    levels_[size_t(i)] = 0;
                }
    }
    const int cell = isHui() ? kHuiCell : kMackieCell;
    std::string names, values;  // HUI: the main display's two rows, a cell per strip
    for (int i = 0; i < mcu::kStrips; ++i) {
        Shown& was = shown_[size_t(i)];
        const int t = bank_ + i;
        const bool has = seq && t < n;
        const Track* tr = has ? &seq->audioTracks[size_t(t)] : nullptr;
        QSlider* fader = has ? mixer_->trackFader(t) : nullptr;
        QDial* pan = has ? mixer_->trackPan(t) : nullptr;
        // A fader under a hand is left alone; otherwise it follows the mixer (automation included).
        const double db = fader ? fader->value() / 10.0 : -60;
        const int fv = has ? mcu::faderValue(db) : 0;
        if (!touched_[size_t(i)] && !holds_[size_t(i)]->isActive() && fv != was.fader) {
            sendFader(i, fv);
            was.fader = fv;
        }
        const int mute = tr && tr->muted ? 2 : 0, solo = tr && tr->solo ? 2 : 0, select = has && t == selected ? 2 : 0;
        if (mute != was.mute) sendLed(mcu::Mute + i, was.mute = mute);
        if (solo != was.solo) sendLed(mcu::Solo + i, was.solo = solo);
        if (select != was.select) sendLed(mcu::Select + i, was.select = select);
        const bool panning = pan && pan->isVisibleTo(mixer_);
        const double panValue = panning ? pan->value() / 100.0 : 0;
        const int ringValue = panning ? mcu::panRing(panValue) : 0;  // (single-dot positions, the same in both)
        if (ringValue != was.ring) sendRing(i, was.ring = ringValue);
        // The name, and below it the level (the pan for a second after it turns).
        const std::string bottom = !has ? std::string(size_t(cell), ' ')
                                   : panning && now - turnedAt_[size_t(i)] < 1000 ? panShort(panValue, cell)
                                                                                    : dbShort(db, cell);
        if (isHui()) {
            // The strip's own four characters, and both on the main display (five characters a strip).
            const std::string top = hui::fit(has ? tr->name : std::string(), hui::kStripChars);
            if (top != was.top) send(hui::stripText(i, was.top = top));
            names += top + ' ';
            values += bottom;
            continue;
        }
        const std::string top = has ? mcu::stripText(tr->name) : std::string(7, ' ');
        if (top != was.top) send(mcu::lcd(i * 7, was.top = top, device_));
        // The level line changes as automation plays: at most ten times a second, so a 5-pin MIDI cable keeps up.
        if (bottom != was.bottom && now - bottomAt_[size_t(i)] >= 100) {
            send(mcu::lcd(mcu::kLcdWidth + i * 7, was.bottom = bottom, device_));
            bottomAt_[size_t(i)] = now;
        }
    }
    if (isHui()) {
        // The main display by its ten-character zones, as they change (the levels at most ten times a second).
        for (int zone = 0; zone < 8; ++zone) {
            const std::string& row = zone < 4 ? names : values;
            const std::string text = row.substr(size_t(zone % 4) * hui::kZoneChars, hui::kZoneChars);
            if (text != zones_[size_t(zone)] && (zone < 4 || now - zoneAt_[size_t(zone)] >= 100)) {
                send(hui::display(zone, text));
                zones_[size_t(zone)] = text;
                zoneAt_[size_t(zone)] = now;
            }
        }
    } else if (QSlider* master = mixer_->masterFader()) {
        // The master fader.
        const int fv = mcu::faderValue(master->value() / 10.0);
        if (!touched_[size_t(kMaster)] && !holds_[size_t(kMaster)]->isActive() && fv != shown_[size_t(kMaster)].fader) {
            sendFader(kMaster, fv);
            shown_[size_t(kMaster)].fader = fv;
        }
    }
    // Transport, loop and automation lights.
    const bool playing = program_->isPlaying();
    const QComboBox* mode = mixer_->trackAutomationMode(selected);
    const int am = mode ? mode->currentIndex() : -1;
    const std::pair<int, bool> lights[] = {{mcu::Play, playing},
                                           {mcu::Stop, !playing},
                                           {mcu::Cycle, program_->loop()},
                                           {mcu::Read, am == int(AutomationMode::Read)},
                                           {mcu::Write, am == int(AutomationMode::Write)},
                                           {mcu::Touch, am == int(AutomationMode::Touch)},
                                           {mcu::Latch, am == int(AutomationMode::Latch)},
                                           {hui::AutomationOff, am == int(AutomationMode::Off)},
                                           {mcu::SmpteLed, true}};
    for (const auto& [note, on] : lights) {
        const int v = on ? 2 : 0;
        const auto it = leds_.find(note);
        if (it == leds_.end() || it->second != v) {
            sendLed(note, v);
            leds_[note] = v;
        }
    }
    // The assignment display (a HUI's Select-Assign): the first track on the strips.
    {
        const std::string a = n > 0 ? std::to_string(std::min(isHui() ? 9999 : 99, bank_ + 1)) : std::string();
        if (a != assignment_) {
            if (isHui()) send(hui::stripText(hui::kStrips, std::string(size_t(std::max(0, 4 - int(a.size()))), ' ') + a));
            else send(mcu::assignment(a));
            assignment_ = a;
        }
    }
    // The timecode display, digit by digit as they change (a HUI's all at once).
    const std::string tc = seq ? formatTimecode(state_->playhead(), seq->fps) : std::string();
    const std::vector<MidiMessage> digits = isHui() ? hui::timecode(tc) : mcu::timecode(tc);
    if (digits_.size() != digits.size()) digits_.assign(digits.size(), std::string());
    for (size_t i = 0; i < digits.size(); ++i) {
        const std::string key(digits[i].begin(), digits[i].end());
        if (key != digits_[i]) {
            send(digits[i]);
            digits_[i] = key;
        }
    }
}

}  // namespace montage
