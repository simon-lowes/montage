#include "SoundOut.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <algorithm>

namespace montage {

SoundOut::SoundOut(QObject* parent) : QObject(parent) {}

SoundOut::~SoundOut() { close(); }

bool SoundOut::open(int rate, int channels, double seconds) {
    close();
    rate_ = rate > 0 ? rate : 48000;
    channels_ = std::clamp(channels, 1, 64);
    written_ = 0;
    if (audiodev::Engine::shared().active()) {
        auto feed = std::make_unique<audiodev::Feed>();
        if (feed->open(rate_, channels_, seconds)) {
            feed_ = std::move(feed);
            return true;
        }
        // (the device is chosen but not open now: Qt Multimedia's output, so the sound is still heard)
    }
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    QAudioFormat fmt;
    fmt.setSampleRate(rate_);
    fmt.setChannelCount(channels_);
    fmt.setSampleFormat(QAudioFormat::Float);
    if (dev.isNull() || !dev.isFormatSupported(fmt)) return false;
    sink_ = new QAudioSink(dev, fmt, this);
    sink_->setBufferSize(fmt.bytesForDuration(qint64(std::clamp(seconds, 0.02, 10.0) * 1e6)));
    io_ = sink_->start();
    if (!io_) {
        close();
        return false;
    }
    return true;
}

void SoundOut::close() {
    feed_.reset();
    if (sink_) {
        sink_->stop();
        delete sink_;
    }
    sink_ = nullptr;
    io_ = nullptr;
    written_ = 0;
}

bool SoundOut::isOpen() const { return feed_ || io_; }

qint64 SoundOut::room() const {
    if (feed_) return feed_->room();
    if (sink_ && io_) return sink_->bytesFree() / qint64(sizeof(float) * size_t(channels_));
    return 0;
}

qint64 SoundOut::write(const float* interleaved, qint64 frames) {
    if (frames <= 0) return 0;
    if (feed_) return feed_->push(interleaved, int(std::min<qint64>(frames, 1 << 30)));
    if (!io_) return 0;
    const qint64 n = std::min(frames, room());
    if (n <= 0) return 0;
    const qint64 bytes = io_->write(reinterpret_cast<const char*>(interleaved), n * qint64(sizeof(float)) * channels_);
    const qint64 took = std::max<qint64>(0, bytes) / (qint64(sizeof(float)) * channels_);
    written_ += took;
    return took;
}

qint64 SoundOut::queued() const {
    if (feed_) return feed_->queued();
    if (!sink_) return 0;
    const qint64 frameBytes = qint64(sizeof(float)) * channels_;
    return std::max<qint64>(0, (sink_->bufferSize() - sink_->bytesFree()) / frameBytes);
}

qint64 SoundOut::played() const {
    if (feed_) return feed_->played();
    return std::max<qint64>(0, written_ - queued());
}

void SoundOut::clear() {
    if (feed_) feed_->clear();
    else if (sink_ && io_) {
        // (Qt's buffer cannot be emptied in place: the sink is started again)
        sink_->stop();
        io_ = sink_->start();
        written_ = 0;
    }
}

}  // namespace montage
