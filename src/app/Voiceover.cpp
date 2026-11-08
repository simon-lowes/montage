#include "Voiceover.h"
#include "Language.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMediaDevices>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorState.h"
#include "PlaybackController.h"
#include "core/EditOps.h"
#include "core/TakeLanes.h"
#include "core/TrackGroups.h"

namespace montage {

// ---------------------------------------------------------------------------
// WavWriter

namespace {

void put32(QFile& f, quint32 v) {
    v = qToLittleEndian(v);
    f.write(reinterpret_cast<const char*>(&v), 4);
}
void put16(QFile& f, quint16 v) {
    v = qToLittleEndian(v);
    f.write(reinterpret_cast<const char*>(&v), 2);
}

}  // namespace

WavWriter::~WavWriter() {
    if (file_.isOpen()) close();
}

bool WavWriter::open(const QString& path, int sampleRate, int channels) {
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    rate_ = sampleRate;
    channels_ = std::max(1, channels);
    frames_ = 0;
    failed_ = false;
    // RIFF header with the sizes left for close() (WAVE_FORMAT_IEEE_FLOAT, 32 bits).
    file_.write("RIFF", 4);
    put32(file_, 0);
    file_.write("WAVEfmt ", 8);
    put32(file_, 16);
    put16(file_, 3);
    put16(file_, quint16(channels_));
    put32(file_, quint32(rate_));
    put32(file_, quint32(rate_ * channels_ * 4));
    put16(file_, quint16(channels_ * 4));
    put16(file_, 32);
    file_.write("data", 4);
    put32(file_, 0);
    return true;
}

bool WavWriter::write(const float* interleaved, qint64 frames) {
    if (!file_.isOpen() || frames <= 0) return file_.isOpen();
    const qint64 bytes = frames * channels_ * qint64(sizeof(float));
    const qint64 wrote = file_.write(reinterpret_cast<const char*>(interleaved), bytes);
    if (wrote != bytes) failed_ = true;
    frames_ += std::max<qint64>(0, wrote) / (channels_ * qint64(sizeof(float)));  // (the header says what is there)
    return !failed_;
}

bool WavWriter::close() {
    if (!file_.isOpen()) return false;
    const quint32 data = quint32(frames_ * channels_ * 4);
    file_.seek(4);
    put32(file_, 36 + data);
    file_.seek(40);
    put32(file_, data);
    if (!file_.flush() || file_.error() != QFileDevice::NoError) failed_ = true;
    file_.close();
    return !failed_;
}

// ---------------------------------------------------------------------------
// VoiceoverRecorder

VoiceoverRecorder::VoiceoverRecorder(EditorState* state, QObject* parent) : QObject(parent), state_(state) {
    drain_.setInterval(20);
    connect(&drain_, &QTimer::timeout, this, &VoiceoverRecorder::drainDevice);
}

VoiceoverRecorder::~VoiceoverRecorder() {
    if (isRecording()) cancel();
}

QStringList VoiceoverRecorder::inputs() {
    QStringList out;
    if (const audiodev::Engine& engine = audiodev::Engine::shared(); engine.active()) {
        const audiodev::DeviceSettings ds = engine.settings();
        for (int k = 0; k < ds.inputs; ++k) out << QString::fromStdString(audiodev::inputName(ds, k));
        return out;
    }
    const QAudioDevice def = QMediaDevices::defaultAudioInput();
    if (!def.isNull()) out << def.description();
    for (const QAudioDevice& d : QMediaDevices::audioInputs())
        if (d.id() != def.id()) out << d.description();
    return out;
}

QString VoiceoverRecorder::takeFolder() const {
    const QString project = state_->filePath();
    const QString base = project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                           : QFileInfo(project).absolutePath();
    return base + '/' + folder_;
}

void VoiceoverRecorder::setTakeNaming(const QString& folder, const QString& stem) {
    folder_ = folder.isEmpty() ? QStringLiteral("Voiceover") : folder;
    stem_ = stem;
}

bool VoiceoverRecorder::start(FrameTime at, int track, FrameTime stopAt, const QString& input, bool openDevice, int sampleRate,
                              int channels) {
    if (isRecording()) return false;
    const Sequence* s = state_->sequence();
    if (!s) return false;
    at_ = at;
    stopAt_ = stopAt > at ? stopAt : -1;
    track_ = std::clamp(track, 0, std::max(0, int(s->audioTracks.size()) - 1));
    rate_ = sampleRate;
    channels_ = std::max(1, channels);
    format_ = 0;
    sampleBytes_ = 4;
    audiodev::Engine& engine = audiodev::Engine::shared();
    const bool onDevice = openDevice && engine.active();
    if (onDevice) {
        // The device's inputs from the one named (its first, else), at the sequence's rate.
        const QStringList names = inputs();
        firstInput_ = std::max(0, int(names.indexOf(input)));
        rate_ = s->sampleRate;
    } else if (openDevice) {
        QAudioDevice device = QMediaDevices::defaultAudioInput();
        for (const QAudioDevice& d : QMediaDevices::audioInputs())
            if (!input.isEmpty() && d.description() == input) device = d;
        if (device.isNull()) {
            state_->message(tr("No microphone or audio input was found"), 5000);
            return false;
        }
        QAudioFormat fmt;
        fmt.setSampleRate(48000);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(fmt)) fmt = device.preferredFormat();
        rate_ = fmt.sampleRate();
        channels_ = std::max(1, fmt.channelCount());
        format_ = fmt.sampleFormat() == QAudioFormat::Int16 ? 1 : fmt.sampleFormat() == QAudioFormat::Int32 ? 2 : 0;
        sampleBytes_ = format_ == 1 ? 2 : 4;
        source_ = new QAudioSource(device, fmt, this);
    }
    // A new numbered file for the take.
    QDir().mkpath(takeFolder());
    const QString stem = stem_.isEmpty() ? QString::fromStdString(s->name) + QStringLiteral(" VO ") : stem_;
    int n = 1;
    // (A loop recording's passes keep its name, "<name> pass 1.wav" ..., so that name is taken too.)
    do path_ = takeFolder() + '/' + stem + QString::number(n++) + QStringLiteral(".wav");
    while (QFileInfo::exists(path_) || QFileInfo::exists(path_.chopped(4) + QStringLiteral(" pass 1.wav")));
    if (!writer_.open(path_, rate_, channels_)) {
        state_->message(tr("Cannot write %1").arg(path_), 5000);
        delete source_;
        source_ = nullptr;
        return false;
    }
    stopping_ = false;
    if (onDevice) {
        // Placed by the playback it follows (the round trip taken off), else the input's latency taken off its start.
        const int64_t first = std::llround(double(at_) / s->fpsValue() * rate_);
        const audiodev::EngineStatus st = engine.status();
        const int64_t fallback = st.rate > 0 ? std::llround(double(st.measured ? st.roundTrip / 2 : st.latency.input) * rate_ / st.rate) : 0;
        aligner_ = audiodev::TakeAligner(first, clock_ != nullptr, fallback);
        {
            std::lock_guard<std::mutex> lock(pendingM_);
            pending_.clear();
        }
        std::string error;
        armed_ = clock_ == nullptr;
        tap_ = engine.addInput(clock_, rate_, [this](const float* in, int channels, int frames, int64_t sample) {
            if (!armed_.load()) return;
            const audiodev::TakeAligner::Step step = aligner_.next(sample, frames);
            if (step.silence == 0 && step.skip >= frames) return;
            std::lock_guard<std::mutex> lock(pendingM_);
            if (pending_.size() > size_t(rate_) * size_t(channels_) * 120) return;  // (no one is draining: a stuck main thread)
            pending_.resize(pending_.size() + size_t(step.silence) * size_t(channels_), 0.0f);
            for (int i = step.skip; i < frames; ++i)
                for (int c = 0; c < channels_; ++c) {
                    const int from = firstInput_ + c;
                    pending_.push_back(from < channels ? in[size_t(i) * size_t(channels) + size_t(from)] : 0.0f);
                }
        }, &error);
        if (tap_ < 0) {
            cancel();
            state_->message(tr("The audio input could not be opened: %1").arg(QString::fromStdString(error)), 5000);
            return false;
        }
        drain_.start();
        if (clock_)
            QMetaObject::invokeMethod(this, [this, take = ++takeNumber_] {
                if (take == takeNumber_ && tap_ >= 0) armed_ = true;
            }, Qt::QueuedConnection);
    }
    if (source_) {
        io_ = source_->start();
        if (!io_) {
            cancel();
            state_->message(tr("The audio input could not be opened"), 5000);
            return false;
        }
        connect(io_, &QIODevice::readyRead, this, &VoiceoverRecorder::readDevice);
    }
    return true;
}

