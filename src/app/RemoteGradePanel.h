// Montage — Playback › Video Output › Remote Grading…: a client's Montage, or a second colourist's, follows this one
// live (render/RemoteGrade.h). Hosting, every change to the project, the Program's playhead and play state, the
// viewer's exposure check and wipe, and the scope shown go to the client, whose notes come back as markers and whose
// pointers show on the Program monitor for a few seconds. Following, this Montage takes the colourist's project in
// place of its own (keeping its own copies of the media), plays as the colourist plays, and shows the viewer as the
// colourist has it; where it lacks a clip's media it shows the colourist's Remote Review stream there instead.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <vector>

#include "core/Model.h"
#include "render/RemoteGrade.h"
#include "render/RemoteReview.h"

class QLabel;
class QPainter;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QTabWidget;
class QTimer;

namespace montage {

class EditorState;
class PlaybackController;

// What the controller asks of the window: its viewer's state, and putting a colourist's view on it.
struct RemoteGradeHooks {
    std::function<QJsonObject()> viewer;                 // {"exposure": n, "wipe": bool, "split": 0..1, "scope": n}
    std::function<QImage()> compare;                     // the wipe's reference picture (null: none)
    std::function<void(const QJsonObject&, const QImage&)> applyViewer;
    std::function<void(Id, const std::vector<output::RemoteMarker>&)> notes;
};

class RemoteGradeController : public QObject {
    Q_OBJECT
public:
    RemoteGradeController(EditorState* state, PlaybackController* program, RemoteGradeHooks hooks, QObject* parent = nullptr);
    ~RemoteGradeController() override;

    enum class Mode { Off, Hosting, Following };
    Mode mode() const { return mode_; }
    bool host(const remote::GradeSettings& settings, QString* error = nullptr);
    // Follows the colourist at `settings.address`; `stream`: their Remote Review stream, shown where media is missing
    // ("" none).
    bool join(const remote::GradeSettings& settings, const QString& stream = QString(), QString* error = nullptr);
    void stop();
    remote::GradeSettings settings() const { return settings_; }
    QString stream() const { return stream_; }
    QString status() const;
    // Following: a note at the playhead, and a pointer at a place on the picture (0..1 across it).
    void sendNote(const QString& text);
    void point(double x, double y);
    // Hosting: the client's pointers of the last few seconds, and how faded each is (1 new, 0 gone).
    struct ShownPointer {
        remote::Pointer pointer;
        double fade = 1;
    };
    std::vector<ShownPointer> pointers() const;
    // Following: whether the frame shown comes from the colourist's stream (its media missing here), and that picture.
    bool showingStream() const { return showingStream_; }
    QImage streamPicture() const { return streamPicture_; }
    // The viewer state the hooks give, as sent (for testing).
    static QJsonObject viewerJson(const QString& text);
    // The Program monitor: following, Alt+click on it points there; drawn over it, the client's pointers (hosting) or
    // the colourist's stream where media is missing (following).
    void watchViewer(QWidget* viewer, std::function<QRectF()> pictureRect);
    void paintOverlay(QPainter& p, const QRectF& picture) const;

    static remote::GradeSettings loadSettings(bool hosting);
    static void saveSettings(const remote::GradeSettings& s, bool hosting, const QString& stream = QString());
    static QString loadStream();

    // The agent link's montage_remote_grade (app/LiveLink.h): status, host, join, note or stop.
    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

signals:
    void modeChanged();
    void pointersChanged();  // (the Program monitor draws them)

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tick();
    void hostTick();
    void followTick();

    EditorState* state_;
    QPointer<PlaybackController> program_;
    RemoteGradeHooks hooks_;
    Mode mode_ = Mode::Off;
    remote::GradeSettings settings_;
    QString stream_;
    remote::GradeHost host_;
    remote::GradeClient client_;
    std::unique_ptr<output::ReviewReceiver> receiver_;
    QTimer* timer_;
    QPointer<QWidget> viewer_;
    std::function<QRectF()> pictureRect_;
    bool projectDirty_ = false;
    // Hosting: the wipe's picture as sent.
    qint64 compareKey_ = -1;
    std::string compareBytes_;
    struct Seen {
        remote::Pointer pointer;
        QElapsedTimer since;
    };
    std::vector<Seen> pointers_;
    // Following.
    Project local_;  // the project as it was here, for its media
    uint64_t revision_ = 0;
    std::string viewerShown_, compareShown_;
    int drift_ = 0;
    QString followError_;
    bool showingStream_ = false;
    QImage streamPicture_;
    int64_t streamSerial_ = 0;
};

class RemoteGradePanel : public QDialog {
    Q_OBJECT
public:
    RemoteGradePanel(RemoteGradeController* controller, QWidget* parent = nullptr);

private:
    void refresh();
    RemoteGradeController* controller_;
    QTabWidget* tabs_;
    QLineEdit* hostAddress_;
    QLineEdit* hostPassphrase_;
    QLineEdit* joinAddress_;
    QLineEdit* joinPassphrase_;
    QLineEdit* name_;
    QLineEdit* stream_;
    QLineEdit* note_;
    QPushButton* send_;
    QLabel* status_;
    QPushButton* start_;
    QTimer* timer_;
};

}  // namespace montage
