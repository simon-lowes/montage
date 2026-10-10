// Montage — Clip > Pan to Follow (Resolve's IntelliTrack panning): click what a sound comes from in the picture, and
// the point is tracked through the shot both ways and written into the audio track's pan automation (its surround
// position in surround and ambisonic sequences), so the sound moves across the stage with what makes it.
#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/ClipAnalysis.h"

class QCloseEvent;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace montage {

class EditorState;
class PanFollowPicker;

class PanFollowDialog : public QDialog {
    Q_OBJECT
public:
    PanFollowDialog(EditorState* state, Id audioClip, QWidget* parent = nullptr);
    ~PanFollowDialog() override;

    // For tests and the agent: the point to follow (fractions of the sequence frame), the size of what is followed
    // (0 small, 1 medium, 2 large), how much of the stage the picture spans (0..1), and tracking (waiting for it).
    void setPoint(double x, double y);
    double pointX() const { return x_; }
    double pointY() const { return y_; }
    void setSize(int size);
    void setWidth(double width);
    FrameTime frame() const { return from_; }
    bool track(bool wait = false);
    bool busy() const { return running_; }
    int keyCount() const { return keys_; }
    QString status() const;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    struct Pending {
        std::vector<PanFollowKey> keys;
        Id source = 0;  // the picture tracked, and where it started
        FrameTime sourceStart = 0;
    };
    void refreshFrame(bool first);
    bool stereoLane() const;
    void finish();
    void applyPending();
    void stop();

    EditorState* state_;
    Id clip_;
    FrameTime from_ = 0;
    double x_ = 0.5, y_ = 0.5;
    int keys_ = 0;
    FrameTime span_ = 0;
    // Where things were when tracking began.
    Id seq_ = 0;
    FrameTime clipStart_ = 0;
    TrackRef clipTrack_;
    bool closed_ = false;
    bool outcome_ = false;       // the hint shows how the last attempt went
    bool refreshLater_ = false;  // edits came while tracking
    QTimer* refresh_ = nullptr;
    PanFollowPicker* picker_;
    QComboBox* size_;
    QSpinBox* width_;
    QLabel* info_;
    QProgressBar* progress_;
    QPushButton* track_;
    QFutureWatcher<bool> watcher_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<double>> fraction_ = std::make_shared<std::atomic<double>>(0);
    std::shared_ptr<Pending> pending_;
    std::shared_ptr<std::string> error_;
    bool running_ = false;
};

}  // namespace montage
