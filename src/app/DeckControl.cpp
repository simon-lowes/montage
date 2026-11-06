#include "DeckControl.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdlib>

#include "EditorState.h"
#include "PlaybackController.h"
#include "media/Capture.h"

namespace montage {

namespace {

QString tc(int64_t frame, Rational rate) {
    const int base = int(std::lround(rate.toDouble()));
    return QString::fromStdString(formatTimecode(frame, base, base == 30 && rate.den == 1001));
}

Rational rateNamed(const QString& name) {
    if (name == QLatin1String("23.976")) return {24000, 1001};
    if (name == QLatin1String("24")) return {24, 1};
    if (name == QLatin1String("29.97")) return {30000, 1001};
    if (name == QLatin1String("30")) return {30, 1};
    return {25, 1};
}

QString rateName(Rational r) {
    if (r == Rational{24000, 1001}) return QStringLiteral("23.976");
    if (r == Rational{30000, 1001}) return QStringLiteral("29.97");
    return QString::number(int(std::lround(r.toDouble())));
}

ninepin::TimeSource timeSourceNamed(const QString& name) {
    if (name == QLatin1String("vitc")) return ninepin::TimeSource::Vitc;
    if (name == QLatin1String("timer")) return ninepin::TimeSource::Timer1;
    if (name == QLatin1String("auto")) return ninepin::TimeSource::Auto;
    return ninepin::TimeSource::Ltc;
}

std::map<std::string, std::string> parseOptions(const QString& text) {
    std::map<std::string, std::string> out;
    for (const QString& kv : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = int(kv.indexOf(QLatin1Char('=')));
        if (eq > 0) out[kv.left(eq).toStdString()] = kv.mid(eq + 1).toStdString();
    }
    return out;
}

}  // namespace

DeckController::DeckController(EditorState* state, PlaybackController* program, QObject* parent)
    : QObject(parent), state_(state), program_(program) {
    if (program_) {
        connect(program_, &PlaybackController::positionChanged, this, [this] { publish(); });
        connect(program_, &PlaybackController::playingChanged, this, [this] { publish(); });
    }
    rollTimer_ = new QTimer(this);
    rollTimer_->setSingleShot(true);
    rollTimer_->setTimerType(Qt::PreciseTimer);
    connect(rollTimer_, &QTimer::timeout, this, [this] {
        if (program_) program_->play();
    });
    chaseTimer_ = new QTimer(this);
    chaseTimer_->setInterval(10);
    connect(chaseTimer_, &QTimer::timeout, this, &DeckController::chaseTick);
}

DeckController::~DeckController() {
    cancelWork();
    if (worker_.joinable()) worker_.join();
    stopRemote();
    stopChase();
}

int DeckController::base() const {
    const Sequence* s = state_->sequence();
    return s ? int(std::lround(s->fps.toDouble())) : 25;
}

bool DeckController::dropFrame() const {
    const Sequence* s = state_->sequence();
    return s && s->fps.den == 1001 && base() == 30;
}

// ---- A deck under control

bool DeckController::connectDeck(const QString& port, Rational rate, QString* error) {
    if (working()) {
        if (error) *error = tr("The deck is busy: %1").arg(lastWork_);
        return false;
    }
    deckRate_ = rate.valid() ? rate : Rational{25, 1};
    const int b = int(std::lround(deckRate_.toDouble()));
    deck_.setBase(b, b == 30 && deckRate_.den == 1001);
    std::string err;
    if (!deck_.open(port.toStdString(), &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    if (!deck_.deviceType()) {
        const QString why = QString::fromStdString(deck_.lastError());
        deck_.close();
        if (error) *error = tr("No deck answered on %1 (%2)").arg(port, why);
        return false;
    }
    deck_.timecode();  // (how it counts its timecode, drop frame or not, from what it sends)
    return true;
}

void DeckController::disconnectDeck() {
    cancelWork();
    if (worker_.joinable()) worker_.join();
    deck_.close();
}

bool DeckController::deckConnected() const { return deck_.isOpen(); }

QString DeckController::deckStatus() {
    if (!deck_.isOpen()) return tr("No deck connected");
    if (working()) return lastWork_;
    // A deck that has stopped answering is asked again only every two seconds (each try waits out its timeout).
    const auto now = std::chrono::steady_clock::now();
    if (!statusError_.isEmpty() && now - statusFailed_ < std::chrono::seconds(2)) return statusError_;
    const auto st = deck_.status();
    if (!st) {
        statusError_ = QString::fromStdString(deck_.lastError());
        statusFailed_ = now;
        return statusError_;
    }
    statusError_.clear();
    const int64_t f = deck_.timecode(ninepin::TimeSource::Ltc);
    QString what = st->tapeOut      ? tr("No tape")
                   : st->record     ? tr("Record")
                   : st->play       ? tr("Play")
                   : st->fastForward ? tr("Fast forward")
                   : st->rewind     ? tr("Rewind")
                   : st->shuttle    ? tr("Shuttle")
                   : st->jog        ? tr("Jog")
                   : st->var        ? tr("Variable")
                   : st->standby    ? tr("Standby")
                                    : tr("Stop");
    if (st->local) what += tr(" (local)");
    return f >= 0 ? tr("%1  %2").arg(what, QString::fromStdString(formatTimecode(f, deck_.base(), deck_.deckDropFrame()))) : what;
}

// ---- Work on the deck

void DeckController::cancelWork() { cancel_ = true; }

void DeckController::finishWork() {
    if (worker_.joinable()) worker_.join();
}

bool DeckController::startBatchCapture(const std::vector<LogEntry>& log, const BatchCaptureSettings& s, QString* error) {
    if (!deck_.isOpen()) {
        if (error) *error = tr("No deck connected");
        return false;
    }
    if (working()) {
        if (error) *error = tr("The deck is busy");
        return false;
    }
    if (log.empty()) {
        if (error) *error = tr("Nothing to capture");
        return false;
    }
    QDir().mkpath(QString::fromStdString(s.folder));
    finishWork();
    cancel_ = false;
    workDone_ = false;
    lastWork_ = tr("Batch capture: starting");
    BatchCaptureSettings settings = s;
    settings.rate = deckRate_;
    QPointer<DeckController> self(this);
    worker_ = std::thread([this, self, log, settings] {
        const std::vector<BatchClip> clips = batchCapture(
            deck_, log, settings,
            [self, n = log.size()](int i, const std::string& what) {
                const QString text = tr("Clip %1 of %2: %3").arg(i + 1).arg(n).arg(QString::fromStdString(what));
                QMetaObject::invokeMethod(self, [self, text] {
                    if (!self) return;
                    self->lastWork_ = text;
                    emit self->workProgress(text);
                }, Qt::QueuedConnection);
            },
            &cancel_);
        QMetaObject::invokeMethod(self, [self, clips] {
            if (!self) return;
            self->captured_ = clips;
            // Each clip into a bin named for its tape, the tape in its log.
            int ok = 0;
            QStringList failed;
            for (const BatchClip& c : clips) {
                if (!c.ok) {
                    failed << QStringLiteral("%1: %2").arg(QString::fromStdString(c.entry.name), QString::fromStdString(c.error));
                    continue;
                }
                const QString bin = c.entry.reel.empty() ? tr("Captured") : QString::fromStdString(c.entry.reel);
                const std::vector<Id> ids = self->state_->importFiles({QString::fromStdString(c.path)}, nullptr, bin);
                if (ids.empty()) continue;
                ++ok;
                self->state_->edit(tr("Batch Capture"), [&](Project& p, Sequence&) {
                    for (MediaItem& m : p.media)
                        if (m.id == ids.front()) {
                            m.name = c.entry.name;
                            if (!c.entry.reel.empty()) m.metadata["tape"] = c.entry.reel;
                            if (!c.entry.comment.empty()) m.metadata["comment"] = c.entry.comment;
                        }
                    return true;
                });
            }
            self->workDone_ = true;
            self->lastWork_ = tr("Batch capture: %1 of %2 clips captured").arg(ok).arg(clips.size()) +
                              (failed.isEmpty() ? QString() : QStringLiteral("; ") + failed.join(QStringLiteral("; ")));
            emit self->workFinished(failed.isEmpty(), self->lastWork_);
        }, Qt::QueuedConnection);
    });
    return true;
}

bool DeckController::startEditToTape(const TapeEdit& e, FrameTime programIn, QString* error) {
    if (!deck_.isOpen()) {
        if (error) *error = tr("No deck connected");
        return false;
    }
    if (working()) {
        if (error) *error = tr("The deck is busy");
        return false;
    }
    if (const Sequence* s = state_->sequence(); s && s->fps != deckRate_) {
        if (error) *error = tr("The Program runs at %1 fps and the deck at %2: set the deck's rate to the Program's").arg(rateName(s->fps), rateName(deckRate_));
        return false;
    }
    finishWork();
    cancel_ = false;
    workDone_ = false;
    lastWork_ = tr("Edit to tape: setting up");
    QPointer<DeckController> self(this);
    worker_ = std::thread([this, self, e, programIn] {
        std::string err;
        const bool ok = editToTape(
            deck_, e,
            [self, programIn](int64_t framesToIn, double clock) {
                QMetaObject::invokeMethod(self, [self, programIn, framesToIn, clock] {
                    if (!self) return;
                    self->rollProgram(programIn, framesToIn, clock);
                    self->lastWork_ = tr("Edit to tape: rolling");
                    emit self->workProgress(self->lastWork_);
                }, Qt::QueuedConnection);
            },
            &cancel_, &err);
        QMetaObject::invokeMethod(self, [self, ok, err] {
            if (!self) return;
            self->rollTimer_->stop();  // (a start still to come, for a roll before the deck stopped: not made)
            if (self->program_) self->program_->pause();
            self->workDone_ = true;
            self->lastWork_ = ok ? tr("Edit to tape: done") : tr("Edit to tape: %1").arg(QString::fromStdString(err));
            emit self->workFinished(ok, self->lastWork_);
        }, Qt::QueuedConnection);
    });
    return true;
}

void DeckController::rollProgram(FrameTime programIn, int64_t framesToIn, double clock) {
    // The Program started so its first frame of the edit goes out as the deck reaches In. Each roll replaces the one
    // before: a start still waiting from an earlier one (a deck that rolled, then searched back and rolled again) is
    // not made, so the Program follows the deck's latest run up alone.
    rollTimer_->stop();
    if (!program_) return;
    const double fps = deckRate_.toDouble();
    const double late = (captureClock() - clock) * fps;
    const FrameTime from = programIn - FrameTime(framesToIn) + FrameTime(std::lround(late));
    if (from >= 0) {
        state_->setPlayhead(from);
        program_->play();
    } else {
        // (the Program's start nearer than the pre-roll: it waits on its first frame until its time)
        program_->pause();
        state_->setPlayhead(0);
        rollTimer_->start(int(std::lround(double(-from) * 1000 / fps)));
    }
}

bool DeckController::rollPending() const { return rollTimer_->isActive(); }

// ---- Montage as a deck

bool DeckController::startRemote(const QString& port, QString* error) {
    stopRemote();
    auto r = std::make_unique<ninepin::DeckEmulator>();
    r->setBase(base(), dropFrame());
    QPointer<DeckController> self(this);
    r->onRequest = [self](const ninepin::TransportRequest& q) {
        QMetaObject::invokeMethod(self, [self, q] {
            if (!self || !self->program_) return;
            switch (q.kind) {
            case ninepin::TransportRequest::Locate:
                self->program_->pause();
                self->state_->setPlayhead(std::max<FrameTime>(0, q.frame - self->programStart_));
                break;
            case ninepin::TransportRequest::Speed:
                if (q.speed == 1) self->program_->play();
                else self->program_->setSpeed(q.speed);
                break;
            case ninepin::TransportRequest::Stop: self->program_->pause(); break;
            case ninepin::TransportRequest::Step: self->program_->step(q.speed > 0 ? 1 : -1); break;
            }
            self->publish();
        }, Qt::QueuedConnection);
    };
    std::string err;
    if (!r->start(port.toStdString(), &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    remote_ = std::move(r);
    publish();
    emit remoteChanged(true);
    return true;
}

void DeckController::stopRemote() {
    if (!remote_) return;
    remote_->stop();
    remote_.reset();
    emit remoteChanged(false);
}

bool DeckController::remoteRunning() const { return remote_ && remote_->running(); }

void DeckController::publish() {
    if (!remote_ || !program_) return;
    const Sequence* s = state_->sequence();
    ninepin::TransportState t;
    t.frame = programStart_ + program_->position();
    t.speed = program_->speed();
    t.at = std::chrono::steady_clock::now();
    t.fps = s ? s->fps.toDouble() : 25;
    remote_->publish(t);
}

// ---- LTC chase

bool DeckController::startChase(const QString& input, int channel, int64_t offsetFrames, double freewheelSeconds, QString* error) {
    stopChase();
    QAudioDevice dev = QMediaDevices::defaultAudioInput();
    for (const QAudioDevice& d : QMediaDevices::audioInputs())
        if (d.description() == input) dev = d;
    if (dev.isNull()) {
        if (error) *error = tr("No sound input to read LTC from");
        return false;
    }
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(std::max(channel + 1, 1));
    fmt.setSampleFormat(QAudioFormat::Float);
    if (!dev.isFormatSupported(fmt)) {
        fmt = dev.preferredFormat();
        fmt.setSampleFormat(QAudioFormat::Float);
        if (!dev.isFormatSupported(fmt) || fmt.channelCount() <= channel) {
            if (error) *error = tr("%1 has no channel %2 to read LTC from").arg(dev.description()).arg(channel + 1);
            return false;
        }
    }
    chase_ = std::make_unique<LtcChase>(fmt.sampleRate(), base(), dropFrame());
    chaseRate_ = fmt.sampleRate();
    chasePending_.clear();
    chase_->setOffset(offsetFrames);
    chase_->setFreewheel(freewheelSeconds);
    chaseChannel_ = channel;
    chaseChannels_ = fmt.channelCount();
    chaseSource_ = new QAudioSource(dev, fmt, this);
    chaseIo_ = chaseSource_->start();
    if (!chaseIo_) {
        stopChase();
        if (error) *error = tr("Cannot open %1").arg(dev.description());
        return false;
    }
    connect(chaseIo_, &QIODevice::readyRead, this, [this] {
        if (!chaseIo_) return;
        // (a read can end inside a sample frame: the rest kept for the next)
        chasePending_ += chaseIo_->readAll();
        const int bytesPerFrame = 4 * chaseChannels_;
        const int frames = int(chasePending_.size() / bytesPerFrame);
        feedChase(reinterpret_cast<const float*>(chasePending_.constData()), frames, chaseChannels_);
        chasePending_.remove(0, qsizetype(frames) * bytesPerFrame);
    });
    chaseTimer_->start();
    emit chaseChanged(true);
    return true;
}

void DeckController::stopChase() {
    chaseTimer_->stop();
    if (chaseSource_) {
        chaseSource_->stop();
        chaseSource_->deleteLater();
    }
    chaseSource_ = nullptr;
    chaseIo_ = nullptr;
    const bool was = chase_ != nullptr;
    chase_.reset();
    if (chaseRolling_ && program_) program_->pause();
    chaseRolling_ = false;
    chasePending_.clear();
    if (was) emit chaseChanged(false);
}

void DeckController::feedChase(const float* samples, int frames, int channels) {
    if (!chase_ || frames <= 0) return;
    std::vector<float> mono(static_cast<size_t>(frames));
    const int ch = std::min(chaseChannel_, channels - 1);
    for (int i = 0; i < frames; ++i) mono[size_t(i)] = samples[size_t(i) * size_t(channels) + size_t(ch)];
    chase_->write(mono.data(), mono.size());
    chaseFed_ = std::chrono::steady_clock::now();
}

LtcChase::Position DeckController::chaseNow() const {
    if (!chase_) return {};
    // Between the input's pieces: carried on by the time since the last (up to a fifth of a second).
    const double since = std::min(0.2, std::chrono::duration<double>(std::chrono::steady_clock::now() - chaseFed_).count());
    return chase_->at(chase_->written() + int64_t(std::max(0.0, since) * chaseRate_));
}

LtcChase::Position DeckController::chasePosition() const { return chaseNow(); }

QString DeckController::chaseStatus() const {
    if (!chase_) return tr("Not chasing");
    const LtcChase::Position p = chaseNow();
    const Sequence* s = state_->sequence();
    const Rational r = s ? s->fps : Rational{25, 1};
    switch (p.state) {
    case LtcChase::State::Locked: return tr("Locked  %1").arg(tc(int64_t(std::floor(p.frame)), r));
    case LtcChase::State::Freewheel: return tr("Freewheeling  %1").arg(tc(int64_t(std::floor(p.frame)), r));
    case LtcChase::State::Locking: return tr("Locking…");
    default: return tr("No LTC");
    }
}

void DeckController::chaseTick() {
    if (!chase_ || !program_) return;
    const LtcChase::Position p = chaseNow();
    if (p.state == LtcChase::State::Locked || p.state == LtcChase::State::Freewheel) {
        const FrameTime target = FrameTime(std::floor(p.frame + 1e-6)) - programStart_;
        if (target < 0) return;
        // (the fastest the Program plays either way, as PlaybackController::setSpeed takes it; faster, it is put where
        // the master is as it goes)
        constexpr double kFastest = 32;
        const bool atSpeed = std::fabs(p.speed - 1) < 0.05, playable = std::fabs(p.speed) <= kFastest;
        if (atSpeed) {
            if (!program_->isPlaying() || program_->speed() != 1) {
                if (!program_->isPlaying()) state_->setPlayhead(target);
                program_->play();
            }
        } else if (playable) {
            if (std::fabs(program_->speed() - p.speed) > 0.01 * std::max(1.0, std::fabs(p.speed))) program_->setSpeed(p.speed);
        } else if (program_->isPlaying()) {
            program_->pause();
        }
        chaseRolling_ = true;
        // (more than a frame out: put right; within it, left to run)
        if (!playable || std::llabs(program_->position() - target) > 1) state_->setPlayhead(target);
    } else if (chaseRolling_) {
        program_->pause();
        chaseRolling_ = false;
    }
}

// ---- The agent link's tool

QByteArray DeckController::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_deck","title":"Deck control (Sony 9-pin RS-422)",
"description":"A tape deck or video server under Sony 9-pin control on a serial port (a USB RS-422 adapter), and Montage answering as one. \"ports\": the serial ports. \"connect\" (port, rate) and \"disconnect\". \"status\": the deck's transport and timecode, the job running and its last result, the remote and the chase. \"transport\": play, stop, fast_forward, rewind, shuttle, jog, variable (speed, times play, negative backwards), step_forward, step_back, eject, preroll. \"cue\": to a timecode. \"capture\": batch capture from a log (an ALE or CSV file, or entries) through a capture feed (as montage_capture's: device and url), each clip from its In less the handles to its Out plus them, starting on the frame by the deck's timecode, kept with its timecode and tape, and imported into a bin named for the tape. \"edit\": edit to tape, the Program from program_in played out through Reference Output while the deck records In to Out (assemble, or insert on the channels chosen) after its pre-roll. \"cancel\": stops the job. \"remote\": Montage as a deck on a port (on false to stop), so an edit controller or DAW machine control locates, plays, shuttles and jogs the Program; program_start is the Program's first frame's timecode. \"chase\": the Program follows LTC on a sound input (on false to stop), with an offset in frames and a freewheel time.",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["ports","connect","disconnect","status","transport","cue","capture","edit","cancel","remote","chase"]},
"port":{"type":"string","description":"A serial port from \"ports\" (/dev/ttyUSB0, COM3...)"},
"rate":{"type":"string","enum":["23.976","24","25","29.97","30"],"description":"The deck's frame rate (29.97 counts in drop frame)"},
"command":{"type":"string","enum":["play","stop","fast_forward","rewind","shuttle","jog","variable","step_forward","step_back","eject","preroll"]},
"speed":{"type":"number"},
"timecode":{"type":"string","description":"\"hh:mm:ss:ff\""},
"log":{"type":"string","description":"An ALE or CSV log file (name, tape, start, end)"},
"entries":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"tape":{"type":"string"},"in":{"type":"string"},"out":{"type":"string","description":"The first frame after the clip"}},"required":["in","out"]}},
"tape":{"type":"string","description":"The tape's name, for entries that name none"},
"device":{"type":"string","description":"The deck's picture: FFmpeg's input device (\"v4l2\", \"avfoundation\", \"dshow\"...), \"omt\", or \"\" for a stream URL"},
"url":{"type":"string","description":"The device or stream"},
"options":{"type":"object","description":"FFmpeg input options"},
"codec":{"type":"string","description":"prores_hq (default), prores, prores_lt, prores_proxy, dnxhr_hq..."},
"folder":{"type":"string"},
"handles":{"type":"number","description":"Seconds either side (2)"},
"preroll":{"type":"number","description":"Seconds (5)"},
"latency_frames":{"type":"number","description":"How late the picture arrives behind the deck's timecode, for a feed without its own"},
"timecode_source":{"type":"string","enum":["ltc","vitc","timer","auto"],"description":"The deck's timecode read as it plays (ltc)"},
"in":{"type":"string","description":"The edit's In on the tape"},
"out":{"type":"string","description":"The edit's Out on the tape (the first frame not recorded)"},
"program_in":{"type":"string","description":"The Program's timecode the edit starts from (default: its In mark)"},
"assemble":{"type":"boolean"},
"video":{"type":"boolean"},
"audio":{"type":"array","items":{"type":"number"},"description":"Channels to insert on (1 to 8)"},
"on":{"type":"boolean"},
"program_start":{"type":"string","description":"The Program's first frame's timecode, as a controller sees it"},
"input":{"type":"string","description":"A sound input's name (\"\": the default)"},
"channel":{"type":"number","description":"Its channel carrying LTC (1 first)"},
"offset":{"type":"number","description":"Frames added to the master's timecode"},
"freewheel":{"type":"number","description":"Seconds a drop-out is ridden through (2)"}},
"required":["action"]},
"annotations":{"readOnlyHint":false,"destructiveHint":false,"openWorldHint":true}})json");
}

