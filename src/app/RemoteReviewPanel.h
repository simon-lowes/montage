// Montage — Playback › Video Output › Remote Review…: the Program monitor streamed to reviewers elsewhere
// (render/RemoteReview.h), beside any reference output; the controller opens the stream for the Program's sequence,
// follows it, and hands on the markers reviewers send. File › Watch Remote Review…: a reviewer's window onto such a
// stream, with its sound, the timecode of the frame shown, and notes sent back as markers at that frame.
#pragma once

#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QWidget>
#include <memory>
#include <vector>

#include "PlaybackController.h"
#include "render/RemoteReview.h"

class QAudioSink;
class QCheckBox;
class QComboBox;
class QIODevice;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace montage {

// How the picture goes out, beyond the stream's own settings.
struct RemoteReviewOptions {
    int maxHeight = 1080;  // scaled down to it (0: the sequence's size)
    bool rec709 = true;    // as the viewer shows it (HDR tone mapped); else the sequence's own colour
    bool audio = true;
};

class RemoteReviewController : public QObject {
    Q_OBJECT
public:
    explicit RemoteReviewController(PlaybackController* program, QObject* parent = nullptr);
    ~RemoteReviewController() override;

    // Streams the Program's sequence; false and why when it cannot.
    bool start(const output::StreamSettings& settings, const RemoteReviewOptions& options, QString* error = nullptr);
    void stop();
    bool running() const;
    // The Program's sequence changed: opened again when its size, rate, colour or sound rate did.
    void sequenceChanged();
    // "Waiting for a viewer…" or "Streaming to a viewer: HEVC 10-bit 1920 × 1080, 7.9 Mb/s, 2 markers" (or why not).
    QString status() const;
    output::StreamSettings settings() const { return settings_; }
    RemoteReviewOptions options() const { return options_; }
    std::shared_ptr<output::StreamBackend> backend() const { return backend_; }
    std::shared_ptr<output::Scheduler> scheduler() const { return scheduler_; }
    // The addresses viewers use: each of this computer's network addresses when listening on all of them.
    QStringList viewerAddresses() const;
    // The frame size a sequence goes out at.
    static output::Mode modeFor(const Sequence& s, const RemoteReviewOptions& o);

    static output::StreamSettings loadSettings();
    static RemoteReviewOptions loadOptions();
    static void saveSettings(const output::StreamSettings& s, const RemoteReviewOptions& o);

    // The agent link's montage_remote_review (app/LiveLink.h): status, start or stop.
    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

signals:
    void runningChanged(bool running);
    // Markers a reviewer sent, for the sequence streamed.
    void markersReceived(montage::Id sequence, const std::vector<montage::output::RemoteMarker>& markers);

private:
    QString formatKey() const;
    QPointer<PlaybackController> program_;
    output::StreamSettings settings_;
    RemoteReviewOptions options_;
    std::shared_ptr<output::StreamBackend> backend_;
    std::shared_ptr<output::Scheduler> scheduler_;
    Id sequence_ = 0;
    QTimer* watch_ = nullptr;  // markers taken; the stream stopping by itself noticed
    QString startedFor_;
    QString lastError_;
};

class RemoteReviewPanel : public QDialog {
    Q_OBJECT
public:
    RemoteReviewPanel(RemoteReviewController* controller, QWidget* parent = nullptr);
    output::StreamSettings chosenSettings() const;
    RemoteReviewOptions chosenOptions() const;

private:
    void refresh();
    RemoteReviewController* controller_;
    QLineEdit* address_;
    QComboBox* direction_;
    QLineEdit* passphrase_;
    QComboBox* codec_;
    QCheckBox* hardware_;
    QComboBox* size_;
    QSpinBox* bitrate_;
    QComboBox* colour_;
    QCheckBox* burn_;
    QCheckBox* audio_;
    QCheckBox* markers_;
    QSpinBox* markersPort_;
    QSpinBox* latency_;
    QLabel* howTo_;
    QLabel* status_;
    QPushButton* start_;
    QTimer* timer_;
};

// The picture, kept to its shape in the space it is given.
class ReviewPicture : public QWidget {
    Q_OBJECT
public:
    explicit ReviewPicture(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    const QImage& image() const { return image_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage image_;
};

class ReviewViewerWindow : public QWidget {
    Q_OBJECT
public:
    explicit ReviewViewerWindow(QWidget* parent = nullptr);
    ~ReviewViewerWindow() override;
    bool watching() const { return watching_; }
    // The frame shown, as its stream tagged it (frame -1: none yet).
    output::StreamTag shownTag() const { return tag_; }

private:
    void toggle();
    void poll();
    void addMarker();
    void stopSound();
    output::ReviewReceiver receiver_;
    bool watching_ = false;
    int64_t serial_ = 0;
    output::StreamTag tag_;
    QLineEdit* address_;
    QLineEdit* passphrase_;
    QLineEdit* author_;
    QLineEdit* markersTo_;
    QPushButton* connect_;
    ReviewPicture* picture_;
    QLabel* timecode_;
    QLineEdit* note_;
    QPushButton* mark_;
    QLabel* status_;
    QTimer* timer_;
    QAudioSink* sink_ = nullptr;
    QIODevice* sound_ = nullptr;
    QString sent_;  // what the last marker sent came to
    int sending_ = 0;
};

}  // namespace montage
