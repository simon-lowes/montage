// Montage — the Sampler panel (Sequence › Sampler): a sound-effect sampler on an audio track (audio/Sampler.h),
// played to picture from MIDI keys or pads, or the computer's keys, and recorded in passes onto the track's take lanes.
// Its table lists the slots by key, with their sounds, gain, attack, release, one-shot, choke group and variation;
// sounds are added by Add Sounds…, by dropping files or sound library selections on a row, or by Split by Onsets, which
// cuts a recording of hits onto successive keys. While the panel is shown the sampler is heard over the programme,
// playing or stopped. Record plays the sequence and keeps the notes; when it stops the pass is rendered to a file beside
// the project and put on a new take lane, the comp taking it. Voice Again renders a pass's notes through the slots as
// they are now.
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
    QPushButton* recordButton_;
    QCheckBox* clipPerHitBox_;
    QComboBox* passBox_;
    QLabel* status_;
};

}  // namespace montage
