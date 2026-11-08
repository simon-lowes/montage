// Montage — File › Capture: recording a live feed (media/Capture.h) while editing, as Resolve's Media page capture
// and Media Composer's Capture tool do. The feed is a network stream (SRT, RIST, UDP, RTMP... by its URL) or a capture
// device (a camera or capture card and a sound card, as FFmpeg names them), recorded to ProRes or DNxHR with its
// timecode into a folder. A second into the recording the file is imported into the "Captures" bin, where it grows
// as it records (EditorState's growing media), so it can be cut in at once. The dialog stays open beside the editor.
// SMPTE ST 2110 streams are recorded from their SDP files (with ST 2022-7's two paths, and the time code their
// ancillary data carries), or offered as NMOS receivers for a facility's controller to connect. While it records, the
// feed can also be packaged live (render/LivePackage.h: HLS and DASH with a sliding window, low latency if asked) into
// a folder, and served from it over HTTP.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <memory>
#include <thread>

#include "core/Model.h"
#include "media/Capture.h"
#include "render/LivePackage.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QSpinBox;
class QLineEdit;
class QPushButton;
class QTimer;

namespace montage {

namespace nmos {
class Node;
}
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
    // A live package made while it records: its folder ("" none), and how. Port 0 (with serve): any free one.
    struct LiveOptions {
        QString folder;
        bool hls = true, dash = true, lowLatency = false;
        int window = 6;
        QString segments = QStringLiteral("cmaf");  // or "ts" (HLS in MPEG-TS)
        QString sound = QStringLiteral("aac");      // aac, libopus, ac3, eac3 or none
        bool serve = false, network = false;
        int port = 0;
    };
    void setLive(const LiveOptions& o);
    LiveOptions live() const;
    QString liveUrl() const;  // where the package is served ("" not served)

    // Starts recording (false with the reason in the status line); stops it.
    bool record();
    void stop();
    bool isRecording() const;
    QString filePath() const { return path_; }  // the file being or last recorded
    Id importedMedia() const { return imported_; }
    QString statusText() const;

    // ST 2110 receivers offered to NMOS controllers (IS-04, with IS-05 connections): what one connects them to
    // becomes the feed. False with why when the node cannot start.
    bool offerNmos(bool on, QString* error = nullptr);
    nmos::Node* nmosNode() const { return node_; }
    QString nmosStatus() const;

    // The agent link's tool (montage_capture).
    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

signals:
    void recordingChanged();

private:
    void refresh();
    void finished(int generation, bool ok, const QString& message);
    void importNow();
    void nmosConnected();
    bool startLive(CaptureSettings& cs);  // false with error_ set
    void endLive();

    EditorState* state_;
    QComboBox* kind_;
    QLineEdit* url_;
    QPushButton* find_;
    QLineEdit* sound_;
    QDoubleSpinBox* soundLatency_;
    QLineEdit* options_;
    QCheckBox* nmos_;
    QLabel* nmosStatus_;
    nmos::Node* node_ = nullptr;
    QString nmosFeed_;  // the feed (SDP text) an NMOS controller last set ("" none)
    QComboBox* codec_;
    QComboBox* wrapper_;
    QLineEdit* folder_;
    QLineEdit* name_;
    QLineEdit* timecode_;
    QDoubleSpinBox* seconds_;
    QCheckBox* import_;
    QGroupBox* live_;
    QLineEdit* liveFolder_;
    QComboBox* liveFormats_;
    QComboBox* liveSegments_;
    QComboBox* liveSound_;
    QCheckBox* liveLowLatency_;
    QSpinBox* liveWindow_;
    QCheckBox* liveServe_;
    QSpinBox* livePort_;
    QCheckBox* liveNetwork_;
    QPushButton* record_;
    QLabel* status_;
    QTimer* tick_;
    std::unique_ptr<LivePackager> packager_;  // (before the recorder, which feeds it: gone after it)
    std::unique_ptr<LiveOrigin> origin_;
    std::thread liveFinish_;  // the package being finished once the recording ends (the queued pictures encoded)
    QString liveUrl_;
    std::unique_ptr<Recorder> recorder_;
    QString path_, error_;
    QString importedPath_;  // the imported file's path on disk, while it records
    Id imported_ = 0;
    bool recording_ = false;  // (until the recording's end has been taken in here)
    int generation_ = 0;      // the recording the recorder's notices are about
    int64_t notedFrames_ = -1;  // the frames recorded when its writing was last noted
};

}  // namespace montage
