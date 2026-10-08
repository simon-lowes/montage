// Montage — audio mixer: one channel strip per audio track (insert effects,
// output routing, pan, fader, meter, mute / solo), a strip per bus, and the
// master (effects, fader, meter).
#pragma once

#include <QVector>
#include <QWidget>
#include <vector>

#include "core/Automation.h"
#include "core/Model.h"

class QDial;
class QHBoxLayout;
class QComboBox;
class QLabel;
class QScrollArea;
class QSlider;
class QToolButton;

namespace montage {

class AudioMeterWidget;
class EditorState;
class SurroundPanner;

class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(EditorState* state, QWidget* parent = nullptr);

    // Fader automation (core/Automation.h). Playback starting and stopping, and each frame it plays: tracks in
    // Write, Latch or Touch record their fader and pan, kept as one undo step when playback stops.
    void playbackStarted(FrameTime t);
    void playbackPosition(FrameTime t);
    void playbackStopped(FrameTime t);
    bool recordingAutomation() const { return !recording_.empty(); }
    QSlider* trackFader(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].fader : nullptr; }
    QDial* trackPan(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].pan : nullptr; }
    QComboBox* trackAutomationMode(int index) const {
        return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].automation : nullptr;
    }

signals:
    // The user asked to see a track's, bus's or the master's effects (now in the Inspector).
    void effectsRequested();

public slots:
    // Master L/R and per-audio-track interleaved L,R linear peaks
    // (as emitted by PlaybackController::audioLevels).
    void setLevels(float masterL, float masterR, const QVector<float>& trackPeaks);
    // Clears every meter (e.g. when playback stops).
    void resetMeters();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Strip {
        QWidget* box = nullptr;
        QLabel* name = nullptr;
        QDial* pan = nullptr;
        QLabel* panLabel = nullptr;
        SurroundPanner* surround = nullptr;  // instead of the pan, in 5.1 and 7.1 sequences
        QSlider* fader = nullptr;
        QLabel* dbLabel = nullptr;
        AudioMeterWidget* meter = nullptr;
        QToolButton* mute = nullptr;
        QToolButton* solo = nullptr;
        QToolButton* fx = nullptr;     // shows the inserts in the Inspector
        QComboBox* output = nullptr;   // master or a bus
        QComboBox* automation = nullptr;  // AutomationMode
    };
    struct BusStrip {
        QWidget* box = nullptr;
        QLabel* name = nullptr;
        QToolButton* fx = nullptr;
        QSlider* fader = nullptr;
        QLabel* dbLabel = nullptr;
        QToolButton* mute = nullptr;
        SurroundPanner* surround = nullptr;
    };

    void syncToProject();  // rebuild if the audio track count changed, else refresh
    void rebuild();
    void refresh();
    Strip makeStrip(int index);
    BusStrip makeBusStrip(Id bus);
    QWidget* makeMasterStrip();
    void inspect(Id owner);
    void addBus();

    void setVolume(int index, double db);
    void setPan(int index, double pan);
    void setSurround(int index, const SurroundPan& pan, bool final);
    void setMute(int index, bool on);
    void setSolo(int index, bool on);
    void setAutomationMode(int index, int mode);
    // Faders and pans show the automation at the playhead on tracks that read it.
    void followAutomation();

    EditorState* state_;
    QScrollArea* scroll_ = nullptr;
    QWidget* stripHost_ = nullptr;
    QHBoxLayout* stripLayout_ = nullptr;
    std::vector<Strip> strips_;
    std::vector<BusStrip> busStrips_;
    QToolButton* addBus_ = nullptr;
    AudioMeterWidget* masterMeter_ = nullptr;
    QToolButton* masterFx_ = nullptr;
    QSlider* masterFader_ = nullptr;
    QLabel* masterDb_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    struct Recording {
        int track = 0;
        AutomationMode mode = AutomationMode::Read;
        AutomationRecorder volume, pan;
        double volumeDb = 0, panValue = 0;  // the fader and pan before
        Param volumeLane, panLane;          // the lanes before
    };
    std::vector<Recording> recording_;
    QString liveState_;  // what the gesture last applied, to skip repeats
};

}  // namespace montage
