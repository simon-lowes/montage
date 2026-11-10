// Montage — sound played that is not the timeline's playback: scrubbing, the sound library's preview, a remote
// review's sound. Through the device chosen in Monitoring › Audio device (audio/AudioDevice.h), so a JACK, ALSA, Core
// Audio or WASAPI user hears it on the same ports as playback; through Qt Multimedia's default output when none is
// (or when the device cannot take it).
#pragma once

#include <QObject>
#include <memory>

#include "audio/AudioDevice.h"

class QAudioSink;
class QIODevice;

namespace montage {

class SoundOut : public QObject {
    Q_OBJECT
public:
    explicit SoundOut(QObject* parent = nullptr);
    ~SoundOut() override;

    // Float sound of `channels` at `rate`, with room queued for `seconds`. False when there is nothing to play on.
    bool open(int rate, int channels, double seconds);
    void close();
    bool isOpen() const;
    bool onDevice() const { return feed_ != nullptr; }  // through the device layer (not Qt Multimedia)
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    qint64 room() const;  // frames that can be written now
    // As much as there is room for; returns the frames taken.
    qint64 write(const float* interleaved, qint64 frames);
    // Frames written and not yet played (Qt Multimedia: as its buffer says).
    qint64 queued() const;
    qint64 played() const;  // frames played since open (the device layer's own count; Qt's: written less queued)
    void clear();           // what is queued let go

private:
    std::unique_ptr<audiodev::Feed> feed_;
    QAudioSink* sink_ = nullptr;
    QIODevice* io_ = nullptr;
    int rate_ = 0, channels_ = 0;
    qint64 written_ = 0;
};

}  // namespace montage
