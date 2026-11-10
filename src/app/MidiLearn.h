// Montage — MIDI Learn (Edit › MIDI Learn): any MIDI controller works the app. Turn Learn on, move a knob, fader,
// encoder or button, then click a parameter: one of the Inspector's (colour wheels included, one axis per control),
// a mixer strip's volume, pan, mute or solo, or a menu command. The mapping (control/MidiMap.h) gives it a range,
// curve, inversion, toggle or momentary behaviour and soft takeover; what it controls lights the controller back
// (buttons, LED rings, motor faders). Mixer strips are counted from a bank that bank buttons move across the tracks,
// and a mapping can belong to one panel, taking over the control while that panel has focus. A fader on a track's
// volume holds it while it moves, so Touch and Latch write automation as the mixer's own fader does. One control can
// drive several targets, each with its own range (learnt with "Add to its targets" on), and a mapping can show its
// target's value on another MIDI output (motor faders or LED rings on another device), so they follow automation.
// Parameters in Montage's dialogs are learnt as those in its panels. Each controller's map is a JSON file
// (core/MidiMaps.h) found again by the device's name when it is plugged back in.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "control/Midi.h"
#include "control/MidiMap.h"
#include "core/MidiMaps.h"

class QDialog;
class QFileSystemWatcher;
class QTimer;
class QWidget;

namespace montage {

class EditorState;
class MixerPanel;

class MidiLearn : public QObject {
    Q_OBJECT
public:
    MidiLearn(EditorState* state, MixerPanel* mixer, QWidget* window, QObject* parent = nullptr);
    ~MidiLearn() override;

    // The controller, by its MIDI ports' names (the output, for lights, may be empty). Kept for the next start; a
    // controller that goes away is looked for again by name until it comes back.
    bool connectDevice(const QString& input, const QString& output, QString* error = nullptr);
    // A connection made elsewhere (tests pass a stand-in), and the device name its map goes by.
    void setConnection(std::unique_ptr<MidiConnection> connection, const QString& device);
    void disconnectDevice();
    bool isConnected() const { return connection_ != nullptr; }
    QString deviceName() const { return device_; }
    // Reconnects to the controller last used, now if it is there and else when it is plugged in.
    void restore();

    // What the connection calls, from any thread (queued for the GUI thread), and handling there.
    void receive(const MidiMessage& m);
    void connectionLost();
    void handle(const MidiMessage& m);

    // Learning: the next control moved and the next parameter clicked are mapped together (either first).
    void setLearning(bool on);
    bool isLearning() const { return learning_; }
    void setLearnInPanel(bool on) { learnInPanel_ = on; }  // learnt mappings belong to the focused panel
    bool learnsInPanel() const { return learnInPanel_; }
    // A control learnt again adds the parameter to those it drives (else it drives only the one learnt last).
    void setLearnAdds(bool on) { learnAdds_ = on; }
    bool learnAdds() const { return learnAdds_; }
    // A parameter chosen while learning (what a click picks; tests and the dialog call it too).
    void learnTarget(const QString& target);
    // What a click on this widget would learn ("" for nothing mappable).
    QString targetAt(QWidget* w) const;
    QString targetLabel(const QString& target) const;

    // The map: mappings, the bank they count strips from, and where maps are kept.
    const std::vector<midimap::Mapping>& mappings() const { return mapper_.mappings(); }
    void setMapping(size_t index, const midimap::Mapping& m);
    void addMapping(const midimap::Mapping& m);
    void removeMapping(size_t index);
    void setFolder(const QString& folder);
    QString folder() const { return folder_; }
    QString mapFile() const;
    int bank() const { return bank_; }
    void setBank(int bank);
    int bankSize() const { return bankSize_; }
    void setBankSize(int n);
    QString panel() const { return QString::fromStdString(mapper_.layer()); }
    void setPanel(const QString& layer);  // (normally the focused panel's, followed by itself)
    QString panelOf(QWidget* w) const;

    // A target's value (0-1; NaN when it is not there) and setting it as a control does.
    double targetValue(const QString& target) const;
    void applyChange(const QString& target, const midimap::Change& c);
    // Lets go of everything held (as when each control rests).
    void releaseHolds();
    void setHoldTime(int ms) { holdMs_ = ms; }
    // Sends the controller what changed (also on a timer while connected), and mappings' own outputs theirs.
    void refresh();
    // How a mapping's own output port is opened, by its name (tests pass stand-ins; by default the system's MIDI
    // output of that name). Ports are opened when first sent to; one that cannot be is looked for again in a while.
    using OutputOpener = std::function<std::unique_ptr<MidiConnection>(const QString& port, QString* error)>;
    void setOutputOpener(OutputOpener opener);
    // The controller's ports, Learn, and the map as a table to adjust (range, curve, inversion, toggle, takeover).
    QDialog* openDialog(QWidget* parent);

signals:
    void connectionChanged(bool connected);
    void learningChanged(bool on);
    void mapChanged();
    void learnt(const QString& control, const QString& target);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void drain();
    void lost();
    void reconnect();
    void loadMap();
    void saveMap();
    void mapFileChanged();
    void learnControl(const midimap::Learnt& l);
    void complete();
    void hold(const QString& target, QObject* what);
    void release(const QString& target);
    QWidget* widgetFor(const QString& target) const;
    bool inApp(const QWidget* w) const;  // in the main window, one of its floating panels or one of its dialogs
    MidiConnection* outputPort(const std::string& name);

    EditorState* state_;
    MixerPanel* mixer_;
    QWidget* window_;
    std::unique_ptr<MidiConnection> connection_;
    QString device_, output_;
    std::atomic<quint64> generation_{0};
    std::mutex inboxMutex_;
    std::vector<MidiMessage> inbox_;
    bool drainPending_ = false;

    midimap::Decoder decoder_;
    midimap::Learner learner_;
    midimap::Mapper mapper_;
    bool learning_ = false, learnInPanel_ = false, learnAdds_ = false;
    QString justLearntTarget_;  // (a key learnt, then pressed into: its aftertouch takes the mapping over)
    std::optional<midimap::Learnt> pendingControl_;
    QString pendingTarget_;
    std::optional<midimap::Key> justLearnt_;  // its moves after it was learnt are not learnt again until it rests
    qint64 justLearntAt_ = 0;
    bool loadFailed_ = false;                  // the file on disk was not read: kept aside before it is written over
    QString mappingsFrom_;                     // the map file the mappings in use came from
    mutable std::map<QString, QPointer<QWidget>> found_;  // widgets found for targets (the refresh asks often)
    QTimer* learnTimer_ = nullptr;  // a 7-bit controller decided once it stops moving
    QTimer* refreshTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QFileSystemWatcher* watcher_ = nullptr;
    QString folder_;
    QByteArray saved_;  // the map file as last written (the watcher's own saves are not read back)
    int bank_ = 0, bankSize_ = 8;
    int holdMs_ = 1000;
    std::map<QString, QTimer*> holds_;
    bool applying_ = false;
    OutputOpener openOutput_;
    std::map<std::string, std::unique_ptr<MidiConnection>> outputs_;  // mappings' own ports, by name
    std::map<std::string, qint64> outputFailed_;                       // (when each last could not be opened)
};

}  // namespace montage
