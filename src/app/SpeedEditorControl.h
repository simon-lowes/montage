// Montage — the Blackmagic Speed Editor and DaVinci Resolve Editor Keyboard (control/SpeedEditor.h) as Resolve uses
// them, over USB or Bluetooth: found when plugged in or paired (looked for every few seconds), authenticated and kept
// so, then
//
//   JOG, SHTL, SCRL   the wheel moves the playhead a frame a step, shuttles (a quarter to 32 times speed either way,
//                     by how far it is turned), or scrolls a quarter second a step; SOURCE and TIMELINE pick the monitor
//   TRIM IN, TRIM OUT, ROLL   the edit nearest the playhead selected for trimming its incoming side, its outgoing side
//                     or both, then trimmed with the wheel (ESC ends it); ROLL twice slides the clip at the playhead
//   SLIP SRC, SLIP DEST   the wheel slips the clip after or before the nearest edit
//   AUDIO LEVEL, TRANS DUR (held)   the wheel sets the level of the sound at the playhead (or the selected sound
//                     clips), or the length of the transition at the nearest edit
//   CUT, DIS, SMTH CUT   the nearest edit made a cut, a dissolve or a smooth cut (their lights show which it is)
//   CAM 1-9           the multicam clip's angle (cut at the playhead while playing, or with LIVE O/WR lit)
//   VIDEO ONLY, AUDIO ONLY, SNAP   switches, lit while on
//   the rest          the menu commands of the same name (source edits, In and Out, Ripple Delete, Split...); pressed
//                     twice, keys do what their brackets say (IN [CLR], ESC [UNDO], AUDIO LEVEL [MARK], TRANS [TITLE])
//
// Any key can do any menu command instead (setBinding). Resolve holds the device while it runs, so only one program
// can use it at a time.
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <memory>

#include "control/Hid.h"
#include "control/SpeedEditor.h"
#include "core/Model.h"

class QTimer;
class QWidget;

namespace montage {

class EditorState;
class PlaybackController;

class SpeedEditorControl : public QObject {
    Q_OBJECT
public:
    // What lives in the main window.
    struct Hooks {
        std::function<PlaybackController*()> controller;  // the monitor the wheel and STOP/PLAY drive
        std::function<void(bool source)> showMonitor;
        std::function<bool(int side)> selectEdit;         // trim mode on the nearest edit: 0 both, 1 outgoing, 2 incoming
        std::function<bool(FrameTime)> trimBy;            // the selected edit
        std::function<bool()> trimming;
        std::function<bool(int angle, bool cut)> switchAngle;
        std::function<int()> angle;                       // the current multicam clip's (-1: none)
    };
    SpeedEditorControl(EditorState* state, QWidget* window, Hooks hooks, QObject* parent = nullptr);
    ~SpeedEditorControl() override;

    // Looks for a device now and every few seconds while on (and lets one go when turned off).
    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }
    bool isConnected() const { return device_ != nullptr; }
    QString deviceName() const { return name_; }
    QString lastError() const { return error_; }
    int batteryLevel() const { return battery_; }  // -1: not reported (on the cable)
    bool charging() const { return charging_; }
    // Opens a device already open (tests): authenticated, then used. False with the reason.
    bool attach(std::unique_ptr<HidDevice> device, const HidDeviceInfo& info, QString* error = nullptr);
    void detach();
    // An input report, as if the device sent it (the device's are handled so).
    void feed(const std::vector<uint8_t>& report);
    // Authenticates again (a timer does it before the device's time runs out).
    bool renew();

    // What a key does: a menu command's id, one of the built-in functions ("speed:jog", ...), or "" for nothing.
    QString binding(speed::Key k) const;
    static QString defaultBinding(speed::Key k);
    void setBinding(speed::Key k, const QString& what);  // what == defaultBinding(k) clears the change
    QJsonObject bindingChanges() const;                  // {key name: what}, the keys changed only
    void setBindingChanges(const QJsonObject& changes);
    static QStringList builtIns();
    static QString builtInLabel(const QString& id);
    // What a key does pressed twice ("" for nothing): only while it does its own thing.
    QString secondBinding(speed::Key k) const;

    // The wheel's mode (Key::Jog, Shuttle or Scroll) and the speed a shuttle position gives.
    speed::Key jogMode() const { return jogMode_; }
    void setJogMode(speed::Key mode);
    static double shuttleSpeed(int32_t position);
    // The lights as last sent.
    uint32_t leds() const { return ledsSent_; }
    uint8_t jogLeds() const { return jogLedsSent_; }
    bool liveOverwrite() const { return liveOverwrite_; }
    void refreshLeds();

signals:
    void connectionChanged();
    void batteryChanged();

private:
    void poll();
    void lost();
    void keyDown(speed::Key k);
    void keyUp(speed::Key k);
    void run(const QString& what, speed::Key k);
    void runBuiltIn(const QString& id, speed::Key k);
    void jog(speed::JogMode mode, int32_t value);
    void turn(int steps);
    bool setEditTransition(const std::string& type);
    std::string editTransition() const;  // the nearest edit's: "" a cut, "?" none there
    bool targetEdit(Id& outgoing, Id& incoming, FrameTime& at) const;
    void startGesture();  // a new hold, slip or slide: one undo step, and the shuttle stopped
    void endTarget();
    void stopShuttle();
    bool wheelTurns() const;  // a target or trim takes the wheel's turns
    void syncWheelMode();
    void send(const std::vector<uint8_t>& report);

    EditorState* state_;
    QWidget* window_;
    Hooks hooks_;
    std::unique_ptr<HidDevice> device_;
    speed::Decoder decoder_;
    QString name_, error_;
    uint16_t product_ = 0;
    bool enabled_ = false;
    QTimer* pollTimer_ = nullptr;
    QTimer* authTimer_ = nullptr;
    QTimer* ledTimer_ = nullptr;
    QTimer* deferTimer_ = nullptr;
    // Paths that would not open or authenticate, and when (tried again a little later).
    std::map<std::string, qint64> refused_;
    QElapsedTimer clock_;
    uint64_t generation_ = 0;  // which device opened the reports still queued come from
    int battery_ = -1;
    bool charging_ = false;
    std::map<speed::Key, QString> changes_;
    // Keys pressed twice.
    speed::Key lastKey_ = speed::Key::None;
    QElapsedTimer lastPress_;
    speed::Key deferred_ = speed::Key::None;
    // The wheel.
    speed::Key jogMode_ = speed::Key::Jog;
    int accumulated_ = 0;
    double shuttleSent_ = 0;
    QPointer<PlaybackController> shuttled_;  // the monitor the shuttle runs
    // The wheel's mode on the device: relative while a target or trim takes its turns, even in SHTL.
    speed::JogMode wheelModeSent_ = speed::JogMode::Relative2;
    bool wheelModeKnown_ = false;
    enum class Target { None, Slip, Slide, Level, TransitionDuration } target_ = Target::None;
    speed::Key holding_ = speed::Key::None;
    Id targetClip_ = 0;
    Id rollClip_ = 0;  // the clip at the playhead when ROLL was pressed (ROLL twice slides it)
    bool second_ = false;  // a key's second press running
    int gesture_ = 0;  // each hold or slip one undo step
    QString group_;    // its undo group while on
    bool liveOverwrite_ = false;
    uint32_t ledsSent_ = 0;
    uint8_t jogLedsSent_ = 0;
    bool ledsKnown_ = false;
};

}  // namespace montage
