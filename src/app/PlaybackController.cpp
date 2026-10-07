#include "PlaybackController.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/Compositor.h"

namespace montage {

// ---------------------------------------------------------------------------
// Renders frames on a background thread; only the newest request is served.

class RenderWorker : public QObject {
    Q_OBJECT
public:
    struct Request {
        std::shared_ptr<const Project> project;
        Id sequence = 0;
        FrameTime frame = 0;
        double scale = 1;
    };
    void request(const Request& r) {
        {
            QMutexLocker lock(&m_);
            pending_ = r;
            has_ = true;
        }
        QMetaObject::invokeMethod(this, &RenderWorker::process, Qt::QueuedConnection);
    }

signals:
    void rendered(const QImage& image, montage::FrameTime t);

private:
    void process() {
        Request r;
        {
            QMutexLocker lock(&m_);
            if (!has_) return;
            r = pending_;
            has_ = false;
        }
        if (!r.project) return;
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s) return;
        RenderOptions o;
        o.scale = r.scale;
        Image img = renderProgramFrame(*r.project, *s, r.frame, o);
        std::vector<uint8_t> rgba = toRgba8(img);
        QImage q(rgba.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888);
        emit rendered(q.copy(), r.frame);
    }

    QMutex m_;
    Request pending_;
    bool has_ = false;
};

// ---------------------------------------------------------------------------
// Pull-mode audio source feeding QAudioSink from the mixer.

class MixerDevice : public QIODevice {
    Q_OBJECT
public:
    explicit MixerDevice(QObject* parent) : QIODevice(parent) { mixer_.setNonBlocking(true); }

    void configure(std::shared_ptr<const Project> p, Id seq, int64_t startSample, bool int16) {
        QMutexLocker lock(&m_);
        project_ = std::move(p);
        seq_ = seq;
        sample_ = startSample;
        int16_ = int16;
        mixer_.reset();
    }
    void setProject(std::shared_ptr<const Project> p) {
        QMutexLocker lock(&m_);
        project_ = std::move(p);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 16) + QIODevice::bytesAvailable(); }

signals:
    void levels(float l, float r, const QVector<float>& tracks);

protected:
    qint64 readData(char* data, qint64 maxlen) override {
        const int bytesPerFrame = int16_ ? 4 : 8;
        int frames = int(maxlen / bytesPerFrame);
        if (frames <= 0) return 0;
        buf_.resize(size_t(frames) * 2);
        std::vector<MeterLevels> tl;
        {
            QMutexLocker lock(&m_);
            const Sequence* s = project_ ? project_->findSequence(seq_) : nullptr;
            if (s) mixer_.mix(*project_, *s, sample_, frames, buf_.data(), &tl);
            else std::fill(buf_.begin(), buf_.end(), 0.0f);
            sample_ += frames;
        }
        float pl = 0, pr = 0;
        for (int i = 0; i < frames; ++i) {
            pl = std::max(pl, std::fabs(buf_[size_t(i) * 2]));
            pr = std::max(pr, std::fabs(buf_[size_t(i) * 2 + 1]));
        }
        if (int16_) {
            auto* d = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < buf_.size(); ++i) d[i] = int16_t(std::lround(std::clamp(buf_[i], -1.0f, 1.0f) * 32767.0f));
        } else {
            std::memcpy(data, buf_.data(), size_t(frames) * 8);
        }
        QVector<float> tracks;
        tracks.reserve(int(tl.size()) * 2);
        for (const auto& lv : tl) {
            tracks << lv.peakL << lv.peakR;
        }
        emit levels(pl, pr, tracks);
        return qint64(frames) * bytesPerFrame;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QMutex m_;
    std::shared_ptr<const Project> project_;
    Id seq_ = 0;
    int64_t sample_ = 0;
    bool int16_ = false;
    AudioMixer mixer_;
    std::vector<float> buf_;
};

// ---------------------------------------------------------------------------

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    project_ = std::make_shared<Project>(makeDefaultProject());
    renderThread_ = new QThread(this);
    renderThread_->setObjectName("montage-render");
    worker_ = new RenderWorker;
    worker_->moveToThread(renderThread_);
    connect(renderThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &RenderWorker::rendered, this, &PlaybackController::frameRendered, Qt::QueuedConnection);
    renderThread_->start();
    device_ = new MixerDevice(this);
    connect(device_, &MixerDevice::levels, this, &PlaybackController::audioLevels);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(8);
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::tick);
}

PlaybackController::~PlaybackController() {
    stopAudio();
    renderThread_->quit();
    renderThread_->wait();
}

void PlaybackController::setProject(const Project& p, Id sequenceId) {
    project_ = std::make_shared<const Project>(p);
    sequenceId_ = sequenceId;
    device_->setProject(project_);
}

