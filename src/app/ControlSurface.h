// Montage — hardware control surfaces (Premiere's and Resolve's Mackie Control support, Pro Tools' HUI and Logic's MCU
// modes): a surface speaking the Mackie Control protocol (control/MackieControl.h) or HUI (control/Hui.h) works the
// mixer and transport. Its eight strips are eight audio tracks (banked across the rest): the motor faders set and
// follow each track's volume, automation included (a touched fader writes in Touch and Latch as the mixer's own fader
// does), the V-Pots pan, the buttons mute, solo and select, and Read, Write, Touch, Latch (and a HUI's Off) set the
// selected track's automation. A Mackie Control master fader is the mix's. Play, Stop, Rewind, Fast Forward, the jog
// wheel and the cursor keys move through the cut, Cycle (a HUI's Loop) loops playback and a HUI's RTZ and End go to
// either end; Marker (a HUI's keypad Enter), Undo and Save do what they say. The displays show track names and
// levels, the meters follow playback and the timecode display the playhead.
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

#include "control/Hui.h"
#include "control/Midi.h"
#include "core/Model.h"

class QTimer;

namespace montage {

class EditorState;
class MixerPanel;
class PlaybackController;

// The protocols a surface can speak (most offer both, chosen on the surface).
enum class SurfaceProtocol { MackieControl, Hui };

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
    // Which protocol the surface speaks; changed while connected, the surface is taken over in the new one.
    void setProtocol(SurfaceProtocol protocol);
    SurfaceProtocol protocol() const { return protocol_; }
    QString surfaceName() const;
    // What the transport keys do that lives in the main window: "record", "marker", "save", "previousEdit",
    // "nextEdit", "loop"; and the monitor keys: "dim", "mono", "monitorMute", "talkbackOn", "talkbackOff".
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
    void send(const std::vector<MidiMessage>& ms);
    void hello();  // what a surface is first sent: Mackie Control's device query, HUI's ping
    void start();  // the connection (or protocol) new: hello, then everything shown
    void reset();
    mcu::Event decode(const MidiMessage& m);  // (a surface announcing itself is shown everything again)
    void apply(const mcu::Event& e);
    void releaseAll();
    bool isHui() const { return protocol_ == SurfaceProtocol::Hui; }
    // To the surface, in its protocol.
    void sendFader(int strip, int value);
    void sendLed(int note, int state);  // 0 off, 2 on
    void sendRing(int strip, int value);
    void sendMeter(int strip, int level);
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
    uint64_t project_ = 0;  // the project the selection and bank are in (EditorState::projectGeneration)
    QString projectFile_;   // and its file
    void followProject();   // another project open: the selection cleared, the first tracks on the strips
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
    SurfaceProtocol protocol_ = SurfaceProtocol::MackieControl;
    hui::Decoder decoder_;
    qint64 pingAt_ = 0, pingReplyAt_ = 0;  // HUI: when a ping was last sent and last answered
    qint64 resentAt_ = 0;                  // HUI: when it was last shown everything again
    qint64 levelsAt_ = 0;                  // when playback levels last came
    std::array<std::string, 8> zones_;     // HUI: the main display's zones as last sent
    std::array<qint64, 8> zoneAt_{};
    std::vector<int> huiDigits_;           // HUI: the timecode digits as last sent, the rightmost first
    std::array<int, 8> levels_{};
    std::array<qint64, 8> meterAt_{};
};

}  // namespace montage
