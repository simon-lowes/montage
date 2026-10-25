// Montage — Clip > Learn Effect from Examples: frames of the clip worked on by hand (its effects: Paint, a grade, masks,
// a matte) are its examples; a small network is trained on them in the background, with a frame shown as it learns,
// and can be stopped and continued; then given to the clip, or the clips selected, as a Learned Effect
// (media/LearnedEffect.h, render/LearnFromClip.h).
#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

#include "core/Model.h"
#include "media/LearnedEffect.h"

class QCheckBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace montage {

class EditorState;

class LearnEffectDialog : public QDialog {
    Q_OBJECT
public:
    LearnEffectDialog(EditorState* state, Id clip, QWidget* parent = nullptr);
    ~LearnEffectDialog() override;

    Id clip() const { return clip_; }
    // For tests and the agent.
    std::vector<FrameTime> frames() const { return frames_; }
    void setFrames(std::vector<FrameTime> frames);
    void setSteps(int steps);
    void setOptions(const learn::TrainOptions& options) { options_ = options; }  // (crops, batch: smaller to test)
    // Trains `steps` more (going on from what was trained before, in this dialog or saved); with `wait`, until done.
    bool train(bool wait = false);
    void stop();
    bool busy() const { return running_; }
    int stepsTrained() const;
    QString networkFile() const { return QString::fromStdString(file_); }
    // Gives the network to the clip (or, with `selected`, the clips selected), turning off the clip's own effects when
    // `replace`.
    bool apply(bool selected = false, bool replace = true);
    QString status() const;

private:
    void showFrames();
    void finished();
    void showPreview(const Image& before, const Image& learned);

    EditorState* state_;
    Id clip_;
    std::string file_;
    std::vector<FrameTime> frames_;
    learn::TrainOptions options_;
    std::shared_ptr<learn::Trainer> trainer_;
    std::vector<FrameTime> trainedOn_;  // the frames trainer_ has
    int steps_done_ = 0;                // (as trainer_ was given back: the worker changes it while training)
    double loss_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QFutureWatcher<QString> watcher_;
    bool running_ = false;
    QListWidget* list_;
    QSpinBox* steps_;
    QLabel* preview_;
    QLabel* info_;
    QProgressBar* progress_;
    QPushButton* train_;
    QPushButton* apply_;
    QCheckBox* replace_;
    QCheckBox* selected_;
};

}  // namespace montage