bool VoiceoverRecorder::startLoop(FrameTime in, FrameTime out, FrameTime preroll, int track, const QString& input, bool openDevice,
                                  int sampleRate, int channels) {
    if (isRecording() || out <= in) return false;
    preroll = std::clamp<FrameTime>(preroll, 0, in);
    loopIn_ = in, loopOut_ = out, preroll_ = preroll;
    // Record arming through a track group (core/TrackGroups.h): every track of the group records, one input channel
    // each (the target's first, then the others in order), and their passes share pass ids and take numbers.
    groupTracks_.clear();
    if (const Sequence* s = state_->sequence(); s && track >= 0 && track < int(s->audioTracks.size()))
        for (TrackRef r : groupTracks(*s, {TrackKind::Audio, track}, GroupSwitch::Record))
            if (r.kind == TrackKind::Audio) groupTracks_.push_back(r.index);
    if (groupTracks_.size() > 1) channels = std::max(channels, int(groupTracks_.size()));
    else groupTracks_.clear();
    if (start(in - preroll, track, -1, input, openDevice, sampleRate, channels)) return true;
    loopIn_ = loopOut_ = preroll_ = 0;
    groupTracks_.clear();
    return false;
}

int VoiceoverRecorder::passes() const {
    const Sequence* s = state_->sequence();
    if (!isLooping() || !s) return 0;
    const double fps = s->fpsValue(), length = double(loopOut_ - loopIn_) / fps * rate_;
    return int(loopPasses(writer_.frames(), std::llround(double(preroll_) / fps * rate_), length).size());
}

