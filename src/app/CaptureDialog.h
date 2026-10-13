// Montage — File › Capture: recording a live feed (media/Capture.h) while editing, as Resolve's Media page capture
// and Media Composer's Capture tool do. The feed is a network stream (SRT, RIST, UDP, RTMP... by its URL) or a capture
// device (a camera or capture card and a sound card, as FFmpeg names them), recorded to ProRes or DNxHR with its
// timecode into a folder. A second into the recording the file is imported into the "Captures" bin, where it grows
// as it records (EditorState's growing media), so it can be cut in at once. The dialog stays open beside the editor.
#pragma once

#include <QDialog>
#include <memory>

#include "core/Model.h"
#include "media/Capture.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace montage {

class EditorState;

class CaptureDialog : public QDialog {
    Q_OBJECT
public:
    CaptureDialog(EditorState* state, QWidget* parent = nullptr);
    ~CaptureDialog() override;

    // The feed: a URL ("" kind), or a device of one of captureDeviceKinds() ("v4l2", "avfoundation", "dshow"...).
    void setSource(const QString& kind, const QString& url);
    void setSoundDevice(const QString& name);  // a sound device of the same kind ("" the feed's own sound)
    void setOptions(const QString& options);   // FFmpeg's input options, "name=value" apart by spaces
    void setCodec(const QString& id);
    void setWrapper(const QString& ext);  // "mov" (imported while it records) or "mxf" (once done)
    void setFolder(const QString& dir);
    void setClipName(const QString& name);
    void setTimecode(const QString& tc);  // "" the feed's own, else the time of day
    void setSeconds(double s);            // 0 until stopped

    // Starts recording (false with the reason in the status line); stops it.
    bool record();
    void stop();
    bool isRecording() const;
    QString filePath() const { return path_; }  // the file being or last recorded
    Id importedMedia() const { return imported_; }
    QString statusText() const;

signals:
    void recordingChanged();

private:
    void refresh();
    void finished(int generation, bool ok, const QString& message);
    void importNow();

    EditorState* state_;
    QComboBox* kind_;
    QLineEdit* url_;
    QLineEdit* sound_;
    QLineEdit* options_;
    QComboBox* codec_;
    QComboBox* wrapper_;
    QLineEdit* folder_;
    QLineEdit* name_;
    QLineEdit* timecode_;
    QDoubleSpinBox* seconds_;
    QCheckBox* import_;
    QPushButton* record_;
    QLabel* status_;
    QTimer* tick_;
    std::unique_ptr<Recorder> recorder_;
    QString path_, error_;
    QString importedPath_;  // the imported file's path on disk, while it records
    Id imported_ = 0;
    bool recording_ = false;  // (until the recording's end has been taken in here)
    int generation_ = 0;      // the recording the recorder's notices are about
    int64_t notedFrames_ = -1;  // the frames recorded when its writing was last noted
};

}  // namespace montage