QString DeckController::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    isError = false;
    const QString action = args.value("action").toString();
    auto fail = [&](const QString& why) {
        isError = true;
        return why;
    };
    const int deckBase = int(std::lround(deckRate_.toDouble()));
    const bool deckDrop = deckBase == 30 && deckRate_.den == 1001;
    auto tapeFrame = [&](const QString& key, int64_t& out) {
        out = parseTimecode(args.value(key).toString().toStdString(), deckBase, nullptr, deckDrop);
        return out >= 0;
    };
    if (action == QLatin1String("ports")) {
        QJsonArray ports;
        for (const std::string& p : SerialPort::available()) ports.append(QString::fromStdString(p));
        structured["ports"] = ports;
        return ports.isEmpty() ? tr("No serial ports") : tr("%n serial port(s)", nullptr, int(ports.size()));
    }
    if (action == QLatin1String("connect")) {
        QString err;
        if (!connectDeck(args.value("port").toString(), rateNamed(args.value("rate").toString(rateName(deckRate_))), &err)) return fail(err);
        return deckStatus();
    }
    if (action == QLatin1String("disconnect")) {
        disconnectDeck();
        return tr("Disconnected");
    }
    if (action == QLatin1String("status")) {
        structured["deck"] = deckStatus();
        structured["connected"] = deckConnected();
        structured["working"] = working();
        structured["job"] = lastWork_;
        QJsonArray clips;
        for (const BatchClip& c : captured_) {
            QJsonObject o;
            o["name"] = QString::fromStdString(c.entry.name);
            o["tape"] = QString::fromStdString(c.entry.reel);
            o["path"] = QString::fromStdString(c.path);
            o["timecode"] = QString::fromStdString(c.timecode);
            o["frames"] = double(c.frames);
            o["ok"] = c.ok;
            if (!c.ok) o["error"] = QString::fromStdString(c.error);
            clips.append(o);
        }
        structured["captured"] = clips;
        structured["remote"] = remoteRunning();
        structured["chase"] = chaseStatus();
        return deckStatus();
    }
    if (action == QLatin1String("cancel")) {
        cancelWork();
        return working() ? tr("Stopping") : tr("Nothing running");
    }
    if (action == QLatin1String("remote")) {
        if (!args.value("on").toBool(true)) {
            stopRemote();
            return tr("Montage no longer answers as a deck");
        }
        if (args.contains("program_start")) {
            const int64_t f = parseTimecode(args.value("program_start").toString().toStdString(), base(), nullptr, dropFrame());
            if (f < 0) return fail(tr("program_start is not a timecode"));
            programStart_ = f;
        }
        QString err;
        if (!startRemote(args.value("port").toString(), &err)) return fail(err);
        return tr("Montage answers as a deck on %1").arg(args.value("port").toString());
    }
    if (action == QLatin1String("chase")) {
        if (!args.value("on").toBool(true)) {
            stopChase();
            return tr("Not chasing");
        }
        QString err;
        if (!startChase(args.value("input").toString(), std::max(0, args.value("channel").toInt(1) - 1), int64_t(args.value("offset").toDouble(0)),
                        args.value("freewheel").toDouble(2), &err))
            return fail(err);
        return tr("Chasing LTC");
    }
    if (!deck_.isOpen()) return fail(tr("No deck connected: \"connect\" first"));
    if (working() && action != QLatin1String("status")) return fail(tr("The deck is busy: %1").arg(lastWork_));
    if (action == QLatin1String("transport")) {
        const QString c = args.value("command").toString();
        const double speed = args.value("speed").toDouble(1);
        bool ok = false;
        if (c == QLatin1String("play")) ok = deck_.play();
        else if (c == QLatin1String("stop")) ok = deck_.stop();
        else if (c == QLatin1String("fast_forward")) ok = deck_.fastForward();
        else if (c == QLatin1String("rewind")) ok = deck_.rewind();
        else if (c == QLatin1String("shuttle")) ok = deck_.shuttle(speed);
        else if (c == QLatin1String("jog")) ok = deck_.jog(speed);
        else if (c == QLatin1String("variable")) ok = deck_.variable(speed);
        else if (c == QLatin1String("step_forward")) ok = deck_.step(true);
        else if (c == QLatin1String("step_back")) ok = deck_.step(false);
        else if (c == QLatin1String("eject")) ok = deck_.eject();
        else if (c == QLatin1String("preroll")) ok = deck_.preroll();
        else return fail(tr("Unknown command"));
        if (!ok) return fail(QString::fromStdString(deck_.lastError()));
        return deckStatus();
    }
    if (action == QLatin1String("cue")) {
        int64_t f = 0;
        if (!tapeFrame("timecode", f)) return fail(tr("timecode is not a timecode"));
        if (!deck_.cue(deck_.toDeck(f))) return fail(QString::fromStdString(deck_.lastError()));
        return tr("Cueing to %1").arg(tc(f, deckRate_));
    }
    if (action == QLatin1String("capture")) {
        std::vector<LogEntry> log;
        const std::string tape = args.value("tape").toString().toStdString();
        if (args.contains("log")) {
            std::string err;
            log = readCaptureLog(args.value("log").toString().toStdString(), deckBase, deckDrop, &err, tape);
            if (log.empty()) return fail(QString::fromStdString(err));
        }
        for (const QJsonValue& v : args.value("entries").toArray()) {
            const QJsonObject o = v.toObject();
            LogEntry e;
            e.name = o.value("name").toString(tr("Clip %1").arg(log.size() + 1)).toStdString();
            e.reel = o.value("tape").toString(QString::fromStdString(tape)).toStdString();
            e.in = parseTimecode(o.value("in").toString().toStdString(), deckBase, nullptr, deckDrop);
            e.out = parseTimecode(o.value("out").toString().toStdString(), deckBase, nullptr, deckDrop);
            if (e.in < 0 || e.out <= e.in) return fail(tr("An entry's in and out are not timecodes, In before Out"));
            log.push_back(e);
        }
        QSettings settings;
        BatchCaptureSettings s;
        s.capture.video.device = args.value("device").toString(settings.value(QStringLiteral("capture/kind")).toString()).toStdString();
        s.capture.video.url = args.value("url").toString(settings.value(QStringLiteral("capture/url")).toString()).toStdString();
        const QJsonObject options = args.value("options").toObject();
        for (auto it = options.begin(); it != options.end(); ++it) s.capture.video.options[it.key().toStdString()] = it.value().toVariant().toString().toStdString();
        s.capture.codec = args.value("codec").toString(QStringLiteral("prores_hq")).toStdString();
        s.folder = args.value("folder").toString(settings.value(QStringLiteral("capture/folder"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString()).toStdString();
        s.handles = args.value("handles").toDouble(2);
        s.preroll = args.value("preroll").toDouble(5);
        s.latencyFrames = args.value("latency_frames").toInt(0);
        s.source = timeSourceNamed(args.value("timecode_source").toString());
        QString err;
        if (!startBatchCapture(log, s, &err)) return fail(err);
        return tr("Capturing %n clip(s): \"status\" follows it", nullptr, int(log.size()));
    }
    if (action == QLatin1String("edit")) {
        TapeEdit e;
        if (!tapeFrame("in", e.in) || !tapeFrame("out", e.out) || e.out <= e.in) return fail(tr("in and out must be timecodes on the tape, In before Out"));
        e.channels.assemble = args.value("assemble").toBool(false);
        e.channels.video = args.value("video").toBool(true);
        e.channels.timecode = e.channels.assemble;
        if (args.contains("audio")) {
            e.channels.audio.fill(false);
            for (const QJsonValue& v : args.value("audio").toArray())
                if (const int ch = v.toInt(); ch >= 1 && ch <= 8) e.channels.audio[size_t(ch - 1)] = true;
        }
        e.preroll = args.value("preroll").toDouble(5);
        const Sequence* s = state_->sequence();
        if (!s) return fail(tr("No sequence open"));
        FrameTime programIn = s->inPoint >= 0 ? s->inPoint : 0;
        if (args.contains("program_in")) {
            const int64_t f = parseTimecode(args.value("program_in").toString().toStdString(), base(), nullptr, dropFrame());
            if (f < 0) return fail(tr("program_in is not a timecode"));
            programIn = f - programStart_;
        }
        QString err;
        if (!startEditToTape(e, programIn, &err)) return fail(err);
        return tr("Editing to tape from %1 to %2: \"status\" follows it").arg(tc(e.in, deckRate_), tc(e.out, deckRate_));
    }
    return fail(tr("Unknown action"));
}

// ---- The window

DeckDialog::DeckDialog(DeckController* deck, EditorState* state, QWidget* parent) : QDialog(parent), deck_(deck), state_(state) {
    setWindowTitle(tr("Deck Control"));
    setObjectName(QStringLiteral("deckControl"));
    QSettings settings;
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs);

    // The deck: its port, rate, transport and timecode.
    auto* deckPage = new QWidget(this);
    auto* deckForm = new QFormLayout(deckPage);
    auto* portRow = new QHBoxLayout;
    port_ = new QComboBox(this);
    port_->setEditable(true);
    port_->setObjectName(QStringLiteral("deckPort"));
    for (const std::string& p : SerialPort::available()) port_->addItem(QString::fromStdString(p));
    port_->setCurrentText(settings.value(QStringLiteral("deck/port"), port_->currentText()).toString());
    rate_ = new QComboBox(this);
    rate_->addItems({QStringLiteral("23.976"), QStringLiteral("24"), QStringLiteral("25"), QStringLiteral("29.97"), QStringLiteral("30")});
    rate_->setCurrentText(settings.value(QStringLiteral("deck/rate"), QStringLiteral("25")).toString());
    connect_ = new QPushButton(tr("Connect"), this);
    connect_->setObjectName(QStringLiteral("deckConnect"));
    connect(connect_, &QPushButton::clicked, this, &DeckDialog::connectDeck);
    portRow->addWidget(port_, 1);
    portRow->addWidget(rate_);
    portRow->addWidget(connect_);
    deckForm->addRow(tr("Serial port:"), portRow);
    status_ = new QLabel(tr("No deck connected"), this);
    status_->setObjectName(QStringLiteral("deckStatus"));
    QFont mono = status_->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setPointSizeF(mono.pointSizeF() * 1.4);
    status_->setFont(mono);
    deckForm->addRow(status_);
    auto* transport = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& tip, std::function<bool(ninepin::Deck&)> run) {
        auto* b = new QPushButton(text, this);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, run] {
            if (!deck_->deckConnected() || deck_->working()) return;
            if (!run(deck_->deck())) activity_->appendPlainText(QString::fromStdString(deck_->deck().lastError()));
            refresh();
        });
        transport->addWidget(b);
    };
    button(QStringLiteral("◀◀"), tr("Rewind"), [](ninepin::Deck& d) { return d.rewind(); });
    button(QStringLiteral("◀"), tr("Shuttle back at twice play speed"), [](ninepin::Deck& d) { return d.shuttle(-2); });
    button(QStringLiteral("◀|"), tr("Back a frame"), [](ninepin::Deck& d) { return d.step(false); });
    button(QStringLiteral("■"), tr("Stop"), [](ninepin::Deck& d) { return d.stop(); });
    button(QStringLiteral("▶"), tr("Play"), [](ninepin::Deck& d) { return d.play(); });
    button(QStringLiteral("|▶"), tr("On a frame"), [](ninepin::Deck& d) { return d.step(true); });
    button(QStringLiteral("▶▶"), tr("Fast forward"), [](ninepin::Deck& d) { return d.fastForward(); });
    deckForm->addRow(transport);
    auto* cueRow = new QHBoxLayout;
    cue_ = new QLineEdit(this);
    cue_->setPlaceholderText(QStringLiteral("10:00:00:00"));
    auto* cueGo = new QPushButton(tr("Cue"), this);
    connect(cueGo, &QPushButton::clicked, this, [this] {
        const int b = int(std::lround(deck_->deckRate().toDouble()));
        const int64_t f = parseTimecode(cue_->text().toStdString(), b, nullptr, b == 30 && deck_->deckRate().den == 1001);
        if (f < 0 || !deck_->deckConnected()) return;
        if (deck_->working()) {
            activity_->appendPlainText(tr("The deck is busy: %1").arg(deck_->lastWork()));
            return;
        }
        if (!deck_->deck().cue(deck_->deck().toDeck(f))) activity_->appendPlainText(QString::fromStdString(deck_->deck().lastError()));
    });
    cueRow->addWidget(cue_, 1);
    cueRow->addWidget(cueGo);
    deckForm->addRow(tr("Cue to:"), cueRow);
    tabs->addTab(deckPage, tr("Deck"));

    // Batch capture.
    auto* batchPage = new QWidget(this);
    auto* batchForm = new QFormLayout(batchPage);
    auto* logRow = new QHBoxLayout;
    log_ = new QLineEdit(this);
    log_->setObjectName(QStringLiteral("deckLog"));
    log_->setPlaceholderText(tr("An ALE or CSV log (empty: the Program's In to Out, as one clip)"));
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Capture Log"), QString(), tr("Logs (*.ale *.csv *.txt)"));
        if (!f.isEmpty()) log_->setText(f);
    });
    logRow->addWidget(log_, 1);
    logRow->addWidget(browse);
    batchForm->addRow(tr("Log:"), logRow);
    sourceKind_ = new QComboBox(this);
    sourceKind_->addItem(tr("Network stream"), QString());
    for (const std::string& k : captureDeviceKinds()) sourceKind_->addItem(QString::fromStdString(k), QString::fromStdString(k));
    if (const int i = sourceKind_->findData(settings.value(QStringLiteral("capture/kind")).toString()); i >= 0) sourceKind_->setCurrentIndex(i);
    batchForm->addRow(tr("The deck's picture:"), sourceKind_);
    source_ = new QLineEdit(settings.value(QStringLiteral("capture/url")).toString(), this);
    batchForm->addRow(tr("Address or device:"), source_);
    folder_ = new QLineEdit(settings.value(QStringLiteral("capture/folder"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString(), this);
    batchForm->addRow(tr("Into:"), folder_);
    handles_ = new QDoubleSpinBox(this);
    handles_->setRange(0, 30);
    handles_->setValue(settings.value(QStringLiteral("deck/handles"), 2.0).toDouble());
    handles_->setSuffix(tr(" s"));
    batchForm->addRow(tr("Handles:"), handles_);
    preroll_ = new QDoubleSpinBox(this);
    preroll_->setRange(1, 30);
    preroll_->setValue(settings.value(QStringLiteral("deck/preroll"), 5.0).toDouble());
    preroll_->setSuffix(tr(" s"));
    batchForm->addRow(tr("Pre-roll:"), preroll_);
    timeSource_ = new QComboBox(this);
    timeSource_->addItem(tr("LTC"), QStringLiteral("ltc"));
    timeSource_->addItem(tr("VITC"), QStringLiteral("vitc"));
    timeSource_->addItem(tr("Timer"), QStringLiteral("timer"));
    timeSource_->addItem(tr("LTC or VITC"), QStringLiteral("auto"));
    if (const int i = timeSource_->findData(settings.value(QStringLiteral("deck/timeSource"))); i >= 0) timeSource_->setCurrentIndex(i);
    timeSource_->setToolTip(tr("The deck's timecode, read as it plays to place pictures that carry none of their own"));
    batchForm->addRow(tr("Deck timecode:"), timeSource_);
    auto* capture = new QPushButton(tr("Capture"), this);
    capture->setObjectName(QStringLiteral("deckCapture"));
    connect(capture, &QPushButton::clicked, this, &DeckDialog::runBatch);
    batchForm->addRow(capture);
    tabs->addTab(batchPage, tr("Batch Capture"));

    // Edit to tape.
    auto* editPage = new QWidget(this);
    auto* editForm = new QFormLayout(editPage);
    editIn_ = new QLineEdit(this);
    editIn_->setPlaceholderText(QStringLiteral("10:00:00:00"));
    editForm->addRow(tr("Record In on the tape:"), editIn_);
    auto* chans = new QHBoxLayout;
    assemble_ = new QCheckBox(tr("Assemble"), this);
    video_ = new QCheckBox(tr("V"), this);
    video_->setChecked(true);
    chans->addWidget(assemble_);
    chans->addWidget(video_);
    for (int i = 0; i < 4; ++i) {
        auto* a = new QCheckBox(tr("A%1").arg(i + 1), this);
        a->setChecked(i < 2);
        audio_.push_back(a);
        chans->addWidget(a);
    }
    editForm->addRow(tr("Channels:"), chans);
    auto* edit = new QPushButton(tr("Edit the Program's In to Out onto Tape"), this);
    edit->setToolTip(tr("The Program plays out through Reference Output while the deck records it"));
    connect(edit, &QPushButton::clicked, this, &DeckDialog::runEdit);
    editForm->addRow(edit);
    tabs->addTab(editPage, tr("Edit to Tape"));

    // Montage as a deck, and LTC chase.
    auto* remotePage = new QWidget(this);
    auto* remoteForm = new QFormLayout(remotePage);
    remotePort_ = new QComboBox(this);
    remotePort_->setEditable(true);
    for (const std::string& p : SerialPort::available()) remotePort_->addItem(QString::fromStdString(p));
    remotePort_->setCurrentText(settings.value(QStringLiteral("deck/remotePort")).toString());
    remoteForm->addRow(tr("Answer as a deck on:"), remotePort_);
    programStart_ = new QLineEdit(settings.value(QStringLiteral("deck/programStart"), QStringLiteral("00:00:00:00")).toString(), this);
    remoteForm->addRow(tr("The Program starts at:"), programStart_);
    remote_ = new QPushButton(tr("Answer as a Deck"), this);
    remote_->setCheckable(true);
    connect(remote_, &QPushButton::clicked, this, &DeckDialog::toggleRemote);
    remoteForm->addRow(remote_);
    chaseInput_ = new QComboBox(this);
    chaseInput_->addItem(tr("Default input"), QString());
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) chaseInput_->addItem(d.description(), d.description());
    remoteForm->addRow(tr("LTC from:"), chaseInput_);
    chaseChannel_ = new QSpinBox(this);
    chaseChannel_->setRange(1, 32);
    remoteForm->addRow(tr("Channel:"), chaseChannel_);
    chaseOffset_ = new QSpinBox(this);
    chaseOffset_->setRange(-1000000, 1000000);
    chaseOffset_->setSuffix(tr(" frames"));
    remoteForm->addRow(tr("Offset:"), chaseOffset_);
    chase_ = new QPushButton(tr("Chase LTC"), this);
    chase_->setCheckable(true);
    connect(chase_, &QPushButton::clicked, this, &DeckDialog::toggleChase);
    remoteForm->addRow(chase_);
    tabs->addTab(remotePage, tr("Montage as a Deck"));

    activity_ = new QPlainTextEdit(this);
    activity_->setReadOnly(true);
    activity_->setMaximumBlockCount(500);
    activity_->setFixedHeight(90);
    layout->addWidget(activity_);
    connect(deck_, &DeckController::workProgress, activity_, &QPlainTextEdit::appendPlainText);
    connect(deck_, &DeckController::workFinished, this, [this](bool, const QString& m) { activity_->appendPlainText(m); });
    connect(deck_, &DeckController::remoteChanged, remote_, &QPushButton::setChecked);
    connect(deck_, &DeckController::chaseChanged, chase_, &QPushButton::setChecked);
    tick_ = new QTimer(this);
    tick_->setInterval(200);
    connect(tick_, &QTimer::timeout, this, &DeckDialog::refresh);
    tick_->start();
    refresh();
}

