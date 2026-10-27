// Montage — audio mixer: one channel strip per audio track (insert effects,
// output routing, pan, fader, meter, mute / solo), a strip per bus, and the
// master (effects, fader, meter).
#pragma once

#include <QVector>
#include <QWidget>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "EditorState.h"
#include "core/Automation.h"
#include "core/Model.h"

class QAction;
class QDial;
class QHBoxLayout;
class QComboBox;
class QLabel;
class QScrollArea;
class QSlider;
class QToolButton;
class QVBoxLayout;

namespace montage {

class AudioMeterWidget;
class PlaybackController;
class SurroundPanner;

class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(EditorState* state, QWidget* parent = nullptr);

    // Mix automation (core/Automation.h). Playback starting and stopping, and each frame it plays: tracks in Write,
    // Latch or Touch record their fader, pan, mute and send levels, and buses in those modes their fader, pan and mute,
    // kept as one undo step when playback stops. Insert parameters on those tracks and buses moved during the pass (in
    // the Inspector or a plugin's editor) are written too: Touch while moved, Latch and Write from the first move to the
    // stop. While the pass records, what it plays goes out as liveMix() (laid over the sound, the project unchanged), so
    // other edits made meanwhile are kept; at the stop the lanes written are laid over the project as it is then.
    // Tracks and buses in a trim mode write their faders' (and sends') trim lanes instead of the volume lanes, each
    // fader then showing its trim; with "Coalesce Trim After Each Pass" set the trim is folded in at the stop.
    void playbackStarted(FrameTime t);
    void playbackPosition(FrameTime t);
    void playbackStopped(FrameTime t);
    bool recordingAutomation() const { return !recording_.empty() || !lanes_.empty(); }
    // The active sequence as the pass plays it (its live values laid over), or with the values held in Preview; the
    // sequence itself otherwise.
    Sequence heard() const;
    // Aux sends, as a track strip's "+ Send" menu: to an existing bus, or to a new effects return (its id).
    void addSend(int track, Id bus);
    Id newReturn(int track);
    QSlider* sendLevel(int track, int send) const {
        if (track < 0 || track >= int(strips_.size()) || send < 0 || send >= int(strips_[size_t(track)].sends.size())) return nullptr;
        return strips_[size_t(track)].sends[size_t(send)].level;
    }
    QToolButton* addSendButton(int track) const { return track >= 0 && track < int(strips_.size()) ? strips_[size_t(track)].addSend : nullptr; }
    QSlider* busFader(int bus) const { return bus >= 0 && bus < int(busStrips_.size()) ? busStrips_[size_t(bus)].fader : nullptr; }
    QDial* busPan(int bus) const { return bus >= 0 && bus < int(busStrips_.size()) ? busStrips_[size_t(bus)].pan : nullptr; }
    QToolButton* busMute(int bus) const { return bus >= 0 && bus < int(busStrips_.size()) ? busStrips_[size_t(bus)].mute : nullptr; }
    QComboBox* busAutomationMode(int bus) const { return bus >= 0 && bus < int(busStrips_.size()) ? busStrips_[size_t(bus)].automation : nullptr; }
    QSlider* trackFader(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].fader : nullptr; }
    SurroundPanner* trackSurround(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].surround : nullptr; }
    // A track folder's fader (a VCA over its tracks), shown after the track strips; nullptr if no such folder.
    QSlider* folderFader(const QString& folder) const {
        for (const FolderStrip& f : folderStrips_)
            if (f.folder == folder.toStdString()) return f.fader;
        return nullptr;
    }
    QDial* trackPan(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].pan : nullptr; }
    QComboBox* trackAutomationMode(int index) const {
        return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].automation : nullptr;
    }
    QToolButton* trackMute(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].mute : nullptr; }
    QToolButton* trackSolo(int index) const { return index >= 0 && index < int(strips_.size()) ? strips_[size_t(index)].solo : nullptr; }
    QSlider* masterFader() const { return masterFader_; }
    // While set, every fader and pan edit shares this undo step key (a control surface moving several at once makes one
    // step, not one per message); empty, each track's control merges its own moves.
    void setEditMergeKey(const QString& key) { mergeKey_ = key; }
    int trackStrips() const { return int(strips_.size()); }
    // Film-mix automation (the Automation menu, automationMenu()). Preview: faders, pans and send levels move without
    // writing anything (heard as set), and playing writes no pass; punchPreview() writes what they were set to over In
    // to Out (from the playhead to the end without them), glidePreview() glides there from what was at In to the
    // value at Out (a second from the playhead without them). With "Prime Latch While Stopped" a Latch track's control
    // moved while stopped is held the same way and written from the first frame played. capture() snapshots every
    // control at the playhead, pasteCapture() writes it over In to Out (or from the playhead to each lane's next
    // point). writeTo(): during a pass, the value each written lane ends on is written to the target as the pass
    // stops; stopped, the armed tracks' and buses' faders (trims in a trim mode) and pans at the playhead are, or the
    // values held in Preview. copyAutomation() takes In to Out (the whole sequence without them) from the armed tracks
    // and buses, every one when none is armed; pasteAutomation() puts it at the playhead on the armed ones (the same
    // ones when none is). coalesceTrim() folds every trim in. Each is one undo step. (Armed: in a writing mode.)
    QToolButton* automationMenu() const { return automationMenu_; }
    QAction* automationAction(const QString& objectName) const;
    bool previewing() const { return previewing_; }
    void setPreviewing(bool on);
    bool punchPreview();
    bool glidePreview();
    void capture();
    bool pasteCapture();
    bool writeTo(WriteTo target);
    bool copyAutomation();
    bool pasteAutomation(bool relative);
    bool coalesceTrim();
    bool holdingMoves() const { return !held_.empty(); }

    // The monitoring section whose solo mode the solo buttons follow: in place, after or before the fader, they solo
    // in what is heard (PlaybackController::monitoring) and leave the project's solos alone.
    void setMonitor(PlaybackController* monitor);

