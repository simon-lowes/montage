// Montage — audio mixer: one channel strip per audio track (pan, fader,
// meter, mute / solo) plus a master meter.
#pragma once

#include <QVector>
#include <QWidget>
#include <vector>

class QDial;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QSlider;
class QToolButton;

namespace montage {

class AudioMeterWidget;
class EditorState;

class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(EditorState* state, QWidget* parent = nullptr);

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
        QSlider* fader = nullptr;
        QLabel* dbLabel = nullptr;
        AudioMeterWidget* meter = nullptr;
        QToolButton* mute = nullptr;
        QToolButton* solo = nullptr;
    };

    void syncToProject();  // rebuild if the audio track count changed, else refresh
    void rebuild();
    void refresh();
    Strip makeStrip(int index);
    QWidget* makeMasterStrip();

    void setVolume(int index, double db);
    void setPan(int index, double pan);
    void setMute(int index, bool on);
    void setSolo(int index, bool on);

    EditorState* state_;
    QScrollArea* scroll_ = nullptr;
    QWidget* stripHost_ = nullptr;
    QHBoxLayout* stripLayout_ = nullptr;
    std::vector<Strip> strips_;
    AudioMeterWidget* masterMeter_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
};

}  // namespace montage
