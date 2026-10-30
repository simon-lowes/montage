// Montage — Playback › Video Output › Reference Output…: the program sent to a broadcast monitor through a DeckLink
// card (SDI or HDMI), or over the network as NDI, OMT or SMPTE ST 2110 (render/ReferenceOutput.h, render/St2110.h). The controller opens the interface for the Program
// monitor's sequence and follows it (a sequence of another format opens it again); the panel chooses the interface,
// device, mode, bit depth, colour, framing, embedded audio and the offsets for a monitor's latency. ST 2110 streams are
// also offered to broadcast controllers as an AMWA NMOS Node (render/Nmos.h), which may send them elsewhere.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <memory>

#include "PlaybackController.h"
#include "render/Nmos.h"
#include "render/ReferenceOutput.h"
#include "render/St2110.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace montage {

class ReferenceOutputController : public QObject {
    Q_OBJECT
public:
    explicit ReferenceOutputController(PlaybackController* program, QObject* parent = nullptr);
    ~ReferenceOutputController() override;

    // Opens `config`'s interface for the Program's sequence; false and why when it cannot.
    bool start(const output::Config& config, QString* error = nullptr);
    void stop();
    bool running() const;
    // The settings last used (kept between sessions).
    output::Config config() const { return config_; }
    // The Program's sequence changed: opened again when its size, rate, colour or sound rate did.
    void sequenceChanged();
    // "1080p25, 10-bit PQ, on DeckLink Mini Monitor 4K: 1234 frames, 2 late" (or why it stopped).
    QString status() const;
    std::shared_ptr<output::Scheduler> scheduler() const { return scheduler_; }
    std::shared_ptr<output::Backend> backend() const { return backend_; }
    const output::Device& device() const { return device_; }
    // Its device's calibration target (app/DisplayCalibration.h); empty when not running.
    QString calibrationTarget() const;

    // The agent link's montage_reference_output (app/LiveLink.h): list, status, start or stop.
    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

    static output::Config loadConfig();
    static void saveConfig(const output::Config& c);
    // ST 2110's streams (where they go, multicast TTL, PTP reference), kept between sessions.
    static st2110::SenderOptions loadSt2110();
    static void saveSt2110(const st2110::SenderOptions& o);
    // The NMOS Node the ST 2110 output is offered through (whether, and how it finds a registry), kept between sessions.
    static nmos::NodeOptions loadNmos(bool* enabled = nullptr);
    static void saveNmos(const nmos::NodeOptions& o, bool enabled);
    // The Node while the ST 2110 output runs with it (else null).
    nmos::Node* nmosNode() const { return nmos_ && nmos_->running() ? nmos_ : nullptr; }

signals:
    void runningChanged(bool running);

private:
    QString formatKey() const;
    void stopOutput();
    void updateNmos();
    Sequence signalSequence(const Sequence& s) const;
    QPointer<PlaybackController> program_;  // (may go first when the window closes)
    output::Config config_;
    std::shared_ptr<output::Backend> backend_;
    std::shared_ptr<output::Scheduler> scheduler_;
    output::Device device_;
    QTimer* watch_ = nullptr;  // notices the output stopping by itself (a card unplugged)
    QString startedFor_;  // the sequence format it was opened for
    QString lastError_;
    nmos::Node* nmos_ = nullptr;
    bool activating_ = false;   // (restarting for a controller's activation)
    bool keepStreams_ = false;  // (restarting for the sequence: the streams a controller switched off stay off)
    bool routed_ = false;       // a controller changed where (or whether) the streams go since the last start by hand
    nmos::NodeOptions nmosStarted_;
};

class ReferenceOutputPanel : public QDialog {
    Q_OBJECT
public:
    ReferenceOutputPanel(ReferenceOutputController* controller, QWidget* parent = nullptr);
    output::Config chosen() const;

private:
    void fillDevices();
    void fillModes();
    void refresh();
    ReferenceOutputController* controller_;
    QComboBox* backend_;
    QComboBox* device_;
    QComboBox* mode_;
    QComboBox* depth_;
    QComboBox* colour_;
    QCheckBox* hdr_;
    QComboBox* scaling_;
    QComboBox* audio_;
    QSpinBox* videoOffset_;
    QSpinBox* audioOffset_;
    QCheckBox* captions_;
    QLineEdit* st2110Video_;
    QLineEdit* st2110Audio_;
    QLineEdit* st2110Data_;
    QSpinBox* st2110Ttl_;
    QLineEdit* st2110Clock_;
    QCheckBox* nmos_;
    QLineEdit* nmosRegistry_;
    QPushButton* saveSdp_;
    QFormLayout* form_;
    st2110::SenderOptions chosenSt2110() const;
    QLabel* why_;
    QLabel* status_;
    QPushButton* start_;
    QTimer* timer_;
    std::vector<output::Device> devices_;
};

}  // namespace montage