const Sequence* PlaybackController::sequence() const {
    return project_ ? project_->findSequence(sequenceId_) : nullptr;
}

void PlaybackController::setPreviewScale(double s) {
    scale_ = std::clamp(s, 0.05, 1.0);
    requestFrame();
}

FrameTime PlaybackController::clampToSequence(FrameTime t) const { return std::max<FrameTime>(0, t); }

void PlaybackController::requestFrame() {
    if (!sequence()) return;
    // Paused frames render at full quality; playback uses the preview scale.
    worker_->request({project_, sequenceId_, position_, isPlaying() ? scale_ : 1.0});
}

void PlaybackController::seek(FrameTime t) {
    t = clampToSequence(t);
    bool playing = isPlaying();
    if (t == position_ && !playing) return;
    position_ = t;
    if (playing) {
        startFrame_ = t;
        clock_.restart();
        if (speed_ == 1) startAudio(t);
    }
    requestFrame();
    emit positionChanged(position_);
}

void PlaybackController::step(int frames) {
    if (isPlaying()) pause();
    seek(position_ + frames);
}

void PlaybackController::play() {
    const Sequence* s = sequence();
    if (!s) return;
    if (speed_ == 1) return;
    FrameTime end = loop_ && s->outPoint >= 0 ? s->outPoint + 1 : s->duration();
    if (position_ >= end && end > 0) position_ = loop_ && s->inPoint >= 0 ? s->inPoint : 0;
    speed_ = 1;
    startFrame_ = position_;
    clock_.restart();
    startAudio(position_);
    timer_.start();
    emit playingChanged(true);
}

void PlaybackController::pause() {
    if (speed_ == 0) return;
    speed_ = 0;
    timer_.stop();
    stopAudio();
    requestFrame();  // re-render at full quality
    emit playingChanged(false);
}

void PlaybackController::togglePlay() {
    if (isPlaying()) pause();
    else play();
}

void PlaybackController::shuttle(int direction) {
    if (direction == 0) {
        pause();
        return;
    }
    double next;
    if (direction > 0) next = speed_ <= 0 ? 1 : std::min(speed_ * 2, 8.0);
    else next = speed_ >= 0 ? -1 : std::max(speed_ * 2, -8.0);
    bool wasPlaying = isPlaying();
    speed_ = next;
    startFrame_ = position_;
    clock_.restart();
    if (speed_ == 1) startAudio(position_);
    else stopAudio();
    timer_.start();
    if (!wasPlaying) emit playingChanged(true);
}

void PlaybackController::tick() {
    const Sequence* s = sequence();
    if (!s || speed_ == 0) return;
    double elapsed = clock_.nsecsElapsed() / 1e9;
    FrameTime t = startFrame_ + FrameTime(std::floor(elapsed * s->fpsValue() * speed_));
    FrameTime end = s->duration();
    FrameTime loopIn = s->inPoint >= 0 ? s->inPoint : 0;
    FrameTime loopOut = s->outPoint >= 0 ? s->outPoint + 1 : end;
    if (loop_ && speed_ > 0 && t >= loopOut) {
        t = loopIn;
        startFrame_ = t;
        clock_.restart();
        if (speed_ == 1) startAudio(t);
    } else if (speed_ > 0 && t >= end) {
        t = end;
        position_ = t;
        emit positionChanged(t);
        pause();
        return;
    } else if (speed_ < 0 && t <= 0) {
        position_ = 0;
        emit positionChanged(0);
        pause();
        return;
    }
    if (t != position_) {
        position_ = t;
        worker_->request({project_, sequenceId_, t, scale_});
        emit positionChanged(t);
    }
}

void PlaybackController::startAudio(FrameTime from) {
    stopAudio();
    const Sequence* s = sequence();
    if (!s) return;
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;
    QAudioFormat fmt;
    fmt.setSampleRate(s->sampleRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    bool int16 = false;
    if (!dev.isFormatSupported(fmt)) {
        fmt.setSampleFormat(QAudioFormat::Int16);
        int16 = true;
        if (!dev.isFormatSupported(fmt)) return;
    }
    int64_t startSample = int64_t(std::llround(double(from) * s->sampleRate / s->fpsValue()));
    device_->configure(project_, sequenceId_, startSample, int16);
    if (!device_->isOpen()) device_->open(QIODevice::ReadOnly);
    sink_ = new QAudioSink(dev, fmt, this);
    sink_->setBufferSize(fmt.bytesForDuration(80000));  // ~80 ms
    sink_->start(device_);
}

void PlaybackController::stopAudio() {
    if (!sink_) return;
    sink_->stop();
    sink_->deleteLater();
    sink_ = nullptr;
}

}  // namespace montage

#include "PlaybackController.moc"
