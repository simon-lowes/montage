// Montage — the ADR panel: the sequence's cue list and the recording of its lines to picture (core/Adr.h). Pick a cue:
// Rehearse plays its cycle (pre-roll, three beeps, the streamer and the line on the Program monitor); Record plays it
// again and records the actor, the take joining the cue's audition clip on the ADR track (the newest the pick); Loop
// records take after take until stopped. The guide (the production sound) can be muted while recording, and the
// takes already made are muted too.
#pragma once

#include <QWidget>
#include <optional>

#include "core/Adr.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPainter;
class QProgressBar;
class QPushButton;
class QRectF;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace montage {

class EditorState;
class PlaybackController;
class ViewerWidget;
class VoiceoverRecorder;

class AdrPanel : public QWidget {
    Q_OBJECT
public:
    // `viewer` (the Program monitor's) gets the streamer and the line; it may be null.
    AdrPanel(EditorState* state, PlaybackController* program, ViewerWidget* viewer, QWidget* parent = nullptr);
    ~AdrPanel() override;

    VoiceoverRecorder* recorder() const { return recorder_; }
    // Recording from the audio input (false: sound only through recorder()->feed(), as tests feed it).
    void setUseInputDevice(bool on) { useDevice_ = on; }
    // The pause between takes when looping, in milliseconds.
    void setLoopGap(int ms) { loopGap_ = ms; }

    Id currentCue() const;
    void selectCue(Id id);
    AdrSettings settings() const;

    // The cue list.
    Id addCue();                // In to Out, or two seconds from the playhead
    int addCuesFromCaptions();  // between In and Out when marked
    int addCuesFromMarkers();   // range markers, between In and Out when marked
    bool importCueSheet(const QString& path, QString* error = nullptr);
    bool exportCueSheet(const QString& path, QString* error = nullptr);
    void removeCue();
    void setCueStatus(int status);

    // Cycles of the current cue.
    void rehearse();
    void record();
    void stop();
    bool isRunning() const { return cycle_.has_value(); }
    bool isRecording() const;
    // The current cue's takes: pick one.
    void pickTake(int index);

signals:
    void takeRecorded(montage::Id cue, montage::Id clip);

private:
    void refresh();
    void refreshTakes();
    void updateButtons();
    void startCycle(bool record);
    void endCycle();
    void onPosition(FrameTime t);
    void onTaken(Id media, FrameTime at);
    void cellEdited(int row, int column);
    void paintOverlay(QPainter& p, const QRectF& r);
    std::vector<int> mutedWhileRecording() const;

    EditorState* state_;
    PlaybackController* program_;
    VoiceoverRecorder* recorder_;
    QTableWidget* table_;
    QComboBox* status_;
    QComboBox* takes_;
    QComboBox* input_;
    QComboBox* guide_;
    QComboBox* track_;
    QCheckBox* muteGuide_;
    QCheckBox* loop_;
    QDoubleSpinBox* preRoll_;
    QDoubleSpinBox* postRoll_;
    QSpinBox* beeps_;
    QDoubleSpinBox* streamer_;
    QPushButton* rehearse_;
    QPushButton* record_;
    QPushButton* stop_;
    QProgressBar* meter_;
    QLabel* statusLabel_;
    std::optional<AdrCycle> cycle_;
    Id cycleCue_ = 0;
    QString cycleText_;
    bool recording_ = false;
    bool stopRequested_ = false;
    bool useDevice_ = true;
    bool refreshing_ = false;
    bool graceArmed_ = false;
    int loopGap_ = 1000;
    QTimer* refreshTimer_;
};

}  // namespace montage
