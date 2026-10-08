#include "PlaybackController.h"

#include "media/Loudness.h"

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
#include <iterator>
#include <map>

#include "render/Compositor.h"

namespace montage {

// ---------------------------------------------------------------------------
// Renders frames on a background thread. Only the newest request is served;
// while playing, idle time renders the frames ahead of the playhead into a
// small cache so playback keeps up with complex timelines.

class RenderWorker : public QObject {
    Q_OBJECT
public:
    struct Request {
        std::shared_ptr<const Project> project;
        Id sequence = 0;
        FrameTime frame = 0;
        double scale = 1;
        bool proxies = false;
        int direction = 0;  // playback step (+1, -2 ...), 0 when paused
        bool captions = false;
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
    static constexpr int kAhead = 24;
    static constexpr size_t kMaxCached = 48;

    bool sameContext(const Request& r) const {
        return r.project == ctx_.project && r.sequence == ctx_.sequence && r.scale == ctx_.scale && r.proxies == ctx_.proxies &&
               r.captions == ctx_.captions;
    }

    QImage render(const Request& r, FrameTime t) {
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s) return {};
        RenderOptions o;
        o.scale = r.scale;
        o.useProxies = r.proxies;
        o.captions = r.captions;
        o.displaySpace = "rec709";  // HDR and wide-gamut sequences are previewed tone mapped to SDR
        Image img = renderProgramFrame(*r.project, *s, t, o);
        QImage out(img.width, img.height, QImage::Format_RGBA8888);
        toRgba8(img, out.bits(), size_t(out.bytesPerLine()));
        return out;
    }

    void process() {
        Request r;
        {
            QMutexLocker lock(&m_);
            if (!has_) return;
            r = pending_;
            has_ = false;
        }
        if (!r.project) return;
        if (!sameContext(r)) {
            cache_.clear();
            ctx_ = r;
        }
        ctx_.direction = r.direction;
        ctx_.frame = r.frame;
        auto it = cache_.find(r.frame);
        QImage img = it != cache_.end() ? it->second : render(r, r.frame);
        if (img.isNull()) return;
        cache_[r.frame] = img;
        emit rendered(img, r.frame);
        trim();
        if (r.direction != 0) QMetaObject::invokeMethod(this, &RenderWorker::prefetch, Qt::QueuedConnection);
    }

    // Renders one missing frame ahead of the playhead, then yields so new
    // requests are always served first.
    void prefetch() {
        {
            QMutexLocker lock(&m_);
            if (has_) return;
        }
        if (ctx_.direction == 0 || !ctx_.project) return;
        const Sequence* s = ctx_.project->findSequence(ctx_.sequence);
        if (!s) return;
        FrameTime end = s->duration();
        for (int k = 1; k <= kAhead; ++k) {
            FrameTime t = ctx_.frame + FrameTime(k) * ctx_.direction;
            if (t < 0 || t >= end) return;
            if (cache_.count(t)) continue;
            QImage img = render(ctx_, t);
            if (img.isNull()) return;
            cache_[t] = img;
            trim();
            QMetaObject::invokeMethod(this, &RenderWorker::prefetch, Qt::QueuedConnection);
            return;
        }
    }

    // Drops cached frames furthest from the playhead.
    void trim() {
        while (cache_.size() > kMaxCached) {
            auto first = cache_.begin(), last = std::prev(cache_.end());
            if (std::llabs(first->first - ctx_.frame) > std::llabs(last->first - ctx_.frame)) cache_.erase(first);
            else cache_.erase(last);
        }
    }

    QMutex m_;
    Request pending_;
    bool has_ = false;
    Request ctx_;  // context of the cached frames (render-thread only)
    std::map<FrameTime, QImage> cache_;
};

// ---------------------------------------------------------------------------
// Pull-mode audio source feeding QAudioSink from the mixer.

class MixerDevice : public QIODevice {
    Q_OBJECT
public:
    explicit MixerDevice(QObject* parent) : QIODevice(parent) { mixer_.setNonBlocking(true); }

