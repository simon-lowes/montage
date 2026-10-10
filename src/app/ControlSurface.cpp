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

std::string centred(const std::string& s) {
    std::string t = s.substr(0, 6);
    const size_t left = (6 - t.size()) / 2;
    t.insert(0, left, ' ');
    t.resize(7, ' ');
    return t;
}

std::string dbShort(double db) {
    if (db <= -59.95) return centred("-inf");
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return centred(std::fabs(db) < 0.05 ? std::string("0.0") : std::string(buf));
}

std::string panShort(double pan) {
    const int p = int(std::lround(pan * 100));
    if (p == 0) return centred("<C>");
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%d", p < 0 ? 'L' : 'R', std::abs(p));
    return centred(buf);
}

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
}

ControlSurface::~ControlSurface() {
    // The connection goes first, so nothing more arrives from its thread.
    connection_.reset();
}

bool ControlSurface::connectPorts(const std::string& input, const std::string& output, std::string* error) {
    disconnectSurface();
    auto c = openMidi(
        input, output,
        [this](const MidiMessage& m) { QMetaObject::invokeMethod(this, [this, m] { handle(m); }, Qt::QueuedConnection); },
        error);
    if (!c) return false;
    setConnection(std::move(c));
    return true;
}

void ControlSurface::setConnection(std::unique_ptr<MidiConnection> connection) {
    if (connection_) disconnectSurface();
    connection_ = std::move(connection);
    if (!connection_) return;
    // Asked to say it is there (surfaces that wait for a host answer it; the rest are taken as there), meters with
    // their signal and peak lights, then everything it shows.
    send(mcu::deviceQuery(device_));
    reset();
    refresh();
    timer_->start();
    emit connectionChanged(true);
}

void ControlSurface::disconnectSurface() {
    if (!connection_) return;
    timer_->stop();
    for (int i = 0; i <= kMaster; ++i)
        if (touched_[size_t(i)] || holds_[size_t(i)]->isActive()) {
            touched_[size_t(i)] = false;
            release(i);
        }
    // Left clean: faders down, every light out, strips and display blank.
    for (int i = 0; i <= kMaster; ++i) send(mcu::fader(i, 0));
    for (int note = 0; note < 0x76; ++note) send(mcu::led(note, 0));
    for (int i = 0; i < mcu::kStrips; ++i) {
        send(mcu::ring(i, 0));
        send(mcu::meter(i, 0));
    }
    send(mcu::lcd(0, std::string(2 * mcu::kLcdWidth, ' '), device_));
    for (const MidiMessage& m : mcu::timecode("")) send(m);
    for (const MidiMessage& m : mcu::assignment("")) send(m);
    connection_.reset();
    emit connectionChanged(false);
}

QString ControlSurface::surfaceName() const {
    if (!connection_) return {};
    const std::string in = connection_->inputName(), out = connection_->outputName();
    return QString::fromStdString(in.empty() ? out : in);
}

void ControlSurface::send(const MidiMessage& m) {
    if (connection_) connection_->send(m);
}

void ControlSurface::reset() {
    shown_ = {};
    leds_.clear();
    digits_.clear();
    assignment_.clear();
    levels_ = {};
    meterAt_ = {};
    for (int i = 0; i < mcu::kStrips; ++i) send(mcu::meterMode(i, 0x03, device_));
}

int ControlSurface::tracks() const {
    const Sequence* s = state_->sequence();
    return s ? int(s->audioTracks.size()) : 0;
}

void ControlSurface::setBank(int first) {
    const int n = tracks();
    first = std::clamp(first, 0, std::max(0, n - mcu::kStrips));
    if (first == bank_) return;
    for (int i = 0; i < mcu::kStrips; ++i)
        if (touched_[size_t(i)] || holds_[size_t(i)]->isActive()) {
            touched_[size_t(i)] = false;
            release(i);
        }
    bank_ = first;
    refresh();
}

// ---- From the surface ------------------------------------------------------------------------------------------------