void DeckDialog::refresh() {
    if (!isVisible()) return;
    QString text = deck_->deckConnected() ? deck_->deckStatus() : tr("No deck connected");
    if (deck_->chasing()) text += QStringLiteral("\n") + deck_->chaseStatus();
    status_->setText(text);
    connect_->setText(deck_->deckConnected() ? tr("Disconnect") : tr("Connect"));
    remote_->setChecked(deck_->remoteRunning());  // (off once its port has gone)
}

void DeckDialog::connectDeck() {
    if (deck_->deckConnected()) {
        deck_->disconnectDeck();
        refresh();
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("deck/port"), port_->currentText());
    settings.setValue(QStringLiteral("deck/rate"), rate_->currentText());
    QString err;
    if (!deck_->connectDeck(port_->currentText(), rateNamed(rate_->currentText()), &err)) activity_->appendPlainText(err);
    refresh();
}

void DeckDialog::runBatch() {
    const Rational r = deck_->deckRate();
    const int b = int(std::lround(r.toDouble()));
    std::vector<LogEntry> log;
    if (!log_->text().isEmpty()) {
        std::string err;
        log = readCaptureLog(log_->text().toStdString(), b, b == 30 && r.den == 1001, &err);
        if (log.empty()) {
            activity_->appendPlainText(QString::fromStdString(err));
            return;
        }
    } else if (const Sequence* s = state_->sequence(); s && s->inPoint >= 0 && s->outPoint > s->inPoint) {
        // (the Program's marks read as the tape's timecode, from where the Program starts)
        LogEntry e;
        e.name = s->name;
        e.in = deck_->programStart() + s->inPoint;
        e.out = deck_->programStart() + s->outPoint + 1;
        log.push_back(e);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("deck/handles"), handles_->value());
    settings.setValue(QStringLiteral("deck/preroll"), preroll_->value());
    settings.setValue(QStringLiteral("deck/timeSource"), timeSource_->currentData());
    BatchCaptureSettings s;
    s.capture.video.device = sourceKind_->currentData().toString().toStdString();
    s.capture.video.url = source_->text().toStdString();
    s.capture.video.options = parseOptions(settings.value(QStringLiteral("capture/options")).toString());
    s.capture.codec = settings.value(QStringLiteral("capture/codec"), QStringLiteral("prores_hq")).toString().toStdString();
    s.folder = folder_->text().toStdString();
    s.handles = handles_->value();
    s.preroll = preroll_->value();
    s.source = timeSourceNamed(timeSource_->currentData().toString());
    QString err;
    if (!deck_->startBatchCapture(log, s, &err)) activity_->appendPlainText(err);
}