void VoiceoverRecorder::readDevice() {
    if (!io_) return;
    const QByteArray data = io_->readAll();
    const qint64 frames = data.size() / (sampleBytes_ * channels_);
    if (frames <= 0) return;
    std::vector<float> buf(size_t(frames * channels_));
    for (size_t i = 0; i < buf.size(); ++i) {
        const char* p = data.constData() + i * size_t(sampleBytes_);
        if (format_ == 1) buf[i] = qFromLittleEndian<qint16>(p) / 32768.0f;
        else if (format_ == 2) buf[i] = float(qFromLittleEndian<qint32>(p) / 2147483648.0);
        else {
            float f;
            std::memcpy(&f, p, 4);
            buf[i] = f;
        }
    }
    feed(buf.data(), frames);
}

void VoiceoverRecorder::drainDevice() {
    std::vector<float> got;
    {
        std::lock_guard<std::mutex> lock(pendingM_);
        got.swap(pending_);
    }
    if (!got.empty()) feed(got.data(), qint64(got.size() / size_t(channels_)));
}

void VoiceoverRecorder::closeDevice() {
    if (tap_ < 0) return;
    audiodev::Engine::shared().removeInput(tap_);
    tap_ = -1;
    drain_.stop();
}

void VoiceoverRecorder::feed(const float* interleaved, qint64 frames) {
    if (!isRecording() || stopping_) return;
    const Sequence* s = state_->sequence();
    // Punch-in: no further than Out.
    if (stopAt_ > at_ && s) {
        const qint64 limit = qint64(std::llround(double(stopAt_ - at_) / s->fpsValue() * rate_));
        frames = std::min(frames, limit - writer_.frames());
    }
    if (frames > 0) {
        writer_.write(interleaved, frames);
        float peak = 0;
        for (qint64 i = 0; i < frames * channels_; ++i) peak = std::max(peak, std::fabs(interleaved[i]));
        emit level(peak);
    }
    if (stopAt_ > at_ && s && writer_.frames() >= qint64(std::llround(double(stopAt_ - at_) / s->fpsValue() * rate_)))
        QMetaObject::invokeMethod(this, [this] { if (isRecording()) stop(); }, Qt::QueuedConnection);
}