void ControlSurface::handle(const MidiMessage& m) {
    int device = 0, cmd = 0;
    if (mcu::surfaceSysex(m, device, cmd)) {
        // A surface switched on or answering the query: shown everything again, addressed as it calls itself.
        if (cmd == 0x01 && (device == 0x14 || device == 0x10)) {
            device_ = device;
            reset();
            refresh();
        }
        return;
    }
    const mcu::Event e = mcu::decode(m);
    switch (e.kind) {
        case mcu::Event::Fader: moveFader(e.index, e.value); break;
        case mcu::Event::Touch: touch(e.index, e.down); break;
        case mcu::Event::VPot: turn(e.index, e.value); break;
        case mcu::Event::Jog:
            if (!program_->isPlaying()) program_->step(e.value);
            break;
        case mcu::Event::Button: button(e.index, e.down); break;
        case mcu::Event::None: return;
    }
    refresh();  // what it changed shows at once
}

void ControlSurface::moveFader(int strip, int value) {
    QSlider* slider = strip == kMaster ? mixer_->masterFader() : mixer_->trackFader(bank_ + strip);
    if (!slider) return;
    if (!touched_[size_t(strip)]) hold(strip);  // surfaces without touch sensing
    // Faders hold their place by the host echoing it (else the servo pulls them back to the last position sent).
    send(mcu::fader(strip, value));
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
    if (strip != kMaster)
        if (QDial* pan = mixer_->trackPan(bank_ + strip); pan && pan->isSliderDown()) pan->setSliderDown(false);
    shown_[size_t(strip)].fader = -1;  // sent again where it now is
}

