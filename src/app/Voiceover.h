// Montage — recording voiceover onto the timeline: sound from an input is
// written to a WAV file beside the project as it arrives, then imported and
// placed on the targeted audio track where recording began (one undo step).
// Punch-in stops by itself at the sequence's Out point.
#pragma once

#include <QDialog>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <optional>

#include "core/Model.h"

class QAudioSource;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QIODevice;
class QLabel;
class QProgressBar;
class QPushButton;

namespace montage {

class EditorState;
class PlaybackController;

// 32-bit float WAV, written as it goes; the header is completed on close().
class WavWriter {
public:
    ~WavWriter();
    bool open(const QString& path, int sampleRate, int channels);
    // False once anything could not be written (a full disk); close() then says so too.
    bool write(const float* interleaved, qint64 frames);
    bool close();
    qint64 frames() const { return frames_; }
    bool isOpen() const { return file_.isOpen(); }

private:
    QFile file_;
    int rate_ = 48000, channels_ = 1;
    qint64 frames_ = 0;
    bool failed_ = false;
};

class VoiceoverRecorder : public QObject {
    Q_OBJECT
public:
    explicit VoiceoverRecorder(EditorState* state, QObject* parent = nullptr);
    ~VoiceoverRecorder() override;

    // The audio inputs (descriptions), the system default first.
    static QStringList inputs();
    // Where takes go: a "Voiceover" folder beside the project (or in Music when it is unsaved).
    QString takeFolder() const;
    // Takes named otherwise: into folder `folder` (on disk beside the project, and in the bin), files `stem` 1, 2...
    // ("" = "<sequence> VO "). ADR takes go to "ADR", named by their cue.
    void setTakeNaming(const QString& folder, const QString& stem = {});
    // With `place` false a take is imported but left off the timeline: taken() says where it began, and stop()
    // returns 0.
    void setPlacing(bool place) { placing_ = place; }

    // Starts a take at timeline frame `at` for audio track `track` (index), from
    // input `input` ("" = default). With `stopAt` > at, it ends by itself there
    // (punch-in to Out). `openDevice` false takes sound only through feed().
    bool start(FrameTime at, int track, FrameTime stopAt = -1, const QString& input = {}, bool openDevice = true,
               int sampleRate = 48000, int channels = 1);
    // Loop recording (core/TakeLanes.h): the take starts `preroll` frames before `in` and runs on while the picture
    // loops from `in` to `out` (exclusive). On stop each pass from In to Out (and a last one of at least a second)
    // becomes a WAV of its own and a take lane on the track starting at In, and the last whole pass is given to the
    // comp there (one undo step); stop() returns that comp clip.
    bool startLoop(FrameTime in, FrameTime out, FrameTime preroll, int track, const QString& input = {}, bool openDevice = true,
                   int sampleRate = 48000, int channels = 1);
    bool isLooping() const { return loopOut_ > loopIn_; }
    int passes() const;  // whole passes recorded so far
    // Sound for the take (float, interleaved); the device's sound arrives here too.
    void feed(const float* interleaved, qint64 frames);
    // Ends the take: the file is finished, imported and placed. Returns the new
    // clip (0 if nothing was recorded or it was cancelled).
    Id stop();
    void cancel();

    bool isRecording() const { return writer_.isOpen(); }
    FrameTime startedAt() const { return at_; }
    double seconds() const { return rate_ > 0 ? double(writer_.frames()) / rate_ : 0; }
    QString lastTake() const { return lastTake_; }

signals:
    void level(float peak);              // linear, per block
    void stopped(montage::Id clip);      // after a take is placed (also when Out ended it)
    // After a take is imported (placed or not): its media, and the timeline frame its first sample belongs at.
    void taken(montage::Id media, montage::FrameTime at);

private:
    void readDevice();
    Id stopLoop();

    EditorState* state_;
    WavWriter writer_;
    QAudioSource* source_ = nullptr;
    QPointer<QIODevice> io_;
    int rate_ = 48000, channels_ = 1, sampleBytes_ = 4;
    int format_ = 0;  // 0 float, 1 int16, 2 int32
    FrameTime at_ = 0, stopAt_ = -1;
    int track_ = 0;
    QString path_, lastTake_;
    QString folder_ = QStringLiteral("Voiceover"), stem_;
    bool stopping_ = false;
    bool placing_ = true;
    FrameTime loopIn_ = 0, loopOut_ = 0, preroll_ = 0;
};

// Sequence › Record Voiceover: input, level, countdown and punch-in.
class VoiceoverDialog : public QDialog {
    Q_OBJECT
public:
    VoiceoverDialog(EditorState* state, PlaybackController* program, QWidget* parent = nullptr);
    VoiceoverRecorder* recorder() const { return recorder_; }

private:
    void toggle();
    void begin();
    void finish();

    EditorState* state_;
    PlaybackController* program_;
    VoiceoverRecorder* recorder_;
    QComboBox* input_;
    QComboBox* track_;
    QCheckBox* countdown_;
    QCheckBox* punch_;
    QCheckBox* loop_;
    QDoubleSpinBox* preroll_;
    std::optional<bool> wasLooping_;  // the player's own loop setting, while a loop recording loops it
    QProgressBar* meter_;
    QLabel* status_;
    QPushButton* record_;
    QTimer tick_;
    int count_ = 0;
};

}  // namespace montage
