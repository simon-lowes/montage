// Montage — control over OSC (control/Osc.h): Bitfocus Companion (Stream Deck, Loupedeck, X-keys, Razer and other
// surfaces) through its generic OSC connection, TouchOSC, QLab, lighting desks and scripts send messages to a UDP port
// and Montage does what they say:
//
//   /montage/play  /montage/stop  /montage/playpause  /montage/rewind  /montage/forward  /montage/shuttle <-1|0|1>
//   /montage/step <frames>  /montage/goto <timecode | seconds>  /montage/start  /montage/end
//   /montage/previous_edit  /montage/next_edit  /montage/in  /montage/out  /montage/marker [name]  /montage/loop [0|1]
//   /montage/undo  /montage/redo  /montage/save  /montage/record
//   /montage/command/<menu>/<command>   any menu command (as Keyboard Shortcuts lists it, e.g. sequence/add_marker)
//   /montage/tool/<name> [json]         any agent tool on the open project (e.g. add_title '{"text":"Hi"}'), one undo step
//   /montage/track/<n>/volume <dB>  /mute [0|1]  /solo [0|1]  /pan <-1..1>   (audio track n, from 1)
//   /montage/master/volume <dB>
//
// Feedback goes to the address messages came from, at the feedback port: /montage/state/playing, /timecode, /loop
// (all again whenever the sender or port changes), tools' answers as /montage/reply/<tool> to whoever asked, and
// errors as /montage/error. Montage's own feedback addresses are never answered (a feedback port pointed back at it
// would otherwise answer itself for ever). It listens on this computer only unless told to listen on the network, and
// runs agent tools only when they are allowed (any program that can send to the port could otherwise run them), and
// then only on the open project.
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>

#include "control/Osc.h"

class QAbstractSlider;
class QTimer;
class QUdpSocket;
class QWidget;

namespace montage {

class EditorState;
class LiveLink;
class MixerPanel;
class PlaybackController;

class OscControl : public QObject {
    Q_OBJECT
public:
    OscControl(EditorState* state, MixerPanel* mixer, PlaybackController* program, LiveLink* link, QWidget* window,
               QObject* parent = nullptr);
    ~OscControl() override;

    // Listens on `port` (0: any free one), on this computer only or on the network too; false with the error.
    bool start(quint16 port, bool network, QString* error = nullptr);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    // Where feedback goes: this port at the address messages last came from (0: none; never the port it listens on).
    void setFeedbackPort(quint16 port);
    quint16 feedbackPort() const { return feedbackPort_; }
    // What the transport messages do that lives in the main window: "marker", "save", "record", "loop",
    // "previousEdit", "nextEdit".
    void setCommand(const QString& name, std::function<void()> fn) { commands_[name] = std::move(fn); }

    // Whether /montage/tool/ runs agent tools (off: refused).
    void setToolsAllowed(bool on) { toolsAllowed_ = on; }
    bool toolsAllowed() const { return toolsAllowed_; }
    // A message, as if it came from `from` (the socket's are handled so; public for tests). False with the reason
    // (also sent back as /montage/error) when it was not understood or could not be done.
    bool handle(const osc::Message& m, const QHostAddress& from = QHostAddress::LocalHost, QString* error = nullptr);
    // A menu command's address part ("Sequence/Add Marker" -> "sequence/add_marker"), and every command's full address
    // with its id, in menu order (for the settings dialog).
    static QString slug(const QString& commandId);
    QList<std::pair<QString, QString>> commandAddresses() const;

signals:
    void runningChanged(bool running);
    void handled(const QString& address, bool ok);

private:
    void readPending();
    void send(const osc::Message& m);
    void send(const osc::Message& m, const QHostAddress& to);
    void hold(QAbstractSlider* control);  // moved from here: held a second, so Touch and Latch passes write it
    void sendFeedback();  // what changed since last time (playing, timecode, loop)
    bool command(const QString& name, QString* error);
    bool runTool(const QString& name, const osc::Message& m, QString* error);

    EditorState* state_;
    MixerPanel* mixer_;
    PlaybackController* program_;
    LiveLink* link_;
    QWidget* window_;
    QUdpSocket* socket_ = nullptr;
    QTimer* feedbackTimer_ = nullptr;
    std::map<QString, std::function<void()>> commands_;
    QHostAddress peer_;  // where messages last came from
    quint16 feedbackPort_ = 0;
    bool toolsAllowed_ = false;
    std::map<QAbstractSlider*, qint64> heldUntil_;
    int playingSent_ = -1, loopSent_ = -1;
    QString timecodeSent_;
    int calls_ = 0;
};

}  // namespace montage