void ControlSurface::turn(int strip, int steps) {
    QDial* pan = mixer_->trackPan(bank_ + strip);
    if (!pan || !pan->isVisibleTo(mixer_) || steps == 0) return;  // (surround tracks pan in two dimensions on screen)
    pan->setSliderDown(true);
    holds_[size_t(strip)]->start();
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
        if (t < n) selected_ = selected_ == t ? -1 : t;
    } else if (const int t = strip(mcu::VPotPush); t >= 0) {
        if (QDial* pan = mixer_->trackPan(t); pan && pan->isVisibleTo(mixer_)) {
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
                if (!program_->isPlaying()) program_->togglePlay();
                break;
            case mcu::Stop:
                if (program_->isPlaying()) program_->shuttle(0);
                break;
            case mcu::Rewind: program_->shuttle(-1); break;
            case mcu::FastForward: program_->shuttle(1); break;
            case mcu::Record: command(QStringLiteral("record")); break;
            case mcu::Left: program_->step(-1); break;
            case mcu::Right: program_->step(1); break;
            case mcu::Up: command(QStringLiteral("previousEdit")); break;
            case mcu::Down: command(QStringLiteral("nextEdit")); break;
            case mcu::Marker: command(QStringLiteral("marker")); break;
            case mcu::Cycle: command(QStringLiteral("loop")); break;
            case mcu::Save: command(QStringLiteral("save")); break;
            case mcu::Undo: state_->undo(); break;
            case mcu::Read:
            case mcu::Write:
            case mcu::Touch:
            case mcu::Latch: {
                // The selected track's fader automation; its own mode again turns it off.
                QComboBox* mode = mixer_->trackAutomationMode(selected_);
                if (!mode || !mode->isEnabled()) break;
                const AutomationMode want = note == mcu::Read ? AutomationMode::Read
                                            : note == mcu::Write ? AutomationMode::Write
                                            : note == mcu::Touch ? AutomationMode::Touch
                                                                 : AutomationMode::Latch;
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
    for (int i = 0; i < mcu::kStrips; ++i) {
        const int t = bank_ + i;
        const float l = 2 * t < trackPeaks.size() ? trackPeaks[2 * t] : 0.f, r = 2 * t + 1 < trackPeaks.size() ? trackPeaks[2 * t + 1] : 0.f;
        const int level = mcu::meterLevel(std::max(l, r));
        // Meters fall by themselves on the surface: a level is sent when it rises, and again every 300 ms it holds.
        if (level > 0 && (level > levels_[size_t(i)] || now - meterAt_[size_t(i)] >= 300)) {
            send(mcu::meter(i, level));
            meterAt_[size_t(i)] = now;
        }
        levels_[size_t(i)] = level;
    }
}

void ControlSurface::refresh() {
    if (!connection_) return;
    const Sequence* seq = state_->sequence();
    const int n = tracks();
    if (bank_ > std::max(0, n - mcu::kStrips)) bank_ = std::max(0, n - mcu::kStrips);
    if (selected_ >= n) selected_ = -1;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
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
            send(mcu::fader(i, fv));
            was.fader = fv;
        }
        const int mute = tr && tr->muted ? 2 : 0, solo = tr && tr->solo ? 2 : 0, select = has && t == selected_ ? 2 : 0;
        if (mute != was.mute) send(mcu::led(mcu::Mute + i, was.mute = mute));
        if (solo != was.solo) send(mcu::led(mcu::Solo + i, was.solo = solo));
        if (select != was.select) send(mcu::led(mcu::Select + i, was.select = select));
        const bool panning = pan && pan->isVisibleTo(mixer_);
        const double panValue = panning ? pan->value() / 100.0 : 0;
        const int ringValue = panning ? mcu::panRing(panValue) : 0;
        if (ringValue != was.ring) send(mcu::ring(i, was.ring = ringValue));
        // Scribble strip: the name above, the level below (the pan for a second after it turns).
        const std::string top = has ? mcu::stripText(tr->name) : std::string(7, ' ');
        const std::string bottom = !has ? std::string(7, ' ') : panning && now - turnedAt_[size_t(i)] < 1000 ? panShort(panValue) : dbShort(db);
        if (top != was.top) send(mcu::lcd(i * 7, was.top = top, device_));
        if (bottom != was.bottom) send(mcu::lcd(mcu::kLcdWidth + i * 7, was.bottom = bottom, device_));
    }
    // The master fader.
    if (QSlider* master = mixer_->masterFader()) {
        const int fv = mcu::faderValue(master->value() / 10.0);
        if (!touched_[size_t(kMaster)] && !holds_[size_t(kMaster)]->isActive() && fv != shown_[size_t(kMaster)].fader) {
            send(mcu::fader(kMaster, fv));
            shown_[size_t(kMaster)].fader = fv;
        }
    }
    // Transport and automation lights.
    const bool playing = program_->isPlaying();
    const QComboBox* mode = mixer_->trackAutomationMode(selected_);
    const int am = mode ? mode->currentIndex() : -1;
    const std::pair<int, bool> lights[] = {{mcu::Play, playing},
                                           {mcu::Stop, !playing},
                                           {mcu::Read, am == int(AutomationMode::Read)},
                                           {mcu::Write, am == int(AutomationMode::Write)},
                                           {mcu::Touch, am == int(AutomationMode::Touch)},
                                           {mcu::Latch, am == int(AutomationMode::Latch)},
                                           {mcu::SmpteLed, true}};
    for (const auto& [note, on] : lights) {
        const int v = on ? 2 : 0;
        const auto it = leds_.find(note);
        if (it == leds_.end() || it->second != v) {
            send(mcu::led(note, v));
            leds_[note] = v;
        }
    }
    // The assignment display: the first track on the strips.
    {
        const std::string a = n > 0 ? std::to_string(std::min(99, bank_ + 1)) : std::string();
        if (a != assignment_) {
            for (const MidiMessage& m : mcu::assignment(a)) send(m);
            assignment_ = a;
        }
    }
    // The timecode display, digit by digit as they change.
    const std::string tc = seq ? formatTimecode(state_->playhead(), seq->fps) : std::string();
    const std::vector<MidiMessage> digits = mcu::timecode(tc);
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