Id VoiceoverRecorder::stop() {
    if (!isRecording()) return 0;
    if (tap_ >= 0) {
        closeDevice();
        drainDevice();  // (what came before the stop)
    }
    stopping_ = true;
    if (source_) {
        source_->stop();
        source_->deleteLater();
        source_ = nullptr;
    }
    const qint64 frames = writer_.frames();
    const bool whole = writer_.close();
    if (frames == 0) {
        QFile::remove(path_);
        loopIn_ = loopOut_ = 0;
        return 0;
    }
    if (!whole) {
        state_->message(tr("The recording could not all be written (is the disk full?): what was is kept in %1").arg(path_), 8000);
        // (a loop recording cut short is not cut into passes: it is kept whole, as it is)
        if (isLooping()) {
            loopIn_ = loopOut_ = preroll_ = 0;
            lastTake_ = path_;
            emit stopped(0);
            return 0;
        }
    }
    if (isLooping()) return stopLoop();
    lastTake_ = path_;
    // Into the bin's Voiceover folder, then onto the track where recording began.
    const auto ids = state_->importFiles({path_}, nullptr, folder_);
    if (ids.empty()) return 0;
    const Id media = ids.front();
    const FrameTime at = at_;
    const int track = track_;
    emit taken(media, at);
    if (!placing_) {
        emit stopped(0);
        return 0;
    }
    std::vector<Id> created;
    state_->apply(tr("Record Voiceover"), [&](Project& p, Sequence& sq) {
        const TrackRef a{TrackKind::Audio, std::min(track, int(sq.audioTracks.size()) - 1)};
        const TrackRef v{TrackKind::Video, 0};
        edit::Result r = edit::placeMedia(p, sq, media, at, 0, -1, v, a, false);
        created = r.created;
        return r;
    });
    const Id clip = created.empty() ? 0 : created.front();
    emit stopped(clip);
    return clip;
}

Id VoiceoverRecorder::stopLoop() {
    const FrameTime in = loopIn_, out = loopOut_, preroll = preroll_;
    loopIn_ = loopOut_ = preroll_ = 0;
    const Sequence* s = state_->sequence();
    QFile whole(path_);
    if (!s || !whole.open(QIODevice::ReadOnly)) {
        emit stopped(0);
        return 0;
    }
    // The recording as written (a 44-byte header, then floats), cut into its passes.
    whole.seek(44);
    const QByteArray data = whole.readAll();
    whole.close();
    const qint64 frames = data.size() / qint64(sizeof(float) * size_t(channels_));
    const float* samples = reinterpret_cast<const float*>(data.constData());
    const double fps = s->fpsValue();
    const std::vector<LoopPass> passes =
        loopPasses(frames, std::llround(double(preroll) / fps * rate_), double(out - in) / fps * rate_, rate_);
    QStringList files;
    bool written = true;
    const QString stem = QFileInfo(path_).completeBaseName() + QStringLiteral(" pass ");
    const std::vector<int> grouped = groupTracks_;
    groupTracks_.clear();
    if (grouped.size() > 1) {
        // A grouped recording: each track's channel of each pass, a mono file of its own (track by track).
        std::vector<float> mono;
        for (size_t t = 0; t < grouped.size() && written; ++t) {
            const int ch = int(t) % channels_;
            const QString name = grouped[t] < int(s->audioTracks.size()) ? QString::fromStdString(s->audioTracks[size_t(grouped[t])].name) : QString();
            for (size_t k = 0; k < passes.size(); ++k) {
                const QString file = takeFolder() + '/' + stem + QString::number(k + 1) + ' ' + (name.isEmpty() ? QString::number(t + 1) : name) +
                                     QStringLiteral(".wav");
                mono.resize(size_t(passes[k].count));
                for (qint64 i = 0; i < passes[k].count; ++i) mono[size_t(i)] = samples[(passes[k].first + i) * channels_ + ch];
                WavWriter w;
                if (!w.open(file, rate_, 1) || (w.write(mono.data(), passes[k].count), !w.close())) {
                    QFile::remove(file);
                    written = false;
                    break;
                }
                files << file;
            }
        }
    } else {
        for (size_t k = 0; k < passes.size(); ++k) {
            const QString file = takeFolder() + '/' + stem + QString::number(k + 1) + QStringLiteral(".wav");
            WavWriter w;
            if (!w.open(file, rate_, channels_)) {
                written = false;
                break;
            }
            w.write(samples + passes[k].first * channels_, passes[k].count);
            if (!w.close()) {
                QFile::remove(file);
                written = false;
                break;
            }
            files << file;
        }
    }
    // The whole recording goes only once every pass is safely written; else it is kept, and the passes written so far
    // removed (nothing is lost, and nothing goes on the timeline half done).
    if (!written || files.isEmpty()) {
        for (const QString& f : files) QFile::remove(f);
        state_->message(written ? tr("The loop recording ended before a whole pass: it is kept in %1").arg(path_)
                                : tr("The passes of the loop recording could not be written (is the disk full?): it is kept in %1").arg(path_),
                        8000);
        lastTake_ = path_;
        emit stopped(0);
        return 0;
    }
    QFile::remove(path_);
    lastTake_ = files.back();
    const auto ids = state_->importFiles(files, nullptr, folder_);
    for (const Id media : ids) emit taken(media, in);
    if (ids.size() != files.size()) {  // (a pass that did not import would put the others on the wrong lanes)
        state_->message(tr("Some passes of the loop recording could not be imported"), 5000);
        emit stopped(0);
        return 0;
    }
    const int track = track_;
    std::vector<bool> complete;
    for (const LoopPass& pass : passes) complete.push_back(pass.complete);
    Id clip = 0;
    state_->apply(tr("Loop Record"), [&](Project& p, Sequence& sq) {
        if (sq.audioTracks.empty()) return edit::Result::fail("No audio track");
        if (grouped.size() > 1) {
            // The passes on every grouped track, recorded together (one pass id and take number per pass).
            std::vector<TrackRef> tracks;
            std::vector<std::vector<Id>> media;
            for (size_t t = 0; t < grouped.size(); ++t) {
                if (grouped[t] >= int(sq.audioTracks.size())) return edit::Result::fail("A grouped track has gone");
                tracks.push_back({TrackKind::Audio, grouped[t]});
                media.emplace_back(ids.begin() + long(t * passes.size()), ids.begin() + long((t + 1) * passes.size()));
            }
            edit::Result r = edit::addGroupLoopTakes(p, sq, tracks, media, complete, in, out);
            const size_t lanes = ids.size();
            if (r.ok && r.created.size() > lanes) clip = r.created[lanes];  // the target track's comp
            return r;
        }
        const TrackRef a{TrackKind::Audio, std::min(track, int(sq.audioTracks.size()) - 1)};
        edit::Result r = edit::addLoopTakes(p, sq, a, ids, complete, in, out);
        if (r.ok && r.created.size() > ids.size()) clip = r.created.back();
        return r;
    });
    emit stopped(clip);
    return clip;
}