signals:
    // The user asked to see a track's, bus's or the master's effects (now in the Inspector).
    void effectsRequested();
    // What an automation pass plays over the sequence while it records (PlaybackController::setLiveMix); empty when
    // it ends.
    void liveMix(const std::function<void(montage::Sequence&)>& apply);

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
        struct SendRow {
            QWidget* box = nullptr;
            QLabel* name = nullptr;
            QSlider* level = nullptr;
        };
        QVBoxLayout* sendLayout = nullptr;
        std::vector<SendRow> sends;  // one per send, in the track's order
        QToolButton* addSend = nullptr;
    };
    struct BusStrip {
        QWidget* box = nullptr;
        QLabel* name = nullptr;
        QToolButton* fx = nullptr;
        QSlider* fader = nullptr;
        QLabel* dbLabel = nullptr;
        QToolButton* mute = nullptr;
        SurroundPanner* surround = nullptr;
        QDial* pan = nullptr;             // in stereo sequences
        QComboBox* automation = nullptr;  // AutomationMode
        Id id = 0;
    };

    struct FolderStrip {
        QWidget* box = nullptr;
        QLabel* name = nullptr;
        QSlider* fader = nullptr;
        QLabel* dbLabel = nullptr;
        std::string folder;
    };
    FolderStrip makeFolderStrip(const std::string& folder);
    std::vector<std::string> audioFolders() const;  // in track order

    void syncToProject();  // rebuild if the audio track count changed, else refresh
    void rebuild();
    void refresh();
    Strip makeStrip(int index);
    Strip::SendRow makeSendRow(Strip& strip, int track, int send);
    BusStrip makeBusStrip(Id bus);
    QWidget* makeMasterStrip();
    void inspect(Id owner);
    void addBus();
    void makeAutomationMenu(QWidget* parent);

    void setVolume(int index, double db);
    void setPan(int index, double pan);
    void setSurround(int index, const SurroundPan& pan, bool final);
    void animateSurround(int index, bool on);  // key the position at the playhead, or stop it moving
    void setMute(int index, bool on);
    void setSolo(int index, bool on);
    void setAutomationMode(int index, int mode);
    void setSendLevel(int track, int send, double db);
    void editSend(int track, int send, const QString& label, const std::function<bool(Send&)>& fn);
    void setBusVolume(Id bus, double db);
    void setBusPan(Id bus, double pan);
    void setBusMute(Id bus, bool on);
    bool recordingLane(const QString& id) const;
    // Ends the automation pass at `t`, its lanes written as one undo step; `final` (playback stopped) also hands Write
    // over to Touch.
    void finishPass(FrameTime t, bool final);
    void clearPass();
    void discardPass();  // ends the pass, writing nothing
    std::function<void(Sequence&)> liveOverlay() const;  // what the pass plays now
    void restoreControls();  // the pass's values back on its controls after a refresh
    void rebaseRecorders();  // the recorders onto the lanes and levels the project holds now (edited mid-pass)
    bool passWrote() const;
    void finishWhenFree();   // finishPass at stopAt_ once no other gesture (a drag) is under way
    // An insert parameter's value as the pass plays it, if the pass is writing it (EditorState::liveParameter).
    std::optional<double> liveInsert(Id owner, Id effect, const std::string& param) const;
    // An insert parameter moved during a pass (EditorState::captureParameter): true if this pass writes it.
    bool captureInsert(const EditorState::ParameterMove& move);
    // Faders and pans show the automation at the playhead on tracks that read it.
    void followAutomation();

    EditorState* state_;
    PlaybackController* monitor_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* stripHost_ = nullptr;
    QHBoxLayout* stripLayout_ = nullptr;
    std::vector<Strip> strips_;
    std::vector<BusStrip> busStrips_;
    std::vector<FolderStrip> folderStrips_;
    QToolButton* addBus_ = nullptr;
    AudioMeterWidget* masterMeter_ = nullptr;
    QToolButton* masterFx_ = nullptr;
    QSlider* masterFader_ = nullptr;
    QString mergeKey_;
    QLabel* masterDb_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    struct Recording {
        int track = 0;
        AutomationMode mode = AutomationMode::Read;  // (its fader writes the trim lane in a trim mode)
        AutomationRecorder volume, pan;
        double volumeDb = 0, panValue = 0;  // the fader and pan before
        Param volumeLane, panLane;          // the lanes before
        // In surround (a panner among the speakers): its x, y and z, recorded as the pan is.
        bool surround = false;
        AutomationRecorder x, y, z;
        SurroundPan position;               // the position before (its width, LFE and object as changed meanwhile)
        Param xLane, yLane, zLane;          // its lanes before
        bool changedOther = false;          // width, LFE or object changed during the pass
        Id trackId = 0;                     // (an edit that moves the track leaves its lanes as they were)
        double shownDb = 0, shownPan = 0;   // what the fader and pan show
        SurroundPan shownPosition{};
    };
    std::vector<Recording> recording_;
    // The other lanes written in a pass (track mutes and send levels, bus faders, pans and mutes), each read from its
    // control and put back when the pass ends.
    struct LaneRecording {
        QString id;  // "track-mute-<index>", "send-<track>-<send>", "bus-volume-<id>"...
        AutomationRecorder rec;
        double tolerance = 0.1;
        bool hold = false;                                      // a switch (mute): its keys hold
        std::function<double()> value;                          // the control now
        std::function<bool()> held;                             // held (Touch), or first moved (Latch)
        std::function<void(double)> show;                       // the lane shown on it while not written
        std::function<void(Sequence&, double)> live;            // what plays while written
        std::function<Param*(Sequence&)> laneIn;                // its lane in a sequence (to write at the stop)
        std::function<double(const Sequence&)> stillIn;         // its static value there (empty: the lane's own)
        std::function<bool(const Sequence&)> valid;             // still the same control after an edit (empty: always)
        bool whole = false;                                     // a switch's values are whole numbers
        double shown = 0;                                       // what its control shows
    };
    std::vector<LaneRecording> lanes_;
    std::set<QString> touched_;       // switches the user changed during the pass (a mute's "move")
    std::vector<Id> busesFromWrite_;  // handed over to Touch when the pass wrote
    QString liveState_;  // what the gesture last applied, to skip repeats
    std::function<void(Sequence&)> lastLive_;  // what plays (the last frame's lanes being written)
    FrameTime lastTick_ = 0;  // the frame the pass last played
    bool resume_ = false;     // the pass was ended by another sequence made active: start again at the next frame
    Id passSeq_ = 0;          // the sequence the pass records
    uint64_t passGeneration_ = 0;  // (EditorState::projectGeneration)
    bool passBaseChanged_ = false;  // the project changed during the pass: the recorders follow it at the next frame
    FrameTime stopAt_ = -1;         // playback stopped there; the pass is written once another gesture ends
    // Insert parameters moved in this pass: the last value, the frame it came at, and whether its control is held.
    struct InsertMove {
        double value = 0;
        FrameTime at = 0;
        std::function<bool()> held;
    };
    std::map<QString, InsertMove> insertMoves_;
    // Moves since the last frame, played at once rather than at the next frame.
    std::map<QString, std::pair<std::function<void(Sequence&, double)>, double>> pendingLive_;

    // Moves held without writing (Preview, and Latch primed while stopped): a lane (core/Automation.h's names) of a
    // track or bus, by "<owner>/<lane>".
    struct HeldMove {
        Id owner = 0;
        std::string lane;
        double value = 0;
    };
    std::map<QString, HeldMove> held_;
    Id heldSequence_ = 0;          // the sequence they were held on
    uint64_t heldGeneration_ = 0;  // (EditorState::projectGeneration)
    bool previewing_ = false;
    // Holds a control's move instead of editing (previewing, or a Latch control primed while stopped): true if held.
    bool holdMove(Id owner, AutomationMode mode, const std::string& lane, double value);
    std::function<void(Sequence&)> heldOverlay() const;
    void showHeld();      // the held values back on their controls (after a refresh or the playhead moved)
    void releaseHeld();   // forgets them, the controls showing the project again
    // In to Out, or from the playhead `fallback` frames on (-1: to the end) without them.
    std::pair<FrameTime, FrameTime> markedRange(FrameTime fallback) const;
    std::vector<Id> armedOwners() const;  // tracks and buses in a writing mode
    std::optional<WriteTo> writeTo_;      // pressed during a pass: applied at its stop
    std::optional<MixSnapshot> capture_;
    std::optional<AutomationCopy> copy_;
    std::vector<Id> copyOwners_;
    QToolButton* automationMenu_ = nullptr;
};

}  // namespace montage
