// Montage — Tangent's colour panels (Elements, Ripple, Wave 2, Arc) through Tangent Hub (control/Tangent.h), as
// Resolve and Lumetri use them. While on, Montage looks for the Hub on this computer (again every few seconds while it
// is not there), names itself and writes the controls it offers (controls.xml, in Montage's data folder) for Tangent
// Mapper and the panels' own default maps:
//
//   Primaries   every Color Correct control (Lift, Gamma and Gain, each master and red, green and blue, bound together
//               for the trackballs; Offset, Exposure, Contrast and Pivot, Saturation, Temperature and Tint, the tone
//               controls and Vibrance). A turn changes the graded clip's Color Correct at the playhead (the one in its
//               picked node, a shared node's in the shared node, added there if it has none), its keyframe there if it
//               has keyframes; a run of turns on one node is one undo step. Reset puts the control back to its default.
//   Node        a menu: the graded clip's correctors in turn (the Nodes panel follows).
//   Transport   the jog wheel steps a frame a count, the shuttle ring plays at 1, 2, 4... 32 times either way as it turns
//               further, and stops back in the middle.
//   Commands    every menu command (as Keyboard Shortcuts lists it), plus Play / Stop and the next and previous node.
//
// The graded clip is the one the Inspector shows (its node the one picked in the Nodes panel), else the top picture
// clip at the playhead.
// The panels' displays show its name and node. When the clip, node or its values change elsewhere, the Hub is told to
// ask for every value again.
#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <map>

#include "control/Tangent.h"
#include "core/Model.h"

class QTcpSocket;
class QTimer;
class QWidget;

namespace montage {

class EditorState;
class PlaybackController;

class TangentControl : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<PlaybackController*()> controller;  // the monitor the transport drives
    };
    TangentControl(EditorState* state, QWidget* window, Hooks hooks, QObject* parent = nullptr);
    ~TangentControl() override;

    // Connects to the Hub now and while on (and lets it go when turned off).
    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }
    bool isConnected() const;  // (and the Hub has greeted it)
    QString lastError() const { return error_; }
    // The Hub's port (tests run their own) and the folder controls.xml goes in.
    void setPort(quint16 port) { port_ = port; }
    void setFolder(const QString& dir) { folder_ = dir; }
    QString folder() const;

    // Ids: a Color Correct control's, the Node menu's, the mode's, a command's (a menu command's id, or one of
    // "tangent:play", "tangent:next_node", "tangent:previous_node").
    static uint32_t parameterId(const std::string& name);
    static constexpr uint32_t kNodeMenu = 0x04000001;
    static constexpr uint32_t kMode = 0x00010001;
    uint32_t actionId(const QString& command) const;
    // What it offers, as controls.xml says it.
    tangent::ControlSystem controls() const;
    // The graded clip, its node, and the frame within it (0 clip: none).
    struct Target {
        Id clip = 0;
        int node = 0;
        FrameTime at = 0;
    };
    Target target() const;
    // A Color Correct control's value there (its default with none).
    double value(const std::string& name) const;

    // A message from the Hub, as if it came over the connection (they are handled so; public for tests).
    void handle(const tangent::HubMessage& m);

signals:
    void connectionChanged();

private:
    void tryConnect();
    void readHub();
    void lost();
    void send(const std::string& bytes);
    void introduce();  // controls.xml written, Montage named
    bool change(const std::string& name, double to, bool reset);
    void sendValue(uint32_t id);
    void sendMenu();
    void sendDisplay(const Target& t);
    double value(const std::string& name, const Target& t) const;
    void watch();  // the clip, node and values: the Hub told when they change
    void command(const QString& id, bool on);
    void buildActions();

    EditorState* state_;
    QWidget* window_;
    Hooks hooks_;
    QTcpSocket* socket_ = nullptr;
    QTimer* retry_ = nullptr;
    QTimer* watch_ = nullptr;
    tangent::Decoder decoder_;
    bool enabled_ = false, introduced_ = false;
    quint16 port_ = tangent::kHubPort;
    QString folder_, error_;
    std::map<uint32_t, std::string> parameters_;  // id -> Color Correct control
    std::map<uint32_t, QString> actions_;         // id -> command
    std::map<QString, uint32_t> actionIds_;
    QString watched_, displayed_;
    int shuttle_ = 0;  // the shuttle ring's place (its turns added up)
};

}  // namespace montage