void VoiceoverRecorder::cancel() {
    loopIn_ = loopOut_ = preroll_ = 0;
    groupTracks_.clear();
    closeDevice();
    {
        std::lock_guard<std::mutex> lock(pendingM_);
        pending_.clear();
    }
    if (source_) {
        source_->stop();
        source_->deleteLater();
        source_ = nullptr;
    }
    if (writer_.isOpen()) {
        writer_.close();
        QFile::remove(path_);
    }
}

// ---------------------------------------------------------------------------
// VoiceoverDialog

VoiceoverDialog::VoiceoverDialog(EditorState* state, PlaybackController* program, QWidget* parent)
    : QDialog(parent), state_(state), program_(program), recorder_(new VoiceoverRecorder(state, this)) {
    recorder_->setClock(program);
    setWindowTitle(tr("Record Voiceover"));
    setObjectName(QStringLiteral("voiceoverDialog"));
    auto* form = new QFormLayout(this);
    input_ = new QComboBox(this);
    input_->setObjectName(QStringLiteral("voiceoverInput"));
    language::keepUntranslated(input_);  // (the computer's own names for its devices)
    input_->addItems(VoiceoverRecorder::inputs());
    track_ = new QComboBox(this);
    track_->setObjectName(QStringLiteral("voiceoverTrack"));
    if (const Sequence* s = state_->sequence())
        for (size_t i = 0; i < s->audioTracks.size(); ++i)
            track_->addItem(QString::fromStdString(s->audioTracks[i].name.empty() ? "A" + std::to_string(i + 1) : s->audioTracks[i].name));
    track_->setCurrentIndex(std::clamp(state_->targetAudioTrack(), 0, std::max(0, track_->count() - 1)));
    countdown_ = new QCheckBox(tr("Count down 3 seconds"), this);
    countdown_->setChecked(true);
    punch_ = new QCheckBox(tr("Punch in: record from In to Out"), this);
    punch_->setObjectName(QStringLiteral("voiceoverPunch"));
    const Sequence* s = state_->sequence();
    punch_->setEnabled(s && s->inPoint >= 0 && s->outPoint > s->inPoint);
    // Loop: In to Out again and again, each pass a take lane (core/TakeLanes.h), after a pre-roll heard once.
    loop_ = new QCheckBox(tr("Loop: each pass from In to Out a take"), this);
    loop_->setObjectName(QStringLiteral("voiceoverLoop"));
    loop_->setEnabled(punch_->isEnabled());
    preroll_ = new QDoubleSpinBox(this);
    preroll_->setObjectName(QStringLiteral("voiceoverPreroll"));
    preroll_->setRange(0, 10);
    preroll_->setDecimals(1);
    preroll_->setSuffix(tr(" s pre-roll"));
    preroll_->setValue(2);
    preroll_->setEnabled(false);
    connect(loop_, &QCheckBox::toggled, preroll_, &QWidget::setEnabled);
    meter_ = new QProgressBar(this);
    meter_->setRange(0, 1000);
    meter_->setTextVisible(false);
    meter_->setMaximumHeight(10);
    status_ = new QLabel(input_->count() ? tr("Ready") : tr("No audio input found"), this);
    status_->setObjectName(QStringLiteral("voiceoverStatus"));
    record_ = new QPushButton(tr("Record"), this);
    record_->setObjectName(QStringLiteral("voiceoverRecord"));
    record_->setEnabled(input_->count() > 0);
    form->addRow(tr("Input:"), input_);
    form->addRow(tr("Track:"), track_);
    form->addRow(countdown_);
    form->addRow(punch_);
    auto* loopRow = new QHBoxLayout;
    loopRow->addWidget(loop_);
    loopRow->addWidget(preroll_);
    form->addRow(loopRow);
    form->addRow(tr("Level:"), meter_);
    form->addRow(status_);
    form->addRow(record_);
    connect(record_, &QPushButton::clicked, this, &VoiceoverDialog::toggle);
    connect(recorder_, &VoiceoverRecorder::level, this, [this](float peak) {
        const double db = peak > 0 ? 20 * std::log10(peak) : -60;
        meter_->setValue(int(std::clamp((db + 60) / 60, 0.0, 1.0) * 1000));
        if (recorder_->isLooping()) status_->setText(tr("Recording pass %1...").arg(recorder_->passes() + 1));
    });
    connect(recorder_, &VoiceoverRecorder::stopped, this, [this](Id) { finish(); });
    tick_.setInterval(1000);
    connect(&tick_, &QTimer::timeout, this, [this] {
        if (--count_ > 0) {
            status_->setText(tr("Recording in %1...").arg(count_));
            return;
        }
        tick_.stop();
        begin();
    });
}

