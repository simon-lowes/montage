// Montage — export dialog: preset, output file, range and size, then a
// background render with progress, ETA and cancel.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QString>
#include <atomic>

#include "core/Model.h"
#include "render/Exporter.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace montage {

class EditorState;
class RenderQueue;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(EditorState* state, QWidget* parent = nullptr);
    ~ExportDialog() override;

    bool isExporting() const { return exporting_; }
    // Shows Add to Queue, which hands the export to this queue instead.
    void setQueue(RenderQueue* queue);

public slots:
    // Closing (Close button, Esc, window close) mid-export cancels the render
    // and waits for the worker to stop before the dialog goes away.
    void reject() override;

private:
    struct Result {
        bool ok = false;
        QString error;
        QString encoder;  // the video encoder that ran (hardware presets resolve at export time)
    };

    const ExportPreset* currentPreset() const;
    bool prepare(ExportSettings& s, FrameTime& in, FrameTime& out);
    void addToQueue();
    void presetChanged();
    void browse();
    void widthChanged(int w);
    void heightChanged(int h);
    void updateControls();
    void updateSummary();
    bool rangeIsInOut() const;
    bool rangeFrames(FrameTime& in, FrameTime& out) const;  // false if empty
    QString defaultOutputPath(const QString& extension) const;

    void startExport();
    void requestCancel();
    void stopAndWait();
    void showProgress();
    void exportFinished();
    void setExporting(bool on);
    void removePartialFile();

    EditorState* state_ = nullptr;

    QWidget* form_ = nullptr;
    QComboBox* preset_ = nullptr;
    QLabel* presetDescription_ = nullptr;
    QLineEdit* path_ = nullptr;
    QPushButton* browse_ = nullptr;
    QComboBox* range_ = nullptr;
    QCheckBox* matchSize_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* quality_ = nullptr;
    QComboBox* captions_ = nullptr;
    QCheckBox* chapters_ = nullptr;
    QComboBox* streams_ = nullptr;      // more audio streams after the mix: none, per role, per track
    QCheckBox* allCaptions_ = nullptr;  // every caption track as its own subtitle stream
    QCheckBox* smart_ = nullptr;
    QComboBox* color_ = nullptr;
    QComboBox* loudness_ = nullptr;
    QComboBox* audioOut_ = nullptr;  // a surround sequence: all its channels, or folded to stereo
    QComboBox* stems_ = nullptr;     // also write stems: none, per track, per bus
    int stemsMode_ = 0;
    // Burn-ins for review copies.
    QCheckBox* burnTimecode_ = nullptr;
    QCheckBox* burnClipName_ = nullptr;
    QLineEdit* burnText_ = nullptr;
    QComboBox* burnCorner_ = nullptr;
    QLineEdit* watermark_ = nullptr;
    QComboBox* watermarkCorner_ = nullptr;
    QPushButton* queueButton_ = nullptr;
    RenderQueue* queue_ = nullptr;
    QLabel* summary_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* eta_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;

    bool updatingSize_ = false;
    QString confirmedPath_;  // chosen in the file dialog, so overwriting is already confirmed

    // Export job. The worker publishes progress through the atomics and posts
    // at most one queued update at a time.
    QFutureWatcher<Result> watcher_;
    std::atomic<bool> cancel_{false};
    std::atomic<double> progressFraction_{0.0};
    std::atomic<FrameTime> progressFrame_{0};
    std::atomic<bool> progressQueued_{false};
    bool exporting_ = false;
    QElapsedTimer elapsed_;
    QString exportPath_;
    FrameTime exportIn_ = 0;
    FrameTime exportFrames_ = 0;
    bool fileExisted_ = false;
    qint64 fileModified_ = 0;
};

}  // namespace montage
