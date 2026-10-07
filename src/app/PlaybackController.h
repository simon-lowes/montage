// Montage — real-time playback of a sequence: background frame rendering,
// audio output (the master clock), J/K/L shuttle and scrubbing.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <memory>

#include "core/Model.h"

class QAudioSink;
class QThread;

namespace montage {

class RenderWorker;
class MixerDevice;

class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    // Replaces the project snapshot used for rendering / mixing and the
    // sequence to play (by id). Cheap enough to call on every edit.
    void setProject(const Project& p, Id sequenceId);
    std::shared_ptr<const Project> snapshot() const { return project_; }
    const Sequence* sequence() const;

    // Preview resolution as a fraction of the sequence size (1, 1/2, 1/4...).
    void setPreviewScale(double s);
    double previewScale() const { return scale_; }

    bool isPlaying() const { return speed_ != 0; }
    double speed() const { return speed_; }
    FrameTime position() const { return position_; }

    // Range playback (loops between in/out when enabled).
    void setLoop(bool on) { loop_ = on; }
    bool loop() const { return loop_; }

public slots:
    void play();                  // forward at 1x
    void pause();
    void togglePlay();
    void shuttle(int direction);  // J (-1) / L (+1): repeated presses speed up; 0 = K (stop)
    void seek(montage::FrameTime t);  // also renders the frame when paused
    void step(int frames);
    void requestFrame();          // re-render the current frame (after edits)

signals:
    void positionChanged(montage::FrameTime t);
    void frameRendered(const QImage& image, montage::FrameTime t);
    // Peak levels of the last audio block: master L/R and per-track
    // interleaved L,R pairs (linear, 0..1+).
    void audioLevels(float masterL, float masterR, const QVector<float>& trackPeaks);
    void playingChanged(bool playing);

private slots:
    void tick();

private:
    void startAudio(FrameTime from);
    void stopAudio();
    FrameTime clampToSequence(FrameTime t) const;

    std::shared_ptr<const Project> project_;
    Id sequenceId_ = 0;
    double scale_ = 0.5;
    double speed_ = 0;
    bool loop_ = false;
    FrameTime position_ = 0;
    FrameTime startFrame_ = 0;
    QElapsedTimer clock_;
    QTimer timer_;
    QThread* renderThread_ = nullptr;
    RenderWorker* worker_ = nullptr;
    QAudioSink* sink_ = nullptr;
    MixerDevice* device_ = nullptr;
    bool audioClock_ = false;
};

}  // namespace montage
