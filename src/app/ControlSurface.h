// Montage — hardware control surfaces (Premiere's and Resolve's Mackie Control support, Pro Tools' and Logic's MCU
// mode): a surface speaking the Mackie Control protocol (control/MackieControl.h) works the mixer and transport.
// Its eight strips are eight audio tracks (banked across the rest): the motor faders set and follow each track's
// volume, automation included (a touched fader writes in Touch and Latch as the mixer's own fader does), the V-Pots
// pan, the buttons mute, solo and select, and Read, Write, Touch and Latch set the selected track's automation. The
// master fader is the mix's. Play, Stop, Rewind, Fast Forward, the jog wheel and the cursor keys move through the
// cut; Marker, Undo and Save do what they say. The scribble strips show track names and levels, the meters follow
// playback and the display shows the timecode.
#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "control/Midi.h"
#include "core/Model.h"

class QTimer;

namespace montage {

class EditorState;
class MixerPanel;
class PlaybackController;

class ControlSurface : public QObject {
    Q_OBJECT
public:
    ControlSurface(EditorState* state, MixerPanel* mixer, PlaybackController* program, QObject* parent = nullptr);
    ~ControlSurface() override;

    // Opens the surface's MIDI input and output (ids from midiInputs and midiOutputs).
    bool connectPorts(const std::string& input, const std::string& output, std::string* error = nullptr);
    // A connection made elsewhere (tests pass a stand-in that records what is sent).
    void setConnection(std::unique_ptr<MidiConnection> connection);
    // Clears the surface (faders down, lights off, strips blank) and closes it.
    void disconnectSurface();
    bool isConnected() const { return connection_ != nullptr; }
    QString surfaceName() const;
    // What the transport keys do that lives in the main window: "record", "marker", "save", "previousEdit",
    // "nextEdit".
    void setCommand(const QString& name, std::function<void()> fn) { commands_[name] = std::move(fn); }

    // A message from the surface (on the GUI thread; the connection's are queued here).
    void handle(const MidiMessage& m);
    // What the connection calls, from any thread: a message received (queued for the GUI thread, where a fader's
    // run of moves that waited together is applied once), and the device gone (holds released, the surface closed).
    void receive(const MidiMessage& m);
    void connectionLost();
    // Sends the surface what changed since last time (also on a timer, about 30 times a second).
    void refresh();
    // The first audio track on the strips, and the track selected on the surface (-1 none).
    int bank() const { return bank_; }
    int selectedTrack() const;

signals:
    void connectionChanged(bool connected);

public slots:
    // Playback levels (as PlaybackController::audioLevels gives them) for the meters.
    void setLevels(float masterL, float masterR, const QVector<float>& trackPeaks);

private:
    struct Shown {  // what the surface was last sent, per strip (-1 / empty: nothing yet)
        int fader = -1, ring = -1, mute = -1, solo = -1, select = -1, meter = -1;
        std::string top, bottom;
    };
    void send(const MidiMessage& m);
    void reset();
    void drain();  // messages queued from the MIDI thread, a fader's run of moves coalesced
    void lost();   // the device went away: holds released, nothing more sent
    int tracks() const;
    void setBank(int first);
    void moveFader(int strip, int value);
    void touch(int strip, bool down);
    void hold(int strip);  // a fader or pot moved without a touch message: held until it rests
    void release(int strip);
    void releasePot(int strip);
    void turn(int strip, int steps);
    void button(int note, bool down);
    void command(const QString& name);

    EditorState* state_;
    MixerPanel* mixer_;
    PlaybackController* program_;
    std::unique_ptr<MidiConnection> connection_;
    std::map<QString, std::function<void()>> commands_;
    QTimer* timer_ = nullptr;
    int bank_ = 0;
    Id selected_ = 0;  // the selected track (by id, so it stays itself when tracks move)
    // Per strip (and the master, index 8): held by touch, held by movement (faders and pots apart), when a pot last
    // turned, and when the level below the name was last sent.
    std::array<bool, 9> touched_{};
    std::array<QTimer*, 9> holds_{};
    std::array<QTimer*, 8> potHolds_{};
    std::array<qint64, 8> turnedAt_{};
    std::array<qint64, 8> bottomAt_{};
    // Messages from the MIDI thread, waiting for the GUI thread.
    std::mutex inboxMutex_;
    std::vector<MidiMessage> inbox_;
    bool drainPending_ = false;
    std::atomic<quint64> generation_{0};  // counts connections, so a loss reported by one closed since is ignored
    std::array<Shown, 9> shown_;
    std::map<int, int> leds_;  // transport and automation lights as last sent
    std::vector<std::string> digits_;  // the timecode display as last sent
    std::string assignment_;           // the assignment display as last sent
    int device_ = 0x14;                // how the surface addresses itself (0x14 Mackie Control, 0x10 Logic Control)
    std::array<int, 8> levels_{};
    std::array<qint64, 8> meterAt_{};
};

}  // namespace montage
