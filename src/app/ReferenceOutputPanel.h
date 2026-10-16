// Montage — Playback › Video Output › Reference Output…: the program sent to a broadcast monitor through a DeckLink
// card (SDI or HDMI) or over NDI (render/ReferenceOutput.h). The controller opens the interface for the Program
// monitor's sequence and follows it (a sequence of another format opens it again); the panel chooses the interface,
// device, mode, bit depth, colour, framing, embedded audio and the offsets for a monitor's latency.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <memory>

#include "PlaybackController.h"
#include "render/ReferenceOutput.h"

class QCheckBox;
class QComboBox;
class QLabel;
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

signals:
    void runningChanged(bool running);

private:
    QString formatKey() const;
    Sequence signalSequence(const Sequence& s) const;
    QPointer<PlaybackController> program_;  // (may go first when the window closes)
    output::Config config_;
    std::shared_ptr<output::Backend> backend_;
    std::shared_ptr<output::Scheduler> scheduler_;
    output::Device device_;
    QTimer* watch_ = nullptr;  // notices the output stopping by itself (a card unplugged)
    QString startedFor_;  // the sequence format it was opened for
    QString lastError_;
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
    QLabel* why_;
    QLabel* status_;
    QPushButton* start_;
    QTimer* timer_;
    std::vector<output::Device> devices_;
};

}  // namespace montage
