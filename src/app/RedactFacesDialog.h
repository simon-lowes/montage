// Montage — Clip > Redact Faces: finds every face in the clip, follows each from frame to frame, groups them by
// person and covers them (blur, pixelate or a solid colour); anyone who may be shown is unticked and left as they are.
// The analysis and the choice are kept on the clip's Redact Faces effect, whose look is set in Effect Controls.
#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

#include "core/Model.h"
#include "media/FaceTracks.h"

class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace montage {

class EditorState;

class RedactFacesDialog : public QDialog {
    Q_OBJECT
public:
    RedactFacesDialog(EditorState* state, Id clip, QWidget* parent = nullptr);
    ~RedactFacesDialog() override;

    // For tests and the agent: run the analysis (and wait for it), tick or untick a person, write the effect.
    bool findFaces(bool wait = false);
    bool busy() const { return running_; }
    int groupCount() const { return int(groups_.size()); }
    const std::vector<FaceGroup>& groups() const { return groups_; }
    bool redacted(int group) const;
    void setRedacted(int group, bool on);
    void setStyle(int style);
    bool apply();
    QString status() const;
    QListWidget* list() const { return list_; }

private:
    void loadFromClip();
    void finishAnalysis();
    void showGroups();
    void stop();

    EditorState* state_;
    Id clip_;
    std::string path_;
    MediaItem media_;
    double start_ = 0, end_ = 0;
    std::shared_ptr<FaceTracks> tracks_;
    std::vector<FaceGroup> groups_;
    QListWidget* list_;
    QComboBox* style_;
    QLabel* info_;
    QProgressBar* progress_;
    QPushButton* find_;
    QPushButton* apply_;
    QFutureWatcher<bool> watcher_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::atomic<double>> fraction_;
    std::shared_ptr<FaceTracks> pending_;
    std::shared_ptr<std::string> error_;
    bool running_ = false;
};

}  // namespace montage