void VoiceoverDialog::toggle() {
    if (recorder_->isRecording()) {
        recorder_->stop();
        return;
    }
    if (tick_.isActive()) {  // cancel the countdown
        tick_.stop();
        status_->setText(tr("Ready"));
        record_->setText(tr("Record"));
        return;
    }
    record_->setText(tr("Stop"));
    if (countdown_->isChecked()) {
        count_ = 3;
        status_->setText(tr("Recording in %1...").arg(count_));
        tick_.start();
    } else {
        begin();
    }
}

void VoiceoverDialog::begin() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    const bool loop = loop_->isChecked() && loop_->isEnabled();
    const bool punch = punch_->isChecked() && punch_->isEnabled();
    if (loop) {
        const FrameTime pre = std::min<FrameTime>(s->inPoint, FrameTime(std::llround(preroll_->value() * s->fpsValue())));
        if (!recorder_->startLoop(s->inPoint, s->outPoint + 1, pre, track_->currentIndex(), input_->currentText())) {
            record_->setText(tr("Record"));
            status_->setText(tr("Could not start recording"));
            return;
        }
        status_->setText(tr("Recording pass 1..."));
        if (program_) {
            wasLooping_ = program_->loop();
            program_->setLoop(true);
            program_->seek(s->inPoint - pre);
            if (!program_->isPlaying()) program_->play();
        }
        return;
    }
    const FrameTime at = punch ? s->inPoint : state_->playhead();
    const FrameTime out = punch ? s->outPoint + 1 : -1;
    if (!recorder_->start(at, track_->currentIndex(), out, input_->currentText())) {
        record_->setText(tr("Record"));
        status_->setText(tr("Could not start recording"));
        return;
    }
    status_->setText(tr("Recording..."));
    // The picture plays along from where the take starts.
    if (program_) {
        program_->seek(at);
        if (!program_->isPlaying()) program_->play();
    }
}

void VoiceoverDialog::finish() {
    if (program_ && program_->isPlaying()) program_->togglePlay();
    if (program_ && wasLooping_) program_->setLoop(*wasLooping_);
    wasLooping_.reset();
    record_->setText(tr("Record"));
    status_->setText(tr("Saved %1").arg(QFileInfo(recorder_->lastTake()).fileName()));
    meter_->setValue(0);
}

}  // namespace montage
