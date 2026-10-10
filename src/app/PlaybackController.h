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
#include <optional>
#include <vector>

#include "core/Adr.h"
#include "core/Model.h"
#include "render/Compositor.h"

class QAudioSink;
class QIODevice;
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
    // How this monitor renders frames (so Render In to Out makes frames it will use).
    RenderOptions renderOptions() const;
    // Decode proxies instead of the original media where available.
    void setUseProxies(bool on);
    bool useProxies() const { return useProxies_; }
    // Draw the sequence's visible caption track (program monitor only).
    void setShowCaptions(bool on);
    bool showCaptions() const { return showCaptions_; }
    // How a stereo 3D sequence is shown: one eye, anaglyph, the eyes packed side by side or top and bottom, or their
    // difference (render/Stereo.h). Other sequences are not affected.
    void setStereoView(StereoView v);
    StereoView stereoView() const { return stereoView_; }

    bool isPlaying() const { return speed_ != 0; }
    // Global Mute (as in Premiere): playback and scrubbing are silent; clips, tracks and exports are untouched.
    void setGlobalMute(bool on);
    bool globalMute() const { return globalMute_; }
    // Audio tracks left out of what is heard (an ADR guide track while recording), the project untouched.
    void setMutedAudioTracks(const std::vector<int>& tracks);
    const std::vector<int>& mutedAudioTracks() const { return mutedTracks_; }
    // An ADR cycle (core/Adr.h): its beeps are heard while it is set; the Program monitor draws its streamer.
    void setAdrCycle(const std::optional<AdrCycle>& cycle);
    const std::optional<AdrCycle>& adrCycle() const { return cycle_; }
    // What the speakers get for `frames` samples of the sequence from sample `start` (silence under Global Mute).
    std::vector<float> heard(int64_t start, int frames);
    double speed() const { return speed_; }
    FrameTime position() const { return position_; }

    // Range playback (loops between in/out when enabled).
    void setLoop(bool on) { loop_ = on; }
    bool loop() const { return loop_; }
    // Play a short audio snippet when seeking while paused (scrubbing).
    void setAudioScrubbing(bool on) { scrubbing_ = on; }

public slots:
    void play();                  // forward at 1x
    void pause();
    void togglePlay();
    void shuttle(int direction);  // J (-1) / L (+1): repeated presses speed up; 0 = K (stop)
    void seek(montage::FrameTime t);  // also renders the frame when paused
    void step(int frames);
    void requestFrame();          // re-render the current frame (after edits)
    // Starts the loudness measurement again (it otherwise runs on across plays of a sequence).
    void resetLoudness();

signals:
    void positionChanged(montage::FrameTime t);
    void frameRendered(const QImage& image, montage::FrameTime t);
    // The same frame for the scopes: as delivered (code values in `space`, 16-bit for sequences outside Rec.709,
    // whose frameRendered pictures are their SDR previews), with the sequence's mastering peak.
    void scopeFrameRendered(const QImage& image, montage::FrameTime t, const QString& space, double peakNits);
    // Peak levels of the last audio block: master L/R and per-track
    // interleaved L,R pairs (linear, 0..1+).
    void audioLevels(float masterL, float masterR, const QVector<float>& trackPeaks);
    // Loudness of what is heard (LUFS, LU and dBTP; -200 for nothing yet), ten times a second.
    void loudness(double momentary, double shortTerm, double integrated, double range, double truePeak);
    void playingChanged(bool playing);

private slots:
    void tick();

private:
    void startAudio(FrameTime from);
    void stopAudio();
    FrameTime clampToSequence(FrameTime t) const;
    int playStep() const;
    void scrubAudio(FrameTime t);  // frames per tick direction for render-ahead (0 = paused)
    void updateHeard();

    std::shared_ptr<const Project> project_;
    std::shared_ptr<const Project> heard_;  // project_, or a copy with mutedTracks_ muted
    std::vector<int> mutedTracks_;
    std::optional<AdrCycle> cycle_;
    Id sequenceId_ = 0;
    double scale_ = 0.5;
    bool useProxies_ = false;
    bool showCaptions_ = false;
    StereoView stereoView_ = StereoView::Left;
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
    bool scrubbing_ = true;
    QAudioSink* scrubSink_ = nullptr;
    QIODevice* scrubIo_ = nullptr;
    int scrubRate_ = 0;
    AudioMixer scrubMixer_;
    bool globalMute_ = false;
};

}  // namespace montage