    void configure(std::shared_ptr<const Project> p, Id seq, int64_t startSample, bool int16) {
        QMutexLocker lock(&m_);
        const Sequence* s = p ? p->findSequence(seq) : nullptr;
        project_ = std::move(p);
        seq_ = seq;
        sample_ = startSample;
        int16_ = int16;
        mixer_.reset();
        // Loudness keeps integrating across plays of the same sequence until reset.
        QMutexLocker meterLock(&meterM_);
        const int rate = s ? s->sampleRate : 48000;
        if (!meter_ || seq != meterSeq_ || rate != meterRate_) {
            meter_ = std::make_unique<LoudnessMeter>(rate);
            meterSeq_ = seq;
            meterRate_ = rate;
        }
    }
    void resetLoudness() {
        QMutexLocker lock(&meterM_);
        if (meter_) meter_->reset();
    }
    void setProject(std::shared_ptr<const Project> p) {
        QMutexLocker lock(&m_);
        project_ = std::move(p);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 16) + QIODevice::bytesAvailable(); }

signals:
    void levels(float l, float r, const QVector<float>& tracks);
    void loudness(double momentary, double shortTerm, double integrated, double range, double truePeak);

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
        // Loudness of what is heard, reported ten times a second.
        {
            QMutexLocker lock(&meterM_);
            if (meter_) {
                meter_->add(buf_.data(), frames);
                sinceReport_ += frames;
                if (sinceReport_ >= meterRate_ / 10) {
                    sinceReport_ = 0;
                    const LoudnessResult r = meter_->result();
                    emit loudness(meter_->momentary(), meter_->shortTerm(), r.valid ? r.integrated : -200.0, meter_->loudnessRange(),
                                  r.truePeakDb);
                }
            }
        }
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
    QMutex meterM_;
    std::unique_ptr<LoudnessMeter> meter_;
    Id meterSeq_ = 0;
    int meterRate_ = 48000;
    int64_t sinceReport_ = 0;
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
    connect(device_, &MixerDevice::loudness, this, &PlaybackController::loudness);
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

void PlaybackController::setUseProxies(bool on) {
    useProxies_ = on;
    requestFrame();
}

void PlaybackController::setShowCaptions(bool on) {
    showCaptions_ = on;
    requestFrame();
}

int PlaybackController::playStep() const {
    if (speed_ == 0) return 0;
    int step = int(std::lround(speed_));
    return step == 0 ? (speed_ > 0 ? 1 : -1) : step;
}

FrameTime PlaybackController::clampToSequence(FrameTime t) const { return std::max<FrameTime>(0, t); }

void PlaybackController::requestFrame() {
    if (!sequence()) return;
    // Paused frames render at full quality; playback uses the preview scale.
    worker_->request({project_, sequenceId_, position_, isPlaying() ? scale_ : 1.0, useProxies_, playStep(), showCaptions_});
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
    if (!playing) scrubAudio(t);
    emit positionChanged(position_);
}

void PlaybackController::scrubAudio(FrameTime t) {
    const Sequence* s = sequence();
    if (!scrubbing_ || !s || s->audioTracks.empty()) return;
    if (!scrubSink_) {
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull()) {
            scrubbing_ = false;  // no output device: don't try again
            return;
        }
        QAudioFormat fmt;
        fmt.setSampleRate(s->sampleRate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) {
            scrubbing_ = false;
            return;
        }
        scrubSink_ = new QAudioSink(dev, fmt, this);
        scrubSink_->setBufferSize(fmt.bytesForDuration(150000));
        scrubIo_ = scrubSink_->start();
        scrubRate_ = s->sampleRate;
        if (!scrubIo_) return;
    }
    if (!scrubIo_ || scrubRate_ != s->sampleRate) return;
    // One frame of audio (at least 40 ms), faded at both ends to avoid clicks.
    int frames = std::max(int(s->sampleRate / s->fpsValue()), s->sampleRate / 25);
    if (scrubSink_->bytesFree() < frames * 4) return;  // still playing the previous snippet
    std::vector<float> mix(size_t(frames) * 2);
    int64_t start = int64_t(std::llround(double(t) * s->sampleRate / s->fpsValue()));
    scrubMixer_.reset();
    scrubMixer_.setNonBlocking(true);
    scrubMixer_.mix(*project_, *s, start, frames, mix.data());
    std::vector<int16_t> pcm(mix.size());
    const int fade = std::min(frames / 4, 240);
    for (int i = 0; i < frames; ++i) {
        float g = 1.0f;
        if (i < fade) g = float(i) / fade;
        if (i >= frames - fade) g = float(frames - 1 - i) / fade;
        for (int c = 0; c < 2; ++c)
            pcm[size_t(i) * 2 + size_t(c)] = int16_t(std::lround(std::clamp(mix[size_t(i) * 2 + size_t(c)] * g, -1.0f, 1.0f) * 32767));
    }
    scrubIo_->write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * sizeof(int16_t)));
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
    // While audio plays, follow what the device has actually played (processed
    // minus what is still buffered) so picture stays locked to sound. Backends
    // differ in how they report this, so only trust it when it is close to
    // the wall clock.
    if (speed_ == 1 && sink_ && sink_->state() == QAudio::ActiveState) {
        qint64 buffered = std::max<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
        double played = (sink_->processedUSecs() - sink_->format().durationForBytes(buffered)) / 1e6;
        if (played > 0 && std::fabs(played - elapsed) < 0.25) elapsed = played;
    }
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
        worker_->request({project_, sequenceId_, t, scale_, useProxies_, playStep(), showCaptions_});
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

void PlaybackController::resetLoudness() { device_->resetLoudness(); }

}  // namespace montage

#include "PlaybackController.moc"
