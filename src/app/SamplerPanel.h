// Montage — the Sampler panel (Sequence › Sampler): a sound-effect sampler on an audio track (audio/Sampler.h),
// played to picture from MIDI keys or pads, or the computer's keys, and recorded in passes onto the track's take lanes.
// Its table lists the slots by key, with their sounds, gain, attack, release, one-shot, choke group and variation;
// sounds are added by Add Sounds…, by dropping files or sound library selections on a row, or by Split by Onsets, which
// cuts a recording of hits onto successive keys. While the panel is shown the sampler is heard over the programme,
// playing or stopped. Record plays the sequence and keeps the notes; when it stops the pass is rendered to a file beside
// the project and put on a new take lane, the comp taking it. Voice Again renders a pass's notes through the slots as
// they are now. Under the table the selected slot's velocity curve is shown, and drawn with the mouse. A slot sent to
// another track (Output) is heard through that track's strip as it is played, as its part of a pass will be.
#pragma once

#include <QPointer>
#include <QWidget>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/Sampler.h"
#include "control/Midi.h"
#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace montage {

class EditorState;
class PlaybackController;

// A slot's velocity curve, drawn: the force a key is played with across (1-127), what comes out of it up (1-127),
// straight between its points. Pressing where there is no point makes one, a drag moves a point, a double click takes
// one away (a double click where there was none puts one down, as a click does: one change, not a point made and taken
// away again). Drawing over a curve that is not drawn (As Played, Soft, Hard) starts from that curve's two ends.
class VelocityCurveView : public QWidget {
    Q_OBJECT
public:
    explicit VelocityCurveView(QWidget* parent = nullptr);
    void setSlot(const SamplerSlot* slot);  // null: nothing shown, nothing drawn
    // (tall and wide enough for a place for every velocity each way: a point lands on the velocities it is put at)
    QSize sizeHint() const override { return {280, 142}; }
    QSize minimumSizeHint() const override { return {160, 142}; }
    // Where a pair of velocities is in the widget, and the pair at a place (clamped to 1-127).
    QPoint pointFor(int in, int out) const;
    std::pair<int, int> velocitiesAt(const QPoint& p) const;
    const std::vector<std::pair<int, int>>& points() const { return points_; }

signals:
    void drawn(const std::vector<std::pair<int, int>>& points);  // (when the mouse is let go: one change)

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRect plot() const;
    int pointNear(const QPoint& p) const;
    SamplerSlot slot_;
    bool has_ = false;
    std::vector<std::pair<int, int>> points_;  // (in order of the velocity in)
    int drag_ = -1;
    std::pair<int, int> made_{-1, -1};  // (the point the last press that made one made, and when)
    quint64 madeAt_ = 0;
};

class SamplerPanel : public QWidget {
    Q_OBJECT
public:
    SamplerPanel(EditorState* state, PlaybackController* program, QWidget* parent = nullptr);
    ~SamplerPanel() override;

    int track() const { return track_; }  // the audio track's index
    void setTrack(int index);
    // A key down or up (the computer's keys, MIDI, tests), pressed `nsAgo` nanoseconds ago.
    void noteOn(int key, int velocity, qint64 nsAgo = 0);
    void noteOff(int key, qint64 nsAgo = 0);
    // Recording a pass: the sequence plays from where it is; the pass is made when recording stops or playback does.
    void setRecording(bool on);
    bool isRecording() const { return recording_; }
    // A MIDI input by its port's name ("" none).
    bool connectMidi(const QString& input, QString* error = nullptr);
    void setComputerKeys(bool on);
    bool computerKeys() const { return computerKeys_; }
    // The key a computer key plays (two rows, from C3 on Z and C4 on Q), or -1.
    static int keyForComputerKey(int qtKey);
    std::shared_ptr<sampler::Engine> engine() const { return engine_; }
    QString passFolder() const;
    bool addSounds(int key, const QStringList& files, double start = 0, double end = 0);
    bool splitByOnsets(const QString& file, int firstKey);
    bool revoice(Id lane);
    // Whether the sampler is heard (while the panel is shown and has sounds).
    void setActive(bool on);
    bool isActive() const { return active_; }

signals:
    void recordingChanged(bool on);
    void passRecorded(montage::Id lane);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    struct Pressed {
        int key = 0, velocity = 0;
        bool on = true;
        qint64 at = 0;  // steady clock, ns
        uint32_t index = 0;
    };
    void projectChanged();
    void updateActive();
    void reloadKit();
    void refreshTable();
    void refreshCurve();  // the curve drawn for the slot selected
    void refreshPasses();
    void cellEdited(int row, int column);
    void finishPass();
    void drainMidi();
    void received(const MidiMessage& m);
    const Track* audioTrack() const;
    int selectedKey() const;
    void setSlot(const SamplerSlot& slot, const QString& label);
    void changeSlot(int key, const std::function<void(SamplerSlot&)>& change);  // the slot on that key, changed (one undo step)
    uint32_t seedFor(uint32_t index) const;

    EditorState* state_;
    QPointer<PlaybackController> program_;
    int track_ = 0;
    std::shared_ptr<sampler::Engine> engine_;
    sampler::KitPtr kit_;
    std::vector<SamplerSlot> loaded_;  // the slots kit_ was loaded from
    int loadedRate_ = 0;
    std::atomic<bool> active_{false};
    bool recording_ = false, startedPlay_ = false, computerKeys_ = false;
    std::atomic<uint32_t> passSeed_{1};
    std::atomic<uint32_t> noteIndex_{0};
    FrameTime passFrom_ = 0;
    std::vector<std::pair<uint32_t, SamplerNote>> notes_;  // (index, note)
    std::map<int, size_t> open_;                           // key -> note held
    std::unique_ptr<MidiConnection> midi_;
    std::mutex inboxMutex_;
    std::vector<Pressed> inbox_;
    bool drainPending_ = false;
    bool refreshing_ = false;

    QComboBox* trackBox_;
    QComboBox* midiBox_;
    QCheckBox* keysBox_;
    QSpinBox* velocityBox_;
    QTableWidget* table_;
    VelocityCurveView* curveView_;
    QPushButton* recordButton_;
    QCheckBox* clipPerHitBox_;
    QComboBox* passBox_;
    QLabel* status_;
};

}  // namespace montage
