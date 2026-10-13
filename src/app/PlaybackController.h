// Montage — real-time playback of a sequence: background frame rendering,
// audio output (the master clock), J/K/L shuttle and scrubbing.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/Adr.h"
#include "core/Model.h"
#include "render/Compositor.h"
#include "render/HdrView.h"

class QAudioSink;
class QIODevice;
class QThread;

namespace montage {

class RenderWorker;
class MixerDevice;
namespace output {
class Scheduler;
}
struct OutputAudio;

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
    // The trim pass shown (render/TrimPasses.h: one of the sequence's targets), "" = the master.
    const std::string& trimTarget() const { return trimTarget_; }
    void setTrimTarget(const std::string& id);
    // Also render HDR sequences as light for the HDR viewer (hdrFrameRendered), beside their SDR pictures.
    void setHdrOutput(bool on);
    bool hdrOutput() const { return hdrOutput_; }
    // Draw the program frames on the GPU where it can (render/GpuCompositor.h; the rest on the CPU as before).
    void setGpuRendering(bool on);
    bool gpuRendering() const { return gpu_; }

    // Reference output (render/ReferenceOutput.h): every frame rendered also goes to `out` (in the sequence's own
    // colour space, at its render scale, the render cache bypassed), with the playhead and the sound heard; null: none.
    void setReferenceOutput(std::shared_ptr<output::Scheduler> out);
    std::shared_ptr<output::Scheduler> referenceOutput() const { return output_; }

    bool isPlaying() const { return speed_ != 0; }
    // Global Mute (as in Premiere): playback and scrubbing are silent; clips, tracks and exports are untouched.
    void setGlobalMute(bool on);
    bool globalMute() const { return globalMute_; }
    // How an ambisonic sequence is heard (playing and scrubbing): binaurally for headphones, or as stereo for speakers.
    // An ambisonic clip in a stereo or surround sequence is decoded as its Ambisonics effect says, as it is exported.
    void setAmbisonicBinaural(bool on);
    bool ambisonicBinaural() const { return ambisonicBinaural_; }
    // Audio tracks left out of what is heard (an ADR guide track while recording), the project untouched.
    void setMutedAudioTracks(const std::vector<int>& tracks);
    const std::vector<int>& mutedAudioTracks() const { return mutedTracks_; }
    // What an automation pass plays while it records (the mixer's controls being written), laid over the sequence
    // played's sound without changing the project; empty = none.
    void setLiveMix(const std::function<void(Sequence&)>& apply);
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
    void setSpeed(double speed);  // a shuttle ring's: any speed either way (0 pauses; sound at 1x only)
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
    // The same frame for the HDR viewer (with setHdrOutput): null when the sequence is not HDR.
    void hdrFrameRendered(montage::HdrPicturePtr picture, montage::FrameTime t);
    // Peak levels of the last audio block: master L/R and per-track
    // interleaved L,R pairs (linear, 0..1+).
    void audioLevels(float masterL, float masterR, const QVector<float>& trackPeaks);
    // Loudness of what is heard (LUFS, LU and dBTP; -200 for nothing yet), ten times a second.
    void loudness(double momentary, double shortTerm, double integrated, double range, double truePeak);
    // The same measured over speech only (render/DialogueLoudness.h, as the sequence marks it: whole Dialogue clips,
    // pauses and all, else transcribed words; -200 for none yet) and the share of the sound that is speech (-1: the
    // sequence marks none). An approximation: exports and Quality Check narrow Dialogue clips to where they sound.
    void dialogueLoudness(double integrated, double speechShare);
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
    void syncOutput();  // the playhead and what is heard, to the reference output
    double renderScale() const;

    std::shared_ptr<const Project> project_;
    std::shared_ptr<const Project> heard_;  // project_, or a copy with mutedTracks_ muted and liveMix_ laid over
    std::vector<int> mutedTracks_;
    std::function<void(Sequence&)> liveMix_;
    std::optional<AdrCycle> cycle_;
    Id sequenceId_ = 0;
    double scale_ = 0.5;
    bool useProxies_ = false;
    bool showCaptions_ = false;
    StereoView stereoView_ = StereoView::Left;
    std::string trimTarget_;
    bool hdrOutput_ = false;
    bool gpu_ = false;
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
    bool ambisonicBinaural_ = true;
    std::shared_ptr<output::Scheduler> output_;
    std::shared_ptr<OutputAudio> outputAudio_;
};

}  // namespace montage
