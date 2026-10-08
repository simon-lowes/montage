#include "Voiceover.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCheckBox>
#include <QComboBox>
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

void WavWriter::write(const float* interleaved, qint64 frames) {
    if (!file_.isOpen() || frames <= 0) return;
    file_.write(reinterpret_cast<const char*>(interleaved), frames * channels_ * qint64(sizeof(float)));
    frames_ += frames;
}

bool WavWriter::close() {
    if (!file_.isOpen()) return false;
    const quint32 data = quint32(frames_ * channels_ * 4);
    file_.seek(4);
    put32(file_, 36 + data);
    file_.seek(40);
    put32(file_, data);
    file_.close();
    return true;
}

// ---------------------------------------------------------------------------
// VoiceoverRecorder

VoiceoverRecorder::VoiceoverRecorder(EditorState* state, QObject* parent) : QObject(parent), state_(state) {}

VoiceoverRecorder::~VoiceoverRecorder() {
    if (isRecording()) cancel();
}

QStringList VoiceoverRecorder::inputs() {
    QStringList out;
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
    return base + QStringLiteral("/Voiceover");
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
    if (openDevice) {
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
    const QString stem = QString::fromStdString(s->name) + QStringLiteral(" VO ");
    int n = 1;
    do path_ = takeFolder() + '/' + stem + QString::number(n++) + QStringLiteral(".wav");
    while (QFileInfo::exists(path_));
    if (!writer_.open(path_, rate_, channels_)) {
        state_->message(tr("Cannot write %1").arg(path_), 5000);
        delete source_;
        source_ = nullptr;
        return false;
    }
    stopping_ = false;
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
    stopping_ = true;
    if (source_) {
        source_->stop();
        source_->deleteLater();
        source_ = nullptr;
    }
    const qint64 frames = writer_.frames();
    writer_.close();
    if (frames == 0) {
        QFile::remove(path_);
        return 0;
    }
    lastTake_ = path_;
    // Into the bin's Voiceover folder, then onto the track where recording began.
    const auto ids = state_->importFiles({path_}, nullptr, QStringLiteral("Voiceover"));
    if (ids.empty()) return 0;
    const Id media = ids.front();
    const FrameTime at = at_;
    const int track = track_;
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

void VoiceoverRecorder::cancel() {
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
    setWindowTitle(tr("Record Voiceover"));
    setObjectName(QStringLiteral("voiceoverDialog"));
    auto* form = new QFormLayout(this);
    input_ = new QComboBox(this);
    input_->setObjectName(QStringLiteral("voiceoverInput"));
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
    form->addRow(tr("Level:"), meter_);
    form->addRow(status_);
    form->addRow(record_);
    connect(record_, &QPushButton::clicked, this, &VoiceoverDialog::toggle);
    connect(recorder_, &VoiceoverRecorder::level, this, [this](float peak) {
        const double db = peak > 0 ? 20 * std::log10(peak) : -60;
        meter_->setValue(int(std::clamp((db + 60) / 60, 0.0, 1.0) * 1000));
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
    const bool punch = punch_->isChecked() && punch_->isEnabled();
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
    record_->setText(tr("Record"));
    status_->setText(tr("Saved %1").arg(QFileInfo(recorder_->lastTake()).fileName()));
    meter_->setValue(0);
}

}  // namespace montage
