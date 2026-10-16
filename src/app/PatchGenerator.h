// Montage — the network patch generator (render/MonitorCalibration.h): connects to calibration software listening on
// a TCP port (20002 by convention) and shows the patches it asks for, full screen on a display or on the reference
// output, so a probe can measure that display through Montage. Keeps trying until the software is listening, and
// again if it goes. Patches bypass the display's calibration unless asked to go through it (to verify one).
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <memory>

#include "render/MonitorCalibration.h"

class QScreen;
class QTcpSocket;
class QTimer;

namespace montage {

class ReferenceOutputController;

// A screen-filling patch.
class PatchWindow : public QWidget {
    Q_OBJECT
public:
    explicit PatchWindow(QWidget* parent = nullptr);
    void setPatch(const CalibrationPatch& patch, std::shared_ptr<const Lut3D> lut);
    // The picture as drawn (at the window's size in pixels).
    QImage picture() const;

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    CalibrationPatch patch_;
    std::shared_ptr<const Lut3D> lut_;
    bool has_ = false;
};

class PatchGenerator : public QObject {
    Q_OBJECT
public:
    explicit PatchGenerator(ReferenceOutputController* output, QWidget* parentWindow, QObject* parent = nullptr);
    ~PatchGenerator() override;

    // Where patches go: a screen (null: the main window's), or the reference output (which must be running).
    void setScreen(QScreen* screen);
    void setReferenceOutput(bool on);
    bool referenceOutput() const { return toOutput_; }
    // Patches through the display's calibration LUT (to verify it), else around it (to measure the display).
    void setThroughCalibration(bool on);
    bool throughCalibration() const { return through_; }

    // Connects to the software at `host`:`port`, trying again until it listens; false and why when the output
    // chosen cannot show patches.
    bool start(const QString& host, int port, QString* error = nullptr);
    void stop();
    bool active() const { return active_; }
    bool connected() const;
    // "Waiting for the calibration software at 127.0.0.1:20002", "Connected: 512 patches, last R 940 G 64 B 64 (10-bit)".
    QString status() const;
    int patches() const { return count_; }
    const CalibrationPatch& lastPatch() const { return last_; }
    PatchWindow* window() const { return window_; }

signals:
    void statusChanged();
    void patchShown();

private:
    void connectNow();
    void show(const CalibrationPatch& p);
    void clearShown();
    QPointer<ReferenceOutputController> output_;
    QPointer<QWidget> parentWindow_;
    QPointer<QScreen> screen_;
    bool toOutput_ = false, through_ = false, active_ = false;
    QString host_;
    int port_ = kPatchGeneratorPort;
    QTcpSocket* socket_ = nullptr;
    QTimer* retry_ = nullptr;
    PatchStream stream_;
    QPointer<PatchWindow> window_;
    CalibrationPatch last_;
    int count_ = 0;
    QString error_;
};

}  // namespace montage