void DeckDialog::runEdit() {
    const Sequence* s = state_->sequence();
    if (!s || s->inPoint < 0 || s->outPoint < s->inPoint) {
        activity_->appendPlainText(tr("Mark the Program's In and Out first"));
        return;
    }
    const Rational r = deck_->deckRate();
    const int b = int(std::lround(r.toDouble()));
    TapeEdit e;
    e.in = parseTimecode(editIn_->text().toStdString(), b, nullptr, b == 30 && r.den == 1001);
    if (e.in < 0) {
        activity_->appendPlainText(tr("The tape's In is not a timecode"));
        return;
    }
    e.out = e.in + (s->outPoint - s->inPoint + 1);
    e.channels.assemble = assemble_->isChecked();
    e.channels.timecode = e.channels.assemble;
    e.channels.video = video_->isChecked();
    for (size_t i = 0; i < audio_.size(); ++i) e.channels.audio[i] = audio_[i]->isChecked();
    e.preroll = preroll_->value();
    QString err;
    if (!deck_->startEditToTape(e, s->inPoint, &err)) activity_->appendPlainText(err);
}

void DeckDialog::toggleRemote() {
    if (deck_->remoteRunning()) {
        deck_->stopRemote();
        return;
    }
    const Sequence* s = state_->sequence();
    const int b = s ? int(std::lround(s->fps.toDouble())) : 25;
    const int64_t start = parseTimecode(programStart_->text().toStdString(), b, nullptr, s && b == 30 && s->fps.den == 1001);
    deck_->setProgramStart(std::max<int64_t>(0, start));
    QSettings settings;
    settings.setValue(QStringLiteral("deck/remotePort"), remotePort_->currentText());
    settings.setValue(QStringLiteral("deck/programStart"), programStart_->text());
    QString err;
    if (!deck_->startRemote(remotePort_->currentText(), &err)) {
        activity_->appendPlainText(err);
        remote_->setChecked(false);
    }
}

void DeckDialog::toggleChase() {
    if (deck_->chasing()) {
        deck_->stopChase();
        return;
    }
    QString err;
    if (!deck_->startChase(chaseInput_->currentData().toString(), chaseChannel_->value() - 1, chaseOffset_->value(), 2, &err)) {
        activity_->appendPlainText(err);
        chase_->setChecked(false);
    }
}

}  // namespace montage
